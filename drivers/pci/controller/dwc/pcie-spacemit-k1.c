// SPDX-License-Identifier: GPL-2.0
/*
 * SpacemiT K1 PCIe host driver
 *
 * Copyright (C) 2025 by RISCstar Solutions Corporation.  All rights reserved.
 * Copyright (c) 2023, spacemit Corporation.
 */

/*
 * [한국어 설명] SpacemiT K1(RISC-V) SoC 의 DesignWare PCIe 호스트 글루 (pcie-spacemit-k1.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 SpacemiT K1 SoC 에 붙이는 글루 드라이버다.
 * config 접근, ATU 설정, MSI, 버스 스캔은 모두 pcie-designware-host.c 가
 * 하고, 이 파일은 DWC 코어가 알 수 없는 SoC 고유의 것만 맡는다.
 *
 * 이 SoC 의 특징은 **제어 레지스터가 두 곳에 나뉘어 있다는 것** 이다.
 *   link — 이 컨트롤러 전용 창. 링크 상태와 인터럽트 허용 비트가 여기 있고,
 *          ioremap 으로 직접 매핑한다.
 *   APMU — SoC 전체의 전원·클럭 관리 블록. LTSSM, PERST#, PHY 리셋 유지,
 *          장치 종류(RC/EP) 같은 근본적인 제어가 여기 있으며, 다른
 *          드라이버와 공유하는 블록이라 syscon regmap 으로 접근한다.
 * 그래서 이 파일의 함수 대부분이 그 두 통로를 오간다 — 링크를 켜는 한
 * 동작만 봐도 APMU 의 LTSSM 비트와 link 창의 인터럽트 비트를 함께 건드린다.
 *
 * 맡는 일이 다섯이다.
 *   1) 소프트 리셋 토글과 클럭·리셋 관리.
 *   2) 벤더·장치 ID 를 config 공간에 직접 써 넣기. DWC 코어의 기본값이
 *      Synopsys 의 것이라 SoC 의 값으로 덮어야 한다.
 *   3) PERST# 순서 지키기 — 어서트하고 규격이 정한 시간을 기다린 뒤 푼다.
 *   4) 링크 시작·중단과 링크 상태 판정.
 *   5) ASPM L1 비활성화. 아래에 적었듯 이것은 우회책이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> k1_pcie_probe()
 *     -> APMU regmap 과 link 창을 얻고, PHY 를 리셋에 붙잡아 둔다
 *     -> 3.3V 전원을 켠다
 *     -> 루트 포트 자식 노드에서 PHY 를 얻는다
 *     -> dw_pcie_host_init()  [pcie-designware-host.c]
 *        -> 그 안에서 콜백 -> [이 파일] k1_pcie_init()
 *           -> 소프트 리셋 토글, 클럭·리셋 해제, ID 쓰기,
 *              PERST# 어서트 -> 100ms 대기 -> RC 모드 지정 -> PHY 초기화
 *              -> PERST# 해제 -> ASPM L1 끄기
 *        -> 코어가 start_link 콜백을 부른다 -> [이 파일] PHY 리셋 해제 +
 *           LTSSM 켜기 + 인터럽트 허용
 *        -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 이 파일에는 인터럽트 핸들러가
 * 없다 — MSI 는 DWC 코어가 다루고, 이 파일은 그 인터럽트가 CPU 까지 오도록
 * 관문 비트만 열어 준다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c. 접점은 두 벌의 콜백
 *   표다 — dw_pcie_ops(link_up, start_link, stop_link)와
 *   dw_pcie_host_ops(init, deinit). 클럭과 리셋도 이 파일이 직접 얻지 않고
 *   DWC 코어가 REQ_RES 능력 표시를 보고 대신 얻어 준다.
 * 옆쪽: syscon(APMU), PHY, regulator, reset 계층. 모두 이 트리에 없어
 *   그쪽 내부는 확인 대상 밖이며, 이 파일에서 읽히는 호출 규약까지만 적었다.
 *
 * 데이터 흐름:
 *   디바이스 트리(APMU phandle + 오프셋, "link" 자원, PHY, 3.3V 공급) -> probe
 *     -> struct k1_pcie
 *   APMU 접근이 언제나 pmu_off + 레지스터 오프셋 형태인 것이 요점이다 —
 *   APMU 블록 안에서 이 컨트롤러의 영역이 어디서 시작하는지를 트리가 알려 준다.
 *
 * 공유 상태: struct k1_pcie 하나. probe 후 불변이며 잠금이 없다.
 *   APMU 는 다른 드라이버와 공유하지만 그 동시 접근은 regmap 계층이 지킨다.
 *
 * === NVMe 관점 ===
 * 이 파일은 NVMe 를 이름으로 언급하는 드문 컨트롤러 드라이버다.
 * k1_pcie_disable_aspm_l1() 위의 상류 FIXME 가 그 이유를 밝힌다 — 일부
 * NVMe 드라이브에서 오류가 보고돼 ASPM L1 을 아예 못 쓰게 막아 두었다.
 * 즉 이 SoC 에 NVMe 를 붙이면 링크가 L1 절전에 들어가지 않으며,
 * 그만큼 전력을 더 쓰는 대신 안정성을 택한 것이다. 그 FIXME 는 이것이
 * 근본 원인을 고친 것이 아니라는 상류의 표시이기도 하다.
 *
 * === 주요 함수/구조체 요약 ===
 * k1_pcie_init()             : DWC 코어가 부르는 초기화 콜백. 이 파일의
 *                              순서 대부분이 여기 모여 있다.
 * k1_pcie_deinit()           : 그 짝. PERST# 를 걸고 PHY 와 자원을 내린다.
 * k1_pcie_start_link()       : PHY 리셋을 풀고 LTSSM 을 켜고 인터럽트를 연다.
 * k1_pcie_stop_link()        : 정확히 그 역순.
 * k1_pcie_disable_aspm_l1()  : ASPM L1 지원 표시를 지우는 우회책.
 * k1_pcie_parse_port()       : 루트 포트 자식 노드에서 PHY 를 얻는다.
 * struct k1_pcie             : dw_pcie 를 맨 앞에 둔 이 드라이버의 상태.
 *                              APMU regmap 과 그 안에서의 오프셋을 함께 든다.
 */

/* [한국어] clk_bulk_prepare_enable() 계열. 코어가 얻어 둔 클럭 묶음을 켜고 끈다. */
#include <linux/clk.h>
/* [한국어] mdelay(). 소프트 리셋과 PERST# 대기 두 곳이 쓴다. */
#include <linux/delay.h>
/* [한국어] dev_of_node() 와 dev_err_probe(). */
#include <linux/device.h>
/* [한국어] IS_ERR()/PTR_ERR(). */
#include <linux/err.h>
/* [한국어] GFP_KERNEL. devm_kzalloc 에 넘긴다. */
#include <linux/gfp.h>
/* [한국어] syscon_regmap_lookup_by_phandle_args(). APMU 블록을 찾는 통로다. */
#include <linux/mfd/syscon.h>
/* [한국어] struct of_device_id. */
#include <linux/mod_devicetable.h>
/* [한국어] phy_init()/phy_exit(). */
#include <linux/phy/phy.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] regmap_set_bits()/clear_bits()/update_bits(). APMU 접근이 모두 이것이다. */
#include <linux/regmap.h>
/* [한국어] reset_control_bulk_assert()/deassert(). 역시 코어가 얻어 둔 묶음을 다룬다. */
#include <linux/reset.h>
/* [한국어] u32 등 기본 타입.
 * **pm_runtime.h 와 regulator/consumer.h 는 이 목록에 없다** —
 * pm_runtime_set_active() 와 devm_regulator_get_enable() 을 쓰는데도
 * 직접 포함하지 않아 다른 헤더를 통해 전이적으로 얻는다.
 * 어느 헤더인지는 이 트리에 include/ 가 없어 확인 못 함. */
#include <linux/types.h>

/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_cap_set(), MAX_MSI_IRQS,
 * PCIE_T_PVPERL_MS 등. */
#include "pcie-designware.h"

/* [한국어] 이 SoC 의 PCI 벤더 ID. **DWC 코어의 기본값이 Synopsys 의 것이라**
 * k1_pcie_init() 이 이 값으로 덮어써야 한다. */
#define PCI_VENDOR_ID_SPACEMIT		0x201f
/* [한국어] 이 SoC 의 PCI 장치 ID. 위와 함께 config 공간에 직접 쓰인다. */
#define PCI_DEVICE_ID_SPACEMIT_K1	0x0001

/* Offsets and field definitions for link management registers */
/* [한국어] link 창의 최상위 인터럽트 관문 레지스터. */
#define K1_PHY_AHB_IRQ_EN			0x0000
/* [한국어] 그 관문 비트. start_link 가 열고 stop_link 가 닫는다. */
#define PCIE_INTERRUPT_EN		BIT(0)

/* [한국어] 링크 상태 레지스터. */
#define K1_PHY_AHB_LINK_STS			0x0004
/* [한국어] PHY 계층의 링크 확립 비트. */
#define SMLH_LINK_UP			BIT(1)
/* [한국어] 데이터 링크 계층의 링크 확립 비트. **두 비트를 모두** 봐야 링크가 선 것이다. */
#define RDLH_LINK_UP			BIT(12)

/* [한국어] 개별 인터럽트 허용 레지스터. */
#define INTR_ENABLE				0x0014
/* [한국어] MSI 인터럽트 허용 비트. 이 파일이 MSI 에 관여하는 유일한 자리다. */
#define MSI_CTRL_INT			BIT(11)

/* Some controls require APMU regmap access */
/* [한국어] 디바이스 트리에서 APMU 를 가리키는 속성 이름. */
#define SYSCON_APMU			"spacemit,apmu"

/* Offsets and field definitions for APMU registers */
/* [한국어] APMU 안의 클럭·리셋 제어 레지스터. 이 파일에서 가장 많이 쓰이는 자리다. */
#define PCIE_CLK_RESET_CONTROL			0x0000
/* [한국어] LTSSM 시작 비트. start_link 가 세우고 stop_link 가 지운다. */
#define LTSSM_EN			BIT(6)
/* [한국어] Vaux(3.3V) 가 있다고 알리는 비트. */
#define PCIE_AUX_PWR_DET		BIT(9)
/* [한국어] PERST# 출력 비트(옆의 상류 주석대로 1 이 어서트다) —
 * **이름과 반대로 세우면 리셋이 걸린다**는 점에 주의해야 한다. */
#define PCIE_RC_PERST			BIT(12)	/* 1: assert PERST# */
/* [한국어] PHY 를 리셋에 붙잡아 두는 비트. probe 가 세우고 start_link 가 지운다. */
#define APP_HOLD_PHY_RST		BIT(30)
/* [한국어] 장치 종류 비트(옆의 상류 주석대로 1 이 RC). 이 SoC 도 EP 로 동작할 수 있다. */
#define DEVICE_TYPE_RC			BIT(31)	/* 0: endpoint; 1: RC */

/* [한국어] APMU 안의 컨트롤러 로직 제어 레지스터. */
#define PCIE_CONTROL_LOGIC			0x0004
/* [한국어] 소프트 리셋 비트. 초기화의 맨 처음에 토글된다. */
#define PCIE_SOFT_RESET			BIT(0)

struct k1_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **맨 앞에 두어** to_dw_pcie_from_pp() 변환이 성립한다.
	 * 설정자: probe 가 dev·ops·벡터 수를 채우고, DWC 코어가 나머지를 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 모든 콜백.
	 * 값 범위: DWC 코어가 정의한 구조체. app_clks 와 app_rsts 는 REQ_RES 표시를
	 * 보고 코어가 대신 채워 준다.
	 * 동기화: 코어가 관리한다. */
	struct dw_pcie pci;
	/* [한국어] PCIe PHY.
	 * 설정자: k1_pcie_parse_port() 가 루트 포트 자식 노드에서 얻는다.
	 * 읽는 자: k1_pcie_init() 이 초기화하고 k1_pcie_deinit() 이 되돌린다.
	 * 값 범위: 유효한 PHY 포인터. 선택 사항이 아니라 없으면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	struct phy *phy;
	/* [한국어] 이 컨트롤러 전용 레지스터 창의 가상 주소.
	 * 설정자: probe 의 devm_platform_ioremap_resource_byname("link").
	 * 읽는 자: 링크 상태 판정과 인터럽트 허용 비트 조작.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변.
	 * APMU 와 달리 이 창은 이 컨트롤러만 쓴다. */
	void __iomem *link;
	/* [한국어] SoC 전체의 전원·클럭 관리 블록을 가리키는 regmap.
	 * 설정자: probe 의 syscon_regmap_lookup_by_phandle_args().
	 * 읽는 자: LTSSM, PERST#, PHY 리셋 유지, 장치 종류를 다루는 모든 자리.
	 * 값 범위: 유효한 regmap 포인터.
	 * 동기화: 여러 드라이버가 공유하는 블록이라 동시 접근은 regmap 계층이 지킨다.
	 * 옆의 상류 주석대로 **이 파일은 regmap 접근의 오류를 확인하지 않는다** —
	 * MMIO 로 뒷받침되는 regmap 이라 실패할 여지가 없다고 본 것이다. */
	struct regmap *pmu;	/* Errors ignored; MMIO-backed regmap */
	/* [한국어] APMU 블록 안에서 이 컨트롤러 영역의 시작 오프셋.
	 * 설정자: 위 syscon 조회가 디바이스 트리의 인자에서 함께 채워 준다.
	 * 읽는 자: 모든 APMU 접근이 이 값에 레지스터 오프셋을 더한다.
	 * 값 범위: APMU 블록 안의 유효한 오프셋.
	 * 동기화: probe 후 불변.
	 * APMU 를 여러 드라이버가 나눠 쓰므로 각자의 영역을 이렇게 지정한다. */
	u32 pmu_off;
/* [한국어] 이 드라이버의 상태 전부. */
};

/* [한국어] dw_pcie 포인터에서 이 드라이버의 상태를 되찾는 통로.
 * container_of 가 아니라 drvdata 를 두 번 거치는데 — device 에서
 * platform_device 로, 다시 drvdata 로 — 그 편이 dw_pcie 가 구조체
 * 맨 앞에 있다는 사실에 기대지 않아 더 안전하다.
 * 전제: probe 가 platform_set_drvdata 로 상태를 미리 심어 두어야 한다. */
#define to_k1_pcie(dw_pcie) \
		platform_get_drvdata(to_platform_device((dw_pcie)->dev))

/* [한국어]
 * k1_pcie_toggle_soft_reset - 컨트롤러 로직에 소프트 리셋을 걸었다 푼다
 *
 * @k1: 드라이버 상태.
 *
 * 초기화의 맨 처음에 불려, 이전 상태가 남아 있을 수 있는 컨트롤러를 깨끗한
 * 상태로 되돌린다.
 *
 * **쓰고 나서 되읽는 것** 이 이 함수의 요점이며, 위 상류 주석이 그 이유를
 * 밝힌다 — regmap 쓰기가 버퍼링될 수 있어, 되읽어야 그 쓰기가 실제로
 * 장치에 도달했음이 보장된다. 그러지 않으면 아래 2ms 대기가 리셋이 걸리기
 * 전에 시작될 수 있다.
 *
 * 같은 관용이 k1_pcie_init() 의 PERST# 어서트에서도 되풀이된다.
 *
 * APMU 접근이 언제나 pmu_off 를 더한 형태다 — 그 블록 안에서 이 컨트롤러의
 * 영역이 어디서 시작하는지를 디바이스 트리가 알려 주기 때문이다.
 *
 * 실행 컨텍스트: host_init 콜백. mdelay 가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다. regmap 접근의 오류를 확인하지 않는데, 구조체 필드의
 * 상류 주석이 그것을 의도된 선택으로 밝힌다.
 *
 * 호출 체인:
 *   k1_pcie_init() → [이 함수]
 *     → regmap_set_bits() → regmap_read() → mdelay() → regmap_clear_bits()
 */
static void k1_pcie_toggle_soft_reset(struct k1_pcie *k1)
{
	u32 offset;
	u32 val;
/* [한국어] 소프트 리셋 비트를 세운다. */

	/*
	 * Write, then read back to guarantee it has reached the device
	 * before we start the delay.
	 */
	offset = k1->pmu_off + PCIE_CONTROL_LOGIC;
	regmap_set_bits(k1->pmu, offset, PCIE_SOFT_RESET);
	/* [한국어] **되읽어 그 쓰기가 장치에 도달했음을 보장한다**(위 상류 주석) —
	 * regmap 쓰기가 버퍼링될 수 있어, 이것 없이는 아래 대기가 리셋이 걸리기
	 * 전에 시작될 수 있다. */
	regmap_read(k1->pmu, offset, &val);
/* [한국어] 2ms 를 기다린다. 리셋이 로직에 전파될 시간이다. */

	mdelay(2);

	regmap_clear_bits(k1->pmu, offset, PCIE_SOFT_RESET);
/* [한국어] 리셋을 풀었다. 이제 컨트롤러가 깨끗한 상태다. */
}

/* Enable app clocks, deassert resets */
/* [한국어]
 * k1_pcie_enable_resources - 애플리케이션 클럭을 켜고 리셋을 푼다
 *
 * @k1: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 클럭과 리셋을 이 파일이 직접 얻지 않는다는 점이 눈에 띈다. probe 가
 * dw_pcie_cap_set(REQ_RES) 로 능력을 표시해 두면 DWC 코어가 대신 얻어
 * pci->app_clks 와 pci->app_rsts 에 채워 주고, 이 함수는 그것을 켜고 풀기만 한다.
 *
 * 순서가 정해져 있다 — 클럭을 먼저 켜고 리셋을 나중에 푼다. 클럭 없이
 * 리셋을 풀면 로직이 동작할 수 없다.
 *
 * 되감기가 한 줄이다. 리셋 해제가 실패하면 방금 켠 클럭을 되돌린다.
 *
 * 옆의 상류 주석이 이 함수의 일을 한 줄로 요약한다.
 *
 * 실행 컨텍스트: host_init 콜백. 클럭 조작이 잠들 수 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 어느 단계가 실패하든 그 오류를 올려보내며, 앞 단계는 되돌린다.
 *
 * 호출 체인:
 *   k1_pcie_init() → [이 함수]
 *     → clk_bulk_prepare_enable() → reset_control_bulk_deassert()
 */
static int k1_pcie_enable_resources(struct k1_pcie *k1)
{
	struct dw_pcie *pci = &k1->pci;
	int ret;
/* [한국어] 먼저 클럭을 켠다 — 클럭 없이 리셋을 풀면 로직이 동작할 수 없다. */

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(pci->app_clks), pci->app_clks);
	/* [한국어] 켜지 못하면, */
	if (ret)
		/* [한국어] 그대로 물러난다. 되돌릴 것이 없다. */
		return ret;

	ret = reset_control_bulk_deassert(ARRAY_SIZE(pci->app_rsts),
					  /* [한국어] 코어가 얻어 둔 리셋들을 통째로 푼다. */
					  pci->app_rsts);
	if (ret)
		/* [한국어] 풀지 못하면 방금 켠 클럭을 되돌린다. */
		goto err_disable_clks;

	return 0;

err_disable_clks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(pci->app_clks), pci->app_clks);

	return ret;
}

/* Assert resets, disable app clocks */
/* [한국어]
 * k1_pcie_disable_resources - 리셋을 걸고 애플리케이션 클럭을 끈다
 *
 * @k1: 드라이버 상태.
 *
 * k1_pcie_enable_resources() 의 짝이며 순서가 정확히 반대다 — 리셋을 먼저
 * 걸고 클럭을 나중에 끈다.
 *
 * 그 순서여야 하는 이유는 켜는 쪽과 대칭이다. 클럭을 먼저 끄면 리셋 신호가
 * 로직에 전달되지 못한 채 멈춘다.
 *
 * 반환값이 없다. 두 해제 함수가 실패를 알리지 않기 때문이다.
 *
 * 옆의 상류 주석이 이 함수의 일을 한 줄로 요약한다.
 *
 * 실행 컨텍스트: deinit 콜백과 init 의 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   k1_pcie_deinit() / k1_pcie_init() 의 되감기 → [이 함수]
 *     → reset_control_bulk_assert() → clk_bulk_disable_unprepare()
 */
static void k1_pcie_disable_resources(struct k1_pcie *k1)
{
	struct dw_pcie *pci = &k1->pci;

	reset_control_bulk_assert(ARRAY_SIZE(pci->app_rsts), pci->app_rsts);
	/* [한국어] 리셋을 건 뒤에 클럭을 끈다 — 순서를 바꾸면 리셋 신호가 로직에
	 * 전달되지 못한 채 멈춘다. */
	clk_bulk_disable_unprepare(ARRAY_SIZE(pci->app_clks), pci->app_clks);
/* [한국어] 자원 정리 끝. */
}

/* FIXME: Disable ASPM L1 to avoid errors reported on some NVMe drives */
/* [한국어]
 * k1_pcie_disable_aspm_l1 - 링크 capability 에서 ASPM L1 지원 표시를 지운다
 *
 * @k1: 드라이버 상태.
 *
 * **우회책이다.** 위 상류 FIXME 가 그것을 명시하며, 일부 NVMe 드라이브에서
 * 오류가 보고돼 ASPM L1 을 아예 못 쓰게 막아 두었다고 적고 있다. 근본 원인이
 * 컨트롤러에 있는지 드라이브에 있는지는 그 주석이 밝히지 않으며, 이 트리에서
 * 확인할 수 없다.
 *
 * **제어가 아니라 capability 를 고치는 것** 이 이 함수의 방법이다. 소프트웨어가
 * ASPM 을 켜려 할 때 먼저 이 레지스터를 보고 "지원한다" 를 확인하는데,
 * 그 표시를 지워 두면 아무도 켜려 하지 않는다. 제어 레지스터를 끄는 것보다
 * 확실한데, 나중에 누가 켜도 다시 이 표시에 걸리기 때문이다.
 *
 * L0s 는 건드리지 않는다 — 문제가 된 것이 L1 뿐이라는 뜻이다.
 *
 * DBI 읽기 전용 쓰기 허용을 여는 것이 필수다. LNKCAP 은 하드웨어가 정하는
 * 읽기 전용 레지스터라, 그 보호를 풀지 않으면 쓰기가 무시된다. 연 뒤
 * 반드시 닫는 것도 마찬가지로 중요하다.
 *
 * 실행 컨텍스트: host_init 콜백의 마지막. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. capability 를 못 찾으면 offset 이 0 이 되어 엉뚱한 자리에
 * 쓰게 되지만, 그 경우를 검사하지 않는다.
 *
 * 호출 체인:
 *   k1_pcie_init() → [이 함수]
 *     → dw_pcie_find_capability() → dw_pcie_dbi_ro_wr_en()
 *     → dw_pcie_writel_dbi() → dw_pcie_dbi_ro_wr_dis()
 */
static void k1_pcie_disable_aspm_l1(struct k1_pcie *k1)
{
	struct dw_pcie *pci = &k1->pci;
	u8 offset;
	/* [한국어] 읽어서 고칠 LNKCAP 값. */
	u32 val;

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* [한국어] capability 시작에서 링크 capability 자리까지 더한다. */
	offset += PCI_EXP_LNKCAP;
/* [한국어] 이제 그 자리를 고칠 수 있다. */

	dw_pcie_dbi_ro_wr_en(pci);
	val = dw_pcie_readl_dbi(pci, offset);
	/* [한국어] **L1 지원 표시만** 지운다 — L0s 는 건드리지 않으므로,
	 * 문제가 된 것이 L1 뿐이라는 뜻이다. */
	val &= ~PCI_EXP_LNKCAP_ASPM_L1;
	/* [한국어] 되쓴다. 이제 소프트웨어가 이 링크에서 ASPM L1 을 켜려 하지 않는다. */
	dw_pcie_writel_dbi(pci, offset, val);
	/* [한국어] 읽기 전용 보호를 다시 건다 — 열어 두면 다른 경로의 실수가
	 * 읽기 전용 레지스터를 망칠 수 있다. */
	dw_pcie_dbi_ro_wr_dis(pci);
}

/* [한국어]
 * k1_pcie_init - 컨트롤러를 리셋에서 깨워 RC 모드로 세운다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 파일에서 가장 긴 함수이며, 내용이 사실상 **순서** 하나다.
 *
 * 일곱 단계다.
 * 1. 소프트 리셋을 걸었다 푼다 — 이전 상태를 지운다.
 * 2. 클럭을 켜고 리셋을 푼다.
 * 3. **벤더·장치 ID 를 config 공간에 직접 쓴다.** DWC 코어의 기본값이
 *    Synopsys 의 것이라, 그대로 두면 이 브리지가 Synopsys 장치로 보인다.
 * 4. PERST# 를 어서트하고 100ms 기다린다. 위 상류 주석이 근거를 밝히는데,
 *    PCI CEM 규격이 전원이 안정된 뒤 최소 그만큼 지나서 PERST# 를 풀라고
 *    정하고 있다. 여기서도 쓰고 되읽어 그 쓰기가 도달했음을 보장한 뒤에야
 *    대기를 시작한다.
 * 5. RC 모드를 지정하고 Vaux 가 있다고 알린다(위 상류 주석).
 * 6. PHY 를 초기화한다. **여기만 실패를 되돌린다** — 앞 단계들은 반환값이
 *    없거나 확인하지 않기 때문이다.
 * 7. PERST# 를 풀고 ASPM L1 을 막는다.
 *
 * 4번과 7번이 한 쌍이며, 그 사이가 하위 장치를 리셋에 붙잡아 둔 구간이다.
 * PHY 초기화가 그 구간 안에 있는 것이 요점으로, 하위 장치가 깨어나기 전에
 * PHY 가 준비돼 있어야 한다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. mdelay 와 PHY 초기화가 있어
 * 프로세스 컨텍스트여야 하며 100ms 이상 걸린다.
 *
 * 에러 경로: 자원 켜기 실패는 그대로 올려보내고, PHY 초기화 실패는 자원을
 * 되돌린 뒤 올려보낸다. 그때 PERST# 는 어서트된 채로 남는다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [이 함수]
 *     → k1_pcie_toggle_soft_reset() → k1_pcie_enable_resources()
 *     → dw_pcie_writew_dbi(ID) → regmap_set_bits(PERST#) → mdelay()
 *     → phy_init() → k1_pcie_disable_aspm_l1()
 */
static int k1_pcie_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1_pcie *k1 = to_k1_pcie(pci);
	/* [한국어] APMU 안의 제어 레지스터 오프셋. 이 함수에서 여러 번 쓰므로 미리 계산해 둔다. */
	u32 reset_ctrl;
	/* [한국어] 되읽기 결과를 버릴 자리. */
	u32 val;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 컨트롤러를 리셋에서 깨운다. */

	k1_pcie_toggle_soft_reset(k1);

	ret = k1_pcie_enable_resources(k1);
	/* [한국어] 자원 켜기가 실패하면, */
	if (ret)
		/* [한국어] 그대로 물러난다. */
		return ret;

	/* Set the PCI vendor and device ID */
	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, PCI_VENDOR_ID_SPACEMIT);
	/* [한국어] 장치 ID 도 SoC 의 것으로 덮는다. 이 둘을 쓰지 않으면
	 * 이 브리지가 Synopsys 장치로 보인다. */
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, PCI_DEVICE_ID_SPACEMIT_K1);
	/* [한국어] 읽기 전용 보호를 다시 건다. */
	dw_pcie_dbi_ro_wr_dis(pci);

	/*
	 * Start by asserting fundamental reset (drive PERST# low).  The
	 * PCI CEM spec says that PERST# should be deasserted at least
	 * 100ms after the power becomes stable, so we'll insert that
	 * delay first.  Write, then read it back to guarantee the write
	 * reaches the device before we start the delay.
	 */
	reset_ctrl = k1->pmu_off + PCIE_CLK_RESET_CONTROL;
	regmap_set_bits(k1->pmu, reset_ctrl, PCIE_RC_PERST);
	/* [한국어] 여기서도 되읽어 쓰기 도달을 보장한 뒤에야(위 상류 주석) 대기를 시작한다. */
	regmap_read(k1->pmu, reset_ctrl, &val);
	/* [한국어] PCI CEM 규격이 정한 시간을 기다린다 — 전원이 안정된 뒤 이만큼 지나야
	 * PERST# 를 풀 수 있다. */
	mdelay(PCIE_T_PVPERL_MS);

	/*
	 * Put the controller in root complex mode, and indicate that
	 * Vaux (3.3v) is present.
	 */
	regmap_set_bits(k1->pmu, reset_ctrl, DEVICE_TYPE_RC | PCIE_AUX_PWR_DET);

	ret = phy_init(k1->phy);
	/* [한국어] PHY 초기화가 실패하면 — */
	if (ret) {
		/* [한국어] **여기만 되돌린다.** 앞 단계들은 반환값이 없거나 확인하지 않아
		 * 되돌릴 대상이 이것뿐이다. PERST# 는 어서트된 채로 남는다. */
		k1_pcie_disable_resources(k1);

		return ret;
	}

	/* Deassert fundamental reset (drive PERST# high) */
	regmap_clear_bits(k1->pmu, reset_ctrl, PCIE_RC_PERST);

	/* Finally, as a workaround, disable ASPM L1 */
	k1_pcie_disable_aspm_l1(k1);

	return 0;
}

/* [한국어]
 * k1_pcie_deinit - PERST# 를 걸고 PHY 와 자원을 내린다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * k1_pcie_init() 의 짝이며 순서가 역순이다.
 *
 * PERST# 를 **가장 먼저** 건다. 하위 장치를 리셋에 넣어 두어야, 그 뒤 PHY 와
 * 클럭이 사라지는 동안 장치가 불안정한 신호를 보지 않는다.
 *
 * init 이 한 일 중 되돌리지 않는 것이 둘 있다 — 벤더·장치 ID 와 ASPM L1
 * 비활성화다. 둘 다 config 공간의 값이라 컨트롤러가 리셋되면 사라지고,
 * 다음 init 이 어차피 다시 쓰기 때문이다.
 *
 * 반환값이 없다. 정리 경로라 실패해도 호출자가 할 수 있는 일이 없다.
 *
 * 실행 컨텍스트: dw_pcie_host_deinit() 안. PHY 조작이 잠들 수 있어
 * 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_deinit() → dw_pcie_host_ops.deinit == [이 함수]
 *     → regmap_set_bits(PERST#) → phy_exit() → k1_pcie_disable_resources()
 */
static void k1_pcie_deinit(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1_pcie *k1 = to_k1_pcie(pci);
/* [한국어] PERST# 를 풀었으니 하위 장치가 깨어난다. */

	/* Assert fundamental reset (drive PERST# low) */
	regmap_set_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
			PCIE_RC_PERST);

	phy_exit(k1->phy);

	k1_pcie_disable_resources(k1);
}

static const struct dw_pcie_host_ops k1_pcie_host_ops = {
	/* [한국어] 초기화 콜백. */
	.init		= k1_pcie_init,
	/* [한국어] 정리 콜백. 이 SoC 는 정리할 것이 있어 deinit 도 둔다 —
	 * deinit 이 없는 글루 드라이버도 많다. */
	.deinit		= k1_pcie_deinit,
};

/* [한국어]
 * k1_pcie_link_up - 링크가 서 있는지 답한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: true = 링크가 섰다, false = 아니다.
 *
 * DWC 코어가 링크를 기다릴 때 반복해 부르는 콜백이다.
 *
 * **두 비트를 모두** 확인한다. PHY 계층의 링크(SMLH)와 데이터 링크 계층
 * (RDLH)이 따로 보고되는데, 둘 다 서야 config 접근이 가능하다.
 *
 * APMU 가 아니라 link 창에서 읽는다 — 링크 상태는 이 컨트롤러 전용
 * 정보라 공유 블록이 아닌 자기 창에 있다.
 *
 * _relaxed 판인 것은 이 읽기가 다른 메모리 접근과 순서를 맞출 필요가
 * 없기 때문이다.
 *
 * 실행 컨텍스트: DWC 코어의 링크 대기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.link_up == [이 함수] → readl_relaxed()
 */
static bool k1_pcie_link_up(struct dw_pcie *pci)
{
	struct k1_pcie *k1 = to_k1_pcie(pci);
	u32 val;
/* [한국어] 두 링크 계층의 상태를 한 번에 읽는다. */

	val = readl_relaxed(k1->link + K1_PHY_AHB_LINK_STS);
/* [한국어] **둘 다** 서야 config 접근이 가능하다 — PHY 만 선 상태는 훈련 중이다. */

	return (val & RDLH_LINK_UP) && (val & SMLH_LINK_UP);
}

/* [한국어]
 * k1_pcie_start_link - PHY 리셋을 풀고 링크 훈련을 시작하며 인터럽트를 연다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * 세 가지를 한다.
 * 1. **PHY 리셋 유지를 풀면서 동시에 LTSSM 을 켠다.** 두 비트를 한 번의
 *    regmap_update_bits 로 다루는 것이 요점으로, 마스크에 둘을 넣고 값에는
 *    LTSSM 만 넣어 하나는 지우고 하나는 세운다. 두 번에 나눠 쓰면 그 사이에
 *    PHY 가 리셋에서 나온 채로 훈련이 시작되지 않는 순간이 생긴다.
 * 2. link 창에서 MSI 인터럽트를 허용한다.
 * 3. link 창의 최상위 인터럽트 관문을 연다.
 *
 * 2번과 3번이 계층을 이룬다 — 개별 인터럽트를 허용해도 최상위 관문이
 * 닫혀 있으면 CPU 까지 오지 않는다.
 *
 * 3번만 읽기-수정-쓰기인 것에 주의할 만하다. 2번은 통째로 쓰는데, 그
 * 레지스터에 MSI 말고 다른 비트를 쓰지 않는다는 전제다.
 *
 * probe 가 PHY 를 리셋에 붙잡아 둔 것이 여기서 풀린다.
 *
 * 실행 컨텍스트: DWC 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 성공을 답한다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수]
 *     → regmap_update_bits() → writel_relaxed()
 */
static int k1_pcie_start_link(struct dw_pcie *pci)
{
	struct k1_pcie *k1 = to_k1_pcie(pci);
	u32 val;
/* [한국어] PHY 리셋 유지와 LTSSM 을 **한 번에** 다룬다. */

	/* Stop holding the PHY in reset, and enable link training */
	regmap_update_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
			   APP_HOLD_PHY_RST | LTSSM_EN, LTSSM_EN);

	/* Enable the MSI interrupt */
	writel_relaxed(MSI_CTRL_INT, k1->link + INTR_ENABLE);

	/* Top-level interrupt enable */
	val = readl_relaxed(k1->link + K1_PHY_AHB_IRQ_EN);
	val |= PCIE_INTERRUPT_EN;
	/* [한국어] 최상위 관문을 연다. 이것이 닫혀 있으면 개별 인터럽트를 허용해도
	 * CPU 까지 오지 않는다. */
	writel_relaxed(val, k1->link + K1_PHY_AHB_IRQ_EN);
/* [한국어] 이제 링크가 서고 인터럽트가 흐른다. */

	return 0;
}

/* [한국어]
 * k1_pcie_stop_link - 인터럽트를 닫고 링크를 내리며 PHY 를 리셋에 붙잡는다
 *
 * @pci: DWC 코어의 문맥.
 *
 * k1_pcie_start_link() 의 짝이며 순서가 정확히 역순이다 — 최상위 관문을
 * 먼저 닫고, 개별 인터럽트를 끄고, 마지막에 링크를 내린다.
 *
 * 그 순서여야 하는 이유는 링크를 먼저 내리면 그 과정에서 생긴 인터럽트가
 * 아직 열린 관문을 통과하기 때문이다.
 *
 * 마지막 줄도 시작 쪽과 대칭이다 — 같은 마스크에 값만 뒤집어,
 * LTSSM 을 끄면서 동시에 PHY 를 리셋에 붙잡는다.
 *
 * 실행 컨텍스트: DWC 코어의 정리 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.stop_link == [이 함수]
 *     → readl_relaxed() → writel_relaxed() → regmap_update_bits()
 */
static void k1_pcie_stop_link(struct dw_pcie *pci)
{
	struct k1_pcie *k1 = to_k1_pcie(pci);
	u32 val;
/* [한국어] 먼저 최상위 관문을 닫는다 — 링크를 내리면서 생긴 인터럽트가
 * 통과하지 못하게 하려는 것이다. */

	/* Disable interrupts */
	val = readl_relaxed(k1->link + K1_PHY_AHB_IRQ_EN);
	val &= ~PCIE_INTERRUPT_EN;
	/* [한국어] 관문을 닫은 값을 되쓴다. */
	writel_relaxed(val, k1->link + K1_PHY_AHB_IRQ_EN);
/* [한국어] 그 다음 개별 인터럽트도 끈다. */

	writel_relaxed(0, k1->link + INTR_ENABLE);
/* [한국어] 마지막으로 링크를 내린다. 시작 쪽과 정확히 역순이다. */

	/* Disable the link and hold the PHY in reset */
	regmap_update_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
			   APP_HOLD_PHY_RST | LTSSM_EN, APP_HOLD_PHY_RST);
}

static const struct dw_pcie_ops k1_pcie_ops = {
	/* [한국어] 링크 판정도 벤더 창을 봐야 해 콜백으로 둔다. */
	.link_up	= k1_pcie_link_up,
	/* [한국어] 시작과 중단 둘 다 APMU 를 건드려야 해 콜백이 필요하다. */
	.start_link	= k1_pcie_start_link,
	.stop_link	= k1_pcie_stop_link,
};

/* [한국어]
 * k1_pcie_parse_port - 루트 포트 자식 노드에서 PHY 를 얻는다
 *
 * @k1: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * PHY 를 **자식 노드에서** 얻는 것이 이 함수의 특징이다. PCIe 루트 포트가
 * 디바이스 트리에서 별도 노드로 서술되고, 링크에 관한 자원이 그 아래
 * 매달려 있기 때문이다.
 *
 * 옆의 상류 주석대로 루트 포트가 하나뿐이라는 전제로 첫 자식만 본다.
 *
 * 노드 참조를 PHY 조회 **직후** 에 놓는다. devm 판이 그 노드를 계속
 * 붙잡지 않는다는 전제이며, 그 덕분에 오류 검사가 참조 해제 뒤로 갈 수 있어
 * 경로가 하나로 단순해진다.
 *
 * PHY 가 없으면 실패다 — 선택 사항으로 다루지 않는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 자식 노드가 없으면 -EINVAL, PHY 조회 실패는 그 오류.
 *
 * 호출 체인:
 *   k1_pcie_probe() → [이 함수]
 *     → of_get_next_available_child() → devm_of_phy_get() → of_node_put()
 */
static int k1_pcie_parse_port(struct k1_pcie *k1)
{
	struct device *dev = k1->pci.dev;
	struct device_node *root_port;
	/* [한국어] 얻어 낼 PHY. */
	struct phy *phy;
/* [한국어] 루트 포트 노드를 먼저 찾는다. */

	/* We assume only one root port */
	root_port = of_get_next_available_child(dev_of_node(dev), NULL);
	if (!root_port)
		/* [한국어] 자식 노드가 없으면 PHY 를 찾을 자리가 없다. */
		return -EINVAL;

	phy = devm_of_phy_get(dev, root_port, NULL);
/* [한국어] **PHY 조회 직후에 노드 참조를 놓는다** — devm 판이 그 노드를 계속
 * 붙잡지 않는다는 전제이며, 덕분에 오류 검사가 하나로 단순해진다. */

	of_node_put(root_port);

	if (IS_ERR(phy))
		/* [한국어] PHY 를 얻지 못하면 그 오류를 올려보낸다. 선택 사항이 아니다. */
		return PTR_ERR(phy);

	k1->phy = phy;
/* [한국어] 얻은 PHY 를 기록한다. */

	return 0;
}

/* [한국어]
 * k1_pcie_probe - APMU 와 link 창을 얻고 DWC 호스트를 초기화한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * APMU 를 phandle 과 **인자로** 찾는 것이 눈에 띈다. 디바이스 트리가
 * syscon 노드와 함께 오프셋 하나를 주고, 그 값이 pmu_off 가 되어 이후
 * 모든 APMU 접근의 기준이 된다. APMU 블록을 여러 드라이버가 나눠 쓰므로
 * 각자의 영역을 그렇게 지정한다.
 *
 * DWC 코어에 넘기는 설정이 셋이다.
 * - 콜백 표 둘.
 * - MSI 벡터 수를 최대로.
 * - REQ_RES 능력 표시 — **클럭과 리셋을 코어가 대신 얻어 달라는 뜻** 이며,
 *   그래서 이 파일에 devm_clk_get 이나 reset_control_get 이 없다.
 *
 * PHY 를 리셋에 붙잡아 두는 것이 순서상 중요하다. 이 시점부터
 * k1_pcie_start_link() 가 풀 때까지 PHY 가 동작하지 않아, 그 사이의
 * 설정이 안정적으로 이뤄진다.
 *
 * 전원을 그 뒤에 켜는 것도 그와 맞물린다 — PHY 가 붙잡힌 상태에서
 * 전원이 들어와야 한다.
 *
 * [상류 코드 관찰] devm_pm_runtime_enable() 의 반환값을 확인하지 않는다.
 * 같은 디렉터리의 pcie-andes-qilai.c, pcie-eswin.c, pcie-nxp-s32g.c 도
 * 같은 형태라, 이 계열 드라이버의 관용으로 보인다. 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 링크 대기와
 * 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 각 단계의 실패를 dev_err_probe 로 기록하고 올려보낸다.
 * 되감기 코드가 없는데, 잡는 자원이 모두 devm 판이기 때문이다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → syscon_regmap_lookup_by_phandle_args()
 *     → devm_platform_ioremap_resource_byname() → dw_pcie_cap_set(REQ_RES)
 *     → devm_regulator_get_enable() → k1_pcie_parse_port()
 *     → dw_pcie_host_init()
 */
static int k1_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct k1_pcie *k1;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 상태 구조를 잡는다. */

	k1 = devm_kzalloc(dev, sizeof(*k1), GFP_KERNEL);
	/* [한국어] 잡지 못하면, */
	if (!k1)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	k1->pmu = syscon_regmap_lookup_by_phandle_args(dev_of_node(dev),
						       /* [한국어] phandle 과 **인자 하나** 를 함께 받는다 — 그 인자가 아래 pmu_off 가 되어
						        * APMU 안에서 이 컨트롤러의 영역을 지정한다. */
						       SYSCON_APMU, 1,
						       &k1->pmu_off);
	if (IS_ERR(k1->pmu))
		/* [한국어] 찾지 못하면 오류 코드를 꺼내, */
		return dev_err_probe(dev, PTR_ERR(k1->pmu),
				     /* [한국어] 무엇이 실패했는지와 함께 올려보낸다. */
				     "failed to lookup PMU registers\n");

	k1->link = devm_platform_ioremap_resource_byname(pdev, "link");
	/* [한국어] link 창 매핑이 실패하면, */
	if (IS_ERR(k1->link))
		/* [한국어] 오류 코드를 꺼내, */
		return dev_err_probe(dev, PTR_ERR(k1->link),
				     /* [한국어] 무엇이 실패했는지와 함께 올려보낸다. */
				     "failed to map \"link\" registers\n");

	k1->pci.dev = dev;
	/* [한국어] 코어 콜백 표를 건다. */
	k1->pci.ops = &k1_pcie_ops;
	/* [한국어] MSI 벡터를 이 컨트롤러가 지원하는 최대로 요청한다. */
	k1->pci.pp.num_vectors = MAX_MSI_IRQS;
	/* [한국어] **클럭과 리셋을 코어가 대신 얻어 달라는 표시다** — 그래서 이 파일에
	 * devm_clk_get 이나 reset_control_get 이 없다. */
	dw_pcie_cap_set(&k1->pci, REQ_RES);
/* [한국어] 호스트 콜백 표까지 걸면 코어와의 접점이 완성된다. */

	k1->pci.pp.ops = &k1_pcie_host_ops;
/* [한국어] **PHY 를 리셋에 붙잡아 둔다**(옆의 상류 주석). 이 시점부터
 * k1_pcie_start_link() 가 풀 때까지 PHY 가 동작하지 않아,
 * 그 사이의 설정이 안정적으로 이뤄진다. */

	/* Hold the PHY in reset until we start the link */
	regmap_set_bits(k1->pmu, k1->pmu_off + PCIE_CLK_RESET_CONTROL,
			APP_HOLD_PHY_RST);

	ret = devm_regulator_get_enable(dev, "vpcie3v3");
	/* [한국어] 3.3V 전원을 얻고 켜는 데 실패하면, */
	if (ret)
		/* [한국어] 무엇이 실패했는지와 함께 */
		return dev_err_probe(dev, ret,
				     /* [한국어] 올려보낸다. PHY 를 붙잡아 둔 **뒤** 에 전원을 켜는 순서가 요점이다. */
				     "failed to get \"vpcie3v3\" supply\n");

	pm_runtime_set_active(dev);
	pm_runtime_no_callbacks(dev);
	devm_pm_runtime_enable(dev);

	platform_set_drvdata(pdev, k1);
/* [한국어] 런타임 PM 준비가 끝났다. */

	ret = k1_pcie_parse_port(k1);
	/* [한국어] 루트 포트 노드에서 PHY 를 얻지 못하면, */
	if (ret)
		/* [한국어] 그 사실과 함께 올려보낸다. */
		return dev_err_probe(dev, ret, "failed to parse root port\n");

	ret = dw_pcie_host_init(&k1->pci.pp);
	/* [한국어] DWC 호스트 초기화가 실패하면, */
	if (ret)
		/* [한국어] 그 사실과 함께 올려보낸다. 되감기가 없는 것은 잡는 자원이 모두 devm 판이기 때문이다. */
		return dev_err_probe(dev, ret, "failed to initialize host\n");
/* [한국어] 이 뒤로는 코어가 링크 훈련과 버스 스캔을 진행한다. */

	return 0;
}

/* [한국어]
 * k1_pcie_remove - DWC 호스트를 내린다
 *
 * @pdev: 플랫폼 장치.
 *
 * 한 줄이다. dw_pcie_host_deinit() 이 버스를 내리고, 그 안에서 이 파일의
 * k1_pcie_deinit() 콜백이 불려 PERST# 와 PHY 와 자원을 정리한다.
 *
 * 나머지 자원은 모두 devm 판이라 이 함수가 돌아간 뒤 코어가 자동으로
 * 되돌린다 — APMU regmap, link 창 매핑, PHY 참조, 전원, 런타임 PM 모두다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → dw_pcie_host_deinit() → k1_pcie_deinit()
 */
static void k1_pcie_remove(struct platform_device *pdev)
{
	struct k1_pcie *k1 = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&k1->pci.pp);
}

static const struct of_device_id k1_pcie_of_match_table[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다. */
	{ .compatible = "spacemit,k1-pcie", },
	/* [한국어] 표의 끝 표시. */
	{ }
};

static struct platform_driver k1_pcie_driver = {
	/* [한국어] probe 콜백. */
	.probe	= k1_pcie_probe,
	/* [한국어] remove 콜백. 이 드라이버는 뗄 수 있다 — builtin 으로 고정하지 않았다. */
	.remove	= k1_pcie_remove,
	.driver = {
		.name			= "spacemit-k1-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table		= k1_pcie_of_match_table,
		.probe_type		= PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(k1_pcie_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SpacemiT K1 PCIe host driver");
