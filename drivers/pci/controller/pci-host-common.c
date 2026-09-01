// SPDX-License-Identifier: GPL-2.0
/*
 * Common library for PCI host controller drivers
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/* [한국어] 커널 공통 정의(-ENODEV/-ENOMEM 등 errno). */
/*
 * [한국어 설명] ECAM 기반 PCI 호스트 컨트롤러 공통 라이브러리 (pci-host-common.c)
 *
 * === 파일의 역할 ===
 * 드라이버가 아니라 여러 호스트 컨트롤러 드라이버가 함께 쓰는 라이브러리다.
 * ECAM(Enhanced Configuration Access Mechanism)을 지원하는 컨트롤러라면
 * "DT 에서 config 창을 읽어 매핑하고, 브리지에 ops 를 걸고, 버스를 스캔한다"는
 * 절차가 사실상 동일하기 때문에, 그 공통 부분을 네 개의 EXPORT 함수로 뽑아
 * 두었다. ECAM 은 config 공간 전체를 MMIO 로 평평하게 노출하는 PCIe 표준
 * 방식이라 주소 계산만으로 접근할 수 있고, 그래서 컨트롤러마다 달라지는 부분이
 * struct pci_ecam_ops 하나로 모인다.
 * 이 라이브러리를 쓰는 드라이버는 두 부류다. 자체 상태가 필요 없는 쪽은
 * pci_host_common_probe() 를 .probe 에 그대로 걸고 DT 매칭 테이블만 정의하면
 * 끝난다(pci-host-generic.c 가 그 예다). 자체 private 데이터가 필요한 쪽은
 * 브리지를 직접 할당한 뒤 pci_host_common_init() 만 부른다 — probe 와 init 이
 * 나뉘어 있는 이유가 그것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층으로 보면 PCI 코어(probe.c/bus.c) 아래, 개별 컨트롤러 드라이버 위에
 * 놓인 얇은 중간층이다. 위로는 pci_host_probe() 를 불러 코어에 브리지를
 * 넘기고, 아래로는 drivers/pci/ecam.c 의 pci_ecam_create()/pci_ecam_free() 에
 * 실제 창 매핑을 맡긴다. config 접근 자체는 이 파일이 전혀 관여하지 않는다 —
 * ops 안의 pci_ops 를 브리지에 걸어 두면 이후 모든 접근은 그쪽으로 간다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. probe 경로는 ioremap 과 하위
 * 드라이버 probe 를 유발해 오래 걸릴 수 있고, remove 경로는 전역
 * rescan/remove 뮤텍스를 잡으므로 잠들 수 있는 곳에서만 불려야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci-ecam.h 의 struct pci_ecam_ops / pci_config_window 와
 * pci_ecam_create()/pci_ecam_free(), 그리고 PCI 코어의 pci_host_probe(),
 * devm_pci_alloc_host_bridge(), pci_stop_root_bus()/pci_remove_root_bus(),
 * pci_lock_rescan_remove().
 * 아래쪽: DT — of_address_to_resource() 로 config 창을, 브리지 윈도 목록에서
 * 버스 번호 범위를 얻고, of_device_get_match_data() 로 SoC 별 ops 를 고른다.
 * of_pci_check_probe_only() 는 linux,pci-probe-only 속성을 읽어 전역 플래그를
 * 세우는데, 그 플래그가 서면 코어가 BAR 를 재배정하지 않고 펌웨어 설정을 그대로 쓴다.
 * 이 파일에 의존하는 쪽: pci-host-generic.c 를 비롯해 ECAM 을 쓰는 여러
 * 컨트롤러 드라이버가 네 심볼을 EXPORT_SYMBOL_GPL 로 가져다 쓴다.
 * 데이터 흐름: DT 의 reg 와 bus-range → struct pci_config_window →
 * bridge->sysdata → ECAM 의 map_bus 구현이 그 값으로 주소를 계산 →
 * config 접근. 그리고 브리지 자체는 drvdata 에 심어 두어 remove 가 되찾는다.
 * 공유 상태: 이 파일은 전역 변수를 하나도 두지 않는다. 상태는 전부
 * struct pci_config_window 와 struct pci_host_bridge 안에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - pci_host_common_ecam_create(): DT 의 reg 0번을 config 창으로, 브리지
 *   윈도의 버스 범위와 함께 pci_ecam_create() 에 넘겨 창을 만든다.
 *   그 창의 해제를 devm 액션으로 얹어 호출자가 정리를 신경 쓰지 않게 한다.
 * - pci_host_common_init(): probe-only 플래그 확인 → drvdata 설정 →
 *   창 생성 → 브리지에 sysdata/ops/후크/msi_domain 설정 → pci_host_probe().
 * - pci_host_common_probe(): 매칭 데이터에서 ops 를 꺼내 브리지를 할당하고
 *   init 에 위임하는 통짜 probe. 브리지 private 크기가 0 인 것이 특징이다.
 * - pci_host_common_remove(): 전역 락 아래에서 버스를 정지·제거한다.
 *   ECAM 창은 devm 액션이 처리하므로 여기서 손대지 않는다.
 * - gen_pci_unmap_cfg(): 그 devm 액션의 본체. pci_ecam_create() 가 devm 판이
 *   아니라서 자동 해제를 따로 얹어야 하기 때문에 존재한다.
 * - 이 파일에는 구조체 정의가 없다. 다루는 pci_config_window 와
 *   pci_ecam_ops 는 linux/pci-ecam.h 소유이고, pci_host_bridge 는 PCI 코어 소유다.
 */

#include <linux/kernel.h>
/* [한국어] EXPORT_SYMBOL_GPL 과 MODULE_* 매크로. 이 파일은 여러 컨트롤러 드라이버가
 * 공유하는 라이브러리 모듈이라 심볼 export 가 핵심이다. */
#include <linux/module.h>
/* [한국어] of_device_get_match_data() 와 device_node 정의. */
#include <linux/of.h>
/* [한국어] of_address_to_resource() — DT 의 reg 속성을 struct resource 로 바꾼다. */
#include <linux/of_address.h>
/* [한국어] of_pci_check_probe_only() — DT 에 linux,pci-probe-only 가 있으면
 * 커널이 자원을 재배정하지 않고 펌웨어가 설정한 그대로 쓰게 만든다. */
#include <linux/of_pci.h>
/* [한국어] ECAM(Enhanced Configuration Access Mechanism) 지원 — struct pci_ecam_ops,
 * pci_config_window, pci_ecam_create()/pci_ecam_free().
 * ECAM 은 config 공간 전체를 MMIO 로 평평하게 노출하는 PCIe 표준 방식이라,
 * 주소 계산만으로 접근할 수 있어 별도 접근 코드가 거의 필요 없다. */
#include <linux/pci-ecam.h>
/* [한국어] platform_driver 와 platform_set_drvdata/get_drvdata. */
#include <linux/platform_device.h>

/* [한국어] 이 라이브러리의 자체 헤더 — 아래 네 함수의 선언이 들어 있다. */
#include "pci-host-common.h"

/* [한국어]
 * gen_pci_unmap_cfg - devm 액션으로 등록되는 ECAM 창 해제 콜백
 *
 * @ptr: devm_add_action_or_reset() 에 넘긴 불투명 포인터. 실제로는
 *       struct pci_config_window 다.
 *
 * 왜 필요한가: pci_ecam_create() 는 devm 판이 아니라서 디바이스가 사라질 때
 * 자동으로 해제되지 않는다. 그렇다고 각 드라이버가 remove 에서 직접 해제하게
 * 하면 정리 코드가 흩어진다. 그래서 이 함수를 devm 액션으로 등록해 자동 해제를
 * 얹는다 — 그 덕분에 pci_host_common_ecam_create() 의 호출자는 창 해제를
 * 전혀 신경 쓰지 않아도 된다.
 *
 * 실행 컨텍스트: 디바이스 해제 경로(probe 실패 또는 언바인드), 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 코어의 devm 정리 → [gen_pci_unmap_cfg] → pci_ecam_free()
 */
static void gen_pci_unmap_cfg(void *ptr)
{
	/* [한국어] devm 액션 콜백. 불투명 포인터를 원래 타입으로 되돌려 ECAM 창을 해제한다.
	 * pci_ecam_create() 가 devm 판이 아니라서, 자동 해제를 이 액션으로 얹는다. */
	pci_ecam_free((struct pci_config_window *)ptr);
}

/* [한국어]
 * pci_host_common_ecam_create - DT 에서 config 창을 읽어 ECAM 창을 만든다
 *
 * @dev: 컨트롤러의 struct device. DT 노드와 devm 의 기준이다.
 * @bridge: 호스트 브리지. windows 목록에서 버스 번호 범위를 얻는다.
 * @ops: SoC 별 ECAM 연산 테이블. 버스 오프셋 계산과 config 접근 방식이 여기 담긴다.
 * @return: 준비된 struct pci_config_window 포인터, 또는 실패를 담은 ERR_PTR.
 *
 * 왜 필요한가: ECAM 은 config 공간 전체를 MMIO 로 평평하게 노출하는 PCIe 표준
 * 방식이다. 주소 계산만으로 접근할 수 있어 컨트롤러마다 별도 코드가 거의 필요
 * 없고, 그래서 여러 드라이버가 이 공통 경로를 나눠 쓴다.
 *
 * 동작 과정:
 *   1) DT 의 첫 번째 reg 항목을 자원으로 변환한다. 관례상 0번이 config 창이다.
 *   2) 브리지 윈도 목록에서 버스 번호 범위를 찾는다. ECAM 창의 크기가
 *      버스 개수에 비례하므로 이 정보가 반드시 있어야 한다.
 *   3) pci_ecam_create() 로 창을 만든다 — 그 안에서 ioremap 과 버스별 오프셋
 *      계산 준비가 이루어진다.
 *   4) 해제 액션을 devm 으로 등록해 자동 정리를 얹는다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. ioremap 이 잠들 수 있다.
 *
 * 에러 경로: 네 지점 모두 ERR_PTR 을 돌려준다. 4)의 등록 실패는 _or_reset 판
 * 덕분에 이미 창이 해제된 상태이므로 추가 정리가 필요 없다.
 *
 * 호출 체인:
 *   pci_host_common_init() 또는 드라이버가 직접 → [이 함수]
 *     → of_address_to_resource() → resource_list_first_type()
 *     → pci_ecam_create() → devm_add_action_or_reset(gen_pci_unmap_cfg)
 */
struct pci_config_window *pci_host_common_ecam_create(struct device *dev,
		struct pci_host_bridge *bridge, const struct pci_ecam_ops *ops)
{
	/* [한국어] 각 단계 결과. */
	int err;
	/* [한국어] DT 에서 읽어 올 config 창 자원. */
	struct resource cfgres;
	/* [한국어] 브리지 윈도 목록에서 찾을 버스 번호 자원. */
	struct resource_entry *bus;
	/* [한국어] 만들어 낼 ECAM 창 객체. */
	struct pci_config_window *cfg;

	/* [한국어] DT 의 첫 번째 reg 항목을 자원으로 변환한다. 관례상 0번이 config 창이다. */
	err = of_address_to_resource(dev->of_node, 0, &cfgres);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 무엇이 잘못됐는지 관리자에게 알린다. */
		dev_err(dev, "missing or malformed \"reg\" property\n");
		/* [한국어] 반환형이 포인터이므로 errno 를 ERR_PTR 로 감싼다. */
		return ERR_PTR(err);
	}

	/* [한국어] 브리지 윈도 목록에서 버스 번호 범위를 찾는다. ECAM 창의 크기가
	 * 버스 개수 × 1MB 로 결정되므로 이 정보가 반드시 필요하다. */
	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	/* [한국어] DT 의 bus-range 서술이 없으면, */
	if (!bus)
		/* [한국어] -ENODEV. */
		return ERR_PTR(-ENODEV);

	/* [한국어] config 창과 버스 범위, 그리고 SoC 별 ops 로 ECAM 창을 만든다.
	 * 이 안에서 실제 ioremap 과 버스별 오프셋 계산 준비가 이루어진다. */
	cfg = pci_ecam_create(dev, &cfgres, bus->res, ops);
	/* [한국어] 생성 실패면, */
	if (IS_ERR(cfg))
		/* [한국어] 그 오류 포인터를 그대로 전달한다. */
		return cfg;

	/* [한국어] 해제 액션을 등록해, 디바이스가 사라질 때 위 gen_pci_unmap_cfg 가
	 * 자동으로 불리게 한다. _or_reset 판이라 등록 자체가 실패하면 그 자리에서
	 * 콜백을 즉시 실행해 주므로 창이 새지 않는다. */
	err = devm_add_action_or_reset(dev, gen_pci_unmap_cfg, cfg);
	/* [한국어] 등록 실패는 이미 위에서 창이 해제된 상태라는 뜻이므로, */
	if (err)
		/* [한국어] 오류만 돌려주면 된다. */
		return ERR_PTR(err);

	/* [한국어] 준비가 끝난 창을 돌려준다. */
	return cfg;
}
EXPORT_SYMBOL_GPL(pci_host_common_ecam_create);

/* [한국어]
 * pci_host_common_init - ECAM 창을 만들고 브리지를 코어에 등록한다
 *
 * @pdev: 플랫폼 디바이스.
 * @bridge: 호출자가 이미 할당한 호스트 브리지. private 영역을 자기 용도로 쓰는
 *       드라이버가 직접 할당해 넘길 수 있도록 인자로 받는다.
 * @ops: SoC 별 ECAM 연산 테이블.
 * @return: 0 = 성공, 음수 = 창 생성 또는 버스 스캔 실패.
 *
 * pci_host_common_probe() 와 나뉘어 있는 이유가 이 함수의 존재 이유다.
 * 자체 private 데이터가 필요한 드라이버는 브리지를 직접 할당한 뒤 이 함수만
 * 부르고, 그렇지 않은 드라이버는 probe 쪽을 통째로 쓴다.
 *
 * 동작 과정:
 *   1) of_pci_check_probe_only() — DT 에 linux,pci-probe-only 가 있으면 전역
 *      플래그를 세운다. 그 플래그가 서면 PCI 코어가 BAR 를 재배정하지 않고
 *      펌웨어가 설정한 그대로 쓴다. 가상화 환경이나 펌웨어가 자원을 확정한
 *      플랫폼을 위한 장치다.
 *   2) drvdata 에 브리지를 심는다 — remove 가 그것으로 되찾는다.
 *   3) ECAM 창을 만들고 sysdata 에 심는다. config 접근 함수들이 그 값에서
 *      창 시작 주소와 버스 범위를 얻는다.
 *   4) ops 안의 pci_ops 를 브리지에 걸고, enable/disable_device 후크와
 *      msi_domain 플래그를 설정한다. msi_domain 이 없으면 코어가 하위 장치에
 *      MSI 를 배정하지 않는다.
 *   5) pci_host_probe() 로 버스를 스캔하고 장치를 등록한다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. 하위 드라이버 probe 까지
 * 유발하므로 오래 걸릴 수 있다.
 *
 * 에러 경로: 창 생성 실패만 자체 오류이고, 나머지는 pci_host_probe() 의
 * 반환값을 그대로 전달한다.
 *
 * 호출 체인:
 *   pci_host_common_probe() 또는 드라이버의 자체 probe → [이 함수]
 *     → of_pci_check_probe_only() → pci_host_common_ecam_create()
 *     → pci_host_probe()
 */
int pci_host_common_init(struct platform_device *pdev,
			 struct pci_host_bridge *bridge,
			 const struct pci_ecam_ops *ops)
{
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] 만들어질 ECAM 창. */
	struct pci_config_window *cfg;

	/* [한국어] DT 에 linux,pci-probe-only 가 있는지 확인해 전역 플래그를 세운다.
	 * 그 플래그가 서면 PCI 코어가 BAR 를 재배정하지 않고 펌웨어 설정을 그대로 쓴다 —
	 * 가상화 환경이나 펌웨어가 이미 자원을 확정한 플랫폼을 위한 것이다. */
	of_pci_check_probe_only();

	/* [한국어] remove 가 되찾을 수 있도록 브리지를 심어 둔다. pci_host_common_remove() 가
	 * 이 값을 쓴다. */
	platform_set_drvdata(pdev, bridge);

	/* Parse and map our Configuration Space windows */
	/* [한국어] config 창을 만든다(옆의 상류 주석). */
	cfg = pci_host_common_ecam_create(dev, bridge, ops);
	/* [한국어] 실패 검사. */
	if (IS_ERR(cfg))
		/* [한국어] 오류를 errno 로 되돌려 전달한다. */
		return PTR_ERR(cfg);

	/* [한국어] config 접근 함수들이 bus->sysdata 로 되찾을 창 객체를 심는다.
	 * ECAM 의 map_bus 구현이 이 값에서 창 시작 주소와 버스 범위를 얻는다. */
	bridge->sysdata = cfg;
	/* [한국어] ECAM ops 안에 들어 있는 pci_ops 를 브리지에 건다. const 를 벗기는 캐스팅이
	 * 필요한 것은 pci_host_bridge.ops 가 비상수 포인터이기 때문이다. */
	bridge->ops = (struct pci_ops *)&ops->pci_ops;
	/* [한국어] 장치 활성화 시 SoC 별 후크를 걸어 둔다. 대부분의 컨트롤러에서는 NULL 이다. */
	bridge->enable_device = ops->enable_device;
	/* [한국어] 장치 비활성화 후크도 마찬가지. */
	bridge->disable_device = ops->disable_device;
	/* [한국어] MSI 도메인을 쓰겠다고 표시한다. 이 플래그가 없으면 PCI 코어가
	 * 하위 장치에 MSI 를 배정하지 않는다. */
	bridge->msi_domain = true;

	/* [한국어] 코어에 브리지를 넘겨 버스를 스캔하고 장치를 등록한다. */
	return pci_host_probe(bridge);
}
EXPORT_SYMBOL_GPL(pci_host_common_init);

/* [한국어]
 * pci_host_common_probe - private 데이터가 필요 없는 드라이버를 위한 통짜 probe
 *
 * @pdev: DT 로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공. -ENODEV = 매칭 데이터 없음. -ENOMEM = 브리지 할당 실패.
 *       그 밖의 음수 = 초기화 실패.
 *
 * 여러 SoC 를 한 코드로 다룰 수 있는 이유가 여기 있다. SoC 마다 다른 부분이
 * struct pci_ecam_ops 하나로 모여 있고, 그것을 DT 매칭 데이터로 골라 오기 때문이다.
 * 드라이버는 매칭 테이블만 정의하고 이 함수를 .probe 에 걸면 끝난다.
 *
 * 브리지를 private 크기 0 으로 할당하는 것이 특징이다. 이 경로를 쓰는
 * 드라이버는 자체 상태를 두지 않고 sysdata 의 ECAM 창만 쓴다는 뜻이다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 지점 모두 곧장 return 한다. devm 할당이라 정리할 것이 없다.
 *
 * 호출 체인:
 *   DT 매칭 → platform_driver.probe == [이 함수]
 *     → of_device_get_match_data() → devm_pci_alloc_host_bridge()
 *     → pci_host_common_init()
 */
int pci_host_common_probe(struct platform_device *pdev)
{
	/* [한국어] DT 매칭에서 딸려 온 SoC 별 ECAM ops. */
	const struct pci_ecam_ops *ops;
	/* [한국어] 할당할 호스트 브리지. */
	struct pci_host_bridge *bridge;

	/* [한국어] compatible 문자열에 묶여 있던 ops 를 꺼낸다. 이 라이브러리가 여러 SoC 를
	 * 한 코드로 다룰 수 있는 것은, SoC 마다 다른 부분이 이 ops 하나로 모여 있기 때문이다. */
	ops = of_device_get_match_data(&pdev->dev);
	/* [한국어] 매칭 데이터가 없으면 DT 항목이 잘못된 것이다. */
	if (!ops)
		/* [한국어] -ENODEV. */
		return -ENODEV;

	/* [한국어] private 영역 없이 브리지만 할당한다. 크기 인자가 0 인 것은 이 라이브러리가
	 * 드라이버별 상태를 따로 두지 않고 sysdata 에 ECAM 창만 심기 때문이다. */
	bridge = devm_pci_alloc_host_bridge(&pdev->dev, 0);
	/* [한국어] 메모리 부족. */
	if (!bridge)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 공통 초기화에 위임한다. probe 와 init 을 나눠 둔 이유는, 자체 private
	 * 데이터가 필요한 드라이버가 브리지를 직접 할당한 뒤 init 만 부를 수 있게
	 * 하기 위해서다. */
	return pci_host_common_init(pdev, bridge, ops);
}
EXPORT_SYMBOL_GPL(pci_host_common_probe);

/* [한국어]
 * pci_host_common_remove - 버스를 정지시키고 제거한다
 *
 * @pdev: 제거되는 플랫폼 디바이스.
 *
 * init 이 심어 둔 브리지를 되찾아 버스를 해체한다. 전역 rescan/remove 뮤텍스를
 * 잡는 것이 중요하다 — sysfs 의 rescan/remove 나 핫플러그가 동시에 버스 트리를
 * 변형하면 리스트가 깨지기 때문이다.
 *
 * pci_stop_root_bus() 와 pci_remove_root_bus() 를 나눠 부르는 것은 PCI 코어의
 * 규약이다. 전자는 드라이버를 떼고 후자는 객체를 없앤다.
 *
 * ECAM 창은 여기서 해제하지 않는다 — gen_pci_unmap_cfg 가 devm 액션으로
 * 등록되어 있어 드라이버 코어가 이 함수 이후에 자동으로 처리한다.
 *
 * 실행 컨텍스트: 언바인드 경로, 프로세스 컨텍스트.
 * 하위 드라이버의 remove 가 연쇄로 불리므로 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값도 void 다.
 *
 * 호출 체인:
 *   드라이버 코어의 언바인드 → platform_driver.remove == [이 함수]
 *     → pci_lock_rescan_remove() → pci_stop_root_bus() → pci_remove_root_bus()
 */
void pci_host_common_remove(struct platform_device *pdev)
{
	/* [한국어] probe 가 심어 둔 브리지를 되찾는다. */
	struct pci_host_bridge *bridge = platform_get_drvdata(pdev);

	/* [한국어] 전역 열거/제거 뮤텍스를 잡는다. sysfs rescan/remove 나 핫플러그와
	 * 버스 트리 변형이 겹치면 리스트가 깨지기 때문이다. */
	pci_lock_rescan_remove();
	/* [한국어] 버스의 장치들을 정지시킨다(드라이버 remove 호출). */
	pci_stop_root_bus(bridge->bus);
	/* [한국어] 버스 객체를 제거한다. 정지와 제거를 나눠 부르는 것이 PCI 코어의 규약이다. */
	pci_remove_root_bus(bridge->bus);
	/* [한국어] 락을 푼다. ECAM 창은 devm 액션이 해제하므로 여기서 손대지 않는다. */
	pci_unlock_rescan_remove();
}
EXPORT_SYMBOL_GPL(pci_host_common_remove);

/* [한국어] modinfo 에 표시될 설명. 이 파일이 드라이버가 아니라 라이브러리임을 밝힌다. */
MODULE_DESCRIPTION("Common library for PCI host controller drivers");
/* [한국어] 라이선스 선언. GPL 계열이어야 위 EXPORT_SYMBOL_GPL 심볼들을 쓸 수 있다. */
MODULE_LICENSE("GPL v2");
