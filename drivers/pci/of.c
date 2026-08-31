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

#define pr_fmt(fmt)	"PCI: OF: " fmt

#include <linux/cleanup.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>
#include "pci.h"

#ifdef CONFIG_PCI
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
int pci_set_of_node(struct pci_dev *dev)
{
	if (!dev->bus->dev.of_node)
		return 0;

	struct device_node *node __free(device_node) =
		of_pci_find_child_device(dev->bus->dev.of_node, dev->devfn);
	if (!node)
		return 0;

	struct device *pdev __free(put_device) =
		bus_find_device_by_of_node(&platform_bus_type, node);
	if (pdev)
		dev->bus->dev.of_node_reused = true;

	device_set_node(&dev->dev, of_fwnode_handle(no_free_ptr(node)));
	return 0;
}

/*
 * NVMe: pci_dev가 사용하던 of_node 참조 카운트를 해제하고 연결을 끊는다.
 *       NVMe 장치가 제거(hotplug/제거)될 때 메모리 누수를 방지한다.
 */
void pci_release_of_node(struct pci_dev *dev)
{
	of_node_put(dev->dev.of_node);
	device_set_node(&dev->dev, NULL);
}

/*
 * NVMe: pci_bus(버스) 객체의 of_node를 설정한다.
 *       root bus(PHB)이면 pcibios_get_phb_of_node()로,
 *       하위 버스이면 상위 P2P bridge의 of_node를 상속받는다.
 *       'external-facing' 속성은 외부 접근 가능 슬롯(예: NVMe hotplug slot) 표시.
 */
void pci_set_bus_of_node(struct pci_bus *bus)
{
	struct device_node *node;

	if (bus->self == NULL) {
		node = pcibios_get_phb_of_node(bus);
	} else {
		node = of_node_get(bus->self->dev.of_node);
		if (node && of_property_read_bool(node, "external-facing"))
			bus->self->external_facing = true;
	}

	device_set_node(&bus->dev, of_fwnode_handle(node));
}

/*
 * NVMe: pci_bus의 of_node 참조를 해제한다.
 *       버스 제거 시 호출되어 NVMe가 탑재된 버스 트리의 DT 연결을 정리.
 */
void pci_release_bus_of_node(struct pci_bus *bus)
{
	of_node_put(bus->dev.of_node);
	device_set_node(&bus->dev, NULL);
}

/*
 * NVMe: PCI host bridge(PHB)에 해당하는 DT node를 반환한다.
 *       __weak로 정의되어 있어 아키텍처/플랫폼별로 재정의 가능하다.
 *       NVMe가 연결된 root bus의 bridge device에서 of_node를 찾는다.
 */
struct device_node * __weak pcibios_get_phb_of_node(struct pci_bus *bus)
{
	/* This should only be called for PHBs */
	if (WARN_ON(bus->self || bus->parent))
		return NULL;

	/*
	 * Look for a node pointer in either the intermediary device we
	 * create above the root bus or its own parent. Normally only
	 * the later is populated.
	 */
	if (bus->bridge->of_node)
		return of_node_get(bus->bridge->of_node);
	if (bus->bridge->parent && bus->bridge->parent->of_node)
		return of_node_get(bus->bridge->parent->of_node);
	return NULL;
}

/*
 * NVMe: host bridge에 연결된 MSI domain을 DT에서 찾는다.
 *       NVMe의 MSI-X 인터럽트는 이 domain을 통해 Linux virq로 매핑된다.
 *       CONFIG_IRQ_DOMAIN이 꺼져 있으면 MSI를 사용할 수 없다.
 */
struct irq_domain *pci_host_bridge_of_msi_domain(struct pci_bus *bus)
{
#ifdef CONFIG_IRQ_DOMAIN
	struct irq_domain *d;

	if (!bus->dev.of_node)
		return NULL;

	/* Start looking for a phandle to an MSI controller. */
	d = of_msi_get_domain(&bus->dev, bus->dev.of_node, DOMAIN_BUS_PCI_MSI);
	if (d)
		return d;

	/*
	 * If we don't have an msi-parent property, look for a domain
	 * directly attached to the host bridge.
	 */
	d = irq_find_matching_host(bus->dev.of_node, DOMAIN_BUS_PCI_MSI);
	if (d)
		return d;

	return irq_find_host(bus->dev.of_node);
#else
	return NULL;
#endif
}

/*
 * NVMe: DT에 'msi-map' 속성이 있는지 확인한다.
 *       msi-map은 MSI controller와 PCI 장치 간 MSI 번호 매핑을 정의하며,
 *       NVMe MSI-X 경로 설정에 영향을 준다.
 */
bool pci_host_of_has_msi_map(struct device *dev)
{
	if (dev && dev->of_node)
		return of_get_property(dev->of_node, "msi-map", NULL);
	return false;
}

/*
 * NVMe: DT 노드의 reg 속성에서 devfn을 추출하여 기대하는 devfn과 비교한다.
 *       NVMe 장치의 BDF(Bus/Device/Function)가 DT 항목과 일치하는지 검증.
 */
static inline int __of_pci_pci_compare(struct device_node *node,
				       unsigned int data)
{
	int devfn;

	devfn = of_pci_get_devfn(node);
	if (devfn < 0)
		return 0;

	return devfn == data;
}

/*
 * NVMe: 부모 DT 노드 아래에서 특정 devfn에 해당하는 자식 노드를 찾는다.
 *       'multifunc-device' 가짜 루트 아래의 다기능 NVMe/PCI 장치도 탐색.
 */
struct device_node *of_pci_find_child_device(struct device_node *parent,
					     unsigned int devfn)
{
	struct device_node *node, *node2;

	for_each_child_of_node(parent, node) {
		if (__of_pci_pci_compare(node, devfn))
			return node;
		/*
		 * Some OFs create a parent node "multifunc-device" as
		 * a fake root for all functions of a multi-function
		 * device we go down them as well.
		 */
		if (of_node_name_eq(node, "multifunc-device")) {
			for_each_child_of_node(node, node2) {
				if (__of_pci_pci_compare(node2, devfn)) {
					of_node_put(node);
					return node2;
				}
			}
		}
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(of_pci_find_child_device);

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
int of_pci_get_devfn(struct device_node *np)
{
	u32 reg[5];
	int error;

	error = of_property_read_u32_array(np, "reg", reg, ARRAY_SIZE(reg));
	if (error)
		return error;

	return (reg[0] >> 8) & 0xff;
}
EXPORT_SYMBOL_GPL(of_pci_get_devfn);

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
static int of_pci_parse_bus_range(struct device_node *node,
				  struct resource *res)
{
	u32 bus_range[2];
	int error;

	error = of_property_read_u32_array(node, "bus-range", bus_range,
					   ARRAY_SIZE(bus_range));
	if (error)
		return error;

	res->name = node->name;
	res->start = bus_range[0];
	res->end = bus_range[1];
	res->flags = IORESOURCE_BUS;

	return 0;
}

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
int of_get_pci_domain_nr(struct device_node *node)
{
	u32 domain;
	int error;

	error = of_property_read_u32(node, "linux,pci-domain", &domain);
	if (error)
		return error;

	return (u16)domain;
}
EXPORT_SYMBOL_GPL(of_get_pci_domain_nr);

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
bool of_pci_preserve_config(struct device_node *node)
{
	u32 val = 0;
	int ret;

	if (!node) {
		pr_warn("device node is NULL, trying with of_chosen\n");
		node = of_chosen;
	}

retry:
	ret = of_property_read_u32(node, "linux,pci-probe-only", &val);
	if (ret) {
		if (ret == -ENODATA || ret == -EOVERFLOW) {
			pr_warn("Incorrect value for linux,pci-probe-only in %pOF, ignoring\n",
				node);
			return false;
		}
		if (ret == -EINVAL) {
			if (node == of_chosen)
				return false;

			node = of_chosen;
			goto retry;
		}
	}

	if (val)
		return true;
	else
		return false;
}

/*
 * NVMe: 'linux,pci-probe-only'에 따라 PCI_PROBE_ONLY 플래그를 설정/해제한다.
 *       설정되면 NVMe 포함 모든 PCI 장치의 리소스를 firmware 구성 그대로 사용.
 */
/**
 * of_pci_check_probe_only - Setup probe only mode if linux,pci-probe-only
 *                           is present and valid
 */
void of_pci_check_probe_only(void)
{
	if (of_pci_preserve_config(of_chosen))
		pci_add_flags(PCI_PROBE_ONLY);
	else
		pci_clear_flags(PCI_PROBE_ONLY);
}
EXPORT_SYMBOL_GPL(of_pci_check_probe_only);

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
static int devm_of_pci_get_host_bridge_resources(struct device *dev,
			struct list_head *resources,
			struct list_head *ib_resources,
			resource_size_t *io_base)
{
	struct device_node *dev_node = dev->of_node;
	struct resource *res, tmp_res;
	struct resource *bus_range;
	struct of_pci_range range;
	struct of_pci_range_parser parser;
	const char *range_type;
	int err;

	if (io_base)
		*io_base = (resource_size_t)OF_BAD_ADDR;

	bus_range = devm_kzalloc(dev, sizeof(*bus_range), GFP_KERNEL);
	if (!bus_range)
		return -ENOMEM;

	dev_info(dev, "host bridge %pOF ranges:\n", dev_node);

	err = of_pci_parse_bus_range(dev_node, bus_range);
	if (err) {
		bus_range->start = 0;
		bus_range->end = 0xff;
		bus_range->flags = IORESOURCE_BUS;
	} else {
		if (bus_range->end > 0xff) {
			dev_warn(dev, "  Invalid end bus number in %pR, defaulting to 0xff\n",
				 bus_range);
			bus_range->end = 0xff;
		}
	}
	pci_add_resource(resources, bus_range);

	/* Check for ranges property */
	err = of_pci_range_parser_init(&parser, dev_node);
	if (err)
		return 0;

	dev_dbg(dev, "Parsing ranges property...\n");
	for_each_of_pci_range(&parser, &range) {
		/* Read next ranges element */
		if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_IO)
			range_type = "IO";
		else if ((range.flags & IORESOURCE_TYPE_BITS) == IORESOURCE_MEM)
			range_type = "MEM";
		else
			range_type = "err";
		dev_info(dev, "  %6s %#012llx..%#012llx -> %#012llx\n",
			 range_type, range.cpu_addr,
			 range.cpu_addr + range.size - 1, range.pci_addr);

		/*
		 * If we failed translation or got a zero-sized region
		 * then skip this range
		 */
		if (range.cpu_addr == OF_BAD_ADDR || range.size == 0)
			continue;

		err = of_pci_range_to_resource(&range, dev_node, &tmp_res);
		if (err)
			continue;

		res = devm_kmemdup(dev, &tmp_res, sizeof(tmp_res), GFP_KERNEL);
		if (!res) {
			err = -ENOMEM;
			goto failed;
		}

		if (resource_type(res) == IORESOURCE_IO) {
			if (!io_base) {
				dev_err(dev, "I/O range found for %pOF. Please provide an io_base pointer to save CPU base address\n",
					dev_node);
				err = -EINVAL;
				goto failed;
			}
			if (*io_base != (resource_size_t)OF_BAD_ADDR)
				dev_warn(dev, "More than one I/O resource converted for %pOF. CPU base address for old range lost!\n",
					 dev_node);
			*io_base = range.cpu_addr;
		} else if (resource_type(res) == IORESOURCE_MEM) {
			res->flags &= ~IORESOURCE_MEM_64;
		}

		pci_add_resource_offset(resources, res,	res->start - range.pci_addr);
	}

	/* Check for dma-ranges property */
	if (!ib_resources)
		return 0;
	err = of_pci_dma_range_parser_init(&parser, dev_node);
	if (err)
		return 0;

	dev_dbg(dev, "Parsing dma-ranges property...\n");
	for_each_of_pci_range(&parser, &range) {
		/*
		 * If we failed translation or got a zero-sized region
		 * then skip this range
		 */
		if (((range.flags & IORESOURCE_TYPE_BITS) != IORESOURCE_MEM) ||
		    range.cpu_addr == OF_BAD_ADDR || range.size == 0)
			continue;

		dev_info(dev, "  %6s %#012llx..%#012llx -> %#012llx\n",
			 "IB MEM", range.cpu_addr,
			 range.cpu_addr + range.size - 1, range.pci_addr);


		err = of_pci_range_to_resource(&range, dev_node, &tmp_res);
		if (err)
			continue;

		res = devm_kmemdup(dev, &tmp_res, sizeof(tmp_res), GFP_KERNEL);
		if (!res) {
			err = -ENOMEM;
			goto failed;
		}

		pci_add_resource_offset(ib_resources, res,
					res->start - range.pci_addr);
	}

	return 0;

failed:
	pci_free_resource_list(resources);
	return err;
}

/*
 * NVMe: DT를 기반으로 PCI 장치의 INTx 인터럽트를 해석한다.
 *       NVMe가 MSI-X 대신 INTx(legacy)를 사용할 때 호출되며,
 *       swizzling과 interrupt-map을 따라 상위 버스/bridge로 올라가며 해석.
 */
#if IS_ENABLED(CONFIG_OF_IRQ)
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
	struct device_node *dn, *ppnode = NULL;
	struct pci_dev *ppdev;
	__be32 laddr[3];
	u8 pin;
	int rc;

	/*
	 * Check if we have a device node, if yes, fallback to standard
	 * device tree parsing
	 */
	dn = pci_device_to_OF_node(pdev);
	if (dn) {
		rc = of_irq_parse_one(dn, 0, out_irq);
		if (!rc)
			return rc;
	}

	/*
	 * Ok, we don't, time to have fun. Let's start by building up an
	 * interrupt spec.  we assume #interrupt-cells is 1, which is standard
	 * for PCI. If you do different, then don't use that routine.
	 */
	rc = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
	if (rc != 0)
		goto err;
	/* No pin, exit with no error message. */
	if (pin == 0)
		return -ENODEV;

	/* Local interrupt-map in the device node? Use it! */
	if (of_property_present(dn, "interrupt-map")) {
		pin = pci_swizzle_interrupt_pin(pdev, pin);
		ppnode = dn;
	}

	/* Now we walk up the PCI tree */
	while (!ppnode) {
		/* Get the pci_dev of our parent */
		ppdev = pdev->bus->self;

		/* Ouch, it's a host bridge... */
		if (ppdev == NULL) {
			ppnode = pci_bus_to_OF_node(pdev->bus);

			/* No node for host bridge ? give up */
			if (ppnode == NULL) {
				rc = -EINVAL;
				goto err;
			}
		} else {
			/* We found a P2P bridge, check if it has a node */
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
		if (ppnode)
			break;

		/*
		 * We can only get here if we hit a P2P bridge with no node;
		 * let's do standard swizzling and try again
		 */
		pin = pci_swizzle_interrupt_pin(pdev, pin);
		pdev = ppdev;
	}

	out_irq->np = ppnode;
	out_irq->args_count = 1;
	out_irq->args[0] = pin;
	laddr[0] = cpu_to_be32((pdev->bus->number << 16) | (pdev->devfn << 8));
	laddr[1] = laddr[2] = cpu_to_be32(0);
	rc = of_irq_parse_raw(laddr, out_irq);
	if (rc)
		goto err;
	return 0;
err:
	if (rc == -ENOENT) {
		dev_warn(&pdev->dev,
			"%s: no interrupt-map found, INTx interrupts not available\n",
			__func__);
		pr_warn_once("%s: possibly some PCI slots don't have level triggered interrupts capability\n",
			__func__);
	} else {
		dev_err(&pdev->dev, "%s: failed with rc=%d\n", __func__, rc);
	}
	return rc;
}

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
int of_irq_parse_and_map_pci(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct of_phandle_args oirq;
	int ret;

	ret = of_irq_parse_pci(dev, &oirq);
	if (ret)
		return 0; /* Proper return code 0 == NO_IRQ */

	return irq_create_of_mapping(&oirq);
}
EXPORT_SYMBOL_GPL(of_irq_parse_and_map_pci);
#endif	/* CONFIG_OF_IRQ */

/*
 * NVMe: devm_of_pci_get_host_bridge_resources()로 추출한 리소스를 요청하고,
 *       IO/MEM 윈도우를 bridge->windows에 등록한다.
 *       non-prefetchable MEM 리소스 존재 여부를 검사한다.
 */
static int pci_parse_request_of_pci_ranges(struct device *dev,
					   struct pci_host_bridge *bridge)
{
	int err, res_valid = 0;
	resource_size_t iobase;
	struct resource_entry *win, *tmp;

	INIT_LIST_HEAD(&bridge->windows);
	INIT_LIST_HEAD(&bridge->dma_ranges);

	err = devm_of_pci_get_host_bridge_resources(dev, &bridge->windows,
						    &bridge->dma_ranges, &iobase);
	if (err)
		return err;

	err = devm_request_pci_bus_resources(dev, &bridge->windows);
	if (err)
		return err;

	resource_list_for_each_entry_safe(win, tmp, &bridge->windows) {
		struct resource *res = win->res;

		switch (resource_type(res)) {
		case IORESOURCE_IO:
			err = devm_pci_remap_iospace(dev, res, iobase);
			if (err) {
				dev_warn(dev, "error %d: failed to map resource %pR\n",
					 err, res);
				resource_list_destroy_entry(win);
			}
			break;
		case IORESOURCE_MEM:
			res_valid |= !(res->flags & IORESOURCE_PREFETCH);

			if (!(res->flags & IORESOURCE_PREFETCH))
				if (upper_32_bits(resource_size(res)))
					dev_warn(dev, "Memory resource size exceeds max for 32 bits\n");

			break;
		}
	}

	if (!res_valid)
		dev_warn(dev, "non-prefetchable memory resource required\n");

	return 0;
}

/*
 * NVMe: host bridge를 DT 기반으로 초기화한다.
 *       swizzle_irq, map_irq 콜백을 설정하고, ranges/dma-ranges를 파싱하여
 *       NVMe DMA를 위한 bridge 리소스 윈도우를 준비한다.
 */
int devm_of_pci_bridge_init(struct device *dev, struct pci_host_bridge *bridge)
{
	if (!dev->of_node)
		return 0;

	bridge->swizzle_irq = pci_common_swizzle;
	bridge->map_irq = of_irq_parse_and_map_pci;

	return pci_parse_request_of_pci_ranges(dev, bridge);
}

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
void of_pci_remove_node(struct pci_dev *pdev)
{
	struct device_node *np;

	np = pci_device_to_OF_node(pdev);
	if (!np || !of_node_check_flag(np, OF_DYNAMIC))
		return;

	device_remove_of_node(&pdev->dev);
	of_changeset_revert(np->data);
	of_changeset_destroy(np->data);
	of_node_put(np);
}

/*
 * NVMe: pci_dev에 아직 of_node가 없으면 동적으로 생성한다.
 *       NVMe 장치가 hotplug로 추가되었을 때 사용자 공간/드라이버가
 *       DT 기반으로 인식할 수 있도록 노드를 만든다.
 */
void of_pci_make_dev_node(struct pci_dev *pdev)
{
	struct device_node *ppnode, *np = NULL;
	const char *pci_type;
	struct of_changeset *cset;
	const char *name;
	int ret;

	/*
	 * If there is already a device tree node linked to this device,
	 * return immediately.
	 */
	if (pci_device_to_OF_node(pdev))
		return;

	/* Check if there is device tree node for parent device */
	if (!pdev->bus->self)
		ppnode = pdev->bus->dev.of_node;
	else
		ppnode = pdev->bus->self->dev.of_node;
	if (!ppnode)
		return;

	if (pci_is_bridge(pdev))
		pci_type = "pci";
	else
		pci_type = "dev";

	name = kasprintf(GFP_KERNEL, "%s@%x,%x", pci_type,
			 PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
	if (!name)
		return;

	cset = kmalloc_obj(*cset);
	if (!cset)
		goto out_free_name;
	of_changeset_init(cset);

	np = of_changeset_create_node(cset, ppnode, name);
	if (!np)
		goto out_destroy_cset;

	ret = of_pci_add_properties(pdev, cset, np);
	if (ret)
		goto out_free_node;

	ret = of_changeset_apply(cset);
	if (ret)
		goto out_free_node;

	np->data = cset;

	ret = device_add_of_node(&pdev->dev, np);
	if (ret)
		goto out_revert_cset;

	kfree(name);

	return;

out_revert_cset:
	np->data = NULL;
	of_changeset_revert(cset);
out_free_node:
	of_node_put(np);
out_destroy_cset:
	of_changeset_destroy(cset);
	kfree(cset);
out_free_name:
	kfree(name);
}

/*
 * NVMe: 동적으로 생성된 host bridge의 DT 노드를 제거한다.
 *       NVMe root complex가 제거될 때 bus/device 양쪽의 of_node 연결을 정리.
 */
void of_pci_remove_host_bridge_node(struct pci_host_bridge *bridge)
{
	struct device_node *np;

	np = pci_bus_to_OF_node(bridge->bus);
	if (!np || !of_node_check_flag(np, OF_DYNAMIC))
		return;

	device_remove_of_node(&bridge->bus->dev);
	device_remove_of_node(&bridge->dev);
	of_changeset_revert(np->data);
	of_changeset_destroy(np->data);
	of_node_put(np);
}

/*
 * NVMe: root bus에 of_node가 없으면 동적으로 host bridge DT 노드를 생성한다.
 *       OF_POPULATED 플래그를 설정하여 불필요한 platform device 생성을 방지.
 */
void of_pci_make_host_bridge_node(struct pci_host_bridge *bridge)
{
	struct device_node *np = NULL;
	struct of_changeset *cset;
	const char *name;
	int ret;

	/*
	 * If there is already a device tree node linked to the PCI bus handled
	 * by this bridge (i.e. the PCI root bus), nothing to do.
	 */
	if (pci_bus_to_OF_node(bridge->bus))
		return;

	/*
	 * The root bus has no node. Check that the host bridge has no node
	 * too
	 */
	if (bridge->dev.of_node) {
		dev_err(&bridge->dev, "PCI host bridge of_node already set");
		return;
	}

	/* Check if there is a DT root node to attach the created node */
	if (!of_root) {
		pr_debug("of_root node is NULL, cannot create PCI host bridge node\n");
		return;
	}

	name = kasprintf(GFP_KERNEL, "pci@%x,%x", pci_domain_nr(bridge->bus),
			 bridge->bus->number);
	if (!name)
		return;

	cset = kmalloc_obj(*cset);
	if (!cset)
		goto out_free_name;
	of_changeset_init(cset);

	np = of_changeset_create_node(cset, of_root, name);
	if (!np)
		goto out_destroy_cset;

	ret = of_pci_add_host_bridge_properties(bridge, cset, np);
	if (ret)
		goto out_free_node;

	/*
	 * This of_node will be added to an existing device. The of_node parent
	 * is the root OF node and so this node will be handled by the platform
	 * bus. Avoid any new device creation.
	 */
	of_node_set_flag(np, OF_POPULATED);
	np->fwnode.dev = &bridge->dev;
	fwnode_dev_initialized(&np->fwnode, true);

	ret = of_changeset_apply(cset);
	if (ret)
		goto out_free_node;

	np->data = cset;

	/* Add the of_node to host bridge and the root bus */
	ret = device_add_of_node(&bridge->dev, np);
	if (ret)
		goto out_revert_cset;

	ret = device_add_of_node(&bridge->bus->dev, np);
	if (ret)
		goto out_remove_bridge_dev_of_node;

	kfree(name);

	return;

out_remove_bridge_dev_of_node:
	device_remove_of_node(&bridge->dev);
out_revert_cset:
	np->data = NULL;
	of_changeset_revert(cset);
out_free_node:
	of_node_put(np);
out_destroy_cset:
	of_changeset_destroy(cset);
	kfree(cset);
out_free_name:
	kfree(name);
}

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
bool of_pci_supply_present(struct device_node *np)
{
	struct property *prop;
	char *supply;

	if (!np)
		return false;

	for_each_property_of_node(np, prop) {
		supply = strrchr(prop->name, '-');
		if (supply && !strcmp(supply, "-supply"))
			return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(of_pci_supply_present);

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
int of_pci_get_max_link_speed(struct device_node *node)
{
	u32 max_link_speed;
	int ret;

	ret = of_property_read_u32(node, "max-link-speed", &max_link_speed);
	if (ret)
		return ret;

	return max_link_speed;
}
EXPORT_SYMBOL_GPL(of_pci_get_max_link_speed);

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
u32 of_pci_get_slot_power_limit(struct device_node *node,
				u8 *slot_power_limit_value,
				u8 *slot_power_limit_scale)
{
	u32 slot_power_limit_mw;
	u8 value, scale;

	if (of_property_read_u32(node, "slot-power-limit-milliwatt",
				 &slot_power_limit_mw))
		slot_power_limit_mw = 0;

	/* Calculate Slot Power Limit Value and Slot Power Limit Scale */
	if (slot_power_limit_mw == 0) {
		value = 0x00;
		scale = 0;
	} else if (slot_power_limit_mw <= 255) {
		value = slot_power_limit_mw;
		scale = 3;
	} else if (slot_power_limit_mw <= 255*10) {
		value = slot_power_limit_mw / 10;
		scale = 2;
		slot_power_limit_mw = slot_power_limit_mw / 10 * 10;
	} else if (slot_power_limit_mw <= 255*100) {
		value = slot_power_limit_mw / 100;
		scale = 1;
		slot_power_limit_mw = slot_power_limit_mw / 100 * 100;
	} else if (slot_power_limit_mw <= 239*1000) {
		value = slot_power_limit_mw / 1000;
		scale = 0;
		slot_power_limit_mw = slot_power_limit_mw / 1000 * 1000;
	} else if (slot_power_limit_mw < 250*1000) {
		value = 0xEF;
		scale = 0;
		slot_power_limit_mw = 239*1000;
	} else if (slot_power_limit_mw <= 600*1000) {
		value = 0xF0 + (slot_power_limit_mw / 1000 - 250) / 25;
		scale = 0;
		slot_power_limit_mw = slot_power_limit_mw / (1000*25) * (1000*25);
	} else {
		value = 0xFE;
		scale = 0;
		slot_power_limit_mw = 600*1000;
	}

	if (slot_power_limit_value)
		*slot_power_limit_value = value;

	if (slot_power_limit_scale)
		*slot_power_limit_scale = scale;

	return slot_power_limit_mw;
}
EXPORT_SYMBOL_GPL(of_pci_get_slot_power_limit);

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
int of_pci_get_equalization_presets(struct device *dev,
				    struct pci_eq_presets *presets,
				    int num_lanes)
{
	char name[20];
	int ret;

	presets->eq_presets_8gts[0] = PCI_EQ_RESV;
	ret = of_property_read_u16_array(dev->of_node, "eq-presets-8gts",
					 presets->eq_presets_8gts, num_lanes);
	if (ret && ret != -EINVAL) {
		dev_err(dev, "Error reading eq-presets-8gts: %d\n", ret);
		return ret;
	}

	for (int i = 0; i < EQ_PRESET_TYPE_MAX - 1; i++) {
		presets->eq_presets_Ngts[i][0] = PCI_EQ_RESV;
		snprintf(name, sizeof(name), "eq-presets-%dgts", 8 << (i + 1));
		ret = of_property_read_u8_array(dev->of_node, name,
						presets->eq_presets_Ngts[i],
						num_lanes);
		if (ret && ret != -EINVAL) {
			dev_err(dev, "Error reading %s: %d\n", name, ret);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(of_pci_get_equalization_presets);
