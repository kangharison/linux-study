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
 * [한국어 설명] 버스 트리에서 장치와 버스를 찾아 주는 조회 함수 모음 (search.c)
 *
 * === 파일의 역할 ===
 * "조건에 맞는 PCI 장치를 찾아 달라" 는 요청을 처리한다. 조건은 도메인:버스:슬롯,
 * Vendor/Device ID, Class Code 등 여러 가지다. 여기에 더해, 이 파일의 절반쯤
 * 특이한 함수 하나가 있다 — pci_for_each_dma_alias().
 *
 * 조회 함수들의 공통 규약 두 가지를 먼저 알아야 한다.
 *   1) pci_get_* 계열은 찾은 장치의 참조 카운트를 올려서 돌려준다. 다 쓴 뒤
 *      pci_dev_put() 을 부르지 않으면 그 장치는 영원히 해제되지 않는다.
 *      반면 pci_find_* 계열은 참조를 올리지 않는다 — 그래서 이름이 다르다.
 *   2) from 인자를 받는 함수들은 "이전에 찾은 것 다음부터" 를 뜻한다. 같은
 *      조건의 장치가 여러 개일 때 반복 호출로 전부 훑는 관용구다. from 에
 *      넘긴 장치의 참조는 함수가 대신 내려 준다.
 *
 * DMA alias 는 별도로 설명이 필요하다. PCIe 에서 DMA 트랜잭션에는 발신자를
 * 나타내는 requester ID(RID)가 붙고, IOMMU 는 그 ID 로 어느 주소 공간을
 * 쓸지 정한다. 문제는 이 ID 가 도중에 바뀔 수 있다는 것이다.
 *   - PCIe-to-PCI 브리지 뒤의 장치는 브리지의 ID 로 바뀐다(구형 PCI 에는
 *     RID 라는 개념 자체가 없어 브리지가 대신 자기 ID 를 붙인다).
 *   - 일부 장치는 펌웨어 버그로 엉뚱한 ID 를 쓴다(quirk 로 등록해 둔다).
 * pci_for_each_dma_alias() 는 한 장치가 낼 수 있는 모든 RID 를 훑어 콜백을
 * 부른다. IOMMU 는 그 전부에 대해 같은 매핑을 걸어야 DMA 가 통한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 자료구조를 읽기만 하는 최하위 유틸리티다. 만들지도, 바꾸지도 않는다.
 *
 *   드라이버/quirk/IOMMU/hotplug
 *     -> [이 파일] pci_get_device(), pci_get_domain_bus_and_slot(),
 *                  pci_for_each_dma_alias()
 *        -> 커널 드라이버 모델의 bus_find_device() 로 pci_bus_type 을 훑거나,
 *           pci_root_buses 목록을 직접 순회
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. bus_find_device() 가 클래스 뮤텍스를
 * 잡으므로 인터럽트 문맥에서 부를 수 없다. pci_for_each_dma_alias() 는
 * 자료구조 순회뿐이라 더 가볍지만, 여전히 pci_bus_sem 이 필요한 경우가 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: quirks.c(특정 장치를 찾아 우회 적용), IOMMU 드라이버(alias 순회),
 *   hotplug, 그리고 수많은 장치 드라이버(짝이 되는 다른 function 찾기).
 * 아래쪽: drivers/base/bus.c 의 bus_find_device, 그리고 pci_root_buses 목록.
 * 공유 상태: pci_bus_type 에 등록된 장치 목록, pci_root_buses(모든 루트 버스),
 *   struct pci_dev 의 dma_alias_mask(quirk 가 표시해 둔 추가 RID 비트맵).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 다만 두 지점에서 간접적으로 NVMe 와 얽힌다.
 *
 *   1) 클래스 기반 매칭의 재료. NVMe 의 id_table 마지막 항목은
 *      { PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) } 로,
 *      벤더를 가리지 않고 "NVM Express 클래스" 인 모든 장치를 잡는다.
 *      이 클래스 코드 개념을 다루는 조회 함수(pci_get_class 등)가 여기 있다.
 *      다만 매칭 자체는 pci-driver.c 의 pci_match_device 가 하고, 이 파일의
 *      함수를 쓰지는 않는다.
 *
 *   2) IOMMU 매핑. NVMe 는 PRP 리스트/SGL 로 호스트 메모리를 DMA 하고,
 *      HMB(Host Memory Buffer)를 쓰면 그 영역도 DMA 대상이다. 이 매핑을
 *      거는 IOMMU 코드가 pci_for_each_dma_alias() 로 NVMe 컨트롤러의 모든
 *      RID 를 훑는다. NVMe 가 PCIe 스위치 뒤에 있고 그 경로에
 *      PCIe-to-PCI 브리지가 끼어 있다면 alias 가 생기는데, 그것을 빠뜨리면
 *      DMA 가 IOMMU 에 막혀 I/O 가 전부 타임아웃난다.
 *
 * (기존 주석의 "class code 0x010802" 는 오기다. NVM Express 의 클래스 코드는
 *  Base Class 0x01(Mass Storage) / Sub-Class 0x08(Non-Volatile Memory) /
 *  Prog-IF 0x02(NVMHCI 가 아닌 NVM Express) 이므로 0x010802 가 아니라
 *  0x010802 를 24비트로 쓴 PCI_CLASS_STORAGE_EXPRESS = 0x010802 가 맞다 —
 *  값 자체는 맞으나 "NVM Express controller" 라는 설명 순서를 위와 같이
 *  풀어 두는 편이 이해에 낫다. 또 nvme_id_table 이 이 파일의 함수를 쓴다는
 *  서술은 사실이 아니어서 위와 같이 정정했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_find_bus()                 : 도메인 + 버스 번호로 struct pci_bus 를 찾는다.
 *                                  참조를 올리지 않는다.
 * pci_find_next_bus()            : 모든 루트 버스를 차례로 훑는 반복자.
 * pci_get_slot()                 : 버스 + devfn 으로 장치를 찾는다(참조 올림).
 * pci_get_domain_bus_and_slot()  : 도메인:버스:슬롯.함수 로 장치를 찾는다.
 *                                  "0000:01:00.0" 같은 주소를 코드로 옮긴 것.
 * pci_get_device()               : Vendor/Device ID 로 찾는다. from 으로 반복.
 * pci_get_subsys()               : 위에 Subsystem ID 조건을 더한다.
 * pci_get_class()                : 24비트 Class Code 전체로 찾는다.
 * pci_get_base_class()           : 상위 8비트(Base Class)만으로 찾는다.
 * pci_get_device_reverse()       : 역순 순회. 제거 경로에서 쓴다.
 * pci_dev_present()              : 주어진 id 표 중 하나라도 시스템에 있는지만
 *                                  확인한다. 참조를 남기지 않아 quirk 판정에 편하다.
 * pci_for_each_dma_alias()       : 이 장치가 낼 수 있는 모든 requester ID 에 대해
 *                                  콜백을 부른다. IOMMU 매핑의 핵심.
 */

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include "pci.h"

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
int pci_for_each_dma_alias(struct pci_dev *pdev,
			   int (*fn)(struct pci_dev *pdev,
				     u16 alias, void *data),
			   void *data)
{
	struct pci_bus *bus;
	int ret;

	/*
	 * The device may have an explicit alias requester ID for DMA where the
	 * requester is on another PCI bus.
	 */
	pdev = pci_real_dma_dev(pdev);

	ret = fn(pdev, pci_dev_id(pdev), data);
	if (ret)
		return ret;

	/*
	 * If the device is broken and uses an alias requester ID for
	 * DMA, iterate over that too.
	 */
	if (unlikely(pdev->dma_alias_mask)) {
		unsigned int devfn;

		for_each_set_bit(devfn, pdev->dma_alias_mask, MAX_NR_DEVFNS) {
			ret = fn(pdev, PCI_DEVID(pdev->bus->number, devfn),
				 data);
			if (ret)
				return ret;
		}
	}

	for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
		struct pci_dev *tmp;

		/* Skip virtual buses */
		if (!bus->self)
			continue;

		tmp = bus->self;

		/* stop at bridge where translation unit is associated */
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
		if (pci_is_pcie(tmp)) {
			switch (pci_pcie_type(tmp)) {
			case PCI_EXP_TYPE_ROOT_PORT:
			case PCI_EXP_TYPE_UPSTREAM:
			case PCI_EXP_TYPE_DOWNSTREAM:
				continue;
			case PCI_EXP_TYPE_PCI_BRIDGE:
				if (tmp->dev_flags & PCI_DEV_FLAGS_PCI_BRIDGE_NO_ALIAS)
					continue;
				ret = fn(tmp,
					 PCI_DEVID(tmp->subordinate->number,
						   PCI_DEVFN(0, 0)), data);
				if (ret)
					return ret;
				continue;
			case PCI_EXP_TYPE_PCIE_BRIDGE:
				ret = fn(tmp, pci_dev_id(tmp), data);
				if (ret)
					return ret;
				continue;
			}
		} else {
			if (tmp->dev_flags & PCI_DEV_FLAG_PCIE_BRIDGE_ALIAS)
				ret = fn(tmp,
					 PCI_DEVID(tmp->subordinate->number,
						   PCI_DEVFN(0, 0)), data);
			else
				ret = fn(tmp, pci_dev_id(tmp), data);
			if (ret)
				return ret;
		}
	}

	return ret;
}

/*
 * NVMe: 재귀적으로 bus number에 해당하는 pci_bus를 찾는 낮은 수준 함수.
 *       NVMe 장치가 연결된 하위 bus를 root bus에서부터 탐색할 때 사용.
 */
static struct pci_bus *pci_do_find_bus(struct pci_bus *bus,
				       unsigned char busnr)
{
	struct pci_bus *child;
	struct pci_bus *tmp;

	if (bus->number == busnr)
		return bus;

	list_for_each_entry(tmp, &bus->children, node) {
		child = pci_do_find_bus(tmp, busnr);
		if (child)
			return child;
	}
	return NULL;
}

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
struct pci_bus *pci_find_bus(int domain,
			     int busnr)
{
	struct pci_bus *bus = NULL;
	struct pci_bus *tmp_bus;

	while ((bus = pci_find_next_bus(bus)) != NULL)  {
		if (pci_domain_nr(bus) != domain)
			continue;
		tmp_bus = pci_do_find_bus(bus, busnr);
		if (tmp_bus)
			return tmp_bus;
	}
	return NULL;
}
EXPORT_SYMBOL(pci_find_bus);

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
struct pci_bus *pci_find_next_bus(const struct pci_bus *from)
{
	struct list_head *n;
	struct pci_bus *b = NULL;

	down_read(&pci_bus_sem);
	n = from ? from->node.next : pci_root_buses.next;
	if (n != &pci_root_buses)
		b = list_entry(n, struct pci_bus, node);
	up_read(&pci_bus_sem);
	return b;
}
EXPORT_SYMBOL(pci_find_next_bus);

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
struct pci_dev *pci_get_slot(struct pci_bus *bus,
			     unsigned int devfn)
{
	struct pci_dev *dev;

	down_read(&pci_bus_sem);

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (dev->devfn == devfn)
			goto out;
	}

	dev = NULL;
 out:
	pci_dev_get(dev);
	up_read(&pci_bus_sem);
	return dev;
}
EXPORT_SYMBOL(pci_get_slot);

/**
 * pci_get_domain_bus_and_slot - locate PCI device for a given PCI domain (segment), bus, and slot
 * @domain: PCI domain/segment on which the PCI device resides.
 * @bus: PCI bus on which desired PCI device resides
 * @devfn: encodes number of PCI slot in which the desired PCI device
 * resides and the logical device number within that slot in case of
 * multi-function devices.
 *
 * Given a PCI domain, bus, and slot/function number, the desired PCI
 * device is located in the list of PCI devices. If the device is
 * found, its reference count is increased and this function returns a
 * pointer to its data structure.  The caller must decrement the
 * reference count by calling pci_dev_put().  If no device is found,
 * %NULL is returned.
 */
/*
 * NVMe: domain+bus+devfn으로 NVMe 컨트롤러나 Root Port를 직접 찾는다.
 *       예: SR-IOV PF/VF, 특정 segment의 NVMe endpoint를 식별할 때.
 */
struct pci_dev *pci_get_domain_bus_and_slot(int domain,
				    unsigned int bus,
				    unsigned int devfn)
{
	struct pci_dev *dev = NULL;

	for_each_pci_dev(dev) {
		if (pci_domain_nr(dev->bus) == domain &&
		    (dev->bus->number == bus && dev->devfn == devfn))
			return dev;
	}
	return NULL;
}
EXPORT_SYMBOL(pci_get_domain_bus_and_slot);

/*
 * NVMe: pci_device_id 하나와 pci_dev가 매칭되는지 검사하는 helper.
 *       NVMe 드라이버의 nvme_id_table 매칭 로직과 동일한 pci_match_one_device 사용.
 */
static int match_pci_dev_by_id(struct device *dev,
			       const void *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	const struct pci_device_id *id = data;

	if (pci_match_one_device(id, pdev))
		return 1;
	return 0;
}

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
static struct pci_dev *pci_get_dev_by_id(const struct pci_device_id *id,
					 struct pci_dev *from)
{
	struct device *dev;
	struct device *dev_start = NULL;
	struct pci_dev *pdev = NULL;

	if (from)
		dev_start = &from->dev;
	dev = bus_find_device(&pci_bus_type, dev_start, (void *)id,
			      match_pci_dev_by_id);
	if (dev)
		pdev = to_pci_dev(dev);
	pci_dev_put(from);
	return pdev;
}

/*
 * NVMe: pci_get_dev_by_id의 역순 검색 버전. NVMe hotplug/remove 시나리오에서
 *       발견 순서가 중요할 때 사용될 수 있음.
 */
static struct pci_dev *pci_get_dev_by_id_reverse(const struct pci_device_id *id,
						 struct pci_dev *from)
{
	struct device *dev;
	struct device *dev_start = NULL;
	struct pci_dev *pdev = NULL;

	if (from)
		dev_start = &from->dev;
	dev = bus_find_device_reverse(&pci_bus_type, dev_start, (void *)id,
				      match_pci_dev_by_id);
	if (dev)
		pdev = to_pci_dev(dev);
	pci_dev_put(from);
	return pdev;
}

enum pci_search_direction {
	PCI_SEARCH_FORWARD,
	PCI_SEARCH_REVERSE,
};

/*
 * NVMe: vendor/device/subvendor/subdevice 기준으로 NVMe 컨트롤러를 검색하는
 *       실제 구현. direction에 따라 순방향/역방향 검색을 선택.
 */
static struct pci_dev *__pci_get_subsys(unsigned int vendor,
					unsigned int device,
					unsigned int ss_vendor,
					unsigned int ss_device,
					struct pci_dev *from,
					enum pci_search_direction dir)
{
	struct pci_device_id id = {
		.vendor = vendor,
		.device = device,
		.subvendor = ss_vendor,
		.subdevice = ss_device,
	};

	if (dir == PCI_SEARCH_FORWARD)
		return pci_get_dev_by_id(&id, from);
	else
		return pci_get_dev_by_id_reverse(&id, from);
}

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
struct pci_dev *pci_get_subsys(unsigned int vendor,
			       unsigned int device,
			       unsigned int ss_vendor,
			       unsigned int ss_device,
			       struct pci_dev *from)
{
	return __pci_get_subsys(vendor, device, ss_vendor, ss_device, from,
				PCI_SEARCH_FORWARD);
}
EXPORT_SYMBOL(pci_get_subsys);

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
struct pci_dev *pci_get_device(unsigned int vendor,
			       unsigned int device,
			       struct pci_dev *from)
{
	return pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}
EXPORT_SYMBOL(pci_get_device);

/*
 * Same semantics as pci_get_device(), except walks the PCI device list
 * in reverse discovery order.
 */
/*
 * NVMe: pci_get_device의 역순 버전. NVMe remove 경로나 후발견 장치를 먼저
 *       다뤄야 할 때 활용.
 */
struct pci_dev *pci_get_device_reverse(unsigned int vendor,
				       unsigned int device,
				       struct pci_dev *from)
{
	return __pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from,
				PCI_SEARCH_REVERSE);
}
EXPORT_SYMBOL(pci_get_device_reverse);

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
struct pci_dev *pci_get_class(unsigned int class,
			      struct pci_dev *from)
{
	struct pci_device_id id = {
		.vendor = PCI_ANY_ID,
		.device = PCI_ANY_ID,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
		.class_mask = PCI_ANY_ID,
		.class = class,
	};

	return pci_get_dev_by_id(&id, from);
}
EXPORT_SYMBOL(pci_get_class);

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
struct pci_dev *pci_get_base_class(unsigned int class,
				   struct pci_dev *from)
{
	struct pci_device_id id = {
		.vendor = PCI_ANY_ID,
		.device = PCI_ANY_ID,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
		.class_mask = 0xFF0000,
		.class = class << 16,
	};

	return pci_get_dev_by_id(&id, from);
}
EXPORT_SYMBOL(pci_get_base_class);

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
int pci_dev_present(const struct pci_device_id *ids)
{
	struct pci_dev *found = NULL;

	while (ids->vendor || ids->subvendor || ids->class_mask) {
		found = pci_get_dev_by_id(ids, NULL);
		if (found) {
			pci_dev_put(found);
			return 1;
		}
		ids++;
	}

	return 0;
}
EXPORT_SYMBOL(pci_dev_present);
