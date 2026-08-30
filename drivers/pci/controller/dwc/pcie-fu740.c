// SPDX-License-Identifier: GPL-2.0
/*
 * FU740 DesignWare PCIe Controller integration
 * Copyright (C) 2019-2021 SiFive, Inc.
 * Paul Walmsley
 * Greentime Hu
 *
 * Based in part on the i.MX6 PCIe host controller shim which is:
 *
 * Copyright (C) 2013 Kosagi
 *		https://www.kosagi.com
 */

#include <linux/clk.h>			/* PCI/NVMe: SoC 클럭 게이팅/언게이팅을 위한 헤더; PCIe 링크 및 NVMe 열거 전 클럭 활성화에 사용 */
#include <linux/delay.h>		/* NVMe: PCIe 리셋 홀드 타임, PHY 안정화, NVMe 디바이스 준비 대기에 필요한 ms/us/ndelay */
#include <linux/gpio.h>			/* PCI/NVMe: PERST# 및 power-enable GPIO 제어를 위한 GPIO 프레임워크 헤더 */
#include <linux/gpio/consumer.h>	/* NVMe: devm_gpiod_get_optional() 등 GPIO 디스크립터 기반 API; NVMe 카드 리셋/전원 제어에 사용 */
#include <linux/kernel.h>		/* NVMe: 커널 기본 타입/매크로; dev_dbg/dev_err 등 NVMe 호스트 진단 메시지에 사용 */
#include <linux/module.h>		/* NVMe: 모듈/빌트인 드라이버 매크로; NVMe용 PCIe 호스트 드라이버 등록에 사용 */
#include <linux/pci.h>			/* PCI/NVMe: PCI 버스, 캐패빌리티, MSI/MSI-X, ASPM, AER 등 NVMe 디바이스 바인딩에 핵심 */
#include <linux/platform_device.h>	/* NVMe: SoC의 platform_device로부터 리소스/IRQ/DMA를 얻어 NVMe 호스트에 연결 */
#include <linux/resource.h>		/* NVMe: IORESOURCE_MEM 등 메모리 영역 정의; NVMe BAR 매핑의 물리적 기반 */
#include <linux/types.h>		/* NVMe: u8/u16/u32 등 정수 타입; PCIe 구성/레지스터 접근에 사용 */
#include <linux/interrupt.h>		/* PCI/NVMe: MSI/MSI-X/레거시 인터럽트 라우팅; NVMe 큐 완료 인터럽트 처리의 근간 */
#include <linux/iopoll.h>		/* NVMe: readl_poll_timeout()로 PCIe PHY/link 상태 폴링; NVMe 디바이스 준비 확인 */
#include <linux/reset.h>		/* NVMe: SoC 리셋 컨트롤러; PCIe 컨트롤러 자체 리셋으로 NVMe 열수 초기 상태 보장 */

#include "pcie-designware.h"		/* PCI/NVMe: DesignWare PCIe 코어 헬퍼/구조체; dw_pcie_host_init(), dw_pcie_wait_for_link() 등 NVMe 호스트 초기화 지원 */

#define to_fu740_pcie(x)	dev_get_drvdata((x)->dev)	/* NVMe: dw_pcie 포인터로부터 SiFive FU740 전용 구조체 fu740_pcie를 얻는 매크로; NVMe 호스트별 SoC 상태 접근용 */

struct fu740_pcie {				/* NVMe: FU740 SoC PCIe 컨트롤러의 전체 상태를 담는 구조체; NVMe 디바이스와의 PCIe 링크/리셋/전원 관리 */
	struct dw_pcie pci;			/* PCI/NVMe: DesignWare PCIe 코어 구조체; NVMe 열거/구성/MSI/DMA를 위한 RC(root complex) 상태 */
	void __iomem *mgmt_base;		/* NVMe: SiFive 관리 레지스터 공간 매핑 주소; PERST#, LTSSM, DEVICE_TYPE, PHY CR 등 NVMe 링크 제어에 사용 */
	struct gpio_desc *reset;		/* NVMe: PERST# 출력 GPIO 디스크립터; NVMe 디바이스의fundamental 리셋을 생성 */
	struct gpio_desc *pwren;		/* NVMe: PCIe 슬롯/디바이스 전원 활성화 GPIO; NVMe 카드 power-up 시퀀스 제어 */
	struct clk *pcie_aux;			/* NVMe: PCIe 보조 클럭; 링크 레이어 및 NVMe 트랜잭션 타이밍에 필요한 클럭 */
	struct reset_control *rst;		/* NVMe: SoC 내 PCIe 컨트롤러 리셋 라인; NVMe 호스트 하드웨어의 클린 초기화에 사용 */
};

#define SIFIVE_DEVICESRESETREG		0x28		/* NVMe: SiFive 장치 리셋 레지스터 오프셋; 현재 코드에서는 직접 사용되지 않으나 SoC 리셋 도메인과 관련 */

#define PCIEX8MGMT_PERST_N		0x0		/* PCI/NVMe: PCIe PERST# 제어 레지스터 오프셋; 0으로 설정하면 NVMe 디바이스에 fundamental 리셋(assert) */
#define PCIEX8MGMT_APP_LTSSM_ENABLE	0x10		/* PCI/NVMe: LTSSM(Link Training and Status State Machine) 활성화 레지스터; 1 설정 시 PCIe 링크 트레이닝 시작 → NVMe 열거 가능 */
#define PCIEX8MGMT_APP_HOLD_PHY_RST	0x18		/* NVMe: PHY 리셋 홀드 레지스터; PHY CR 파라미터 프로그래밍 동안 LTSSM을 리셋 상태로 고정 */
#define PCIEX8MGMT_DEVICE_TYPE		0x708		/* PCI/NVMe: PCIe 디바이스 타입 설정 레지스터; 0x4는 RC(Root Complex) 모드 → NVMe 호스트 역할 */
#define PCIEX8MGMT_PHY0_CR_PARA_ADDR	0x860		/* NVMe: PHY0 설정 레지스터 어드레스 포트; PHY 트레이닝 파라미터를 NVMe 링크 품질에 맞게 설정 */
#define PCIEX8MGMT_PHY0_CR_PARA_RD_EN	0x870		/* NVMe: PHY0 CR 파라미터 읽기 enable 포트; PHY 상태 확인 */
#define PCIEX8MGMT_PHY0_CR_PARA_RD_DATA	0x878		/* NVMe: PHY0 CR 파라미터 읽기 데이터 포트; PHY 트레이닝 상태/캘리브레이션 값 확인 */
#define PCIEX8MGMT_PHY0_CR_PARA_SEL	0x880		/* NVMe: PHY0 CR 파라미터 인터페이스 선택; 파라미터 편집 가능하도록 활성화 */
#define PCIEX8MGMT_PHY0_CR_PARA_WR_DATA	0x888		/* NVMe: PHY0 CR 파라미터 쓰기 데이터 포트; PHY 트레이닝 값 기록 */
#define PCIEX8MGMT_PHY0_CR_PARA_WR_EN	0x890		/* NVMe: PHY0 CR 파라미터 쓰기 enable 포트; 쓰기 트랜잭션 트리거 */
#define PCIEX8MGMT_PHY0_CR_PARA_ACK	0x898		/* NVMe: PHY0 CR 작업 완료(ack) 포트; 폴링으로 쓰기 완료 대기 */
#define PCIEX8MGMT_PHY1_CR_PARA_ADDR	0x8a0		/* NVMe: PHY1 설정 레지스터 어드레스 포트; x8 링크의 후반 4레인 PHY */
#define PCIEX8MGMT_PHY1_CR_PARA_RD_EN	0x8b0		/* NVMe: PHY1 CR 파라미터 읽기 enable 포트 */
#define PCIEX8MGMT_PHY1_CR_PARA_RD_DATA	0x8b8		/* NVMe: PHY1 CR 파라미터 읽기 데이터 포트 */
#define PCIEX8MGMT_PHY1_CR_PARA_SEL	0x8c0		/* NVMe: PHY1 CR 파라미터 인터페이스 선택 */
#define PCIEX8MGMT_PHY1_CR_PARA_WR_DATA	0x8c8		/* NVMe: PHY1 CR 파라미터 쓰기 데이터 포트 */
#define PCIEX8MGMT_PHY1_CR_PARA_WR_EN	0x8d0		/* NVMe: PHY1 CR 파라미터 쓰기 enable 포트 */
#define PCIEX8MGMT_PHY1_CR_PARA_ACK	0x8d8		/* NVMe: PHY1 CR 작업 완료(ack) 포트 */

#define PCIEX8MGMT_PHY_CDR_TRACK_EN	BIT(0)		/* NVMe: CDR(Clock Data Recovery) 추적 enable 비트; NVMe 디바이스에서 수신된 신호 클록 복원에 필수 */
#define PCIEX8MGMT_PHY_LOS_THRSHLD	BIT(5)		/* NVMe: Loss-of-Signal 임계값 설정 비트; NVMe 링크 감도/신호 감지 기준 조정 */
#define PCIEX8MGMT_PHY_TERM_EN		BIT(9)		/* NVMe: PHY termination enable 비트; PCIe 신호 무결성을 위해 NVMe 링크 양단 종단 저항 활성화 */
#define PCIEX8MGMT_PHY_TERM_ACDC	BIT(10)		/* NVMe: termination AC/DC 모드 선택 비트; NVMe 카드와의 전기 특성 맞춤 */
#define PCIEX8MGMT_PHY_EN		BIT(11)		/* NVMe: PHY enable 비트; PHY 블록 전원/기능 활성화로 NVMe 트랜잭션 송수신 가능 */
/* NVMe: PHY 레인 초기값; CDR/LOS/Termination/PHY enable을 한 번에 설정 */
#define PCIEX8MGMT_PHY_INIT_VAL		(PCIEX8MGMT_PHY_CDR_TRACK_EN | \
					 PCIEX8MGMT_PHY_LOS_THRSHLD | \
					 PCIEX8MGMT_PHY_TERM_EN | \
					 PCIEX8MGMT_PHY_TERM_ACDC | \
					 PCIEX8MGMT_PHY_EN)

#define PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3	0x1008	/* NVMe: PHY 레인 RX override 레지스터 기준 오프셋; 수신부 캘리브레이션 값 기록 */
#define PCIEX8MGMT_PHY_LANE_OFF		0x100		/* NVMe: 인접 PHY 레인 간 레지스터 오프셋; 4개 레인에 걸쳐 동일 설정 반복 */
#define PCIEX8MGMT_PHY_LANE0_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 0)	/* NVMe: PHY lane0 RX override 레지스터 주소 */
#define PCIEX8MGMT_PHY_LANE1_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 1)	/* NVMe: PHY lane1 RX override 레지스터 주소 */
#define PCIEX8MGMT_PHY_LANE2_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 2)	/* NVMe: PHY lane2 RX override 레지스터 주소 */
#define PCIEX8MGMT_PHY_LANE3_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 3)	/* NVMe: PHY lane3 RX override 레지스터 주소 */

static void fu740_pcie_assert_reset(struct fu740_pcie *afp)	/* NVMe: NVMe 디바이스에 fundamental reset(PERST#)을 assert하는 함수; 장치 상태 머신 초기화 */
{
	/* Assert PERST_N GPIO */
	gpiod_set_value_cansleep(afp->reset, 0);			/* NVMe: GPIO로 PERST#를 active-low로 assert; NVMe 컨트롤러가 리셋 상태 진입 */
	/* Assert controller PERST_N */
	writel_relaxed(0x0, afp->mgmt_base + PCIEX8MGMT_PERST_N);	/* NVMe: 컨트롤러 내 PERST# 레지스터도 assert; SoC 낸드도 NVMe 리셋을 제어 */
}

static void fu740_pcie_deassert_reset(struct fu740_pcie *afp)	/* NVMe: PERST#를 deassert하여 NVMe 디바이스가 초기화 후 정상 동작 시작 */
{
	/* Deassert controller PERST_N */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_PERST_N);	/* NVMe: 컨트롤러 PERST# 레지스터를 해제; NVMe 디바이스의 리셋 상태 해제 준비 */
	/* Deassert PERST_N GPIO */
	gpiod_set_value_cansleep(afp->reset, 1);			/* NVMe: GPIO PERST#를 inactive-high로 해제; NVMe 컨트롤러가 PCIe 링크 트레이닝 시작 */
}

static void fu740_pcie_power_on(struct fu740_pcie *afp)	/* NVMe: PCIe/NVMe 슬롯 전원을 켜고 PERST# 홀드 타임을 준수 */
{
	gpiod_set_value_cansleep(afp->pwren, 1);			/* NVMe: power-enable GPIO를 high로 설정; NVMe SSD/카드에 전원 공급 시작 */
	/*
	 * Ensure that PERST has been asserted for at least 100 ms.
	 * Section 2.2 of PCI Express Card Electromechanical Specification
	 * Revision 3.0
	 */
	msleep(100);							/* NVMe: PCIe CEM 스펙 준수; 전원 안정화 및 NVMe 디바이스 낸부 회로 초기화 대기 */
}

static void fu740_pcie_drive_reset(struct fu740_pcie *afp)	/* NVMe: 전체 리셋 시퀀스 수행; NVMe 호스트-디바이스 링크 초기화 */
{
	fu740_pcie_assert_reset(afp);					/* NVMe: 먼저 PERST# assert; NVMe 디바이스를 알려진 리셋 상태로 만듦 */
	fu740_pcie_power_on(afp);					/* NVMe: 전원 인가 및 100ms 대기; NVMe 플랫폼 전원 안정화 */
	fu740_pcie_deassert_reset(afp);					/* NVMe: PERST# 해제; NVMe 디바이스가 링크 트레이닝 및 열거 준비 */
}

static void fu740_phyregwrite(const uint8_t phy, const uint16_t addr,		/* NVMe: 대상 PHY 번호(0/1); x8 링크의 전/후반 4레인 구분 */
			      const uint16_t wrdata, struct fu740_pcie *afp)	/* NVMe: PHY 레인 설정 값; NVMe 링크 신호 품질에 영향 */
{
	struct device *dev = afp->pci.dev;					/* NVMe: DesignWare PCIe 코어의 device 구조체; 경고 메시지 출력용 */
	void __iomem *phy_cr_para_addr;						/* NVMe: PHY CR 어드레스 레지스터 매핑 포인터 */
	void __iomem *phy_cr_para_wr_data;					/* NVMe: PHY CR 쓰기 데이터 레지스터 매핑 포인터 */
	void __iomem *phy_cr_para_wr_en;					/* NVMe: PHY CR 쓰기 enable 레지스터 매핑 포인터 */
	void __iomem *phy_cr_para_ack;						/* NVMe: PHY CR ack 레지스터 매핑 포인터 */
	int ret, val;								/* NVMe: 폴링 반환값(ret)과 읽은 ack 값(val) */

	/* Setup */
	if (phy) {								/* NVMe: PHY1(뒤 4레인) 선택 시 */
		phy_cr_para_addr = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_ADDR;	/* NVMe: PHY1 주소 포트 매핑 */
		phy_cr_para_wr_data = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_WR_DATA;	/* NVMe: PHY1 데이터 포트 매핑 */
		phy_cr_para_wr_en = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_WR_EN;	/* NVMe: PHY1 쓰기 enable 포트 매핑 */
		phy_cr_para_ack = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_ACK;	/* NVMe: PHY1 ack 포트 매핑 */
	} else {								/* NVMe: PHY0(앞 4레인) 선택 시 */
		phy_cr_para_addr = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_ADDR;	/* NVMe: PHY0 주소 포트 매핑 */
		phy_cr_para_wr_data = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_WR_DATA;	/* NVMe: PHY0 데이터 포트 매핑 */
		phy_cr_para_wr_en = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_WR_EN;	/* NVMe: PHY0 쓰기 enable 포트 매핑 */
		phy_cr_para_ack = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_ACK;	/* NVMe: PHY0 ack 포트 매핑 */
	}

	writel_relaxed(addr, phy_cr_para_addr);					/* NVMe: 쓰고자 하는 PHY 레지스터 오프셋 설정 */
	writel_relaxed(wrdata, phy_cr_para_wr_data);				/* NVMe: 해당 PHY 레지스터에 기록할 데이터 설정; NVMe 링크 물리층 파라미터 */
	writel_relaxed(1, phy_cr_para_wr_en);					/* NVMe: PHY CR 쓰기 트랜잭션 시작 */

	/* Wait for wait_idle */
	ret = readl_poll_timeout(phy_cr_para_ack, val, val, 10, 5000);	/* NVMe: ack가 1이 될 때까지 최대 5ms(10us 단위) 폴링; PHY 쓰기 완료 대기 */
	if (ret)								/* NVMe: 타임아웃 발생 시 */
		dev_warn(dev, "Wait for wait_idle state failed!\n");		/* NVMe: PHY 쓰기 완료 대기 실패 경고; NVMe 링크 품질 저하 가능성 */

	/* Clear */
	writel_relaxed(0, phy_cr_para_wr_en);					/* NVMe: 쓰기 enable 클리어; 다음 PHY 트랜잭션 준비 */

	/* Wait for ~wait_idle */
	ret = readl_poll_timeout(phy_cr_para_ack, val, !val, 10, 5000);	/* NVMe: ack가 0으로 돌아갈 때까지 폴링; PHY 상태 머신 idle 복귀 확인 */
	if (ret)								/* NVMe: 타임아웃 발생 시 */
		dev_warn(dev, "Wait for !wait_idle state failed!\n");		/* NVMe: PHY idle 복귀 실패 경고; 링크 트레이닝 이상 가능성 */
}

static void fu740_pcie_init_phy(struct fu740_pcie *afp)	/* NVMe: PHY(물리층) 초기화; NVMe 디바이스와의 신뢰적 PCIe 링크 형성을 위한 핵심 단계 */
{
	/* Enable phy cr_para_sel interfaces */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_SEL);	/* NVMe: PHY0 CR 인터페이스 활성화; 파라미터 편집 가능하도록 설정 */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_SEL);	/* NVMe: PHY1 CR 인터페이스 활성화; x8 양쪽 PHY 모두 편집 가능 */

	/*
	 * Wait 10 cr_para cycles to guarantee that the registers are ready
	 * to be edited.
	 */
	ndelay(10);								/* NVMe: CR 인터페이스 준비 시간; 짧은 지연 후 PHY 레지스터 안전 편집 */

	/* Set PHY AC termination mode */
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE0_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY0 lane0 종단/CDR/LOS 설정; NVMe 신호 무결성 확보 */
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE1_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY0 lane1 동일 설정; x8 링크의 앞쪽 4레인 중 두 번째 */
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE2_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY0 lane2 동일 설정 */
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE3_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY0 lane3 동일 설정; PHY0 4레인 모두 동일 RX override 적용 */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE0_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY1 lane0 동일 설정; x8 링크의 뒤쪽 4레인 시작 */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE1_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY1 lane1 동일 설정 */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE2_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY1 lane2 동일 설정 */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE3_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);	/* NVMe: PHY1 lane3 동일 설정; x8 전체 8레인 PHY 초기화 완료 */
}

static int fu740_pcie_start_link(struct dw_pcie *pci)	/* NVMe: PCIe 링크 트레이닝을 시작하고 속도를 협상; NVMe 디바이스가 config space에 응답 가능해지는 시점 */
{
	struct device *dev = pci->dev;				/* NVMe: 디버그/에러 메시지용 device */
	struct fu740_pcie *afp = dev_get_drvdata(dev);		/* NVMe: SoC별 상태 접근 */
	u8 cap_exp = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);	/* NVMe: PCIe Express Capability 오프셋 획득; LNKCAP 등 NVMe 링크 제어 레지스터 접근에 필요 */
	int ret;							/* NVMe: 함수 반환값 */
	u32 orig, tmp;							/* NVMe: orig=원래 링크 속도, tmp=임시 레지스터 값 */

	/*
	 * Force 2.5GT/s when starting the link, due to some devices not
	 * probing at higher speeds. This happens with the PCIe switch
	 * on the Unmatched board when U-Boot has not initialised the PCIe.
	 * The fix in U-Boot is to force 2.5GT/s, which then gets cleared
	 * by the soft reset done by this driver.
	 */
	dev_dbg(dev, "cap_exp at %x\n", cap_exp);			/* NVMe: PCIe Express Capability 위치 로깅; NVMe 호스트 디버깅 시 유용 */
	dw_pcie_dbi_ro_wr_en(pci);						/* NVMe: DBI(Dwarf Bus Interface)의 read-only 레지스터 쓰기 가능하게 설정; LNKCAP 수정 가능 */

	tmp = dw_pcie_readl_dbi(pci, cap_exp + PCI_EXP_LNKCAP);	/* NVMe: Link Capability 레지스터 읽기; 현재 지원 링크 속도 확인 */
	orig = tmp & PCI_EXP_LNKCAP_SLS;				/* NVMe: 원래 Supported Link Speed 필드 보관; 나중에 복원 */
	tmp &= ~PCI_EXP_LNKCAP_SLS;					/* NVMe: SLS 필드를 클리어 */
	tmp |= PCI_EXP_LNKCAP_SLS_2_5GB;				/* NVMe: 2.5GT/s(Gen1)로 강제; 일부 NVMe/스위치가 높은 속도에서 인식되지 않는 문제 회피 */
	dw_pcie_writel_dbi(pci, cap_exp + PCI_EXP_LNKCAP, tmp);		/* NVMe: 강제 Gen1 설정을 PCIe 코어에 기록; NVMe 링크 초기 협상 속도 제한 */

	/* Enable LTSSM */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_APP_LTSSM_ENABLE);	/* NVMe: LTSSM 활성화; PCIe 물리/링크 상태 머신 시작 → NVMe 장치와의 link-up 시도 */

	ret = dw_pcie_wait_for_link(pci);					/* NVMe: link-up(L0 상태) 대기; NVMe 디바이스가 config request에 응답하려면 먼저 링크가 UP해야 함 */
	if (ret) {								/* NVMe: 링크 시작 실패 시 */
		dev_err(dev, "error: link did not start\n");			/* NVMe: NVMe 호스트가 디바이스를 검출하지 못함; 드라이브 인식 실패 */
		goto err;							/* NVMe: 에러 처리 경로로 점프; DBI 쓰기 보호 복원 */
	}

	tmp = dw_pcie_readl_dbi(pci, cap_exp + PCI_EXP_LNKCAP);	/* NVMe: 링크 업 후 LNKCAP 재확인; 원래 속도로 복원 가능한지 검사 */
	if ((tmp & PCI_EXP_LNKCAP_SLS) != orig) {			/* NVMe: 현재 속도가 원래 속도와 다를 때(즉 Gen1로 강제 중일 때) */
		dev_dbg(dev, "changing speed back to original\n");	/* NVMe: 원래 속도로 재협상 시도 로깅 */

		tmp &= ~PCI_EXP_LNKCAP_SLS;					/* NVMe: SLS 필드 다시 클리어 */
		tmp |= orig;							/* NVMe: 원래 지원 속도로 복원; NVMe SSD가 Gen3/Gen4를 지원하면 더 높은 속도로 재협상 */
		dw_pcie_writel_dbi(pci, cap_exp + PCI_EXP_LNKCAP, tmp);	/* NVMe: 복원된 속도를 LNKCAP에 기록 */

		tmp = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);	/* NVMe: DesignWare link width/speed control 레지스터 읽기 */
		tmp |= PORT_LOGIC_SPEED_CHANGE;					/* NVMe: speed change 요청 비트 설정; NVMe 링크 재협상 트리거 */
		dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, tmp);	/* NVMe: speed change 명령 기록 */

		ret = dw_pcie_wait_for_link(pci);				/* NVMe: 재협상 후 link-up 다시 대기 */
		if (ret) {							/* NVMe: 재협상 실패 시 */
			dev_err(dev, "error: link did not start at new speed\n");	/* NVMe: 고속 재협상 실패; NVMe 성능 저하 또는 장치 미인식 */
			goto err;						/* NVMe: 에러 처리 경로로 점프 */
		}
	}

	ret = 0;								/* NVMe: 링크 트레이닝 최종 성공 */
err:
	WARN_ON(ret);	/* we assume that errors will be very rare */		/* NVMe: 에러 시 경고; 일반적으로는 리셋 후 재시도 또는 NVMe 장치 교체 필요 */
	dw_pcie_dbi_ro_wr_dis(pci);						/* NVMe: DBI read-only 쓰기 보호 복원; 이후 config space 접근 안전성 유지 */
	return ret;								/* NVMe: 0이면 NVMe 열거 계속, 음수면 dw_pcie_host_init() 실패 처리 */
}

static int fu740_pcie_host_init(struct dw_pcie_rp *pp)	/* NVMe: DesignWare PCIe RC(root complex) 호스트 초기화 콜백; NVMe 디바이스를 위한 PCIe 버스 생성 직전에 호출 */
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);			/* NVMe: dw_pcie 코어 구조체 획득; config space, MSI, DMA 설정 등에 사용 */
	struct fu740_pcie *afp = to_fu740_pcie(pci);			/* NVMe: SiFive FU740 전용 상태 획득; GPIO/clk/reset/mgmt 레지스터 제어 */
	struct device *dev = pci->dev;					/* NVMe: 디버그/에러 출력용 device */
	int ret;								/* NVMe: 함수 반환값 */

	/* Power on reset */
	fu740_pcie_drive_reset(afp);					/* NVMe: PERST# assert → 전원 인가 → PERST# deassert; NVMe 디바이스를 클린 상태로 시작 */

	/* Enable pcieauxclk */
	ret = clk_prepare_enable(afp->pcie_aux);			/* NVMe: PCIe 보조 클럭 활성화; 링크 레이어/PHY 디지털 로직 및 NVMe 트랜잭션 처리에 필수 */
	if (ret) {								/* NVMe: 클럭 활성화 실패 시 */
		dev_err(dev, "unable to enable pcie_aux clock\n");	/* NVMe: NVMe 호스트 하드웨어가 동작 불가 */
		return ret;							/* NVMe: 초기화 실패; NVMe 장치가 검출되지 않음 */
	}

	/*
	 * Assert hold_phy_rst (hold the controller LTSSM in reset after
	 * power_up_rst_n for register programming with cr_para)
	 */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_APP_HOLD_PHY_RST);	/* NVMe: PHY 리셋 홀드; LTSSM은 멈춘 상태에서 PHY CR 파라미터 프로그래밍 */

	/* Deassert power_up_rst_n */
	ret = reset_control_deassert(afp->rst);					/* NVMe: SoC PCIe 컨트롤러 하드 리셋 해제; NVMe 호스트 코어가 레지스터 접근 가능 */
	if (ret) {								/* NVMe: 리셋 해제 실패 시 */
		dev_err(dev, "unable to deassert pcie_power_up_rst_n\n");	/* NVMe: 컨트롤러가 정지 상태; NVMe 열수 불가 */
		return ret;							/* NVMe: 초기화 실패 */
	}

	fu740_pcie_init_phy(afp);						/* NVMe: PHY 레인별 termination/CDR/LOS 설정; NVMe 링크 품질 확보 */

	/* Disable pcieauxclk */
	clk_disable_unprepare(afp->pcie_aux);					/* NVMe: PHY 프로그래밍 중 불필요한 클럭 소모 방지; 일시적으로 클럭 정지 */
	/* Clear hold_phy_rst */
	writel_relaxed(0x0, afp->mgmt_base + PCIEX8MGMT_APP_HOLD_PHY_RST);	/* NVMe: PHY 리셋 홀드 해제; LTSSM이 링크 트레이닝 준비 가능 */
	/* Enable pcieauxclk */
	clk_prepare_enable(afp->pcie_aux);					/* NVMe: PCIe 클럭 다시 활성화; 링크 트레이닝 및 NVMe 트랜잭션에 필요 */
	/* Set RC mode */
	writel_relaxed(0x4, afp->mgmt_base + PCIEX8MGMT_DEVICE_TYPE);		/* PCI/NVMe: 디바이스 타입을 RC로 설정; NVMe 디바이스를 아래 단말(endpoint)로 열수하는 버스 역할 */

	return 0;								/* NVMe: SoC별 초기화 성공; 이후 dw_pcie_host_init()가 config space, MSI, IO/mem 윈도우 설정 */
}

static const struct dw_pcie_host_ops fu740_pcie_host_ops = {	/* NVMe: DesignWare RC 호스트 동작 테이블; NVMe PCIe 버스 생성 시 필요한 SoC별 콜백 */
	.init = fu740_pcie_host_init,						/* NVMe: RC 초기화 콜백; NVMe 장치 열수 전 리셋/클록/PHY/RC 모드 설정 */
};

static const struct dw_pcie_ops dw_pcie_ops = {		/* NVMe: DesignWare PCIe 코어 동작 테이블; 링크 트레이닝 등 NVMe 관련 low-level 동작 */
	.start_link = fu740_pcie_start_link,				/* NVMe: 링크 트레이닝 콜백; NVMe 디바이스와의 PCIe 연결 수립 */
};

static int fu740_pcie_probe(struct platform_device *pdev)	/* NVMe: platform 드라이버 probe; FU740 PCIe RC를 초기화하고 NVMe 호스트용 PCIe 버스를 등록 */
{
	struct device *dev = &pdev->dev;				/* NVMe: platform_device의 device; DT 리소스/IRQ/DMA 바인딩에 사용 */
	struct dw_pcie *pci;						/* NVMe: DesignWare PCIe 코어 구조체 포인터 */
	struct fu740_pcie *afp;						/* NVMe: FU740 전용 상태 포인터 */

	afp = devm_kzalloc(dev, sizeof(*afp), GFP_KERNEL);		/* NVMe: FU740 상태 구조체 할당; probe → remove 생애주기 동안 관리 */
	if (!afp)								/* NVMe: 메모리 부족 시 */
		return -ENOMEM;							/* NVMe: NVMe 호스트 초기화 실패; PCIe 버스 미생성 */
	pci = &afp->pci;							/* NVMe: fu740_pcie 내 dw_pcie 위치; DesignWare 코어 초기화 대상 */
	pci->dev = dev;								/* NVMe: dw_pcie에 device 연결; 리소스/DMA/IRQ 매핑의 기준 */
	pci->ops = &dw_pcie_ops;						/* NVMe: DesignWare 코어에 링크 트레이닝 콜백 등록; NVMe 링크 수립 */
	pci->pp.ops = &fu740_pcie_host_ops;					/* NVMe: RC 호스트 콜백 등록; NVMe 열수 전 SoC 초기화 수행 */
	pci->pp.num_vectors = MAX_MSI_IRQS;					/* PCI/NVMe: MSI 인터럽트 벡터 최대 개수 설정; NVMe 큐별 완료 인터럽트 할당의 상한 */

	/* SiFive specific region: mgmt */
	afp->mgmt_base = devm_platform_ioremap_resource_byname(pdev, "mgmt");	/* NVMe: "mgmt" DT 메모리 영역을 ioremap; PERST#/LTSSM/DEVICE_TYPE/PHY CR 접근 가능 */
	if (IS_ERR(afp->mgmt_base))						/* NVMe: 매핑 실패 시 */
		return PTR_ERR(afp->mgmt_base);					/* NVMe: NVMe 호스트가 컨트롤러 레지스터에 접근 불가 */

	/* Fetch GPIOs */
	afp->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);	/* NVMe: "reset" GPIO 획득; 초기값 LOW로 PERST# assert 상태; NVMe 카드 리셋 제어 */
	if (IS_ERR(afp->reset))						/* NVMe: GPIO 획득 오류 시 */
		return dev_err_probe(dev, PTR_ERR(afp->reset), "unable to get reset-gpios\n");	/* NVMe: 리셋 GPIO 없으면 NVMe 장치 초기화 불가 */

	afp->pwren = devm_gpiod_get_optional(dev, "pwren", GPIOD_OUT_LOW);	/* NVMe: "pwren" GPIO 획득; 초기값 LOW로 전원 off; NVMe 카드 전원 제어 */
	if (IS_ERR(afp->pwren))						/* NVMe: GPIO 획득 오류 시 */
		return dev_err_probe(dev, PTR_ERR(afp->pwren), "unable to get pwren-gpios\n");	/* NVMe: 전원 GPIO 없으면 NVMe 카드 구동 불가 */

	/* Fetch clocks */
	afp->pcie_aux = devm_clk_get(dev, "pcie_aux");				/* NVMe: "pcie_aux" 클럭 획득; PCIe/NVMe 링크 레이어 동작 클럭 */
	if (IS_ERR(afp->pcie_aux))						/* NVMe: 클럭 획득 오류 시 */
		return dev_err_probe(dev, PTR_ERR(afp->pcie_aux),
				     "pcie_aux clock source missing or invalid\n");	/* NVMe: PCIe 클럭 없음; NVMe 트랜잭션 불가 */

	/* Fetch reset */
	afp->rst = devm_reset_control_get_exclusive(dev, NULL);		/* NVMe: PCIe 컨트롤러 전용 리셋 라인 획득; NVMe 호스트 하드웨어 클린 초기화 */
	if (IS_ERR(afp->rst))							/* NVMe: 리셋 획득 오류 시 */
		return dev_err_probe(dev, PTR_ERR(afp->rst), "unable to get reset\n");	/* NVMe: 컨트롤러 리셋 제어 불가; NVMe 열수 위험 */

	platform_set_drvdata(pdev, afp);					/* NVMe: platform_device에 afp 저장; shutdown 등에서 NVMe 링크 정리 시 사용 */

	return dw_pcie_host_init(&pci->pp);					/* PCI/NVMe: DesignWare RC 호스트 초기화 및 PCIe 버스 등록; 이후 NVMe 디바이스가 pci_bus_scan()으로 발견/바인딩됨 */
}

static void fu740_pcie_shutdown(struct platform_device *pdev)	/* NVMe: 시스템 종료/재부팅 시 호출; NVMe 디바이스에 클린한 리셋 상태를 남겨 bootloader 안정성 확보 */
{
	struct fu740_pcie *afp = platform_get_drvdata(pdev);		/* NVMe: probe에서 저장한 FU740 상태 복원 */

	/* Bring down link, so bootloader gets clean state in case of reboot */
	fu740_pcie_assert_reset(afp);						/* NVMe: PERST# assert로 링크 다운; 재부팅 시 NVMe 장치가 초기 상태에서 열수되도록 함 */
}

static const struct of_device_id fu740_pcie_of_match[] = {	/* NVMe: Device Tree 호환성 테이블; 해당 문자열이 있는 DT 노드에 이 드라이버 바인딩 */
	{ .compatible = "sifive,fu740-pcie", },				/* NVMe: SiFive FU740 PCIe RC 노드; NVMe 호스트로 동작 */
	{},									/* NVMe: of_device_id 테이블 종료 표시자 */
};

static struct platform_driver fu740_pcie_driver = {		/* NVMe: platform_driver 구조체; 커널 부팅 시 FU740 PCIe RC 등록 및 NVMe 호스트 초기화 */
	.driver = {
		   .name = "fu740-pcie",					/* NVMe: 드라이버 이름; /sys/bus/platform/drivers/fu740-pcie */
		   .of_match_table = fu740_pcie_of_match,			/* NVMe: DT 호환성 매칭; sifive,fu740-pcie 노드 발견 시 probe 호출 */
		   .suppress_bind_attrs = true,					/* NVMe: 수동 bind/unbind 파일 노출 억제; RC는 런타임 재바인딩이 위험 */
	},
	.probe = fu740_pcie_probe,						/* NVMe: 장치 발견 시 NVMe 호스트 초기화 수행 */
	.shutdown = fu740_pcie_shutdown,					/* NVMe: 종료 시 PERST# assert로 NVMe 링크 안전 정리 */
};

builtin_platform_driver(fu740_pcie_driver);				/* NVMe: 커널 빌트인으로 FU740 PCIe RC 드라이버 등록; 부팅 시 NVMe PCIe 호스트 초기화 */
