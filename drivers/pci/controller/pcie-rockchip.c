// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip AXI PCIe host controller driver
 *
 * Copyright (c) 2016 Rockchip, Inc.
 *
 * Author: Shawn Lin <shawn.lin@rock-chips.com>
 *         Wenrui Li <wenrui.li@rock-chips.com>
 *
 * Bits taken from Synopsys DesignWare Host controller driver and
 * ARM PCI Host generic driver.
 */

/* [한국어] clk_bulk_prepare_enable() / devm_clk_bulk_get_all(). 클록을 이름 없이
 * 통째로 받는 방식이라 SoC 마다 개수가 달라도 코드가 같다. */
/*
 * [한국어 설명] Rockchip AXI PCIe 컨트롤러의 RC/EP 공용 코드 (pcie-rockchip.c)
 *
 * === 파일의 역할 ===
 * Rockchip SoC 의 자체 PCIe 컨트롤러를 다루는 두 드라이버 — 루트 컴플렉스인
 * pcie-rockchip-host.c 와 엔드포인트인 pcie-rockchip-ep.c — 가 공유하는
 * 공통 코드다. DesignWare 계열이 아니라 Rockchip 자체 IP 이므로, DWC 코어가
 * 대신 해 주는 일들(자원 확보, 리셋·PHY 순서, 클록)을 여기서 직접 짠다.
 * 담고 있는 것은 함수 일곱 개이며 크게 세 묶음이다.
 * (1) 자원 확보 — parse_dt(), get_phys()
 * (2) 전원·리셋 순서 — init_port(), deinit_phys(), enable/disable_clocks()
 * (3) 주소 변환 창 설정 — cfg_configuration_accesses()
 * 이 파일을 읽는 열쇠는 **is_rc 플래그 하나가 두 모드를 가른다** 는 점이다.
 * 같은 IP 를 루트 컴플렉스로도 엔드포인트로도 쓸 수 있고, 그 차이가 코드
 * 몇 줄로 압축되어 있다. parse_dt 에서는 메모리 자원의 성격(RC 는 매핑,
 * EP 는 서술자만)과 PERST# GPIO 의 방향(RC 는 출력, EP 는 입력)이 갈리고,
 * init_port 에서는 클라이언트 설정 레지스터에 쓰는 값이 갈린다 —
 * RC 는 링크 훈련 활성화, EP 는 설정 비활성화. 같은 신호와 같은 레지스터를
 * 두 모드가 정반대로 쓰는 셈이다.
 * 또 하나 눈에 띄는 것은 DT 값에 대한 관대함이다. num-lanes 가 0/3/4 초과면
 * 오류로 거절하지 않고 경고와 함께 1 로 되돌리고, link_gen 도 범위를 벗어나면
 * Gen2 로 맞춘다. DT 의 실수 때문에 부팅이 막히지 않게 하려는 판단이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * Rockchip PCIe 스택은 네 파일이다. 레지스터 지도와 struct rockchip_pcie 를
 * 정의하는 pcie-rockchip.h, 그 위의 공통 코드인 이 파일, 그리고 서로 반대
 * 역할인 pcie-rockchip-host.c 와 pcie-rockchip-ep.c.
 * 이 파일에는 probe 도 모듈 진입점도 없다. 대신 일곱 함수를 모두
 * EXPORT_SYMBOL_GPL 로 내보내는데, host 판과 ep 판이 각각 모듈이 될 수
 * 있기 때문이다. 호출은 언제나 위에서 아래로 한 방향이며 콜백으로
 * 되불리는 일이 없다.
 * 전형적인 흐름:
 *   host/ep probe → parse_dt()(자원 확보, 안에서 get_phys() 호출)
 *     → enable_clocks() → init_port()(리셋·PHY 순서)
 *     → 이후 host 는 config 접근마다 cfg_configuration_accesses()
 *   remove/suspend → deinit_phys() → disable_clocks()
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. init_port 의 PHY PLL 대기가
 * 최대 100ms 잠들 수 있고, 나머지는 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-rockchip-host.c 와 pcie-rockchip-ep.c 가 이 파일의 일곱 함수를
 * 모두 쓴다. 두 드라이버를 가르는 것은 rockchip->is_rc 하나이며, 그 값은
 * 각 드라이버의 probe 가 채운다.
 * 옆쪽: pcie-rockchip.h 의 struct rockchip_pcie, 레지스터 상수
 * (PCIE_CLIENT_CONFIG, PCIE_CLIENT_SIDE_BAND_STATUS, PCIE_CORE_OB_REGION_ 계열,
 * PCIE_RC_BAR_CONF), 접근자 rockchip_pcie_read/write, 그리고 리셋 이름 표
 * rockchip_pci_pm_rsts / rockchip_pci_core_rsts.
 * 아래쪽: 리셋 서브시스템(reset_control_bulk_ 계열), PHY 서브시스템
 * (devm_phy_get, devm_of_phy_get, phy_init/exit/power_on/off), 클록
 * 서브시스템(devm_clk_bulk_get_all, clk_bulk_prepare_enable), GPIO
 * (devm_gpiod_get_optional), 그리고 PCI 코어의
 * devm_pci_remap_cfg_resource() 와 of_pci_get_max_link_speed().
 * 공유 상태: struct rockchip_pcie 하나다. 이 파일은 그 안의 reg_base,
 * apb_base, mem_res, phys[], legacy_phy, lanes, link_gen, pm_rsts[],
 * core_rsts[], perst_gpio, clks, num_clks 를 채우고, is_rc 와 lanes_map 은
 * 읽기만 한다. 전역 변수는 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - rockchip_pcie_parse_dt(): 자원 확보의 전부. is_rc 로 갈리는 곳이 둘뿐이며
 *   (메모리 자원의 성격, PERST# GPIO 방향), 리셋을 PM 계열과 Core 계열 두
 *   묶음으로 나눠 확보한다 — 그 이유는 init_port 의 해제 순서에 있다.
 * - rockchip_pcie_init_port(): 이 파일에서 가장 순서가 중요한 함수.
 *   PM 리셋을 **먼저** 풀고 그 사이에 클라이언트 설정을 쓴 뒤 Core 리셋을
 *   **나중에** 푼다. 코어가 동작 중이면 설정이 반영되지 않기 때문이다.
 *   되감기가 err_power_off_phy → err_exit_phy 폭포 구조이며, 중간에서
 *   i 를 MAX_LANE_NUM 으로 되돌리는 한 줄이 두 라벨을 잇는 고리다.
 * - rockchip_pcie_get_phys(): DT 바인딩이 바뀐 흔적. 옛 "pcie-phy" 하나를
 *   먼저 시도하고 없으면 "pcie-phy-N" 을 레인마다 찾는다. 옛 방식 조회가
 *   -EPROBE_DEFER 로 실패하면 새 방식을 시도하지 **않고** 그대로 반환하는데,
 *   지연은 "없다" 가 아니라 "아직" 이기 때문이다.
 * - rockchip_pcie_deinit_phys(): 전원 차단은 lanes_map 에 표시된 레인만,
 *   초기화 해제는 전 레인에 대해 한다. 두 반복의 범위가 다른 것은 실수가
 *   아니라 init_port 의 비대칭을 그대로 되짚는 것이다.
 * - rockchip_pcie_enable_clocks() / _disable_clocks(): 한 줄짜리 래퍼.
 *   켜기만 반환값이 있는데, 끄기는 실패를 알려도 호출자가 할 일이 없기 때문이다.
 * - rockchip_pcie_cfg_configuration_accesses(): outbound region 0 의
 *   디스크립터 타입을 갈아 끼운다. config 접근이 Type 0 인지 Type 1 인지를
 *   이 하드웨어는 region 디스크립터로 표현하므로, 대상 버스가 바뀔 때마다
 *   host 드라이버가 이 함수를 부른다.
 *
 * === 이 트리에서 확인하지 못한 것 ===
 * init_port 의 udelay(10) 이 요구하는 리셋 유지 시간, 그리고
 * cfg_configuration_accesses 가 디스크립터에 넣는 23번 비트의 의미는
 * 벤더 문서에만 있어 확인할 수 없었다. 코드가 하는 일만 적었다.
 *
 * === NVMe 관점 ===
 * 직접적인 코드 접점은 없다. 다만 Rockchip SoC 를 쓰는 보드에 NVMe SSD 를
 * 붙이면 이 파일이 그 링크의 아래쪽 절반을 담당한다 — parse_dt 가 읽는
 * num-lanes 와 max-link-speed 가 곧 그 SSD 가 쓸 수 있는 대역폭의 상한이고,
 * init_port 의 PHY PLL 잠금 대기가 실패하면 SSD 는 아예 열거되지 않는다.
 * 이 컨트롤러가 Gen2 까지만 지원한다는 점도 실무에서 의미가 있다 —
 * Gen3/Gen4 NVMe 를 꽂아도 Gen2 로 협상된다.
 */

#include <linux/clk.h>
/* [한국어] udelay() — 리셋 사이의 짧은 대기에 쓴다. */
#include <linux/delay.h>
/* [한국어] devm_gpiod_get_optional() — PERST# GPIO. */
#include <linux/gpio/consumer.h>
/* [한국어] readx_poll_timeout() — PHY PLL 잠금을 기다리는 폴링 매크로. */
#include <linux/iopoll.h>
/* [한국어] of_property_read_u32() — DT 의 num-lanes. */
#include <linux/of.h>
/* [한국어] of_pci_get_max_link_speed() — DT 의 max-link-speed. */
#include <linux/of_pci.h>
/* [한국어] devm_phy_get() / devm_of_phy_get() / phy_init/exit/power_on/off. */
#include <linux/phy/phy.h>
/* [한국어] platform_get_resource_byname(), devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] reset_control_bulk_ 계열. 이 드라이버는 리셋을 PM 계열과 Core 계열
 * 두 묶음으로 나눠 다룬다. */
#include <linux/reset.h>

/* [한국어] devm_pci_remap_cfg_resource() 선언. config 공간 전용 매핑이다. */
#include "../pci.h"
/* [한국어] struct rockchip_pcie 와 레지스터 상수, rockchip_pcie_read/write 접근자,
 * 그리고 rockchip_pci_pm_rsts / rockchip_pci_core_rsts 리셋 이름 표. */
#include "pcie-rockchip.h"

/* [한국어]
 * rockchip_pcie_parse_dt - DT 에서 자원을 모두 확보한다 (RC/EP 공통)
 *
 * @rockchip: 이 컨트롤러의 상태. is_rc 가 미리 채워져 있어야 한다.
 * @return: 0 = 성공, 그 밖에 각 자원 확보 단계의 오류.
 *
 * host 판과 ep 판이 공유하는 자원 확보 함수다. is_rc 플래그 하나가 두 모드를
 * 가르며, 이 파일 전체를 관통하는 방식이기도 하다.
 *
 * 모드에 따라 갈리는 곳이 둘뿐이라는 점이 깔끔하다.
 * 첫째, 메모리 자원. RC 는 "axi-base" 를 config 전용으로 **매핑** 하고,
 * EP 는 "mem-base" 의 자원 서술자만 **들고** 있는다. EP 에게 그 구간은
 * 호스트가 볼 BAR 로 내줄 대상이지 자기가 읽고 쓰는 창이 아니기 때문이다.
 * 둘째, PERST# GPIO 의 방향. RC 는 "ep" 를 출력으로(상대에게 리셋을 내보낸다),
 * EP 는 "reset" 을 입력으로(호스트가 보내는 리셋을 받는다) 얻는다.
 * 같은 신호선을 두 모드가 정반대 방향으로 쓰는 셈이다.
 *
 * DT 값의 유효성 검사가 관대하다. num-lanes 가 0, 3, 4 초과이면 오류로
 * 거절하지 않고 경고와 함께 1 로 되돌린다(PCIe 는 1/2/4/8/16 레인만 정의하고
 * 이 컨트롤러는 4 레인까지라 3 이 빠진다). link_gen 도 범위를 벗어나면
 * Gen2 로 맞춘다. DT 의 실수 때문에 부팅이 막히지 않게 하려는 것이다.
 *
 * 리셋을 PM 계열과 Core 계열 두 묶음으로 나눠 확보하는데, 그 이유는
 * rockchip_pcie_init_port() 에서 해제 순서가 다르기 때문이다.
 *
 * 클록은 이름 없이 devm_clk_bulk_get_all() 로 통째로 받는다. SoC 마다 개수와
 * 이름이 달라도 이 한 줄로 처리되며, 반환값이 곧 개수다 — 그래서 0 은
 * 정상(클록 없음)이고 음수만 실패다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 모든 할당이 devm_ 이라 되돌릴 것이 없다. 대부분
 * dev_err_probe() 로 보고해 -EPROBE_DEFER 를 조용히 처리한다.
 *
 * 호출 체인:
 *   pcie-rockchip-host.c / pcie-rockchip-ep.c 의 probe → [이 함수]
 *     → platform_get_resource_byname() → devm_pci_remap_cfg_resource()
 *     → rockchip_pcie_get_phys() → devm_reset_control_bulk_get_exclusive() ×2
 *     → devm_gpiod_get_optional() → devm_clk_bulk_get_all()
 */
int rockchip_pcie_parse_dt(struct rockchip_pcie *rockchip)
{
	/* [한국어] 진단 메시지와 devm_ 할당의 기준이 될 device. */
	struct device *dev = rockchip->dev;
	/* [한국어] 자원 조회에 쓸 플랫폼 장치. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] DT 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] 조회한 자원. */
	struct resource *regs;
	/* [한국어] 오류 코드와 순회 인덱스. */
	int err, i;

	/* [한국어] 루트 컴플렉스 모드면, */
	if (rockchip->is_rc) {
		/* [한국어] "axi-base" 라는 이름의 자원을 찾는다. RC 는 이 창으로 하위 장치의
		 * config 공간에 접근한다. */
		regs = platform_get_resource_byname(pdev,
						    IORESOURCE_MEM,
						    "axi-base");
		/* [한국어] config 전용 매핑 함수를 쓴다. 평범한 ioremap 이 아닌 이유는 아키텍처에
		 * 따라 config 공간이 다른 메모리 속성을 요구하기 때문이다. */
		rockchip->reg_base = devm_pci_remap_cfg_resource(dev, regs);
		/* [한국어] 실패하면, */
		if (IS_ERR(rockchip->reg_base))
			return PTR_ERR(rockchip->reg_base);
	} else {
		/* [한국어] 엔드포인트 모드면 "mem-base" 를 찾는다. 매핑하지 않고 자원 서술자만
		 * 들고 있는데, EP 는 이 구간을 호스트가 볼 BAR 로 내주는 것이지 자기가
		 * 읽고 쓰는 창이 아니기 때문이다. RC 와 EP 가 갈리는 첫 지점이다. */
		rockchip->mem_res =
			platform_get_resource_byname(pdev, IORESOURCE_MEM,
						     "mem-base");
		/* [한국어] 자원이 없으면, */
		if (!rockchip->mem_res)
			return -EINVAL;
	}

	/* [한국어] 두 모드 공통인 APB 레지스터 창. 컨트롤러 제어 레지스터가 여기 있다. */
	rockchip->apb_base =
		devm_platform_ioremap_resource_byname(pdev, "apb-base");
	/* [한국어] 매핑 실패면, */
	if (IS_ERR(rockchip->apb_base))
		return PTR_ERR(rockchip->apb_base);

	/* [한국어] PHY 를 확보한다. 레인마다 하나씩일 수도, 옛 방식으로 하나뿐일 수도 있다. */
	err = rockchip_pcie_get_phys(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		return err;

	/* [한국어] 기본값을 1 레인으로 둔다. DT 에 지정이 없거나 값이 이상할 때 쓰인다. */
	rockchip->lanes = 1;
	/* [한국어] DT 가 지정한 레인 수를 읽는다. */
	err = of_property_read_u32(node, "num-lanes", &rockchip->lanes);
	/* [한국어] 읽기에 성공했더라도 0, 3, 4 초과는 유효하지 않다. PCIe 는 1/2/4/8/16
	 * 레인만 정의하며 이 컨트롤러는 최대 4 레인이다 — 그래서 3 이 빠져 있다. */
	if (!err && (rockchip->lanes == 0 ||
		     rockchip->lanes == 3 ||
		     rockchip->lanes > 4)) {
		/* [한국어] 경고를 남기고, */
		dev_warn(dev, "invalid num-lanes, default to use one lane\n");
		/* [한국어] 1 레인으로 되돌린다. 오류로 거절하지 않는 것은 DT 의 실수 때문에
		 * 부팅이 막히지 않게 하려는 것이다. */
		rockchip->lanes = 1;
	}

	/* [한국어] DT 의 최대 링크 속도를 읽는다. */
	rockchip->link_gen = of_pci_get_max_link_speed(node);
	/* [한국어] 음수(지정 없음)이거나 2 를 넘으면, */
	if (rockchip->link_gen < 0 || rockchip->link_gen > 2)
		/* [한국어] Gen2 로 맞춘다. 이 컨트롤러가 Gen2 까지만 지원하기 때문이다. */
		rockchip->link_gen = 2;

	/* [한국어] PM 계열 리셋들의 이름을 배열에 채운다. */
	for (i = 0; i < ROCKCHIP_NUM_PM_RSTS; i++)
		/* [한국어] 이름 표는 pcie-rockchip.h 가 소유한다. */
		rockchip->pm_rsts[i].id = rockchip_pci_pm_rsts[i];

	/* [한국어] 그 이름들로 리셋 핸들을 한꺼번에 확보한다. 배타적(exclusive) 판이라
	 * 다른 드라이버와 공유하지 않는다. */
	err = devm_reset_control_bulk_get_exclusive(dev,
						    ROCKCHIP_NUM_PM_RSTS,
						    rockchip->pm_rsts);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] probe 지연이면 조용히 넘어가는 dev_err_probe 로 보고한다. */
		return dev_err_probe(dev, err, "Cannot get the PM reset\n");

	/* [한국어] Core 계열 리셋도 같은 방식으로. */
	for (i = 0; i < ROCKCHIP_NUM_CORE_RSTS; i++)
		/* [한국어] 이름 표에서 가져온다. */
		rockchip->core_rsts[i].id = rockchip_pci_core_rsts[i];

	/* [한국어] 핸들을 확보한다. 두 묶음을 나눠 두는 이유는 init_port 에서 해제 순서가
	 * 다르기 때문이다 — PM 을 먼저 풀고 Core 를 나중에 푼다. */
	err = devm_reset_control_bulk_get_exclusive(dev,
						    ROCKCHIP_NUM_CORE_RSTS,
						    rockchip->core_rsts);
	/* [한국어] 실패하면, */
	if (err)
		return dev_err_probe(dev, err, "Cannot get the Core resets\n");

	/* [한국어] RC 모드면, */
	if (rockchip->is_rc)
		/* [한국어] "ep" 라는 이름의 GPIO 를 **출력** 으로 얻는다. RC 는 상대 엔드포인트에
		 * PERST# 를 **내보내는** 쪽이기 때문이다. */
		rockchip->perst_gpio = devm_gpiod_get_optional(dev, "ep",
							       GPIOD_OUT_LOW);
	else
		/* [한국어] EP 모드면 "reset" 을 **입력** 으로 얻는다. EP 는 호스트가 보내는
		 * PERST# 를 **받는** 쪽이라 방향이 정반대다. 같은 신호선을 두 모드가
		 * 반대 방향으로 쓰는 것이 이 드라이버의 대칭 구조를 잘 보여 준다. */
		rockchip->perst_gpio = devm_gpiod_get_optional(dev, "reset",
							       GPIOD_IN);
	/* [한국어] 실패하면(없는 것은 optional 이라 오류가 아니다), */
	if (IS_ERR(rockchip->perst_gpio))
		return dev_err_probe(dev, PTR_ERR(rockchip->perst_gpio),
				     "failed to get PERST# GPIO\n");

	/* [한국어] 클록을 이름 없이 통째로 받는다. SoC 마다 개수와 이름이 달라도 이 한 줄로
	 * 처리되며, 반환값이 곧 개수다. */
	rockchip->num_clks = devm_clk_bulk_get_all(dev, &rockchip->clks);
	/* [한국어] 음수면 실패다. 개수를 반환하는 함수라 0 은 정상(클록 없음)이다. */
	if (rockchip->num_clks < 0)
		return dev_err_probe(dev, rockchip->num_clks,
				     "failed to get clocks\n");

	/* [한국어] 모든 자원을 확보했다. */
	return 0;
}
/* [한국어] host 판과 ep 판 두 드라이버가 각각 모듈이 될 수 있어 내보낸다. */
EXPORT_SYMBOL_GPL(rockchip_pcie_parse_dt);

/* [한국어] 아래 readx_poll_timeout 에 넘길 읽기 함수를 만든다. 그 매크로가 인자
 * 하나짜리 함수를 요구하므로, rockchip 을 캡처한 매크로로 감싼다. */
#define rockchip_pcie_read_addr(addr) rockchip_pcie_read(rockchip, addr)
/* 100 ms max wait time for PHY PLLs to lock */
/* [한국어] PHY PLL 잠금 대기 상한. 위 영어 주석대로 100ms 다. */
#define RK_PHY_PLL_LOCK_TIMEOUT_US 100000
/* Sleep should be less than 20ms */
/* [한국어] 폴링 간격. 위 영어 주석대로 20ms 미만이어야 한다는 제약을 1ms 로 만족한다. */
#define RK_PHY_PLL_LOCK_SLEEP_US 1000

/* [한국어]
 * rockchip_pcie_init_port - 리셋과 PHY 를 정해진 순서로 풀어 컨트롤러를 깨운다
 *
 * @rockchip: 이 컨트롤러의 상태.
 * @return: 0 = 성공, 그 밖에 어느 단계의 오류.
 *
 * 이 파일에서 가장 순서가 중요한 함수다. 리셋 두 묶음과 PHY 두 단계
 * (초기화·전원)를 엮어 여섯 걸음으로 진행한다.
 *
 *   1. PM 리셋을 건다.
 *   2. 레인마다 phy_init().
 *   3. Core 리셋을 건다.
 *   4. 10us 대기 후 PM 리셋을 **먼저** 푼다.
 *   5. 클라이언트 설정(속도, 레인 수, RC/EP 모드)을 쓴다.
 *   6. 레인마다 phy_power_on() → PHY PLL 잠금 대기 → Core 리셋을 **나중에** 푼다.
 *
 * PM 을 먼저 풀고 Core 를 나중에 푸는 것이 핵심이다. 그 사이에 클라이언트
 * 설정을 써야 하는데, 코어가 동작 중이면 그 설정이 반영되지 않기 때문이다.
 * parse_dt 가 리셋을 두 묶음으로 나눠 확보한 이유가 여기서 드러난다.
 *
 * RC 와 EP 가 갈리는 곳은 5번의 한 줄이다. RC 는 링크 훈련 활성화와 설정
 * 활성화를 켜고, EP 는 설정 **비활성화** 를 켠다. RC 가 스스로 훈련을
 * 시작하는 반면 EP 는 호스트가 시작하기를 기다리기 때문이다.
 *
 * 되감기가 두 라벨의 폭포 구조다. err_power_off_phy 에서 켠 전원을 되돌린 뒤
 * i 를 MAX_LANE_NUM 으로 되돌려 err_exit_phy 로 흘러내리는데, 전원 단계까지
 * 왔다는 것은 모든 레인의 phy_init 이 끝났다는 뜻이므로 전부 되돌려야 하기
 * 때문이다. 반대로 phy_init 도중 실패하면 i 가 실패 지점을 가리킨 채
 * err_exit_phy 로 직접 뛰어, 성공한 것까지만 되돌린다.
 *
 * PHY PLL 대기는 부정 논리다. 상태 비트가 **내려가야** 잠긴 것이라
 * 조건에 ! 가 붙는다. 1ms 간격으로 최대 100ms 기다리며, 그 간격은 함수 위
 * 영어 주석이 요구하는 "20ms 미만" 을 만족한다.
 *
 * 실행 컨텍스트: probe 와 재개 경로, 프로세스 컨텍스트. 폴링이 잠들 수 있다.
 *
 * 에러 경로: 위 두 라벨. 리셋은 되돌리지 않는데, 실패했다면 어차피 컨트롤러를
 * 쓸 수 없고 다음 시도가 다시 처음부터 걸기 때문이다.
 *
 * 호출 체인:
 *   host/ep 드라이버의 probe·resume → [이 함수]
 *     → reset_control_bulk_assert/deassert() ×4 → phy_init/power_on() ×레인
 *     → rockchip_pcie_write(PCIE_CLIENT_CONFIG) → readx_poll_timeout()
 */
int rockchip_pcie_init_port(struct rockchip_pcie *rockchip)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = rockchip->dev;
	/* [한국어] 오류 코드와 순회 인덱스. i 는 아래 되감기 라벨에서도 쓰이므로 함수
	 * 전체에 걸쳐 살아 있어야 한다. */
	int err, i;
	/* [한국어] 레지스터 값이자 폴링 결과를 받을 변수. 두 용도로 재사용된다. */
	u32 regs;

	/* [한국어] 먼저 PM 리셋을 건다. 전원 관리 영역을 리셋 상태로 두고 PHY 를 초기화한다. */
	err = reset_control_bulk_assert(ROCKCHIP_NUM_PM_RSTS,
					rockchip->pm_rsts);
	/* [한국어] 실패하면, */
	if (err)
		return dev_err_probe(dev, err, "Couldn't assert PM resets\n");

	/* [한국어] 레인마다 PHY 를 초기화한다. MAX_LANE_NUM 만큼 도는데, 실제로 쓰지 않는
	 * 레인의 phys[] 도 유효한 포인터이거나 NULL 이며 PHY 코어가 NULL 을
	 * 무해하게 처리한다. */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		/* [한국어] 초기화. */
		err = phy_init(rockchip->phys[i]);
		/* [한국어] 실패하면, */
		if (err) {
			/* [한국어] 몇 번 레인인지 남기고, */
			dev_err(dev, "init phy%d err %d\n", i, err);
			/* [한국어] 지금까지 초기화한 것만 되돌리러 간다. i 가 실패한 위치를 가리킨 채
			 * 라벨로 넘어가는 것이 되감기의 열쇠다. */
			goto err_exit_phy;
		}
	}

	/* [한국어] Core 리셋도 건다. PHY 초기화 뒤에 거는 순서다. */
	err = reset_control_bulk_assert(ROCKCHIP_NUM_CORE_RSTS,
					rockchip->core_rsts);
	/* [한국어] 실패하면, */
	if (err) {
		/* [한국어] 기록만 하고(반환하지 않는다), */
		dev_err_probe(dev, err, "Couldn't assert Core resets\n");
		/* [한국어] 되감기로 간다. */
		goto err_exit_phy;
	}

	/* [한국어] 리셋 신호를 유지할 최소 시간을 확보한다. 10마이크로초는 하드웨어
	 * 요구 사항으로, 이 트리에는 근거 문서가 없다. */
	udelay(10);

	/* [한국어] PM 리셋을 **먼저** 푼다. Core 는 아직 리셋 상태로 둔 채 클라이언트
	 * 설정을 하기 위해서다. */
	err = reset_control_bulk_deassert(ROCKCHIP_NUM_PM_RSTS,
					  rockchip->pm_rsts);
	/* [한국어] 실패하면, */
	if (err) {
		/* [한국어] 기록하고, */
		dev_err(dev, "Couldn't deassert PM resets %d\n", err);
		goto err_exit_phy;
	}

	/* [한국어] Gen2 를 쓰기로 했으면, */
	if (rockchip->link_gen == 2)
		/* [한국어] 클라이언트 설정 레지스터에 Gen2 선택을 쓴다. */
		rockchip_pcie_write(rockchip, PCIE_CLIENT_GEN_SEL_2,
				    PCIE_CLIENT_CONFIG);
	else
		/* [한국어] 아니면 Gen1 선택. 이 쓰기가 아래 규모 있는 설정보다 먼저 오는 이유는
		 * 속도 선택이 별도 필드가 아니라 같은 레지스터의 다른 비트이기 때문으로
		 * 보이며, 아래 쓰기가 이 값을 덮어쓰지 않도록 상수가 배치되어 있다. */
		rockchip_pcie_write(rockchip, PCIE_CLIENT_GEN_SEL_1,
				    PCIE_CLIENT_CONFIG);

	/* [한국어] ARI(Alternative Routing-ID) 활성화와, */
	regs = PCIE_CLIENT_ARI_ENABLE |
	       /* [한국어] 레인 수를 담은 공통 설정을 만든다. */
	       PCIE_CLIENT_CONF_LANE_NUM(rockchip->lanes);

	/* [한국어] RC 모드면, */
	if (rockchip->is_rc)
		/* [한국어] 링크 훈련 활성화와, */
		regs |= PCIE_CLIENT_LINK_TRAIN_ENABLE |
			/* [한국어] 설정 활성화, 그리고 RC 모드 표시를 더한다. */
			PCIE_CLIENT_CONF_ENABLE | PCIE_CLIENT_MODE_RC;
	else
		/* [한국어] EP 모드면 설정 **비활성화** 와 EP 모드 표시를 더한다. RC 가 스스로
		 * 링크 훈련을 시작하는 반면 EP 는 호스트가 시작하기를 기다리므로,
		 * 같은 레지스터에 정반대 값이 들어간다. */
		regs |= PCIE_CLIENT_CONF_DISABLE | PCIE_CLIENT_MODE_EP;

	/* [한국어] 완성된 값을 쓴다. */
	rockchip_pcie_write(rockchip, regs, PCIE_CLIENT_CONFIG);

	/* [한국어] 이제 레인마다 PHY 전원을 넣는다. 초기화(phy_init)와 전원(phy_power_on)이
	 * 나뉘어 있고, 그 사이에 클라이언트 설정이 들어가는 순서다. */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		/* [한국어] 전원 인가. */
		err = phy_power_on(rockchip->phys[i]);
		/* [한국어] 실패하면, */
		if (err) {
			/* [한국어] 몇 번 레인인지 남기고, */
			dev_err(dev, "power on phy%d err %d\n", i, err);
			/* [한국어] 전원까지 되돌리는 라벨로 간다. */
			goto err_power_off_phy;
		}
	}

	/* [한국어] PHY PLL 이 잠길 때까지 폴링한다. 위에서 정의한 매크로가 인자 하나짜리
	 * 읽기 함수 노릇을 한다. */
	err = readx_poll_timeout(rockchip_pcie_read_addr,
				 PCIE_CLIENT_SIDE_BAND_STATUS,
				 /* [한국어] 상태 비트가 **내려가면** 잠긴 것이다. 부정 논리라 조건에 ! 가 붙는다. */
				 regs, !(regs & PCIE_CLIENT_PHY_ST),
				 /* [한국어] 1ms 간격으로, */
				 RK_PHY_PLL_LOCK_SLEEP_US,
				 /* [한국어] 최대 100ms 기다린다. */
				 RK_PHY_PLL_LOCK_TIMEOUT_US);
	/* [한국어] 시간 안에 잠기지 않으면, */
	if (err) {
		/* [한국어] 기록하고, */
		dev_err(dev, "PHY PLLs could not lock, %d\n", err);
		goto err_power_off_phy;
	}

	/* [한국어] 마지막으로 Core 리셋을 푼다. 이 시점이면 PHY 가 준비되어 있어
	 * 컨트롤러 코어가 동작을 시작해도 된다. */
	err = reset_control_bulk_deassert(ROCKCHIP_NUM_CORE_RSTS,
					  rockchip->core_rsts);
	/* [한국어] 실패하면, */
	if (err) {
		/* [한국어] 기록하고, */
		dev_err(dev, "Couldn't deassert Core reset %d\n", err);
		goto err_power_off_phy;
	}

	/* [한국어] 모든 단계가 성공했다. */
	return 0;
/* [한국어] 전원까지 켠 뒤 실패한 경로. */
err_power_off_phy:
	/* [한국어] i 를 하나씩 줄이며 켠 것만 끈다. 후위 감소라 실패한 인덱스는 건드리지
	 * 않고 그 앞까지만 되돌린다. */
	while (i--)
		phy_power_off(rockchip->phys[i]);
	/* [한국어] 전원 되감기가 끝났으니 초기화 되감기를 위해 i 를 최대로 되돌린다.
	 * 전원까지 갔다는 것은 모든 레인의 phy_init 이 끝났다는 뜻이기 때문이다.
	 * 두 라벨이 이어져 있는 폭포 구조의 연결 고리다. */
	i = MAX_LANE_NUM;
/* [한국어] 초기화만 한 뒤 실패한 경로. 위에서 흘러내려 오기도 하고, phy_init 실패
 * 시 직접 뛰어오기도 한다. */
err_exit_phy:
	/* [한국어] i 를 줄이며, */
	while (i--)
		/* [한국어] 초기화한 것만 되돌린다. */
		phy_exit(rockchip->phys[i]);
	/* [한국어] 원래 오류를 올려보낸다. */
	return err;
}
/* [한국어] 두 드라이버가 공유하므로 내보낸다. */
EXPORT_SYMBOL_GPL(rockchip_pcie_init_port);

/* [한국어]
 * rockchip_pcie_get_phys - 레인별 PHY 를 얻는다. 옛 단일 PHY 방식도 받아 준다
 *
 * @rockchip: 이 컨트롤러의 상태.
 * @return: 0 = 성공, -ENOMEM, -EPROBE_DEFER, 또는 PHY 조회 오류.
 *
 * DT 바인딩이 바뀐 흔적이 그대로 남아 있는 함수다. 옛 바인딩은 "pcie-phy"
 * 하나로 전체를 표현했고, 새 바인딩은 "pcie-phy-0" 부터 레인마다 하나씩 둔다.
 *
 * 옛 방식을 먼저 시도하고, 있으면 legacy_phy 를 표시한 뒤 0번 자리에만 넣고
 * 곧장 반환한다. 경고 로그를 남기는 것은 DT 를 갱신하라는 신호다.
 *
 * -EPROBE_DEFER 처리가 이 함수의 요점이다. 옛 방식 조회가 지연으로 실패하면
 * 레인별 탐색을 시도하지 않고 그대로 반환한다. 지연은 "없다" 가 아니라
 * "아직" 이므로, 그 상태에서 새 방식으로 넘어가면 잘못된 결론에 이른다.
 *
 * 레인별 탐색에서는 실패 로그도 지연 여부로 가른다. 진짜 오류일 때만 남기고
 * 지연은 조용히 넘어가, 부팅 로그가 정상적인 재시도로 채워지지 않게 한다.
 *
 * 이름 버퍼를 조회 직후 해제하는 것은 devm_of_phy_get() 이 이름을 복사해
 * 가기 때문이다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 레인 하나라도 얻지 못하면 전체 실패다. 이미 얻은 것은 devm_ 이라
 * 되돌릴 필요가 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_parse_dt() → [이 함수]
 *     → devm_phy_get("pcie-phy") → kasprintf() → devm_of_phy_get()
 */
int rockchip_pcie_get_phys(struct rockchip_pcie *rockchip)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = rockchip->dev;
	/* [한국어] 얻어 낼 PHY. */
	struct phy *phy;
	/* [한국어] 레인별 PHY 이름을 만들 버퍼. */
	char *name;
	/* [한국어] 레인 인덱스. */
	u32 i;

	/* [한국어] 먼저 옛 방식인 "pcie-phy" 하나를 시도한다. */
	phy = devm_phy_get(dev, "pcie-phy");
	/* [한국어] 있으면, */
	if (!IS_ERR(phy)) {
		/* [한국어] 옛 방식임을 기록해 둔다. 이후 코드가 레인별 처리를 건너뛰는 근거가 된다. */
		rockchip->legacy_phy = true;
		/* [한국어] 0번 자리에만 넣는다. 나머지는 NULL 로 남는다. */
		rockchip->phys[0] = phy;
		/* [한국어] 더 이상 권장하지 않는 방식임을 알린다. DT 를 갱신하라는 신호다. */
		dev_warn(dev, "legacy phy model is deprecated!\n");
		/* [한국어] 성공. */
		return 0;
	}

	/* [한국어] PHY 드라이버가 아직 준비되지 않은 것이면, */
	if (PTR_ERR(phy) == -EPROBE_DEFER)
		/* [한국어] 그 코드를 그대로 올려보내 나중에 다시 시도되게 한다. 아래 레인별 탐색을
		 * 시도하지 않는 것이 중요하다 — 지연은 "없다" 가 아니라 "아직" 이기
		 * 때문이다. */
		return PTR_ERR(phy);

	/* [한국어] 옛 방식이 없으므로 레인별 탐색으로 넘어간다는 사실을 디버그 로그에 남긴다. */
	dev_dbg(dev, "missing legacy phy; search for per-lane PHY\n");

	/* [한국어] 레인마다, */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		/* [한국어] "pcie-phy-0" 같은 이름을 만든다. */
		name = kasprintf(GFP_KERNEL, "pcie-phy-%u", i);
		/* [한국어] 할당 실패면, */
		if (!name)
			return -ENOMEM;

		/* [한국어] 그 이름으로 PHY 를 얻는다. */
		phy = devm_of_phy_get(dev, dev->of_node, name);
		/* [한국어] 이름 버퍼는 바로 놓는다. 조회가 이름을 복사해 가기 때문이다. */
		kfree(name);

		/* [한국어] 실패하면, */
		if (IS_ERR(phy)) {
			/* [한국어] probe 지연이 아닌 진짜 오류일 때만, */
			if (PTR_ERR(phy) != -EPROBE_DEFER)
				/* [한국어] 어느 레인이었는지 남긴다. 지연은 흔한 정상 상황이라 로그를 남기지 않는다. */
				dev_err(dev, "missing phy for lane %d: %ld\n",
					i, PTR_ERR(phy));
			/* [한국어] 어느 쪽이든 그 코드를 올려보낸다. 레인 하나라도 없으면 전체 실패다. */
			return PTR_ERR(phy);
		}

		/* [한국어] 확보한 PHY 를 배열에 넣는다. */
		rockchip->phys[i] = phy;
	}

	/* [한국어] 모든 레인의 PHY 를 얻었다. */
	return 0;
}
/* [한국어] 두 드라이버가 공유한다. */
EXPORT_SYMBOL_GPL(rockchip_pcie_get_phys);

/* [한국어]
 * rockchip_pcie_deinit_phys - PHY 전원을 끄고 초기화를 되돌린다
 *
 * @rockchip: 이 컨트롤러의 상태.
 *
 * rockchip_pcie_init_port() 의 PHY 부분을 되돌린다. 두 단계를 다르게 다루는
 * 것이 특징이다.
 *
 * 전원 차단은 lanes_map 에 표시된 레인만 한다. 함수 안의 영어 주석대로
 * 쓰지 않는 레인은 애초에 켜지 않았기 때문이다.
 * 반면 초기화 해제는 모든 레인에 대해 한다. init_port 가 lanes_map 과
 * 무관하게 MAX_LANE_NUM 만큼 phy_init 을 했으므로 그렇게 해야 짝이 맞는다.
 *
 * 즉 두 반복의 범위가 다른 것이 실수가 아니라, 앞선 함수의 비대칭을
 * 그대로 되짚는 것이다.
 *
 * 실행 컨텍스트: remove 와 절전 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없고 각 PHY 호출의 결과도 보지 않는다.
 *
 * 호출 체인:
 *   host/ep 드라이버의 remove·suspend → [이 함수]
 *     → phy_power_off()(활성 레인만) → phy_exit()(전 레인)
 */
void rockchip_pcie_deinit_phys(struct rockchip_pcie *rockchip)
{
	/* [한국어] 레인 인덱스. */
	int i;

	/* [한국어] 레인마다, */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		/* inactive lanes are already powered off */
		/* [한국어] 옆의 영어 주석대로, 쓰지 않는 레인은 애초에 전원을 켜지 않았으므로
		 * lanes_map 에 표시된 레인만 끈다. */
		if (rockchip->lanes_map & BIT(i))
			/* [한국어] 전원 차단. */
			phy_power_off(rockchip->phys[i]);
		/* [한국어] 초기화 해제는 모든 레인에 대해 한다. init_port 가 lanes_map 과 무관하게
		 * MAX_LANE_NUM 만큼 phy_init 을 했기 때문에 짝이 맞는다. */
		phy_exit(rockchip->phys[i]);
	}
}
/* [한국어] 두 드라이버가 공유한다. */
EXPORT_SYMBOL_GPL(rockchip_pcie_deinit_phys);

/* [한국어]
 * rockchip_pcie_enable_clocks - parse_dt 가 모아 둔 클록을 한꺼번에 켠다
 *
 * @rockchip: 이 컨트롤러의 상태.
 * @return: 0 = 성공, 그 밖에 clk_bulk_prepare_enable() 의 오류.
 *
 * 한 줄짜리 래퍼지만 두 드라이버가 공유하므로 여기 있다.
 *
 * 개수를 parse_dt 가 세어 두었기 때문에 이 함수는 그것을 그대로 쓴다.
 * 클록을 이름 없이 통째로 받는 설계 덕분에, SoC 마다 클록 구성이 달라도
 * 이 함수가 바뀌지 않는다.
 *
 * dev_err_probe() 로 보고하는 것은 클록 드라이버가 아직 준비되지 않아
 * -EPROBE_DEFER 가 올 수 있기 때문이다.
 *
 * 실행 컨텍스트: probe 와 재개 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   host/ep 드라이버의 probe·resume → [이 함수] → clk_bulk_prepare_enable()
 */
int rockchip_pcie_enable_clocks(struct rockchip_pcie *rockchip)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = rockchip->dev;
	/* [한국어] 결과. */
	int err;

	/* [한국어] parse_dt 가 세어 둔 개수만큼 한꺼번에 켠다. */
	err = clk_bulk_prepare_enable(rockchip->num_clks, rockchip->clks);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] probe 지연을 구분해 보고한다. */
		return dev_err_probe(dev, err, "failed to enable clocks\n");

	/* [한국어] 성공. */
	return 0;
}
/* [한국어] 두 드라이버가 공유한다. */
EXPORT_SYMBOL_GPL(rockchip_pcie_enable_clocks);

/* [한국어]
 * rockchip_pcie_disable_clocks - 켜 두었던 클록을 한꺼번에 끈다
 *
 * @rockchip: 이 컨트롤러의 상태.
 *
 * rockchip_pcie_enable_clocks() 의 짝이다. 반환값이 없는 것이 켜기 쪽과
 * 다른데, 해제는 실패를 알려도 호출자가 할 수 있는 일이 없기 때문이다.
 *
 * 실행 컨텍스트: remove 와 절전 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   host/ep 드라이버의 remove·suspend → [이 함수]
 *     → clk_bulk_disable_unprepare()
 */
void rockchip_pcie_disable_clocks(struct rockchip_pcie *rockchip)
{

	/* [한국어] 한꺼번에 끈다. 켜기와 달리 실패를 보고할 방법이 없어 반환값이 없다. */
	clk_bulk_disable_unprepare(rockchip->num_clks, rockchip->clks);
}
/* [한국어] 두 드라이버가 공유한다. */
EXPORT_SYMBOL_GPL(rockchip_pcie_disable_clocks);

/* [한국어]
 * rockchip_pcie_cfg_configuration_accesses - outbound region 0 을 config 접근용으로 설정한다
 *
 * @rockchip: 이 컨트롤러의 상태.
 * @type: 이 region 이 낼 트랜잭션 종류(Type 0 인지 Type 1 인지).
 *
 * PCIe config 접근에는 두 종류가 있다. 바로 아래 버스의 장치에는 Type 0,
 * 더 깊은 버스에는 Type 1 을 쓰며 브리지가 그것을 변환한다. 이 컨트롤러는
 * 그 구분을 outbound region 의 디스크립터 타입 필드로 표현하므로, 접근
 * 대상이 바뀔 때마다 이 함수로 갈아 끼운다.
 *
 * 네 단계다. BAR 설정을 지우고, region 0 의 주소 상·하위 워드를 쓰고,
 * 디스크립터의 타입 필드만 읽기-수정-쓰기로 갈아 끼운 뒤, DESC1 을 0 으로
 * 지운다.
 *
 * 주소 하위 워드에 통과 비트 수를 더하는 것이 이 하드웨어의 관용구다.
 * 그 값이 "변환하지 않고 그대로 내보낼 하위 주소 비트 개수" 를 뜻해,
 * region 하나가 덮는 크기를 정한다.
 *
 * 디스크립터에 함께 넣는 23번 비트의 의미는 벤더 문서에만 있어 이 트리에서
 * 확인할 수 없다.
 *
 * 실행 컨텍스트: config 접근 경로. 호출자가 잠금을 관리한다.
 *
 * 에러 경로: 없다. 반환값이 없고 쓰기의 성공 여부를 확인할 방법도 없다.
 *
 * 호출 체인:
 *   pcie-rockchip-host.c 의 config 읽기·쓰기 → [이 함수]
 *     → rockchip_pcie_write() ×4 → rockchip_pcie_read() ×1
 */
void rockchip_pcie_cfg_configuration_accesses(
		struct rockchip_pcie *rockchip, u32 type)
{
	/* [한국어] 읽고 고칠 디스크립터 값. */
	u32 ob_desc_0;

	/* Configuration Accesses for region 0 */
	/* [한국어] 옆의 영어 주석대로 region 0 을 config 접근용으로 쓴다. BAR 설정을 0 으로
	 * 지우는 것이 그 준비다. */
	rockchip_pcie_write(rockchip, 0x0, PCIE_RC_BAR_CONF);

	/* [한국어] outbound region 0 의 주소 하위 워드를 쓴다. 변환 주소와 통과 비트 수를
	 * 더한 값인데, 통과 비트가 변환하지 않고 그대로 내보낼 하위 주소 비트
	 * 개수를 뜻한다. */
	rockchip_pcie_write(rockchip,
			    (RC_REGION_0_ADDR_TRANS_L + RC_REGION_0_PASS_BITS),
			    PCIE_CORE_OB_REGION_ADDR0);
	/* [한국어] 주소 상위 워드. */
	rockchip_pcie_write(rockchip, RC_REGION_0_ADDR_TRANS_H,
			    PCIE_CORE_OB_REGION_ADDR1);

	/* [한국어] 디스크립터를 읽는다. 타입 필드만 고치고 나머지를 보존해야 하므로
	 * 읽기-수정-쓰기다. */
	ob_desc_0 = rockchip_pcie_read(rockchip, PCIE_CORE_OB_REGION_DESC0);
	/* [한국어] 타입 필드를 지운다. */
	ob_desc_0 &= ~(RC_REGION_0_TYPE_MASK);
	/* [한국어] 새 타입과 23번 비트를 넣는다. 23번 비트의 의미는 벤더 문서에만 있어
	 * 이 트리에서 확인할 수 없다. */
	ob_desc_0 |= (type | (0x1 << 23));
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, ob_desc_0, PCIE_CORE_OB_REGION_DESC0);
	/* [한국어] DESC1(오프셋 0xc)을 0 으로 지운다. 바로 위에서 지운 ADDR1(0x4)과는
	 * 다른 레지스터다 — outbound region 하나를 무효화하려면 주소 레지스터와
	 * 디스크립터 레지스터를 모두 지워야 한다. */
	rockchip_pcie_write(rockchip, 0x0, PCIE_CORE_OB_REGION_DESC1);
}
EXPORT_SYMBOL_GPL(rockchip_pcie_cfg_configuration_accesses);
