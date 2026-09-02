// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics STM32MP25 PCIe endpoint driver.
 *
 * Copyright (C) 2025 STMicroelectronics
 * Author: Christian Bruel <christian.bruel@foss.st.com>
 */

/*
 * [한국어 설명] STM32MP25 의 DesignWare PCIe 엔드포인트 글루 (pcie-stm32-ep.c)
 *
 * === 파일의 역할 ===
 * STM32MP25 SoC 를 PCIe **엔드포인트** 로 동작시키는 드라이버다. 이 트리의
 * 다른 컨트롤러 드라이버 대부분이 호스트(루트 컴플렉스) 쪽인 것과 방향이
 * 반대다 — 여기서는 이 SoC 가 남의 PCIe 슬롯에 꽂힌 카드 노릇을 한다.
 *
 * 그 방향의 차이가 이 파일의 모든 것을 정한다. 호스트 드라이버는 자기가
 * 링크를 세우지만, 엔드포인트는 **호스트가 세워 주기를 기다린다.**
 * 그 기다림의 신호가 PERST# 이며, 이 파일의 뼈대가 그 신호를 GPIO
 * 인터럽트로 받아 처리하는 구조다.
 *
 *   PERST# 가 어서트되면 — 호스트가 리셋을 걸었다는 뜻이다.
 *     LTSSM 을 끄고, EPC 에 해제를 알리고, PHY 와 클럭을 끄고, 절전에 든다.
 *   PERST# 가 디어서트되면 — 호스트가 리셋을 풀었다는 뜻이다.
 *     깨어나서 자원을 켜고, **config 공간을 다시 프로그래밍하고**,
 *     EPC 에 초기화를 알리고, LTSSM 을 켠다.
 *
 * config 공간을 다시 프로그래밍해야 하는 이유가 이 파일에서 가장 중요한
 * 대목이다 — phy_init() 이 PHY 의 RCC 를 거치며 DBI 레지스터를 리셋해 버려,
 * 그 전에 써 둔 벤더 ID·BAR 설정이 모두 지워지기 때문이다(해당 자리의
 * 상류 주석이 그것을 밝힌다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> stm32_pcie_probe()
 *     -> syscon, PHY, 클럭, 리셋, PERST# GPIO 를 얻는다
 *     -> PERST# 인터럽트를 **꺼진 채로** 등록한다(IRQ_NOAUTOEN)
 *     -> stm32_add_pcie_ep()
 *        -> syscon 으로 EP 모드를 지정하고 리셋을 한 번 돌린다
 *        -> dw_pcie_ep_init()  [pcie-designware-ep.c]
 *           -> EPC 를 등록하고, 그것이 EPF 드라이버와 짝지어진다
 *        -> 자원을 켠다
 *
 * 동작 중(호스트가 켜질 때):
 *   EPF 드라이버가 준비를 마치고 dw_pcie_start_link()
 *     -> [이 파일] stm32_pcie_start_link()
 *        -> PERST# 인터럽트를 켠다. 이것이 "이제 호스트를 기다린다" 는 뜻이다.
 *   호스트가 PERST# 를 풂
 *     -> [이 파일] stm32_pcie_ep_perst_irq_thread()
 *        -> stm32_pcie_perst_deassert() -> 자원 켜기 -> config 재설정
 *           -> LTSSM 켜기 -> 링크 훈련 시작
 *
 * 실행 컨텍스트: probe 와 PERST# 처리는 프로세스 컨텍스트다. PERST# 는
 * **스레드 인터럽트** 로 등록되는데, 처리 안에서 PHY 초기화와 런타임 PM
 * 이 일어나 잠들 수 있기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/endpoint/ 의 EPC/EPF 계층. 이 파일이 등록한 EPC 에
 *   기능 드라이버(pci-epf-test 등)가 붙어 실제 동작을 정의한다.
 *   pci_epc_init_notify()/deinit_notify() 가 그 계층에 링크 상태를 알린다.
 * 아래쪽: pcie-designware-ep.c 와 pcie-designware.c. 접점이 두 벌의 콜백
 *   표다 — dw_pcie_ops(start_link, stop_link)와 dw_pcie_ep_ops(raise_irq,
 *   get_features).
 * 옆쪽: pcie-stm32.h 가 같은 SoC 의 호스트 드라이버(pcie-stm32.c)와 공유하는
 *   syscon 레지스터 정의를 담는다. 두 드라이버가 같은 하드웨어를 서로 다른
 *   모드로 쓰며, SYSCFG_PCIECR 의 타입 필드가 그 모드를 정한다.
 *
 * 데이터 흐름:
 *   디바이스 트리(syscon, PHY, 클럭, 리셋, "reset" GPIO) -> probe
 *     -> struct stm32_pcie
 *   PERST# GPIO 값 -> 인터럽트 스레드 -> assert/deassert 두 갈래
 *   그 갈래가 EPC 계층에 알림을 보내고, EPF 드라이버가 그것을 받아
 *   BAR 설정과 데이터 전송을 준비한다.
 *
 * 공유 상태: struct stm32_pcie 하나. probe 후 불변이며 잠금이 없다 —
 *   PERST# 스레드가 유일한 동시 실행 주체이고, IRQF_ONESHOT 이 그것을
 *   한 번에 하나로 제한한다.
 *
 * === NVMe 관점 ===
 * 이 드라이버는 NVMe 를 쓰는 쪽이 아니라 **NVMe 장치가 될 수도 있는 쪽**
 * 이다. EPF 계층에 NVMe 기능 드라이버를 붙이면 이 SoC 가 호스트에게 NVMe
 * 컨트롤러로 보이게 만들 수 있고, 그때 이 파일이 하는 일은 그 아래의
 * 링크와 config 공간을 관리하는 것뿐이다. 이 트리에 그런 EPF 드라이버는
 * 없다(endpoint/functions/ 에 test, ntb, vntb, mhi 만 있다).
 *
 * === 주요 함수/구조체 요약 ===
 * stm32_pcie_perst_deassert()      : 호스트가 리셋을 풀었을 때의 전 과정.
 *                                    이 파일에서 가장 중요한 함수다.
 * stm32_pcie_perst_assert()        : 그 반대. 자원을 끄고 절전에 든다.
 * stm32_pcie_ep_perst_irq_thread() : PERST# 값을 읽어 두 갈래로 보내고
 *                                    다음 에지를 기다리도록 트리거를 뒤집는다.
 * stm32_pcie_start_link()          : 링크를 세우지 않는다 — PERST# 인터럽트를
 *                                    켜서 호스트를 기다리기 시작할 뿐이다.
 * stm32_pcie_raise_irq()           : EPF 가 호스트에게 인터럽트를 보낼 때.
 * struct stm32_pcie                : dw_pcie 를 맨 앞에 둔 이 드라이버의 상태.
 */

/* [한국어] clk_prepare_enable()/clk_disable_unprepare(). 이 파일은 클럭을 실제로 켜고 끈다. */
#include <linux/clk.h>
/* [한국어] gpiod_get_value() 와 gpiod_to_irq(). PERST# 를 GPIO 로 받는 구조의 근거다. */
#include <linux/gpio/consumer.h>
/* [한국어] syscon_regmap_lookup_by_compatible(). SoC 의 SYSCFG 블록을 찾는 데 쓴다. */
#include <linux/mfd/syscon.h>
/* [한국어] of_platform 헤더. */
#include <linux/of_platform.h>
/* [한국어] phy_init()/phy_exit()/phy_set_mode(). PHY 가 이 드라이버의 핵심 자원 중 하나다. */
#include <linux/phy/phy.h>
/* [한국어] struct platform_device. */
#include <linux/platform_device.h>
/* [한국어] 런타임 PM. PERST# 어서트가 절전에 들어가고 디어서트가 깨어나는 구조다. */
#include <linux/pm_runtime.h>
/* [한국어] regmap_update_bits(). syscon 레지스터를 비트 단위로 고친다. */
#include <linux/regmap.h>
/* [한국어] reset_control_assert()/deassert(). 모드를 바꾼 뒤 컨트롤러를 리셋하는 데 쓴다. */
#include <linux/reset.h>
/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_ep_ops, dw_pcie_ep_init(). */
#include "pcie-designware.h"
/* [한국어] 같은 SoC 의 호스트 드라이버(pcie-stm32.c)와 공유하는 syscon 정의.
 * SYSCFG_PCIECR 과 그 안의 타입·LTSSM 비트가 거기 있어,
 * 두 드라이버가 같은 레지스터를 서로 다른 모드로 쓴다. */
#include "pcie-stm32.h"

/* [한국어] 이 드라이버의 상태 전부. */
struct stm32_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **맨 앞에 두어** to_stm32_pcie 변환이 성립한다.
	 * 설정자: probe 가 dev 와 ops 를 채우고, DWC 코어가 나머지를 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 모든 콜백.
	 * 값 범위: DWC 코어가 정의한 구조체.
	 * 동기화: 코어가 관리한다. */
	struct dw_pcie pci;
	/* [한국어] SoC 의 SYSCFG 블록을 가리키는 regmap.
	 * 설정자: probe 의 syscon_regmap_lookup_by_compatible("st,stm32mp25-syscfg").
	 * 읽는 자: 모드 설정(stm32_add_pcie_ep)과 LTSSM 제어(perst_assert/deassert).
	 * 값 범위: 유효한 regmap 포인터. syscon 은 여러 드라이버가 공유하는 블록이라,
	 * regmap 계층이 그 동시 접근을 지킨다.
	 * 동기화: probe 후 불변. 레지스터 접근의 원자성은 regmap 이 맡는다. */
	struct regmap *regmap;
	/* [한국어] PCIe 컨트롤러 리셋.
	 * 설정자: probe 의 devm_reset_control_get_exclusive().
	 * 읽는 자: stm32_add_pcie_ep() 이 모드를 바꾼 뒤 한 번 걸었다 푼다.
	 * 값 범위: 유효한 리셋 컨트롤. exclusive 판이라 다른 드라이버와 공유되지 않는다.
	 * 동기화: probe 후 불변. */
	struct reset_control *rst;
	/* [한국어] PCIe PHY.
	 * 설정자: probe 의 devm_phy_get().
	 * 읽는 자: stm32_pcie_enable_resources()/disable_resources().
	 * 값 범위: 유효한 PHY 포인터.
	 * 동기화: probe 후 불변. **phy_init() 이 DBI 레지스터를 리셋한다는 부작용** 이
	 * 이 파일의 config 재프로그래밍 단계를 필요하게 만든다. */
	struct phy *phy;
	/* [한국어] PCIe 클럭.
	 * 설정자: probe 의 devm_clk_get().
	 * 읽는 자: stm32_pcie_enable_resources()/disable_resources().
	 * 값 범위: 유효한 clk 포인터. 이 파일은 실제로 prepare/enable 한다 —
	 * 얻기만 하는 드라이버들과 다르다.
	 * 동기화: probe 후 불변. */
	struct clk *clk;
	/* [한국어] 호스트가 내보내는 PERST# 신호를 받는 GPIO.
	 * 설정자: probe 의 devm_gpiod_get("reset", GPIOD_IN) — 입력이다.
	 * 읽는 자: stm32_pcie_ep_perst_irq_thread() 가 값을 읽어 갈래를 정하고,
	 * 같은 함수가 gpiod_to_irq() 로 트리거 방향을 뒤집는다.
	 * 값 범위: 1 = 어서트(호스트가 리셋을 걸었다), 0 = 디어서트.
	 * 동기화: probe 후 불변. 값 자체는 호스트가 바꾼다. */
	struct gpio_desc *perst_gpio;
	/* [한국어] 위 GPIO 에 대응하는 인터럽트 번호.
	 * 설정자: probe 의 gpiod_to_irq().
	 * 읽는 자: stm32_pcie_start_link() 가 켜고 stop_link() 가 끈다.
	 * 값 범위: 유효한 IRQ 번호.
	 * 동기화: probe 후 불변. probe 는 이것을 IRQ_NOAUTOEN 으로 꺼 둔 채 등록하고,
	 * start_link 가 켜는 것이 이 드라이버의 시점 분리다. */
	unsigned int perst_irq;
};

/* [한국어]
 * stm32_pcie_start_link - PERST# 인터럽트를 켜서 호스트를 기다리기 시작한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * 이름과 달리 **링크를 세우지 않는다.** 엔드포인트는 링크를 스스로 세울 수
 * 없고 호스트가 PERST# 를 풀어 주기를 기다려야 하므로, 이 콜백이 할 수 있는
 * 일은 그 신호를 받을 준비를 하는 것뿐이다.
 *
 * probe 에서 IRQ_NOAUTOEN 으로 꺼 둔 인터럽트를 여기서 켠다. 그 시점 분리가
 * 설계의 요점이다 — EPF 드라이버가 BAR 설정을 마치기 전에 호스트가 링크를
 * 세우면, 아직 준비되지 않은 config 공간이 노출된다.
 *
 * 호출자가 dw_pcie_start_link() 이고, 그것을 부르는 것은 EPF 계층이다.
 * 즉 "기능 드라이버가 준비를 마쳤다" 가 이 함수의 진입 조건이다.
 *
 * 실행 컨텍스트: EPF 계층. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 성공을 답한다.
 *
 * 호출 체인:
 *   EPF 드라이버 → dw_pcie_start_link() → dw_pcie_ops.start_link == [이 함수]
 *     → enable_irq()
 */
static int stm32_pcie_start_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);

	enable_irq(stm32_pcie->perst_irq);

	return 0;
}

/* [한국어]
 * stm32_pcie_stop_link - PERST# 인터럽트를 꺼서 호스트 신호를 무시한다
 *
 * @pci: DWC 코어의 문맥.
 *
 * stm32_pcie_start_link() 의 짝이며, 역시 링크 자체를 끊지 않는다.
 * 호스트의 PERST# 를 더는 보지 않겠다는 뜻이다.
 *
 * remove 경로에서 가장 먼저 불린다. 정리 도중에 PERST# 인터럽트가 들어와
 * 해제 중인 자원을 다시 켜려 드는 것을 막아야 하기 때문이다.
 *
 * disable_irq() 는 이미 실행 중인 핸들러가 끝나기를 기다린다. 그 덕분에
 * 이 함수가 돌아온 시점에는 PERST# 처리가 진행 중이지 않다는 것이 보장된다.
 *
 * 실행 컨텍스트: EPF 계층 또는 remove. 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   stm32_pcie_remove() / EPF 계층 → dw_pcie_stop_link()
 *     → dw_pcie_ops.stop_link == [이 함수] → disable_irq()
 */
static void stm32_pcie_stop_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);

	disable_irq(stm32_pcie->perst_irq);
}

/* [한국어]
 * stm32_pcie_raise_irq - 엔드포인트가 호스트에게 인터럽트를 보낸다
 *
 * @ep: DWC 의 엔드포인트 문맥.
 * @func_no: 물리 기능 번호.
 * @type: INTx 인지 MSI 인지.
 * @interrupt_num: MSI 벡터 번호.
 * @return: 0 = 성공, -EINVAL = 지원하지 않는 종류.
 *
 * 방향이 호스트 드라이버와 반대다. 여기서는 이 SoC 가 인터럽트를 **내는**
 * 쪽이며, EPF 드라이버가 "작업이 끝났다" 를 호스트에게 알릴 때 이 경로를 쓴다.
 *
 * MSI-X 를 지원하지 않는 것이 이 함수에서 드러난다. switch 에 그 갈래가 없어
 * -EINVAL 로 떨어지며, 아래 epc_features 의 msi_capable 만 참인 것과 맞물린다.
 *
 * 실제 동작은 모두 DWC 코어가 한다. 이 파일은 종류를 가려 넘기기만 한다.
 *
 * 실행 컨텍스트: EPF 드라이버. 프로세스 컨텍스트.
 *
 * 에러 경로: 알 수 없는 종류는 기록을 남기고 -EINVAL.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq() → dw_pcie_ep_ops.raise_irq == [이 함수]
 *     → dw_pcie_ep_raise_intx_irq() 또는 dw_pcie_ep_raise_msi_irq()
 */
static int stm32_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	switch (type) {
	/* [한국어] 레거시 INTx 면, */
	case PCI_IRQ_INTX:
		/* [한국어] DWC 코어의 INTx 발생 함수로 넘긴다. */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	case PCI_IRQ_MSI:
		/* [한국어] MSI 면 벡터 번호까지 넘긴다. MSI-X 갈래가 없어 그 요청은 아래 default 로 떨어진다. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
		return -EINVAL;
	}
}

static const struct pci_epc_features stm32_pcie_epc_features = {
	/* [한국어] DWC 엔드포인트가 공통으로 갖는 능력들. */
	DWC_EPC_COMMON_FEATURES,
	/* [한국어] MSI 를 지원한다. **MSI-X 는 없다** — 위 raise_irq 의 switch 에 그 갈래가
	 * 없는 것과 맞물린다. */
	.msi_capable = true,
	/* [한국어] EPF 가 잡을 메모리의 정렬 요구. probe 가 이 값을 ep->page_size 로도
	 * 복사해, EPF 가 어느 쪽을 보든 같은 답을 얻게 한다. */
	.align = SZ_64K,
};

/* [한국어]
 * stm32_pcie_get_features - 이 엔드포인트의 능력을 EPC 계층에 알린다
 *
 * @ep: DWC 의 엔드포인트 문맥. 쓰지 않는다.
 * @return: 정적 능력 서술자.
 *
 * EPF 드라이버가 BAR 을 설정하기 전에 "이 하드웨어가 무엇을 할 수 있는가"
 * 를 물어보는 통로다.
 *
 * 정적 구조체 하나를 그대로 돌려준다 — 이 SoC 에 변종이 없어 런타임에
 * 달라질 것이 없기 때문이다.
 *
 * 그 구조체가 알리는 것이 셋이다. DWC 공통 능력, MSI 지원(MSI-X 는 없다),
 * 그리고 64KB 정렬 요구. 마지막 것이 EPF 가 잡을 메모리의 정렬을 정하며,
 * probe 가 그 값을 ep->page_size 로도 복사한다.
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
stm32_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &stm32_pcie_epc_features;
}

static const struct dw_pcie_ep_ops stm32_pcie_ep_ops = {
	/* [한국어] 호스트에게 인터럽트를 보내는 통로. */
	.raise_irq = stm32_pcie_raise_irq,
	/* [한국어] 이 하드웨어의 능력을 EPF 에 알리는 통로. 이 둘이 EP 쪽 접점 전부다. */
	.get_features = stm32_pcie_get_features,
};

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크를 세우는 것이 아니라 PERST# 를 기다리기 시작한다. */
	.start_link = stm32_pcie_start_link,
	/* [한국어] 그 반대. 이 표에 콜백이 둘뿐인 것은 나머지를 DWC 코어가 표준대로 처리하기 때문이다. */
	.stop_link = stm32_pcie_stop_link,
};

/* [한국어]
 * stm32_pcie_enable_resources - PHY 와 클럭을 켠다
 *
 * @stm32_pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 순서가 정해져 있다 — PHY 를 먼저, 클럭을 나중에. PHY 초기화가 클럭
 * 설정까지 건드릴 수 있어 그쪽이 먼저여야 한다.
 *
 * 되감기가 한 줄이다. 클럭 켜기가 실패하면 방금 초기화한 PHY 를 되돌린다.
 * PHY 초기화 자체가 실패하면 되돌릴 것이 없어 그대로 물러난다.
 *
 * **phy_init() 이 DBI 레지스터를 리셋한다** 는 것이 이 함수의 숨은 부작용이며,
 * 그래서 이것을 부른 뒤에는 config 공간을 다시 프로그래밍해야 한다.
 * stm32_pcie_perst_deassert() 가 그 일을 하고, 해당 자리의 상류 주석이
 * 근거를 밝힌다.
 *
 * 실행 컨텍스트: probe 와 PERST# 스레드. PHY 초기화가 잠들 수 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 어느 단계가 실패하든 그 오류를 올려보내며, 앞 단계는 되돌린다.
 *
 * 호출 체인:
 *   stm32_add_pcie_ep() / stm32_pcie_perst_deassert() → [이 함수]
 *     → phy_init() → clk_prepare_enable()
 */
static int stm32_pcie_enable_resources(struct stm32_pcie *stm32_pcie)
{
	int ret;

	ret = phy_init(stm32_pcie->phy);
	/* [한국어] PHY 초기화가 실패하면, */
	if (ret)
		/* [한국어] 되돌릴 것이 없으므로 그대로 물러난다. */
		return ret;

	ret = clk_prepare_enable(stm32_pcie->clk);
	/* [한국어] 클럭 켜기가 실패했으면, */
	if (ret)
		/* [한국어] 방금 초기화한 PHY 를 되돌린다. */
		phy_exit(stm32_pcie->phy);

	return ret;
}

/* [한국어]
 * stm32_pcie_disable_resources - 클럭과 PHY 를 끈다
 *
 * @stm32_pcie: 드라이버 상태.
 *
 * stm32_pcie_enable_resources() 의 짝이며 순서가 정확히 반대다 —
 * 클럭을 먼저 끄고 PHY 를 나중에 끈다.
 *
 * 반환값이 없다. 두 해제 함수가 실패를 알리지 않기 때문이다.
 *
 * 실행 컨텍스트: PERST# 스레드와 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   stm32_pcie_perst_assert() / stm32_pcie_remove() /
 *   stm32_pcie_perst_deassert() 의 되감기 → [이 함수]
 *     → clk_disable_unprepare() → phy_exit()
 */
static void stm32_pcie_disable_resources(struct stm32_pcie *stm32_pcie)
{
	clk_disable_unprepare(stm32_pcie->clk);

	phy_exit(stm32_pcie->phy);
}

/* [한국어]
 * stm32_pcie_perst_assert - 호스트가 리셋을 걸었을 때 자원을 내린다
 *
 * @pci: DWC 코어의 문맥.
 *
 * 호스트가 꺼지거나 재부팅할 때 PERST# 를 어서트하며, 그때 이 함수가 불린다.
 *
 * 네 단계로 내려간다.
 * 1. LTSSM 을 끈다 — 링크 훈련을 멈춘다.
 * 2. EPC 계층에 해제를 알린다. EPF 드라이버가 그 알림을 받아 자기 상태를
 *    정리하며, 그 전에 자원을 끄면 EPF 가 이미 없는 하드웨어에 접근한다.
 * 3. PHY 와 클럭을 끈다.
 * 4. 런타임 PM 참조를 놓는다. 이제 이 장치가 절전에 들어갈 수 있다.
 *
 * 순서가 위에서 아래로 "논리적인 것부터 물리적인 것" 이다. 그 반대로 하면
 * 각 단계가 이미 사라진 아래 단계를 건드리게 된다.
 *
 * _sync 판으로 참조를 놓는 것이 눈에 띈다. 절전 진입이 끝날 때까지 기다리는
 * 것으로, 다음 PERST# 디어서트가 곧바로 이어질 때 절전과 복귀가 겹치지 않게
 * 한다.
 *
 * 실행 컨텍스트: PERST# 인터럽트 스레드. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   stm32_pcie_ep_perst_irq_thread() → [이 함수]
 *     → regmap_update_bits() → pci_epc_deinit_notify()
 *     → stm32_pcie_disable_resources() → pm_runtime_put_sync()
 */
static void stm32_pcie_perst_assert(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);
	struct dw_pcie_ep *ep = &stm32_pcie->pci.ep;
	/* [한국어] 로그와 런타임 PM 에 쓸 device. */
	struct device *dev = pci->dev;

	dev_dbg(dev, "PERST asserted by host\n");
/* [한국어] 어느 방향의 변화였는지 기록에 남긴다. 이 경로는 호스트 쪽 사정으로
 * 일어나므로, 로그가 원인 추적의 단서가 된다. */

	regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
			   /* [한국어] LTSSM 을 끈다 — 링크 훈련을 멈추는 것이 내려가는 첫 단계다. */
			   STM32MP25_PCIECR_LTSSM_EN, 0);

	pci_epc_deinit_notify(ep->epc);

	stm32_pcie_disable_resources(stm32_pcie);

	pm_runtime_put_sync(dev);
}

/* [한국어]
 * stm32_pcie_perst_deassert - 호스트가 리셋을 풀었을 때 다시 준비한다
 *
 * @pci: DWC 코어의 문맥.
 *
 * 이 파일에서 가장 중요한 함수다. 호스트가 켜지면 PERST# 를 풀고, 그때부터
 * 링크 훈련이 시작될 수 있으므로 그 전에 모든 준비가 끝나 있어야 한다.
 *
 * 다섯 단계다.
 * 1. 런타임 PM 으로 깨어난다.
 * 2. PHY 와 클럭을 켠다.
 * 3. **config 공간을 다시 프로그래밍한다.** 옆의 상류 주석이 그 이유를
 *    밝히는데, 방금 부른 phy_init() 이 PHY 의 RCC 를 거치며 DBI 레지스터를
 *    리셋해 버리기 때문이다. 이 단계를 빠뜨리면 호스트가 벤더 ID 도 BAR 도
 *    설정되지 않은 장치를 보게 된다.
 * 4. EPC 계층에 초기화를 알린다. EPF 드라이버가 그 알림을 받아 BAR 을
 *    설정한다.
 * 5. **마지막으로** LTSSM 을 켠다. 이 순서가 이 함수의 핵심으로, 앞의 넷이
 *    끝나기 전에 링크를 열면 호스트가 준비되지 않은 장치를 열거한다.
 *
 * 되감기가 계단이다. 3번이나 4번에서 실패하면 자원을 끄고 PM 참조를 놓으며,
 * 1번 다음에 실패하면 PM 참조만 놓는다.
 *
 * 반환값이 없어 실패를 호출자에게 알리지 못한다. 그때 LTSSM 이 켜지지 않아
 * 호스트가 이 장치를 보지 못하는 것으로 나타난다.
 *
 * 실행 컨텍스트: PERST# 인터럽트 스레드. PM 복귀와 PHY 초기화가 있어
 * 잠들 수 있는 문맥이어야 하며, 그래서 스레드 인터럽트로 등록한다.
 *
 * 에러 경로: 각 단계의 실패를 기록하고 goto 로 앞 단계를 되돌린다.
 *
 * 호출 체인:
 *   stm32_pcie_ep_perst_irq_thread() → [이 함수]
 *     → pm_runtime_resume_and_get() → stm32_pcie_enable_resources()
 *     → dw_pcie_ep_init_registers() → pci_epc_init_notify()
 *     → regmap_update_bits(LTSSM_EN)
 */
static void stm32_pcie_perst_deassert(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);
	struct device *dev = pci->dev;
	/* [한국어] EPC 계층에 알릴 때 쓸 엔드포인트 문맥. */
	struct dw_pcie_ep *ep = &pci->ep;
	/* [한국어] 각 단계의 결과. */
	int ret;

	dev_dbg(dev, "PERST de-asserted by host\n");
/* [한국어] 어느 방향이었는지 기록에 남긴다. */

	ret = pm_runtime_resume_and_get(dev);
	/* [한국어] 런타임 PM 복귀가 실패하면 — 이 경우 참조가 올라가지 않았다. */
	if (ret < 0) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to resume runtime PM: %d\n", ret);
		/* [한국어] 되돌릴 것이 없으므로 그대로 물러난다. */
		return;
	}

	ret = stm32_pcie_enable_resources(stm32_pcie);
	/* [한국어] 자원 켜기가 실패하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to enable resources: %d\n", ret);
		/* [한국어] PM 참조만 놓는 경로로 간다. */
		goto err_pm_put_sync;
	}

	/*
	 * Reprogram the configuration space registers here because the DBI
	 * registers were reset by the PHY RCC during phy_init().
	 */
	ret = dw_pcie_ep_init_registers(ep);
	if (ret) {
		/* [한국어] config 재프로그래밍이 실패했음을 남기고, */
		dev_err(dev, "Failed to complete initialization: %d\n", ret);
		/* [한국어] 자원과 PM 참조를 모두 되돌리는 경로로 간다. */
		goto err_disable_resources;
	}

	pci_epc_init_notify(ep->epc);

	/* Enable link training */
	regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
			   STM32MP25_PCIECR_LTSSM_EN,
			   STM32MP25_PCIECR_LTSSM_EN);

	return;

err_disable_resources:
	stm32_pcie_disable_resources(stm32_pcie);

err_pm_put_sync:
	pm_runtime_put_sync(dev);
}

/* [한국어]
 * stm32_pcie_ep_perst_irq_thread - PERST# 변화를 읽어 두 갈래로 보낸다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @data: 등록 시 넘겨 둔 드라이버 상태.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 이 파일의 유일한 인터럽트 핸들러이며, 스레드 판이다. 처리 안에서 PHY
 * 초기화와 런타임 PM 이 일어나 잠들 수 있어, 인터럽트 문맥에서는 할 수 없다.
 *
 * **트리거 방향을 뒤집는 마지막 줄이 이 함수의 핵심이다.** 이 GPIO 는 에지가
 * 아니라 레벨 트리거로 등록되어 있어, 지금 상태 그대로 두면 같은 인터럽트가
 * 계속 다시 올라온다. 그래서 처리한 방향의 반대를 다음 트리거로 설정한다 —
 * 어서트를 처리했으면 다음은 로우(디어서트)를 기다리고, 그 반대도 마찬가지다.
 *
 * 레벨 트리거를 쓰는 이유는 놓침을 막기 위해서로 보인다. 에지 트리거라면
 * 스레드가 도는 동안 일어난 변화를 놓칠 수 있지만, 레벨은 현재 상태가
 * 남아 있어 다음 확인에서 잡힌다.
 *
 * GPIO 값을 읽어 판단하는 것도 같은 맥락이다. 인터럽트가 왔다는 사실이
 * 아니라 **지금의 실제 값** 으로 갈래를 정하므로, 처리가 늦어 그 사이에
 * 상태가 바뀌었더라도 최신 상태를 따른다.
 *
 * IRQF_ONESHOT 으로 등록되어 스레드가 도는 동안 인터럽트가 마스크된다.
 * 그 덕분에 두 갈래가 겹쳐 실행되지 않아 이 파일에 잠금이 없어도 된다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 아래 두 함수가 반환값이 없어, 실패해도 언제나 IRQ_HANDLED 다.
 *
 * 호출 체인:
 *   PERST# GPIO 인터럽트 → [이 함수]
 *     → gpiod_get_value() → stm32_pcie_perst_assert() / _deassert()
 *     → irq_set_irq_type()
 */
static irqreturn_t stm32_pcie_ep_perst_irq_thread(int irq, void *data)
{
	struct stm32_pcie *stm32_pcie = data;
	struct dw_pcie *pci = &stm32_pcie->pci;
	/* [한국어] 지금 읽은 PERST# 값. */
	u32 perst;

	perst = gpiod_get_value(stm32_pcie->perst_gpio);
	/* [한국어] 어서트(1)면 — 호스트가 리셋을 걸었다는 뜻이다. */
	if (perst)
		/* [한국어] 내려가는 갈래로 간다. */
		stm32_pcie_perst_assert(pci);
	else
		stm32_pcie_perst_deassert(pci);

	irq_set_irq_type(gpiod_to_irq(stm32_pcie->perst_gpio),
			 (perst ? IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW));

	return IRQ_HANDLED;
}

/* [한국어]
 * stm32_add_pcie_ep - EP 모드로 설정하고 엔드포인트 컨트롤러를 등록한다
 *
 * @stm32_pcie: 드라이버 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * probe 의 뒷부분을 떼어 낸 함수이며, 이 SoC 를 엔드포인트로 만드는 지점이다.
 *
 * syscon 으로 타입 필드를 EP 로 쓰는 것이 첫 단계다. 같은 하드웨어가
 * 호스트로도 동작할 수 있어(pcie-stm32.c 가 그쪽이다) 모드를 명시해야 한다.
 *
 * 리셋을 걸었다 푸는 것이 그 다음이다. 모드를 바꾼 뒤에는 컨트롤러를
 * 리셋해야 새 모드로 동작한다 — 두 줄이 붙어 있고 사이에 지연이 없는데,
 * 리셋 컨트롤러가 필요한 유지 시간을 알아서 지킨다는 전제다.
 *
 * page_size 를 능력 서술자의 정렬 값으로 채운다. 두 곳이 같은 값을 갖게
 * 하는 것으로, EPF 가 어느 쪽을 보든 같은 답을 얻는다.
 *
 * 자원 켜기를 dw_pcie_ep_init() **뒤** 에 하는 순서에 주의할 만하다.
 * 초기화가 실패하면 자원을 켜지 않고 물러나므로, 실패 경로에서 끌 것이 없다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: syscon 쓰기와 EP 초기화의 오류를 올려보내고, 자원 켜기가
 * 실패하면 EP 초기화를 되돌린다.
 *
 * 호출 체인:
 *   stm32_pcie_probe() → [이 함수]
 *     → regmap_update_bits(EP 모드) → reset_control_assert/deassert()
 *     → dw_pcie_ep_init() → stm32_pcie_enable_resources()
 */
static int stm32_add_pcie_ep(struct stm32_pcie *stm32_pcie,
			     struct platform_device *pdev)
{
	struct dw_pcie_ep *ep = &stm32_pcie->pci.ep;
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 결과. */
	int ret;

	ret = regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
				 /* [한국어] 타입 필드만 갈아 끼운다 — 같은 하드웨어가 호스트로도 동작할 수 있어
				  * 모드를 명시해야 한다. */
				 STM32MP25_PCIECR_TYPE_MASK,
				 STM32MP25_PCIECR_EP);
	if (ret)
		/* [한국어] 모드 설정이 실패하면 EP 로 동작할 수 없다. */
		return ret;

	reset_control_assert(stm32_pcie->rst);
	reset_control_deassert(stm32_pcie->rst);

	ep->ops = &stm32_pcie_ep_ops;
/* [한국어] 모드를 바꾼 뒤 컨트롤러를 리셋해야 새 모드로 동작한다.
 * 두 줄 사이에 지연이 없는데, 리셋 컨트롤러가 필요한 유지 시간을
 * 알아서 지킨다는 전제다. */

	ep->page_size = stm32_pcie_epc_features.align;
/* [한국어] EP 쪽 콜백 표를 건다. */

	ret = dw_pcie_ep_init(ep);
	/* [한국어] EPC 등록이 실패하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to initialize ep: %d\n", ret);
		/* [한국어] 물러난다. 아직 자원을 켜지 않아 되돌릴 것이 없다. */
		return ret;
	}

	ret = stm32_pcie_enable_resources(stm32_pcie);
	/* [한국어] 자원 켜기가 실패하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to enable resources: %d\n", ret);
		/* [한국어] 방금 등록한 EPC 를 되돌린다. */
		dw_pcie_ep_deinit(ep);
		return ret;
	}

	return 0;
}

/* [한국어]
 * stm32_pcie_probe - 자원을 얻고 PERST# 인터럽트를 준비한 뒤 EP 를 등록한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * 자원을 다섯 얻는다 — syscon regmap, PHY, 클럭, 리셋, PERST# GPIO.
 * syscon 을 compatible 문자열로 찾는 것이 눈에 띄는데, 디바이스 트리의
 * phandle 이 아니라 시스템 전체에서 그 노드를 찾는 방식이다.
 *
 * **IRQ_NOAUTOEN 이 이 probe 의 핵심이다**(옆의 상류 주석). 인터럽트를
 * 등록하되 켜지 않는데, 켜 두면 EPF 드라이버가 준비를 마치기 전에 호스트의
 * PERST# 를 처리하게 된다. 켜는 것은 stm32_pcie_start_link() 의 몫이다.
 *
 * 런타임 PM 참조를 get_noresume 으로 미리 올려 두는 것도 같은 취지다.
 * probe 동안 장치가 절전에 들어가지 못하게 붙잡고, 이후 PERST# 어서트가
 * 그것을 놓아 준다.
 *
 * 그래서 오류 경로마다 put_noidle 이 붙는다 — 실패하면 그 참조를 되돌려야
 * 장치가 정상적으로 절전에 들어갈 수 있다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 자원 확보 실패는 dev_err_probe 로 기록하고 올려보낸다.
 * PM 참조를 올린 뒤의 실패는 모두 put_noidle 로 그것을 되돌린다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → syscon_regmap_lookup_by_compatible() → devm_phy_get()
 *     → devm_clk_get() → devm_gpiod_get() → devm_pm_runtime_enable()
 *     → devm_request_threaded_irq() → stm32_add_pcie_ep()
 */
static int stm32_pcie_probe(struct platform_device *pdev)
{
	struct stm32_pcie *stm32_pcie;
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 결과. */
	int ret;

	stm32_pcie = devm_kzalloc(dev, sizeof(*stm32_pcie), GFP_KERNEL);
	/* [한국어] 상태 구조를 잡지 못하면, */
	if (!stm32_pcie)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	stm32_pcie->pci.dev = dev;
	/* [한국어] 코어 콜백 표를 건다 — start_link 와 stop_link 둘이다. */
	stm32_pcie->pci.ops = &dw_pcie_ops;

	stm32_pcie->regmap = syscon_regmap_lookup_by_compatible("st,stm32mp25-syscfg");
	/* [한국어] syscon 을 찾지 못하면, */
	if (IS_ERR(stm32_pcie->regmap))
		/* [한국어] 오류 포인터에서 코드를 꺼내 */
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->regmap),
				     /* [한국어] 올려보낸다. phandle 이 아니라 compatible 문자열로 찾는 방식이라,
				      * 시스템 전체에서 그 노드를 뒤진다. */
				     "No syscfg specified\n");

	stm32_pcie->phy = devm_phy_get(dev, NULL);
	/* [한국어] PHY 를 얻지 못하면, */
	if (IS_ERR(stm32_pcie->phy))
		/* [한국어] 오류 코드를 꺼내 */
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->phy),
				     /* [한국어] 올려보낸다. */
				     "failed to get pcie-phy\n");

	stm32_pcie->clk = devm_clk_get(dev, NULL);
	/* [한국어] 클럭을 얻지 못하면, */
	if (IS_ERR(stm32_pcie->clk))
		/* [한국어] 오류 코드를 꺼내 */
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->clk),
				     /* [한국어] 올려보낸다. */
				     "Failed to get PCIe clock source\n");

	stm32_pcie->rst = devm_reset_control_get_exclusive(dev, NULL);
	/* [한국어] 리셋을 얻지 못하면, */
	if (IS_ERR(stm32_pcie->rst))
		/* [한국어] 오류 코드를 꺼내 */
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->rst),
				     /* [한국어] 올려보낸다. */
				     "Failed to get PCIe reset\n");

	stm32_pcie->perst_gpio = devm_gpiod_get(dev, "reset", GPIOD_IN);
	/* [한국어] PERST# GPIO 를 얻지 못하면, */
	if (IS_ERR(stm32_pcie->perst_gpio))
		/* [한국어] 오류 코드를 꺼내 */
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->perst_gpio),
				     /* [한국어] 올려보낸다. 이 GPIO 가 없으면 호스트의 신호를 받을 방법이 없다. */
				     "Failed to get reset GPIO\n");

	ret = phy_set_mode(stm32_pcie->phy, PHY_MODE_PCIE);
	/* [한국어] PHY 모드 설정이 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. 여기까지는 PM 참조를 올리기 전이라 되돌릴 것이 없다. */
		return ret;

	platform_set_drvdata(pdev, stm32_pcie);
/* [한국어] **probe 동안 장치가 절전에 들어가지 못하게 붙잡는다.** 이 참조는
 * PERST# 어서트가 놓아 주며, 그래서 아래 오류 경로마다 put_noidle 이 붙는다. */

	pm_runtime_get_noresume(dev);

	ret = devm_pm_runtime_enable(dev);
	/* [한국어] 런타임 PM 활성화가 실패하면, */
	if (ret < 0) {
		/* [한국어] 방금 올린 참조를 되돌린다. */
		pm_runtime_put_noidle(&pdev->dev);
		return dev_err_probe(dev, ret, "Failed to enable runtime PM\n");
	/* [한국어] PM 활성화 실패 처리 끝. */
	}

	stm32_pcie->perst_irq = gpiod_to_irq(stm32_pcie->perst_gpio);
/* [한국어] GPIO 에 대응하는 인터럽트 번호를 얻는다. */

	/* Will be enabled in start_link when device is initialized. */
	irq_set_status_flags(stm32_pcie->perst_irq, IRQ_NOAUTOEN);

	ret = devm_request_threaded_irq(dev, stm32_pcie->perst_irq, NULL,
					/* [한국어] 위쪽 핸들러 없이 스레드만 등록한다 — 처리 안에서 PHY 초기화와
					 * 런타임 PM 이 일어나 잠들 수 있기 때문이다. */
					stm32_pcie_ep_perst_irq_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"perst_irq", stm32_pcie);
	if (ret) {
		/* [한국어] 인터럽트 등록이 실패하면 참조를 되돌린다. */
		pm_runtime_put_noidle(&pdev->dev);
		return dev_err_probe(dev, ret, "Failed to request PERST IRQ\n");
	/* [한국어] 인터럽트 등록 실패 처리 끝. */
	}

	ret = stm32_add_pcie_ep(stm32_pcie, pdev);
	/* [한국어] EP 등록이 실패하면, */
	if (ret)
		/* [한국어] 참조를 되돌린다. 그 뒤 아래에서 그 오류가 나간다. */
		pm_runtime_put_noidle(&pdev->dev);

	return ret;
}

/* [한국어]
 * stm32_pcie_remove - 엔드포인트를 내리고 자원을 놓는다
 *
 * @pdev: 플랫폼 장치.
 *
 * 순서가 이 함수의 뼈대이며, probe 와 PERST# 처리의 역순이다.
 *
 * 1. dw_pcie_stop_link() 로 PERST# 인터럽트를 끈다. **가장 먼저** 해야 하는데,
 *    정리 도중에 PERST# 가 들어오면 해제 중인 자원을 다시 켜려 든다.
 *    그 안의 disable_irq() 가 실행 중인 스레드를 기다려 주므로, 이 줄이
 *    돌아온 뒤에는 PERST# 처리가 진행 중이지 않다.
 * 2. EPC 계층에 해제를 알린다. EPF 드라이버가 먼저 정리해야 한다.
 * 3. EP 를 해제한다.
 * 4. 자원을 끈다.
 * 5. probe 가 올려 둔 런타임 PM 참조를 놓는다.
 *
 * PERST# 가 어서트된 상태에서 remove 가 불리면 4번이 이미 꺼진 자원을
 * 다시 끄게 되는데, 두 해제 함수가 그 경우에 안전한지는 이 트리에서
 * 확인 못 함.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → dw_pcie_stop_link() → pci_epc_deinit_notify() → dw_pcie_ep_deinit()
 *     → stm32_pcie_disable_resources() → pm_runtime_put_sync()
 */
static void stm32_pcie_remove(struct platform_device *pdev)
{
	struct stm32_pcie *stm32_pcie = platform_get_drvdata(pdev);
	struct dw_pcie *pci = &stm32_pcie->pci;
	/* [한국어] 해제 알림과 EP 해제에 쓸 엔드포인트 문맥. */
	struct dw_pcie_ep *ep = &pci->ep;

	dw_pcie_stop_link(pci);

	pci_epc_deinit_notify(ep->epc);
	dw_pcie_ep_deinit(ep);

	stm32_pcie_disable_resources(stm32_pcie);

	pm_runtime_put_sync(&pdev->dev);
}

static const struct of_device_id stm32_pcie_ep_of_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다 — 호스트 쪽(pcie-stm32.c)과
	 * 다른 compatible 이라, 같은 하드웨어를 어느 모드로 쓸지는 트리가 정한다. */
	{ .compatible = "st,stm32mp25-pcie-ep" },
	/* [한국어] 표의 끝 표시. */
	{},
};

static struct platform_driver stm32_pcie_ep_driver = {
	/* [한국어] 이 드라이버는 remove 를 지원한다 — 호스트 브리지와 달리
	 * 엔드포인트는 런타임에 떼어도 시스템이 무너지지 않는다. */
	.probe = stm32_pcie_probe,
	/* [한국어] 그 remove 콜백. */
	.remove = stm32_pcie_remove,
	.driver = {
		.name = "stm32-ep-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = stm32_pcie_ep_of_match,
	},
};

module_platform_driver(stm32_pcie_ep_driver);

MODULE_AUTHOR("Christian Bruel <christian.bruel@foss.st.com>");
MODULE_DESCRIPTION("STM32MP25 PCIe Endpoint Controller driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, stm32_pcie_ep_of_match);
