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

/* PCI/NVMe: DesignWare PCIe platform host/endpoint 드라이버.
 *           이 드라이버는 SoC 내장 DesignWare PCIe controller를 초기화하여
 *           Root Complex(RC) 모드에서 하위 PCIe 장치(NVMe SSD 등)를
 *           열거(enumerate)하고 바인딩할 수 있는 PCI host bridge를 만든다.
 *           NVMe host driver(drivers/nvme/host/pci.c)는 이 host bridge가
 *           생성한 PCI bus를 통해 nvme_dev를 발견하고 pci_driver의 probe
 *           (nvme_probe)가 호출된다.
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

/* PCI/NVMe: platform-specific wrapper 구조체.
 *           dw_pcie는 DesignWare core 공통 구조체로, RC 모드에서는 pp(Root Port)
 *           EP 모드에서는 ep(endpoint)가 NVMe와의 PCIe 링크 설정에 사용된다.
 */
struct dw_plat_pcie {
	struct dw_pcie			*pci;	/* PCI/NVMe: DesignWare PCIe core 객체. RC 모드에서 host bridge/PCI bus 생성의 근간. */
	enum dw_pcie_device_mode	mode;	/* PCI/NVMe: RC 또는 EP 모드. NVMe SSD는 RC 아래 endpoint로 연결됨. */
};

/* PCI/NVMe: device-tree match data. compatible 문자열로 RC/EP 모드를 구분. */
struct dw_plat_pcie_of_data {
	enum dw_pcie_device_mode	mode;	/* PCI/NVMe: dt match entry별 동작 모드. */
};

/* PCI/NVMe: RC mode용 host operations. 이 예제에서는 기본 동작(dw_pcie_host_init 낸부)에
 *           의존하므로 별도 콜백은 비어 있다. 실제 NVMe 열거는 dw_pcie_host_init이
 *           등록한 pci_host_bridge를 통해 이뤄진다.
 */
static const struct dw_pcie_host_ops dw_plat_pcie_host_ops = {
};

/* PCI/NVMe: Endpoint(EP) 모드에서 상위 RC(또는 NVMe target simulator)로
 *           인터럽트를 raise할 때 사용하는 콜백. NVMe host 관점보다는
 *           NVMe target/fabrics 구현 시 MSI-X/MSI/INTx injection 지점.
 */
static int dw_plat_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				     unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);	/* PCI/NVMe: ep로부터 dw_pcie core 객체 획득. */

	switch (type) {
	case PCI_IRQ_INTX:	/* PCI/NVMe: 레거시 INTx(#INTA~#INTD). 일부 NVMe fallback 환경에서 사용. */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	case PCI_IRQ_MSI:	/* PCI/NVMe: Message Signaled Interrupts. NVMe queue interrupt 대안. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	case PCI_IRQ_MSIX:	/* PCI/NVMe: MSI-X. NVMe 일반적으로 선호하는 벡터당 queue interrupt 메커니즘. */
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");	/* PCI/NVMe: 잘못된 IRQ type 오류 기록. */
	}

	return 0;
}

/* PCI/NVMe: EP controller capability 구조체. NVMe target 구현 시 MSI/MSIX capability
 *           를 상위 RC에 보고하는 데 사용.
 */
static const struct pci_epc_features dw_plat_pcie_epc_features = {
	DWC_EPC_COMMON_FEATURES,	/* PCI/NVMe: DesignWare EP 공통 기능 플래그/필드 매크로. */
	.msi_capable = true,		/* PCI/NVMe: MSI capability 보유. NVMe MSI mode 가능. */
	.msix_capable = true,		/* PCI/NVMe: MSI-X capability 보유. NVMe MSI-X mode 가능. */
};

/* PCI/NVMe: EP features getter 콜백. */
static const struct pci_epc_features*
dw_plat_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &dw_plat_pcie_epc_features;	/* PCI/NVMe: 고정 capability 구조체 반환. */
}

/* PCI/NVMe: EP mode operations 구조체. */
static const struct dw_pcie_ep_ops pcie_ep_ops = {
	.raise_irq = dw_plat_pcie_ep_raise_irq,	/* PCI/NVMe: MSI-X/MSI/INTx IRQ injection. */
	.get_features = dw_plat_pcie_get_features,	/* PCI/NVMe: EP capability 노출. */
};

/* PCI/NVMe: RC 모드에서 Root Port를 추가하고 host bridge를 초기화.
 *           성공 시 PCI bus가 생성되어 NVMe 장치의 config read/write 및
 *           BAR/MSI-X/DMA 매핑이 가능해진다.
 */
static int dw_plat_add_pcie_port(struct dw_plat_pcie *dw_plat_pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = dw_plat_pcie->pci;	/* PCI/NVMe: dw_pcie core 객체. */
	struct dw_pcie_rp *pp = &pci->pp;		/* PCI/NVMe: Root Port 상태/자원 구조체. */
	struct device *dev = &pdev->dev;		/* PCI/NVMe: Linux device 객체. DMA/IOMMU mapping의 출발점. */
	int ret;

	pp->irq = platform_get_irq(pdev, 1);	/* PCI/NVMe: RC 물리 IRQ 획득(legacy/MSI/MSIX용). NVMe queue interrupt는 별도 MSI-X 벡터로 매핑됨. */
	if (pp->irq < 0)
		return pp->irq;			/* PCI/NVMe: IRQ 획득 실패 시 오류 코드 그대로 반환. */

	pp->num_vectors = MAX_MSI_IRQS;		/* PCI/NVMe: DesignWare core가 지원하는 최대 MSI vector 수. NVMe 다중 queue에 MSI-X/MSI 벡터 할당. */
	pp->ops = &dw_plat_pcie_host_ops;	/* PCI/NVMe: host-specific callbacks 연결. */

	ret = dw_pcie_host_init(pp);		/* PCI/NVMe: 핵심 host bridge 초기화. PCI bus scan, NVMe 장치 열거, BAR 할당, MSI domain 설정 등이 이뤄짐. */
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");	/* PCI/NVMe: host bridge 초기화 실패. NVMe 디바이스 발견 불가. */
		return ret;
	}

	return 0;	/* PCI/NVMe: Root Port 추가/초기화 완료. */
}

/* PCI/NVMe: platform_driver probe. dt compatible에 따라 RC 또는 EP 모드로
 *           DesignWare PCIe controller를 초기화. NVMe SSD는 RC 모드에서
 *           발견/바인딩된다.
 */
static int dw_plat_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;		/* PCI/NVMe: Linux device. DMA mask, IOMMU group, devres에 사용. */
	struct dw_plat_pcie *dw_plat_pcie;		/* PCI/NVMe: platform private state. */
	struct dw_pcie *pci;
	int ret;
	const struct dw_plat_pcie_of_data *data;
	enum dw_pcie_device_mode mode;

	data = of_device_get_match_data(dev);		/* PCI/NVMe: dt compatible에 대응하는 dw_plat_pcie_of_data 획득. */
	if (!data)
		return -EINVAL;				/* PCI/NVMe: match data 없으면 probe 실패. */

	mode = (enum dw_pcie_device_mode)data->mode;	/* PCI/NVMe: RC 또는 EP 모드 결정. */

	dw_plat_pcie = devm_kzalloc(dev, sizeof(*dw_plat_pcie), GFP_KERNEL);
	if (!dw_plat_pcie)
		return -ENOMEM;				/* PCI/NVMe: private 구조체 메모리 할당 실패. */

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;				/* PCI/NVMe: dw_pcie core 구조체 메모리 할당 실패. */

	pci->dev = dev;					/* PCI/NVMe: dw_pcie core에 device 역참조 설정. */

	dw_plat_pcie->pci = pci;			/* PCI/NVMe: platform state와 dw_pcie 연결. */
	dw_plat_pcie->mode = mode;			/* PCI/NVMe: 동작 모드 기록. */

	platform_set_drvdata(pdev, dw_plat_pcie);	/* PCI/NVMe: remove/suspend/resume 시 state 접근용. */

	switch (dw_plat_pcie->mode) {
	case DW_PCIE_RC_TYPE:				/* PCI/NVMe: Root Complex 모드. NVMe host driver 관점에서 핵심 경로. */
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_HOST))
			return -ENODEV;			/* PCI/NVMe: RC 모드 커널 설정이 꺼져 있으면 probe 실패. */

		ret = dw_plat_add_pcie_port(dw_plat_pcie, pdev);	/* PCI/NVMe: host bridge/PCI bus 생성 -> NVMe 장치 열거 준비. */
		break;
	case DW_PCIE_EP_TYPE:				/* PCI/NVMe: Endpoint 모드. NVMe target/fabrics 구현 시 사용. */
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_EP))
			return -ENODEV;			/* PCI/NVMe: EP 모드 설정 꺼져 있으면 실패. */

		pci->ep.ops = &pcie_ep_ops;		/* PCI/NVMe: EP operations 연결. */
		ret = dw_pcie_ep_init(&pci->ep);	/* PCI/NVMe: DesignWare EP 초기화. */
		if (ret)
			return ret;			/* PCI/NVMe: EP 초기화 실패 시 그대로 반환. */

		ret = dw_pcie_ep_init_registers(&pci->ep);	/* PCI/NVMe: EP PCIe capability register 초기화. */
		if (ret) {
			dev_err(dev, "Failed to initialize DWC endpoint registers\n");
			dw_pcie_ep_deinit(&pci->ep);	/* PCI/NVMe: 실패 시 EP 자원 정리. */
		}

		pci_epc_init_notify(pci->ep.epc);	/* PCI/NVMe: EP controller 준비 완료 알림. */

		break;
	default:
		dev_err(dev, "INVALID device type %d\n", dw_plat_pcie->mode);	/* PCI/NVMe: 알 수 없는 모드. */
		ret = -EINVAL;
		break;
	}

	return ret;	/* PCI/NVMe: probe 성공(0) 또는 오류 코드. */
}

/* PCI/NVMe: RC 모드 dt match data. */
static const struct dw_plat_pcie_of_data dw_plat_pcie_rc_of_data = {
	.mode = DW_PCIE_RC_TYPE,	/* PCI/NVMe: "snps,dw-pcie" compatible 매칭 시 RC 모드. */
};

/* PCI/NVMe: EP 모드 dt match data. */
static const struct dw_plat_pcie_of_data dw_plat_pcie_ep_of_data = {
	.mode = DW_PCIE_EP_TYPE,	/* PCI/NVMe: "snps,dw-pcie-ep" compatible 매칭 시 EP 모드. */
};

/* PCI/NVMe: device-tree compatible 매핑 테이블. */
static const struct of_device_id dw_plat_pcie_of_match[] = {
	{
		.compatible = "snps,dw-pcie",	/* PCI/NVMe: RC용 compatible. NVMe SSD 연결 시 사용. */
		.data = &dw_plat_pcie_rc_of_data,
	},
	{
		.compatible = "snps,dw-pcie-ep",	/* PCI/NVMe: EP용 compatible. */
		.data = &dw_plat_pcie_ep_of_data,
	},
	{},
};

/* PCI/NVMe: platform_driver 등록 구조체. */
static struct platform_driver dw_plat_pcie_driver = {
	.driver = {
		.name	= "dw-pcie",			/* PCI/NVMe: driver 이름. */
		.of_match_table = dw_plat_pcie_of_match,	/* PCI/NVMe: dt 자동 매칭. */
		.suppress_bind_attrs = true,			/* PCI/NVMe: sysfs manual bind/unblock 억제. */
	},
	.probe = dw_plat_pcie_probe,				/* PCI/NVMe: dt 노드 발견 시 호출. */
};
builtin_platform_driver(dw_plat_pcie_driver);			/* PCI/NVMe: 부팅 시 platform driver 자동 등록. RC 모드에서는 이후 NVMe PCIe SSD가 열거/바인딩됨. */
