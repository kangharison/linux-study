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
 */

#define pr_fmt(fmt)	"PCI: OF: " fmt /* NVMe: 이 모듈의 pr_xxx 메시지에 'PCI: OF: ' 프리픽스 추가 */

#include <linux/cleanup.h> /* NVMe: 자동 정리 매크로 지원 */
#include <linux/irqdomain.h> /* NVMe: 인터럽트 domain 관련 헤더, MSI domain 조회에 사용 */
#include <linux/kernel.h> /* NVMe: 커널 기본 헤더 */
#include <linux/pci.h> /* NVMe: PCI 핵심 구조체(pci_dev, pci_bus, pci_host_bridge 등) 정의 */
#include <linux/of.h> /* NVMe: Open Firmware / Device Tree 핵심 헤더 */
#include <linux/of_irq.h> /* NVMe: OF 인터럽트 파싱 헤더 */
#include <linux/of_address.h> /* NVMe: OF 주소 변환 헤더 */
#include <linux/of_pci.h> /* NVMe: OF PCI 관련 헬퍼와 구조체 정의 */
#include <linux/platform_device.h> /* NVMe: platform bus device 검색용 */
#include "pci.h" /* NVMe: PCI 서브시스템 내부 헤더(pci.h) */

#ifdef CONFIG_PCI /* NVMe: PCI 서브시스템이 활성화된 경우에만 컴파일 */
/*
 * NVMe: pci_dev 구조체의 of_node를 DT에서 찾아 연결한다.
 *       NVMe SSD의 pci_dev가 생성될 때 호출되어, 이후 DT 기반 인터럽트/전력/
 *       링크 속도 등의 속성을 읽을 수 있게 한다.
 */
/**
 * pci_set_of_node - Find and set device's DT device_node
 * @dev: the PCI device structure to fill
 *
 * Returns 0 on success with of_node set or when no device is described in the
 * DT. Returns -ENODEV if the device is present, but disabled in the DT.
 */
int pci_set_of_node(struct pci_dev *dev) /* NVMe: pci_dev에 DT of_node를 설정하는 함수 시작 */
{ /* NVMe: 함수/블록 본문 시작 */
	if (!dev->bus->dev.of_node) /* NVMe: 상위 버스에 of_node가 없으면 DT 매핑 불필요, 바로 리턴 */
		return 0; /* NVMe: of_node 없음으로 정상 완료 */

	struct device_node *node __free(device_node) = /* NVMe: DT에서 이 pci_dev의 devfn에 해당하는 자식 노드를 검색, __free로 자동 해제 */
		of_pci_find_child_device(dev->bus->dev.of_node, dev->devfn); /* NVMe: of_pci_find_child_device() 함수 호출 */
	if (!node) /* NVMe: DT에 해당 장치가 없으면 매핑할 필요 없음 */
		return 0; /* NVMe: 정상 완료 반환 */

	struct device *pdev __free(put_device) = /* NVMe: platform bus에서 동일 of_node를 가진 device가 이미 있는지 확인 */
		bus_find_device_by_of_node(&platform_bus_type, node); /* NVMe: bus_find_device_by_of_node() 함수 호출 */
	if (pdev) /* NVMe: platform device가 동일 of_node를 사용 중이면 재사용 플래그 설정 */
		dev->bus->dev.of_node_reused = true; /* NVMe: 플래그 멤버 설정 */

	device_set_node(&dev->dev, of_fwnode_handle(no_free_ptr(node))); /* NVMe: pci_dev의 device에 of_node를 연결, 참조 카운트 이전 */
	return 0; /* NVMe: of_node 설정 완료 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: pci_dev가 사용하던 of_node 참조 카운트를 해제하고 연결을 끊는다.
 *       NVMe 장치가 제거(hotplug/제거)될 때 메모리 누수를 방지한다.
 */
void pci_release_of_node(struct pci_dev *dev) /* NVMe: pci_release_of_node() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	of_node_put(dev->dev.of_node); /* NVMe: pci_dev의 of_node 참조 카운트 감소 */
	device_set_node(&dev->dev, NULL); /* NVMe: device 구조체의 of_node 포인터를 NULL로 초기화 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: pci_bus(버스) 객체의 of_node를 설정한다.
 *       root bus(PHB)이면 pcibios_get_phb_of_node()로,
 *       하위 버스이면 상위 P2P bridge의 of_node를 상속받는다.
 *       'external-facing' 속성은 외부 접근 가능 슬롯(예: NVMe hotplug slot) 표시.
 */
void pci_set_bus_of_node(struct pci_bus *bus) /* NVMe: pci_set_bus_of_node() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *node; /* NVMe: DT 노드 포인터 선언 */

	if (bus->self == NULL) { /* NVMe: root bus(PHB)인 경우 */
		node = pcibios_get_phb_of_node(bus); /* NVMe: 아키텍처별 PHB of_node 획득 */
	} else { /* NVMe: 이전 조건이 아닌 경우 else 분기 */
		node = of_node_get(bus->self->dev.of_node); /* NVMe: 하위 버스이면 상위 P2P bridge의 of_node를 상속 */
		if (node && of_property_read_bool(node, "external-facing")) /* NVMe: 'external-facing' 속성이 있으면 외부 슬롯으로 표시 */
			bus->self->external_facing = true; /* NVMe: 플래그 멤버 설정 */
	} /* NVMe: 함수/블록 본문 끝 */

	device_set_node(&bus->dev, of_fwnode_handle(node)); /* NVMe: bus device에 of_node 설정 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: pci_bus의 of_node 참조를 해제한다.
 *       버스 제거 시 호출되어 NVMe가 탑재된 버스 트리의 DT 연결을 정리.
 */
void pci_release_bus_of_node(struct pci_bus *bus) /* NVMe: pci_release_bus_of_node() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	of_node_put(bus->dev.of_node); /* NVMe: bus의 of_node 참조 해제 */
	device_set_node(&bus->dev, NULL); /* NVMe: bus device의 of_node 포인터 클리어 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: PCI host bridge(PHB)에 해당하는 DT node를 반환한다.
 *       __weak로 정의되어 있어 아키텍처/플랫폼별로 재정의 가능하다.
 *       NVMe가 연결된 root bus의 bridge device에서 of_node를 찾는다.
 */
struct device_node * __weak pcibios_get_phb_of_node(struct pci_bus *bus) /* NVMe: device_node() 함수 정의/선언 */
{ /* NVMe: PHB 전용으로만 호출되어야 함, 아키텍처별 재정의 가능(weak) */
	/* This should only be called for PHBs */
	if (WARN_ON(bus->self || bus->parent)) /* NVMe: root bus가 아닌데 호출되면 경고 후 NULL 반환 */
		return NULL; /* NVMe: NULL 반환 */

	/*
	 * Look for a node pointer in either the intermediary device we
	 * create above the root bus or its own parent. Normally only
	 * the later is populated.
	 */
	if (bus->bridge->of_node) /* NVMe: bridge device 자체에 of_node가 있으면 반환 */
		return of_node_get(bus->bridge->of_node); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
	if (bus->bridge->parent && bus->bridge->parent->of_node) /* NVMe: bridge 부모에 of_node가 있으면 반환(일반적인 경우) */
		return of_node_get(bus->bridge->parent->of_node); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
	return NULL; /* NVMe: NULL 반환 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: host bridge에 연결된 MSI domain을 DT에서 찾는다.
 *       NVMe의 MSI-X 인터럽트는 이 domain을 통해 Linux virq로 매핑된다.
 *       CONFIG_IRQ_DOMAIN이 꺼져 있으면 MSI를 사용할 수 없다.
 */
struct irq_domain *pci_host_bridge_of_msi_domain(struct pci_bus *bus) /* NVMe: 함수 호출 또는 조건/선언의 일부 */
{ /* NVMe: 함수/블록 본문 시작 */
#ifdef CONFIG_IRQ_DOMAIN /* NVMe: IRQ domain 지원 시에만 MSI domain 조회 */
	struct irq_domain *d; /* NVMe: *d 포인터 변수 선언 */

	if (!bus->dev.of_node) /* NVMe: bus에 of_node가 없으면 MSI domain 없음 */
		return NULL; /* NVMe: NULL 반환 */

	/* Start looking for a phandle to an MSI controller. */
	d = of_msi_get_domain(&bus->dev, bus->dev.of_node, DOMAIN_BUS_PCI_MSI); /* NVMe: 'msi-parent' phandle로 MSI controller domain 획득 시도 */
	if (d) /* NVMe: 조건 검사 */
		return d; /* NVMe: MSI domain 찾으면 반환 */

	/*
	 * If we don't have an msi-parent property, look for a domain
	 * directly attached to the host bridge.
	 */
	d = irq_find_matching_host(bus->dev.of_node, DOMAIN_BUS_PCI_MSI); /* NVMe: msi-parent 없으면 host bridge에 직접 연결된 domain 검색 */
	if (d) /* NVMe: 조건 검사 */
		return d; /* NVMe: 값 반환 */

	return irq_find_host(bus->dev.of_node); /* NVMe: 마지막으로 irq_find_host()로 domain 탐색 */
#else
	return NULL; /* NVMe: CONFIG_IRQ_DOMAIN 미설정 시 MSI domain 없음 */
#endif
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: DT에 'msi-map' 속성이 있는지 확인한다.
 *       msi-map은 MSI controller와 PCI 장치 간 MSI 번호 매핑을 정의하며,
 *       NVMe MSI-X 경로 설정에 영향을 준다.
 */
bool pci_host_of_has_msi_map(struct device *dev) /* NVMe: pci_host_of_has_msi_map() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	if (dev && dev->of_node) /* NVMe: device와 of_node가 유효한지 확인 */
		return of_get_property(dev->of_node, "msi-map", NULL); /* NVMe: 'msi-map' 속성 존재 여부 반환 */
	return false; /* NVMe: false 반환 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: DT 노드의 reg 속성에서 devfn을 추출하여 기대하는 devfn과 비교한다.
 *       NVMe 장치의 BDF(Bus/Device/Function)가 DT 항목과 일치하는지 검증.
 */
static inline int __of_pci_pci_compare(struct device_node *node, /* NVMe: __of_pci_pci_compare() 인라인 비교 헬퍼 함수 선언 */
				       unsigned int data) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
{ /* NVMe: 함수/블록 본문 시작 */
	int devfn; /* NVMe: DT 노드에서 devfn 추출 */

	devfn = of_pci_get_devfn(node); /* NVMe: devfn에 함수 호출 결과 저장 */
	if (devfn < 0) /* NVMe: devfn 파싱 실패 시 불일치로 처리 */
		return 0; /* NVMe: 정상 완료 반환 */

	return devfn == data; /* NVMe: 추출한 devfn과 찾고자 하는 devfn 비교 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: 부모 DT 노드 아래에서 특정 devfn에 해당하는 자식 노드를 찾는다.
 *       'multifunc-device' 가짜 루트 아래의 다기능 NVMe/PCI 장치도 탐색.
 */
struct device_node *of_pci_find_child_device(struct device_node *parent, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
					     unsigned int devfn) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *node, *node2; /* NVMe: 순회용 DT 노드 포인터 2개 */

	for_each_child_of_node(parent, node) { /* NVMe: 부모 노드의 모든 자식 노드를 순회 */
		if (__of_pci_pci_compare(node, devfn)) /* NVMe: devfn이 일치하면 해당 노드 반환 */
			return node; /* NVMe: 값 반환 */
		/*
		 * Some OFs create a parent node "multifunc-device" as
		 * a fake root for all functions of a multi-function
		 * device we go down them as well.
		 */
		if (of_node_name_eq(node, "multifunc-device")) { /* NVMe: 다기능 장치용 가짜 루트 'multifunc-device' 처리 */
			for_each_child_of_node(node, node2) { /* NVMe: 가짜 루트 아래 자식 노드들도 순회 */
				if (__of_pci_pci_compare(node2, devfn)) { /* NVMe: 조건이 참이면 블록 실행 */
					of_node_put(node); /* NVMe: 일치하는 다기능 자식 노드 발견 시 부모 참조 해제 후 반환 */
					return node2; /* NVMe: 값 반환 */
				} /* NVMe: 함수/블록 본문 끝 */
			} /* NVMe: 함수/블록 본문 끝 */
		} /* NVMe: 함수/블록 본문 끝 */
	} /* NVMe: 함수/블록 본문 끝 */
	return NULL; /* NVMe: 일치하는 노드 없음 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_pci_find_child_device); /* NVMe: of_pci_find_child_device 심볼을 모듈 외부에 노출 */

/*
 * NVMe: DT 노드의 5-cell reg 속성을 파싱해 devfn(8bit)을 반환한다.
 *       PCI_SLOT()/PCI_FUNC() 매크로로 장치/기능 번호 분리 가능.
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
int of_pci_get_devfn(struct device_node *np) /* NVMe: of_pci_get_devfn() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	u32 reg[5]; /* NVMe: 5개 u32로 구성된 PCI reg 셀 저장 배열 */
	int error; /* NVMe: error 저장 변수 선언 */

	error = of_property_read_u32_array(np, "reg", reg, ARRAY_SIZE(reg)); /* NVMe: DT 'reg' 속성에서 5개 u32 읽기 */
	if (error) /* NVMe: 조건 검사 */
		return error; /* NVMe: 읽기 실패 시 에러 코드 반환 */

	return (reg[0] >> 8) & 0xff; /* NVMe: reg[0]의 상위 8bit에서 devfn 추출(버스는 하위 8bit) */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_pci_get_devfn); /* NVMe: of_pci_get_devfn 심볼을 모듈 외부에 노출 */

/*
 * NVMe: DT의 'bus-range' 속성을 읽어 PCI 버스 번호 범위를 struct resource로 변환.
 *       host bridge 뒤의 NVMe가 속할 secondary/subordinate bus 범위를 결정.
 */
/**
 * of_pci_parse_bus_range() - parse the bus-range property of a PCI device
 * @node: device node
 * @res: address to a struct resource to return the bus-range
 *
 * Returns 0 on success or a negative error-code on failure.
 */
static int of_pci_parse_bus_range(struct device_node *node, /* NVMe: 정적 함수 정의/선언 */
				  struct resource *res) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
{ /* NVMe: 함수/블록 본문 시작 */
	u32 bus_range[2]; /* NVMe: 'bus-range' 속성의 2개 u32 저장 배열 */
	int error; /* NVMe: error 저장 변수 선언 */

	error = of_property_read_u32_array(node, "bus-range", bus_range, /* NVMe: DT에서 'bus-range' 속성 읽기 */
					   ARRAY_SIZE(bus_range)); /* NVMe: ARRAY_SIZE() 함수 호출 */
	if (error) /* NVMe: 읽기 실패 시 에러 반환 */
		return error; /* NVMe: 에러 코드 반환 */

	res->name = node->name; /* NVMe: 리소스 이름을 노드 이름으로 설정 */
	res->start = bus_range[0]; /* NVMe: 버스 범위 시작 번호 */
	res->end = bus_range[1]; /* NVMe: 버스 범위 끝 번호 */
	res->flags = IORESOURCE_BUS; /* NVMe: 리소스 타입을 bus로 표시 */

	return 0; /* NVMe: 정상 완료 반환 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: DT 노드의 'linux,pci-domain' 속성에서 PCI domain 번호를 읽는다.
 *       다중 PCI segment 시스템에서 NVMe 장치가 속한 segment(domain) 식별에 사용.
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
int of_get_pci_domain_nr(struct device_node *node) /* NVMe: of_get_pci_domain_nr() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	u32 domain; /* NVMe: domain 번호 저장 변수 */
	int error; /* NVMe: error 저장 변수 선언 */

	error = of_property_read_u32(node, "linux,pci-domain", &domain); /* NVMe: 'linux,pci-domain' 속성 읽기 */
	if (error) /* NVMe: 조건 검사 */
		return error; /* NVMe: 속성 읽기 실패 시 에러 반환 */

	return (u16)domain; /* NVMe: domain 번호를 16bit로 클램핑 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_get_pci_domain_nr); /* NVMe: of_get_pci_domain_nr 심볼을 모듈 외부에 노출 */

/*
 * NVMe: 'linux,pci-probe-only' 속성이 있으면 true를 반환한다.
 *       true이면 firmware가 이미 구성한 BAR/bridge window를 커널이 재구성하지 않으므로,
 *       NVMe BAR 접근 방식에 영향을 준다.
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
bool of_pci_preserve_config(struct device_node *node) /* NVMe: of_pci_preserve_config() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	u32 val = 0; /* NVMe: 속성 값 저장 변수, 기본 0 */
	int ret; /* NVMe: ret 저장 변수 선언 */

	if (!node) { /* NVMe: 노드가 NULL이면 of_chosen(선택 노드)에서 재시도 */
		pr_warn("device node is NULL, trying with of_chosen\n"); /* NVMe: pr_warn() 함수 호출 */
		node = of_chosen; /* NVMe: node 변수에 값 할당 */
	} /* NVMe: 함수/블록 본문 끝 */

retry: /* NVMe: retry 레이블 */
	ret = of_property_read_u32(node, "linux,pci-probe-only", &val); /* NVMe: 'linux,pci-probe-only' 속성 읽기 */
	if (ret) { /* NVMe: 조건이 참이면 블록 실행 */
		if (ret == -ENODATA || ret == -EOVERFLOW) { /* NVMe: 속성은 있지만 값이 잘못된 경우 경고 후 false */
			pr_warn("Incorrect value for linux,pci-probe-only in %pOF, ignoring\n", /* NVMe: 진단/로그 메시지 출력 */
				node); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
			return false; /* NVMe: false 반환 */
		} /* NVMe: 함수/블록 본문 끝 */
		if (ret == -EINVAL) { /* NVMe: 속성이 없는 경우 */
			if (node == of_chosen) /* NVMe: 이미 of_chosen까지 검색했으면 false */
				return false; /* NVMe: false 반환 */

			node = of_chosen; /* NVMe: of_chosen으로 전환 후 재시도 */
			goto retry; /* NVMe: retry 레이블로 이동 */
		} /* NVMe: 함수/블록 본문 끝 */
	} /* NVMe: 함수/블록 본문 끝 */

	if (val) /* NVMe: 속성 값이 0이 아니면 true */
		return true; /* NVMe: true 반환 */
	else /* NVMe: if 조건 불만족 시 실행 */
		return false; /* NVMe: false 반환 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: 'linux,pci-probe-only'에 따라 PCI_PROBE_ONLY 플래그를 설정/해제한다.
 *       설정되면 NVMe 포함 모든 PCI 장치의 리소스를 firmware 구성 그대로 사용.
 */
/**
 * of_pci_check_probe_only - Setup probe only mode if linux,pci-probe-only
 *                           is present and valid
 */
void of_pci_check_probe_only(void) /* NVMe: of_pci_check_probe_only() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	if (of_pci_preserve_config(of_chosen)) /* NVMe: of_chosen 노드의 'linux,pci-probe-only' 확인 */
		pci_add_flags(PCI_PROBE_ONLY); /* NVMe: 속성 있으면 PCI_PROBE_ONLY 플래그 설정 */
	else /* NVMe: if 조건 불만족 시 실행 */
		pci_clear_flags(PCI_PROBE_ONLY); /* NVMe: 속성 없으면 PCI_PROBE_ONLY 플래그 해제 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_pci_check_probe_only); /* NVMe: of_pci_check_probe_only 심볼을 모듈 외부에 노출 */

/*
 * NVMe: DT의 'ranges'/'dma-ranges'를 파싱해 host bridge 리소스 목록을 구성한다.
 *       NVMe가 사용할 MEM/IO/DMA(inbound) 공간을 DT에서 추출하는 핵심 함수.
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
static int devm_of_pci_get_host_bridge_resources(struct device *dev, /* NVMe: 정적 함수 정의/선언 */
			struct list_head *resources, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
			struct list_head *ib_resources, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
			resource_size_t *io_base) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *dev_node = dev->of_node; /* NVMe: host bridge device의 of_node */
	struct resource *res, tmp_res; /* NVMe: 리소스 포인터와 임시 리소스 */
	struct resource *bus_range; /* NVMe: 버스 번호 범위 리소스 */
	struct of_pci_range range; /* NVMe: DT range 파싱 결과 구조체 */
	struct of_pci_range_parser parser; /* NVMe: range 파서 상태 구조체 */
	const char *range_type; /* NVMe: range 타입 문자열(IO/MEM/err) */
	int err; /* NVMe: 에러 코드 */

	if (io_base) /* NVMe: io_base가 주어지면 초기값을 OF_BAD_ADDR로 설정 */
		*io_base = (resource_size_t)OF_BAD_ADDR;

	bus_range = devm_kzalloc(dev, sizeof(*bus_range), GFP_KERNEL); /* NVMe: 버스 범위 리소스 메모리 할당 */
	if (!bus_range) /* NVMe: 조건 검사 */
		return -ENOMEM; /* NVMe: 메모리 할당 실패 */

	dev_info(dev, "host bridge %pOF ranges:\n", dev_node); /* NVMe: host bridge DT 노드와 ranges 파싱 시작 로그 */

	err = of_pci_parse_bus_range(dev_node, bus_range); /* NVMe: DT 'bus-range' 파싱 */
	if (err) { /* NVMe: 조건이 참이면 블록 실행 */
		bus_range->start = 0; /* NVMe: 'bus-range' 없으면 기본 0~0xff 사용 */
		bus_range->end = 0xff; /* NVMe: 구조체 멤버에 값 할당 */
		bus_range->flags = IORESOURCE_BUS; /* NVMe: 구조체 멤버에 값 할당 */
	} else { /* NVMe: 이전 조건이 아닌 경우 else 분기 */
		if (bus_range->end > 0xff) { /* NVMe: 'bus-range' 끝이 0xff를 넘으면 0xff로 클램핑 */
			dev_warn(dev, "  Invalid end bus number in %pR, defaulting to 0xff\n", /* NVMe: 진단/로그 메시지 출력 */
				 bus_range); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
			bus_range->end = 0xff; /* NVMe: 구조체 멤버에 값 할당 */
		} /* NVMe: 함수/블록 본문 끝 */
	} /* NVMe: 함수/블록 본문 끝 */
	pci_add_resource(resources, bus_range); /* NVMe: 버스 범위를 host bridge 리소스 목록에 추가 */

	/* Check for ranges property */
	err = of_pci_range_parser_init(&parser, dev_node); /* NVMe: 'ranges' 속성 파싱 준비 */
	if (err) /* NVMe: 조건 검사 */
		return 0; /* NVMe: 'ranges'가 없으면 리소스 없이 성공 */

	dev_dbg(dev, "Parsing ranges property...\n"); /* NVMe: ranges 파싱 디버그 로그 */
	for_each_of_pci_range(&parser, &range) { /* NVMe: ranges의 각 entry를 순회 */
		/* Read next ranges element */
		if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_IO) /* NVMe: IORESOURCE_TYPE_BITS로 IO/MEM 타입 구분 */
			range_type = "IO"; /* NVMe: 문자열 상수 할당 */
		else if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_MEM) /* NVMe: 메모리 타입 range */
			range_type = "MEM"; /* NVMe: 문자열 상수 할당 */
		else /* NVMe: if 조건 불만족 시 실행 */
			range_type = "err"; /* NVMe: 알 수 없는 타입 */
		dev_info(dev, "  %6s %#012llx..%#012llx -> %#012llx\n", /* NVMe: range 정보 출력(cpu_addr 범위와 pci_addr 매핑) */
			 range_type, range.cpu_addr, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
			 range.cpu_addr + range.size - 1, range.pci_addr); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */

		/*
		 * If we failed translation or got a zero-sized region
		 * then skip this range
		 */
		if (range.cpu_addr == OF_BAD_ADDR || range.size == 0) /* NVMe: 변환 실패하거나 크기 0이면 무시 */
			continue; /* NVMe: 다음 반복으로 걍너감 */

		err = of_pci_range_to_resource(&range, dev_node, &tmp_res); /* NVMe: of_pci_range를 struct resource로 변환 */
		if (err) /* NVMe: 조건 검사 */
			continue; /* NVMe: 다음 반복으로 걍너감 */

		res = devm_kmemdup(dev, &tmp_res, sizeof(tmp_res), GFP_KERNEL); /* NVMe: 변환된 리소스를 관리형 메모리로 복사 */
		if (!res) { /* NVMe: 조건이 참이면 블록 실행 */
			err = -ENOMEM; /* NVMe: 메모리 할당 실패 시 에러 처리 */
			goto failed; /* NVMe: failed 레이블로 이동하여 정리/에러 처리 */
		} /* NVMe: 함수/블록 본문 끝 */

		if (resource_type(res) == IORESOURCE_IO) { /* NVMe: IO 리소스인 경우 */
			if (!io_base) { /* NVMe: io_base 포인터가 없으면 IO range 처리 불가 */
				dev_err(dev, "I/O range found for %pOF. Please provide an io_base pointer to save CPU base address\n", /* NVMe: 진단/로그 메시지 출력 */
					dev_node); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
				err = -EINVAL; /* NVMe: 에러 코드 기록 */
				goto failed; /* NVMe: failed 레이블로 이동하여 정리/에러 처리 */
			} /* NVMe: 함수/블록 본문 끝 */
			if (*io_base != (resource_size_t)OF_BAD_ADDR) /* NVMe: IO가 여러 개면 경고, 마지막 cpu_addr만 저장 */
				dev_warn(dev, "More than one I/O resource converted for %pOF. CPU base address for old range lost!\n", /* NVMe: 진단/로그 메시지 출력 */
					 dev_node); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
			*io_base = range.cpu_addr; /* NVMe: IO range의 CPU 시작 주소 저장 */
		} else if (resource_type(res) == IORESOURCE_MEM) { /* NVMe: 메모리 리소스인 경우 */
			res->flags &= ~IORESOURCE_MEM_64; /* NVMe: 64bit MEM 플래그를 해제(기본 32bit 관리) */
		} /* NVMe: 함수/블록 본문 끝 */

		pci_add_resource_offset(resources, res,	res->start - range.pci_addr); /* NVMe: PCI 주소와 CPU 주소 간 오프셋을 적용해 리소스 추가 */
	} /* NVMe: 함수/블록 본문 끝 */

	/* Check for dma-ranges property */
	if (!ib_resources) /* NVMe: ib_resources가 없으면 dma-ranges 파싱 안 함 */
		return 0; /* NVMe: 정상 완료 반환 */
	err = of_pci_dma_range_parser_init(&parser, dev_node); /* NVMe: 'dma-ranges' 속성 파싱 준비 */
	if (err) /* NVMe: 조건 검사 */
		return 0; /* NVMe: 정상 완료 반환 */

	dev_dbg(dev, "Parsing dma-ranges property...\n"); /* NVMe: dev_dbg() 함수 호출 */
	for_each_of_pci_range(&parser, &range) { /* NVMe: dma-ranges의 각 entry 순회 */
		/*
		 * If we failed translation or got a zero-sized region
		 * then skip this range
		 */
		if (((range.flags & IORESOURCE_TYPE_BITS) != IORESOURCE_MEM) || /* NVMe: MEM이 아니거나 변환 실패/크기 0이면 스킵 */
		    range.cpu_addr == OF_BAD_ADDR || range.size == 0) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
			continue; /* NVMe: 다음 반복으로 걍너감 */

		dev_info(dev, "  %6s %#012llx..%#012llx -> %#012llx\n", /* NVMe: inbound DMA range 정보 출력 */
			 "IB MEM", range.cpu_addr, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
			 range.cpu_addr + range.size - 1, range.pci_addr); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */


		err = of_pci_range_to_resource(&range, dev_node, &tmp_res); /* NVMe: dma range를 resource로 변환 */
		if (err) /* NVMe: 조건 검사 */
			continue; /* NVMe: 다음 반복으로 걍너감 */

		res = devm_kmemdup(dev, &tmp_res, sizeof(tmp_res), GFP_KERNEL); /* NVMe: 관리형 메모리로 리소스 복사 */
		if (!res) { /* NVMe: 조건이 참이면 블록 실행 */
			err = -ENOMEM; /* NVMe: 에러 코드 기록 */
			goto failed; /* NVMe: failed 레이블로 이동하여 정리/에러 처리 */
		} /* NVMe: 함수/블록 본문 끝 */

		pci_add_resource_offset(ib_resources, res, /* NVMe: DMA inbound 리소스를 ib_resources 목록에 추가 */
					res->start - range.pci_addr); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
	} /* NVMe: 함수/블록 본문 끝 */

	return 0; /* NVMe: ranges/dma-ranges 파싱 성공 */

failed: /* NVMe: failed 레이블 */
	pci_free_resource_list(resources); /* NVMe: 파싱 실패 시 할당했던 리소스 목록 해제 */
	return err; /* NVMe: 에러 코드 반환 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: DT를 기반으로 PCI 장치의 INTx 인터럽트를 해석한다.
 *       NVMe가 MSI-X 대신 INTx(legacy)를 사용할 때 호출되며,
 *       swizzling과 interrupt-map을 따라 상위 버스/bridge로 올라가며 해석.
 */
#if IS_ENABLED(CONFIG_OF_IRQ) /* NVMe: 함수 호출 또는 조건/선언의 일부 */
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
static int of_irq_parse_pci(const struct pci_dev *pdev, struct of_phandle_args *out_irq) /* NVMe: INTx 인터럽트 해석 함수 시작 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *dn, *ppnode = NULL; /* NVMe: 장치 노드와 부모 장치 노드, laddr 인터럽트 주소 */
	struct pci_dev *ppdev; /* NVMe: *ppdev 포인터 변수 선언 */
	__be32 laddr[3]; /* NVMe: big-endian 32bit 배열 laddr[3] 선언 */
	u8 pin; /* NVMe: pin 1바이트 변수 선언 */
	int rc; /* NVMe: 반환 코드 */

	/*
	 * Check if we have a device node, if yes, fallback to standard
	 * device tree parsing
	 */
	dn = pci_device_to_OF_node(pdev); /* NVMe: pci_dev에서 OF node 획득 */
	if (dn) { /* NVMe: 조건이 참이면 블록 실행 */
		rc = of_irq_parse_one(dn, 0, out_irq); /* NVMe: OF node가 있으면 표준 OF 인터럽트 파싱 사용 */
		if (!rc) /* NVMe: 조건 검사 */
			return rc; /* NVMe: 반환 코드 전달 */
	} /* NVMe: 함수/블록 본문 끝 */

	/*
	 * Ok, we don't, time to have fun. Let's start by building up an
	 * interrupt spec.  we assume #interrupt-cells is 1, which is standard
	 * for PCI. If you do different, then don't use that routine.
	 */
	rc = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin); /* NVMe: PCI config space에서 interrupt pin 읽기 */
	if (rc != 0) /* NVMe: 조건 검사 */
		goto err; /* NVMe: err 레이블로 이동하여 정리/에러 처리 */
	/* No pin, exit with no error message. */
	if (pin == 0) /* NVMe: interrupt pin이 0이면 INTx 없음 */
		return -ENODEV; /* NVMe: 장치 없음 에러 반환 */

	/* Local interrupt-map in the device node? Use it! */
	if (of_property_present(dn, "interrupt-map")) { /* NVMe: 장치 노드에 'interrupt-map'이 있으면 로컬 맵 사용 */
		pin = pci_swizzle_interrupt_pin(pdev, pin); /* NVMe: interrupt pin swizzling 수행 */
		ppnode = dn; /* NVMe: ppnode 변수에 값 할당 */
	} /* NVMe: 함수/블록 본문 끝 */

	/* Now we walk up the PCI tree */
	while (!ppnode) { /* NVMe: 부모 노드를 찾을 때까지 버스 트리를 따라 올라감 */
		/* Get the pci_dev of our parent */
		ppdev = pdev->bus->self; /* NVMe: 상위 P2P bridge의 pci_dev 획득 */

		/* Ouch, it's a host bridge... */
		if (ppdev == NULL) { /* NVMe: 상위가 host bridge이면 bus의 OF node 사용 */
			ppnode = pci_bus_to_OF_node(pdev->bus); /* NVMe: ppnode에 함수 호출 결과 저장 */

			/* No node for host bridge ? give up */
			if (ppnode == NULL) { /* NVMe: host bridge에도 OF node가 없으면 실패 */
				rc = -EINVAL; /* NVMe: 반환 코드 기록 */
				goto err; /* NVMe: err 레이블로 이동하여 정리/에러 처리 */
			} /* NVMe: 함수/블록 본문 끝 */
		} else { /* NVMe: 이전 조건이 아닌 경우 else 분기 */
			/* We found a P2P bridge, check if it has a node */
			ppnode = pci_device_to_OF_node(ppdev); /* NVMe: P2P bridge의 OF node가 있으면 사용 */
		} /* NVMe: 함수/블록 본문 끝 */

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
		if (ppnode) /* NVMe: OF node를 찾으면 루프 탈출 */
			break; /* NVMe: switch 분기 종료 */

		/*
		 * We can only get here if we hit a P2P bridge with no node;
		 * let's do standard swizzling and try again
		 */
		pin = pci_swizzle_interrupt_pin(pdev, pin); /* NVMe: OF node 없는 P2P bridge에서 swizzling 후 계속 상승 */
		pdev = ppdev; /* NVMe: pdev 변수에 값 할당 */
	} /* NVMe: 함수/블록 본문 끝 */

	out_irq->np = ppnode; /* NVMe: 해상된 OF node 설정 */
	out_irq->args_count = 1; /* NVMe: PCI 인터럽트 셀 개수는 1개(pin)로 가정 */
	out_irq->args[0] = pin; /* NVMe: 첫 번째 인자로 interrupt pin 설정 */
	laddr[0] = cpu_to_be32((pdev->bus->number << 16) | (pdev->devfn << 8)); /* NVMe: bus 번호와 devfn으로 unit address 구성(be32) */
	laddr[1] = laddr[2] = cpu_to_be32(0); /* NVMe: interrupt-map 용 unit address 상위 셀을 0으로 설정 */
	rc = of_irq_parse_raw(laddr, out_irq); /* NVMe: OF raw 인터럽트 파싱 수행 */
	if (rc) /* NVMe: 조건 검사 */
		goto err; /* NVMe: err 레이블로 이동하여 정리/에러 처리 */
	return 0; /* NVMe: 정상 완료 반환 */
err: /* NVMe: 에러 처리 레이블 */
	if (rc == -ENOENT) { /* NVMe: interrupt-map이 없으면 INTx 사용 불가 경고 */
		dev_warn(&pdev->dev, /* NVMe: 진단/로그 메시지 출력 */
			"%s: no interrupt-map found, INTx interrupts not available\n", /* NVMe: 다중 라인 선언/호출의 연속 인자 */
			__func__); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
		pr_warn_once("%s: possibly some PCI slots don't have level triggered interrupts capability\n", /* NVMe: 다중 라인 선언/호출의 연속 인자 */
			__func__); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
	} else { /* NVMe: 이전 조건이 아닌 경우 else 분기 */
		dev_err(&pdev->dev, "%s: failed with rc=%d\n", __func__, rc); /* NVMe: 기타 에러는 오류 메시지 출력 */
	} /* NVMe: 함수/블록 본문 끝 */
	return rc; /* NVMe: 반환 코드 전달 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: DT의 PCI 인터럽트를 해석한 뒤 Linux virq 번호로 변환한다.
 *       pci_host_bridge.map_irq 콜백으로 등록되어 NVMe INTx 라우팅에 사용.
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
int of_irq_parse_and_map_pci(const struct pci_dev *dev, u8 slot, u8 pin) /* NVMe: of_irq_parse_and_map_pci() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct of_phandle_args oirq; /* NVMe: OF 인터럽트 인자 구조체 */
	int ret; /* NVMe: ret 저장 변수 선언 */

	ret = of_irq_parse_pci(dev, &oirq); /* NVMe: PCI 장치의 OF 인터럽트 해석 */
	if (ret) /* NVMe: 조건 검사 */
		return 0; /* Proper return code 0 == NO_IRQ */ /* NVMe: OF IRQ 해석 실패 시 NO_IRQ(0) 반환 */

	return irq_create_of_mapping(&oirq); /* NVMe: OF 인터럽트를 Linux virq로 매핑 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_irq_parse_and_map_pci); /* NVMe: of_irq_parse_and_map_pci 심볼을 모듈 외부에 노출 */
#endif	/* CONFIG_OF_IRQ */

/*
 * NVMe: devm_of_pci_get_host_bridge_resources()로 추출한 리소스를 요청하고,
 *       IO/MEM 윈도우를 bridge->windows에 등록한다.
 *       non-prefetchable MEM 리소스 존재 여부를 검사한다.
 */
static int pci_parse_request_of_pci_ranges(struct device *dev, /* NVMe: 정적 함수 정의/선언 */
					   struct pci_host_bridge *bridge) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
{ /* NVMe: 함수/블록 본문 시작 */
	int err, res_valid = 0; /* NVMe: err는 에러 코드, res_valid는 non-prefetch MEM 존재 플래그 */
	resource_size_t iobase; /* NVMe: iobase 리소스 크기/주소 변수 선언 */
	struct resource_entry *win, *tmp; /* NVMe: 변수 선언 */

	INIT_LIST_HEAD(&bridge->windows); /* NVMe: bridge 윈도우 목록 초기화 */
	INIT_LIST_HEAD(&bridge->dma_ranges); /* NVMe: bridge DMA inbound 범위 목록 초기화 */

	err = devm_of_pci_get_host_bridge_resources(dev, &bridge->windows, /* NVMe: DT에서 bridge 리소스 파싱 */
						    &bridge->dma_ranges, &iobase); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
	if (err) /* NVMe: 조건 검사 */
		return err; /* NVMe: 에러 코드 반환 */

	err = devm_request_pci_bus_resources(dev, &bridge->windows); /* NVMe: 파싱된 리소스를 시스템에 요청 */
	if (err) /* NVMe: 조건 검사 */
		return err; /* NVMe: 에러 코드 반환 */

	resource_list_for_each_entry_safe(win, tmp, &bridge->windows) { /* NVMe: 각 윈도우 리소스를 순회하며 처리 */
		struct resource *res = win->res; /* NVMe: 변수 선언 */

		switch (resource_type(res)) { /* NVMe: 리소스 타입에 따라 분기 */
		case IORESOURCE_IO: /* NVMe: switch case 분기 */
			err = devm_pci_remap_iospace(dev, res, iobase); /* NVMe: IO 리소스는 CPU IO 공간으로 remap */
			if (err) { /* NVMe: 조건이 참이면 블록 실행 */
				dev_warn(dev, "error %d: failed to map resource %pR\n", /* NVMe: 진단/로그 메시지 출력 */
					 err, res); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
				resource_list_destroy_entry(win); /* NVMe: remap 실패 시 해당 윈도우 제거 */
			} /* NVMe: 함수/블록 본문 끝 */
			break; /* NVMe: switch 분기 종료 */
		case IORESOURCE_MEM: /* NVMe: switch case 분기 */
			res_valid |= !(res->flags & IORESOURCE_PREFETCH); /* NVMe: non-prefetchable MEM이면 res_valid 설정 */

			if (!(res->flags & IORESOURCE_PREFETCH)) /* NVMe: 조건 검사 */
				if (upper_32_bits(resource_size(res))) /* NVMe: 32bit를 초과하는 MEM 크기 경고 */
					dev_warn(dev, "Memory resource size exceeds max for 32 bits\n"); /* NVMe: dev_warn() 함수 호출 */

			break; /* NVMe: switch 분기 종료 */
		} /* NVMe: 함수/블록 본문 끝 */
	} /* NVMe: 함수/블록 본문 끝 */

	if (!res_valid) /* NVMe: non-prefetchable MEM이 하나도 없으면 경고 */
		dev_warn(dev, "non-prefetchable memory resource required\n"); /* NVMe: dev_warn() 함수 호출 */

	return 0; /* NVMe: 정상 완료 반환 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: host bridge를 DT 기반으로 초기화한다.
 *       swizzle_irq, map_irq 콜백을 설정하고, ranges/dma-ranges를 파싱하여
 *       NVMe DMA를 위한 bridge 리소스 윈도우를 준비한다.
 */
int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge) /* NVMe: devm_of_pci_bridge_init() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	if (!dev->of_node) /* NVMe: host bridge에 of_node가 없으면 초기화 불필요 */
		return 0; /* NVMe: 정상 완료 반환 */

	bridge->swizzle_irq = pci_common_swizzle; /* NVMe: 표준 PCI interrupt swizzling 함수 연결 */
	bridge->map_irq = of_irq_parse_and_map_pci; /* NVMe: DT 기반 IRQ 매핑 콜백 연결 */

	return pci_parse_request_of_pci_ranges(dev, bridge); /* NVMe: ranges/dma-ranges 파싱 및 리소스 등록 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: CONFIG_PCI_DYNAMIC_OF_NODES가 활성화된 경우,
 *       런타임에 PCI 장치/bridge에 동적으로 DT 노드를 생성/제거하는 함수들.
 *       NVMe hotplug 시 동적으로 OF 노드를 추가해야 하는 플랫폼에서 사용.
 */
#ifdef CONFIG_PCI_DYNAMIC_OF_NODES

/*
 * NVMe: 동적으로 생성된 pci_dev의 DT 노드를 제거한다.
 *       OF_DYNAMIC 플래그가 설정된 동적 노드만 정리.
 */
void of_pci_remove_node(struct pci_dev *pdev) /* NVMe: of_pci_remove_node() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *np; /* NVMe: *np 포인터 변수 선언 */

	np = pci_device_to_OF_node(pdev); /* NVMe: pci_dev의 OF node 획득 */
	if (!np || !of_node_check_flag(np, OF_DYNAMIC)) /* NVMe: 동적 생성된 노드가 아니면 제거하지 않음 */
		return; /* NVMe: 함수 종료 */

	device_remove_of_node(&pdev->dev); /* NVMe: device에서 of_node 제거 */
	of_changeset_revert(np->data); /* NVMe: changeset 되돌리기 */
	of_changeset_destroy(np->data); /* NVMe: changeset 메모리 해제 */
	of_node_put(np); /* NVMe: of_node_put() 함수 호출 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: pci_dev에 아직 of_node가 없으면 동적으로 생성한다.
 *       NVMe 장치가 hotplug로 추가되었을 때 사용자 공간/드라이버가
 *       DT 기반으로 인식할 수 있도록 노드를 만든다.
 */
void of_pci_make_dev_node(struct pci_dev *pdev) /* NVMe: of_pci_make_dev_node() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *ppnode, *np = NULL; /* NVMe: 변수 선언 */
	const char *pci_type; /* NVMe: *pci_type 문자열 포인터 선언 */
	struct of_changeset *cset; /* NVMe: *cset 포인터 변수 선언 */
	const char *name; /* NVMe: *name 문자열 포인터 선언 */
	int ret; /* NVMe: ret 저장 변수 선언 */

	/*
	 * If there is already a device tree node linked to this device,
	 * return immediately.
	 */
	if (pci_device_to_OF_node(pdev)) /* NVMe: 이미 of_node가 있으면 동적 생성 불필요 */
		return; /* NVMe: 함수 종료 */

	/* Check if there is device tree node for parent device */
	if (!pdev->bus->self) /* NVMe: root bus이면 bus의 of_node를 부모로 사용 */
		ppnode = pdev->bus->dev.of_node; /* NVMe: 구조체/멤버 값 할당 */
	else /* NVMe: if 조건 불만족 시 실행 */
		ppnode = pdev->bus->self->dev.of_node; /* NVMe: 하위 버스이면 상위 P2P bridge의 of_node를 부모로 사용 */
	if (!ppnode) /* NVMe: 조건 검사 */
		return; /* NVMe: 함수 종료 */

	if (pci_is_bridge(pdev)) /* NVMe: 브리지 장치인지 여부에 따라 노드 이름 결정 */
		pci_type = "pci"; /* NVMe: 동적 노드 이름 접두사 설정 */
	else /* NVMe: if 조건 불만족 시 실행 */
		pci_type = "dev"; /* NVMe: 동적 노드 이름 접두사 설정 */

	name = kasprintf(GFP_KERNEL, "%s@%x,%x", pci_type, /* NVMe: 'pci@slot,func' 또는 'dev@slot,func' 형태 이름 생성 */
			 PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn)); /* NVMe: PCI_SLOT() 함수 호출 */
	if (!name) /* NVMe: 조건 검사 */
		return; /* NVMe: 함수 종료 */

	cset = kmalloc_obj(*cset); /* NVMe: of_changeset 메모리 할당 */
	if (!cset) /* NVMe: 조건 검사 */
		goto out_free_name; /* NVMe: out_free_name 레이블로 이동 */
	of_changeset_init(cset); /* NVMe: changeset 초기화 */

	np = of_changeset_create_node(cset, ppnode, name); /* NVMe: 부모 노드 아래 동적 노드 생성 */
	if (!np) /* NVMe: 조건 검사 */
		goto out_destroy_cset; /* NVMe: out_destroy_cset 레이블로 이동 */

	ret = of_pci_add_properties(pdev, cset, np); /* NVMe: pci_dev 속성을 동적 노드에 추가 */
	if (ret) /* NVMe: 조건 검사 */
		goto out_free_node; /* NVMe: out_free_node 레이블로 이동 */

	ret = of_changeset_apply(cset); /* NVMe: changeset을 실제 DT에 적용 */
	if (ret) /* NVMe: 조건 검사 */
		goto out_free_node; /* NVMe: out_free_node 레이블로 이동 */

	np->data = cset; /* NVMe: changeset 포인터를 노드에 저장 */

	ret = device_add_of_node(&pdev->dev, np); /* NVMe: pci_dev device에 of_node 추가 */
	if (ret) /* NVMe: 조건 검사 */
		goto out_revert_cset; /* NVMe: out_revert_cset 레이블로 이동 */

	kfree(name); /* NVMe: kfree() 함수 호출 */

	return; /* NVMe: 함수 종료 */

out_revert_cset: /* NVMe: out_revert_cset 레이블 */
	np->data = NULL; /* NVMe: 되돌리기 전 changeset 포인터 클리어 */
	of_changeset_revert(cset); /* NVMe: of_changeset_revert() 함수 호출 */
out_free_node: /* NVMe: out_free_node 레이블 */
	of_node_put(np); /* NVMe: 동적 노드 참조 해제 */
out_destroy_cset: /* NVMe: out_destroy_cset 레이블 */
	of_changeset_destroy(cset); /* NVMe: changeset 메모리 해제 */
	kfree(cset); /* NVMe: kfree() 함수 호출 */
out_free_name: /* NVMe: out_free_name 레이블 */
	kfree(name); /* NVMe: kfree() 함수 호출 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: 동적으로 생성된 host bridge의 DT 노드를 제거한다.
 *       NVMe root complex가 제거될 때 bus/device 양쪽의 of_node 연결을 정리.
 */
void of_pci_remove_host_bridge_node(struct pci_host_bridge *bridge) /* NVMe: of_pci_remove_host_bridge_node() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *np; /* NVMe: *np 포인터 변수 선언 */

	np = pci_bus_to_OF_node(bridge->bus); /* NVMe: host bridge가 처리하는 bus의 OF node 획득 */
	if (!np || !of_node_check_flag(np, OF_DYNAMIC)) /* NVMe: 동적 노드가 아니면 제거하지 않음 */
		return; /* NVMe: 함수 종료 */

	device_remove_of_node(&bridge->bus->dev); /* NVMe: bus device의 of_node 제거 */
	device_remove_of_node(&bridge->dev); /* NVMe: host bridge device의 of_node 제거 */
	of_changeset_revert(np->data); /* NVMe: of_changeset_revert() 함수 호출 */
	of_changeset_destroy(np->data); /* NVMe: of_changeset_destroy() 함수 호출 */
	of_node_put(np); /* NVMe: of_node_put() 함수 호출 */
} /* NVMe: 함수/블록 본문 끝 */

/*
 * NVMe: root bus에 of_node가 없으면 동적으로 host bridge DT 노드를 생성한다.
 *       OF_POPULATED 플래그를 설정하여 불필요한 platform device 생성을 방지.
 */
void of_pci_make_host_bridge_node(struct pci_host_bridge *bridge) /* NVMe: of_pci_make_host_bridge_node() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	struct device_node *np = NULL; /* NVMe: 변수 선언 */
	struct of_changeset *cset; /* NVMe: *cset 포인터 변수 선언 */
	const char *name; /* NVMe: *name 문자열 포인터 선언 */
	int ret; /* NVMe: ret 저장 변수 선언 */

	/*
	 * If there is already a device tree node linked to the PCI bus handled
	 * by this bridge (i.e. the PCI root bus), nothing to do.
	 */
	if (pci_bus_to_OF_node(bridge->bus)) /* NVMe: root bus에 이미 of_node가 있으면 중단 */
		return; /* NVMe: 함수 종료 */

	/*
	 * The root bus has no node. Check that the host bridge has no node
	 * too
	 */
	if (bridge->dev.of_node) { /* NVMe: host bridge device에도 of_node가 없어야 함 */
		dev_err(&bridge->dev, "PCI host bridge of_node already set"); /* NVMe: dev_err() 함수 호출 */
		return; /* NVMe: 함수 종료 */
	} /* NVMe: 함수/블록 본문 끝 */

	/* Check if there is a DT root node to attach the created node */
	if (!of_root) { /* NVMe: DT root가 없으면 동적 노드 생성 불가 */
		pr_debug("of_root node is NULL, cannot create PCI host bridge node\n"); /* NVMe: pr_debug() 함수 호출 */
		return; /* NVMe: 함수 종료 */
	} /* NVMe: 함수/블록 본문 끝 */

	name = kasprintf(GFP_KERNEL, "pci@%x,%x", pci_domain_nr(bridge->bus), /* NVMe: 'pci@domain,bus' 형태 이름 생성 */
			 bridge->bus->number); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
	if (!name) /* NVMe: 조건 검사 */
		return; /* NVMe: 함수 종료 */

	cset = kmalloc_obj(*cset); /* NVMe: of_changeset 메모리 할당 */
	if (!cset) /* NVMe: 조건 검사 */
		goto out_free_name; /* NVMe: out_free_name 레이블로 이동 */
	of_changeset_init(cset); /* NVMe: changeset 초기화 */

	np = of_changeset_create_node(cset, of_root, name); /* NVMe: of_root 아래 동적 host bridge 노드 생성 */
	if (!np) /* NVMe: 조건 검사 */
		goto out_destroy_cset; /* NVMe: out_destroy_cset 레이블로 이동 */

	ret = of_pci_add_host_bridge_properties(bridge, cset, np); /* NVMe: host bridge 속성 추가 */
	if (ret) /* NVMe: 조건 검사 */
		goto out_free_node; /* NVMe: out_free_node 레이블로 이동 */

	/*
	 * This of_node will be added to an existing device. The of_node parent
	 * is the root OF node and so this node will be handled by the platform
	 * bus. Avoid any new device creation.
	 */
	of_node_set_flag(np, OF_POPULATED); /* NVMe: platform device 자동 생성 방지 플래그 설정 */
	np->fwnode.dev = &bridge->dev; /* NVMe: fwnode를 host bridge device에 연결 */
	fwnode_dev_initialized(&np->fwnode, true); /* NVMe: fwnode 초기화 완료 표시 */

	ret = of_changeset_apply(cset); /* NVMe: changeset 적용 */
	if (ret) /* NVMe: 조건 검사 */
		goto out_free_node; /* NVMe: out_free_node 레이블로 이동 */

	np->data = cset; /* NVMe: 구조체 멤버에 값 할당 */

	/* Add the of_node to host bridge and the root bus */
	ret = device_add_of_node(&bridge->dev, np); /* NVMe: host bridge device에 of_node 추가 */
	if (ret) /* NVMe: 조건 검사 */
		goto out_revert_cset; /* NVMe: out_revert_cset 레이블로 이동 */

	ret = device_add_of_node(&bridge->bus->dev, np); /* NVMe: root bus device에도 동일 of_node 추가 */
	if (ret) /* NVMe: 조건 검사 */
		goto out_remove_bridge_dev_of_node; /* NVMe: out_remove_bridge_dev_of_node 레이블로 이동 */

	kfree(name); /* NVMe: kfree() 함수 호출 */

	return; /* NVMe: 함수 종료 */

out_remove_bridge_dev_of_node: /* NVMe: out_remove_bridge_dev_of_node 레이블 */
	device_remove_of_node(&bridge->dev); /* NVMe: bridge device의 of_node 제거(롤백) */
out_revert_cset: /* NVMe: out_revert_cset 레이블 */
	np->data = NULL; /* NVMe: changeset 포인터 클리어 후 되돌리기 */
	of_changeset_revert(cset); /* NVMe: of_changeset_revert() 함수 호출 */
out_free_node: /* NVMe: out_free_node 레이블 */
	of_node_put(np); /* NVMe: of_node_put() 함수 호출 */
out_destroy_cset: /* NVMe: out_destroy_cset 레이블 */
	of_changeset_destroy(cset); /* NVMe: of_changeset_destroy() 함수 호출 */
	kfree(cset); /* NVMe: kfree() 함수 호출 */
out_free_name: /* NVMe: out_free_name 레이블 */
	kfree(name); /* NVMe: kfree() 함수 호출 */
} /* NVMe: 함수/블록 본문 끝 */

#endif /* CONFIG_PCI_DYNAMIC_OF_NODES */

/*
 * NVMe: DT 노드에 'xxx-supply' 형태의 power supply 속성이 있는지 확인한다.
 *       NVMe SSD의 전원 레일이 DT에 정의되어 있는지 검사할 때 사용.
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
bool of_pci_supply_present(struct device_node *np) /* NVMe: DT 노드 포인터 */
{ /* NVMe: 속성 이름에서 '-' 위치를 찾기 위한 포인터 */
	struct property *prop; /* NVMe: *prop 포인터 변수 선언 */
	char *supply; /* NVMe: 변수 선언 */

	if (!np) /* NVMe: 노드가 없으면 전원 공급 없음 */
		return false; /* NVMe: false 반환 */

	for_each_property_of_node(np, prop) { /* NVMe: 노드의 모든 속성 순회 */
		supply = strrchr(prop->name, '-'); /* NVMe: 속성 이름에서 마지막 '-' 위치 탐색 */
		if (supply && !strcmp(supply, "-supply")) /* NVMe: 접미사가 '-supply'이면 전원 공급 존재 */
			return true; /* NVMe: true 반환 */
	} /* NVMe: 함수/블록 본문 끝 */

	return false; /* NVMe: false 반환 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_pci_supply_present); /* NVMe: of_pci_supply_present 심볼을 모듈 외부에 노출 */

#endif /* CONFIG_PCI */

/*
 * NVMe: DT의 'max-link-speed' 속성을 읽어 PCIe 링크 최대 속도를 반환한다.
 *       Gen3/4/5 등 NVMe SSD와 host bridge 간 링크 협상 속도에 직접 영향.
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
int of_pci_get_max_link_speed(struct device_node *node) /* NVMe: of_pci_get_max_link_speed() 함수 정의/선언 */
{ /* NVMe: 함수/블록 본문 시작 */
	u32 max_link_speed; /* NVMe: max-link-speed 값 저장 변수 */
	int ret; /* NVMe: ret 저장 변수 선언 */

	ret = of_property_read_u32(node, "max-link-speed", &max_link_speed); /* NVMe: DT 'max-link-speed' 속성 읽기 */
	if (ret) /* NVMe: 조건 검사 */
		return ret; /* NVMe: 읽기 실패 시 에러 반환 */

	return max_link_speed; /* NVMe: 최대 링크 속도 반환 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_pci_get_max_link_speed); /* NVMe: of_pci_get_max_link_speed 심볼을 모듈 외부에 노출 */

/*
 * NVMe: DT의 'slot-power-limit-milliwatt' 속성을 PCIe Slot Capabilities
 *       Register 형식(value/scale)으로 변환한다.
 *       NVMe SSD의 슬롯 전력 제한을 결정하며, 전력 예산 초과 시 동작에 영향.
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
u32 of_pci_get_slot_power_limit(struct device_node *node, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
				u8 *slot_power_limit_value, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
				u8 *slot_power_limit_scale) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
{ /* NVMe: 함수/블록 본문 시작 */
	u32 slot_power_limit_mw; /* NVMe: milliwatt 단위 전력 제한 */
	u8 value, scale; /* NVMe: Slot Capabilities Register 형식의 value/scale */

	if (of_property_read_u32(node, "slot-power-limit-milliwatt", /* NVMe: 'slot-power-limit-milliwatt' 속성 읽기 */
				 &slot_power_limit_mw)) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
		slot_power_limit_mw = 0; /* NVMe: 속성 없으면 0으로 초기화 */

	/* Calculate Slot Power Limit Value and Slot Power Limit Scale */
	if (slot_power_limit_mw == 0) { /* NVMe: 조건이 참이면 블록 실행 */
		value = 0x00; /* NVMe: 0mW이면 value/scale 모두 0 */
		scale = 0; /* NVMe: scale 변수에 값 할당 */
	} else if (slot_power_limit_mw <= 255) { /* NVMe: 이전 조건 불만족 시 추가 조건 검사 */
		value = slot_power_limit_mw; /* NVMe: 1mW~255mW: scale=3(0.001x) */
		scale = 3; /* NVMe: scale 변수에 값 할당 */
	} else if (slot_power_limit_mw <= 255*10) { /* NVMe: 이전 조건 불만족 시 추가 조건 검사 */
		value = slot_power_limit_mw / 10; /* NVMe: 256mW~2550mW: scale=2(0.01x) */
		scale = 2; /* NVMe: scale 변수에 값 할당 */
		slot_power_limit_mw = slot_power_limit_mw / 10 * 10; /* NVMe: milliwatt 값을 scale에 맞게 내림 정렬 */
	} else if (slot_power_limit_mw <= 255*100) { /* NVMe: 이전 조건 불만족 시 추가 조건 검사 */
		value = slot_power_limit_mw / 100; /* NVMe: 2551mW~25500mW: scale=1(0.1x) */
		scale = 1; /* NVMe: scale 변수에 값 할당 */
		slot_power_limit_mw = slot_power_limit_mw / 100 * 100; /* NVMe: milliwatt 값을 scale에 맞게 내림 정렬 */
	} else if (slot_power_limit_mw <= 239*1000) { /* NVMe: 이전 조건 불만족 시 추가 조건 검사 */
		value = slot_power_limit_mw / 1000; /* NVMe: 25501mW~239000mW: scale=0(1x) */
		scale = 0; /* NVMe: scale 변수에 값 할당 */
		slot_power_limit_mw = slot_power_limit_mw / 1000 * 1000; /* NVMe: milliwatt 값을 scale에 맞게 내림 정렬 */
	} else if (slot_power_limit_mw < 250*1000) { /* NVMe: 이전 조건 불만족 시 추가 조건 검사 */
		value = 0xEF; /* NVMe: 239001mW~249999mW: 최대값 0xEF로 클램핑 */
		scale = 0; /* NVMe: scale 변수에 값 할당 */
		slot_power_limit_mw = 239*1000; /* NVMe: milliwatt 값을 scale에 맞게 내림 정렬 */
	} else if (slot_power_limit_mw <= 600*1000) { /* NVMe: 이전 조건 불만족 시 추가 조건 검사 */
		value = 0xF0 + (slot_power_limit_mw / 1000 - 250) / 25; /* NVMe: 250000mW~600000mW: 25mW 단위로 인코딩 */
		scale = 0; /* NVMe: scale 변수에 값 할당 */
		slot_power_limit_mw = slot_power_limit_mw / (1000*25) * (1000*25); /* NVMe: milliwatt 값을 scale에 맞게 내림 정렬 */
	} else { /* NVMe: 이전 조건이 아닌 경우 else 분기 */
		value = 0xFE; /* NVMe: 600000mW 초과: 최대값 0xFE로 클램핑 */
		scale = 0; /* NVMe: scale 변수에 값 할당 */
		slot_power_limit_mw = 600*1000; /* NVMe: milliwatt 값을 scale에 맞게 내림 정렬 */
	} /* NVMe: 함수/블록 본문 끝 */

	if (slot_power_limit_value) /* NVMe: 조건 검사 */
		*slot_power_limit_value = value; /* NVMe: value 포인터가 유효하면 저장 */

	if (slot_power_limit_scale) /* NVMe: 조건 검사 */
		*slot_power_limit_scale = scale; /* NVMe: scale 포인터가 유효하면 저장 */

	return slot_power_limit_mw; /* NVMe: milliwatt 단위 전력 제한 반환 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_pci_get_slot_power_limit); /* NVMe: of_pci_get_slot_power_limit 심볼을 모듈 외부에 노출 */

/*
 * NVMe: DT의 'eq-presets-Ngts' 속성을 읽어 PCIe Equalization Preset을 구성한다.
 *       고속 링크(Gen3 이상)에서 NVMe와 host 간 신호 품질/링크 안정성에 영향.
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
int of_pci_get_equalization_presets(struct device *dev, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
				    struct pci_eq_presets *presets, /* NVMe: 다중 라인 선언/호출의 연속 인자 */
				    int num_lanes) /* NVMe: 다중 라인 함수 호출/선언의 마지막 인자 */
{ /* NVMe: 함수/블록 본문 시작 */
	char name[20]; /* NVMe: 속성 이름 버퍼 */
	int ret; /* NVMe: ret 저장 변수 선언 */

	presets->eq_presets_8gts[0] = PCI_EQ_RESV; /* NVMe: 8GT/s preset 기본값으로 PCI_EQ_RESV 설정 */
	ret = of_property_read_u16_array(dev->of_node, "eq-presets-8gts", /* NVMe: 'eq-presets-8gts' 속성 읽기 */
					 presets->eq_presets_8gts, num_lanes); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
	if (ret && ret != -EINVAL) { /* NVMe: 조건이 참이면 블록 실행 */
		dev_err(dev, "Error reading eq-presets-8gts: %d\n", ret); /* NVMe: -EINVAL 외의 에러는 실패 처리 */
		return ret; /* NVMe: 반환 코드 전달 */
	} /* NVMe: 함수/블록 본문 끝 */

	for (int i = 0; i < EQ_PRESET_TYPE_MAX - 1; i++) { /* NVMe: 16GT/s, 32GT/s 등 고속 preset 순회 */
		presets->eq_presets_Ngts[i][0] = PCI_EQ_RESV; /* NVMe: 각 속도별 preset 기본값 설정 */
		snprintf(name, sizeof(name), "eq-presets-%dgts", 8 << (i + 1)); /* NVMe: 'eq-presets-Ngts' 형태 속성 이름 생성 */
		ret = of_property_read_u8_array(dev->of_node, name, /* NVMe: 해당 속도의 u8 preset 배열 읽기 */
						presets->eq_presets_Ngts[i], /* NVMe: 다중 라인 선언/호출의 연속 인자 */
						num_lanes); /* NVMe: 다중 라인 함수 호출의 마지막 인자/닫기 */
		if (ret && ret != -EINVAL) { /* NVMe: 조건이 참이면 블록 실행 */
			dev_err(dev, "Error reading %s: %d\n", name, ret); /* NVMe: dev_err() 함수 호출 */
			return ret; /* NVMe: 반환 코드 전달 */
		} /* NVMe: 함수/블록 본문 끝 */
	} /* NVMe: 함수/블록 본문 끝 */

	return 0; /* NVMe: 정상 완료 반환 */
} /* NVMe: 함수/블록 본문 끝 */
EXPORT_SYMBOL_GPL(of_pci_get_equalization_presets); /* NVMe: of_pci_get_equalization_presets 심볼을 모듈 외부에 노출 */
