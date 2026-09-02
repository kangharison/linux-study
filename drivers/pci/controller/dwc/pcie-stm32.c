// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics STM32MP25 PCIe root complex driver.
 *
 * Copyright (C) 2025 STMicroelectronics
 * Author: Christian Bruel <christian.bruel@foss.st.com>
 */

/*
 * [한국어 설명] STM32MP25 의 DesignWare PCIe 루트 컴플렉스 글루 (pcie-stm32.c)
 *
 * === 파일의 역할 ===
 * STM32MP25 SoC 를 PCIe **루트 컴플렉스** 로 동작시키는 드라이버다.
 * 같은 SoC 의 엔드포인트 드라이버(pcie-stm32-ep.c)와 짝을 이루며,
 * 두 파일이 같은 하드웨어를 서로 반대 방향으로 쓴다. 어느 쪽으로 동작할지는
 * pcie-stm32.h 가 정의한 SYSCFG_PCIECR 의 타입 필드가 정하고,
 * 디바이스 트리의 compatible 문자열이 어느 드라이버가 붙을지를 정한다.
 *
 * 이 파일이 맡는 것은 셋이다.
 *   1) 모드 설정과 링크 시작 — syscon 으로 RC 모드를 지정하고, 준비가 끝나면
 *      LTSSM 비트를 세운다.
 *   2) PERST# 출력 — 엔드포인트 판이 PERST# 를 **받는** 것과 반대로,
 *      여기서는 하위 장치에 리셋을 **내보낸다.** 그래서 같은 이름의 GPIO 가
 *      이 파일에서는 출력이다.
 *   3) 절전과 깨우기 — noirq 단계의 서스펜드·리줌 한 쌍, 그리고 하위 장치가
 *      WAKE# 로 시스템을 깨울 수 있게 하는 전용 wake IRQ.
 *
 * dw_pcie_host_ops 가 **비어 있는 것** 이 이 파일의 성격을 말해 준다.
 * DWC 코어가 호스트 초기화 도중에 물어볼 것이 하나도 없다는 뜻이며,
 * SoC 고유의 준비가 모두 probe 안에서 끝나기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> stm32_pcie_probe()
 *     -> syscon, 클럭, 리셋을 얻고, 루트 포트 자식 노드에서 PHY 와
 *        PERST#/WAKE# GPIO 를 얻는다(stm32_pcie_parse_port)
 *     -> stm32_add_pcie_port()
 *        -> PHY 를 PCIe 모드로 초기화하고 RC 모드를 지정
 *        -> PERST# 를 풀어 하위 장치를 깨운다
 *        -> WAKE# 가 있으면 전용 wake IRQ 로 등록
 *     -> 컨트롤러 리셋을 한 번 돌리고 클럭을 켠다
 *     -> dw_pcie_host_init()  [pcie-designware-host.c]
 *        -> 코어가 start_link 콜백을 부른다 -> [이 파일] LTSSM 켜기
 *        -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * 절전 시:
 *   noirq 단계 -> [이 파일] stm32_pcie_suspend_noirq()
 *     -> DWC 코어에 상태 저장을 맡기고, PERST# 를 걸고, 클럭을 끄고,
 *        깨우기 경로가 아니면 PHY 까지 내린 뒤 핀을 절전 상태로 바꾼다
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 인터럽트 핸들러가 없다 —
 * INTx 와 MSI 는 DWC 코어가 다루고, WAKE# 는 전용 wake IRQ 로 등록해
 * PM 코어가 처리한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c. 접점은 dw_pcie_ops
 *   의 콜백 둘(start_link, stop_link)과, 절전 경로가 부르는
 *   dw_pcie_suspend_noirq()/resume_noirq() 다.
 * 옆쪽: pcie-stm32.h 가 엔드포인트 드라이버와 공유하는 syscon 정의를 담는다.
 *   pinctrl 계층과도 이어지는데, resume 경로가 핀 상태를 바꿔야만 DBI
 *   레지스터에 접근할 수 있기 때문이다(해당 자리의 상류 주석이 근거를 밝힌다).
 *
 * 데이터 흐름:
 *   디바이스 트리(syscon, 클럭, 리셋, 루트 포트 자식 노드의 PHY 와 두 GPIO)
 *     -> probe -> struct stm32_pcie
 *   PHY 와 GPIO 를 **자식 노드에서** 얻는 것이 이 파일의 특징이다.
 *   PCIe 루트 포트가 트리에서 별도 노드로 서술되고, 그 아래 링크에 관한
 *   자원들이 매달려 있기 때문이다.
 *
 * 공유 상태: struct stm32_pcie 하나. probe 후 불변이며 잠금이 없다 —
 *   절전·복귀가 유일한 후속 경로이고, PM 코어가 그것을 직렬화한다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면, 시스템 절전 때
 * stm32_pcie_suspend_noirq() 가 PERST# 를 걸어 그 컨트롤러를 리셋 상태로
 * 만든다. 그래서 복귀 뒤 nvme 드라이버가 컨트롤러를 처음부터 다시
 * 초기화해야 하며, 그 순서를 어기지 않도록 이 파일이 noirq 단계에서
 * DWC 코어의 상태 저장·복원과 PERST# 조작을 함께 처리한다.
 *
 * === 주요 함수/구조체 요약 ===
 * stm32_pcie_probe()          : 자원 확보부터 DWC 호스트 초기화까지.
 * stm32_add_pcie_port()       : PHY·모드·PERST#·wake IRQ 를 준비한다.
 * stm32_pcie_parse_port()     : 루트 포트 자식 노드에서 PHY 와 두 GPIO 를 얻는다.
 * stm32_pcie_suspend_noirq()  : 절전 진입. 상태 저장 -> PERST# -> 클럭 -> PHY.
 * stm32_pcie_resume_noirq()   : 그 역순. 핀 상태를 먼저 바꾸는 것이 요점이다.
 * stm32_pcie_start_link()     : LTSSM 비트 하나를 세운다.
 * struct stm32_pcie           : dw_pcie 를 맨 앞에 둔 이 드라이버의 상태.
 */

/* [한국어] clk_prepare_enable()/clk_disable_unprepare(). */
#include <linux/clk.h>
/* [한국어] msleep(). PERST# 해제 전후의 규격 대기 두 곳이 쓴다. */
#include <linux/delay.h>
/* [한국어] dev_get_drvdata() 와 dev_err(). */
#include <linux/device.h>
/* [한국어] IS_ERR()/PTR_ERR(). */
#include <linux/err.h>
/* [한국어] gpiod_set_value() 와 gpiod_to_irq(). PERST# 와 WAKE# 를 GPIO 로 다룬다. */
#include <linux/gpio/consumer.h>
/* [한국어] irq_set_irq_type(). WAKE# 의 트리거를 하강 에지로 바꾸는 데 쓴다. */
#include <linux/irq.h>
/* [한국어] syscon_regmap_lookup_by_compatible(). SoC 의 SYSCFG 블록을 찾는다. */
#include <linux/mfd/syscon.h>
/* [한국어] struct of_device_id. */
#include <linux/mod_devicetable.h>
/* [한국어] MODULE_ 매크로들. */
#include <linux/module.h>
/* [한국어] of_get_next_available_child() 와 of_fwnode_handle(). */
#include <linux/of.h>
/* [한국어] of_platform 헤더. */
#include <linux/of_platform.h>
/* [한국어] phy_init()/phy_exit()/phy_set_mode(). */
#include <linux/phy/phy.h>
/* [한국어] pinctrl_pm_select_*_state(). **복귀 경로가 이것 없이는 DBI 에 접근할 수 없다** —
 * 코어 클럭이 CLKREQ# 로 게이팅되어 있기 때문이다. */
#include <linux/pinctrl/consumer.h>
/* [한국어] struct platform_device. */
#include <linux/platform_device.h>
/* [한국어] dev_pm_ops 와 NOIRQ_SYSTEM_SLEEP_PM_OPS. */
#include <linux/pm.h>
/* [한국어] 런타임 PM. 이 드라이버는 no_callbacks 로 등록해 실제 콜백은 두지 않는다. */
#include <linux/pm_runtime.h>
/* [한국어] dev_pm_set_dedicated_wake_irq()/clear_wake_irq(). WAKE# 를 PM 코어에 맡긴다. */
#include <linux/pm_wakeirq.h>
/* [한국어] regmap_update_bits(). syscon 레지스터를 비트 단위로 고친다. */
#include <linux/regmap.h>
/* [한국어] reset_control_assert()/deassert(). */
#include <linux/reset.h>
/* [한국어] NULL 등 기본 정의. */
#include <linux/stddef.h>

/* [한국어] PCIE_T_PVPERL_MS 와 PCIE_RESET_CONFIG_WAIT_MS — 규격이 정한 대기 시간 상수가
 * 여기 있다. drivers/pci 안에서만 쓰는 헤더다. */
#include "../../pci.h"

/* [한국어] DWC 코어의 선언 전부. */
#include "pcie-designware.h"
/* [한국어] 엔드포인트 드라이버와 공유하는 syscon 정의 — SYSCFG_PCIECR 과
 * 그 안의 타입·LTSSM 비트, 그리고 to_stm32_pcie 변환 매크로. */
#include "pcie-stm32.h"

/* [한국어] 이 드라이버의 상태 전부. */
struct stm32_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **맨 앞에 두어** to_stm32_pcie 변환이 성립한다.
	 * 설정자: probe 가 dev 와 두 콜백 표를 채우고, DWC 코어가 나머지를 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 모든 콜백.
	 * 값 범위: DWC 코어가 정의한 구조체.
	 * 동기화: 코어가 관리한다. */
	struct dw_pcie pci;
	/* [한국어] SoC 의 SYSCFG 블록을 가리키는 regmap.
	 * 설정자: probe 의 syscon_regmap_lookup_by_compatible("st,stm32mp25-syscfg").
	 * 읽는 자: 모드 설정(stm32_add_pcie_port)과 LTSSM 제어(start_link/stop_link).
	 * 값 범위: 유효한 regmap 포인터. syscon 은 여러 드라이버가 공유하는 블록이라
	 * 동시 접근은 regmap 계층이 지킨다.
	 * 동기화: probe 후 불변. */
	struct regmap *regmap;
	/* [한국어] PCIe 컨트롤러 리셋.
	 * 설정자: probe 의 devm_reset_control_get_exclusive().
	 * 읽는 자: probe 가 PHY·모드 설정을 마친 뒤 한 번 걸었다 푼다.
	 * 값 범위: 유효한 리셋 컨트롤.
	 * 동기화: probe 후 불변. */
	struct reset_control *rst;
	/* [한국어] PCIe PHY.
	 * 설정자: stm32_pcie_parse_port() 가 **루트 포트 자식 노드** 에서 얻는다.
	 * 읽는 자: stm32_add_pcie_port(), 절전·복귀 경로, stm32_remove_pcie_port().
	 * 값 범위: 유효한 PHY 포인터.
	 * 동기화: probe 후 불변. 깨우기 경로일 때는 절전 중에도 살려 둔다. */
	struct phy *phy;
	/* [한국어] PCIe 코어 클럭.
	 * 설정자: probe 의 devm_clk_get().
	 * 읽는 자: probe 와 절전·복귀 경로가 켜고 끈다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: probe 후 불변. 이 클럭이 COMBOPHY 의 CLKREQ# 로 게이팅되어 있어,
	 * 복귀 경로가 핀 상태를 먼저 바꿔야 하는 이유가 된다. */
	struct clk *clk;
	/* [한국어] 하위 장치로 내보내는 PERST# GPIO.
	 * 설정자: stm32_pcie_parse_port() 의 devm_fwnode_gpiod_get("reset", GPIOD_OUT_HIGH)
	 * — **출력** 이며, 얻는 순간 리셋이 걸린 상태가 된다.
	 * 읽는 자: assert/deassert 두 함수.
	 * 값 범위: 유효한 GPIO 서술자 또는 NULL(선택 사항이라 없는 보드가 있다).
	 * 동기화: probe 후 불변.
	 * 엔드포인트 판의 같은 이름 GPIO 가 **입력** 인 것과 정반대다. */
	struct gpio_desc *perst_gpio;
	/* [한국어] 하위 장치가 보내는 WAKE# GPIO.
	 * 설정자: stm32_pcie_parse_port() 의 devm_fwnode_gpiod_get("wake", GPIOD_IN).
	 * 읽는 자: stm32_add_pcie_port() 가 전용 wake IRQ 로 등록하고,
	 * probe/remove 가 깨우기 설정을 켜고 끌지 판단하는 데 쓴다.
	 * 값 범위: 유효한 GPIO 서술자 또는 NULL.
	 * 동기화: probe 후 불변. 이 파일에만 있고 엔드포인트 판에는 없다. */
	struct gpio_desc *wake_gpio;
};

/* [한국어]
 * stm32_pcie_deassert_perst - 하위 장치의 리셋을 풀고 규격이 정한 시간을 기다린다
 *
 * @stm32_pcie: 드라이버 상태.
 *
 * 엔드포인트 판과 방향이 반대다 — 여기서는 이 SoC 가 PERST# 를 **내보내는**
 * 쪽이라, GPIO 가 출력이고 이 함수가 그 값을 내린다.
 *
 * 기다림이 둘이고 뜻이 다르다.
 * 1. PERST# 를 내리기 **전** 의 T_PVPERL — 전원이 안정된 뒤 리셋을 풀기까지
 *    규격이 요구하는 최소 시간이다. 그 전에 풀면 하위 장치가 불안정한 전원에서
 *    깨어난다.
 * 2. 내린 **뒤** 의 대기 — 장치가 config 접근을 받아들일 준비를 마칠 때까지
 *    기다리는 시간이다.
 *
 * GPIO 가 없으면 첫 대기와 값 쓰기를 건너뛰고 두 번째 대기만 한다. PERST# 를
 * 소프트웨어로 제어하지 않는 보드에서는 전원이 켜지는 순간 이미 리셋이
 * 풀려 있으므로, 남는 것은 장치가 준비될 때까지 기다리는 일뿐이다.
 *
 * 실행 컨텍스트: probe 와 resume. msleep 이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   stm32_add_pcie_port() / stm32_pcie_resume_noirq() → [이 함수]
 *     → msleep() → gpiod_set_value()
 */
static void stm32_pcie_deassert_perst(struct stm32_pcie *stm32_pcie)
{
	if (stm32_pcie->perst_gpio) {
		msleep(PCIE_T_PVPERL_MS);
		gpiod_set_value(stm32_pcie->perst_gpio, 0);
	/* [한국어] GPIO 가 없어도 두 번째 대기는 한다 — 리셋을 소프트웨어로 제어하지 않는
	 * 보드에서도 장치가 준비될 시간은 필요하기 때문이다. */
	}

	msleep(PCIE_RESET_CONFIG_WAIT_MS);
}

/* [한국어]
 * stm32_pcie_assert_perst - 하위 장치에 리셋을 건다
 *
 * @stm32_pcie: 드라이버 상태.
 *
 * stm32_pcie_deassert_perst() 의 짝이며 한 줄이다.
 *
 * 푸는 쪽과 달리 **GPIO 유무를 검사하지 않는다.** 푸는 쪽은 검사한 뒤에만
 * 값을 쓰는데, 이쪽은 조건 없이 부른다. gpiod_set_value() 가 NULL 서술자를
 * 어떻게 다루는지는 이 트리에 drivers/gpio 가 없어 확인 못 함 — 다만 두
 * 함수의 비대칭 자체는 코드에서 읽히는 사실이다.
 *
 * 기다림도 없다. 리셋을 거는 것은 즉시 효력이 생기고, 그 뒤에 이어지는
 * 동작(클럭 끄기, PHY 내리기)이 자연스레 시간을 주기 때문이다.
 *
 * 실행 컨텍스트: 절전 진입과 오류 되감기. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   stm32_pcie_suspend_noirq() / stm32_remove_pcie_port() /
 *   stm32_pcie_resume_noirq() 의 되감기 → [이 함수] → gpiod_set_value()
 */
static void stm32_pcie_assert_perst(struct stm32_pcie *stm32_pcie)
{
	gpiod_set_value(stm32_pcie->perst_gpio, 1);
}

/* [한국어]
 * stm32_pcie_start_link - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 0 = 성공, regmap 오류.
 *
 * 엔드포인트 판이 "호스트를 기다리기 시작한다" 는 뜻이었던 것과 달리,
 * 루트 컴플렉스에서는 이것이 **실제로 링크를 세우는** 동작이다.
 *
 * syscon 의 비트 하나를 세우는 것이 전부다. LTSSM 비트가 타입 필드 바깥에
 * 있어(pcie-stm32.h 참조) 모드와 독립적으로 켜고 끌 수 있으며, 그래서
 * 모드 설정이 끝난 뒤 마지막에 이것만 따로 세운다.
 *
 * DWC 코어가 이 콜백을 부른 뒤 링크가 설 때까지 기다리므로, 이 함수 자체는
 * 기다리지 않는다.
 *
 * 실행 컨텍스트: DWC 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: regmap 쓰기 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수] → regmap_update_bits()
 */
static int stm32_pcie_start_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);

	return regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
				  /* [한국어] LTSSM 비트만 골라, */
				  STM32MP25_PCIECR_LTSSM_EN,
				  STM32MP25_PCIECR_LTSSM_EN);
}

/* [한국어]
 * stm32_pcie_stop_link - LTSSM 을 꺼 링크를 내린다
 *
 * @pci: DWC 코어의 문맥.
 *
 * stm32_pcie_start_link() 의 짝이며 같은 비트를 지운다.
 *
 * 반환값이 없어 regmap 쓰기 실패를 알리지 못한다. 시작 쪽이 int 를 돌려주는
 * 것과 대비되는데, 콜백 표가 요구하는 형태가 그렇다.
 *
 * 실행 컨텍스트: DWC 코어의 정리 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.stop_link == [이 함수] → regmap_update_bits()
 */
static void stm32_pcie_stop_link(struct dw_pcie *pci)
{
	struct stm32_pcie *stm32_pcie = to_stm32_pcie(pci);

	regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
			   /* [한국어] 같은 비트를 지운다. 타입 필드 바깥에 있어 모드와 독립적으로 다룰 수 있다. */
			   STM32MP25_PCIECR_LTSSM_EN, 0);
}

/* [한국어]
 * stm32_pcie_suspend_noirq - 절전 진입 시 상태를 저장하고 자원을 내린다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 = 성공, 음수 오류.
 *
 * noirq 단계에서 도는 이유는 이 안에서 링크를 내리기 때문이다. 인터럽트가
 * 아직 살아 있는 단계에서 링크를 끊으면 그 사이에 온 인터럽트를 처리할 수 없다.
 *
 * 순서가 위에서 아래로 "논리적인 것부터 물리적인 것" 이다.
 * 1. DWC 코어에 상태 저장을 맡긴다. 이것이 실패하면 아무것도 내리지 않고
 *    물러나 절전을 취소한다.
 * 2. PERST# 를 걸어 하위 장치를 리셋 상태로 만든다.
 * 3. 클럭을 끈다.
 * 4. **깨우기 경로가 아닐 때만** PHY 를 내린다.
 * 5. 핀을 절전 상태로 바꾼다.
 *
 * 4번의 조건이 이 함수의 요점이다. 하위 장치가 WAKE# 로 시스템을 깨울 수
 * 있으려면 그 신호를 받을 PHY 가 살아 있어야 한다. device_wakeup_path() 가
 * "이 장치를 통해 깨우기 신호가 지나가는가" 를 답해 주며, 참이면 PHY 를
 * 남겨 둔다.
 *
 * 핀 상태 변경을 마지막에 하는 것도 그와 맞물린다 — 절전 상태의 핀 설정이
 * 깨우기에 필요한 핀은 살려 두도록 디바이스 트리에 쓰여 있다는 전제다.
 *
 * 실행 컨텍스트: 시스템 절전의 noirq 단계. 인터럽트가 꺼진 상태이며
 * 프로세스 컨텍스트다.
 *
 * 에러 경로: 상태 저장이 실패하면 아무것도 내리지 않고 그 오류를 올려보낸다.
 * 그 뒤 단계들은 실패를 확인하지 않는다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq == [이 함수]
 *     → dw_pcie_suspend_noirq() → stm32_pcie_assert_perst()
 *     → clk_disable_unprepare() → phy_exit()
 *     → pinctrl_pm_select_sleep_state()
 */
static int stm32_pcie_suspend_noirq(struct device *dev)
{
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);
	int ret;

	ret = dw_pcie_suspend_noirq(&stm32_pcie->pci);
	/* [한국어] DWC 코어의 상태 저장이 실패하면, */
	if (ret)
		/* [한국어] 아무것도 내리지 않고 물러나 절전을 취소한다. */
		return ret;

	stm32_pcie_assert_perst(stm32_pcie);

	clk_disable_unprepare(stm32_pcie->clk);

	if (!device_wakeup_path(dev))
		/* [한국어] 깨우기 경로가 아닐 때만 PHY 를 내린다. 하위 장치가 WAKE# 로 시스템을
		 * 깨우려면 그 신호를 받을 PHY 가 살아 있어야 한다. */
		phy_exit(stm32_pcie->phy);

	return pinctrl_pm_select_sleep_state(dev);
}

/* [한국어]
 * stm32_pcie_resume_noirq - 절전에서 깨어나 자원을 되살리고 상태를 복원한다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 = 성공, 음수 오류.
 *
 * stm32_pcie_suspend_noirq() 의 짝이며 순서가 역순이다. 다만 **첫 단계가
 * 추가되어 있고**, 그것이 이 함수에서 가장 중요한 부분이다.
 *
 * 핀 상태를 먼저 init 으로 바꾸는 이유를 위 상류 주석이 밝힌다 — 코어 클럭이
 * COMBOPHY 의 REFCLK 에서 오는 CLKREQ# 로 게이팅되어 있어, 장치가 없으면
 * 그 신호가 오지 않아 클럭이 멎는다. 그 상태에서 DBI 레지스터를 읽으면
 * 접근이 멈춘다. 그래서 pinctrl 의 init 상태가 그 핀을 강제로 구동해
 * 클럭을 살려 둔다.
 *
 * 그 다음은 서스펜드의 역순이다 — PHY(깨우기 경로가 아니었을 때만),
 * 클럭, PERST# 해제, 상태 복원.
 *
 * 마지막에 핀을 default 상태로 되돌린다. init 상태는 복귀 중에만 필요한
 * 임시 설정이기 때문이다.
 *
 * 되감기가 계단이며, 두 라벨 모두 핀을 default 로 되돌리고 끝난다 —
 * 실패하더라도 핀을 init 상태로 남겨 두면 안 되기 때문이다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 실패를 goto 로 앞 단계까지 되감고 오류를 올려보낸다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq == [이 함수]
 *     → pinctrl_pm_select_init_state() → phy_init() → clk_prepare_enable()
 *     → stm32_pcie_deassert_perst() → dw_pcie_resume_noirq()
 *     → pinctrl_pm_select_default_state()
 */
static int stm32_pcie_resume_noirq(struct device *dev)
{
	/* [한국어] PM 코어가 준 device 에서 이 드라이버의 상태를 되찾는다. */
	struct stm32_pcie *stm32_pcie = dev_get_drvdata(dev);
	/* [한국어] 각 단계의 결과. */
	int ret;

	/*
	 * The core clock is gated with CLKREQ# from the COMBOPHY REFCLK,
	 * thus if no device is present, must deassert it with a GPIO from
	 * pinctrl pinmux before accessing the DBI registers.
	 */
	ret = pinctrl_pm_select_init_state(dev);
	if (ret) {
		/* [한국어] 핀 상태 변경이 실패했음을 남기고, */
		dev_err(dev, "Failed to activate pinctrl pm state: %d\n", ret);
		/* [한국어] 물러난다. 이 단계가 실패하면 아래 DBI 접근이 멎을 수 있어 진행할 수 없다. */
		return ret;
	}

	if (!device_wakeup_path(dev)) {
		/* [한국어] 깨우기 경로가 아니었다면 절전 때 PHY 를 내렸으므로 다시 올린다. */
		ret = phy_init(stm32_pcie->phy);
		/* [한국어] PHY 초기화가 실패하면, */
		if (ret) {
			/* [한국어] 핀을 default 로 되돌리고 물러난다 — init 상태로 남겨 두면 안 된다. */
			pinctrl_pm_select_default_state(dev);
			return ret;
		}
	}

	ret = clk_prepare_enable(stm32_pcie->clk);
	/* [한국어] 클럭 켜기가 실패하면, */
	if (ret)
		/* [한국어] PHY 를 내리고 핀을 되돌리는 자리로 뛴다. */
		goto err_phy_exit;

	stm32_pcie_deassert_perst(stm32_pcie);

	ret = dw_pcie_resume_noirq(&stm32_pcie->pci);
	/* [한국어] DWC 코어의 상태 복원이 실패하면, */
	if (ret)
		/* [한국어] PERST# 를 다시 걸고 클럭부터 되돌리는 자리로 뛴다. */
		goto err_disable_clk;

	pinctrl_pm_select_default_state(dev);

	return 0;

err_disable_clk:
	stm32_pcie_assert_perst(stm32_pcie);
	clk_disable_unprepare(stm32_pcie->clk);

err_phy_exit:
	phy_exit(stm32_pcie->phy);
	pinctrl_pm_select_default_state(dev);

	return ret;
}

static const struct dev_pm_ops stm32_pcie_pm_ops = {
	/* [한국어] 서스펜드·리줌을 **noirq 단계** 에만 단다. 이 안에서 링크를 내리므로,
	 * 인터럽트가 살아 있는 단계에서 하면 그 사이에 온 인터럽트를 처리할 수 없다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(stm32_pcie_suspend_noirq,
				  stm32_pcie_resume_noirq)
};

static const struct dw_pcie_host_ops stm32_pcie_host_ops = {
/* [한국어] **비어 있다.** DWC 코어가 호스트 초기화 도중에 이 드라이버에 물어볼 것이
 * 하나도 없다는 뜻이며, SoC 고유의 준비가 모두 probe 안에서 끝나기 때문이다.
 * 엔드포인트 판이 콜백 둘을 두는 것과 대비된다. */
};

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] LTSSM 을 켜는 콜백. */
	.start_link = stm32_pcie_start_link,
	/* [한국어] 끄는 콜백. 이 표에 둘뿐인 것은 나머지를 DWC 코어가 표준대로 처리하기 때문이다. */
	.stop_link = stm32_pcie_stop_link
};

/* [한국어]
 * stm32_add_pcie_port - PHY 와 모드를 준비하고 하위 장치를 깨운다
 *
 * @stm32_pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * probe 에서 SoC 고유의 준비를 모아 둔 함수다. dw_pcie_host_ops 가 비어
 * 있는 것이 여기서 설명되는데, 코어가 물어볼 것을 이 함수가 미리 다 해
 * 두기 때문이다.
 *
 * 네 단계다.
 * 1. PHY 를 PCIe 모드로 지정하고 초기화한다.
 * 2. syscon 으로 RC 모드를 지정한다. 같은 하드웨어가 EP 로도 동작할 수 있어
 *    명시해야 한다.
 * 3. PERST# 를 풀어 하위 장치를 깨운다.
 * 4. WAKE# GPIO 가 있으면 전용 wake IRQ 로 등록한다.
 *
 * 4번이 이 파일에만 있는 것이다(엔드포인트 판에는 없다). 하위 장치가
 * 시스템을 깨울 수 있게 하는 경로이며, 전용 wake IRQ 로 등록하면 PM 코어가
 * 절전 중에 그 인터럽트를 활성화하고 깨어나면 비활성화하는 일을 대신한다.
 *
 * 트리거를 하강 에지로 바꾸는 것이 등록 **뒤** 인 것에 주의할 만하다.
 * WAKE# 는 로우 액티브라 하강 에지가 "깨워 달라" 는 신호다.
 *
 * 되감기가 계단이다 — wake IRQ 등록이 실패하면 PERST# 를 다시 걸고 PHY 를
 * 내리며, 모드 설정이 실패하면 PHY 만 내린다.
 *
 * 실행 컨텍스트: probe. PHY 초기화와 msleep 이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 각 단계의 실패를 goto 로 앞 단계까지 되감고 오류를 올려보낸다.
 *
 * 호출 체인:
 *   stm32_pcie_probe() → [이 함수]
 *     → phy_set_mode() → phy_init() → regmap_update_bits(RC 모드)
 *     → stm32_pcie_deassert_perst() → dev_pm_set_dedicated_wake_irq()
 */
static int stm32_add_pcie_port(struct stm32_pcie *stm32_pcie)
{
	struct device *dev = stm32_pcie->pci.dev;
	unsigned int wake_irq;
	/* [한국어] 각 단계의 결과. */
	int ret;

	ret = phy_set_mode(stm32_pcie->phy, PHY_MODE_PCIE);
	/* [한국어] PHY 모드 설정이 실패하면, */
	if (ret)
		/* [한국어] 그대로 물러난다. 아직 되돌릴 것이 없다. */
		return ret;

	ret = phy_init(stm32_pcie->phy);
	/* [한국어] PHY 초기화가 실패하면, */
	if (ret)
		/* [한국어] 역시 그대로 물러난다. */
		return ret;

	ret = regmap_update_bits(stm32_pcie->regmap, SYSCFG_PCIECR,
				 /* [한국어] 타입 필드만 갈아 끼운다 — 같은 하드웨어가 EP 로도 동작할 수 있어 명시해야 한다. */
				 STM32MP25_PCIECR_TYPE_MASK,
				 STM32MP25_PCIECR_RC);
	if (ret)
		/* [한국어] 모드 설정이 실패하면 PHY 를 내리는 자리로 뛴다. */
		goto err_phy_exit;

	stm32_pcie_deassert_perst(stm32_pcie);

	if (stm32_pcie->wake_gpio) {
		/* [한국어] GPIO 에 대응하는 인터럽트 번호를 얻는다. */
		wake_irq = gpiod_to_irq(stm32_pcie->wake_gpio);
		/* [한국어] 전용 wake IRQ 로 등록하면 PM 코어가 절전 중에 그것을 켜고 깨어나면
		 * 끄는 일을 대신해 준다. */
		ret = dev_pm_set_dedicated_wake_irq(dev, wake_irq);
		/* [한국어] 등록이 실패하면, */
		if (ret) {
			/* [한국어] 그 사실을 남기고, */
			dev_err(dev, "Failed to enable wakeup irq %d\n", ret);
			/* [한국어] PERST# 를 다시 걸고 PHY 를 내리는 자리로 뛴다. */
			goto err_assert_perst;
		}
		irq_set_irq_type(wake_irq, IRQ_TYPE_EDGE_FALLING);
	/* [한국어] WAKE# 처리 끝. GPIO 가 없는 보드에서는 이 블록 전체를 건너뛴다. */
	}

	return 0;

err_assert_perst:
	stm32_pcie_assert_perst(stm32_pcie);

err_phy_exit:
	phy_exit(stm32_pcie->phy);

	return ret;
}

/* [한국어]
 * stm32_remove_pcie_port - wake IRQ 를 풀고 PERST# 를 걸고 PHY 를 내린다
 *
 * @stm32_pcie: 드라이버 상태.
 *
 * stm32_add_pcie_port() 의 짝이며 역순이다.
 *
 * wake IRQ 를 조건 없이 푸는 것이 눈에 띈다. 등록할 때는 GPIO 유무를
 * 확인했는데, dev_pm_clear_wake_irq() 는 등록되지 않은 경우에도 안전하다는
 * 전제로 검사를 생략한 것으로 보인다.
 *
 * RC 모드 설정은 되돌리지 않는다. syscon 의 그 필드를 남겨 두어도 컨트롤러가
 * 리셋되면 무의미해지고, 다음 probe 가 어차피 다시 쓰기 때문이다.
 *
 * 실행 컨텍스트: remove, 또는 probe 의 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   stm32_pcie_remove() / stm32_pcie_probe() 의 되감기 → [이 함수]
 *     → dev_pm_clear_wake_irq() → stm32_pcie_assert_perst() → phy_exit()
 */
static void stm32_remove_pcie_port(struct stm32_pcie *stm32_pcie)
{
	dev_pm_clear_wake_irq(stm32_pcie->pci.dev);

	stm32_pcie_assert_perst(stm32_pcie);

	phy_exit(stm32_pcie->phy);
}

/* [한국어]
 * stm32_pcie_parse_port - 루트 포트 자식 노드에서 PHY 와 두 GPIO 를 얻는다
 *
 * @stm32_pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 자원을 **자식 노드에서** 얻는 것이 이 함수의 특징이다. PCIe 루트 포트가
 * 디바이스 트리에서 별도 노드로 서술되고, 링크에 관한 자원(PHY, PERST#,
 * WAKE#)이 그 아래 매달려 있기 때문이다. 컨트롤러 자신의 자원(클럭, 리셋,
 * syscon)은 probe 가 부모 노드에서 직접 얻는다.
 *
 * 두 GPIO 의 -ENOENT 를 오류로 다루지 않는 것이 요점이다. 둘 다 선택 사항이라
 * 없는 보드가 있고, 그때는 포인터를 NULL 로 두어 이후 코드가 그 유무로
 * 동작을 가른다. -ENOENT 가 아닌 오류만 실패로 올려보낸다.
 *
 * PERST# 를 GPIOD_OUT_HIGH 로 얻는 것이 중요하다 — 얻는 순간 리셋이 걸린
 * 상태가 되어, 이후 stm32_add_pcie_port() 가 명시적으로 풀 때까지 하위
 * 장치가 깨어나지 않는다. WAKE# 는 GPIOD_IN 으로, 이쪽은 받는 신호다.
 *
 * 노드 참조를 모든 경로에서 놓는다. 오류 경로 셋과 정상 경로 하나가 각각
 * of_node_put 을 부르는데, of_get_next_available_child() 가 참조를 올려
 * 주기 때문이다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: PHY 를 못 얻으면 실패, GPIO 는 -ENOENT 가 아닌 오류만 실패다.
 *
 * 호출 체인:
 *   stm32_pcie_probe() → [이 함수]
 *     → of_get_next_available_child() → devm_of_phy_get()
 *     → devm_fwnode_gpiod_get() → of_node_put()
 */
static int stm32_pcie_parse_port(struct stm32_pcie *stm32_pcie)
{
	struct device *dev = stm32_pcie->pci.dev;
	struct device_node *root_port;

	root_port = of_get_next_available_child(dev->of_node, NULL);
/* [한국어] 루트 포트를 서술하는 자식 노드를 얻는다 — 참조가 올라가므로 모든 경로에서 놓아야 한다. */

	stm32_pcie->phy = devm_of_phy_get(dev, root_port, NULL);
	/* [한국어] PHY 를 얻지 못하면, */
	if (IS_ERR(stm32_pcie->phy)) {
		/* [한국어] 노드 참조를 놓고, */
		of_node_put(root_port);
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->phy),
				     /* [한국어] 그 오류를 올려보낸다. PHY 는 선택 사항이 아니라 필수다. */
				     "Failed to get pcie-phy\n");
	}

	stm32_pcie->perst_gpio = devm_fwnode_gpiod_get(dev, of_fwnode_handle(root_port),
						       /* [한국어] **출력** 으로, 초기값을 HIGH 로 얻는다 — 얻는 순간 리셋이 걸린 상태가 되어
						        * 이후 명시적으로 풀 때까지 하위 장치가 깨어나지 않는다. */
						       "reset", GPIOD_OUT_HIGH, NULL);
	if (IS_ERR(stm32_pcie->perst_gpio)) {
		/* [한국어] -ENOENT 가 아닌 오류라면 — 즉 속성은 있는데 잘못된 경우라면, */
		if (PTR_ERR(stm32_pcie->perst_gpio) != -ENOENT) {
			/* [한국어] 노드 참조를 놓고, */
			of_node_put(root_port);
			return dev_err_probe(dev, PTR_ERR(stm32_pcie->perst_gpio),
					     /* [한국어] 그 오류를 올려보낸다. */
					     "Failed to get reset GPIO\n");
		}
		stm32_pcie->perst_gpio = NULL;
	/* [한국어] -ENOENT 였다면 아래에서 NULL 로 두어, 이후 코드가 그 유무로 동작을 가른다. */
	}

	stm32_pcie->wake_gpio = devm_fwnode_gpiod_get(dev, of_fwnode_handle(root_port),
						      /* [한국어] 이쪽은 **입력** 이다 — 하위 장치가 보내는 신호를 받는다. */
						      "wake", GPIOD_IN, NULL);

	if (IS_ERR(stm32_pcie->wake_gpio)) {
		/* [한국어] PERST# 와 같은 규약이다 — -ENOENT 가 아닌 오류만 실패로 다룬다. */
		if (PTR_ERR(stm32_pcie->wake_gpio) != -ENOENT) {
			/* [한국어] 노드 참조를 놓고, */
			of_node_put(root_port);
			return dev_err_probe(dev, PTR_ERR(stm32_pcie->wake_gpio),
					     /* [한국어] 그 오류를 올려보낸다. */
					     "Failed to get wake GPIO\n");
		}
		stm32_pcie->wake_gpio = NULL;
	/* [한국어] -ENOENT 였다면 아래에서 NULL 로 둔다. */
	}

	of_node_put(root_port);

	return 0;
}

/* [한국어]
 * stm32_pcie_probe - 자원을 얻고 하드웨어를 준비한 뒤 DWC 호스트를 초기화한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며, 순서가 길다.
 *
 * 콜백 표를 셋 다 채우는 것이 첫 부분이다 — dev, dw_pcie_ops, 그리고
 * **비어 있는** host_ops. 빈 표를 굳이 다는 이유는 DWC 코어가 그 포인터를
 * NULL 로 두는 것과 빈 표를 구분하기 때문으로 보인다.
 *
 * 자원을 두 곳에서 얻는다. syscon·클럭·리셋은 이 노드에서, PHY 와 두 GPIO 는
 * stm32_pcie_parse_port() 가 자식 노드에서 얻는다.
 *
 * 리셋을 stm32_add_pcie_port() **뒤** 에 돌리는 순서가 눈에 띈다. PHY 초기화와
 * 모드 설정이 끝난 뒤 컨트롤러를 리셋해, 그 설정이 반영된 상태로 시작하게
 * 한다.
 *
 * 런타임 PM 설정이 셋이다 — set_active 로 현재 상태를 알리고,
 * no_callbacks 로 런타임 콜백이 없음을 표시하고, devm 판으로 활성화한다.
 * no_callbacks 는 이 드라이버가 런타임 절전에 관여하지 않고 시스템 절전만
 * 다룬다는 뜻이다.
 *
 * [상류 코드 관찰] 이 probe 에는 런타임 PM 참조를 올리는 호출이 없다.
 * set_active 와 devm_pm_runtime_enable 은 참조를 올리지 않는데, remove 는
 * pm_runtime_put_noidle() 로 참조를 내린다. 이 파일 안에서는 그 짝이 맞지
 * 않으며, 두 헬퍼의 내부 동작은 이 트리에 drivers/base 가 없어 확인 못 함.
 * 코드는 고치지 않았다.
 *
 * 되감기가 계단이며, 클럭을 켠 뒤의 실패는 클럭까지, 그 전의 실패는 포트만
 * 되돌린다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 링크 대기와
 * 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 각 단계의 실패를 goto 로 앞 단계까지 되감고 오류를 올려보낸다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → syscon_regmap_lookup_by_compatible() → devm_clk_get()
 *     → devm_reset_control_get_exclusive() → stm32_pcie_parse_port()
 *     → stm32_add_pcie_port() → clk_prepare_enable()
 *     → dw_pcie_host_init()
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
	/* [한국어] 호스트 콜백 표도 건다. **비어 있는 표** 인데도 다는 것은,
	 * DWC 코어가 NULL 포인터와 빈 표를 구분하기 때문으로 보인다. */
	stm32_pcie->pci.pp.ops = &stm32_pcie_host_ops;

	stm32_pcie->regmap = syscon_regmap_lookup_by_compatible("st,stm32mp25-syscfg");
	/* [한국어] syscon 을 찾지 못하면, */
	if (IS_ERR(stm32_pcie->regmap))
		/* [한국어] 오류 코드를 꺼내 */
		return dev_err_probe(dev, PTR_ERR(stm32_pcie->regmap),
				     /* [한국어] 올려보낸다. */
				     "No syscfg specified\n");

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

	ret = stm32_pcie_parse_port(stm32_pcie);
	/* [한국어] 자식 노드의 자원 확보가 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;

	platform_set_drvdata(pdev, stm32_pcie);
/* [한국어] 이 파일의 콜백들이 drvdata 로 상태를 되찾으므로,
 * 콜백이 처음 불릴 수 있는 아래 호출보다 먼저 매달아야 한다. */

	ret = stm32_add_pcie_port(stm32_pcie);
	/* [한국어] PHY·모드·PERST#·wake IRQ 준비가 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. 그 함수가 자기 되감기를 끝낸 뒤다. */
		return ret;

	reset_control_assert(stm32_pcie->rst);
	reset_control_deassert(stm32_pcie->rst);

	ret = clk_prepare_enable(stm32_pcie->clk);
	/* [한국어] 코어 클럭 켜기가 실패하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Core clock enable failed %d\n", ret);
		/* [한국어] 포트만 되돌리는 자리로 뛴다. */
		goto err_remove_port;
	}

	ret = pm_runtime_set_active(dev);
	/* [한국어] 런타임 PM 상태 설정이 실패하면, */
	if (ret < 0) {
		/* [한국어] 그 사실을 남기고, */
		dev_err_probe(dev, ret, "Failed to activate runtime PM\n");
		/* [한국어] 클럭부터 되돌리는 자리로 뛴다. */
		goto err_disable_clk;
	}

	pm_runtime_no_callbacks(dev);

	ret = devm_pm_runtime_enable(dev);
	/* [한국어] 런타임 PM 활성화가 실패하면, */
	if (ret < 0) {
		/* [한국어] 그 사실을 남기고, */
		dev_err_probe(dev, ret, "Failed to enable runtime PM\n");
		/* [한국어] 클럭부터 되돌린다. */
		goto err_disable_clk;
	}

	ret = dw_pcie_host_init(&stm32_pcie->pci.pp);
	/* [한국어] DWC 호스트 초기화가 실패하면, */
	if (ret)
		/* [한국어] 클럭부터 되돌린다. */
		goto err_disable_clk;

	if (stm32_pcie->wake_gpio)
		/* [한국어] WAKE# 가 있는 보드에서만 깨우기를 켠다. 이것이 켜져야
		 * 하위 장치의 WAKE# 가 시스템을 깨울 수 있다. */
		device_init_wakeup(dev, true);

	return 0;

err_disable_clk:
	clk_disable_unprepare(stm32_pcie->clk);

err_remove_port:
	stm32_remove_pcie_port(stm32_pcie);

	return ret;
}

/* [한국어]
 * stm32_pcie_remove - 호스트를 내리고 자원을 놓는다
 *
 * @pdev: 플랫폼 장치.
 *
 * probe 의 역순이다.
 *
 * 깨우기 설정을 가장 먼저 끈다. 정리 도중에 WAKE# 가 들어와 시스템을
 * 깨우려 드는 것을 막기 위해서다.
 *
 * dw_pcie_host_deinit() 이 버스를 내리고 그 아래 장치들의 드라이버를
 * 제거한다. 그것이 끝난 뒤에야 클럭을 끌 수 있다 — 제거 도중에 config
 * 접근이 일어나기 때문이다.
 *
 * 마지막의 pm_runtime_put_noidle() 이 probe 의 어느 호출과 짝인지는
 * 위 stm32_pcie_probe() 의 관찰에 적었다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → device_init_wakeup(false) → dw_pcie_host_deinit()
 *     → clk_disable_unprepare() → stm32_remove_pcie_port()
 *     → pm_runtime_put_noidle()
 */
static void stm32_pcie_remove(struct platform_device *pdev)
{
	struct stm32_pcie *stm32_pcie = platform_get_drvdata(pdev);
	struct dw_pcie_rp *pp = &stm32_pcie->pci.pp;

	if (stm32_pcie->wake_gpio)
		/* [한국어] 깨우기 설정을 **가장 먼저** 끈다. 정리 도중에 WAKE# 가 들어와
		 * 시스템을 깨우려 드는 것을 막기 위해서다. */
		device_init_wakeup(&pdev->dev, false);

	dw_pcie_host_deinit(pp);

	clk_disable_unprepare(stm32_pcie->clk);

	stm32_remove_pcie_port(stm32_pcie);

	pm_runtime_put_noidle(&pdev->dev);
}

static const struct of_device_id stm32_pcie_of_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다 — 엔드포인트 판과 다른
	 * compatible 이라, 같은 하드웨어를 어느 모드로 쓸지는 트리가 정한다. */
	{ .compatible = "st,stm32mp25-pcie-rc" },
	/* [한국어] 표의 끝 표시. */
	{},
};

static struct platform_driver stm32_pcie_driver = {
	/* [한국어] 이 드라이버는 remove 를 지원한다. */
	.probe = stm32_pcie_probe,
	/* [한국어] 그 remove 콜백. */
	.remove = stm32_pcie_remove,
	.driver = {
		.name = "stm32-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = stm32_pcie_of_match,
		.pm = &stm32_pcie_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_platform_driver(stm32_pcie_driver);

MODULE_AUTHOR("Christian Bruel <christian.bruel@foss.st.com>");
MODULE_DESCRIPTION("STM32MP25 PCIe Controller driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, stm32_pcie_of_match);
