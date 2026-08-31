// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2016 Broadcom
 */

/*
 * [한국어 설명] 메모리 매핑 방식 config 접근의 공통 구현 (ecam.c)
 *
 * === 파일의 역할 ===
 * ECAM(Enhanced Configuration Access Mechanism)은 PCIe 가 정한 config space
 * 접근 방식이다. config space 전체를 물리 메모리 구간에 매핑해 두고,
 * 정해진 규칙으로 주소를 계산해 그냥 읽고 쓰면 된다.
 *
 * 주소 계산 규칙이 스펙에 못박혀 있다:
 *   주소 = base + (bus << 20) + (devfn << 12) + offset
 * 버스마다 1MB, 장치+function 마다 4KB 가 배정되는 셈이다. 4KB 는 확장
 * config space(0x000~0xFFF)의 크기와 정확히 일치한다.
 *
 * 이 파일은 그 매핑을 관리하는 공통 코드를 제공한다. 호스트 브리지
 * 드라이버가 "내 ECAM 은 이 물리 주소에서 시작하고 이 버스 범위를 담당한다"
 * 고 알려 주면, 이 파일이 매핑을 잡고 주소 계산 콜백(map_bus)을 준비한다.
 * 그 콜백이 access.c 의 pci_generic_config_read/write 와 짝을 이룬다.
 *
 * 변형도 다룬다. 어떤 SoC 는 버스당 1MB 가 아니라 다른 크기를 쓰거나
 * (bus_shift 를 조정), ECAM 창을 한 번에 다 매핑하지 못해 버스마다
 * 따로 매핑해야 한다. pci_ecam_ops 의 변형들이 그것을 흡수한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 초기화: 호스트 브리지 드라이버(controller/ 아래) 또는 ACPI MCFG 처리
 *           -> [이 파일] pci_ecam_create(dev, cfgres, busr, ops)
 *              -> ECAM 창을 ioremap 하고 struct pci_config_window 를 만든다
 *              -> ops->init 이 있으면 SoC 별 추가 초기화
 *
 * 접근:   pci_read_config_word 등
 *           -> bus->ops->read = pci_generic_config_read [access.c]
 *              -> bus->ops->map_bus = [이 파일] pci_ecam_map_bus
 *                 -> 위 공식으로 주소를 계산해 돌려준다
 *
 * 실행 컨텍스트: 생성/해제는 프로세스 컨텍스트. map_bus 는 config 접근
 * 경로에서 pci_lock 을 쥔 채 불리므로 잠들 수 없고 계산만 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: controller/ 아래의 여러 호스트 브리지 드라이버,
 *   그리고 ACPI 기반 시스템의 MCFG 처리(pci-acpi.c).
 * 아래쪽: access.c 의 pci_generic_config_read/write, ioremap.
 * 공유 상태: struct pci_config_window — ECAM 창 하나를 나타낸다.
 *   물리 주소 범위, 버스 범위, 매핑된 가상 주소, 그리고 ops 를 담는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * 하지만 NVMe 의 config space 접근이 대부분 이 경로를 지난다. NVMe 는
 * PCIe 장치이고 현대 시스템은 거의 다 ECAM 을 쓰기 때문이다.
 * 그리고 ECAM 이어야만 확장 config space(0x100 이상)에 닿을 수 있어,
 * NVMe 의 SR-IOV, AER, ATS 같은 확장 capability 가 보이려면
 * 이 방식이 필수다(access.c 의 pci_ext_cfg_avail 주석 참고).
 *
 * === 주요 함수/구조체 요약 ===
 * pci_ecam_create()      : ECAM 창을 만들고 매핑한다. 버스 범위가 크면
 *                          한 번에 매핑하지 못할 수 있어 버스별 매핑도
 *                          지원한다.
 * pci_ecam_free()        : 매핑을 풀고 구조체를 해제한다.
 * pci_ecam_map_bus()     : 표준 주소 공식으로 위치를 계산한다.
 *                          bus_shift 를 곱하는 방식이라 변형도 흡수한다.
 * pci_32b_ops / pci_32b_read_ops : 32비트 접근만 되는 하드웨어용 변형.
 *                          access.c 의 pci_generic_config_read32 와 짝이다.
 * pci_ecam_ops           : 기본 ops. bus_shift = 20 과 표준 map_bus.
 * struct pci_config_window : ECAM 창 하나. 물리/가상 주소, 버스 범위,
 *                          그리고 SoC 별 사설 데이터를 담는다.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci-ecam.h>
#include <linux/slab.h>

/*
 * On 64-bit systems, we do a single ioremap for the whole config space
 * since we have enough virtual address range available.  On 32-bit, we
 * ioremap the config space for each bus individually.
 */
static const bool per_bus_mapping = !IS_ENABLED(CONFIG_64BIT);

/*
 * Create a PCI config space window
 *  - reserve mem region
 *  - alloc struct pci_config_window with space for all mappings
 *  - ioremap the config space
 */
struct pci_config_window *pci_ecam_create(struct device *dev,
		struct resource *cfgres, struct resource *busr,
		const struct pci_ecam_ops *ops)
{
	unsigned int bus_shift = ops->bus_shift;
	struct pci_config_window *cfg;
	unsigned int bus_range, bus_range_max, bsz;
	struct resource *conflict;
	int err;

	if (busr->start > busr->end)
		return ERR_PTR(-EINVAL);

	cfg = kzalloc_obj(*cfg);
	if (!cfg)
		return ERR_PTR(-ENOMEM);

	/* ECAM-compliant platforms need not supply ops->bus_shift */
	if (!bus_shift)
		bus_shift = PCIE_ECAM_BUS_SHIFT;

	cfg->parent = dev;
	cfg->ops = ops;
	cfg->busr.start = busr->start;
	cfg->busr.end = busr->end;
	cfg->busr.flags = IORESOURCE_BUS;
	cfg->bus_shift = bus_shift;
	bus_range = resource_size(&cfg->busr);
	bus_range_max = resource_size(cfgres) >> bus_shift;
	if (bus_range > bus_range_max) {
		bus_range = bus_range_max;
		resource_set_size(&cfg->busr, bus_range);
		dev_warn(dev, "ECAM area %pR can only accommodate %pR (reduced from %pR desired)\n",
			 cfgres, &cfg->busr, busr);
	}
	bsz = 1 << bus_shift;

	cfg->res.start = cfgres->start;
	cfg->res.end = cfgres->end;
	cfg->res.flags = IORESOURCE_MEM | IORESOURCE_BUSY;
	cfg->res.name = "PCI ECAM";

	conflict = request_resource_conflict(&iomem_resource, &cfg->res);
	if (conflict) {
		err = -EBUSY;
		dev_err(dev, "can't claim ECAM area %pR: address conflict with %s %pR\n",
			&cfg->res, conflict->name, conflict);
		goto err_exit;
	}

	if (per_bus_mapping) {
		cfg->winp = kzalloc_objs(*cfg->winp, bus_range);
		if (!cfg->winp)
			goto err_exit_malloc;
	} else {
		cfg->win = pci_remap_cfgspace(cfgres->start, bus_range * bsz);
		if (!cfg->win)
			goto err_exit_iomap;
	}

	if (ops->init) {
		err = ops->init(cfg);
		if (err)
			goto err_exit;
	}
	dev_info(dev, "ECAM at %pR for %pR\n", &cfg->res, &cfg->busr);
	return cfg;

err_exit_iomap:
	dev_err(dev, "ECAM ioremap failed\n");
err_exit_malloc:
	err = -ENOMEM;
err_exit:
	pci_ecam_free(cfg);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(pci_ecam_create);

void pci_ecam_free(struct pci_config_window *cfg)
{
	int i;

	if (per_bus_mapping) {
		if (cfg->winp) {
			for (i = 0; i < resource_size(&cfg->busr); i++)
				if (cfg->winp[i])
					iounmap(cfg->winp[i]);
			kfree(cfg->winp);
		}
	} else {
		if (cfg->win)
			iounmap(cfg->win);
	}
	if (cfg->res.parent)
		release_resource(&cfg->res);
	kfree(cfg);
}
EXPORT_SYMBOL_GPL(pci_ecam_free);

static int pci_ecam_add_bus(struct pci_bus *bus)
{
	struct pci_config_window *cfg = bus->sysdata;
	unsigned int bsz = 1 << cfg->bus_shift;
	unsigned int busn = bus->number;
	phys_addr_t start;

	if (!per_bus_mapping)
		return 0;

	if (busn < cfg->busr.start || busn > cfg->busr.end)
		return -EINVAL;

	busn -= cfg->busr.start;
	start = cfg->res.start + busn * bsz;

	cfg->winp[busn] = pci_remap_cfgspace(start, bsz);
	if (!cfg->winp[busn])
		return -ENOMEM;

	return 0;
}

static void pci_ecam_remove_bus(struct pci_bus *bus)
{
	struct pci_config_window *cfg = bus->sysdata;
	unsigned int busn = bus->number;

	if (!per_bus_mapping || busn < cfg->busr.start || busn > cfg->busr.end)
		return;

	busn -= cfg->busr.start;
	if (cfg->winp[busn]) {
		iounmap(cfg->winp[busn]);
		cfg->winp[busn] = NULL;
	}
}

/*
 * Function to implement the pci_ops ->map_bus method
 */
void __iomem *pci_ecam_map_bus(struct pci_bus *bus, unsigned int devfn,
			       int where)
{
	struct pci_config_window *cfg = bus->sysdata;
	unsigned int bus_shift = cfg->ops->bus_shift;
	unsigned int devfn_shift = cfg->ops->bus_shift - 8;
	unsigned int busn = bus->number;
	void __iomem *base;
	u32 bus_offset, devfn_offset;

	if (busn < cfg->busr.start || busn > cfg->busr.end)
		return NULL;

	busn -= cfg->busr.start;
	if (per_bus_mapping) {
		base = cfg->winp[busn];
		busn = 0;
	} else
		base = cfg->win;

	if (cfg->ops->bus_shift) {
		bus_offset = (busn & PCIE_ECAM_BUS_MASK) << bus_shift;
		devfn_offset = (devfn & PCIE_ECAM_DEVFN_MASK) << devfn_shift;
		where &= PCIE_ECAM_REG_MASK;

		return base + (bus_offset | devfn_offset | where);
	}

	return base + PCIE_ECAM_OFFSET(busn, devfn, where);
}
EXPORT_SYMBOL_GPL(pci_ecam_map_bus);

/* ECAM ops */
const struct pci_ecam_ops pci_generic_ecam_ops = {
	.pci_ops	= {
		.add_bus	= pci_ecam_add_bus,
		.remove_bus	= pci_ecam_remove_bus,
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};
EXPORT_SYMBOL_GPL(pci_generic_ecam_ops);

#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)
/* ECAM ops for 32-bit access only (non-compliant) */
const struct pci_ecam_ops pci_32b_ops = {
	.pci_ops	= {
		.add_bus	= pci_ecam_add_bus,
		.remove_bus	= pci_ecam_remove_bus,
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read32,
		.write		= pci_generic_config_write32,
	}
};

/* ECAM ops for 32-bit read only (non-compliant) */
const struct pci_ecam_ops pci_32b_read_ops = {
	.pci_ops	= {
		.add_bus	= pci_ecam_add_bus,
		.remove_bus	= pci_ecam_remove_bus,
		.map_bus	= pci_ecam_map_bus,
		.read		= pci_generic_config_read32,
		.write		= pci_generic_config_write,
	}
};
#endif
