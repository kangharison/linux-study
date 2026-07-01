// SPDX-License-Identifier: GPL-2.0
/*
 * PCI searching functions
 *
 * Copyright (C) 1993 -- 1997 Drew Eckhardt, Frederic Potter,
 *					David Mosberger-Tang
 * Copyright (C) 1997 -- 2000 Martin Mares <mj@ucw.cz>
 * Copyright (C) 2003 -- 2004 Greg Kroah-Hartman <greg@kroah.com>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/search.c)은 PCI 버스/장치 검색 유틸리티를 제공한다.
 * NVMe SSD 입장에서 본 파일은 다음과 같은 경로에서 간접적으로 사용된다.
 *   - NVMe probe: nvme_probe() -> pci_enable_device() 등에서 참조하는
 *     pdev가 이미 PCI core의 장치 검색 결과물이다. pci_get_class() 같은
 *     함수로 class code 0x010802(NVM Express controller)를 갖는 장치를
 *     찾을 때 사용된다.
 *   - DMA alias 탐색: pci_for_each_dma_alias()는 IOMMU, P2PDMA, ATS 등에서
 *     NVMe 장치의 DMA requester ID가 bridge/alias를 거쳐 root bus에 도달하는
 *     경로를 식별할 때 사용된다. NVMe의 DMA 메모리(dma_pool, PRP/SGL,
 *     Host Memory Buffer)가 실제로 어떤 requester ID로 노출되는지 파악하는
 *     데 핵심적이다.
 *   - 버스/장치 검색: pci_find_bus(), pci_find_next_bus(), pci_get_slot(),
 *     pci_get_domain_bus_and_slot() 등은 NVMe가 연결된 bus/slot을 식별하고,
 *     상위 Root Port/Upstream Port를 찾을 때 사용된다. ASPM, MSI-X, BAR
 *     할당, 전원 관리 등은 모두 이 검색 결과를 기반으로 동작한다.
 *   - ID/클스 검색: pci_get_device(), pci_get_subsys(), pci_get_class(),
 *     pci_get_base_class(), pci_dev_present()는 vendor/device ID 또는 class
 *     code 기반으로 NVMe 컨트롤러를 매칭하거나 존재 여부를 확인할 때
 *     사용된다. 예: drivers/nvme/host/pci.c의 nvme_id_table.
 * 본 파일은 drivers/nvme/host/pci.c에서 직접 호출하지 않을 수도 있지만,
 * NVMe 장치의 탐색, DMA alias 해석, 상위 bridge/root port 식별 등에
 * 근간이 되는 PCI core 함수군이다.
 * ===================================================================
 */

#include <linux/pci.h>		/* NVMe: PCI core 헤더. NVMe endpoint/bridge/BAR/DMA 관련 구조체와 함수 선언. */
#include <linux/slab.h>		/* NVMe: kmalloc/kzalloc 등 동적 메모리 할당 API 선언. */
#include <linux/module.h>	/* NVMe: EXPORT_SYMBOL, module 매크로 등 선언. */
#include <linux/interrupt.h>	/* NVMe: MSI-X/인터럽트 관련 정의. NVMe doorbell 인터럽트 경로. */
#include "pci.h"		/* NVMe: PCI core 낮은 수준 헤더. pci_bus_sem, pci_root_buses 등 선언. */

/* NVMe: 전역 PCI bus 리스트에 대한 읽기/쓰기 세마포어. NVMe probe/remove 시 bus tree 보호. */
DECLARE_RWSEM(pci_bus_sem);

/*
 * pci_for_each_dma_alias - Iterate over DMA aliases for a device
 * @pdev: starting downstream device
 * @fn: function to call for each alias
 * @data: opaque data to pass to @fn
 *
 * Starting @pdev, walk up the bus calling @fn for each possible alias
 * of @pdev at the root bus.
 */
/*
 * NVMe: NVMe 컨트롤러의 DMA alias(requester ID)를 root bus 방향으로 순회한다.
 *       NVMe의 PRP/SGL/HMB DMA, doorbell DMA 등이 IOMMU 뒤에서 어떤 alias로
 *       보이는지 파악할 때 사용된다. P2PDMA나 ATS 활성화 시 alias가 중요하다.
 */
int pci_for_each_dma_alias(struct pci_dev *pdev,	/* NVMe: DMA alias 탐색을 시작할 NVMe endpoint pci_dev. */
			   int (*fn)(struct pci_dev *pdev,	/* NVMe: alias 발견 시 호출할 콜백 함수 포인터. */
				     u16 alias, void *data), void *data)	/* NVMe: alias 값과 NVMe 드라이버 전용 데이터. */
{	/* NVMe: NVMe DMA alias 순회 함수 본문 시작. */
	struct pci_bus *bus; /* NVMe: NVMe 장치가 연결된 bus를 따라 root 방향으로 이동할 포인터. */
	int ret; /* NVMe: 콜백 fn의 반환값을 누적 저장. */

	/*
	 * The device may have an explicit alias requester ID for DMA where the
	 * requester is on another PCI bus.
	 */
	/* NVMe: 실제 DMA를 수행하는 PCI device를 얻는다(Single Root I/O Virtualization 등 alias 가능). */
	pdev = pci_real_dma_dev(pdev);

	/* NVMe: NVMe endpoint 자신의 DMA alias를 먼저 콜백에 전달. */
	ret = fn(pdev, pci_dev_id(pdev), data);
	/* NVMe: 콜백이 0이 아닌 값을 반환하면(예: IOMMU 그룹 매칭 완료) 즉시 순회 종료. */
	if (ret)
		return ret;

	/*
	 * If the device is broken and uses an alias requester ID for
	 * DMA, iterate over that too.
	 */
	/* NVMe: firmware/quirk상 dma_alias_mask가 설정된 broken alias가 있다면 추가로 순회. */
	if (unlikely(pdev->dma_alias_mask)) {
		unsigned int devfn; /* NVMe: alias mask에서 설정된 각 devfn을 순회. */

		for_each_set_bit(devfn, pdev->dma_alias_mask, MAX_NR_DEVFNS) {
			/* NVMe: alias된 bus number와 devfn으로 requester ID를 만들어 콜백 호출. */
			ret = fn(pdev, PCI_DEVID(pdev->bus->number, devfn),
				 data);
			/* NVMe: 콜백이 중단 요청하면 즉시 리턴. */
			if (ret)
				return ret;
		}	/* NVMe: dma_alias_mask 순회 종료. */
	}	/* NVMe: broken alias 처리 블록 종료. */

	/* NVMe: NVMe 장치가 연결된 bus에서 시작해 root bus까지 거슬러 올라간다. */
	for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
		struct pci_dev *tmp; /* NVMe: 현재 bus의 상위 bridge device. */

		/* Skip virtual buses */
		/* NVMe: 가상 bus(예: PCIe switch의 논리적 bus)는 skip. */
		if (!bus->self)
			continue;

		tmp = bus->self; /* NVMe: 상위 bridge의 pci_dev 획득. */

		/* stop at bridge where translation unit is associated */
		/* NVMe: translation unit가 연결된 bridge면 더 이상 올라가지 않는다. */
		if (tmp->dev_flags & PCI_DEV_FLAGS_BRIDGE_XLATE_ROOT)
			return ret;

		/*
		 * PCIe-to-PCI/X bridges alias transactions from downstream
		 * devices using the subordinate bus number (PCI Express to
		 * PCI/PCI-X Bridge Spec, rev 1.0, sec 2.3).  For all cases
		 * where the upstream bus is PCI/X we alias to the bridge
		 * (there are various conditions in the previous reference
		 * where the bridge may take ownership of transactions, even
		 * when the secondary interface is PCI-X).
		 */
		/* NVMe: PCIe bridge 종류에 따라 alias requester ID를 다르게 처리. */
		if (pci_is_pcie(tmp)) {
			/* NVMe: bridge의 PCIe 포트 타입에 따라 분기. */
			switch (pci_pcie_type(tmp)) {
			case PCI_EXP_TYPE_ROOT_PORT:	/* NVMe: Root Port case. */
			case PCI_EXP_TYPE_UPSTREAM:	/* NVMe: Switch Upstream Port case. */
			case PCI_EXP_TYPE_DOWNSTREAM:	/* NVMe: Switch Downstream Port case. */
				/* NVMe: Root Port/Switch 포트는 NVMe 장치의 requester ID를 그대로 전달하므로 skip. */
				continue;
			case PCI_EXP_TYPE_PCI_BRIDGE:	/* NVMe: PCIe-to-PCI bridge case. */
				/* NVMe: PCIe-to-PCI bridge는 subordinate bus 기준으로 alias를 만든다. */
				if (tmp->dev_flags & PCI_DEV_FLAGS_PCI_BRIDGE_NO_ALIAS)
					continue;	/* NVMe: alias가 금지된 bridge면 skip. */
				ret = fn(tmp,
					 PCI_DEVID(tmp->subordinate->number,
						   PCI_DEVFN(0, 0)), data);
				/* NVMe: IOMMU 그룹 매칭 완료 시 중단. */
				if (ret)
					return ret;
				continue;	/* NVMe: 다음 상위 bridge로 이동. */
			case PCI_EXP_TYPE_PCIE_BRIDGE:	/* NVMe: PCIe bridge case. */
				/* NVMe: PCIe bridge는 bridge 자신의 ID를 alias로 전달. */
				ret = fn(tmp, pci_dev_id(tmp), data);
				if (ret)
					return ret;
				continue;	/* NVMe: 다음 상위 bridge로 이동. */
			}	/* NVMe: PCIe bridge 종류 switch 종료. */
		} else {
			/* NVMe: legacy PCI bridge의 경우 PCIe bridge alias 플래그에 따라 분기. */
			if (tmp->dev_flags & PCI_DEV_FLAG_PCIE_BRIDGE_ALIAS)
				ret = fn(tmp,
					 PCI_DEVID(tmp->subordinate->number,
						   PCI_DEVFN(0, 0)), data);
			else
				/* NVMe: alias가 아니면 bridge 자신의 ID를 콜백에 전달. */
				ret = fn(tmp, pci_dev_id(tmp), data);
			/* NVMe: 콜백이 중단을 원하면 순회 종료. */
			if (ret)
				return ret;
		}	/* NVMe: legacy PCI bridge 처리 블록 종료. */
	}	/* NVMe: root bus까지의 alias 순회 종료. */

	return ret; /* NVMe: root bus까지 alias 탐색을 마친 최종 반환값. */
}	/* NVMe: pci_for_each_dma_alias 함수 종료. */

/*
 * NVMe: 재귀적으로 bus number에 해당하는 pci_bus를 찾는 낮은 수준 함수.
 *       NVMe 장치가 연결된 하위 bus를 root bus에서부터 탐색할 때 사용.
 */
static struct pci_bus *pci_do_find_bus(struct pci_bus *bus, unsigned char busnr)	/* NVMe: 현재 bus와 찾을 bus number를 인자로 받는다. */
{	/* NVMe: pci_do_find_bus 함수 본문 시작. */
	struct pci_bus *child; /* NVMe: 재귀 호출 결과로 얻은 하위 bus. */
	struct pci_bus *tmp; /* NVMe: 현재 bus의 children 리스트를 순회할 포인터. */

	/* NVMe: 현재 bus 번호가 찾는 번호와 일치하면 반환. */
	if (bus->number == busnr)
		return bus;

	/* NVMe: 현재 bus의 모든 child bus를 순회하며 재귀 탐색. */
	list_for_each_entry(tmp, &bus->children, node) {
		child = pci_do_find_bus(tmp, busnr);
		/* NVMe: 하위 트리에서 찾았으면 즉시 반환. */
		if (child)
			return child;
	}	/* NVMe: children 순회 종료. */
	return NULL; /* NVMe: 해당 bus 번호를 찾지 못하면 NULL. */
}	/* NVMe: pci_do_find_bus 함수 종료. */

/**
 * pci_find_bus - locate PCI bus from a given domain and bus number
 * @domain: number of PCI domain to search
 * @busnr: number of desired PCI bus
 *
 * Given a PCI bus number and domain number, the desired PCI bus is located
 * in the global list of PCI buses.  If the bus is found, a pointer to its
 * data structure is returned.  If no bus is found, %NULL is returned.
 */
/*
 * NVMe: 지정된 PCI domain(segment)과 bus number에 해당하는 pci_bus를 찾는다.
 *       NVMe 컨트롤러가 연결된 bus를 식별해 Root Port/Upstream Port 정보를
 *       얻거나, BAR/DMA/MSI-X 설정 시 bus context를 확보할 때 사용.
 */
struct pci_bus *pci_find_bus(int domain, int busnr)	/* NVMe: 검색할 PCI domain과 bus number. */
{	/* NVMe: pci_find_bus 함수 본문 시작. */
	struct pci_bus *bus = NULL; /* NVMe: pci_find_next_bus의 시작점(NULL이면 처음부터). */
	struct pci_bus *tmp_bus; /* NVMe: 재귀 탐색으로 발견한 bus. */

	/* NVMe: 전역 root bus 리스트를 순회. */
	while ((bus = pci_find_next_bus(bus)) != NULL)  {
		/* NVMe: domain 번호가 다른면 skip. */
		if (pci_domain_nr(bus) != domain)
			continue;
		/* NVMe: 동일 domain의 root bus 아래에서 busnr 탐색. */
		tmp_bus = pci_do_find_bus(bus, busnr);
		/* NVMe: 발견하면 해당 pci_bus 포인터 반환. */
		if (tmp_bus)
			return tmp_bus;
	}	/* NVMe: root bus 리스트 순회 종료. */
	return NULL; /* NVMe: 전체 PCI 트리에서 bus를 찾지 못함. */
}	/* NVMe: pci_find_bus 함수 종료. */
EXPORT_SYMBOL(pci_find_bus);	/* NVMe: NVMe 및 다른 드라이버에서 pci_find_bus를 호출할 수 있도록 심볼 노출. */

/**
 * pci_find_next_bus - begin or continue searching for a PCI bus
 * @from: Previous PCI bus found, or %NULL for new search.
 *
 * Iterates through the list of known PCI buses.  A new search is
 * initiated by passing %NULL as the @from argument.  Otherwise if
 * @from is not %NULL, searches continue from next device on the
 * global list.
 */
/*
 * NVMe: 전역 pci_root_buses 리스트에서 다음 root bus를 찾는다.
 *       멀티 도메인/멀티 Root Complex 시스템에서 NVMe 컨트롤러가 속한
 *       domain을 찾을 때 pci_find_bus() 낶에서 간접적으로 사용.
 */
struct pci_bus *pci_find_next_bus(const struct pci_bus *from)	/* NVMe: 이전에 찾은 bus, NULL이면 처음부터. */
{	/* NVMe: pci_find_next_bus 함수 본문 시작. */
	struct list_head *n; /* NVMe: 리스트의 다음 노드 포인터. */
	struct pci_bus *b = NULL; /* NVMe: 찾은 bus를 저장(NULL이면 끝에 도달). */

	/* NVMe: pci_bus_sem read lock으로 root bus 리스트 보호. */
	down_read(&pci_bus_sem);
	/* NVMe: from이 NULL이면 리스트 head 다음부터, 아니면 from 다음 노드부터. */
	n = from ? from->node.next : pci_root_buses.next;
	/* NVMe: 끝 sentinel이 아니면 list_entry로 pci_bus 포인터 복원. */
	if (n != &pci_root_buses)
		b = list_entry(n, struct pci_bus, node);
	/* NVMe: read lock 해제. */
	up_read(&pci_bus_sem);
	return b; /* NVMe: 다음 root bus 또는 NULL. */
}	/* NVMe: pci_find_next_bus 함수 종료. */
EXPORT_SYMBOL(pci_find_next_bus);	/* NVMe: NVMe 등에서 pci_find_next_bus 심볼을 사용할 수 있도록 노출. */

/**
 * pci_get_slot - locate PCI device for a given PCI slot
 * @bus: PCI bus on which desired PCI device resides
 * @devfn: encodes number of PCI slot in which the desired PCI
 * device resides and the logical device number within that slot
 * in case of multi-function devices.
 *
 * Given a PCI bus and slot/function number, the desired PCI device
 * is located in the list of PCI devices.
 * If the device is found, its reference count is increased and this
 * function returns a pointer to its data structure.  The caller must
 * decrement the reference count by calling pci_dev_put().
 * If no device is found, %NULL is returned.
 */
/*
 * NVMe: 주어진 bus와 slot/function(devfn)에 해당하는 pci_dev를 찾는다.
 *       NVMe 컨트롤러 자신이나 상위 Root Port, switch upstream/downstream
 *       port를 특정 bus:slot.fn으로 조회할 때 사용. 반환된 pci_dev는
 *       pci_dev_put()으로 참조 해제해야 한다.
 */
struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn)	/* NVMe: 대상 bus와 slot/function 번호. */
{	/* NVMe: pci_get_slot 함수 본문 시작. */
	struct pci_dev *dev; /* NVMe: bus->devices 리스트를 순회할 포인터. */

	/* NVMe: bus의 device 리스트 보호를 위해 read lock 획득. */
	down_read(&pci_bus_sem);

	/* NVMe: 해당 bus에 연결된 모든 PCI 장치를 순회. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* NVMe: devfn이 일치하면 검색 성공. */
		if (dev->devfn == devfn)
			goto out;
	}	/* NVMe: bus device 리스트 순회 종료. */

	dev = NULL; /* NVMe: 일치하는 장치가 없으면 NULL로 설정. */
 out:	/* NVMe: 검색 성공/실패 공통 처리 레이블. */
	/* NVMe: 발견한 pci_dev의 참조 카운트 증가(NULL이면 무시됨). */
	pci_dev_get(dev);
	/* NVMe: read lock 해제. */
	up_read(&pci_bus_sem);
	return dev; /* NVMe: NVMe 관련 pci_dev 또는 NULL. */
}	/* NVMe: pci_get_slot 함수 종료. */
EXPORT_SYMBOL(pci_get_slot);	/* NVMe: NVMe 드라이버 등에서 pci_get_slot 심볼을 사용할 수 있도록 노출. */

/**
 * pci_get_domain_bus_and_slot - locate PCI device for a given PCI domain (segment), bus, and slot
 * @domain: PCI domain/segment on which the PCI device resides.
 * @bus: PCI bus on which desired PCI device resides
 * @devfn: encodes number of PCI slot in which the desired PCI
 * device resides and the logical device number within that slot in case of
 * multi-function devices.
 *
 * Given a PCI domain, bus, and slot/function number, the desired PCI
 * device is located in the list of PCI devices. If the device is
 * found, its reference count is increased and this pointer to its
 * data structure is returned.  The caller must decrement the
 * reference count by calling pci_dev_put().  If no device is found,
 * %NULL is returned.
 */
/*
 * NVMe: domain+bus+devfn으로 NVMe 컨트롤러나 Root Port를 직접 찾는다.
 *       예: SR-IOV PF/VF, 특정 segment의 NVMe endpoint를 식별할 때.
 */
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,	/* NVMe: 검색할 PCI domain과 bus number. */
					    unsigned int devfn)	/* NVMe: slot/function 번호. */
{	/* NVMe: pci_get_domain_bus_and_slot 함수 본문 시작. */
	struct pci_dev *dev = NULL; /* NVMe: for_each_pci_dev 루프 변수. */

	/* NVMe: 전역 PCI 장치 리스트를 순회. */
	for_each_pci_dev(dev) {
		/* NVMe: domain, bus number, devfn이 모두 일치하면 반환. */
		if (pci_domain_nr(dev->bus) == domain &&
		    (dev->bus->number == bus && dev->devfn == devfn))
			return dev;
	}	/* NVMe: 전역 PCI 장치 리스트 순회 종료. */
	return NULL; /* NVMe: 조건에 맞는 장치 없음. */
}	/* NVMe: pci_get_domain_bus_and_slot 함수 종료. */
EXPORT_SYMBOL(pci_get_domain_bus_and_slot);	/* NVMe: NVMe 등에서 domain+bus+slot 기반 검색 심볼 사용 가능. */

/*
 * NVMe: pci_device_id 하나와 pci_dev가 매칭되는지 검사하는 helper.
 *       NVMe 드라이버의 nvme_id_table 매칭 로직과 동일한 pci_match_one_device 사용.
 */
static int match_pci_dev_by_id(struct device *dev, const void *data)	/* NVMe: generic device와 비교할 ID 패턴 포인터. */
{	/* NVMe: match_pci_dev_by_id 함수 본문 시작. */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: device 구조체에서 pci_dev 추출. */
	const struct pci_device_id *id = data; /* NVMe: 비교 대상 ID 패턴. */

	/* NVMe: vendor/device/class 등이 일치하면 1 반환. */
	if (pci_match_one_device(id, pdev))
		return 1;
	return 0; /* NVMe: 매칭 실패. */
}	/* NVMe: match_pci_dev_by_id 함수 종료. */

/*
 * pci_get_dev_by_id - begin or continue searching for a PCI device by id
 * @id: pointer to struct pci_device_id to match for the device
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is found
 * with a matching id a pointer to its device structure is returned, and the
 * reference count to the device is incremented.  Otherwise, %NULL is returned.
 * A new search is initiated by passing %NULL as the @from argument.  Otherwise
 * if @from is not %NULL, searches continue from next device on the global
 * list.  The reference count for @from is always decremented if it is not
 * %NULL.
 *
 * This is an internal function for use by the other search functions in
 * this file.
 */
/*
 * NVMe: 주어진 pci_device_id 패턴과 일치하는 PCI 장치를 전역 리스트에서
 *       순방향으로 검색. NVMe quirk 매칭이나 class-based 검색의 밑바탕.
 */
static struct pci_dev *pci_get_dev_by_id(const struct pci_device_id *id,	/* NVMe: 매칭할 PCI ID 패턴. */
					 struct pci_dev *from)	/* NVMe: 이전 검색 위치, NULL이면 처음부터. */
{	/* NVMe: pci_get_dev_by_id 함수 본문 시작. */
	struct device *dev; /* NVMe: bus_find_device가 반환한 generic device. */
	struct device *dev_start = NULL; /* NVMe: 검색 시작점(NULL이면 처음부터). */
	struct pci_dev *pdev = NULL; /* NVMe: 변환된 pci_dev 결과. */

	/* NVMe: 이전 검색 결과가 있으면 그 다음 device부터 탐색. */
	if (from)
		dev_start = &from->dev;
	/* NVMe: pci_bus_type 전체에서 match_pci_dev_by_id 콜백으로 검색. */
	dev = bus_find_device(&pci_bus_type, dev_start, (void *)id,
			      match_pci_dev_by_id);
	/* NVMe: device를 찾으면 pci_dev로 변환. */
	if (dev)
		pdev = to_pci_dev(dev);
	/* NVMe: 검색의 기준점이던 from 장치의 참조 카운트 감소. */
	pci_dev_put(from);
	return pdev; /* NVMe: 매칭된 NVMe 관련 pci_dev 또는 NULL. */
}	/* NVMe: pci_get_dev_by_id 함수 종료. */

/*
 * NVMe: pci_get_dev_by_id의 역순 검색 버전. NVMe hotplug/remove 시나리오에서
 *       발견 순서가 중요할 때 사용될 수 있음.
 */
static struct pci_dev *pci_get_dev_by_id_reverse(const struct pci_device_id *id,	/* NVMe: 매칭할 PCI ID 패턴. */
						 struct pci_dev *from)	/* NVMe: 이전 검색 위치, NULL이면 끝부터. */
{	/* NVMe: pci_get_dev_by_id_reverse 함수 본문 시작. */
	struct device *dev; /* NVMe: bus_find_device_reverse의 반환값. */
	struct device *dev_start = NULL; /* NVMe: 역순 검색 시작점. */
	struct pci_dev *pdev = NULL; /* NVMe: 최종 pci_dev 결과. */

	/* NVMe: 이전 장치가 있으면 그 이전부터 역순 탐색. */
	if (from)
		dev_start = &from->dev;
	/* NVMe: pci_bus_type 장치 리스트를 뒤에서부터 검색. */
	dev = bus_find_device_reverse(&pci_bus_type, dev_start, (void *)id,
				      match_pci_dev_by_id);
	/* NVMe: 발견 시 pci_dev로 변환. */
	if (dev)
		pdev = to_pci_dev(dev);
	/* NVMe: from의 참조 해제. */
	pci_dev_put(from);
	return pdev; /* NVMe: 역순으로 매칭된 pci_dev 또는 NULL. */
}	/* NVMe: pci_get_dev_by_id_reverse 함수 종료. */

/* NVMe: pci_get_dev_by_id의 검색 방향(순방향/역방향)을 지정하는 열거형. */
enum pci_search_direction {
	PCI_SEARCH_FORWARD,	/* NVMe: 전방향(리스트 head에서 tail로) 검색. */
	PCI_SEARCH_REVERSE,	/* NVMe: 역방향(리스트 tail에서 head로) 검색. */
};

/*
 * NVMe: vendor/device/subvendor/subdevice 기준으로 NVMe 컨트롤러를 검색하는
 *       실제 구현. direction에 따라 순방향/역방향 검색을 선택.
 */
static struct pci_dev *__pci_get_subsys(unsigned int vendor, unsigned int device,	/* NVMe: 원하는 PCI vendor/device ID. */
					unsigned int ss_vendor, unsigned int ss_device,	/* NVMe: 원하는 서브시스템 vendor/device ID. */
					struct pci_dev *from, enum pci_search_direction dir)	/* NVMe: 검색 시작점과 방향. */
{	/* NVMe: __pci_get_subsys 함수 본문 시작. */
	struct pci_device_id id = {
		.vendor = vendor,	/* NVMe: 원하는 PCI vendor ID. */
		.device = device,	/* NVMe: 원하는 PCI device ID. */
		.subvendor = ss_vendor,	/* NVMe: 서브시스템 vendor ID. */
		.subdevice = ss_device,	/* NVMe: 서브시스템 device ID. */
	};	/* NVMe: 비교용 pci_device_id 구조체 초기화 종료. */

	/* NVMe: 방향에 맞는 검색 함수 호출. */
	if (dir == PCI_SEARCH_FORWARD)
		return pci_get_dev_by_id(&id, from);
	else
		/* NVMe: 역방향 검색 수행. */
		return pci_get_dev_by_id_reverse(&id, from);
}	/* NVMe: __pci_get_subsys 함수 종료. */

/**
 * pci_get_subsys - begin or continue searching for a PCI device by vendor/subvendor/device/subdevice id
 * @vendor: PCI vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @device: PCI device id to match, or %PCI_ANY_ID to match all device ids
 * @ss_vendor: PCI subsystem vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @ss_device: PCI subsystem device id to match, or %PCI_ANY_ID to match all device ids
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is found
 * with a matching @vendor, @device, @ss_vendor and @ss_device, a pointer to its
 * device structure is returned, and the reference count to the device is
 * incremented.  Otherwise, %NULL is returned.  A new search is initiated by
 * passing %NULL as the @from argument.  Otherwise if @from is not %NULL,
 * searches continue from next device on the global list.
 * The reference count for @from is always decremented if it is not %NULL.
 */
/*
 * NVMe: VID/DID/SSID 기준으로 특정 NVMe SSD 모델을 검색. 예: 특정 벤더의
 *       NVMe 컨트롤러를 quirk 적용 대상으로 찾을 때 사용.
 */
struct pci_dev *pci_get_subsys(unsigned int vendor, unsigned int device,	/* NVMe: vendor/device ID. */
			       unsigned int ss_vendor, unsigned int ss_device,	/* NVMe: 서브시스템 vendor/device ID. */
			       struct pci_dev *from)	/* NVMe: 이전 검색 위치. */
{	/* NVMe: pci_get_subsys 함수 본문 시작. */
	return __pci_get_subsys(vendor, device, ss_vendor, ss_device, from,
				PCI_SEARCH_FORWARD); /* NVMe: 순방향 검색. */
}	/* NVMe: pci_get_subsys 함수 종료. */
EXPORT_SYMBOL(pci_get_subsys);	/* NVMe: NVMe 등에서 pci_get_subsys 심볼 사용 가능. */

/**
 * pci_get_device - begin or continue searching for a PCI device by vendor/device id
 * @vendor: PCI vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @device: PCI device id to match, or %PCI_ANY_ID to match all device ids
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @vendor and @device, the reference count to the
 * device is incremented and a pointer to its device structure is returned.
 * Otherwise, %NULL is returned.  A new search is initiated by passing %NULL
 * as the @from argument.  Otherwise if @from is not %NULL, searches continue
 * from next device on the global list.  The reference count for @from is
 * always decremented if it is not %NULL.
 */
/*
 * NVMe: vendor/device ID만으로 NVMe 컨트롤러를 검색. drivers/nvme/host/pci.c의
 *       nvme_id_table에 정의된 VID/DID를 기반으로 매칭 가능.
 */
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,	/* NVMe: 매칭할 vendor/device ID. */
			       struct pci_dev *from)	/* NVMe: 이전 검색 위치. */
{	/* NVMe: pci_get_device 함수 본문 시작. */
	/* NVMe: subvendor/subdevice는 ANY_ID로 두고 __pci_get_subsys 호출. */
	return pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}	/* NVMe: pci_get_device 함수 종료. */
EXPORT_SYMBOL(pci_get_device);	/* NVMe: NVMe 드라이버 등에서 pci_get_device 심볼 사용 가능. */

/*
 * Same semantics as pci_get_device(), except walks the PCI device list
 * in reverse discovery order.
 */
/*
 * NVMe: pci_get_device의 역순 버전. NVMe remove 경로나 후발견 장치를 먼저
 *       다뤄야 할 때 활용.
 */
struct pci_dev *pci_get_device_reverse(unsigned int vendor,	/* NVMe: 매칭할 vendor ID. */
				       unsigned int device,	/* NVMe: 매칭할 device ID. */
				       struct pci_dev *from)	/* NVMe: 이전 검색 위치. */
{	/* NVMe: pci_get_device_reverse 함수 본문 시작. */
	return __pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from,
				PCI_SEARCH_REVERSE); /* NVMe: 역방향 검색. */
}	/* NVMe: pci_get_device_reverse 함수 종료. */
EXPORT_SYMBOL(pci_get_device_reverse);	/* NVMe: NVMe 등에서 역순 검색 심볼 사용 가능. */

/**
 * pci_get_class - begin or continue searching for a PCI device by class
 * @class: search for a PCI device with this class designation
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @class, the reference count to the device is
 * incremented and a pointer to its device structure is returned.
 * Otherwise, %NULL is returned.
 * A new search is initiated by passing %NULL as the @from argument.
 * Otherwise if @from is not %NULL, searches continue from next device
 * on the global list.  The reference count for @from is always decremented
 * if it is not %NULL.
 */
/*
 * NVMe: class code 기반으로 NVMe 컨트롤러(0x010802)를 검색. PCI core가 NVMe
 *       장치를 찾아 nvme_probe()를 호출하는 과정에서 활용될 수 있다.
 */
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from)	/* NVMe: NVMe class code(0x010802)와 이전 검색 위치. */
{	/* NVMe: pci_get_class 함수 본문 시작. */
	struct pci_device_id id = {
		.vendor = PCI_ANY_ID,	/* NVMe: vendor 제한 없음. */
		.device = PCI_ANY_ID,	/* NVMe: device 제한 없음. */
		.subvendor = PCI_ANY_ID,	/* NVMe: subvendor 제한 없음. */
		.subdevice = PCI_ANY_ID,	/* NVMe: subdevice 제한 없음. */
		.class_mask = PCI_ANY_ID,	/* NVMe: class code 전체 비트 비교. */
		.class = class,	/* NVMe: NVMe class code(0x010802) 등. */
	};

	return pci_get_dev_by_id(&id, from); /* NVMe: class code 기반 검색 수행. */
}	/* NVMe: pci_get_class 함수 종료. */
EXPORT_SYMBOL(pci_get_class);	/* NVMe: NVMe 등에서 class 기반 검색 심볼 사용 가능. */

/**
 * pci_get_base_class - searching for a PCI device by matching against the base class code only
 * @class: search for a PCI device with this base class code
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices. If a PCI device is found
 * with a matching base class code, the reference count to the device is
 * incremented. See pci_match_one_device() to figure out how does this works.
 * A new search is initiated by passing %NULL as the @from argument.
 * Otherwise if @from is not %NULL, searches continue from next device on the
 * global list. The reference count for @from is always decremented if it is
 * not %NULL.
 *
 * Returns:
 * A pointer to a matched PCI device, %NULL Otherwise.
 */
/*
 * NVMe: base class만 매칭. 예: mass storage base class(0x01) 전체를 검색해
 *       NVMe(0x010802)를 포함한 저장 장치를 찾을 때 사용.
 */
struct pci_dev *pci_get_base_class(unsigned int class, struct pci_dev *from)	/* NVMe: base class 값과 이전 검색 위치. */
{	/* NVMe: pci_get_base_class 함수 본문 시작. */
	struct pci_device_id id = {
		.vendor = PCI_ANY_ID,	/* NVMe: vendor 무관. */
		.device = PCI_ANY_ID,	/* NVMe: device 무관. */
		.subvendor = PCI_ANY_ID,	/* NVMe: subvendor 무관. */
		.subdevice = PCI_ANY_ID,	/* NVMe: subdevice 무관. */
		.class_mask = 0xFF0000,	/* NVMe: 상위 8비트(base class)만 비교. */
		.class = class << 16,	/* NVMe: base class를 24비트 class 코드 상위에 배치. */
	};

	return pci_get_dev_by_id(&id, from); /* NVMe: base class 기반 검색 수행. */
}	/* NVMe: pci_get_base_class 함수 종료. */
EXPORT_SYMBOL(pci_get_base_class);	/* NVMe: NVMe 등에서 base class 검색 심볼 사용 가능. */

/**
 * pci_dev_present - Returns 1 if device matching the device list is present, 0 if not.
 * @ids: A pointer to a null terminated list of struct pci_device_id structures
 * that describe the type of PCI device the caller is trying to find.
 *
 * Obvious fact: You do not have a reference to any device that might be found
 * by this function, so if that device is removed from the system right after
 * this function is finished, the value will be stale.  Use this function to
 * find devices that are usually built into a system, or for a general hint as
 * to if another device happens to be present at this specific moment in time.
 */
/*
 * NVMe: NVMe ID 테이블(nvme_id_table)에 등록된 VID/DID/Class 조합 중 하나라도
 *       현재 시스템에 존재하는지 확인. 단, 반환 직후 장치가 제거될 수 있으므로
 *       probe용 참조 카운트 확보는 별도로 해야 한다.
 */
int pci_dev_present(const struct pci_device_id *ids)	/* NVMe: NVMe ID 테이블 포인터. */
{	/* NVMe: pci_dev_present 함수 본문 시작. */
	struct pci_dev *found = NULL; /* NVMe: 매칭된 장치 임시 포인터. */

	/* NVMe: IDs 리스트의 끝(모두 0)까지 순회. */
	while (ids->vendor || ids->subvendor || ids->class_mask) {
		/* NVMe: 현재 ID 항목과 일치하는 PCI 장치 검색. */
		found = pci_get_dev_by_id(ids, NULL);
		/* NVMe: 하나라도 발견하면 참조 해제 후 1 반환. */
		if (found) {
			pci_dev_put(found);
			return 1;
		}	/* NVMe: 발견 시 처리 블록 종료. */
		ids++; /* NVMe: 다음 ID 항목으로 이동. */
	}	/* NVMe: ID 테이블 순회 종료. */

	return 0; /* NVMe: NVMe ID 테이블의 어떤 항목과도 일치하는 장치가 없음. */
}	/* NVMe: pci_dev_present 함수 종료. */
EXPORT_SYMBOL(pci_dev_present);	/* NVMe: NVMe 등에서 pci_dev_present 심볼 사용 가능. */
