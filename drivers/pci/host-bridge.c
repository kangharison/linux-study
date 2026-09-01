// SPDX-License-Identifier: GPL-2.0
/*
 * Host bridge related code
 */

/*
 * [한국어 설명] 호스트 브리지 조회와 CPU 주소 ↔ PCI 버스 주소 변환 (host-bridge.c)
 *
 * === 파일의 역할 ===
 * 아주 짧은 파일이고 하는 일도 둘뿐이다.
 *
 *   1) 어떤 장치가 속한 호스트 브리지를 찾아 준다(pci_find_host_bridge).
 *      버스 트리를 부모 쪽으로 계속 올라가 루트 버스를 만나면, 그 버스의
 *      bridge 필드가 struct pci_host_bridge 다.
 *   2) CPU 물리 주소와 PCI 버스 주소 사이를 변환한다
 *      (pcibios_resource_to_bus / pcibios_bus_to_resource).
 *
 * 2번이 이 파일의 존재 이유다. x86 에서는 두 주소가 같아서 변환이 항등이고,
 * 그래서 이 함수들이 왜 있는지 잘 와닿지 않는다. 하지만 많은 임베디드 SoC 와
 * 일부 서버 아키텍처에서는 다르다. PCIe 컨트롤러가 CPU 물리 주소
 * 0xC0000000 에 매핑한 창을, PCI 버스 쪽에서는 0x00000000 으로 보이게
 * 하는 식이다. 이 차이를 "오프셋" 이라 부르고, 호스트 브리지 드라이버가
 * pci_add_resource_offset() 으로 등록해 둔다.
 *
 * 왜 이 구분이 필요한가. BAR 레지스터에는 반드시 PCI 버스 주소가 들어가야
 * 한다 — 장치가 그 값으로 자기 창을 인식하기 때문이다. 반면 CPU 가
 * ioremap 할 때는 CPU 물리 주소여야 한다. 두 값이 다른 시스템에서 이 변환을
 * 빼먹으면, 커널은 엉뚱한 주소를 매핑하고 장치는 자기에게 오지 않는
 * 트랜잭션을 기다린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  호스트 브리지 드라이버(controller/ 아래)
 *          -> pci_add_resource_offset(&resources, res, offset)   [bus.c]
 *          -> pci_create_root_bus() 가 그 목록을 루트 버스에 매단다
 *
 * 사용:  probe.c 의 __pci_read_base 가 BAR 를 읽으면 그 값은 버스 주소다
 *          -> [이 파일] pcibios_bus_to_resource() 로 CPU 주소로 바꿔
 *             pdev->resource[] 에 저장
 *        setup-res.c 가 BAR 에 값을 쓸 때는 반대로
 *          -> [이 파일] pcibios_resource_to_bus() 로 버스 주소로 바꿔서 쓴다
 *
 * 실행 컨텍스트: 제약 없음. 자료구조 조회와 산술뿐이라 잠들지도, 락을 잡지도
 * 않는다. 다만 pci_get_host_bridge_device() 는 참조 카운트를 올리므로
 * 반드시 pci_put_host_bridge_device() 와 짝을 맞춰야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(BAR 해석), setup-res.c(BAR 기록), setup-bus.c(브리지 윈도우 계산),
 *   그리고 각 호스트 브리지 드라이버.
 * 아래쪽: 없다. 이 파일은 최하위에 가깝고, 버스 트리 자료구조만 읽는다.
 * 공유 상태: struct pci_host_bridge 의 windows 목록(struct resource_entry 로,
 *   각 항목이 자원과 offset 을 함께 갖는다), 그리고 struct pci_bus 의 parent/bridge.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 하지만 NVMe 가 쓰는 BAR0 주소의 정확성이 이 변환에 달려 있다.
 *
 * NVMe 드라이버는 pci_resource_start(pdev, 0) 로 BAR0 의 시작 주소를 얻어
 * 그대로 ioremap 한다. pci_resource_start 가 돌려주는 것은 pdev->resource[0]
 * 이고, 그 값은 열거 단계에서 이 파일의 pcibios_bus_to_resource() 를 거쳐
 * 이미 CPU 물리 주소로 변환돼 있다. 즉 NVMe 드라이버가 주소 변환을 전혀
 * 신경 쓰지 않아도 되는 이유가 여기 있다 — PCI 코어가 저장 시점에 한 번
 * 변환해 두었기 때문이다.
 *
 * P2PDMA(peer-to-peer DMA)에서는 이야기가 달라진다. NVMe 의 CMB(Controller
 * Memory Buffer)를 다른 장치가 직접 읽고 쓰려면, 그 장치에게는 CPU 주소가
 * 아니라 PCI 버스 주소를 줘야 한다. 그래서 p2pdma.c 가 반대 방향 변환인
 * pcibios_resource_to_bus() 를 쓴다.
 *
 * (기존 주석은 "pci_resource_start() 는 버스 주소" 라고 적었으나 반대다 —
 *  pdev->resource[] 는 CPU 주소 기준이고, 버스 주소는 BAR 레지스터에
 *  들어가는 값이다. 또 NVMe 경로로 pci_request_regions 와 pci_iomap 을
 *  들었으나 drivers/nvme/ 에 두 호출 모두 0건이다. 정정했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * find_pci_root_bus()          : 버스 트리를 위로 올라가 루트 버스를 찾는다.
 * pci_find_host_bridge()       : 위 결과에서 struct pci_host_bridge 를 꺼낸다.
 * pci_get_host_bridge_device() : 호스트 브리지의 struct device 를 참조를 올려
 *                                돌려준다. IOMMU 코드가 주로 쓴다.
 * pci_put_host_bridge_device() : 위의 짝. 참조를 내린다.
 * pci_set_host_bridge_release(): 브리지가 해제될 때 부를 콜백을 등록.
 *                                브리지 드라이버가 자기 자원을 정리할 자리다.
 * pcibios_resource_to_bus()    : CPU 물리 주소 -> PCI 버스 주소.
 *                                BAR 에 값을 써 넣기 직전에 쓴다.
 * pcibios_bus_to_resource()    : PCI 버스 주소 -> CPU 물리 주소.
 *                                BAR 에서 읽은 값을 해석할 때 쓴다.
 * region_contains()            : 한 구간이 다른 구간을 완전히 포함하는지 검사.
 *                                어느 윈도우에 속하는지 고르는 데 쓰는 보조 함수.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>

#include "pci.h"

static struct pci_bus *find_pci_root_bus(struct pci_bus *bus)
{
	while (bus->parent)
		bus = bus->parent;

	return bus;
}

struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *bus)
{
	struct pci_bus *root_bus = find_pci_root_bus(bus);

	return to_pci_host_bridge(root_bus->bridge);
}
EXPORT_SYMBOL_GPL(pci_find_host_bridge);

struct device *pci_get_host_bridge_device(struct pci_dev *dev)
{
	struct pci_bus *root_bus = find_pci_root_bus(dev->bus);
	struct device *bridge = root_bus->bridge;

	kobject_get(&bridge->kobj);
	return bridge;
}
EXPORT_SYMBOL_GPL(pci_get_host_bridge_device);

void  pci_put_host_bridge_device(struct device *dev)
{
	kobject_put(&dev->kobj);
}

void pci_set_host_bridge_release(struct pci_host_bridge *bridge,
				 void (*release_fn)(struct pci_host_bridge *),
				 void *release_data)
{
	bridge->release_fn = release_fn;
	bridge->release_data = release_data;
}
EXPORT_SYMBOL_GPL(pci_set_host_bridge_release);

void pcibios_resource_to_bus(struct pci_bus *bus, struct pci_bus_region *region,
			     struct resource *res)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct resource_entry *window;
	resource_size_t offset = 0;

	resource_list_for_each_entry(window, &bridge->windows) {
		if (resource_contains(window->res, res)) {
			offset = window->offset;
			break;
		}
	}

	region->start = res->start - offset;
	region->end = res->end - offset;
}
EXPORT_SYMBOL(pcibios_resource_to_bus);

static bool region_contains(struct pci_bus_region *region1,
			    struct pci_bus_region *region2)
{
	return region1->start <= region2->start && region1->end >= region2->end;
}

void pcibios_bus_to_resource(struct pci_bus *bus, struct resource *res,
			     struct pci_bus_region *region)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct resource_entry *window;
	resource_size_t offset = 0;

	resource_list_for_each_entry(window, &bridge->windows) {
		struct pci_bus_region bus_region;

		if (resource_type(res) != resource_type(window->res))
			continue;

		bus_region.start = window->res->start - window->offset;
		bus_region.end = window->res->end - window->offset;

		if (region_contains(&bus_region, region)) {
			offset = window->offset;
			break;
		}
	}

	res->start = region->start + offset;
	res->end = region->end + offset;
}
EXPORT_SYMBOL(pcibios_bus_to_resource);
