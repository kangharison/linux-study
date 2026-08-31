// SPDX-License-Identifier: GPL-2.0
/*
 * From setup-res.c, by:
 *	Dave Rusling (david.rusling@reo.mts.dec.com)
 *	David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *	Ivan Kokshaysky (ink@jurassic.park.msu.ru)
 */

/*
 * [한국어 설명] 버스 객체의 자원 목록과 장치 등록·순회 (bus.c)
 *
 * === 파일의 역할 ===
 * struct pci_bus 하나가 "어떤 주소 범위를 자기 것으로 갖는가" 를 관리하고,
 * 그 범위 안에서 하위 장치에게 자리를 떼어 주며, 버스 트리를 훑는 도구를
 * 제공한다. 세 덩어리로 나뉜다.
 *
 *   1) 자원 목록 관리 - pci_add_resource(), pci_bus_add_resource(),
 *      pci_bus_remove_resources(). 호스트 브리지가 "이 도메인은 메모리
 *      0x80000000~0xBFFFFFFF 와 I/O 0x0~0xFFFF 를 쓴다" 고 알려 준 것을
 *      루트 버스의 자원 목록에 등록한다. 하위 브리지는 자기 윈도우 레지스터
 *      값이 자원이 된다.
 *   2) 자원 할당 - pci_bus_alloc_resource(). 장치의 BAR 하나가 요구하는
 *      크기와 정렬을 만족하는 빈 구간을 이 버스(또는 조상 버스)의 자원에서
 *      찾아 준다. setup-bus.c 가 BAR 배치를 정할 때 실제로 자리를 얻는 곳이다.
 *   3) 장치 등록과 순회 - pci_bus_add_device()/pci_bus_add_devices() 는
 *      새로 발견한 장치를 sysfs 에 올리고 드라이버 바인딩을 시작시킨다.
 *      pci_walk_bus() 계열은 버스 트리 전체에 콜백을 적용한다(에러 복구나
 *      전원 상태 변경처럼 "이 아래 전부" 를 대상으로 하는 동작에 쓴다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거:  probe.c 의 pci_scan_child_bus 가 장치를 찾아 struct pci_dev 를 만든다
 *   -> setup-bus.c 가 BAR 크기를 모아 배치를 계산
 *      -> [이 파일] pci_bus_alloc_resource() 로 실제 주소 구간을 얻는다
 *   -> [이 파일] pci_bus_add_devices()
 *      -> pci_bus_add_device() -> device_attach()
 *         -> pci-driver.c 의 pci_device_probe() -> nvme_probe()
 *
 * 순회: AER 복구, D3 전환, 리셋 등
 *   -> [이 파일] pci_walk_bus(bus, cb, userdata) -> 서브트리의 모든 장치에 cb
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_walk_bus() 는 pci_bus_sem 을 읽기
 * 잠금으로 잡고 돌므로 콜백 안에서 장치를 제거하면 안 된다. 잠금 없이 도는
 * __pci_walk_bus() 는 호출자가 이미 잠금을 쥔 경우에만 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거 후 등록), setup-bus.c(자원 배치), hotplug 드라이버.
 * 아래쪽: 커널 공통 자원 관리(kernel/resource.c 의 allocate_resource),
 *   드라이버 모델(device_attach), pci.c 의 전원 관리 함수.
 * 공유 상태: struct pci_bus 의 resources 목록(struct pci_bus_resource 로
 *   감싼 struct resource 포인터들), pci_bus_sem(버스 트리 전역 rw 세마포어),
 *   그리고 struct pci_dev 의 match_driver / is_added 플래그.
 * 데이터 흐름: 펌웨어/브리지 윈도우 -> 버스 자원 목록 -> BAR 별 구간 할당
 *   -> struct resource 로 pci_dev->resource[] 에 기록 -> 드라이버가
 *   pci_resource_start() 로 읽는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 직접 부르지 않는다(전수 확인).
 * 그럼에도 NVMe 가 동작하기 위한 두 가지 전제를 이 파일이 만든다.
 *
 *   1) BAR0 의 주소. NVMe 컨트롤러 레지스터(CAP, VS, CC, CSTS, 그리고
 *      도어벨 배열)는 전부 BAR0 가 가리키는 메모리 창 안에 있다. 그 창이
 *      물리 주소 공간의 어디에 놓일지를 정하는 마지막 단계가
 *      pci_bus_alloc_resource() 다. 결과는 pdev->resource[0] 에 담기고,
 *      NVMe 드라이버가 pci_resource_start(pdev, 0) 로 읽어
 *      ioremap() 하는 값이 바로 이것이다.
 *
 *   2) nvme_probe() 가 불리는 계기. pci_bus_add_device() 가 장치를 드라이버
 *      모델에 올리면서 device_attach() 를 부르고, 그 끝에 pci_device_probe()
 *      를 거쳐 nvme_probe() 가 실행된다. NVMe SSD 를 핫플러그로 꽂았을 때도
 *      pciehp 가 재스캔한 뒤 이 경로를 탄다.
 *
 * (기존 주석은 NVMe 경로로 "pci_enable_device -> pci_request_regions ->
 *  pci_iomap" 을 적어 두었으나 drivers/nvme/ 에 pci_request_regions 와
 *  pci_iomap 호출은 0건이다. 실제로는 pci_enable_device_mem() 뒤에
 *  ioremap(pci_resource_start(pdev, 0), size) 로 직접 매핑한다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_add_resource() / pci_add_resource_offset()
 *                            : 호스트 브리지 등록용 자원 목록에 항목을 추가.
 *                              offset 판은 CPU 주소와 PCI 버스 주소가 다른
 *                              플랫폼에서 그 차이를 함께 기록한다.
 * pci_bus_add_resource()     : 이미 만들어진 버스에 자원을 붙인다.
 * pci_bus_for_each_resource(): 이 버스와 조상 버스의 자원을 순회하는 매크로.
 * pci_bus_alloc_resource()   : 크기/정렬/타입 조건에 맞는 빈 구간을 찾아 예약.
 *                              실패하면 상위 버스로 올라가며 다시 시도한다.
 * pci_bus_clip_resource()    : 브리지 윈도우 밖으로 삐져나온 자원을 잘라낸다.
 *                              펌웨어가 잘못 설정해 둔 경우를 수습하는 용도.
 * pci_bus_add_device()       : 장치 하나를 sysfs 에 올리고 드라이버 바인딩 시작.
 * pci_bus_add_devices()      : 버스 아래 모든 장치에 대해 위를 재귀 수행.
 * pci_walk_bus()             : 서브트리의 모든 장치에 콜백 적용(잠금 포함).
 * pci_walk_bus_reverse()     : 역순 순회. 제거 경로에서 잎부터 처리해야 할 때.
 * pci_bus_get() / pci_bus_put() : struct pci_bus 참조 카운트.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cleanup.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>

#include "pci.h"

/*
 * The first PCI_BRIDGE_RESOURCE_NUM PCI bus resources (those that correspond
 * to P2P or CardBus bridge windows) go in a table.  Additional ones (for
 * buses below host bridges or subtractive decode bridges) go in the list.
 * Use pci_bus_for_each_resource() to iterate through all the resources.
 */

struct pci_bus_resource {
	struct list_head	list;
	struct resource		*res;
};

/*
 * pci_add_resource_offset:
 *   PCI bus 리소스 목록에 리소스와 CPU↔bus 주소 오프셋을 추가한다.
 *   NVMe 장치의 BAR가 매핑될 메모리/IO window를 구성할 때 호스트 브리지나
 *   Root Port가 이 함수로 리소스를 등록한다.
 */
void pci_add_resource_offset(struct list_head *resources, struct resource *res,
			     resource_size_t offset)
{
	struct resource_entry *entry;

	entry = resource_list_create_entry(res, 0);
	if (!entry) {
		pr_err("PCI: can't add host bridge window %pR\n", res);
		return;
	}

	entry->offset = offset;
	resource_list_add_tail(entry, resources);
}
EXPORT_SYMBOL(pci_add_resource_offset);

/*
 * pci_add_resource:
 *   오프셋 0으로 pci_add_resource_offset()을 호출하여 리소스를 등록한다.
 *   NVMe BAR의 기본 매핑 정보가 포함된 리소스를 추가할 때 사용된다.
 */
void pci_add_resource(struct list_head *resources, struct resource *res)
{
	pci_add_resource_offset(resources, res, 0);
}
EXPORT_SYMBOL(pci_add_resource);

/*
 * pci_free_resource_list:
 *   PCI bus 리소스 목록을 해제한다.
 *   NVMe 장치가 제거되거나 host bridge teardown 시 관련 리소스 정리에
 *   사용될 수 있다.
 */
void pci_free_resource_list(struct list_head *resources)
{
	resource_list_free(resources);
}
/* [한국어] 주의: 이 심볼은 EXPORT_SYMBOL(GPL 아님)이다. 같은 파일의
 * pci_bus_resource_n 등이 EXPORT_SYMBOL_GPL 인 것과 대비되며, 비-GPL 모듈도
 * 이 함수를 쓸 수 있다는 뜻이다. 내보내기 종류는 코드이므로 바꾸면 안 된다. */
EXPORT_SYMBOL(pci_free_resource_list);

/*
 * pci_bus_add_resource:
 *   pci_bus 구조체의 추가 리소스 리스트에 리소스를 등록한다.
 *   NVMe 장치가 속한 bus가 사용하는 bridge window 이상의 추가 영역을
 *   관리할 때 사용된다.
 */
void pci_bus_add_resource(struct pci_bus *bus, struct resource *res)
{
	struct pci_bus_resource *bus_res;

	bus_res = kzalloc_obj(struct pci_bus_resource);
	if (!bus_res) {
		dev_err(&bus->dev, "can't add %pR resource\n", res);
		return;
	}

	bus_res->res = res;
	list_add_tail(&bus_res->list, &bus->resources);
}

/*
 * pci_bus_resource_n:
 *   bus에 등록된 n번째 리소스를 반환한다.
 *   NVMe driver가 pci_resource_start()로 얻는 BAR 리소스 외에, bridge
 *   window 등 bus 레벨의 n번째 리소스를 확인할 때 사용된다.
 */
struct resource *pci_bus_resource_n(const struct pci_bus *bus, int n)
{
	struct pci_bus_resource *bus_res;

	if (n < PCI_BRIDGE_RESOURCE_NUM)
		return bus->resource[n];

	n -= PCI_BRIDGE_RESOURCE_NUM;
	list_for_each_entry(bus_res, &bus->resources, list) {
		if (n-- == 0)
			return bus_res->res;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_bus_resource_n);

/*
 * pci_bus_remove_resource:
 *   bus에서 특정 리소스를 제거한다.
 *   NVMe 장치의 BAR나 bridge window가 재구성될 때 기존 리소스를 제거하는
 *   데 사용될 수 있다.
 */
void pci_bus_remove_resource(struct pci_bus *bus, struct resource *res)
{
	struct pci_bus_resource *bus_res, *tmp;
	int i;

	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) {
		if (bus->resource[i] == res) {
			bus->resource[i] = NULL;
			return;
		}
	}

	list_for_each_entry_safe(bus_res, tmp, &bus->resources, list) {
		if (bus_res->res == res) {
			list_del(&bus_res->list);
			kfree(bus_res);
			return;
		}
	}
}

/*
 * pci_bus_remove_resources:
 *   bus의 모든 리소스(고정 슬롯 및 추가 리스트)를 제거한다.
 *   NVMe 장치가 연결된 bus가 해제될 때 리소스 정리에 사용된다.
 */
void pci_bus_remove_resources(struct pci_bus *bus)
{
	int i;
	struct pci_bus_resource *bus_res, *tmp;

	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++)
		bus->resource[i] = NULL;

	list_for_each_entry_safe(bus_res, tmp, &bus->resources, list) {
		list_del(&bus_res->list);
		kfree(bus_res);
	}
}

/*
 * devm_request_pci_bus_resources:
 *   PCI bus 리소스 목록에서 IORESOURCE_IO/MEM 타입 리소스를 시스템 리소스
 *   트리(ioport_resource/iomem_resource)에 등록한다.
 *   NVMe BAR가 사용하는 메모리 영역이 시스템 전체 리소스 충돌 없이
 *   예약되도록 보장한다.
 */
int devm_request_pci_bus_resources(struct device *dev,
				   struct list_head *resources)
{
	struct resource_entry *win;
	struct resource *parent, *res;
	int err;

	resource_list_for_each_entry(win, resources) {
		res = win->res;
		switch (resource_type(res)) {
		case IORESOURCE_IO:
			parent = &ioport_resource;
			break;
		case IORESOURCE_MEM:
			parent = &iomem_resource;
			break;
		default:
			continue;
		}

		err = devm_request_resource(dev, parent, res);
		if (err)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(devm_request_pci_bus_resources);

static struct pci_bus_region pci_32_bit = {0, 0xffffffffULL};
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
static struct pci_bus_region pci_64_bit = {0,
				(pci_bus_addr_t) 0xffffffffffffffffULL};
static struct pci_bus_region pci_high = {(pci_bus_addr_t) 0x100000000ULL,
				(pci_bus_addr_t) 0xffffffffffffffffULL};
#endif

/*
 * @res contains CPU addresses.  Clip it so the corresponding bus addresses
 * on @bus are entirely within @region.  This is used to control the bus
 * addresses of resources we allocate, e.g., we may need a resource that
 * can be mapped by a 32-bit BAR.
 */

/*
 * pci_clip_resource_to_region:
 *   CPU 주소 기준의 res를 주어진 bus 주소 region에 맞게 자른다.
 *   NVMe 32비트 BAR는 4GB 이하 bus 주소만 표현 가능하므로, 64비트 영역을
 *   할당하면 안 되는 경우 이 함수로 범위를 제한한다.
 */
static void pci_clip_resource_to_region(struct pci_bus *bus,
					struct resource *res,
					struct pci_bus_region *region)
{
	struct pci_bus_region r;

	pcibios_resource_to_bus(bus, &r, res);
	if (r.start < region->start)
		r.start = region->start;
	if (r.end > region->end)
		r.end = region->end;

	if (r.end < r.start)
		res->end = res->start - 1;
	else
		pcibios_bus_to_resource(bus, res, &r);
}

/*
 * pci_bus_alloc_from_region:
 *   특정 PCI bus 주소 region 내에서 장치 리소스를 할당한다.
 *   NVMe BAR 할당 시 32비트/64비트 적합 영역을 찾아 메모리를 배정한다.
 */
static int pci_bus_alloc_from_region(struct pci_bus *bus, struct resource *res,
		resource_size_t size, resource_size_t align,
		resource_size_t min, unsigned long type_mask,
		resource_alignf alignf,
		void *alignf_data,
		struct pci_bus_region *region)
{
	struct resource *r, avail;
	resource_size_t max;
	int ret;

	type_mask |= IORESOURCE_TYPE_BITS;

	pci_bus_for_each_resource(bus, r) {
		resource_size_t min_used = min;

		if (!r)
			continue;

		if (r->flags & (IORESOURCE_UNSET|IORESOURCE_DISABLED))
			continue;

		/* type_mask must match */
		if ((res->flags ^ r->flags) & type_mask)
			continue;

		/* We cannot allocate a non-prefetching resource
		   from a pre-fetching area */
		if ((r->flags & IORESOURCE_PREFETCH) &&
		    !(res->flags & IORESOURCE_PREFETCH))
			continue;

		avail = *r;
		pci_clip_resource_to_region(bus, &avail, region);

		/*
		 * "min" is typically PCIBIOS_MIN_IO or PCIBIOS_MIN_MEM to
		 * protect badly documented motherboard resources, but if
		 * this is an already-configured bridge window, its start
		 * overrides "min".
		 */
		if (avail.start)
			min_used = avail.start;

		max = avail.end;

		/* Don't bother if available space isn't large enough */
		if (size > max - min_used + 1)
			continue;

		/* Ok, try it out.. */
		ret = allocate_resource(r, res, size, min_used, max,
					align, alignf, alignf_data);
		if (ret == 0)
			return 0;
	}
	return -ENOMEM;
}

/**
 * pci_bus_alloc_resource - allocate a resource from a parent bus
 * @bus: PCI bus
 * @res: resource to allocate
 * @size: size of resource to allocate
 * @align: alignment of resource to allocate
 * @min: minimum /proc/iomem address to allocate
 * @type_mask: IORESOURCE_* type flags
 * @alignf: resource alignment function
 * @alignf_data: data argument for resource alignment function
 *
 * Given the PCI bus a device resides on, the size, minimum address,
 * alignment and type, try to find an acceptable resource allocation
 * for a specific device resource.
 */

/*
 * pci_bus_alloc_resource:
 *   NVMe 장치가 속한 PCI bus로부터 size/align/type을 만족하는 리소스를
 *   할당한다. NVMe BAR(특히 BAR0의 doorbell/register 영역)가 사용할
 *   물리 메모리 공간을 확보하는 핵심 함수이다.
 */
int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res,
		resource_size_t size, resource_size_t align,
		resource_size_t min, unsigned long type_mask,
		resource_alignf alignf,
		void *alignf_data)
{
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	int rc;

	if (res->flags & IORESOURCE_MEM_64) {
		rc = pci_bus_alloc_from_region(bus, res, size, align, min,
				       type_mask, alignf, alignf_data,
				       &pci_high);
		if (rc == 0)
			return 0;

		return pci_bus_alloc_from_region(bus, res, size, align, min,
					 type_mask, alignf, alignf_data,
					 &pci_64_bit);
	}
#endif

	return pci_bus_alloc_from_region(bus, res, size, align, min,
				 type_mask, alignf, alignf_data,
				 &pci_32_bit);
}
EXPORT_SYMBOL(pci_bus_alloc_resource);

/*
 * The @idx resource of @dev should be a PCI-PCI bridge window.  If this
 * resource fits inside a window of an upstream bridge, do nothing.  If it
 * overlaps an upstream window but extends outside it, clip the resource so
 * it fits completely inside.
 */

/*
 * pci_bus_clip_resource:
 *   PCI-PCI bridge의 윈도우 리소스를 상위 bridge 윈도우 범위 내로 자른다.
 *   NVMe 장치가 여러 단계의 bridge 뒤에 있을 때, 중간 bridge의 메모리
 *   window가 상위 bridge를 벗어나지 않도록 보정한다.
 */
bool pci_bus_clip_resource(struct pci_dev *dev, int idx)
{
	struct pci_bus *bus = dev->bus;
	struct resource *res = &dev->resource[idx];
	struct resource orig_res = *res;
	struct resource *r;

	pci_bus_for_each_resource(bus, r) {
		resource_size_t start, end;

		if (!r)
			continue;

		if (resource_type(res) != resource_type(r))
			continue;

		start = max(r->start, res->start);
		end = min(r->end, res->end);

		if (start > end)
			continue;	/* no overlap */

		if (res->start == start && res->end == end)
			return false;	/* no change */

		res->start = start;
		res->end = end;
		res->flags &= ~IORESOURCE_UNSET;
		orig_res.flags &= ~IORESOURCE_UNSET;
		pci_info(dev, "%pR clipped to %pR\n", &orig_res, res);

		return true;
	}

	return false;
}

void __weak pcibios_resource_survey_bus(struct pci_bus *bus) { }

void __weak pcibios_bus_add_device(struct pci_dev *pdev) { }

/**
 * pci_bus_add_device - start driver for a single device
 * @dev: device to add
 *
 * This adds add sysfs entries and start device drivers
 */

/*
 * pci_bus_add_device:
 *   단일 PCI 장치(예: NVMe SSD)를 시스템에 등록하고 드라이버를 시작한다.
 *   NVMe 드라이버의 nvme_probe()이 호출되는 직접적인 진입점 중 하나이며,
 *   sysfs/proc 노드 생성, config space 저장, runtime PM 활성화 등을
 *   수행한다.
 */
void pci_bus_add_device(struct pci_dev *dev)
{
	struct device_node *dn = dev->dev.of_node;

	/*
	 * Can not put in pci_device_add yet because resources
	 * are not assigned yet for some devices.
	 */
	pcibios_bus_add_device(dev);
	pci_fixup_device(pci_fixup_final, dev);
	if (pci_is_bridge(dev))
		of_pci_make_dev_node(dev);
	pci_create_sysfs_dev_files(dev);
	pci_proc_attach_device(dev);
	pci_bridge_d3_update(dev);

	/* Save config space for error recoverability */
	pci_save_state(dev);

	/*
	 * Enable runtime PM, which potentially allows the device to
	 * suspend immediately, only after the PCI state has been
	 * configured completely.
	 */
	pm_runtime_enable(&dev->dev);

	if (!dn || of_device_is_available(dn))
		pci_dev_allow_binding(dev);

	device_initial_probe(&dev->dev);

	pci_dev_assign_added(dev);
}
EXPORT_SYMBOL_GPL(pci_bus_add_device);

/**
 * pci_bus_add_devices - start driver for PCI devices
 * @bus: bus to check for new devices
 *
 * Start driver for PCI devices and add some sysfs entries.
 */

/*
 * pci_bus_add_devices:
 *   주어진 PCI bus와 하위 bus의 모든 장치에 대해 pci_bus_add_device()를
 *   호출한다. NVMe SSD가 연결된 bus를 스캔할 때 nvme_probe()가 호출되는
 *   경로이다.
 */
void pci_bus_add_devices(const struct pci_bus *bus)
{
	struct pci_dev *dev;
	struct pci_bus *child;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* Skip already-added devices */
		if (pci_dev_is_added(dev))
			continue;
		pci_bus_add_device(dev);
	}

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* Skip if device attach failed */
		if (!pci_dev_is_added(dev))
			continue;
		child = dev->subordinate;
		if (child)
			pci_bus_add_devices(child);
	}
}
/* [한국어] 주의: 원본은 EXPORT_SYMBOL(GPL 아님)이다. 내보내기 종류는 코드이므로
 * 주석을 달면서 바꾸면 안 된다 — 비-GPL 모듈이 이 심볼을 못 쓰게 된다. */
EXPORT_SYMBOL(pci_bus_add_devices);

/*
 * __pci_walk_bus:
 *   주어진 bus와 그 하위 bus의 모든 장치에 대해 콜백을 순방향으로 호출.
 *   NVMe 장치를 포함한 전체 PCIe 계층을 순회할 때 사용.
 */
static int __pci_walk_bus(struct pci_bus *top, int (*cb)(struct pci_dev *, void *),
			  void *userdata)
{
	struct pci_dev *dev;
	int ret = 0;

	list_for_each_entry(dev, &top->devices, bus_list) {
		ret = cb(dev, userdata);
		if (ret)
			break;
		if (dev->subordinate) {
			ret = __pci_walk_bus(dev->subordinate, cb, userdata);
			if (ret)
				break;
		}
	}
	return ret;
}

/*
 * __pci_walk_bus_reverse:
 *   주어진 bus와 그 하위 bus의 모든 장치에 대해 콜백을 역방향으로 호출.
 *   NVMe 장치 제거 시 하위 장치부터 정리해야 할 때 사용.
 */
static int __pci_walk_bus_reverse(struct pci_bus *top,
				  int (*cb)(struct pci_dev *, void *),
				  void *userdata)
{
	struct pci_dev *dev;
	int ret = 0;

	list_for_each_entry_reverse(dev, &top->devices, bus_list) {
		if (dev->subordinate) {
			ret = __pci_walk_bus_reverse(dev->subordinate, cb,
					     userdata);
			if (ret)
				break;
		}
		ret = cb(dev, userdata);
		if (ret)
			break;
	}
	return ret;
}

/**
 *  pci_walk_bus - walk devices on/under bus, calling callback.
 *  @top: bus whose devices should be walked
 *  @cb: callback to be called for each device found
 *  @userdata: arbitrary pointer to be passed to callback
 *
 *  Walk the given bus, including any bridged devices
 *  on buses under this bus.  Call the provided callback
 *  on each device found.
 *
 *  We check the return of @cb each time. If it returns anything
 *  other than 0, we break out.
 */

/*
 * pci_walk_bus:
 *   pci_bus_sem read lock을 잡고 bus 트리를 순방향으로 순회한다.
 *   NVMe 장치의 전원/에러 복구/리셋 작업에서 전체 PCIe 계층을 안전하게
 *   순회할 때 사용된다.
 */
void pci_walk_bus(struct pci_bus *top, int (*cb)(struct pci_dev *, void *), void *userdata)
{
	down_read(&pci_bus_sem);
	__pci_walk_bus(top, cb, userdata);
	up_read(&pci_bus_sem);
}
EXPORT_SYMBOL_GPL(pci_walk_bus);

/**
 * pci_walk_bus_reverse - walk devices on/under bus, calling callback.
 * @top: bus whose devices should be walked
 * @cb: callback to be called for each device found
 * @userdata: arbitrary pointer to be passed to callback
 *
 * Same semantics as pci_walk_bus(), but walks the bus in reverse order.
 */

/*
 * pci_walk_bus_reverse:
 *   pci_bus_sem read lock을 잡고 bus 트리를 역방향으로 순회한다.
 *   NVMe suspend/remove 시 하위 장치부터 역순으로 처리해야 할 때 사용.
 */
void pci_walk_bus_reverse(struct pci_bus *top,
			  int (*cb)(struct pci_dev *, void *), void *userdata)
{
	down_read(&pci_bus_sem);
	__pci_walk_bus_reverse(top, cb, userdata);
	up_read(&pci_bus_sem);
}
EXPORT_SYMBOL_GPL(pci_walk_bus_reverse);

/*
 * pci_walk_bus_locked:
 *   호출자가 이미 pci_bus_sem을 잡고 있다고 가정하고 bus를 순회.
 *   NVMe AER 복구 등 lock context 내에서 재귀적으로 장치를 다룰 때 사용.
 */
void pci_walk_bus_locked(struct pci_bus *top, int (*cb)(struct pci_dev *, void *), void *userdata)
{
	lockdep_assert_held(&pci_bus_sem);

	__pci_walk_bus(top, cb, userdata);
}

/*
 * pci_bus_get:
 *   pci_bus의 참조 카운트를 증가시키고 반환.
 *   NVMe 드라이버가 bus 객체를 오래 유지해야 할 때 사용.
 */
struct pci_bus *pci_bus_get(struct pci_bus *bus)
{
	if (bus)
		get_device(&bus->dev);
	return bus;
}

/*
 * pci_bus_put:
 *   pci_bus의 참조 카운트를 감소.
 *   NVMe 드라이버가 bus 객체 사용을 마쳤을 때 반납.
 */
void pci_bus_put(struct pci_bus *bus)
{
	if (bus)
		put_device(&bus->dev);
}
