// SPDX-License-Identifier: GPL-2.0
/*
 * PCI detection and setup code
 */

#include <linux/array_size.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/msi.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pci_hotplug.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/aer.h>
#include <linux/acpi.h>
#include <linux/hypervisor.h>
#include <linux/irqdomain.h>
#include <linux/pm_runtime.h>
#include <linux/bitfield.h>
#include <trace/events/pci.h>
#include "pci.h"

/* NVMe: 변수 선언/초기화 */
static struct resource busn_resource = {
	/* NVMe: 구조체 필드에 값 저장: .name */
	.name	= "PCI busn",
	/* NVMe: 구조체 필드에 값 저장: .start */
	.start	= 0,
	/* NVMe: 구조체 필드에 값 저장: .end */
	.end	= 255,
	/* NVMe: 구조체 필드에 값 저장: .flags */
	.flags	= IORESOURCE_BUS,
};

/* Ugh.  Need to stop exporting this to modules. */
/* NVMe: 함수 호출: LIST_HEAD */
LIST_HEAD(pci_root_buses);
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_root_buses);

/* NVMe: 함수 정의: LIST_HEAD */
static LIST_HEAD(pci_domain_busn_res_list);

/* NVMe: 변수 선언/초기화 */
struct pci_domain_busn_res {
	/* NVMe: 변수 선언/초기화 */
	struct list_head list;
	/* NVMe: 변수 선언/초기화 */
	struct resource res;
	/* NVMe: 변수 선언/초기화 */
	int domain_nr;
};

/* NVMe: 함수 호출: get_pci_domain_busn_res */
static struct resource *get_pci_domain_busn_res(int domain_nr)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_domain_busn_res *r;

	/* NVMe: 리스트 순회 */
	list_for_each_entry(r, &pci_domain_busn_res_list, list)
		/* NVMe: 조걸 분기 */
		if (r->domain_nr == domain_nr)
			/* NVMe: 결과 반환: &r->res */
			return &r->res;

	/* NVMe: 객체 메모리 할당 및 0 초기화 */
	r = kzalloc_obj(*r);
	/* NVMe: 조걸 분기 */
	if (!r)
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	/* NVMe: 구조체 필드에 값 저장: r->domain_nr */
	r->domain_nr = domain_nr;
	/* NVMe: 구조체 필드에 값 저장: r->res.start */
	r->res.start = 0;
	/* NVMe: 구조체 필드에 값 저장: r->res.end */
	r->res.end = 0xff;
	/* NVMe: 구조체 필드에 비트 마스크 적용: r->res.flags */
	r->res.flags = IORESOURCE_BUS | IORESOURCE_PCI_FIXED;

	/* NVMe: 리스트 뒤에 추가 */
	list_add_tail(&r->list, &pci_domain_busn_res_list);

	/* NVMe: 결과 반환: &r->res */
	return &r->res;
}

/*
 * PCI Bus Class
 */
/* NVMe: 함수 정의: release_pcibus_dev */
static void release_pcibus_dev(struct device *dev)
{
	/* NVMe: device를 pci_bus로 변환 */
	struct pci_bus *pci_bus = to_pci_bus(dev);

	/* NVMe: 장치 레퍼런스 감소 */
	put_device(pci_bus->bridge);
	/* NVMe: 버스 리소스 제거 */
	pci_bus_remove_resources(pci_bus);
	/* NVMe: 버스 OF node 해제 */
	pci_release_bus_of_node(pci_bus);
	/* NVMe: 메모리 해제 */
	kfree(pci_bus);
}

/* NVMe: 변수 선언/초기화 */
static const struct class pcibus_class = {
	/* NVMe: 구조체 필드에 값 저장: .name */
	.name		= "pci_bus",
	/* NVMe: 구조체 필드에 비트 마스크 적용: .dev_release */
	.dev_release	= &release_pcibus_dev,
	/* NVMe: 구조체 필드에 값 저장: .dev_groups */
	.dev_groups	= pcibus_groups,
};

/* NVMe: 함수 호출: pcibus_class_init */
static int __init pcibus_class_init(void)
{
	/* NVMe: 결과 반환: 장치 클래스 등록 */
	return class_register(&pcibus_class);
}
/* NVMe: 함수 호출: postcore_initcall */
postcore_initcall(pcibus_class_init);

/* NVMe: 함수 정의: pci_size */
static u64 pci_size(u64 base, u64 maxbase, u64 mask)
{
	u64 size = mask & maxbase;	/* Find the significant bits */
	/* NVMe: 조걸 분기 */
	if (!size)
		/* NVMe: 성공 반환 */
		return 0;

	/*
	 * Get the lowest of them to find the decode size, and from that
	 * the extent.
	 */
	/* NVMe: 비트 연산으로 값 설정/마스크: size */
	size = size & ~(size-1);

	/*
	 * base == maxbase can be valid only if the BAR has already been
	 * programmed with all 1s.
	 */
	/* NVMe: 조걸 분기 */
	if (base == maxbase && ((base | (size - 1)) & mask) != mask)
		/* NVMe: 성공 반환 */
		return 0;

	/* NVMe: 결과 반환: size */
	return size;
}

/* NVMe: BAR 속성 디코딩 */
static inline unsigned long decode_bar(struct pci_dev *dev, u32 bar)
{
	/* NVMe: 변수 선언/초기화 */
	u32 mem_type;
	/* NVMe: 변수 선언/초기화 */
	unsigned long flags;

	/* NVMe: 조걸 분기 */
	if ((bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
		/* NVMe: 비트 연산으로 값 설정/마스크: flags */
		flags = bar & ~PCI_BASE_ADDRESS_IO_MASK;
		/* NVMe: 변수에 값 할당: flags | */
		flags |= IORESOURCE_IO;
		/* NVMe: 결과 반환: flags */
		return flags;
	}

	/* NVMe: 비트 연산으로 값 설정/마스크: flags */
	flags = bar & ~PCI_BASE_ADDRESS_MEM_MASK;
	/* NVMe: 변수에 값 할당: flags | */
	flags |= IORESOURCE_MEM;
	/* NVMe: 조걸 분기 */
	if (flags & PCI_BASE_ADDRESS_MEM_PREFETCH)
		/* NVMe: 변수에 값 할당: flags | */
		flags |= IORESOURCE_PREFETCH;

	/* NVMe: 비트 연산으로 값 설정/마스크: mem_type */
	mem_type = bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK;
	/* NVMe: 다중 분기 선택 */
	switch (mem_type) {
	/* NVMe: case 분기: PCI_BASE_ADDRESS_MEM_TYPE_32 */
	case PCI_BASE_ADDRESS_MEM_TYPE_32:
		break;
	/* NVMe: case 분기: PCI_BASE_ADDRESS_MEM_TYPE_1M */
	case PCI_BASE_ADDRESS_MEM_TYPE_1M:
		/* 1M mem BAR treated as 32-bit BAR */
		break;
	/* NVMe: case 분기: PCI_BASE_ADDRESS_MEM_TYPE_64 */
	case PCI_BASE_ADDRESS_MEM_TYPE_64:
		/* NVMe: 변수에 값 할당: flags | */
		flags |= IORESOURCE_MEM_64;
		break;
	default:
		/* mem unknown type treated as 32-bit BAR */
		break;
	}
	/* NVMe: 결과 반환: flags */
	return flags;
}

/* NVMe: 매크로 정의: PCI_COMMAND_DECODE_ENABLE */
#define PCI_COMMAND_DECODE_ENABLE	(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)

/**
 * __pci_size_bars - Read the raw BAR mask for a range of PCI BARs
 * @dev: the PCI device
 * @count: number of BARs to size
 * @pos: starting config space position
 * @sizes: array to store mask values
 * @rom: indicate whether to use ROM mask, which avoids enabling ROM BARs
 *
 * Provided @sizes array must be sufficiently sized to store results for
 * @count u32 BARs.  Caller is responsible for disabling decode to specified
 * BAR range around calling this function.  This function is intended to avoid
 * disabling decode around sizing each BAR individually, which can result in
 * non-trivial overhead in virtualized environments with very large PCI BARs.
 */
/*
 * __pci_size_bars() - 여러 BAR에 all-1s를 써서 decode mask를 한꺼번에 읽음
 *
 * NVMe 연결: BAR0(controller registers)와 64-bit BAR(doorbell 배열)의
 * 실제 decode 크기를 산정하기 위해 사용. all-1s 쓰기 후 읽은 값의 하위
 * 비트가 0인 부분이 디코딩되지 않는 영역을 나타낸다. decode가 켜진
 * 상태에서 수행하면 잘못된 값을 얻을 수 있으므로 호출자가 COMMAND
 * decode를 끄고 호출해야 한다.
 */
/* NVMe: BAR 크기 마스크 측정 */
static void __pci_size_bars(struct pci_dev *dev, int count,
			    /* NVMe: 코드 동작 수행 */
			    unsigned int pos, u32 *sizes, bool rom)
{
	u32 orig, mask = rom ? PCI_ROM_ADDRESS_MASK : ~0; /* NVMe: ROM BAR이면 ROM 주소 마스크, 일반 BAR이면 전체 1로 설정 */
	/* NVMe: 변수 선언/초기화 */
	int i;

	for (i = 0; i < count; i++, pos += 4, sizes++) { /* NVMe: BAR0부터 4바이트씩 이동하며 count개 BAR 크기 산정 */
		pci_read_config_dword(dev, pos, &orig); /* NVMe: 현재 BAR의 원래 값을 보존 */
		pci_write_config_dword(dev, pos, mask); /* NVMe: all-1s를 써서 decode mask를 노출 */
		pci_read_config_dword(dev, pos, sizes); /* NVMe: 장치가 decode하지 않는 비트가 0인 mask 획득 */
		pci_write_config_dword(dev, pos, orig); /* NVMe: 원래 BAR 값 복원 */
	}
}

/*
 * __pci_size_stdbars() - 표준 BAR(0~5) 크기 산정용 래퍼
 *
 * NVMe 연결: NVMe BAR0/64-bit BAR의 크기를 산정할 때 호출.
 */
/* NVMe: 표준 BAR 크기 측정 */
void __pci_size_stdbars(struct pci_dev *dev, int count,
			/* NVMe: 코드 동작 수행 */
			unsigned int pos, u32 *sizes)
{
	__pci_size_bars(dev, count, pos, sizes, false); /* NVMe: ROM BAR이 아닌 일반 BAR mask 읽기 */
}

/*
 * __pci_size_rom() - 옵션 ROM BAR 크기 산정용 래퍼
 *
 * NVMe 연결: NVMe 컨트롤러는 일반적으로 ROM BAR이 없지만, BIOS/Option ROM
 * 기반 초기화를 가진 장치도 있을 수 있다.
 */
/* NVMe: 함수 정의: __pci_size_rom */
static void __pci_size_rom(struct pci_dev *dev, unsigned int pos, u32 *sizes)
{
	__pci_size_bars(dev, 1, pos, sizes, true); /* NVMe: ROM BAR용 mask로 1개 BAR 크기 산정 */
}

/**
 * __pci_read_base - Read a PCI BAR
 * @dev: the PCI device
 * @type: type of the BAR
 * @res: resource buffer to be filled in
 * @pos: BAR position in the config space
 * @sizes: array of one or more pre-read BAR masks
 *
 * Returns 1 if the BAR is 64-bit, or 0 if 32-bit.
 */
/* NVMe: 개별 BAR 읽기 */
int __pci_read_base(struct pci_dev *dev, enum pci_bar_type type,
		    /* NVMe: 코드 동작 수행 */
		    struct resource *res, unsigned int pos, u32 *sizes)
{
	/* NVMe: 변수에 값 할당: u32 l */
	u32 l = 0, sz;
	/* NVMe: 변수 선언/초기화 */
	u64 l64, sz64, mask64;
	/* NVMe: 변수 선언/초기화 */
	struct pci_bus_region region, inverted_region;
	/* NVMe: PCI 리소스 이름 획득 */
	const char *res_name = pci_resource_name(dev, res - dev->resource);

	/* NVMe: PCI 장치 이름 획득 */
	res->name = pci_name(dev);

	/* NVMe: PCI config space 4바이트 읽기 */
	pci_read_config_dword(dev, pos, &l);
	/* NVMe: 변수에 값 할당: sz */
	sz = sizes[0];

	/*
	 * All bits set in sz means the device isn't working properly.
	 * If the BAR isn't implemented, all bits must be 0.  If it's a
	 * memory BAR or a ROM, bit 0 must be clear; if it's an io BAR, bit
	 * 1 must be clear.
	 */
	/* NVMe: 조걸 분기 */
	if (PCI_POSSIBLE_ERROR(sz))
		/* NVMe: 변수에 값 할당: sz */
		sz = 0;

	/*
	 * I don't know how l can have all bits set.  Copied from old code.
	 * Maybe it fixes a bug on some ancient platform.
	 */
	/* NVMe: 조걸 분기 */
	if (PCI_POSSIBLE_ERROR(l))
		/* NVMe: 변수에 값 할당: l */
		l = 0;

	/* NVMe: 조걸 분기 */
	if (type == pci_bar_unknown) {
		/* NVMe: BAR 속성 디코딩 */
		res->flags = decode_bar(dev, l);
		/* NVMe: 구조체 필드에 값 저장: res->flags | */
		res->flags |= IORESOURCE_SIZEALIGN;
		/* NVMe: 조걸 분기 */
		if (res->flags & IORESOURCE_IO) {
			/* NVMe: 비트 연산으로 값 설정/마스크: l64 */
			l64 = l & PCI_BASE_ADDRESS_IO_MASK;
			/* NVMe: 비트 연산으로 값 설정/마스크: sz64 */
			sz64 = sz & PCI_BASE_ADDRESS_IO_MASK;
			/* NVMe: 비트 연산으로 값 설정/마스크: mask64 */
			mask64 = PCI_BASE_ADDRESS_IO_MASK & (u32)IO_SPACE_LIMIT;
		/* NVMe: 코드 동작 수행 */
		} else {
			/* NVMe: 비트 연산으로 값 설정/마스크: l64 */
			l64 = l & PCI_BASE_ADDRESS_MEM_MASK;
			/* NVMe: 비트 연산으로 값 설정/마스크: sz64 */
			sz64 = sz & PCI_BASE_ADDRESS_MEM_MASK;
			/* NVMe: 변수에 값 할당: mask64 */
			mask64 = (u32)PCI_BASE_ADDRESS_MEM_MASK;
		}
	/* NVMe: 코드 동작 수행 */
	} else {
		/* NVMe: 조걸 분기 */
		if (l & PCI_ROM_ADDRESS_ENABLE)
			/* NVMe: 구조체 필드에 값 저장: res->flags | */
			res->flags |= IORESOURCE_ROM_ENABLE;
		/* NVMe: 비트 연산으로 값 설정/마스크: l64 */
		l64 = l & PCI_ROM_ADDRESS_MASK;
		/* NVMe: 비트 연산으로 값 설정/마스크: sz64 */
		sz64 = sz & PCI_ROM_ADDRESS_MASK;
		/* NVMe: 변수에 값 할당: mask64 */
		mask64 = PCI_ROM_ADDRESS_MASK;
	}

	/* NVMe: 조걸 분기 */
	if (res->flags & IORESOURCE_MEM_64) {
		/* NVMe: PCI config space 4바이트 읽기 */
		pci_read_config_dword(dev, pos + 4, &l);
		/* NVMe: 변수에 값 할당: sz */
		sz = sizes[1];

		/* NVMe: 비트 연산으로 값 설정/마스크: l64 | */
		l64 |= ((u64)l << 32);
		/* NVMe: 비트 연산으로 값 설정/마스크: sz64 | */
		sz64 |= ((u64)sz << 32);
		/* NVMe: 비트 연산으로 값 설정/마스크: mask64 | */
		mask64 |= ((u64)~0 << 32);
	}

	/* NVMe: 조걸 분기 */
	if (!sz64)
		goto fail;

	/* NVMe: BAR 크기 계산 */
	sz64 = pci_size(l64, sz64, mask64);
	/* NVMe: 조걸 분기 */
	if (!sz64) {
		/* NVMe: 정보 메시지 출력 */
		pci_info(dev, FW_BUG "%s: invalid; can't size\n", res_name);
		goto fail;
	}

	/* NVMe: 조걸 분기 */
	if (res->flags & IORESOURCE_MEM_64) {
		/* NVMe: 조걸 분기 */
		if ((sizeof(pci_bus_addr_t) < 8 || sizeof(resource_size_t) < 8)
		    /* NVMe: 코드 동작 수행 */
		    && sz64 > 0x100000000ULL) {
			/* NVMe: 구조체 필드에 비트 마스크 적용: res->flags | */
			res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
			/* NVMe: 리소스 범위 설정 */
			resource_set_range(res, 0, 0);
			/* NVMe: 오류 메시지 출력 */
			pci_err(dev, "%s: can't handle BAR larger than 4GB (size %#010llx)\n",
				/* NVMe: 코드 동작 수행 */
				res_name, (unsigned long long)sz64);
			goto out;
		}

		/* NVMe: 조걸 분기 */
		if ((sizeof(pci_bus_addr_t) < 8) && l) {
			/* Above 32-bit boundary; try to reallocate */
			/* NVMe: 구조체 필드에 값 저장: res->flags | */
			res->flags |= IORESOURCE_UNSET;
			/* NVMe: 리소스 범위 설정 */
			resource_set_range(res, 0, sz64);
			/* NVMe: 정보 메시지 출력 */
			pci_info(dev, "%s: can't handle BAR above 4GB (bus address %#010llx)\n",
				 /* NVMe: 코드 동작 수행 */
				 res_name, (unsigned long long)l64);
			goto out;
		}
	}

	/* NVMe: 구조체 필드에 값 저장: region.start */
	region.start = l64;
	/* NVMe: 구조체 필드에 값 저장: region.end */
	region.end = l64 + sz64 - 1;

	/* NVMe: 버스 주소를 CPU 물리 주소로 변환 */
	pcibios_bus_to_resource(dev->bus, res, &region);
	/* NVMe: CPU 물리 주소를 버스 주소로 변환 */
	pcibios_resource_to_bus(dev->bus, &inverted_region, res);

	/*
	 * If "A" is a BAR value (a bus address), "bus_to_resource(A)" is
	 * the corresponding resource address (the physical address used by
	 * the CPU.  Converting that resource address back to a bus address
	 * should yield the original BAR value:
	 *
	 *     resource_to_bus(bus_to_resource(A)) == A
	 *
	 * If it doesn't, CPU accesses to "bus_to_resource(A)" will not
	 * be claimed by the device.
	 */
	/* NVMe: 조걸 분기 */
	if (inverted_region.start != region.start) {
		/* NVMe: 구조체 필드에 값 저장: res->flags | */
		res->flags |= IORESOURCE_UNSET;
		/* NVMe: 구조체 필드에 값 저장: res->start */
		res->start = 0;
		/* NVMe: 구조체 필드에 값 저장: res->end */
		res->end = region.end - region.start;
		/* NVMe: 정보 메시지 출력 */
		pci_info(dev, "%s: initial BAR value %#010llx invalid\n",
			 /* NVMe: 코드 동작 수행 */
			 res_name, (unsigned long long)region.start);
	}

	goto out;


fail:
	/* NVMe: 구조체 필드에 값 저장: res->flags */
	res->flags = 0;
out:
	/* NVMe: 조걸 분기 */
	if (res->flags)
		/* NVMe: 정보 메시지 출력 */
		pci_info(dev, "%s %pR\n", res_name, res);

	/* NVMe: 결과 반환: (res->flags & IORESOURCE_MEM_64) ? 1 : 0 */
	return (res->flags & IORESOURCE_MEM_64) ? 1 : 0;
}

/* NVMe: 표준 BAR 읽기 */
static __always_inline void pci_read_bases(struct pci_dev *dev,
					   /* NVMe: 코드 동작 수행 */
					   unsigned int howmany, int rom)
{
	/* NVMe: 변수 선언/초기화 */
	u32 rombar, stdbars[PCI_STD_NUM_BARS];
	/* NVMe: 변수 선언/초기화 */
	unsigned int pos, reg;
	/* NVMe: 변수 선언/초기화 */
	u16 orig_cmd;

	/* NVMe: 컴파일 시 조건 검사 */
	BUILD_BUG_ON(statically_true(howmany > PCI_STD_NUM_BARS));

	/* NVMe: 조걸 분기 */
	if (dev->non_compliant_bars)
		return;

	/* Per PCIe r4.0, sec 9.3.4.1.11, the VF BARs are all RO Zero */
	/* NVMe: 조걸 분기 */
	if (dev->is_virtfn)
		return;

	/* No printks while decoding is disabled! */
	/* NVMe: 조걸 분기 */
	if (!dev->mmio_always_on) {
		/* NVMe: PCI config space 2바이트 읽기 */
		pci_read_config_word(dev, PCI_COMMAND, &orig_cmd);
		/* NVMe: 조걸 분기 */
		if (orig_cmd & PCI_COMMAND_DECODE_ENABLE) {
			/* NVMe: PCI config space 2바이트 쓰기 */
			pci_write_config_word(dev, PCI_COMMAND,
				/* NVMe: 코드 동작 수행 */
				orig_cmd & ~PCI_COMMAND_DECODE_ENABLE);
		}
	}

	/* NVMe: 표준 BAR 크기 측정 */
	__pci_size_stdbars(dev, howmany, PCI_BASE_ADDRESS_0, stdbars);
	/* NVMe: 조걸 분기 */
	if (rom)
		/* NVMe: ROM BAR 크기 측정 */
		__pci_size_rom(dev, rom, &rombar);

	/* NVMe: 조걸 분기 */
	if (!dev->mmio_always_on &&
	    /* NVMe: 코드 동작 수행 */
	    (orig_cmd & PCI_COMMAND_DECODE_ENABLE))
		/* NVMe: PCI config space 2바이트 쓰기 */
		pci_write_config_word(dev, PCI_COMMAND, orig_cmd);

	/* NVMe: 반복문 */
	for (pos = 0; pos < howmany; pos++) {
		/* NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
		struct resource *res = &dev->resource[pos];
		/* NVMe: 비트 연산으로 값 설정/마스크: reg */
		reg = PCI_BASE_ADDRESS_0 + (pos << 2);
		/* NVMe: 개별 BAR 읽기 */
		pos += __pci_read_base(dev, pci_bar_unknown,
				       /* NVMe: 코드 동작 수행 */
				       res, reg, &stdbars[pos]);
	}

	/* NVMe: 조걸 분기 */
	if (rom) {
		/* NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
		struct resource *res = &dev->resource[PCI_ROM_RESOURCE];
		/* NVMe: 구조체 필드에 값 저장: dev->rom_base_reg */
		dev->rom_base_reg = rom;
		/* NVMe: 구조체 필드에 비트 마스크 적용: res->flags */
		res->flags = IORESOURCE_MEM | IORESOURCE_PREFETCH |
				/* NVMe: 코드 동작 수행 */
				IORESOURCE_READONLY | IORESOURCE_SIZEALIGN;
		/* NVMe: 개별 BAR 읽기 */
		__pci_read_base(dev, pci_bar_mem32, res, rom, &rombar);
	}
}

/*
 * pci_read_bridge_io() - PCI-to-PCI 브리지의 I/O window 읽기
 *
 * NVMe 연결: NVMe PCIe 장치는 Memory BAR만 사용하므로 I/O window는 직접
 * 사용되지 않는다. 다만 Root Port 아래의 레거시 호환성을 위해 bridge
 * 설정 공간의 I/O base/limit를 파싱하여 bus resource 트리를 완성한다.
 */
/* NVMe: 브리지 IO 윈도우 읽기 */
static void pci_read_bridge_io(struct pci_dev *dev, struct resource *res,
			       /* NVMe: 코드 동작 수행 */
			       bool log)
{
	/* NVMe: 변수 선언/초기화 */
	u8 io_base_lo, io_limit_lo;
	/* NVMe: 변수 선언/초기화 */
	unsigned long io_mask, io_granularity, base, limit;
	/* NVMe: 변수 선언/초기화 */
	struct pci_bus_region region;

	if (!dev->io_window) /* NVMe: 브리지가 I/O window를 지원하지 않으면 즉시 반환 */
		return;

	io_mask = PCI_IO_RANGE_MASK; /* NVMe: 기본 4K granularity I/O mask */
	io_granularity = 0x1000; /* NVMe: I/O window 기본 granule 4KB */
	if (dev->io_window_1k) { /* NVMe: 1K granularity를 지원하는 bridge인지 확인 */
		/* Support 1K I/O space granularity */
		io_mask = PCI_IO_1K_RANGE_MASK; /* NVMe: 1KB 단위 mask로 변경 */
		io_granularity = 0x400; /* NVMe: granule을 1KB로 조정 */
	}

	pci_read_config_byte(dev, PCI_IO_BASE, &io_base_lo); /* NVMe: bridge 설정 공간의 I/O base 하위 바이트 읽기 */
	pci_read_config_byte(dev, PCI_IO_LIMIT, &io_limit_lo); /* NVMe: I/O limit 하위 바이트 읽기 */
	base = (io_base_lo & io_mask) << 8; /* NVMe: base 상위 주소 비트 조합 */
	limit = (io_limit_lo & io_mask) << 8; /* NVMe: limit 상위 주소 비트 조합 */

	if ((io_base_lo & PCI_IO_RANGE_TYPE_MASK) == PCI_IO_RANGE_TYPE_32) { /* NVMe: 32-bit I/O window인지 확인 */
		/* NVMe: 변수 선언/초기화 */
		u16 io_base_hi, io_limit_hi;

		pci_read_config_word(dev, PCI_IO_BASE_UPPER16, &io_base_hi); /* NVMe: 32-bit base 상위 16비트 읽기 */
		pci_read_config_word(dev, PCI_IO_LIMIT_UPPER16, &io_limit_hi); /* NVMe: 32-bit limit 상위 16비트 읽기 */
		base |= ((unsigned long) io_base_hi << 16); /* NVMe: 상위 비트를 base에 합성 */
		limit |= ((unsigned long) io_limit_hi << 16); /* NVMe: 상위 비트를 limit에 합성 */
	}

	res->flags = (io_base_lo & PCI_IO_RANGE_TYPE_MASK) | IORESOURCE_IO; /* NVMe: resource 플래그에 I/O 영역임을 표시 */

	if (base <= limit) { /* NVMe: 유효한 I/O window 범위인지 확인 */
		/* NVMe: 구조체 필드에 값 저장: region.start */
		region.start = base;
		/* NVMe: 구조체 필드에 값 저장: region.end */
		region.end = limit + io_granularity - 1;
		pcibios_bus_to_resource(dev->bus, res, &region); /* NVMe: bus 주소를 CPU 물리 주소로 변환하여 res에 저장 */
		/* NVMe: 조걸 분기 */
		if (log)
			pci_info(dev, "  bridge window %pR\n", res); /* NVMe: 로그 출력 시 bridge I/O window 정보 기록 */
	/* NVMe: 코드 동작 수행 */
	} else {
		resource_set_range(res, 0, 0); /* NVMe: 유효하지 않은 window이면 범위를 0으로 설정 */
		res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED; /* NVMe: 할당되지 않은/비활성화 상태로 표시 */
	}
}

/*
 * pci_read_bridge_mmio() - PCI-to-PCI 브리지의 32-bit MMIO window 읽기
 *
 * NVMe 연결: NVMe BAR가 속할 수 있는 non-prefetchable 32-bit memory
 * window의 범위를 파싱. Root Port 아래의 메모리 공간이 이 window를 통해
 * 하위 버스로 포워딩되므로, NVMe BAR 할당 시 이 범위를 벗어나면 안 된다.
 */
/* NVMe: 브리지 MMIO 윈도우 읽기 */
static void pci_read_bridge_mmio(struct pci_dev *dev, struct resource *res,
				 /* NVMe: 코드 동작 수행 */
				 bool log)
{
	/* NVMe: 변수 선언/초기화 */
	u16 mem_base_lo, mem_limit_lo;
	/* NVMe: 변수 선언/초기화 */
	unsigned long base, limit;
	/* NVMe: 변수 선언/초기화 */
	struct pci_bus_region region;

	pci_read_config_word(dev, PCI_MEMORY_BASE, &mem_base_lo); /* NVMe: bridge 설정 공간의 MMIO base 읽기 */
	pci_read_config_word(dev, PCI_MEMORY_LIMIT, &mem_limit_lo); /* NVMe: MMIO limit 읽기 */
	base = ((unsigned long) mem_base_lo & PCI_MEMORY_RANGE_MASK) << 16; /* NVMe: 16비트 정렬된 base 주소 산출 */
	limit = ((unsigned long) mem_limit_lo & PCI_MEMORY_RANGE_MASK) << 16; /* NVMe: 16비트 정렬된 limit 주소 산출 */

	res->flags = (mem_base_lo & PCI_MEMORY_RANGE_TYPE_MASK) | IORESOURCE_MEM; /* NVMe: 32-bit memory resource 플래그 설정 */

	if (base <= limit) { /* NVMe: 유효한 MMIO window 범위인지 확인 */
		/* NVMe: 구조체 필드에 값 저장: region.start */
		region.start = base;
		/* NVMe: 구조체 필드에 값 저장: region.end */
		region.end = limit + 0xfffff;
		pcibios_bus_to_resource(dev->bus, res, &region); /* NVMe: bus 주소를 CPU 물리 주소로 변환 */
		/* NVMe: 조걸 분기 */
		if (log)
			pci_info(dev, "  bridge window %pR\n", res); /* NVMe: bridge MMIO window 정보 로깅 */
	/* NVMe: 코드 동작 수행 */
	} else {
		resource_set_range(res, 0, 0); /* NVMe: 유효하지 않으면 범위 0으로 초기화 */
		res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED; /* NVMe: 비활성화/미할당 표시 */
	}
}

/*
 * pci_read_bridge_mmio_pref() - PCI-to-PCI 브리지의 prefetchable MMIO window 읽기
 *
 * NVMe 연결: NVMe 컨트롤러의 BAR는 종종 prefetchable로 노출된다. 본 함수는
 * Root Port에서 하위 버스로 포워딩되는 prefetchable memory window의 범위를
 * 파싱하며, NVMe BAR(특히 64-bit doorbell 영역)가 이 window 안에 배치될
 * 수 있는지 판단하는 데 사용된다.
 */
/* NVMe: 브리지 Prefetchable MMIO 윈도우 읽기 */
static void pci_read_bridge_mmio_pref(struct pci_dev *dev, struct resource *res,
				      /* NVMe: 코드 동작 수행 */
				      bool log)
{
	/* NVMe: 변수 선언/초기화 */
	u16 mem_base_lo, mem_limit_lo;
	/* NVMe: 변수 선언/초기화 */
	u64 base64, limit64;
	/* NVMe: 변수 선언/초기화 */
	pci_bus_addr_t base, limit;
	/* NVMe: 변수 선언/초기화 */
	struct pci_bus_region region;

	if (!dev->pref_window) /* NVMe: bridge가 prefetchable window를 지원하지 않으면 즉시 반환 */
		return;

	pci_read_config_word(dev, PCI_PREF_MEMORY_BASE, &mem_base_lo); /* NVMe: prefetchable base 하위 워드 읽기 */
	pci_read_config_word(dev, PCI_PREF_MEMORY_LIMIT, &mem_limit_lo); /* NVMe: prefetchable limit 하위 워드 읽기 */
	base64 = (mem_base_lo & PCI_PREF_RANGE_MASK) << 16; /* NVMe: prefetchable base 주소 초기화 */
	limit64 = (mem_limit_lo & PCI_PREF_RANGE_MASK) << 16; /* NVMe: prefetchable limit 주소 초기화 */

	if ((mem_base_lo & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) { /* NVMe: 64-bit prefetchable window인지 확인 */
		/* NVMe: 변수 선언/초기화 */
		u32 mem_base_hi, mem_limit_hi;

		pci_read_config_dword(dev, PCI_PREF_BASE_UPPER32, &mem_base_hi); /* NVMe: 64-bit base 상위 32비트 읽기 */
		pci_read_config_dword(dev, PCI_PREF_LIMIT_UPPER32, &mem_limit_hi); /* NVMe: 64-bit limit 상위 32비트 읽기 */

		/*
		 * Some bridges set the base > limit by default, and some
		 * (broken) BIOSes do not initialize them.  If we find
		 * this, just assume they are not being used.
		 */
		if (mem_base_hi <= mem_limit_hi) { /* NVMe: 상위 비트가 정상 범위이면 64-bit 주소 합성 */
			/* NVMe: 비트 연산으로 값 설정/마스크: base64 | */
			base64 |= (u64) mem_base_hi << 32;
			/* NVMe: 비트 연산으로 값 설정/마스크: limit64 | */
			limit64 |= (u64) mem_limit_hi << 32;
		}
	}

	/* NVMe: 변수에 값 할당: base */
	base = (pci_bus_addr_t) base64;
	/* NVMe: 변수에 값 할당: limit */
	limit = (pci_bus_addr_t) limit64;

	if (base != base64) { /* NVMe: 64-bit 주소가 32-bit 공간에 잘리면 처리 불가 */
		/* NVMe: 오류 메시지 출력 */
		pci_err(dev, "can't handle bridge window above 4GB (bus address %#010llx)\n",
			/* NVMe: 코드 동작 수행 */
			(unsigned long long) base64);
		return;
	}

	/* NVMe: 구조체 필드에 비트 마스크 적용: res->flags */
	res->flags = (mem_base_lo & PCI_PREF_RANGE_TYPE_MASK) | IORESOURCE_MEM |
		     IORESOURCE_PREFETCH; /* NVMe: prefetchable memory resource 플래그 설정 */
	/* NVMe: 조걸 분기 */
	if (res->flags & PCI_PREF_RANGE_TYPE_64)
		res->flags |= IORESOURCE_MEM_64; /* NVMe: 64-bit window이면 MEM_64 플래그 추가 */

	if (base <= limit) { /* NVMe: 유효한 prefetchable window 범위인지 확인 */
		/* NVMe: 구조체 필드에 값 저장: region.start */
		region.start = base;
		/* NVMe: 구조체 필드에 값 저장: region.end */
		region.end = limit + 0xfffff;
		pcibios_bus_to_resource(dev->bus, res, &region); /* NVMe: bus 주소를 CPU 물리 주소로 변환 */
		/* NVMe: 조걸 분기 */
		if (log)
			pci_info(dev, "  bridge window %pR\n", res); /* NVMe: prefetchable bridge window 정보 로깅 */
	/* NVMe: 코드 동작 수행 */
	} else {
		resource_set_range(res, 0, 0); /* NVMe: 유효하지 않으면 범위 0으로 설정 */
		res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED; /* NVMe: 비활성화/미할당 표시 */
	}
}

/*
 * pci_read_bridge_windows() - 브리지의 I/O/MMIO/prefetchable window를 모두 읽음
 *
 * NVMe 연결: NVMe SSD가 연결된 Root Port나 Switch Downstream Port의
 * 주소 포워딩 window를 파싱. NVMe BAR가 이 window들의 합집합 안에
 * 할당되어야 하며, 특히 64-bit prefetchable window가 있어야 큰 doorbell
 * 영역을 위한 BAR를 배치할 수 있다.
 */
/* NVMe: 함수 정의: pci_read_bridge_windows */
static void pci_read_bridge_windows(struct pci_dev *bridge)
{
	/* NVMe: 변수 선언/초기화 */
	u32 buses;
	/* NVMe: 변수 선언/초기화 */
	u16 io;
	/* NVMe: 변수 선언/초기화 */
	u32 pmem, tmp;
	/* NVMe: 변수 선언/초기화 */
	struct resource res;

	pci_read_config_dword(bridge, PCI_PRIMARY_BUS, &buses); /* NVMe: primary/secondary/subordinate bus 번호 읽기 */
	/* NVMe: 구조체 필드에 값 저장: res.flags */
	res.flags = IORESOURCE_BUS;
	res.start = FIELD_GET(PCI_SECONDARY_BUS_MASK, buses); /* NVMe: secondary bus 번호 추출 */
	res.end = FIELD_GET(PCI_SUBORDINATE_BUS_MASK, buses); /* NVMe: subordinate bus 번호 추출 */
	/* NVMe: 정보 메시지 출력 */
	pci_info(bridge, "PCI bridge to %pR%s\n", &res,
		 bridge->transparent ? " (subtractive decode)" : ""); /* NVMe: bridge가 커버하는 bus 범위 로깅 */

	pci_read_config_word(bridge, PCI_IO_BASE, &io); /* NVMe: I/O window base/limit 워드 읽기 */
	if (!io) { /* NVMe: I/O window가 아직 설정되지 않았으면 쓰기 가능성 테스트 */
		pci_write_config_word(bridge, PCI_IO_BASE, 0xe0f0); /* NVMe: test pattern 쓰기 */
		pci_read_config_word(bridge, PCI_IO_BASE, &io); /* NVMe: 쓰인 값이 돌아오는지 확인 */
		pci_write_config_word(bridge, PCI_IO_BASE, 0x0); /* NVMe: 원래 0으로 복원 */
	}
	if (io) { /* NVMe: I/O window가 실제로 존재하면 */
		bridge->io_window = 1; /* NVMe: bridge가 I/O window를 가짐을 표시 */
		pci_read_bridge_io(bridge, &res, true); /* NVMe: I/O window 자세히 파싱 */
	}

	pci_read_bridge_mmio(bridge, &res, true); /* NVMe: 32-bit MMIO window 파싱; NVMe BAR 배치에 관련 */

	/*
	 * DECchip 21050 pass 2 errata: the bridge may miss an address
	 * disconnect boundary by one PCI data phase.  Workaround: do not
	 * use prefetching on this device.
	 */
	if (bridge->vendor == PCI_VENDOR_ID_DEC && bridge->device == 0x0001) /* NVMe: DECchip errata가 있으면 prefetchable window 스킵 */
		return;

	pci_read_config_dword(bridge, PCI_PREF_MEMORY_BASE, &pmem); /* NVMe: prefetchable memory base/limit 읽기 */
	if (!pmem) { /* NVMe: prefetchable window가 설정되지 않았으면 쓰기 가능성 테스트 */
		/* NVMe: PCI config space 4바이트 쓰기 */
		pci_write_config_dword(bridge, PCI_PREF_MEMORY_BASE,
					       0xffe0fff0); /* NVMe: test pattern 쓰기 */
		pci_read_config_dword(bridge, PCI_PREF_MEMORY_BASE, &pmem); /* NVMe: 값이 돌아오는지 확인 */
		pci_write_config_dword(bridge, PCI_PREF_MEMORY_BASE, 0x0); /* NVMe: 원래 0으로 복원 */
	}
	if (!pmem) /* NVMe: prefetchable window가 없으면 여기서 종료 */
		return;

	bridge->pref_window = 1; /* NVMe: bridge가 prefetchable window를 가짐을 표시 */

	if ((pmem & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) { /* NVMe: 64-bit prefetchable window인지 확인 */

		/*
		 * Bridge claims to have a 64-bit prefetchable memory
		 * window; verify that the upper bits are actually
		 * writable.
		 */
		pci_read_config_dword(bridge, PCI_PREF_BASE_UPPER32, &pmem); /* NVMe: 상위 32비트 레지스터 원래 값 저장 */
		/* NVMe: PCI config space 4바이트 쓰기 */
		pci_write_config_dword(bridge, PCI_PREF_BASE_UPPER32,
				       0xffffffff); /* NVMe: 상위 32비트 쓰기 가능성 테스트 */
		pci_read_config_dword(bridge, PCI_PREF_BASE_UPPER32, &tmp); /* NVMe: 실제 쓰인 값 확인 */
		pci_write_config_dword(bridge, PCI_PREF_BASE_UPPER32, pmem); /* NVMe: 원래 값 복원 */
		/* NVMe: 조걸 분기 */
		if (tmp)
			bridge->pref_64_window = 1; /* NVMe: 상위 32비트가 실제로 쓰이면 64-bit window 지원 */
	}

	pci_read_bridge_mmio_pref(bridge, &res, true); /* NVMe: prefetchable window 파싱; NVMe 64-bit BAR 배치에 필수 */
}

/*
 * pci_read_bridge_bases() - 자식 버스의 bridge resource 포인터를 설정하고
 *                            window들을 읽음
 *
 * NVMe 연결: NVMe SSD가 연결된 하위 PCIe 버스의 resource 트리를 구성.
 * 이 함수가 완료되면 child->resource[]가 bridge의 I/O/MMIO/prefetchable
 * window를 가리키므로, 이후 NVMe BAR 할당이 이 window 안에서 이루어진다.
 */
/* NVMe: 함수 정의: pci_read_bridge_bases */
void pci_read_bridge_bases(struct pci_bus *child)
{
	struct pci_dev *dev = child->self; /* NVMe: 자식 버스를 만든 bridge PCI 장치 */
	/* NVMe: 코드 동작 수행 */
	struct resource *res;
	/* NVMe: 변수 선언/초기화 */
	int i;

	if (pci_is_root_bus(child))	/* It's a host bus, nothing to read */ /* NVMe: root bus이면 bridge window가 없으므로 종료 */
		return;

	/* NVMe: 정보 메시지 출력 */
	pci_info(dev, "PCI bridge to %pR%s\n",
		 /* NVMe: 코드 동작 수행 */
		 &child->busn_res,
		 dev->transparent ? " (subtractive decode)" : ""); /* NVMe: 자식 버스 번호 범위 로깅 */

	pci_bus_remove_resources(child); /* NVMe: 기존 자식 버스 리소스 제거 후 재구성 */
	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) /* NVMe: bridge당 I/O, MEM, PREF MEM resource 포인터 연결 */
		/* NVMe: 구조체 필드에 비트 마스크 적용: child->resource[i] */
		child->resource[i] = &dev->resource[PCI_BRIDGE_RESOURCES+i];

	/* NVMe: 브리지 IO 윈도우 읽기 */
	pci_read_bridge_io(child->self,
			   child->resource[PCI_BUS_BRIDGE_IO_WINDOW], false); /* NVMe: I/O window 파싱 */
	/* NVMe: 브리지 MMIO 윈도우 읽기 */
	pci_read_bridge_mmio(child->self,
			     child->resource[PCI_BUS_BRIDGE_MEM_WINDOW], false); /* NVMe: 32-bit MMIO window 파싱 */
	/* NVMe: 브리지 Prefetchable MMIO 윈도우 읽기 */
	pci_read_bridge_mmio_pref(child->self,
				  /* NVMe: 코드 동작 수행 */
				  child->resource[PCI_BUS_BRIDGE_PREF_MEM_WINDOW],
				  false); /* NVMe: prefetchable MMIO window 파싱; NVMe BAR 관련 */

	if (!dev->transparent) /* NVMe: subtractive decode bridge가 아니면 종료 */
		return;

	pci_bus_for_each_resource(child->parent, res) { /* NVMe: subtractive decode bridge는 부모 리소스를 상속 */
		if (!res || !res->flags) /* NVMe: 유효하지 않은 부모 리소스는 건다 */
			continue;

		pci_bus_add_resource(child, res); /* NVMe: 부모의 리소스를 자식 버스에 추가 */
		pci_info(dev, "  bridge window %pR (subtractive decode)\n", res); /* NVMe: 상속된 window 로깅 */
	}
}

/*
 * pci_alloc_bus() - 새 PCI 버스를 표현할 struct pci_bus 할당 및 초기화
 *
 * NVMe 연결: NVMe 컨트롤러가 연결될 하위 PCIe 버스가 여기서 생성된다.
 * bus->devices 리스트는 이 버스에 속한 NVMe 컨트롤러의 pci_dev가
 * 연결될 위치이며, bus->resources는 BAR 할당의 기준점이 된다.
 */
/* NVMe: PCI 버스 구조체 할당 */
static struct pci_bus *pci_alloc_bus(struct pci_bus *parent)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *b;

	b = kzalloc_obj(*b); /* NVMe: PCI 버스 구조체를 0으로 초기화하며 할당 */
	if (!b) /* NVMe: 메모리 할당 실패 시 NULL 반환 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	INIT_LIST_HEAD(&b->node); /* NVMe: bus 리스트 연결용 node 초기화 */
	INIT_LIST_HEAD(&b->children); /* NVMe: 하위 버스 리스트 초기화 */
	INIT_LIST_HEAD(&b->devices); /* NVMe: 이 버스의 pci_dev 리스트 초기화; NVMe 컨트롤러가 여기 연결됨 */
	INIT_LIST_HEAD(&b->slots); /* NVMe: PCI slot 리스트 초기화 */
	INIT_LIST_HEAD(&b->resources); /* NVMe: 버스 리소스 리스트 초기화; NVMe BAR 할당 기준 */
	b->max_bus_speed = PCI_SPEED_UNKNOWN; /* NVMe: 최대 버스 속도 미확인 상태로 초기화 */
	b->cur_bus_speed = PCI_SPEED_UNKNOWN; /* NVMe: 현재 버스 속도 미확인 상태로 초기화 */
/* NVMe: 컴파일 조건: CONFIG_PCI_DOMAINS_GENERIC 정의 시 포함 */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	if (parent) /* NVMe: 부모 버스가 있으면 domain 번호 상속; NVMe 컨트롤러의 domain 결정 */
		/* NVMe: 구조체 필드에 값 저장: b->domain_nr */
		b->domain_nr = parent->domain_nr;
/* NVMe: 컴파일 조건 종료 */
#endif
	return b; /* NVMe: 초기화된 pci_bus 반환 */
}

/*
 * pci_release_host_bridge_dev() - host bridge device의 최종 해제
 *
 * NVMe 연결: NVMe 컨트롤러가 연결된 Root Complex의 host bridge가
 * 소멸될 때 호출. bridge의 dma_ranges/window 해제는 NVMe DMA 주소
 * 변환에 사용되던 정보를 정리한다.
 */
/* NVMe: 함수 정의: pci_release_host_bridge_dev */
static void pci_release_host_bridge_dev(struct device *dev)
{
	struct pci_host_bridge *bridge = to_pci_host_bridge(dev); /* NVMe: device에서 host bridge 구조체 획득 */

	if (bridge->release_fn) /* NVMe: platform/driver specific 해제 콜백이 있으면 호출 */
		/* NVMe: 함수 호출: release_fn */
		bridge->release_fn(bridge);

	pci_free_resource_list(&bridge->windows); /* NVMe: CPU↔bus 주소 변환에 사용되던 address window 해제 */
	pci_free_resource_list(&bridge->dma_ranges); /* NVMe: DMA 주소 변환용 dma_range 해제; NVMe DMA 영향 */

	/* Host bridges only have domain_nr set in the emulation case */
	if (bridge->domain_nr != PCI_DOMAIN_NR_NOT_SET) /* NVMe: 에뮬레이션 domain 번호가 할당된 경우 반납 */
		/* NVMe: 에뮬레이션 도메인 번호 반환 */
		pci_bus_release_emul_domain_nr(bridge->domain_nr);

	kfree(bridge); /* NVMe: host bridge 구조체 메모리 해제 */
}

/* NVMe: 변수에 값 할당: static const struct attribute_group *pci_host_bridge_groups[] */
static const struct attribute_group *pci_host_bridge_groups[] = {
/* NVMe: 컴파일 조건: CONFIG_PCI_IDE 정의 시 포함 */
#ifdef CONFIG_PCI_IDE
	/* NVMe: 코드 동작 수행 */
	&pci_ide_attr_group,
/* NVMe: 컴파일 조건 종료 */
#endif
	/* NVMe: 코드 동작 수행 */
	NULL
};

/* NVMe: 변수 선언/초기화 */
static const struct device_type pci_host_bridge_type = {
	/* NVMe: 구조체 필드에 값 저장: .groups */
	.groups = pci_host_bridge_groups,
	/* NVMe: 구조체 필드에 값 저장: .release */
	.release = pci_release_host_bridge_dev,
};

/*
 * pci_init_host_bridge() - host bridge 구조체의 필드를 기본값으로 초기화
 *
 * NVMe 연결: host bridge는 NVMe 컨트롤러가 연결된 PCIe 트리의 루트.
 * native_aer/native_ltr/native_dpc 등의 플래그는 NVMe 장치의 AER, LTR,
 * DPC 오류 보고 및 전원 관리 기능이 OS에서 처리될지 여부를 결정한다.
 */
/* NVMe: 함수 정의: pci_init_host_bridge */
static void pci_init_host_bridge(struct pci_host_bridge *bridge)
{
	INIT_LIST_HEAD(&bridge->windows); /* NVMe: CPU↔bus 주소 변환 window 리스트 초기화 */
	INIT_LIST_HEAD(&bridge->dma_ranges); /* NVMe: DMA 주소 매핑 range 리스트 초기화 */

	/*
	 * We assume we can manage these PCIe features.  Some systems may
	 * reserve these for use by the platform itself, e.g., an ACPI BIOS
	 * may implement its own AER handling and use _OSC to prevent the
	 * OS from interfering.
	 */
	bridge->native_aer = 1; /* NVMe: OS가 AER(Advanced Error Reporting) 처리; NVMe 데이터 무결성 */
	bridge->native_pcie_hotplug = 1; /* NVMe: PCIe native hotplug 관리 */
	bridge->native_shpc_hotplug = 1; /* NVMe: SHPC hotplug 관리 */
	bridge->native_pme = 1; /* NVMe: Power Management Event 처리 */
	bridge->native_ltr = 1; /* NVMe: LTR(Latency Tolerance Reporting) 처리; NVMe ASPM/RTD3 지연 */
	bridge->native_dpc = 1; /* NVMe: Downstream Port Containment 처리; NVMe 오류 격리 */
	bridge->domain_nr = PCI_DOMAIN_NR_NOT_SET; /* NVMe: domain 번호 아직 미할당 */
	bridge->native_cxl_error = 1; /* NVMe: CXL 오류 처리(해당 시) */
	bridge->dev.type = &pci_host_bridge_type; /* NVMe: device type 설정 */
	pci_ide_init_host_bridge(bridge); /* NVMe: IDE 관련 host bridge 초기화 */

	device_initialize(&bridge->dev); /* NVMe: kobject/device core 초기화 */
}

/*
 * pci_alloc_host_bridge() - host bridge 구조체 할당
 *
 * NVMe 연결: NVMe 컨트롤러가 연결된 Root Complex를 표현하는 host bridge가
 * 여기서 생성. priv 크기만큼 추가 공간을 할당하여 platform driver가
 * 고유 데이터를 저장할 수 있다.
 */
/* NVMe: PCI 호스트 브리지 할당 */
struct pci_host_bridge *pci_alloc_host_bridge(size_t priv)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_host_bridge *bridge;

	bridge = kzalloc(sizeof(*bridge) + priv, GFP_KERNEL); /* NVMe: host bridge + private data 공간 할당 */
	if (!bridge) /* NVMe: 메모리 부족 시 NULL 반환 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	pci_init_host_bridge(bridge); /* NVMe: 기본 필드 초기화; NVMe DMA/ASPM/AER 정책의 근거 */

	return bridge; /* NVMe: 초기화된 host bridge 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_alloc_host_bridge);

/* NVMe: 함수 정의: devm_pci_alloc_host_bridge_release */
static void devm_pci_alloc_host_bridge_release(void *data)
{
	/* NVMe: 함수 호출: pci_free_host_bridge */
	pci_free_host_bridge(data);
}

/*
 * devm_pci_alloc_host_bridge() - device-managed host bridge 할당
 *
 * NVMe 연결: host controller driver가 probe()에서 사용. device가 제거될
 * 때 자동으로 pci_free_host_bridge()가 호출되어 NVMe 컨트롤러가 연결된
 * PCIe 계층의 루트 자원을 정리한다.
 */
/* NVMe: devres 기반 호스트 브리지 할당 */
struct pci_host_bridge *devm_pci_alloc_host_bridge(struct device *dev,
						   /* NVMe: 코드 동작 수행 */
						   size_t priv)
{
	/* NVMe: 변수 선언/초기화 */
	int ret;
	/* NVMe: 코드 동작 수행 */
	struct pci_host_bridge *bridge;

	bridge = pci_alloc_host_bridge(priv); /* NVMe: host bridge 할당 */
	if (!bridge) /* NVMe: 할당 실패 시 NULL 반환 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	bridge->dev.parent = dev; /* NVMe: host bridge의 부모 device 설정 */

	/* NVMe: devres 동작 등록 */
	ret = devm_add_action_or_reset(dev, devm_pci_alloc_host_bridge_release,
				       bridge); /* NVMe: device 제거 시 자동 해제 콜백 등록 */
	if (ret) /* NVMe: 등록 실패 시 bridge가 자동 해제되므로 NULL 반환 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	ret = devm_of_pci_bridge_init(dev, bridge); /* NVMe: device tree 기반 bridge 초기화 */
	if (ret) /* NVMe: OF 초기화 실패 시 NULL 반환; bridge는 devm에 의해 정리 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	return bridge; /* NVMe: 초기화된 host bridge 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(devm_pci_alloc_host_bridge);

/*
 * pci_free_host_bridge() - host bridge 참조 카운트 감소를 통한 해제
 *
 * NVMe 연결: host bridge가 해제되면 이 아래의 NVMe 장치도 정리 경로에
 * 들어간다.
 */
/* NVMe: 함수 정의: pci_free_host_bridge */
void pci_free_host_bridge(struct pci_host_bridge *bridge)
{
	put_device(&bridge->dev); /* NVMe: host bridge device 참조 카운트 감소; 0이면 release 호출 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_free_host_bridge);

/* Indexed by PCI_X_SSTATUS_FREQ (secondary bus mode and frequency) */
/* NVMe: 변수 선언/초기화 */
static const unsigned char pcix_bus_speed[] = {
	PCI_SPEED_UNKNOWN,		/* 0 */
	PCI_SPEED_66MHz_PCIX,		/* 1 */
	PCI_SPEED_100MHz_PCIX,		/* 2 */
	PCI_SPEED_133MHz_PCIX,		/* 3 */
	PCI_SPEED_UNKNOWN,		/* 4 */
	PCI_SPEED_66MHz_PCIX_ECC,	/* 5 */
	PCI_SPEED_100MHz_PCIX_ECC,	/* 6 */
	PCI_SPEED_133MHz_PCIX_ECC,	/* 7 */
	PCI_SPEED_UNKNOWN,		/* 8 */
	PCI_SPEED_66MHz_PCIX_266,	/* 9 */
	PCI_SPEED_100MHz_PCIX_266,	/* A */
	PCI_SPEED_133MHz_PCIX_266,	/* B */
	PCI_SPEED_UNKNOWN,		/* C */
	PCI_SPEED_66MHz_PCIX_533,	/* D */
	PCI_SPEED_100MHz_PCIX_533,	/* E */
	PCI_SPEED_133MHz_PCIX_533	/* F */
};

/* Indexed by PCI_EXP_LNKCAP_SLS, PCI_EXP_LNKSTA_CLS */
/* NVMe: 변수 선언/초기화 */
const unsigned char pcie_link_speed[] = {
	PCI_SPEED_UNKNOWN,		/* 0 */
	PCIE_SPEED_2_5GT,		/* 1 */
	PCIE_SPEED_5_0GT,		/* 2 */
	PCIE_SPEED_8_0GT,		/* 3 */
	PCIE_SPEED_16_0GT,		/* 4 */
	PCIE_SPEED_32_0GT,		/* 5 */
	PCIE_SPEED_64_0GT,		/* 6 */
	PCI_SPEED_UNKNOWN,		/* 7 */
	PCI_SPEED_UNKNOWN,		/* 8 */
	PCI_SPEED_UNKNOWN,		/* 9 */
	PCI_SPEED_UNKNOWN,		/* A */
	PCI_SPEED_UNKNOWN,		/* B */
	PCI_SPEED_UNKNOWN,		/* C */
	PCI_SPEED_UNKNOWN,		/* D */
	PCI_SPEED_UNKNOWN,		/* E */
	PCI_SPEED_UNKNOWN		/* F */
};
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pcie_link_speed);

/**
 * pcie_get_link_speed - Get speed value from PCIe generation number
 * @speed: PCIe speed (1-based: 1 = 2.5GT, 2 = 5GT, ...)
 *
 * Returns the speed value (e.g., PCIE_SPEED_2_5GT) if @speed is valid,
 * otherwise returns PCI_SPEED_UNKNOWN.
 */
/* NVMe: 링크 속도 값 획득 */
unsigned char pcie_get_link_speed(unsigned int speed)
{
	/* NVMe: 조걸 분기 */
	if (speed >= ARRAY_SIZE(pcie_link_speed))
		/* NVMe: 결과 반환: PCI_SPEED_UNKNOWN */
		return PCI_SPEED_UNKNOWN;

	/* NVMe: 결과 반환: pcie_link_speed[speed] */
	return pcie_link_speed[speed];
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pcie_get_link_speed);

/* NVMe: 버스 속도 문자열 반환 */
const char *pci_speed_string(enum pci_bus_speed speed)
{
	/* Indexed by the pci_bus_speed enum */
	/* NVMe: 변수에 값 할당: static const char *speed_strings[] */
	static const char *speed_strings[] = {
	    "33 MHz PCI",		/* 0x00 */
	    "66 MHz PCI",		/* 0x01 */
	    "66 MHz PCI-X",		/* 0x02 */
	    "100 MHz PCI-X",		/* 0x03 */
	    "133 MHz PCI-X",		/* 0x04 */
	    NULL,			/* 0x05 */
	    NULL,			/* 0x06 */
	    NULL,			/* 0x07 */
	    NULL,			/* 0x08 */
	    "66 MHz PCI-X 266",		/* 0x09 */
	    "100 MHz PCI-X 266",	/* 0x0a */
	    "133 MHz PCI-X 266",	/* 0x0b */
	    "Unknown AGP",		/* 0x0c */
	    "1x AGP",			/* 0x0d */
	    "2x AGP",			/* 0x0e */
	    "4x AGP",			/* 0x0f */
	    "8x AGP",			/* 0x10 */
	    "66 MHz PCI-X 533",		/* 0x11 */
	    "100 MHz PCI-X 533",	/* 0x12 */
	    "133 MHz PCI-X 533",	/* 0x13 */
	    "2.5 GT/s PCIe",		/* 0x14 */
	    "5.0 GT/s PCIe",		/* 0x15 */
	    "8.0 GT/s PCIe",		/* 0x16 */
	    "16.0 GT/s PCIe",		/* 0x17 */
	    "32.0 GT/s PCIe",		/* 0x18 */
	    "64.0 GT/s PCIe",		/* 0x19 */
	};

	/* NVMe: 조걸 분기 */
	if (speed < ARRAY_SIZE(speed_strings))
		/* NVMe: 결과 반환: speed_strings[speed] */
		return speed_strings[speed];
	/* NVMe: 결과 반환: "Unknown" */
	return "Unknown";
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_speed_string);

/*
 * pcie_update_link_speed() - PCIe 링크 속도/폭 변화 시 bus->cur_bus_speed 갱신
 *
 * NVMe 연결: 링크 재학습(retrain)이나 hotplug 후 링크 속도가 바뀌면
 * NVMe DMA throughput이 직접 영향받는다. LNKSTA/LNKSTA2를 읽어 현재
 * 협상된 speed/width를 반영.
 */
/* NVMe: 현재 링크 속도 갱신 */
void pcie_update_link_speed(struct pci_bus *bus,
			    /* NVMe: 코드 동작 수행 */
			    enum pcie_link_change_reason reason)
{
	struct pci_dev *bridge = bus->self; /* NVMe: 이 버스의 upstream bridge */
	/* NVMe: 변수 선언/초기화 */
	u16 linksta, linksta2;

	pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &linksta); /* NVMe: PCIe Link Status 레지스터 읽기; 현재 link speed/width 포함 */
	pcie_capability_read_word(bridge, PCI_EXP_LNKSTA2, &linksta2); /* NVMe: PCIe Link Status 2 레지스터 읽기 */

	__pcie_update_link_speed(bus, reason, linksta, linksta2); /* NVMe: 읽은 값으로 bus 속도 갱신; NVMe 성능 추적에 사용 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pcie_update_link_speed);

/* NVMe: 변수 선언/초기화 */
static unsigned char agp_speeds[] = {
	/* NVMe: 코드 동작 수행 */
	AGP_UNKNOWN,
	/* NVMe: 코드 동작 수행 */
	AGP_1X,
	/* NVMe: 코드 동작 수행 */
	AGP_2X,
	/* NVMe: 코드 동작 수행 */
	AGP_4X,
	/* NVMe: 코드 동작 수행 */
	AGP_8X
};

/*
 * agp_speed() - AGP 상태 비트를 enum pci_bus_speed로 변환
 *
 * NVMe 연결: NVMe 장치는 AGP를 사용하지 않으므로 직접 관련 없음.
 */
/* NVMe: 함수 정의: agp_speed */
static enum pci_bus_speed agp_speed(int agp3, int agpstat)
{
	/* NVMe: 변수 선언/초기화 */
	int index = 0;

	/* NVMe: 조걸 분기 */
	if (agpstat & 4)
		index = 3; /* NVMe: AGP 4x 또는 AGP3 8x */
	/* NVMe: 추가 조걸 분기 */
	else if (agpstat & 2)
		index = 2; /* NVMe: AGP 2x 또는 AGP3 4x */
	/* NVMe: 추가 조걸 분기 */
	else if (agpstat & 1)
		index = 1; /* NVMe: AGP 1x 또는 AGP3 1x */
	/* NVMe: 조걸 분기의 else 경로 */
	else
		goto out;

	/* NVMe: 조걸 분기 */
	if (agp3) {
		index += 2; /* NVMe: AGP3 모드 시 속도 등급 보정 */
		/* NVMe: 조걸 분기 */
		if (index == 5)
			index = 0; /* NVMe: 유효하지 않은 조합을 UNKNOWN으로 */
	}

 out:
	return agp_speeds[index]; /* NVMe: AGP 속도 enum 반환 */
}

/*
 * pci_set_bus_speed() - 버스의 최대/현재 속도를 bridge capability에서 읽음
 *
 * NVMe 연결: NVMe SSD가 연결된 PCIe 링크의 속도는 DMA throughput과
 * 직결된다. 이 함수는 AGP/PCI-X/PCIe capability를 순회하며 버스 속도를
 * 결정. PCIe의 경우 LNKCAP/LNKSTA를 통해 link speed를 읽는다.
 */
/* NVMe: 함수 정의: pci_set_bus_speed */
static void pci_set_bus_speed(struct pci_bus *bus)
{
	struct pci_dev *bridge = bus->self; /* NVMe: 이 버스를 만든 bridge 장치 */
	/* NVMe: 변수 선언/초기화 */
	int pos;

	pos = pci_find_capability(bridge, PCI_CAP_ID_AGP); /* NVMe: AGP capability 위치 탐색 */
	/* NVMe: 조걸 분기 */
	if (!pos)
		pos = pci_find_capability(bridge, PCI_CAP_ID_AGP3); /* NVMe: AGP3 capability 탐색 */
	/* NVMe: 조걸 분기 */
	if (pos) {
		/* NVMe: 변수 선언/초기화 */
		u32 agpstat, agpcmd;

		pci_read_config_dword(bridge, pos + PCI_AGP_STATUS, &agpstat); /* NVMe: AGP 상태 레지스터 읽기 */
		bus->max_bus_speed = agp_speed(agpstat & 8, agpstat & 7); /* NVMe: AGP 최대 속도 산출 */

		pci_read_config_dword(bridge, pos + PCI_AGP_COMMAND, &agpcmd); /* NVMe: AGP command 레지스터 읽기 */
		bus->cur_bus_speed = agp_speed(agpstat & 8, agpcmd & 7); /* NVMe: AGP 현재 속도 산출 */
	}

	pos = pci_find_capability(bridge, PCI_CAP_ID_PCIX); /* NVMe: PCI-X capability 위치 탐색 */
	/* NVMe: 조걸 분기 */
	if (pos) {
		/* NVMe: 변수 선언/초기화 */
		u16 status;
		/* NVMe: 변수 선언/초기화 */
		enum pci_bus_speed max;

		/* NVMe: PCI config space 2바이트 읽기 */
		pci_read_config_word(bridge, pos + PCI_X_BRIDGE_SSTATUS,
				     &status); /* NVMe: PCI-X bridge secondary status 읽기 */

		if (status & PCI_X_SSTATUS_533MHZ) { /* NVMe: 533MHz PCI-X */
			/* NVMe: 변수에 값 할당: max */
			max = PCI_SPEED_133MHz_PCIX_533;
		} else if (status & PCI_X_SSTATUS_266MHZ) { /* NVMe: 266MHz PCI-X */
			/* NVMe: 변수에 값 할당: max */
			max = PCI_SPEED_133MHz_PCIX_266;
		} else if (status & PCI_X_SSTATUS_133MHZ) { /* NVMe: 133MHz PCI-X */
			/* NVMe: 조걸 분기 */
			if ((status & PCI_X_SSTATUS_VERS) == PCI_X_SSTATUS_V2)
				/* NVMe: 변수에 값 할당: max */
				max = PCI_SPEED_133MHz_PCIX_ECC;
			/* NVMe: 조걸 분기의 else 경로 */
			else
				/* NVMe: 변수에 값 할당: max */
				max = PCI_SPEED_133MHz_PCIX;
		/* NVMe: 코드 동작 수행 */
		} else {
			max = PCI_SPEED_66MHz_PCIX; /* NVMe: 기본 66MHz PCI-X */
		}

		bus->max_bus_speed = max; /* NVMe: PCI-X 최대 속도 기록 */
		/* NVMe: 구조체 필드에 값 저장: bus->cur_bus_speed */
		bus->cur_bus_speed =
			pcix_bus_speed[FIELD_GET(PCI_X_SSTATUS_FREQ, status)]; /* NVMe: PCI-X 현재 속도 기록 */

		return;
	}

	if (pci_is_pcie(bridge)) { /* NVMe: PCIe bridge이면 LNKCAP/LNKSTA 기반 속도 설정; NVMe 링크 속도 결정 */
		/* NVMe: 변수 선언/초기화 */
		u32 linkcap;

		/* NVMe: PCIe capability 4바이트 읽기 */
		pcie_capability_read_dword(bridge, PCI_EXP_LNKCAP, &linkcap);
		/* NVMe: 구조체 필드에 비트 마스크 적용: bus->max_bus_speed */
		bus->max_bus_speed = pcie_link_speed[linkcap & PCI_EXP_LNKCAP_SLS];

		/* NVMe: 현재 링크 속도 갱신 */
		pcie_update_link_speed(bus, PCIE_ADD_BUS);
	}
}

/*
 * pci_host_bridge_msi_domain() - host bridge에 연결된 MSI/MSI-X IRQ domain 탐색
 *
 * NVMe 연결: NVMe 컨트롤러의 per-queue MSI-X 인터럽트는 이 IRQ domain을
 * 통해 Linux irq 번호로 매핑. Interrupt remapping, vCPU affinity, IRQ
 * delivery 모드가 domain에 의해 결정되므로 NVMe I/O completion 지연과
 * 관련.
 */
/* NVMe: 호스트 브리지 MSI 도메인 획득 */
static struct irq_domain *pci_host_bridge_msi_domain(struct pci_bus *bus)
{
	/* NVMe: 코드 동작 수행 */
	struct irq_domain *d;

	/* If the host bridge driver sets a MSI domain of the bridge, use it */
	d = dev_get_msi_domain(bus->bridge); /* NVMe: host bridge device에 직접 등록된 MSI domain 우선 사용 */

	/*
	 * Any firmware interface that can resolve the msi_domain
	 * should be called from here.
	 */
	/* NVMe: 조걸 분기 */
	if (!d)
		d = pci_host_bridge_of_msi_domain(bus); /* NVMe: device tree에서 MSI domain 탐색 */
	/* NVMe: 조걸 분기 */
	if (!d)
		d = pci_host_bridge_acpi_msi_domain(bus); /* NVMe: ACPI MADT/MSI mapping에서 domain 탐색 */

	/*
	 * If no IRQ domain was found via the OF tree, try looking it up
	 * directly through the fwnode_handle.
	 */
	/* NVMe: 조걸 분기 */
	if (!d) {
		struct fwnode_handle *fwnode = pci_root_bus_fwnode(bus); /* NVMe: root bus의 firmware node 획득 */

		/* NVMe: 조걸 분기 */
		if (fwnode)
			/* NVMe: fwnode로 IRQ domain 검색 */
			d = irq_find_matching_fwnode(fwnode,
						     DOMAIN_BUS_PCI_MSI); /* NVMe: firmware node로부터 PCI MSI domain 검색 */
	}

	return d; /* NVMe: 찾은 MSI IRQ domain 반환; 없으면 NULL */
}

/*
 * pci_set_bus_msi_domain() - 버스 device에 MSI domain 연결
 *
 * NVMe 연결: NVMe 컨트롤러가 연결된 버스의 MSI domain을 결정. SR-IOV
 * virtual bus를 포함해 상위 bridge의 MSI domain을 따라 올라가고, 없으면
 * host bridge의 domain을 상속.
 */
/* NVMe: 함수 정의: pci_set_bus_msi_domain */
static void pci_set_bus_msi_domain(struct pci_bus *bus)
{
	/* NVMe: 코드 동작 수행 */
	struct irq_domain *d;
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *b;

	/*
	 * The bus can be a root bus, a subordinate bus, or a virtual bus
	 * created by an SR-IOV device.  Walk up to the first bridge device
	 * found or derive the domain from the host bridge.
	 */
	for (b = bus, d = NULL; !d && !pci_is_root_bus(b); b = b->parent) { /* NVMe: root bus에 도달하거나 MSI domain을 찾을 때까지 상위로 이동 */
		/* NVMe: 조걸 분기 */
		if (b->self)
			d = dev_get_msi_domain(&b->self->dev); /* NVMe: bridge device에 설정된 MSI domain 확인; NVMe VF의 경우 PF 경로 */
	}

	/* NVMe: 조걸 분기 */
	if (!d)
		d = pci_host_bridge_msi_domain(b); /* NVMe: 상위에서 domain을 찾지 못하면 host bridge의 domain 사용 */

	dev_set_msi_domain(&bus->dev, d); /* NVMe: 버스 device의 MSI domain 설정; 이 버스의 NVMe 장치가 상속 */
}

/*
 * pci_preserve_config() - firmware가 설정한 PCI 리소스를 그대로 유지할지 결정
 *
 * NVMe 연결: true이면 kernel이 BAR/버스 번호를 재할당하지 않고 firmware
 * 설정을 존중. NVMe BAR의 물리 주소가 부팅 후 변경되지 않으므로
 * nvme_probe에서 보는 pci_resource_start() 값이 firmware가 배치한 값과
 * 동일.
 */
/* NVMe: 함수 정의: pci_preserve_config */
static bool pci_preserve_config(struct pci_host_bridge *host_bridge)
{
	if (pci_acpi_preserve_config(host_bridge)) /* NVMe: ACPI _OSC 등에서 config preserve 요청 시 true */
		/* NVMe: 참 반환 */
		return true;

	/* NVMe: 조걸 분기 */
	if (host_bridge->dev.parent && host_bridge->dev.parent->of_node)
		return of_pci_preserve_config(host_bridge->dev.parent->of_node); /* NVMe: device tree에서 preserve 속성 확인 */

	return false; /* NVMe: 기본적으로 firmware 설정을 유지하지 않고 kernel이 재할당 가능 */
}

/*
 * pci_register_host_bridge() - host bridge를 PCI 코어에 등록하고 root bus 생성
 *
 * NVMe 연결: NVMe 컨트롤러가 연결될 root bus가 여기서 만들어지고,
 * bridge의 windows(MEM/IO/BUS 리소스)가 bus resources로 추가된다.
 * MSI domain, NUMA node, preserve_config 등 NVMe 동작에 영향을 주는
 * 설정이 이루어진다.
 */
/* NVMe: 함수 정의: pci_register_host_bridge */
static int pci_register_host_bridge(struct pci_host_bridge *bridge)
{
	/* NVMe: 변수에 값 할당: struct device *parent */
	struct device *parent = bridge->dev.parent;
	/* NVMe: 코드 동작 수행 */
	struct resource_entry *window, *next, *n;
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *bus, *b;
	/* NVMe: 변수 선언/초기화 */
	resource_size_t offset, next_offset;
	/* NVMe: 함수 호출: LIST_HEAD */
	LIST_HEAD(resources);
	/* NVMe: 코드 동작 수행 */
	struct resource *res, *next_res;
	/* NVMe: 변수 선언/초기화 */
	bool bus_registered = false;
	/* NVMe: 코드 동작 수행 */
	char addr[64], *fmt;
	/* NVMe: 코드 동작 수행 */
	const char *name;
	/* NVMe: 변수 선언/초기화 */
	int err;

	bus = pci_alloc_bus(NULL); /* NVMe: root bus용 pci_bus 할당 */
	if (!bus) /* NVMe: 메모리 부족 시 -ENOMEM */
		/* NVMe: 오류 코드 반환: -ENOMEM */
		return -ENOMEM;

	bridge->bus = bus; /* NVMe: host bridge가 생성한 root bus 연결 */

	bus->sysdata = bridge->sysdata; /* NVMe: platform-specific sysdata 연결 */
	bus->ops = bridge->ops; /* NVMe: PCI config space access ops 연결 */
	bus->number = bus->busn_res.start = bridge->busnr; /* NVMe: root bus 번호 설정 */
/* NVMe: 컴파일 조건: CONFIG_PCI_DOMAINS_GENERIC 정의 시 포함 */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* NVMe: 조걸 분기 */
	if (bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		bus->domain_nr = pci_bus_find_domain_nr(bus, parent); /* NVMe: domain 번호 동적 할당; NUMA/RC별 NVMe 구분 */
	/* NVMe: 조걸 분기의 else 경로 */
	else
		bus->domain_nr = bridge->domain_nr; /* NVMe: 미리 지정된 domain 번호 사용 */
	/* NVMe: 조걸 분기 */
	if (bus->domain_nr < 0) {
		/* NVMe: 변수에 값 할당: err */
		err = bus->domain_nr;
		/* NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;
	}
/* NVMe: 컴파일 조건 종료 */
#endif

	/* NVMe: 버스 검색 */
	b = pci_find_bus(pci_domain_nr(bus), bridge->busnr);
	/* NVMe: 조걸 분기 */
	if (b) {
		/* Ignore it if we already got here via a different bridge */
		dev_dbg(&b->dev, "bus already known\n"); /* NVMe: 동일 domain/bus의 중복 bridge이면 무시 */
		/* NVMe: 변수에 값 할당: err */
		err = -EEXIST;
		/* NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;
	}

	/* NVMe: 장치 이름 설정 */
	dev_set_name(&bridge->dev, "pci%04x:%02x", pci_domain_nr(bus),
		     bridge->busnr); /* NVMe: host bridge device 이름 설정 */

	err = pcibios_root_bridge_prepare(bridge); /* NVMe: 아키텍처별 root bridge 준비 */
	/* NVMe: 조걸 분기 */
	if (err)
		/* NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;

	/* Temporarily move resources off the list */
	list_splice_init(&bridge->windows, &resources); /* NVMe: bridge window를 임시 리스트로 이동 */
	err = device_add(&bridge->dev); /* NVMe: host bridge device를 device model에 추가 */
	/* NVMe: 조걸 분기 */
	if (err)
		/* NVMe: 오류 처리/종료 지점으로 이동: free */
		goto free;

	bus->bridge = get_device(&bridge->dev); /* NVMe: bus가 host bridge device를 참조 */
	device_enable_async_suspend(bus->bridge); /* NVMe: 비동기 suspend 활성화 */
	pci_set_bus_of_node(bus); /* NVMe: device tree node 연결 */
	pci_set_bus_msi_domain(bus); /* NVMe: root bus의 MSI domain 설정; 하위 NVMe 장치 상속 */
	/* NVMe: 조걸 분기 */
	if (bridge->msi_domain && !dev_get_msi_domain(&bus->dev) &&
	    /* NVMe: OF MSI map 존재 여부 확인 */
	    !pci_host_of_has_msi_map(parent))
		bus->bus_flags |= PCI_BUS_FLAGS_NO_MSI; /* NVMe: MSI domain이 없으면 bus에 MSI 불가 표시; NVMe MSI-X 사용 불가 */

	/* NVMe: 조걸 분기 */
	if (!parent)
		set_dev_node(bus->bridge, pcibus_to_node(bus)); /* NVMe: NUMA node 설정; NVMe locality에 영향 */

	bus->dev.class = &pcibus_class; /* NVMe: bus device class 설정 */
	bus->dev.parent = bus->bridge; /* NVMe: bus device의 부모를 host bridge로 */

	dev_set_name(&bus->dev, "%04x:%02x", pci_domain_nr(bus), bus->number); /* NVMe: bus device 이름 설정; 예: 0000:00 */
	name = dev_name(&bus->dev); /* NVMe: 설정된 이름 포인터 획득 */

	err = device_register(&bus->dev); /* NVMe: bus device 등록 */
	bus_registered = true; /* NVMe: bus 등록 완료 플래그 설정 */
	/* NVMe: 조걸 분기 */
	if (err)
		/* NVMe: 오류 처리/종료 지점으로 이동: unregister */
		goto unregister;

	pcibios_add_bus(bus); /* NVMe: 아키텍처별 bus 추가 처리 */

	/* NVMe: 조걸 분기 */
	if (bus->ops->add_bus) {
		err = bus->ops->add_bus(bus); /* NVMe: platform-specific bus 추가 콜백 */
		/* NVMe: 조걸 분기 */
		if (WARN_ON(err < 0))
			/* NVMe: 장치 오류 메시지 출력 */
			dev_err(&bus->dev, "failed to add bus: %d\n", err);
	}

	/* Create legacy_io and legacy_mem files for this bus */
	pci_create_legacy_files(bus); /* NVMe: 레거시 I/O/MEM sysfs 파일 생성 */

	/* NVMe: 조걸 분기 */
	if (parent)
		dev_info(parent, "PCI host bridge to bus %s\n", name); /* NVMe: parent device에 host bridge 정보 출력 */
	/* NVMe: 조걸 분기의 else 경로 */
	else
		pr_info("PCI host bridge to bus %s\n", name); /* NVMe: parent 없이 host bridge 정보 출력 */

	/* NVMe: 조걸 분기 */
	if (nr_node_ids > 1 && pcibus_to_node(bus) == NUMA_NO_NODE)
		dev_warn(&bus->dev, "Unknown NUMA node; performance will be reduced\n"); /* NVMe: NUMA node 미지정 시 성능 경고; NVMe latency에 영향 */

	/* Check if the boot configuration by FW needs to be preserved */
	bridge->preserve_config = pci_preserve_config(bridge); /* NVMe: firmware config 유지 여부 결정; NVMe BAR 재할당 여부 */

	/* Coalesce contiguous windows */
	resource_list_for_each_entry_safe(window, n, &resources) { /* NVMe: 인접한 address window를 병합 */
		if (list_is_last(&window->node, &resources)) /* NVMe: 마지막 entry이면 더 병합할 것이 없음 */
			break;

		next = list_next_entry(window, node); /* NVMe: 다음 window entry 획득 */
		offset = window->offset; /* NVMe: 현재 window의 CPU↔bus 오프셋 */
		res = window->res; /* NVMe: 현재 window의 resource */
		next_offset = next->offset; /* NVMe: 다음 window의 오프셋 */
		next_res = next->res; /* NVMe: 다음 window의 resource */

		if (res->flags != next_res->flags || offset != next_offset) /* NVMe: type이나 오프셋이 다륾면 병합 불가 */
			continue;

		if (res->end + 1 == next_res->start) { /* NVMe: 인접하면 두 window를 하나로 병합 */
			/* NVMe: 구조체 필드에 값 저장: next_res->start */
			next_res->start = res->start;
			/* NVMe: 구조체 필드에 값 저장: res->flags */
			res->flags = res->start = res->end = 0;
		}
	}

	/* Add initial resources to the bus */
	resource_list_for_each_entry_safe(window, n, &resources) { /* NVMe: 임시 리스트의 window를 bus resources로 이동 */
		/* NVMe: 변수에 값 할당: offset */
		offset = window->offset;
		/* NVMe: 변수에 값 할당: res */
		res = window->res;
		if (!res->flags && !res->start && !res->end) { /* NVMe: 병합으로 제거된 빈 entry는 해제 */
			/* NVMe: 리소스 해제 */
			release_resource(res);
			/* NVMe: 함수 호출: resource_list_destroy_entry */
			resource_list_destroy_entry(window);
			continue;
		}

		list_move_tail(&window->node, &bridge->windows); /* NVMe: 유효한 window를 bridge->windows로 복원 */

		/* NVMe: 조걸 분기 */
		if (res->flags & IORESOURCE_BUS)
			pci_bus_insert_busn_res(bus, bus->number, res->end); /* NVMe: BUS 리소스이면 버스 번호 범위 등록 */
		/* NVMe: 조걸 분기의 else 경로 */
		else
			pci_bus_add_resource(bus, res); /* NVMe: MEM/IO 리소스를 bus resources에 추가; NVMe BAR 할당 기준 */

		/* NVMe: 조걸 분기 */
		if (offset) {
			/* NVMe: 조걸 분기 */
			if (resource_type(res) == IORESOURCE_IO)
				/* NVMe: 변수에 값 할당: fmt */
				fmt = " (bus address [%#06llx-%#06llx])";
			/* NVMe: 조걸 분기의 else 경로 */
			else
				/* NVMe: 변수에 값 할당: fmt */
				fmt = " (bus address [%#010llx-%#010llx])";

			/* NVMe: 문자열 포맷 출력 */
			snprintf(addr, sizeof(addr), fmt,
				 /* NVMe: 코드 동작 수행 */
				 (unsigned long long)(res->start - offset),
				 (unsigned long long)(res->end - offset)); /* NVMe: bus 주소 범위를 문자열로 변환 */
		/* NVMe: 코드 동작 수행 */
		} else
			addr[0] = '\0'; /* NVMe: 오프셋이 0이면 bus 주주 문자열 생략 */

		dev_info(&bus->dev, "root bus resource %pR%s\n", res, addr); /* NVMe: root bus 리소스 정보 로깅 */
	}

	of_pci_make_host_bridge_node(bridge); /* NVMe: device tree에서 host bridge node 생성/연결 */

	down_write(&pci_bus_sem); /* NVMe: root bus 리스트 쓰기 잠금 */
	list_add_tail(&bus->node, &pci_root_buses); /* NVMe: 전역 root bus 리스트에 추가 */
	up_write(&pci_bus_sem); /* NVMe: 잠금 해제 */

	/* NVMe: 성공 반환 */
	return 0;

unregister:
	/* NVMe: 장치 레퍼런스 감소 */
	put_device(&bridge->dev);
	/* NVMe: 장치 제거 */
	device_del(&bridge->dev);
free:
/* NVMe: 컴파일 조건: CONFIG_PCI_DOMAINS_GENERIC 정의 시 포함 */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* NVMe: 조걸 분기 */
	if (bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		pci_bus_release_domain_nr(parent, bus->domain_nr); /* NVMe: 동적 할당 domain 번호 반납 */
/* NVMe: 컴파일 조건 종료 */
#endif
	/* NVMe: 조걸 분기 */
	if (bus_registered)
		put_device(&bus->dev); /* NVMe: 등록된 bus device 참조 감소 */
	/* NVMe: 조걸 분기의 else 경로 */
	else
		kfree(bus); /* NVMe: 등록되지 않았으면 메모리 직접 해제 */

	/* NVMe: 결과 반환: err */
	return err;
}

/*
 * pci_bridge_child_ext_cfg_accessible() - 브리지 하위 버스에서 4KB extended
 *                                          config space 접근 가능 여부 판단
 *
 * NVMe 연결: NVMe PCIe 컨트롤러는 4KB extended config space에 MSI-X,
 * AER, LTR 등의 capability를 가진다. 이 함수가 false를 반환하면 하위
 * 버스의 장치들은 256B standard config space만 접근 가능하며, NVMe
 * MSI-X capability(offset 0x100 이상)를 읽을 수 없게 된다.
 */
/* NVMe: 함수 정의: pci_bridge_child_ext_cfg_accessible */
static bool pci_bridge_child_ext_cfg_accessible(struct pci_dev *bridge)
{
	/* NVMe: 변수 선언/초기화 */
	int pos;
	/* NVMe: 변수 선언/초기화 */
	u32 status;

	/*
	 * If extended config space isn't accessible on a bridge's primary
	 * bus, we certainly can't access it on the secondary bus.
	 */
	if (bridge->bus->bus_flags & PCI_BUS_FLAGS_NO_EXTCFG) /* NVMe: 상위 버스에서 extended config가 불가능하면 하위도 불가 */
		/* NVMe: 거짓 반환 */
		return false;

	/*
	 * PCIe Root Ports and switch ports are PCIe on both sides, so if
	 * extended config space is accessible on the primary, it's also
	 * accessible on the secondary.
	 */
	if (pci_is_pcie(bridge) && /* NVMe: PCIe bridge이고 */
	    (pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: Root Port이거나 */
	     pci_pcie_type(bridge) == PCI_EXP_TYPE_UPSTREAM || /* NVMe: Switch Upstream Port이거나 */
	     pci_pcie_type(bridge) == PCI_EXP_TYPE_DOWNSTREAM)) /* NVMe: Switch Downstream Port이면 */
		return true; /* NVMe: extended config space 접근 가능; NVMe MSI-X/AER/LTR 접근 가능 */

	/*
	 * For the other bridge types:
	 *   - PCI-to-PCI bridges
	 *   - PCIe-to-PCI/PCI-X forward bridges
	 *   - PCI/PCI-X-to-PCIe reverse bridges
	 * extended config space on the secondary side is only accessible
	 * if the bridge supports PCI-X Mode 2.
	 */
	pos = pci_find_capability(bridge, PCI_CAP_ID_PCIX); /* NVMe: PCI-X capability 탐색 */
	/* NVMe: 조걸 분기 */
	if (!pos)
		/* NVMe: 거짓 반환 */
		return false;

	pci_read_config_dword(bridge, pos + PCI_X_STATUS, &status); /* NVMe: PCI-X status 레지스터 읽기 */
	return status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ); /* NVMe: PCI-X 266/533MHz 지원 시 extended config 접근 가능 */
}

/*
 * pci_alloc_child_bus() - parent bus 아래 새로운 하위 PCI 버스 할당
 *
 * NVMe 연결: NVMe SSD가 연결될 수 있는 하위 PCIe 버스(예: Root Port
 * 아래의 bus)가 여기서 생성. ops, bus_flags, MSI domain, NUMA node,
 * extended config 접근성 등을 부모로부터 상속받는다.
 */
/* NVMe: 자식 버스 할당 */
static struct pci_bus *pci_alloc_child_bus(struct pci_bus *parent,
					   /* NVMe: 코드 동작 수행 */
					   struct pci_dev *bridge, int busnr)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *child;
	/* NVMe: 코드 동작 수행 */
	struct pci_host_bridge *host;
	/* NVMe: 변수 선언/초기화 */
	int i;
	/* NVMe: 변수 선언/초기화 */
	int ret;

	/* Allocate a new bus and inherit stuff from the parent */
	child = pci_alloc_bus(parent); /* NVMe: 부모 bus로부터 상속하여 자식 bus 할당 */
	if (!child) /* NVMe: 할당 실패 시 NULL 반환 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	child->parent = parent; /* NVMe: 부모 bus 포인터 설정 */
	child->sysdata = parent->sysdata; /* NVMe: platform sysdata 상속 */
	child->bus_flags = parent->bus_flags; /* NVMe: bus flags 상속; NO_MSI/NO_EXTCFG 등 NVMe 기능 제한 전파 */

	host = pci_find_host_bridge(parent); /* NVMe: 부모 버스의 host bridge 획득 */
	/* NVMe: 조걸 분기 */
	if (host->child_ops)
		child->ops = host->child_ops; /* NVMe: host bridge가 지정한 child_ops 사용 */
	/* NVMe: 조걸 분기의 else 경로 */
	else
		child->ops = parent->ops; /* NVMe: 부모 bus의 config access ops 상속 */

	/*
	 * Initialize some portions of the bus device, but don't register
	 * it now as the parent is not properly set up yet.
	 */
	child->dev.class = &pcibus_class; /* NVMe: bus device class 설정 */
	dev_set_name(&child->dev, "%04x:%02x", pci_domain_nr(child), busnr); /* NVMe: 자식 bus device 이름 설정; 예: 0000:01 */

	/* Set up the primary, secondary and subordinate bus numbers */
	child->number = child->busn_res.start = busnr; /* NVMe: secondary bus 번호 설정 */
	child->primary = parent->busn_res.start; /* NVMe: primary bus 번호 설정 */
	child->busn_res.end = 0xff; /* NVMe: subordinate bus 번호를 최대 0xff로 초기화 */

	if (!bridge) { /* NVMe: bridge가 없으면 root bus의 가상 자식으로 취급 */
		/* NVMe: 구조체 필드에 값 저장: child->dev.parent */
		child->dev.parent = parent->bridge;
		/* NVMe: 오류 처리/종료 지점으로 이동: add_dev */
		goto add_dev;
	}

	child->self = bridge; /* NVMe: 이 버스를 만든 bridge pci_dev 설정 */
	child->bridge = get_device(&bridge->dev); /* NVMe: bridge device 참조 증가 */
	child->dev.parent = child->bridge; /* NVMe: bus device의 부모를 bridge로 설정 */
	pci_set_bus_of_node(child); /* NVMe: device tree node 연결 */
	pci_set_bus_speed(child); /* NVMe: 버스 속도 설정; NVMe 링크 속도 반영 */

	/*
	 * Check whether extended config space is accessible on the child
	 * bus.  Note that we currently assume it is always accessible on
	 * the root bus.
	 */
	if (!pci_bridge_child_ext_cfg_accessible(bridge)) { /* NVMe: extended config 접근 불가 시 */
		child->bus_flags |= PCI_BUS_FLAGS_NO_EXTCFG; /* NVMe: bus flag에 extended config 불가 표시; NVMe MSI-X capability 접근 제한 */
		/* NVMe: 정보 메시지 출력 */
		pci_info(child, "extended config space not accessible\n");
	}

	/* Set up default resource pointers and names */
	for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) { /* NVMe: bridge의 I/O/MEM/PREF MEM resource 포인터 연결 */
		/* NVMe: 구조체 필드에 비트 마스크 적용: child->resource[i] */
		child->resource[i] = &bridge->resource[PCI_BRIDGE_RESOURCES+i];
		/* NVMe: 구조체 필드에 값 저장: child->resource[i]->name */
		child->resource[i]->name = child->name;
	}
	bridge->subordinate = child; /* NVMe: bridge가 이 child bus를 하위로 가리킴 */

add_dev:
	pci_set_bus_msi_domain(child); /* NVMe: 자식 bus의 MSI domain 설정; NVMe MSI-X 상속 */
	ret = device_register(&child->dev); /* NVMe: 자식 bus device 등록 */
	/* NVMe: 조걸 분기 */
	if (WARN_ON(ret < 0)) {
		put_device(&child->dev); /* NVMe: 등록 실패 시 device 참조 감소 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;
	}

	pcibios_add_bus(child); /* NVMe: 아키텍처별 자식 bus 추가 처리 */

	/* NVMe: 조걸 분기 */
	if (child->ops->add_bus) {
		ret = child->ops->add_bus(child); /* NVMe: platform-specific bus 추가 콜백 */
		/* NVMe: 조걸 분기 */
		if (WARN_ON(ret < 0))
			/* NVMe: 장치 오류 메시지 출력 */
			dev_err(&child->dev, "failed to add bus: %d\n", ret);
	}

	/* Create legacy_io and legacy_mem files for this bus */
	pci_create_legacy_files(child); /* NVMe: 레거시 I/O/MEM sysfs 파일 생성 */

	return child; /* NVMe: 초기화된 자식 bus 반환 */
}

/*
 * pci_add_new_bus() - parent bus 아래 새로운 하위 버스를 할당하고 연결
 *
 * NVMe 연결: bridge 뒤의 NVMe SSD가 위치할 하위 버스를 PCI 트리에
 * 추가. pci_bus_sem으로 트리 동시 수정을 보호.
 */
/* NVMe: 새 버스 추가 */
struct pci_bus *pci_add_new_bus(struct pci_bus *parent, struct pci_dev *dev,
				/* NVMe: 코드 동작 수행 */
				int busnr)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *child;

	child = pci_alloc_child_bus(parent, dev, busnr); /* NVMe: 지정 bus 번호로 자식 bus 할당 */
	if (child) { /* NVMe: 할당 성공 시 parent의 children 리스트에 추가 */
		down_write(&pci_bus_sem); /* NVMe: bus tree 쓰기 잠금 */
		list_add_tail(&child->node, &parent->children); /* NVMe: 부모의 하위 버스 리스트에 연결 */
		up_write(&pci_bus_sem); /* NVMe: 잠금 해제 */
	}
	return child; /* NVMe: 생성된 자식 bus 또는 NULL 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_add_new_bus);

/*
 * pci_enable_rrs_sv() - Root Port의 Configuration RRS Software Visibility 활성화
 *
 * NVMe 연결: RRS(Retry Request Status)는 장치가 준비되지 않았을 때
 * config read가 retry됨을 알리는 메커니즘. Software visibility를
 * 활성화하면 OS가 retry 상태를 직접 관찰할 수 있어, NVMe 컨트롤러의
 * config space 접근 대기 시간을 더 정확히 제어할 수 있다.
 */
/* NVMe: 함수 정의: pci_enable_rrs_sv */
static void pci_enable_rrs_sv(struct pci_dev *pdev)
{
	/* NVMe: 변수 선언/초기화 */
	u16 root_cap = 0;

	/* Enable Configuration RRS Software Visibility if supported */
	pcie_capability_read_word(pdev, PCI_EXP_RTCAP, &root_cap); /* NVMe: Root Capabilities 레지스터 읽기 */
	if (root_cap & PCI_EXP_RTCAP_RRS_SV) { /* NVMe: RRS Software Visibility 지원 여부 확인 */
		/* NVMe: PCIe capability 비트 설정 */
		pcie_capability_set_word(pdev, PCI_EXP_RTCTL,
					 PCI_EXP_RTCTL_RRS_SVE); /* NVMe: RRS Software Visibility Enable bit 설정 */
		pdev->config_rrs_sv = 1; /* NVMe: pci_dev에 RRS SV 활성화 표시 */
	}
}

/* NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
					      /* NVMe: 코드 동작 수행 */
					      unsigned int available_buses);

/*
 * pbus_validate_busn() - 하위 버스 번호 범위가 상위 버스 범위 내에 있는지 검증
 *
 * NVMe 연결: NVMe SSD가 연결된 하위 버스의 busn_res가 상위 bridge의
 * secondary/subordinate 범위를 벗어나면 config cycle이 도달하지 못해
 * NVMe 장치에 접근할 수 없게 된다.
 */
/* NVMe: 함수 정의: pbus_validate_busn */
void pbus_validate_busn(struct pci_bus *bus)
{
	struct pci_bus *upstream = bus->parent; /* NVMe: 직계 상위 버스 */
	struct pci_dev *bridge = bus->self; /* NVMe: 이 버스를 만든 bridge 장치 */

	/* Check that all devices are accessible */
	while (upstream->parent) { /* NVMe: root bus에 도달할 때까지 상위 버스를 따라 검증 */
		if ((bus->busn_res.end > upstream->busn_res.end) || /* NVMe: 하위 subordinate가 상위 subordinate보다 크면 오류 */
		    (bus->number > upstream->busn_res.end) || /* NVMe: 하위 secondary가 상위 subordinate보다 크면 오류 */
		    (bus->number < upstream->number) || /* NVMe: 하위 secondary가 상위 secondary보다 작으면 오류 */
		    (bus->busn_res.end < upstream->number)) { /* NVMe: 하위 subordinate가 상위 secondary보다 작으면 오류 */
			/* NVMe: 정보 메시지 출력 */
			pci_info(bridge, "devices behind bridge are unusable because %pR cannot be assigned for them\n",
				 &bus->busn_res); /* NVMe: 범위가 잘못되면 bridge 뒤 장치(NVMe 포함) 사용 불가 경고 */
			break;
		}
		upstream = upstream->parent; /* NVMe: 한 단계 더 상위 버스로 이동 */
	}
}

/**
 * pci_ea_fixed_busnrs() - Read fixed Secondary and Subordinate bus
 * numbers from EA capability.
 * @dev: Bridge
 * @sec: updated with secondary bus number from EA
 * @sub: updated with subordinate bus number from EA
 *
 * If @dev is a bridge with EA capability that specifies valid secondary
 * and subordinate bus numbers, return true with the bus numbers in @sec
 * and @sub.  Otherwise return false.
 */
/*
 * pci_ea_fixed_busnrs() - Enhanced Allocation capability에서 고정된 secondary/
 *                         subordinate bus 번호 읽기
 *
 * NVMe 연결: EA capability는 firmware가 미리 할당한 bus 번호를
 * 알려준다. NVMe SSD가 연결될 하위 버스 번호가 EA에 의해 고정되어
 * 있으면 kernel은 이를 존중하여 할당해야 한다.
 */
/* NVMe: 함수 정의: pci_ea_fixed_busnrs */
bool pci_ea_fixed_busnrs(struct pci_dev *dev, u8 *sec, u8 *sub)
{
	/* NVMe: 변수 선언/초기화 */
	int ea, offset;
	/* NVMe: 변수 선언/초기화 */
	u32 dw;
	/* NVMe: 변수 선언/초기화 */
	u8 ea_sec, ea_sub;

	if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) /* NVMe: bridge가 아니면 bus 번호가 없음 */
		/* NVMe: 거짓 반환 */
		return false;

	/* find PCI EA capability in list */
	ea = pci_find_capability(dev, PCI_CAP_ID_EA); /* NVMe: EA capability offset 탐색 */
	if (!ea) /* NVMe: EA capability가 없으면 고정 bus 번호 없음 */
		/* NVMe: 거짓 반환 */
		return false;

	offset = ea + PCI_EA_FIRST_ENT; /* NVMe: EA의 첫 번째 entry offset 계산 */
	pci_read_config_dword(dev, offset, &dw); /* NVMe: EA entry에서 bus 번호 필드가 포함된 dword 읽기 */
	ea_sec = FIELD_GET(PCI_EA_SEC_BUS_MASK, dw); /* NVMe: secondary bus 번호 추출 */
	ea_sub = FIELD_GET(PCI_EA_SUB_BUS_MASK, dw); /* NVMe: subordinate bus 번호 추출 */
	if (ea_sec  == 0 || ea_sub < ea_sec) /* NVMe: secondary가 0이거나 subordinate이 더 작으면 무효 */
		/* NVMe: 거짓 반환 */
		return false;

	*sec = ea_sec; /* NVMe: 호출자에게 secondary bus 번호 반환 */
	*sub = ea_sub; /* NVMe: 호출자에게 subordinate bus 번호 반환 */
	return true; /* NVMe: 고정 bus 번호 존재 */
}

/*
 * pci_scan_bridge_extend() - Scan buses behind a bridge
 * @bus: Parent bus the bridge is on
 * @dev: Bridge itself
 * @max: Starting subordinate number of buses behind this bridge
 * @available_buses: Total number of buses available for this bridge and
 *		     the devices below. After the minimal bus space has
 *		     been allocated the remaining buses will be
 *		     distributed equally between hotplug-capable bridges.
 * @pass: Either %0 (scan already configured bridges) or %1 (scan bridges
 *        that need to be reconfigured.
 *
 * If it's a bridge, configure it and scan the bus behind it.
 * For CardBus bridges, we don't scan behind as the devices will
 * be handled by the bridge driver itself.
 *
 * We need to process bridges in two passes -- first we scan those
 * already configured by the BIOS and after we are done with all of
 * them, we proceed to assigning numbers to the remaining buses in
 * order to avoid overlaps between old and new bus numbers.
 *
 * Return: New subordinate number covering all buses behind this bridge.
 */
/* NVMe: 브리지 뒤편 버스 확장 스캔 */
static int pci_scan_bridge_extend(struct pci_bus *bus, struct pci_dev *dev,
				  /* NVMe: 변수 선언/초기화 */
				  int max, unsigned int available_buses,
				  /* NVMe: 코드 동작 수행 */
				  int pass)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *child;
	/* NVMe: 변수 선언/초기화 */
	u32 buses;
	/* NVMe: 변수 선언/초기화 */
	u16 bctl;
	/* NVMe: 변수 선언/초기화 */
	u8 primary, secondary, subordinate;
	/* NVMe: 변수 선언/초기화 */
	int broken = 0;
	/* NVMe: 변수 선언/초기화 */
	bool fixed_buses;
	/* NVMe: 변수 선언/초기화 */
	u8 fixed_sec, fixed_sub;
	/* NVMe: 변수 선언/초기화 */
	int next_busnr;

	/*
	 * Make sure the bridge is powered on to be able to access config
	 * space of devices below it.
	 */
	/* NVMe: 런타임 PM 레퍼런스 획득 */
	pm_runtime_get_sync(&dev->dev);

	/* NVMe: PCI config space 4바이트 읽기 */
	pci_read_config_dword(dev, PCI_PRIMARY_BUS, &buses);
	/* NVMe: 함수 호출: FIELD_GET */
	primary = FIELD_GET(PCI_PRIMARY_BUS_MASK, buses);
	/* NVMe: 함수 호출: FIELD_GET */
	secondary = FIELD_GET(PCI_SECONDARY_BUS_MASK, buses);
	/* NVMe: 함수 호출: FIELD_GET */
	subordinate = FIELD_GET(PCI_SUBORDINATE_BUS_MASK, buses);

	/* NVMe: 디버그 메시지 출력 */
	pci_dbg(dev, "scanning [bus %02x-%02x] behind bridge, pass %d\n",
		/* NVMe: 코드 동작 수행 */
		secondary, subordinate, pass);

	/* NVMe: 조걸 분기 */
	if (!primary && (primary != bus->number) && secondary && subordinate) {
		/* NVMe: 경고 메시지 출력 */
		pci_warn(dev, "Primary bus is hard wired to 0\n");
		/* NVMe: 변수에 값 할당: primary */
		primary = bus->number;
	}

	/* Check if setup is sensible at all */
	/* NVMe: 조걸 분기 */
	if (!pass &&
	    /* NVMe: 비트 연산으로 값 설정/마스크: (primary ! */
	    (primary != bus->number || secondary <= bus->number ||
	     /* NVMe: 코드 동작 수행 */
	     secondary > subordinate)) {
		/* NVMe: 정보 메시지 출력 */
		pci_info(dev, "bridge configuration invalid ([bus %02x-%02x]), reconfiguring\n",
			 /* NVMe: 코드 동작 수행 */
			 secondary, subordinate);
		/* NVMe: 변수에 값 할당: broken */
		broken = 1;
	}

	/*
	 * Disable Master-Abort Mode during probing to avoid reporting of
	 * bus errors in some architectures.
	 */
	/* NVMe: PCI config space 2바이트 읽기 */
	pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &bctl);
	/* NVMe: PCI config space 2바이트 쓰기 */
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL,
			      /* NVMe: 코드 동작 수행 */
			      bctl & ~PCI_BRIDGE_CTL_MASTER_ABORT);

	/* NVMe: 조걸 분기 */
	if (pci_is_cardbus_bridge(dev)) {
		/* NVMe: CardBus 브리지 확장 스캔 */
		max = pci_cardbus_scan_bridge_extend(bus, dev, buses, max,
						     /* NVMe: 코드 동작 수행 */
						     available_buses,
						     /* NVMe: 코드 동작 수행 */
						     pass);
		goto out;
	}

	/* NVMe: 조걸 분기 */
	if ((secondary || subordinate) &&
	    /* NVMe: 모든 버스 수동 할당 여부 */
	    !pcibios_assign_all_busses() && !broken) {
		/* NVMe: 변수 선언/초기화 */
		unsigned int cmax, buses;

		/*
		 * Bus already configured by firmware, process it in the
		 * first pass and just note the configuration.
		 */
		/* NVMe: 조걸 분기 */
		if (pass)
			goto out;

		/*
		 * The bus might already exist for two reasons: Either we
		 * are rescanning the bus or the bus is reachable through
		 * more than one bridge. The second case can happen with
		 * the i450NX chipset.
		 */
		/* NVMe: 버스 검색 */
		child = pci_find_bus(pci_domain_nr(bus), secondary);
		/* NVMe: 조걸 분기 */
		if (!child) {
			/* NVMe: 새 버스 추가 */
			child = pci_add_new_bus(bus, dev, secondary);
			/* NVMe: 조걸 분기 */
			if (!child)
				goto out;
			/* NVMe: 구조체 필드에 값 저장: child->primary */
			child->primary = primary;
			/* NVMe: bus 번호 리소스 삽입 */
			pci_bus_insert_busn_res(child, secondary, subordinate);
			/* NVMe: 구조체 필드에 값 저장: child->bridge_ctl */
			child->bridge_ctl = bctl;
		}

		/* NVMe: 변수에 값 할당: buses */
		buses = subordinate - secondary;
		/* NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
		cmax = pci_scan_child_bus_extend(child, buses);
		/* NVMe: 조걸 분기 */
		if (cmax > subordinate)
			/* NVMe: 경고 메시지 출력 */
			pci_warn(dev, "bridge has subordinate %02x but max busn %02x\n",
				 /* NVMe: 코드 동작 수행 */
				 subordinate, cmax);

		/* Subordinate should equal child->busn_res.end */
		/* NVMe: 조걸 분기 */
		if (subordinate > max)
			/* NVMe: 변수에 값 할당: max */
			max = subordinate;
	/* NVMe: 코드 동작 수행 */
	} else {

		/*
		 * We need to assign a number to this bus which we always
		 * do in the second pass.
		 */
		/* NVMe: 조걸 분기 */
		if (!pass) {
			/* NVMe: 조걸 분기 */
			if (pcibios_assign_all_busses() || broken)

				/*
				 * Temporarily disable forwarding of the
				 * configuration cycles on all bridges in
				 * this bus segment to avoid possible
				 * conflicts in the second pass between two
				 * bridges programmed with overlapping bus
				 * ranges.
				 */
				/* NVMe: PCI config space 4바이트 쓰기 */
				pci_write_config_dword(dev, PCI_PRIMARY_BUS,
						       /* NVMe: 코드 동작 수행 */
						       buses & PCI_SEC_LATENCY_TIMER_MASK);
			goto out;
		}

		/* Clear errors */
		/* NVMe: PCI config space 2바이트 쓰기 */
		pci_write_config_word(dev, PCI_STATUS, 0xffff);

		/* Read bus numbers from EA Capability (if present) */
		/* NVMe: 함수 호출: pci_ea_fixed_busnrs */
		fixed_buses = pci_ea_fixed_busnrs(dev, &fixed_sec, &fixed_sub);
		/* NVMe: 조걸 분기 */
		if (fixed_buses)
			/* NVMe: 변수에 값 할당: next_busnr */
			next_busnr = fixed_sec;
		/* NVMe: 조걸 분기의 else 경로 */
		else
			/* NVMe: 변수에 값 할당: next_busnr */
			next_busnr = max + 1;

		/*
		 * Prevent assigning a bus number that already exists.
		 * This can happen when a bridge is hot-plugged, so in this
		 * case we only re-scan this bus.
		 */
		/* NVMe: 버스 검색 */
		child = pci_find_bus(pci_domain_nr(bus), next_busnr);
		/* NVMe: 조걸 분기 */
		if (!child) {
			/* NVMe: 새 버스 추가 */
			child = pci_add_new_bus(bus, dev, next_busnr);
			/* NVMe: 조걸 분기 */
			if (!child)
				goto out;
			/* NVMe: bus 번호 리소스 삽입 */
			pci_bus_insert_busn_res(child, next_busnr,
						/* NVMe: 코드 동작 수행 */
						bus->busn_res.end);
		}
		/* NVMe: 카운터 증감 */
		max++;
		/* NVMe: 조걸 분기 */
		if (available_buses)
			/* NVMe: 카운터 증감 */
			available_buses--;

		/* NVMe: 비트 연산으로 값 설정/마스크: buses */
		buses = (buses & PCI_SEC_LATENCY_TIMER_MASK) |
			/* NVMe: 함수 호출: FIELD_PREP */
			FIELD_PREP(PCI_PRIMARY_BUS_MASK, child->primary) |
			/* NVMe: 함수 호출: FIELD_PREP */
			FIELD_PREP(PCI_SECONDARY_BUS_MASK, child->busn_res.start) |
			/* NVMe: 함수 호출: FIELD_PREP */
			FIELD_PREP(PCI_SUBORDINATE_BUS_MASK, child->busn_res.end);

		/* We need to blast all three values with a single write */
		/* NVMe: PCI config space 4바이트 쓰기 */
		pci_write_config_dword(dev, PCI_PRIMARY_BUS, buses);

		/* NVMe: 구조체 필드에 값 저장: child->bridge_ctl */
		child->bridge_ctl = bctl;
		/* NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
		max = pci_scan_child_bus_extend(child, available_buses);

		/*
		 * Set subordinate bus number to its real value.
		 * If fixed subordinate bus number exists from EA
		 * capability then use it.
		 */
		/* NVMe: 조걸 분기 */
		if (fixed_buses)
			/* NVMe: 변수에 값 할당: max */
			max = fixed_sub;
		/* NVMe: bus 번호 리소스 끝 갱신 */
		pci_bus_update_busn_res_end(child, max);
		/* NVMe: PCI config space 1바이트 쓰기 */
		pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max);
	}
	/* NVMe: 문자열 포맷 출력(길이 제한) */
	scnprintf(child->name, sizeof(child->name), "PCI Bus %04x:%02x",
		  /* NVMe: PCI 도메인 번호 획득 */
		  pci_domain_nr(bus), child->number);

	/* NVMe: 함수 호출: pbus_validate_busn */
	pbus_validate_busn(child);

out:
	/* Clear errors in the Secondary Status Register */
	/* NVMe: PCI config space 2바이트 쓰기 */
	pci_write_config_word(dev, PCI_SEC_STATUS, 0xffff);

	/* NVMe: PCI config space 2바이트 쓰기 */
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, bctl);

	/* NVMe: 런타임 PM 레퍼런스 반납 */
	pm_runtime_put(&dev->dev);

	/* NVMe: 결과 반환: max */
	return max;
}

/*
 * pci_scan_bridge() - Scan buses behind a bridge
 * @bus: Parent bus the bridge is on
 * @dev: Bridge itself
 * @max: Starting subordinate number of buses behind this bridge
 * @pass: Either %0 (scan already configured bridges) or %1 (scan bridges
 *        that need to be reconfigured.
 *
 * If it's a bridge, configure it and scan the bus behind it.
 * For CardBus bridges, we don't scan behind as the devices will
 * be handled by the bridge driver itself.
 *
 * We need to process bridges in two passes -- first we scan those
 * already configured by the BIOS and after we are done with all of
 * them, we proceed to assigning numbers to the remaining buses in
 * order to avoid overlaps between old and new bus numbers.
 *
 * Return: New subordinate number covering all buses behind this bridge.
 */
/*
 * pci_scan_bridge() - bridge 뒤의 버스를 스캔(확장 옵션 없음)
 *
 * NVMe 연결: available_buses=0으로 bridge 뒤를 스캔. NVMe SSD가
 * 연결된 downstream port를 탐색할 때 사용.
 */
/* NVMe: 함수 정의: pci_scan_bridge */
int pci_scan_bridge(struct pci_bus *bus, struct pci_dev *dev, int max, int pass)
{
	return pci_scan_bridge_extend(bus, dev, max, 0, pass); /* NVMe: 추가 available_buses 없이 bridge 스캔 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_scan_bridge);

/*
 * Read interrupt line and base address registers.
 * The architecture-dependent code can tweak these, of course.
 */
/* NVMe: 함수 정의: pci_read_irq */
static void pci_read_irq(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	unsigned char irq;

	/* VFs are not allowed to use INTx, so skip the config reads */
	/* NVMe: 조걸 분기 */
	if (dev->is_virtfn) {
		/* NVMe: 구조체 필드에 값 저장: dev->pin */
		dev->pin = 0;
		/* NVMe: 구조체 필드에 값 저장: dev->irq */
		dev->irq = 0;
		return;
	}

	/* NVMe: PCI config space 1바이트 읽기 */
	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &irq);
	/* NVMe: 구조체 필드에 값 저장: dev->pin */
	dev->pin = irq;
	/* NVMe: 조걸 분기 */
	if (irq)
		/* NVMe: PCI config space 1바이트 읽기 */
		pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &irq);
	/* NVMe: 구조체 필드에 값 저장: dev->irq */
	dev->irq = irq;
}

/* NVMe: 함수 정의: set_pcie_port_type */
void set_pcie_port_type(struct pci_dev *pdev)
{
	/* NVMe: 변수 선언/초기화 */
	int pos;
	/* NVMe: 변수 선언/초기화 */
	u16 reg16;
	/* NVMe: 변수 선언/초기화 */
	u32 reg32;
	/* NVMe: 변수 선언/초기화 */
	int type;
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *parent;

	/* NVMe: PCI capability 위치 탐색 */
	pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	/* NVMe: 조걸 분기 */
	if (!pos)
		return;

	/* NVMe: 구조체 필드에 값 저장: pdev->pcie_cap */
	pdev->pcie_cap = pos;
	/* NVMe: PCI config space 2바이트 읽기 */
	pci_read_config_word(pdev, pos + PCI_EXP_FLAGS, &reg16);
	/* NVMe: 구조체 필드에 값 저장: pdev->pcie_flags_reg */
	pdev->pcie_flags_reg = reg16;

	/* NVMe: PCIe 포트 타입 획득 */
	type = pci_pcie_type(pdev);
	/* NVMe: 조걸 분기 */
	if (type == PCI_EXP_TYPE_ROOT_PORT)
		/* NVMe: Configuration RRS Software Visibility 활성화 */
		pci_enable_rrs_sv(pdev);

	/* NVMe: PCI config space 4바이트 읽기 */
	pci_read_config_dword(pdev, pos + PCI_EXP_DEVCAP, &pdev->devcap);
	/* NVMe: 함수 호출: FIELD_GET */
	pdev->pcie_mpss = FIELD_GET(PCI_EXP_DEVCAP_PAYLOAD, pdev->devcap);

	/* NVMe: PCIe capability 4바이트 읽기 */
	pcie_capability_read_dword(pdev, PCI_EXP_LNKCAP, &reg32);
	/* NVMe: 조걸 분기 */
	if (reg32 & PCI_EXP_LNKCAP_DLLLARC)
		/* NVMe: 구조체 필드에 값 저장: pdev->link_active_reporting */
		pdev->link_active_reporting = 1;

/* NVMe: 컴파일 조건: CONFIG_PCIEASPM 정의 시 포함 */
#ifdef CONFIG_PCIEASPM
	/* NVMe: 조걸 분기 */
	if (reg32 & PCI_EXP_LNKCAP_ASPM_L0S)
		/* NVMe: 구조체 필드에 값 저장: pdev->aspm_l0s_support */
		pdev->aspm_l0s_support = 1;
	/* NVMe: 조걸 분기 */
	if (reg32 & PCI_EXP_LNKCAP_ASPM_L1)
		/* NVMe: 구조체 필드에 값 저장: pdev->aspm_l1_support */
		pdev->aspm_l1_support = 1;
/* NVMe: 컴파일 조건 종료 */
#endif

	/* NVMe: 상위 브리지 획득 */
	parent = pci_upstream_bridge(pdev);
	/* NVMe: 조걸 분기 */
	if (!parent)
		return;

	/*
	 * Some systems do not identify their upstream/downstream ports
	 * correctly so detect impossible configurations here and correct
	 * the port type accordingly.
	 */
	/* NVMe: 조걸 분기 */
	if (type == PCI_EXP_TYPE_DOWNSTREAM) {
		/*
		 * If pdev claims to be downstream port but the parent
		 * device is also downstream port assume pdev is actually
		 * upstream port.
		 */
		/* NVMe: 조걸 분기 */
		if (pcie_downstream_port(parent)) {
			/* NVMe: 정보 메시지 출력 */
			pci_info(pdev, "claims to be downstream port but is acting as upstream port, correcting type\n");
			/* NVMe: 구조체 필드에 비트 마스크 적용: pdev->pcie_flags_reg & */
			pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
			/* NVMe: 구조체 필드에 값 저장: pdev->pcie_flags_reg | */
			pdev->pcie_flags_reg |= PCI_EXP_TYPE_UPSTREAM;
		}
	/* NVMe: 함수 호출: if */
	} else if (type == PCI_EXP_TYPE_UPSTREAM) {
		/*
		 * If pdev claims to be upstream port but the parent
		 * device is also upstream port assume pdev is actually
		 * downstream port.
		 */
		/* NVMe: 조걸 분기 */
		if (pci_pcie_type(parent) == PCI_EXP_TYPE_UPSTREAM) {
			/* NVMe: 정보 메시지 출력 */
			pci_info(pdev, "claims to be upstream port but is acting as downstream port, correcting type\n");
			/* NVMe: 구조체 필드에 비트 마스크 적용: pdev->pcie_flags_reg & */
			pdev->pcie_flags_reg &= ~PCI_EXP_FLAGS_TYPE;
			/* NVMe: 구조체 필드에 값 저장: pdev->pcie_flags_reg | */
			pdev->pcie_flags_reg |= PCI_EXP_TYPE_DOWNSTREAM;
		}
	}
}

/*
 * set_pcie_hotplug_bridge() - PCIe hotplug capability 지원 여부를 표시
 *
 * NVMe 연결: NVMe SSD가 hotplug slot에 연결된 경우(예: U.2/U.3
 * 백플레인), 이 함수가 bridge에 hotplug 플래그를 설정. 이후 PCI
 * hotplug 이벤트가 발생하면 nvme_remove_work 등을 통해 NVMe 드라이버가
 * 정리된다.
 */
/* NVMe: 함수 정의: set_pcie_hotplug_bridge */
void set_pcie_hotplug_bridge(struct pci_dev *pdev)
{
	/* NVMe: 변수 선언/초기화 */
	u32 reg32;

	pcie_capability_read_dword(pdev, PCI_EXP_SLTCAP, &reg32); /* NVMe: Slot Capabilities 레지스터 읽기 */
	if (reg32 & PCI_EXP_SLTCAP_HPC) /* NVMe: Hot-Plug Controller가 slot에 내장되어 있으면 */
		pdev->is_hotplug_bridge = pdev->is_pciehp = 1; /* NVMe: bridge에 hotplug 지원 표시 */
}

/*
 * set_pcie_thunderbolt() - Thunderbolt 컨트롤러 하위 장치 여부 표시
 *
 * NVMe 연결: Thunderbolt 도킹이나 외장 NVMe 케이스가 연결된 경우,
 * 해당 PCIe 장치들은 is_thunderbolt=1로 표시되어 보안/전원 정책이
 * 다르게 적용될 수 있다.
 */
/* NVMe: 함수 정의: set_pcie_thunderbolt */
static void set_pcie_thunderbolt(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u16 vsec;

	/* Is the device part of a Thunderbolt controller? */
	vsec = pci_find_vsec_capability(dev, PCI_VENDOR_ID_INTEL, PCI_VSEC_ID_INTEL_TBT); /* NVMe: Intel Thunderbolt vendor-specific capability 탐색 */
	/* NVMe: 조걸 분기 */
	if (vsec)
		dev->is_thunderbolt = 1; /* NVMe: Thunderbolt 계열 장치로 표시 */
}

/*
 * set_pcie_cxl() - CXL(Compute Express Link) 장치 여부 표시
 *
 * NVMe 연결: CXL 메모리 확장 장치나 CXL 2.0/3.0 기반 NVMe가 연결된
 * 경우 is_cxl 플래그가 설정. 이 플래그는 DMA coherence, 리소스 할당,
 * 전원 관리 정책에 영향을 줄 수 있다.
 */
/* NVMe: 함수 정의: set_pcie_cxl */
static void set_pcie_cxl(struct pci_dev *dev)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *bridge;
	/* NVMe: 변수 선언/초기화 */
	u16 dvsec, cap;

	if (!pci_is_pcie(dev)) /* NVMe: PCIe 장치가 아니면 CXL 불가 */
		return;

	/*
	 * Update parent's CXL state because alternate protocol training
	 * may have changed
	 */
	bridge = pci_upstream_bridge(dev); /* NVMe: 상위 bridge 획득 */
	/* NVMe: 조걸 분기 */
	if (bridge)
		set_pcie_cxl(bridge); /* NVMe: 상위 bridge의 CXL 상태 재귀 갱신 */

	/* NVMe: Designated vendor-specific extended capability 탐색 */
	dvsec = pci_find_dvsec_capability(dev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_FLEXBUS_PORT); /* NVMe: CXL Designated Vendor-Specific Extended Capability 탐색 */
	if (!dvsec) /* NVMe: CXL DVSEC가 없으면 CXL 장치 아님 */
		return;

	/* NVMe: PCI config space 2바이트 읽기 */
	pci_read_config_word(dev, dvsec + PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS,
			     &cap); /* NVMe: CXL Flexbus Port Status 읽기 */

	dev->is_cxl = FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS_CACHE, cap) || /* NVMe: CXL cache 기능 지원 시 */
		FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS_MEM, cap); /* NVMe: CXL memory 기능 지원 시 */

}

/*
 * set_pcie_untrusted() - 외부/제거 가능한 PCIe 장치를 untrusted로 표시
 *
 * NVMe 연결: 외장 NVMe enclosure나 Thunderbolt NVMe 같은 removable
 * 장치는 untrusted로 표시되어 DMA attack 방지를 위한 IOMMU/ATS 정책이
 * 적용될 수 있다.
 */
/* NVMe: 함수 정의: set_pcie_untrusted */
static void set_pcie_untrusted(struct pci_dev *dev)
{
	struct pci_dev *parent = pci_upstream_bridge(dev); /* NVMe: 상위 bridge 장치 획득 */

	if (!parent) /* NVMe: root bus 직접 연결 장치이면 신뢰 여부를 여기서 결정하지 않음 */
		return;
	/*
	 * If the upstream bridge is untrusted we treat this device as
	 * untrusted as well.
	 */
	if (parent->untrusted) { /* NVMe: 상위 bridge가 untrusted이면 상속 */
		/* NVMe: 구조체 필드에 값 저장: dev->untrusted */
		dev->untrusted = true;
		return;
	}

	if (arch_pci_dev_is_removable(dev)) { /* NVMe: 아키텍처에서 제거 가능으로 판단하면 */
		/* NVMe: 디버그 메시지 출력 */
		pci_dbg(dev, "marking as untrusted\n");
		dev->untrusted = true; /* NVMe: untrusted 표시; IOMMU/ATS 정책에 영향 */
	}
}

/*
 * pci_set_removable() - 사용자가 제거할 수 있는 PCIe 장치로 표시
 *
 * NVMe 연결: 외장 NVMe 케이스, Thunderbolt NVMe 등은 removable로
 * 표시되어 userspace(udev 등)가 이를 인식할 수 있다. 이는 eject
 * 처리와 보안 정책에 사용.
 */
/* NVMe: 함수 정의: pci_set_removable */
static void pci_set_removable(struct pci_dev *dev)
{
	struct pci_dev *parent = pci_upstream_bridge(dev); /* NVMe: 상위 bridge 장치 획득 */

	if (!parent) /* NVMe: root 직접 연결 장치는 여기서 처리하지 않음 */
		return;
	/*
	 * We (only) consider everything tunneled below an external_facing
	 * device to be removable by the user. We're mainly concerned with
	 * consumer platforms with user accessible thunderbolt ports that are
	 * vulnerable to DMA attacks, and we expect those ports to be marked by
	 * the firmware as external_facing. Devices in traditional hotplug
	 * slots can technically be removed, but the expectation is that unless
	 * the port is marked with external_facing, such devices are less
	 * accessible to user / may not be removed by end user, and thus not
	 * exposed as "removable" to userspace.
	 */
	if (dev_is_removable(&parent->dev)) { /* NVMe: 상위 bridge가 external_facing/removable이면 */
		dev_set_removable(&dev->dev, DEVICE_REMOVABLE); /* NVMe: 이 장치도 removable로 표시 */
		return;
	}

	if (arch_pci_dev_is_removable(dev)) { /* NVMe: 아키텍처에서 removable로 판단하면 */
		/* NVMe: 디버그 메시지 출력 */
		pci_dbg(dev, "marking as removable\n");
		dev_set_removable(&dev->dev, DEVICE_REMOVABLE); /* NVMe: removable 속성 설정 */
	}
}

/**
 * pci_ext_cfg_is_aliased - Is ext config space just an alias of std config?
 * @dev: PCI device
 *
 * PCI Express to PCI/PCI-X Bridge Specification, rev 1.0, 4.1.4 says that
 * when forwarding a type1 configuration request the bridge must check that
 * the extended register address field is zero.  The bridge is not permitted
 * to forward the transactions and must handle it as an Unsupported Request.
 * Some bridges do not follow this rule and simply drop the extended register
 * bits, resulting in the standard config space being aliased, every 256
 * bytes across the entire configuration space.  Test for this condition by
 * comparing the first dword of each potential alias to the vendor/device ID.
 * Known offenders:
 *   ASM1083/1085 PCIe-to-PCI Reversible Bridge (1b21:1080, rev 01 & 03)
 *   AMD/ATI SBx00 PCI to PCI Bridge (1002:4384, rev 40)
 */
/*
 * pci_ext_cfg_is_aliased() - extended config space가 standard space의
 *                            alias인지 검출
 *
 * NVMe 연결: 일부 broken bridge는 extended config access(0x100~)를
 * 256B standard config로 잘못 forwarding. 이 경우 NVMe 컨트롤러의
 * MSI-X capability(0x100 이상)가 올바르게 읽히지 않아 MSI-X 인터럽트
 * 초기화가 실패할 수 있다.
 */
/* NVMe: 함수 정의: pci_ext_cfg_is_aliased */
static bool pci_ext_cfg_is_aliased(struct pci_dev *dev)
{
/* NVMe: 컴파일 조건: CONFIG_PCI_QUIRKS 정의 시 포함 */
#ifdef CONFIG_PCI_QUIRKS
	/* NVMe: 변수 선언/초기화 */
	int pos, ret;
	/* NVMe: 변수 선언/초기화 */
	u32 header, tmp;

	pci_read_config_dword(dev, PCI_VENDOR_ID, &header); /* NVMe: standard config의 vendor/device ID 읽기 */

	/* NVMe: 반복문 */
	for (pos = PCI_CFG_SPACE_SIZE;
	     pos < PCI_CFG_SPACE_EXP_SIZE; pos += PCI_CFG_SPACE_SIZE) { /* NVMe: 0x100, 0x200, 0x300 등 extended offset 순회 */
		ret = pci_read_config_dword(dev, pos, &tmp); /* NVMe: extended offset에서 dword 읽기 */
		if ((ret != PCIBIOS_SUCCESSFUL) || (header != tmp)) /* NVMe: 읽기 실패거나 vendor ID와 다륾면 alias 아님 */
			/* NVMe: 거짓 반환 */
			return false;
	}

	return true; /* NVMe: 모든 extended offset이 vendor ID와 동일하면 alias로 판단; extended config 사용 불가 */
/* NVMe: 컴파일 조건: 이전 조건의 반대 경로 */
#else
	return false; /* NVMe: PCI_QUIRKS가 꺼져 있으면 alias 검출 안 함 */
/* NVMe: 컴파일 조건 종료 */
#endif
}

/**
 * pci_cfg_space_size_ext - Get the configuration space size of the PCI device
 * @dev: PCI device
 *
 * Regular PCI devices have 256 bytes, but PCI-X 2 and PCI Express devices
 * have 4096 bytes.  Even if the device is capable, that doesn't mean we can
 * access it.  Maybe we don't have a way to generate extended config space
 * accesses, or the device is behind a reverse Express bridge.  So we try
 * reading the dword at 0x100 which must either be 0 or a valid extended
 * capability header.
 */
/*
 * pci_cfg_space_size_ext() - extended config space 접근 가능성 테스트
 *
 * NVMe 연결: NVMe PCIe 컨트롤러는 4KB config space가 필요(MSI-X, AER,
 * LTR 등). 0x100에서 유효한 extended capability header를 읽을 수 있어야
 * 4KB로 인식. alias나 error response면 256B로 제한되어 NVMe 고급
 * capability 접근이 불가능해진다.
 */
/* NVMe: 함수 정의: pci_cfg_space_size_ext */
static int pci_cfg_space_size_ext(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u32 status;
	int pos = PCI_CFG_SPACE_SIZE; /* NVMe: extended config space 시작 offset 0x100 */

	if (pci_read_config_dword(dev, pos, &status) != PCIBIOS_SUCCESSFUL) /* NVMe: 0x100 dword 읽기 시도 */
		return PCI_CFG_SPACE_SIZE; /* NVMe: 접근 실패 시 256B로 제한; NVMe MSI-X 접근 불가 */
	if (PCI_POSSIBLE_ERROR(status) || pci_ext_cfg_is_aliased(dev)) /* NVMe: error response이거나 alias이면 */
		return PCI_CFG_SPACE_SIZE; /* NVMe: 256B로 제한 */

	return PCI_CFG_SPACE_EXP_SIZE; /* NVMe: 4KB extended config space 사용 가능; NVMe MSI-X/AER/LTR 접근 가능 */
}

/*
 * pci_cfg_space_size() - 장치의 config space 크기(256B 또는 4KB) 결정
 *
 * NVMe 연결: NVMe PCIe 컨트롤러는 PCIe capability를 가지므로
 * pci_cfg_space_size_ext()를 통해 4KB로 인식. 4KB가 확볼되어야
 * pci_init_capabilities()에서 MSI-X, AER, LTR 등의 extended capability를
 * 파싱할 수 있다. SR-IOV VF는 spec상 4KB를 사용.
 */
/* NVMe: 함수 정의: pci_cfg_space_size */
int pci_cfg_space_size(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	int pos;
	/* NVMe: 변수 선언/초기화 */
	u32 status;
	/* NVMe: 변수 선언/초기화 */
	u16 class;

/* NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
#ifdef CONFIG_PCI_IOV
	/*
	 * Per the SR-IOV specification (rev 1.1, sec 3.5), VFs are required to
	 * implement a PCIe capability and therefore must implement extended
	 * config space.  We can skip the NO_EXTCFG test below and the
	 * reachability/aliasing test in pci_cfg_space_size_ext() by virtue of
	 * the fact that the SR-IOV capability on the PF resides in extended
	 * config space and must be accessible and non-aliased to have enabled
	 * support for this VF.  This is a micro performance optimization for
	 * systems supporting many VFs.
	 */
	if (dev->is_virtfn) /* NVMe: SR-IOV VF는 항상 4KB config space 사용; NVMe VF의 MSI-X 접근 보장 */
		/* NVMe: 결과 반환: PCI_CFG_SPACE_EXP_SIZE */
		return PCI_CFG_SPACE_EXP_SIZE;
/* NVMe: 컴파일 조건 종료 */
#endif

	if (dev->bus->bus_flags & PCI_BUS_FLAGS_NO_EXTCFG) /* NVMe: 상위 버스에서 extended config가 불가능하면 */
		return PCI_CFG_SPACE_SIZE; /* NVMe: 256B로 제한; NVMe MSI-X 초기화 실패 가능 */

	class = dev->class >> 8; /* NVMe: class code 상위 3바이트 추출 */
	/* NVMe: 조걸 분기 */
	if (class == PCI_CLASS_BRIDGE_HOST)
		return pci_cfg_space_size_ext(dev); /* NVMe: host bridge도 4KB 가능성 테스트 */

	if (pci_is_pcie(dev)) /* NVMe: PCIe 장치(NVMe 포함)이면 */
		return pci_cfg_space_size_ext(dev); /* NVMe: 4KB 접근 가능성 테스트 */

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* NVMe: PCI-X capability 탐색 */
	/* NVMe: 조걸 분기 */
	if (!pos)
		return PCI_CFG_SPACE_SIZE; /* NVMe: PCI-X capability 없으면 256B */

	pci_read_config_dword(dev, pos + PCI_X_STATUS, &status); /* NVMe: PCI-X status 읽기 */
	if (status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ)) /* NVMe: PCI-X 266/533MHz이면 extended config 가능 */
		return pci_cfg_space_size_ext(dev); /* NVMe: 4KB 테스트 수행 */

	return PCI_CFG_SPACE_SIZE; /* NVMe: 기본 256B config space */
}

/*
 * pci_class() - 장치의 class/revision 코드 읽기
 *
 * NVMe 연결: NVMe 컨트롤러는 class 0x010802(Non-Volatile Memory
 * Controller)를 가진다. 이 값이 pci_setup_device()에서 dev->class에
 * 저장되어 드라이버 매칭(nvme_pci_driver)의 기준이 된다.
 */
/* NVMe: 함수 정의: pci_class */
static u32 pci_class(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u32 class;

/* NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn) /* NVMe: VF는 PF의 SR-IOV 구조체에 저장된 class 사용 */
		/* NVMe: 결과 반환: dev->physfn->sriov->class */
		return dev->physfn->sriov->class;
/* NVMe: 컴파일 조건 종료 */
#endif
	pci_read_config_dword(dev, PCI_CLASS_REVISION, &class); /* NVMe: 설정 공간의 class/revision dword 읽기 */
	return class; /* NVMe: class(상위 3바이트)와 revision(하위 1바이트) 반환 */
}

/*
 * pci_subsystem_ids() - subsystem vendor/device ID 읽기
 *
 * NVMe 연결: 동일 NVMe controller chip이라도 subsystem vendor/device가
 * 다륾면 firmware, thermal, form factor 등이 달라질 수 있다. quirk나
 * 드라이버 매칭에 사용.
 */
/* NVMe: 함수 정의: pci_subsystem_ids */
static void pci_subsystem_ids(struct pci_dev *dev, u16 *vendor, u16 *device)
{
/* NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn) { /* NVMe: VF는 PF의 SR-IOV 구조체에서 subsystem ID 상속 */
		*vendor = dev->physfn->sriov->subsystem_vendor; /* NVMe: subsystem vendor ID */
		*device = dev->physfn->sriov->subsystem_device; /* NVMe: subsystem device ID */
		return;
	}
/* NVMe: 컴파일 조건 종료 */
#endif
	pci_read_config_word(dev, PCI_SUBSYSTEM_VENDOR_ID, vendor); /* NVMe: 설정 공간에서 subsystem vendor ID 읽기 */
	pci_read_config_word(dev, PCI_SUBSYSTEM_ID, device); /* NVMe: 설정 공간에서 subsystem device ID 읽기 */
}

/*
 * pci_hdr_type() - PCI header type 읽기
 *
 * NVMe 연결: NVMe 컨트롤러는 PCI_HEADER_TYPE_NORMAL(0x00) 헤더를
 * 사용하며, 이로 인해 pci_setup_device()에서 standard BAR(6개)와
 * INTx/MSI-X를 파싱한다.
 */
/* NVMe: 함수 정의: pci_hdr_type */
static u8 pci_hdr_type(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u8 hdr_type;

/* NVMe: 컴파일 조건: CONFIG_PCI_IOV 정의 시 포함 */
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn) /* NVMe: VF는 PF의 SR-IOV 구조체에서 header type 상속 */
		/* NVMe: 결과 반환: dev->physfn->sriov->hdr_type */
		return dev->physfn->sriov->hdr_type;
/* NVMe: 컴파일 조건 종료 */
#endif
	pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type); /* NVMe: 설정 공간의 header type 바이트 읽기 */
	return hdr_type; /* NVMe: header type(멀티펑션 비트 포함) 반환 */
}

/* NVMe: 매크로 정의: 레거시 IO 리소스 플래그 */
#define LEGACY_IO_RESOURCE	(IORESOURCE_IO | IORESOURCE_PCI_FIXED)

/**
 * pci_intx_mask_broken - Test PCI_COMMAND_INTX_DISABLE writability
 * @dev: PCI device
 *
 * Test whether PCI_COMMAND_INTX_DISABLE is writable for @dev.  Check this
 * at enumeration-time to avoid modifying PCI_COMMAND at run-time.
 */
/*
 * pci_intx_mask_broken() - PCI_COMMAND_INTX_DISABLE 비트의 쓰기 가능성 테스트
 *
 * NVMe 연결: NVMe는 주로 MSI-X를 사용하지만 INTx disable이 제대로
 * 작동하지 않으면 런타임에 pci_intx() 호출 시 의도치 않은 INTx가 남을
 * 수 있다. 이 함수는 probe 시점에 미리 감지하여 dev->broken_intx_masking
 * 플래그를 설정.
 */
/* NVMe: 함수 정의: pci_intx_mask_broken */
static int pci_intx_mask_broken(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u16 orig, toggle, new;

	pci_read_config_word(dev, PCI_COMMAND, &orig); /* NVMe: 원래 COMMAND 레지스터 값 읽기 */
	toggle = orig ^ PCI_COMMAND_INTX_DISABLE; /* NVMe: INTx disable bit를 toggle한 값 */
	pci_write_config_word(dev, PCI_COMMAND, toggle); /* NVMe: toggle 값 쓰기 */
	pci_read_config_word(dev, PCI_COMMAND, &new); /* NVMe: 실제 쓰인 값 읽기 */

	pci_write_config_word(dev, PCI_COMMAND, orig); /* NVMe: 원래 값 복원 */

	/*
	 * PCI_COMMAND_INTX_DISABLE was reserved and read-only prior to PCI
	 * r2.3, so strictly speaking, a device is not *broken* if it's not
	 * writable.  But we'll live with the misnomer for now.
	 */
	if (new != toggle) /* NVMe: 쓰기가 반영되지 않으면 INTx masking이 broken */
		return 1; /* NVMe: broken_intx_masking = true */
	return 0; /* NVMe: 정상 동작 */
}

/*
 * early_dump_pci_device() - probe 시점에 장치의 standard config space를 덤프
 *
 * NVMe 연결: pci_early_dump 커널 파라미터가 켜져 있으면 NVMe 컨트롤러의
 * 설정 공간을 부팅 초기에 16진수로 출력. 디버깅/ Bring-up 시 유용.
 */
/* NVMe: 함수 정의: early_dump_pci_device */
static void early_dump_pci_device(struct pci_dev *pdev)
{
	/* NVMe: 함수 호출: sizeof */
	u32 value[PCI_CFG_SPACE_SIZE / sizeof(u32)];
	/* NVMe: 변수 선언/초기화 */
	int i;

	pci_info(pdev, "config space:\n"); /* NVMe: config space 덤프 시작 로그 */

	for (i = 0; i < ARRAY_SIZE(value); i++) /* NVMe: 256B를 4바이트 단위로 읽기 */
		pci_read_config_dword(pdev, i * sizeof(u32), &value[i]); /* NVMe: offset i*4에서 dword 읽기 */

	/* NVMe: 16진수 덤프 출력 */
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 1,
		       value, ARRAY_SIZE(value) * sizeof(u32), false); /* NVMe: 읽은 256B를 16진수로 출력 */
}

/*
 * pci_type_str() - PCIe/PCI 장치 유형을 사람이 읽을 수 있는 문자열로 변환
 *
 * NVMe 연결: NVMe 컨트롤러는 일반적으로 "PCIe Endpoint"로 인쇄. 로그를
 * 통해 NVMe 장치가 정상적으로 PCIe Endpoint로 인식되었는지 확인 가능.
 */
/* NVMe: 장치 타입 문자열 반환 */
static const char *pci_type_str(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	static const char * const str[] = {
		/* NVMe: 코드 동작 수행 */
		"PCIe Endpoint",
		/* NVMe: 코드 동작 수행 */
		"PCIe Legacy Endpoint",
		/* NVMe: 코드 동작 수행 */
		"PCIe unknown",
		/* NVMe: 코드 동작 수행 */
		"PCIe unknown",
		/* NVMe: 코드 동작 수행 */
		"PCIe Root Port",
		/* NVMe: 코드 동작 수행 */
		"PCIe Switch Upstream Port",
		/* NVMe: 코드 동작 수행 */
		"PCIe Switch Downstream Port",
		/* NVMe: 코드 동작 수행 */
		"PCIe to PCI/PCI-X bridge",
		/* NVMe: 코드 동작 수행 */
		"PCI/PCI-X to PCIe bridge",
		/* NVMe: 코드 동작 수행 */
		"PCIe Root Complex Integrated Endpoint",
		/* NVMe: 코드 동작 수행 */
		"PCIe Root Complex Event Collector",
	};
	/* NVMe: 변수 선언/초기화 */
	int type;

	if (pci_is_pcie(dev)) { /* NVMe: PCIe 장치이면 */
		type = pci_pcie_type(dev); /* NVMe: PCIe capability의 device/port type 필드 읽기 */
		/* NVMe: 조걸 분기 */
		if (type < ARRAY_SIZE(str))
			return str[type]; /* NVMe: NVMe 컨트롤러는 보통 여기서 "PCIe Endpoint" 반환 */

		/* NVMe: 결과 반환: "PCIe unknown" */
		return "PCIe unknown";
	}

	switch (dev->hdr_type) { /* NVMe: PCIe가 아닌 legacy PCI 장치 유형 */
	/* NVMe: case 분기: PCI_HEADER_TYPE_NORMAL */
	case PCI_HEADER_TYPE_NORMAL:
		/* NVMe: 결과 반환: "conventional PCI endpoint" */
		return "conventional PCI endpoint";
	/* NVMe: case 분기: PCI_HEADER_TYPE_BRIDGE */
	case PCI_HEADER_TYPE_BRIDGE:
		/* NVMe: 결과 반환: "conventional PCI bridge" */
		return "conventional PCI bridge";
	/* NVMe: case 분기: PCI_HEADER_TYPE_CARDBUS */
	case PCI_HEADER_TYPE_CARDBUS:
		/* NVMe: 결과 반환: "CardBus bridge" */
		return "CardBus bridge";
	default:
		/* NVMe: 결과 반환: "conventional PCI" */
		return "conventional PCI";
	}
}

/**
 * pci_setup_device - Fill in class and map information of a device
 * @dev: the device structure to fill
 *
 * Initialize the device structure with information about the device's
 * vendor,class,memory and IO-space addresses, IRQ lines etc.
 * Called at initialisation of the PCI subsystem and by CardBus services.
 * Returns 0 on success and negative if unknown type of device (not normal,
 * bridge or CardBus).
 */
/* NVMe: 함수 정의: pci_setup_device */
int pci_setup_device(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u32 class;
	/* NVMe: 변수 선언/초기화 */
	u16 cmd;
	/* NVMe: 변수 선언/초기화 */
	u8 hdr_type;
	/* NVMe: 변수 선언/초기화 */
	int err, pos = 0;
	/* NVMe: 변수 선언/초기화 */
	struct pci_bus_region region;
	/* NVMe: 코드 동작 수행 */
	struct resource *res;

	/* NVMe: 헤더 타입 읽기 */
	hdr_type = pci_hdr_type(dev);

	/* NVMe: 구조체 필드에 값 저장: dev->sysdata */
	dev->sysdata = dev->bus->sysdata;
	/* NVMe: 구조체 필드에 값 저장: dev->dev.parent */
	dev->dev.parent = dev->bus->bridge;
	/* NVMe: 구조체 필드에 비트 마스크 적용: dev->dev.bus */
	dev->dev.bus = &pci_bus_type;
	/* NVMe: 함수 호출: FIELD_GET */
	dev->hdr_type = FIELD_GET(PCI_HEADER_TYPE_MASK, hdr_type);
	/* NVMe: 함수 호출: FIELD_GET */
	dev->multifunction = FIELD_GET(PCI_HEADER_TYPE_MFD, hdr_type);
	/* NVMe: 구조체 필드에 값 저장: dev->error_state */
	dev->error_state = pci_channel_io_normal;
	/* NVMe: PCIe 포트 타입 설정 */
	set_pcie_port_type(dev);

	/* NVMe: OF node 연결 */
	err = pci_set_of_node(dev);
	/* NVMe: 조걸 분기 */
	if (err)
		/* NVMe: 결과 반환: err */
		return err;
	/* NVMe: ACPI fwnode 연결 */
	pci_set_acpi_fwnode(dev);

	/* NVMe: PCI 슬롯 할당 */
	pci_dev_assign_slot(dev);

	/*
	 * Assume 32-bit PCI; let 64-bit PCI cards (which are far rarer)
	 * set this higher, assuming the system even supports it.
	 */
	/* NVMe: 구조체 필드에 값 저장: dev->dma_mask */
	dev->dma_mask = 0xffffffff;

	/*
	 * Assume 64-bit addresses for MSI initially. Will be changed to 32-bit
	 * if MSI (rather than MSI-X) capability does not have
	 * PCI_MSI_FLAGS_64BIT. Can also be overridden by driver.
	 */
	/* NVMe: 함수 호출: DMA_BIT_MASK */
	dev->msi_addr_mask = DMA_BIT_MASK(64);

	/* NVMe: 장치 이름 설정 */
	dev_set_name(&dev->dev, "%04x:%02x:%02x.%d", pci_domain_nr(dev->bus),
		     /* NVMe: 함수 호출: PCI_SLOT */
		     dev->bus->number, PCI_SLOT(dev->devfn),
		     /* NVMe: 함수 호출: PCI_FUNC */
		     PCI_FUNC(dev->devfn));

	/* NVMe: Class Code/Revision 읽기 */
	class = pci_class(dev);

	/* NVMe: 구조체 필드에 비트 마스크 적용: dev->revision */
	dev->revision = class & 0xff;
	dev->class = class >> 8;		    /* upper 3 bytes */

	/* NVMe: 조걸 분기 */
	if (pci_early_dump)
		/* NVMe: 초기 config space 덤프 */
		early_dump_pci_device(dev);

	/* Need to have dev->class ready */
	/* NVMe: config space 크기 결정 */
	dev->cfg_size = pci_cfg_space_size(dev);

	/* Need to have dev->cfg_size ready */
	/* NVMe: Thunderbolt 장치 표시 */
	set_pcie_thunderbolt(dev);

	/* NVMe: CXL 장치 표시 */
	set_pcie_cxl(dev);

	/* NVMe: 신뢰할 수 없는 장치 표시 */
	set_pcie_untrusted(dev);

	/* NVMe: 조걸 분기 */
	if (pci_is_pcie(dev))
		/* NVMe: 지원 링크 속도 획득 */
		dev->supported_speeds = pcie_get_supported_speeds(dev);

	/* "Unknown power state" */
	/* NVMe: 구조체 필드에 값 저장: dev->current_state */
	dev->current_state = PCI_UNKNOWN;

	/* Early fixups, before probing the BARs */
	/* NVMe: 장치 quirk/workaround 적용 */
	pci_fixup_device(pci_fixup_early, dev);

	/* NVMe: 제거 가능 장치 표시 */
	pci_set_removable(dev);

	/* NVMe: 정보 메시지 출력 */
	pci_info(dev, "[%04x:%04x] type %02x class %#08x %s\n",
		 /* NVMe: 코드 동작 수행 */
		 dev->vendor, dev->device, dev->hdr_type, dev->class,
		 /* NVMe: 장치 타입 문자열 반환 */
		 pci_type_str(dev));

	/* Device class may be changed after fixup */
	/* NVMe: 비트 연산으로 값 설정/마스크: class */
	class = dev->class >> 8;

	/* NVMe: 조걸 분기 */
	if (dev->non_compliant_bars && !dev->mmio_always_on) {
		/* NVMe: PCI config space 2바이트 읽기 */
		pci_read_config_word(dev, PCI_COMMAND, &cmd);
		/* NVMe: 조걸 분기 */
		if (cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {
			/* NVMe: 정보 메시지 출력 */
			pci_info(dev, "device has non-compliant BARs; disabling IO/MEM decoding\n");
			/* NVMe: 비트 연산으로 값 설정/마스크: cmd & */
			cmd &= ~PCI_COMMAND_IO;
			/* NVMe: 비트 연산으로 값 설정/마스크: cmd & */
			cmd &= ~PCI_COMMAND_MEMORY;
			/* NVMe: PCI config space 2바이트 쓰기 */
			pci_write_config_word(dev, PCI_COMMAND, cmd);
		}
	}

	/* NVMe: INTx 마스크 쓰기 가능 여부 검사 */
	dev->broken_intx_masking = pci_intx_mask_broken(dev);

	switch (dev->hdr_type) {		    /* header type */
	case PCI_HEADER_TYPE_NORMAL:		    /* standard header */
		/* NVMe: 조걸 분기 */
		if (class == PCI_CLASS_BRIDGE_PCI)
			/* NVMe: 오류 처리/종료 지점으로 이동: bad */
			goto bad;
		/* NVMe: INTx 인터럽트 라인 읽기 */
		pci_read_irq(dev);
		/* NVMe: 표준 BAR 읽기 */
		pci_read_bases(dev, PCI_STD_NUM_BARS, PCI_ROM_ADDRESS);

		/* NVMe: 서브시스템 ID 읽기 */
		pci_subsystem_ids(dev, &dev->subsystem_vendor, &dev->subsystem_device);

		/*
		 * Do the ugly legacy mode stuff here rather than broken chip
		 * quirk code. Legacy mode ATA controllers have fixed
		 * addresses. These are not always echoed in BAR0-3, and
		 * BAR0-3 in a few cases contain junk!
		 */
		/* NVMe: 조걸 분기 */
		if (class == PCI_CLASS_STORAGE_IDE) {
			/* NVMe: 변수 선언/초기화 */
			u8 progif;
			/* NVMe: PCI config space 1바이트 읽기 */
			pci_read_config_byte(dev, PCI_CLASS_PROG, &progif);
			/* NVMe: 조걸 분기 */
			if ((progif & 1) == 0) {
				/* NVMe: 구조체 필드에 값 저장: region.start */
				region.start = 0x1F0;
				/* NVMe: 구조체 필드에 값 저장: region.end */
				region.end = 0x1F7;
				/* NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[0];
				/* NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* NVMe: 버스 주소를 CPU 물리 주소로 변환 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* NVMe: 정보 메시지 출력 */
				pci_info(dev, "BAR 0 %pR: legacy IDE quirk\n",
					 /* NVMe: 코드 동작 수행 */
					 res);
				/* NVMe: 구조체 필드에 값 저장: region.start */
				region.start = 0x3F6;
				/* NVMe: 구조체 필드에 값 저장: region.end */
				region.end = 0x3F6;
				/* NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[1];
				/* NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* NVMe: 버스 주소를 CPU 물리 주소로 변환 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* NVMe: 정보 메시지 출력 */
				pci_info(dev, "BAR 1 %pR: legacy IDE quirk\n",
					 /* NVMe: 코드 동작 수행 */
					 res);
			}
			/* NVMe: 조걸 분기 */
			if ((progif & 4) == 0) {
				/* NVMe: 구조체 필드에 값 저장: region.start */
				region.start = 0x170;
				/* NVMe: 구조체 필드에 값 저장: region.end */
				region.end = 0x177;
				/* NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[2];
				/* NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* NVMe: 버스 주소를 CPU 물리 주소로 변환 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* NVMe: 정보 메시지 출력 */
				pci_info(dev, "BAR 2 %pR: legacy IDE quirk\n",
					 /* NVMe: 코드 동작 수행 */
					 res);
				/* NVMe: 구조체 필드에 값 저장: region.start */
				region.start = 0x376;
				/* NVMe: 구조체 필드에 값 저장: region.end */
				region.end = 0x376;
				/* NVMe: 비트 연산으로 값 설정/마스크: res */
				res = &dev->resource[3];
				/* NVMe: 구조체 필드에 값 저장: res->flags */
				res->flags = LEGACY_IO_RESOURCE;
				/* NVMe: 버스 주소를 CPU 물리 주소로 변환 */
				pcibios_bus_to_resource(dev->bus, res, &region);
				/* NVMe: 정보 메시지 출력 */
				pci_info(dev, "BAR 3 %pR: legacy IDE quirk\n",
					 /* NVMe: 코드 동작 수행 */
					 res);
			}
		}
		break;

	case PCI_HEADER_TYPE_BRIDGE:		    /* bridge header */
		/*
		 * The PCI-to-PCI bridge spec requires that subtractive
		 * decoding (i.e. transparent) bridge must have programming
		 * interface code of 0x01.
		 */
		/* NVMe: INTx 인터럽트 라인 읽기 */
		pci_read_irq(dev);
		/* NVMe: 구조체 필드에 비트 마스크 적용: dev->transparent */
		dev->transparent = ((dev->class & 0xff) == 1);
		/* NVMe: 표준 BAR 읽기 */
		pci_read_bases(dev, 2, PCI_ROM_ADDRESS1);
		/* NVMe: 브리지 윈도우 일괄 읽기 */
		pci_read_bridge_windows(dev);
		/* NVMe: 핫플러그 브리지 플래그 설정 */
		set_pcie_hotplug_bridge(dev);
		/* NVMe: PCI capability 위치 탐색 */
		pos = pci_find_capability(dev, PCI_CAP_ID_SSVID);
		/* NVMe: 조걸 분기 */
		if (pos) {
			/* NVMe: PCI config space 2바이트 읽기 */
			pci_read_config_word(dev, pos + PCI_SSVID_VENDOR_ID, &dev->subsystem_vendor);
			/* NVMe: PCI config space 2바이트 읽기 */
			pci_read_config_word(dev, pos + PCI_SSVID_DEVICE_ID, &dev->subsystem_device);
		}
		break;

	case PCI_HEADER_TYPE_CARDBUS:		    /* CardBus bridge header */
		/* NVMe: 조걸 분기 */
		if (class != PCI_CLASS_BRIDGE_CARDBUS)
			/* NVMe: 오류 처리/종료 지점으로 이동: bad */
			goto bad;
		/* NVMe: INTx 인터럽트 라인 읽기 */
		pci_read_irq(dev);
		/* NVMe: 표준 BAR 읽기 */
		pci_read_bases(dev, 1, 0);
		/* NVMe: PCI config space 2바이트 읽기 */
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_VENDOR_ID, &dev->subsystem_vendor);
		/* NVMe: PCI config space 2바이트 읽기 */
		pci_read_config_word(dev, PCI_CB_SUBSYSTEM_ID, &dev->subsystem_device);
		break;

	default:				    /* unknown header */
		/* NVMe: 오류 메시지 출력 */
		pci_err(dev, "unknown header type %02x, ignoring device\n",
			/* NVMe: 코드 동작 수행 */
			dev->hdr_type);
		/* NVMe: OF node 레퍼런스 해제 */
		pci_release_of_node(dev);
		/* NVMe: 오류 코드 반환: -EIO */
		return -EIO;

	bad:
		/* NVMe: 오류 메시지 출력 */
		pci_err(dev, "ignoring class %#08x (doesn't match header type %02x)\n",
			/* NVMe: 코드 동작 수행 */
			dev->class, dev->hdr_type);
		/* NVMe: 구조체 필드에 비트 마스크 적용: dev->class */
		dev->class = PCI_CLASS_NOT_DEFINED << 8;
	}

	/* We found a fine healthy device, go go go... */
	/* NVMe: 성공 반환 */
	return 0;
}

/* NVMe: 함수 정의: pci_configure_mps */
static void pci_configure_mps(struct pci_dev *dev)
{
	/* NVMe: 상위 브리지 획득 */
	struct pci_dev *bridge = pci_upstream_bridge(dev);
	/* NVMe: 변수 선언/초기화 */
	int mps, mpss, p_mps, rc;

	/* NVMe: 조걸 분기 */
	if (!pci_is_pcie(dev))
		return;

	/* MPS and MRRS fields are of type 'RsvdP' for VFs, short-circuit out */
	/* NVMe: 조걸 분기 */
	if (dev->is_virtfn)
		return;

	/*
	 * For Root Complex Integrated Endpoints, program the maximum
	 * supported value unless limited by the PCIE_BUS_PEER2PEER case.
	 */
	/* NVMe: 조걸 분기 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) {
		/* NVMe: 조걸 분기 */
		if (pcie_bus_config == PCIE_BUS_PEER2PEER)
			/* NVMe: 변수에 값 할당: mps */
			mps = 128;
		/* NVMe: 조걸 분기의 else 경로 */
		else
			/* NVMe: 비트 연산으로 값 설정/마스크: mps */
			mps = 128 << dev->pcie_mpss;
		/* NVMe: MPS 레지스터 쓰기 */
		rc = pcie_set_mps(dev, mps);
		/* NVMe: 조걸 분기 */
		if (rc) {
			/* NVMe: 경고 메시지 출력 */
			pci_warn(dev, "can't set Max Payload Size to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
				 /* NVMe: 코드 동작 수행 */
				 mps);
		}
		return;
	}

	/* NVMe: 조걸 분기 */
	if (!bridge || !pci_is_pcie(bridge))
		return;

	/* NVMe: MPS 레지스터 읽기 */
	mps = pcie_get_mps(dev);
	/* NVMe: MPS 레지스터 읽기 */
	p_mps = pcie_get_mps(bridge);

	/* NVMe: 조걸 분기 */
	if (mps == p_mps)
		return;

	/* NVMe: 조걸 분기 */
	if (pcie_bus_config == PCIE_BUS_TUNE_OFF) {
		/* NVMe: 경고 메시지 출력 */
		pci_warn(dev, "Max Payload Size %d, but upstream %s set to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
			 /* NVMe: PCI 장치 이름 획득 */
			 mps, pci_name(bridge), p_mps);
		return;
	}

	/*
	 * Fancier MPS configuration is done later by
	 * pcie_bus_configure_settings()
	 */
	/* NVMe: 조걸 분기 */
	if (pcie_bus_config != PCIE_BUS_DEFAULT)
		return;

	/* NVMe: 비트 연산으로 값 설정/마스크: mpss */
	mpss = 128 << dev->pcie_mpss;
	/* NVMe: 조걸 분기 */
	if (mpss < p_mps && pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT) {
		/* NVMe: MPS 레지스터 쓰기 */
		pcie_set_mps(bridge, mpss);
		/* NVMe: 정보 메시지 출력 */
		pci_info(dev, "Upstream bridge's Max Payload Size set to %d (was %d, max %d)\n",
			 /* NVMe: 코드 동작 수행 */
			 mpss, p_mps, 128 << bridge->pcie_mpss);
		/* NVMe: MPS 레지스터 읽기 */
		p_mps = pcie_get_mps(bridge);
	}

	/* NVMe: MPS 레지스터 쓰기 */
	rc = pcie_set_mps(dev, p_mps);
	/* NVMe: 조걸 분기 */
	if (rc) {
		/* NVMe: 경고 메시지 출력 */
		pci_warn(dev, "can't set Max Payload Size to %d; if necessary, use \"pci=pcie_bus_safe\" and report a bug\n",
			 /* NVMe: 코드 동작 수행 */
			 p_mps);
		return;
	}

	/* NVMe: 정보 메시지 출력 */
	pci_info(dev, "Max Payload Size set to %d (was %d, max %d)\n",
		 /* NVMe: 코드 동작 수행 */
		 p_mps, mps, mpss);
}

/* NVMe: 함수 정의: pci_configure_extended_tags */
int pci_configure_extended_tags(struct pci_dev *dev, void *ign)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_host_bridge *host;
	/* NVMe: 변수 선언/초기화 */
	u32 cap;
	/* NVMe: 변수 선언/초기화 */
	u16 ctl;
	/* NVMe: 변수 선언/초기화 */
	int ret;

	/* PCI_EXP_DEVCTL_EXT_TAG is RsvdP in VFs */
	/* NVMe: 조걸 분기 */
	if (!pci_is_pcie(dev) || dev->is_virtfn)
		/* NVMe: 성공 반환 */
		return 0;

	/* NVMe: PCIe capability 4바이트 읽기 */
	ret = pcie_capability_read_dword(dev, PCI_EXP_DEVCAP, &cap);
	/* NVMe: 조걸 분기 */
	if (ret)
		/* NVMe: 성공 반환 */
		return 0;

	/* NVMe: 조걸 분기 */
	if (!(cap & PCI_EXP_DEVCAP_EXT_TAG))
		/* NVMe: 성공 반환 */
		return 0;

	/* NVMe: PCIe capability 2바이트 읽기 */
	ret = pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);
	/* NVMe: 조걸 분기 */
	if (ret)
		/* NVMe: 성공 반환 */
		return 0;

	/* NVMe: 호스트 브리지 검색 */
	host = pci_find_host_bridge(dev->bus);
	/* NVMe: 조걸 분기 */
	if (!host)
		/* NVMe: 성공 반환 */
		return 0;

	/*
	 * If some device in the hierarchy doesn't handle Extended Tags
	 * correctly, make sure they're disabled.
	 */
	/* NVMe: 조걸 분기 */
	if (host->no_ext_tags) {
		/* NVMe: 조걸 분기 */
		if (ctl & PCI_EXP_DEVCTL_EXT_TAG) {
			/* NVMe: 정보 메시지 출력 */
			pci_info(dev, "disabling Extended Tags\n");
			/* NVMe: PCIe capability 비트 클리어 */
			pcie_capability_clear_word(dev, PCI_EXP_DEVCTL,
						   /* NVMe: 코드 동작 수행 */
						   PCI_EXP_DEVCTL_EXT_TAG);
		}
		/* NVMe: 성공 반환 */
		return 0;
	}

	/* NVMe: 조걸 분기 */
	if (!(ctl & PCI_EXP_DEVCTL_EXT_TAG)) {
		/* NVMe: 정보 메시지 출력 */
		pci_info(dev, "enabling Extended Tags\n");
		/* NVMe: PCIe capability 비트 설정 */
		pcie_capability_set_word(dev, PCI_EXP_DEVCTL,
					 /* NVMe: 코드 동작 수행 */
					 PCI_EXP_DEVCTL_EXT_TAG);
	}
	/* NVMe: 성공 반환 */
	return 0;
}

/*
 * pci_dev3_init() - PCI Express Device 3(PCIe 6.0+) capability 초기화
 *
 * NVMe 연결: Device 3 capability는 FLIT 모드, Retry buffer 등 PCIe 6.0
 * 이상의 기능을 노출. FLIT 모드는 NVMe PCIe 6.0 SSD의 데이터 링크
 * 효율에 영향을 줄 수 있다(추정).
 */
/* NVMe: 함수 정의: pci_dev3_init */
static void pci_dev3_init(struct pci_dev *pdev)
{
	u16 cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DEV3); /* NVMe: Device 3 extended capability 탐색 */
	/* NVMe: 변수 선언/초기화 */
	u32 val = 0;

	if (!cap) /* NVMe: capability가 없으면 초기화 불필요 */
		return;
	pci_read_config_dword(pdev, cap + PCI_DEV3_STA, &val); /* NVMe: Device 3 Status 레지스터 읽기 */
	pdev->fm_enabled = !!(val & PCI_DEV3_STA_SEGMENT); /* NVMe: FLIT 모드 segment 지원 여부를 pci_dev에 기록 */
}

/**
 * pcie_relaxed_ordering_enabled - Probe for PCIe relaxed ordering enable
 * @dev: PCI device to query
 *
 * Returns true if the device has enabled relaxed ordering attribute.
 */
/*
 * pcie_relaxed_ordering_enabled() - 장치의 Relaxed Ordering 활성화 여부 반환
 *
 * NVMe 연결: NVMe queue 간 순서 독립성이 높아 RO(Relaxed Ordering)를
 * 활성화하면 링크 사용률을 높일 수 있다. 이 함수는 다른 드라이버나
 * quirk에서 RO 상태를 확인할 때 사용.
 */
/* NVMe: 함수 정의: pcie_relaxed_ordering_enabled */
bool pcie_relaxed_ordering_enabled(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u16 v;

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &v); /* NVMe: Device Control 레지스터 읽기 */

	return !!(v & PCI_EXP_DEVCTL_RELAX_EN); /* NVMe: Relaxed Ordering Enable bit 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pcie_relaxed_ordering_enabled);

/*
 * pci_configure_relaxed_ordering() - Root Port가 RO를 지원하지 않으면
 *                                     endpoint의 RO를 비활성화
 *
 * NVMe 연결: 일부 Root Port는 Relaxed Ordering을 제대로 지원하지 않아
 * 데이터 무결성 문제를 일으킬 수 있다. NVMe endpoint의 RO를 비활성화하여
 * 안정성을 확보.
 */
/* NVMe: 함수 정의: pci_configure_relaxed_ordering */
static void pci_configure_relaxed_ordering(struct pci_dev *dev)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *root;

	/* PCI_EXP_DEVCTL_RELAX_EN is RsvdP in VFs */
	if (dev->is_virtfn) /* NVMe: VF는 RO bit가 예약되어 있으므로 건드리지 않음 */
		return;

	if (!pcie_relaxed_ordering_enabled(dev)) /* NVMe: 이미 RO가 꺼져 있으면 할 것 없음 */
		return;

	/*
	 * For now, we only deal with Relaxed Ordering issues with Root
	 * Ports. Peer-to-Peer DMA is another can of worms.
	 */
	root = pcie_find_root_port(dev); /* NVMe: NVMe 장치의 Root Port 탐색 */
	if (!root) /* NVMe: Root Port를 찾을 수 없으면(가상화 등) 처리 불가 */
		return;

	if (root->dev_flags & PCI_DEV_FLAGS_NO_RELAXED_ORDERING) { /* NVMe: Root Port가 RO 미지원으로 표시되어 있으면 */
		/* NVMe: PCIe capability 비트 클리어 */
		pcie_capability_clear_word(dev, PCI_EXP_DEVCTL,
					   PCI_EXP_DEVCTL_RELAX_EN); /* NVMe: NVMe endpoint의 RO 비활성화 */
		/* NVMe: 정보 메시지 출력 */
		pci_info(dev, "Relaxed Ordering disabled because the Root Port didn't support it\n");
	}
}

/*
 * pci_configure_eetlp_prefix() - End-to-End TLP Prefix 지원 크기 설정
 *
 * NVMe 연결: EETLP Prefix는 PCIe 3.1 이상의 고급 기능으로, TLP에
 * 추가 메타데이터를 담을 수 있다. NVMe와 직접 관련은 적으나, prefix
 * 지원 여부는 향상된 에러 보고나 보안 확장에 사용될 수 있다.
 */
/* NVMe: 함수 정의: pci_configure_eetlp_prefix */
static void pci_configure_eetlp_prefix(struct pci_dev *dev)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *bridge;
	/* NVMe: 변수 선언/초기화 */
	unsigned int eetlp_max;
	/* NVMe: 변수 선언/초기화 */
	int pcie_type;
	/* NVMe: 변수 선언/초기화 */
	u32 cap;

	if (!pci_is_pcie(dev)) /* NVMe: PCIe 장치가 아니면 EETLP prefix 불가 */
		return;

	pcie_capability_read_dword(dev, PCI_EXP_DEVCAP2, &cap); /* NVMe: Device Capabilities 2 레지스터 읽기 */
	if (!(cap & PCI_EXP_DEVCAP2_EE_PREFIX)) /* NVMe: EETLP Prefix 지원 bit 확인 */
		return;

	pcie_type = pci_pcie_type(dev); /* NVMe: 장치의 PCIe type 확인 */

	eetlp_max = FIELD_GET(PCI_EXP_DEVCAP2_EE_PREFIX_MAX, cap); /* NVMe: 지원하는 prefix 최대 개수 필드 추출 */
	/* 00b means 4 */
	eetlp_max = eetlp_max ?: 4; /* NVMe: 0이면 최대 4개로 해석 */

	/* NVMe: 조걸 분기 */
	if (pcie_type == PCI_EXP_TYPE_ROOT_PORT ||
	    pcie_type == PCI_EXP_TYPE_RC_END) /* NVMe: Root Port나 RCiE이면 자신의 prefix 크기 설정 */
		/* NVMe: 구조체 필드에 값 저장: dev->eetlp_prefix_max */
		dev->eetlp_prefix_max = eetlp_max;
	/* NVMe: 조걸 분기의 else 경로 */
	else {
		bridge = pci_upstream_bridge(dev); /* NVMe: 상위 bridge 획득 */
		if (bridge && bridge->eetlp_prefix_max) /* NVMe: 상위 bridge가 prefix를 지원하면 endpoint도 설정 */
			/* NVMe: 구조체 필드에 값 저장: dev->eetlp_prefix_max */
			dev->eetlp_prefix_max = eetlp_max;
	}
}

/*
 * pci_configure_serr() - bridge의 SERR# forwarding 활성화
 *
 * NVMe 연결: bridge가 SERR# forwarding을 하지 않으면 NVMe endpoint에서
 * 발생한 PCIe fatal/non-fatal error(ERR_COR/ERR_NONFATAL/ERR_FATAL)가
 * Root Complex에 도달하지 못해 AER 인터럽트가 발생하지 않는다.
 */
/* NVMe: 함수 정의: pci_configure_serr */
static void pci_configure_serr(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u16 control;

	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) { /* NVMe: bridge 장치에만 적용 */

		/*
		 * A bridge will not forward ERR_ messages coming from an
		 * endpoint unless SERR# forwarding is enabled.
		 */
		pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &control); /* NVMe: Bridge Control 레지스터 읽기 */
		if (!(control & PCI_BRIDGE_CTL_SERR)) { /* NVMe: SERR forwarding이 꺼져 있으면 */
			control |= PCI_BRIDGE_CTL_SERR; /* NVMe: SERR forwarding enable bit 설정 */
			pci_write_config_word(dev, PCI_BRIDGE_CONTROL, control); /* NVMe: Bridge Control에 기록 */
		}
	}
}

/*
 * pci_configure_rcb() - Read Completion Boundary를 Root Port와 일치시킴
 *
 * NVMe 연결: RCB는 Memory Read completion을 정렬하는 단위(64B 또는
 * 128B). NVMe DMA read 완료 데이터의 정렬 방식이 RCB에 따라 달라질 수
 * 있어, endpoint의 RCB를 Root Port와 맞추는 것이 바람직하다.
 */
/* NVMe: 함수 정의: pci_configure_rcb */
static void pci_configure_rcb(struct pci_dev *dev)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *rp;
	/* NVMe: 변수 선언/초기화 */
	u16 rp_lnkctl;

	/*
	 * Per PCIe r7.0, sec 7.5.3.7, RCB is only meaningful in Root Ports
	 * (where it is read-only), Endpoints, and Bridges.  It may only be
	 * set for Endpoints and Bridges if it is set in the Root Port. For
	 * Endpoints, it is 'RsvdP' for Virtual Functions.
	 */
	if (!pci_is_pcie(dev) || /* NVMe: PCIe 장치가 아니면 RCB 의미 없음 */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: Root Port의 RCB는 read-only */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_UPSTREAM || /* NVMe: Switch upstream은 RCB 설정 대상 아님 */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM || /* NVMe: Switch downstream은 RCB 설정 대상 아님 */
	    pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC || /* NVMe: RC Event Collector 제외 */
	    dev->is_virtfn) /* NVMe: VF는 RCB bit가 RsvdP */
		return;

	/* Root Port often not visible to virtualized guests */
	rp = pcie_find_root_port(dev); /* NVMe: NVMe 장치의 Root Port 탐색 */
	if (!rp) /* NVMe: 가상화 등에서 Root Port를 찾지 못하면 처리 불가 */
		return;

	pcie_capability_read_word(rp, PCI_EXP_LNKCTL, &rp_lnkctl); /* NVMe: Root Port의 Link Control 읽기 */
	/* NVMe: PCIe capability 비트 클리어 후 설정 */
	pcie_capability_clear_and_set_word(dev, PCI_EXP_LNKCTL,
					   /* NVMe: 코드 동작 수행 */
					   PCI_EXP_LNKCTL_RCB,
					   /* NVMe: 코드 동작 수행 */
					   (rp_lnkctl & PCI_EXP_LNKCTL_RCB) ?
					   PCI_EXP_LNKCTL_RCB : 0); /* NVMe: Root Port의 RCB 값을 NVMe endpoint에 동기화 */
}

/* NVMe: 함수 정의: pci_configure_device */
static void pci_configure_device(struct pci_dev *dev)
{
	/* NVMe: Max Payload Size 설정 */
	pci_configure_mps(dev);
	/* NVMe: Extended Tag 활성화 */
	pci_configure_extended_tags(dev, NULL);
	/* NVMe: Relaxed Ordering 설정 */
	pci_configure_relaxed_ordering(dev);
	/* NVMe: Latency Tolerance Reporting 설정 */
	pci_configure_ltr(dev);
	/* NVMe: ASPM L1 Substates 설정 */
	pci_configure_aspm_l1ss(dev);
	/* NVMe: End-to-End TLP Prefix 설정 */
	pci_configure_eetlp_prefix(dev);
	/* NVMe: SERR 포워딩 설정 */
	pci_configure_serr(dev);
	/* NVMe: Read Completion Boundary 설정 */
	pci_configure_rcb(dev);

	/* NVMe: ACPI 핫플러그 파라미터 적용 */
	pci_acpi_program_hp_params(dev);
}

/*
 * pci_release_capabilities() - pci_dev의 PCI/PCIe 고급 capability 정리
 *
 * NVMe 연결: NVMe 컨트롤러가 제거되거나 해제될 때 AER, SR-IOV, capability
 * save buffer 등을 정리. MSI-X는 pci_disable_msix() 등에서 별도 처리.
 */
/* NVMe: 함수 정의: pci_release_capabilities */
static void pci_release_capabilities(struct pci_dev *dev)
{
	pci_aer_exit(dev); /* NVMe: AER 콜백 및 자원 정리; NVMe 오류 보고 중단 */
	pci_rcec_exit(dev); /* NVMe: Root Complex Event Collector 관련 정리 */
	pci_iov_release(dev); /* NVMe: SR-IOV 자원 반납; NVMe VF 정리 */
	pci_free_cap_save_buffers(dev); /* NVMe: suspend/resume용 capability 저장 버퍼 해제 */
}

/**
 * pci_release_dev - Free a PCI device structure when all users of it are
 *		     finished
 * @dev: device that's been disconnected
 *
 * Will be called only by the device core when all users of this PCI device are
 * done.
 */
/*
 * pci_release_dev() - pci_dev의 최종 해제
 *
 * NVMe 연결: NVMe 컨트롤러의 struct device 참조 카운트가 0이 되면
 * 호출. capability, OF node, DMA alias bitmap, bus 참조 등을 정리하고
 * pci_dev 메모리를 해제.
 */
/* NVMe: 함수 정의: pci_release_dev */
static void pci_release_dev(struct device *dev)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *pci_dev;

	pci_dev = to_pci_dev(dev); /* NVMe: device에서 pci_dev 구조체 획득 */
	pci_release_capabilities(pci_dev); /* NVMe: AER/SR-IOV/cap save buffer 정리 */
	pci_release_of_node(pci_dev); /* NVMe: device tree node 참조 해제 */
	pcibios_release_device(pci_dev); /* NVMe: 아키텍처별 device 해제 처리 */
	pci_bus_put(pci_dev->bus); /* NVMe: bus 참조 카운트 감소 */
	bitmap_free(pci_dev->dma_alias_mask); /* NVMe: DMA alias bitmap 해제; NVMe DMA alias 정리 */
	dev_dbg(dev, "device released\n"); /* NVMe: device 해제 로그 */
	kfree(pci_dev); /* NVMe: pci_dev 메모리 해제 */
}

/* NVMe: 변수 선언/초기화 */
static const struct device_type pci_dev_type = {
	/* NVMe: 구조체 필드에 값 저장: .groups */
	.groups = pci_dev_attr_groups,
};

/*
 * pci_alloc_dev() - 새 PCI 디바이스를 표현할 struct pci_dev 할당
 *
 * NVMe 연결: nvme_pci_driver가 바인딩될 대상 pci_dev가 여기서 생성된다.
 * dev->resource[]는 아직 비어 있고, 뒤의 pci_setup_device()에서
 * BAR/IRQ/클록 등을 채운다. msi_lock은 MSI-X 벡터 할당 시 사용.
 */
/* NVMe: PCI 장치 구조체 할당 */
struct pci_dev *pci_alloc_dev(struct pci_bus *bus)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *dev;

	dev = kzalloc_obj(struct pci_dev); /* NVMe: pci_dev 구조체를 0으로 초기화하며 할당 */
	if (!dev) /* NVMe: 메모리 부족 시 NULL 반환 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	INIT_LIST_HEAD(&dev->bus_list); /* NVMe: bus devices 리스트 연결용 node 초기화 */
	dev->dev.type = &pci_dev_type; /* NVMe: PCI device type 설정 */
	dev->bus = pci_bus_get(bus); /* NVMe: 소속 bus 참조 카운트 증가 */
	/* NVMe: 구조체 필드에 값 저장: dev->driver_exclusive_resource */
	dev->driver_exclusive_resource = (struct resource) {
		/* NVMe: 구조체 필드에 값 저장: .name */
		.name = "PCI Exclusive",
		/* NVMe: 구조체 필드에 값 저장: .start */
		.start = 0,
		/* NVMe: 구조체 필드에 값 저장: .end */
		.end = -1,
	}; /* NVMe: 드라이버 전용 resource 범위 초기화 */

	spin_lock_init(&dev->pcie_cap_lock); /* NVMe: PCIe capability 접근 보호용 spinlock 초기화 */
/* NVMe: 컴파일 조건: CONFIG_PCI_MSI 정의 시 포함 */
#ifdef CONFIG_PCI_MSI
	raw_spin_lock_init(&dev->msi_lock); /* NVMe: MSI/MSI-X 할당 보호용 raw spinlock 초기화; NVMe per-queue MSI-X에 사용 */
/* NVMe: 컴파일 조건 종료 */
#endif
	return dev; /* NVMe: 초기화된 pci_dev 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_alloc_dev);

/*
 * pci_bus_wait_rrs() - Configuration Request Retry Status 대기
 *
 * NVMe 연결: NVMe 컨트롤러가 부팅 직후 아직 준비되지 않았을 때,
 * config read는 RRS(예약된 vendor ID)로 완료될 수 있다. 이 함수는
 * 지수 백오프로 재시도하여 컨트롤러가 준비될 때까지 기다린다.
 */
/* NVMe: Configuration RRS 대기 */
static bool pci_bus_wait_rrs(struct pci_bus *bus, int devfn, u32 *l,
			     /* NVMe: 코드 동작 수행 */
			     int timeout)
{
	/* NVMe: 변수 선언/초기화 */
	int delay = 1;

	/* NVMe: 조걸 분기 */
	if (!pci_bus_rrs_vendor_id(*l))
		return true;	/* not a Configuration RRS completion */ /* NVMe: RRS가 아니면 즉시 true */

	/* NVMe: 조걸 분기 */
	if (!timeout)
		return false;	/* RRS, but caller doesn't want to wait */ /* NVMe: 대기를 원하지 않으면 false */

	/*
	 * We got the reserved Vendor ID that indicates a completion with
	 * Configuration Request Retry Status (RRS).  Retry until we get a
	 * valid Vendor ID or we time out.
	 */
	while (pci_bus_rrs_vendor_id(*l)) { /* NVMe: RRS가 해제될 때까지 반복 */
		if (delay > timeout) { /* NVMe: timeout 초과 시 */
			/* NVMe: 커널 경고 메시지 출력 */
			pr_warn("pci %04x:%02x:%02x.%d: not ready after %dms; giving up\n",
				/* NVMe: PCI 도메인 번호 획득 */
				pci_domain_nr(bus), bus->number,
				PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1); /* NVMe: NVMe 장치 준비 실패 경고 */

			/* NVMe: 거짓 반환 */
			return false;
		}
		/* NVMe: 조걸 분기 */
		if (delay >= 1000)
			/* NVMe: 커널 정보 메시지 출력 */
			pr_info("pci %04x:%02x:%02x.%d: not ready after %dms; waiting\n",
				/* NVMe: PCI 도메인 번호 획득 */
				pci_domain_nr(bus), bus->number,
				PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1); /* NVMe: 1초 이상 대기 시 정보 로그 */

		msleep(delay); /* NVMe: 지연 시간만큼 대기 */
		delay *= 2; /* NVMe: 지수 백오프로 대기 시간 증가 */

		/* NVMe: 조걸 분기 */
		if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l))
			return false; /* NVMe: config read 자체가 실패하면 false */
	}

	/* NVMe: 조걸 분기 */
	if (delay >= 1000)
		/* NVMe: 커널 정보 메시지 출력 */
		pr_info("pci %04x:%02x:%02x.%d: ready after %dms\n",
			/* NVMe: PCI 도메인 번호 획득 */
			pci_domain_nr(bus), bus->number,
			PCI_SLOT(devfn), PCI_FUNC(devfn), delay - 1); /* NVMe: 준비 완료 로그 */

	return true; /* NVMe: 유효한 vendor ID 획득 */
}

/*
 * pci_bus_generic_read_dev_vendor_id() - slot에서 vendor/device ID 읽기
 *
 * NVMe 연결: bus/dev/function 위치에 실제로 장치(예: NVMe 컨트롤러)가
 * 있는지 확인하는 첫 단계. 유효한 vendor ID가 읽히면 해당 위치에
 * pci_dev를 할당하고 계속 초기화.
 */
/* NVMe: Vendor/Device ID 일반 읽기 */
bool pci_bus_generic_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
					/* NVMe: 코드 동작 수행 */
					int timeout)
{
	if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l)) /* NVMe: 설정 공간 0x00에서 vendor/device ID 읽기 시도 */
		return false; /* NVMe: 읽기 실패 시 slot이 비어있거나 접근 불가 */

	/* Some broken boards return 0 or ~0 (PCI_ERROR_RESPONSE) if a slot is empty: */
	/* NVMe: 조걸 분기 */
	if (PCI_POSSIBLE_ERROR(*l) || *l == 0x00000000 ||
	    *l == 0x0000ffff || *l == 0xffff0000) /* NVMe: empty slot이나 error response이면 장치 없음 */
		/* NVMe: 거짓 반환 */
		return false;

	if (pci_bus_rrs_vendor_id(*l)) /* NVMe: RRS(예약 vendor ID)이면 */
		return pci_bus_wait_rrs(bus, devfn, l, timeout); /* NVMe: 장치 준비될 때까지 대기 */

	return true; /* NVMe: 유효한 vendor/device ID 획득; NVMe 컨트롤러 탐색 성공 */
}

/*
 * pci_bus_read_dev_vendor_id() - vendor/device ID 읽기의 아키텍처 기본 래퍼
 *
 * NVMe 연결: 대부분의 플랫폼에서 pci_bus_generic_read_dev_vendor_id()를
 * 직접 호출. NVMe 컨트롤러 탐색의 출발점.
 */
/* NVMe: 함수 호출: pci_bus_read_dev_vendor_id */
bool pci_bus_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
				/* NVMe: 코드 동작 수행 */
				int timeout)
{
	return pci_bus_generic_read_dev_vendor_id(bus, devfn, l, timeout); /* NVMe: 일반 vendor ID 읽기 경로 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_bus_read_dev_vendor_id);

/*
 * Read the config data for a PCI device, sanity-check it,
 * and fill in the dev structure.
 */
/* NVMe: PCI config space 읽어 장치 구조체 채우기 */
static struct pci_dev *pci_scan_device(struct pci_bus *bus, int devfn)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *dev;
	/* NVMe: 변수 선언/초기화 */
	u32 l;

	/* NVMe: 조걸 분기 */
	if (!pci_bus_read_dev_vendor_id(bus, devfn, &l, 60*1000))
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	/* NVMe: PCI 장치 구조체 할당 */
	dev = pci_alloc_dev(bus);
	/* NVMe: 조걸 분기 */
	if (!dev)
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	/* NVMe: 구조체 필드에 값 저장: dev->devfn */
	dev->devfn = devfn;
	/* NVMe: 구조체 필드에 비트 마스크 적용: dev->vendor */
	dev->vendor = l & 0xffff;
	/* NVMe: 구조체 필드에 비트 마스크 적용: dev->device */
	dev->device = (l >> 16) & 0xffff;

	/* NVMe: 조걸 분기 */
	if (pci_setup_device(dev)) {
		/* NVMe: 버스 레퍼런스 해제 */
		pci_bus_put(dev->bus);
		/* NVMe: 메모리 해제 */
		kfree(dev);
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;
	}

	/* NVMe: 결과 반환: dev */
	return dev;
}

/*
 * pcie_report_downtraining() - 링크가 최대 속도/폭보다 낮게 협상되면 경고
 *
 * NVMe 연결: NVMe SSD가 x4 링크에 연결되어야 하는데 x1로 협상되거나,
 * Gen4로 연결되어야 하는데 Gen3로 낮아지면 성능이 크게 저하. 이 함수는
 * pci_init_capabilities()에서 호출되어 문제를 로그에 남긴다.
 */
/* NVMe: 함수 정의: pcie_report_downtraining */
void pcie_report_downtraining(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev)) /* NVMe: PCIe 장치가 아니면 무관 */
		return;

	/* Look from the device up to avoid downstream ports with no devices */
	if ((pci_pcie_type(dev) != PCI_EXP_TYPE_ENDPOINT) && /* NVMe: Endpoint가 아니고 */
	    (pci_pcie_type(dev) != PCI_EXP_TYPE_LEG_END) && /* NVMe: Legacy Endpoint가 아니고 */
	    (pci_pcie_type(dev) != PCI_EXP_TYPE_UPSTREAM)) /* NVMe: Switch Upstream Port도 아니면 skip */
		return;

	/* Multi-function PCIe devices share the same link/status */
	if (PCI_FUNC(dev->devfn) != 0 || dev->is_virtfn) /* NVMe: 멀티펑션 장치는 function 0에서만 보고; VF는 skip */
		return;

	/* Print link status only if the device is constrained by the fabric */
	__pcie_print_link_status(dev, false); /* NVMe: 링크가 최대 값보다 낮으면 경고 출력; NVMe 성능 저하 원인 분석 */
}

/*
 * pci_imm_ready_init() - Immediate Readiness Status bit 확인
 *
 * NVMe 연결: Immediate Readiness를 지원하는 장치는 전원 상태 전환 후
 * 추가 지연 없이 config access에 응답. NVMe RTD3 복귀 지연에 영향.
 */
/* NVMe: 함수 정의: pci_imm_ready_init */
static void pci_imm_ready_init(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	u16 status;

	pci_read_config_word(dev, PCI_STATUS, &status); /* NVMe: PCI Status 레지스터 읽기 */
	if (status & PCI_STATUS_IMM_READY) /* NVMe: Immediate Readiness bit 확인 */
		dev->imm_ready = 1; /* NVMe: pci_dev에 즉시 준비 가능 표시 */
}

/* NVMe: 함수 정의: pci_init_capabilities */
static void pci_init_capabilities(struct pci_dev *dev)
{
	pci_ea_init(dev);		/* Enhanced Allocation */
	pci_msi_init(dev);		/* Disable MSI */
	pci_msix_init(dev);		/* Disable MSI-X */

	/* Buffers for saving PCIe and PCI-X capabilities */
	/* NVMe: capability 저장 버퍼 할당 */
	pci_allocate_cap_save_buffers(dev);

	pci_imm_ready_init(dev);	/* Immediate Readiness */
	pci_pm_init(dev);		/* Power Management */
	pci_vpd_init(dev);		/* Vital Product Data */
	pci_configure_ari(dev);		/* Alternative Routing-ID Forwarding */
	pci_iov_init(dev);		/* Single Root I/O Virtualization */
	pci_ats_init(dev);		/* Address Translation Services */
	pci_pri_init(dev);		/* Page Request Interface */
	pci_pasid_init(dev);		/* Process Address Space ID */
	pci_acs_init(dev);		/* Access Control Services */
	pci_ptm_init(dev);		/* Precision Time Measurement */
	pci_aer_init(dev);		/* Advanced Error Reporting */
	pci_dpc_init(dev);		/* Downstream Port Containment */
	pci_rcec_init(dev);		/* Root Complex Event Collector */
	pci_doe_init(dev);		/* Data Object Exchange */
	pci_tph_init(dev);		/* TLP Processing Hints */
	pci_rebar_init(dev);		/* Resizable BAR */
	pci_dev3_init(dev);		/* Device 3 capabilities */
	pci_ide_init(dev);		/* Link Integrity and Data Encryption */

	/* NVMe: 링크 다운트레이닝 경고 */
	pcie_report_downtraining(dev);
	/* NVMe: 리셋 방법 초기화 */
	pci_init_reset_methods(dev);
}

/*
 * This is the equivalent of pci_host_bridge_msi_domain() that acts on
 * devices. Firmware interfaces that can select the MSI domain on a
 * per-device basis should be called from here.
 */
/*
 * pci_dev_msi_domain() - 개별 PCI 장치에 대한 MSI domain 탐색
 *
 * NVMe 연결: NVMe 컨트롤러의 MSI-X 벡터가 매핑될 IRQ domain을 찾는다.
 * interrupt remapping이 활성화된 시스템에서는 MSI message가 이 domain에
 * 의해 물리 인터럽트로 변환.
 */
/* NVMe: 장치별 MSI 도메인 획득 */
static struct irq_domain *pci_dev_msi_domain(struct pci_dev *dev)
{
	/* NVMe: 코드 동작 수행 */
	struct irq_domain *d;

	/*
	 * If a domain has been set through the pcibios_device_add()
	 * callback, then this is the one (platform code knows best).
	 */
	d = dev_get_msi_domain(&dev->dev); /* NVMe: platform이 미리 설정한 MSI domain 확인 */
	/* NVMe: 조걸 분기 */
	if (d)
		return d; /* NVMe: platform-specific domain 우선 사용 */

	/*
	 * Let's see if we have a firmware interface able to provide
	 * the domain.
	 */
	d = pci_msi_get_device_domain(dev); /* NVMe: firmware(ACPI/DT)에서 장치별 MSI domain 검색 */
	/* NVMe: 조걸 분기 */
	if (d)
		return d; /* NVMe: firmware가 제공한 domain 사용 */

	return NULL; /* NVMe: 개별 domain 없음; bus/host bridge domain 상속 필요 */
}

/*
 * pci_set_msi_domain() - pci_dev에 최종 MSI domain 설정
 *
 * NVMe 연결: pci_device_add()에서 호출되며, NVMe 컨트롤러의 MSI-X
 * 인터럽트가 속할 IRQ domain을 확정. 개별 domain이 없으면 버스의
 * domain을 상속.
 */
/* NVMe: 함수 정의: pci_set_msi_domain */
static void pci_set_msi_domain(struct pci_dev *dev)
{
	/* NVMe: 코드 동작 수행 */
	struct irq_domain *d;

	/*
	 * If the platform or firmware interfaces cannot supply a
	 * device-specific MSI domain, then inherit the default domain
	 * from the host bridge itself.
	 */
	d = pci_dev_msi_domain(dev); /* NVMe: 장치별 MSI domain 탐색 */
	/* NVMe: 조걸 분기 */
	if (!d)
		d = dev_get_msi_domain(&dev->bus->dev); /* NVMe: 없으면 bus의 MSI domain 상속; NVMe MSI-X affinity의 기반 */

	dev_set_msi_domain(&dev->dev, d); /* NVMe: pci_dev의 MSI domain 확정; nvme_probe에서 pci_enable_msix_range() 사용 */
}

/* NVMe: 함수 정의: pci_device_add */
void pci_device_add(struct pci_dev *dev, struct pci_bus *bus)
{
	/* NVMe: 변수 선언/초기화 */
	int ret;

	/* NVMe: 장치 PCIe 설정 일괄 구성 */
	pci_configure_device(dev);

	/* NVMe: 커널 장치 초기화 */
	device_initialize(&dev->dev);
	/* NVMe: 구조체 필드에 값 저장: dev->dev.release */
	dev->dev.release = pci_release_dev;

	/* NVMe: NUMA 노드 설정 */
	set_dev_node(&dev->dev, pcibus_to_node(bus));
	/* NVMe: 구조체 필드에 비트 마스크 적용: dev->dev.dma_mask */
	dev->dev.dma_mask = &dev->dma_mask;
	/* NVMe: 구조체 필드에 비트 마스크 적용: dev->dev.dma_parms */
	dev->dev.dma_parms = &dev->dma_parms;
	/* NVMe: 구조체 필드에 값 저장: dev->dev.coherent_dma_mask */
	dev->dev.coherent_dma_mask = 0xffffffffull;

	/* NVMe: 함수 호출: dma_set_max_seg_size */
	dma_set_max_seg_size(&dev->dev, 65536);
	/* NVMe: 함수 호출: dma_set_seg_boundary */
	dma_set_seg_boundary(&dev->dev, 0xffffffff);

	/* NVMe: 링크 재학습 실패 기록 */
	pcie_failed_link_retrain(dev);

	/* Fix up broken headers */
	/* NVMe: 장치 quirk/workaround 적용 */
	pci_fixup_device(pci_fixup_header, dev);

	/* NVMe: 장치 리소스 정렬 재할당 */
	pci_reassigndev_resource_alignment(dev);

	/* NVMe: PCIe 확장 기능 초기화 */
	pci_init_capabilities(dev);

	/*
	 * Add the device to our list of discovered devices
	 * and the bus list for fixup functions, etc.
	 */
	/* NVMe: do-while 반복문 시작 */
	down_write(&pci_bus_sem);
	/* NVMe: 리스트 뒤에 추가 */
	list_add_tail(&dev->bus_list, &bus->devices);
	/* NVMe: 쓰기 세마포어 해제 */
	up_write(&pci_bus_sem);

	/* NVMe: 아키텍처별 장치 추가 */
	ret = pcibios_device_add(dev);
	/* NVMe: 조건 경고 */
	WARN_ON(ret < 0);

	/* Set up MSI IRQ domain */
	/* NVMe: MSI 인터럽트 도메인 설정 */
	pci_set_msi_domain(dev);

	/* Notifier could use PCI capabilities */
	/* NVMe: 커널에 장치 추가 */
	ret = device_add(&dev->dev);
	/* NVMe: 조건 경고 */
	WARN_ON(ret < 0);

	/* Establish pdev->tsm for newly added (e.g. new SR-IOV VFs) */
	/* NVMe: Thermal Status Management 초기화 */
	pci_tsm_init(dev);

	/* NVMe: NPEM 객체 생성 */
	pci_npem_create(dev);

	/* NVMe: DOE sysfs 초기화 */
	pci_doe_sysfs_init(dev);
}

/*
 * pci_scan_single_device() - 특정 bus/dev/function에서 장치를 스캔하거나
 *                            이미 등록된 장치 반환
 *
 * NVMe 연결: NVMe 컨트롤러가 있는 정확한 bus/dev/function 위치를
 * 처리. 이미 스캔된 장치면 참조를 반환하고, 새 장치면 pci_device_add()를
 * 통해 nvme_pci_driver가 probe할 수 있게 등록.
 */
/* NVMe: 단일 PCI function 스캔 */
struct pci_dev *pci_scan_single_device(struct pci_bus *bus, int devfn)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *dev;

	dev = pci_get_slot(bus, devfn); /* NVMe: 해당 slot에 이미 등록된 pci_dev가 있는지 확인 */
	/* NVMe: 조걸 분기 */
	if (dev) {
		pci_dev_put(dev); /* NVMe: 참조 카운트 감소 */
		return dev; /* NVMe: 이미 스캔된 장치 반환 */
	}

	dev = pci_scan_device(bus, devfn); /* NVMe: 설정 공간을 읽어 새 pci_dev 생성; NVMe 컨트롤러 발견 */
	/* NVMe: 조걸 분기 */
	if (!dev)
		return NULL; /* NVMe: 해당 slot에 장치 없음 */

	pci_device_add(dev, bus); /* NVMe: pci_dev를 PCI 코어에 등록; nvme_pci_driver.probe -> nvme_probe */

	return dev; /* NVMe: 새로 등록된 pci_dev 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_scan_single_device);

/*
 * next_ari_fn() - ARI(Alternative Routing-ID)를 사용하는 장치의 다음
 *                 function 번호 반환
 *
 * NVMe 연결: ARI를 지원하는 NVMe 컨트롤러는 function 번호를 0~255까지
 * 사용할 수 있어, multifunction/SR-IOV 형태로 더 많은 function을 노출.
 */
/* NVMe: 함수 정의: next_ari_fn */
static int next_ari_fn(struct pci_bus *bus, struct pci_dev *dev, int fn)
{
	/* NVMe: 변수 선언/초기화 */
	int pos;
	/* NVMe: 변수 선언/초기화 */
	u16 cap = 0;
	/* NVMe: 변수 선언/초기화 */
	unsigned int next_fn;

	if (!dev) /* NVMe: function 0이 없으면 ARI function도 없음 */
		/* NVMe: 오류 코드 반환: -ENODEV */
		return -ENODEV;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ARI); /* NVMe: ARI extended capability 탐색 */
	/* NVMe: 조걸 분기 */
	if (!pos)
		return -ENODEV; /* NVMe: ARI 미지원 */

	pci_read_config_word(dev, pos + PCI_ARI_CAP, &cap); /* NVMe: ARI Capability 레지스터 읽기 */
	next_fn = PCI_ARI_CAP_NFN(cap); /* NVMe: Next Function Number 필드 추출 */
	/* NVMe: 조걸 분기 */
	if (next_fn <= fn)
		return -ENODEV;	/* protect against malformed list */ /* NVMe: 잘못된 ARI capability이면 중단 */

	return next_fn; /* NVMe: 다음 스캔할 ARI function 번호 반환 */
}

/*
 * next_fn() - 다음에 스캔할 PCI function 번호 반환
 *
 * NVMe 연결: ARI가 활성화된 버스에서는 ARI function 번호를 따르고,
 * 그렇지 않으면 전통적인 8개 function을 multifunction 장치에 대해서만
 * 스캔. 멀티펑션 NVMe 컨트롤러를 모두 발견하는 데 사용.
 */
/* NVMe: 함수 정의: next_fn */
static int next_fn(struct pci_bus *bus, struct pci_dev *dev, int fn)
{
	if (pci_ari_enabled(bus)) /* NVMe: 버스에서 ARI가 활성화되어 있으면 */
		return next_ari_fn(bus, dev, fn); /* NVMe: ARI 기반 다음 function 번호 반환 */

	if (fn >= 7) /* NVMe: 전통적인 function 번호가 7에 도달하면 종료 */
		/* NVMe: 오류 코드 반환: -ENODEV */
		return -ENODEV;
	/* only multifunction devices may have more functions */
	if (dev && !dev->multifunction) /* NVMe: 단일 function 장치이면 더 이상 스캔하지 않음 */
		/* NVMe: 오류 코드 반환: -ENODEV */
		return -ENODEV;

	return fn + 1; /* NVMe: 다음 function 번호 반환 */
}

/*
 * only_one_child() - PCIe Downstream Port 아래에서는 Device 0만 스캔할지 결정
 *
 * NVMe 연결: PCIe link에는 보통 Device 0 하나만 연결되므로 function 1~7
 * 스캔을 생략하여 부팅 시간을 단축. 다만 ARI를 사용하는 NVMe
 * 컨트롤러는 function 0이 여러 function을 대표할 수 있다.
 */
/* NVMe: 함수 정의: only_one_child */
static int only_one_child(struct pci_bus *bus)
{
	struct pci_dev *bridge = bus->self; /* NVMe: 이 버스의 upstream bridge */

	/*
	 * Systems with unusual topologies set PCI_SCAN_ALL_PCIE_DEVS so
	 * we scan for all possible devices, not just Device 0.
	 */
	if (pci_has_flag(PCI_SCAN_ALL_PCIE_DEVS)) /* NVMe: 강제 전체 스캔 플래그가 켜져 있으면 */
		return 0; /* NVMe: Device 0만 스캔하지 않고 모든 device/function 스캔 */

	/*
	 * A PCIe Downstream Port normally leads to a Link with only Device
	 * 0 on it (PCIe spec r3.1, sec 7.3.1).  As an optimization, scan
	 * only for Device 0 in that situation.
	 */
	if (bridge && pci_is_pcie(bridge) && pcie_downstream_port(bridge)) /* NVMe: PCIe Downstream Port 아래면 */
		return 1; /* NVMe: Device 0만 스캔; NVMe는 보통 function 0에 위치 */

	return 0; /* NVMe: 일반 PCI 버스이면 모든 device/function 스캔 */
}

/**
 * pci_scan_slot - Scan a PCI slot on a bus for devices
 * @bus: PCI bus to scan
 * @devfn: slot number to scan (must have zero function)
 *
 * Scan a PCI slot on the specified PCI bus for devices, adding
 * discovered devices to the @bus->devices list.  New devices
 * will not have is_added set.
 *
 * Returns the number of new devices found.
 */
/* NVMe: 함수 정의: pci_scan_slot */
int pci_scan_slot(struct pci_bus *bus, int devfn)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *dev;
	/* NVMe: 변수에 값 할당: int fn */
	int fn = 0, nr = 0;

	/* NVMe: 조걸 분기 */
	if (only_one_child(bus) && (devfn > 0))
		return 0; /* Already scanned the entire slot */

	/* NVMe: do-while 반복문 시작 */
	do {
		/* NVMe: 단일 PCI function 스캔 */
		dev = pci_scan_single_device(bus, devfn + fn);
		/* NVMe: 조걸 분기 */
		if (dev) {
			/* NVMe: 조걸 분기 */
			if (!pci_dev_is_added(dev))
				/* NVMe: 카운터 증감 */
				nr++;
			/* NVMe: 조걸 분기 */
			if (fn > 0)
				/* NVMe: 구조체 필드에 값 저장: dev->multifunction */
				dev->multifunction = 1;
		/* NVMe: 함수 호출: if */
		} else if (fn == 0) {
			/*
			 * Function 0 is required unless we are running on
			 * a hypervisor that passes through individual PCI
			 * functions.
			 */
			/* NVMe: 조걸 분기 */
			if (!hypervisor_isolated_pci_functions())
				break;
		}
		/* NVMe: 함수 호출: next_fn */
		fn = next_fn(bus, dev, fn);
	/* NVMe: 함수 호출: while */
	} while (fn >= 0);

	/* Only one slot has PCIe device */
	/* NVMe: 조걸 분기 */
	if (bus->self && nr)
		/* NVMe: ASPM 링크 상태 초기화 */
		pcie_aspm_init_link_state(bus->self);

	/* NVMe: 결과 반환: nr */
	return nr;
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_scan_slot);

/*
 * pcie_find_smpss() - 버스 계층에서 가장 작은 MPSS(MPS Supported Size) 찾기
 *
 * NVMe 연결: MPS는 PCIe TLP payload 상한을 결정. SAFE 모드에서는 계층
 * 내 모든 장치의 MPSS 중 가장 작은 값을 선택해 호환성을 보장. NVMe
 * 성능보다 안정성을 우선.
 */
/* NVMe: 함수 정의: pcie_find_smpss */
static int pcie_find_smpss(struct pci_dev *dev, void *data)
{
	u8 *smpss = data; /* NVMe: 현재까지 찾은 최소 MPSS 포인터 */

	if (!pci_is_pcie(dev)) /* NVMe: PCIe 장치가 아니면 MPS 개념 없음 */
		/* NVMe: 성공 반환 */
		return 0;

	/*
	 * We don't have a way to change MPS settings on devices that have
	 * drivers attached.  A hot-added device might support only the minimum
	 * MPS setting (MPS=128).  Therefore, if the fabric contains a bridge
	 * where devices may be hot-added, we limit the fabric MPS to 128 so
	 * hot-added devices will work correctly.
	 *
	 * However, if we hot-add a device to a slot directly below a Root
	 * Port, it's impossible for there to be other existing devices below
	 * the port.  We don't limit the MPS in this case because we can
	 * reconfigure MPS on both the Root Port and the hot-added device,
	 * and there are no other devices involved.
	 *
	 * Note that this PCIE_BUS_SAFE path assumes no peer-to-peer DMA.
	 */
	if (dev->is_hotplug_bridge && /* NVMe: hotplug bridge이고 */
	    pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT) /* NVMe: Root Port 직접 아래가 아니면 */
		*smpss = 0; /* NVMe: MPS를 최소 128B로 제한; 향후 hot-add 장치 호환성 */

	if (*smpss > dev->pcie_mpss) /* NVMe: 현재 최소값보다 장치의 MPSS가 더 작으면 */
		*smpss = dev->pcie_mpss; /* NVMe: 최소 MPSS 갱신 */

	return 0; /* NVMe: pci_walk_bus 콜백 계속 진행 */
}

/*
 * pcie_write_mps() - 장치의 Max Payload Size 레지스터에 기록
 *
 * NVMe 연결: PERFORMANCE 모드에서는 장치가 지원하는 최대 MPS를 사용하여
 * NVMe Read/Write TLP 효율을 극대화. SAFE 모드에서는 이미 계산된
 * smpss 값을 사용.
 */
/* NVMe: 함수 정의: pcie_write_mps */
static void pcie_write_mps(struct pci_dev *dev, int mps)
{
	/* NVMe: 변수 선언/초기화 */
	int rc;

	if (pcie_bus_config == PCIE_BUS_PERFORMANCE) { /* NVMe: 성능 우선 모드이면 */
		mps = 128 << dev->pcie_mpss; /* NVMe: 장치가 지원하는 최대 MPS로 설정 */

		if (pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT && /* NVMe: Root Port가 아니고 */
		    dev->bus->self) /* NVMe: 상위 bridge가 있으면 */

			/*
			 * For "Performance", the assumption is made that
			 * downstream communication will never be larger than
			 * the MRRS.  So, the MPS only needs to be configured
			 * for the upstream communication.  This being the case,
			 * walk from the top down and set the MPS of the child
			 * to that of the parent bus.
			 *
			 * Configure the device MPS with the smaller of the
			 * device MPSS or the bridge MPS (which is assumed to be
			 * properly configured at this point to the largest
			 * allowable MPS based on its parent bus).
			 */
			mps = min(mps, pcie_get_mps(dev->bus->self)); /* NVMe: 장치 최대 MPS와 상위 bridge MPS 중 작은 값 선택 */
	}

	rc = pcie_set_mps(dev, mps); /* NVMe: PCI_EXP_DEVCTL의 MPS 필드 기록 */
	/* NVMe: 조걸 분기 */
	if (rc)
		pci_err(dev, "Failed attempting to set the MPS\n"); /* NVMe: MPS 설정 실패 로그; NVMe TLP 크기 제한 가능 */
}

/*
 * pcie_write_mrrs() - Max Read Request Size 설정
 *
 * NVMe 연결: MRRS는 한 번의 Memory Read 요청이 요청할 수 있는 최대
 * 바이트 수. PERFORMANCE 모드에서 큰 값으로 설정하면 NVMe의 대용량
 * DMA Read(PRP/SGL) 효율이 향상. MPS보다 클 수 없다.
 */
/* NVMe: 함수 정의: pcie_write_mrrs */
static void pcie_write_mrrs(struct pci_dev *dev)
{
	/* NVMe: 변수 선언/초기화 */
	int rc, mrrs;

	/*
	 * In the "safe" case, do not configure the MRRS.  There appear to be
	 * issues with setting MRRS to 0 on a number of devices.
	 */
	if (pcie_bus_config != PCIE_BUS_PERFORMANCE) /* NVMe: PERFORMANCE 모드가 아니면 MRRS 변경 안 함 */
		return;

	/*
	 * For max performance, the MRRS must be set to the largest supported
	 * value.  However, it cannot be configured larger than the MPS the
	 * device or the bus can support.  This should already be properly
	 * configured by a prior call to pcie_write_mps().
	 */
	mrrs = pcie_get_mps(dev); /* NVMe: MPS를 초과할 수 없으므로 MPS 값을 MRRS 초기값으로 사용 */

	/*
	 * MRRS is a R/W register.  Invalid values can be written, but a
	 * subsequent read will verify if the value is acceptable or not.
	 * If the MRRS value provided is not acceptable (e.g., too large),
	 * shrink the value until it is acceptable to the HW.
	 */
	while (mrrs != pcie_get_readrq(dev) && mrrs >= 128) { /* NVMe: 설정된 MRRS가 실제 값과 다륾고 128B 이상이면 재시도 */
		rc = pcie_set_readrq(dev, mrrs); /* NVMe: MRRS 레지스터 기록 */
		/* NVMe: 조걸 분기 */
		if (!rc)
			break; /* NVMe: 성공하면 루프 종료 */

		/* NVMe: 경고 메시지 출력 */
		pci_warn(dev, "Failed attempting to set the MRRS\n");
		mrrs /= 2; /* NVMe: 실패하면 MRRS를 절반으로 줄여 재시도 */
	}

	/* NVMe: 조걸 분기 */
	if (mrrs < 128)
		pci_err(dev, "MRRS was unable to be configured with a safe value.  If problems are experienced, try running with pci=pcie_bus_safe\n"); /* NVMe: MRRS 설정 최종 실패; NVMe Read 효율 저하 */
}

/*
 * pcie_bus_configure_set() - 개별 장치에 MPS/MRRS 적용
 *
 * NVMe 연결: pcie_bus_configure_settings()가 버스를 순회하면서 각
 * 장치(NVMe 포함)의 MPS/MRRS를 설정. TUNE_OFF나 DEFAULT 모드에서는
 * 아무 것도 하지 않는다.
 */
/* NVMe: 함수 정의: pcie_bus_configure_set */
static int pcie_bus_configure_set(struct pci_dev *dev, void *data)
{
	/* NVMe: 변수 선언/초기화 */
	int mps, orig_mps;

	if (!pci_is_pcie(dev)) /* NVMe: PCIe 장치가 아니면 MPS/MRRS 무관 */
		/* NVMe: 성공 반환 */
		return 0;

	/* NVMe: 조걸 분기 */
	if (pcie_bus_config == PCIE_BUS_TUNE_OFF ||
	    pcie_bus_config == PCIE_BUS_DEFAULT) /* NVMe: tuning하지 않는 모드이면 skip */
		/* NVMe: 성공 반환 */
		return 0;

	mps = 128 << *(u8 *)data; /* NVMe: smpss에 기반한 목표 MPS 산출; 128*2^smpss */
	orig_mps = pcie_get_mps(dev); /* NVMe: 설정 전 원래 MPS 저장 */

	pcie_write_mps(dev, mps); /* NVMe: 목표 MPS로 설정; NVMe TLP payload 크기 결정 */
	pcie_write_mrrs(dev); /* NVMe: MRRS 설정; NVMe Read request 크기 결정 */

	/* NVMe: 정보 메시지 출력 */
	pci_info(dev, "Max Payload Size set to %4d/%4d (was %4d), Max Read Rq %4d\n",
		 /* NVMe: MPS 레지스터 읽기 */
		 pcie_get_mps(dev), 128 << dev->pcie_mpss,
		 orig_mps, pcie_get_readrq(dev)); /* NVMe: 설정 결과 로깅 */

	return 0; /* NVMe: pci_walk_bus 콜백 계속 진행 */
}

/*
 * pcie_bus_configure_settings() requires that pci_walk_bus work in a top-down,
 * parents then children fashion.  If this changes, then this code will not
 * work as designed.
 */
/* NVMe: 함수 정의: pcie_bus_configure_settings */
void pcie_bus_configure_settings(struct pci_bus *bus)
{
	/* NVMe: 변수 선언/초기화 */
	u8 smpss = 0;

	/* NVMe: 조걸 분기 */
	if (!bus->self)
		return;

	/* NVMe: 조걸 분기 */
	if (!pci_is_pcie(bus->self))
		return;

	/*
	 * FIXME - Peer to peer DMA is possible, though the endpoint would need
	 * to be aware of the MPS of the destination.  To work around this,
	 * simply force the MPS of the entire system to the smallest possible.
	 */
	/* NVMe: 조걸 분기 */
	if (pcie_bus_config == PCIE_BUS_PEER2PEER)
		/* NVMe: 변수에 값 할당: smpss */
		smpss = 0;

	/* NVMe: 조걸 분기 */
	if (pcie_bus_config == PCIE_BUS_SAFE) {
		/* NVMe: 변수에 값 할당: smpss */
		smpss = bus->self->pcie_mpss;

		/* NVMe: 최소 MPS 인덱스 탐색 */
		pcie_find_smpss(bus->self, &smpss);
		/* NVMe: 버스 전체 순회 */
		pci_walk_bus(bus, pcie_find_smpss, &smpss);
	}

	/* NVMe: 버스 설정 적용 */
	pcie_bus_configure_set(bus->self, &smpss);
	/* NVMe: 버스 전체 순회 */
	pci_walk_bus(bus, pcie_bus_configure_set, &smpss);
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pcie_bus_configure_settings);

/*
 * Called after each bus is probed, but before its children are examined.  This
 * is marked as __weak because multiple architectures define it.
 */
/*
 * pcibios_fixup_bus() - 아키텍처별 bus fixup(기본 빈 구현)
 *
 * NVMe 연결: 특정 아키텍처에서 필요한 bus 리소스나 속성을 조정.
 * 대부분의 플랫폼에서는 아무 것도 하지 않는다.
 */
/* NVMe: 아키텍처별 버스 fixup */
void __weak pcibios_fixup_bus(struct pci_bus *bus)
{
       /* nothing to do, expected to be removed in the future */ /* NVMe: 기본적으로 수행할 작업 없음 */
}

/**
 * pci_scan_child_bus_extend() - Scan devices below a bus
 * @bus: Bus to scan for devices
 * @available_buses: Total number of buses available (%0 does not try to
 *		     extend beyond the minimal)
 *
 * Scans devices below @bus including subordinate buses. Returns new
 * subordinate number including all the found devices. Passing
 * @available_buses causes the remaining bus space to be distributed
 * equally between hotplug-capable bridges to allow future extension of the
 * hierarchy.
 */
/*
 * pci_scan_child_bus_extend() - 버스 아래의 모든 장치와 하위 버스를 재귀 스캔
 *
 * NVMe 연결: 이 함수는 NVMe SSD가 발견되는 핵심 루프. 모든 slot을
 * 스캔하고, SR-IOV용 bus 번호를 예약하며, bridge 뒤의 하위 버스를
 * 재귀적으로 탐색. available_buses는 hotplug bridge를 위한 추가 bus
 * 번호를 분배하는 데 사용.
 */
/* NVMe: 하위 버스 재귀 스캔(확장 버스 분배) */
static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
					      /* NVMe: 코드 동작 수행 */
					      unsigned int available_buses)
{
	/* NVMe: 변수에 값 할당: unsigned int used_buses, normal_bridges */
	unsigned int used_buses, normal_bridges = 0, hotplug_bridges = 0;
	/* NVMe: 변수에 값 할당: unsigned int start */
	unsigned int start = bus->busn_res.start;
	/* NVMe: 변수 선언/초기화 */
	unsigned int devnr, cmax, max = start;
	/* NVMe: 코드 동작 수행 */
	struct pci_dev *dev;

	dev_dbg(&bus->dev, "scanning bus\n"); /* NVMe: 버스 스캔 시작 디버그 로그 */

	/* Go find them, Rover! */
	for (devnr = 0; devnr < PCI_MAX_NR_DEVS; devnr++) /* NVMe: 0~31 slot 순회 */
		pci_scan_slot(bus, PCI_DEVFN(devnr, 0)); /* NVMe: 각 slot의 function 0부터 스캔; NVMe 컨트롤러 탐색 */

	/* Reserve buses for SR-IOV capability */
	used_buses = pci_iov_bus_range(bus); /* NVMe: 이 버스의 SR-IOV VF들을 위해 필요한 bus 번호 수 계산 */
	max += used_buses; /* NVMe: 사용된 bus 번호만큼 max 증가 */

	/*
	 * After performing arch-dependent fixup of the bus, look behind
	 * all PCI-to-PCI bridges on this bus.
	 */
	if (!bus->is_added) { /* NVMe: 처음 추가된 버스이면 */
		/* NVMe: 장치 디버그 메시지 출력 */
		dev_dbg(&bus->dev, "fixups for bus\n");
		pcibios_fixup_bus(bus); /* NVMe: 아키텍처별 bus fixup 수행 */
		bus->is_added = 1; /* NVMe: fixup 완료 표시 */
	}

	/*
	 * Calculate how many hotplug bridges and normal bridges there
	 * are on this bus. We will distribute the additional available
	 * buses between hotplug bridges.
	 */
	for_each_pci_bridge(dev, bus) { /* NVMe: 이 버스에 있는 모든 bridge 순회 */
		/* NVMe: 조걸 분기 */
		if (dev->is_hotplug_bridge)
			hotplug_bridges++; /* NVMe: hotplug bridge 카운트 */
		/* NVMe: 조걸 분기의 else 경로 */
		else
			normal_bridges++; /* NVMe: 일반 bridge 카운트 */
	}

	/*
	 * Scan bridges that are already configured. We don't touch them
	 * unless they are misconfigured (which will be done in the second
	 * scan below).
	 */
	for_each_pci_bridge(dev, bus) { /* NVMe: 이미 firmware가 설정한 bridge들을 첫 pass로 스캔 */
		/* NVMe: 변수에 값 할당: cmax */
		cmax = max;
		max = pci_scan_bridge_extend(bus, dev, max, 0, 0); /* NVMe: pass 0로 bridge 뒤 스캔; NVMe가 연결될 수 있음 */

		/*
		 * Reserve one bus for each bridge now to avoid extending
		 * hotplug bridges too much during the second scan below.
		 */
		used_buses++; /* NVMe: 각 bridge에 최소 1개 bus 예약 */
		/* NVMe: 조걸 분기 */
		if (max - cmax > 1)
			used_buses += max - cmax - 1; /* NVMe: 추가로 사용된 bus 수만큼 증가 */
	}

	/* Scan bridges that need to be reconfigured */
	for_each_pci_bridge(dev, bus) { /* NVMe: 재설정이 필요한 bridge들을 두 번째 pass로 스캔 */
		/* NVMe: 변수 선언/초기화 */
		unsigned int buses = 0;

		if (!hotplug_bridges && normal_bridges == 1) { /* NVMe: hotplug bridge가 없고 일반 bridge가 1개뿐이면 */
			/*
			 * There is only one bridge on the bus (upstream
			 * port) so it gets all available buses which it
			 * can then distribute to the possible hotplug
			 * bridges below.
			 */
			buses = available_buses; /* NVMe: 모든 추가 available bus를 이 bridge에 할당 */
		} else if (dev->is_hotplug_bridge) { /* NVMe: hotplug bridge이면 */
			/*
			 * Distribute the extra buses between hotplug
			 * bridges if any.
			 */
			buses = available_buses / hotplug_bridges; /* NVMe: hotplug bridge들 간 추가 bus 분배 */
			buses = min(buses, available_buses - used_buses + 1); /* NVMe: 남은 bus를 초과하지 않도록 제한 */
		}

		/* NVMe: 변수에 값 할당: cmax */
		cmax = max;
		max = pci_scan_bridge_extend(bus, dev, cmax, buses, 1); /* NVMe: pass 1로 재설정 및 하위 스캔 */
		/* One bus is already accounted so don't add it again */
		/* NVMe: 조걸 분기 */
		if (max - cmax > 1)
			used_buses += max - cmax - 1; /* NVMe: 추가 사용 bus 수 갱신 */
	}

	/*
	 * Make sure a hotplug bridge has at least the minimum requested
	 * number of buses but allow it to grow up to the maximum available
	 * bus number if there is room.
	 */
	if (bus->self && bus->self->is_hotplug_bridge) { /* NVMe: 현재 버스가 hotplug bridge 아래이면 */
		used_buses = max(available_buses, pci_hotplug_bus_size - 1); /* NVMe: 최소 요구 bus 수 확보 */
		if (max - start < used_buses) { /* NVMe: 현재 max가 최소 요구보다 작으면 확장 */
			/* NVMe: 변수에 값 할당: max */
			max = start + used_buses;

			/* Do not allocate more buses than we have room left */
			/* NVMe: 조걸 분기 */
			if (max > bus->busn_res.end)
				max = bus->busn_res.end; /* NVMe: 상위가 허용한 bus 번호를 초과하지 않도록 제한 */

			/* NVMe: 장치 디버그 메시지 출력 */
			dev_dbg(&bus->dev, "%pR extended by %#02x\n",
				&bus->busn_res, max - start); /* NVMe: bus 번호 범위 확장 디버그 로그 */
		}
	}

	/*
	 * We've scanned the bus and so we know all about what's on
	 * the other side of any bridges that may be on this bus plus
	 * any devices.
	 *
	 * Return how far we've got finding sub-buses.
	 */
	dev_dbg(&bus->dev, "bus scan returning with max=%02x\n", max); /* NVMe: 스캔 완료 및 최대 subordinate bus 출력 */
	return max; /* NVMe: 현재 버스 계층에서 사용한 최대 bus 번호 반환 */
}

/**
 * pci_scan_child_bus() - Scan devices below a bus
 * @bus: Bus to scan for devices
 *
 * Scans devices below @bus including subordinate buses. Returns new
 * subordinate number including all the found devices.
 */
/*
 * pci_scan_child_bus() - 추가 available bus 없이 하위 버스 스캔
 *
 * NVMe 연결: 일반적인 PCI 스캔 경로. NVMe SSD가 발견될 때까지
 * pci_scan_child_bus_extend()를 호출.
 */
/* NVMe: 하위 버스 재귀 스캔 */
unsigned int pci_scan_child_bus(struct pci_bus *bus)
{
	return pci_scan_child_bus_extend(bus, 0); /* NVMe: 추가 bus 예약 없이 하위 스캔 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_scan_child_bus);

/**
 * pcibios_root_bridge_prepare - Platform-specific host bridge setup
 * @bridge: Host bridge to set up
 *
 * Default empty implementation.  Replace with an architecture-specific setup
 * routine, if necessary.
 */
/* NVMe: 아키텍처별 호스트 브리지 준비 */
int __weak pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	/* NVMe: 성공 반환 */
	return 0;
}

/* NVMe: 아키텍처별 버스 추가 */
void __weak pcibios_add_bus(struct pci_bus *bus)
{
}

/* NVMe: 아키텍처별 버스 제거 */
void __weak pcibios_remove_bus(struct pci_bus *bus)
{
}

/*
 * pci_create_root_bus() - host bridge를 할당하고 root bus 등록
 *
 * NVMe 연결: PCI host controller driver가 Root Complex 아래의 root bus를
 * 생성할 때 사용. resources에는 MEM/IO/BUS window가 포함되며, 이는
 * NVMe BAR 할당의 전체 공간이 된다.
 */
/* NVMe: 루트 PCI 버스 생성 */
struct pci_bus *pci_create_root_bus(struct device *parent, int bus,
		/* NVMe: 코드 동작 수행 */
		struct pci_ops *ops, void *sysdata, struct list_head *resources)
{
	/* NVMe: 변수 선언/초기화 */
	int error;
	/* NVMe: 코드 동작 수행 */
	struct pci_host_bridge *bridge;

	bridge = pci_alloc_host_bridge(0); /* NVMe: host bridge 할당 */
	if (!bridge) /* NVMe: 할당 실패 시 NULL 반환 */
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	bridge->dev.parent = parent; /* NVMe: host bridge의 부모 device 설정 */

	list_splice_init(resources, &bridge->windows); /* NVMe: caller가 제공한 resource window를 bridge로 이동 */
	bridge->sysdata = sysdata; /* NVMe: platform-specific data 연결 */
	bridge->busnr = bus; /* NVMe: root bus 번호 설정 */
	bridge->ops = ops; /* NVMe: PCI config space ops 연결 */

	error = pci_register_host_bridge(bridge); /* NVMe: host bridge 등록 및 root bus 생성; NVMe 트리의 시작점 */
	/* NVMe: 조걸 분기 */
	if (error < 0)
		/* NVMe: 오류 처리/종료 지점으로 이동: err_out */
		goto err_out;

	return bridge->bus; /* NVMe: 생성된 root bus 반환 */

err_out:
	put_device(&bridge->dev); /* NVMe: 등록 실패 시 host bridge 참조 감소 */
	/* NVMe: NULL 반환(실패/끝) */
	return NULL;
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_create_root_bus);

/*
 * pci_host_probe() - PCI host bridge 아래 전체 PCIe 계층 탐색 및 초기화
 *
 * NVMe 연결: host controller driver가 이 함수를 호출하여 Root Complex
 * 아래의 모든 PCIe 장치를 스캔하고, 리소스를 할당하며, 매칭되는
 * 드라이버를 probe. NVMe SSD는 pci_bus_add_devices()에서
 * nvme_pci_driver의 nvme_probe()가 호출되어 초기화.
 */
/* NVMe: 함수 정의: pci_host_probe */
int pci_host_probe(struct pci_host_bridge *bridge)
{
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *bus, *child;
	/* NVMe: 변수 선언/초기화 */
	int ret;

	pci_lock_rescan_remove(); /* NVMe: rescan/remove 상호 배제 잠금 */
	ret = pci_scan_root_bus_bridge(bridge); /* NVMe: root bus 및 전체 계층 스캔; NVMe 컨트롤러 pci_dev 생성 */
	pci_unlock_rescan_remove(); /* NVMe: 잠금 해제 */
	/* NVMe: 조걸 분기 */
	if (ret < 0) {
		dev_err(bridge->dev.parent, "Scanning root bridge failed"); /* NVMe: root bridge 스캔 실패 로그 */
		/* NVMe: 결과 반환: ret */
		return ret;
	}

	bus = bridge->bus; /* NVMe: 스캔이 완료된 root bus 획득 */

	/* If we must preserve the resource configuration, claim now */
	/* NVMe: 조걸 분기 */
	if (bridge->preserve_config)
		pci_bus_claim_resources(bus); /* NVMe: firmware 설정을 유지하며 리소스 claim */

	/*
	 * Assign whatever was left unassigned. If we didn't claim above,
	 * this will reassign everything.
	 */
	pci_assign_unassigned_root_bus_resources(bus); /* NVMe: 할당되지 않은 BAR/버스 번호 할당; NVMe BAR 물리 주소 확정 */

	/* NVMe: 리스트 순회 */
	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child); /* NVMe: 각 서브 버스의 MPS/MRRS 설정; NVMe 링크 효율 결정 */

	pci_lock_rescan_remove(); /* NVMe: device 추가 동안 잠금 */
	pci_bus_add_devices(bus); /* NVMe: 매칭되는 PCI 드라이버 probe 호출; nvme_probe 실행 */
	pci_unlock_rescan_remove(); /* NVMe: 잠금 해제 */

	/*
	 * Ensure pm_runtime_enable() is called for the controller drivers
	 * before calling pci_host_probe(). The PM framework expects that
	 * if the parent device supports runtime PM, it will be enabled
	 * before child runtime PM is enabled.
	 */
	pm_runtime_set_active(&bridge->dev); /* NVMe: host bridge runtime PM 활성 상태 설정 */
	pm_runtime_no_callbacks(&bridge->dev); /* NVMe: bridge 자체에는 runtime PM 콜백 없음 */
	devm_pm_runtime_enable(&bridge->dev); /* NVMe: device-managed runtime PM 활성화; NVMe RTD3의 상위 전원 관리 */

	/* NVMe: 성공 반환 */
	return 0;
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_host_probe);

/* NVMe: 함수 정의: pci_bus_insert_busn_res */
int pci_bus_insert_busn_res(struct pci_bus *b, int bus, int bus_max)
{
	/* NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
	struct resource *res = &b->busn_res;
	/* NVMe: 코드 동작 수행 */
	struct resource *parent_res, *conflict;

	/* NVMe: 구조체 필드에 값 저장: res->start */
	res->start = bus;
	/* NVMe: 구조체 필드에 값 저장: res->end */
	res->end = bus_max;
	/* NVMe: 구조체 필드에 값 저장: res->flags */
	res->flags = IORESOURCE_BUS;

	/* NVMe: 조걸 분기 */
	if (!pci_is_root_bus(b))
		/* NVMe: 비트 연산으로 값 설정/마스크: parent_res */
		parent_res = &b->parent->busn_res;
	/* NVMe: 조걸 분기의 else 경로 */
	else {
		/* NVMe: 함수 호출: get_pci_domain_busn_res */
		parent_res = get_pci_domain_busn_res(pci_domain_nr(b));
		/* NVMe: 구조체 필드에 값 저장: res->flags | */
		res->flags |= IORESOURCE_PCI_FIXED;
	}

	/* NVMe: 리소스 충돌 검사 */
	conflict = request_resource_conflict(parent_res, res);

	/* NVMe: 조걸 분기 */
	if (conflict)
		/* NVMe: 장치 정보 메시지 출력 */
		dev_info(&b->dev,
			   /* NVMe: 함수 호출: pR */
			   "busn_res: can not insert %pR under %s%pR (conflicts with %s %pR)\n",
			    /* NVMe: 루트 버스 여부 확인 */
			    res, pci_is_root_bus(b) ? "domain " : "",
			    /* NVMe: 코드 동작 수행 */
			    parent_res, conflict->name, conflict);

	/* NVMe: 결과 반환: conflict == NULL */
	return conflict == NULL;
}

/* NVMe: 함수 정의: pci_bus_update_busn_res_end */
int pci_bus_update_busn_res_end(struct pci_bus *b, int bus_max)
{
	/* NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
	struct resource *res = &b->busn_res;
	/* NVMe: 변수에 값 할당: struct resource old_res */
	struct resource old_res = *res;
	/* NVMe: 변수 선언/초기화 */
	resource_size_t size;
	/* NVMe: 변수 선언/초기화 */
	int ret;

	/* NVMe: 조걸 분기 */
	if (res->start > bus_max)
		/* NVMe: 오류 코드 반환: -EINVAL */
		return -EINVAL;

	/* NVMe: 변수에 값 할당: size */
	size = bus_max - res->start + 1;
	/* NVMe: 리소스 범위 조정 */
	ret = adjust_resource(res, res->start, size);
	/* NVMe: 장치 정보 메시지 출력 */
	dev_info(&b->dev, "busn_res: %pR end %s updated to %02x\n",
			/* NVMe: 코드 동작 수행 */
			&old_res, ret ? "can not be" : "is", bus_max);

	/* NVMe: 조걸 분기 */
	if (!ret && !res->parent)
		/* NVMe: bus 번호 리소스 삽입 */
		pci_bus_insert_busn_res(b, res->start, res->end);

	/* NVMe: 결과 반환: ret */
	return ret;
}

/* NVMe: 함수 정의: pci_bus_release_busn_res */
void pci_bus_release_busn_res(struct pci_bus *b)
{
	/* NVMe: 비트 연산으로 값 설정/마스크: struct resource *res */
	struct resource *res = &b->busn_res;
	/* NVMe: 변수 선언/초기화 */
	int ret;

	/* NVMe: 조걸 분기 */
	if (!res->flags || !res->parent)
		return;

	/* NVMe: 리소스 해제 */
	ret = release_resource(res);
	/* NVMe: 장치 정보 메시지 출력 */
	dev_info(&b->dev, "busn_res: %pR %s released\n",
			/* NVMe: 코드 동작 수행 */
			res, ret ? "can not be" : "is");
}

/* NVMe: 함수 정의: pci_scan_root_bus_bridge */
int pci_scan_root_bus_bridge(struct pci_host_bridge *bridge)
{
	/* NVMe: 코드 동작 수행 */
	struct resource_entry *window;
	/* NVMe: 변수 선언/초기화 */
	bool found = false;
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *b;
	/* NVMe: 변수 선언/초기화 */
	int max, bus, ret;

	/* NVMe: 조걸 분기 */
	if (!bridge)
		/* NVMe: 오류 코드 반환: -EINVAL */
		return -EINVAL;

	/* NVMe: 함수 호출: resource_list_for_each_entry */
	resource_list_for_each_entry(window, &bridge->windows)
		/* NVMe: 조걸 분기 */
		if (window->res->flags & IORESOURCE_BUS) {
			/* NVMe: 구조체 필드에 값 저장: bridge->busnr */
			bridge->busnr = window->res->start;
			/* NVMe: 변수에 값 할당: found */
			found = true;
			break;
		}

	/* NVMe: 호스트 브리지 등록 */
	ret = pci_register_host_bridge(bridge);
	/* NVMe: 조걸 분기 */
	if (ret < 0)
		/* NVMe: 결과 반환: ret */
		return ret;

	/* NVMe: 변수에 값 할당: b */
	b = bridge->bus;
	/* NVMe: 변수에 값 할당: bus */
	bus = bridge->busnr;

	/* NVMe: 조걸 분기 */
	if (!found) {
		/* NVMe: 장치 정보 메시지 출력 */
		dev_info(&b->dev,
		 /* NVMe: 코드 동작 수행 */
		 "No busn resource found for root bus, will use [bus %02x-ff]\n",
			/* NVMe: 코드 동작 수행 */
			bus);
		/* NVMe: bus 번호 리소스 삽입 */
		pci_bus_insert_busn_res(b, bus, 255);
	}

	/* NVMe: 하위 버스 재귀 스캔 */
	max = pci_scan_child_bus(b);

	/* NVMe: 조걸 분기 */
	if (!found)
		/* NVMe: bus 번호 리소스 끝 갱신 */
		pci_bus_update_busn_res_end(b, max);

	/* NVMe: 성공 반환 */
	return 0;
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_scan_root_bus_bridge);

/* NVMe: 루트 버스 스캔 */
struct pci_bus *pci_scan_root_bus(struct device *parent, int bus,
		/* NVMe: 코드 동작 수행 */
		struct pci_ops *ops, void *sysdata, struct list_head *resources)
{
	/* NVMe: 코드 동작 수행 */
	struct resource_entry *window;
	/* NVMe: 변수 선언/초기화 */
	bool found = false;
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *b;
	/* NVMe: 변수 선언/초기화 */
	int max;

	/* NVMe: 함수 호출: resource_list_for_each_entry */
	resource_list_for_each_entry(window, resources)
		/* NVMe: 조걸 분기 */
		if (window->res->flags & IORESOURCE_BUS) {
			/* NVMe: 변수에 값 할당: found */
			found = true;
			break;
		}

	/* NVMe: 루트 PCI 버스 생성 */
	b = pci_create_root_bus(parent, bus, ops, sysdata, resources);
	/* NVMe: 조걸 분기 */
	if (!b)
		/* NVMe: NULL 반환(실패/끝) */
		return NULL;

	/* NVMe: 조걸 분기 */
	if (!found) {
		/* NVMe: 장치 정보 메시지 출력 */
		dev_info(&b->dev,
		 /* NVMe: 코드 동작 수행 */
		 "No busn resource found for root bus, will use [bus %02x-ff]\n",
			/* NVMe: 코드 동작 수행 */
			bus);
		/* NVMe: bus 번호 리소스 삽입 */
		pci_bus_insert_busn_res(b, bus, 255);
	}

	/* NVMe: 하위 버스 재귀 스캔 */
	max = pci_scan_child_bus(b);

	/* NVMe: 조걸 분기 */
	if (!found)
		/* NVMe: bus 번호 리소스 끝 갱신 */
		pci_bus_update_busn_res_end(b, max);

	/* NVMe: 결과 반환: b */
	return b;
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_scan_root_bus);

/*
 * pci_scan_bus() - 단순 root bus 스캔(레거시 API)
 *
 * NVMe 연결: MEM/IO/BUS 전체 리소스를 사용하여 root bus를 만들고
 * 스캔. 일부 아키텍처나 초기화 코드에서 여전히 사용.
 */
/* NVMe: 함수 호출: pci_scan_bus */
struct pci_bus *pci_scan_bus(int bus, struct pci_ops *ops,
					/* NVMe: 코드 동작 수행 */
					void *sysdata)
{
	/* NVMe: 함수 호출: LIST_HEAD */
	LIST_HEAD(resources);
	/* NVMe: 코드 동작 수행 */
	struct pci_bus *b;

	pci_add_resource(&resources, &ioport_resource); /* NVMe: 전체 I/O port 리소스 추가 */
	pci_add_resource(&resources, &iomem_resource); /* NVMe: 전체 memory 리소스 추가 */
	pci_add_resource(&resources, &busn_resource); /* NVMe: bus 번호 리소스 추가 */
	b = pci_create_root_bus(NULL, bus, ops, sysdata, &resources); /* NVMe: root bus 생성; NVMe BAR 할당 공간 설정 */
	/* NVMe: 조걸 분기 */
	if (b) {
		pci_scan_child_bus(b); /* NVMe: 하위 계층 스캔; NVMe SSD 탐색 */
	/* NVMe: 코드 동작 수행 */
	} else {
		pci_free_resource_list(&resources); /* NVMe: root bus 생성 실패 시 리소스 해제 */
	}
	return b; /* NVMe: 생성된 root bus 또는 NULL 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL */
EXPORT_SYMBOL(pci_scan_bus);

/**
 * pci_rescan_bus_bridge_resize - Scan a PCI bus for devices
 * @bridge: PCI bridge for the bus to scan
 *
 * Scan a PCI bus and child buses for new devices, add them,
 * and enable them, resizing bridge mmio/io resource if necessary
 * and possible.  The caller must ensure the child devices are already
 * removed for resizing to occur.
 *
 * Returns the max number of subordinate bus discovered.
 */
/*
 * pci_rescan_bus_bridge_resize() - bridge 아래 버스를 재스캔하고 리소스 재할당
 *
 * NVMe 연결: hotplug나 SR-IOV VF 추가 등으로 bridge 아래에 새 NVMe
 * 장치가 추가되었을 때 호출. 필요하면 bridge window 크기를 조정.
 */
/* NVMe: 함수 호출: pci_rescan_bus_bridge_resize */
unsigned int pci_rescan_bus_bridge_resize(struct pci_dev *bridge)
{
	/* NVMe: 변수 선언/초기화 */
	unsigned int max;
	struct pci_bus *bus = bridge->subordinate; /* NVMe: 재스캔할 하위 버스 */

	max = pci_scan_child_bus(bus); /* NVMe: 하위 버스 재스캔; 새 NVMe 장치 탐색 */

	pci_assign_unassigned_bridge_resources(bridge); /* NVMe: 새 장치의 BAR를 위해 bridge 리소스 재할당/확장 */

	pci_bus_add_devices(bus); /* NVMe: 새로 발견된 장치에 드라이버 probe; 새 NVMe 컨트롤러 초기화 */

	return max; /* NVMe: 새로 발견된 최대 subordinate bus 번호 반환 */
}

/**
 * pci_rescan_bus - Scan a PCI bus for devices
 * @bus: PCI bus to scan
 *
 * Scan a PCI bus and child buses for new devices, add them,
 * and enable them.
 *
 * Returns the max number of subordinate bus discovered.
 */
/*
 * pci_rescan_bus() - bus를 재스캔하고 리소스 할당 및 드라이버 바인딩
 *
 * NVMe 연결: runtime rescan 요청(예: sysfs echo 1 > rescan) 시 호출.
 * 새로 연결된 NVMe SSD를 발견하고 초기화.
 */
/* NVMe: 함수 호출: pci_rescan_bus */
unsigned int pci_rescan_bus(struct pci_bus *bus)
{
	/* NVMe: 변수 선언/초기화 */
	unsigned int max;

	max = pci_scan_child_bus(bus); /* NVMe: 하위 계층 재스캔; NVMe SSD 탐색 */
	pci_assign_unassigned_bus_resources(bus); /* NVMe: 할당되지 않은 리소스(BAR) 할당 */
	pci_bus_add_devices(bus); /* NVMe: 새 장치에 드라이버 probe; nvme_probe 호출 */

	return max; /* NVMe: 새로 발견된 최대 subordinate bus 번호 반환 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_rescan_bus);

/*
 * pci_rescan_bus(), pci_rescan_bus_bridge_resize() and PCI device removal
 * routines should always be executed under this mutex.
 */
/* NVMe: 뮤텍스 정의 */
DEFINE_MUTEX(pci_rescan_remove_lock);

/* NVMe: 함수 정의: pci_lock_rescan_remove */
void pci_lock_rescan_remove(void)
{
	/* NVMe: 뮤텍스 획득 */
	mutex_lock(&pci_rescan_remove_lock);
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_lock_rescan_remove);

/* NVMe: 함수 정의: pci_unlock_rescan_remove */
void pci_unlock_rescan_remove(void)
{
	/* NVMe: 뮤텍스 해제 */
	mutex_unlock(&pci_rescan_remove_lock);
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_unlock_rescan_remove);

/* NVMe: 함수 호출: pci_sort_bf_cmp */
static int __init pci_sort_bf_cmp(const struct device *d_a,
				  /* NVMe: 코드 동작 수행 */
				  const struct device *d_b)
{
	/* NVMe: device를 pci_dev로 변환 */
	const struct pci_dev *a = to_pci_dev(d_a);
	/* NVMe: device를 pci_dev로 변환 */
	const struct pci_dev *b = to_pci_dev(d_b);

	/* NVMe: 조걸 분기 */
	if      (pci_domain_nr(a->bus) < pci_domain_nr(b->bus)) return -1;
	/* NVMe: 추가 조걸 분기 */
	else if (pci_domain_nr(a->bus) > pci_domain_nr(b->bus)) return  1;

	/* NVMe: 조걸 분기 */
	if      (a->bus->number < b->bus->number) return -1;
	/* NVMe: 추가 조걸 분기 */
	else if (a->bus->number > b->bus->number) return  1;

	/* NVMe: 조걸 분기 */
	if      (a->devfn < b->devfn) return -1;
	/* NVMe: 추가 조걸 분기 */
	else if (a->devfn > b->devfn) return  1;

	/* NVMe: 성공 반환 */
	return 0;
}

/* NVMe: 함수 호출: pci_sort_breadthfirst */
void __init pci_sort_breadthfirst(void)
{
	/* NVMe: 함수 호출: bus_sort_breadthfirst */
	bus_sort_breadthfirst(&pci_bus_type, &pci_sort_bf_cmp);
}

/*
 * pci_hp_add_bridge() - hotplug로 추가된 bridge를 PCI 트리에 통합
 *
 * NVMe 연결: hotplug slot에 새 Root Port/Switch가 추가되었을 때 호출.
 * 이 bridge 아래에 NVMe SSD가 연결될 수 있으므로 bus 번호를 할당하고
 * 하위를 스캔.
 */
/* NVMe: 함수 정의: pci_hp_add_bridge */
int pci_hp_add_bridge(struct pci_dev *dev)
{
	struct pci_bus *parent = dev->bus; /* NVMe: hotplug bridge의 부모 버스 */
	int busnr, start = parent->busn_res.start; /* NVMe: 부모 버스 번호 범위 시작 */
	/* NVMe: 변수 선언/초기화 */
	unsigned int available_buses = 0;
	int end = parent->busn_res.end; /* NVMe: 부모 버스 번호 범위 끝 */

	for (busnr = start; busnr <= end; busnr++) { /* NVMe: 부모 범위 내에서 사용 가능한 bus 번호 탐색 */
		/* NVMe: 조걸 분기 */
		if (!pci_find_bus(pci_domain_nr(parent), busnr))
			break; /* NVMe: 비어있는 bus 번호 찾기 */
	}
	if (busnr-- > end) { /* NVMe: 사용 가능한 bus 번호가 없으면 */
		pci_err(dev, "No bus number available for hot-added bridge\n"); /* NVMe: bus 번호 부족 오류 */
		/* NVMe: 결과 반환: -1 */
		return -1;
	}

	/* Scan bridges that are already configured */
	busnr = pci_scan_bridge(parent, dev, busnr, 0); /* NVMe: pass 0로 이미 설정된 bridge 스캔 */

	/*
	 * Distribute the available bus numbers between hotplug-capable
	 * bridges to make extending the chain later possible.
	 */
	available_buses = end - busnr; /* NVMe: 남은 추가 bus 번호 계산 */

	/* Scan bridges that need to be reconfigured */
	pci_scan_bridge_extend(parent, dev, busnr, available_buses, 1); /* NVMe: pass 1로 재설정 및 하위 스캔; NVMe 장치 탐색 */

	if (!dev->subordinate) /* NVMe: subordinate bus가 생성되지 않았으면 실패 */
		/* NVMe: 결과 반환: -1 */
		return -1;

	return 0; /* NVMe: hotplug bridge 통합 성공 */
}
/* NVMe: 함수 호출: EXPORT_SYMBOL_GPL */
EXPORT_SYMBOL_GPL(pci_hp_add_bridge);
