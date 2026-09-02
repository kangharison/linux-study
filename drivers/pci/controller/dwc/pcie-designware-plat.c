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
	/* [한국어] DesignWare 공통 컨트롤러 구조체를 가리킨다.
	 * 설정자: dw_plat_pcie_probe() 가 별도로 devm_kzalloc 한 dw_pcie 를 건다.
	 * 읽는 자: 이 파일의 host/ep 초기화 경로 전부.
	 * 값 범위: 유효한 포인터. probe 가 실패하면 이 구조체 자체가 버려진다.
	 * 동기화: probe 이후 불변이라 별도 보호가 없다. */
	struct dw_pcie			*pci;
	/* [한국어] 이 인스턴스가 루트 콤플렉스인지 엔드포인트인지.
	 * 설정자: probe 가 of_device_get_match_data() 로 얻은
	 *   dw_plat_pcie_of_data.mode 를 그대로 옮긴다.
	 * 읽는 자: probe 가 이 값으로 dw_pcie_host_init 과 dw_pcie_ep_init 중
	 *   무엇을 부를지 가른다.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: probe 이후 불변. */
	enum dw_pcie_device_mode	mode;
/* [한국어] 이 구조체가 dw_pcie 를 **포인터로** 들고 있는 점에 유의 -- 다른 글루는
 * 대개 값으로 품는다. 그래서 probe 가 두 번 할당한다. */
};
struct dw_plat_pcie_of_data {
	/* [한국어] 이 compatible 이 RC 인지 EP 인지.
	 * 설정자: 파일 끝의 두 정적 인스턴스가 컴파일 시점에 정한다.
	 * 읽는 자: dw_plat_pcie_probe 의 switch 분기.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: const 정적 데이터라 경쟁이 없다. */
	enum dw_pcie_device_mode	mode;
/* [한국어] 필드가 하나뿐인 구조체를 쓰는 이유는 나중에 SoC 별 항목을 늘리기 쉽게
 * 하려는 것이다. 값 하나만 필요하다면 .data 에 직접 넣어도 됐다. */
};

static const struct dw_pcie_host_ops dw_plat_pcie_host_ops = {
};

/* [한국어]
 * dw_plat_pcie_ep_raise_irq - EP 가 호스트에게 인터럽트를 올린다 (종류별 분배)
 *
 * @ep: DWC 엔드포인트 인스턴스.
 * @func_no: 어느 물리 함수가 보내는지.
 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX.
 * @interrupt_num: MSI/MSI-X 벡터 번호(1-기반). INTx 에서는 쓰이지 않는다.
 * @return: 각 DWC 헬퍼의 반환값. 알 수 없는 종류이면 오류를 찍고 0 을 돌려준다.
 *
 * 이 참조 드라이버에는 인터럽트를 올리는 자체 회로가 없어 세 경로 모두 DWC
 * 코어의 공용 구현에 넘긴다. 그럼에도 EPC 규약이 raise_irq 콜백을 요구하므로
 * 존재해야 한다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): default 분기가 오류를 로그로만
 * 남기고 switch 를 빠져나가 **0(성공)** 을 반환한다. 상위인 pci_epc_raise_irq
 * 는 이 값을 성공으로 받아들이므로, 잘못된 종류를 요청한 EPF 는 인터럽트가
 * 나가지 않았다는 사실을 알 수 없다. (pcie-dw-rockchip.c 의 같은 이름 함수도
 * 동일한 형태다.)
 *
 * 실행 컨텍스트: EPF 드라이버가 인터럽트를 요청하는 프로세스 문맥.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq → epc_ops->raise_irq → [이 함수]
 *     → dw_pcie_ep_raise_intx_irq / _msi_irq / _msix_irq
 */
static int dw_plat_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				     unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	switch (type) {
	/* [한국어] 레거시 INTx. 벡터 번호가 필요 없다. */
	case PCI_IRQ_INTX:
		/* [한국어] DWC 공용 구현에 넘긴다. 이 참조 드라이버에는 자체 회로가 없다. */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	case PCI_IRQ_MSI:
		/* [한국어] MSI. interrupt_num 은 1-기반 벡터 번호다. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	case PCI_IRQ_MSIX:
		/* [한국어] MSI-X. 테이블 항목 번호로 메시지를 만든다. */
		return dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static const struct pci_epc_features dw_plat_pcie_epc_features = {
	/* [한국어] DWC 코어가 공통으로 제공하는 기능들(동적 인바운드 매핑, 부분 범위 매핑). */
	DWC_EPC_COMMON_FEATURES,
	.msi_capable = true,
	.msix_capable = true,
};

static const struct pci_epc_features*
/* [한국어] 인자 ep 를 쓰지 않는다 -- 모든 인스턴스가 같은 기능표를 공유하기 때문이다.
 * SoC 별 차이가 없는 참조 드라이버라 가능한 단순화다. */
dw_plat_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &dw_plat_pcie_epc_features;
/* [한국어] BAR 종류를 하나도 지정하지 않았다는 점에 유의. 그러면 EPC 코어가 전부
 * 기본값(제약 없음)으로 다룬다. */
}

static const struct dw_pcie_ep_ops pcie_ep_ops = {
	/* [한국어] 인터럽트 발생 훅. */
	.raise_irq = dw_plat_pcie_ep_raise_irq,
	/* [한국어] 기능표 조회 훅. init 훅이 없는 것은 되부를 SoC 초기화가 없기 때문이다. */
	.get_features = dw_plat_pcie_get_features,
};

/* [한국어]
 * dw_plat_add_pcie_port - 루트 컴플렉스 모드의 최소 설정 후 DWC 코어에 넘긴다
 *
 * @dw_plat_pcie: 이 드라이버 인스턴스.
 * @pdev: 플랫폼 디바이스. MSI 인터럽트를 여기서 얻는다.
 * @return: 0 성공, 음수는 IRQ 조회 또는 호스트 초기화 실패값.
 *
 * 참조(reference) 드라이버라 SoC 고유 처리가 없다. 하는 일은 셋뿐이다:
 *
 *  1. **platform_get_irq(pdev, 1)** -- 인덱스 **1** 의 인터럽트를 pp->irq 에
 *     담는다. 인덱스 0 이 아닌 점에 유의: DWC 코어의 MSI 초기화가 이름 없는
 *     폴백으로 인덱스 0 을 쓰므로, 이 DT 바인딩은 0 을 MSI 에, 1 을 이
 *     필드에 배정한 배치를 전제한다.
 *  2. num_vectors 를 MAX_MSI_IRQS(256)로 -- 하드웨어가 낼 수 있는 최대치를
 *     요구한다. 실제로 확보되는 수는 dw_pcie_msi_host_init 이 인터럽트 선의
 *     개수를 보고 깎는다.
 *  3. **비어 있는 host_ops 를 건다.** dw_plat_pcie_host_ops 에는 콜백이
 *     하나도 없다 -- 되부를 SoC 초기화가 없다는 뜻이고, 그럼에도 걸어 두는
 *     이유는 DWC 코어가 pp->ops 를 NULL 검사 없이 역참조하기 때문이다.
 *
 * 그 뒤 dw_pcie_host_init 이 브리지 생성부터 버스 열거까지 전부 처리한다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_plat_pcie_probe → [이 함수] → platform_get_irq → dw_pcie_host_init
 */
static int dw_plat_add_pcie_port(struct dw_plat_pcie *dw_plat_pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = dw_plat_pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 오류 로그의 주체. */
	struct device *dev = &pdev->dev;
	/* [한국어] dw_pcie_host_init 의 반환값. */
	int ret;
	/* [한국어] **인덱스 1** 의 인터럽트를 쓴다. 0 이 아닌 이유: DWC 코어의 MSI 초기화가
	 * 이름 없는 폴백으로 인덱스 0 을 쓰므로, 이 바인딩은 0 을 MSI 에, 1 을
	 * 여기에 배정한 배치를 전제한다. */
	pp->irq = platform_get_irq(pdev, 1);
	/* [한국어] IRQ 조회 실패. */
	if (pp->irq < 0)
		/* [한국어] -EPROBE_DEFER 포함해 그대로 올린다. */
		return pp->irq;
	/* [한국어] 하드웨어가 낼 수 있는 최대치(256)를 요구한다. 실제 확보량은
	 * dw_pcie_msi_host_init 이 인터럽트 선 개수를 보고 깎는다. */
	pp->num_vectors = MAX_MSI_IRQS;
	/* [한국어] **콜백이 하나도 없는 빈 ops 를 건다.** 되부를 SoC 초기화가 없다는 뜻이지만,
	 * DWC 코어가 pp->ops 를 NULL 검사 없이 역참조하므로 걸어 두어야 한다. */
	pp->ops = &dw_plat_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	/* [한국어] 호스트 초기화 실패. */
	if (ret) {
		/* [한국어] 어느 단계에서 막혔는지 알 수 있게 남긴다. */
		dev_err(dev, "Failed to initialize host\n");
		/* [한국어] 실패값을 그대로 올린다. */
		return ret;
	}

	return 0;
}

/* [한국어]
 * dw_plat_pcie_probe - DesignWare 참조 플랫폼 드라이버의 진입점
 *
 * @pdev: 매칭된 플랫폼 디바이스.
 * @return: 0 성공, 음수는 실패값.
 *
 * "snps,dw-pcie"(RC)와 "snps,dw-pcie-ep"(EP) 두 compatible 을 지원한다.
 * SoC 고유 코드가 전혀 없는 참조 구현이라, 시뮬레이션이나 FPGA 처럼
 * 클록·리셋·PHY 를 소프트웨어가 다룰 필요가 없는 환경을 대상으로 한다 --
 * 그래서 이 파일에는 clk/reset/phy 를 만지는 코드가 하나도 없다.
 *
 * 구조가 다른 글루와 다른 점: struct dw_pcie 를 인스턴스 안에 값으로 품지
 * 않고 **따로 devm_kzalloc 한 뒤 포인터로 연결**한다. 그래서 할당이 두 번이고,
 * 역방향 변환도 container_of 가 아니라 drvdata 를 쓴다.
 *
 * mode 분기:
 *  - RC: Kconfig 확인 후 dw_plat_add_pcie_port.
 *  - EP: Kconfig 확인 후 ep.ops 를 걸고 dw_pcie_ep_init →
 *    dw_pcie_ep_init_registers → pci_epc_init_notify.
 *  - 그 외: 오류를 찍고 -EINVAL.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): EP 경로에서
 * dw_pcie_ep_init_registers() 가 실패하면 dw_pcie_ep_deinit() 을 부르지만
 * **반환하지 않는다.** 그대로 아래로 흘러가 이미 해제된 ep 의 epc 로
 * pci_epc_init_notify() 를 호출한다. 최종 반환값은 ret(실패값)이라 프로브
 * 자체는 실패로 끝나지만, 그 사이에 해제된 객체를 한 번 건드린다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → of_device_get_match_data
 *     → dw_plat_add_pcie_port (RC) / dw_pcie_ep_init (EP)
 */
static int dw_plat_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_plat_pcie *dw_plat_pcie;
	/* [한국어] 따로 할당할 dw_pcie 를 가리킬 포인터. */
	struct dw_pcie *pci;
	/* [한국어] 각 단계의 반환값. */
	int ret;
	/* [한국어] compatible 에 매인 설정표. */
	const struct dw_plat_pcie_of_data *data;
	/* [한국어] 그 표에서 꺼낸 동작 모드. */
	enum dw_pcie_device_mode mode;

	data = of_device_get_match_data(dev);
	/* [한국어] of_match_table 로 매칭됐다면 data 가 있어야 한다. 방어적 검사다. */
	if (!data)
		/* [한국어] 진행할 근거가 없다는 뜻으로 -EINVAL. */
		return -EINVAL;

	mode = (enum dw_pcie_device_mode)data->mode;
/* [한국어] 이 캐스팅은 형식적이다 -- data->mode 가 이미 같은 열거형이다. */

	dw_plat_pcie = devm_kzalloc(dev, sizeof(*dw_plat_pcie), GFP_KERNEL);
	/* [한국어] 인스턴스 할당 실패. */
	if (!dw_plat_pcie)
		/* [한국어] devm 이라 이후 자동 해제된다. */
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] **dw_pcie 를 별도로 할당한다.** 다른 글루가 인스턴스 안에 값으로 품는 것과
	 * 다른 구조라, 할당이 두 번이다. */
	if (!pci)
		/* [한국어] 두 번째 할당 실패. 첫 번째는 devm 이 처리한다. */
		return -ENOMEM;

	pci->dev = dev;
/* [한국어] DWC 코어가 로그와 DT 접근에 쓸 device. */

	dw_plat_pcie->pci = pci;
	/* [한국어] 모드를 보관한다. 아래 switch 가 이 값으로 갈린다. */
	dw_plat_pcie->mode = mode;
/* [한국어] 인스턴스를 플랫폼 디바이스에 매단다. */

	platform_set_drvdata(pdev, dw_plat_pcie);
/* [한국어] 모드에 따라 완전히 다른 초기화 경로를 탄다. */

	switch (dw_plat_pcie->mode) {
	/* [한국어] 루트 컴플렉스 모드. */
	case DW_PCIE_RC_TYPE:
		/* [한국어] 이 파일 하나가 RC/EP 를 모두 담고 있어, 한쪽 Kconfig 만 켠 커널에서도
		 * 컴파일은 된다. 그래서 실행 시점에 지원 여부를 확인한다. */
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_HOST))
			return -ENODEV;

		ret = dw_plat_add_pcie_port(dw_plat_pcie, pdev);
		/* [한국어] RC 경로 끝. */
		break;
	case DW_PCIE_EP_TYPE:
		/* [한국어] EP 지원이 빌드에 없으면 물러난다. */
		if (!IS_ENABLED(CONFIG_PCIE_DW_PLAT_EP))
			return -ENODEV;

		pci->ep.ops = &pcie_ep_ops;
		/* [한국어] EP 코어 초기화. epc 객체가 여기서 만들어진다. */
		ret = dw_pcie_ep_init(&pci->ep);
		/* [한국어] 초기화 실패. */
		if (ret)
			/* [한국어] 아직 되감을 것이 없으므로 바로 반환한다. */
			return ret;

		ret = dw_pcie_ep_init_registers(&pci->ep);
		/* [한국어] 레지스터 초기화 실패. */
		if (ret) {
			/* [한국어] 어느 단계에서 막혔는지 구별할 수 있게 남긴다. */
			dev_err(dev, "Failed to initialize DWC endpoint registers\n");
			/* [한국어] **코드 관찰 (상류 그대로, 수정하지 않음): 여기서 반환하지 않는다.**
			 * 그대로 아래로 흘러가 이미 해제된 ep 의 epc 로 pci_epc_init_notify() 를
			 * 호출한다. 최종 반환값은 ret(실패값)이라 프로브 자체는 실패로 끝나지만,
			 * 그 사이에 해제된 객체를 한 번 건드린다. */
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
	/* [한국어] RC 용 설정표. */
	.mode = DW_PCIE_RC_TYPE,
/* [한국어] 필드가 mode 하나뿐이다. */
};

static const struct dw_plat_pcie_of_data dw_plat_pcie_ep_of_data = {
	/* [한국어] EP 용 설정표. */
	.mode = DW_PCIE_EP_TYPE,
/* [한국어] 같은 구조에 값만 다르다. */
};

static const struct of_device_id dw_plat_pcie_of_match[] = {
	/* [한국어] compatible 과 설정표의 짝. */
	{
		.compatible = "snps,dw-pcie",
		/* [한국어] "snps,dw-pcie" 는 루트 컴플렉스. */
		.data = &dw_plat_pcie_rc_of_data,
	},
	{
		.compatible = "snps,dw-pcie-ep",
		/* [한국어] "snps,dw-pcie-ep" 는 엔드포인트. */
		.data = &dw_plat_pcie_ep_of_data,
	},
	{},
};

static struct platform_driver dw_plat_pcie_driver = {
	/* [한국어] 플랫폼 드라이버 등록 정보. */
	.driver = {
		/* [한국어] sysfs 에 나타날 이름. RC/EP 두 compatible 이 같은 드라이버를 공유한다. */
		.name	= "dw-pcie",
		/* [한국어] 위 표를 걸어 매칭되면 probe 가 불린다. */
		.of_match_table = dw_plat_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = dw_plat_pcie_probe,
};
builtin_platform_driver(dw_plat_pcie_driver);
