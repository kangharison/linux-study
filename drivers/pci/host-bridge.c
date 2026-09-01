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

/* [한국어] 커널 공통 정의. */
#include <linux/kernel.h>
/* [한국어] PCI 코어 공개 API — struct pci_bus, pci_host_bridge, pci_bus_region,
 * to_pci_host_bridge(), resource_list_for_each_entry(). */
#include <linux/pci.h>
/* [한국어] EXPORT_SYMBOL_GPL 과 EXPORT_SYMBOL 매크로. 이 파일의 함수 대부분이
 * 다른 모듈에 공개된다. */
#include <linux/module.h>

/* [한국어] PCI 서브시스템 내부 헤더. */
#include "pci.h"

/* [한국어]
 * find_pci_root_bus - 어느 버스에서 시작하든 그 트리의 루트 버스를 찾는다
 *
 * @bus: 시작 버스.
 * @return: 루트 버스.
 *
 * PCI 버스는 트리 구조이고 루트 버스만이 parent 가 NULL 이다. 그 성질을
 * 이용해 부모를 따라 올라가기만 하면 된다.
 *
 * 이 파일의 다른 함수들이 호스트 브리지의 자원 창 목록을 봐야 하는데,
 * 그 목록은 루트 버스의 브리지에만 있다. 그래서 어떤 버스를 받든 먼저
 * 루트로 올라가는 이 헬퍼가 필요하다.
 *
 * 실행 컨텍스트: 어디서든 안전하다. 포인터 순회가 전부다.
 *
 * 에러 경로: 없다. 루프가 반드시 끝나는 것은 트리에 순환이 없다는 전제 덕분이다.
 *
 * 호출 체인:
 *   pci_find_host_bridge() / pci_get_host_bridge_device() → [이 함수]
 */
static struct pci_bus *find_pci_root_bus(struct pci_bus *bus)
{
	/* [한국어] 부모가 없어질 때까지 거슬러 올라간다 — 루트 버스만이 부모가 NULL 이다. */
	while (bus->parent)
		/* [한국어] 한 단계 올라간다. */
		bus = bus->parent;

	/* [한국어] 루트 버스를 돌려준다. */
	return bus;
}

/* [한국어]
 * pci_find_host_bridge - 버스가 속한 호스트 브리지 객체를 얻는다
 *
 * @bus: 임의의 PCI 버스.
 * @return: 그 트리의 struct pci_host_bridge.
 *
 * 루트 버스로 올라간 뒤, 그 버스의 bridge 필드(struct device)를 감싸고 있는
 * struct pci_host_bridge 로 되돌린다. to_pci_host_bridge() 가 container_of
 * 기반 매크로다.
 *
 * 참조 카운트를 올리지 않는 것이 pci_get_host_bridge_device() 와 다른 점이다.
 * 호출자가 이미 버스를 붙잡고 있으므로 그 브리지도 살아 있다는 전제이며,
 * 그래서 짝이 되는 put 함수가 없다.
 *
 * 실행 컨텍스트: 어디서든 안전하다.
 *
 * 에러 경로: 없다. 모든 PCI 버스는 반드시 어떤 호스트 브리지에 속한다.
 *
 * 호출 체인:
 *   pcibios_resource_to_bus() / pcibios_bus_to_resource() 와
 *   여러 드라이버 → [이 함수] → find_pci_root_bus() → to_pci_host_bridge()
 */
struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *bus)
{
	/* [한국어] 어느 버스에서 시작하든 루트 버스를 찾는다. */
	struct pci_bus *root_bus = find_pci_root_bus(bus);

	/* [한국어] 루트 버스의 bridge 필드는 struct device 인데, 그것을 감싸고 있는
	 * struct pci_host_bridge 로 되돌린다. container_of 기반 매크로다. */
	return to_pci_host_bridge(root_bus->bridge);
}
EXPORT_SYMBOL_GPL(pci_find_host_bridge);

/* [한국어]
 * pci_get_host_bridge_device - 호스트 브리지 device 의 참조를 잡아 돌려준다
 *
 * @dev: 임의의 PCI 장치.
 * @return: 참조가 잡힌 브리지 struct device.
 *
 * pci_find_host_bridge() 와 달리 kobject 참조를 올린다. 브리지 device 를
 * 장치 수명보다 오래 들고 있어야 하는 호출자를 위한 것이며, 다 쓴 뒤 반드시
 * pci_put_host_bridge_device() 로 짝을 맞춰야 한다.
 *
 * 참조를 놓지 않으면 브리지 device 가 영영 해제되지 않아, 컨트롤러 드라이버를
 * 언로드해도 sysfs 항목이 남는다.
 *
 * 실행 컨텍스트: 어디서든 안전하다. kobject_get 은 원자적 증가다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   브리지 정보를 오래 들고 있어야 하는 드라이버 → [이 함수]
 *     → find_pci_root_bus() → kobject_get()
 */
struct device *pci_get_host_bridge_device(struct pci_dev *dev)
{
	/* [한국어] 장치가 속한 버스에서 루트 버스를 찾는다. */
	struct pci_bus *root_bus = find_pci_root_bus(dev->bus);
	/* [한국어] 그 루트 버스의 브리지 device. */
	struct device *bridge = root_bus->bridge;

	/* [한국어] 참조 카운트를 올린다. 이 함수가 "get" 인 이유이며, 호출자는 다 쓴 뒤
	 * 반드시 pci_put_host_bridge_device() 로 짝을 맞춰야 한다.
	 * 그렇게 하지 않으면 브리지 device 가 영영 해제되지 않는다. */
	kobject_get(&bridge->kobj);
	/* [한국어] 참조를 잡은 채로 포인터를 돌려준다. */
	return bridge;
}
EXPORT_SYMBOL_GPL(pci_get_host_bridge_device);

/* [한국어]
 * pci_put_host_bridge_device - 잡아 둔 브리지 device 참조를 놓는다
 *
 * @dev: pci_get_host_bridge_device() 가 돌려준 device.
 *
 * get 의 짝이다. 마지막 참조라면 여기서 실제 해제가 일어난다.
 *
 * [상류 코드 관찰, 수정하지 않음] get 쪽은 EXPORT_SYMBOL_GPL 인데 이 함수는
 * export 되지 않는다. 모듈이 get 을 쓰면 짝을 맞출 방법이 없는 셈이다.
 *
 * 실행 컨텍스트: 어디서든 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_get_host_bridge_device() 를 쓴 코드 → [이 함수] → kobject_put()
 */
void  pci_put_host_bridge_device(struct device *dev)
{
	/* [한국어] get 이 올린 참조를 내린다. 마지막 참조라면 여기서 실제로 해제된다.
	 * [상류 코드 관찰] get 쪽과 달리 이 함수는 EXPORT 되지 않는다 —
	 * get 을 쓴 모듈이 짝을 맞출 수 없다는 뜻인데, 실제로 이 트리 안에서
	 * get 을 부르는 곳이 있는지는 확인하지 않았다. */
	kobject_put(&dev->kobj);
}

/* [한국어]
 * pci_set_host_bridge_release - 브리지 해제 시 불릴 콜백을 등록한다
 *
 * @bridge: 대상 호스트 브리지.
 * @release_fn: 브리지가 해제될 때 불릴 함수.
 * @release_data: 그 함수가 받을 문맥.
 *
 * 호스트 컨트롤러 드라이버가 자기 자원을 브리지 수명에 묶고 싶을 때 쓴다.
 * 브리지는 PCI 코어가 관리하는 객체라 드라이버보다 오래 살 수 있는데,
 * 그때 드라이버가 할당한 것을 언제 해제할지가 문제가 된다. 이 콜백이
 * 그 시점을 알려 준다.
 *
 * 두 필드에 대입하는 것이 전부이므로 함수라기보다 설정자에 가깝다.
 * 그래도 함수로 두는 것은 struct pci_host_bridge 의 필드 배치를 드라이버에
 * 노출하지 않기 위해서다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   호스트 컨트롤러 드라이버의 probe → [이 함수]
 *   이후 브리지 해제 시: pci_release_host_bridge_dev() → release_fn(bridge)
 */
void pci_set_host_bridge_release(struct pci_host_bridge *bridge,
				 void (*release_fn)(struct pci_host_bridge *),
				 void *release_data)
{
	/* [한국어] 브리지가 해제될 때 불릴 콜백을 등록한다. 호스트 컨트롤러 드라이버가
	 * 자기 자원을 브리지 수명에 묶고 싶을 때 쓴다. */
	bridge->release_fn = release_fn;
	/* [한국어] 그 콜백이 받을 문맥. 드라이버가 자기 private 구조체를 넣어 둔다. */
	bridge->release_data = release_data;
}
EXPORT_SYMBOL_GPL(pci_set_host_bridge_release);

/* [한국어]
 * pcibios_resource_to_bus - CPU 물리 주소 범위를 PCI 버스 주소로 변환한다
 *
 * @bus: 기준이 될 PCI 버스(그 트리의 호스트 브리지를 찾는 데 쓴다).
 * @region: 결과를 담을 버스 주소 범위.
 * @res: 변환할 CPU 주소 범위.
 *
 * 왜 변환이 필요한가: 많은 아키텍처에서 CPU 가 보는 물리 주소와 PCI 장치가
 * 보는 버스 주소가 다르다. BAR 에 쓸 값은 반드시 버스 주소여야 하고,
 * 드라이버가 ioremap 할 주소는 CPU 주소여야 한다. 그 둘을 오가는 것이
 * 이 함수와 짝인 pcibios_bus_to_resource() 다.
 *
 * 변환 방법은 단순하다. 호스트 브리지의 자원 창 목록에는 창마다
 * "CPU 주소 - 버스 주소" 의 차이가 offset 으로 기록되어 있으므로,
 * 요청한 범위를 통째로 포함하는 창을 찾아 그 오프셋을 빼면 된다.
 * 부분적으로 겹치는 창은 답이 될 수 없어 resource_contains() 로 완전 포함만 본다.
 *
 * 맞는 창을 찾지 못하면 offset 이 0 으로 남아 CPU 주소와 버스 주소가 같다고
 * 본다. 두 주소가 같은 플랫폼(x86 등)에서는 언제나 그렇게 되므로,
 * 그런 곳의 코드는 이 변환의 존재를 의식하지 않는다.
 *
 * 실행 컨텍스트: 자원 배정과 BAR 프로그래밍 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 창을 못 찾아도 오류가 아니라 항등 변환이 된다.
 *
 * 호출 체인:
 *   pci_update_resource() / 컨트롤러 드라이버의 BAR 설정 → [이 함수]
 *     → pci_find_host_bridge() → resource_list_for_each_entry()
 */
void pcibios_resource_to_bus(struct pci_bus *bus, struct pci_bus_region *region,
			     struct resource *res)
{
	/* [한국어] 이 버스가 속한 호스트 브리지를 찾는다. 창 목록이 거기 있기 때문이다. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	/* [한국어] 순회할 자원 창. */
	struct resource_entry *window;
	/* [한국어] 찾은 창의 오프셋. 못 찾으면 0 이 되어 CPU 주소와 버스 주소가 같다고 본다. */
	resource_size_t offset = 0;

	/* [한국어] 브리지가 가진 자원 창들을 순회한다. 창마다 "CPU 주소 - 버스 주소"의
	 * 차이가 offset 으로 기록되어 있다. */
	resource_list_for_each_entry(window, &bridge->windows) {
		/* [한국어] 이 자원을 통째로 포함하는 창을 찾는다. 부분적으로 겹치는 창은 답이 될 수 없다. */
		if (resource_contains(window->res, res)) {
			/* [한국어] 그 창의 오프셋을 채택하고, */
			offset = window->offset;
			/* [한국어] 더 볼 필요가 없다. */
			break;
		}
	}

	/* [한국어] CPU 주소에서 오프셋을 빼 버스 주소를 만든다. 두 주소가 같은 플랫폼에서는
	 * offset 이 0 이라 이 계산이 항등식이 되고, 그래서 대부분의 x86 코드가
	 * 이 변환의 존재를 의식하지 않는다. */
	region->start = res->start - offset;
	/* [한국어] 끝 주소도 같은 방식으로 변환한다. */
	region->end = res->end - offset;
}
EXPORT_SYMBOL(pcibios_resource_to_bus);

/* [한국어]
 * region_contains - 버스 주소 영역 하나가 다른 하나를 완전히 포함하는지 본다
 *
 * @region1: 바깥 영역 후보.
 * @region2: 안쪽 영역 후보.
 * @return: region1 이 region2 를 완전히 포함하면 참.
 *
 * 커널에는 struct resource 용 resource_contains() 가 있지만
 * struct pci_bus_region 용은 없다. 그래서 이 파일이 같은 판정을 직접 정의한다.
 *
 * 시작이 같거나 앞서고 끝이 같거나 뒤여야 한다 — 부분적으로 겹치는 것은
 * 포함이 아니다.
 *
 * 실행 컨텍스트: 어디서든 안전한 순수 비교.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcibios_bus_to_resource() → [이 함수]
 */
static bool region_contains(struct pci_bus_region *region1,
			    struct pci_bus_region *region2)
{
	/* [한국어] region1 이 region2 를 완전히 포함하는지 본다. 시작이 같거나 앞서고
	 * 끝이 같거나 뒤여야 한다. resource 용 resource_contains() 의
	 * bus_region 판이 커널에 없어 이 파일이 직접 정의한다. */
	return region1->start <= region2->start && region1->end >= region2->end;
}

/* [한국어]
 * pcibios_bus_to_resource - PCI 버스 주소 범위를 CPU 물리 주소로 변환한다
 *
 * @bus: 기준이 될 PCI 버스.
 * @res: 결과를 담을 CPU 주소 범위.
 * @region: 변환할 버스 주소 범위.
 *
 * pcibios_resource_to_bus() 의 역방향이다. BAR 에서 읽은 값(버스 주소)을
 * 드라이버가 ioremap 할 수 있는 CPU 주소로 바꿀 때 쓴다.
 *
 * 역방향에 없는 검사가 하나 있다 — 자원 종류(메모리 / I/O)가 같은 창만
 * 비교한다. CPU 주소 공간에서는 메모리와 I/O 가 서로 다른 주소를 갖지만,
 * 버스 주소 공간에서는 둘이 같은 숫자를 가질 수 있어 종류를 먼저 걸러야
 * 엉뚱한 창을 고르지 않는다.
 *
 * 또 창을 버스 주소로 바꿔 비교해야 하므로, 창마다 bus_region 을 계산한 뒤
 * region_contains() 로 판정한다. 역방향이 resource_contains() 를 그대로
 * 쓸 수 있었던 것과 다른 점이다.
 *
 * 실행 컨텍스트: BAR 읽기 후 자원 등록 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 창을 못 찾으면 offset 0 으로 항등 변환이 된다.
 *
 * 호출 체인:
 *   pci_read_bases() / 컨트롤러 드라이버의 자원 등록 → [이 함수]
 *     → pci_find_host_bridge() → region_contains()
 */
void pcibios_bus_to_resource(struct pci_bus *bus, struct resource *res,
			     struct pci_bus_region *region)
{
	/* [한국어] 호스트 브리지를 찾는다. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	/* [한국어] 순회할 자원 창. */
	struct resource_entry *window;
	/* [한국어] 찾은 창의 오프셋. */
	resource_size_t offset = 0;

	/* [한국어] 창들을 순회한다. */
	resource_list_for_each_entry(window, &bridge->windows) {
		/* [한국어] 이 창을 버스 주소로 표현한 값을 담을 임시 변수. */
		struct pci_bus_region bus_region;

		/* [한국어] 자원 종류(메모리 / I/O)가 다르면 비교할 이유가 없다.
		 * 역방향(pcibios_resource_to_bus)에는 이 검사가 없는데, 그쪽은 CPU 주소
		 * 공간에서 비교하므로 종류가 달라도 주소가 겹치지 않기 때문이다.
		 * 반면 버스 주소 공간에서는 메모리와 I/O 가 같은 숫자를 가질 수 있어
		 * 종류를 먼저 걸러야 한다. */
		if (resource_type(res) != resource_type(window->res))
			/* [한국어] 다른 종류는 건너뛴다. */
			continue;

		/* [한국어] 창의 CPU 주소를 버스 주소로 바꾼다. */
		bus_region.start = window->res->start - window->offset;
		/* [한국어] 끝 주소도 마찬가지. */
		bus_region.end = window->res->end - window->offset;

		/* [한국어] 그 버스 주소 범위가 요청한 영역을 포함하면, */
		if (region_contains(&bus_region, region)) {
			/* [한국어] 그 창의 오프셋을 채택하고, */
			offset = window->offset;
			/* [한국어] 멈춘다. */
			break;
		}
	}

	/* [한국어] 버스 주소에 오프셋을 더해 CPU 주소를 만든다. to_bus 의 정확한 역연산이다. */
	res->start = region->start + offset;
	/* [한국어] 끝 주소도 같은 방식으로. */
	res->end = region->end + offset;
}
EXPORT_SYMBOL(pcibios_bus_to_resource);
