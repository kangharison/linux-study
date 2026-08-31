// SPDX-License-Identifier: GPL-2.0
/*
 * pcie-sg2042 - PCIe controller driver for Sophgo SG2042 SoC
 *
 * Copyright (C) 2025 Sophgo Technology Inc.
 * Copyright (C) 2025 Chen Wang <unicorn_wang@outlook.com>
 */

/*
 * [한국어 설명] Sophgo SG2042 RISC-V SoC 의 PCIe 호스트 (pcie-sg2042.c)
 *
 * === 파일의 역할 ===
 * SG2042 는 64코어 RISC-V 서버 칩이다. PCIe 컨트롤러로 Cadence IP 를
 * 쓰므로 이 파일이 하는 일은 주변부뿐이다 — 호스트 브리지를 만들고,
 * PHY 를 켜고, 공통 코어에 넘긴다.
 *
 * 130줄 남짓의 짧은 드라이버인데, 그중 두 가지가 이 SoC 에 고유하다.
 *
 * 첫째, config 접근 폭 제약이다. 상류 주석이 밝히듯 루트 포트 자신의
 * config 는 4바이트 정렬 접근만 받아들이고, 그 아래 장치들은 1/2/4바이트
 * 모두 가능하다. 그래서 pci_ops 를 두 벌 두고 bridge->ops 와
 * bridge->child_ops 로 나눠 건다. 루트용은 pci_generic_config_read32
 * (읽어서 필요한 바이트만 꺼내는 방식), 하위용은 pci_generic_config_read
 * (요청한 폭 그대로 접근)를 쓴다.
 *
 * 둘째, ASPM 이 망가져 있다. quirk_broken_aspm_l0s 와 _l1 을 둘 다
 * 세우는데, 이 SoC 의 ASPM 구현에 문제가 있어 절전 상태로 들어가면
 * 링크가 깨지기 때문으로 읽힌다. 그 결과 이 보드에서는 PCIe 링크
 * 절전이 아예 동작하지 않는다 — 전력을 더 쓰지만 안정적이다.
 * (그 결함의 구체적 내용은 이 트리에서 확인할 수 없다.)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 sophgo,sg2042-pcie-host 가 있으면
 *   -> [이 파일] sg2042_pcie_probe()
 *      -> 호스트 브리지 할당, quirk 설정, 런타임 PM 준비
 *      -> cdns_pcie_init_phy() [pcie-cadence.c]
 *      -> cdns_pcie_host_setup() [pcie-cadence-host.c]
 *         -> 링크를 올리고 PCI 코어의 열거로 이어진다
 *
 * 실행 컨텍스트: probe/remove 와 PM 콜백 — 전부 프로세스 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: pcie-cadence.c(PHY, 공통 코어), pcie-cadence-host.c(호스트 설정),
 *   PHY 서브시스템, PCI 코어의 범용 config 접근 함수.
 * 공유 상태: struct cdns_pcie_rc 를 브리지의 private 영역에 둔다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 다만 이 SoC 는 RISC-V 서버용이라 NVMe 를 실제로 붙여 쓰는 대상이다.
 * 그때 알아 둘 점이 위의 ASPM quirk 다 — 이 보드에서는 NVMe 링크가
 * L0s 나 L1 로 내려가지 않으므로, 아이들 전력이 다른 플랫폼보다
 * 높게 나온다. 드라이브나 커널 설정 문제가 아니라 호스트 쪽 제약이다.
 *
 * === 주요 함수/구조체 요약 ===
 * sg2042_pcie_probe()      : 브리지를 만들고 quirk 를 세운 뒤 공통 코어에
 *                            넘긴다. 이 파일의 본체.
 * sg2042_pcie_remove()     : 호스트를 내리고 PHY 를 끈다.
 * sg2042_pcie_suspend_noirq() / _resume_noirq() : 절전 시 PHY 를 끄고 켠다.
 *                            공통 코어에 같은 함수가 있지만 이 드라이버는
 *                            자기 것을 따로 두었다(내용은 사실상 같다).
 * sg2042_pcie_root_ops     : 루트 포트용 config 접근. 32비트 전용.
 * sg2042_pcie_child_ops    : 하위 장치용. 임의 폭 접근 가능.
 * sg2042_pcie_of_match      : 지원 compatible.
 */

/* [한국어] of_device_id 등 디바이스 트리 매칭 구조체. */
#include <linux/mod_devicetable.h>
/* [한국어] struct pci_ops 와 pci_generic_config_read/write 계열.
 * 이 파일이 config 접근 표를 두 벌 정의하므로 필요하다. */
#include <linux/pci.h>
/* [한국어] 플랫폼 드라이버 등록과 devm_pci_alloc_host_bridge. */
#include <linux/platform_device.h>
/* [한국어] 런타임 전원 관리. 아래에서 이 장치를 활성 상태로 표시하고
 * 콜백 없이 관리하도록 설정한다. */
#include <linux/pm_runtime.h>

/* [한국어] struct cdns_pcie_rc, cdns_pci_map_bus, 그리고 공통 코어의
 * PHY·호스트 설정 함수 선언. */
#include "pcie-cadence.h"

/*
 * SG2042 only supports 4-byte aligned access, so for the rootbus (i.e. to
 * read/write the Root Port itself, read32/write32 is required. For
 * non-rootbus (i.e. to read/write the PCIe peripheral registers, supports
 * 1/2/4 byte aligned access, so directly using read/write should be fine.
 */

/* [한국어] 루트 포트 자신의 config 접근 표.
 * 상류 주석대로 이 SoC 는 루트 버스에 대해 4바이트 정렬 접근만
 * 받아들인다. pci_generic_config_read32 는 항상 32비트로 읽은 뒤
 * 요청한 바이트만 꺼내 주는 구현이라 그 제약을 만족한다.
 * 쓰기 쪽은 더 까다로운데, 1바이트를 쓰려면 32비트를 읽어 해당
 * 바이트만 바꾼 뒤 다시 32비트로 써야 한다 — write32 가 그것을 한다.
 * map_bus 는 주소 계산만 하므로 공통 코어의 것을 그대로 쓴다. */
static struct pci_ops sg2042_pcie_root_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= pci_generic_config_read32,
	.write		= pci_generic_config_write32,
};

/* [한국어] 루트 아래 장치들의 config 접근 표.
 * 이쪽은 1/2/4바이트 접근이 모두 되므로 범용 구현을 그대로 쓴다.
 * 읽고-고쳐-쓰기가 없어 위 루트용보다 빠르고, 특히 쓰기에서
 * 부작용이 있는 레지스터(write-1-to-clear 등)를 다룰 때 안전하다. */
static struct pci_ops sg2042_pcie_child_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

/* [한국어]
 * sg2042_pcie_probe - SG2042 의 PCIe 호스트를 초기화한다
 *
 * @pdev: 디바이스 트리에서 만들어진 플랫폼 장치.
 * @return: 0 이면 성공. 실패 시 음수 오류.
 *
 * 순서가 정해져 있다. 브리지를 만들고 → config 접근 방법을 정하고 →
 * quirk 를 세우고 → 런타임 PM 을 준비하고 → PHY 를 켜고 →
 * 공통 코어에 넘긴다.
 *
 * quirk 를 host_setup 전에 세우는 것이 중요하다. 공통 코어가 그 값을
 * 보고 ASPM 을 설정할지 말지 정하기 때문이다.
 *
 * 실행 컨텍스트: probe — 프로세스 컨텍스트. PHY 초기화와 링크 대기가
 *   있어 잠들 수 있다.
 *
 * 에러 경로: PHY 초기화 실패는 devm 이 정리하므로 그냥 물러난다.
 *   호스트 설정 실패는 이미 켠 PHY 를 직접 꺼야 한다 — cdns_pcie_init_phy
 *   가 PHY 를 켜기까지 하는데 그것은 devres 로 자동 정리되지 않기 때문이다.
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → [이 함수]
 *     → cdns_pcie_init_phy() → cdns_pcie_host_setup()
 */
static int sg2042_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* [한국어] PCI 코어가 다룰 호스트 브리지. 아래에서 private 영역과 함께 잡는다. */
	struct pci_host_bridge *bridge;
	/* [한국어] 그 private 영역 안의 공통 컨트롤러 구조체. */
	struct cdns_pcie *pcie;
	struct cdns_pcie_rc *rc;
	int ret;

	/* [한국어] 브리지와 그 private 영역(struct cdns_pcie_rc)을 함께
	 * 잡는다. devm 이라 드라이버가 떨어질 때 자동 해제된다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge)
		return dev_err_probe(dev, -ENOMEM, "Failed to alloc host bridge!\n");

	/* [한국어] 위에서 정의한 두 접근 표를 건다. ops 는 루트 버스,
	 * child_ops 는 그 아래 모든 버스에 쓰인다 — PCI 코어가 버스
	 * 번호를 보고 알아서 갈라 쓴다. */
	bridge->ops = &sg2042_pcie_root_ops;
	bridge->child_ops = &sg2042_pcie_child_ops;

	/* [한국어] 브리지의 private 영역이 곧 우리 rc 구조체다. */
	rc = pci_host_bridge_priv(bridge);
	/* [한국어] 이 SoC 의 ASPM 이 망가져 있어 L0s 와 L1 을 둘 다 막는다.
	 * 공통 코어가 이 플래그를 보고 해당 상태를 활성화하지 않는다.
	 * host_setup 전에 세워야 효과가 있다. */
	rc->quirk_broken_aspm_l0s = 1;
	rc->quirk_broken_aspm_l1 = 1;
	/* [한국어] rc 안에 박힌 공통 구조체를 꺼내 device 를 연결한다. */
	pcie = &rc->pcie;
	pcie->dev = dev;

	/* [한국어] PM 콜백들이 dev_get_drvdata 로 이 포인터를 꺼내 쓴다.
	 * rc 가 아니라 pcie 를 저장하는 점에 주의 — remove 에서는
	 * container_of 로 rc 를 되찾는다. */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] 런타임 PM 을 준비한다. set_active 는 "지금 켜져 있다" 고
	 * 알리는 것이고, no_callbacks 는 런타임 절전 콜백을 두지 않겠다는
	 * 뜻이다 — 이 드라이버는 시스템 절전만 다루고 런타임 절전은
	 * 하지 않는다. 그래도 enable 하는 것은 하위 장치들이 런타임 PM 을
	 * 쓸 수 있게 부모가 준비되어 있어야 하기 때문이다. */
	pm_runtime_set_active(dev);
	pm_runtime_no_callbacks(dev);
	devm_pm_runtime_enable(dev);

	/* [한국어] PHY 를 확보하고 켠다. */
	ret = cdns_pcie_init_phy(dev, pcie);
	if (ret)
		/* [한국어] init_phy 안에서 실패한 것은 그쪽이 정리했고,
		 * 배열은 devm 이라 자동 해제된다. 여기서 할 일이 없다. */
		return dev_err_probe(dev, ret, "Failed to init phy!\n");

	/* [한국어] 공통 코어에 넘긴다. 여기서 링크가 올라가고 PCI 코어의
	 * 열거가 시작된다. */
	ret = cdns_pcie_host_setup(rc);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to setup host!\n");
		/* [한국어] 이미 켠 PHY 를 꺼야 한다. init_phy 가 켜기까지
		 * 했는데 그것은 devres 대상이 아니기 때문이다.
		 * dev_err_probe 의 반환값을 쓰지 않고 ret 를 따로 돌려주는
		 * 것은 그 사이에 정리 코드를 끼워야 해서다. */
		cdns_pcie_disable_phy(pcie);
		return ret;
	}

	return 0;
}

/* [한국어] sg2042_pcie_remove - 호스트를 내리고 PHY 를 끈다
 * 
 * @pdev: 제거되는 플랫폼 장치.
 * @return: 없음.
 * 
 * probe 의 역순이다. 호스트를 먼저 내려 그 아래 장치들이 정리되게 한
 * 뒤에 PHY 를 꺼야 한다 — 순서가 반대면 장치들이 아직 살아 있는데
 * 링크가 사라진다.
 * 
 * 브리지와 rc 구조체는 devm 이라 여기서 해제하지 않는다.
 * 
 * 실행 컨텍스트: remove — 프로세스 컨텍스트.
 * 
 * 호출 체인:
 *   드라이버 언바인드 → [이 함수]
 *     → cdns_pcie_host_disable() → cdns_pcie_disable_phy() */
static void sg2042_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] probe 에서 저장해 둔 공통 구조체 포인터. */
	struct cdns_pcie *pcie = platform_get_drvdata(pdev);
	/* [한국어] 그것을 품고 있는 rc 구조체. 아래에서 되짚는다. */
	struct cdns_pcie_rc *rc;

	/* [한국어] pcie 가 rc 안에 박혀 있으므로 container_of 로 바깥을 구한다.
	 * probe 가 rc 가 아니라 pcie 를 저장했기 때문에 필요한 단계다. */
	rc = container_of(pcie, struct cdns_pcie_rc, pcie);
	cdns_pcie_host_disable(rc);

	cdns_pcie_disable_phy(pcie);
}

/* [한국어] sg2042_pcie_suspend_noirq - 절전 진입 시 PHY 를 끈다
 * 
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0.
 * 
 * 공통 코어의 cdns_pcie_suspend_noirq() 와 내용이 같은데 이 드라이버가
 * 자기 것을 따로 두었다. 그 이유는 이 트리에서 확인하지 못했다 —
 * 앞으로 이 SoC 고유의 처리를 넣을 자리로 남겨 둔 것으로 보인다.
 * 
 * 실행 컨텍스트: 시스템 절전 진입의 noirq 단계.
 * 
 * 호출 체인:
 *   PM 코어 → sg2042_pcie_pm_ops → [이 함수] → cdns_pcie_disable_phy() */
static int sg2042_pcie_suspend_noirq(struct device *dev)
{
	/* [한국어] probe 에서 저장해 둔 포인터를 꺼낸다. */
	struct cdns_pcie *pcie = dev_get_drvdata(dev);

	cdns_pcie_disable_phy(pcie);

	return 0;
}

/* [한국어] sg2042_pcie_resume_noirq - 절전 복귀 시 PHY 를 다시 켠다
 * 
 * @dev: 이 컨트롤러의 device.
 * @return: 0 이면 성공. PHY 를 못 켜면 그 오류.
 * 
 * suspend 의 반대다. 인터럽트가 다시 열리기 전에 PHY 가 준비되어야
 * 하므로 noirq 단계에서 처리한다.
 * 
 * suspend 와 달리 오류를 전하는 이유는 PHY 가 켜지지 않으면 링크를
 * 되살릴 수 없어 그 아래 장치가 전부 사라진 것과 같기 때문이다.
 * 
 * 실행 컨텍스트: 시스템 절전 복귀의 noirq 단계.
 * 
 * 호출 체인:
 *   PM 코어 → sg2042_pcie_pm_ops → [이 함수] → cdns_pcie_enable_phy() */
static int sg2042_pcie_resume_noirq(struct device *dev)
{
	/* [한국어] 저장해 둔 컨트롤러 구조체. */
	struct cdns_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] PHY 활성화 결과. */
	int ret;

	/* [한국어] 절전 중에 전원이 끊겼으므로 처음부터 다시 켠다. */
	ret = cdns_pcie_enable_phy(pcie);
	/* [한국어] 실패하면 링크를 되살릴 수 없다. */
	if (ret) {
		/* [한국어] 복귀 실패는 반드시 알려야 한다. */
		dev_err(dev, "failed to enable PHY\n");
		return ret;
	}

	return 0;
}

/* [한국어] 위 두 함수를 noirq 단계 전용 전원 관리 표로 묶는다.
 * 이 매크로는 시스템 절전(suspend/resume)만 채우고 런타임 절전은
 * 비워 두는데, 위 probe 에서 pm_runtime_no_callbacks 를 부른 것과
 * 짝이 맞는다. */
static DEFINE_NOIRQ_DEV_PM_OPS(sg2042_pcie_pm_ops,
			       sg2042_pcie_suspend_noirq,
			       sg2042_pcie_resume_noirq);

/* [한국어] 이 드라이버가 지원하는 디바이스 트리 compatible 목록. */
static const struct of_device_id sg2042_pcie_of_match[] = {
	/* [한국어] SG2042 의 PCIe 호스트 노드. 이 문자열이 디바이스 트리에 있어야
	 * 이 드라이버가 바인딩된다. */
	{ .compatible = "sophgo,sg2042-pcie-host" },
	{},
};
MODULE_DEVICE_TABLE(of, sg2042_pcie_of_match);

/* [한국어] 플랫폼 드라이버 정의. */
static struct platform_driver sg2042_pcie_driver = {
	/* [한국어] 드라이버 공통 속성. */
	.driver = {
		/* [한국어] sysfs 와 로그에 나타날 이름. */
		.name		= "sg2042-pcie",
		.of_match_table	= sg2042_pcie_of_match,
		.pm		= pm_sleep_ptr(&sg2042_pcie_pm_ops),
	},
	.probe		= sg2042_pcie_probe,
	.remove		= sg2042_pcie_remove,
};
module_platform_driver(sg2042_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PCIe controller driver for SG2042 SoCs");
MODULE_AUTHOR("Chen Wang <unicorn_wang@outlook.com>");
