// SPDX-License-Identifier: GPL-2.0
/*
 * Host bridge related code
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/host-bridge.c)은 PCI host bridge(Root Complex의
 * 일부)를 찾고, CPU 물리 주소와 PCI bus 주소 간 변환을 수행한다.
 * NVMe SSD 입장에서 host bridge는 PCIe 트리의 루트이며, 다음과 같은
 * NVMe 동작에 직접 관여한다.
 *   - NVMe endpoint가 연결된 Root Port부터 host bridge까지의 경로 결정
 *   - NVMe BAR(특히 BAR0의 doorbell/register 영역)의 CPU 주소 ↔ bus
 *     주소 변환
 *   - DMA 주소 변환 시 root bus의 address window 기준 참조
 * 일반적인 NVMe 드라이버 호출 경로:
 *   nvme_probe -> pci_enable_device -> pci_request_regions ->
 *   pci_iomap(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0)) ->
 *   doorbell access
 * 여기서 pci_resource_start()는 남장기 bus 주소이며, ioremap() 등에서
 * 사용되기 전에 pcibios_bus_to_resource()를 통해 CPU 물리 주소로 변환
 * 되는 경우가 있다. 또한 P2PDMA나 IOMMU 설정 시 host bridge의 window
 * 정보가 사용된다.
 * 본 파일은 drivers/pci/probe.c, drivers/pci/setup-bus.c,
 * drivers/pci/iomap.c 등에서 간접적으로 사용되며, NVMe 드라이버는
 * 직접 호출하지 않지만 NVMe 장치의 주소 공간 해석에 근간이 된다.
 * ===================================================================
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>

#include "pci.h"

/*
 * find_pci_root_bus:
 *   주어진 pci_bus 구조체를 따라 최상위 root bus를 찾는다.
 *   NVMe 장치가 연결된 하위 bus에서 시작해 Root Complex 아래 root bus를
 *   얻는 데 사용된다. 이 root bus가 host bridge를 가리킨다.
 */
static struct pci_bus *find_pci_root_bus(struct pci_bus *bus)
{
	while (bus->parent) /* NVMe: 현재 bus의 상위 bus가 있으면 계속 따라 올라간다. */
		bus = bus->parent; /* NVMe: parent 포인터를 타고 root 방향으로 이동. */

	return bus; /* NVMe: parent가 NULL인 최상위 root bus 반환. */
}

/*
 * pci_find_host_bridge:
 *   임의의 PCI bus가 속한 pci_host_bridge 구조체를 반환한다.
 *   NVMe 장치의 pci_dev->bus를 넘기면 해당 NVMe가 연결된 host bridge의
 *   메타정보(예: domain_nr, windows, MSI/IRQ 도메인)를 얻을 수 있다.
 */
struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *bus)
{
	struct pci_bus *root_bus = find_pci_root_bus(bus); /* NVMe: NVMe bus의 root bus 획득. */

	return to_pci_host_bridge(root_bus->bridge); /* NVMe: root bus의 bridge 객체를 host bridge로 변환하여 반환. */
}
EXPORT_SYMBOL_GPL(pci_find_host_bridge);

/*
 * pci_get_host_bridge_device:
 *   NVMe 장치가 연결된 host bridge의 struct device 참조 카운트를 증가시키고
 *   반환한다. host bridge의 전원/라이프사이클 관리에 사용된다.
 */
struct device *pci_get_host_bridge_device(struct pci_dev *dev)
{
	struct pci_bus *root_bus = find_pci_root_bus(dev->bus); /* NVMe: NVMe 장치가 속한 bus tree의 root bus 획득. */
	struct device *bridge = root_bus->bridge; /* NVMe: root bus에 매핑된 bridge device 획득. */

	kobject_get(&bridge->kobj); /* NVMe: bridge device의 kobject 참조 카운트 증가(사용 중 유지). */
	return bridge; /* NVMe: 참조 증가된 host bridge device 반환. */
}
EXPORT_SYMBOL_GPL(pci_get_host_bridge_device);

/*
 * pci_put_host_bridge_device:
 *   pci_get_host_bridge_device()로 획득한 host bridge device의 참조를
 *   해제한다. NVMe 드라이버가 host bridge 관련 리소스 사용 후 반납할 때
 *   호출된다.
 */
void  pci_put_host_bridge_device(struct device *dev)
{
	kobject_put(&dev->kobj); /* NVMe: host bridge device의 kobject 참조 카운트 감소. */
}

/*
 * pci_set_host_bridge_release:
 *   host bridge가 해제될 때 호출될 release 콜백과 데이터를 등록한다.
 *   NVMe 장치가 연결된 host bridge의 teardown 시 필요한 정리 작업을
 *   platform/driver specific하게 수행할 수 있게 한다.
 */
void pci_set_host_bridge_release(struct pci_host_bridge *bridge,
				 void (*release_fn)(struct pci_host_bridge *),
				 void *release_data)
{
	bridge->release_fn = release_fn; /* NVMe: host bridge 해제 시 호출할 함수 포인터 저장. */
	bridge->release_data = release_data; /* NVMe: release 콜백에 전달할 platform data 저장. */
}
EXPORT_SYMBOL_GPL(pci_set_host_bridge_release);

/*
 * pcibios_resource_to_bus:
 *   CPU 물리 주소(resource)를 PCI bus 주소 공간(bus_region)으로 변환한다.
 *   NVMe BAR가 가리키는 CPU 물리 주소를 bus 주소로 환산할 때 사용되며,
 *   P2PDMA, ATS, IOMMU 등에서 중요하다.
 */
void pcibios_resource_to_bus(struct pci_bus *bus, struct pci_bus_region *region,
			     struct resource *res)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus); /* NVMe: 해당 bus의 host bridge 획득. */
	struct resource_entry *window; /* NVMe: host bridge의 address window를 순회할 포인터. */
	resource_size_t offset = 0; /* NVMe: CPU 주소와 bus 주소 간 오프셋(0으로 초기화). */

	resource_list_for_each_entry(window, &bridge->windows) { /* NVMe: host bridge가 노출한 모든 address window를 순회. */
		if (resource_contains(window->res, res)) { /* NVMe: NVMe BAR 리소스가 현재 window 안에 포함되는지 확인. */
			offset = window->offset; /* NVMe: 포함되면 해당 window의 CPU↔bus 오프셋 기록. */
			break; /* NVMe: 적절한 window를 찾았으므로 순회 종료. */
		}
	}

	region->start = res->start - offset; /* NVMe: CPU 시작 주소에서 오프셋을 빼 bus 시작 주소 산출. */
	region->end = res->end - offset; /* NVMe: CPU 끝 주소에서 오프셋을 빼 bus 끝 주소 산출. */
}
EXPORT_SYMBOL(pcibios_resource_to_bus);

/*
 * region_contains:
 *   bus_region1이 bus_region2를 완전히 포함하는지 검사한다.
 *   NVMe BAR가 특정 host bridge window 안에 들어오는지 판단할 때 쓰인다.
 */
static bool region_contains(struct pci_bus_region *region1,
			    struct pci_bus_region *region2)
{
	return region1->start <= region2->start && region1->end >= region2->end; /* NVMe: region2의 시작/끝이 모두 region1 안에 있으면 true. */
}

/*
 * pcibios_bus_to_resource:
 *   PCI bus 주소(region)를 CPU 물리 주소(resource)로 변환한다.
 *   NVMe driver가 pci_resource_start()로 얻은 bus 주소를 ioremap()이나
 *   DMA 주소 설정에 사용하기 위해 CPU 물리 주소로 환산할 때 사용된다.
 */
void pcibios_bus_to_resource(struct pci_bus *bus, struct resource *res,
			     struct pci_bus_region *region)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus); /* NVMe: NVMe bus가 속한 host bridge 획득. */
	struct resource_entry *window; /* NVMe: host bridge window 순회용. */
	resource_size_t offset = 0; /* NVMe: bus→CPU 변환에 사용할 오프셋. */

	resource_list_for_each_entry(window, &bridge->windows) { /* NVMe: host bridge의 모든 address window를 순회. */
		struct pci_bus_region bus_region; /* NVMe: 현재 window를 bus 주소 공간으로 환산한 임시 영역. */

		if (resource_type(res) != resource_type(window->res)) /* NVMe: NVMe BAR의 resource type(MEM/IO)과 window type이 다른지 확인. */
			continue; /* NVMe: type이 맞지 않으면 이 window는 사용 불가. */

		bus_region.start = window->res->start - window->offset; /* NVMe: window 시작을 bus 주소로 변환. */
		bus_region.end = window->res->end - window->offset; /* NVMe: window 끝을 bus 주소로 변환. */

		if (region_contains(&bus_region, region)) { /* NVMe: 변환 대상 region이 이 window 안에 들어오는지 확인. */
			offset = window->offset; /* NVMe: 적합한 window의 오프셋 선택. */
			break; /* NVMe: 적절한 window를 찾았으므로 순회 종료. */
		}
	}

	res->start = region->start + offset; /* NVMe: bus 시작 주소에 오프셋을 더해 CPU 물리 시작 주소 산출. */
	res->end = region->end + offset; /* NVMe: bus 끝 주소에 오프셋을 더해 CPU 물리 끝 주소 산출. */
}
EXPORT_SYMBOL(pcibios_bus_to_resource);
