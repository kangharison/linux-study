// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for NXP S32G SoCs
 *
 * Copyright 2019-2025 NXP
 */

/*
 * [한국어 설명] NXP S32G 자동차용 SoC 의 DesignWare PCIe 호스트 글루
 * (pcie-nxp-s32g.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 NXP S32G 에 붙이는 글루 드라이버다.
 * config 접근, ATU 설정, 버스 스캔은 pcie-designware-host.c 가 맡고,
 * 이 파일은 SoC 고유의 것만 처리한다.
 *
 * 하는 일이 넷이다.
 *   1) 컨트롤러를 루트 포트 모드로 세우고 LTSSM 을 켜고 끈다.
 *   2) **캐시 일관성 경계** 를 설정한다 — 이 파일의 가장 중요한 부분이며
 *      아래에서 따로 설명한다.
 *   3) SerDes PHY 를 초기화한다. 포트를 리스트로 관리하는 구조라
 *      여러 개를 상정하고 있으나, 실제로는 하나만 지원한다.
 *   4) 디바이스 트리에서 num-lanes 를 직접 읽어 DWC 코어에 넘긴다.
 *
 * 같은 트리의 다른 DWC 글루들과 견주면 특징이 뚜렷하다. pci-exynos.c 나
 * pcie-histb.c 는 DBI 접근에 사이드밴드가 필요해 그 처리에 코드를 쓰는데,
 * 이 파일은 그런 것이 없다. 대신 **일관성 경계 설정** 이라는, 다른 어느
 * 글루에도 없는 문제를 푼다.
 *
 * === Ncore 일관성 경계 (이 드라이버의 핵심) ===
 * S32G 에는 Ncore 라는 캐시 일관성 인터커넥트가 있다. 캐시를 지키는 주체와
 * 그렇지 않은 주체를 한 칩에 섞어 놓기 위한 장치다.
 *
 * 문제는 이렇다. **주변장치(peripheral)는 S32G 에서 슬레이브로서 일관성을
 * 갖지 않는데**, PCIe 쪽에서 일관성 있는 트랜잭션이 주변장치 주소로
 * 향하면 Ncore 가 그것을 **그냥 버린다.**
 *
 * 대표적인 피해자가 **MSI** 다. PCIe MSI 는 NoSnoop=0 으로 — 즉 "캐시를
 * 살펴 달라" 는 뜻으로 — 나가는데, MSI 목적지가 주변장치 주소 영역이면
 * Ncore 를 거치며 사라진다. 인터럽트가 오지 않는 것으로 나타난다.
 *
 * 해법이 s32g_pcie_reset_mstr_ace() 다. PCIe 컨트롤러의 일관성 제어
 * 레지스터에 **주소 경계를 하나 못 박아**, 그 아래는 주변장치(비일관성),
 * 그 위는 물리 메모리(일관성)로 나눈다. 경계값은 리눅스가 보는 DDR 의
 * 시작 주소인 0x80000000 이다.
 *
 * 이 설정은 DWC 코어의 ACE(AMBA AXI Coherency Extensions) 인터페이스
 * 레지스터를 통해 이뤄지며, 그 레지스터들이 읽기 전용이라
 * dw_pcie_dbi_ro_wr_en/dis() 로 감싸야 쓸 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> s32g_pcie_probe()
 *     -> s32g_pcie_get_resources()
 *        -> 제어 레지스터 창 매핑 -> 디바이스 트리의 포트 자식 노드 훑기
 *           -> 포트마다 PHY 확보, num-lanes 읽기
 *     -> 런타임 PM 참조 획득
 *     -> s32g_pcie_init() -> LTSSM 끄기 -> PHY 초기화·전원
 *     -> dw_pcie_host_init()
 *        -> 그 안에서 콜백 -> [이 파일] s32g_init_pcie_controller()
 *           -> 루트 포트 모드 -> 일관성 경계 -> SRIS 디스큐 -> Gen3 EQ
 *        -> 코어가 start_link 콜백 -> [이 파일] LTSSM 켜기
 *        -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 이 파일에는 인터럽트 핸들러가
 * 하나도 없다 — 인터럽트 처리는 DWC 코어와 MSI 계층이 맡는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware.c / pcie-designware-host.c. 접점이 두 벌의
 *   콜백 표다 — dw_pcie_ops(start_link, stop_link)와
 *   dw_pcie_host_ops(init 하나). 그리고 DBI 접근 도우미들을 쓴다.
 * 옆쪽: phy 계층(SerDes)과 런타임 PM. phy 드라이버는 이 트리에 없다.
 *
 * 데이터 흐름:
 *   디바이스 트리(ctrl 레지스터 창, 포트 자식 노드, PHY, num-lanes)
 *     -> s32g_pcie_get_resources() -> struct s32g_pcie
 *   일관성 경계값은 코드에 박힌 상수 하나에서 나온다 — 디바이스 트리가
 *   아니라 컴파일 시점에 정해진다.
 *
 * 공유 상태: struct s32g_pcie 하나. 포트 리스트를 담고 있으며 probe 이후
 *   바뀌지 않는다. 잠금이 없는데, 리스트를 만드는 것도 없애는 것도 probe
 *   경로 하나뿐이라 성립한다.
 *
 * === NVMe 관점 ===
 * 위의 Ncore 문제가 NVMe 에 직결된다. NVMe 컨트롤러는 완료를 알릴 때
 * MSI/MSI-X 를 쓰는데, 일관성 경계가 잘못 설정돼 있으면 그 MSI 가 Ncore 에서
 * 사라져 **명령이 완료됐는데도 인터럽트가 오지 않는** 증상이 된다.
 * 큐 데이터 자체는 DDR(경계 위)에 있어 무사하므로, 폴링으로는 완료가
 * 보이는데 인터럽트만 오지 않는 형태로 드러난다.
 *
 * === 주요 함수/구조체 요약 ===
 * s32g_pcie_reset_mstr_ace()   : 일관성 경계를 못 박는다. 이 파일의 핵심.
 * s32g_init_pcie_controller()  : 루트 포트 모드, 경계, SRIS 디스큐, Gen3 EQ.
 * s32g_init_pcie_phy()         : 포트 리스트를 돌며 PHY 를 세운다. 되감기가
 *                                라벨 두 개로 나뉜 것이 볼 만하다.
 * s32g_pcie_parse_ports()      : 디바이스 트리의 "pci" 타입 자식만 골라 훑는다.
 * s32g_pcie_probe()            : 위 전부를 순서대로 엮는다.
 * struct s32g_pcie             : dw_pcie 를 맨 앞에 둔 상태 구조체.
 * struct s32g_pcie_port        : 포트 하나 — 사실상 PHY 하나를 감싼 것.
 */

/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(irqreturn_t, request_irq, IRQF_ 계열)을
 * 쓰는 곳을 이 파일에서 찾지 못했다. 이 드라이버에는 인터럽트 핸들러가 없다. */
#include <linux/interrupt.h>
/* [한국어] readl/writel — 제어 레지스터 창 접근에 쓴다. */
#include <linux/io.h>
/* [한국어] MODULE_AUTHOR/DESCRIPTION/LICENSE 선언용. */
#include <linux/module.h>
/* [한국어] of_ 계열 디바이스 매칭 헤더. */
#include <linux/of_device.h>
/* [한국어] [상류 코드 관찰] 이 헤더의 이름(of_address_to_resource 등)을 쓰는 곳을
 * 이 파일에서 찾지 못했다. */
#include <linux/of_address.h>
/* [한국어] PCI_EXP_TYPE_ROOT_PORT 를 쓰기 위해 필요하다. */
#include <linux/pci.h>
/* [한국어] phy_init/phy_power_on/phy_set_mode_ext — SerDes PHY 조작. */
#include <linux/phy/phy.h>
/* [한국어] platform_device 와 자원 확보 도우미들. */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_ 계열 — probe 가 런타임 PM 참조를 잡는다. */
#include <linux/pm_runtime.h>
/* [한국어] [상류 코드 관찰] SZ_ 계열 상수를 쓰는 곳을 이 파일에서 찾지 못했다. */
#include <linux/sizes.h>
/* [한국어] u32 등 기본 타입. 대개 다른 헤더로도 딸려 오지만 명시해 두었다. */
#include <linux/types.h>

/* [한국어] DWC 코어의 자료구조와 DBI 도우미. **일관성 제어 레지스터 오프셋들
 * (COHERENCY_CONTROL_1/2/3_OFF)도 이 헤더에 있다** — 이 파일이 그 이름들을
 * 쓰는 두 곳 중 하나이며, 다른 하나는 pcie-andes-qilai.c 다. */
#include "pcie-designware.h"

/* PCIe controller Sub-System */

/* PCIe controller 0 General Control 1 */
/* [한국어] SoC 제어 창 안의 일반 제어 레지스터 1. 동작 모드가 여기 있다. */
#define PCIE_S32G_PE0_GEN_CTRL_1		0x50
/* [한국어] 장치 종류 필드(4비트). PCIe 규격의 장치 종류 값을 그대로 넣는다 —
 * 이 드라이버는 언제나 루트 포트를 뜻하는 값을 쓴다. */
#define DEVICE_TYPE_MASK			GENMASK(3, 0)
/* [한국어] SRIS(Separate Reference clock with Independent SSC) 모드 비트.
 * 켜면 양쪽이 서로 다른 기준 클럭을 쓰는 구성이 되며, 이 드라이버는
 * 지워서 기본 방식(CRNS)을 쓴다. */
#define SRIS_MODE				BIT(8)

/* PCIe controller 0 General Control 3 */
/* [한국어] SoC 제어 창 안의 일반 제어 레지스터 3. LTSSM 제어가 여기 있다. */
#define PCIE_S32G_PE0_GEN_CTRL_3		0x58
/* [한국어] LTSSM 활성 비트. 이 하나로 링크 훈련을 켜고 끈다. */
#define LTSSM_EN				BIT(0)

/* PCIe Controller 0  Interrupt Status */
/* [한국어] [상류 코드 관찰] 인터럽트 상태 레지스터 오프셋이지만 이 파일 어디에서도
 * 참조하지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#define PCIE_S32G_PE0_INT_STS			0xE8
/* [한국어] [상류 코드 관찰] 핫플러그 인터럽트 상태 비트. 위 레지스터와 마찬가지로
 * 이 파일에서 참조하는 곳이 없다. 이 드라이버가 핫플러그를 다루지 않기
 * 때문으로 보인다. */
#define HP_INT_STS				BIT(6)

/* Boundary between peripheral space and physical memory space */
/* [한국어] 일관성 경계 주소. 리눅스가 보는 DDR 의 시작이며, 이 값 아래는
 * 주변장치(비일관성), 위는 물리 메모리(일관성)로 나뉜다.
 * **디바이스 트리가 아니라 코드에 박혀 있다** — DDR 시작이 다른 보드는
 * 이 상수를 고쳐야 한다. */
#define S32G_MEMORY_BOUNDARY_ADDR		0x80000000

struct s32g_pcie_port {
	/* [한국어] 포트 리스트의 연결 고리.
	 * 설정자: s32g_pcie_parse_port() 가 INIT_LIST_HEAD 로 초기화한 뒤
	 * 곧바로 s32g_pp->ports 꼬리에 붙인다.
	 * 읽는 자: PHY 초기화·해제 루프와, 실패 시 리스트를 비우는 코드.
	 * 값 범위: 리스트에 붙어 있는 동안 유효. list_del 후에는 끊긴 상태로 남는다.
	 * 동기화: 없다. 리스트를 만드는 것도 없애는 것도 probe 경로 하나뿐이다. */
	struct list_head list;
	/* [한국어] 이 포트의 SerDes PHY 핸들.
	 * 설정자: s32g_pcie_parse_port() 의 devm_of_phy_get().
	 * 읽는 자: s32g_init_pcie_phy() 가 세우고 s32g_deinit_pcie_phy() 가 내린다.
	 * 값 범위: 유효한 phy 포인터. 실패는 파싱 단계에서 걸러진다.
	 * 동기화: 없다. **사실상 이 구조체는 PHY 하나를 리스트에 담기 위한
	 * 껍데기다** — 필드가 둘뿐이고 그중 하나가 연결 고리다. */
	struct phy *phy;
};

struct s32g_pcie {
	/* [한국어] DWC PCIe 코어의 공통 문맥. **구조체 맨 앞** 이라 이 주소가 곧
	 * s32g_pcie 의 주소이며, to_s32g_from_dw_pcie 매크로가 그것에 기댄다.
	 * 설정자: s32g_pcie_get_resources() 가 dev 와 ops 를 채우고
	 * s32g_pcie_parse_port() 가 num_lanes 를 채운다. 나머지는 DWC 코어의 몫.
	 * 읽는 자: 이 파일의 거의 모든 함수.
	 * 값 범위: 언제나 유효한 내장 구조체이며 포인터가 아니다.
	 * 동기화: probe 이후 이 파일이 바꾸는 필드가 없다. */
	struct dw_pcie	pci;
	/* [한국어] SoC 고유 제어 레지스터 창의 가상 주소.
	 * 설정자: s32g_pcie_get_resources() 가 "ctrl" 이라는 **이름으로** 얻는다.
	 * 읽는 자: s32g_pcie_readl_ctrl()/writel_ctrl() 둘뿐.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 이후 불변.
	 * DWC 코어의 DBI 창과는 **별개의 창** 이라는 점이 중요하다. */
	void __iomem *ctrl_base;
	/* [한국어] 포트 리스트의 머리.
	 * 설정자: s32g_pcie_get_resources() 가 초기화하고,
	 * s32g_pcie_parse_port() 가 원소를 붙인다.
	 * 읽는 자: PHY 초기화·해제 루프.
	 * 값 범위: 원소가 0개 이상. 실제로는 S32G 가 루트 포트 하나만 지원하므로
	 * 언제나 하나다 — 리스트 구조는 앞날을 위한 여지로 보인다.
	 * 동기화: 없다. probe 경로 하나만 닿는다. */
	struct list_head ports;
};

/* [한국어] DWC 코어가 주는 dw_pcie 포인터를 이 드라이버의 구조체로 되돌린다.
 * pci 가 구조체 맨 앞이라 오프셋이 0 이지만, container_of 를 쓰면 나중에
 * 필드 순서가 바뀌어도 안전하다. */
#define to_s32g_from_dw_pcie(x) \
	container_of(x, struct s32g_pcie, pci)

/* [한국어]
 * s32g_pcie_writel_ctrl - SoC 제어 레지스터 창에 쓴다
 *
 * @s32g_pp: 드라이버 상태. 창의 가상 주소를 담고 있다.
 * @reg: 창 안의 오프셋.
 * @val: 쓸 값.
 *
 * 이 파일에는 창이 둘 있다 — SoC 고유의 제어 창(ctrl_base)과 DWC 코어의
 * DBI 창이다. 이 도우미는 **앞의 것 전용** 이며, DBI 쪽은 코어가 제공하는
 * dw_pcie_writel_dbi() 를 쓴다. 두 창을 헷갈리지 않게 하는 것이 이 짧은
 * 함수의 존재 이유다.
 *
 * 인자 순서가 오프셋 먼저, 값 나중이다 — 같은 트리의 pci-exynos.c 가
 * 반대 순서를 쓰는 것과 대비된다.
 *
 * 실행 컨텍스트: probe 와 링크 제어. 프로세스 컨텍스트이며 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   s32g_pcie_enable_ltssm() / s32g_pcie_disable_ltssm() /
 *   s32g_init_pcie_controller() → [이 함수] → writel()
 */
static void s32g_pcie_writel_ctrl(struct s32g_pcie *s32g_pp, u32 reg, u32 val)
{
	writel(val, s32g_pp->ctrl_base + reg);
}

/* [한국어]
 * s32g_pcie_readl_ctrl - SoC 제어 레지스터 창을 읽는다
 *
 * @s32g_pp: 드라이버 상태.
 * @reg: 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * 위 쓰기 판의 짝이다. 이 파일의 레지스터 조작이 모두 읽기-수정-쓰기라
 * 언제나 쌍으로 쓰인다.
 *
 * 실행 컨텍스트: probe 와 링크 제어. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   s32g_pcie_enable_ltssm() / s32g_pcie_disable_ltssm() /
 *   s32g_init_pcie_controller() → [이 함수] → readl()
 */
static u32 s32g_pcie_readl_ctrl(struct s32g_pcie *s32g_pp, u32 reg)
{
	return readl(s32g_pp->ctrl_base + reg);
}

/* [한국어]
 * s32g_pcie_enable_ltssm - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @s32g_pp: 드라이버 상태.
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크를 세우는 상태
 * 기계다. 이 비트를 켜야 컨트롤러가 상대와 협상을 시작한다.
 *
 * 읽기-수정-쓰기로 비트 하나만 건드린다. 같은 레지스터의 다른 설정을
 * 보존해야 하기 때문이다.
 *
 * **켜기 전에 컨트롤러 설정이 모두 끝나 있어야 한다.** 링크가 서고 나면
 * 바꿀 수 없는 설정(레인 수, 일관성 경계 등)이 있기 때문이며, 그래서
 * DWC 코어가 host_init 콜백을 먼저 부르고 start_link 를 나중에 부른다.
 *
 * 실행 컨텍스트: 코어의 링크 시작 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   s32g_pcie_start_link() → [이 함수]
 *     → s32g_pcie_readl_ctrl() → s32g_pcie_writel_ctrl()
 */
static void s32g_pcie_enable_ltssm(struct s32g_pcie *s32g_pp)
{
	u32 reg;

	reg = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3);
	/* [한국어] LTSSM 비트만 세운다. 나머지 설정은 그대로 둬야 한다. */
	reg |= LTSSM_EN;
	/* [한국어] 고친 값을 되쓴다. 이 쓰기 순간부터 링크 훈련이 시작된다. */
	s32g_pcie_writel_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3, reg);
}

/* [한국어]
 * s32g_pcie_disable_ltssm - LTSSM 을 꺼 링크를 내린다
 *
 * @s32g_pp: 드라이버 상태.
 *
 * 위 함수의 짝이며 비트를 지운다는 것만 다르다.
 *
 * **부르는 자리가 셋** 이라는 점이 눈여겨볼 만하다 — 코어의 stop_link
 * 콜백, 그리고 초기화·해제 함수 양쪽이다. 초기화에서 먼저 끄는 것은
 * 부트로더가 링크를 이미 세워 두었을 수 있어서, 알려진 상태에서 설정을
 * 시작하려는 것이다.
 *
 * 실행 컨텍스트: 코어의 링크 정지, probe, 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   s32g_pcie_stop_link() / s32g_pcie_init() / s32g_pcie_deinit()
 *     → [이 함수] → s32g_pcie_readl_ctrl() → s32g_pcie_writel_ctrl()
 */
static void s32g_pcie_disable_ltssm(struct s32g_pcie *s32g_pp)
{
	u32 reg;

	reg = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3);
	/* [한국어] LTSSM 비트만 지운다. */
	reg &= ~LTSSM_EN;
	/* [한국어] 고친 값을 되쓴다. 링크가 서 있었다면 여기서 내려간다. */
	s32g_pcie_writel_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_3, reg);
}

/* [한국어]
 * s32g_pcie_start_link - DWC 코어의 링크 시작 요청을 받는다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * 코어와 이 파일 사이의 얇은 껍데기다. 코어가 주는 dw_pcie 포인터를
 * container_of 매크로로 이 드라이버의 구조체로 되돌린 뒤 실제 일을 넘긴다.
 *
 * **dw_pcie 를 구조체 맨 앞에 둔 덕분에** 그 변환이 성립한다. 이 파일이
 * drvdata 를 거치지 않고 container_of 를 쓰는 것은 pci-exynos.c 와 다른
 * 선택인데, 결과는 같다.
 *
 * 언제나 0 을 돌려준다 — 켜는 동작 자체가 실패할 여지가 없기 때문이다.
 * 링크가 실제로 서는지는 그 뒤 코어가 따로 확인한다.
 *
 * 실행 컨텍스트: 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수] → s32g_pcie_enable_ltssm()
 */
static int s32g_pcie_start_link(struct dw_pcie *pci)
{
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);

	s32g_pcie_enable_ltssm(s32g_pp);

	return 0;
}

/* [한국어]
 * s32g_pcie_stop_link - DWC 코어의 링크 정지 요청을 받는다
 *
 * @pci: DWC 코어의 문맥.
 *
 * 위 함수의 짝이며 반환값이 없다는 것이 다르다 — 코어의 콜백 규약이
 * 그렇다. 링크를 내리는 것은 실패할 수 없다는 전제다.
 *
 * **stop_link 를 제공하는 것** 자체가 이 파일의 특징이다. 같은 트리의
 * pci-exynos.c 는 이 콜백을 두지 않아, 코어가 링크를 내려야 할 때 할 수
 * 있는 일이 없다.
 *
 * 실행 컨텍스트: 코어의 해제·절전 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.stop_link == [이 함수] → s32g_pcie_disable_ltssm()
 */
static void s32g_pcie_stop_link(struct dw_pcie *pci)
{
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);

	s32g_pcie_disable_ltssm(s32g_pp);
}

static struct dw_pcie_ops s32g_pcie_ops = {
	/* [한국어] 링크를 세워야 할 때 코어가 이것을 부른다. */
	.start_link = s32g_pcie_start_link,
	/* [한국어] 링크를 내려야 할 때 부른다. 이 콜백을 두지 않는 글루도 많다. */
	.stop_link = s32g_pcie_stop_link,
};

/* Configure the AMBA AXI Coherency Extensions (ACE) interface */
/* [한국어]
 * s32g_pcie_reset_mstr_ace - 일관성 경계를 못 박아 MSI 유실을 막는다
 *
 * @pci: DWC 코어의 문맥. DBI 접근에 쓴다.
 *
 * **이 파일에서 가장 중요한 함수다.** 상단 블록의 "Ncore 일관성 경계" 와
 * 옆의 상류 주석이 배경을 밝히고 있다.
 *
 * 요약하면 이렇다. S32G 의 주변장치는 슬레이브로서 캐시 일관성을 갖지
 * 않는데, PCIe 쪽에서 일관성 있는 트랜잭션(예: NoSnoop=0 인 MSI)이 그 주소로
 * 향하면 Ncore 인터커넥트가 **조용히 버린다.** 그래서 컨트롤러에 주소
 * 경계를 알려 그 아래로 가는 것은 비일관성으로 처리하게 한다.
 *
 * 세 단계다.
 * 1. DBI 의 읽기 전용 보호를 푼다. 이 레지스터들이 읽기 전용이라
 *    그러지 않으면 쓰기가 먹히지 않는다.
 * 2. 제어 레지스터 3 을 0 으로 지우고, 1 과 2 에 경계 주소의 하위·상위
 *    32비트를 각각 넣는다.
 * 3. 보호를 다시 건다.
 *
 * 이름의 ACE 는 AMBA AXI Coherency Extensions 로, ARM 버스 규격의 일관성
 * 확장이다. mstr 은 마스터 쪽 인터페이스를 가리킨다.
 *
 * **경계값이 코드에 박혀 있다.** 디바이스 트리에서 읽지 않으므로,
 * DDR 시작 주소가 다른 보드가 나오면 이 상수를 고쳐야 한다.
 *
 * 실행 컨텍스트: host_init 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. DBI 쓰기의 성패를 확인하지 않는다.
 *
 * 호출 체인:
 *   s32g_init_pcie_controller() → [이 함수]
 *     → dw_pcie_dbi_ro_wr_en() → dw_pcie_writel_dbi() ×3
 *     → dw_pcie_dbi_ro_wr_dis()
 */
static void s32g_pcie_reset_mstr_ace(struct dw_pcie *pci)
{
	/* [한국어] 경계 주소의 하위 32비트. 0x80000000 이 그대로 들어간다. */
	u32 ddr_base_low = lower_32_bits(S32G_MEMORY_BOUNDARY_ADDR);
	/* [한국어] 상위 32비트. 경계값이 32비트 범위 안이라 이 값은 0 이다. */
	u32 ddr_base_high = upper_32_bits(S32G_MEMORY_BOUNDARY_ADDR);

	/* [한국어] DBI 의 읽기 전용 보호를 푼다. 일관성 제어 레지스터들이 읽기 전용이라
	 * 이것 없이는 아래 쓰기가 먹히지 않는다. */
	dw_pcie_dbi_ro_wr_en(pci);
	/* [한국어] 제어 레지스터 3 을 지운다. 경계를 새로 세우기 전에 이전 설정을
	 * 없애는 것으로 보이며, 그 레지스터의 비트별 의미는 이 트리에서 확인 못 함. */
	dw_pcie_writel_dbi(pci, COHERENCY_CONTROL_3_OFF, 0x0);

	/*
	 * Ncore is a cache-coherent interconnect module that enables the
	 * integration of heterogeneous coherent and non-coherent agents in
	 * the chip. Ncore transactions to peripheral should be non-coherent
	 * or it might drop them.
	 *
	 * One example where this is needed are PCIe MSIs, which use NoSnoop=0
	 * and might end up routed to Ncore. PCIe coherent traffic (e.g. MSIs)
	 * that targets peripheral space will be dropped by Ncore because
	 * peripherals on S32G are not coherent as slaves. We add a hard
	 * boundary in the PCIe controller coherency control registers to
	 * separate physical memory space from peripheral space.
	 *
	 * Define the start of DDR as seen by Linux as this boundary between
	 * "memory" and "peripherals", with peripherals being below.
	 */
	dw_pcie_writel_dbi(pci, COHERENCY_CONTROL_1_OFF,
			   (ddr_base_low & CFG_MEMTYPE_BOUNDARY_LOW_ADDR_MASK));
	dw_pcie_writel_dbi(pci, COHERENCY_CONTROL_2_OFF, ddr_base_high);
	/* [한국어] 읽기 전용 보호를 다시 건다. **반드시 짝을 맞춰야** 이후의 실수로
	 * 읽기 전용 레지스터가 덮이지 않는다. */
	dw_pcie_dbi_ro_wr_dis(pci);
}

/* [한국어]
 * s32g_init_pcie_controller - 루트 포트 모드로 세우고 일관성·디스큐·EQ 를 설정한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 언제나 0.
 *
 * DWC 코어가 링크를 세우기 **전에** 부르는 초기화 콜백이며, 링크가 선 뒤에는
 * 바꿀 수 없는 설정들이 여기 모여 있다.
 *
 * 네 묶음이다.
 * 1. **동작 모드.** 장치 종류 필드를 루트 포트로 놓고, SRIS 모드 비트를
 *    지워 기본 클럭 방식(CRNS)을 쓴다. 두 변경을 한 값에 모아 한 번에 쓴다.
 * 2. **일관성 경계.** 위 함수를 부른다. 옆의 상류 주석이 밝히듯 "혹시 설정이
 *    리셋값에서 바뀌었을 경우" 를 대비해 다시 세우는 것이다.
 * 3. **SRIS 디스큐.** PORT_FORCE 레지스터의 디스큐 강제 비트를 켠다.
 *    1번에서 SRIS 모드를 껐는데 SRIS 용 디스큐를 켜는 셈이라 어긋나 보이지만,
 *    그 조합이 의도된 것인지는 이 트리에서 확인 못 함.
 * 4. **Gen3 이퀄라이제이션.** EQ 페이즈 2·3 관련 비트를 켠다. Gen3 속도에서
 *    신호 품질을 맞추는 절차이며, 구체적 의미는 DWC 문서 소관이라
 *    이 트리에서 확인 못 함.
 *
 * 3·4번은 DBI 쓰기라 읽기 전용 보호를 풀고 감싼다. 2번은 그 함수가 자기
 * 안에서 따로 감싸므로 **보호를 푼 구간이 두 번 나뉜다** — 한 번에 묶지
 * 않은 이유는 코드에 적혀 있지 않다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 0 을 돌려주므로 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [이 함수]
 *     → s32g_pcie_readl_ctrl() → s32g_pcie_writel_ctrl()
 *     → s32g_pcie_reset_mstr_ace() → dw_pcie_readl_dbi()/writel_dbi()
 */
static int s32g_init_pcie_controller(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct s32g_pcie *s32g_pp = to_s32g_from_dw_pcie(pci);
	/* [한국어] 레지스터 값을 읽어 고칠 자리. */
	u32 val;

	/* Set RP mode */
	val = s32g_pcie_readl_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_1);
	val &= ~DEVICE_TYPE_MASK;
	/* [한국어] 장치 종류를 루트 포트로 놓는다. 이 값이 컨트롤러의 동작 방식을
	 * 근본적으로 가른다 — 엔드포인트 모드였다면 전혀 다른 초기화가 필요하다. */
	val |= FIELD_PREP(DEVICE_TYPE_MASK, PCI_EXP_TYPE_ROOT_PORT);

	/* Use default CRNS */
	/* [한국어] SRIS 모드를 끈다 — 옆의 상류 주석대로 기본 클럭 방식(CRNS,
	 * Common Reference clock with No Spread)을 쓰겠다는 뜻이다. */
	val &= ~SRIS_MODE;

	/* [한국어] 모드 변경 둘을 **한 번에** 쓴다. 장치 종류와 SRIS 를 따로 쓰면
	 * 중간 상태가 잠시 하드웨어에 보이게 된다. */
	s32g_pcie_writel_ctrl(s32g_pp, PCIE_S32G_PE0_GEN_CTRL_1, val);

	/*
	 * Make sure we use the coherency defaults (just in case the settings
	 * have been changed from their reset values)
	 */
	s32g_pcie_reset_mstr_ace(pci);

	dw_pcie_dbi_ro_wr_en(pci);

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_FORCE);
	/* [한국어] SRIS 용 디스큐 강제 비트를 켠다. 위에서 SRIS 모드를 껐는데도 켜는
	 * 것이라 어긋나 보이나, 그 조합의 근거는 이 트리에서 확인 못 함. */
	val |= PORT_FORCE_DO_DESKEW_FOR_SRIS;
	/* [한국어] 고친 값을 되쓴다. */
	dw_pcie_writel_dbi(pci, PCIE_PORT_FORCE, val);

	val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
	/* [한국어] Gen3 이퀄라이제이션의 페이즈 2·3 관련 비트를 켠다. Gen3 속도에서
	 * 송수신 신호를 맞추는 절차이며, 세부는 DWC 문서 소관이라 확인 못 함. */
	val |= GEN3_RELATED_OFF_EQ_PHASE_2_3;
	/* [한국어] 고친 값을 되쓴다. */
	dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);

	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static const struct dw_pcie_host_ops s32g_pcie_host_ops = {
	/* [한국어] 링크를 세우기 전에 코어가 이 콜백 하나를 부른다. */
	.init = s32g_init_pcie_controller,
};

/* [한국어]
 * s32g_init_pcie_phy - 포트 리스트를 돌며 SerDes PHY 를 세운다
 *
 * @s32g_pp: 드라이버 상태. 포트 리스트를 담고 있다.
 * @return: 0 = 성공, 음수 오류.
 *
 * 포트마다 세 단계를 밟는다 — 초기화, PCIe 모드 지정, 전원 켜기.
 * SerDes 는 PCIe 말고 다른 프로토콜로도 쓸 수 있는 물리 계층이라,
 * 가운데의 모드 지정이 필요하다.
 *
 * **되감기가 이 함수에서 가장 볼 만한 부분이다.** 라벨이 둘이고 아래로
 * 흘러내리게(fall-through) 배치돼 있다.
 *   - 초기화가 실패하면: 그 PHY 는 아직 열리지 않았으므로 곧바로
 *     두 번째 라벨로 가 **앞서 성공한 것들만** 되감는다.
 *   - 모드 지정이나 전원 켜기가 실패하면: 그 PHY 는 이미 열려 있으므로
 *     첫 번째 라벨에서 그것을 닫고, 이어서 두 번째 라벨로 흘러내려
 *     앞엣것들을 되감는다.
 * `list_for_each_entry_continue_reverse` 가 **실패한 원소 바로 앞** 부터
 * 역순으로 도는 것이 이 배치를 성립시킨다.
 *
 * 마지막에 리스트를 통째로 비운다. 원소들은 devm 할당이라 여기서 해제되지
 * 않고 연결만 끊긴다 — 이후 해제 경로가 이미 되감은 PHY 를 다시 건드리지
 * 않게 하려는 것이다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 PHY 조작이 잠들 수 있다.
 *
 * 에러 경로: 세 단계 어디서 실패하든 기록을 남기고 되감은 뒤 그 오류를
 * 올려보낸다. probe 는 그것을 받아 런타임 PM 참조까지 되돌린다.
 *
 * 호출 체인:
 *   s32g_pcie_init() → [이 함수]
 *     → phy_init() → phy_set_mode_ext() → phy_power_on()
 *   (실패 시) → phy_exit() → phy_power_off() → list_del()
 */
static int s32g_init_pcie_phy(struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	struct device *dev = pci->dev;
	/* [한국어] 순회 커서와, 리스트를 비울 때 쓸 임시 포인터. */
	struct s32g_pcie_port *port, *tmp;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	list_for_each_entry(port, &s32g_pp->ports, list) {
		/* [한국어] PHY 를 연다. 클럭·전원 같은 기반 자원이 여기서 잡힌다. */
		ret = phy_init(port->phy);
		/* [한국어] 초기화 실패다. **이 PHY 는 열리지 않았으므로 닫을 필요가 없다.** */
		if (ret) {
			/* [한국어] 실패를 알린다. */
			dev_err(dev, "Failed to init serdes PHY\n");
			/* [한국어] 그래서 닫기 라벨을 건너뛰고 앞엣것들만 되감는 라벨로 간다. */
			goto err_phy_revert;
		}

		ret = phy_set_mode_ext(port->phy, PHY_MODE_PCIE, 0);
		/* [한국어] 모드 지정 실패다. 이 PHY 는 이미 열려 있다. */
		if (ret) {
			/* [한국어] 실패를 알린다. */
			dev_err(dev, "Failed to set mode on serdes PHY\n");
			/* [한국어] 그래서 이 PHY 를 닫는 라벨로 간다. */
			goto err_phy_exit;
		}

		ret = phy_power_on(port->phy);
		/* [한국어] 전원 켜기 실패다. 역시 이 PHY 는 열려 있다. */
		if (ret) {
			/* [한국어] 실패를 알린다. */
			dev_err(dev, "Failed to power on serdes PHY\n");
			/* [한국어] 닫는 라벨로 간다. */
			goto err_phy_exit;
		}
	}

	return 0;

err_phy_exit:
	phy_exit(port->phy);

err_phy_revert:
	list_for_each_entry_continue_reverse(port, &s32g_pp->ports, list) {
		phy_power_off(port->phy);
		phy_exit(port->phy);
	}

	list_for_each_entry_safe(port, tmp, &s32g_pp->ports, list)
		/* [한국어] 리스트에서 뗀다. 원소 자체는 devm 할당이라 여기서 해제되지 않고
		 * 연결만 끊긴다 — 이미 되감은 PHY 를 나중에 또 건드리지 않게 하려는 것이다. */
		list_del(&port->list);

	return ret;
}

/* [한국어]
 * s32g_deinit_pcie_phy - 포트 리스트를 돌며 PHY 를 끄고 리스트를 비운다
 *
 * @s32g_pp: 드라이버 상태.
 *
 * 위 함수의 짝이며, 순회하면서 원소를 지우므로 `_safe` 판을 쓴다 —
 * 그러지 않으면 지운 원소에서 다음을 찾으려다 깨진다.
 *
 * 포트마다 전원을 끄고 PHY 를 닫은 뒤 리스트에서 뗀다. 순서가 중요하다 —
 * 닫기 전에 전원을 내려야 한다.
 *
 * 원소 자체는 devm 할당이라 해제하지 않는다.
 *
 * **부르는 자리가 하나뿐** 이다. 이 드라이버에 remove 콜백이 없어,
 * s32g_pcie_deinit() 을 거쳐 probe 실패 경로에서만 닿는다.
 *
 * 실행 컨텍스트: probe 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   s32g_pcie_deinit() → [이 함수] → phy_power_off() → phy_exit() → list_del()
 */
static void s32g_deinit_pcie_phy(struct s32g_pcie *s32g_pp)
{
	struct s32g_pcie_port *port, *tmp;

	list_for_each_entry_safe(port, tmp, &s32g_pp->ports, list) {
		/* [한국어] 전원을 먼저 내린다. 닫기 전에 해야 한다. */
		phy_power_off(port->phy);
		phy_exit(port->phy);
		list_del(&port->list);
	}
}

/* [한국어]
 * s32g_pcie_init - LTSSM 을 끄고 PHY 를 세운다
 *
 * @dev: 이 컨트롤러의 device.
 * @s32g_pp: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 두 줄짜리 묶음 함수다.
 *
 * **LTSSM 을 먼저 끄는 것** 이 요점이다. 부트로더가 링크를 이미 세워 두었을
 * 수 있으므로, 알려진 정지 상태에서 PHY 설정을 시작한다. 링크가 살아 있는
 * 채로 PHY 를 건드리면 그 결과를 예측할 수 없다.
 *
 * [상류 코드 관찰] 첫 인자 dev 를 함수 본문에서 한 번도 쓰지 않는다.
 * 필요한 device 는 s32g_pp 안의 pci.dev 로도 닿을 수 있어, 인자가 남은
 * 것으로 보인다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: PHY 초기화 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   s32g_pcie_probe() → [이 함수]
 *     → s32g_pcie_disable_ltssm() → s32g_init_pcie_phy()
 */
static int s32g_pcie_init(struct device *dev, struct s32g_pcie *s32g_pp)
{
	s32g_pcie_disable_ltssm(s32g_pp);

	return s32g_init_pcie_phy(s32g_pp);
}

/* [한국어]
 * s32g_pcie_deinit - LTSSM 을 끄고 PHY 를 내린다
 *
 * @s32g_pp: 드라이버 상태.
 *
 * s32g_pcie_init() 의 짝이며 같은 순서를 따른다 — 링크를 먼저 내리고
 * 그다음에 PHY 를 건드린다.
 *
 * 이 드라이버에 remove 콜백이 없으므로 **probe 의 되감기 경로에서만**
 * 불린다. 즉 한 번 성공적으로 붙은 컨트롤러는 이 함수를 거치지 않는다.
 *
 * 실행 컨텍스트: probe 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   s32g_pcie_probe() 의 err_pcie_deinit → [이 함수]
 *     → s32g_pcie_disable_ltssm() → s32g_deinit_pcie_phy()
 */
static void s32g_pcie_deinit(struct s32g_pcie *s32g_pp)
{
	s32g_pcie_disable_ltssm(s32g_pp);

	s32g_deinit_pcie_phy(s32g_pp);
}

/* [한국어]
 * s32g_pcie_parse_port - 포트 자식 노드 하나에서 PHY 와 레인 수를 읽는다
 *
 * @s32g_pp: 드라이버 상태. 포트를 이 안의 리스트에 붙인다.
 * @node: 디바이스 트리의 포트 자식 노드.
 * @return: 0 = 성공, 음수 오류.
 *
 * 포트 구조체를 하나 만들어 PHY 핸들을 담고 리스트에 붙인다.
 *
 * **num-lanes 를 이 파일이 직접 읽는 이유** 는 옆의 상류 주석이 밝힌다 —
 * DWC 코어의 초기화 코드가 아직 루트 포트 노드의 그 속성을 해석하지 못해서,
 * S32G 가 루트 포트 하나만 지원하는 김에 드라이버가 대신 읽어 코어의
 * num_lanes 필드에 넣어 준다는 것이다. 즉 **임시 우회로** 임을 상류가
 * 스스로 밝히고 있다.
 *
 * 속성이 없으면 그냥 두는데, 그 경우 코어가 기본값을 쓴다.
 *
 * dev_err_probe() 를 쓰는 것이 눈에 띈다. -EPROBE_DEFER 일 때는 오류를
 * 기록하지 않고 조용히 넘겨, 부팅 로그가 지저분해지는 것을 막는다.
 *
 * INIT_LIST_HEAD 뒤에 곧바로 list_add_tail 을 하는데, 붙이는 쪽이 앞의
 * 초기화를 덮어쓰므로 그 한 줄은 사실상 방어적이다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, PHY 확보 실패는 그 오류를 올려보내며,
 * 호출자가 리스트를 비운다.
 *
 * 호출 체인:
 *   s32g_pcie_parse_ports() → [이 함수]
 *     → devm_kzalloc() → devm_of_phy_get() → list_add_tail()
 *     → of_property_read_u32()
 */
static int s32g_pcie_parse_port(struct s32g_pcie *s32g_pp, struct device_node *node)
{
	struct device *dev = s32g_pp->pci.dev;
	struct s32g_pcie_port *port;
	/* [한국어] 디바이스 트리에서 읽을 레인 수를 담을 자리. */
	int num_lanes;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!port)
		/* [한국어] 호출자가 리스트를 비우고 그대로 올려보낸다. */
		return -ENOMEM;

	port->phy = devm_of_phy_get(dev, node, NULL);
	/* [한국어] PHY 를 못 얻었다. */
	if (IS_ERR(port->phy))
		/* [한국어] dev_err_probe 를 쓴다 — -EPROBE_DEFER 일 때는 기록을 남기지 않아
		 * 부팅 로그가 지저분해지지 않는다. */
		return dev_err_probe(dev, PTR_ERR(port->phy),
				"Failed to get serdes PHY\n");

	/* [한국어] 연결 고리를 초기화한다. 바로 다음 줄이 덮어쓰므로 사실상 방어적인
	 * 한 줄이다. */
	INIT_LIST_HEAD(&port->list);
	/* [한국어] 리스트 꼬리에 붙인다. 디바이스 트리에 적힌 순서가 그대로 유지된다. */
	list_add_tail(&port->list, &s32g_pp->ports);

	/*
	 * The DWC core initialization code cannot yet parse the num-lanes
	 * attribute in the Root Port node. The S32G only supports one Root
	 * Port for now so its driver can parse the node and set the num_lanes
	 * field of struct dwc_pcie before calling dw_pcie_host_init().
	 */
	/* [한국어] 레인 수 속성을 읽는다. 옆의 상류 주석이 밝히듯 **DWC 코어가 아직
	 * 이 속성을 해석하지 못해** 드라이버가 대신 읽는 임시 우회로다. */
	if (!of_property_read_u32(node, "num-lanes", &num_lanes))
		/* [한국어] 읽은 값을 코어의 필드에 직접 넣는다. 링크를 세우기 전이라 유효하다. */
		s32g_pp->pci.num_lanes = num_lanes;

	/* [한국어] 여기까지 오면 이 포트는 준비가 끝났다. */
	return 0;
}

/* [한국어]
 * s32g_pcie_parse_ports - 디바이스 트리의 "pci" 타입 자식만 골라 훑는다
 *
 * @dev: 이 컨트롤러의 device. 자식 노드를 찾는 기준이다.
 * @s32g_pp: 드라이버 상태.
 * @return: 0 = 하나 이상 성공, 음수 오류.
 *
 * 자식 노드를 돌면서 device_type 이 "pci" 인 것만 포트로 취급한다.
 * 그렇지 않은 자식(예: 다른 용도의 서브노드)은 건너뛴다.
 *
 * **초기값이 -ENOENT 인 것** 이 이 함수의 요령이다. 자격을 갖춘 자식이
 * 하나도 없으면 그 값이 그대로 돌아가 "포트를 찾지 못했다" 가 된다.
 * 하나라도 성공하면 0 으로 덮인다.
 *
 * [상류 코드 관찰] 그 방식의 부작용으로, 반환값은 **마지막으로 처리한
 * 포트의 결과** 다. 앞의 포트가 실패했더라도 뒤엣것이 성공하면 0 이 돌아갈
 * 수 있는 구조인데, 실패 시 곧바로 break 하므로 실제로는 그 경로가 생기지
 * 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * _scoped 순회 판을 쓰므로 자식 노드의 참조를 손수 내려놓을 필요가 없다.
 *
 * 실패하면 그때까지 붙인 포트를 리스트에서 모두 뗀다. devm 할당이라
 * 메모리는 그대로 남고 연결만 끊긴다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 자격 있는 자식이 없으면 -ENOENT, 파싱이 실패하면 그 오류를
 * 올려보내며, 어느 쪽이든 리스트를 비운 뒤 돌아간다.
 *
 * 호출 체인:
 *   s32g_pcie_get_resources() → [이 함수]
 *     → of_node_is_type() → s32g_pcie_parse_port() → list_del()
 */
static int s32g_pcie_parse_ports(struct device *dev, struct s32g_pcie *s32g_pp)
{
	struct s32g_pcie_port *port, *tmp;
	int ret = -ENOENT;

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		/* [한국어] device_type 이 "pci" 인 자식만 포트로 본다. 다른 용도의 서브노드를
		 * 잘못 집는 것을 막는다. */
		if (!of_node_is_type(of_port, "pci"))
			/* [한국어] 자격 없는 자식은 건너뛴다. */
			continue;

		ret = s32g_pcie_parse_port(s32g_pp, of_port);
		/* [한국어] 이 포트를 파싱하지 못했다. */
		if (ret)
			/* [한국어] 곧바로 중단한다. 그래서 ret 이 마지막 결과라는 구조에도 불구하고
			 * 실패가 성공으로 덮이는 일은 생기지 않는다. */
			break;
	}

	if (ret)
		/* [한국어] 실패했으므로 그때까지 붙인 포트를 모두 뗀다. */
		list_for_each_entry_safe(port, tmp, &s32g_pp->ports, list)
			/* [한국어] 연결만 끊는다. 메모리는 devm 이 관리한다. */
			list_del(&port->list);

	return ret;
}

/* [한국어]
 * s32g_pcie_get_resources - 제어 레지스터 창을 잡고 포트들을 훑는다
 *
 * @pdev: 플랫폼 장치.
 * @s32g_pp: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * probe 가 쓸 자원을 모으는 앞단이다.
 *
 * 먼저 DWC 코어 문맥에 device 와 콜백 표를 심는다. 그 표를 여기서 거는
 * 덕분에, 아래 어느 단계에서 실패하더라도 코어가 잘못된 표를 볼 일이 없다.
 *
 * 레지스터 창을 **이름으로** 얻는다("ctrl"). 번호가 아니라 이름을 쓰므로
 * 디바이스 트리에서 창의 순서가 바뀌어도 안전하다.
 *
 * 포트 리스트를 초기화한 **뒤** 에 파싱을 부른다 — 순서가 반대면 리스트에
 * 붙이는 순간 깨진다.
 *
 * 마지막에 drvdata 를 매단다. 절전 콜백들이 그것으로 상태를 찾는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 창을 못 잡으면 그 오류를, 포트 파싱이 실패하면 dev_err_probe
 * 로 기록을 남기며 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   s32g_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname() → INIT_LIST_HEAD()
 *     → s32g_pcie_parse_ports() → platform_set_drvdata()
 */
static int s32g_pcie_get_resources(struct platform_device *pdev,
				   struct s32g_pcie *s32g_pp)
{
	struct dw_pcie *pci = &s32g_pp->pci;
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	pci->dev = dev;
	/* [한국어] 콜백 표를 **여기서** 건다. 아래 단계가 실패하더라도 코어가 잘못된
	 * 표를 볼 일이 없게 하려는 배치다. */
	pci->ops = &s32g_pcie_ops;

	s32g_pp->ctrl_base = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	/* [한국어] 제어 레지스터 창을 못 잡았다. */
	if (IS_ERR(s32g_pp->ctrl_base))
		/* [한국어] 그 오류를 그대로 올려보낸다. */
		return PTR_ERR(s32g_pp->ctrl_base);

	INIT_LIST_HEAD(&s32g_pp->ports);

	ret = s32g_pcie_parse_ports(dev, s32g_pp);
	/* [한국어] 포트를 하나도 찾지 못했거나 파싱이 실패했다. */
	if (ret)
		/* [한국어] dev_err_probe 로 기록을 남기며 그 오류를 올려보낸다. */
		return dev_err_probe(dev, ret,
				"Failed to parse Root Port: %d\n", ret);

	/* [한국어] 절전 콜백들이 상태를 찾을 수 있도록 매단다. **포트 파싱까지 끝난
	 * 뒤** 에 매다는 배치라, 중간 상태가 드러나지 않는다. */
	platform_set_drvdata(pdev, s32g_pp);

	/* [한국어] 자원 수집이 모두 끝났다. */
	return 0;
}

/* [한국어]
 * s32g_pcie_probe - 자원·전원·PHY 를 세우고 DWC 호스트를 등록한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며 다섯 단계다.
 *
 * 1. 상태 구조체 할당.
 * 2. 자원 수집(위 함수).
 * 3. **런타임 PM 참조 획득.** pm_runtime_no_callbacks() 로 이 장치에
 *    자체 절전 콜백이 없음을 알린 뒤, 참조를 하나 올려 장치를 켜 둔다.
 *    성공 경로에서 그 참조를 **되돌리지 않는다** — 컨트롤러가 계속 켜져
 *    있어야 하므로 의도된 것이다.
 * 4. LTSSM 정지와 PHY 초기화.
 * 5. DWC 호스트 등록. 그 안에서 host_init 콜백과 링크 훈련이 일어난다.
 *
 * 되감기 라벨이 둘이고 아래로 흘러내린다 — 호스트 등록이 실패하면 PHY 를
 * 되감고 이어서 PM 참조까지 내려놓고, 그보다 앞에서 실패하면 PM 참조만
 * 내려놓는다.
 *
 * use_atu_msg 를 참으로 두는 것은 ATU 를 통한 메시지 전송을 쓰겠다는
 * 뜻인데, 그 기능의 세부는 DWC 코어 소관이라 이 트리에서 확인 못 함.
 *
 * [상류 코드 관찰] pm_runtime_get_sync() 는 실패해도 참조 계수를 올리므로
 * 되감기의 pm_runtime_put() 이 짝을 이룬다. 다만 devm_pm_runtime_enable() 의
 * 반환값은 확인하지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 링크 대기와
 * 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 각 단계의 실패가 해당 라벨로 모여 되감고 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_kzalloc() → s32g_pcie_get_resources()
 *     → pm_runtime_get_sync() → s32g_pcie_init() → dw_pcie_host_init()
 */
static int s32g_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s32g_pcie *s32g_pp;
	/* [한국어] DWC 루트 포트 문맥을 가리킬 포인터. */
	struct dw_pcie_rp *pp;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	s32g_pp = devm_kzalloc(dev, sizeof(*s32g_pp), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!s32g_pp)
		/* [한국어] 아직 되감을 것이 없어 곧바로 돌아간다. */
		return -ENOMEM;

	ret = s32g_pcie_get_resources(pdev, s32g_pp);
	/* [한국어] 자원 수집이 실패했다. */
	if (ret)
		/* [한국어] 역시 되감을 것이 없다. 런타임 PM 은 아직 손대지 않았다. */
		return ret;

	pm_runtime_no_callbacks(dev);
	devm_pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	/* [한국어] 참조 획득 실패다. **음수만 실패로 본다** — 이 함수는 장치가 이미
	 * 켜져 있으면 양수를 돌려주는데 그것은 성공이다. */
	if (ret < 0)
		/* [한국어] 되감기로 간다. pm_runtime_get_sync() 는 실패해도 참조 계수를 올리므로
		 * put 이 짝을 이룬다. */
		goto err_pm_runtime_put;

	ret = s32g_pcie_init(dev, s32g_pp);
	/* [한국어] PHY 초기화가 실패했다. */
	if (ret)
		/* [한국어] PHY 쪽은 자기가 이미 되감았으므로 PM 참조만 내려놓으면 된다. */
		goto err_pm_runtime_put;

	pp = &s32g_pp->pci.pp;
	/* [한국어] 이 파일의 초기화 콜백을 코어에 건다. */
	pp->ops = &s32g_pcie_host_ops;
	/* [한국어] ATU 를 통한 메시지 전송을 쓰겠다고 알린다. 그 기능의 세부는
	 * DWC 코어 소관이라 이 트리에서 확인 못 함. */
	pp->use_atu_msg = true;

	ret = dw_pcie_host_init(pp);
	/* [한국어] 호스트 등록이 실패했다. 링크가 서지 않은 경우가 대표적이다. */
	if (ret)
		/* [한국어] PHY 와 PM 참조를 차례로 되감는 라벨로 간다. */
		goto err_pcie_deinit;

	return 0;

err_pcie_deinit:
	s32g_pcie_deinit(s32g_pp);
err_pm_runtime_put:
	pm_runtime_put(dev);

	return ret;
}

/* [한국어]
 * s32g_pcie_suspend_noirq - 절전 진입을 DWC 코어에 그대로 넘긴다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 코어가 돌려준 값.
 *
 * drvdata 에서 상태를 꺼내 DWC 코어의 공용 절전 함수를 부르는 것이 전부다.
 * SoC 고유로 더 할 일이 없다는 뜻이다.
 *
 * **LTSSM 을 끄지 않는다.** 링크 상태 저장과 복원을 코어의 공용 구현에
 * 맡기기 때문이며, 같은 트리의 pci-exynos.c 가 절전 진입에서 컨트롤러를
 * 직접 리셋하는 것과 정반대의 선택이다.
 *
 * noirq 단계에서 도는 이유는 코어의 구현이 그 단계를 요구하기 때문이다.
 *
 * 실행 컨텍스트: 시스템 절전의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 코어의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq == [이 함수] → dw_pcie_suspend_noirq()
 */
static int s32g_pcie_suspend_noirq(struct device *dev)
{
	/* [한국어] drvdata 에서 이 컨트롤러의 상태를 되찾는다. */
	struct s32g_pcie *s32g_pp = dev_get_drvdata(dev);
	/* [한국어] 코어에 넘길 DWC 문맥. */
	struct dw_pcie *pci = &s32g_pp->pci;

	return dw_pcie_suspend_noirq(pci);
}

/* [한국어]
 * s32g_pcie_resume_noirq - 절전 복귀를 DWC 코어에 그대로 넘긴다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 코어가 돌려준 값.
 *
 * 위 함수의 짝이며 구조가 같다.
 *
 * 복귀 과정에서 코어가 링크를 다시 세우는데, 그때 start_link 콜백을 통해
 * 이 파일의 LTSSM 켜기가 불린다. 즉 SoC 고유 동작은 콜백으로만 참여한다.
 *
 * pci-exynos.c 가 복귀 경로에서 초기화 순서를 손수 재현하는 것과 견주면,
 * 이쪽은 코어를 온전히 믿는 쪽이다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 코어의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq == [이 함수] → dw_pcie_resume_noirq()
 */
static int s32g_pcie_resume_noirq(struct device *dev)
{
	/* [한국어] drvdata 에서 상태를 되찾는다. */
	struct s32g_pcie *s32g_pp = dev_get_drvdata(dev);
	/* [한국어] 코어에 넘길 DWC 문맥. */
	struct dw_pcie *pci = &s32g_pp->pci;

	/* [한국어] 코어의 공용 복귀 구현에 맡긴다. 그 안에서 링크를 다시 세울 때
	 * 이 파일의 start_link 콜백이 불린다. */
	return dw_pcie_resume_noirq(pci);
}

static const struct dev_pm_ops s32g_pcie_pm_ops = {
	/* [한국어] 절전 진입·복귀를 noirq 단계에 건다. DWC 코어의 공용 구현이 그 단계를
	 * 전제한다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(s32g_pcie_suspend_noirq,
				  s32g_pcie_resume_noirq)
};

static const struct of_device_id s32g_pcie_of_match[] = {
	/* [한국어] 이 드라이버가 지원하는 유일한 SoC. */
	{ .compatible = "nxp,s32g2-pcie" },
	/* [한국어] 표의 끝 표시. */
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, s32g_pcie_of_match);

static struct platform_driver s32g_pcie_driver = {
	/* [한국어] 드라이버 이름과 매칭 표, 절전 연결. */
	.driver = {
		/* [한국어] sysfs 에 보일 이름. */
		.name	= "s32g-pcie",
		/* [한국어] 위의 디바이스 트리 매칭 표를 건다. */
		.of_match_table = s32g_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&s32g_pcie_pm_ops),
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = s32g_pcie_probe,
};

builtin_platform_driver(s32g_pcie_driver);

MODULE_AUTHOR("Ionut Vicovan <Ionut.Vicovan@nxp.com>");
MODULE_DESCRIPTION("NXP S32G PCIe Host controller driver");
MODULE_LICENSE("GPL");
