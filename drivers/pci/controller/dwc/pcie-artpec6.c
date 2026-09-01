// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Axis ARTPEC-6 SoC
 *
 * Author: Niklas Cassel <niklas.cassel@axis.com>
 *
 * Based on work done by Phil Edworthy <phil@edworthys.org>
 */

/* [한국어] usleep_range() 선언. 이 파일은 PHY 준비를 기다릴 때 바쁜 대기 대신
 * 잠드는 대기를 쓴다 — probe 경로라 잠들 수 있기 때문이다. */
/*
 * [한국어 설명] Axis ARTPEC-6/7 SoC 의 DesignWare PCIe 결합 계층 (pcie-artpec6.c)
 *
 * === 파일의 역할 ===
 * Axis Communications 의 ARTPEC-6 / ARTPEC-7 SoC 에 들어 있는 PCIe 컨트롤러를
 * 초기화하는 드라이버다. 컨트롤러 IP 는 Synopsys DesignWare(DWC) PCIe 이므로
 * 링크 관리·주소 변환·버스 스캔 같은 공통 로직은 이웃 파일 pcie-designware*.c 가
 * 담당하고, 이 파일은 SoC 고유 부분만 얹는다.
 * 이 드라이버의 특징은 두 가지다. 첫째, 같은 하드웨어를 루트 컴플렉스(RC)로도
 * 엔드포인트(EP)로도 쓸 수 있어 한 소스가 두 모드를 모두 담는다. 어느 쪽인지는
 * DT 의 compatible 문자열이 결정하며(-ep 로 끝나면 EP), 매칭 테이블 항목이
 * (세대 2 × 모드 2) = 4개다. 둘째, 제어 레지스터가 PCIe IP 안이 아니라 SoC 의
 * 공용 시스템 컨트롤러(syscon) 블록에 흩어져 있다. 그래서 MMIO 를 직접 매핑하지
 * 않고 DT 의 axis,syscon-pcie phandle 로 얻은 regmap 을 빌려 쓴다 —
 * PCIECFG / PCIESTAT / NOCCFG 세 레지스터가 모두 그 안에 있다. 예외는 PHY
 * 레지스터 블록 하나로, 그것만 별도 MMIO 로 매핑한다.
 * 실제로 하는 일은 하드웨어 시퀀스 네 단계다 — 코어 리셋 어서트 → PHY 초기화 →
 * 코어 리셋 해제 → PHY 준비 대기. 그 순서가 절대적이다. 코어가 동작 중일 때
 * PHY 설정을 바꾸면 진행 중인 트랜잭션이 깨지기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층으로 보면 PCI 코어 → DWC 공용 코어(pcie-designware-host.c 또는 -ep.c) →
 * 이 파일 → syscon regmap / PHY MMIO → SoC 하드웨어다.
 * 진입 경로는 모드에 따라 갈린다. RC 면 probe 가 dw_pcie_host_init() 을 부르고,
 * 그 안에서 코어가 artpec6_pcie_host_init() 을 되불러 하드웨어 시퀀스를 실행한
 * 뒤 artpec6_pcie_establish_link() 로 링크 훈련을 시작한다. EP 면
 * dw_pcie_ep_init() → artpec6_pcie_ep_init() → dw_pcie_ep_init_registers() →
 * pci_epc_init_notify() 순으로 진행하고, 이후 EPF 드라이버가
 * artpec6_pcie_raise_irq() 로 호스트에 인터럽트를 올린다.
 * 두 모드가 공유하는 것은 cpu_addr_fixup 콜백인데, 기준 주소가 모드에 따라
 * 달라진다(RC 는 pp->cfg0_base, EP 는 ep->phys_base).
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. PHY 초기화와 대기에 usleep_range 로
 * 잠드는 구간이 있어 최대 200ms 남짓 걸릴 수 있다. 이 드라이버는
 * builtin_platform_driver 로 커널에 내장되고 remove 콜백이 없으며,
 * suppress_bind_attrs = true 로 sysfs bind/unbind 자체를 막아 둔다 —
 * 정리 경로가 없는 드라이버가 취해야 할 올바른 조치다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-designware.h 의 struct dw_pcie / dw_pcie_rp / dw_pcie_ep,
 * 세 개의 콜백 테이블(dw_pcie_ops / dw_pcie_host_ops / dw_pcie_ep_ops),
 * dw_pcie_host_init(), dw_pcie_ep_init(), dw_pcie_ep_init_registers(),
 * dw_pcie_ep_raise_msi_irq(), DWC_EPC_COMMON_FEATURES(:649),
 * 그리고 n_fts[2] 필드(:1972)와 그 값이 쓰이는 두 레지스터(:270 PCIE_PORT_AFR,
 * :303 PORT_LOGIC_N_FTS_MASK).
 * 아래쪽: syscon/regmap(제어 레지스터 접근), 별도 매핑한 PHY MMIO,
 * 그리고 DT(of_device_get_match_data 로 세대·모드를 얻는다).
 * 이 드라이버가 클럭·리셋·PHY 프레임워크를 전혀 쓰지 않는다는 점이 특징이다 —
 * 전원·클럭·리셋이 모두 syscon 레지스터 비트로 표현되기 때문이다.
 * 데이터 흐름: DT compatible → (세대, 모드) → struct artpec6_pcie →
 * syscon 레지스터 쓰기로 PHY 와 코어 제어 → 링크 훈련 → DWC 코어가 버스 스캔
 * (RC) 또는 EPC 등록(EP)을 진행한다.
 * 공유 상태: syscon regmap 은 다른 SoC 블록과 공유하는 레지스터 블록이지만,
 * regmap 이 자체 락으로 접근을 직렬화하므로 이 파일에는 락이 하나도 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct artpec6_pcie: 컨트롤러 하나. dw_pcie 를 "포인터로" 들고 있고,
 *   syscon regmap, PHY MMIO 주소, 세대(variant), 모드(mode)를 담는다.
 *   dw_pcie 를 내장하지 않았기 때문에 to_artpec6_pcie() 가 container_of 를
 *   쓸 수 없고 drvdata 에 의존한다 — probe 에서 platform_set_drvdata() 가
 *   초기화보다 앞서야 하는 이유다.
 * - artpec6_pcie_probe(): 진입점. DT 매칭 데이터로 세대·모드를 정하고,
 *   PHY 매핑과 syscon 조회를 마친 뒤 모드별로 DWC 초기화에 넘긴다.
 * - artpec6_pcie_host_init() / artpec6_pcie_ep_init(): 같은 네 단계 하드웨어
 *   시퀀스를 실행한다. 차이는 RC 쪽만 ARTPEC-7 에서 n_fts 를 180 으로 두는 것뿐이다.
 * - init_phy_a6() / init_phy_a7(): 세대별 PHY 초기화. 6 세대는 4단계 11ms 로
 *   아날로그 설정·클럭·PLL 을 차례로 켜고, 7 세대는 3단계 50us 로 끝나며
 *   대신 PCIESTAT 를 읽어 외부/내부 레퍼런스 클럭을 스스로 고른다.
 * - wait_for_phy_a6() / wait_for_phy_a7(): 세대별 준비 대기. 두 판 모두 먼저
 *   NOC 유휴 이탈을 기다리고, 그다음 6 세대는 PLL 잠금(비트가 서기)을,
 *   7 세대는 PHY 의 Pn 상태 진입(TX/RX ACK 비트가 내려가기)을 기다린다 —
 *   폴링 극성이 반대다.
 * - assert/deassert_core_reset(): 세대별 리셋. 6 세대는 CORE_RESET_REQ(bit 21)를
 *   세워 리셋을 걸고, 7 세대는 NOC_RESET(bit 3)을 지워 건다. 극성이 반대일 뿐
 *   아니라 7 세대의 bit 3 은 6 세대의 MODE_TX_DRV_EN 과 같은 자리다.
 * - artpec6_pcie_cpu_addr_fixup(): iATU 가 요구하는 상대 주소로 변환.
 *   기준이 RC 는 cfg0_base, EP 는 phys_base 다.
 * - [상류 코드 관찰, 수정하지 않음] 네 가지를 기록해 둔다.
 *   (a) probe 의 default 갈래와 raise_irq 의 default 갈래가 오류를 로그로만
 *   남기고 0(성공)을 돌려준다. (b) wait_for_phy 계열은 시간 초과를 오류로
 *   돌려주지 않아, PHY 가 준비되지 않아도 링크 훈련이 시작된다. 또 탈출 조건과
 *   retries 소진이 같은 반복에서 겹치면 성공했는데도 오류 로그가 찍힌다.
 *   (c) readl/writel 래퍼가 regmap 반환값을 검사하지 않아, 읽기 실패 시
 *   초기화되지 않은 스택 값이 반환된다. (d) 파일 앞의
 *   artpec6_pcie_of_match[] 전방 선언은 of_device_get_match_data() 로 바뀐 뒤
 *   쓰이지 않는 죽은 선언이다.
 */

#include <linux/delay.h>
/* [한국어] 커널 공통 정의(-EINVAL/-ENODEV/-ENOMEM 등). */
#include <linux/kernel.h>
/* [한국어] __init 계열 매크로. 이 파일에서 직접 쓰지는 않지만 관례적으로 포함한다. */
#include <linux/init.h>
/* [한국어] of_device_get_match_data() 와 struct of_device_id 정의.
 * DT 매칭 결과에서 변종·모드 정보를 꺼내는 데 필수다. */
#include <linux/of.h>
/* [한국어] PCI 코어 공개 API. PCI_IRQ_INTX / PCI_IRQ_MSI 상수가 여기서 온다. */
#include <linux/pci.h>
/* [한국어] platform_driver / devm_platform_ioremap_resource_byname /
 * platform_set_drvdata 선언. */
#include <linux/platform_device.h>
/* [한국어] struct resource 정의. */
#include <linux/resource.h>
/* [한국어] 시그널 관련 정의. 이 파일에서 직접 쓰는 심볼은 없다. */
#include <linux/signal.h>
/* [한국어] u16/u32/u64 등 고정폭 정수 타입. */
#include <linux/types.h>
/* [한국어] 인터럽트 관련 기본 정의. */
#include <linux/interrupt.h>
/* [한국어] syscon_regmap_lookup_by_phandle() 선언. 이 컨트롤러의 제어 레지스터는
 * PCIe IP 안이 아니라 SoC 의 공용 시스템 컨트롤러(syscon) 안에 있어서,
 * MMIO 를 직접 매핑하는 대신 그 syscon 의 regmap 을 빌려 쓴다. */
#include <linux/mfd/syscon.h>
/* [한국어] regmap_read()/regmap_write() 선언. 위 syscon 접근의 실제 수단이다. */
#include <linux/regmap.h>

/* [한국어] DesignWare PCIe 공용 코어 헤더. struct dw_pcie / dw_pcie_rp / dw_pcie_ep 와
 * dw_pcie_ops / dw_pcie_host_ops / dw_pcie_ep_ops, dw_pcie_host_init(),
 * dw_pcie_ep_init(), dw_pcie_ep_raise_msi_irq(), DWC_EPC_COMMON_FEATURES(:649),
 * 그리고 n_fts[2] 필드(:1972)가 모두 여기서 온다. */
#include "pcie-designware.h"

/* [한국어] struct dw_pcie 포인터에서 이 파일의 struct artpec6_pcie 를 되찾는 매크로.
 * container_of 가 아니라 drvdata 를 쓰므로, probe 에서
 * platform_set_drvdata() 가 실행된 뒤에만 유효하다. 실제 사용처인 콜백들은
 * 모두 그 대입(:429) 이후에 불린다. */
#define to_artpec6_pcie(x)	dev_get_drvdata((x)->dev)

enum artpec_pcie_variants {
	/* [한국어] ARTPEC-6 SoC. PHY 초기화 순서와 코어 리셋 비트가 7 세대와 다르다. */
	ARTPEC6,
	/* [한국어] ARTPEC-7 SoC. 외부 레퍼런스 클럭 선택과 NOC 리셋 방식이 추가되었다. */
	ARTPEC7,
};

struct artpec6_pcie {
	/* [한국어] DesignWare 공용 코어 객체 포인터.
	 * 설정자: artpec6_pcie_probe() 가 별도로 devm_kzalloc 한 객체를 가리키게 한다.
	 *   다른 DWC 드라이버들이 struct dw_pcie 를 자기 구조체에 내장하는 것과 달리
	 *   이 파일은 포인터로 들고 있다 — 그래서 to_artpec6_pcie 가 container_of 를
	 *   쓸 수 없고 drvdata 에 의존한다.
	 * 읽는 자: 로그(pci->dev)와 콜백에서 rp/ep 하위 객체에 닿을 때.
	 * 값 범위: 항상 유효(NULL 불가).
	 * 동기화: DWC 코어가 관리한다. */
	struct dw_pcie		*pci;
	/* [한국어] SoC 시스템 컨트롤러(syscon)의 regmap. PCIECFG/PCIESTAT/NOCCFG 레지스터가
	 * 이 regmap 안에 있다.
	 * 설정자: probe 가 DT 의 axis,syscon-pcie phandle 로 조회해 채운다.
	 * 읽는 자: artpec6_pcie_readl()/artpec6_pcie_writel() 만이 쓴다.
	 * 값 범위: 유효한 regmap 포인터.
	 * 동기화: regmap 이 자체 락으로 보호하므로 이 파일에는 락이 없다. */
	struct regmap		*regmap;	/* DT axis,syscon-pcie */
	/* [한국어] PHY 레지스터 블록의 가상 주소(DT 의 "phy" 자원).
	 * 설정자: probe 가 devm_platform_ioremap_resource_byname 으로 매핑한다.
	 * 읽는 자: wait_for_phy_a6() 가 readl 로 PHY_STATUS 를,
	 *   wait_for_phy_a7() 이 readw 로 TX/RX ASIC_OUT 을 읽는다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: probe 경로에서만 접근하므로 락이 없다. */
	void __iomem		*phy_base;	/* DT phy */
	/* [한국어] 이 SoC 가 ARTPEC6 인지 ARTPEC7 인지.
	 * 설정자: probe 가 DT 매칭 데이터에서 꺼낸다.
	 * 읽는 자: init_phy/wait_for_phy/assert_core_reset/deassert_core_reset 의
	 *   switch 분기와 host_init 의 n_fts 설정.
	 * 값 범위: ARTPEC6 또는 ARTPEC7.
	 * 동기화: 설정 후 읽기 전용. */
	enum artpec_pcie_variants variant;
	/* [한국어] 이 인스턴스가 루트 컴플렉스(RC)인지 엔드포인트(EP)인지.
	 * 설정자: probe 가 DT 매칭 데이터에서 꺼낸다 — compatible 문자열이
	 *   -ep 로 끝나는지로 갈린다.
	 * 읽는 자: cpu_addr_fixup 의 주소 변환 기준 선택과 probe 의 초기화 분기.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: 설정 후 읽기 전용. */
	enum dw_pcie_device_mode mode;
};

struct artpec_pcie_of_data {
	/* [한국어] 이 DT 항목이 가리키는 SoC 세대.
	 * 설정자: 아래 네 개의 정적 상수 정의.
	 * 읽는 자: probe 의 of_device_get_match_data() 결과 해석.
	 * 값 범위: ARTPEC6 또는 ARTPEC7.
	 * 동기화: const 정적 데이터이므로 필요 없다. */
	enum artpec_pcie_variants variant;
	/* [한국어] 이 DT 항목이 가리키는 동작 모드.
	 * 설정자: 아래 네 개의 정적 상수 정의.
	 * 읽는 자: probe.
	 * 값 범위: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 동기화: const 정적 데이터. */
	enum dw_pcie_device_mode mode;
};

/* [한국어] [상류 코드 관찰, 수정하지 않음] 매칭 테이블의 전방 선언인데, 정의(:493)보다
 * 앞서 이 이름을 쓰는 코드가 이 파일에 하나도 없다. 예전에는
 * of_match_device() 로 테이블을 직접 참조했지만 지금은
 * of_device_get_match_data() 를 쓰므로 이 선언은 필요 없어졌다. */
static const struct of_device_id artpec6_pcie_of_match[];

/* ARTPEC-6 specific registers */
/* [한국어] syscon 안의 PCIe 설정 레지스터 오프셋. 이 파일이 다루는 제어 비트 대부분이
 * 여기 모여 있다. */
#define PCIECFG				0x18
/* [한국어] bit 24 — 디버그 출력 활성화로 보인다. ARTPEC-6 PHY 초기화에서 명시적으로 끈다. 이 트리에 하드웨어 문서가 없어 비트의 정확한 의미는 확인할 수 없다. 코드가
 * 그 비트를 어떻게 쓰는지로만 설명한다. */
#define  PCIECFG_DBG_OEN		BIT(24)
/* [한국어] bit 21 — ARTPEC-6 의 코어 리셋 요청. 1 이 리셋 어서트다. */
#define  PCIECFG_CORE_RESET_REQ		BIT(21)
/* [한국어] bit 20 — LTSSM(링크 상태 기계) 활성화. 이 비트를 세워야 링크 훈련이 시작되고,
 * 지우면 링크가 내려간다. start_link/stop_link 콜백이 이 비트 하나만 토글한다. */
#define  PCIECFG_LTSSM_ENABLE		BIT(20)
/* [한국어] bit [19:16] — 장치 타입 필드. EP 모드 probe 에서 이 필드를 0 으로 지운다.
 * RC 모드에서는 건드리지 않으므로 기본값을 그대로 쓴다는 뜻이다. 이 트리에 하드웨어 문서가 없어 비트의 정확한 의미는 확인할 수 없다. 코드가
 * 그 비트를 어떻게 쓰는지로만 설명한다. */
#define  PCIECFG_DEVICE_TYPE_MASK	GENMASK(19, 16)
/* [한국어] bit 11 — CLKREQ# 관련. ARTPEC-6 PHY 초기화에서 지운다. 이 트리에 하드웨어 문서가 없어 비트의 정확한 의미는 확인할 수 없다. 코드가
 * 그 비트를 어떻게 쓰는지로만 설명한다. */
#define  PCIECFG_CLKREQ_B		BIT(11)
/* [한국어] bit 10 — 레퍼런스 클럭 활성화. ARTPEC-6 에서만 명시적으로 켠다. */
#define  PCIECFG_REFCLK_ENABLE		BIT(10)
/* [한국어] bit 9 — PLL 활성화. ARTPEC-6 의 3단계 PHY 초기화 중 세 번째에서 켠다. */
#define  PCIECFG_PLL_ENABLE		BIT(9)
/* [한국어] bit 8 — PCLK 활성화. 두 변종 모두 켜지만 켜는 시점이 다르다. */
#define  PCIECFG_PCLK_ENABLE		BIT(8)
/* [한국어] bit 4 — 수신단 종단 저항 50옴 설정(옆의 상류 주석 근거).
 * 두 변종의 PHY 초기화가 모두 이 비트를 켠다. */
#define  PCIECFG_RISRCREN		BIT(4)
/* [한국어] bit 3 — 송신 드라이버 활성화. ARTPEC-6 에서만 켠다.
 * 주의: 아래 PCIECFG_NOC_RESET 도 BIT(3) 이다 — 같은 비트를 두 변종이
 * 서로 다른 의미로 쓴다. */
#define  PCIECFG_MODE_TX_DRV_EN		BIT(3)
/* [한국어] bit 2 — 레퍼런스 클럭 종단 저항 100옴 설정(옆의 상류 주석 근거). */
#define  PCIECFG_CISRREN		BIT(2)
/* [한국어] bit 0 — PHY 매크로 전체 활성화. ARTPEC-6 초기화의 첫 단계에서 켠다. */
#define  PCIECFG_MACRO_ENABLE		BIT(0)
/* ARTPEC-7 specific fields */
/* [한국어] bit 23 — ARTPEC-7 전용. 레퍼런스 클럭 소스 선택(외부/내부).
 * PCIESTAT 에서 읽은 외부 클럭 연결 여부에 따라 켜거나 끈다. */
#define  PCIECFG_REFCLKSEL		BIT(23)
/* [한국어] bit 3 — ARTPEC-7 전용 NOC(Network-on-Chip) 리셋.
 * 주의: ARTPEC-6 의 PCIECFG_MODE_TX_DRV_EN 과 같은 비트 위치이며,
 * 게다가 극성이 반대다 — 0 이 리셋 어서트, 1 이 해제다
 * (assert_core_reset 이 AND ~, deassert 가 OR 를 쓰는 것이 근거). */
#define  PCIECFG_NOC_RESET		BIT(3)

/* [한국어] syscon 안의 PCIe 상태 레지스터 오프셋. */
#define PCIESTAT			0x1c
/* ARTPEC-7 specific fields */
/* [한국어] bit 3 — 외부 레퍼런스 클럭이 연결되어 있는지. ARTPEC-7 초기화가 이 비트를
 * 읽어 REFCLKSEL 을 어떻게 설정할지 정한다. */
#define  PCIESTAT_EXTREFCLK		BIT(3)

/* [한국어] syscon 안의 NOC(Network-on-Chip) 설정 레지스터 오프셋.
 * PCIe 블록의 클럭 게이팅과 유휴 상태 진입/이탈을 제어한다. */
#define NOCCFG				0x40
/* [한국어] bit 4 — PCIe 로 가는 NOC 클럭 활성화. 두 변종 모두 PHY 초기화 중에 켠다. */
#define  NOCCFG_ENABLE_CLK_PCIE		BIT(4)
/* [한국어] bit 3 — 유휴 요청에 대한 하드웨어의 확인(ACK). 이 비트가 서 있으면
 * 아직 유휴 상태를 벗어나지 못했다는 뜻이라, wait_for_phy 가 이 비트가
 * 내려가기를 기다린다. */
#define  NOCCFG_POWER_PCIE_IDLEACK	BIT(3)
/* [한국어] bit 2 — 현재 유휴 상태임을 나타낸다. IDLEACK 와 함께 폴링 조건에 쓰인다. */
#define  NOCCFG_POWER_PCIE_IDLE		BIT(2)
/* [한국어] bit 1 — 유휴 상태로 들어가라는 요청. PHY 초기화의 마지막 단계에서 이 비트를
 * 지워 유휴에서 벗어나게 한다. */
#define  NOCCFG_POWER_PCIE_IDLEREQ	BIT(1)

/* [한국어] PHY 레지스터 블록(phy_base) 안의 상태 레지스터 오프셋. syscon 이 아니라
 * 별도로 매핑한 MMIO 영역이라는 점에 주의한다. */
#define PHY_STATUS			0x118
/* [한국어] bit 0 — PHY 의 CosPLL 이 잠겼는지. ARTPEC-6 는 이 비트가 서기를 기다린다. */
#define  PHY_COSPLLLOCK			BIT(0)

/* [한국어] ARTPEC-7 PHY 의 송신 ASIC 출력 레지스터 오프셋. */
#define PHY_TX_ASIC_OUT			0x4040
/* [한국어] bit 0 — 송신 쪽 확인 신호. ARTPEC-7 는 이 비트가 내려가기를 기다린다
 * (6 세대와 폴링 극성이 반대다). */
#define  PHY_TX_ASIC_OUT_TX_ACK		BIT(0)

/* [한국어] ARTPEC-7 PHY 의 수신 ASIC 출력 레지스터 오프셋. */
#define PHY_RX_ASIC_OUT			0x405c
/* [한국어] bit 0 — 수신 쪽 확인 신호. TX 쪽과 함께 둘 다 내려가야 PHY 가 준비된 것이다. */
#define  PHY_RX_ASIC_OUT_ACK		BIT(0)

/* [한국어]
 * artpec6_pcie_readl - SoC 시스템 컨트롤러(syscon) 안의 레지스터를 읽는다
 *
 * @artpec6_pcie: 컨트롤러 객체. regmap 필드가 syscon 접근 통로다.
 * @offset: syscon 안에서의 레지스터 오프셋(PCIECFG / PCIESTAT / NOCCFG).
 * @return: 읽은 32비트 값.
 *
 * 이 컨트롤러의 특이점은 제어 레지스터가 PCIe IP 안이 아니라 SoC 의 공용
 * 시스템 컨트롤러 블록에 흩어져 있다는 것이다. 그래서 MMIO 를 직접 매핑하지 않고
 * DT 의 axis,syscon-pcie phandle 로 얻은 regmap 을 빌려 쓴다. regmap 은 여러
 * 드라이버가 같은 레지스터 블록을 공유할 때 접근을 직렬화해 주므로,
 * 이 파일에는 별도의 락이 없다.
 *
 * [상류 코드 관찰, 수정하지 않음] regmap_read() 의 반환값을 검사하지 않는다.
 * 읽기가 실패하면 val 이 초기화되지 않은 스택 값 그대로 반환된다.
 *
 * 실행 컨텍스트: probe 경로와 링크 시작/정지 콜백. regmap 접근이 잠들 수 있는
 * 버스(I2C 등) 뒤에 있을 수 있으나, 이 SoC 에서는 메모리 맵 regmap 이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   init_phy / wait_for_phy / assert_core_reset / establish_link 등
 *     → [artpec6_pcie_readl] → regmap_read()
 */
static u32 artpec6_pcie_readl(struct artpec6_pcie *artpec6_pcie, u32 offset)
{
	/* [한국어] regmap_read 가 값을 채워 줄 지역 변수. */
	u32 val;

	/* [한국어] syscon regmap 을 통해 레지스터를 읽는다. regmap 이 내부적으로 락을 잡으므로
	 * 이 파일에는 동기화 코드가 없다. 반환값(오류)을 검사하지 않는 점에 주의 —
	 * 실패하면 val 이 초기화되지 않은 채 반환된다. */
	regmap_read(artpec6_pcie->regmap, offset, &val);
	/* [한국어] 읽은 값을 그대로 돌려준다. */
	return val;
}

/* [한국어]
 * artpec6_pcie_writel - SoC 시스템 컨트롤러(syscon) 안의 레지스터에 쓴다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 * @offset: syscon 안에서의 레지스터 오프셋.
 * @val: 쓸 값.
 *
 * 읽기 쪽과 대칭이다. 인자 순서가 (offset, val) 로, 같은 파일의
 * artpec6_pcie_readl(pcie, offset) 과 나란히 놓았을 때 자연스럽다 —
 * 많은 드라이버가 쓰는 (val, offset) 순서와 반대라는 점에 주의한다.
 *
 * [상류 코드 관찰] 읽기 쪽과 마찬가지로 regmap_write() 의 반환값을 검사하지 않는다.
 *
 * 실행 컨텍스트: probe 경로와 링크 시작/정지 콜백.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   init_phy / assert_core_reset / establish_link 등
 *     → [artpec6_pcie_writel] → regmap_write()
 */
static void artpec6_pcie_writel(struct artpec6_pcie *artpec6_pcie, u32 offset, u32 val)
{
	/* [한국어] syscon regmap 을 통해 레지스터를 쓴다. 역시 반환값을 검사하지 않는다. */
	regmap_write(artpec6_pcie->regmap, offset, val);
}

/* [한국어]
 * artpec6_pcie_cpu_addr_fixup - CPU 물리 주소를 컨트롤러 기준 상대 주소로 바꾼다
 *
 * @pci: DWC 공용 코어 객체.
 * @cpu_addr: 변환할 CPU 물리 주소.
 * @return: 컨트롤러의 iATU 가 기대하는 상대 주소. 모드를 알 수 없으면
 *       변환하지 않은 원래 주소를 그대로 돌려준다.
 *
 * 왜 필요한가: DWC 의 iATU(주소 변환 유닛)는 CPU 물리 주소를 그대로 받지 않고,
 * 컨트롤러마다 정해진 기준점에서 잰 오프셋을 받는 경우가 있다. 그 차이를 흡수하는
 * 것이 이 콜백이며, DWC 코어는 아웃바운드 창을 설정할 때마다 이것을 부른다.
 *
 * 기준점이 모드에 따라 다르다.
 *   - RC 모드: pp->cfg0_base — config 공간의 시작.
 *   - EP 모드: ep->phys_base — 엔드포인트가 노출하는 물리 영역의 시작.
 * 같은 하드웨어라도 RC 로 쓸 때와 EP 로 쓸 때 주소 창의 배치가 달라지기 때문이다.
 *
 * [상류 코드 관찰, 수정하지 않음] default 갈래는 오류를 로그로 남기지만
 * 변환하지 않은 주소를 그대로 돌려준다. 반환형이 u64 라 오류를 표현할 방법이
 * 없어서인데, probe 가 모드를 걸러 내므로 정상 경로에서는 도달하지 않는다.
 *
 * 실행 컨텍스트: DWC 코어의 iATU 설정 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 위의 default 갈래뿐이다.
 *
 * 호출 체인:
 *   dw_pcie_prog_outbound_atu() 등 → dw_pcie_ops.cpu_addr_fixup == [이 함수]
 */
static u64 artpec6_pcie_cpu_addr_fixup(struct dw_pcie *pci, u64 cpu_addr)
{
	/* [한국어] 공용 코어 객체에서 이 파일의 컨트롤러를 되찾는다. */
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);
	/* [한국어] RC 모드에서 쓸 호스트 포트 객체. */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] EP 모드에서 쓸 엔드포인트 객체. 둘 중 하나만 유효하지만 미리 꺼내 둔다. */
	struct dw_pcie_ep *ep = &pci->ep;

	/* [한국어] 이 인스턴스가 RC 인지 EP 인지에 따라 기준 주소가 달라진다. */
	switch (artpec6_pcie->mode) {
	/* [한국어] 루트 컴플렉스 모드. */
	case DW_PCIE_RC_TYPE:
		/* [한국어] CPU 주소에서 config 영역의 시작을 빼 하드웨어가 기대하는 상대 주소로 바꾼다.
		 * DWC 의 iATU 는 CPU 물리 주소가 아니라 이 컨트롤러 고유의 기준에서 잰
		 * 오프셋을 요구하므로, 이 콜백이 그 차이를 흡수한다. */
		return cpu_addr - pp->cfg0_base;
	/* [한국어] 엔드포인트 모드. */
	case DW_PCIE_EP_TYPE:
		/* [한국어] EP 에서는 기준이 endpoint 의 물리 영역 시작이다. */
		return cpu_addr - ep->phys_base;
	/* [한국어] 모드가 둘 중 어느 것도 아닌 경우 — probe 가 걸러 내므로 정상 경로에서는 오지 않는다. */
	default:
		/* [한국어] 그래도 방어적으로 로그를 남긴다. */
		dev_err(pci->dev, "UNKNOWN device type\n");
	}
	/* [한국어] [상류 코드 관찰] default 로 빠졌을 때 변환하지 않은 원래 주소를 그대로 돌려준다.
	 * 오류를 알릴 방법이 없는 반환형(u64)이라 이렇게 처리한 것으로 보인다. */
	return cpu_addr;
}

/* [한국어]
 * artpec6_pcie_establish_link - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @pci: DWC 공용 코어 객체.
 * @return: 항상 0.
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크의 상태 기계다.
 * PCIECFG 의 bit 20 을 세우면 훈련이 시작되고, 지우면 링크가 내려간다.
 * 이 함수가 하는 일은 그 비트 하나를 켜는 읽기-수정-쓰기뿐이다.
 *
 * 항상 0 을 돌려주는 이유는 이 콜백의 계약이 "훈련을 시작시켜라" 이지
 * "링크를 성립시켜라" 가 아니기 때문이다. 성립 여부 판정과 타임아웃은
 * DWC 코어가 담당한다. 특히 이 파일은 link_up 콜백을 제공하지 않으므로,
 * 코어의 기본 구현(DWC 표준 상태 레지스터 판독)이 그 판정을 맡는다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 또는 EP 초기화 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_ops.start_link == [이 함수]
 *     → artpec6_pcie_readl/writel(PCIECFG)
 */
static int artpec6_pcie_establish_link(struct dw_pcie *pci)
{
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 val;

	/* [한국어] 현재 PCIECFG 값을 읽는다. */
	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	/* [한국어] LTSSM 활성화 비트만 켠다. */
	val |= PCIECFG_LTSSM_ENABLE;
	/* [한국어] 되쓴다. 이 쓰기로 링크 훈련이 시작된다. */
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);

	/* [한국어] 항상 0 을 돌려준다. 실제 링크 성립 판정은 DWC 코어가 담당한다.
	 * 이 파일은 link_up 콜백을 제공하지 않으므로, 코어의 기본 구현
	 * (DWC 표준 상태 레지스터 판독)이 쓰인다. */
	return 0;
}

/* [한국어]
 * artpec6_pcie_stop_link - LTSSM 을 꺼 링크를 내린다
 *
 * @pci: DWC 공용 코어 객체.
 *
 * establish_link 와 정확히 대칭으로, PCIECFG 의 LTSSM 비트를 AND ~ 로 지운다.
 * 이 쓰기 직후 링크가 내려가므로, 진행 중인 트랜잭션이 있으면 잃는다.
 * DWC 코어는 EP 의 링크 재설정이나 호스트 초기화 실패 되감기에서 이 콜백을 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   DWC 코어(링크 재설정 또는 되감기) → dw_pcie_ops.stop_link == [이 함수]
 *     → artpec6_pcie_readl/writel(PCIECFG)
 */
static void artpec6_pcie_stop_link(struct dw_pcie *pci)
{
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 val;

	/* [한국어] 현재 값을 읽고, */
	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	/* [한국어] LTSSM 비트만 지우고, */
	val &= ~PCIECFG_LTSSM_ENABLE;
	/* [한국어] 되쓴다. 링크가 즉시 내려간다. */
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] CPU 주소를 컨트롤러 기준 주소로 바꾸는 콜백. RC/EP 모드에 따라 기준이 다르다. */
	.cpu_addr_fixup = artpec6_pcie_cpu_addr_fixup,
	/* [한국어] 링크 훈련 시작 콜백. */
	.start_link = artpec6_pcie_establish_link,
	/* [한국어] 링크 정지 콜백. link_up 콜백이 없다는 점에 주목 — DWC 코어의 기본 판정을 쓴다. */
	.stop_link = artpec6_pcie_stop_link,
};

/* [한국어]
 * artpec6_pcie_wait_for_phy_a6 - ARTPEC-6 PHY 가 준비될 때까지 기다린다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 *
 * 두 단계로 나뉜 폴링이다.
 *   1) NOC 가 유휴 상태를 벗어나기를 기다린다 — NOCCFG 의 IDLEACK 와 IDLE 비트가
 *      둘 다 내려가야 한다. init_phy 가 마지막에 IDLEREQ 를 지웠으므로,
 *      하드웨어가 그 요청에 응답해 유휴에서 빠져나오는 것을 확인하는 셈이다.
 *   2) PHY 의 CosPLL 이 잠기기를 기다린다 — PHY_STATUS 의 bit 0 이 서야 한다.
 *      여기서만 syscon regmap 이 아니라 별도로 매핑한 phy_base 를 readl 로 읽는다.
 *
 * 두 루프 모두 최대 50회, 회당 1~2ms 잠들며 기다리므로 최대 100ms 씩이다.
 * usleep_range 를 쓰는 것은 이 함수가 probe 경로의 프로세스 컨텍스트에서만
 * 불리기 때문이고, 타이머 병합을 허용해 바쁜 대기보다 전력 효율이 좋다.
 * "자고 나서 읽는" 순서인 것도 의도적이다 — 하드웨어가 상태를 바꿀 시간을
 * 먼저 주는 편이 첫 판독에서 성공할 확률을 높인다.
 *
 * [상류 코드 관찰, 수정하지 않음] 탈출 조건이 만족되는 반복과 retries 가 0 이
 * 되는 반복이 겹치면, 성공했는데도 오류 로그가 찍힌다. 또 시간 초과를 오류로
 * 돌려주지 않고 로그만 남긴 채 진행하므로, PHY 가 준비되지 않았어도
 * 링크 훈련이 시작된다.
 *
 * 실행 컨텍스트: host_init / ep_init 경로, 프로세스 컨텍스트. 최대 200ms 잠든다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   artpec6_pcie_wait_for_phy() → [이 함수]
 *     → artpec6_pcie_readl(NOCCFG) / readl(phy_base + PHY_STATUS)
 */
static void artpec6_pcie_wait_for_phy_a6(struct artpec6_pcie *artpec6_pcie)
{
	/* [한국어] 로그에 쓸 공용 코어 객체. */
	struct dw_pcie *pci = artpec6_pcie->pci;
	/* [한국어] dev_err 대상. */
	struct device *dev = pci->dev;
	/* [한국어] 레지스터 값을 담을 변수. */
	u32 val;
	/* [한국어] 남은 재시도 횟수. */
	unsigned int retries;

	/* [한국어] 최대 50회 시도 = 최대 100ms 대기(회당 1~2ms). */
	retries = 50;
	/* [한국어] 먼저 자고 나서 읽는 구조다 — 하드웨어가 상태를 바꿀 시간을 먼저 준다. */
	do {
		/* [한국어] 1~2ms 잠든다. usleep_range 는 타이머 병합을 허용해 바쁜 대기보다 효율적이다. */
		usleep_range(1000, 2000);
		/* [한국어] NOC 설정 레지스터를 읽는다. */
		val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
		/* [한국어] 재시도 횟수를 줄인다. */
		retries--;
	/* [한국어] IDLEACK 나 IDLE 중 하나라도 서 있으면 아직 유휴 상태를 벗어나지 못한 것이다.
	 * 둘 다 내려가야 루프를 빠져나간다. */
	} while (retries &&
		(val & (NOCCFG_POWER_PCIE_IDLEACK | NOCCFG_POWER_PCIE_IDLE)));
	/* [한국어] [상류 코드 관찰] retries 가 0 이 되는 것과 조건이 만족되는 것이 같은 반복에서
	 * 일어나면, 성공했는데도 오류 로그가 찍힌다. 확률은 낮고 실害도 없다. */
	if (!retries)
		/* [한국어] 시간 초과를 알린다. 오류를 반환하지는 않고 계속 진행한다. */
		dev_err(dev, "PCIe clock manager did not leave idle state\n");

	/* [한국어] 두 번째 폴링 — PHY PLL 잠금 대기. 역시 최대 50회. */
	retries = 50;
	/* [한국어] 자고 나서 읽는 구조. */
	do {
		/* [한국어] 1~2ms 대기. */
		usleep_range(1000, 2000);
		/* [한국어] PHY 상태 레지스터를 읽는다. syscon 이 아니라 별도 매핑한 MMIO 이므로
		 * regmap 이 아니라 readl 을 쓴다. */
		val = readl(artpec6_pcie->phy_base + PHY_STATUS);
		/* [한국어] 재시도 감소. */
		retries--;
	/* [한국어] PLL 이 잠기면(비트가 서면) 탈출한다 — 첫 번째 루프와 극성이 반대다. */
	} while (retries && !(val & PHY_COSPLLLOCK));
	/* [한국어] 시간 초과 검사. */
	if (!retries)
		/* [한국어] PLL 이 잠기지 않았음을 알린다. 역시 반환값은 없다. */
		dev_err(dev, "PHY PLL did not lock\n");
}

/* [한국어]
 * artpec6_pcie_wait_for_phy_a7 - ARTPEC-7 PHY 가 준비될 때까지 기다린다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 *
 * 6 세대 판과 첫 단계는 완전히 같고(NOC 유휴 이탈 대기), 두 번째 단계가 다르다.
 * 6 세대는 PLL 잠금(비트가 서기)을 기다리는 반면, 7 세대는 PHY 가 저전력 상태
 * (Pn)에 들어가기를 기다린다 — TX 와 RX 두 ASIC 출력 레지스터의 ACK 비트가
 * 둘 다 내려가야 한다. 폴링 극성이 반대라는 점이 두 세대의 가장 큰 차이다.
 *
 * 또 하나의 차이는 레지스터 폭이다. 6 세대는 PHY_STATUS 를 readl(32비트)로 읽지만
 * 7 세대는 TX/RX ASIC 출력을 readw(16비트)로 읽는다. 그래서 지역 변수도
 * u32 가 아니라 u16 이다.
 *
 * 시간 제약과 usleep_range 사용, 그리고 "시간 초과를 오류로 돌려주지 않는다" 는
 * 성질은 6 세대 판과 동일하다.
 *
 * 실행 컨텍스트: host_init / ep_init 경로, 프로세스 컨텍스트. 최대 200ms 잠든다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   artpec6_pcie_wait_for_phy() → [이 함수]
 *     → artpec6_pcie_readl(NOCCFG) / readw(phy_base + PHY_TX_ASIC_OUT, PHY_RX_ASIC_OUT)
 */
static void artpec6_pcie_wait_for_phy_a7(struct artpec6_pcie *artpec6_pcie)
{
	/* [한국어] 로그에 쓸 공용 코어 객체. */
	struct dw_pcie *pci = artpec6_pcie->pci;
	/* [한국어] dev_err 대상. */
	struct device *dev = pci->dev;
	/* [한국어] NOC 레지스터 값. */
	u32 val;
	/* [한국어] TX/RX ASIC 출력 레지스터 값. 16비트 폭이라 u16 이다. */
	u16 phy_status_tx, phy_status_rx;
	/* [한국어] 남은 재시도 횟수. */
	unsigned int retries;

	/* [한국어] 첫 번째 폴링 — 6 세대와 동일한 NOC 유휴 이탈 대기. */
	retries = 50;
	/* [한국어] 자고 나서 읽는다. */
	do {
		/* [한국어] 1~2ms 대기. */
		usleep_range(1000, 2000);
		/* [한국어] NOC 설정 레지스터를 읽는다. */
		val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
		/* [한국어] 재시도 감소. */
		retries--;
	/* [한국어] 6 세대와 완전히 같은 탈출 조건이다. */
	} while (retries &&
		(val & (NOCCFG_POWER_PCIE_IDLEACK | NOCCFG_POWER_PCIE_IDLE)));
	/* [한국어] 시간 초과 검사. */
	if (!retries)
		/* [한국어] 같은 메시지를 쓴다. */
		dev_err(dev, "PCIe clock manager did not leave idle state\n");

	/* [한국어] 두 번째 폴링 — 6 세대와 달리 PLL 잠금이 아니라 PHY 의 저전력 상태(Pn) 진입을 기다린다. */
	retries = 50;
	/* [한국어] 자고 나서 읽는다. */
	do {
		/* [한국어] 1~2ms 대기. */
		usleep_range(1000, 2000);
		/* [한국어] 송신 쪽 ASIC 출력을 16비트로 읽는다. */
		phy_status_tx = readw(artpec6_pcie->phy_base + PHY_TX_ASIC_OUT);
		/* [한국어] 수신 쪽도 읽는다. */
		phy_status_rx = readw(artpec6_pcie->phy_base + PHY_RX_ASIC_OUT);
		/* [한국어] 재시도 감소. */
		retries--;
	/* [한국어] TX ACK 나 RX ACK 중 하나라도 서 있으면 아직 Pn 상태가 아니다.
	 * 둘 다 내려가야 탈출한다 — 6 세대의 PLL 잠금 대기와 극성이 반대다. */
	} while (retries && ((phy_status_tx & PHY_TX_ASIC_OUT_TX_ACK) ||
				(phy_status_rx & PHY_RX_ASIC_OUT_ACK)));
	/* [한국어] 시간 초과 검사. */
	if (!retries)
		/* [한국어] PHY 가 Pn 상태에 들어가지 못했음을 알린다. */
		dev_err(dev, "PHY did not enter Pn state\n");
}

/* [한국어]
 * artpec6_pcie_wait_for_phy - SoC 세대에 맞는 PHY 대기 루틴을 고른다
 *
 * @artpec6_pcie: 컨트롤러 객체. variant 필드로 갈린다.
 *
 * 두 세대의 PHY 준비 신호가 다르기 때문에 존재하는 분기 함수다.
 * 6 세대는 PLL 잠금을, 7 세대는 저전력 상태 진입을 기다린다.
 * switch 에 default 갈래가 없는데, enum 의 값이 둘뿐이고 probe 가 DT 매칭
 * 데이터에서만 값을 받으므로 그 밖의 값이 들어올 수 없기 때문이다.
 *
 * 실행 컨텍스트: host_init / ep_init 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   artpec6_pcie_host_init() / artpec6_pcie_ep_init() → [이 함수]
 *     → wait_for_phy_a6() 또는 wait_for_phy_a7()
 */
static void artpec6_pcie_wait_for_phy(struct artpec6_pcie *artpec6_pcie)
{
	/* [한국어] SoC 세대에 따라 대기 방식이 다르다. */
	switch (artpec6_pcie->variant) {
	/* [한국어] 6 세대: NOC 유휴 이탈 + PLL 잠금. */
	case ARTPEC6:
		artpec6_pcie_wait_for_phy_a6(artpec6_pcie);
		break;
	/* [한국어] 7 세대: NOC 유휴 이탈 + PHY Pn 상태 진입. */
	case ARTPEC7:
		artpec6_pcie_wait_for_phy_a7(artpec6_pcie);
		break;
	}
}

/* [한국어]
 * artpec6_pcie_init_phy_a6 - ARTPEC-6 PHY 를 4단계로 초기화한다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 *
 * 순서와 대기 시간이 이 함수의 전부다. 각 단계 사이의 지연은 아날로그 회로가
 * 안정되는 데 필요한 시간이며, 짧게 잡으면 다음 단계가 실패한다.
 *
 *   1) PCIECFG 에 아날로그 설정을 한 번에 반영한다 — 수신 종단 저항 50옴,
 *      송신 드라이버 활성화, 레퍼런스 클럭 종단 저항 100옴, PHY 매크로 활성화,
 *      레퍼런스 클럭 활성화를 켜고, 디버그 출력과 CLKREQ# 를 끈다.
 *      그 뒤 5~6ms 대기.
 *   2) NOCCFG 에서 PCIe 로 가는 클럭을 켠다. 클럭 게이트 해제는 빨라
 *      20~30us 만 대기한다.
 *   3) PCIECFG 에서 PCLK 과 PLL 을 켠다. 클럭이 들어온 뒤에 PLL 을 켜야 하므로
 *      2)와 순서를 바꿀 수 없다. PLL 잠금에 필요한 6~7ms 대기.
 *   4) NOCCFG 에서 유휴 요청 비트를 지워 유휴 상태에서 벗어나게 한다.
 *      이 시점부터 wait_for_phy 가 유휴 이탈과 PLL 잠금을 폴링할 수 있다.
 *
 * 총 11ms 남짓 잠든다. 7 세대 판이 50us 로 끝나는 것과 크게 대비된다.
 *
 * 실행 컨텍스트: host_init / ep_init 경로, 프로세스 컨텍스트.
 * 반드시 코어가 리셋 상태일 때 불려야 한다 — 동작 중에 PHY 설정을 바꾸면
 * 진행 중인 트랜잭션이 깨진다.
 *
 * 에러 경로: 없다. 모든 쓰기의 성공을 가정한다.
 *
 * 호출 체인:
 *   artpec6_pcie_init_phy() → [이 함수]
 *     → artpec6_pcie_readl/writel(PCIECFG, NOCCFG) / usleep_range()
 */
static void artpec6_pcie_init_phy_a6(struct artpec6_pcie *artpec6_pcie)
{
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 val;

	/* [한국어] 현재 PCIECFG 값을 읽는다. */
	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	/* [한국어] 수신 종단 저항 50옴을 켠다(옆의 상류 주석 근거). */
	val |=  PCIECFG_RISRCREN |	/* Receiver term. 50 Ohm */
		/* [한국어] 송신 드라이버를 켠다. */
		PCIECFG_MODE_TX_DRV_EN |
		/* [한국어] 레퍼런스 클럭 종단 저항 100옴을 켠다(옆의 상류 주석 근거). */
		PCIECFG_CISRREN |	/* Reference clock term. 100 Ohm */
		/* [한국어] PHY 매크로 전체를 켠다. 위 네 비트를 한 번의 OR 로 묶는다. */
		PCIECFG_MACRO_ENABLE;
	/* [한국어] 레퍼런스 클럭도 켠다. 앞의 OR 와 나눠 쓴 것은 논리적 그룹을 구분하기 위함으로 보인다. */
	val |= PCIECFG_REFCLK_ENABLE;
	/* [한국어] 디버그 출력을 끈다. */
	val &= ~PCIECFG_DBG_OEN;
	/* [한국어] CLKREQ# 를 끈다. */
	val &= ~PCIECFG_CLKREQ_B;
	/* [한국어] 1단계 설정을 한 번에 반영한다. */
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	/* [한국어] 5~6ms 기다린다. 아날로그 회로가 안정되는 데 필요한 시간이다. */
	usleep_range(5000, 6000);

	/* [한국어] NOC 설정을 읽어, */
	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	/* [한국어] PCIe 로 가는 클럭을 켜고, */
	val |= NOCCFG_ENABLE_CLK_PCIE;
	/* [한국어] 반영한다. */
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
	/* [한국어] 20~30us 기다린다. 클럭 게이트 해제는 훨씬 빨라 대기가 짧다. */
	usleep_range(20, 30);

	/* [한국어] 다시 PCIECFG 를 읽어, */
	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	/* [한국어] PCLK 과 PLL 을 켠다. 클럭이 들어온 뒤에 PLL 을 켜야 하므로 순서가 중요하다. */
	val |= PCIECFG_PCLK_ENABLE | PCIECFG_PLL_ENABLE;
	/* [한국어] 반영한다. */
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	/* [한국어] 6~7ms 기다린다. PLL 잠금에 필요한 시간이다. */
	usleep_range(6000, 7000);

	/* [한국어] 마지막으로 NOC 설정을 읽어, */
	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	/* [한국어] 유휴 요청 비트를 지워 유휴 상태에서 벗어나게 한다. */
	val &= ~NOCCFG_POWER_PCIE_IDLEREQ;
	/* [한국어] 반영한다. 이 시점부터 wait_for_phy 가 유휴 이탈을 폴링할 수 있다. */
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
}

/* [한국어]
 * artpec6_pcie_init_phy_a7 - ARTPEC-7 PHY 를 3단계로 초기화한다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 *
 * 6 세대와 두 가지가 다르다.
 * 첫째, 레퍼런스 클럭 소스를 하드웨어에 물어본다. PCIESTAT 의 EXTREFCLK 비트를
 * 읽어 외부 클럭이 연결되어 있으면 PCIECFG 의 REFCLKSEL 을 켜고, 아니면 끈다.
 * 6 세대에는 이 선택 자체가 없다.
 * 둘째, 켜는 비트가 훨씬 적다 — 수신 종단 저항과 PCLK 뿐이고, PLL·매크로·
 * 송신 드라이버는 건드리지 않는다. 7 세대 PHY 가 그 부분을 자체적으로 처리하는
 * 것으로 보이며, 그래서 대기 시간도 10~20us 로 6 세대의 5ms 보다 훨씬 짧다.
 *
 *   1) 클럭 소스 판별 후 PCIECFG 설정, 10~20us 대기.
 *   2) NOCCFG 에서 PCIe 클럭 활성화, 20~30us 대기.
 *   3) NOCCFG 에서 유휴 요청 비트 제거.
 *
 * 실행 컨텍스트: host_init / ep_init 경로, 프로세스 컨텍스트. 총 50us 남짓.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   artpec6_pcie_init_phy() → [이 함수]
 *     → artpec6_pcie_readl(PCIESTAT) / readl,writel(PCIECFG, NOCCFG)
 */
static void artpec6_pcie_init_phy_a7(struct artpec6_pcie *artpec6_pcie)
{
	struct dw_pcie *pci = artpec6_pcie->pci;
	/* [한국어] 임시 변수. */
	u32 val;
	/* [한국어] 외부 레퍼런스 클럭 연결 여부. */
	bool extrefclk;

	/* Check if external reference clock is connected */
	/* [한국어] 상태 레지스터를 읽어, */
	val = artpec6_pcie_readl(artpec6_pcie, PCIESTAT);
	/* [한국어] 외부 클럭 비트를 bool 로 정규화한다. !! 는 0/비0 을 0/1 로 바꾸는 관용구다. */
	extrefclk = !!(val & PCIESTAT_EXTREFCLK);
	/* [한국어] 어느 클럭을 쓰는지 디버그 로그로 남긴다. dev_dbg 라 기본 빌드에서는 출력되지 않는다. */
	dev_dbg(pci->dev, "Using reference clock: %s\n",
		extrefclk ? "external" : "internal");

	/* [한국어] PCIECFG 를 읽어, */
	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	/* [한국어] 수신 종단 저항 50옴과, */
	val |=  PCIECFG_RISRCREN |	/* Receiver term. 50 Ohm */
		/* [한국어] PCLK 을 켠다. 6 세대와 달리 PLL/매크로/송신 드라이버는 건드리지 않는다 —
		 * 7 세대 PHY 가 그 부분을 자체적으로 처리하기 때문으로 보인다. */
		PCIECFG_PCLK_ENABLE;
	/* [한국어] 외부 클럭이 연결되어 있으면, */
	if (extrefclk)
		/* [한국어] 클럭 소스 선택 비트를 켜고, */
		val |= PCIECFG_REFCLKSEL;
	/* [한국어] 아니면, */
	else
		/* [한국어] 지운다. 하드웨어가 알려 준 상태를 그대로 따르는 셈이다. */
		val &= ~PCIECFG_REFCLKSEL;
	/* [한국어] 반영한다. */
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	/* [한국어] 10~20us 기다린다. 6 세대의 5ms 에 비해 훨씬 짧다. */
	usleep_range(10, 20);

	/* [한국어] NOC 설정을 읽어, */
	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	/* [한국어] PCIe 클럭을 켜고, */
	val |= NOCCFG_ENABLE_CLK_PCIE;
	/* [한국어] 반영한다. */
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
	/* [한국어] 20~30us 기다린다. */
	usleep_range(20, 30);

	/* [한국어] NOC 설정을 다시 읽어, */
	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	/* [한국어] 유휴 요청을 지우고, */
	val &= ~NOCCFG_POWER_PCIE_IDLEREQ;
	/* [한국어] 반영한다. 6 세대와 달리 PLL 활성화 단계가 없어 3단계로 끝난다. */
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
}

/* [한국어]
 * artpec6_pcie_init_phy - SoC 세대에 맞는 PHY 초기화 루틴을 고른다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 *
 * wait_for_phy 와 같은 형태의 분기 함수다. 두 세대의 PHY 가 요구하는 설정
 * 순서와 대기 시간이 크게 달라 하나의 함수로 합칠 수 없다.
 *
 * 실행 컨텍스트: host_init / ep_init 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   artpec6_pcie_host_init() / artpec6_pcie_ep_init() → [이 함수]
 *     → init_phy_a6() 또는 init_phy_a7()
 */
static void artpec6_pcie_init_phy(struct artpec6_pcie *artpec6_pcie)
{
	/* [한국어] SoC 세대에 따라 초기화 절차가 다르다. */
	switch (artpec6_pcie->variant) {
	/* [한국어] 6 세대: 4단계, 총 11ms 가량. */
	case ARTPEC6:
		artpec6_pcie_init_phy_a6(artpec6_pcie);
		break;
	/* [한국어] 7 세대: 3단계, 총 50us 가량. 훨씬 짧다. */
	case ARTPEC7:
		artpec6_pcie_init_phy_a7(artpec6_pcie);
		break;
	}
}

/* [한국어]
 * artpec6_pcie_assert_core_reset - 컨트롤러 코어를 리셋 상태로 넣는다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 *
 * 두 세대가 서로 다른 비트를, 그것도 반대 극성으로 쓴다는 점이 이 함수의 핵심이다.
 *   - ARTPEC-6: PCIECFG_CORE_RESET_REQ(bit 21)를 "세워" 리셋을 건다.
 *   - ARTPEC-7: PCIECFG_NOC_RESET(bit 3)을 "지워" 리셋을 건다.
 * 게다가 7 세대가 쓰는 bit 3 은 6 세대의 PCIECFG_MODE_TX_DRV_EN 과 같은 자리다.
 * 같은 레지스터의 같은 비트가 세대에 따라 전혀 다른 의미를 갖는 셈이라,
 * 이 분기를 빠뜨리면 엉뚱한 기능을 켜거나 끄게 된다.
 *
 * 리셋을 건 뒤 대기가 없다. 리셋 유지 시간은 곧바로 이어지는 PHY 초기화
 * (6 세대 11ms, 7 세대 50us)가 자연스럽게 확보해 준다.
 *
 * 실행 컨텍스트: host_init / ep_init 경로의 첫 단계, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   artpec6_pcie_host_init() / artpec6_pcie_ep_init() → [이 함수]
 *     → artpec6_pcie_readl/writel(PCIECFG)
 */
static void artpec6_pcie_assert_core_reset(struct artpec6_pcie *artpec6_pcie)
{
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 val;

	/* [한국어] 현재 PCIECFG 를 읽는다. */
	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	/* [한국어] 세대에 따라 리셋 비트와 극성이 다르다. */
	switch (artpec6_pcie->variant) {
	/* [한국어] 6 세대. */
	case ARTPEC6:
		/* [한국어] CORE_RESET_REQ(bit 21)를 세워 리셋을 건다 — 1 이 어서트. */
		val |= PCIECFG_CORE_RESET_REQ;
		break;
	/* [한국어] 7 세대. */
	case ARTPEC7:
		/* [한국어] NOC_RESET(bit 3)을 지워 리셋을 건다 — 0 이 어서트로, 6 세대와 극성이 반대다.
		 * 게다가 이 비트는 6 세대의 MODE_TX_DRV_EN 과 같은 자리다. */
		val &= ~PCIECFG_NOC_RESET;
		break;
	}
	/* [한국어] 반영한다. 대기 없이 곧장 돌아가므로, 리셋 유지 시간은 이어지는
	 * PHY 초기화(수 밀리초)가 자연스럽게 확보해 준다. */
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
}

/* [한국어]
 * artpec6_pcie_deassert_core_reset - 컨트롤러 코어의 리셋을 푼다
 *
 * @artpec6_pcie: 컨트롤러 객체.
 *
 * assert 와 정확히 대칭이며 세대별 극성도 그대로 뒤집는다.
 *   - ARTPEC-6: CORE_RESET_REQ 를 지운다.
 *   - ARTPEC-7: NOC_RESET 을 세운다.
 *
 * assert 와 달리 100~200us 의 명시적 대기가 붙어 있다. 바로 뒤에
 * wait_for_phy 가 상태 레지스터를 폴링하는데, 리셋 해제가 하드웨어에
 * 반영되기 전에 읽으면 오래된 값을 보게 되기 때문이다.
 *
 * 실행 컨텍스트: host_init / ep_init 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   artpec6_pcie_host_init() / artpec6_pcie_ep_init() → [이 함수]
 *     → artpec6_pcie_readl/writel(PCIECFG) → usleep_range()
 */
static void artpec6_pcie_deassert_core_reset(struct artpec6_pcie *artpec6_pcie)
{
	/* [한국어] 임시 변수. */
	u32 val;

	/* [한국어] 현재 값을 읽는다. */
	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	/* [한국어] 세대별 분기. */
	switch (artpec6_pcie->variant) {
	/* [한국어] 6 세대. */
	case ARTPEC6:
		/* [한국어] CORE_RESET_REQ 를 지워 리셋을 푼다. */
		val &= ~PCIECFG_CORE_RESET_REQ;
		break;
	/* [한국어] 7 세대. */
	case ARTPEC7:
		/* [한국어] NOC_RESET 을 세워 리셋을 푼다. */
		val |= PCIECFG_NOC_RESET;
		break;
	}
	/* [한국어] 반영한다. */
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	/* [한국어] 100~200us 기다린다. assert 쪽과 달리 여기서는 명시적 대기가 필요하다 —
	 * 바로 뒤에 wait_for_phy 가 상태를 폴링하기 때문이다. */
	usleep_range(100, 200);
}

/* [한국어]
 * artpec6_pcie_host_init - RC 모드에서 하드웨어를 초기화한다(DWC 코어 콜백)
 *
 * @pp: DWC 호스트 포트 객체.
 * @return: 항상 0.
 *
 * DWC 코어가 자원 준비를 마치고 링크 훈련을 시작하기 직전에 부른다.
 * 하는 일은 두 가지다.
 *
 * 첫째, ARTPEC-7 에서만 N_FTS 를 180 으로 설정한다. FTS(Fast Training Sequence)는
 * 링크가 L0s 절전 상태에서 깨어날 때 보내는 훈련 시퀀스로, 개수가 많을수록 복원이
 * 확실하지만 지연이 커진다. n_fts 가 원소 2개 배열인 것은 DWC 가 두 레지스터에
 * 각각 다른 값을 넣을 수 있게 해 두었기 때문이며(pcie-designware.h:270 의
 * PCIE_PORT_AFR 와 :303 의 PORT_LOGIC), 여기서는 둘 다 180 으로 같다.
 *
 * 둘째, 네 단계 하드웨어 시퀀스를 순서대로 밟는다 —
 * 코어 리셋 어서트 → PHY 초기화 → 코어 리셋 해제 → PHY 준비 대기.
 * 이 순서가 핵심이다. 코어가 동작 중일 때 PHY 설정을 바꾸면 진행 중인 트랜잭션이
 * 깨지므로, 반드시 리셋 상태에서 PHY 를 만져야 한다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안, 프로세스 컨텍스트.
 * PHY 초기화와 대기에 최대 200ms 남짓 잠든다.
 *
 * 에러 경로: 없다. 하위 폴링이 시간 초과되어도 로그만 남기므로,
 * PHY 가 준비되지 않은 채로 0 을 돌려줄 수 있다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [이 함수]
 *     → assert_core_reset() → init_phy() → deassert_core_reset() → wait_for_phy()
 */
static int artpec6_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* [한국어] 호스트 포트 객체에서 공용 코어 객체를 얻는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 거기서 이 파일의 컨트롤러를 얻는다. */
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);

	/* [한국어] 7 세대만 N_FTS 를 조정한다. */
	if (artpec6_pcie->variant == ARTPEC7) {
		/* [한국어] n_fts[0] 은 PCIE_PORT_AFR 레지스터의 두 필드에 쓰인다(pcie-designware.h:270, :274). */
		pci->n_fts[0] = 180;
		/* [한국어] n_fts[1] 은 PORT_LOGIC 쪽 N_FTS 필드에 쓰인다(:303). 두 레지스터에 서로 다른
		 * 값을 넣을 수 있도록 배열이 원소 2개인데, 여기서는 같은 값 180 을 넣는다.
		 * FTS 는 L0s 에서 깨어날 때 보낼 훈련 시퀀스 개수이며, 값이 클수록 복원이
		 * 확실하지만 지연이 커진다. */
		pci->n_fts[1] = 180;
	}
	/* [한국어] 코어를 리셋 상태로 넣는다. */
	artpec6_pcie_assert_core_reset(artpec6_pcie);
	/* [한국어] 리셋 상태에서 PHY 를 초기화한다 — 이 순서가 핵심이다. 코어가 동작 중일 때
	 * PHY 설정을 바꾸면 진행 중인 트랜잭션이 깨진다. */
	artpec6_pcie_init_phy(artpec6_pcie);
	/* [한국어] PHY 설정이 끝난 뒤 코어 리셋을 푼다. */
	artpec6_pcie_deassert_core_reset(artpec6_pcie);
	/* [한국어] PHY 가 실제로 준비될 때까지 기다린다. 이 대기가 끝나야 링크 훈련을 시작해도 된다. */
	artpec6_pcie_wait_for_phy(artpec6_pcie);

	/* [한국어] 항상 0. 위 폴링들이 시간 초과되어도 로그만 남기고 성공으로 처리한다. */
	return 0;
}

static const struct dw_pcie_host_ops artpec6_pcie_host_ops = {
	/* [한국어] 호스트 초기화 콜백만 제공한다. 나머지는 DWC 공용 구현으로 충분하다. */
	.init = artpec6_pcie_host_init,
};

/* [한국어]
 * artpec6_pcie_ep_init - EP 모드에서 하드웨어를 초기화한다(DWC 코어 콜백)
 *
 * @ep: DWC 엔드포인트 객체.
 *
 * host_init 과 같은 네 단계를 밟는다 — 코어 리셋 어서트 → PHY 초기화 →
 * 코어 리셋 해제 → PHY 준비 대기. 유일한 차이는 n_fts 설정이 없다는 것이다.
 * FTS 는 링크의 L0s 복원 동작에 관한 값이라 RC 쪽에서만 조정하면 되는 것으로 보인다.
 *
 * 반환형이 void 인 것도 host_init 과 다르다. DWC 의 EP 초기화 콜백 규약이
 * 실패를 표현하지 않기 때문이며, 실제로 이 시퀀스에는 실패로 처리할 지점이 없다.
 *
 * 실행 컨텍스트: dw_pcie_ep_init() 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_ep_init() → dw_pcie_ep_ops.init == [이 함수]
 *     → assert_core_reset() → init_phy() → deassert_core_reset() → wait_for_phy()
 */
static void artpec6_pcie_ep_init(struct dw_pcie_ep *ep)
{
	/* [한국어] 엔드포인트 객체에서 공용 코어 객체를 얻는다. */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 거기서 이 파일의 컨트롤러를 얻는다. */
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);

	/* [한국어] RC 경로와 완전히 같은 네 단계를 밟는다. */
	artpec6_pcie_assert_core_reset(artpec6_pcie);
	/* [한국어] PHY 초기화. */
	artpec6_pcie_init_phy(artpec6_pcie);
	/* [한국어] 코어 리셋 해제. */
	artpec6_pcie_deassert_core_reset(artpec6_pcie);
	/* [한국어] PHY 준비 대기. RC 의 host_init 과 다른 점은 n_fts 설정이 없다는 것뿐이다. */
	artpec6_pcie_wait_for_phy(artpec6_pcie);
}

/* [한국어]
 * artpec6_pcie_raise_irq - EP 모드에서 호스트에 인터럽트를 올린다(DWC 코어 콜백)
 *
 * @ep: DWC 엔드포인트 객체.
 * @func_no: 물리 기능 번호.
 * @type: 인터럽트 종류(PCI_IRQ_INTX / PCI_IRQ_MSI / 그 밖).
 * @interrupt_num: MSI 벡터 번호(1부터 시작).
 * @return: 0 = 성공(또는 아래 관찰의 default 갈래).
 *       -EINVAL = INTx 요청 — 이 컨트롤러의 EP 모드는 INTx 를 만들 수 없다.
 *
 * 왜 INTx 를 못 만드는가: INTx 는 전용 신호선을 어서트하는 방식인데, 이 SoC 의
 * EP 구현에는 그 경로가 없다. 그래서 명확한 오류로 거절하고 로그를 남긴다.
 * MSI 는 메모리 쓰기로 표현되므로 DWC 공용 구현
 * (dw_pcie_ep_raise_msi_irq)에 그대로 위임할 수 있다.
 *
 * MSI-X 분기가 없는 것은 능력 선언과 일치한다 — artpec6_pcie_epc_features 가
 * msi_capable 만 true 로 두고 msix_capable 은 명시하지 않는다.
 *
 * [상류 코드 관찰, 수정하지 않음] default 갈래가 "UNKNOWN IRQ type" 을 로그로
 * 남기고도 0(성공)을 돌려준다. 호출자는 인터럽트가 실제로 발생했다고 믿게 된다.
 * INTx 갈래가 -EINVAL 을 돌려주는 것과 대비된다.
 *
 * 실행 컨텍스트: EPF(엔드포인트 함수) 드라이버의 인터럽트 발생 요청 경로.
 *
 * 에러 경로: INTx 요청만 오류를 돌려준다.
 *
 * 호출 체인:
 *   pci_epc_raise_irq() → dw_pcie_ep_ops.raise_irq == [이 함수]
 *     → dw_pcie_ep_raise_msi_irq()
 */
static int artpec6_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				  unsigned int type, u16 interrupt_num)
{
	/* [한국어] 로그에 쓸 공용 코어 객체. */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* [한국어] 요청받은 인터럽트 종류에 따라 분기한다. */
	switch (type) {
	/* [한국어] 레거시 INTx 요청. */
	case PCI_IRQ_INTX:
		/* [한국어] 이 컨트롤러의 EP 모드는 INTx 를 만들 수 없다. */
		dev_err(pci->dev, "EP cannot trigger INTx IRQs\n");
		/* [한국어] 명확한 오류로 거절한다. */
		return -EINVAL;
	/* [한국어] MSI 요청. */
	case PCI_IRQ_MSI:
		/* [한국어] DWC 공용 구현에 위임한다. MSI-X 는 지원하지 않으므로 별도 분기가 없다. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	/* [한국어] 그 밖의 종류(MSI-X 등). */
	default:
		/* [한국어] 알 수 없는 종류라고 로그를 남긴다. */
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	/* [한국어] [상류 코드 관찰, 수정하지 않음] default 갈래가 오류를 로그로 남기고도
	 * 0(성공)을 돌려준다. 호출자는 인터럽트가 실제로 발생했다고 믿게 된다. */
	return 0;
}

static const struct pci_epc_features artpec6_pcie_epc_features = {
	/* [한국어] DWC 계열 EP 가 공통으로 갖는 기능들을 한 번에 채우는 매크로
	 * (pcie-designware.h:649). 동적 인바운드 매핑 등이 여기 들어간다. */
	DWC_EPC_COMMON_FEATURES,
	/* [한국어] 이 컨트롤러는 MSI 를 지원한다. msix_capable 을 명시하지 않았으므로
	 * MSI-X 는 지원하지 않는다 — 위 raise_irq 에 MSI-X 분기가 없는 것과 일치한다. */
	.msi_capable = true,
};

static const struct pci_epc_features *
artpec6_pcie_get_features(struct dw_pcie_ep *ep)
{
	/* [한국어] 인스턴스와 무관하게 같은 정적 구조체를 돌려준다. 이 컨트롤러의 EP 능력은
	 * 런타임에 달라지지 않기 때문이다. */
	return &artpec6_pcie_epc_features;
}

static const struct dw_pcie_ep_ops pcie_ep_ops = {
	/* [한국어] EP 초기화 콜백. */
	.init = artpec6_pcie_ep_init,
	/* [한국어] 인터럽트 발생 콜백. */
	.raise_irq = artpec6_pcie_raise_irq,
	/* [한국어] EP 능력 조회 콜백. */
	.get_features = artpec6_pcie_get_features,
};

/* [한국어]
 * artpec6_pcie_probe - ARTPEC-6/7 PCIe 컨트롤러를 RC 또는 EP 로 초기화한다
 *
 * @pdev: DT 에서 네 개의 compatible 문자열 중 하나로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공(아래 관찰의 default 갈래 포함).
 *       -EINVAL = DT 매칭 데이터 없음. -ENOMEM = 할당 실패.
 *       -ENODEV = 해당 모드가 빌드에서 비활성화됨.
 *       그 밖의 음수 = DWC 호스트/EP 초기화 실패.
 *
 * 왜 하나의 probe 가 두 모드를 다루는가: 같은 하드웨어를 보드 설계에 따라
 * 루트 컴플렉스로도, 엔드포인트로도 쓸 수 있다. 어느 쪽인지는 DT 의 compatible
 * 문자열이 결정하며(-ep 로 끝나면 EP), 그 정보가 of_device_get_match_data() 로
 * 딸려 온다. 그래서 매칭 테이블 항목이 (세대 × 모드) = 4개다.
 *
 * 동작 과정:
 *   1) DT 매칭 데이터에서 세대와 모드를 꺼낸다. 없으면 -EINVAL.
 *   2) 컨트롤러 객체와 DWC 공용 코어 객체를 각각 devm 으로 할당한다.
 *      다른 DWC 드라이버들이 dw_pcie 를 자기 구조체에 내장하는 것과 달리 이 파일은
 *      포인터로 들고 있고, 그 때문에 to_artpec6_pcie() 가 container_of 대신
 *      drvdata 를 써야 한다.
 *   3) PHY 레지스터 블록을 매핑하고, syscon regmap 을 조회한다.
 *      제어 레지스터가 syscon 안에 있으므로 이 조회가 실패하면 아무것도 할 수 없다.
 *   4) drvdata 를 심는다 — 아래 초기화가 콜백을 통해 to_artpec6_pcie() 를 쓰므로
 *      반드시 그보다 앞서야 한다.
 *   5) 모드별 분기. RC 면 host ops 를 걸고 dw_pcie_host_init() 을 부른다.
 *      EP 면 PCIECFG 의 장치 타입 필드를 0 으로 지우고, ep ops 를 걸고,
 *      dw_pcie_ep_init() → dw_pcie_ep_init_registers() → pci_epc_init_notify()
 *      순으로 진행한다. 각 모드는 해당 CONFIG 가 켜져 있을 때만 동작한다.
 *
 * [상류 코드 관찰, 수정하지 않음] default 갈래가 "INVALID device type" 을 로그로
 * 남기지만 ret 을 설정하지 않고 마지막 return 0 으로 빠진다. 결과적으로 아무것도
 * 초기화되지 않은 채 probe 가 성공한 것으로 처리된다. 다만 mode 는 DT 매칭
 * 데이터에서만 오므로 실제로 도달하기는 어렵다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 * PHY 초기화와 대기에 최대 200ms 남짓 잠든다.
 *
 * 에러 경로: 대부분 곧장 return 한다. 명시적 되감기가 필요한 곳은 EP 경로의
 * dw_pcie_ep_init_registers() 실패 하나뿐이고, 거기서 dw_pcie_ep_deinit() 을
 * 부른다. 나머지 자원은 모두 devm 이라 드라이버 코어가 회수한다.
 * remove 콜백이 없어 언바인드 경로가 없으므로, 드라이버는
 * suppress_bind_attrs = true 로 sysfs bind/unbind 자체를 막아 둔다.
 *
 * 호출 체인:
 *   DT 매칭 → platform_driver.probe == [artpec6_pcie_probe]
 *     → of_device_get_match_data() / devm_platform_ioremap_resource_byname()
 *     → syscon_regmap_lookup_by_phandle()
 *     → (RC) dw_pcie_host_init() → artpec6_pcie_host_init()
 *     → (EP) dw_pcie_ep_init() → artpec6_pcie_ep_init()
 *     → dw_pcie_ep_init_registers() → pci_epc_init_notify()
 */
static int artpec6_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] DWC 공용 코어 객체. */
	struct dw_pcie *pci;
	/* [한국어] 이 파일의 컨트롤러 객체. */
	struct artpec6_pcie *artpec6_pcie;
	/* [한국어] 각 단계의 반환값. */
	int ret;
	/* [한국어] DT 매칭 데이터. */
	const struct artpec_pcie_of_data *data;
	/* [한국어] SoC 세대. */
	enum artpec_pcie_variants variant;
	/* [한국어] 동작 모드. */
	enum dw_pcie_device_mode mode;
	/* [한국어] EP 모드에서 레지스터를 손볼 때 쓸 임시 변수. */
	u32 val;

	/* [한국어] DT 매칭에서 딸려 온 데이터를 꺼낸다. 네 개의 compatible 문자열이 각각
	 * (세대, 모드) 조합을 가리킨다. */
	data = of_device_get_match_data(dev);
	/* [한국어] 데이터가 없다면 DT 항목이 잘못된 것이다. */
	if (!data)
		/* [한국어] -EINVAL 로 거절한다. */
		return -EINVAL;

	/* [한국어] 세대를 꺼낸다. 이미 같은 타입인데도 캐스팅을 적어 둔 것은 예전 코드에서
	 * 정수로 저장하던 흔적으로 보인다. */
	variant = (enum artpec_pcie_variants)data->variant;
	/* [한국어] 모드를 꺼낸다. */
	mode = (enum dw_pcie_device_mode)data->mode;

	/* [한국어] 컨트롤러 객체를 0 초기화 할당한다. */
	artpec6_pcie = devm_kzalloc(dev, sizeof(*artpec6_pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!artpec6_pcie)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 공용 코어 객체를 별도로 할당한다. 다른 DWC 드라이버들이 자기 구조체에
	 * 내장하는 것과 다른 점이며, 그래서 to_artpec6_pcie 가 drvdata 를 써야 한다. */
	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!pci)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] DWC 코어가 로그와 자원 관리에 쓸 디바이스. */
	pci->dev = dev;
	/* [한국어] cpu_addr_fixup/start_link/stop_link 콜백 테이블 연결. */
	pci->ops = &dw_pcie_ops;

	/* [한국어] 양방향 연결의 한쪽 — 컨트롤러가 공용 코어를 가리킨다. */
	artpec6_pcie->pci = pci;
	/* [한국어] 세대 기록. */
	artpec6_pcie->variant = variant;
	/* [한국어] 모드 기록. */
	artpec6_pcie->mode = mode;

	/* [한국어] PHY 레지스터 블록을 매핑한다. syscon 과 별개의 MMIO 영역이다. */
	artpec6_pcie->phy_base =
		devm_platform_ioremap_resource_byname(pdev, "phy");
	/* [한국어] 매핑 실패 검사. */
	if (IS_ERR(artpec6_pcie->phy_base))
		/* [한국어] 오류 전달. */
		return PTR_ERR(artpec6_pcie->phy_base);

	/* [한국어] DT 의 axis,syscon-pcie phandle 이 가리키는 시스템 컨트롤러의 regmap 을 얻는다.
	 * PCIECFG/PCIESTAT/NOCCFG 가 그 안에 있으므로, 이 조회가 실패하면
	 * 컨트롤러를 전혀 제어할 수 없다. */
	artpec6_pcie->regmap =
		syscon_regmap_lookup_by_phandle(dev->of_node,
						"axis,syscon-pcie");
	/* [한국어] 조회 실패 검사. */
	if (IS_ERR(artpec6_pcie->regmap))
		/* [한국어] 오류 전달. */
		return PTR_ERR(artpec6_pcie->regmap);

	/* [한국어] to_artpec6_pcie() 매크로가 되찾을 수 있도록 컨트롤러를 심는다.
	 * 아래 dw_pcie_host_init()/dw_pcie_ep_init() 이 콜백을 통해 그 매크로를 쓰므로,
	 * 이 대입이 반드시 그보다 앞서야 한다. */
	platform_set_drvdata(pdev, artpec6_pcie);

	/* [한국어] RC 인지 EP 인지에 따라 초기화 경로가 갈린다. */
	switch (artpec6_pcie->mode) {
	/* [한국어] 루트 컴플렉스 모드. */
	case DW_PCIE_RC_TYPE:
		/* [한국어] 빌드 설정에서 RC 지원이 꺼져 있으면, */
		if (!IS_ENABLED(CONFIG_PCIE_ARTPEC6_HOST))
			/* [한국어] 장치가 없는 것으로 처리한다. 한 소스가 두 모드를 모두 담고 있어
			 * 런타임 검사와 빌드 시 검사가 함께 필요하다. */
			return -ENODEV;

		/* [한국어] 호스트 초기화 콜백 테이블 연결. */
		pci->pp.ops = &artpec6_pcie_host_ops;

		/* [한국어] DWC 공용 호스트 초기화 — 여기서 host_init 콜백이 되불리고, 링크 훈련과
		 * 버스 스캔이 이루어진다. */
		ret = dw_pcie_host_init(&pci->pp);
		/* [한국어] 실패 검사. */
		if (ret < 0)
			/* [한국어] 오류 전달. devm 자원뿐이라 정리할 것이 없다. */
			return ret;
		break;
	/* [한국어] 엔드포인트 모드. */
	case DW_PCIE_EP_TYPE:
		/* [한국어] 빌드 설정에서 EP 지원이 꺼져 있으면, */
		if (!IS_ENABLED(CONFIG_PCIE_ARTPEC6_EP))
			/* [한국어] 장치가 없는 것으로 처리한다. */
			return -ENODEV;

		/* [한국어] PCIECFG 를 읽어, */
		val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
		/* [한국어] 장치 타입 필드를 0 으로 지우고, */
		val &= ~PCIECFG_DEVICE_TYPE_MASK;
		/* [한국어] 반영한다. RC 모드에서는 이 조작이 없으므로, 기본값이 RC 이고 EP 로 쓰려면
		 * 명시적으로 0 을 써야 하는 하드웨어로 보인다. */
		artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);

		/* [한국어] EP 콜백 테이블 연결. */
		pci->ep.ops = &pcie_ep_ops;

		/* [한국어] DWC 공용 EP 초기화 — EPC(엔드포인트 컨트롤러) 객체를 만들고 등록한다. */
		ret = dw_pcie_ep_init(&pci->ep);
		/* [한국어] 실패 검사. */
		if (ret)
			/* [한국어] 오류 전달. */
			return ret;

		/* [한국어] EP 레지스터 초기화. init 과 분리된 이유는 EPC 등록 이후에 해야 하는 작업이기 때문이다. */
		ret = dw_pcie_ep_init_registers(&pci->ep);
		/* [한국어] 실패 검사. */
		if (ret) {
			/* [한국어] 실패 로그. */
			dev_err(dev, "Failed to initialize DWC endpoint registers\n");
			/* [한국어] 앞 단계의 dw_pcie_ep_init() 을 되돌린다 — 이 파일에서 유일하게 명시적
			 * 되감기가 필요한 지점이다. */
			dw_pcie_ep_deinit(&pci->ep);
			/* [한국어] 오류 전달. */
			return ret;
		}

		/* [한국어] EPC 코어에 "이제 준비됐다"고 알린다. 이 통지를 받은 EPF(엔드포인트 함수)
		 * 드라이버들이 바인딩을 시작한다. */
		pci_epc_init_notify(pci->ep.epc);

		break;
	default:
		/* [한국어] [상류 코드 관찰, 수정하지 않음] 모드가 RC 도 EP 도 아니면 오류를 로그로
		 * 남기지만 ret 을 설정하지 않고 아래에서 0(성공)을 돌려준다. 결과적으로
		 * 아무것도 초기화되지 않은 채 probe 가 성공한 것으로 처리된다. */
		dev_err(dev, "INVALID device type %d\n", artpec6_pcie->mode);
	}

	/* [한국어] 정상 경로와 default 경로가 함께 0 을 돌려준다. */
	return 0;
}

static const struct artpec_pcie_of_data artpec6_pcie_rc_of_data = {
	/* [한국어] ARTPEC-6, 루트 컴플렉스. */
	.variant = ARTPEC6,
	/* [한국어] 모드 지정. */
	.mode = DW_PCIE_RC_TYPE,
};

static const struct artpec_pcie_of_data artpec6_pcie_ep_of_data = {
	/* [한국어] ARTPEC-6, 엔드포인트. */
	.variant = ARTPEC6,
	/* [한국어] 모드 지정. */
	.mode = DW_PCIE_EP_TYPE,
};

static const struct artpec_pcie_of_data artpec7_pcie_rc_of_data = {
	/* [한국어] ARTPEC-7, 루트 컴플렉스. */
	.variant = ARTPEC7,
	/* [한국어] 모드 지정. */
	.mode = DW_PCIE_RC_TYPE,
};

static const struct artpec_pcie_of_data artpec7_pcie_ep_of_data = {
	/* [한국어] ARTPEC-7, 엔드포인트. */
	.variant = ARTPEC7,
	/* [한국어] 모드 지정. */
	.mode = DW_PCIE_EP_TYPE,
};

static const struct of_device_id artpec6_pcie_of_match[] = {
	{
		/* [한국어] DT compatible 문자열 — ARTPEC-6 RC. */
		.compatible = "axis,artpec6-pcie",
		/* [한국어] 위에서 정의한 (세대, 모드) 쌍을 연결한다. */
		.data = &artpec6_pcie_rc_of_data,
	},
	{
		/* [한국어] ARTPEC-6 EP. compatible 이 -ep 로 끝나는 것이 모드를 가르는 유일한 표시다. */
		.compatible = "axis,artpec6-pcie-ep",
		/* [한국어] 해당 데이터 연결. */
		.data = &artpec6_pcie_ep_of_data,
	},
	{
		/* [한국어] ARTPEC-7 RC. */
		.compatible = "axis,artpec7-pcie",
		/* [한국어] 해당 데이터 연결. */
		.data = &artpec7_pcie_rc_of_data,
	},
	{
		/* [한국어] ARTPEC-7 EP. */
		.compatible = "axis,artpec7-pcie-ep",
		/* [한국어] 해당 데이터 연결. */
		.data = &artpec7_pcie_ep_of_data,
	},
	/* [한국어] 테이블 끝을 알리는 빈 항목.
	 * [상류 코드 관찰] MODULE_DEVICE_TABLE 이 없다 — 아래 builtin_platform_driver 로
	 * 커널에 내장되므로 모듈 자동 로딩 정보가 필요 없기 때문이다. */
	{},
};

static struct platform_driver artpec6_pcie_driver = {
	/* [한국어] 장치가 나타났을 때 불릴 진입점. remove 콜백은 없다. */
	.probe = artpec6_pcie_probe,
	.driver = {
		/* [한국어] 드라이버 이름. */
		.name	= "artpec6-pcie",
		/* [한국어] 위에서 정의한 DT 매칭 테이블. */
		.of_match_table = artpec6_pcie_of_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다. remove 콜백이 없어 언바인드가
		 * 안전하지 않으므로, 아예 그 경로를 막아 두는 것이다 —
		 * remove 없는 드라이버가 취해야 할 올바른 조치다. */
		.suppress_bind_attrs = true,
	},
};
/* [한국어] module_platform_driver 가 아니라 builtin_ 판이다. 이 드라이버는 커널에
 * 내장되며 모듈로 빌드되지 않는다. 그래서 MODULE_LICENSE 등 모듈 메타데이터도
 * 이 파일에는 없다. */
builtin_platform_driver(artpec6_pcie_driver);
