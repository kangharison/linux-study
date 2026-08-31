// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe RC driver for Synopsys DesignWare Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <Joao.Pinto@synopsys.com>
 */

/*
 * [한국어 설명] 주변부가 없는 DesignWare 보드용 드라이버 (pcie-designware-plat.c)
 *
 * === 파일의 역할 ===
 * DesignWare IP 를 쓰는 SoC 는 보통 자기 전용 드라이버가 있다. 클럭이나
 * 전원, PHY 를 다루는 방식이 저마다 다르기 때문이다. 그런데 그런 주변부가
 * 없거나 이미 다른 곳에서 켜지는 경우도 있다 — Synopsys 의 평가 보드,
 * FPGA 프로토타입, 그리고 펌웨어가 미리 다 준비해 두는 시스템.
 *
 * 그런 경우를 위한 최소 드라이버가 이 파일이다. 하는 일이 거의 없다.
 * 디바이스 트리에서 레지스터 위치를 읽어 struct dw_pcie 를 채우고,
 * 공통 코어에 넘기는 것이 전부다.
 *
 * 호스트 모드와 엔드포인트 모드를 둘 다 지원한다는 점이 특징이다.
 * 디바이스 트리의 compatible 로 어느 쪽인지 정하고, 그에 따라
 * dw_pcie_host_init() 또는 dw_pcie_ep_init() 을 부른다. 엔드포인트
 * 모드는 이 SoC 를 다른 컴퓨터에 꽂아 장치처럼 보이게 하는 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 이 compatible 이 있으면
 *   -> [이 파일] dw_plat_pcie_probe()
 *      -> 호스트 모드면 dw_pcie_host_init() [designware-host.c]
 *      -> EP 모드면 dw_pcie_ep_init() [designware-ep.c]
 *
 * 실행 컨텍스트: probe 만 있고 나머지는 공통 코어가 처리한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스(디바이스 트리 기반 바인딩).
 * 아래쪽: pcie-designware.c, pcie-designware-host.c, pcie-designware-ep.c.
 * 공유 상태: struct dw_plat_pcie 가 struct dw_pcie 를 품는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 * 이 드라이버가 만든 호스트 브리지 아래에 NVMe 를 꽂으면 열거되어
 * nvme_probe() 가 불리지만, 그 경로는 PCI 코어를 통한 것이고 이 파일이
 * 관여하지는 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * dw_plat_pcie_probe()    : 디바이스 트리를 읽고 모드에 맞게 초기화한다.
 *                           이 파일에서 실제 일을 하는 거의 유일한 함수다.
 * dw_plat_add_pcie_port()  : 호스트 모드일 때 IRQ 를 얻고 dw_pcie_host_init()
 *                           으로 넘긴다.
 * dw_plat_pcie_ep_raise_irq() : EP 모드에서 호스트에 인터럽트를 올린다.
 *                           INTx/MSI/MSI-X 중 요청된 방식으로 공통 코어에 위임.
 * dw_plat_pcie_get_features() : 이 EP 가 지원하는 기능을 알린다.
 * struct dw_plat_pcie     : dw_pcie 를 감싸는 껍데기.
 * dw_plat_pcie_host_ops   : 호스트 모드 콜백. 이 보드는 특별히 할 일이 없어
 *                           비어 있다시피 하다 — 그것이 이 드라이버의 요점이다.
 * dw_plat_pcie_epc_features : EP 모드에서 노출할 BAR 구성.
 * dw_plat_pcie_rc_of_data / _ep_of_data : compatible 마다 모드를 구분하는 값.
 * dw_plat_pcie_of_match   : 지원하는 compatible 목록.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/types.h>

#include "pcie-designware.h"

struct dw_plat_pcie {
	struct dw_pcie			*pci;
	enum dw_pcie_device_mode	mode;
};
struct dw_plat_pcie_of_data {
	enum dw_pcie_device_mode	mode;
};

static const struct dw_pcie_host_ops dw_plat_pcie_host_ops = {
};

static int dw_plat_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				     unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	switch (type) {
	case PCI_IRQ_INTX:
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	case PCI_IRQ_MSI:
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	case PCI_IRQ_MSIX:
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static const struct pci_epc_features dw_plat_pcie_epc_features = {
	DWC_EPC_COMMON_FEATURES,
	.msi_capable = true,
	.msix_capable = true,
};

static const struct pci_epc_features*
dw_plat_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &dw_plat_pcie_epc_features;
}

static const struct dw_pcie_ep_ops pcie_ep_ops = {
	.raise_irq = dw_plat_pcie_ep_raise_irq,
	.get_features = dw_plat_pcie_get_features,
};

static int dw_plat_add_pcie_port(struct dw_plat_pcie *dw_plat_pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = dw_plat_pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;
	pp->irq = platform_get_irq(pdev, 1);
	if (pp->irq < 0)
		return pp->irq;
	pp->num_vectors = MAX_MSI_IRQS;
	pp->ops = &dw_plat_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		return ret;
	}

	return 0;
}

static int dw_plat_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_plat_pcie *dw_plat_pcie;
	struct dw_pcie *pci;
	int ret;
	const struct dw_plat_pcie_of_data *data;
	enum dw_pcie_device_mode mode;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	mode = (enum dw_pcie_device_mode)data->mode;

	dw_plat_pcie = devm_kzalloc(dev, sizeof(*dw_plat_pcie), GFP_KERNEL);
	if (!dw_plat_pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;

	dw_plat_pcie->pci = pci;
	dw_plat_pcie->mode = mode;

	platform_set_drvdata(pdev, dw_plat_pcie);

	switch (dw_plat_pcie->mode) {
	case DW_PCIE_RC_TYPE:
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_HOST))
			return -ENODEV;

		ret = dw_plat_add_pcie_port(dw_plat_pcie, pdev);
		break;
	case DW_PCIE_EP_TYPE:
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_EP))
			return -ENODEV;

		pci->ep.ops = &pcie_ep_ops;
		ret = dw_pcie_ep_init(&pci->ep);
		if (ret)
			return ret;

		ret = dw_pcie_ep_init_registers(&pci->ep);
		if (ret) {
			dev_err(dev, "Failed to initialize DWC endpoint registers\n");
			dw_pcie_ep_deinit(&pci->ep);
		}

		pci_epc_init_notify(pci->ep.epc);

		break;
	default:
		dev_err(dev, "INVALID device type %d\n", dw_plat_pcie->mode);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct dw_plat_pcie_of_data dw_plat_pcie_rc_of_data = {
	.mode = DW_PCIE_RC_TYPE,
};

static const struct dw_plat_pcie_of_data dw_plat_pcie_ep_of_data = {
	.mode = DW_PCIE_EP_TYPE,
};

static const struct of_device_id dw_plat_pcie_of_match[] = {
	{
		.compatible = "snps,dw-pcie",
		.data = &dw_plat_pcie_rc_of_data,
	},
	{
		.compatible = "snps,dw-pcie-ep",
		.data = &dw_plat_pcie_ep_of_data,
	},
	{},
};

static struct platform_driver dw_plat_pcie_driver = {
	.driver = {
		.name	= "dw-pcie",
		.of_match_table = dw_plat_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = dw_plat_pcie_probe,
};
builtin_platform_driver(dw_plat_pcie_driver);
