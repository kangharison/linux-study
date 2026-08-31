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

#include <linux/kernel.h> /* NVMe: 커널 기본 자료형 및 매크로 정의 포함. */
#include <linux/pci.h> /* NVMe: PCI 버스, 장치, host bridge 관련 구조체 및 함수 선언 포함. */
#include <linux/module.h> /* NVMe: 모듈 로드/언로드 및 EXPORT 매크로 정의 포함. */

#include "pci.h" /* NVMe: PCI 핵심 헤더 포함. */

/*
 * find_pci_root_bus:
 *   주어진 pci_bus 구조체를 따라 최상위 root bus를 찾는다.
 *   NVMe 장치가 연결된 하위 bus에서 시작해 Root Complex 아래 root bus를
 *   얻는 데 사용된다. 이 root bus가 host bridge를 가리킨다.
 */
static struct pci_bus *find_pci_root_bus(struct pci_bus *bus) /* NVMe: 주어진 bus를 따라 Root Complex 아래 root bus를 찾는다. */
{	/* NVMe: find_pci_root_bus 함수 본문 시작. */
	while (bus->parent) /* NVMe: 현재 bus의 상위 bus가 있으면 계속 따라 올라간다. */
		bus = bus->parent; /* NVMe: parent 포인터를 타고 root 방향으로 이동. */

	return bus; /* NVMe: parent가 NULL인 최상위 root bus 반환. */
}	/* NVMe: find_pci_root_bus 함수 종료. */

/*
 * pci_find_host_bridge:
 *   임의의 PCI bus가 속한 pci_host_bridge 구조체를 반환한다.
 *   NVMe 장치의 pci_dev->bus를 넘기면 해당 NVMe가 연결된 host bridge의
 *   메타정보(예: domain_nr, windows, MSI/IRQ 도메인)를 얻을 수 있다.
 */
struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *bus) /* NVMe: NVMe 장치가 속한 PCI host bridge 구조체를 반환한다. */
{	/* NVMe: pci_find_host_bridge 함수 본문 시작. */
	struct pci_bus *root_bus = find_pci_root_bus(bus); /* NVMe: NVMe bus의 root bus 획득. */

	return to_pci_host_bridge(root_bus->bridge); /* NVMe: root bus의 bridge 객체를 host bridge로 변환하여 반환. */
}	/* NVMe: pci_find_host_bridge 함수 종료. */
EXPORT_SYMBOL_GPL(pci_find_host_bridge); /* NVMe: NVMe 장치가 속한 host bridge 조회 함수를 외부 모듈에 노출. */

/*
 * pci_get_host_bridge_device:
 *   NVMe 장치가 연결된 host bridge의 struct device 참조 카운트를 증가시키고
 *   반환한다. host bridge의 전원/라이프사이클 관리에 사용된다.
 */
struct device *pci_get_host_bridge_device(struct pci_dev *dev) /* NVMe: NVMe 장치가 연결된 host bridge의 struct device 참조를 획득한다. */
{	/* NVMe: pci_get_host_bridge_device 함수 본문 시작. */
	struct pci_bus *root_bus = find_pci_root_bus(dev->bus); /* NVMe: NVMe 장치가 속한 bus tree의 root bus 획득. */
	struct device *bridge = root_bus->bridge; /* NVMe: root bus에 매핑된 bridge device 획득. */

	kobject_get(&bridge->kobj); /* NVMe: bridge device의 kobject 참조 카운트 증가(사용 중 유지). */
	return bridge; /* NVMe: 참조 증가된 host bridge device 반환. */
}	/* NVMe: pci_get_host_bridge_device 함수 종료. */
EXPORT_SYMBOL_GPL(pci_get_host_bridge_device); /* NVMe: host bridge device 참조 획득 함수를 외부 모듈에 노출. */

/*
 * pci_put_host_bridge_device:
 *   pci_get_host_bridge_device()로 획득한 host bridge device의 참조를
 *   해제한다. NVMe 드라이버가 host bridge 관련 리소스 사용 후 반납할 때
 *   호출된다.
 */
void  pci_put_host_bridge_device(struct device *dev) /* NVMe: host bridge device의 참조 카운트를 감소시킨다. */
{	/* NVMe: pci_put_host_bridge_device 함수 본문 시작. */
	kobject_put(&dev->kobj); /* NVMe: host bridge device의 kobject 참조 카운트 감소. */
}	/* NVMe: pci_put_host_bridge_device 함수 종료. */

/*
 * pci_set_host_bridge_release:
 *   host bridge가 해제될 때 호출될 release 콜백과 데이터를 등록한다.
 *   NVMe 장치가 연결된 host bridge의 teardown 시 필요한 정리 작업을
 *   platform/driver specific하게 수행할 수 있게 한다.
 */
void pci_set_host_bridge_release(struct pci_host_bridge *bridge, /* NVMe: host bridge 해제 시 호출할 release 콜백과 데이터를 등록한다. */
				 void (*release_fn)(struct pci_host_bridge *), /* NVMe: host bridge 해제 시 호출될 콜백 함수 포인터 매개변수. */
				 void *release_data) /* NVMe: release 콜백에 전달할 platform 특화 데이터 매개변수. */
{	/* NVMe: pci_set_host_bridge_release 함수 본문 시작. */
	bridge->release_fn = release_fn; /* NVMe: host bridge 해제 시 호출할 함수 포인터 저장. */
	bridge->release_data = release_data; /* NVMe: release 콜백에 전달할 platform data 저장. */
}	/* NVMe: pci_set_host_bridge_release 함수 종료. */
EXPORT_SYMBOL_GPL(pci_set_host_bridge_release); /* NVMe: host bridge release 콜백 등록 함수를 외부 모듈에 노출. */

/*
 * pcibios_resource_to_bus:
 *   CPU 물리 주소(resource)를 PCI bus 주소 공간(bus_region)으로 변환한다.
 *   NVMe BAR가 가리키는 CPU 물리 주소를 bus 주소로 환산할 때 사용되며,
 *   P2PDMA, ATS, IOMMU 등에서 중요하다.
 */
void pcibios_resource_to_bus(struct pci_bus *bus, struct pci_bus_region *region, /* NVMe: CPU 물리 주소를 PCI bus 주소로 변환한다. */
			     struct resource *res) /* NVMe: 변환할 NVMe BAR 등 CPU 물리 주소 리소스 매개변수. */
{	/* NVMe: pcibios_resource_to_bus 함수 본문 시작. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus); /* NVMe: 해당 bus의 host bridge 획득. */
	struct resource_entry *window; /* NVMe: host bridge의 address window를 순회할 포인터. */
	resource_size_t offset = 0; /* NVMe: CPU 주소와 bus 주소 간 오프셋(0으로 초기화). */

	resource_list_for_each_entry(window, &bridge->windows) { /* NVMe: host bridge가 노출한 모든 address window를 순회. */
		if (resource_contains(window->res, res)) { /* NVMe: NVMe BAR 리소스가 현재 window 안에 포함되는지 확인. */
			offset = window->offset; /* NVMe: 포함되면 해당 window의 CPU↔bus 오프셋 기록. */
			break; /* NVMe: 적절한 window를 찾았으므로 순회 종료. */
		}	/* NVMe: window 포함 검사 if 블록 종료. */
	}	/* NVMe: host bridge address window 순회 for 블록 종료. */

	region->start = res->start - offset; /* NVMe: CPU 시작 주소에서 오프셋을 빼 bus 시작 주소 산출. */
	region->end = res->end - offset; /* NVMe: CPU 끝 주소에서 오프셋을 빼 bus 끝 주소 산출. */
}	/* NVMe: pcibios_resource_to_bus 함수 종료. */
EXPORT_SYMBOL(pcibios_resource_to_bus); /* NVMe: CPU 주소를 PCI bus 주소로 변환하는 함수를 외부 모듈에 노출. */

/*
 * region_contains:
 *   bus_region1이 bus_region2를 완전히 포함하는지 검사한다.
 *   NVMe BAR가 특정 host bridge window 안에 들어오는지 판단할 때 쓰인다.
 */
static bool region_contains(struct pci_bus_region *region1, /* NVMe: region1이 region2를 완전히 포함하는지 검사한다. */
			    struct pci_bus_region *region2) /* NVMe: 포함 여부를 검사할 두 번째 bus region 매개변수. */
{	/* NVMe: region_contains 함수 본문 시작. */
	return region1->start <= region2->start && region1->end >= region2->end; /* NVMe: region2의 시작/끝이 모두 region1 안에 있으면 true. */
}	/* NVMe: region_contains 함수 종료. */

/*
 * pcibios_bus_to_resource:
 *   PCI bus 주소(region)를 CPU 물리 주소(resource)로 변환한다.
 *   NVMe driver가 pci_resource_start()로 얻은 bus 주소를 ioremap()이나
 *   DMA 주소 설정에 사용하기 위해 CPU 물리 주소로 환산할 때 사용된다.
 */
void pcibios_bus_to_resource(struct pci_bus *bus, struct resource *res, /* NVMe: PCI bus 주소를 CPU 물리 주소로 변환한다. */
			     struct pci_bus_region *region) /* NVMe: 변환할 PCI bus 주소 region 매개변수. */
{	/* NVMe: pcibios_bus_to_resource 함수 본문 시작. */
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
		}	/* NVMe: region_contains 검사 if 블록 종료. */
	}	/* NVMe: host bridge window 순회 for 블록 종료. */

	res->start = region->start + offset; /* NVMe: bus 시작 주소에 오프셋을 더해 CPU 물리 시작 주소 산출. */
	res->end = region->end + offset; /* NVMe: bus 끝 주소에 오프셋을 더해 CPU 물리 끝 주소 산출. */
}	/* NVMe: pcibios_bus_to_resource 함수 종료. */
EXPORT_SYMBOL(pcibios_bus_to_resource); /* NVMe: PCI bus 주소를 CPU 물리 주소로 변환하는 함수를 외부 모듈에 노출. */
