// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Texas Instruments Keystone SoCs
 *
 * Copyright (C) 2013-2014 Texas Instruments., Ltd.
 *		https://www.ti.com
 *
 * Author: Murali Karicheri <m-karicheri2@ti.com>
 * Implementation based on pci-exynos.c and pcie-designware.c
 */

/*
 * [한국어 설명] TI Keystone SoC 용 DesignWare 접착 계층 (pci-keystone.c)
 *
 * === 파일의 역할 ===
 * TI 의 Keystone 2 계열(K2HK/K2E/K2L/K2G)과 AM654 SoC 에 들어 있는
 * PCIe 컨트롤러 드라이버다. 컨트롤러의 본체는 Synopsys DesignWare(DWC) IP
 * 이므로 이 파일은 **접착 계층(glue)** 이다 — 링크 훈련, config 접근,
 * ATU 프로그래밍 같은 공통 동작은 dwc/ 의 공용 코드가 하고, 이 파일은
 * TI 가 그 IP 를 SoC 에 붙이면서 덧붙인 "애플리케이션 레지스터" 창을
 * 다루며 DWC 코어에 콜백 세 벌을 제공한다.
 *
 *   struct dw_pcie_ops       (ks_pcie_dw_pcie_ops)
 *       start_link / stop_link / link_up / write_dbi2 — IP 를 몰고 상태를 읽는다.
 *   struct dw_pcie_host_ops  (ks_pcie_host_ops, ks_pcie_am654_host_ops)
 *       init / msi_init — RC 모드의 초기화.
 *   struct dw_pcie_ep_ops    (ks_pcie_am654_ep_ops)
 *       init / raise_irq / get_features — EP 모드의 초기화와 인터럽트 발생.
 *
 * 이 파일에서 가장 특이한 것이 **인터럽트 구조**다. 보통의 DWC 접착 계층은
 * IP 내장 MSI 수신기(iMSI-RX)와 DWC 가 만들어 주는 도메인을 그대로 쓰지만,
 * Keystone 은 MSI 와 INTx 를 **SoC 쪽 애플리케이션 레지스터로 직접** 받는다.
 * 그래서 두 가지를 갈아 끼운다.
 *
 *   MSI  — ks_pcie_msi_host_init() 이 dw_pcie_host_ops.msi_init 으로 등록된다.
 *          DWC 코어는 그 콜백이 있다는 사실만으로 내장 iMSI-RX 를 쓰지
 *          않기로 결정하고(drivers/pci/controller/dwc/pcie-designware-host.c:1524
 *          의 use_imsi_rx 계산), 초기화 때 이 콜백을 부른다(같은 파일 :1546).
 *          이 파일은 그 안에서 pp->msi_irq_chip 을 자기 것으로 바꾼 뒤
 *          dw_pcie_allocate_domains() 만 빌려 쓴다 — 도메인 뼈대는 DWC 것을,
 *          바닥 chip 은 자기 것을 쓰는 절충이다. DWC 기본값은 같은 파일
 *          :947 에서 dw_pci_msi_bottom_irq_chip 으로 세워지고, 그 값이
 *          :556 의 irq_domain_set_info() 에 쓰인다.
 *          벡터 번호가 레지스터에 흩어진 방식이 독특하다 — 상태 레지스터
 *          여덟 개가 있고, MSI0 의 비트 0~3 이 벡터 0/8/16/24 를,
 *          MSI1 의 비트가 1/9/17/25 를 뜻한다(ks_pcie_msi_irq_handler 안의
 *          원문 영어 주석이 그 배치를 밝힌다). 그래서 이 파일의 MSI 코드는
 *          어디서나 reg_offset = irq % 8, bit_pos = irq >> 3 으로 자리를 푼다.
 *
 *   INTx — 아예 자기 IRQ 도메인을 따로 만든다(ks_pcie_config_intx_irq()).
 *          장치 트리의 legacy-interrupt-controller 자식 노드에 걸리며,
 *          INTA~INTD 각각이 SoC 인터럽트 하나씩을 차지해 네 개의 체인
 *          핸들러가 붙는다. 그 irq_chip(ks_pcie_intx_irq_chip)의 ack/mask/
 *          unmask 세 콜백이 **전부 빈 함수**인 점이 눈에 띈다 — 개별 INTx 를
 *          끄고 켜는 수단이 없어 자리만 채워 둔 것으로 보이며, 그 근거가
 *          되는 TI 문서는 이 트리에 없다.
 *
 * RC 와 EP 두 모드를 한 파일에 담고 있고, 갈리는 지점은 of_device_id 의
 * ks_pcie_of_data 다. compatible 문자열 셋이 각각 RC(Keystone 2),
 * RC(AM654), EP(AM654)로 이어지며, ks_pcie_probe() 의 switch (mode) 가
 * dw_pcie_host_init() 과 dw_pcie_ep_init() 중 하나를 고른다.
 * 두 경로가 쓰는 것이 완전히 다르다 — 아래 "RC 경로와 EP 경로" 절 참조.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: 장치 트리에 ti,keystone-pcie / ti,am654-pcie-rc / ti,am654-pcie-ep
 *         -> 플랫폼 버스가 ks_pcie_probe() 를 부른다
 *            -> of_device_get_match_data() 로 모드와 콜백 표와 DWC 버전을 고른다
 *            -> "app" 창(애플리케이션 레지스터)과 "dbics" 창(DWC DBI)을 매핑
 *            -> 오류 IRQ 를 걸고, 레인마다 PHY 를 얻어 켠다
 *            -> pm_runtime 을 켜고 syscon 으로 SoC 의 RC/EP 모드 비트를 쓴다
 *            -> RC 면 dw_pcie_host_init(), EP 면 dw_pcie_ep_init()
 *            -> ks_pcie_enable_error_irq() 로 오류 보고를 연다
 *
 * RC 초기화(DWC 코어가 되부른다):
 *         dw_pcie_host_init() -> pp->ops->init = ks_pcie_host_init()
 *            -> bridge->ops 와 child_ops 를 이 파일 것으로 바꾼다
 *            -> ks_pcie_config_intx_irq() / ks_pcie_config_msi_irq()
 *            -> ks_pcie_stop_link() 로 훈련을 멈춘 뒤
 *            -> ks_pcie_setup_rc_app_regs() 로 outbound 창을 1:1 로 깐다
 *            -> ks_pcie_init_id() 로 syscon 에서 읽은 ID 를 DBI 에 쓴다
 *         그 뒤 DWC 코어가 ks_pcie_start_link() 로 훈련을 시작하고
 *         ks_pcie_link_up() 으로 결과를 확인한다.
 *
 * config 접근: PCI 코어
 *         -> 루트 버스면 ks_pcie_ops.map_bus = dw_pcie_own_conf_map_bus (DWC 공용)
 *         -> 그 아래면 ks_child_pcie_ops.map_bus = ks_pcie_other_map_bus (이 파일)
 *            -> 링크가 서 있는지 먼저 보고(원문 주석이 그 이유를 밝힌다),
 *               CFG_SETUP 레지스터에 대상 BDF 와 Type 0/1 을 써 둔 뒤
 *               va_cfg0_base + where 주소를 돌려준다
 *         AM654 는 child_ops 를 걸지 않는다 — 그 세대는 DWC 표준 경로를 쓴다.
 *
 * 인터럽트: 하위 장치의 MSI -> ks_pcie_msi_irq_handler()(체인)
 *           하위 장치의 INTx -> ks_pcie_intx_irq_handler()(체인)
 *           컨트롤러 오류    -> ks_pcie_err_irq_handler()(공유 IRQ)
 *
 * 실행 컨텍스트: probe 와 host_init 은 프로세스 컨텍스트이고 usleep/mdelay 로
 * 잠들 수 있다. config 접근은 PCI 코어가 스핀락을 쥔 채 부르므로 잠들 수
 * 없다. 세 인터럽트 핸들러는 인터럽트 컨텍스트이고, MSI mask/unmask 는
 * pp->lock 을 raw_spin_lock_irqsave 로 잡는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스와 장치 트리가 진입점이다. RC 모드에서는 PCI 코어가
 *   DWC 를 거쳐 이 파일의 pci_ops 에 닿고, EP 모드에서는 EPC 코어가
 *   DWC EP 계층을 거쳐 이 파일의 raise_irq 에 닿는다.
 * 아래쪽:
 *   dwc/pcie-designware-host.c — dw_pcie_host_init(), dw_pcie_allocate_domains(),
 *     그리고 이 파일의 콜백을 되부르는 쪽.
 *   dwc/pcie-designware-ep.c   — dw_pcie_ep_init(), dw_pcie_ep_init_registers(),
 *     dw_pcie_ep_raise_msi_irq(), dw_pcie_ep_raise_msix_irq().
 *   dwc/pcie-designware.c/.h   — dw_pcie_readl_dbi 계열, dw_pcie_link_up(),
 *     dw_pcie_dbi_ro_wr_en/dis(), DW_PCIE_VER_ 계열 상수, DWC_EPC_COMMON_FEATURES.
 *   syscon/regmap — SoC 의 PCIe 모드 비트(ti,syscon-pcie-mode)와
 *     장치 ID(ti,syscon-pcie-id)를 읽고 쓴다. 둘 다 이 컨트롤러 밖에 있다.
 *   phy — 레인마다 하나씩, reset/init/power_on 순서로 켠다.
 *   "../../pci.h" — PCI 코어 내부 헬퍼.
 * 공유 상태: struct keystone_pcie 하나. struct dw_pcie 를 감싸지 않고
 *   포인터로 들고 있으며, 역방향은 to_keystone_pcie() 매크로가
 *   dev_get_drvdata() 로 되찾는다 — 대부분의 DWC 접착 계층이
 *   container_of 를 쓰는 것과 다른 방식이다.
 *
 * === 주요 함수/구조체 요약 ===
 * ks_pcie_probe() / ks_pcie_remove() : 플랫폼 드라이버 진입점과 정리.
 * ks_pcie_app_readl() / ks_pcie_app_writel() : 애플리케이션 레지스터 접근.
 *                        이 파일의 거의 모든 하드웨어 조작이 이 둘을 거친다.
 * ks_pcie_set_dbi_mode() / ks_pcie_clear_dbi_mode()
 *                      : DBI_CS2 를 켜고 꺼 BAR 마스크 레지스터를 드러낸다.
 *                        클럭 도메인이 달라 되읽어 확인하는 것이 요점이다.
 * ks_pcie_host_init()  : RC 모드 초기화의 본체(DWC 가 되부른다).
 * ks_pcie_msi_host_init() : MSI 를 자기 방식으로 초기화한다.
 * ks_pcie_setup_rc_app_regs() : outbound 창을 1:1 로 깔고 inbound BAR 를 끈다.
 * ks_pcie_other_map_bus() : 하위 장치 config 접근의 주소를 만든다.
 * ks_pcie_start_link() / ks_pcie_stop_link() / ks_pcie_link_up()
 *                      : dw_pcie_ops 의 링크 제어 삼총사.
 * ks_pcie_config_intx_irq() / ks_pcie_config_msi_irq()
 *                      : 장치 트리의 자식 노드에서 IRQ 를 얻어 체인 핸들러를 건다.
 * ks_pcie_intx_irq_handler() / ks_pcie_msi_irq_handler()
 *                      : 그 체인 핸들러들.
 * ks_pcie_handle_intx_irq() : INTx 하나를 도메인으로 넘기고 EOI 한다.
 * ks_pcie_err_irq_handler() / ks_pcie_handle_error_irq() / ks_pcie_enable_error_irq()
 *                      : 컨트롤러 오류를 종류별로 로그한다.
 * ks_pcie_init_id()    : syscon 에서 읽은 벤더/장치 ID 를 DBI 에 쓴다.
 * ks_pcie_set_mode() / ks_pcie_am654_set_mode()
 *                      : SoC 의 RC/EP 모드 비트를 syscon 으로 쓴다. 세대별로 다르다.
 * ks_pcie_enable_phy() / ks_pcie_disable_phy() : 레인별 PHY 를 켜고 끈다.
 * ks_pcie_quirk()      : MRRS 상한을 하드웨어 한계에 맞춰 낮춘다(PCI fixup).
 * ks_pcie_am654_ep_init() / ks_pcie_am654_raise_irq() / ks_pcie_am654_raise_intx_irq()
 *                      : EP 모드의 초기화와 인터럽트 발생.
 * ks_pcie_fault()      : ARM 전용. 없는 장치를 찔러 생긴 외부 abort 를 삼킨다.
 *
 * struct keystone_pcie : 이 컨트롤러 하나의 상태 전부.
 * struct ks_pcie_of_data : compatible 별 모드/콜백/DWC 버전 묶음.
 *
 * === RC 경로와 EP 경로 ===
 * 한 파일에 두 모드가 있으므로, 어느 함수가 어느 경로에만 쓰이는지가
 * 읽는 데 중요하다.
 *
 *   RC 전용 : ks_pcie_host_init, ks_pcie_msi_host_init,
 *             ks_pcie_setup_rc_app_regs, ks_pcie_other_map_bus,
 *             ks_pcie_config_intx_irq, ks_pcie_config_msi_irq,
 *             ks_pcie_intx_irq_handler, ks_pcie_msi_irq_handler,
 *             ks_pcie_handle_intx_irq, INTx/MSI irq_chip 과 도메인 전부,
 *             ks_pcie_init_id, ks_pcie_quirk.
 *   EP 전용 : ks_pcie_am654_ep_init, ks_pcie_am654_raise_irq,
 *             ks_pcie_am654_raise_intx_irq, ks_pcie_am654_get_features.
 *   양쪽 공용: ks_pcie_app_readl/writel, set/clear_dbi_mode,
 *             start_link/stop_link/link_up, enable/disable_phy,
 *             오류 IRQ 삼총사, ks_pcie_probe/remove.
 *
 * EP 모드는 AM654 에만 있다. Keystone 2 계열의 of_data 에는 ep_ops 가
 * 없고, ks_pcie_probe() 의 EP 갈래는 CONFIG_PCI_KEYSTONE_EP 가 꺼져 있으면
 * -ENODEV 로 물러난다. 마찬가지로 RC 갈래는 CONFIG_PCI_KEYSTONE_HOST 를 본다.
 *
 * 두 세대의 차이도 크다. is_am6 플래그와 dw_pcie_ver_is_ge(pci, 480A)
 * 두 가지로 갈리며, AM654 는 child_ops 를 걸지 않고, outbound 창을 직접
 * 깔지 않으며(ks_pcie_setup_rc_app_regs 가 BAR 만 끄고 곧바로 돌아온다),
 * INTx 와 MSI 의 장치 트리 자식 노드가 없어도 오류로 보지 않는다.
 *
 * === 값의 근거에 대하여 ===
 * 이 파일 위쪽의 레지스터 오프셋과 비트(CMD_STATUS, CFG_SETUP, OB_ 계열,
 * MSI_IRQ_ 계열, IRQ_ 계열, ERR_ 계열, KS_PCIE_DEV_TYPE 계열)는 모두 이
 * 파일 안에 정의되어 있으나, 그 근거가 되는 TI 하드웨어 문서는 이 트리에
 * 없다. 따라서 아래 주석은 값의 의미를 단정하지 않고 **코드가 그 상수를
 * 어떻게 쓰는지**(마스크인지, 세우는 비트인지, RW1C 인지, 폴링 조건인지,
 * 인덱스 계산인지)로 설명한다.
 * PCI_BASE_ADDRESS_ 계열과 PCI_EXP_ 계열 표준 상수는
 * include/linux/pci_regs.h 가, PCI_VENDOR_ID_TI 는 include/linux/pci_ids.h 가
 * 이 스파스 체크아웃에 없어 값을 확인하지 못했다.
 * DWC 쪽 상수(PORT_LOGIC_LTSSM_STATE_MASK, DW_PCIE_VER_ 계열,
 * DWC_EPC_COMMON_FEATURES)는 dwc/pcie-designware.h 에 있고 그 파일에는
 * 이미 한국어 주석이 달려 있어, 여기서는 이 파일이 그것을 어떻게 쓰는지만 적는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 아무 접점이 없다(drivers/nvme 전수 grep 0건).
 * 이 파일은 특정 SoC 의 호스트/엔드포인트 컨트롤러 접착 계층이라, 장치
 * 종류와 무관하게 "PCI 버스를 제공하거나 자기가 장치가 되는" 쪽이기 때문이다.
 *
 * 다만 ks_pcie_quirk() 는 NVMe 를 포함한 모든 하위 장치에 직접 영향을 준다 —
 * Keystone 은 최대 읽기 요청 크기(MRRS)가 256바이트, AM654 PG1.0 은
 * 128바이트로 제한되어 그 위를 넘기면 트랜잭션이 실패한다. 그래서 이
 * fixup 이 모든 하위 장치의 MRRS 를 강제로 낮춘다. NVMe 컨트롤러가 큰
 * 읽기를 쪼개 보내게 되므로 대역폭에 직접 영향이 있고, 이 파일에서 NVMe
 * 독자에게 의미 있는 유일한 대목이다.
 */

/* [한국어] clk_ 계열. 다만 이 파일에서 직접 부르는 클록 함수는 확인되지 않았다 —
 * 클록은 pm_runtime 이 전원 도메인과 함께 다룬다. 상류 그대로 둔다 */
#include <linux/clk.h>
/* [한국어] mdelay. ks_pcie_am654_raise_intx_irq() 의 1밀리초 INTx 펄스에 쓴다 */
#include <linux/delay.h>
/* [한국어] gpiod_ 계열. RC 모드에서 PERST 를 푸는 reset GPIO 를 다룬다 */
#include <linux/gpio/consumer.h>
/* [한국어] __init 과 device_initcall. ARM 갈래의 ks_pcie_init() 을 부팅 초기화로 등록한다 */
#include <linux/init.h>
/* [한국어] irqreturn_t, IRQF_SHARED, devm_request_irq — 오류 IRQ 등록 */
#include <linux/interrupt.h>
/* [한국어] chained_irq_enter/exit. MSI 와 INTx 체인 핸들러가 상위 컨트롤러의
 * 마스킹과 확인응답을 맡기는 데 쓴다 */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain 과 irq_domain_create_linear, generic_handle_domain_irq,
 * irq_domain_xlate_onetwocell — INTx 전용 도메인을 만든다 */
#include <linux/irqdomain.h>
/* [한국어] syscon_regmap_lookup_by_phandle. SoC 의 PCIe 모드 비트와 장치 ID 가
 * 이 컨트롤러 밖 레지스터에 있어 syscon 을 거친다 */
#include <linux/mfd/syscon.h>
/* [한국어] MODULE_ 계열 매크로. 이 파일은 module_platform_driver 를 쓰지 않고
 * 아키텍처별로 device_initcall 또는 builtin_platform_driver 를 쓴다 */
#include <linux/module.h>
/* [한국어] struct msi_msg. ks_pcie_compose_msi_msg() 가 채운다 */
#include <linux/msi.h>
/* [한국어] 장치 트리 조회 API — of_device_get_match_data 로 compatible 별 데이터를
 * 얻고, of_property_read_u32 로 num-lanes 와 num-viewport 를 읽고,
 * of_get_child_by_name 으로 인터럽트 컨트롤러 자식 노드를 찾고,
 * of_device_is_compatible 로 AM654 RC 인지 가리고,
 * of_parse_phandle_with_fixed_args 로 syscon 오프셋 인자를 얻는다 */
#include <linux/of.h>
/* [한국어] of_irq_count 와 irq_of_parse_and_map. 장치 트리 자식 노드에서
 * INTx 넷과 MSI 여덟의 IRQ 를 얻는다 */
#include <linux/of_irq.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다. 상류 그대로 둔다 */
#include <linux/of_pci.h>
/* [한국어] devm_phy_optional_get, phy_reset/init/power_on/exit,
 * phy_pm_runtime_get_sync 계열 — 레인마다 PHY 를 다룬다 */
#include <linux/phy/phy.h>
/* [한국어] platform_driver 등록과 platform_get_resource_byname, platform_get_irq */
#include <linux/platform_device.h>
/* [한국어] regmap_read 와 regmap_update_bits. syscon 으로 얻은 외부 레지스터를 다룬다 */
#include <linux/regmap.h>
/* [한국어] struct resource. ks_pcie->app 이 애플리케이션 창의 물리 주소를 기억한다 —
 * MSI 목적지 주소를 만들 때 그 물리 주소가 필요하다 */
#include <linux/resource.h>
/* [한국어] SIGBUS. ARM 갈래에서 hook_fault_code 에 넘긴다 */
#include <linux/signal.h>

/* [한국어] PCI 코어 내부 헤더 */
#include "../../pci.h"
/* [한국어] DWC 공용 헤더. struct dw_pcie, dw_pcie_rp, dw_pcie_ep 와 그 ops 표,
 * dw_pcie_readl_dbi 계열, PORT_LOGIC_LTSSM_STATE 계열,
 * DW_PCIE_VER_ 계열, DWC_EPC_COMMON_FEATURES 가 여기서 온다.
 * 그 파일에는 이미 한국어 주석이 달려 있다 */
#include "pcie-designware.h"

/* [한국어] syscon 에서 읽은 32비트에서 벤더 ID(하위 16비트)를 뽑는 마스크.
 * 읽는 자: ks_pcie_init_id() */
#define PCIE_VENDORID_MASK	0xffff
/* [한국어] 같은 값에서 장치 ID(상위 16비트)를 뽑는 시프트량 */
#define PCIE_DEVICEID_SHIFT	16

/* Application registers */
/* [한국어] 애플리케이션 창의 주변장치 ID 레지스터. 실리콘 리비전이 여기 있다.
 * 읽는 자: ks_pcie_quirk() 가 AM654 PG1.0 인지 판정할 때만 */
#define PID				0x000
/* [한국어] 그 레지스터에서 RTL 버전 필드의 마스크(비트 15-11) */
#define RTL				GENMASK(15, 11)
/* [한국어] 그 필드를 오른쪽으로 밀 시프트량. FIELD_GET 대신 마스크와 시프트를
 * 따로 쓰는 옛 방식이다 */
#define RTL_SHIFT			11
/* [한국어] AM654 PG1.0 실리콘의 RTL 버전 값. ks_pcie_quirk() 가 이 값과 같을 때만
 * MRRS 를 128바이트로 낮춘다. 값의 근거가 되는 TI 문서는 이 트리에 없다 */
#define AM6_PCI_PG1_RTL_VER		0x15

/* [한국어] 애플리케이션 창의 명령/상태 레지스터. 이 파일에서 가장 자주 쓰이는
 * 레지스터이며, 아래 세 비트가 모두 여기 있다.
 * 읽고-고치고-쓰기로 다루므로 세 비트가 서로를 지우지 않는다 */
#define CMD_STATUS			0x004
/* [한국어] 링크 훈련 활성화 비트. ks_pcie_start_link() 가 세우고
 * ks_pcie_stop_link() 가 지운다 */
#define LTSSM_EN_VAL		        BIT(0)
/* [한국어] outbound 주소 변환 활성화 비트. ks_pcie_setup_rc_app_regs() 가
 * 창을 다 깐 뒤 마지막에 세운다 */
#define OB_XLAT_EN_VAL		        BIT(1)
/* [한국어] DBI_CS2 비트. 세우면 DBI 공간에 BAR 크기 마스크가 겹쳐 보인다.
 * ks_pcie_set_dbi_mode() 와 _clear_dbi_mode() 가 이 비트 하나를 다루며,
 * 클럭 도메인이 달라 되읽어 확인해야 한다 */
#define DBI_CS2				BIT(5)

/* [한국어] config 접근 대상을 지정하는 레지스터. 옛 PCI 의 CF8 에 해당한다.
 * ks_pcie_other_map_bus() 가 접근 직전에 여기 BDF 를 써 둔다 */
#define CFG_SETUP			0x008
/* [한국어] 그 레지스터에서 버스 번호가 놓이는 자리(비트 23-16) */
#define CFG_BUS(x)			(((x) & 0xff) << 16)
/* [한국어] 장치 번호가 놓이는 자리(비트 12-8). 5비트라 0~31 이다 */
#define CFG_DEVICE(x)			(((x) & 0x1f) << 8)
/* [한국어] 기능 번호가 놓이는 자리(비트 2-0). 3비트라 0~7 이다 */
#define CFG_FUNC(x)			((x) & 0x7)
/* [한국어] Type 1 config 접근 표시. 대상이 루트 버스 바로 아래가 아니면 세운다 —
 * 브리지를 한 번 더 지나야 하는 접근이라는 뜻이다 */
#define CFG_TYPE1			BIT(24)

/* [한국어] outbound 창 크기 레지스터. ks_pcie_setup_rc_app_regs() 가
 * ilog2(OB_WIN_SIZE) 를 쓴다 */
#define OB_SIZE				0x030
/* [한국어] outbound 창 n 의 하위 주소 레지스터. 창 하나가 8바이트를 차지한다 */
#define OB_OFFSET_INDEX(n)		(0x200 + (8 * (n)))
/* [한국어] 그 창의 상위 주소 레지스터. 하위와 4바이트 떨어져 있다 */
#define OB_OFFSET_HI(n)			(0x204 + (8 * (n)))
/* [한국어] 창 활성화 비트. 하위 주소와 OR 해서 함께 쓴다 —
 * 주소의 하위 비트가 정렬 때문에 0 이라 그 자리를 빌려 쓸 수 있다 */
#define OB_ENABLEN			BIT(0)
/* [한국어] outbound 창 하나의 크기(MB 단위). 원문 주석이 8MB 임을 밝힌다.
 * ks_pcie_setup_rc_app_regs() 가 이 단위로 메모리 창을 잘라 매핑한다 */
#define OB_WIN_SIZE			8	/* 8MB */

/* [한국어] EP 모드에서 INTx n 을 켜는 레지스터. (n) - 1 을 곱하는 것은 인자가
 * 1~4(INTA~INTD)로 들어오기 때문이다 — 0 기반이 아니다.
 * ks_pcie_am654_raise_intx_irq() 가 Interrupt Pin 값을 그대로 넘긴다 */
#define PCIE_LEGACY_IRQ_ENABLE_SET(n)	(0x188 + (0x10 * ((n) - 1)))
/* [한국어] 그 짝이 되는 끄기 레지스터. set 과 4바이트 떨어져 있다 */
#define PCIE_LEGACY_IRQ_ENABLE_CLR(n)	(0x18c + (0x10 * ((n) - 1)))
/* [한국어] EP 모드에서 인터럽트를 올리는(assert) 레지스터 */
#define PCIE_EP_IRQ_SET			0x64
/* [한국어] 그 짝이 되는 내리는(deassert) 레지스터 */
#define PCIE_EP_IRQ_CLR			0x68
/* [한국어] 위 네 레지스터에 공통으로 쓰는 활성화 비트 */
#define INT_ENABLE			BIT(0)

/* IRQ register defines */
/* [한국어] 인터럽트 종료(EOI) 레지스터. 값으로 어느 인터럽트인지를 지정한다 —
 * INTx 는 0~3, MSI 는 MSI_IRQ_OFFSET 을 더한 4~11 로 보인다.
 * 근거가 되는 TI 문서는 이 트리에 없다 */
#define IRQ_EOI				0x050

/* [한국어] MSI 목적지 주소의 오프셋. ks_pcie_compose_msi_msg() 가 애플리케이션
 * 창의 물리 주소에 이것을 더해 장치에게 알려 줄 주소를 만든다 */
#define MSI_IRQ				0x054
/* [한국어] MSI 상태 레지스터 n. 여덟 개가 16바이트 간격으로 있고,
 * 각각이 벡터 넷을 담는다(비트 0~3) */
#define MSI_IRQ_STATUS(n)		(0x104 + ((n) << 4))
/* [한국어] MSI 벡터를 켜는 레지스터 n */
#define MSI_IRQ_ENABLE_SET(n)		(0x108 + ((n) << 4))
/* [한국어] MSI 벡터를 끄는 레지스터 n. set 과 별개라 읽고-고치고-쓰기가 필요 없다 */
#define MSI_IRQ_ENABLE_CLR(n)		(0x10c + ((n) << 4))
/* [한국어] EOI 레지스터에서 MSI 가 INTx 뒤에 이어지는 번호 오프셋.
 * ks_pcie_msi_irq_ack() 이 reg_offset 에 이것을 더해 쓴다 */
#define MSI_IRQ_OFFSET			4

/* [한국어] INTx 상태 레지스터 n. MSI 와 같은 16바이트 간격이지만 기준 오프셋이
 * 다르다. 이 INTx 의 상태는 비트 0 에 있다 */
#define IRQ_STATUS(n)			(0x184 + ((n) << 4))
/* [한국어] INTx 를 켜는 레지스터 n. 위 PCIE_LEGACY_IRQ_ENABLE_SET 과 계산식이
 * 다르지만 n=1 일 때 같은 주소가 된다 — 하나는 0 기반, 다른 하나는
 * 1 기반이기 때문이다. RC 경로와 EP 경로가 각자의 관례로 같은
 * 레지스터를 가리키는 셈이다 */
#define IRQ_ENABLE_SET(n)		(0x188 + ((n) << 4))
/* [한국어] INTx 활성화 비트. ks_pcie_config_intx_irq() 가 초기화 때 넷을 모두
 * 켜고 이후 건드리지 않는다 */
#define INTx_EN				BIT(0)

/* [한국어] 오류 상태 레지스터. 읽은 값을 그대로 되써서 지운다(RW1C 관용구) */
#define ERR_IRQ_STATUS			0x1c4
/* [한국어] 오류 보고를 켜는 레지스터. ks_pcie_enable_error_irq() 가 한 번만 쓴다 */
#define ERR_IRQ_ENABLE_SET		0x1c8
/* [한국어] ECRC 오류 비트(Keystone 2). 원문 주석이 밝힌다 */
#define ERR_AER				BIT(5)	/* ECRC error */
/* [한국어] ECRC 오류 비트(AM654). 세대마다 자리가 달라 둘을 따로 정의하고,
 * ks_pcie_handle_error_irq() 가 is_am6 로 갈라 본다 */
#define AM6_ERR_AER			BIT(4)	/* AM6 ECRC error */
/* [한국어] AXI 태그 조회 치명 오류. AM654 의 ECRC 와 **같은 비트 4** 다 —
 * 그래서 그 검사에 !is_am6 조건이 붙어 있다 */
#define ERR_AXI				BIT(4)	/* AXI tag lookup fatal error */
/* [한국어] 정정 가능 오류. 하드웨어가 이미 해결했으므로 dev_dbg 로만 남긴다 */
#define ERR_CORR			BIT(3)	/* Correctable error */
/* [한국어] 치명적이지 않은 오류. 역시 dev_dbg */
#define ERR_NONFATAL			BIT(2)	/* Non-fatal error */
/* [한국어] 치명적 오류. dev_err 로 남긴다 */
#define ERR_FATAL			BIT(1)	/* Fatal error */
/* [한국어] 시스템 오류. dev_err */
#define ERR_SYS				BIT(0)	/* System error */
/* [한국어] 위 여섯을 묶은 마스크. ks_pcie_enable_error_irq() 가 한 번에 켠다.
 * AM6_ERR_AER 는 ERR_AXI 와 같은 비트라 따로 넣지 않아도 덮인다 */
#define ERR_IRQ_ALL			(ERR_AER | ERR_AXI | ERR_CORR | \
					 ERR_NONFATAL | ERR_FATAL | ERR_SYS)

/* PCIE controller device IDs */
/* [한국어] Keystone 2 HK 계열의 장치 ID. ks_pcie_quirk() 의 매칭 표에 쓰인다 */
#define PCIE_RC_K2HK			0xb008
/* [한국어] Keystone 2 E 계열의 장치 ID */
#define PCIE_RC_K2E			0xb009
/* [한국어] Keystone 2 L 계열의 장치 ID */
#define PCIE_RC_K2L			0xb00a
/* [한국어] Keystone 2 G 계열의 장치 ID. 넷 다 MRRS 256바이트 제한에 걸린다 */
#define PCIE_RC_K2G			0xb00b

/* [한국어] Keystone 2 의 SoC 모드 필드 마스크. 비트 2-1 이다 — 한 비트 밀려 있는
 * 점이 AM654 와 다르다 */
#define KS_PCIE_DEV_TYPE_MASK		(0x3 << 1)
/* [한국어] 그 자리에 모드 값을 밀어 넣는 매크로 */
#define KS_PCIE_DEV_TYPE(mode)		((mode) << 1)

/* [한국어] 엔드포인트 모드 값 */
#define EP				0x0
/* [한국어] 레거시 엔드포인트 모드 값. 이 파일의 어느 코드도 이 값을 쓰지 않는다 —
 * 하드웨어가 제공하는 모드를 기록해 둔 것으로 보인다 */
#define LEG_EP				0x1
/* [한국어] 루트 컴플렉스 모드 값. ks_pcie_set_mode() 는 언제나 이것을 쓴다 */
#define RC				0x2

/* [한국어] SoC 클록 출력 활성화 비트. ks_pcie_set_mode() 가 모드와 함께 세운다.
 * 이름으로 보아 레퍼런스 클록을 밖으로 내보내는 스위치이며 RC 가 슬롯에
 * 클록을 공급해야 한다는 사실과 어긋나지 않으나, 근거가 되는 TI 문서는
 * 이 트리에 없다 */
#define KS_PCIE_SYSCLOCKOUTEN		BIT(0)

/* [한국어] AM654 의 SoC 모드 필드 마스크. 시프트 없이 하위 두 비트다 */
#define AM654_PCIE_DEV_TYPE_MASK	0x3
/* [한국어] AM654 EP 의 outbound 창 단위(64KB). ks_pcie_am654_ep_init() 이
 * ep->page_size 로 쓰고, epc_features 의 align 과 같은 값이어야 한다 */
#define AM654_WIN_SIZE			SZ_64K

/* [한국어] AM654 EP 의 BAR0 크기(16KB). ks_pcie_am654_ep_init() 이 크기 마스크로
 * (이 값 - 1) 을 쓴다. 그 창이 무엇을 노출하는지는 이 트리에서 확인 못 함 */
#define APP_ADDR_SPACE_0		(16 * SZ_1K)

/* [한국어] struct dw_pcie 에서 struct keystone_pcie 로 되돌아가는 매크로.
 * container_of 가 아니라 dev_get_drvdata 를 쓰는 점이 대부분의 DWC
 * 접착 계층과 다르다 — 이 파일은 dw_pcie 를 자기 구조체에 박아 두지 않고
 * 포인터로만 들고 있기 때문이다. 그래서 probe 가
 * platform_set_drvdata(pdev, ks_pcie) 로 걸어 둔 것을 되찾는 방식이 된다 */
#define to_keystone_pcie(x)		dev_get_drvdata((x)->dev)

/* [한국어] AM654 의 장치 ID. ks_pcie_quirk() 의 AM654 매칭 표에 쓰인다.
 * PCI_DEVICE_ID_ 로 시작하지만 include/linux/pci_ids.h 가 아니라
 * 이 파일에 정의되어 있다 */
#define PCI_DEVICE_ID_TI_AM654X		0xb00c

struct ks_pcie_of_data {
	/* [한국어] 이 compatible 이 RC 인지 EP 인지.
	 * 설정자: 아래 세 정적 ks_pcie_of_data 초기화자.
	 * 읽는 자: ks_pcie_probe() 의 switch (mode) 와
	 *   ks_pcie_am654_set_mode() 에 넘기는 인자.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE */
	enum dw_pcie_device_mode mode;
	/* [한국어] RC 모드에서 DWC 코어에 줄 콜백 표.
	 * 설정자: ks_pcie_rc_of_data(ks_pcie_host_ops)와
	 *   ks_pcie_am654_rc_of_data(ks_pcie_am654_host_ops).
	 * 값 범위: EP 전용 of_data 에서는 NULL 이다.
	 * 두 host_ops 의 차이는 msi_init 유무뿐이다 — AM654 는 DWC 내장 MSI 를 쓴다 */
	const struct dw_pcie_host_ops *host_ops;
	/* [한국어] EP 모드에서 DWC 코어에 줄 콜백 표.
	 * 설정자: ks_pcie_am654_ep_of_data 만 채운다.
	 * 값 범위: Keystone 2 에는 EP 모드가 없어 NULL 이다 */
	const struct dw_pcie_ep_ops *ep_ops;
	/* [한국어] 이 하드웨어에 들어 있는 DWC IP 의 버전.
	 * 설정자: 세 of_data 초기화자(365A 또는 490A).
	 * 읽는 자: ks_pcie_probe() 가 pci->version 에 넣고,
	 *   dw_pcie_ver_is_ge(pci, 480A) 로 어느 set_mode 를 쓸지 가른다.
	 * 값 범위: DW_PCIE_VER_365A(Keystone 2) 또는 DW_PCIE_VER_490A(AM654) */
	u32 version;
};

struct keystone_pcie {
	/* [한국어] DWC 코어의 컨트롤러 상태. 이 구조체 안에 박아 두지 않고 포인터로만
	 * 들고 있다.
	 * 설정자: ks_pcie_probe() 가 따로 devm_kzalloc 한 것을 넣는다.
	 * 읽는 자: 이 파일의 거의 모든 함수.
	 * 역방향 변환은 to_keystone_pcie() 매크로가 drvdata 로 한다 */
	struct dw_pcie		*pci;
	/* PCI Device ID */
	/* [한국어] 이 파일의 어느 코드도 이 필드를 읽거나 쓰지 않는다(전수 확인).
	 * 장치 ID 를 담으려던 자리로 보이나 실제로는 ks_pcie_init_id() 가
	 *  syscon 에서 읽어 곧바로 DBI 에 쓰므로 저장할 곳이 필요 없다.
	 * 코드는 고치지 않고 이 사실만 적어 둔다 */
	u32			device_id;
	/* [한국어] INTA~INTD 각각에 배정된 커널 IRQ 번호.
	 * 설정자: ks_pcie_config_intx_irq() 의 루프가 순서대로 채운다.
	 * 읽는 자: ks_pcie_intx_irq_handler() 가 [0] 을 기준으로
	 *   irq_offset = irq - intx_host_irqs[0] 을 계산한다.
	 * 값 범위: 네 IRQ 번호가 연속이라는 전제가 그 계산에 깔려 있고,
	 *   그 전제는 장치 트리의 나열 순서에 달려 있다 */
	int			intx_host_irqs[PCI_NUM_INTX];

	/* [한국어] MSI 인터럽트 여덟 개 중 첫 번째의 **하드웨어** IRQ 번호.
	 * 설정자: ks_pcie_config_msi_irq() 가 첫 바퀴에서만 저장한다.
	 * 읽는 자: ks_pcie_msi_irq_handler() 가
	 *   offset = irq - msi_host_irq 로 자기가 몇 번째인지 역산한다.
	 * 값 범위: 커널 가상 IRQ 가 아니라 irq_data->hwirq 인 점에 주의 —
	 *   핸들러도 desc->irq_data.hwirq 와 비교하므로 앞뒤가 맞는다 */
	int			msi_host_irq;
	/* [한국어] 이 컨트롤러가 쓰는 PCIe 레인 수.
	 * 설정자: ks_pcie_probe() 가 DT 의 num-lanes 에서 읽는다(없으면 1).
	 * 읽는 자: PHY 배열과 device_link 배열의 길이,
	 *   ks_pcie_enable_phy()/_disable_phy() 의 루프 횟수 */
	int			num_lanes;
	/* [한국어] outbound 창 개수.
	 * 설정자: ks_pcie_probe() 의 RC 갈래가 DT 의 num-viewport 에서 읽는다.
	 *   이 속성이 없으면 probe 가 실패한다.
	 * 읽는 자: ks_pcie_setup_rc_app_regs() 의 매핑 루프 상한.
	 * 값 범위: EP 모드에서는 채워지지 않는다 */
	u32			num_viewport;
	/* [한국어] 레인마다 하나씩인 PHY 포인터 배열.
	 * 설정자: ks_pcie_probe() 가 devm_kcalloc 으로 잡고
	 *   devm_phy_optional_get 으로 채운다. optional 이라 NULL 일 수 있다.
	 * 읽는 자: ks_pcie_enable_phy()/_disable_phy() */
	struct phy		**phy;
	/* [한국어] 레인마다 하나씩인 device_link 배열.
	 * 설정자: ks_pcie_probe() 가 device_link_add 로 만든다.
	 * 왜 필요한가: PHY 드라이버와 이 드라이버 사이의 PM 순서를 정한다.
	 *   DL_FLAG_STATELESS 이므로 상태 추적은 하지 않고 순서만 강제한다.
	 * 읽는 자: ks_pcie_remove() 와 probe 의 err_link 경로가 역순으로 푼다 */
	struct device_link	**link;
	/* [한국어] 이 파일의 어느 코드도 이 필드를 읽거나 쓰지 않는다(전수 확인).
	 * MSI 인터럽트 컨트롤러 노드를 기억하려던 자리로 보이나,
	 * ks_pcie_config_msi_irq() 는 지역 변수로 얻어 쓰고 곧바로 of_node_put 한다.
	 * 코드는 고치지 않고 이 사실만 적어 둔다 */
	struct			device_node *msi_intc_np;
	/* [한국어] INTx 전용 IRQ 도메인.
	 * 설정자: ks_pcie_config_intx_irq() 가 만든다.
	 * 읽는 자: ks_pcie_handle_intx_irq() 의 generic_handle_domain_irq().
	 * 값 범위: AM654 에서는 만들어지지 않아 NULL 로 남는다 —
	 *   그 세대는 INTx 를 다루지 않기 때문이다.
	 * 해제하는 코드가 이 파일에 없다 */
	struct irq_domain	*intx_irq_domain;
	/* [한국어] 이 컨트롤러의 장치 트리 노드.
	 * 설정자: ks_pcie_probe().
	 * 읽는 자: ks_pcie_config_intx_irq()/_config_msi_irq() 가
	 *   자식 노드(legacy/msi-interrupt-controller)를 찾는 기준으로 쓴다.
	 *   dev->of_node 와 같은 값이지만 따로 들고 있다 */
	struct device_node	*np;

	/* Application register space */
	/* [한국어] 애플리케이션 레지스터 창의 커널 가상 주소.
	 * 원문 주석대로 DT 의 첫 번째 리소스(이름 app)다.
	 * 설정자: ks_pcie_probe() 의 devm_ioremap_resource.
	 * 읽는 자: ks_pcie_app_readl()/_writel() 둘뿐이고, 그 둘을 통해
	 *   이 파일 전체가 쓴다 */
	void __iomem		*va_app_base;	/* DT 1st resource */
	/* [한국어] 같은 창의 struct resource 사본(물리 주소를 담고 있다).
	 * 설정자: ks_pcie_probe() 가 *res 를 통째로 복사한다.
	 * 왜 필요한가: ks_pcie_compose_msi_msg() 가 MSI 목적지로 알려 줄 주소는
	 *   가상 주소가 아니라 **물리 주소**여야 한다. 링크 너머의 장치가 쓸
	 *   주소이기 때문이다. app.start 가 그 값이다.
	 * 읽는 자: ks_pcie_compose_msi_msg() 와 ks_pcie_msi_host_init() */
	struct resource		app;
	/* [한국어] 이 하드웨어가 AM654 RC 인가.
	 * 설정자: ks_pcie_probe() 가 of_device_is_compatible(np,
	 *   "ti,am654-pcie-rc") 일 때만 true 로 세운다.
	 * 값 범위: **AM654 EP 에서는 false 로 남는다** — compatible 문자열이
	 *   다르기 때문이다. 이름과 달리 "AM654 인가" 가 아니라
	 *   "AM654 RC 인가" 를 뜻하는 셈이며, 실제로 이 플래그를 보는 곳이
	 *   모두 RC 경로라 문제가 되지 않는다.
	 * 읽는 자: ks_pcie_setup_rc_app_regs(), ks_pcie_handle_error_irq(),
	 *   ks_pcie_host_init(), ks_pcie_config_intx_irq()/_config_msi_irq() */
	bool			is_am6;
};

/* [한국어]
 * ks_pcie_app_readl - 애플리케이션 레지스터 창에서 32비트를 읽는다
 *
 * @ks_pcie: 이 컨트롤러.  @offset: 창 시작점으로부터의 오프셋
 * @return: 읽은 값
 *
 * "app" 창은 ks_pcie_probe() 가 장치 트리의 같은 이름 리소스를 ioremap 해 둔
 * MMIO 영역이다. DWC IP 자체의 레지스터(DBI)와는 **다른 창**이며, TI 가 그
 * IP 를 SoC 에 붙이면서 덧붙인 것이다 — 링크 제어(CMD_STATUS), config 대상
 * 지정(CFG_SETUP), outbound 창, MSI/INTx 상태와 활성화, 오류 상태가 모두
 * 여기 있다.
 *
 * 이 파일의 하드웨어 조작 대부분이 이 함수와 짝인 writel 을 거친다.
 * DWC 쪽 레지스터는 dw_pcie_readl_dbi 계열로 따로 접근한다.
 *
 * _relaxed 가 아닌 보통의 readl 을 쓴다 — 이 파일의 다른 접근과의 순서를
 * 보장한다.
 *
 * 실행 컨텍스트: 제한 없음. MMIO 읽기 한 번이라 잠들지 않는다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → readl()
 */
static u32 ks_pcie_app_readl(struct keystone_pcie *ks_pcie, u32 offset)
{
	return readl(ks_pcie->va_app_base + offset);
}

/* [한국어]
 * ks_pcie_app_writel - 애플리케이션 레지스터 창에 32비트를 쓴다
 *
 * @ks_pcie: 이 컨트롤러.  @offset: 창 시작점으로부터의 오프셋.  @val: 쓸 값
 * @return: 없음
 *
 * ks_pcie_app_readl() 의 짝이다. 인자 순서가 (오프셋, 값)인 점에 주의한다 —
 * 커널의 writel(값, 주소)과 반대라 읽을 때 헷갈리기 쉽다.
 *
 * 실행 컨텍스트: 제한 없음.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → writel()
 */
static void ks_pcie_app_writel(struct keystone_pcie *ks_pcie, u32 offset,
			       u32 val)
{
	writel(val, ks_pcie->va_app_base + offset);
}

/* [한국어]
 * ks_pcie_msi_irq_ack - MSI 벡터 하나를 확인응답한다
 *
 * @data: 이 MSI 벡터의 irq_data. hwirq 가 벡터 번호다.  @return: 없음
 *
 * Keystone 은 MSI 상태를 레지스터 여덟 개에 흩어 담는다. 벡터 번호에서
 * 자리를 푸는 계산이 이 파일 전체에서 같은 모양으로 되풀이된다.
 *   reg_offset = irq % 8   어느 상태 레지스터인가
 *   bit_pos    = irq >> 3  그 안의 몇 번째 비트인가
 * ks_pcie_msi_irq_handler() 안의 원문 영어 주석이 그 배치를 밝힌다 —
 * MSI0 의 비트 0~3 이 벡터 0/8/16/24 를, MSI1 이 1/9/17/25 를 뜻한다.
 *
 * 두 번 쓴다.
 *   1) 상태 레지스터에 해당 비트를 써서 지운다(RW1C 로 보이는 사용 방식).
 *   2) IRQ_EOI 레지스터에 (reg_offset + MSI_IRQ_OFFSET) 을 써서 인터럽트
 *      컨트롤러 쪽에도 끝났음을 알린다. MSI_IRQ_OFFSET(4)을 더하는 것은
 *      EOI 레지스터가 INTx 넷과 MSI 여덟을 한 번호 공간에 담기 때문으로
 *      보이나, 근거가 되는 TI 문서는 이 트리에 없다.
 *
 * irq_data 에서 dw_pcie_rp 를 얻고 그것으로 dw_pcie 를, 다시
 * to_keystone_pcie()(dev_get_drvdata)로 이 컨트롤러를 되찾는 세 단계가
 * 이 파일의 MSI 콜백 넷에 공통이다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 락을 잡지 않는다 — mask/unmask 와 달리
 * 읽고-고치고-쓰기가 아니라 단순 쓰기뿐이라 경합이 없다.
 *
 * 호출 체인:
 *   (IRQ 코어) → irq_chip.irq_ack → [이 함수] → ks_pcie_app_writel()
 */
static void ks_pcie_msi_irq_ack(struct irq_data *data)
{
	struct dw_pcie_rp *pp  = irq_data_get_irq_chip_data(data);
	/* [한국어] 이 컨트롤러. 아래 두 단계를 거쳐 되찾는다 */
	struct keystone_pcie *ks_pcie;
	/* [한국어] MSI 벡터 번호. irq_data 에 hwirq 로 들어 있다 */
	u32 irq = data->hwirq;
	/* [한국어] 중간 단계인 DWC 컨트롤러 상태 */
	struct dw_pcie *pci;
	/* [한국어] 여덟 상태 레지스터 중 몇 번인가 */
	u32 reg_offset;
	/* [한국어] 그 레지스터 안의 몇 번째 비트인가 */
	u32 bit_pos;

	/* [한국어] dw_pcie_rp 에서 dw_pcie 로. container_of 계열 매크로다 */
	pci = to_dw_pcie_from_pp(pp);
	/* [한국어] dw_pcie 에서 이 파일의 구조체로. 이쪽은 dev_get_drvdata 를 쓴다 */
	ks_pcie = to_keystone_pcie(pci);

	/* [한국어] 여덟 레지스터에 벡터가 흩어져 있어 나머지 연산으로 자리를 고른다 */
	reg_offset = irq % 8;
	/* [한국어] 3비트 오른쪽으로 밀어 그 레지스터 안의 비트 번호를 얻는다.
	 * MSI0 의 비트 0~3 이 벡터 0/8/16/24 를 뜻하는 배치와 짝이다 */
	bit_pos = irq >> 3;

	/* [한국어] 상태 비트를 써서 지운다(RW1C 로 보이는 사용 방식) */
	ks_pcie_app_writel(ks_pcie, MSI_IRQ_STATUS(reg_offset),
			   BIT(bit_pos));
	ks_pcie_app_writel(ks_pcie, IRQ_EOI, reg_offset + MSI_IRQ_OFFSET);
}

/* [한국어]
 * ks_pcie_compose_msi_msg - EP 에 알려 줄 MSI 주소와 데이터를 만든다
 *
 * @data: 이 MSI 벡터의 irq_data.  @msg: 채울 MSI 메시지.  @return: 없음
 *
 * MSI 는 "장치가 특정 주소에 특정 값을 쓰면 그것이 인터럽트가 된다" 는
 * 방식이다. 그 주소와 값을 정해 주는 것이 이 콜백이고, 실제로 장치의 MSI
 * capability 에 써 넣는 일은 PCI 코어가 한다.
 *
 * 주소는 애플리케이션 레지스터 창의 **물리 주소** + MSI_IRQ 오프셋이다.
 * ks_pcie->app 이 probe 에서 저장해 둔 struct resource 이므로 app.start 가
 * 그 물리 시작 주소다. 가상 주소(va_app_base)가 아니라 물리 주소를 쓰는
 * 것이 요점 — 이 값은 PCIe 링크 너머의 장치가 쓸 주소이지 CPU 가 쓸
 * 주소가 아니다.
 *
 * 데이터는 hwirq 를 그대로 쓴다. 장치가 벡터 번호를 값으로 써 보내면
 * 컨트롤러가 그 번호에 해당하는 상태 비트를 세우고,
 * ks_pcie_msi_irq_handler() 가 그것을 풀어 도메인으로 넘긴다.
 *
 * 64비트 주소를 상하위로 나눠 담으므로 장치가 32비트 MSI 만 지원해도
 * 하위만 쓰면 된다.
 *
 * 실행 컨텍스트: MSI 설정 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (PCI MSI 코어) → irq_chip.irq_compose_msi_msg → [이 함수]
 */
static void ks_pcie_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(data);
	/* [한국어] 이 컨트롤러 */
	struct keystone_pcie *ks_pcie;
	/* [한국어] 중간 단계인 DWC 컨트롤러 상태 */
	struct dw_pcie *pci;
	/* [한국어] 장치에게 알려 줄 MSI 목적지 주소 */
	u64 msi_target;

	/* [한국어] dw_pcie_rp 에서 dw_pcie 로 */
	pci = to_dw_pcie_from_pp(pp);
	/* [한국어] dw_pcie 에서 이 파일의 구조체로 */
	ks_pcie = to_keystone_pcie(pci);

	/* [한국어] 애플리케이션 창의 **물리** 시작 주소에 MSI_IRQ 오프셋을 더한다.
	 * 가상 주소가 아닌 이유는 이 값을 쓸 주체가 링크 너머의 장치이기 때문이다 */
	msi_target = ks_pcie->app.start + MSI_IRQ;
	/* [한국어] 하위 32비트 */
	msg->address_lo = lower_32_bits(msi_target);
	/* [한국어] 상위 32비트. 장치가 32비트 MSI 만 지원하면 하위만 쓴다 */
	msg->address_hi = upper_32_bits(msi_target);
	/* [한국어] 벡터 번호를 데이터로 쓴다. 장치가 이 값을 써 보내면 컨트롤러가
	 * 해당 상태 비트를 세운다 */
	msg->data = data->hwirq;

	/* [한국어] 어떤 주소와 벡터를 알려 줬는지 디버그 로그로 남긴다 */
	dev_dbg(pci->dev, "msi#%d address_hi %#x address_lo %#x\n",
		(int)data->hwirq, msg->address_hi, msg->address_lo);
}

/* [한국어]
 * ks_pcie_msi_mask - MSI 벡터 하나를 막는다
 *
 * @data: 이 MSI 벡터의 irq_data.  @return: 없음
 *
 * 벡터 번호에서 (reg_offset, bit_pos) 를 푸는 계산은 ks_pcie_msi_irq_ack()
 * 과 같다. 그 자리의 비트를 ENABLE_CLR 레지스터에 써서 끈다.
 *
 * set 과 clr 이 별개 레지스터인 점이 요점이다 — 읽고-고치고-쓰기가 필요
 * 없으므로 다른 벡터의 상태를 건드릴 위험이 원천적으로 없다.
 *
 * 그런데도 pp->lock 을 raw_spin_lock_irqsave 로 잡는다. 이 함수 안의
 * 쓰기 하나만 보면 락이 필요 없어 보이지만, DWC 코어의 MSI 경로가 같은
 * 락으로 직렬화되므로 그 규약을 따르는 것으로 보인다. raw_ 판인 것은
 * 이 경로가 인터럽트 컨텍스트에서도 불릴 수 있어서다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트 또는 프로세스 컨텍스트. 스핀락을
 * 잡으므로 그 안에서 잠들 수 없다.
 *
 * 호출 체인:
 *   (IRQ 코어) → irq_chip.irq_mask → [이 함수] → ks_pcie_app_writel()
 */
static void ks_pcie_msi_mask(struct irq_data *data)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(data);
	/* [한국어] 이 컨트롤러 */
	struct keystone_pcie *ks_pcie;
	/* [한국어] MSI 벡터 번호 */
	u32 irq = data->hwirq;
	/* [한국어] 중간 단계 */
	struct dw_pcie *pci;
	/* [한국어] 인터럽트 상태를 저장할 자리 */
	unsigned long flags;
	/* [한국어] 여덟 상태 레지스터 중 몇 번인가 */
	u32 reg_offset;
	/* [한국어] 그 안의 비트 번호 */
	u32 bit_pos;

	/* [한국어] 이 함수 안의 쓰기 하나만 보면 락이 필요 없지만, DWC 코어의 MSI 경로가
	 * 같은 락으로 직렬화되므로 그 규약을 따른다. raw_ 판인 것은 이 경로가
	 * 인터럽트 컨텍스트에서도 불릴 수 있어서다 */
	raw_spin_lock_irqsave(&pp->lock, flags);

	/* [한국어] 락을 잡은 뒤에 포인터를 푸는 순서다. 반대여도 동작에는 차이가 없다 */
	pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 이 파일의 구조체로 */
	ks_pcie = to_keystone_pcie(pci);

	/* [한국어] 자리 계산은 ack 와 같다 */
	reg_offset = irq % 8;
	/* [한국어] 같은 배치 */
	bit_pos = irq >> 3;

	/* [한국어] 끄기 전용 레지스터에 쓴다. 켜기와 별개라 읽고-고치고-쓰기가 필요 없다 */
	ks_pcie_app_writel(ks_pcie, MSI_IRQ_ENABLE_CLR(reg_offset),
			   BIT(bit_pos));

	raw_spin_unlock_irqrestore(&pp->lock, flags);
}

/* [한국어]
 * ks_pcie_msi_unmask - MSI 벡터 하나를 다시 연다
 *
 * @data: 이 MSI 벡터의 irq_data.  @return: 없음
 *
 * ks_pcie_msi_mask() 의 정확한 거울상이다. 같은 자리 계산을 하고
 * ENABLE_CLR 대신 ENABLE_SET 레지스터에 쓴다.
 *
 * 락 사용도 mask 와 같다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트 또는 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (IRQ 코어) → irq_chip.irq_unmask → [이 함수] → ks_pcie_app_writel()
 */
static void ks_pcie_msi_unmask(struct irq_data *data)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(data);
	/* [한국어] 이 컨트롤러 */
	struct keystone_pcie *ks_pcie;
	/* [한국어] MSI 벡터 번호 */
	u32 irq = data->hwirq;
	/* [한국어] 중간 단계 */
	struct dw_pcie *pci;
	/* [한국어] 인터럽트 상태 저장 */
	unsigned long flags;
	/* [한국어] 레지스터 번호 */
	u32 reg_offset;
	/* [한국어] 비트 번호 */
	u32 bit_pos;

	/* [한국어] mask 와 같은 이유로 같은 락을 잡는다 */
	raw_spin_lock_irqsave(&pp->lock, flags);

	/* [한국어] dw_pcie_rp 에서 dw_pcie 로 */
	pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 이 파일의 구조체로 */
	ks_pcie = to_keystone_pcie(pci);

	/* [한국어] 자리 계산 */
	reg_offset = irq % 8;
	/* [한국어] 같은 배치 */
	bit_pos = irq >> 3;

	/* [한국어] 켜기 전용 레지스터. mask 가 쓰는 CLR 과 4바이트 떨어져 있다 */
	ks_pcie_app_writel(ks_pcie, MSI_IRQ_ENABLE_SET(reg_offset),
			   BIT(bit_pos));

	/* [한국어] 락을 푼다 */
	raw_spin_unlock_irqrestore(&pp->lock, flags);
}

/* [한국어] MSI 계층의 바닥 irq_chip. DWC 기본값(dw_pci_msi_bottom_irq_chip) 대신
 * 이것을 쓰도록 ks_pcie_msi_host_init() 이 pp->msi_irq_chip 을 갈아 끼운다 */
static struct irq_chip ks_pcie_msi_irq_chip = {
	/* [한국어] /proc/interrupts 에 보이는 이름 */
	.name = "KEYSTONE-PCI-MSI",
	.irq_ack = ks_pcie_msi_irq_ack,
	.irq_compose_msi_msg = ks_pcie_compose_msi_msg,
	.irq_mask = ks_pcie_msi_mask,
	.irq_unmask = ks_pcie_msi_unmask,
};

/**
 * ks_pcie_set_dbi_mode() - Set DBI mode to access overlaid BAR mask registers
 * @ks_pcie: A pointer to the keystone_pcie structure which holds the KeyStone
 *	     PCIe host controller driver information.
 *
 * Since modification of dbi_cs2 involves different clock domain, read the
 * status back to ensure the transition is complete.
 */
/* [한국어]
 * ks_pcie_set_dbi_mode - DBI_CS2 를 켜 BAR 마스크 레지스터를 드러낸다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 없음
 *
 * 위 원문 kernel-doc 이 목적과 주의점을 모두 밝힌다 — DBI 공간에 겹쳐
 * 놓인(overlaid) BAR 마스크 레지스터에 접근하기 위한 모드 전환이고,
 * dbi_cs2 수정이 다른 클럭 도메인에 걸쳐 있으므로 되읽어 전환이 끝났는지
 * 확인해야 한다.
 *
 * DWC IP 는 BAR 의 "값" 과 "크기 마스크" 를 같은 config 오프셋에 겹쳐 둔다.
 * 평소에는 값이 보이고, CS2 를 켜면 마스크가 보인다. 그래서 BAR 크기를
 * 정하려면 이 모드를 켜고 써야 한다.
 *
 * do-while 로 CS2 가 실제로 설 때까지 도는 것이 그 클럭 도메인 문제
 * 때문이다. 시한이 없는 무한 루프인데, 하드웨어가 응답하지 않으면 여기서
 * 멈춘다 — 코드는 고치지 않고 이 사실만 적어 둔다.
 *
 * 짝이 되는 ks_pcie_clear_dbi_mode() 로 반드시 되돌려야 한다. 켠 채로
 * 두면 이후의 평범한 BAR 접근이 마스크를 건드리게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(host_init 과 EP init 경로).
 * 바쁜 대기를 하므로 CPU 를 붙든다.
 *
 * 호출 체인:
 *   ks_pcie_msi_host_init() / ks_pcie_setup_rc_app_regs() /
 *   ks_pcie_am654_write_dbi2() → [이 함수] → ks_pcie_app_readl/writel()
 */
static void ks_pcie_set_dbi_mode(struct keystone_pcie *ks_pcie)
{
	u32 val;

	/* [한국어] 현재 값을 읽는다. 읽고-고치고-쓰기라 LTSSM_EN 과 OB_XLAT_EN 은 보존된다 */
	val = ks_pcie_app_readl(ks_pcie, CMD_STATUS);
	/* [한국어] CS2 비트를 세운다 */
	val |= DBI_CS2;
	/* [한국어] 되쓴다. 이 순간 DBI 공간에 BAR 크기 마스크가 겹쳐 보이기 시작한다 */
	ks_pcie_app_writel(ks_pcie, CMD_STATUS, val);

	do {
		/* [한국어] 클럭 도메인이 달라 쓰기가 곧바로 반영되지 않는다. 되읽어 확인한다.
		 * 시한이 없어 하드웨어가 응답하지 않으면 여기서 멈춘다 */
		val = ks_pcie_app_readl(ks_pcie, CMD_STATUS);
	} while (!(val & DBI_CS2));
}

/**
 * ks_pcie_clear_dbi_mode() - Disable DBI mode
 * @ks_pcie: A pointer to the keystone_pcie structure which holds the KeyStone
 *	     PCIe host controller driver information.
 *
 * Since modification of dbi_cs2 involves different clock domain, read the
 * status back to ensure the transition is complete.
 */
/* [한국어]
 * ks_pcie_clear_dbi_mode - DBI_CS2 를 꺼 평상시 BAR 접근으로 되돌린다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 없음
 *
 * ks_pcie_set_dbi_mode() 의 거울상이다. 위 원문 kernel-doc 이 같은 이유로
 * 되읽어 확인해야 한다고 밝힌다.
 *
 * do-while 의 조건만 반대다 — CS2 가 내려갈 때까지 돈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 바쁜 대기를 한다.
 *
 * 호출 체인:
 *   ks_pcie_msi_host_init() / ks_pcie_setup_rc_app_regs() /
 *   ks_pcie_am654_write_dbi2() → [이 함수]
 */
static void ks_pcie_clear_dbi_mode(struct keystone_pcie *ks_pcie)
{
	u32 val;

	/* [한국어] 현재 값을 읽는다 */
	val = ks_pcie_app_readl(ks_pcie, CMD_STATUS);
	/* [한국어] CS2 비트만 지운다 */
	val &= ~DBI_CS2;
	/* [한국어] 되쓴다 */
	ks_pcie_app_writel(ks_pcie, CMD_STATUS, val);

	do {
		/* [한국어] set 판과 같은 이유로 되읽어 확인한다. 조건만 반대다 */
		val = ks_pcie_app_readl(ks_pcie, CMD_STATUS);
	} while (val & DBI_CS2);
}

/* [한국어]
 * ks_pcie_msi_host_init - MSI 를 Keystone 방식으로 초기화한다
 *
 * @pp: DWC 의 루트 포트 상태.  @return: 0 = 성공, 음수 errno = 도메인 생성 실패
 *
 * dw_pcie_host_ops 의 msi_init 콜백이며, 이 파일의 인터럽트 구조에서
 * 핵심이 되는 함수다.
 *
 * 이 콜백이 **존재한다는 사실 자체**가 먼저 의미를 갖는다. DWC 코어는
 * drivers/pci/controller/dwc/pcie-designware-host.c:1524 에서
 * use_imsi_rx = !(pp->ops->msi_init || ...) 로 계산하므로, 이 콜백을
 * 등록한 것만으로 IP 내장 MSI 수신기(iMSI-RX)를 쓰지 않기로 결정된다.
 * 그 뒤 같은 파일 :1546 에서 이 함수가 불린다.
 *
 * 하는 일은 셋이다.
 *   1) BAR0 를 MSI 수신용으로 설정한다. DBI_CS2 모드를 켜고 크기 마스크에
 *      SZ_4K - 1 을 쓴 뒤 모드를 되돌린다. 그 사이의 두 dw_pcie_writel_dbi
 *      중 첫 번째(값 1)의 역할은 코드만으로는 분명하지 않다 — 근거가 되는
 *      TI/Synopsys 문서는 이 트리에 없다.
 *   2) 원문 영어 주석대로, BAR0 에 애플리케이션 레지스터 창의 **물리
 *      주소**를 써 넣는다. 그러면 링크 너머의 장치가 그 주소에 쓴 것이
 *      MSI 로 잡힌다. 주석이 "물리 주소를 써서 충돌을 피한다" 고 밝힌다.
 *   3) pp->msi_irq_chip 을 이 파일의 것으로 바꾼 뒤
 *      dw_pcie_allocate_domains() 를 부른다. 도메인 뼈대는 DWC 것을 그대로
 *      빌리고 바닥 chip 만 갈아 끼우는 절충이다. DWC 기본값은
 *      같은 파일 :947 이 세우고 :556 의 irq_domain_set_info() 가 쓴다.
 *
 * AM654 는 이 콜백을 등록하지 않는다(ks_pcie_am654_host_ops 에 msi_init 이
 * 없다) — 그 세대는 DWC 내장 MSI 를 그대로 쓴다는 뜻이다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트(DWC 코어가 되부른다).
 *
 * 호출 체인:
 *   ks_pcie_probe() → dw_pcie_host_init() → pp->ops->msi_init → [이 함수]
 *     → ks_pcie_set_dbi_mode() → dw_pcie_allocate_domains()
 */
static int ks_pcie_msi_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 이 컨트롤러 */
	struct keystone_pcie *ks_pcie = to_keystone_pcie(pci);

	/* Configure and set up BAR0 */
	ks_pcie_set_dbi_mode(ks_pcie);

	/* Enable BAR0 */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 1);
	/* [한국어] BAR0 의 크기 마스크를 4KB - 1 로 정한다. 바로 위 줄의 값 1 쓰기가
	 * 무엇을 뜻하는지는 코드만으로 분명하지 않고, 근거가 되는 문서는
	 * 이 트리에 없다 */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, SZ_4K - 1);

	ks_pcie_clear_dbi_mode(ks_pcie);

	/*
	 * For BAR0, just setting bus address for inbound writes (MSI) should
	 * be sufficient.  Use physical address to avoid any conflicts.
	 */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, ks_pcie->app.start);

	/* [한국어] DWC 기본 바닥 chip 대신 이 파일의 것을 쓴다.
	 * 그 기본값은 drivers/pci/controller/dwc/pcie-designware-host.c:947 이 세우고
	 * 같은 파일 :556 의 irq_domain_set_info() 가 쓴다.
	 * 이 줄이 dw_pcie_allocate_domains() 보다 먼저여야 그 도메인이
	 * 이 chip 을 쓰게 된다 */
	pp->msi_irq_chip = &ks_pcie_msi_irq_chip;
	return dw_pcie_allocate_domains(pp);
}

/* [한국어]
 * ks_pcie_handle_intx_irq - INTx 하나를 도메인으로 넘기고 EOI 한다
 *
 * @ks_pcie: 이 컨트롤러.  @offset: INTx 번호(0~3, INTA~INTD)
 * @return: 없음
 *
 * Keystone 은 INTA~INTD 각각에 상태 레지스터를 따로 둔다. 그래서 이
 * 함수는 "몇 번 INTx 인가" 를 오프셋으로 받아 그 레지스터 하나만 본다 —
 * 여러 비트를 훑는 루프가 없는 이유다.
 *
 * 비트 0 만 확인하는 것도 그 때문이다. IRQ_STATUS(offset) 레지스터에서
 * 이 INTx 의 상태가 비트 0 에 있다.
 *
 * EOI 는 pending 여부와 무관하게 언제나 한다. 인터럽트가 걸려 이 함수가
 * 불렸는데 상태 비트가 서 있지 않은 경우에도 컨트롤러 쪽은 정리해야
 * 하기 때문으로 보인다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(체인 핸들러 안).
 *
 * 호출 체인:
 *   ks_pcie_intx_irq_handler() → [이 함수] → generic_handle_domain_irq()
 */
static void ks_pcie_handle_intx_irq(struct keystone_pcie *ks_pcie,
				    int offset)
{
	struct dw_pcie *pci = ks_pcie->pci;
	/* [한국어] 로그 출력 대상 */
	struct device *dev = pci->dev;
	/* [한국어] 이 INTx 의 상태 */
	u32 pending;

	/* [한국어] INTx 마다 상태 레지스터가 따로라 하나만 읽으면 된다 */
	pending = ks_pcie_app_readl(ks_pcie, IRQ_STATUS(offset));

	/* [한국어] 이 INTx 의 상태가 비트 0 에 있다 */
	if (BIT(0) & pending) {
		/* [한국어] 어느 INTx 인지 디버그 로그로 남긴다 */
		dev_dbg(dev, ": irq: irq_offset %d", offset);
		/* [한국어] 도메인에 매핑된 장치 핸들러를 부른다 */
		generic_handle_domain_irq(ks_pcie->intx_irq_domain, offset);
	}

	/* EOI the INTx interrupt */
	ks_pcie_app_writel(ks_pcie, IRQ_EOI, offset);
}

/* [한국어]
 * ks_pcie_enable_error_irq - 컨트롤러 오류 보고를 모두 연다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 없음
 *
 * ERR_IRQ_ALL 은 이 파일 위쪽에서 여섯 오류 비트(AER/AXI/CORR/NONFATAL/
 * FATAL/SYS)를 OR 로 묶어 둔 것이다. 그것을 ENABLE_SET 레지스터에 한 번에
 * 쓴다.
 *
 * ks_pcie_probe() 의 맨 마지막에 불린다 — RC 든 EP 든 초기화가 다 끝난
 * 뒤에야 오류를 받기 시작한다. 초기화 도중의 링크 흔들림을 오류로
 * 보고하지 않으려는 순서다.
 *
 * 읽고-OR-쓰기가 아니라 통째로 쓰는데, ENABLE_SET 이 "1 을 쓴 비트만
 * 켜는" 레지스터로 보이기 때문이다(짝이 되는 ERR_IRQ_STATUS 를
 * ks_pcie_handle_error_irq 이 읽은 값 그대로 되써서 지우는 것과 같은
 * RW1C 계열 관용구). 근거가 되는 TI 문서는 이 트리에 없다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ks_pcie_probe() → [이 함수] → ks_pcie_app_writel()
 */
static void ks_pcie_enable_error_irq(struct keystone_pcie *ks_pcie)
{
	ks_pcie_app_writel(ks_pcie, ERR_IRQ_ENABLE_SET, ERR_IRQ_ALL);
}

/* [한국어]
 * ks_pcie_handle_error_irq - 컨트롤러 오류를 종류별로 로그하고 지운다
 *
 * @ks_pcie: 이 컨트롤러
 * @return: IRQ_HANDLED = 처리했다, IRQ_NONE = 내 인터럽트가 아니다
 *
 * 오류 상태 레지스터를 한 번 읽어 비트마다 메시지를 남긴다. 0 이면
 * IRQ_NONE 을 돌려주는데, 오류 IRQ 가 IRQF_SHARED 로 등록되어 있어
 * 다른 장치의 인터럽트에도 불릴 수 있기 때문이다.
 *
 * 로그 수준이 심각도에 따라 갈린다 — System/Fatal/AXI/ECRC 는 dev_err,
 * Non Fatal 과 Correctable 은 dev_dbg 다. 정정 가능한 오류는 하드웨어가
 * 이미 해결했으므로 평상시에 로그를 채우지 않게 한 것이다.
 *
 * 세대별 분기가 둘 있다.
 *   - ERR_AXI 는 AM654 가 아닐 때만 본다.
 *   - ECRC 오류의 비트 자리가 세대마다 달라(ERR_AER 는 비트 5,
 *     AM6_ERR_AER 는 비트 4) 둘을 OR 조건으로 확인한다.
 *     비트 4 가 Keystone 2 에서는 AXI 오류이고 AM654 에서는 ECRC 라는
 *     뜻이며, 그래서 위의 AXI 검사에 is_am6 조건이 붙어 있다.
 *
 * 마지막에 읽은 값을 그대로 되써서 지운다 — RW1C 관용구이며, 읽은 뒤
 * 새로 선 비트는 살아남아 다음 인터럽트가 처리한다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   ks_pcie_err_irq_handler() → [이 함수] → ks_pcie_app_readl/writel()
 */
static irqreturn_t ks_pcie_handle_error_irq(struct keystone_pcie *ks_pcie)
{
	u32 reg;
	/* [한국어] 로그 출력 대상 */
	struct device *dev = ks_pcie->pci->dev;

	/* [한국어] 오류 상태를 한 번 읽어 아래에서 비트마다 확인한다 */
	reg = ks_pcie_app_readl(ks_pcie, ERR_IRQ_STATUS);
	/* [한국어] 0 이면 다른 장치의 인터럽트다. IRQF_SHARED 로 등록했으므로 필요한 판정이다 */
	if (!reg)
		return IRQ_NONE;

	/* [한국어] 시스템 오류 */
	if (reg & ERR_SYS)
		/* [한국어] 심각하므로 dev_err */
		dev_err(dev, "System Error\n");

	/* [한국어] 치명적 오류 */
	if (reg & ERR_FATAL)
		/* [한국어] 역시 dev_err */
		dev_err(dev, "Fatal Error\n");

	/* [한국어] 치명적이지 않은 오류 */
	if (reg & ERR_NONFATAL)
		/* [한국어] 그 트랜잭션만 실패한 것이므로 dev_dbg */
		dev_dbg(dev, "Non Fatal Error\n");

	/* [한국어] 정정 가능 오류 */
	if (reg & ERR_CORR)
		/* [한국어] 하드웨어가 이미 해결했으므로 dev_dbg. 평상시 로그를 채우지 않게 한다 */
		dev_dbg(dev, "Correctable Error\n");

	/* [한국어] AXI 오류는 Keystone 2 에서만 본다. AM654 에서는 같은 비트 4 가
	 * ECRC 를 뜻하기 때문이다 */
	if (!ks_pcie->is_am6 && (reg & ERR_AXI))
		/* [한국어] 치명적이므로 dev_err */
		dev_err(dev, "AXI tag lookup fatal Error\n");

	/* [한국어] ECRC 오류의 비트 자리가 세대마다 달라 둘을 OR 로 확인한다 */
	if (reg & ERR_AER || (ks_pcie->is_am6 && (reg & AM6_ERR_AER)))
		/* [한국어] dev_err */
		dev_err(dev, "ECRC Error\n");

	/* [한국어] 읽은 값을 그대로 되써서 지운다. 읽은 뒤 새로 선 비트는 살아남아
	 * 다음 인터럽트가 처리한다 */
	ks_pcie_app_writel(ks_pcie, ERR_IRQ_STATUS, reg);

	return IRQ_HANDLED;
}

/* [한국어]
 * ks_pcie_ack_intx_irq - INTx 확인응답 콜백(빈 구현)
 *
 * @d: 이 INTx 의 irq_data(쓰지 않는다).  @return: 없음
 *
 * 본문이 비어 있다. 실제 확인응답은 ks_pcie_handle_intx_irq() 가
 * IRQ_EOI 레지스터에 쓰는 것으로 이뤄지므로, irq_chip 쪽에서 할 일이 없다.
 *
 * 그렇다면 왜 두는가: handle_level_irq 는 chip->irq_ack 를 조건 없이
 * 부르므로 NULL 이면 안 된다. 자리를 채우기 위한 빈 함수다.
 *
 * 같은 이유로 mask/unmask 도 비어 있다 — 이 하드웨어에 개별 INTx 를 끄고
 * 켜는 수단이 있는지는 코드만으로 알 수 없고, 근거가 되는 TI 문서는
 * 이 트리에 없다. 참고로 IRQ_ENABLE_SET(i) 레지스터는 존재하지만
 * ks_pcie_config_intx_irq() 가 초기화 때 넷을 모두 켜고 이후 건드리지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 아무 일도 하지 않는다.
 *
 * 호출 체인:
 *   (IRQ 코어의 handle_level_irq) → irq_chip.irq_ack → [이 함수]
 */
static void ks_pcie_ack_intx_irq(struct irq_data *d)
{
}

/* [한국어]
 * ks_pcie_mask_intx_irq - INTx 마스크 콜백(빈 구현)
 *
 * @d: 이 INTx 의 irq_data(쓰지 않는다).  @return: 없음
 *
 * ks_pcie_ack_intx_irq() 와 같은 이유로 비어 있다(그쪽 주석 참조).
 * handle_level_irq 가 핸들러를 부르기 전에 이것을 부르지만, 실제로
 * 막지는 않는다.
 *
 * 즉 이 드라이버에서 disable_irq() 로 INTx 하나를 막는 것은 IRQ 코어
 * 수준에서만 동작하고 하드웨어에는 전달되지 않는다. 코드는 고치지 않고
 * 이 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트 또는 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (IRQ 코어) → irq_chip.irq_mask → [이 함수]
 */
static void ks_pcie_mask_intx_irq(struct irq_data *d)
{
}

/* [한국어]
 * ks_pcie_unmask_intx_irq - INTx 언마스크 콜백(빈 구현)
 *
 * @d: 이 INTx 의 irq_data(쓰지 않는다).  @return: 없음
 *
 * 위 둘과 같은 이유로 비어 있다. handle_level_irq 가 장치 핸들러를 부른
 * 뒤 이것을 부른다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   (IRQ 코어의 handle_level_irq) → irq_chip.irq_unmask → [이 함수]
 */
static void ks_pcie_unmask_intx_irq(struct irq_data *d)
{
}

/* [한국어] INTx 용 irq_chip. 세 콜백이 모두 빈 함수다 — 자리를 채우기 위한 것이며
 * 실제 확인응답은 ks_pcie_handle_intx_irq() 의 EOI 쓰기가 한다 */
static struct irq_chip ks_pcie_intx_irq_chip = {
	/* [한국어] /proc/interrupts 에 보이는 이름 */
	.name = "Keystone-PCI-INTX-IRQ",
	.irq_ack = ks_pcie_ack_intx_irq,
	.irq_mask = ks_pcie_mask_intx_irq,
	.irq_unmask = ks_pcie_unmask_intx_irq,
};

/* [한국어]
 * ks_pcie_init_intx_irq_map - INTx 가상 IRQ 하나를 설정한다
 *
 * @d: INTx 도메인.  @irq: 배정된 커널 가상 IRQ 번호
 * @hw_irq: 하드웨어 번호(0~3, INTA~INTD).  @return: 항상 0
 *
 * irq_domain_ops 의 map 콜백이다. 하위 장치가 INTx 를 요청할 때 불린다.
 *
 * handle_level_irq 를 쓰는 것은 INTx 가 PCIe 스펙상 레벨 트리거이기
 * 때문이다. 그것이 mask → ack → 장치 핸들러 → unmask 순서를 보장하는데,
 * 이 파일에서는 그 셋이 모두 빈 함수라 실질적으로는 핸들러 호출만 남는다.
 *
 * irq_set_chip_data 에 d->host_data 를 붙이지만, 그 값은
 * ks_pcie_config_intx_irq() 가 도메인을 만들 때 NULL 을 넘겼으므로
 * NULL 이다. 세 irq_chip 콜백이 모두 비어 있어 그 값을 쓸 일도 없다.
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * xlate 로 irq_domain_xlate_onetwocell 을 쓰는 것은 장치 트리의
 * interrupts 속성이 셀 하나 또는 둘로 오는 표준 형태를 받기 위해서다.
 *
 * 실행 컨텍스트: IRQ 매핑 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (irq_domain 코어) → irq_domain_ops.map → [이 함수]
 */
static int ks_pcie_init_intx_irq_map(struct irq_domain *d,
				     unsigned int irq, irq_hw_number_t hw_irq)
{
	irq_set_chip_and_handler(irq, &ks_pcie_intx_irq_chip,
				 handle_level_irq);
	/* [한국어] 도메인의 host_data 를 붙이지만, 그 값은 도메인 생성 때 NULL 을
	 * 넘겼으므로 NULL 이다. 세 콜백이 모두 비어 있어 쓸 일도 없다 */
	irq_set_chip_data(irq, d->host_data);

	return 0;
}

/* [한국어] INTx 도메인의 연산 표 */
static const struct irq_domain_ops ks_pcie_intx_irq_domain_ops = {
	/* [한국어] 가상 IRQ 하나를 설정하는 콜백 */
	.map = ks_pcie_init_intx_irq_map,
	.xlate = irq_domain_xlate_onetwocell,
};

/* [한국어]
 * ks_pcie_setup_rc_app_regs - RC 모드의 애플리케이션 레지스터를 설정한다
 *
 * @ks_pcie: 이 컨트롤러
 * @return: 0 = 성공, -ENODEV = 메모리 창을 찾지 못함
 *
 * RC 로 동작하려면 두 가지가 필요하다 — 밖에서 들어오는 접근(inbound)을
 * 막고, 안에서 나가는 접근(outbound)의 주소 변환을 깔아 두는 것이다.
 *
 *   1) inbound 차단. DBI_CS2 모드에서 BAR0/BAR1 을 0 으로 만든다.
 *      RC 는 자기 BAR 로 메모리를 노출할 이유가 없고, 열어 두면 링크 너머
 *      장치가 SoC 메모리에 닿는 통로가 된다.
 *      원문 주석 "Disable BARs for inbound access" 가 그 뜻이다.
 *
 *   2) AM654 는 여기서 끝난다(is_am6 검사로 곧바로 반환). 그 세대는
 *      outbound 변환을 DWC 표준 ATU 로 처리하기 때문으로 보이며,
 *      근거가 되는 TI 문서는 이 트리에 없다.
 *
 *   3) Keystone 2 는 outbound 창을 손으로 깐다. 창 크기를 OB_WIN_SIZE(8MB)
 *      의 로그값으로 설정한 뒤, DT 의 메모리 창을 8MB 단위로 잘라
 *      num_viewport 개까지 1:1 로 매핑한다. 원문 주석
 *      "Using Direct 1:1 mapping of RC <-> PCI memory space" 가 그것이다.
 *      각 창은 하위 32비트에 OB_ENABLEN 을 OR 해 함께 켠다.
 *
 *   4) 마지막에 CMD_STATUS 의 OB_XLAT_EN_VAL 을 세워 outbound 변환을
 *      통째로 활성화한다. 창을 다 깐 뒤에 켜는 순서다.
 *
 * 루프 조건에 (start < end) 가 함께 있어, num_viewport 보다 창이 작으면
 * 일찍 끝난다. 반대로 창이 num_viewport 로 덮을 수 있는 것보다 크면
 * 나머지는 매핑되지 않는다 — 그 경우를 알리는 코드는 없다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트(DWC 가 host_init 을 통해
 * 되부른다). ks_pcie_set_dbi_mode() 가 바쁜 대기를 한다.
 *
 * 호출 체인:
 *   ks_pcie_host_init() → [이 함수]
 *     → ks_pcie_set_dbi_mode() → dw_pcie_writel_dbi() → ks_pcie_app_writel()
 */
static int ks_pcie_setup_rc_app_regs(struct keystone_pcie *ks_pcie)
{
	u32 val;
	/* [한국어] 깔 outbound 창의 개수. DT 의 num-viewport 에서 온다 */
	u32 num_viewport = ks_pcie->num_viewport;
	/* [한국어] DWC 컨트롤러 상태 */
	struct dw_pcie *pci = ks_pcie->pci;
	/* [한국어] DWC 루트 포트 상태. 아래에서 브리지의 창 목록을 얻는 데 쓴다 */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 창 목록을 훑을 반복자 */
	struct resource_entry *entry;
	/* [한국어] 찾은 메모리 창 */
	struct resource *mem;
	/* [한국어] 그 창의 시작과 끝. 루프가 start 를 8MB 씩 전진시킨다 */
	u64 start, end;
	/* [한국어] 창 인덱스 */
	int i;

	/* [한국어] PCI 코어가 DT 의 ranges 로부터 만들어 둔 창 목록에서 첫 메모리 창을 찾는다 */
	entry = resource_list_first_type(&pp->bridge->windows, IORESOURCE_MEM);
	/* [한국어] 메모리 창이 없으면 RC 로 동작할 수 없다 */
	if (!entry)
		return -ENODEV;

	/* [한국어] 찾은 자원 */
	mem = entry->res;
	/* [한국어] 매핑을 시작할 주소 */
	start = mem->start;
	/* [한국어] 매핑을 끝낼 주소 */
	end = mem->end;

	/* Disable BARs for inbound access */
	ks_pcie_set_dbi_mode(ks_pcie);
	/* [한국어] BAR0 을 0 으로 만들어 inbound 접근을 막는다.
	 * RC 는 자기 BAR 로 메모리를 노출할 이유가 없고, 열어 두면 링크 너머
	 * 장치가 SoC 메모리에 닿는 통로가 된다 */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 0);
	/* [한국어] BAR1 도 마찬가지 */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_1, 0);
	ks_pcie_clear_dbi_mode(ks_pcie);

	/* [한국어] AM654 는 여기서 끝난다. 그 세대는 outbound 변환을 DWC 표준 ATU 로
	 * 처리하기 때문으로 보이며, 근거가 되는 TI 문서는 이 트리에 없다 */
	if (ks_pcie->is_am6)
		return 0;

	/* [한국어] 창 크기를 로그값으로 쓴다. 8MB 이므로 23 이다 */
	val = ilog2(OB_WIN_SIZE);
	/* [한국어] 크기 레지스터에 쓴다. 모든 창에 공통으로 적용된다 */
	ks_pcie_app_writel(ks_pcie, OB_SIZE, val);

	/* Using Direct 1:1 mapping of RC <-> PCI memory space */
	for (i = 0; i < num_viewport && (start < end); i++) {
		/* [한국어] 창 i 의 하위 주소를 쓴다. 활성화 비트를 OR 해 함께 켠다 —
		 * 주소의 하위 비트가 정렬 때문에 0 이라 그 자리를 빌려 쓸 수 있다 */
		ks_pcie_app_writel(ks_pcie, OB_OFFSET_INDEX(i),
				   lower_32_bits(start) | OB_ENABLEN);
		/* [한국어] 상위 주소를 쓴다 */
		ks_pcie_app_writel(ks_pcie, OB_OFFSET_HI(i),
				   upper_32_bits(start));
		/* [한국어] 다음 창은 8MB 뒤다. 이렇게 1:1 로 이어 붙인다 */
		start += OB_WIN_SIZE * SZ_1M;
	}

	/* [한국어] 현재 명령/상태 값을 읽는다 */
	val = ks_pcie_app_readl(ks_pcie, CMD_STATUS);
	/* [한국어] outbound 변환 활성화 비트를 세운다 */
	val |= OB_XLAT_EN_VAL;
	/* [한국어] 창을 다 깐 뒤에 켜는 순서다. 반대로 하면 설정 중인 창으로 트랜잭션이
	 * 나갈 수 있다 */
	ks_pcie_app_writel(ks_pcie, CMD_STATUS, val);

	return 0;
}

/* [한국어]
 * ks_pcie_other_map_bus - 하위 장치 config 접근의 주소를 만든다
 *
 * @bus: 대상 버스.  @devfn: 대상 장치/기능.  @where: config 오프셋
 * @return: 접근할 커널 가상 주소, 또는 NULL(링크가 없을 때)
 *
 * pci_ops 의 map_bus 콜백이다. PCI 코어의 generic config 접근이 이 함수가
 * 돌려준 주소에 그대로 읽고 쓴다.
 *
 * 이 하드웨어는 config 대상을 주소로 인코딩하지 않는다. 대신 CFG_SETUP
 * 레지스터에 "다음 접근은 이 BDF 를 향한다" 를 써 두고, 고정된 창
 * (pp->va_cfg0_base)에 접근하면 컨트롤러가 그리로 보낸다. 옛 PCI 의
 * CF8/CFC 방식과 같은 발상이다.
 *
 * 그래서 이 함수가 하는 일이 둘이다.
 *   1) 링크가 서 있는지 확인한다. 위 원문 영어 주석이 그 이유를 길게
 *      밝힌다 — 링크가 없을 때 config 접근을 하면 시스템 버스가 오류를
 *      SError 로 올려 보내는 플랫폼이 있고, 그것을 막는 마지막 방어선이다.
 *      주석은 이 검사가 본질적으로 경합적이라는 점도 인정한다(검사 뒤
 *      링크가 내려가면 여전히 SError 가 난다).
 *   2) CFG_SETUP 에 버스/장치/기능과 Type 0/1 구분을 써 넣는다.
 *      부모가 루트 버스가 아니면 CFG_TYPE1 을 얹는다 — 브리지를 한 번 더
 *      지나야 하는 접근이라는 뜻이다.
 *
 * CFG_SETUP 을 쓰고 주소를 돌려주는 사이에 다른 CPU 가 끼어들면 엉뚱한
 * 장치에 접근하게 되지만, PCI 코어가 config 접근 전체를 pci_lock 으로
 * 직렬화하므로 성립한다.
 *
 * AM654 는 이 경로를 쓰지 않는다 — ks_pcie_host_init() 이 그 세대에는
 * child_ops 를 걸지 않으므로, 하위 장치도 DWC 표준 경로로 간다.
 *
 * 실행 컨텍스트: config 접근 경로(스핀락 보유). 잠들 수 없다.
 *
 * 호출 체인:
 *   (PCI 코어) → ks_child_pcie_ops.map_bus → [이 함수] → dw_pcie_link_up()
 */
static void __iomem *ks_pcie_other_map_bus(struct pci_bus *bus,
					   unsigned int devfn, int where)
{
	struct dw_pcie_rp *pp = bus->sysdata;
	/* [한국어] DWC 컨트롤러 상태 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 이 파일의 구조체 */
	struct keystone_pcie *ks_pcie = to_keystone_pcie(pci);
	/* [한국어] CFG_SETUP 에 쓸 값 */
	u32 reg;

	/*
	 * Checking whether the link is up here is a last line of defense
	 * against platforms that forward errors on the system bus as
	 * SError upon PCI configuration transactions issued when the link
	 * is down. This check is racy by definition and does not stop
	 * the system from triggering an SError if the link goes down
	 * after this check is performed.
	 */
	if (!dw_pcie_link_up(pci))
		return NULL;

	/* [한국어] 대상 버스와 장치 번호를 각각의 자리에 넣는다 */
	reg = CFG_BUS(bus->number) | CFG_DEVICE(PCI_SLOT(devfn)) |
		CFG_FUNC(PCI_FUNC(devfn));
	/* [한국어] 부모가 루트 버스가 아니면 브리지를 한 번 더 지나야 하는 접근이다 */
	if (!pci_is_root_bus(bus->parent))
		/* [한국어] Type 1 표시를 얹는다 */
		reg |= CFG_TYPE1;
	/* [한국어] 다음 접근의 대상을 하드웨어에 알린다. 이 쓰기와 아래 주소 반환 사이에
	 * 다른 CPU 가 끼어들면 엉뚱한 장치에 접근하게 되지만, PCI 코어가
	 * config 접근 전체를 pci_lock 으로 직렬화하므로 성립한다 */
	ks_pcie_app_writel(ks_pcie, CFG_SETUP, reg);

	/* [한국어] 고정된 config 창의 주소를 돌려준다. 실제 읽기와 쓰기는 PCI 코어의
	 * generic 함수가 이 주소에 한다 */
	return pp->va_cfg0_base + where;
}

/* [한국어] 하위 버스용 연산 표. Keystone 2 에서만 걸린다 */
static struct pci_ops ks_child_pcie_ops = {
	/* [한국어] 이 파일이 제공하는 것은 map_bus 하나이고, 읽기와 쓰기는 PCI 코어의
	 * generic 구현을 그대로 쓴다 */
	.map_bus = ks_pcie_other_map_bus,
	.read = pci_generic_config_read,
	.write = pci_generic_config_write,
};

/* [한국어] 루트 버스용 연산 표 */
static struct pci_ops ks_pcie_ops = {
	/* [한국어] 루트 버스는 DWC 표준 map_bus 를 그대로 쓴다 — CFG_SETUP 을 거칠 필요가
	 * 없기 때문이다 */
	.map_bus = dw_pcie_own_conf_map_bus,
	.read = pci_generic_config_read,
	.write = pci_generic_config_write,
};

/**
 * ks_pcie_link_up() - Check if link up
 * @pci: A pointer to the dw_pcie structure which holds the DesignWare PCIe host
 *	 controller driver information.
 */
/* [한국어]
 * ks_pcie_link_up - 링크가 L0 상태인지 확인한다
 *
 * @pci: DWC 컨트롤러 상태.  @return: true 면 링크가 서 있다
 *
 * dw_pcie_ops 의 link_up 콜백이다. DWC 코어가 링크 대기와 config 접근
 * 가능 여부 판정에 이것을 쓴다.
 *
 * DBI 의 PORT_DEBUG0 레지스터에서 LTSSM 상태를 뽑아 L0 인지 본다.
 * L0 은 PCIe 링크가 정상 동작 중인 상태다. 같은지(==)를 보는 것이지
 * 비트가 서 있는지를 보는 것이 아니다 — LTSSM 은 상태 번호를 담는
 * 필드이지 비트 묶음이 아니기 때문이다.
 *
 * 두 상수(PORT_LOGIC_LTSSM_STATE_MASK, PORT_LOGIC_LTSSM_STATE_L0)는
 * dwc/pcie-designware.h 에 있는 DWC 공용 정의다 — 이 컨트롤러가 표준
 * DWC IP 이므로 TI 고유 레지스터가 아니라 그쪽을 쓴다.
 * 애플리케이션 창이 아니라 DBI 를 읽는 몇 안 되는 함수 중 하나다.
 *
 * 실행 컨텍스트: 제한 없음. config 접근 경로(스핀락 보유)에서도 불리므로
 * 잠들지 않는다.
 *
 * 호출 체인:
 *   DWC 코어의 링크 대기 / ks_pcie_other_map_bus() → [이 함수]
 *     → dw_pcie_readl_dbi()
 */
static bool ks_pcie_link_up(struct dw_pcie *pci)
{
	u32 val;

	/* [한국어] DBI 의 PORT_DEBUG0 에서 LTSSM 상태를 읽는다. 애플리케이션 창이 아니라
	 * DWC 표준 레지스터를 보는 몇 안 되는 곳이다 */
	val = dw_pcie_readl_dbi(pci, PCIE_PORT_DEBUG0);
	return (val & PORT_LOGIC_LTSSM_STATE_MASK) == PORT_LOGIC_LTSSM_STATE_L0;
}

/* [한국어]
 * ks_pcie_stop_link - 링크 훈련을 멈춘다
 *
 * @pci: DWC 컨트롤러 상태.  @return: 없음
 *
 * dw_pcie_ops 의 stop_link 콜백이다. CMD_STATUS 의 LTSSM_EN_VAL 을
 * AND-NOT 으로 지운다. 읽고-고치고-쓰기라 같은 레지스터의 다른 비트
 * (OB_XLAT_EN_VAL, DBI_CS2)는 보존된다.
 *
 * ks_pcie_host_init() 이 초기화 도중에 명시적으로 부르는 점이 눈에 띈다 —
 * 애플리케이션 레지스터를 건드리기 전에 훈련을 멈춰, 설정 중에 링크가
 * 서서 상태가 흔들리는 것을 막는 것으로 보인다. 그 뒤 DWC 코어가
 * ks_pcie_start_link() 로 다시 시작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ks_pcie_host_init() 또는 DWC 코어 → [이 함수] → ks_pcie_app_writel()
 */
static void ks_pcie_stop_link(struct dw_pcie *pci)
{
	struct keystone_pcie *ks_pcie = to_keystone_pcie(pci);
	/* [한국어] 읽고-고치고-쓰기에 쓸 지역 변수 */
	u32 val;

	/* Disable Link training */
	val = ks_pcie_app_readl(ks_pcie, CMD_STATUS);
	/* [한국어] 링크 훈련 비트만 지운다. 같은 레지스터의 OB_XLAT_EN 과 DBI_CS2 는 보존된다 */
	val &= ~LTSSM_EN_VAL;
	ks_pcie_app_writel(ks_pcie, CMD_STATUS, val);
}

/* [한국어]
 * ks_pcie_start_link - 링크 훈련을 시작한다
 *
 * @pci: DWC 컨트롤러 상태.  @return: 항상 0
 *
 * dw_pcie_ops 의 start_link 콜백이다. CMD_STATUS 의 LTSSM_EN_VAL 을
 * 세운다.
 *
 * stop_link 와 달리 val 을 |= 로 갱신하지 않고 (LTSSM_EN_VAL | val) 을
 * 그대로 쓰는데, 결과는 같다.
 *
 * 언제나 0 을 돌려준다. 훈련이 실제로 성공했는지는 DWC 코어가
 * ks_pcie_link_up() 을 폴링해 판정하므로, 이 함수는 "시작 신호를 보냈다"
 * 까지만 책임진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(DWC 코어의 링크 시작 경로).
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_start_link) → [이 함수] → ks_pcie_app_writel()
 */
static int ks_pcie_start_link(struct dw_pcie *pci)
{
	struct keystone_pcie *ks_pcie = to_keystone_pcie(pci);
	/* [한국어] 읽고-고치고-쓰기에 쓸 지역 변수 */
	u32 val;

	/* Initiate Link Training */
	val = ks_pcie_app_readl(ks_pcie, CMD_STATUS);
	/* [한국어] 훈련 비트를 세워 되쓴다. stop 판과 달리 val 을 갱신하지 않고 OR 결과를
	 * 곧바로 넘기는데, 결과는 같다 */
	ks_pcie_app_writel(ks_pcie, CMD_STATUS, LTSSM_EN_VAL | val);

	return 0;
}

/* [한국어]
 * ks_pcie_quirk - 하위 장치의 최대 읽기 요청 크기를 하드웨어 한계로 낮춘다
 *
 * @dev: 방금 enable 된 PCI 장치.  @return: 없음
 *
 * DECLARE_PCI_FIXUP_ENABLE 로 등록되어, **모든** PCI 장치가 enable 될 때
 * 불린다(PCI_ANY_ID). 그래서 맨 먼저 "이 장치가 Keystone 컨트롤러 아래에
 * 있는가" 를 확인해야 한다.
 *
 * 호스트 브리지 찾기: 루트 버스에 닿을 때까지 bus->self 를 따라 올라간다.
 * 장치 자신이 루트 버스에 있으면 그 자신이 브리지다.
 *
 * 두 가지 한계를 각각 다룬다. 위 원문 영어 주석 둘이 근거를 밝힌다.
 *   1) Keystone 2(K2HK/K2E/K2L/K2G) — 하드웨어가 최대 읽기 요청 크기
 *      256바이트를 넘기지 못한다. 브리지의 vendor/device ID 가 그 넷 중
 *      하나면 하위 장치의 MRRS 를 256 으로 낮춘다.
 *   2) AM654 PG1.0 — 128바이트를 넘으면 메모리 트랜잭션이 실패한다.
 *      그런데 실리콘 리비전까지 확인해야 한다 — PID 레지스터에서 RTL
 *      버전을 뽑아 AM6_PCI_PG1_RTL_VER 와 같을 때만 적용한다.
 *      그러려면 이 컨트롤러의 struct keystone_pcie 가 필요하므로,
 *      pci_get_host_bridge_device() 로 브리지 장치를 얻고 그 부모(플랫폼
 *      장치)의 drvdata 에서 꺼낸다.
 *
 * MRRS 를 낮추면 큰 읽기가 여러 트랜잭션으로 쪼개져 대역폭이 준다.
 * NVMe 처럼 큰 DMA 읽기를 하는 장치에 직접 영향이 있는 대목이다.
 *
 * 두 검사가 배타적이지 않고 순서대로 실행되지만, 브리지가 두 목록에
 * 동시에 걸릴 수는 없으므로 문제가 되지 않는다.
 *
 * bridge 변수가 초기화 없이 쓰일 수 있는 경로가 있다 — 루트 버스가
 * 아니면서 while 루프가 한 번도 돌지 않는 경우인데, 그런 버스 구조는
 * 성립하지 않는다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 장치 enable 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_enable_device() → (PCI fixup) → [이 함수]
 *     → pci_match_id() → pcie_get_readrq() → pcie_set_readrq()
 */
static void ks_pcie_quirk(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus;
	/* [한국어] AM654 갈래에서 실리콘 리비전을 읽을 때만 쓴다 */
	struct keystone_pcie *ks_pcie;
	/* [한국어] 그 리비전을 읽으려면 이 컨트롤러를 찾아야 하고, 그 통로가 브리지 장치다 */
	struct device *bridge_dev;
	/* [한국어] 찾아 올라간 호스트 브리지 */
	struct pci_dev *bridge;
	/* [한국어] PID 레지스터 값 */
	u32 val;

	/* [한국어] Keystone 2 계열 넷의 매칭 표. class 까지 확인해 브리지인지 본다 */
	static const struct pci_device_id rc_pci_devids[] = {
		/* [한국어] K2HK */
		{ PCI_DEVICE(PCI_VENDOR_ID_TI, PCIE_RC_K2HK),
		 .class = PCI_CLASS_BRIDGE_PCI_NORMAL, .class_mask = ~0, },
		{ PCI_DEVICE(PCI_VENDOR_ID_TI, PCIE_RC_K2E),
		 .class = PCI_CLASS_BRIDGE_PCI_NORMAL, .class_mask = ~0, },
		{ PCI_DEVICE(PCI_VENDOR_ID_TI, PCIE_RC_K2L),
		 .class = PCI_CLASS_BRIDGE_PCI_NORMAL, .class_mask = ~0, },
		{ PCI_DEVICE(PCI_VENDOR_ID_TI, PCIE_RC_K2G),
		 .class = PCI_CLASS_BRIDGE_PCI_NORMAL, .class_mask = ~0, },
		{ 0, },
	};
	/* [한국어] AM654 의 매칭 표. class 마스크 표현이 위와 다른데, 두 상수의 정의가
	 * 한쪽은 이미 시프트된 값이고 다른 쪽은 아니기 때문으로 보인다.
	 * 근거가 되는 헤더가 이 트리에 없어 확인하지 못했다 */
	static const struct pci_device_id am6_pci_devids[] = {
		/* [한국어] AM654 */
		{ PCI_DEVICE(PCI_VENDOR_ID_TI, PCI_DEVICE_ID_TI_AM654X),
		 .class = PCI_CLASS_BRIDGE_PCI << 8, .class_mask = ~0, },
		{ 0, },
	};

	/* [한국어] 장치 자신이 루트 버스에 있으면 */
	if (pci_is_root_bus(bus))
		/* [한국어] 그 자신이 브리지다 */
		bridge = dev;

	/* look for the host bridge */
	while (!pci_is_root_bus(bus)) {
		/* [한국어] 그렇지 않으면 위로 올라가며 브리지를 찾는다 */
		bridge = bus->self;
		/* [한국어] 한 단계 위로 */
		bus = bus->parent;
	}

	/* [한국어] 브리지를 못 찾았으면 이 장치는 이 드라이버 소관이 아니다 */
	if (!bridge)
		return;

	/*
	 * Keystone PCI controller has a h/w limitation of
	 * 256 bytes maximum read request size.  It can't handle
	 * anything higher than this.  So force this limit on
	 * all downstream devices.
	 */
	if (pci_match_id(rc_pci_devids, bridge)) {
		/* [한국어] 현재 MRRS 가 한계를 넘는가 */
		if (pcie_get_readrq(dev) > 256) {
			/* [한국어] 낮춘다는 사실을 남긴다 */
			dev_info(&dev->dev, "limiting MRRS to 256 bytes\n");
			/* [한국어] 256바이트로 강제한다. 큰 읽기가 쪼개져 대역폭이 줄지만,
			 * 그러지 않으면 트랜잭션 자체가 실패한다 */
			pcie_set_readrq(dev, 256);
		}
	}

	/*
	 * Memory transactions fail with PCI controller in AM654 PG1.0
	 * when MRRS is set to more than 128 bytes. Force the MRRS to
	 * 128 bytes in all downstream devices.
	 */
	if (pci_match_id(am6_pci_devids, bridge)) {
		/* [한국어] 실리콘 리비전을 읽으려면 이 컨트롤러의 구조체가 필요하다 */
		bridge_dev = pci_get_host_bridge_device(dev);
		/* [한국어] 브리지 장치나 그 부모(플랫폼 장치)가 없으면 얻을 수 없다 */
		if (!bridge_dev || !bridge_dev->parent)
			return;

		/* [한국어] 플랫폼 장치의 drvdata 가 곧 struct keystone_pcie 다 —
		 * ks_pcie_probe() 가 platform_set_drvdata 로 걸어 둔 것이다 */
		ks_pcie = dev_get_drvdata(bridge_dev->parent);
		/* [한국어] 아직 세워지지 않았으면 물러난다 */
		if (!ks_pcie)
			return;

		/* [한국어] 주변장치 ID 레지스터를 읽는다 */
		val = ks_pcie_app_readl(ks_pcie, PID);
		/* [한국어] RTL 버전 필드만 남긴다 */
		val &= RTL;
		/* [한국어] 제자리로 민다 */
		val >>= RTL_SHIFT;
		/* [한국어] PG1.0 이 아니면 이 제한이 없다 */
		if (val != AM6_PCI_PG1_RTL_VER)
			return;

		/* [한국어] 현재 MRRS 가 128바이트를 넘는가 */
		if (pcie_get_readrq(dev) > 128) {
			/* [한국어] 낮춘다는 사실을 남긴다 */
			dev_info(&dev->dev, "limiting MRRS to 128 bytes\n");
			/* [한국어] 128바이트로 강제한다. Keystone 2 보다 더 엄격하다 */
			pcie_set_readrq(dev, 128);
		}
	}
}
DECLARE_PCI_FIXUP_ENABLE(PCI_ANY_ID, PCI_ANY_ID, ks_pcie_quirk);

/* [한국어]
 * ks_pcie_msi_irq_handler - MSI 체인 인터럽트 핸들러
 *
 * @desc: 상위 인터럽트 컨트롤러가 넘겨준 irq_desc.  @return: 없음
 *
 * Keystone 은 MSI 를 여덟 개의 SoC 인터럽트로 나눠 받는다. 이 함수는 그
 * 중 하나에 걸리며, 자기가 몇 번인지를 hwirq 에서 역산한다 —
 * offset = irq - ks_pcie->msi_host_irq. msi_host_irq 는
 * ks_pcie_config_msi_irq() 가 첫 번째 IRQ 의 hwirq 를 저장해 둔 값이다.
 *
 * 벡터 번호 배치가 이 함수의 핵심이고, 안쪽 원문 영어 주석이 그것을
 * 밝힌다 — MSI0 상태의 비트 0~3 이 벡터 0/8/16/24 를, MSI1 이 1/9/17/25 를
 * 뜻한다. 그래서 vector = offset + (pos << 3) 로 되살린다.
 * 같은 배치를 ack/mask/unmask 쪽에서는 reg_offset = irq % 8,
 * bit_pos = irq >> 3 으로 반대 방향으로 푼다.
 *
 * 비트를 넷만 훑는(pos < 4) 것은 상태 레지스터 하나가 벡터 넷을 담기
 * 때문이다. 여덟 레지스터 x 네 비트 = 32 벡터가 이 컨트롤러의 MSI 용량이다.
 *
 * chained_irq_enter/exit 로 감싸는 이유를 원문 주석이 밝힌다 — 체인 핸들러
 * 설치가 평범한 인터럽트 핸들러를 대체했으므로 mask/unmask 와 ack 를
 * 직접 챙겨야 한다.
 *
 * 상태 비트를 여기서 지우지 않는 점에 주의한다. 그 일은 각 벡터의
 * irq_chip 콜백인 ks_pcie_msi_irq_ack() 가 한다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   (상위 인터럽트 컨트롤러) → [이 함수]
 *     → ks_pcie_app_readl() → generic_handle_domain_irq()
 */
static void ks_pcie_msi_irq_handler(struct irq_desc *desc)
{
	unsigned int irq = desc->irq_data.hwirq;
	/* [한국어] 체인 데이터에서 되찾은 이 컨트롤러 */
	struct keystone_pcie *ks_pcie = irq_desc_get_handler_data(desc);
	/* [한국어] 여덟 MSI 인터럽트 중 자기가 몇 번째인지 역산한다.
	 * msi_host_irq 는 ks_pcie_config_msi_irq() 가 첫 번째의 hwirq 를 저장해
	 * 둔 값이고, 위에서 irq 도 hwirq 로 얻었으므로 같은 공간의 뺄셈이다 */
	u32 offset = irq - ks_pcie->msi_host_irq;
	/* [한국어] DWC 컨트롤러 상태 */
	struct dw_pcie *pci = ks_pcie->pci;
	/* [한국어] DWC 루트 포트 상태. 아래 도메인이 여기 있다 */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 로그 출력 대상 */
	struct device *dev = pci->dev;
	/* [한국어] 상위 인터럽트 컨트롤러의 chip */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] vector = 최종 벡터 번호, reg = 상태 값, pos = 비트 위치 */
	u32 vector, reg, pos;

	/* [한국어] 어느 IRQ 가 걸렸는지 디버그 로그 */
	dev_dbg(dev, "%s, irq %d\n", __func__, irq);

	/*
	 * The chained irq handler installation would have replaced normal
	 * interrupt driver handler so we need to take care of mask/unmask and
	 * ack operation.
	 */
	chained_irq_enter(chip, desc);

	reg = ks_pcie_app_readl(ks_pcie, MSI_IRQ_STATUS(offset));
	/*
	 * MSI0 status bit 0-3 shows vectors 0, 8, 16, 24, MSI1 status bit
	 * shows 1, 9, 17, 25 and so forth
	 */
	for (pos = 0; pos < 4; pos++) {
		/* [한국어] 서 있지 않은 비트는 건너뛴다 */
		if (!(reg & BIT(pos)))
			continue;

		/* [한국어] 비트 위치를 3비트 왼쪽으로 밀고 offset 을 더해 벡터 번호를 되살린다.
		 * ack/mask 쪽의 (irq % 8, irq >> 3) 계산을 정확히 되돌리는 식이다 */
		vector = offset + (pos << 3);
		/* [한국어] 어느 벡터인지 디버그 로그 */
		dev_dbg(dev, "irq: bit %d, vector %d\n", pos, vector);
		/* [한국어] DWC 가 만든 MSI 도메인에 넘긴다. 그 도메인의 바닥 chip 은
		 * ks_pcie_msi_host_init() 이 갈아 끼운 이 파일의 것이다 */
		generic_handle_domain_irq(pp->irq_domain, vector);
	}

	/* [한국어] 상위 컨트롤러 쪽 처리를 되돌린다 */
	chained_irq_exit(chip, desc);
}

/**
 * ks_pcie_intx_irq_handler() - Handle INTX interrupt
 * @desc: Pointer to irq descriptor
 *
 * Traverse through pending INTX interrupts and invoke handler for each. Also
 * takes care of interrupt controller level mask/ack operation.
 */
/* [한국어]
 * ks_pcie_intx_irq_handler - INTx 체인 인터럽트 핸들러
 *
 * @desc: 상위 인터럽트 컨트롤러가 넘겨준 irq_desc.  @return: 없음
 *
 * 위 원문 kernel-doc 이 역할을 밝힌다 — 대기 중인 INTx 를 훑어 각각의
 * 핸들러를 부르고, 인터럽트 컨트롤러 수준의 mask/ack 도 챙긴다.
 *
 * INTA~INTD 각각이 SoC 인터럽트 하나씩을 차지하므로, 이 핸들러도 네 번
 * 따로 등록된다(ks_pcie_config_intx_irq 의 루프). 자기가 몇 번인지는
 * irq_offset = irq - intx_host_irqs[0] 로 역산한다 — 네 IRQ 번호가 연속
 * 이라는 전제이며, 그 전제는 장치 트리의 나열 순서에 달려 있다.
 *
 * 실제 처리는 ks_pcie_handle_intx_irq() 에 넘긴다. 그래서 kernel-doc 이
 * 말하는 "훑는다" 는 이 함수가 아니라 그쪽에서 하는 일인데, 실제로는
 * INTx 마다 레지스터가 따로라 훑을 것이 하나뿐이다.
 *
 * chained_irq_enter/exit 의 이유는 MSI 판과 같고, 원문 주석도 같은 문구다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   (상위 인터럽트 컨트롤러) → [이 함수] → ks_pcie_handle_intx_irq()
 */
static void ks_pcie_intx_irq_handler(struct irq_desc *desc)
{
	unsigned int irq = irq_desc_get_irq(desc);
	/* [한국어] 체인 데이터에서 되찾은 이 컨트롤러 */
	struct keystone_pcie *ks_pcie = irq_desc_get_handler_data(desc);
	/* [한국어] DWC 컨트롤러 상태 */
	struct dw_pcie *pci = ks_pcie->pci;
	/* [한국어] 로그 출력 대상 */
	struct device *dev = pci->dev;
	/* [한국어] 네 INTx 중 자기가 몇 번째인지 역산한다. 네 IRQ 번호가 연속이라는
	 * 전제이며, 그 전제는 장치 트리의 나열 순서에 달려 있다 */
	u32 irq_offset = irq - ks_pcie->intx_host_irqs[0];
	/* [한국어] 상위 인터럽트 컨트롤러의 chip */
	struct irq_chip *chip = irq_desc_get_chip(desc);

	/* [한국어] 어느 INTx 인지 디버그 로그 */
	dev_dbg(dev, ": Handling INTX irq %d\n", irq);

	/*
	 * The chained irq handler installation would have replaced normal
	 * interrupt driver handler so we need to take care of mask/unmask and
	 * ack operation.
	 */
	chained_irq_enter(chip, desc);
	/* [한국어] 실제 처리는 이쪽에 넘긴다 */
	ks_pcie_handle_intx_irq(ks_pcie, irq_offset);
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * ks_pcie_config_msi_irq - 장치 트리에서 MSI IRQ 를 얻어 체인 핸들러를 건다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 0 = 성공(또는 할 일 없음), 음수 errno = 실패
 *
 * 컨트롤러 노드의 msi-interrupt-controller 자식 노드에서 IRQ 목록을 얻어,
 * 각각에 ks_pcie_msi_irq_handler() 를 체인으로 건다.
 *
 * 세 갈래로 물러난다.
 *   - CONFIG_PCI_MSI 가 꺼져 있으면 아무 일도 하지 않고 성공.
 *   - 자식 노드가 없는데 AM654 면 성공. 그 세대는 DWC 내장 MSI 를 쓰므로
 *     이 노드가 필요 없다.
 *   - 자식 노드가 없는데 Keystone 2 면 경고를 남기고 -EINVAL. 그 세대는
 *     이 노드 없이는 MSI 를 받을 수 없다.
 *
 * 첫 IRQ 의 hwirq 를 msi_host_irq 에 저장해 두는 것이 요점이다.
 * ks_pcie_msi_irq_handler() 가 그것으로 "내가 여덟 중 몇 번인가" 를
 * 역산하므로, 이 값이 없으면 벡터 번호를 복원할 수 없다.
 * 저장은 첫 바퀴에서만 한다(!ks_pcie->msi_host_irq 검사).
 *
 * of_node_put 을 성공 경로와 실패 경로 양쪽에서 부른다 —
 * of_get_child_by_name() 이 참조를 잡아 주기 때문이다.
 *
 * 에러 경로: 중간에 실패하면 이미 건 체인 핸들러를 풀지 않는다.
 * 코드는 고치지 않고 이 관찰만 적어 둔다 — probe 가 실패하면 그대로
 * 드라이버가 붙지 않으므로 실질적 문제로 이어지지는 않는다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트(host_init 을 통해).
 *
 * 호출 체인:
 *   ks_pcie_host_init() → [이 함수]
 *     → of_irq_count() → irq_of_parse_and_map()
 *     → irq_set_chained_handler_and_data()
 */
static int ks_pcie_config_msi_irq(struct keystone_pcie *ks_pcie)
{
	struct device *dev = ks_pcie->pci->dev;
	/* [한국어] 이 컨트롤러의 장치 트리 노드 */
	struct device_node *np = ks_pcie->np;
	/* [한국어] MSI 인터럽트 컨트롤러 자식 노드 */
	struct device_node *intc_np;
	/* [한국어] 첫 IRQ 의 hwirq 를 얻기 위한 중간 변수 */
	struct irq_data *irq_data;
	/* [한국어] irq_count = 자식 노드의 IRQ 개수 */
	int irq_count, irq, ret, i;

	/* [한국어] MSI 를 쓰지 않는 커널이면 할 일이 없다 */
	if (!IS_ENABLED(CONFIG_PCI_MSI))
		return 0;

	/* [한국어] MSI IRQ 들이 나열된 자식 노드를 찾는다 */
	intc_np = of_get_child_by_name(np, "msi-interrupt-controller");
	/* [한국어] 그 노드가 없는 경우 */
	if (!intc_np) {
		/* [한국어] AM654 는 DWC 내장 MSI 를 쓰므로 이 노드가 필요 없다 */
		if (ks_pcie->is_am6)
			return 0;
		/* [한국어] Keystone 2 는 이 노드 없이 MSI 를 받을 수 없다 */
		dev_warn(dev, "msi-interrupt-controller node is absent\n");
		return -EINVAL;
	}

	/* [한국어] 노드에 나열된 IRQ 개수를 센다 */
	irq_count = of_irq_count(intc_np);
	/* [한국어] 하나도 없으면 잘못된 장치 트리다 */
	if (!irq_count) {
		/* [한국어] 무엇이 문제인지 남긴다 */
		dev_err(dev, "No IRQ entries in msi-interrupt-controller\n");
		ret = -EINVAL;
		goto err;
	}

	/* [한국어] IRQ 마다 체인 핸들러를 건다 */
	for (i = 0; i < irq_count; i++) {
		/* [한국어] 장치 트리의 i 번째 인터럽트를 커널 IRQ 로 매핑한다 */
		irq = irq_of_parse_and_map(intc_np, i);
		/* [한국어] 매핑 실패 */
		if (!irq) {
			ret = -EINVAL;
			goto err;
		}

		/* [한국어] 첫 바퀴에서만 기준값을 저장한다 */
		if (!ks_pcie->msi_host_irq) {
			/* [한국어] 커널 가상 IRQ 가 아니라 하드웨어 번호가 필요하다 */
			irq_data = irq_get_irq_data(irq);
			/* [한국어] 얻지 못하면 벡터 번호를 복원할 수 없다 */
			if (!irq_data) {
				ret = -EINVAL;
				goto err;
			}
			/* [한국어] 이 값이 ks_pcie_msi_irq_handler() 의 offset 계산 기준이 된다 */
			ks_pcie->msi_host_irq = irq_data->hwirq;
		}

		/* [한국어] MSI 체인 핸들러를 건다 */
		irq_set_chained_handler_and_data(irq, ks_pcie_msi_irq_handler,
						 ks_pcie);
	}

	of_node_put(intc_np);
	return 0;

err:
	of_node_put(intc_np);
	return ret;
}

/* [한국어]
 * ks_pcie_config_intx_irq - INTx IRQ 를 얻고 전용 도메인을 만든다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 0 = 성공(또는 할 일 없음), 음수 errno = 실패
 *
 * MSI 판과 구조가 비슷하지만 하나가 더 있다 — 이쪽은 **자기 IRQ 도메인을
 * 직접 만든다**. MSI 는 DWC 의 dw_pcie_allocate_domains() 를 빌려 썼지만,
 * INTx 는 DWC 가 관여하지 않으므로 처음부터 끝까지 이 파일의 일이다.
 *
 * 절차:
 *   1) legacy-interrupt-controller 자식 노드를 찾는다. 없으면
 *      AM654 는 성공으로 물러난다 — 안쪽 원문 영어 주석이 이유를 밝힌다:
 *      AM6 에서는 INTx 가 에지 인터럽트로 모델링되어 있어 당분간
 *      비활성으로 둔다. Keystone 2 면 경고와 -EINVAL 이다.
 *   2) IRQ 목록을 얻어 각각에 ks_pcie_intx_irq_handler() 를 체인으로 걸고,
 *      번호를 intx_host_irqs[] 에 순서대로 저장한다. 그 순서가
 *      핸들러의 irq_offset 역산에 쓰인다.
 *   3) PCI_NUM_INTX(4) 크기의 선형 도메인을 만든다. host_data 로 NULL 을
 *      넘기는데, 이 도메인의 irq_chip 콜백이 모두 비어 있어 쓸 일이 없다.
 *   4) 마지막에 IRQ_ENABLE_SET(i) 에 INTx_EN 을 써서 넷을 모두 켠다.
 *      이후 개별로 끄고 켜지 않는다 — irq_chip 의 mask/unmask 가 빈
 *      함수인 것과 짝이 되는 사실이다.
 *
 * err 라벨이 성공 경로에서도 지나가는 구조다(4번 루프 뒤에 곧바로 이어진다).
 * ret 이 0 인 채로 of_node_put 을 하고 0 을 돌려주므로 동작은 맞지만,
 * 라벨 이름과 흐름이 어긋나 보인다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트(host_init 을 통해).
 *
 * 호출 체인:
 *   ks_pcie_host_init() → [이 함수]
 *     → irq_of_parse_and_map() → irq_domain_create_linear()
 *     → ks_pcie_app_writel()
 */
static int ks_pcie_config_intx_irq(struct keystone_pcie *ks_pcie)
{
	struct device *dev = ks_pcie->pci->dev;
	/* [한국어] 만들 INTx 도메인 */
	struct irq_domain *intx_irq_domain;
	/* [한국어] 이 컨트롤러의 장치 트리 노드 */
	struct device_node *np = ks_pcie->np;
	/* [한국어] INTx 인터럽트 컨트롤러 자식 노드 */
	struct device_node *intc_np;
	/* [한국어] ret 을 0 으로 초기화하는 것이 요점 — 아래 err 라벨을 성공 경로도
	 * 지나가기 때문이다 */
	int irq_count, irq, ret = 0, i;

	/* [한국어] INTx IRQ 들이 나열된 자식 노드를 찾는다 */
	intc_np = of_get_child_by_name(np, "legacy-interrupt-controller");
	if (!intc_np) {
		/*
		 * Since INTX interrupts are modeled as edge-interrupts in
		 * AM6, keep it disabled for now.
		 */
		if (ks_pcie->is_am6)
			return 0;
		/* [한국어] Keystone 2 는 이 노드가 있어야 INTx 를 받는다 */
		dev_warn(dev, "legacy-interrupt-controller node is absent\n");
		return -EINVAL;
	}

	/* [한국어] 노드에 나열된 IRQ 개수를 센다 */
	irq_count = of_irq_count(intc_np);
	/* [한국어] 하나도 없으면 잘못된 장치 트리다 */
	if (!irq_count) {
		/* [한국어] 무엇이 문제인지 남긴다 */
		dev_err(dev, "No IRQ entries in legacy-interrupt-controller\n");
		ret = -EINVAL;
		goto err;
	}

	/* [한국어] INTA~INTD 각각에 체인 핸들러를 건다 */
	for (i = 0; i < irq_count; i++) {
		/* [한국어] 장치 트리의 i 번째 인터럽트를 매핑한다 */
		irq = irq_of_parse_and_map(intc_np, i);
		/* [한국어] 매핑 실패 */
		if (!irq) {
			ret = -EINVAL;
			goto err;
		}
		/* [한국어] 번호를 순서대로 저장한다. 핸들러가 [0] 을 기준으로 역산한다 */
		ks_pcie->intx_host_irqs[i] = irq;

		irq_set_chained_handler_and_data(irq,
						 ks_pcie_intx_irq_handler,
						 ks_pcie);
	}

	/* [한국어] PCI_NUM_INTX(4) 크기의 선형 도메인을 만든다. host_data 로 NULL 을
	 * 넘기는데, 이 도메인의 irq_chip 콜백이 모두 비어 있어 쓸 일이 없다 */
	intx_irq_domain = irq_domain_create_linear(of_fwnode_handle(intc_np), PCI_NUM_INTX,
					&ks_pcie_intx_irq_domain_ops, NULL);
	/* [한국어] 도메인 생성 실패 */
	if (!intx_irq_domain) {
		/* [한국어] 무엇이 실패했는지 남긴다 */
		dev_err(dev, "Failed to add irq domain for INTX irqs\n");
		ret = -EINVAL;
		goto err;
	}
	/* [한국어] 핸들러가 이 도메인으로 인터럽트를 넘긴다 */
	ks_pcie->intx_irq_domain = intx_irq_domain;

	/* [한국어] INTx 넷을 모두 켠다 */
	for (i = 0; i < PCI_NUM_INTX; i++)
		/* [한국어] 이후 개별로 끄고 켜지 않는다 — irq_chip 의 mask/unmask 가 빈 함수인
		 * 것과 짝이 되는 사실이다 */
		ks_pcie_app_writel(ks_pcie, IRQ_ENABLE_SET(i), INTx_EN);

err:
	of_node_put(intc_np);
	return ret;
}

/* [한국어]
 * ks_pcie_init_id - syscon 에서 읽은 벤더/장치 ID 를 DBI 에 써 넣는다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 0 = 성공, 음수 errno = 실패
 *
 * DWC IP 의 기본 벤더/장치 ID 는 TI 의 것이 아니므로, SoC 의 설정
 * 레지스터(syscon)에 박혀 있는 진짜 ID 를 읽어 DBI 에 덮어쓴다.
 * 이 값이 곧 하위 소프트웨어가 lspci 로 보게 될 ID 이고,
 * ks_pcie_quirk() 가 "이 브리지가 Keystone 인가" 를 판정하는 근거이기도 하다.
 *
 * ti,syscon-pcie-id phandle 로 regmap 을 얻는다. 그 뒤 같은 phandle 을
 * of_parse_phandle_with_fixed_args 로 다시 파싱해 오프셋 인자를 얻는데,
 * 그 결과를 확인하지 않고 실패하면 offset 을 0 으로 둔다.
 * 원문 영어 주석이 이유를 밝힌다 — 옛 장치 트리 호환성을 지키려는 것이다.
 * 옛 바인딩에는 인자가 없었으므로, 없으면 0 을 쓴다.
 *
 * 읽은 32비트 하나에 두 ID 가 들어 있다 — 하위 16비트가 벤더,
 * 상위 16비트가 장치다.
 *
 * DBI 에 쓰기 전후로 dw_pcie_dbi_ro_wr_en/dis 를 부르는 것이 중요하다.
 * 벤더/장치 ID 는 PCI 규격상 읽기 전용이므로, DWC IP 의 "읽기 전용
 * 레지스터 쓰기 허용" 스위치를 잠시 켜야 한다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트(host_init 을 통해).
 *
 * 호출 체인:
 *   ks_pcie_host_init() → [이 함수]
 *     → syscon_regmap_lookup_by_phandle() → regmap_read()
 *     → dw_pcie_dbi_ro_wr_en() → dw_pcie_writew_dbi()
 */
static int ks_pcie_init_id(struct keystone_pcie *ks_pcie)
{
	int ret;
	/* [한국어] syscon 에서 읽을 32비트. 두 ID 가 함께 들어 있다 */
	unsigned int id;
	/* [한국어] syscon 레지스터 맵 */
	struct regmap *devctrl_regs;
	/* [한국어] DWC 컨트롤러 상태 */
	struct dw_pcie *pci = ks_pcie->pci;
	/* [한국어] 로그 출력 대상 */
	struct device *dev = pci->dev;
	/* [한국어] 이 컨트롤러의 장치 트리 노드 */
	struct device_node *np = dev->of_node;
	/* [한국어] phandle 인자를 받을 구조체 */
	struct of_phandle_args args;
	/* [한국어] 레지스터 오프셋. 파싱에 실패하면 0 으로 남는다 */
	unsigned int offset = 0;

	/* [한국어] SoC 의 장치 ID 레지스터를 syscon 으로 찾는다 */
	devctrl_regs = syscon_regmap_lookup_by_phandle(np, "ti,syscon-pcie-id");
	/* [한국어] 없으면 오류를 그대로 올린다. 모드 설정과 달리 여기서는 실패로 본다 */
	if (IS_ERR(devctrl_regs))
		return PTR_ERR(devctrl_regs);

	/* Do not error out to maintain old DT compatibility */
	ret = of_parse_phandle_with_fixed_args(np, "ti,syscon-pcie-id", 1, 0, &args);
	/* [한국어] 파싱에 성공했을 때만 */
	if (!ret)
		/* [한국어] 오프셋 인자를 쓴다. 실패하면 0 이 유지된다 —
		 * 원문 영어 주석대로 옛 장치 트리 호환성을 위해서다 */
		offset = args.args[0];

	/* [한국어] 그 오프셋에서 32비트를 읽는다 */
	ret = regmap_read(devctrl_regs, offset, &id);
	/* [한국어] 읽기 실패 */
	if (ret)
		return ret;

	dw_pcie_dbi_ro_wr_en(pci);
	/* [한국어] 하위 16비트가 벤더 ID 다 */
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, id & PCIE_VENDORID_MASK);
	/* [한국어] 상위 16비트가 장치 ID 다. 이 값이 lspci 에 보이고,
	 * ks_pcie_quirk() 가 브리지를 알아보는 근거가 된다 */
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, id >> PCIE_DEVICEID_SHIFT);
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

/* [한국어]
 * ks_pcie_host_init - RC 모드 초기화의 본체(DWC 가 되부른다)
 *
 * @pp: DWC 의 루트 포트 상태.  @return: 0 = 성공, 음수 errno = 실패
 *
 * dw_pcie_host_ops 의 init 콜백이며, RC 경로의 중심이다.
 * dw_pcie_host_init() 이 DWC 쪽 준비를 마친 뒤 이것을 부른다.
 *
 * 순서에 각각 이유가 있다.
 *   1) bridge->ops 를 이 파일의 것으로 바꾼다. 루트 버스는 DWC 표준
 *      map_bus 를 쓰지만(dw_pcie_own_conf_map_bus), 하위 버스는
 *      Keystone 2 에서만 이 파일의 ks_pcie_other_map_bus 를 쓴다.
 *      AM654 는 child_ops 를 걸지 않아 DWC 표준 경로로 간다.
 *   2) INTx 와 MSI 를 설정한다. 둘 다 장치 트리 자식 노드에 의존하므로
 *      실패할 수 있고, 실패하면 그대로 올린다.
 *   3) ks_pcie_stop_link() 로 훈련을 멈춘다. 아래 애플리케이션 레지스터를
 *      건드리는 동안 링크가 서서 상태가 흔들리는 것을 막는 것으로 보인다.
 *   4) ks_pcie_setup_rc_app_regs() 로 inbound BAR 를 끄고 outbound 창을 깐다.
 *   5) PCI_IO_BASE 에 32비트 IO 범위 종류를 쓴다. dw_pcie_writel_dbi 가
 *      아니라 writew 를 dbi_base 에 직접 하는 점이 눈에 띈다 —
 *      16비트 접근이 필요해서로 보이며, 코드는 고치지 않고 이 사실만 적어 둔다.
 *   6) ks_pcie_init_id() 로 진짜 벤더/장치 ID 를 써 넣는다.
 *
 * 훈련을 다시 시작하는 코드가 여기 없다 — 그 일은 DWC 코어가
 * ks_pcie_start_link() 를 불러서 한다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ks_pcie_probe() → dw_pcie_host_init() → pp->ops->init → [이 함수]
 *     → ks_pcie_config_intx_irq() → ks_pcie_config_msi_irq()
 *     → ks_pcie_stop_link() → ks_pcie_setup_rc_app_regs() → ks_pcie_init_id()
 */
static int ks_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 이 파일의 구조체 */
	struct keystone_pcie *ks_pcie = to_keystone_pcie(pci);
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* [한국어] 루트 버스용 연산 표를 건다. DWC 가 기본으로 걸어 둔 것을 덮어쓴다 */
	pp->bridge->ops = &ks_pcie_ops;
	/* [한국어] AM654 가 아닐 때만 */
	if (!ks_pcie->is_am6)
		/* [한국어] 하위 버스용 연산 표를 건다. AM654 는 걸지 않아 DWC 표준 경로로 간다 */
		pp->bridge->child_ops = &ks_child_pcie_ops;

	/* [한국어] INTx 를 먼저 설정한다 */
	ret = ks_pcie_config_intx_irq(ks_pcie);
	/* [한국어] 실패하면 그대로 올린다 */
	if (ret)
		return ret;

	/* [한국어] 그 다음 MSI */
	ret = ks_pcie_config_msi_irq(ks_pcie);
	/* [한국어] 실패하면 그대로 올린다 */
	if (ret)
		return ret;

	ks_pcie_stop_link(pci);
	/* [한국어] inbound BAR 를 끄고 outbound 창을 깐다 */
	ret = ks_pcie_setup_rc_app_regs(ks_pcie);
	/* [한국어] 실패하면 그대로 올린다 */
	if (ret)
		return ret;

	/* [한국어] IO 범위 종류를 32비트로 설정한다. 하위와 상위 바이트에 같은 값을
	 * 넣는다. dw_pcie_writel_dbi 가 아니라 writew 를 dbi_base 에 직접 하는
	 * 점이 눈에 띄는데, 16비트 접근이 필요해서로 보인다 */
	writew(PCI_IO_RANGE_TYPE_32 | (PCI_IO_RANGE_TYPE_32 << 8),
			pci->dbi_base + PCI_IO_BASE);

	/* [한국어] 진짜 벤더/장치 ID 를 써 넣는다 */
	ret = ks_pcie_init_id(ks_pcie);
	/* [한국어] 실패하면 그대로 올린다 */
	if (ret < 0)
		return ret;

	return 0;
}

/* [한국어] Keystone 2 의 RC 콜백 표 */
static const struct dw_pcie_host_ops ks_pcie_host_ops = {
	/* [한국어] 초기화 콜백 */
	.init = ks_pcie_host_init,
	.msi_init = ks_pcie_msi_host_init,
};

/* [한국어] AM654 의 RC 콜백 표. 위와 달리 msi_init 이 없다 —
 * 그 세대는 DWC 내장 MSI 를 그대로 쓴다는 뜻이다 */
static const struct dw_pcie_host_ops ks_pcie_am654_host_ops = {
	.init = ks_pcie_host_init,
};

/* [한국어]
 * ks_pcie_err_irq_handler - 오류 IRQ 의 최상위 핸들러
 *
 * @irq: IRQ 번호(쓰지 않는다).  @priv: struct keystone_pcie
 * @return: ks_pcie_handle_error_irq() 가 돌려준 값
 *
 * devm_request_irq(IRQF_SHARED) 로 등록된 보통의 핸들러다. 하는 일은
 * ks_pcie_handle_error_irq() 를 그대로 부르는 것뿐이다.
 *
 * 둘로 나눈 이유는 서명이 다르기 때문이다 — 인터럽트 핸들러는
 * (int, void *) 를 받아야 하지만, 실제 처리 함수는 struct keystone_pcie
 * 하나만 있으면 된다.
 *
 * IRQF_SHARED 이므로 다른 장치의 인터럽트에도 불릴 수 있고, 그때
 * ks_pcie_handle_error_irq() 가 상태 레지스터를 보고 IRQ_NONE 을 돌려준다.
 *
 * RC 와 EP 양쪽에서 쓰인다 — ks_pcie_probe() 가 모드를 가르기 전에
 * 등록하기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   (하드웨어 인터럽트) → [이 함수] → ks_pcie_handle_error_irq()
 */
static irqreturn_t ks_pcie_err_irq_handler(int irq, void *priv)
{
	struct keystone_pcie *ks_pcie = priv;

	return ks_pcie_handle_error_irq(ks_pcie);
}

/* [한국어]
 * ks_pcie_am654_write_dbi2 - DBI_CS2 모드로 전환해 BAR 마스크에 쓴다
 *
 * @pci: DWC 컨트롤러 상태.  @base: 쓸 기준 주소.  @reg: 오프셋
 * @size: 접근 크기.  @val: 쓸 값
 * @return: 없음
 *
 * dw_pcie_ops 의 write_dbi2 콜백이다. DWC 코어가 BAR 크기 마스크를 쓸 때
 * 이것을 부른다.
 *
 * 보통의 DWC 구현은 별도의 dbi2 창에 쓰지만, 이 하드웨어는 dbi2 창이
 * 따로 없고 CS2 모드로 전환해 같은 창에 겹쳐 접근한다. 그래서 probe 에서
 * pci->dbi_base2 를 dbi_base 와 같은 값으로 둔다.
 *
 * set → 쓰기 → clear 세 줄이 전부다. 두 모드 전환 함수가 각각 바쁜
 * 대기를 하므로 이 콜백은 생각보다 비싸다.
 *
 * 이름에 am654 가 붙어 있지만 dw_pcie_ops 는 세대 공용
 * (ks_pcie_dw_pcie_ops)이라 Keystone 2 에서도 등록된다. 다만 실제로
 * write_dbi2 를 부르는 것은 EP 경로의 BAR 설정이고 EP 는 AM654 에만
 * 있으므로, Keystone 2 에서는 불리지 않는 것으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 바쁜 대기를 한다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_write_dbi2) → [이 함수]
 *     → ks_pcie_set_dbi_mode() → dw_pcie_write() → ks_pcie_clear_dbi_mode()
 */
static void ks_pcie_am654_write_dbi2(struct dw_pcie *pci, void __iomem *base,
				     u32 reg, size_t size, u32 val)
{
	struct keystone_pcie *ks_pcie = to_keystone_pcie(pci);

	ks_pcie_set_dbi_mode(ks_pcie);
	/* [한국어] CS2 모드가 켜진 상태에서 실제 쓰기를 한다. 크기를 인자로 받아
	 * 8/16/32비트를 모두 다룬다 */
	dw_pcie_write(base + reg, size, val);
	ks_pcie_clear_dbi_mode(ks_pcie);
}

/* [한국어] DWC 코어에 주는 컨트롤러 조작 표. RC/EP 공용이다 */
static const struct dw_pcie_ops ks_pcie_dw_pcie_ops = {
	/* [한국어] 링크 훈련 시작 */
	.start_link = ks_pcie_start_link,
	.stop_link = ks_pcie_stop_link,
	.link_up = ks_pcie_link_up,
	.write_dbi2 = ks_pcie_am654_write_dbi2,
};

/* [한국어]
 * ks_pcie_am654_ep_init - EP 모드의 초기화(DWC 가 되부른다)
 *
 * @ep: DWC 의 엔드포인트 상태.  @return: 없음
 *
 * dw_pcie_ep_ops 의 init 콜백이며, EP 경로에서 이 파일이 하드웨어를
 * 건드리는 유일한 초기화 지점이다.
 *
 * 두 가지를 한다.
 *   1) ep->page_size 를 AM654_WIN_SIZE(64KB)로 정한다. EPC 코어가 outbound
 *      창을 이 단위로 잘라 쓰므로, 이 값이 곧 매핑 granularity 다.
 *      아래 epc_features 의 .align 과 같은 값이어야 앞뒤가 맞는다.
 *   2) BAR0 를 고정 설정한다. dbi2(즉 CS2 모드)에 크기 마스크로
 *      APP_ADDR_SPACE_0 - 1 을, dbi 에 "32비트 메모리 BAR" 플래그를 쓴다.
 *      APP_ADDR_SPACE_0 이 16KB 이므로 BAR0 가 16KB 짜리 메모리 창이 된다.
 *      그 창이 애플리케이션 레지스터를 호스트에게 노출하는 통로로 보이나,
 *      근거가 되는 TI 문서는 이 트리에 없다.
 *
 * BAR0 를 여기서 못 박기 때문에 아래 epc_features 가 BAR_0 를
 * BAR_RESERVED 로 표시한다 — EPF 드라이버가 그 BAR 를 쓰지 못하게 하는 것이다.
 *
 * dw_pcie_writel_dbi2() 는 위 ks_pcie_am654_write_dbi2() 콜백을 거쳐
 * CS2 모드 전환을 동반한다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트(DWC EP 코어가 되부른다).
 *
 * 호출 체인:
 *   ks_pcie_probe() → dw_pcie_ep_init() → ep->ops->init → [이 함수]
 *     → dw_pcie_writel_dbi2() → dw_pcie_writel_dbi()
 */
static void ks_pcie_am654_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] BAR0 플래그 값을 담을 지역 변수 */
	int flags;

	/* [한국어] EPC 코어가 outbound 창을 이 단위로 잘라 쓴다.
	 * 아래 epc_features 의 align 과 같은 값이어야 앞뒤가 맞는다 */
	ep->page_size = AM654_WIN_SIZE;
	/* [한국어] 32비트 메모리 BAR 로 표시한다 */
	flags = PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_32;
	/* [한국어] 크기 마스크를 16KB - 1 로 정한다. dbi2 쓰기라
	 * ks_pcie_am654_write_dbi2() 를 거쳐 CS2 모드 전환이 동반된다 */
	dw_pcie_writel_dbi2(pci, PCI_BASE_ADDRESS_0, APP_ADDR_SPACE_0 - 1);
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, flags);
}

/* [한국어]
 * ks_pcie_am654_raise_intx_irq - EP 로서 INTx 를 호스트에 보낸다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 없음
 *
 * EP 모드에서 호스트에게 레거시 인터럽트를 올리는 경로다. 호스트 쪽
 * ks_pcie_handle_intx_irq() 와 정확히 반대 방향이다.
 *
 * 먼저 자기 config 의 Interrupt Pin 을 읽어 어느 INTx 를 쓸지 알아낸다.
 * 0 이면 INTx 를 쓰지 않는다는 뜻이고, 4 를 넘으면 잘못된 값이므로
 * 둘 다 조용히 돌아선다.
 *
 * 그 다음 네 번의 레지스터 쓰기로 펄스를 만든다.
 *   1) 해당 핀의 LEGACY_IRQ_ENABLE_SET 에 INT_ENABLE — 그 핀을 연다.
 *   2) PCIE_EP_IRQ_SET 에 INT_ENABLE — 인터럽트를 올린다(assert).
 *   3) mdelay(1) 로 1밀리초 유지.
 *   4) PCIE_EP_IRQ_CLR 로 내리고, ENABLE_CLR 로 핀을 닫는다.
 *
 * INTx 는 레벨 트리거이므로 올렸다 내리는 펄스를 만들어야 호스트가
 * 한 번의 인터럽트로 인식한다. 1밀리초를 유지하는 이유가 코드에는
 * 적혀 있지 않다 — 짝이 되는 rockchip EP 구현에는 TRM 이 AHB 클럭 몇
 * 사이클을 요구한다는 주석이 있으나, 이 파일에는 그런 근거가 없다.
 * 근거가 되는 TI 문서는 이 트리에 없다.
 *
 * mdelay 는 바쁜 대기라 1밀리초 동안 CPU 를 붙든다. 이 함수가 EPC 코어의
 * raise_irq 경로에서 불리므로 프로세스 컨텍스트이고, msleep 도 가능했을
 * 자리다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 1밀리초 바쁜 대기.
 *
 * 호출 체인:
 *   (EPC 코어) → ks_pcie_am654_raise_irq() → [이 함수] → ks_pcie_app_writel()
 */
static void ks_pcie_am654_raise_intx_irq(struct keystone_pcie *ks_pcie)
{
	struct dw_pcie *pci = ks_pcie->pci;
	/* [한국어] 자기 config 의 Interrupt Pin 값 */
	u8 int_pin;

	/* [한국어] 어느 INTx 를 쓸지 자기 config 에서 읽는다 */
	int_pin = dw_pcie_readb_dbi(pci, PCI_INTERRUPT_PIN);
	/* [한국어] 0 이면 INTx 를 쓰지 않는다는 뜻이고, 4 를 넘으면 잘못된 값이다 */
	if (int_pin == 0 || int_pin > 4)
		return;

	/* [한국어] 그 핀을 연다. 인자가 1~4 로 들어오는 점에 주의 —
	 * 레지스터 매크로가 (n) - 1 로 0 기반으로 되돌린다 */
	ks_pcie_app_writel(ks_pcie, PCIE_LEGACY_IRQ_ENABLE_SET(int_pin),
			   INT_ENABLE);
	/* [한국어] 인터럽트를 올린다(assert) */
	ks_pcie_app_writel(ks_pcie, PCIE_EP_IRQ_SET, INT_ENABLE);
	mdelay(1);
	/* [한국어] 내린다(deassert). 그 사이의 1밀리초가 펄스 폭이다 */
	ks_pcie_app_writel(ks_pcie, PCIE_EP_IRQ_CLR, INT_ENABLE);
	/* [한국어] 핀을 닫는다. 올렸다 내리는 펄스를 만들어야 호스트가 한 번의
	 * 인터럽트로 인식한다 */
	ks_pcie_app_writel(ks_pcie, PCIE_LEGACY_IRQ_ENABLE_CLR(int_pin),
			   INT_ENABLE);
}

/* [한국어]
 * ks_pcie_am654_raise_irq - EP 로서 인터럽트를 호스트에 보낸다
 *
 * @ep: DWC 의 엔드포인트 상태.  @func_no: 물리 기능 번호
 * @type: PCI_IRQ_INTX / PCI_IRQ_MSI / PCI_IRQ_MSIX
 * @interrupt_num: MSI/MSI-X 의 벡터 번호
 * @return: 0 = 성공, -EINVAL = 알 수 없는 종류
 *
 * dw_pcie_ep_ops 의 raise_irq 콜백이다. EPF 드라이버가 호스트를 깨우려 할
 * 때 EPC 코어를 거쳐 여기로 온다.
 *
 * 세 갈래 중 INTx 만 이 파일이 직접 구현하고, MSI 와 MSI-X 는 DWC 공용
 * 구현에 그대로 넘긴다. TI 가 덧붙인 애플리케이션 레지스터가 필요한 것이
 * INTx 뿐이기 때문이다 — MSI 는 결국 메모리 쓰기 TLP 이므로 DWC 의 표준
 * outbound 경로로 처리된다.
 *
 * 아래 epc_features 가 msi_capable 과 msix_capable 을 모두 true 로 두므로
 * 세 갈래가 전부 실제로 쓰일 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(EPC 코어의 raise_irq 경로).
 * INTx 갈래는 1밀리초 바쁜 대기를 한다.
 *
 * 호출 체인:
 *   (EPF 드라이버) → pci_epc_raise_irq() → dw_pcie_ep_raise_irq
 *     → ep->ops->raise_irq → [이 함수]
 *     → ks_pcie_am654_raise_intx_irq() 또는 dw_pcie_ep_raise_msi_irq()
 */
static int ks_pcie_am654_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				   unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 이 파일의 구조체. INTx 갈래에서만 쓰인다 */
	struct keystone_pcie *ks_pcie = to_keystone_pcie(pci);

	/* [한국어] 인터럽트 종류에 따라 갈린다 */
	switch (type) {
	/* [한국어] 레거시 INTx — 이 파일이 직접 구현한다 */
	case PCI_IRQ_INTX:
		ks_pcie_am654_raise_intx_irq(ks_pcie);
		break;
	/* [한국어] MSI — DWC 공용 구현에 넘긴다. 결국 메모리 쓰기 TLP 이므로
	 * TI 고유 레지스터가 필요 없다 */
	case PCI_IRQ_MSI:
		dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
		break;
	/* [한국어] MSI-X — 역시 DWC 공용 구현 */
	case PCI_IRQ_MSIX:
		dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
		break;
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
		return -EINVAL;
	}

	return 0;
}

/* [한국어] 이 EP 컨트롤러의 능력 표. EPF 드라이버가 BAR 를 어떻게 쓸 수 있는지
 * 이 값을 보고 정한다 */
static const struct pci_epc_features ks_pcie_am654_epc_features = {
	DWC_EPC_COMMON_FEATURES,
	.msi_capable = true,
	.msix_capable = true,
	/*
	 * TODO: This driver is the only DWC glue driver that had BAR_RESERVED
	 * BARs, but did not call dw_pcie_ep_reset_bar() for the reserved BARs.
	 *
	 * To not change the existing behavior, these BARs were not migrated to
	 * BAR_DISABLED. If this driver wants the BAR_RESERVED BARs to be
	 * disabled, it should migrate them to BAR_DISABLED.
	 *
	 * If they actually should be enabled, then the driver must also define
	 * what is behind these reserved BARs, see the definition of struct
	 * pci_epc_bar_rsvd_region.
	 */
	.bar[BAR_0] = { .type = BAR_RESERVED, },
	.bar[BAR_1] = { .type = BAR_RESERVED, },
	.bar[BAR_2] = { .type = BAR_RESIZABLE, },
	.bar[BAR_3] = { .type = BAR_FIXED, .fixed_size = SZ_64K, },
	.bar[BAR_4] = { .type = BAR_FIXED, .fixed_size = 256, },
	.bar[BAR_5] = { .type = BAR_RESIZABLE, },
	.align = SZ_64K,
};

/* [한국어] 반환형이 앞 줄에 따로 있는 형태다 */
static const struct pci_epc_features*
ks_pcie_am654_get_features(struct dw_pcie_ep *ep)
{
	/* [한국어] 정적 구조체를 그대로 돌려준다 */
	return &ks_pcie_am654_epc_features;
}

/* [한국어] AM654 의 EP 콜백 표. Keystone 2 에는 EP 모드가 없다 */
static const struct dw_pcie_ep_ops ks_pcie_am654_ep_ops = {
	/* [한국어] EP 초기화 콜백 */
	.init = ks_pcie_am654_ep_init,
	.raise_irq = ks_pcie_am654_raise_irq,
	.get_features = &ks_pcie_am654_get_features,
};

/* [한국어]
 * ks_pcie_disable_phy - 레인별 PHY 를 모두 끈다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 없음
 *
 * 레인 수만큼 역순으로 돌며 power_off 뒤 exit 를 부른다.
 * 켜는 쪽(ks_pcie_enable_phy)이 init 뒤 power_on 순이므로 정확한 역순이다.
 *
 * while (num_lanes--) 로 감소시키며 도는 것이 마지막 인덱스부터 0 까지를
 * 훑는 관용구다.
 *
 * probe 실패 경로와 remove 양쪽에서 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PHY 조작이 잠들 수 있다.
 *
 * 호출 체인:
 *   ks_pcie_probe()의 실패 경로 / ks_pcie_remove() → [이 함수]
 */
static void ks_pcie_disable_phy(struct keystone_pcie *ks_pcie)
{
	int num_lanes = ks_pcie->num_lanes;

	/* [한국어] 레인 수만큼 역순으로 돈다. 켜는 쪽이 init 뒤 power_on 순이므로
	 * 끄는 쪽은 power_off 뒤 exit 로 정확한 역순이다 */
	while (num_lanes--) {
		phy_power_off(ks_pcie->phy[num_lanes]);
		phy_exit(ks_pcie->phy[num_lanes]);
	}
}

/* [한국어]
 * ks_pcie_enable_phy - 레인별 PHY 를 순서대로 켠다
 *
 * @ks_pcie: 이 컨트롤러.  @return: 0 = 성공, 음수 errno = 실패
 *
 * 레인마다 reset → init → power_on 세 단계를 밟는다. reset 을 먼저 하는
 * 것이 이 드라이버의 특징이다 — 이전 상태가 남아 있을 수 있는 PHY 를
 * 알려진 상태로 되돌린 뒤 초기화한다.
 *
 * 에러 처리가 두 겹이다.
 *   - power_on 이 실패하면 그 레인의 phy_exit 를 먼저 부른 뒤 공통
 *     되돌리기로 간다. init 은 성공했으므로 짝을 맞춰야 하기 때문이다.
 *   - err_phy 라벨은 --i 로 이미 성공한 레인들만 역순으로 되돌린다.
 *     실패한 레인 자신은 위에서 처리했거나(power_on) 아직 init 전이라
 *     (reset/init 실패) 되돌릴 것이 없다.
 *
 * probe 에서 이 함수를 부르기 전후로 phy_pm_runtime_get_sync/put_sync 를
 * 레인마다 감싸는 점이 눈에 띈다 — PHY 의 런타임 PM 참조를 잡아 둔 채
 * 초기화한다는 뜻이다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   ks_pcie_probe() → [이 함수] → phy_reset() → phy_init() → phy_power_on()
 */
static int ks_pcie_enable_phy(struct keystone_pcie *ks_pcie)
{
	int i;
	/* [한국어] 각 단계의 결과 */
	int ret;
	/* [한국어] 레인 수 */
	int num_lanes = ks_pcie->num_lanes;

	/* [한국어] 레인마다 순서대로 켠다 */
	for (i = 0; i < num_lanes; i++) {
		/* [한국어] 먼저 리셋한다. 이전 상태가 남아 있을 수 있는 PHY 를 알려진 상태로
		 * 되돌리는 것이 이 드라이버의 특징이다 */
		ret = phy_reset(ks_pcie->phy[i]);
		/* [한국어] 실패하면 공통 되돌리기로 */
		if (ret < 0)
			goto err_phy;

		/* [한국어] 초기화 */
		ret = phy_init(ks_pcie->phy[i]);
		/* [한국어] 실패하면 공통 되돌리기로 */
		if (ret < 0)
			goto err_phy;

		/* [한국어] 전원을 켠다 */
		ret = phy_power_on(ks_pcie->phy[i]);
		/* [한국어] 여기서 실패하면 이 레인의 init 은 성공한 상태이므로 */
		if (ret < 0) {
			phy_exit(ks_pcie->phy[i]);
			goto err_phy;
		}
	}

	return 0;

err_phy:
	while (--i >= 0) {
		phy_power_off(ks_pcie->phy[i]);
		phy_exit(ks_pcie->phy[i]);
	}

	return ret;
}

/* [한국어]
 * ks_pcie_set_mode - Keystone 2 의 SoC 모드 비트를 RC 로 설정한다
 *
 * @dev: 이 컨트롤러의 device.  @return: 0 = 성공(또는 할 일 없음), 음수 errno
 *
 * 이 컨트롤러가 RC 로 동작할지 EP 로 동작할지는 PCIe IP 밖의 SoC 설정
 * 레지스터가 정한다. 그것을 syscon 으로 찾아 쓰는 것이 이 함수다.
 *
 * Keystone 2 는 RC 만 지원하므로 모드 인자가 없다 — 언제나
 * KS_PCIE_DEV_TYPE(RC) 를 쓴다. AM654 판(ks_pcie_am654_set_mode)이 모드를
 * 인자로 받는 것과 대조된다.
 *
 * KS_PCIE_SYSCLOCKOUTEN 을 함께 세우는데, 그 뜻은 코드만으로는 분명하지
 * 않다 — 이름으로 보아 SoC 가 레퍼런스 클록을 밖으로 내보내게 하는
 * 스위치로 보이나, 근거가 되는 TI 문서는 이 트리에 없다.
 * RC 는 슬롯에 클록을 공급해야 하므로 그 해석과 어긋나지 않는다.
 *
 * syscon phandle 이 없으면 **성공(0)으로 물러난다**. 오류가 아니라 "이
 * 보드는 그 설정이 필요 없다" 로 보는 것이다.
 * 반면 오프셋 인자 파싱은 실패해도 offset 을 0 으로 두고 계속한다 —
 * 원문 영어 주석이 옛 장치 트리 호환성 때문이라고 밝힌다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ks_pcie_probe() → [이 함수]
 *     → syscon_regmap_lookup_by_phandle() → regmap_update_bits()
 */
static int ks_pcie_set_mode(struct device *dev)
{
	struct device_node *np = dev->of_node;
	/* [한국어] phandle 인자를 받을 구조체 */
	struct of_phandle_args args;
	/* [한국어] 레지스터 오프셋. 파싱 실패 시 0 으로 남는다 */
	unsigned int offset = 0;
	/* [한국어] syscon 레지스터 맵 */
	struct regmap *syscon;
	/* [한국어] 쓸 값 */
	u32 val;
	/* [한국어] 쓸 비트의 마스크 */
	u32 mask;
	/* [한국어] 0 으로 초기화한다 — 아래 파싱 결과를 담기 전에 쓰이지는 않는다 */
	int ret = 0;

	/* [한국어] SoC 의 PCIe 모드 레지스터를 syscon 으로 찾는다 */
	syscon = syscon_regmap_lookup_by_phandle(np, "ti,syscon-pcie-mode");
	/* [한국어] 없으면 **성공으로** 물러난다. 이 보드는 그 설정이 필요 없다는 뜻이다.
	 * ks_pcie_init_id() 가 같은 상황을 오류로 보는 것과 대조된다 */
	if (IS_ERR(syscon))
		return 0;

	/* Do not error out to maintain old DT compatibility */
	ret = of_parse_phandle_with_fixed_args(np, "ti,syscon-pcie-mode", 1, 0, &args);
	/* [한국어] 파싱에 성공했을 때만 */
	if (!ret)
		/* [한국어] 오프셋 인자를 쓴다. 원문 영어 주석대로 옛 DT 호환성을 위해
		 * 실패해도 계속한다 */
		offset = args.args[0];

	/* [한국어] 모드 필드와 클록 출력 비트를 함께 바꾼다 */
	mask = KS_PCIE_DEV_TYPE_MASK | KS_PCIE_SYSCLOCKOUTEN;
	/* [한국어] Keystone 2 는 RC 만 지원하므로 언제나 RC 다 */
	val = KS_PCIE_DEV_TYPE(RC) | KS_PCIE_SYSCLOCKOUTEN;

	/* [한국어] 마스크 안의 비트만 갈아 끼운다 */
	ret = regmap_update_bits(syscon, offset, mask, val);
	/* [한국어] 쓰기 실패 */
	if (ret) {
		/* [한국어] 무엇이 실패했는지 남긴다 */
		dev_err(dev, "failed to set pcie mode\n");
		return ret;
	}

	return 0;
}

/* [한국어]
 * ks_pcie_am654_set_mode - AM654 의 SoC 모드 비트를 RC 또는 EP 로 설정한다
 *
 * @dev: 이 컨트롤러의 device.  @mode: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE
 * @return: 0 = 성공(또는 할 일 없음), 음수 errno
 *
 * Keystone 2 판과 구조가 같고 셋이 다르다.
 *   - 모드를 인자로 받는다. AM654 는 EP 로도 동작할 수 있기 때문이다.
 *   - 마스크가 AM654_PCIE_DEV_TYPE_MASK(0x3)로, 시프트 없이 하위 두 비트다.
 *     Keystone 2 는 KS_PCIE_DEV_TYPE_MASK 가 (0x3 << 1)로 한 비트 밀려 있다.
 *     같은 개념의 필드가 세대마다 다른 자리에 있다는 뜻이다.
 *   - 클록 출력 비트를 건드리지 않는다.
 *
 * switch 로 모드를 값으로 옮기고, 알 수 없는 모드면 -EINVAL 이다.
 * RC 와 EP 상수는 이 파일 위쪽에 정의된 0x2 와 0x0 이다(LEG_EP 0x1 도
 * 정의되어 있으나 이 파일에서 쓰이지 않는다).
 *
 * syscon 부재를 성공으로 보는 것과 오프셋 파싱 실패를 무시하는 것은
 * Keystone 2 판과 같다(원문 영어 주석도 같은 문구다).
 *
 * 어느 판을 쓸지는 ks_pcie_probe() 가 dw_pcie_ver_is_ge(pci, 480A) 로
 * 가른다 — DWC IP 버전이 4.80a 이상이면 이쪽이다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   ks_pcie_probe() → [이 함수] → regmap_update_bits()
 */
static int ks_pcie_am654_set_mode(struct device *dev,
				  enum dw_pcie_device_mode mode)
{
	struct device_node *np = dev->of_node;
	/* [한국어] phandle 인자를 받을 구조체 */
	struct of_phandle_args args;
	/* [한국어] 레지스터 오프셋 */
	unsigned int offset = 0;
	/* [한국어] syscon 레지스터 맵 */
	struct regmap *syscon;
	/* [한국어] 쓸 값. 아래 switch 가 정한다 */
	u32 val;
	/* [한국어] 쓸 비트의 마스크 */
	u32 mask;
	/* [한국어] 0 으로 초기화 */
	int ret = 0;

	/* [한국어] 같은 phandle 이름을 쓴다 — 두 세대가 같은 DT 속성을 공유한다 */
	syscon = syscon_regmap_lookup_by_phandle(np, "ti,syscon-pcie-mode");
	/* [한국어] 없으면 성공으로 물러난다 */
	if (IS_ERR(syscon))
		return 0;

	/* Do not error out to maintain old DT compatibility */
	ret = of_parse_phandle_with_fixed_args(np, "ti,syscon-pcie-mode", 1, 0, &args);
	/* [한국어] 파싱에 성공했을 때만 */
	if (!ret)
		/* [한국어] 오프셋 인자를 쓴다 */
		offset = args.args[0];

	/* [한국어] AM654 는 시프트 없이 하위 두 비트다. Keystone 2 가 한 비트 밀려 있는
	 * 것과 다르다 */
	mask = AM654_PCIE_DEV_TYPE_MASK;

	/* [한국어] 모드를 값으로 옮긴다 */
	switch (mode) {
	/* [한국어] 루트 컴플렉스 */
	case DW_PCIE_RC_TYPE:
		val = RC;
		break;
	/* [한국어] 엔드포인트. AM654 만 이 갈래를 쓸 수 있다 */
	case DW_PCIE_EP_TYPE:
		val = EP;
		break;
	default:
		dev_err(dev, "INVALID device type %d\n", mode);
		return -EINVAL;
	}

	/* [한국어] 마스크 안의 비트만 갈아 끼운다 */
	ret = regmap_update_bits(syscon, offset, mask, val);
	/* [한국어] 쓰기 실패 */
	if (ret) {
		/* [한국어] 무엇이 실패했는지 남긴다 */
		dev_err(dev, "failed to set pcie mode\n");
		return ret;
	}

	return 0;
}

/* [한국어] ti,keystone-pcie 용 데이터. RC 전용이다 */
static const struct ks_pcie_of_data ks_pcie_rc_of_data = {
	/* [한국어] msi_init 을 가진 host_ops 를 건다 */
	.host_ops = &ks_pcie_host_ops,
	.mode = DW_PCIE_RC_TYPE,
	.version = DW_PCIE_VER_365A,
};

/* [한국어] ti,am654-pcie-rc 용 데이터 */
static const struct ks_pcie_of_data ks_pcie_am654_rc_of_data = {
	/* [한국어] msi_init 이 없는 host_ops 를 건다 */
	.host_ops = &ks_pcie_am654_host_ops,
	.mode = DW_PCIE_RC_TYPE,
	.version = DW_PCIE_VER_490A,
};

/* [한국어] ti,am654-pcie-ep 용 데이터. 이 파일에서 유일한 EP 항목이다 */
static const struct ks_pcie_of_data ks_pcie_am654_ep_of_data = {
	/* [한국어] EP 콜백 표를 건다. host_ops 는 NULL 로 남는다 */
	.ep_ops = &ks_pcie_am654_ep_ops,
	.mode = DW_PCIE_EP_TYPE,
	.version = DW_PCIE_VER_490A,
};

/* [한국어] 장치 트리 compatible 과 데이터의 대응표 */
static const struct of_device_id ks_pcie_of_match[] = {
	{
		/* [한국어] 이 항목만 type 을 함께 요구한다. 옛 바인딩의 형태로 보인다 */
		.type = "pci",
		.data = &ks_pcie_rc_of_data,
		.compatible = "ti,keystone-pcie",
	},
	{
		/* [한국어] AM654 RC */
		.data = &ks_pcie_am654_rc_of_data,
		.compatible = "ti,am654-pcie-rc",
	},
	{
		/* [한국어] AM654 EP */
		.data = &ks_pcie_am654_ep_of_data,
		.compatible = "ti,am654-pcie-ep",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, ks_pcie_of_match);

/* [한국어]
 * ks_pcie_probe - 플랫폼 장치를 잡아 RC 또는 EP 로 세운다
 *
 * @pdev: 장치 트리가 만든 플랫폼 장치
 * @return: 0 = 성공, 음수 errno = 실패
 *
 * 이 드라이버의 진입점이다. compatible 문자열 셋(ti,keystone-pcie,
 * ti,am654-pcie-rc, ti,am654-pcie-ep)이 모두 여기로 온다.
 *
 * 공통 준비:
 *   1) of_device_get_match_data() 로 ks_pcie_of_data 를 얻어 모드,
 *      콜백 표, DWC 버전을 정한다. 이후의 모든 분기가 여기서 시작한다.
 *   2) struct keystone_pcie 와 struct dw_pcie 를 각각 devm 으로 잡는다.
 *      대부분의 DWC 접착 계층이 dw_pcie 를 자기 구조체에 박아 두는 것과
 *      달리 이 파일은 포인터로 들고 있고, 역방향은 to_keystone_pcie()
 *      매크로가 dev_get_drvdata() 로 되찾는다.
 *   3) "app" 창(TI 애플리케이션 레지스터)과 "dbics" 창(DWC DBI)을 매핑한다.
 *      dbi_base 와 dbi_base2 를 같은 값으로 두는 것이 요점 — 이 하드웨어는
 *      dbi2 창이 따로 없고 CS2 모드로 겹쳐 접근하기 때문이다
 *      (ks_pcie_am654_write_dbi2 참조).
 *   4) is_am6 를 compatible 문자열로 정한다. ti,am654-pcie-rc 일 때만
 *      참이다 — EP 쪽은 이 플래그를 세우지 않는 점에 주의한다.
 *   5) 오류 IRQ 를 IRQF_SHARED 로 건다. RC/EP 공통이다.
 *   6) 레인 수만큼 PHY 를 얻고 device_link 로 묶는다. DL_FLAG_STATELESS 는
 *      PM 순서만 정하고 상태 추적은 하지 않는 형태다.
 *   7) reset GPIO 를 얻고, PHY 런타임 PM 참조를 잡은 채
 *      ks_pcie_enable_phy() 로 켠다.
 *   8) pm_runtime 을 켜고, DWC 버전에 따라 두 set_mode 중 하나를 부른다.
 *
 * 모드별 갈래:
 *   RC — CONFIG_PCI_KEYSTONE_HOST 가 꺼져 있으면 -ENODEV.
 *        num-viewport 를 DT 에서 읽는다(없으면 실패).
 *        원문 영어 주석대로 PCIe CEM 규격 r2.0 의 전원 시퀀스 표에 따라
 *        REFCLK 이 안정된 뒤 최소 100마이크로초 뒤에 PERST 를 풀어야 하고,
 *        RC 모드의 REFCLK 은 PHY 를 켤 때 선택되므로 여기서 100마이크로초
 *        뒤에 GPIO 를 올린다.
 *        그 뒤 dw_pcie_host_init() 이 ks_pcie_host_init() 을 되부른다.
 *   EP — CONFIG_PCI_KEYSTONE_EP 가 꺼져 있으면 -ENODEV.
 *        dw_pcie_ep_init() → dw_pcie_ep_init_registers() →
 *        pci_epc_init_notify() 순서다. PERST GPIO 를 건드리지 않는다 —
 *        EP 는 PERST 를 받는 쪽이기 때문이다.
 *
 * 마지막에 ks_pcie_enable_error_irq() 로 오류 보고를 연다. 초기화가 다
 * 끝난 뒤여야 초기화 중의 링크 흔들림을 오류로 보고하지 않는다.
 *
 * 에러 경로가 세 라벨로 나뉜다 — err_ep_init(EP 레지스터 초기화 실패),
 * err_get_sync(pm_runtime 이후 실패), err_link(PHY/GPIO 단계 실패).
 * device_link 는 err_link 에서 역순으로 푼다.
 *
 * 실행 컨텍스트: 드라이버 바인드 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (플랫폼 버스 바인드) → [이 함수]
 *     → ks_pcie_enable_phy() → ks_pcie_set_mode()/ks_pcie_am654_set_mode()
 *     → dw_pcie_host_init() 또는 dw_pcie_ep_init()
 *     → ks_pcie_enable_error_irq()
 */
static int ks_pcie_probe(struct platform_device *pdev)
{
	const struct dw_pcie_host_ops *host_ops;
	/* [한국어] EP 모드에서 DWC 에 줄 콜백 표 */
	const struct dw_pcie_ep_ops *ep_ops;
	/* [한국어] 로그와 devm 의 기준 */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 장치 트리 노드 */
	struct device_node *np = dev->of_node;
	/* [한국어] compatible 별 데이터 */
	const struct ks_pcie_of_data *data;
	/* [한국어] RC 인지 EP 인지 */
	enum dw_pcie_device_mode mode;
	/* [한국어] DWC 컨트롤러 상태. 이 구조체에 박아 두지 않고 따로 잡는다 */
	struct dw_pcie *pci;
	/* [한국어] 이 파일의 구조체 */
	struct keystone_pcie *ks_pcie;
	/* [한국어] 레인별 device_link 배열 */
	struct device_link **link;
	/* [한국어] PERST reset GPIO. RC 모드에서만 쓴다 */
	struct gpio_desc *gpiod;
	/* [한국어] 창을 얻을 때 쓰는 임시 변수 */
	struct resource *res;
	/* [한국어] DBI 창의 가상 주소 */
	void __iomem *base;
	/* [한국어] outbound 창 개수. RC 갈래에서만 읽는다 */
	u32 num_viewport;
	/* [한국어] 레인별 PHY 배열 */
	struct phy **phy;
	/* [한국어] 레인 수 */
	u32 num_lanes;
	/* [한국어] PHY 이름을 조립할 버퍼. "pcie-phy0" 처럼 최대 9자라 10바이트면 넉넉하다 */
	char name[10];
	/* [한국어] DWC IP 버전 */
	u32 version;
	/* [한국어] 각 단계의 결과 */
	int ret;
	/* [한국어] 오류 IRQ 번호 */
	int irq;
	/* [한국어] 루프 인덱스. err_link 경로가 이 값을 이어받아 되돌린다 */
	int i;

	/* [한국어] 어느 compatible 로 매칭됐는지에 따라 데이터를 얻는다 */
	data = of_device_get_match_data(dev);
	/* [한국어] 지원하지 않는 하드웨어 */
	if (!data)
		return -EINVAL;

	/* [한국어] DWC IP 버전 */
	version = data->version;
	/* [한국어] RC 콜백 표(EP 항목이면 NULL) */
	host_ops = data->host_ops;
	/* [한국어] EP 콜백 표(RC 항목이면 NULL) */
	ep_ops = data->ep_ops;
	/* [한국어] 모드 */
	mode = data->mode;

	/* [한국어] 이 파일의 구조체를 devm 으로 잡는다 */
	ks_pcie = devm_kzalloc(dev, sizeof(*ks_pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!ks_pcie)
		return -ENOMEM;

	/* [한국어] DWC 구조체를 따로 잡는다. 대부분의 접착 계층이 자기 구조체에
	 * 박아 두는 것과 다른 방식이다 */
	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!pci)
		return -ENOMEM;

	/* [한국어] TI 애플리케이션 레지스터 창 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "app");
	/* [한국어] 매핑한다 */
	ks_pcie->va_app_base = devm_ioremap_resource(dev, res);
	/* [한국어] 매핑 실패 */
	if (IS_ERR(ks_pcie->va_app_base))
		/* [한국어] 오류를 올린다 */
		return PTR_ERR(ks_pcie->va_app_base);

	/* [한국어] struct resource 를 통째로 복사해 둔다. MSI 목적지 주소를 만들 때
	 * 물리 주소가 필요하기 때문이다 */
	ks_pcie->app = *res;

	/* [한국어] DWC DBI 창 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbics");
	/* [한국어] config 접근용으로 매핑한다 */
	base = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] 매핑 실패 */
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* [한국어] AM654 RC 인지 판정한다. **EP 는 compatible 이 달라 false 로 남는다** */
	if (of_device_is_compatible(np, "ti,am654-pcie-rc"))
		/* [한국어] 플래그를 세운다 */
		ks_pcie->is_am6 = true;

	/* [한국어] DBI 창을 건다 */
	pci->dbi_base = base;
	/* [한국어] dbi2 를 같은 값으로 둔다 — 이 하드웨어는 dbi2 창이 따로 없고
	 * CS2 모드로 겹쳐 접근하기 때문이다 */
	pci->dbi_base2 = base;
	/* [한국어] 장치 포인터 */
	pci->dev = dev;
	/* [한국어] 컨트롤러 조작 표 */
	pci->ops = &ks_pcie_dw_pcie_ops;
	/* [한국어] IP 버전. 아래 dw_pcie_ver_is_ge 판정의 근거가 된다 */
	pci->version = version;

	/* [한국어] 오류 IRQ 를 얻는다 */
	irq = platform_get_irq(pdev, 0);
	/* [한국어] 획득 실패 */
	if (irq < 0)
		/* [한국어] 그대로 올린다 */
		return irq;

	/* [한국어] 오류 핸들러를 건다. RC/EP 공통이라 모드를 가르기 전에 등록한다 */
	ret = devm_request_irq(dev, irq, ks_pcie_err_irq_handler, IRQF_SHARED,
			       "ks-pcie-error-irq", ks_pcie);
	/* [한국어] 등록 실패 */
	if (ret < 0) {
		/* [한국어] 어느 IRQ 였는지 남긴다 */
		dev_err(dev, "failed to request error IRQ %d\n",
			irq);
		return ret;
	}

	/* [한국어] 레인 수를 DT 에서 읽는다 */
	ret = of_property_read_u32(np, "num-lanes", &num_lanes);
	/* [한국어] 속성이 없으면 */
	if (ret)
		/* [한국어] 1 레인으로 본다 */
		num_lanes = 1;

	/* [한국어] 레인별 PHY 배열을 잡는다 */
	phy = devm_kcalloc(dev, num_lanes, sizeof(*phy), GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!phy)
		return -ENOMEM;

	/* [한국어] 레인별 device_link 배열을 잡는다 */
	link = devm_kcalloc(dev, num_lanes, sizeof(*link), GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!link)
		return -ENOMEM;

	/* [한국어] 레인마다 PHY 를 얻는다 */
	for (i = 0; i < num_lanes; i++) {
		/* [한국어] "pcie-phy0" 형태의 이름을 만든다 */
		snprintf(name, sizeof(name), "pcie-phy%d", i);
		/* [한국어] optional 판이라 없어도 오류가 아니다 */
		phy[i] = devm_phy_optional_get(dev, name);
		/* [한국어] 오류 포인터면 실패다 */
		if (IS_ERR(phy[i])) {
			/* [한국어] errno 를 꺼낸다 */
			ret = PTR_ERR(phy[i]);
			goto err_link;
		}

		/* [한국어] PHY 가 없는 레인은 */
		if (!phy[i])
			continue;

		/* [한국어] PM 순서를 정하는 링크를 건다. DL_FLAG_STATELESS 는 순서만 강제하고
		 * 상태 추적은 하지 않는다 */
		link[i] = device_link_add(dev, &phy[i]->dev, DL_FLAG_STATELESS);
		/* [한국어] 링크 생성 실패 */
		if (!link[i]) {
			ret = -EINVAL;
			goto err_link;
		}
	}

	/* [한국어] 이후 자식 노드를 찾는 기준 */
	ks_pcie->np = np;
	/* [한국어] DWC 구조체를 연결한다 */
	ks_pcie->pci = pci;
	/* [한국어] device_link 배열 */
	ks_pcie->link = link;
	/* [한국어] 레인 수 */
	ks_pcie->num_lanes = num_lanes;
	/* [한국어] PHY 배열 */
	ks_pcie->phy = phy;

	/* [한국어] PERST GPIO 를 얻는다. optional 이라 없어도 된다 */
	gpiod = devm_gpiod_get_optional(dev, "reset",
					GPIOD_OUT_LOW);
	/* [한국어] 오류 포인터면 실패다 */
	if (IS_ERR(gpiod)) {
		/* [한국어] errno 를 꺼낸다 */
		ret = PTR_ERR(gpiod);
		/* [한국어] 재시도 요청은 흔한 경우라 로그를 남기지 않는다 */
		if (ret != -EPROBE_DEFER)
			/* [한국어] 그 밖의 실패만 남긴다 */
			dev_err(dev, "Failed to get reset GPIO\n");
		goto err_link;
	}

	/* Obtain references to the PHYs */
	for (i = 0; i < num_lanes; i++)
		phy_pm_runtime_get_sync(ks_pcie->phy[i]);

	/* [한국어] 레인별 PHY 를 켠다. 앞뒤로 PHY 런타임 PM 참조를 잡아 둔 상태다 */
	ret = ks_pcie_enable_phy(ks_pcie);

	/* Release references to the PHYs */
	for (i = 0; i < num_lanes; i++)
		phy_pm_runtime_put_sync(ks_pcie->phy[i]);

	/* [한국어] PHY 를 켜지 못했다 */
	if (ret) {
		/* [한국어] 실패를 남긴다 */
		dev_err(dev, "failed to enable phy\n");
		goto err_link;
	}

	/* [한국어] to_keystone_pcie() 매크로가 이 값을 되찾는다.
	 * ks_pcie_quirk() 도 브리지 부모의 drvdata 로 같은 값을 꺼낸다 */
	platform_set_drvdata(pdev, ks_pcie);
	pm_runtime_enable(dev);
	/* [한국어] 전원 도메인을 켠다 */
	ret = pm_runtime_get_sync(dev);
	/* [한국어] 실패 */
	if (ret < 0) {
		/* [한국어] 무엇이 실패했는지 남긴다 */
		dev_err(dev, "pm_runtime_get_sync failed\n");
		goto err_get_sync;
	}

	/* [한국어] DWC IP 가 4.80a 이상인가로 세대를 가른다 */
	if (dw_pcie_ver_is_ge(pci, 480A))
		/* [한국어] AM654 판. 모드를 인자로 받는다 */
		ret = ks_pcie_am654_set_mode(dev, mode);
	else
		/* [한국어] Keystone 2 판. 언제나 RC 다 */
		ret = ks_pcie_set_mode(dev);
	/* [한국어] 모드 설정 실패 */
	if (ret < 0)
		goto err_get_sync;

	/* [한국어] 여기서 RC 와 EP 가 갈린다 */
	switch (mode) {
	/* [한국어] 루트 컴플렉스 경로 */
	case DW_PCIE_RC_TYPE:
		if (!IS_ENABLED(CONFIG_PCI_KEYSTONE_HOST)) {
			ret = -ENODEV;
			goto err_get_sync;
		}

		/* [한국어] outbound 창 개수를 DT 에서 읽는다 */
		ret = of_property_read_u32(np, "num-viewport", &num_viewport);
		/* [한국어] 속성이 없으면 실패한다 — 창을 몇 개 깔지 알 수 없기 때문이다 */
		if (ret < 0) {
			/* [한국어] 무엇이 없는지 남긴다 */
			dev_err(dev, "unable to read *num-viewport* property\n");
			goto err_get_sync;
		}

		/*
		 * "Power Sequencing and Reset Signal Timings" table in
		 * PCI EXPRESS CARD ELECTROMECHANICAL SPECIFICATION, REV. 2.0
		 * indicates PERST# should be deasserted after minimum of 100us
		 * once REFCLK is stable. The REFCLK to the connector in RC
		 * mode is selected while enabling the PHY. So deassert PERST#
		 * after 100 us.
		 */
		if (gpiod) {
			/* [한국어] 위 원문 영어 주석대로, REFCLK 이 안정된 뒤 최소 100마이크로초를
			 * 기다렸다가 PERST 를 풀어야 한다 */
			usleep_range(100, 200);
			/* [한국어] PERST 를 푼다. 이 순간부터 상대 장치가 링크 훈련을 시작한다 */
			gpiod_set_value_cansleep(gpiod, 1);
		}

		/* [한국어] 창 개수를 기억해 둔다 */
		ks_pcie->num_viewport = num_viewport;
		/* [한국어] 이 세대의 host_ops 를 건다 */
		pci->pp.ops = host_ops;
		/* [한국어] DWC 코어에 넘긴다. 그 안에서 ks_pcie_host_init() 이 되불린다 */
		ret = dw_pcie_host_init(&pci->pp);
		/* [한국어] 초기화 실패 */
		if (ret < 0)
			goto err_get_sync;
		break;
	/* [한국어] 엔드포인트 경로 */
	case DW_PCIE_EP_TYPE:
		if (!IS_ENABLED(CONFIG_PCI_KEYSTONE_EP)) {
			ret = -ENODEV;
			goto err_get_sync;
		}

		/* [한국어] EP 콜백 표를 건다 */
		pci->ep.ops = ep_ops;
		/* [한국어] DWC EP 계층을 초기화한다. 그 안에서 ks_pcie_am654_ep_init() 이 되불린다 */
		ret = dw_pcie_ep_init(&pci->ep);
		/* [한국어] 실패 */
		if (ret < 0)
			goto err_get_sync;

		/* [한국어] EP 레지스터를 초기화한다 */
		ret = dw_pcie_ep_init_registers(&pci->ep);
		/* [한국어] 실패 */
		if (ret) {
			/* [한국어] 무엇이 실패했는지 남긴다 */
			dev_err(dev, "Failed to initialize DWC endpoint registers\n");
			goto err_ep_init;
		}

		pci_epc_init_notify(pci->ep.epc);

		break;
	default:
		dev_err(dev, "INVALID device type %d\n", mode);
		ret = -EINVAL;
		goto err_get_sync;
	}

	ks_pcie_enable_error_irq(ks_pcie);

	return 0;

err_ep_init:
	dw_pcie_ep_deinit(&pci->ep);
err_get_sync:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
	ks_pcie_disable_phy(ks_pcie);

err_link:
	while (--i >= 0 && link[i])
		device_link_del(link[i]);

	return ret;
}

/* [한국어]
 * ks_pcie_remove - 이 컨트롤러를 떼어 낸다
 *
 * @pdev: 제거되는 플랫폼 장치.  @return: 없음
 *
 * pm_runtime 을 내리고, PHY 를 끄고, device_link 를 모두 푼다.
 *
 * probe 의 정확한 역순은 아니다 — dw_pcie_host_init() 이나
 * dw_pcie_ep_init() 을 되돌리는 코드가 없고, IRQ 도메인과 체인 핸들러도
 * 정리하지 않는다. 그래서 이 드라이버를 언바인드한 뒤 다시 바인드하는
 * 것이 안전한지는 코드만으로 판단할 수 없다.
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * device_link 를 while (num_lanes--) 로 역순으로 푸는 것은
 * ks_pcie_disable_phy() 와 같은 관용구다. 다만 probe 의 err_link 경로가
 * link[i] 가 NULL 인지 확인하는 것과 달리 여기서는 확인하지 않는다 —
 * 정상 경로로 여기 왔다면 모두 유효하다는 전제다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (플랫폼 버스 언바인드) → [이 함수]
 *     → pm_runtime_put/disable() → ks_pcie_disable_phy() → device_link_del()
 */
static void ks_pcie_remove(struct platform_device *pdev)
{
	struct keystone_pcie *ks_pcie = platform_get_drvdata(pdev);
	/* [한국어] 레인별 device_link 배열 */
	struct device_link **link = ks_pcie->link;
	/* [한국어] 레인 수 */
	int num_lanes = ks_pcie->num_lanes;
	/* [한국어] 로그 출력 대상 */
	struct device *dev = &pdev->dev;

	pm_runtime_put(dev);
	pm_runtime_disable(dev);
	ks_pcie_disable_phy(ks_pcie);
	/* [한국어] 역순으로 링크를 푼다. probe 의 err_link 와 달리 NULL 검사가 없다 —
	 * 정상 경로로 여기 왔다면 모두 유효하다는 전제다 */
	while (num_lanes--)
		device_link_del(link[num_lanes]);
}

/* [한국어] 플랫폼 드라이버 서술자 */
static struct platform_driver ks_pcie_driver = {
	/* [한국어] 바인드 시 진입점 */
	.probe  = ks_pcie_probe,
	.remove = ks_pcie_remove,
	.driver = {
		/* [한국어] sysfs 와 로그에 보이는 이름 */
		.name	= "keystone-pcie",
		.of_match_table = ks_pcie_of_match,
	},
};

#ifdef CONFIG_ARM
/*
 * When a PCI device does not exist during config cycles, keystone host
 * gets a bus error instead of returning 0xffffffff (PCI_ERROR_RESPONSE).
 * This handler always returns 0 for this kind of fault.
 */
/* [한국어]
 * ks_pcie_fault - ARM 전용. 없는 장치를 찔러 생긴 외부 abort 를 삼킨다
 *
 * @addr: 오류가 난 주소(쓰지 않는다).  @fsr: 오류 상태(쓰지 않는다)
 * @regs: 오류 시점의 레지스터 상태.  @return: 항상 0(오류를 처리했음)
 *
 * 위 원문 영어 주석이 문제를 밝힌다 — config 사이클 중 장치가 없으면
 * Keystone 호스트는 0xffffffff(PCI_ERROR_RESPONSE)를 돌려주는 대신
 * **버스 오류**를 낸다. ARM 에서 그것이 "External abort" 로 올라오고,
 * 아무도 처리하지 않으면 커널이 죽는다.
 *
 * 그래서 이 핸들러가 그 abort 를 가로채 "장치 없음" 처럼 보이게 고친다.
 *   1) 오류를 일으킨 명령어를 읽는다(instruction_pointer 가 가리키는 곳).
 *   2) (instr & 0x0e100090) == 0x00100090 으로 그것이 로드 명령인지 본다.
 *      ARM 명령어 인코딩을 직접 해독하는 것이며, 그 비트 패턴의 근거는
 *      ARM 아키텍처 참조 매뉴얼 소관이라 이 트리에서 확인하지 못했다.
 *   3) 맞으면 목적지 레지스터 번호를 비트 12~15 에서 뽑아, 그 레지스터에
 *      -1(즉 0xffffffff)을 넣는다. 읽기가 성공해 0xffffffff 를 읽은 것처럼
 *      만드는 것이다.
 *   4) PC 를 4 바이트 전진시켜 그 명령어를 건너뛴다.
 *
 * 로드가 아니면 레지스터를 건드리지 않고 PC 도 전진시키지 않은 채 0 을
 * 돌려준다. 그러면 같은 명령어를 다시 실행해 무한 루프에 빠질 수 있으나,
 * config 접근이 아닌 abort 가 여기 올 일이 없다는 전제로 보인다.
 *
 * CONFIG_ARM 에서만 컴파일된다. 64비트 ARM 에서는 이 문제를 다르게
 * 다루거나 발생하지 않는 것으로 보이며, 근거는 이 트리에 없다.
 *
 * 실행 컨텍스트: abort 예외 처리 문맥. 매우 제한적이다.
 *
 * 호출 체인:
 *   (ARM abort 예외) → hook_fault_code 로 등록된 핸들러 → [이 함수]
 */
static int ks_pcie_fault(unsigned long addr, unsigned int fsr,
			 struct pt_regs *regs)
{
	unsigned long instr = *(unsigned long *)instruction_pointer(regs);

	/* [한국어] ARM 명령어를 해독해 로드 명령인지 본다. 그 비트 패턴의 근거는
	 * ARM 아키텍처 참조 매뉴얼 소관이라 이 트리에서 확인하지 못했다 */
	if ((instr & 0x0e100090) == 0x00100090) {
		/* [한국어] 목적지 레지스터 번호를 비트 15-12 에서 뽑는다 */
		int reg = (instr >> 12) & 15;

		/* [한국어] 그 레지스터에 -1(0xffffffff)을 넣는다. 읽기가 성공해 "장치 없음" 값을
		 * 읽은 것처럼 만드는 것이다 */
		regs->uregs[reg] = -1;
		/* [한국어] PC 를 4바이트 전진시켜 그 명령어를 건너뛴다 */
		regs->ARM_pc += 4;
	}

	return 0;
}

/* [한국어]
 * ks_pcie_init - ARM 전용 초기화. abort 훅을 걸고 드라이버를 등록한다
 *
 * @return: platform_driver_register() 의 반환값
 *
 * CONFIG_ARM 에서만 존재한다. 그 밖의 아키텍처에서는 이 함수 대신
 * builtin_platform_driver(ks_pcie_driver) 한 줄이 쓰인다.
 *
 * 하는 일이 둘이다.
 *   1) 장치 트리에 이 드라이버가 맡을 노드가 실제로 있는지 확인하고,
 *      있을 때만 abort 훅을 건다. 없는 시스템에 전역 예외 훅을 거는 것을
 *      피하려는 것이다.
 *      원문 영어 주석이 이유를 밝힌다 — OCP 오류로 이어지는 PCIe 접근
 *      오류를 ARM 이 "External aborts" 로 잡는다.
 *      fault code 17 에 SIGBUS 와 함께 등록한다. 그 번호의 의미는 ARM
 *      아키텍처 소관이라 이 트리에서 확인하지 못했다.
 *   2) 플랫폼 드라이버를 등록한다.
 *
 * device_initcall 로 등록되므로 부팅 중 한 번 불린다.
 * 훅을 드라이버 등록보다 **먼저** 거는 순서가 중요하다 — 등록 직후
 * probe 가 돌면서 config 접근을 시작할 수 있고, 그때 이미 훅이 있어야 한다.
 *
 * 실행 컨텍스트: 부팅 initcall, 단일 스레드.
 *
 * 호출 체인:
 *   (device_initcall) → [이 함수]
 *     → of_find_matching_node() → hook_fault_code() → platform_driver_register()
 */
static int __init ks_pcie_init(void)
{
	/*
	 * PCIe access errors that result into OCP errors are caught by ARM as
	 * "External aborts"
	 */
	if (of_find_matching_node(NULL, ks_pcie_of_match))
		/* [한국어] fault code 17 에 이 핸들러를 건다. 그 번호의 의미는 ARM 아키텍처
		 * 소관이라 이 트리에서 확인하지 못했다. SIGBUS 는 핸들러가 처리하지
		 * 못했을 때 보낼 시그널이다 */
		hook_fault_code(17, ks_pcie_fault, SIGBUS, 0,
				"Asynchronous external abort");

	/* [한국어] 훅을 건 뒤에 드라이버를 등록한다. 등록 직후 probe 가 config 접근을
	 * 시작할 수 있으므로 이 순서여야 한다 */
	return platform_driver_register(&ks_pcie_driver);
}
device_initcall(ks_pcie_init);
#else
builtin_platform_driver(ks_pcie_driver);
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PCIe controller driver for Texas Instruments Keystone SoCs");
MODULE_AUTHOR("Murali Karicheri <m-karicheri2@ti.com>");
