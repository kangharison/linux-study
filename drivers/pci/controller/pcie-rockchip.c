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

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "../pci.h"
#include "pcie-rockchip.h"

int rockchip_pcie_parse_dt(struct rockchip_pcie *rockchip)
{
	struct device *dev = rockchip->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *node = dev->of_node;
	struct resource *regs;
	int err, i;

	/* PCI/NVMe: RC 모드인 경우에만 axi-base 레지스터를 CFG/MMIO 용도로 매핑;
	 * NVMe PCIe host driver(drivers/nvme/host/pci.c)가 이 매핑을 통해
	 * 열거된 NVMe 장치의 BAR/CFG 공간에 접근할 수 있게 된다. */
	if (rockchip->is_rc) {
		regs = platform_get_resource_byname(pdev,
						    IORESOURCE_MEM,
						    "axi-base");
		rockchip->reg_base = devm_pci_remap_cfg_resource(dev, regs);
		if (IS_ERR(rockchip->reg_base))
			return PTR_ERR(rockchip->reg_base);
	} else {
		/* PCI/NVMe: EP 모드에서는 mem-base 리소스만 필요;
		 * RC에서 동작할 때 NVMe 열거 및 DMA 매핑을 위해 사용되는 메모리
		 * 영역이 여기서 확본된다. */
		rockchip->mem_res =
			platform_get_resource_byname(pdev, IORESOURCE_MEM,
						     "mem-base");
		if (!rockchip->mem_res)
			return -EINVAL;
	}

	/* PCI/NVMe: APB 레지스터 공간 매핑; controller 제어/상태 레지스터이며
	 * link training, 인터럽트 설정, AER, ASPM 등 NVMe 장치와의 PCIe
	 * 통신을 위한 물리/링크 계층 상태를 조회/설정하는 데 쓰인다. */
	rockchip->apb_base =
		devm_platform_ioremap_resource_byname(pdev, "apb-base");
	if (IS_ERR(rockchip->apb_base))
		return PTR_ERR(rockchip->apb_base);

	/* PCI/NVMe: PCIe PHY 획득; PHY 설정이 실패하면 PCIe link up이 되지 않아
	 * NVMe 장치를 pci_scan_root_bus() 단계에서 발견할 수 없게 된다. */
	err = rockchip_pcie_get_phys(rockchip);
	if (err)
		return err;

	/* PCI/NVMe: 기본 lane 수 1로 설정; NVMe SSD와의 link width 협상에
	 * 영향을 주며, 링크 폭이 줄어들면 NVMe 성능/대역폭이 직접적으로 감소. */
	rockchip->lanes = 1;
	err = of_property_read_u32(node, "num-lanes", &rockchip->lanes);
	if (!err && (rockchip->lanes == 0 ||
		     rockchip->lanes == 3 ||
		     rockchip->lanes > 4)) {
		dev_warn(dev, "invalid num-lanes, default to use one lane\n");
		rockchip->lanes = 1;
	}

	/* PCI/NVMe: PCIe link speed(1.x/2.x) 설정; NVMe SSD의 전송률과
	 * ASPM/전력 관리 정책에 영향을 준다. */
	rockchip->link_gen = of_pci_get_max_link_speed(node);
	if (rockchip->link_gen < 0 || rockchip->link_gen > 2)
		rockchip->link_gen = 2;

	/* PCI/NVMe: PM reset handle 배열 초기화; PCI host controller 전원/저장
	 * 상태 전환 시 NVMe 장치의 PCIe link 안정성과 관련된 reset 제어. */
	for (i = 0; i < ROCKCHIP_NUM_PM_RSTS; i++)
		rockchip->pm_rsts[i].id = rockchip_pci_pm_rsts[i];

	/* PCI/NVMe: PM reset exclusive 획득; devm이므로 probe 실패/드라이버
	 * 제거 시 자동 해제되며, NVMe probe 도중 controller가 안전하게 reset
	 * 상태를 유지할 수 있도록 한다. */
	err = devm_reset_control_bulk_get_exclusive(dev,
						    ROCKCHIP_NUM_PM_RSTS,
						    rockchip->pm_rsts);
	if (err)
		return dev_err_probe(dev, err, "Cannot get the PM reset\n");

	/* PCI/NVMe: Core reset handle 배열 초기화; controller 코어 로직을
	 * 초기화하여 NVMe 장치를 위한 PCIe config/MMIO 엑세스 경로를 활성화. */
	for (i = 0; i < ROCKCHIP_NUM_CORE_RSTS; i++)
		rockchip->core_rsts[i].id = rockchip_pci_core_rsts[i];

	/* PCI/NVMe: Core reset exclusive 획득; PCIe host controller 코어를
	 * reset/정복하여 NVMe SSD가 pci_scan_root_bus()에서 정상 인식되도록 함. */
	err = devm_reset_control_bulk_get_exclusive(dev,
						    ROCKCHIP_NUM_CORE_RSTS,
						    rockchip->core_rsts);
	if (err)
		return dev_err_probe(dev, err, "Cannot get the Core resets\n");

	/* PCI/NVMe: PERST# GPIO 획득; RC 모드에서는 EP를 리셋, EP 모드에서는
	 * reset 입력. PERST#는 NVMe SSD의 fundamental reset으로, NVMe controller
	 * 초기화/재열거(hotplug 포함)에 필수적이다. */
	if (rockchip->is_rc)
		rockchip->perst_gpio = devm_gpiod_get_optional(dev, "ep",
							       GPIOD_OUT_LOW);
	else
		rockchip->perst_gpio = devm_gpiod_get_optional(dev, "reset",
							       GPIOD_IN);
	if (IS_ERR(rockchip->perst_gpio))
		return dev_err_probe(dev, PTR_ERR(rockchip->perst_gpio),
				     "failed to get PERST# GPIO\n");

	/* PCI/NVMe: PCIe controller 클럭 전체 획득 및 활성화 준비;
	 * 클럭이 없으면 PCIe link 자체가 동작하지 않아 NVMe 장치를 전혀
	 * 탐지할 수 없다. */
	rockchip->num_clks = devm_clk_bulk_get_all(dev, &rockchip->clks);
	if (rockchip->num_clks < 0)
		return dev_err_probe(dev, rockchip->num_clks,
				     "failed to get clocks\n");

	return 0;
}
EXPORT_SYMBOL_GPL(rockchip_pcie_parse_dt);

/* PCI/NVMe: read_addr 래퍼 매크로; controller 레지스터를 폴링하며
 * NVMe SSD 연결을 위한 PHY PLL lock, link up 등 상태 확인에 사용. */
#define rockchip_pcie_read_addr(addr) rockchip_pcie_read(rockchip, addr)
/* 100 ms max wait time for PHY PLLs to lock */
#define RK_PHY_PLL_LOCK_TIMEOUT_US 100000
/* Sleep should be less than 20ms */
#define RK_PHY_PLL_LOCK_SLEEP_US 1000

int rockchip_pcie_init_port(struct rockchip_pcie *rockchip)
{
	struct device *dev = rockchip->dev;
	int err, i;
	u32 regs;

	/* PCI/NVMe: PM reset assert; controller의 전원 관리 도메인을
	 * reset하여 이전 NVMe 장치 상태나 AER 에러 상태가 남아있지 않도록
	 * 깨끗이 초기화한다. */
	err = reset_control_bulk_assert(ROCKCHIP_NUM_PM_RSTS,
					rockchip->pm_rsts);
	if (err)
		return dev_err_probe(dev, err, "Couldn't assert PM resets\n");

	/* PCI/NVMe: 물리 lane 수만큼 PHY 초기화; lane별 PHY가 준비되지 않으면
	 * PCIe link training 실패로 NVMe 장치를 열거할 수 없다. */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		err = phy_init(rockchip->phys[i]);
		if (err) {
			dev_err(dev, "init phy%d err %d\n", i, err);
			goto err_exit_phy;
		}
	}

	/* PCI/NVMe: Core reset assert; controller 코어 로직을 reset하여
	 * NVMe PCIe host와의 link training이 깨끗한 상태에서 시작되도록 함. */
	err = reset_control_bulk_assert(ROCKCHIP_NUM_CORE_RSTS,
					rockchip->core_rsts);
	if (err) {
		dev_err_probe(dev, err, "Couldn't assert Core resets\n");
		goto err_exit_phy;
	}

	/* PCI/NVMe: reset이 안정화될 시간을 확보; 짧은 딜레이 후
	 * deassert를 진행하여 NVMe SSD 측과의 동기화 조건을 만든다. */
	udelay(10);

	/* PCI/NVMe: PM reset deassert; 전원 관리 도메인 정상 동작 시작,
	 * 이후 PCIe config space 접근이 가능해지면 NVMe BAR 매핑 준비 완료. */
	err = reset_control_bulk_deassert(ROCKCHIP_NUM_PM_RSTS,
					  rockchip->pm_rsts);
	if (err) {
		dev_err(dev, "Couldn't deassert PM resets %d\n", err);
		goto err_exit_phy;
	}

	/* PCI/NVMe: PCIe link generation(Gen1/Gen2) 선택; NVMe SSD가
	 * 협상한 link speed에 따라 transfer rate가 결정되며, Gen2로 설정하면
	 * NVMe 대역폭이 증가한다. */
	if (rockchip->link_gen == 2)
		rockchip_pcie_write(rockchip, PCIE_CLIENT_GEN_SEL_2,
				    PCIE_CLIENT_CONFIG);
	else
		rockchip_pcie_write(rockchip, PCIE_CLIENT_GEN_SEL_1,
				    PCIE_CLIENT_CONFIG);

	/* PCI/NVMe: ARI enable 및 lane 수 설정; ARI는 NVMe 장치가
	 * 단일 function에서 더 많은 capabilities/function을 노출할 수 있게
	 * 하며, lane 수는 NVMe link width에 직접 영향. */
	regs = PCIE_CLIENT_ARI_ENABLE |
	       PCIE_CLIENT_CONF_LANE_NUM(rockchip->lanes);

	/* PCI/NVMe: RC/EP 모드에 따른 controller 동작 모드 설정;
	 * RC 모드에서만 pci_scan_root_bus()를 통해 NVMe 장치를 탐색/바인딩
	 * 할 수 있다. */
	if (rockchip->is_rc)
		regs |= PCIE_CLIENT_LINK_TRAIN_ENABLE |
			PCIE_CLIENT_CONF_ENABLE | PCIE_CLIENT_MODE_RC;
	else
		regs |= PCIE_CLIENT_CONF_DISABLE | PCIE_CLIENT_MODE_EP;

	rockchip_pcie_write(rockchip, regs, PCIE_CLIENT_CONFIG);

	/* PCI/NVMe: 물리 lane별 PHY power on; PHY가 활성화되어야 PCIe
	 * link training이 시작되고, NVMe SSD가 L0 상태로 진입할 수 있다. */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		err = phy_power_on(rockchip->phys[i]);
		if (err) {
			dev_err(dev, "power on phy%d err %d\n", i, err);
			goto err_power_off_phy;
		}
	}

	/* PCI/NVMe: PHY PLL lock 폴링; PHY 클록이 안정되어야 PCIe link
	 * training이 성공하며, NVMe 장치가 pci_scan_root_bus()에서 검출된다.
	 * timeout 시 NVMe SSD 미인식/링크 다운 상태로 이어진다. */
	err = readx_poll_timeout(rockchip_pcie_read_addr,
				 PCIE_CLIENT_SIDE_BAND_STATUS,
				 regs, !(regs & PCIE_CLIENT_PHY_ST),
				 RK_PHY_PLL_LOCK_SLEEP_US,
				 RK_PHY_PLL_LOCK_TIMEOUT_US);
	if (err) {
		dev_err(dev, "PHY PLLs could not lock, %d\n", err);
		goto err_power_off_phy;
	}

	/* PCI/NVMe: Core reset deassert; controller 코어가 동작 시작하면
	 * PCIe config cycle이 활성화되고 NVMe host driver의 probe가 진행된다. */
	err = reset_control_bulk_deassert(ROCKCHIP_NUM_CORE_RSTS,
					  rockchip->core_rsts);
	if (err) {
		dev_err(dev, "Couldn't deassert Core reset %d\n", err);
		goto err_power_off_phy;
	}

	return 0;
err_power_off_phy:
	/* PCI/NVMe: 초기화 실패 시 활성화된 PHY 전원 차례로 off;
	 * NVMe 장치가 연결된 lane의 전력을 안전하게 제거해 hotplug/재시도
	 * 시 손상을 방지한다. */
	while (i--)
		phy_power_off(rockchip->phys[i]);
	i = MAX_LANE_NUM;
err_exit_phy:
	/* PCI/NVMe: 초기화 실패 시 PHY exit 정리; PCIe controller 자원을
	 * 정리하여 NVMe 재탐색/드라이버 재로드 시 깨끗한 상태를 유지. */
	while (i--)
		phy_exit(rockchip->phys[i]);
	return err;
}
EXPORT_SYMBOL_GPL(rockchip_pcie_init_port);

int rockchip_pcie_get_phys(struct rockchip_pcie *rockchip)
{
	struct device *dev = rockchip->dev;
	struct phy *phy;
	char *name;
	u32 i;

	/* PCI/NVMe: legacy "pcie-phy" 속성으로 PHY 획득 시도;
	 * 이전 dts 호환용이며, NVMe SSD 연동을 위한 controller 물리계층
	 * 초기화의 시작점이다. */
	phy = devm_phy_get(dev, "pcie-phy");
	if (!IS_ERR(phy)) {
		rockchip->legacy_phy = true;
		rockchip->phys[0] = phy;
		dev_warn(dev, "legacy phy model is deprecated!\n");
		return 0;
	}

	/* PCI/NVMe: EPROBE_DEFER면 아직 dependency가 준비되지 않은 상태;
	 * NVMe PCIe host driver의 probe도 함께 defer되어 나중에 재시도된다. */
	if (PTR_ERR(phy) == -EPROBE_DEFER)
		return PTR_ERR(phy);

	/* PCI/NVMe: legacy PHY가 없으면 per-lane PHY를 순회하며 획득;
	 * 각 lane은 NVMe SSD와의 PCIe link width 구성 요소다. */
	dev_dbg(dev, "missing legacy phy; search for per-lane PHY\n");

	/* PCI/NVMe: 최대 lane 수만큼 per-lane PHY 획득 루프;
	 * 모든 lane의 PHY가 준비되어야 NVMe link negotiation이 정상 동작. */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		name = kasprintf(GFP_KERNEL, "pcie-phy-%u", i);
		if (!name)
			return -ENOMEM;

		/* PCI/NVMe: dts의 per-lane PHY 노드를 연결; lane별 PHY 획득
		 * 실패 시 해당 lane을 통해 NVMe 데이터가 흐를 수 없게 된다. */
		phy = devm_of_phy_get(dev, dev->of_node, name);
		kfree(name);

		if (IS_ERR(phy)) {
			if (PTR_ERR(phy) != -EPROBE_DEFER)
				dev_err(dev, "missing phy for lane %d: %ld\n",
					i, PTR_ERR(phy));
			return PTR_ERR(phy);
		}

		rockchip->phys[i] = phy;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rockchip_pcie_get_phys);

void rockchip_pcie_deinit_phys(struct rockchip_pcie *rockchip)
{
	int i;

	/* PCI/NVMe: 활성 lane에 대해 PHY power off/exit;
	 * lanes_map 비트가 설정된 lane은 NVMe 장치와 실제로 link up되었던
	 * lane이며, 안전하게 전원을 제거해 hotplug/removal 처리에 대비. */
	for (i = 0; i < MAX_LANE_NUM; i++) {
		/* inactive lanes are already powered off */
		if (rockchip->lanes_map & BIT(i))
			phy_power_off(rockchip->phys[i]);
		phy_exit(rockchip->phys[i]);
	}
}
EXPORT_SYMBOL_GPL(rockchip_pcie_deinit_phys);

int rockchip_pcie_enable_clocks(struct rockchip_pcie *rockchip)
{
	struct device *dev = rockchip->dev;
	int err;

	/* PCI/NVMe: PCIe controller에 필요한 모든 클럭 enable;
	 * 클럭이 꺼진 상태에서는 config cycle/MMIO/DMA/MSI-X 등 NVMe
	 * host driver가 의존하는 모든 PCIe 경로가 정지한다. */
	err = clk_bulk_prepare_enable(rockchip->num_clks, rockchip->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to enable clocks\n");

	return 0;
}
EXPORT_SYMBOL_GPL(rockchip_pcie_enable_clocks);

void rockchip_pcie_disable_clocks(struct rockchip_pcie *rockchip)
{

	/* PCI/NVMe: PCIe controller 클럭 전체 disable/unprepare;
	 * ASPM/저장 상태 전환이나 드라이버 제거 시 호출되며, NVMe 장치의
	 * PCIe link를 안전하게 정지시킨다. */
	clk_bulk_disable_unprepare(rockchip->num_clks, rockchip->clks);
}
EXPORT_SYMBOL_GPL(rockchip_pcie_disable_clocks);

void rockchip_pcie_cfg_configuration_accesses(
		struct rockchip_pcie *rockchip, u32 type)
{
	u32 ob_desc_0;

	/* PCI/NVMe: outbound region 0을 PCIe configuration access용으로 설정;
	 * pci_bus_read_config/pci_bus_write_config 등 NVMe host driver가
	 * NVMe 장치의 PCI configuration space에 접근할 때 이 경로를 사용. */
	/* Configuration Accesses for region 0 */
	rockchip_pcie_write(rockchip, 0x0, PCIE_RC_BAR_CONF);

	/* PCI/NVMe: outbound region 0 주소 변환 하위/상위 설정;
	 * CPU 물리 주소를 PCIe bus 주소로 변환하여 NVMe 장치의
	 * configuration space를 엑세스할 수 있게 한다. */
	rockchip_pcie_write(rockchip,
			    (RC_REGION_0_ADDR_TRANS_L + RC_REGION_0_PASS_BITS),
			    PCIE_CORE_OB_REGION_ADDR0);
	rockchip_pcie_write(rockchip, RC_REGION_0_ADDR_TRANS_H,
			    PCIE_CORE_OB_REGION_ADDR1);

	/* PCI/NVMe: outbound descriptor에 transaction type(Type 0/1 config)
	 * 설정; pci_scan_root_bus()가 NVMe bridge/device를 발견/구성할 때
	 * Type 0/1 config cycle 구분을 위해 사용된다. */
	ob_desc_0 = rockchip_pcie_read(rockchip, PCIE_CORE_OB_REGION_DESC0);
	ob_desc_0 &= ~(RC_REGION_0_TYPE_MASK);
	ob_desc_0 |= (type | (0x1 << 23));
	rockchip_pcie_write(rockchip, ob_desc_0, PCIE_CORE_OB_REGION_DESC0);
	/* [한국어] DESC1(오프셋 0xc)을 0 으로 지운다. 바로 위에서 지운 ADDR1(0x4)과는
	 * 다른 레지스터다 — outbound region 하나를 무효화하려면 주소 레지스터와
	 * 디스크립터 레지스터를 모두 지워야 한다. */
	rockchip_pcie_write(rockchip, 0x0, PCIE_CORE_OB_REGION_DESC1);
}
EXPORT_SYMBOL_GPL(rockchip_pcie_cfg_configuration_accesses);
