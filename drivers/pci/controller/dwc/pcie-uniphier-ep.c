// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe endpoint controller driver for UniPhier SoCs
 * Copyright 2018 Socionext Inc.
 * Author: Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 */

/*
 * [한국어 설명] Socionext UniPhier SoC 의 DesignWare PCIe 엔드포인트 글루 (pcie-uniphier-ep.c)
 *
 * === 파일의 역할 ===
 * UniPhier SoC 를 PCIe **엔드포인트** 로 동작시키는 드라이버다. 같은 SoC 의
 * 호스트 드라이버(pcie-uniphier.c)와 짝을 이루며, 두 파일이 같은 "link"
 * 레지스터 창을 서로 다른 모드로 쓴다 — PCL_MODE 의 REGVAL 을 지우면 RC,
 * 세우면 EP 다.
 *
 * 호스트 판과 가장 크게 다른 것이 **인터럽트의 방향** 이다. 호스트 판은
 * INTx 를 받아 도메인으로 갈라 보내지만, 여기서는 이 SoC 가 인터럽트를
 * **내는** 쪽이라 INTx 펄스를 만들고 MSI 요청 비트를 세운다. 그래서
 * 이 파일에는 인터럽트 도메인도 핸들러도 없다.
 *
 * 또 하나의 차이가 **SoC 세대별 분기** 다. Pro5 와 NX1 이 초기화 순서와
 * 필요한 클럭·리셋이 달라, 그 차이를 struct uniphier_pcie_ep_soc_data 라는
 * 표로 뽑아 두고 디바이스 트리의 compatible 이 어느 표를 쓸지 정한다.
 * 호스트 판에는 그런 표가 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> uniphier_pcie_ep_probe()
 *     -> 매칭된 SoC 표를 얻는다(Pro5 또는 NX1)
 *     -> "link" 창과, 표가 요구하는 클럭·리셋·PHY 를 얻는다
 *     -> uniphier_pcie_ep_enable()
 *        -> 클럭·리셋 -> 표의 init 콜백(EP 모드 설정) -> PHY -> 표의 wait 콜백
 *     -> dw_pcie_ep_init()  [pcie-designware-ep.c]
 *        -> EPC 를 등록하고, 그것이 EPF 드라이버와 짝지어진다
 *     -> dw_pcie_ep_init_registers() -> pci_epc_init_notify()
 *
 * 동작 중:
 *   EPF 드라이버가 호스트에게 알릴 일이 생기면 pci_epc_raise_irq()
 *     -> [이 파일] uniphier_pcie_ep_raise_irq()
 *        -> INTx 면 펄스를 만들고, MSI 면 벡터 번호를 써서 요청 비트를 세운다
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. INTx 발생 경로에 udelay 가
 * 있어 그 구간 동안 CPU 를 붙잡지만, 30µs 로 짧다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/endpoint/ 의 EPC/EPF 계층. 이 파일이 등록한 EPC 에
 *   기능 드라이버가 붙어 실제 동작을 정의한다.
 * 아래쪽: pcie-designware-ep.c 와 pcie-designware.c. 접점은 두 벌의 콜백
 *   표다 — dw_pcie_ops(start_link, stop_link)와
 *   dw_pcie_ep_ops(raise_irq, get_features).
 * 옆쪽: 같은 SoC 의 호스트 드라이버와 레지스터 정의를 **공유하지 않는다** —
 *   두 파일이 같은 이름의 매크로를 각자 따로 정의하고 있다.
 *
 * 데이터 흐름:
 *   디바이스 트리(compatible -> SoC 표, "link" 자원, 클럭·리셋, PHY) -> probe
 *     -> struct uniphier_pcie_ep_priv
 *   MSI: EPF 가 준 func_no 와 벡터 번호 -> PCL_APP_MSI0 의 두 필드
 *        -> PCL_APP_MSI1 의 요청 비트 -> 호스트로 나가는 MSI
 *   INTx: PCL_APP_INTX 의 비트를 세웠다 30µs 뒤 지운다 -> 펄스
 *
 * 공유 상태: struct uniphier_pcie_ep_priv 하나. probe 후 불변이며
 *   잠금이 없다 — INTx 발생의 세우기·지우기 한 쌍은 상위
 *   pci_epc_raise_irq() 의 뮤텍스가 지킨다(해당 자리의 상류 주석 참조).
 *
 * === NVMe 관점 ===
 * 이 드라이버는 NVMe 를 쓰는 쪽이 아니라 **NVMe 장치가 될 수도 있는 쪽**
 * 이다. EPF 계층에 NVMe 기능 드라이버를 붙이면 이 SoC 가 호스트에게 NVMe
 * 컨트롤러로 보이게 만들 수 있고, 그때 완료 알림이 이 파일의
 * uniphier_pcie_ep_raise_msi_irq() 를 지난다. 다만 이 트리의
 * endpoint/functions/ 에는 test, ntb, vntb, mhi 만 있고 NVMe 기능
 * 드라이버는 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * uniphier_pcie_ep_enable()        : 클럭·리셋·PHY 를 순서대로 켠다.
 *                                    세대별 차이는 표의 콜백 둘이 흡수한다.
 * uniphier_pcie_pro5_init_ep()     : Pro5 의 EP 모드 초기화.
 * uniphier_pcie_nx1_init_ep()      : NX1 의 EP 모드 초기화. PERST# 를 다룬다.
 * uniphier_pcie_ep_raise_intx_irq(): INTx 펄스를 만든다.
 * uniphier_pcie_ep_raise_msi_irq() : MSI 요청을 낸다.
 * struct uniphier_pcie_ep_priv     : 이 드라이버의 상태. **dw_pcie 가 맨 앞이
 *                                    아니라** 두 번째 필드다.
 * struct uniphier_pcie_ep_soc_data : 세대별 차이를 담은 표.
 */

/* [한국어] BIT()/GENMASK(). */
#include <linux/bitops.h>
/* [한국어] FIELD_PREP(). MSI 벡터 번호를 필드 자리로 옮기는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] clk_prepare_enable()/clk_disable_unprepare(). */
#include <linux/clk.h>
/* [한국어] udelay()/msleep()/usleep_range(). 세 종류의 대기가 모두 쓰인다. */
#include <linux/delay.h>
/* [한국어] __init 표시. */
#include <linux/init.h>
/* [한국어] readl_poll_timeout(). NX1 의 PIPE 클럭 대기가 쓴다. */
#include <linux/iopoll.h>
/* [한국어] of_device_get_match_data(). SoC 표를 고르는 통로다. */
#include <linux/of.h>
/* [한국어] PCI_IRQ_INTX/MSI 등 PCI 코어 타입들. */
#include <linux/pci.h>
/* [한국어] phy_init()/phy_exit(). */
#include <linux/phy/phy.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] reset_control_assert()/deassert(). */
#include <linux/reset.h>

/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_ep_ops,
 * DWC_EPC_COMMON_FEATURES 등. */
#include "pcie-designware.h"

/* Link Glue registers */
/* [한국어] 리셋 제어 레지스터 0(옆의 상류 주석대로 링크 글루 영역이다). */
#define PCL_RSTCTRL0			0x0010
/* [한국어] AXI 레지스터 경로 리셋. */
#define PCL_RSTCTRL_AXI_REG		BIT(3)
/* [한국어] AXI 슬레이브 경로 리셋. */
#define PCL_RSTCTRL_AXI_SLAVE		BIT(2)
/* [한국어] AXI 마스터 경로 리셋. */
#define PCL_RSTCTRL_AXI_MASTER		BIT(1)
/* [한국어] PIPE3 리셋. Pro5 판이 이 넷을 함께 푼다 — 세 AXI 경로가 모두 열려야
 * 호스트가 이 엔드포인트의 BAR 에 접근할 수 있다. */
#define PCL_RSTCTRL_PIPE3		BIT(0)

/* [한국어] 리셋 제어 레지스터 1. */
#define PCL_RSTCTRL1			0x0020
/* [한국어] PERST# 리셋 비트. **이 파일에서 읽는 곳은 없다**(전수 확인) —
 * PERST# 는 아래 PCL_PINCTRL0 로 다룬다. */
#define PCL_RSTCTRL_PERST		BIT(0)

/* [한국어] 리셋 제어 레지스터 2. */
#define PCL_RSTCTRL2			0x0024
/* [한국어] PHY 리셋 비트. uniphier_pcie_phy_reset() 이 이것을 다룬다. */
#define PCL_RSTCTRL_PHY_RESET		BIT(0)

/* [한국어] 핀 제어 레지스터. NX1 판의 PERST# 조작이 여기서 이뤄진다. */
#define PCL_PINCTRL0			0x002c
/* [한국어] PERST# 풀다운을 레지스터로 제어할지. */
#define PCL_PERST_PLDN_REGEN		BIT(12)
/* [한국어] PERST# 출력 허용을 레지스터로 제어할지. */
#define PCL_PERST_NOE_REGEN		BIT(11)
/* [한국어] PERST# 출력 값을 레지스터로 제어할지.
 * **REGEN 셋과 REGVAL 셋이 짝을 이룬다** — 호스트 판과 같은 짜임이다. */
#define PCL_PERST_OUT_REGEN		BIT(8)
/* [한국어] PERST# 풀다운 값. */
#define PCL_PERST_PLDN_REGVAL		BIT(4)
/* [한국어] PERST# 출력 허용 값. */
#define PCL_PERST_NOE_REGVAL		BIT(3)
/* [한국어] PERST# 출력 값. */
#define PCL_PERST_OUT_REGVAL		BIT(0)

/* [한국어] PIPE 모니터 레지스터. */
#define PCL_PIPEMON			0x0044
/* [한국어] PIPE 클럭이 살아 있는지. NX1 의 wait 콜백이 이 비트를 폴링한다. */
#define PCL_PCLK_ALIVE			BIT(15)

/* [한국어] 동작 모드 레지스터. */
#define PCL_MODE			0x8000
/* [한국어] 모드를 레지스터로 제어할지. */
#define PCL_MODE_REGEN			BIT(8)
/* [한국어] 모드 값. **세우면 EP** 다 — 호스트 판이 같은 비트를 지워 RC 를 고르는
 * 것과 정확히 반대이며, 두 파일이 같은 하드웨어를 나눠 쓰는 근거다. */
#define PCL_MODE_REGVAL			BIT(0)

/* [한국어] 애플리케이션 클럭 제어 레지스터. */
#define PCL_APP_CLK_CTRL		0x8004
/* [한국어] 클럭 요청 비트. Pro5 판이 이것을 끈다. */
#define PCL_APP_CLK_REQ			BIT(0)

/* [한국어] 애플리케이션 준비 제어 레지스터. */
#define PCL_APP_READY_CTRL		0x8008
/* [한국어] LTSSM 시작 비트. */
#define PCL_APP_LTSSM_ENABLE		BIT(0)

/* [한국어] 벤더 MSI 레지스터 0 — 어느 기능의 몇 번 벡터인지 지정한다. */
#define PCL_APP_MSI0			0x8040
/* [한국어] Traffic Class 필드. **기능 번호가 여기 들어간다** — 이름과 쓰임이
 * 어긋나 보이며, 그 필드가 실제로 무엇을 뜻하는지는 확인 못 함. */
#define PCL_APP_VEN_MSI_TC_MASK		GENMASK(10, 8)
/* [한국어] 벡터 번호 필드(5비트). 0부터 세므로 EPF 가 준 번호에서 1 을 뺀다. */
#define PCL_APP_VEN_MSI_VECTOR_MASK	GENMASK(4, 0)

#define PCL_APP_MSI1			0x8044
#define PCL_APP_MSI_REQ			BIT(0)

#define PCL_APP_INTX			0x8074
#define PCL_APP_INTX_SYS_INT		BIT(0)

#define PCL_APP_PM0			0x8078
#define PCL_SYS_AUX_PWR_DET		BIT(8)

/* assertion time of INTx in usec */
#define PCL_INTX_WIDTH_USEC		30

struct uniphier_pcie_ep_priv {
	/* [한국어] "link" 레지스터 창의 가상 주소.
	 * 설정자: probe 의 devm_platform_ioremap_resource_byname("link").
	 * 읽는 자: 이 파일의 모든 레지스터 접근.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변.
	 * **이 필드가 맨 앞이라 dw_pcie 가 구조체 시작이 아니다** — 그래서
	 * 변환 매크로가 container_of 가 아니라 drvdata 를 쓴다. */
	void __iomem *base;
	/* [한국어] DWC 코어가 다루는 부분.
	 * 설정자: probe 가 dev 와 ops 를 채우고, DWC 코어가 나머지를 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 모든 콜백.
	 * 값 범위: DWC 코어가 정의한 구조체.
	 * 동기화: 코어가 관리한다. */
	struct dw_pcie pci;
	/* [한국어] link 클럭과 gio 클럭.
	 * 설정자: probe. gio 는 has_gio 가 참일 때만 얻고, 거짓이면 NULL 로 남는다.
	 * 읽는 자: uniphier_pcie_ep_enable() 과 그 되감기.
	 * 값 범위: 유효한 clk 포인터 또는 NULL. **NULL 이면 clk 함수들이 무동작이 된다.**
	 * 동기화: probe 후 불변. */
	struct clk *clk, *clk_gio;
	/* [한국어] link 리셋과 gio 리셋. 위 클럭과 같은 규약이며 shared 판이라
	 * 다른 블록과 공유한다.
	 * 설정자: probe.
	 * 읽는 자: uniphier_pcie_ep_enable() 과 그 되감기.
	 * 값 범위: 유효한 리셋 컨트롤 또는 NULL.
	 * 동기화: probe 후 불변. */
	struct reset_control *rst, *rst_gio;
	/* [한국어] PCIe PHY. 선택 사항이라 없는 보드에서는 NULL 이 된다.
	 * 설정자: probe 의 devm_phy_optional_get().
	 * 읽는 자: uniphier_pcie_ep_enable() 과 그 되감기.
	 * 값 범위: 유효한 PHY 포인터 또는 NULL.
	 * 동기화: probe 후 불변. */
	struct phy *phy;
	/* [한국어] 이 SoC 세대의 표.
	 * 설정자: probe 의 of_device_get_match_data().
	 * 읽는 자: 자원 확보 조건, 초기화 콜백, 대기 콜백, 능력 서술 모두.
	 * 값 범위: uniphier_pro5_data 또는 uniphier_nx1_data.
	 * 동기화: probe 후 불변. */
	const struct uniphier_pcie_ep_soc_data *data;
/* [한국어] 이 드라이버의 상태 전부. */
};

struct uniphier_pcie_ep_soc_data {
	/* [한국어] gio 클럭·리셋이 필요한 세대인지.
	 * 설정자: 두 정적 표가 상수로 정한다.
	 * 읽는 자: probe 가 그 자원을 얻을지 판단한다.
	 * 값 범위: Pro5 는 true, NX1 은 false.
	 * 동기화: const 라 불변. */
	bool has_gio;
	/* [한국어] EP 모드 초기화 콜백.
	 * 설정자: 두 정적 표.
	 * 읽는 자: uniphier_pcie_ep_enable().
	 * 값 범위: 세대별 init 함수. 두 표 모두 채워져 있으나 호출부는 NULL 을 확인한다.
	 * 동기화: const 라 불변. */
	void (*init)(struct uniphier_pcie_ep_priv *priv);
	/* [한국어] 초기화 뒤 대기 콜백.
	 * 설정자: 두 정적 표.
	 * 읽는 자: uniphier_pcie_ep_enable().
	 * 값 범위: NX1 은 wait 함수, **Pro5 는 NULL** — 그 세대는 대기를 건너뛴다.
	 * 동기화: const 라 불변. */
	int (*wait)(struct uniphier_pcie_ep_priv *priv);
	/* [한국어] 이 세대의 엔드포인트 능력.
	 * 설정자: 두 정적 표.
	 * 읽는 자: uniphier_pcie_get_features() 가 그대로 돌려준다.
	 * 값 범위: 정렬 요구와 BAR 별 제약이 세대마다 다르다.
	 * 동기화: const 라 불변.
	 * 포인터가 아니라 **값으로** 품고 있어, 표 전체가 이 서술을 포함한다. */
	const struct pci_epc_features features;
/* [한국어] 세대별 차이를 담은 표. 이 표 하나가 Pro5 와 NX1 의 모든 차이를 흡수한다. */
};

#define to_uniphier_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어]
 * uniphier_pcie_ltssm_enable - LTSSM 을 켜거나 끈다
 *
 * @priv: 드라이버 상태.
 * @enable: true 면 켠다.
 *
 * 호스트 판(pcie-uniphier.c)의 같은 이름 함수와 내용이 동일하다 — 두 파일이
 * 코드를 공유하지 않고 각자 같은 것을 정의해 두었다.
 *
 * 읽기-수정-쓰기가 필수다. 같은 레지스터에 다른 제어 비트가 있을 수 있어
 * 통째로 쓸 수 없다.
 *
 * 두 init 콜백이 설정 전에 이것을 끄고, start_link 가 EPF 준비가 끝난 뒤 켠다.
 *
 * 실행 컨텍스트: probe 와 DWC 코어의 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_pro5_init_ep() / nx1_init_ep() / start_link() / stop_link()
 *     → [이 함수] → readl() → writel()
 */
static void uniphier_pcie_ltssm_enable(struct uniphier_pcie_ep_priv *priv,
				       bool enable)
{
	u32 val;

	val = readl(priv->base + PCL_APP_READY_CTRL);
	/* [한국어] 켜라는 요청이면, */
	if (enable)
		/* [한국어] 비트를 세우고, */
		val |= PCL_APP_LTSSM_ENABLE;
	/* [한국어] 끄라는 요청이면 — */
	else
		val &= ~PCL_APP_LTSSM_ENABLE;
	/* [한국어] 되쓴다. */
	writel(val, priv->base + PCL_APP_READY_CTRL);
}

/* [한국어]
 * uniphier_pcie_phy_reset - PHY 리셋을 걸거나 푼다
 *
 * @priv: 드라이버 상태.
 * @assert: true 면 건다.
 *
 * 호스트 판에는 없는 함수다. 이 파일은 PHY 초기화를 **리셋으로 감싸는데**,
 * uniphier_pcie_ep_enable() 이 리셋을 걸고 phy_init() 을 부른 뒤 리셋을 푼다.
 *
 * 그 순서의 근거는 이 트리에 drivers/phy 가 없어 확인 못 함 — PHY 드라이버가
 * 리셋 상태에서 레지스터를 설정하도록 요구하는 것으로 보인다.
 *
 * ltssm_enable 과 같은 형태로 방향을 인자로 받는다.
 *
 * 실행 컨텍스트: uniphier_pcie_ep_enable() 안. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_ep_enable() → [이 함수] → readl() → writel()
 */
static void uniphier_pcie_phy_reset(struct uniphier_pcie_ep_priv *priv,
				    bool assert)
{
	u32 val;

	val = readl(priv->base + PCL_RSTCTRL2);
	/* [한국어] 걸라는 요청이면, */
	if (assert)
		/* [한국어] 비트를 세우고, */
		val |= PCL_RSTCTRL_PHY_RESET;
	/* [한국어] 풀라는 요청이면 — */
	else
		val &= ~PCL_RSTCTRL_PHY_RESET;
	/* [한국어] 되쓴다. phy_init() 을 이 리셋으로 감싸는 것이 이 파일에만 있는 순서다. */
	writel(val, priv->base + PCL_RSTCTRL2);
}

/* [한국어]
 * uniphier_pcie_pro5_init_ep - Pro5 세대의 EP 모드 초기화
 *
 * @priv: 드라이버 상태.
 *
 * SoC 표의 init 콜백으로 등록되어, Pro5 에서만 불린다.
 *
 * 네 단계다.
 * 1. **EP 모드로 설정한다.** REGEN 과 REGVAL 을 함께 세우는데, REGVAL 을
 *    세우는 것이 EP 라는 뜻이다 — 호스트 판이 같은 비트를 지워 RC 를 고르는
 *    것과 정확히 반대다.
 * 2. 클럭 요청을 끈다.
 * 3. PIPE3 와 AXI 리셋 셋을 함께 푼다 — 레지스터·슬레이브·마스터 세
 *    AXI 경로가 모두 열려야 한다.
 * 4. LTSSM 을 끄고 100ms 기다린다.
 *
 * **PERST# 를 다루지 않는다.** NX1 판이 그것을 조작하는 것과 다른데,
 * 이 세대에서는 그 신호가 다른 방식으로 처리되는 것으로 보이며 근거는
 * 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: uniphier_pcie_ep_enable() 안. msleep 이 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_ep_enable() → data->init == [이 함수]
 *     → readl()/writel() → uniphier_pcie_ltssm_enable(false) → msleep()
 */
static void uniphier_pcie_pro5_init_ep(struct uniphier_pcie_ep_priv *priv)
{
	u32 val;

	/* set EP mode */
	val = readl(priv->base + PCL_MODE);
	val |= PCL_MODE_REGEN | PCL_MODE_REGVAL;
	/* [한국어] 되쓴다. **REGVAL 을 세워 EP 를 고른다** — 호스트 판은 이것을 지운다. */
	writel(val, priv->base + PCL_MODE);
/* [한국어] 다음은 클럭 요청이다. */

	/* clock request */
	val = readl(priv->base + PCL_APP_CLK_CTRL);
	val &= ~PCL_APP_CLK_REQ;
	/* [한국어] 요청 비트를 꺼 되쓴다. */
	writel(val, priv->base + PCL_APP_CLK_CTRL);
/* [한국어] 이제 AXI 와 PIPE3 리셋을 푼다. */

	/* deassert PIPE3 and AXI reset */
	val = readl(priv->base + PCL_RSTCTRL0);
	val |= PCL_RSTCTRL_AXI_REG | PCL_RSTCTRL_AXI_SLAVE
		/* [한국어] 네 비트를 함께 세운다 — 세우는 것이 리셋 해제라는 뜻이다. */
		| PCL_RSTCTRL_AXI_MASTER | PCL_RSTCTRL_PIPE3;
	/* [한국어] 되쓴다. 이제 AXI 세 경로가 모두 열린다. */
	writel(val, priv->base + PCL_RSTCTRL0);
/* [한국어] 설정하는 동안 링크 훈련이 돌면 안 되므로 — */

	uniphier_pcie_ltssm_enable(priv, false);
/* [한국어] 100ms 기다린다. 위 usleep_range 와 달리 msleep 인 것은
 * 이 세대의 요구가 다른 것으로 보이나 근거는 확인 못 함. */

	msleep(100);
}

/* [한국어]
 * uniphier_pcie_nx1_init_ep - NX1 세대의 EP 모드 초기화
 *
 * @priv: 드라이버 상태.
 *
 * Pro5 판과 같은 자리(SoC 표의 init 콜백)를 채우지만 내용이 다르다.
 *
 * **호스트 판의 uniphier_pcie_init_rc() 와 거의 같은 순서다** — EP 모드
 * 설정, 보조 전원 감지, PERST# 어서트, LTSSM 끄기, 100~200ms 대기,
 * PERST# 디어서트. 다른 것은 첫 단계에서 REGVAL 을 **세운다**(EP)는 것뿐이다.
 *
 * PERST# 를 다루는 것이 Pro5 판과 다른 점이며, 엔드포인트인데도 이 신호를
 * 조작한다는 점이 눈에 띈다 — 호스트가 보내는 PERST# 를 받는 것이 아니라
 * 이 컨트롤러 내부의 리셋 경로를 제어하는 것으로 보이나, 그 구분의 근거는
 * 이 트리에서 확인 못 함.
 *
 * AXI 리셋을 다루지 않는다. Pro5 판이 그것을 푸는 것과 다르며, 이 세대에서는
 * 그 리셋이 다른 경로로 풀리는 것으로 보인다.
 *
 * 실행 컨텍스트: uniphier_pcie_ep_enable() 안. usleep_range 가 있어
 * 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_ep_enable() → data->init == [이 함수]
 *     → readl()/writel() → uniphier_pcie_ltssm_enable(false) → usleep_range()
 */
static void uniphier_pcie_nx1_init_ep(struct uniphier_pcie_ep_priv *priv)
{
	u32 val;

	/* set EP mode */
	val = readl(priv->base + PCL_MODE);
	val |= PCL_MODE_REGEN | PCL_MODE_REGVAL;
	/* [한국어] 되쓴다. 여기서도 REGVAL 을 세워 EP 를 고른다. */
	writel(val, priv->base + PCL_MODE);
/* [한국어] 다음은 보조 전원 감지다. */

	/* use auxiliary power detection */
	val = readl(priv->base + PCL_APP_PM0);
	val |= PCL_SYS_AUX_PWR_DET;
	/* [한국어] 되쓴다. */
	writel(val, priv->base + PCL_APP_PM0);
/* [한국어] 이제 PERST# 를 어서트한다. */

	/* assert PERST# */
	val = readl(priv->base + PCL_PINCTRL0);
	val &= ~(PCL_PERST_NOE_REGVAL | PCL_PERST_OUT_REGVAL
		 /* [한국어] 세 신호의 값을 모두 지운다. */
		 | PCL_PERST_PLDN_REGVAL);
	/* [한국어] 그와 동시에 세 REGEN 을 세워 소프트웨어가 그 셋을 쥔 상태로 만든다. */
	val |= PCL_PERST_NOE_REGEN | PCL_PERST_OUT_REGEN
		/* [한국어] 풀다운까지 포함해 셋을 함께 제어한다. */
		| PCL_PERST_PLDN_REGEN;
	/* [한국어] 되쓴다. */
	writel(val, priv->base + PCL_PINCTRL0);
/* [한국어] 설정하는 동안 훈련이 돌면 안 되므로 — */

	uniphier_pcie_ltssm_enable(priv, false);
/* [한국어] LTSSM 을 꺼 둔다. */

	usleep_range(100000, 200000);
/* [한국어] 규격이 요구하는 시간을 기다린다. */

	/* deassert PERST# */
	val = readl(priv->base + PCL_PINCTRL0);
	val |= PCL_PERST_OUT_REGVAL | PCL_PERST_OUT_REGEN;
	/* [한국어] 출력 값과 출력 허용만 다시 세워 PERST# 를 푼다. */
	writel(val, priv->base + PCL_PINCTRL0);
}

/* [한국어]
 * uniphier_pcie_nx1_wait_ep - NX1 에서 PIPE 클럭이 살아나기를 기다린다
 *
 * @priv: 드라이버 상태.
 * @return: 0 = 성공, -ETIMEDOUT.
 *
 * SoC 표의 wait 콜백이며 **NX1 에만 있다** — Pro5 표는 이 자리가 NULL 이라
 * 그 세대에서는 대기를 건너뛴다.
 *
 * 호스트 판의 uniphier_pcie_wait_rc() 와 같은 레지스터를 같은 시간으로
 * 폴링한다. 기록 문구만 "EP 모드" 로 다르다.
 *
 * 100ms 간격으로 1초까지 기다린다.
 *
 * 실행 컨텍스트: uniphier_pcie_ep_enable() 안. 폴링 대기가 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 시간이 다하면 기록을 남기고 -ETIMEDOUT.
 *
 * 호출 체인:
 *   uniphier_pcie_ep_enable() → data->wait == [이 함수]
 *     → readl_poll_timeout()
 */
static int uniphier_pcie_nx1_wait_ep(struct uniphier_pcie_ep_priv *priv)
{
	u32 status;
	int ret;
/* [한국어] PHY 를 켠 뒤 컨트롤러가 실제로 살아났는지 확인한다. */

	/* wait PIPE clock */
	ret = readl_poll_timeout(priv->base + PCL_PIPEMON, status,
				 status & PCL_PCLK_ALIVE, 100000, 1000000);
	if (ret) {
		/* [한국어] 실패하면 EP 모드 초기화 실패로 기록한다. */
		dev_err(priv->pci.dev,
			"Failed to initialize controller in EP mode\n");
		return ret;
	}

	return 0;
}

/* [한국어]
 * uniphier_pcie_start_link - LTSSM 을 켠다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * init 콜백이 꺼 두었던 LTSSM 을 여기서 켠다.
 *
 * 엔드포인트에서 이 콜백이 불리는 시점은 EPF 드라이버가 준비를 마쳤을
 * 때다 — 그 전에 링크를 열면 호스트가 준비되지 않은 장치를 열거한다.
 *
 * 실행 컨텍스트: EPF 계층. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 → dw_pcie_start_link() → dw_pcie_ops.start_link == [이 함수]
 *     → uniphier_pcie_ltssm_enable(true)
 */
static int uniphier_pcie_start_link(struct dw_pcie *pci)
{
	struct uniphier_pcie_ep_priv *priv = to_uniphier_pcie(pci);

	uniphier_pcie_ltssm_enable(priv, true);
/* [한국어] init 콜백이 꺼 두었던 LTSSM 을 여기서 켠다 — EPF 준비가 끝난 시점이다. */

	return 0;
}

/* [한국어]
 * uniphier_pcie_stop_link - LTSSM 을 끈다
 *
 * @pci: DWC 코어의 문맥.
 *
 * uniphier_pcie_start_link() 의 짝이다.
 *
 * 실행 컨텍스트: EPF 계층. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 → dw_pcie_stop_link() → dw_pcie_ops.stop_link == [이 함수]
 *     → uniphier_pcie_ltssm_enable(false)
 */
static void uniphier_pcie_stop_link(struct dw_pcie *pci)
{
	struct uniphier_pcie_ep_priv *priv = to_uniphier_pcie(pci);

	uniphier_pcie_ltssm_enable(priv, false);
}

/* [한국어]
 * uniphier_pcie_ep_raise_intx_irq - 호스트에게 INTx 펄스를 보낸다
 *
 * @ep: DWC 의 엔드포인트 문맥.
 * @return: 언제나 0.
 *
 * **펄스를 만드는 것** 이 이 함수의 전부다. 비트를 세워 INTx 를 어서트하고,
 * 30µs 뒤 지워 디어서트한다.
 *
 * 위 상류 주석이 두 가지를 밝힌다 — 이 신호가 펄스이므로 되도록 빨리
 * 지워야 한다는 것, 그리고 이 세우기·지우기 한 쌍이 상위
 * pci_epc_raise_irq() 의 뮤텍스로 보호된다는 것. 그 덕분에 이 파일에
 * 별도 잠금이 없다.
 *
 * 지울 때 레지스터를 다시 읽지 않고 위에서 읽어 둔 값을 쓴다. 그 사이에
 * 같은 레지스터의 다른 비트가 바뀌었다면 되돌려지는 셈이지만, 뮤텍스가
 * 그 사이를 막는다.
 *
 * udelay 를 쓰는 것은 30µs 가 짧아 잠들 가치가 없기 때문이다.
 *
 * 실행 컨텍스트: EPF 드라이버. udelay 동안 CPU 를 붙잡는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_ep_raise_irq() → [이 함수] → readl()/writel() → udelay()
 */
static int uniphier_pcie_ep_raise_intx_irq(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct uniphier_pcie_ep_priv *priv = to_uniphier_pcie(pci);
	/* [한국어] 읽기-수정-쓰기 할 값. */
	u32 val;
/* [한국어] 위 상류 주석이 이 시퀀스의 성격과 보호 방식을 밝힌다. */

	/*
	 * This makes pulse signal to send INTx to the RC, so this should
	 * be cleared as soon as possible. This sequence is covered with
	 * mutex in pci_epc_raise_irq().
	 */
	/* assert INTx */
	val = readl(priv->base + PCL_APP_INTX);
	val |= PCL_APP_INTX_SYS_INT;
	/* [한국어] 비트를 세워 INTx 를 어서트한다. */
	writel(val, priv->base + PCL_APP_INTX);
/* [한국어] 이제 펄스 폭만큼 기다린다. */

	udelay(PCL_INTX_WIDTH_USEC);

	/* deassert INTx */
	val &= ~PCL_APP_INTX_SYS_INT;
	writel(val, priv->base + PCL_APP_INTX);
/* [한국어] **레지스터를 다시 읽지 않고** 위에서 읽어 둔 값에서 비트만 지운다 —
 * 그 사이 다른 비트가 바뀌었다면 되돌려지지만, 상위 뮤텍스가 그 사이를 막는다. */

	return 0;
}

/* [한국어]
 * uniphier_pcie_ep_raise_msi_irq - 호스트에게 MSI 를 보낸다
 *
 * @ep: DWC 의 엔드포인트 문맥.
 * @func_no: 물리 기능 번호.
 * @interrupt_num: MSI 벡터 번호(1부터 센다).
 * @return: 언제나 0.
 *
 * 두 레지스터를 순서대로 쓴다 — 먼저 어느 기능의 몇 번 벡터인지 지정하고,
 * 그 다음 요청 비트를 세워 실제로 보낸다.
 *
 * **벡터 번호에서 1 을 빼는 것** 이 요점이다. EPF 계층은 벡터를 1부터
 * 세는데 하드웨어 필드는 0부터라, 그 차이를 여기서 흡수한다.
 *
 * 기능 번호를 TC(Traffic Class) 필드에 넣는다. 이름과 쓰임이 어긋나 보이는데,
 * 그 필드가 실제로 무엇을 뜻하는지는 데이터시트 없이 확인 못 함.
 *
 * 요청 비트만 읽기-수정-쓰기다. 첫 레지스터는 통째로 쓰는데, 그 안에
 * 다른 비트를 이 드라이버가 쓰지 않기 때문이다.
 *
 * 실행 컨텍스트: EPF 드라이버. 잠들지 않는다.
 *
 * 에러 경로: 없다. 요청이 실제로 전달됐는지 확인하지 않는다.
 *
 * 호출 체인:
 *   uniphier_pcie_ep_raise_irq() → [이 함수] → writel() → readl() → writel()
 */
static int uniphier_pcie_ep_raise_msi_irq(struct dw_pcie_ep *ep,
					  u8 func_no, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct uniphier_pcie_ep_priv *priv = to_uniphier_pcie(pci);
	/* [한국어] 두 레지스터에 나눠 쓸 값. */
	u32 val;
/* [한국어] 먼저 대상을 지정한다. */

	val = FIELD_PREP(PCL_APP_VEN_MSI_TC_MASK, func_no)
		/* [한국어] **벡터 번호에서 1 을 뺀다** — EPF 는 1부터, 하드웨어 필드는 0부터 세기 때문이다. */
		| FIELD_PREP(PCL_APP_VEN_MSI_VECTOR_MASK, interrupt_num - 1);
	/* [한국어] 통째로 쓴다. 이 레지스터에 다른 비트를 이 드라이버가 쓰지 않는다. */
	writel(val, priv->base + PCL_APP_MSI0);
/* [한국어] 이제 요청 비트를 세워 실제로 보낸다. */

	val = readl(priv->base + PCL_APP_MSI1);
	/* [한국어] 요청 비트만 세우고, */
	val |= PCL_APP_MSI_REQ;
	/* [한국어] 되쓴다. 이쪽은 읽기-수정-쓰기다. */
	writel(val, priv->base + PCL_APP_MSI1);
/* [한국어] 요청이 나갔다. 실제로 전달됐는지는 확인하지 않는다. */

	return 0;
}

/* [한국어]
 * uniphier_pcie_ep_raise_irq - 인터럽트 종류를 가려 해당 경로로 보낸다
 *
 * @ep: DWC 의 엔드포인트 문맥.
 * @func_no: 물리 기능 번호.
 * @type: INTx 인지 MSI 인지.
 * @interrupt_num: MSI 벡터 번호.
 * @return: 0.
 *
 * EPF 드라이버가 호스트에게 알릴 일이 생겼을 때 들어오는 진입점이다.
 *
 * MSI-X 를 지원하지 않는다. switch 에 그 갈래가 없으며, 아래 두 SoC 표의
 * msix_capable 이 모두 false 인 것과 맞물린다.
 *
 * [상류 코드 관찰] **알 수 없는 종류도 0(성공)을 돌려준다.** default 갈래가
 * 기록만 남기고 빠져나와 마지막 `return 0` 에 닿기 때문이다. 같은 계열의
 * 다른 EP 드라이버들이 -EINVAL 을 돌려주는 것과 다르며, 호출자는 인터럽트가
 * 실제로 나갔다고 믿게 된다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: EPF 드라이버. 프로세스 컨텍스트.
 *
 * 에러 경로: 위 관찰 참조 — 오류를 올려보내지 않는다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq() → dw_pcie_ep_ops.raise_irq == [이 함수]
 *     → uniphier_pcie_ep_raise_intx_irq() / _raise_msi_irq()
 */
static int uniphier_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				      unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	switch (type) {
	/* [한국어] 레거시 INTx 면, */
	case PCI_IRQ_INTX:
		/* [한국어] 펄스를 만드는 경로로 간다. */
		return uniphier_pcie_ep_raise_intx_irq(ep);
	case PCI_IRQ_MSI:
		/* [한국어] MSI 면 기능 번호와 벡터 번호까지 넘긴다. */
		return uniphier_pcie_ep_raise_msi_irq(ep, func_no,
						      interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type (%d)\n", type);
	}

	return 0;
}

/* [한국어]
 * uniphier_pcie_get_features - 이 세대의 엔드포인트 능력을 EPC 계층에 알린다
 *
 * @ep: DWC 의 엔드포인트 문맥.
 * @return: 이 SoC 의 능력 서술.
 *
 * EPF 드라이버가 BAR 을 설정하기 전에 "이 하드웨어가 무엇을 할 수 있는가"
 * 를 묻는 통로다.
 *
 * **SoC 표 안의 서술을 돌려준다.** 정적 구조체 하나를 돌려주는 다른
 * 드라이버들과 달리, 이 파일은 세대마다 능력이 달라 표를 거친다 —
 * Pro5 는 64KB 정렬에 BAR4/5 가 없고, NX1 은 4KB 정렬에 BAR4 가 살아 있다.
 *
 * 실행 컨텍스트: EPF 드라이버. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_get_features()
 *     → dw_pcie_ep_ops.get_features == [이 함수]
 */
static const struct pci_epc_features*
uniphier_pcie_get_features(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct uniphier_pcie_ep_priv *priv = to_uniphier_pcie(pci);
/* [한국어] 알 수 없는 종류면 기록만 남기고 아래로 빠진다. */

	return &priv->data->features;
/* [한국어] **그 경우에도 0(성공)이 나간다**(위 함수 블록의 관찰 참조). */
}

static const struct dw_pcie_ep_ops uniphier_pcie_ep_ops = {
	/* [한국어] 호스트에게 인터럽트를 보내는 통로. */
	.raise_irq = uniphier_pcie_ep_raise_irq,
	/* [한국어] 이 세대의 능력을 알리는 통로. 이 둘이 EP 쪽 접점 전부다. */
	.get_features = uniphier_pcie_get_features,
};

/* [한국어]
 * uniphier_pcie_ep_enable - 클럭·리셋·PHY 를 순서대로 켜고 EP 모드로 세운다
 *
 * @priv: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 하드웨어를 동작 상태로 만드는 전 과정이며, **세대별 차이를 표의 콜백
 * 둘로 흡수하는 것** 이 이 함수의 구조다.
 *
 * 여섯 단계다.
 * 1. link 클럭을 켠다.
 * 2. gio 클럭을 켠다 — Pro5 에만 있고 NX1 에서는 NULL 이라 무동작이 된다.
 * 3. link 리셋을 푼다.
 * 4. gio 리셋을 푼다 — 역시 Pro5 에만 해당한다.
 * 5. 표의 init 콜백으로 EP 모드를 설정한다.
 * 6. **PHY 리셋을 건 채로** phy_init() 을 부르고 리셋을 푼 뒤,
 *    표의 wait 콜백이 있으면 기다린다.
 *
 * 6번의 감싸기가 이 파일에만 있는 것이다. 호스트 판은 PHY 리셋을 다루지
 * 않는다.
 *
 * 콜백이 NULL 일 수 있어 둘 다 확인하고 부른다 — Pro5 표는 wait 이 NULL 이다.
 *
 * 되감기가 다섯 계단이다. 각 진입점이 그 지점까지 성공한 것만 정확히
 * 되돌리는데, **PHY 리셋은 되돌리지 않는다** — phy_exit() 뒤에 리셋이
 * 걸린 채로 남는지 풀린 채로 남는지는 실패 지점에 따라 다르다.
 *
 * 실행 컨텍스트: probe. 대기가 여럿 있어 프로세스 컨텍스트여야 하며
 * 100ms 이상 걸린다.
 *
 * 에러 경로: 각 단계의 실패를 goto 로 해당 라벨에 넣어 역순으로 되감는다.
 *
 * 호출 체인:
 *   uniphier_pcie_ep_probe() → [이 함수]
 *     → clk_prepare_enable() → reset_control_deassert()
 *     → data->init() → uniphier_pcie_phy_reset() → phy_init()
 *     → data->wait()
 */
static int uniphier_pcie_ep_enable(struct uniphier_pcie_ep_priv *priv)
{
	int ret;

	ret = clk_prepare_enable(priv->clk);
	/* [한국어] link 클럭을 켜지 못하면, */
	if (ret)
		/* [한국어] 그대로 물러난다. */
		return ret;

	ret = clk_prepare_enable(priv->clk_gio);
	/* [한국어] gio 클럭을 켜지 못하면 — NULL 이면 무동작이라 실패하지 않는다. */
	if (ret)
		/* [한국어] link 클럭을 되돌린다. */
		goto out_clk_disable;

	ret = reset_control_deassert(priv->rst);
	/* [한국어] link 리셋을 풀지 못하면, */
	if (ret)
		/* [한국어] gio 클럭부터 되돌린다. */
		goto out_clk_gio_disable;

	ret = reset_control_deassert(priv->rst_gio);
	/* [한국어] gio 리셋을 풀지 못하면, */
	if (ret)
		/* [한국어] link 리셋부터 되돌린다. */
		goto out_rst_assert;

	if (priv->data->init)
		/* [한국어] 세대별 EP 모드 초기화를 맡긴다. */
		priv->data->init(priv);

	uniphier_pcie_phy_reset(priv, true);
/* [한국어] **PHY 초기화를 리셋으로 감싼다** — 먼저 리셋을 건다. */

	ret = phy_init(priv->phy);
	/* [한국어] PHY 초기화가 실패하면, */
	if (ret)
		/* [한국어] gio 리셋부터 되돌린다. **PHY 리셋은 걸린 채로 남는다.** */
		goto out_rst_gio_assert;

	uniphier_pcie_phy_reset(priv, false);
/* [한국어] 초기화가 끝났으니 PHY 리셋을 푼다. */

	if (priv->data->wait) {
		/* [한국어] 대기 콜백이 있는 세대면 — NX1 만 해당한다. */
		ret = priv->data->wait(priv);
		/* [한국어] PIPE 클럭을 기다리고, */
		if (ret)
			/* [한국어] 실패하면 PHY 부터 되돌린다. */
			goto out_phy_exit;
	}

	return 0;

out_phy_exit:
	phy_exit(priv->phy);
out_rst_gio_assert:
	reset_control_assert(priv->rst_gio);
out_rst_assert:
	reset_control_assert(priv->rst);
out_clk_gio_disable:
	clk_disable_unprepare(priv->clk_gio);
out_clk_disable:
	clk_disable_unprepare(priv->clk);

	return ret;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크 시작과, */
	.start_link = uniphier_pcie_start_link,
	/* [한국어] 중단 둘뿐이다. 엔드포인트라 link_up 콜백이 없다 —
	 * 링크 여부는 호스트가 정하고, 이쪽은 알 필요가 없다. */
	.stop_link = uniphier_pcie_stop_link,
};

/* [한국어]
 * uniphier_pcie_ep_probe - SoC 표를 고르고 자원을 얻어 엔드포인트를 등록한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * **SoC 표를 가장 먼저 얻는다.** 그 표가 어떤 자원이 필요한지를 정하므로,
 * 자원 확보보다 앞서야 한다. WARN_ON 으로 잡는 것은 매칭 표에 data 를
 * 빠뜨린 것이 코드의 버그이지 런타임 상황이 아니기 때문이다.
 *
 * gio 클럭·리셋을 조건부로 얻는 것이 그 표의 첫 쓰임이다. has_gio 가
 * 거짓이면 두 포인터가 NULL 로 남고, 아래 clk_prepare_enable(NULL) 과
 * reset_control_deassert(NULL) 이 무동작이 된다.
 *
 * PHY 는 선택 사항이지만 **오류를 기록한다** — optional 판이라 없는 것은
 * 오류가 아니고, 여기 걸리는 것은 진짜 실패뿐이다.
 *
 * EP 등록이 세 단계다 — dw_pcie_ep_init() 으로 EPC 를 만들고,
 * dw_pcie_ep_init_registers() 로 config 공간을 채우고,
 * pci_epc_init_notify() 로 EPF 계층에 알린다.
 *
 * 되감기가 부분적이다. 두 번째 단계가 실패하면 첫 단계를 되돌리지만,
 * **uniphier_pcie_ep_enable() 이 켠 클럭·리셋·PHY 는 어느 경로에서도
 * 되돌리지 않는다.** 이 파일에 remove 콜백도 없어 그 상태가 그대로 남는다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 표가 없으면 -EINVAL, 자원 확보 실패는 그 오류, EP 등록
 * 실패는 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → of_device_get_match_data() → devm_platform_ioremap_resource_byname()
 *     → devm_clk_get() → devm_reset_control_get_shared()
 *     → devm_phy_optional_get() → uniphier_pcie_ep_enable()
 *     → dw_pcie_ep_init() → dw_pcie_ep_init_registers()
 */
static int uniphier_pcie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphier_pcie_ep_priv *priv;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 상태 구조를 잡는다. */

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	/* [한국어] 잡지 못하면, */
	if (!priv)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	priv->data = of_device_get_match_data(dev);
	/* [한국어] **표가 없으면 코드의 버그다** — 매칭 표에 data 를 빠뜨린 경우이지
	 * 런타임 상황이 아니라, 경고와 함께 스택 추적을 남긴다. */
	if (WARN_ON(!priv->data))
		/* [한국어] 잘못된 인자로 답한다. */
		return -EINVAL;

	priv->pci.dev = dev;
	/* [한국어] 코어 콜백 표를 건다. */
	priv->pci.ops = &dw_pcie_ops;
/* [한국어] 이제 자원을 얻는다. */

	priv->base = devm_platform_ioremap_resource_byname(pdev, "link");
	/* [한국어] "link" 창 매핑이 실패하면, */
	if (IS_ERR(priv->base))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(priv->base);

	if (priv->data->has_gio) {
		/* [한국어] **표가 요구하는 세대에서만** gio 클럭을 얻는다. */
		priv->clk_gio = devm_clk_get(dev, "gio");
		/* [한국어] 얻지 못하면, */
		if (IS_ERR(priv->clk_gio))
			/* [한국어] 그 오류를 올려보낸다. */
			return PTR_ERR(priv->clk_gio);
/* [한국어] gio 리셋도 마찬가지다. */

		priv->rst_gio = devm_reset_control_get_shared(dev, "gio");
		/* [한국어] shared 판이라 다른 블록과 공유한다. */
		if (IS_ERR(priv->rst_gio))
			/* [한국어] 얻지 못하면 그 오류를 올려보낸다. */
			return PTR_ERR(priv->rst_gio);
	/* [한국어] gio 자원 처리 끝. has_gio 가 거짓이면 두 포인터가 NULL 로 남는다. */
	}

	priv->clk = devm_clk_get(dev, "link");
	/* [한국어] link 클럭을 얻지 못하면, */
	if (IS_ERR(priv->clk))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(priv->clk);

	priv->rst = devm_reset_control_get_shared(dev, "link");
	/* [한국어] link 리셋을 얻지 못하면, */
	if (IS_ERR(priv->rst))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(priv->rst);

	priv->phy = devm_phy_optional_get(dev, "pcie-phy");
	/* [한국어] PHY 조회가 실패하면 — 선택 사항이라 없는 것은 실패가 아니다. */
	if (IS_ERR(priv->phy)) {
		/* [한국어] 오류 코드를 꺼내, */
		ret = PTR_ERR(priv->phy);
		/* [한국어] **기록을 남긴다** — 다른 자원과 달리 이쪽만 로그가 있다. */
		dev_err(dev, "Failed to get phy (%d)\n", ret);
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;
	}

	platform_set_drvdata(pdev, priv);
/* [한국어] 이 파일의 변환 매크로가 drvdata 를 거치므로,
 * 그것을 쓰는 init 콜백보다 먼저 매달아야 한다. */

	ret = uniphier_pcie_ep_enable(priv);
	/* [한국어] 하드웨어를 깨우지 못하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	priv->pci.ep.ops = &uniphier_pcie_ep_ops;
	/* [한국어] EPC 를 등록한다 — 이 시점부터 EPF 드라이버가 붙을 수 있다. */
	ret = dw_pcie_ep_init(&priv->pci.ep);
	/* [한국어] 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. **켜 둔 클럭·리셋·PHY 는 되돌리지 않는다.** */
		return ret;

	ret = dw_pcie_ep_init_registers(&priv->pci.ep);
	/* [한국어] config 공간 초기화가 실패하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to initialize DWC endpoint registers\n");
		/* [한국어] 방금 등록한 EPC 를 되돌린다. */
		dw_pcie_ep_deinit(&priv->pci.ep);
		return ret;
	}

	pci_epc_init_notify(priv->pci.ep.epc);

	return 0;
}

static const struct uniphier_pcie_ep_soc_data uniphier_pro5_data = {
	/* [한국어] Pro5 는 gio 클럭·리셋이 필요하다. */
	.has_gio = true,
	/* [한국어] AXI 리셋을 다루는 초기화를 쓴다. */
	.init = uniphier_pcie_pro5_init_ep,
	.wait = NULL,
	.features = {
		DWC_EPC_COMMON_FEATURES,
		.linkup_notifier = false,
		.msi_capable = true,
		.msix_capable = false,
		.align = 1 << 16,
		.bar[BAR_0] = { .only_64bit = true, },
		.bar[BAR_2] = { .only_64bit = true, },
		.bar[BAR_4] = { .type = BAR_DISABLED, },
		.bar[BAR_5] = { .type = BAR_DISABLED, },
	},
};

static const struct uniphier_pcie_ep_soc_data uniphier_nx1_data = {
	/* [한국어] NX1 은 gio 가 없다. */
	.has_gio = false,
	/* [한국어] PERST# 를 다루는 초기화를 쓴다. **두 세대의 차이가 이 표 두 줄에 있다.** */
	.init = uniphier_pcie_nx1_init_ep,
	.wait = uniphier_pcie_nx1_wait_ep,
	.features = {
		DWC_EPC_COMMON_FEATURES,
		.linkup_notifier = false,
		.msi_capable = true,
		.msix_capable = false,
		.align = 1 << 12,
		.bar[BAR_0] = { .only_64bit = true, },
		.bar[BAR_2] = { .only_64bit = true, },
		.bar[BAR_4] = { .only_64bit = true, },
	},
};

static const struct of_device_id uniphier_pcie_ep_match[] = {
	/* [한국어] Pro5 항목. */
	{
		.compatible = "socionext,uniphier-pro5-pcie-ep",
		/* [한국어] 그 표를 가리킨다. */
		.data = &uniphier_pro5_data,
	},
	{
		.compatible = "socionext,uniphier-nx1-pcie-ep",
		/* [한국어] NX1 은 다른 표를 가리킨다 — compatible 하나가 초기화 순서와
		 * 필요한 자원, 능력 서술을 모두 정한다. */
		.data = &uniphier_nx1_data,
	},
	{ /* sentinel */ },
};

static struct platform_driver uniphier_pcie_ep_driver = {
	/* [한국어] probe 콜백. **remove 가 없다** — builtin 이고 언바인드도 막혀 있다. */
	.probe  = uniphier_pcie_ep_probe,
	/* [한국어] 드라이버 정보. */
	.driver = {
		.name = "uniphier-pcie-ep",
		/* [한국어] 위 매칭 표. */
		.of_match_table = uniphier_pcie_ep_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(uniphier_pcie_ep_driver);
