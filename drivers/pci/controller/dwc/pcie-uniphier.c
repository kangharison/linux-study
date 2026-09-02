// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for UniPhier SoCs
 * Copyright 2018 Socionext Inc.
 * Author: Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 */

/*
 * [한국어 설명] Socionext UniPhier SoC 의 DesignWare PCIe 호스트 글루 (pcie-uniphier.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 Socionext UniPhier SoC 에 붙이는 글루
 * 드라이버다. config 접근, ATU 설정, MSI, 버스 스캔은 모두
 * pcie-designware-host.c 가 하고, 이 파일은 DWC 코어가 알 수 없는 SoC
 * 고유의 것만 맡는다.
 *
 * 같은 SoC 의 엔드포인트 드라이버(pcie-uniphier-ep.c)와 짝을 이루며,
 * 두 파일이 같은 "link" 레지스터 창을 서로 다른 모드로 쓴다. 어느 쪽으로
 * 동작할지는 PCL_MODE 레지스터가 정하고, 디바이스 트리의 compatible 이
 * 어느 드라이버가 붙을지를 정한다.
 *
 * 맡는 일이 넷이다.
 *   1) RC 모드 설정과 PERST# 출력 — 엔드포인트 판이 PERST# 를 받는 것과
 *      반대로, 여기서는 하위 장치에 리셋을 내보낸다.
 *   2) LTSSM 제어 — 링크 훈련을 켜고 끈다.
 *   3) PIPE 클럭 대기 — 컨트롤러가 살아났는지 폴링으로 확인한다.
 *   4) **INTx 도메인** — 이 SoC 는 INTx 넷을 요약 인터럽트 하나로 모아
 *      올리므로, 그것을 넷으로 갈라 주는 도메인을 이 파일이 직접 만든다.
 *
 * 4번이 이 파일에서 가장 큰 덩어리이며, 인터럽트 칩·도메인·체인 핸들러가
 * 모두 여기 있다. 디바이스 트리의 자식 노드
 * "legacy-interrupt-controller" 가 그 도메인의 근거다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> uniphier_pcie_probe()
 *     -> "link" 창, 클럭, 리셋, PHY 를 얻는다
 *     -> uniphier_pcie_host_enable()
 *        -> 클럭·리셋 -> RC 모드 설정과 PERST# 조작 -> PHY -> PIPE 클럭 대기
 *     -> dw_pcie_host_init()  [pcie-designware-host.c]
 *        -> 그 안에서 콜백 -> [이 파일] uniphier_pcie_host_init()
 *           -> INTx 도메인을 만들고 체인 핸들러를 걸고 인터럽트를 허용
 *        -> 코어가 start_link 콜백을 부른다 -> [이 파일] LTSSM 켜기
 *        -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * INTx 인터럽트가 올라오는 방향:
 *   장치가 INTx 어서션 -> SoC 가 요약 인터럽트 하나를 올림
 *     -> [이 파일] uniphier_pcie_irq_handler() (체인 핸들러)
 *        -> 디버그용 이벤트 비트를 찍고 지운다
 *        -> PCL_RCV_INTX 의 상태 비트를 읽어 어느 INTx 인지 가림
 *        -> generic_handle_domain_irq() -> 장치 드라이버의 핸들러
 *
 * 실행 컨텍스트: probe 와 host_init 은 프로세스 컨텍스트,
 * uniphier_pcie_irq_handler() 는 인터럽트 문맥, 마스크·언마스크는
 * 인터럽트 문맥일 수 있다. 그래서 그 셋이 공유하는 PCL_RCV_INTX 레지스터를
 * DWC 코어의 pp->lock 을 빌려 지킨다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c. 접점은 두 벌의 콜백
 *   표다 — dw_pcie_ops(start_link, stop_link, link_up)와
 *   dw_pcie_host_ops(init 하나).
 * 옆쪽: irqdomain·irqchip 코어에 INTx 도메인이 얹히고, PHY·클럭·리셋
 *   계층에 하드웨어 제어를 맡긴다. 그 계층들은 이 트리에 없어
 *   내부는 확인 대상 밖이다.
 *
 * 데이터 흐름:
 *   디바이스 트리("link" 자원, 클럭, 리셋, 선택적 PHY,
 *                 자식 노드 legacy-interrupt-controller) -> probe
 *     -> struct uniphier_pcie
 *   인터럽트: PCL_RCV_INTX 의 상태 비트(0~3) -> hwirq -> 도메인 -> 핸들러
 *   제어:     hwirq -> PCL_RCV_INTX 의 마스크 비트(8~11)
 *   같은 레지스터가 상태와 마스크를 서로 다른 비트 자리에 담고 있어,
 *   마스크 조작이 hwirq 에 8 을 더한 자리를 건드린다.
 *
 * 공유 상태: struct uniphier_pcie 하나. PCL_RCV_INTX 레지스터만 잠금이
 *   필요하고 나머지는 probe 후 불변이다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면 그 인터럽트는 MSI 로 가므로
 * 이 파일의 INTx 경로를 지나지 않는다. 다만 uniphier_pcie_init_rc() 의
 * PERST# 조작은 그 컨트롤러에 직접 닿는다 — 이 함수가 PERST# 를 걸고
 * 100ms 이상 기다린 뒤 푸는 순서가, 하위 NVMe 가 안정적으로 리셋에서
 * 깨어나는 조건이 된다.
 *
 * === 주요 함수/구조체 요약 ===
 * uniphier_pcie_init_rc()        : RC 모드 지정과 PERST# 조작. 이 파일의
 *                                  하드웨어 순서가 여기 모여 있다.
 * uniphier_pcie_host_enable()    : 클럭·리셋·PHY·PIPE 대기를 순서대로 밟는다.
 * uniphier_pcie_config_intx_irq(): INTx 도메인을 만들고 체인 핸들러를 건다.
 * uniphier_pcie_irq_handler()    : 요약 인터럽트를 INTx 넷으로 갈라 보낸다.
 * uniphier_pcie_ltssm_enable()   : LTSSM 비트 하나를 켜고 끈다.
 * struct uniphier_pcie           : dw_pcie 를 맨 앞에 둔 이 드라이버의 상태.
 */

/* [한국어] BIT()/GENMASK(). 아래 레지스터 필드 정의가 이것을 쓴다. */
#include <linux/bitops.h>
/* [한국어] FIELD_GET(). 인터럽트 상태 비트를 뽑는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] clk_prepare_enable()/clk_disable_unprepare(). */
#include <linux/clk.h>
/* [한국어] usleep_range(). PERST# 대기가 쓴다. */
#include <linux/delay.h>
/* [한국어] __init 표시. */
#include <linux/init.h>
/* [한국어] irqreturn_t 와 인터럽트 선언들. */
#include <linux/interrupt.h>
/* [한국어] readl_poll_timeout(). PIPE 클럭 대기가 쓴다. */
#include <linux/iopoll.h>
/* [한국어] chained_irq_enter()/exit(). 이 파일의 INTx 핸들러가 체인 핸들러다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain_create_linear() 와 irq_domain_ops. INTx 도메인의 뼈대다. */
#include <linux/irqdomain.h>
/* [한국어] irq_of_parse_and_map(). 자식 노드에서 인터럽트 번호를 얻는다. */
#include <linux/of_irq.h>
/* [한국어] PCI_NUM_INTX 등 PCI 코어 타입들. */
#include <linux/pci.h>
/* [한국어] phy_init()/phy_exit(). */
#include <linux/phy/phy.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] reset_control_assert()/deassert(). */
#include <linux/reset.h>

/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_host_init() 등. */
#include "pcie-designware.h"

/* [한국어] 핀 제어 레지스터. PERST# 출력이 여기 있다. */
#define PCL_PINCTRL0			0x002c
/* [한국어] PERST# 풀다운을 레지스터로 제어할지. */
#define PCL_PERST_PLDN_REGEN		BIT(12)
/* [한국어] PERST# 출력 허용(Not Output Enable)을 레지스터로 제어할지. */
#define PCL_PERST_NOE_REGEN		BIT(11)
/* [한국어] PERST# 출력 값을 레지스터로 제어할지.
 * **REGEN 셋과 REGVAL 셋이 짝을 이룬다** — REGEN 이 '이 신호를 레지스터로
 * 제어한다', REGVAL 이 '그 값' 이라는 구조이며, 아래 PCL_MODE 도 같은 짜임이다. */
#define PCL_PERST_OUT_REGEN		BIT(8)
/* [한국어] PERST# 풀다운 값. */
#define PCL_PERST_PLDN_REGVAL		BIT(4)
/* [한국어] PERST# 출력 허용 값. */
#define PCL_PERST_NOE_REGVAL		BIT(3)
/* [한국어] PERST# 출력 값. 세우면 디어서트(하이), 지우면 어서트(로)다. */
#define PCL_PERST_OUT_REGVAL		BIT(0)

/* [한국어] PIPE 모니터 레지스터. */
#define PCL_PIPEMON			0x0044
/* [한국어] PIPE 클럭이 살아 있는지. uniphier_pcie_wait_rc() 가 이 비트를 폴링한다. */
#define PCL_PCLK_ALIVE			BIT(15)

/* [한국어] 동작 모드 레지스터. */
#define PCL_MODE			0x8000
/* [한국어] 모드를 레지스터로 제어할지. */
#define PCL_MODE_REGEN			BIT(8)
/* [한국어] 모드 값. **지우면 RC, 세우면 EP** 다 — init_rc 가 이 비트를 지운다. */
#define PCL_MODE_REGVAL			BIT(0)

/* [한국어] 애플리케이션 준비 제어 레지스터. */
#define PCL_APP_READY_CTRL		0x8008
/* [한국어] LTSSM 시작 비트. 이 파일의 링크 제어가 이 비트 하나로 이뤄진다. */
#define PCL_APP_LTSSM_ENABLE		BIT(0)

/* [한국어] 전원 관리 레지스터 0. */
#define PCL_APP_PM0			0x8078
/* [한국어] 보조 전원 감지 비트. init_rc 가 이것을 켠다. */
#define PCL_SYS_AUX_PWR_DET		BIT(8)

/* [한국어] 이벤트 인터럽트 레지스터. **허용 비트와 상태 비트가 한 레지스터에 있다.** */
#define PCL_RCV_INT			0x8108
/* [한국어] 이벤트 넷을 모두 허용하는 마스크(비트 17~20). */
#define PCL_RCV_INT_ALL_ENABLE		GENMASK(20, 17)
/* [한국어] 대역폭 관리 이벤트 상태. */
#define PCL_CFG_BW_MGT_STATUS		BIT(4)
/* [한국어] 자율 대역폭 변경 이벤트 상태. */
#define PCL_CFG_LINK_AUTO_BW_STATUS	BIT(3)
/* [한국어] AER 루트 오류 상태. */
#define PCL_CFG_AER_RC_ERR_MSI_STATUS	BIT(2)
/* [한국어] PME 상태. 네 상태 모두 핸들러에서 디버그 기록으로만 쓰인다 —
 * 실제 처리는 DWC 코어와 PCI 코어가 자기 경로로 따로 한다. */
#define PCL_CFG_PME_MSI_STATUS		BIT(1)

/* [한국어] INTx 레지스터. 이 하나에 허용·마스크·상태가 모두 들어 있다. */
#define PCL_RCV_INTX			0x810c
/* [한국어] INTx 넷을 모두 허용하는 마스크(비트 16~19). */
#define PCL_RCV_INTX_ALL_ENABLE		GENMASK(19, 16)
/* [한국어] 마스크 비트 넷(비트 8~11). 이 파일에서 직접 참조하는 곳은 없다 —
 * 마스크·언마스크가 hwirq 에 SHIFT 를 더해 비트를 하나씩 만든다. */
#define PCL_RCV_INTX_ALL_MASK		GENMASK(11, 8)
/* [한국어] **마스크 비트의 시작 자리.** hwirq 0~3 에 이 값을 더하면 마스크 비트가 된다. */
#define PCL_RCV_INTX_MASK_SHIFT		8
/* [한국어] 상태 비트 넷(비트 0~3). 핸들러가 FIELD_GET 으로 이것을 뽑는다. */
#define PCL_RCV_INTX_ALL_STATUS		GENMASK(3, 0)
/* [한국어] 상태 비트의 시작 자리. 0 이라 hwirq 와 그대로 대응하며,
 * 이 파일에서 직접 참조하는 곳은 없다. */
#define PCL_RCV_INTX_STATUS_SHIFT	0

/* [한국어] 링크 상태 레지스터. */
#define PCL_STATUS_LINK			0x8140
/* [한국어] 데이터 링크 계층의 링크 확립 비트. */
#define PCL_RDLH_LINK_UP		BIT(1)
#define PCL_XMLH_LINK_UP		BIT(0)

struct uniphier_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **맨 앞에 두어** to_dw_pcie_from_pp() 변환이 성립한다.
	 * 설정자: probe 가 dev 와 ops 를 채우고, DWC 코어가 나머지를 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 모든 콜백.
	 * 값 범위: DWC 코어가 정의한 구조체.
	 * 동기화: 코어가 관리한다. */
	struct dw_pcie pci;
	/* [한국어] "link" 레지스터 창의 가상 주소.
	 * 설정자: probe 의 devm_platform_ioremap_resource_byname("link").
	 * 읽는 자: 이 파일의 모든 레지스터 접근.
	 * 값 범위: 유효한 iomem 포인터. 위 PCL_ 계열 오프셋이 이 창 기준이다.
	 * 동기화: probe 후 불변. 그 안의 PCL_RCV_INTX 만 pp->lock 이 지킨다. */
	void __iomem *base;
	/* [한국어] PCIe 클럭.
	 * 설정자: probe 의 devm_clk_get().
	 * 읽는 자: uniphier_pcie_host_enable() 이 켜고 되감기가 끈다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: probe 후 불변. */
	struct clk *clk;
	/* [한국어] PCIe 리셋. **shared 판** 이라 다른 블록과 공유한다 —
	 * 이 드라이버가 풀어도 다른 쪽이 걸어 두면 걸린 채로 남는다.
	 * 설정자: probe 의 devm_reset_control_get_shared().
	 * 읽는 자: uniphier_pcie_host_enable() 과 그 되감기.
	 * 값 범위: 유효한 리셋 컨트롤.
	 * 동기화: probe 후 불변. */
	struct reset_control *rst;
	/* [한국어] PCIe PHY. **선택 사항** 이라 없는 보드에서는 NULL 이 되고
	 * phy_init() 이 무동작이 된다.
	 * 설정자: probe 의 devm_phy_optional_get().
	 * 읽는 자: uniphier_pcie_host_enable() 과 그 되감기.
	 * 값 범위: 유효한 PHY 포인터 또는 NULL.
	 * 동기화: probe 후 불변. */
	struct phy *phy;
	/* [한국어] 이 파일이 만든 INTx 도메인.
	 * 설정자: uniphier_pcie_config_intx_irq().
	 * 읽는 자: uniphier_pcie_irq_handler() 가 hwirq 로 핸들러를 찾는 데 쓴다.
	 * 값 범위: PCI_NUM_INTX(4) 크기의 선형 도메인.
	 * 동기화: host_init 후 불변. */
	struct irq_domain *intx_irq_domain;
/* [한국어] 이 드라이버의 상태 전부. */
};

#define to_uniphier_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어]
 * uniphier_pcie_ltssm_enable - LTSSM 을 켜거나 끈다
 *
 * @pcie: 드라이버 상태.
 * @enable: true 면 켠다.
 *
 * 링크 훈련 상태 기계를 제어하는 한 비트를 다룬다.
 *
 * 켜기와 끄기를 한 함수로 합친 것이 이 파일의 관용이다. 두 동작이 같은
 * 비트를 반대로 다룰 뿐이라, 인자 하나로 방향을 정한다.
 *
 * 읽기-수정-쓰기가 필수다. 같은 레지스터에 다른 제어 비트가 있을 수 있어
 * 통째로 쓸 수 없다.
 *
 * init_rc 가 설정 전에 이것을 끄고, start_link 가 설정이 끝난 뒤 켠다 —
 * 그 한 쌍이 이 파일의 링크 제어 전부다.
 *
 * 실행 컨텍스트: probe 와 DWC 코어의 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_init_rc() / uniphier_pcie_start_link() / stop_link()
 *     → [이 함수] → readl() → writel()
 */
static void uniphier_pcie_ltssm_enable(struct uniphier_pcie *pcie,
				       bool enable)
{
	u32 val;

	val = readl(pcie->base + PCL_APP_READY_CTRL);
	/* [한국어] 켜라는 요청이면, */
	if (enable)
		/* [한국어] 비트를 세우고, */
		val |= PCL_APP_LTSSM_ENABLE;
	/* [한국어] 끄라는 요청이면 — */
	else
		val &= ~PCL_APP_LTSSM_ENABLE;
	/* [한국어] 되쓴다. 읽기-수정-쓰기라 같은 레지스터의 다른 비트가 보존된다. */
	writel(val, pcie->base + PCL_APP_READY_CTRL);
}

/* [한국어]
 * uniphier_pcie_init_rc - RC 모드를 지정하고 PERST# 를 걸었다 푼다
 *
 * @pcie: 드라이버 상태.
 *
 * 이 파일의 하드웨어 순서가 모두 여기 있다.
 *
 * 네 단계다.
 * 1. **RC 모드로 설정한다.** 이 SoC 는 EP 로도 동작할 수 있어 명시해야 하며,
 *    REGEN 비트를 세워 소프트웨어 제어를 켠 뒤 REGVAL 을 지워 RC 를 고른다.
 *    같은 짝(REGEN/REGVAL)이 아래 PERST# 제어에도 되풀이된다 — REGEN 이
 *    "이 신호를 레지스터로 제어한다", REGVAL 이 "그 값" 이라는 구조다.
 * 2. 보조 전원 감지를 켠다.
 * 3. **PERST# 를 어서트한다.** 세 신호(출력 값, 출력 허용, 풀다운)의 REGVAL 을
 *    모두 지우고 REGEN 을 세워, 소프트웨어가 그 셋을 쥔 상태로 만든다.
 * 4. LTSSM 을 끄고 100~200ms 기다린 뒤 PERST# 를 푼다.
 *
 * 4번의 대기가 규격이 요구하는 시간이다. 그 사이에 LTSSM 을 꺼 두는 것이
 * 요점으로, 리셋 중에 링크 훈련이 돌면 안 된다.
 *
 * PERST# 를 풀 때 출력 값과 출력 허용만 다시 세우고 풀다운은 건드리지
 * 않는다 — 3번에서 이미 REGEN 을 세워 두었기 때문이다.
 *
 * 반환값이 없다. 레지스터 쓰기만 하므로 실패할 여지가 없다고 본 것이다.
 *
 * 실행 컨텍스트: uniphier_pcie_host_enable() 안. usleep_range 가 있어
 * 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_host_enable() → [이 함수]
 *     → readl()/writel() → uniphier_pcie_ltssm_enable(false) → usleep_range()
 */
static void uniphier_pcie_init_rc(struct uniphier_pcie *pcie)
{
	u32 val;

	/* set RC MODE */
	val = readl(pcie->base + PCL_MODE);
	val |= PCL_MODE_REGEN;
	/* [한국어] 모드 값을 지워 RC 를 고른다 — 세우면 EP 다. */
	val &= ~PCL_MODE_REGVAL;
	/* [한국어] 되쓴다. 이 두 줄이 이 컨트롤러를 루트 컴플렉스로 만든다. */
	writel(val, pcie->base + PCL_MODE);
/* [한국어] 다음은 보조 전원 감지다. */

	/* use auxiliary power detection */
	val = readl(pcie->base + PCL_APP_PM0);
	val |= PCL_SYS_AUX_PWR_DET;
	/* [한국어] 되쓴다. */
	writel(val, pcie->base + PCL_APP_PM0);
/* [한국어] 이제 PERST# 를 어서트한다. */

	/* assert PERST# */
	val = readl(pcie->base + PCL_PINCTRL0);
	val &= ~(PCL_PERST_NOE_REGVAL | PCL_PERST_OUT_REGVAL
		 /* [한국어] 세 신호의 값을 모두 지운다 — 출력 값이 0 이면 PERST# 가 어서트다. */
		 | PCL_PERST_PLDN_REGVAL);
	/* [한국어] 그와 동시에 세 REGEN 을 세워 소프트웨어가 그 셋을 쥔 상태로 만든다. */
	val |= PCL_PERST_NOE_REGEN | PCL_PERST_OUT_REGEN
		/* [한국어] 풀다운까지 포함해 셋을 함께 제어한다. */
		| PCL_PERST_PLDN_REGEN;
	/* [한국어] 되쓴다. 이제 하위 장치가 리셋 상태다. */
	writel(val, pcie->base + PCL_PINCTRL0);
/* [한국어] 설정하는 동안 링크 훈련이 돌면 안 되므로 — */

	uniphier_pcie_ltssm_enable(pcie, false);
/* [한국어] LTSSM 을 꺼 둔다. */

	usleep_range(100000, 200000);
/* [한국어] 규격이 요구하는 시간을 기다린다. 100~200ms 범위를 주어 커널이
 * 다른 타이머와 묶어 처리할 여지를 남긴다. */

	/* deassert PERST# */
	val = readl(pcie->base + PCL_PINCTRL0);
	val |= PCL_PERST_OUT_REGVAL | PCL_PERST_OUT_REGEN;
	/* [한국어] 출력 값과 출력 허용만 다시 세워 PERST# 를 푼다 — 풀다운은
	 * 위에서 이미 REGEN 을 세워 두었으므로 건드리지 않는다. */
	writel(val, pcie->base + PCL_PINCTRL0);
}

/* [한국어]
 * uniphier_pcie_wait_rc - PIPE 클럭이 살아나기를 기다린다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, -ETIMEDOUT.
 *
 * PHY 를 초기화한 뒤 컨트롤러가 실제로 동작하기 시작했는지 확인한다.
 * PIPE 클럭은 PHY 와 컨트롤러 사이의 인터페이스 클럭이라, 그것이 살아 있어야
 * 이후의 링크 훈련이 가능하다.
 *
 * 100ms 간격으로 1초까지 기다린다. 이 파일에서 가장 긴 대기이며,
 * PHY 가 잠기는 데 걸리는 시간을 넉넉히 잡은 값이다.
 *
 * 실패하면 "RC 모드 초기화 실패" 로 기록한다 — 실제로는 PIPE 클럭만
 * 확인한 것이지만, 그것이 살아나지 않으면 RC 로 동작할 수 없기 때문이다.
 *
 * 실행 컨텍스트: uniphier_pcie_host_enable() 안. 폴링 대기가 있어
 * 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 시간이 다하면 기록을 남기고 -ETIMEDOUT.
 *
 * 호출 체인:
 *   uniphier_pcie_host_enable() → [이 함수] → readl_poll_timeout()
 */
static int uniphier_pcie_wait_rc(struct uniphier_pcie *pcie)
{
	u32 status;
	int ret;
/* [한국어] PHY 를 켠 뒤 컨트롤러가 실제로 살아났는지 확인한다. */

	/* wait PIPE clock */
	ret = readl_poll_timeout(pcie->base + PCL_PIPEMON, status,
				 status & PCL_PCLK_ALIVE, 100000, 1000000);
	if (ret) {
		/* [한국어] 실패하면 RC 모드 초기화 실패로 기록한다 — 실제로는 PIPE 클럭만
		 * 확인했지만, 그것이 없으면 RC 로 동작할 수 없다. */
		dev_err(pcie->pci.dev,
			"Failed to initialize controller in RC mode\n");
		return ret;
	}

	return 0;
}

/* [한국어]
 * uniphier_pcie_link_up - 링크가 서 있는지 답한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: true = 링크가 섰다, false = 아니다.
 *
 * DWC 코어가 링크를 기다릴 때 반복해 부르는 콜백이다.
 *
 * **두 비트를 모두** 확인한다. XMLH 는 물리 계층의 링크를, RDLH 는 데이터
 * 링크 계층의 링크를 뜻하며, 둘 다 서야 config 접근이 가능하다. 물리
 * 계층만 선 상태는 훈련이 끝나지 않은 중간 단계다.
 *
 * 동등 비교로 판정하는 것이 이 파일의 방식이다 — 마스크와 AND 한 결과가
 * 마스크와 같아야 참이라, 두 비트가 모두 선 경우만 통과한다.
 *
 * 실행 컨텍스트: DWC 코어의 링크 대기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.link_up == [이 함수] → readl()
 */
static bool uniphier_pcie_link_up(struct dw_pcie *pci)
{
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);
	u32 val, mask;
/* [한국어] 링크 상태를 한 번에 읽는다. */

	val = readl(pcie->base + PCL_STATUS_LINK);
	/* [한국어] 물리 계층과 데이터 링크 계층 두 비트를 모두 본다. */
	mask = PCL_RDLH_LINK_UP | PCL_XMLH_LINK_UP;
/* [한국어] 이제 그 둘이 다 섰는지 확인한다. */

	return (val & mask) == mask;
}

/* [한국어]
 * uniphier_pcie_start_link - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * init_rc 가 꺼 두었던 LTSSM 을 여기서 켠다. 그 사이에 RC 모드 설정과
 * PERST# 조작이 끝나 있으므로, 이 시점에 훈련을 시작해도 안전하다.
 *
 * 한 줄 껍데기이며 실제 동작은 uniphier_pcie_ltssm_enable() 이 한다.
 *
 * 실행 컨텍스트: DWC 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 성공을 답한다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수]
 *     → uniphier_pcie_ltssm_enable(true)
 */
static int uniphier_pcie_start_link(struct dw_pcie *pci)
{
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);

	uniphier_pcie_ltssm_enable(pcie, true);
/* [한국어] init_rc 가 꺼 두었던 LTSSM 을 여기서 켠다 — 그 사이에 모드 설정과
 * PERST# 조작이 끝나 있다. */

	return 0;
}

/* [한국어]
 * uniphier_pcie_stop_link - LTSSM 을 꺼 링크를 내린다
 *
 * @pci: DWC 코어의 문맥.
 *
 * uniphier_pcie_start_link() 의 짝이며 같은 비트를 지운다.
 *
 * 반환값이 없어 실패를 알릴 수 없지만, 아래 함수도 반환값이 없어
 * 알릴 것이 애초에 없다.
 *
 * 실행 컨텍스트: DWC 코어의 정리 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.stop_link == [이 함수]
 *     → uniphier_pcie_ltssm_enable(false)
 */
static void uniphier_pcie_stop_link(struct dw_pcie *pci)
{
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);

	uniphier_pcie_ltssm_enable(pcie, false);
}

/* [한국어]
 * uniphier_pcie_irq_enable - 디버그 이벤트와 INTx 인터럽트를 모두 허용한다
 *
 * @pcie: 드라이버 상태.
 *
 * 두 레지스터에 허용 비트를 통째로 쓴다.
 *
 * **읽기-수정-쓰기가 아니다.** 두 레지스터 모두 허용 비트 말고 상태 비트도
 * 담고 있는데, 통째로 쓰면 그 상태 비트에 값이 쓰인다 — 상태 비트가
 * write-1-to-clear 라면 그 시점의 대기 인터럽트가 지워지는 셈이다.
 * 초기화 시점이라 지울 것이 없다고 본 것으로 보이며, 그 근거는 이 트리에서
 * 확인 못 함.
 *
 * PCL_RCV_INT 는 대역폭 관리·AER·PME 같은 이벤트를, PCL_RCV_INTX 는
 * INTx 넷을 허용한다. 앞의 것은 핸들러에서 디버그 기록으로만 쓰인다.
 *
 * 실행 컨텍스트: host_init 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   uniphier_pcie_host_init() → [이 함수] → writel()
 */
static void uniphier_pcie_irq_enable(struct uniphier_pcie *pcie)
{
	writel(PCL_RCV_INT_ALL_ENABLE, pcie->base + PCL_RCV_INT);
	writel(PCL_RCV_INTX_ALL_ENABLE, pcie->base + PCL_RCV_INTX);
/* [한국어] 허용 비트를 통째로 쓴다. 같은 레지스터의 상태 비트에도 값이 쓰이지만,
 * 초기화 시점이라 지울 것이 없다고 본 것으로 보인다. */
}


/* [한국어]
 * uniphier_pcie_irq_mask - INTx 하나를 막는다
 *
 * @d: 대상 인터럽트. hwirq 가 0~3 이다.
 *
 * PCL_RCV_INTX 의 마스크 비트를 세운다.
 *
 * **hwirq 에 8 을 더하는 것** 이 이 함수의 요점이다. 같은 레지스터가
 * 상태 비트를 0~3 에, 마스크 비트를 8~11 에 담고 있어, 같은 INTx 가
 * 두 자리로 나타난다.
 *
 * DWC 코어의 pp->lock 을 빌려 쓴다. 이 파일이 자기 잠금을 두지 않는 이유는
 * 지켜야 할 것이 이 레지스터 하나뿐이고, 그것을 건드리는 함수가 모두
 * 이 잠금을 잡기 때문이다.
 *
 * raw_spin_lock_irqsave 인 것이 두 가지를 말해 준다 — 인터럽트 문맥에서
 * 잡을 수 있어야 하고(irqsave), PREEMPT_RT 에서도 잠들지 않는 진짜
 * 스핀락이어야 한다(raw).
 *
 * 실행 컨텍스트: 인터럽트 마스킹. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_mask == [이 함수] → readl() → writel()
 */
static void uniphier_pcie_irq_mask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 로 이 드라이버의 상태를 얻는다. */
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);
	/* [한국어] 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 읽기-수정-쓰기 할 값. */
	u32 val;
/* [한국어] 이 레지스터를 인터럽트 문맥에서도 건드리므로 irqsave 판이 필요하다. */

	raw_spin_lock_irqsave(&pp->lock, flags);
/* [한국어] INTx 레지스터를 읽는다. */

	val = readl(pcie->base + PCL_RCV_INTX);
	/* [한국어] **hwirq 에 8 을 더해** 마스크 비트 자리로 옮긴다 — 같은 레지스터가
	 * 상태를 0~3 에, 마스크를 8~11 에 담고 있기 때문이다. */
	val |= BIT(irqd_to_hwirq(d) + PCL_RCV_INTX_MASK_SHIFT);
	/* [한국어] 되쓴다. */
	writel(val, pcie->base + PCL_RCV_INTX);
/* [한국어] 잠금을 놓는다. */

	raw_spin_unlock_irqrestore(&pp->lock, flags);
}

/* [한국어]
 * uniphier_pcie_irq_unmask - INTx 하나를 다시 허용한다
 *
 * @d: 대상 인터럽트.
 *
 * uniphier_pcie_irq_mask() 의 짝이며, 비트를 세우는 대신 지우는 것만 다르다.
 *
 * 잠금과 비트 자리 계산은 마스크 쪽과 같다.
 *
 * 실행 컨텍스트: 인터럽트 마스킹 해제. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_unmask == [이 함수] → readl() → writel()
 */
static void uniphier_pcie_irq_unmask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 로 상태를 얻는다. */
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);
	/* [한국어] 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 읽기-수정-쓰기 할 값. */
	u32 val;
/* [한국어] 마스크 쪽과 같은 잠금을 쓴다. */

	raw_spin_lock_irqsave(&pp->lock, flags);
/* [한국어] INTx 레지스터를 읽는다. */

	val = readl(pcie->base + PCL_RCV_INTX);
	/* [한국어] 같은 자리 계산으로 비트를 지운다. */
	val &= ~BIT(irqd_to_hwirq(d) + PCL_RCV_INTX_MASK_SHIFT);
	/* [한국어] 되쓴다. */
	writel(val, pcie->base + PCL_RCV_INTX);
/* [한국어] 잠금을 놓는다. */

	raw_spin_unlock_irqrestore(&pp->lock, flags);
/* [한국어] 언마스크 끝. */
}

static struct irq_chip uniphier_pcie_irq_chip = {
	/* [한국어] /proc/interrupts 에 나올 칩 이름. */
	.name = "PCI",
	/* [한국어] 이 칩이 제공하는 것은 마스킹 한 쌍뿐이다 — INTx 는 메시지가 없어
	 * compose_msi_msg 가 필요 없고, EOI 도 레벨 트리거라 흐름 처리기가 맡는다. */
	.irq_mask = uniphier_pcie_irq_mask,
	.irq_unmask = uniphier_pcie_irq_unmask,
};

/* [한국어]
 * uniphier_pcie_intx_map - INTx 하나에 칩과 흐름 처리기를 붙인다
 *
 * @domain: INTx 도메인.
 * @irq: 배정된 가상 IRQ 번호.
 * @hwirq: 하드웨어 인터럽트 번호(0~3). 쓰지 않는다.
 * @return: 언제나 0.
 *
 * 도메인이 가상 IRQ 를 처음 만들 때 불려, 그 번호가 어떻게 동작할지를 정한다.
 *
 * handle_level_irq 를 고르는 것이 이 함수의 유일한 판단이다. INTx 가
 * 레벨 트리거이기 때문이며, 그 흐름 처리기는 핸들러를 부르기 전에
 * 마스크하고 끝난 뒤 언마스크한다 — 그것이 위 마스크·언마스크 콜백이
 * 필요한 이유다.
 *
 * chip_data 로 dw_pcie_rp 를 넘긴다. 마스크·언마스크가 그것으로부터
 * 드라이버 상태를 되찾는다.
 *
 * 실행 컨텍스트: 인터럽트 매핑. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq_domain 코어 → irq_domain_ops.map == [이 함수]
 *     → irq_set_chip_and_handler() → irq_set_chip_data()
 */
static int uniphier_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &uniphier_pcie_irq_chip,
				 handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);
/* [한국어] chip_data 로 pp 를 넘긴다 — 마스크·언마스크가 그것으로부터
 * 드라이버 상태를 되찾는다. */

	return 0;
}

static const struct irq_domain_ops uniphier_intx_domain_ops = {
	/* [한국어] 매핑 콜백 하나뿐이다. 선형 도메인이라 별도의 xlate 나 alloc 이 필요 없다. */
	.map = uniphier_pcie_intx_map,
};

/* [한국어]
 * uniphier_pcie_irq_handler - 요약 인터럽트를 디버그 기록과 INTx 로 나눈다
 *
 * @desc: 이 체인 인터럽트의 서술자.
 *
 * 이 SoC 는 여러 종류의 이벤트를 인터럽트 선 하나로 모아 올린다.
 * 이 함수가 그것을 두 갈래로 나눈다.
 *
 * **첫 갈래는 디버그 전용이다**(옆의 상류 주석). 대역폭 관리, 자율 대역폭
 * 변경, AER 루트 오류, PME 네 가지를 확인해 기록만 남기고 지운다.
 * 실제 처리는 하지 않는데, AER 과 PME 는 DWC 코어와 PCI 코어가 자기
 * 경로로 따로 다루기 때문이다.
 *
 * 읽은 값을 그대로 되쓰는 것이 지우는 동작이다. 지우지 않으면 같은
 * 이벤트가 계속 다시 올라온다.
 *
 * **둘째 갈래가 INTx 다.** 상태 비트 넷을 읽어 각각의 핸들러로 갈라 보낸다.
 * chained_irq_enter/exit 이 상위 컨트롤러의 마스킹과 EOI 를 대신한다.
 *
 * 체인 구간이 INTx 쪽만 감싸는 것에 주의할 만하다 — 디버그 기록은 그
 * 바깥에서 이뤄진다.
 *
 * 인터럽트를 지우지 않는다. INTx 는 레벨 트리거라 장치가 신호를 내릴
 * 때까지 비트가 서 있고, 그것을 내리는 것은 장치 드라이버의 몫이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들 수 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   상위 인터럽트 컨트롤러 → [이 함수]
 *     → readl() → chained_irq_enter() → generic_handle_domain_irq()
 *     → chained_irq_exit()
 */
static void uniphier_pcie_irq_handler(struct irq_desc *desc)
{
	struct dw_pcie_rp *pp = irq_desc_get_handler_data(desc);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 로 상태를 얻는다. */
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);
	/* [한국어] 체인 핸들러의 상위 칩. enter/exit 에 넘긴다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 상태 비트를 담을 자리. for_each_set_bit 이 unsigned long 을 요구한다. */
	unsigned long reg;
	/* [한국어] 읽은 값과 처리 중인 비트 번호. */
	u32 val, bit;
/* [한국어] 먼저 디버그용 이벤트를 확인한다(옆의 상류 주석). */

	/* INT for debug */
	val = readl(pcie->base + PCL_RCV_INT);

	if (val & PCL_CFG_BW_MGT_STATUS)
		/* [한국어] 대역폭 관리 이벤트를 기록만 남긴다. */
		dev_dbg(pci->dev, "Link Bandwidth Management Event\n");
	/* [한국어] 자율 대역폭 변경이면, */
	if (val & PCL_CFG_LINK_AUTO_BW_STATUS)
		/* [한국어] 역시 기록만 남긴다. */
		dev_dbg(pci->dev, "Link Autonomous Bandwidth Event\n");
	/* [한국어] AER 루트 오류면, */
	if (val & PCL_CFG_AER_RC_ERR_MSI_STATUS)
		/* [한국어] 기록만 남긴다 — 실제 처리는 PCI 코어의 AER 경로가 한다. */
		dev_dbg(pci->dev, "Root Error\n");
	/* [한국어] PME 면, */
	if (val & PCL_CFG_PME_MSI_STATUS)
		/* [한국어] 기록만 남긴다 — 실제 처리는 PME 서비스가 한다. */
		dev_dbg(pci->dev, "PME Interrupt\n");
/* [한국어] 네 이벤트를 다 확인했다. */

	writel(val, pcie->base + PCL_RCV_INT);
/* [한국어] 읽은 값을 그대로 되써 지운다. 지우지 않으면 같은 이벤트가 반복해 올라온다. */

	/* INTx */
	chained_irq_enter(chip, desc);

	val = readl(pcie->base + PCL_RCV_INTX);
	/* [한국어] 상태 비트 넷을 뽑는다. 마스크 비트(8~11)가 아니라 상태 비트(0~3)다. */
	reg = FIELD_GET(PCL_RCV_INTX_ALL_STATUS, val);
/* [한국어] 이제 선 비트마다 갈라 보낸다. */

	for_each_set_bit(bit, &reg, PCI_NUM_INTX)
		/* [한국어] 그 hwirq 에 등록된 장치 드라이버의 핸들러를 부른다. */
		generic_handle_domain_irq(pcie->intx_irq_domain, bit);
/* [한국어] INTx 처리 끝. 인터럽트를 지우지 않는 것은 레벨 트리거라
 * 장치가 신호를 내릴 때까지 비트가 서 있기 때문이다. */

	chained_irq_exit(chip, desc);
}

/* [한국어]
 * uniphier_pcie_config_intx_irq - INTx 도메인을 만들고 체인 핸들러를 건다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, -EINVAL / -ENODEV.
 *
 * 디바이스 트리의 자식 노드에서 INTx 컨트롤러를 찾아 도메인을 세운다.
 *
 * 그 자식 노드("legacy-interrupt-controller")가 이 SoC 의 트리 관용이다.
 * INTx 컨트롤러를 브리지 노드 안의 별도 노드로 서술하고, 그 노드가
 * 요약 인터럽트를 하나 갖는다.
 *
 * **pp->irq 에 담는 것** 이 요점이다. 그 필드를 DWC 코어도 보므로, 같은
 * 선을 이 파일의 체인 핸들러와 코어가 나눠 쓰는 셈이다.
 *
 * 선형 도메인을 쓰는 것은 hwirq 가 0~3 으로 조밀하기 때문이다.
 *
 * host_data 로 pp 를 넘겨, 위 map 콜백이 그것을 chip_data 로 이어 준다.
 *
 * [상류 코드 관찰] 도메인 생성이 실패하는 경로에서 이미 매핑한 pp->irq 를
 * `irq_dispose_mapping()` 등으로 되돌리지 않는다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * 노드 참조는 모든 경로에서 놓는다 — of_get_child_by_name() 이 참조를
 * 올려 주기 때문이며, goto 라벨 하나로 그것을 모았다.
 *
 * 실행 컨텍스트: host_init 콜백. 프로세스 컨텍스트.
 *
 * 에러 경로: 자식 노드가 없거나 인터럽트 항목이 없으면 -EINVAL,
 * 도메인 생성 실패는 -ENODEV. 각각 기록을 남긴다.
 *
 * 호출 체인:
 *   uniphier_pcie_host_init() → [이 함수]
 *     → of_get_child_by_name() → irq_of_parse_and_map()
 *     → irq_domain_create_linear() → irq_set_chained_handler_and_data()
 */
static int uniphier_pcie_config_intx_irq(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);
	/* [한국어] 이 브리지의 디바이스 트리 노드. */
	struct device_node *np = pci->dev->of_node;
	/* [한국어] 그 안의 INTx 컨트롤러 자식 노드. */
	struct device_node *np_intc;
	/* [한국어] 각 단계의 결과. 0 으로 시작해 정리 라벨을 지나도 성공이 유지된다. */
	int ret = 0;
/* [한국어] 자식 노드를 먼저 찾는다. */

	np_intc = of_get_child_by_name(np, "legacy-interrupt-controller");
	/* [한국어] 없으면 — 이 SoC 의 트리 관용을 따르지 않는 트리다. */
	if (!np_intc) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(pci->dev, "Failed to get legacy-interrupt-controller node\n");
		/* [한국어] 잘못된 인자로 답한다. 여기서는 아직 참조를 올리지 않아 곧바로 돌아간다. */
		return -EINVAL;
	}

	pp->irq = irq_of_parse_and_map(np_intc, 0);
	/* [한국어] 그 노드의 인터럽트 항목이 없으면, */
	if (!pp->irq) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(pci->dev, "Failed to get an IRQ entry in legacy-interrupt-controller\n");
		/* [한국어] 잘못된 인자로 기록한 뒤, */
		ret = -EINVAL;
		goto out_put_node;
	}

	pcie->intx_irq_domain = irq_domain_create_linear(of_fwnode_handle(np_intc), PCI_NUM_INTX,
						/* [한국어] host_data 로 pp 를 넘기면 map 콜백이 그것을 chip_data 로 이어 준다. */
						&uniphier_intx_domain_ops, pp);
	if (!pcie->intx_irq_domain) {
		/* [한국어] 도메인을 만들지 못했으면 그 사실을 남기고, */
		dev_err(pci->dev, "Failed to get INTx domain\n");
		/* [한국어] 장치 없음으로 기록한 뒤 정리 라벨로 간다. */
		ret = -ENODEV;
		goto out_put_node;
	}

	irq_set_chained_handler_and_data(pp->irq, uniphier_pcie_irq_handler,
					 /* [한국어] 체인 핸들러에 pp 를 문맥으로 넘긴다. */
					 pp);

out_put_node:
	of_node_put(np_intc);
	return ret;
}

/* [한국어]
 * uniphier_pcie_host_init - INTx 도메인을 세우고 인터럽트를 허용한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 파일이 DWC 코어와 만나는 유일한 콜백이다.
 *
 * 두 단계뿐인 것이 이 SoC 의 특징을 말해 준다 — 하드웨어 초기화가 모두
 * probe 의 uniphier_pcie_host_enable() 에서 끝나 있어, 코어가 물어볼
 * 시점에는 INTx 준비만 남는다.
 *
 * 순서가 정해져 있다. 도메인을 만들고 핸들러를 건 **뒤** 에 인터럽트를
 * 허용하는데, 반대로 하면 갈라 보낼 도메인이 없는 상태에서 인터럽트가
 * 올라온다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 도메인 생성이 실패하면 그 오류를 올려보내고, DWC 코어가
 * 그것을 probe 실패로 전한다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [이 함수]
 *     → uniphier_pcie_config_intx_irq() → uniphier_pcie_irq_enable()
 */
static int uniphier_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct uniphier_pcie *pcie = to_uniphier_pcie(pci);
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 INTx 도메인을 세운다. */

	ret = uniphier_pcie_config_intx_irq(pp);
	/* [한국어] 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다 — 인터럽트를 허용하기 전이라 되돌릴 것이 없다. */
		return ret;

	uniphier_pcie_irq_enable(pcie);

	return 0;
}

static const struct dw_pcie_host_ops uniphier_pcie_host_ops = {
	/* [한국어] 하드웨어 초기화가 probe 에서 끝나 있어, 코어가 물어볼 것은 INTx 준비뿐이다. */
	.init = uniphier_pcie_host_init,
};

/* [한국어]
 * uniphier_pcie_host_enable - 클럭·리셋·PHY 를 순서대로 켜고 컨트롤러를 깨운다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 하드웨어를 동작 상태로 만드는 전 과정이며, 순서가 전부다.
 *
 * 다섯 단계다.
 * 1. 클럭을 켠다.
 * 2. 리셋을 푼다 — 클럭이 있어야 리셋 해제가 의미를 갖는다.
 * 3. RC 모드를 지정하고 PERST# 를 조작한다.
 * 4. PHY 를 초기화한다.
 * 5. PIPE 클럭이 살아나기를 기다린다.
 *
 * 3번이 4번보다 **앞** 인 것이 눈에 띈다. PERST# 를 푼 뒤에 PHY 를 켜는
 * 셈인데, 그 순서의 근거는 이 트리에서 확인 못 함.
 *
 * 되감기가 계단이며 세 라벨이 있다. 각 진입점이 그 지점까지 성공한 것만
 * 정확히 되돌린다.
 *
 * 실행 컨텍스트: probe. 대기가 여럿 있어 프로세스 컨텍스트여야 하며
 * 100ms 이상 걸린다.
 *
 * 에러 경로: 각 단계의 실패를 goto 로 해당 라벨에 넣어 역순으로 되감는다.
 *
 * 호출 체인:
 *   uniphier_pcie_probe() → [이 함수]
 *     → clk_prepare_enable() → reset_control_deassert()
 *     → uniphier_pcie_init_rc() → phy_init() → uniphier_pcie_wait_rc()
 */
static int uniphier_pcie_host_enable(struct uniphier_pcie *pcie)
{
	int ret;

	ret = clk_prepare_enable(pcie->clk);
	/* [한국어] 클럭을 켜지 못하면, */
	if (ret)
		/* [한국어] 그대로 물러난다. */
		return ret;

	ret = reset_control_deassert(pcie->rst);
	/* [한국어] 리셋을 풀지 못하면, */
	if (ret)
		/* [한국어] 클럭을 되돌린다. */
		goto out_clk_disable;

	uniphier_pcie_init_rc(pcie);

	ret = phy_init(pcie->phy);
	/* [한국어] PHY 초기화가 실패하면, */
	if (ret)
		/* [한국어] 리셋부터 되돌린다. */
		goto out_rst_assert;

	ret = uniphier_pcie_wait_rc(pcie);
	/* [한국어] PIPE 클럭이 살아나지 않으면, */
	if (ret)
		/* [한국어] PHY 부터 되돌린다. */
		goto out_phy_exit;

	return 0;

out_phy_exit:
	phy_exit(pcie->phy);
out_rst_assert:
	reset_control_assert(pcie->rst);
out_clk_disable:
	clk_disable_unprepare(pcie->clk);

	return ret;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크 시작과, */
	.start_link = uniphier_pcie_start_link,
	/* [한국어] 중단, 그리고 링크 판정 셋이 이 SoC 가 DWC 표준에서 벗어나는 부분이다. */
	.stop_link = uniphier_pcie_stop_link,
	.link_up = uniphier_pcie_link_up,
};

/* [한국어]
 * uniphier_pcie_probe - 자원을 얻고 하드웨어를 깨운 뒤 DWC 호스트를 초기화한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * 자원 넷을 얻는다 — "link" 창, 클럭, 리셋, 그리고 **선택적** PHY.
 * PHY 만 optional 판인데, PHY 가 없는 보드에서는 그 초기화가 무동작이 된다.
 *
 * 리셋을 shared 판으로 얻는 것이 눈에 띈다. 이 리셋 선을 다른 블록과
 * 공유한다는 뜻이라, 이 드라이버가 풀어도 다른 쪽이 걸어 두면 걸린 채로 남는다.
 *
 * drvdata 를 하드웨어 초기화 **앞** 에 매단다. 이 파일의 변환 매크로가
 * drvdata 를 거치는데, uniphier_pcie_init_rc() 가 그것을 쓰기 때문이다.
 *
 * 호스트 콜백 표를 하드웨어 초기화 **뒤** 에 거는 순서도 눈에 띈다 —
 * 코어를 부르기 직전에만 있으면 되므로 어디든 상관없지만, 하드웨어가
 * 준비된 뒤에 소프트웨어 연결을 잇는 흐름으로 읽힌다.
 *
 * [상류 코드 관찰] `dw_pcie_host_init()` 이 실패해도
 * uniphier_pcie_host_enable() 이 켜 둔 클럭·리셋·PHY 를 되돌리지 않는다.
 * 이 파일에 remove 콜백도 없어, 그 상태가 그대로 남는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 링크 대기와
 * 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 자원 확보 실패는 그 오류를 그대로 올려보낸다. 되감기가 없는
 * 것은 잡는 자원이 모두 devm 판이기 때문이며, 켜 둔 하드웨어는 위 관찰 참조.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_platform_ioremap_resource_byname() → devm_clk_get()
 *     → devm_reset_control_get_shared() → devm_phy_optional_get()
 *     → uniphier_pcie_host_enable() → dw_pcie_host_init()
 */
static int uniphier_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphier_pcie *pcie;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 상태 구조를 잡는다. */

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 잡지 못하면, */
	if (!pcie)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	pcie->pci.dev = dev;
	/* [한국어] 코어 콜백 표를 건다. */
	pcie->pci.ops = &dw_pcie_ops;
/* [한국어] 이제 자원을 얻는다. */

	pcie->base = devm_platform_ioremap_resource_byname(pdev, "link");
	/* [한국어] "link" 창 매핑이 실패하면, */
	if (IS_ERR(pcie->base))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pcie->base);

	pcie->clk = devm_clk_get(dev, NULL);
	/* [한국어] 클럭을 얻지 못하면, */
	if (IS_ERR(pcie->clk))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pcie->clk);

	pcie->rst = devm_reset_control_get_shared(dev, NULL);
	/* [한국어] 리셋을 얻지 못하면, */
	if (IS_ERR(pcie->rst))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pcie->rst);

	pcie->phy = devm_phy_optional_get(dev, "pcie-phy");
	/* [한국어] PHY 조회가 실패하면 — **선택 사항이라 없는 것은 실패가 아니다.**
	 * optional 판이 그 경우 NULL 을 돌려주므로, 여기 걸리는 것은 진짜 오류뿐이다. */
	if (IS_ERR(pcie->phy))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pcie->phy);

	platform_set_drvdata(pdev, pcie);
/* [한국어] 이 파일의 변환 매크로가 drvdata 를 거치므로,
 * 그것을 쓰는 uniphier_pcie_init_rc() 보다 먼저 매달아야 한다. */

	ret = uniphier_pcie_host_enable(pcie);
	/* [한국어] 하드웨어를 깨우지 못하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	pcie->pci.pp.ops = &uniphier_pcie_host_ops;
/* [한국어] 호스트 콜백 표를 건다. */

	return dw_pcie_host_init(&pcie->pci.pp);
/* [한국어] 이 뒤로는 코어가 host_init 을 부르고 링크 훈련과 버스 스캔을 진행한다. */
}

static const struct of_device_id uniphier_pcie_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다. */
	{ .compatible = "socionext,uniphier-pcie", },
	/* [한국어] 표의 끝 표시. */
	{ /* sentinel */ },
};

static struct platform_driver uniphier_pcie_driver = {
	/* [한국어] probe 콜백. **remove 가 없다** — builtin 으로 등록되어 뗄 수 없다. */
	.probe  = uniphier_pcie_probe,
	/* [한국어] 드라이버 정보. */
	.driver = {
		.name = "uniphier-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = uniphier_pcie_match,
	},
};
builtin_platform_driver(uniphier_pcie_driver);
