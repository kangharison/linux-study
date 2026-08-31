// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Intel Gateway SoCs
 *
 * Copyright (c) 2019 Intel Corporation.
 */

/*
 * [한국어 설명] 인텔 Gateway SoC 의 PCIe (pcie-intel-gw.c)
 *
 * === 파일의 역할 ===
 * 인텔 Gateway 는 가정용 라우터나 게이트웨이 장비에 들어가는 SoC 다
 * (원래 랜틱/인피니언 계열). x86 이 아니라 MIPS 나 ARM 기반이며,
 * DesignWare PCIe IP 를 써서 Wi-Fi 카드나 스토리지를 붙인다.
 *
 * 이 드라이버가 하는 일은 다른 DesignWare 파생 드라이버와 같다 — 주변부
 * 준비. 다만 이 SoC 에 고유한 부분이 몇 가지 있다.
 *
 * 하나는 애플리케이션 레지스터(app 레지스터)다. DesignWare IP 의 표준
 * 레지스터와 별개로, 이 SoC 가 IP 를 감싸며 추가한 제어 레지스터 묶음이
 * 있다. 링크 트레이닝을 시작하거나 인터럽트를 다루는 일부가 그쪽에 있어서
 * 표준 경로만으로는 안 된다.
 *
 * 다른 하나는 링크 속도와 폭을 디바이스 트리에서 제한하는 기능이다.
 * 보드 배선 품질이 좋지 않으면 Gen3 로 올리려다 실패해 링크가 계속
 * 재트레이닝될 수 있어서, 아예 낮은 속도로 고정하는 편이 안정적이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 intel,lgm-pcie 가 있으면
 *   -> [이 파일] intel_pcie_probe()
 *      -> 클럭/리셋/PHY 준비, app 레지스터 설정
 *      -> dw_pcie_host_init() [pcie-designware-host.c]
 *         -> PCI 코어 열거
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: pcie-designware.c / -host.c, PHY·클럭·리셋·GPIO 프레임워크.
 * 공유 상태: struct intel_pcie 가 struct dw_pcie 를 품는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 이런 게이트웨이 SoC 에 NVMe 를 붙이는 구성은 흔치 않지만, NAS 기능을
 * 갖춘 공유기에서는 있을 수 있다. 그때 링크 속도가 디바이스 트리에서
 * 제한되어 있으면 NVMe 대역폭이 그만큼 묶인다 — 드라이브가 Gen3 x4 를
 * 지원해도 보드가 Gen2 x1 로 고정해 두었다면 그것이 상한이 된다.
 *
 * === 주요 함수/구조체 요약 ===
 * intel_pcie_probe()      : 전체 초기화.
 * intel_pcie_host_setup()  : 클럭·리셋·PHY 를 켜고 링크를 올린다.
 * intel_pcie_link_setup()  : app 레지스터로 링크 속도·폭 제한을 적용한다.
 * intel_pcie_rc_init()    : 루트 컴플렉스 초기화 전체 흐름.
 * intel_pcie_ltssm_enable() / intel_pcie_ltssm_disable() : LTSSM(Link
 *                           Training and Status State Machine)을 켜고 끈다.
 *                           app 레지스터를 통해야 하는 대표적인 조작이다.
 * intel_pcie_init_n_fts() : N_FTS(Fast Training Sequence 개수)를 설정한다.
 *                           L0s 에서 깨어날 때 링크를 다시 맞추는 데 쓰는
 *                           시퀀스 수로, 이 값이 부족하면 복귀가 실패한다.
 * intel_pcie_device_rst_assert() / _deassert() : 장치 쪽 PERST# 제어.
 * intel_pcie_core_rst_assert() / _deassert() : 컨트롤러 쪽 리셋.
 * intel_pcie_wait_l2()    : 절전 진입 시 링크가 L2 에 들어가기를 기다린다.
 * intel_pcie_turn_off()   : 정리 경로. 리셋을 걸고 클럭을 끈다.
 * intel_pcie_suspend_noirq() / intel_pcie_resume_noirq() : 절전 전후.
 * intel_pcie_get_resources() : 디바이스 트리에서 클럭·리셋·GPIO 를 얻는다.
 * struct intel_pcie       : dw_pcie 와 이 SoC 의 app 레지스터·GPIO 핸들.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/pci_regs.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset.h>

#include "../../pci.h"
#include "pcie-designware.h"

#define PORT_AFR_N_FTS_GEN12_DFT	(SZ_128 - 1)
#define PORT_AFR_N_FTS_GEN3		180
#define PORT_AFR_N_FTS_GEN4		196
/* PCIe Application logic Registers */
#define PCIE_APP_CCR			0x10
#define PCIE_APP_CCR_LTSSM_ENABLE	BIT(0)
#define PCIE_APP_MSG_CR			0x30
#define PCIE_APP_MSG_XMT_PM_TURNOFF	BIT(0)

#define PCIE_APP_PMC			0x44
#define PCIE_APP_PMC_IN_L2		BIT(20)

#define PCIE_APP_IRNEN			0xF4
#define PCIE_APP_IRNCR			0xF8
#define PCIE_APP_IRN_AER_REPORT		BIT(0)
#define PCIE_APP_IRN_PME		BIT(2)
#define PCIE_APP_IRN_RX_VDM_MSG		BIT(4)
#define PCIE_APP_IRN_PM_TO_ACK		BIT(9)
#define PCIE_APP_IRN_LINK_AUTO_BW_STAT	BIT(11)
#define PCIE_APP_IRN_BW_MGT		BIT(12)
#define PCIE_APP_IRN_INTA		BIT(13)
#define PCIE_APP_IRN_INTB		BIT(14)
#define PCIE_APP_IRN_INTC		BIT(15)
#define PCIE_APP_IRN_INTD		BIT(16)
#define PCIE_APP_IRN_MSG_LTR		BIT(18)
#define PCIE_APP_IRN_SYS_ERR_RC		BIT(29)
#define PCIE_APP_INTX_OFST		12

#define PCIE_APP_IRN_INT \
	(PCIE_APP_IRN_AER_REPORT | PCIE_APP_IRN_PME | \
	PCIE_APP_IRN_RX_VDM_MSG | PCIE_APP_IRN_SYS_ERR_RC | \
	PCIE_APP_IRN_PM_TO_ACK | PCIE_APP_IRN_MSG_LTR | \
	PCIE_APP_IRN_BW_MGT | PCIE_APP_IRN_LINK_AUTO_BW_STAT | \
	PCIE_APP_IRN_INTA | PCIE_APP_IRN_INTB | \
	PCIE_APP_IRN_INTC | PCIE_APP_IRN_INTD)

#define RESET_INTERVAL_MS		100

struct intel_pcie {
	struct dw_pcie		pci;
	void __iomem		*app_base;
	struct gpio_desc	*reset_gpio;
	u32			rst_intrvl;
	struct clk		*core_clk;
	struct reset_control	*core_rst;
	struct phy		*phy;
};
static void pcie_update_bits(void __iomem *base, u32 ofs, u32 mask, u32 val)
{
	u32 old;

	old = readl(base + ofs);
	val = (old & ~mask) | (val & mask);

	if (val != old)
		writel(val, base + ofs);
}

static inline void pcie_app_wr(struct intel_pcie *pcie, u32 ofs, u32 val)
{
	writel(val, pcie->app_base + ofs);
}

static void pcie_app_wr_mask(struct intel_pcie *pcie, u32 ofs,
			     u32 mask, u32 val)
{
	pcie_update_bits(pcie->app_base, ofs, mask, val);
}

static inline u32 pcie_rc_cfg_rd(struct intel_pcie *pcie, u32 ofs)
{
	return dw_pcie_readl_dbi(&pcie->pci, ofs);
}

static inline void pcie_rc_cfg_wr(struct intel_pcie *pcie, u32 ofs, u32 val)
{
	dw_pcie_writel_dbi(&pcie->pci, ofs, val);
}

static void pcie_rc_cfg_wr_mask(struct intel_pcie *pcie, u32 ofs,
				u32 mask, u32 val)
{
	pcie_update_bits(pcie->pci.dbi_base, ofs, mask, val);
}

static void intel_pcie_ltssm_enable(struct intel_pcie *pcie)
{
	pcie_app_wr_mask(pcie, PCIE_APP_CCR, PCIE_APP_CCR_LTSSM_ENABLE,
			 PCIE_APP_CCR_LTSSM_ENABLE);
}

static void intel_pcie_ltssm_disable(struct intel_pcie *pcie)
{
	pcie_app_wr_mask(pcie, PCIE_APP_CCR, PCIE_APP_CCR_LTSSM_ENABLE, 0);
}

static void intel_pcie_link_setup(struct intel_pcie *pcie)
{
	u32 val;
	u8 offset = dw_pcie_find_capability(&pcie->pci, PCI_CAP_ID_EXP);
	val = pcie_rc_cfg_rd(pcie, offset + PCI_EXP_LNKCTL);

	val &= ~(PCI_EXP_LNKCTL_LD | PCI_EXP_LNKCTL_ASPMC);
	pcie_rc_cfg_wr(pcie, offset + PCI_EXP_LNKCTL, val);
}

static void intel_pcie_init_n_fts(struct dw_pcie *pci)
{
	switch (pci->max_link_speed) {
	case 3:
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN3;
		break;
	case 4:
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN4;
		break;
	default:
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN12_DFT;
		break;
	}
	pci->n_fts[0] = PORT_AFR_N_FTS_GEN12_DFT;
}

static int intel_pcie_ep_rst_init(struct intel_pcie *pcie)
{
	struct device *dev = pcie->pci.dev;
	int ret;

	pcie->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pcie->reset_gpio)) {
		ret = PTR_ERR(pcie->reset_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to request PCIe GPIO: %d\n", ret);
		return ret;
	}

	/* Make initial reset last for 100us */
	usleep_range(100, 200);

	return 0;
}

static void intel_pcie_core_rst_assert(struct intel_pcie *pcie)
{
	reset_control_assert(pcie->core_rst);
}

static void intel_pcie_core_rst_deassert(struct intel_pcie *pcie)
{
	/*
	 * One micro-second delay to make sure the reset pulse
	 * wide enough so that core reset is clean.
	 */
	udelay(1);
	reset_control_deassert(pcie->core_rst);

	/*
	 * Some SoC core reset also reset PHY, more delay needed
	 * to make sure the reset process is done.
	 */
	usleep_range(1000, 2000);
}

static void intel_pcie_device_rst_assert(struct intel_pcie *pcie)
{
	gpiod_set_value_cansleep(pcie->reset_gpio, 1);
}

static void intel_pcie_device_rst_deassert(struct intel_pcie *pcie)
{
	msleep(pcie->rst_intrvl);
	gpiod_set_value_cansleep(pcie->reset_gpio, 0);
}

static void intel_pcie_core_irq_disable(struct intel_pcie *pcie)
{
	pcie_app_wr(pcie, PCIE_APP_IRNEN, 0);
	pcie_app_wr(pcie, PCIE_APP_IRNCR, PCIE_APP_IRN_INT);
}

static int intel_pcie_get_resources(struct platform_device *pdev)
{
	struct intel_pcie *pcie = platform_get_drvdata(pdev);
	struct dw_pcie *pci = &pcie->pci;
	struct device *dev = pci->dev;
	int ret;

	pcie->core_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(pcie->core_clk)) {
		ret = PTR_ERR(pcie->core_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get clks: %d\n", ret);
		return ret;
	}

	pcie->core_rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(pcie->core_rst)) {
		ret = PTR_ERR(pcie->core_rst);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get resets: %d\n", ret);
		return ret;
	}

	ret = device_property_read_u32(dev, "reset-assert-ms",
				       &pcie->rst_intrvl);
	if (ret)
		pcie->rst_intrvl = RESET_INTERVAL_MS;

	pcie->app_base = devm_platform_ioremap_resource_byname(pdev, "app");
	if (IS_ERR(pcie->app_base))
		return PTR_ERR(pcie->app_base);

	pcie->phy = devm_phy_get(dev, "pcie");
	if (IS_ERR(pcie->phy)) {
		ret = PTR_ERR(pcie->phy);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Couldn't get pcie-phy: %d\n", ret);
		return ret;
	}

	return 0;
}

static int intel_pcie_wait_l2(struct intel_pcie *pcie)
{
	u32 value;
	int ret;
	struct dw_pcie *pci = &pcie->pci;

	if (pci->max_link_speed < 3)
		return 0;

	/* Send PME_TURN_OFF message */
	pcie_app_wr_mask(pcie, PCIE_APP_MSG_CR, PCIE_APP_MSG_XMT_PM_TURNOFF,
			 PCIE_APP_MSG_XMT_PM_TURNOFF);

	/* Read PMC status and wait for falling into L2 link state */
	ret = readl_poll_timeout(pcie->app_base + PCIE_APP_PMC, value,
				 value & PCIE_APP_PMC_IN_L2, 20,
				 jiffies_to_usecs(5 * HZ));
	if (ret)
		dev_err(pcie->pci.dev, "PCIe link enter L2 timeout!\n");

	return ret;
}

static void intel_pcie_turn_off(struct intel_pcie *pcie)
{
	if (dw_pcie_link_up(&pcie->pci))
		intel_pcie_wait_l2(pcie);

	/* Put endpoint device in reset state */
	intel_pcie_device_rst_assert(pcie);
	pcie_rc_cfg_wr_mask(pcie, PCI_COMMAND, PCI_COMMAND_MEMORY, 0);
}

static int intel_pcie_host_setup(struct intel_pcie *pcie)
{
	int ret;
	struct dw_pcie *pci = &pcie->pci;

	intel_pcie_core_rst_assert(pcie);
	intel_pcie_device_rst_assert(pcie);

	ret = phy_init(pcie->phy);
	if (ret)
		return ret;

	intel_pcie_core_rst_deassert(pcie);

	ret = clk_prepare_enable(pcie->core_clk);
	if (ret) {
		dev_err(pcie->pci.dev, "Core clock enable failed: %d\n", ret);
		goto clk_err;
	}

	pci->atu_base = pci->dbi_base + 0xC0000;

	intel_pcie_ltssm_disable(pcie);
	intel_pcie_link_setup(pcie);
	intel_pcie_init_n_fts(pci);

	ret = dw_pcie_setup_rc(&pci->pp);
	if (ret)
		goto app_init_err;

	dw_pcie_upconfig_setup(pci);

	intel_pcie_device_rst_deassert(pcie);
	intel_pcie_ltssm_enable(pcie);

	ret = dw_pcie_wait_for_link(pci);
	if (ret)
		goto app_init_err;

	/* Enable integrated interrupts */
	pcie_app_wr_mask(pcie, PCIE_APP_IRNEN, PCIE_APP_IRN_INT,
			 PCIE_APP_IRN_INT);

	return 0;

app_init_err:
	clk_disable_unprepare(pcie->core_clk);
clk_err:
	intel_pcie_core_rst_assert(pcie);
	phy_exit(pcie->phy);

	return ret;
}

static void __intel_pcie_remove(struct intel_pcie *pcie)
{
	intel_pcie_core_irq_disable(pcie);
	intel_pcie_turn_off(pcie);
	clk_disable_unprepare(pcie->core_clk);
	intel_pcie_core_rst_assert(pcie);
	phy_exit(pcie->phy);
}

static void intel_pcie_remove(struct platform_device *pdev)
{
	struct intel_pcie *pcie = platform_get_drvdata(pdev);
	struct dw_pcie_rp *pp = &pcie->pci.pp;

	dw_pcie_host_deinit(pp);
	__intel_pcie_remove(pcie);
}

static int intel_pcie_suspend_noirq(struct device *dev)
{
	struct intel_pcie *pcie = dev_get_drvdata(dev);
	int ret;
	intel_pcie_core_irq_disable(pcie);
	ret = intel_pcie_wait_l2(pcie);
	if (ret)
		return ret;

	phy_exit(pcie->phy);
	clk_disable_unprepare(pcie->core_clk);
	return ret;
}

static int intel_pcie_resume_noirq(struct device *dev)
{
	struct intel_pcie *pcie = dev_get_drvdata(dev);
	return intel_pcie_host_setup(pcie);
}

static int intel_pcie_rc_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct intel_pcie *pcie = dev_get_drvdata(pci->dev);
	return intel_pcie_host_setup(pcie);
}

static const struct dw_pcie_ops intel_pcie_ops = {
};

static const struct dw_pcie_host_ops intel_pcie_dw_ops = {
	.init = intel_pcie_rc_init,
};

static int intel_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct intel_pcie *pcie;
	struct dw_pcie_rp *pp;
	struct dw_pcie *pci;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	platform_set_drvdata(pdev, pcie);
	pci = &pcie->pci;
	pci->dev = dev;
	pci->use_parent_dt_ranges = true;
	pp = &pci->pp;

	ret = intel_pcie_get_resources(pdev);
	if (ret)
		return ret;

	ret = intel_pcie_ep_rst_init(pcie);
	if (ret)
		return ret;

	pci->ops = &intel_pcie_ops;
	pp->ops = &intel_pcie_dw_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Cannot initialize host\n");
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops intel_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(intel_pcie_suspend_noirq,
				  intel_pcie_resume_noirq)
};

static const struct of_device_id of_intel_pcie_match[] = {
	{ .compatible = "intel,lgm-pcie" },
	{}
};

static struct platform_driver intel_pcie_driver = {
	.probe = intel_pcie_probe,
	.remove = intel_pcie_remove,
	.driver = {
		.name = "intel-gw-pcie",
		.of_match_table = of_intel_pcie_match,
		.pm = &intel_pcie_pm_ops,
	},
};
builtin_platform_driver(intel_pcie_driver);
