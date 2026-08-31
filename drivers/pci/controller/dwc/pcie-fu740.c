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

/*
 * [한국어 설명] SiFive FU740 RISC-V SoC 의 PCIe (pcie-fu740.c)
 *
 * === 파일의 역할 ===
 * SiFive FU740 은 HiFive Unmatched 보드에 들어간 RISC-V SoC 다. 리눅스가
 * 도는 RISC-V 보드 중 M.2 슬롯에 NVMe 를 꽂을 수 있는 초기 사례여서,
 * RISC-V 에서 NVMe 를 다뤄 본 사람은 대개 이 드라이버를 거쳤다.
 *
 * DesignWare IP 를 쓰므로 이 파일이 하는 일은 주변부뿐이다. 링크를 올리고
 * config 를 다루는 핵심은 pcie-designware.c 와 -host.c 가 맡는다.
 * 여기서 하는 것은 그 전 단계다.
 *   1) 전원을 넣는다. M.2 슬롯의 3.3V 를 GPIO 로 켠다.
 *   2) 클럭을 켠다.
 *   3) PHY 를 초기화한다. FU740 은 PCIe PHY 가 여러 개라 각각 다룬다.
 *   4) PERST# 를 뗀다. 이것이 장치에게 "이제 시작해도 좋다" 는 신호다.
 *
 * 순서와 타이밍이 중요하다. PCIe 규격은 전원이 안정된 뒤 최소 100ms 를
 * 기다렸다가 PERST# 를 떼라고 정하고 있다. 너무 빨리 떼면 장치가 아직
 * 준비되지 않아 링크가 올라오지 않거나 불안정해진다. 이 드라이버가
 * delay 를 여기저기 넣는 이유다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 sifive,fu740-pcie 가 있으면
 *   -> [이 파일] fu740_pcie_probe()
 *      -> 전원/클럭/PHY/리셋 준비
 *      -> dw_pcie_host_init() [pcie-designware-host.c]
 *         -> 링크 대기 -> PCI 코어 열거 -> nvme_probe()
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트. 여러 곳에서 잠들며 기다린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: pcie-designware.c / -host.c, PHY 서브시스템, 클럭·리셋
 *   프레임워크, GPIO(gpiod) 프레임워크.
 * 공유 상태: struct fu740_pcie 가 struct dw_pcie 를 품는다.
 *
 * === 주요 함수/구조체 요약 ===
 * fu740_pcie_probe()      : 전체 초기화. 이 파일의 입구.
 * fu740_pcie_host_init()  : DesignWare 호스트 초기화 콜백. 전원과 리셋
 *                           순서를 여기서 지킨다.
 * fu740_pcie_start_link() : 링크 트레이닝을 시작한다. 그냥 시작하는 것이
 *                           아니라 LNKCAP 의 지원 속도를 2.5GT/s 로 낮춰
 *                           먼저 붙인 뒤 원래 속도로 재협상시킨다 --
 *                           고속에서 바로 학습하면 링크가 서지 않는
 *                           하드웨어를 위한 우회다.
 * fu740_pcie_init_phy()   : 이 SoC 의 PCIe PHY 를 초기화한다.
 *                           fu740_phyregwrite() 로 PHY 레지스터를 직접 쓴다.
 * fu740_pcie_power_on()   : pwren GPIO 로 슬롯 전원을 켜고 100ms 기다린다.
 *                           pcie_aux 는 GPIO 가 아니라 클록이며 이 함수는
 *                           건드리지 않는다 -- 그것은 host_init 의 몫이다.
 * fu740_pcie_drive_reset() : PERST# 를 제어한다. 전원 안정화 후 규격이
 *                           요구하는 시간을 기다렸다가 뗀다.
 * fu740_pcie_assert_reset() / fu740_pcie_deassert_reset() : PERST# 를
 *                           **두 곳에서 함께** 제어한다 -- 보드의 reset
 *                           GPIO(장치로 나가는 신호)와 관리 창의
 *                           PCIEX8MGMT_PERST_N(컨트롤러 내부 상태).
 *                           별개인 것은 afp->rst 로 다루는 컨트롤러
 *                           파워업 리셋 쪽이고, 그것은 host_init 이 푼다.
 *                           두 함수의 순서가 서로 반대인 점도 의도된 것이다.
 * fu740_pcie_shutdown()   : 종료 시 링크를 내린다.
 * fu740_pcie_host_ops     : DesignWare 호스트 콜백 표.
 * fu740_pcie_of_match / fu740_pcie_driver : 바인딩 정의.
 * struct fu740_pcie       : dw_pcie 와 이 보드의 GPIO·클럭·리셋 핸들.
 *                           PHY 는 별도 핸들이 없다 -- PHY 프레임워크를
 *                           쓰지 않고 관리 창의 제어 버스로 직접 다룬다.
 *
 * === NVMe 관점 (필수 4섹션에 대한 부가 절) ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 호출하지 않는다(이 트리에서 전수 확인).
 * 이 드라이버가 세우는 것은 버스이고, NVMe 는 그 위에서 열거되는 장치 중
 * 하나일 뿐이다.
 *
 * 다만 이 파일의 두 타이밍이 슬롯에 꽂힌 **모든** 장치의 열거 성공 여부를
 * 좌우한다. 코드에서 확인되는 값은 이렇다:
 *   - fu740_pcie_power_on() 의 msleep(100) -- 전원 인가 후 PERST# 를 놓기
 *     전까지의 대기.
 *   - fu740_pcie_drive_reset() 의 순서 -- 리셋을 먼저 걸고 전원을 켠 뒤
 *     리셋을 푼다.
 * 이 값들이 어떤 장치에 충분한지는 장치마다 다르고, 그 판단 근거는 이
 * 트리 안에 없다(상류에도 근거 주석이 없다). 그래서 여기서는 코드가 정하는
 * 타이밍이 무엇인지만 적고, 특정 장치에서의 증상은 단정하지 않는다.
 */

/* [한국어] clk_prepare_enable / clk_disable_unprepare. pcie_aux 보조 클록을 다룬다. */
#include <linux/clk.h>
/* [한국어] msleep / ndelay. 전원 안정화 100ms 와 PHY 선택 신호 전달 10ns 에 쓴다. */
#include <linux/delay.h>
/* [한국어] 구형 GPIO 정수 API 헤더. 이 파일은 서술자 API 만 쓰므로 실제로 참조하는
 * 심볼은 없다 -- 아래 gpio/consumer.h 가 필요한 쪽이다. */
#include <linux/gpio.h>
/* [한국어] gpiod_set_value_cansleep 과 devm_gpiod_get_optional. PERST# 와 슬롯 전원을
 * GPIO 서술자로 제어한다. */
#include <linux/gpio/consumer.h>
/* [한국어] 커널 기본 매크로. 이 파일은 특별히 참조하는 심볼이 없다. */
#include <linux/kernel.h>
/* [한국어] MODULE_* 매크로용. 다만 이 드라이버는 builtin_platform_driver 라
 * 모듈로 빌드되지 않는다. */
#include <linux/module.h>
/* [한국어] PCI_CAP_ID_EXP, PCI_EXP_LNKCAP, PCI_EXP_LNKCAP_SLS 같은 규약 상수를 위해.
 * start_link 의 속도 조작이 이것들에 의존한다. */
#include <linux/pci.h>
/* [한국어] platform_driver 등록과 devm_platform_ioremap_resource_byname 을 위해. */
#include <linux/platform_device.h>
/* [한국어] struct resource 정의. 자원 조회 경로에서 간접적으로 쓰인다. */
#include <linux/resource.h>
/* [한국어] uint8_t/uint16_t 등 고정폭 타입. fu740_phyregwrite 의 인자 타입이 이것들이다. */
#include <linux/types.h>
/* [한국어] 인터럽트 API 헤더. 이 파일에는 자체 인터럽트 핸들러가 없어 직접 참조하는
 * 심볼이 없다. */
#include <linux/interrupt.h>
/* [한국어] readl_poll_timeout. PHY 제어 버스의 ACK 악수를 기다리는 데 쓴다. */
#include <linux/iopoll.h>
/* [한국어] reset_control_deassert. 컨트롤러 파워업 리셋을 푼다. */
#include <linux/reset.h>

/* [한국어] DWC 코어의 공개 인터페이스. struct dw_pcie, dw_pcie_host_init,
 * dw_pcie_wait_for_link, DBI 접근자가 여기 있다. */
#include "pcie-designware.h"

/* [한국어] dw_pcie 포인터에서 이 드라이버 인스턴스를 되찾는 매크로.
 * container_of 가 아니라 dev_get_drvdata 인 점이 특징이다 -- probe 가
 * platform_set_drvdata 로 심어 둔 값을 꺼낸다. 그래서 probe 에서 그 호출이
 * dw_pcie_host_init 보다 먼저 있어야 한다.
 * (struct fu740_pcie 의 첫 필드가 pci 이므로 container_of 로도 가능했다.) */
#define to_fu740_pcie(x)	dev_get_drvdata((x)->dev)

struct fu740_pcie {
	struct dw_pcie pci;
	void __iomem *mgmt_base;
	/* [한국어] 슬롯의 PERST# 신호를 내보내는 GPIO.
	 * 설정자: fu740_pcie_probe 가 devm_gpiod_get_optional 로 얻는다.
	 * 읽는 자: assert_reset(값 0) / deassert_reset(값 1).
	 * 값 범위: **NULL 일 수 있다** -- PERST# 가 하드웨어로 처리되는 보드에서는
	 * 없다. gpiod_set_value_cansleep(NULL, ...) 은 아무 일도 하지 않으므로
	 * 그런 보드에서도 레지스터 쪽 PERST_N 제어만으로 동작한다.
	 * 동기화: 프로브·종료 경로에서만 다뤄져 경쟁이 없다. */
	struct gpio_desc *reset;
	/* [한국어] 슬롯 전원 스위치를 켜는 GPIO.
	 * 설정자: probe 가 optional 로 얻는다.
	 * 읽는 자: fu740_pcie_power_on 하나뿐.
	 * 값 범위: NULL 가능 -- 전원이 상시 인가된 보드다. 그 경우에도 power_on 의
	 * 100ms 대기는 그대로 일어난다.
	 * 동기화: 프로브 경로 전용. */
	struct gpio_desc *pwren;
	/* [한국어] PHY 설정에 필요한 보조 클록.
	 * 설정자: probe 가 이름 'pcie_aux' 로 얻는다(필수).
	 * 읽는 자: host_init 이 켜고, PHY 초기화 뒤 껐다 다시 켠다.
	 * 값 범위: 유효한 clk 핸들. devm 이라 해제는 자동이다.
	 * 동기화: host_init 안에서만 다뤄지며, 그 함수는 프로브 경로에서 한 번 불린다. */
	struct clk *pcie_aux;
	/* [한국어] 컨트롤러 파워업 리셋 라인.
	 * 설정자: probe 가 이름 없이(NULL) exclusive 로 얻는다.
	 * 읽는 자: host_init 의 reset_control_deassert 하나뿐 -- **다시 assert 하는
	 * 코드가 이 파일에 없다.**
	 * 값 범위: 유효한 핸들.
	 * 동기화: 프로브 경로 전용. */
	struct reset_control *rst;
/* [한국어] 이 구조체에는 잠금이 없다. 모든 필드가 프로브·종료 경로에서만 다뤄지고
 * 그 경로들이 서로 겹치지 않기 때문이다. */
};

/* [한국어] SiFive 프리즘 컨트롤러의 장치 리셋 레지스터 오프셋.
 * **이 파일 안에서 참조하는 곳이 없다** -- 리셋은 reset 프레임워크
 * (afp->rst)로 다루기 때문이다. */
#define SIFIVE_DEVICESRESETREG		0x28

/* [한국어] 관리 창의 PERST_N 레지스터. 컨트롤러 내부의 리셋 상태를 제어한다.
 * 보드 GPIO 와 짝을 이뤄 두 곳에서 PERST# 를 건다. */
#define PCIEX8MGMT_PERST_N		0x0
/* [한국어] LTSSM 활성 레지스터. 1 을 써야 링크 학습이 시작된다. */
#define PCIEX8MGMT_APP_LTSSM_ENABLE	0x10
/* [한국어] PHY 리셋 유지 레지스터. 1 이면 PHY 를 리셋 상태로 붙들어, 그 사이에
 * PHY 레지스터를 설정할 수 있다. */
#define PCIEX8MGMT_APP_HOLD_PHY_RST	0x18
/* [한국어] 장치 종류 레지스터. host_init 이 4 를 써 루트 컴플렉스 모드로 지정한다. */
#define PCIEX8MGMT_DEVICE_TYPE		0x708
/* [한국어] PHY0 제어 버스의 주소 레지스터. 아래 여섯 개가 한 벌로 간접 접근
 * 프로토콜을 이룬다 -- PHY 내부 레지스터는 메모리에 직접 매핑되어 있지 않다. */
#define PCIEX8MGMT_PHY0_CR_PARA_ADDR	0x860
/* [한국어] PHY0 읽기 활성. **이 파일은 PHY 를 쓰기만 하고 읽지 않아 참조하는 곳이 없다.** */
#define PCIEX8MGMT_PHY0_CR_PARA_RD_EN	0x870
/* [한국어] PHY0 읽기 데이터. 같은 이유로 참조하는 곳이 없다. */
#define PCIEX8MGMT_PHY0_CR_PARA_RD_DATA	0x878
/* [한국어] PHY0 제어 버스 선택. init_phy 가 1 을 써 버스를 활성화한다. */
#define PCIEX8MGMT_PHY0_CR_PARA_SEL	0x880
/* [한국어] PHY0 쓰기 데이터. */
#define PCIEX8MGMT_PHY0_CR_PARA_WR_DATA	0x888
/* [한국어] PHY0 쓰기 활성. 이것을 세우면 전송이 시작된다. */
#define PCIEX8MGMT_PHY0_CR_PARA_WR_EN	0x890
/* [한국어] PHY0 확인(ACK). 4단계 악수의 응답 쪽이다. */
#define PCIEX8MGMT_PHY0_CR_PARA_ACK	0x898
/* [한국어] PHY1 의 같은 여섯 레지스터. **오프셋 간격이 PHY0 과 규칙적이지 않아**
 * (PHY0 0x860~0x898, PHY1 0x8a0~0x8d8) 산술 대신 상수를 따로 나열한다. */
#define PCIEX8MGMT_PHY1_CR_PARA_ADDR	0x8a0
/* [한국어] PHY1 읽기 활성. 참조하는 곳이 없다. */
#define PCIEX8MGMT_PHY1_CR_PARA_RD_EN	0x8b0
/* [한국어] PHY1 읽기 데이터. 참조하는 곳이 없다. */
#define PCIEX8MGMT_PHY1_CR_PARA_RD_DATA	0x8b8
/* [한국어] PHY1 제어 버스 선택. */
#define PCIEX8MGMT_PHY1_CR_PARA_SEL	0x8c0
/* [한국어] PHY1 쓰기 데이터. */
#define PCIEX8MGMT_PHY1_CR_PARA_WR_DATA	0x8c8
/* [한국어] PHY1 쓰기 활성. */
#define PCIEX8MGMT_PHY1_CR_PARA_WR_EN	0x8d0
/* [한국어] PHY1 확인(ACK). */
#define PCIEX8MGMT_PHY1_CR_PARA_ACK	0x8d8

/* [한국어] CDR(Clock and Data Recovery) 추적 활성. 수신 신호에서 클록을 복원하는
 * 회로를 켠다. */
#define PCIEX8MGMT_PHY_CDR_TRACK_EN	BIT(0)
/* [한국어] LOS(Loss of Signal) 임계값 설정. 신호 없음을 판정하는 기준이다. */
#define PCIEX8MGMT_PHY_LOS_THRSHLD	BIT(5)
/* [한국어] 수신 종단 저항 활성. */
#define PCIEX8MGMT_PHY_TERM_EN		BIT(9)
/* [한국어] 그 종단을 AC 결합으로 쓸지 DC 로 쓸지 고르는 비트. */
#define PCIEX8MGMT_PHY_TERM_ACDC	BIT(10)
/* [한국어] PHY 자체 활성 비트. */
#define PCIEX8MGMT_PHY_EN		BIT(11)
/* [한국어] 위 다섯 비트를 모두 켠 조합. 8개 레인 전부에 같은 값을 써, 수신단이
 * 신호를 제대로 잡도록 강제로 덮어쓴다(override). */
#define PCIEX8MGMT_PHY_INIT_VAL		(PCIEX8MGMT_PHY_CDR_TRACK_EN | \
					 PCIEX8MGMT_PHY_LOS_THRSHLD | \
					 PCIEX8MGMT_PHY_TERM_EN | \
					 PCIEX8MGMT_PHY_TERM_ACDC | \
					 PCIEX8MGMT_PHY_EN)

/* [한국어] 레인별 수신 덮어쓰기 레지스터의 기준 주소. 이름의 OVRD_IN 이 '입력을
 * 덮어쓴다' 는 뜻으로, 자동 조정 대신 소프트웨어 값을 강제한다는 의미다. */
#define PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3	0x1008
/* [한국어] 레인 간 간격(0x100). **이 상수를 참조하는 곳이 없다** -- 아래 LANE0~3
 * 상수가 0x100 리터럴을 직접 쓰기 때문이다. */
#define PCIEX8MGMT_PHY_LANE_OFF		0x100
/* [한국어] 레인 0 (기준 주소 그대로). */
#define PCIEX8MGMT_PHY_LANE0_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 0)
/* [한국어] 레인 1. */
#define PCIEX8MGMT_PHY_LANE1_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 1)
/* [한국어] 레인 2. */
#define PCIEX8MGMT_PHY_LANE2_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 2)
/* [한국어] 레인 3. 네 상수가 모두 같은 산술이라 루프로도 쓸 수 있었을 자리다. */
#define PCIEX8MGMT_PHY_LANE3_BASE	(PCIEX8MGMT_PHY_LANEN_DIG_ASIC_RX_OVRD_IN_3 + 0x100 * 3)

/* [한국어]
 * fu740_pcie_assert_reset - 보드 GPIO 와 컨트롤러 레지스터 양쪽으로 PERST# 를 건다
 *
 * @afp: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * PERST# 를 두 곳에서 제어하는 것이 이 하드웨어의 특징이다:
 *  - 보드의 리셋 GPIO -- 슬롯에 꽂힌 장치로 나가는 물리 신호.
 *  - PCIEX8MGMT_PERST_N 레지스터 -- 컨트롤러 내부의 리셋 상태.
 * 둘을 함께 걸어야 장치와 컨트롤러가 같은 리셋 상태에 놓인다.
 *
 * GPIO 값 0 이 assert 인 점에 유의. deassert 가 1 을 쓰므로 이 파일 안에서는
 * 일관되지만, 같은 트리의 pcie-dw-rockchip.c 는 정반대 규약(1 이 assert)을
 * 쓴다 -- GPIO 서술자의 논리값은 드라이버가 정하기 나름이라 파일마다 확인해야
 * 한다. 실제 전기 레벨은 DT 의 active-low 표기가 다시 뒤집을 수 있다.
 *
 * writel_relaxed 를 쓰는 이유: 이 레지스터 접근은 DMA 버퍼 가시성과 순서를
 * 맞출 필요가 없고, 관리 레지스터끼리의 순서는 같은 버스가 보장한다.
 *
 * 실행 컨텍스트: 프로브·종료 경로의 프로세스 문맥. cansleep 판을 쓰므로
 * 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   fu740_pcie_drive_reset / fu740_pcie_shutdown → [이 함수]
 *     → gpiod_set_value_cansleep → writel_relaxed
 */
static void fu740_pcie_assert_reset(struct fu740_pcie *afp)
{
	/* Assert PERST_N GPIO */
	gpiod_set_value_cansleep(afp->reset, 0);
	/* Assert controller PERST_N */
	writel_relaxed(0x0, afp->mgmt_base + PCIEX8MGMT_PERST_N);
}

/* [한국어]
 * fu740_pcie_deassert_reset - 컨트롤러 리셋을 먼저 풀고 그 다음 장치 PERST# 를 놓는다
 *
 * @afp: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * assert 와 **순서가 반대**인 점이 요점이다. assert 는 GPIO 를 먼저 걸고
 * 레지스터를 걸었지만, deassert 는 레지스터를 먼저 풀고 GPIO 를 나중에 푼다.
 * 컨트롤러가 먼저 깨어나 있어야 장치가 리셋에서 벗어났을 때 곧바로 링크
 * 학습을 받을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로브 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   fu740_pcie_drive_reset → [이 함수] → writel_relaxed
 *     → gpiod_set_value_cansleep
 */
static void fu740_pcie_deassert_reset(struct fu740_pcie *afp)
{
	/* Deassert controller PERST_N */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_PERST_N);
	/* Deassert PERST_N GPIO */
	gpiod_set_value_cansleep(afp->reset, 1);
}

/* [한국어]
 * fu740_pcie_power_on - 슬롯 전원을 켜고 안정될 때까지 기다린다
 *
 * @afp: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * pwren GPIO 로 슬롯 전원 스위치를 켠다. 그 뒤 msleep(100)이 하는 일이
 * 이 함수의 실체다 -- 상류 주석대로 전원 레일이 규격 범위 안으로 올라오는
 * 데 시간이 걸리고, 그 전에 PERST# 를 놓으면 장치가 불완전한 전원에서
 * 리셋을 벗어나 오동작한다.
 *
 * pwren 은 optional 이므로 NULL 일 수 있다. gpiod_set_value_cansleep(NULL, ...)
 * 은 아무 일도 하지 않으므로, 전원이 상시 인가된 보드에서도 이 코드가
 * 그대로 동작한다 -- 다만 100ms 대기는 여전히 일어난다.
 *
 * 실행 컨텍스트: 프로브 경로의 프로세스 문맥. msleep 이 있어 반드시 잠들 수
 * 있어야 한다.
 *
 * 호출 체인:
 *   fu740_pcie_drive_reset → [이 함수] → gpiod_set_value_cansleep → msleep
 */
static void fu740_pcie_power_on(struct fu740_pcie *afp)
{
	gpiod_set_value_cansleep(afp->pwren, 1);
	/*
	 * Ensure that PERST has been asserted for at least 100 ms.
	 * Section 2.2 of PCI Express Card Electromechanical Specification
	 * Revision 3.0
	 */
	msleep(100);
}

/* [한국어]
 * fu740_pcie_drive_reset - 리셋 → 전원 인가 → 리셋 해제의 한 벌을 수행한다
 *
 * @afp: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * 세 단계의 순서가 전부다. **리셋을 먼저 걸고 전원을 켜는** 것이 핵심 --
 * 전원이 올라오는 동안 장치가 리셋 상태에 있어야, 불안정한 전압에서
 * 내부 상태가 엉키지 않는다. 전원이 안정된 뒤(power_on 의 100ms) 리셋을
 * 푼다.
 *
 * 실행 컨텍스트: host_init 초입의 프로세스 문맥.
 *
 * 호출 체인:
 *   fu740_pcie_host_init → [이 함수] → assert_reset → power_on
 *     → deassert_reset
 */
static void fu740_pcie_drive_reset(struct fu740_pcie *afp)
{
	fu740_pcie_assert_reset(afp);
	fu740_pcie_power_on(afp);
	fu740_pcie_deassert_reset(afp);
}

/* [한국어]
 * fu740_phyregwrite - 제어 병렬 버스(CR para)를 통해 PHY 내부 레지스터에 쓴다
 *
 * @phy: 0 또는 1. 이 SoC 에는 PHY 가 두 개 있다.
 * @addr: PHY 내부 레지스터 주소.
 * @wrdata: 쓸 16비트 값.
 * @afp: 이 드라이버 인스턴스.
 * @return: 없음. 실패해도 경고만 남긴다.
 *
 * PHY 내부 레지스터는 메모리에 직접 매핑되어 있지 않고, 관리 레지스터
 * 몇 개로 이루어진 **간접 접근 프로토콜**로만 닿는다. 그 절차가 이 함수다:
 *  1. 주소 레지스터에 대상 주소를 쓴다.
 *  2. 데이터 레지스터에 값을 쓴다.
 *  3. WR_EN 에 1 을 써 전송을 시작한다.
 *  4. ACK 가 **설 때까지** 기다린다 -- PHY 가 받았다는 신호다.
 *  5. WR_EN 을 0 으로 내린다.
 *  6. ACK 가 **내려갈 때까지** 기다린다 -- 악수가 끝나 다음 전송을 받을
 *     준비가 됐다는 신호다.
 * 4번과 6번이 모두 필요한 이유가 이 4단계 악수다. 6번을 빼면 다음 호출이
 * 이전 전송의 잔여 ACK 를 자기 것으로 오인한다.
 *
 * 두 PHY 의 레지스터 묶음이 통째로 다른 주소에 있어, 앞머리에서 네 개의
 * 포인터를 한꺼번에 고른다. 오프셋 산술로 계산하지 않고 상수를 나열한 것은
 * PHY0(0x860~0x898)과 PHY1(0x8a0~0x8d8)의 간격이 규칙적이지 않기 때문이다.
 *
 * 두 대기 모두 10us 간격으로 5ms 까지 기다리고, 실패해도 dev_warn 만 남기고
 * 진행한다 -- 반환형이 void 라 상위에 알릴 통로가 없다.
 *
 * 실행 컨텍스트: 프로브 경로의 프로세스 문맥. readl_poll_timeout 이 잠들 수 있다.
 *
 * 호출 체인:
 *   fu740_pcie_init_phy → [이 함수] → writel_relaxed → readl_poll_timeout
 */
static void fu740_phyregwrite(const uint8_t phy, const uint16_t addr,
			      const uint16_t wrdata, struct fu740_pcie *afp)
{
	struct device *dev = afp->pci.dev;
	void __iomem *phy_cr_para_addr;
	/* [한국어] 쓸 값을 담을 데이터 레지스터의 주소. */
	void __iomem *phy_cr_para_wr_data;
	/* [한국어] 전송을 시작시킬 활성 레지스터의 주소. */
	void __iomem *phy_cr_para_wr_en;
	/* [한국어] PHY 의 응답을 읽을 확인 레지스터의 주소. */
	void __iomem *phy_cr_para_ack;
	/* [한국어] ret 은 대기 결과, val 은 readl_poll_timeout 이 읽은 ACK 값을 담는다. */
	int ret, val;

	/* Setup */
	if (phy) {
		phy_cr_para_addr = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_ADDR;
		/* [한국어] PHY1 의 데이터 레지스터. */
		phy_cr_para_wr_data = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_WR_DATA;
		/* [한국어] PHY1 의 활성 레지스터. */
		phy_cr_para_wr_en = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_WR_EN;
		/* [한국어] PHY1 의 확인 레지스터. */
		phy_cr_para_ack = afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_ACK;
	/* [한국어] phy 가 0 이면 PHY0 쪽 묶음을 고른다. */
	} else {
		phy_cr_para_addr = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_ADDR;
		/* [한국어] PHY0 의 데이터 레지스터. */
		phy_cr_para_wr_data = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_WR_DATA;
		/* [한국어] PHY0 의 활성 레지스터. */
		phy_cr_para_wr_en = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_WR_EN;
		/* [한국어] PHY0 의 확인 레지스터. 두 묶음의 오프셋 간격이 규칙적이지 않아
		 * 산술 대신 상수를 나열한다(PHY0 0x860~0x898, PHY1 0x8a0~0x8d8). */
		phy_cr_para_ack = afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_ACK;
	/* [한국어] 이제 네 포인터가 정해졌으므로 아래 절차는 PHY 번호와 무관하다. */
	}

	writel_relaxed(addr, phy_cr_para_addr);
	/* [한국어] 쓸 값을 데이터 레지스터에 올린다. 주소보다 나중에 쓰는 순서는 이 프로토콜의
	 * 요구가 아니라 관례다 -- WR_EN 을 세우기 전이면 순서는 자유롭다. */
	writel_relaxed(wrdata, phy_cr_para_wr_data);
	/* [한국어] WR_EN 을 세워 전송을 시작한다. 여기부터 4단계 악수가 시작된다. */
	writel_relaxed(1, phy_cr_para_wr_en);

	/* Wait for wait_idle */
	ret = readl_poll_timeout(phy_cr_para_ack, val, val, 10, 5000);
	if (ret)
		/* [한국어] ACK 가 5ms 안에 서지 않았다. PHY 가 응답하지 않는다는 뜻이지만,
		 * 반환형이 void 라 경고만 남기고 진행한다. */
		dev_warn(dev, "Wait for wait_idle state failed!\n");
/* [한국어] 악수의 후반부로 넘어간다. */

	/* Clear */
	writel_relaxed(0, phy_cr_para_wr_en);

	/* Wait for ~wait_idle */
	ret = readl_poll_timeout(phy_cr_para_ack, val, !val, 10, 5000);
	if (ret)
		/* [한국어] ACK 가 내려가지 않았다. 이 대기를 빠뜨리면 다음 호출이 이전 전송의
		 * 잔여 ACK 를 자기 것으로 오인한다. */
		dev_warn(dev, "Wait for !wait_idle state failed!\n");
}

/* [한국어]
 * fu740_pcie_init_phy - 두 PHY 의 네 레인씩, 총 8개 레인을 같은 값으로 초기화한다
 *
 * @afp: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * 이 컨트롤러는 x8 링크를 PHY 두 개(각 4레인)로 구성한다. 그래서 8번의
 * 레지스터 쓰기가 필요하다.
 *
 * 순서:
 *  1. 두 PHY 의 CR_PARA_SEL 에 1 을 써 **제어 병렬 버스를 활성화**한다.
 *     이것이 켜져야 fu740_phyregwrite 의 악수가 동작한다.
 *  2. ndelay(10) -- 상류 주석대로 선택 신호가 PHY 에 전달될 시간을 준다.
 *     나노초 단위라 바쁜 대기(ndelay)가 맞다.
 *  3. 각 레인의 DIG_ASIC_RX_OVRD_IN_3 레지스터에 PCIEX8MGMT_PHY_INIT_VAL 을
 *     쓴다. 그 값은 CDR 추적, LOS 임계, 종단 저항과 그 AC/DC 모드, PHY 활성
 *     다섯 비트를 켠 조합이다 -- 수신단이 신호를 제대로 잡도록 강제로
 *     덮어쓰는(override) 설정이다.
 *
 * 레인 주소는 LANEN_DIG_ASIC_RX_OVRD_IN_3(0x1008)에서 0x100 씩 떨어진다.
 * 코드가 LANE0~LANE3 상수를 나열하는데, 그 상수들이 같은 산술로 정의되어
 * 있어 루프로 쓸 수도 있었을 자리다.
 *
 * 실행 컨텍스트: host_init 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   fu740_pcie_host_init → [이 함수] → writel_relaxed → ndelay
 *     → fu740_phyregwrite (8회)
 */
static void fu740_pcie_init_phy(struct fu740_pcie *afp)
{
	/* Enable phy cr_para_sel interfaces */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_PHY0_CR_PARA_SEL);
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_PHY1_CR_PARA_SEL);
/* [한국어] 여기부터 8개 레인을 같은 값으로 설정한다. */

	/*
	 * Wait 10 cr_para cycles to guarantee that the registers are ready
	 * to be edited.
	 */
	ndelay(10);

	/* Set PHY AC termination mode */
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE0_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE1_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
	/* [한국어] PHY0 레인 2. */
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE2_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
	/* [한국어] PHY0 레인 3. 여기까지가 첫 PHY 의 네 레인이다. */
	fu740_phyregwrite(0, PCIEX8MGMT_PHY_LANE3_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
	/* [한국어] PHY1 레인 0. 두 번째 PHY 로 넘어간다. */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE0_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
	/* [한국어] PHY1 레인 1. */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE1_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
	/* [한국어] PHY1 레인 2. */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE2_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
	/* [한국어] PHY1 레인 3. 두 PHY 4레인씩 합쳐 x8 링크가 된다.
	 * 레인 상수들이 모두 같은 산술(BASE + 0x100 * n)로 정의되어 있어 루프로도
	 * 쓸 수 있었을 자리다. */
	fu740_phyregwrite(1, PCIEX8MGMT_PHY_LANE3_BASE, PCIEX8MGMT_PHY_INIT_VAL, afp);
}

/* [한국어]
 * fu740_pcie_start_link - Gen1 으로 먼저 링크를 세운 뒤 원래 속도로 올린다
 *
 * @pci: DWC 인스턴스.
 * @return: 0 성공, 음수는 링크 대기 실패값.
 *
 * 이 드라이버의 가장 특징적인 함수다. 곧바로 목표 속도로 학습하지 않고
 * **2.5GT/s(Gen1)로 낮춰 링크를 세운 다음** 원래 속도로 다시 협상한다.
 * 상류 주석이 이유를 적어 두었다 -- 이 하드웨어는 고속에서 바로 학습하면
 * 링크가 서지 않는 경우가 있어, 안정적인 저속으로 먼저 붙이고 올린다.
 *
 * 단계:
 *  1. LNKCAP 의 지원 속도 필드(SLS)를 읽어 **원래 값을 orig 에 보관**하고
 *     2.5GB 로 낮춰 쓴다. LNKCAP 은 읽기 전용이라 전체가
 *     dw_pcie_dbi_ro_wr_en / _dis 로 감싸여 있다.
 *  2. APP_LTSSM_ENABLE 에 1 을 써 링크 학습을 시작한다.
 *  3. 링크를 기다린다. 실패하면 err 로.
 *  4. LNKCAP 을 다시 읽어 여전히 낮은 값이면(즉 우리가 바꾼 그대로면)
 *     orig 를 되돌려 쓰고, PORT_LOGIC_SPEED_CHANGE 를 세워 재협상을 시킨다.
 *  5. 새 속도로 다시 링크를 기다린다.
 *
 * 4번의 조건이 '같지 않으면' 인 점에 유의 -- 하드웨어가 스스로 값을
 * 되돌렸다면 다시 손댈 필요가 없다는 뜻이다.
 *
 * err 라벨의 WARN_ON(ret) 은 실패 시 스택 트레이스를 남긴다. 성공 경로도
 * 이 라벨을 지나지만 ret 이 0 이라 아무 일도 일어나지 않는다 -- 그 덕에
 * dw_pcie_dbi_ro_wr_dis 를 두 경로가 공유한다.
 *
 * 실행 컨텍스트: DWC 코어가 부르는 프로세스 문맥. 링크 대기가 잠들 수 있다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_start_link) → pci->ops->start_link → [이 함수]
 *     → dw_pcie_dbi_ro_wr_en → dw_pcie_wait_for_link (최대 2회)
 */
static int fu740_pcie_start_link(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;
	struct fu740_pcie *afp = dev_get_drvdata(dev);
	/* [한국어] LNKCAP 오프셋의 기준이 될 PCI Express 능력 구조 위치.
	 * **0 인 경우를 검사하지 않는다**(상류 그대로) -- PCIe 장치라면 반드시
	 * 존재하므로 실제로 도달하지는 않는다. */
	u8 cap_exp = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* [한국어] 각 링크 대기의 결과. */
	int ret;
	/* [한국어] orig 는 원래 지원 속도, tmp 는 읽기-수정-쓰기용 임시값. */
	u32 orig, tmp;
/* [한국어] 아래 전체가 dbi_ro_wr_en/_dis 구간 안이다 -- LNKCAP 이 읽기 전용이기 때문이다. */

	/*
	 * Force 2.5GT/s when starting the link, due to some devices not
	 * probing at higher speeds. This happens with the PCIe switch
	 * on the Unmatched board when U-Boot has not initialised the PCIe.
	 * The fix in U-Boot is to force 2.5GT/s, which then gets cleared
	 * by the soft reset done by this driver.
	 */
	dev_dbg(dev, "cap_exp at %x\n", cap_exp);
	dw_pcie_dbi_ro_wr_en(pci);

	tmp = dw_pcie_readl_dbi(pci, cap_exp + PCI_EXP_LNKCAP);
	/* [한국어] **원래 지원 속도를 보관해 둔다.** 링크가 선 뒤 이 값으로 되돌린다. */
	orig = tmp & PCI_EXP_LNKCAP_SLS;
	/* [한국어] 지원 속도 필드를 비운다. */
	tmp &= ~PCI_EXP_LNKCAP_SLS;
	/* [한국어] 2.5GT/s(Gen1)로 낮춘다. 이 하드웨어는 고속에서 바로 학습하면 링크가
	 * 서지 않는 경우가 있어, 안정적인 저속으로 먼저 붙인다. */
	tmp |= PCI_EXP_LNKCAP_SLS_2_5GB;
	/* [한국어] 낮춘 값을 실제로 반영한다. */
	dw_pcie_writel_dbi(pci, cap_exp + PCI_EXP_LNKCAP, tmp);
/* [한국어] 이제 링크 학습을 시작할 준비가 됐다. */

	/* Enable LTSSM */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_APP_LTSSM_ENABLE);

	ret = dw_pcie_wait_for_link(pci);
	/* [한국어] Gen1 으로도 링크가 서지 않았다. */
	if (ret) {
		/* [한국어] 저속에서도 실패했다면 배선이나 장치 자체의 문제다. */
		dev_err(dev, "error: link did not start\n");
		/* [한국어] err 라벨로 -- WARN_ON 과 dbi_ro_wr_dis 를 거친다. */
		goto err;
	}

	tmp = dw_pcie_readl_dbi(pci, cap_exp + PCI_EXP_LNKCAP);
	/* [한국어] 우리가 낮춰 둔 값이 그대로면(= 하드웨어가 스스로 되돌리지 않았으면)
	 * 원래 속도로 다시 올린다. 이미 되돌아가 있으면 손댈 필요가 없다. */
	if ((tmp & PCI_EXP_LNKCAP_SLS) != orig) {
		/* [한국어] 속도 재협상이 일어난다는 것을 디버그 로그로 남긴다. */
		dev_dbg(dev, "changing speed back to original\n");
/* [한국어] 아래에서 원래 값을 복원한다. */

		tmp &= ~PCI_EXP_LNKCAP_SLS;
		/* [한국어] 보관해 둔 원래 지원 속도를 넣는다. */
		tmp |= orig;
		/* [한국어] 복원한 값을 반영한다. */
		dw_pcie_writel_dbi(pci, cap_exp + PCI_EXP_LNKCAP, tmp);
/* [한국어] 이제 재협상을 트리거한다. */

		tmp = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
		/* [한국어] PORT_LOGIC_SPEED_CHANGE 를 세우면 링크가 새 속도로 다시 협상한다. */
		tmp |= PORT_LOGIC_SPEED_CHANGE;
		/* [한국어] 트리거를 실제로 건다. */
		dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, tmp);
/* [한국어] 새 속도로 링크가 다시 서기를 기다린다. */

		ret = dw_pcie_wait_for_link(pci);
		/* [한국어] 고속 재협상 실패. */
		if (ret) {
			/* [한국어] Gen1 으로는 섰는데 목표 속도로는 안 됐다는 뜻이라, 앞의 실패와 구별해
			 * 다른 메시지를 남긴다. */
			dev_err(dev, "error: link did not start at new speed\n");
			/* [한국어] 같은 err 라벨로. */
			goto err;
		}
	}

	ret = 0;
err:
	WARN_ON(ret);	/* we assume that errors will be very rare */
	dw_pcie_dbi_ro_wr_dis(pci);
	return ret;
}

/* [한국어]
 * fu740_pcie_host_init - 리셋·클록·PHY 를 정해진 순서로 올린다
 *
 * @pp: DWC 루트 포트.
 * @return: 0 성공, 음수는 클록/리셋 실패값.
 *
 * 순서가 이 함수의 전부이고, 그 근거는 상류 주석에 나뉘어 적혀 있다:
 *
 *  1. drive_reset -- 리셋을 걸고 전원을 켜고 리셋을 푼다.
 *  2. pcie_aux 클록을 켠다. 아래 PHY 초기화에 이 클록이 필요하다.
 *  3. **APP_HOLD_PHY_RST 에 1** -- PHY 를 리셋 상태로 붙들어 둔다.
 *     그래야 아래에서 PHY 레지스터를 설정하는 동안 PHY 가 동작을 시작하지
 *     않는다.
 *  4. 컨트롤러 리셋 해제.
 *  5. PHY 초기화(8개 레인).
 *  6. **클록을 껐다가** APP_HOLD_PHY_RST 를 0 으로 내리고 **다시 켠다.**
 *     이 껐다 켜기가 요점이다 -- PHY 리셋을 푸는 순간에 클록이 흐르고
 *     있으면 설정이 반영되기 전에 PHY 가 움직인다.
 *  7. DEVICE_TYPE 에 4 를 써 루트 컴플렉스 모드로 지정한다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음):
 *  - 4번 reset_control_deassert 가 실패하면 2번에서 켠 pcie_aux 클록을
 *    끄지 않고 반환한다.
 *  - 6번의 두 번째 clk_prepare_enable 은 반환값을 검사하지 않는다.
 *    2번의 첫 번째 호출은 검사하는 것과 대조된다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init → pp->ops->init → [이 함수] → fu740_pcie_drive_reset
 *     → clk_prepare_enable → reset_control_deassert → fu740_pcie_init_phy
 */
static int fu740_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct fu740_pcie *afp = to_fu740_pcie(pci);
	/* [한국어] 오류 로그의 주체. */
	struct device *dev = pci->dev;
	/* [한국어] 클록·리셋 호출의 반환값. */
	int ret;
/* [한국어] 아래 순서 하나하나가 하드웨어 의존성을 따른다. */

	/* Power on reset */
	fu740_pcie_drive_reset(afp);

	/* Enable pcieauxclk */
	ret = clk_prepare_enable(afp->pcie_aux);
	if (ret) {
		/* [한국어] 보조 클록 없이는 PHY 설정을 할 수 없다. */
		dev_err(dev, "unable to enable pcie_aux clock\n");
		/* [한국어] 코드 관찰: 이 시점에는 되감을 것이 없으므로 바로 반환해도 무방하다. */
		return ret;
	}

	/*
	 * Assert hold_phy_rst (hold the controller LTSSM in reset after
	 * power_up_rst_n for register programming with cr_para)
	 */
	writel_relaxed(0x1, afp->mgmt_base + PCIEX8MGMT_APP_HOLD_PHY_RST);

	/* Deassert power_up_rst_n */
	ret = reset_control_deassert(afp->rst);
	if (ret) {
		/* [한국어] 컨트롤러 리셋 해제 실패. */
		dev_err(dev, "unable to deassert pcie_power_up_rst_n\n");
		/* [한국어] 코드 관찰 (상류 그대로, 수정하지 않음): **바로 위에서 켠 pcie_aux 클록을
		 * 끄지 않고 반환한다.** 위 클록 실패 경로와 달리 여기서는 되감을 것이 있다. */
		return ret;
	}

	fu740_pcie_init_phy(afp);

	/* Disable pcieauxclk */
	clk_disable_unprepare(afp->pcie_aux);
	/* Clear hold_phy_rst */
	writel_relaxed(0x0, afp->mgmt_base + PCIEX8MGMT_APP_HOLD_PHY_RST);
	/* Enable pcieauxclk */
	clk_prepare_enable(afp->pcie_aux);
	/* Set RC mode */
	writel_relaxed(0x4, afp->mgmt_base + PCIEX8MGMT_DEVICE_TYPE);

	return 0;
}

static const struct dw_pcie_host_ops fu740_pcie_host_ops = {
	/* [한국어] 되부를 훅은 init 하나뿐이다. */
	.init = fu740_pcie_host_init,
/* [한국어] post_init 도 자체 MSI 도 없다. */
};

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] **start_link 만 등록한다.** link_up 은 코어의 기본 구현(PORT_DEBUG1 비트)을
	 * 쓰고, stop_link 는 두지 않는다 -- 이 드라이버에 링크를 멈추는 경로가
	 * 없기 때문이다(shutdown 은 PERST# 만 건다). */
	.start_link = fu740_pcie_start_link,
};

/* [한국어]
 * fu740_pcie_probe - SiFive FU740 PCIe 드라이버 진입점
 *
 * @pdev: 매칭된 플랫폼 디바이스.
 * @return: 0 성공, 음수는 각 조회의 실패값.
 *
 * 자원만 챙기고 DWC 코어에 넘긴다. 실제 하드웨어 초기화는 코어가 되부르는
 * fu740_pcie_host_init 에서 일어난다.
 *
 * 챙기는 것:
 *  - "mgmt" 메모리 창 -- SiFive 가 DWC IP 바깥에 덧붙인 관리 레지스터.
 *    PERST_N, LTSSM_ENABLE, PHY 제어가 모두 여기 있다. 필수다.
 *  - reset / pwren GPIO -- 둘 다 **optional** 이다. 전원이 상시 인가되고
 *    PERST# 가 하드웨어로 처리되는 보드에서는 없을 수 있다.
 *  - "pcie_aux" 클록 -- 필수. PHY 설정에 쓰인다.
 *  - 리셋 라인 -- 이름 없이(NULL) exclusive 로 하나 얻는다. 필수다.
 *
 * pci->pp.num_vectors 를 MAX_MSI_IRQS(256)로 두어 최대치를 요구한다.
 * 실제 확보량은 DWC 코어가 인터럽트 선 개수를 보고 깎는다.
 *
 * platform_set_drvdata 를 **자원 확보가 모두 끝난 뒤** 부르는 점에 유의.
 * to_fu740_pcie() 매크로가 dev_get_drvdata 이므로, 이 호출 전에 DWC 콜백이
 * 불리면 NULL 을 얻는다 -- 다만 그 콜백은 바로 다음 줄의 dw_pcie_host_init
 * 안에서야 불리므로 순서가 성립한다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → devm_platform_ioremap_resource_byname
 *     → devm_gpiod_get_optional → devm_clk_get
 *       → devm_reset_control_get_exclusive → dw_pcie_host_init
 */
static int fu740_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci;
	/* [한국어] 이 드라이버 인스턴스. */
	struct fu740_pcie *afp;
/* [한국어] 아래에서 할당한다. */

	afp = devm_kzalloc(dev, sizeof(*afp), GFP_KERNEL);
	/* [한국어] 인스턴스 할당 실패. */
	if (!afp)
		/* [한국어] devm 이라 이후 자동 해제된다. */
		return -ENOMEM;
	pci = &afp->pci;
	/* [한국어] DWC 코어가 로그와 DT 접근에 쓸 device. */
	pci->dev = dev;
	/* [한국어] 링크 기동 콜백 테이블을 건다. */
	pci->ops = &dw_pcie_ops;
	/* [한국어] 호스트 훅을 건다. 이것이 있어야 코어가 host_init 을 되부른다. */
	pci->pp.ops = &fu740_pcie_host_ops;
	/* [한국어] 하드웨어가 낼 수 있는 최대치(256)를 요구한다. 실제 확보량은 코어가
	 * 인터럽트 선 개수를 보고 깎는다. */
	pci->pp.num_vectors = MAX_MSI_IRQS;
/* [한국어] 여기부터 DT 자원을 챙긴다. */

	/* SiFive specific region: mgmt */
	afp->mgmt_base = devm_platform_ioremap_resource_byname(pdev, "mgmt");
	if (IS_ERR(afp->mgmt_base))
		/* [한국어] 관리 창 없이는 PERST_N, LTSSM, PHY 제어에 닿을 수 없으므로 필수다. */
		return PTR_ERR(afp->mgmt_base);
/* [한국어] 다음 자원으로. */

	/* Fetch GPIOs */
	afp->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(afp->reset))
		/* [한국어] GPIO 표기가 잘못된 경우만 여기 걸린다 -- optional 이라 '없음' 은 NULL 이고
		 * 오류가 아니다. */
		return dev_err_probe(dev, PTR_ERR(afp->reset), "unable to get reset-gpios\n");
/* [한국어] dev_err_probe 는 -EPROBE_DEFER 일 때 로그를 남기지 않는다. */

	afp->pwren = devm_gpiod_get_optional(dev, "pwren", GPIOD_OUT_LOW);
	/* [한국어] 전원 GPIO 조회 실패. */
	if (IS_ERR(afp->pwren))
		/* [한국어] reset 쪽과 같은 이유로 optional 이며, 여기 걸리는 것은 표기 오류뿐이다. */
		return dev_err_probe(dev, PTR_ERR(afp->pwren), "unable to get pwren-gpios\n");

	/* Fetch clocks */
	afp->pcie_aux = devm_clk_get(dev, "pcie_aux");
	if (IS_ERR(afp->pcie_aux))
		/* [한국어] 보조 클록은 필수라 없으면 실패한다. */
		return dev_err_probe(dev, PTR_ERR(afp->pcie_aux),
				     /* [한국어] 클록 공급자가 늦게 올라오는 -EPROBE_DEFER 가 흔하다. */
				     "pcie_aux clock source missing or invalid\n");

	/* Fetch reset */
	afp->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(afp->rst))
		/* [한국어] 리셋 라인도 필수다. exclusive 라 다른 드라이버가 같은 리셋을 쥐고 있어도
		 * 여기서 실패한다. */
		return dev_err_probe(dev, PTR_ERR(afp->rst), "unable to get reset\n");
/* [한국어] 모든 자원이 확보됐다. */

	platform_set_drvdata(pdev, afp);
/* [한국어] **자원 확보가 모두 끝난 뒤** 인스턴스를 매단다. to_fu740_pcie() 가
 * dev_get_drvdata 이므로, 바로 아래 dw_pcie_host_init 이 host_init 을
 * 되부르기 전에 심어져 있어야 한다. */

	return dw_pcie_host_init(&pci->pp);
}

/* [한국어]
 * fu740_pcie_shutdown - 시스템 종료·재부팅 직전에 링크를 끊는다
 *
 * @pdev: 이 플랫폼 디바이스.
 * @return: 없음.
 *
 * 상류 주석이 이유를 적어 두었다 -- 종료 시 PERST# 를 걸어 두지 않으면
 * 다음 부팅에서 장치가 이전 상태를 들고 있어 열거가 어긋날 수 있다.
 *
 * remove 가 아니라 shutdown 만 있는 점에 유의. 드라이버에
 * suppress_bind_attrs = true 가 걸려 있어 sysfs 로 언바인드할 수 없으므로,
 * 이 드라이버가 내려가는 경우는 시스템 종료뿐이다.
 *
 * 실행 컨텍스트: 시스템 종료 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스(shutdown) → [이 함수] → fu740_pcie_assert_reset
 */
static void fu740_pcie_shutdown(struct platform_device *pdev)
{
	struct fu740_pcie *afp = platform_get_drvdata(pdev);

	/* Bring down link, so bootloader gets clean state in case of reboot */
	fu740_pcie_assert_reset(afp);
}

static const struct of_device_id fu740_pcie_of_match[] = {
	/* [한국어] SiFive FU740 SoC 의 PCIe 컨트롤러. 이 드라이버가 지원하는 유일한 compatible. */
	{ .compatible = "sifive,fu740-pcie", },
	/* [한국어] 표의 끝. */
	{},
};

static struct platform_driver fu740_pcie_driver = {
	/* [한국어] 플랫폼 드라이버 등록 정보. */
	.driver = {
		   /* [한국어] sysfs 에 나타날 이름. */
		   .name = "fu740-pcie",
		   /* [한국어] 위 표를 걸어 매칭되면 probe 가 불린다. suppress_bind_attrs 로 sysfs
		    * 언바인드를 막아 두었으므로, 이 드라이버가 내려가는 경우는 시스템 종료뿐이다. */
		   .of_match_table = fu740_pcie_of_match,
		   .suppress_bind_attrs = true,
	},
	.probe = fu740_pcie_probe,
	.shutdown = fu740_pcie_shutdown,
};

builtin_platform_driver(fu740_pcie_driver);
