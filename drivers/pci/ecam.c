// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2016 Broadcom
 */

/*
 * [한국어 설명] 메모리 매핑 방식 config 접근(ECAM)의 공통 구현 (ecam.c)
 *
 * === 파일의 역할 ===
 * ECAM(Enhanced Configuration Access Mechanism)은 PCIe 가 정한 config 공간
 * 접근 방식이다. config 공간 전체를 물리 메모리 구간에 매핑해 두고,
 * 정해진 규칙으로 주소를 계산해 그냥 읽고 쓰면 된다. 규칙은 스펙에
 * 못박혀 있다 — 주소 = base + (bus << 20) + (devfn << 12) + offset.
 * 버스마다 1MB, 장치+기능마다 4KB 가 배정되는 셈이고, 그 4KB 는 확장 config
 * 공간(0x000~0xFFF)의 크기와 정확히 일치한다.
 * 이 파일이 하는 일은 그 매핑의 관리다. 호스트 브리지 드라이버가 "내 ECAM 은
 * 이 물리 주소에서 시작하고 이 버스 범위를 담당한다" 고 알려 주면, 이 파일이
 * 자원을 등록하고 매핑을 잡고 주소 계산 콜백(map_bus)을 준비한다.
 * 파일 전체가 300줄, 함수 다섯 개, ops 표 셋으로 작다. 그런데 그 안에
 * 두 가지 축이 겹쳐 있어 읽을 때 헷갈리기 쉽다.
 * 첫째 축은 커널 비트 수다. per_bus_mapping 이 그 스위치로, 64비트에서는
 * 창 전체를 한 번에 매핑하고 32비트에서는 가상 주소가 모자라 버스가 나타날
 * 때마다 그 몫만 따로 매핑한다. 다섯 함수 중 넷이 이 분기를 갖고 있고,
 * pci_ecam_add_bus() / pci_ecam_remove_bus() 는 아예 32비트를 위해서만 있다.
 * 둘째 축은 하드웨어가 표준을 따르는가다. 표준을 벗어난 SoC 는 버스당 크기가
 * 1MB 가 아니거나(bus_shift 를 직접 지정), config 접근 폭이 32비트로
 * 제한된다(pci_32b_ops / pci_32b_read_ops).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 초기화: 호스트 브리지 드라이버(controller/ 아래) 또는 ACPI MCFG 처리
 *           → [이 파일] pci_ecam_create(dev, cfgres, busr, ops)
 *              → iomem 자원 등록 → 매핑 → ops->init 이 있으면 SoC 별 추가 초기화
 * 접근:   pci_read_config_word() 등
 *           → bus->ops->read = pci_generic_config_read [access.c]
 *              → bus->ops->map_bus = [이 파일] pci_ecam_map_bus
 *                 → 위 공식으로 주소를 계산해 돌려준다
 * 버스 생성/제거(32비트만): pci_scan_child_bus() 계열
 *           → pci_ops->add_bus / remove_bus
 *              → [이 파일] pci_ecam_add_bus() / pci_ecam_remove_bus()
 * 실행 컨텍스트가 둘로 갈린다. 생성·해제·버스 추가/제거는 프로세스
 * 컨텍스트이며 잠들 수 있다. 반면 map_bus 는 config 접근 경로에서 코어가
 * pci_lock 을 쥔 채 부르므로 잠들면 안 되고, 실제로 계산만 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: controller/ 아래의 여러 호스트 브리지 드라이버(pci-host-generic.c,
 * pcie-hisi.c 등)와 ACPI 기반 시스템의 MCFG 처리. 그들은 pci_ecam_create() 로
 * 창을 만들고, 자기 ops 표에 pci_ecam_map_bus() 를 직접 넣거나 이 파일이
 * 제공하는 pci_generic_ecam_ops 를 통째로 쓴다.
 * 아래쪽: access.c 의 pci_generic_config_read/write 와 그 32비트 판,
 * pci_remap_cfgspace()(평범한 ioremap 이 아니라 config 공간 전용으로,
 * 아키텍처별 메모리 속성 차이를 흡수한다), 그리고 iomem 자원 트리.
 * 옆쪽: linux/pci-ecam.h 의 struct pci_config_window 와 struct pci_ecam_ops,
 * PCIE_ECAM_BUS_SHIFT / _BUS_MASK / _DEVFN_MASK / _REG_MASK / _OFFSET 상수
 * (이 스파스 체크아웃에는 그 헤더가 없어 값은 쓰임새로만 확인했다).
 * 확장 사례: pcie-hisi.c 처럼 루트 포트 config 만 ECAM 창 밖에 두는
 * 하드웨어는 ops->init 으로 별도 창을 매핑하고 map_bus 를 자기 것으로
 * 바꿔 끼운다 — 이 파일의 확장점이 어떻게 쓰이는지 보여 주는 예다.
 * 공유 상태: struct pci_config_window 하나. 물리 주소 범위, 버스 범위,
 * 매핑된 가상 주소(win 또는 winp 배열), bus_shift, ops, 그리고 SoC 별
 * 사설 데이터(priv)를 담는다. bus->sysdata 로 코어가 들고 다닌다.
 *
 * === 주요 함수/구조체 요약 ===
 * - per_bus_mapping: 이 파일을 지배하는 컴파일 시점 상수(!IS_ENABLED(64BIT)).
 *   const 라 컴파일러가 한쪽 분기만 남기므로 런타임 비용이 없다.
 * - pci_ecam_create(): 서술자 할당 → 버스 범위 조정 → 자원 등록 → 매핑 →
 *   ops->init. 요청한 버스 범위가 창보다 크면 오류가 아니라 경고와 함께
 *   줄인다. 오류 라벨 셋이 모두 pci_ecam_free() 로 모이는데, cfg 를
 *   kzalloc 으로 0 초기화한 것이 그 안전장치다.
 * - pci_ecam_free(): 생성의 짝이자 생성 실패의 정리 경로. 두 역할을 겸해
 *   모든 단계를 "있으면 푼다" 로 검사하며, cfg->res.parent 검사가 자원 등록
 *   실패 경로를 받아 준다.
 * - pci_ecam_add_bus() / pci_ecam_remove_bus(): 32비트 전용. 버스가 나타나고
 *   사라질 때 그 몫의 매핑을 잡고 푼다. remove 쪽이 자리를 NULL 로 되돌려야
 *   재등장 시 재매핑과 이중 해제 방지가 모두 성립한다.
 * - pci_ecam_map_bus(): 가장 자주 불리는 함수. 기준 주소를 고를 때
 *   32비트면 버스 오프셋을 0 으로 만든다(이미 버스별로 나눠 매핑했으므로
 *   버스 번호를 두 번 더하면 안 된다). 오프셋 계산은 ops->bus_shift 가
 *   0 인지로 갈리는데, create() 가 cfg->bus_shift 만 보정하고 ops 쪽은
 *   그대로 둔 덕분에 그 값이 "드라이버가 직접 지정했는가" 의 표식이 된다.
 *   devfn_shift = bus_shift - 8 은 버스 번호가 8비트라서이며, 표준이면
 *   12 가 되어 4KB — 확장 config 공간 하나의 크기와 맞는다.
 * - pci_generic_ecam_ops: 표준을 따르는 컨트롤러가 쓰는 기본 표.
 *   bus_shift 를 **지정하지 않는** 것이 핵심이다.
 * - pci_32b_ops / pci_32b_read_ops: ACPI 쿼크 전용. 전자는 읽기와 쓰기
 *   모두, 후자는 읽기만 32비트로 제한된 하드웨어용이며 둘의 차이는 write
 *   한 줄뿐이다. 그런 조합이 실재해서 표가 둘로 나뉜다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 쓰지 않는다(전수 확인).
 * 그러나 NVMe 의 config 공간 접근이 대부분 이 경로를 지난다. NVMe 는 PCIe
 * 장치이고 현대 시스템은 거의 다 ECAM 을 쓰기 때문이다. 더 중요한 것은
 * ECAM 이어야만 확장 config 공간(0x100 이상)에 닿을 수 있다는 점이다.
 * NVMe 의 SR-IOV, AER, ATS 같은 확장 capability 가 보이려면 이 방식이
 * 필수다(access.c 의 pci_ext_cfg_avail 관련 설명 참고).
 * 뒤집어 말하면, 이 파일이 계산하는 주소 하나하나가 NVMe 드라이버의
 * pci_read_config_dword() 한 번에 대응한다.
 */

/* [한국어] struct device 와 dev_info/dev_warn/dev_err. */
#include <linux/device.h>
/* [한국어] iounmap(). 매핑을 만드는 pci_remap_cfgspace() 는 아래 pci.h 쪽에서 온다. */
#include <linux/io.h>
/* [한국어] 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] EXPORT_SYMBOL_GPL. 이 파일의 세 함수와 세 ops 표가 모듈에서 쓰인다. */
#include <linux/module.h>
/* [한국어] struct pci_bus, pci_generic_config_read/write 와 그 32비트 판,
 * pci_remap_cfgspace(). */
#include <linux/pci.h>
/* [한국어] struct pci_config_window, struct pci_ecam_ops, PCIE_ECAM_ 계열 상수 —
 * 이 파일이 구현하는 규약의 정의처다. */
#include <linux/pci-ecam.h>
/* [한국어] kzalloc_obj() / kzalloc_objs() / kfree(). */
#include <linux/slab.h>

/*
 * On 64-bit systems, we do a single ioremap for the whole config space
 * since we have enough virtual address range available.  On 32-bit, we
 * ioremap the config space for each bus individually.
 */
/* [한국어] 위 영어 주석이 이 파일의 가장 큰 분기를 설명한다. 64비트에서는 가상
 * 주소가 넉넉하므로 ECAM 창 전체를 한 번에 매핑하고, 32비트에서는 그럴
 * 수 없어 버스가 나타날 때마다 그 버스 몫만 따로 매핑한다.
 * IS_ENABLED 로 컴파일 시점에 정해지는 const 라, 아래 여러 분기가
 * 컴파일러 최적화로 한쪽만 남는다 — 런타임 비용이 없다. */
static const bool per_bus_mapping = !IS_ENABLED(CONFIG_64BIT);

/*
 * Create a PCI config space window
 *  - reserve mem region
 *  - alloc struct pci_config_window with space for all mappings
 *  - ioremap the config space
 */
/* [한국어]
 * pci_ecam_create - ECAM 창 하나를 만들어 매핑하고 서술자를 돌려준다
 *
 * @dev: 호스트 브리지의 device. 진단 메시지와 부모 관계에 쓴다.
 * @cfgres: ECAM 창의 물리 주소 구간.
 * @busr: 이 창이 담당할 버스 번호 범위.
 * @ops: 드라이버가 준 ops 표. bus_shift, 선택적 init, 그리고 pci_ops 를 담는다.
 * @return: 완성된 창 서술자, 또는 ERR_PTR(-EINVAL / -ENOMEM / -EBUSY /
 *   ops->init 이 준 오류).
 *
 * 호스트 브리지 드라이버나 ACPI MCFG 처리가 부르는 진입점이다.
 *
 * 하는 일은 넷이다. 창 서술자를 할당하고, 버스 범위를 창 크기에 맞게
 * 조정하고, iomem 자원 트리에 등록하고, 매핑을 만든다.
 *
 * 버스 범위 조정이 눈에 띄는 처리다. 요청한 범위가 창에 담기지 않으면
 * 오류로 거절하지 않고 담을 수 있는 만큼으로 줄인 뒤 경고만 남긴다.
 * 창이 작아도 그 안의 버스는 정상적으로 접근되므로, 시스템을 못 쓰게 만들
 * 이유가 없기 때문이다.
 *
 * 매핑 방식이 커널 비트 수에 따라 갈린다(per_bus_mapping). 64비트에서는
 * 창 전체를 한 번에 매핑하고, 32비트에서는 포인터 배열만 할당해 두고
 * 버스가 나타날 때마다 pci_ecam_add_bus() 가 그 몫을 채운다.
 * 가상 주소 공간이 좁아 통짜 매핑이 어렵기 때문이다.
 *
 * bus_shift 처리에 미묘한 점이 있다. ops->bus_shift 가 0 이면 표준값 20 으로
 * 보정해 cfg->bus_shift 에 넣지만, ops->bus_shift 자체는 0 인 채로 남는다.
 * 그래서 나중에 pci_ecam_map_bus() 가 ops->bus_shift 를 보고 "표준인가 변형인가"
 * 를 가를 수 있다. 두 필드가 같지 않다는 것이 의도된 설계다.
 *
 * 오류 처리가 라벨 셋으로 계단을 이루며, 모두 pci_ecam_free() 한 곳으로 모인다.
 * 그것이 안전한 이유는 cfg 를 kzalloc 으로 0 초기화했기 때문이다 —
 * 어느 단계에서 뛰어들든 아직 채우지 않은 포인터가 전부 NULL 이라
 * 해제 함수가 알아서 건너뛴다.
 *
 * 실행 컨텍스트: 호스트 브리지 probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 버스 범위가 뒤집힌 경우만 할당 전이라 바로 반환하고,
 * 나머지는 전부 err_exit 로 모여 pci_ecam_free() 를 거친다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버 / ACPI MCFG → [이 함수]
 *     → kzalloc_obj() → request_resource_conflict()
 *     → pci_remap_cfgspace() 또는 kzalloc_objs()
 *     → ops->init() → (실패 시) pci_ecam_free()
 */
struct pci_config_window *pci_ecam_create(struct device *dev,
		struct resource *cfgres, struct resource *busr,
		const struct pci_ecam_ops *ops)
{
	/* [한국어] 드라이버가 지정한 버스당 주소 폭. 0 이면 아래에서 표준값으로 채운다. */
	unsigned int bus_shift = ops->bus_shift;
	/* [한국어] 만들어 반환할 창 서술자. */
	struct pci_config_window *cfg;
	/* [한국어] 버스 개수, ECAM 창이 담을 수 있는 최대 버스 개수, 버스 하나의 크기. */
	unsigned int bus_range, bus_range_max, bsz;
	/* [한국어] 자원 등록이 충돌했을 때 상대를 받을 곳. */
	struct resource *conflict;
	/* [한국어] 오류 코드. */
	int err;

	/* [한국어] 버스 범위가 뒤집혀 있으면, */
	if (busr->start > busr->end)
		/* [한국어] 잘못된 인자다. 여기서만 ERR_PTR 을 직접 반환하는데, 아직 할당한 것이
		 * 없어 되돌릴 것이 없기 때문이다. */
		return ERR_PTR(-EINVAL);

	/* [한국어] 창 서술자를 0 으로 초기화해 할당한다. 아래 오류 경로가 pci_ecam_free()
	 * 하나로 통일되는 것은 이 0 초기화 덕분이다 — 아직 채우지 않은 포인터가
	 * 모두 NULL 이라 해제 함수가 안전하게 건너뛴다. */
	cfg = kzalloc_obj(*cfg);
	/* [한국어] 할당 실패면, */
	if (!cfg)
		/* [한국어] 메모리 부족. */
		return ERR_PTR(-ENOMEM);

	/* ECAM-compliant platforms need not supply ops->bus_shift */
	/* [한국어] 위 영어 주석대로 표준을 따르는 플랫폼은 bus_shift 를 지정할 필요가 없다. */
	if (!bus_shift)
		/* [한국어] 표준값(20)으로 채운다. 2^20 = 1MB 로, 버스 하나에 1MB 가 배정된다는
		 * PCIe 스펙의 규칙이다. */
		bus_shift = PCIE_ECAM_BUS_SHIFT;

	/* [한국어] 진단 메시지에 쓸 부모 장치. */
	cfg->parent = dev;
	/* [한국어] 드라이버가 준 ops 표. map_bus 와 선택적 init 이 여기 들어 있다. */
	cfg->ops = ops;
	/* [한국어] 담당할 버스 번호의 시작. */
	cfg->busr.start = busr->start;
	/* [한국어] 끝. */
	cfg->busr.end = busr->end;
	/* [한국어] 이것이 버스 번호 자원임을 표시한다. */
	cfg->busr.flags = IORESOURCE_BUS;
	/* [한국어] 보정된 shift 값을 창에 기록한다. 여기 저장되는 것은 보정 **후** 값이라
	 * 언제나 유효하지만, ops->bus_shift 는 0 일 수 있다 — 아래 map_bus 가
	 * 그 차이를 이용해 분기한다. */
	cfg->bus_shift = bus_shift;
	/* [한국어] 요청된 버스 개수. */
	bus_range = resource_size(&cfg->busr);
	/* [한국어] ECAM 창의 물리적 크기로 담을 수 있는 최대 버스 개수. 창 크기를
	 * 버스 하나의 크기로 나누는 것이라 shift 로 표현된다. */
	bus_range_max = resource_size(cfgres) >> bus_shift;
	/* [한국어] 요청이 창보다 크면, */
	if (bus_range > bus_range_max) {
		/* [한국어] 담을 수 있는 만큼으로 줄이고, */
		bus_range = bus_range_max;
		/* [한국어] 자원 구조체의 크기도 함께 줄인다. */
		resource_set_size(&cfg->busr, bus_range);
		/* [한국어] 경고를 남긴다. 오류로 처리하지 않는 이유는, 창이 작아도 그 안의 버스는
		 * 정상적으로 접근할 수 있어 시스템을 못 쓰게 만들 이유가 없기 때문이다. */
		dev_warn(dev, "ECAM area %pR can only accommodate %pR (reduced from %pR desired)\n",
			 cfgres, &cfg->busr, busr);
	}
	/* [한국어] 버스 하나의 바이트 크기. 표준이면 1MB 다. */
	bsz = 1 << bus_shift;

	/* [한국어] 물리 주소 구간의 시작을, */
	cfg->res.start = cfgres->start;
	/* [한국어] 끝을 기록한다. */
	cfg->res.end = cfgres->end;
	/* [한국어] 메모리 자원이며 이미 점유되었음(BUSY)을 표시한다. */
	cfg->res.flags = IORESOURCE_MEM | IORESOURCE_BUSY;
	/* [한국어] /proc/iomem 에 보일 이름. */
	cfg->res.name = "PCI ECAM";

	/* [한국어] iomem 자원 트리에 등록을 시도한다. 충돌하면 그 상대를 반환하는 판이라,
	 * 누구와 겹쳤는지를 진단에 쓸 수 있다. */
	conflict = request_resource_conflict(&iomem_resource, &cfg->res);
	/* [한국어] 충돌했으면, */
	if (conflict) {
		/* [한국어] 장치 사용 중 오류로 표시하고, */
		err = -EBUSY;
		/* [한국어] 겹친 상대의 이름과 범위까지 찍어 준다. ECAM 창이 다른 장치와 겹치는 것은
		 * 펌웨어 설정 문제라, 사용자가 원인을 찾을 수 있게 상세히 남긴다. */
		dev_err(dev, "can't claim ECAM area %pR: address conflict with %s %pR\n",
			&cfg->res, conflict->name, conflict);
		/* [한국어] 공통 정리 경로로. */
		goto err_exit;
	}

	/* [한국어] 32비트 커널이면, */
	if (per_bus_mapping) {
		/* [한국어] 버스 개수만큼의 포인터 배열을 할당한다. 실제 매핑은 각 버스가 나타날 때
		 * pci_ecam_add_bus() 가 채운다. */
		cfg->winp = kzalloc_objs(*cfg->winp, bus_range);
		/* [한국어] 할당 실패면, */
		if (!cfg->winp)
			/* [한국어] 메모리 부족 경로로. */
			goto err_exit_malloc;
	} else {
		/* [한국어] 64비트면 창 전체를 한 번에 매핑한다. 평범한 ioremap 이 아니라
		 * config 공간 전용 함수인 것은, 아키텍처에 따라 쓰기 결합이나 추측 실행을
		 * 금지하는 메모리 속성이 필요하기 때문이다. */
		cfg->win = pci_remap_cfgspace(cfgres->start, bus_range * bsz);
		/* [한국어] 매핑 실패면, */
		if (!cfg->win)
			/* [한국어] 매핑 실패 경로로. */
			goto err_exit_iomap;
	}

	/* [한국어] 드라이버가 추가 초기화를 요구하면, */
	if (ops->init) {
		/* [한국어] 불러 준다. pcie-hisi.c 처럼 ECAM 바깥의 레지스터 창이 더 필요한
		 * 드라이버가 여기서 준비를 마친다. */
		err = ops->init(cfg);
		/* [한국어] 실패하면, */
		if (err)
			/* [한국어] 공통 정리 경로로. 이때 err 은 ops->init 이 준 값을 그대로 쓴다. */
			goto err_exit;
	}
	/* [한국어] 성공 로그. dmesg 에서 어느 물리 주소가 어느 버스 범위를 담당하는지
	 * 확인할 수 있다. */
	dev_info(dev, "ECAM at %pR for %pR\n", &cfg->res, &cfg->busr);
	/* [한국어] 완성된 창을 돌려준다. */
	return cfg;

/* [한국어] 매핑 실패 라벨. 로그만 남기고 아래로 흘러내린다. */
err_exit_iomap:
	/* [한국어] 어떤 단계에서 실패했는지 남긴다. */
	dev_err(dev, "ECAM ioremap failed\n");
/* [한국어] 메모리 부족 라벨. 위에서 흘러내려 온 경우와 winp 할당 실패가 여기서 만난다. */
err_exit_malloc:
	/* [한국어] 두 경우 모두 -ENOMEM 이다. */
	err = -ENOMEM;
/* [한국어] 공통 정리 라벨. */
err_exit:
	/* [한국어] 할당했던 것을 모두 되돌린다. 어느 단계에서 뛰어들어도 안전한 이유는
	 * cfg 가 kzalloc 으로 0 초기화되어 있어, 아직 채우지 않은 포인터가 모두
	 * NULL 이기 때문이다. */
	pci_ecam_free(cfg);
	/* [한국어] 오류를 포인터에 실어 반환한다. */
	return ERR_PTR(err);
}
/* [한국어] 모듈로 빌드되는 호스트 브리지 드라이버들이 쓴다. */
EXPORT_SYMBOL_GPL(pci_ecam_create);

/* [한국어]
 * pci_ecam_free - ECAM 창의 매핑과 자원, 서술자를 모두 되돌린다
 *
 * @cfg: pci_ecam_create() 가 만든 창 서술자.
 *
 * 생성의 짝이자, 생성 도중 실패했을 때의 공통 정리 경로이기도 하다.
 * 두 역할을 겸하기 때문에 모든 단계를 "있으면 푼다" 로 검사한다.
 *
 * 매핑 해제는 생성과 같은 방식으로 갈린다. 32비트면 포인터 배열을 순회하며
 * 매핑된 버스만 골라 풀고 배열도 해제하고, 64비트면 창 하나만 푼다.
 * 아직 나타나지 않은 버스의 자리는 NULL 이라 자연스럽게 건너뛴다.
 *
 * 자원 해제에 cfg->res.parent 검사가 붙어 있는 것은,
 * request_resource_conflict() 가 실패한 경로에서도 이 함수가 불리기 때문이다.
 * 등록되지 않은 자원을 release_resource() 에 넘기면 안 된다.
 *
 * 실행 컨텍스트: 호스트 브리지 해제 또는 probe 실패 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없고 실패할 수 있는 동작도 없다.
 *
 * 호출 체인:
 *   호스트 브리지 remove / pci_ecam_create() 의 오류 경로 → [이 함수]
 *     → iounmap() → kfree() → release_resource() → kfree()
 */
void pci_ecam_free(struct pci_config_window *cfg)
{
	/* [한국어] 버스별 매핑을 순회할 인덱스. */
	int i;

	/* [한국어] 32비트 커널이면, */
	if (per_bus_mapping) {
		/* [한국어] 포인터 배열이 할당되어 있을 때만, */
		if (cfg->winp) {
			/* [한국어] 버스 개수만큼 돌며, */
			for (i = 0; i < resource_size(&cfg->busr); i++)
				/* [한국어] 매핑된 버스만 골라, */
				if (cfg->winp[i])
					/* [한국어] 해제한다. 아직 나타나지 않은 버스는 NULL 이라 건너뛴다. */
					iounmap(cfg->winp[i]);
			/* [한국어] 배열 자체도 해제한다. */
			kfree(cfg->winp);
		}
	} else {
		/* [한국어] 64비트면 창 하나만 확인해, */
		if (cfg->win)
			/* [한국어] 매핑되어 있으면 해제한다. */
			iounmap(cfg->win);
	}
	/* [한국어] 자원 트리에 등록되어 있으면(parent 가 있으면), */
	if (cfg->res.parent)
		/* [한국어] 뗀다. 이 검사가 필요한 이유는 request_resource_conflict() 가 실패한
		 * 경로에서도 이 함수가 불리기 때문이다. */
		release_resource(&cfg->res);
	/* [한국어] 마지막으로 서술자 자체를 해제한다. */
	kfree(cfg);
}
/* [한국어] pci_ecam_create() 와 짝으로 내보낸다. */
EXPORT_SYMBOL_GPL(pci_ecam_free);

/* [한국어]
 * pci_ecam_add_bus - 32비트 커널에서 새로 나타난 버스의 config 창을 매핑한다
 *
 * @bus: 코어가 막 만든 PCI 버스.
 * @return: 0 = 성공(또는 64비트라 할 일 없음), -EINVAL = 담당 범위 밖,
 *   -ENOMEM = 매핑 실패.
 *
 * 64비트 커널에서는 즉시 0 을 반환하고 끝난다. 창이 이미 통째로 매핑되어
 * 있기 때문이다. 이 함수의 존재 이유는 오직 32비트 커널이다 —
 * 가상 주소 공간이 좁아 ECAM 창 전체를 한 번에 매핑할 수 없으므로,
 * 버스가 실제로 나타날 때마다 그 몫(기본 1MB)만 따로 매핑한다.
 *
 * 버스 하나의 크기를 계산할 때 ops->bus_shift 가 아니라 cfg->bus_shift 를
 * 쓰는 것이 중요하다. 전자는 표준 플랫폼에서 0 인 채로 남지만,
 * 후자는 pci_ecam_create() 가 20 으로 보정해 둔 값이라 언제나 유효하다.
 *
 * per_bus_mapping 이 컴파일 시점 상수라, 64비트 빌드에서는 이 함수의 몸통이
 * 사실상 `return 0;` 하나로 접힌다.
 *
 * 실행 컨텍스트: 버스 생성 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 오류는 코어의 버스 생성 경로로 올라가 그 버스를 만들지 못하게 한다.
 *
 * 호출 체인:
 *   pci_scan_child_bus() 계열 → pci_ops->add_bus → [이 함수]
 *     → pci_remap_cfgspace()
 */
static int pci_ecam_add_bus(struct pci_bus *bus)
{
	/* [한국어] 코어가 bus->sysdata 에 넣어 둔 창 서술자. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] 버스 하나의 크기. 여기서는 ops->bus_shift 가 아니라 보정된 cfg->bus_shift
	 * 를 쓴다 — 표준 플랫폼에서도 올바른 크기가 나와야 하기 때문이다. */
	unsigned int bsz = 1 << cfg->bus_shift;
	/* [한국어] 이 버스의 번호. */
	unsigned int busn = bus->number;
	/* [한국어] 매핑할 물리 주소. */
	phys_addr_t start;

	/* [한국어] 64비트면 창이 이미 통째로 매핑되어 있으므로, */
	if (!per_bus_mapping)
		/* [한국어] 할 일이 없다. */
		return 0;

	/* [한국어] 우리가 담당하는 범위 밖의 버스면, */
	if (busn < cfg->busr.start || busn > cfg->busr.end)
		/* [한국어] 거절한다. 코어가 범위 밖 버스를 만들려 한다는 뜻이므로 오류다. */
		return -EINVAL;

	/* [한국어] 범위 시작을 빼서 0 기준 인덱스로 만든다. */
	busn -= cfg->busr.start;
	/* [한국어] 그 인덱스로 물리 주소를 계산한다. */
	start = cfg->res.start + busn * bsz;

	/* [한국어] 그 버스 몫만 매핑한다. */
	cfg->winp[busn] = pci_remap_cfgspace(start, bsz);
	/* [한국어] 실패하면, */
	if (!cfg->winp[busn])
		/* [한국어] 메모리 부족. 이 오류는 코어의 버스 생성 경로로 올라간다. */
		return -ENOMEM;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * pci_ecam_remove_bus - 32비트 커널에서 사라지는 버스의 config 창 매핑을 푼다
 *
 * @bus: 제거되는 PCI 버스.
 *
 * pci_ecam_add_bus() 의 짝이다. 64비트이거나 담당 범위 밖이면 할 일이 없다.
 *
 * add_bus 가 세 조건을 각각 다른 반환값으로 구분한 것과 달리 여기서는
 * 한 줄로 묶었다. 반환값이 없어 어느 이유로 물러났는지 알릴 방법이
 * 없기 때문이다.
 *
 * 매핑을 푼 뒤 그 자리를 NULL 로 되돌리는 것이 중요하다. 그래야 같은 버스가
 * 다시 나타났을 때 add_bus 가 새로 매핑할 수 있고, pci_ecam_free() 가
 * 나중에 순회할 때 이중 해제를 하지 않는다.
 *
 * 실행 컨텍스트: 버스 제거 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_remove_bus() 계열 → pci_ops->remove_bus → [이 함수] → iounmap()
 */
static void pci_ecam_remove_bus(struct pci_bus *bus)
{
	/* [한국어] 창 서술자. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] 버스 번호. */
	unsigned int busn = bus->number;

	/* [한국어] 64비트이거나 범위 밖이면 해제할 매핑이 없다. add_bus 쪽과 달리 세 조건을
	 * 한 줄로 묶은 것은 반환값이 없어 구분할 필요가 없기 때문이다. */
	if (!per_bus_mapping || busn < cfg->busr.start || busn > cfg->busr.end)
		/* [한국어] 조용히 돌아간다. */
		return;

	/* [한국어] 0 기준 인덱스로. */
	busn -= cfg->busr.start;
	/* [한국어] 매핑되어 있으면, */
	if (cfg->winp[busn]) {
		/* [한국어] 해제하고, */
		iounmap(cfg->winp[busn]);
		/* [한국어] NULL 로 되돌린다. 이렇게 해야 같은 버스가 다시 나타났을 때
		 * add_bus 가 새로 매핑할 수 있고, pci_ecam_free() 도 이중 해제를 피한다. */
		cfg->winp[busn] = NULL;
	}
}

/*
 * Function to implement the pci_ops ->map_bus method
 */
/* [한국어]
 * pci_ecam_map_bus - ECAM 공식으로 config 접근 주소를 계산한다
 *
 * @bus: 대상 버스. sysdata 에 창 서술자가 들어 있다.
 * @devfn: 장치(상위 5비트) + 기능(하위 3비트).
 * @where: config 공간 안의 바이트 오프셋.
 * @return: 접근할 __iomem 주소. 담당 범위 밖이면 NULL.
 *
 * 이 파일의 핵심이자 가장 자주 불리는 함수다. access.c 의
 * pci_generic_config_read/write 가 매 접근마다 이것을 부른다.
 *
 * 기준 주소를 고르는 데서 커널 비트 수의 차이가 다시 나타난다.
 * 32비트면 그 버스 전용 매핑을 기준으로 삼고 버스 오프셋을 0 으로 만든다 —
 * 이미 버스별로 매핑을 나눠 두었으므로 주소 안에 버스 번호를 다시 넣으면
 * 같은 값을 두 번 더하는 셈이 되기 때문이다. 64비트면 통짜 매핑이 기준이고
 * 버스 번호가 오프셋에 그대로 들어간다.
 *
 * 오프셋 계산이 두 갈래인 것이 이 함수의 설계다. 갈림길은
 * cfg->ops->bus_shift 가 0 인지 아닌지인데, pci_ecam_create() 가
 * cfg->bus_shift 만 20 으로 보정하고 ops->bus_shift 는 건드리지 않은 덕분에
 * 그 값이 "드라이버가 직접 지정했는가" 의 표식으로 남는다.
 * 지정했으면 마스크와 shift 로 직접 계산하고, 아니면 표준 전용 매크로
 * PCIE_ECAM_OFFSET() 이 같은 일을 한다.
 *
 * devfn_shift 가 bus_shift - 8 인 이유는 버스 번호가 8비트이기 때문이다.
 * 표준이면 20 - 8 = 12 이고, 2^12 = 4KB 는 확장 config 공간 하나의 크기와
 * 정확히 맞는다. ECAM 의 주소 배치가 스펙의 구조를 그대로 반영한다.
 *
 * 세 조각을 더하기가 아니라 OR 로 합치는 것도 그 덕분이다 —
 * 마스크와 shift 를 맞춰 두어 자리가 겹치지 않는다.
 *
 * 실행 컨텍스트: config 접근 경로. 코어가 pci_lock 을 쥔 채 부르므로
 * 잠들면 안 된다. 이 함수는 계산만 한다.
 *
 * 에러 경로: 담당 범위 밖이면 NULL. 호출자가 그것을 장치 없음으로 처리한다.
 *
 * 호출 체인:
 *   pci_read/write_config_*() → pci_generic_config_read/write [access.c]
 *     → pci_ops->map_bus → [이 함수]
 */
void __iomem *pci_ecam_map_bus(struct pci_bus *bus, unsigned int devfn,
			       int where)
{
	/* [한국어] 창 서술자. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] 드라이버가 지정한 shift. **보정 전** 값이라 0 일 수 있다. */
	unsigned int bus_shift = cfg->ops->bus_shift;
	/* [한국어] devfn 이 들어갈 자리의 shift. 버스 8비트를 빼면 devfn 자리가 나온다 —
	 * 표준이면 20 - 8 = 12 이고, 2^12 = 4KB 로 확장 config 공간 하나의 크기와
	 * 정확히 맞는다. */
	unsigned int devfn_shift = cfg->ops->bus_shift - 8;
	/* [한국어] 버스 번호. */
	unsigned int busn = bus->number;
	/* [한국어] 계산의 기준이 될 매핑 주소. */
	void __iomem *base;
	/* [한국어] 버스와 devfn 이 만드는 오프셋. */
	u32 bus_offset, devfn_offset;

	/* [한국어] 담당 범위 밖이면, */
	if (busn < cfg->busr.start || busn > cfg->busr.end)
		/* [한국어] NULL 을 반환한다. 호출자(access.c 의 generic 접근자)가 이것을 보고
		 * 장치 없음으로 처리한다. */
		return NULL;

	/* [한국어] 0 기준 인덱스로. */
	busn -= cfg->busr.start;
	/* [한국어] 32비트면, */
	if (per_bus_mapping) {
		/* [한국어] 그 버스 전용 매핑을 기준으로 삼고, */
		base = cfg->winp[busn];
		/* [한국어] 버스 오프셋을 0 으로 만든다. 이미 버스별로 매핑을 나눠 두었으므로
		 * 주소 안에 버스 번호를 다시 넣으면 안 되기 때문이다. */
		busn = 0;
	} else
		/* [한국어] 64비트면 통짜 매핑이 기준이고, 버스 번호가 오프셋에 그대로 반영된다. */
		base = cfg->win;

	/* [한국어] 드라이버가 shift 를 **직접 지정한** 경우다. 위 create() 에서 0 이면
	 * cfg->bus_shift 를 20 으로 보정했지만 ops->bus_shift 는 그대로 0 이라,
	 * 이 검사가 "표준인가 변형인가" 를 가르는 표식이 된다. */
	if (cfg->ops->bus_shift) {
		/* [한국어] 버스 번호를 마스크로 자르고 지정된 폭만큼 민다. */
		bus_offset = (busn & PCIE_ECAM_BUS_MASK) << bus_shift;
		/* [한국어] devfn 도 마찬가지로 자르고 민다. */
		devfn_offset = (devfn & PCIE_ECAM_DEVFN_MASK) << devfn_shift;
		/* [한국어] 레지스터 오프셋은 확장 config 공간 크기(4KB) 안으로 자른다. */
		where &= PCIE_ECAM_REG_MASK;

		/* [한국어] 세 조각을 OR 로 합친다. 자리가 겹치지 않게 마스크와 shift 를 맞춰
		 * 두었으므로 더하기 대신 OR 로 충분하다. */
		return base + (bus_offset | devfn_offset | where);
	}

	/* [한국어] 표준 경우는 전용 매크로가 같은 계산을 한다. 두 갈래로 나눈 이유는
	 * 표준 경로를 매크로 한 줄로 유지해 컴파일러가 상수 접기를 할 수 있게
	 * 하려는 것으로 보인다. */
	return base + PCIE_ECAM_OFFSET(busn, devfn, where);
}
/* [한국어] 호스트 브리지 드라이버들이 자기 ops 표에 이 함수를 직접 넣는다. */
EXPORT_SYMBOL_GPL(pci_ecam_map_bus);

/* ECAM ops */
const struct pci_ecam_ops pci_generic_ecam_ops = {
	.pci_ops	= {
		/* [한국어] 버스가 생길 때 32비트 커널에서 그 버스 몫을 매핑한다. */
		.add_bus	= pci_ecam_add_bus,
		/* [한국어] 버스가 사라질 때 되돌린다. */
		.remove_bus	= pci_ecam_remove_bus,
		/* [한국어] 표준 주소 계산. */
		.map_bus	= pci_ecam_map_bus,
		/* [한국어] 폭 그대로 읽는 접근자. */
		.read		= pci_generic_config_read,
		/* [한국어] 폭 그대로 쓰는 접근자. */
		.write		= pci_generic_config_write,
	}
/* [한국어] bus_shift 를 지정하지 않았다는 점이 중요하다. 0 으로 남으므로
 * 위 map_bus 가 매크로 경로를 타고, create() 는 cfg->bus_shift 를
 * 20 으로 보정한다. */
};
/* [한국어] 가장 널리 쓰이는 표 — 표준을 따르는 모든 ECAM 컨트롤러가 이것을 쓴다. */
EXPORT_SYMBOL_GPL(pci_generic_ecam_ops);

/* [한국어] 아래 두 표는 ACPI 쿼크로만 쓰인다. 표준을 지키지 않는 하드웨어를
 * MCFG 정보만으로 다뤄야 하는 경우이기 때문이다. */
#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)
/* ECAM ops for 32-bit access only (non-compliant) */
/* [한국어] 읽기와 쓰기 모두 32비트만 가능한 하드웨어용. */
const struct pci_ecam_ops pci_32b_ops = {
	.pci_ops	= {
		/* [한국어] 주소 계산과 버스 매핑은 표준과 같다. */
		.add_bus	= pci_ecam_add_bus,
		/* [한국어] 마찬가지. */
		.remove_bus	= pci_ecam_remove_bus,
		/* [한국어] 마찬가지. */
		.map_bus	= pci_ecam_map_bus,
		/* [한국어] 언제나 dword 를 읽어 필요한 바이트를 소프트웨어로 뽑아낸다. */
		.read		= pci_generic_config_read32,
		/* [한국어] 1/2바이트 쓰기를 dword 읽기-수정-쓰기로 처리한다. */
		.write		= pci_generic_config_write32,
	}
};

/* ECAM ops for 32-bit read only (non-compliant) */
/* [한국어] 읽기만 32비트 제약이 있고 쓰기는 정상인 하드웨어용. 이런 조합이 실재해서
 * 표가 둘로 나뉜다. */
const struct pci_ecam_ops pci_32b_read_ops = {
	.pci_ops	= {
		/* [한국어] 동일. */
		.add_bus	= pci_ecam_add_bus,
		/* [한국어] 동일. */
		.remove_bus	= pci_ecam_remove_bus,
		/* [한국어] 동일. */
		.map_bus	= pci_ecam_map_bus,
		/* [한국어] 읽기만 32비트 전용, */
		.read		= pci_generic_config_read32,
		/* [한국어] 쓰기는 폭 그대로. 위 표와 이 한 줄만 다르다. */
		.write		= pci_generic_config_write,
	}
};
/* [한국어] ACPI 쿼크 조건 끝. */
#endif
