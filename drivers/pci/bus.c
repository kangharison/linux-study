// SPDX-License-Identifier: GPL-2.0
/*
 * From setup-res.c, by:
 *	Dave Rusling (david.rusling@reo.mts.dec.com)
 *	David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *	Ivan Kokshaysky (ink@jurassic.park.msu.ru)
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/bus.c)은 PCI bus 객체의 리소스 관리, 장치 추가,
 * bus 순회 등 PCI core의 핵심 기능을 구현한다.
 * NVMe SSD 입장에서 본 파일은 다음과 같은 동작에 직접 관여한다.
 *   - NVMe endpoint의 PCI bus 리소스(BAR0/1 등 doorbell/register 영역,
 *     DMA를 위한 메모리 공간) 할당 및 관리
 *   - NVMe 장치가 속한 bus의 상위 bridge window와의 정합성 확인(BAR
 *     클리핑)
 *   - NVMe 장치를 sysfs/proc에 노출하고, NVMe 드라이버(nvme_probe)를
 *     호출하기 위한 pci_bus_add_device()/pci_bus_add_devices() 처리
 *   - NVMe 장치의 runtime PM, config space 저장, bridge D3 상태 갱신 등
 *     전원 관련 초기화
 * 일반적인 NVMe 드라이버 호출 경로:
 *   nvme_probe -> pci_enable_device -> pci_request_regions ->
 *   pci_iomap -> doorbell access
 *   pci_bus_add_devices -> pci_bus_add_device -> device_initial_probe ->
 *   nvme_probe
 *   pci_bus_alloc_resource는 NVMe BAR의 크기/정렬을 만족하는 메모리
 *   영역을 상위 bus에서 할당하며, 이 주소가 NVMe driver가 사용하는
 *   pci_resource_start(pdev, 0) 값이 된다.
 *   pcibios_resource_to_bus()/pcibios_bus_to_resource()는 DMA 주소 변환,
 *   IOMMU/ATS, P2PDMA 시 NVMe BAR/버퍼의 CPU 물리 주소와 PCI bus 주소
 *   간 환산에 사용된다.
 * 본 파일은 drivers/nvme/host/pci.c가 직접 호출하지는 않으나, NVMe 장치의
 * PCI 리소스 할당, 드라이버 바인딩, 전원/라이프사이클 관리의 토대가 된다.
 * ===================================================================
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

/* NVMe: bus별 추가 리소스를 연결 리스트로 관리하기 위한 날것의 구조체. */
struct pci_bus_resource {
	struct list_head	list; /* NVMe: bus 리소스 연결 리스트의 노드. */
	struct resource		*res; /* NVMe: NVMe 장치가 사용 가능한 bus 리소스를 가리킴. */
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
	struct resource_entry *entry; /* NVMe: 등록할 리소스 항목 포인터. */

	entry = resource_list_create_entry(res, 0); /* NVMe: res를 연결 리스트에 담을 entry를 할당. */
	if (!entry) { /* NVMe: entry 할당 실패 시. */
		pr_err("PCI: can't add host bridge window %pR\n", res); /* NVMe: 오류 로그: NVMe BAR가 사용할 window 등록 실패. */
		return; /* NVMe: 등록 중단. */
	}

	entry->offset = offset; /* NVMe: CPU 물리 주소와 PCI bus 주소 간 오프셋 기록. */
	resource_list_add_tail(entry, resources); /* NVMe: resources 리스트 끝에 추가. */
}
EXPORT_SYMBOL(pci_add_resource_offset);

/*
 * pci_add_resource:
 *   오프셋 0으로 pci_add_resource_offset()을 호출하여 리소스를 등록한다.
 *   NVMe BAR의 기본 매핑 정보가 포함된 리소스를 추가할 때 사용된다.
 */
void pci_add_resource(struct list_head *resources, struct resource *res)
{
	pci_add_resource_offset(resources, res, 0); /* NVMe: offset 0으로 리소스를 추가. */
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
	resource_list_free(resources); /* NVMe: resources 리스트의 모든 entry를 해제. */
}
EXPORT_SYMBOL(pci_free_resource_list);

/*
 * pci_bus_add_resource:
 *   pci_bus 구조체의 추가 리소스 리스트에 리소스를 등록한다.
 *   NVMe 장치가 속한 bus가 사용하는 bridge window 이상의 추가 영역을
 *   관리할 때 사용된다.
 */
void pci_bus_add_resource(struct pci_bus *bus, struct resource *res)
{
	struct pci_bus_resource *bus_res; /* NVMe: bus별 추가 리소스 항목 포인터. */

	bus_res = kzalloc_obj(struct pci_bus_resource); /* NVMe: pci_bus_resource 구조체를 kzalloc로 할당. */
	if (!bus_res) { /* NVMe: 메모리 할당 실패 시. */
		dev_err(&bus->dev, "can't add %pR resource\n", res); /* NVMe: bus device에 리소스 추가 실패 로그. */
		return; /* NVMe: 추가 작업 중단. */
	}

	bus_res->res = res; /* NVMe: 추가 리소스 포인터 저장. */
	list_add_tail(&bus_res->list, &bus->resources); /* NVMe: bus->resources 리스트 끝에 추가. */
}

/*
 * pci_bus_resource_n:
 *   bus에 등록된 n번째 리소스를 반환한다.
 *   NVMe driver가 pci_resource_start()로 얻는 BAR 리소스 외에, bridge
 *   window 등 bus 레벨의 n번째 리소스를 확인할 때 사용된다.
 */
struct resource *pci_bus_resource_n(const struct pci_bus *bus, int n)
{
	struct pci_bus_resource *bus_res; /* NVMe: 추가 리소스 순회용 포인터. */

	if (n < PCI_BRIDGE_RESOURCE_NUM) /* NVMe: 인덱스가 고정 bridge resource 범위 내이면. */
		return bus->resource[n]; /* NVMe: 배열에 직접 저장된 리소스 반환. */

	n -= PCI_BRIDGE_RESOURCE_NUM; /* NVMe: 추가 리소스 리스트의 인덱스로 변환. */
	list_for_each_entry(bus_res, &bus->resources, list) { /* NVMe: bus->resources 연결 리스트를 순회. */
		if (n-- == 0) /* NVMe: 목표 인덱스에 도달하면. */
			return bus_res->res; /* NVMe: 해당 추가 리소스 반환. */
	}
	return NULL; /* NVMe: 인덱스가 범위를 벗어나면 NULL 반환. */
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
	struct pci_bus_resource *bus_res, *tmp; /* NVMe: 추가 리소스 순회 및 임시 포인터. */
	int i; /* NVMe: 고정 bridge resource 인덱스. */

	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) { /* NVMe: 고정 bridge resource 슬롯을 순회. */
		if (bus->resource[i] == res) { /* NVMe: 제거하려는 리소스를 찾으면. */
			bus->resource[i] = NULL; /* NVMe: 해당 슬롯을 비운다. */
			return; /* NVMe: 제거 완료. */
		}
	}

	list_for_each_entry_safe(bus_res, tmp, &bus->resources, list) { /* NVMe: 추가 리소스 리스트를 안전하게 순회. */
		if (bus_res->res == res) { /* NVMe: 제거 대상 리소스를 찾으면. */
			list_del(&bus_res->list); /* NVMe: 리스트에서 노드 분리. */
			kfree(bus_res); /* NVMe: 노드 메모리 해제. */
			return; /* NVMe: 제거 완료. */
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
	int i; /* NVMe: 고정 bridge resource 인덱스. */
	struct pci_bus_resource *bus_res, *tmp; /* NVMe: 추가 리소스 순회용. */

	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) /* NVMe: 모든 고정 bridge resource 슬롯을 순회. */
		bus->resource[i] = NULL; /* NVMe: 각 슬롯을 NULL로 초기화. */

	list_for_each_entry_safe(bus_res, tmp, &bus->resources, list) { /* NVMe: 추가 리소스 리스트를 순회하며. */
		list_del(&bus_res->list); /* NVMe: 리스트에서 노드 분리. */
		kfree(bus_res); /* NVMe: 노드 메모리 해제. */
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
	struct resource_entry *win; /* NVMe: bus 리소스 목록의 각 항목을 순회. */
	struct resource *parent, *res; /* NVMe: 시스템 상위 리소스와 현재 리소스. */
	int err; /* NVMe: 등록 결과 코드. */

	resource_list_for_each_entry(win, resources) { /* NVMe: resources 리스트의 각 entry를 순회. */
		res = win->res; /* NVMe: 현재 entry의 resource 포인터 획득. */
		switch (resource_type(res)) { /* NVMe: 리소스 타입에 따라 상위 리소스 선택. */
		case IORESOURCE_IO: /* NVMe: IO 포트 리소스인 경우. */
			parent = &ioport_resource; /* NVMe: 시스템 IO 포트 트리의 루트 사용. */
			break; /* NVMe: IO 처리 종료. */
		case IORESOURCE_MEM: /* NVMe: 메모리 리소스인 경우(NVMe BAR가 대부분). */
			parent = &iomem_resource; /* NVMe: 시스템 메모리 리소스 트리의 루트 사용. */
			break; /* NVMe: MEM 처리 종료. */
		default: /* NVMe: IO/MEM 외 리소스는 등록할 필요 없음. */
			continue; /* NVMe: 다음 entry로 걄뜀. */
		}

		err = devm_request_resource(dev, parent, res); /* NVMe: 상위 리소스 아래 res를 예약. */
		if (err) /* NVMe: 예약 실패 시. */
			return err; /* NVMe: 오류 코드 반환. */
	}

	return 0; /* NVMe: 모든 리소스 예약 성공. */
}
EXPORT_SYMBOL_GPL(devm_request_pci_bus_resources);

/* NVMe: 32비트 PCI bus 주소 전체 범위를 나타내는 상수(0~4GB). */
static struct pci_bus_region pci_32_bit = {0, 0xffffffffULL};
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
/* NVMe: 64비트 PCI bus 주소 전체 범위를 나타내는 상수. */
static struct pci_bus_region pci_64_bit = {0,
				(pci_bus_addr_t) 0xffffffffffffffffULL};
/* NVMe: 64비트 공간 중 4GB 이상 고주소 영역을 나타내는 상수(NVMe 64bit BAR 할당 시 사용). */
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
	struct pci_bus_region r; /* NVMe: res를 bus 주소로 변환한 임시 영역. */

	pcibios_resource_to_bus(bus, &r, res); /* NVMe: CPU 주소 res를 PCI bus 주소 r로 변환. */
	if (r.start < region->start) /* NVMe: 시작 주소가 region 아래쪽을 벗어나면. */
		r.start = region->start; /* NVMe: region 시작으로 클리핑. */
	if (r.end > region->end) /* NVMe: 끝 주소가 region 위쪽을 벗어나면. */
		r.end = region->end; /* NVMe: region 끝으로 클리핑. */

	if (r.end < r.start) /* NVMe: 클리핑 결과 유효하지 않은 범위이면. */
		res->end = res->start - 1; /* NVMe: 빈 리소스로 표시. */
	else /* NVMe: 유효한 범위이면. */
		pcibios_bus_to_resource(bus, res, &r); /* NVMe: 다시 CPU 주소 res로 변환. */
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
	struct resource *r, avail; /* NVMe: bus 리소스와 클리핑 후 가용 영역. */
	resource_size_t max; /* NVMe: 할당 가능한 최대 주소. */
	int ret; /* NVMe: 할당 결과 코드. */

	type_mask |= IORESOURCE_TYPE_BITS; /* NVMe: 타입 비트를 항상 포함하도록 마스크 갱신. */

	pci_bus_for_each_resource(bus, r) { /* NVMe: NVMe 장치가 속한 bus의 모든 리소스를 순회. */
		resource_size_t min_used = min; /* NVMe: 실제로 사용할 최소 주소(기본값은 min). */

		if (!r) /* NVMe: 유효하지 않은 리소스 슬롯은 걄뜀. */
			continue; /* NVMe: 다음 리소스로 이동. */

		if (r->flags & (IORESOURCE_UNSET|IORESOURCE_DISABLED)) /* NVMe: 설정되지 않거나 비활성화된 리소스는 걄뜀. */
			continue; /* NVMe: 다음 리소스로 이동. */

		/* type_mask must match */
		if ((res->flags ^ r->flags) & type_mask) /* NVMe: 요청 리소스와 bus 리소스의 타입이 다른지 확인. */
			continue; /* NVMe: 타입 불일치 시 걄뜀. */

		/* We cannot allocate a non-prefetching resource
		   from a pre-fetching area */
		if ((r->flags & IORESOURCE_PREFETCH) && /* NVMe: bus 리소스가 prefetchable이고. */
		    !(res->flags & IORESOURCE_PREFETCH)) /* NVMe: 요청 리소스가 prefetchable이 아니면. */
			continue; /* NVMe: non-prefetch 리소스를 prefetch 영역에서 할당할 수 없음. */

		avail = *r; /* NVMe: 현재 bus 리소스를 복사. */
		pci_clip_resource_to_region(bus, &avail, region); /* NVMe: region 범위 내로 클리핑. */

		/*
		 * "min" is typically PCIBIOS_MIN_IO or PCIBIOS_MIN_MEM to
		 * protect badly documented motherboard resources, but if
		 * this is an already-configured bridge window, its start
		 * overrides "min".
		 */
		if (avail.start) /* NVMe: 이미 설정된 bridge window의 시작이 0이 아니면. */
			min_used = avail.start; /* NVMe: 최소 주소를 bridge window 시작으로 설정. */

		max = avail.end; /* NVMe: 가용 영역의 끝을 최대 주소로 설정. */

		/* Don't bother if available space isn't large enough */
		if (size > max - min_used + 1) /* NVMe: 요청 size가 가용 공간보다 크면. */
			continue; /* NVMe: 이 리소스에서는 할당 불가. */

		/* Ok, try it out.. */
		ret = allocate_resource(r, res, size, min_used, max,
					align, alignf, alignf_data); /* NVMe: 실제로 리소스 트리에서 size만큼 할당 시도. */
		if (ret == 0) /* NVMe: 할당 성공 시. */
			return 0; /* NVMe: 성공 반환. */
	}
	return -ENOMEM; /* NVMe: 모든 bus 리소스에서 할당 실패. */
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
	int rc; /* NVMe: 64비트 할당 시도 결과. */

	if (res->flags & IORESOURCE_MEM_64) { /* NVMe: NVMe BAR가 64비트 메모리 리소스이면. */
		rc = pci_bus_alloc_from_region(bus, res, size, align, min,
					       type_mask, alignf, alignf_data,
					       &pci_high); /* NVMe: 우선 4GB 이상 고주소 영역에서 할당 시도. */
		if (rc == 0) /* NVMe: 고주소 영역 할당 성공 시. */
			return 0; /* NVMe: 성공 반환. */

		return pci_bus_alloc_from_region(bus, res, size, align, min,
						 type_mask, alignf, alignf_data,
						 &pci_64_bit); /* NVMe: 고주소 실패 시 전체 64비트 영역에서 재시도. */
	}
#endif

	return pci_bus_alloc_from_region(bus, res, size, align, min,
					 type_mask, alignf, alignf_data,
					 &pci_32_bit); /* NVMe: 32비트 BAR 또는 32비트 모드에서는 4GB 이하 영역에서 할당. */
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
	struct pci_bus *bus = dev->bus; /* NVMe: 대상 bridge가 속한 PCI bus 획득. */
	struct resource *res = &dev->resource[idx]; /* NVMe: 자를 대상 bridge window 리소스. */
	struct resource orig_res = *res; /* NVMe: 변경 전 원본 리소스를 보존. */
	struct resource *r; /* NVMe: 상위 bus 리소스 순회용. */

	pci_bus_for_each_resource(bus, r) { /* NVMe: 상위 bus의 모든 리소스를 순회. */
		resource_size_t start, end; /* NVMe: 겹치는 영역의 시작/끝. */

		if (!r) /* NVMe: 유효하지 않은 상위 리소스는 걄뜀. */
			continue; /* NVMe: 다음 리소스로 이동. */

		if (resource_type(res) != resource_type(r)) /* NVMe: IO/MEM 타입이 다른지 확인. */
			continue; /* NVMe: 타입 불일치 시 걄뜀. */

		start = max(r->start, res->start); /* NVMe: 두 리소스의 시작 중 큰 값으로 겹침 시작. */
		end = min(r->end, res->end); /* NVMe: 두 리소스의 끝 중 작은 값으로 겹침 끝. */

		if (start > end) /* NVMe: 겹치는 영역이 없으면. */
			continue;	/* no overlap */ /* NVMe: 다음 상위 리소스로 이동. */

		if (res->start == start && res->end == end) /* NVMe: 이미 상위 윈도우 안에 완전히 포함되면. */
			return false;	/* no change */ /* NVMe: 변경 불필요. */

		res->start = start; /* NVMe: 시작을 겹침 영역으로 갱신. */
		res->end = end; /* NVMe: 끝을 겹침 영역으로 갱신. */
		res->flags &= ~IORESOURCE_UNSET; /* NVMe: 리소스가 설정되었음을 표시. */
		orig_res.flags &= ~IORESOURCE_UNSET; /* NVMe: 원본에도 설정 플래그 반영(로깅용). */
		pci_info(dev, "%pR clipped to %pR\n", &orig_res, res); /* NVMe: 클리핑 내용을 로그로 기록. */

		return true; /* NVMe: 리소스가 변경되었음을 반환. */
	}

	return false; /* NVMe: 변경이 발생하지 않았음을 반환. */
}

/* NVMe: 아키텍처별 bus 리소스 조사 weak 함수(기본 no-op). */
void __weak pcibios_resource_survey_bus(struct pci_bus *bus) { }

/* NVMe: 아키텍처별 bus 장치 추가 weak 함수(기본 no-op). */
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
	struct device_node *dn = dev->dev.of_node; /* NVMe: 장치의 device tree 노드 포인터 획득. */

	/*
	 * Can not put in pci_device_add yet because resources
	 * are not assigned yet for some devices.
	 */
	pcibios_bus_add_device(dev); /* NVMe: 아키텍처별 추가 초기화(예: firmware quirk). */
	pci_fixup_device(pci_fixup_final, dev); /* NVMe: 최종 PCI quirk 적용(NVMe 호환성 보정). */
	if (pci_is_bridge(dev)) /* NVMe: 대상이 PCI bridge이면. */
		of_pci_make_dev_node(dev); /* NVMe: device tree 노드를 bridge에 연결. */
	pci_create_sysfs_dev_files(dev); /* NVMe: /sys/bus/pci/devices/... 아래 장치 파일 생성. */
	pci_proc_attach_device(dev); /* NVMe: /proc/bus/pci/... 노드 생성. */
	pci_bridge_d3_update(dev); /* NVMe: bridge D3 상태 갱신(전원 관리, ASPM 관련). */

	/* Save config space for error recoverability */
	pci_save_state(dev); /* NVMe: NVMe 장치의 PCI config space를 저장(AER 복구 시 복원). */

	/*
	 * Enable runtime PM, which potentially allows the device to
	 * suspend immediately, only after the PCI state has been
	 * configured completely.
	 */
	pm_runtime_enable(&dev->dev); /* NVMe: runtime PM 활성화(NVMe idle 시 저전력 전환 가능). */

	if (!dn || of_device_is_available(dn)) /* NVMe: device tree에서 사용 가능하거나 OF를 사용하지 않으면. */
		pci_dev_allow_binding(dev); /* NVMe: 드라이버 바인딩을 허용. */

	device_initial_probe(&dev->dev); /* NVMe: 등록된 드라이버 중 일치하는 드라이버를 찾아 probe 수행(NVMe의 경우 nvme_probe). */

	pci_dev_assign_added(dev); /* NVMe: 장치가 추가되었음을 표시. */
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
	struct pci_dev *dev; /* NVMe: 현재 bus의 PCI 장치 순회용. */
	struct pci_bus *child; /* NVMe: 하위 bus 포인터. */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: bus의 장치 목록을 순회. */
		/* Skip already-added devices */
		if (pci_dev_is_added(dev)) /* NVMe: 이미 추가된 장치는 걄뜀. */
			continue; /* NVMe: 다음 장치로 이동. */
		pci_bus_add_device(dev); /* NVMe: NVMe 장치 등록 및 드라이버 시작. */
	}

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: 다시 장치 목록을 순회. */
		/* Skip if device attach failed */
		if (!pci_dev_is_added(dev)) /* NVMe: 이전 단계에서 추가 실패한 장치는 걄뜀. */
			continue; /* NVMe: 다음 장치로 이동. */
		child = dev->subordinate; /* NVMe: 현재 장치가 bridge인 경우 하위 bus 획득. */
		if (child) /* NVMe: 하위 bus가 존재하면. */
			pci_bus_add_devices(child); /* NVMe: 재귀적으로 하위 bus의 장치 등록. */
	}
}
EXPORT_SYMBOL_GPL(pci_bus_add_devices);

/*
 * __pci_walk_bus:
 *   주어진 bus와 그 하위 bus의 모든 장치에 대해 콜백을 순방향으로 호출.
 *   NVMe 장치를 포함한 전체 PCIe 계층을 순회할 때 사용.
 */
static int __pci_walk_bus(struct pci_bus *top, int (*cb)(struct pci_dev *, void *),
			  void *userdata)
{
	struct pci_dev *dev; /* NVMe: 순회 중인 PCI 장치. */
	int ret = 0; /* NVMe: 콜백 반환값. */

	list_for_each_entry(dev, &top->devices, bus_list) { /* NVMe: top bus의 장치 목록 순회. */
		ret = cb(dev, userdata); /* NVMe: NVMe 장치에 대해 등록된 콜백 실행. */
		if (ret) /* NVMe: 콜백이 0이 아닌 값을 반환하면. */
			break; /* NVMe: 순회 중단. */
		if (dev->subordinate) { /* NVMe: 현재 장치가 bridge이면. */
			ret = __pci_walk_bus(dev->subordinate, cb, userdata); /* NVMe: 하위 bus 재귀 순회. */
			if (ret) /* NVMe: 하위 순회에서 중단 요청이 오면. */
				break; /* NVMe: 상위 순회도 중단. */
		}
	}
	return ret; /* NVMe: 최종 콜백 반환값. */
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
	struct pci_dev *dev; /* NVMe: 순회 중인 PCI 장치. */
	int ret = 0; /* NVMe: 콜백 반환값. */

	list_for_each_entry_reverse(dev, &top->devices, bus_list) { /* NVMe: 장치 목록을 역순으로 순회. */
		if (dev->subordinate) { /* NVMe: bridge의 하위 bus가 있으면. */
			ret = __pci_walk_bus_reverse(dev->subordinate, cb,
						     userdata); /* NVMe: 하위 bus를 먼저 역순 순회. */
			if (ret) /* NVMe: 하위 순회 중단 요청 시. */
				break; /* NVMe: 상위 순회 중단. */
		}
		ret = cb(dev, userdata); /* NVMe: NVMe 장치에 대해 콜백 실행. */
		if (ret) /* NVMe: 콜백이 0이 아닌 값을 반환하면. */
			break; /* NVMe: 순회 중단. */
	}
	return ret; /* NVMe: 최종 콜백 반환값. */
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
	down_read(&pci_bus_sem); /* NVMe: PCI bus 구조 read lock 획득. */
	__pci_walk_bus(top, cb, userdata); /* NVMe: 실제 순회 수행. */
	up_read(&pci_bus_sem); /* NVMe: read lock 해제. */
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
	down_read(&pci_bus_sem); /* NVMe: PCI bus 구조 read lock 획득. */
	__pci_walk_bus_reverse(top, cb, userdata); /* NVMe: 역순 순회 수행. */
	up_read(&pci_bus_sem); /* NVMe: read lock 해제. */
}
EXPORT_SYMBOL_GPL(pci_walk_bus_reverse);

/*
 * pci_walk_bus_locked:
 *   호출자가 이미 pci_bus_sem을 잡고 있다고 가정하고 bus를 순회.
 *   NVMe AER 복구 등 lock context 내에서 재귀적으로 장치를 다룰 때 사용.
 */
void pci_walk_bus_locked(struct pci_bus *top, int (*cb)(struct pci_dev *, void *), void *userdata)
{
	lockdep_assert_held(&pci_bus_sem); /* NVMe: pci_bus_sem이 잡혀 있음을 lockdep으로 검증. */

	__pci_walk_bus(top, cb, userdata); /* NVMe: lock 획득 없이 순회 수행. */
}

/*
 * pci_bus_get:
 *   pci_bus의 참조 카운트를 증가시키고 반환.
 *   NVMe 드라이버가 bus 객체를 오래 유지해야 할 때 사용.
 */
struct pci_bus *pci_bus_get(struct pci_bus *bus)
{
	if (bus) /* NVMe: 유효한 bus 포인터인 경우. */
		get_device(&bus->dev); /* NVMe: bus의 struct device 참조 카운트 증가. */
	return bus; /* NVMe: 참조 증가된 bus 포인터 반환(또는 NULL). */
}

/*
 * pci_bus_put:
 *   pci_bus의 참조 카운트를 감소.
 *   NVMe 드라이버가 bus 객체 사용을 마쳤을 때 반납.
 */
void pci_bus_put(struct pci_bus *bus)
{
	if (bus) /* NVMe: 유효한 bus 포인터인 경우. */
		put_device(&bus->dev); /* NVMe: bus의 struct device 참조 카운트 감소. */
}
