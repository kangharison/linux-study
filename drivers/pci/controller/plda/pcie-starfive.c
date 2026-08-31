// SPDX-License-Identifier: GPL-2.0+
/*
 * PCIe host controller driver for StarFive JH7110 Soc.
 *
 * Copyright (C) 2023 StarFive Technology Co., Ltd.
 */

/*
 * [한국어 설명] StarFive JH7110 SoC PCIe 루트 포트 드라이버 (pcie-starfive.c)
 *
 * === 파일의 역할 ===
 * RISC-V SoC 인 StarFive JH7110 에 들어 있는 두 개의 PCIe 루트 포트를 다루는
 * 플랫폼 드라이버다. JH7110 은 PLDA XpressRich IP 를 그대로 쓰므로, 레지스터
 * 조작과 인터럽트 처리는 전부 공용 코어(pcie-plda-host.c)에 맡기고 이 파일은
 * "JH7110 에서만 다른 것" 만 담당한다. 구체적으로 (1) devicetree 파싱(클럭,
 * 리셋, PHY, syscon, PERST GPIO, 3.3V 레귤레이터, PCI 도메인 번호),
 * (2) STG(System-Top-Group) syscon 을 통한 SoC 수준 설정(클럭 소스 선택,
 * CLKREQ, RP_NEP 비트, 물리 함수 비활성화 라우팅), (3) 전원/리셋/PHY 순서와
 * PCIe 규격이 요구하는 대기 시간, (4) 루트 포트 BAR0/BAR1 을 config 접근에서
 * 감추는 pci_ops 래퍼, (5) suspend/resume 전원 관리다. 모듈로 빌드되며
 * (CONFIG_PCIE_STARFIVE_HOST=m 이면 pcie-starfive.ko) 코어가 EXPORT 한 심볼을
 * 링크해 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름은 [devicetree 의 starfive,jh7110-pcie 노드] -> [플랫폼 버스 매칭] ->
 * starfive_pcie_probe() -> plda_pcie_host_init()(공용 코어) -> 코어가 다시
 * 이 파일의 starfive_pcie_host_init() 훅을 부름 -> 다시 코어로 돌아가
 * pci_host_probe() -> [PCI 코어의 버스 스캔] 순이다. 즉 제어권이 이 파일과
 * 코어 사이를 한 번 왕복한다. 이 구조는 pcie-microchip-host.c 와 대조적인데,
 * 그쪽은 ECAM 공용 코어(pci_host_common_probe)로 진입하고 PLDA 코어를
 * 헬퍼로만 쓴다. 즉 같은 디렉터리의 두 SoC 드라이버가 서로 다른 방식으로
 * 같은 코어를 소비한다. 실행 컨텍스트는 대부분 probe/remove/PM 의 프로세스
 * 컨텍스트이며, 이 파일에는 인터럽트 핸들러가 하나도 없다(전부 코어에 있다).
 * 다만 starfive_pcie_config_read/write 는 런타임 config 접근 경로에서도 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 아래 방향 의존: pcie-plda.h(struct plda_pcie_rp, 브리지 레지스터 헬퍼들),
 * pcie-plda-host.c 의 EXPORT 심볼 세 개(plda_pcie_map_bus, plda_pcie_host_init,
 * plda_pcie_host_deinit), drivers/pci/pci.h 의 PCIE_RESET_CONFIG_WAIT_MS,
 * 그리고 커널 서브시스템 다수 -- clk_bulk(클럭 묶음), reset control, generic PHY,
 * regmap/syscon(STG 레지스터), gpiod(PERST 핀), regulator(vpcie3v3), pm_runtime.
 * 위 방향 의존자: 없다. 이 파일은 최종 드라이버이며 다른 파일이 이 파일의
 * 심볼을 참조하지 않는다.
 * 데이터 흐름: devicetree -> struct starfive_jh7110_pcie(자원 핸들) ->
 * struct plda_pcie_rp(코어에 넘길 설정) -> 코어 -> 하드웨어 레지스터.
 * 반대 방향으로는 STG syscon 의 링크 상태 비트를 읽어 링크 업 여부를 판단한다.
 * NVMe 와의 관계: 이 파일에 nvme 식별자는 없다. JH7110 보드(VisionFive 2 등)의
 * M.2 슬롯에 NVMe SSD 를 꽂으면 그 SSD 는 이 드라이버가 올린 루트 포트 아래에
 * 열거되므로, 여기서 하는 PERST 타이밍과 링크 대기가 잘못되면 SSD 가 아예
 * 보이지 않는다. 그런 의미에서 실질적 연관은 크지만, 코드 호출 관계는 없다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct starfive_jh7110_pcie : plda_pcie_rp 를 첫 필드로 감싸고 JH7110 전용
 *    자원(클럭 묶음, 리셋, syscon, 레귤레이터, PERST GPIO, PHY)을 담는 구조체.
 *  - starfive_pcie_probe() : 드라이버 진입점. DT 파싱 후 events_bitmap 을 만들고
 *    코어에 제어를 넘긴다.
 *  - starfive_pcie_host_init() : 코어가 부르는 SoC 초기화 훅. 이 파일에서 가장
 *    길고 중요한 함수이며 전원 켜기 순서와 PCIe 규격 대기 시간을 모두 담고 있다.
 *  - starfive_pcie_config_read/write() : 루트 포트 BAR0/BAR1 접근을 가로채는
 *    pci_ops 래퍼. starfive_pcie_hide_rc_bar() 가 그 판정을 한다.
 *  - starfive_pcie_link_up() / starfive_pcie_host_wait_for_link() : STG syscon 의
 *    링크 상태 비트를 읽어 최대 1초가량 링크 업을 기다린다.
 *  - starfive_pcie_suspend_noirq / resume_noirq : 서스펜드 시 클럭과 PHY 를
 *    내리고 복귀 시 되살린다.
 */

/* [한국어] FIELD_PREP() -- STG syscon 의 비트필드(AR 은 비트 20:17, AW 는 12:9, CKREF_SRC 는
 * 19:18)에 값을 넣을 때 시프트를 손으로 계산하지 않기 위해 필요하다. */
#include <linux/bitfield.h>
/* [한국어] clk_bulk_prepare_enable() 등 클럭 묶음 API. JH7110 PCIe 는 클럭이 여러 개라
 * 하나씩이 아니라 묶음으로 다룬다. */
#include <linux/clk.h>
/* [한국어] msleep(), usleep_range() -- PERST 어서션 100ms, 리셋 후 config 대기 100ms,
 * 링크 업 폴링 간격에 쓴다. PCIe 규격이 요구하는 대기 시간을 지키기 위한 것이다. */
#include <linux/delay.h>
/* [한국어] gpiod_set_value_cansleep() 등 GPIO consumer API. PERST# 핀을 소프트웨어로
 * 토글하기 위해 필요하다. cansleep 판을 쓰는 것은 이 핀이 I2C 확장기 뒤에 있을
 * 수도 있기 때문이다. */
#include <linux/gpio/consumer.h>
/* [한국어] 인터럽트 관련 선언. 이 파일 자체에는 핸들러가 없지만 헤더 체인상 필요하다. */
#include <linux/interrupt.h>
/* [한국어] 커널 공통 매크로(ARRAY_SIZE 등)를 위한 헤더. */
#include <linux/kernel.h>
/* [한국어] syscon_regmap_lookup_by_phandle() -- devicetree 의 starfive,stg-syscon 프로퍼티가
 * 가리키는 STG 시스템 컨트롤러 regmap 을 얻는다. 이 파일의 SoC 설정 대부분이
 * 그 regmap 을 통해 이루어진다. */
#include <linux/mfd/syscon.h>
/* [한국어] module_platform_driver(), MODULE_LICENSE 등 모듈 인프라. 이 드라이버는
 * 모듈로 빌드될 수 있어 필요하다(Microchip 쪽은 builtin 전용이다). */
#include <linux/module.h>
/* [한국어] of_address_to_resource 계열 devicetree 주소 파싱 선언. */
#include <linux/of_address.h>
/* [한국어] devicetree 인터럽트 파싱 선언. */
#include <linux/of_irq.h>
/* [한국어] of_get_pci_domain_nr() -- devicetree 의 linux,pci-domain 값을 읽는다.
 * 이 드라이버는 그 값으로 RP0 인지 RP1 인지를 구별해 STG 베이스를 고른다. */
#include <linux/of_pci.h>
/* [한국어] struct pci_bus, pci_is_root_bus(), pci_generic_config_read/write 등 PCI 코어 API. */
#include <linux/pci.h>
/* [한국어] generic PHY API(phy_init, phy_set_mode, phy_power_on). JH7110 의 PCIe PHY 는
 * 별도 드라이버로 있고 여기서는 그 소비자다. */
#include <linux/phy/phy.h>
/* [한국어] platform_device, platform_driver 정의. */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_enable/get_sync 등 런타임 전원 관리 API. */
#include <linux/pm_runtime.h>
/* [한국어] regmap_read(), regmap_update_bits() -- STG syscon 접근의 실제 수단. */
#include <linux/regmap.h>
/* [한국어] reset_control_assert/deassert -- PCIe 블록의 리셋 신호를 제어한다. */
#include <linux/reset.h>
/* [한국어] drivers/pci/pci.h (PCI 코어 내부 헤더). PCIE_RESET_CONFIG_WAIT_MS(100ms)를
 * 쓰기 위해 포함한다. 상대 경로인 것은 이 헤더가 include/ 가 아니라
 * drivers/pci/ 안에만 있는 내부 헤더이기 때문이다. */
#include "../../pci.h"

/* [한국어] PLDA 공용 헤더. struct plda_pcie_rp 와 브리지 레지스터 헬퍼
 * (plda_pcie_enable_root_port 등)가 여기서 온다. */
#include "pcie-plda.h"

/* [한국어] 이 IP 가 지원하는 물리 함수(PF) 개수 4. 아래 host_init 이 PF1~PF3 을 끄는
 * 루프의 상한으로 쓴다. JH7110 은 PF0 하나만 쓴다. */
#define PCIE_FUNC_NUM			4

/* system control */
/* [한국어] PCIe 루트 포트 0번(RP0)의 STG syscon 레지스터 블록 시작 오프셋.
 * 아래 모든 STG 오프셋은 이 베이스에 더해져 쓰인다. */
#define STG_SYSCON_PCIE0_BASE			0x48
/* [한국어] 루트 포트 1번(RP1)의 STG 블록 시작 오프셋. 두 포트가 같은 syscon 안에서
 * 서로 다른 구역을 쓰므로, 도메인 번호로 어느 쪽인지 골라야 한다. */
#define STG_SYSCON_PCIE1_BASE			0x1f8

/* [한국어] STG 블록 안에서 AXI4 slave 의 '읽기 주소 채널(AR)' 설정 레지스터 오프셋. */
#define STG_SYSCON_AR_OFFSET			0x78
/* [한국어] AR 레지스터에서 이 드라이버가 건드리는 전체 필드 범위(비트 22:8).
 * regmap_update_bits 의 마스크로 쓰여, 이 범위 밖 비트는 보존된다. */
#define STG_SYSCON_AXI4_SLVL_AR_MASK		GENMASK(22, 8)
/* [한국어] AR 필드 중 물리 함수 번호가 들어가는 자리(비트 20:17)에 x 를 넣는다.
 * 여기에 함수 번호를 써 두면 그 다음 브리지 레지스터 접근이 해당 PF 를 향한다 --
 * 이 해석은 host_init 의 사용 패턴에서 추론한 것이며 JH7110 STG 데이터시트는
 * 이 트리에 없다. */
#define STG_SYSCON_AXI4_SLVL_PHY_AR(x)		FIELD_PREP(GENMASK(20, 17), x)
/* [한국어] AXI4 slave 의 '쓰기 주소 채널(AW)' 설정 레지스터 오프셋.
 * CLKREQ 와 CKREF_SRC 도 이 레지스터에 들어 있다. */
#define STG_SYSCON_AW_OFFSET			0x7c
/* [한국어] AW 레지스터에서 이 드라이버가 건드리는 필드 범위(비트 14:0).
 * CLKREQ(비트 22)와 CKREF_SRC(비트 19:18)와 겹치지 않으므로 서로 독립적으로
 * 갱신할 수 있다. */
#define STG_SYSCON_AXI4_SLVL_AW_MASK		GENMASK(14, 0)
/* [한국어] AW 필드 중 물리 함수 번호 자리(비트 12:9)에 x 를 넣는다. AR 판과 짝이다. */
#define STG_SYSCON_AXI4_SLVL_PHY_AW(x)		FIELD_PREP(GENMASK(12, 9), x)
/* [한국어] AW 레지스터의 bit22 -- CLKREQ# 신호 사용을 켠다. 엔드포인트가 클럭을 요청하는
 * 사이드밴드 신호로, 켜지지 않으면 일부 장치에서 링크가 올라오지 않는다. */
#define STG_SYSCON_CLKREQ			BIT(22)
/* [한국어] AW 레지스터의 비트 19:18 -- 참조 클럭(CKREF) 소스 선택. host_init 이 값 2 를 쓴다.
 * 각 값이 어떤 소스를 뜻하는지는 이 트리의 코드만으로는 알 수 없다. */
#define STG_SYSCON_CKREF_SRC_MASK		GENMASK(19, 18)
/* [한국어] STG 블록 안의 RP_NEP(Root Port, No EP) 레지스터 오프셋. */
#define STG_SYSCON_RP_NEP_OFFSET		0xe8
/* [한국어] RP_NEP 레지스터의 bit8. host_init 이 가장 먼저 세우는 비트로, 이름으로 보아
 * 이 포트를 Root Port 로 확정하는 설정이다. 정확한 의미는 데이터시트가 없어
 * 이 트리만으로는 확인할 수 없다. */
#define STG_SYSCON_K_RP_NEP			BIT(8)
/* [한국어] STG 블록 안의 링크 상태 레지스터 오프셋. starfive_pcie_link_up() 이 읽는다. */
#define STG_SYSCON_LNKSTA_OFFSET		0x170
/* [한국어] 링크 상태 레지스터의 bit5 -- PCIe Data Link Layer 가 활성(DL_Active)인지.
 * 이 비트가 서야 config 요청을 보낼 수 있다. */
#define DATA_LINK_ACTIVE			BIT(5)

/* Parameters for the waiting for link up routine */
/* [한국어] 링크 업 폴링 최대 횟수 10회. 아래 sleep 값과 곱하면 최대 약 0.9~1.0초를 기다린다. */
#define LINK_WAIT_MAX_RETRIES	10
/* [한국어] 폴링 간격 하한 90ms. usleep_range 는 이 범위 안에서 타이머를 다른 이벤트와
 * 합쳐 깨어나므로, 정확한 90ms 가 아니라 90~100ms 사이에 깬다. */
#define LINK_WAIT_USLEEP_MIN	90000
/* [한국어] 폴링 간격 상한 100ms. 하한과 상한을 벌려 두면 커널이 타이머를 묶어 처리해
 * 전력 소모가 줄어든다. */
#define LINK_WAIT_USLEEP_MAX	100000

/*
 * [한국어]
 * struct starfive_jh7110_pcie - JH7110 PCIe 루트 포트 인스턴스 하나
 *
 * PLDA 공용 코어의 struct plda_pcie_rp 를 첫 필드로 감싸는 "임베디드 상속"
 * 구조체다. 코어 함수는 plda 포인터만 받으므로, SoC 훅에서는
 * container_of(plda, struct starfive_jh7110_pcie, plda) 로 이 구조체를 되찾는다.
 * 그 패턴이 쓰이는 곳은 starfive_pcie_link_up(), starfive_pcie_host_deinit(),
 * starfive_pcie_host_init() 세 곳이다.
 *
 * 수명: starfive_pcie_probe() 의 devm_kzalloc 으로 할당되어 디바이스가 사라질 때
 * 자동 해제된다. platform_set_drvdata 로 pdev 에 매달아 두었다가 remove 와
 * suspend/resume 에서 되찾는다.
 * 실행 컨텍스트: 모든 필드가 probe(프로세스 컨텍스트)에서 채워지고, 이후에는
 * host_init/host_deinit/PM 콜백에서만 읽힌다. 인터럽트 컨텍스트에서 접근되는
 * 필드는 plda 하위의 것들뿐이다.
 * 동기화: 이 구조체 자체에는 락이 없다. 필요한 락(plda.lock, plda.msi.lock)은
 * 모두 plda 안에 있고 코어가 관리한다.
 */
struct starfive_jh7110_pcie {
	/* [한국어] 역할: PLDA 공용 코어와 주고받는 컨트롤러 상태. 반드시 '첫 필드' 여야 한다 --
	 * 코어가 struct plda_pcie_rp 포인터만 갖고 있다가 container_of 로 이 구조체를
	 * 되찾기 때문이다(starfive_pcie_link_up, host_init, host_deinit 세 곳).
	 * 설정자: starfive_pcie_probe() 가 dev, host_ops, num_events, events_bitmap 을
	 * 채우고, 나머지는 코어(plda_pcie_host_init)가 되채운다.
	 * 읽는 자: 코어 전체와 이 파일의 host_init/host_deinit.
	 * 값 범위: struct plda_pcie_rp 참조.
	 * 동기화: 필드별로 다르다 -- plda.lock 과 plda.msi.lock 참조. */
	struct plda_pcie_rp plda;
	/* [한국어] 역할: 이 PCIe 블록에 연결된 리셋 신호들의 묶음 핸들.
	 * 설정자: starfive_pcie_parse_dt() 의 devm_reset_control_array_get_exclusive().
	 * 'array' 판이라 devicetree 의 resets 프로퍼티에 적힌 여러 리셋을 하나로 묶어
	 * 다루고, 'exclusive' 는 이 드라이버만 독점적으로 제어한다는 뜻이다.
	 * 읽는 자: starfive_pcie_clk_rst_init()(deassert), _deinit()(assert).
	 * 값 범위: 유효 포인터. IS_ERR 로 검사된 뒤에만 저장된다.
	 * 동기화: probe/remove/PM 프로세스 컨텍스트에서만 쓰이므로 별도 락 없음. */
	struct reset_control *resets;
	/* [한국어] 역할: 이 PCIe 블록의 클럭들을 담은 배열. 개수는 num_clks 에 있다.
	 * 설정자: starfive_pcie_parse_dt() 의 devm_clk_bulk_get_all() 이 배열을 할당해
	 * 이 포인터에 채운다 -- 이름을 지정하지 않고 devicetree 의 clocks 를 전부 가져온다.
	 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare (init, deinit,
	 * suspend_noirq, resume_noirq 네 곳).
	 * 값 범위: 유효 포인터.
	 * 동기화: probe/remove/PM 컨텍스트 전용. */
	struct clk_bulk_data *clks;
	/* [한국어] 역할: STG(System-Top-Group) 시스템 컨트롤러의 regmap 핸들. 이 파일의 SoC 설정
	 * (클럭 소스, CLKREQ, RP_NEP, PF 라우팅, 링크 상태 읽기)이 전부 이것을 통한다.
	 * 설정자: starfive_pcie_parse_dt() 의 syscon_regmap_lookup_by_phandle(
	 * 'starfive,stg-syscon').
	 * 읽는 자: starfive_pcie_link_up()(regmap_read), starfive_pcie_host_init()
	 * (regmap_update_bits 여러 번).
	 * 값 범위: 유효 포인터.
	 * 동기화: regmap 자체가 내부 락을 갖고 있어 이 드라이버는 따로 잠그지 않는다. */
	struct regmap *reg_syscon;
	/* [한국어] 역할: 슬롯에 3.3V 를 공급하는 레귤레이터. 보드에 따라 없을 수도 있다.
	 * 설정자: starfive_pcie_parse_dt() 의 devm_regulator_get_optional('vpcie3v3').
	 * 없으면(-ENODEV) NULL 로 두고 계속 진행한다.
	 * 읽는 자: starfive_pcie_host_init()(enable), starfive_pcie_host_deinit()(disable).
	 * 둘 다 NULL 검사를 먼저 한다.
	 * 값 범위: 유효 포인터 또는 NULL.
	 * 동기화: probe/remove 컨텍스트 전용. */
	struct regulator *vpcie3v3;
	/* [한국어] 역할: PERST# (Fundamental Reset) 핀의 GPIO 디스크립터. 이 핀으로 엔드포인트를
	 * 리셋 상태에 두었다가 풀어 준다.
	 * 설정자: starfive_pcie_parse_dt() 의 devm_gpiod_get_optional('perst',
	 * GPIOD_OUT_HIGH). HIGH 로 시작한다는 것은 논리적 어서션 상태(리셋 유지)로
	 * 시작한다는 뜻이다 -- 실제 전기 극성은 devicetree 의 GPIO_ACTIVE_LOW 플래그가 정한다.
	 * 읽는 자: starfive_pcie_host_init() 이 값 1(어서트)과 0(디어서트)을 쓴다.
	 * 값 범위: 유효 포인터 또는 NULL(보드에 PERST 제어가 없는 경우).
	 * 동기화: probe 컨텍스트 전용. */
	struct gpio_desc *reset_gpio;
	/* [한국어] 역할: JH7110 PCIe 물리 계층(PHY) 핸들. 별도 PHY 드라이버가 제공한다.
	 * 설정자: starfive_pcie_parse_dt() 의 devm_phy_optional_get().
	 * 읽는 자: starfive_pcie_enable_phy()(init/set_mode/power_on),
	 * starfive_pcie_disable_phy()(power_off/exit), suspend/resume 경로.
	 * 값 범위: 유효 포인터 또는 NULL. enable_phy 는 NULL 을 검사하지만
	 * disable_phy 는 검사하지 않는데, 문제가 되지 않는다 -- drivers/phy/phy-core.c 의
	 * phy_power_off() 와 phy_exit() 가 모두 첫 줄에서 NULL 이면 0 을 돌려주기 때문이다
	 * (원본 스냅숏 1f0e418bb6 에서 확인).
	 * 동기화: probe/remove/PM 컨텍스트 전용. */
	struct phy *phy;

	/* [한국어] 역할: 이 포트가 쓸 STG syscon 블록의 시작 오프셋. RP0 면 0x48, RP1 이면 0x1f8.
	 * 설정자: starfive_pcie_parse_dt() 이 devicetree 의 PCI 도메인 번호를 보고 정한다.
	 * 읽는 자: STG 에 접근하는 모든 곳 -- link_up 의 regmap_read 와 host_init 의
	 * regmap_update_bits 전부가 이 값에 오프셋을 더해 쓴다.
	 * 값 범위: STG_SYSCON_PCIE0_BASE 또는 STG_SYSCON_PCIE1_BASE 둘 중 하나.
	 * 동기화: probe 에서 확정 후 읽기 전용. */
	unsigned int stg_pcie_base;
	/* [한국어] 역할: clks 배열의 원소 개수.
	 * 설정자: devm_clk_bulk_get_all() 의 반환값. 음수면 오류다.
	 * 읽는 자: clk_bulk_prepare_enable / clk_bulk_disable_unprepare 의 첫 인자.
	 * 값 범위: 0 이상. 음수는 parse_dt 에서 걸러진다.
	 * 동기화: probe 에서 확정 후 읽기 전용. */
	int num_clks;
};

/*
 * JH7110 PCIe port BAR0/1 can be configured as 64-bit prefetchable memory
 * space. PCIe read and write requests targeting BAR0/1 are routed to so called
 * 'Bridge Configuration space' in PLDA IP datasheet, which contains the bridge
 * internal registers, such as interrupt, DMA and ATU registers...
 * JH7110 can access the Bridge Configuration space by local bus, and don`t
 * want the bridge internal registers accessed by the DMA from EP devices.
 * Thus, they are unimplemented and should be hidden here.
 */
/*
 * [한국어] 위 영문 주석 요약과 보충
 *
 * starfive_pcie_hide_rc_bar - 이 config 접근이 루트 포트의 BAR0/BAR1 인지 판정한다
 *
 * @bus: 접근 대상 버스.
 * @devfn: 접근 대상 장치/함수 번호.
 * @offset: config space 오프셋.
 * @return: true 면 감춰야 할 접근, false 면 그대로 하드웨어로 보내도 되는 접근.
 *
 * 왜 필요한가(영문 주석의 요지): JH7110 에서 루트 포트의 BAR0/BAR1 은 64비트
 * prefetchable 메모리 공간으로 설정될 수 있는데, 그 공간을 겨냥한 PCIe 읽기/쓰기는
 * PLDA 데이터시트가 말하는 'Bridge Configuration space' 로 라우팅된다. 그 안에는
 * 인터럽트/DMA/ATU 레지스터 같은 브리지 내부 레지스터가 들어 있다. JH7110 은 그
 * 공간을 로컬 버스로만 접근하고 싶어 하고, 엔드포인트의 DMA 가 그 레지스터에
 * 닿는 것은 원하지 않는다. 그래서 두 BAR 을 "구현되지 않은 것" 으로 감춘다.
 *
 * 세 조건이 모두 참일 때만 감춘다:
 *  - 루트 버스일 것(하위 버스의 장치 BAR 은 정상 처리해야 한다)
 *  - devfn 이 0 일 것(루트 포트 자신, 즉 device 0 function 0)
 *  - 오프셋이 PCI_BASE_ADDRESS_0(0x10) 또는 _1(0x14)일 것
 *
 * 한계: size 를 보지 않으므로 오프셋이 정확히 0x10/0x14 인 접근만 걸린다.
 * 예컨대 0x0c 에서 8바이트를 읽는 식의 접근은 걸러지지 않는데, PCI 코어는
 * config 를 1/2/4바이트 단위로 정렬해 접근하므로 실제로는 문제가 되지 않는다.
 *
 * 실행 컨텍스트: config 접근 경로 어디서나. 순수 판정이라 재진입 안전하다.
 * 호출자: starfive_pcie_config_read(), starfive_pcie_config_write().
 * 피호출자: pci_is_root_bus.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   PCI 코어 -> starfive_pcie_config_read/write() -> [이 함수]
 */
static bool starfive_pcie_hide_rc_bar(struct pci_bus *bus, unsigned int devfn,
				      int offset)
{
	if (pci_is_root_bus(bus) && !devfn &&
	    (offset == PCI_BASE_ADDRESS_0 || offset == PCI_BASE_ADDRESS_1))
		return true;

	return false;
}

/*
 * [한국어]
 * starfive_pcie_config_write - 루트 포트 BAR 쓰기를 삼키는 config 쓰기 래퍼
 *
 * @bus: 대상 버스. @devfn: 대상 장치/함수. @where: config 오프셋.
 * @size: 접근 폭(1/2/4 바이트). @value: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL(0) 또는 pci_generic_config_write 의 반환값.
 *
 * 왜 필요한가: 위 영문 주석대로 루트 포트의 BAR0/BAR1 은 브리지 내부 레지스터
 * 창으로 라우팅되므로, PCI 코어가 열거 중 BAR 크기를 알아내려고 0xffffffff 를
 * 써 넣는 것조차 위험하다. 그래서 그 두 오프셋에 대한 쓰기는 하드웨어까지
 * 내려보내지 않고 "성공했다" 고만 답한다.
 *
 * 동작: starfive_pcie_hide_rc_bar() 가 참이면 아무것도 하지 않고 성공을 반환하고,
 * 아니면 표준 구현 pci_generic_config_write() 로 넘긴다. 그 표준 구현이
 * pci_ops.map_bus(= plda_pcie_map_bus)를 불러 주소를 얻는다.
 *
 * 실행 컨텍스트: PCI config 접근 경로 어디서나 -- 열거(프로세스 컨텍스트)와
 * 런타임 드라이버 접근 모두. 락은 PCI 코어의 pci_lock 이 담당한다.
 * 호출자: PCI 코어가 bus->ops->write 로 간접 호출. starfive_pcie_ops 에 등록됨.
 * 피호출자: starfive_pcie_hide_rc_bar, pci_generic_config_write.
 * 에러 경로: 표준 구현이 돌려주는 PCIBIOS_ 오류를 그대로 전달한다.
 *
 * 호출 체인:
 *   pci_write_config_dword() -> PCI 코어 -> [이 함수] -> pci_generic_config_write()
 *     -> plda_pcie_map_bus()
 */
static int starfive_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				      int where, int size, u32 value)
{
	if (starfive_pcie_hide_rc_bar(bus, devfn, where))
		return PCIBIOS_SUCCESSFUL;

	/* [한국어] 감출 접근이 아니면 커널 표준 config 쓰기로 넘긴다. 그 안에서
	 * pci_ops.map_bus(= plda_pcie_map_bus)가 불려 실제 주소가 계산된다. */
	return pci_generic_config_write(bus, devfn, where, size, value);
}

/*
 * [한국어]
 * starfive_pcie_config_read - 루트 포트 BAR 읽기를 0 으로 가장하는 config 읽기 래퍼
 *
 * @bus: 대상 버스. @devfn: 대상 장치/함수. @where: config 오프셋.
 * @size: 접근 폭. @value: 읽은 값을 담을 출력 포인터.
 * @return: PCIBIOS_SUCCESSFUL(0) 또는 pci_generic_config_read 의 반환값.
 *
 * 왜 필요한가: 쓰기 래퍼와 짝이다. BAR0/BAR1 을 읽으면 0 을 돌려주어 PCI 코어가
 * "구현되지 않은 BAR" 로 인식하게 만든다. 규격상 미구현 BAR 은 전부 0 을
 * 돌려주므로, 코어는 그 BAR 에 자원을 할당하지 않고 조용히 넘어간다.
 * plda_pcie_write_rc_bar(plda, 0)(하드웨어 쪽 무력화)과 함께 앞뒤로 막는 구성이다.
 *
 * 동작: hide 판정이 참이면 *value 에 0 을 넣고 성공 반환. 아니면 표준 구현으로.
 *
 * 실행 컨텍스트: 쓰기 래퍼와 동일.
 * 호출자: PCI 코어가 bus->ops->read 로 간접 호출.
 * 피호출자: starfive_pcie_hide_rc_bar, pci_generic_config_read.
 * 에러 경로: 표준 구현의 오류를 그대로 전달.
 *
 * 호출 체인:
 *   pci_read_config_dword() -> PCI 코어 -> [이 함수] -> pci_generic_config_read()
 *     -> plda_pcie_map_bus()
 */
static int starfive_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 *value)
{
	if (starfive_pcie_hide_rc_bar(bus, devfn, where)) {
		*value = 0;
		return PCIBIOS_SUCCESSFUL;
	}

	/* [한국어] 감출 접근이 아니면 커널 표준 config 읽기로 넘긴다. */
	return pci_generic_config_read(bus, devfn, where, size, value);
}

/*
 * [한국어]
 * starfive_pcie_parse_dt - devicetree 에서 이 포트가 필요한 모든 자원을 끌어온다
 *
 * @pcie: 자원 핸들을 채울 대상. plda.dev 는 이미 설정되어 있어야 한다.
 * @dev: 이 플랫폼 디바이스의 struct device. of_node 가 devicetree 노드다.
 * @return: 0 성공, 음수 errno 실패. -EPROBE_DEFER 가 섞여 나올 수 있는데,
 *          이는 아직 준비되지 않은 공급자(클럭/PHY/레귤레이터 드라이버)를
 *          기다려야 한다는 뜻이며 커널이 나중에 probe 를 재시도한다.
 *
 * 왜 필요한가: JH7110 PCIe 포트를 켜려면 클럭, 리셋, PHY, syscon, PERST GPIO,
 * 레귤레이터라는 여섯 종류의 외부 자원이 필요하다. 하나라도 없으면 하드웨어를
 * 만질 수 없으므로, 레지스터를 건드리기 "전에" 전부 확보해 두는 것이 원칙이다.
 *
 * 동작 단계(필수와 선택이 구분된다):
 *  1. 클럭 묶음을 전부 가져온다(필수). 이름을 지정하지 않는 _get_all 판이다.
 *  2. 리셋 배열을 독점 모드로 가져온다(필수).
 *  3. starfive,stg-syscon phandle 로 STG regmap 을 얻는다(필수).
 *  4. PHY 를 가져온다(_optional 이라 없어도 되지만, 오류는 실패로 처리한다).
 *  5. PCI 도메인 번호를 읽어 0 이면 RP0, 1 이면 RP1 의 STG 베이스를 고른다.
 *     그 밖의 값은 -ENODEV. 이 판정이 이 파일에서 두 포트를 구별하는 유일한
 *     수단이며, 그래서 devicetree 가 도메인 번호를 정적으로 지정해야 한다
 *     (위 영문 주석이 그 이유를 설명한다).
 *  6. PERST GPIO 를 optional 로 가져온다(없으면 NULL).
 *  7. vpcie3v3 레귤레이터를 optional 로 가져온다. -ENODEV 만 "없음" 으로 보고
 *     NULL 로 만들며, 그 밖의 오류는 실패로 처리한다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. devm_ 할당과 phandle 해석으로 잠들 수 있다.
 * 호출자: starfive_pcie_probe() 하나뿐.
 * 피호출자: devm_clk_bulk_get_all, devm_reset_control_array_get_exclusive,
 * syscon_regmap_lookup_by_phandle, devm_phy_optional_get, of_get_pci_domain_nr,
 * devm_gpiod_get_optional, devm_regulator_get_optional, dev_err_probe.
 * 에러 경로: 모든 실패가 dev_err_probe 를 거친다. 이 헬퍼는 -EPROBE_DEFER 일 때는
 * 로그를 남기지 않고 조용히 넘어가므로, 재시도 때마다 커널 로그가 더러워지지 않는다.
 * 자원은 전부 devm_ 이라 실패 시 자동 해제된다.
 *
 * 호출 체인:
 *   starfive_pcie_probe() -> [이 함수]
 */
static int starfive_pcie_parse_dt(struct starfive_jh7110_pcie *pcie,
				  struct device *dev)
{
	int domain_nr;

	/* [한국어] devicetree 의 clocks 프로퍼티에 적힌 클럭을 이름 없이 전부 가져온다.
	 * 반환값이 개수이고 음수면 오류다 -- IS_ERR 가 아니라 부호 검사인 이유가 이것이다. */
	pcie->num_clks = devm_clk_bulk_get_all(dev, &pcie->clks);
	/* [한국어] 음수면 오류. -EPROBE_DEFER 도 여기에 섞여 들어온다. */
	if (pcie->num_clks < 0)
		/* [한국어] dev_err_probe 는 -EPROBE_DEFER 일 때 로그를 남기지 않으므로,
		 * probe 재시도가 반복되어도 커널 로그가 더러워지지 않는다. */
		return dev_err_probe(dev, pcie->num_clks,
				     "failed to get pcie clocks\n");

	/* [한국어] devicetree 의 resets 를 배열로 묶어 독점(exclusive) 모드로 가져온다.
	 * 독점이란 이 리셋을 공유하는 다른 드라이버가 없어야 한다는 뜻이다. */
	pcie->resets = devm_reset_control_array_get_exclusive(dev);
	/* [한국어] 포인터를 돌려주는 API 라 이쪽은 IS_ERR 로 검사한다. */
	if (IS_ERR(pcie->resets))
		/* [한국어] 실패 원인을 남기고 반환. 문자열 끝에 개행이 빠져 있는 것은 상류 코드 그대로다. */
		return dev_err_probe(dev, PTR_ERR(pcie->resets),
				     "failed to get pcie resets");

	/* [한국어] devicetree 의 starfive,stg-syscon phandle 이 가리키는 STG 시스템 컨트롤러의
	 * regmap 을 얻는다. 이 파일의 SoC 설정 전부가 이 regmap 을 통해 이루어지므로
	 * 가장 중요한 자원이다. */
	pcie->reg_syscon =
		syscon_regmap_lookup_by_phandle(dev->of_node,
						"starfive,stg-syscon");

	/* [한국어] syscon 을 못 얻으면 SoC 설정을 아무것도 할 수 없다. */
	if (IS_ERR(pcie->reg_syscon))
		/* [한국어] 실패 원인을 남기고 반환. */
		return dev_err_probe(dev, PTR_ERR(pcie->reg_syscon),
				     "failed to parse starfive,stg-syscon\n");

	/* [한국어] PCIe PHY 를 가져온다. optional 판이라 devicetree 에 phys 가 없으면 NULL 을
	 * 돌려주고 오류가 아니다. 반대로 있는데 아직 준비 안 됐으면 -EPROBE_DEFER 다. */
	pcie->phy = devm_phy_optional_get(dev, NULL);
	/* [한국어] 오류 포인터면 실패. */
	if (IS_ERR(pcie->phy))
		/* [한국어] 실패 원인을 남기고 반환. */
		return dev_err_probe(dev, PTR_ERR(pcie->phy),
				     "failed to get pcie phy\n");

	/*
	 * The PCIe domain numbers are set to be static in JH7110 DTS.
	 * As the STG system controller defines different bases in PCIe RP0 &
	 * RP1, we use them to identify which controller is doing the hardware
	 * initialization.
	 */
	domain_nr = of_get_pci_domain_nr(dev->of_node);

	/* [한국어] JH7110 에는 루트 포트가 두 개뿐이므로 도메인 번호는 0 또는 1 이어야 한다.
	 * 범위 밖이면 devicetree 가 잘못된 것이다. */
	if (domain_nr < 0 || domain_nr > 1)
		/* [한국어] -ENODEV 로 probe 실패. of_get_pci_domain_nr 이 음수(속성 없음)를 돌려준 경우도
		 * 여기서 걸린다. */
		return dev_err_probe(dev, -ENODEV,
				     "failed to get valid pcie domain\n");

	/* [한국어] 도메인 0 = 루트 포트 0 */
	if (domain_nr == 0)
		/* [한국어] RP0 의 STG 블록 베이스(0x48)를 쓴다. */
		pcie->stg_pcie_base = STG_SYSCON_PCIE0_BASE;
	else
		/* [한국어] 도메인 1 = 루트 포트 1. RP1 의 베이스(0x1f8)를 쓴다.
		 * 이 한 줄이 두 포트를 구별하는 유일한 지점이며, 이후 모든 STG 접근이
		 * 이 베이스에 오프셋을 더해 이루어진다. */
		pcie->stg_pcie_base = STG_SYSCON_PCIE1_BASE;

	/* [한국어] PERST# 제어용 GPIO 를 가져온다. GPIOD_OUT_HIGH 는 출력 모드로 잡으면서
	 * 초기값을 논리 1(= 리셋 어서트)로 둔다는 뜻이다. 실제 전기 극성은
	 * devicetree 의 GPIO_ACTIVE_LOW 플래그가 결정한다. optional 이라 없어도 된다. */
	pcie->reset_gpio = devm_gpiod_get_optional(dev, "perst",
						   GPIOD_OUT_HIGH);
	/* [한국어] 오류 포인터면 실패(없는 경우는 NULL 이라 여기 걸리지 않는다). */
	if (IS_ERR(pcie->reset_gpio))
		/* [한국어] 실패 원인을 남기고 반환. */
		return dev_err_probe(dev, PTR_ERR(pcie->reset_gpio),
				     "failed to get perst-gpio\n");

	/* [한국어] 슬롯 3.3V 레귤레이터를 가져온다. _optional 판인데도 없을 때 NULL 이 아니라
	 * -ENODEV 오류 포인터를 돌려주므로, 아래에서 따로 걸러야 한다. */
	pcie->vpcie3v3 = devm_regulator_get_optional(dev, "vpcie3v3");
	/* [한국어] 오류 포인터인 경우 두 갈래로 나뉜다. */
	if (IS_ERR(pcie->vpcie3v3)) {
		/* [한국어] -ENODEV 가 아니면 진짜 오류다(예: -EPROBE_DEFER). */
		if (PTR_ERR(pcie->vpcie3v3) != -ENODEV)
			/* [한국어] 실패 원인을 남기고 반환. */
			return dev_err_probe(dev, PTR_ERR(pcie->vpcie3v3),
					     "failed to get vpcie3v3 regulator\n");
		/* [한국어] -ENODEV 는 '보드에 이 레귤레이터가 없다' 는 뜻이므로 NULL 로 만들고 계속 진행한다.
		 * 이후 코드가 전부 NULL 검사를 하므로 안전하다. */
		pcie->vpcie3v3 = NULL;
	}

	return 0;
}

/*
 * [한국어] 이 루트 포트의 config space 접근 방식을 PCI 코어에 알리는 테이블.
 *
 *  - map_bus = plda_pcie_map_bus : ECAM 규칙으로 주소를 계산한다(코어 제공).
 *  - read / write : 표준 구현이 아니라 이 파일의 래퍼를 쓴다. 루트 포트 BAR 을
 *    감추기 위한 것이며, 래퍼가 결국 pci_generic_config_read/write 를 부른다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: starfive_pcie_probe() 가
 * plda_pcie_host_init() 의 두 번째 인자로 넘기고, 코어가 bridge->ops 에 꽂는다.
 * const 가 아닌 이유는 plda_pcie_host_init() 의 인자 타입이 struct pci_ops 포인터
 * (const 없음)이기 때문이다.
 * 동기화: 실질적으로 읽기 전용.
 */
static struct pci_ops starfive_pcie_ops = {
	.map_bus	= plda_pcie_map_bus,
	.read           = starfive_pcie_config_read,
	.write          = starfive_pcie_config_write,
};

/*
 * [한국어]
 * starfive_pcie_clk_rst_init - 클럭을 켜고 리셋을 푼다
 *
 * @pcie: clks/num_clks/resets 가 채워진 컨트롤러.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 왜 필요한가: 클럭이 돌지 않는 블록의 리셋을 풀면 내부 상태가 불확정해진다.
 * 따라서 반드시 "클럭 먼저, 리셋 나중" 순서를 지켜야 한다. 이 함수가 그 순서를
 * 캡슐화한다.
 *
 * 동작 단계:
 *  1. clk_bulk_prepare_enable 로 모든 클럭을 준비하고 켠다. 실패하면 바로 반환.
 *  2. reset_control_deassert 로 리셋을 푼다.
 *  3. 리셋 해제가 실패하면 이미 켠 클럭을 되돌린다 -- 부분 성공 상태로 빠져나가면
 *     전력만 먹고 동작하지 않는 블록이 남기 때문이다.
 *  4. ret 을 반환한다(성공이면 0).
 *
 * 눈에 띄는 점: 3번 경로에서 dev_err_probe 의 반환값을 받지 않고 버린다.
 * 그래도 ret 에 원래 오류가 들어 있어 결과는 같다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트(host_init 안). 잠들 수 있다.
 * 호출자: starfive_pcie_host_init() 하나뿐.
 * 피호출자: clk_bulk_prepare_enable, reset_control_deassert,
 * clk_bulk_disable_unprepare, dev_err_probe.
 * 에러 경로: 위 3번 참조.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수]
 */
static int starfive_pcie_clk_rst_init(struct starfive_jh7110_pcie *pcie)
{
	struct device *dev = pcie->plda.dev;
	/* [한국어] 클럭 켜기와 리셋 해제의 결과를 담는다. */
	int ret;

	/* [한국어] 모든 클럭을 prepare(사전 준비) 후 enable(실제 공급)한다. 두 단계를 합친 헬퍼로,
	 * prepare 는 잠들 수 있고 enable 은 그렇지 않다는 클럭 프레임워크 규약 때문에
	 * 원래는 나뉘어 있다. */
	ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	/* [한국어] 클럭이 안 켜지면 이후 아무것도 할 수 없다. */
	if (ret)
		/* [한국어] 오류를 남기고 그대로 반환. 리셋은 아직 손대지 않았으므로 되돌릴 것이 없다. */
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	/* [한국어] 리셋을 푼다. 반드시 클럭이 돈 뒤여야 한다 -- 클럭 없이 리셋을 풀면
	 * 내부 플립플롭이 불확정 상태로 남는다. */
	ret = reset_control_deassert(pcie->resets);
	/* [한국어] 리셋 해제 실패 -- 여기서는 이미 클럭을 켰으므로 되돌려야 한다. */
	if (ret) {
		/* [한국어] 켠 클럭을 다시 끈다. 부분 성공 상태로 빠져나가면 전력만 먹고 동작하지 않는
		 * 블록이 남는다. */
		clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
		/* [한국어] 오류를 남긴다. 반환값을 쓰지 않는데, 아래 return ret 이 같은 값을 돌려주므로
		 * 결과는 같다. */
		dev_err_probe(dev, ret, "failed to deassert resets\n");
	}

	return ret;
}

/*
 * [한국어]
 * starfive_pcie_clk_rst_deinit - 리셋을 걸고 클럭을 끈다
 *
 * @pcie: resets/clks 가 채워진 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: clk_rst_init 의 정확한 역순이다. 클럭이 살아 있는 동안 리셋을
 * 걸어야 블록이 깨끗한 상태로 정지한다 -- 클럭을 먼저 끊으면 리셋 신호가
 * 내부 플립플롭까지 전파되지 않을 수 있다.
 *
 * 동작: reset_control_assert 로 리셋을 건 뒤 clk_bulk_disable_unprepare 로
 * 클럭을 끈다. 두 호출 모두 반환값을 보지 않는데, 해제 경로에서는 실패해도
 * 할 수 있는 일이 없기 때문이다.
 *
 * 실행 컨텍스트: remove 또는 probe 실패 경로의 프로세스 컨텍스트.
 * 호출자: starfive_pcie_host_deinit() 하나뿐.
 * 피호출자: reset_control_assert, clk_bulk_disable_unprepare.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_pcie_host_deinit() -> starfive_pcie_host_deinit() -> [이 함수]
 */
static void starfive_pcie_clk_rst_deinit(struct starfive_jh7110_pcie *pcie)
{
	reset_control_assert(pcie->resets);
	/* [한국어] 클럭을 끈다. 바로 위에서 리셋을 먼저 걸었으므로, 리셋 신호가 내부까지
	 * 전파된 뒤에 클럭이 끊긴다. */
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
}

/*
 * [한국어]
 * starfive_pcie_link_up - STG syscon 을 읽어 데이터 링크가 올라왔는지 판정한다
 *
 * @plda: 코어 구조체 포인터. container_of 로 JH7110 구조체를 되찾는다.
 * @return: true 면 링크 활성, false 면 아직 아니거나 레지스터 읽기 실패.
 *
 * 왜 필요한가: PCIe 링크 훈련(Link Training)은 수십~수백 밀리초가 걸리고, 링크가
 * 올라오기 전에 config 요청을 보내면 응답이 없다. 그래서 진행 여부를 폴링해야
 * 하는데, JH7110 은 그 상태를 PLDA 브리지 레지스터가 아니라 SoC 의 STG syscon
 * 에 노출한다 -- 그래서 이 판정이 코어가 아니라 SoC 드라이버에 있다.
 *
 * 동작:
 *  1. container_of 로 JH7110 구조체를 얻는다. plda 가 첫 필드라 주소는 같지만
 *     타입 안전성을 위해 매크로를 쓴다.
 *  2. regmap_read 로 (stg_pcie_base + STG_SYSCON_LNKSTA_OFFSET) 을 읽는다.
 *     stg_pcie_base 가 RP0/RP1 을 가르므로 같은 코드가 두 포트에 다 쓰인다.
 *  3. 읽기 실패는 링크 없음으로 취급하고 로그를 남긴다.
 *  4. DATA_LINK_ACTIVE(bit5) 를 검사해 !! 로 bool 로 만든다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트(wait_for_link 루프 안). regmap 접근이
 * 잠들 수 있으므로 인터럽트 컨텍스트에서 부르면 안 된다.
 * 호출자: starfive_pcie_host_wait_for_link() 하나뿐이다. 시그니처가
 * struct plda_pcie_rp 를 받는 형태인데도 코어의 어떤 ops 에도 등록되지 않는다 --
 * 즉 이 트리에서는 코어가 이 함수를 부르지 않는다.
 * 피호출자: container_of, regmap_read, dev_err.
 * 에러 경로: 읽기 실패 시 false 반환 -> 호출자가 계속 재시도하다 타임아웃한다.
 *
 * 호출 체인:
 *   starfive_pcie_host_init() -> starfive_pcie_host_wait_for_link() -> [이 함수]
 */
static bool starfive_pcie_link_up(struct plda_pcie_rp *plda)
{
	struct starfive_jh7110_pcie *pcie =
		container_of(plda, struct starfive_jh7110_pcie, plda);
	/* [한국어] regmap_read 의 반환값(레지스터 접근 자체의 성공 여부). */
	int ret;
	/* [한국어] 읽어 온 링크 상태 레지스터 값. */
	u32 stg_reg_val;

	/* [한국어] STG 의 (이 포트 베이스 + 링크 상태 오프셋 0x170) 을 읽는다.
	 * regmap 은 내부에 락과 캐시를 갖고 있어 이 드라이버는 직접 동기화하지 않는다. */
	ret = regmap_read(pcie->reg_syscon,
			  pcie->stg_pcie_base + STG_SYSCON_LNKSTA_OFFSET,
			  &stg_reg_val);
	/* [한국어] regmap 접근 실패는 syscon 설정 문제이므로 링크 없음으로 취급한다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. ratelimit 을 쓰지 않는 것은 이 함수가 최대 10번만 불리기 때문이다. */
		dev_err(pcie->plda.dev, "failed to read link status\n");
		return false;
	}

	/* [한국어] DATA_LINK_ACTIVE(bit5) 를 검사한다. !! 로 0/1 bool 로 정규화하는 관용구다.
	 * 이 비트가 서야 PCIe Data Link Layer 가 살아 있는 것이고, 그래야 config 요청에
	 * 응답이 온다. */
	return !!(stg_reg_val & DATA_LINK_ACTIVE);
}

/*
 * [한국어]
 * starfive_pcie_host_wait_for_link - 링크가 올라올 때까지 최대 약 1초를 기다린다
 *
 * @pcie: reg_syscon 과 stg_pcie_base 가 채워진 컨트롤러.
 * @return: 0 은 링크 업, -ETIMEDOUT 은 시간 안에 올라오지 않음.
 *
 * 왜 필요한가: 슬롯이 비어 있거나 장치가 느리게 준비되는 경우가 있어, 무한
 * 대기 대신 유한한 폴링이 필요하다. 흥미롭게도 호출자는 이 반환값을 실패로
 * 취급하지 않는다 -- 링크가 없어도 probe 는 성공시키고 로그만 남긴다.
 *
 * 동작: LINK_WAIT_MAX_RETRIES(10)회 동안 starfive_pcie_link_up() 을 검사하고,
 * 아니면 usleep_range(90ms, 100ms) 로 잠든다. 최대 대기는 약 0.9~1.0초다.
 * 링크가 올라오면 dev_info 로 알리고 0 을 반환한다.
 * 첫 검사를 sleep 보다 먼저 하므로, 이미 링크가 올라와 있으면 즉시 반환한다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. usleep_range 로 잠들므로 인터럽트
 * 컨텍스트에서 부르면 안 된다.
 * 호출자: starfive_pcie_host_init() 의 마지막 부분 하나뿐.
 * 피호출자: starfive_pcie_link_up, dev_info, usleep_range.
 * 에러 경로: -ETIMEDOUT 을 돌려주지만 호출자가 무시하고 "port link down" 만 찍는다.
 * 슬롯이 비어 있는 것은 오류가 아니므로 합리적인 선택이다.
 *
 * 호출 체인:
 *   starfive_pcie_host_init() -> [이 함수] -> starfive_pcie_link_up()
 */
static int starfive_pcie_host_wait_for_link(struct starfive_jh7110_pcie *pcie)
{
	int retries;

	/* Check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {
		/* [한국어] 링크가 올라왔는지 검사한다. 첫 검사를 sleep 보다 먼저 하므로 이미 올라와 있으면
		 * 즉시 반환한다. */
		if (starfive_pcie_link_up(&pcie->plda)) {
			/* [한국어] 링크 업을 알린다. 이 메시지가 보이지 않으면 슬롯이 비었거나 장치가 죽은 것이다. */
			dev_info(pcie->plda.dev, "port link up\n");
			return 0;
		}
		/* [한국어] 90~100ms 잠든다. 범위를 벌려 두면 커널이 다른 타이머와 묶어 처리해 전력이 절약된다.
		 * 10회 반복이므로 최대 약 1초를 기다린다. */
		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX);
	}

	return -ETIMEDOUT;
}

/*
 * [한국어]
 * starfive_pcie_enable_phy - PCIe PHY 를 초기화하고 PCIE 모드로 전원을 켠다
 *
 * @dev: 오류 로그와 dev_err_probe 의 기준 device.
 * @pcie: phy 핸들이 채워진(또는 NULL 인) 컨트롤러.
 * @return: 0 성공(PHY 가 없는 경우 포함), 음수 errno 실패.
 *
 * 왜 필요한가: PCIe 물리 계층은 SerDes 이며 별도 PHY 드라이버가 관리한다.
 * 링크 훈련이 시작되기 전에 PHY 가 초기화되고 전원이 들어와 있어야 한다.
 * 그래서 host_init 의 가장 첫 단계가 이것이다.
 *
 * 동작 단계(generic PHY 프레임워크의 표준 순서):
 *  1. PHY 가 없으면(optional 이라 NULL 가능) 아무것도 하지 않고 성공.
 *  2. phy_init -- PHY 하드웨어 초기화.
 *  3. phy_set_mode(PHY_MODE_PCIE) -- 이 SerDes 를 PCIe 용도로 설정한다.
 *     JH7110 의 PHY 는 USB3 등 다른 프로토콜과 공유될 수 있어 모드 지정이 필요하다.
 *  4. phy_power_on -- 전원 인가.
 *  실패 시 err_phy_on 라벨로 가서 phy_exit 로 2번을 되돌린다.
 *
 * 주의: 라벨 이름이 err_phy_on 이지만 set_mode 실패도 같은 라벨로 간다. 두 경우
 * 모두 되돌릴 것이 phy_init 뿐이라 결과적으로 맞는 처리다.
 *
 * 실행 컨텍스트: probe 또는 resume 프로세스 컨텍스트. PHY 조작은 잠들 수 있다.
 * 호출자: starfive_pcie_host_init(), starfive_pcie_resume_noirq().
 * 피호출자: phy_init, phy_set_mode, phy_power_on, phy_exit, dev_err_probe.
 * 에러 경로: 위 참조. phy_init 실패는 되돌릴 것이 없어 곧장 반환한다.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> starfive_pcie_host_init() -> [이 함수]
 *   PM 코어 -> starfive_pcie_resume_noirq() -> [이 함수]
 */
static int starfive_pcie_enable_phy(struct device *dev,
				    struct starfive_jh7110_pcie *pcie)
{
	int ret;

	/* [한국어] PHY 가 devicetree 에 없으면(optional) 할 일이 없다. */
	if (!pcie->phy)
		return 0;

	/* [한국어] PHY 하드웨어를 초기화한다. generic PHY 프레임워크의 첫 단계다. */
	ret = phy_init(pcie->phy);
	/* [한국어] 초기화 실패. */
	if (ret)
		/* [한국어] 오류를 남기고 반환. 아직 되돌릴 것이 없으므로 라벨로 가지 않는다. */
		return dev_err_probe(dev, ret,
				     "failed to initialize pcie phy\n");

	/* [한국어] 이 SerDes 를 PCIe 용도로 설정한다. JH7110 의 PHY 는 USB3 등과 공유될 수 있어
	 * 모드 지정이 필요하다. */
	ret = phy_set_mode(pcie->phy, PHY_MODE_PCIE);
	/* [한국어] 모드 설정 실패 -- phy_init 를 되돌려야 한다. */
	if (ret) {
		/* [한국어] 오류를 남긴다(반환값은 아래 ret 이 대신한다). */
		dev_err_probe(dev, ret, "failed to set pcie mode\n");
		goto err_phy_on;
	}

	/* [한국어] PHY 에 전원을 인가한다. 이 단계가 끝나야 링크 훈련이 가능해진다. */
	ret = phy_power_on(pcie->phy);
	/* [한국어] 전원 인가 실패 -- 역시 phy_init 를 되돌려야 한다. */
	if (ret) {
		/* [한국어] 오류를 남긴다. */
		dev_err_probe(dev, ret, "failed to power on pcie phy\n");
		goto err_phy_on;
	}

	return 0;

err_phy_on:
	phy_exit(pcie->phy);
	return ret;
}

/*
 * [한국어]
 * starfive_pcie_disable_phy - PCIe PHY 전원을 끄고 해제한다
 *
 * @pcie: phy 핸들이 채워진(또는 NULL 인) 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: enable_phy 의 역순이다. 서스펜드와 드라이버 해제 양쪽에서 쓰인다.
 *
 * 동작: phy_power_off 후 phy_exit. NULL 검사를 하지 않는데, phy 코어의 두 함수가
 * 모두 NULL 을 받으면 0 을 돌려주므로(drivers/phy/phy-core.c 에서 확인) 안전하다.
 * 반환값을 보지 않는 것은 해제 경로에서 할 수 있는 일이 없기 때문이다.
 *
 * 실행 컨텍스트: remove, probe 실패, suspend, resume 실패 경로의 프로세스 컨텍스트.
 * 호출자: starfive_pcie_host_deinit(), starfive_pcie_suspend_noirq(),
 * starfive_pcie_resume_noirq()의 클럭 실패 경로 -- 세 곳이다.
 * 피호출자: phy_power_off, phy_exit.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   starfive_pcie_host_deinit() -> [이 함수]
 *   starfive_pcie_suspend_noirq() -> [이 함수]
 */
static void starfive_pcie_disable_phy(struct starfive_jh7110_pcie *pcie)
{
	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
}

/*
 * [한국어]
 * starfive_pcie_host_deinit - 코어가 부르는 SoC 해제 훅 (전원/클럭/PHY 내리기)
 *
 * @plda: 코어 구조체 포인터. container_of 로 JH7110 구조체를 되찾는다.
 * @return: 없음.
 *
 * 왜 필요한가: 코어(plda_pcie_host_deinit)는 PCI 버스와 인터럽트만 정리할 수
 * 있을 뿐 SoC 전원 계통은 모른다. 그 부분을 이 훅이 맡는다.
 *
 * 동작 순서(전원을 안전하게 내리는 순서):
 *  1. 클럭과 리셋을 되돌린다(리셋 걸기 -> 클럭 끄기).
 *  2. 3.3V 레귤레이터가 있으면 끈다. 슬롯 전원을 마지막에 가깝게 끊는다.
 *  3. PHY 전원을 끄고 해제한다.
 *
 * 이 순서는 starfive_pcie_host_init() 의 역순과 정확히 일치하지는 않는다 --
 * init 에서는 PHY -> STG -> 클럭/리셋 -> 레귤레이터 순인데 deinit 은
 * 클럭/리셋 -> 레귤레이터 -> PHY 다. 코드가 그렇게 되어 있다는 사실만 적어 둔다.
 *
 * 실행 컨텍스트: remove 또는 probe 실패 경로의 프로세스 컨텍스트.
 * 호출자: 코어가 host_ops->host_deinit 으로 간접 호출한다. 실제 호출 지점은
 * plda_pcie_host_deinit()(정상 해제)과 plda_pcie_host_init() 의 err_host /
 * err_probe(실패 정리) 세 곳이다.
 * 피호출자: container_of, starfive_pcie_clk_rst_deinit, regulator_disable,
 * starfive_pcie_disable_phy.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   starfive_pcie_remove() -> plda_pcie_host_deinit() -> host_ops->host_deinit
 *     = [이 함수]
 */
static void starfive_pcie_host_deinit(struct plda_pcie_rp *plda)
{
	struct starfive_jh7110_pcie *pcie =
		container_of(plda, struct starfive_jh7110_pcie, plda);

	starfive_pcie_clk_rst_deinit(pcie);
	/* [한국어] 레귤레이터가 있는 보드에서만 끈다. 없으면 NULL 이라 건너뛴다. */
	if (pcie->vpcie3v3)
		regulator_disable(pcie->vpcie3v3);
	starfive_pcie_disable_phy(pcie);
}

/*
 * [한국어]
 * starfive_pcie_host_init - 코어가 부르는 SoC 초기화 훅. 이 파일의 핵심 함수
 *
 * @plda: 코어 구조체 포인터. bridge_addr 은 이미 ioremap 되어 있다
 *        (코어가 이 훅을 부르기 직전에 매핑을 마쳤기 때문).
 * @return: 0 성공, 음수 errno 실패. 실패하면 코어가 즉시 probe 를 접는다.
 *
 * 왜 필요한가: JH7110 에서 PCIe 를 실제로 살아나게 하는 모든 순서 의존적 작업이
 * 여기 모여 있다. 순서가 하나라도 어긋나면 링크가 올라오지 않거나 커널이 멈춘다.
 *
 * 동작 단계(순서가 곧 이유다):
 *  1. PHY 초기화/전원 인가. 물리 계층이 준비되기 전에는 아무것도 할 수 없다.
 *  2. STG RP_NEP 비트 세우기 -- 이 포트를 Root Port 로 확정하는 설정으로 보인다.
 *  3. STG CKREF_SRC 를 2 로 -- 참조 클럭 소스 선택.
 *  4. STG CLKREQ 켜기 -- CLKREQ# 사이드밴드 신호 사용.
 *     2~4 는 모두 클럭을 켜기 "전에" 해야 하는 SoC 배선 설정이다.
 *  5. 클럭 켜고 리셋 해제.
 *  6. 3.3V 레귤레이터가 있으면 켠다. 슬롯에 전원이 들어간다.
 *  7. PERST# 를 어서트(값 1)해 엔드포인트를 리셋 상태로 붙잡는다. 전원이 들어온
 *     직후 장치가 제멋대로 링크 훈련을 시작하지 못하게 막는 것이다.
 *  8. PF1~PF3 비활성화 루프 -- STG 의 AR/AW 필드에 함수 번호를 쓴 뒤
 *     plda_pcie_disable_func() 로 PCI_MISC.PHY_FUNCTION_DIS 를 세운다.
 *     AR/AW 필드가 대상 PF 를 고르는 라우팅이라는 해석은 이 사용 패턴에서
 *     나온 추론이며, JH7110 STG 데이터시트는 이 트리에 없다.
 *  9. AR/AW 필드를 0 으로 되돌린다 -- 이후 브리지 접근이 PF0 을 향하게 한다.
 * 10. RP_ENABLE 세우기, 루트 포트 BAR0/1 을 0 으로 만들기, Class Code 를
 *     PCI-to-PCI 브리지로 고치기.
 * 11. LTR 메시지 전달 끄기 -- 켜 둔 채로 두면 전달 주소가 초기화되지 않아
 *     커널이 멈춘다(바로 위 영문 주석이 그 현상을 설명한다).
 * 12. prefetchable 창 64비트 지원 켜기 -- 이 뒤에야 ATU 의 상위 32비트 설정이 먹는다.
 * 13. msleep(100) -- PERST 를 최소 100ms 어서트 유지. PCIe CEM 규격 r2.0
 *     Table 2-4 의 T_PVPERL(전원 안정 후 PERST 해제까지의 최소 시간)이다.
 * 14. PERST# 디어서트(값 0) -- 이제 엔드포인트가 링크 훈련을 시작한다.
 * 15. msleep(PCIE_RESET_CONFIG_WAIT_MS = 100) -- 리셋 해제 후 첫 config 요청까지
 *     최소 100ms 를 기다려야 한다는 규격 요구.
 * 16. 링크 업을 최대 1초 기다린다. 실패해도 오류로 처리하지 않고 로그만 남긴다 --
 *     슬롯이 비어 있는 것은 정상 상황이기 때문이다.
 * 항상 0 을 반환한다(단계 1과 5의 실패만 조기 반환한다).
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. msleep 으로 200ms 이상 잠들므로
 * 인터럽트 컨텍스트에서는 절대 불릴 수 없다. 이 시점에는 아직 인터럽트가
 * 설정되지 않았고(코어가 이 훅 이후에 plda_init_interrupts 를 부른다)
 * PCI 버스도 없어, 동시 접근자가 없으므로 락을 전혀 쓰지 않는다.
 * 호출자: 코어가 host_ops->host_init 으로 간접 호출.
 * 피호출자: starfive_pcie_enable_phy, regmap_update_bits(여러 번),
 * starfive_pcie_clk_rst_init, regulator_enable, gpiod_set_value_cansleep,
 * plda_pcie_disable_func / _enable_root_port / _write_rc_bar /
 * _set_standard_class / _disable_ltr / _set_pref_win_64bit, msleep,
 * starfive_pcie_host_wait_for_link.
 * 에러 경로: PHY 실패와 클럭/리셋 실패만 조기 반환한다. 레귤레이터 enable 실패는
 * 로그만 남기고 계속 진행하는데(반환값을 ret 에 넣고도 검사하지 않는다),
 * 코드가 그렇게 되어 있다는 사실만 적어 둔다.
 *
 * 호출 체인:
 *   starfive_pcie_probe() -> plda_pcie_host_init() -> host_ops->host_init
 *     = [이 함수] -> starfive_pcie_host_wait_for_link()
 */
static int starfive_pcie_host_init(struct plda_pcie_rp *plda)
{
	struct starfive_jh7110_pcie *pcie =
		container_of(plda, struct starfive_jh7110_pcie, plda);
	/* [한국어] 로그와 dev_err_probe 의 기준 device. plda 에서 꺼내 지역 변수로 둔다. */
	struct device *dev = plda->dev;
	/* [한국어] 각 단계의 반환값을 담는다. */
	int ret;
	/* [한국어] 물리 함수 비활성화 루프의 인덱스. */
	int i;

	/* [한국어] 1단계: PHY 초기화와 전원 인가. 물리 계층이 준비되기 전에는 아무것도 할 수 없어
	 * 가장 먼저 한다. */
	ret = starfive_pcie_enable_phy(dev, pcie);
	/* [한국어] PHY 실패는 회복 불가이므로 곧장 반환한다. 코어가 이 값을 보고 probe 를 접는다. */
	if (ret)
		return ret;

	/* [한국어] 2단계: STG 의 RP_NEP 레지스터에서 K_RP_NEP(bit8)를 세운다.
	 * 이름(Root Port, No EP)으로 보아 이 포트를 Root Port 로 확정하는 설정이지만,
	 * 정확한 의미는 JH7110 데이터시트가 이 트리에 없어 확인할 수 없다.
	 * regmap_update_bits(map, reg, mask, val) 은 mask 범위만 val 로 바꾸는
	 * read-modify-write 이며 regmap 내부 락이 원자성을 보장한다. */
	regmap_update_bits(pcie->reg_syscon,
			   pcie->stg_pcie_base + STG_SYSCON_RP_NEP_OFFSET,
			   STG_SYSCON_K_RP_NEP, STG_SYSCON_K_RP_NEP);

	/* [한국어] 3단계: AW 레지스터의 CKREF_SRC 필드(비트 19:18)를 2 로 설정한다.
	 * PCIe 참조 클럭 소스 선택이며, 값 2 가 어떤 소스인지는 이 트리의 코드만으로는
	 * 알 수 없다. 클럭을 켜기 '전에' 소스를 정해야 하므로 순서가 중요하다. */
	regmap_update_bits(pcie->reg_syscon,
			   pcie->stg_pcie_base + STG_SYSCON_AW_OFFSET,
			   STG_SYSCON_CKREF_SRC_MASK,
			   FIELD_PREP(STG_SYSCON_CKREF_SRC_MASK, 2));

	/* [한국어] 4단계: 같은 AW 레지스터의 CLKREQ(bit22)를 켠다. CLKREQ# 는 엔드포인트가
	 * 클럭을 요청하는 사이드밴드 신호로, 켜지지 않으면 일부 장치에서 링크가
	 * 올라오지 않는다. CKREF_SRC(19:18)와 비트가 겹치지 않아 별도 호출로 나눠도 안전하다. */
	regmap_update_bits(pcie->reg_syscon,
			   pcie->stg_pcie_base + STG_SYSCON_AW_OFFSET,
			   STG_SYSCON_CLKREQ, STG_SYSCON_CLKREQ);

	/* [한국어] 5단계: 클럭을 켜고 리셋을 푼다. 위 STG 설정이 모두 끝난 뒤여야 한다. */
	ret = starfive_pcie_clk_rst_init(pcie);
	/* [한국어] 클럭/리셋 실패는 회복 불가. */
	if (ret)
		return ret;

	/* [한국어] 6단계: 슬롯 3.3V 를 켠다. 보드에 레귤레이터가 있을 때만. */
	if (pcie->vpcie3v3) {
		/* [한국어] 전원을 인가한다. */
		ret = regulator_enable(pcie->vpcie3v3);
		/* [한국어] 실패하면 */
		if (ret)
			/* [한국어] 로그만 남기고 계속 진행한다 -- ret 을 검사하지 않아 실패해도 probe 가 이어진다.
			 * 코드가 그렇게 되어 있다는 사실만 적어 둔다. */
			dev_err_probe(dev, ret, "failed to enable vpcie3v3 regulator\n");
	}

	/* [한국어] 7단계: PERST# 제어가 있는 보드에서만 */
	if (pcie->reset_gpio)
		/* [한국어] PERST# 를 어서트(논리 1)한다. 전원이 막 들어온 엔드포인트가 제멋대로 링크
		 * 훈련을 시작하지 못하도록 리셋 상태에 붙잡아 둔다. cansleep 판을 쓰는 것은
		 * 이 핀이 I2C GPIO 확장기 뒤에 있을 수도 있기 때문이다. */
		gpiod_set_value_cansleep(pcie->reset_gpio, 1);

	/* Disable physical functions except #0 */
	/* [한국어] 8단계: PF1 부터 PF3 까지 순회한다. PF0 은 실제로 쓸 함수이므로 건드리지 않는다. */
	for (i = 1; i < PCIE_FUNC_NUM; i++) {
		/* [한국어] AR(읽기 주소 채널) 필드에 함수 번호 i 를 써 넣는다. */
		regmap_update_bits(pcie->reg_syscon,
				   pcie->stg_pcie_base + STG_SYSCON_AR_OFFSET,
				   STG_SYSCON_AXI4_SLVL_AR_MASK,
				   STG_SYSCON_AXI4_SLVL_PHY_AR(i));

		/* [한국어] AW(쓰기 주소 채널) 필드에도 같은 함수 번호를 써 넣는다. 읽기와 쓰기 채널을
		 * 모두 지정해야 이후의 read-modify-write 가 같은 PF 를 향한다. */
		regmap_update_bits(pcie->reg_syscon,
				   pcie->stg_pcie_base + STG_SYSCON_AW_OFFSET,
				   STG_SYSCON_AXI4_SLVL_AW_MASK,
				   STG_SYSCON_AXI4_SLVL_PHY_AW(i));

		/* [한국어] 그 상태에서 PCI_MISC.PHY_FUNCTION_DIS 를 세워 해당 PF 를 끈다.
		 * 즉 AR/AW 필드가 '이후 브리지 레지스터 접근의 대상 PF' 를 고르는 라우팅으로
		 * 동작한다는 뜻이다 -- 이 해석은 이 사용 패턴에서 나온 추론이며,
		 * JH7110 STG 데이터시트는 이 트리에 없어 직접 확인하지는 못했다. */
		plda_pcie_disable_func(plda);
	}

	/* [한국어] 9단계: AR 필드를 0 으로 되돌린다. */
	regmap_update_bits(pcie->reg_syscon,
			   pcie->stg_pcie_base + STG_SYSCON_AR_OFFSET,
			   STG_SYSCON_AXI4_SLVL_AR_MASK, 0);
	/* [한국어] AW 필드도 0 으로 되돌린다. 이후 브리지 레지스터 접근이 PF0 을 향하게 하기
	 * 위한 것으로, 이 두 줄이 없으면 아래의 모든 설정이 PF3 에 적용된다. */
	regmap_update_bits(pcie->reg_syscon,
			   pcie->stg_pcie_base + STG_SYSCON_AW_OFFSET,
			   STG_SYSCON_AXI4_SLVL_AW_MASK, 0);

	/* [한국어] 10단계: GEN_SETTINGS.RP_ENABLE 을 세워 루트 포트 동작을 켠다. */
	plda_pcie_enable_root_port(plda);
	/* [한국어] 루트 포트 자신의 BAR0/BAR1 을 0 으로 만든다. 엔드포인트의 DMA 가 브리지 내부
	 * 레지스터에 닿지 못하게 하는 하드웨어 쪽 차단이며, 소프트웨어 쪽 차단은
	 * starfive_pcie_hide_rc_bar() 가 담당한다. */
	plda_pcie_write_rc_bar(plda, 0);

	/* PCIe PCI Standard Configuration Identification Settings. */
	/* [한국어] Class Code 를 PCI-to-PCI 브리지(0x060400)로 고친다. 이 값이라야 커널 PCI 코어가
	 * 하위 버스를 열거한다. */
	plda_pcie_set_standard_class(plda);

	/*
	 * The LTR message receiving is enabled by the register "PCIe Message
	 * Reception" as default, but the forward id & addr are uninitialized.
	 * If we do not disable LTR message forwarding here, or set a legal
	 * forwarding address, the kernel will get stuck.
	 * To workaround, disable the LTR message forwarding here before using
	 * this feature.
	 */
	/* [한국어] 11단계: LTR 메시지 전달을 끈다. 위 영문 주석대로, 전달 주소가 초기화되지 않은
	 * 채로 켜져 있으면 커널이 멈춘다. */
	plda_pcie_disable_ltr(plda);

	/*
	 * Enable the prefetchable memory window 64-bit addressing in JH7110.
	 * The 64-bits prefetchable address translation configurations in ATU
	 * can be work after enable the register setting below.
	 */
	/* [한국어] 12단계: prefetchable 창의 64비트 주소 지원을 켠다. 위 영문 주석대로,
	 * 이 설정 뒤에야 ATU 의 64비트 주소 변환이 실제로 동작한다. */
	plda_pcie_set_pref_win_64bit(plda);

	/*
	 * Ensure that PERST has been asserted for at least 100 ms,
	 * the sleep value is T_PVPERL from PCIe CEM spec r2.0 (Table 2-4)
	 */
	/* [한국어] 13단계: 100ms 잠든다. 위 영문 주석의 T_PVPERL -- PCIe CEM 규격 r2.0 Table 2-4 가
	 * 정한 '전원 안정 후 PERST 해제까지의 최소 시간' 이다. 이 대기를 지키지 않으면
	 * 장치가 준비되기 전에 리셋이 풀려 링크가 불안정해진다. */
	msleep(100);
	/* [한국어] 14단계: PERST# 제어가 있는 보드에서만 */
	if (pcie->reset_gpio)
		/* [한국어] PERST# 를 디어서트(논리 0)한다. 이 순간부터 엔드포인트가 링크 훈련을 시작한다. */
		gpiod_set_value_cansleep(pcie->reset_gpio, 0);

	/*
	 * With a Downstream Port (<=5GT/s), software must wait a minimum
	 * of 100ms following exit from a conventional reset before
	 * sending a configuration request to the device.
	 */
	/* [한국어] 15단계: 리셋 해제 후 첫 Configuration Request 까지 최소 100ms 를 기다린다.
	 * PCIE_RESET_CONFIG_WAIT_MS 는 drivers/pci/pci.h 에 100 으로 정의되어 있다.
	 * 위 영문 주석이 5GT/s 이하 Downstream Port 에 대한 이 규격 요구를 설명한다. */
	msleep(PCIE_RESET_CONFIG_WAIT_MS);

	/* [한국어] 16단계: 최대 1초 동안 링크 업을 기다린다. */
	if (starfive_pcie_host_wait_for_link(pcie))
		/* [한국어] 타임아웃해도 오류로 처리하지 않고 정보성 로그만 남긴다 -- 슬롯이 비어 있는
		 * 것은 정상 상황이며, 링크가 없어도 루트 포트 자체는 열거되어야 하기 때문이다. */
		dev_info(dev, "port link down\n");

	/* [한국어] 항상 0 을 반환한다. 조기 반환하는 실패는 PHY(1단계)와 클럭/리셋(5단계)뿐이다. */
	return 0;
}

/*
 * [한국어] 코어가 부를 SoC 초기화/해제 훅 테이블.
 *
 * 이 두 줄이 "코어가 JH7110 을 켜고 끄는" 유일한 통로다. 코어는 이 함수들의
 * 내용을 전혀 모르고, 정해진 시점에 부르기만 한다.
 * 설정자: 컴파일 타임 상수. 읽는 자: starfive_pcie_probe() 가 plda->host_ops 에
 * 대입하고, plda_pcie_host_init()/_deinit() 이 두 겹 NULL 검사 후 호출한다.
 * 동기화: 상수.
 */
static const struct plda_pcie_host_ops sf_host_ops = {
	.host_init = starfive_pcie_host_init,
	.host_deinit = starfive_pcie_host_deinit,
};

/*
 * [한국어] 코어에 알려 주는 INTx/MSI 이벤트 hwirq 번호.
 *
 *  - intx_event = EVENT_PM_MSI_INT_INTX = PLDA_NUM_DMA_EVENTS(16) + PLDA_INTX(8) = 24
 *  - msi_event  = EVENT_PM_MSI_INT_MSI  = 16 + PLDA_MSI(9) = 25
 * request_event_irq 는 지정하지 않으므로(암시적 NULL) 코어가 기본
 * devm_request_irq(plda_event_handler) 를 쓴다. Microchip 의 mc_event 가 여기에
 * 콜백을 채우고 번호도 23/24 로 다른 것과 비교하면, 이 구조체가 왜 필요한지가
 * 분명해진다 -- 앞에 놓인 이벤트 개수가 SoC 마다 다르기 때문이다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_init_interrupts() 가 값만 읽고 버린다.
 * 동기화: 상수.
 */
static const struct plda_event stf_pcie_event = {
	.intx_event = EVENT_PM_MSI_INT_INTX,
	.msi_event  = EVENT_PM_MSI_INT_MSI
};

/*
 * [한국어]
 * starfive_pcie_probe - 드라이버 진입점. DT 를 읽고 코어에 제어를 넘긴다
 *
 * @pdev: 플랫폼 버스가 starfive,jh7110-pcie 노드와 매칭시켜 만든 디바이스.
 * @return: 0 성공, 음수 errno 실패(-EPROBE_DEFER 포함).
 *
 * 왜 필요한가: 커널 디바이스 모델에서 이 함수가 하드웨어와 소프트웨어가 처음
 * 만나는 지점이다. 여기서 자원을 확보하고, 코어가 이해하는 형태로 설정을
 * 만들어 넘긴다.
 *
 * 동작 단계:
 *  1. struct starfive_jh7110_pcie 를 devm_kzalloc 으로 잡는다. devm 이므로
 *     이후 어떤 실패 경로에서도 명시적 free 가 필요 없다.
 *  2. plda.dev 를 채운다. 이 필드가 없으면 코어의 거의 모든 함수가 동작하지 않는다.
 *  3. devicetree 를 파싱해 자원을 확보한다.
 *  4. 런타임 PM 을 켜고 참조를 잡아 디바이스를 활성 상태로 만든다.
 *  5. host_ops 를 지정해 코어가 이 파일의 host_init/host_deinit 을 부르게 한다.
 *  6. num_events 와 events_bitmap 을 계산한다. 이 비트맵 계산이 이 함수에서
 *     가장 까다로운 부분으로, 아래 인라인 주석 참조.
 *  7. plda_pcie_host_init() 에 제어를 넘긴다. 이 호출 안에서 5번의 host_init 이
 *     불리고, ATR 창과 인터럽트가 설정되고, PCI 버스가 스캔된다.
 *  8. 실패하면 런타임 PM 을 되돌린 뒤 반환한다.
 *  9. 성공하면 drvdata 에 저장해 remove/suspend/resume 이 찾을 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있고, 실제로 200ms 이상 잠든다.
 * 호출자: 커널 플랫폼 버스 코어(driver->probe).
 * 피호출자: devm_kzalloc, starfive_pcie_parse_dt, pm_runtime_enable,
 * pm_runtime_get_sync, plda_pcie_host_init, platform_set_drvdata.
 * 에러 경로: parse_dt 실패는 PM 을 켜기 전이라 그냥 반환하고, host_init 실패는
 * pm_runtime_put_sync + pm_runtime_disable 로 4번을 되돌린 뒤 반환한다.
 * devm 자원은 커널이 자동 해제한다.
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 -> [이 함수] -> plda_pcie_host_init() ->
 *     starfive_pcie_host_init() -> plda_init_interrupts() -> pci_host_probe()
 */
static int starfive_pcie_probe(struct platform_device *pdev)
{
	struct starfive_jh7110_pcie *pcie;
	/* [한국어] 로그와 devm_ 할당의 기준 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 코어에 넘길 구조체를 가리킬 지역 포인터. 반복 타이핑을 줄이기 위한 것이다. */
	struct plda_pcie_rp *plda;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] 컨트롤러 구조체를 0 초기화해 할당한다. devm_ 이므로 이후 어떤 실패 경로에서도
	 * 명시적 해제가 필요 없다. GFP_KERNEL 은 잠들 수 있는 할당이라는 뜻으로,
	 * probe 가 프로세스 컨텍스트이기에 쓸 수 있다. */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!pcie)
		return -ENOMEM;

	/* [한국어] 코어 구조체는 이 구조체의 첫 필드다. 코어에는 이 주소만 넘어간다. */
	plda = &pcie->plda;
	/* [한국어] 코어가 쓸 device 포인터를 채운다. 이 한 줄이 없으면 코어의 거의 모든 함수가
	 * NULL 역참조로 죽는다. */
	plda->dev = dev;

	/* [한국어] devicetree 에서 클럭/리셋/PHY/syscon/GPIO/레귤레이터와 도메인 번호를 확보한다. */
	ret = starfive_pcie_parse_dt(pcie, dev);
	/* [한국어] -EPROBE_DEFER 를 포함한 모든 실패를 그대로 올려보낸다. 아직 PM 을 켜기 전이라
	 * 되돌릴 것이 없다. */
	if (ret)
		return ret;

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	/* [한국어] 코어가 부를 SoC 초기화/해제 훅을 지정한다. 이것이 이 파일과 코어를 잇는 고리다. */
	plda->host_ops = &sf_host_ops;
	/* [한국어] 이벤트 도메인의 크기를 PLDA_MAX_EVENT_NUM(= 16 + 13 = 29)으로 잡는다.
	 * hwirq 0..28 이 유효해진다. */
	plda->num_events = PLDA_MAX_EVENT_NUM;
	/* mask doorbell event */
	/* [한국어] 실제로 쓸 이벤트만 고른다. GENMASK(PLDA_INT_EVENT_NUM - 1, 0) = 비트 0..12,
	 * 즉 enum plda_int_event 의 13개 이벤트를 전부 켜는 것에서 시작한다. */
	plda->events_bitmap = GENMASK(PLDA_INT_EVENT_NUM - 1, 0)
			     /* [한국어] AXI doorbell(enum 3)을 뺀다 -- A_ATR_EVT_DOORBELL_MASK 가 0x00000000 이라
			      * 하드웨어가 구현하지 않은 이벤트이기 때문이다. */
			     & ~BIT(PLDA_AXI_DOORBELL)
			     /* [한국어] PCIe doorbell(enum 7)도 같은 이유로 뺀다. 위 주석 'mask doorbell event' 가
			      * 이것을 말한다. */
			     & ~BIT(PLDA_PCIE_DOORBELL);
	/* [한국어] 13비트 마스크를 PLDA_NUM_DMA_EVENTS(16)만큼 왼쪽으로 밀어 실제 hwirq 위치로
	 * 옮긴다. 결과는 비트 16..28 중 19 와 23 이 빠진 값이다. 이 시프트를 빼먹으면
	 * 코어가 DMA 이벤트 자리(0..15)에 IRQ 를 매핑하려 들어 전혀 다른 동작이 된다. */
	plda->events_bitmap <<= PLDA_NUM_DMA_EVENTS;
	/* [한국어] 코어에 제어를 넘긴다. 이 호출 안에서 ioremap, host_init 훅, ATR 창,
	 * 인터럽트 설정, pci_host_probe(버스 스캔)가 모두 일어난다 -- 즉 이 한 줄이
	 * 끝나면 PCIe 장치들이 이미 발견되어 동작 중이다. */
	ret = plda_pcie_host_init(&pcie->plda, &starfive_pcie_ops,
				  &stf_pcie_event);
	/* [한국어] 코어 실패 -- PM 을 켠 뒤이므로 되돌려야 한다. */
	if (ret) {
		pm_runtime_put_sync(&pdev->dev);
		pm_runtime_disable(&pdev->dev);
		return ret;
	}

	/* [한국어] 성공했으므로 drvdata 에 저장한다. remove 와 suspend/resume 이 이것으로
	 * 컨트롤러를 되찾는다. */
	platform_set_drvdata(pdev, pcie);

	return 0;
}

/*
 * [한국어]
 * starfive_pcie_remove - 드라이버 해제. PM 참조를 놓고 코어에 해제를 맡긴다
 *
 * @pdev: 해제할 플랫폼 디바이스.
 * @return: 없음(최신 커널의 platform_driver.remove 는 void 반환이다).
 *
 * 왜 필요한가: 모듈 언로드나 디바이스 언바인드 시 하드웨어를 안전하게 내려야 한다.
 *
 * 동작 단계:
 *  1. drvdata 에서 컨트롤러를 되찾는다.
 *  2. pm_runtime_put / pm_runtime_disable 로 probe 의 4번 단계를 되돌린다.
 *     put_sync 가 아니라 put 을 쓰는 점이 probe 의 get_sync 와 비대칭이다.
 *  3. plda_pcie_host_deinit() 으로 PCI 버스와 인터럽트를 정리한다.
 *     그 안에서 다시 이 파일의 starfive_pcie_host_deinit() 이 불려 전원이 내려간다.
 *  4. drvdata 를 NULL 로 만든다.
 *
 * 주의: 3번(PCI 버스 제거)보다 2번(PM 비활성화)이 먼저다. 즉 아직 장치 드라이버가
 * 붙어 있을 수 있는 상태에서 런타임 PM 을 끈다. 코드가 그렇게 되어 있다는 사실만
 * 적어 두며, 이 순서가 문제가 되는지는 이 파일만으로는 판단할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: 커널 플랫폼 버스 코어(driver->remove).
 * 피호출자: platform_get_drvdata, pm_runtime_put, pm_runtime_disable,
 * plda_pcie_host_deinit, platform_set_drvdata.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   모듈 언로드 -> [이 함수] -> plda_pcie_host_deinit()
 *     -> starfive_pcie_host_deinit()
 */
static void starfive_pcie_remove(struct platform_device *pdev)
{
	struct starfive_jh7110_pcie *pcie = platform_get_drvdata(pdev);

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	plda_pcie_host_deinit(&pcie->plda);
	/* [한국어] drvdata 를 지운다. 이후 platform_get_drvdata 는 NULL 을 돌려준다. */
	platform_set_drvdata(pdev, NULL);
}

/*
 * [한국어]
 * starfive_pcie_suspend_noirq - 시스템 서스펜드 시 클럭과 PHY 를 내린다
 *
 * @dev: 이 컨트롤러의 device. drvdata 에 JH7110 구조체가 들어 있다.
 * @return: 항상 0.
 *
 * 왜 필요한가: 서스펜드 중에 PCIe PHY 와 클럭이 계속 돌면 전력을 낭비한다.
 * _noirq 단계인 이유는 이 시점에는 이미 인터럽트가 비활성화되어 있어, 클럭을
 * 끊는 동안 인터럽트 핸들러가 죽은 레지스터에 접근할 위험이 없기 때문이다.
 *
 * 동작: 클럭 묶음을 끄고 PHY 를 내린다. 리셋은 걸지 않는데, 이는
 * starfive_pcie_clk_rst_deinit() 과 다른 점이다 -- 리셋을 걸면 브리지 레지스터
 * 설정이 날아가 resume 때 전부 다시 해야 하기 때문으로 보인다(코드에 근거가
 * 명시되어 있지는 않다).
 *
 * 실행 컨텍스트: 시스템 서스펜드의 noirq 단계. 인터럽트가 꺼진 상태이지만
 * 프로세스 컨텍스트이므로 잠들 수는 있다.
 * 호출자: PM 코어가 dev_pm_ops 를 통해 호출. NOIRQ_SYSTEM_SLEEP_PM_OPS 매크로가
 * suspend_noirq/resume_noirq/freeze_noirq/thaw_noirq/poweroff_noirq/restore_noirq
 * 전부에 이 짝을 연결한다.
 * 피호출자: dev_get_drvdata, clk_bulk_disable_unprepare, starfive_pcie_disable_phy.
 * 에러 경로: 없음 -- 항상 0 을 돌려주므로 서스펜드가 이 드라이버 때문에 실패하지 않는다.
 *
 * 호출 체인:
 *   PM 코어(suspend_noirq 단계) -> [이 함수]
 */
static int starfive_pcie_suspend_noirq(struct device *dev)
{
	struct starfive_jh7110_pcie *pcie = dev_get_drvdata(dev);

	/* [한국어] 클럭을 끈다. 리셋은 걸지 않는데, 리셋을 걸면 브리지 레지스터 설정이 날아가
	 * resume 때 전부 다시 해야 하기 때문으로 보인다(코드에 근거가 명시되어 있지는 않다). */
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
	starfive_pcie_disable_phy(pcie);

	return 0;
}

/*
 * [한국어]
 * starfive_pcie_resume_noirq - 시스템 복귀 시 PHY 와 클럭을 되살린다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 성공, 음수 errno 실패(복귀 실패는 PM 코어에 보고된다).
 *
 * 왜 필요한가: suspend_noirq 의 짝. 인터럽트가 다시 켜지기 전에 하드웨어가
 * 동작 가능한 상태로 돌아와 있어야 한다.
 *
 * 동작 단계(suspend 의 역순):
 *  1. PHY 를 먼저 켠다 -- 클럭보다 PHY 가 먼저인 것이 suspend 의 역순이다.
 *  2. 클럭 묶음을 켠다.
 *  3. 클럭 켜기가 실패하면 방금 켠 PHY 를 되돌리고 오류를 반환한다.
 *     부분 성공 상태를 남기지 않기 위한 처리다.
 *
 * 주의: 링크 재훈련이나 PERST 토글을 하지 않는다. 서스펜드 중에도 슬롯 전원과
 * 리셋 상태가 유지된다는 전제다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계. 프로세스 컨텍스트이며 잠들 수 있다.
 * 호출자: PM 코어가 dev_pm_ops 를 통해 호출.
 * 피호출자: dev_get_drvdata, starfive_pcie_enable_phy, clk_bulk_prepare_enable,
 * dev_err, starfive_pcie_disable_phy.
 * 에러 경로: 위 3번 참조.
 *
 * 호출 체인:
 *   PM 코어(resume_noirq 단계) -> [이 함수]
 */
static int starfive_pcie_resume_noirq(struct device *dev)
{
	struct starfive_jh7110_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] PHY 켜기와 클럭 켜기의 결과. */
	int ret;

	/* [한국어] suspend 의 역순으로 PHY 를 먼저 켠다. */
	ret = starfive_pcie_enable_phy(dev, pcie);
	/* [한국어] PHY 실패면 클럭은 손대지 않고 그대로 반환. */
	if (ret)
		return ret;

	/* [한국어] 클럭을 켠다. */
	ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	/* [한국어] 클럭 실패 -- 방금 켠 PHY 를 되돌려야 한다. */
	if (ret) {
		/* [한국어] 원인을 남긴다. */
		dev_err(dev, "failed to enable clocks\n");
		starfive_pcie_disable_phy(pcie);
		return ret;
	}

	return 0;
}

/*
 * [한국어] 시스템 절전 콜백 테이블.
 *
 * NOIRQ_SYSTEM_SLEEP_PM_OPS 매크로는 suspend_noirq/resume_noirq 뿐 아니라
 * freeze/thaw/poweroff/restore 의 noirq 단계까지 같은 함수 짝으로 채운다.
 * 즉 하이버네이션 경로도 동일하게 처리된다.
 * noirq 단계를 고른 이유: 이 콜백이 클럭과 PHY 를 끊으므로, 인터럽트 핸들러가
 * 죽은 레지스터를 건드릴 여지가 없는 단계여야 하기 때문이다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: 아래 driver.pm 에 pm_sleep_ptr 로 연결된다.
 * 동기화: 상수.
 */
static const struct dev_pm_ops starfive_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(starfive_pcie_suspend_noirq,
				  starfive_pcie_resume_noirq)
};

/*
 * [한국어] devicetree compatible 문자열 매칭 표.
 *
 * "starfive,jh7110-pcie" 노드 하나만 받는다. Microchip 판과 달리 .data 로
 * ops 를 넘기지 않는데, 이 드라이버는 지원 SoC 가 하나뿐이라 분기가 필요 없기
 * 때문이다. 마지막 빈 항목은 배열의 끝을 알리는 sentinel 로 필수다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: 플랫폼 버스가 노드와 드라이버를 맺을 때.
 * 동기화: 상수.
 */
static const struct of_device_id starfive_pcie_of_match[] = {
	/* [한국어] 이 드라이버가 받아들이는 유일한 compatible 문자열.
	 * devicetree 노드의 compatible 프로퍼티가 이 값과 같아야 probe 가 불린다. */
	{ .compatible = "starfive,jh7110-pcie", },
	/* [한국어] 배열 끝을 알리는 빈 항목. 커널은 .compatible 이 비어 있는 항목을 만나면
	 * 순회를 멈추므로 반드시 있어야 한다. */
	{ /* sentinel */ }
};
/* [한국어] 이 매치 표를 모듈 메타데이터(modules.alias)에 심는다. 그래야 udev 가
 * devicetree 노드를 보고 pcie-starfive.ko 를 자동으로 로드할 수 있다. */
MODULE_DEVICE_TABLE(of, starfive_pcie_of_match);

/*
 * [한국어] 이 파일이 커널에 등록하는 플랫폼 드라이버 정의.
 *
 *  - driver.name : sysfs 와 로그에 나타나는 드라이버 이름.
 *  - of_match_table : of_match_ptr 로 감싸 CONFIG_OF 가 꺼진 빌드에서는 NULL 이
 *    되게 한다. 실제로는 Kconfig 가 OF 를 요구하므로 항상 유효하다.
 *  - pm : pm_sleep_ptr 로 감싸 CONFIG_PM_SLEEP 이 꺼지면 NULL 이 되고, 그러면
 *    위 pm_ops 와 두 콜백이 링커에 의해 제거된다.
 *  - probe / remove : 위에서 정의한 두 함수.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: module_platform_driver 매크로가 만드는
 * 모듈 init/exit 함수가 platform_driver_register/unregister 에 넘긴다.
 * 동기화: 상수.
 */
static struct platform_driver starfive_pcie_driver = {
	.driver = {
		/* [한국어] sysfs 의 /sys/bus/platform/drivers 아래에 나타나는 이름. */
		.name = "pcie-starfive",
		.of_match_table = of_match_ptr(starfive_pcie_of_match),
		.pm = pm_sleep_ptr(&starfive_pcie_pm_ops),
	},
	.probe = starfive_pcie_probe,
	.remove = starfive_pcie_remove,
};
/* [한국어] 모듈 init/exit 함수를 자동 생성해 platform_driver_register/unregister 를 부른다.
 * 이 한 줄이 없으면 드라이버가 커널에 등록되지 않는다. */
module_platform_driver(starfive_pcie_driver);

/* [한국어] modinfo 에 나타나는 모듈 설명. */
MODULE_DESCRIPTION("StarFive JH7110 PCIe host driver");
/* [한국어] 모듈 라이선스. GPL 계열이어야 EXPORT_SYMBOL_GPL 로 노출된 코어 심볼
 * (plda_pcie_host_init 등)을 링크할 수 있다 -- 이 선언이 없거나 비 GPL 이면
 * 모듈 로드가 거부된다. */
MODULE_LICENSE("GPL v2");
