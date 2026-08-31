// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

/*
 * [한국어 설명] DeviceTree 서술만으로 동작하는 범용 전원 제어 드라이버 (pwrctrl/generic.c)
 *
 * === 파일의 역할 ===
 * PCIe 슬롯에 전원을 넣는 가장 단순한 경우를 처리한다. DT 에 레귤레이터
 * 목록이 적혀 있으면 그것을 순서대로 켜고, pwrctrl 코어에 "준비됐다" 고
 * 알리는 것이 전부다.
 *
 * 특별한 순서 제약이나 클럭 조작이 필요한 보드는 자기 전용 드라이버를
 * 쓰지만(예: 같은 디렉터리의 tc9563), 단순히 전원만 넣으면 되는 보드는
 * 이 드라이버 하나로 충분하다. DT 의 compatible 문자열로 매칭된다.
 *
 * devm_ 계열(devres)을 적극적으로 쓴다는 점이 눈에 띈다. 레귤레이터 획득,
 * 활성화, pwrctrl 등록이 모두 devres 로 관리되어, probe 가 실패하거나
 * 드라이버가 떨어질 때 커널이 역순으로 알아서 되돌린다. 그래서 이 파일에는
 * 명시적인 에러 정리 코드가 거의 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pwrctrl/core.c 가 DT 를 보고 만든 platform device
 *   -> 드라이버 코어가 compatible 로 이 드라이버를 바인딩
 *      -> [이 파일] pci_pwrctrl_generic_probe()
 *         -> devm_regulator_bulk_get_enable() 로 레귤레이터를 켜고
 *         -> devm_pci_pwrctrl_device_set_ready() 로 코어에 알린다
 *            -> 코어가 버스 재스캔을 예약 -> 장치 발견
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 드라이버 코어.
 * 아래쪽: pwrctrl/core.c 의 인프라, regulator 서브시스템.
 * 공유 상태: struct pci_pwrctrl 하나.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버와 직접 관련이 없다(전수 확인).
 *
 * 임베디드 보드에 NVMe 를 붙였고 그 슬롯의 전원이 DT 에 단순 레귤레이터로
 * 기술돼 있다면, 이 드라이버가 전원을 넣은 뒤에야 NVMe 가 열거된다.
 * 자세한 흐름은 pwrctrl/core.c 의 헤더 참고.
 *
 * (기존 주석은 이 드라이버가 "전원/클록/리셋 시퀀스" 를 제어한다고 적었으나,
 *  이 파일이 실제로 다루는 것은 레귤레이터뿐이다. 클럭과 리셋을 다루는
 *  것은 같은 디렉터리의 보드 전용 드라이버들이다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_pwrctrl_generic_probe()  : DT 의 레귤레이터 목록을 켜고 코어에 알린다.
 *                                devres 덕분에 정리 코드가 필요 없다.
 * pci_pwrctrl_generic_dt_ids[] : 이 드라이버가 담당할 DT compatible 목록.
 * pci_pwrctrl_generic_driver   : 플랫폼 드라이버 구조체.
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
	 *       통해 NVMe 슬롯 전원을 내린다. 핫리무브 시 데이터 손상 방지.
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
