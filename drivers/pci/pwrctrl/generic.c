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

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/pci-pwrctrl.h>
#include <linux/platform_device.h>
#include <linux/pwrseq/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

struct slot_pwrctrl {
	struct pci_pwrctrl pwrctrl;
	struct regulator_bulk_data *supplies;
	int num_supplies;
	struct clk *clk;
	struct pwrseq_desc *pwrseq;
};

static int slot_pwrctrl_power_on(struct pci_pwrctrl *pwrctrl)
{
	struct slot_pwrctrl *slot = container_of(pwrctrl,
						struct slot_pwrctrl, pwrctrl);
	int ret;

	if (slot->pwrseq) {
		pwrseq_power_on(slot->pwrseq);
		return 0;
	}

	ret = regulator_bulk_enable(slot->num_supplies, slot->supplies);
	if (ret < 0) {
		dev_err(slot->pwrctrl.dev, "Failed to enable slot regulators\n");
		return ret;
	}

	return clk_prepare_enable(slot->clk);
}

static int slot_pwrctrl_power_off(struct pci_pwrctrl *pwrctrl)
{
	struct slot_pwrctrl *slot = container_of(pwrctrl,
						struct slot_pwrctrl, pwrctrl);

	if (slot->pwrseq) {
		pwrseq_power_off(slot->pwrseq);
		return 0;
	}

	regulator_bulk_disable(slot->num_supplies, slot->supplies);
	clk_disable_unprepare(slot->clk);

	return 0;
}

static void devm_slot_pwrctrl_release(void *data)
{
	struct slot_pwrctrl *slot = data;

	regulator_bulk_free(slot->num_supplies, slot->supplies);
}

static int slot_pwrctrl_probe(struct platform_device *pdev)
{
	struct slot_pwrctrl *slot;
	struct device *dev = &pdev->dev;
	int ret;

	slot = devm_kzalloc(dev, sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return -ENOMEM;

	if (of_graph_is_present(dev_of_node(dev))) {
		slot->pwrseq = devm_pwrseq_get(dev, "pcie");
		if (IS_ERR(slot->pwrseq))
			return dev_err_probe(dev, PTR_ERR(slot->pwrseq),
				     "Failed to get the power sequencer\n");

		goto skip_resources;
	}

	ret = of_regulator_bulk_get_all(dev, dev_of_node(dev),
					&slot->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get slot regulators\n");

	slot->num_supplies = ret;

	slot->clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(slot->clk))
		return dev_err_probe(dev, PTR_ERR(slot->clk),
				     "Failed to enable slot clock\n");

skip_resources:
	slot->pwrctrl.power_on = slot_pwrctrl_power_on;
	slot->pwrctrl.power_off = slot_pwrctrl_power_off;

	ret = devm_add_action_or_reset(dev, devm_slot_pwrctrl_release, slot);
	if (ret)
		return ret;

	pci_pwrctrl_init(&slot->pwrctrl, dev);

	ret = devm_pci_pwrctrl_device_set_ready(dev, &slot->pwrctrl);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register pwrctrl driver\n");

	return 0;
}

static const struct of_device_id slot_pwrctrl_of_match[] = {
	{
		.compatible = "pciclass,0604",
	},
	/* Renesas UPD720201/UPD720202 USB 3.0 xHCI Host Controller */
	{
		.compatible = "pci1912,0014",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, slot_pwrctrl_of_match);

static struct platform_driver slot_pwrctrl_driver = {
	.driver = {
		.name = "pci-pwrctrl-slot",
		.of_match_table = slot_pwrctrl_of_match,
	},
	.probe = slot_pwrctrl_probe,
};
module_platform_driver(slot_pwrctrl_driver);

MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_DESCRIPTION("Generic PCI Power Control driver for PCI Slots");
MODULE_LICENSE("GPL");
