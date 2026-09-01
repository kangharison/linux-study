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
int pci_for_each_dma_alias(struct pci_dev *pdev,
			   int (*fn)(struct pci_dev *pdev,
				     u16 alias, void *data), void *data)
{
	/* [한국어] 버스를 거슬러 올라갈 커서. */
	struct pci_bus *bus;
	/* [한국어] 콜백의 반환값. 0 이 아니면 순회를 멈춘다. */
	int ret;

	/*
	 * The device may have an explicit alias requester ID for DMA where the
	 * requester is on another PCI bus.
	 */
	pdev = pci_real_dma_dev(pdev);

	/* [한국어] 먼저 장치 자신의 requester ID 로 한 번 부른다. 별칭이 하나도 없는
	 * 정상적인 장치에서는 이 호출이 전부다. */
	ret = fn(pdev, pci_dev_id(pdev), data);
	/* [한국어] 콜백이 멈추라고 하면, */
	if (ret)
		return ret;

	/*
	 * If the device is broken and uses an alias requester ID for
	 * DMA, iterate over that too.
	 */
	if (unlikely(pdev->dma_alias_mask)) {
		/* [한국어] 별칭으로 쓰이는 devfn 을 담을 변수. */
		unsigned int devfn;

		/* [한국어] 위 영어 주석대로 고장 난 장치가 자기 것이 아닌 requester ID 로 DMA 를
		 * 내는 경우가 있다. 그런 장치는 쿼크가 이 비트맵을 채워 두며, 여기서
		 * 세워진 비트마다 그 devfn 을 별칭으로 함께 보고한다. */
		for_each_set_bit(devfn, pdev->dma_alias_mask, MAX_NR_DEVFNS) {
			/* [한국어] 버스 번호는 그대로 두고 devfn 만 바꾼 ID 로 부른다. */
			ret = fn(pdev, PCI_DEVID(pdev->bus->number, devfn),
				 data);
			/* [한국어] 콜백이 멈추라고 하면, */
			if (ret)
				return ret;
		}
	}

	/* [한국어] 루트 버스에 닿을 때까지 브리지를 거슬러 올라간다. 브리지마다 트랜잭션의
	 * requester ID 가 바뀔 수 있어, 상류에서 볼 수 있는 모든 ID 를 모아야 한다. */
	for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
		/* [한국어] 이 버스를 만든 브리지 장치. */
		struct pci_dev *tmp;

		/* Skip virtual buses */
		if (!bus->self)
			continue;

		/* [한국어] 브리지 장치를 꺼낸다. 옆의 영어 주석대로 가상 버스(브리지 없는 버스)는
		 * 위에서 이미 건너뛰었다. */
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
			/* [한국어] PCIe 브리지는 종류마다 별칭 규칙이 다르다. */
			switch (pci_pcie_type(tmp)) {
			/* [한국어] 루트 포트·업스트림·다운스트림 포트는 requester ID 를 바꾸지 않으므로
			 * 별칭이 생기지 않는다. 그냥 다음 단계로 올라간다. */
			case PCI_EXP_TYPE_ROOT_PORT:
			case PCI_EXP_TYPE_UPSTREAM:
			case PCI_EXP_TYPE_DOWNSTREAM:
				continue;
			/* [한국어] PCIe-to-PCI 브리지는 별칭을 만든다. 다만 쿼크로 예외 표시가 붙은
			 * 장치는 건너뛴다. */
			case PCI_EXP_TYPE_PCI_BRIDGE:
				if (tmp->dev_flags & PCI_DEV_FLAGS_PCI_BRIDGE_NO_ALIAS)
					continue;
				/* [한국어] 위 영어 주석의 근거(PCIe to PCI/PCI-X Bridge 규격 1.0, 2.3절)대로
				 * 세컨더리 버스의 장치 0, 기능 0 을 requester 로 쓴다. */
				ret = fn(tmp,
					 PCI_DEVID(tmp->subordinate->number,
						   PCI_DEVFN(0, 0)), data);
				/* [한국어] 콜백이 멈추라고 하면, */
				if (ret)
					return ret;
				continue;
			/* [한국어] PCI-to-PCIe 브리지는 자기 자신의 ID 로 별칭을 만든다. */
			case PCI_EXP_TYPE_PCIE_BRIDGE:
				ret = fn(tmp, pci_dev_id(tmp), data);
				/* [한국어] 콜백이 멈추라고 하면, */
				if (ret)
					return ret;
				continue;
			}
		} else {
			/* [한국어] PCIe 가 아닌(전통적인 PCI) 브리지. 쿼크가 "PCIe 브리지처럼 별칭을
			 * 만든다" 고 표시해 두었으면, */
			if (tmp->dev_flags & PCI_DEV_FLAG_PCIE_BRIDGE_ALIAS)
				/* [한국어] 세컨더리 버스의 0,0 을 쓰고, */
				ret = fn(tmp,
					 PCI_DEVID(tmp->subordinate->number,
						   PCI_DEVFN(0, 0)), data);
			else
				/* [한국어] 아니면 자기 ID 를 쓴다. */
				ret = fn(tmp, pci_dev_id(tmp), data);
			/* [한국어] 콜백이 멈추라고 하면, */
			if (ret)
				return ret;
		}
	}

	return ret;
}

/* [한국어]
 * pci_do_find_bus - 이 버스와 그 아래에서 번호가 맞는 버스를 재귀로 찾는다
 *
 * @bus: 탐색을 시작할 버스.
 * @busnr: 찾는 버스 번호.
 * @return: 찾은 버스, 없으면 NULL.
 *
 * 깊이 우선 재귀 탐색이다. 자기 번호를 먼저 확인하고, 아니면 자식 버스들을
 * 차례로 내려가며 같은 일을 반복한다.
 *
 * 참조를 올리지 않는다는 점이 이 함수의 성격이다. 버스 구조체는 장치와 달리
 * 호출자가 pci_bus_sem 등으로 보호받는 문맥에서 쓰는 것이 전제이며,
 * 그래서 이름에 do 가 붙은 내부 함수로 남아 있다.
 *
 * 실행 컨텍스트: 버스 조회 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 못 찾으면 NULL 이다.
 *
 * 호출 체인:
 *   pci_find_bus() → [이 함수](재귀)
 */
static struct pci_bus *pci_do_find_bus(struct pci_bus *bus, unsigned char busnr)
{
	/* [한국어] 재귀로 찾은 결과. */
	struct pci_bus *child;
	/* [한국어] 자식 버스 순회 커서. */
	struct pci_bus *tmp;

	/* [한국어] 이 버스가 찾던 번호면, */
	if (bus->number == busnr)
		/* [한국어] 바로 돌려준다. */
		return bus;

	/* [한국어] 아니면 자식 버스들을 하나씩, */
	list_for_each_entry(tmp, &bus->children, node) {
		/* [한국어] 재귀로 찾는다. */
		child = pci_do_find_bus(tmp, busnr);
		/* [한국어] 찾았으면, */
		if (child)
			/* [한국어] 그대로 올려보낸다. */
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
struct pci_bus *pci_find_bus(int domain, int busnr)
{
	/* [한국어] 루트 버스 순회 커서. NULL 로 시작해 처음부터 훑는다. */
	struct pci_bus *bus = NULL;
	/* [한국어] 각 루트 아래에서 찾은 결과. */
	struct pci_bus *tmp_bus;

	/* [한국어] 루트 버스를 하나씩 꺼낸다. */
	while ((bus = pci_find_next_bus(bus)) != NULL)  {
		/* [한국어] 도메인이 다르면 볼 필요가 없다. 같은 버스 번호가 도메인마다 따로
		 * 존재할 수 있으므로 반드시 함께 비교해야 한다. */
		if (pci_domain_nr(bus) != domain)
			continue;
		/* [한국어] 그 루트 아래를 재귀로 훑는다. */
		tmp_bus = pci_do_find_bus(bus, busnr);
		/* [한국어] 찾았으면, */
		if (tmp_bus)
			/* [한국어] 돌려준다. */
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
struct pci_bus *pci_find_next_bus(const struct pci_bus *from)
{
	/* [한국어] 리스트 노드 커서. */
	struct list_head *n;
	/* [한국어] 결과. 못 찾으면 NULL 로 남는다. */
	struct pci_bus *b = NULL;

	down_read(&pci_bus_sem);
	/* [한국어] from 이 있으면 그 다음부터, 없으면 리스트 처음부터. 이 한 줄이 "이어서
	 * 찾기" 규약을 만든다 — 호출자가 이전 결과를 넘기면 그 다음을 준다. */
	n = from ? from->node.next : pci_root_buses.next;
	/* [한국어] 리스트 머리로 돌아온 것이 아니면 아직 항목이 남아 있다. */
	if (n != &pci_root_buses)
		/* [한국어] 리스트 노드에서 바깥 구조체를 되찾는다. */
		b = list_entry(n, struct pci_bus, node);
	up_read(&pci_bus_sem);
	/* [한국어] 찾았으면 그 버스를, 끝까지 갔으면 NULL 을 돌려준다. */
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
struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn)
{
	/* [한국어] 순회 커서이자 결과. */
	struct pci_dev *dev;

	down_read(&pci_bus_sem);

	/* [한국어] 이 버스에 매달린 장치들을 훑는다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* [한국어] devfn 이 맞으면, */
		if (dev->devfn == devfn)
			goto out;
	}

	/* [한국어] 끝까지 못 찾았으면 NULL 로 만든다. 루프가 정상 종료하면 dev 가 리스트
	 * 머리를 가리키는 엉뚱한 값이 되므로, 반드시 여기서 지워야 한다. */
	dev = NULL;
 out:
	pci_dev_get(dev);
	up_read(&pci_bus_sem);
	/* [한국어] 찾은 장치(참조가 올라간 상태) 또는 NULL. */
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
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
					    unsigned int devfn)
{
	/* [한국어] 순회 커서. NULL 로 시작한다. */
	struct pci_dev *dev = NULL;

	/* [한국어] 시스템의 모든 PCI 장치를 훑는다. 이 매크로가 반복마다 참조를 올리고
	 * 내려 준다. */
	for_each_pci_dev(dev) {
		/* [한국어] 도메인과 버스 번호, devfn 이 모두 맞으면, */
		if (pci_domain_nr(dev->bus) == domain &&
		    (dev->bus->number == bus && dev->devfn == devfn))
			/* [한국어] 그 장치를 돌려준다. 이때 매크로가 올려 둔 참조가 그대로 호출자에게
			 * 넘어가므로, 호출자가 pci_dev_put() 해야 한다. */
			return dev;
	}
	return NULL;
}
EXPORT_SYMBOL(pci_get_domain_bus_and_slot);

/* [한국어]
 * match_pci_dev_by_id - 드라이버 코어의 버스 순회에 넘길 일치 판정 콜백
 *
 * @dev: 순회 중 만난 device.
 * @data: 찾을 조건(struct pci_device_id).
 * @return: 1 = 맞음(순회 중단), 0 = 아님(계속).
 *
 * bus_find_device() 와 bus_find_device_reverse() 가 쓰는 콜백이다.
 * 0 이 아닌 값을 돌려주면 순회가 그 자리에서 멈추고 그 device 가 반환된다.
 *
 * 판정 자체는 pci_match_one_device() 에 맡긴다. 벤더·장치·서브시스템 벤더·
 * 서브시스템 장치·클래스를 PCI_ANY_ID 와 클래스 마스크까지 고려해 비교하는
 * 일이라, 이 파일이 직접 하지 않는다.
 *
 * 실행 컨텍스트: 드라이버 코어의 버스 순회. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_get_dev_by_id() / pci_get_dev_by_id_reverse()
 *     → bus_find_device[_reverse]() → [이 함수] → pci_match_one_device()
 */
static int match_pci_dev_by_id(struct device *dev, const void *data)
{
	/* [한국어] 드라이버 코어가 넘긴 device 를 pci_dev 로 되돌린다. */
	struct pci_dev *pdev = to_pci_dev(dev);
	/* [한국어] 찾을 조건. */
	const struct pci_device_id *id = data;

	/* [한국어] 벤더·장치·서브시스템·클래스가 모두 맞으면, */
	if (pci_match_one_device(id, pdev))
		/* [한국어] 1 을 돌려 순회를 멈춘다. bus_find_device() 의 규약이다. */
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
static struct pci_dev *pci_get_dev_by_id(const struct pci_device_id *id,
					 struct pci_dev *from)
{
	/* [한국어] 찾은 device. */
	struct device *dev;
	/* [한국어] 이어서 찾기의 시작점. NULL 이면 처음부터. */
	struct device *dev_start = NULL;
	/* [한국어] 결과. */
	struct pci_dev *pdev = NULL;

	/* [한국어] 이전 결과가 주어졌으면, */
	if (from)
		/* [한국어] 그 device 를 시작점으로 삼는다. */
		dev_start = &from->dev;
	/* [한국어] 드라이버 코어의 버스 순회를 쓴다. 직접 리스트를 돌지 않는 이유는
	 * 코어가 잠금과 참조 관리를 대신해 주기 때문이다. */
	dev = bus_find_device(&pci_bus_type, dev_start, (void *)id,
			      match_pci_dev_by_id);
	/* [한국어] 찾았으면, */
	if (dev)
		/* [한국어] pci_dev 로 되돌린다. 코어가 올려 둔 참조가 그대로 호출자 몫이 된다. */
		pdev = to_pci_dev(dev);
	pci_dev_put(from);
	/* [한국어] 결과 또는 NULL. */
	return pdev;
}

/* [한국어]
 * pci_get_dev_by_id_reverse - 조건에 맞는 장치를 끝에서부터 찾는다
 *
 * @id: 찾을 조건.
 * @from: 이전 결과. NULL 이면 끝에서부터 새로 시작한다.
 * @return: 찾은 장치(참조가 올라간 상태), 없으면 NULL.
 *
 * pci_get_dev_by_id() 와 코드가 완전히 같고 bus_find_device_reverse() 를
 * 쓴다는 한 줄만 다르다.
 *
 * 역방향 탐색이 필요한 이유는 같은 조건에 맞는 장치가 여럿일 때 "마지막
 * 것" 을 원하는 호출자가 있기 때문이다. __pci_get_subsys() 가 방향 인자로
 * 두 함수 중 하나를 고른다.
 *
 * 반환된 장치의 참조는 호출자 몫이다. 드라이버 코어가 올려 준 것을 그대로
 * 넘겨받는 구조이므로, 다 쓴 뒤 pci_dev_put() 해야 한다.
 *
 * 실행 컨텍스트: 장치 조회 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 못 찾으면 NULL 이다.
 *
 * 호출 체인:
 *   __pci_get_subsys(PCI_SEARCH_REVERSE) → [이 함수]
 *     → bus_find_device_reverse() → match_pci_dev_by_id()
 */
static struct pci_dev *pci_get_dev_by_id_reverse(const struct pci_device_id *id,
						 struct pci_dev *from)
{
	/* [한국어] 찾은 device. */
	struct device *dev;
	/* [한국어] 시작점. */
	struct device *dev_start = NULL;
	/* [한국어] 결과. */
	struct pci_dev *pdev = NULL;

	/* [한국어] 이전 결과가 있으면, */
	if (from)
		/* [한국어] 그 자리부터. */
		dev_start = &from->dev;
	/* [한국어] 역방향 순회 판. 앞의 함수와 이 한 줄만 다르다 — 같은 조건을 끝에서부터
	 * 찾는다. */
	dev = bus_find_device_reverse(&pci_bus_type, dev_start, (void *)id,
				      match_pci_dev_by_id);
	/* [한국어] 찾았으면, */
	if (dev)
		/* [한국어] 되돌린다. */
		pdev = to_pci_dev(dev);
	pci_dev_put(from);
	/* [한국어] 결과 또는 NULL. */
	return pdev;
}

/* [한국어] 순회 방향을 나타내는 열거형. bool 대신 이름을 붙여 호출부에서
 * 정방향인지 역방향인지가 드러나게 한다. */
enum pci_search_direction {
	PCI_SEARCH_FORWARD,
	PCI_SEARCH_REVERSE,
};

/* [한국어]
 * __pci_get_subsys - 다섯 조건과 방향을 받아 장치를 찾는 공통 구현
 *
 * @vendor: 벤더 ID. PCI_ANY_ID 면 아무거나.
 * @device: 장치 ID.
 * @ss_vendor: 서브시스템 벤더 ID.
 * @ss_device: 서브시스템 장치 ID.
 * @from: 이전 결과. 이어서 찾을 때 넘긴다.
 * @dir: 정방향인지 역방향인지.
 * @return: 찾은 장치(참조가 올라간 상태), 없으면 NULL.
 *
 * 이 파일의 공개 조회 함수 셋(pci_get_subsys, pci_get_device,
 * pci_get_device_reverse)이 모두 여기로 모인다. 각자는 조건 몇 개를
 * PCI_ANY_ID 로 채우고 방향을 정해 넘기는 얇은 래퍼일 뿐이다.
 *
 * 조건 구조체를 스택에 지정 초기화로 만드는 것이 요령이다. 명시하지 않은
 * 필드(class, class_mask, driver_data)가 0 이 되고, 그 0 이
 * pci_match_one_device() 에서 "클래스는 보지 않는다" 로 해석된다.
 *
 * 방향 분기가 이 한 함수에만 있는 덕분에, 위쪽 공개 함수들은 두 탐색 함수의
 * 존재를 알 필요가 없다.
 *
 * 실행 컨텍스트: 장치 조회 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_get_subsys() / pci_get_device() / pci_get_device_reverse()
 *     → [이 함수] → pci_get_dev_by_id() 또는 pci_get_dev_by_id_reverse()
 */
static struct pci_dev *__pci_get_subsys(unsigned int vendor, unsigned int device,
				 unsigned int ss_vendor, unsigned int ss_device,
				 struct pci_dev *from, enum pci_search_direction dir)
{
	/* [한국어] 조건을 담을 구조체를 스택에 만든다. 지정 초기화라 나머지 필드는 0 이다. */
	struct pci_device_id id = {
		/* [한국어] 벤더 ID. */
		.vendor = vendor,
		.device = device,
		.subvendor = ss_vendor,
		.subdevice = ss_device,
	};

	/* [한국어] 정방향이면, */
	if (dir == PCI_SEARCH_FORWARD)
		/* [한국어] 앞에서부터 찾고, */
		return pci_get_dev_by_id(&id, from);
	else
		/* [한국어] 아니면 뒤에서부터 찾는다. 두 방향의 차이가 이 분기 하나로 모여 있어
		 * 위쪽 공개 함수들이 방향을 신경 쓰지 않아도 된다. */
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
struct pci_dev *pci_get_subsys(unsigned int vendor, unsigned int device,
			       unsigned int ss_vendor, unsigned int ss_device,
			       struct pci_dev *from)
{
	/* [한국어] 정방향으로 위임한다. */
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
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
			       struct pci_dev *from)
{
	/* [한국어] 서브시스템 조건을 "아무거나" 로 두고 위 함수에 위임한다. */
	return pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}
EXPORT_SYMBOL(pci_get_device);

/*
 * Same semantics as pci_get_device(), except walks the PCI device list
 * in reverse discovery order.
 */
struct pci_dev *pci_get_device_reverse(unsigned int vendor,
				       unsigned int device,
				       struct pci_dev *from)
{
	/* [한국어] 같은 조건을 역방향으로 찾는다. */
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
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from)
{
	/* [한국어] 조건 구조체. 이번에는 클래스 코드로 찾는다. */
	struct pci_device_id id = {
		/* [한국어] 벤더는 아무거나. */
		.vendor = PCI_ANY_ID,
		.device = PCI_ANY_ID,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
		.class_mask = PCI_ANY_ID,
		.class = class,
	};

	/* [한국어] 정방향으로 찾는다. */
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
struct pci_dev *pci_get_base_class(unsigned int class, struct pci_dev *from)
{
	/* [한국어] 조건 구조체. 클래스 코드에 마스크가 붙는 판이다. */
	struct pci_device_id id = {
		/* [한국어] 벤더는 아무거나. */
		.vendor = PCI_ANY_ID,
		.device = PCI_ANY_ID,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
		.class_mask = 0xFF0000,
		.class = class << 16,
	};

	/* [한국어] 정방향으로 찾는다. */
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
int pci_dev_present(const struct pci_device_id *ids)
{
	/* [한국어] 찾은 장치. */
	struct pci_dev *found = NULL;

	/* [한국어] 표의 끝(벤더·서브벤더·클래스 마스크가 모두 0)에 닿을 때까지 순회한다.
	 * 세 필드를 함께 보는 이유는 어느 하나만으로는 끝 표시를 구분할 수 없기
	 * 때문이다. */
	while (ids->vendor || ids->subvendor || ids->class_mask) {
		/* [한국어] 이 조건에 맞는 장치를 찾는다. */
		found = pci_get_dev_by_id(ids, NULL);
		/* [한국어] 하나라도 있으면, */
		if (found) {
			pci_dev_put(found);
			/* [한국어] 있다고 답한다. 참조는 위에서 놓은 뒤다. */
			return 1;
		}
		/* [한국어] 다음 조건으로. */
		ids++;
	}

	return 0;
}
EXPORT_SYMBOL(pci_dev_present);
