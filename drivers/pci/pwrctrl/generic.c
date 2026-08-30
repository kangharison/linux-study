// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

/*
 * NVMe 관점 요약:
 * 이 드라이버는 PCIe 슬롯에 공급되는 전원/클록/리셋 시퀀스를 제어하는
 * generic PCI power control 드라이버이다. NVMe SSD는 PCIe 엔드포인트로
 * PCIe 슬롯(또는 Root Port 뒤의 링크)에 연결되므로, 이 드라이버가 전원을
 * 켜고 끄는 동작은 NVMe 장치의 생사, 링크 트레이닝, 버스 재스캔,
 * 핫플러그/핫리묶에 직접 영향을 준다.
 *
 * NVMe 호스트 드라이버(drivers/nvme/host/pci.c)가 동작하기 위해서는
 * 먼저 PCIe Root Complex/Root Port/슬롯 쪽 전원과 클록이 안정적으로
 * 공급되어야 하며, 그 후에야 PCI core가 bus scan을 수행하고 NVMe
 * 장치를 발견하여 nvme_probe()가 호출된다. 반대로 전원을 끄면 링크가
 * 다울되고 NVMe 장치는 사라지며, nvme_remove()가 호출될 수 있다.
 *
 * 주요 호출 경로:
 *   platform_driver probe(slot_pwrctrl_probe)
 *     -> pci_pwrctrl_init() + devm_pci_pwrctrl_device_set_ready()
 *        => PCI power control subsystem에 등록
 *     -> PCIe core/portdrv가 필요 시 slot_pwrctrl_power_on/off 호출
 *        => regulator/clk/pwrseq를 통해 슬롯 전원/클록/리셋 제어
 *        => NVMe 장치의 PCIe 링크/ECAM 가시성 변경
 */

#include <linux/clk.h>		/* NVMe: PCIe Root Port/슬롯에 공급되는 bus clock 정의 */
#include <linux/device.h>		/* NVMe: 장치 모델과 dev_info/dev_err 등 사용 */
#include <linux/mod_devicetable.h>	/* NVMe: OF compatible 매칭 테이블 */
#include <linux/module.h>		/* NVMe: 모듈 로드/언로드 및 라이선스 */
#include <linux/of_graph.h>		/* NVMe: DT graph(power sequencer 연결) 존재 여부 확인 */
#include <linux/pci-pwrctrl.h>		/* NVMe: PCI power control core 인터페이스 */
#include <linux/platform_device.h>	/* NVMe: platform bus 드라이버 등록 */
#include <linux/pwrseq/consumer.h>	/* NVMe: 전원 온/오프 시퀀서 제어 */
#include <linux/regulator/consumer.h>	/* NVMe: 슬롯 레귤레이터(전원) 제어 */
#include <linux/slab.h>			/* NVMe: devm_kzalloc() 메모리 할당 */

/*
 * NVMe: PCIe 슬롯 전원 제어를 위한 낮은 수준 구조체.
 *       pci_pwrctrl은 PCI core에 등록되어, NVMe 장치가 탑재된 슬롯의
 *       전원 상태를 상위 계층이 제어할 수 있게 한다.
 */
struct slot_pwrctrl {
	struct pci_pwrctrl pwrctrl;		/* NVMe: PCI power control core 구조체 */
	struct regulator_bulk_data *supplies;	/* NVMe: 슬롯에 공급되는 레귤레이터(전원) 배열 */
	int num_supplies;			/* NVMe: supplies 배열의 개수 */
	struct clk *clk;			/* NVMe: PCIe 슬롯/링크에 공급되는 클록 */
	struct pwrseq_desc *pwrseq;		/* NVMe: 전원 온/오프 시퀀서(리셋/전원 순서 제어) */
};

/*
 * NVMe: 슬롯 전원을 켠다. PCIe 링크 트레이닝과 ECAM을 통해 NVMe
 *       엔드포인트가 Root Complex에 보이기 위해 필요한 첫 단계이다.
 */
static int slot_pwrctrl_power_on(struct pci_pwrctrl *pwrctrl)
{
	/*
	 * NVMe: 상위 pci_pwrctrl 포인터에서 slot_pwrctrl 구조체를 역산한다.
	 *       PCIe 슬롯마다 별도의 slot_pwrctrl 인스턴스가 존재한다.
	 */
	struct slot_pwrctrl *slot = container_of(pwrctrl,
						struct slot_pwrctrl, pwrctrl);
	int ret;				/* NVMe: 함수 호출 결과 저장 */

	/*
	 * NVMe: DT에 power sequencer(graph)가 연결되어 있으면,
	 *       별도의 regulator/clk 대신 시퀀서를 통해 전원을 켠다.
	 *       이는 NVMe 핫플러그/핫스왑 시 전원/리셋 순서를 보장한다.
	 */
	if (slot->pwrseq) {
		/* NVMe: 시퀀서를 동작시켜 슬롯 전원/리셋 순서를 실행 */
		pwrseq_power_on(slot->pwrseq);
		/* NVMe: 전원 시퀀스 완료, PCIe 링크 업 및 NVMe 인식 준비 */
		return 0;
	}

	/*
	 * NVMe: 시퀀서가 없는 경우, 개별 레귤레이터를 모두 켜서
	 *       NVMe 장치가 탑재된 슬롯에 전원을 공급한다.
	 */
	ret = regulator_bulk_enable(slot->num_supplies, slot->supplies);
	/* NVMe: 레귤레이터 활성화 실패 시 NVMe 장치는 전원을 받지 못함 */
	if (ret < 0) {
		/* NVMe: 오류 로그: 슬롯 전원 공급 실패로 NVMe 초기화 불가 */
		dev_err(slot->pwrctrl.dev, "Failed to enable slot regulators\n");
		/* NVMe: 오류 코드를 PCI core로 반환하여 NVMe probe 차단 */
		return ret;
	}

	/*
	 * NVMe: 슬롯 클록을 활성화한다. 클록이 없으면 PCIe PHY/MAC가
	 *       동작하지 않고 NVMe 링크 트레이닝도 시작되지 않는다.
	 */
	return clk_prepare_enable(slot->clk);
}

/*
 * NVMe: 슬롯 전원을 끈다. NVMe 장치의 링크가 다울되고 PCI core는
 *       해당 PCIe 버스에서 NVMe 장치를 제거 처리한다.
 */
static int slot_pwrctrl_power_off(struct pci_pwrctrl *pwrctrl)
{
	/*
	 * NVMe: pci_pwrctrl에서 slot_pwrctrl을 얻어, 슬롯 전원 제어
	 *       자원들에 접근한다.
	 */
	struct slot_pwrctrl *slot = container_of(pwrctrl,
						struct slot_pwrctrl, pwrctrl);

	/*
	 * NVMe: power sequencer가 구성된 경우, 안전한 전원 오프 시퀀스를
	 *       통해 NVMe 슬롯 전원을 내린다. 핫리묶 시 데이터 손상 방지.
	 */
	if (slot->pwrseq) {
		/* NVMe: 시퀀서에 의해 슬롯 전원/리셋 순차적 차단 */
		pwrseq_power_off(slot->pwrseq);
		/* NVMe: 전원 오프 완료, NVMe 장치는 더 이상 버스에 보이지 않음 */
		return 0;
	}

	/*
	 * NVMe: 개별 레귤레이터를 모두 끈다. NVMe 장치의 VCC/VCCQ 등이
	 *       차단되면서 PCIe 링크도 다울된다.
	 */
	regulator_bulk_disable(slot->num_supplies, slot->supplies);
	/*
	 * NVMe: PCIe Root Port/슬롯에 공급되던 클록을 중지한다.
	 *       클록이 멈추면 링크 레이어가 완전히 정지한다.
	 */
	clk_disable_unprepare(slot->clk);

	/* NVMe: 정상적으로 전원을 끈 경우 0 반환 */
	return 0;
}

/*
 * NVMe: 드라이버 분리 시 할당했던 regulator 리소스를 해제한다.
 *       NVMe 장치 제거 후에도 슬롯 자원이 남지 않도록 정리.
 */
static void devm_slot_pwrctrl_release(void *data)
{
	/* NVMe: 해제 대상 slot_pwrctrl 인스턴스 */
	struct slot_pwrctrl *slot = data;

	/* NVMe: devm으로 얻은 레귤레이터 배열의 메타데이터를 해제 */
	regulator_bulk_free(slot->num_supplies, slot->supplies);
}

/*
 * NVMe: platform device가 probe될 때 호출된다. DT에 기술된
 *       PCIe 슬롯(예: NVMe SSD가 연결될 M.2 슬롯)의 전원 제어를
 *       초기화하고 PCI power control subsystem에 등록한다.
 */
static int slot_pwrctrl_probe(struct platform_device *pdev)
{
	/* NVMe: per-slot 전원 제어 구조체 포인터 */
	struct slot_pwrctrl *slot;
	/* NVMe: platform device의 범용 device 구조체 */
	struct device *dev = &pdev->dev;
	int ret;				/* NVMe: 함수 호출 결과 저장 */

	/*
	 * NVMe: 슬롯 제어 구조체를 devm으로 할당. probe 실패/제거 시
	 *       자동 해제되어 NVMe 장치와 함께 깔끔히 정리된다.
	 */
	slot = devm_kzalloc(dev, sizeof(*slot), GFP_KERNEL);
	/* NVMe: 메모리 부족 시 NVMe 슬롯 초기화를 중단 */
	if (!slot)
		return -ENOMEM;

	/*
	 * NVMe: DT graph가 있으면 외부 power sequencer를 통해
	 *       NVMe 슬롯 전원/리셋을 제어한다. 별도의 regulator/clk는
	 *       사용하지 않는다.
	 */
	if (of_graph_is_present(dev_of_node(dev))) {
		/*
		 * NVMe: "pcie" 라벨의 power sequencer handle를 얻는다.
		 *       이 시퀀서는 NVMe 장치의 안정적인 전원 온/오프를 보장.
		 */
		slot->pwrseq = devm_pwrseq_get(dev, "pcie");
		/* NVMe: 시퀀서 획득 실패 시, NVMe 슬롯 제어 등록을 포기 */
		if (IS_ERR(slot->pwrseq))
			return dev_err_probe(dev, PTR_ERR(slot->pwrseq),
				     "Failed to get the power sequencer\n");

		/*
		 * NVMe: 시퀀서만 사용하므로 regulator/clk 획득 단계를 건다.
		 *       곧바로 pci_pwrctrl 구조체 등록 단계로 이동.
		 */
		goto skip_resources;
	}

	/*
	 * NVMe: 시퀀서가 없는 경우, DT의 모든 regulator를 한꺼번에 얻는다.
	 *       이 레귤레이터들이 NVMe 장치에 필요한 모든 전원 레일을 담당.
	 */
	ret = of_regulator_bulk_get_all(dev, dev_of_node(dev),
					&slot->supplies);
	/* NVMe: regulator 획득 실패 시 NVMe 슬롯 전원 제어를 등록하지 않음 */
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get slot regulators\n");

	/*
	 * NVMe: 얻은 레귤레이터의 개수를 저장. power_on()에서 모두 켜야
	 *       NVMe 장치가 정상 동작한다.
	 */
	slot->num_supplies = ret;

	/*
	 * NVMe: PCIe 슬롯/Root Port에 공급되는 선택적 클록을 얻는다.
	 *       일부 플랫폼에서는 클록이 항상 켜져 있어 optional이다.
	 */
	slot->clk = devm_clk_get_optional(dev, NULL);
	/* NVMe: 클록 획득 실패 시 NVMe 링크 활성화에 실패할 수 있음 */
	if (IS_ERR(slot->clk))
		return dev_err_probe(dev, PTR_ERR(slot->clk),
				     "Failed to enable slot clock\n");

/*
 * NVMe: 시퀀서 사용 시 regulator/clk 획득을 생략하고 이 레이블로 직행.
 *       이후 pci_pwrctrl ops 등록 및 PCI core 등록을 수행.
 */
skip_resources:
	/*
	 * NVMe: 상위 pci_pwrctrl에 슬롯 전원 켜기/끄기 콜백을 연결.
	 *       PCIe core가 NVMe 장치 초기화/제거 시 이 함수들을 호출.
	 */
	slot->pwrctrl.power_on = slot_pwrctrl_power_on;
	slot->pwrctrl.power_off = slot_pwrctrl_power_off;

	/*
	 * NVMe: 드라이버 제거 시 regulator 배열을 해제하는 devm action을
	 *       등록한다. NVMe 장치 제거 후에도 메모리/리소스 누수를 막음.
	 */
	ret = devm_add_action_or_reset(dev, devm_slot_pwrctrl_release, slot);
	/* NVMe: action 등록 실패 시 probe를 중단하여 불완전한 등록을 방지 */
	if (ret)
		return ret;

	/*
	 * NVMe: pci_pwrctrl 구조체를 초기화. 이후 PCI power control core가
	 *       이 슬롯을 NVMe 장치 전원 관리 후보로 인식하게 된다.
	 */
	pci_pwrctrl_init(&slot->pwrctrl, dev);

	/*
	 * NVMe: pci_pwrctrl를 PCI core에 등록한다. 등록이 완료되면
	 *       NVMe PCIe 호스트 드라이버 입장에서 이 슬롯의 전원 제어가
	 *       사용 가능해지며, 이후 PCIe bus scan에서 NVMe 장치를 발견할
	 *       수 있는 물리적 전원/클록 기반이 마련된다.
	 */
	ret = devm_pci_pwrctrl_device_set_ready(dev, &slot->pwrctrl);
	/* NVMe: 등록 실패 시 NVMe 장치를 위한 슬롯 전원 제어가 동작하지 않음 */
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register pwrctrl driver\n");

	/* NVMe: probe 성공. NVMe 장치를 위한 슬롯 전원 제어 준비 완료 */
	return 0;
}

/*
 * NVMe: 이 드라이버가 처리하는 DT compatible 목록.
 *       "pciclass,0604"은 PCI-to-PCI bridge(즉, Root Port/슬롯)를 의미
 *       하며, NVMe SSD가 연결되는 PCIe 링크의 전원을 제어할 수 있다.
 *       "pci1912,0014"은 Renesas USB xHCI 장치이나, 동일한 전원 제어
 *       패턴을 공유하는 예시이다.
 */
static const struct of_device_id slot_pwrctrl_of_match[] = {
	{
		.compatible = "pciclass,0604",	/* NVMe: PCI bridge/slot 클래스 */
	},
	/* Renesas UPD720201/UPD720202 USB 3.0 xHCI Host Controller */
	{
		.compatible = "pci1912,0014",	/* NVMe: Renesas 장치용 전원 제어 */
	},
	{ }
};
/* NVMe: of_match_table로 사용되도록 모듈 심볼로 등록 */
MODULE_DEVICE_TABLE(of, slot_pwrctrl_of_match);

/*
 * NVMe: platform_driver 정의. "pci-pwrctrl-slot" 이름으로 등록되며,
 *       PCIe 슬롯 전원 제어를 담당한다. NVMe 장치의 probe/remove는
 *       이 드라이버가 아닌 nvme-pci 드라이버가 처리하지만, 이 드라이버가
 *       제공하는 전원/클록이 NVMe 장치의 가시성을 결정한다.
 */
static struct platform_driver slot_pwrctrl_driver = {
	.driver = {
		.name = "pci-pwrctrl-slot",		/* NVMe: platform bus 등록 이름 */
		.of_match_table = slot_pwrctrl_of_match,	/* NVMe: DT 매칭 테이블 */
	},
	.probe = slot_pwrctrl_probe,			/* NVMe: 슬롯 초기화 및 PCI core 등록 */
};
/*
 * NVMe: 모듈 로드 시 platform_driver를 자동 등록/해제한다.
 *       이로 인해 PCIe 슬롯 전원 제어가 활성화되고, NVMe PCIe 호스트
 *       드라이버가 정상적으로 NVMe 장치를 인식할 수 있게 된다.
 */
module_platform_driver(slot_pwrctrl_driver);

/* NVMe: 모듈 저자 정보 */
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
/*
 * NVMe: 모듈 설명. PCIe 슬롯 전원 제어 드라이버로, NVMe SSD 등의
 *       PCIe 엔드포인트가 탑재된 슬롯에 전원을 공급/차단한다.
 */
MODULE_DESCRIPTION("Generic PCI Power Control driver for PCI Slots");
/* NVMe: GPL 라이선스 */
MODULE_LICENSE("GPL");
