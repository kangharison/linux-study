// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] 드라이버가 잡은 PCI 자원을 자동으로 되돌려 주는 계층 (devres.c)
 *
 * === 파일의 역할 ===
 * devres(device resource management)는 "드라이버가 죽거나 언바인딩될 때
 * 잡아 둔 것을 커널이 알아서 풀어 주는" 커널 공통 기능이다. 이 파일은
 * 그것의 PCI 판 — pcim_* 접두사가 붙은 함수들을 제공한다.
 *
 * 문제의식은 이렇다. 드라이버의 probe 는 실패 지점이 여럿이고, 각 지점에서
 * 그때까지 잡은 것을 정확한 역순으로 풀어야 한다. goto 사슬로 그것을 쓰다
 * 보면 하나를 빠뜨리기 쉽고, 그 실수는 조용한 자원 누수가 된다.
 * devres 를 쓰면 잡은 것이 장치에 매달리고, remove 시점에 커널이 역순으로
 * 전부 푼다. probe 는 실패해도 그냥 return 하면 된다.
 *
 * 다루는 자원이 넷이다.
 *   - 장치 활성화(pcim_enable_device) — pci_disable_device 를 자동 호출
 *   - 자원 영역 예약(pcim_request_region 계열) — pci_release_region 자동
 *   - BAR 매핑(pcim_iomap 계열) — iounmap 자동
 *   - INTx 설정(pcim_intx) — 원래 상태로 자동 복원
 *
 * 구현 방식이 흥미롭다. devres 는 "해제 함수 + 데이터" 를 한 덩어리로
 * 장치에 매다는데, 이 파일은 자원 종류마다 그 덩어리의 모양과 해제
 * 함수를 정의한다. 예컨대 pcim_addr_devres 는 매핑 종류(BAR 번호,
 * 주소 범위)와 그것을 푸는 방법을 함께 담는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 probe
 *   -> [이 파일] pcim_enable_device(pdev)
 *      -> pci_enable_device() + devres 에 해제 항목 등록
 *   -> [이 파일] pcim_iomap_regions(pdev, mask, name)
 *      -> pci_request_region + ioremap + 해제 항목 등록
 *   (실패하면 그냥 return — 정리는 커널이 한다)
 *
 * 드라이버 remove 또는 probe 실패
 *   -> 드라이버 코어가 devres 목록을 역순으로 훑으며 해제 함수 호출
 *      -> [이 파일] 각 해제 함수가 대응하는 pci_* 를 부른다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 할당과 자원 조작이 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 드라이버들. pcim_ 을 쓰는 드라이버와 pci_ 를 직접 쓰는 드라이버가
 *   섞여 있고, 두 방식을 한 드라이버 안에서 섞으면 안 된다.
 * 아래쪽: drivers/base/devres.c 의 devres 코어, pci.c 의 자원 함수들.
 * 공유 상태: struct pci_dev 에 매달린 devres 목록(struct device 의 devres_head).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 쓰지 않는다. drivers/nvme/ 에
 * "pcim_" 문자열이 0건이다(주석 제거 후 검색).
 *
 * NVMe 는 자원을 직접 잡고 직접 푼다 — pci_enable_device_mem(),
 * pci_request_mem_regions(), ioremap() 을 부르고, nvme_dev_unmap() 과
 * nvme_pci_disable() 에서 대칭적으로 되돌린다.
 *
 * 그 선택에는 이유가 있다. NVMe 는 컨트롤러 리셋 중에 자원을 잠시 놓았다가
 * 다시 잡는 경우가 있는데(nvme_dev_disable -> nvme_reset_work), devres 는
 * "드라이버가 떨어질 때 한 번에 푼다" 는 모델이라 그런 중간 해제를
 * 표현하기 어렵다. 수명이 probe~remove 와 정확히 일치하지 않는 자원에는
 * devres 가 맞지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * pcim_enable_device()      : pci_enable_device + 자동 해제 등록.
 *                             한 번 쓰면 그 장치의 다른 자원도 devres 로
 *                             관리된다는 표시(is_managed)가 선다.
 * pcim_iomap()              : BAR 하나를 매핑하고 해제를 등록한다.
 * pcim_iomap_regions()      : 마스크로 지정한 여러 BAR 을 한 번에
 *                             request + iomap 한다. 가장 자주 쓰이는 함수다.
 * pcim_iounmap()            : 명시적으로 풀 때. 등록된 항목도 함께 지운다.
 * pcim_request_region()     : 자원 영역만 예약(매핑은 하지 않는다).
 * pcim_intx()               : INTx 설정을 바꾸되 원래 값을 기억해 둔다.
 * pcim_set_mwi()            : Memory-Write-Invalidate 를 켜고 자동 해제 등록.
 * struct pcim_addr_devres   : 매핑/예약 하나를 나타내는 devres 항목.
 *                             종류(type), BAR 번호, 주소 범위를 담는다.
 * struct pcim_intx_devres   : INTx 의 원래 상태를 담는 항목.
 */

#include <linux/device.h>
#include <linux/pci.h>
#include "pci.h"

/*
 * On the state of PCI's devres implementation:
 *
 * The older PCI devres API has one significant problem:
 *
 * It is very strongly tied to the statically allocated mapping table in struct
 * pcim_iomap_devres below. This is mostly solved in the sense of the pcim_
 * functions in this file providing things like ranged mapping by bypassing
 * this table, whereas the functions that were present in the old API still
 * enter the mapping addresses into the table for users of the old API.
 *
 * TODO:
 * Remove the legacy table entirely once all calls to pcim_iomap_table() in
 * the kernel have been removed.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/devres.c)은 PCI 장치 드라이버가 사용하는 관리형
 * 리소스(devres) helper 함수들을 제공한다. 드라이버 detach(제거) 시
 * 자동으로 정리되어야 할 PCI 리소스(BAR region, BAR iomapping, INTx,
 * MWI, device enable 상태 등)를 device lifetime에 연결한다.
 *
 * NVMe PCIe SSD 입장에서 본 파일의 기능은 다음과 같다.
 *   - pcim_enable_device(): NVMe 컨트롤러의 PCI device 활성화를 관리형으로
 *     등록. 드라이버 제거 시 pci_disable_device()를 자동 수행.
 *   - pcim_iomap() / pcim_iomap_region() / pcim_iomap_range(): NVMe BAR0
 *     (register/doorbell 영역)이나 CMB가 위치한 BAR를 커널 가상 주소로
 *     매핑. 드라이버 제거 시 iounmap/release 를 자동 수행.
 *   - pcim_intx(): NVMe 장치가 MSI-X 대신 INTx를 사용하는 레거시 환경에서
 *     INTx enable/disable 을 관리하고, 드라이버 제거 시 원래 상태로 복원.
 *   - pcim_set_mwi(): Memory Write Invalidate 활성화를 관리형으로 등록.
 *   - pcim_pin_device(): NVMe 장치를 suspend/resume 중에도 비활성화되지
 *     않도록 고정할 때 사용.
 *   - devm_pci_remap_cfgspace()/devm_pci_remap_cfg_resource(): PCI
 *     configuration space 접근용 매핑을 관리형으로 수행. NVMe 드라이버는
 *     직접 호출하지 않지만 PCI core가 NVMe 장치의 config space를 다룰 때
 *     사용될 수 있다.
 *
 * 일반적인 NVMe 드라이버 호출 경로(예시):
 *   nvme_probe -> pci_enable_device_mem(pdev) (또는 pcim_enable_device)
 *              -> pci_set_master(pdev)
 *              -> pci_request_regions(pdev, DRV_NAME)
 *              -> pci_iomap(pdev, 0, ...) 또는 pcim_iomap_region(pdev, 0, ...)
 *              -> ioremap(pci_resource_start(pdev,0), size) (NVMe pci.c)
 *              -> readl/writel(dev->bar + NVME_REG_...)
 *              -> pci_alloc_irq_vectors() (MSI-X)
 *              -> dma_pool_create() (SQ/CQ/PRP/SGL descriptor)
 *              -> pci_save_state()/pci_restore_state() (suspend/resume)
 *              -> pci_disable_device() (remove/shutdown)
 *
 * 본 파일은 drivers/nvme/host/pci.c에서 직접 pcim_ 계열 함수를 모두
 * 사용하지는 않지만, NVMe 드라이버가 사용하는 PCI BAR, INTx, device enable
 * 등의 관리형 생명주기를 담당하는 핵심 코드이며, managed PCI API를 통해
 * 리소스 누수를 방지하는 역할을 한다.
 * ===================================================================
 */

/*
 * Legacy struct storing addresses to whole mapped BARs.
 */
struct pcim_iomap_devres {
	void __iomem *table[PCI_NUM_RESOURCES];
};

/* Used to restore the old INTx state on driver detach. */
struct pcim_intx_devres {
	int orig_intx;
};

enum pcim_addr_devres_type {
	/* Default initializer. */
	PCIM_ADDR_DEVRES_TYPE_INVALID,

	/* A requested region spanning an entire BAR. */
	PCIM_ADDR_DEVRES_TYPE_REGION,

	/*
	 * A requested region spanning an entire BAR, and a mapping for
	 * the entire BAR.
	 */
	PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING,

	/*
	 * A mapping within a BAR, either spanning the whole BAR or just a
	 * range.  Without a requested region.
	 */
	PCIM_ADDR_DEVRES_TYPE_MAPPING,
};

/*
 * This struct envelops IO or MEM addresses, i.e., mappings and region
 * requests, because those are very frequently requested and released
 * together.
 */
struct pcim_addr_devres {
	enum pcim_addr_devres_type type;
	void __iomem *baseaddr;
	unsigned long offset;
	unsigned long len;
	int bar;
};

/*
 * pcim_addr_devres_clear:
 *   pcim_addr_devres 구조체를 안전한 초기 상태로 만든다.
 *   NVMe BAR 매핑/region 등록 전 devres 템플릿을 초기화할 때 사용된다.
 */
static inline void pcim_addr_devres_clear(struct pcim_addr_devres *res)
{
	memset(res, 0, sizeof(*res));
	res->bar = -1;
}

/*
 * pcim_addr_resource_release:
 *   드라이버 detach 시 devres에 의해 자동으로 호출되어,
 *   NVMe 장치가 사용하던 BAR region / iomapping 을 정리한다.
 */
static void pcim_addr_resource_release(struct device *dev, void *resource_raw)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcim_addr_devres *res = resource_raw;

	switch (res->type) {
	case PCIM_ADDR_DEVRES_TYPE_REGION:
		pci_release_region(pdev, res->bar);
		break;
	case PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING:
		pci_iounmap(pdev, res->baseaddr);
		pci_release_region(pdev, res->bar);
		break;
	case PCIM_ADDR_DEVRES_TYPE_MAPPING:
		pci_iounmap(pdev, res->baseaddr);
		break;
	default:
		break;
	}
}

/*
 * pcim_addr_devres_alloc:
 *   NVMe 장치가 속한 NUMA 노드를 고려해 BAR/region 관리용 devres를 할당한다.
 */
static struct pcim_addr_devres *pcim_addr_devres_alloc(struct pci_dev *pdev)
{
	struct pcim_addr_devres *res;

	res = devres_alloc_node(pcim_addr_resource_release, sizeof(*res),
				GFP_KERNEL, dev_to_node(&pdev->dev));
	if (res)
		pcim_addr_devres_clear(res);
	return res;
}

/* Just for consistency and readability. */
static inline void pcim_addr_devres_free(struct pcim_addr_devres *res)
{
	devres_free(res);
}

/*
 * Used by devres to identify a pcim_addr_devres.
 */
static int pcim_addr_resources_match(struct device *dev,
				     void *a_raw, void *b_raw)
{
	struct pcim_addr_devres *a, *b;

	a = a_raw;
	b = b_raw;

	if (a->type != b->type)
		return 0;

	switch (a->type) {
	case PCIM_ADDR_DEVRES_TYPE_REGION:
	case PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING:
		return a->bar == b->bar;
	case PCIM_ADDR_DEVRES_TYPE_MAPPING:
		return a->baseaddr == b->baseaddr;
	default:
		return 0;
	}
}

/*
 * devm_pci_unmap_iospace:
 *   관리형 I/O space 매핑 해제 콜백. NVMe는 주로 MMIO를 사용하지만,
 *   일부 레거시 환경에서 I/O space 기반 PCI 접근 시 사용될 수 있다.
 */
static void devm_pci_unmap_iospace(struct device *dev, void *ptr)
{
	struct resource **res = ptr;

	pci_unmap_iospace(*res);
}

/**
 * devm_pci_remap_iospace - Managed pci_remap_iospace()
 * @dev: Generic device to remap IO address for
 * @res: Resource describing the I/O space
 * @phys_addr: physical address of range to be mapped
 *
 * Managed pci_remap_iospace().  Map is automatically unmapped on driver
 * detach.
 */
int devm_pci_remap_iospace(struct device *dev, const struct resource *res,
			   phys_addr_t phys_addr)
{
	const struct resource **ptr;
	int error;

	ptr = devres_alloc(devm_pci_unmap_iospace, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	error = pci_remap_iospace(res, phys_addr);
	if (error) {
		devres_free(ptr);
	} else	{
		*ptr = res;
		devres_add(dev, ptr);
	}

	return error;
}
EXPORT_SYMBOL(devm_pci_remap_iospace);

/**
 * devm_pci_remap_cfgspace - Managed pci_remap_cfgspace()
 * @dev: Generic device to remap IO address for
 * @offset: Resource address to map
 * @size: Size of map
 *
 * Managed pci_remap_cfgspace().  Map is automatically unmapped on driver
 * detach.
 */
void __iomem *devm_pci_remap_cfgspace(struct device *dev,
				      resource_size_t offset,
				      resource_size_t size)
{
	void __iomem **ptr, *addr;

	ptr = devres_alloc(devm_ioremap_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return NULL;

	addr = pci_remap_cfgspace(offset, size);
	if (addr) {
		*ptr = addr;
		devres_add(dev, ptr);
	} else
		devres_free(ptr);

	return addr;
}
EXPORT_SYMBOL(devm_pci_remap_cfgspace);

/**
 * devm_pci_remap_cfg_resource - check, request region and ioremap cfg resource
 * @dev: generic device to handle the resource for
 * @res: configuration space resource to be handled
 *
 * Checks that a resource is a valid memory region, requests the memory
 * region and ioremaps with pci_remap_cfgspace() API that ensures the
 * proper PCI configuration space memory attributes are guaranteed.
 *
 * All operations are managed and will be undone on driver detach.
 *
 * Returns a pointer to the remapped memory or an IOMEM_ERR_PTR() encoded error
 * code on failure. Usage example::
 *
 *	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
 *	base = devm_pci_remap_cfg_resource(&pdev->dev, res);
 *	if (IS_ERR(base))
 *		return PTR_ERR(base);
 */
void __iomem *devm_pci_remap_cfg_resource(struct device *dev,
					  struct resource *res)
{
	resource_size_t size;
	const char *name;
	void __iomem *dest_ptr;

	BUG_ON(!dev);

	if (!res || resource_type(res) != IORESOURCE_MEM) {
		dev_err(dev, "invalid resource\n");
		return IOMEM_ERR_PTR(-EINVAL);
	}

	size = resource_size(res);

	if (res->name)
		name = devm_kasprintf(dev, GFP_KERNEL, "%s %s", dev_name(dev),
				      res->name);
	else
		name = devm_kstrdup(dev, dev_name(dev), GFP_KERNEL);
	if (!name)
		return IOMEM_ERR_PTR(-ENOMEM);

	if (!devm_request_mem_region(dev, res->start, size, name)) {
		dev_err(dev, "can't request region for resource %pR\n", res);
		return IOMEM_ERR_PTR(-EBUSY);
	}

	dest_ptr = devm_pci_remap_cfgspace(dev, res->start, size);
	if (!dest_ptr) {
		dev_err(dev, "ioremap failed for resource %pR\n", res);
		devm_release_mem_region(dev, res->start, size);
		dest_ptr = IOMEM_ERR_PTR(-ENOMEM);
	}

	return dest_ptr;
}
EXPORT_SYMBOL(devm_pci_remap_cfg_resource);

/*
 * __pcim_clear_mwi:
 *   pcim_set_mwi() 등록 시 드라이버 detach에 호출되어
 *   NVMe 장치의 Memory Write Invalidate(MWI)를 해제한다.
 */
static void __pcim_clear_mwi(void *pdev_raw)
{
	struct pci_dev *pdev = pdev_raw;

	pci_clear_mwi(pdev);
}

/**
 * pcim_set_mwi - a device-managed pci_set_mwi()
 * @pdev: the PCI device for which MWI is enabled
 *
 * Managed pci_set_mwi().
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
int pcim_set_mwi(struct pci_dev *pdev)
{
	int ret;

	ret = devm_add_action(&pdev->dev, __pcim_clear_mwi, pdev);
	if (ret != 0)
		return ret;

	ret = pci_set_mwi(pdev);
	if (ret != 0)
		devm_remove_action(&pdev->dev, __pcim_clear_mwi, pdev);

	return ret;
}
EXPORT_SYMBOL(pcim_set_mwi);

static inline bool mask_contains_bar(int mask, int bar)
{
	return mask & BIT(bar);
}

/*
 * pcim_intx_restore:
 *   드라이버 detach 시 원래 INTx 상태로 복원한다.
 *   NVMe가 MSI-X 대신 INTx를 쓰는 경우에 해당한다.
 */
static void pcim_intx_restore(struct device *dev, void *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcim_intx_devres *res = data;

	pci_intx(pdev, res->orig_intx);
}

/*
 * save_orig_intx:
 *   pcim_intx() 최초 호출 시 PCI command 레지스터를 읽어
 *   NVMe 장치의 원래 INTx 상태를 보관한다.
 */
static void save_orig_intx(struct pci_dev *pdev, struct pcim_intx_devres *res)
{
	u16 pci_command;

	pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
	res->orig_intx = !(pci_command & PCI_COMMAND_INTX_DISABLE);
}

/**
 * pcim_intx - managed pci_intx()
 * @pdev: the PCI device to operate on
 * @enable: boolean: whether to enable or disable PCI INTx
 *
 * Returns: 0 on success, -ENOMEM on error.
 *
 * Enable/disable PCI INTx for device @pdev.
 * Restore the original state on driver detach.
 */
int pcim_intx(struct pci_dev *pdev, int enable)
{
	struct pcim_intx_devres *res;
	struct device *dev = &pdev->dev;

	/*
	 * pcim_intx() must only restore the INTx value that existed before the
	 * driver was loaded, i.e., before it called pcim_intx() for the
	 * first time.
	 */
	res = devres_find(dev, pcim_intx_restore, NULL, NULL);
	if (!res) {
		res = devres_alloc(pcim_intx_restore, sizeof(*res), GFP_KERNEL);
		if (!res)
			return -ENOMEM;

		save_orig_intx(pdev, res);
		devres_add(dev, res);
	}

	pci_intx(pdev, enable);

	return 0;
}
EXPORT_SYMBOL_GPL(pcim_intx);

/*
 * pcim_disable_device:
 *   pcim_enable_device()가 등록한 해제 콜백. 드라이버 detach 시
 *   NVMe 장치가 고정되지 않았다면 pci_disable_device()를 호출한다.
 */
static void pcim_disable_device(void *pdev_raw)
{
	struct pci_dev *pdev = pdev_raw;

	if (!pdev->pinned)
		pci_disable_device(pdev);

	pdev->is_managed = false;
}

/**
 * pcim_enable_device - Managed pci_enable_device()
 * @pdev: PCI device to be initialized
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Managed pci_enable_device(). Device will automatically be disabled on
 * driver detach.
 */
int pcim_enable_device(struct pci_dev *pdev)
{
	int ret;

	ret = devm_add_action(&pdev->dev, pcim_disable_device, pdev);
	if (ret != 0)
		return ret;

	/*
	 * We prefer removing the action in case of an error over
	 * devm_add_action_or_reset() because the latter could theoretically be
	 * disturbed by users having pinned the device too soon.
	 */
	ret = pci_enable_device(pdev);
	if (ret != 0) {
		devm_remove_action(&pdev->dev, pcim_disable_device, pdev);
		return ret;
	}

	pdev->is_managed = true;

	return ret;
}
EXPORT_SYMBOL(pcim_enable_device);

/**
 * pcim_pin_device - Pin managed PCI device
 * @pdev: PCI device to pin
 *
 * Pin managed PCI device @pdev. Pinned device won't be disabled on driver
 * detach. @pdev must have been enabled with pcim_enable_device().
 */
void pcim_pin_device(struct pci_dev *pdev)
{
	pdev->pinned = true;
}
EXPORT_SYMBOL(pcim_pin_device);

/*
 * pcim_iomap_release:
 *   레거시 iomap 테이블용 no-op 해제 콜백.
 *   실제 매핑 정리는 매핑 등록 시 사용된 콜백에서 수행된다.
 */
static void pcim_iomap_release(struct device *gendev, void *res)
{
	/*
	 * Do nothing. This is legacy code.
	 *
	 * Cleanup of the mappings is now done directly through the callbacks
	 * registered when creating them.
	 */
}

/**
 * pcim_iomap_table - access iomap allocation table (DEPRECATED)
 * @pdev: PCI device to access iomap table for
 *
 * Returns:
 * Const pointer to array of __iomem pointers on success, NULL on failure.
 *
 * Access iomap allocation table for @dev.  If iomap table doesn't
 * exist and @pdev is managed, it will be allocated.  All iomaps
 * recorded in the iomap table are automatically unmapped on driver
 * detach.
 *
 * This function might sleep when the table is first allocated but can
 * be safely called without context and guaranteed to succeed once
 * allocated.
 *
 * This function is DEPRECATED. Do not use it in new code. Instead, obtain a
 * mapping's address directly from one of the pcim_* mapping functions. For
 * example:
 * void __iomem \*mappy = pcim_iomap(pdev, bar, length);
 */
void __iomem * const *pcim_iomap_table(struct pci_dev *pdev)
{
	struct pcim_iomap_devres *dr, *new_dr;

	dr = devres_find(&pdev->dev, pcim_iomap_release, NULL, NULL);
	if (dr)
		return dr->table;

	new_dr = devres_alloc_node(pcim_iomap_release, sizeof(*new_dr), GFP_KERNEL,
				   dev_to_node(&pdev->dev));
	if (!new_dr)
		return NULL;
	dr = devres_get(&pdev->dev, new_dr, NULL, NULL);
	return dr->table;
}
EXPORT_SYMBOL(pcim_iomap_table);

/*
 * Fill the legacy mapping-table, so that drivers using the old API can
 * still get a BAR's mapping address through pcim_iomap_table().
 */
static int pcim_add_mapping_to_legacy_table(struct pci_dev *pdev,
					    void __iomem *mapping, int bar)
{
	void __iomem **legacy_iomap_table;

	if (!pci_bar_index_is_valid(bar))
		return -EINVAL;

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev);
	if (!legacy_iomap_table)
		return -ENOMEM;

	legacy_iomap_table[bar] = mapping;

	return 0;
}

/*
 * Remove a mapping. The table only contains whole-BAR mappings, so this will
 * never interfere with ranged mappings.
 */
static void pcim_remove_mapping_from_legacy_table(struct pci_dev *pdev,
						  void __iomem *addr)
{
	int bar;
	void __iomem **legacy_iomap_table;

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev);
	if (!legacy_iomap_table)
		return;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (legacy_iomap_table[bar] == addr) {
			legacy_iomap_table[bar] = NULL;
			return;
		}
	}
}

/*
 * The same as pcim_remove_mapping_from_legacy_table(), but identifies the
 * mapping by its BAR index.
 */
static void pcim_remove_bar_from_legacy_table(struct pci_dev *pdev, int bar)
{
	void __iomem **legacy_iomap_table;

	if (!pci_bar_index_is_valid(bar))
		return;

	legacy_iomap_table = (void __iomem **)pcim_iomap_table(pdev);
	if (!legacy_iomap_table)
		return;

	legacy_iomap_table[bar] = NULL;
}

/**
 * pcim_iomap - Managed pcim_iomap()
 * @pdev: PCI device to iomap for
 * @bar: BAR to iomap
 * @maxlen: Maximum length of iomap
 *
 * Returns: __iomem pointer on success, NULL on failure.
 *
 * Managed pci_iomap(). Map is automatically unmapped on driver detach. If
 * desired, unmap manually only with pcim_iounmap().
 *
 * This SHOULD only be used once per BAR.
 *
 * NOTE:
 * Contrary to the other pcim_* functions, this function does not return an
 * IOMEM_ERR_PTR() on failure, but a simple NULL. This is done for backwards
 * compatibility.
 */
void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	void __iomem *mapping;
	struct pcim_addr_devres *res;

	if (!pci_bar_index_is_valid(bar))
		return NULL;

	res = pcim_addr_devres_alloc(pdev);
	if (!res)
		return NULL;
	res->type = PCIM_ADDR_DEVRES_TYPE_MAPPING;

	mapping = pci_iomap(pdev, bar, maxlen);
	if (!mapping)
		goto err_iomap;
	res->baseaddr = mapping;

	if (pcim_add_mapping_to_legacy_table(pdev, mapping, bar) != 0)
		goto err_table;

	devres_add(&pdev->dev, res);
	return mapping;

err_table:
	pci_iounmap(pdev, mapping);
err_iomap:
	pcim_addr_devres_free(res);
	return NULL;
}
EXPORT_SYMBOL(pcim_iomap);

/**
 * pcim_iounmap - Managed pci_iounmap()
 * @pdev: PCI device to iounmap for
 * @addr: Address to unmap
 *
 * Managed pci_iounmap(). @addr must have been mapped using a pcim_* mapping
 * function.
 */
void pcim_iounmap(struct pci_dev *pdev, void __iomem *addr)
{
	struct pcim_addr_devres res_searched;

	pcim_addr_devres_clear(&res_searched);
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_MAPPING;
	res_searched.baseaddr = addr;

	if (devres_release(&pdev->dev, pcim_addr_resource_release,
			pcim_addr_resources_match, &res_searched) != 0) {
		/* Doesn't exist. User passed nonsense. */
		return;
	}

	pcim_remove_mapping_from_legacy_table(pdev, addr);
}
EXPORT_SYMBOL(pcim_iounmap);

/**
 * pcim_iomap_region - Request and iomap a PCI BAR
 * @pdev: PCI device to map IO resources for
 * @bar: Index of a BAR to map
 * @name: Name of the driver requesting the resource
 *
 * Returns: __iomem pointer on success, an IOMEM_ERR_PTR on failure.
 *
 * Mapping and region will get automatically released on driver detach. If
 * desired, release manually only with pcim_iounmap_region().
 */
void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar,
			       const char *name)
{
	int ret;
	struct pcim_addr_devres *res;

	if (!pci_bar_index_is_valid(bar))
		return IOMEM_ERR_PTR(-EINVAL);

	res = pcim_addr_devres_alloc(pdev);
	if (!res)
		return IOMEM_ERR_PTR(-ENOMEM);

	res->type = PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING;
	res->bar = bar;

	ret = pci_request_region(pdev, bar, name);
	if (ret != 0)
		goto err_region;

	res->baseaddr = pci_iomap(pdev, bar, 0);
	if (!res->baseaddr) {
		ret = -EINVAL;
		goto err_iomap;
	}

	devres_add(&pdev->dev, res);
	return res->baseaddr;

err_iomap:
	pci_release_region(pdev, bar);
err_region:
	pcim_addr_devres_free(res);

	return IOMEM_ERR_PTR(ret);
}
EXPORT_SYMBOL(pcim_iomap_region);

/**
 * pcim_iounmap_region - Unmap and release a PCI BAR
 * @pdev: PCI device to operate on
 * @bar: Index of BAR to unmap and release
 *
 * Unmap a BAR and release its region manually. Only pass BARs that were
 * previously mapped by pcim_iomap_region().
 */
void pcim_iounmap_region(struct pci_dev *pdev, int bar)
{
	struct pcim_addr_devres res_searched;

	pcim_addr_devres_clear(&res_searched);
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_REGION_MAPPING;
	res_searched.bar = bar;

	devres_release(&pdev->dev, pcim_addr_resource_release,
			pcim_addr_resources_match, &res_searched);
}
EXPORT_SYMBOL(pcim_iounmap_region);

/**
 * pcim_iomap_regions - Request and iomap PCI BARs (DEPRECATED)
 * @pdev: PCI device to map IO resources for
 * @mask: Mask of BARs to request and iomap
 * @name: Name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Request and iomap regions specified by @mask.
 *
 * This function is DEPRECATED. Do not use it in new code.
 * Use pcim_iomap_region() instead.
 */
int pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name)
{
	int ret;
	int bar;
	void __iomem *mapping;

	for (bar = 0; bar < DEVICE_COUNT_RESOURCE; bar++) {
		if (!mask_contains_bar(mask, bar))
			continue;

		mapping = pcim_iomap_region(pdev, bar, name);
		if (IS_ERR(mapping)) {
			ret = PTR_ERR(mapping);
			goto err;
		}
		ret = pcim_add_mapping_to_legacy_table(pdev, mapping, bar);
		if (ret != 0)
			goto err;
	}

	return 0;

err:
	while (--bar >= 0) {
		pcim_iounmap_region(pdev, bar);
		pcim_remove_bar_from_legacy_table(pdev, bar);
	}

	return ret;
}
EXPORT_SYMBOL(pcim_iomap_regions);

/**
 * pcim_request_region - Request a PCI BAR
 * @pdev: PCI device to request region for
 * @bar: Index of BAR to request
 * @name: Name of the driver requesting the resource
 *
 * Returns: 0 on success, a negative error code on failure.
 *
 * Request region specified by @bar.
 *
 * The region will automatically be released on driver detach. If desired,
 * release manually only with pcim_release_region().
 */
int pcim_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	int ret;
	struct pcim_addr_devres *res;

	if (!pci_bar_index_is_valid(bar))
		return -EINVAL;

	res = pcim_addr_devres_alloc(pdev);
	if (!res)
		return -ENOMEM;
	res->type = PCIM_ADDR_DEVRES_TYPE_REGION;
	res->bar = bar;

	ret = pci_request_region(pdev, bar, name);
	if (ret != 0) {
		pcim_addr_devres_free(res);
		return ret;
	}

	devres_add(&pdev->dev, res);
	return 0;
}
EXPORT_SYMBOL(pcim_request_region);

/**
 * pcim_release_region - Release a PCI BAR
 * @pdev: PCI device to operate on
 * @bar: Index of BAR to release
 *
 * Release a region manually that was previously requested by
 * pcim_request_region().
 */
static void pcim_release_region(struct pci_dev *pdev, int bar)
{
	struct pcim_addr_devres res_searched;

	pcim_addr_devres_clear(&res_searched);
	res_searched.type = PCIM_ADDR_DEVRES_TYPE_REGION;
	res_searched.bar = bar;

	devres_release(&pdev->dev, pcim_addr_resource_release,
			pcim_addr_resources_match, &res_searched);
}


/**
 * pcim_release_all_regions - Release all regions of a PCI-device
 * @pdev: the PCI device
 *
 * Release all regions previously requested through pcim_request_region()
 * or pcim_request_all_regions().
 *
 * Can be called from any context, i.e., not necessarily as a counterpart to
 * pcim_request_all_regions().
 */
static void pcim_release_all_regions(struct pci_dev *pdev)
{
	int bar;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++)
		pcim_release_region(pdev, bar);
}

/**
 * pcim_request_all_regions - Request all regions
 * @pdev: PCI device to map IO resources for
 * @name: name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Requested regions will automatically be released at driver detach. If
 * desired, release individual regions with pcim_release_region() or all of
 * them at once with pcim_release_all_regions().
 */
int pcim_request_all_regions(struct pci_dev *pdev, const char *name)
{
	int ret;
	int bar;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		ret = pcim_request_region(pdev, bar, name);
		if (ret != 0)
			goto err;
	}

	return 0;

err:
	pcim_release_all_regions(pdev);

	return ret;
}
EXPORT_SYMBOL(pcim_request_all_regions);

/**
 * pcim_iomap_range - Create a ranged __iomap mapping within a PCI BAR
 * @pdev: PCI device to map IO resources for
 * @bar: Index of the BAR
 * @offset: Offset from the begin of the BAR
 * @len: Length in bytes for the mapping
 *
 * Returns: __iomem pointer on success, an IOMEM_ERR_PTR on failure.
 *
 * Creates a new IO-Mapping within the specified @bar, ranging from @offset to
 * @offset + @len.
 *
 * The mapping will automatically get unmapped on driver detach. If desired,
 * release manually only with pcim_iounmap().
 */
void __iomem *pcim_iomap_range(struct pci_dev *pdev, int bar,
		unsigned long offset, unsigned long len)
{
	void __iomem *mapping;
	struct pcim_addr_devres *res;

	if (!pci_bar_index_is_valid(bar))
		return IOMEM_ERR_PTR(-EINVAL);

	res = pcim_addr_devres_alloc(pdev);
	if (!res)
		return IOMEM_ERR_PTR(-ENOMEM);

	mapping = pci_iomap_range(pdev, bar, offset, len);
	if (!mapping) {
		pcim_addr_devres_free(res);
		return IOMEM_ERR_PTR(-EINVAL);
	}

	res->type = PCIM_ADDR_DEVRES_TYPE_MAPPING;
	res->baseaddr = mapping;

	/*
	 * Ranged mappings don't get added to the legacy-table, since the table
	 * only ever keeps track of whole BARs.
	 */

	devres_add(&pdev->dev, res);
	return mapping;
}
EXPORT_SYMBOL(pcim_iomap_range);
