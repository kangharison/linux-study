// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI <-> OF mapping helpers
 *
 * Copyright 2011 IBM Corp.
 */

/*
 * [한국어 설명] PCI 와 DeviceTree 를 잇는 헬퍼 (of.c)
 *
 * === 파일의 역할 ===
 * DeviceTree(DT)는 ACPI 가 없는 시스템 — 주로 ARM/RISC-V 임베디드와
 * SoC — 이 하드웨어를 기술하는 방식이다. 이 파일은 DT 노드에 적힌
 * 정보를 PCI 자료구조로 옮긴다.
 *
 * 옮기는 것이 여럿이다.
 *   - 주소 범위(ranges 속성) — 호스트 브리지가 어떤 CPU 주소 구간을
 *     어떤 PCI 버스 주소로 매핑하는지. CPU 주소와 버스 주소가 다른
 *     플랫폼이 많아 offset 정보가 함께 온다.
 *   - 버스 번호 범위(bus-range 속성)
 *   - 인터럽트 매핑(interrupt-map) — INTx 핀이 어느 인터럽트 컨트롤러의
 *     몇 번에 연결됐는가. ACPI 의 _PRT 에 대응한다.
 *   - MSI 컨트롤러 연결(msi-parent) — 이 장치의 MSI 를 누가 담당하는가.
 *   - 도메인 번호(linux,pci-domain) — pci.c 의 도메인 배정이 이것을 읽는다.
 *   - 장치별 속성 — 최대 링크 속도(max-link-speed), 슬롯 전원 등.
 *
 * DT 노드를 PCI 장치에 연결하는 방식도 다룬다. DT 는 장치를 주소로
 * 표현하므로("pci@1,0"), 열거로 발견한 pci_dev 와 그 노드를 짝지어야
 * 한다. of_pci_find_child_device() 가 그 일을 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호스트 브리지 드라이버(controller/ 아래)
 *   -> [이 파일] devm_of_pci_get_host_bridge_resources()
 *      DT 의 ranges 를 읽어 자원 목록을 만든다
 *   -> pci_host_probe() -> probe.c 의 열거
 *      -> [이 파일] pci_set_of_node() 로 각 장치에 DT 노드를 연결
 *      -> [이 파일] of_irq_parse_and_map_pci() 로 INTx 배정
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. DT 순회와 메모리 할당이 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: controller/ 아래의 SoC 호스트 브리지 드라이버들, probe.c.
 * 아래쪽: drivers/of/ 의 DT 파서, bus.c 의 자원 목록.
 * 옆쪽: of_property.c — 반대 방향이다. 커널이 만든 PCI 정보를 DT 속성으로
 *   내보낸다(주로 가상화 환경에서 게스트에게 넘길 때).
 * 공유 상태: struct pci_dev / pci_bus / pci_host_bridge 의 of_node 포인터.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_set_of_node() / pci_release_of_node() : pci_dev 에 DT 노드를 연결/해제.
 * of_pci_find_child_device()   : 주소로 DT 자식 노드를 찾는다.
 * of_pci_get_devfn()           : DT 의 reg 속성에서 devfn 을 뽑아낸다.
 * of_pci_parse_bus_range()     : bus-range 속성을 읽는다.
 * of_get_pci_domain_nr()       : linux,pci-domain 속성을 읽는다.
 *                                pci.c 의 도메인 배정이 이것을 쓴다.
 * of_pci_check_probe_only()    : linux,pci-probe-only 속성. 자원 재배치를
 *                                금지하고 펌웨어 배치를 그대로 쓰게 한다.
 * devm_of_pci_get_host_bridge_resources() : ranges 를 읽어 자원 목록을 만든다.
 *                                이 파일에서 가장 큰 함수다.
 * of_irq_parse_and_map_pci()   : interrupt-map 으로 INTx IRQ 를 배정한다.
 * of_pci_get_max_link_speed()  : max-link-speed 속성. 보드가 신호 품질상
 *                                낮은 속도만 보장할 때 상한을 건다.
 * of_pci_get_slot_power_limit(): 슬롯이 공급할 수 있는 전력 상한.
 * of_pci_get_equalization_presets() : 8GT/s 이상에서 쓸 링크 이퀄라이제이션
 *                                프리셋 표를 DT 에서 읽는다.
 * of_pci_supply_present()      : "xxx-supply" 속성이 하나라도 있는지.
 *                                pwrctrl 이 전원 컨트롤러를 붙일지 판단한다.
 * of_pci_make_dev_node() / of_pci_remove_node()
 *                              : CONFIG_PCI_DYNAMIC_OF_NODES 에서 열거된
 *                                장치에 DT 노드를 런타임 생성/제거한다.
 * of_irq_parse_pci()           : interrupt-map 을 따라 INTx 를 해석하는 본체.
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * 관련이 생기는 것은 ARM 서버나 SoC 에 NVMe 를 붙이는 경우다. 그런
 * 시스템에서는 PCIe 컨트롤러가 DT 로 기술되고, 이 파일이 그 정보를
 * 읽어 버스 트리를 만든다. NVMe SSD 는 그 트리 위에 열거되는
 * 장치 중 하나일 뿐이라 이 파일과 직접 얽히지 않는다.
 *
 * 다만 주소 변환은 눈여겨볼 만하다. DT 의 ranges 에 CPU 주소와 PCI 버스
 * 주소의 대응이 적혀 있고, 그것이 host-bridge.c 의
 * pcibios_resource_to_bus / pcibios_bus_to_resource 가 쓰는 offset 이 된다.
 * x86 에서는 두 주소가 같아 이 구분이 눈에 띄지 않지만, 여기서는
 * NVMe 의 BAR0 주소가 실제로 두 값을 갖는다.
 *
 */

/* [한국어] 이 파일의 모든 pr_/dev_ 로그 앞에 "PCI: OF: " 를 붙인다.
 * DT 파싱 오류는 부팅 로그에 섞여 나오므로 출처를 표시해 두어야 찾기 쉽다. */
#define pr_fmt(fmt)	"PCI: OF: " fmt

/* [한국어] cleanup.h — __free(device_node) / __free(put_device) / no_free_ptr().
 * pci_set_of_node() 가 참조 계수를 자동으로 되돌리는 데 쓴다. */
#include <linux/cleanup.h>
/* [한국어] irqdomain.h — irq_find_host(), irq_find_matching_host(),
 * irq_create_of_mapping(). MSI 도메인 찾기와 INTx 매핑에 필요하다. */
#include <linux/irqdomain.h>
/* [한국어] kernel.h — ARRAY_SIZE 등 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] pci.h — struct pci_dev / pci_bus / pci_host_bridge, PCI_SLOT/PCI_FUNC. */
#include <linux/pci.h>
/* [한국어] of.h — DT 노드 순회와 속성 읽기의 본체(of_property_read_u32 계열). */
#include <linux/of.h>
/* [한국어] of_irq.h — of_irq_parse_one() / of_irq_parse_raw(). INTx 해석의 핵심이다. */
#include <linux/of_irq.h>
/* [한국어] of_address.h — of_pci_range_parser 와 of_pci_range_to_resource().
 * ranges 속성을 struct resource 로 바꾸는 일을 담당한다. */
#include <linux/of_address.h>
/* [한국어] of_pci.h — PCI 전용 DT 헬퍼 선언. 이 트리에는 include/linux/of_pci.h 가
 * 없어 안의 상수 정의를 직접 확인하지는 못했다. */
#include <linux/of_pci.h>
/* [한국어] platform_device.h — bus_find_device_by_of_node(&platform_bus_type, ...).
 * 같은 DT 노드로 등록된 platform 장치가 있는지 확인하는 데 쓴다. */
#include <linux/platform_device.h>
/* [한국어] pci.h(로컬) — PCI 코어 내부 선언. pci_add_resource_offset(),
 * devm_request_pci_bus_resources(), pci_common_swizzle() 등이 여기 있다. */
#include "pci.h"

/* [한국어] PCI 코어가 빌드에 포함될 때만 아래 함수들을 컴파일한다.
 * #endif 는 of_pci_supply_present() 뒤에 있고, 그 아래 링크 속도/전력/프리셋
 * 헬퍼는 이 밖에 있다 — 컨트롤러 드라이버가 PCI 코어 없이도 쓰기 때문이다. */
#ifdef CONFIG_PCI
/* [한국어]
 * pci_set_of_node - 열거로 발견한 장치에 대응하는 DT 노드를 찾아 연결한다
 *
 * @dev: 방금 만들어진 pci_dev. bus 와 devfn 이 이미 채워져 있어야 한다.
 * @return: 0. 아래 설명대로 이 판에서는 다른 값을 돌려주지 않는다.
 *
 * PCI 는 버스를 훑어 장치를 발견하고, DT 는 그 장치를 주소로 미리 적어
 * 둔다. 둘을 짝지어야 DT 에만 있는 정보(전원 레일, GPIO 리셋, 링크 속도
 * 상한 등)를 드라이버가 찾을 수 있다.
 *
 * 절차:
 *   1) 부모 버스에 노드가 없으면 DT 로 기술된 트리가 아니므로 그냥 0.
 *   2) of_pci_find_child_device() 로 devfn 이 맞는 자식 노드를 찾는다.
 *   3) 그 노드로 등록된 platform_device 가 이미 있는지 본다. 있으면
 *      of_node_reused 를 세워 "이 노드는 두 device 가 공유한다" 고 표시한다.
 *      한 DT 노드가 platform 장치와 PCI 장치 양쪽으로 보이는 구성 - 예컨대
 *      SoC 내장 블록이 PCI 로도 노출되는 경우 - 에서 나온다.
 *   4) device_set_node 로 붙인다.
 *
 * cleanup.h 의 __free 를 세 군데 쓴다. node 는 __free(device_node) 로
 * 묶어 두었다가 실제로 붙일 때 no_free_ptr() 로 소유권을 넘기고, pdev 는
 * __free(put_device) 라 함수를 나갈 때 자동으로 반납된다 - 존재 확인만
 * 하고 쓰지 않기 때문이다.
 *
 * 상류 kernel-doc 은 "장치가 DT 에 있지만 disabled 이면 -ENODEV" 라고
 * 적고 있으나, 이 판의 코드에는 status 를 확인하는 경로도 -ENODEV 를
 * 돌려주는 경로도 없다. 모든 갈래가 0 이다. 코드를 고치지 않고 관찰만
 * 기록해 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 열거).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:4883 (pci_setup_device).
 *
 * 호출 체인:
 *   pci_setup_device() [probe.c] -> [이 함수]
 *     -> of_pci_find_child_device() -> bus_find_device_by_of_node()
 *     -> device_set_node()
 */
/**
 * pci_set_of_node - Find and set device's DT device_node
 * @dev: the PCI device structure to fill
 *
 * Returns 0 on success with of_node set or when no device is described in the
 * DT. Returns -ENODEV if the device is present, but disabled in the DT.
 */
int pci_set_of_node(struct pci_dev *dev)
{
	/* [한국어] 부모 버스에 DT 노드가 없으면 DT 로 기술된 트리가 아니다. */
	if (!dev->bus->dev.of_node)
		/* [한국어] 성공으로 돌려준다 — 짝지을 노드가 없을 뿐 오류가 아니다. */
		return 0;

	/* [한국어] __free(device_node) — 이 포인터가 스코프를 벗어나면 of_node_put 이 자동으로
	 * 불린다. 아래 no_free_ptr() 로 소유권을 넘기면 그 자동 해제가 취소된다. */
	struct device_node *node __free(device_node) =
		/* [한국어] 부모 노드 아래에서 devfn 이 맞는 자식을 찾는다. */
		of_pci_find_child_device(dev->bus->dev.of_node, dev->devfn);
	/* [한국어] DT 에 이 장치가 적혀 있지 않다. */
	if (!node)
		/* [한국어] 역시 성공. 열거로만 발견된 장치는 노드가 없는 것이 정상이다. */
		return 0;

	/* [한국어] __free(put_device) — 존재만 확인하고 쓰지 않으므로
	 * 함수를 나갈 때 참조가 자동 반납된다. */
	struct device *pdev __free(put_device) =
		/* [한국어] 같은 DT 노드로 이미 등록된 platform 장치가 있는지 본다. */
		bus_find_device_by_of_node(&platform_bus_type, node);
	/* [한국어] 있으면 한 노드를 두 device 가 공유하는 상황이다. */
	if (pdev)
		/* [한국어] 버스 쪽에 그 사실을 표시해 둔다. SoC 내장 블록이 PCI 로도 노출되는
		 * 구성에서 나오며, 이 표시가 없으면 노드 소유권이 꼬인다. */
		dev->bus->dev.of_node_reused = true;

	/* [한국어] no_free_ptr(node) 로 소유권을 넘기면서 device 에 노드를 붙인다.
	 * 이 시점부터 참조 반납 책임은 pci_release_of_node() 에 있다. */
	device_set_node(&dev->dev, of_fwnode_handle(no_free_ptr(node)));
	/* [한국어] 항상 0 이다. 상류 kernel-doc 은 "DT 에서 disabled 면 -ENODEV" 라고
	 * 적었지만, 이 판의 코드에는 status 를 보는 경로가 없다. 관찰만 기록한다. */
	return 0;
}

/* [한국어]
 * pci_release_of_node - 장치에 걸어 둔 DT 노드 참조를 반납한다
 *
 * @dev: 대상 PCI 장치.
 * @return: 없음.
 *
 * pci_set_of_node() 의 역이다. of_node_put 으로 참조 계수를 내리고
 * device_set_node(NULL) 로 끊는다. 노드가 없던 장치(DT 에 기술되지
 * 않은 장치)에 불러도 of_node_put(NULL) 이 무해해 안전하다.
 *
 * 이 반납이 없으면 노드가 영원히 해제되지 않는다. DT 노드는 보통 부팅
 * 내내 살아 있지만, of_changeset 으로 만든 동적 노드는 참조가 0 이 되어야
 * 사라지므로 실제로 누수가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 제거 또는 등록 실패 경로).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:5169 (등록 실패 되돌리기),
 * 5896 (pci_release_dev).
 *
 * 호출 체인:
 *   pci_release_dev() / pci_setup_device() 실패 경로 [probe.c]
 *     -> [이 함수] -> of_node_put()
 */
void pci_release_of_node(struct pci_dev *dev)
{
	/* [한국어] 참조 계수를 내린다. of_node_put(NULL) 은 무해하므로
	 * 노드가 없던 장치에 불러도 안전하다. */
	of_node_put(dev->dev.of_node);
	/* [한국어] device 에서 노드 연결을 끊는다. */
	device_set_node(&dev->dev, NULL);
}

/* [한국어]
 * pci_set_bus_of_node - PCI 버스에 대응하는 DT 노드를 연결한다
 *
 * @bus: 대상 PCI 버스.
 * @return: 없음. 노드를 못 찾으면 NULL 을 연결한다.
 *
 * 버스가 루트냐 아니냐로 갈린다.
 *   - 루트 버스(bus->self == NULL)는 위에 브리지가 없으므로 호스트 브리지
 *     쪽에서 노드를 찾아온다(pcibios_get_phb_of_node).
 *   - 하위 버스는 자기를 만들어 낸 P2P 브리지의 노드를 물려받는다. DT 에서
 *     브리지 노드가 곧 그 아래 버스를 뜻하기 때문이다.
 *
 * 물려받는 김에 "external-facing" 속성을 확인해 브리지에 표시해 둔다.
 * 이 속성은 그 아래가 사용자가 물리적으로 접근할 수 있는 슬롯이나 포트라는
 * 뜻이며, 커널은 그런 포트 아래 장치를 신뢰하지 않아 IOMMU 를 더 엄격하게
 * 건다. Thunderbolt/USB4 처럼 임의의 장치를 꽂을 수 있는 포트가 대상이다.
 * 표시를 버스가 아니라 브리지(bus->self)에 다는 것이 요령이다 - 판단
 * 주체가 "이 브리지 아래가 외부인가" 이기 때문이다.
 *
 * of_node_get 으로 참조를 올린 뒤 device_set_node 에 넘긴다. 그 참조는
 * pci_release_bus_of_node() 가 반납한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(버스 생성).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:2632(루트 버스),
 * 3019(하위 버스).
 *
 * 호출 체인:
 *   pci_register_host_bridge() / pci_add_new_bus() [probe.c] -> [이 함수]
 *     -> pcibios_get_phb_of_node() 또는 of_node_get()
 */
void pci_set_bus_of_node(struct pci_bus *bus)
{
	/* [한국어] 찾아 붙일 DT 노드. */
	struct device_node *node;

	/* [한국어] self 가 NULL 이면 위에 P2P 브리지가 없는 루트 버스다. */
	if (bus->self == NULL) {
		/* [한국어] 호스트 브리지 쪽에서 노드를 찾아온다(참조 계수가 올라간 채로 온다). */
		node = pcibios_get_phb_of_node(bus);
	/* [한국어] 하위 버스 — 자기를 만들어 낸 브리지에서 물려받는다. */
	} else {
		/* [한국어] 브리지의 노드에 참조를 걸어 가져온다. DT 에서 브리지 노드가
		 * 곧 그 아래 버스를 뜻하기 때문에 같은 노드를 공유한다. */
		node = of_node_get(bus->self->dev.of_node);
		/* [한국어] 물려받는 김에 external-facing 속성을 확인한다. */
		if (node && of_property_read_bool(node, "external-facing"))
			/* [한국어] 그 아래가 사용자가 물리적으로 꽂을 수 있는 포트라는 표시.
			 * 커널은 그런 포트 아래 장치를 신뢰하지 않아 IOMMU 를 더 엄격하게 건다.
			 * 표시를 버스가 아니라 브리지에 다는 것은 판단 주체가 브리지이기 때문이다. */
			bus->self->external_facing = true;
	}

	/* [한국어] 찾은 노드(또는 NULL)를 버스 device 에 붙인다. */
	device_set_node(&bus->dev, of_fwnode_handle(node));
}

/* [한국어]
 * pci_release_bus_of_node - 버스에 걸어 둔 DT 노드 참조를 반납한다
 *
 * @bus: 대상 PCI 버스.
 * @return: 없음.
 *
 * pci_set_bus_of_node() 의 역이다. of_node_put 으로 참조 계수를 내리고
 * device_set_node(NULL) 로 연결을 끊는다.
 *
 * 순서가 반대로 보일 수 있다 - 먼저 put 하고 나중에 NULL 로 만든다.
 * 이 사이에 다른 문맥이 끼어들면 이미 해제됐을 수도 있는 노드를 보게
 * 되지만, 버스 제거 경로는 이미 직렬화되어 있다는 전제다.
 * of_node_put(NULL) 은 무해하므로 노드가 없던 버스에 불러도 안전하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(버스 제거).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:273 —
 * 그 줄을 품은 함수는 release_pcibus_dev() 다.
 *
 * 호출 체인:
 *   release_pcibus_dev() [probe.c:273] -> [이 함수] -> of_node_put()
 */
void pci_release_bus_of_node(struct pci_bus *bus)
{
	/* [한국어] 참조 계수를 내린다. */
	of_node_put(bus->dev.of_node);
	/* [한국어] 연결을 끊는다. */
	device_set_node(&bus->dev, NULL);
}

/* [한국어]
 * pcibios_get_phb_of_node - 루트 버스에 붙일 DT 노드를 찾는다
 *
 * @bus: 루트 버스여야 한다(self 와 parent 가 모두 NULL).
 * @return: 참조 계수를 올린 DT 노드. 없으면 NULL.
 *
 * __weak 이라 아키텍처가 같은 이름으로 덮어쓸 수 있다. 이 트리 안에는
 * 덮어쓴 구현이 없다(전수 grep 확인) - 아키텍처 코드가 스파스 체크아웃에
 * 들어 있지 않아서일 수 있다.
 *
 * 기본 구현은 두 곳을 본다. bus->bridge 는 PCI 코어가 루트 버스 위에
 * 만들어 두는 중간 device 인데, 여기에 노드가 붙어 있으면 그것을 쓰고,
 * 없으면 그 부모(실제 컨트롤러 device)를 본다. 상류 주석이 적었듯 보통은
 * 뒤쪽에만 채워져 있다 - DT 로 등록된 platform_device 가 노드를 갖고,
 * 중간 device 는 PCI 코어가 만든 것이라 노드가 없기 때문이다.
 *
 * 시작 부분의 WARN_ON 은 루트 버스가 아닌 버스로 부르면 잡아낸다.
 * 하위 버스의 노드는 위쪽 P2P 브리지에서 물려받아야지 이 함수로 찾으면
 * 안 되기 때문이다.
 *
 * of_node_get 으로 참조를 올려 돌려주므로, 호출자인 pci_set_bus_of_node()
 * 가 그 참조를 버스에 넘기고 pci_release_bus_of_node() 가 반납한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_set_bus_of_node() -> [이 함수] -> of_node_get()
 */
struct device_node * __weak pcibios_get_phb_of_node(struct pci_bus *bus)
{
	/* This should only be called for PHBs */
	/* [한국어] 루트 버스가 아닌 버스로 부르면 잘못된 사용이다. 하위 버스의 노드는
	 * 위쪽 브리지에서 물려받아야지 이 함수로 찾으면 안 된다. */
	if (WARN_ON(bus->self || bus->parent))
		/* [한국어] 경고를 남기고 아무 노드도 주지 않는다. */
		return NULL;

	/*
	 * Look for a node pointer in either the intermediary device we
	 * create above the root bus or its own parent. Normally only
	 * the later is populated.
	 */
	/* [한국어] PCI 코어가 루트 버스 위에 만들어 두는 중간 device 에 노드가 있는가. */
	if (bus->bridge->of_node)
		/* [한국어] 있으면 참조를 걸어 돌려준다. */
		return of_node_get(bus->bridge->of_node);
	/* [한국어] 보통은 그 부모, 즉 DT 로 등록된 실제 컨트롤러 device 에만 노드가 있다. */
	if (bus->bridge->parent && bus->bridge->parent->of_node)
		/* [한국어] 그쪽에서 참조를 걸어 돌려준다. */
		return of_node_get(bus->bridge->parent->of_node);
	/* [한국어] 둘 다 없으면 DT 로 기술된 브리지가 아니다. */
	return NULL;
}

/* [한국어]
 * pci_host_bridge_of_msi_domain - 이 버스의 MSI 를 담당할 irq_domain 을 찾는다
 *
 * @bus: 루트 버스.
 * @return: 찾은 irq_domain, 없으면 NULL. CONFIG_IRQ_DOMAIN 이 꺼져 있으면
 *          항상 NULL 이다.
 *
 * MSI 는 결국 특정 주소로의 메모리 쓰기이고, 그 쓰기를 인터럽트로 바꿔
 * 주는 하드웨어가 MSI 컨트롤러다. DT 시스템에서는 그것이 별개 노드라
 * 누구인지 찾아야 한다.
 *
 * 세 단계로 찾는다. 앞이 실패하면 뒤로 간다.
 *   1) of_msi_get_domain() - "msi-parent" 속성이 가리키는 phandle 을 따라간다.
 *      가장 명시적인 방법이라 먼저 본다.
 *   2) irq_find_matching_host(DOMAIN_BUS_PCI_MSI) - 호스트 브리지 노드 자체에
 *      PCI MSI 도메인이 직접 등록되어 있는 경우다. 컨트롤러가 MSI 처리까지
 *      겸하는 설계에서 나온다.
 *   3) irq_find_host() - 버스 종류를 가리지 않고 그 노드의 도메인을 찾는다.
 *      마지막 보루다.
 *
 * NULL 을 돌려주면 이 버스에서는 MSI 를 쓸 수 없다는 뜻이고, 호출자 쪽에서
 * PCI_BUS_FLAGS_NO_MSI 로 이어져 msi/msi.c 의 pci_msi_supported() 가 거절한다.
 *
 * #else 가지가 NULL 인 이유는 irq_domain 자체가 없는 구성에서는 MSI 를
 * 쓸 방법이 없기 때문이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(버스 등록).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:2382.
 *
 * 호출 체인:
 *   pci_host_bridge_msi_domain() [probe.c] -> [이 함수]
 *     -> of_msi_get_domain() / irq_find_matching_host() / irq_find_host()
 */
struct irq_domain *pci_host_bridge_of_msi_domain(struct pci_bus *bus)
{
/* [한국어] irq_domain 자체가 없는 구성에서는 MSI 를 쓸 방법이 없다. */
#ifdef CONFIG_IRQ_DOMAIN
	/* [한국어] 찾은 도메인을 담을 곳. */
	struct irq_domain *d;

	/* [한국어] 버스에 DT 노드가 없으면 찾을 근거가 없다. */
	if (!bus->dev.of_node)
		/* [한국어] MSI 불가. */
		return NULL;

	/* Start looking for a phandle to an MSI controller. */
	/* [한국어] 1순위 — msi-parent 속성이 가리키는 phandle 을 따라간다.
	 * 가장 명시적인 지정 방식이라 먼저 본다. */
	d = of_msi_get_domain(&bus->dev, bus->dev.of_node, DOMAIN_BUS_PCI_MSI);
	/* [한국어] 찾았으면 */
	if (d)
		/* [한국어] 그대로 쓴다. */
		return d;

	/*
	 * If we don't have an msi-parent property, look for a domain
	 * directly attached to the host bridge.
	 */
	/* [한국어] 2순위 — 호스트 브리지 노드 자체에 PCI MSI 도메인이 등록된 경우.
	 * 컨트롤러가 MSI 처리까지 겸하는 설계에서 나온다. */
	d = irq_find_matching_host(bus->dev.of_node, DOMAIN_BUS_PCI_MSI);
	/* [한국어] 찾았으면 */
	if (d)
		/* [한국어] 그대로 쓴다. */
		return d;

	/* [한국어] 3순위 — 버스 종류를 가리지 않고 그 노드의 도메인을 찾는다. 마지막 보루다. */
	return irq_find_host(bus->dev.of_node);
/* [한국어] CONFIG_IRQ_DOMAIN 이 꺼진 구성. */
#else
	/* [한국어] 항상 NULL — MSI 를 쓸 수 없다. 호출자 쪽에서 이것이
	 * PCI_BUS_FLAGS_NO_MSI 로 이어져 msi/msi.c 의 pci_msi_supported() 가 거절한다. */
	return NULL;
/* [한국어] CONFIG_IRQ_DOMAIN 분기 끝. */
#endif
}

/* [한국어]
 * pci_host_of_has_msi_map - 이 노드에 msi-map 속성이 있는가
 *
 * @dev: 호스트 브리지 device. NULL 이거나 of_node 가 없으면 false.
 * @return: msi-map 속성이 있으면 true.
 *
 * "msi-map" 은 RID(Requester ID, 즉 버스/장치/기능 번호)를 MSI 컨트롤러가
 * 쓰는 식별자로 바꾸는 표다. ARM GIC ITS 처럼 어느 장치가 보낸 MSI 인지를
 * 식별자로 구분하는 컨트롤러에 필요하다.
 *
 * 여기서는 표를 해석하지 않고 있는지 없는지만 본다. 쓰임새가 그것이면
 * 충분하기 때문이다 - probe.c 가 자식 버스의 MSI 도메인을 부모에서
 * 물려받을지 결정할 때, msi-map 이 있으면 RID 마다 도메인이 다를 수 있어
 * 그냥 물려받으면 안 된다.
 *
 * 반환형이 bool 인데 of_get_property() 의 포인터를 그대로 돌려준다.
 * 암묵적 변환이라 NULL 이 false 가 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:2638.
 *
 * 호출 체인:
 *   pci_set_bus_msi_domain() [probe.c] -> [이 함수] -> of_get_property()
 */
bool pci_host_of_has_msi_map(struct device *dev)
{
	/* [한국어] device 도 노드도 있어야 속성을 볼 수 있다. */
	if (dev && dev->of_node)
		/* [한국어] msi-map 은 RID(버스/장치/기능 번호)를 MSI 컨트롤러의 식별자로 바꾸는 표다.
		 * 여기서는 해석하지 않고 있는지 없는지만 본다. 포인터가 bool 로 암묵 변환된다. */
		return of_get_property(dev->of_node, "msi-map", NULL);
	/* [한국어] 속성이 없거나 볼 노드가 없으면 false. */
	return false;
}

/* [한국어]
 * __of_pci_pci_compare - DT 노드 하나가 찾는 devfn 인지 판정한다
 *
 * @node: 검사할 DT 노드.
 * @data: 찾는 devfn 값. unsigned int 로 받지만 실제로는 8비트다.
 * @return: 1 이면 일치, 0 이면 불일치.
 *
 * of_pci_get_devfn() 으로 노드의 devfn 을 뽑아 비교한다. 뽑기에 실패하면
 * (reg 속성이 없는 노드 등) 불일치로 본다 - 오류를 위로 전하지 않고 0 을
 * 돌려주는 이유는 호출자가 "찾았나 못 찾았나" 만 알면 되기 때문이다.
 * DT 에는 PCI 장치가 아닌 자식 노드도 섞여 있을 수 있어 이 관용이 필요하다.
 *
 * static inline 이고 호출자가 하나뿐이다. 그래도 함수로 떼어 둔 이유는
 * of_pci_find_child_device() 가 이 비교를 두 곳(바깥 루프와 multifunc
 * 안쪽 루프)에서 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   of_pci_find_child_device() -> [이 함수] -> of_pci_get_devfn()
 */
static inline int __of_pci_pci_compare(struct device_node *node,
				       unsigned int data)
{
	/* [한국어] 노드에서 뽑아낸 devfn. */
	int devfn;

	/* [한국어] reg 속성에서 devfn 을 계산한다. */
	devfn = of_pci_get_devfn(node);
	/* [한국어] reg 가 없거나 형식이 틀린 노드 — DT 에는 PCI 장치가 아닌 자식도 섞여 있다. */
	if (devfn < 0)
		/* [한국어] 오류를 위로 전하지 않고 "불일치" 로 처리한다.
		 * 호출자는 찾았는지 못 찾았는지만 알면 되기 때문이다. */
		return 0;

	/* [한국어] 찾는 devfn 과 같으면 1. */
	return devfn == data;
}

/* [한국어]
 * of_pci_find_child_device - 부모 노드 아래에서 devfn 이 맞는 자식을 찾는다
 *
 * @parent: 검색할 부모 노드(보통 버스의 노드).
 * @devfn:  찾는 장치의 devfn.
 * @return: 찾은 노드(참조 계수가 올라간 채로). 없으면 NULL.
 *
 * DT 는 장치를 이름과 reg 로 표현하고 커널은 devfn 으로 다루므로, 그 둘을
 * 잇는 조회가 필요하다. 자식을 하나씩 훑으며 __of_pci_pci_compare() 로
 * 비교한다.
 *
 * "multifunc-device" 예외가 있다. 상류 주석대로 일부 OpenFirmware 구현이
 * 다기능 장치의 기능들을 그 이름의 가짜 부모 아래에 모아 두는데, 그러면
 * 한 단계 아래에 있어 평범한 순회로는 찾지 못한다. 그래서 그 이름의
 * 노드를 만나면 한 겹 더 들어간다.
 *
 * 참조 계수 다루기가 미묘하다. for_each_child_of_node 는 순회하며 참조를
 * 올렸다 내리지만, 반환하는 노드는 올린 채로 둔다 - 호출자가 다 쓰면
 * of_node_put 을 해야 한다는 뜻이다. 안쪽 루프에서 찾았을 때 바깥 노드를
 * of_node_put 하는 줄이 그 균형을 맞춘다.
 *
 * EXPORT_SYMBOL_GPL 이지만 이 트리 안에서는 pci_set_of_node() 말고
 * 부르는 곳이 없다(전수 grep 확인). 아키텍처 쪽 코드가 이 스파스
 * 체크아웃에 없기 때문일 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_set_of_node() -> [이 함수] -> __of_pci_pci_compare() -> of_pci_get_devfn()
 */
struct device_node *of_pci_find_child_device(struct device_node *parent,
					     unsigned int devfn)
{
	/* [한국어] node 는 바깥 루프 커서, node2 는 multifunc-device 안쪽 루프 커서. */
	struct device_node *node, *node2;

	/* [한국어] 부모의 자식들을 하나씩 훑는다. 이 매크로가 순회하며 참조를 걸었다 푼다. */
	for_each_child_of_node(parent, node) {
		/* [한국어] devfn 이 맞는가. */
		if (__of_pci_pci_compare(node, devfn))
			/* [한국어] 참조를 건 채로 돌려준다 — 호출자가 다 쓰면 of_node_put 을 해야 한다. */
			return node;
		/*
		 * Some OFs create a parent node "multifunc-device" as
		 * a fake root for all functions of a multi-function
		 * device we go down them as well.
		 */
		/* [한국어] 일부 OpenFirmware 구현은 다기능 장치의 기능들을 이 이름의 가짜 부모
		 * 아래에 모아 둔다. 그러면 한 단계 아래에 있어 위 순회로는 못 찾는다. */
		if (of_node_name_eq(node, "multifunc-device")) {
			/* [한국어] 그래서 한 겹 더 들어간다. */
			for_each_child_of_node(node, node2) {
				/* [한국어] 안쪽에서 맞는 것을 찾았으면 */
				if (__of_pci_pci_compare(node2, devfn)) {
					/* [한국어] 바깥 커서의 참조를 먼저 반납한다. 이 줄이 참조 계수의 균형을 맞춘다. */
					of_node_put(node);
					/* [한국어] 안쪽 노드를 참조를 건 채로 돌려준다. */
					return node2;
				}
			}
		}
	}
	/* [한국어] 끝까지 못 찾았다. */
	return NULL;
}
/* [한국어] 모듈에 공개. 다만 이 트리 안에서는 pci_set_of_node() 말고 부르는 곳이 없다. */
EXPORT_SYMBOL_GPL(of_pci_find_child_device);

/* [한국어]
 * of_pci_get_devfn - DT 노드의 reg 속성에서 devfn 을 뽑아낸다
 *
 * @np: 장치의 DT 노드.
 * @return: 8비트 devfn 값. reg 속성이 없거나 5워드가 아니면 음수 errno.
 *
 * PCI 바인딩에서 reg 의 첫 워드(phys.hi)는 장치의 주소를 비트 자리별로
 * 나눠 담는다. 이 트리에 include/linux/of_pci.h 가 없어 상수 정의를
 * 직접 확인하지는 못했으나, 코드가 하는 연산은 분명하다.
 *
 *   (reg[0] >> 8) & 0xff
 *
 * 8비트 오른쪽으로 밀어 아래쪽(레지스터 번호 자리)을 버리고, 0xff 로
 * 잘라 그 위(버스 번호 자리)를 버린다. 남는 8비트가 곧 devfn 이며,
 * PCI_SLOT()/PCI_FUNC() 매크로로 장치 번호와 기능 번호로 다시 나눌 수 있다.
 *
 * reg 를 5워드로 읽는 이유는 PCI 바인딩이 주소 3워드 + 크기 2워드를
 * 요구하기 때문이다. 그보다 짧으면 형식 오류로 본다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 이 트리에서 확인한 호출자: __of_pci_pci_compare()(같은 파일), 그리고
 * pci-tegra.c, pci-mvebu.c, pcie-mt7621.c, pcie-aspeed.c, pcie-mediatek.c
 * 등 DT 자식 노드로 포트를 기술하는 컨트롤러 드라이버들.
 *
 * 호출 체인:
 *   __of_pci_pci_compare() / 컨트롤러 probe -> [이 함수]
 *     -> of_property_read_u32_array()
 */
/**
 * of_pci_get_devfn() - Get device and function numbers for a device node
 * @np: device node
 *
 * Parses a standard 5-cell PCI resource and returns an 8-bit value that can
 * be passed to the PCI_SLOT() and PCI_FUNC() macros to extract the device
 * and function numbers respectively. On error a negative error code is
 * returned.
 */
int of_pci_get_devfn(struct device_node *np)
{
	/* [한국어] PCI 바인딩의 reg 는 주소 3워드 + 크기 2워드로 모두 5워드다. */
	u32 reg[5];
	/* [한국어] 읽기 결과. */
	int error;

	/* [한국어] 5워드를 통째로 읽는다. 그보다 짧으면 형식 오류로 본다. */
	error = of_property_read_u32_array(np, "reg", reg, ARRAY_SIZE(reg));
	/* [한국어] reg 가 없거나 워드 수가 모자란다. */
	if (error)
		/* [한국어] errno 를 그대로 전한다. */
		return error;

	/* [한국어] reg[0](phys.hi)에서 8비트 밀어 아래쪽 레지스터 번호 자리를 버리고,
	 * 0xff 로 잘라 그 위 버스 번호 자리를 버린다. 남는 8비트가 devfn 이다.
	 * PCI_SLOT()/PCI_FUNC() 로 장치 번호와 기능 번호로 다시 나눌 수 있다. */
	return (reg[0] >> 8) & 0xff;
}
/* [한국어] 모듈에 공개. 여러 컨트롤러 드라이버가 DT 자식 노드에서 포트의 devfn 을
 * 뽑는 데 쓴다(pci-tegra.c, pci-mvebu.c, pcie-mt7621.c, pcie-aspeed.c 등). */
EXPORT_SYMBOL_GPL(of_pci_get_devfn);

/* [한국어]
 * of_pci_parse_bus_range - bus-range 속성을 struct resource 로 바꾼다
 *
 * @node: 호스트 브리지의 DT 노드.
 * @res:  결과를 담을 자원 구조체.
 * @return: 0 성공, 속성이 없거나 형식이 틀리면 음수 errno.
 *
 * "bus-range = <시작 끝>" 두 워드를 읽어 IORESOURCE_BUS 자원으로 만든다.
 * 버스 번호도 주소 공간의 하나로 다루는 것이 PCI 코어의 방식이라, 메모리나
 * I/O 창과 같은 struct resource 에 담는다. 그래야 자원 배정 코드가 버스
 * 번호를 다른 자원과 똑같은 방법으로 나눠 줄 수 있다.
 *
 * res->name 에 노드 이름을 그대로 꽂는다. 문자열을 복사하지 않으므로
 * 이 자원은 노드보다 오래 살면 안 된다 - 실제로는 devm 으로 브리지 수명에
 * 묶여 있어 문제가 되지 않는다.
 *
 * static 이라 이 파일 밖에서는 부를 수 없다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   devm_of_pci_get_host_bridge_resources() -> [이 함수]
 *     -> of_property_read_u32_array()
 */
/**
 * of_pci_parse_bus_range() - parse the bus-range property of a PCI device
 * @node: device node
 * @res: address to a struct resource to return the bus-range
 *
 * Returns 0 on success or a negative error-code on failure.
 */
static int of_pci_parse_bus_range(struct device_node *node,
				  struct resource *res)
{
	/* [한국어] 시작 버스 번호와 끝 버스 번호 두 워드. */
	u32 bus_range[2];
	/* [한국어] 읽기 결과. */
	int error;

	/* [한국어] bus-range 속성을 두 워드로 읽는다. */
	error = of_property_read_u32_array(node, "bus-range", bus_range,
					   ARRAY_SIZE(bus_range));
	/* [한국어] 속성이 없거나 형식이 틀리다. */
	if (error)
		/* [한국어] errno 를 그대로 전한다 — 호출자가 기본값 0..0xff 로 대신한다. */
		return error;

	/* [한국어] 노드 이름을 그대로 꽂는다. 문자열을 복사하지 않으므로 이 자원은
	 * 노드보다 오래 살면 안 된다. 실제로는 devm 으로 브리지 수명에 묶여 있다. */
	res->name = node->name;
	/* [한국어] 시작 버스 번호. */
	res->start = bus_range[0];
	/* [한국어] 끝 버스 번호. */
	res->end = bus_range[1];
	/* [한국어] 버스 번호도 주소 공간의 하나로 다룬다는 표시. 이렇게 해 두면
	 * 자원 배정 코드가 버스 번호를 메모리 창과 똑같은 방법으로 나눠 줄 수 있다. */
	res->flags = IORESOURCE_BUS;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * of_get_pci_domain_nr - DT 가 지정한 PCI 도메인(세그먼트) 번호를 읽는다
 *
 * @node: 호스트 브리지의 DT 노드.
 * @return: 0..0xffff 범위의 도메인 번호. 속성이 없으면 -EINVAL,
 *          값이 비었으면 -ENODATA, 32비트를 넘으면 -EOVERFLOW.
 *
 * 도메인은 서로 독립된 PCI 버스 트리를 구분하는 번호다. 호스트 브리지가
 * 여럿인 시스템에서는 각 브리지 아래에 버스 0 이 따로 있으므로, 버스
 * 번호만으로는 장치를 식별할 수 없어 도메인이 필요하다.
 *
 * "linux,pci-domain" 이라는 이름에서 보듯 Linux 전용 확장이다. DT 표준이
 * 아니라 커널이 정한 속성이라 접두사가 붙었다. 이 속성을 쓰면 도메인
 * 번호가 부팅마다 고정되어 "0000:01:00.0" 같은 장치 이름이 안정된다.
 * 없으면 커널이 발견 순서대로 배정하므로 순서가 바뀌면 이름도 바뀐다.
 *
 * 반환값 관용구를 눈여겨볼 만하다. 성공 시 (u16) 로 잘라 돌려주므로
 * 항상 0..65535 의 양수이고, 실패는 음수다. 그래서 호출자가 부호만으로
 * 성패를 가를 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 이 트리에서 확인한 호출자: drivers/pci/pci.c:13710, 13773, 13828
 * (도메인 배정), pci-imx6.c:1721, pcie-starfive.c:494, pcie-mediatek.c:2456.
 *
 * 호출 체인:
 *   pci_bus_find_domain_nr() [pci.c] 등 -> [이 함수] -> of_property_read_u32()
 */
/**
 * of_get_pci_domain_nr - Find the host bridge domain number
 *			  of the given device node.
 * @node: Device tree node with the domain information.
 *
 * This function will try to obtain the host bridge domain number by finding
 * a property called "linux,pci-domain" of the given device node.
 *
 * Return:
 * * > 0	- On success, an associated domain number.
 * * -EINVAL	- The property "linux,pci-domain" does not exist.
 * * -ENODATA	- The linux,pci-domain" property does not have value.
 * * -EOVERFLOW	- Invalid "linux,pci-domain" property value.
 *
 * Returns the associated domain number from DT in the range [0-0xffff], or
 * a negative value if the required property is not found.
 */
int of_get_pci_domain_nr(struct device_node *node)
{
	/* [한국어] 읽은 도메인 번호. 속성은 32비트지만 실제 범위는 16비트다. */
	u32 domain;
	/* [한국어] 읽기 결과. */
	int error;

	/* [한국어] Linux 전용 확장 속성이라 이름에 접두사가 붙어 있다. DT 표준이 아니다. */
	error = of_property_read_u32(node, "linux,pci-domain", &domain);
	/* [한국어] 속성이 없거나(-EINVAL) 값이 비었거나(-ENODATA) 32비트를 넘는다(-EOVERFLOW). */
	if (error)
		/* [한국어] errno 를 그대로 전한다 — 호출자가 부호로 성패를 가른다. */
		return error;

	/* [한국어] u16 으로 잘라 0..65535 의 양수로 만든다. 성공은 항상 양수, 실패는 음수라
	 * 호출자가 부호만 보면 된다. 이 속성을 쓰면 도메인 번호가 부팅마다 고정되어
	 * "0000:01:00.0" 같은 장치 이름이 안정된다. */
	return (u16)domain;
}
/* [한국어] 모듈에 공개. pci.c 의 도메인 배정과 여러 컨트롤러 드라이버가 쓴다. */
EXPORT_SYMBOL_GPL(of_get_pci_domain_nr);

/* [한국어]
 * of_pci_preserve_config - 펌웨어가 해 둔 자원 배치를 유지해야 하는가
 *
 * @node: 컨트롤러의 DT 노드. NULL 이면 경고 후 of_chosen 으로 대신한다.
 * @return: true 면 재배치 금지, false 면 커널이 다시 배정해도 된다.
 *
 * "linux,pci-probe-only" 속성을 찾는다. 찾는 곳이 두 군데인 것이 요령이다.
 * 먼저 컨트롤러 노드에서 보고, 없으면 /chosen 에서 다시 본다. 컨트롤러별로
 * 지정할 수도 있고 시스템 전체로 지정할 수도 있게 하려는 것이며, 그
 * "다시 보기" 를 retry 라벨과 goto 로 구현했다.
 *
 * 반환값 판정이 세 갈래다.
 *   -ENODATA / -EOVERFLOW : 속성은 있는데 값이 없거나 32비트를 넘는다.
 *     형식 오류이므로 경고를 찍고 false. 여기서 /chosen 을 다시 보지
 *     않는 이유는 명시된 값이 잘못된 것과 값이 없는 것은 다른 상황이기 때문이다.
 *   -EINVAL : 속성 자체가 없다. 이때만 /chosen 으로 넘어가 다시 본다.
 *     이미 /chosen 을 보고 있었다면 더 갈 곳이 없어 false.
 *   성공 : 읽은 값이 0 이 아니면 true.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 이 트리에서 확인한 호출자: of_pci_check_probe_only()(같은 파일),
 * drivers/pci/probe.c:2495 (pci_preserve_config).
 *
 * 호출 체인:
 *   of_pci_check_probe_only() / pci_preserve_config() [probe.c]
 *     -> [이 함수] -> of_property_read_u32()
 */
/**
 * of_pci_preserve_config - Return true if the boot configuration needs to
 *                          be preserved
 * @node: Device tree node.
 *
 * Look for "linux,pci-probe-only" property for a given PCI controller's
 * node and return true if found. Also look in the chosen node if the
 * property is not found in the given controller's node.  Having this
 * property ensures that the kernel doesn't reconfigure the BARs and bridge
 * windows that are already done by the platform firmware.
 *
 * Return: true if the property exists; false otherwise.
 */
bool of_pci_preserve_config(struct device_node *node)
{
	/* [한국어] 읽은 값. 0 으로 초기화해 두어 속성이 없을 때 false 로 떨어지게 한다. */
	u32 val = 0;
	/* [한국어] 읽기 결과. */
	int ret;

	/* [한국어] 노드를 안 줬으면 */
	if (!node) {
		/* [한국어] 경고를 남기고 */
		pr_warn("device node is NULL, trying with of_chosen\n");
		/* [한국어] /chosen 으로 대신한다. 시스템 전체 지정을 보겠다는 뜻이다. */
		node = of_chosen;
	}

/* [한국어] 컨트롤러 노드에서 못 찾았을 때 /chosen 으로 한 번 더 오는 지점. */
retry:
	/* [한국어] 속성을 읽는다. */
	ret = of_property_read_u32(node, "linux,pci-probe-only", &val);
	/* [한국어] 읽기에 실패한 경우 원인별로 갈린다. */
	if (ret) {
		/* [한국어] 속성은 있는데 값이 없거나 32비트를 넘는다 — 형식 오류다. */
		if (ret == -ENODATA || ret == -EOVERFLOW) {
			/* [한국어] 어느 노드가 잘못됐는지 남긴다. */
			pr_warn("Incorrect value for linux,pci-probe-only in %pOF, ignoring\n",
				node);
			/* [한국어] false. 여기서 /chosen 을 다시 보지 않는 이유는 "명시했는데 잘못 썼다" 와
			 * "아예 안 썼다" 가 다른 상황이기 때문이다. */
			return false;
		}
		/* [한국어] 속성 자체가 없는 경우. */
		if (ret == -EINVAL) {
			/* [한국어] 이미 /chosen 을 보고 있었다면 더 갈 곳이 없다. */
			if (node == of_chosen)
				/* [한국어] false. */
				return false;

			/* [한국어] 컨트롤러 노드에 없었으니 시스템 전체 지정을 본다. */
			node = of_chosen;
			/* [한국어] 위로 돌아가 /chosen 에서 다시 읽는다. */
			goto retry;
		}
	}

	/* [한국어] 읽은 값이 0 이 아니면 */
	if (val)
		/* [한국어] 펌웨어 배치를 유지하라는 뜻. */
		return true;
	/* [한국어] 0 이면 */
	else
		/* [한국어] 커널이 다시 배정해도 된다. */
		return false;
}

/* [한국어]
 * of_pci_check_probe_only - probe-only 여부를 전역 PCI 플래그에 반영한다
 *
 * @return: 없음.
 *
 * of_pci_preserve_config() 의 판정을 받아 전역 플래그 PCI_PROBE_ONLY 를
 * 켜거나 끈다. 이 플래그가 켜지면 PCI 코어는 BAR 와 브리지 창을 다시
 * 배정하지 않고 펌웨어가 해 둔 배치를 그대로 쓴다.
 *
 * 전역이라는 점이 중요하다. 브리지마다 다르게 걸 수 없고 시스템 전체에
 * 적용된다. 그래서 인자 없이 of_chosen 노드만 본다.
 *
 * 쓰이는 곳은 주로 가상화 게스트다. 하이퍼바이저가 이미 배치를 정해 놓고
 * 게스트가 그것을 바꾸면 안 되는 경우, 또는 재배치할 만큼의 정보를
 * 게스트가 갖지 못한 경우다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(호스트 브리지 probe 초기).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/controller/pci-host-common.c:62.
 *
 * 호출 체인:
 *   pci_host_common_probe() -> [이 함수]
 *     -> of_pci_preserve_config() -> pci_add_flags()/pci_clear_flags()
 */
/**
 * of_pci_check_probe_only - Setup probe only mode if linux,pci-probe-only
 *                           is present and valid
 */
void of_pci_check_probe_only(void)
{
	/* [한국어] /chosen 에서 판정을 받는다. */
	if (of_pci_preserve_config(of_chosen))
		/* [한국어] 전역 플래그를 켠다. 이후 PCI 코어가 BAR 와 브리지 창을 재배정하지 않는다. */
		pci_add_flags(PCI_PROBE_ONLY);
	/* [한국어] 아니면 */
	else
		/* [한국어] 플래그를 내려 평소대로 재배정하게 한다. 전역이라 브리지마다 다르게 걸 수 없다. */
		pci_clear_flags(PCI_PROBE_ONLY);
}
/* [한국어] 모듈에 공개. pci-host-common.c 가 부른다. */
EXPORT_SYMBOL_GPL(of_pci_check_probe_only);

/* [한국어]
 * devm_of_pci_get_host_bridge_resources - ranges/dma-ranges 를 자원 목록으로
 *
 * @dev:           호스트 브리지 장치. devm 할당의 주인이기도 하다.
 * @resources:     바깥 방향(outbound) 창을 담을 목록.
 * @ib_resources:  안쪽 방향(inbound, dma-ranges) 창을 담을 목록. NULL 이면
 *                 dma-ranges 를 아예 읽지 않는다.
 * @io_base:       I/O 창의 CPU 물리 시작 주소를 돌려줄 곳. I/O 창이 있는
 *                 보드에서는 NULL 을 주면 -EINVAL 이다.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 이 파일에서 가장 큰 함수이고, DT 와 PCI 를 잇는 핵심이다.
 *
 * 읽는 것이 셋이다.
 *   1) bus-range - 이 브리지 아래 버스 번호 범위. 없으면 0..0xff 로 두고,
 *      0xff 를 넘게 적혀 있으면 경고 후 0xff 로 자른다. 버스 번호가 8비트라
 *      그 위는 표현할 수 없기 때문이다.
 *   2) ranges - 바깥 방향 창. 항목마다 (플래그, PCI 버스 주소, CPU 주소,
 *      크기)가 들어 있다. CPU 주소와 PCI 주소가 다를 수 있다는 것이 이
 *      파일 전체에서 가장 중요한 사실이며, 그 차이를 offset 으로 계산해
 *      pci_add_resource_offset() 에 함께 넘긴다. 그 offset 이 나중에
 *      host-bridge.c 의 pcibios_resource_to_bus 계열이 쓰는 값이다.
 *   3) dma-ranges - 안쪽 방향 창. 장치가 DMA 로 접근할 수 있는 메모리
 *      구간이며, 방향만 반대일 뿐 처리 방식은 같다.
 *
 * 예외 처리에 규칙이 있다. 번역 실패(OF_BAD_ADDR)나 크기 0 인 항목은
 * 건너뛰기만 하고 실패로 보지 않는다 - 항목 하나가 이상하다고 브리지
 * 전체를 포기할 이유가 없다. 반대로 메모리 할당 실패는 곧바로 failed 로
 * 가서 지금까지 만든 목록을 통째로 되돌린다.
 *
 * I/O 창은 하나만 지원한다. 둘째가 나오면 경고를 찍고 앞의 것을 덮어쓴다 -
 * io_base 가 스칼라 하나뿐이라 그 이상은 표현할 방법이 없다.
 * MEM 창에서는 IORESOURCE_MEM_64 를 떼는데, 64비트 표시는 장치 BAR 의
 * 성질이지 브리지 창의 성질이 아니기 때문이다.
 *
 * devm_kzalloc/devm_kmemdup 을 쓰므로 자원 구조체 수명이 브리지 장치에
 * 묶인다. 브리지가 사라지면 자동으로 반납된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL 할당).
 *
 * 호출 체인:
 *   pci_parse_request_of_pci_ranges() -> [이 함수]
 *     -> of_pci_parse_bus_range() -> of_pci_range_parser_init()
 *     -> of_pci_range_to_resource() -> pci_add_resource_offset()
 */
/**
 * devm_of_pci_get_host_bridge_resources() - Resource-managed parsing of PCI
 *                                           host bridge resources from DT
 * @dev: host bridge device
 * @resources: list where the range of resources will be added after DT parsing
 * @ib_resources: list where the range of inbound resources (with addresses
 *                from 'dma-ranges') will be added after DT parsing
 * @io_base: pointer to a variable that will contain on return the physical
 * address for the start of the I/O range. Can be NULL if the caller doesn't
 * expect I/O ranges to be present in the device tree.
 *
 * This function will parse the "ranges" property of a PCI host bridge device
 * node and setup the resource mapping based on its content. It is expected
 * that the property conforms with the Power ePAPR document.
 *
 * It returns zero if the range parsing has been successful or a standard error
 * value if it failed.
 */
static int devm_of_pci_get_host_bridge_resources(struct device *dev,
			struct list_head *resources,
			struct list_head *ib_resources,
			resource_size_t *io_base)
{
	/* [한국어] 파싱할 DT 노드. */
	struct device_node *dev_node = dev->of_node;
	/* [한국어] res 는 목록에 넣을 실물, tmp_res 는 변환 결과를 잠시 받는 스택 변수다. */
	struct resource *res, tmp_res;
	/* [한국어] 버스 번호 범위 자원. devm 으로 할당해 브리지 수명에 묶는다. */
	struct resource *bus_range;
	/* [한국어] ranges 항목 하나를 담는 구조체(플래그, PCI 주소, CPU 주소, 크기). */
	struct of_pci_range range;
	/* [한국어] ranges 를 항목 단위로 잘라 주는 파서 상태. */
	struct of_pci_range_parser parser;
	/* [한국어] 로그에 찍을 종류 문자열. */
	const char *range_type;
	/* [한국어] 오류 코드. */
	int err;

	/* [한국어] 호출자가 I/O 시작 주소를 받겠다고 했으면 */
	if (io_base)
		/* [한국어] 먼저 "아직 없음" 표시로 채워 둔다. 아래에서 I/O 창을 만나야 실제 값이 들어간다. */
		*io_base = (resource_size_t)OF_BAD_ADDR;

	/* [한국어] 버스 번호 자원 구조체를 devm 으로 할당한다. */
	bus_range = devm_kzalloc(dev, sizeof(*bus_range), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!bus_range)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] 어느 브리지의 창을 파싱하는지 부팅 로그에 남긴다. */
	dev_info(dev, "host bridge %pOF ranges:\n", dev_node);

	/* [한국어] bus-range 속성을 읽어 본다. */
	err = of_pci_parse_bus_range(dev_node, bus_range);
	/* [한국어] 없거나 형식이 틀리면 */
	if (err) {
		/* [한국어] 기본값 0 부터 */
		bus_range->start = 0;
		/* [한국어] 0xff 까지 — 버스 번호가 8비트라 이것이 표현 가능한 전 범위다. */
		bus_range->end = 0xff;
		/* [한국어] 버스 번호 자원임을 표시. */
		bus_range->flags = IORESOURCE_BUS;
	/* [한국어] 제대로 읽힌 경우. */
	} else {
		/* [한국어] 끝 번호가 8비트를 넘게 적혀 있으면 */
		if (bus_range->end > 0xff) {
			/* [한국어] 경고를 남기고 */
			dev_warn(dev, "  Invalid end bus number in %pR, defaulting to 0xff\n",
				 bus_range);
			/* [한국어] 표현 가능한 최대값으로 자른다. */
			bus_range->end = 0xff;
		}
	}
	/* [한국어] 버스 번호 범위를 자원 목록의 첫 항목으로 넣는다.
	 * 오프셋이 없는 pci_add_resource() 인 이유는 버스 번호에는 주소 변환이 없기 때문이다. */
	pci_add_resource(resources, bus_range);

	/* Check for ranges property */
	/* [한국어] ranges 속성 파서를 초기화한다. */
	err = of_pci_range_parser_init(&parser, dev_node);
	/* [한국어] ranges 가 아예 없는 브리지도 있다. */
	if (err)
		/* [한국어] 오류가 아니라 성공으로 돌아간다 — 버스 범위만 있어도 쓸 수 있다. */
		return 0;

	/* [한국어] 파싱 시작을 디버그 로그에 남긴다. */
	dev_dbg(dev, "Parsing ranges property...\n");
	/* [한국어] ranges 를 항목 하나씩 꺼내며 순회한다. */
	for_each_of_pci_range(&parser, &range) {
		/* Read next ranges element */
		/* [한국어] 플래그의 종류 비트가 I/O 를 가리키는가. */
		if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_IO)
			/* [한국어] 로그용 이름. */
			range_type = "IO";
		/* [한국어] 메모리 창인가. */
		else if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_MEM)
			/* [한국어] 로그용 이름. */
			range_type = "MEM";
		/* [한국어] 둘 다 아니면 */
		else
			/* [한국어] 알 수 없는 종류로 표시한다. 아래에서 걸러지거나 변환에 실패한다. */
			range_type = "err";
		/* [한국어] CPU 주소 구간과 그것이 대응하는 PCI 버스 주소를 로그에 남긴다.
		 * 두 값이 다를 수 있다는 것이 이 파일 전체에서 가장 중요한 사실이다. */
		dev_info(dev, "  %6s %#012llx..%#012llx -> %#012llx\n",
			 range_type, range.cpu_addr,
			 range.cpu_addr + range.size - 1, range.pci_addr);

		/*
		 * If we failed translation or got a zero-sized region
		 * then skip this range
		 */
		/* [한국어] 주소 번역에 실패했거나 크기가 0 인 항목. */
		if (range.cpu_addr == OF_BAD_ADDR || range.size == 0)
			/* [한국어] 건너뛴다. 항목 하나가 이상하다고 브리지 전체를 포기할 이유가 없다. */
			continue;

		/* [한국어] ranges 항목을 struct resource 로 변환한다. */
		err = of_pci_range_to_resource(&range, dev_node, &tmp_res);
		/* [한국어] 변환 실패. */
		if (err)
			/* [한국어] 역시 건너뛴다. */
			continue;

		/* [한국어] 스택의 변환 결과를 devm 힙으로 복사한다. 목록에 남을 실물이므로
		 * 브리지가 사라질 때까지 살아야 한다. */
		res = devm_kmemdup(dev, &tmp_res, sizeof(tmp_res), GFP_KERNEL);
		/* [한국어] 복사 실패. */
		if (!res) {
			/* [한국어] 메모리 부족으로 기록하고 */
			err = -ENOMEM;
			/* [한국어] 지금까지 만든 목록을 통째로 되돌리는 경로로. 할당 실패만은 치명적으로 다룬다. */
			goto failed;
		}

		/* [한국어] I/O 창인 경우. */
		if (resource_type(res) == IORESOURCE_IO) {
			/* [한국어] 호출자가 io_base 를 안 줬으면 CPU 쪽 시작 주소를 돌려줄 방법이 없다. */
			if (!io_base) {
				/* [한국어] 어느 노드에서 문제가 생겼는지 남기고 */
				dev_err(dev, "I/O range found for %pOF. Please provide an io_base pointer to save CPU base address\n",
					dev_node);
				/* [한국어] 인자 오류로 기록해 */
				err = -EINVAL;
				/* [한국어] 되돌리기 경로로 간다. */
				goto failed;
			}
			/* [한국어] 이미 I/O 창을 하나 처리했다면 */
			if (*io_base != (resource_size_t)OF_BAD_ADDR)
				/* [한국어] 앞의 것을 덮어쓴다고 경고한다. io_base 가 스칼라 하나뿐이라
				 * I/O 창을 둘 이상 표현할 방법이 없다. */
				dev_warn(dev, "More than one I/O resource converted for %pOF. CPU base address for old range lost!\n",
					 dev_node);
			/* [한국어] 이 창의 CPU 쪽 시작 주소를 돌려준다. */
			*io_base = range.cpu_addr;
		/* [한국어] 메모리 창인 경우. */
		} else if (resource_type(res) == IORESOURCE_MEM) {
			/* [한국어] 64비트 표시를 뗀다. 그것은 장치 BAR 의 성질이지 브리지 창의 성질이 아니다. */
			res->flags &= ~IORESOURCE_MEM_64;
		}

		/* [한국어] 자원을 목록에 넣으면서 CPU 주소와 PCI 주소의 차이를 offset 으로 함께 넘긴다.
		 * 이 offset 이 나중에 host-bridge.c 의 pcibios_resource_to_bus 계열이 쓰는 값이다. */
		pci_add_resource_offset(resources, res,	res->start - range.pci_addr);
	}

	/* Check for dma-ranges property */
	/* [한국어] 호출자가 안쪽 방향 창을 원하지 않으면 */
	if (!ib_resources)
		/* [한국어] 바깥 방향만 채우고 끝낸다. */
		return 0;
	/* [한국어] dma-ranges 파서를 초기화한다. */
	err = of_pci_dma_range_parser_init(&parser, dev_node);
	/* [한국어] dma-ranges 가 없는 브리지도 있다. */
	if (err)
		/* [한국어] 오류가 아니다. */
		return 0;

	/* [한국어] 파싱 시작을 디버그 로그에 남긴다. */
	dev_dbg(dev, "Parsing dma-ranges property...\n");
	/* [한국어] dma-ranges 항목을 하나씩 꺼내며 순회한다. */
	for_each_of_pci_range(&parser, &range) {
		/*
		 * If we failed translation or got a zero-sized region
		 * then skip this range
		 */
		/* [한국어] 메모리 종류가 아니거나 */
		if (((range.flags & IORESOURCE_TYPE_BITS) != IORESOURCE_MEM) ||
		    /* [한국어] 번역 실패 또는 크기 0 이면 */
		    range.cpu_addr == OF_BAD_ADDR || range.size == 0)
			/* [한국어] 건너뛴다. 안쪽 방향은 메모리만 의미가 있다. */
			continue;

		/* [한국어] CPU 주소 구간과 대응하는 PCI 주소를 남긴다. "IB" 가 inbound 다. */
		dev_info(dev, "  %6s %#012llx..%#012llx -> %#012llx\n",
			 "IB MEM", range.cpu_addr,
			 range.cpu_addr + range.size - 1, range.pci_addr);


		/* [한국어] struct resource 로 변환한다. */
		err = of_pci_range_to_resource(&range, dev_node, &tmp_res);
		/* [한국어] 변환 실패. */
		if (err)
			/* [한국어] 건너뛴다. */
			continue;

		/* [한국어] devm 힙으로 복사한다. */
		res = devm_kmemdup(dev, &tmp_res, sizeof(tmp_res), GFP_KERNEL);
		/* [한국어] 복사 실패. */
		if (!res) {
			/* [한국어] 메모리 부족으로 기록하고 */
			err = -ENOMEM;
			/* [한국어] 되돌리기 경로로. */
			goto failed;
		}

		/* [한국어] 안쪽 방향 목록에 넣는다. 방향만 반대일 뿐 offset 계산은 같다 —
		 * 장치가 내는 PCI 주소를 CPU 물리 주소로 되돌릴 때 쓰인다. */
		pci_add_resource_offset(ib_resources, res,
					res->start - range.pci_addr);
	}

	/* [한국어] 여기까지 오면 성공이다. */
	return 0;

/* [한국어] 할당 실패로만 도달하는 되돌리기 경로. */
failed:
	/* [한국어] 지금까지 목록에 넣은 자원을 통째로 해제한다. 구조체 자체는 devm 이
	 * 반납하지만, 목록 항목(resource_entry)은 여기서 풀어야 한다. */
	pci_free_resource_list(resources);
	/* [한국어] 실패 원인을 그대로 위로 전한다. */
	return err;
}

/* [한국어] DT 인터럽트 파서가 빌드에 포함될 때만 아래 두 함수를 컴파일한다.
 * of_irq_parse_one() / of_irq_parse_raw() / irq_create_of_mapping() 이
 * 그 안에 있어, 없으면 링크가 깨진다. */
#if IS_ENABLED(CONFIG_OF_IRQ)
/* [한국어]
 * of_irq_parse_pci - INTx 핀이 어느 인터럽트 컨트롤러의 몇 번인지 알아낸다
 *
 * @pdev:    IRQ 가 필요한 PCI 장치.
 * @out_irq: 결과를 담을 of_phandle_args(컨트롤러 노드 + 인자).
 * @return: 0 성공. -ENODEV 는 이 장치가 INTx 핀을 쓰지 않는다는 뜻이고,
 *          -EINVAL 은 위로 올라가다 노드를 못 찾은 것, -ENOENT 는
 *          interrupt-map 자체가 없는 것이다.
 *
 * ACPI 의 _PRT 에 해당하는 일을 DT 로 한다. 어려운 점은 DT 가 모든 PCI
 * 장치를 기술하지는 않는다는 것이다. 열거로 발견된 장치에는 노드가 없는
 * 경우가 흔하다.
 *
 * 그래서 두 갈래로 간다.
 *   - 장치 자신의 노드가 있고 거기서 인터럽트가 풀리면 그대로 끝낸다.
 *   - 없으면 스위즐링을 하며 위로 올라간다. PCI 규격이 정한 규칙에 따라
 *     브리지를 하나 건널 때마다 INTA-D 가 장치 번호만큼 회전하는데
 *     (pci_swizzle_interrupt_pin), 그 회전을 흉내 내면서 interrupt-map 을
 *     가진 조상을 찾는다. 찾으면 거기서부터는 표준 DT 파서에 넘긴다.
 *
 * 마지막에 만드는 laddr 3워드가 DT 규약의 "자식 주소" 다. interrupt-map 은
 * (자식주소, 자식인터럽트) -> (부모, 부모인터럽트) 형태의 표라, 조회 키로
 * 주소가 필요하다. 첫 워드에 버스 번호와 devfn 을 넣고 나머지 둘은 0 이다.
 * 상류 주석이 경고하듯 여기 쓰는 버스 번호는 Linux 가 배정한 것이라
 * 펌웨어가 매긴 번호와 다를 수 있다. 다행히 interrupt-map-mask 가 보통
 * 버스 번호를 비교 대상에서 빼기 때문에 대개 문제가 되지 않는다.
 *
 * -ENOENT 만 경고 수준을 낮춰 다루는데, interrupt-map 이 아예 없는 것은
 * "이 보드는 INTx 를 안 쓴다" 일 수 있어 오류로 볼 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   of_irq_parse_and_map_pci() -> [이 함수]
 *     -> of_irq_parse_one() 또는 pci_swizzle_interrupt_pin() + of_irq_parse_raw()
 */
/**
 * of_irq_parse_pci - Resolve the interrupt for a PCI device
 * @pdev:       the device whose interrupt is to be resolved
 * @out_irq:    structure of_phandle_args filled by this function
 *
 * This function resolves the PCI interrupt for a given PCI device. If a
 * device node exists for a given pci_dev, it will use normal OF tree
 * walking. If not, it will implement standard swizzling and walk up the
 * PCI tree until a device node is found, at which point it will finish
 * resolving using the OF tree walking.
 */
static int of_irq_parse_pci(const struct pci_dev *pdev, struct of_phandle_args *out_irq)
{
	/* [한국어] dn 은 장치 자신의 노드, ppnode 는 interrupt-map 을 가진 조상 노드.
	 * ppnode 가 NULL 인 동안 아래 루프가 위로 올라간다. */
	struct device_node *dn, *ppnode = NULL;
	/* [한국어] 위쪽 P2P 브리지의 pci_dev. */
	struct pci_dev *ppdev;
	/* [한국어] interrupt-map 조회 키로 쓸 "자식 주소" 3워드. DT 는 big-endian 이라
	 * __be32 로 선언하고 아래에서 cpu_to_be32 로 채운다. */
	__be32 laddr[3];
	/* [한국어] INTx 핀 번호(1=INTA .. 4=INTD). 브리지를 건널 때마다 회전한다. */
	u8 pin;
	/* [한국어] 오류 코드. */
	int rc;

	/*
	 * Check if we have a device node, if yes, fallback to standard
	 * device tree parsing
	 */
	/* [한국어] 장치 자신의 DT 노드를 찾는다. 없을 수도 있다. */
	dn = pci_device_to_OF_node(pdev);
	/* [한국어] 있으면 */
	if (dn) {
		/* [한국어] 표준 DT 파서로 바로 풀어 본다. 0번째 인터럽트를 묻는다. */
		rc = of_irq_parse_one(dn, 0, out_irq);
		/* [한국어] 성공했으면 */
		if (!rc)
			/* [한국어] 그대로 끝낸다(rc 는 0 이다). 가장 단순한 경로다. */
			return rc;
	}

	/*
	 * Ok, we don't, time to have fun. Let's start by building up an
	 * interrupt spec.  we assume #interrupt-cells is 1, which is standard
	 * for PCI. If you do different, then don't use that routine.
	 */
	/* [한국어] config space 의 Interrupt Pin 레지스터에서 이 장치가 쓰는 핀을 읽는다. */
	rc = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
	/* [한국어] 읽기 실패. */
	if (rc != 0)
		/* [한국어] 공통 오류 경로로. */
		goto err;
	/* No pin, exit with no error message. */
	/* [한국어] 0 은 "이 장치는 INTx 를 쓰지 않는다" 는 뜻이다. */
	if (pin == 0)
		/* [한국어] 오류 메시지 없이 조용히 물러난다. 실패가 아니라 해당 없음이다. */
		return -ENODEV;

	/* Local interrupt-map in the device node? Use it! */
	/* [한국어] 장치 노드 자체가 interrupt-map 을 갖고 있는가.
	 * dn 이 NULL 일 수 있는데, of_property_present 의 NULL 처리는
	 * 이 트리에 include/linux/of.h 가 없어 직접 확인하지 못했다. */
	if (of_property_present(dn, "interrupt-map")) {
		/* [한국어] 있으면 한 단계만큼 스위즐링한다. */
		pin = pci_swizzle_interrupt_pin(pdev, pin);
		/* [한국어] 그리고 그 노드에서 조회를 시작한다. */
		ppnode = dn;
	}

	/* Now we walk up the PCI tree */
	/* [한국어] interrupt-map 을 가진 조상을 찾을 때까지 위로 올라간다. */
	while (!ppnode) {
		/* Get the pci_dev of our parent */
		/* [한국어] 현재 장치가 매달린 버스를 만든 브리지. */
		ppdev = pdev->bus->self;

		/* Ouch, it's a host bridge... */
		/* [한국어] NULL 이면 루트 버스에 닿았다 — 위에 브리지가 없다. */
		if (ppdev == NULL) {
			/* [한국어] 호스트 브리지 쪽 노드를 가져온다. */
			ppnode = pci_bus_to_OF_node(pdev->bus);

			/* No node for host bridge ? give up */
			/* [한국어] 그것마저 없으면 더 올라갈 곳이 없다. */
			if (ppnode == NULL) {
				/* [한국어] 인자/구성 오류로 기록하고 */
				rc = -EINVAL;
				/* [한국어] 오류 경로로. */
				goto err;
			}
		/* [한국어] P2P 브리지를 만난 경우. */
		} else {
			/* We found a P2P bridge, check if it has a node */
			/* [한국어] 그 브리지의 노드를 본다. 없으면 NULL 이라 루프가 한 번 더 돈다. */
			ppnode = pci_device_to_OF_node(ppdev);
		}

		/*
		 * Ok, we have found a parent with a device node, hand over to
		 * the OF parsing code.
		 *
		 * We build a unit address from the linux device to be used for
		 * resolution. Note that we use the linux bus number which may
		 * not match your firmware bus numbering.
		 *
		 * Fortunately, in most cases, interrupt-map-mask doesn't
		 * include the bus number as part of the matching.
		 *
		 * You should still be careful about that though if you intend
		 * to rely on this function (you ship a firmware that doesn't
		 * create device nodes for all PCI devices).
		 */
		/* [한국어] 노드를 찾았으면 */
		if (ppnode)
			/* [한국어] 루프를 끝내고 아래 표준 파서로 넘긴다. */
			break;

		/*
		 * We can only get here if we hit a P2P bridge with no node;
		 * let's do standard swizzling and try again
		 */
		/* [한국어] 노드가 없는 브리지였다 — PCI 규격의 회전 규칙을 흉내 내고 */
		pin = pci_swizzle_interrupt_pin(pdev, pin);
		/* [한국어] 한 단계 위로 올라가 다시 시도한다. */
		pdev = ppdev;
	}

	/* [한국어] 조회를 시작할 노드(interrupt-map 을 가진 조상). */
	out_irq->np = ppnode;
	/* [한국어] PCI 의 #interrupt-cells 는 1 이다 — 핀 번호 하나만 넘긴다. */
	out_irq->args_count = 1;
	/* [한국어] 회전을 마친 최종 핀 번호. */
	out_irq->args[0] = pin;
	/* [한국어] 조회 키의 첫 워드에 버스 번호와 devfn 을 넣는다. 상류 주석의 경고대로
	 * 여기 쓰는 버스 번호는 Linux 가 배정한 것이라 펌웨어 번호와 다를 수 있다.
	 * 다행히 interrupt-map-mask 가 보통 버스 번호를 비교에서 빼 준다. */
	laddr[0] = cpu_to_be32((pdev->bus->number << 16) | (pdev->devfn << 8));
	/* [한국어] 나머지 두 워드는 0 이다. PCI 주소는 세 워드지만 조회에는 첫 워드만 쓴다. */
	laddr[1] = laddr[2] = cpu_to_be32(0);
	/* [한국어] 조립한 키로 interrupt-map 표를 따라가 최종 컨트롤러와 번호를 얻는다. */
	rc = of_irq_parse_raw(laddr, out_irq);
	/* [한국어] 표에 맞는 항목이 없거나 표 자체가 없다. */
	if (rc)
		/* [한국어] 오류 경로로. */
		goto err;
	/* [한국어] 성공. */
	return 0;
/* [한국어] 공통 오류 경로. */
err:
	/* [한국어] -ENOENT 는 interrupt-map 이 아예 없다는 뜻이다. */
	if (rc == -ENOENT) {
		/* [한국어] 경고 수준으로만 남긴다 — "이 보드는 INTx 를 안 쓴다" 일 수 있어
		 * 오류로 단정할 수 없기 때문이다. */
		dev_warn(&pdev->dev,
			"%s: no interrupt-map found, INTx interrupts not available\n",
			__func__);
		/* [한국어] 같은 이유의 일반 안내는 한 번만 찍는다. 장치마다 반복되면 로그가 넘친다. */
		pr_warn_once("%s: possibly some PCI slots don't have level triggered interrupts capability\n",
			__func__);
	/* [한국어] 그 밖의 실패는 */
	} else {
		/* [한국어] 진짜 오류로 남긴다. */
		dev_err(&pdev->dev, "%s: failed with rc=%d\n", __func__, rc);
	}
	/* [한국어] 원인을 그대로 위로 전한다. */
	return rc;
}

/* [한국어]
 * of_irq_parse_and_map_pci - INTx 를 해석해 Linux IRQ 번호까지 만들어 준다
 *
 * @dev:  IRQ 가 필요한 PCI 장치.
 * @slot: 쓰지 않는다. map_irq 콜백 서명을 맞추기 위한 자리다.
 * @pin:  역시 쓰지 않는다. 핀은 config space 에서 직접 읽는다.
 * @return: Linux 가상 IRQ 번호. 실패하면 0 인데, 이것이 "IRQ 없음" 의
 *          관례값이라 음수 errno 를 돌려주지 않는다.
 *
 * of_irq_parse_pci() 가 "어느 컨트롤러의 몇 번" 까지 알아내면, 여기서
 * irq_create_of_mapping() 으로 그 하드웨어 번호를 Linux 가상 IRQ 번호로
 * 바꾼다. 두 단계를 나눈 이유는 앞 단계가 DT 해석이고 뒤 단계가 IRQ
 * 서브시스템 등록이라 관심사가 다르기 때문이다.
 *
 * 인자 두 개를 쓰지 않으면서도 받는 이유가 상류 주석에 있다. 이 함수를
 * pci_host_bridge.map_irq 에 그대로 꽂아 쓰기 위해서다. 그 콜백 서명이
 * (dev, slot, pin) 이라 맞춰 준 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(자원 배정 단계).
 *
 * 호출 체인:
 *   pci_assign_irq() -> bridge->map_irq -> [이 함수]
 *     -> of_irq_parse_pci() -> irq_create_of_mapping()
 */
/**
 * of_irq_parse_and_map_pci() - Decode a PCI IRQ from the device tree and map to a VIRQ
 * @dev: The PCI device needing an IRQ
 * @slot: PCI slot number; passed when used as map_irq callback. Unused
 * @pin: PCI IRQ pin number; passed when used as map_irq callback. Unused
 *
 * @slot and @pin are unused, but included in the function so that this
 * function can be used directly as the map_irq callback to
 * pci_assign_irq() and struct pci_host_bridge.map_irq pointer
 */
int of_irq_parse_and_map_pci(const struct pci_dev *dev, u8 slot, u8 pin)
{
	/* [한국어] of_irq_parse_pci() 가 채워 줄 결과(컨트롤러 노드 + 인자). */
	struct of_phandle_args oirq;
	/* [한국어] 파싱 결과. */
	int ret;

	/* [한국어] 어느 컨트롤러의 몇 번인지 알아낸다. */
	ret = of_irq_parse_pci(dev, &oirq);
	/* [한국어] 해석에 실패했으면 */
	if (ret)
		/* [한국어] 음수 errno 가 아니라 0 을 준다. 이 함수의 반환 규약에서 0 이 "IRQ 없음" 이다. */
		return 0; /* Proper return code 0 == NO_IRQ */

	/* [한국어] 하드웨어 번호를 Linux 가상 IRQ 번호로 바꿔 등록한다. */
	return irq_create_of_mapping(&oirq);
}
/* [한국어] 모듈에 공개. 컨트롤러 드라이버가 bridge->map_irq 에 그대로 꽂아 쓴다. */
EXPORT_SYMBOL_GPL(of_irq_parse_and_map_pci);
/* [한국어] CONFIG_OF_IRQ 블록 끝. */
#endif	/* CONFIG_OF_IRQ */

/* [한국어]
 * pci_parse_request_of_pci_ranges - 파싱한 자원을 실제로 예약하고 매핑한다
 *
 * @dev:    호스트 브리지 장치.
 * @bridge: 자원 목록을 담을 pci_host_bridge.
 * @return: 0 성공, 음수 errno 실패.
 *
 * devm_of_pci_get_host_bridge_resources() 가 DT 를 읽어 만든 목록을 받아
 * 그다음 단계를 밟는다.
 *   1) windows 와 dma_ranges 목록을 초기화하고 파싱 결과를 채운다
 *   2) devm_request_pci_bus_resources() 로 그 구간을 커널 자원 트리에
 *      예약한다. 다른 장치가 같은 물리 주소를 잡는 것을 막는 단계다
 *   3) 창을 하나씩 훑으며
 *      - I/O 창은 devm_pci_remap_iospace() 로 CPU 의 I/O 주소 공간에
 *        매핑한다. ARM 처럼 I/O 명령이 없는 아키텍처는 MMIO 구간을 잘라
 *        I/O 처럼 쓰기 때문에 이 매핑이 필요하다. 실패하면 그 창만 목록에서
 *        빼고 계속 간다 - I/O 는 없어도 대부분 동작한다
 *      - MEM 창은 prefetchable 여부를 본다
 *
 * 마지막 검사가 중요하다. non-prefetchable 메모리 창이 하나도 없으면
 * 경고한다. 대부분의 장치 BAR 가 non-prefetchable 이라 그 창이 없으면
 * 자원 배정이 통째로 실패하기 때문이다. 32비트를 넘는 non-prefetchable
 * 창에도 경고를 찍는다 - 그 위쪽은 prefetchable 로 선언해야 브리지가
 * 64비트 창으로 다룰 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   devm_of_pci_bridge_init() -> [이 함수]
 *     -> devm_of_pci_get_host_bridge_resources()
 *     -> devm_request_pci_bus_resources() -> devm_pci_remap_iospace()
 */
static int pci_parse_request_of_pci_ranges(struct device *dev,
					   struct pci_host_bridge *bridge)
{
	/* [한국어] err 는 하위 호출 결과, res_valid 는 non-prefetchable 메모리 창을
	 * 하나라도 봤는지를 기록하는 표시다. */
	int err, res_valid = 0;
	/* [한국어] I/O 창의 CPU 물리 시작 주소를 받을 곳. */
	resource_size_t iobase;
	/* [한국어] 창 목록을 안전하게 순회하기 위한 커서 쌍. 순회 중 항목을 지울 수 있어
	 * _safe 판을 쓰므로 다음 항목을 미리 잡아 둘 tmp 가 필요하다. */
	struct resource_entry *win, *tmp;

	/* [한국어] 바깥 방향 창 목록을 비운다. */
	INIT_LIST_HEAD(&bridge->windows);
	/* [한국어] 안쪽 방향(dma-ranges) 목록도 비운다. */
	INIT_LIST_HEAD(&bridge->dma_ranges);

	/* [한국어] DT 를 읽어 바깥 방향 창은 bridge->windows 에, */
	err = devm_of_pci_get_host_bridge_resources(dev, &bridge->windows,
						    /* [한국어] 안쪽 방향 창은 bridge->dma_ranges 에 채우고 I/O 시작 주소를 받는다. */
						    &bridge->dma_ranges, &iobase);
	/* [한국어] 파싱 실패(할당 실패 또는 I/O 창 인자 오류). */
	if (err)
		/* [한국어] 그대로 전한다. */
		return err;

	/* [한국어] 파싱한 구간을 커널 자원 트리에 예약한다.
	 * 다른 장치가 같은 물리 주소를 잡는 것을 막는 단계다. */
	err = devm_request_pci_bus_resources(dev, &bridge->windows);
	/* [한국어] 예약 실패 — 대개 다른 드라이버와 구간이 겹친다. */
	if (err)
		/* [한국어] 그대로 전한다. */
		return err;

	/* [한국어] 창을 하나씩 훑는다. 아래에서 항목을 지울 수 있어 _safe 판을 쓴다. */
	resource_list_for_each_entry_safe(win, tmp, &bridge->windows) {
		/* [한국어] 이 항목이 가리키는 실제 자원. */
		struct resource *res = win->res;

		/* [한국어] 창의 종류로 갈린다. */
		switch (resource_type(res)) {
		/* [한국어] I/O 창. */
		case IORESOURCE_IO:
			/* [한국어] CPU 의 I/O 주소 공간에 매핑한다. ARM 처럼 I/O 명령이 없는 아키텍처는
			 * MMIO 구간을 잘라 I/O 처럼 쓰기 때문에 이 매핑이 필요하다. */
			err = devm_pci_remap_iospace(dev, res, iobase);
			/* [한국어] 매핑 실패. */
			if (err) {
				/* [한국어] 어느 자원이 실패했는지 남기고 */
				dev_warn(dev, "error %d: failed to map resource %pR\n",
					 err, res);
				/* [한국어] 그 창만 목록에서 빼고 계속 간다. I/O 는 없어도 대부분 동작하므로
				 * 브리지 전체를 포기하지 않는다. */
				resource_list_destroy_entry(win);
			}
			/* [한국어] I/O 처리 끝. */
			break;
		/* [한국어] 메모리 창. */
		case IORESOURCE_MEM:
			/* [한국어] non-prefetchable 창을 하나라도 봤으면 표시를 세운다.
			 * 대부분의 장치 BAR 가 non-prefetchable 이라 그 창이 반드시 필요하다. */
			res_valid |= !(res->flags & IORESOURCE_PREFETCH);

			/* [한국어] non-prefetchable 인데 */
			if (!(res->flags & IORESOURCE_PREFETCH))
				/* [한국어] 크기가 32비트를 넘으면 */
				if (upper_32_bits(resource_size(res)))
					/* [한국어] 경고한다. 그만큼 큰 구간은 prefetchable 로 선언해야 브리지가
					 * 64비트 창으로 다룰 수 있기 때문이다. */
					dev_warn(dev, "Memory resource size exceeds max for 32 bits\n");

			/* [한국어] 메모리 처리 끝. */
			break;
		}
	}

	/* [한국어] non-prefetchable 메모리 창을 하나도 못 봤으면 */
	if (!res_valid)
		/* [한국어] 경고한다. 그 창이 없으면 장치 BAR 배정이 통째로 실패한다.
		 * 다만 오류로 만들지는 않는다 — 경고만 남기고 진행한다. */
		dev_warn(dev, "non-prefetchable memory resource required\n");

	/* [한국어] 성공. 위 경고들은 반환값에 영향을 주지 않는다. */
	return 0;
}

/* [한국어]
 * devm_of_pci_bridge_init - DT 기반 호스트 브리지의 공통 초기화
 *
 * @dev:    호스트 브리지 장치.
 * @bridge: 채울 pci_host_bridge.
 * @return: 0 성공(DT 노드가 없어도 0), 음수 errno 실패.
 *
 * 컨트롤러 드라이버가 자원 파싱과 INTx 콜백 설정을 각자 베껴 쓰지 않도록
 * 모아 둔 진입점이다. dev->of_node 가 없으면 DT 시스템이 아니라는 뜻이라
 * 아무 일도 하지 않고 0 을 돌려준다 - 그래야 ACPI 경로에서도 같은 코드가
 * 무해하게 지나간다.
 *
 * 하는 일은 둘이다.
 *   swizzle_irq = pci_common_swizzle : 브리지를 건널 때 INTA-D 가 어떻게
 *     회전하는지를 정한 표준 규칙. PCI 규격이 정한 것이라 DT 와 무관하다.
 *   map_irq = of_irq_parse_and_map_pci : 회전 끝에 나온 핀을 실제 IRQ 로
 *     바꾸는 함수. 이쪽은 DT 의 interrupt-map 을 읽는다.
 * 그다음 pci_parse_request_of_pci_ranges() 로 주소 공간을 세운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(브리지 probe).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:1894 (devm_pci_alloc_host_bridge).
 *
 * 호출 체인:
 *   devm_pci_alloc_host_bridge() [probe.c] -> [이 함수]
 *     -> pci_parse_request_of_pci_ranges()
 */
int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge)
{
	/* [한국어] DT 노드가 없으면 DT 시스템이 아니다. */
	if (!dev->of_node)
		/* [한국어] 성공으로 돌아간다 — ACPI 경로에서도 이 코드가 무해하게 지나가야 한다. */
		return 0;

	/* [한국어] 브리지를 건널 때 INTA-D 가 어떻게 회전하는지 정한 PCI 표준 규칙.
	 * DT 와 무관하게 규격이 정한 것이라 고정 함수를 꽂는다. */
	bridge->swizzle_irq = pci_common_swizzle;
	/* [한국어] 회전 끝에 나온 핀을 실제 IRQ 로 바꾸는 함수. 이쪽은 DT 의 interrupt-map 을 읽는다. */
	bridge->map_irq = of_irq_parse_and_map_pci;

	/* [한국어] 주소 공간(ranges/dma-ranges)을 세운다. */
	return pci_parse_request_of_pci_ranges(dev, bridge);
}

/* [한국어] 여기부터 #endif 까지는 CONFIG_PCI_DYNAMIC_OF_NODES 가 켜졌을 때만
 * 컴파일되는 네 함수다 - 장치와 호스트 브리지 각각의 노드 생성/제거.
 *
 * 이 묶음만 데이터 흐름이 반대다. 파일의 나머지는 DT 를 읽어 PCI 자료구조를
 * 채우지만, 여기서는 열거로 알아낸 PCI 장치를 DT 에 써 넣는다. 속성을 실제로
 * 만드는 일은 of_property.c 가 하고, 이 파일은 노드를 만들어 붙이고 떼는
 * 껍데기와 수명 관리를 맡는다.
 *
 * 네 함수가 공통으로 쓰는 장치가 of_changeset 이다. DT 는 여러 서브시스템이
 * 동시에 들여다보는 자료구조라 반쯤 만들어진 노드가 보이면 안 되므로,
 * 변경을 changeset 에 모아 두었다가 한 번에 적용한다. 그리고 그 changeset
 * 포인터를 만든 노드의 data 필드에 숨겨 두어, 제거할 때 그대로 revert 한다. */
/* [한국어] 동적 DT 노드 생성이 빌드에 포함될 때만 아래 네 함수를 컴파일한다.
 * #endif 는 of_pci_make_host_bridge_node() 뒤에 있다. */
#ifdef CONFIG_PCI_DYNAMIC_OF_NODES

/* [한국어]
 * of_pci_remove_node - 동적으로 만든 장치 노드를 되돌린다
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음.
 *
 * of_pci_make_dev_node() 의 역이다. 노드가 없거나 OF_DYNAMIC 이 아니면
 * (즉 펌웨어가 준 진짜 노드면) 손대지 않는다.
 *
 * 되돌리는 방법이 노드를 직접 지우는 것이 아니라, make 때 np->data 에
 * 숨겨 둔 of_changeset 을 revert 하는 것이다. 생성 당시의 모든 변경을
 * 역순으로 취소하므로 속성 하나까지 빠짐없이 정리된다.
 *
 * 순서는 device 에서 떼기 -> changeset revert -> changeset 파괴 ->
 * 노드 참조 반납이다. 반대로 하면 이미 사라진 노드를 device 가 가리키게 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 제거).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/remove.c:125.
 *
 * 호출 체인:
 *   pci_destroy_dev() [remove.c] -> [이 함수] -> of_changeset_revert()
 */
void of_pci_remove_node(struct pci_dev *pdev)
{
	/* [한국어] 제거할 노드. */
	struct device_node *np;

	/* [한국어] 이 장치에 붙어 있는 DT 노드를 가져온다. */
	np = pci_device_to_OF_node(pdev);
	/* [한국어] 노드가 없거나 OF_DYNAMIC 이 아니면 — 즉 펌웨어가 준 진짜 노드면 — */
	if (!np || !of_node_check_flag(np, OF_DYNAMIC))
		/* [한국어] 손대지 않는다. 이 검사가 진짜 DT 를 실수로 지우는 것을 막는 안전장치다. */
		return;

	/* [한국어] 먼저 device 에서 노드를 뗀다. 반대로 하면 사라진 노드를 device 가 가리킨다. */
	device_remove_of_node(&pdev->dev);
	/* [한국어] 생성 당시의 모든 변경을 역순으로 취소한다. np->data 에 숨겨 둔
	 * changeset 이 그 기록이라 속성 하나까지 빠짐없이 정리된다. */
	of_changeset_revert(np->data);
	/* [한국어] changeset 자체를 파괴한다. */
	of_changeset_destroy(np->data);
	/* [한국어] 노드 참조를 반납한다. 이것이 0 이 되면 노드가 실제로 사라진다. */
	of_node_put(np);
}

/* [한국어]
 * of_pci_make_dev_node - 열거로 발견한 장치에 DT 노드를 런타임 생성한다
 *
 * @pdev: 대상 PCI 장치.
 * @return: 없음. 어느 단계에서 실패해도 조용히 물러난다.
 *
 * 방향이 이 파일의 나머지와 반대다. 보통은 DT 를 읽어 PCI 자료구조를
 * 채우지만, 여기서는 이미 열거로 알아낸 PCI 장치를 DT 에 등록한다.
 * 필요한 이유는 DT 로만 표현되는 결선 정보 - 이 장치의 어느 핀이 어느
 * GPIO 나 인터럽트 컨트롤러에 물려 있는지 - 를 다른 서브시스템이 찾을 수
 * 있게 하기 위해서다.
 *
 * 절차:
 *   1) 이미 노드가 있으면 할 일이 없다
 *   2) 부모가 될 노드를 정한다. 루트 버스면 버스의 노드, 아니면 위쪽
 *      P2P 브리지의 노드. 부모가 없으면 붙일 곳이 없어 물러난다
 *   3) 이름을 만든다. 브리지면 "pci@x,y", 아니면 "dev@x,y" 로 DT 관례를
 *      따르며 x 가 장치 번호, y 가 기능 번호다
 *   4) of_changeset 하나에 노드 생성과 속성 추가를 모아 원자적으로 적용한다.
 *      중간 상태가 다른 코드에 보이면 안 되기 때문이다
 *   5) 적용에 성공하면 changeset 포인터를 np->data 에 숨겨 둔다.
 *      나중에 of_pci_remove_node() 가 이것으로 되돌린다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 이 트리에서 확인한 호출자는 quirks.c 의 DECLARE_PCI_FIXUP_FINAL 항목들이다
 * (12589-12592: Xilinx 0x5020/0x5021, Red Hat 0x0005, EFAR 0x9660).
 * 즉 모든 장치가 아니라 DT 표현이 필요한 특정 장치에만 걸린다.
 *
 * 호출 체인:
 *   pci_fixup_device(final) -> [이 함수]
 *     -> of_changeset_create_node() -> of_pci_add_properties() [of_property.c]
 *     -> of_changeset_apply() -> device_add_of_node()
 */
void of_pci_make_dev_node(struct pci_dev *pdev)
{
	/* [한국어] ppnode 는 붙일 부모 노드, np 는 새로 만들 노드. */
	struct device_node *ppnode, *np = NULL;
	/* [한국어] 노드 이름의 앞부분. 브리지냐 아니냐로 갈린다. */
	const char *pci_type;
	/* [한국어] 노드 생성과 속성 추가를 원자적으로 모을 변경 집합. */
	struct of_changeset *cset;
	/* [한국어] 만들 노드의 이름 문자열. */
	const char *name;
	/* [한국어] 하위 호출 결과. */
	int ret;

	/*
	 * If there is already a device tree node linked to this device,
	 * return immediately.
	 */
	/* [한국어] 이미 노드가 붙어 있으면 */
	if (pci_device_to_OF_node(pdev))
		/* [한국어] 할 일이 없다. */
		return;

	/* Check if there is device tree node for parent device */
	/* [한국어] 루트 버스면 붙일 부모는 버스 자신의 노드, */
	if (!pdev->bus->self)
		/* [한국어] 그것을 쓴다. */
		ppnode = pdev->bus->dev.of_node;
	/* [한국어] 아니면 */
	else
		/* [한국어] 위쪽 P2P 브리지의 노드가 부모다. */
		ppnode = pdev->bus->self->dev.of_node;
	/* [한국어] 부모가 없으면 트리에 붙일 곳이 없다. */
	if (!ppnode)
		/* [한국어] 물러난다. */
		return;

	/* [한국어] 브리지인가. */
	if (pci_is_bridge(pdev))
		/* [한국어] DT 관례상 브리지 노드 이름은 "pci" 로 시작한다. */
		pci_type = "pci";
	/* [한국어] 아니면 */
	else
		/* [한국어] 일반 장치는 "dev" 다. */
		pci_type = "dev";

	/* [한국어] "pci@x,y" 또는 "dev@x,y" 형태로 이름을 만든다. */
	name = kasprintf(GFP_KERNEL, "%s@%x,%x", pci_type,
			 /* [한국어] x 가 장치 번호, y 가 기능 번호다. DT 의 단위 주소 표기 관례를 따른다. */
			 PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
	/* [한국어] 이름 할당 실패. */
	if (!name)
		/* [한국어] 물러난다. */
		return;

	/* [한국어] 변경 집합을 할당한다. */
	cset = kmalloc_obj(*cset);
	/* [한국어] 할당 실패. */
	if (!cset)
		/* [한국어] 이름만 반납하고 물러난다. */
		goto out_free_name;
	/* [한국어] 빈 변경 집합으로 초기화. */
	of_changeset_init(cset);

	/* [한국어] 부모 아래에 노드를 만든다. 아직 적용 전이라 트리에는 보이지 않는다. */
	np = of_changeset_create_node(cset, ppnode, name);
	/* [한국어] 만들지 못했다. */
	if (!np)
		/* [한국어] 변경 집합을 파괴하는 경로로. */
		goto out_destroy_cset;

	/* [한국어] of_property.c 가 reg, device_type 등 PCI 속성을 채운다.
	 * 방향이 이 파일의 나머지와 반대인 부분이다. */
	ret = of_pci_add_properties(pdev, cset, np);
	/* [한국어] 속성 추가 실패. */
	if (ret)
		/* [한국어] 노드 참조부터 반납하는 경로로. */
		goto out_free_node;

	/* [한국어] 여기서 비로소 모든 변경이 한 번에 트리에 반영된다.
	 * 중간 상태가 다른 서브시스템에 보이지 않게 하려는 것이다. */
	ret = of_changeset_apply(cset);
	/* [한국어] 적용 실패. */
	if (ret)
		/* [한국어] 같은 경로로. */
		goto out_free_node;

	/* [한국어] 변경 집합 포인터를 노드에 숨겨 둔다.
	 * of_pci_remove_node() 가 나중에 이것으로 되돌린다. */
	np->data = cset;

	/* [한국어] 만든 노드를 pci_dev 의 device 에 붙인다. */
	ret = device_add_of_node(&pdev->dev, np);
	/* [한국어] 붙이기 실패. */
	if (ret)
		/* [한국어] 적용까지 되돌려야 하는 경로로. */
		goto out_revert_cset;

	/* [한국어] 성공했으므로 임시로 만든 이름 문자열은 더 필요 없다.
	 * 노드가 이름을 복사해 갔기 때문이다. */
	kfree(name);

	/* [한국어] 성공. 반환값이 없어 실패해도 호출자는 알 수 없지만,
	 * 노드가 없다고 PCI 가 못 도는 것은 아니라 그렇게 설계됐다. */
	return;

/* [한국어] device_add_of_node 실패 — 적용된 변경까지 되돌린다. */
out_revert_cset:
	/* [한국어] 숨겨 둔 포인터를 먼저 지운다. 아래에서 파괴할 것이라 남겨 두면 dangling 이다. */
	np->data = NULL;
	/* [한국어] 트리에 반영된 변경을 취소한다. */
	of_changeset_revert(cset);
/* [한국어] 속성 추가 또는 적용 실패 — 노드 참조부터 반납한다. */
out_free_node:
	/* [한국어] 노드 참조 반납. */
	of_node_put(np);
/* [한국어] 노드 생성 실패 — 변경 집합만 정리하면 된다. */
out_destroy_cset:
	/* [한국어] 변경 집합 내용을 파괴하고 */
	of_changeset_destroy(cset);
	/* [한국어] 구조체 자체도 반납한다. */
	kfree(cset);
/* [한국어] 변경 집합 할당 실패 — 이름만 남아 있다. */
out_free_name:
	/* [한국어] 이름 문자열 반납. 되돌리기 경로가 여섯 단계인 이유는 어느 단계에서
	 * 실패했느냐에 따라 되돌릴 것이 다르기 때문이다. */
	kfree(name);
}

/* [한국어]
 * of_pci_remove_host_bridge_node - 동적으로 만든 호스트 브리지 노드를 되돌린다
 *
 * @bridge: 대상 호스트 브리지.
 * @return: 없음.
 *
 * of_pci_make_host_bridge_node() 의 역이다. 루트 버스에 매달린 노드를 찾아
 * OF_DYNAMIC 인지 확인하고, 맞으면 changeset 을 revert 한다.
 *
 * device_remove_of_node 를 두 번 부르는 것은 make 쪽이 브리지 device 와
 * 루트 버스 device 양쪽에 같은 노드를 붙였기 때문이다. 붙인 수만큼 떼야
 * 참조 계수가 맞는다.
 *
 * OF_DYNAMIC 검사가 안전장치다. 펌웨어가 제공한 진짜 DT 노드에는 이 표시가
 * 없으므로, 실수로 그것을 지울 일이 없다.
 * 실행 컨텍스트: 프로세스 컨텍스트(호스트 브리지 제거).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/remove.c:298.
 *
 * 호출 체인:
 *   pci_remove_root_bus() [remove.c] -> [이 함수] -> of_changeset_revert()
 */
void of_pci_remove_host_bridge_node(struct pci_host_bridge *bridge)
{
	/* [한국어] 제거할 노드. */
	struct device_node *np;

	/* [한국어] 루트 버스에 붙어 있는 노드를 가져온다. */
	np = pci_bus_to_OF_node(bridge->bus);
	/* [한국어] 노드가 없거나 동적으로 만든 것이 아니면 */
	if (!np || !of_node_check_flag(np, OF_DYNAMIC))
		/* [한국어] 손대지 않는다. */
		return;

	/* [한국어] 루트 버스 device 에서 뗀다. */
	device_remove_of_node(&bridge->bus->dev);
	/* [한국어] 호스트 브리지 device 에서도 뗀다. make 쪽이 양쪽에 같은 노드를 붙였으므로
	 * 붙인 수만큼 떼야 참조 계수가 맞는다. */
	device_remove_of_node(&bridge->dev);
	/* [한국어] 생성 당시의 변경을 역순으로 취소한다. */
	of_changeset_revert(np->data);
	/* [한국어] 변경 집합을 파괴한다. */
	of_changeset_destroy(np->data);
	/* [한국어] 노드 참조를 반납한다. */
	of_node_put(np);
}

/* [한국어]
 * of_pci_make_host_bridge_node - 루트 버스에 DT 노드가 없으면 만들어 붙인다
 *
 * @bridge: 대상 호스트 브리지.
 * @return: 없음. 실패해도 조용히 돌아간다 - 노드가 없다고 PCI 가 동작하지
 *          못하는 것은 아니기 때문이다.
 *
 * of_pci_make_dev_node() 의 호스트 브리지 판이다. ACPI 나 하드코딩으로
 * 발견된 호스트 브리지에는 DT 노드가 없는데, 그 아래 장치들에 노드를
 * 만들어 주려면 붙일 부모가 필요하다. 그래서 루트에 만든다.
 *
 * 절차:
 *   1) 루트 버스에 이미 노드가 있으면 할 일이 없다
 *   2) 버스에는 없는데 브리지 device 에만 있으면 상태가 어긋난 것이라
 *      오류를 찍고 물러난다
 *   3) DT 루트(of_root)가 아예 없는 시스템이면 붙일 곳이 없다
 *   4) "pci@<도메인>,<버스번호>" 이름으로 of_root 아래에 노드를 만들고
 *      of_pci_add_host_bridge_properties() [of_property.c] 로 속성을 채운다
 *   5) OF_POPULATED 를 세운다. of_root 바로 아래 노드는 platform 버스가
 *      훑어 platform_device 를 만들어 버리는데, 이 노드는 이미 존재하는
 *      호스트 브리지를 가리킬 뿐이라 새 장치가 생기면 안 된다
 *   6) changeset 을 적용하고 브리지 device 와 루트 버스 device 양쪽에 붙인다
 *
 * 되돌리기 경로가 여섯 단계로 나뉘어 있는 이유는 어느 단계에서 실패했느냐에
 * 따라 되돌릴 것이 다르기 때문이다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 이 트리에서 확인한 호출자: drivers/pci/probe.c:2795.
 *
 * 호출 체인:
 *   pci_register_host_bridge() [probe.c] -> [이 함수]
 *     -> of_pci_add_host_bridge_properties() [of_property.c] -> of_changeset_apply()
 */
void of_pci_make_host_bridge_node(struct pci_host_bridge *bridge)
{
	struct device_node *np = NULL;
	struct of_changeset *cset;
	/* [한국어] 만들 노드의 이름 문자열. */
	const char *name;
	/* [한국어] 하위 호출 결과. */
	int ret;

	/*
	 * If there is already a device tree node linked to the PCI bus handled
	 * by this bridge (i.e. the PCI root bus), nothing to do.
	 */
	/* [한국어] 루트 버스에 이미 노드가 있으면 */
	if (pci_bus_to_OF_node(bridge->bus))
		/* [한국어] 할 일이 없다. */
		return;

	/*
	 * The root bus has no node. Check that the host bridge has no node
	 * too
	 */
	/* [한국어] 버스에는 없는데 브리지 device 에만 노드가 있으면 상태가 어긋난 것이다. */
	if (bridge->dev.of_node) {
		/* [한국어] 오류를 남기고 */
		dev_err(&bridge->dev, "PCI host bridge of_node already set");
		/* [한국어] 물러난다. 억지로 만들면 노드가 둘이 되어 더 나빠진다. */
		return;
	}

	/* Check if there is a DT root node to attach the created node */
	/* [한국어] DT 루트가 아예 없는 시스템이면 붙일 곳이 없다. */
	if (!of_root) {
		/* [한국어] 디버그 로그만 남기고 */
		pr_debug("of_root node is NULL, cannot create PCI host bridge node\n");
		/* [한국어] 물러난다. ACPI 전용 시스템에서 흔한 경우라 오류 수준으로 찍지 않는다. */
		return;
	}

	/* [한국어] "pci@<도메인>,<버스번호>" 형태로 이름을 만든다. 호스트 브리지는
	 * 장치 번호/기능 번호가 없으므로 도메인과 버스 번호로 구분한다. */
	name = kasprintf(GFP_KERNEL, "pci@%x,%x", pci_domain_nr(bridge->bus),
			 bridge->bus->number);
	/* [한국어] 이름 할당 실패. */
	if (!name)
		/* [한국어] 물러난다. */
		return;

	/* [한국어] 변경 집합을 할당한다. */
	cset = kmalloc_obj(*cset);
	/* [한국어] 할당 실패. */
	if (!cset)
		/* [한국어] 이름만 반납하고 물러난다. */
		goto out_free_name;
	/* [한국어] 빈 변경 집합으로 초기화. */
	of_changeset_init(cset);

	/* [한국어] of_root 바로 아래에 노드를 만든다. 장치 노드와 달리 부모가 DT 루트다. */
	np = of_changeset_create_node(cset, of_root, name);
	/* [한국어] 만들지 못했다. */
	if (!np)
		/* [한국어] 변경 집합을 파괴하는 경로로. */
		goto out_destroy_cset;

	/* [한국어] of_property.c 가 호스트 브리지용 속성(ranges, bus-range 등)을 채운다. */
	ret = of_pci_add_host_bridge_properties(bridge, cset, np);
	/* [한국어] 속성 추가 실패. */
	if (ret)
		/* [한국어] 노드 참조부터 반납하는 경로로. */
		goto out_free_node;

	/*
	 * This of_node will be added to an existing device. The of_node parent
	 * is the root OF node and so this node will be handled by the platform
	 * bus. Avoid any new device creation.
	 */
	/* [한국어] of_root 바로 아래 노드는 platform 버스가 훑어 platform_device 를 만들어
	 * 버린다. 이 노드는 이미 존재하는 호스트 브리지를 가리킬 뿐이라 새 장치가
	 * 생기면 안 되므로, "이미 처리됨" 표시를 미리 세워 그 생성을 막는다. */
	of_node_set_flag(np, OF_POPULATED);
	/* [한국어] 이 fwnode 가 어느 device 를 가리키는지 연결해 둔다. */
	np->fwnode.dev = &bridge->dev;
	/* [한국어] 초기화가 끝났다고 알린다. 이것이 없으면 다른 코드가 이 노드를
	 * 아직 준비 중인 것으로 보고 프로브를 미룬다. */
	fwnode_dev_initialized(&np->fwnode, true);

	/* [한국어] 모든 변경을 한 번에 트리에 반영한다. */
	ret = of_changeset_apply(cset);
	/* [한국어] 적용 실패. */
	if (ret)
		/* [한국어] 노드 참조를 반납하는 경로로. */
		goto out_free_node;

	/* [한국어] 변경 집합 포인터를 노드에 숨겨 둔다.
	 * of_pci_remove_host_bridge_node() 가 이것으로 되돌린다. */
	np->data = cset;

	/* Add the of_node to host bridge and the root bus */
	/* [한국어] 호스트 브리지 device 에 붙인다. */
	ret = device_add_of_node(&bridge->dev, np);
	/* [한국어] 붙이기 실패. */
	if (ret)
		/* [한국어] 적용까지 되돌리는 경로로. */
		goto out_revert_cset;

	/* [한국어] 루트 버스 device 에도 같은 노드를 붙인다. 두 device 가 같은 노드를
	 * 공유하므로, 제거할 때도 두 번 떼야 한다. */
	ret = device_add_of_node(&bridge->bus->dev, np);
	/* [한국어] 둘째 붙이기 실패. */
	if (ret)
		/* [한국어] 첫째 붙이기부터 되돌리는 경로로. */
		goto out_remove_bridge_dev_of_node;

	/* [한국어] 성공했으니 임시 이름 문자열을 반납한다. 노드가 이름을 복사해 갔다. */
	kfree(name);

	/* [한국어] 성공. 반환값이 없어 호출자는 성패를 알 수 없다. */
	return;

/* [한국어] 루트 버스에 붙이다 실패한 경로. */
out_remove_bridge_dev_of_node:
	/* [한국어] 먼저 붙인 브리지 device 쪽을 뗀다. */
	device_remove_of_node(&bridge->dev);
/* [한국어] 브리지 device 에 붙이다 실패한 경로. */
out_revert_cset:
	/* [한국어] 숨겨 둔 포인터를 지운다. 아래에서 파괴하므로 남겨 두면 dangling 이다. */
	np->data = NULL;
	/* [한국어] 트리에 반영된 변경을 취소한다. */
	of_changeset_revert(cset);
/* [한국어] 속성 추가 또는 적용에 실패한 경로. */
out_free_node:
	/* [한국어] 노드 참조를 반납한다. */
	of_node_put(np);
/* [한국어] 노드 생성에 실패한 경로. */
out_destroy_cset:
	/* [한국어] 변경 집합 내용을 파괴하고 */
	of_changeset_destroy(cset);
	/* [한국어] 구조체도 반납한다. */
	kfree(cset);
/* [한국어] 변경 집합 할당에 실패한 경로. */
out_free_name:
	/* [한국어] 이름 문자열만 반납한다. */
	kfree(name);
}

/* [한국어] CONFIG_PCI_DYNAMIC_OF_NODES 블록 끝. */
#endif /* CONFIG_PCI_DYNAMIC_OF_NODES */

/* [한국어]
 * of_pci_supply_present - 이 노드에 전원 공급 속성이 하나라도 있는가
 *
 * @np: 검사할 DT 노드. NULL 이면 false.
 * @return: "-supply" 로 끝나는 속성이 하나라도 있으면 true.
 *
 * DT 규약에서 전원 레귤레이터는 "vpcie3v3-supply" 처럼 이름이 항상
 * "-supply" 로 끝난다. 어떤 레일이 있는지 미리 알 수 없으므로 이름을
 * 열거하는 대신 접미사로 판정한다.
 *
 * 구현이 접미사 검사인 이유를 짚어 둘 만하다. strrchr 로 마지막 '-' 를
 * 찾고 거기서부터 "-supply" 와 정확히 비교하므로, "supply" 로만 끝나는
 * 이름("power-supply-name" 같은)은 걸리지 않는다.
 *
 * 쓰임새는 "이 슬롯에 전원 컨트롤러를 붙일 것인가" 판단이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(DT 속성 목록 순회).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/pwrctrl/core.c:441, 449.
 *
 * 호출 체인:
 *   pwrctrl 코어 -> [이 함수] -> for_each_property_of_node()
 */
/**
 * of_pci_supply_present() - Check if the power supply is present for the PCI
 *				device
 * @np: Device tree node
 *
 * Check if the power supply for the PCI device is present in the device tree
 * node or not.
 *
 * Return: true if at least one power supply exists; false otherwise.
 */
bool of_pci_supply_present(struct device_node *np)
{
	/* [한국어] 속성 목록을 훑을 커서. */
	struct property *prop;
	/* [한국어] 이름에서 마지막 '-' 부터의 부분 문자열. */
	char *supply;

	/* [한국어] 노드가 없으면 볼 속성도 없다. */
	if (!np)
		/* [한국어] 없다고 답한다. */
		return false;

	/* [한국어] 이 노드의 모든 속성을 하나씩 본다. 어떤 레일이 있는지 미리 알 수 없어
	 * 이름을 열거하는 대신 접미사로 판정하기 때문이다. */
	for_each_property_of_node(np, prop) {
		/* [한국어] 마지막 '-' 위치를 찾는다. */
		supply = strrchr(prop->name, '-');
		/* [한국어] 거기서부터 정확히 "-supply" 인가. strcmp 라 "power-supply-name" 처럼
		 * 뒤에 뭔가 더 붙은 이름은 걸리지 않는다. */
		if (supply && !strcmp(supply, "-supply"))
			/* [한국어] 하나라도 있으면 전원 레일이 DT 에 정의돼 있다는 뜻이다. */
			return true;
	}

	/* [한국어] 끝까지 못 찾았다. */
	return false;
}
/* [한국어] 모듈에 공개. drivers/pci/pwrctrl/core.c 가 전원 컨트롤러를 붙일지 판단할 때 쓴다. */
EXPORT_SYMBOL_GPL(of_pci_supply_present);

/* [한국어] CONFIG_PCI 블록 끝. 아래 세 헬퍼는 이 밖에 있어 PCI 코어 없이도 쓸 수 있다. */
#endif /* CONFIG_PCI */

/* [한국어]
 * of_pci_get_max_link_speed - DT 의 max-link-speed 값을 그대로 읽어 준다
 *
 * @node: 링크를 기술한 DT 노드(보통 호스트 브리지나 포트).
 * @return: 속성값(1=2.5GT/s, 2=5GT/s, 3=8GT/s, 4=16GT/s ...). 속성이 없거나
 *          형식이 틀리면 of_property_read_u32() 의 음수 errno.
 *
 * 링크가 협상할 수 있는 최고 속도를 보드 쪽에서 낮춰 거는 수단이다.
 * 컨트롤러와 장치가 Gen4 를 지원해도 기판 배선이나 커넥터가 그 속도를
 * 버티지 못하면 링크가 오르내리며 불안정해지므로, 보드 설계자가 DT 에
 * 상한을 적어 둔다.
 *
 * 상류 주석이 "값을 검증하지 않는다" 고 못박아 두었다. 범위를 벗어난 값이
 * 들어와도 그대로 돌려주므로, 판단은 호출자인 컨트롤러 드라이버 몫이다.
 *
 * CONFIG_PCI 블록 바깥에 있다. PCI 컨트롤러 드라이버가 PCI 코어 없이도
 * 이 헬퍼만 쓰는 구성을 허용하기 위해서다.
 * 실행 컨텍스트: 프로세스 컨텍스트(대부분 probe).
 *
 * 이 트리에서 확인한 호출자: pcie-designware.c, pcie-brcmstb.c,
 * pci-mvebu.c, pcie-rockchip.c 등 다수의 컨트롤러 드라이버.
 *
 * 호출 체인:
 *   컨트롤러 probe -> [이 함수] -> of_property_read_u32()
 */
/**
 * of_pci_get_max_link_speed - Find the maximum link speed of the given device node.
 * @node: Device tree node with the maximum link speed information.
 *
 * This function will try to read the "max-link-speed" property of the given
 * device tree node. It does NOT validate the value of the property.
 *
 * Return: Maximum link speed value on success, errno on failure.
 */
int of_pci_get_max_link_speed(struct device_node *node)
{
	/* [한국어] 읽은 속도 값. */
	u32 max_link_speed;
	/* [한국어] 읽기 결과. */
	int ret;

	/* [한국어] 속성을 읽는다. 1=2.5GT/s, 2=5GT/s, 3=8GT/s, 4=16GT/s 식의 세대 번호다. */
	ret = of_property_read_u32(node, "max-link-speed", &max_link_speed);
	/* [한국어] 속성이 없거나 형식이 틀리다. */
	if (ret)
		/* [한국어] errno 를 그대로 전한다. */
		return ret;

	/* [한국어] 상류 주석이 못박았듯 값을 검증하지 않는다. 범위를 벗어난 값도
	 * 그대로 돌려주므로 판단은 호출자인 컨트롤러 드라이버 몫이다. */
	return max_link_speed;
}
/* [한국어] 모듈에 공개. pcie-designware.c, pcie-brcmstb.c 등 다수가 쓴다. */
EXPORT_SYMBOL_GPL(of_pci_get_max_link_speed);

/* [한국어]
 * of_pci_get_slot_power_limit - 슬롯 전력 상한을 읽어 PCIe 인코딩으로 바꾼다
 *
 * @node: 슬롯을 기술한 DT 노드.
 * @slot_power_limit_value: PCIe Slot Capabilities 의 Slot Power Limit Value
 *        (8비트)를 받을 곳. NULL 이면 채우지 않는다.
 * @slot_power_limit_scale: 같은 레지스터의 Slot Power Limit Scale(2비트).
 * @return: 실제로 표현 가능한 전력(mW). 속성이 없거나 0 이면 0.
 *
 * DT 에는 사람이 읽기 쉬운 밀리와트("slot-power-limit-milliwatt")로 적혀
 * 있지만, 하드웨어 레지스터는 값 8비트와 배율 2비트로 나뉜 형식을 쓴다.
 * 그 변환이 이 함수의 전부이며, 계단식 if 가 곧 인코딩 표다.
 *
 *   scale 3(x0.001) : 0.001W 단위. 최대 255mW
 *   scale 2(x0.01)  : 0.01W  단위. 최대 2.55W
 *   scale 1(x0.1)   : 0.1W   단위. 최대 25.5W
 *   scale 0(x1)     : 1W     단위. value 0..239 이 곧 0..239W
 * 여기까지가 원래 형식이고, 그 위는 PCIe 가 나중에 덧붙인 특수값이다.
 *   value 0xF0..0xFE : 250W 부터 25W 씩. 0xF0=250W, 0xFE=600W
 * 그래서 240~249W 는 표현할 수 없어 239W(0xEF)로 깎이고, 600W 를 넘으면
 * 600W 로 잘린다.
 *
 * 반환값이 입력과 다를 수 있다는 점이 중요하다. 표현 불가능한 값은 아래로
 * 내림되며, 호출자는 반환값을 "실제로 알릴 수 있는 전력" 으로 받아야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산이다.
 *
 * 호출 체인:
 *   호스트 브리지/슬롯 드라이버 -> [이 함수] -> of_property_read_u32()
 */
/**
 * of_pci_get_slot_power_limit - Parses the "slot-power-limit-milliwatt"
 *				 property.
 *
 * @node: device tree node with the slot power limit information
 * @slot_power_limit_value: pointer where the value should be stored in PCIe
 *			    Slot Capabilities Register format
 * @slot_power_limit_scale: pointer where the scale should be stored in PCIe
 *			    Slot Capabilities Register format
 *
 * Returns the slot power limit in milliwatts and if @slot_power_limit_value
 * and @slot_power_limit_scale pointers are non-NULL, fills in the value and
 * scale in format used by PCIe Slot Capabilities Register.
 *
 * If the property is not found or is invalid, returns 0.
 */
u32 of_pci_get_slot_power_limit(struct device_node *node,
				u8 *slot_power_limit_value,
				u8 *slot_power_limit_scale)
{
	/* [한국어] DT 에서 읽은 밀리와트 값. 아래에서 표현 가능한 값으로 깎이기도 한다. */
	u32 slot_power_limit_mw;
	/* [한국어] 레지스터에 넣을 값 8비트와 배율 2비트. */
	u8 value, scale;

	/* [한국어] 사람이 읽기 쉬운 밀리와트 단위로 적힌 속성을 읽는다. */
	if (of_property_read_u32(node, "slot-power-limit-milliwatt",
				 &slot_power_limit_mw))
		/* [한국어] 속성이 없으면 0 으로 본다 — 아래 첫 갈래가 이것을 받는다. */
		slot_power_limit_mw = 0;

	/* Calculate Slot Power Limit Value and Slot Power Limit Scale */
	/* [한국어] 0 이면 "제한 없음/미지정" 이다. */
	if (slot_power_limit_mw == 0) {
		/* [한국어] 값 0, */
		value = 0x00;
		/* [한국어] 배율 0. */
		scale = 0;
	/* [한국어] 255mW 이하 — 가장 정밀한 배율로 표현할 수 있는 범위. */
	} else if (slot_power_limit_mw <= 255) {
		/* [한국어] 1mW 단위이므로 값을 그대로 쓴다. */
		value = slot_power_limit_mw;
		/* [한국어] 배율 3 은 x0.001W, 즉 밀리와트 단위다. */
		scale = 3;
	/* [한국어] 2.55W 이하. */
	} else if (slot_power_limit_mw <= 255*10) {
		/* [한국어] 10mW 단위로 나눠 담는다. */
		value = slot_power_limit_mw / 10;
		/* [한국어] 배율 2 는 x0.01W. */
		scale = 2;
		/* [한국어] 실제로 표현되는 값으로 되돌려 준다. 나눗셈에서 버려진 나머지만큼
		 * 반환값이 입력보다 작아진다. */
		slot_power_limit_mw = slot_power_limit_mw / 10 * 10;
	/* [한국어] 25.5W 이하. */
	} else if (slot_power_limit_mw <= 255*100) {
		/* [한국어] 100mW 단위. */
		value = slot_power_limit_mw / 100;
		/* [한국어] 배율 1 은 x0.1W. */
		scale = 1;
		/* [한국어] 표현 가능한 값으로 내림. */
		slot_power_limit_mw = slot_power_limit_mw / 100 * 100;
	/* [한국어] 239W 이하 — 여기까지가 원래 형식의 마지막 구간이다. */
	} else if (slot_power_limit_mw <= 239*1000) {
		/* [한국어] 1W 단위. value 0..239 가 곧 0..239W 다. */
		value = slot_power_limit_mw / 1000;
		/* [한국어] 배율 0 은 x1W. */
		scale = 0;
		/* [한국어] 표현 가능한 값으로 내림. */
		slot_power_limit_mw = slot_power_limit_mw / 1000 * 1000;
	/* [한국어] 240~249W — 표현할 방법이 없는 구멍이다. */
	} else if (slot_power_limit_mw < 250*1000) {
		/* [한국어] 0xEF(=239)로 깎는다. 0xF0 부터는 아래의 특수 인코딩이 시작되기 때문이다. */
		value = 0xEF;
		scale = 0;
		/* [한국어] 그래서 실제로 알리는 값도 239W 로 되돌려 준다. */
		slot_power_limit_mw = 239*1000;
	/* [한국어] 250~600W — PCIe 가 나중에 덧붙인 특수 인코딩 구간. */
	} else if (slot_power_limit_mw <= 600*1000) {
		/* [한국어] 0xF0 이 250W 이고 25W 씩 올라간다. 0xFE 가 600W 다. */
		value = 0xF0 + (slot_power_limit_mw / 1000 - 250) / 25;
		scale = 0;
		/* [한국어] 25W 단위로 내림해 실제 표현값을 맞춘다. */
		slot_power_limit_mw = slot_power_limit_mw / (1000*25) * (1000*25);
	/* [한국어] 600W 초과. */
	} else {
		/* [한국어] 표현할 수 있는 최대값 0xFE. */
		value = 0xFE;
		scale = 0;
		/* [한국어] 600W 로 자른다. */
		slot_power_limit_mw = 600*1000;
	}

	/* [한국어] 호출자가 값을 원했으면 */
	if (slot_power_limit_value)
		/* [한국어] 채워 준다. */
		*slot_power_limit_value = value;

	/* [한국어] 배율을 원했으면 */
	if (slot_power_limit_scale)
		/* [한국어] 채워 준다. */
		*slot_power_limit_scale = scale;

	/* [한국어] 실제로 표현 가능한 전력을 돌려준다. 입력과 다를 수 있으므로
	 * 호출자는 이 값을 "실제로 알릴 수 있는 전력" 으로 받아야 한다. */
	return slot_power_limit_mw;
}
/* [한국어] 모듈에 공개. */
EXPORT_SYMBOL_GPL(of_pci_get_slot_power_limit);

/* [한국어]
 * of_pci_get_equalization_presets - 링크 이퀄라이제이션 프리셋 표를 DT 에서 읽는다
 *
 * @dev:       속성을 가진 장치(보통 호스트 브리지). dev->of_node 를 본다.
 * @presets:   읽은 값을 담을 구조체.
 * @num_lanes: 이 링크의 최대 레인 수. 레인마다 값이 하나씩 필요하다.
 * @return: 0 성공(속성이 없어도 0). 속성이 있는데 형식이 틀리면 음수 errno.
 *
 * 8GT/s(Gen3) 이상에서 송신기는 수신 쪽 보드 특성에 맞춰 파형을 미리
 * 왜곡(pre-emphasis/de-emphasis)해 보내야 신호가 살아남는다. 그 왜곡 방식을
 * 번호로 고른 것이 프리셋이고, 보드 설계자가 측정해 DT 에 적어 둔다.
 *
 * 속도마다 인코딩이 다르다. 8GT/s 는 프리셋 값이 16비트라 u16 배열로 읽고
 * ("eq-presets-8gts"), 16/32/64GT/s 는 8비트라 u8 배열로 읽는다. 뒤쪽 이름은
 * 8 << (i+1) 로 만들어 내므로 i=0,1,2 가 각각 16, 32, 64 가 된다.
 *
 * 각 배열의 0번 원소를 먼저 PCI_EQ_RESV 로 채워 두는 것이 요령이다. 속성이
 * 없으면 of_property_read 계열이 -EINVAL 을 주고 배열을 건드리지 않으므로,
 * 그 표시가 남아 "이 속도는 DT 에 지정이 없다" 를 뜻하게 된다. 그래서
 * -EINVAL 만은 에러로 보지 않고 넘어간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(호스트 브리지 probe).
 *
 * 이 트리에서 확인한 호출자: drivers/pci/controller/dwc/pcie-designware-host.c:1572
 * (DesignWare 호스트가 pp->presets 를 채운다).
 *
 * 호출 체인:
 *   호스트 브리지 probe -> [이 함수] -> of_property_read_u16_array / u8_array
 */
/**
 * of_pci_get_equalization_presets - Parses the "eq-presets-Ngts" property.
 *
 * @dev: Device containing the properties.
 * @presets: Pointer to store the parsed data.
 * @num_lanes: Maximum number of lanes supported.
 *
 * If the property is present, read and store the data in the @presets structure.
 * Else, assign a default value of PCI_EQ_RESV.
 *
 * Return: 0 if the property is not available or successfully parsed else
 * errno otherwise.
 */
int of_pci_get_equalization_presets(struct device *dev,
				    struct pci_eq_presets *presets,
				    int num_lanes)
{
	/* [한국어] "eq-presets-64gts" 가 가장 긴 이름이라 20바이트면 넉넉하다. */
	char name[20];
	/* [한국어] 읽기 결과. */
	int ret;

	/* [한국어] 먼저 예약값으로 채워 둔다. 속성이 없으면 아래 읽기가 배열을 건드리지
	 * 않으므로 이 표시가 남아 "이 속도는 DT 지정이 없다" 를 뜻하게 된다. */
	presets->eq_presets_8gts[0] = PCI_EQ_RESV;
	/* [한국어] 8GT/s 프리셋은 값이 16비트라 u16 배열로 읽는다. */
	ret = of_property_read_u16_array(dev->of_node, "eq-presets-8gts",
					 /* [한국어] 레인마다 하나씩 필요하므로 num_lanes 개를 읽는다. */
					 presets->eq_presets_8gts, num_lanes);
	/* [한국어] -EINVAL(속성 없음)은 정상이고, 그 밖의 오류만 진짜 실패다. */
	if (ret && ret != -EINVAL) {
		/* [한국어] 어느 속성에서 무슨 오류가 났는지 남기고 */
		dev_err(dev, "Error reading eq-presets-8gts: %d\n", ret);
		/* [한국어] 위로 전한다. */
		return ret;
	}

	/* [한국어] 16/32/64GT/s 를 차례로 처리한다. EQ_PRESET_TYPE_MAX 에서 1 을 빼는 것은
	 * 8GT/s 를 위에서 따로 처리했기 때문이다. */
	for (int i = 0; i < EQ_PRESET_TYPE_MAX - 1; i++) {
		/* [한국어] 이 속도의 배열도 예약값으로 표시해 둔다. */
		presets->eq_presets_Ngts[i][0] = PCI_EQ_RESV;
		/* [한국어] 8 << (i+1) 이 i=0,1,2 에 대해 16, 32, 64 를 만든다.
		 * 그것을 이름에 넣어 "eq-presets-16gts" 같은 속성 이름을 조립한다. */
		snprintf(name, sizeof(name), "eq-presets-%dgts", 8 << (i + 1));
		/* [한국어] 16GT/s 이상은 프리셋 값이 8비트라 u8 배열로 읽는다. */
		ret = of_property_read_u8_array(dev->of_node, name,
						/* [한국어] 이 속도용 배열에 채운다. */
						presets->eq_presets_Ngts[i],
						/* [한국어] 레인 수만큼. */
						num_lanes);
		/* [한국어] 속성 없음(-EINVAL)이 아닌 오류만 실패로 본다. */
		if (ret && ret != -EINVAL) {
			/* [한국어] 어느 속성인지 이름과 함께 남기고 */
			dev_err(dev, "Error reading %s: %d\n", name, ret);
			/* [한국어] 위로 전한다. */
			return ret;
		}
	}

	/* [한국어] 속성이 하나도 없어도 성공이다 — 그 경우 배열에 예약값이 남는다. */
	return 0;
}
/* [한국어] 모듈에 공개. pcie-designware-host.c 가 pp->presets 를 채울 때 쓴다. */
EXPORT_SYMBOL_GPL(of_pci_get_equalization_presets);
