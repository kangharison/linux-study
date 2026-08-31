// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence PCIe platform  driver.
 *
 * Copyright (c) 2019, Cadence Design Systems
 * Author: Tom Joseph <tjoseph@cadence.com>
 */
/*
 * [한국어 설명] 주변부가 없는 Cadence 평가 보드용 드라이버 (pcie-cadence-plat.c)
 *
 * === 파일의 역할 ===
 * dwc/pcie-designware-plat.c 와 같은 위치의 파일이다. Cadence IP 를
 * 쓰되 SoC 고유의 클럭·리셋·전원 처리가 필요 없는 경우 — Cadence 의
 * 평가 보드나 FPGA 프로토타입 — 를 위한 최소 드라이버다.
 *
 * 호스트(RC)와 엔드포인트(EP) 모드를 둘 다 지원하며, 디바이스 트리의
 * compatible 로 구분한다. 그래서 probe 가 두 갈래로 크게 갈리고,
 * 각 갈래가 서로 다른 구조체를 할당해 서로 다른 setup 함수를 부른다.
 *
 * 이 파일에 고유한 것이 하나 있다. cpu_addr_fixup 이다.
 * 이 보드에서는 CPU 물리 주소의 상위 4비트가 PCIe 쪽으로 나갈 때
 * 의미가 없어, 마스크로 잘라 낸다(CDNS_PLAT_CPU_TO_BUS_ADDR).
 * 공통 코어의 아웃바운드 창 설정이 이 콜백을 통해 주소를 보정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 cdns,cdns-pcie-host 또는 cdns,cdns-pcie-ep 가 있으면
 *   -> [이 파일] cdns_plat_pcie_probe()
 *      -> RC 모드: 호스트 브리지 할당 → cdns_pcie_init_phy()
 *         → cdns_pcie_host_setup() [pcie-cadence-host.c]
 *      -> EP 모드: cdns_pcie_ep 할당 → cdns_pcie_init_phy()
 *         → cdns_pcie_ep_setup() [pcie-cadence-ep.c]
 *
 * 실행 컨텍스트: probe 와 shutdown — 프로세스 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: pcie-cadence.c(PHY, 공통 ops, 전원 관리 표),
 *   pcie-cadence-host.c 또는 pcie-cadence-ep.c.
 * 공유 상태: struct cdns_plat_pcie 가 공통 구조체를 가리킨다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — cdns_ 심볼 호출 0건). 평가 보드용 드라이버라
 * 실제 NVMe 를 붙여 쓰는 대상도 아니다.
 *
 * === 주요 함수/구조체 요약 ===
 * cdns_plat_cpu_addr_fixup() : CPU 주소의 상위 비트를 잘라 낸다.
 * cdns_plat_ops             : 그 콜백 하나만 담은 ops 표.
 * cdns_plat_pcie_probe()    : RC/EP 모드에 따라 갈라 초기화한다.
 * cdns_plat_pcie_shutdown() : 전원을 정리하고 PHY 를 끈다.
 * struct cdns_plat_pcie     : 공통 구조체를 가리키는 얇은 껍데기.
 * cdns_plat_pcie_of_match   : 두 compatible 과 각각의 모드 데이터.
 *
 * === 코드를 읽으며 확인한 사실 (수정하지 않고 기록만) ===
 * 1) probe 의 에러 경로가 0(성공)을 반환한다.
 *    err_init 과 err_get_sync 레이블이 정리를 마친 뒤 return 0 을 한다.
 *    그러면 초기화에 실패했는데도 커널은 probe 가 성공한 것으로 보고
 *    드라이버를 바인딩한 채 남겨 둔다. 원본 스냅숏 1f0e418bb6 에서도
 *    같으므로 상류 코드 그대로이며, 의도인지 실수인지는 판단하지 않는다.
 *
 * 2) probe 가 저장한 것과 shutdown 이 꺼내는 것의 타입이 다르다.
 *    probe:    platform_set_drvdata(pdev, cdns_plat_pcie);
 *              → 저장되는 것은 struct cdns_plat_pcie * 다.
 *    shutdown: struct cdns_pcie *pcie = dev_get_drvdata(dev);
 *              → struct cdns_pcie * 로 받는다.
 *    struct cdns_plat_pcie 는 포인터 하나(8바이트)만 가진 구조체이고,
 *    struct cdns_pcie 는 pcie-cadence.h 에서 확인하면 reg_base, dev,
 *    phy_count, phy 등을 담은 훨씬 큰 구조체다. 그래서 shutdown 이
 *    cdns_pcie_disable_phy(pcie) 를 부르면 8바이트 할당 바깥을
 *    phy_count 로 읽게 된다.
 *    이것도 원본 스냅숏에 그대로 있는 상류 코드다. 코드는 수정하지
 *    않으며, 관찰된 사실만 여기 남긴다.
 */

/* [한국어] 기본 커널 매크로. */
#include <linux/kernel.h>
/* [한국어] of_device_get_match_data — compatible 에 딸린 모드 데이터를
 * 꺼내 RC 인지 EP 인지 판단한다. */
#include <linux/of.h>
/* [한국어] PCI 관련 디바이스 트리 헬퍼. 다만 이 파일이 직접 쓰는
 * of_pci_* 심볼은 확인되지 않았다. */
#include <linux/of_pci.h>
/* [한국어] 플랫폼 드라이버 등록과 devm_pci_alloc_host_bridge. */
#include <linux/platform_device.h>
/* [한국어] 런타임 전원 관리. 이 드라이버는 get_sync 로 전원을 올린 뒤
 * 그 상태를 유지한다. */
#include <linux/pm_runtime.h>
/* [한국어] struct cdns_pcie / _rc / _ep, 공통 ops 타입, PHY 함수,
 * 그리고 cdns_pcie_pm_ops. */
#include "pcie-cadence.h"

/* [한국어] CPU 주소에서 PCIe 버스 주소로 넘어갈 때 남길 비트.
 * 상위 4비트를 잘라 내는 마스크(28비트, 256MB 범위)다.
 * 이 보드의 주소 배치에서 그 상위 비트가 PCIe 쪽에는 의미가 없기
 * 때문이며, 아래 cpu_addr_fixup 이 이 마스크를 적용한다.
 * 그 배치의 근거는 보드 문서에 있고 이 트리에서는 확인할 수 없다. */
#define CDNS_PLAT_CPU_TO_BUS_ADDR	0x0FFFFFFF

/**
 * struct cdns_plat_pcie - private data for this PCIe platform driver
 * @pcie: Cadence PCIe controller
 */
struct cdns_plat_pcie {
	struct cdns_pcie        *pcie;
	/* [한국어] 이 드라이버가 다루는 공통 컨트롤러 구조체를 가리킨다.
	 * RC 모드면 struct cdns_pcie_rc 안의 것을, EP 모드면
	 * struct cdns_pcie_ep 안의 것을 가리키므로, 두 갈래를 하나로
	 * 다루기 위한 간접층이다.
	 * 설정자: cdns_plat_pcie_probe() 가 모드에 따라 채운다.
	 * 읽는 자: probe 의 에러 경로가 PHY 를 끌 때.
	 * 값 범위: 유효한 포인터. probe 성공 이후에는 NULL 이 아니다.
	 * 동기화: probe 시점에만 쓰이므로 별도 보호가 없다. */
};

/* [한국어] of_match 표의 전방 선언. 정의는 파일 아래쪽에 있는데,
 * probe 가 그보다 위에 있어 여기서 미리 알려 둔다.
 * 다만 확인해 보면 probe 는 이 표를 직접 참조하지 않고
 * of_device_get_match_data() 로 데이터만 꺼내 쓴다 — 이 전방 선언이
 * 실제로 필요한지는 이 트리에서 근거를 찾지 못했다. */
static const struct of_device_id cdns_plat_pcie_of_match[];

/* [한국어]
 * cdns_plat_cpu_addr_fixup - CPU 주소를 이 보드의 버스 주소로 보정한다
 *
 * @pcie: 대상 컨트롤러. 이 구현에서는 쓰지 않는다.
 * @cpu_addr: 보정할 CPU 물리 주소.
 * @return: 상위 비트를 잘라 낸 주소.
 *
 * 공통 코어의 아웃바운드 창 설정(cdns_pcie_set_outbound_region)이
 * ops->cpu_addr_fixup 이 있으면 이것을 통해 주소를 보정한다.
 *
 * 이 보드에서는 CPU 주소의 상위 4비트가 PCIe 쪽에 의미가 없어 잘라 낸다.
 * SoC 마다 주소 배치가 달라 이런 보정이 필요한 경우가 있고, 그래서
 * 공통 코어가 콜백으로 열어 둔 것이다.
 *
 * 실행 컨텍스트: 창 설정 중 프로세스 컨텍스트. 순수 계산.
 *
 * 호출 체인:
 *   cdns_pcie_set_outbound_region() [pcie-cadence.c] → [이 함수]
 */
static u64 cdns_plat_cpu_addr_fixup(struct cdns_pcie *pcie, u64 cpu_addr)
{
	/* [한국어] 하위 28비트만 남긴다. */
	return cpu_addr & CDNS_PLAT_CPU_TO_BUS_ADDR;
}

/* [한국어] 이 보드의 ops 표. 콜백이 하나뿐인 것이 이 드라이버가
 * 얼마나 얇은지를 보여 준다 — 주소 보정 말고는 공통 코어의 기본
 * 동작을 그대로 쓴다. */
static const struct cdns_pcie_ops cdns_plat_ops = {
	.cpu_addr_fixup = cdns_plat_cpu_addr_fixup,
};

/* [한국어]
 * cdns_plat_pcie_probe - RC 또는 EP 모드로 이 컨트롤러를 초기화한다
 *
 * @pdev: 디바이스 트리에서 만들어진 플랫폼 장치.
 * @return: 0. (아래 주의 참고 — 실패 경로도 0 을 돌려준다.)
 *
 * compatible 에 딸린 데이터로 RC 인지 EP 인지 알아낸 뒤 두 갈래로 갈린다.
 * 갈래마다 다른 구조체를 할당하고 다른 setup 함수를 부르지만, 그
 * 사이의 절차(PHY 초기화 → 런타임 PM 활성화 → setup)는 같다.
 *
 * 각 갈래의 첫머리에서 IS_ENABLED 로 해당 기능이 빌드에 포함됐는지
 * 확인한다. 커널 설정에서 호스트만 켜고 엔드포인트는 껐다면, EP
 * compatible 을 만나도 붙을 수 없으므로 -ENODEV 로 물러난다.
 *
 * 실행 컨텍스트: probe — 프로세스 컨텍스트. PHY 와 링크 대기로 잠든다.
 *
 * 주의: 에러 경로(err_init, err_get_sync)가 정리를 마친 뒤 0 을
 *   반환한다. 초기화에 실패했는데도 커널은 성공으로 보고 드라이버를
 *   바인딩한 채 남겨 둔다. 원본 스냅숏에도 같으므로 상류 코드 그대로이며,
 *   의도인지 실수인지는 판단하지 않는다.
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → [이 함수]
 *     → cdns_pcie_init_phy() → cdns_pcie_host_setup() 또는 cdns_pcie_ep_setup()
 */
static int cdns_plat_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] compatible 에 딸린 모드 데이터. is_rc 하나만 담고 있다. */
	const struct cdns_plat_pcie_of_data *data;
	struct cdns_plat_pcie *cdns_plat_pcie;
	struct device *dev = &pdev->dev;
	/* [한국어] RC 모드에서만 쓰는 호스트 브리지. EP 갈래에서는 건드리지 않는다. */
	struct pci_host_bridge *bridge;
	/* [한국어] EP 모드에서만 쓰는 엔드포인트 구조체. */
	struct cdns_pcie_ep *ep;
	/* [한국어] RC 모드의 루트 컴플렉스 구조체. 브리지의 private 영역을 가리킨다. */
	struct cdns_pcie_rc *rc;
	/* [한국어] 에러 경로에서 device link 를 끊을 때 쓰는 반복 변수. */
	int phy_count;
	bool is_rc;
	int ret;

	/* [한국어] 어느 compatible 로 매칭됐는지에 딸린 데이터를 꺼낸다.
	 * 이 표를 채우지 않은 compatible 로 들어올 수는 없으므로 NULL 이면
	 * of_match 표의 정의 오류다. */
	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	/* [한국어] 이 한 비트가 아래 전체를 두 갈래로 가른다. */
	is_rc = data->is_rc;

	/* [한국어] dev_dbg 가 아니라 pr_debug 인 것이 눈에 띈다. 이 시점에는
	 * dev 가 이미 유효하므로 dev_dbg 를 쓸 수 있었을 텐데 그러지 않았다.
	 * 그 이유는 이 트리에서 확인하지 못했다. */
	pr_debug(" Started %s with is_rc: %d\n", __func__, is_rc);
	/* [한국어] 두 갈래를 하나로 다루기 위한 얇은 껍데기를 잡는다.
	 * devm 이라 드라이버가 떨어질 때 자동 해제된다. */
	cdns_plat_pcie = devm_kzalloc(dev, sizeof(*cdns_plat_pcie), GFP_KERNEL);
	if (!cdns_plat_pcie)
		return -ENOMEM;

	/* [한국어] 이 껍데기를 drvdata 에 저장한다.
	 * 파일 상단 주석 2)에 적었듯, shutdown 은 이것을
	 * struct cdns_pcie * 로 꺼내 쓴다 — 타입이 맞지 않는다.
	 * 상류 코드 그대로이며 여기서는 사실만 기록한다. */
	platform_set_drvdata(pdev, cdns_plat_pcie);
	if (is_rc) {
		/* [한국어] 호스트 기능이 빌드에 없으면 붙을 수 없다.
		 * IS_ENABLED 는 컴파일 시점 상수라, 꺼져 있으면 이 갈래
		 * 전체가 최적화로 사라지고 -ENODEV 만 남는다. */
		if (!IS_ENABLED(CONFIG_PCIE_CADENCE_PLAT_HOST))
			return -ENODEV;

		/* [한국어] 브리지와 그 private 영역(struct cdns_pcie_rc)을
		 * 함께 잡는다. */
		bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
		if (!bridge)
			return -ENOMEM;

		/* [한국어] private 영역이 곧 rc 구조체다. */
		rc = pci_host_bridge_priv(bridge);
		rc->pcie.dev = dev;
		/* [한국어] 위에서 정의한 ops 를 건다. 공통 코어가 주소 창을
		 * 설정할 때 이 표의 cpu_addr_fixup 을 부른다. */
		rc->pcie.ops = &cdns_plat_ops;
		/* [한국어] 껍데기가 그 안의 공통 구조체를 가리키게 한다.
		 * 아래 에러 경로가 이 포인터로 PHY 를 끈다. */
		cdns_plat_pcie->pcie = &rc->pcie;

		/* [한국어] PHY 를 확보하고 켠다. 링크를 올리기 전에 반드시. */
		ret = cdns_pcie_init_phy(dev, cdns_plat_pcie->pcie);
		if (ret) {
			dev_err(dev, "failed to init phy\n");
			/* [한국어] 이 경로만 실제 오류를 돌려준다. 아래
			 * goto 경로들과 달리 0 으로 뭉개지 않는다. */
			return ret;
		}
		/* [한국어] 런타임 PM 을 켠 뒤 곧바로 참조를 올려 전원을
		 * 활성 상태로 만든다. get_sync 는 전원이 올라올 때까지
		 * 기다리므로 이 호출이 끝나면 하드웨어를 만질 수 있다. */
		pm_runtime_enable(dev);
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			/* [한국어] 전원을 올리지 못하면 하드웨어를 만질 수 없다. */
			dev_err(dev, "pm_runtime_get_sync() failed\n");
			goto err_get_sync;
		}

		/* [한국어] 공통 호스트 초기화. 링크가 올라가고 PCI 코어의
		 * 열거가 시작된다. */
		ret = cdns_pcie_host_setup(rc);
		if (ret)
			goto err_init;
	} else {
		/* [한국어] 엔드포인트 기능이 빌드에 없으면 붙을 수 없다. */
		if (!IS_ENABLED(CONFIG_PCIE_CADENCE_PLAT_EP))
			return -ENODEV;

		/* [한국어] EP 모드는 호스트 브리지가 필요 없다 — 이쪽은
		 * 버스를 만드는 것이 아니라 장치가 되는 것이기 때문이다.
		 * 그래서 구조체만 직접 잡는다. */
		ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
		if (!ep)
			return -ENOMEM;

		ep->pcie.dev = dev;
		/* [한국어] RC 갈래와 같은 ops 를 쓴다. 주소 보정은 방향과
		 * 무관하게 이 보드의 성질이기 때문이다. */
		ep->pcie.ops = &cdns_plat_ops;
		cdns_plat_pcie->pcie = &ep->pcie;

		/* [한국어] RC 갈래와 같은 절차다. PHY 를 켜고, */
		ret = cdns_pcie_init_phy(dev, cdns_plat_pcie->pcie);
		if (ret) {
			dev_err(dev, "failed to init phy\n");
			return ret;
		}

		/* [한국어] 전원을 활성으로 만든 뒤, */
		pm_runtime_enable(dev);
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			/* [한국어] EP 갈래도 같은 이유로 실패를 알린다. */
			dev_err(dev, "pm_runtime_get_sync() failed\n");
			goto err_get_sync;
		}

		/* [한국어] 엔드포인트 초기화를 부른다. 그 안에서 EPC 로
		 * 등록되어 endpoint/ 프레임워크가 이 하드웨어를 쓸 수 있게 된다. */
		ret = cdns_pcie_ep_setup(ep);
		if (ret)
			goto err_init;
	}

	return 0;

 err_init:
 err_get_sync:
	/* [한국어] 두 레이블이 같은 곳을 가리킨다. err_get_sync 로 들어온
	 * 경우 get_sync 가 실패했는데도 put_sync 를 부르는데, 실패해도
	 * 참조 카운트는 올라가 있으므로 내려 주는 것이 맞다 —
	 * pm_runtime_get_sync 의 규약이 그렇다. */
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	/* [한국어] PHY 전원을 끈다. */
	cdns_pcie_disable_phy(cdns_plat_pcie->pcie);
	/* [한국어] device link 도 끊는다. cdns_pcie_init_phy() 가 만든
	 * 것으로, disable_phy 는 전원만 끄고 링크는 그대로 두기 때문에
	 * 여기서 따로 정리한다. */
	phy_count = cdns_plat_pcie->pcie->phy_count;
	while (phy_count--)
		device_link_del(cdns_plat_pcie->pcie->link[phy_count]);

	/* [한국어] 파일 상단 주석 1)에 적은 부분이다. 정리를 다 하고도
	 * 0(성공)을 돌려주므로 커널은 probe 가 성공한 것으로 본다.
	 * 상류 코드 그대로이며 여기서 고치지 않는다. */
	return 0;
}

/* [한국어]
 * cdns_plat_pcie_shutdown - 시스템 종료 시 전원과 PHY 를 정리한다
 *
 * @pdev: 이 플랫폼 장치.
 * @return: 없음.
 *
 * remove 가 아니라 shutdown 이다. 드라이버 언바인드가 아니라 시스템이
 * 꺼지거나 재부팅될 때 불리며, 하드웨어를 조용한 상태로 두어 다음
 * 부팅이나 kexec 이 깨끗한 상태에서 시작하게 하는 것이 목적이다.
 *
 * 주의: 위 probe 는 drvdata 에 struct cdns_plat_pcie * 를 저장하는데
 *   이 함수는 그것을 struct cdns_pcie * 로 받는다. 두 구조체는 크기도
 *   내용도 다르므로(전자는 포인터 하나, 후자는 reg_base·dev·phy_count·
 *   phy 등을 담는다) 아래 disable_phy 가 엉뚱한 자리를 phy_count 로
 *   읽게 된다. 원본 스냅숏 1f0e418bb6 에도 같으므로 상류 코드 그대로이며,
 *   코드를 고치지 않고 관찰된 사실만 기록한다.
 *
 * 실행 컨텍스트: 시스템 종료 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   커널 종료 경로 → 플랫폼 버스 → [이 함수]
 */
static void cdns_plat_pcie_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* [한국어] 위 주의에 적은 타입 불일치가 있는 줄이다. */
	struct cdns_pcie *pcie = dev_get_drvdata(dev);
	int ret;

	/* [한국어] probe 에서 올린 런타임 PM 참조를 내린다. */
	ret = pm_runtime_put_sync(dev);
	if (ret < 0)
		/* [한국어] 종료 중이라 할 수 있는 일이 없으므로 dev_err 이
		 * 아니라 dev_dbg 로만 남긴다. */
		dev_dbg(dev, "pm_runtime_put_sync failed\n");

	pm_runtime_disable(dev);

	/* [한국어] PHY 전원을 끈다. probe 의 에러 경로와 달리 device link 는
	 * 끊지 않는데, 종료 중이라 그 정리가 의미가 없기 때문으로 읽힌다. */
	cdns_pcie_disable_phy(pcie);
}

/* [한국어] cdns,cdns-pcie-host compatible 에 딸릴 데이터. is_rc 가 true 이므로
 * probe 가 호스트 갈래로 간다. */
static const struct cdns_plat_pcie_of_data cdns_plat_pcie_host_of_data = {
	/* [한국어] 호스트 모드임을 나타내는 유일한 필드. */
	.is_rc = true,
};

/* [한국어] cdns,cdns-pcie-ep 에 딸릴 데이터. */
static const struct cdns_plat_pcie_of_data cdns_plat_pcie_ep_of_data = {
	/* [한국어] 엔드포인트 모드. probe 가 아래 갈래로 간다. */
	.is_rc = false,
};

/* [한국어] 이 드라이버가 지원하는 compatible 목록. 위 두 데이터를 각각 연결해,
 * 같은 드라이버가 모드에 따라 다르게 동작하게 한다. */
static const struct of_device_id cdns_plat_pcie_of_match[] = {
	{
		/* [한국어] 호스트 모드로 쓸 때의 compatible. */
		.compatible = "cdns,cdns-pcie-host",
		.data = &cdns_plat_pcie_host_of_data,
	},
	{
		/* [한국어] 엔드포인트 모드로 쓸 때의 compatible. 같은 하드웨어라도 디바이스
		 * 트리에 어느 쪽을 적느냐로 역할이 정해진다. */
		.compatible = "cdns,cdns-pcie-ep",
		.data = &cdns_plat_pcie_ep_of_data,
	},
	{},
};

/* [한국어] 플랫폼 드라이버 정의. */
static struct platform_driver cdns_plat_pcie_driver = {
	/* [한국어] 드라이버 공통 속성. */
	.driver = {
		/* [한국어] sysfs 와 로그에 나타날 이름. */
		.name = "cdns-pcie",
		.of_match_table = cdns_plat_pcie_of_match,
		.pm	= &cdns_pcie_pm_ops,
	},
	.probe = cdns_plat_pcie_probe,
	.shutdown = cdns_plat_pcie_shutdown,
};
module_platform_driver(cdns_plat_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe controller platform driver");
