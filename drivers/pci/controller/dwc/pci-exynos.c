// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Samsung Exynos SoCs
 *
 * Copyright (C) 2013-2020 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 *	   Jaehoon Chung <jh80.chung@samsung.com>
 */

/*
 * [한국어 설명] Samsung Exynos5433 의 DesignWare PCIe 호스트 글루 (pci-exynos.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 Samsung Exynos SoC 에 붙이는 글루
 * 드라이버다. config 접근, ATU 설정, 버스 스캔은 pcie-designware-host.c 가
 * 하고, 이 파일은 DWC 코어가 알 수 없는 SoC 고유의 것만 맡는다.
 *
 * 이 파일도 **DBI 접근에 사이드밴드가 필요한** 부류다 — 같은 트리의
 * pcie-histb.c 와 같은 문제를 푼다. 다만 두 가지가 다르다.
 *   - 사이드밴드 비트가 드라이버 전용 창이 아니라 **DWC 코어가 아는
 *     elbi_base 창** 에 있다. 그래서 이 파일에는 자기 레지스터 창이 없고,
 *     모든 접근이 pci->elbi_base 를 기준으로 이뤄진다.
 *   - 읽기용은 ARMISC(AXI read misc), 쓰기용은 AWMISC(AXI write misc)로,
 *     이름이 AXI 채널을 그대로 딴다. histb 가 SYS_CTRL0/1 이라는 일반적인
 *     이름을 쓰는 것과 대비된다.
 *
 * 맡는 일이 다섯이다.
 *   1) DBI 사이드밴드 감싸기.
 *   2) 루트 포트 자신의 config 접근 — 슬롯 0 만 받아들인다.
 *   3) 코어 리셋 걸기·풀기. 푸는 쪽은 APP_INIT_RESET 을 1 로 썼다 0 으로
 *      되돌리는 펄스까지 포함한다.
 *   4) INTx 펄스 인터럽트 처리 — 다만 실제로 하는 일은 원인 비트를 지우는
 *      것뿐이다.
 *   5) 절전 진입·복귀 한 쌍.
 *
 * **MSI 를 쓰지 않는다.** exynos_add_pcie_port() 가 pp->msi_irq[0] 에
 * -ENODEV 를 넣어 DWC 코어의 MSI 처리를 꺼 버리므로, 이 브리지 아래
 * 장치들은 INTx 로만 인터럽트를 낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> exynos_pcie_probe()
 *     -> PHY, 클럭 전부, 전원 둘을 얻고 전원을 켠다
 *     -> exynos_add_pcie_port()
 *        -> 인터럽트를 걸고 MSI 를 끈 뒤 dw_pcie_host_init()
 *           -> 그 안에서 콜백 -> [이 파일] exynos_pcie_host_init()
 *              -> 루트 버스 ops 교체 -> 코어 리셋 -> PHY -> 리셋 해제
 *                 -> INTx 허용
 *           -> 코어가 start_link 콜백을 부른다 -> [이 파일] LTSSM 켜기
 *           -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * config 접근이 두 갈래로 갈린다:
 *   루트 포트 자신 -> exynos_pcie_rd/wr_own_conf() -> dw_pcie_read/write_dbi()
 *     -> [이 파일] exynos_pcie_read/write_dbi() -> 사이드밴드 켜기-접근-끄기
 *   하위 장치     -> DWC 코어의 기본 경로
 *
 * 절전 복귀가 특이하다 — exynos_pcie_resume_noirq() 가 host_init 콜백을
 * **직접** 부르고 이어서 dw_pcie_setup_rc() 와 start_link 까지 손수 밟는다.
 * 코어에 맡기지 않고 초기화 순서를 그대로 재현하는 셈이다.
 *
 * 실행 컨텍스트: probe 와 host_init 은 프로세스 컨텍스트,
 * exynos_pcie_irq_handler() 는 인터럽트 문맥이다. DBI 접근 경로는 PCI
 * 코어가 잠금을 쥔 채 부를 수 있어 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c. 접점이 두 벌의
 *   콜백 표다 — dw_pcie_ops(read_dbi, write_dbi, link_up, start_link)와
 *   dw_pcie_host_ops(init 하나). elbi_base 를 코어와 나눠 쓴다.
 * 옆쪽: phy·clk·regulator 계층. 모두 이 트리에 없어 내부는 확인 대상
 *   밖이며, 이 파일에서 읽히는 호출 규약까지만 적었다.
 *
 * 데이터 흐름:
 *   디바이스 트리(PHY, 클럭 전부, vdd18/vdd10 전원, 인터럽트) -> probe
 *     -> struct exynos_pcie
 *   DBI 접근: 사이드밴드 켜기(elbi) -> 실제 접근(DBI) -> 끄기(elbi)
 *
 * 공유 상태: struct exynos_pcie 하나. probe 후 불변이며 잠금이 없다 —
 *   사이드밴드 켜기-끄기 쌍의 원자성은 PCI 코어의 잠금에 기댄다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면 **MSI 를 쓸 수 없다.** 위에
 * 적었듯 이 드라이버가 DWC 의 MSI 처리를 꺼 두므로 INTx 로만 인터럽트를
 * 받게 되며, NVMe 의 큐별 벡터 분산이 성립하지 않아 성능이 크게 제한된다.
 *
 * === 주요 함수/구조체 요약 ===
 * exynos_pcie_read_dbi()/write_dbi()      : 사이드밴드를 켜고 접근한 뒤 끈다.
 * exynos_pcie_sideband_dbi_r/w_mode()     : 그 비트를 다룬다. 읽기는 ARMISC,
 *                                           쓰기는 AWMISC 다.
 * exynos_pcie_assert/deassert_core_reset(): 코어 리셋을 걸고 푼다.
 * exynos_pcie_host_init()                 : 초기화 순서 전부. 복귀 경로도
 *                                           이것을 직접 부른다.
 * exynos_pcie_resume_noirq()              : 초기화 순서를 손수 재현한다.
 * struct exynos_pcie                      : dw_pcie 를 맨 앞에 둔 상태 구조체.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regulator/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

#include "pcie-designware.h"

#define to_exynos_pcie(x)	dev_get_drvdata((x)->dev)

/* PCIe ELBI registers */
#define PCIE_IRQ_PULSE			0x000
#define IRQ_INTA_ASSERT			BIT(0)
#define IRQ_INTB_ASSERT			BIT(2)
#define IRQ_INTC_ASSERT			BIT(4)
#define IRQ_INTD_ASSERT			BIT(6)
#define PCIE_IRQ_LEVEL			0x004
#define PCIE_IRQ_SPECIAL		0x008
#define PCIE_IRQ_EN_PULSE		0x00c
#define PCIE_IRQ_EN_LEVEL		0x010
#define PCIE_IRQ_EN_SPECIAL		0x014
#define PCIE_SW_WAKE			0x018
#define PCIE_BUS_EN			BIT(1)
#define PCIE_CORE_RESET			0x01c
#define PCIE_CORE_RESET_ENABLE		BIT(0)
#define PCIE_STICKY_RESET		0x020
#define PCIE_NONSTICKY_RESET		0x024
#define PCIE_APP_INIT_RESET		0x028
#define PCIE_APP_LTSSM_ENABLE		0x02c
#define PCIE_ELBI_RDLH_LINKUP		0x074
#define PCIE_ELBI_XMLH_LINKUP		BIT(4)
#define PCIE_ELBI_LTSSM_ENABLE		0x1
#define PCIE_ELBI_SLV_AWMISC		0x11c
#define PCIE_ELBI_SLV_ARMISC		0x120
#define PCIE_ELBI_SLV_DBI_ENABLE	BIT(21)

struct exynos_pcie {
	/* [한국어] DWC PCIe 코어의 공통 문맥. **구조체의 맨 앞** 에 두는 것이 규약이라,
	 * 이 주소가 곧 exynos_pcie 의 주소가 된다.
	 * 설정자: probe 가 dev 와 ops 를 채우고, 나머지는 DWC 코어가 채운다.
	 * 읽는 자: 이 파일의 거의 모든 함수. 특히 pci->elbi_base 를 통해
	 * 레지스터에 닿는다 — 이 드라이버에 자기 창이 없기 때문이다.
	 * 값 범위: 언제나 유효한 내장 구조체이며 포인터가 아니다.
	 * 동기화: probe 이후 이 파일이 바꾸는 필드가 없다. */
	struct dw_pcie			pci;
	/* [한국어] 이 컨트롤러에 딸린 클럭 전부의 목록.
	 * 설정자: probe 의 devm_clk_bulk_get_all_enabled() 가 배열을 할당해 채우고
	 * 동시에 전부 켠다 — 얻기와 켜기가 한 호출에 묶여 있다.
	 * 읽는 자: 없다. 이 파일 어디에서도 이 필드를 다시 읽지 않는다.
	 * 클럭을 끄는 일까지 devm 이 맡기 때문이다.
	 * 값 범위: 유효한 배열 포인터. 개수는 디바이스 트리가 정하며 이 파일은
	 * 그 수를 알지도, 알 필요도 없다.
	 * 동기화: 필요 없다. */
	struct clk_bulk_data		*clks;
	/* [한국어] PCIe 물리 계층 PHY 핸들.
	 * 설정자: probe 의 devm_of_phy_get() 이 디바이스 트리 노드에서 얻는다.
	 * 이름 없이(NULL) 얻으므로 노드에 PHY 가 하나뿐이라는 전제다.
	 * 읽는 자: host_init 이 켜고, remove 와 절전 진입이 끈다.
	 * 값 범위: 유효한 phy 포인터. 실패는 probe 에서 걸러진다.
	 * 동기화: 켜고 끄는 짝이 초기화·해제·절전 경로에만 있어 겹치지 않는다. */
	struct phy			*phy;
	/* [한국어] 이 컨트롤러가 쓰는 전원 둘의 묶음.
	 * 설정자: probe 가 이름을 직접 적어 넣은 뒤 devm_regulator_bulk_get() 으로
	 * 핸들을 채운다 — vdd18(1.8V)과 vdd10(1.0V)이다.
	 * 읽는 자: probe 가 켜고, remove 와 절전 진입이 끄고, 절전 복귀가 다시 켠다.
	 * 값 범위: 원소 둘 고정. 이 SoC 가 하나뿐이라 개수를 표로 빼지 않았다.
	 * 동기화: 켜고 끄는 경로가 초기화·해제·절전에만 있어 겹치지 않는다. */
	struct regulator_bulk_data	supplies[2];
};

/* [한국어]
 * exynos_pcie_writel - 주어진 창의 레지스터에 쓴다
 *
 * @base: 창의 기준 주소. 이 파일에서는 언제나 pci->elbi_base 다.
 * @val: 쓸 값.
 * @reg: 그 창 안의 오프셋.
 *
 * **창을 인자로 받는 것** 이 이 파일의 특징이다. histb 판이 드라이버 상태를
 * 받아 자기 창을 쓰는 것과 달리, 여기서는 창이 DWC 코어의 elbi_base 라
 * 그것을 호출부가 넘긴다.
 *
 * 그 덕분에 이 파일에는 자기 레지스터 창이 없다 — 구조체에 iomem 필드가
 * 하나도 없다.
 *
 * 인자 순서가 커널 관용(값, 주소)과 섞여 있다 — 값이 먼저이고 오프셋이 뒤다.
 *
 * 실행 컨텍스트: config 접근 경로, probe, 인터럽트 문맥. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → writel()
 */
static void exynos_pcie_writel(void __iomem *base, u32 val, u32 reg)
{
	writel(val, base + reg);
}

/* [한국어]
 * exynos_pcie_readl - 주어진 창의 레지스터를 읽는다
 *
 * @base: 창의 기준 주소.
 * @reg: 그 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * exynos_pcie_writel() 의 짝이며 같은 규약을 따른다.
 *
 * 실행 컨텍스트: config 접근 경로, probe, 인터럽트 문맥. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → readl()
 */
static u32 exynos_pcie_readl(void __iomem *base, u32 reg)
{
	return readl(base + reg);
}

/* [한국어]
 * exynos_pcie_sideband_dbi_w_mode - DBI **쓰기** 사이드밴드를 켜거나 끈다
 *
 * @ep: 드라이버 상태.
 * @on: true 면 켠다.
 *
 * 이 하드웨어는 DBI 창에 그냥 쓸 수 없다. AXI 쓰기 채널의 misc 신호를
 * 먼저 켜야 그 쓰기가 DBI 접근으로 해석된다.
 *
 * 레지스터 이름이 **AWMISC** — AXI Write MISC 다. 사이드밴드가 AXI 채널
 * 단위로 나뉘어 있음을 이름이 그대로 말해 준다. 읽기용은 ARMISC 로 따로 있다.
 *
 * 읽기-수정-쓰기가 필수다. 같은 레지스터의 다른 AXI 신호를 보존해야 한다.
 *
 * 실행 컨텍스트: DBI 쓰기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   exynos_pcie_write_dbi() → [이 함수]
 *     → exynos_pcie_readl() → exynos_pcie_writel()
 */
static void exynos_pcie_sideband_dbi_w_mode(struct exynos_pcie *ep, bool on)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;
/* [한국어] 사이드밴드 비트를 담은 레지스터의 현재 값을 담을 자리. */

	val = exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_SLV_AWMISC);
	/* [한국어] 켜는 요청이면 비트를 세우고, 끄는 요청이면 지운다. */
	if (on)
		/* [한국어] DBI 접근 허용 비트를 세운다 — 이 뒤의 AXI 쓰기가 DBI 로 해석된다. */
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 끄는 요청이다. 접근이 끝난 뒤 원래대로 되돌리는 쪽이다. */
	else
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 고친 값을 되쓴다. 이 쓰기가 끝나야 다음 DBI 접근이 의도대로 간다. */
	exynos_pcie_writel(pci->elbi_base, val, PCIE_ELBI_SLV_AWMISC);
}

/* [한국어]
 * exynos_pcie_sideband_dbi_r_mode - DBI **읽기** 사이드밴드를 켜거나 끈다
 *
 * @ep: 드라이버 상태.
 * @on: true 면 켠다.
 *
 * 위 쓰기 판의 읽기 짝이며, 건드리는 레지스터가 ARMISC(AXI Read MISC)로
 * 다르다는 것만 차이다.
 *
 * 같은 비트 이름(ELBI_SLV_DBI_ENABLE)이 두 레지스터에 각각 존재한다.
 *
 * 실행 컨텍스트: DBI 읽기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   exynos_pcie_read_dbi() → [이 함수]
 *     → exynos_pcie_readl() → exynos_pcie_writel()
 */
static void exynos_pcie_sideband_dbi_r_mode(struct exynos_pcie *ep, bool on)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;
/* [한국어] 위 쓰기 판과 같은 자리이며, 읽는 레지스터만 ARMISC 로 다르다. */

	val = exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_SLV_ARMISC);
	/* [한국어] 켜는 요청이면 비트를 세우고, 끄는 요청이면 지운다. */
	if (on)
		/* [한국어] 같은 이름의 비트지만 이쪽은 ARMISC 레지스터 안의 것이다. */
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 끄는 요청이다. */
	else
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	/* [한국어] 고친 값을 ARMISC 로 되쓴다. */
	exynos_pcie_writel(pci->elbi_base, val, PCIE_ELBI_SLV_ARMISC);
}

/* [한국어]
 * exynos_pcie_assert_core_reset - 코어 리셋을 걸고 sticky/non-sticky 를 지운다
 *
 * @ep: 드라이버 상태.
 *
 * 세 레지스터를 건드린다 — 코어 리셋 활성 비트를 지우고, sticky 와
 * non-sticky 리셋을 각각 0 으로 쓴다.
 *
 * **sticky 와 non-sticky 가 나뉜 이유** 는 PCIe 규격에 있다. sticky 레지스터는
 * 일반 리셋으로 지워지지 않고 전원이 끊겨야 지워지는 상태를 담으며,
 * 링크 훈련 이력 같은 정보가 그에 해당한다.
 *
 * 코어 리셋만 읽기-수정-쓰기이고 나머지 둘은 통째로 쓴다 — 그 두 레지스터에
 * 다른 비트를 이 드라이버가 쓰지 않는다는 전제다.
 *
 * remove 와 절전 진입에서도 불려, "이 컨트롤러를 정지 상태로 두는" 공통
 * 동작 노릇을 한다.
 *
 * 실행 컨텍스트: host_init, remove, 절전 진입. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   exynos_pcie_host_init() / exynos_pcie_remove() / suspend_noirq()
 *     → [이 함수] → exynos_pcie_readl() → exynos_pcie_writel()
 */
static void exynos_pcie_assert_core_reset(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;
/* [한국어] 코어 리셋 레지스터의 현재 값을 담을 자리. */

	val = exynos_pcie_readl(pci->elbi_base, PCIE_CORE_RESET);
	/* [한국어] 코어 리셋 활성 비트를 **지운다.** 이 비트가 서 있어야 코어가 도는
	 * 구조라, 지우는 것이 곧 리셋을 거는 것이다. */
	val &= ~PCIE_CORE_RESET_ENABLE;
	/* [한국어] 리셋을 건다. */
	exynos_pcie_writel(pci->elbi_base, val, PCIE_CORE_RESET);
	/* [한국어] sticky 리셋을 0 으로 쓴다. sticky 는 PCIe 규격상 일반 리셋으로
	 * 지워지지 않는 상태를 담는 영역이다. */
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_STICKY_RESET);
	/* [한국어] non-sticky 리셋도 0 으로 쓴다. 일반 리셋으로 지워지는 쪽이다. */
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_NONSTICKY_RESET);
}

/* [한국어]
 * exynos_pcie_deassert_core_reset - 코어 리셋을 풀고 앱 초기화 펄스를 낸다
 *
 * @ep: 드라이버 상태.
 *
 * exynos_pcie_assert_core_reset() 의 짝이지만 **완전한 대칭은 아니다.**
 *
 * 앞의 셋은 대칭이다 — 코어 리셋 활성 비트를 세우고, sticky 와 non-sticky 를
 * 1 로 쓴다.
 *
 * 넷째와 다섯째가 추가된 것이다. APP_INIT_RESET 에 1 을 썼다가 곧바로 0 을
 * 써 **펄스** 를 만드는데, 애플리케이션 계층을 한 번 초기화시키는 신호로
 * 보인다. 그 펄스의 근거는 이 트리에서 확인 못 함.
 *
 * 두 쓰기 사이에 지연이 없다. 하드웨어가 그 폭으로도 인식한다는 전제다.
 *
 * 실행 컨텍스트: host_init. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   exynos_pcie_host_init() → [이 함수]
 *     → exynos_pcie_readl() → exynos_pcie_writel()
 */
static void exynos_pcie_deassert_core_reset(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;
/* [한국어] 코어 리셋 레지스터의 현재 값을 담을 자리. */

	val = exynos_pcie_readl(pci->elbi_base, PCIE_CORE_RESET);
	/* [한국어] 코어 리셋 활성 비트를 세운다 — 리셋을 푸는 쪽이다. */
	val |= PCIE_CORE_RESET_ENABLE;

	exynos_pcie_writel(pci->elbi_base, val, PCIE_CORE_RESET);
	/* [한국어] sticky 리셋을 1 로 쓴다. 거는 쪽의 0 과 대칭이다. */
	exynos_pcie_writel(pci->elbi_base, 1, PCIE_STICKY_RESET);
	/* [한국어] non-sticky 리셋도 1 로 쓴다. */
	exynos_pcie_writel(pci->elbi_base, 1, PCIE_NONSTICKY_RESET);
	/* [한국어] 여기부터가 비대칭이다. 애플리케이션 초기화 리셋을 1 로 세운다.
	 * 근거는 이 트리에서 확인 못 함. */
	exynos_pcie_writel(pci->elbi_base, 1, PCIE_APP_INIT_RESET);
	/* [한국어] 곧바로 0 으로 되돌려 **펄스** 를 만든다. 두 쓰기 사이에 지연이 없어,
	 * 하드웨어가 이 폭으로도 인식한다는 전제다. */
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_APP_INIT_RESET);
}

/* [한국어]
 * exynos_pcie_start_link - 버스 활성 비트를 지우고 LTSSM 을 켠다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * 두 단계를 밟는다.
 *
 * 1. SW_WAKE 레지스터의 BUS_EN 비트를 **지운다.** 이름과 동작이 어긋나
 *    보이는데, 그 비트의 의미와 왜 지워야 링크가 서는지는 이 트리에서
 *    확인 못 함.
 * 2. LTSSM 활성 값을 쓴다.
 *
 * 두 번째가 통째 쓰기다 — 그 레지스터가 LTSSM 제어 전용이라는 전제다.
 *
 * **끄는 짝이 없다.** dw_pcie_ops 에 stop_link 를 두지 않아, 한 번 켠
 * LTSSM 을 이 드라이버가 끌 방법이 없다. 정지는 대신
 * exynos_pcie_assert_core_reset() 이 코어 리셋으로 처리한다.
 *
 * 절전 복귀 경로가 이 함수를 **직접** 부른다는 점도 눈여겨볼 만하다.
 *
 * 실행 컨텍스트: DWC 코어의 초기화와 절전 복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 / exynos_pcie_resume_noirq()
 *     → dw_pcie_ops.start_link == [이 함수]
 *     → exynos_pcie_readl() → exynos_pcie_writel()
 */
static int exynos_pcie_start_link(struct dw_pcie *pci)
{
	u32 val;

	val = exynos_pcie_readl(pci->elbi_base, PCIE_SW_WAKE);
	/* [한국어] BUS_EN 비트를 **지운다.** 이름과 동작이 어긋나 보이나, 그 비트의
	 * 의미는 이 트리에서 확인 못 함. */
	val &= ~PCIE_BUS_EN;
	/* [한국어] 고친 값을 되쓴다. 이것이 LTSSM 을 켜기 전의 준비 단계다. */
	exynos_pcie_writel(pci->elbi_base, val, PCIE_SW_WAKE);

	/* assert LTSSM enable */
	exynos_pcie_writel(pci->elbi_base, PCIE_ELBI_LTSSM_ENABLE,
			  PCIE_APP_LTSSM_ENABLE);
	return 0;
}

/* [한국어]
 * exynos_pcie_clear_irq_pulse - 대기 중인 펄스 인터럽트 원인을 지운다
 *
 * @ep: 드라이버 상태.
 *
 * 읽은 값을 그대로 되쓰는 write-1-to-clear 동작이다. 지우지 않으면
 * 같은 인터럽트가 계속 다시 올라온다.
 *
 * **지우는 것 말고는 아무것도 하지 않는다.** 이 컨트롤러가 INTx 를 래치만
 * 해 두고, 실제 처리는 장치 드라이버가 자기 경로로 하기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   exynos_pcie_irq_handler() → [이 함수]
 *     → exynos_pcie_readl() → exynos_pcie_writel()
 */
static void exynos_pcie_clear_irq_pulse(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;

	/* [한국어] 대기 중인 원인 비트들을 읽는다. 선언과 대입을 한 줄에 붙였다. */
	u32 val = exynos_pcie_readl(pci->elbi_base, PCIE_IRQ_PULSE);

	/* [한국어] 읽은 값을 그대로 되쓴다 — write-1-to-clear 다. 서 있던 비트만 지워지고
	 * 그 사이에 새로 선 비트는 건드리지 않아, 인터럽트를 잃지 않는다. */
	exynos_pcie_writel(pci->elbi_base, val, PCIE_IRQ_PULSE);
}

/* [한국어]
 * exynos_pcie_irq_handler - 펄스 인터럽트를 받아 원인만 지운다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: 등록 시 넘겨 둔 드라이버 상태.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 한 줄 껍데기이며 실제 동작은 위 함수가 한다.
 *
 * [상류 코드 관찰] IRQF_SHARED 로 등록되는데도 언제나 IRQ_HANDLED 를
 * 돌려준다. 자기 원인 비트가 하나도 서 있지 않아도 그렇게 답하므로,
 * 같은 선을 쓰는 다른 핸들러에게 차례가 넘어가지 않는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들 수 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 → [이 함수] → exynos_pcie_clear_irq_pulse()
 */
static irqreturn_t exynos_pcie_irq_handler(int irq, void *arg)
{
	struct exynos_pcie *ep = arg;

	exynos_pcie_clear_irq_pulse(ep);
	return IRQ_HANDLED;
}

/* [한국어]
 * exynos_pcie_enable_irq_pulse - INTx 펄스만 허용하고 나머지는 막는다
 *
 * @ep: 드라이버 상태.
 *
 * 세 종류의 인터럽트 허용 레지스터를 한 번에 정한다.
 *
 * - 펄스: INTA~INTD 넷을 허용한다.
 * - 레벨: 0 을 써서 모두 막는다.
 * - 특수: 역시 0 으로 모두 막는다.
 *
 * **허용할 것만 켜고 나머지 둘은 명시적으로 끈다** 는 점이 요점이다.
 * 부트로더가 남긴 설정이 있더라도 이 세 줄로 상태가 확정된다.
 *
 * 통째로 쓰므로 읽기-수정-쓰기가 없다 — 세 레지스터가 모두 허용 비트
 * 전용이라는 전제다.
 *
 * 실행 컨텍스트: host_init. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   exynos_pcie_host_init() → [이 함수] → exynos_pcie_writel()
 */
static void exynos_pcie_enable_irq_pulse(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;

	u32 val = IRQ_INTA_ASSERT | IRQ_INTB_ASSERT |
		  /* [한국어] 넷을 한꺼번에 허용한다 — INTx 네 선을 모두 받겠다는 뜻이다. */
		  IRQ_INTC_ASSERT | IRQ_INTD_ASSERT;

	exynos_pcie_writel(pci->elbi_base, val, PCIE_IRQ_EN_PULSE);
	/* [한국어] 레벨 인터럽트를 **명시적으로 모두 막는다.** 부트로더가 남긴 설정을
	 * 지우기 위해서다. */
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_IRQ_EN_LEVEL);
	/* [한국어] 특수 인터럽트도 같은 이유로 모두 막는다. */
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_IRQ_EN_SPECIAL);
}

/* [한국어]
 * exynos_pcie_read_dbi - 사이드밴드를 켜고 DBI 를 읽은 뒤 끈다
 *
 * @pci: DWC 코어의 문맥.
 * @base: 읽을 창의 기준 주소.
 * @reg: 그 안의 오프셋.
 * @size: 읽을 폭.
 * @return: 읽은 값.
 *
 * DWC 코어의 read_dbi 콜백이며, **코어의 모든 DBI 읽기가 이 함수를 지난다.**
 * 그래서 코어 쪽 코드는 사이드밴드의 존재를 모른 채 동작한다.
 *
 * 한 번의 논리적 읽기가 MMIO 세 번이 된다 — 켜기, 읽기, 끄기.
 *
 * [상류 코드 관찰] 켜기와 끄기 사이에 잠금이 없다. 그 구간에 다른 스레드가
 * 같은 사이드밴드 비트를 끄면 이 읽기가 실패한다. DBI 접근이 모두 PCI
 * 코어의 잠금 아래에서 일어난다는 전제로 보이나, 그 근거는 코드에 없다.
 * dw_pcie_read() 의 반환값도 확인하지 않아, 실패하면 초기화되지 않은
 * 값이 나간다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: DBI 읽기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.read_dbi == [이 함수]
 *     → exynos_pcie_sideband_dbi_r_mode(true) → dw_pcie_read() → _r_mode(false)
 */
static u32 exynos_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
				u32 reg, size_t size)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	u32 val;
/* [한국어] 읽은 값을 담을 자리. */

	exynos_pcie_sideband_dbi_r_mode(ep, true);
	/* [한국어] DWC 코어의 공용 읽기 도우미. 폭(1/2/4바이트)에 맞춰 실제 MMIO 를 낸다.
	 * [상류 코드 관찰] 반환값을 확인하지 않아, 실패하면 초기화되지 않은
	 * val 이 그대로 나간다. 원본에서 확인했으며 코드는 고치지 않았다. */
	dw_pcie_read(base + reg, size, &val);
	/* [한국어] 사이드밴드를 **반드시 되돌린다.** 켜 둔 채 두면 이후의 일반 AXI
	 * 읽기가 DBI 접근으로 잘못 해석된다. */
	exynos_pcie_sideband_dbi_r_mode(ep, false);
	/* [한국어] 읽은 값을 코어에 돌려준다. */
	return val;
}

/* [한국어]
 * exynos_pcie_write_dbi - 사이드밴드를 켜고 DBI 에 쓴 뒤 끈다
 *
 * @pci: DWC 코어의 문맥.
 * @base: 쓸 창의 기준 주소.
 * @reg: 그 안의 오프셋.
 * @size: 쓸 폭.
 * @val: 쓸 값.
 *
 * exynos_pcie_read_dbi() 의 쓰기 판이며, 켜는 사이드밴드가 쓰기용
 * (AWMISC)이라는 것만 다르다.
 *
 * 읽기 쪽과 같은 잠금 부재가 여기에도 해당한다.
 *
 * 실행 컨텍스트: DBI 쓰기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.write_dbi == [이 함수]
 *     → exynos_pcie_sideband_dbi_w_mode(true) → dw_pcie_write() → _w_mode(false)
 */
static void exynos_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				  u32 reg, size_t size, u32 val)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	exynos_pcie_sideband_dbi_w_mode(ep, true);
	/* [한국어] DWC 코어의 공용 쓰기 도우미. 폭에 맞춰 실제 MMIO 를 낸다. */
	dw_pcie_write(base + reg, size, val);
	/* [한국어] 쓰기 사이드밴드를 되돌린다. 읽기 쪽과 같은 이유다. */
	exynos_pcie_sideband_dbi_w_mode(ep, false);
}

/* [한국어]
 * exynos_pcie_rd_own_conf - 루트 포트 자신의 config 공간을 읽는다
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
 * **슬롯 0 만 받아들인다.** 루트 포트가 하나뿐이라 그 자리에만 장치가 있고,
 * 나머지는 "없음" 으로 답해야 코어가 헛되이 탐색하지 않는다.
 *
 * 기능 번호는 확인하지 않는다 — 다중 기능 루트 포트를 상정하지 않는다.
 *
 * dw_pcie_read_dbi() 를 거치므로 결국 위 사이드밴드 삼단을 지난다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 슬롯 0 이 아니면 장치 없음으로 답한다.
 *
 * 호출 체인:
 *   PCI 코어의 config 읽기 → pci_ops.read == [이 함수]
 *     → dw_pcie_read_dbi() → exynos_pcie_read_dbi()
 */
static int exynos_pcie_rd_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		/* [한국어] 슬롯 0 이 아니면 장치 없음으로 답한다. 루트 포트가 하나뿐이라 다른
		 * 슬롯을 탐색할 이유가 없고, 그대로 두면 DBI 창의 같은 자리를 여러 번
		 * 읽어 유령 장치가 생긴다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	*val = dw_pcie_read_dbi(pci, where, size);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * exynos_pcie_wr_own_conf - 루트 포트 자신의 config 공간에 쓴다
 *
 * @bus: 루트 버스.
 * @devfn: 장치·기능 번호.
 * @where: 레지스터 오프셋.
 * @size: 쓸 폭.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * exynos_pcie_rd_own_conf() 의 짝이며 규약이 같다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 슬롯 0 이 아니면 장치 없음으로 답한다.
 *
 * 호출 체인:
 *   PCI 코어의 config 쓰기 → pci_ops.write == [이 함수]
 *     → dw_pcie_write_dbi() → exynos_pcie_write_dbi()
 */
static int exynos_pcie_wr_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		/* [한국어] 쓰기 쪽도 같은 이유로 슬롯 0 만 받는다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	dw_pcie_write_dbi(pci, where, size, val);
	/* [한국어] 성공을 알린다. dw_pcie_write_dbi() 는 실패를 보고하지 않는다. */
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops exynos_pci_ops = {
	/* [한국어] 루트 포트 자신의 config 읽기를 이 파일의 DBI 경로로 돌린다.
	 * host_init 이 pp->bridge->ops 에 이 표를 걸어야 효력이 생긴다. */
	.read = exynos_pcie_rd_own_conf,
	/* [한국어] 쓰기 쪽도 같은 방식으로 돌린다. */
	.write = exynos_pcie_wr_own_conf,
};

/* [한국어]
 * exynos_pcie_link_up - 링크가 서 있는지 답한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: true = 링크가 섰다, false = 아니다.
 *
 * **비트 하나만 본다.** 레지스터 이름은 RDLH_LINKUP 인데 확인하는 비트는
 * XMLH_LINKUP 이라, 이름과 비트가 어긋나 보인다. 그 레지스터가 두 계층의
 * 상태를 함께 담고 있는 것으로 보이나 근거는 이 트리에서 확인 못 함.
 *
 * histb 판이 링크 비트 둘과 LTSSM 상태까지 셋을 보는 것과 대비된다.
 *
 * 실행 컨텍스트: DWC 코어의 링크 대기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.link_up == [이 함수] → exynos_pcie_readl()
 */
static bool exynos_pcie_link_up(struct dw_pcie *pci)
{
	u32 val = exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_RDLH_LINKUP);

	return val & PCIE_ELBI_XMLH_LINKUP;
}

/* [한국어]
 * exynos_pcie_host_init - 루트 버스 ops 를 갈아 끼우고 코어와 PHY 를 초기화한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 언제나 0.
 *
 * 이 파일의 초기화 순서가 모두 여기 있으며, **절전 복귀 경로도 이 함수를
 * 직접 부른다.**
 *
 * 다섯 단계다.
 * 1. 루트 버스용 pci_ops 를 갈아 끼운다. 그래야 루트 포트 자신의 config
 *    접근이 이 파일의 DBI 경로로 온다.
 * 2. 코어 리셋을 건다.
 * 3. PHY 를 초기화하고 전원을 켠다.
 * 4. 코어 리셋을 푼다.
 * 5. INTx 인터럽트를 허용한다.
 *
 * 2~4번의 감싸기가 요점이다. PHY 를 코어 리셋 **안에서** 초기화하므로,
 * 코어가 정지한 상태에서 PHY 설정이 이뤄진다.
 *
 * [상류 코드 관찰] phy_init() 과 phy_power_on() 의 반환값을 모두 확인하지
 * 않는다. PHY 초기화가 실패해도 그대로 진행하며, 이후 링크가 서지 않는
 * 것으로만 나타난다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안, 그리고 절전 복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 0 을 돌려주므로 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() / exynos_pcie_resume_noirq()
 *     → dw_pcie_host_ops.init == [이 함수]
 *     → exynos_pcie_assert_core_reset() → phy_init() → phy_power_on()
 *     → exynos_pcie_deassert_core_reset() → exynos_pcie_enable_irq_pulse()
 */
static int exynos_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	pp->bridge->ops = &exynos_pci_ops;
/* [한국어] 루트 버스의 config ops 를 **가장 먼저** 갈아 끼운다. 아래 초기화가
 * 끝나 버스 스캔이 시작되기 전에 걸려 있어야 하기 때문이다. */

	exynos_pcie_assert_core_reset(ep);

	phy_init(ep->phy);
	phy_power_on(ep->phy);

	exynos_pcie_deassert_core_reset(ep);
	exynos_pcie_enable_irq_pulse(ep);

	return 0;
}

static const struct dw_pcie_host_ops exynos_pcie_host_ops = {
	/* [한국어] 이 드라이버가 DWC 코어에 주는 콜백은 이 하나뿐이다. 다른 글루들이
	 * 쓰는 pre/post 훅을 쓰지 않고 초기화를 한 함수에 모았다. */
	.init = exynos_pcie_host_init,
};

/* [한국어]
 * exynos_add_pcie_port - 인터럽트를 걸고 MSI 를 끈 뒤 DWC 호스트를 초기화한다
 *
 * @ep: 드라이버 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버가 DWC 코어에 제어를 넘기는 지점이다.
 *
 * **pp->msi_irq[0] 에 -ENODEV 를 넣는 것** 이 이 함수에서 가장 중요한 줄이다.
 * 그 값이 DWC 코어에게 "이 컨트롤러에 MSI 인터럽트가 없다" 고 알려,
 * 코어가 MSI 도메인을 만들지 않는다. 그 결과 이 브리지 아래 장치들은
 * INTx 로만 인터럽트를 낼 수 있다.
 *
 * 인터럽트를 IRQF_SHARED 로 거는데, 이 파일의 핸들러는 자기 것이 아닌
 * 인터럽트에도 IRQ_HANDLED 를 돌려준다.
 *
 * 되감기가 없다. 인터럽트는 devm 판이라 자동으로 풀리고, 코어 초기화가
 * 실패하면 probe 의 fail_probe 경로가 나머지를 되돌린다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 링크 대기와 버스 스캔으로
 * 오래 걸린다.
 *
 * 에러 경로: 인터럽트를 못 얻거나 못 걸면 그 오류를, 코어 초기화가
 * 실패하면 그 오류를 기록과 함께 올려보낸다.
 *
 * 호출 체인:
 *   exynos_pcie_probe() → [이 함수]
 *     → platform_get_irq() → devm_request_irq() → dw_pcie_host_init()
 */
static int exynos_add_pcie_port(struct exynos_pcie *ep,
				       struct platform_device *pdev)
{
	struct dw_pcie *pci = &ep->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 오류 기록에 쓸 device 포인터. */
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 반환값을 받을 자리. */
	int ret;

	pp->irq = platform_get_irq(pdev, 0);
	/* [한국어] 인터럽트를 못 얻으면 오류다. 0 도 유효하지 않은 번호로 친다. */
	if (pp->irq < 0)
		/* [한국어] 그 오류를 그대로 올려보낸다. */
		return pp->irq;

	ret = devm_request_irq(dev, pp->irq, exynos_pcie_irq_handler,
			       /* [한국어] IRQF_SHARED — 이 선을 다른 장치와 나눠 쓸 수 있다. 다만 이 파일의
			        * 핸들러는 자기 원인이 없어도 IRQ_HANDLED 를 돌려주므로, 실제로는
			        * 다른 핸들러에게 차례가 넘어가지 않는다. */
			       IRQF_SHARED, "exynos-pcie", ep);
	if (ret) {
		/* [한국어] 인터럽트를 걸지 못했음을 알린다. */
		dev_err(dev, "failed to request irq\n");
		/* [한국어] 그 오류를 올려보낸다. 앞서 얻은 인터럽트는 devm 이 되돌린다. */
		return ret;
	}

	pp->ops = &exynos_pcie_host_ops;
	/* [한국어] **이 파일에서 가장 중요한 한 줄.** MSI 인터럽트 번호에 -ENODEV 를 넣어
	 * DWC 코어에게 "MSI 가 없다" 고 알린다. 코어는 MSI 도메인을 만들지 않고,
	 * 그 결과 이 브리지 아래 장치들은 INTx 로만 인터럽트를 낼 수 있다.
	 * NVMe 컨트롤러가 붙는 경우 큐별 벡터 분산이 성립하지 않는다. */
	pp->msi_irq[0] = -ENODEV;

	ret = dw_pcie_host_init(pp);
	/* [한국어] 코어 초기화 실패다. 링크가 서지 않았거나 자원 확보에 실패한 경우다. */
	if (ret) {
		/* [한국어] 실패를 알린다. */
		dev_err(dev, "failed to initialize host\n");
		/* [한국어] 오류를 올려보낸다. 나머지 되감기는 probe 의 fail_probe 가 맡는다. */
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] DBI 읽기를 가로채 사이드밴드를 켜게 한다. */
	.read_dbi = exynos_pcie_read_dbi,
	/* [한국어] DBI 쓰기도 마찬가지다. */
	.write_dbi = exynos_pcie_write_dbi,
	.link_up = exynos_pcie_link_up,
	.start_link = exynos_pcie_start_link,
};

/* [한국어]
 * exynos_pcie_probe - PHY·클럭·전원을 얻어 켜고 DWC 호스트를 세운다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * 자원이 셋이다.
 * - PHY: 디바이스 트리 노드에서 이름 없이 얻는다.
 * - 클럭: **개수를 모른 채 전부** 얻고 한 번에 켠다(_get_all_enabled).
 * - 전원: vdd18 과 vdd10 둘을 이름으로 얻어 함께 켠다.
 *
 * 전원 이름을 코드에 직접 적는 것이 눈에 띈다. 이 SoC 가 하나뿐이라
 * 표로 뺄 이유가 없다고 본 것이다.
 *
 * drvdata 를 전원을 켠 **뒤** 에 매단다. 이 파일의 변환 매크로가 drvdata 를
 * 거치는데, 그것을 처음 쓰는 것은 아래 호출 안의 host_init 이므로 그 전에만
 * 있으면 된다.
 *
 * 되감기가 한 갈래다 — PHY 를 내리고 전원을 끈다. 클럭은 devm 판이라
 * 자동으로 꺼진다.
 *
 * [상류 코드 관찰] 되감기가 `phy_exit()` 만 부르고 `phy_power_off()` 는
 * 부르지 않는다. host_init 이 `phy_power_on()` 까지 했으므로, 그 뒤에
 * 실패하면 PHY 전원이 켜진 채로 남는다. remove 와 절전 진입 경로는 둘 다
 * 부른다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 자원 확보와 전원 켜기의 실패는 그대로 올려보내고,
 * 호스트 등록 실패는 fail_probe 로 되감는다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_of_phy_get() → devm_clk_bulk_get_all_enabled()
 *     → devm_regulator_bulk_get() → regulator_bulk_enable()
 *     → exynos_add_pcie_port()
 */
static int exynos_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_pcie *ep;
	/* [한국어] PHY 를 찾을 디바이스 트리 노드. */
	struct device_node *np = dev->of_node;
	/* [한국어] 각 단계의 반환값을 받을 자리. */
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!ep)
		/* [한국어] 더 진행할 수 없다. */
		return -ENOMEM;

	ep->pci.dev = dev;
	/* [한국어] 코어가 DBI 접근과 링크 제어를 이 파일로 돌리도록 콜백 표를 건다. */
	ep->pci.ops = &dw_pcie_ops;

	ep->phy = devm_of_phy_get(dev, np, NULL);
	/* [한국어] PHY 를 못 얻었다. 대개 디바이스 트리 기술이 빠졌거나 아직 준비 전이다. */
	if (IS_ERR(ep->phy))
		/* [한국어] 오류를 그대로 올려보낸다 — -EPROBE_DEFER 를 포함하므로, 나중에 다시
		 * 시도될 수 있다. */
		return PTR_ERR(ep->phy);

	ret = devm_clk_bulk_get_all_enabled(dev, &ep->clks);
	/* [한국어] 클럭을 못 얻었거나 못 켰다. */
	if (ret < 0)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	ep->supplies[0].supply = "vdd18";
	/* [한국어] 둘째는 1.0V 코어 전원이다. */
	ep->supplies[1].supply = "vdd10";
	/* [한국어] 이름으로 두 전원 핸들을 얻는다. devm 판이라 해제는 자동이지만,
	 * **켜고 끄는 것은 자동이 아니다** — 아래에서 손수 켠다. */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ep->supplies),
				      ep->supplies);
	if (ret)
		/* [한국어] 전원 핸들을 못 얻었다면 그대로 올려보낸다. */
		return ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ep->supplies), ep->supplies);
	/* [한국어] 전원을 못 켰다. */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	platform_set_drvdata(pdev, ep);

	ret = exynos_add_pcie_port(ep, pdev);
	/* [한국어] 호스트 등록이 실패했다. */
	if (ret < 0)
		/* [한국어] 여기서부터는 되감을 것이 있어 goto 로 간다. */
		goto fail_probe;

	/* [한국어] 성공이다. 되감기 구간을 건너뛴다. */
	return 0;

fail_probe:
	/* [한국어] PHY 를 해제한다.
	 * [상류 코드 관찰] phy_power_off() 를 부르지 않는다. host_init 이
	 * phy_power_on() 까지 했으므로 PHY 전원이 켜진 채로 남는다. remove 와
	 * 절전 진입 경로는 둘 다 부른다. 원본에서 확인했으며 고치지 않았다. */
	phy_exit(ep->phy);
	/* [한국어] 앞서 켠 두 전원을 끈다. 클럭은 devm 판이라 자동으로 꺼진다. */
	regulator_bulk_disable(ARRAY_SIZE(ep->supplies), ep->supplies);

	/* [한국어] 실패 이유를 그대로 올려보낸다. */
	return ret;
}

/* [한국어]
 * exynos_pcie_remove - 호스트를 내리고 코어·PHY·전원을 차례로 끈다
 *
 * @pdev: 플랫폼 장치.
 *
 * probe 의 역순이다.
 *
 * 다섯 단계 — DWC 호스트를 내리고, 코어 리셋을 걸고, PHY 전원을 끄고,
 * PHY 를 해제하고, 전원을 끊는다.
 *
 * **호스트를 가장 먼저 내리는 것** 이 중요하다. 그 아래 장치들의 드라이버
 * remove 가 config 접근을 하는데, 코어를 먼저 리셋하면 그 접근이 실패한다.
 *
 * probe 의 되감기와 달리 phy_power_off() 를 부른다 — 그쪽의 누락과 대비된다.
 *
 * 클럭은 여기서 끄지 않는다. devm 판이라 이 함수가 돌아간 뒤 코어가
 * 자동으로 끈다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → dw_pcie_host_deinit() → exynos_pcie_assert_core_reset()
 *     → phy_power_off() → phy_exit() → regulator_bulk_disable()
 */
static void exynos_pcie_remove(struct platform_device *pdev)
{
	struct exynos_pcie *ep = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&ep->pci.pp);
	exynos_pcie_assert_core_reset(ep);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	regulator_bulk_disable(ARRAY_SIZE(ep->supplies), ep->supplies);
}

/* [한국어]
 * exynos_pcie_suspend_noirq - 절전 진입 시 코어와 PHY, 전원을 내린다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 언제나 0.
 *
 * remove 의 뒤 네 단계와 똑같다 — 코어 리셋, PHY 전원, PHY 해제, 전원 차단.
 * 다른 것은 DWC 호스트를 내리지 않는다는 점뿐이며, 복귀 후 같은 버스를
 * 그대로 쓸 것이기 때문이다.
 *
 * noirq 단계에서 도는 이유는 이 안에서 컨트롤러를 정지시키기 때문이다.
 * 인터럽트가 아직 살아 있는 단계에서 그렇게 하면 그 사이에 온 인터럽트를
 * 처리할 수 없다.
 *
 * DWC 코어의 dw_pcie_suspend_noirq() 를 부르지 않는다 — 링크 상태 저장
 * 없이 복귀 때 처음부터 다시 세우는 방식이다.
 *
 * 실행 컨텍스트: 시스템 절전의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq == [이 함수]
 *     → exynos_pcie_assert_core_reset() → phy_power_off() → phy_exit()
 *     → regulator_bulk_disable()
 */
static int exynos_pcie_suspend_noirq(struct device *dev)
{
	struct exynos_pcie *ep = dev_get_drvdata(dev);

	/* [한국어] 컨트롤러를 정지시킨다. 복귀 때 처음부터 다시 세울 것이므로 링크
	 * 상태를 저장하지 않는다. */
	exynos_pcie_assert_core_reset(ep);
	/* [한국어] PHY 전원을 끈다. */
	phy_power_off(ep->phy);
	/* [한국어] PHY 를 해제한다. probe 의 되감기와 달리 여기는 두 호출이 모두 있다. */
	phy_exit(ep->phy);
	/* [한국어] 마지막으로 전원을 끊는다. 안쪽에서 바깥쪽 순서다. */
	regulator_bulk_disable(ARRAY_SIZE(ep->supplies), ep->supplies);

	/* [한국어] 실패할 여지가 없어 언제나 성공을 알린다. */
	return 0;
}

/* [한국어]
 * exynos_pcie_resume_noirq - 전원을 켜고 초기화 순서를 손수 재현한다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 = 성공, 음수 오류.
 *
 * exynos_pcie_suspend_noirq() 의 짝이지만, **초기화를 코어에 맡기지 않고
 * 직접 밟는 것** 이 이 함수의 특징이다.
 *
 * 네 단계다.
 * 1. 전원을 켠다.
 * 2. exynos_pcie_host_init() 을 직접 부른다 — 원래 DWC 코어가 콜백으로
 *    부르는 함수를 여기서 손수 호출한다.
 * 3. dw_pcie_setup_rc() 로 루트 포트 config 를 다시 세운다.
 * 4. exynos_pcie_start_link() 로 LTSSM 을 켜고 링크를 기다린다.
 *
 * 즉 probe 경로에서 DWC 코어가 해 주던 일을 이 함수가 순서대로 재현한다.
 * 코어에 복귀용 진입점(dw_pcie_resume_noirq)을 쓰지 않는 선택이며,
 * 그 이유는 이 트리에서 확인 못 함.
 *
 * [상류 코드 관찰] host_init 과 start_link 의 반환값을 확인하지 않는다.
 * 다만 두 함수 모두 언제나 0 을 돌려주므로 확인할 것이 없기는 하다.
 * 최종 반환은 링크 대기의 결과다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계. 링크 대기가 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 전원 켜기가 실패하면 그 오류를, 링크가 서지 않으면
 * dw_pcie_wait_for_link() 의 오류를 올려보낸다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq == [이 함수]
 *     → regulator_bulk_enable() → exynos_pcie_host_init()
 *     → dw_pcie_setup_rc() → exynos_pcie_start_link()
 *     → dw_pcie_wait_for_link()
 */
static int exynos_pcie_resume_noirq(struct device *dev)
{
	struct exynos_pcie *ep = dev_get_drvdata(dev);
	struct dw_pcie *pci = &ep->pci;
	/* [한국어] start_link 에 넘길 dw_pcie 와, host_init 에 넘길 루트 포트 문맥이다. */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 전원 켜기의 반환값을 받을 자리. */
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ep->supplies), ep->supplies);
	/* [한국어] 전원을 못 켜면 나머지를 할 수 없다. */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	/* exynos_pcie_host_init controls ep->phy */
	/* [한국어] **초기화 콜백을 손수 부른다.** 원래 dw_pcie_host_init() 이 부르는
	 * 함수인데, 복귀 경로에서는 코어를 다시 거치지 않고 직접 호출한다.
	 * 반환값은 확인하지 않으나 이 함수는 언제나 0 이다. */
	exynos_pcie_host_init(pp);
	/* [한국어] 루트 포트 config 와 ATU 를 다시 세운다. 절전 중에 코어 리셋이 걸려
	 * 그 설정이 모두 날아갔기 때문이다. */
	dw_pcie_setup_rc(pp);
	/* [한국어] LTSSM 을 켜 링크 훈련을 시작시킨다. 역시 언제나 0 이다. */
	exynos_pcie_start_link(pci);
	/* [한국어] 링크가 설 때까지 기다린 결과가 이 함수의 최종 반환값이다.
	 * 링크가 서지 않으면 복귀가 실패로 보고된다. */
	return dw_pcie_wait_for_link(pci);
}

static const struct dev_pm_ops exynos_pcie_pm_ops = {
	/* [한국어] 절전 진입·복귀를 noirq 단계에 건다. 인터럽트가 이미 막힌 뒤에
	 * 컨트롤러를 정지시키려는 것이다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(exynos_pcie_suspend_noirq,
				  exynos_pcie_resume_noirq)
};

static const struct of_device_id exynos_pcie_of_match[] = {
	/* [한국어] 이 드라이버가 지원하는 유일한 SoC. 표에 항목이 하나뿐이다. */
	{ .compatible = "samsung,exynos5433-pcie", },
	/* [한국어] 표의 끝 표시. */
	{ },
};

static struct platform_driver exynos_pcie_driver = {
	/* [한국어] 장치가 붙을 때 부를 진입점. */
	.probe		= exynos_pcie_probe,
	/* [한국어] 장치가 떨어질 때 부를 해제 함수. */
	.remove		= exynos_pcie_remove,
	.driver = {
		.name	= "exynos-pcie",
		/* [한국어] 위의 표로 디바이스 트리 노드와 짝을 맞춘다. */
		.of_match_table = exynos_pcie_of_match,
		.pm		= &exynos_pcie_pm_ops,
	},
};
module_platform_driver(exynos_pcie_driver);
MODULE_DESCRIPTION("Samsung Exynos PCIe host controller driver");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, exynos_pcie_of_match);
