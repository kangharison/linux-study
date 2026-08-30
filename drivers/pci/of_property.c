// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022-2023, Advanced Micro Devices, Inc.
 */

/*
 * NVMe 관점 요약:
 *  이 파일은 PCI 장치(bridge 또는 endpoint)와 PCI host bridge를 대상으로
 *  Open Firmware / Device Tree(DT)에서 사용하는 속성(ranges, reg, interrupts,
 *  compatible, interrupt-map 등)을 생성하는 PCI OF helper 모음이다.
 *
 *  NVMe SSD는 일반적으로 PCIe root complex 아래의 endpoint로 동작하므로
 *  drivers/nvme/host/pci.c 입장에서는 pci_dev가 bridge가 아닌 경우
 *  of_pci_add_properties()의 else 분기(endpoint 경로)를 통해
 *  ranges, reg, compatible, interrupts 등의 DT property가 채워진다.
 *
 *  NVMe 장치가 연결된 PCIe 포트(bridge)를 기준으로 하면
 *  of_pci_add_host_bridge_properties()를 통해 host bridge의
 *  device_type, #address-cells, #size-cells, ranges 속성이 생성된다.
 *
 *  주요 호출 경로(개념):
 *    NVMe pci_dev -> of_pci_add_properties() -> of_pci_prop_ranges()
 *                                           -> of_pci_prop_reg()
 *                                           -> of_pci_prop_compatible()
 *                                           -> of_pci_prop_interrupts()
 *                                           -> of_pci_prop_intr_ctrl()
 *    NVMe가 탑재된 host bridge -> of_pci_add_host_bridge_properties()
 *                            -> of_pci_host_bridge_prop_ranges()
 */

/* NVMe: PCI 핵심 구조체와 함수를 사용하기 위한 헤더 */
#include <linux/pci.h>
/* NVMe: Open Firmware / Device Tree 파싱 및 조작 헤더 */
#include <linux/of.h>
/* NVMe: OF 기반 인터럽트 파싱 헤더 */
#include <linux/of_irq.h>
/* NVMe: 비트 필드 준비/추출 매크로 */
#include <linux/bitfield.h>
/* NVMe: GENMASK, BIT 등 비트 연산 매크로 */
#include <linux/bits.h>
/* NVMe: PCI 서브시스템 내부 헤더, pci_host_bridge 등 핵심 구조체 포함 */
#include "pci.h"

/* NVMe: PCI 주소 셀 개수: flag+devfn/bus, 상위 32bit, 하위 32bit */
#define OF_PCI_ADDRESS_CELLS		3
/* NVMe: PCI 크기 셀 개수: 상위 32bit, 하위 32bit */
#define OF_PCI_SIZE_CELLS		2
/* NVMe: PCI 장치가 지원하는 최대 INT_PIN 개수(INTA~INTD) */
#define OF_PCI_MAX_INT_PIN		4

/* NVMe: DT 'reg' 속성에 들어갈 물리 주소/크기 쌍 */
struct of_pci_addr_pair {
	/* NVMe: 3개의 PCI 주소 셀 */
	u32		phys_addr[OF_PCI_ADDRESS_CELLS];
	/* NVMe: 2개의 크기 셀 */
	u32		size[OF_PCI_SIZE_CELLS];
};

/*
 * Each entry in the ranges table is a tuple containing the child address,
 * the parent address, and the size of the region in the child address space.
 * Thus, for PCI, in each entry parent address is an address on the primary
 * side and the child address is the corresponding address on the secondary
 * side.
 */
/* NVMe: DT 'ranges' 속성의 한 항목(bridge 경로에서 사용) */
struct of_pci_range_entry {
	/* NVMe: secondary(child) 측 PCI 주소 셀 */
	u32		child_addr[OF_PCI_ADDRESS_CELLS];
	/* NVMe: primary(parent) 측 PCI 주소 셀 */
	u32		parent_addr[OF_PCI_ADDRESS_CELLS];
	/* NVMe: 영역 크기 셀 */
	u32		size[OF_PCI_SIZE_CELLS];
};

/* NVMe: PCI 주소 공간이 I/O 공간임을 나타내는 플래그 */
#define OF_PCI_ADDR_SPACE_IO		0x1
/* NVMe: PCI 주소 공간이 32bit 메모리 공간임을 나타내는 플래그 */
#define OF_PCI_ADDR_SPACE_MEM32		0x2
/* NVMe: PCI 주소 공간이 64bit 메모리 공간임을 나타내는 플래그 */
#define OF_PCI_ADDR_SPACE_MEM64		0x3

/* NVMe: non-relocatable(고정) 주소임을 표시하는 비트: 주소 셀 0의 bit 31 */
#define OF_PCI_ADDR_FIELD_NONRELOC	BIT(31)
/* NVMe: 주소 공간 유형(IO/MEM32/MEM64)을 담는 비트 필드: bit 25:24 */
#define OF_PCI_ADDR_FIELD_SS		GENMASK(25, 24)
/* NVMe: prefetchable 메모리 영역임을 표시하는 비트: 주소 셀 0의 bit 30 */
#define OF_PCI_ADDR_FIELD_PREFETCH	BIT(30)
/* NVMe: PCI 버스 번호를 담는 비트 필드: bit 23:16 */
#define OF_PCI_ADDR_FIELD_BUS		GENMASK(23, 16)
/* NVMe: PCI 장치 번호를 담는 비트 필드: bit 15:11 */
#define OF_PCI_ADDR_FIELD_DEV		GENMASK(15, 11)
/* NVMe: PCI 기능 번호를 담는 비트 필드: bit 10:8 */
#define OF_PCI_ADDR_FIELD_FUNC		GENMASK(10, 8)
/* NVMe: PCI BAR/구성 공간 레지스터 번호를 담는 비트 필드: bit 7:0 */
#define OF_PCI_ADDR_FIELD_REG		GENMASK(7, 0)

/* NVMe: 'compatible' 속성에 사용할 호환 문자열 종류 */
enum of_pci_prop_compatible {
	/* NVMe: vendor/device 기반 호환 문자열, 예: pci144d,a808 */
	PROP_COMPAT_PCI_VVVV_DDDD,
	/* NVMe: class 코드(6자리) 기반 호환 문자열, 예: pciclass,010802 */
	PROP_COMPAT_PCICLASS_CCSSPP,
	/* NVMe: class 코드(4자리) 기반 호환 문자열, 예: pciclass,0108 */
	PROP_COMPAT_PCICLASS_CCSS,
	/* NVMe: 호환 문자열 개수 */
	PROP_COMPAT_NUM,
};

/* NVMe: DT 'reg'/'ranges' 등에 기록할 PCI 주소 셀을 구성하는 helper 함수 */
static void of_pci_set_address(struct pci_dev *pdev, u32 *prop, u64 addr,
			       u32 reg_num, u32 flags, bool reloc)
{
	/* NVMe: 유효한 pci_dev가 주어지면 BDF(bus/dev/func) 정보를 채운다 */
	if (pdev) {
		/* NVMe: 버스 번호를 bit 23:16에 배치 */
		prop[0] = FIELD_PREP(OF_PCI_ADDR_FIELD_BUS, pdev->bus->number) |
			  /* NVMe: 장치 번호를 bit 15:11에 배치 */
			  FIELD_PREP(OF_PCI_ADDR_FIELD_DEV, PCI_SLOT(pdev->devfn)) |
			  /* NVMe: 기능 번호를 bit 10:8에 배치 */
			  FIELD_PREP(OF_PCI_ADDR_FIELD_FUNC, PCI_FUNC(pdev->devfn));
	} else
		/* NVMe: pdev가 없으면 첫 셀을 0으로 초기화 */
		prop[0] = 0;

	/* NVMe: 공간 유형/프리페치 등 플래그와 레지스터 번호를 OR한다 */
	prop[0] |= flags | reg_num;
	/* NVMe: non-relocatable(고정) 주소인 경우 */
	if (!reloc) {
		/* NVMe: non-relocatable 비트를 설정 */
		prop[0] |= OF_PCI_ADDR_FIELD_NONRELOC;
		/* NVMe: 64bit 주소의 상위 32bit를 두 번째 셀에 기록 */
		prop[1] = upper_32_bits(addr);
		/* NVMe: 64bit 주소의 하위 32bit를 세 번째 셀에 기록 */
		prop[2] = lower_32_bits(addr);
	}
}

/* NVMe: struct resource의 플래그를 PCI OF 주소 공간 플래그로 변환 */
static int of_pci_get_addr_flags(const struct resource *res, u32 *flags)
{
	/* NVMe: 변환된 공간 유형을 임시 저장 */
	u32 ss;

	/* NVMe: I/O 자원이면 IO 공간으로 분류 */
	if (res->flags & IORESOURCE_IO)
		ss = OF_PCI_ADDR_SPACE_IO;
	/* NVMe: 64bit 메모리 자원이면 MEM64 공간으로 분류 */
	else if (res->flags & IORESOURCE_MEM_64)
		ss = OF_PCI_ADDR_SPACE_MEM64;
	/* NVMe: 32bit 메모리 자원이면 MEM32 공간으로 분류 */
	else if (res->flags & IORESOURCE_MEM)
		ss = OF_PCI_ADDR_SPACE_MEM32;
	else
		/* NVMe: PCI 주소 공간에 맞지 않는 자원이면 오류 반환 */
		return -EINVAL;

	/* NVMe: 출력 플래그를 0으로 초기화 */
	*flags = 0;
	/* NVMe: prefetchable 자원이면 prefetch 비트 추가 */
	if (res->flags & IORESOURCE_PREFETCH)
		*flags |= OF_PCI_ADDR_FIELD_PREFETCH;

	/* NVMe: 공간 유형을 SS 필드에 배치 */
	*flags |= FIELD_PREP(OF_PCI_ADDR_FIELD_SS, ss);

	/* NVMe: 플래그 변환 성공 */
	return 0;
}

/* NVMe: PCI bridge 아래의 버스 범위를 DT 'bus-range' 속성으로 생성 */
static int of_pci_prop_bus_range(struct pci_dev *pdev,
				 struct of_changeset *ocs,
				 struct device_node *np)
{
	/* NVMe: bridge의 하위 버스 시작/끝을 배열에 저장 */
	u32 bus_range[] = { pdev->subordinate->busn_res.start,
			    pdev->subordinate->busn_res.end };

	/* NVMe: 'bus-range' 속성을 changeset에 추가 */
	return of_changeset_add_prop_u32_array(ocs, np, "bus-range", bus_range,
					       ARRAY_SIZE(bus_range));
}

/* NVMe: PCI 장치의 BAR(또는 bridge window)를 DT 'ranges' 속성으로 생성 */
static int of_pci_prop_ranges(struct pci_dev *pdev, struct of_changeset *ocs,
			      struct device_node *np)
{
	/* NVMe: 생성할 ranges 항목 배열 */
	struct of_pci_range_entry *rp;
	/* NVMe: pdev의 resource 배열 시작 포인터 */
	struct resource *res;
	/* NVMe: 반복 인덱스 및 반환값 */
	int i, j, ret;
	/* NVMe: 주소 공간 플래그와 처리할 BAR/bridge 자원 개수 */
	u32 flags, num;
	/* NVMe: 64bit 주소/크기 계산용 임시 변수 */
	u64 val64;

	/* NVMe: bridge이면 bridge 자원을, endpoint이면 표준 BAR를 사용 */
	if (pci_is_bridge(pdev)) {
		/* NVMe: PCI-to-PCI bridge의 window 자원 개수 */
		num = PCI_BRIDGE_RESOURCE_NUM;
		/* NVMe: bridge window 자원 배열 시작 위치 */
		res = &pdev->resource[PCI_BRIDGE_RESOURCES];
	} else {
		/* NVMe: NVMe SSD 같은 endpoint의 표준 BAR 개수(6개) */
		num = PCI_STD_NUM_BARS;
		/* NVMe: 표준 BAR 자원 배열 시작 위치 */
		res = &pdev->resource[PCI_STD_RESOURCES];
	}

	/* NVMe: num 개수만큼 range 항목을 할당 */
	rp = kzalloc_objs(*rp, num);
	/* NVMe: 메모리 할당 실패 시 -ENOMEM 반환 */
	if (!rp)
		return -ENOMEM;

	/* NVMe: 각 BAR/window를 순회하며 유효한 항목을 ranges로 변환 */
	for (i = 0, j = 0; j < num; j++) {
		/* NVMe: 자원 크기가 0이면 비활성 BAR이므로 건다 */
		if (!resource_size(&res[j]))
			continue;

		/* NVMe: 자원 플래그를 OF 주소 공간 플래그로 변환, 실패 시 건다 */
		if (of_pci_get_addr_flags(&res[j], &flags))
			continue;

		/* NVMe: PCI 버스 주소(translation 적용 전)를 가져옴 */
		val64 = pci_bus_address(pdev, &res[j] - pdev->resource);
		/* NVMe: parent_addr(PCI 측 주소) 셀을 구성, non-relocatable */
		of_pci_set_address(pdev, rp[i].parent_addr, val64, 0, flags,
				   false);
		/* NVMe: bridge이면 child_addr를 parent_addr와 동일하게 설정 */
		if (pci_is_bridge(pdev)) {
			memcpy(rp[i].child_addr, rp[i].parent_addr,
			       sizeof(rp[i].child_addr));
		} else {
			/*
			 * For endpoint device, the lower 64-bits of child
			 * address is always zero.
			 */
			/* NVMe: endpoint(NVMe 등)는 child_addr 하위를 BAR 인덱스로 사용 */
			rp[i].child_addr[0] = j;
		}

		/* NVMe: 자원 크기를 64bit로 읽음 */
		val64 = resource_size(&res[j]);
		/* NVMe: 크기 상위 32bit */
		rp[i].size[0] = upper_32_bits(val64);
		/* NVMe: 크기 하위 32bit */
		rp[i].size[1] = lower_32_bits(val64);

		/* NVMe: 유효한 항목 하나 추가 */
		i++;
	}

	/* NVMe: 구성된 range 항목들을 'ranges' 속성으로 DT에 추가 */
	ret = of_changeset_add_prop_u32_array(ocs, np, "ranges", (u32 *)rp,
					      i * sizeof(*rp) / sizeof(u32));
	/* NVMe: 할당한 range 배열 해제 */
	kfree(rp);

	/* NVMe: changeset 추가 결과 반환 */
	return ret;
}

/* NVMe: PCI 장치의 구성 공간 위치를 DT 'reg' 속성으로 생성 */
static int of_pci_prop_reg(struct pci_dev *pdev, struct of_changeset *ocs,
			   struct device_node *np)
{
	/* NVMe: 'reg' 속성에 기록할 주소/크기 쌍을 0으로 초기화 */
	struct of_pci_addr_pair reg = { 0 };

	/* configuration space */
	/* NVMe: 구성 공간(Configuration Space) 주소를 셀로 구성, relocatable로 설정 */
	of_pci_set_address(pdev, reg.phys_addr, 0, 0, 0, true);

	/* NVMe: 'reg' 속성을 changeset에 추가 */
	return of_changeset_add_prop_u32_array(ocs, np, "reg", (u32 *)&reg,
					       sizeof(reg) / sizeof(u32));
}

/* NVMe: PCI_INTERRUPT_PIN을 읽어 DT 'interrupts' 속성으로 생성 */
static int of_pci_prop_interrupts(struct pci_dev *pdev,
				  struct of_changeset *ocs,
				  struct device_node *np)
{
	/* NVMe: 반환값 저장 */
	int ret;
	/* NVMe: 구성 공간 interrupt pin 값 */
	u8 pin;

	/* NVMe: PCI 구성 공간에서 interrupt pin 레지스터 읽기 */
	ret = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
	/* NVMe: 구성 공간 읽기 실패 시 해당 오류 반환 */
	if (ret != 0)
		return ret;

	/* NVMe: pin이 0이면 인터럽트 없음, 속성 추가 없이 성공 반환 */
	if (!pin)
		return 0;

	/* NVMe: pin 값을 'interrupts' 속성으로 추가 */
	return of_changeset_add_prop_u32(ocs, np, "interrupts", (u32)pin);
}

/* NVMe: endpoint가 직접 interrupt-controller임을 표시하는 속성 생성 */
static int of_pci_prop_intr_ctrl(struct pci_dev *pdev, struct of_changeset *ocs,
				 struct device_node *np)
{
	/* NVMe: 반환값 저장 */
	int ret;
	/* NVMe: PCI interrupt pin 값 */
	u8 pin;

	/* NVMe: PCI 구성 공간에서 interrupt pin 읽기 */
	ret = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
	/* NVMe: 읽기 실패 시 오류 반환 */
	if (ret != 0)
		return ret;

	/* NVMe: pin이 0이면 인터럽트 관련 속성 추가 안 함 */
	if (!pin)
		return 0;

	/* NVMe: 인터럽트 specifier 셀 개수를 1로 설정 */
	ret = of_changeset_add_prop_u32(ocs, np, "#interrupt-cells", 1);
	/* NVMe: 추가 실패 시 오류 반환 */
	if (ret)
		return ret;

	/* NVMe: 이 장치가 interrupt controller임을 표시하는 bool 속성 추가 */
	return of_changeset_add_prop_bool(ocs, np, "interrupt-controller");
}

/* NVMe: bridge 아래 장치들의 인터럽트 라우팅 정보를 DT 'interrupt-map'으로 생성 */
static int of_pci_prop_intr_map(struct pci_dev *pdev, struct of_changeset *ocs,
				struct device_node *np)
{
	/* NVMe: pin 순회용 인덱스, 각 pin별 parent #address-cells, map 총 크기 */
	u32 i, addr_sz[OF_PCI_MAX_INT_PIN] = { 0 }, map_sz = 0;
	/* NVMe: 각 INT_PIN에 대해 해석된 출력 인터럽트 정보 */
	struct of_phandle_args out_irq[OF_PCI_MAX_INT_PIN];
	/* NVMe: irq 파싱용 로컬 PCI 주소, big-endian으로 사용 */
	__be32 laddr[OF_PCI_ADDRESS_CELLS] = { 0 };
	/* NVMe: interrupt-map-mask 속성 값: bus/devfn, 셀 2개, pin */
	u32 int_map_mask[] = { 0xffff00, 0, 0, 7 };
	/* NVMe: 부모(인터럽트 컨트롤러) DT 노드 */
	struct device_node *pnode;
	/* NVMe: bridge 아래 순회할 자식 PCI 장치 */
	struct pci_dev *child;
	/* NVMe: interrupt-map 속성에 기록할 버퍼와 현재 쓰기 위치 */
	u32 *int_map, *mapp;
	/* NVMe: 함수 반환값 */
	int ret;
	/* NVMe: 현재 처리 중인 interrupt pin 값 */
	u8 pin;

	/* NVMe: bridge 상위 장치(parent PCI device)에 해당하는 OF 노드 획득 */
	pnode = pci_device_to_OF_node(pdev->bus->self);
	/* NVMe: parent device 노드가 없으면 bus 자체의 OF 노드를 사용 */
	if (!pnode)
		pnode = pci_bus_to_OF_node(pdev->bus);

	/* NVMe: 부모 OF 노드를 찾지 못하면 interrupt-map 생성 불가 */
	if (!pnode) {
		pci_err(pdev, "failed to get parent device node");
		return -EINVAL;
	}

	/* NVMe: 현재 bridge의 bus/devfn을 big-endian 주소 셀로 구성 */
	laddr[0] = cpu_to_be32((pdev->bus->number << 16) | (pdev->devfn << 8));
	/* NVMe: INTA~INTD(pin 1~4)에 대해 각각 인터럽트 라우팅 해석 */
	for (pin = 1; pin <= OF_PCI_MAX_INT_PIN;  pin++) {
		/* NVMe: 배열 인덱스는 pin - 1 */
		i = pin - 1;
		/* NVMe: 출력 인터럽트의 parent 노드 설정 */
		out_irq[i].np = pnode;
		/* NVMe: 인터럽트 specifier 인수 개수를 1로 설정 */
		out_irq[i].args_count = 1;
		/* NVMe: 인터럽트 specifier 첫 인수로 pin 번호 설정 */
		out_irq[i].args[0] = pin;
		/* NVMe: raw OF 주소를 기반으로 인터럽트 라우팅 해석 */
		ret = of_irq_parse_raw(laddr, &out_irq[i]);
		/* NVMe: 해석 실패 시 해당 pin의 np를 NULL로 표시하고 다음 pin으로 */
		if (ret) {
			out_irq[i].np = NULL;
			pci_dbg(pdev, "parse irq %d failed, ret %d", pin, ret);
			continue;
		}
		/* NVMe: parent 노드의 #address-cells 값을 읽어 저장 */
		of_property_read_u32(out_irq[i].np, "#address-cells",
				     &addr_sz[i]);
	}

	/* NVMe: bridge 아래의 모든 PCI 장치를 순회하며 map 크기 계산 */
	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) {
		/* NVMe: 각 장치의 INTA~INTD에 대해 */
		for (pin = 1; pin <= OF_PCI_MAX_INT_PIN; pin++) {
			/* NVMe: swizzling을 통해 parent 측 pin 인덱스 계산 */
			i = pci_swizzle_interrupt_pin(child, pin) - 1;
			/* NVMe: 해당 parent pin의 라우팅이 실패했으면 건다 */
			if (!out_irq[i].np)
				continue;
			/* NVMe: child 주소 3셀 + pin 1셀 + phandle 1셀 + parent 주소 셀 + args 셀 */
			map_sz += 5 + addr_sz[i] + out_irq[i].args_count;
		}
	}

	/*
	 * Parsing interrupt failed for all pins. In this case, it does not
	 * need to generate interrupt-map property.
	 */
	/* NVMe: 모든 pin 파싱에 실패하면 interrupt-map 속성을 만들지 않음 */
	if (!map_sz)
		return 0;

	/* NVMe: 계산된 크기만큼 interrupt-map용 u32 배열 할당 */
	int_map = kcalloc(map_sz, sizeof(u32), GFP_KERNEL);
	/* NVMe: 메모리 할당 실패 시 -ENOMEM 반환 */
	if (!int_map)
		return -ENOMEM;
	/* NVMe: 쓰기 포인터를 버퍼 시작으로 설정 */
	mapp = int_map;

	/* NVMe: 다시 각 자식 장치별로 interrupt-map 항목을 채움 */
	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) {
		for (pin = 1; pin <= OF_PCI_MAX_INT_PIN; pin++) {
			/* NVMe: swizzling으로 parent 측 pin 인덱스 계산 */
			i = pci_swizzle_interrupt_pin(child, pin) - 1;
			/* NVMe: 라우팅 실패한 pin은 건다 */
			if (!out_irq[i].np)
				continue;

			/* NVMe: map 항목의 child 주소: bus << 16 | devfn << 8 */
			*mapp = (child->bus->number << 16) |
				(child->devfn << 8);
			/* NVMe: 주소 셀 3개만큼 포인터 이동 */
			mapp += OF_PCI_ADDRESS_CELLS;
			/* NVMe: 인터럽트 specifier의 pin 번호 기록 */
			*mapp = pin;
			/* NVMe: pin 셀 1개 이동 */
			mapp++;
			/* NVMe: parent(인터럽트 컨트롤러) 노드의 phandle 기록 */
			*mapp = out_irq[i].np->phandle;
			/* NVMe: phandle 셀 1개 이동 */
			mapp++;

			/*
			 * A device address does not affect the device <->
			 * interrupt-controller HW connection for all
			 * modern interrupt controllers; moreover, the
			 * kernel (i.e., of_irq_parse_raw()) ignores the
			 * values in the parent unit address cells while
			 * parsing the interrupt-map property because they
			 * are irrelevant for interrupt mapping in modern
			 * systems.
			 *
			 * Leave the parent unit address initialized to 0 --
			 * just take into account the #address-cells size
			 * to build the property properly.
			 */
			/* NVMe: parent 주소 셀은 0으로 두고 크기만큼 포인터 이동 */
			mapp += addr_sz[i];
			/* NVMe: 인터럽트 컨트롤러 인수를 map에 복사 */
			memcpy(mapp, out_irq[i].args,
			       out_irq[i].args_count * sizeof(u32));
			/* NVMe: args 셀만큼 포인터 이동 */
			mapp += out_irq[i].args_count;
		}
	}

	/* NVMe: 구성된 interrupt-map 속성을 DT에 추가 */
	ret = of_changeset_add_prop_u32_array(ocs, np, "interrupt-map", int_map,
					      map_sz);
	/* NVMe: 추가 실패 시 failed 레이블로 이동하여 정리 */
	if (ret)
		goto failed;

	/* NVMe: #interrupt-cells 속성을 1로 추가 */
	ret = of_changeset_add_prop_u32(ocs, np, "#interrupt-cells", 1);
	/* NVMe: 추가 실패 시 정리 */
	if (ret)
		goto failed;

	/* NVMe: interrupt-map-mask 속성 추가 */
	ret = of_changeset_add_prop_u32_array(ocs, np, "interrupt-map-mask",
					      int_map_mask,
					      ARRAY_SIZE(int_map_mask));
	/* NVMe: 추가 실패 시 정리 */
	if (ret)
		goto failed;

	/* NVMe: 모든 속성 추가 성공, 할당한 버퍼 해제 */
	kfree(int_map);
	/* NVMe: 성공 반환 */
	return 0;

failed:
	/* NVMe: 오류 발생 시 할당한 interrupt-map 버퍼 해제 */
	kfree(int_map);
	/* NVMe: 오류 코드 반환 */
	return ret;
}

/* NVMe: PCI vendor/device 및 class 코드 기반 'compatible' 문자열 생성 */
static int of_pci_prop_compatible(struct pci_dev *pdev,
				  struct of_changeset *ocs,
				  struct device_node *np)
{
	/* NVMe: 호환 문자열 포인터 배열, 개수만큼 초기화 */
	const char *compat_strs[PROP_COMPAT_NUM] = { 0 };
	/* NVMe: 반복 인덱스 및 반환값 */
	int i, ret;

	/* NVMe: vendor/device 기반 문자열 할당, 예: pci144d,a808 (Samsung NVMe) */
	compat_strs[PROP_COMPAT_PCI_VVVV_DDDD] =
		kasprintf(GFP_KERNEL, "pci%x,%x", pdev->vendor, pdev->device);
	/* NVMe: 6자리 class 코드 기반 문자열 할당, NVMe는 0x010802 */
	compat_strs[PROP_COMPAT_PCICLASS_CCSSPP] =
		kasprintf(GFP_KERNEL, "pciclass,%06x", pdev->class);
	/* NVMe: 4자리 class 코드 기반 문자열 할당 */
	compat_strs[PROP_COMPAT_PCICLASS_CCSS] =
		kasprintf(GFP_KERNEL, "pciclass,%04x", pdev->class >> 8);

	/* NVMe: 'compatible' 속성을 문자열 배열로 DT에 추가 */
	ret = of_changeset_add_prop_string_array(ocs, np, "compatible",
						 compat_strs, PROP_COMPAT_NUM);
	/* NVMe: kasprintf로 할당한 문자열들을 해제 */
	for (i = 0; i < PROP_COMPAT_NUM; i++)
		kfree(compat_strs[i]);

	/* NVMe: compatible 속성 추가 결과 반환 */
	return ret;
}

/* NVMe: 단일 PCI 장치에 대해 OF property들을 생성하여 changeset에 추가 */
int of_pci_add_properties(struct pci_dev *pdev, struct of_changeset *ocs,
			  struct device_node *np)
{
	/* NVMe: 각 단계의 반환값 저장 */
	int ret;

	/*
	 * The added properties will be released when the
	 * changeset is destroyed.
	 */
	/* NVMe: bridge이면 bridge 전용 속성들 추가 */
	if (pci_is_bridge(pdev)) {
		/* NVMe: device_type을 "pci"로 설정 */
		ret = of_changeset_add_prop_string(ocs, np, "device_type",
						   "pci");
		/* NVMe: 추가 실패 시 즉시 반환 */
		if (ret)
			return ret;

		/* NVMe: 하위 버스 범위 속성 추가 */
		ret = of_pci_prop_bus_range(pdev, ocs, np);
		if (ret)
			return ret;

		/* NVMe: 하위 장치 인터럽트 라우팅 테이블 추가 */
		ret = of_pci_prop_intr_map(pdev, ocs, np);
		if (ret)
			return ret;
	} else {
		/* NVMe: NVMe SSD 같은 endpoint는 interrupt-controller 속성 추가 */
		ret = of_pci_prop_intr_ctrl(pdev, ocs, np);
		if (ret)
			return ret;
	}

	/* NVMe: BAR/bridge window를 ranges 속성으로 추가 */
	ret = of_pci_prop_ranges(pdev, ocs, np);
	if (ret)
		return ret;

	/* NVMe: #address-cells를 3으로 추가 */
	ret = of_changeset_add_prop_u32(ocs, np, "#address-cells",
					OF_PCI_ADDRESS_CELLS);
	if (ret)
		return ret;

	/* NVMe: #size-cells를 2로 추가 */
	ret = of_changeset_add_prop_u32(ocs, np, "#size-cells",
					OF_PCI_SIZE_CELLS);
	if (ret)
		return ret;

	/* NVMe: 구성 공간 주소를 reg 속성으로 추가 */
	ret = of_pci_prop_reg(pdev, ocs, np);
	if (ret)
		return ret;

	/* NVMe: vendor/device/class 기반 compatible 문자열 추가 */
	ret = of_pci_prop_compatible(pdev, ocs, np);
	if (ret)
		return ret;

	/* NVMe: interrupt pin 값을 interrupts 속성으로 추가 */
	ret = of_pci_prop_interrupts(pdev, ocs, np);
	if (ret)
		return ret;

	/* NVMe: 모든 OF property 추가 완료 */
	return 0;
}

/* NVMe: resource가 PCI OF 'ranges'에 포함될 메모리 자원인지 검사 */
static bool of_pci_is_range_resource(const struct resource *res, u32 *flags)
{
	/* NVMe: 메모리 자원이 아니고 64bit 메모리 자원도 아니면 제외 */
	if (!(resource_type(res) & IORESOURCE_MEM) &&
	    !(resource_type(res) & IORESOURCE_MEM_64))
		return false;

	/* NVMe: OF 주소 공간 플래그 변환에 실패하면 제외 */
	if (of_pci_get_addr_flags(res, flags))
		return false;

	/* NVMe: 메모리 자원이며 OF 플래그 변환 성공, ranges 대상임 */
	return true;
}

/* NVMe: PCI host bridge의 window 목록을 DT 'ranges' 속성으로 생성 */
static int of_pci_host_bridge_prop_ranges(struct pci_host_bridge *bridge,
					  struct of_changeset *ocs,
					  struct device_node *np)
{
	/* NVMe: host bridge window 리스트 항목 */
	struct resource_entry *window;
	/* NVMe: ranges 속성의 총 u32 개수 */
	unsigned int ranges_sz = 0;
	/* NVMe: 메모리 window 개수 */
	unsigned int n_range = 0;
	/* NVMe: 현재 window의 resource 포인터 */
	struct resource *res;
	/* NVMe: DT 노드의 parent 주소 셀 개수 */
	int n_addr_cells;
	/* NVMe: ranges 속성 버퍼 */
	u32 *ranges;
	/* NVMe: 64bit 주소/크기 계산용 */
	u64 val64;
	/* NVMe: OF 주소 공간 플래그 */
	u32 flags;
	/* NVMe: 반환값 */
	int ret;

	/* NVMe: DT 노드의 #address-cells 값을 가져옴 */
	n_addr_cells = of_n_addr_cells(np);
	/* NVMe: #address-cells가 1 또는 2가 아니면 잘못된 DT이므로 오류 */
	if (n_addr_cells <= 0 || n_addr_cells > 2)
		return -EINVAL;

	/* NVMe: bridge의 window 리스트를 순회하며 메모리 window 개수 계산 */
	resource_list_for_each_entry(window, &bridge->windows) {
		res = window->res;
		if (!of_pci_is_range_resource(res, &flags))
			continue;
		n_range++;
	}

	/* NVMe: 메모리 window가 없으면 ranges 속성 추가 없이 성공 */
	if (!n_range)
		return 0;

	/* NVMe: 각 range 항목 = PCI 주소 3셀 + parent 주소 n_addr_cells 셀 + 크기 2셀 */
	ranges = kcalloc(n_range,
			 (OF_PCI_ADDRESS_CELLS + OF_PCI_SIZE_CELLS +
			  n_addr_cells) * sizeof(*ranges),
			 GFP_KERNEL);
	/* NVMe: 메모리 할당 실패 시 -ENOMEM 반환 */
	if (!ranges)
		return -ENOMEM;

	/* NVMe: 다시 window 리스트를 순회하며 ranges 데이터 채움 */
	resource_list_for_each_entry(window, &bridge->windows) {
		res = window->res;
		if (!of_pci_is_range_resource(res, &flags))
			continue;

		/* PCI bus address */
		/* NVMe: window의 시작 주소를 PCI 버스 주소로 변환(offset 보정) */
		val64 = res->start;
		/* NVMe: PCI 측 주소 셀 3개를 채움, pdev는 없으므로 BDF 필드는 0 */
		of_pci_set_address(NULL, &ranges[ranges_sz],
				   val64 - window->offset, 0, flags, false);
		/* NVMe: PCI 주소 셀만큼 인덱스 이동 */
		ranges_sz += OF_PCI_ADDRESS_CELLS;

		/* Host bus address */
		/* NVMe: parent(host) 주소 셀이 2개면 상위 32bit 먼저 기록 */
		if (n_addr_cells == 2)
			ranges[ranges_sz++] = upper_32_bits(val64);
		/* NVMe: parent(host) 주소 하위 32bit 기록 */
		ranges[ranges_sz++] = lower_32_bits(val64);

		/* Size */
		/* NVMe: window 크기를 64bit로 읽음 */
		val64 = resource_size(res);
		/* NVMe: 크기 상위 32bit 기록 */
		ranges[ranges_sz] = upper_32_bits(val64);
		/* NVMe: 크기 하위 32bit 기록 */
		ranges[ranges_sz + 1] = lower_32_bits(val64);
		/* NVMe: 크기 셀만큼 인덱스 이동 */
		ranges_sz += OF_PCI_SIZE_CELLS;
	}

	/* NVMe: 구성된 ranges 속성을 DT에 추가 */
	ret = of_changeset_add_prop_u32_array(ocs, np, "ranges", ranges,
					      ranges_sz);
	/* NVMe: 할당한 ranges 버퍼 해제 */
	kfree(ranges);
	/* NVMe: 추가 결과 반환 */
	return ret;
}

/* NVMe: PCI host bridge(루트 컴플렉스)에 대한 OF property들을 생성 */
int of_pci_add_host_bridge_properties(struct pci_host_bridge *bridge,
				      struct of_changeset *ocs,
				      struct device_node *np)
{
	/* NVMe: 반환값 저장 */
	int ret;

	/* NVMe: host bridge의 device_type을 "pci"로 설정 */
	ret = of_changeset_add_prop_string(ocs, np, "device_type", "pci");
	if (ret)
		return ret;

	/* NVMe: #address-cells를 3으로 설정 */
	ret = of_changeset_add_prop_u32(ocs, np, "#address-cells",
					OF_PCI_ADDRESS_CELLS);
	if (ret)
		return ret;

	/* NVMe: #size-cells를 2로 설정 */
	ret = of_changeset_add_prop_u32(ocs, np, "#size-cells",
					OF_PCI_SIZE_CELLS);
	if (ret)
		return ret;

	/* NVMe: host bridge window들을 ranges 속성으로 추가 */
	ret = of_pci_host_bridge_prop_ranges(bridge, ocs, np);
	if (ret)
		return ret;

	/* NVMe: host bridge OF property 추가 완료 */
	return 0;
}
