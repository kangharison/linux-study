// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] HiSilicon STB(Hi3798CV200) 의 DesignWare PCIe 호스트 글루 (pcie-histb.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 HiSilicon 셋톱박스 SoC 에 붙이는 글루
 * 드라이버다. config 접근, ATU 설정, MSI, 버스 스캔은 모두
 * pcie-designware-host.c 가 하고, 이 파일은 DWC 코어가 알 수 없는 SoC
 * 고유의 것만 맡는다.
 *
 * 이 파일에서 가장 특이한 것은 **DBI 접근이 사이드밴드 신호를 켜야만
 * 통한다는 점** 이다. 다른 글루 드라이버들이 DBI 창을 그냥 읽고 쓰는 것과
 * 달리, 이 하드웨어는 접근 직전에 제어 레지스터의 ELBI_SLV_DBI_ENABLE 을
 * 켜고 접근 뒤 다시 꺼야 한다. 그래서 이 파일은 DWC 코어의 read_dbi/
 * write_dbi 콜백을 채워, 코어의 모든 DBI 접근이 그 켜기-접근-끄기 삼단을
 * 거치게 만든다.
 *
 * 읽기와 쓰기의 사이드밴드 비트가 **서로 다른 레지스터** 에 있다는 점도
 * 눈여겨볼 만하다 — 읽기는 PCIE_SYS_CTRL1, 쓰기는 PCIE_SYS_CTRL0 이며,
 * 두 함수가 같은 비트 이름을 다른 자리에 쓴다.
 *
 * 맡는 일이 넷이다.
 *   1) DBI 사이드밴드 감싸기(위 설명).
 *   2) 루트 포트 자신의 config 접근 — 슬롯 0 만 받아들이고 나머지는
 *      "장치 없음" 으로 답한다.
 *   3) RC 모드 설정과 LTSSM 시작.
 *   4) 전원·클럭·리셋 관리 — 클럭 넷과 리셋 셋, 선택적 전원과 리셋 GPIO.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> histb_pcie_probe()
 *     -> "control" 창과 "rc-dbi" 창, 전원, GPIO, 클럭 넷, 리셋 셋, PHY 를 얻는다
 *     -> histb_pcie_host_enable()
 *        -> 전원 -> GPIO 리셋 해제 -> 클럭 넷 -> 리셋 셋을 차례로 토글
 *     -> dw_pcie_host_init()  [pcie-designware-host.c]
 *        -> 그 안에서 콜백 -> [이 파일] histb_pcie_host_init()
 *           -> 루트 버스용 pci_ops 를 갈아 끼우고 RC 모드를 지정
 *        -> 코어가 start_link 콜백을 부른다 -> [이 파일] LTSSM 켜기
 *        -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * config 접근이 두 갈래로 갈린다:
 *   루트 포트 자신 -> histb_pcie_rd/wr_own_conf() -> dw_pcie_read/write_dbi()
 *     -> [이 파일] histb_pcie_read/write_dbi() -> 사이드밴드 켜기-접근-끄기
 *   하위 장치     -> DWC 코어의 기본 경로(ATU 를 통한 config 창)
 *
 * 실행 컨텍스트: probe 와 host_init 은 프로세스 컨텍스트다. DBI 접근
 * 경로는 PCI 코어가 잠금을 쥔 채 부를 수 있어 잠들지 않는다.
 * 이 파일에는 인터럽트 핸들러가 없다 — INTx 와 MSI 를 모두 DWC 코어가 다룬다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c. 접점이 두 벌의
 *   콜백 표다 — dw_pcie_ops(read_dbi, write_dbi, link_up, start_link)와
 *   dw_pcie_host_ops(init 하나). **read_dbi/write_dbi 를 채우는 글루는
 *   드물다** — 대부분은 코어의 기본 접근으로 충분하다.
 * 옆쪽: regulator·gpio·clk·reset·phy 계층. 모두 이 트리에 없어 내부는
 *   확인 대상 밖이며, 이 파일에서 읽히는 호출 규약까지만 적었다.
 *
 * 데이터 흐름:
 *   디바이스 트리("control"/"rc-dbi" 자원, 선택적 vpcie 와 reset GPIO,
 *                 클럭 넷, 리셋 셋, 선택적 PHY) -> probe -> struct histb_pcie
 *   DBI 접근: 사이드밴드 켜기(제어 창) -> 실제 접근(DBI 창) -> 끄기(제어 창)
 *   즉 한 번의 논리적 접근이 **세 번의 MMIO** 로 이뤄진다.
 *
 * 공유 상태: struct histb_pcie 하나. probe 후 불변이며 잠금이 없다 —
 *   사이드밴드 켜기-끄기 쌍의 원자성은 PCI 코어의 잠금에 기댄다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면 그 config 접근은 위 두 갈래 중
 * 하위 장치 경로로 가므로 사이드밴드 삼단을 지나지 않는다. 이 파일의
 * DBI 감싸기가 닿는 것은 루트 포트 자신의 레지스터뿐이다.
 *
 * === 주요 함수/구조체 요약 ===
 * histb_pcie_read_dbi()/write_dbi() : 사이드밴드를 켜고 접근한 뒤 끈다.
 *                                     이 파일의 핵심이다.
 * histb_pcie_dbi_r_mode()/w_mode()  : 그 사이드밴드 비트를 다룬다.
 *                                     읽기와 쓰기가 서로 다른 레지스터를 쓴다.
 * histb_pcie_rd/wr_own_conf()       : 루트 포트 자신의 config 접근.
 * histb_pcie_host_enable()/disable(): 전원·클럭 넷·리셋 셋을 켜고 끈다.
 * histb_pcie_link_up()              : 링크 비트 둘과 LTSSM 상태까지 **셋** 을 본다.
 * struct histb_pcie                 : dw_pcie 를 포인터로 든 이 드라이버의 상태.
 */

/*
 * PCIe host controller driver for HiSilicon STB SoCs
 *
 * Copyright (C) 2016-2017 HiSilicon Co., Ltd. http://www.hisilicon.com
 *
 * Authors: Ruqiang Ju <juruqiang@hisilicon.com>
 *          Jianguo Sun <sunjianguo1@huawei.com>
 */

/* [한국어] clk_prepare_enable()/clk_disable_unprepare(). 클럭 넷을 다룬다. */
#include <linux/clk.h>
/* [한국어] 지연 헬퍼. 이 파일에서 직접 쓰는 곳은 없다(전수 확인). */
#include <linux/delay.h>
/* [한국어] gpiod_set_value_cansleep() 과 devm_gpiod_get_optional(). */
#include <linux/gpio/consumer.h>
/* [한국어] 인터럽트 헤더. 이 파일은 핸들러를 두지 않는다(전수 확인) —
 * INTx 와 MSI 를 모두 DWC 코어가 다룬다. */
#include <linux/interrupt.h>
/* [한국어] 기본 커널 관용구. */
#include <linux/kernel.h>
/* [한국어] MODULE_ 매크로들. */
#include <linux/module.h>
/* [한국어] of_ 헤더. */
#include <linux/of.h>
/* [한국어] PCI_SLOT() 과 PCIBIOS_ 반환 코드. */
#include <linux/pci.h>
/* [한국어] phy_init()/phy_exit()/devm_phy_get(). */
#include <linux/phy/phy.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] struct resource. */
#include <linux/resource.h>
/* [한국어] reset_control_assert()/deassert(). 리셋 셋을 토글한다. */
#include <linux/reset.h>

/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_read()/write(),
 * dw_pcie_read_dbi()/write_dbi(), dw_pcie_host_init() 등. */
#include "pcie-designware.h"

/* [한국어] dw_pcie 포인터에서 이 드라이버의 상태를 되찾는 통로.
 * container_of 가 아니라 drvdata 인 것은, 이 드라이버가 dw_pcie 를
 * **포인터로** 들고 있어 두 구조체가 한 덩어리가 아니기 때문이다.
 * 전제: probe 가 platform_set_drvdata 로 상태를 미리 심어 두어야 한다. */
#define to_histb_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어] 제어 레지스터 0 — 장치 종류 필드와 **DBI 쓰기** 사이드밴드가 함께 있다.
 * 그래서 두 자리 모두 읽기-수정-쓰기로 다뤄야 한다. */
#define PCIE_SYS_CTRL0		0x0000
/* [한국어] 제어 레지스터 1 — **DBI 읽기** 사이드밴드가 여기 있다.
 * 읽기와 쓰기를 따로 열 수 있게 한 설계로 보이나 근거는 확인 못 함. */
#define PCIE_SYS_CTRL1		0x0004
/* [한국어] 제어 레지스터 7 — LTSSM 시작 비트가 여기 있다. */
#define PCIE_SYS_CTRL7		0x001C
/* [한국어] 제어 레지스터 13. 이 파일에서 읽는 곳은 없다(전수 확인). */
#define PCIE_SYS_CTRL13		0x0034
/* [한국어] 제어 레지스터 15. 역시 쓰이지 않는다. */
#define PCIE_SYS_CTRL15		0x003C
/* [한국어] 제어 레지스터 16. 역시 쓰이지 않는다. */
#define PCIE_SYS_CTRL16		0x0040
/* [한국어] 제어 레지스터 17. 네 개 모두 정의만 있고 참조가 없다 —
 * 하드웨어에 존재하는 레지스터를 표로 적어 둔 것으로 보인다. */
#define PCIE_SYS_CTRL17		0x0044

/* [한국어] 상태 레지스터 0 — 링크 비트 둘이 여기 있다. */
#define PCIE_SYS_STAT0		0x0100
/* [한국어] 상태 레지스터 4 — LTSSM 상태가 여기 있다.
 * **링크 판정이 레지스터 둘을 읽어야 하는 이유** 다. */
#define PCIE_SYS_STAT4		0x0110

/* [한국어] 데이터 링크 계층의 링크 확립 비트. */
#define PCIE_RDLH_LINK_UP	BIT(5)
/* [한국어] 물리 계층의 링크 확립 비트. */
#define PCIE_XMLH_LINK_UP	BIT(15)
/* [한국어] DBI 사이드밴드 활성 비트. **CTRL0 과 CTRL1 양쪽에서 같은 비트 자리를
 * 쓰며**, 어느 레지스터에 쓰느냐가 읽기냐 쓰기냐를 가른다. */
#define PCIE_ELBI_SLV_DBI_ENABLE BIT(21)
/* [한국어] LTSSM 시작 비트. start_link 가 세우며, 끄는 짝은 없다. */
#define PCIE_APP_LTSSM_ENABLE	BIT(11)

/* [한국어] 장치 종류 필드(비트 28~31). 모드를 바꿀 때 이 마스크로 먼저 지운다. */
#define PCIE_DEVICE_TYPE_MASK	GENMASK(31, 28)
/* [한국어] 엔드포인트 값. **0 이라** 필드를 지우는 것만으로 EP 가 된다 —
 * 이 파일에서 읽는 곳은 없다. */
#define PCIE_WM_EP		0
/* [한국어] legacy 모드 값. 역시 이 파일에서 읽는 곳은 없다. */
#define PCIE_WM_LEGACY		BIT(1)
/* [한국어] 루트 컴플렉스 값. host_init 이 이 값을 넣는다.
 * 세 값 중 이것만 실제로 쓰인다. */
#define PCIE_WM_RC		BIT(30)

/* [한국어] LTSSM 상태 필드(하위 6비트). */
#define PCIE_LTSSM_STATE_MASK	GENMASK(5, 0)
/* [한국어] L0(정상 동작) 상태 값. link_up 이 이 값과 **동등 비교** 한다 —
 * 비트 마스크가 아니라 정확히 이 값이어야 한다. */
#define PCIE_LTSSM_STATE_ACTIVE	0x11

struct histb_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **포인터로 든다** — 값으로 품는 글루들과 달라
	 * 이 구조체 포인터가 dw_pcie 포인터가 되지 않는다.
	 * 설정자: probe 가 따로 할당해 매단다.
	 * 읽는 자: 이 파일의 모든 함수와 DWC 코어.
	 * 값 범위: 유효한 dw_pcie 포인터.
	 * 동기화: probe 후 불변. */
	struct dw_pcie *pci;
	/* [한국어] 보조 클럭.
	 * 설정자: probe 의 devm_clk_get().
	 * 읽는 자: histb_pcie_host_enable() 이 켜고 disable() 이 끈다.
	 * 값 범위: 유효한 clk 포인터. 넷 모두 필수라 못 얻으면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	struct clk *aux_clk;
	/* [한국어] PIPE 클럭(PHY 와 컨트롤러 사이의 인터페이스).
	 * 설정자: probe 의 devm_clk_get().
	 * 읽는 자: histb_pcie_host_enable() 이 켜고 disable() 이 끈다.
	 * 값 범위: 유효한 clk 포인터. 넷 모두 필수라 못 얻으면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	struct clk *pipe_clk;
	/* [한국어] 시스템 클럭.
	 * 설정자: probe 의 devm_clk_get().
	 * 읽는 자: histb_pcie_host_enable() 이 켜고 disable() 이 끈다.
	 * 값 범위: 유효한 clk 포인터. 넷 모두 필수라 못 얻으면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	struct clk *sys_clk;
	/* [한국어] 버스 클럭. **켜는 순서가 bus → sys → pipe → aux** 이며,
	 * 끄는 순서는 정확히 그 역순이다.
	 * 설정자: probe 의 devm_clk_get().
	 * 읽는 자: histb_pcie_host_enable() 이 켜고 disable() 이 끈다.
	 * 값 범위: 유효한 clk 포인터. 넷 모두 필수라 못 얻으면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	struct clk *bus_clk;
	/* [한국어] PCIe PHY.
	 * 설정자: probe 의 devm_phy_get(). **못 얻어도 실패로 다루지 않고** NULL 로 둔다.
	 * 읽는 자: probe 가 초기화하고 remove 와 되감기가 해제한다.
	 * 값 범위: 유효한 PHY 포인터 또는 NULL.
	 * 동기화: probe 후 불변. */
	struct phy *phy;
	/* [한국어] 소프트 리셋.
	 * 설정자: probe 의 devm_reset_control_get().
	 * 읽는 자: histb_pcie_host_enable() 이 걸었다 풀고, disable() 이 건다.
	 * 값 범위: 유효한 리셋 컨트롤. 셋 모두 필수다.
	 * 동기화: probe 후 불변. */
	struct reset_control *soft_reset;
	/* [한국어] 시스템 리셋.
	 * 설정자: probe 의 devm_reset_control_get().
	 * 읽는 자: histb_pcie_host_enable() 이 걸었다 풀고, disable() 이 건다.
	 * 값 범위: 유효한 리셋 컨트롤. 셋 모두 필수다.
	 * 동기화: probe 후 불변. */
	struct reset_control *sys_reset;
	/* [한국어] 버스 리셋. **셋 모두 걸었다 푸는 토글** 로 쓰인다 — 단순 해제가 아니다.
	 * 설정자: probe 의 devm_reset_control_get().
	 * 읽는 자: histb_pcie_host_enable() 이 걸었다 풀고, disable() 이 건다.
	 * 값 범위: 유효한 리셋 컨트롤. 셋 모두 필수다.
	 * 동기화: probe 후 불변. */
	struct reset_control *bus_reset;
	/* [한국어] SoC 제어 창의 가상 주소.
	 * 설정자: probe 의 ioremap("control").
	 * 읽는 자: histb_pcie_readl()/writel().
	 * 값 범위: 유효한 iomem 포인터. **DWC 의 DBI 창과 별개** 이며,
	 * 모드·LTSSM·링크 상태·DBI 사이드밴드가 모두 이 창에 있다.
	 * 동기화: probe 후 불변. */
	void __iomem *ctrl;
	/* [한국어] 하위 장치로 내보내는 리셋 GPIO.
	 * 설정자: probe 의 devm_gpiod_get_optional("reset", GPIOD_OUT_HIGH) —
	 * 출력이며 얻는 순간 리셋이 걸린 상태다.
	 * 읽는 자: host_enable 이 풀고 host_disable 이 건다.
	 * 값 범위: 유효한 GPIO 서술자 또는 NULL(선택 사항).
	 * 동기화: probe 후 불변. */
	struct gpio_desc *reset_gpio;
	/* [한국어] PCIe 전원 공급.
	 * 설정자: probe 의 devm_regulator_get_optional("vpcie").
	 * 읽는 자: host_enable 이 켜고 host_disable 이 끈다.
	 * 값 범위: 유효한 regulator 포인터 또는 NULL(선택 사항).
	 * 동기화: probe 후 불변. */
	struct regulator *vpcie;
};

/* [한국어]
 * histb_pcie_readl - SoC 제어 창의 레지스터를 읽는다
 *
 * @histb_pcie: 드라이버 상태.
 * @reg: 제어 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * DWC 의 DBI 창이 아니라 **"control" 이라는 별도 창** 을 읽는다. 모드 설정,
 * LTSSM, 링크 상태, 그리고 DBI 사이드밴드 비트가 모두 이 창에 있다.
 *
 * _relaxed 판이 아니라 일반 readl 이다. 이 파일의 사이드밴드 켜기-접근-끄기
 * 삼단이 순서를 지켜야 하는데, 일반 판이 그 순서를 보장해 준다.
 *
 * 실행 컨텍스트: config 접근 경로와 probe. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   histb_pcie_dbi_r_mode()/w_mode() / link_up() / start_link() / host_init()
 *     → [이 함수] → readl()
 */
static u32 histb_pcie_readl(struct histb_pcie *histb_pcie, u32 reg)
{
	return readl(histb_pcie->ctrl + reg);
}

/* [한국어]
 * histb_pcie_writel - SoC 제어 창의 레지스터에 쓴다
 *
 * @histb_pcie: 드라이버 상태.
 * @reg: 제어 창 안의 오프셋.
 * @val: 쓸 값.
 *
 * histb_pcie_readl() 의 짝이다.
 *
 * 인자 순서가 커널의 writel(값, 주소) 관용과 반대로 오프셋이 앞에 온다.
 * 이 파일 안에서는 일관되지만 다른 파일에서 온 독자가 헷갈리기 쉽다.
 *
 * 실행 컨텍스트: config 접근 경로와 probe. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   histb_pcie_dbi_r_mode()/w_mode() / start_link() / host_init()
 *     → [이 함수] → writel()
 */
static void histb_pcie_writel(struct histb_pcie *histb_pcie, u32 reg, u32 val)
{
	writel(val, histb_pcie->ctrl + reg);
}

/* [한국어]
 * histb_pcie_dbi_w_mode - DBI **쓰기** 사이드밴드를 켜거나 끈다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @enable: true 면 켠다.
 *
 * 이 하드웨어는 DBI 창에 그냥 쓸 수 없다. 제어 창의 사이드밴드 비트를
 * 먼저 켜야 그 쓰기가 컨트롤러에 닿는다.
 *
 * **쓰기용 비트가 PCIE_SYS_CTRL0 에 있다** — 아래 읽기용은 PCIE_SYS_CTRL1
 * 이라, 같은 비트 이름(ELBI_SLV_DBI_ENABLE)이 두 레지스터에 각각 존재한다.
 * 읽기와 쓰기를 따로 열 수 있게 한 설계로 보이나, 그 이유는 이 트리에서
 * 확인 못 함.
 *
 * 읽기-수정-쓰기가 필수다. 같은 레지스터에 장치 종류 필드가 있어
 * 통째로 쓰면 RC 설정이 지워진다.
 *
 * 실행 컨텍스트: DBI 쓰기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   histb_pcie_write_dbi() → [이 함수]
 *     → histb_pcie_readl() → histb_pcie_writel()
 */
static void histb_pcie_dbi_w_mode(struct dw_pcie_rp *pp, bool enable)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct histb_pcie *hipcie = to_histb_pcie(pci);
	/* [한국어] 읽기-수정-쓰기 할 값. */
	u32 val;
/* [한국어] 쓰기용 사이드밴드는 CTRL0 에 있다. */

	val = histb_pcie_readl(hipcie, PCIE_SYS_CTRL0);
	/* [한국어] 켜라는 요청이면, */
	if (enable)
		/* [한국어] 비트를 세우고, */
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 끄라는 요청이면 — */
	else
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 되쓴다. 같은 레지스터에 장치 종류 필드가 있어 통째로 쓰면 RC 설정이 지워진다. */
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL0, val);
}

/* [한국어]
 * histb_pcie_dbi_r_mode - DBI **읽기** 사이드밴드를 켜거나 끈다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @enable: true 면 켠다.
 *
 * histb_pcie_dbi_w_mode() 의 읽기 판이며, 건드리는 레지스터가
 * PCIE_SYS_CTRL1 로 다르다는 것만 차이다.
 *
 * 실행 컨텍스트: DBI 읽기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   histb_pcie_read_dbi() → [이 함수]
 *     → histb_pcie_readl() → histb_pcie_writel()
 */
static void histb_pcie_dbi_r_mode(struct dw_pcie_rp *pp, bool enable)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct histb_pcie *hipcie = to_histb_pcie(pci);
	/* [한국어] 읽기-수정-쓰기 할 값. */
	u32 val;
/* [한국어] **읽기용 사이드밴드는 CTRL1 에 있다** — 쓰기용과 다른 레지스터다. */

	val = histb_pcie_readl(hipcie, PCIE_SYS_CTRL1);
	/* [한국어] 켜라는 요청이면, */
	if (enable)
		/* [한국어] 같은 이름의 비트를 세우고, */
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 끄라는 요청이면 — */
	else
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 되쓴다. */
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL1, val);
}

/* [한국어]
 * histb_pcie_read_dbi - 사이드밴드를 켜고 DBI 를 읽은 뒤 끈다
 *
 * @pci: DWC 코어의 문맥.
 * @base: 읽을 창의 기준 주소.
 * @reg: 그 안의 오프셋.
 * @size: 읽을 폭.
 * @return: 읽은 값.
 *
 * DWC 코어의 read_dbi 콜백이며, **코어의 모든 DBI 읽기가 이 함수를 지난다.**
 * 그래서 코어 쪽 코드는 사이드밴드의 존재를 전혀 모른 채 동작할 수 있다.
 *
 * 한 번의 논리적 읽기가 **MMIO 세 번** 이 된다 — 켜기, 읽기, 끄기.
 * 그만큼 느리지만 이 하드웨어에서는 다른 방법이 없다.
 *
 * [상류 코드 관찰] 켜기와 끄기 사이에 잠금이 없다. 그 구간에 다른 스레드가
 * 같은 사이드밴드 비트를 끄면 이 읽기가 실패한다. 이 파일의 DBI 접근이
 * 모두 PCI 코어의 잠금 아래에서 일어난다는 전제로 보이나, 그 근거는
 * 코드에 없다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * dw_pcie_read() 의 반환값을 확인하지 않는다 — 실패해도 val 이 그대로
 * 나가며, 그 경우 호출자는 쓰레기를 읽는다.
 *
 * 실행 컨텍스트: DBI 읽기 경로. PCI 코어가 잠금을 쥔 채 부를 수 있어
 * 잠들지 않는다.
 *
 * 에러 경로: 없다. 아래 읽기의 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.read_dbi == [이 함수]
 *     → histb_pcie_dbi_r_mode(true) → dw_pcie_read() → _r_mode(false)
 */
static u32 histb_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
			       u32 reg, size_t size)
{
	u32 val;

	histb_pcie_dbi_r_mode(&pci->pp, true);
	/* [한국어] 이제 실제 읽기가 통한다. 반환값을 확인하지 않아, 실패하면
	 * val 이 초기화되지 않은 채 나간다. */
	dw_pcie_read(base + reg, size, &val);
	/* [한국어] 곧바로 사이드밴드를 끈다 — 켠 채로 두면 다른 접근에 영향을 준다. */
	histb_pcie_dbi_r_mode(&pci->pp, false);
/* [한국어] 한 번의 논리적 읽기가 MMIO 세 번으로 끝났다. */

	return val;
}

/* [한국어]
 * histb_pcie_write_dbi - 사이드밴드를 켜고 DBI 에 쓴 뒤 끈다
 *
 * @pci: DWC 코어의 문맥.
 * @base: 쓸 창의 기준 주소.
 * @reg: 그 안의 오프셋.
 * @size: 쓸 폭.
 * @val: 쓸 값.
 *
 * histb_pcie_read_dbi() 의 쓰기 판이며, 켜는 사이드밴드가 쓰기용이라는
 * 것만 다르다.
 *
 * 읽기 쪽과 같은 잠금 부재가 여기에도 해당한다.
 *
 * 실행 컨텍스트: DBI 쓰기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법이 애초에 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.write_dbi == [이 함수]
 *     → histb_pcie_dbi_w_mode(true) → dw_pcie_write() → _w_mode(false)
 */
static void histb_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				 u32 reg, size_t size, u32 val)
{
	histb_pcie_dbi_w_mode(&pci->pp, true);
	dw_pcie_write(base + reg, size, val);
	/* [한국어] 쓰기 뒤에도 곧바로 사이드밴드를 끈다. */
	histb_pcie_dbi_w_mode(&pci->pp, false);
}

/* [한국어]
 * histb_pcie_rd_own_conf - 루트 포트 자신의 config 공간을 읽는다
 *
 * @bus: 루트 버스.
 * @devfn: 장치·기능 번호.
 * @where: 레지스터 오프셋.
 * @size: 읽을 폭.
 * @val: 결과를 담을 자리.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 루트 버스의 config 접근을 DBI 창으로 돌린다. 루트 포트 자신의 config
 * 공간이 그 창에 있기 때문이다.
 *
 * **슬롯 0 만 받아들인다.** 루트 포트가 하나뿐이라 그 자리에만 장치가
 * 있고, 나머지는 "없음" 으로 답해야 코어가 헛되이 탐색하지 않는다.
 *
 * 기능 번호는 확인하지 않는다 — 다중 기능 루트 포트를 상정하지 않는 것으로
 * 보이며, 그 경우 같은 값이 모든 기능에서 읽힌다.
 *
 * dw_pcie_read_dbi() 를 거치므로 결국 위 사이드밴드 삼단을 지난다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 슬롯 0 이 아니면 장치 없음으로 답한다.
 *
 * 호출 체인:
 *   PCI 코어의 config 읽기 → pci_ops.read == [이 함수]
 *     → dw_pcie_read_dbi() → histb_pcie_read_dbi()
 */
static int histb_pcie_rd_own_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		/* [한국어] 슬롯 0 이 아니면 장치 없음으로 답한다 — 루트 포트가 하나뿐이라
		 * 그 자리에만 장치가 있다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	*val = dw_pcie_read_dbi(pci, where, size);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * histb_pcie_wr_own_conf - 루트 포트 자신의 config 공간에 쓴다
 *
 * @bus: 루트 버스.
 * @devfn: 장치·기능 번호.
 * @where: 레지스터 오프셋.
 * @size: 쓸 폭.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * histb_pcie_rd_own_conf() 의 짝이며 규약이 같다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 슬롯 0 이 아니면 장치 없음으로 답한다.
 *
 * 호출 체인:
 *   PCI 코어의 config 쓰기 → pci_ops.write == [이 함수]
 *     → dw_pcie_write_dbi() → histb_pcie_write_dbi()
 */
static int histb_pcie_wr_own_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		/* [한국어] 쓰기 쪽도 같은 규약이다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	dw_pcie_write_dbi(pci, where, size, val);
	/* [한국어] 쓰기는 반환값이 없어 언제나 성공으로 답한다. */
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops histb_pci_ops = {
	/* [한국어] 루트 버스 읽기를 DBI 로 돌린다. */
	.read = histb_pcie_rd_own_conf,
	/* [한국어] 쓰기도 마찬가지다. 하위 버스는 DWC 코어의 기본 경로를 그대로 쓴다. */
	.write = histb_pcie_wr_own_conf,
};

/* [한국어]
 * histb_pcie_link_up - 링크가 서 있는지 답한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: true = 링크가 섰다, false = 아니다.
 *
 * **세 조건을 모두** 확인하는 것이 이 함수의 특징이다. 대부분의 글루가
 * 링크 비트 둘만 보는 데 비해, 여기서는 LTSSM 상태 기계가 실제로
 * ACTIVE(L0) 상태인지까지 확인한다.
 *
 * 그래서 레지스터를 둘 읽는다 — 링크 비트는 PCIE_SYS_STAT0 에,
 * LTSSM 상태는 PCIE_SYS_STAT4 에 있다.
 *
 * LTSSM 상태는 마스크로 하위 6비트를 뽑아 **동등 비교** 한다. 값 하나만
 * 정확히 맞아야 하므로, 비트 마스크 방식으로 판정하는 다른 드라이버들과
 * 다르다.
 *
 * 실행 컨텍스트: DWC 코어의 링크 대기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.link_up == [이 함수] → histb_pcie_readl()
 */
static bool histb_pcie_link_up(struct dw_pcie *pci)
{
	struct histb_pcie *hipcie = to_histb_pcie(pci);
	u32 regval;
	/* [한국어] LTSSM 상태를 담을 자리. */
	u32 status;
/* [한국어] 먼저 링크 비트를 읽는다. */

	regval = histb_pcie_readl(hipcie, PCIE_SYS_STAT0);
	/* [한국어] **다른 레지스터** 에서 LTSSM 상태를 읽는다. */
	status = histb_pcie_readl(hipcie, PCIE_SYS_STAT4);
	/* [한국어] 하위 6비트만 남긴다. */
	status &= PCIE_LTSSM_STATE_MASK;
	/* [한국어] 물리 계층과 데이터 링크 계층 두 비트가 모두 서 있고, */
	return ((regval & PCIE_XMLH_LINK_UP) && (regval & PCIE_RDLH_LINK_UP) &&
		/* [한국어] **LTSSM 이 정확히 ACTIVE 여야** 참이다 — 비트 마스크가 아니라 동등 비교다. */
		(status == PCIE_LTSSM_STATE_ACTIVE));
}

/* [한국어]
 * histb_pcie_start_link - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * 제어 창의 LTSSM 비트 하나를 세운다.
 *
 * **끄는 짝이 없다.** dw_pcie_ops 에 stop_link 를 두지 않아, 한 번 켠
 * LTSSM 을 이 드라이버가 끌 방법이 없다. remove 경로는 대신
 * histb_pcie_host_disable() 로 클럭과 리셋을 통째로 내린다.
 *
 * 읽기-수정-쓰기가 필수다. 같은 레지스터의 다른 비트를 보존해야 한다.
 *
 * 실행 컨텍스트: DWC 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수]
 *     → histb_pcie_readl() → histb_pcie_writel()
 */
static int histb_pcie_start_link(struct dw_pcie *pci)
{
	struct histb_pcie *hipcie = to_histb_pcie(pci);
	u32 regval;
/* [한국어] 제어 레지스터 7 을 읽는다. */

	/* assert LTSSM enable */
	regval = histb_pcie_readl(hipcie, PCIE_SYS_CTRL7);
	regval |= PCIE_APP_LTSSM_ENABLE;
	/* [한국어] LTSSM 비트를 세워 되쓴다. **끄는 짝이 없다** — 이 표에 stop_link 를
	 * 두지 않아, 한 번 켠 것을 이 드라이버가 끌 수 없다. */
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL7, regval);
/* [한국어] 링크 훈련이 시작된다. */

	return 0;
}

/* [한국어]
 * histb_pcie_host_init - 루트 버스 ops 를 갈아 끼우고 RC 모드를 지정한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 언제나 0.
 *
 * 이 파일이 DWC 코어와 만나는 유일한 콜백이다.
 *
 * 두 가지를 한다.
 * 1. **루트 버스용 pci_ops 를 갈아 끼운다.** 그래야 루트 포트 자신의
 *    config 접근이 이 파일의 DBI 경로로 온다. 하위 버스는 코어의 기본
 *    경로를 그대로 쓴다.
 * 2. 장치 종류 필드를 RC 로 지정한다. 마스크로 지운 뒤 값을 넣는
 *    읽기-수정-쓰기이며, 이 SoC 가 EP 나 legacy 모드로도 동작할 수 있어
 *    명시가 필요하다.
 *
 * 하드웨어 초기화는 이미 probe 의 histb_pcie_host_enable() 에서 끝나 있어,
 * 코어가 물어볼 시점에는 이 둘만 남는다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [이 함수]
 *     → histb_pcie_readl() → histb_pcie_writel()
 */
static int histb_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct histb_pcie *hipcie = to_histb_pcie(pci);
	/* [한국어] 읽기-수정-쓰기 할 값. */
	u32 regval;
/* [한국어] 먼저 루트 버스 접근 함수를 갈아 끼운다. */

	pp->bridge->ops = &histb_pci_ops;
/* [한국어] **루트 버스만 바꾼다** — 하위 버스는 DWC 코어의 기본 경로가 맞다. */

	/* PCIe RC work mode */
	regval = histb_pcie_readl(hipcie, PCIE_SYS_CTRL0);
	regval &= ~PCIE_DEVICE_TYPE_MASK;
	/* [한국어] RC 값을 그 자리에 넣는다. */
	regval |= PCIE_WM_RC;
	/* [한국어] 되쓴다. 이 SoC 는 EP 나 legacy 모드로도 동작할 수 있어 명시가 필요하다. */
	histb_pcie_writel(hipcie, PCIE_SYS_CTRL0, regval);
/* [한국어] SoC 고유의 설정이 이 둘로 끝난다. */

	return 0;
}

static const struct dw_pcie_host_ops histb_pcie_host_ops = {
	/* [한국어] 하드웨어 초기화가 probe 에서 끝나 있어, 코어가 물어볼 것은 이 둘뿐이다. */
	.init = histb_pcie_host_init,
};

/* [한국어]
 * histb_pcie_host_disable - 리셋을 걸고 클럭·GPIO·전원을 차례로 내린다
 *
 * @hipcie: 드라이버 상태.
 *
 * histb_pcie_host_enable() 의 짝이며 순서가 역순이다.
 *
 * 네 단계다 — 리셋 셋을 걸고, 클럭 넷을 끄고, GPIO 로 하위 장치를 리셋에
 * 넣고, 전원을 끊는다.
 *
 * **리셋을 가장 먼저 거는 것** 이 요점이다. 클럭이 아직 살아 있어야 리셋
 * 신호가 로직에 전달되기 때문이다.
 *
 * GPIO 와 전원은 있을 때만 다룬다 — 둘 다 선택 사항이라 없는 보드가 있다.
 *
 * 클럭을 끄는 순서가 켜는 쪽의 역순이 아니다. 켤 때는 bus → sys → pipe →
 * aux 인데 끌 때는 aux → pipe → sys → bus 로, 실제로는 정확한 역순이다.
 *
 * 반환값이 없다. 정리 경로라 실패해도 할 수 있는 일이 없다.
 *
 * 실행 컨텍스트: remove. gpiod_set_value_cansleep 이 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   histb_pcie_remove() → [이 함수]
 *     → reset_control_assert() → clk_disable_unprepare()
 *     → gpiod_set_value_cansleep() → regulator_disable()
 */
static void histb_pcie_host_disable(struct histb_pcie *hipcie)
{
	reset_control_assert(hipcie->soft_reset);
	reset_control_assert(hipcie->sys_reset);
	reset_control_assert(hipcie->bus_reset);

	clk_disable_unprepare(hipcie->aux_clk);
	clk_disable_unprepare(hipcie->pipe_clk);
	clk_disable_unprepare(hipcie->sys_clk);
	clk_disable_unprepare(hipcie->bus_clk);

	if (hipcie->reset_gpio)
		/* [한국어] 하위 장치를 리셋에 넣는다 — 클럭을 끈 뒤라 이 신호만으로 붙잡는다. */
		gpiod_set_value_cansleep(hipcie->reset_gpio, 1);
/* [한국어] GPIO 가 없는 보드에서는 건너뛴다. */

	if (hipcie->vpcie)
		/* [한국어] 마지막으로 전원을 끊는다. */
		regulator_disable(hipcie->vpcie);
}

/* [한국어]
 * histb_pcie_host_enable - 전원·GPIO·클럭 넷·리셋 셋을 순서대로 올린다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, 음수 오류.
 *
 * 하드웨어를 동작 상태로 만드는 전 과정이며, 순서가 전부다.
 *
 * 네 단계다.
 * 1. 전원을 켠다 — 선택 사항이라 없으면 건너뛴다.
 * 2. GPIO 리셋을 푼다 — 역시 선택 사항이다.
 * 3. 클럭 넷을 bus → sys → pipe → aux 순으로 켠다.
 * 4. 리셋 셋을 각각 **걸었다 푼다** — soft, sys, bus 순이다.
 *
 * 4번이 단순한 해제가 아니라 토글인 것이 눈에 띈다. 부트로더가 남긴
 * 상태를 확실히 지우려는 것으로 보이며, 그 근거는 이 트리에서 확인 못 함.
 *
 * 되감기가 계단이며 네 라벨이 있다. 각 진입점이 그 지점까지 켠 것만
 * 정확히 되돌린다 — 다만 **GPIO 리셋은 어느 경로에서도 되돌리지 않는다.**
 *
 * 인자로 pp 를 받는 것이 눈에 띈다. 짝인 disable 은 hipcie 를 직접 받는데,
 * 이쪽만 DWC 문맥을 거쳐 변환한다.
 *
 * 실행 컨텍스트: probe. 전원과 GPIO 조작이 잠들 수 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 각 단계의 실패를 어느 클럭이었는지와 함께 기록하고
 * goto 로 되감는다.
 *
 * 호출 체인:
 *   histb_pcie_probe() → [이 함수]
 *     → regulator_enable() → gpiod_set_value_cansleep()
 *     → clk_prepare_enable() → reset_control_assert()/deassert()
 */
static int histb_pcie_host_enable(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct histb_pcie *hipcie = to_histb_pcie(pci);
	/* [한국어] 로그에 쓸 device. */
	struct device *dev = pci->dev;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 전원을 켠다. */

	/* power on PCIe device if have */
	if (hipcie->vpcie) {
		ret = regulator_enable(hipcie->vpcie);
		/* [한국어] 켜지 못하면, */
		if (ret) {
			/* [한국어] 그 사실을 남기고, */
			dev_err(dev, "failed to enable regulator: %d\n", ret);
			/* [한국어] 물러난다. 아직 되돌릴 것이 없다. */
			return ret;
		}
	}

	if (hipcie->reset_gpio)
		/* [한국어] GPIO 리셋을 푼다 — 전원이 들어온 뒤라야 의미가 있다. */
		gpiod_set_value_cansleep(hipcie->reset_gpio, 0);
/* [한국어] 이제 클럭 넷을 순서대로 켠다. */

	ret = clk_prepare_enable(hipcie->bus_clk);
	/* [한국어] 버스 클럭을 켜지 못하면, */
	if (ret) {
		/* [한국어] 어느 클럭이었는지 남기고, */
		dev_err(dev, "cannot prepare/enable bus clk\n");
		/* [한국어] 전원만 되돌리는 자리로 뛴다. */
		goto err_bus_clk;
	}

	ret = clk_prepare_enable(hipcie->sys_clk);
	/* [한국어] 시스템 클럭을 켜지 못하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "cannot prepare/enable sys clk\n");
		/* [한국어] 버스 클럭부터 되돌린다. */
		goto err_sys_clk;
	}

	ret = clk_prepare_enable(hipcie->pipe_clk);
	/* [한국어] PIPE 클럭을 켜지 못하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "cannot prepare/enable pipe clk\n");
		/* [한국어] 시스템 클럭부터 되돌린다. */
		goto err_pipe_clk;
	}

	ret = clk_prepare_enable(hipcie->aux_clk);
	/* [한국어] 보조 클럭을 켜지 못하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "cannot prepare/enable aux clk\n");
		/* [한국어] PIPE 클럭부터 되돌린다. */
		goto err_aux_clk;
	}

	reset_control_assert(hipcie->soft_reset);
	reset_control_deassert(hipcie->soft_reset);

	reset_control_assert(hipcie->sys_reset);
	reset_control_deassert(hipcie->sys_reset);

	reset_control_assert(hipcie->bus_reset);
	reset_control_deassert(hipcie->bus_reset);

	return 0;

err_aux_clk:
	clk_disable_unprepare(hipcie->pipe_clk);
err_pipe_clk:
	clk_disable_unprepare(hipcie->sys_clk);
err_sys_clk:
	clk_disable_unprepare(hipcie->bus_clk);
err_bus_clk:
	if (hipcie->vpcie)
		regulator_disable(hipcie->vpcie);

	return ret;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] **read_dbi/write_dbi 를 채우는 글루는 드물다** — 대부분은 코어의
	 * 기본 접근으로 충분한데, 이 하드웨어는 사이드밴드가 필요해 가로챈다. */
	.read_dbi = histb_pcie_read_dbi,
	/* [한국어] 쓰기 쪽도 같은 이유로 가로챈다. */
	.write_dbi = histb_pcie_write_dbi,
	.link_up = histb_pcie_link_up,
	.start_link = histb_pcie_start_link,
};

/* [한국어]
 * histb_pcie_probe - 자원을 모두 얻고 하드웨어를 깨운 뒤 DWC 호스트를 초기화한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며, 이 파일에서 가장 긴 함수다. 얻는 자원이
 * 열두 가지라 대부분이 그 확보 코드다.
 *
 * **dw_pcie 를 따로 할당한다** — 이 드라이버가 그것을 포인터로 들기
 * 때문이며, 그래서 to_histb_pcie() 매크로도 drvdata 를 거친다.
 *
 * 창이 둘이고 이름이 다르다 — "control"(SoC 제어)과 "rc-dbi"(DWC DBI).
 * 전자는 이 파일이, 후자는 DWC 코어가 주로 쓴다.
 *
 * 선택 사항 처리가 셋 다 다르다.
 * - vpcie: -ENODEV 만 걸러 NULL 로 두고, 그 밖의 오류는 실패다.
 * - reset GPIO: optional 판이라 없으면 NULL 이고, 오류만 실패다.
 * - PHY: **오류를 실패로 다루지 않는다** — 못 얻으면 정보 로그를 남기고
 *   NULL 로 두어 계속 진행한다. 세 자원의 방어 수준이 제각각이다.
 *
 * [상류 코드 관찰] `phy_init(hipcie->phy)` 의 반환값을 확인하지 않는다.
 * PHY 초기화가 실패해도 그대로 진행하며, 이후 링크가 서지 않는 것으로만
 * 나타난다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 되감기가 한 갈래뿐이다 — PHY 만 되돌리고, histb_pcie_host_enable() 이
 * 켠 전원·클럭·리셋은 그 함수 자신의 되감기에 맡긴다. 다만
 * dw_pcie_host_init() 이 실패하는 경로에서는 그것들이 켜진 채로 남는다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 링크 대기와
 * 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 각 자원 확보의 실패를 어느 것이었는지와 함께 기록하고
 * 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_platform_ioremap_resource_byname() → devm_regulator_get_optional()
 *     → devm_gpiod_get_optional() → devm_clk_get() → devm_reset_control_get()
 *     → devm_phy_get() → histb_pcie_host_enable() → dw_pcie_host_init()
 */
static int histb_pcie_probe(struct platform_device *pdev)
{
	struct histb_pcie *hipcie;
	struct dw_pcie *pci;
	/* [한국어] 루트 포트 문맥. 아래에서 여러 번 쓰므로 미리 잡아 둔다. */
	struct dw_pcie_rp *pp;
	/* [한국어] 로그와 자원 조회에 쓸 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 상태 구조를 잡는다. */

	hipcie = devm_kzalloc(dev, sizeof(*hipcie), GFP_KERNEL);
	/* [한국어] 잡지 못하면, */
	if (!hipcie)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] dw_pcie 를 따로 잡지 못하면, */
	if (!pci)
		/* [한국어] 역시 메모리 부족이다. */
		return -ENOMEM;

	hipcie->pci = pci;
	/* [한국어] 루트 포트 문맥을 가리켜 둔다. */
	pp = &pci->pp;
	/* [한국어] DWC 코어가 이 값으로 디바이스 트리와 로그를 다룬다. */
	pci->dev = dev;
	/* [한국어] 코어 콜백 표를 건다 — DBI 접근 둘까지 포함한 네 개다. */
	pci->ops = &dw_pcie_ops;
/* [한국어] 이제 자원을 얻는다. */

	hipcie->ctrl = devm_platform_ioremap_resource_byname(pdev, "control");
	/* [한국어] SoC 제어 창 매핑이 실패하면, */
	if (IS_ERR(hipcie->ctrl)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "cannot get control reg base\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->ctrl);
	/* [한국어] 제어 창 처리 끝. */
	}

	pci->dbi_base = devm_platform_ioremap_resource_byname(pdev, "rc-dbi");
	/* [한국어] DBI 창 매핑이 실패하면, */
	if (IS_ERR(pci->dbi_base)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "cannot get rc-dbi base\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pci->dbi_base);
	/* [한국어] 창 둘을 모두 얻었다. */
	}

	hipcie->vpcie = devm_regulator_get_optional(dev, "vpcie");
	/* [한국어] 전원 조회가 실패했으면, */
	if (IS_ERR(hipcie->vpcie)) {
		/* [한국어] -ENODEV 가 아닌 오류는 — 즉 속성은 있는데 잘못된 경우는, */
		if (PTR_ERR(hipcie->vpcie) != -ENODEV)
			/* [한국어] 실패로 올려보낸다. */
			return PTR_ERR(hipcie->vpcie);
		/* [한국어] -ENODEV 였다면 이 보드에 그 전원이 없다는 뜻이므로 NULL 로 둔다. */
		hipcie->vpcie = NULL;
	/* [한국어] 전원 처리 끝. */
	}

	hipcie->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     /* [한국어] 출력으로, 초기값을 HIGH 로 얻는다 — 얻는 순간 하위 장치가 리셋 상태다. */
						     GPIOD_OUT_HIGH);
	ret = PTR_ERR_OR_ZERO(hipcie->reset_gpio);
	/* [한국어] 오류면 — optional 판이라 없는 것은 오류가 아니다. */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "unable to request reset gpio: %d\n", ret);
		/* [한국어] 물러난다. */
		return ret;
	}

	ret = gpiod_set_consumer_name(hipcie->reset_gpio,
				      /* [한국어] GPIO 에 사람이 읽을 이름을 붙인다 — /sys 에서 이 핀의 용도를 알 수 있게 한다. */
				      "PCIe device power control");
	if (ret) {
		/* [한국어] 이름 설정이 실패하면 그 사실을 남기고, */
		dev_err(dev, "unable to set reset gpio name: %d\n", ret);
		/* [한국어] 물러난다. 이름 실패로 probe 를 접는 것이 엄격해 보이지만 상류가 그렇게 되어 있다. */
		return ret;
	}

	hipcie->aux_clk = devm_clk_get(dev, "aux");
	/* [한국어] 보조 클럭을 얻지 못하면, */
	if (IS_ERR(hipcie->aux_clk)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to get PCIe aux clk\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->aux_clk);
	/* [한국어] 보조 클럭 처리 끝. */
	}

	hipcie->pipe_clk = devm_clk_get(dev, "pipe");
	/* [한국어] PIPE 클럭을 얻지 못하면, */
	if (IS_ERR(hipcie->pipe_clk)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to get PCIe pipe clk\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->pipe_clk);
	/* [한국어] PIPE 클럭 처리 끝. */
	}

	hipcie->sys_clk = devm_clk_get(dev, "sys");
	/* [한국어] 시스템 클럭을 얻지 못하면, */
	if (IS_ERR(hipcie->sys_clk)) {
		/* [한국어] 그 사실을 남기고(문구에 오타가 있으나 상류 그대로다), */
		dev_err(dev, "Failed to get PCIEe sys clk\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->sys_clk);
	/* [한국어] 시스템 클럭 처리 끝. */
	}

	hipcie->bus_clk = devm_clk_get(dev, "bus");
	/* [한국어] 버스 클럭을 얻지 못하면, */
	if (IS_ERR(hipcie->bus_clk)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to get PCIe bus clk\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->bus_clk);
	/* [한국어] 클럭 넷을 모두 얻었다. */
	}

	hipcie->soft_reset = devm_reset_control_get(dev, "soft");
	/* [한국어] 소프트 리셋을 얻지 못하면, */
	if (IS_ERR(hipcie->soft_reset)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "couldn't get soft reset\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->soft_reset);
	/* [한국어] 소프트 리셋 처리 끝. */
	}

	hipcie->sys_reset = devm_reset_control_get(dev, "sys");
	/* [한국어] 시스템 리셋을 얻지 못하면, */
	if (IS_ERR(hipcie->sys_reset)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "couldn't get sys reset\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->sys_reset);
	/* [한국어] 시스템 리셋 처리 끝. */
	}

	hipcie->bus_reset = devm_reset_control_get(dev, "bus");
	/* [한국어] 버스 리셋을 얻지 못하면, */
	if (IS_ERR(hipcie->bus_reset)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "couldn't get bus reset\n");
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(hipcie->bus_reset);
	/* [한국어] 리셋 셋을 모두 얻었다. */
	}

	hipcie->phy = devm_phy_get(dev, "phy");
	/* [한국어] PHY 조회가 실패했으면 — **여기만 실패로 다루지 않는다.** */
	if (IS_ERR(hipcie->phy)) {
		/* [한국어] 정보 수준으로만 남기고, */
		dev_info(dev, "no pcie-phy found\n");
		/* [한국어] NULL 로 두어 계속 진행한다. 전원·GPIO·PHY 세 선택 자원의
		 * 방어 수준이 제각각인 셈이다. */
		hipcie->phy = NULL;
		/* fall through here!
		 * if no pcie-phy found, phy init
		 * should be done under boot!
		 */
	} else {
		phy_init(hipcie->phy);
	}

	pp->ops = &histb_pcie_host_ops;
/* [한국어] 호스트 콜백 표를 건다. */

	platform_set_drvdata(pdev, hipcie);
/* [한국어] 이 파일의 변환 매크로가 drvdata 를 거치므로 아래 호출보다 먼저 매달아야 한다. */

	ret = histb_pcie_host_enable(pp);
	/* [한국어] 하드웨어를 깨우지 못하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "failed to enable host\n");
		/* [한국어] PHY 를 되돌리는 자리로 뛴다. */
		goto err_exit_phy;
	}

	ret = dw_pcie_host_init(pp);
	/* [한국어] DWC 호스트 초기화가 실패하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "failed to initialize host\n");
		/* [한국어] **PHY 만 되돌린다** — host_enable 이 켠 전원·클럭·리셋은 켜진 채로 남는다. */
		goto err_exit_phy;
	}

	return 0;

err_exit_phy:
	phy_exit(hipcie->phy);

	return ret;
}

/* [한국어]
 * histb_pcie_remove - 하드웨어를 내리고 PHY 를 해제한다
 *
 * @pdev: 플랫폼 장치.
 *
 * probe 의 역순이지만 짧다.
 *
 * DWC 호스트를 내리지 않는 것이 눈에 띈다 — `dw_pcie_host_deinit()` 을
 * 부르지 않으므로, 버스와 그 아래 장치들이 어떻게 정리되는지는 이 파일에
 * 드러나지 않는다.
 *
 * phy_exit() 을 조건 없이 부른다. PHY 가 NULL 일 수 있는데 그 함수가
 * NULL 을 어떻게 다루는지는 이 트리에 drivers/phy 가 없어 확인 못 함 —
 * probe 의 되감기도 같은 방식으로 부른다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → histb_pcie_host_disable() → phy_exit()
 */
static void histb_pcie_remove(struct platform_device *pdev)
{
	struct histb_pcie *hipcie = platform_get_drvdata(pdev);

	histb_pcie_host_disable(hipcie);

	phy_exit(hipcie->phy);
}

static const struct of_device_id histb_pcie_of_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다. */
	{ .compatible = "hisilicon,hi3798cv200-pcie", },
	/* [한국어] 표의 끝 표시. */
	{},
};
MODULE_DEVICE_TABLE(of, histb_pcie_of_match);

static struct platform_driver histb_pcie_platform_driver = {
	/* [한국어] probe 콜백. */
	.probe	= histb_pcie_probe,
	/* [한국어] remove 콜백. 이 드라이버는 뗄 수 있다. */
	.remove = histb_pcie_remove,
	.driver = {
		.name = "histb-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = histb_pcie_of_match,
	},
};
module_platform_driver(histb_pcie_platform_driver);

MODULE_DESCRIPTION("HiSilicon STB PCIe host controller driver");
