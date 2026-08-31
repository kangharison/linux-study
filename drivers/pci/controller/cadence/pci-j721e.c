// SPDX-License-Identifier: GPL-2.0
/*
 * pci-j721e - PCIe controller driver for TI's J721E SoCs
 *
 * Copyright (C) 2020 Texas Instruments Incorporated - http://www.ti.com
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

/*
 * [한국어 설명] TI J721E 계열 SoC 용 Cadence PCIe 글루(glue) 드라이버 (pci-j721e.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 PCIe 컨트롤러 IP 자체를 다루지 않는다. TI 가 Cadence 로부터
 * 라이선스한 PCIe 컨트롤러 IP 를 J721E/J7200/AM64/J784S4/J722S 같은 자사 SoC 에
 * 집어넣으면서 그 주변에 덧붙인 "글루 로직" 만을 담당한다. 구체적으로는
 * (1) CTRL_MMR 이라는 SoC 시스템 레지스터 공간에 RC/EP 모드·링크 속도·레인 수
 * 같은 "스트랩(strap)" 값을 써 넣고, (2) TI 고유의 user_cfg 레지스터로 링크
 * 트레이닝을 켜고 끄고 상태를 읽으며, (3) TI 고유의 intd_cfg(인터럽트 취합기)
 * 레지스터로 링크 다운 인터럽트를 켜고 지운다. 표준적인 아웃바운드/인바운드
 * 주소 변환, config space 접근, BAR 배정 같은 IP 공통 동작은 모두
 * pcie-cadence*.c 쪽 공통 코어에 위임한다.
 * 하나의 드라이버가 호스트(Root Complex)와 엔드포인트(Endpoint) 두 역할을
 * 모두 지원하며, 어느 쪽으로 동작할지는 devicetree 의 compatible 문자열
 * ("ti,...-pcie-host" 대 "ti,...-pcie-ep")로 결정된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 커널 모듈이며, 진입점은 platform_driver 의 probe 콜백이다.
 * 부팅 시 흐름은 다음과 같다.
 *   devicetree 의 compatible 매칭
 *     -> j721e_pcie_probe()               [이 파일] 모드 선택, 자원 확보, 스트랩 설정
 *        -> j721e_pcie_ctrl_init()        [이 파일] CTRL_MMR 스트랩 프로그래밍
 *        -> cdns_pcie_init_phy()          [pcie-cadence.c] PHY 확보 + 켜기
 *        -> cdns_pcie_host_setup()        [pcie-cadence-host.c]   RC 모드
 *           또는 cdns_pcie_ep_setup()     [pcie-cadence-ep.c]     EP 모드
 * 반대로 공통 코어는 링크를 다룰 때마다 이 파일로 되돌아 내려온다. 공통 코어의
 * cdns_pcie_start_link()/stop_link()/cdns_pcie_link_up() 은 pcie-cadence.h 에
 * 있는 static inline 디스패처인데, 그것들이 부르는 pcie->ops 가 바로 이 파일의
 * j721e_pcie_ops 다. 즉 "언제 링크를 올릴지" 는 공통 코어가 정하고 "어떻게
 * 올릴지" 는 이 파일이 안다.
 * 이 트리에서 ops->link_up 을 실제로 채우는 SoC 드라이버는 이 파일의
 * j721e_pcie_link_up() 과 pci-sky1.c 의 sky1_pcie_link_up() 둘뿐이다. 다만
 * 디스패처 cdns_pcie_link_up() 을 실제로 넘기는 곳은 구형(LGA) 호스트 경로인
 * pcie-cadence-host.c:1183 하나이고, 신형(HPA) 경로는 cdns_pcie_hpa_link_up()
 * 을 직접 넘기므로, 결과적으로 디스패처를 통해 불리는 것은 이 파일의
 * j721e_pcie_link_up() 쪽이다.
 *
 * === 타 모듈과의 연결 ===
 * 아래로는 Cadence 공통 코어 세 덩어리에 의존한다 — pcie-cadence.c(PHY 관리,
 * 아웃바운드 창), pcie-cadence-host.c 및 pcie-cadence-host-common.c(RC 초기화,
 * 링크 대기·재트레이닝, config 접근), pcie-cadence-ep.c(EP 초기화). 이들은
 * CONFIG_PCIE_CADENCE_HOST / CONFIG_PCIE_CADENCE_EP 로 나뉘어 빌드되며,
 * Kconfig 의 PCI_J721E 가 PCI_J721E_HOST/EP 설정에 따라 그것들을 select 한다.
 * 옆으로는 SoC 인프라에 의존한다 — regmap/syscon 으로 CTRL_MMR 을 두드리고,
 * GENPD(전원 도메인)를 움직이기 위해 runtime PM 을 쓰고, PERST# 을 흔들기 위해
 * gpiod 를, 레퍼런스 클럭을 위해 clk 프레임워크를 쓴다.
 * 데이터 흐름 관점에서, 이 파일이 만들어 내는 "산출물" 은 결국 struct cdns_pcie
 * 한 개다. probe 가 그 안에 dev, ops 를 채워 공통 코어에 넘기면, 공통 코어가
 * 나머지(reg_base, mem_res, phy 배열 등)를 채우고 PCI 버스를 열거한다. 이후
 * 열거된 NVMe 등 엔드포인트 드라이버는 이 파일을 코드로 부르지 않는다 —
 * 관계는 "이 컨트롤러 아래에 그 장치가 매달린다" 는 토폴로지상의 것이지,
 * 함수 호출 관계가 아니다(drivers/nvme 에서 cdns_ 심볼 호출은 0건).
 * 반대로 이 파일이 공통 코어와 공유하는 상태는 struct cdns_pcie(그리고 그것을
 * 품은 cdns_pcie_rc / cdns_pcie_ep)이며, quirk 플래그들도 그 구조체를 통해
 * 전달된다.
 *
 * === 주요 함수/구조체 요약 ===
 * j721e_pcie_probe()          : 진입점. compatible 로 SoC 별 데이터를 고르고,
 *                               RC/EP 를 갈라 브리지 또는 ep 구조체를 만들고,
 *                               스트랩 설정 -> PHY -> refclk -> PERST# 해제 ->
 *                               공통 코어 setup 순서로 진행한다.
 * j721e_pcie_ctrl_init()      : CTRL_MMR 스트랩(모드/속도/레인)을 쓴다. 스트랩은
 *                               전원 인가 시점에 래치되므로 컨트롤러를 껐다가
 *                               쓴 뒤 다시 켜는 순서가 필수다.
 * j721e_pcie_ops              : 공통 코어가 링크를 제어할 때 되부르는 콜백 3종
 *                               (start_link / stop_link / link_up).
 * j721e_pcie_link_irq_handler(): 링크 다운 인터럽트를 받아 로그를 남기고 상태를
 *                               지운다. 복구를 시도하지는 않는다.
 * j721e_pcie_resume_noirq()   : 절전 복귀. 스트랩이 날아갔으므로 ctrl_init 부터
 *                               다시 하고, PHY/refclk/PERST# 를 되살린 뒤
 *                               인바운드 BAR 가용 표시를 초기화해 재초기화시킨다.
 * struct j721e_pcie           : 인스턴스별 상태(글루 레지스터 두 창, refclk,
 *                               reset GPIO, 레인 수, 링크다운 IRQ 비트).
 * struct j721e_pcie_data      : compatible 문자열마다 다른 SoC 고정 특성 표
 *                               (모드, quirk 3종, 링크다운 비트, 최대 레인 수,
 *                               config space 바이트 접근 허용 여부).
 */

/* [한국어] clk_prepare_enable(), clk_disable_unprepare(), devm_clk_get_optional_enabled()
 * — 슬롯에 내보내는 PCIe 레퍼런스 클럭(pcie->refclk)을 다루기 위해. */
#include <linux/clk.h>
/* [한국어] 클럭 '공급자(provider)' 쪽 API 헤더. 이 파일은 클럭을 만들지 않고 쓰기만
 * 하므로 여기서 직접 참조하는 심볼을 찾지 못했다 — 상류에 남아 있는
 * 포함으로 보이며, 확실한 근거를 이 트리에서 확인하지 못했다. */
#include <linux/clk-provider.h>
/* [한국어] container_of() 매크로. 아래 cdns_pcie_to_rc 매크로와 j721e_pcie_remove()
 * 가 struct cdns_pcie 포인터에서 그것을 품은 cdns_pcie_rc / cdns_pcie_ep 로
 * 거슬러 올라갈 때 쓴다. */
#include <linux/container_of.h>
/* [한국어] msleep(). PERST# 해제 전 100ms(PCIE_T_PVPERL_MS)를 기다리는 데 필요하다. */
#include <linux/delay.h>
/* [한국어] gpiod_set_value_cansleep() 등 GPIO '소비자' API. PERST# 리셋 신호를
 * 흔드는 데 쓴다. cansleep 판을 쓰는 이유는 I2C 확장기 뒤에 달린 GPIO 도
 * 지원하기 위해서다. */
#include <linux/gpio/consumer.h>
/* [한국어] readl()/writel() MMIO 접근자. TI 글루 레지스터 두 창을 읽고 쓰는
 * j721e_pcie_user_readl 계열 래퍼의 기반이다. */
#include <linux/io.h>
/* [한국어] 연쇄(chained) 인터럽트 컨트롤러용 헬퍼(chained_irq_enter 등).
 * 이 파일은 devm_request_irq 로 일반 핸들러만 등록하고 연쇄 IRQ 를 만들지
 * 않으므로 직접 쓰는 심볼을 찾지 못했다 — 상류에 남아 있는 포함이다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain API 헤더. 이 파일은 자체 irq_domain 을 만들지 않아 직접 쓰는
 * 심볼을 찾지 못했다 — 위와 같이 상류에 남아 있는 포함으로 보인다. */
#include <linux/irqdomain.h>
/* [한국어] syscon_regmap_lookup_by_phandle() 와 그 _optional 판. DT phandle 로
 * CTRL_MMR 및 ACSPCIE 시스템 레지스터 공간의 regmap 을 얻는 데 쓴다. */
#include <linux/mfd/syscon.h>
/* [한국어] MODULE_LICENSE/DESCRIPTION/AUTHOR/DEVICE_TABLE 과 module_platform_driver
 * 매크로. 이 드라이버는 tristate 라 모듈로 빌드될 수 있다. */
#include <linux/module.h>
/* [한국어] of_property_read_u32(), of_device_get_match_data(),
 * of_parse_phandle_with_fixed_args(), struct device_node — devicetree 에서
 * 설정을 읽어 오는 모든 경로가 이 헤더에 의존한다. */
#include <linux/of.h>
/* [한국어] struct pci_bus, struct pci_host_bridge, struct pci_ops,
 * pci_is_root_bus(), pci_generic_config_read32 계열, pci_host_bridge_priv(),
 * devm_pci_alloc_host_bridge() — PCI 코어와 맞물리는 부분 전부. */
#include <linux/pci.h>
/* [한국어] struct platform_device, platform_get_irq_byname(),
 * devm_platform_ioremap_resource_byname(), module_platform_driver().
 * 이 드라이버는 DT 로 열거되는 platform 드라이버다. */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_enable/get_sync/put/put_sync/disable.
 * GENPD(전원 도메인)를 켜고 끄는 유일한 수단이며, CTRL_MMR 스트랩을
 * 래치시키기 위해 컨트롤러 전원을 껐다 켜는 데 반드시 필요하다. */
#include <linux/pm_runtime.h>
/* [한국어] struct regmap 과 regmap_update_bits(). CTRL_MMR 처럼 여러 드라이버가
 * 공유하는 레지스터 공간은 직접 ioremap 하지 않고 regmap 을 통해 락 아래에서
 * read-modify-write 해야 한다. */
#include <linux/regmap.h>

/* [한국어] PCI 서브시스템 내부 전용 헤더(drivers/pci/pci.h). 외부에 공개되지 않는
 * 세 가지를 여기서 가져온다 — PCIE_T_PVPERL_MS(100ms 상수),
 * pcie_get_link_speed()(세대 번호를 속도 enum 으로), 그리고
 * of_pci_get_max_link_speed()(DT max-link-speed 읽기). */
#include "../../pci.h"
/* [한국어] Cadence 공통 코어의 인터페이스. struct cdns_pcie / cdns_pcie_rc /
 * cdns_pcie_ep / cdns_pcie_ops, 링크 제어 디스패처(cdns_pcie_start_link 등),
 * PHY 관리(cdns_pcie_init_phy/enable_phy/disable_phy), 그리고
 * cdns_pcie_host_setup()/cdns_pcie_ep_setup() 선언이 모두 여기 있다. */
#include "pcie-cadence.h"

/* [한국어] struct cdns_pcie 포인터를 그것을 첫 멤버로 품은 cdns_pcie_rc 로 되돌린다.
 * j721e_pcie_resume_noirq() 가 RC 전용 상태(avail_ib_bar 등)에 접근할 때 쓴다.
 * j721e_pcie_remove() 는 EP 도 다뤄야 해서 이 매크로 대신 container_of 를
 * 직접 쓴다. */
#define cdns_pcie_to_rc(p) container_of(p, struct cdns_pcie_rc, pcie)

/* [한국어] INTD 취합기의 이벤트 enable 레지스터(SYS_2 그룹) 오프셋 0x108.
 * 여기에 1 을 쓴 비트가 인터럽트를 낼 수 있게 된다. */
#define ENABLE_REG_SYS_2	0x108
/* [한국어] 같은 그룹의 enable clear 레지스터 오프셋 0x308. 여기에 1 을 쓴 비트가
 * enable 에서 지워진다. set/clear 가 분리되어 있어 다른 비트를 뭉갤 위험 없이
 * 원자적으로 끌 수 있다. */
#define ENABLE_CLR_REG_SYS_2	0x308
/* [한국어] 같은 그룹의 status 레지스터 오프셋 0x508. 어떤 이벤트가 실제로
 * 발생했는지를 읽는다. IRQ 핸들러가 자기 이벤트인지 판별하는 데 쓴다. */
#define STATUS_REG_SYS_2	0x508
/* [한국어] 같은 그룹의 status clear 레지스터 오프셋 0x708. 여기에 1 을 쓰면 해당
 * status 비트가 지워진다(write-1-to-clear). 핸들러가 이것을 하지 않으면
 * 레벨 트리거된 선이 계속 걸려 인터럽트 폭풍이 난다. */
#define STATUS_CLR_REG_SYS_2	0x708
/* [한국어] J721E 에서 링크 다운 이벤트가 놓인 비트 — SYS_2 그룹의 비트 1.
 * j721e_pcie_rc_data / j721e_pcie_ep_data 만 이 값을 쓴다. */
#define LINK_DOWN		BIT(1)
/* [한국어] J7200 이후 SoC(J7200/AM64/J784S4/J722S)에서 링크 다운이 옮겨 간 비트 10.
 * 취합기 배치가 실리콘 세대마다 달라 상수를 못 박지 못하고 특성표로 뺀 이유다. */
#define J7200_LINK_DOWN		BIT(10)

/* [한국어] TI 글루 user_cfg 창의 명령/상태 레지스터 오프셋 0x4.
 * 링크 트레이닝을 켜고 끄는 비트가 여기 있다. */
#define J721E_PCIE_USER_CMD_STATUS	0x4
/* [한국어] 위 레지스터의 비트 0. 세우면 LTSSM 이 Detect 에서 풀려 트레이닝을
 * 시작하고, 지우면 트레이닝을 멈춰 링크가 내려간다. */
#define LINK_TRAINING_ENABLE		BIT(0)

/* [한국어] TI 글루 user_cfg 창의 링크 상태 레지스터 오프셋 0x14. */
#define J721E_PCIE_USER_LINKSTATUS	0x14
/* [한국어] 위 레지스터에서 링크 상태를 담은 비트 1:0. 이 2비트 값을 enum link_status
 * 와 비교해 링크 완료를 판정한다. GENMASK(1, 0) 은 비트 1 부터 0 까지를
 * 1 로 채운 마스크 0x3 이다. */
#define LINK_STATUS			GENMASK(1, 0)

/* [한국어]
 * enum link_status - J721E_PCIE_USER_LINKSTATUS 의 LINK_STATUS(비트 1:0) 값
 *
 * TI 글루가 LTSSM(Link Training and Status State Machine) 진행 정도를 2비트로
 * 요약해 놓은 것이다. j721e_pcie_link_up() 이 이 중 LINK_UP_DL_COMPLETED 인지만
 * 검사한다. 열거 상수에 값을 명시하지 않았으므로 0,1,2,3 이 차례로 붙는다.
 */
enum link_status {
	NO_RECEIVERS_DETECTED,
	/* [한국어] 값 0. 아직 링크 파트너(수신기)가 감지되지 않은 상태.
	 * 설정자: 하드웨어(LTSSM). 읽는 자: j721e_pcie_link_up().
	 * 슬롯이 비었거나 PERST# 이 아직 해제되지 않았을 때 이 값이 보인다. */
	LINK_TRAINING_IN_PROGRESS,
	/* [한국어] 값 1. 수신기는 감지됐고 물리 계층 트레이닝이 진행 중인 상태.
	 * 설정자: 하드웨어. 읽는 자: j721e_pcie_link_up().
	 * 이 상태에서는 아직 config 접근을 하면 안 된다. */

	LINK_UP_DL_IN_PROGRESS,
	/* [한국어] 값 2. 물리 계층은 올라왔으나 DL(Data Link) 계층 초기화가
	 * 진행 중인 상태.
	 * 설정자: 하드웨어. 읽는 자: j721e_pcie_link_up().
	 * "링크가 있다" 고 보기에는 이르므로 link_up 은 여기서도 false 를 준다. */

	LINK_UP_DL_COMPLETED,
	/* [한국어] 값 3. DL 계층까지 초기화가 끝나 TLP 를 주고받을 수 있는 상태.
	 * 설정자: 하드웨어. 읽는 자: j721e_pcie_link_up() — 오직 이 값일 때만
	 * true 를 돌려준다. 값 범위: 이 enum 은 2비트 필드이므로 0~3 이 전부다. */
};

/* [한국어] CTRL_MMR 의 PCIEn_CTRL 레지스터에서 '이 인스턴스는 Root Complex 다' 를
 * 뜻하는 스트랩 비트 7. 지우면 Endpoint 로 동작한다.
 * 전원 인가 시점에 한 번 래치되므로 컨트롤러가 꺼진 동안 써야 한다. */
#define J721E_MODE_RC			BIT(7)
/* [한국어] 레인 수를 같은 레지스터의 비트 8 부터 시작하는 필드에 넣기 위한
 * 시프트 매크로. 필드가 0-based 라 호출자는 (실제 레인 수 - 1) 을 넘긴다. */
#define LANE_COUNT(n)			((n) << 8)

/* [한국어] ACSPCIE 제어 레지스터에서 refclk 출력 패드의 'disable' 비트 1:0.
 * 1 이 '패드 끔' 이므로, 켜려면 이 비트들을 0 으로 만들어야 한다. */
#define ACSPCIE_PAD_DISABLE_MASK	GENMASK(1, 0)
/* [한국어] PCIEn_CTRL 에서 최대 링크 속도(세대)를 담는 비트 1:0.
 * 0=Gen1, 1=Gen2, 2=Gen3, 3=Gen4 로 0-based 인코딩이라
 * j721e_pcie_set_link_speed() 가 (세대 번호 - 1) 을 써 넣는다. */
#define GENERATION_SEL_MASK		GENMASK(1, 0)

/* [한국어]
 * struct j721e_pcie - 이 드라이버 인스턴스 하나의 런타임 상태
 *
 * probe 에서 devm_kzalloc 으로 할당해 dev_set_drvdata() 로 device 에 매단다.
 * 그래서 ops 콜백들(j721e_pcie_start_link 등)은 cdns_pcie->dev 만 들고도
 * dev_get_drvdata() 로 이 구조체를 되찾을 수 있다. 이 구조체는 Cadence 공통
 * 코어가 아는 struct cdns_pcie 와 별개이며, "TI 글루만 아는 것" 을 담는다.
 */
struct j721e_pcie {
	struct cdns_pcie	*cdns_pcie;
	/* [한국어] Cadence 공통 코어가 다루는 컨트롤러 구조체를 가리킨다.
	 * RC 모드면 cdns_pcie_rc 안의 .pcie 를, EP 모드면 cdns_pcie_ep 안의
	 * .pcie 를 가리키므로, 되돌아갈 때는 container_of 가 필요하다
	 * (이 파일 상단의 cdns_pcie_to_rc 매크로, 그리고 remove 의 container_of).
	 * 설정자: j721e_pcie_probe() 의 모드 분기.
	 * 읽는 자: 이 파일의 거의 모든 함수 — dev 를 얻거나 공통 코어를 부를 때.
	 * 값 범위: probe 성공 후에는 항상 유효한 포인터(NULL 불가).
	 * 동기화: probe/remove/PM 콜백에서만 다루므로 별도 락이 없다. */

	struct clk		*refclk;
	/* [한국어] 슬롯의 엔드포인트에게 공급하는 PCIe 레퍼런스 클럭.
	 * 설정자: probe 의 RC 분기에서 devm_clk_get_optional_enabled() —
	 * 이름 그대로 "얻는 즉시 켜고", devres 가 해제 시 끈다.
	 * 읽는 자: suspend 에서 clk_disable_unprepare(), resume 에서
	 * clk_prepare_enable() — 절전 구간에는 클럭을 죽인다.
	 * 값 범위: 옵셔널이므로 DT 에 없으면 NULL 이고, clk API 는 NULL 을
	 * 조용히 무시하므로 별도 검사가 필요 없다. EP 모드에서는 쓰지 않는다.
	 * 동기화: PM 콜백은 직렬화되므로 락 불필요. */

	u32			mode;
	/* [한국어] 이 인스턴스가 RC 로 동작하는지 EP 로 동작하는지.
	 * 설정자: probe 가 compatible 로 고른 j721e_pcie_data.mode 를 복사.
	 * 읽는 자: remove(), suspend_noirq(), resume_noirq() — RC 에만 필요한
	 * refclk/PERST# 처리를 걸러 내는 데 쓴다.
	 * 값 범위: PCI_MODE_RC(0) 또는 PCI_MODE_EP(1).
	 * 동기화: probe 이후 불변이므로 락 불필요. */

	u32			num_lanes;
	/* [한국어] 실제로 쓸 레인 수. DT 의 "num-lanes" 속성에서 온다.
	 * 설정자: probe — 속성이 없거나 max_lanes 를 넘으면 1 로 낮춘다.
	 * 읽는 자: j721e_pcie_set_lane_count() 가 CTRL_MMR 스트랩에 (n-1) 로
	 * 인코딩해 써 넣는다.
	 * 값 범위: 1 이상 max_lanes 이하.
	 * 동기화: probe 이후 불변. */

	u32			max_lanes;
	/* [한국어] 이 SoC 의 컨트롤러가 물리적으로 지원하는 최대 레인 수.
	 * 설정자: probe 가 j721e_pcie_data.max_lanes 를 복사.
	 * 읽는 자: probe 의 범위 검사와 j721e_pcie_set_lane_count() 의 마스크
	 * 폭 결정(4레인이면 스트랩 필드가 2비트로 넓어진다).
	 * 값 범위: 이 파일의 데이터 표 기준 1, 2, 4.
	 * 동기화: probe 이후 불변. */

	struct gpio_desc	*reset_gpio;
	/* [한국어] 슬롯의 PERST#(fundamental reset) 신호를 흔드는 GPIO.
	 * 신호 자체는 active-low 지만 gpiod API 는 DT 의 극성 플래그를 반영한
	 * "논리값" 을 쓰므로, 1 이 "리셋 해제", 0 이 "리셋 유지" 다.
	 * 설정자: probe 의 RC 분기 devm_gpiod_get_optional(..., GPIOD_OUT_LOW)
	 * — 즉 확보하는 순간 리셋을 걸어 둔 상태로 시작한다.
	 * 읽는 자: probe/resume 이 100ms 뒤 1 로 올려 해제하고, remove/suspend 가
	 * 0 으로 내려 다시 건다.
	 * 값 범위: 옵셔널이라 NULL 가능. gpiod_set_value_cansleep() 은 NULL 을
	 * 무시하므로 remove 에서 검사 없이 부르는 것이 안전하다.
	 * 동기화: probe/remove/PM 에서만 접근하므로 락 불필요. */

	void __iomem		*user_cfg_base;
	/* [한국어] TI 가 Cadence IP 바깥에 덧붙인 "user_cfg" 레지스터 창의
	 * 가상 주소. 링크 트레이닝 enable 비트와 링크 상태 필드가 여기 있다.
	 * 설정자: probe 의 devm_platform_ioremap_resource_byname(pdev, "user_cfg").
	 * 읽는 자: j721e_pcie_user_readl/writel 래퍼 — 즉 start/stop/link_up 콜백.
	 * 값 범위: ioremap 된 유효 주소. __iomem 이므로 readl/writel 로만 접근.
	 * 동기화: read-modify-write 를 하지만 락이 없다. 이 레지스터를 만지는
	 * 경로가 probe/PM/공통 코어의 링크 제어뿐이고 서로 직렬화되어 있기
	 * 때문이다(이 트리에서 확인한 범위). */

	void __iomem		*intd_cfg_base;
	/* [한국어] TI 의 인터럽트 취합기("INTD") 레지스터 창의 가상 주소.
	 * 링크 다운 같은 시스템 이벤트의 enable/status 비트가 여기 모여 있다.
	 * 설정자: probe 의 devm_platform_ioremap_resource_byname(pdev, "intd_cfg").
	 * 읽는 자: j721e_pcie_intd_readl/writel 래퍼 — IRQ 핸들러와
	 * config_link_irq()/disable_link_irq().
	 * 값 범위: ioremap 된 유효 주소.
	 * 동기화: IRQ 핸들러(인터럽트 컨텍스트)와 config/disable(프로세스
	 * 컨텍스트)이 같은 창을 만지지만, 핸들러는 STATUS 계열만, 나머지는
	 * ENABLE 계열만 건드려 레지스터가 갈린다. */

	u32			linkdown_irq_regfield;
	/* [한국어] 이 SoC 에서 "링크 다운" 이벤트가 배정된 비트 마스크.
	 * SoC 마다 취합기 안의 비트 위치가 달라서 상수로 못 박지 못하고
	 * compatible 별 데이터에서 가져온다 — J721E 는 LINK_DOWN(비트 1),
	 * J7200 이후 SoC 들은 J7200_LINK_DOWN(비트 10).
	 * 설정자: probe 가 j721e_pcie_data.linkdown_irq_regfield 를 복사.
	 * 읽는 자: 링크 IRQ 핸들러(내 이벤트인지 판별 + status clear),
	 * config_link_irq()(enable), disable_link_irq()(enable clear).
	 * 값 범위: 32비트 중 한 비트.
	 * 동기화: probe 이후 불변. */
};

/* [한국어]
 * enum j721e_pcie_mode - 컨트롤러가 어느 쪽으로 동작할지
 *
 * 같은 실리콘이 Root Complex(호스트) 로도, Endpoint(장치) 로도 쓰일 수 있어서
 * DT compatible 이 둘 중 하나를 고르고, 그 값이 CTRL_MMR 스트랩의 모드 비트와
 * probe 의 분기를 동시에 결정한다.
 */
enum j721e_pcie_mode {
	PCI_MODE_RC,
	/* [한국어] 값 0. Root Complex — 우리가 버스를 열거하고 아래 장치를 찾는다.
	 * 설정자: "ti,...-pcie-host" compatible 에 붙은 데이터.
	 * 읽는 자: j721e_pcie_set_mode() 가 이때만 J721E_MODE_RC 비트를 세우고,
	 * probe/remove/PM 이 refclk·PERST# 처리를 이 값으로 가른다. */

	PCI_MODE_EP,
	/* [한국어] 값 1. Endpoint — 우리가 남의 버스에 매달리는 장치가 된다.
	 * 설정자: "ti,...-pcie-ep" compatible 에 붙은 데이터.
	 * 읽는 자: 위와 같음. 이 경우 refclk 와 PERST# 는 상대편이 공급하므로
	 * 이 드라이버가 건드리지 않는다.
	 * 값 범위: 이 enum 은 두 값이 전부다. */
};

/* [한국어]
 * struct j721e_pcie_data - compatible 문자열 하나에 대응하는 SoC 고정 특성표
 *
 * 이 드라이버가 지원하는 SoC 는 다섯 종(J721E, J7200, AM64, J784S4, J722S)이고,
 * 각각 RC/EP 변형이 있어 아래에 const 인스턴스가 아홉 개 정의되어 있다.
 * of_device_id.data 로 매달려 있다가 probe 초입의 of_device_get_match_data()
 * 로 뽑혀, 그 값들이 j721e_pcie 와 cdns_pcie_rc/ep 로 흩뿌려진다.
 * 모두 const 정적 데이터이므로 읽기 전용이고 동기화가 필요 없다.
 */
struct j721e_pcie_data {
	enum j721e_pcie_mode	mode;
	/* [한국어] 이 compatible 이 RC 인지 EP 인지.
	 * 설정자: 아래 정적 초기화. 읽는 자: probe 의 첫 switch 와
	 * pcie->mode 복사본.
	 * 값 범위: PCI_MODE_RC / PCI_MODE_EP. */

	unsigned int		quirk_retrain_flag:1;
	/* [한국어] Gen2 트레이닝 결함 우회 — 링크가 올라온 뒤 한 번 더
	 * 재트레이닝을 걸어야 원하는 속도로 붙는 IP 판본에 세운다.
	 * 설정자: 아래 정적 초기화(J721E RC 와 J784S4 RC 만 true).
	 * 읽는 자: probe 가 rc->quirk_retrain_flag 로 복사하고, 실제 사용은
	 * pcie-cadence-host-common.c 의 cdns_pcie_host_start_link() 가
	 * 링크 대기 성공 후 cdns_pcie_retrain() 을 부를지 결정할 때.
	 * 값 범위: 0 또는 1. */

	unsigned int		quirk_detect_quiet_flag:1;
	/* [한국어] LTSSM 의 Detect.Quiet 최소 대기 시간이 기본값으로는 너무
	 * 짧은 IP 판본을 위한 우회. 세우면 그 지연을 늘려 준다.
	 * 설정자: 아래 정적 초기화(J7200 RC/EP 만 true).
	 * 읽는 자: probe 가 rc->quirk_detect_quiet_flag 또는
	 * ep->quirk_detect_quiet_flag 로 복사하고, 실제 사용은 공통 코어의
	 * cdns_pcie_host_link_setup()/EP 초기화가
	 * cdns_pcie_detect_quiet_min_delay_set() 을 부를지 결정할 때.
	 * 값 범위: 0 또는 1. */

	unsigned int		quirk_disable_flr:1;
	/* [한국어] EP 모드 전용. FLR(Function Level Reset) 동작에 결함이 있어
	 * 호스트가 FLR 을 걸면 복구되지 않는 판본에서, 각 함수의
	 * PCI_EXP_DEVCAP 에서 FLR Capable 비트를 지워 능력 자체를 감춘다.
	 * 설정자: 아래 정적 초기화(J7200 EP 만 true).
	 * 읽는 자: probe 가 ep->quirk_disable_flr 로 복사하고, 실제 사용은
	 * pcie-cadence-ep.c 의 EP 초기화 경로.
	 * 값 범위: 0 또는 1. RC 데이터에서는 늘 0. */

	u32			linkdown_irq_regfield;
	/* [한국어] INTD 취합기에서 이 SoC 의 링크 다운 이벤트가 놓인 비트.
	 * 설정자: 아래 정적 초기화 — J721E 는 LINK_DOWN, 그 뒤 SoC 들은
	 * J7200_LINK_DOWN.
	 * 읽는 자: probe 가 pcie->linkdown_irq_regfield 로 복사.
	 * 값 범위: 32비트 중 한 비트. */

	unsigned int		byte_access_allowed:1;
	/* [한국어] 루트 포트 자신의 config space 를 바이트/워드 단위로 읽고 쓸 수
	 * 있는지. 0 이면 dword 단위로만 접근해야 하므로 probe 가
	 * bridge->ops 를 cdns_ti_pcie_host_ops 로 갈아 끼워 루트 버스 접근만
	 * pci_generic_config_read32 계열로 우회시킨다.
	 * 설정자: 아래 정적 초기화(J721E RC 와 J784S4 RC 는 false, J7200/AM64/
	 * J722S RC 는 true).
	 * 읽는 자: probe 의 RC 분기.
	 * 값 범위: 0 또는 1. EP 데이터에서는 의미가 없어 초기화하지 않는다. */

	unsigned int		max_lanes;
	/* [한국어] 이 SoC 컨트롤러가 지원하는 최대 레인 수.
	 * 설정자: 아래 정적 초기화(AM64/J722S 는 1, J721E/J7200 은 2,
	 * J784S4 는 4).
	 * 읽는 자: probe 의 num-lanes 범위 검사와 pcie->max_lanes 복사본,
	 * 그리고 j721e_pcie_set_lane_count() 의 스트랩 마스크 폭 결정.
	 * 값 범위: 1, 2, 4. */
};

/* [한국어]
 * j721e_pcie_user_readl - TI 글루의 user_cfg 레지스터 하나를 읽는다
 *
 * @pcie: probe 에서 dev_set_drvdata() 로 매달아 둔 인스턴스 상태.
 * @offset: user_cfg 창 시작점으로부터의 바이트 오프셋
 *          (J721E_PCIE_USER_CMD_STATUS=0x4, J721E_PCIE_USER_LINKSTATUS=0x14).
 * @return: 그 레지스터의 32비트 값.
 *
 * 이 창은 Cadence IP 의 레지스터가 아니라 TI 가 IP 바깥에 덧댄 글루
 * 레지스터라서, 공통 코어의 cdns_pcie_readl()(pcie->reg_base 기준)로는
 * 닿을 수 없다. 그래서 별도의 base 와 별도의 접근자가 필요하다.
 * 동작은 base + offset 주소에 readl() 한 번이 전부다. readl() 은 완전한
 * 메모리 배리어를 동반하는 MMIO 읽기이므로 앞선 쓰기가 하드웨어에 도달한
 * 뒤에 실행되는 것이 보장된다. 실패할 수 있는 경로가 없어 반환값 검사도 없다.
 * 실행 컨텍스트는 호출자를 따른다 — link_up/start_link 경로에서는 프로세스
 * 컨텍스트지만, 원리상 어느 컨텍스트에서도 안전하다(잠들지 않는다).
 * 에러 경로: 없음. 창이 매핑되지 않은 상태로 불리면 그것은 probe 순서 버그다.
 *
 * 호출 체인:
 *   j721e_pcie_start_link() / j721e_pcie_stop_link() / j721e_pcie_link_up()
 *     → [이 함수] → readl()
 */
static inline u32 j721e_pcie_user_readl(struct j721e_pcie *pcie, u32 offset)
{
	/* [한국어] user_cfg 창의 base 에 오프셋을 더해 32비트를 읽는다.
	 * readl() 은 컴파일러 최적화로 사라지지 않고 순서도 보장되는 MMIO 읽기다. */
	return readl(pcie->user_cfg_base + offset);
}

/* [한국어]
 * j721e_pcie_user_writel - TI 글루의 user_cfg 레지스터 하나에 쓴다
 *
 * @pcie: 인스턴스 상태(user_cfg_base 를 꺼내려고 받는다).
 * @offset: user_cfg 창 기준 바이트 오프셋.
 * @value: 써 넣을 32비트 값. 호출자가 이미 read-modify-write 로 만들어 온다.
 * @return: 없음. MMIO 쓰기는 실패를 보고하지 않는다.
 *
 * 위 readl 래퍼와 짝이 되는 쓰기 쪽이다. 이 파일에서 이 함수로 바뀌는 것은
 * 링크 트레이닝 enable 비트뿐이며, 항상 "읽어서 비트만 바꿔 되쓰는" 형태로
 * 쓰인다 — 같은 레지스터의 다른 비트를 뭉개지 않기 위해서다.
 * writel() 은 순서 보장이 있는 MMIO 쓰기이지만 하드웨어가 그 쓰기를 언제
 * 반영하는지까지 기다려 주지는 않는다. 그래서 링크가 실제로 올라왔는지는
 * 별도로 j721e_pcie_link_up() 을 폴링해 확인한다.
 * 실행 컨텍스트: 호출자를 따르며 잠들지 않는다.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   j721e_pcie_start_link() / j721e_pcie_stop_link() → [이 함수] → writel()
 */
static inline void j721e_pcie_user_writel(struct j721e_pcie *pcie, u32 offset,
					  u32 value)
{
	/* [한국어] user_cfg 창의 해당 레지스터에 32비트를 쓴다. 인자 순서가 값-주소인 점에 유의. */
	writel(value, pcie->user_cfg_base + offset);
}

/* [한국어]
 * j721e_pcie_intd_readl - TI 인터럽트 취합기(INTD) 레지스터 하나를 읽는다
 *
 * @pcie: 인스턴스 상태(intd_cfg_base 사용).
 * @offset: INTD 창 기준 바이트 오프셋 — ENABLE_REG_SYS_2(0x108),
 *          ENABLE_CLR_REG_SYS_2(0x308), STATUS_REG_SYS_2(0x508).
 * @return: 그 레지스터의 32비트 값. 각 비트가 서로 다른 시스템 이벤트다.
 *
 * user_cfg 와 INTD 는 서로 다른 물리 창이라 base 가 따로이고, 그래서
 * 접근자도 따로 있다. INTD 는 여러 이벤트를 한 레지스터에 모아 두므로
 * 이 드라이버는 자기 몫인 pcie->linkdown_irq_regfield 비트만 본다.
 * 실행 컨텍스트: IRQ 핸들러(인터럽트 컨텍스트)에서도 불리므로 절대 잠들면
 * 안 되는데, readl() 은 잠들지 않으므로 안전하다.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   j721e_pcie_link_irq_handler() / j721e_pcie_config_link_irq() /
 *   j721e_pcie_disable_link_irq() → [이 함수] → readl()
 */
static inline u32 j721e_pcie_intd_readl(struct j721e_pcie *pcie, u32 offset)
{
	/* [한국어] intd_cfg 창의 base 에 오프셋을 더해 32비트를 읽는다. */
	return readl(pcie->intd_cfg_base + offset);
}

/* [한국어]
 * j721e_pcie_intd_writel - TI 인터럽트 취합기(INTD) 레지스터 하나에 쓴다
 *
 * @pcie: 인스턴스 상태(intd_cfg_base 사용).
 * @offset: INTD 창 기준 바이트 오프셋.
 * @value: 써 넣을 값. ENABLE 계열에는 "켤 비트", STATUS_CLR 계열에는
 *         "지울 비트" 를 넣는다(write-1-to-clear 형태).
 * @return: 없음.
 *
 * 이 취합기는 set/clear 레지스터가 쌍으로 나뉘어 있는 구조라
 * (ENABLE_REG_SYS_2 는 켜기, ENABLE_CLR_REG_SYS_2 는 끄기,
 * STATUS_REG_SYS_2 는 읽기, STATUS_CLR_REG_SYS_2 는 지우기) 비트를 끄려고
 * enable 레지스터에 0 을 쓸 필요가 없다 — clear 레지스터에 1 을 쓰면 된다.
 * 이 구조 덕분에 서로 다른 비트를 쓰는 주체끼리 read-modify-write 경쟁이
 * 생기지 않는다.
 * 실행 컨텍스트: IRQ 핸들러에서도 불린다(status clear). 잠들지 않는다.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   j721e_pcie_link_irq_handler() / j721e_pcie_config_link_irq() /
 *   j721e_pcie_disable_link_irq() → [이 함수] → writel()
 */
static inline void j721e_pcie_intd_writel(struct j721e_pcie *pcie, u32 offset,
					  u32 value)
{
	/* [한국어] intd_cfg 창의 해당 레지스터에 32비트를 쓴다. */
	writel(value, pcie->intd_cfg_base + offset);
}

/* [한국어]
 * j721e_pcie_link_irq_handler - 링크 다운 인터럽트 핸들러
 *
 * @irq: 발생한 IRQ 번호. 이 드라이버는 쓰지 않는다(핸들러가 하나뿐이라
 *       번호로 구분할 일이 없다).
 * @priv: devm_request_irq() 에 넘긴 dev_id — 우리 struct j721e_pcie.
 * @return: 우리 이벤트가 아니면 IRQ_NONE, 처리했으면 IRQ_HANDLED.
 *          IRQ_NONE 을 정확히 돌려주는 것이 중요하다 — 이 인터럽트 선은
 *          여러 이벤트가 공유하는 취합기에 물려 있어서, 아무도 손들지
 *          않으면 커널이 "spurious IRQ" 로 판단해 선을 꺼 버린다.
 *
 * PCIe 링크가 끊어지면(카드 뽑힘, 상대 리셋, 신호 불량) 취합기가 이 선을
 * 올린다. 이 드라이버는 복구를 시도하지 않고 로그만 남긴 뒤 상태 비트를
 * 지운다 — 지우지 않으면 레벨 트리거된 선이 계속 걸려 인터럽트 폭풍이 난다.
 * 동작 단계: (1) STATUS_REG_SYS_2 를 읽어 (2) 내 비트가 서 있는지 보고
 * (3) 아니면 IRQ_NONE, 맞으면 (4) 에러 로그 (5) STATUS_CLR_REG_SYS_2 에
 * 내 비트만 써서 지우고 IRQ_HANDLED.
 * 실행 컨텍스트: 인터럽트 컨텍스트(하드IRQ). 잠드는 함수를 부르면 안 되며,
 * 여기서 부르는 readl/writel/dev_err 는 모두 안전하다. 등록이 스레드화되지
 * 않았으므로(devm_request_irq 의 flags 가 0) 진짜 하드IRQ 다.
 * 호출자: 커널 IRQ 코어(handle_irq_event) — probe 의 devm_request_irq() 로
 * 등록된 뒤부터, devres 가 해제할 때까지.
 * 피호출자: j721e_pcie_intd_readl/writel, dev_err.
 * 에러 경로: 자체 에러는 없다. 링크 복구는 상위 계층(핫플러그 등)의 몫이다.
 *
 * 호출 체인:
 *   커널 IRQ 코어 → [이 함수] → j721e_pcie_intd_readl/writel()
 */
static irqreturn_t j721e_pcie_link_irq_handler(int irq, void *priv)
{
	/* [한국어] devm_request_irq 에 dev_id 로 넘긴 포인터가 그대로 돌아온다.
	 * 핸들러가 어느 인스턴스의 인터럽트인지 아는 유일한 수단이다. */
	struct j721e_pcie *pcie = priv;
	/* [한국어] 오류 로그를 낼 device. 우리 상태에서 공통 코어 구조체를 거쳐 꺼낸다. */
	struct device *dev = pcie->cdns_pcie->dev;
	/* [한국어] STATUS 레지스터에서 읽은 이벤트 비트들을 담을 임시 변수. */
	u32 reg;

	/* [한국어] 어떤 시스템 이벤트가 발생했는지 읽는다. 이 취합기 레지스터에는 우리
	 * 링크 다운 말고도 다른 이벤트 비트가 함께 들어 있다. */
	reg = j721e_pcie_intd_readl(pcie, STATUS_REG_SYS_2);
	/* [한국어] 내 비트가 안 서 있으면 이 인터럽트는 다른 이벤트 때문이다.
	 * 공유된 선이므로 IRQ_NONE 을 돌려 다른 핸들러에게 차례를 넘겨야 한다. */
	if (!(reg & pcie->linkdown_irq_regfield))
		/* [한국어] 내 이벤트가 아니므로 처리하지 않았다고 알린다.
		 * 이 반환이 정확해야 커널이 공유된 선을 잘못 끄지 않는다. */
		return IRQ_NONE;

	/* [한국어] 링크가 끊어졌음을 커널 로그에 남긴다. 이 드라이버는 복구를 시도하지
	 * 않으므로, 사용자가 원인을 추적할 단서는 이 한 줄이 전부다. */
	dev_err(dev, "LINK DOWN!\n");

	/* [한국어] STATUS_CLR 에 내 비트만 써서 지운다(write-1-to-clear).
	 * 다른 이벤트의 status 는 그 주인이 지우도록 건드리지 않는다.
	 * 이것을 빠뜨리면 레벨 트리거된 선이 계속 걸려 인터럽트가 무한 반복된다. */
	j721e_pcie_intd_writel(pcie, STATUS_CLR_REG_SYS_2, pcie->linkdown_irq_regfield);
	/* [한국어] 내 이벤트를 처리했음을 알린다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * j721e_pcie_disable_link_irq - 링크 다운 인터럽트를 취합기에서 끈다
 *
 * @pcie: 인스턴스 상태. 어느 비트를 끌지는 pcie->linkdown_irq_regfield 로 안다.
 * @return: 없음.
 *
 * 드라이버를 내릴 때 인터럽트 소스를 하드웨어 쪽에서 막아 두기 위한 함수다.
 * devm_request_irq 로 등록한 핸들러는 devres 가 알아서 떼 주지만, 그 사이
 * 하드웨어가 선을 올려 두면 남은 공유 핸들러들이 헛돌게 되므로 소스 자체를
 * 끄는 편이 깨끗하다.
 * 동작 단계: ENABLE_CLR_REG_SYS_2 를 읽고, 거기에 내 비트를 OR 로 얹어 되쓴다.
 * 이 레지스터는 "1 을 쓴 비트를 enable 에서 지운다" 는 clear 레지스터라
 * 다른 이벤트의 enable 은 건드리지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트(remove 경로). 잠들지 않는다.
 * 호출자: j721e_pcie_remove() 한 곳뿐이다.
 * 피호출자: j721e_pcie_intd_readl/writel.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   j721e_pcie_remove() → [이 함수] → j721e_pcie_intd_readl/writel()
 */
static void j721e_pcie_disable_link_irq(struct j721e_pcie *pcie)
{
	/* [한국어] enable clear 레지스터 값을 담을 임시 변수. */
	u32 reg;

	/* [한국어] enable clear 레지스터를 먼저 읽는다. 이 레지스터는 보통 읽으면 0 을
	 * 주지만, 상류 코드가 read-modify-write 형태를 유지하고 있다. */
	reg = j721e_pcie_intd_readl(pcie, ENABLE_CLR_REG_SYS_2);
	/* [한국어] 끌 비트를 얹는다. 다른 비트는 0 이므로 그 이벤트들의 enable 은
	 * 영향을 받지 않는다. */
	reg |= pcie->linkdown_irq_regfield;
	/* [한국어] 고친 값을 되쓴다. 이 순간부터 링크 다운이 인터럽트를 내지 못한다. */
	j721e_pcie_intd_writel(pcie, ENABLE_CLR_REG_SYS_2, reg);
}

/* [한국어]
 * j721e_pcie_config_link_irq - 링크 다운 인터럽트를 취합기에서 켠다
 *
 * @pcie: 인스턴스 상태. 켤 비트는 pcie->linkdown_irq_regfield.
 * @return: 없음.
 *
 * 위 disable 의 대칭이다. ENABLE_REG_SYS_2 를 읽어 내 비트를 OR 로 얹어
 * 되쓴다. 이쪽은 set 레지스터라 읽어서 얹는 형태를 쓴다.
 * 순서가 중요하다 — probe 에서는 devm_request_irq() 로 핸들러를 먼저 등록한
 * 뒤에 이 함수를 부른다. 반대로 하면 핸들러가 없는 상태에서 인터럽트가 올라와
 * 처리되지 않은 인터럽트가 된다.
 * 절전 복귀(resume) 경로에서도 다시 부른다. 컨트롤러 전원 도메인이 내려갔다
 * 오면 이 enable 비트가 리셋 값으로 돌아가기 때문이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe / resume_noirq). 잠들지 않는다.
 * 호출자: j721e_pcie_probe(), j721e_pcie_resume_noirq().
 * 피호출자: j721e_pcie_intd_readl/writel.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   j721e_pcie_probe() 또는 j721e_pcie_resume_noirq()
 *     → [이 함수] → j721e_pcie_intd_readl/writel()
 */
static void j721e_pcie_config_link_irq(struct j721e_pcie *pcie)
{
	/* [한국어] enable 레지스터 값을 담을 임시 변수. */
	u32 reg;

	/* [한국어] 현재 enable 상태를 읽는다. set 레지스터라 다른 이벤트가 이미 켜 둔
	 * 비트를 보존하려면 반드시 읽어서 얹어야 한다. */
	reg = j721e_pcie_intd_readl(pcie, ENABLE_REG_SYS_2);
	/* [한국어] 내 링크 다운 비트를 켠다. */
	reg |= pcie->linkdown_irq_regfield;
	/* [한국어] 고친 값을 되쓴다. 이 순간부터 링크 다운이 인터럽트를 낼 수 있게 된다. */
	j721e_pcie_intd_writel(pcie, ENABLE_REG_SYS_2, reg);
}

/* [한국어]
 * j721e_pcie_start_link - 링크 트레이닝을 시작시킨다 (ops->start_link 구현)
 *
 * @cdns_pcie: Cadence 공통 코어가 들고 있는 컨트롤러 구조체. 우리 것이 아니라
 *             공통 코어의 것이므로, 여기서 우리 상태로 되돌아가려면
 *             cdns_pcie->dev 의 drvdata 를 꺼내야 한다.
 * @return: 항상 0. 이 SoC 에서는 레지스터 한 비트를 세우는 것이 전부라
 *          실패할 여지가 없다. 그래도 int 를 돌려주는 이유는 콜백 시그니처가
 *          공통 코어(struct cdns_pcie_ops)에 고정되어 있기 때문이다.
 *
 * 공통 코어는 "링크를 올려라" 만 알고, 그것을 어느 레지스터의 어느 비트로
 * 하는지는 SoC 마다 다르다. 그 SoC 별 지식이 이 함수다.
 * 동작 단계: J721E_PCIE_USER_CMD_STATUS(0x4) 를 읽어 LINK_TRAINING_ENABLE
 * (비트 0)만 세워 되쓴다. read-modify-write 를 쓰는 이유는 같은 레지스터에
 * 다른 제어 비트가 함께 있기 때문이다. 이 비트를 세우는 순간 LTSSM 이
 * Detect 상태에서 풀려 트레이닝을 시작한다. 완료를 기다리는 것은 이 함수가
 * 아니라 공통 코어의 cdns_pcie_host_wait_for_link() 몫이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 또는 resume 에서 이어진 호출).
 * 잠들지 않는다.
 * 호출자: 공통 코어의 static inline 디스패처 cdns_pcie_start_link()
 * (pcie-cadence.h:418). 그 디스패처를 부르는 곳은 RC 경로의
 * cdns_pcie_host_link_setup()(pcie-cadence-host.c:1166) 과 EP 경로의
 * pcie-cadence-ep.c:1825 다.
 * 피호출자: j721e_pcie_user_readl/writel.
 * 에러 경로: 없음. 링크가 안 올라오는 것은 여기서가 아니라 대기 함수에서
 * 드러나며, 그때도 RC 경로는 probe 를 접지 않고 dev_dbg 로만 남긴다.
 *
 * 호출 체인:
 *   cdns_pcie_host_link_setup() → cdns_pcie_start_link()[디스패처]
 *     → [이 함수] → j721e_pcie_user_writel()
 */
static int j721e_pcie_start_link(struct cdns_pcie *cdns_pcie)
{
	/* [한국어] 공통 코어가 넘긴 cdns_pcie 의 dev 를 통해 우리 인스턴스 상태를 되찾는다.
	 * probe 가 dev_set_drvdata() 를 미리 해 두었기에 성립한다. */
	struct j721e_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	/* [한국어] user_cfg 명령/상태 레지스터 값을 담을 임시 변수. */
	u32 reg;

	/* [한국어] 현재 명령/상태 값을 읽는다. 같은 레지스터의 다른 제어 비트를 보존하기
	 * 위해 통째로 읽어 온다. */
	reg = j721e_pcie_user_readl(pcie, J721E_PCIE_USER_CMD_STATUS);
	/* [한국어] LINK_TRAINING_ENABLE(비트 0)만 세운다. 이 순간 LTSSM 이 Detect 에서
	 * 풀려 링크 파트너를 찾기 시작한다. */
	reg |= LINK_TRAINING_ENABLE;
	/* [한국어] 고친 값을 되쓴다. 트레이닝 완료를 기다리는 것은 이 함수가 아니라
	 * 공통 코어의 폴링 루프 몫이다. */
	j721e_pcie_user_writel(pcie, J721E_PCIE_USER_CMD_STATUS, reg);

	/* [한국어] 이 SoC 에서는 실패할 수 없으므로 항상 성공을 알린다. */
	return 0;
}

/* [한국어]
 * j721e_pcie_stop_link - 링크 트레이닝을 멈춘다 (ops->stop_link 구현)
 *
 * @cdns_pcie: 공통 코어의 컨트롤러 구조체. drvdata 로 우리 상태를 되찾는다.
 * @return: 없음(콜백 시그니처가 void).
 *
 * start_link 의 정확한 대칭이다. J721E_PCIE_USER_CMD_STATUS 를 읽어
 * LINK_TRAINING_ENABLE 비트만 지워 되쓴다. 그러면 LTSSM 이 트레이닝을
 * 중단하고 링크가 내려간다.
 * 왜 필요한가: 드라이버를 내릴 때 링크를 살려 둔 채로 컨트롤러를 방치하면
 * 아래 장치가 여전히 DMA 를 시도할 수 있다. 그래서 열거를 걷어내는 것과
 * 함께 링크도 끊는다.
 * 실행 컨텍스트: 프로세스 컨텍스트(remove 경로). 잠들지 않는다.
 * 호출자: 디스패처 cdns_pcie_stop_link()(pcie-cadence.h:426). 그것을 부르는
 * 곳은 이 트리에서 cdns_pcie_host_link_disable()(pcie-cadence-host.c:1110)
 * 한 곳이고, 그 함수는 cdns_pcie_host_disable() 을 거쳐 j721e_pcie_remove()
 * 에서 시작된다.
 * 피호출자: j721e_pcie_user_readl/writel.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   j721e_pcie_remove() → cdns_pcie_host_disable() →
 *   cdns_pcie_host_link_disable() → cdns_pcie_stop_link()[디스패처]
 *     → [이 함수] → j721e_pcie_user_writel()
 */
static void j721e_pcie_stop_link(struct cdns_pcie *cdns_pcie)
{
	/* [한국어] 우리 인스턴스 상태를 되찾는다. */
	struct j721e_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	/* [한국어] user_cfg 명령/상태 레지스터 값을 담을 임시 변수. */
	u32 reg;

	/* [한국어] 현재 값을 읽는다. */
	reg = j721e_pcie_user_readl(pcie, J721E_PCIE_USER_CMD_STATUS);
	/* [한국어] LINK_TRAINING_ENABLE 비트만 지운다. ~ 로 뒤집은 마스크와 AND 하므로
	 * 다른 제어 비트는 그대로 남는다. 이 비트가 내려가면 링크가 끊어진다. */
	reg &= ~LINK_TRAINING_ENABLE;
	/* [한국어] 고친 값을 되쓴다. 트레이닝이 멈추고 링크가 내려간다. */
	j721e_pcie_user_writel(pcie, J721E_PCIE_USER_CMD_STATUS, reg);
}

/* [한국어]
 * j721e_pcie_link_up - 링크가 실제로 올라왔는지 읽는다 (ops->link_up 구현)
 *
 * @cdns_pcie: 공통 코어의 컨트롤러 구조체. drvdata 로 우리 상태를 되찾는다.
 * @return: DL(Data Link) 계층까지 초기화가 끝났으면 true, 아니면 false.
 *
 * 공통 코어는 링크 트레이닝을 시작한 뒤 이 함수를 반복 폴링해 완료를
 * 판정한다. "어느 레지스터의 어느 값이 링크 완료인가" 가 SoC 마다 다르므로
 * 콜백으로 빠져 있다.
 * 동작: J721E_PCIE_USER_LINKSTATUS(0x14) 를 읽어 LINK_STATUS(비트 1:0)만
 * 남기고, 그 값이 LINK_UP_DL_COMPLETED(3)와 정확히 같은지 본다. 부분적으로
 * 올라온 상태(LINK_UP_DL_IN_PROGRESS=2)를 링크로 쳐 주면 아직 TLP 를 받을
 * 수 없는 컨트롤러에 config 접근을 하게 되어 타임아웃이 난다. 그래서
 * 비트 검사가 아니라 값 일치 비교를 쓴다.
 * 실행 컨텍스트: 프로세스 컨텍스트(폴링 루프 안). 잠들지 않는다.
 * 호출자: pcie-cadence.h:432 의 static inline 디스패처 cdns_pcie_link_up().
 * 이 트리에서 그 디스패처를 실제로 넘기는 곳은 구형(LGA) 호스트 경로인
 * cdns_pcie_host_link_setup()(pcie-cadence-host.c:1183) 한 곳이며, 거기서
 * cdns_pcie_host_start_link() 를 거쳐 폴링 루프
 * cdns_pcie_host_wait_for_link() 와 재트레이닝 cdns_pcie_retrain() 에
 * 함수 포인터로 전달된다.
 * 피호출자: j721e_pcie_user_readl.
 * 에러 경로: 없음. 시간 내에 true 가 안 나오면 호출자 쪽에서 타임아웃 처리한다.
 *
 * 호출 체인:
 *   cdns_pcie_host_wait_for_link() → cdns_pcie_link_up()[디스패처]
 *     → [이 함수] → j721e_pcie_user_readl()
 */
static bool j721e_pcie_link_up(struct cdns_pcie *cdns_pcie)
{
	/* [한국어] 우리 인스턴스 상태를 되찾는다. */
	struct j721e_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	/* [한국어] 링크 상태 레지스터 값을 담을 임시 변수. */
	u32 reg;

	/* [한국어] J721E_PCIE_USER_LINKSTATUS(0x14)를 읽는다. */
	reg = j721e_pcie_user_readl(pcie, J721E_PCIE_USER_LINKSTATUS);
	/* [한국어] LINK_STATUS(비트 1:0)만 남겨 LINK_UP_DL_COMPLETED(3)와 값이 같은지 본다.
	 * 비트 검사가 아니라 값 일치 비교여야 부분적으로 올라온 상태를 걸러 낼 수 있다. */
	return (reg & LINK_STATUS) == LINK_UP_DL_COMPLETED;
}

/* [한국어]
 * j721e_pcie_ops - Cadence 공통 코어가 되부르는 SoC 별 링크 제어 콜백 표
 *
 * probe 가 cdns_pcie->ops 에 이 표의 주소를 꽂아 두면, 그 뒤로 공통 코어는
 * 링크를 다뤄야 할 때마다 pcie-cadence.h 의 static inline 디스패처
 * (cdns_pcie_start_link / cdns_pcie_stop_link / cdns_pcie_link_up)를 거쳐
 * 여기로 내려온다. 디스패처들은 ops 나 해당 콜백이 NULL 이면 각각 0 / 무동작 /
 * true 를 돌려주도록 되어 있어, 콜백을 안 채우는 SoC 도 동작한다.
 * 네 번째 멤버 cpu_addr_fixup 은 채우지 않는다 — J721E 는 CPU 물리 주소를
 * 그대로 PCIe 아웃바운드 창 주소로 쓸 수 있기 때문이다(pcie-cadence-plat.c 는
 * 반대로 그것 하나만 채운다).
 * const 정적 데이터이므로 읽기 전용이고 동기화가 필요 없다.
 */
static const struct cdns_pcie_ops j721e_pcie_ops = {
	/* [한국어] 링크 트레이닝 시작 콜백. */
	.start_link = j721e_pcie_start_link,
	/* [한국어] 링크 트레이닝 중단 콜백. */
	.stop_link = j721e_pcie_stop_link,
	/* [한국어] 링크 완료 판정 콜백. */
	.link_up = j721e_pcie_link_up,
};

/* [한국어]
 * j721e_pcie_set_mode - CTRL_MMR 스트랩에 RC/EP 모드를 써 넣는다
 *
 * @pcie: 인스턴스 상태. pcie->mode 와 오류 로그용 dev 를 꺼내 쓴다.
 * @syscon: "ti,syscon-pcie-ctrl" 로 얻은 CTRL_MMR regmap. 이 레지스터 공간은
 *          PCIe 컨트롤러가 아니라 SoC 시스템 제어 블록에 있으며 여러
 *          드라이버가 공유하므로, 직접 ioremap 하지 않고 regmap 을 쓴다.
 * @offset: 그 regmap 안에서 이 PCIe 인스턴스의 PCIEn_CTRL 레지스터 오프셋.
 *          DT phandle 의 인자로 오며, 구형 DT 호환을 위해 없으면 0 이다.
 * @return: 0 성공, 음수 errno 실패(regmap 접근 실패).
 *
 * 왜 필요한가: 같은 실리콘을 RC 로도 EP 로도 쓸 수 있는데, 그 선택은
 * 컨트롤러가 전원을 받는 순간 CTRL_MMR 의 스트랩 비트를 샘플링해 결정된다.
 * 따라서 컨트롤러가 꺼져 있는 동안 이 비트를 맞춰 놓아야 한다(그 껐다 켜는
 * 일은 호출자 j721e_pcie_ctrl_init 이 한다).
 * 동작 단계: mask 를 J721E_MODE_RC(비트 7)로 두고, RC 모드면 그 비트를 1 로,
 * EP 모드면 0 으로 하는 val 을 만들어 regmap_update_bits() 로 그 비트만 바꾼다.
 * update_bits 는 내부적으로 read-modify-write 를 regmap 락 아래에서 하므로,
 * 같은 CTRL_MMR 을 쓰는 다른 드라이버와의 경쟁이 안전하게 처리된다.
 * 실행 컨텍스트: 프로세스 컨텍스트. regmap 이 잠들 수 있으므로 아토믹
 * 컨텍스트에서 부르면 안 된다.
 * 호출자: j721e_pcie_ctrl_init() 한 곳.
 * 피호출자: regmap_update_bits(), 실패 시 dev_err().
 * 에러 경로: 실패하면 로그를 남기고 errno 를 그대로 올려 보내며,
 * ctrl_init → probe 로 전파되어 probe 가 실패한다.
 *
 * 호출 체인:
 *   j721e_pcie_ctrl_init() → [이 함수] → regmap_update_bits()
 */
static int j721e_pcie_set_mode(struct j721e_pcie *pcie, struct regmap *syscon,
			       unsigned int offset)
{
	/* [한국어] 오류 로그를 낼 device. 공통 코어 구조체를 거쳐 꺼낸다. */
	struct device *dev = pcie->cdns_pcie->dev;
	/* [한국어] 바꿀 비트는 모드 비트 하나뿐이다. regmap_update_bits 에 이 마스크를
	 * 넘겨 나머지 스트랩 비트(속도, 레인 수)를 보존한다. */
	u32 mask = J721E_MODE_RC;
	/* [한국어] probe 가 compatible 로부터 복사해 둔 RC/EP 선택값. */
	u32 mode = pcie->mode;
	/* [한국어] 쓸 값. 0 으로 시작하므로 EP 모드면 그대로 비트를 0 으로 쓰게 된다. */
	u32 val = 0;
	/* [한국어] regmap 반환값. 0 으로 초기화해 두었지만 아래에서 곧바로 덮어쓴다. */
	int ret = 0;

	/* [한국어] RC 모드일 때만 비트를 세운다. */
	if (mode == PCI_MODE_RC)
		/* [한국어] RC 모드 표시. mask 와 같은 값이므로 결과적으로 비트 7 이 1 이 된다. */
		val = J721E_MODE_RC;

	/* [한국어] CTRL_MMR 의 PCIEn_CTRL 레지스터에서 모드 비트만 갈아 끼운다.
	 * regmap 내부 락 아래의 read-modify-write 이므로, 같은 레지스터를 쓰는
	 * 다른 SoC 드라이버와 경쟁해도 안전하다. */
	ret = regmap_update_bits(syscon, offset, mask, val);
	/* [한국어] regmap 접근 실패(시스템 컨트롤러 응답 없음 등). */
	if (ret)
		/* [한국어] 로그를 남긴다. 반환값은 아래에서 그대로 올려 보내 probe 를 실패시킨다. */
		dev_err(dev, "failed to set pcie mode\n");

	/* [한국어] regmap 결과를 그대로 올려 보낸다. 0 이면 성공. */
	return ret;
}

/* [한국어]
 * j721e_pcie_set_link_speed - CTRL_MMR 스트랩에 최대 링크 속도를 써 넣는다
 *
 * @pcie: 인스턴스 상태. of_node 와 오류 로그용 dev 를 꺼내 쓴다.
 * @syscon: CTRL_MMR regmap.
 * @offset: 이 인스턴스의 PCIEn_CTRL 레지스터 오프셋.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 왜 필요한가: 보드 배선 품질에 따라 Gen3 까지 못 올리는 경우가 있어,
 * DT 의 "max-link-speed" 로 상한을 걸 수 있어야 한다. 그 상한도 스트랩이므로
 * 컨트롤러가 꺼진 동안 써야 한다.
 * 동작 단계: (1) of_pci_get_max_link_speed() 로 DT 값을 읽는다(1-based
 * 세대 번호: 1=Gen1, 2=Gen2, ...). (2) 값이 2 미만이거나
 * pcie_get_link_speed() 가 모르는 세대면 2(Gen2)로 강제한다 — 속성이 없으면
 * of_pci_get_max_link_speed 가 음수를 돌려주므로 이 검사 하나가 "없음" 과
 * "이상한 값" 을 함께 처리한다. 결과적으로 이 드라이버의 기본값은 Gen2 다.
 * (3) 스트랩 필드는 0-based 인코딩이라 val = link_speed - 1 로 바꾼다.
 * (4) GENERATION_SEL_MASK(비트 1:0)만 갈아 끼운다.
 * 실행 컨텍스트: 프로세스 컨텍스트. of/regmap 모두 잠들 수 있다.
 * 호출자: j721e_pcie_ctrl_init() 한 곳.
 * 피호출자: of_pci_get_max_link_speed()(drivers/pci/of.c),
 * pcie_get_link_speed()(drivers/pci/probe.c), regmap_update_bits().
 * 에러 경로: DT 값이 이상해도 실패시키지 않고 Gen2 로 낮춰 계속한다 —
 * 링크 속도는 부팅을 막을 만한 문제가 아니기 때문이다. regmap 실패만
 * errno 로 올려 보낸다.
 *
 * 호출 체인:
 *   j721e_pcie_ctrl_init() → [이 함수] → of_pci_get_max_link_speed(),
 *   pcie_get_link_speed(), regmap_update_bits()
 */
static int j721e_pcie_set_link_speed(struct j721e_pcie *pcie,
				     struct regmap *syscon, unsigned int offset)
{
	/* [한국어] 오류 로그를 낼 device. */
	struct device *dev = pcie->cdns_pcie->dev;
	/* [한국어] DT 속성을 읽을 노드. 이 PCIe 인스턴스의 DT 노드다. */
	struct device_node *np = dev->of_node;
	/* [한국어] DT 에서 읽은 최대 링크 세대 번호(1-based). 음수일 수 있어 int 다. */
	int link_speed;
	/* [한국어] 스트랩에 실제로 쓸 0-based 값. */
	u32 val = 0;
	/* [한국어] regmap 반환값. */
	int ret;

	/* [한국어] DT 의 'max-link-speed' 속성을 읽는다. 속성이 없거나 형식이 틀리면
	 * 음수 errno 를 돌려준다(drivers/pci/of.c). */
	link_speed = of_pci_get_max_link_speed(np);
	/* [한국어] 두 가지를 한 번에 거른다 — 값이 2 미만(속성 없음의 음수, 또는 Gen1),
	 * 그리고 pcie_get_link_speed() 가 모르는 세대 번호(범위를 벗어난 값). */
	if ((link_speed < 2) ||
	    (pcie_get_link_speed(link_speed) == PCI_SPEED_UNKNOWN))
		/* [한국어] 어느 쪽이든 Gen2 로 강제한다. 즉 이 드라이버의 기본 상한은 Gen2 이며,
		 * 더 높이려면 DT 에 명시해야 한다. */
		link_speed = 2;

	/* [한국어] 스트랩 필드가 0-based 라 1 을 뺀다(Gen2 -> 1). */
	val = link_speed - 1;
	/* [한국어] GENERATION_SEL_MASK(비트 1:0)만 갈아 끼운다. 모드 비트와 레인 비트는
	 * 건드리지 않는다. */
	ret = regmap_update_bits(syscon, offset, GENERATION_SEL_MASK, val);
	/* [한국어] regmap 접근 실패. */
	if (ret)
		/* [한국어] 로그만 남기고 아래에서 errno 를 올려 보낸다. */
		dev_err(dev, "failed to set link speed\n");

	/* [한국어] regmap 결과를 그대로 올려 보낸다. */
	return ret;
}

/* [한국어]
 * j721e_pcie_set_lane_count - CTRL_MMR 스트랩에 레인 수를 써 넣는다
 *
 * @pcie: 인스턴스 상태. pcie->num_lanes(쓸 레인 수)와 pcie->max_lanes
 *        (이 SoC 가 지원하는 최대치)를 함께 쓴다.
 * @syscon: CTRL_MMR regmap.
 * @offset: 이 인스턴스의 PCIEn_CTRL 레지스터 오프셋.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 왜 필요한가: 보드가 x1 로만 배선했는데 컨트롤러가 x2 로 트레이닝을 시도하면
 * 시간을 낭비하거나 링크가 불안정해진다. 레인 수도 스트랩이라 전원 인가 전에
 * 정해야 한다.
 * 동작 단계: (1) 기본 마스크를 BIT(8) 한 비트로 잡는다 — 최대 2레인인 SoC 는
 * 스트랩 필드가 1비트면 충분하다(0=x1, 1=x2). (2) 이 SoC 가 4레인까지
 * 지원하면(max_lanes == 4) 필드를 GENMASK(9, 8) 두 비트로 넓힌다
 * (0=x1, 1=x2, 3=x4). (3) LANE_COUNT(n) 매크로로 (num_lanes - 1) 을
 * 비트 8 위치로 밀어 값을 만든다 — 필드가 0-based 이기 때문이다.
 * (4) regmap_update_bits() 로 그 필드만 바꾼다.
 * 주의: max_lanes 가 1 인 SoC(AM64, J722S)에서는 num_lanes 도 1 로 제한되어
 * val 이 0 이 되므로, BIT(8) 마스크로 0 을 쓰는 것이 곧 x1 설정이 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 호출자: j721e_pcie_ctrl_init() 한 곳.
 * 피호출자: regmap_update_bits(), 실패 시 dev_err().
 * 에러 경로: regmap 실패를 errno 로 올려 보내 probe 를 실패시킨다.
 *
 * 호출 체인:
 *   j721e_pcie_ctrl_init() → [이 함수] → regmap_update_bits()
 */
static int j721e_pcie_set_lane_count(struct j721e_pcie *pcie,
				     struct regmap *syscon, unsigned int offset)
{
	/* [한국어] 오류 로그를 낼 device. */
	struct device *dev = pcie->cdns_pcie->dev;
	/* [한국어] probe 가 DT 에서 읽어 검증까지 마친 레인 수(1 이상 max_lanes 이하). */
	u32 lanes = pcie->num_lanes;
	/* [한국어] 기본 스트랩 필드 폭은 비트 8 한 개다. 최대 2레인인 SoC 는
	 * 0=x1, 1=x2 로 1비트면 충분하다. */
	u32 mask = BIT(8);
	/* [한국어] 필드에 쓸 값. */
	u32 val = 0;
	/* [한국어] regmap 반환값. */
	int ret;

	/* [한국어] 이 SoC 가 4레인까지 지원하면 1비트로는 표현이 안 된다. */
	if (pcie->max_lanes == 4)
		/* [한국어] 필드를 비트 9:8 두 비트로 넓힌다. x4 는 값 3 이라 두 비트가 필요하다. */
		mask = GENMASK(9, 8);

	/* [한국어] (레인 수 - 1) 을 비트 8 위치로 민다. x1 -> 0, x2 -> 1, x4 -> 3. */
	val = LANE_COUNT(lanes - 1);
	/* [한국어] 레인 수 필드만 갈아 끼운다. */
	ret = regmap_update_bits(syscon, offset, mask, val);
	/* [한국어] regmap 접근 실패. */
	if (ret)
		/* [한국어] 로그만 남기고 errno 를 올려 보낸다. */
		dev_err(dev, "failed to set link count\n");

	/* [한국어] regmap 결과를 그대로 올려 보낸다. */
	return ret;
}

/* [한국어]
 * j721e_enable_acspcie_refclk - SoC 내부 ACSPCIE 버퍼로 refclk 를 내보낸다
 *
 * @pcie: 인스턴스 상태. of_node 와 dev 를 쓴다.
 * @syscon: "ti,syscon-acspcie-proxy-ctrl" 로 얻은 별도의 regmap.
 *          위 세 함수가 쓰는 PCIEn_CTRL regmap 과는 다른 창이다.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 왜 필요한가: 보드에 따로 클럭 발생기를 두지 않고 SoC 안의 ACSPCIE 모듈로
 * PCIe 레퍼런스 클럭을 만들어 슬롯에 내보내는 설계가 있다. 그런 보드에서는
 * 해당 출력 패드가 리셋 직후 "disable" 상태라, 커널이 명시적으로 풀어 주어야
 * 카드가 클럭을 받는다. 보드가 외부 클럭을 쓰면 DT 속성이 아예 없고,
 * 호출자가 그 경우 이 함수를 부르지 않는다.
 * 동작 단계: (1) of_parse_phandle_with_fixed_args() 로 그 속성의 고정 인자
 * 1개를 읽는다. 이 인자는 "어느 패드를 켤 것인가" 의 비트마스크다.
 * (2) val = ~args.args[0] 로 뒤집는다 — 하드웨어 필드는 "disable" 비트라
 * 켜려면 0 을 써야 하기 때문이다. 위 영문 주석이 말하는 그대로다.
 * (3) ACSPCIE_PAD_DISABLE_MASK(비트 1:0) 범위에서만 regmap_update_bits() 로
 * 갈아 끼운다. 마스크 밖의 뒤집힌 1 들은 update_bits 가 걸러 준다.
 * 이 regmap 에서 오프셋으로 0 을 쓴다는 점에 주의 — DT 의 고정 인자는
 * 오프셋이 아니라 데이터로 쓰이며, 창의 첫 레지스터가 곧 제어 레지스터다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 또는 resume 에서 이어짐).
 * 호출자: j721e_pcie_ctrl_init() 한 곳(속성이 있을 때만).
 * 피호출자: of_parse_phandle_with_fixed_args(), regmap_update_bits().
 * 에러 경로: DT 인자 파싱 실패나 regmap 실패 모두 로그를 남기고 errno 를
 * 올려 보내 probe 를 실패시킨다. 클럭이 없으면 어차피 링크가 안 올라오므로
 * 조용히 넘어가지 않는 편이 낫다.
 *
 * 호출 체인:
 *   j721e_pcie_ctrl_init() → [이 함수] → regmap_update_bits()
 */
static int j721e_enable_acspcie_refclk(struct j721e_pcie *pcie,
				       struct regmap *syscon)
{
	/* [한국어] 오류 로그를 낼 device. */
	struct device *dev = pcie->cdns_pcie->dev;
	/* [한국어] ACSPCIE phandle 속성을 읽을 DT 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] 바꿀 비트는 패드 disable 비트 1:0 뿐이다. */
	u32 mask = ACSPCIE_PAD_DISABLE_MASK;
	/* [한국어] of_parse_phandle_with_fixed_args() 가 채워 줄 결과 — 대상 노드와
	 * 고정 인자 배열을 담는다. */
	struct of_phandle_args args;
	/* [한국어] regmap 에 쓸 값(뒤집은 마스크). */
	u32 val;
	/* [한국어] 반환값. */
	int ret;

	/* [한국어] 'ti,syscon-acspcie-proxy-ctrl' phandle 의 고정 인자 1개를 읽는다.
	 * 세 번째 인자 1 이 '인자 개수', 네 번째 0 이 '몇 번째 phandle 인가' 다.
	 * 여기서 얻는 args.args[0] 은 오프셋이 아니라 '어느 refclk 출력 패드를
	 * 켤 것인가' 의 비트마스크다. */
	ret = of_parse_phandle_with_fixed_args(node,
					       "ti,syscon-acspcie-proxy-ctrl",
					       1, 0, &args);
	/* [한국어] 속성은 있는데 인자 형식이 틀린 경우. 호출자는 속성이 존재할 때만
	 * 이 함수를 부르므로, 여기 걸리는 것은 DT 작성 오류다. */
	if (ret) {
		dev_err(dev,
			"ti,syscon-acspcie-proxy-ctrl has invalid arguments\n");
		/* [한국어] DT 인자 형식 오류를 그대로 올려 보낸다. */
		return ret;
	}

	/* Clear PAD IO disable bits to enable refclk output */
	val = ~(args.args[0]);
	/* [한국어] 이 regmap 의 오프셋 0 에 있는 제어 레지스터에서 패드 disable 비트만
	 * 갈아 끼운다. val 의 마스크 밖 비트(뒤집기로 생긴 상위 1 들)는
	 * update_bits 가 mask 로 걸러 준다. */
	ret = regmap_update_bits(syscon, 0, mask, val);
	/* [한국어] regmap 접근 실패. */
	if (ret) {
		/* [한국어] refclk 가 없으면 카드가 아예 동작하지 않으므로 errno 를 함께 남긴다. */
		dev_err(dev, "failed to enable ACSPCIE refclk: %d\n", ret);
		/* [한국어] regmap 실패를 그대로 올려 보낸다. */
		return ret;
	}

	/* [한국어] 패드가 열렸다. refclk 가 슬롯으로 나가기 시작한다. */
	return 0;
}

/* [한국어]
 * j721e_pcie_ctrl_init - CTRL_MMR 스트랩을 전부 프로그래밍한다
 *
 * @pcie: 인스턴스 상태. dev / of_node / mode / num_lanes / max_lanes 를 쓴다.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 이 파일에서 가장 SoC 색이 짙은 함수다. Cadence IP 자체가 아니라 그 바깥의
 * TI 시스템 제어 레지스터를 다룬다.
 * 왜 필요한가: 아래 영문 주석이 설명하듯, PCIe 컨트롤러의 레지스터
 * "리셋 값" 은 CTRL_MMR 의 PCIEn_CTRL 스트랩 설정에 따라 달라지고, 그 스트랩은
 * 컨트롤러에 전원이 들어오는 순간 한 번 래치된다. 그래서 그냥 쓰기만 해서는
 * 반영되지 않고, 반드시 "컨트롤러 전원 끄기 → 스트랩 쓰기 → 전원 켜기" 순서를
 * 지켜야 한다. 이 함수의 구조가 통째로 그 순서다.
 * 동작 단계:
 *   (1) DT phandle "ti,syscon-pcie-ctrl" 로 CTRL_MMR regmap 을 얻는다.
 *   (2) 같은 phandle 의 고정 인자에서 이 인스턴스의 레지스터 오프셋을 읽는다.
 *       인자가 없는 구형 DT 도 있으므로 실패해도 오프셋 0 으로 계속한다.
 *   (3) pm_runtime_put_sync() — 사용 카운트를 내려 GENPD 가 컨트롤러 전원
 *       도메인을 끄게 한다. 이것이 "전원 끄기" 다.
 *   (4) 모드 / 링크 속도 / 레인 수 세 스트랩을 쓴다.
 *   (5) pm_runtime_get_sync() — 다시 켜면서 방금 쓴 스트랩이 래치된다.
 *   (6) 선택 사항인 ACSPCIE refclk 출력 활성화. 속성이 없으면 그냥 0 을
 *       돌려주고 끝낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트. runtime PM 의 _sync 계열은 전원 도메인이
 * 켜지고 꺼질 때까지 잠들며 기다리므로 아토믹 컨텍스트에서 부를 수 없다.
 * 호출자: j721e_pcie_probe() 와 j721e_pcie_resume_noirq(). resume 에서 다시
 * 부르는 이유는 절전 중 전원 도메인이 내려가 스트랩 래치가 사라지기 때문이다.
 * 피호출자: syscon_regmap_lookup_by_phandle(), of_parse_phandle_with_fixed_args(),
 * pm_runtime_put_sync()/get_sync(), 이 파일의 set_mode/set_link_speed/
 * set_lane_count/enable_acspcie_refclk.
 * 에러 경로: 어느 단계든 실패하면 즉시 errno 를 올려 보낸다. 주의할 점은
 * (3)에서 전원을 끈 뒤 (4)가 실패하면 전원이 꺼진 채로 반환된다는 것이다 —
 * 그 경우 호출자인 probe 는 err_get_sync 로 가서 pm_runtime_put()/disable()
 * 을 부르고 정리한다.
 *
 * 호출 체인:
 *   j721e_pcie_probe() 또는 j721e_pcie_resume_noirq()
 *     → [이 함수] → j721e_pcie_set_mode() / set_link_speed() /
 *       set_lane_count() / j721e_enable_acspcie_refclk()
 */
static int j721e_pcie_ctrl_init(struct j721e_pcie *pcie)
{
	/* [한국어] 오류 로그를 낼 device. */
	struct device *dev = pcie->cdns_pcie->dev;
	/* [한국어] DT phandle 속성을 읽을 이 PCIe 인스턴스의 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] phandle 의 고정 인자를 받을 구조체. 여기서는 인자 하나(레지스터 오프셋)를 쓴다. */
	struct of_phandle_args args;
	/* [한국어] CTRL_MMR regmap 안에서 이 인스턴스의 PCIEn_CTRL 오프셋.
	 * 0 으로 초기화해 두는 것이 중요하다 — 아래 파싱이 실패해도 그대로 0 을 쓴다. */
	unsigned int offset = 0;
	/* [한국어] CTRL_MMR(그리고 뒤에서 재활용해 ACSPCIE)의 regmap 핸들. */
	struct regmap *syscon;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] 'ti,syscon-pcie-ctrl' phandle 이 가리키는 시스템 컨트롤러의 regmap 을 얻는다.
	 * 이 regmap 은 syscon 코어가 캐시해 두고 여러 드라이버가 공유하므로,
	 * 여기서 해제할 책임이 없다. */
	syscon = syscon_regmap_lookup_by_phandle(node, "ti,syscon-pcie-ctrl");
	/* [한국어] IS_ERR 로 검사하는 이유 — 이 API 는 실패 시 NULL 이 아니라 ERR_PTR 을 준다.
	 * SoC 계열이 아닌 보드나 DT 속성 누락이 여기 걸린다. */
	if (IS_ERR(syscon)) {
		/* [한국어] regmap 이 없으면 스트랩을 전혀 만질 수 없으므로 probe 를 접는다. */
		dev_err(dev, "Unable to get ti,syscon-pcie-ctrl regmap\n");
		/* [한국어] IS_ERR 로 확인한 포인터에서 errno 를 뽑아 돌려준다. */
		return PTR_ERR(syscon);
	}

	/* Do not error out to maintain old DT compatibility */
	ret = of_parse_phandle_with_fixed_args(node, "ti,syscon-pcie-ctrl", 1,
					       0, &args);
	/* [한국어] 파싱이 성공했을 때만 오프셋을 갱신한다. 실패는 위 영문 주석대로
	 * 구형 DT 호환을 위해 오류로 취급하지 않는다. */
	if (!ret)
		/* [한국어] phandle 의 첫 번째 고정 인자가 이 인스턴스의 PCIEn_CTRL 오프셋이다.
		 * 같은 SoC 에 PCIe 인스턴스가 여럿이라 각자 다른 오프셋을 갖는다. */
		offset = args.args[0];

	/*
	 * The PCIe Controller's registers have different "reset-values"
	 * depending on the "strap" settings programmed into the PCIEn_CTRL
	 * register within the CTRL_MMR memory-mapped register space.
	 * The registers latch onto a "reset-value" based on the "strap"
	 * settings sampled after the PCIe Controller is powered on.
	 * To ensure that the "reset-values" are sampled accurately, power
	 * off the PCIe Controller before programming the "strap" settings
	 * and power it on after that. The runtime PM APIs namely
	 * pm_runtime_put_sync() and pm_runtime_get_sync() will decrement and
	 * increment the usage counter respectively, causing GENPD to power off
	 * and power on the PCIe Controller.
	 */
	ret = pm_runtime_put_sync(dev);
	/* [한국어] 전원 도메인을 끄지 못했다면 스트랩을 써도 래치되지 않는다. */
	if (ret < 0) {
		/* [한국어] 이 경우 스트랩 프로그래밍 자체가 무의미하므로 여기서 중단한다. */
		dev_err(dev, "Failed to power off PCIe Controller\n");
		/* [한국어] 모드 스트랩 실패를 올려 보낸다. 이때 컨트롤러 전원은 꺼진 상태다. */
		return ret;
	}

	/* [한국어] RC/EP 모드 스트랩을 쓴다. 컨트롤러가 꺼져 있는 지금이 유일한 기회다. */
	ret = j721e_pcie_set_mode(pcie, syscon, offset);
	/* [한국어] regmap 쓰기 실패. */
	if (ret < 0) {
		/* [한국어] 주의 — 이 지점에서 반환하면 컨트롤러 전원이 꺼진 채로 남는다.
		 * 호출자(probe)가 err_get_sync 로 가서 runtime PM 을 정리한다. */
		dev_err(dev, "Failed to set pci mode\n");
		/* [한국어] 속도 스트랩 실패를 올려 보낸다. 역시 전원이 꺼진 상태다. */
		return ret;
	}

	/* [한국어] 최대 링크 속도 스트랩을 쓴다. */
	ret = j721e_pcie_set_link_speed(pcie, syscon, offset);
	/* [한국어] regmap 쓰기 실패. */
	if (ret < 0) {
		/* [한국어] 위와 같이 전원이 꺼진 상태로 반환된다. */
		dev_err(dev, "Failed to set link speed\n");
		/* [한국어] 레인 수 스트랩 실패를 올려 보낸다. 역시 전원이 꺼진 상태다. */
		return ret;
	}

	/* [한국어] 레인 수 스트랩을 쓴다. 세 스트랩 모두 같은 PCIEn_CTRL 레지스터의
	 * 서로 다른 비트 필드라, 순서 자체에는 의존성이 없다. */
	ret = j721e_pcie_set_lane_count(pcie, syscon, offset);
	/* [한국어] regmap 쓰기 실패. */
	if (ret < 0) {
		/* [한국어] 위와 같다. */
		dev_err(dev, "Failed to set num-lanes\n");
		/* [한국어] 전원 복구 실패를 올려 보낸다. */
		return ret;
	}

	/* [한국어] 전원을 다시 켠다. 이 순간 방금 쓴 세 스트랩이 컨트롤러 레지스터의
	 * '리셋 값' 으로 래치된다 — 위 영문 주석이 설명하는 핵심 동작이다. */
	ret = pm_runtime_get_sync(dev);
	/* [한국어] 전원 도메인을 켜지 못한 경우. GENPD 나 펌웨어 쪽 문제다. */
	if (ret < 0) {
		/* [한국어] 컨트롤러가 죽은 채로는 아무것도 할 수 없으므로 중단한다. */
		dev_err(dev, "Failed to power on PCIe Controller\n");
		/* [한국어] ACSPCIE 활성화 실패를 올려 보낸다. */
		return ret;
	}

	/* Enable ACSPCIE refclk output if the optional property exists */
	syscon = syscon_regmap_lookup_by_phandle_optional(node,
						"ti,syscon-acspcie-proxy-ctrl");
	/* [한국어] 속성이 없으면 보드가 외부 클럭 발생기를 쓴다는 뜻이므로 할 일이 없다.
	 * _optional 판이라 없을 때 오류가 아니라 NULL 을 돌려준다는 점에 유의. */
	if (!syscon)
		/* [한국어] ACSPCIE 속성이 없는 보드는 여기서 정상 종료한다. */
		return 0;

	/* [한국어] 속성이 있으면 refclk 출력 패드를 열고 그 결과를 그대로 돌려준다. */
	return j721e_enable_acspcie_refclk(pcie, syscon);
}

/* [한국어]
 * cdns_ti_pcie_config_read - config space 읽기 (바이트 접근 제약 우회)
 *
 * @bus: 접근 대상 버스. 루트 버스인지 아래 장치인지 여기서 갈린다.
 * @devfn: 장치/함수 번호(상위 5비트 device, 하위 3비트 function).
 * @where: config space 안의 바이트 오프셋.
 * @size: 읽을 바이트 수 — 1, 2, 4 중 하나.
 * @value: 읽은 값을 담아 돌려줄 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND 등 PCI 규약 코드.
 *
 * 왜 필요한가: 이 컨트롤러의 루트 포트 자신의 config space(= 루트 버스)는
 * dword(4바이트) 단위 접근만 받는 판본이 있다. 그런 판본에서 1바이트나
 * 2바이트 읽기를 그대로 내려보내면 잘못된 값이 나오거나 버스 오류가 난다.
 * 그렇다고 모든 접근을 dword 로 강제하면 아래에 매달린 장치들과의 호환성이
 * 떨어지므로, 루트 버스일 때만 우회한다.
 * 동작 단계: pci_is_root_bus() 로 갈라, 루트 버스면
 * pci_generic_config_read32()(항상 dword 로 읽어 필요한 바이트만 잘라 준다),
 * 아니면 pci_generic_config_read()(요청한 폭 그대로 접근)를 부른다.
 * 두 함수 모두 안에서 bus->ops->map_bus 를 부르므로, 실제 주소 계산은
 * 결국 cdns_pci_map_bus() 로 간다.
 * 이 우회는 byte_access_allowed 가 false 인 SoC(J721E RC, J784S4 RC)에서만
 * 쓰인다 — probe 가 그때만 bridge->ops 를 cdns_ti_pcie_host_ops 로 바꾼다.
 * 실행 컨텍스트: 프로세스 컨텍스트. PCI config 접근은 상위에서 pci_lock 으로
 * 직렬화된다.
 * 호출자: PCI 코어의 config 접근 경로(pci_bus_read_config_ 계열).
 * 피호출자: pci_generic_config_read32() / pci_generic_config_read()
 * (drivers/pci/access.c).
 * 에러 경로: map_bus 가 NULL 을 돌려주면(존재하지 않는 장치) 아래 함수들이
 * PCIBIOS_DEVICE_NOT_FOUND 를 돌려주고, 코어가 그것을 "장치 없음" 으로 읽는다.
 *
 * 호출 체인:
 *   PCI 코어 → cdns_ti_pcie_host_ops.read → [이 함수]
 *     → pci_generic_config_read32() 또는 pci_generic_config_read()
 *       → cdns_pci_map_bus()
 */
static int cdns_ti_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 *value)
{
	/* [한국어] 루트 버스(= 루트 포트 자신의 config space)인지 판별한다.
	 * 판별에 bus->number 대신 이 헬퍼를 쓰는 이유는 상위 브리지 유무까지
	 * 고려해 주기 때문이다. */
	if (pci_is_root_bus(bus))
		/* [한국어] 루트 버스에는 항상 dword 로 접근한다. 이 함수가 내부에서 요청한 size
		 * 만큼 잘라 주므로 호출자는 차이를 느끼지 않는다. */
		return pci_generic_config_read32(bus, devfn, where, size,
						 value);

	/* [한국어] 루트 버스가 아니면 요청한 폭 그대로 접근한다.
	 * 아래 장치들은 바이트/워드 접근을 정상적으로 처리한다. */
	return pci_generic_config_read(bus, devfn, where, size, value);
}

/* [한국어]
 * cdns_ti_pcie_config_write - config space 쓰기 (바이트 접근 제약 우회)
 *
 * @bus: 접근 대상 버스.
 * @devfn: 장치/함수 번호.
 * @where: config space 안의 바이트 오프셋.
 * @size: 쓸 바이트 수 — 1, 2, 4.
 * @value: 써 넣을 값(하위 size 바이트만 의미가 있다).
 * @return: PCIBIOS_SUCCESSFUL 등 PCI 규약 코드.
 *
 * 위 read 쪽과 정확히 같은 이유, 같은 구조의 쓰기 판이다. 루트 버스면
 * pci_generic_config_write32() 로 우회한다. 이 함수는 read-modify-write 를
 * 하므로(dword 를 읽어 해당 바이트만 바꿔 되쓴다) 부분 쓰기가 안 되는
 * 하드웨어에서도 올바른 결과를 낸다. 다만 그 과정에서 write-1-to-clear
 * 성격의 비트를 의도치 않게 지울 수 있는데, 그것은 이 우회 방식의 알려진
 * 대가다.
 * 루트 버스가 아니면 pci_generic_config_write() 로 요청한 폭 그대로 쓴다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 상위에서 직렬화된다.
 * 호출자: PCI 코어의 config 쓰기 경로(pci_bus_write_config_ 계열).
 * 피호출자: pci_generic_config_write32() / pci_generic_config_write().
 * 에러 경로: 대상 장치가 없으면 PCIBIOS_DEVICE_NOT_FOUND 가 올라간다.
 *
 * 호출 체인:
 *   PCI 코어 → cdns_ti_pcie_host_ops.write → [이 함수]
 *     → pci_generic_config_write32() 또는 pci_generic_config_write()
 *       → cdns_pci_map_bus()
 */
static int cdns_ti_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 value)
{
	/* [한국어] 루트 버스인지 판별한다. */
	if (pci_is_root_bus(bus))
		/* [한국어] 루트 버스 쓰기도 dword 로 우회한다. 이 함수는 dword 를 읽어 해당
		 * 바이트만 바꿔 되쓰는 read-modify-write 를 한다. */
		return pci_generic_config_write32(bus, devfn, where, size,
						  value);

	/* [한국어] 아래 장치에는 요청한 폭 그대로 쓴다. */
	return pci_generic_config_write(bus, devfn, where, size, value);
}

/* [한국어]
 * cdns_ti_pcie_host_ops - 루트 버스에 dword 접근을 강제하는 config 접근자 표
 *
 * probe 가 byte_access_allowed == false 인 SoC 에서만 bridge->ops 에 꽂는다.
 * 그러지 않으면 bridge->ops 는 NULL 로 남고, 공통 코어의
 * cdns_pcie_host_setup() 이 자기 기본 표(cdns_pcie_host_ops)를 채운다.
 * map_bus 는 우회할 이유가 없어 공통 코어의 cdns_pci_map_bus() 를 그대로
 * 쓴다 — 바뀌는 것은 "그 주소에 몇 바이트로 접근하느냐" 뿐이다.
 * const 가 아닌 이유는 pci_ops 를 받는 필드가 비-const 포인터이기 때문이며,
 * 내용은 실행 중에 바뀌지 않는다.
 */
static struct pci_ops cdns_ti_pcie_host_ops = {
	/* [한국어] 주소 계산은 우회할 이유가 없어 공통 코어의 것을 그대로 쓴다. */
	.map_bus	= cdns_pci_map_bus,
	/* [한국어] 읽기만 루트 버스 우회판으로 바꾼다. */
	.read		= cdns_ti_pcie_config_read,
	/* [한국어] 쓰기도 루트 버스 우회판으로 바꾼다. */
	.write		= cdns_ti_pcie_config_write,
};

/* [한국어] J721E 를 RC 로 쓸 때의 특성표. 이 최초 판본은 Gen2 트레이닝 결함이
 * 있어 quirk_retrain_flag 가 필요하고, 루트 포트 config space 에 바이트 접근이
 * 안 되어 byte_access_allowed 를 false 로 둔다(그래서 probe 가 bridge->ops 를
 * cdns_ti_pcie_host_ops 로 갈아 끼운다). 링크 다운 비트는 LINK_DOWN(비트 1),
 * 최대 2레인. */
static const struct j721e_pcie_data j721e_pcie_rc_data = {
	/* [한국어] Root Complex 로 동작한다. */
	.mode = PCI_MODE_RC,
	/* [한국어] Gen2 트레이닝 결함이 있어 링크가 올라온 뒤 재트레이닝이 필요하다. */
	.quirk_retrain_flag = true,
	/* [한국어] 루트 포트 config space 에 바이트 접근이 안 된다 — probe 가 config
	 * 접근자를 dword 우회판으로 갈아 끼우게 만드는 값이다. */
	.byte_access_allowed = false,
	/* [한국어] 이 실리콘 세대의 링크 다운 비트는 SYS_2 그룹 비트 1 이다. */
	.linkdown_irq_regfield = LINK_DOWN,
	/* [한국어] 컨트롤러가 최대 2레인을 지원한다. */
	.max_lanes = 2,
};

/* [한국어] 같은 J721E 를 EP 로 쓸 때의 특성표. EP 에는 루트 포트 config
 * 접근이나 재트레이닝 개념이 없으므로 quirk 를 하나도 켜지 않는다.
 * 링크 다운 비트와 최대 레인 수는 RC 판과 같은 실리콘이므로 동일하다. */
static const struct j721e_pcie_data j721e_pcie_ep_data = {
	/* [한국어] Endpoint 로 동작한다. */
	.mode = PCI_MODE_EP,
	/* [한국어] 링크 다운 비트는 RC 판과 같은 실리콘이므로 동일하다. */
	.linkdown_irq_regfield = LINK_DOWN,
	/* [한국어] 최대 2레인. */
	.max_lanes = 2,
};

/* [한국어] J7200 RC. J721E 의 Gen2 재트레이닝 결함은 고쳐졌지만 LTSSM 의
 * Detect.Quiet 최소 지연 문제가 새로 생겨 quirk_detect_quiet_flag 를 켠다.
 * 인터럽트 취합기 배치가 바뀌어 링크 다운 비트가 J7200_LINK_DOWN(비트 10)
 * 으로 옮겨졌고, 루트 포트 바이트 접근이 가능해져 byte_access_allowed 가
 * true 다 — 따라서 config 접근 우회가 필요 없다. */
static const struct j721e_pcie_data j7200_pcie_rc_data = {
	/* [한국어] Root Complex. */
	.mode = PCI_MODE_RC,
	/* [한국어] LTSSM 의 Detect.Quiet 최소 지연을 늘려야 하는 판본이다. */
	.quirk_detect_quiet_flag = true,
	/* [한국어] 취합기 배치가 바뀌어 링크 다운이 비트 10 으로 옮겨졌다. */
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	/* [한국어] 루트 포트 바이트 접근이 가능해져 config 접근 우회가 필요 없다. */
	.byte_access_allowed = true,
	/* [한국어] 최대 2레인. */
	.max_lanes = 2,
};

/* [한국어] J7200 EP. Detect.Quiet quirk 에 더해 이 판본은 FLR 동작이 깨져
 * 있어 quirk_disable_flr 로 FLR 능력 비트를 감춘다. 이 파일의 아홉 개
 * 특성표 중 quirk_disable_flr 를 켜는 유일한 항목이다. */
static const struct j721e_pcie_data j7200_pcie_ep_data = {
	/* [한국어] Endpoint. */
	.mode = PCI_MODE_EP,
	/* [한국어] Detect.Quiet 지연 quirk 는 EP 에서도 필요하다. */
	.quirk_detect_quiet_flag = true,
	/* [한국어] 링크 다운 비트 10. */
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	/* [한국어] FLR 동작이 깨져 있어 능력 비트를 감춘다. 이 파일에서 이 quirk 를 쓰는
	 * 유일한 항목이다. */
	.quirk_disable_flr = true,
	/* [한국어] 최대 2레인. */
	.max_lanes = 2,
};

/* [한국어] AM64 RC. 저가형 SoC 라 PCIe 가 1레인뿐이다. quirk 는 없고
 * 바이트 접근이 가능하며, 취합기 배치는 J7200 계열을 따른다. */
static const struct j721e_pcie_data am64_pcie_rc_data = {
	/* [한국어] Root Complex. */
	.mode = PCI_MODE_RC,
	/* [한국어] 링크 다운 비트 10. */
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	/* [한국어] 바이트 접근 가능. */
	.byte_access_allowed = true,
	/* [한국어] 이 SoC 의 PCIe 는 1레인뿐이다. */
	.max_lanes = 1,
};

/* [한국어] AM64 EP. 1레인, quirk 없음. byte_access_allowed 는 EP 에서
 * 의미가 없어 초기화하지 않았고, C 의 정적 초기화 규칙에 따라 0 이 된다. */
static const struct j721e_pcie_data am64_pcie_ep_data = {
	/* [한국어] Endpoint. */
	.mode = PCI_MODE_EP,
	/* [한국어] 링크 다운 비트 10. */
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	/* [한국어] 1레인. */
	.max_lanes = 1,
};

/* [한국어] J784S4 RC. 이 파일에서 유일하게 4레인을 지원하는 SoC 라
 * j721e_pcie_set_lane_count() 가 스트랩 마스크를 GENMASK(9, 8) 로 넓히는
 * 분기에 걸리는 대상이다. J721E 와 같은 Gen2 재트레이닝 결함이 있어
 * quirk_retrain_flag 를 켜고, 루트 포트 바이트 접근도 안 된다. */
static const struct j721e_pcie_data j784s4_pcie_rc_data = {
	/* [한국어] Root Complex. */
	.mode = PCI_MODE_RC,
	/* [한국어] J721E 와 같은 Gen2 재트레이닝 결함이 있다. */
	.quirk_retrain_flag = true,
	/* [한국어] 루트 포트 바이트 접근 불가 — config 접근 우회가 필요하다. */
	.byte_access_allowed = false,
	/* [한국어] 링크 다운 비트 10. */
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	/* [한국어] 이 파일에서 유일한 4레인 SoC. set_lane_count 가 스트랩 마스크를
	 * GENMASK(9, 8) 로 넓히는 분기에 걸린다. */
	.max_lanes = 4,
};

/* [한국어] J784S4 EP. 4레인이지만 EP 쪽에는 quirk 가 필요 없다. */
static const struct j721e_pcie_data j784s4_pcie_ep_data = {
	/* [한국어] Endpoint. */
	.mode = PCI_MODE_EP,
	/* [한국어] 링크 다운 비트 10. */
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	/* [한국어] 4레인. */
	.max_lanes = 4,
};

/* [한국어] J722S RC. 1레인, quirk 없음, 바이트 접근 가능.
 * 이 SoC 는 EP 변형이 이 트리에 등록되어 있지 않다(아래 매칭 표에
 * "ti,j722s-pcie-ep" 항목이 없다). */
static const struct j721e_pcie_data j722s_pcie_rc_data = {
	/* [한국어] Root Complex. */
	.mode = PCI_MODE_RC,
	/* [한국어] 링크 다운 비트 10. */
	.linkdown_irq_regfield = J7200_LINK_DOWN,
	/* [한국어] 바이트 접근 가능. */
	.byte_access_allowed = true,
	/* [한국어] 1레인. */
	.max_lanes = 1,
};

/* [한국어]
 * of_j721e_pcie_match - devicetree compatible 문자열과 특성표의 대응 표
 *
 * 커널의 platform 버스가 DT 노드의 compatible 을 이 표와 맞춰 보고, 맞으면
 * j721e_pcie_probe() 를 부르면서 매칭된 항목의 .data 를 붙여 준다. probe 는
 * 초입에서 of_device_get_match_data() 로 그것을 꺼내 "이 SoC 는 어떤 특성인가"
 * 를 알아낸다. 즉 이 표가 다섯 SoC × RC/EP 조합을 하나의 드라이버로 묶는
 * 접합점이다.
 * 마지막 원소 {} 는 표의 끝을 알리는 sentinel 로 반드시 있어야 한다 —
 * 매칭 루프가 compatible 이 NULL 인 항목을 끝으로 본다.
 */
static const struct of_device_id of_j721e_pcie_match[] = {
	{
		/* [한국어] J721E 를 호스트(RC)로 쓰는 DT 노드. 최초 판본이라 Gen2 재트레이닝 quirk 와
		 * 루트 포트 dword 접근 우회가 모두 필요하다. */
		.compatible = "ti,j721e-pcie-host",
		/* [한국어] 위에서 정의한 J721E RC 특성표를 붙인다. */
		.data = &j721e_pcie_rc_data,
	},
	{
		/* [한국어] 같은 J721E 를 엔드포인트로 쓰는 DT 노드. */
		.compatible = "ti,j721e-pcie-ep",
		/* [한국어] J721E EP 특성표. */
		.data = &j721e_pcie_ep_data,
	},
	{
		/* [한국어] J7200 호스트. Detect.Quiet quirk 가 필요하고 링크 다운 비트가 바뀌었다. */
		.compatible = "ti,j7200-pcie-host",
		/* [한국어] J7200 RC 특성표. */
		.data = &j7200_pcie_rc_data,
	},
	{
		/* [한국어] J7200 엔드포인트. 여기에만 FLR quirk 가 추가로 붙는다. */
		.compatible = "ti,j7200-pcie-ep",
		/* [한국어] J7200 EP 특성표. */
		.data = &j7200_pcie_ep_data,
	},
	{
		/* [한국어] AM64 호스트. 1레인 저가형. */
		.compatible = "ti,am64-pcie-host",
		/* [한국어] AM64 RC 특성표. */
		.data = &am64_pcie_rc_data,
	},
	{
		/* [한국어] AM64 엔드포인트. */
		.compatible = "ti,am64-pcie-ep",
		/* [한국어] AM64 EP 특성표. */
		.data = &am64_pcie_ep_data,
	},
	{
		/* [한국어] J784S4 호스트. 이 파일에서 유일한 4레인 항목. */
		.compatible = "ti,j784s4-pcie-host",
		/* [한국어] J784S4 RC 특성표. */
		.data = &j784s4_pcie_rc_data,
	},
	{
		/* [한국어] J784S4 엔드포인트. */
		.compatible = "ti,j784s4-pcie-ep",
		/* [한국어] J784S4 EP 특성표. */
		.data = &j784s4_pcie_ep_data,
	},
	{
		/* [한국어] J722S 호스트. 대응하는 엔드포인트 항목은 이 트리에 없다. */
		.compatible = "ti,j722s-pcie-host",
		/* [한국어] J722S RC 특성표. */
		.data = &j722s_pcie_rc_data,
	},
	/* [한국어] 표의 끝을 알리는 sentinel. compatible 이 NULL 이라 매칭 루프가 여기서 멈춘다. */
	{},
};
/* [한국어] 위 매칭 표를 모듈 바이너리의 별도 섹션에 복사해 둔다.
 * depmod 가 그것을 읽어 modules.alias 를 만들고, udev 가 DT 노드를 보고
 * 이 모듈을 자동으로 올릴 수 있게 된다. 이것이 없으면 드라이버를 손으로
 * insmod 해야 한다. */
MODULE_DEVICE_TABLE(of, of_j721e_pcie_match);

/* [한국어]
 * j721e_pcie_probe - 드라이버 진입점. 컨트롤러를 처음부터 끝까지 세운다
 *
 * @pdev: platform 버스가 DT 노드로부터 만들어 준 디바이스. 레지스터 창,
 *        인터럽트, DT 속성이 모두 여기에 매달려 있다.
 * @return: 0 성공, 음수 errno 실패. 단 알 수 없는 mode 인 경우에만
 *          예외적으로 0 을 돌려주는데(아래 default 분기), 이는 사실상
 *          도달할 수 없는 방어 코드다.
 *
 * 왜 필요한가: PCIe 컨트롤러 하나를 쓰려면 (a) SoC 스트랩, (b) 전원 도메인,
 * (c) PHY, (d) 레퍼런스 클럭, (e) PERST# 해제, (f) IP 초기화가 정해진 순서로
 * 일어나야 한다. 순서를 어기면 링크가 올라오지 않는다. 이 함수가 그 순서를
 * 강제하는 곳이다.
 * 동작 단계:
 *   (1) compatible 로 SoC 특성표를 얻고 인스턴스 상태를 할당한다.
 *   (2) RC/EP 분기 — RC 면 pci_host_bridge 를 할당해 그 private 영역을
 *       cdns_pcie_rc 로 쓰고, EP 면 cdns_pcie_ep 를 따로 할당한다.
 *       어느 쪽이든 그 안의 struct cdns_pcie 에 dev 와 ops 를 꽂는다.
 *   (3) TI 글루 레지스터 두 창(intd_cfg, user_cfg)을 매핑한다.
 *   (4) num-lanes 속성을 읽어 범위를 검사하고, DMA 마스크를 48비트로 정한다.
 *   (5) link_state IRQ 번호를 미리 확보한다(실패 시 아직 되돌릴 것이 없다).
 *   (6) drvdata 를 매단 뒤 runtime PM 을 켜고 전원을 올린다. drvdata 를
 *       먼저 매다는 것이 중요하다 — 이후 ops 콜백들이 그것으로 우리를 찾는다.
 *   (7) j721e_pcie_ctrl_init() 로 스트랩을 프로그래밍한다(내부에서 전원을
 *       껐다 켠다).
 *   (8) 링크 다운 IRQ 핸들러를 등록하고 취합기에서 그 이벤트를 켠다.
 *   (9) 다시 RC/EP 분기 — RC 면 reset GPIO → PHY → refclk → 100ms 대기 →
 *       PERST# 해제 → cdns_pcie_host_setup(), EP 면 PHY →
 *       cdns_pcie_ep_setup().
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 코어의 probe 스레드). 여러 번
 * 잠들며, 이 드라이버는 PROBE_PREFER_ASYNCHRONOUS 를 쓰지 않으므로 기본
 * 동기 probe 다.
 * 호출자: 드라이버 코어(really_probe) — DT 매칭 결과로.
 * 피호출자: devm_* 자원 API, runtime PM, 이 파일의 ctrl_init/config_link_irq,
 * 공통 코어의 cdns_pcie_init_phy()/cdns_pcie_host_setup()/cdns_pcie_ep_setup().
 * 에러 경로: 두 개의 goto 라벨로 갈린다. err_pcie_setup 은 PHY 를 켠 뒤에
 * 실패한 경우로, cdns_pcie_disable_phy() 를 직접 불러야 한다 —
 * cdns_pcie_init_phy() 가 확보와 켜기를 함께 하지만 그 "켜기" 는 devres 로
 * 자동 정리되지 않기 때문이다. err_get_sync 는 그 이전 단계의 실패로,
 * runtime PM 만 되돌리면 된다. 나머지 자원은 모두 devm_* 이라 코어가 푼다.
 *
 * 호출 체인:
 *   드라이버 코어 → [이 함수] → j721e_pcie_ctrl_init() →
 *   cdns_pcie_init_phy() → cdns_pcie_host_setup() 또는 cdns_pcie_ep_setup()
 */
static int j721e_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 자원 관리에 쓸 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] DT 속성(num-lanes 등)을 읽을 이 인스턴스의 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] RC 모드에서만 쓰는 PCI 호스트 브리지. EP 모드에서는 초기화되지 않은 채 남는데,
	 * 그 경로에서 참조하지 않으므로 문제가 없다. */
	struct pci_host_bridge *bridge;
	/* [한국어] compatible 로 결정된 SoC 특성표. const 정적 데이터를 가리킨다. */
	const struct j721e_pcie_data *data;
	/* [한국어] RC/EP 어느 쪽이든 공통 코어에 넘길 컨트롤러 구조체 포인터. */
	struct cdns_pcie *cdns_pcie;
	/* [한국어] 이 파일이 쓸 인스턴스 상태. */
	struct j721e_pcie *pcie;
	/* [한국어] RC 모드의 Cadence 루트 컴플렉스 구조체. NULL 로 초기화하는 이유는
	 * 아래 IS_ENABLED(CONFIG_PCI_J721E_HOST) 분기 때문에 컴파일러가 '초기화되지
	 * 않은 채 쓰일 수 있다' 고 경고하는 것을 막기 위해서다. */
	struct cdns_pcie_rc *rc = NULL;
	/* [한국어] EP 모드의 Cadence 엔드포인트 구조체. 위와 같은 이유로 NULL 초기화. */
	struct cdns_pcie_ep *ep = NULL;
	/* [한국어] ioremap 결과를 잠시 받는 임시 변수. 두 창에 재사용한다. */
	void __iomem *base;
	/* [한국어] DT 에서 읽은 레인 수. */
	u32 num_lanes;
	/* [한국어] data->mode 를 u32 로 복사한 값. switch 문에서 쓴다. */
	u32 mode;
	/* [한국어] 각 단계의 반환값. */
	int ret;
	/* [한국어] 링크 다운 인터럽트의 IRQ 번호. */
	int irq;

	/* [한국어] platform 버스가 매칭해 둔 of_device_id.data 를 꺼낸다.
	 * 이 한 줄이 '어느 SoC 의 어느 모드인가' 를 결정한다. */
	data = of_device_get_match_data(dev);
	/* [한국어] 매칭 데이터가 없다는 것은 표에 .data 를 빠뜨렸다는 뜻이라 코드 버그다. */
	if (!data)
		/* [한국어] 매칭 데이터가 없다는 것은 이 파일의 of_device_id 표에 .data 를 빠뜨렸다는
		 * 뜻이므로 설정 오류로 처리한다. */
		return -EINVAL;

	/* [한국어] enum 을 u32 로 캐스팅한다. 아래 switch 의 case 라벨과 타입을 맞추려는 것이다. */
	mode = (u32)data->mode;

	/* [한국어] 인스턴스 상태를 0 으로 채워 할당한다. devm_ 이므로 probe 가 실패하거나
	 * 디바이스가 사라질 때 코어가 자동으로 해제한다. */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족. 아직 확보한 자원이 없어 그냥 반환하면 된다. */
	if (!pcie)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] 여기서부터 RC 와 EP 의 자료구조가 완전히 갈린다. */
	switch (mode) {
	/* [한국어] Root Complex 로 동작하는 경우. */
	case PCI_MODE_RC:
		/* [한국어] 호스트 지원이 빌드에 들어 있지 않은데 DT 가 호스트 모드를 요구하는 경우. */
		if (!IS_ENABLED(CONFIG_PCI_J721E_HOST))
			/* [한국어] 이 커널로는 처리할 수 없는 장치이므로 -ENODEV 로 물러난다. */
			return -ENODEV;

		/* [한국어] PCI 호스트 브리지를 할당하면서 private 영역을 cdns_pcie_rc 크기만큼 더 잡는다.
		 * 이렇게 하면 브리지와 rc 가 한 덩어리 메모리에 놓여,
		 * pci_host_bridge_priv() 와 pci_host_bridge_from_priv() 로 서로를 오갈 수 있다.
		 * 이 함수는 안에서 DT 의 ranges/bus-range 도 파싱해 bridge->windows 를 채운다. */
		bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
		/* [한국어] 메모리 부족. */
		if (!bridge)
			/* [한국어] 메모리 부족. */
			return -ENOMEM;

		/* [한국어] 루트 포트 config space 에 바이트 접근이 안 되는 SoC 인 경우. */
		if (!data->byte_access_allowed)
			/* [한국어] 그런 SoC 에서만 config 접근자를 dword 우회판으로 갈아 끼운다.
			 * 여기서 채워 두면 나중에 cdns_pcie_host_setup() 이 기본 표로 덮어쓰지 않는다
			 * (그 함수는 bridge->ops 가 비어 있을 때만 채운다). */
			bridge->ops = &cdns_ti_pcie_host_ops;
		/* [한국어] 브리지 뒤에 붙여 둔 private 영역을 cdns_pcie_rc 로 해석한다. */
		rc = pci_host_bridge_priv(bridge);
		/* [한국어] Gen2 재트레이닝 quirk 를 공통 코어가 볼 수 있는 자리로 옮긴다.
		 * 실제 사용처는 pcie-cadence-host-common.c 의 cdns_pcie_host_start_link(). */
		rc->quirk_retrain_flag = data->quirk_retrain_flag;
		/* [한국어] Detect.Quiet 지연 quirk 도 마찬가지로 옮긴다.
		 * 실제 사용처는 pcie-cadence-host.c 의 cdns_pcie_host_link_setup(). */
		rc->quirk_detect_quiet_flag = data->quirk_detect_quiet_flag;

		/* [한국어] cdns_pcie_rc 의 첫 멤버가 struct cdns_pcie 이므로 그 주소를 그대로 쓴다.
		 * 반대 방향은 container_of 로 되돌린다. */
		cdns_pcie = &rc->pcie;
		/* [한국어] 공통 코어가 로그와 devm 자원 관리에 쓸 device 를 꽂는다. */
		cdns_pcie->dev = dev;
		/* [한국어] 링크 제어 콜백 표를 꽂는다. 이 순간부터 공통 코어의 디스패처들이
		 * 이 파일의 start_link/stop_link/link_up 으로 내려올 수 있게 된다. */
		cdns_pcie->ops = &j721e_pcie_ops;
		/* [한국어] 우리 상태에서도 공통 코어 구조체를 찾아갈 수 있게 역참조를 저장한다. */
		pcie->cdns_pcie = cdns_pcie;
		/* [한국어] RC 자료구조 준비 완료. 아래 공통 처리로 빠져나간다. */
		break;
	/* [한국어] Endpoint 로 동작하는 경우. */
	case PCI_MODE_EP:
		/* [한국어] 엔드포인트 지원이 빌드에 들어 있지 않은 경우. */
		if (!IS_ENABLED(CONFIG_PCI_J721E_EP))
			/* [한국어] -ENODEV 로 물러난다. */
			return -ENODEV;

		/* [한국어] EP 에는 pci_host_bridge 가 없으므로 cdns_pcie_ep 를 직접 할당한다.
		 * EP 는 PCI 버스를 열거하는 쪽이 아니라 열거당하는 쪽이라 브리지가 필요 없다. */
		ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
		/* [한국어] 메모리 부족. */
		if (!ep)
			/* [한국어] 메모리 부족. */
			return -ENOMEM;

		/* [한국어] Detect.Quiet quirk 를 EP 구조체로 옮긴다. */
		ep->quirk_detect_quiet_flag = data->quirk_detect_quiet_flag;
		/* [한국어] FLR 능력을 감출지 여부를 EP 구조체로 옮긴다. RC 에는 없는 필드다. */
		ep->quirk_disable_flr = data->quirk_disable_flr;

		/* [한국어] cdns_pcie_ep 의 첫 멤버인 struct cdns_pcie 의 주소. */
		cdns_pcie = &ep->pcie;
		/* [한국어] RC 와 똑같이 device 를 꽂는다. */
		cdns_pcie->dev = dev;
		/* [한국어] RC 와 똑같은 콜백 표를 쓴다 — 링크 제어 방법은 모드와 무관하게 같다. */
		cdns_pcie->ops = &j721e_pcie_ops;
		/* [한국어] 역참조 저장. */
		pcie->cdns_pcie = cdns_pcie;
		/* [한국어] EP 자료구조 준비 완료. 아래 공통 처리로 빠져나간다. */
		break;
	/* [한국어] RC 도 EP 도 아닌 값. 특성표의 mode 는 이 둘뿐이라 사실상 도달할 수 없는
	 * 방어 코드다. */
	default:
		/* [한국어] 그래도 조용히 넘어가지 않도록 오류를 남긴다. */
		dev_err(dev, "INVALID device type %d\n", mode);
		/* [한국어] 음수 errno 가 아니라 0 을 돌려주는 점이 특이하다 — probe 가 '성공' 으로
		 * 끝나 아무것도 초기화되지 않은 채 바인딩이 유지된다. 상류 코드가 그렇게
		 * 되어 있으며, 왜 그런지에 대한 근거는 이 트리에서 찾지 못했다. */
		return 0;
	}

	/* [한국어] 이후 remove/PM 이 모드를 다시 판별할 수 있도록 복사해 둔다. */
	pcie->mode = mode;
	/* [한국어] 이 SoC 의 링크 다운 비트 위치를 복사해 둔다. IRQ 핸들러가 인터럽트
	 * 컨텍스트에서 이 값을 읽으므로 미리 확정해 두어야 한다. */
	pcie->linkdown_irq_regfield = data->linkdown_irq_regfield;

	/* [한국어] DT 의 reg 항목 중 이름이 'intd_cfg' 인 창을 ioremap 한다.
	 * 이름으로 찾는 이유는 DT 에서 창 순서가 바뀌어도 깨지지 않게 하려는 것이다. */
	base = devm_platform_ioremap_resource_byname(pdev, "intd_cfg");
	/* [한국어] ioremap 실패는 ERR_PTR 로 온다. */
	if (IS_ERR(base))
		/* [한국어] ERR_PTR 에서 errno 를 뽑아 돌려준다. */
		return PTR_ERR(base);
	/* [한국어] 인터럽트 취합기 접근자들이 쓸 base 를 확정한다. */
	pcie->intd_cfg_base = base;

	/* [한국어] 같은 방식으로 'user_cfg' 창을 매핑한다. 링크 제어/상태 레지스터가 여기 있다. */
	base = devm_platform_ioremap_resource_byname(pdev, "user_cfg");
	/* [한국어] ioremap 실패. */
	if (IS_ERR(base))
		/* [한국어] ERR_PTR 에서 errno 를 뽑아 돌려준다. */
		return PTR_ERR(base);
	/* [한국어] 링크 제어 접근자들이 쓸 base 를 확정한다. 이 시점 이후에야
	 * start_link/link_up 콜백이 안전하게 불릴 수 있다. */
	pcie->user_cfg_base = base;

	/* [한국어] DT 의 'num-lanes' 를 읽는다. 속성이 없으면 음수 errno 를 돌려준다. */
	ret = of_property_read_u32(node, "num-lanes", &num_lanes);
	/* [한국어] 속성이 없거나(ret != 0) 이 SoC 가 감당 못 하는 값이면 걸린다. */
	if (ret || num_lanes > data->max_lanes) {
		/* [한국어] 부팅을 막을 일은 아니므로 경고만 남긴다. */
		dev_warn(dev, "num-lanes property not provided or invalid, setting num-lanes to 1\n");
		/* [한국어] 가장 안전한 값인 x1 로 낮춘다. 링크는 느려도 올라온다. */
		num_lanes = 1;
	}

	/* [한국어] 검증을 마친 레인 수를 저장한다. j721e_pcie_set_lane_count() 가 읽는다. */
	pcie->num_lanes = num_lanes;
	/* [한국어] SoC 의 최대 레인 수도 저장한다. 스트랩 마스크 폭 결정에 쓰인다. */
	pcie->max_lanes = data->max_lanes;

	/* [한국어] 이 컨트롤러 아래 장치들이 쓸 DMA 주소 폭을 48비트로 선언한다.
	 * K3 계열 SoC 의 PCIe 인바운드 주소 변환이 48비트까지 다루기 때문이다.
	 * 스트리밍과 코히런트 마스크를 한 번에 설정하며, 실패하면 이 플랫폼에서
	 * DMA 를 제대로 할 수 없다는 뜻이므로 probe 를 접는다. */
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48)))
		/* [한국어] 이 플랫폼에서 48비트 DMA 를 설정할 수 없다는 뜻이므로 물러난다. */
		return -EINVAL;

	/* [한국어] DT 의 interrupt-names 중 'link_state' 인터럽트 번호를 얻는다.
	 * 아직 핸들러를 등록하지는 않고 번호만 확보한다. */
	irq = platform_get_irq_byname(pdev, "link_state");
	/* [한국어] 음수는 실패(EPROBE_DEFER 포함). 인터럽트 컨트롤러가 아직 안 올라왔으면
	 * EPROBE_DEFER 가 오고, 코어가 나중에 probe 를 다시 시도한다. */
	if (irq < 0)
		/* [한국어] 이 시점까지 확보한 자원은 전부 devm_ 이라 그냥 반환해도 코어가 정리한다. */
		return irq;

	/* [한국어] 인스턴스 상태를 device 에 매단다. 이 줄이 반드시 아래 runtime PM 과
	 * ctrl_init 보다 앞에 와야 한다 — 그 뒤로 불릴 수 있는 ops 콜백들이
	 * dev_get_drvdata() 로 우리를 찾기 때문이다. */
	dev_set_drvdata(dev, pcie);
	/* [한국어] runtime PM 을 활성화한다. 이 줄 이후에야 pm_runtime_get/put 이 의미를 갖는다.
	 * ctrl_init 이 내부에서 put_sync/get_sync 로 전원을 껐다 켜므로 필수 전제다. */
	pm_runtime_enable(dev);
	/* [한국어] 전원 도메인을 켜고 사용 카운트를 올린다. _sync 라 실제로 켜질 때까지 기다린다. */
	ret = pm_runtime_get_sync(dev);
	/* [한국어] 전원을 못 켠 경우. */
	if (ret < 0) {
		/* [한국어] dev_err_probe 는 EPROBE_DEFER 일 때 로그 수준을 낮춰 주므로,
		 * 반복되는 defer 로 로그가 넘치지 않는다. */
		dev_err_probe(dev, ret, "pm_runtime_get_sync failed\n");
		/* [한국어] 이 시점 이후의 실패는 runtime PM 을 되돌려야 하므로 goto 로 모은다. */
		goto err_get_sync;
	}

	/* [한국어] CTRL_MMR 스트랩을 프로그래밍한다. 이 함수가 내부에서 전원을 껐다 켜므로,
	 * 위에서 먼저 runtime PM 을 켜 두어야 카운트가 맞는다. */
	ret = j721e_pcie_ctrl_init(pcie);
	/* [한국어] 스트랩 설정 실패. */
	if (ret < 0) {
		/* [한국어] 실패 사유를 남긴다. */
		dev_err_probe(dev, ret, "j721e_pcie_ctrl_init failed\n");
		/* [한국어] 스트랩 실패도 같은 정리 경로로 간다. */
		goto err_get_sync;
	}

	/* [한국어] 링크 다운 핸들러를 등록한다. flags 가 0 이라 공유가 아닌 일반 하드IRQ 다.
	 * 마지막 인자 pcie 가 핸들러의 dev_id 로 전달된다.
	 * devm_ 판이라 디바이스가 사라질 때 코어가 자동으로 뗀다. */
	ret = devm_request_irq(dev, irq, j721e_pcie_link_irq_handler, 0,
			       "j721e-pcie-link-down-irq", pcie);
	/* [한국어] IRQ 등록 실패. */
	if (ret < 0) {
		/* [한국어] 어느 IRQ 였는지 함께 남긴다. */
		dev_err_probe(dev, ret, "failed to request link state IRQ %d\n", irq);
		/* [한국어] IRQ 등록 실패도 같은 정리 경로. */
		goto err_get_sync;
	}

	/* [한국어] 핸들러 등록이 끝난 뒤에야 취합기에서 이벤트를 켠다.
	 * 순서가 반대면 핸들러 없는 인터럽트가 올라온다. */
	j721e_pcie_config_link_irq(pcie);

	/* [한국어] 두 번째 모드 분기 — 이번에는 하드웨어를 실제로 켜는 순서를 다룬다.
	 * 앞의 분기가 자료구조를 갈랐다면 여기는 전원 시퀀스를 가른다. */
	switch (mode) {
	/* [한국어] Root Complex 경로. refclk 와 PERST# 을 우리가 공급해야 한다. */
	case PCI_MODE_RC:
		/* [한국어] PERST# 용 GPIO 를 확보한다. GPIOD_OUT_LOW 라 확보하는 순간 리셋이 걸린
		 * 상태로 시작한다 — 아래에서 100ms 뒤에 해제한다.
		 * _optional 이라 DT 에 없으면 NULL 이 오고 그것은 오류가 아니다. */
		pcie->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
		/* [한국어] GPIO 확보 실패. optional 이라 '없음' 은 NULL 로 오고 오류가 아니다.
		 * 여기 걸리는 것은 DT 에 있는데 확보에 실패한 경우다. */
		if (IS_ERR(pcie->reset_gpio)) {
			/* [한국어] EPROBE_DEFER 가능성이 있어 dev_err_probe 를 쓴다. */
			ret = dev_err_probe(dev, PTR_ERR(pcie->reset_gpio),
					    "Failed to get reset GPIO\n");
			/* [한국어] 아직 PHY 를 켜기 전이므로 err_get_sync 로 간다. */
			goto err_get_sync;
		}

		/* [한국어] PHY 를 확보하고 켠다. 이 한 번의 호출이 phy_get, phy_init, phy_power_on
		 * 까지 한다는 점이 중요하다 — 그중 '켜기' 는 devres 로 자동 정리되지 않아,
		 * 이후 실패 경로에서 cdns_pcie_disable_phy() 를 손으로 불러야 한다. */
		ret = cdns_pcie_init_phy(dev, cdns_pcie);
		/* [한국어] PHY 초기화 실패. */
		if (ret) {
			/* [한국어] PHY 드라이버가 아직 안 올라왔으면 EPROBE_DEFER 가 온다. */
			dev_err_probe(dev, ret, "Failed to init phy\n");
			/* [한국어] cdns_pcie_init_phy 가 실패했다면 PHY 는 켜지지 않은 상태이므로
			 * err_get_sync 로 간다(PHY 를 끌 필요가 없다). */
			goto err_get_sync;
		}

		/* [한국어] 레퍼런스 클럭을 얻는 즉시 켠다. optional 이라 DT 에 없으면 NULL 이고,
		 * 그때는 보드가 외부 클럭 발생기나 ACSPCIE 출력을 쓴다는 뜻이다.
		 * _enabled 판이라 devres 가 해제 시 자동으로 꺼 준다. */
		pcie->refclk = devm_clk_get_optional_enabled(dev, "pcie_refclk");
		/* [한국어] 클럭 확보/활성화 실패. */
		if (IS_ERR(pcie->refclk)) {
			/* [한국어] 여기부터는 PHY 가 켜져 있으므로 err_pcie_setup 으로 가야 한다. */
			ret = dev_err_probe(dev, PTR_ERR(pcie->refclk),
					    "failed to enable pcie_refclk\n");
			/* [한국어] 여기서부터는 PHY 가 켜져 있으므로 err_pcie_setup 으로 가야 한다. */
			goto err_pcie_setup;
		}

		/*
		 * Section 2.2 of the PCI Express Card Electromechanical
		 * Specification (Revision 5.1) mandates that the deassertion
		 * of the PERST# signal should be delayed by 100 ms (TPVPERL).
		 * This shall ensure that the power and the reference clock
		 * are stable.
		 */
		if (pcie->reset_gpio) {
			/* [한국어] 위 영문 주석의 T_PVPERL — 전원과 레퍼런스 클럭이 안정될 시간을 준다.
			 * msleep 이므로 이 지점은 반드시 잠들 수 있는 컨텍스트여야 한다. */
			msleep(PCIE_T_PVPERL_MS);
			/* [한국어] PERST# 을 해제한다(논리값 1). 이 시점에는 전원과 refclk 가 모두 안정된
			 * 뒤라 규격이 요구하는 조건을 만족한다. 이제 카드가 부팅을 시작한다. */
			gpiod_set_value_cansleep(pcie->reset_gpio, 1);
		}

		/* [한국어] 컴파일 타임 상수 검사. 호스트 지원이 꺼진 빌드에서는 이 블록이 통째로
		 * 사라져 cdns_pcie_host_setup() 에 대한 링크 참조도 생기지 않는다.
		 * (위쪽 case 진입부에서 이미 -ENODEV 로 걸러지지만, 링커 관점에서는
		 * 이 IS_ENABLED 가 있어야 심볼이 빠진다.) */
		if (IS_ENABLED(CONFIG_PCI_J721E_HOST)) {
			/* [한국어] Cadence 공통 코어에 넘긴다. 여기서 링크를 올리고, 인바운드/아웃바운드
			 * 주소 변환을 세우고, pci_host_probe() 로 버스를 열거한다.
			 * 이 호출 안에서 우리 start_link/link_up 콜백이 되불린다. */
			ret = cdns_pcie_host_setup(rc);
			/* [한국어] 공통 코어 초기화 실패. PHY 가 켜진 뒤이므로 err_pcie_setup 으로 간다. */
			if (ret < 0)
				/* [한국어] PHY 가 켜진 뒤의 실패이므로 err_pcie_setup 으로 간다. */
				goto err_pcie_setup;
		}

		/* [한국어] RC 전원/링크 시퀀스 완료. */
		break;
	/* [한국어] Endpoint 경로. refclk 와 PERST# 은 상대 호스트가 공급하므로 건드리지 않는다. */
	case PCI_MODE_EP:
		/* [한국어] EP 모드도 PHY 는 필요하다. 다만 refclk 와 PERST# 은 상대가 공급한다. */
		ret = cdns_pcie_init_phy(dev, cdns_pcie);
		/* [한국어] PHY 초기화 실패. */
		if (ret) {
			/* [한국어] EPROBE_DEFER 가능. */
			dev_err_probe(dev, ret, "Failed to init phy\n");
			/* [한국어] PHY 초기화 실패는 아직 끌 것이 없어 err_get_sync 로 간다. */
			goto err_get_sync;
		}

		/* [한국어] EP 지원이 꺼진 빌드에서는 이 블록이 사라진다. */
		if (IS_ENABLED(CONFIG_PCI_J721E_EP)) {
			/* [한국어] Cadence 공통 코어의 EP 초기화. BAR, config space, EPC 등록을 처리한다. */
			ret = cdns_pcie_ep_setup(ep);
			/* [한국어] 실패 시 PHY 를 꺼야 하므로 err_pcie_setup 으로 간다. */
			if (ret < 0)
				/* [한국어] PHY 가 켜진 뒤의 실패이므로 err_pcie_setup 으로 간다. */
				goto err_pcie_setup;
		}

		/* [한국어] EP 전원 시퀀스 완료. */
		break;
	}

	/* [한국어] 여기까지 왔으면 컨트롤러가 살아 있고 버스 열거까지 끝났다. */
	return 0;

/* [한국어] PHY 를 켠 뒤에 실패한 경로. cdns_pcie_init_phy 의 '켜기' 는 devres 대상이
 * 아니므로 여기서 직접 꺼야 한다. */
err_pcie_setup:
	/* [한국어] PHY 를 끈다. 그 다음 아래 라벨로 흘러 내려가 runtime PM 도 정리한다. */
	cdns_pcie_disable_phy(cdns_pcie);

/* [한국어] PHY 를 켜기 전에 실패한 경로. runtime PM 만 되돌리면 된다. */
err_get_sync:
	/* [한국어] probe 초입에서 올린 사용 카운트를 내린다. */
	pm_runtime_put(dev);
	/* [한국어] runtime PM 을 비활성화해 probe 이전 상태로 되돌린다. */
	pm_runtime_disable(dev);

	/* [한국어] 실패 사유를 드라이버 코어에 전달한다. */
	return ret;
}

/* [한국어]
 * j721e_pcie_remove - 드라이버를 내리며 probe 가 한 일을 역순으로 되돌린다
 *
 * @pdev: 대상 platform 디바이스. drvdata 로 우리 상태를 되찾는다.
 * @return: 없음. 최신 커널의 platform remove 콜백은 void 다 — 실패를
 *          보고해 봐야 코어가 할 수 있는 일이 없기 때문이다.
 *
 * 왜 필요한가: PCIe 컨트롤러는 아래에 장치들이 매달려 있고 그것들이 DMA 를
 * 하고 있을 수 있다. 그래서 순서가 중요하다 — 먼저 열거된 장치들을 걷어내
 * DMA 를 멈추고, 그 다음 링크를 끊고, 마지막에 하드웨어를 끈다.
 * 동작 단계:
 *   (1) 모드에 따라 cdns_pcie_host_disable() 또는 cdns_pcie_ep_disable() —
 *       PCI 장치 열거를 걷어내고 링크를 끊는다(이 안에서 ops->stop_link 가
 *       불린다).
 *   (2) PERST# 을 다시 건다(GPIO 를 0 으로).
 *   (3) PHY 를 끈다. init_phy 의 "켜기" 는 devres 대상이 아니므로 명시적으로
 *       꺼야 한다.
 *   (4) 링크 다운 IRQ 를 취합기에서 끈다.
 *   (5) runtime PM 사용 카운트를 내리고 runtime PM 을 끈다.
 * 이 순서에서 (1)이 (2)(3)보다 먼저인 것이 핵심이다 — 링크를 먼저 끊지 않고
 * PHY 를 끄면 진행 중인 트랜잭션이 버스 오류를 낸다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: 드라이버 코어(모듈 rmmod 또는 디바이스 언바인드). 다만 이 드라이버는
 * driver.suppress_bind_attrs = true 라서 sysfs 를 통한 수동 언바인드는 막혀
 * 있고, 실질적으로는 모듈을 내릴 때만 불린다.
 * 피호출자: 공통 코어의 host/ep disable, gpiod, 이 파일의 disable_link_irq,
 * runtime PM.
 * 에러 경로: 없다. 정리 경로에서는 실패해도 계속 진행한다.
 *
 * 호출 체인:
 *   드라이버 코어 → [이 함수] → cdns_pcie_host_disable() 또는
 *   cdns_pcie_ep_disable() → (내부에서) cdns_pcie_stop_link()[디스패처]
 *     → j721e_pcie_stop_link()
 */
static void j721e_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] probe 가 매달아 둔 인스턴스 상태를 되찾는다. */
	struct j721e_pcie *pcie = platform_get_drvdata(pdev);
	/* [한국어] 공통 코어 구조체. 여기서 container_of 로 rc 또는 ep 를 되찾는다. */
	struct cdns_pcie *cdns_pcie = pcie->cdns_pcie;
	/* [한국어] runtime PM 정리에 쓸 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] EP 모드에서 쓸 포인터. */
	struct cdns_pcie_ep *ep;
	/* [한국어] RC 모드에서 쓸 포인터. */
	struct cdns_pcie_rc *rc;

	/* [한국어] 호스트 지원이 빌드에 들어 있고 이 인스턴스가 RC 인 경우.
	 * 두 조건을 AND 로 묶은 덕분에, 호스트 지원이 꺼진 빌드에서는
	 * cdns_pcie_host_disable 참조가 통째로 사라진다. */
	if (IS_ENABLED(CONFIG_PCI_J721E_HOST) &&
	    /* [한국어] 두 조건을 AND 로 묶어, 호스트 지원이 꺼진 빌드에서는
	     * cdns_pcie_host_disable 참조 자체가 사라지게 한다. */
	    pcie->mode == PCI_MODE_RC) {
		/* [한국어] cdns_pcie 를 품은 cdns_pcie_rc 로 거슬러 올라간다.
		 * (파일 상단의 cdns_pcie_to_rc 매크로와 같은 일이지만, 이 함수는 EP 도
		 * 다루므로 대칭을 위해 양쪽 모두 container_of 를 직접 쓴다.) */
		rc = container_of(cdns_pcie, struct cdns_pcie_rc, pcie);
		/* [한국어] PCI 장치 열거를 걷어내고 링크를 끊는다.
		 * 이 안에서 우리 stop_link 콜백이 되불린다.
		 * 순서상 반드시 PHY 를 끄기 전이어야 한다. */
		cdns_pcie_host_disable(rc);
	/* [한국어] RC 가 아니면 EP 다. EP 지원이 꺼진 빌드에서는 이 블록도 사라진다. */
	} else if (IS_ENABLED(CONFIG_PCI_J721E_EP)) {
		/* [한국어] cdns_pcie 를 품은 cdns_pcie_ep 로 거슬러 올라간다. */
		ep = container_of(cdns_pcie, struct cdns_pcie_ep, pcie);
		/* [한국어] EP 쪽 정리. EPC 등록을 걷어낸다. */
		cdns_pcie_ep_disable(ep);
	}

	/* [한국어] PERST# 을 다시 건다(논리값 0). 위 disable 이 링크를 이미 끊은 뒤이므로
	 * 순서가 안전하다. reset_gpio 가 NULL 이어도 gpiod API 가 조용히 넘어간다. */
	gpiod_set_value_cansleep(pcie->reset_gpio, 0);

	/* [한국어] PHY 를 끈다. init_phy 의 '켜기' 는 devres 로 자동 정리되지 않기 때문이다. */
	cdns_pcie_disable_phy(cdns_pcie);
	/* [한국어] 취합기에서 링크 다운 이벤트를 끈다. 핸들러 자체는 devres 가 뗀다. */
	j721e_pcie_disable_link_irq(pcie);
	/* [한국어] probe 에서 올린 runtime PM 사용 카운트를 내려 전원 도메인을 놓아 준다. */
	pm_runtime_put(dev);
	/* [한국어] runtime PM 을 비활성화한다. */
	pm_runtime_disable(dev);
}

/* [한국어]
 * j721e_pcie_suspend_noirq - 시스템 절전 진입 시 하드웨어를 내린다
 *
 * @dev: 이 컨트롤러의 device. drvdata 로 우리 상태를 되찾는다.
 * @return: 항상 0. 절전을 막을 이유가 없다.
 *
 * 왜 필요한가: 시스템이 잠들면 컨트롤러 전원 도메인이 내려가는데, 그 전에
 * 아래 슬롯의 카드도 정상적으로 리셋 상태로 넣고 클럭을 끊어 두어야 복귀 시
 * 상태가 예측 가능해진다.
 * 동작 단계: RC 모드면 (1) PERST# 을 걸고(GPIO 0) (2) refclk 를 끈다.
 * 순서가 중요하다 — 클럭을 먼저 끊고 리셋을 걸면 카드가 클럭 없는 상태에서
 * 리셋 에지를 받게 되어 규격을 벗어난다. EP 모드에서는 refclk 도 PERST# 도
 * 상대편이 공급하므로 건너뛴다. 그 다음 모드와 무관하게 (3) PHY 를 끈다.
 * 실행 컨텍스트: 시스템 절전의 "noirq" 단계 — 인터럽트가 이미 꺼진 뒤라
 * 다른 드라이버의 인터럽트가 끼어들지 않는다. 이 단계를 고른 이유는 PCIe
 * 컨트롤러가 아래 장치들보다 늦게 내려가야 하기 때문이다.
 * 여전히 프로세스 컨텍스트이므로 잠들 수는 있다.
 * 호출자: PM 코어. DEFINE_NOIRQ_DEV_PM_OPS 로 등록된 suspend_noirq 슬롯.
 * 피호출자: gpiod_set_value_cansleep(), clk_disable_unprepare(),
 * cdns_pcie_disable_phy().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PM 코어(dpm_suspend_noirq) → [이 함수] → cdns_pcie_disable_phy()
 */
static int j721e_pcie_suspend_noirq(struct device *dev)
{
	/* [한국어] PM 코어가 넘긴 device 에서 인스턴스 상태를 되찾는다. */
	struct j721e_pcie *pcie = dev_get_drvdata(dev);

	/* [한국어] RC 모드에서만 refclk 와 PERST# 을 우리가 관리한다. */
	if (pcie->mode == PCI_MODE_RC) {
		/* [한국어] PERST# 을 걸어 카드를 리셋 상태로 넣는다. 클럭을 끊기 전에 해야
		 * 카드가 클럭 있는 상태에서 리셋 에지를 받는다. */
		gpiod_set_value_cansleep(pcie->reset_gpio, 0);
		/* [한국어] 레퍼런스 클럭을 끈다. 절전 중 불필요한 전력을 아낀다.
		 * refclk 가 NULL 이어도 clk API 가 조용히 넘어간다. */
		clk_disable_unprepare(pcie->refclk);
	}

	/* [한국어] PHY 를 끈다. RC/EP 공통이다. resume 에서 cdns_pcie_enable_phy 로 되살린다. */
	cdns_pcie_disable_phy(pcie->cdns_pcie);

	/* [한국어] 절전 진입을 막지 않는다. */
	return 0;
}

/* [한국어]
 * j721e_pcie_resume_noirq - 절전에서 깨어나며 컨트롤러를 다시 세운다
 *
 * @dev: 이 컨트롤러의 device. drvdata 로 우리 상태를 되찾는다.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 왜 필요한가: 절전 중 컨트롤러 전원 도메인이 내려가면 CTRL_MMR 스트랩 래치,
 * 취합기 enable 비트, PHY 상태, 링크가 전부 사라진다. 그래서 이 함수는
 * probe 가 하는 일 중 "하드웨어 상태" 부분을 거의 그대로 다시 한다.
 * probe 와 다른 점은 메모리 할당과 자원 확보를 반복하지 않는다는 것뿐이다.
 * 동작 단계:
 *   (1) j721e_pcie_ctrl_init() — 스트랩을 다시 프로그래밍한다(내부에서
 *       전원을 껐다 켜며 래치시킨다).
 *   (2) j721e_pcie_config_link_irq() — 취합기 enable 비트를 되살린다.
 *   (3) cdns_pcie_enable_phy() — PHY 를 켠다. probe 에서는 이것을
 *       cdns_pcie_init_phy() 가 대신 해 주었으므로 직접 부를 일이 없었는데,
 *       여기서는 PHY 를 다시 "확보" 할 필요는 없고 "켜기" 만 필요해서
 *       이 함수를 직접 부른다(아래 영문 주석이 말하는 그대로다).
 *   (4) RC 모드면 refclk 를 켜고, 100ms 를 기다린 뒤 PERST# 을 해제하고,
 *       cdns_pcie_host_link_setup() 으로 링크를 다시 올린다.
 *   (5) rc->avail_ib_bar[] 를 전부 true 로 되돌린다. 이 배열은 "인바운드
 *       BAR 가 아직 비어 있는가" 를 추적하는데, 하드웨어가 리셋되어 실제로는
 *       다 비었는데 소프트웨어 기록은 probe 때의 "사용 중" 이 남아 있어서,
 *       비워 주지 않으면 (6)이 BAR 를 하나도 배정하지 못한다.
 *   (6) cdns_pcie_host_init() 으로 루트 포트와 주소 변환을 다시 세운다.
 * 실행 컨텍스트: 시스템 복귀의 "noirq" 단계. 인터럽트가 아직 꺼져 있어
 * 아래 장치들보다 먼저 컨트롤러가 살아나는 것이 보장된다. 잠들 수는 있다.
 * 호출자: PM 코어. DEFINE_NOIRQ_DEV_PM_OPS 의 resume_noirq 슬롯.
 * 피호출자: 이 파일의 ctrl_init/config_link_irq, 공통 코어의
 * cdns_pcie_enable_phy()/cdns_pcie_host_link_setup()/cdns_pcie_host_init(),
 * clk 와 gpiod.
 * 에러 경로: refclk 를 켠 뒤 실패하는 두 지점에서는 clk_disable_unprepare()
 * 로 클럭만 되돌리고 errno 를 올린다. 그보다 앞에서 실패하면 아무것도
 * 되돌리지 않고 반환하는데, 복귀 실패는 어차피 PM 코어가 치명적으로
 * 취급하는 상황이다. PHY 는 어느 경우에도 여기서 끄지 않는다.
 *
 * 호출 체인:
 *   PM 코어(dpm_resume_noirq) → [이 함수] → j721e_pcie_ctrl_init() →
 *   cdns_pcie_enable_phy() → cdns_pcie_host_link_setup() →
 *   cdns_pcie_host_init()
 */
static int j721e_pcie_resume_noirq(struct device *dev)
{
	/* [한국어] PM 코어가 넘긴 device 에서 인스턴스 상태를 되찾는다. */
	struct j721e_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] RC 분기에서 container_of 에 쓸 공통 코어 구조체. */
	struct cdns_pcie *cdns_pcie = pcie->cdns_pcie;
	/* [한국어] 각 단계 반환값. */
	int ret;

	/* [한국어] 스트랩부터 다시 쓴다. 절전 중 전원 도메인이 내려가면서 래치가 사라졌기
	 * 때문이다. 이 함수 안에서 전원을 다시 껐다 켜며 래치시킨다. */
	ret = j721e_pcie_ctrl_init(pcie);
	/* [한국어] 스트랩 실패는 곧 링크 실패이므로 즉시 반환한다. */
	if (ret < 0)
		/* [한국어] 스트랩을 못 쓰면 복귀를 계속해도 링크가 올라오지 않는다. */
		return ret;

	/* [한국어] 취합기 enable 비트를 되살린다. 전원 도메인이 내려가며 지워졌기 때문이다. */
	j721e_pcie_config_link_irq(pcie);

	/*
	 * This is not called explicitly in the probe, it is called by
	 * cdns_pcie_init_phy().
	 */
	ret = cdns_pcie_enable_phy(pcie->cdns_pcie);
	/* [한국어] PHY 를 못 켜면 링크가 불가능하다. 여기서 반환해도 이전 단계에서
	 * 되돌릴 자원이 없다(스트랩과 IRQ enable 은 상태일 뿐 해제 대상이 아니다). */
	if (ret < 0)
		/* [한국어] PHY 를 못 켜면 더 진행할 수 없다. */
		return ret;

	/* [한국어] 아래는 RC 에만 필요한 복구 — refclk, PERST#, 링크, 인바운드 BAR. */
	if (pcie->mode == PCI_MODE_RC) {
		/* [한국어] 공통 코어 구조체에서 RC 구조체로 되돌아간다. 아래 avail_ib_bar 접근에 필요하다. */
		struct cdns_pcie_rc *rc = cdns_pcie_to_rc(cdns_pcie);

		/* [한국어] 레퍼런스 클럭을 다시 켠다. suspend 에서 clk_disable_unprepare 로 끈 것의 대칭.
		 * probe 때와 달리 devm_clk_get_optional_enabled 를 다시 부르지 않는 이유는
		 * clk 핸들 자체는 살아 있고 켜기만 필요하기 때문이다. */
		ret = clk_prepare_enable(pcie->refclk);
		/* [한국어] 클럭을 못 켜면 카드가 동작하지 않는다. */
		if (ret < 0)
			/* [한국어] 클럭을 못 켜면 카드가 동작하지 않는다. */
			return ret;

		/*
		 * Section 2.2 of the PCI Express Card Electromechanical
		 * Specification (Revision 5.1) mandates that the deassertion
		 * of the PERST# signal should be delayed by 100 ms (TPVPERL).
		 * This shall ensure that the power and the reference clock
		 * are stable.
		 */
		if (pcie->reset_gpio) {
			/* [한국어] suspend 에서 PERST# 을 걸어 두었으므로, 해제 전에 다시 100ms 를 지킨다. */
			msleep(PCIE_T_PVPERL_MS);
			/* [한국어] PERST# 을 해제해 카드를 깨운다. 위 100ms 대기 뒤라야 규격을 만족한다. */
			gpiod_set_value_cansleep(pcie->reset_gpio, 1);
		}

		/* [한국어] 호스트 지원이 꺼진 빌드에서는 이 블록이 사라진다. */
		if (IS_ENABLED(CONFIG_PCI_J721E_HOST)) {
			/* [한국어] 링크만 다시 올린다. probe 의 cdns_pcie_host_setup() 과 달리 버스 열거는
			 * 하지 않는다 — 장치들은 이미 커널이 알고 있고, 필요한 것은 링크뿐이다. */
			ret = cdns_pcie_host_link_setup(rc);
			/* [한국어] 링크 셋업 실패. */
			if (ret < 0) {
				/* [한국어] 링크 셋업이 실패했으므로 방금 켠 클럭을 되돌린다. */
				clk_disable_unprepare(pcie->refclk);
				/* [한국어] 실패를 PM 코어에 전달한다. */
				return ret;
			}
		}

		/*
		 * Reset internal status of BARs to force reinitialization in
		 * cdns_pcie_host_init().
		 */
		for (enum cdns_pcie_rp_bar bar = RP_BAR0; bar <= RP_NO_BAR; bar++)
			/* [한국어] 세 슬롯(RP_BAR0, RP_BAR1, RP_NO_BAR)을 모두 '비어 있음' 으로 되돌린다.
			 * 하드웨어는 리셋되어 실제로 비었는데 소프트웨어 기록만 probe 때의
			 * '사용 중' 으로 남아 있어서, 이것을 지우지 않으면 아래 host_init 이
			 * 인바운드 BAR 를 하나도 배정하지 못한다. */
			rc->avail_ib_bar[bar] = true;

		/* [한국어] 호스트 지원이 꺼진 빌드에서는 사라진다. */
		if (IS_ENABLED(CONFIG_PCI_J721E_HOST)) {
			/* [한국어] 루트 포트 config 와 주소 변환(인바운드 BAR, 아웃바운드 창)을 다시 세운다.
			 * 위에서 avail_ib_bar 를 비워 두었기 때문에 이 호출이 BAR 를 재배정한다. */
			ret = cdns_pcie_host_init(rc);
			/* [한국어] 재초기화 실패. */
			if (ret) {
				/* [한국어] 재초기화가 실패했으므로 방금 켠 클럭을 되돌린다. */
				clk_disable_unprepare(pcie->refclk);
				/* [한국어] 실패를 PM 코어에 전달한다. */
				return ret;
			}
		}
	}

	/* [한국어] RC 든 EP 든 여기까지 오면 복귀 성공이다. */
	return 0;
}

/* [한국어] 위 두 함수를 dev_pm_ops 의 suspend_noirq / resume_noirq 슬롯에
 * 꽂아 j721e_pcie_pm_ops 라는 구조체를 만든다. NOIRQ 판 매크로를 쓰는 이유는
 * PCIe 컨트롤러가 그 아래 매달린 장치들보다 늦게 내려가고 먼저 올라와야
 * 하기 때문이다 — 일반 suspend/resume 단계는 그 순서를 보장하지 않는다.
 * 이 매크로는 freeze/thaw/poweroff/restore(하이버네이션) 슬롯에도 같은 두
 * 함수를 채워 준다. CONFIG_PM_SLEEP 이 꺼져 있으면 구조체가 통째로 빠지도록
 * 정의되어 있고, 그래서 아래에서 pm_sleep_ptr() 로 감싸 참조한다. */
static DEFINE_NOIRQ_DEV_PM_OPS(j721e_pcie_pm_ops,
			       j721e_pcie_suspend_noirq,
			       j721e_pcie_resume_noirq);

/* [한국어]
 * j721e_pcie_driver - 커널 platform 버스에 등록하는 드라이버 서술자
 *
 * 이 구조체 하나가 "언제 이 코드가 불릴지" 를 전부 정한다. of_match_table 이
 * DT 매칭 조건이고, probe/remove 가 생명주기이며, pm 이 절전 동작이다.
 * suppress_bind_attrs 를 true 로 둔 것이 눈에 띄는데, 이는 sysfs 의
 * bind/unbind 파일을 만들지 않아 사용자가 손으로 컨트롤러를 언바인드하지
 * 못하게 막는 설정이다. PCIe 호스트 브리지를 임의로 떼면 그 아래 열거된
 * 모든 장치가 사라져 시스템이 불안정해지기 때문에 호스트 브리지 드라이버들이
 * 흔히 쓰는 안전장치다.
 */
static struct platform_driver j721e_pcie_driver = {
	/* [한국어] 바인딩 시 불릴 진입점. */
	.probe  = j721e_pcie_probe,
	/* [한국어] 언바인딩 시 불릴 정리 함수. */
	.remove = j721e_pcie_remove,
	/* [한국어] 드라이버 코어에 등록할 공통 속성 묶음. */
	.driver = {
		/* [한국어] 드라이버 이름. sysfs 의 /sys/bus/platform/drivers/ 아래에 이 이름으로
		 * 디렉터리가 생기고, 로그 접두사에도 쓰인다. */
		.name	= "j721e-pcie",
		/* [한국어] 이 표와 DT compatible 이 맞아야 probe 가 불린다. */
		.of_match_table = of_j721e_pcie_match,
		/* [한국어] sysfs 의 bind/unbind 파일을 만들지 않는다. PCIe 호스트 브리지를 손으로
		 * 떼면 그 아래 열거된 장치가 모두 사라져 시스템이 불안정해지기 때문이다. */
		.suppress_bind_attrs = true,
		/* [한국어] CONFIG_PM_SLEEP 이 꺼진 빌드에서는 pm_sleep_ptr 이 NULL 로 접혀,
		 * 위 pm_ops 구조체와 두 콜백이 통째로 빠진다. */
		.pm	= pm_sleep_ptr(&j721e_pcie_pm_ops),
	},
};
/* [한국어] module_init/module_exit 보일러플레이트를 대신 만들어 준다.
 * 모듈이 올라올 때 platform_driver_register(&j721e_pcie_driver) 를,
 * 내려갈 때 platform_driver_unregister() 를 부르는 함수를 생성한다. */
module_platform_driver(j721e_pcie_driver);

/* [한국어] 모듈 라이선스를 GPL 로 선언한다. 이 선언이 있어야 커널이
 * 모듈을 "tainted 아님" 으로 보고, EXPORT_SYMBOL_GPL 로 내보낸 심볼
 * (cdns_pcie_host_setup, cdns_pcie_init_phy 등 이 파일이 쓰는 것 대부분)
 * 을 링크할 수 있다. */
MODULE_LICENSE("GPL");
/* [한국어] modinfo 에 보이는 한 줄 설명. */
MODULE_DESCRIPTION("PCIe controller driver for TI's J721E and related SoCs");
/* [한국어] modinfo 에 보이는 원 저자 정보. */
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
