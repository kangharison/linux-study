// SPDX-License-Identifier: GPL-2.0+
/*
 * BRIEF MODULE DESCRIPTION
 *     PCI init for Ralink RT2880 solution
 *
 * Copyright 2007 Ralink Inc. (bruce_chang@ralinktech.com.tw)
 *
 * May 2007 Bruce Chang
 * Initial Release
 *
 * May 2009 Bruce Chang
 * support RT2880/RT3883 PCIe
 *
 * May 2011 Bruce Chang
 * support RT6855/MT7620 PCIe
 */

/* [한국어] GENMASK()/BIT() 비트 마스크 매크로. 아래 레지스터 비트 정의가 전부 이 둘로 표현된다. */
/*
 * [한국어 설명] MediaTek MT7621 SoC 내장 PCIe 호스트 컨트롤러 드라이버 (pcie-mt7621.c)
 *
 * === 파일의 역할 ===
 * MediaTek MT7621(그 조상인 Ralink RT2880/RT3883/RT6855 계열 포함) SoC 안에
 * 들어 있는 3포트 PCIe 루트 컴플렉스를 초기화하고 리눅스 PCI 서브시스템에
 * 등록하는 드라이버다. PCI 장치가 아니라 SoC 내부 블록이므로 플랫폼 드라이버로
 * 등록되고, 자기 서술을 config space 가 아니라 디바이스 트리에서 읽는다.
 * 하는 일은 크게 다섯이다. (1) DT 에서 공용 레지스터 창 하나와 포트별 창 세 개,
 * 그리고 포트마다의 클럭·리셋·PHY·GPIO 를 모아 객체로 만든다. (2) RC 와 EP 양쪽
 * 리셋을 걸었다 순서대로 푸는 시퀀스로 링크를 학습시킨다. (3) 살아 있는 포트만
 * 골라 클럭을 켜고, 인터럽트 허용 비트·인바운드 DMA 창·class code·FTS 개수를
 * 설정한다. (4) config space 접근을 x86 의 0xCF8/0xCFC 와 같은 주소/데이터
 * 레지스터 쌍으로 중계한다. (5) 준비가 끝나면 pci_host_probe() 로 코어에 넘긴다.
 * 이 하드웨어의 두 가지 특이점이 코드 전반을 지배한다 — 리셋 신호의 극성이
 * 칩 리비전(E2)에 따라 뒤집히고, 포트 0 과 포트 1 이 SerDes PHY 하나를 공유한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층을 위에서부터 보면 PCI 코어(probe.c/bus.c) → 호스트 브리지 추상화
 * (pci_host_bridge) → 이 파일 → SoC 하드웨어다. 즉 PCI 스택의 최하단, 실제
 * 레지스터를 두드리는 자리에 있다. 진입은 세 방향이다. 첫째, 부팅 시 DT 매칭으로
 * mt7621_pcie_probe() 가 불려 위 다섯 단계를 순서대로 밟는다. 둘째, 버스 스캔이
 * 시작되면 PCI 코어가 config 접근마다 mt7621_pcie_map_bus() 를 되부른다 —
 * 이때는 전역 pci_lock 을 쥔 상태이므로, 주소 레지스터에 쓰고 데이터 레지스터를
 * 읽는 2단계 접근이 원자적으로 보장된다. 셋째, 언바인드 시 mt7621_pcie_remove()
 * 가 devm 이 아닌 자원만 반납한다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. probe 경로는 msleep 이 세 번 들어가
 * 총 300ms 가량 잠들고, config 접근 경로는 잠들지 않는 단순 MMIO 다.
 * 이 드라이버는 builtin_platform_driver 로 등록되어 커널에 내장된다 —
 * SoC 부팅 초기에 필요한 버스라 모듈로 미룰 수 없기 때문이며, 그래서 모듈
 * 언로드 경로가 없고 remove() 는 sysfs 수동 언바인드에서만 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci.h 의 devm_pci_alloc_host_bridge(), pci_host_bridge_priv(),
 * pci_host_probe(), pci_generic_config_read/write, PCI_SLOT/PCI_FUNC.
 * 그리고 "../pci.h" 로 끌어오는 서브시스템 내부 매크로 PCI_CONF1_EXT_ADDRESS() —
 * 표준 CF8 주소(bit31 enable | bus<<16 | dev<<11 | func<<8 | reg&0xfc)에
 * reg[11:8] 을 [27:24] 로 옮긴 확장 비트를 더해, ECAM 없이도 4KB 확장 config
 * 영역에 닿게 해 주는 비표준 관용이다.
 * 아래쪽: 네 개의 프레임워크에 각각 의존한다. 클럭(clk_prepare_enable),
 * 리셋(of_reset_control_get_exclusive/assert/deassert/put), PHY(phy_init/
 * phy_power_on/phy_power_off/phy_exit), GPIO(devm_gpiod_get_index_optional/
 * gpiod_set_value). 리셋만 devm_ 판이 아니라서 해제 책임이 이 파일에 남고,
 * 그것이 remove_reset 라벨과 remove() 의 존재 이유다.
 * 옆쪽: sys_soc.h 의 soc_device_match() 로 칩 리비전을 조회해 리셋 극성을 정한다.
 * DT: of_pci_get_devfn() 으로 자식 노드의 슬롯 번호를,
 * for_each_available_child_of_node_scoped() 로 활성 포트만 순회한다.
 * 데이터 흐름: DT 서술 → struct mt7621_pcie/mt7621_pcie_port → 레지스터 쓰기 →
 * 링크 학습 → 링크 상태 읽기 → 살아 있는 포트만 활성화 → pci_host_probe() →
 * PCI 코어의 버스 스캔 → config 접근이 다시 이 파일의 map_bus 로 돌아온다.
 * 공유 상태: pcie->base(공용 레지스터 창)와 pcie->ports 리스트. 둘 다 probe
 * 경로에서만 변경되고 이후 읽기 전용이라 이 파일에는 락이 하나도 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mt7621_pcie: 컨트롤러 하나. dev / base(공용 창) / ports(포트 리스트) /
 *   resets_inverted(E2 리비전 리셋 극성 뒤집힘) 네 필드뿐이다.
 * - struct mt7621_pcie_port: 포트 하나. base(전용 창) / clk / phy / pcie_rst /
 *   gpio_rst / slot / enabled 와 리스트 노드. 이 중 pcie_rst 만 devm 자원이 아니다.
 * - mt7621_pcie_probe(): 진입점. DT 확인 → 브리지 할당 → 리비전 quirk 판정 →
 *   parse_dt → init_ports → enable_ports → register_host.
 * - mt7621_pcie_init_ports(): 이 파일의 심장. 리셋 시퀀스(RC·EP 동시 assert →
 *   RC 만 deassert → PHY 초기화 → EP deassert)를 밟고 링크를 판정하며,
 *   포트 0/1 의 PHY 공유를 특별 처리한다. 세 포트가 모두 죽었을 때만 -ENODEV.
 * - mt7621_pcie_enable_port(): 살아 있는 포트에 네 가지를 설정한다 —
 *   인터럽트 허용 비트, 2GB 인바운드 DMA 창(BAR0), PCI-to-PCI 브리지 class code,
 *   그리고 L0s 탈출용 FTS 개수. class code 를 써 주지 않으면 코어가 하위 버스를
 *   열거하지 않고, 인바운드 창을 열지 않으면 엔드포인트의 DMA 가 전부 실패한다.
 * - mt7621_pcie_map_bus(): config 접근 주소 계산. read/write 는 코어의 범용
 *   구현에 위임하므로 이 콜백 하나만 구현하면 된다.
 * - mt7621_control_assert()/deassert(): 리셋 극성 뒤집힘을 안으로 숨기는 래퍼.
 *   정상 칩에서 "논리적 assert" 가 reset_control_deassert() 로 내려가는,
 *   이 파일에서 가장 헷갈리는 부분이다.
 * - read_config()/write_config(): 버스 스캔 이전 단계에서 루트 컴플렉스 자신의
 *   config 레지스터에 닿기 위한 헬퍼. 유일한 용도는 FTS 개수 설정이다.
 * - [상류 코드 관찰, 수정하지 않음] 자원 정리가 일관되지 않다. 링크가 하나도
 *   없을 때 probe 는 오류를 로그로 남기고도 0 을 돌려주며 리셋을 반납하지 않고,
 *   parse_dt 실패 경로와 init_ports 의 list_del 경로에서도 리셋이 누수된다.
 *   remove_resets 라벨을 실제로 거치는 것은 enable_ports 실패 하나뿐이다.
 *   또 enable_port() 위의 영어 주석은 FTS 를 250 이라 적었지만 실제 값은 0x50(80)이다.
 */

#include <linux/bitops.h>
/* [한국어] clk_prepare_enable() 등 클럭 게이트 제어 API. 포트마다 별도의 클럭 게이트가 있어
 * 링크가 살아 있는 포트만 클럭을 켠다. */
#include <linux/clk.h>
/* [한국어] msleep() 선언. 리셋 해제 후 하드웨어가 안정될 때까지 기다리는 데 쓴다. */
#include <linux/delay.h>
/* [한국어] gpiod_set_value()/devm_gpiod_get_index_optional() 등 GPIO 컨슈머 API.
 * PERST#(엔드포인트 리셋) 신호가 SoC 내부 레지스터가 아니라 범용 GPIO 핀에
 * 연결되어 있어 이 API 로 직접 토글해야 한다. */
#include <linux/gpio/consumer.h>
/* [한국어] MODULE_DESCRIPTION/MODULE_LICENSE/MODULE_DEVICE_TABLE 매크로. */
#include <linux/module.h>
/* [한국어] 디바이스 트리 노드 순회 매크로 for_each_available_child_of_node_scoped() 와
 * device_node 타입 정의. */
#include <linux/of.h>
/* [한국어] of_address 계열 헬퍼. 이 파일은 주소 변환을 직접 하지 않고
 * devm_platform_ioremap_resource() 에 맡기지만, DT 자원 처리 경로가 이 헤더에 의존한다. */
#include <linux/of_address.h>
/* [한국어] of_pci_get_devfn() 선언 — 자식 노드의 reg 속성 첫 셀에서 devfn 을 뽑아낸다.
 * 포트마다 슬롯 번호를 얻는 유일한 경로다. */
#include <linux/of_pci.h>
/* [한국어] to_platform_device() 등 OF 플랫폼 디바이스 연결 헬퍼. */
#include <linux/of_platform.h>
/* [한국어] PCI 코어 공개 API — pci_host_bridge, devm_pci_alloc_host_bridge(),
 * pci_host_probe(), pci_generic_config_read/write, PCI_SLOT/PCI_FUNC,
 * PCI_BASE_ADDRESS_0 이 여기서 온다. */
#include <linux/pci.h>
/* [한국어] PHY 서브시스템 API — phy_init()/phy_power_on()/phy_power_off()/phy_exit().
 * MT7621 의 PCIe SerDes 는 별도 PHY 드라이버가 관리하므로 이 파일은 그 API 만 호출한다. */
#include <linux/phy/phy.h>
/* [한국어] platform_driver / platform_get_drvdata / devm_platform_ioremap_resource 선언.
 * 이 컨트롤러는 PCI 장치가 아니라 SoC 내부 플랫폼 장치이므로 플랫폼 드라이버로 등록된다. */
#include <linux/platform_device.h>
/* [한국어] reset_control_assert/deassert/put 과 of_reset_control_get_exclusive 선언.
 * 포트별 리셋 라인을 개별 제어하기 위해 필요하다. */
#include <linux/reset.h>
/* [한국어] soc_device_match() 와 struct soc_device_attribute 선언.
 * 칩 리비전에 따라 리셋 신호 극성이 뒤집히는 문제를 런타임에 판별하는 데 쓴다. */
#include <linux/sys_soc.h>

/* [한국어] PCI 서브시스템 내부 헤더(drivers/pci/pci.h). 외부 모듈에는 공개되지 않는
 * PCI_CONF1_EXT_ADDRESS() 매크로를 쓰기 위해 상대 경로로 포함한다.
 * 그 매크로는 CF8 형식 주소(PCI_CONF1_ADDRESS: bit31 enable | bus<<16 | dev<<11 |
 * func<<8 | reg&0xfc)에 확장 레지스터 비트(reg 의 [11:8] 을 [27:24] 로 옮긴 것)를
 * OR 로 덧붙여, 256바이트를 넘는 PCIe 확장 config 영역까지 CF8 방식으로 접근하게 해 준다.
 * 표준이 아닌 관용이며, ECAM 을 지원하지 않는 ARM SoC 들이 널리 쓴다. */
#include "../pci.h"

/* MediaTek-specific configuration registers */
/* [한국어] L0s 탈출 시 보낼 FTS(Fast Training Sequence) 개수를 담는 레지스터 오프셋.
 * 0x70c 는 MMIO 창의 오프셋이 아니라 config space 안의 벤더 전용 오프셋이며,
 * 그래서 아래 read_config()/write_config() 로 접근한다. */
#define PCIE_FTS_NUM			0x70c
/* [한국어] 그 레지스터에서 FTS 개수가 차지하는 비트 [15:8]. */
#define PCIE_FTS_NUM_MASK		GENMASK(15, 8)
/* [한국어] 값 x 를 [15:8] 자리에 놓는 매크로. 마스크를 먼저 씌운 뒤 시프트하도록
 * 괄호가 정확히 잡혀 있다 — 연산자 우선순위 함정을 피한 형태다. */
#define PCIE_FTS_NUM_L0(x)		(((x) & 0xff) << 8)

/* Host-PCI bridge registers */
/* [한국어] 호스트-PCI 브리지 제어 레지스터. 이 파일에서는 정의만 있고 참조하지 않는다. */
#define RALINK_PCI_PCICFG_ADDR		0x0000
/* [한국어] 인터럽트 마스크 레지스터 오프셋. 포트별 인터럽트 허용 비트를 담는다. */
#define RALINK_PCI_PCIMSK_ADDR		0x000c
/* [한국어] CF8 형식 주소를 쓰는 레지스터. 여기에 주소를 쓴 뒤 CONFIG_DATA 를 읽거나 쓰면
 * 그 주소의 config space 에 접근된다 — x86 의 0xCF8/0xCFC 쌍과 같은 구조다. */
#define RALINK_PCI_CONFIG_ADDR		0x0020
/* [한국어] CF8 형식 데이터 창 레지스터. 위 ADDR 과 짝을 이룬다. */
#define RALINK_PCI_CONFIG_DATA		0x0024
/* [한국어] 하위 장치가 볼 메모리 창의 베이스를 설정하는 레지스터. */
#define RALINK_PCI_MEMBASE		0x0028
/* [한국어] 하위 장치가 볼 I/O 창의 베이스를 설정하는 레지스터. */
#define RALINK_PCI_IOBASE		0x002c

/* PCIe RC control registers */
/* [한국어] 포트별 RC 제어 영역의 Vendor/Device ID 레지스터. 정의만 있고 참조하지 않는다. */
#define RALINK_PCI_ID			0x0030
/* [한국어] 포트별 RC 제어 영역의 Class Code/Revision ID 레지스터.
 * 링크가 살아난 포트마다 여기에 브리지 class 를 써 넣는다. */
#define RALINK_PCI_CLASS		0x0034
/* [한국어] Subsystem ID 레지스터. 정의만 있고 참조하지 않는다. */
#define RALINK_PCI_SUBID		0x0038
/* [한국어] 포트 상태 레지스터. bit 0 이 링크 업 여부를 나타낸다. */
#define RALINK_PCI_STATUS		0x0050

/* Some definition values */
/* [한국어] Revision ID 를 1 로 설정하기 위한 비트. 아래에서 class code 와 OR 로 합쳐
 * RALINK_PCI_CLASS 에 한 번에 쓴다. */
#define PCIE_REVISION_ID		BIT(0)
/* [한국어] PCI-to-PCI 브리지 class code(0x060400)를 레지스터의 [31:8] 자리로 올린 값.
 * class code 는 config space 오프셋 0x08 의 상위 3바이트에 놓이고 하위 1바이트가
 * Revision ID 이므로, 8비트 왼쪽 시프트가 바로 그 배치를 만든다.
 * 이 값을 써 주지 않으면 커널이 루트 포트를 브리지로 인식하지 못한다. */
#define PCIE_CLASS_CODE			(0x60400 << 8)
/* [한국어] 인바운드 BAR 가 덮을 최대 범위를 나타내는 비트 [30:16]. 아래에서 BAR0 에
 * 이 값을 통째로 써서 2GB DDR 전체를 엔드포인트가 볼 수 있게 연다. */
#define PCIE_BAR_MAP_MAX		GENMASK(30, 16)
/* [한국어] 그 BAR 를 활성화하는 비트 0. */
#define PCIE_BAR_ENABLE			BIT(0)
/* [한국어] 포트 x 의 인터럽트를 허용하는 비트. 포트 0/1/2 가 각각 비트 20/21/22 에 대응한다.
 * 비트 위치가 20 부터 시작하는 것은 이 레지스터의 하위 비트들이 다른 용도로
 * 쓰이기 때문이며, 그 용도는 이 파일에서 확인할 수 없다. */
#define PCIE_PORT_INT_EN(x)		BIT(20 + (x))
/* [한국어] RALINK_PCI_STATUS 의 bit 0 — 1 이면 그 포트의 링크가 학습을 마쳤다는 뜻이다. */
#define PCIE_PORT_LINKUP		BIT(0)
/* [한국어] 이 SoC 의 PCIe 포트 개수. 세 포트가 모두 죽었을 때만 probe 를 포기하는
 * 판정 기준으로 쓰인다. */
#define PCIE_PORT_CNT			3

/* [한국어] 포트 초기화 후 링크 학습을 기다리는 시간(밀리초). */
#define INIT_PORTS_DELAY_MS		100
/* [한국어] PERST#(엔드포인트 리셋) 신호를 유지/해제한 뒤 기다리는 시간(밀리초).
 * PCIe 규격이 요구하는 리셋 유지 시간과 전원 안정화 시간을 넉넉히 덮는 값이다. */
#define PERST_DELAY_MS			100

/**
 * struct mt7621_pcie_port - PCIe port information
 * @base: I/O mapped register base
 * @list: port list
 * @pcie: pointer to PCIe host info
 * @clk: pointer to the port clock gate
 * @phy: pointer to PHY control block
 * @pcie_rst: pointer to port reset control
 * @gpio_rst: gpio reset
 * @slot: port slot
 * @enabled: indicates if port is enabled
 */
struct mt7621_pcie_port {
	/* [한국어] 이 포트 전용 레지스터 창의 커널 가상 주소.
	 * 설정자: mt7621_pcie_parse_port() 가 devm_platform_ioremap_resource(pdev, slot+1) 로 채운다.
	 *   인덱스가 slot+1 인 이유는 DT 의 reg 0번이 공용 컨트롤러 창이고 1번부터 포트별 창이기 때문이다.
	 * 읽는 자: pcie_port_read()/pcie_port_write() 만이 역참조한다.
	 * 값 범위: 유효한 __iomem 포인터. 실패 시 IS_ERR() 로 걸러져 여기까지 오지 않는다.
	 * 동기화: 포트마다 별개의 창이고 probe 경로에서만 접근하므로 락이 없다. */
	void __iomem *base;
	/* [한국어] pcie->ports 연결 리스트에 이 포트를 매다는 노드.
	 * 설정자: parse_port() 가 INIT_LIST_HEAD 후 list_add_tail 로 꼬리에 붙인다.
	 *   init_ports() 는 PHY 초기화가 실패한 포트를 list_del 로 떼어 낸다.
	 * 읽는 자: 이 파일의 모든 list_for_each_entry 순회.
	 * 값 범위: 항상 유효한 리스트 노드. 떼어 낸 뒤에도 구조체 자체는 devm 이 잡고 있다.
	 * 동기화: probe/remove 경로에서만 다뤄지며 동시 진입이 없어 락이 필요 없다. */
	struct list_head list;
	/* [한국어] 이 포트를 소유한 컨트롤러로 거슬러 올라가는 역포인터.
	 * 설정자: parse_port() 가 마지막에 대입한다.
	 * 읽는 자: init_port()/control_assert()/control_deassert()/enable_port() 가
	 *   공용 레지스터 창(pcie->base)과 dev, resets_inverted 에 닿기 위해 쓴다.
	 * 값 범위: 항상 유효(NULL 불가).
	 * 동기화: 설정 후 읽기 전용이라 필요 없다. */
	struct mt7621_pcie *pcie;
	/* [한국어] 이 포트의 클럭 게이트.
	 * 설정자: parse_port() 가 devm_get_clk_from_child() 로 얻는다.
	 * 읽는 자: enable_ports() 가 링크가 살아 있는 포트에 대해서만 clk_prepare_enable() 한다.
	 * 값 범위: 유효한 clk 포인터. 오류면 parse_port 에서 곧장 실패한다.
	 * 동기화: 클럭 프레임워크 내부에서 처리하므로 이 파일에는 락이 없다. */
	struct clk *clk;
	/* [한국어] 이 포트의 SerDes PHY.
	 * 설정자: parse_port() 가 devm_of_phy_get(dev, node, "pcie-phyN") 로 얻는다.
	 * 읽는 자: init_port() 가 phy_init/phy_power_on 하고, init_ports() 가 공유 PHY
	 *   절전 판정에서 phy_power_off 한다.
	 * 값 범위: 유효한 phy 포인터.
	 * 동기화: PHY 프레임워크가 자체 뮤텍스로 보호한다. */
	struct phy *phy;
	/* [한국어] 이 포트의 리셋 컨트롤. 주의: devm_ 판이 아니라 of_reset_control_get_exclusive() 로
	 * 얻으므로 자동 해제되지 않고, 실패·제거 경로에서 reset_control_put() 을 손으로 불러야 한다.
	 * 설정자: parse_port().
	 * 읽는 자: mt7621_control_assert()/deassert() 가 극성에 따라 assert/deassert 를 뒤집어 부른다.
	 * 값 범위: 유효 포인터 또는 -EPROBE_DEFER 가 아닌 다른 오류의 ERR_PTR(아래 관찰 참조).
	 * 동기화: 리셋 프레임워크가 처리한다. */
	struct reset_control *pcie_rst;
	/* [한국어] 이 포트의 PERST#(엔드포인트 리셋) GPIO. 없을 수도 있어 optional 판으로 얻는다.
	 * 설정자: parse_port() 가 devm_gpiod_get_index_optional(dev, "reset", slot, GPIOD_OUT_LOW).
	 * 읽는 자: mt7621_rst_gpio_pcie_assert()/deassert() 가 NULL 검사 후 토글한다.
	 * 값 범위: 유효 포인터 또는 NULL(해당 포트에 리셋 GPIO 가 없음).
	 * 동기화: GPIO 프레임워크가 처리한다. */
	struct gpio_desc *gpio_rst;
	/* [한국어] 이 포트의 슬롯 번호(0, 1, 2).
	 * 설정자: parse_dt() 가 of_pci_get_devfn() 결과에서 PCI_SLOT() 으로 뽑아 parse_port 에 넘긴다.
	 * 읽는 자: 레지스터 창 인덱스(slot+1), 인터럽트 비트 PCIE_PORT_INT_EN(slot),
	 *   PHY 이름 "pcie-phyN", 그리고 slot 0/1 의 PHY 공유 처리 분기에서 쓰인다.
	 * 값 범위: 0 이상 PCIE_PORT_CNT 미만.
	 * 동기화: 설정 후 읽기 전용. */
	u32 slot;
	/* [한국어] 이 포트를 실제로 쓸 수 있는지 여부.
	 * 설정자: init_ports() 가 세 곳에서 바꾼다 — slot 1 은 PHY 초기화를 건너뛰고 곧장 true,
	 *   init_port() 성공 시 true, 링크 다운으로 판정되면 false.
	 * 읽는 자: enable_ports() 가 true 인 포트에만 클럭을 켜고 레지스터를 설정한다.
	 *   init_ports() 의 PHY 공유 판정도 이 값을 본다.
	 * 값 범위: true/false. devm_kzalloc 덕분에 초기값은 false.
	 * 동기화: 단일 스레드 probe 경로에서만 다뤄진다. */
	bool enabled;
};

/**
 * struct mt7621_pcie - PCIe host information
 * @base: IO Mapped Register Base
 * @dev: Pointer to PCIe device
 * @ports: pointer to PCIe port information
 * @resets_inverted: depends on chip revision
 * reset lines are inverted.
 */
struct mt7621_pcie {
	/* [한국어] 컨트롤러의 플랫폼 디바이스.
	 * 설정자: probe() 가 &pdev->dev 로 채운다.
	 * 읽는 자: 모든 devm_ 할당의 수명 기준이자 dev_err/dev_info 의 대상.
	 * 값 범위: 항상 유효.
	 * 동기화: 읽기 전용. */
	struct device *dev;
	/* [한국어] 포트들이 공유하는 컨트롤러 레지스터 창의 가상 주소(DT reg 인덱스 0).
	 * 설정자: parse_dt() 가 devm_platform_ioremap_resource(pdev, 0) 로 채운다.
	 * 읽는 자: pcie_read()/pcie_write() 와 map_bus 콜백이 역참조한다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: config 접근은 PCI 코어의 pci_lock 아래에서 이루어지므로 이 파일에는 락이 없다. */
	void __iomem *base;
	/* [한국어] 이 컨트롤러에 속한 mt7621_pcie_port 들의 머리.
	 * 설정자: probe() 가 INIT_LIST_HEAD 로 초기화하고 parse_port() 가 꼬리에 붙인다.
	 * 읽는 자: 리셋/초기화/활성화/제거의 모든 순회.
	 * 값 범위: 비어 있을 수도 있다(DT 에 자식 노드가 없는 경우).
	 * 동기화: probe/remove 경로 전용이라 락이 없다. */
	struct list_head ports;
	/* [한국어] 이 칩에서 리셋 신호의 극성이 뒤집혀 있는지 여부.
	 * 설정자: probe() 가 soc_device_match(mt7621_pcie_quirks_match) 결과가 있으면 true 로 둔다
	 *   — 즉 SoC ID "mt7621" 의 리비전 "E2" 에서만 참이다.
	 * 읽는 자: mt7621_control_assert()/deassert() 가 이 값에 따라 호출할 API 를 맞바꾼다.
	 * 값 범위: true/false. 기본값 false(할당이 kzalloc 계열이라 0).
	 * 동기화: probe 초반에 한 번 정해진 뒤 읽기 전용. */
	bool resets_inverted;
};

/* [한국어]
 * pcie_read - 컨트롤러 공용 레지스터 창에서 32비트를 읽는다
 *
 * @pcie: 컨트롤러 객체. pcie->base 가 ioremap 된 공용 창의 시작이다.
 * @reg: 그 창 안에서의 바이트 오프셋(RALINK_PCI_* 상수).
 * @return: 읽은 32비트 값.
 *
 * readl_relaxed() 를 쓰는 이유: 이 드라이버의 레지스터 접근은 전부 probe 경로의
 * 순차 실행이고 DMA 버퍼와의 순서를 맞출 필요가 없다. 배리어를 생략해 SoC 마다
 * 수십 사이클씩 드는 동기화 비용을 없앤다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트)에서만 불린다. 락은 잡지 않는다.
 *
 * 호출 체인:
 *   mt7621_pcie_enable_port() / read_config() → [pcie_read] → readl_relaxed()
 */
static inline u32 pcie_read(struct mt7621_pcie *pcie, u32 reg)
{
	/* [한국어] 컨트롤러 공용 창에서 32비트 레지스터를 읽는다. _relaxed 판이라 앞뒤에
	 * 메모리 배리어를 넣지 않는다 — 이 드라이버의 레지스터 접근은 모두 probe 경로의
	 * 순차 실행이라 DMA 와의 순서 보장이 필요 없기 때문이다. */
	return readl_relaxed(pcie->base + reg);
}

/* [한국어]
 * pcie_write - 컨트롤러 공용 레지스터 창에 32비트를 쓴다
 *
 * @pcie: 컨트롤러 객체.
 * @val: 쓸 값.
 * @reg: 창 안에서의 바이트 오프셋.
 *
 * 인자 순서가 (val, reg) 로 표준 writel(val, addr) 과 같은 배치이지만,
 * 같은 파일의 pcie_read(pcie, reg) 와 나란히 놓고 보면 두 번째 인자의 의미가
 * 달라 헷갈리기 쉽다. 이 파일 안에서만 쓰이는 관용이다.
 *
 * 실행 컨텍스트: probe 경로 전용. 배리어 없는 writel_relaxed 를 쓴다.
 *
 * 호출 체인:
 *   write_config() / mt7621_pcie_enable_port() / mt7621_pcie_enable_ports()
 *     → [pcie_write] → writel_relaxed()
 */
static inline void pcie_write(struct mt7621_pcie *pcie, u32 val, u32 reg)
{
	/* [한국어] 컨트롤러 공용 창에 32비트를 쓴다. 인자 순서가 (val, reg) 로 writel 과 반대인
	 * 점에 주의 — 이 파일 안에서만 쓰이는 관용이다. */
	writel_relaxed(val, pcie->base + reg);
}

/* [한국어]
 * pcie_port_read - 포트 전용 레지스터 창에서 32비트를 읽는다
 *
 * @port: 포트 객체. port->base 가 DT reg 인덱스 slot+1 로 매핑된 창이다.
 * @reg: 그 창 안에서의 바이트 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 공용 창(pcie->base)과 포트 창(port->base)은 서로 다른 물리 영역이다.
 * 같은 오프셋 상수라도 어느 창에 쓰느냐에 따라 전혀 다른 레지스터를 가리키므로,
 * 이 파일은 두 벌의 접근자를 따로 두어 실수를 구조적으로 막는다.
 *
 * 실행 컨텍스트: probe 경로 전용.
 *
 * 호출 체인:
 *   mt7621_pcie_port_is_linkup() → [pcie_port_read] → readl_relaxed()
 */
static inline u32 pcie_port_read(struct mt7621_pcie_port *port, u32 reg)
{
	/* [한국어] 포트 전용 창에서 32비트를 읽는다. 위 pcie_read 와 같은 형태지만 기준 주소가
	 * pcie->base 가 아니라 port->base 다. */
	return readl_relaxed(port->base + reg);
}

/* [한국어]
 * pcie_port_write - 포트 전용 레지스터 창에 32비트를 쓴다
 *
 * @port: 포트 객체.
 * @val: 쓸 값.
 * @reg: 창 안에서의 바이트 오프셋.
 *
 * 실행 컨텍스트: probe 경로 전용.
 *
 * 호출 체인:
 *   mt7621_pcie_enable_port() → [pcie_port_write] → writel_relaxed()
 *   (인바운드 BAR 설정과 class code 쓰기 두 곳에서만 쓰인다)
 */
static inline void pcie_port_write(struct mt7621_pcie_port *port,
				   u32 val, u32 reg)
{
	/* [한국어] 포트 전용 창에 32비트를 쓴다. */
	writel_relaxed(val, port->base + reg);
}

/* [한국어]
 * mt7621_pcie_map_bus - config space 접근 주소를 계산해 코어에 돌려준다
 *
 * @bus: 접근 대상 PCI 버스. bus->sysdata 에 컨트롤러가 심어져 있다.
 * @devfn: 디바이스·함수 번호를 합친 8비트 값.
 * @where: config space 안의 바이트 오프셋(0~4095).
 * @return: 실제로 읽거나 쓸 MMIO 주소. NULL 을 돌려주지 않으므로 접근이
 *       거부되는 경우가 없다 — 존재하지 않는 장치를 읽으면 하드웨어가
 *       all-ones 를 돌려주고, 그것으로 PCI 코어가 부재를 판정한다.
 *
 * 왜 필요한가: 이 하드웨어의 config 접근은 x86 의 0xCF8/0xCFC 와 같은
 * "주소 레지스터에 쓰고 데이터 레지스터를 읽는" 2단계 방식이다. 그 구조에서는
 * 주소 계산만 컨트롤러마다 다르고 폭 맞추기·바이트 추출은 모두 공통이므로,
 * PCI 코어가 map_bus 콜백 하나만 요구하고 나머지는 pci_generic_config_read/write 가
 * 처리한다. 이 파일이 read/write 를 직접 구현하지 않는 이유가 그것이다.
 *
 * 동작 과정:
 *   1) bus->sysdata 에서 컨트롤러를 꺼낸다.
 *   2) PCI_CONF1_EXT_ADDRESS() 로 CF8 형식 주소를 합성한다. 표준 CF8 주소
 *      (bit31 enable | bus<<16 | dev<<11 | func<<8 | reg&0xfc)에 더해,
 *      reg 의 [11:8] 을 [27:24] 로 옮긴 확장 비트를 OR 한다. 이 확장이 있어야
 *      256바이트를 넘는 PCIe 확장 config 영역까지 CF8 방식으로 닿는다.
 *      표준이 아니라 ECAM 미지원 ARM SoC 들이 널리 쓰는 관용이다.
 *   3) 주소 레지스터에 그 값을 쓴다.
 *   4) 데이터 레지스터 주소에 (where & 3) 을 더해 돌려준다. 데이터 창이 4바이트
 *      워드 단위인데 호출자는 바이트/워드 접근을 요구할 수 있으므로, 워드 안에서의
 *      바이트 위치를 주소에 반영해 주는 것이다.
 *
 * 실행 컨텍스트: PCI 코어의 config 접근 경로. 전역 pci_lock 을 잡은 상태로
 * 불리므로 주소 레지스터 설정과 데이터 접근 사이에 다른 CPU 가 끼어들 수 없다 —
 * 이 2단계 방식이 안전한 것은 전적으로 그 락 덕분이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_read_config_dword() 등 → bus->ops->read == pci_generic_config_read()
 *     → [mt7621_pcie_map_bus] → 코어가 돌려받은 주소에서 readl/readw/readb
 */
static void __iomem *mt7621_pcie_map_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	/* [한국어] PCI 코어가 bus->sysdata 에 심어 둔 컨트롤러 객체를 꺼낸다.
	 * 그 값은 register_host() 가 host->sysdata = pcie 로 설정한 것이다. */
	struct mt7621_pcie *pcie = bus->sysdata;
	/* [한국어] 버스/디바이스/함수/오프셋을 CF8 형식 주소 한 워드로 합성한다.
	 * PCI_CONF1_EXT_ADDRESS 는 표준 CF8 주소(bit31 enable | bus<<16 | dev<<11 |
	 * func<<8 | reg&0xfc)에 확장 비트(reg[11:8] 을 [27:24] 로)를 더해,
	 * PCIe 확장 config 영역(256~4095바이트)까지 접근할 수 있게 한다. */
	u32 address = PCI_CONF1_EXT_ADDRESS(bus->number, PCI_SLOT(devfn),
					    PCI_FUNC(devfn), where);

	/* [한국어] 주소 레지스터에 방금 만든 주소를 써서 다음 데이터 접근의 대상을 지정한다.
	 * 여기서 _relaxed 를 쓰지만, 뒤이은 데이터 접근이 같은 디바이스의 같은 창에 대한
	 * MMIO 라 순서가 보장되므로 문제가 되지 않는다. */
	writel_relaxed(address, pcie->base + RALINK_PCI_CONFIG_ADDR);

	/* [한국어] 데이터 창의 주소를 돌려준다. (where & 3) 을 더하는 이유는 데이터 레지스터가
	 * 4바이트 워드 단위인데 호출자가 바이트/워드 단위 접근을 요구할 수 있기 때문이다.
	 * 예를 들어 오프셋 0x0d 를 1바이트 읽으면 워드 주소 0x0c 를 지정한 뒤
	 * 데이터 창의 +1 바이트를 읽어야 한다. 이 주소를 돌려주면 나머지 처리는
	 * pci_generic_config_read() 가 대신해 준다 — map_bus 만 구현하면 되는 이유다. */
	return pcie->base + RALINK_PCI_CONFIG_DATA + (where & 3);
}

static struct pci_ops mt7621_pcie_ops = {
	/* [한국어] 주소 계산만 이 파일이 맡고, 실제 읽기/쓰기는 코어의 범용 구현에 위임한다. */
	.map_bus	= mt7621_pcie_map_bus,
	/* [한국어] map_bus 가 돌려준 주소에서 폭에 맞춰 읽는 코어 공용 함수. */
	.read		= pci_generic_config_read,
	/* [한국어] 같은 방식의 쓰기. read/write 를 직접 구현하지 않아도 되는 것은 이 하드웨어가
	 * 주소/데이터 쌍이라는 단순한 구조이기 때문이다. */
	.write		= pci_generic_config_write,
};

/* [한국어]
 * read_config - 루트 컴플렉스 자신의 config 레지스터를 32비트 읽는다
 *
 * @pcie: 컨트롤러 객체.
 * @dev: 디바이스(슬롯) 번호. 버스 0 의 그 슬롯에 해당하는 가상 브리지를 가리킨다.
 * @reg: config space 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 왜 map_bus 경로를 쓰지 않는가: 이 헬퍼는 PCI 버스가 아직 스캔되기 전,
 * 즉 struct pci_bus 도 struct pci_dev 도 없는 초기화 단계에서 쓰인다.
 * 그래서 버스 번호 0 과 함수 번호 0 을 상수로 박고 주소·데이터 레지스터를
 * 직접 두드린다. 실제 용도는 단 하나, PCIE_FTS_NUM(0x70c) 을 읽고 쓰는 것이다.
 *
 * 동작 과정: 주소 합성 → 주소 레지스터에 쓰기 → 데이터 레지스터 읽기.
 *
 * 실행 컨텍스트: probe 경로. 여기서는 pci_lock 을 잡지 않지만, 아직 PCI 코어가
 * 이 컨트롤러를 모르는 시점이라 경쟁 상대가 없다.
 *
 * 에러 경로: 없다. 응답이 없으면 all-ones 가 그대로 돌아온다.
 *
 * 호출 체인:
 *   mt7621_pcie_enable_port() → [read_config] → pcie_write/pcie_read
 */
static u32 read_config(struct mt7621_pcie *pcie, unsigned int dev, u32 reg)
{
	/* [한국어] 버스 번호 0, 함수 0 으로 고정한 주소를 만든다. 이 헬퍼는 루트 컴플렉스
	 * 자신의 포트(가상 브리지)에만 접근하므로 버스와 함수를 볼 필요가 없다. */
	u32 address = PCI_CONF1_EXT_ADDRESS(0, dev, 0, reg);

	/* [한국어] 주소 레지스터 설정. */
	pcie_write(pcie, address, RALINK_PCI_CONFIG_ADDR);
	/* [한국어] 데이터 레지스터에서 32비트를 읽어 그대로 돌려준다. */
	return pcie_read(pcie, RALINK_PCI_CONFIG_DATA);
}

/* [한국어]
 * write_config - 루트 컴플렉스 자신의 config 레지스터에 32비트를 쓴다
 *
 * @pcie: 컨트롤러 객체.
 * @dev: 디바이스(슬롯) 번호.
 * @reg: config space 오프셋.
 * @val: 쓸 값.
 *
 * read_config() 와 대칭이며 같은 이유로 존재한다. 이 파일에서 유일한 호출은
 * FTS 개수를 갱신하는 곳이다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   mt7621_pcie_enable_port() → [write_config] → pcie_write ×2
 */
static void write_config(struct mt7621_pcie *pcie, unsigned int dev,
			 u32 reg, u32 val)
{
	/* [한국어] 쓰기용 주소 합성 — 읽기와 완전히 같다. */
	u32 address = PCI_CONF1_EXT_ADDRESS(0, dev, 0, reg);

	/* [한국어] 주소 레지스터 설정. */
	pcie_write(pcie, address, RALINK_PCI_CONFIG_ADDR);
	/* [한국어] 데이터 레지스터에 값을 쓴다. */
	pcie_write(pcie, val, RALINK_PCI_CONFIG_DATA);
}

/* [한국어]
 * mt7621_rst_gpio_pcie_assert - 엔드포인트 리셋(PERST#)을 건다
 *
 * @port: 대상 포트.
 *
 * PERST# 는 PCIe 규격이 정의한 엔드포인트 리셋 신호다. MT7621 에서는 이 신호가
 * SoC 내부 레지스터가 아니라 범용 GPIO 핀에 연결되어 있어, 리셋 컨트롤러가 아닌
 * GPIO API 로 토글해야 한다. 그래서 이 파일에는 리셋 제어가 두 벌 존재한다 —
 * RC 쪽은 reset_control_*, EP 쪽은 이 GPIO 함수들이다.
 *
 * gpiod_set_value(gpio, 1) 의 1 은 전기적 레벨이 아니라 논리값이다. DT 에
 * active-low 로 서술되어 있으면 GPIO 프레임워크가 알아서 반전하므로,
 * 드라이버는 언제나 "1 = 리셋 걸기"로만 생각하면 된다.
 *
 * GPIO 는 optional 로 얻으므로 NULL 일 수 있고, 그래서 검사가 먼저 온다.
 *
 * 실행 컨텍스트: probe 경로. gpiod_set_value() 는 잠들 수 없는 판이라
 * GPIO 컨트롤러가 I2C 같은 느린 버스 뒤에 있으면 쓸 수 없다 —
 * 이 SoC 는 메모리 맵 GPIO 라 문제가 없다.
 *
 * 호출 체인:
 *   mt7621_pcie_reset_assert() → [mt7621_rst_gpio_pcie_assert] → gpiod_set_value()
 */
static inline void mt7621_rst_gpio_pcie_assert(struct mt7621_pcie_port *port)
{
	/* [한국어] 리셋 GPIO 가 DT 에 없을 수도 있으므로(optional 로 얻었다) NULL 검사가 필수다. */
	if (port->gpio_rst)
		/* [한국어] 1 = PERST# 어서트. gpiod_ 계열은 DT 의 active-low 서술을 이미 반영하므로,
		 * 여기서 1 은 전기적 레벨이 아니라 논리적 "리셋 걸기"를 뜻한다. */
		gpiod_set_value(port->gpio_rst, 1);
}

/* [한국어]
 * mt7621_rst_gpio_pcie_deassert - 엔드포인트 리셋(PERST#)을 푼다
 *
 * @port: 대상 포트.
 *
 * 이 호출이 링크 학습의 출발 신호다. 여기서 리셋이 풀린 뒤 엔드포인트가
 * 전원·클럭을 안정시키고 링크 훈련을 시작하므로, 호출자는 곧바로 링크 상태를
 * 확인하지 않고 PERST_DELAY_MS 만큼 기다린다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   mt7621_pcie_reset_ep_deassert() → [mt7621_rst_gpio_pcie_deassert] → gpiod_set_value()
 */
static inline void mt7621_rst_gpio_pcie_deassert(struct mt7621_pcie_port *port)
{
	/* [한국어] GPIO 부재 검사 — assert 쪽과 같은 이유다. */
	if (port->gpio_rst)
		/* [한국어] 0 = PERST# 해제. 엔드포인트가 링크 학습을 시작할 수 있게 된다. */
		gpiod_set_value(port->gpio_rst, 0);
}

/* [한국어]
 * mt7621_pcie_port_is_linkup - 포트의 링크가 학습을 마쳤는지 확인한다
 *
 * @port: 대상 포트.
 * @return: true = 링크 업, false = 링크 다운(카드 없음 또는 훈련 실패).
 *
 * 포트 전용 창의 RALINK_PCI_STATUS 에서 bit 0(PCIE_PORT_LINKUP)만 본다.
 * != 0 비교로 bool 로 정규화하는 것은, 비트가 0 번이 아닌 경우에도 안전하게
 * true/false 로 축약하기 위한 관용이다.
 *
 * 이 판정에는 폴링도 타임아웃도 없다. 단 한 번 읽고 끝낸다. 링크 학습에
 * 필요한 시간은 호출 전에 msleep(PERST_DELAY_MS) 로 이미 확보했다는 전제다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   mt7621_pcie_init_ports() → [mt7621_pcie_port_is_linkup] → pcie_port_read()
 */
static inline bool mt7621_pcie_port_is_linkup(struct mt7621_pcie_port *port)
{
	/* [한국어] 포트 상태 레지스터의 링크 업 비트를 확인한다. != 0 비교로 bool 로 정규화한다. */
	return (pcie_port_read(port, RALINK_PCI_STATUS) & PCIE_PORT_LINKUP) != 0;
}

/* [한국어]
 * mt7621_control_assert - 루트 컴플렉스 쪽 리셋을 논리적으로 건다
 *
 * @port: 대상 포트. port->pcie->resets_inverted 로 극성을 판별한다.
 *
 * 왜 필요한가: MT7621 의 E2 리비전에서 PCIe 리셋 신호의 극성이 뒤집혀 있다.
 * 그 사실을 호출부마다 신경 쓰게 하면 실수가 나므로, 이 함수가 "논리적 assert"
 * 라는 하나의 의미를 제공하고 극성 처리를 안으로 숨긴다.
 *
 * 동작: resets_inverted 가 참이면 reset_control_assert() 를, 거짓이면
 * reset_control_deassert() 를 부른다. 이름과 호출이 반대로 보이는 쪽(정상 칩)이
 * 오히려 기본 경로라는 점이 이 코드에서 가장 헷갈리는 부분이다. 그렇게 되는 이유는
 * 이 SoC 의 리셋 라인이 하드웨어 수준에서 이미 한 번 반전되어 있고,
 * E2 리비전에서 그 반전이 정정되었기 때문으로 보인다 — 다만 그 근거가 되는
 * 하드웨어 문서는 이 트리에 없으므로 코드가 그렇게 되어 있다는 사실만 기록한다.
 *
 * 실행 컨텍스트: probe 경로. 리셋 프레임워크가 자체 락으로 보호한다.
 *
 * 호출 체인:
 *   mt7621_pcie_reset_assert() / mt7621_pcie_init_ports()
 *     → [mt7621_control_assert] → reset_control_assert() 또는 reset_control_deassert()
 */
static inline void mt7621_control_assert(struct mt7621_pcie_port *port)
{
	/* [한국어] 극성 판정에 필요한 컨트롤러 객체를 포트에서 되찾는다. */
	struct mt7621_pcie *pcie = port->pcie;

	/* [한국어] E2 리비전에서는 리셋 신호가 반전되어 있어 assert 요청에 실제로는 assert 를 부른다. */
	if (pcie->resets_inverted)
		/* [한국어] 반전된 칩: 논리적 assert = reset_control_assert. */
		reset_control_assert(port->pcie_rst);
	else
		/* [한국어] 정상 칩: 논리적 assert 를 위해 오히려 deassert 를 부른다. 이 뒤집힘이
		 * 이 파일에서 가장 헷갈리는 부분이며, resets_inverted 라는 이름과 실제 분기가
		 * 반대로 보이는 이유는 하드웨어 레벨의 극성이 이미 한 번 뒤집혀 있기 때문이다. */
		reset_control_deassert(port->pcie_rst);
}

/* [한국어]
 * mt7621_control_deassert - 루트 컴플렉스 쪽 리셋을 논리적으로 푼다
 *
 * @port: 대상 포트.
 *
 * mt7621_control_assert() 와 완전히 대칭이며, 같은 극성 규칙을 반대로 적용한다.
 * 이 호출 이후 RC 가 동작을 시작하므로, 엔드포인트 리셋(PERST#)을 풀기 전에
 * 반드시 이것이 먼저 와야 한다 — RC 가 준비되기 전에 EP 가 링크를 요청하면
 * 훈련이 실패한다. mt7621_pcie_init_ports() 의 호출 순서가 그 규칙을 지킨다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   mt7621_pcie_reset_rc_deassert() → [mt7621_control_deassert]
 *     → reset_control_deassert() 또는 reset_control_assert()
 */
static inline void mt7621_control_deassert(struct mt7621_pcie_port *port)
{
	/* [한국어] assert 쪽과 같은 방식으로 컨트롤러를 되찾는다. */
	struct mt7621_pcie *pcie = port->pcie;

	/* [한국어] 반전 칩 여부에 따라 호출을 맞바꾼다. */
	if (pcie->resets_inverted)
		/* [한국어] 반전 칩: 논리적 deassert = reset_control_deassert. */
		reset_control_deassert(port->pcie_rst);
	else
		/* [한국어] 정상 칩: 논리적 deassert 를 위해 assert 를 부른다. */
		reset_control_assert(port->pcie_rst);
}

/* [한국어]
 * mt7621_pcie_parse_port - DT 자식 노드 하나를 포트 객체로 만든다
 *
 * @pcie: 컨트롤러 객체. 완성된 포트를 pcie->ports 꼬리에 매단다.
 * @node: 이 포트를 서술하는 DT 자식 노드.
 * @slot: 이 포트의 슬롯 번호(0~2). 호출자가 of_pci_get_devfn() 결과에서 뽑아 넘긴다.
 * @return: 0 = 성공. -ENOMEM = 포트 구조체 할당 실패.
 *       그 밖의 음수 = 레지스터 매핑·클럭·리셋·PHY·GPIO 획득 실패,
 *       또는 아직 준비되지 않은 의존성에 대한 -EPROBE_DEFER.
 *
 * 왜 필요한가: 한 포트를 쓰려면 다섯 가지 자원이 모두 있어야 한다 —
 * 전용 레지스터 창, 클럭 게이트, 리셋 컨트롤, SerDes PHY, 그리고 PERST# GPIO.
 * 이 함수가 그 다섯을 DT 서술에서 모아 하나의 struct mt7621_pcie_port 에 담는다.
 *
 * 동작 과정:
 *   1) devm_kzalloc 으로 포트 구조체를 0 초기화 할당한다(enabled 가 false 로 시작).
 *   2) devm_platform_ioremap_resource(pdev, slot + 1) — 인덱스가 slot+1 인 것은
 *      DT reg 0번이 공용 창이고 1번부터 포트별 창이기 때문이다.
 *   3) devm_get_clk_from_child() 로 클럭을 얻는다.
 *   4) of_reset_control_get_exclusive() 로 리셋을 얻는다. 여기만 devm_ 판이 아니라
 *      이 함수 이후로 해제 책임이 생긴다 — remove_reset 라벨과 probe/remove 의
 *      reset_control_put() 이 그래서 존재한다.
 *   5) "pcie-phyN" 이름으로 PHY 를, "reset" 이름의 slot 번째 GPIO 를 얻는다.
 *      GPIO 는 optional 판이라 없으면 NULL 이 정상이다.
 *   6) slot 과 역포인터를 채우고 리스트 꼬리에 붙인다.
 *
 * [상류 코드 관찰, 수정하지 않음] 4)의 오류 검사가 -EPROBE_DEFER 만 본다.
 * 다른 오류가 나면 port->pcie_rst 에 ERR_PTR 이 담긴 채 초기화가 계속되고,
 * 나중에 mt7621_control_assert() 가 그 값을 리셋 API 에 넘기게 된다.
 * 리셋 프레임워크 구현이 이 트리에 없어 그때의 동작은 확인할 수 없다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. DT 노드마다 한 번씩 불린다.
 *
 * 에러 경로: 두 갈래다. 리셋을 얻기 전의 실패는 devm 자원만 잡았으므로 곧장
 * return 하고, 그 이후의 실패는 remove_reset 라벨을 거쳐 reset_control_put() 을
 * 부른 뒤 반환한다. 이 구분이 이 함수 오류 처리의 전부다.
 *
 * 호출 체인:
 *   mt7621_pcie_parse_dt() → [mt7621_pcie_parse_port]
 *     → devm_platform_ioremap_resource / devm_get_clk_from_child
 *     → of_reset_control_get_exclusive / devm_of_phy_get
 *     → devm_gpiod_get_index_optional / list_add_tail
 */
static int mt7621_pcie_parse_port(struct mt7621_pcie *pcie,
				  struct device_node *node,
				  int slot)
{
	/* [한국어] 이번 자식 노드에 대응할 포트 객체. */
	struct mt7621_pcie_port *port;
	/* [한국어] 로그와 devm 할당의 기준이 될 컨트롤러 디바이스. */
	struct device *dev = pcie->dev;
	/* [한국어] devm_platform_ioremap_resource() 가 플랫폼 디바이스를 요구하므로 되돌린다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] "pcie-phyN" 문자열을 담을 버퍼. 11바이트면 "pcie-phy"(8) + 숫자 + NUL 을 담고도 남는다. */
	char name[11];
	/* [한국어] 오류 코드를 잠시 담아 두는 변수. goto 로 정리 구간에 넘길 때 필요하다. */
	int err;

	/* [한국어] 포트 구조체를 0 으로 초기화해 할당한다. devm 이므로 실패·제거 시 자동 해제된다.
	 * enabled 필드가 false 로 시작하는 것도 이 kzalloc 덕분이다. */
	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!port)
		/* [한국어] 곧장 반환해도 되는 이유는 이 시점까지 devm 이 아닌 자원을 잡은 것이 없기 때문이다. */
		return -ENOMEM;

	/* [한국어] DT reg 인덱스 slot+1 의 자원을 매핑한다. 인덱스 0 은 공용 컨트롤러 창이므로
	 * 포트별 창은 1 부터 시작한다. */
	port->base = devm_platform_ioremap_resource(pdev, slot + 1);
	/* [한국어] devm_platform_ioremap_resource() 는 실패 시 NULL 이 아니라 ERR_PTR 을 돌려주므로
	 * IS_ERR() 로 검사해야 한다. */
	if (IS_ERR(port->base))
		/* [한국어] 오류 포인터에서 errno 를 꺼내 그대로 전달한다. */
		return PTR_ERR(port->base);

	/* [한국어] 자식 노드에 서술된 클럭을 가져온다. 이름 없이(NULL) 첫 번째 클럭을 쓴다. */
	port->clk = devm_get_clk_from_child(dev, node, NULL);
	/* [한국어] 클럭이 없으면 포트를 켤 수 없다. */
	if (IS_ERR(port->clk)) {
		/* [한국어] 어느 포트에서 실패했는지 슬롯 번호와 함께 남긴다. */
		dev_err(dev, "failed to get pcie%d clock\n", slot);
		/* [한국어] 오류 코드 전달. */
		return PTR_ERR(port->clk);
	}

	/* [한국어] 포트별 리셋 컨트롤을 배타적으로 가져온다. devm_ 판이 아니라는 점이 중요하다 —
	 * 성공하면 이 함수는 해제 책임을 지게 되고, 그래서 아래 remove_reset 라벨과
	 * probe/remove 의 reset_control_put() 호출이 존재한다. */
	port->pcie_rst = of_reset_control_get_exclusive(node, NULL);
	/* [한국어] [상류 코드 관찰, 수정하지 않음] -EPROBE_DEFER 만 검사한다. 다른 오류가 나면
	 * port->pcie_rst 에 ERR_PTR 이 담긴 채로 초기화가 계속 진행되고, 나중에
	 * mt7621_control_assert() 가 그 값을 리셋 API 에 넘기게 된다. 리셋 프레임워크
	 * 구현이 이 트리에 없어 그때 어떤 일이 벌어지는지는 여기서 확인할 수 없다. */
	if (PTR_ERR(port->pcie_rst) == -EPROBE_DEFER) {
		/* [한국어] 지연 재시도 사유를 로그로 남긴다. */
		dev_err(dev, "failed to get pcie%d reset control\n", slot);
		/* [한국어] -EPROBE_DEFER 를 그대로 올려 보내면 드라이버 코어가 나중에 probe 를 다시 시도한다. */
		return PTR_ERR(port->pcie_rst);
	}

	/* [한국어] 슬롯 번호를 넣어 "pcie-phy0" 같은 PHY 이름을 만든다. DT 의 phy-names 속성과
	 * 이 문자열이 일치해야 매칭된다. */
	snprintf(name, sizeof(name), "pcie-phy%d", slot);
	/* [한국어] 자식 노드에서 그 이름의 PHY 를 가져온다. devm 이므로 자동 해제된다. */
	port->phy = devm_of_phy_get(dev, node, name);
	/* [한국어] PHY 가 없거나 아직 준비되지 않았다. */
	if (IS_ERR(port->phy)) {
		/* [한국어] 실패한 PHY 이름을 로그에 남긴다. */
		dev_err(dev, "failed to get pcie-phy%d\n", slot);
		/* [한국어] 오류 코드를 err 에 담는다 — 아래 goto 가 반환값으로 쓴다. */
		err = PTR_ERR(port->phy);
		/* [한국어] 위에서 잡은 리셋 컨트롤이 devm 이 아니므로, 여기부터는 곧장 return 하면 안 되고
		 * 반드시 정리 구간을 거쳐야 한다. */
		goto remove_reset;
	}

	/* [한국어] PERST# 용 GPIO 를 가져온다. optional 판이라 DT 에 없으면 오류가 아니라 NULL 을 돌려준다.
	 * GPIOD_OUT_LOW 는 출력 방향으로 설정하면서 초기값을 논리 0(리셋 해제)으로 두라는 뜻이다. */
	port->gpio_rst = devm_gpiod_get_index_optional(dev, "reset", slot,
						       GPIOD_OUT_LOW);
	/* [한국어] NULL 은 정상이지만 ERR_PTR 은 실제 오류다. */
	if (IS_ERR(port->gpio_rst)) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to get GPIO for PCIe%d\n", slot);
		/* [한국어] 오류 코드 보관. */
		err = PTR_ERR(port->gpio_rst);
		/* [한국어] 리셋 정리 구간으로. */
		goto remove_reset;
	}

	/* [한국어] 슬롯 번호를 포트에 기록한다. 이후 레지스터 비트 위치와 로그에 쓰인다. */
	port->slot = slot;
	/* [한국어] 포트에서 컨트롤러로 거슬러 올라갈 역포인터를 심는다. */
	port->pcie = pcie;

	/* [한국어] 리스트 노드를 자기 자신을 가리키도록 초기화한다.
	 * 곧바로 list_add_tail 하므로 필수는 아니지만, 나중에 list_del 된 뒤에도
	 * 노드가 유효한 상태를 유지하게 하는 방어적 관용이다. */
	INIT_LIST_HEAD(&port->list);
	/* [한국어] 컨트롤러의 포트 리스트 꼬리에 붙인다. DT 자식 노드 순서가 그대로 리스트 순서가 된다. */
	list_add_tail(&port->list, &pcie->ports);

	/* [한국어] 여기까지 왔으면 이 포트의 모든 자원이 준비되었다. */
	return 0;

/* [한국어] PHY 나 GPIO 획득이 실패했을 때만 도달하는 정리 라벨. */
remove_reset:
	/* [한국어] devm 이 아닌 유일한 자원인 리셋 컨트롤을 반납한다. */
	reset_control_put(port->pcie_rst);
	/* [한국어] 위에서 담아 둔 오류를 호출자에게 전달한다. */
	return err;
}

/* [한국어]
 * mt7621_pcie_parse_dt - 컨트롤러 창을 매핑하고 모든 포트를 DT 에서 읽어 들인다
 *
 * @pcie: 컨트롤러 객체. base 와 ports 리스트를 채운다.
 * @return: 0 = 성공. 음수 = 공용 창 매핑 실패, devfn 파싱 실패,
 *       또는 어느 한 포트의 초기화 실패.
 *
 * 왜 필요한가: 이 컨트롤러는 PCI 장치가 아니라 SoC 내부 플랫폼 장치라
 * 자기 서술이 config space 가 아닌 디바이스 트리에 있다. 레지스터 창이
 * 몇 개이고 포트가 몇 개인지, 각 포트가 어떤 클럭·PHY·GPIO 를 쓰는지가
 * 모두 DT 에 적혀 있고, 이 함수가 그것을 읽어 메모리 상의 객체로 옮긴다.
 *
 * 동작 과정:
 *   1) DT reg 인덱스 0 = 포트들이 공유하는 컨트롤러 레지스터 창을 매핑한다.
 *   2) status 가 "disabled" 가 아닌 자식 노드만 순회한다.
 *      _scoped 판 매크로라 루프를 어떤 경로로 빠져나가도 of_node_put() 이
 *      자동으로 불려 DT 노드 참조가 새지 않는다.
 *   3) 각 자식의 reg 첫 셀에서 devfn 을 얻고 PCI_SLOT() 으로 슬롯 번호를 뽑아
 *      mt7621_pcie_parse_port() 에 넘긴다.
 *   4) 한 포트라도 실패하면 즉시 중단한다 — 부분 성공을 허용하지 않는다.
 *
 * [상류 코드 관찰, 수정하지 않음] 4)에서 중단할 때 이미 리스트에 붙은 포트들의
 * pcie_rst 는 반납되지 않는다. 호출자 probe() 도 이 실패 경로에서는
 * remove_resets 라벨을 거치지 않고 곧장 반환하므로, 그 리셋 컨트롤들은 누수된다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 세 지점 모두 곧장 return 한다. devfn 파싱 실패만 dev_err_probe() 를
 * 써서 -EPROBE_DEFER 일 때 로그를 억제한다.
 *
 * 호출 체인:
 *   mt7621_pcie_probe() → [mt7621_pcie_parse_dt]
 *     → devm_platform_ioremap_resource() / of_pci_get_devfn()
 *     → mt7621_pcie_parse_port()
 */
static int mt7621_pcie_parse_dt(struct mt7621_pcie *pcie)
{
	/* [한국어] 로그와 자원 매핑에 쓸 디바이스. */
	struct device *dev = pcie->dev;
	/* [한국어] devm_platform_ioremap_resource() 가 요구하는 플랫폼 디바이스. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 컨트롤러 노드 자신 — 그 자식들이 각각 하나의 포트다. */
	struct device_node *node = dev->of_node;
	/* [한국어] 자식 순회 중 발생한 오류를 담는다. */
	int err;

	/* [한국어] DT reg 인덱스 0 = 포트들이 공유하는 컨트롤러 레지스터 창. */
	pcie->base = devm_platform_ioremap_resource(pdev, 0);
	/* [한국어] 매핑 실패 검사(ERR_PTR 반환 API). */
	if (IS_ERR(pcie->base))
		/* [한국어] 오류 전달. */
		return PTR_ERR(pcie->base);

	/* [한국어] status 가 "disabled" 가 아닌 자식 노드만 순회한다. _scoped 판이라 루프를 어떤
	 * 경로로 빠져나가도 of_node_put() 이 자동으로 불려, DT 노드 참조 누수를 막는다. */
	for_each_available_child_of_node_scoped(node, child) {
		/* [한국어] 이번 자식이 담당할 슬롯 번호. */
		int slot;

		/* [한국어] 자식 노드의 reg 속성 첫 셀에서 devfn 을 뽑는다. 음수면 파싱 실패다. */
		err = of_pci_get_devfn(child);
		/* [한국어] DT 가 잘못되었으면 더 진행할 수 없다. */
		if (err < 0)
			/* [한국어] dev_err_probe() 는 -EPROBE_DEFER 일 때는 조용히, 그 밖에는 오류로 로그를 남기고
			 * 인자로 받은 errno 를 그대로 돌려주는 편의 함수다. */
			return dev_err_probe(dev, err, "failed to parse devfn\n");

		/* [한국어] devfn 의 상위 5비트에서 디바이스(슬롯) 번호를 얻는다. */
		slot = PCI_SLOT(err);

		/* [한국어] 이 자식 노드 하나를 포트로 만든다. */
		err = mt7621_pcie_parse_port(pcie, child, slot);
		/* [한국어] 한 포트라도 실패하면 전체 probe 를 중단한다 — 부분 성공을 허용하지 않는다. */
		if (err)
			/* [한국어] 오류 전달. 이미 리스트에 붙은 포트들의 리셋 컨트롤은 여기서 반납되지 않고,
			 * probe() 의 remove_resets 라벨이 처리할 것으로 기대된다. */
			return err;
	}

	/* [한국어] 모든 자식 노드를 성공적으로 처리했다. */
	return 0;
}

/* [한국어]
 * mt7621_pcie_init_port - 포트의 SerDes PHY 를 초기화하고 전원을 넣는다
 *
 * @port: 대상 포트.
 * @return: 0 = 성공(port->enabled 가 true 로 설정됨).
 *       음수 = phy_init() 또는 phy_power_on() 실패.
 *
 * 왜 필요한가: PCIe 링크는 SerDes 물리 계층이 살아 있어야 훈련을 시작할 수 있다.
 * MT7621 의 SerDes 는 별도의 PHY 드라이버가 관리하므로, 이 파일은 PHY 프레임워크의
 * 두 단계 규약 — 초기화(phy_init)와 전원 인가(phy_power_on) — 를 순서대로 밟는다.
 *
 * 동작 과정:
 *   1) phy_init() 으로 레인 설정 등 초기화를 요청한다.
 *   2) phy_power_on() 으로 전원을 넣는다.
 *   3) 실패하면 앞 단계를 되돌린다 — phy_power_on 이 실패하면 phy_exit() 를 불러
 *      phy_init 과 짝을 맞춘다. 이 짝 맞추기가 없으면 PHY 의 내부 참조 카운트가
 *      어긋나 다음 probe 시도에서 이상 동작한다.
 *   4) port->enabled 를 true 로 표시한다. 다만 이것은 "PHY 가 준비됐다"는 뜻일 뿐
 *      링크가 올라왔다는 뜻이 아니다 — 링크 판정은 나중에 별도로 이루어지고,
 *      그때 다시 false 가 될 수 있다.
 *
 * 실행 컨텍스트: probe 경로. PHY 프레임워크 호출은 잠들 수 있다.
 *
 * 에러 경로: phy_init 실패는 되돌릴 것이 없어 곧장 반환하고, phy_power_on 실패만
 * phy_exit() 를 거친다. 호출자는 실패한 포트를 리스트에서 떼어 낸다.
 *
 * 호출 체인:
 *   mt7621_pcie_init_ports() → [mt7621_pcie_init_port]
 *     → phy_init() → phy_power_on() (실패 시 phy_exit())
 */
static int mt7621_pcie_init_port(struct mt7621_pcie_port *port)
{
	/* [한국어] 로그에 쓸 컨트롤러를 포트에서 되찾는다. */
	struct mt7621_pcie *pcie = port->pcie;
	/* [한국어] dev_err 대상. */
	struct device *dev = pcie->dev;
	/* [한국어] 로그에 찍을 슬롯 번호를 미리 꺼내 둔다. */
	u32 slot = port->slot;
	/* [한국어] PHY API 반환값. */
	int err;

	/* [한국어] PHY 를 초기화한다. SerDes 레인 설정 등 실제 동작은 PHY 드라이버가 한다. */
	err = phy_init(port->phy);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 어느 포트의 PHY 인지 로그에 남긴다. */
		dev_err(dev, "failed to initialize port%d phy\n", slot);
		/* [한국어] 오류 전달 — 호출자가 이 포트를 리스트에서 떼어 낸다. */
		return err;
	}

	/* [한국어] PHY 에 전원을 넣는다. 초기화와 전원 인가가 분리되어 있는 것은 PHY 프레임워크의 규약이다. */
	err = phy_power_on(port->phy);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to power on port%d phy\n", slot);
		/* [한국어] 전원 인가가 실패했으므로 앞서 성공한 phy_init 을 되돌린다 — 짝을 맞추는 정리다. */
		phy_exit(port->phy);
		/* [한국어] 오류 전달. */
		return err;
	}

	/* [한국어] 이 포트는 쓸 준비가 되었다. 이후 링크 검사에서 다시 false 가 될 수도 있다. */
	port->enabled = true;

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * mt7621_pcie_reset_assert - 모든 포트의 RC 와 EP 리셋을 동시에 건다
 *
 * @pcie: 컨트롤러 객체.
 *
 * 왜 필요한가: 부팅 직후 하드웨어의 상태는 알 수 없다. 부트로더가 이미 링크를
 * 올려 두었을 수도, 반쯤 초기화하다 만 상태일 수도 있다. 알려진 상태에서
 * 출발하기 위해 초기화의 첫 단계로 양쪽 끝을 모두 리셋에 넣는다.
 *
 * 동작 과정: 모든 포트를 순회하며 RC 리셋(reset_control 경유)과
 * EP 리셋(PERST# GPIO 경유)을 함께 건 뒤, PERST_DELAY_MS 만큼 기다린다.
 * 그 지연은 PCIe 규격이 요구하는 리셋 유지 시간을 덮기 위한 것으로,
 * 이것이 없으면 일부 엔드포인트가 리셋을 인지하지 못한다.
 *
 * 리셋을 거는 경로가 두 벌인 것이 이 하드웨어의 특징이다 — RC 쪽은 SoC 의
 * 리셋 컨트롤러가, EP 쪽은 보드의 GPIO 핀이 담당한다.
 *
 * 실행 컨텍스트: probe 경로. msleep 으로 잠든다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   mt7621_pcie_init_ports() → [mt7621_pcie_reset_assert]
 *     → mt7621_control_assert() / mt7621_rst_gpio_pcie_assert() → msleep()
 */
static void mt7621_pcie_reset_assert(struct mt7621_pcie *pcie)
{
	/* [한국어] 순회용 포트 포인터. */
	struct mt7621_pcie_port *port;

	/* [한국어] 모든 포트를 순회한다. */
	list_for_each_entry(port, &pcie->ports, list) {
		/* PCIe RC reset assert */
		/* [한국어] 루트 컴플렉스 쪽 리셋을 건다. */
		mt7621_control_assert(port);

		/* PCIe EP reset assert */
		/* [한국어] 엔드포인트 쪽 PERST# 를 건다. RC 와 EP 를 모두 리셋 상태로 만들어
		 * 링크를 확실히 끊은 뒤 초기화를 시작하려는 것이다. */
		mt7621_rst_gpio_pcie_assert(port);
	}

	/* [한국어] PCIe 규격이 요구하는 리셋 유지 시간을 확보한다. 이 지연이 없으면 일부
	 * 엔드포인트가 리셋을 인지하지 못한다. */
	msleep(PERST_DELAY_MS);
}

/* [한국어]
 * mt7621_pcie_reset_rc_deassert - 루트 컴플렉스 쪽 리셋만 푼다
 *
 * @pcie: 컨트롤러 객체.
 *
 * 엔드포인트 리셋은 그대로 두고 RC 만 먼저 깨우는 것이 핵심이다. RC 가 준비되기
 * 전에 엔드포인트가 링크를 요청하면 훈련이 실패하므로, 반드시 RC → (PHY 초기화)
 * → EP 순서를 지켜야 한다. mt7621_pcie_init_ports() 가 그 순서를 강제한다.
 *
 * 여기에는 지연이 없다. RC 가 안정될 시간은 뒤이은 PHY 초기화와
 * msleep(INIT_PORTS_DELAY_MS) 가 대신 확보해 준다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   mt7621_pcie_init_ports() → [mt7621_pcie_reset_rc_deassert]
 *     → mt7621_control_deassert()
 */
static void mt7621_pcie_reset_rc_deassert(struct mt7621_pcie *pcie)
{
	/* [한국어] 순회용 포트 포인터. */
	struct mt7621_pcie_port *port;

	/* [한국어] 모든 포트의 RC 리셋만 해제한다. EP(PERST#)는 아직 걸린 채로 둔다 —
	 * RC 가 먼저 준비를 마쳐야 엔드포인트의 링크 요청을 받을 수 있기 때문이다. */
	list_for_each_entry(port, &pcie->ports, list)
		mt7621_control_deassert(port);
}

/* [한국어]
 * mt7621_pcie_reset_ep_deassert - 엔드포인트 리셋(PERST#)을 풀고 링크 학습을 기다린다
 *
 * @pcie: 컨트롤러 객체.
 *
 * 이 호출이 링크 훈련의 출발 신호다. 모든 포트의 PERST# 를 푼 뒤
 * PERST_DELAY_MS 만큼 기다린다. 그 지연 동안 엔드포인트는 전원·클럭을 안정시키고
 * 링크 훈련을 완료한다. 지연이 끝난 직후 호출자가 링크 상태를 한 번만 읽고
 * 판정하므로, 이 대기 시간이 부족하면 멀쩡한 카드가 "없음" 으로 잘못 판정된다.
 *
 * 실행 컨텍스트: probe 경로. msleep 으로 잠든다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   mt7621_pcie_init_ports() → [mt7621_pcie_reset_ep_deassert]
 *     → mt7621_rst_gpio_pcie_deassert() → msleep()
 */
static void mt7621_pcie_reset_ep_deassert(struct mt7621_pcie *pcie)
{
	/* [한국어] 순회용 포트 포인터. */
	struct mt7621_pcie_port *port;

	/* [한국어] 이제 엔드포인트 리셋도 해제한다. 이 순간부터 링크 학습이 시작된다. */
	list_for_each_entry(port, &pcie->ports, list)
		mt7621_rst_gpio_pcie_deassert(port);

	/* [한국어] 링크 학습과 엔드포인트 부팅에 필요한 시간을 기다린다. */
	msleep(PERST_DELAY_MS);
}

/* [한국어]
 * mt7621_pcie_init_ports - 리셋 시퀀스와 PHY 초기화를 거쳐 각 포트의 링크를 판정한다
 *
 * @pcie: 컨트롤러 객체.
 * @return: 0 = 최소 한 포트의 링크가 살아 있음.
 *       -ENODEV = 세 포트가 전부 링크 다운(연결된 카드가 하나도 없음).
 *
 * 왜 필요한가: 이 함수가 이 드라이버의 심장이다. 리셋 순서, PHY 공유 처리,
 * 링크 판정, 죽은 포트의 전력 차단이 모두 여기서 일어난다.
 *
 * 동작 과정:
 *   1) 모든 포트의 RC·EP 리셋을 걸고 기다린다.
 *   2) RC 리셋만 푼다.
 *   3) 포트를 순회하며 PHY 를 초기화한다. 단 슬롯 1 은 건너뛴다 —
 *      이 SoC 에서 포트 0 과 포트 1 이 하나의 PHY 를 공유하므로, 포트 0 이 이미
 *      초기화한 PHY 를 포트 1 이 다시 건드리면 안 된다. 슬롯 1 은 곧장
 *      enabled = true 로 표시하고 넘어간다.
 *      초기화가 실패한 포트는 list_del 로 리스트에서 떼어 낸다.
 *   4) INIT_PORTS_DELAY_MS 기다린 뒤 EP 리셋을 풀고 다시 기다린다.
 *   5) 남은 포트를 다시 순회하며 링크 업 비트를 확인한다. 죽은 포트는 리셋을 걸고
 *      enabled = false 로 표시하고 개수를 센다.
 *   6) 공유 PHY 절전 처리: 슬롯 0 이 죽었으면 판정을 미루고 tmp 에 기억해 둔다.
 *      이어서 슬롯 1 도 죽었다면 그 PHY 를 쓰는 쪽이 아무도 없으므로
 *      phy_power_off(tmp->phy) 로 전원을 내린다. 리스트에서 슬롯 0 이 슬롯 1 보다
 *      먼저 나온다는 전제가 깔려 있으며, 그 순서는 DT 자식 노드 순서에서 온다.
 *   7) 죽은 포트 수가 PCIE_PORT_CNT 와 같을 때만 -ENODEV.
 *
 * 변수 tmp 가 두 가지 의미로 재사용되는 점에 주의해야 한다 — 3)의 루프에서는
 * list_for_each_entry_safe 의 백업 포인터이고, 5)의 루프에서는 슬롯 0 포트를
 * 기억해 두는 임시 변수다.
 *
 * [상류 코드 관찰, 수정하지 않음] 3)에서 list_del 로 떼어 낸 포트의 pcie_rst 는
 * devm 자원이 아닌데, 이후 probe 의 remove_resets 순회나 remove() 의 순회에도
 * 잡히지 않아 반납되지 않는다.
 *
 * 실행 컨텍스트: probe 경로. msleep 이 세 번 들어가므로 총 300ms 가량 잠든다.
 *
 * 에러 경로: 개별 포트의 실패는 그 포트만 제외하고 계속 진행한다.
 * 전체 실패(-ENODEV)만 호출자에게 전달된다.
 *
 * 호출 체인:
 *   mt7621_pcie_probe() → [mt7621_pcie_init_ports]
 *     → mt7621_pcie_reset_assert() → mt7621_pcie_reset_rc_deassert()
 *     → mt7621_pcie_init_port() → mt7621_pcie_reset_ep_deassert()
 *     → mt7621_pcie_port_is_linkup() / mt7621_control_assert() / phy_power_off()
 */
static int mt7621_pcie_init_ports(struct mt7621_pcie *pcie)
{
	/* [한국어] 로그 대상. */
	struct device *dev = pcie->dev;
	/* [한국어] port 는 순회용, tmp 는 두 가지 용도로 재사용된다 — 앞 루프에서는
	 * list_for_each_entry_safe 의 백업 포인터, 뒤 루프에서는 슬롯 0 포트를 기억해 두는
	 * 임시 변수다. 같은 변수를 다른 의미로 재사용하는 점에 주의해야 한다. */
	struct mt7621_pcie_port *port, *tmp;
	/* [한국어] 링크가 죽은 포트의 개수. 세 개 모두 죽었을 때만 실패로 판정한다. */
	u8 num_disabled = 0;
	/* [한국어] init_port() 반환값. */
	int err;

	/* [한국어] RC 와 EP 를 모두 리셋 상태로 만든다. */
	mt7621_pcie_reset_assert(pcie);
	/* [한국어] RC 리셋만 먼저 해제한다. */
	mt7621_pcie_reset_rc_deassert(pcie);

	/* [한국어] _safe 판을 쓰는 이유: 루프 안에서 실패한 포트를 list_del 로 떼어 내기 때문이다. */
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		/* [한국어] 분기 판정에 쓸 슬롯 번호. */
		u32 slot = port->slot;

		/* [한국어] 슬롯 1 만 특별 취급한다. 이 SoC 에서 포트 0 과 포트 1 이 하나의 PHY 를 공유하므로,
		 * 포트 0 이 이미 초기화한 PHY 를 포트 1 이 다시 초기화하면 안 된다. */
		if (slot == 1) {
			/* [한국어] PHY 초기화를 건너뛰고 곧장 사용 가능으로 표시한다. */
			port->enabled = true;
			/* [한국어] 다음 포트로. */
			continue;
		}

		/* [한국어] 슬롯 0 과 2 는 각자의 PHY 를 초기화한다. */
		err = mt7621_pcie_init_port(port);
		/* [한국어] 실패 검사. */
		if (err) {
			/* [한국어] 실패 로그. */
			dev_err(dev, "initializing port %d failed\n", slot);
			/* [한국어] 실패한 포트를 리스트에서 떼어 낸다. 이후 어떤 순회에도 나타나지 않으므로
			 * 클럭도 켜지지 않고 레지스터 설정도 받지 않는다.
			 * [상류 코드 관찰, 수정하지 않음] 떼어 낸 포트의 pcie_rst 는 devm 자원이 아니어서
			 * probe/remove 의 리스트 순회로도 반납되지 않는다 — 그 리셋 컨트롤은 누수된다. */
			list_del(&port->list);
		}
	}

	/* [한국어] 모든 포트의 PHY 가 켜진 뒤 안정화 시간을 준다. */
	msleep(INIT_PORTS_DELAY_MS);
	/* [한국어] 이제 엔드포인트 리셋을 풀어 링크 학습을 시작시킨다. */
	mt7621_pcie_reset_ep_deassert(pcie);

	/* [한국어] 뒤 루프에서 "슬롯 0 포트"를 기억하는 용도로 재사용하기 위해 초기화한다. */
	tmp = NULL;
	/* [한국어] 링크 상태를 확인하기 위해 남아 있는 포트를 다시 순회한다. 이번에는 리스트를
	 * 변경하지 않으므로 _safe 판이 필요 없다. */
	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] 판정에 쓸 슬롯 번호. */
		u32 slot = port->slot;

		/* [한국어] 링크 업 비트가 서지 않았다면 카드가 없거나 링크 학습이 실패한 것이다. */
		if (!mt7621_pcie_port_is_linkup(port)) {
			/* [한국어] 어느 포트가 비었는지 알린다. 오류가 아니라 정보성 로그다. */
			dev_info(dev, "pcie%d no card, disable it (RST & CLK)\n",
				 slot);
			/* [한국어] 쓸 수 없는 포트는 리셋을 걸어 둔다 — 전력 절감과 오동작 방지 목적이다. */
			mt7621_control_assert(port);
			/* [한국어] 이후 enable_ports() 가 이 포트를 건너뛰게 한다. */
			port->enabled = false;
			/* [한국어] 실패 개수를 센다. */
			num_disabled++;

			/* [한국어] 슬롯 0 이 죽은 경우, 그 PHY 를 바로 끄면 안 된다. 슬롯 1 이 같은 PHY 를 쓰고 있어
			 * 슬롯 1 도 죽었는지 확인한 뒤에야 꺼도 되기 때문이다. */
			if (slot == 0) {
				/* [한국어] 슬롯 0 포트를 기억해 둔다. */
				tmp = port;
				/* [한국어] 판정을 미루고 다음 포트로. */
				continue;
			}

			/* [한국어] 슬롯 1 도 죽었고, 앞서 기억해 둔 슬롯 0 도 죽어 있다면 공유 PHY 를 쓰는 쪽이
			 * 아무도 없다는 뜻이다. 리스트 순서상 슬롯 0 이 슬롯 1 보다 먼저 나온다는 전제가
			 * 깔려 있으며, 그 순서는 DT 자식 노드 순서에서 온다. */
			if (slot == 1 && tmp && !tmp->enabled)
				/* [한국어] 공유 PHY 의 전원을 내린다. 슬롯 1 은 애초에 phy_init 을 하지 않았으므로
				 * 여기서 끄는 것은 슬롯 0 이 켠 PHY 다. */
				phy_power_off(tmp->phy);
		}
	}

	/* [한국어] 세 포트가 전부 죽었을 때만 -ENODEV, 하나라도 살아 있으면 0.
	 * PCIE_PORT_CNT 와 비교하므로 리스트에서 떼어 낸 포트가 있어도 개수는
	 * "링크 다운으로 판정된 수"만 센다는 점에 주의해야 한다. */
	return (num_disabled != PCIE_PORT_CNT) ? 0 : -ENODEV;
}

/* [한국어]
 * mt7621_pcie_enable_port - 링크가 살아 있는 포트의 레지스터를 실제로 설정한다
 *
 * @port: 대상 포트. 호출자가 port->enabled 를 이미 확인했다.
 *
 * 왜 필요한가: 링크가 올라왔다고 해서 바로 쓸 수 있는 것이 아니다. 네 가지를
 * 더 설정해야 PCI 코어가 이 포트를 정상적인 브리지로 열거하고, 그 아래 장치가
 * DMA 를 할 수 있다.
 *
 * 동작 과정 — 설정하는 네 가지:
 *   1) 인터럽트 허용: 공용 마스크 레지스터에서 이 포트의 비트
 *      PCIE_PORT_INT_EN(slot)(비트 20+slot)만 OR 로 켠다. 다른 포트의 설정을
 *      지우지 않기 위해 읽기-수정-쓰기를 쓴다.
 *   2) 인바운드 창: 포트의 BAR0 에 PCIE_BAR_MAP_MAX | PCIE_BAR_ENABLE 을 써서
 *      2GB DDR 영역을 연다. 아웃바운드(CPU → 장치)가 아니라 인바운드(장치 → DDR)
 *      설정이라는 점이 중요하다. 이것이 없으면 엔드포인트의 DMA 가 전부 실패한다.
 *   3) class code: 이 하드웨어는 기본 class 가 브리지가 아니어서, PCI-to-PCI
 *      브리지 코드(0x060400)와 revision ID 를 써 넣어야 코어가 하위 버스를 열거한다.
 *   4) FTS 개수: L0s 절전 상태에서 깨어날 때 보낼 훈련 시퀀스 개수를
 *      config space 의 벤더 전용 레지스터(0x70c)에 설정한다. 이 레지스터는
 *      MMIO 창이 아니라 config space 에 있어 read_config/write_config 를 쓴다.
 *
 * [상류 코드 관찰, 수정하지 않음] 위 영어 주석은 FTS 를 250 으로 설정한다고
 * 적었지만 실제 값은 PCIE_FTS_NUM_L0(0x50), 즉 십진 80 이다.
 *
 * 실행 컨텍스트: probe 경로. 락 없이 읽기-수정-쓰기를 하지만 순차 실행이라 안전하다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   mt7621_pcie_enable_ports() → [mt7621_pcie_enable_port]
 *     → pcie_read/pcie_write / pcie_port_write / read_config / write_config
 */
static void mt7621_pcie_enable_port(struct mt7621_pcie_port *port)
{
	/* [한국어] 레지스터 접근에 필요한 컨트롤러. */
	struct mt7621_pcie *pcie = port->pcie;
	/* [한국어] 인터럽트 비트 위치 계산에 쓸 슬롯 번호. */
	u32 slot = port->slot;
	/* [한국어] 읽고-수정하고-쓰기(read-modify-write)에 쓸 임시 변수. */
	u32 val;

	/* enable pcie interrupt */
	/* [한국어] 현재 인터럽트 마스크를 읽는다. */
	val = pcie_read(pcie, RALINK_PCI_PCIMSK_ADDR);
	/* [한국어] 이 포트의 허용 비트만 켠다. 다른 포트의 설정을 지우지 않기 위해 OR 를 쓴다. */
	val |= PCIE_PORT_INT_EN(slot);
	/* [한국어] 수정한 값을 되쓴다. 읽기-수정-쓰기 사이에 락이 없지만, probe 경로에서
	 * 순차적으로만 실행되므로 경쟁이 없다. */
	pcie_write(pcie, val, RALINK_PCI_PCIMSK_ADDR);

	/* map 2G DDR region */
	/* [한국어] BAR0 에 최대 범위 비트와 활성화 비트를 한 번에 쓴다. 이것은 아웃바운드가 아니라
	 * 인바운드 창 설정이다 — 엔드포인트가 DMA 로 호스트 DDR 에 닿을 수 있도록
	 * 2GB 영역을 열어 주는 것이며, 열어 주지 않으면 어떤 DMA 도 동작하지 않는다. */
	pcie_port_write(port, PCIE_BAR_MAP_MAX | PCIE_BAR_ENABLE,
			PCI_BASE_ADDRESS_0);

	/* configure class code and revision ID */
	/* [한국어] 포트의 class code 와 revision ID 를 써 넣는다. 이 하드웨어는 기본값이 브리지가
	 * 아니어서, 이 쓰기가 없으면 PCI 코어가 하위 버스를 열거하지 않는다. */
	pcie_port_write(port, PCIE_CLASS_CODE | PCIE_REVISION_ID,
			RALINK_PCI_CLASS);

	/* configure RC FTS number to 250 when it leaves L0s */
	/* [한국어] 현재 FTS 설정을 config space 에서 읽는다. MMIO 가 아니라 CF8 경로를 쓰는 이유는
	 * 0x70c 가 config space 안의 벤더 전용 레지스터이기 때문이다. */
	val = read_config(pcie, slot, PCIE_FTS_NUM);
	/* [한국어] 기존 FTS 필드를 지운다. */
	val &= ~PCIE_FTS_NUM_MASK;
	/* [한국어] 0x50(십진 80)을 FTS 개수로 넣는다. 위 영어 주석은 250 이라고 적었지만 실제 값은
	 * 0x50 = 80 이다 — 주석과 코드가 어긋나 있으며 상류 그대로 둔다.
	 * FTS 는 링크가 L0s 절전 상태에서 깨어날 때 보내는 훈련 시퀀스로, 개수가 많을수록
	 * 복원이 확실하지만 지연이 커진다. */
	val |= PCIE_FTS_NUM_L0(0x50);
	/* [한국어] 수정한 값을 되쓴다. */
	write_config(pcie, slot, PCIE_FTS_NUM, val);
}

/* [한국어]
 * mt7621_pcie_enable_ports - 주소 창을 설정하고 살아 있는 포트들의 클럭을 켠다
 *
 * @host: PCI 호스트 브리지. private 영역에 컨트롤러가 들어 있고,
 *       host->windows 에 DT ranges 에서 파싱된 자원 목록이 들어 있다.
 * @return: 0 = 성공. -EINVAL = 브리지 윈도 목록에 I/O 자원이 없음.
 *       그 밖의 음수 = 어느 포트의 클럭 활성화 실패.
 *
 * 왜 필요한가: 링크 판정까지 끝났으면 이제 주소 공간을 열어야 한다.
 * 컨트롤러 수준에서 메모리·I/O 창의 베이스를 잡고, 포트 수준에서 클럭을 켠 뒤
 * 레지스터를 설정하는 것이 이 함수의 일이다.
 *
 * 동작 과정:
 *   1) host->windows 에서 첫 I/O 자원을 찾는다. 없으면 -EINVAL — DT 의 ranges
 *      서술이 잘못된 것이므로 진행할 수 없다.
 *   2) 메모리 창 베이스에 0xffffffff 를 쓴다. 이 값이 무엇을 뜻하는지(창을 최대로
 *      여는 것인지, 사실상 비활성인지)는 이 트리의 코드만으로는 확인할 수 없다.
 *      상류 주석도 "Setup MEMWIN and IOWIN" 이라고만 적어 두었다.
 *   3) I/O 창 베이스에 entry->res->start - entry->offset 을 쓴다. res->start 는
 *      CPU 물리 주소이고 offset 은 CPU 주소와 PCI 버스 주소의 차이이므로,
 *      그 차를 빼면 PCI 버스 주소 0 에 대응하는 CPU 주소가 된다 —
 *      하드웨어가 그 기준점을 요구하는 형태다.
 *   4) enabled 인 포트만 클럭을 켜고 mt7621_pcie_enable_port() 로 레지스터를
 *      설정한 뒤, 활성화됐음을 로그로 알린다. 죽은 포트의 클럭은 끈 채로 두어
 *      전력을 아낀다.
 *
 * [상류 코드 관찰, 수정하지 않음] 4)에서 클럭 활성화가 실패하면 곧장 반환하는데,
 * 그 전에 켠 포트들의 클럭은 꺼지지 않는다. 호출자 probe() 도 remove_resets 로 가
 * 리셋만 반납할 뿐 클럭은 손대지 않는다.
 *
 * 실행 컨텍스트: probe 경로. clk_prepare_enable() 은 잠들 수 있다.
 *
 * 에러 경로: 두 갈래 모두 정리 없이 return 한다.
 *
 * 호출 체인:
 *   mt7621_pcie_probe() → [mt7621_pcie_enable_ports]
 *     → resource_list_first_type() / pcie_write() ×2
 *     → clk_prepare_enable() / mt7621_pcie_enable_port()
 */
static int mt7621_pcie_enable_ports(struct pci_host_bridge *host)
{
	/* [한국어] 브리지 뒤에 붙어 있는 private 영역에서 컨트롤러 객체를 꺼낸다. */
	struct mt7621_pcie *pcie = pci_host_bridge_priv(host);
	/* [한국어] 로그 대상. */
	struct device *dev = pcie->dev;
	/* [한국어] 순회용 포트 포인터. */
	struct mt7621_pcie_port *port;
	/* [한국어] 호스트 브리지의 자원 목록에서 항목을 받을 포인터. */
	struct resource_entry *entry;
	/* [한국어] 클럭 활성화 결과. */
	int err;
/* [한국어] [상류 코드 관찰] 이 아래 코드는 IORESOURCE_IO 자원이 반드시 있다고 전제한다. */

	/* [한국어] 브리지 윈도 목록에서 첫 번째 I/O 자원을 찾는다. DT 의 ranges 속성에서
	 * PCI 코어가 미리 파싱해 넣어 둔 것이다. */
	entry = resource_list_first_type(&host->windows, IORESOURCE_IO);
	/* [한국어] I/O 창이 없으면 아래 IOBASE 설정을 할 수 없다. */
	if (!entry) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "cannot get io resource\n");
		/* [한국어] DT 서술이 잘못된 것이므로 -EINVAL. */
		return -EINVAL;
	}

	/* Setup MEMWIN and IOWIN */
	/* [한국어] 메모리 창 베이스를 0xffffffff 로 설정한다. 이 하드웨어에서 이 값이 무엇을
	 * 뜻하는지(창을 최대로 여는 것인지, 사실상 비활성인지)는 이 트리의 코드만으로는
	 * 확인할 수 없다. 상류가 "Setup MEMWIN and IOWIN" 이라고만 적어 두었다. */
	pcie_write(pcie, 0xffffffff, RALINK_PCI_MEMBASE);
	/* [한국어] I/O 창 베이스에 CPU 물리 주소를 쓴다. entry->offset 을 빼는 이유는
	 * entry->res->start 가 CPU 주소인데 이 레지스터는 그 차이를 스스로 더하기 때문이며,
	 * 결과적으로 PCI 버스 주소 0 에 대응하는 CPU 주소를 알려 주는 셈이다. */
	pcie_write(pcie, entry->res->start - entry->offset, RALINK_PCI_IOBASE);

	/* [한국어] 살아 있는 포트만 실제로 켠다. */
	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] init_ports() 가 링크 업으로 판정한 포트만 통과한다. */
		if (port->enabled) {
			/* [한국어] 이 포트의 클럭 게이트를 켠다. 링크가 죽은 포트의 클럭은 끈 채로 두어 전력을 아낀다. */
			err = clk_prepare_enable(port->clk);
			/* [한국어] 클럭 활성화 실패. */
			if (err) {
				/* [한국어] 어느 포트인지 로그에 남긴다. */
				dev_err(dev, "enabling clk pcie%d\n",
					port->slot);
				/* [한국어] [상류 코드 관찰, 수정하지 않음] 여기서 곧장 반환하면 앞서 켠 포트들의 클럭이
				 * 꺼지지 않고, 호출자인 probe() 는 remove_resets 로 가 리셋만 반납한다. */
				return err;
			}

			/* [한국어] 레지스터 설정(인터럽트 허용, 인바운드 BAR, class code, FTS)을 적용한다. */
			mt7621_pcie_enable_port(port);
			/* [한국어] 어느 포트가 최종적으로 활성화됐는지 알린다. */
			dev_info(dev, "PCIE%d enabled\n", port->slot);
		}
	}

	/* [한국어] 모든 활성 포트를 성공적으로 켰다. */
	return 0;
}

/* [한국어]
 * mt7621_pcie_register_host - PCI 코어에 호스트 브리지를 등록하고 버스를 스캔한다
 *
 * @host: 등록할 호스트 브리지.
 * @return: pci_host_probe() 의 반환값. 0 = 성공, 음수 = 스캔/등록 실패.
 *
 * 왜 필요한가: 하드웨어 준비가 모두 끝난 마지막 단계다. config 접근 방법과
 * private 포인터를 브리지에 알린 뒤 코어에 넘기면, 코어가 버스를 스캔하고
 * 장치를 만들고 자원을 배정하고 드라이버를 바인딩한다.
 *
 * 동작 과정:
 *   1) host->ops 에 mt7621_pcie_ops 를 건다. 이 테이블은 map_bus 만 이 파일이
 *      구현하고 read/write 는 코어의 범용 구현을 쓴다.
 *   2) host->sysdata 에 컨트롤러를 심는다. map_bus 콜백이 bus->sysdata 로
 *      이 값을 되찾으므로, 이 대입이 없으면 첫 config 접근에서 NULL 역참조가 난다.
 *   3) pci_host_probe() 를 부른다.
 *
 * 실행 컨텍스트: probe 경로. 하위 장치들의 probe 까지 유발하므로 오래 걸리고
 * 잠들 수 있다.
 *
 * 에러 경로: 반환값을 그대로 probe 의 결과로 넘긴다.
 *
 * 호출 체인:
 *   mt7621_pcie_probe() → [mt7621_pcie_register_host] → pci_host_probe()
 *     → pci_scan_root_bus_bridge → pci_bus_add_devices → 각 드라이버의 probe()
 */
static int mt7621_pcie_register_host(struct pci_host_bridge *host)
{
	/* [한국어] 브리지의 private 영역에서 컨트롤러를 꺼낸다. */
	struct mt7621_pcie *pcie = pci_host_bridge_priv(host);

	/* [한국어] config 접근 방법(map_bus + 코어 범용 read/write)을 브리지에 알린다. */
	host->ops = &mt7621_pcie_ops;
	/* [한국어] map_bus 콜백이 bus->sysdata 로 되찾을 수 있도록 컨트롤러를 심는다. */
	host->sysdata = pcie;
	/* [한국어] 버스 스캔, 자원 배정, 드라이버 바인딩을 코어에 맡긴다. 이 호출이 끝나면
	 * 하위 장치들이 시스템에 나타난다. */
	return pci_host_probe(host);
}

static const struct soc_device_attribute mt7621_pcie_quirks_match[] = {
	/* [한국어] SoC ID 가 "mt7621" 이고 리비전이 "E2" 인 칩만 매칭한다. 이 조합에서만
	 * 리셋 신호 극성이 뒤집혀 있다. */
	{ .soc_id = "mt7621", .revision = "E2" },
	/* [한국어] 테이블 끝을 알리는 빈 항목. */
	{ /* sentinel */ }
};

/* [한국어]
 * mt7621_pcie_probe - MT7621 PCIe 컨트롤러를 초기화하고 PCI 버스를 올린다
 *
 * @pdev: DT 에서 "mediatek,mt7621-pci" 로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공. -ENODEV = DT 노드 없음. -ENOMEM = 브리지 할당 실패.
 *       그 밖의 음수 = DT 파싱 실패 또는 포트 활성화 실패.
 *       주의: 연결된 카드가 하나도 없을 때도 0 을 돌려준다(아래 관찰 참조).
 *
 * 왜 필요한가: 이 드라이버의 진입점이다. 다섯 단계를 순서대로 밟는다.
 *
 * 동작 과정:
 *   1) DT 노드가 있는지 확인한다. 없으면 포트 정보를 알 수 없으므로 -ENODEV.
 *   2) devm_pci_alloc_host_bridge() 로 브리지와 private 영역을 함께 할당하고,
 *      dev 를 심고, drvdata 를 설정하고, 포트 리스트를 초기화한다.
 *   3) soc_device_match() 로 이 칩이 리셋 극성 quirk 대상(mt7621 리비전 E2)인지
 *      판별해 pcie->resets_inverted 를 정한다. 이후 모든 리셋 조작이 이 값을 본다.
 *   4) mt7621_pcie_parse_dt() → mt7621_pcie_init_ports() → mt7621_pcie_enable_ports()
 *      순으로 진행한다.
 *   5) mt7621_pcie_register_host() 로 코어에 넘긴다.
 *
 * [상류 코드 관찰, 수정하지 않음] 세 가지가 눈에 띈다.
 *   (a) init_ports() 가 -ENODEV 를 돌려주면 오류 로그를 남기고도 0(성공)을
 *       반환한다. 그 결과 probe 는 성공으로 처리되어 드라이버가 바인딩된 채
 *       남지만, 호스트 브리지는 등록되지 않고 잡아 둔 리셋 컨트롤도 반납되지
 *       않는다. "카드가 없는 것은 오류가 아니다" 라는 의도로 보이나 정리가 빠졌다.
 *   (b) parse_dt() 실패 경로도 remove_resets 를 거치지 않아, 이미 만들어진
 *       포트들의 리셋 컨트롤이 누수된다.
 *   (c) 결국 remove_resets 라벨로 가는 것은 enable_ports() 실패 하나뿐이다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 * 총 300ms 가량 잠들며, 장치마다 한 번만 불린다.
 *
 * 에러 경로: 위 관찰대로 일관되지 않다. 정리가 필요한 유일한 자원은
 * devm_ 이 아닌 port->pcie_rst 이고, 나머지(브리지, 포트 구조체, MMIO, 클럭,
 * PHY, GPIO)는 모두 devm 이라 드라이버 코어가 되돌린다.
 *
 * 호출 체인:
 *   DT 매칭 → platform_driver.probe == [mt7621_pcie_probe]
 *     → devm_pci_alloc_host_bridge() / soc_device_match()
 *     → mt7621_pcie_parse_dt() → mt7621_pcie_init_ports()
 *     → mt7621_pcie_enable_ports() → mt7621_pcie_register_host()
 */
static int mt7621_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 플랫폼 디바이스의 struct device — 모든 devm 할당과 로그의 기준. */
	struct device *dev = &pdev->dev;
	/* [한국어] soc_device_match() 결과를 받을 포인터. NULL 이면 해당 quirk 대상이 아니다. */
	const struct soc_device_attribute *attr;
	/* [한국어] 오류 정리 구간에서 쓸 순회용 포인터. */
	struct mt7621_pcie_port *port;
	/* [한국어] 컨트롤러 객체. */
	struct mt7621_pcie *pcie;
	/* [한국어] PCI 호스트 브리지 객체. */
	struct pci_host_bridge *bridge;
	/* [한국어] 각 단계의 반환값. */
	int err;

	/* [한국어] DT 노드 없이는 포트 정보를 알 수 없으므로 진행할 수 없다. */
	if (!dev->of_node)
		/* [한국어] 장치가 없는 것으로 처리한다. */
		return -ENODEV;

	/* [한국어] 브리지와 private 영역(sizeof(*pcie))을 한 번에 할당한다. devm 이라 자동 해제된다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 메모리 부족. */
	if (!bridge)
		/* [한국어] -ENOMEM 전달. */
		return -ENOMEM;

	/* [한국어] 방금 할당한 private 영역의 주소. */
	pcie = pci_host_bridge_priv(bridge);
	/* [한국어] 로그와 이후 devm 할당의 기준이 될 디바이스를 심는다. */
	pcie->dev = dev;
	/* [한국어] remove() 가 되찾을 수 있도록 플랫폼 디바이스에 컨트롤러를 매단다. */
	platform_set_drvdata(pdev, pcie);
	/* [한국어] 포트 리스트를 빈 상태로 초기화한다. parse_dt 가 곧 채운다. */
	INIT_LIST_HEAD(&pcie->ports);

	/* [한국어] 현재 SoC 가 리셋 극성 quirk 대상인지 조회한다. */
	attr = soc_device_match(mt7621_pcie_quirks_match);
	/* [한국어] 매칭되면 attr 이 NULL 이 아니다. */
	if (attr)
		/* [한국어] 이후 control_assert/deassert 의 분기를 뒤집는다. */
		pcie->resets_inverted = true;

	/* [한국어] DT 를 읽어 공용 레지스터 창을 매핑하고 자식 노드마다 포트를 만든다. */
	err = mt7621_pcie_parse_dt(pcie);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "parsing DT failed\n");
		/* [한국어] [상류 코드 관찰, 수정하지 않음] 이미 리스트에 붙은 포트들의 pcie_rst 는
		 * devm 자원이 아닌데 여기서 반납되지 않는다 — remove_resets 라벨을 거치지 않는다. */
		return err;
	}

	/* [한국어] 리셋 시퀀스와 PHY 초기화를 거쳐 각 포트의 링크 상태를 판정한다. */
	err = mt7621_pcie_init_ports(pcie);
	/* [한국어] 세 포트가 모두 죽으면 -ENODEV 가 온다. */
	if (err) {
		/* [한국어] 연결된 카드가 하나도 없다는 뜻이다. */
		dev_err(dev, "nothing connected in virtual bridges\n");
		/* [한국어] [상류 코드 관찰, 수정하지 않음] 오류를 로그로 남기고도 0(성공)을 돌려준다.
		 * 그 결과 probe 는 성공한 것으로 처리되어 드라이버가 바인딩된 채 남지만,
		 * 호스트 브리지는 등록되지 않고 잡아 둔 리셋 컨트롤도 반납되지 않는다.
		 * 카드가 없는 것을 오류로 취급하지 않으려는 의도로 보이나, 자원 정리는 빠져 있다. */
		return 0;
	}

	/* [한국어] 살아 있는 포트들의 클럭을 켜고 레지스터를 설정한다. */
	err = mt7621_pcie_enable_ports(bridge);
	/* [한국어] 실패 검사. */
	if (err) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "error enabling pcie ports\n");
		/* [한국어] 여기서만 정리 구간으로 간다 — 앞의 두 실패 경로와 다른 점이다. */
		goto remove_resets;
	}

	/* [한국어] 모든 준비가 끝났으므로 PCI 코어에 브리지를 등록하고 버스를 스캔한다.
	 * 이 반환값이 곧 probe 의 최종 결과다. */
	return mt7621_pcie_register_host(bridge);

/* [한국어] enable_ports 실패 전용 정리 라벨. */
remove_resets:
	/* [한국어] 리스트에 남아 있는 모든 포트를 순회한다. */
	list_for_each_entry(port, &pcie->ports, list)
		/* [한국어] devm 이 아닌 유일한 자원인 리셋 컨트롤을 반납한다. */
		reset_control_put(port->pcie_rst);

	/* [한국어] enable_ports 가 준 오류를 그대로 돌려준다. */
	return err;
}

/* [한국어]
 * mt7621_pcie_remove - 드라이버 언바인드 시 devm 이 아닌 자원을 반납한다
 *
 * @pdev: 제거되는 플랫폼 디바이스. probe 가 platform_set_drvdata 로 심어 둔
 *       컨트롤러 객체를 되찾는 열쇠다.
 *
 * 왜 필요한가: probe 가 잡은 자원 가운데 자동 해제되지 않는 것은
 * of_reset_control_get_exclusive() 로 얻은 포트별 리셋 컨트롤 하나뿐이다.
 * 그것만 손으로 반납하면 나머지(브리지, 포트 구조체, MMIO 매핑, 클럭, PHY,
 * GPIO)는 devm 이 드라이버 코어의 정리 단계에서 되돌려 준다.
 *
 * 동작 과정: drvdata 에서 컨트롤러를 꺼내 포트 리스트를 순회하며
 * reset_control_put() 을 부른다. 그것이 전부다.
 *
 * [상류 코드 관찰, 수정하지 않음] 두 가지가 빠져 있다.
 *   (a) pci_host_probe() 로 등록한 버스를 제거하지 않는다 —
 *       pci_stop_root_bus()/pci_remove_root_bus() 에 해당하는 호출이 없다.
 *   (b) enable_ports() 가 켠 클럭을 끄지 않는다.
 *   다만 이 드라이버는 builtin_platform_driver 로 등록되어 모듈 언로드 경로가
 *   없으므로, 실제로 이 함수가 불리는 것은 sysfs 를 통한 수동 언바인드뿐이다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 void 다.
 *
 * 호출 체인:
 *   드라이버 코어의 언바인드 → platform_driver.remove == [mt7621_pcie_remove]
 *     → reset_control_put() (포트마다 한 번)
 */
static void mt7621_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] probe 가 platform_set_drvdata 로 심어 둔 컨트롤러를 되찾는다. */
	struct mt7621_pcie *pcie = platform_get_drvdata(pdev);
	/* [한국어] 순회용 포인터. */
	struct mt7621_pcie_port *port;

	/* [한국어] 남아 있는 포트를 모두 순회한다. */
	list_for_each_entry(port, &pcie->ports, list)
		/* [한국어] 리셋 컨트롤을 반납한다. 나머지 자원(브리지, 포트 구조체, MMIO 매핑, 클럭, PHY,
		 * GPIO)은 전부 devm 이라 드라이버 코어가 알아서 되돌린다.
		 * [상류 코드 관찰] pci_host_probe() 로 등록한 버스를 여기서 제거하지 않는다. */
		reset_control_put(port->pcie_rst);
}

static const struct of_device_id mt7621_pcie_ids[] = {
	/* [한국어] DT 의 compatible 문자열. 이 문자열을 가진 노드에 이 드라이버가 붙는다. */
	{ .compatible = "mediatek,mt7621-pci" },
	/* [한국어] 테이블 끝을 알리는 빈 항목. */
	{},
};
/* [한국어] 모듈 자동 로딩을 위해 이 테이블을 모듈 메타데이터로 내보낸다. */
MODULE_DEVICE_TABLE(of, mt7621_pcie_ids);

static struct platform_driver mt7621_pcie_driver = {
	/* [한국어] 장치가 나타났을 때 불릴 진입점. */
	.probe = mt7621_pcie_probe,
	/* [한국어] 장치가 사라질 때 불릴 정리 함수. 반환값이 void 인 최신 플랫폼 드라이버 규약을 따른다. */
	.remove = mt7621_pcie_remove,
	.driver = {
		/* [한국어] 드라이버 이름 — sysfs 와 로그에 나타난다. */
		.name = "mt7621-pci",
		/* [한국어] 위에서 정의한 DT 매칭 테이블 연결. */
		.of_match_table = mt7621_pcie_ids,
	},
};
/* [한국어] module_platform_driver 가 아니라 builtin_ 판이다. 즉 이 드라이버는 모듈로
 * 빌드되지 않고 커널에 내장되며, 언로드 경로가 없다. SoC 부팅 초기에 반드시
 * 필요한 버스라 모듈로 미루면 곤란하기 때문이다.
 * 아래 MODULE_* 매크로는 내장 빌드에서도 modinfo 문자열로 남는다. */
builtin_platform_driver(mt7621_pcie_driver);

/* [한국어] modinfo 에 표시될 설명. */
MODULE_DESCRIPTION("MediaTek MT7621 PCIe host controller driver");
/* [한국어] 라이선스 선언. */
MODULE_LICENSE("GPL v2");
