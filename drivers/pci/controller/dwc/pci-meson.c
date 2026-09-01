// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Amlogic MESON SoCs
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Yue Wang <yue.wang@amlogic.com>
 */

/* [한국어] clk_get/clk_set_rate/clk_prepare_enable 등 클럭 프레임워크 API.
 * 이 컨트롤러는 port/general/pclk 세 개의 클럭을 쓴다. */
/*
 * [한국어 설명] Amlogic MESON SoC 의 DesignWare PCIe 결합 계층 (pci-meson.c)
 *
 * === 파일의 역할 ===
 * Amlogic AXG / G12A SoC 안에 들어 있는 PCIe 루트 컴플렉스를 초기화하고
 * 리눅스 PCI 서브시스템에 올리는 드라이버다. 컨트롤러 IP 자체는 Synopsys
 * DesignWare(DWC) PCIe 이므로 링크 관리·iATU 주소 변환·버스 스캔 같은 공통
 * 로직은 전부 이웃 파일 pcie-designware*.c 가 담당하고, 이 파일은 그 위에
 * Amlogic 고유 부분만 얹는다. 고유 부분은 넷이다.
 * (1) 자원 준비 — PHY, PERST# GPIO, port/apb 두 리셋, 세 개의 클럭,
 * 그리고 DWC 표준 DBI 창과 별개인 Amlogic 전용 config 레지스터 블록.
 * (2) 리셋과 링크 시작 시퀀스 — PHY 리셋 → 컨트롤러 리셋 펄스 → LTSSM 활성화
 * → PERST# 펄스 순서로, 각 단계가 앞 단계에 의존한다.
 * (3) 링크 상태 판정 — DWC 표준 레지스터가 아니라 Amlogic 전용 상태
 * 레지스터의 SMLH/RDLH 두 비트를 본다.
 * (4) 하드웨어 결함 우회 — AXG 컨트롤러는 소프트웨어가 class code 를 쓸 수
 * 없어서, config 읽기를 가로채 브리지 class code(0x060400)를 지어내 돌려준다.
 * 이것이 없으면 PCI 코어가 루트 포트를 브리지로 인식하지 못해 하위 버스를
 * 통째로 열거하지 않는다. 여기에 더해 MPS/MRRS 를 256바이트로 고정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층으로 보면 PCI 코어 → DWC 공용 코어(pcie-designware-host.c) → 이 파일 →
 * SoC 하드웨어다. DWC 계열 드라이버의 전형적 구조로, struct meson_pcie 가
 * struct dw_pcie 를 첫 멤버로 품고 세 개의 콜백 테이블(dw_pcie_ops,
 * dw_pcie_host_ops, pci_ops)로 공용 코어와 맞물린다.
 * 진입 경로는 넷이다. (1) 부팅 시 DT 매칭으로 meson_pcie_probe() 가 불려
 * 자원 준비를 마치고 dw_pcie_host_init() 에 제어를 넘긴다. (2) 그 안에서 코어가
 * meson_pcie_host_init() 을 되불러 config ops 교체와 MPS/MRRS 고정이 이루어진다.
 * (3) 이어서 meson_pcie_start_link() 로 링크 훈련을 시작시키고,
 * meson_pcie_link_up() 을 반복 호출하며 성립을 기다린다. (4) 버스 스캔이
 * 시작되면 config 읽기마다 meson_pcie_rd_own_conf() 가 불린다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. probe 경로에 udelay 로 1.5ms 가량의
 * 바쁜 대기가 들어가고, config 접근 경로는 전역 pci_lock 아래에서 실행된다.
 * struct dw_pcie 에서 이 파일의 객체로 돌아오는 방법이 container_of 가 아니라
 * drvdata 라는 점(to_meson_pcie 매크로)이 특징이며, 그래서 probe 에서
 * platform_set_drvdata() 가 dw_pcie_host_init() 보다 앞서야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-designware.h 의 struct dw_pcie / dw_pcie_rp / dw_pcie_ops /
 * dw_pcie_host_ops 와 dw_pcie_host_init(), dw_pcie_readl_dbi()/writel_dbi(),
 * dw_pcie_find_capability(), dw_pcie_own_conf_map_bus().
 * 그리고 linux/pci.h 의 pci_ops, pci_generic_config_read/write,
 * PCI_CLASS_BRIDGE_PCI_NORMAL, PCI_EXP_DEVCTL 계열 상수.
 * 아래쪽: 네 개의 프레임워크에 각각 의존한다 — PHY(init/power_on/reset/
 * power_off/exit), 리셋(전용 port + 공유 apb), 클럭(port 100MHz / general / pclk),
 * GPIO(PERST#). 리셋과 클럭의 처리 방식이 대조적이다. 클럭은 devm 액션으로
 * 해제를 자동화해 오류 경로에 되감기 코드가 전혀 없는 반면, 리셋은
 * meson_pcie_get_resets() 가 획득 직후 해제까지 해 버려 실패 시 상태가 남는다.
 * 데이터 흐름: DT 서술 → struct meson_pcie(클럭·리셋·PHY·GPIO·두 개의 레지스터
 * 창) → 리셋과 링크 시퀀스 → DWC 코어의 iATU/버스 스캔 → config 접근이 다시
 * 이 파일의 rd_own_conf 로 돌아와 class code 가 교정된다.
 * 공유 상태: mp->cfg_base(Amlogic 전용 레지스터 창)와 DWC 코어가 관리하는
 * DBI 창. 둘 다 probe 경로와 링크 폴링에서만 접근하며 이 파일에는 락이 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct meson_pcie: 컨트롤러 하나. 첫 멤버가 struct dw_pcie 이고, 그 뒤에
 *   Amlogic 전용 config 창(cfg_base), 클럭 셋, 리셋 둘, PERST# GPIO, PHY 가 온다.
 * - meson_pcie_probe(): 진입점. 자원 준비 아홉 단계를 밟고 dw_pcie_host_init()
 *   에 제어를 넘긴다. 정리가 필요한 자원이 PHY 전원 하나뿐이라 라벨도 하나다.
 * - meson_pcie_rd_own_conf(): 이 파일에서 가장 특이한 함수. AXG 하드웨어 결함
 *   때문에 config 읽기를 가로채 class code 를 지어낸다. 1~2바이트 읽기에서는
 *   값을 워드 위치로 되돌렸다가 조작 후 다시 잘라 내는 시프트 왕복이 들어간다.
 * - meson_pcie_start_link() / meson_pcie_link_up(): 링크 훈련 시작과 판정.
 *   시작은 LTSSM 활성화 → PERST# 펄스 순서이고, 판정은 SMLH 와 RDLH 두 비트가
 *   모두 서야 참이다.
 * - meson_size_to_payload(): 바이트 크기를 PCIe 의 2^(값+7) 3비트 인코딩으로
 *   바꾼다. fls(size) - 8 이 그 역함수다.
 * - meson_pcie_probe_clock(): 클럭 하나에 대해 얻기·주파수 설정·켜기·해제 등록
 *   네 단계를 묶는다. devm_add_action_or_reset() 덕분에 호출부에 되감기가 없다.
 * - [상류 코드 관찰, 수정하지 않음] 네 가지를 코드 수정 없이 기록해 둔다.
 *   (a) meson_pcie_get_resets() 는 "get" 이라는 이름과 달리 해제까지 하며,
 *   apb 획득이 실패하면 port 리셋이 풀린 채 남는다.
 *   (b) meson_pcie_assert_reset() 은 이름과 달리 어서트가 아니라 펄스를 만들고,
 *   대기 시간에 PCIE_RESET_DELAY 대신 숫자 500 을 직접 적었다.
 *   (c) MPS/MRRS 설정이 한 번이면 될 읽기-수정-쓰기를 두 번에 나눠 한다.
 *   (d) remove 콜백이 없다 — 모듈로 빌드될 수 있는데도 언바인드 경로가 없어,
 *   sysfs 로 언바인드하면 dw_pcie_host_init() 이 만든 버스가 등록된 채 남는다.
 *   또 enum pcie_data_rate, IS_LTSSM_UP, PCIE_CFG_STATUS17, PM_CURRENT_STATE 는
 *   정의만 있고 참조하는 곳이 없는 죽은 정의다.
 */

#include <linux/clk.h>
/* [한국어] udelay() 선언. 리셋 유지 시간과 PERST# 펄스 폭을 만드는 데 쓴다. */
#include <linux/delay.h>
/* [한국어] gpiod_set_value_cansleep() 등 GPIO 컨슈머 API. PERST#(엔드포인트 리셋)가
 * GPIO 핀에 연결되어 있어 소프트웨어가 직접 토글해야 한다. */
#include <linux/gpio/consumer.h>
/* [한국어] PCI 코어 공개 API — pci_ops, pci_generic_config_read/write,
 * PCI_CLASS_REVISION, PCI_CLASS_BRIDGE_PCI_NORMAL, PCI_EXP_DEVCTL 등. */
#include <linux/pci.h>
/* [한국어] platform_driver / platform_get_resource_byname / platform_set_drvdata.
 * 이 컨트롤러는 SoC 내부 블록이라 플랫폼 드라이버로 등록된다. */
#include <linux/platform_device.h>
/* [한국어] reset_control_assert/deassert 와 devm_reset_control_get 계열.
 * port(전용)와 apb(공유) 두 리셋 라인을 다룬다. */
#include <linux/reset.h>
/* [한국어] struct resource 정의. platform_get_resource_byname() 의 반환 타입이다. */
#include <linux/resource.h>
/* [한국어] u32/u16 등 고정폭 정수 타입. */
#include <linux/types.h>
/* [한국어] PHY 프레임워크 API — phy_init/phy_power_on/phy_reset/phy_power_off/phy_exit.
 * Amlogic 의 PCIe SerDes 는 별도 PHY 드라이버가 관리한다. */
#include <linux/phy/phy.h>
/* [한국어] of_device_id 구조체 정의. DT 매칭 테이블에 필요하다. */
#include <linux/mod_devicetable.h>
/* [한국어] MODULE_* 매크로와 module_platform_driver(). */
#include <linux/module.h>

/* [한국어] DesignWare PCIe 공용 코어 헤더(같은 디렉토리). struct dw_pcie, dw_pcie_rp,
 * dw_pcie_ops, dw_pcie_host_ops 와 dw_pcie_readl_dbi()/dw_pcie_writel_dbi(),
 * dw_pcie_find_capability(), dw_pcie_own_conf_map_bus(), dw_pcie_host_init() 이
 * 모두 여기서 온다. 이 파일은 그 공용 코어의 Amlogic 전용 결합 계층이다. */
#include "pcie-designware.h"

/* [한국어] struct dw_pcie 포인터에서 이 파일의 struct meson_pcie 를 되찾는 매크로.
 * container_of 가 아니라 drvdata 를 쓰는 이유는 probe 에서
 * platform_set_drvdata(pdev, mp) 로 심어 두었기 때문이다.
 * 주의: 그 대입은 probe 의 뒷부분(:439)에 있으므로, 그보다 앞서 이 매크로를
 * 쓰면 NULL 이 나온다. 실제 사용처인 link_up/start_link 콜백은 모두
 * dw_pcie_host_init() 이후에 불리므로 안전하다. */
#define to_meson_pcie(x) dev_get_drvdata((x)->dev)

/* [한국어] PCIe Device Control 레지스터에서 Max Payload Size 필드([7:5])에 값을 놓는 매크로.
 * PCIe 규격상 이 3비트 필드는 2^(값+7) 바이트를 뜻한다(0=128B, 1=256B, ...). */
#define PCIE_CAP_MAX_PAYLOAD_SIZE(x)	((x) << 5)
/* [한국어] 같은 레지스터의 Max Read Request Size 필드([14:12])에 값을 놓는 매크로.
 * 인코딩 규칙은 Max Payload Size 와 동일하다. */
#define PCIE_CAP_MAX_READ_REQ_SIZE(x)	((x) << 12)

/* PCIe specific config registers */
/* [한국어] Amlogic 전용 config 레지스터 블록(cfg_base)의 오프셋 0. LTSSM 활성화 비트를 담는다. */
#define PCIE_CFG0			0x0
/* [한국어] 그 레지스터의 bit 7 — 1 로 세우면 LTSSM(Link Training and Status State Machine)이
 * 동작을 시작해 링크 훈련에 들어간다. 이 비트를 세우기 전에는 링크가 절대 올라오지 않는다. */
#define APP_LTSSM_ENABLE		BIT(7)

/* [한국어] 링크 상태를 담은 상태 레지스터 12번의 오프셋. */
#define PCIE_CFG_STATUS12		0x30
/* [한국어] bit 6 — SMLH(Standard Media Layer Handler) 링크 업. PHY/물리 계층이 준비됐다는 뜻이다. */
#define IS_SMLH_LINK_UP(x)		((x) & (1 << 6))
/* [한국어] bit 16 — RDLH(Receive Data Link Handler) 링크 업. 데이터 링크 계층까지 올라왔다는 뜻이다.
 * 두 비트가 모두 서야 진짜 링크 업으로 판정한다. */
#define IS_RDLH_LINK_UP(x)		((x) & (1 << 16))
/* [한국어] bit [14:10] 이 0x11 이면 LTSSM 이 L0(정상 동작) 상태라는 뜻이다.
 * [상류 코드 관찰, 수정하지 않음] 이 파일 안에서 참조하는 곳이 하나도 없다
 * (정의 자체가 유일한 등장이다). 하드웨어 문서를 옮겨 두었거나 예전 코드의
 * 잔재로 보이며, 지금은 죽은 정의다. */
#define IS_LTSSM_UP(x)			((((x) >> 10) & 0x1f) == 0x11)

/* [한국어] 상태 레지스터 17번의 오프셋.
 * [상류 코드 관찰, 수정하지 않음] 이 파일 안에서 참조하는 곳이 하나도 없다
 * (정의 자체가 유일한 등장이다). 하드웨어 문서를 옮겨 두었거나 예전 코드의
 * 잔재로 보이며, 지금은 죽은 정의다. */
#define PCIE_CFG_STATUS17		0x44
/* [한국어] 그 레지스터의 bit 7 — 현재 전원 관리 상태.
 * [상류 코드 관찰, 수정하지 않음] 이 파일 안에서 참조하는 곳이 하나도 없다
 * (정의 자체가 유일한 등장이다). 하드웨어 문서를 옮겨 두었거나 예전 코드의
 * 잔재로 보이며, 지금은 죽은 정의다. */
#define PM_CURRENT_STATE(x)		(((x) >> 7) & 0x1)

/* [한국어] port 클럭에 설정할 주파수(100MHz). PCIe 레퍼런스 클럭 규격값이다. */
#define PORT_CLK_RATE			100000000UL
/* [한국어] 이 드라이버가 강제할 Max Payload Size(바이트). DWC 코어가 정하는 값을
 * 덮어쓰기 위해 host_init 에서 명시적으로 설정한다. */
#define MAX_PAYLOAD_SIZE		256
/* [한국어] 이 드라이버가 강제할 Max Read Request Size(바이트). */
#define MAX_READ_REQ_SIZE		256
/* [한국어] 리셋 유지/해제 후 대기 시간(마이크로초). udelay 로 바쁜 대기를 하므로
 * 500us 동안 CPU 를 점유한다. probe 경로라 허용되는 비용이다. */
#define PCIE_RESET_DELAY		500
/* [한국어] meson_pcie_get_reset() 에 넘길 값 — 이 리셋 라인을 다른 장치와 공유한다는 뜻.
 * 공유 리셋은 모든 사용자가 assert 해야 실제로 걸리고, 하나라도 deassert 하면 풀린다. */
#define PCIE_SHARED_RESET		1
/* [한국어] 전용(배타적) 리셋이라는 뜻. 이 드라이버만 그 라인을 제어한다. */
#define PCIE_NORMAL_RESET		0

/* [한국어] PCIe 링크 속도 세대를 나타내는 열거형.
 * [상류 코드 관찰, 수정하지 않음] 이 파일 안에서 참조하는 곳이 하나도 없다
 * (정의 자체가 유일한 등장이다). 하드웨어 문서를 옮겨 두었거나 예전 코드의
 * 잔재로 보이며, 지금은 죽은 정의다. */
enum pcie_data_rate {
	/* [한국어] Gen1(2.5 GT/s). */
	PCIE_GEN1,
	/* [한국어] Gen2(5 GT/s). */
	PCIE_GEN2,
	/* [한국어] Gen3(8 GT/s). */
	PCIE_GEN3,
	/* [한국어] Gen4(16 GT/s). */
	PCIE_GEN4
};

struct meson_pcie_clk_res {
	/* [한국어] pclk — APB 버스 인터페이스 클럭.
	 * 설정자: meson_pcie_probe_clocks() 가 meson_pcie_probe_clock(dev, "pclk", 0) 으로 얻는다.
	 * 읽는 자: 없다. 얻는 즉시 활성화되고 devm 액션이 해제를 맡으므로,
	 *   이 필드는 사실상 보관용이다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: 클럭 프레임워크 내부에서 처리한다. */
	struct clk *clk;
	/* [한국어] port 클럭 — PCIe 레퍼런스 클럭. 유일하게 주파수를 명시적으로 100MHz 로 설정한다.
	 * 설정자: meson_pcie_probe_clocks().
	 * 읽는 자: 없다(위 clk 과 같은 이유).
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: 클럭 프레임워크가 처리한다. */
	struct clk *port_clk;
	/* [한국어] general 클럭 — 컨트롤러 코어 클럭.
	 * 설정자: meson_pcie_probe_clocks().
	 * 읽는 자: 없다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: 클럭 프레임워크가 처리한다. */
	struct clk *general_clk;
};

struct meson_pcie_rc_reset {
	/* [한국어] PCIe 포트 전용 리셋. devm_reset_control_get() 으로 배타적으로 얻는다.
	 * 설정자: meson_pcie_get_resets() 가 얻은 직후 곧바로 deassert 까지 한다.
	 * 읽는 자: meson_pcie_reset() 이 assert -> 대기 -> deassert 순서로 펄스를 만든다.
	 * 값 범위: 유효 포인터. 오류면 get_resets 가 곧장 실패한다.
	 * 동기화: 리셋 프레임워크가 처리한다. */
	struct reset_control *port;
	/* [한국어] APB 인터페이스 리셋. devm_reset_control_get_shared() 로 얻는다 —
	 * 같은 APB 리셋 라인을 다른 SoC 블록과 공유하기 때문이다.
	 * 설정자: meson_pcie_get_resets() 가 얻은 직후 deassert.
	 * 읽는 자: meson_pcie_reset().
	 * 값 범위: 유효 포인터.
	 * 동기화: 공유 리셋이므로 프레임워크가 참조 계수로 관리한다 —
	 *   모든 사용자가 assert 해야 실제로 걸린다. */
	struct reset_control *apb;
};

struct meson_pcie {
	/* [한국어] DesignWare 공용 코어 객체. 이 파일의 구조체 맨 앞에 두어
	 * &mp->pci 와 mp 가 같은 주소가 되게 했지만, 실제 역변환은 container_of 가 아니라
	 * drvdata 를 쓰는 to_meson_pcie() 매크로로 한다.
	 * 설정자: meson_pcie_probe() 가 dev/ops/pp.ops/num_lanes 를 채운다.
	 * 읽는 자: DWC 공용 코어 전체와 이 파일의 콜백들.
	 * 값 범위: 항상 유효(구조체 내장이라 NULL 불가).
	 * 동기화: DWC 코어가 관리한다. */
	struct dw_pcie pci;
	/* [한국어] Amlogic 전용 config 레지스터 블록(DT 의 "cfg" 자원)의 가상 주소.
	 * DWC 표준 DBI 창과는 별개의, SoC 벤더가 추가한 레지스터 영역이다.
	 * 설정자: meson_pcie_get_mems() 가 devm_platform_ioremap_resource_byname 으로 채운다.
	 * 읽는 자: meson_cfg_readl()/meson_cfg_writel() 만이 역참조한다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: probe 경로와 link_up 폴링에서만 접근하며 락이 없다. */
	void __iomem *cfg_base;
	/* [한국어] 세 개의 클럭을 묶은 하위 구조체.
	 * 설정자: meson_pcie_probe_clocks().
	 * 읽는 자: 없다 — 클럭 해제는 devm 액션이 담당한다.
	 * 값 범위: 구조체 내장.
	 * 동기화: 필요 없다. */
	struct meson_pcie_clk_res clk_res;
	/* [한국어] 두 개의 리셋 컨트롤을 묶은 하위 구조체.
	 * 설정자: meson_pcie_get_resets().
	 * 읽는 자: meson_pcie_reset().
	 * 값 범위: 구조체 내장.
	 * 동기화: 리셋 프레임워크가 처리한다. */
	struct meson_pcie_rc_reset mrst;
	/* [한국어] PERST#(엔드포인트 리셋) GPIO.
	 * 설정자: meson_pcie_probe() 가 devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW) 로 얻는다.
	 *   optional 판이 아니므로 DT 에 반드시 있어야 하고, 없으면 probe 가 실패한다.
	 * 읽는 자: meson_pcie_assert_reset() 이 1 -> 대기 -> 0 으로 펄스를 만든다.
	 * 값 범위: 유효 포인터(NULL 불가).
	 * 동기화: GPIO 프레임워크가 처리한다. */
	struct gpio_desc *reset_gpio;
	/* [한국어] PCIe SerDes PHY.
	 * 설정자: meson_pcie_probe() 가 devm_phy_get(dev, "pcie") 로 얻는다.
	 * 읽는 자: power_on/power_off/reset 세 함수가 PHY API 에 넘긴다.
	 * 값 범위: 유효 포인터(NULL 불가).
	 * 동기화: PHY 프레임워크가 자체 뮤텍스로 보호한다. */
	struct phy *phy;
};

/* [한국어]
 * meson_pcie_get_reset - 이름으로 리셋 컨트롤 하나를 얻는다(공유/전용 선택)
 *
 * @mp: 컨트롤러 객체. mp->pci.dev 가 devm 수명 기준이다.
 * @id: DT 의 reset-names 에 적힌 이름("port" 또는 "apb").
 * @reset_type: PCIE_SHARED_RESET 이면 공유 판, PCIE_NORMAL_RESET 이면 전용 판.
 * @return: 유효한 reset_control 포인터 또는 ERR_PTR. 검사는 호출자가 한다.
 *
 * 왜 필요한가: 두 리셋 라인의 성격이 다르다. port 리셋은 이 컨트롤러 전용이라
 * 배타적으로 잡아야 하고, apb 리셋은 SoC 의 다른 블록과 공유하는 라인이라
 * 참조 계수로 관리되는 shared 판으로 잡아야 한다. 전용 판으로 공유 라인을 잡으려
 * 하면 다른 사용자가 이미 잡고 있을 때 획득이 실패한다. 이 함수는 그 선택을
 * 한 곳으로 모아 호출부를 단순하게 만든다.
 *
 * 공유 리셋의 의미: 모든 사용자가 assert 해야 실제로 리셋이 걸리고,
 * 하나라도 deassert 하면 풀린다. 그래서 apb 리셋을 이 드라이버가 assert 해도
 * 다른 블록이 쓰고 있으면 실제로는 걸리지 않을 수 있다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. ERR_PTR 을 그대로 통과시킨다.
 *
 * 호출 체인:
 *   meson_pcie_get_resets() → [meson_pcie_get_reset]
 *     → devm_reset_control_get_shared() 또는 devm_reset_control_get()
 */
static struct reset_control *meson_pcie_get_reset(struct meson_pcie *mp,
						  const char *id,
						  u32 reset_type)
{
	/* [한국어] 로그와 devm 할당의 기준이 될 디바이스를 공용 코어 객체에서 꺼낸다. */
	struct device *dev = mp->pci.dev;
	/* [한국어] 얻은 리셋 컨트롤을 담을 지역 변수. */
	struct reset_control *reset;

	/* [한국어] 공유 리셋인지 전용 리셋인지에 따라 다른 API 를 써야 한다. */
	if (reset_type == PCIE_SHARED_RESET)
		/* [한국어] 공유 판. 여러 드라이버가 같은 리셋 라인을 참조 계수로 나눠 쓴다. */
		reset = devm_reset_control_get_shared(dev, id);
	else
		/* [한국어] 전용 판. 이 드라이버만 그 라인을 제어하며, 다른 사용자가 있으면 획득이 실패한다. */
		reset = devm_reset_control_get(dev, id);

	/* [한국어] 성공 포인터든 ERR_PTR 이든 그대로 돌려준다. 검사는 호출자의 몫이다. */
	return reset;
}

/* [한국어]
 * meson_pcie_get_resets - port/apb 두 리셋을 얻고 곧바로 해제한다
 *
 * @mp: 컨트롤러 객체. mp->mrst 의 두 필드를 채운다.
 * @return: 0 = 성공, 음수 = 어느 한쪽 획득 실패(-EPROBE_DEFER 포함).
 *
 * [상류 코드 관찰, 수정하지 않음] 이름은 "get" 이지만 획득 직후 곧바로
 * reset_control_deassert() 까지 한다 — 자원을 얻는 것을 넘어 하드웨어 상태를
 * 바꾸는 부수 효과가 있다. 그 결과 apb 획득이 실패하면 port 리셋이 이미 풀린
 * 상태로 probe 가 중단된다. 되돌리는 코드는 없다.
 *
 * 왜 여기서 해제까지 하는가: 부트로더가 리셋을 걸어 둔 채 넘겨줄 수 있으므로,
 * 이후 단계(메모리 매핑, PHY 전원)가 동작하려면 컨트롤러가 리셋에서 벗어나
 * 있어야 한다. 뒤이은 meson_pcie_reset() 이 다시 assert -> deassert 펄스를 주므로
 * 여기서의 해제는 "알려진 출발 상태 만들기" 에 가깝다.
 *
 * 동작 과정: port 를 전용으로 얻어 해제하고, apb 를 공유로 얻어 해제한다.
 * 두 획득 사이에 순서 의존성은 없지만, 실패 시 되감기가 없어 순서가 결과에 남는다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 지점 모두 곧장 return 한다. devm 자원이라 reset_control 자체는
 * 드라이버 코어가 회수하지만, 위에서 말한 "풀린 채 남는 리셋" 은 회수되지 않는다.
 *
 * 호출 체인:
 *   meson_pcie_probe() → [meson_pcie_get_resets]
 *     → meson_pcie_get_reset() ×2 → reset_control_deassert() ×2
 */
static int meson_pcie_get_resets(struct meson_pcie *mp)
{
	/* [한국어] 채워 넣을 리셋 묶음 구조체의 주소. */
	struct meson_pcie_rc_reset *mrst = &mp->mrst;

	/* [한국어] 포트 리셋을 전용으로 얻는다. */
	mrst->port = meson_pcie_get_reset(mp, "port", PCIE_NORMAL_RESET);
	/* [한국어] 획득 실패 검사. */
	if (IS_ERR(mrst->port))
		/* [한국어] 오류 코드를 꺼내 전달한다. */
		return PTR_ERR(mrst->port);
	/* [한국어] [상류 코드 관찰] "get" 이라는 이름의 함수가 곧바로 deassert 까지 한다 —
	 * 즉 자원 획득만이 아니라 하드웨어 상태를 바꾸는 부수 효과가 있다.
	 * 그래서 다음 줄의 apb 획득이 실패하면 포트 리셋이 풀린 채로 probe 가 중단된다. */
	reset_control_deassert(mrst->port);

	/* [한국어] APB 리셋을 공유로 얻는다. */
	mrst->apb = meson_pcie_get_reset(mp, "apb", PCIE_SHARED_RESET);
	/* [한국어] 획득 실패 검사. */
	if (IS_ERR(mrst->apb))
		/* [한국어] 오류 전달. 위에서 언급한 대로 이 경로에서는 포트 리셋이 이미 풀려 있다. */
		return PTR_ERR(mrst->apb);
	/* [한국어] APB 리셋도 곧바로 해제한다. */
	reset_control_deassert(mrst->apb);

	/* [한국어] 두 리셋 모두 획득·해제 완료. */
	return 0;
}

/* [한국어]
 * meson_pcie_get_mems - DBI 창과 Amlogic 전용 config 창을 매핑한다
 *
 * @pdev: 플랫폼 디바이스. DT 자원을 이름으로 조회하는 데 쓴다.
 * @mp: 컨트롤러 객체. mp->pci.elbi_base / dbi_base / dbi_phys_addr 과
 *       mp->cfg_base 를 채운다.
 * @return: 0 = 성공, 음수 = 매핑 실패.
 *
 * 왜 필요한가: 이 컨트롤러는 두 종류의 레지스터 영역을 갖는다. 하나는 DesignWare
 * 표준 DBI 창이고, 다른 하나는 Amlogic 이 추가한 전용 config 블록(LTSSM 제어와
 * 링크 상태가 여기 있다)이다. 공용 코어는 앞의 것만 알고 있으므로 뒤의 것은
 * 이 파일이 직접 매핑해야 한다.
 *
 * 동작 과정:
 *   1) 위 영어 주석이 설명하는 호환 처리 — 일부 DT 가 'dbi' 영역을 'elbi' 라는
 *      이름으로 잘못 기술한다. 그런 DT 면 그 영역을 매핑해 elbi_base 와 dbi_base
 *      양쪽에 넣고 물리 주소까지 기록해, DWC 코어가 두 영역을 다시 파싱하지 않게 한다.
 *      'elbi' 가 없으면 이 블록을 통째로 건너뛰고 DWC 코어가 'dbi' 를 직접 찾는다.
 *   2) 'cfg' 이름의 Amlogic 전용 블록을 매핑한다. 이쪽은 필수다.
 *
 * devm_pci_remap_cfg_resource() 를 쓰는 이유는 config 접근에 안전한 메모리 속성
 * (쓰기 결합 없음)으로 매핑하기 위해서다. 일반 ioremap 으로 매핑하면 config
 * 트랜잭션이 병합되거나 재정렬되어 하드웨어가 오동작할 수 있다.
 *
 * [상류 코드 관찰, 수정하지 않음] 위 영어 주석은 대입 대상을 'pci->dbi_space' 라고
 * 적었지만 실제 코드가 채우는 필드는 pci->dbi_base 이고, dbi_space 라는 이름은
 * pcie-designware.h 에 존재하지 않는다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 세 지점 모두 곧장 return. 모든 매핑이 devm 이라 정리할 것이 없다.
 *
 * 호출 체인:
 *   meson_pcie_probe() → [meson_pcie_get_mems]
 *     → platform_get_resource_byname() / devm_pci_remap_cfg_resource()
 *     → devm_platform_ioremap_resource_byname()
 */
static int meson_pcie_get_mems(struct platform_device *pdev,
			       struct meson_pcie *mp)
{
	/* [한국어] 자원을 담을 공용 코어 객체. */
	struct dw_pcie *pci = &mp->pci;
	/* [한국어] platform_get_resource_byname() 결과를 받을 포인터. */
	struct resource *res;

	/*
	 * For the broken DTs that supply 'dbi' as 'elbi', parse the 'elbi'
	 * region and assign it to both 'pci->elbi_base' and 'pci->dbi_space' so
	 * that the DWC core can skip parsing both regions.
	 */
	/* [한국어] DT 에서 "elbi" 이름의 메모리 자원을 찾는다. 없을 수도 있으므로 곧바로
	 * IS_ERR 로 검사하지 않고 NULL 여부로 분기한다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	/* [한국어] 위 영어 주석대로, 일부 DT 가 'dbi' 영역을 'elbi' 라는 이름으로 잘못 기술한다.
	 * 그런 DT 를 위한 호환 경로다. */
	if (res) {
		/* [한국어] config 접근에 적합한 속성으로 매핑한다. devm_pci_remap_cfg_resource() 는
		 * 쓰기 결합(write-combine)을 끄고 config 접근에 안전한 매핑을 만들어 준다. */
		pci->elbi_base = devm_pci_remap_cfg_resource(pci->dev, res);
		/* [한국어] 매핑 실패 검사(ERR_PTR 반환 API). */
		if (IS_ERR(pci->elbi_base))
			/* [한국어] 오류 전달. */
			return PTR_ERR(pci->elbi_base);

		/* [한국어] 같은 매핑을 DBI 창으로도 쓴다. 이렇게 두 필드를 모두 채워 두면
		 * DWC 공용 코어가 'dbi' 자원을 다시 파싱하지 않고 넘어간다.
		 * [상류 코드 관찰] 위 영어 주석은 대입 대상을 'pci->dbi_space' 라고 적었지만
		 * 실제 코드가 채우는 필드는 pci->dbi_base 이고, dbi_space 라는 이름은
		 * pcie-designware.h 에 존재하지 않는다. 주석과 코드가 어긋나 있다. */
		pci->dbi_base = pci->elbi_base;
		/* [한국어] DBI 창의 물리 주소도 함께 기록한다. iATU 설정 등에서 물리 주소가 필요하다. */
		pci->dbi_phys_addr = res->start;
	}

	/* [한국어] Amlogic 전용 config 레지스터 블록을 매핑한다. 이쪽은 이름이 "cfg" 로 고정이고
	 * 반드시 있어야 한다. */
	mp->cfg_base = devm_platform_ioremap_resource_byname(pdev, "cfg");
	/* [한국어] 매핑 실패 검사. */
	if (IS_ERR(mp->cfg_base))
		/* [한국어] 오류 전달. */
		return PTR_ERR(mp->cfg_base);

	/* [한국어] 필요한 메모리 자원을 모두 확보했다. */
	return 0;
}

/* [한국어]
 * meson_pcie_power_on - SerDes PHY 를 초기화하고 전원을 넣는다
 *
 * @mp: 컨트롤러 객체.
 * @return: 0 = 성공, 음수 = phy_init 또는 phy_power_on 실패.
 *
 * PHY 프레임워크는 초기화(phy_init)와 전원 인가(phy_power_on)를 두 단계로 나눈다.
 * 전자는 레지스터 설정과 레인 구성 같은 준비이고, 후자는 실제 전력 공급이다.
 * 둘을 나눈 이유는 런타임 절전에서 전원만 껐다 켜고 초기화는 유지할 수 있게
 * 하기 위해서다.
 *
 * 이 함수의 핵심은 짝 맞추기다. phy_power_on 이 실패하면 앞서 성공한 phy_init 을
 * phy_exit 로 되돌린다. 그렇게 하지 않으면 PHY 의 내부 참조 계수가 어긋나
 * 다음 probe 시도(예: -EPROBE_DEFER 후 재시도)에서 오동작한다.
 *
 * 실행 컨텍스트: probe 경로. PHY 콜백이 잠들 수 있다.
 *
 * 에러 경로: phy_init 실패는 되돌릴 것이 없어 곧장 반환하고,
 * phy_power_on 실패만 phy_exit 를 거친다. 호출자는 이 함수가 실패하면
 * 정리할 것이 없다고 가정해도 된다.
 *
 * 호출 체인:
 *   meson_pcie_probe() → [meson_pcie_power_on] → phy_init() → phy_power_on()
 */
static int meson_pcie_power_on(struct meson_pcie *mp)
{
	/* [한국어] PHY API 반환값. 0 으로 초기화하지만 곧바로 대입되므로 의미는 없다. */
	int ret = 0;

	/* [한국어] PHY 를 초기화한다 — 레인 설정 등 실제 동작은 PHY 드라이버가 한다. */
	ret = phy_init(mp->phy);
	/* [한국어] 실패 검사. */
	if (ret)
		/* [한국어] 되돌릴 것이 없으므로 곧장 반환한다. */
		return ret;

	/* [한국어] PHY 에 전원을 넣는다. init 과 power_on 이 분리된 것은 PHY 프레임워크의 규약이다. */
	ret = phy_power_on(mp->phy);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 앞서 성공한 phy_init 을 되돌린다 — 짝을 맞추지 않으면 PHY 의 내부 참조 계수가
		 * 어긋나 다음 probe 시도에서 오동작한다. */
		phy_exit(mp->phy);
		/* [한국어] 오류 전달. */
		return ret;
	}

	/* [한국어] PHY 준비 완료. */
	return 0;
}

/* [한국어]
 * meson_pcie_power_off - PHY 전원을 내리고 초기화를 되돌린다
 *
 * @mp: 컨트롤러 객체.
 *
 * power_on 과 정확히 역순이다. 전원을 먼저 내리고 나서 초기화를 되돌리는 순서가
 * 중요하다 — 초기화 해제가 먼저 오면 PHY 드라이버가 이미 없는 설정을 참조하며
 * 전원을 내리려 할 수 있다.
 *
 * probe 의 err_phy 라벨에서만 불린다. remove 콜백이 없으므로 정상 종료 경로에서는
 * 불리지 않는다는 점에 주의해야 한다.
 *
 * 실행 컨텍스트: probe 실패 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   meson_pcie_probe()(err_phy 라벨) → [meson_pcie_power_off]
 *     → phy_power_off() → phy_exit()
 */
static void meson_pcie_power_off(struct meson_pcie *mp)
{
	/* [한국어] 전원을 내린다. */
	phy_power_off(mp->phy);
	/* [한국어] 초기화를 되돌린다. power_on 과 반대 순서로 짝을 맞춘다. */
	phy_exit(mp->phy);
}

/* [한국어]
 * meson_pcie_reset - PHY 와 컨트롤러를 리셋해 알려진 상태로 만든다
 *
 * @mp: 컨트롤러 객체.
 * @return: 0 = 성공, 음수 = phy_reset 실패.
 *
 * 왜 필요한가: 부트로더가 PCIe 를 이미 초기화했을 수도, 반쯤 하다 말았을 수도 있다.
 * 링크 훈련을 시작하기 전에 물리 계층과 컨트롤러를 모두 알려진 상태로 되돌려야
 * 결과가 재현 가능해진다.
 *
 * 동작 순서와 그 이유:
 *   1) phy_reset() — 물리 계층을 먼저 리셋한다. 그 위의 컨트롤러 리셋이 의미를
 *      가지려면 아래층이 먼저 정리되어 있어야 한다.
 *   2) port 와 apb 리셋을 함께 assert 하고 PCIE_RESET_DELAY(500us) 유지한다.
 *      udelay 는 바쁜 대기라 그동안 CPU 를 점유하지만, probe 경로이고 짧아서
 *      허용된다.
 *   3) 둘 다 deassert 하고 다시 같은 시간을 기다려 하드웨어가 안정되게 한다.
 *
 * apb 리셋은 공유 라인이므로, 다른 SoC 블록이 사용 중이면 2)의 assert 가 실제로는
 * 걸리지 않을 수 있다. 그것이 shared 판의 정의된 동작이다.
 *
 * 실행 컨텍스트: probe 경로. 총 1ms 가량 바쁜 대기를 한다.
 *
 * 에러 경로: phy_reset 실패 시 컨트롤러 리셋을 시도하지 않고 곧장 반환한다.
 * 호출자는 err_phy 라벨로 가서 PHY 전원을 내린다.
 *
 * 호출 체인:
 *   meson_pcie_probe() → [meson_pcie_reset]
 *     → phy_reset() → reset_control_assert() ×2 → udelay()
 *     → reset_control_deassert() ×2 → udelay()
 */
static int meson_pcie_reset(struct meson_pcie *mp)
{
	/* [한국어] 리셋 묶음의 주소. */
	struct meson_pcie_rc_reset *mrst = &mp->mrst;
	/* [한국어] phy_reset() 반환값. */
	int ret = 0;

	/* [한국어] 먼저 PHY 를 리셋한다. 컨트롤러 리셋보다 PHY 리셋이 앞서는 이유는
	 * 물리 계층이 먼저 알려진 상태가 되어야 그 위의 컨트롤러 리셋이 의미를 갖기 때문이다. */
	ret = phy_reset(mp->phy);
	/* [한국어] 실패 검사. */
	if (ret)
		/* [한국어] 컨트롤러 리셋을 시도하지 않고 곧장 반환한다. */
		return ret;

	/* [한국어] 포트 리셋을 건다. */
	reset_control_assert(mrst->port);
	/* [한국어] APB 리셋도 건다. 공유 리셋이므로 다른 사용자가 잡고 있으면 실제로는 걸리지 않을 수 있다. */
	reset_control_assert(mrst->apb);
	/* [한국어] 500us 동안 리셋을 유지한다. udelay 는 바쁜 대기라 이 시간 동안 CPU 를 점유하지만,
	 * probe 경로이고 짧아서 문제가 되지 않는다. */
	udelay(PCIE_RESET_DELAY);
	/* [한국어] 포트 리셋 해제. */
	reset_control_deassert(mrst->port);
	/* [한국어] APB 리셋 해제. */
	reset_control_deassert(mrst->apb);
	/* [한국어] 해제 후에도 같은 시간을 기다려 하드웨어가 안정되게 한다. */
	udelay(PCIE_RESET_DELAY);

	/* [한국어] 리셋 시퀀스 완료. */
	return 0;
}

/* [한국어]
 * meson_pcie_disable_clock - devm 액션으로 등록되는 클럭 해제 콜백
 *
 * @data: devm_add_action_or_reset() 에 넘긴 불투명 포인터. 실제로는 struct clk 다.
 *
 * 왜 필요한가: devm_clk_get() 은 clk 참조만 자동 해제할 뿐 clk_prepare_enable() 로
 * 켠 상태까지 되돌려 주지는 않는다. 그래서 "켠 것을 끄는" 동작을 별도의 devm
 * 액션으로 등록해야 하고, 이 함수가 그 액션의 본체다.
 *
 * 이 방식의 장점은 오류 경로가 단순해진다는 것이다. 클럭을 세 개 켜는 도중
 * 두 번째에서 실패해도, 첫 번째 클럭의 해제는 드라이버 코어가 알아서 해 준다 —
 * 호출부에 되감기 코드가 전혀 없는 이유가 그것이다.
 *
 * 실행 컨텍스트: 디바이스 해제 경로(probe 실패 또는 언바인드), 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 코어의 devm 정리 → [meson_pcie_disable_clock] → clk_disable_unprepare()
 */
static inline void meson_pcie_disable_clock(void *data)
{
	/* [한국어] devm 액션이 넘겨 준 불투명 포인터를 clk 로 되돌린다. */
	struct clk *clk = data;

	/* [한국어] 클럭을 끄고 prepare 도 되돌린다. clk_prepare_enable() 의 정확한 짝이다. */
	clk_disable_unprepare(clk);
}

/* [한국어]
 * meson_pcie_probe_clock - 클럭 하나를 얻고, 주파수를 맞추고, 켜고, 자동 해제를 등록한다
 *
 * @dev: devm 기준 디바이스이자 로그 대상.
 * @id: DT 의 clock-names 에 적힌 이름("port"/"general"/"pclk").
 * @rate: 설정할 주파수(Hz). 0 이면 주파수를 건드리지 않는다.
 * @return: 준비가 끝난 clk 포인터, 또는 실패를 담은 ERR_PTR.
 *       반환형이 포인터이므로 errno 는 ERR_PTR() 로 감싸 돌려준다.
 *
 * 왜 필요한가: 클럭 하나를 쓸 수 있게 만들려면 네 단계(얻기 → 주파수 설정 →
 * 켜기 → 해제 등록)가 필요하다. 이 드라이버는 클럭이 셋이라 그 네 단계를
 * 함수로 묶어 두지 않으면 호출부가 장황해진다.
 *
 * 동작 과정:
 *   1) devm_clk_get() 으로 얻는다. 실패하면 그 ERR_PTR 을 그대로 돌려준다 —
 *      -EPROBE_DEFER 도 이 경로로 전달되어 재시도가 성립한다.
 *   2) rate 가 0 이 아니면 clk_set_rate() 로 주파수를 맞춘다. port 클럭만
 *      100MHz(PCIe 레퍼런스 클럭 규격값)를 요구하고 나머지 둘은 0 을 넘긴다.
 *   3) clk_prepare_enable() 로 켠다.
 *   4) devm_add_action_or_reset() 으로 해제 콜백을 등록한다. _or_reset 판이라
 *      등록 자체가 실패하면 그 자리에서 콜백을 즉시 실행해 주므로, 등록 실패를
 *      따로 검사하지 않아도 클럭이 새지 않는다.
 *
 * 실행 컨텍스트: probe 경로. 클럭 프레임워크 호출이 잠들 수 있다.
 *
 * 에러 경로: 2)와 3)의 실패는 로그를 남기고 ERR_PTR 로 돌려준다. 이 시점에는
 * 아직 devm 액션이 등록되지 않았으므로, 3)이 실패했다면 클럭은 꺼진 상태 그대로다.
 *
 * 호출 체인:
 *   meson_pcie_probe_clocks() → [meson_pcie_probe_clock]
 *     → devm_clk_get() → clk_set_rate() → clk_prepare_enable()
 *     → devm_add_action_or_reset(meson_pcie_disable_clock)
 */
static inline struct clk *meson_pcie_probe_clock(struct device *dev,
						 const char *id, u64 rate)
{
	/* [한국어] 얻을 클럭. */
	struct clk *clk;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] DT 에서 이름으로 클럭을 찾는다. devm 이라 자동 해제된다. */
	clk = devm_clk_get(dev, id);
	/* [한국어] 획득 실패 검사. */
	if (IS_ERR(clk))
		/* [한국어] ERR_PTR 을 그대로 돌려주면 호출자가 IS_ERR 로 판별한다. */
		return clk;

	/* [한국어] rate 가 0 이 아니면 주파수를 명시적으로 설정한다. general/pclk 은 0 을 넘겨
	 * 이 단계를 건너뛰고, port 클럭만 100MHz 로 맞춘다. */
	if (rate) {
		/* [한국어] 주파수 설정. */
		ret = clk_set_rate(clk, rate);
		/* [한국어] 실패 검사. */
		if (ret) {
			/* [한국어] 실패 로그. */
			dev_err(dev, "set clk rate failed, ret = %d\n", ret);
			/* [한국어] errno 를 ERR_PTR 로 감싸 돌려준다 — 반환형이 포인터이기 때문이다. */
			return ERR_PTR(ret);
		}
	}

	/* [한국어] 클럭을 준비하고 켠다. prepare 와 enable 을 한 번에 하는 편의 함수다. */
	ret = clk_prepare_enable(clk);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "couldn't enable clk\n");
		/* [한국어] ERR_PTR 로 감싸 전달. */
		return ERR_PTR(ret);
	}

	/* [한국어] devm 액션을 등록해, 이 디바이스가 사라질 때 위 disable_clock 이 자동으로 불리게 한다.
	 * _or_reset 판이라 등록 자체가 실패하면 그 자리에서 콜백을 즉시 실행해 준다 —
	 * 그래서 등록 실패를 따로 검사하지 않아도 클럭이 새지 않는다. */
	devm_add_action_or_reset(dev, meson_pcie_disable_clock, clk);

	/* [한국어] 준비가 끝난 클럭 포인터를 돌려준다. */
	return clk;
}

/* [한국어]
 * meson_pcie_probe_clocks - 세 개의 클럭을 모두 준비한다
 *
 * @mp: 컨트롤러 객체. mp->clk_res 의 세 필드를 채운다.
 * @return: 0 = 성공, 음수 = 어느 하나라도 실패.
 *
 * 세 클럭의 역할:
 *   - port: PCIe 레퍼런스 클럭. 유일하게 주파수를 100MHz 로 명시 지정한다.
 *   - general: 컨트롤러 코어 클럭.
 *   - pclk: APB 인터페이스 클럭. DT 이름은 "pclk" 인데 구조체 필드 이름은 clk 이라
 *     둘이 어긋나 보이는 점에 주의한다.
 *
 * 세 필드에 담아 두기는 하지만 이후 아무도 읽지 않는다 — 해제가 devm 액션으로
 * 자동화되어 있어 포인터를 보관할 실질적 이유가 없다. 사실상 디버깅용 기록이다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 세 지점 모두 곧장 return 한다. 앞서 성공한 클럭들은 이미 devm 액션이
 * 등록되어 있어 드라이버 코어가 끄고 반납한다 — 되감기 코드가 없는 이유다.
 *
 * 호출 체인:
 *   meson_pcie_probe() → [meson_pcie_probe_clocks] → meson_pcie_probe_clock() ×3
 */
static int meson_pcie_probe_clocks(struct meson_pcie *mp)
{
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = mp->pci.dev;
	/* [한국어] 채워 넣을 클럭 묶음의 주소. */
	struct meson_pcie_clk_res *res = &mp->clk_res;

	/* [한국어] port 클럭만 100MHz 로 주파수를 지정해 얻는다. */
	res->port_clk = meson_pcie_probe_clock(dev, "port", PORT_CLK_RATE);
	/* [한국어] 실패 검사. */
	if (IS_ERR(res->port_clk))
		/* [한국어] 오류 전달. 앞서 성공한 클럭은 devm 액션이 정리한다. */
		return PTR_ERR(res->port_clk);

	/* [한국어] general 클럭. 주파수는 그대로 둔다. */
	res->general_clk = meson_pcie_probe_clock(dev, "general", 0);
	/* [한국어] 실패 검사. */
	if (IS_ERR(res->general_clk))
		/* [한국어] 오류 전달. */
		return PTR_ERR(res->general_clk);

	/* [한국어] pclk(APB 인터페이스 클럭). 구조체 필드 이름은 clk 인데 DT 이름은 pclk 이라
	 * 이름이 어긋나 보이는 점에 주의한다. */
	res->clk = meson_pcie_probe_clock(dev, "pclk", 0);
	/* [한국어] 실패 검사. */
	if (IS_ERR(res->clk))
		/* [한국어] 오류 전달. */
		return PTR_ERR(res->clk);

	/* [한국어] 세 클럭 모두 활성화 완료. */
	return 0;
}

/* [한국어]
 * meson_cfg_readl - Amlogic 전용 config 레지스터 블록에서 32비트를 읽는다
 *
 * @mp: 컨트롤러 객체. mp->cfg_base 가 그 블록의 시작이다.
 * @reg: 블록 안에서의 바이트 오프셋(PCIE_CFG0, PCIE_CFG_STATUS12 등).
 * @return: 읽은 32비트 값.
 *
 * 이 블록은 DesignWare 표준 DBI 창과 완전히 별개다. DWC 코어가 모르는,
 * Amlogic 이 자기 SoC 를 위해 추가한 레지스터들이 여기 있다 —
 * LTSSM 활성화 비트와 링크 상태 비트가 대표적이다.
 *
 * _relaxed 가 아닌 readl() 을 쓰는 이유: 링크 상태 폴링에서 쓰이므로 컴파일러나
 * CPU 가 이전 값을 재사용하면 무한 루프가 된다. readl 의 배리어가 매번 실제
 * 하드웨어를 읽도록 보장한다.
 *
 * 실행 컨텍스트: probe 경로와 DWC 코어의 링크 대기 폴링. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   meson_pcie_link_up() / meson_pcie_ltssm_enable() → [meson_cfg_readl] → readl()
 */
static inline u32 meson_cfg_readl(struct meson_pcie *mp, u32 reg)
{
	/* [한국어] Amlogic 전용 config 블록에서 32비트를 읽는다. readl 은 배리어를 포함한 판이라,
	 * 링크 상태 폴링에서 캐시된 값을 보는 일이 없다. */
	return readl(mp->cfg_base + reg);
}

/* [한국어]
 * meson_cfg_writel - Amlogic 전용 config 레지스터 블록에 32비트를 쓴다
 *
 * @mp: 컨트롤러 객체.
 * @val: 쓸 값.
 * @reg: 블록 안에서의 바이트 오프셋.
 *
 * 읽기 쪽과 대칭이다. 인자 순서가 (val, reg) 로 표준 writel(val, addr) 의 배치를
 * 따르지만, 짝인 meson_cfg_readl(mp, reg) 과 나란히 놓고 보면 두 번째 인자의
 * 의미가 달라 헷갈리기 쉽다.
 *
 * 실행 컨텍스트: probe 경로(LTSSM 활성화)에서만 쓰인다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   meson_pcie_ltssm_enable() → [meson_cfg_writel] → writel()
 */
static inline void meson_cfg_writel(struct meson_pcie *mp, u32 val, u32 reg)
{
	/* [한국어] 같은 블록에 32비트를 쓴다. 인자 순서가 (val, reg) 인 점에 주의. */
	writel(val, mp->cfg_base + reg);
}

/* [한국어]
 * meson_pcie_assert_reset - 엔드포인트에 PERST# 펄스를 준다
 *
 * @mp: 컨트롤러 객체. mp->reset_gpio 를 토글한다.
 *
 * [이름에 주의] "assert_reset" 이라는 이름과 달리 이 함수는 리셋을 걸어 두는 것이
 * 아니라 걸었다 푸는 펄스를 만든다. 어서트 → 500us 유지 → 디어서트가 전부이고,
 * 함수가 돌아오면 PERST# 는 해제된 상태다. 링크 훈련은 그 해제 시점부터 시작된다.
 *
 * 왜 펄스인가: PCIe 규격상 PERST# 는 전원과 레퍼런스 클럭이 안정된 뒤 최소 시간
 * 이상 유지했다가 해제해야 한다. 엔드포인트는 그 해제 에지를 보고 초기화를
 * 시작하므로, 한 번의 완결된 펄스가 필요하다.
 *
 * _cansleep 판을 쓰는 이유: GPIO 컨트롤러가 I2C 같은 느린 버스 뒤에 있을 수 있어
 * 설정에 시간이 걸릴 수 있다. 이 함수가 프로세스 컨텍스트에서만 불린다는 뜻이기도 하다.
 *
 * [상류 코드 관찰, 수정하지 않음] 대기 시간을 PCIE_RESET_DELAY 상수가 아니라
 * 숫자 500 으로 직접 적었다. 값은 같지만 상수와 연결되어 있지 않아,
 * 상수를 바꿔도 이 자리는 따라 바뀌지 않는다.
 *
 * 실행 컨텍스트: start_link 콜백 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   meson_pcie_start_link() → [meson_pcie_assert_reset] → gpiod_set_value_cansleep()
 */
static void meson_pcie_assert_reset(struct meson_pcie *mp)
{
	/* [한국어] PERST# 를 어서트한다. _cansleep 판을 쓰는 이유는 GPIO 컨트롤러가 I2C 같은
	 * 느린 버스 뒤에 있을 수도 있기 때문이며, 이 함수가 프로세스 컨텍스트에서만
	 * 불린다는 뜻이기도 하다. */
	gpiod_set_value_cansleep(mp->reset_gpio, 1);
	/* [한국어] [상류 코드 관찰] 500us 를 기다리는데 PCIE_RESET_DELAY 상수를 쓰지 않고
	 * 숫자를 직접 적었다. 값은 같지만 상수와 연결되어 있지 않다. */
	udelay(500);
	/* [한국어] PERST# 를 해제한다. 이 순간부터 엔드포인트가 링크 훈련을 시작한다. */
	gpiod_set_value_cansleep(mp->reset_gpio, 0);
}

/* [한국어]
 * meson_pcie_ltssm_enable - LTSSM 을 켜 컨트롤러 쪽 링크 훈련을 시작시킨다
 *
 * @mp: 컨트롤러 객체.
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크의 상태 기계다.
 * 이 비트를 세우기 전에는 컨트롤러가 링크 훈련을 아예 시도하지 않으므로,
 * 아무리 기다려도 링크가 올라오지 않는다.
 *
 * 읽기-수정-쓰기로 APP_LTSSM_ENABLE 비트만 켠다. CFG0 의 다른 비트가 무엇인지는
 * 이 트리의 코드만으로 알 수 없지만, OR 를 쓰는 이상 그것들을 보존하려는 의도는
 * 분명하다. 락이 없어도 안전한 것은 probe 경로에서 한 번만 불리기 때문이다.
 *
 * 실행 컨텍스트: start_link 콜백 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   meson_pcie_start_link() → [meson_pcie_ltssm_enable]
 *     → meson_cfg_readl() → meson_cfg_writel()
 */
static void meson_pcie_ltssm_enable(struct meson_pcie *mp)
{
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 val;

	/* [한국어] 현재 CFG0 값을 읽는다. */
	val = meson_cfg_readl(mp, PCIE_CFG0);
	/* [한국어] LTSSM 활성화 비트만 켠다. 다른 비트를 보존하기 위해 OR 를 쓴다. */
	val |= APP_LTSSM_ENABLE;
	/* [한국어] 되쓴다. 이 쓰기로 링크 훈련이 시작된다. */
	meson_cfg_writel(mp, val, PCIE_CFG0);
}

/* [한국어]
 * meson_size_to_payload - 바이트 크기를 PCIe 의 3비트 인코딩 값으로 바꾼다
 *
 * @mp: 컨트롤러 객체. 경고 로그에 쓸 디바이스를 얻는 용도로만 쓴다.
 * @size: 바이트 단위 크기(128 ~ 4096 의 2의 거듭제곱).
 * @return: PCIe 규격의 인코딩 값(0 ~ 5). 유효하지 않은 입력이면 기본값 1(=256바이트).
 *
 * 왜 필요한가: PCIe 의 Max Payload Size 와 Max Read Request Size 필드는 3비트로
 * 크기를 표현하는데, 그 인코딩이 2^(값+7) 바이트다. 즉 0=128B, 1=256B, 2=512B,
 * … 5=4096B. 사람이 읽기 쉬운 바이트 값을 그 인코딩으로 바꾸는 것이 이 함수다.
 *
 * 동작 과정: 위 영어 주석이 설명하는 세 조건(2의 거듭제곱, 128 이상, 4096 이하)을
 * 모두 만족하지 않으면 경고를 남기고 기본값 1 을 돌려준다. 유효하면
 * fls(size) - 8 을 계산한다. fls() 는 최상위 비트의 위치를 1부터 세어 돌려주므로
 * size = 2^n 이면 fls = n+1 이고, 거기서 8 을 빼면 n-7 이 되어 정확히
 * 2^(값+7) 인코딩의 역함수가 된다. 예: 256 = 2^8 → fls = 9 → 1.
 *
 * 실행 컨텍스트: host_init 콜백 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 잘못된 입력을 오류로 만들지 않고 기본값으로 대체한다 —
 * 링크를 못 쓰게 만드는 것보다 안전한 기본값으로 계속하는 편이 낫다는 판단이다.
 *
 * 호출 체인:
 *   meson_set_max_payload() / meson_set_max_rd_req_size() → [meson_size_to_payload]
 */
static int meson_size_to_payload(struct meson_pcie *mp, int size)
{
	/* [한국어] 경고 로그에 쓸 디바이스. */
	struct device *dev = mp->pci.dev;

	/*
	 * dwc supports 2^(val+7) payload size, which val is 0~5 default to 1.
	 * So if input size is not 2^order alignment or less than 2^7 or bigger
	 * than 2^12, just set to default size 2^(1+7).
	 */
	/* [한국어] 세 가지 조건을 모두 만족해야 유효한 크기다 — 2의 거듭제곱이고, 128 이상,
	 * 4096 이하. PCIe 규격이 그 범위만 허용하기 때문이다. */
	if (!is_power_of_2(size) || size < 128 || size > 4096) {
		/* [한국어] 잘못된 값이면 경고만 남기고 진행한다. */
		dev_warn(dev, "payload size %d, set to default 256\n", size);
		/* [한국어] 기본값 1 = 2^(1+7) = 256바이트. */
		return 1;
	}

	/* [한국어] fls(size) 는 최상위 비트 위치(1부터 셈)를 준다. 256 이면 fls = 9 이고
	 * 9 - 8 = 1 이 되어 위 기본값과 같은 인코딩이 나온다.
	 * 일반화하면 size = 2^n 일 때 n - 7 을 돌려주는 셈이며, 이것이 바로
	 * PCIe 의 2^(값+7) 인코딩의 역함수다. */
	return fls(size) - 8;
}

/* [한국어]
 * meson_set_max_payload - Max Payload Size 를 지정한 값으로 강제한다
 *
 * @mp: 컨트롤러 객체.
 * @size: 설정할 바이트 크기. 호출부는 MAX_PAYLOAD_SIZE(256)을 넘긴다.
 *
 * 왜 필요한가: MPS 는 링크 위의 모든 장치가 합의해야 하는 값이다. 루트 포트가
 * 자신의 능력대로 큰 값을 쓰는데 하위 장치가 그만큼 못 받으면 패킷이 깨진다.
 * 이 SoC 에서는 안전한 값으로 256바이트를 고정하는 정책을 택했고, 그 강제가
 * 버스 스캔 이전(host_init)에 이루어져야 한다.
 *
 * 동작 과정:
 *   1) dw_pcie_find_capability() 로 PCI Express Capability 구조체의 config space
 *      오프셋을 찾는다. capability 위치는 장치마다 다르므로 런타임 탐색이 필수다.
 *   2) 바이트 크기를 인코딩 값으로 바꾼다.
 *   3) Device Control 레지스터에서 MPS 필드를 지우고 쓴다.
 *   4) 다시 읽어 원하는 값을 넣고 쓴다.
 *
 * [상류 코드 관찰, 수정하지 않음] 3)과 4)를 한 번의 읽기-수정-쓰기로 끝낼 수
 * 있는데도 두 번에 나눠 하고 있다. 그 사이 아주 짧게 MPS 가 0(=128바이트)인
 * 상태가 존재하는데, 아직 링크 위에 트래픽이 없는 시점이라 실害는 없다.
 *
 * 실행 컨텍스트: host_init 콜백, 프로세스 컨텍스트. DBI 접근은 잠들지 않는다.
 *
 * 에러 경로: 없다. dw_pcie_find_capability() 가 0 을 돌려주는 경우(capability
 * 없음)를 검사하지 않으므로, 그때는 오프셋 0 + PCI_EXP_DEVCTL 자리에 쓰게 된다.
 *
 * 호출 체인:
 *   meson_pcie_host_init() → [meson_set_max_payload]
 *     → dw_pcie_find_capability() / meson_size_to_payload()
 *     → dw_pcie_readl_dbi() / dw_pcie_writel_dbi()
 */
static void meson_set_max_payload(struct meson_pcie *mp, int size)
{
	/* [한국어] DBI 접근에 필요한 공용 코어 객체. */
	struct dw_pcie *pci = &mp->pci;
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 val;
	/* [한국어] PCI Express Capability 구조체가 config space 안 어디에 있는지 찾는다.
	 * capability 위치는 장치마다 다르므로 반드시 런타임에 탐색해야 한다. */
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* [한국어] 바이트 크기를 PCIe 인코딩 값으로 변환한다. */
	int max_payload_size = meson_size_to_payload(mp, size);

	/* [한국어] Device Control 레지스터를 읽는다. */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_DEVCTL);
	/* [한국어] Max Payload Size 필드를 0 으로 지운다. */
	val &= ~PCI_EXP_DEVCTL_PAYLOAD;
	/* [한국어] [상류 코드 관찰] 지운 값을 곧바로 되쓴 뒤 아래에서 다시 읽는다.
	 * 한 번의 읽기-수정-쓰기로 끝낼 수 있는 일을 두 번에 나눠 하고 있어,
	 * 그 사이 아주 짧게 MPS 가 0(=128바이트)인 상태가 존재한다. */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_DEVCTL, val);

	/* [한국어] 방금 쓴 값을 다시 읽는다. */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_DEVCTL);
	/* [한국어] 원하는 MPS 값을 필드에 넣는다. */
	val |= PCIE_CAP_MAX_PAYLOAD_SIZE(max_payload_size);
	/* [한국어] 최종 값을 쓴다. */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_DEVCTL, val);
}

/* [한국어]
 * meson_set_max_rd_req_size - Max Read Request Size 를 지정한 값으로 강제한다
 *
 * @mp: 컨트롤러 객체.
 * @size: 설정할 바이트 크기. 호출부는 MAX_READ_REQ_SIZE(256)을 넘긴다.
 *
 * MRRS 는 이 장치가 한 번의 읽기 요청으로 요구할 수 있는 최대 바이트 수다.
 * MPS 와 인코딩 규칙이 같고(2^(값+7)) 같은 Device Control 레지스터의 다른
 * 필드([14:12])에 들어간다.
 *
 * MPS 와 달리 MRRS 는 링크 전체의 합의가 아니라 개별 장치의 요청 크기 상한이라,
 * 너무 크게 잡으면 한 장치가 링크를 오래 점유해 다른 장치의 지연이 커진다.
 * 256바이트는 처리량과 공정성의 절충값이다.
 *
 * 동작 과정과 구조는 meson_set_max_payload() 와 완전히 동일하다 —
 * capability 탐색, 인코딩 변환, 두 번에 나눈 읽기-수정-쓰기까지 같다.
 * 두 함수가 capability 를 각자 다시 찾는 중복이 있지만, 서로 독립적으로
 * 호출될 수 있어 그렇게 되어 있다.
 *
 * 실행 컨텍스트: host_init 콜백, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. capability 부재를 검사하지 않는 것도 payload 쪽과 같다.
 *
 * 호출 체인:
 *   meson_pcie_host_init() → [meson_set_max_rd_req_size]
 *     → dw_pcie_find_capability() / meson_size_to_payload()
 *     → dw_pcie_readl_dbi() / dw_pcie_writel_dbi()
 */
static void meson_set_max_rd_req_size(struct meson_pcie *mp, int size)
{
	/* [한국어] DBI 접근용 공용 코어 객체. */
	struct dw_pcie *pci = &mp->pci;
	/* [한국어] 임시 변수. */
	u32 val;
	/* [한국어] PCI Express Capability 위치를 다시 찾는다. 바로 위 함수와 중복 탐색이지만
	 * 두 함수가 독립적으로 호출될 수 있어 각자 찾는다. */
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* [한국어] 바이트 크기를 인코딩 값으로 변환한다. payload 와 같은 인코딩 규칙을 쓴다. */
	int max_rd_req_size = meson_size_to_payload(mp, size);

	/* [한국어] Device Control 레지스터를 읽는다. */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_DEVCTL);
	/* [한국어] Max Read Request Size 필드를 지운다. */
	val &= ~PCI_EXP_DEVCTL_READRQ;
	/* [한국어] 지운 값을 쓴다 — payload 쪽과 같은 2단계 방식이다. */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_DEVCTL, val);

	/* [한국어] 다시 읽는다. */
	val = dw_pcie_readl_dbi(pci, offset + PCI_EXP_DEVCTL);
	/* [한국어] 원하는 값을 필드에 넣는다. */
	val |= PCIE_CAP_MAX_READ_REQ_SIZE(max_rd_req_size);
	/* [한국어] 최종 값을 쓴다. */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_DEVCTL, val);
}

/* [한국어]
 * meson_pcie_start_link - 링크 훈련을 시작시킨다(DWC 코어 콜백)
 *
 * @pci: DWC 공용 코어 객체. to_meson_pcie() 로 이 파일의 컨트롤러를 되찾는다.
 * @return: 항상 0.
 *
 * 왜 항상 0 인가: 이 콜백의 계약은 "훈련을 시작시켜라" 이지 "링크를 성립시켜라"
 * 가 아니다. 실제 성립 여부는 DWC 코어가 link_up 콜백을 반복 호출하며 판정하고,
 * 타임아웃도 코어가 관리한다. 그래서 여기서 돌려줄 실패가 없다.
 *
 * 동작 순서와 그 이유:
 *   1) LTSSM 을 켠다 — 컨트롤러 쪽이 훈련을 받아들일 준비를 한다.
 *   2) 엔드포인트에 PERST# 펄스를 준다 — 상대편이 훈련을 시작한다.
 * 순서가 반대면 엔드포인트가 훈련을 시작했을 때 컨트롤러가 응답하지 못해
 * 훈련이 실패하거나 늦어진다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안, 프로세스 컨텍스트.
 * PERST# 펄스에서 500us 바쁜 대기가 들어간다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_ops.start_link == [meson_pcie_start_link]
 *     → meson_pcie_ltssm_enable() → meson_pcie_assert_reset()
 */
static int meson_pcie_start_link(struct dw_pcie *pci)
{
	/* [한국어] 공용 코어 객체에서 이 파일의 컨트롤러를 되찾는다. */
	struct meson_pcie *mp = to_meson_pcie(pci);

	/* [한국어] LTSSM 을 켜 컨트롤러 쪽 링크 훈련을 시작시킨다. */
	meson_pcie_ltssm_enable(mp);
	/* [한국어] 엔드포인트에 PERST# 펄스를 주어 상대편도 훈련을 시작하게 한다.
	 * 순서가 중요하다 — 컨트롤러가 먼저 준비되어야 엔드포인트의 훈련 요청을 받는다. */
	meson_pcie_assert_reset(mp);

	/* [한국어] 항상 0 을 돌려준다. 실제 링크 성립 여부는 DWC 코어가 link_up 콜백을
	 * 폴링해 판정하므로, 이 함수는 훈련을 "시작"시키기만 하면 된다. */
	return 0;
}

/* [한국어]
 * meson_pcie_rd_own_conf - 루트 포트 자신의 config 읽기를 가로채 class code 를 지어낸다
 *
 * @bus: 접근 대상 버스(루트 버스).
 * @devfn: 디바이스·함수 번호.
 * @where: config space 안의 바이트 오프셋.
 * @size: 읽을 바이트 수(1, 2, 4).
 * @val: 읽은 값을 담을 출력 인자.
 * @return: PCIBIOS_SUCCESSFUL 또는 하위 읽기가 준 PCIBIOS_* 오류.
 *
 * 왜 필요한가: 위 영어 주석이 밝히듯 MESON AXG 컨트롤러에는 소프트웨어가
 * PCI_CLASS_DEVICE 레지스터를 쓸 수 없는 버그가 있다. 그런데 PCI 코어는 class
 * code 를 보고 그 장치가 브리지인지 판단하고, 브리지가 아니면 하위 버스를 열거하지
 * 않는다. 하드웨어를 고칠 수 없으므로, 읽을 때 값을 지어내는 방식으로 우회한다.
 *
 * 동작 과정:
 *   1) 먼저 표준 경로로 실제 값을 읽는다. 실패하면 그대로 전달한다.
 *   2) 읽은 오프셋이 PCI_CLASS_REVISION(0x08)을 포함하는 워드인지 확인한다.
 *      (where & ~3) 로 워드 경계에 맞춰 비교하므로 0x08~0x0b 어느 바이트를 읽어도 걸린다.
 *   3) 1~2바이트 읽기였다면 값이 이미 워드 안에서 잘려 나온 상태이므로,
 *      원래 워드 위치로 되돌려 놓는다. (where & 3) 이 워드 안 바이트 오프셋이고
 *      8 을 곱해 비트 자리로 바꾼다.
 *   4) 상위 3바이트(class code 자리)를 지우고 PCI-to-PCI 브리지 코드(0x060400)를
 *      8비트 밀어 넣는다. 하위 1바이트(Revision ID)는 실제 값을 보존한다.
 *   5) 1~2바이트 읽기였다면 다시 요청 폭으로 잘라 낸다 — 3)의 역연산이다.
 *
 * 쓰기는 가로채지 않는다. 어차피 하드웨어가 받아 주지 않고, PCI 코어가 class code 를
 * 쓰는 일도 없기 때문이다.
 *
 * 실행 컨텍스트: PCI 코어의 config 접근 경로. 전역 pci_lock 을 쥔 상태로 불린다.
 *
 * 에러 경로: 하위 읽기의 오류만 전달한다. 자체 오류는 없다.
 *
 * 호출 체인:
 *   pci_read_config_dword() 등 → bus->ops->read == [meson_pcie_rd_own_conf]
 *     → pci_generic_config_read() → (dw_pcie_own_conf_map_bus 가 계산한 주소)
 */
static int meson_pcie_rd_own_conf(struct pci_bus *bus, u32 devfn,
				  int where, int size, u32 *val)
{
	/* [한국어] 코어 범용 읽기의 결과를 받을 변수. */
	int ret;

	/* [한국어] 먼저 표준 경로로 실제 값을 읽는다. */
	ret = pci_generic_config_read(bus, devfn, where, size, val);
	/* [한국어] 읽기 자체가 실패했으면 조작할 것이 없다. */
	if (ret != PCIBIOS_SUCCESSFUL)
		/* [한국어] 오류를 그대로 전달한다. */
		return ret;

	/*
	 * There is a bug in the MESON AXG PCIe controller whereby software
	 * cannot program the PCI_CLASS_DEVICE register, so we must fabricate
	 * the return value in the config accessors.
	 */
	/* [한국어] 읽은 오프셋이 PCI_CLASS_REVISION(0x08) 을 포함하는 워드인지 확인한다.
	 * & ~3 으로 워드 경계에 맞춰 비교하므로, 0x08~0x0b 어느 바이트를 읽어도 걸린다. */
	if ((where & ~3) == PCI_CLASS_REVISION) {
		/* [한국어] 1바이트나 2바이트 읽기였다면, 값이 이미 워드 안에서 잘려 나온 상태다. */
		if (size <= 2)
			/* [한국어] 그것을 원래 워드 위치로 되돌려 놓는다. (where & 3) 이 워드 안에서의
			 * 바이트 오프셋이고, 8을 곱해 비트 자리로 바꾼다. */
			*val = (*val & ((1 << (size * 8)) - 1)) << (8 * (where & 3));
		/* [한국어] class code 가 들어갈 상위 3바이트를 지운다. 하위 1바이트(Revision ID)는 보존한다. */
		*val &= ~0xffffff00;
		/* [한국어] PCI-to-PCI 브리지 class code(0x060400)를 8비트 왼쪽으로 밀어 그 자리에 넣는다.
		 * 위 영어 주석대로 AXG 컨트롤러는 이 레지스터를 소프트웨어로 쓸 수 없어,
		 * 읽을 때 값을 지어내는 방식으로 우회한다. 이렇게 해야 PCI 코어가 루트 포트를
		 * 브리지로 인식하고 하위 버스를 열거한다. */
		*val |= PCI_CLASS_BRIDGE_PCI_NORMAL << 8;
		/* [한국어] 1~2바이트 읽기였다면, */
		if (size <= 2)
			/* [한국어] 다시 원래 요청 폭으로 잘라 낸다. 333번 줄의 역연산이다. */
			*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);
	}

	/* [한국어] 조작 여부와 무관하게 성공을 돌려준다. */
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops meson_pci_ops = {
	/* [한국어] 주소 계산은 DWC 공용 구현에 맡긴다. */
	.map_bus = dw_pcie_own_conf_map_bus,
	/* [한국어] 읽기만 이 파일이 가로챈다 — class code 를 지어내야 하기 때문이다. */
	.read = meson_pcie_rd_own_conf,
	/* [한국어] 쓰기는 손댈 이유가 없어 코어 범용 구현을 그대로 쓴다. */
	.write = pci_generic_config_write,
};

/* [한국어]
 * meson_pcie_link_up - 링크가 올라왔는지 판정한다(DWC 코어 콜백)
 *
 * @pci: DWC 공용 코어 객체.
 * @return: true = 링크 업, false = 아직 아니거나 실패.
 *
 * DWC 코어가 링크 대기 루프에서 이 콜백을 반복 호출한다. 그래서 이 함수는
 * 가볍고 부작용이 없어야 하며, 실제로 레지스터 한 번 읽기가 전부다.
 *
 * 판정 기준이 두 비트인 이유: SMLH 는 물리 계층(Standard Media Layer Handler)이,
 * RDLH 는 데이터 링크 계층(Receive Data Link Handler)이 올라왔음을 뜻한다.
 * 물리 계층만 올라온 상태에서는 아직 TLP 를 주고받을 수 없으므로, 둘 다 서야
 * 비로소 config 접근을 시작해도 안전하다.
 *
 * Amlogic 전용 상태 레지스터를 읽으므로 DWC 표준 링크 판정과는 다른 경로다.
 * 그래서 이 콜백을 반드시 구현해야 한다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 의 링크 대기 루프, 프로세스 컨텍스트.
 * 읽기만 하므로 잠들지 않는다.
 *
 * 에러 경로: 없다. 링크가 끝내 올라오지 않으면 DWC 코어의 타임아웃이 처리한다.
 *
 * 호출 체인:
 *   dw_pcie_wait_for_link() → dw_pcie_ops.link_up == [meson_pcie_link_up]
 *     → meson_cfg_readl(PCIE_CFG_STATUS12)
 */
static bool meson_pcie_link_up(struct dw_pcie *pci)
{
	/* [한국어] 공용 코어 객체에서 컨트롤러를 되찾는다. */
	struct meson_pcie *mp = to_meson_pcie(pci);
	/* [한국어] 상태 레지스터 값을 담을 변수. */
	u32 state12;

	/* [한국어] Amlogic 전용 상태 레지스터를 읽는다. */
	state12 = meson_cfg_readl(mp, PCIE_CFG_STATUS12);
	/* [한국어] 물리 계층(SMLH)과 데이터 링크 계층(RDLH)이 모두 올라와야 링크 업으로 판정한다.
	 * 둘 중 하나만 서 있으면 훈련이 진행 중이거나 실패한 상태다. */
	return IS_SMLH_LINK_UP(state12) && IS_RDLH_LINK_UP(state12);
}

/* [한국어]
 * meson_pcie_host_init - 버스 스캔 직전에 config ops 를 갈아 끼우고 MPS/MRRS 를 고정한다
 *
 * @pp: DWC 호스트 포트 객체. 여기서 dw_pcie 를, 다시 meson_pcie 를 되찾는다.
 * @return: 항상 0.
 *
 * 왜 이 시점인가: DWC 코어는 자원 준비를 마치고 버스를 스캔하기 직전에 이 콜백을
 * 부른다. 두 가지 일이 반드시 그 사이에 일어나야 한다.
 *   1) config 접근 ops 교체 — 스캔이 시작되면 곧바로 config 읽기가 일어나는데,
 *      그때 class code 조작이 이미 걸려 있어야 루트 포트가 브리지로 인식된다.
 *      한 발 늦으면 하위 버스가 열거되지 않는다.
 *   2) MPS/MRRS 고정 — 하위 장치가 열거되기 전에 루트 포트의 값을 정해 두어야
 *      코어의 MPS 협상이 올바른 상한에서 출발한다.
 *
 * pp->bridge->ops 에 대입하는 것이지 pci->ops 가 아니라는 점에 주의한다.
 * 전자는 PCI 코어가 config 접근에 쓰는 pci_ops 이고, 후자는 DWC 내부 콜백 테이블이다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 항상 0 을 돌려주므로 코어는 계속 진행한다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [meson_pcie_host_init]
 *     → meson_set_max_payload() → meson_set_max_rd_req_size()
 */
static int meson_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* [한국어] dw_pcie_rp 에서 감싸고 있는 dw_pcie 를 얻는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 거기서 다시 이 파일의 컨트롤러를 얻는다. */
	struct meson_pcie *mp = to_meson_pcie(pci);

	/* [한국어] config 접근 연산을 이 파일 것으로 바꿔 단다. class code 조작이 필요하므로
	 * DWC 기본 ops 를 그대로 쓸 수 없다. 이 대입은 버스 스캔이 시작되기 전에
	 * 이루어져야 하며, host_init 콜백이 바로 그 시점이다. */
	pp->bridge->ops = &meson_pci_ops;

	/* [한국어] MPS 를 256바이트로 강제한다. */
	meson_set_max_payload(mp, MAX_PAYLOAD_SIZE);
	/* [한국어] Max Read Request Size 도 256바이트로 강제한다. */
	meson_set_max_rd_req_size(mp, MAX_READ_REQ_SIZE);

	/* [한국어] 초기화 성공. */
	return 0;
}

static const struct dw_pcie_host_ops meson_pcie_host_ops = {
	/* [한국어] 호스트 초기화 콜백만 제공한다. 나머지는 DWC 공용 구현으로 충분하다. */
	.init = meson_pcie_host_init,
};

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크 상태 판정 콜백 — DWC 코어가 링크 대기 루프에서 반복 호출한다. */
	.link_up = meson_pcie_link_up,
	/* [한국어] 링크 훈련 시작 콜백. */
	.start_link = meson_pcie_start_link,
};

/* [한국어]
 * meson_pcie_probe - Amlogic MESON PCIe 컨트롤러를 초기화하고 버스를 올린다
 *
 * @pdev: DT 에서 "amlogic,axg-pcie" 또는 "amlogic,g12a-pcie" 로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공. -ENOMEM = 컨트롤러 객체 할당 실패.
 *       그 밖의 음수 = PHY/GPIO/리셋/메모리 획득 실패, 리셋 시퀀스 실패,
 *       클럭 준비 실패, 또는 DWC 호스트 초기화 실패(-EPROBE_DEFER 포함).
 *
 * 왜 이 순서인가: 각 단계가 앞 단계의 결과에 의존한다.
 *   1) 컨트롤러 객체를 devm 으로 할당하고 DWC 코어 객체의 필드를 채운다 —
 *      ops 테이블 둘과 레인 수(1 고정)를 여기서 정한다.
 *   2) PHY 와 PERST# GPIO 를 얻는다. 둘 다 optional 이 아니므로 DT 에 반드시 있어야
 *      하고, 없으면 probe 가 실패한다.
 *   3) 리셋을 얻고 해제한다(부수 효과 주의 — get_resets 주석 참조).
 *   4) DBI 창과 Amlogic 전용 config 창을 매핑한다.
 *   5) PHY 전원을 넣는다. 이때부터 err_phy 정리가 필요해진다.
 *   6) 리셋 펄스로 알려진 상태를 만든다.
 *   7) 세 클럭을 준비한다.
 *   8) drvdata 를 심는다 — to_meson_pcie() 가 이 값을 쓰므로, 그것을 사용하는
 *      link_up/start_link 콜백이 불리기 전(즉 dw_pcie_host_init 전)이어야 한다.
 *   9) dw_pcie_host_init() 이 iATU 설정, 링크 훈련 대기, 버스 스캔, 장치 등록까지
 *      모두 처리한다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 * 리셋과 PERST# 펄스에서 합쳐 1.5ms 가량 바쁜 대기가 들어간다.
 *
 * 에러 경로: 정리가 필요한 유일한 자원이 PHY 전원이므로 라벨도 err_phy 하나뿐이다.
 * 그 이전 단계의 실패는 곧장 return 하고, 클럭·리셋·GPIO·메모리 매핑은
 * devm(또는 devm 액션)이라 드라이버 코어가 되돌린다.
 * 다만 get_resets 가 풀어 둔 리셋 상태는 어느 경로로도 되돌아가지 않는다.
 *
 * 호출 체인:
 *   DT 매칭 → platform_driver.probe == [meson_pcie_probe]
 *     → devm_phy_get() / devm_gpiod_get()
 *     → meson_pcie_get_resets() → meson_pcie_get_mems()
 *     → meson_pcie_power_on() → meson_pcie_reset() → meson_pcie_probe_clocks()
 *     → dw_pcie_host_init() → (콜백으로 되돌아옴) meson_pcie_host_init(),
 *   meson_pcie_start_link(), meson_pcie_link_up()
 */
static int meson_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] 공용 코어 객체를 가리킬 포인터. */
	struct dw_pcie *pci;
	/* [한국어] 이 파일의 컨트롤러 객체. */
	struct meson_pcie *mp;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] 컨트롤러 객체를 0 초기화 할당한다. devm 이라 자동 해제된다. */
	mp = devm_kzalloc(dev, sizeof(*mp), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!mp)
		/* [한국어] -ENOMEM 전달. */
		return -ENOMEM;

	/* [한국어] 구조체에 내장된 공용 코어 객체의 주소. */
	pci = &mp->pci;
	/* [한국어] DWC 코어가 로그와 자원 관리에 쓸 디바이스. */
	pci->dev = dev;
	/* [한국어] link_up/start_link 콜백 테이블 연결. */
	pci->ops = &dw_pcie_ops;
	/* [한국어] 호스트 초기화 콜백 테이블 연결. */
	pci->pp.ops = &meson_pcie_host_ops;
	/* [한국어] 이 컨트롤러는 1레인 고정이다. DT 에서 읽지 않고 코드에 박아 두었다. */
	pci->num_lanes = 1;

	/* [한국어] PCIe SerDes PHY 를 얻는다. optional 판이 아니므로 DT 에 반드시 있어야 한다. */
	mp->phy = devm_phy_get(dev, "pcie");
	/* [한국어] 획득 실패 검사. */
	if (IS_ERR(mp->phy)) {
		/* [한국어] 실패 로그. PTR_ERR 값을 함께 찍어 원인을 알 수 있게 한다. */
		dev_err(dev, "get phy failed, %ld\n", PTR_ERR(mp->phy));
		/* [한국어] 오류 전달. */
		return PTR_ERR(mp->phy);
	}

	/* [한국어] PERST# GPIO 를 얻는다. GPIOD_OUT_LOW 는 출력 방향으로 설정하면서
	 * 초기값을 논리 0(리셋 해제)으로 두라는 뜻이다. */
	mp->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	/* [한국어] 획득 실패 검사. */
	if (IS_ERR(mp->reset_gpio)) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "get reset gpio failed\n");
		/* [한국어] 오류 전달. */
		return PTR_ERR(mp->reset_gpio);
	}

	/* [한국어] 두 리셋 컨트롤을 얻고 곧바로 해제한다. */
	ret = meson_pcie_get_resets(mp);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "get reset resource failed, %d\n", ret);
		/* [한국어] 오류 전달. */
		return ret;
	}

	/* [한국어] DBI 창과 Amlogic 전용 config 창을 매핑한다. */
	ret = meson_pcie_get_mems(pdev, mp);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "get memory resource failed, %d\n", ret);
		/* [한국어] 오류 전달. */
		return ret;
	}

	/* [한국어] PHY 를 초기화하고 전원을 넣는다. */
	ret = meson_pcie_power_on(mp);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "phy power on failed, %d\n", ret);
		/* [한국어] [상류 코드 관찰] power_on 이 자체적으로 되감아 주므로 여기서는 정리가 필요 없다. */
		return ret;
	}

	/* [한국어] PHY 리셋과 컨트롤러 리셋 시퀀스를 수행한다. */
	ret = meson_pcie_reset(mp);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "reset failed, %d\n", ret);
		/* [한국어] 여기부터는 PHY 가 켜져 있으므로 반드시 정리 구간을 거쳐야 한다. */
		goto err_phy;
	}

	/* [한국어] 세 클럭을 얻고 활성화한다. */
	ret = meson_pcie_probe_clocks(mp);
	/* [한국어] 실패 검사. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "init clock resources failed, %d\n", ret);
		/* [한국어] PHY 정리 구간으로. */
		goto err_phy;
	}

	/* [한국어] to_meson_pcie() 매크로가 되찾을 수 있도록 컨트롤러를 심는다.
	 * 이 대입이 dw_pcie_host_init() 보다 앞서야 하는 이유는, 그 안에서 불리는
	 * link_up/start_link 콜백이 이 매크로를 쓰기 때문이다. */
	platform_set_drvdata(pdev, mp);

	/* [한국어] DWC 공용 호스트 초기화 — DBI 파싱, iATU 설정, 링크 훈련 대기,
	 * PCI 버스 스캔과 장치 등록이 모두 여기서 일어난다. */
	ret = dw_pcie_host_init(&pci->pp);
	/* [한국어] 실패 검사. */
	if (ret < 0) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "Add PCIe port failed, %d\n", ret);
		/* [한국어] PHY 정리 구간으로. */
		goto err_phy;
	}

	/* [한국어] 모든 초기화 성공. */
	return 0;

/* [한국어] PHY 를 켠 뒤의 실패가 모이는 정리 라벨. */
err_phy:
	/* [한국어] PHY 전원을 내리고 초기화를 되돌린다. 클럭과 리셋은 각각 devm 액션과
	 * devm 자원이라 드라이버 코어가 처리한다. */
	meson_pcie_power_off(mp);
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return ret;
}

static const struct of_device_id meson_pcie_of_match[] = {
	{
		/* [한국어] Amlogic AXG SoC 의 PCIe 컨트롤러. */
		.compatible = "amlogic,axg-pcie",
	},
	{
		/* [한국어] Amlogic G12A SoC 의 PCIe 컨트롤러. 두 SoC 가 같은 코드를 쓴다. */
		.compatible = "amlogic,g12a-pcie",
	},
	/* [한국어] 테이블 끝을 알리는 빈 항목. */
	{},
};
/* [한국어] 모듈 자동 로딩을 위해 매칭 테이블을 모듈 메타데이터로 내보낸다. */
MODULE_DEVICE_TABLE(of, meson_pcie_of_match);

static struct platform_driver meson_pcie_driver = {
	/* [한국어] 장치가 나타났을 때 불릴 진입점.
	 * [상류 코드 관찰, 수정하지 않음] remove 콜백이 없다. 모듈로 빌드될 수 있는데도
	 * 언바인드 경로가 준비되어 있지 않아, sysfs 로 언바인드하면 PCI 버스가 등록된 채
	 * 남는다. devm 자원만 회수되고 dw_pcie_host_init() 이 만든 버스는 정리되지 않는다. */
	.probe = meson_pcie_probe,
	.driver = {
		/* [한국어] 드라이버 이름 — sysfs 와 로그에 나타난다. */
		.name = "meson-pcie",
		/* [한국어] 위에서 정의한 DT 매칭 테이블. */
		.of_match_table = meson_pcie_of_match,
	},
};

/* [한국어] module_init/module_exit 보일러플레이트를 대신하는 매크로.
 * builtin_ 판이 아니므로 이 드라이버는 모듈로 빌드될 수 있다. */
module_platform_driver(meson_pcie_driver);

/* [한국어] modinfo 에 표시될 작성자. */
MODULE_AUTHOR("Yue Wang <yue.wang@amlogic.com>");
/* [한국어] modinfo 에 표시될 설명. */
MODULE_DESCRIPTION("Amlogic PCIe Controller driver");
/* [한국어] 라이선스 선언. GPL 계열이어야 DWC 공용 코어가 내보낸 심볼을 쓸 수 있다. */
MODULE_LICENSE("GPL v2");
