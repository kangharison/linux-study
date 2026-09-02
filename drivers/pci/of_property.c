// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022-2023, Advanced Micro Devices, Inc.
 */

/*
 * [한국어 설명] 커널의 PCI 정보를 DeviceTree 속성으로 내보내는 계층 (of_property.c)
 *
 * === 파일의 역할 ===
 * of.c 의 반대 방향이다. of.c 가 DT 를 읽어 PCI 자료구조를 만든다면,
 * 이 파일은 이미 만들어진 PCI 정보를 DT 속성 형태로 조립한다.
 *
 * 왜 그런 것이 필요한가. 가상화 때문이다. 호스트가 PCI 장치를 게스트에
 * 넘길 때, 게스트에게도 그 장치를 기술해 줘야 한다. 게스트가 DT 기반
 * 시스템이라면 그 기술은 DT 노드 형태여야 하고, 호스트는 자기가 아는
 * PCI 정보로 그것을 만들어 줘야 한다.
 *
 * 만드는 속성들:
 *   reg          — 장치 주소(bus/devfn 을 DT 형식으로 인코딩)
 *   ranges       — 브리지의 주소 변환 범위
 *   #address-cells / #size-cells — DT 의 주소 표현 규칙
 *   interrupt-map / interrupt-map-mask — INTx 라우팅
 *   device_type  — "pci"
 *
 * DT 의 주소 인코딩이 까다로운 부분이다. PCI 주소는 3개의 32비트 셀로
 * 표현되며, 첫 셀에 공간 종류(config/IO/mem32/mem64)와 prefetchable
 * 여부, bus/devfn 이 비트로 채워진다. 이 파일이 그 인코딩을 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 가상화 계층(VFIO 등) 또는 오버레이 생성 코드
 *   -> [이 파일] of_pci_add_properties(pdev, ...)
 *      -> reg / ranges / interrupt-map 등을 만들어 property 배열로
 *      -> 그것이 DT 오버레이가 되어 게스트에게 전달된다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 메모리 할당이 많다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: DT 오버레이를 만드는 코드(주로 가상화 관련).
 * 아래쪽: drivers/of/ 의 property 헬퍼, pci.c 의 자원 정보.
 * 옆쪽: of.c — 정확히 반대 방향의 변환을 한다.
 *
 * === NVMe 관점 ===
 * NVMe 와 직접 관련이 없다(전수 확인).
 *
 * 간접적으로는 NVMe 를 DT 기반 게스트에 통째로 넘기는 경우가 해당한다.
 * 그때 이 파일이 만든 DT 노드로 게스트가 그 NVMe 컨트롤러를 인식하고
 * 자기 nvme 드라이버를 붙인다.
 *
 * === 주요 함수/구조체 요약 ===
 * of_pci_add_properties()      : 장치 하나에 대한 DT 속성 묶음을 만든다.
 *                                이 파일의 진입점이다.
 * of_pci_set_address()         : PCI 주소를 DT 의 3셀 형식으로 채운다.
 *                                이 파일의 모든 주소 속성이 이것을 거친다.
 * of_pci_prop_reg()            : reg 속성. bus/devfn 을 DT 3셀 형식으로 인코딩.
 * of_pci_prop_ranges()         : 주소 변환 범위. 브리지는 창을, 일반 장치는
 *                                BAR 을 대상으로 삼는다.
 * (#address-cells 와 #size-cells 는 전용 함수 없이 of_pci_add_properties()
 *  안에서 3 과 2 를 직접 적는다 — PCI 에서 고정된 값이기 때문이다.)
 * of_pci_prop_intr_map()       : interrupt-map. 자식 장치들의 INTx 를
 *                                상위 컨트롤러 입력에 대응시킨다.
 * of_pci_prop_compatible()     : compatible 문자열("pciVVVV,DDDD" 형식).
 * of_pci_get_addr_flags()      : 자원의 종류를 DT 첫 셀의 플래그 비트로 변환.
 */

#include <linux/pci.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include "pci.h"

#define OF_PCI_ADDRESS_CELLS		3
#define OF_PCI_SIZE_CELLS		2
#define OF_PCI_MAX_INT_PIN		4

struct of_pci_addr_pair {
	/* [한국어] PCI 3셀 형식의 물리 주소. 첫 셀이 위치·공간 종류(설정/IO/MEM32/MEM64)
	 * 이고 나머지 두 셀이 64비트 주소를 상위·하위로 나눠 담는다.
	 * 설정자: of_pci_prop_reg() 가 of_pci_set_address() 로 채운다.
	 * 읽는 자: 생성된 디바이스 트리 노드의 reg 속성을 읽는 쪽.
	 * 값 범위: PCI 바인딩이 규정한 3셀 인코딩.
	 * 동기화: 함수 지역 배열이라 공유되지 않는다. */
	u32		phys_addr[OF_PCI_ADDRESS_CELLS];
	/* [한국어] 해당 영역의 크기를 2셀(64비트)로 담는다.
	 * 설정자: of_pci_prop_reg().
	 * 읽는 자: 위와 같다.
	 * 값 범위: 상위 셀이 크기의 상위 32비트, 하위 셀이 하위 32비트.
	 * 동기화: 함수 지역 배열. */
	u32		size[OF_PCI_SIZE_CELLS];
/* [한국어] reg 속성 하나가 갖는 형태 — 주소 3셀 + 크기 2셀. */
};

/*
 * Each entry in the ranges table is a tuple containing the child address,
 * the parent address, and the size of the region in the child address space.
 * Thus, for PCI, in each entry parent address is an address on the primary
 * side and the child address is the corresponding address on the secondary
 * side.
 */
struct of_pci_range_entry {
	/* [한국어] 자식(하위 버스) 쪽 주소 3셀. 위 영어 주석대로 ranges 항목은
	 * (자식 주소, 부모 주소, 크기) 세 짝이며, PCI 에서는 자식이 세컨더리 쪽,
	 * 부모가 프라이머리 쪽 주소다.
	 * 설정자: of_pci_prop_ranges() 가 of_pci_set_address() 로 채운다.
	 * 읽는 자: 디바이스 트리를 읽는 쪽이 이 주소를 부모 주소로 변환한다.
	 * 값 범위: PCI 3셀 형식.
	 * 동기화: 함수 지역 배열. */
	u32		child_addr[OF_PCI_ADDRESS_CELLS];
	/* [한국어] 부모(상위 버스) 쪽 주소 3셀.
	 * 설정자: of_pci_prop_ranges() 가 of_pci_set_address() 로 채운다.
	 * 읽는 자: 디바이스 트리를 읽는 쪽이 자식 주소를 이 주소로 옮긴다.
	 * 값 범위: PCI 3셀 형식 — 첫 셀이 위치·공간 종류, 나머지 둘이 64비트 주소.
	 * 동기화: 함수 지역 배열이라 공유되지 않는다. */
	u32		parent_addr[OF_PCI_ADDRESS_CELLS];
	/* [한국어] 이 구간의 크기 2셀.
	 * 설정자: of_pci_prop_ranges() 가 상위·하위 32비트로 나눠 담는다.
	 * 읽는 자: 트리를 읽는 쪽이 매핑 범위의 끝을 계산하는 데 쓴다.
	 * 값 범위: 64비트 크기를 빅엔디언 순서로 — [0]이 상위, [1]이 하위.
	 * 동기화: 함수 지역 배열이라 공유되지 않는다. */
	u32		size[OF_PCI_SIZE_CELLS];
/* [한국어] ranges 속성의 항목 하나. 자식 주소·부모 주소·크기가 한 벌이다. */
};

#define OF_PCI_ADDR_SPACE_IO		0x1
#define OF_PCI_ADDR_SPACE_MEM32		0x2
#define OF_PCI_ADDR_SPACE_MEM64		0x3

#define OF_PCI_ADDR_FIELD_NONRELOC	BIT(31)
#define OF_PCI_ADDR_FIELD_SS		GENMASK(25, 24)
#define OF_PCI_ADDR_FIELD_PREFETCH	BIT(30)
#define OF_PCI_ADDR_FIELD_BUS		GENMASK(23, 16)
#define OF_PCI_ADDR_FIELD_DEV		GENMASK(15, 11)
#define OF_PCI_ADDR_FIELD_FUNC		GENMASK(10, 8)
#define OF_PCI_ADDR_FIELD_REG		GENMASK(7, 0)

enum of_pci_prop_compatible {
	/* [한국어] 벤더·장치 ID 로 만든 가장 구체적인 문자열의 자리. */
	PROP_COMPAT_PCI_VVVV_DDDD,
	PROP_COMPAT_PCICLASS_CCSSPP,
	PROP_COMPAT_PCICLASS_CCSS,
	PROP_COMPAT_NUM,
};

/* [한국어]
 * of_pci_set_address - PCI 주소를 디바이스 트리의 3셀 형식으로 채운다
 *
 * @pdev: 주소의 주인이 될 장치. NULL 이면 위치 필드를 0 으로 둔다.
 * @prop: 결과를 담을 3워드 배열.
 * @addr: 주소 값.
 * @reg_num: 레지스터 번호(BAR 오프셋 등).
 * @flags: 공간 종류와 prefetchable 비트.
 * @reloc: 재배치 가능한 주소인지.
 *
 * 디바이스 트리에서 PCI 주소는 3개의 32비트 셀로 적힌다. 첫 셀이 "어느
 * 장치의 어떤 공간인가" 를 담고, 나머지 둘이 64비트 주소를 나눠 담는다.
 * 이 함수가 그 형식을 만든다.
 *
 * 첫 셀의 구조가 규격으로 정해져 있다 — 상위에 재배치·prefetch 표시와
 * 공간 종류가 있고, 그 아래 버스·장치·기능 번호가, 맨 아래 레지스터 번호가
 * 들어간다. FIELD_PREP 이 각 값을 제 자리로 옮긴다.
 *
 * pdev 가 NULL 인 경우는 호스트 브리지 창을 다룰 때다. 그 주소는 특정 장치에
 * 속하지 않으므로 위치 필드가 의미가 없다.
 *
 * reloc 이 이 함수의 두 쓰임을 가른다. 참이면 주소 셀을 채우지 않는데,
 * "아직 정해지지 않았고 커널이 정할 수 있다" 는 뜻이기 때문이다.
 * reg 속성이 그 형태로 쓰이고, ranges 속성은 거짓으로 실제 주소를 적는다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   of_pci_prop_ranges() / of_pci_prop_reg() / of_pci_host_bridge_prop_ranges()
 *     → [이 함수]
 */
static void of_pci_set_address(struct pci_dev *pdev, u32 *prop, u64 addr,
			       u32 reg_num, u32 flags, bool reloc)
{
	if (pdev) {
		prop[0] = FIELD_PREP(OF_PCI_ADDR_FIELD_BUS, pdev->bus->number) |
			  /* [한국어] 장치 번호는 devfn 의 상위 5비트다. */
			  FIELD_PREP(OF_PCI_ADDR_FIELD_DEV, PCI_SLOT(pdev->devfn)) |
			  /* [한국어] 기능 번호는 하위 3비트다. 셋을 합쳐 첫 셀의 위치 필드가 완성된다. */
			  FIELD_PREP(OF_PCI_ADDR_FIELD_FUNC, PCI_FUNC(pdev->devfn));
	/* [한국어] 장치가 지정되지 않았으면 — 호스트 브리지 창을 다룰 때다. */
	} else
		/* [한국어] 위치 필드를 0 으로 둔다. 그 주소는 특정 장치에 속하지 않는다. */
		prop[0] = 0;

	prop[0] |= flags | reg_num;
	/* [한국어] 재배치할 수 없는 주소면 — 즉 실제 주소가 이미 정해져 있으면, */
	if (!reloc) {
		/* [한국어] 그 사실을 표시하고, */
		prop[0] |= OF_PCI_ADDR_FIELD_NONRELOC;
		/* [한국어] 주소의 상위 32비트를, */
		prop[1] = upper_32_bits(addr);
		/* [한국어] 하위 32비트와 함께 나머지 두 셀에 담는다. reloc 이 참이면 이 셋을 건너뛰어,
		 * 주소 셀이 0 인 채로 남는다 — '아직 정해지지 않았다' 는 뜻이다. */
		prop[2] = lower_32_bits(addr);
	}
}

/* [한국어]
 * of_pci_get_addr_flags - 커널 자원 플래그를 디바이스 트리의 공간 종류로 옮긴다
 *
 * @res: 원본 자원.
 * @flags: 결과를 담을 자리.
 * @return: 0 = 성공, -EINVAL = 옮길 수 없는 종류.
 *
 * 커널은 자원의 종류를 IORESOURCE_ 비트로 나타내고 디바이스 트리는 2비트
 * 공간 코드로 나타낸다. 이 함수가 그 사이의 번역이다.
 *
 * 검사 순서가 중요하다. 64비트 메모리를 32비트 메모리보다 **먼저** 본다.
 * 64비트 메모리 자원은 IORESOURCE_MEM 과 IORESOURCE_MEM_64 를 둘 다 갖고
 * 있어, 순서를 바꾸면 모두 32비트로 잘못 분류된다.
 *
 * prefetchable 은 공간 종류와 별개의 비트라 따로 얹는다.
 *
 * 셋 중 어느 것도 아니면 -EINVAL 이다. 버스 번호 자원 같은 것이 그에
 * 해당하며, 호출자들이 그 경우 그 자원을 건너뛴다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. 프로세스 컨텍스트.
 *
 * 에러 경로: 옮길 수 없는 종류는 -EINVAL.
 *
 * 호출 체인:
 *   of_pci_prop_ranges() / of_pci_is_range_resource() → [이 함수]
 */
static int of_pci_get_addr_flags(const struct resource *res, u32 *flags)
{
	u32 ss;

	if (res->flags & IORESOURCE_IO)
		/* [한국어] I/O 공간은 코드 1. */
		ss = OF_PCI_ADDR_SPACE_IO;
	/* [한국어] 64비트 메모리를 32비트보다 **먼저** 본다. */
	else if (res->flags & IORESOURCE_MEM_64)
		/* [한국어] 64비트 메모리는 코드 3. */
		ss = OF_PCI_ADDR_SPACE_MEM64;
	/* [한국어] 그 다음이 32비트 메모리다. 순서를 바꾸면 64비트 자원이 MEM 비트도
	 * 갖고 있어 모두 32비트로 잘못 분류된다. */
	else if (res->flags & IORESOURCE_MEM)
		ss = OF_PCI_ADDR_SPACE_MEM32;
	/* [한국어] 셋 중 어느 것도 아니면 — 버스 번호 자원 등이다. */
	else
		return -EINVAL;

	*flags = 0;
	if (res->flags & IORESOURCE_PREFETCH)
		*flags |= OF_PCI_ADDR_FIELD_PREFETCH;

	*flags |= FIELD_PREP(OF_PCI_ADDR_FIELD_SS, ss);

	return 0;
}

/* [한국어]
 * of_pci_prop_bus_range - 브리지의 bus-range 속성을 만든다
 *
 * @pdev: 브리지 장치.
 * @ocs: 변경 집합. 만든 속성이 여기 쌓인다.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * bus-range 는 이 브리지 아래에 어떤 버스 번호들이 있는지를 알려 준다.
 * 그 아래 장치를 디바이스 트리로 찾으려면 그 범위를 알아야 한다.
 *
 * 값은 커널이 이미 알고 있는 하위 버스 번호 자원에서 그대로 가져온다.
 * 열거가 끝난 뒤에 불리므로 그 값이 확정되어 있다.
 *
 * 변경 집합에 쌓는 방식이 이 파일 전체의 관용이다. 속성을 바로 붙이지 않고
 * 모아 두었다가 한꺼번에 적용하는데, 중간에 실패하면 통째로 되돌릴 수 있기
 * 때문이다.
 *
 * 실행 컨텍스트: 브리지의 디바이스 트리 노드 생성. 프로세스 컨텍스트.
 *
 * 에러 경로: 변경 집합 추가의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   of_pci_add_properties() → [이 함수]
 *     → of_changeset_add_prop_u32_array()
 */
static int of_pci_prop_bus_range(struct pci_dev *pdev,
				 struct of_changeset *ocs,
				 struct device_node *np)
{
	u32 bus_range[] = { pdev->subordinate->busn_res.start,
			    pdev->subordinate->busn_res.end };

	return of_changeset_add_prop_u32_array(ocs, np, "bus-range", bus_range,
					       /* [한국어] 두 값(시작, 끝)을 배열로 넘긴다. */
					       ARRAY_SIZE(bus_range));
}

/* [한국어]
 * of_pci_prop_ranges - 이 장치의 주소 창들을 ranges 속성으로 만든다
 *
 * @pdev: 대상 장치.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공, -ENOMEM 또는 변경 집합 오류.
 *
 * ranges 는 "자식 쪽 주소가 부모 쪽 주소로 어떻게 옮겨지는가" 를 적는 속성이다.
 *
 * 브리지와 일반 장치를 다르게 다루는 것이 이 함수의 핵심이다.
 * - 브리지는 창을 갖고 있고, 그 안에서는 자식 주소와 부모 주소가 같다.
 *   그래서 child_addr 를 parent_addr 의 사본으로 채운다.
 * - 일반 장치는 BAR 을 갖고 있으며, 자식 쪽 "주소" 가 BAR 번호다. 그래서
 *   child_addr 의 첫 셀에 번호만 넣는다.
 *
 * 크기가 0 인 자원은 건너뛴다. 배정되지 않았거나 존재하지 않는 BAR 이다.
 *
 * 인덱스 두 개(i, j)를 따로 두는 이유가 그 건너뜀 때문이다. j 는 자원 배열을
 * 훑고 i 는 실제로 담은 개수를 센다. 마지막에 i 로 크기를 계산하므로,
 * 건너뛴 자리가 빈 항목으로 남지 않는다.
 *
 * 주소를 pci_bus_address() 로 얻는 것이 중요하다. CPU 주소가 아니라 PCI
 * 버스 주소를 적어야 디바이스 트리를 읽는 쪽이 올바르게 해석한다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. 할당이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM. 그 밖은 변경 집합 오류를 올려보내며,
 * 어느 경로든 임시 배열을 해제한다.
 *
 * 호출 체인:
 *   of_pci_add_properties() → [이 함수]
 *     → of_pci_get_addr_flags() → pci_bus_address() → of_pci_set_address()
 */
static int of_pci_prop_ranges(struct pci_dev *pdev, struct of_changeset *ocs,
			      struct device_node *np)
{
	struct of_pci_range_entry *rp;
	struct resource *res;
	/* [한국어] i 는 실제로 담은 항목 수, j 는 자원 배열의 색인. 건너뛰는 자원이 있어 둘이 갈린다. */
	int i, j, ret;
	/* [한국어] flags 는 공간 종류, num 은 훑을 자원 개수. */
	u32 flags, num;
	/* [한국어] 64비트 값을 잠시 담는 자리. */
	u64 val64;

	if (pci_is_bridge(pdev)) {
		/* [한국어] 브리지는 창을 갖는다 — 개수가 하드웨어로 정해져 있다. */
		num = PCI_BRIDGE_RESOURCE_NUM;
		/* [한국어] 브리지 창은 자원 배열의 그 자리부터 시작한다. */
		res = &pdev->resource[PCI_BRIDGE_RESOURCES];
	/* [한국어] 일반 장치면, */
	} else {
		num = PCI_STD_NUM_BARS;
		/* [한국어] 표준 BAR 영역부터 시작한다. */
		res = &pdev->resource[PCI_STD_RESOURCES];
	}

	rp = kzalloc_objs(*rp, num);
	/* [한국어] 항목 배열을 잡지 못하면, */
	if (!rp)
		/* [한국어] 이 노드에 ranges 를 적을 수 없다. */
		return -ENOMEM;

	for (i = 0, j = 0; j < num; j++) {
		/* [한국어] 크기가 0 인 자원은 배정되지 않았거나 존재하지 않는 BAR 이다. */
		if (!resource_size(&res[j]))
			/* [한국어] 건너뛴다 — i 는 늘지 않으므로 빈 항목이 남지 않는다. */
			continue;

		if (of_pci_get_addr_flags(&res[j], &flags))
			/* [한국어] 옮길 수 없는 종류(버스 번호 등)도 건너뛴다. */
			continue;

		val64 = pci_bus_address(pdev, &res[j] - pdev->resource);
		/* [한국어] 부모 쪽 주소를 채운다. reg_num 을 0 으로, reloc 을 거짓으로 넘겨
		 * 실제 주소가 셀에 들어가게 한다. */
		of_pci_set_address(pdev, rp[i].parent_addr, val64, 0, flags,
				   /* [한국어] 재배치 불가 — 이 주소는 이미 확정됐다. */
				   false);
		if (pci_is_bridge(pdev)) {
			/* [한국어] 브리지는 자식 주소와 부모 주소가 같다. 창 안에서는 주소가 변환되지 않기 때문이다. */
			memcpy(rp[i].child_addr, rp[i].parent_addr,
			       /* [한국어] 3셀을 통째로 복사한다. */
			       sizeof(rp[i].child_addr));
		} else {
			/*
			 * For endpoint device, the lower 64-bits of child
			 * address is always zero.
			 */
			rp[i].child_addr[0] = j;
		}

		val64 = resource_size(&res[j]);
		/* [한국어] 크기의 상위 32비트, */
		rp[i].size[0] = upper_32_bits(val64);
		/* [한국어] 하위 32비트를 나눠 담는다. */
		rp[i].size[1] = lower_32_bits(val64);

		i++;
	/* [한국어] 담은 항목 수를 늘린다. 건너뛴 자원에서는 이 줄에 오지 않는다. */
	}

	ret = of_changeset_add_prop_u32_array(ocs, np, "ranges", (u32 *)rp,
					      /* [한국어] 실제로 담은 i 개만큼의 워드 수를 계산해 넘긴다. num 이 아니라 i 를 쓰는 것이
					       * 건너뛴 자리를 배제하는 지점이다. */
					      i * sizeof(*rp) / sizeof(u32));
	kfree(rp);

	return ret;
}

/* [한국어]
 * of_pci_prop_reg - 이 장치의 위치를 reg 속성으로 만든다
 *
 * @pdev: 대상 장치.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * reg 는 디바이스 트리에서 "이 노드가 무엇인가" 를 가리키는 가장 기본적인
 * 속성이다. PCI 에서는 그것이 버스·장치·기능 번호다.
 *
 * 주소와 크기를 0 으로 두고 reloc 을 참으로 넘기는 것이 요점이다. 그 조합이
 * "주소는 아직 정해지지 않았다" 를 뜻하며, 실제 BAR 주소는 위의 ranges 가
 * 따로 적는다. reg 는 위치만 알리면 된다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. 프로세스 컨텍스트.
 *
 * 에러 경로: 변경 집합 추가의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   of_pci_add_properties() → [이 함수]
 *     → of_pci_set_address(reloc=true) → of_changeset_add_prop_u32_array()
 */
static int of_pci_prop_reg(struct pci_dev *pdev, struct of_changeset *ocs,
			   struct device_node *np)
{
	struct of_pci_addr_pair reg = { 0 };

	/* configuration space */
	of_pci_set_address(pdev, reg.phys_addr, 0, 0, 0, true);

	return of_changeset_add_prop_u32_array(ocs, np, "reg", (u32 *)&reg,
					       /* [한국어] 구조체 전체를 워드 수로 환산해 넘긴다 — 주소 3셀 + 크기 2셀 = 5워드다. */
					       sizeof(reg) / sizeof(u32));
}

/* [한국어]
 * of_pci_prop_interrupts - 이 장치가 쓰는 INTx 핀을 interrupts 속성으로 만든다
 *
 * @pdev: 대상 장치.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공(또는 핀 없음), 또는 음수 오류.
 *
 * 장치의 Interrupt Pin 레지스터를 읽어 그대로 적는다. 1~4 가 INTA~INTD 이며,
 * 0 은 INTx 를 쓰지 않는다는 뜻이다.
 *
 * 핀이 0 이면 속성을 만들지 않고 성공으로 답한다. MSI 만 쓰는 요즘 장치가
 * 대부분 그렇고, 그 경우 interrupts 속성이 있으면 오히려 잘못된 정보가 된다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. config 읽기가 있어 프로세스
 * 컨텍스트가 적절하다.
 *
 * 에러 경로: config 읽기 실패와 변경 집합 오류를 각각 올려보낸다.
 *
 * 호출 체인:
 *   of_pci_add_properties() → [이 함수]
 *     → pci_read_config_byte() → of_changeset_add_prop_u32()
 */
static int of_pci_prop_interrupts(struct pci_dev *pdev,
				  struct of_changeset *ocs,
				  struct device_node *np)
{
	int ret;
	u8 pin;

	ret = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
	/* [한국어] config 읽기가 실패하면, */
	if (ret != 0)
		/* [한국어] 그 오류를 그대로 올려보낸다. */
		return ret;

	if (!pin)
		/* [한국어] 핀이 0 이면 INTx 를 쓰지 않는 장치다. 속성을 만들지 않는 것이 옳은데,
		 * 있으면 오히려 잘못된 정보가 된다. */
		return 0;

	return of_changeset_add_prop_u32(ocs, np, "interrupts", (u32)pin);
}

/* [한국어]
 * of_pci_prop_intr_ctrl - 일반 장치를 인터럽트 컨트롤러로 표시한다
 *
 * @pdev: 대상 장치.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공(또는 핀 없음), 또는 음수 오류.
 *
 * 일반 장치를 "인터럽트 컨트롤러" 로 적는 것이 처음에는 이상해 보인다.
 * 그 이유는 디바이스 트리의 인터럽트 모델 때문이다 — 인터럽트를 **내는**
 * 노드가 아니라 인터럽트를 **받아 전달하는** 노드가 컨트롤러이고, PCI
 * 장치는 자기 INTx 핀 넷을 상위로 전달하는 위치에 있다. 그래서 그 넷을
 * 번호로 지목할 수 있게 컨트롤러로 표시한다.
 *
 * #interrupt-cells 를 1 로 두는 것이 그 뜻이다 — 인터럽트 하나를 가리키는 데
 * 숫자 하나(핀 번호)면 충분하다.
 *
 * 핀이 없으면 아무것도 하지 않는다. 전달할 인터럽트가 없기 때문이다.
 *
 * 브리지에는 이 함수 대신 of_pci_prop_intr_map() 이 쓰인다. 브리지는 하위
 * 장치들의 핀을 회전시켜 전달하므로 단순한 표시로는 부족하다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. config 읽기가 있어 프로세스
 * 컨텍스트가 적절하다.
 *
 * 에러 경로: config 읽기 실패와 두 속성 추가의 오류를 각각 올려보낸다.
 *
 * 호출 체인:
 *   of_pci_add_properties() → [이 함수]
 *     → pci_read_config_byte() → of_changeset_add_prop_u32()
 *     → of_changeset_add_prop_bool()
 */
static int of_pci_prop_intr_ctrl(struct pci_dev *pdev, struct of_changeset *ocs,
				 struct device_node *np)
{
	int ret;
	u8 pin;

	ret = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
	/* [한국어] config 읽기가 실패하면, */
	if (ret != 0)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	if (!pin)
		/* [한국어] 전달할 인터럽트가 없으면 컨트롤러로 표시할 이유도 없다. */
		return 0;

	ret = of_changeset_add_prop_u32(ocs, np, "#interrupt-cells", 1);
	/* [한국어] 셀 수 속성 추가가 실패하면, */
	if (ret)
		/* [한국어] 컨트롤러 표시는 붙이지 않고 물러난다. 둘 중 하나만 있으면 트리가 모순된다. */
		return ret;

	return of_changeset_add_prop_bool(ocs, np, "interrupt-controller");
}

/* [한국어]
 * of_pci_prop_intr_map - 브리지 아래 장치들의 INTx 배선을 interrupt-map 으로 적는다
 *
 * @pdev: 브리지 장치.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공(또는 적을 것이 없음), -EINVAL / -ENOMEM.
 *
 * 이 파일에서 가장 복잡한 함수다. 브리지 아래 각 장치의 각 핀이 상위의
 * 어느 인터럽트로 이어지는지를 표로 적어야 하기 때문이다.
 *
 * 세 단계로 진행한다.
 * 1. 상위 노드에서 핀 넷 각각의 목적지를 알아낸다. of_irq_parse_raw() 가
 *    상위의 interrupt-map 을 따라가 "이 브리지의 INTA 는 어디로 가는가" 를
 *    풀어 준다. 실패한 핀은 노드를 NULL 로 두어 이후 단계에서 걸러진다.
 * 2. 필요한 표 크기를 센다. 항목 하나의 크기가 고정이 아니라 목적지
 *    컨트롤러가 요구하는 셀 수에 따라 달라, 실제로 세어 봐야 한다.
 * 3. 표를 채운다.
 *
 * pci_swizzle_interrupt_pin() 이 두 번 나오는 것이 이 함수의 핵심이다.
 * 브리지를 지날 때 핀이 슬롯 번호만큼 회전하므로, 하위 장치의 INTA 가
 * 브리지 쪽에서는 다른 핀일 수 있다. 그 회전을 반영해야 올바른 목적지에
 * 연결된다. 세는 루프와 채우는 루프가 **같은 계산** 을 해야 크기가 맞는다.
 *
 * int_map_mask 가 "이 표에서 무엇을 비교할지" 를 정한다. 0xffff00 은 주소
 * 셀에서 버스·장치 번호만 보고 기능 번호는 무시하라는 뜻이고, 마지막 7 은
 * 핀 번호 세 비트를 보라는 뜻이다. 기능 번호를 무시하는 이유는 다중 기능
 * 장치의 모든 기능이 같은 INTx 배선을 공유하기 때문이다.
 *
 * 주소 셀 자리를 건너뛰는 것에 주의할 만하다. 목적지 컨트롤러가 주소 셀을
 * 요구하면 그만큼 자리를 비워 두는데, 인터럽트 컨트롤러의 주소는 의미가
 * 없어 0 으로 남긴다.
 *
 * 실행 컨텍스트: 브리지의 디바이스 트리 노드 생성. 할당이 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 상위 노드를 못 찾으면 -EINVAL, 할당 실패는 -ENOMEM.
 * 속성 추가가 실패하면 goto 로 임시 표를 해제하고 오류를 올려보낸다.
 *
 * 호출 체인:
 *   of_pci_add_properties() → [이 함수]
 *     → of_irq_parse_raw() → pci_swizzle_interrupt_pin()
 *     → of_changeset_add_prop_u32_array()
 */
static int of_pci_prop_intr_map(struct pci_dev *pdev, struct of_changeset *ocs,
				struct device_node *np)
{
	u32 i, addr_sz[OF_PCI_MAX_INT_PIN] = { 0 }, map_sz = 0;
	struct of_phandle_args out_irq[OF_PCI_MAX_INT_PIN];
	/* [한국어] of_irq_parse_raw() 에 넘길 자식 주소. 빅엔디언인 것은 디바이스 트리의
	 * 모든 셀이 빅엔디언으로 저장되기 때문이다. */
	__be32 laddr[OF_PCI_ADDRESS_CELLS] = { 0 };
	/* [한국어] 표에서 무엇을 비교할지 정하는 마스크. 0xffff00 은 주소 첫 셀에서 버스·장치
	 * 번호만 보라는 뜻이고 — 기능 번호를 무시하는 것은 다중 기능 장치의 모든
	 * 기능이 같은 INTx 배선을 공유하기 때문이다 — 마지막 7 은 핀 번호 세 비트를 보라는 뜻이다. */
	u32 int_map_mask[] = { 0xffff00, 0, 0, 7 };
	/* [한국어] 상위 노드. 여기서부터 인터럽트 경로를 풀어 나간다. */
	struct device_node *pnode;
	/* [한국어] 하위 장치를 훑을 포인터. */
	struct pci_dev *child;
	/* [한국어] 만들 표와 그 안을 훑는 포인터. */
	u32 *int_map, *mapp;
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] 핀 번호(1~4). */
	u8 pin;

	pnode = pci_device_to_OF_node(pdev->bus->self);
	/* [한국어] 브리지에 트리 노드가 없으면, */
	if (!pnode)
		/* [한국어] 그 버스의 노드를 대신 쓴다. 루트 포트처럼 브리지 노드가 따로 없는 경우다. */
		pnode = pci_bus_to_OF_node(pdev->bus);

	if (!pnode) {
		/* [한국어] 어느 쪽으로도 상위 노드를 못 찾으면 인터럽트 경로를 풀 수 없다. */
		pci_err(pdev, "failed to get parent device node");
		/* [한국어] 잘못된 인자로 답한다. */
		return -EINVAL;
	}

	laddr[0] = cpu_to_be32((pdev->bus->number << 16) | (pdev->devfn << 8));
	/* [한국어] 핀 넷 각각의 목적지를 알아낸다. */
	for (pin = 1; pin <= OF_PCI_MAX_INT_PIN;  pin++) {
		/* [한국어] 배열 색인은 0 기준이라 하나를 뺀다. */
		i = pin - 1;
		/* [한국어] 출발점은 상위 노드다. */
		out_irq[i].np = pnode;
		/* [한국어] 인자 하나 — 핀 번호만 넘긴다. */
		out_irq[i].args_count = 1;
		/* [한국어] 그 핀 번호. */
		out_irq[i].args[0] = pin;
		/* [한국어] 상위의 interrupt-map 을 따라가 '이 브리지의 이 핀은 어디로 가는가' 를 푼다. */
		ret = of_irq_parse_raw(laddr, &out_irq[i]);
		/* [한국어] 풀지 못한 핀은, */
		if (ret) {
			/* [한국어] 노드를 NULL 로 두어 이후 단계에서 걸러지게 한다. */
			out_irq[i].np = NULL;
			/* [한국어] 디버그 기록만 남긴다 — 배선되지 않은 핀이 있는 것은 정상이다. */
			pci_dbg(pdev, "parse irq %d failed, ret %d", pin, ret);
			/* [한국어] 다음 핀으로 넘어간다. */
			continue;
		}
		of_property_read_u32(out_irq[i].np, "#address-cells",
				     /* [한국어] 목적지 컨트롤러가 요구하는 주소 셀 수를 읽어 둔다. 항목 크기가 그만큼 달라진다. */
				     &addr_sz[i]);
	}

	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) {
		/* [한국어] 각 장치의 네 핀을 본다. */
		for (pin = 1; pin <= OF_PCI_MAX_INT_PIN; pin++) {
			/* [한국어] 브리지를 지나며 핀이 회전한다. 하위 장치의 INTA 가 브리지 쪽에서는
			 * 다른 핀일 수 있어, 그 회전을 반영해야 올바른 목적지에 연결된다. */
			i = pci_swizzle_interrupt_pin(child, pin) - 1;
			/* [한국어] 그 핀이 배선되지 않았으면 표에 넣을 것이 없다. */
			if (!out_irq[i].np)
				continue;
			map_sz += 5 + addr_sz[i] + out_irq[i].args_count;
		/* [한국어] 이 장치의 네 핀을 다 셌다. */
		}
	}

	/*
	 * Parsing interrupt failed for all pins. In this case, it does not
	 * need to generate interrupt-map property.
	 */
	if (!map_sz)
		return 0;

	int_map = kcalloc(map_sz, sizeof(u32), GFP_KERNEL);
	/* [한국어] 표를 잡지 못하면, */
	if (!int_map)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;
	mapp = int_map;

	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) {
		/* [한국어] 세는 루프와 **같은 순서·같은 조건** 으로 다시 돈다. 계산이 어긋나면
		 * 표 크기가 맞지 않아 넘쳐 쓰게 된다. */
		for (pin = 1; pin <= OF_PCI_MAX_INT_PIN; pin++) {
			/* [한국어] 여기서도 같은 회전 계산을 한다. */
			i = pci_swizzle_interrupt_pin(child, pin) - 1;
			/* [한국어] 배선되지 않은 핀은 세는 루프에서도 건너뛰었다. */
			if (!out_irq[i].np)
				continue;

			*mapp = (child->bus->number << 16) |
				(child->devfn << 8);
			mapp += OF_PCI_ADDRESS_CELLS;
			*mapp = pin;
			mapp++;
			*mapp = out_irq[i].np->phandle;
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
			mapp += addr_sz[i];
			memcpy(mapp, out_irq[i].args,
			       /* [한국어] 목적지 컨트롤러가 요구하는 인자들을 그대로 복사한다. */
			       out_irq[i].args_count * sizeof(u32));
			mapp += out_irq[i].args_count;
		/* [한국어] 이 장치의 네 핀 처리 끝. */
		}
	}

	ret = of_changeset_add_prop_u32_array(ocs, np, "interrupt-map", int_map,
					      /* [한국어] 센 크기만큼을 그대로 넘긴다. */
					      map_sz);
	if (ret)
		/* [한국어] 실패하면 임시 표를 해제하는 자리로 뛴다. */
		goto failed;

	ret = of_changeset_add_prop_u32(ocs, np, "#interrupt-cells", 1);
	/* [한국어] 셀 수 속성 추가가 실패하면, */
	if (ret)
		/* [한국어] 같은 정리 경로로 간다. */
		goto failed;

	ret = of_changeset_add_prop_u32_array(ocs, np, "interrupt-map-mask",
					      /* [한국어] 위에서 만든 마스크를 넘긴다. */
					      int_map_mask,
					      ARRAY_SIZE(int_map_mask));
	if (ret)
		/* [한국어] 여기서 실패해도 같은 정리를 거친다. */
		goto failed;

	kfree(int_map);
	return 0;

failed:
	kfree(int_map);
	return ret;
}

/* [한국어]
 * of_pci_prop_compatible - 이 장치의 compatible 문자열들을 만든다
 *
 * @pdev: 대상 장치.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * compatible 은 디바이스 트리에서 드라이버를 고르는 열쇠다. 세 문자열을
 * **구체적인 것부터** 늘어놓는데, 그것이 이 속성의 규약이다 — 읽는 쪽이
 * 앞에서부터 맞는 드라이버를 찾으므로, 정확히 이 장치를 아는 드라이버가
 * 먼저 잡힌다.
 *
 * 세 단계의 구체성이다.
 * 1. 벤더·장치 ID — 정확히 이 모델.
 * 2. 클래스 전체(class, subclass, prog-if) — 같은 종류의 장치.
 * 3. 클래스 상위 둘(class, subclass) — 더 넓은 종류.
 *
 * 예를 들어 NVMe 컨트롤러라면 3번이 "대량 저장 장치의 NVM 하위 종류" 가
 * 되어, 벤더를 모르는 범용 드라이버가 그것으로 잡을 수 있다.
 *
 * kasprintf 가 실패하면 그 자리가 NULL 로 남는데, 그것을 따로 검사하지
 * 않는다 — 아래 속성 추가 함수가 NULL 항목을 다루도록 되어 있다는 전제다.
 *
 * 성공하든 실패하든 세 문자열을 모두 해제한다. 속성 추가가 값을 복사해
 * 가므로 원본을 들고 있을 이유가 없다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. 할당이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 변경 집합 추가의 오류를 올려보내며, 어느 경우든 문자열을 해제한다.
 *
 * 호출 체인:
 *   of_pci_add_properties() → [이 함수]
 *     → kasprintf() → of_changeset_add_prop_string_array() → kfree()
 */
static int of_pci_prop_compatible(struct pci_dev *pdev,
				  struct of_changeset *ocs,
				  struct device_node *np)
{
	const char *compat_strs[PROP_COMPAT_NUM] = { 0 };
	int i, ret;

	compat_strs[PROP_COMPAT_PCI_VVVV_DDDD] =
		/* [한국어] 가장 구체적인 문자열 — 정확히 이 모델을 아는 드라이버가 이것으로 잡힌다. */
		kasprintf(GFP_KERNEL, "pci%x,%x", pdev->vendor, pdev->device);
	compat_strs[PROP_COMPAT_PCICLASS_CCSSPP] =
		/* [한국어] 클래스 전체(class, subclass, prog-if) — 같은 종류의 장치. */
		kasprintf(GFP_KERNEL, "pciclass,%06x", pdev->class);
	compat_strs[PROP_COMPAT_PCICLASS_CCSS] =
		/* [한국어] 클래스 상위 둘만 — 더 넓은 종류다. 8비트를 미는 것이 prog-if 를 떨어내는 계산이다. */
		kasprintf(GFP_KERNEL, "pciclass,%04x", pdev->class >> 8);

	ret = of_changeset_add_prop_string_array(ocs, np, "compatible",
						 /* [한국어] 세 문자열을 구체적인 것부터 늘어놓는다. 읽는 쪽이 앞에서부터 맞는
						  * 드라이버를 찾으므로 이 순서가 곧 우선순위가 된다. */
						 compat_strs, PROP_COMPAT_NUM);
	for (i = 0; i < PROP_COMPAT_NUM; i++)
		/* [한국어] 속성 추가가 값을 복사해 갔으므로 원본은 놓는다. 성공·실패 모두 해제한다. */
		kfree(compat_strs[i]);

	return ret;
}

/* [한국어]
 * of_pci_add_properties - 장치 하나의 디바이스 트리 노드에 필요한 속성을 모두 붙인다
 *
 * @pdev: 대상 장치.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공, 또는 첫 실패의 오류.
 *
 * 이 파일의 진입점이다. PCI 로 발견한 장치를 디바이스 트리에 나타내야 할 때
 * — 오버레이나 가상화에서 그 트리를 게스트에 넘겨야 할 때 — 불린다.
 *
 * 브리지와 일반 장치의 갈림이 앞부분에 있다.
 * - 브리지는 device_type 이 "pci" 이고, bus-range 와 interrupt-map 을 갖는다.
 *   그 셋이 "이 아래에 또 버스가 있다" 를 알린다.
 * - 일반 장치는 인터럽트 컨트롤러 표시만 갖는다.
 *
 * 그 뒤는 공통이다 — ranges, 주소·크기 셀 수, reg, compatible, interrupts.
 *
 * #address-cells 3 과 #size-cells 2 가 PCI 의 고정 규약이다. 파일 앞머리의
 * 매크로가 그 값을 정의하고, 이 자리와 호스트 브리지 쪽이 함께 쓴다.
 *
 * 한 단계라도 실패하면 즉시 물러난다. 되감기가 없는 이유는 변경 집합이
 * 그 역할을 하기 때문이다 — 호출자가 집합을 적용하지 않고 버리면 지금까지
 * 쌓은 것이 모두 사라진다.
 *
 * 실행 컨텍스트: 디바이스 트리 노드 생성. 프로세스 컨텍스트.
 *
 * 에러 경로: 첫 실패의 오류를 그대로 올려보내며, 되감기는 호출자가 변경
 * 집합을 버리는 것으로 이뤄진다.
 *
 * 호출 체인:
 *   of.c 의 of_pci_make_dev_node() → [이 함수]
 *     → of_pci_prop_bus_range() → of_pci_prop_intr_map()
 *     → of_pci_prop_ranges() → of_pci_prop_reg()
 *     → of_pci_prop_compatible() → of_pci_prop_interrupts()
 */
int of_pci_add_properties(struct pci_dev *pdev, struct of_changeset *ocs,
			  struct device_node *np)
{
	int ret;

	/*
	 * The added properties will be released when the
	 * changeset is destroyed.
	 */
	if (pci_is_bridge(pdev)) {
		ret = of_changeset_add_prop_string(ocs, np, "device_type",
						   /* [한국어] 브리지임을 알리는 표준 값이다. 이 값이 있어야 트리를 읽는 쪽이
						    * 그 아래에 또 버스가 있다고 판단한다. */
						   "pci");
		if (ret)
			/* [한국어] 실패하면 그대로 물러난다. 되감기가 없는 것은 변경 집합이 그 역할을 하기 때문으로,
			 * 호출자가 집합을 버리면 지금까지 쌓은 것이 모두 사라진다. */
			return ret;

		ret = of_pci_prop_bus_range(pdev, ocs, np);
		/* [한국어] bus-range 추가가 실패하면, */
		if (ret)
			/* [한국어] 물러난다. */
			return ret;

		ret = of_pci_prop_intr_map(pdev, ocs, np);
		/* [한국어] interrupt-map 추가가 실패하면, */
		if (ret)
			/* [한국어] 물러난다. */
			return ret;
	} else {
		ret = of_pci_prop_intr_ctrl(pdev, ocs, np);
		/* [한국어] 일반 장치의 인터럽트 컨트롤러 표시가 실패하면, */
		if (ret)
			/* [한국어] 물러난다. */
			return ret;
	}

	ret = of_pci_prop_ranges(pdev, ocs, np);
	/* [한국어] ranges 추가가 실패하면, */
	if (ret)
		/* [한국어] 물러난다. */
		return ret;

	ret = of_changeset_add_prop_u32(ocs, np, "#address-cells",
					/* [한국어] PCI 주소는 언제나 3셀이다. */
					OF_PCI_ADDRESS_CELLS);
	if (ret)
		/* [한국어] 실패하면 물러난다. */
		return ret;

	ret = of_changeset_add_prop_u32(ocs, np, "#size-cells",
					/* [한국어] 크기는 언제나 2셀이다. */
					OF_PCI_SIZE_CELLS);
	if (ret)
		/* [한국어] 실패하면 물러난다. */
		return ret;

	ret = of_pci_prop_reg(pdev, ocs, np);
	/* [한국어] reg 추가가 실패하면, */
	if (ret)
		/* [한국어] 물러난다. */
		return ret;

	ret = of_pci_prop_compatible(pdev, ocs, np);
	/* [한국어] compatible 추가가 실패하면, */
	if (ret)
		/* [한국어] 물러난다. */
		return ret;

	ret = of_pci_prop_interrupts(pdev, ocs, np);
	/* [한국어] interrupts 추가가 실패하면, */
	if (ret)
		/* [한국어] 물러난다. */
		return ret;

	return 0;
}

/* [한국어]
 * of_pci_is_range_resource - 이 자원을 호스트 브리지 ranges 에 넣어야 하는지 판단한다
 *
 * @res: 검사할 자원.
 * @flags: 넣어야 한다면 그 공간 플래그를 담을 자리.
 * @return: true = 넣는다, false = 건너뛴다.
 *
 * 호스트 브리지의 창 목록에는 메모리 창뿐 아니라 I/O 창과 버스 번호 자원도
 * 섞여 있다. 이 함수가 메모리만 골라낸다.
 *
 * I/O 를 빼는 이유는 이 트리에서 확인할 수 없다 — 상류 코드가 메모리만
 * 다루도록 되어 있다는 사실만 읽을 수 있다.
 *
 * 플래그 변환까지 겸하는 것이 이 함수의 편의다. 판단과 변환이 같은 조건에
 * 달려 있어, 나누면 호출부가 두 번 검사하게 된다.
 *
 * resource_type() 으로 비교하는 것에 주의할 만하다. 그 매크로가 종류 비트만
 * 남기므로, IORESOURCE_MEM_64 와의 비교는 그 비트가 종류 마스크 안에 있을
 * 때만 뜻을 갖는다.
 *
 * 실행 컨텍스트: 호스트 브리지 노드 생성. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 판단이 서지 않는 모든 경우가 false 다.
 *
 * 호출 체인:
 *   of_pci_host_bridge_prop_ranges() → [이 함수] → of_pci_get_addr_flags()
 */
static bool of_pci_is_range_resource(const struct resource *res, u32 *flags)
{
	if (!(resource_type(res) & IORESOURCE_MEM) &&
	    !(resource_type(res) & IORESOURCE_MEM_64))
		return false;

	if (of_pci_get_addr_flags(res, flags))
		/* [한국어] 메모리도 64비트 메모리도 아니면 호스트 브리지 ranges 의 대상이 아니다. */
		return false;

	return true;
}

/* [한국어]
 * of_pci_host_bridge_prop_ranges - 호스트 브리지의 창들을 ranges 속성으로 만든다
 *
 * @bridge: 대상 호스트 브리지.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공(또는 적을 창이 없음), -EINVAL / -ENOMEM.
 *
 * of_pci_prop_ranges() 의 호스트 브리지 판이며, 한 가지가 근본적으로 다르다 —
 * **부모 쪽 주소 셀 수가 고정이 아니다**.
 *
 * 일반 장치의 ranges 는 양쪽이 다 PCI 주소라 3셀씩이지만, 호스트 브리지의
 * 부모는 CPU 버스라 그쪽 주소 셀 수를 트리에서 읽어야 한다. 1 이면 32비트
 * 주소, 2 면 64비트 주소다.
 *
 * 그래서 항목 크기가 n_addr_cells 에 따라 달라지고, 세는 루프와 채우는 루프를
 * 따로 두어 필요한 크기를 먼저 구한다.
 *
 * 오프셋을 빼는 것이 이 함수의 요점이다. 자식 쪽(PCI 주소)에는 CPU 주소에서
 * 창의 오프셋을 뺀 값을 적고, 부모 쪽에는 CPU 주소를 그대로 적는다. 그
 * 한 쌍이 곧 "이 PCI 주소는 저 CPU 주소다" 를 뜻한다.
 *
 * 주소 셀 수가 1 이면 상위 32비트를 아예 쓰지 않는다. 그 시스템에서는 4GB
 * 위 주소를 표현할 수 없기 때문이다.
 *
 * pdev 에 NULL 을 넘기는 것도 눈에 띈다 — 호스트 브리지 창은 특정 장치에
 * 속하지 않아 위치 필드가 의미가 없다.
 *
 * 실행 컨텍스트: 호스트 브리지 노드 생성. 할당이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 주소 셀 수가 범위 밖이면 -EINVAL, 할당 실패는 -ENOMEM.
 * 적을 창이 없으면 속성을 만들지 않고 성공으로 답한다.
 *
 * 호출 체인:
 *   of_pci_add_host_bridge_properties() → [이 함수]
 *     → of_n_addr_cells() → of_pci_is_range_resource() → of_pci_set_address()
 */
static int of_pci_host_bridge_prop_ranges(struct pci_host_bridge *bridge,
					  struct of_changeset *ocs,
					  struct device_node *np)
{
	struct resource_entry *window;
	unsigned int ranges_sz = 0;
	/* [한국어] 적을 창의 개수. 아래 첫 루프가 센다. */
	unsigned int n_range = 0;
	/* [한국어] 순회 중의 자원. */
	struct resource *res;
	/* [한국어] 부모 쪽 주소 셀 수. 이 값이 항목 크기를 좌우한다. */
	int n_addr_cells;
	/* [한국어] 만들 표. */
	u32 *ranges;
	/* [한국어] 64비트 값을 잠시 담는 자리. */
	u64 val64;
	/* [한국어] 공간 종류 플래그. */
	u32 flags;
	/* [한국어] 속성 추가의 결과. */
	int ret;

	n_addr_cells = of_n_addr_cells(np);
	/* [한국어] 1(32비트)이나 2(64비트)가 아니면 다룰 수 없는 트리다. */
	if (n_addr_cells <= 0 || n_addr_cells > 2)
		/* [한국어] 잘못된 인자로 답한다. */
		return -EINVAL;

	resource_list_for_each_entry(window, &bridge->windows) {
		/* [한국어] 이 창의 자원. */
		res = window->res;
		/* [한국어] 메모리 창이 아니면, */
		if (!of_pci_is_range_resource(res, &flags))
			/* [한국어] 세지 않는다. */
			continue;
		n_range++;
	/* [한국어] 개수 세기 끝. */
	}

	if (!n_range)
		/* [한국어] 적을 창이 하나도 없으면 속성을 만들지 않고 성공으로 답한다. */
		return 0;

	ranges = kcalloc(n_range,
			 /* [한국어] 항목 하나의 워드 수 — 자식 주소 3셀 + 크기 2셀 + 부모 주소 n_addr_cells 셀.
			  * 부모 쪽만 가변인 것이 일반 장치의 ranges 와 다른 점이다. */
			 (OF_PCI_ADDRESS_CELLS + OF_PCI_SIZE_CELLS +
			  n_addr_cells) * sizeof(*ranges),
			 GFP_KERNEL);
	if (!ranges)
		/* [한국어] 표를 잡지 못하면 메모리 부족으로 물러난다. */
		return -ENOMEM;

	resource_list_for_each_entry(window, &bridge->windows) {
		/* [한국어] 세는 루프와 같은 순서로 다시 돈다. */
		res = window->res;
		/* [한국어] 같은 조건으로 거른다. */
		if (!of_pci_is_range_resource(res, &flags))
			/* [한국어] 메모리 창이 아니면 건너뛴다. */
			continue;

		/* PCI bus address */
		val64 = res->start;
		of_pci_set_address(NULL, &ranges[ranges_sz],
				   /* [한국어] 자식(PCI) 주소에는 CPU 주소에서 창의 오프셋을 뺀 값을 적는다.
				    * 그 한 쌍이 곧 '이 PCI 주소는 저 CPU 주소다' 를 뜻한다. */
				   val64 - window->offset, 0, flags, false);
		ranges_sz += OF_PCI_ADDRESS_CELLS;

		/* Host bus address */
		if (n_addr_cells == 2)
			ranges[ranges_sz++] = upper_32_bits(val64);
		/* [한국어] 부모 주소의 하위 32비트. 셀 수가 1 이면 이것만 적히므로,
		 * 그 시스템에서는 4GB 위 주소를 표현할 수 없다. */
		ranges[ranges_sz++] = lower_32_bits(val64);

		/* Size */
		val64 = resource_size(res);
		ranges[ranges_sz] = upper_32_bits(val64);
		/* [한국어] 크기의 하위 32비트. */
		ranges[ranges_sz + 1] = lower_32_bits(val64);
		/* [한국어] 크기 셀 수만큼 전진한다. */
		ranges_sz += OF_PCI_SIZE_CELLS;
	/* [한국어] 이 창 처리 끝. */
	}

	ret = of_changeset_add_prop_u32_array(ocs, np, "ranges", ranges,
					      /* [한국어] 실제로 채운 워드 수만큼 넘긴다. */
					      ranges_sz);
	kfree(ranges);
	return ret;
}

/* [한국어]
 * of_pci_add_host_bridge_properties - 호스트 브리지 노드에 필요한 속성을 모두 붙인다
 *
 * @bridge: 대상 호스트 브리지.
 * @ocs: 변경 집합.
 * @np: 속성을 붙일 노드.
 * @return: 0 = 성공, 또는 첫 실패의 오류.
 *
 * of_pci_add_properties() 의 호스트 브리지 판이며 훨씬 짧다.
 *
 * 붙이는 것이 넷뿐이다 — device_type, 주소·크기 셀 수, 그리고 ranges.
 * 호스트 브리지에는 reg 도 compatible 도 interrupts 도 붙이지 않는데,
 * 그 노드는 이미 디바이스 트리에 존재하고 그 속성들이 이미 적혀 있기
 * 때문이다. 이 함수는 커널이 알아낸 정보만 채워 넣는다.
 *
 * 셀 수를 여기서도 3 과 2 로 두는 것이 중요하다. 이 노드의 **자식** 이
 * PCI 주소를 쓰므로, 부모 쪽 셀 수가 무엇이든 자식 주소는 PCI 형식이다.
 *
 * 실행 컨텍스트: 호스트 브리지 노드 생성. 프로세스 컨텍스트.
 *
 * 에러 경로: 첫 실패의 오류를 올려보내며, 되감기는 호출자가 변경 집합을
 * 버리는 것으로 이뤄진다.
 *
 * 호출 체인:
 *   of.c 의 호스트 브리지 노드 생성 → [이 함수]
 *     → of_changeset_add_prop_string() → of_changeset_add_prop_u32()
 *     → of_pci_host_bridge_prop_ranges()
 */
int of_pci_add_host_bridge_properties(struct pci_host_bridge *bridge,
				      struct of_changeset *ocs,
				      struct device_node *np)
{
	int ret;

	ret = of_changeset_add_prop_string(ocs, np, "device_type", "pci");
	/* [한국어] device_type 추가가 실패하면, */
	if (ret)
		/* [한국어] 물러난다. */
		return ret;

	ret = of_changeset_add_prop_u32(ocs, np, "#address-cells",
					/* [한국어] 이 노드의 **자식** 이 PCI 주소를 쓰므로 3셀이다. 부모 쪽 셀 수가
					 * 무엇이든 이 값은 달라지지 않는다. */
					OF_PCI_ADDRESS_CELLS);
	if (ret)
		/* [한국어] 실패하면 물러난다. */
		return ret;

	ret = of_changeset_add_prop_u32(ocs, np, "#size-cells",
					/* [한국어] 자식 쪽 크기는 2셀이다. */
					OF_PCI_SIZE_CELLS);
	if (ret)
		/* [한국어] 실패하면 물러난다. */
		return ret;

	ret = of_pci_host_bridge_prop_ranges(bridge, ocs, np);
	/* [한국어] ranges 추가가 실패하면, */
	if (ret)
		/* [한국어] 물러난다. */
		return ret;

	return 0;
}
