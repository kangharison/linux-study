// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek PCIe host controller driver.
 *
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Jianjun Wang <jianjun.wang@mediatek.com>
 */

/*
 * [한국어 설명] 미디어텍 3세대 PCIe 호스트 컨트롤러 (pcie-mediatek-gen3.c)
 *
 * === 파일의 역할 ===
 * 미디어텍의 3세대 PCIe 컨트롤러 IP 를 다룬다. MT8192/MT8196 과, 같은 IP 를
 * 쓰는 Airoha EN7581 이 대상이다. DesignWare 가 아닌 미디어텍 자체 IP 라
 * dwc/ 가 아닌 이 자리에 있고, config 접근·링크 훈련·주소 변환·인터럽트를
 * 전부 이 파일이 직접 구현한다.
 *
 * 앞 세대(pcie-mediatek.c 의 v1/v2)와 **같은 벤더의 다른 IP** 이지 개선판이
 * 아니다. 두 파일이 공유하는 코드가 한 줄도 없고, 레지스터 지도도 완전히
 * 다르다. 아래 "앞 세대와 무엇이 다른가" 절에서 구체적으로 견준다.
 *
 * 이 파일이 맡는 일은 넷이다.
 *   1) config 접근 — 고정된 창(PCIE_CFG_OFFSET_ADDR)에 접근하기 전에
 *      대상 BDF 와 byte enable 을 PCIE_CFGNUM_REG 에 써 두는 방식이다.
 *      TLP 를 손으로 조립하지 않으므로 읽고 쓰는 코드가 매우 짧다.
 *   2) 링크 훈련 — RC 모드로 설정하고, DT 가 지정한 속도와 레인 수 상한을
 *      반영한 뒤, PERST 를 풀고 링크가 서기를 폴링한다. 실패하면 LTSSM
 *      상태를 사람이 읽는 이름(ltssm_str[])으로 찍어 준다.
 *   3) 주소 변환 — 최대 여덟 개의 변환 표(translate table)에 DT 의 창을
 *      2의 거듭제곱 단위로 잘라 채운다.
 *   4) 인터럽트 — INTx 넷과 MSI 256개를 컨트롤러 인터럽트 하나로 받아
 *      두 도메인으로 나눠 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: 장치 트리에 mediatek,mt8192-pcie / mediatek,mt8196-pcie /
 *       airoha,en7581-pcie
 *         -> 플랫폼 버스가 mtk_pcie_probe() 를 부른다
 *            -> devm_pci_alloc_host_bridge() 로 브리지와 상태를 한 번에 잡고
 *            -> device_get_match_data() 로 SoC 별 pdata 를 고른다
 *            -> mtk_pcie_setup_irq() — IRQ 를 얻고 두 도메인을 만들고
 *               체인 핸들러를 건다. **하드웨어를 건드리기 전이다.**
 *            -> pci_pwrctrl_create_devices() — 전원 컨트롤러 자식 장치
 *            -> mtk_pcie_setup()
 *               -> mtk_pcie_parse_port() 로 레지스터 창·리셋·PHY·클록을 얻고
 *               -> soc->power_up() 으로 전원을 올린다(SoC 별로 다르다)
 *               -> DT 의 max-link-speed 와 컨트롤러 능력 중 작은 쪽을 고르고
 *               -> mtk_pcie_startup_port() 로 링크를 세운다
 *            -> pci_host_probe() 로 PCI 코어에 넘긴다
 *
 * config 접근: PCI 코어
 *         -> mtk_pcie_ops.read/write -> mtk_pcie_config_read/write()
 *            -> mtk_pcie_config_tlp_header() 로 BDF 와 byte enable 을 써 두고
 *            -> pci_generic_config_read32/write32() 가 map_bus 가 돌려준
 *               고정 창 주소에 접근한다
 *
 * 인터럽트: 컨트롤러 인터럽트 하나
 *         -> mtk_pcie_irq_handler()(체인)
 *            -> 상태 레지스터의 INTx 구간(비트 24~27)을 훑어 intx_domain 으로
 *            -> MSI 구간(비트 8~15)을 훑어 세트별 mtk_pcie_msi_handler() 로
 *               -> 그 세트의 상태 레지스터를 훑어 msi_bottom_domain 으로
 *
 * 실행 컨텍스트: probe 와 PM 콜백은 프로세스 컨텍스트이고 msleep 으로
 * 잠든다. config 접근은 PCI 코어가 스핀락을 쥔 채 부르므로 잠들 수 없다 —
 * 그래서 이 파일의 config 경로에는 대기가 전혀 없다.
 * 인터럽트 핸들러는 인터럽트 컨텍스트이고, 레지스터를 읽고-고치고-쓰는
 * 곳은 irq_lock(raw_spinlock)으로 보호된다. MSI 비트맵은 그와 별개로
 * mutex(lock)로 보호된다 — 락이 둘인 이유는 쓰이는 문맥이 다르기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어 전체가 mtk_pcie_ops(pci_ops) 를 통해서만 이 하드웨어에
 *   닿는다. 플랫폼 버스와 장치 트리가 진입점이다.
 * 아래쪽:
 *   readl_relaxed/writel_relaxed — 이 파일의 모든 하드웨어 접근.
 *     _relaxed 판만 쓰는 것이 이 드라이버의 일관된 선택이다.
 *   irqdomain 과 irq-msi-lib — INTx 도메인과 MSI 부모 도메인.
 *     msi_create_parent_irq_domain() 과 msi_lib_init_dev_msi_info() 를 쓴다.
 *   clk / phy / reset — 클록은 devm_clk_bulk_get_all 로 통째로 얻고,
 *     리셋은 PHY 용 bulk 와 MAC 용 하나로 나뉜다.
 *   pci-pwrctrl — pci_pwrctrl_create_devices() 계열. 슬롯 전원을 별도
 *     드라이버가 관리하게 하는 최근에 들어온 계층이다.
 *   syscon/regmap — EN7581 만 쓴다(mediatek,pbus-csr).
 *   pm_runtime — 전원 도메인.
 *   "../pci.h" — PCIE_T_PVPERL_MS 와 PCI_PM_D3COLD_WAIT 가 거기 있다.
 * 옆쪽: pcie-mediatek.c(v1/v2). 같은 벤더의 앞 세대이지만 코드를 공유하지
 *   않는다. 아래 대비 절 참조.
 * 공유 상태: struct mtk_gen3_pcie 하나이며 pci_host_bridge 의 private
 *   영역에 얹혀 있다. v1/v2 가 struct mtk_pcie 아래에 포트 목록을 두는
 *   것과 달리, 이쪽은 인스턴스 하나가 포트 하나다.
 *
 * === 주요 함수/구조체 요약 ===
 * mtk_pcie_probe() / mtk_pcie_remove() : 플랫폼 드라이버 진입점과 정리.
 * mtk_pcie_setup()          : 자원 확보부터 링크 기동까지의 순서를 엮는다.
 * mtk_pcie_parse_port()     : 레지스터 창, 리셋, PHY, 클록, 레인 수를 얻는다.
 * mtk_pcie_power_up() / mtk_pcie_en7581_power_up()
 *                           : SoC 별 전원 시퀀스. pdata 의 콜백으로 갈린다.
 * mtk_pcie_power_down()     : 그 역순.
 * mtk_pcie_startup_port()   : RC 모드 설정, 속도/폭 제한, 변환 표, 링크 대기.
 * mtk_pcie_set_trans_table(): 창 하나를 2의 거듭제곱 표 여러 개로 나눠 채운다.
 * mtk_pcie_config_tlp_header() / mtk_pcie_map_bus()
 *                           : config 접근의 앞뒤 절반.
 * mtk_pcie_config_read() / mtk_pcie_config_write() : pci_ops 콜백.
 * mtk_pcie_enable_msi()     : 여덟 MSI 세트의 수신 주소를 깔고 활성화한다.
 * mtk_pcie_init_irq_domains() / mtk_pcie_setup_irq() / mtk_pcie_irq_teardown()
 *                           : 두 도메인의 생성·연결·해제.
 * mtk_pcie_irq_handler()    : 컨트롤러 인터럽트를 INTx 와 MSI 로 가른다.
 * mtk_pcie_msi_handler()    : 한 MSI 세트의 대기 비트를 훑는다.
 * mtk_compose_msi_msg() / mtk_msi_bottom_irq_ack/mask/unmask()
 *                           : MSI 바닥 irq_chip 구현.
 * mtk_msi_bottom_domain_alloc() / _free() : MSI 벡터를 비트맵에서 떼고 돌려준다.
 * mtk_intx_mask() / mtk_intx_unmask() / mtk_intx_eoi() : INTx irq_chip 구현.
 * mtk_pcie_devices_power_up() / _power_down() : PERST 와 슬롯 전원.
 * mtk_pcie_turn_off_link()  : 절전 전에 링크를 L2 로 내린다.
 * mtk_pcie_irq_save() / _irq_restore() : 절전 전후로 인터럽트 마스크를 보존한다.
 * mtk_pcie_get_controller_max_link_speed() : 하드웨어가 지원하는 최대 속도.
 * mtk_pcie_suspend_noirq() / _resume_noirq() : 시스템 절전 진입과 복귀.
 *
 * struct mtk_gen3_pcie      : 이 컨트롤러 하나의 상태 전부.
 * struct mtk_msi_set        : MSI 세트 하나(32 벡터)의 창과 수신 주소.
 * struct mtk_gen3_pcie_pdata: SoC 별 전원 콜백·리셋 이름·플래그.
 * enum mtk_gen3_pcie_flags  : 지금은 SKIP_PCIE_RSTB 하나뿐.
 * ltssm_str[]               : LTSSM 상태 번호를 사람이 읽는 이름으로 바꾸는 표.
 *
 * === 앞 세대(pcie-mediatek.c 의 v1/v2)와 무엇이 다른가 ===
 * 같은 벤더의 PCIe 호스트이지만 IP 가 달라 구조가 전면적으로 바뀌었다.
 * 특히 config 접근과 MSI 가 그렇다.
 *
 *   config 접근
 *     v1  : PCIE_CFG_ADDR(0x20)에 주소를 쓰고 PCIE_CFG_DATA(0x24)로 읽고
 *           쓰는 간접 방식. 옛 PCI 의 CF8/CFC 그대로다.
 *     v2  : **TLP 헤더를 소프트웨어로 조립한다.** CFG_HEADER_DW0/DW1/DW2
 *           매크로로 fmt/type, byte enable, BDF 를 만들어 세 레지스터에 쓴
 *           뒤, 완료(CPLD)가 돌아오기를 폴링한다
 *           (그 파일의 mtk_pcie_check_cfg_cpld).
 *     gen3: 조립도 폴링도 없다. mtk_pcie_config_tlp_header() 가 BDF 와
 *           byte enable 을 레지스터 하나(PCIE_CFGNUM_REG)에 써 두면,
 *           고정 창(base + 0x1000 + where)에 대한 평범한 MMIO 접근이 곧
 *           config 사이클이 된다. 그래서 읽기·쓰기 구현이 커널 공통
 *           pci_generic_config_read32/write32 두 줄로 끝난다.
 *           함수 이름에 "tlp_header" 가 남아 있는 것은 그 레지스터가
 *           하드웨어 안에서 TLP 헤더로 쓰이기 때문이며, 소프트웨어가
 *           TLP 를 만드는 것은 아니다.
 *
 *   MSI
 *     v1/v2: 벡터 32개, 평평한 비트맵 하나(MTK_MSI_IRQS_NUM 32),
 *            포트마다 도메인 하나.
 *     gen3 : 벡터 256개를 **여덟 세트로 나눈다**(세트당 32개).
 *            세트마다 자기 레지스터 창(msi_set->base)과 자기 수신 주소
 *            (msg_addr)를 갖고, irq_chip 의 chip_data 가 struct mtk_gen3_pcie
 *            가 아니라 **struct mtk_msi_set** 이다. 그래서 ack/mask/unmask 가
 *            hwirq % 32 로 세트 안의 자리만 계산하면 된다.
 *            수신 주소가 세트마다 다르다는 것이 요점 — 장치가 어느 주소에
 *            쓰느냐로 이미 세트가 갈리고, 그 안의 데이터 값이 벡터를 가른다.
 *
 *   INTx
 *     v1/v2: dummy_irq_chip. 하드웨어의 INT_MASK 가 INTx 넷을 한 덩어리로만
 *            다뤄 개별 마스크가 불가능하다.
 *     gen3 : 진짜 irq_chip(mask/unmask/eoi). 활성화 레지스터의 비트 24~27 이
 *            INTA~INTD 에 하나씩 대응해 개별 제어가 된다.
 *            handle_fasteoi_irq 를 쓰고, mtk_intx_eoi() 위의 원문 주석이
 *            "에뮬레이트된 레벨 IRQ" 라 밝힌다.
 *
 *   포트 구조
 *     v1/v2: 컨트롤러 하나가 포트 여럿. struct mtk_pcie 가 포트 목록을 들고
 *            포트마다 PHY·클록·리셋을 따로 갖는다. 포트 하나가 실패해도
 *            나머지를 살린다.
 *     gen3 : 인스턴스 하나가 포트 하나. 목록이 없고 구조가 훨씬 평평하다.
 *
 *   세대 분기
 *     v1/v2: soc_data 로 링크 기동 콜백 등을 갈아 끼운다(같은 파일 안 두 세대).
 *     gen3 : pdata 로 전원 콜백과 리셋 이름 목록을 갈아 끼운다.
 *            MT8192/MT8196 은 mtk_pcie_power_up(), EN7581 은
 *            mtk_pcie_en7581_power_up() 을 쓰며, 후자는 PHY 초기화 순서가
 *            반대이고 PERST 를 건드리지 않는다(SKIP_PCIE_RSTB).
 *
 * === 값의 근거에 대하여 ===
 * 이 파일 위쪽의 레지스터 오프셋과 비트(PCIE_SETTING_REG, PCIE_CFGNUM_REG,
 * PCIE_RST_CTRL_REG, PCIE_LTSSM_ 계열, PCIE_MSI_ 계열, PCIE_ATR_ 계열,
 * PCIE_PIPE4_PIE8_REG 의 이퀄라이저 필드 등)는 모두 이 파일 안에 정의되어
 * 있으나, 그 근거가 되는 미디어텍/에어로하 하드웨어 문서는 이 트리에 없다.
 * 따라서 아래 주석은 값의 의미를 단정하지 않고 **코드가 그 상수를 어떻게
 * 쓰는지**(마스크인지, 필드에 넣는 값인지, 폴링 조건인지, RW1C 인지)로
 * 설명한다. 특히 EN7581 전원 경로의 매직 넘버(0x47, 0x41, 0x80, 0x2, 0xf)는
 * 코드에 근거가 전혀 없어 그렇게 밝혀 두었다.
 * PCI_CLASS_BRIDGE_PCI_NORMAL 과 PCI_NUM_INTX 같은 커널 공통 상수는
 * 해당 헤더가 이 스파스 체크아웃에 없어 값을 확인하지 못했다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 아무 접점이 없다(drivers/nvme 전수 grep 0건).
 * 이 파일은 특정 SoC 의 호스트 컨트롤러 드라이버라, 장치 종류와 무관하게
 * "PCI 버스를 제공" 하는 쪽이기 때문이다.
 *
 * 다만 앞 세대와 달리 이 드라이버는 **링크 속도와 레인 수를 실제로
 * 설정한다**. mtk_pcie_startup_port() 가 DT 의 max-link-speed 와 num-lanes 를
 * 읽어 PCIE_SETTING_REG 와 Link Control 2 에 반영하고,
 * mtk_pcie_get_controller_max_link_speed() 로 하드웨어 상한과 견줘 작은
 * 쪽을 고른다. 그 위에 붙는 NVMe SSD 의 실제 대역폭이 그 설정에 직접
 * 좌우되므로, 이 파일에서 NVMe 독자에게 의미 있는 대목은 그 부분이다.
 * (앞 세대 pcie-mediatek.c 는 그 값들을 아예 읽지 않는다 — 그쪽 헤더의
 *  NVMe 절이 그 사실을 적고 있다.)
 */

/* [한국어] FIELD_PREP / FIELD_GET 을 쓰기 위해서다. 마스크와 값을 함께 넘기면
 * 컴파일 시점에 시프트를 계산해 준다. 링크 속도·레인 폭·이퀄라이저
 * 프리셋 필드가 모두 이 매크로로 조립된다 */
#include <linux/bitfield.h>
/* [한국어] clk_bulk_prepare_enable / clk_bulk_disable_unprepare. 이 컨트롤러는
 * 클록 개수가 SoC 마다 달라 하나씩 다루지 않고 bulk 로 묶어 켜고 끈다 */
#include <linux/clk.h>
/* [한국어] devm_clk_bulk_get_all 이 여기 선언되어 있다. 이름을 미리 알지 못한 채
 * 장치 트리에 적힌 클록을 전부 가져오기 위해 필요하다 */
#include <linux/clk-provider.h>
/* [한국어] msleep / usleep_range. 리셋 유지 시간(10us)과 전원 안정 시간(100ms)을
 * 지키는 데 쓴다. 두 함수 모두 잠들 수 있어 probe 와 PM 경로에서만 쓰인다 */
#include <linux/delay.h>
/* [한국어] readl_poll_timeout. 링크가 서기를 기다리는 폴링과 L2 진입을 기다리는
 * 폴링이 모두 이 매크로 하나로 쓰였다. 조건과 간격, 상한을 인자로 받는다 */
#include <linux/iopoll.h>
/* [한국어] struct irq_chip 과 irq_set_chip_and_handler_name 계열. INTx 와 MSI 의
 * irq_chip 정의가 이 헤더에 의존한다 */
#include <linux/irq.h>
/* [한국어] msi_lib_init_dev_msi_info. MSI 부모 도메인이 자식 도메인 정보를 채울 때
 * 쓰는 공통 구현이며, mtk_msi_parent_ops 가 그것을 그대로 가리킨다.
 * 드라이버가 직접 구현하던 옛 방식을 대신한다 */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] chained_irq_enter / chained_irq_exit. 컨트롤러 인터럽트 하나를 받아
 * 아래 도메인으로 나누는 체인 핸들러가 상위 컨트롤러의 ack/eoi 를 대신
 * 처리하기 위해 필요하다 */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain_create_linear 와 struct irq_domain_ops. INTx 도메인이
 * 이 판을 쓴다 */
#include <linux/irqdomain.h>
/* [한국어] ARRAY_SIZE, min, upper_32_bits / lower_32_bits 등 기본 매크로.
 * 64비트 주소를 32비트 레지스터 두 개로 나눠 쓰는 곳마다 등장한다 */
#include <linux/kernel.h>
/* [한국어] syscon_regmap_lookup_by_phandle_args. EN7581 만 쓴다 — PBus 주소 범위를
 * 별도 레지스터 블록에 알려 줘야 하기 때문이다 */
#include <linux/mfd/syscon.h>
/* [한국어] MODULE_DEVICE_TABLE, MODULE_LICENSE 등. 이 드라이버는 모듈로 빌드될 수 있다 */
#include <linux/module.h>
/* [한국어] struct msi_msg 와 struct msi_parent_ops. MSI 주소·데이터를 조립하는
 * mtk_compose_msi_msg 와 부모 도메인 ops 정의에 필요하다 */
#include <linux/msi.h>
/* [한국어] 장치 트리 매칭 관련. device_get_match_data 로 SoC 별 pdata 를 고른다 */
#include <linux/of_device.h>
/* [한국어] of_pci_get_max_link_speed(drivers/pci/of.c:2174). 장치 트리의
 * max-link-speed 속성을 읽어 링크 속도 상한을 정하는 데 쓴다 */
#include <linux/of_pci.h>
/* [한국어] struct pci_bus, struct pci_ops, pci_generic_config_read32 계열.
 * PCI 코어와 맞물리는 모든 타입이 여기서 온다 */
#include <linux/pci.h>
/* [한국어] pci_pwrctrl_create_devices / _power_on_devices 계열.
 * 슬롯 전원을 별도 드라이버가 관리하게 하는 계층이며, 구현은
 * drivers/pci/pwrctrl/core.c 에 있다 */
#include <linux/pci-pwrctrl.h>
/* [한국어] phy_init / phy_power_on / phy_exit. PHY 를 켜고 끄는 순서가 SoC 마다
 * 달라, 이 API 를 부르는 순서가 두 power_up 구현을 가르는 핵심이다 */
#include <linux/phy/phy.h>
/* [한국어] platform_get_irq, platform_get_resource_byname, module_platform_driver.
 * 이 드라이버는 PCI 장치가 아니라 플랫폼 장치로 등록된다 — 자기 자신이
 * PCI 버스를 제공하는 쪽이기 때문이다 */
#include <linux/platform_device.h>
/* [한국어] 전원 도메인 관련. 이 파일은 pm_runtime 만 직접 쓰지만 도메인 타입이
 * 필요하다 */
#include <linux/pm_domain.h>
/* [한국어] pm_runtime_enable / _get_sync / _put_sync / _disable.
 * 전원 도메인이 실제로 올라오기를 기다린 뒤 클록을 켜기 위해 동기 판을 쓴다 */
#include <linux/pm_runtime.h>
/* [한국어] regmap_write. EN7581 의 PBus 설정에만 쓰인다 */
#include <linux/regmap.h>
/* [한국어] reset_control_bulk_assert / _deassert 와 단일 리셋 API.
 * PHY 리셋은 bulk(SoC 마다 1~3개), MAC 리셋은 하나로 나뉘어 있다 */
#include <linux/reset.h>

/* [한국어] PCI 서브시스템 내부 헤더. 여기서 쓰는 것은 두 상수다 —
 * PCIE_T_PVPERL_MS(drivers/pci/pci.h:107, 100ms)와
 * PCI_PM_D3COLD_WAIT(drivers/pci/pci.h:617, 100ms).
 * 앞의 것은 전원 안정 후 PERST 해제까지의 대기이고, 뒤의 것은 링크가
 * 서기를 기다리는 폴링 상한으로 재사용된다 */
#include "../pci.h"

/* [한국어] 컨트롤러 능력 레지스터. mtk_pcie_get_controller_max_link_speed 가 여기서
 * 하드웨어가 지원하는 최대 링크 속도를 읽는다. 읽기 전용으로만 쓰인다 */
#define PCIE_BASE_CFG_REG		0x14
/* [한국어] 위 레지스터의 속도 필드(비트 15~8). **비트마스크** 이지 숫자가 아니다 —
 * 지원하는 세대마다 비트가 하나씩 서고, fls 로 가장 높은 비트를 찾으면
 * 그것이 최대 세대 번호가 된다 */
#define PCIE_BASE_CFG_SPEED		GENMASK(15, 8)

/* [한국어] 컨트롤러 기본 설정 레지스터. RC/EP 모드, 지원 속도, 링크 폭이 한 곳에
 * 모여 있어 mtk_pcie_startup_port 가 세 값을 한 번에 조립해 쓴다 */
#define PCIE_SETTING_REG		0x80
/* [한국어] 링크 폭 필드(비트 11~8). 0 이 x1 을 뜻하고 비트마다 x2/x4/x8/x16 이
 * 켜진다 — 그래서 num_lanes 가 1 이면 아무 비트도 세우지 않는다 */
#define PCIE_SETTING_LINK_WIDTH		GENMASK(11, 8)
/* [한국어] 지원 세대 필드(비트 14~12). Gen2 부터 표현되므로
 * GENMASK(max_link_speed - 2, 0) 로 만든다. Gen1 전용을 표현할 방법은 없다 */
#define PCIE_SETTING_GEN_SUPPORT	GENMASK(14, 12)
/* [한국어] 컨트롤러 자신의 클래스 코드가 담긴 레지스터. 기본값이 브리지가 아니면
 * PCI 코어가 이 장치를 브리지로 인식하지 못해, mtk_pcie_startup_port 가
 * 부팅 때마다 덮어쓴다 */
#define PCIE_PCI_IDS_1			0x9c
/* [한국어] 클래스 코드를 레지스터 자리로 옮기는 시프트. 위 레지스터의 상위 24비트가
 * 클래스 코드라 8비트 왼쪽으로 민다. 인자를 괄호로 감싸지 않은 매크로지만
 * 호출처가 단일 상수 하나뿐이라 문제가 되지 않는다 */
#define PCI_CLASS(class)		(class << 8)
/* [한국어] Root Complex 모드 비트. 이 비트를 세우지 않으면 컨트롤러가 엔드포인트로
 * 동작한다. 이 파일은 RC 만 다루므로 항상 세운다 */
#define PCIE_RC_MODE			BIT(0)

/* [한국어] 이퀄라이저 프리셋 레지스터. EN7581 전원 경로에서만 쓴다.
 * 오프셋과 필드 구성의 근거 문서는 이 트리에 없다 */
#define PCIE_EQ_PRESET_01_REG		0x100
/* [한국어] 레인 0 의 하류 방향 프리셋 필드(비트 6~0) */
#define PCIE_VAL_LN0_DOWNSTREAM		GENMASK(6, 0)
/* [한국어] 레인 0 의 상류 방향 프리셋 필드(비트 14~8) */
#define PCIE_VAL_LN0_UPSTREAM		GENMASK(14, 8)
/* [한국어] 레인 1 의 하류 방향 프리셋 필드(비트 22~16) */
#define PCIE_VAL_LN1_DOWNSTREAM		GENMASK(22, 16)
/* [한국어] 레인 1 의 상류 방향 프리셋 필드(비트 30~24).
 * 네 필드 모두 mtk_pcie_en7581_power_up 에서 FIELD_PREP 으로 채워지며,
 * 들어가는 값(0x47, 0x41)의 의미는 이 트리에서 확인할 수 없다 */
#define PCIE_VAL_LN1_UPSTREAM		GENMASK(30, 24)

/* [한국어] **이 드라이버 config 접근의 중심 레지스터.** 창을 건드리기 직전에
 * 대상 BDF 와 byte enable 을 여기 써 두면, 하드웨어가 이어지는 창 접근에
 * 그 값을 붙여 config TLP 를 만든다. 앞 세대가 TLP 헤더 세 개를 손으로
 * 조립하던 것을 이 레지스터 하나가 대신한다 */
#define PCIE_CFGNUM_REG			0x140
/* [한국어] 위 레지스터의 devfn 필드(비트 7~0). 상위 5비트가 장치, 하위 3비트가
 * 함수 번호라는 PCI 규약을 그대로 담는다 */
#define PCIE_CFG_DEVFN(devfn)		((devfn) & GENMASK(7, 0))
/* [한국어] 버스 번호 필드(비트 15~8). 8비트 왼쪽으로 민 뒤 마스크로 잘라 낸다 —
 * 인자가 범위를 넘어도 다른 필드를 침범하지 않게 하는 방어다 */
#define PCIE_CFG_BUS(bus)		(((bus) << 8) & GENMASK(15, 8))
/* [한국어] byte enable 필드(비트 19~16). 네 비트가 dword 안의 네 바이트에
 * 하나씩 대응하며, mtk_pcie_config_tlp_header 가 size 와 오프셋으로
 * 계산해 넣는다 */
#define PCIE_CFG_BYTE_EN(bytes)		(((bytes) << 16) & GENMASK(19, 16))
/* [한국어] byte enable 강제 비트. 세우지 않으면 하드웨어가 스스로 byte enable 을
 * 정하므로, 소프트웨어가 계산한 값을 쓰게 하려면 반드시 함께 세워야 한다 */
#define PCIE_CFG_FORCE_BYTE_EN		BIT(20)
/* [한국어] **config 창의 오프셋.** mtk_pcie_map_bus 가 돌려주는 주소의 기준이며,
 * 버스나 devfn 과 무관하게 항상 같다. 대상은 위의 CFGNUM 레지스터가 정한다.
 * 컨트롤러 자신의 Link Control 2 도 이 창을 통해 접근한다 */
#define PCIE_CFG_OFFSET_ADDR		0x1000
/* [한국어] 버스와 devfn 을 한 값으로 합치는 매크로. 두 필드가 인접해 있어
 * 한 번의 OR 로 끝난다 */
#define PCIE_CFG_HEADER(bus, devfn) \
	(PCIE_CFG_BUS(bus) | PCIE_CFG_DEVFN(devfn))

/* [한국어] 리셋 제어 레지스터. 네 리셋 신호가 비트 하나씩을 차지한다.
 * **1이 어서트(리셋 유지)** 라는 점에 주의 — mtk_pcie_devices_power_up 이
 * 비트를 세워 리셋에 넣고, 지워서 푼다 */
#define PCIE_RST_CTRL_REG		0x148
/* [한국어] MAC 리셋 비트 */
#define PCIE_MAC_RSTB			BIT(0)
/* [한국어] PHY 리셋 비트 */
#define PCIE_PHY_RSTB			BIT(1)
/* [한국어] 브리지 리셋 비트 */
#define PCIE_BRG_RSTB			BIT(2)
/* [한국어] PERST 리셋 비트. 슬롯에 꽂힌 장치로 나가는 리셋 신호다.
 * EN7581 은 이 비트를 건드리면 링크가 간헐적으로 끊기는 하드웨어 문제가
 * 있어 SKIP_PCIE_RSTB 로 건너뛴다 */
#define PCIE_PE_RSTB			BIT(3)

/* [한국어] LTSSM(링크 훈련 상태 기계) 상태 레지스터. 링크가 안 설 때 원인을
 * 좁히는 유일한 단서이고, L2 진입을 확인하는 근거이기도 하다 */
#define PCIE_LTSSM_STATUS_REG		0x150
/* [한국어] 상태 필드(비트 28~24). 5비트라 0x00~0x1F 를 표현하며, ltssm_str[] 이
 * 그중 0x00~0x1A 에 이름을 붙여 둔다 */
#define PCIE_LTSSM_STATE_MASK		GENMASK(28, 24)
/* [한국어] 상태 값을 꺼내는 매크로. FIELD_GET 대신 직접 시프트하는데, val 을 괄호로
 * 감싸지 않아 복합식을 넘기면 위험하다. 호출처가 모두 단일 변수라 실제
 * 문제는 없다 */
#define PCIE_LTSSM_STATE(val)		((val & PCIE_LTSSM_STATE_MASK) >> 24)
/* [한국어] L2.idle 상태 값. mtk_pcie_turn_off_link 가 이 값에 도달하기를 기다린다.
 * ltssm_str[] 의 0x14 번째 항목이 "L2.idle" 인 것과 일치한다 */
#define PCIE_LTSSM_STATE_L2_IDLE	0x14

/* [한국어] 링크 상태 레지스터 */
#define PCIE_LINK_STATUS_REG		0x154
/* [한국어] 링크 업 비트(비트 8). mtk_pcie_startup_port 가 이 비트가 서기를
 * 20us 간격으로 최대 100ms 폴링한다 */
#define PCIE_PORT_LINKUP		BIT(8)

/* [한국어] **MSI 세트 개수 8.** 이 드라이버 MSI 구조의 출발점이다.
 * 세트마다 자기 레지스터 창과 자기 수신 주소를 갖는다 */
#define PCIE_MSI_SET_NUM		8
/* [한국어] 세트 하나가 담당하는 벡터 수 32. 세트의 활성화·상태 레지스터가
 * 32비트라 자연스럽게 이 값이 된다 */
#define PCIE_MSI_IRQS_PER_SET		32
/* [한국어] 전체 벡터 수 256(8 × 32). MSI 비트맵 크기이자 도메인 크기다.
 * 앞 세대(pcie-mediatek.c)의 32개와 견주면 여덟 배다 */
#define PCIE_MSI_IRQS_NUM \
	(PCIE_MSI_IRQS_PER_SET * PCIE_MSI_SET_NUM)

/* [한국어] 컨트롤러 인터럽트 활성화 레지스터. INTx 비트와 MSI 세트 비트가 한 곳에
 * 있어, INTx 마스크와 MSI 켜기가 같은 레지스터를 읽고-고쳐-쓴다.
 * irq_lock 이 그 경쟁을 막는다 */
#define PCIE_INT_ENABLE_REG		0x180
/* [한국어] MSI 세트 활성화 비트들(비트 8~15). 세트 개수에서 폭을 유도하므로
 * 세트 수가 바뀌어도 자동으로 맞는다 */
#define PCIE_MSI_ENABLE			GENMASK(PCIE_MSI_SET_NUM + 8 - 1, 8)
/* [한국어] MSI 세트 비트의 시작 위치 8. 상태 레지스터에서도 같은 자리를 쓴다 */
#define PCIE_MSI_SHIFT			8
/* [한국어] INTx 비트의 시작 위치 24. INTA 가 24, INTD 가 27 이다 */
#define PCIE_INTX_SHIFT			24
/* [한국어] INTx 활성화 비트들(비트 24~27). PCI_NUM_INTX 로 폭을 잡는데,
 * 그 상수를 정의한 헤더는 이 스파스 체크아웃에 없다. 코드가 INTA~INTD
 * 넷을 다루는 것으로 미루어 4 다 */
#define PCIE_INTX_ENABLE \
	GENMASK(PCIE_INTX_SHIFT + PCI_NUM_INTX - 1, PCIE_INTX_SHIFT)

/* [한국어] 컨트롤러 인터럽트 상태 레지스터. 활성화 레지스터와 비트 배치가 같다.
 * **1을 쓰면 지워진다** — mtk_intx_eoi 와 mtk_pcie_irq_handler 가
 * 읽지 않고 비트만 쓰는 데서 알 수 있다 */
#define PCIE_INT_STATUS_REG		0x184
/* [한국어] MSI 세트 자체를 켜는 레지스터. 위의 INT_ENABLE 과 별개의 층이다 —
 * 세트를 켜고(이 레지스터), 컨트롤러 인터럽트에서 MSI 를 켜야(INT_ENABLE)
 * 비로소 전달된다 */
#define PCIE_MSI_SET_ENABLE_REG		0x190
/* [한국어] 세트 여덟 개를 모두 켜는 마스크(비트 0~7).
 * mtk_pcie_enable_msi 가 한 번에 전부 켠다 — 세트 단위로 끄는 경로는 없다 */
#define PCIE_MSI_SET_ENABLE		GENMASK(PCIE_MSI_SET_NUM - 1, 0)

/* [한국어] PIPE4/PIE8 레지스터. EN7581 전원 경로에서만 쓰인다. 이름과 필드로 미루어
 * PHY 파라미터·프리셋 관련이지만, 근거 문서는 이 트리에 없다 */
#define PCIE_PIPE4_PIE8_REG		0x338
/* [한국어] 파인튜닝 최대값 필드(비트 5~0). 0xf 가 들어간다 */
#define PCIE_K_FINETUNE_MAX		GENMASK(5, 0)
/* [한국어] 파인튜닝 오류 필드(비트 7~6). **이 파일 어디에서도 쓰이지 않는다** —
 * 레지스터 지도를 완전하게 적어 둔 것으로 보인다 */
#define PCIE_K_FINETUNE_ERR		GENMASK(7, 6)
/* [한국어] 사용할 프리셋 필드(비트 18~8). 0x2 가 들어간다 */
#define PCIE_K_PRESET_TO_USE		GENMASK(18, 8)
/* [한국어] PHY 파라미터 질의 비트(비트 19). 값 없이 비트만 세운다 */
#define PCIE_K_PHYPARAM_QUERY		BIT(19)
/* [한국어] 질의 시간 초과 비트(비트 20). 역시 비트만 세운다 */
#define PCIE_K_QUERY_TIMEOUT		BIT(20)
/* [한국어] 16GT/s 용 프리셋 필드(비트 31~21). 0x80 이 들어간다.
 * 이 네 필드에 들어가는 숫자들의 근거는 이 트리에서 확인할 수 없어,
 * 무엇을 뜻하는지 단정하지 않는다 */
#define PCIE_K_PRESET_TO_USE_16G	GENMASK(31, 21)

/* [한국어] MSI 세트 레지스터 블록의 시작 오프셋. 세트마다 이 값에서
 * PCIE_MSI_SET_OFFSET 만큼 떨어진 곳에 자기 창을 갖는다 */
#define PCIE_MSI_SET_BASE_REG		0xc00
/* [한국어] 세트 사이의 간격 0x10(16바이트). 세트 하나가 레지스터 네 개를 쓰는 셈이다 */
#define PCIE_MSI_SET_OFFSET		0x10
/* [한국어] 세트 창 안에서 상태 레지스터의 위치(+0x04). 도착한 MSI 의 비트가 서고,
 * 1을 쓰면 지워진다(mtk_msi_bottom_irq_ack 참조) */
#define PCIE_MSI_SET_STATUS_OFFSET	0x04
/* [한국어] 세트 창 안에서 활성화 레지스터의 위치(+0x08). 벡터별 마스크 비트가
 * 여기 있고, 읽고-고쳐-쓰기라 irq_lock 이 필요하다.
 * 세트 창의 +0x00 은 MSI 수신 주소의 하위 32비트다 */
#define PCIE_MSI_SET_ENABLE_OFFSET	0x08

/* [한국어] MSI 수신 주소의 **상위** 32비트를 모아 둔 별도 블록.
 * 하위 32비트는 세트 창 안(+0x00)에 있는데 상위는 여기 따로 있는 비대칭
 * 구조라, mtk_pcie_enable_msi 가 두 곳에 나눠 쓴다 */
#define PCIE_MSI_SET_ADDR_HI_BASE	0xc80
/* [한국어] 위 블록 안에서 세트 사이의 간격 0x04. 세트마다 32비트 하나씩이다 */
#define PCIE_MSI_SET_ADDR_HI_OFFSET	0x04

/* [한국어] 자원 제어 레지스터. MT8196 만 이 레지스터를 건드린다 */
#define PCIE_RESOURCE_CTRL_REG		0xd2c
/* [한국어] 시스템 클록 준비 시간 필드(비트 7~0). pdata 의 sys_clk_rdy_time_us 가
 * 있을 때만 덮어쓰며, 원문 주석은 글리치를 피하기 위해서라고 한다 */
#define PCIE_RSRC_SYS_CLK_RDY_TIME_MASK	GENMASK(7, 0)

/* [한국어] 전원 관리 명령 레지스터 */
#define PCIE_ICMD_PM_REG		0x198
/* [한국어] 링크 끄기 비트(비트 4). 세우면 컨트롤러가 L2 진입 흐름을 시작한다.
 * 소프트웨어는 LTSSM 상태로 결과만 확인한다 */
#define PCIE_TURN_OFF_LINK		BIT(4)

/* [한국어] 기타 제어 레지스터 */
#define PCIE_MISC_CTRL_REG		0x348
/* [한국어] DVFSRC 전압 요청 비활성화 비트(비트 1). DVFSRC 는 미디어텍의 동적
 * 전압·주파수 조정 자원 제어기이며, 이 비트를 세워 PCIe 가 그쪽에
 * 전압을 요구하지 않게 한다. mtk_pcie_startup_port 가 항상 세운다 —
 * SoC 별 분기가 없다 */
#define PCIE_DISABLE_DVFSRC_VLT_REQ	BIT(1)

/* [한국어] 주소 변환 표 블록의 시작 오프셋. 표 여덟 개가 여기서부터 이어진다 */
#define PCIE_TRANS_TABLE_BASE_REG	0x800
/* [한국어] 표 안에서 CPU 주소 상위 32비트의 위치(+0x4).
 * 하위 32비트는 +0x00 에 있고, 거기에는 크기 필드와 EN 비트가 겹쳐 있다 */
#define PCIE_ATR_SRC_ADDR_MSB_OFFSET	0x4
/* [한국어] 표 안에서 PCI 주소 하위 32비트의 위치(+0x8) */
#define PCIE_ATR_TRSL_ADDR_LSB_OFFSET	0x8
/* [한국어] 표 안에서 PCI 주소 상위 32비트의 위치(+0xc) */
#define PCIE_ATR_TRSL_ADDR_MSB_OFFSET	0xc
/* [한국어] 표 안에서 종류 필드의 위치(+0x10). MEM/IO 구분과 내보낼 TLP 종류가
 * 함께 들어간다. **마지막에 쓴다** — 주소가 다 채워지기 전에 표가
 * 동작하지 않게 하려는 순서로 보인다 */
#define PCIE_ATR_TRSL_PARAM_OFFSET	0x10
/* [한국어] 표 하나의 크기 0x20(32바이트). 표 번호에 이 값을 곱해 창 주소를 구한다.
 * 실제로 쓰는 레지스터는 다섯 개(20바이트)이고 나머지는 예약이다 */
#define PCIE_ATR_TLB_SET_OFFSET		0x20

/* [한국어] 표 개수 8. 장치 트리의 창을 2의 거듭제곱으로 쪼개다 이 개수를 다 쓰면
 * 남은 영역은 경고만 찍고 포기한다 — 실패로 다루지 않는다 */
#define PCIE_MAX_TRANS_TABLES		8
/* [한국어] 표 활성화 비트(비트 0). 크기 필드와 같은 레지스터에 있어
 * PCIE_ATR_SIZE 가 둘을 함께 만든다 */
#define PCIE_ATR_EN			BIT(0)
/* [한국어] 크기 필드와 활성화 비트를 함께 만드는 매크로.
 * 인자로 받는 size 는 바이트 수가 아니라 **2의 지수** 다 —
 * 호출처가 fls(table_size) - 1 을 넘긴다. (size-1) << 1 로 비트 6~1 에
 * 넣고 EN 비트를 겹친다. 즉 표의 크기는 2^(필드값+1) 바이트가 된다 */
#define PCIE_ATR_SIZE(size) \
	(((((size) - 1) << 1) & GENMASK(6, 1)) | PCIE_ATR_EN)
/* [한국어] 표 종류 필드(비트 3~0)를 만드는 매크로 */
#define PCIE_ATR_ID(id)			((id) & GENMASK(3, 0))
/* [한국어] 메모리 공간 표(0) */
#define PCIE_ATR_TYPE_MEM		PCIE_ATR_ID(0)
/* [한국어] IO 공간 표(1). 위 둘은 표 자체의 종류이고, 아래의 TLP_TYPE 은
 * 내보낼 TLP 의 종류다 — 두 필드가 별개라 함께 써야 한다 */
#define PCIE_ATR_TYPE_IO		PCIE_ATR_ID(1)
/* [한국어] TLP 종류 필드(비트 18~16)를 만드는 매크로 */
#define PCIE_ATR_TLP_TYPE(type)		(((type) << 16) & GENMASK(18, 16))
/* [한국어] 메모리 TLP(0). PCIe 규격의 Memory Request 에 해당한다 */
#define PCIE_ATR_TLP_TYPE_MEM		PCIE_ATR_TLP_TYPE(0)
/* [한국어] IO TLP(2). 값이 1 이 아니라 2 인 것은 이 하드웨어의 인코딩이며,
 * 그 근거 문서는 이 트리에 없다 */
#define PCIE_ATR_TLP_TYPE_IO		PCIE_ATR_TLP_TYPE(2)

/* [한국어] PHY 리셋 선의 최대 개수 3. EN7581 이 레인마다 하나씩 셋을 쓰고,
 * MT8192/MT8196 은 하나만 쓴다. 이 값이 pdata 의 id[] 배열과
 * struct mtk_gen3_pcie 의 phy_resets[] 배열 크기를 정한다 */
#define MAX_NUM_PHY_RESETS		3

/* [한국어] 리셋 신호를 어서트한 채 유지하는 시간 10us.
 * mtk_pcie_power_up 이 usleep_range(10, 20) 로 쓴다 — 상한을 두 배로
 * 주는 것은 커널이 타이머를 뭉쳐 처리할 여지를 주기 위한 관례다 */
#define PCIE_MTK_RESET_TIME_US		10

/* Time in ms needed to complete PCIe reset on EN7581 SoC */
/* [한국어] EN7581 전용 리셋 대기 시간 100ms. 위 영문 주석대로 이 SoC 의 리셋이
 * 끝나는 데 필요한 시간이며, 어서트 뒤와 디어서트 뒤에 각각 한 번씩
 * 쉰다. 다른 SoC 의 10us 와 견주면 만 배다 */
#define PCIE_EN7581_RESET_TIME_MS	100

/* [한국어] 전방 선언. 아래 struct mtk_gen3_pcie_pdata 의 power_up 콜백이 이 타입의
 * 포인터를 받는데, 정작 struct mtk_gen3_pcie 는 pdata 를 필드로 갖는다.
 * 두 구조체가 서로를 참조하므로 한쪽을 먼저 선언해 순환을 끊는다 */
struct mtk_gen3_pcie;

/* [한국어] 컨트롤러 자신의 Link Control 2 레지스터 주소.
 * **config 창 안의 오프셋** 이라는 점이 요점이다 — 별도의 전용 레지스터가
 * 아니라, 자기 config 공간을 config 창을 통해 접근한다.
 * 0xb0 이라는 위치는 이 컨트롤러의 PCIe 능력 구조가 놓인 자리에 달려 있다 */
#define PCIE_CONF_LINK2_CTL_STS		(PCIE_CFG_OFFSET_ADDR + 0xb0)
/* [한국어] 목표 링크 속도 필드(비트 3~0). PCIe 규격의 Target Link Speed 에 해당하며,
 * 링크 협상이 목표로 삼을 세대를 정한다. PCIE_SETTING_GEN_SUPPORT 가
 * "할 수 있는 것" 이라면 이쪽은 "하려는 것" 이다 */
#define PCIE_CONF_LINK2_LCR2_LINK_SPEED	GENMASK(3, 0)

/* [한국어] SoC 별 동작 차이를 비트 하나로 표현하는 플래그 모음.
 * pdata 의 flags 필드에 담기며, 지금은 값이 하나뿐이다 */
enum mtk_gen3_pcie_flags {
	/* [한국어] PERST 어서트를 건너뛰라는 플래그.
	 * 설정자: 정적 초기화. mtk_pcie_soc_en7581 만 이 값을 넣는다.
	 * 읽는 자: mtk_pcie_devices_power_up 과 mtk_pcie_devices_power_down 이
	 *   리셋 비트를 건드리기 전에 검사한다.
	 * 값 범위: BIT(0) 하나. 지금은 이 enum 에 다른 값이 없다.
	 * 이유: 위 영문 주석대로 EN7581 에서 PERST 를 어서트/해제하면 링크가
	 *   간헐적으로 끊기는 하드웨어 문제가 있다. 그 SoC 는 리셋을 클록 콜백
	 *   쪽에서 대신 처리한다.
	 * 동기화: 읽기 전용 상수라 불필요 */
	SKIP_PCIE_RSTB	= BIT(0), /* Skip PERST# assertion during device
				   * probing or suspend/resume phase to
				   * avoid hw bugs/issues.
				   */
};

/**
 * struct mtk_gen3_pcie_pdata - differentiate between host generations
 * @power_up: pcie power_up callback
 * @phy_resets: phy reset lines SoC data.
 * @sys_clk_rdy_time_us: System clock ready time override (microseconds)
 * @flags: pcie device flags.
 */
struct mtk_gen3_pcie_pdata {
	/* [한국어] SoC 별 전원 인가 절차를 갈아 끼우는 콜백.
	 * 설정자: 정적 초기화. mtk_pcie_power_up(MT8192/MT8196) 또는
	 *   mtk_pcie_en7581_power_up(EN7581) 중 하나.
	 * 읽는 자: mtk_pcie_setup 과 mtk_pcie_resume_noirq 가 pcie->soc->power_up 으로
	 *   간접 호출한다. 절전에서 깨어날 때도 같은 콜백을 다시 탄다.
	 * 값 범위: NULL 이 될 수 없다 — 세 pdata 모두 값을 채우고, 호출부가
	 *   검사 없이 부른다.
	 * 왜 콜백인가: 두 절차가 PHY 초기화와 리셋 해제의 **순서 자체** 가 달라
	 *   플래그 하나로 표현할 수 없기 때문이다.
	 * 동기화: 읽기 전용 */
	int (*power_up)(struct mtk_gen3_pcie *pcie);
	/* [한국어] PHY 리셋 선 정보를 담는 익명 구조체.
	 * 이름 배열과 개수를 함께 두어, mtk_pcie_parse_port 가 개수만큼 순회하며
	 * struct mtk_gen3_pcie 의 phy_resets[] 에 이름을 복사한다 */
	struct {
		/* [한국어] PHY 리셋 선의 장치 트리 이름들.
		 * 설정자: 정적 초기화. MT8192/MT8196 은 id[0]="phy" 하나,
		 *   EN7581 은 "phy-lane0", "phy-lane1", "phy-lane2" 셋.
		 * 읽는 자: mtk_pcie_parse_port 가 num_resets 개만큼 읽어
		 *   pcie->phy_resets[i].id 에 넣는다.
		 * 값 범위: 배열 크기는 MAX_NUM_PHY_RESETS(3). num_resets 를 넘는 자리는
		 *   NULL 로 남고 읽히지 않는다.
		 * 동기화: 읽기 전용 상수 */
		const char *id[MAX_NUM_PHY_RESETS];
		/* [한국어] 실제로 쓰는 PHY 리셋 선의 개수.
		 * 설정자: 정적 초기화(1 또는 3).
		 * 읽는 자: mtk_pcie_parse_port 의 복사 루프, 그리고 리셋을 어서트하거나
		 *   디어서트하는 모든 곳(두 power_up, mtk_pcie_power_down, mtk_pcie_setup).
		 * 값 범위: 1~MAX_NUM_PHY_RESETS(3).
		 * 동기화: 읽기 전용 상수 */
		int num_resets;
	} phy_resets;
	/* [한국어] 시스템 클록 준비 시간을 덮어쓸 값(마이크로초).
	 * 설정자: 정적 초기화. **MT8196 만 10 을 넣는다.**
	 * 읽는 자: mtk_pcie_startup_port 가 0 이 아닐 때만
	 *   PCIE_RESOURCE_CTRL_REG 의 해당 필드를 고친다.
	 * 값 범위: 0 이면 "건드리지 않음"을 뜻한다. u8 이므로 0~255 이고,
	 *   레지스터 필드도 8비트라 잘림이 없다.
	 * 동기화: 읽기 전용 상수 */
	u8 sys_clk_rdy_time_us;
	/* [한국어] SoC 별 동작 플래그.
	 * 설정자: 정적 초기화. EN7581 만 SKIP_PCIE_RSTB 를 넣고 나머지는 0 이다.
	 * 읽는 자: mtk_pcie_devices_power_up 과 mtk_pcie_devices_power_down.
	 * 값 범위: enum mtk_gen3_pcie_flags 의 조합. 현재는 값이 하나뿐이다.
	 * 동기화: 읽기 전용 상수 */
	u32 flags;
};

/**
 * struct mtk_msi_set - MSI information for each set
 * @base: IO mapped register base
 * @msg_addr: MSI message address
 * @saved_irq_state: IRQ enable state saved at suspend time
 */
struct mtk_msi_set {
	/* [한국어] 이 세트의 레지스터 창을 가리키는 **가상** 주소.
	 * 설정자: mtk_pcie_enable_msi 가 pcie->base 에 세트 오프셋을 더해 계산한다.
	 *   링크를 세울 때마다(probe 와 resume 양쪽) 다시 계산된다.
	 * 읽는 자: mtk_msi_bottom_irq_ack / _mask / _unmask 가 이 주소에서
	 *   상태·활성화 레지스터를 찾고, mtk_pcie_msi_handler 와
	 *   mtk_pcie_irq_save / _irq_restore 도 쓴다.
	 * 값 범위: pcie->base + 0xc00 + (세트번호 × 0x10).
	 * 동기화: 값 자체는 링크 기동 시에만 쓰이고 이후 읽기 전용이다.
	 *   이 주소가 가리키는 레지스터는 irq_lock 으로 보호된다 */
	void __iomem *base;
	/* [한국어] 이 세트의 MSI 수신 **물리** 주소.
	 * 설정자: mtk_pcie_enable_msi 가 pcie->reg_base 에 같은 오프셋을 더해 만들고,
	 *   하드웨어에도 그 값을 써 넣는다(하위는 세트 창 +0x00, 상위는
	 *   PCIE_MSI_SET_ADDR_HI_BASE 블록).
	 * 읽는 자: mtk_compose_msi_msg 가 그대로 MSI 메시지 주소로 쓴다.
	 * 값 범위: base 와 같은 레지스터를 가리키는 물리 주소.
	 * 왜 물리 주소인가: 이 주소는 커널이 접근하는 것이 아니라 **PCIe 장치가**
	 *   쓰기 트랜잭션을 보낼 대상이다. 그래서 가상 주소가 아무 의미가 없다.
	 * 왜 세트마다 다른가: 장치가 쓴 주소로 세트가 갈리고, 쓴 데이터로 세트 안
	 *   벡터가 갈리는 2단 구조이기 때문이다.
	 * 동기화: 링크 기동 시에만 쓰이고 이후 읽기 전용 */
	phys_addr_t msg_addr;
	/* [한국어] 절전 직전 이 세트의 벡터 활성화 상태.
	 * 설정자: mtk_pcie_irq_save 가 세트의 활성화 레지스터를 그대로 읽어 담는다.
	 * 읽는 자: mtk_pcie_irq_restore 가 같은 레지스터에 되쓴다.
	 * 값 범위: 32비트. 각 비트가 세트 안 벡터 하나의 마스크 상태다.
	 * 왜 필요한가: 절전 중 전원이 끊겨 레지스터가 초기화되는데, 어느 벡터가
	 *   켜져 있었는지는 하드웨어에만 있는 정보이기 때문이다.
	 * 동기화: irq_lock 을 쥔 채 읽고 쓴다. 다만 두 함수 모두 noirq 단계에서만
	 *   불려 실질적인 경쟁은 없다 */
	u32 saved_irq_state;
};

/**
 * struct mtk_gen3_pcie - PCIe port information
 * @dev: pointer to PCIe device
 * @base: IO mapped register base
 * @reg_base: physical register base
 * @mac_reset: MAC reset control
 * @phy_resets: PHY reset controllers
 * @phy: PHY controller block
 * @clks: PCIe clocks
 * @num_clks: PCIe clocks count for this port
 * @max_link_speed: Maximum link speed (PCIe Gen) for this port
 * @num_lanes: Number of PCIe lanes for this port
 * @irq: PCIe controller interrupt number
 * @saved_irq_state: IRQ enable state saved at suspend time
 * @irq_lock: lock protecting IRQ register access
 * @intx_domain: legacy INTx IRQ domain
 * @msi_bottom_domain: MSI IRQ bottom domain
 * @msi_sets: MSI sets information
 * @lock: lock protecting IRQ bit map
 * @msi_irq_in_use: bit map for assigned MSI IRQ
 * @soc: pointer to SoC-dependent operations
 */
struct mtk_gen3_pcie {
	/* [한국어] 이 컨트롤러의 플랫폼 device 포인터.
	 * 설정자: mtk_pcie_probe 가 &pdev->dev 로 채운다.
	 * 읽는 자: 거의 모든 함수. 로그(dev_err/dev_dbg), devm 자원 획득,
	 *   pm_runtime, pci_pwrctrl 호출의 첫 인자로 쓰인다.
	 * 값 범위: 유효한 포인터. 드라이버가 살아 있는 동안 NULL 이 되지 않는다.
	 * 동기화: 설정 후 불변이라 불필요 */
	struct device *dev;
	/* [한국어] 컨트롤러 레지스터 창의 **가상** 시작 주소.
	 * 설정자: mtk_pcie_parse_port 가 devm_ioremap_resource 로 얻는다.
	 * 읽는 자: 이 파일의 모든 readl_relaxed / writel_relaxed. config 창
	 *   (mtk_pcie_map_bus), 변환 표, MSI 세트 창이 모두 이 값에서 파생된다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: devm 으로 잡아 remove 때 커널이 놓는다. 절전 중에도 유효하므로
	 *   mtk_pcie_resume_noirq 가 다시 매핑하지 않는다 */
	void __iomem *base;
	/* [한국어] 같은 레지스터 창의 **물리** 시작 주소.
	 * 설정자: mtk_pcie_parse_port 가 regs->start 를 그대로 담는다.
	 * 읽는 자: mtk_pcie_enable_msi 하나뿐이다.
	 * 왜 필요한가: MSI 수신 주소는 커널이 아니라 **PCIe 장치가** 쓰기를 보낼
	 *   대상이라 물리 주소여야 한다. base 를 그대로 알려 주면 장치가 접근할 수
	 *   없는 커널 가상 주소가 되어 버린다.
	 * 동기화: 설정 후 불변 */
	phys_addr_t reg_base;
	/* [한국어] MAC 리셋 제어 핸들.
	 * 설정자: mtk_pcie_parse_port 가 이름 "mac" 으로 optional exclusive 획득.
	 * 읽는 자: mtk_pcie_power_up 과 mtk_pcie_power_down 이 어서트/디어서트한다.
	 *   **mtk_pcie_en7581_power_up 은 쓰지 않는다** — 그 SoC 는 MAC 리셋을
	 *   장치 트리에 두지 않는다.
	 * 값 범위: NULL 일 수 있다(optional). reset_control API 가 NULL 을
	 *   받아들여 아무것도 하지 않으므로 호출부에 검사가 없다.
	 * 동기화: 리셋 자체는 exclusive 라 이 드라이버만 쓴다 */
	struct reset_control *mac_reset;
	/* [한국어] PHY 리셋 제어 핸들들.
	 * 설정자: mtk_pcie_parse_port 가 pdata 의 이름을 복사한 뒤
	 *   devm_reset_control_bulk_get_optional_shared 로 채운다.
	 * 읽는 자: 두 power_up, mtk_pcie_power_down, mtk_pcie_setup 이 bulk API 로
	 *   한꺼번에 어서트/디어서트한다.
	 * 값 범위: 배열 크기는 MAX_NUM_PHY_RESETS(3)이고 실제로는
	 *   soc->phy_resets.num_resets 개만 쓴다.
	 * **shared 인 점이 중요하다** — 같은 리셋 선을 다른 장치와 나눠 쓸 수 있어
	 *   커널이 어서트/디어서트 횟수를 센다. 그래서 mtk_pcie_setup 이 균형을
	 *   맞추려 한 번 디어서트해 둔다.
	 * 동기화: 리셋 프레임워크가 내부 락으로 카운터를 지킨다 */
	struct reset_control_bulk_data phy_resets[MAX_NUM_PHY_RESETS];
	/* [한국어] PCIe PHY 핸들.
	 * 설정자: mtk_pcie_parse_port 가 이름 "pcie-phy" 로 optional 획득.
	 * 읽는 자: 두 power_up 이 phy_init / phy_power_on 으로 켜고,
	 *   mtk_pcie_power_down 이 phy_power_off / phy_exit 로 끈다.
	 * 값 범위: NULL 일 수 있다(optional). PHY API 가 NULL 을 무시한다.
	 * 왜 SoC 마다 순서가 다른가: MT8192 계열은 리셋을 푼 뒤 PHY 를 켜지만
	 *   EN7581 은 그 반대다. mtk_pcie_en7581_power_up 위의 원문 주석이
	 *   그 사실을 명시한다.
	 * 동기화: PHY 프레임워크 내부 락 */
	struct phy *phy;
	/* [한국어] 이 컨트롤러가 쓰는 클록 전부.
	 * 설정자: mtk_pcie_parse_port 가 devm_clk_bulk_get_all 로 얻는다.
	 *   이름을 지정하지 않고 장치 트리에 적힌 것을 통째로 가져온다.
	 * 읽는 자: 두 power_up 이 clk_bulk_prepare_enable 로 켜고,
	 *   mtk_pcie_power_down 이 clk_bulk_disable_unprepare 로 끈다.
	 * 값 범위: num_clks 개의 배열. devm 이 관리한다.
	 * 왜 bulk 인가: 클록 개수와 이름이 SoC 마다 달라 하드코딩할 수 없다.
	 * 동기화: 클록 프레임워크 내부 락 */
	struct clk_bulk_data *clks;
	/* [한국어] 위 배열의 원소 개수.
	 * 설정자: mtk_pcie_parse_port 가 devm_clk_bulk_get_all 의 반환값으로 채운다.
	 * 읽는 자: clk_bulk 계열 호출의 첫 인자.
	 * 값 범위: 0 이상. **음수면 오류** 이고 mtk_pcie_parse_port 가 그 경우
	 *   probe 를 중단한다 — 즉 이후 코드에서는 항상 0 이상이다.
	 * 동기화: 설정 후 불변 */
	int num_clks;
	/* [한국어] 이 포트에 적용할 최대 링크 속도(PCIe 세대 번호).
	 * 설정자: mtk_pcie_setup 이 장치 트리의 max-link-speed 와 하드웨어 상한을
	 *   견줘, DT 값이 상한 이하일 때만 채운다.
	 * 읽는 자: mtk_pcie_startup_port 가 PCIE_SETTING_GEN_SUPPORT 와
	 *   Link Control 2 의 속도 필드에 반영한다.
	 * 값 범위: 0 이면 "제한하지 않음"을 뜻하고, 1 이상이면 Gen 번호다.
	 *   Gen2 미만은 하드웨어가 제한을 표현하지 못해 실질적으로 2 이상만 의미가 있다.
	 * **NVMe 관점**: 이 값이 그 위에 붙는 SSD 의 대역폭 상한을 직접 정한다.
	 * 동기화: probe 때 한 번 정하고 이후 읽기 전용 */
	u8 max_link_speed;
	/* [한국어] 이 포트에 적용할 링크 폭(레인 수).
	 * 설정자: mtk_pcie_parse_port 가 장치 트리의 num-lanes 를 검증해 채운다.
	 *   값이 잘못됐거나 속성이 없으면 0 으로 남는다.
	 * 읽는 자: mtk_pcie_startup_port 가 PCIE_SETTING_LINK_WIDTH 에 반영한다.
	 * 값 범위: 0(설정 안 함) 또는 1, 2, 4, 8, 16.
	 *   검증 조건이 "1~16 이면서 1이거나 짝수" 라 6 이나 12 도 통과하지만,
	 *   PCIe 링크 폭에 그런 값은 없어 실제로는 나타나지 않는다.
	 * 동기화: probe 때 한 번 정하고 이후 읽기 전용 */
	u8 num_lanes;

	/* [한국어] 컨트롤러 인터럽트의 가상 IRQ 번호.
	 * 설정자: mtk_pcie_setup_irq 가 platform_get_irq 로 얻는다.
	 * 읽는 자: irq_set_chained_handler_and_data 로 핸들러를 걸 때와,
	 *   mtk_pcie_irq_teardown 이 그것을 떼고 매핑을 놓을 때.
	 * 값 범위: 양수. 음수면 오류이고 그 경우 probe 가 중단된다
	 *   (-EPROBE_DEFER 일 수 있다).
	 * 왜 하나뿐인가: INTx 넷과 MSI 256개가 모두 이 선 하나로 올라오고,
	 *   mtk_pcie_irq_handler 가 그것을 갈라 준다.
	 * 동기화: 설정 후 불변 */
	int irq;
	/* [한국어] 절전 직전 컨트롤러 전역 인터럽트 활성화 상태.
	 * 설정자: mtk_pcie_irq_save 가 PCIE_INT_ENABLE_REG 를 그대로 읽어 담는다.
	 * 읽는 자: mtk_pcie_irq_restore 가 같은 레지스터에 되쓴다.
	 * 값 범위: 32비트. 비트 8~15 가 MSI 세트, 24~27 이 INTx 다.
	 * 세트 안 개별 벡터의 상태는 여기 없고 각 mtk_msi_set 의 같은 이름 필드에
	 * 따로 저장된다 — 두 층을 모두 보관해야 복원이 완전해진다.
	 * 동기화: irq_lock 을 쥔 채 접근. 다만 noirq 단계에서만 쓰여 경쟁은 없다 */
	u32 saved_irq_state;
	/* [한국어] 인터럽트 레지스터 접근을 지키는 raw 스핀락.
	 * 설정자: mtk_pcie_init_irq_domains 가 초기화한다.
	 * 읽는 자(쥐는 곳): mtk_intx_mask, mtk_intx_unmask,
	 *   mtk_msi_bottom_irq_mask, mtk_msi_bottom_irq_unmask,
	 *   mtk_pcie_irq_save, mtk_pcie_irq_restore.
	 * 무엇을 지키는가: **읽고-고쳐-쓰기를 하는 레지스터들** 이다.
	 *   PCIE_INT_ENABLE_REG(INTx 와 MSI 세트가 공유)와 각 세트의 활성화
	 *   레지스터(벡터 32개가 공유). 락이 없으면 한쪽의 변경이 사라진다.
	 *   1을 써서 지우는 상태 레지스터는 읽지 않으므로 락이 필요 없고,
	 *   그래서 mtk_intx_eoi 와 mtk_msi_bottom_irq_ack 에는 락이 없다.
	 * 왜 raw 인가: irq_chip 콜백이 인터럽트 컨텍스트에서 불리고,
	 *   PREEMPT_RT 에서도 잠들면 안 되는 자리이기 때문이다.
	 * 비트맵을 지키는 lock(mutex)과는 별개의 락이다 */
	raw_spinlock_t irq_lock;
	/* [한국어] INTx 인터럽트 도메인.
	 * 설정자: mtk_pcie_init_irq_domains 가 장치 트리의
	 *   "interrupt-controller" 자식 노드를 fwnode 로 삼아 만든다.
	 * 읽는 자: mtk_pcie_irq_handler 가 generic_handle_domain_irq 로 넘길 때,
	 *   mtk_pcie_irq_teardown 이 지울 때.
	 * 값 범위: 유효한 포인터 또는 NULL(생성 실패 시). teardown 이 NULL 을
	 *   검사하는 것은 부분 실패 상태에서도 불릴 수 있기 때문이다.
	 * 크기: PCI_NUM_INTX — INTA~INTD 넷.
	 * 동기화: 도메인 내부는 irqdomain 코어가 지킨다 */
	struct irq_domain *intx_domain;
	/* [한국어] MSI 바닥 인터럽트 도메인.
	 * 설정자: mtk_pcie_init_irq_domains 가 msi_create_parent_irq_domain 으로
	 *   만든다. 그 위의 장치별 도메인은 커널이 mtk_msi_parent_ops 를 보고
	 *   붙여 주므로 드라이버가 직접 만들지 않는다.
	 * 읽는 자: mtk_pcie_msi_handler 가 벡터를 넘길 때,
	 *   mtk_pcie_irq_teardown 이 지울 때.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 크기: PCIE_MSI_IRQS_NUM(256).
	 * host_data 에 struct mtk_gen3_pcie 가 들어 있어
	 *   irq_chip 콜백들이 data->domain->host_data 로 되찾는다.
	 * 동기화: irqdomain 코어 */
	struct irq_domain *msi_bottom_domain;
	/* [한국어] MSI 세트 여덟 개의 정보.
	 * 설정자: mtk_pcie_enable_msi 가 링크를 세울 때마다 base 와 msg_addr 을
	 *   다시 계산해 채운다(probe 와 resume 양쪽).
	 * 읽는 자: mtk_pcie_msi_handler 가 세트 번호로 접근하고,
	 *   mtk_msi_bottom_domain_alloc 이 벡터의 chip_data 로 심는다.
	 *   그 뒤 ack/mask/unmask/compose 는 chip_data 로 직접 받는다.
	 * 값 범위: 배열 크기 PCIE_MSI_SET_NUM(8). 항상 전부 쓴다.
	 * **이 배열이 앞 세대와 갈리는 핵심** 이다. pcie-mediatek.c 의 v1/v2 는
	 *   세트 개념 없이 벡터 32개를 평평하게 다룬다.
	 * 동기화: 필드 자체는 링크 기동 시에만 쓰이고 이후 읽기 전용.
	 *   그것이 가리키는 레지스터는 irq_lock 이 지킨다 */
	struct mtk_msi_set msi_sets[PCIE_MSI_SET_NUM];
	/* [한국어] MSI 비트맵을 지키는 뮤텍스.
	 * 설정자: mtk_pcie_init_irq_domains 가 MSI 준비 직전에 초기화한다.
	 * 읽는 자(쥐는 곳): mtk_msi_bottom_domain_alloc 과 _free 두 곳뿐이다.
	 * 왜 mutex 인가: 이 두 함수는 프로세스 컨텍스트에서만 불리고 잠들어도
	 *   되기 때문이다. 인터럽트 컨텍스트에서 불리는 irq_lock 과 종류를
	 *   달리한 이유가 여기 있다.
	 * 범위가 좁은 점에 주의 — 비트맵 조작 직후 풀고, 이어지는
	 *   irq_domain_set_info 는 락 밖에서 한다 */
	struct mutex lock;
	/* [한국어] 어느 MSI 벡터가 쓰이는지 표시하는 비트맵.
	 * 설정자: mtk_msi_bottom_domain_alloc 이 bitmap_find_free_region 으로
	 *   자리를 잡아 표시하고, _free 가 bitmap_release_region 으로 푼다.
	 * 읽는 자: 위 두 함수뿐이다.
	 * 값 범위: PCIE_MSI_IRQS_NUM(256)비트. 비트 번호가 곧 전역 hwirq 이며,
	 *   32로 나눈 몫이 세트 번호, 나머지가 세트 안의 자리다.
	 * 왜 find_free_region 인가: 다중 MSI 는 벡터가 연속이고 시작이 개수에
	 *   정렬되어야 해서, 단순한 find_first_zero_bit 로는 부족하다.
	 * 동기화: 위의 lock(mutex)이 지킨다 */
	DECLARE_BITMAP(msi_irq_in_use, PCIE_MSI_IRQS_NUM);

	/* [한국어] 이 SoC 의 pdata 포인터.
	 * 설정자: mtk_pcie_probe 가 device_get_match_data 로 얻는다.
	 *   장치 트리의 compatible 이 어느 것과 맞았느냐로 값이 정해진다.
	 * 읽는 자: 전원 절차(soc->power_up), 리셋 이름과 개수
	 *   (soc->phy_resets), 클록 준비 시간(soc->sys_clk_rdy_time_us),
	 *   PERST 건너뛰기(soc->flags).
	 * 값 범위: mtk_pcie_soc_mt8192 / mtk_pcie_soc_mt8196 / mtk_pcie_soc_en7581
	 *   셋 중 하나. of_match_table 에 없는 compatible 로는 probe 자체가
	 *   불리지 않으므로 NULL 이 될 수 없다.
	 * **이 파일의 모든 SoC 분기가 이 포인터 하나를 통과한다.**
	 * 동기화: 설정 후 불변인 읽기 전용 상수 */
	const struct mtk_gen3_pcie_pdata *soc;
};

/* LTSSM state in PCIE_LTSSM_STATUS_REG bit[28:24] */
static const char *const ltssm_str[] = {
	/* [한국어] LTSSM 상태 번호와 이름의 대응표. 인덱스가 곧 상태 번호이고,
	 * 오른쪽 영문 주석이 그 번호를 밝힌다. mtk_pcie_startup_port 가 링크
	 * 실패를 진단할 때 이 표로 번호를 사람이 읽는 이름으로 바꾼다.
	 * 0x10 의 "L0" 가 정상 동작 상태이고, 0x14 의 "L2.idle" 은
	 * mtk_pcie_turn_off_link 가 목표로 삼는 상태다 */
	"detect.quiet",			/* 0x00 */
	"detect.active",		/* 0x01 */
	"polling.active",		/* 0x02 */
	"polling.compliance",		/* 0x03 */
	"polling.configuration",	/* 0x04 */
	"config.linkwidthstart",	/* 0x05 */
	"config.linkwidthaccept",	/* 0x06 */
	"config.lanenumwait",		/* 0x07 */
	"config.lanenumaccept",		/* 0x08 */
	"config.complete",		/* 0x09 */
	"config.idle",			/* 0x0A */
	"recovery.receiverlock",	/* 0x0B */
	"recovery.equalization",	/* 0x0C */
	"recovery.speed",		/* 0x0D */
	"recovery.receiverconfig",	/* 0x0E */
	"recovery.idle",		/* 0x0F */
	"L0",				/* 0x10 */
	"L0s",				/* 0x11 */
	"L1.entry",			/* 0x12 */
	"L1.idle",			/* 0x13 */
	"L2.idle",			/* 0x14 */
	"L2.transmitwake",		/* 0x15 */
	"disable",			/* 0x16 */
	"loopback.entry",		/* 0x17 */
	"loopback.active",		/* 0x18 */
	"loopback.exit",		/* 0x19 */
	"hotreset",			/* 0x1A */
};

/**
 * mtk_pcie_config_tlp_header() - Configure a configuration TLP header
 * @bus: PCI bus to query
 * @devfn: device/function number
 * @where: offset in config space
 * @size: data size in TLP header
 *
 * Set byte enable field and device information in configuration TLP header.
 */
/* [한국어]
 * mtk_pcie_config_tlp_header - config 접근 직전에 대상과 byte enable 을 예고한다
 *
 * @bus:   접근할 PCI 버스. bus->sysdata 에 struct mtk_gen3_pcie 가 들어 있다.
 * @devfn: 장치/함수 번호(상위 5비트 장치, 하위 3비트 함수).
 * @where: config 공간 안의 바이트 오프셋.
 * @size:  이번에 실제로 건드릴 바이트 수(1/2/4).
 * @return: 없음. 실패할 수 있는 동작이 아니다 — 레지스터 한 번 쓰기가 전부다.
 *
 * 이 드라이버의 config 접근이 두 단계인 이유가 여기 있다. 이 컨트롤러는
 * config 창(PCIE_CFG_OFFSET_ADDR)에 대한 MMIO 접근을 config TLP 로 바꿔
 * 내보내는데, 그 TLP 의 "누구에게"(BDF)와 "어느 바이트를"(byte enable)은
 * 창 주소만으로는 표현되지 않는다. 그래서 창을 건드리기 직전에 그 두 정보를
 * PCIE_CFGNUM_REG 에 미리 적어 둔다. 하드웨어는 이어지는 창 접근에 이 값을
 * 붙여 TLP 를 만든다.
 *
 * 동작:
 *   1) bus->sysdata 로 컨트롤러 상태를 되찾는다. PCI 코어가 host->sysdata 에
 *      넣어 둔 값이 버스마다 그대로 전달된다(mtk_pcie_probe 에서 설정).
 *   2) byte enable 4비트를 만든다. GENMASK(size-1, 0) 이 크기만큼의 연속 비트를
 *      만들고, (where & 0x3) 만큼 왼쪽으로 밀어 dword 안의 위치를 맞춘다.
 *      예: where=0x6, size=2 이면 0b0011 << 2 = 0b1100 — dword 의 상위 두 바이트.
 *   3) FORCE_BYTE_EN 비트를 세워 "byte enable 을 자동 계산하지 말고 내가 준
 *      값을 쓰라" 고 지시하고, 버스·devfn 을 합쳐 한 번에 쓴다.
 *
 * 실행 컨텍스트: PCI 코어가 config 락(pci_lock)을 쥔 채 부르므로 잠들 수
 * 없다. 이 함수에는 대기가 없다.
 * 재진입: 이 레지스터는 컨트롤러 전역이라 두 CPU 가 서로 다른 BDF 를 동시에
 * 접근하면 덮어쓸 수 있다. 그러나 PCI 코어의 config 락이 그 동시성을 막아
 * 주므로 드라이버가 따로 락을 걸지 않는다.
 *
 * 호출 체인:
 *   PCI 코어 config 접근 → mtk_pcie_config_read / mtk_pcie_config_write
 *     → [이 함수] → writel_relaxed(PCIE_CFGNUM_REG)
 */
static void mtk_pcie_config_tlp_header(struct pci_bus *bus, unsigned int devfn,
					int where, int size)
{
	struct mtk_gen3_pcie *pcie = bus->sysdata;
	/* [한국어] 계산할 byte enable 비트열 */
	int bytes;
	/* [한국어] 레지스터에 쓸 값 */
	u32 val;

	/* [한국어] **byte enable 4비트를 만든다.** GENMASK(size-1, 0) 이 크기만큼의 연속
	 * 비트를 만들고, dword 안의 바이트 오프셋만큼 왼쪽으로 민다.
	 * 예: where=0x6, size=2 이면 0b0011 << 2 = 0b1100 — dword 의 상위 두 바이트다.
	 * & 0xf 는 size 가 4 일 때 GENMASK(3,0) 이 이미 4비트라 여분을 자르는
	 * 방어이며, 시프트 뒤에도 4비트 필드를 넘지 않게 한다 */
	bytes = (GENMASK(size - 1, 0) & 0xf) << (where & 0x3);

	/* [한국어] byte enable 강제 비트를 세우고 계산한 byte enable 을 넣는다.
	 * 강제 비트가 없으면 하드웨어가 스스로 byte enable 을 정해 버린다 */
	val = PCIE_CFG_FORCE_BYTE_EN | PCIE_CFG_BYTE_EN(bytes) |
	      /* [한국어] 버스 번호와 devfn 을 합친다. 이 값이 TLP 의 목적지가 된다 */
	      PCIE_CFG_HEADER(bus->number, devfn);

	writel_relaxed(val, pcie->base + PCIE_CFGNUM_REG);
}

/* [한국어]
 * mtk_pcie_map_bus - config 창의 가상 주소를 돌려준다 (pci_ops.map_bus)
 *
 * @bus:   접근할 버스. 여기서는 쓰이지 않고 sysdata 만 꺼낸다.
 * @devfn: 장치/함수 번호. **이 함수는 이 값을 쓰지 않는다.**
 * @where: config 공간 오프셋.
 * @return: 접근할 MMIO 주소. NULL 을 돌려주는 경로가 없어 실패하지 않는다.
 *
 * 이 드라이버의 config 접근에서 가장 눈에 띄는 대목이다. 보통의 map_bus 는
 * 버스·devfn·오프셋을 모두 주소로 인코딩하지만, 여기서는 **오프셋만** 주소에
 * 반영한다. 버스와 devfn 은 이미 mtk_pcie_config_tlp_header 가 레지스터에
 * 써 두었기 때문이다. 그래서 어느 장치를 접근하든 창 주소는 같고, 대상은
 * 직전에 쓴 레지스터 값으로 갈린다.
 *
 * 이 구조는 순서 의존을 만든다 — 창에 접근하기 전에 반드시 헤더 레지스터를
 * 써야 한다. read/write 콜백이 그 순서를 지키는 유일한 진입점이다.
 *
 * 주의: pci_generic_config_read32 와 pci_generic_config_write32
 * (drivers/pci/access.c:431, :496)는 map_bus 를 부를 때 오프셋을
 * where & ~0x3 으로 정렬해 넘긴다. 따라서 이 함수가 받는 where 는 항상
 * dword 정렬되어 있고, 반환 주소도 dword 정렬이다.
 *
 * 실행 컨텍스트: config 락 안. 잠들 수 없다.
 *
 * 호출 체인:
 *   pci_generic_config_read32 / _write32 → [이 함수]
 */
static void __iomem *mtk_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				      int where)
{
	struct mtk_gen3_pcie *pcie = bus->sysdata;

	return pcie->base + PCIE_CFG_OFFSET_ADDR + where;
}

/* [한국어]
 * mtk_pcie_config_read - config 읽기 (pci_ops.read)
 *
 * @bus:   대상 버스.
 * @devfn: 대상 장치/함수.
 * @where: config 오프셋.
 * @size:  읽을 바이트 수(1/2/4).
 * @val:   읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND. 커널 공통 함수가
 *   정하는 값을 그대로 전달한다.
 *
 * 읽기는 두 줄이면 끝난다. 대상과 byte enable 을 예고한 뒤, 커널 공통
 * 구현에 넘긴다. 앞 세대(pcie-mediatek.c 의 v2)가 TLP 헤더 세 개를 손으로
 * 조립하고 완료(CPLD)를 폴링하던 것과 대비된다 — 그쪽은 함수 하나가 수십
 * 줄이다.
 *
 * pci_generic_config_read32(drivers/pci/access.c:431)는 항상 32비트를 읽은 뒤
 * size 가 1/2 이면 소프트웨어로 잘라 낸다. 읽기는 부작용이 없으므로 필요보다
 * 넓게 읽어도 무방하다 — 쓰기 쪽이 훨씬 까다로운 이유다.
 *
 * 실행 컨텍스트: PCI 코어의 config 락 안. 잠들 수 없다.
 *
 * 호출 체인:
 *   pci_read_config_ 계열 → PCI 코어 → mtk_pcie_ops.read → [이 함수]
 *     → mtk_pcie_config_tlp_header, pci_generic_config_read32
 */
static int mtk_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 *val)
{
	mtk_pcie_config_tlp_header(bus, devfn, where, size);

	return pci_generic_config_read32(bus, devfn, where, size, val);
}

/* [한국어]
 * mtk_pcie_config_write - config 쓰기 (pci_ops.write)
 *
 * @bus:   대상 버스.
 * @devfn: 대상 장치/함수.
 * @where: config 오프셋.
 * @size:  쓸 바이트 수(1/2/4).
 * @val:   쓸 값. size 만큼의 하위 비트에 담겨 온다.
 * @return: 커널 공통 함수의 반환값.
 *
**size 를 4 로 바꿔 넘기는 것이 이 함수의 핵심**이다. 이유는 두 겹이다.
 *
 * 첫째, 하드웨어가 이미 바이트 단위를 안다. 직전에 쓴 byte enable 이 dword
 * 안에서 어느 바이트가 실제로 갱신될지 정하므로, 소프트웨어는 dword 를
 * 통째로 던져도 원하지 않는 바이트는 건드려지지 않는다.
 *
 * 둘째, 커널 공통 구현의 부분 쓰기 경로를 피하기 위해서다.
 * pci_generic_config_write32(drivers/pci/access.c:496)는 size 가 4 면
 * writel 한 번으로 끝내지만, 1/2 면 읽고-고쳐-쓰기를 한다. config 공간에는
 * 읽으면 지워지는 비트(RW1C 등)가 있어 그 방식이 위험하고, 그래서 그 경로는
 * 경고까지 찍는다. 여기서는 하드웨어 byte enable 이 있으므로 그 위험을
 * 감수할 이유가 없다.
 *
 * 그 대신 소프트웨어가 값을 제자리에 밀어 넣어야 한다 — size 가 1/2 이면
 * val 을 (where & 0x3) 바이트만큼, 즉 그 8배 비트만큼 왼쪽으로 민다.
 * byte enable 이 가리키는 자리와 값의 자리가 맞아떨어져야 하기 때문이다.
 *
 * 실행 컨텍스트: config 락 안. 잠들 수 없다.
 *
 * 호출 체인:
 *   pci_write_config_ 계열 → PCI 코어 → mtk_pcie_ops.write → [이 함수]
 *     → mtk_pcie_config_tlp_header, pci_generic_config_write32
 */
static int mtk_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 val)
{
	mtk_pcie_config_tlp_header(bus, devfn, where, size);

	/* [한국어] 부분 쓰기일 때만 자리를 맞춘다 */
	if (size <= 2)
		/* [한국어] 값을 dword 안의 제자리로 민다. byte enable 이 가리키는 자리와 값의
		 * 자리가 맞아떨어져야 하기 때문이다. 바이트 오프셋의 8배만큼 왼쪽으로 민다 */
		val <<= (where & 0x3) * 8;

	/* [한국어] **size 를 4 로 바꿔 넘긴다.** 하드웨어 byte enable 이 이미 어느 바이트를
	 * 갱신할지 정했으므로 dword 를 통째로 써도 안전하고, 커널 공통 구현의
	 * 부분 쓰기 경로(읽고-고쳐-쓰기, config 공간에서는 위험해 경고까지 찍는다)를
	 * 피할 수 있다 */
	return pci_generic_config_write32(bus, devfn, where, 4, val);
}

/* [한국어] PCI 코어가 이 하드웨어에 닿는 유일한 통로 */
static struct pci_ops mtk_pcie_ops = {
	/* [한국어] **map_bus 가 버스·devfn 을 쓰지 않는 것** 이 이 드라이버 config 접근의
	 * 특징이다. 대상은 read/write 가 먼저 쓴 레지스터가 정한다 */
	.map_bus = mtk_pcie_map_bus,
	.read  = mtk_pcie_config_read,
	.write = mtk_pcie_config_write,
};

/* [한국어]
 * mtk_pcie_set_trans_table - CPU 주소 창 하나를 변환 표 여러 개로 나눠 채운다
 *
 * @pcie:     컨트롤러 상태.
 * @cpu_addr: CPU 쪽 시작 주소(호스트가 접근하는 주소).
 * @pci_addr: PCI 쪽 시작 주소(버스 위로 나갈 주소).
 * @size:     창의 크기.
 * @type:     IORESOURCE_IO 또는 IORESOURCE_MEM. TLP 종류를 가른다.
 * @num:      **입출력 겸용.** 다음에 쓸 표 번호가 들어오고, 이 함수가 쓴
 *   개수만큼 늘어나 나온다. 호출자가 여러 창을 연달아 넣을 때 번호가
 *   이어지도록 하는 장치다.
 * @return: 0 성공. 표 크기가 4KiB 미만이면 -EINVAL.
 *
 * 이 하드웨어의 주소 변환 표는 **크기가 2의 거듭제곱이어야** 하고
**시작 주소가 그 크기에 정렬되어야** 한다. 그런데 장치 트리가 주는 창은
 * 그런 제약이 없다. 그래서 창 하나를 표 여러 개로 쪼개 덮는다.
 *
 * 한 바퀴가 하는 일:
 *   1) 남은 크기 이하의 가장 큰 2의 거듭제곱을 고른다 — BIT(fls(remaining)-1).
 *   2) 시작 주소의 정렬 한계를 구한다 — BIT(ffs(cpu_addr)-1) 는 그 주소를
 *      나누는 가장 큰 2의 거듭제곱이다. 표 크기를 그보다 크게 잡을 수 없으니
 *      둘 중 작은 쪽을 쓴다. cpu_addr 이 0 이면 ffs 가 정의되지 않으므로
 *      0보다 클 때만 계산한다.
 *   3) 4KiB 미만이면 하드웨어가 표현할 수 없어 실패로 끝낸다.
 *   4) 표 하나에 네 값을 쓴다 — CPU 주소 하위(+크기 필드와 EN 비트를 겹쳐서),
 *      CPU 주소 상위, PCI 주소 하위, PCI 주소 상위.
 *   5) 마지막으로 종류(MEM/IO)를 쓴다. 여기에는 두 필드가 함께 들어간다 —
 *      표 자체의 종류(PCIE_ATR_TYPE_ 계열)와 내보낼 TLP 의 종류
 *      (PCIE_ATR_TLP_TYPE_ 계열).
 *   6) 주소와 남은 크기를 표 크기만큼 진행시키고 표 번호를 늘린다.
 *
 * 표는 여덟 개(PCIE_MAX_TRANS_TABLES)뿐이라 다 쓰고도 남으면 경고만 찍고
 * 성공으로 돌아간다. 덮이지 않은 주소 영역은 그냥 접근되지 않는다 — 링크
 * 자체를 못 세울 만한 오류는 아니므로 실패로 다루지 않는 선택이다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). 링크가 서기 전에 불리므로
 * 경쟁 상대가 없다.
 *
 * 호출 체인:
 *   mtk_pcie_startup_port → [이 함수] → writel_relaxed(변환 표 레지스터들)
 */
static int mtk_pcie_set_trans_table(struct mtk_gen3_pcie *pcie,
				    resource_size_t cpu_addr,
				    resource_size_t pci_addr,
				    resource_size_t size,
				    unsigned long type, int *num)
{
	resource_size_t remaining = size;
	/* [한국어] 이번 바퀴에 만들 표의 크기 */
	resource_size_t table_size;
	/* [한국어] 시작 주소가 허용하는 최대 표 크기 */
	resource_size_t addr_align;
	/* [한국어] 로그에 찍을 종류 이름 */
	const char *range_type;
	/* [한국어] 이 표의 레지스터 창 주소 */
	void __iomem *table;
	/* [한국어] 레지스터에 쓸 종류 값 */
	u32 val;

	while (remaining && (*num < PCIE_MAX_TRANS_TABLES)) {
		/* Table size needs to be a power of 2 */
		table_size = BIT(fls(remaining) - 1);

		/* [한국어] **cpu_addr 이 0 이면 건너뛴다.** ffs(0) 이 정의되지 않기 때문이며,
		 * 주소가 0 이면 정렬 제약도 없다 */
		if (cpu_addr > 0) {
			/* [한국어] 시작 주소의 정렬 한계를 구한다. ffs 로 가장 낮은 선 비트를 찾으면
			 * 그것이 그 주소를 나누는 가장 큰 2의 거듭제곱이다.
			 * 예: 0x3000 이면 0x1000 이 되어 표 크기를 4KiB 이하로 제한한다 */
			addr_align = BIT(ffs(cpu_addr) - 1);
			/* [한국어] 둘 중 작은 쪽을 표 크기로 삼는다. 크기와 정렬 제약을 동시에 만족시키는
			 * 최대값이다 */
			table_size = min(table_size, addr_align);
		}

		/* Minimum size of translate table is 4KiB */
		if (table_size < 0x1000) {
			/* [한국어] 4KiB 미만은 하드웨어가 표현할 수 없다 */
			dev_err(pcie->dev, "illegal table size %#llx\n",
				(unsigned long long)table_size);
			return -EINVAL;
		}

		/* [한국어] 이 표의 레지스터 창 주소를 구한다. 표 번호에 표 간격(0x20)을 곱한다 */
		table = pcie->base + PCIE_TRANS_TABLE_BASE_REG + *num * PCIE_ATR_TLB_SET_OFFSET;
		/* [한국어] CPU 주소 하위와 크기·활성화를 한 레지스터에 함께 쓴다.
		 * 주소가 표 크기에 정렬되어 있어 하위 비트가 비고, 거기에 크기 필드가
		 * 들어가는 구조다. PCIE_ATR_SIZE 에 넘기는 fls(table_size) - 1 은
		 * 바이트 수가 아니라 **2의 지수** 다 */
		writel_relaxed(lower_32_bits(cpu_addr) | PCIE_ATR_SIZE(fls(table_size) - 1), table);
		/* [한국어] CPU 주소 상위 32비트 */
		writel_relaxed(upper_32_bits(cpu_addr), table + PCIE_ATR_SRC_ADDR_MSB_OFFSET);
		/* [한국어] PCI 쪽 주소 하위 32비트. 이 표에 걸린 CPU 주소는 이 주소로 바뀌어
		 * PCIe 버스로 나간다 */
		writel_relaxed(lower_32_bits(pci_addr), table + PCIE_ATR_TRSL_ADDR_LSB_OFFSET);
		/* [한국어] PCI 쪽 주소 상위 32비트 */
		writel_relaxed(upper_32_bits(pci_addr), table + PCIE_ATR_TRSL_ADDR_MSB_OFFSET);

		/* [한국어] IO 창과 메모리 창의 종류가 갈린다 */
		if (type == IORESOURCE_IO) {
			/* [한국어] IO 표와 IO TLP */
			val = PCIE_ATR_TYPE_IO | PCIE_ATR_TLP_TYPE_IO;
			/* [한국어] 로그에 찍을 이름 */
			range_type = "IO";
		} else {
			/* [한국어] 메모리 표와 메모리 TLP. 두 필드가 별개라 함께 넣어야 한다 */
			val = PCIE_ATR_TYPE_MEM | PCIE_ATR_TLP_TYPE_MEM;
			/* [한국어] 로그에 찍을 이름 */
			range_type = "MEM";
		}

		/* [한국어] **종류를 마지막에 쓴다.** 주소가 다 채워지기 전에 표가 동작하지 않게
		 * 하려는 순서로 보인다 */
		writel_relaxed(val, table + PCIE_ATR_TRSL_PARAM_OFFSET);

		/* [한국어] 설정한 표를 디버그 로그로 남긴다. 창 하나가 표 몇 개로 쪼개졌는지
		 * 확인할 수 있다 */
		dev_dbg(pcie->dev, "set %s trans window[%d]: cpu_addr = %#llx, pci_addr = %#llx, size = %#llx\n",
			range_type, *num, (unsigned long long)cpu_addr,
			(unsigned long long)pci_addr,
			(unsigned long long)table_size);

		/* [한국어] 다음 표가 덮을 CPU 주소로 진행한다 */
		cpu_addr += table_size;
		/* [한국어] PCI 쪽 주소도 같은 만큼 진행한다 */
		pci_addr += table_size;
		/* [한국어] 남은 크기를 줄인다. 이 값이 0 이 되면 루프가 끝난다 */
		remaining -= table_size;
		/* [한국어] 다음 표 번호로 넘어간다. 참조로 받았으므로 호출자에게도 반영된다 */
		(*num)++;
	}

	/* [한국어] 루프가 표 개수 제한으로 끝났으면 남은 크기가 0 이 아니다 */
	if (remaining)
		/* [한국어] 표를 다 쓰고도 남으면 **경고만 찍고 성공으로 돌아간다.** 덮이지 않은
		 * 영역은 접근되지 않을 뿐이고 링크 자체는 설 수 있으므로, 실패로 다루지
		 * 않는 선택이다 */
		dev_warn(pcie->dev, "not enough translate table for addr: %#llx, limited to [%d]\n",
			 (unsigned long long)cpu_addr, PCIE_MAX_TRANS_TABLES);

	return 0;
}

/* [한국어]
 * mtk_pcie_enable_msi - 여덟 MSI 세트의 수신 주소를 깔고 전부 켠다
 *
 * @pcie: 컨트롤러 상태. msi_sets[] 를 여기서 처음 채운다.
 * @return: 없음. 레지스터 쓰기만 하므로 실패할 여지가 없다.
 *
 * MSI 는 "장치가 특정 주소에 특정 값을 쓰면 인터럽트" 인 구조다. 그 특정
 * 주소를 정하는 것이 이 함수다.
 *
 * 이 컨트롤러의 특징은 그 주소가 **하나가 아니라 여덟 개**라는 점이다.
 * 세트마다 자기 수신 주소를 갖고, 세트 하나가 벡터 32개를 담당한다
 * (8 × 32 = 256). 장치가 어느 주소에 썼느냐로 세트가 갈리고, 그때 쓴
 * 데이터 값으로 세트 안의 벡터가 갈린다.
 * 앞 세대(pcie-mediatek.c)는 벡터 32개에 평평한 구조였다.
 *
 * 주소를 두 벌 계산하는 이유가 중요하다.
 *   base     = pcie->base + ...     — CPU 가 쓰는 **가상** 주소. 드라이버가
 *              상태·활성화 레지스터를 읽고 쓸 때 쓴다.
 *   msg_addr = pcie->reg_base + ... — 같은 레지스터의 **물리** 주소.
 *              이 값을 장치에게 알려 줘야 장치가 그 주소로 쓸 수 있다.
 *              mtk_compose_msi_msg 가 이 값을 그대로 MSI 메시지에 담는다.
 * 둘은 같은 레지스터를 가리키는 서로 다른 주소다. reg_base 는
 * mtk_pcie_parse_port 가 "pcie-mac" 리소스의 물리 시작 주소로 채워 둔다.
 *
 * 주소를 깐 뒤 두 단계로 켠다 — 세트 자체를 켜고(PCIE_MSI_SET_ENABLE_REG),
 * 컨트롤러 인터럽트에서 MSI 구간을 켠다(PCIE_INT_ENABLE_REG 의 비트 8~15).
 *
 * 실행 컨텍스트: mtk_pcie_startup_port 안이므로 probe 또는 resume 경로다.
 * 아직 인터럽트가 올라오기 전이라 irq_lock 없이 읽고-고쳐-쓰기를 한다.
 * 같은 레지스터를 다루는 mtk_intx_mask 계열은 락을 쥐는데, 그쪽은 실제
 * 동작 중에 불리기 때문이다.
 *
 * 호출 체인:
 *   mtk_pcie_startup_port → [이 함수]
 */
static void mtk_pcie_enable_msi(struct mtk_gen3_pcie *pcie)
{
	int i;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* [한국어] 세트 여덟 개를 모두 초기화한다 */
	for (i = 0; i < PCIE_MSI_SET_NUM; i++) {
		/* [한국어] 세트 하나를 잡는다 */
		struct mtk_msi_set *msi_set = &pcie->msi_sets[i];

		/* [한국어] 세트의 레지스터 창을 가리키는 가상 주소. 드라이버가 상태·활성화
		 * 레지스터를 읽고 쓸 때 쓴다 */
		msi_set->base = pcie->base + PCIE_MSI_SET_BASE_REG +
				i * PCIE_MSI_SET_OFFSET;
		/* [한국어] **같은 레지스터의 물리 주소.** 장치가 MSI 쓰기를 보낼 대상이라
		 * 가상 주소로는 쓸 수 없다. mtk_compose_msi_msg 가 이 값을 그대로
		 * 메시지에 담고, 아래에서 하드웨어에도 써 넣는다 */
		msi_set->msg_addr = pcie->reg_base + PCIE_MSI_SET_BASE_REG +
				    i * PCIE_MSI_SET_OFFSET;

		/* Configure the MSI capture address */
		writel_relaxed(lower_32_bits(msi_set->msg_addr), msi_set->base);
		writel_relaxed(upper_32_bits(msi_set->msg_addr),
			       pcie->base + PCIE_MSI_SET_ADDR_HI_BASE +
			       i * PCIE_MSI_SET_ADDR_HI_OFFSET);
	}

	/* [한국어] 세트 활성화 레지스터를 읽는다 */
	val = readl_relaxed(pcie->base + PCIE_MSI_SET_ENABLE_REG);
	/* [한국어] 세트 여덟 개를 모두 켠다. 세트 단위로 끄는 경로는 이 파일에 없다 */
	val |= PCIE_MSI_SET_ENABLE;
	/* [한국어] 되쓴다 */
	writel_relaxed(val, pcie->base + PCIE_MSI_SET_ENABLE_REG);

	/* [한국어] 현재 값을 읽는다. INTx 비트를 보존해야 하므로 읽고-고쳐-쓰기를 한다 */
	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	/* [한국어] 컨트롤러 인터럽트에서 MSI 구간을 켠다. **두 단계 중 두 번째** 로,
	 * 세트를 켜는 것(위)과 컨트롤러 인터럽트를 켜는 것(여기)이 모두 되어야
	 * MSI 가 전달된다 */
	val |= PCIE_MSI_ENABLE;
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);
}

/* [한국어]
 * mtk_pcie_devices_power_up - PERST 를 풀고 슬롯 장치에 전원을 준다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류. 슬롯 전원 인가에 실패하면 그 값을 그대로 올린다.
 *
 * 컨트롤러 자신이 아니라 **슬롯에 꽂힌 장치** 를 깨우는 단계다. 컨트롤러
 * 전원은 이미 pcie->soc->power_up() 이 올린 뒤다.
 *
 * 순서가 스펙으로 정해져 있다.
 *   1) 리셋 신호를 모두 어서트한다 — 장치를 리셋 상태로 붙들어 둔 채
 *      전원을 넣기 위해서다.
 *   2) 전원을 넣는다(pci_pwrctrl_power_on_devices,
 *      drivers/pci/pwrctrl/core.c:313).
 *   3) 100ms 기다린다. 위의 원문 주석대로 PCIe CEM 스펙의 T_PVPERL 이며,
 *      전원과 클록이 안정되기까지의 시간이다. 상수는
 *      drivers/pci/pci.h:107 에 있다.
 *   4) 리셋을 푼다. 이제 장치가 링크 훈련을 시작한다.
 *
 * EN7581 예외: 이 SoC 는 PCIE_PE_RSTB 를 건드리면 링크가 간헐적으로 끊기는
 * 하드웨어 문제가 있다고 위의 원문 주석이 밝힌다. 그래서 SKIP_PCIE_RSTB
 * 플래그가 있으면 1)과 4)를 건너뛰고, 리셋은 클록 콜백 쪽에서 대신 한다.
 * 대기(3)는 건너뛰지 않는다 — 전원 안정 시간은 리셋 방식과 무관하기 때문이다.
 *
 * 주의할 점: val 은 1)에서만 읽는다. 4)에서 다시 읽지 않고 1)의 값을 그대로
 * 고쳐 쓴다. SKIP_PCIE_RSTB 가 없을 때만 두 블록이 함께 실행되므로 val 이
 * 초기화되지 않은 채 쓰이는 일은 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 100ms 잠든다.
 *
 * 호출 체인:
 *   mtk_pcie_startup_port → [이 함수]
 *     → pci_pwrctrl_power_on_devices, msleep
 */
static int mtk_pcie_devices_power_up(struct mtk_gen3_pcie *pcie)
{
	int err;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/*
	 * Airoha EN7581 has a hw bug asserting/releasing PCIE_PE_RSTB signal
	 * causing occasional PCIe link down. In order to overcome the issue,
	 * PCIE_RSTB signals are not asserted/released at this stage and the
	 * PCIe block is reset using en7523_reset_assert() and
	 * en7581_pci_enable().
	 */
	if (!(pcie->soc->flags & SKIP_PCIE_RSTB)) {
		/* Assert all reset signals */
		val = readl_relaxed(pcie->base + PCIE_RST_CTRL_REG);
		/* [한국어] MAC, PHY, 브리지 리셋 비트를 세운다. **1이 어서트(리셋 유지)** 다 */
		val |= PCIE_MAC_RSTB | PCIE_PHY_RSTB | PCIE_BRG_RSTB |
		       /* [한국어] PERST 는 슬롯 장치로 나가는 리셋이다 */
		       PCIE_PE_RSTB;
		/* [한국어] 네 리셋을 한 번에 어서트한다 */
		writel_relaxed(val, pcie->base + PCIE_RST_CTRL_REG);
	}

	/* [한국어] 슬롯 장치에 전원을 넣는다(drivers/pci/pwrctrl/core.c:313).
	 * 리셋을 어서트한 상태에서 전원을 넣는 것이 규격상의 순서다 */
	err = pci_pwrctrl_power_on_devices(pcie->dev);
	/* [한국어] 전원을 못 넣으면 링크를 세울 수 없다 */
	if (err) {
		/* [한국어] 슬롯 전원 인가 실패. %pe 로 오류 포인터를 사람이 읽는 이름으로 찍는다 */
		dev_err(pcie->dev, "Failed to power on devices: %pe\n", ERR_PTR(err));
		return err;
	}

	/*
	 * Described in PCIe CEM specification revision 6.0.
	 *
	 * The deassertion of PERST# should be delayed 100ms (TPVPERL)
	 * for the power and clock to become stable.
	 */
	msleep(PCIE_T_PVPERL_MS);

	if (!(pcie->soc->flags & SKIP_PCIE_RSTB)) {
		/* De-assert reset signals */
		val &= ~(PCIE_MAC_RSTB | PCIE_PHY_RSTB | PCIE_BRG_RSTB |
			 PCIE_PE_RSTB);
		/* [한국어] **리셋을 푼다.** val 을 다시 읽지 않고 위에서 읽은 값을 그대로 고쳐
		 * 쓴다. SKIP_PCIE_RSTB 가 없을 때만 두 블록이 함께 실행되므로 val 이
		 * 초기화되지 않은 채 쓰이는 일은 없다 */
		writel_relaxed(val, pcie->base + PCIE_RST_CTRL_REG);
	}

	return 0;
}

/* [한국어]
 * mtk_pcie_devices_power_down - 슬롯 장치를 리셋에 넣고 전원을 끊는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음. 되돌리는 경로라 실패해도 할 수 있는 일이 없다.
 *
 * mtk_pcie_devices_power_up 의 역순이되, 대칭이 완전하지는 않다. 올릴 때는
 * 네 리셋 신호를 모두 어서트했지만 내릴 때는 PERST(PCIE_PE_RSTB) 하나만
 * 어서트한다. 장치를 리셋 상태로 보내는 데는 그것으로 충분하고, 컨트롤러
 * 쪽 리셋(MAC/PHY/BRG)은 mtk_pcie_power_down 이 따로 처리하기 때문이다.
 *
 * EN7581 은 여기서도 PERST 를 건드리지 않는다(SKIP_PCIE_RSTB). 전원만 끊는다.
 *
 * 불리는 곳이 셋이다 — 링크가 안 서서 되돌릴 때(mtk_pcie_startup_port 의
 * 오류 경로), probe 가 실패해 되감을 때, 그리고 절전에 들어갈 때
 * (mtk_pcie_suspend_noirq).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   mtk_pcie_startup_port(오류) / mtk_pcie_probe(오류) / mtk_pcie_suspend_noirq
 *     → [이 함수] → pci_pwrctrl_power_off_devices
 */
static void mtk_pcie_devices_power_down(struct mtk_gen3_pcie *pcie)
{
	u32 val;

	if (!(pcie->soc->flags & SKIP_PCIE_RSTB)) {
		/* Assert the PERST# pin */
		val = readl_relaxed(pcie->base + PCIE_RST_CTRL_REG);
		/* [한국어] PERST 만 어서트한다. 올릴 때 네 신호를 모두 어서트했던 것과 대칭이
		 * 아닌데, 컨트롤러 쪽 리셋은 mtk_pcie_power_down 이 따로 처리하기 때문이다 */
		val |= PCIE_PE_RSTB;
		/* [한국어] 되쓴다 */
		writel_relaxed(val, pcie->base + PCIE_RST_CTRL_REG);
	}

	pci_pwrctrl_power_off_devices(pcie->dev);
}

/* [한국어]
 * mtk_pcie_startup_port - 컨트롤러를 RC 로 세우고 링크를 올린다
 *
 * @pcie: 컨트롤러 상태. base 가 매핑되어 있고 전원이 올라와 있어야 한다.
 * @return: 0 링크 성공. 변환 표 오류나 링크 실패 시 음수.
 *
 * 전원이 올라온 뒤 실제로 PCIe 링크를 세우는 함수다. probe 와 resume 이
 * 모두 이 함수를 부르므로, 여기 있는 설정은 절전에서 깨어날 때마다 다시
 * 적용된다 — 레지스터가 전원과 함께 날아가기 때문이다.
 *
 * 순서:
 *   1) RC 모드로 세우고, 속도와 레인 수 상한을 같은 레지스터에 함께 반영한다.
 *      속도: Gen2 부터만 제한할 수 있다. GENMASK(max-2, 0) 이 "Gen2 부터
 *        max 까지 지원" 을 뜻하는 비트열이 된다. Gen1 만 쓰라는 표현은
 *        하드웨어에 없다.
 *      레인: 0 이 x1 을 뜻하고 비트마다 x2/x4/x8/x16 이 켜진다.
 *   2) Link Control 2 의 속도 필드에도 같은 상한을 쓴다. 1)이 컨트롤러의
 *      능력 광고라면 이쪽은 링크 협상이 목표로 삼을 속도다. 주소가
 *      PCIE_CFG_OFFSET_ADDR + 0xb0 인 데서 보듯, 이 컨트롤러의 자기 config
 *      공간을 config 창을 통해 직접 건드리는 것이다.
 *   3) sys_clk_rdy_time_us 가 pdata 에 있으면 그 값을 쓴다. MT8196 만
 *      10 을 지정하며, 원문 주석은 글리치를 피하기 위해서라고 한다.
 *   4) 클래스 코드를 PCI-to-PCI 브리지로 바꾼다. 하드웨어 기본값이 브리지가
 *      아니면 PCI 코어가 이 컨트롤러를 브리지로 인식하지 못한다.
 *   5) INTx 를 모두 마스크한다. 도메인은 이미 만들어져 있으나, 아직 아무도
 *      핸들러를 걸지 않은 인터럽트가 올라오면 곤란하기 때문이다. 개별
 *      장치가 INTx 를 쓰기 시작하면 mtk_intx_unmask 가 하나씩 연다.
 *   6) DVFSRC 전압 요청을 끈다.
 *   7) MSI 수신 주소를 깔고 켠다.
 *   8) 브리지의 모든 창을 변환 표에 넣는다. IO 는 pci_pio_to_address 로
 *      실제 주소를 얻고, MEM 은 res->start 를 그대로 쓴다. PCI 쪽 주소는
 *      entry->offset 을 빼서 구한다.
 *   9) 슬롯 전원을 올리고 PERST 를 푼다.
 *  10) 링크가 설 때까지 20us 간격으로 100ms 까지 폴링한다.
 *
 * 링크가 안 서면 LTSSM 상태를 읽어 ltssm_str[] 로 이름을 찍는다. 링크
 * 실패는 원인이 여럿이라(케이블, 전원, 속도 협상) 상태 이름 하나가 진단에
 * 결정적이다. 표 범위를 넘는 값이면 "Unknown state" 로 방어한다.
 * 그 뒤 슬롯 전원을 되돌리고 오류를 올린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 9)의 msleep 으로 잠들고 10)에서 폴링한다.
 *
 * 호출 체인:
 *   mtk_pcie_setup / mtk_pcie_resume_noirq → [이 함수]
 *     → mtk_pcie_enable_msi, mtk_pcie_set_trans_table,
 *       mtk_pcie_devices_power_up, mtk_pcie_devices_power_down(오류)
 */
static int mtk_pcie_startup_port(struct mtk_gen3_pcie *pcie)
{
	struct resource_entry *entry;
	/* [한국어] private 영역에서 브리지를 되찾는다. 창 목록이 브리지에 있기 때문이다 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	/* [한국어] **0 에서 시작해 창마다 이어진다.** mtk_pcie_set_trans_table 이 참조로
	 * 받아 자기가 쓴 개수만큼 늘려 주므로, 여러 창이 표를 나눠 써도 번호가
	 * 겹치지 않는다 */
	unsigned int table_index = 0;
	/* [한국어] 오류 코드 */
	int err;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* Set as RC mode and set controller PCIe Gen speed restriction, if any */
	val = readl_relaxed(pcie->base + PCIE_SETTING_REG);
	/* [한국어] Root Complex 모드 비트를 세운다. 이 파일은 RC 만 다룬다 */
	val |= PCIE_RC_MODE;
	/* [한국어] 속도 상한이 정해졌을 때만 건드린다 */
	if (pcie->max_link_speed) {
		/* [한국어] 기존 속도 필드를 지운다 */
		val &= ~PCIE_SETTING_GEN_SUPPORT;

		/* Can enable link speed support only from Gen2 onwards */
		if (pcie->max_link_speed >= 2)
			/* [한국어] 지원 세대를 넣는다. GENMASK(max-2, 0) 이 "Gen2 부터 max 까지" 를 뜻하는
			 * 비트열이 된다 — 필드가 Gen2 부터 표현되기 때문이다 */
			val |= FIELD_PREP(PCIE_SETTING_GEN_SUPPORT,
					  GENMASK(pcie->max_link_speed - 2, 0));
	}
	/* [한국어] 장치 트리가 레인 수를 지정했을 때만 건드린다. 0 이면 컨트롤러
	 * 기본값을 그대로 둔다 */
	if (pcie->num_lanes) {
		/* [한국어] 기존 폭 필드를 지운다 */
		val &= ~PCIE_SETTING_LINK_WIDTH;

		/* Zero means one lane, each bit activates x2/x4/x8/x16 */
		if (pcie->num_lanes > 1)
			/* [한국어] 폭을 넣는다. GENMASK(fls(num_lanes >> 2), 0) 은 x2 부터 해당 폭까지의
			 * 비트를 세우는 식이다 — 예컨대 x4 면 fls(1)=1 이라 비트 1~0 이 선다 */
			val |= FIELD_PREP(PCIE_SETTING_LINK_WIDTH,
					  GENMASK(fls(pcie->num_lanes >> 2), 0));
	}
	/* [한국어] RC 모드와 속도·폭 제한을 한 번에 쓴다 */
	writel_relaxed(val, pcie->base + PCIE_SETTING_REG);

	/* Set Link Control 2 (LNKCTL2) speed restriction, if any */
	if (pcie->max_link_speed) {
		/* [한국어] **컨트롤러 자신의 config 공간을 config 창을 통해 읽는다.**
		 * 주소가 PCIE_CFG_OFFSET_ADDR + 0xb0 인 데서 그것이 드러난다 */
		val = readl_relaxed(pcie->base + PCIE_CONF_LINK2_CTL_STS);
		/* [한국어] 기존 속도 필드를 지운다 */
		val &= ~PCIE_CONF_LINK2_LCR2_LINK_SPEED;
		/* [한국어] 목표 속도를 넣는다 */
		val |= FIELD_PREP(PCIE_CONF_LINK2_LCR2_LINK_SPEED, pcie->max_link_speed);
		/* [한국어] 되쓴다 */
		writel_relaxed(val, pcie->base + PCIE_CONF_LINK2_CTL_STS);
	}

	/* If parameter is present, adjust SYS_CLK_RDY_TIME to avoid glitching */
	if (pcie->soc->sys_clk_rdy_time_us) {
		/* [한국어] 현재 값을 읽는다 */
		val = readl_relaxed(pcie->base + PCIE_RESOURCE_CTRL_REG);
		/* [한국어] 필드만 골라 바꾼다. **FIELD_MODIFY 매크로를 정의한 헤더는 이 스파스
		 * 체크아웃에 없다.** 이름과 인자 형태(마스크, 포인터, 값)로 보아
		 * 읽고-고쳐-쓰기를 한 줄로 줄여 주는 도우미다 */
		FIELD_MODIFY(PCIE_RSRC_SYS_CLK_RDY_TIME_MASK, &val,
			     pcie->soc->sys_clk_rdy_time_us);
		/* [한국어] 되쓴다 */
		writel_relaxed(val, pcie->base + PCIE_RESOURCE_CTRL_REG);
	}

	/* Set class code */
	val = readl_relaxed(pcie->base + PCIE_PCI_IDS_1);
	/* [한국어] 상위 24비트(클래스 코드)를 지운다. 하위 8비트는 리비전이라 보존한다 */
	val &= ~GENMASK(31, 8);
	/* [한국어] PCI-to-PCI 브리지 클래스를 넣는다. **PCI_CLASS_BRIDGE_PCI_NORMAL 을
	 * 정의한 헤더는 이 스파스 체크아웃에 없어 값을 확인하지 못했다.**
	 * 이름과 쓰임으로 보아 브리지 클래스/서브클래스/프로그래밍 인터페이스를
	 * 합친 값이다 */
	val |= PCI_CLASS(PCI_CLASS_BRIDGE_PCI_NORMAL);
	/* [한국어] 되쓴다 */
	writel_relaxed(val, pcie->base + PCIE_PCI_IDS_1);

	/* Mask all INTx interrupts */
	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	/* [한국어] **INTx 를 모두 마스크한다.** 도메인은 이미 만들어져 있지만 아직 아무도
	 * 핸들러를 걸지 않아, 열어 두면 처리할 수 없는 인터럽트가 올라온다.
	 * 개별 장치가 INTx 를 쓰기 시작하면 mtk_intx_unmask 가 하나씩 연다 */
	val &= ~PCIE_INTX_ENABLE;
	/* [한국어] 되쓴다 */
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);

	/* Disable DVFSRC voltage request */
	val = readl_relaxed(pcie->base + PCIE_MISC_CTRL_REG);
	/* [한국어] DVFSRC 전압 요청을 끈다. SoC 별 분기 없이 항상 켠다 */
	val |= PCIE_DISABLE_DVFSRC_VLT_REQ;
	/* [한국어] 되쓴다 */
	writel_relaxed(val, pcie->base + PCIE_MISC_CTRL_REG);

	mtk_pcie_enable_msi(pcie);

	/* Set PCIe translation windows */
	resource_list_for_each_entry(entry, &host->windows) {
		/* [한국어] 이 창의 리소스 */
		struct resource *res = entry->res;
		/* [한국어] IORESOURCE_IO 인지 IORESOURCE_MEM 인지 가른다. 그 밖의 종류(버스 번호
		 * 등)는 변환 표에 넣을 것이 아니라 건너뛴다 */
		unsigned long type = resource_type(res);
		/* [한국어] CPU 쪽 시작 주소 */
		resource_size_t cpu_addr;
		/* [한국어] PCI 쪽 시작 주소 */
		resource_size_t pci_addr;
		/* [한국어] 창의 크기 */
		resource_size_t size;

		/* [한국어] IO 창인지 확인한다 */
		if (type == IORESOURCE_IO)
			/* [한국어] IO 창은 논리적인 포트 번호로 표현되어 있어 실제 주소로 바꿔야 한다 */
			cpu_addr = pci_pio_to_address(res->start);
		/* [한국어] 메모리 창인지 확인한다 */
		else if (type == IORESOURCE_MEM)
			/* [한국어] 메모리는 CPU 주소를 그대로 쓴다 */
			cpu_addr = res->start;
		else
			continue;

		/* [한국어] PCI 쪽 주소를 구한다. entry->offset 이 CPU 주소와 PCI 주소의 차이다 */
		pci_addr = res->start - entry->offset;
		/* [한국어] 창의 크기 */
		size = resource_size(res);
		/* [한국어] 이 창을 변환 표에 넣는다. table_index 가 참조로 전달되어 창마다
		 * 이어지는 번호를 받는다 */
		err = mtk_pcie_set_trans_table(pcie, cpu_addr, pci_addr, size,
					       type, &table_index);
		/* [한국어] 표가 모자라는 것은 경고로 끝나지만, 표 크기가 4KiB 미만이면 오류다 */
		if (err)
			return err;
	}

	/* [한국어] 슬롯 전원을 올리고 PERST 를 푼다. **이 시점부터 상대 장치가 링크 훈련을
	 * 시작한다** — 그래서 바로 아래에서 결과를 기다린다 */
	err = mtk_pcie_devices_power_up(pcie);
	/* [한국어] 슬롯 전원 인가 실패면 그대로 올린다 */
	if (err)
		return err;

	/* Check if the link is up or not */
	err = readl_poll_timeout(pcie->base + PCIE_LINK_STATUS_REG, val,
				 !!(val & PCIE_PORT_LINKUP), 20,
				 PCI_PM_D3COLD_WAIT * USEC_PER_MSEC);
	/* [한국어] 시간 안에 링크가 서지 않았다 */
	if (err) {
		/* [한국어] 상태 이름 */
		const char *ltssm_state;
		/* [한국어] 상태 번호 */
		int ltssm_index;

		/* [한국어] LTSSM 상태를 읽는다. **링크가 안 설 때 원인을 좁히는 유일한 단서** 다 —
		 * 예컨대 polling 에서 멈췄으면 물리 계층, config 에서 멈췄으면 폭 협상
		 * 문제를 의심하게 된다 */
		val = readl_relaxed(pcie->base + PCIE_LTSSM_STATUS_REG);
		/* [한국어] 상태 번호를 꺼낸다 */
		ltssm_index = PCIE_LTSSM_STATE(val);
		/* [한국어] 표 범위를 넘는 값이면 "Unknown state" 로 방어한다. LTSSM 필드가
		 * 5비트라 0x1B~0x1F 가 나올 수 있는데 표에는 0x1A 까지만 있다 */
		ltssm_state = ltssm_index >= ARRAY_SIZE(ltssm_str) ?
			      "Unknown state" : ltssm_str[ltssm_index];
		dev_err(pcie->dev,
			"PCIe link down, current LTSSM state: %s (%#x)\n",
			ltssm_state, val);
		goto err_power_down_device;
	}

	return 0;

err_power_down_device:
	mtk_pcie_devices_power_down(pcie);
	return err;
}

#define MTK_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				MSI_FLAG_USE_DEF_CHIP_OPS	| \
				MSI_FLAG_NO_AFFINITY		| \
				MSI_FLAG_PCI_MSI_MASK_PARENT)

#define MTK_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		| \
				 MSI_FLAG_PCI_MSIX		| \
				 MSI_FLAG_MULTI_PCI_MSI)

/* [한국어] MSI 부모 도메인의 동작 정의. init_dev_msi_info 가
 * msi_lib_init_dev_msi_info 로 되어 있어, 자식 도메인 정보 채우기를
 * 커널 공통 구현에 맡긴다 */
static const struct msi_parent_ops mtk_msi_parent_ops = {
	/* [한국어] required_flags 는 자식 도메인이 반드시 갖춰야 할 성질이고,
	 * supported_flags 는 허용되는 성질의 상한이다. 아래 두 매크로에
	 * 그 내용이 정의되어 있으며, MSI_FLAG_PCI_MSIX 와
	 * MSI_FLAG_MULTI_PCI_MSI 가 있어 MSI-X 와 다중 MSI 를 모두 지원한다 */
	.required_flags		= MTK_MSI_FLAGS_REQUIRED,
	.supported_flags	= MTK_MSI_FLAGS_SUPPORTED,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.prefix			= "MTK3-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어]
 * mtk_compose_msi_msg - 장치에게 알려 줄 MSI 주소와 데이터를 만든다
 *
 * @data: 이 벡터의 irq_data. chip_data 에 **struct mtk_msi_set** 이 들어 있고
 *   (mtk_msi_bottom_domain_alloc 이 넣는다), domain->host_data 에
 *   struct mtk_gen3_pcie 가 들어 있다.
 * @msg:  채워 줄 메시지. 커널이 이 값을 장치의 MSI/MSI-X 능력 레지스터에
 *   써서 "여기에 이 값을 써라" 고 알린다.
 * @return: 없음.
 *
 * MSI 는 인터럽트 선이 아니라 메모리 쓰기다. 장치가 msg->address 에
 * msg->data 를 쓰면 컨트롤러가 그것을 인터럽트로 바꾼다. 그 두 값을 정하는
 * 곳이 여기다.
 *
 * 이 드라이버에서 눈여겨볼 점은 **주소가 세트마다 다르다** 는 것이다.
 * msi_set->msg_addr 은 mtk_pcie_enable_msi 가 세트별로 다르게 계산해 둔
 * 물리 주소이고, 데이터에는 세트 안의 자리(hwirq % 32)만 담는다. 그래서
 * 장치의 쓰기 주소가 세트를 고르고, 쓴 값이 그 세트 안의 벡터를 고른다.
 * 전역 hwirq(0~255)를 그대로 데이터에 넣지 않는 이유가 이것이다.
 *
 * chip_data 가 컨트롤러가 아니라 세트라는 점이 이 구조를 떠받친다 — 세트를
 * 찾기 위해 hwirq 를 32로 나눌 필요가 없고, 세트 구조체가 자기 주소를
 * 이미 알고 있다.
 *
 * 실행 컨텍스트: 인터럽트 할당 경로(프로세스 컨텍스트)에서 커널 MSI 코어가
 * 부른다. 인터럽트 핸들러 안이 아니다.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors 계열 → MSI 코어 →
 *     mtk_msi_bottom_irq_chip.irq_compose_msi_msg → [이 함수]
 */
static void mtk_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	/* [한국어] 로그에 쓸 device 를 얻기 위해 컨트롤러 상태가 필요하다 */
	struct mtk_gen3_pcie *pcie = data->domain->host_data;
	/* [한국어] 세트 안의 자리를 담을 변수 */
	unsigned long hwirq;

	/* [한국어] 세트 안의 자리를 구한다 */
	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	/* [한국어] 수신 주소 상위 32비트. 32비트 시스템에서는 0 이 된다 */
	msg->address_hi = upper_32_bits(msi_set->msg_addr);
	/* [한국어] 수신 주소 하위 32비트 */
	msg->address_lo = lower_32_bits(msi_set->msg_addr);
	/* [한국어] **데이터에는 세트 안의 자리만 담는다.** 전역 hwirq(0~255)가 아니다 —
	 * 어느 세트인지는 이미 주소가 말해 주기 때문이다 */
	msg->data = hwirq;
	/* [한국어] 만든 주소와 데이터를 디버그 로그로 남긴다. MSI 가 안 올 때 주소가
	 * 맞는지 확인하는 첫 단서가 된다 */
	dev_dbg(pcie->dev, "msi#%#lx address_hi %#x address_lo %#x data %d\n",
		hwirq, msg->address_hi, msg->address_lo, msg->data);
}

/* [한국어]
 * mtk_msi_bottom_irq_ack - 처리한 MSI 벡터의 대기 비트를 지운다
 *
 * @data: 이 벡터의 irq_data. chip_data 가 struct mtk_msi_set 이다.
 * @return: 없음.
 *
 * MSI 하나가 도착하면 세트의 상태 레지스터에 그 벡터의 비트가 선다.
 * 핸들러를 부르기 전에 그 비트를 지워야, 처리 중에 같은 벡터가 또 오면
 * 새 인터럽트로 잡힌다. 지우지 않으면 mtk_pcie_msi_handler 의 do-while 이
 * 같은 비트를 계속 보고 무한히 돈다.
 *
 * 상태 레지스터는 **1을 쓰면 지워지는(RW1C)** 방식이다 — 코드가 읽지 않고
 * BIT(hwirq) 만 쓰는 데서 알 수 있다. 읽고-고쳐-쓰기가 아니므로 다른 벡터의
 * 비트를 실수로 지울 일이 없고, 그래서 이 함수만 락이 없다
 * (mask/unmask 는 락을 쥔다 — 그쪽은 읽고-고쳐-쓰기라서다).
 *
 * hwirq 를 32로 나눈 나머지를 쓰는 것은 세트 안의 자리를 구하기 위해서다.
 * 세트 자체는 chip_data 로 이미 정해져 있다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. handle_edge_irq 가 핸들러를 부르기
 * 직전에 불러 준다(mtk_msi_bottom_domain_alloc 이 그 흐름 함수를 지정한다).
 *
 * 호출 체인:
 *   mtk_pcie_msi_handler → generic_handle_domain_irq → handle_edge_irq
 *     → [이 함수]
 */
static void mtk_msi_bottom_irq_ack(struct irq_data *data)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	/* [한국어] 비트 위치 보관용 */
	unsigned long hwirq;

	/* [한국어] 세트 안의 자리를 구한다. 상태 레지스터가 세트마다 따로 있어
	 * 전역 번호가 아니라 세트 안 번호가 필요하다 */
	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	writel_relaxed(BIT(hwirq), msi_set->base + PCIE_MSI_SET_STATUS_OFFSET);
}

/* [한국어]
 * mtk_msi_bottom_irq_mask - MSI 벡터 하나를 세트 안에서 끈다
 *
 * @data: 이 벡터의 irq_data. chip_data 가 세트, domain->host_data 가 컨트롤러다.
 * @return: 없음.
 *
 * 세트의 활성화 레지스터에서 해당 비트를 내린다. 꺼진 벡터의 대기 비트는
 * mtk_pcie_msi_handler 가 msi_status 를 msi_enable 로 걸러 내므로 무시된다.
 *
**읽고-고쳐-쓰기라 락이 필요하다.** 같은 세트의 벡터 32개가 이 레지스터
 * 하나를 공유하므로, 두 CPU 가 각기 다른 벡터를 동시에 마스크하면 한쪽의
 * 변경이 사라질 수 있다. irq_lock 은 raw_spinlock 이고 irqsave 로 잡는다 —
 * 인터럽트 컨텍스트에서도 불릴 수 있고, PREEMPT_RT 에서도 잠들면 안 되는
 * 자리이기 때문이다. 같은 락을 mtk_intx_mask 계열도 쓴다. 두 곳이 서로 다른
 * 레지스터를 만지지만 하나의 락으로 묶어 단순하게 유지한 것이다.
 *
 * MSI 비트맵을 지키는 mutex(pcie->lock)와는 다른 락이다. 그쪽은 할당
 * 경로에서만 쓰여 잠들어도 되기 때문에 종류가 다르다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수도, 프로세스 컨텍스트일 수도 있다.
 * irq_chip 콜백은 양쪽에서 불린다.
 *
 * 호출 체인:
 *   disable_irq 계열 / MSI 코어의 마스크 흐름 → [이 함수]
 */
static void mtk_msi_bottom_irq_mask(struct irq_data *data)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	/* [한국어] 도메인의 host_data 로 컨트롤러 상태를 얻는다. 락이 컨트롤러에 있기
	 * 때문이며, 세트에는 락이 없다 */
	struct mtk_gen3_pcie *pcie = data->domain->host_data;
	/* [한국어] 비트 위치와 락 플래그 */
	unsigned long hwirq, flags;
	/* [한국어] 레지스터 값 */
	u32 val;

	/* [한국어] 세트 안의 자리(0~31)를 구한다 */
	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	/* [한국어] **읽고-고쳐-쓰기라 락이 필요하다.** 같은 세트의 벡터 32개가 이
	 * 활성화 레지스터를 공유한다. 1을 써서 지우는 상태 레지스터를 다루는
	 * mtk_msi_bottom_irq_ack 에 락이 없는 것과 대비된다 */
	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	/* [한국어] 현재 값을 읽는다 */
	val = readl_relaxed(msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	/* [한국어] 해당 벡터 비트를 내린다 */
	val &= ~BIT(hwirq);
	/* [한국어] 되쓴다 */
	writel_relaxed(val, msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

/* [한국어]
 * mtk_msi_bottom_irq_unmask - MSI 벡터 하나를 세트 안에서 켠다
 *
 * @data: 이 벡터의 irq_data.
 * @return: 없음.
 *
 * mtk_msi_bottom_irq_mask 의 반대로, 활성화 비트를 세운다. 락을 쓰는 이유와
 * 방식은 그쪽과 같다 — 세트 안 32개 벡터가 레지스터 하나를 공유한다.
 *
 * 이 함수가 불려야 비로소 그 벡터의 인터럽트가 실제로 전달된다. 벡터를
 * 할당하는 것(mtk_msi_bottom_domain_alloc)과 켜는 것은 별개이며, 드라이버가
 * request_irq 를 마친 뒤에 커널이 켜 준다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수도, 프로세스 컨텍스트일 수도 있다.
 *
 * 호출 체인:
 *   request_irq / enable_irq → MSI 코어 → [이 함수]
 */
static void mtk_msi_bottom_irq_unmask(struct irq_data *data)
{
	struct mtk_msi_set *msi_set = irq_data_get_irq_chip_data(data);
	/* [한국어] 로그용은 아니고, 락을 얻기 위해 컨트롤러 상태가 필요하다 */
	struct mtk_gen3_pcie *pcie = data->domain->host_data;
	/* [한국어] 비트 위치와 락 플래그 */
	unsigned long hwirq, flags;
	/* [한국어] 레지스터 값 */
	u32 val;

	/* [한국어] 세트 안의 자리를 구한다. 세트 자체는 chip_data 로 이미 정해져 있다 */
	hwirq =	data->hwirq % PCIE_MSI_IRQS_PER_SET;

	/* [한국어] 같은 세트의 벡터 32개가 이 레지스터 하나를 공유하므로 락이 필요하다 */
	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	/* [한국어] 현재 값을 읽는다 */
	val = readl_relaxed(msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	/* [한국어] 해당 벡터 비트를 세운다 */
	val |= BIT(hwirq);
	/* [한국어] 되쓴다 */
	writel_relaxed(val, msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	/* [한국어] 락을 푼다 */
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

/* [한국어] MSI 벡터 하나에 붙는 irq_chip. chip_data 로 struct mtk_msi_set 을 받는다 */
static struct irq_chip mtk_msi_bottom_irq_chip = {
	/* [한국어] ack 가 있는 것이 INTx 쪽과 다르다. MSI 는 에지라 핸들러 **전에**
	 * 상태를 지워야 하고, INTx 는 레벨을 흉내 내 핸들러 **뒤에** 지운다 */
	.irq_ack		= mtk_msi_bottom_irq_ack,
	.irq_mask		= mtk_msi_bottom_irq_mask,
	.irq_unmask		= mtk_msi_bottom_irq_unmask,
	.irq_compose_msi_msg	= mtk_compose_msi_msg,
	.name			= "MSI",
};

/* [한국어]
 * mtk_msi_bottom_domain_alloc - MSI 벡터를 연속으로 떼어 준다
 *
 * @domain:  MSI 바닥 도메인. host_data 가 컨트롤러다.
 * @virq:    커널이 이미 잡아 둔 가상 IRQ 번호의 시작값.
 * @nr_irqs: 요청 개수. MSI-X 는 보통 1, 다중 MSI 는 2의 거듭제곱이다.
 * @arg:     상위 도메인이 넘기는 정보. 이 드라이버는 쓰지 않는다.
 * @return: 0 성공, 빈 벡터가 없으면 -ENOSPC.
 *
 * 256개 벡터를 담은 비트맵에서 빈 자리를 찾아 표시하고, 각 가상 IRQ 에
 * irq_chip 과 chip_data 를 연결한다.
 *
 * bitmap_find_free_region 을 쓰는 이유는 **다중 MSI** 때문이다. 다중 MSI 는
 * 벡터가 연속이어야 하고 시작 번호가 개수에 정렬되어야 한다(장치가 기준
 * 주소에 번호를 더해 쓰기 때문). 그 두 제약을 한 번에 만족시키는 것이 이
 * 함수이며, order_base_2(nr_irqs) 로 2의 거듭제곱 차수를 넘긴다.
 *
 * chip_data 로 컨트롤러가 아니라 **세트** 를 넣는 것이 이 드라이버의
 * 설계다. hwirq 를 32로 나눠 세트를 고르고 그 포인터를 심어 두면, 이후
 * ack/mask/unmask/compose 가 세트를 다시 찾을 필요가 없다.
 *
 * nr_irqs 개가 한 세트 안에 다 들어간다고 전제한다 —
 * bitmap_find_free_region 이 정렬된 연속 영역을 주므로, 32 이하의 2의
 * 거듭제곱 요청은 세트 경계를 넘지 않는다.
 *
 * handle_edge_irq 를 흐름 함수로 지정한다. MSI 는 본질적으로 에지이며,
 * 이 흐름 함수가 핸들러 전에 irq_ack 를 불러 준다.
 *
 * 락: 비트맵은 mutex 로 지킨다. 이 경로는 프로세스 컨텍스트에서만 불리고
 * 잠들어도 되기 때문이다. 레지스터를 지키는 irq_lock 과는 별개다.
 * 비트맵 조작 뒤 곧바로 락을 푸는 것에 주의 — 이후의 irq_domain_set_info 는
 * 비트맵을 건드리지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 드라이버의 벡터 할당 경로).
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors 계열 → MSI 코어 → irq_domain_alloc_irqs
 *     → [이 함수] → bitmap_find_free_region, irq_domain_set_info
 */
static int mtk_msi_bottom_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *arg)
{
	struct mtk_gen3_pcie *pcie = domain->host_data;
	/* [한국어] 이 벡터들이 속할 세트 */
	struct mtk_msi_set *msi_set;
	/* [한국어] 반복자, 전역 hwirq, 세트 번호 */
	int i, hwirq, set_idx;

	mutex_lock(&pcie->lock);

	/* [한국어] **연속되고 정렬된** 빈 영역을 찾아 표시한다. 다중 MSI 는 벡터가
	 * 연속이어야 하고 시작 번호가 개수에 정렬되어야 하는데(장치가 기준
	 * 주소에 번호를 더해 쓰므로), 이 함수가 두 제약을 한 번에 만족시킨다.
	 * 단순한 find_first_zero_bit 로는 부족한 이유다 */
	hwirq = bitmap_find_free_region(pcie->msi_irq_in_use, PCIE_MSI_IRQS_NUM,
					order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);

	/* [한국어] 빈 자리가 없으면 -ENOSPC. 벡터 256개를 다 쓴 경우다 */
	if (hwirq < 0)
		return -ENOSPC;

	/* [한국어] 전역 hwirq 를 32로 나눠 세트를 고른다. nr_irqs 개가 한 세트 안에 다
	 * 들어간다고 전제하는데, bitmap_find_free_region 이 정렬된 연속 영역을
	 * 주므로 32 이하의 2의 거듭제곱 요청은 세트 경계를 넘지 않는다 */
	set_idx = hwirq / PCIE_MSI_IRQS_PER_SET;
	/* [한국어] 그 세트의 포인터를 얻는다 */
	msi_set = &pcie->msi_sets[set_idx];

	/* [한국어] 요청 개수만큼 순회한다 */
	for (i = 0; i < nr_irqs; i++)
		/* [한국어] 각 가상 IRQ 에 irq_chip 과 chip_data 를 심는다.
		 * **chip_data 가 컨트롤러가 아니라 세트** 라는 점이 이 드라이버의 설계다.
		 * handle_edge_irq 를 흐름 함수로 주는 것은 MSI 가 본질적으로 에지이기
		 * 때문이며, 그 판이 핸들러 전에 irq_ack 를 불러 준다 */
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &mtk_msi_bottom_irq_chip, msi_set,
				    handle_edge_irq, NULL, NULL);

	return 0;
}

/* [한국어]
 * mtk_msi_bottom_domain_free - MSI 벡터를 비트맵에 돌려준다
 *
 * @domain:  MSI 바닥 도메인.
 * @virq:    반납할 가상 IRQ 시작 번호.
 * @nr_irqs: 반납 개수.
 * @return: 없음.
 *
 * mtk_msi_bottom_domain_alloc 의 반대다. virq 로 irq_data 를 찾아 그 안의
 * hwirq 를 얻고, 그 자리를 비트맵에서 푼다.
 *
 * hwirq 를 인자로 받지 않고 irq_data 에서 꺼내는 이유는, 커널의 free 콜백
 * 규약이 가상 IRQ 번호만 넘기기 때문이다. 매핑을 지우기 전에 꺼내야 하므로
 * irq_domain_free_irqs_common 보다 먼저 읽는다.
 *
 * bitmap_release_region 은 alloc 때와 같은 차수를 받아야 짝이 맞는다.
 *
 * 락: alloc 과 같은 mutex 다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_free_irq_vectors 계열 → MSI 코어 → [이 함수]
 *     → bitmap_release_region, irq_domain_free_irqs_common
 */
static void mtk_msi_bottom_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	struct mtk_gen3_pcie *pcie = domain->host_data;
	/* [한국어] **매핑을 지우기 전에** irq_data 를 얻는다. free 콜백 규약이 가상 IRQ
	 * 번호만 넘기므로, hwirq 는 여기서 꺼내야만 알 수 있다 */
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);

	mutex_lock(&pcie->lock);

	/* [한국어] 비트맵에서 그 자리를 푼다. alloc 때와 같은 차수를 넘겨야 짝이 맞는다 */
	bitmap_release_region(pcie->msi_irq_in_use, data->hwirq,
			      order_base_2(nr_irqs));

	mutex_unlock(&pcie->lock);

	/* [한국어] 가상 IRQ 매핑을 커널 공통 구현으로 지운다. **비트맵 조작보다 나중** 인
	 * 이유는, 그 전에 irq_data 에서 hwirq 를 꺼내야 하기 때문이다 */
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

/* [한국어] MSI 바닥 도메인의 동작 정의 */
static const struct irq_domain_ops mtk_msi_bottom_domain_ops = {
	/* [한국어] alloc/free 만 있고 map 이 없다. MSI 벡터는 장치 트리로 참조되지 않고
	 * 장치가 요청할 때 동적으로 떼어 주기 때문이다 — INTx 도메인이 map 만
	 * 갖는 것과 정확히 반대다 */
	.alloc = mtk_msi_bottom_domain_alloc,
	.free = mtk_msi_bottom_domain_free,
};

/* [한국어]
 * mtk_intx_mask - INTx 하나를 끈다
 *
 * @data: 이 INTx 의 irq_data. chip_data 가 struct mtk_gen3_pcie 다
 *   (mtk_pcie_intx_map 이 domain->host_data 를 넣는다). hwirq 는 0~3 이며
 *   INTA~INTD 에 대응한다.
 * @return: 없음.
 *
 * 컨트롤러 인터럽트 활성화 레지스터의 비트 24~27 이 INTA~INTD 다. hwirq 에
 * PCIE_INTX_SHIFT(24)를 더해 자리를 구하고 그 비트를 내린다.
 *
**앞 세대와 가장 뚜렷이 갈리는 대목**이다. pcie-mediatek.c 의 v1/v2 는
 * 하드웨어가 INTx 넷을 한 덩어리로만 마스크할 수 있어 dummy_irq_chip 을
 * 쓴다 — 즉 개별 마스크를 포기했다. 여기서는 비트가 하나씩 있으므로 진짜
 * 마스크가 가능하다.
 *
 * 읽고-고쳐-쓰기라 irq_lock 이 필요하다. 이 레지스터는 INTx 비트와 MSI
 * 세트 비트를 함께 담고 있어, 락 없이 고치면 mtk_pcie_enable_msi 나
 * mtk_pcie_irq_restore 의 변경과 겹칠 수 있다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수도, 프로세스 컨텍스트일 수도 있다.
 *
 * 호출 체인:
 *   disable_irq / handle_fasteoi_irq 의 마스크 경로 → [이 함수]
 */
static void mtk_intx_mask(struct irq_data *data)
{
	struct mtk_gen3_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* [한국어] 락 플래그 */
	unsigned long flags;
	/* [한국어] 레지스터 값 */
	u32 val;

	/* [한국어] irqsave 판을 쓰는 것은 이 콜백이 인터럽트 컨텍스트에서도 불릴 수 있기
	 * 때문이다. raw 스핀락이라 PREEMPT_RT 에서도 잠들지 않는다 */
	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	/* [한국어] 현재 값을 읽는다 */
	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	/* [한국어] 해당 INTx 비트를 내린다. hwirq(0~3)에 24를 더해 자리를 구한다 */
	val &= ~BIT(data->hwirq + PCIE_INTX_SHIFT);
	/* [한국어] 되쓴다 */
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

/* [한국어]
 * mtk_intx_unmask - INTx 하나를 켠다
 *
 * @data: 이 INTx 의 irq_data. hwirq 0~3 이 INTA~INTD 다.
 * @return: 없음.
 *
 * mtk_intx_mask 의 반대로 비트를 세운다. 락과 비트 위치 계산은 그쪽과 같다.
 *
 * mtk_pcie_startup_port 가 링크를 세울 때 INTx 를 모두 마스크해 두므로,
 * INTx 를 쓰는 장치 드라이버가 request_irq 를 한 뒤에야 이 함수를 통해
 * 해당 선이 열린다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수도, 프로세스 컨텍스트일 수도 있다.
 *
 * 호출 체인:
 *   request_irq / enable_irq → [이 함수]
 */
static void mtk_intx_unmask(struct irq_data *data)
{
	struct mtk_gen3_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* [한국어] 락 플래그 */
	unsigned long flags;
	/* [한국어] 레지스터 값 */
	u32 val;

	/* [한국어] **읽고-고쳐-쓰기라 락이 필요하다.** 이 레지스터에는 INTx 넷과 MSI 세트
	 * 여덟 개의 비트가 함께 있어, 락 없이 고치면 다른 쪽의 변경이 사라진다 */
	raw_spin_lock_irqsave(&pcie->irq_lock, flags);
	/* [한국어] 현재 값을 읽는다 */
	val = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);
	/* [한국어] 해당 INTx 비트를 세운다 */
	val |= BIT(data->hwirq + PCIE_INTX_SHIFT);
	/* [한국어] 되쓴다 */
	writel_relaxed(val, pcie->base + PCIE_INT_ENABLE_REG);
	/* [한국어] 락을 푼다 */
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);
}

/**
 * mtk_intx_eoi() - Clear INTx IRQ status at the end of interrupt
 * @data: pointer to chip specific data
 *
 * As an emulated level IRQ, its interrupt status will remain
 * until the corresponding de-assert message is received; hence that
 * the status can only be cleared when the interrupt has been serviced.
 */
/* [한국어]
 * mtk_intx_eoi - 처리를 마친 INTx 의 상태 비트를 지운다
 *
 * @data: 이 INTx 의 irq_data.
 * @return: 없음.
 *
 * 위의 원문 kernel-doc 가 설명하듯, 이 하드웨어의 INTx 는 **에뮬레이트된
 * 레벨 인터럽트** 다. PCIe 에는 물리적인 INTx 선이 없고 Assert_INTx /
 * Deassert_INTx 메시지가 그 역할을 대신하는데, 컨트롤러는 그것을 레벨
 * 인터럽트처럼 보이게 흉내 낸다. 그래서 상태 비트가 "처리했다" 고 표시할
 * 때까지 남는다.
 *
 * 레벨 인터럽트이므로 handle_fasteoi_irq 를 쓰고, 그 흐름 함수는 핸들러가
 * 끝난 **뒤에** irq_eoi 를 부른다. MSI 쪽이 handle_edge_irq 와 irq_ack 로
 * 핸들러 **전에** 지우는 것과 정반대다 — 레벨은 원인이 사라지기 전에
 * 지우면 곧바로 다시 서기 때문이다.
 *
 * 상태 레지스터는 1을 쓰면 지워진다 — 읽지 않고 BIT 만 쓰는 데서 알 수
 * 있다. 그래서 락이 없다. mask/unmask 가 만지는 활성화 레지스터와는 다른
 * 레지스터다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   mtk_pcie_irq_handler → generic_handle_domain_irq → handle_fasteoi_irq
 *     → (핸들러 실행) → [이 함수]
 */
static void mtk_intx_eoi(struct irq_data *data)
{
	struct mtk_gen3_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* [한국어] 비트 위치 보관용 */
	unsigned long hwirq;

	/* [한국어] INTx 비트의 실제 자리를 구한다 */
	hwirq = data->hwirq + PCIE_INTX_SHIFT;
	/* [한국어] 상태 비트를 지운다. 1을 쓰면 지워지는 레지스터라 읽지 않는다 —
	 * 그래서 락도 필요 없다 */
	writel_relaxed(BIT(hwirq), pcie->base + PCIE_INT_STATUS_REG);
}

/* [한국어] **앞 세대와 갈리는 대목.** pcie-mediatek.c 의 v1/v2 는 하드웨어가 INTx
 * 넷을 한 덩어리로만 마스크해 dummy_irq_chip 을 쓴다. 여기서는 비트가
 * 하나씩 있어 진짜 irq_chip 을 만들 수 있다 */
static struct irq_chip mtk_intx_irq_chip = {
	/* [한국어] INTx 는 마스크·언마스크·EOI 세 콜백만 있으면 된다. compose 가 없는 것은
	 * MSI 가 아니기 때문이고, ack 가 없는 것은 fasteoi 판이 eoi 를 쓰기 때문이다 */
	.irq_mask		= mtk_intx_mask,
	.irq_unmask		= mtk_intx_unmask,
	.irq_eoi		= mtk_intx_eoi,
	.name			= "INTx",
};

/* [한국어]
 * mtk_pcie_intx_map - INTx 가상 IRQ 하나를 하드웨어에 연결한다
 *
 * @domain: INTx 도메인. host_data 가 struct mtk_gen3_pcie 다.
 * @irq:    커널이 잡아 준 가상 IRQ 번호.
 * @hwirq:  0~3. INTA~INTD 에 대응한다.
 * @return: 항상 0. 실패할 여지가 없다.
 *
 * irq_domain_ops.map 콜백이다. 장치 트리에서 INTx 를 참조하는 자식이
 * 생길 때마다 커널이 불러 준다.
 *
 * 두 가지를 심는다.
 *   chip_data: domain->host_data, 즉 컨트롤러 상태. 이후 mtk_intx_mask 계열이
 *     irq_data_get_irq_chip_data 로 이 값을 꺼낸다. MSI 쪽이 세트를 심는
 *     것과 달리 여기서는 컨트롤러 자체를 심는다 — INTx 는 레지스터 하나로
 *     끝나 중간 단위가 필요 없기 때문이다.
 *   irq_chip 과 흐름 함수: mtk_intx_irq_chip 과 handle_fasteoi_irq.
 *     레벨 인터럽트를 흉내 내므로 fasteoi 를 쓴다(mtk_intx_eoi 참조).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(도메인 매핑 생성 경로).
 *
 * 호출 체인:
 *   irq_create_mapping / of_irq_parse 경로 → irq_domain_associate → [이 함수]
 */
static int mtk_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, domain->host_data);
	/* [한국어] irq_chip 과 흐름 함수를 건다. handle_fasteoi_irq 는 핸들러가 끝난 뒤
	 * irq_eoi 를 부르는 판이며, 레벨을 흉내 내는 이 하드웨어에 맞는 선택이다 */
	irq_set_chip_and_handler_name(irq, &mtk_intx_irq_chip,
				      handle_fasteoi_irq, "INTx");
	return 0;
}

/* [한국어] INTx 도메인은 map 콜백 하나만 있으면 된다. 벡터가 넷뿐이고 동적으로
 * 할당·해제할 일이 없어 alloc/free 가 필요 없다 */
static const struct irq_domain_ops intx_domain_ops = {
	.map = mtk_pcie_intx_map,
};

/* [한국어]
 * mtk_pcie_init_irq_domains - INTx 도메인과 MSI 바닥 도메인을 만든다
 *
 * @pcie: 컨트롤러 상태. 두 도메인 포인터와 두 락을 여기서 초기화한다.
 * @return: 0 성공. interrupt-controller 노드가 없거나 도메인 생성이 실패하면
 *   음수.
 *
 * 이 컨트롤러의 인터럽트는 선 하나로 올라온다. 그 하나를 INTx 넷과 MSI
 * 256개로 갈라 주기 위해 도메인 둘을 만든다.
 *
 * INTx 쪽:
 *   장치 트리의 자식 노드 "interrupt-controller" 를 fwnode 로 삼는다. 자식
 *   장치가 interrupts 속성으로 이 노드를 가리키면 커널이 그 매핑을
 *   mtk_pcie_intx_map 으로 연결한다. 노드가 없으면 INTx 를 표현할 방법이
 *   없으므로 실패로 끝낸다. 크기는 PCI_NUM_INTX — INTA~INTD 넷이다.
 *
 * MSI 쪽:
 *   msi_create_parent_irq_domain 으로 만든다. 이것이 앞 세대와 갈리는 또
 *   하나의 지점이다. 옛 방식은 드라이버가 상위 MSI 도메인까지 직접 만들었지만,
 *   지금은 바닥 도메인만 만들고 그 위는 커널이 mtk_msi_parent_ops 를 보고
 *   붙여 준다. 그 ops 의 init_dev_msi_info 가 msi_lib_init_dev_msi_info 로
 *   되어 있어 공통 구현을 그대로 쓴다.
 *   같은 파일의 mtk_msi_parent_ops 정의에 required/supported 플래그가 있고,
 *   거기서 MSI-X 와 다중 MSI 지원 여부가 정해진다.
 *
 * 락 초기화 두 가지가 여기 섞여 있다.
 *   irq_lock  : 레지스터 접근용 raw_spinlock. 함수 맨 앞에서 초기화한다.
 *   lock      : MSI 비트맵용 mutex. MSI 준비 직전에 초기화한다.
 * 둘의 종류가 다른 이유는 mtk_msi_bottom_irq_mask 와
 * mtk_msi_bottom_domain_alloc 의 설명에 있다.
 *
 * 정리 경로가 두 갈래인 데 주의한다. INTx 도메인을 만든 뒤 MSI 가 실패하면
 * INTx 도메인까지 되돌려야 하고(err_msi_bottom_domain), 그 전에 실패하면
 * 노드 참조만 놓으면 된다(out_put_node). of_get_child_by_name 이 참조를
 * 올려 주므로 성공 경로에서도 반드시 of_node_put 을 부른다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). 아직 하드웨어를 켜기
 * 전이다 — mtk_pcie_setup_irq 가 mtk_pcie_setup 보다 먼저 불린다.
 *
 * 호출 체인:
 *   mtk_pcie_setup_irq → [이 함수]
 *     → irq_domain_create_linear, msi_create_parent_irq_domain
 */
static int mtk_pcie_init_irq_domains(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] INTx 를 위한 자식 노드와 이 컨트롤러의 노드 */
	struct device_node *intc_node, *node = dev->of_node;
	/* [한국어] 오류 코드 보관용 */
	int ret;

	raw_spin_lock_init(&pcie->irq_lock);

	/* Setup INTx */
	intc_node = of_get_child_by_name(node, "interrupt-controller");
	/* [한국어] 자식 노드가 없으면 장치 트리가 불완전한 것이다 */
	if (!intc_node) {
		/* [한국어] 이 노드가 없으면 INTx 를 표현할 방법이 없다 */
		dev_err(dev, "missing interrupt-controller node\n");
		return -ENODEV;
	}

	/* [한국어] INTx 도메인을 만든다. 크기가 PCI_NUM_INTX 인 것은 INTA~INTD 넷을
	 * 다루기 때문이다. 자식 노드를 fwnode 로 삼아야 장치 트리의 interrupts
	 * 속성이 이 도메인을 가리킬 수 있다 */
	pcie->intx_domain = irq_domain_create_linear(of_fwnode_handle(intc_node), PCI_NUM_INTX,
						     &intx_domain_ops, pcie);
	/* [한국어] 실패하면 노드 참조만 놓고 끝낸다 */
	if (!pcie->intx_domain) {
		/* [한국어] INTx 도메인 생성 실패 */
		dev_err(dev, "failed to create INTx IRQ domain\n");
		ret = -ENODEV;
		goto out_put_node;
	}

	/* Setup MSI */
	mutex_init(&pcie->lock);

	/* [한국어] 도메인 정보를 채운다. host_data 에 pcie 를 넣어 두면 irq_chip 콜백들이
	 * data->domain->host_data 로 되찾는다 */
	struct irq_domain_info info = {
		/* [한국어] 이 컨트롤러의 fwnode 를 도메인 식별자로 쓴다. INTx 가 자식 노드를
		 * 쓰는 것과 달리 MSI 는 컨트롤러 자신을 쓴다 — MSI 는 장치 트리로
		 * 참조되는 대상이 아니기 때문이다 */
		.fwnode		= dev_fwnode(dev),
		.ops		= &mtk_msi_bottom_domain_ops,
		.host_data	= pcie,
		.size		= PCIE_MSI_IRQS_NUM,
	};

	/* [한국어] **MSI 바닥 도메인만 만든다.** 그 위의 장치별 도메인은 커널이
	 * mtk_msi_parent_ops 를 보고 붙여 준다. 드라이버가 상위 도메인까지 직접
	 * 만들던 옛 방식을 대신하는 구조다 */
	pcie->msi_bottom_domain = msi_create_parent_irq_domain(&info, &mtk_msi_parent_ops);
	/* [한국어] 실패하면 이미 만든 INTx 도메인을 되돌려야 한다 */
	if (!pcie->msi_bottom_domain) {
		/* [한국어] MSI 도메인 생성 실패 */
		dev_err(dev, "failed to create MSI bottom domain\n");
		ret = -ENODEV;
		goto err_msi_bottom_domain;
	}

	of_node_put(intc_node);
	return 0;

err_msi_bottom_domain:
	irq_domain_remove(pcie->intx_domain);
out_put_node:
	of_node_put(intc_node);
	return ret;
}

/* [한국어]
 * mtk_pcie_irq_teardown - 인터럽트 연결과 두 도메인을 걷어 낸다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음.
 *
 * 순서가 중요하다. **먼저 체인 핸들러를 떼고** 도메인을 지운다. 반대로
 * 하면 이미 사라진 도메인으로 인터럽트가 들어올 수 있다.
 * irq_set_chained_handler_and_data 에 NULL 을 주는 것이 그 분리다.
 *
 * 도메인 포인터를 검사하는 것은 부분 실패를 감안한 것이다.
 * mtk_pcie_init_irq_domains 가 MSI 단계에서 실패하면 INTx 도메인만 남은
 * 상태로 되돌아가는데, probe 의 오류 경로가 이 함수를 부르므로 한쪽만
 * 있어도 안전해야 한다.
 *
 * irq_dispose_mapping 은 컨트롤러 인터럽트 자체의 매핑을 놓는다.
 * platform_get_irq 가 만든 매핑에 대응한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 오류 경로 또는 remove).
 *
 * 호출 체인:
 *   mtk_pcie_probe(오류) / mtk_pcie_remove → [이 함수]
 */
static void mtk_pcie_irq_teardown(struct mtk_gen3_pcie *pcie)
{
	irq_set_chained_handler_and_data(pcie->irq, NULL, NULL);

	/* [한국어] INTx 도메인이 만들어졌으면 지운다 */
	if (pcie->intx_domain)
		irq_domain_remove(pcie->intx_domain);

	/* [한국어] MSI 도메인도 있으면 지운다. 두 도메인 다 검사하는 것은
	 * mtk_pcie_init_irq_domains 가 중간에 실패한 상태에서도 이 함수가
	 * 불릴 수 있기 때문이다 */
	if (pcie->msi_bottom_domain)
		irq_domain_remove(pcie->msi_bottom_domain);

	irq_dispose_mapping(pcie->irq);
}

/* [한국어]
 * mtk_pcie_msi_handler - MSI 세트 하나의 대기 벡터를 모두 처리한다
 *
 * @pcie:    컨트롤러 상태.
 * @set_idx: 처리할 세트 번호(0~7). 상위 핸들러가 상태 비트에서 알아낸다.
 * @return: 없음.
 *
 * 세트 하나가 벡터 32개를 담당하므로, 인터럽트 한 번에 그 안의 여러 벡터가
 * 동시에 서 있을 수 있다. 이 함수가 그것을 모두 훑는다.
 *
 * 활성화 마스크를 **루프 밖에서 한 번만** 읽는 것에 주의한다. 루프 도중
 * 어떤 벡터가 마스크되어도 이번 바퀴에서는 처리된다. 이미 도착한
 * 인터럽트를 놓치지 않는 쪽을 택한 것이며, 매 바퀴 읽으면 MMIO 읽기가
 * 늘어나기도 한다.
 *
 * 무한 루프로 도는 이유: 비트를 처리하는 동안 새 MSI 가 도착해 다른 비트가
 * 설 수 있다. 상태가 완전히 빌 때까지 반복해야 인터럽트가 레벨로 남아
 * 있는 컨트롤러에서 재진입 없이 끝난다. 상태 비트는 각 벡터의 irq_ack
 * (mtk_msi_bottom_irq_ack)가 지우므로, 처리한 비트는 다음 바퀴에서 사라진다.
 * 그래서 루프가 끝난다.
 *
 * 전역 hwirq 를 복원하는 계산이 bit + set_idx * 32 다. 세트 안의 자리에
 * 세트의 시작 번호를 더해 0~255 범위의 번호를 만든다 — 도메인은 전역
 * 번호로 벡터를 찾기 때문이다. 이는 mtk_compose_msi_msg 가 hwirq % 32 로
 * 데이터를 만드는 것의 역연산에 해당한다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(체인 핸들러 안).
 *
 * 호출 체인:
 *   mtk_pcie_irq_handler → [이 함수] → generic_handle_domain_irq
 */
static void mtk_pcie_msi_handler(struct mtk_gen3_pcie *pcie, int set_idx)
{
	struct mtk_msi_set *msi_set = &pcie->msi_sets[set_idx];
	/* [한국어] 활성화 마스크와 대기 상태 */
	unsigned long msi_enable, msi_status;
	/* [한국어] 비트 위치와 전역 hwirq */
	irq_hw_number_t bit, hwirq;

	/* [한국어] **활성화 마스크는 루프 밖에서 한 번만 읽는다.** 루프 도중 어떤 벡터가
	 * 마스크되어도 이번 바퀴에서는 처리된다 — 이미 도착한 인터럽트를 놓치지
	 * 않는 쪽을 택한 것이고, MMIO 읽기를 줄이는 효과도 있다 */
	msi_enable = readl_relaxed(msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);

	do {
		/* [한국어] 매 바퀴 상태를 다시 읽는다. 처리 도중 새 MSI 가 도착할 수 있기 때문이다 */
		msi_status = readl_relaxed(msi_set->base +
					   PCIE_MSI_SET_STATUS_OFFSET);
		/* [한국어] **마스크된 벡터는 걸러 낸다.** 꺼진 벡터의 대기 비트가 서 있어도
		 * 처리하지 않는다 */
		msi_status &= msi_enable;
		/* [한국어] 처리할 것이 없으면 루프를 끝낸다. 이것이 유일한 탈출구다 */
		if (!msi_status)
			break;

		/* [한국어] 선 비트를 모두 훑는다 */
		for_each_set_bit(bit, &msi_status, PCIE_MSI_IRQS_PER_SET) {
			/* [한국어] 세트 안의 자리에 세트 시작 번호를 더해 전역 번호(0~255)를 만든다.
			 * mtk_compose_msi_msg 가 hwirq % 32 로 데이터를 만든 것의 역연산이다 */
			hwirq = bit + set_idx * PCIE_MSI_IRQS_PER_SET;
			/* [한국어] 전역 hwirq 로 도메인에 넘긴다. 여기서 handle_edge_irq 가 돌고,
			 * 그것이 irq_ack(mtk_msi_bottom_irq_ack)로 상태 비트를 지운 뒤 핸들러를 부른다 */
			generic_handle_domain_irq(pcie->msi_bottom_domain, hwirq);
		}
	} while (true);
}

/* [한국어]
 * mtk_pcie_irq_handler - 컨트롤러 인터럽트를 INTx 와 MSI 로 가른다
 *
 * @desc: 이 인터럽트의 irq_desc. handler_data 에 컨트롤러 상태가 들어 있다
 *   (mtk_pcie_setup_irq 가 넣는다).
 * @return: 없음.
 *
 * 체인 핸들러다. 상위 인터럽트 컨트롤러(보통 GIC)의 선 하나에 붙어, 그것을
 * 아래 도메인 둘로 분배한다. chained_irq_enter / _exit 로 감싸는 것은 상위
 * 컨트롤러에 대한 ack/eoi 를 대신 처리하기 위해서다 — 이 짝이 없으면 상위
 * 컨트롤러가 다음 인터럽트를 올려 주지 않거나 폭주한다.
 *
 * 상태 레지스터 하나에 두 종류가 들어 있다.
 *   비트 24~27 : INTA~INTD (PCIE_INTX_SHIFT)
 *   비트  8~15 : MSI 세트 0~7 (PCIE_MSI_SHIFT)
 * 그래서 상태를 한 번만 읽고 두 번 훑는다.
 *
 * for_each_set_bit_from 을 쓰는 이유가 여기 있다. 일반적인
 * for_each_set_bit 은 0부터 훑지만, 여기서는 24부터 27까지 / 8부터 15까지
 * 같은 **부분 구간** 만 봐야 하므로 시작 위치를 지정할 수 있는 판이 필요하다.
 * 그래서 irq_bit 를 미리 시작값으로 두고, 두 번째 루프 전에 다시 설정한다.
 *
 * 상태 비트를 지우는 방식이 둘로 갈리는 데 주의한다.
 *   INTx: 여기서 지우지 않는다. 레벨을 흉내 내므로 핸들러가 끝난 뒤
 *         mtk_intx_eoi 가 지운다.
 *   MSI : 세트를 처리한 **뒤** 여기서 세트 비트를 지운다. 세트 안의 개별
 *         벡터 비트는 각 벡터의 irq_ack 가 이미 지웠고, 이것은 그 위층의
 *         "이 세트에 뭔가 있다" 비트다. 처리 전에 지우면 처리 도중 도착한
 *         MSI 가 세운 세트 비트를 지워 인터럽트를 잃는다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   GIC 등 상위 컨트롤러 → [이 함수]
 *     → generic_handle_domain_irq(INTx), mtk_pcie_msi_handler(MSI)
 */
static void mtk_pcie_irq_handler(struct irq_desc *desc)
{
	struct mtk_gen3_pcie *pcie = irq_desc_get_handler_data(desc);
	/* [한국어] 상위 인터럽트 컨트롤러의 irq_chip. chained_irq_enter/exit 에 넘긴다 */
	struct irq_chip *irqchip = irq_desc_get_chip(desc);
	/* [한국어] 컨트롤러 인터럽트 상태 */
	unsigned long status;
	/* [한국어] INTx 구간의 시작 비트로 초기화한다 */
	irq_hw_number_t irq_bit = PCIE_INTX_SHIFT;

	/* [한국어] 상위 컨트롤러(보통 GIC)에 대한 ack 를 대신 처리한다.
	 * 이 짝(enter/exit)이 없으면 상위 컨트롤러가 다음 인터럽트를 올려 주지
	 * 않거나 폭주한다 */
	chained_irq_enter(irqchip, desc);

	/* [한국어] 상태를 한 번만 읽는다. INTx 와 MSI 비트가 한 레지스터에 함께 있어
	 * 두 번 읽을 필요가 없다 */
	status = readl_relaxed(pcie->base + PCIE_INT_STATUS_REG);
	/* [한국어] INTx 구간(비트 24~27)만 훑는다. for_each_set_bit 이 아니라 _from 을
	 * 쓰는 것은 0 이 아닌 중간 지점부터 시작해야 하기 때문이다 */
	for_each_set_bit_from(irq_bit, &status, PCI_NUM_INTX +
			      PCIE_INTX_SHIFT)
		generic_handle_domain_irq(pcie->intx_domain,
					  irq_bit - PCIE_INTX_SHIFT);

	/* [한국어] **두 번째 루프 전에 시작 위치를 다시 놓는다.** for_each_set_bit_from 은
	 * 반복자를 시작점으로 삼기 때문에, 앞 루프가 남긴 값을 그대로 두면
	 * MSI 구간을 제대로 훑지 못한다 */
	irq_bit = PCIE_MSI_SHIFT;
	/* [한국어] MSI 세트 구간(비트 8~15)을 훑는다 */
	for_each_set_bit_from(irq_bit, &status, PCIE_MSI_SET_NUM +
			      PCIE_MSI_SHIFT) {
		/* [한국어] 그 세트 안의 대기 벡터를 모두 처리한다 */
		mtk_pcie_msi_handler(pcie, irq_bit - PCIE_MSI_SHIFT);

		/* [한국어] **세트를 처리한 뒤** 세트 비트를 지운다. 처리 전에 지우면, 처리 도중
		 * 도착한 MSI 가 세운 비트까지 지워져 인터럽트를 잃는다.
		 * INTx 는 여기서 지우지 않는다 — 레벨을 흉내 내므로 mtk_intx_eoi 가
		 * 핸들러 뒤에 지운다 */
		writel_relaxed(BIT(irq_bit), pcie->base + PCIE_INT_STATUS_REG);
	}

	chained_irq_exit(irqchip, desc);
}

/* [한국어]
 * mtk_pcie_setup_irq - 인터럽트 번호를 얻고 도메인을 만들어 핸들러를 건다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. IRQ 를 못 얻거나 도메인 생성이 실패하면 음수.
 *
 * 세 단계를 순서대로 한다 — 번호 얻기, 도메인 만들기, 핸들러 걸기.
 * 도메인이 준비되기 전에 핸들러를 걸면 인터럽트가 갈 곳이 없으므로 순서가
 * 고정되어 있다.
 *
 * 이 함수는 mtk_pcie_probe 에서 **하드웨어를 건드리기 전에** 불린다.
 * mtk_pcie_setup 보다 앞이라는 뜻이며, 전원이 올라오기 전에 인터럽트 받을
 * 준비를 끝내 두는 구조다. 그래야 링크가 서는 순간 올라오는 인터럽트를
 * 놓치지 않는다.
 *
 * platform_get_irq 실패값을 그대로 반환하는 것에 주의 — -EPROBE_DEFER 일
 * 수 있고, 그러면 probe 가 나중에 다시 시도된다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   mtk_pcie_probe → [이 함수]
 *     → platform_get_irq, mtk_pcie_init_irq_domains,
 *       irq_set_chained_handler_and_data
 */
static int mtk_pcie_setup_irq(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 플랫폼 장치로 되돌린다 */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 컨트롤러 인터럽트 번호를 얻는다. 인터럽트가 하나뿐이라 인덱스 0 이다 */
	pcie->irq = platform_get_irq(pdev, 0);
	/* [한국어] platform_get_irq 는 오류를 음수로 돌려준다 */
	if (pcie->irq < 0)
		/* [한국어] **실패값을 그대로 올린다.** -EPROBE_DEFER 일 수 있고, 그러면 probe 가
		 * 나중에 다시 시도된다 */
		return pcie->irq;

	/* [한국어] INTx 도메인과 MSI 바닥 도메인을 만든다 */
	err = mtk_pcie_init_irq_domains(pcie);
	/* [한국어] 도메인 생성 실패면 IRQ 매핑만 남은 상태로 반환한다.
	 * 그 매핑은 probe 오류 경로의 mtk_pcie_irq_teardown 이 정리한다 */
	if (err)
		return err;

	/* [한국어] **도메인이 준비된 뒤에** 체인 핸들러를 건다. 순서가 뒤바뀌면 인터럽트가
	 * 갈 곳이 없다. pcie 를 handler_data 로 넘겨 핸들러가 상태를 되찾게 한다 */
	irq_set_chained_handler_and_data(pcie->irq, mtk_pcie_irq_handler, pcie);

	return 0;
}

/* [한국어]
 * mtk_pcie_parse_port - 장치 트리에서 이 포트의 자원을 모두 얻는다
 *
 * @pcie: 컨트롤러 상태. 이 함수가 base, reg_base, 리셋, PHY, 클록,
 *   num_lanes 를 채운다.
 * @return: 0 성공. 자원 획득 실패 시 음수(-EPROBE_DEFER 포함).
 *
 * 하드웨어를 건드리기 전에 필요한 것을 모두 손에 넣는 단계다. 여기서
 * 얻은 것들을 mtk_pcie_setup 이 순서대로 쓴다.
 *
 * 레지스터 창은 이름("pcie-mac")으로 찾는다. 번호가 아니라 이름을 쓰는
 * 것은 장치 트리에 창이 여럿일 수 있기 때문이다. 가상 주소(base)와
 * 물리 주소(reg_base)를 **둘 다** 보관하는 것이 중요한데, 물리 주소는
 * MSI 수신 주소를 장치에게 알려 줄 때 필요하다(mtk_pcie_enable_msi 참조).
 *
 * 리셋이 두 갈래다.
 *   PHY 리셋: bulk 로 얻는다. 개수와 이름이 SoC 마다 달라
 *     pcie->soc->phy_resets 에서 가져온다. MT8192/MT8196 은 "phy" 하나,
 *     EN7581 은 "phy-lane0/1/2" 셋이다. **shared** 로 얻는 것에 주의 —
 *     같은 리셋 선을 다른 장치와 나눠 쓸 수 있다는 뜻이고, 그래서
 *     mtk_pcie_setup 이 균형을 맞추려 한 번 디어서트해 둔다.
 *   MAC 리셋: "mac" 하나를 exclusive 로 얻는다.
 * 둘 다 optional 이라 없어도 오류가 아니다.
 *
 * PHY 와 클록도 optional 이거나 통째로 얻는다. 클록은
 * devm_clk_bulk_get_all 로 개수를 세지 않고 전부 가져온다 — SoC 마다
 * 클록 개수가 달라 하드코딩할 수 없기 때문이다. 반환값이 개수이고
 * 음수면 오류다.
 *
 * num-lanes 는 검증이 특이하다. 잘못된 값이면 실패시키지 않고 경고만
 * 찍은 뒤 컨트롤러 기본값을 쓴다. 유효 조건은 1~16 이면서 1이거나 짝수 —
 * PCIe 링크 폭이 x1, x2, x4, x8, x16 뿐이기 때문이다. 값을 못 읽으면
 * (속성이 없으면) num_lanes 는 0 으로 남고, mtk_pcie_startup_port 가
 * 0 을 "설정하지 않음" 으로 해석한다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   mtk_pcie_setup → [이 함수]
 */
static int mtk_pcie_parse_port(struct mtk_gen3_pcie *pcie)
{
	int i, ret, num_resets = pcie->soc->phy_resets.num_resets;
	/* [한국어] 로그와 devm 자원 획득의 기준 */
	struct device *dev = pcie->dev;
	/* [한국어] 플랫폼 장치로 되돌린다. 리소스와 IRQ 를 얻으려면 이 타입이 필요하다 */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 레지스터 창 리소스 */
	struct resource *regs;
	/* [한국어] 장치 트리에서 읽을 레인 수 임시 변수 */
	u32 num_lanes;

	/* [한국어] 레지스터 창을 **이름** 으로 찾는다. 번호가 아니라 이름을 쓰는 것은
	 * 장치 트리에 창이 여럿일 수 있기 때문이다 */
	regs = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pcie-mac");
	/* [한국어] 이름이 맞는 창이 없으면 장치 트리가 잘못된 것이다 */
	if (!regs)
		return -EINVAL;
	/* [한국어] 레지스터 창을 가상 주소 공간에 매핑한다. devm 이라 remove 때 자동으로
	 * 풀린다. 절전 중에도 매핑은 유효해 resume 이 다시 하지 않는다 */
	pcie->base = devm_ioremap_resource(dev, regs);
	/* [한국어] devm_ioremap_resource 는 오류를 포인터로 돌려준다 */
	if (IS_ERR(pcie->base))
		/* [한국어] 매핑 실패. dev_err_probe 를 쓰면 -EPROBE_DEFER 일 때 로그가 억제된다 */
		return dev_err_probe(dev, PTR_ERR(pcie->base), "failed to map register base\n");

	/* [한국어] **같은 창의 물리 주소를 따로 보관한다.** MSI 수신 주소는 커널이 아니라
	 * PCIe 장치가 쓰기를 보낼 대상이라 물리 주소여야 하기 때문이다
	 * (mtk_pcie_enable_msi 참조) */
	pcie->reg_base = regs->start;

	/* [한국어] SoC 가 지정한 개수만큼만 순회한다. MT8192 계열은 1, EN7581 은 3 이다 */
	for (i = 0; i < num_resets; i++)
		/* [한국어] pdata 의 이름을 bulk 구조체에 복사한다 */
		pcie->phy_resets[i].id = pcie->soc->phy_resets.id[i];

	/* [한국어] PHY 리셋들을 한꺼번에 얻는다. **shared 인 점이 중요하다** — 같은 리셋
	 * 선을 다른 장치와 나눠 쓸 수 있어 커널이 어서트/디어서트 횟수를 센다.
	 * 그래서 mtk_pcie_setup 이 균형을 맞추려 한 번 디어서트해 둔다 */
	ret = devm_reset_control_bulk_get_optional_shared(dev, num_resets,
							  pcie->phy_resets);
	/* [한국어] optional 이라 리셋이 없어도 오류가 아니다 */
	if (ret)
		/* [한국어] 리셋 획득 실패는 -EPROBE_DEFER 일 수 있다 */
		return dev_err_probe(dev, ret, "failed to get PHY bulk reset\n");

	/* [한국어] MAC 리셋을 exclusive 로 얻는다. PHY 리셋이 shared 인 것과 대비되는데,
	 * MAC 리셋은 이 컨트롤러 전용이라 나눠 쓸 이유가 없기 때문이다 */
	pcie->mac_reset = devm_reset_control_get_optional_exclusive(dev, "mac");
	/* [한국어] optional 이라 없으면 NULL 이 오고 오류 포인터만 걸러 낸다 */
	if (IS_ERR(pcie->mac_reset))
		/* [한국어] MAC 리셋 획득 실패 */
		return dev_err_probe(dev, PTR_ERR(pcie->mac_reset), "failed to get MAC reset\n");

	/* [한국어] PCIe PHY 를 이름 "pcie-phy" 로 얻는다 */
	pcie->phy = devm_phy_optional_get(dev, "pcie-phy");
	/* [한국어] optional 이라 없으면 NULL 이지만, 오류 포인터는 별개로 걸러야 한다 */
	if (IS_ERR(pcie->phy))
		/* [한국어] PHY 획득 실패. optional 이므로 없어서가 아니라 다른 이유다 */
		return dev_err_probe(dev, PTR_ERR(pcie->phy), "failed to get PHY\n");

	/* [한국어] 클록을 이름 없이 통째로 얻는다. 개수와 이름이 SoC 마다 달라
	 * 하드코딩할 수 없기 때문이다 */
	pcie->num_clks = devm_clk_bulk_get_all(dev, &pcie->clks);
	/* [한국어] **반환값이 개수이고 음수면 오류다.** 이 검사를 지나면 num_clks 는
	 * 항상 0 이상이므로 이후 clk_bulk 호출이 안전하다 */
	if (pcie->num_clks < 0)
		/* [한국어] 클록을 못 얻으면 -EPROBE_DEFER 일 수 있다 */
		return dev_err_probe(dev, pcie->num_clks, "failed to get clocks\n");

	/* [한국어] 장치 트리의 num-lanes 를 읽는다 */
	ret = of_property_read_u32(dev->of_node, "num-lanes", &num_lanes);
	/* [한국어] 속성이 있을 때만 검사한다. 없으면 num_lanes 가 0 으로 남는다 */
	if (ret == 0) {
		/* [한국어] 유효 조건은 1~16 이면서 1이거나 짝수다. PCIe 링크 폭이 x1, x2, x4,
		 * x8, x16 뿐이기 때문인데, 이 검사는 6 이나 12 도 통과시킨다 —
		 * 실제 장치 트리에 그런 값이 없어 문제가 되지 않는 느슨한 검사다 */
		if (num_lanes == 0 || num_lanes > 16 ||
		    (num_lanes != 1 && num_lanes % 2))
			/* [한국어] **실패시키지 않고 경고만 찍는다.** 링크 폭을 설정하지 못해도 컨트롤러
			 * 기본값으로 동작할 수 있으므로, 부팅 자체를 막을 이유가 없다는 판단이다 */
			dev_warn(dev, "invalid num-lanes, using controller defaults\n");
		else
			/* [한국어] 검증을 통과한 값만 채택한다. 통과하지 못하면 0 으로 남아
			 * mtk_pcie_startup_port 가 링크 폭을 건드리지 않는다 */
			pcie->num_lanes = num_lanes;
	}

	return 0;
}

/* [한국어]
 * mtk_pcie_en7581_power_up - Airoha EN7581 전용 전원 시퀀스
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * pdata 의 power_up 콜백 두 구현 중 하나다. 같은 IP 를 쓰지만 EN7581 은
 * 순서와 추가 설정이 달라 함수를 따로 둔다. 무엇이 다른지가 이 함수의
 * 요점이다.
 *
 * 첫째, **PHY 초기화가 리셋 디어서트보다 먼저** 온다. 위의 원문 주석이
 * 그 사실을 명시한다. mtk_pcie_power_up 은 리셋을 푼 뒤 PHY 를 켜는데,
 * 여기서는 PHY 를 켠 뒤 리셋을 푼다.
 *
 * 둘째, PBus 주소 범위를 syscon 으로 알려 준다. 하드웨어가 "이 주소가
 * PCIe 로 가야 하는지" 판단하려면 기준 주소와 마스크가 필요하다.
 * mediatek,pbus-csr 프로퍼티가 regmap 과 두 오프셋(args[0], args[1])을
 * 함께 준다. 마스크는 GENMASK(31, __fls(size)) — 크기의 최상위 비트
 * 위쪽을 모두 세운 값이라, 크기가 2의 거듭제곱일 때 그 범위를 정확히
 * 가리키는 마스크가 된다.
 *
 * 셋째, 이퀄라이저 프리셋을 직접 쓴다. PCIE_EQ_PRESET_01_REG 에 레인 0/1 의
 * 상하류 값을, PCIE_PIPE4_PIE8_REG 에 프리셋 선택과 질의 관련 필드를 쓴다.
**여기 쓰이는 0x47, 0x41, 0x80, 0x2, 0xf 는 이 트리에 근거가 없다.**
 * 필드 이름으로 미루어 신호 이퀄라이제이션 튜닝 값으로 보이지만, 각 숫자가
 * 무엇을 뜻하는지는 미디어텍/에어로하 문서 없이는 알 수 없다. 확실한 것은
 * FIELD_PREP 이 각 값을 해당 마스크 자리로 옮겨 넣는다는 것뿐이다.
 *
 * 넷째, 리셋 대기가 길다. 어서트 뒤와 디어서트 뒤에 각각 100ms 를 쉰다
 * (PCIE_EN7581_RESET_TIME_MS). 원문 주석이 EN7581 고유의 시간이라고 밝힌다.
 * mtk_pcie_power_up 은 10us 만 쉰다.
 *
 * 다섯째, 함수 맨 앞에서 리셋을 어서트한다. 원문 주석대로 부트로더가
 * 컨트롤러를 리셋에서 꺼내 놓았을 수 있어, 깨끗한 상태에서 시작하기
 * 위해서다. 이 부분만은 mtk_pcie_power_up 과 같다.
 *
 * 정리 경로가 세 단계로 층져 있다 — 클록 실패면 pm_runtime 과 리셋까지
 * 되돌리고, 리셋 디어서트 실패면 PHY 전원부터, PHY 전원 실패면 phy_exit
 * 만 한다. 각 라벨이 그 직전까지 성공한 것만 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 여러 번 잠든다.
 *
 * 호출 체인:
 *   mtk_pcie_setup / mtk_pcie_resume_noirq → pcie->soc->power_up → [이 함수]
 */
static int mtk_pcie_en7581_power_up(struct mtk_gen3_pcie *pcie)
{
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);
	/* [한국어] 로그와 자원 접근에 쓸 device 포인터 */
	struct device *dev = pcie->dev;
	/* [한국어] 브리지 창 목록에서 메모리 창을 찾기 위한 반복자 */
	struct resource_entry *entry;
	/* [한국어] PBus 제어 레지스터에 접근할 regmap. **EN7581 만 쓴다** */
	struct regmap *pbus_regmap;
	/* [한국어] args[0] 과 args[1] 은 PBus 레지스터 안의 두 오프셋 —
	 * 기준 주소용과 마스크용이며, 장치 트리가 phandle 인자로 함께 준다 */
	u32 val, args[2], size;
	/* [한국어] PCI 쪽 시작 주소 */
	resource_size_t addr;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/*
	 * The controller may have been left out of reset by the bootloader
	 * so make sure that we get a clean start by asserting resets here.
	 */
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);

	/* Wait for the time needed to complete the reset lines assert. */
	msleep(PCIE_EN7581_RESET_TIME_MS);

	/*
	 * Configure PBus base address and base address mask to allow the
	 * hw to detect if a given address is accessible on PCIe controller.
	 */
	pbus_regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
							   "mediatek,pbus-csr",
							   ARRAY_SIZE(args),
							   args);
	/* [한국어] syscon 을 못 찾으면 -EPROBE_DEFER 일 수 있다 */
	if (IS_ERR(pbus_regmap))
		return PTR_ERR(pbus_regmap);

	/* [한국어] 첫 번째 메모리 창을 찾는다. PBus 에 알려 줄 것은 메모리 범위 하나뿐이다 */
	entry = resource_list_first_type(&host->windows, IORESOURCE_MEM);
	/* [한국어] 메모리 창이 없으면 알려 줄 범위가 없으므로 실패로 끝낸다 */
	if (!entry)
		return -ENODEV;

	/* [한국어] PCI 쪽 주소를 구한다. entry->offset 이 CPU 주소와 PCI 주소의 차이다 */
	addr = entry->res->start - entry->offset;
	/* [한국어] PBus 기준 주소를 쓴다. args[0] 이 그 레지스터의 오프셋이며,
	 * syscon 조회 때 장치 트리가 함께 준 값이다 */
	regmap_write(pbus_regmap, args[0], lower_32_bits(addr));
	/* [한국어] 창의 크기를 32비트로 잘라 낸다 */
	size = lower_32_bits(resource_size(entry->res));
	/* [한국어] 주소 마스크를 쓴다. GENMASK(31, __fls(size)) 는 크기의 최상위 비트
	 * 위쪽을 모두 세운 값이라, 크기가 2의 거듭제곱일 때 그 범위를 정확히
	 * 가리키는 마스크가 된다. 하드웨어는 (주소 & 마스크) == 기준주소 로
	 * PCIe 대상 여부를 판단할 것이다 */
	regmap_write(pbus_regmap, args[1], GENMASK(31, __fls(size)));

	/*
	 * Unlike the other MediaTek Gen3 controllers, the Airoha EN7581
	 * requires PHY initialization and power-on before PHY reset deassert.
	 */
	err = phy_init(pcie->phy);
	/* [한국어] 실패하면 되돌릴 것이 없다. 리셋은 이미 어서트된 상태다 */
	if (err) {
		/* [한국어] PHY 초기화 실패 */
		dev_err(dev, "failed to initialize PHY\n");
		return err;
	}

	/* [한국어] PHY 에 전원을 넣는다 */
	err = phy_power_on(pcie->phy);
	/* [한국어] 실패하면 phy_exit 만 되돌린다 */
	if (err) {
		/* [한국어] PHY 전원 인가 실패 */
		dev_err(dev, "failed to power on PHY\n");
		goto err_phy_on;
	}

	/* [한국어] **PHY 를 켠 뒤에 리셋을 푼다.** mtk_pcie_power_up 과 정반대 순서이며,
	 * 위의 원문 주석이 EN7581 이 그것을 요구한다고 밝힌다 */
	err = reset_control_bulk_deassert(pcie->soc->phy_resets.num_resets,
					  pcie->phy_resets);
	/* [한국어] 실패하면 PHY 전원부터 되감는다 */
	if (err) {
		/* [한국어] 리셋 해제 실패 */
		dev_err(dev, "failed to deassert PHYs\n");
		goto err_phy_deassert;
	}

	/*
	 * Wait for the time needed to complete the bulk de-assert above.
	 * This time is specific for EN7581 SoC.
	 */
	msleep(PCIE_EN7581_RESET_TIME_MS);

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	/* [한국어] **EN7581 전용 이퀄라이저 프리셋.** 레인 0 하류에 0x47 을 넣는다.
	 * 이 숫자들(0x47, 0x41, 그리고 아래의 0x80, 0x2, 0xf)의 근거는 이 트리에
	 * 없다. 확실한 것은 FIELD_PREP 이 각 값을 해당 마스크 자리로 옮긴다는
	 * 것뿐이며, 필드 이름으로 미루어 신호 이퀄라이제이션 튜닝 값으로 보인다 */
	val = FIELD_PREP(PCIE_VAL_LN0_DOWNSTREAM, 0x47) |
	      /* [한국어] 레인 1 하류에도 같은 값. 두 레인에 같은 값을 주는 것으로 보아
	       * 레인별 튜닝이 아니라 일괄 설정이다 */
	      FIELD_PREP(PCIE_VAL_LN1_DOWNSTREAM, 0x47) |
	      /* [한국어] 레인 0 상류에 0x41 */
	      FIELD_PREP(PCIE_VAL_LN0_UPSTREAM, 0x41) |
	      /* [한국어] 레인 1 상류에도 같은 값 */
	      FIELD_PREP(PCIE_VAL_LN1_UPSTREAM, 0x41);
	/* [한국어] 이퀄라이저 프리셋 레지스터에 한 번에 쓴다 */
	writel_relaxed(val, pcie->base + PCIE_EQ_PRESET_01_REG);

	/* [한국어] PHY 파라미터 질의 비트와 질의 시간 초과 비트를 세운다.
	 * 이 두 비트는 값 없이 세우기만 한다 */
	val = PCIE_K_PHYPARAM_QUERY | PCIE_K_QUERY_TIMEOUT |
	      /* [한국어] 16GT/s 용 프리셋 필드에 0x80 */
	      FIELD_PREP(PCIE_K_PRESET_TO_USE_16G, 0x80) |
	      /* [한국어] 사용할 프리셋 필드에 0x2 */
	      FIELD_PREP(PCIE_K_PRESET_TO_USE, 0x2) |
	      /* [한국어] 파인튜닝 최대값 필드에 0xf. 이 값의 근거도 이 트리에 없다 */
	      FIELD_PREP(PCIE_K_FINETUNE_MAX, 0xf);
	/* [한국어] PIPE4/PIE8 레지스터에 쓴다 */
	writel_relaxed(val, pcie->base + PCIE_PIPE4_PIE8_REG);

	/* [한국어] 클록을 켠다. MT8192 계열과 달리 이퀄라이저 설정 뒤에 온다 */
	err = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	/* [한국어] 실패하면 pm_runtime, 리셋, PHY 를 차례로 되감는다 */
	if (err) {
		/* [한국어] 클록 준비 실패 */
		dev_err(dev, "failed to prepare clock\n");
		goto err_clk_prepare_enable;
	}

	/*
	 * Airoha EN7581 performs PCIe reset via clk callbacks since it has a
	 * hw issue with PCIE_PE_RSTB signal. Add wait for the time needed to
	 * complete the PCIe reset.
	 */
	msleep(PCIE_T_PVPERL_MS);

	return 0;

err_clk_prepare_enable:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);
err_phy_deassert:
	phy_power_off(pcie->phy);
err_phy_on:
	phy_exit(pcie->phy);

	return err;
}

/* [한국어]
 * mtk_pcie_power_up - MT8192/MT8196 의 표준 전원 시퀀스
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * pdata 의 power_up 콜백 기본 구현이다. 순서는 아래에서 위로 — PHY 를
 * 먼저 살리고, 그 위에 MAC 을 올리고, 마지막에 클록을 켠다.
 *
 *   1) 모든 리셋을 어서트한다. 원문 주석대로 부트로더가 남겨 놓은 상태를
 *      지우기 위해서다. 10us 쉰다(PCIE_MTK_RESET_TIME_US, 최대 20us).
 *   2) PHY 리셋을 푼 뒤 phy_init / phy_power_on 으로 PHY 를 켠다.
     **EN7581 과 정반대 순서** 다 — 그쪽은 PHY 를 켠 뒤 리셋을 푼다.
 *   3) MAC 리셋을 푼다. 이제 트랜잭션 계층 클록이 돈다.
 *   4) pm_runtime 을 켜고 동기 get 으로 전원 도메인을 확보한다.
 *   5) 클록을 모두 켠다.
 *
 * pm_runtime_get_sync 를 쓰는 것은 전원 도메인이 실제로 올라올 때까지
 * 기다려야 하기 때문이다. 비동기로 두면 클록을 켜는 시점에 도메인이 아직
 * 꺼져 있을 수 있다.
 *
 * 정리 경로가 세 라벨로 나뉜다. 클록 실패면 pm_runtime, MAC 리셋, PHY 전원,
 * phy_exit, PHY 리셋까지 전부 되감고, PHY 전원 실패면 phy_exit 부터,
 * phy_init 실패면 PHY 리셋 어서트만 한다. 라벨 이름이 "실패한 단계" 가
 * 아니라 "되돌리기 시작할 지점" 을 가리키는 흔한 커널 관례다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:
 *   mtk_pcie_setup / mtk_pcie_resume_noirq → pcie->soc->power_up → [이 함수]
 */
static int mtk_pcie_power_up(struct mtk_gen3_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/*
	 * The controller may have been left out of reset by the bootloader
	 * so make sure that we get a clean start by asserting resets here.
	 */
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);
	reset_control_assert(pcie->mac_reset);
	/* [한국어] 리셋을 어서트한 채 10~20us 유지한다. 상한을 두 배로 주는 것은 커널이
	 * 타이머를 뭉쳐 처리할 여지를 주는 관례다. EN7581 이 같은 자리에서
	 * 100ms 를 쉬는 것과 견주면 만 배 짧다 */
	usleep_range(PCIE_MTK_RESET_TIME_US, 2 * PCIE_MTK_RESET_TIME_US);

	/* PHY power on and enable pipe clock */
	err = reset_control_bulk_deassert(pcie->soc->phy_resets.num_resets,
					  pcie->phy_resets);
	/* [한국어] 실패하면 아직 아무것도 켜지 않았으므로 그대로 반환한다 */
	if (err) {
		/* [한국어] 리셋 해제 실패는 리셋 컨트롤러 쪽 문제다 */
		dev_err(dev, "failed to deassert PHYs\n");
		return err;
	}

	/* [한국어] **리셋을 푼 뒤 PHY 를 초기화한다.** mtk_pcie_en7581_power_up 은 이
	 * 순서가 정반대다 — 두 함수를 나눈 근본 이유가 여기 있다 */
	err = phy_init(pcie->phy);
	/* [한국어] 실패하면 PHY 리셋 어서트만 되돌리면 된다 */
	if (err) {
		/* [한국어] PHY 초기화 실패 */
		dev_err(dev, "failed to initialize PHY\n");
		goto err_phy_init;
	}

	/* [한국어] PHY 에 전원을 넣는다. init 과 power_on 이 나뉜 것은 PHY 프레임워크의
	 * 규약으로, 초기화와 전원 인가를 분리해 두었기 때문이다 */
	err = phy_power_on(pcie->phy);
	/* [한국어] 실패하면 phy_exit 부터 되감는다 */
	if (err) {
		/* [한국어] PHY 전원 인가 실패 */
		dev_err(dev, "failed to power on PHY\n");
		goto err_phy_on;
	}

	/* MAC power on and enable transaction layer clocks */
	reset_control_deassert(pcie->mac_reset);

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	/* [한국어] 마지막으로 클록을 켠다. 이 시점에는 PHY 도 MAC 도 리셋에서 풀려 있다 */
	err = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	/* [한국어] 실패하면 pm_runtime 부터 PHY 리셋까지 전부 되감는다 */
	if (err) {
		/* [한국어] 클록 실패는 흔치 않지만 여기까지 왔다면 되감을 것이 많다 */
		dev_err(dev, "failed to enable clocks\n");
		goto err_clk_init;
	}

	return 0;

err_clk_init:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	reset_control_assert(pcie->mac_reset);
	phy_power_off(pcie->phy);
err_phy_on:
	phy_exit(pcie->phy);
err_phy_init:
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);

	return err;
}

/* [한국어]
 * mtk_pcie_power_down - 컨트롤러 전원을 내린다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음. 되돌리는 경로라 실패를 보고할 곳이 없다.
 *
 * 전원을 올린 순서의 정확한 역순이다 — 클록, pm_runtime, MAC 리셋, PHY,
 * PHY 리셋.
 *
**두 power_up 구현이 공유하는 유일한 내림 경로** 라는 점이 중요하다.
 * EN7581 은 올릴 때 순서가 다르지만 내릴 때는 이 함수 하나를 쓴다.
 * 그것이 성립하는 이유는, 내리는 쪽은 순서에 덜 민감하고(이미 동작을
 * 멈추는 중이므로) EN7581 고유의 syscon 설정이나 이퀄라이저 값은
 * 되돌릴 필요가 없기 때문이다.
 *
 * pm_runtime_put_sync 와 pm_runtime_disable 을 함께 부르는 짝은
 * mtk_pcie_power_up 과 mtk_pcie_en7581_power_up 의 오류 경로에도 그대로
 * 나온다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   mtk_pcie_setup(오류) / mtk_pcie_probe(오류) / mtk_pcie_remove /
 *   mtk_pcie_suspend_noirq / mtk_pcie_resume_noirq(오류) → [이 함수]
 */
static void mtk_pcie_power_down(struct mtk_gen3_pcie *pcie)
{
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);

	pm_runtime_put_sync(pcie->dev);
	pm_runtime_disable(pcie->dev);
	reset_control_assert(pcie->mac_reset);

	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
	reset_control_bulk_assert(pcie->soc->phy_resets.num_resets,
				  pcie->phy_resets);
}

/* [한국어]
 * mtk_pcie_get_controller_max_link_speed - 하드웨어가 지원하는 최대 속도를 읽는다
 *
 * @pcie: 컨트롤러 상태. base 가 매핑되고 전원이 올라와 있어야 한다.
 * @return: 지원하는 최대 PCIe 세대(1 이상). 지원 비트가 하나도 없으면 -EINVAL.
 *
 * PCIE_BASE_CFG_REG 의 속도 필드는 **비트마스크** 다 — 세대마다 비트 하나가
 * 서 있는 구조다. fls 로 가장 높이 선 비트의 위치를 구하면 그것이 곧
 * 지원하는 최고 세대 번호가 된다. fls 는 1-based 라 별도 보정이 필요 없다.
 *
 * fls 가 0 이면 켜진 비트가 없다는 뜻이고, 그것은 값을 읽지 못했거나
 * 하드웨어가 이상하다는 신호이므로 -EINVAL 로 알린다.
 *
 * 이 값을 쓰는 곳이 mtk_pcie_setup 하나뿐이며, 거기서 장치 트리가 요구한
 * 속도와 견줘 더 작은 쪽을 쓴다. DT 가 하드웨어보다 높은 속도를 요구해도
 * 컨트롤러가 못 내는 값을 설정하지 않기 위한 안전장치다.
 *
 * 전원이 올라온 뒤에만 유효하다 — mtk_pcie_setup 이 power_up 다음에
 * 부르는 이유다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   mtk_pcie_setup → [이 함수]
 */
static int mtk_pcie_get_controller_max_link_speed(struct mtk_gen3_pcie *pcie)
{
	u32 val;
	/* [한국어] 반환값 보관용 */
	int ret;

	/* [한국어] 컨트롤러 능력 레지스터를 읽는다. 전원이 올라온 뒤에만 유효하다 */
	val = readl_relaxed(pcie->base + PCIE_BASE_CFG_REG);
	/* [한국어] 속도 필드(비트 15~8)만 꺼낸다 */
	val = FIELD_GET(PCIE_BASE_CFG_SPEED, val);
	/* [한국어] 선 비트 중 가장 높은 자리를 찾는다. 필드가 비트마스크라 이 값이 곧
	 * 지원하는 최고 세대 번호가 된다. fls 는 1-based 라 보정이 필요 없다 */
	ret = fls(val);

	return ret > 0 ? ret : -EINVAL;
}

/* [한국어]
 * mtk_pcie_setup - 자원 확보부터 링크 기동까지의 순서를 엮는다
 *
 * @pcie: 컨트롤러 상태. dev 와 soc 만 채워진 상태로 들어온다.
 * @return: 0 성공, 음수 오류.
 *
 * probe 의 하드웨어 절반을 담당한다. 인터럽트 준비는 이미
 * mtk_pcie_setup_irq 가 끝냈고, 여기서 처음으로 하드웨어를 켠다.
 *
 *   1) 장치 트리에서 자원을 모은다(mtk_pcie_parse_port).
 *   2) PHY 리셋을 한 번 디어서트한다. 위의 원문 주석이 밝히듯 **균형을 위한
 *      것** 이다. 이 리셋은 shared 로 얻었으므로 커널이 어서트/디어서트
 *      횟수를 센다. power_up 구현들이 맨 앞에서 어서트부터 하기 때문에,
 *      여기서 미리 한 번 디어서트해 두지 않으면 카운터가 음수 쪽으로
 *      기울어 다른 사용자에게 영향을 준다.
 *   3) 전원을 올린다. **여기서부터만 레지스터를 건드릴 수 있다** — 원문
 *      주석이 그 경계를 명시한다. SoC 에 따라 mtk_pcie_power_up 또는
 *      mtk_pcie_en7581_power_up 이 불린다.
 *   4) 속도 상한을 정한다. 장치 트리의 max-link-speed
 *      (drivers/pci/of.c:2174)를 읽고, 그것이 유효한 속도인지
 *      pcie_get_link_speed(drivers/pci/probe.c:2017)로 확인한 뒤,
 *      컨트롤러가 지원하는 최대와 견준다. DT 값이 하드웨어 능력 이하일
 *      때만 채택한다 — 즉 **낮추는 방향으로만** 작용한다.
 *   5) 링크를 세운다(mtk_pcie_startup_port).
 *
 * 4)의 로그 문구에 주의할 점이 있다. "maximum controller link speed Gen%d,
 * overriding to Gen%u" 로 max_speed 와 채택값을 함께 찍는데, 조건상
 * max_speed >= err 이므로 실제로는 상한보다 같거나 낮은 값으로 맞추는
 * 것이다.
 *
 * 실패하면 전원을 되돌린다. 인터럽트 도메인은 여기서 건드리지 않는다 —
 * 그것은 probe 의 err_tear_down_irq 가 맡는다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). 하위 함수들이 잠든다.
 *
 * 호출 체인:
 *   mtk_pcie_probe → [이 함수]
 *     → mtk_pcie_parse_port, pcie->soc->power_up,
 *       mtk_pcie_get_controller_max_link_speed, mtk_pcie_startup_port
 */
static int mtk_pcie_setup(struct mtk_gen3_pcie *pcie)
{
	int err, max_speed;

	/* [한국어] 장치 트리에서 레지스터 창, 리셋, PHY, 클록, 레인 수를 모두 얻는다 */
	err = mtk_pcie_parse_port(pcie);
	/* [한국어] 자원을 못 얻으면 -EPROBE_DEFER 일 수 있고, 그러면 나중에 다시 시도된다 */
	if (err)
		return err;

	/*
	 * Deassert the line in order to avoid unbalance in deassert_count
	 * counter since the bulk is shared.
	 */
	reset_control_bulk_deassert(pcie->soc->phy_resets.num_resets,
				    pcie->phy_resets);

	/* Don't touch the hardware registers before power up */
	err = pcie->soc->power_up(pcie);
	/* [한국어] 전원 인가 실패면 여기서 끝낸다 */
	if (err)
		return err;

	/* [한국어] 장치 트리의 max-link-speed 를 읽는다(drivers/pci/of.c:2174).
	 * 반환값을 err 에 담지만 오류 코드가 아니라 속도 값이라는 점에 주의 —
	 * 바로 아래에서 pcie_get_link_speed 로 유효성을 확인한다 */
	err = of_pci_get_max_link_speed(pcie->dev->of_node);
	if (pcie_get_link_speed(err) != PCI_SPEED_UNKNOWN) {
		/* Get the maximum speed supported by the controller */
		max_speed = mtk_pcie_get_controller_max_link_speed(pcie);

		/* Set max_link_speed only if the controller supports it */
		if (max_speed >= 0 && max_speed <= err) {
			/* [한국어] **DT 값이 하드웨어 상한 이하일 때만 채택한다.** 즉 이 설정은 속도를
			 * 낮추는 방향으로만 작용하며, 컨트롤러가 못 내는 속도를 요구하지 못한다 */
			pcie->max_link_speed = err;
			dev_info(pcie->dev,
				 "maximum controller link speed Gen%d, overriding to Gen%u",
				 max_speed, pcie->max_link_speed);
		}
	}

	/* Try link up */
	err = mtk_pcie_startup_port(pcie);
	/* [한국어] 링크 기동 실패면 전원을 되돌린다 */
	if (err)
		goto err_setup;

	return 0;

err_setup:
	mtk_pcie_power_down(pcie);

	return err;
}

/* [한국어]
 * mtk_pcie_probe - 플랫폼 드라이버 진입점
 *
 * @pdev: 장치 트리 매칭으로 만들어진 플랫폼 장치.
 * @return: 0 성공, 음수 오류(-EPROBE_DEFER 포함).
 *
 * 장치 트리에 mediatek,mt8192-pcie / mediatek,mt8196-pcie /
 * airoha,en7581-pcie 가 있으면 플랫폼 버스가 이 함수를 부른다.
 *
 *   1) devm_pci_alloc_host_bridge 로 브리지와 드라이버 상태를 한 덩어리로
 *      잡는다. 뒤에 붙는 private 영역이 struct mtk_gen3_pcie 이고,
 *      pci_host_bridge_priv 로 그 포인터를 얻는다. 반대 방향은
 *      pci_host_bridge_from_priv 이며 다른 함수들이 그것을 쓴다.
 *   2) device_get_match_data 로 SoC 별 pdata 를 고른다. 이 값이 이후 모든
 *      SoC 분기의 근거다 — 전원 콜백, 리셋 이름, 플래그가 여기서 나온다.
 *   3) **인터럽트를 먼저 준비한다.** 하드웨어를 켜기 전이다.
 *   4) 전원 컨트롤러 자식 장치를 만든다(drivers/pci/pwrctrl/core.c:426).
 *      슬롯 전원을 별도 드라이버가 관리하게 하는 계층이다.
 *   5) 하드웨어를 켜고 링크를 세운다(mtk_pcie_setup).
 *   6) pci_ops 와 sysdata 를 브리지에 꽂고 PCI 코어에 넘긴다. sysdata 가
 *      config 접근 경로에서 bus->sysdata 로 되돌아온다.
 *
 * 정리 경로가 라벨 셋으로 층져 있다. err_destroy_pwrctrl 에서
 * -EPROBE_DEFER 를 걸러 내는 것에 주의 — 나중에 다시 시도할 것이므로 만든
 * 자식 장치를 지우지 않고 그대로 둔다.
 *
**원문 코드의 특이점**: 4)의 오류 처리에서 goto 가 dev_err_probe 보다
 * 앞에 있어, 그 dev_err_probe 는 실행되지 않는다. 즉 pwrctrl 장치 생성이
 * 실패해도 그 메시지는 찍히지 않는다. 코드는 손대지 않고 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe_type 이
 * PROBE_PREFER_ASYNCHRONOUS 라 부팅 중 별도 스레드에서 돌 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수]
 *     → mtk_pcie_setup_irq, pci_pwrctrl_create_devices, mtk_pcie_setup,
 *       pci_host_probe
 */
static int mtk_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* [한국어] 브리지 뒤에 붙을 이 드라이버의 상태 */
	struct mtk_gen3_pcie *pcie;
	/* [한국어] PCI 코어에 넘길 브리지 */
	struct pci_host_bridge *host;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 브리지와 드라이버 상태를 한 덩어리로 잡는다. devm 이라 실패 경로나
	 * remove 에서 따로 놓을 필요가 없다 */
	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 할당 실패는 메모리 부족뿐이다 */
	if (!host)
		return -ENOMEM;

	/* [한국어] 브리지 뒤에 붙은 private 영역이 struct mtk_gen3_pcie 다.
	 * 두 구조체를 한 번에 할당해 수명을 묶는 커널 관례다 */
	pcie = pci_host_bridge_priv(host);

	/* [한국어] 이후 모든 로그와 자원 획득의 기준이 될 device 포인터 */
	pcie->dev = dev;
	/* [한국어] 장치 트리의 compatible 이 어느 항목과 맞았는지로 SoC 별 pdata 가 정해진다.
	 * 이 한 줄이 이후 모든 SoC 분기의 근거다 */
	pcie->soc = device_get_match_data(dev);
	/* [한국어] 플랫폼 장치에 드라이버 상태를 매단다. PM 콜백이 dev_get_drvdata 로
	 * 이 값을 되찾는다 */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] **하드웨어를 켜기 전에 인터럽트부터 준비한다.** 링크가 서는 순간
	 * 올라올 인터럽트를 놓치지 않기 위한 순서다 */
	err = mtk_pcie_setup_irq(pcie);
	/* [한국어] 인터럽트 준비 실패면 아직 되돌릴 것이 없으므로 바로 반환한다 */
	if (err)
		/* [한국어] platform_get_irq 가 -EPROBE_DEFER 를 줄 수 있고, dev_err_probe 는
		 * 그 경우 로그를 억제해 준다 */
		return dev_err_probe(dev, err, "Failed to setup IRQ domains\n");

	/* [한국어] 슬롯 전원을 관리할 자식 장치를 만든다
	 * (drivers/pci/pwrctrl/core.c:426). 실제 전원 인가는 나중에
	 * mtk_pcie_devices_power_up 이 한다 */
	err = pci_pwrctrl_create_devices(pcie->dev);
	/* [한국어] 실패하면 인터럽트 정리로 넘어간다 */
	if (err) {
		goto err_tear_down_irq;
		/* [한국어] **이 줄은 실행되지 않는다.** 바로 위의 goto 가 먼저 분기하기 때문이다.
		 * 원문 코드의 상태를 그대로 두고 사실만 적어 둔다 — 결과적으로 pwrctrl
		 * 장치 생성 실패 시 그 메시지는 찍히지 않는다 */
		dev_err_probe(dev, err, "failed to create pwrctrl devices\n");
	}

	/* [한국어] 여기서 처음 하드웨어를 켜고 링크를 세운다 */
	err = mtk_pcie_setup(pcie);
	/* [한국어] 하드웨어 기동 실패면 pwrctrl 자식 장치부터 되감는다 */
	if (err)
		goto err_destroy_pwrctrl;

	/* [한국어] config 접근 방법을 브리지에 꽂는다 */
	host->ops = &mtk_pcie_ops;
	/* [한국어] sysdata 로 컨트롤러 상태를 심는다. 이 값이 config 접근 경로에서
	 * bus->sysdata 로 되돌아와 mtk_pcie_config_tlp_header 등이 쓴다 */
	host->sysdata = pcie;

	/* [한국어] PCI 코어에 넘긴다. 이 호출 안에서 버스가 스캔되고 장치들이 발견되며,
	 * 그 과정에서 mtk_pcie_ops 의 config 접근이 처음 쓰인다 */
	err = pci_host_probe(host);
	/* [한국어] PCI 코어에 넘기는 데 실패하면 링크와 전원을 되감는다 */
	if (err)
		goto err_power_down_pcie;

	return 0;

err_power_down_pcie:
	mtk_pcie_devices_power_down(pcie);
	mtk_pcie_power_down(pcie);
err_destroy_pwrctrl:
	if (err != -EPROBE_DEFER)
		pci_pwrctrl_destroy_devices(pcie->dev);
err_tear_down_irq:
	mtk_pcie_irq_teardown(pcie);
	return err;
}

/* [한국어]
 * mtk_pcie_remove - 드라이버를 떼어 낸다
 *
 * @pdev: 제거되는 플랫폼 장치.
 * @return: 없음.
 *
 * probe 의 역순으로 걷어 낸다.
 *
 * 먼저 PCI 코어 쪽을 정리한다. pci_lock_rescan_remove 로 감싸는 것은
 * 이 시점에 다른 경로가 버스를 다시 훑거나 장치를 지우려 할 수 있기
 * 때문이다. 그 락이 재훑기와 제거를 직렬화한다.
 * pci_stop_root_bus 로 장치들의 동작을 멈춘 뒤 pci_remove_root_bus 로
 * 자료구조를 지운다 — 멈추기와 지우기가 두 단계로 나뉘어 있다.
 *
 * 그 다음 하드웨어를 내린다 — 슬롯 전원, 컨트롤러 전원, pwrctrl 자식
 * 장치, 인터럽트 순이다. 인터럽트를 마지막에 거두는 것이 중요하다.
 * 그 전까지는 아직 인터럽트가 올라올 수 있고, 도메인이 살아 있어야
 * 안전하게 처리된다.
 *
 * 브리지 자체는 devm 으로 잡았으므로 커널이 알아서 놓는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 버스(드라이버 언바인드) → [이 함수]
 *     → pci_stop_root_bus, pci_remove_root_bus,
 *       pci_pwrctrl_power_off_devices, mtk_pcie_power_down,
 *       pci_pwrctrl_destroy_devices, mtk_pcie_irq_teardown
 */
static void mtk_pcie_remove(struct platform_device *pdev)
{
	struct mtk_gen3_pcie *pcie = platform_get_drvdata(pdev);
	/* [한국어] private 영역 포인터에서 거꾸로 브리지를 찾는다.
	 * mtk_pcie_probe 의 pci_host_bridge_priv 와 짝을 이루는 연산이다 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);

	pci_lock_rescan_remove();
	pci_stop_root_bus(host->bus);
	pci_remove_root_bus(host->bus);
	pci_unlock_rescan_remove();

	pci_pwrctrl_power_off_devices(pcie->dev);
	mtk_pcie_power_down(pcie);
	pci_pwrctrl_destroy_devices(pcie->dev);
	mtk_pcie_irq_teardown(pcie);
}

/* [한국어]
 * mtk_pcie_irq_save - 절전 전에 인터럽트 마스크 상태를 기억해 둔다
 *
 * @pcie: 컨트롤러 상태. saved_irq_state 와 각 세트의 saved_irq_state 를 채운다.
 * @return: 없음.
 *
 * 절전에 들어가면 컨트롤러 전원이 끊겨 레지스터가 모두 초기화된다. 어떤
 * 인터럽트가 켜져 있었는지는 커널의 irq_desc 가 아니라 하드웨어 레지스터에
 * 있으므로, 그 값을 소프트웨어가 따로 보관해야 한다.
 *
 * 두 층을 저장한다.
 *   컨트롤러 전역 : PCIE_INT_ENABLE_REG — INTx 넷과 MSI 세트 여덟 개의
 *     활성화 비트가 함께 들어 있다.
 *   세트별       : 각 세트의 활성화 레지스터 — 그 안 벡터 32개의 비트.
 * 두 층을 모두 저장해야 복원이 완전해진다. 전역만 저장하면 어느 세트가
 * 켜져 있었는지는 알아도 그 안 어느 벡터가 켜져 있었는지를 잃는다.
 *
 * 락을 raw_spin_lock 으로 잡되 irqsave 를 쓰지 않는 것에 주의한다.
 * 이 함수는 suspend_noirq 단계에서만 불리고, 그때는 이미 인터럽트가
 * 꺼져 있어 저장·복원이 불필요하기 때문이다. 같은 락을 쓰는
 * mtk_intx_mask 계열이 irqsave 를 쓰는 것과 대비된다.
 *
 * 실행 컨텍스트: 시스템 절전의 noirq 단계(프로세스 컨텍스트, 인터럽트 꺼짐).
 *
 * 호출 체인:
 *   mtk_pcie_suspend_noirq → [이 함수]
 */
static void mtk_pcie_irq_save(struct mtk_gen3_pcie *pcie)
{
	int i;

	raw_spin_lock(&pcie->irq_lock);

	/* [한국어] 컨트롤러 전역 활성화 레지스터를 저장한다. INTx 넷과 MSI 세트 여덟 개의
	 * 비트가 여기 함께 들어 있다 */
	pcie->saved_irq_state = readl_relaxed(pcie->base + PCIE_INT_ENABLE_REG);

	/* [한국어] 세트 여덟 개를 모두 순회한다 */
	for (i = 0; i < PCIE_MSI_SET_NUM; i++) {
		/* [한국어] 세트 포인터를 꺼낸다 */
		struct mtk_msi_set *msi_set = &pcie->msi_sets[i];

		/* [한국어] 세트의 활성화 레지스터를 그대로 담는다. 32비트 각각이 그 세트 안
		 * 벡터 하나의 마스크 상태다 */
		msi_set->saved_irq_state = readl_relaxed(msi_set->base +
					   PCIE_MSI_SET_ENABLE_OFFSET);
	}

	raw_spin_unlock(&pcie->irq_lock);
}

/* [한국어]
 * mtk_pcie_irq_restore - 절전에서 깨어난 뒤 인터럽트 마스크를 되돌린다
 *
 * @pcie: 컨트롤러 상태. 저장해 둔 값을 레지스터로 되쓴다.
 * @return: 없음.
 *
 * mtk_pcie_irq_save 의 정확한 역동작이다. 저장할 때와 같은 두 층을 같은
 * 순서로 되쓴다.
 *
**부르는 시점이 중요하다.** mtk_pcie_resume_noirq 가 링크를 세운
**뒤에** 이 함수를 부른다. mtk_pcie_startup_port 가 도중에
 * mtk_pcie_enable_msi 를 불러 MSI 세트를 다시 깔고 INTx 를 모두 마스크하기
 * 때문에, 그보다 먼저 복원하면 그 설정에 덮여 사라진다.
 *
 * 읽고-고쳐-쓰기가 아니라 통째로 덮어쓰는 것에 주의 — 저장한 값이 그
 * 레지스터의 완전한 상태이므로 그대로 쓰면 된다.
 *
 * 락은 저장 쪽과 같은 이유로 irqsave 없이 잡는다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계(인터럽트 꺼짐).
 *
 * 호출 체인:
 *   mtk_pcie_resume_noirq → [이 함수]
 */
static void mtk_pcie_irq_restore(struct mtk_gen3_pcie *pcie)
{
	int i;

	raw_spin_lock(&pcie->irq_lock);

	/* [한국어] 컨트롤러 전역 인터럽트 활성화 상태를 통째로 되쓴다.
	 * 읽고-고쳐-쓰기가 아닌 이유는, 저장한 값이 이미 그 레지스터의 완전한
	 * 상태이기 때문이다 */
	writel_relaxed(pcie->saved_irq_state, pcie->base + PCIE_INT_ENABLE_REG);

	/* [한국어] 세트별 벡터 마스크도 되돌린다. 전역만 복원하면 어느 세트가 켜져
	 * 있었는지는 알아도 그 안 어느 벡터가 켜져 있었는지를 잃는다 */
	for (i = 0; i < PCIE_MSI_SET_NUM; i++) {
		/* [한국어] 세트 하나씩 순회한다 */
		struct mtk_msi_set *msi_set = &pcie->msi_sets[i];

		writel_relaxed(msi_set->saved_irq_state,
			       msi_set->base + PCIE_MSI_SET_ENABLE_OFFSET);
	}

	raw_spin_unlock(&pcie->irq_lock);
}

/* [한국어]
 * mtk_pcie_turn_off_link - 링크를 L2 로 내린다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 이면 L2 진입 성공. 시간 안에 못 들어가면 -ETIMEDOUT.
 *
 * 절전에 들어가기 전에 링크를 규격에 맞게 내리는 절차다. 전원을 그냥
 * 끊으면 상대 장치가 링크가 끊긴 것을 오류로 볼 수 있으므로, PM_Enter_L23
 * 흐름을 거쳐 양쪽이 합의한 상태로 만든다.
 *
 * PCIE_ICMD_PM_REG 의 PCIE_TURN_OFF_LINK 비트를 세우면 컨트롤러가 그
 * 흐름을 시작한다. 소프트웨어는 결과만 기다린다.
 *
 * 기다리는 방법이 LTSSM 상태 폴링이다. LTSSM 은 링크 훈련 상태 기계이고,
 * L2.idle(0x14)에 도달하면 링크가 내려간 것이다. 20us 간격으로 50ms 까지
 * 본다. 상대 장치가 응답하지 않으면 그 안에 못 들어가고 시간 초과가 난다.
 *
 * 같은 LTSSM 레지스터를 mtk_pcie_startup_port 도 읽는데, 그쪽은 실패
 * 진단용으로 한 번 읽어 이름을 찍고 여기서는 특정 상태를 기다린다.
 *
 * 실행 컨텍스트: 절전의 noirq 단계. readl_poll_timeout 이
 * 잠들지 않는 판(udelay 기반)으로 도는 자리다.
 *
 * 호출 체인:
 *   mtk_pcie_suspend_noirq → [이 함수]
 */
static int mtk_pcie_turn_off_link(struct mtk_gen3_pcie *pcie)
{
	u32 val;

	/* [한국어] 전원 관리 명령 레지스터를 읽는다. 다른 비트를 보존해야 하므로
	 * 읽고-고쳐-쓰기를 한다 */
	val = readl_relaxed(pcie->base + PCIE_ICMD_PM_REG);
	/* [한국어] 링크 끄기 비트를 세운다 */
	val |= PCIE_TURN_OFF_LINK;
	/* [한국어] 비트를 써 넣으면 컨트롤러가 PM_Enter_L23 흐름을 시작한다.
	 * 소프트웨어는 이후 LTSSM 상태로 결과만 확인한다 */
	writel_relaxed(val, pcie->base + PCIE_ICMD_PM_REG);

	/* Check the link is L2 */
	return readl_poll_timeout(pcie->base + PCIE_LTSSM_STATUS_REG, val,
				  (PCIE_LTSSM_STATE(val) ==
				   PCIE_LTSSM_STATE_L2_IDLE), 20,
				   50 * USEC_PER_MSEC);
}

/* [한국어]
 * mtk_pcie_suspend_noirq - 시스템 절전에 들어간다
 *
 * @dev: 이 컨트롤러의 device. drvdata 에 상태가 있다.
 * @return: 0 성공. L2 진입 실패면 음수를 올려 절전을 중단시킨다.
 *
 * noirq 단계에 붙는 이유는 두 가지다. 첫째, 이 시점에는 자식 장치들이 이미
 * 절전에 들어가 있어 링크를 내려도 안전하다. 둘째, 인터럽트가 꺼져 있어
 * 마스크 상태를 안전하게 읽어 둘 수 있다.
 *
 *   1) 링크를 L2 로 내린다. 실패하면 여기서 멈추고 오류를 올린다 —
 *      링크를 정리하지 못한 채 전원을 끊으면 복귀가 불확실해지므로,
 *      절전 자체를 포기하는 편이 낫다는 판단이다.
 *   2) 슬롯 장치의 전원을 끊는다.
 *   3) 인터럽트 마스크를 저장한다.
 *   4) 컨트롤러 전원을 내린다.
 *
 * 3)이 4)보다 먼저여야 한다 — 전원이 끊긴 뒤에는 레지스터를 읽을 수 없다.
 *
 * 실행 컨텍스트: 시스템 절전의 noirq 단계.
 *
 * 호출 체인:
 *   PM 코어 → mtk_pcie_pm_ops 의 suspend_noirq → [이 함수]
 *     → mtk_pcie_turn_off_link, mtk_pcie_devices_power_down,
 *       mtk_pcie_irq_save, mtk_pcie_power_down
 */
static int mtk_pcie_suspend_noirq(struct device *dev)
{
	struct mtk_gen3_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* Trigger link to L2 state */
	err = mtk_pcie_turn_off_link(pcie);
	/* [한국어] **L2 진입에 실패하면 절전 자체를 포기한다.** 링크를 정리하지 못한 채
	 * 전원을 끊으면 복귀가 불확실해지므로, 절전을 중단하는 편이 안전하다는
	 * 판단이다 */
	if (err) {
		/* [한국어] L2 에 못 들어갔다는 것은 상대 장치가 응답하지 않았다는 뜻이다 */
		dev_err(pcie->dev, "cannot enter L2 state\n");
		return err;
	}

	mtk_pcie_devices_power_down(pcie);
	/* [한국어] L2 진입 성공을 디버그 로그로 남긴다. 링크가 규격대로 내려갔음을
	 * 확인하는 흔적이다 */
	dev_dbg(pcie->dev, "entered L2 states successfully");

	mtk_pcie_irq_save(pcie);
	mtk_pcie_power_down(pcie);

	return 0;
}

/* [한국어]
 * mtk_pcie_resume_noirq - 절전에서 깨어난다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 성공, 음수 오류.
 *
 * 전원이 끊겼다 들어왔으므로 레지스터가 모두 초기값이다. 사실상 probe 의
 * 하드웨어 부분을 다시 하는 셈이다.
 *
 *   1) SoC 별 전원 시퀀스를 다시 돈다. probe 때와 같은 콜백이다.
 *   2) 링크를 다시 세운다. mtk_pcie_startup_port 안에서 RC 모드, 속도·폭
 *      제한, 클래스 코드, MSI 수신 주소, 변환 표가 모두 다시 설정된다 —
 *      그 함수가 probe 와 resume 양쪽에서 불리도록 만들어진 이유다.
 *   3) 인터럽트 마스크를 되돌린다. **반드시 2) 다음이어야 한다** —
 *      2)가 INTx 를 모두 마스크하고 MSI 를 다시 깔기 때문이다.
 *
 * 장치 트리 파싱(mtk_pcie_parse_port)은 다시 하지 않는다. 매핑된 주소와
 * 클록·리셋 핸들은 절전 중에도 유효하기 때문이다.
 *
 * 2)가 실패하면 전원을 되돌린다. 인터럽트 도메인은 그대로 둔다 — 드라이버가
 * 언바인드되는 것이 아니므로 도메인은 계속 유효하다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계.
 *
 * 호출 체인:
 *   PM 코어 → mtk_pcie_pm_ops 의 resume_noirq → [이 함수]
 *     → pcie->soc->power_up, mtk_pcie_startup_port, mtk_pcie_irq_restore
 */
static int mtk_pcie_resume_noirq(struct device *dev)
{
	struct mtk_gen3_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] SoC 별 전원 절차를 다시 탄다. probe 때와 같은 콜백이다 */
	err = pcie->soc->power_up(pcie);
	/* [한국어] 전원 인가 실패면 여기서 끝낸다. 되돌릴 것이 아직 없다 */
	if (err)
		return err;

	/* [한국어] 링크를 다시 세운다. 이 안에서 RC 모드, 속도·폭 제한, 클래스 코드,
	 * MSI 수신 주소, 변환 표가 모두 다시 설정된다 — 전원이 끊겨 레지스터가
	 * 초기화되었기 때문이다 */
	err = mtk_pcie_startup_port(pcie);
	/* [한국어] 링크 기동 실패면 전원을 되돌린다. 인터럽트 도메인은 건드리지 않는다 —
	 * 드라이버가 언바인드되는 것이 아니라 복귀에 실패한 것뿐이라, 도메인은
	 * 계속 유효하다 */
	if (err)
		goto err_power_down;

	mtk_pcie_irq_restore(pcie);

	return 0;

err_power_down:
	mtk_pcie_power_down(pcie);
	return err;
}

/* [한국어] 시스템 절전 콜백 등록. **noirq 단계** 를 쓰는 것이 요점이다 — 그때는
 * 자식 장치가 이미 잠들어 링크를 내려도 안전하고, 인터럽트가 꺼져 있어
 * 마스크 상태를 안전하게 저장할 수 있다 */
static const struct dev_pm_ops mtk_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(mtk_pcie_suspend_noirq,
				  mtk_pcie_resume_noirq)
};

/* [한국어] MT8192 용 pdata. 플래그도 클록 준비 시간도 없는 가장 단순한 구성이라,
 * 다른 두 SoC 가 무엇을 더 필요로 하는지 견주는 기준이 된다 */
static const struct mtk_gen3_pcie_pdata mtk_pcie_soc_mt8192 = {
	/* [한국어] MT8192 는 표준 전원 절차를 쓴다 */
	.power_up = mtk_pcie_power_up,
	.phy_resets = {
		/* [한국어] PHY 리셋 선 하나의 장치 트리 이름 */
		.id[0] = "phy",
		.num_resets = 1,
	},
};

/* [한국어] MT8196 용 pdata */
static const struct mtk_gen3_pcie_pdata mtk_pcie_soc_mt8196 = {
	/* [한국어] MT8196 도 표준 전원 절차를 쓴다. MT8192 와 유일하게 다른 점은 아래의
	 * sys_clk_rdy_time_us = 10 뿐이다 */
	.power_up = mtk_pcie_power_up,
	.phy_resets = {
		/* [한국어] MT8192 와 같은 리셋 이름 하나 */
		.id[0] = "phy",
		.num_resets = 1,
	},
	.sys_clk_rdy_time_us = 10,
};

/* [한국어] Airoha EN7581 용 pdata. 아래에 SKIP_PCIE_RSTB 플래그가 붙는다 */
static const struct mtk_gen3_pcie_pdata mtk_pcie_soc_en7581 = {
	/* [한국어] **EN7581 만 전원 콜백이 다르다.** PHY 초기화와 리셋 해제의 순서가
	 * 반대이고, PBus 설정과 이퀄라이저 프리셋이 추가로 필요하기 때문이다 */
	.power_up = mtk_pcie_en7581_power_up,
	.phy_resets = {
		/* [한국어] EN7581 은 레인마다 리셋 선을 따로 갖는다. 아래 두 줄에 lane1/lane2 가
		 * 이어지고 num_resets 가 3 이다 — MT8192 계열의 하나와 대비된다 */
		.id[0] = "phy-lane0",
		.id[1] = "phy-lane1",
		.id[2] = "phy-lane2",
		.num_resets = 3,
	},
	.flags = SKIP_PCIE_RSTB,
};

/* [한국어] 이 드라이버가 다루는 SoC 목록 */
static const struct of_device_id mtk_pcie_of_match[] = {
	/* [한국어] 장치 트리 compatible 과 pdata 의 대응. 이 표의 data 가
	 * device_get_match_data 를 통해 pcie->soc 로 들어가고, 이 파일의 모든
	 * SoC 분기가 그 포인터를 통과한다. **에어로하 EN7581 이 미디어텍 SoC 들과
	 * 같은 표에 있는 것** 이 이 IP 를 라이선스해 쓴다는 증거다 */
	{ .compatible = "airoha,en7581-pcie", .data = &mtk_pcie_soc_en7581 },
	{ .compatible = "mediatek,mt8192-pcie", .data = &mtk_pcie_soc_mt8192 },
	{ .compatible = "mediatek,mt8196-pcie", .data = &mtk_pcie_soc_mt8196 },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_pcie_of_match);

/* [한국어] 플랫폼 드라이버 정의. 이 컨트롤러는 PCI 장치가 아니라 플랫폼 장치다 —
 * 자기 자신이 PCI 버스를 만들어 주는 쪽이기 때문이다 */
static struct platform_driver mtk_pcie_driver = {
	/* [한국어] probe/remove 콜백 등록. 장치 트리 매칭이 성사되면 플랫폼 버스가 부른다 */
	.probe = mtk_pcie_probe,
	.remove = mtk_pcie_remove,
	.driver = {
		/* [한국어] 드라이버 이름. sysfs 와 로그에 이 이름으로 나타난다 */
		.name = "mtk-pcie-gen3",
		.of_match_table = mtk_pcie_of_match,
		.pm = &mtk_pcie_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_platform_driver(mtk_pcie_driver);
MODULE_DESCRIPTION("MediaTek Gen3 PCIe host controller driver");
MODULE_LICENSE("GPL v2");
