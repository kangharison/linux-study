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

#include <linux/device.h>		/* NVMe: struct device, dev_info/err/warn() 등 사용 */
#include <linux/io.h>			/* NVMe: ioremap/iounmap, readl/writel 등 MMIO API */
#include <linux/kernel.h>		/* NVMe: 커널 범용 매크로 및 IS_ENABLED 사용 */
#include <linux/module.h>		/* NVMe: EXPORT_SYMBOL_GPL() 등 모듈 매크로 */
#include <linux/pci.h>			/* NVMe: PCI 버스, pci_bus, pci_config_window 정의 */
#include <linux/pci-ecam.h>		/* NVMe: ECAM 관련 구조체/매크로/extern 선언 */
#include <linux/slab.h>			/* NVMe: kzalloc_obj, kzalloc_objs, kfree 사용 */

/*
 * On 64-bit systems, we do a single ioremap for the whole config space
 * since we have enough virtual address range available.  On 32-bit, we
 * ioremap the config space for each bus individually.
 */
static const bool per_bus_mapping = !IS_ENABLED(CONFIG_64BIT);
/* NVMe: 64비트 시스템이면 전체 ECAM 공간을 한 번에 ioremap하고,
 * 32비트 시스템이면 버스별로 나누어 매핑하는 정책을 나타낸다.
 * NVMe 장치가 있는 버스를 개별 매핑할 때 사용된다. */

/*
 * Create a PCI config space window
 *  - reserve mem region
 *  - alloc struct pci_config_window with space for all mappings
 *  - ioremap the config space
 */
struct pci_config_window *pci_ecam_create(struct device *dev,
		struct resource *cfgres, struct resource *busr,
		const struct pci_ecam_ops *ops)
/* NVMe: 호스트 브리지가 NVMe 장치가 연결될 PCI 버스를 위해 ECAM 윈도우를 생성한다.
 * cfgres: ECAM 물리 메모리 영역, busr: 버스 번호 범위, ops: 플랫폼별 ECAM 동작 */
{
	unsigned int bus_shift = ops->bus_shift;
	/* NVMe: 플랫폼별 ECAM에서 버스 번호를 얼마나 시프트할지 결정(기본 20) */
	struct pci_config_window *cfg;
	/* NVMe: 생성할 ECAM 윈도우 객체 */
	unsigned int bus_range, bus_range_max, bsz;
	/* NVMe: 실제 버스 개수, 최대 가능 버스 개수, 버스당 바이트 크기 */
	struct resource *conflict;
	/* NVMe: iomem_resource 충돌 검사용 임시 변수 */
	int err;
	/* NVMe: 에러 코드 저장 */

	if (busr->start > busr->end)
		return ERR_PTR(-EINVAL);
	/* NVMe: 버스 범위가 뒤집혀 있으면 바로 오류 반환 */

	cfg = kzalloc_obj(*cfg);
	/* NVMe: pci_config_window 구조체를 0으로 초기화하며 동적 할당 */
	if (!cfg)
		return ERR_PTR(-ENOMEM);
	/* NVMe: 메모리 할당 실패 시 -ENOMEM 반환 */

	/* ECAM-compliant platforms need not supply ops->bus_shift */
	if (!bus_shift)
		bus_shift = PCIE_ECAM_BUS_SHIFT;
	/* NVMe: bus_shift가 0이면 ECAM 표준값(20)을 사용한다.
	 * 1 버스 = 1MB(256dev × 8func × 4KB) 가정. */

	cfg->parent = dev;
	/* NVMe: 이 ECAM 윈도우의 부모 device(보통 PCI 호스트 브리지) 저장 */
	cfg->ops = ops;
	/* NVMe: read/write/add_bus/remove_bus/map_bus 등 ECAM 동작 등록 */
	cfg->busr.start = busr->start;
	/* NVMe: 버스 범위 시작 번호 저장 */
	cfg->busr.end = busr->end;
	/* NVMe: 버스 범위 마지막 번호 저장 */
	cfg->busr.flags = IORESOURCE_BUS;
	/* NVMe: 이 resource가 PCI 버스 번호 공간임을 표시 */
	cfg->bus_shift = bus_shift;
	/* NVMe: 계산된 bus_shift 저장 */
	bus_range = resource_size(&cfg->busr);
	/* NVMe: 설정된 버스 범위의 개수(end-start+1) 계산 */
	bus_range_max = resource_size(cfgres) >> bus_shift;
	/* NVMe: ECAM 물리 영역 크기로 커버 가능한 최대 버스 수 계산 */
	if (bus_range > bus_range_max) {
		/* NVMe: 요청한 버스 범위가 물리 ECAM 공간보다 큰 경우 */
		bus_range = bus_range_max;
		/* NVMe: 가능한 최대 버스 수로 축소 */
		resource_set_size(&cfg->busr, bus_range);
		/* NVMe: busr resource의 크기도 축소된 값으로 갱신 */
		dev_warn(dev, "ECAM area %pR can only accommodate %pR (reduced from %pR desired)\n",
			 cfgres, &cfg->busr, busr);
		/* NVMe: NVMe 장치가 포함될 버스 범위가 줄었음을 경고 */
	}
	bsz = 1 << bus_shift;
	/* NVMe: 한 버스가 차지하는 바이트 수(일반적으로 1<<20 = 1MB) */

	cfg->res.start = cfgres->start;
	/* NVMe: ECAM 물리 메모리 시작 주소 저장 */
	cfg->res.end = cfgres->end;
	/* NVMe: ECAM 물리 메모리 마지막 주소 저장 */
	cfg->res.flags = IORESOURCE_MEM | IORESOURCE_BUSY;
	/* NVMe: 메모리 리소스이며 사용 중(busy)임을 표시 */
	cfg->res.name = "PCI ECAM";
	/* NVMe: 리소스 이름을 "PCI ECAM"으로 지정 */

	conflict = request_resource_conflict(&iomem_resource, &cfg->res);
	/* NVMe: 시스템 iomem_resource 트리에 ECAM 영역 등록, 충돌 여부 확인 */
	if (conflict) {
		/* NVMe: 다른 드라이버가 이미 이 물리 영역을 사용 중이면 */
		err = -EBUSY;
		/* NVMe: -EBUSY 에러 코드 설정 */
		dev_err(dev, "can't claim ECAM area %pR: address conflict with %s %pR\n",
			&cfg->res, conflict->name, conflict);
		/* NVMe: NVMe 장치 접근을 위한 ECAM 영역 충돌을 오류로 기록 */
		goto err_exit;
		/* NVMe: 생성 실패 처리로 이동하여 메모리 해제 */
	}

	if (per_bus_mapping) {
		/* NVMe: 32비트처럼 버스별로 개별 매핑이 필요한 경우 */
		cfg->winp = kzalloc_objs(*cfg->winp, bus_range);
		/* NVMe: bus_range 개수만큼 void __iomem 포인터 배열 할당 */
		if (!cfg->winp)
			goto err_exit_malloc;
		/* NVMe: 포인터 배열 할당 실패 시 메모리 해제 경로로 이동 */
	} else {
		/* NVMe: 64비트처럼 전체 ECAM 공간을 한 번에 매핑하는 경우 */
		cfg->win = pci_remap_cfgspace(cfgres->start, bus_range * bsz);
		/* NVMe: 전체 ECAM 물리 영역을 가상 주소 공간에 매핑.
		 * NVMe BAR 탐색/MSI-X 설정 시 이 주소 기준으로 접근한다. */
		if (!cfg->win)
			goto err_exit_iomap;
		/* NVMe: ioremap 실패 시 전용 에러 경로로 이동 */
	}

	if (ops->init) {
		/* NVMe: 플랫폼별 ECAM 초기화 콜백이 등록되어 있으면 */
		err = ops->init(cfg);
		/* NVMe: 플랫폼별 추가 초기화 수행(예: quirk 등록) */
		if (err)
			goto err_exit;
		/* NVMe: 초기화 실패 시 생성 실패 처리로 이동 */
	}
	dev_info(dev, "ECAM at %pR for %pR\n", &cfg->res, &cfg->busr);
	/* NVMe: ECAM 윈도우 생성 완료 로그. NVMe 장치가 이 버스 범위 내에서
	 * PCI 설정 공간에 접근할 수 있음을 의미한다. */
	return cfg;
	/* NVMe: 생성된 pci_config_window 포인터 반환 */

err_exit_iomap:
	dev_err(dev, "ECAM ioremap failed\n");
	/* NVMe: ioremap 실패 원인을 로그로 남김 */
err_exit_malloc:
	err = -ENOMEM;
	/* NVMe: 메모리 할당 실패 에러 코드 설정 */
err_exit:
	pci_ecam_free(cfg);
	/* NVMe: 할당된 리소스와 매핑을 해제하고 cfg를 kfree */
	return ERR_PTR(err);
	/* NVMe: 음수 에러 포인터 반환. 상위 pci-host-bridge가 이를 확인 */
}
EXPORT_SYMBOL_GPL(pci_ecam_create);
/* NVMe: pci_ecam_create를 GPL 모듈에 심볼로 낸다.
 * drivers/nvme/host/pci.c는 직접 호출하지 않지만, PCI host generic 드라이버가
 * 이를 사용하여 NVMe 컨트롤러를 탑재할 버스를 만든다. */

void pci_ecam_free(struct pci_config_window *cfg)
/* NVMe: PCI/PCIe 버스 제거 시 호출. NVMe 장치가 분리되거나
 * hotplug로 버스가 사라질 때 관련 ECAM 매핑을 정리한다. */
{
	int i;
	/* NVMe: per_bus_mapping 시 루프 인덱스 */

	if (per_bus_mapping) {
		/* NVMe: 버스별로 ioremap이 분리된 환경이면 */
		if (cfg->winp) {
			/* NVMe: 포인터 배열이 존재할 때 */
			for (i = 0; i < resource_size(&cfg->busr); i++)
				/* NVMe: 버스 범위 내 모든 버스를 순회 */
				if (cfg->winp[i])
					/* NVMe: 해당 버스에 대한 매핑이 있으면 */
					iounmap(cfg->winp[i]);
					/* NVMe: 버스별 ECAM 가상 주소 매핑 해제 */
			kfree(cfg->winp);
			/* NVMe: 포인터 배열 자체를 해제 */
		}
	} else {
		/* NVMe: 64비트 등 전체 공간을 한 번에 매핑한 경우 */
		if (cfg->win)
			iounmap(cfg->win);
		/* NVMe: 전체 ECAM 가상 주소 매핑 해제. 이후 NVMe 설정 공간
		 * 접근은 모두 무효가 되므로 상위에서 먼저 장치를 disable해야 한다. */
	}
	if (cfg->res.parent)
		release_resource(&cfg->res);
	/* NVMe: iomem_resource 트리에서 ECAM 물리 영역 등록 해제 */
	kfree(cfg);
	/* NVMe: pci_config_window 구조체 자체를 해제 */
}
EXPORT_SYMBOL_GPL(pci_ecam_free);
/* NVMe: pci_ecam_free를 GPL 모듈에 심볼로 노출. NVMe 장치 제거 시
 * pci-host-generic 등이 호출하여 설정 공간 접근을 막는다. */

static int pci_ecam_add_bus(struct pci_bus *bus)
/* NVMe: 새 PCI 버스가 생성될 때(예: NVMe SSD가 연결된 하위 버스 탐색 시)
 * per_bus_mapping 환경에서 해당 버스에 대한 ECAM 매핑을 추가한다. */
{
	struct pci_config_window *cfg = bus->sysdata;
	/* NVMe: 이 버스가 속한 ECAM 윈도우 객체 획득 */
	unsigned int bsz = 1 << cfg->bus_shift;
	/* NVMe: 이 ECAM에서 한 버스가 차지하는 바이트 수 계산 */
	unsigned int busn = bus->number;
	/* NVMe: 추가할 버스의 번호(예: NVMe 장치가 달린 bus number) */
	phys_addr_t start;
	/* NVMe: 이 버스의 ECAM 시작 물리 주소 */

	if (!per_bus_mapping)
		return 0;
	/* NVMe: 64비트 전체 매핑 환경이면 별도 버스 추가 작업 불필요 */

	if (busn < cfg->busr.start || busn > cfg->busr.end)
		return -EINVAL;
	/* NVMe: 추가 요청한 버스가 ECAM 윈도우 범위를 벗어나면 오류 */

	busn -= cfg->busr.start;
	/* NVMe: 상대 버스 인덱스로 변환. 배열 인덱스용 0 기준 값 */
	start = cfg->res.start + busn * bsz;
	/* NVMe: 해당 버스의 ECAM 물리 시작 주소 계산 */

	cfg->winp[busn] = pci_remap_cfgspace(start, bsz);
	/* NVMe: 이 버스의 ECAM 영역을 가상 주소로 매핑.
	 * NVMe 컨트롤러가 이 버스에 있으면 이 주소를 통해 BAR/MSI-X를 읽는다. */
	if (!cfg->winp[busn])
		return -ENOMEM;
	/* NVMe: 매핑 실패 시 -ENOMEM 반환. NVMe 장치 인식이 실패할 수 있다. */

	return 0;
	/* NVMe: 버스 추가/매핑 성공 */
}

static void pci_ecam_remove_bus(struct pci_bus *bus)
/* NVMe: PCI 버스가 제거될 때(예: NVMe 장치 hot-remove 시)
 * per_bus_mapping 환경에서 해당 버스의 ECAM 매핑을 해제한다. */
{
	struct pci_config_window *cfg = bus->sysdata;
	/* NVMe: 버스가 속한 ECAM 윈도우 객체 획득 */
	unsigned int busn = bus->number;
	/* NVMe: 제거할 버스 번호 */

	if (!per_bus_mapping || busn < cfg->busr.start || busn > cfg->busr.end)
		return;
	/* NVMe: 전체 매핑 환경이거나 범위 밖 버스면 아무 것도 하지 않음 */

	busn -= cfg->busr.start;
	/* NVMe: 상대 버스 인덱스로 변환 */
	if (cfg->winp[busn]) {
		/* NVMe: 이 버스에 대한 매핑이 존재하면 */
		iounmap(cfg->winp[busn]);
		/* NVMe: 해당 버스의 ECAM 가상 주소 매핑 해제 */
		cfg->winp[busn] = NULL;
		/* NVMe: 포인터를 NULL로 설정하여 재사용/이중 해제 방지 */
	}
}

/*
 * Function to implement the pci_ops ->map_bus method
 */
void __iomem *pci_ecam_map_bus(struct pci_bus *bus, unsigned int devfn,
			       int where)
/* NVMe: pci_ops->map_bus의 실제 구현. NVMe SSD의 특정 장치/함수/오프셋에
 * 해당하는 PCI 설정 공간의 커널 가상 주소를 반환한다.
 * 예: NVMe 컨트롤러 devfn=0, where=BAR0 오프셋 0x10 일 때
 *     BAR 값을 읽기 위해 이 함수가 먼저 호출된다. */
{
	struct pci_config_window *cfg = bus->sysdata;
	/* NVMe: 이 버스의 ECAM 윈도우(==bus->sysdata) 획득 */
	unsigned int bus_shift = cfg->ops->bus_shift;
	/* NVMe: 버스 번호 시프트 값 획득(기본 20) */
	unsigned int devfn_shift = cfg->ops->bus_shift - 8;
	/* NVMe: devfn 시프트 값은 bus_shift-8(기본 12). devfn은 8비트이므로
	 * 상위 12비트만큼 시프트해 device/function offset을 만든다. */
	unsigned int busn = bus->number;
	/* NVMe: 접근할 PCI 버스 번호(NVMe 컨트롤러가 있는 버스) */
	void __iomem *base;
	/* NVMe: 이 버스(또는 전체) ECAM의 기준 가상 주소 */
	u32 bus_offset, devfn_offset;
	/* NVMe: ECAM 내 버스/장치 오프셋 저장 */

	if (busn < cfg->busr.start || busn > cfg->busr.end)
		return NULL;
	/* NVMe: 접근 요청 버스가 ECAM 범위 밖이면 NULL 반환.
	 * NVMe 드라이버의 설정 공간 접근이 실패하게 된다. */

	busn -= cfg->busr.start;
	/* NVMe: ECAM 윈도우 내 상대 버스 인덱스로 변환 */
	if (per_bus_mapping) {
		/* NVMe: 버스별 매핑 환경이면 */
		base = cfg->winp[busn];
		/* NVMe: 이 버스에 대한 ECAM 기준 가상 주소 획득 */
		busn = 0;
		/* NVMe: base가 이미 버스별 시작 주소이므로 버스 오프셋은 0으로 만든다 */
	} else
		base = cfg->win;
	/* NVMe: 전체 매핑 환경이면 전체 ECAM의 기준 가상 주소 사용 */

	if (cfg->ops->bus_shift) {
		/* NVMe: 플랫폼별 bus_shift가 명시되어 있으면 수동으로 오프셋 조합 */
		bus_offset = (busn & PCIE_ECAM_BUS_MASK) << bus_shift;
		/* NVMe: 상대 버스 번호를 ECAM 오프셋으로 변환 */
		devfn_offset = (devfn & PCIE_ECAM_DEVFN_MASK) << devfn_shift;
		/* NVMe: devfn을 ECAM 오프셋으로 변환. NVMe 컨트롤러의
		 * device/function 위치를 결정한다. */
		where &= PCIE_ECAM_REG_MASK;
		/* NVMe: 레지스터 오프셋을 4KB 설정 공간 범위 내로 마스크 */

		return base + (bus_offset | devfn_offset | where);
		/* NVMe: 기준 주소에 버스/장치/레지스터 오프셋을 더해
		 * 실제 설정 공간 가상 주소 반환. NVMe BAR/MSI-X CAP 등을
		 * 읽고 쓸 때 이 주소가 사용된다. */
	}

	return base + PCIE_ECAM_OFFSET(busn, devfn, where);
	/* NVMe: bus_shift가 0이면 ECAM 표준 매크로로 오프셋 계산 */
}
EXPORT_SYMBOL_GPL(pci_ecam_map_bus);
/* NVMe: pci_ecam_map_bus를 GPL 모듈에 노출. pci_generic_config_read/write
 * 등이 NVMe 장치의 설정 공간 접근 시 이 함수를 호출한다. */

/* ECAM ops */
const struct pci_ecam_ops pci_generic_ecam_ops = {
	/* NVMe: 표준 ECAM 동작 테이블. ARM/ACPI PCI host 등에서 사용 */
	.pci_ops	= {
		.add_bus	= pci_ecam_add_bus,
		/* NVMe: 버스 추가 시 ECAM 매핑 추가 */
		.remove_bus	= pci_ecam_remove_bus,
		/* NVMe: 버스 제거 시 ECAM 매핑 해제 */
		.map_bus	= pci_ecam_map_bus,
		/* NVMe: (bus, devfn, where) -> 가상 주소 변환. NVMe 핵심 경로 */
		.read		= pci_generic_config_read,
		/* NVMe: 변환된 주소에서 설정 공간 읽기. NVMe BAR/MSI-X 탐색에 사용 */
		.write		= pci_generic_config_write,
		/* NVMe: 변환된 주소에 설정 공간 쓰기. MSI-X enable/BAR 할당 등 */
	}
};
EXPORT_SYMBOL_GPL(pci_generic_ecam_ops);
/* NVMe: 표준 ECAM ops를 GPL 모듈에 노출. NVMe 장치를 탑재하는
 * PCI host bridge 드라이버가 이 ops를 채택한다. */

#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)
/* NVMe: ACPI 기반 시스템에서 PCI quirk가 필요한 경우에만 아래 ops 컴파일 */
/* ECAM ops for 32-bit access only (non-compliant) */
const struct pci_ecam_ops pci_32b_ops = {
	/* NVMe: 32비트 단위 접근만 가능한 비표준 ECAM용 ops.
	 * 일부 플랫폼은 8/16비트 접근이 불가능해 NVMe BAR 하위 바이트
	 * 읽기에도 32비트 read-modify-write가 필요하다. */
	.pci_ops	= {
		.add_bus	= pci_ecam_add_bus,
		/* NVMe: 버스 추가 시 매핑 추가 */
		.remove_bus	= pci_ecam_remove_bus,
		/* NVMe: 버스 제거 시 매핑 해제 */
		.map_bus	= pci_ecam_map_bus,
		/* NVMe: 동일한 map_bus 사용 */
		.read		= pci_generic_config_read32,
		/* NVMe: 항상 32비트로 설정 공간 읽기 */
		.write		= pci_generic_config_write32,
		/* NVMe: 항상 32비트로 설정 공간 쓰기 */
	}
};

/* ECAM ops for 32-bit read only (non-compliant) */
const struct pci_ecam_ops pci_32b_read_ops = {
	/* NVMe: 쓰기는 표준(8/16/32비트), 읽기만 32비트로 제한된 플랫폼용 ops */
	.pci_ops	= {
		.add_bus	= pci_ecam_add_bus,
		/* NVMe: 버스 추가 시 매핑 추가 */
		.remove_bus	= pci_ecam_remove_bus,
		/* NVMe: 버스 제거 시 매핑 해제 */
		.map_bus	= pci_ecam_map_bus,
		/* NVMe: 동일한 map_bus 사용 */
		.read		= pci_generic_config_read32,
		/* NVMe: 32비트 읽기만 가능 */
		.write		= pci_generic_config_write,
		/* NVMe: 쓰기는 표준 바이트/워드/더블워드 모두 지원 */
	}
};
#endif
/* NVMe: ACPI+QUIRKS 조건 종료. 비표준 ECAM 환경에서도 NVMe 설정
 * 공간 접근이 가능하도록 다양한 ops를 제공한다. */
