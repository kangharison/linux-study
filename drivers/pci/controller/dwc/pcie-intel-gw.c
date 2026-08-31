// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Intel Gateway SoCs
 *
 * Copyright (c) 2019 Intel Corporation.
 */
/* PCI/NVMe: Intel Gateway SoC의 DesignWare 기반 PCIe Root Complex(RC) 드라이버.
 *           이 RC 아래에 NVMe SSD가 연결되며, NVMe 장치의 PCIe 열거, MMIO/메모리
 *           매핑, MSI-X/MSI/INTx 인터럽트, ASPM/전력 관리, AER 등의 동작이
 *           이 컨트롤러의 설정과 직결된다. */

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

#include <linux/bitfield.h>	/* NVMe: 레지스터 비트 필드 조작용 헤더 */
#include <linux/clk.h>		/* NVMe: PCIe core 클럭 활성화/비활성화 */
#include <linux/gpio/consumer.h>	/* NVMe: 엔드포인트(NVMe SSD) 리셋 GPIO 제어 */
#include <linux/iopoll.h>	/* NVMe: 레지스터 폴링(readl_poll_timeout) */
#include <linux/mod_devicetable.h>	/* NVMe: OF compatible 매칭 테이블 */
#include <linux/pci_regs.h>	/* NVMe: PCI Express 표준 레지스터 오프셋/비트 */
#include <linux/phy/phy.h>	/* NVMe: PCIe PHY 초기화/종료 */
#include <linux/platform_device.h>	/* NVMe: platform bus 프로브/리묵 */
#include <linux/property.h>	/* NVMe: device tree 속성 읽기(reset-assert-ms) */
#include <linux/reset.h>	/* NVMe: PCIe core reset assert/deassert */

#include "../../pci.h"		/* NVMe: PCIe core 관련 구조체/함수 선언 */
#include "pcie-designware.h"	/* NVMe: DesignWare PCIe host 공통 인프라 */

/* NVMe: Training 시 송신할 FTS 개수 설정용 매크로.
 *       링크 품질이 나쁘면 NVMe SSD와의 link up이 늦어지거나 실패한다. */
#define PORT_AFR_N_FTS_GEN12_DFT	(SZ_128 - 1)	/* PCI/NVMe: Gen1/2 기본 FTS 수 */
#define PORT_AFR_N_FTS_GEN3		180		/* PCI/NVMe: Gen3 FTS 수 */
#define PORT_AFR_N_FTS_GEN4		196		/* PCI/NVMe: Gen4 FTS 수 */

/* PCIe Application logic Registers */
/* PCI/NVMe: 위 레지스터들은 DesignWare core 외부의 SoC 특화 제어 레지스터이며,
 *           NVMe SSD의 PCIe 열거/전력/인터럽트 동작과 직결된다. */
#define PCIE_APP_CCR			0x10	/* PCI/NVMe: Common Control Register */
#define PCIE_APP_CCR_LTSSM_ENABLE	BIT(0)	/* PCI/NVMe: LTSSM(Link Training) 활성화 비트 */

#define PCIE_APP_MSG_CR			0x30	/* PCI/NVMe: Message Control Register */
#define PCIE_APP_MSG_XMT_PM_TURNOFF	BIT(0)	/* PCI/NVMe: PME_TURN_OFF 메시지 전송 비트 */

#define PCIE_APP_PMC			0x44	/* PCI/NVMe: Power Management Control/Status */
#define PCIE_APP_PMC_IN_L2		BIT(20)	/* PCI/NVMe: 링크가 L2 상태 진입함을 나타냄 */

#define PCIE_APP_IRNEN			0xF4	/* PCI/NVMe: Interrupt Receive Enable */
#define PCIE_APP_IRNCR			0xF8	/* PCI/NVMe: Interrupt Receive Control/Clear */
#define PCIE_APP_IRN_AER_REPORT		BIT(0)	/* PCI/NVMe: AER(Advanced Error Reporting) 인터럽트 */
#define PCIE_APP_IRN_PME		BIT(2)	/* PCI/NVMe: PME(Power Management Event) 인터럽트 */
#define PCIE_APP_IRN_RX_VDM_MSG		BIT(4)	/* PCI/NVMe: Vendor Defined Message 수신 인터럽트 */
#define PCIE_APP_IRN_PM_TO_ACK		BIT(9)	/* PCI/NVMe: PM_TURN_OFF Ack 수신 인터럽트 */
#define PCIE_APP_IRN_LINK_AUTO_BW_STAT	BIT(11)	/* PCI/NVMe: Link Autonomous Bandwidth Status */
#define PCIE_APP_IRN_BW_MGT		BIT(12)	/* PCI/NVMe: Bandwidth Management 인터럽트 */
#define PCIE_APP_IRN_INTA		BIT(13)	/* PCI/NVMe: legacy INTx#A 인터럽트(NVMe fallback) */
#define PCIE_APP_IRN_INTB		BIT(14)	/* PCI/NVMe: legacy INTx#B 인터럽트 */
#define PCIE_APP_IRN_INTC		BIT(15)	/* PCI/NVMe: legacy INTx#C 인터럽트 */
#define PCIE_APP_IRN_INTD		BIT(16)	/* PCI/NVMe: legacy INTx#D 인터럽트 */
#define PCIE_APP_IRN_MSG_LTR		BIT(18)	/* PCI/NVMe: Latency Tolerance Reporting 메시지 인터럽트 */
#define PCIE_APP_IRN_SYS_ERR_RC		BIT(29)	/* PCI/NVMe: 시스템 에러(RC detect) 인터럽트 */
#define PCIE_APP_INTX_OFST		12	/* PCI/NVMe: INTx 비트 시작 오프셋 */

/* NVMe: RC가 처리할 Application 레벨 인터럽트 마스크.
 *       AER/PME/VDM/LTR/BW/INTx 등 NVMe 운영에 영향을 주는 이벤트들을 포함한다. */
#define PCIE_APP_IRN_INT \
	(PCIE_APP_IRN_AER_REPORT | PCIE_APP_IRN_PME | \
	PCIE_APP_IRN_RX_VDM_MSG | PCIE_APP_IRN_SYS_ERR_RC | \
	PCIE_APP_IRN_PM_TO_ACK | PCIE_APP_IRN_MSG_LTR | \
	PCIE_APP_IRN_BW_MGT | PCIE_APP_IRN_LINK_AUTO_BW_STAT | \
	PCIE_APP_IRN_INTA | PCIE_APP_IRN_INTB | \
	PCIE_APP_IRN_INTC | PCIE_APP_IRN_INTD)

#define RESET_INTERVAL_MS		100	/* PCI/NVMe: 기본 PERST# assert 지속 시간(ms) */

/* PCI/NVMe: Intel Gateway PCIe host의 private 데이터 구조체.
 *            dw_pcie를 상속(임베드)받아 DesignWare 공통 코드와 연동된다.
 *            NVMe 입장에서는 이 구조체가 속한 RC가 BAR/메모리/MSI-X 등을
 *            할당하고 관리하는 주체다. */
struct intel_pcie {
	struct dw_pcie		pci;		/* PCI/NVMe: DesignWare PCIe 공통 상태 */
	void __iomem		*app_base;	/* PCI/NVMe: SoC application 레지스터 매핑 주소 */
	struct gpio_desc	*reset_gpio;	/* PCI/NVMe: PERST# 또는 장치 리셋 GPIO */
	u32			rst_intrvl;	/* PCI/NVMe: 리셋 assert 유지 시간(ms) */
	struct clk		*core_clk;	/* PCI/NVMe: PCIe controller 코어 클럭 */
	struct reset_control	*core_rst;	/* PCI/NVMe: PCIe controller 코어 리셋 */
	struct phy		*phy;		/* PCI/NVMe: PCIe PHY 핸들 */
};

/* PCI/NVMe: SoC application/DBI 레지스터의 read-modify-write 헬퍼.
 *           NVMe 장치의 PCIe capability, MSI-X capability 등을 변경할 때
 *           atomic하게 비트를 갱신하기 위해 사용된다. */
static void pcie_update_bits(void __iomem *base, u32 ofs, u32 mask, u32 val)
{
	u32 old;

	old = readl(base + ofs);	/* NVMe: 현재 레지스터 값 읽기 */
	val = (old & ~mask) | (val & mask);	/* NVMe: mask 영역만 val로 갱신 */

	if (val != old)			/* NVMe: 실제 변경 시에만 쓰기(불필요한 버스 트랜잭션 방지) */
		writel(val, base + ofs);
}

/* PCI/NVMe: Application 레지스터에 32비트 값을 쓴다.
 *           링크 트레이닝, 전력 관리, 인터럽트 마스크 등 NVMe 운영 관련
 *           SoC 특화 제어에 사용된다. */
static inline void pcie_app_wr(struct intel_pcie *pcie, u32 ofs, u32 val)
{
	writel(val, pcie->app_base + ofs);
}

/* PCI/NVMe: Application 레지스터의 특정 비트 영역만 갱신한다. */
static void pcie_app_wr_mask(struct intel_pcie *pcie, u32 ofs,
			     u32 mask, u32 val)
{
	pcie_update_bits(pcie->app_base, ofs, mask, val);
}

/* PCI/NVMe: RC configuration space(DBI, DesignWare Bus Interface)에서 32비트 읽기.
 *           NVMe 장치는 이 RC 아래에서 열거되며, RC의 capability/CSR 접근에 사용. */
static inline u32 pcie_rc_cfg_rd(struct intel_pcie *pcie, u32 ofs)
{
	return dw_pcie_readl_dbi(&pcie->pci, ofs);
}

/* PCI/NVMe: RC configuration space에 32비트 쓰기.
 *           NVMe SSD가 보게 될 PCIe bridge의 설정을 변경한다. */
static inline void pcie_rc_cfg_wr(struct intel_pcie *pcie, u32 ofs, u32 val)
{
	dw_pcie_writel_dbi(&pcie->pci, ofs, val);
}

/* PCI/NVMe: RC configuration space의 특정 비트 영역만 갱신한다. */
static void pcie_rc_cfg_wr_mask(struct intel_pcie *pcie, u32 ofs,
				u32 mask, u32 val)
{
	pcie_update_bits(pcie->pci.dbi_base, ofs, mask, val);
}

/* PCI/NVMe: LTSSM(Link Training and Status State Machine)을 활성화.
 *           이 비트가 설정되어야 PCIe 링크가 트레이닝을 시작하고,
 *           그 결과 NVMe SSD와의 물리/데이터 링크가 설정된다. */
static void intel_pcie_ltssm_enable(struct intel_pcie *pcie)
{
	pcie_app_wr_mask(pcie, PCIE_APP_CCR, PCIE_APP_CCR_LTSSM_ENABLE,
			 PCIE_APP_CCR_LTSSM_ENABLE);
}

/* PCI/NVMe: LTSSM을 비활성화. 재설정이나 링크 재협상 전에 사용된다. */
static void intel_pcie_ltssm_disable(struct intel_pcie *pcie)
{
	pcie_app_wr_mask(pcie, PCIE_APP_CCR, PCIE_APP_CCR_LTSSM_ENABLE, 0);
}

/* PCI/NVMe: PCIe 링크 제어 레지스터(LINKCTRL) 초기 설정.
 *           NVMe SSD와의 link training 직전/도중 ASPM, Link Disable 등을
 *           원하는 상태로 맞춘다. */
static void intel_pcie_link_setup(struct intel_pcie *pcie)
{
	u32 val;
	u8 offset = dw_pcie_find_capability(&pcie->pci, PCI_CAP_ID_EXP);	/* PCI/NVMe: PCIe capability offset */

	val = pcie_rc_cfg_rd(pcie, offset + PCI_EXP_LNKCTL);	/* NVMe: Link Control 레지스터 읽기 */

	val &= ~(PCI_EXP_LNKCTL_LD | PCI_EXP_LNKCTL_ASPMC);	/* PCI/NVMe: Link Disable, ASPM 필드 클리어 */
	pcie_rc_cfg_wr(pcie, offset + PCI_EXP_LNKCTL, val);	/* NVMe: 수정된 Link Control 쓰기 */
}

/* PCI/NVMe: FTS(Number of Fast Training Sequences) 개수 초기화.
 *           Gen3/Gen4에서 적절한 FTS 값이 없으면 link retrain 시
 *           NVMe I/O가 지연되거나 링크 다운이 발생할 수 있다. */
static void intel_pcie_init_n_fts(struct dw_pcie *pci)
{
	switch (pci->max_link_speed) {	/* NVMe: DT/설정에서 지정한 최대 링크 속도 */
	case 3:
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN3;	/* PCI/NVMe: Gen3용 FTS 설정 */
		break;
	case 4:
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN4;	/* PCI/NVMe: Gen4용 FTS 설정 */
		break;
	default:
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN12_DFT;	/* PCI/NVMe: Gen1/2 기본 FTS */
		break;
	}
	pci->n_fts[0] = PORT_AFR_N_FTS_GEN12_DFT;	/* PCI/NVMe: common GEN1/2 FTS */
}

/* PCI/NVMe: 엔드포인트(NVMe SSD) 리셋 GPIO를 요청하고 초기 LOW 상태로 설정.
 *           GPIO 획득 실패 시 -EPROBE_DEFER면 드라이버가 나중에 다시 프로브된다. */
static int intel_pcie_ep_rst_init(struct intel_pcie *pcie)
{
	struct device *dev = pcie->pci.dev;
	int ret;

	pcie->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);	/* NVMe: "reset" GPIO 요청, 초기 LOW */
	if (IS_ERR(pcie->reset_gpio)) {	/* NVMe: GPIO 획득 실패 처리 */
		ret = PTR_ERR(pcie->reset_gpio);
		if (ret != -EPROBE_DEFER)	/* NVMe: defer가 아니면 에러 로그 출력 */
			dev_err(dev, "Failed to request PCIe GPIO: %d\n", ret);
		return ret;
	}

	/* Make initial reset last for 100us */
	usleep_range(100, 200);	/* NVMe: NVMe SSD가 안정적으로 리셋되도록 100~200us 대기 */

	return 0;
}

/* PCI/NVMe: PCIe controller 코어 리셋을 assert(활성) 상태로 만든다.
 *           링크를 내리거나 장치를 제거하기 전 사용된다. */
static void intel_pcie_core_rst_assert(struct intel_pcie *pcie)
{
	reset_control_assert(pcie->core_rst);
}

/* PCI/NVMe: PCIe controller 코어 리셋을 deassert(해제)한다.
 *           충분한 delay를 두어 PHY/코어가 안정화된 후 링크 트레이닝을 시작한다. */
static void intel_pcie_core_rst_deassert(struct intel_pcie *pcie)
{
	/*
	 * One micro-second delay to make sure the reset pulse
	 * wide enough so that core reset is clean.
	 */
	udelay(1);	/* NVMe: reset pulse 폭 보장 */
	reset_control_deassert(pcie->core_rst);

	/*
	 * Some SoC core reset also reset PHY, more delay needed
	 * to make sure the reset process is done.
	 */
	usleep_range(1000, 2000);	/* NVMe: PHY/코어 안정화를 위한 1~2ms 대기 */
}

/* PCI/NVMe: 엔드포인트(NVMe SSD)에 리셋을 assert(PERST# 활성/High 또는 active-low GPIO 기준).
 *           실제 GPIO polarity는 DTS에 따라 gpiod_set_value_cansleep이 처리한다. */
static void intel_pcie_device_rst_assert(struct intel_pcie *pcie)
{
	gpiod_set_value_cansleep(pcie->reset_gpio, 1);
}

/* PCI/NVMe: 엔드포인트 리셋을 deassert한다. rst_intrvl만큼 대기 후 해제하여
 *           NVMe SSD의 납기(PERST# deassert to first config access)를 만족시킨다. */
static void intel_pcie_device_rst_deassert(struct intel_pcie *pcie)
{
	msleep(pcie->rst_intrvl);	/* NVMe: 리셋 해제 전 필요한 대기 시간 */
	gpiod_set_value_cansleep(pcie->reset_gpio, 0);
}

/* PCI/NVMe: Application 레벨 인터럽트를 비활성화하고 pending 상태를 클리어.
 *           suspend/remove 시 NVMe SSD로부터의 추가 인터럽트가 발생하지 않도록 한다. */
static void intel_pcie_core_irq_disable(struct intel_pcie *pcie)
{
	pcie_app_wr(pcie, PCIE_APP_IRNEN, 0);		/* NVMe: 모든 APP 인터럽트 disable */
	pcie_app_wr(pcie, PCIE_APP_IRNCR, PCIE_APP_IRN_INT);	/* NVMe: pending 인터럽트 클리어 */
}

/* PCI/NVMe: platform device에서 DT/ACPI 기반 리소스(클록, 리셋, MMIO, PHY) 획득.
 *           NVMe SSD 동작 전 이 컨트롤러의 물리 리소스를 준비한다. */
static int intel_pcie_get_resources(struct platform_device *pdev)
{
	struct intel_pcie *pcie = platform_get_drvdata(pdev);	/* NVMe: 드라이버 private 데이터 획득 */
	struct dw_pcie *pci = &pcie->pci;			/* NVMe: DesignWare PCIe 구조체 */
	struct device *dev = pci->dev;				/* NVMe: device 구조체 */
	int ret;

	pcie->core_clk = devm_clk_get(dev, NULL);	/* NVMe: PCIe core clock 획득 */
	if (IS_ERR(pcie->core_clk)) {	/* NVMe: clock 획득 실패 */
		ret = PTR_ERR(pcie->core_clk);
		if (ret != -EPROBE_DEFER)	/* NVMe: defer 외 실패 시 로그 */
			dev_err(dev, "Failed to get clks: %d\n", ret);
		return ret;
	}

	pcie->core_rst = devm_reset_control_get(dev, NULL);	/* NVMe: PCIe core reset 획득 */
	if (IS_ERR(pcie->core_rst)) {	/* NVMe: reset 획득 실패 */
		ret = PTR_ERR(pcie->core_rst);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get resets: %d\n", ret);
		return ret;
	}

	ret = device_property_read_u32(dev, "reset-assert-ms",
				       &pcie->rst_intrvl);	/* NVMe: PERST# assert 시간(ms) DT 속성 읽기 */
	if (ret)	/* NVMe: 속성이 없으면 기본값 100ms 사용 */
		pcie->rst_intrvl = RESET_INTERVAL_MS;

	pcie->app_base = devm_platform_ioremap_resource_byname(pdev, "app");	/* NVMe: "app" MMIO 영역 매핑 */
	if (IS_ERR(pcie->app_base))	/* NVMe: 매핑 실패 */
		return PTR_ERR(pcie->app_base);

	pcie->phy = devm_phy_get(dev, "pcie");	/* NVMe: PCIe PHY 획득 */
	if (IS_ERR(pcie->phy)) {	/* NVMe: PHY 획득 실패 */
		ret = PTR_ERR(pcie->phy);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Couldn't get pcie-phy: %d\n", ret);
		return ret;
	}

	return 0;
}

/* PCI/NVMe: PCIe 링크가 L2 상태로 진입할 때까지 대기.
 *           Gen3 이상에서 NVMe SSD를 정상적으로 전원 관리(suspend/turn_off)하기 위해
 *           PME_TURN_OFF 메시지를 본내고 L2 진입을 확인한다. */
static int intel_pcie_wait_l2(struct intel_pcie *pcie)
{
	u32 value;
	int ret;
	struct dw_pcie *pci = &pcie->pci;

	if (pci->max_link_speed < 3)	/* NVMe: Gen3 미만은 L2 대기 생략 */
		return 0;

	/* Send PME_TURN_OFF message */
	pcie_app_wr_mask(pcie, PCIE_APP_MSG_CR, PCIE_APP_MSG_XMT_PM_TURNOFF,
			 PCIE_APP_MSG_XMT_PM_TURNOFF);	/* NVMe: NVMe SSD에 PME_TURN_OFF 전송 */

	/* Read PMC status and wait for falling into L2 link state */
	ret = readl_poll_timeout(pcie->app_base + PCIE_APP_PMC, value,
				 value & PCIE_APP_PMC_IN_L2, 20,
				 jiffies_to_usecs(5 * HZ));	/* NVMe: L2 진입 폴링, 최대 5초 */
	if (ret)
		dev_err(pcie->pci.dev, "PCIe link enter L2 timeout!\n");	/* NVMe: L2 진입 실패 로그 */

	return ret;
}

/* PCI/NVMe: PCIe 링크/장치를 끈다. remove/suspend 전에 호출.
 *           NVMe SSD가 메모리/IO 응답을 중단하고 안전하게 리셋 상태로 들어간다. */
static void intel_pcie_turn_off(struct intel_pcie *pcie)
{
	if (dw_pcie_link_up(&pcie->pci))	/* NVMe: 링크가 살아 있을 때만 L2 진입 시도 */
		intel_pcie_wait_l2(pcie);

	/* Put endpoint device in reset state */
	intel_pcie_device_rst_assert(pcie);	/* NVMe: NVMe SSD 리셋 assert */
	pcie_rc_cfg_wr_mask(pcie, PCI_COMMAND, PCI_COMMAND_MEMORY, 0);	/* NVMe: RC 메모리 응답 disable */
}

/* PCI/NVMe: PCIe RC 초기화 및 NVMe 장치와의 link up 수행.
 *           PHY, clock, reset, ATU, LTSSM, interrupts 순으로 설정한다. */
static int intel_pcie_host_setup(struct intel_pcie *pcie)
{
	int ret;
	struct dw_pcie *pci = &pcie->pci;

	intel_pcie_core_rst_assert(pcie);	/* NVMe: PCIe controller 리셋 assert */
	intel_pcie_device_rst_assert(pcie);	/* NVMe: NVMe SSD 리셋 assert(초기 안정화) */

	ret = phy_init(pcie->phy);	/* NVMe: PCIe PHY 초기화 */
	if (ret)
		return ret;

	intel_pcie_core_rst_deassert(pcie);	/* NVMe: PCIe controller 리셋 해제 */

	ret = clk_prepare_enable(pcie->core_clk);	/* NVMe: PCIe core clock 활성화 */
	if (ret) {
		dev_err(pcie->pci.dev, "Core clock enable failed: %d\n", ret);
		goto clk_err;
	}

	pci->atu_base = pci->dbi_base + 0xC0000;	/* NVMe: DesignWare ATU 레지스터 베이스 설정;
							     NVMe BAR/메모리 매핑에 사용 */

	intel_pcie_ltssm_disable(pcie);	/* NVMe: 링크 트레이닝 비활성화(설정 중) */
	intel_pcie_link_setup(pcie);	/* NVMe: Link Control 설정 */
	intel_pcie_init_n_fts(pci);	/* NVMe: FTS 설정 */

	ret = dw_pcie_setup_rc(&pci->pp);	/* NVMe: DesignWare RC 공통 설정(Bar, ATU, capability 등);
							     이 설정이 NVMe SSD가 보는 config space를 만든다 */
	if (ret)
		goto app_init_err;

	dw_pcie_upconfig_setup(pci);	/* NVMe: upconfigure(레인 수/속도 자동 복원) 설정 */

	intel_pcie_device_rst_deassert(pcie);	/* NVMe: NVMe SSD 리셋 해제 → 장치가 응답 시작 */
	intel_pcie_ltssm_enable(pcie);	/* NVMe: PCIe link training 시작 */

	ret = dw_pcie_wait_for_link(pci);	/* NVMe: link up 대기(NVMe SSD와의 physical link 확립) */
	if (ret)
		goto app_init_err;

	/* Enable integrated interrupts */
	pcie_app_wr_mask(pcie, PCIE_APP_IRNEN, PCIE_APP_IRN_INT,
			 PCIE_APP_IRN_INT);	/* NVMe: AER/PME/INTx 등 APP 인터럽트 활성화;
							     NVMe SSD의 MSI-X 실패 fallback INTx,
							     AER 등을 이 RC가 수신 */

	return 0;

app_init_err:
	clk_disable_unprepare(pcie->core_clk);	/* NVMe: clock 비활성화(역진 에러 처리) */
clk_err:
	intel_pcie_core_rst_assert(pcie);	/* NVMe: controller 리셋 assert(역진) */
	phy_exit(pcie->phy);	/* NVMe: PHY 종료(역진) */

	return ret;
}

/* PCI/NVMe: 드라이버 언바인드 시 호출되는 낮은 수준 제거 함수.
 *           IRQ, 링크, clock, reset, PHY를 역순으로 정리한다. */
static void __intel_pcie_remove(struct intel_pcie *pcie)
{
	intel_pcie_core_irq_disable(pcie);	/* NVMe: APP 인터럽트 disable/clear */
	intel_pcie_turn_off(pcie);		/* NVMe: NVMe SSD 리셋 및 RC 메모리 응답 disable */
	clk_disable_unprepare(pcie->core_clk);	/* NVMe: core clock 비활성화 */
	intel_pcie_core_rst_assert(pcie);	/* NVMe: controller 리셋 assert */
	phy_exit(pcie->phy);			/* NVMe: PHY 종료 */
}

/* PCI/NVMe: platform remove 콜백. DesignWare host deinit 후 SoC 제거 수행. */
static void intel_pcie_remove(struct platform_device *pdev)
{
	struct intel_pcie *pcie = platform_get_drvdata(pdev);	/* NVMe: 드라이버 private 획득 */
	struct dw_pcie_rp *pp = &pcie->pci.pp;			/* NVMe: DesignWare RP 포트 */

	dw_pcie_host_deinit(pp);	/* NVMe: DesignWare host 공통 자원 해제;
							     NVMe MSI/MSI-X, config space, bus 등 정리 */
	__intel_pcie_remove(pcie);	/* NVMe: SoC 특화 자원 정리 */
}

/* PCI/NVMe: 시스템 suspend(noirq 단계) 콜백.
 *           NVMe SSD가 runtime/suspend 상태라도 system suspend 전에
 *           링크를 L2로 복(민)어 전력을 절감한다. */
static int intel_pcie_suspend_noirq(struct device *dev)
{
	struct intel_pcie *pcie = dev_get_drvdata(dev);	/* NVMe: 드라이버 private 획득 */
	int ret;

	intel_pcie_core_irq_disable(pcie);	/* NVMe: APP 인터럽트 disable */
	ret = intel_pcie_wait_l2(pcie);		/* NVMe: L2 링크 상태 진입 */
	if (ret)
		return ret;

	phy_exit(pcie->phy);			/* NVMe: PHY 종료 */
	clk_disable_unprepare(pcie->core_clk);	/* NVMe: core clock 비활성화 */
	return ret;
}

/* PCI/NVMe: 시스템 resume(noirq 단계) 콜백. host_setup을 재수행해
 *           NVMe SSD와의 링크 및 인터럽트를 복원한다. */
static int intel_pcie_resume_noirq(struct device *dev)
{
	struct intel_pcie *pcie = dev_get_drvdata(dev);	/* NVMe: 드라이버 private 획득 */

	return intel_pcie_host_setup(pcie);	/* NVMe: RC 재초기화 및 link up */
}

/* PCI/NVMe: DesignWare host_ops.init 콜백. dw_pcie_host_init() 과정에서 호출되어
 *           NVMe SSD가 연결될 PCIe bus를 준비한다. */
static int intel_pcie_rc_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: RP에서 dw_pcie 획득 */
	struct intel_pcie *pcie = dev_get_drvdata(pci->dev);	/* NVMe: Intel private 획득 */

	return intel_pcie_host_setup(pcie);	/* NVMe: RC 설정 및 link up 수행 */
}

/* PCI/NVMe: DesignWare core ops. 현재 Intel GW SoC에서는 별도 콜백이 필요 없음.
 *           추후 read_dbi/write_dbi 등 NVMe config/BAR 접근 최적화를 추가할 수 있다. */
static const struct dw_pcie_ops intel_pcie_ops = {
};

/* PCI/NVMe: DesignWare host 레이어에 등록할 콜백 테이블.
 *           .init이 호출되면 NVMe SSD의 PCIe 열거를 위한 RC 초기화가 완료된다. */
static const struct dw_pcie_host_ops intel_pcie_dw_ops = {
	.init = intel_pcie_rc_init,	/* NVMe: RC 초기화 콜백 */
};

/* PCI/NVMe: platform driver probe 콜백. DT에서 "intel,lgm-pcie"에 매칭되면
 *           이 함수가 호출되어 NVMe SSD를 위한 PCIe host를 생성한다. */
static int intel_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;	/* NVMe: platform device */
	struct intel_pcie *pcie;		/* NVMe: Intel private 구조체 포인터 */
	struct dw_pcie_rp *pp;			/* NVMe: DesignWare RP 포인터 */
	struct dw_pcie *pci;			/* NVMe: DesignWare PCIe 포인터 */
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);	/* NVMe: private 구조체 할당 */
	if (!pcie)	/* NVMe: 메모리 부족 */
		return -ENOMEM;

	platform_set_drvdata(pdev, pcie);	/* NVMe: pdev에 private 저장(suspend/remove 사용) */
	pci = &pcie->pci;			/* NVMe: embedded dw_pcie 참조 */
	pci->dev = dev;				/* NVMe: device 연결 */
	pci->use_parent_dt_ranges = true;	/* NVMe: DT ranges를 상위 bus에서 상속 */
	pp = &pci->pp;				/* NVMe: RP 포인터 초기화 */

	ret = intel_pcie_get_resources(pdev);	/* NVMe: clock/reset/MMIO/PHY 획득 */
	if (ret)
		return ret;

	ret = intel_pcie_ep_rst_init(pcie);	/* NVMe: NVMe SSD 리셋 GPIO 획득 */
	if (ret)
		return ret;

	pci->ops = &intel_pcie_ops;	/* NVMe: DesignWare core ops 연결 */
	pp->ops = &intel_pcie_dw_ops;	/* NVMe: DesignWare host ops 연결 */

	ret = dw_pcie_host_init(pp);	/* NVMe: DesignWare host 초기화 및 NVMe PCIe bus 열거;
							     이 시점부터 PCI core가 NVMe SSD를 발견/바인딩 */
	if (ret) {
		dev_err(dev, "Cannot initialize host\n");
		return ret;
	}

	return 0;
}

/* PCI/NVMe: 전원 관리 ops. 시스템 suspend/resume 시 NVMe SSD 링크 상태를
 *           안전하게 보존/복원하기 위해 noirq 단계에서 동작한다. */
static const struct dev_pm_ops intel_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(intel_pcie_suspend_noirq,
				  intel_pcie_resume_noirq)
};

/* PCI/NVMe: DT compatible 매칭 테이블. "intel,lgm-pcie" 노드에 이 드라이버 바인딩. */
static const struct of_device_id of_intel_pcie_match[] = {
	{ .compatible = "intel,lgm-pcie" },	/* NVMe: Lightning Mountain(LGM) SoC PCIe */
	{}
};

/* PCI/NVMe: platform_driver 등록. 프로브/리묵/PM 콜백과 OF 매칭 테이블을 포함한다. */
static struct platform_driver intel_pcie_driver = {
	.probe = intel_pcie_probe,	/* NVMe: NVMe PCIe host 생성 */
	.remove = intel_pcie_remove,	/* NVMe: NVMe PCIe host 제거 */
	.driver = {
		.name = "intel-gw-pcie",		/* NVMe: 드라이버 이름 */
		.of_match_table = of_intel_pcie_match,	/* NVMe: DT compatible 테이블 */
		.pm = &intel_pcie_pm_ops,		/* NVMe: suspend/resume ops */
	},
};
builtin_platform_driver(intel_pcie_driver);	/* NVMe: 부팅 시 platform driver 자동 등록 */
