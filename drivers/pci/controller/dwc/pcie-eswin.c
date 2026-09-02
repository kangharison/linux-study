// SPDX-License-Identifier: GPL-2.0
/*
 * ESWIN PCIe Root Complex driver
 *
 * Copyright 2026, Beijing ESWIN Computing Technology Co., Ltd.
 *
 * Authors: Yu Ning <ningyu@eswincomputing.com>
 *          Senchuan Zhang <zhangsenchuan@eswincomputing.com>
 *          Yanghui Ou <ouyanghui@eswincomputing.com>
 */

/*
 * [한국어 설명] ESWIN EIC7700 의 DesignWare PCIe 루트 컴플렉스 글루
 * (pcie-eswin.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 ESWIN EIC7700 SoC 에 붙이는 글루
 * 드라이버다. config 접근, ATU 설정, 버스 스캔은 pcie-designware-host.c 가
 * 맡고, 이 파일은 SoC 고유의 것만 처리한다.
 *
 * 하는 일이 다섯이다.
 *   1) 리셋 신호 세 갈래(pwr, dbi, PERST#)를 정해진 순서로 풀고 건다.
 *   2) 컨트롤러를 루트 포트 모드로 세우고 LTSSM 을 켠다.
 *   3) PHY 리셋을 풀고 **보조 클럭에서 본 클럭으로 전환** 되기를 기다린다.
 *   4) 벤더·장치 ID 를 손수 써 넣는다 — 기본값이 유효하지 않기 때문이다.
 *   5) L2/L3 저전력 진입을 건너뛰게 한다. 이 SoC 가 그 상태를 지원하지
 *      않아 PME 핸드셰이크가 끝나지 않기 때문이다.
 *
 * 같은 트리의 pcie-nxp-s32g.c 와 구조가 눈에 띄게 닮았다 — 포트를
 * 리스트로 관리하고, DWC 코어가 아직 루트 포트 노드의 num-lanes 를
 * 해석하지 못해 드라이버가 대신 읽는 우회로까지 같다(양쪽 모두 상류
 * 주석이 그 사실을 밝히고 있다). 다만 다음 세 가지는 다르다.
 *   - s32g 는 자식 노드 중 device_type 이 "pci" 인 것만 포트로 보는데,
 *     이 파일은 **모든 사용 가능한 자식** 을 포트로 취급한다.
 *   - s32g 는 PHY 계층(phy_init 등)을 쓰는데, 이 파일은 PHY 를 리셋
 *     비트 하나로 다룬다.
 *   - s32g 는 devm 판 PHY 를 쓰지만, 이 파일은 devm 이 아닌
 *     of_reset_control_get_exclusive() 를 써서 해제를 손수 챙긴다.
 *
 * === 리셋 세 갈래 (이 드라이버의 뼈대) ===
 * 상류 주석이 host_init 안에서 밝히듯, 신호마다 미치는 범위가 다르다.
 *   - pwr  : PCIe 컨트롤러를 리셋한다.
 *   - dbi  : DBI 레지스터를 리셋한다.
 *   - PERST#: **컨트롤러·PHY·엔드포인트를 한꺼번에** 리셋한다. 포트마다
 *            하나씩 있으며, PHY 설정 전에 반드시 풀려 있어야 한다.
 * 앞의 둘은 컨트롤러 단위라 묶음(bulk) API 로 함께 다루고, PERST# 만
 * 포트마다 따로 관리한다. 그래서 구조체가 둘로 나뉘어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> eswin_pcie_probe()
 *     -> 매칭 데이터, 클럭 전부, 리셋 둘 확보
 *     -> eswin_pcie_parse_ports() -> 포트마다 PERST# 와 num-lanes
 *     -> 런타임 PM 참조 획득
 *     -> dw_pcie_host_init()
 *        -> 콜백 -> [이 파일] eswin_pcie_host_init()
 *           -> 클럭 켜기 -> pwr/dbi 리셋 해제 -> 루트 포트 모드
 *              -> PERST# 걸었다 풀기(100ms) -> PHY 리셋 해제
 *                 -> 클럭 전환 대기(최대 20ms) -> 벤더·장치 ID 쓰기
 *        -> 코어가 start_link 콜백 -> [이 파일] LTSSM 켜기
 *        -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * 절전 진입 시:
 *   PM 코어 -> eswin_pcie_suspend_noirq() -> DWC 코어의 공용 구현
 *     -> 그 안에서 콜백 -> [이 파일] eswin_pcie_pme_turn_off()
 *        -> L2/L3 대기를 건너뛰라는 표시를 세운다
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. host_init 안에 msleep(100ms)와
 * 폴링 대기가 있어 잠들 수 있어야 한다. 이 파일에는 인터럽트 핸들러가 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware.c / pcie-designware-host.c. 접점이 두 벌의
 *   콜백 표다 — dw_pcie_ops(start_link, link_up)와
 *   dw_pcie_host_ops(init, deinit, pme_turn_off). elbi_base 를 코어와 나눠
 *   쓰며, DBI 쓰기 도우미도 코어의 것을 쓴다.
 * 옆쪽: clk·reset 계층과 런타임 PM. 앞의 둘은 이 트리에 없어 호출 규약까지만
 *   적었다.
 *
 * 데이터 흐름:
 *   디바이스 트리(클럭 전부, pwr/dbi 리셋, 자식 노드마다 PERST# 와
 *   num-lanes) -> probe -> struct eswin_pcie
 *   매칭 데이터(skip_l23) -> pcie->data -> pme_turn_off 에서 코어로
 *
 * 공유 상태: struct eswin_pcie 하나. 포트 리스트를 담으며 잠금이 없다 —
 *   리스트를 만들고 없애는 경로가 probe 와 host_init 뿐이라 성립한다.
 *
 * === NVMe 관점 ===
 * skip_l23 이 참이라 이 SoC 는 링크를 L2/L3 로 내리지 못한다. NVMe SSD 가
 * 붙어 있으면 시스템 절전 시 링크가 완전히 꺼지지 않으므로, 그만큼 전력이
 * 더 든다. 반대로 복귀는 링크를 다시 세우는 절차가 짧아진다.
 *
 * === 주요 함수/구조체 요약 ===
 * eswin_pcie_host_init()    : 이 파일의 거의 전부. 클럭·리셋·PHY·ID 를
 *                             순서대로 세우며, 되감기 라벨이 셋이다.
 * eswin_pcie_perst_reset()  : PERST# 를 걸고 100ms 기다렸다 푼다.
 * eswin_pcie_link_up()      : 링크 상태를 **config 공간의 표준 필드** 로
 *                             확인한다 — 전용 레지스터를 쓰지 않는다.
 * eswin_pcie_pme_turn_off() : 이름과 달리 PME 를 끄지 않고, L2/L3 대기를
 *                             건너뛰라는 표시만 세운다.
 * eswin_pcie_parse_ports()  : 자식 노드를 모두 포트로 훑는다.
 * struct eswin_pcie         : dw_pcie 를 맨 앞에 둔 상태 구조체.
 * struct eswin_pcie_port    : 포트 하나 — PERST# 와 레인 수.
 * struct eswin_pcie_data    : SoC 별 차이. 지금은 불리언 하나뿐이다.
 */

/* [한국어] [상류 코드 관찰] 이 헤더의 이름(irqreturn_t, request_irq, IRQF_ 계열)을
 * 쓰는 곳을 이 파일에서 찾지 못했다. 이 드라이버에는 인터럽트 핸들러가 없다. */
#include <linux/interrupt.h>
/* [한국어] readl_poll_timeout — 클럭 전환을 기다리는 폴링에 쓴다. */
#include <linux/iopoll.h>
/* [한국어] MODULE_DESCRIPTION/AUTHOR/LICENSE 선언용. */
#include <linux/module.h>
/* [한국어] of_property_read_u32 와 자식 노드 순회 매크로. */
#include <linux/of.h>
/* [한국어] PCI_EXP_LNKSTA, PCI_VENDOR_ID 등 PCIe 규격 상수. */
#include <linux/pci.h>
/* [한국어] platform_device 와 드라이버 등록. */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_ 계열 — probe 가 런타임 PM 참조를 잡는다. */
#include <linux/pm_runtime.h>
/* [한국어] [상류 코드 관찰] struct resource 나 resource_size 를 쓰는 곳을 이 파일에서
 * 찾지 못했다. 이 드라이버는 레지스터 창을 직접 매핑하지 않고 DWC 코어의
 * elbi_base 를 쓴다. */
#include <linux/resource.h>
/* [한국어] reset_control_ 계열 — 이 드라이버의 뼈대인 리셋 조작. */
#include <linux/reset.h>
/* [한국어] u32 등 기본 타입. */
#include <linux/types.h>

/* [한국어] DWC 코어의 자료구조와 DBI 도우미. **이 헤더가 linux/bitfield.h,
 * linux/clk.h, 그리고 "../../pci.h" 를 함께 끌어온다** — 그래서 이 파일이
 * 직접 포함하지 않는 FIELD_PREP, clk_bulk_ 계열, PCIE_T_PVPERL_MS 를
 * 쓸 수 있다. */
#include "pcie-designware.h"

/* ELBI registers */
/* [한국어] ELBI(External Local Bus Interface) 제어 레지스터 0.
 * **LTSSM 활성, PHY 리셋 유지, 장치 종류가 모두 이 한 레지스터에** 들어
 * 있어서, 이 파일의 모든 쓰기가 읽기-수정-쓰기여야 한다. */
#define PCIEELBI_CTRL0_OFFSET		0x0
/* [한국어] ELBI 상태 레지스터 0. 클럭 전환 완료를 여기서 읽는다. */
#define PCIEELBI_STATUS0_OFFSET		0x100

/* LTSSM register fields */
/* [한국어] LTSSM 활성 비트. eswin_pcie_start_link() 가 이것만 세운다. */
#define PCIEELBI_APP_LTSSM_ENABLE	BIT(5)

/* APP_HOLD_PHY_RST register fields */
/* [한국어] PHY 리셋 유지 비트. **세워져 있으면 PHY 가 리셋 상태로 잡혀 있고,
 * 지워야 풀린다** — 이름 그대로 "붙잡고 있음" 이 활성 의미다. */
#define PCIEELBI_APP_HOLD_PHY_RST	BIT(6)

/* PM_SEL_AUX_CLK register fields */
/* [한국어] 보조 클럭 선택 비트. 상태 레지스터에서 이 비트가 **내려가야**
 * 본 클럭으로 전환이 끝난 것이다. */
#define PCIEELBI_PM_SEL_AUX_CLK		BIT(16)

/* DEV_TYPE register fields */
/* [한국어] 장치 종류 필드(4비트). PCIe 규격의 장치 종류 값을 그대로 넣는다. */
#define PCIEELBI_CTRL0_DEV_TYPE		GENMASK(3, 0)

/* Vendor and device ID value */
/* [한국어] ESWIN 의 PCI 벤더 ID. 보통 이런 상수는 include/linux/pci_ids.h 에
 * 모아 두는데 여기서는 파일 안에 두었다. 그 헤더는 이 트리에 없다. */
#define PCI_VENDOR_ID_ESWIN		0x1fe1
/* [한국어] EIC7700 루트 포트의 장치 ID. 위와 같은 이유로 여기에 있다. */
#define PCI_DEVICE_ID_ESWIN_EIC7700	0x2030

/* [한국어] 묶음으로 다룰 리셋 개수. 아래 표의 길이에서 뽑으므로 표에 항목을
 * 더하면 구조체의 배열 크기도 함께 늘어난다. */
#define ESWIN_NUM_RSTS			ARRAY_SIZE(eswin_pcie_rsts)

/* [한국어] 묶음 리셋의 이름표. 디바이스 트리의 reset-names 와 짝을 맞춘다.
 * PERST# 가 여기 없는 것이 요점이다 — 그것은 컨트롤러가 아니라 포트마다
 * 하나씩이라 따로 관리한다. */
static const char * const eswin_pcie_rsts[] = {
	/* [한국어] PCIe 컨트롤러 자체를 리셋한다. */
	"pwr",
	/* [한국어] DBI 레지스터를 리셋한다. */
	"dbi",
};

struct eswin_pcie_data {
	/* [한국어] 이 SoC 가 L2/L3 저전력 링크 상태를 지원하지 않는지 여부.
	 * 설정자: 아래 매칭 데이터 표가 컴파일 시점에 정한다.
	 * 읽는 자: eswin_pcie_pme_turn_off() 하나뿐.
	 * 값 범위: EIC7700 에서는 참. 앞으로 지원하는 SoC 가 나오면 거짓이 될 자리다.
	 * 동기화: 상수이므로 필요 없다.
	 * **구조체 전체가 이 불리언 하나뿐** 인데, 앞으로 SoC 별 차이가 늘어날 것을
	 * 내다본 배치로 보인다. */
	bool skip_l23;
};

struct eswin_pcie_port {
	/* [한국어] 포트 리스트의 연결 고리.
	 * 설정자: eswin_pcie_parse_port() 가 초기화한 뒤 pcie->ports 꼬리에 붙인다.
	 * 읽는 자: PERST# 를 다루는 모든 루프.
	 * 값 범위: 리스트에 붙어 있는 동안 유효.
	 * 동기화: 없다. 리스트를 만들고 없애는 경로가 probe 와 host_init 뿐이다. */
	struct list_head list;
	/* [한국어] 이 포트의 PERST# 리셋 핸들.
	 * 설정자: eswin_pcie_parse_port() 의 of_reset_control_get_exclusive().
	 * 읽는 자: eswin_pcie_perst_reset() 과 eswin_pcie_assert(), 그리고
	 * 되감기 경로 셋이 reset_control_put() 으로 내려놓는다.
	 * 값 범위: 유효한 reset_control 포인터.
	 * 동기화: 없다.
	 * **devm 판이 아니라 해제를 손수 챙겨야 한다** — 이 파일의 되감기 코드가
	 * 세 군데로 흩어진 원인이다. */
	struct reset_control *perst;
	/* [한국어] 이 포트의 레인 수.
	 * 설정자: eswin_pcie_parse_port() 가 디바이스 트리에서 읽어 담는다.
	 * 읽는 자: [상류 코드 관찰] **없다.** 담은 값을 곧바로 코어의 필드에
	 * 넣으므로, 이 필드를 다시 읽는 곳이 이 파일에 없다.
	 * 값 범위: 속성이 있으면 그 값, 없으면 0 인 채로 남는다.
	 * 동기화: 없다. */
	int num_lanes;
};

struct eswin_pcie {
	/* [한국어] DWC PCIe 코어의 공통 문맥. **구조체 맨 앞** 이지만, 이 파일은
	 * container_of 가 아니라 drvdata 를 거쳐 변환한다(아래 매크로 참조).
	 * 설정자: probe 가 dev 와 콜백 표 둘을 채우고, parse_port 가 num_lanes 를
	 * 채운다. 나머지는 DWC 코어의 몫.
	 * 읽는 자: 이 파일의 거의 모든 함수. 특히 pci->elbi_base 로 레지스터에 닿는다.
	 * 값 범위: 언제나 유효한 내장 구조체이며 포인터가 아니다.
	 * 동기화: probe 이후 이 파일이 바꾸는 필드가 없다. */
	struct dw_pcie pci;
	/* [한국어] 이 컨트롤러에 딸린 클럭 전부의 목록.
	 * 설정자: probe 의 devm_clk_bulk_get_all() 이 배열을 할당해 채운다.
	 * **얻기만 하고 켜지는 않는다** — 켜는 것은 host_init 의 몫이다.
	 * 읽는 자: host_init 이 켜고 host_deinit 과 되감기가 끈다.
	 * 값 범위: 유효한 배열 포인터. 개수는 아래 num_clks 에 있다.
	 * 동기화: 없다. */
	struct clk_bulk_data *clks;
	/* [한국어] pwr 과 dbi 리셋의 묶음.
	 * 설정자: probe 가 이름표를 채운 뒤 devm 판으로 핸들을 얻는다.
	 * 읽는 자: host_init 이 풀고, eswin_pcie_assert() 와 되감기가 건다.
	 * 값 범위: 원소 둘 고정. 위 표의 길이가 그대로 배열 크기가 된다.
	 * 동기화: 없다.
	 * PERST# 와 달리 devm 판이라 해제를 챙길 필요가 없다. */
	struct reset_control_bulk_data resets[ESWIN_NUM_RSTS];
	/* [한국어] 포트 리스트의 머리.
	 * 설정자: probe 가 초기화하고 parse_port 가 원소를 붙인다.
	 * 읽는 자: PERST# 를 다루는 모든 루프.
	 * 값 범위: 원소가 0개 이상. 실제로는 EIC7700 이 루트 포트 하나만 지원한다.
	 * 동기화: 없다.
	 * [상류 코드 관찰] host_init 이 실패하면 이 리스트가 비워진다. */
	struct list_head ports;
	/* [한국어] 이 SoC 의 매칭 데이터.
	 * 설정자: probe 가 of_device_get_match_data() 결과를 담는다.
	 * 읽는 자: eswin_pcie_pme_turn_off() 하나뿐.
	 * 값 범위: 매칭 표의 항목을 가리키는 포인터. NULL 은 probe 가 걸러 낸다.
	 * 동기화: 상수를 가리키므로 필요 없다. */
	const struct eswin_pcie_data *data;
	/* [한국어] 위 clks 배열의 원소 수.
	 * 설정자: probe 의 devm_clk_bulk_get_all() 이 돌려주는 값.
	 * 읽는 자: 클럭을 켜고 끄는 세 자리.
	 * 값 범위: 0 이상. **음수면 오류** 라 probe 가 그 경우를 따로 걸러 낸다.
	 * 동기화: 없다. */
	int num_clks;
};

/* [한국어] DWC 코어가 주는 dw_pcie 포인터를 이 드라이버의 구조체로 되돌린다.
 * pci 가 구조체 맨 앞이라 container_of 로도 되지만, 이 파일은 drvdata 를
 * 거치는 쪽을 골랐다 — pci-exynos.c 와 같은 방식이고, container_of 를 쓰는
 * pcie-nxp-s32g.c 와 대비된다. 그래서 **probe 가 drvdata 를 매단 뒤에만**
 * 이 매크로가 유효하다. */
#define to_eswin_pcie(x) dev_get_drvdata((x)->dev)

/* [한국어]
 * eswin_pcie_start_link - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @pci: DWC 코어의 문맥. elbi_base 를 여기서 얻는다.
 * @return: 언제나 0.
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크를 세우는 상태
 * 기계다. 이 비트를 켜야 컨트롤러가 상대와 협상을 시작한다.
 *
 * 읽기-수정-쓰기로 비트 하나만 건드린다. **같은 레지스터(CTRL0)에 장치 종류
 * 필드와 PHY 리셋 비트도 함께 있어서** 통째로 쓰면 그것들이 지워진다.
 *
 * 레지스터 창을 드라이버가 따로 매핑하지 않고 코어의 elbi_base 를 쓴다 —
 * pci-exynos.c 와 같은 방식이고, 자기 창을 가진 pcie-histb.c 와 대비된다.
 *
 * relaxed 판을 쓴다. 이 쓰기 전후로 다른 메모리 접근과의 순서를 강제할
 * 필요가 없기 때문이다.
 *
 * **끄는 짝이 없다.** dw_pcie_ops 에 stop_link 를 두지 않아, 한 번 켠
 * LTSSM 을 이 드라이버가 끌 방법이 없다. 정지는 대신 eswin_pcie_assert() 가
 * 리셋으로 처리한다.
 *
 * 실행 컨텍스트: 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수]
 *     → readl_relaxed() → writel_relaxed()
 */
static int eswin_pcie_start_link(struct dw_pcie *pci)
{
	u32 val;

	/* Enable LTSSM */
	val = readl_relaxed(pci->elbi_base + PCIEELBI_CTRL0_OFFSET);
	val |= PCIEELBI_APP_LTSSM_ENABLE;
	/* [한국어] 고친 값을 되쓴다. 이 쓰기 순간부터 링크 훈련이 시작된다. */
	writel_relaxed(val, pci->elbi_base + PCIEELBI_CTRL0_OFFSET);

	return 0;
}

/* [한국어]
 * eswin_pcie_link_up - config 공간의 표준 필드로 링크 상태를 확인한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: true = 데이터 링크 계층이 활성, false = 아니다.
 *
 * **SoC 전용 레지스터를 전혀 쓰지 않는다** 는 점이 이 함수의 특징이다.
 * PCIe 규격이 정한 표준 경로만 쓴다 — 루트 포트의 PCI Express 능력 구조를
 * 찾아 링크 상태 레지스터를 읽고, 데이터 링크 계층 활성(DLLLA) 비트를 본다.
 *
 * 같은 트리의 다른 글루들과 대비된다. pci-exynos.c 는 전용 레지스터의
 * 비트 하나를, pcie-histb.c 는 셋을 본다. 이쪽은 하드웨어 문서 없이도
 * 무엇을 보는지 알 수 있는 대신, 매 호출마다 능력 목록을 훑는 비용이 든다.
 *
 * [상류 코드 관찰] dw_pcie_find_capability() 를 **호출할 때마다** 부른다.
 * 링크 대기는 이 함수를 반복 호출하는 폴링이므로, 같은 능력 구조를 찾는
 * config 공간 순회가 그만큼 되풀이된다. 오프셋을 한 번 찾아 보관하는
 * 구조가 아니다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * DLLLA 는 물리 계층이 아니라 **데이터 링크 계층** 이 살아 있음을 뜻한다.
 * 즉 이 비트가 서면 config 트랜잭션이 오갈 수 있는 상태다.
 *
 * 실행 컨텍스트: 코어의 링크 대기 폴링. 프로세스 컨텍스트.
 *
 * 에러 경로: 능력 구조를 찾지 못하면 offset 이 0 이 되어 config 공간
 * 맨 앞을 읽게 되나, 그 경우를 따로 걸러 내지 않는다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.link_up == [이 함수]
 *     → dw_pcie_find_capability() → dw_pcie_readw_dbi()
 */
static bool eswin_pcie_link_up(struct dw_pcie *pci)
{
	/* [한국어] PCI Express 능력 구조의 오프셋을 찾는다. **호출할 때마다 config
	 * 공간을 순회하므로**, 링크 대기 폴링에서 이 비용이 되풀이된다. */
	u16 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* [한국어] 그 구조 안의 링크 상태 레지스터를 읽는다. 16비트 필드다. */
	u16 val = dw_pcie_readw_dbi(pci, offset + PCI_EXP_LNKSTA);

	/* [한국어] 데이터 링크 계층 활성(DLLLA) 비트를 본다. 물리 계층이 아니라
	 * **config 트랜잭션이 오갈 수 있는 상태** 인지를 뜻한다. */
	return val & PCI_EXP_LNKSTA_DLLLA;
}

/* [한국어]
 * eswin_pcie_perst_reset - PERST# 를 걸고 규격이 정한 시간만큼 기다렸다 푼다
 *
 * @port: 이 PERST# 를 가진 포트.
 * @pcie: 드라이버 상태. 오류 기록에 쓸 device 를 얻는 용도다.
 * @return: 0 = 성공, 음수 오류.
 *
 * PERST# 는 PCIe 의 기본 리셋 신호이며, 상류 주석이 밝히듯 이 SoC 에서는
 * **컨트롤러·PHY·엔드포인트를 한꺼번에** 리셋한다. 즉 이 한 줄이 링크
 * 반대편의 장치까지 초기 상태로 되돌린다.
 *
 * 가운데의 100ms 대기가 이 함수의 존재 이유다. PCIe CEM 규격이 정한
 * T_PVPERL — 전원이 안정된 뒤 PERST# 를 풀기까지 지켜야 하는 최소 시간이며,
 * 그 값(PCIE_T_PVPERL_MS)은 drivers/pci/pci.h:107 에 100 으로 정의돼 있다.
 * 짧게 풀면 상대 장치가 준비되기 전에 링크 훈련이 시작돼 실패한다.
 *
 * msleep 을 쓰므로 **잠들 수 있는 문맥에서만** 부를 수 있다.
 *
 * 실행 컨텍스트: host_init 안. 프로세스 컨텍스트이며 최소 100ms 잠든다.
 *
 * 에러 경로: 걸기나 풀기가 실패하면 기록을 남기고 그 오류를 올려보내며,
 * 호출자가 되감기 라벨로 간다.
 *
 * 호출 체인:
 *   eswin_pcie_host_init() → [이 함수]
 *     → reset_control_assert() → msleep() → reset_control_deassert()
 */
static int eswin_pcie_perst_reset(struct eswin_pcie_port *port,
				  struct eswin_pcie *pcie)
{
	int ret;

	ret = reset_control_assert(port->perst);
	/* [한국어] PERST# 를 걸지 못했다. 리셋 컨트롤러 쪽 문제다. */
	if (ret) {
		/* [한국어] 실패를 알린다. */
		dev_err(pcie->pci.dev, "Failed to assert PERST#\n");
		/* [한국어] 그 오류를 올려보낸다. 아직 건 것이 없어 되감을 것도 없다. */
		return ret;
	}

	/* Ensure that PERST# has been asserted for at least 100 ms */
	msleep(PCIE_T_PVPERL_MS);

	ret = reset_control_deassert(port->perst);
	/* [한국어] PERST# 를 풀지 못했다. 이 경우 리셋이 걸린 채로 남는다. */
	if (ret) {
		/* [한국어] 실패를 알린다. */
		dev_err(pcie->pci.dev, "Failed to deassert PERST#\n");
		/* [한국어] 호출자가 되감기 라벨로 가 나머지를 정리한다. */
		return ret;
	}

	return 0;
}

/* [한국어]
 * eswin_pcie_assert - 모든 리셋을 걸어 컨트롤러를 정지 상태로 둔다
 *
 * @pcie: 드라이버 상태.
 *
 * 세 갈래 리셋을 모두 건다 — 포트마다의 PERST# 를 차례로, 그다음 pwr 과
 * dbi 를 묶음으로.
 *
 * **순서가 중요하다.** PERST# 를 먼저 걸어 링크 반대편까지 정지시킨 뒤에
 * 컨트롤러 자신을 리셋한다. 반대로 하면 컨트롤러가 죽은 상태에서 PERST#
 * 조작이 통하지 않을 수 있다.
 *
 * 이 드라이버에 stop_link 콜백이 없으므로, **링크를 내리는 유일한 수단이
 * 이 함수** 다.
 *
 * 반환값을 확인하지 않는다. 해제 경로에서 불리므로 실패해도 달리 할 일이
 * 없기 때문이다.
 *
 * 실행 컨텍스트: host_deinit. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   eswin_pcie_host_deinit() → [이 함수]
 *     → reset_control_assert() ×포트수 → reset_control_bulk_assert()
 */
static void eswin_pcie_assert(struct eswin_pcie *pcie)
{
	struct eswin_pcie_port *port;

	list_for_each_entry(port, &pcie->ports, list)
		/* [한국어] 포트마다 PERST# 를 건다. 링크 반대편 장치까지 정지한다. */
		reset_control_assert(port->perst);
	reset_control_bulk_assert(ESWIN_NUM_RSTS, pcie->resets);
}

/* [한국어]
 * eswin_pcie_parse_port - 자식 노드 하나에서 PERST# 와 레인 수를 읽는다
 *
 * @pcie: 드라이버 상태. 포트를 이 안의 리스트에 붙인다.
 * @node: 디바이스 트리의 자식 노드.
 * @return: 0 = 성공, 음수 오류.
 *
 * 포트 구조체를 하나 만들어 PERST# 핸들을 담고 리스트에 붙인다.
 *
 * **devm 이 아닌 판을 쓴다.** of_reset_control_get_exclusive() 는 자동
 * 해제가 없으므로, 이 파일의 여러 되감기 경로가 reset_control_put() 을
 * 손수 부른다. 그 때문에 되감기 코드가 세 군데로 흩어져 있다.
 *
 * exclusive 판이라는 것은 이 리셋 신호를 다른 소비자와 나눠 쓰지 않겠다는
 * 뜻이다. PERST# 가 링크 반대편까지 리셋하므로 공유하면 위험하다.
 *
 * num-lanes 를 이 파일이 직접 읽는 이유는 옆의 상류 TODO 주석이 밝힌다 —
 * 디바이스 트리에서 루트 포트 노드가 분리돼 나오면서 DWC 코어의 초기화
 * 코드가 그 속성을 해석하지 못하게 됐고, ESWIN 이 루트 포트 하나만
 * 지원하므로 드라이버가 대신 읽는다는 것이다. 즉 **임시 우회로** 임을
 * 상류가 스스로 밝히고 있다. 같은 우회로가 pcie-nxp-s32g.c 에도 있다.
 *
 * [상류 코드 관찰] 읽은 값을 port->num_lanes 에 담고 곧바로 코어의
 * 필드에도 넣는데, **port->num_lanes 를 다시 읽는 곳이 이 파일에 없다.**
 * 포트가 하나뿐이라 구조체에 남겨 둘 이유가 없는 셈이다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, PERST# 확보 실패는 기록과 함께 그 오류를
 * 올려보내며, 호출자가 리스트를 정리한다.
 *
 * 호출 체인:
 *   eswin_pcie_parse_ports() → [이 함수]
 *     → devm_kzalloc() → of_reset_control_get_exclusive()
 *     → of_property_read_u32() → list_add_tail()
 */
static int eswin_pcie_parse_port(struct eswin_pcie *pcie,
				 struct device_node *node)
{
	struct device *dev = pcie->pci.dev;
	struct eswin_pcie_port *port;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!port)
		/* [한국어] 호출자가 그때까지의 포트를 정리한다. */
		return -ENOMEM;

	port->perst = of_reset_control_get_exclusive(node, "perst");
	/* [한국어] PERST# 를 못 얻었다. 디바이스 트리 기술이 빠졌거나 이미 점유된 경우다. */
	if (IS_ERR(port->perst)) {
		/* [한국어] 실패를 알린다. */
		dev_err(dev, "Failed to get PERST# reset\n");
		/* [한국어] 오류 코드를 꺼내 올려보낸다. */
		return PTR_ERR(port->perst);
	}

	/*
	 * TODO: Since the Root Port node is separated out by pcie devicetree,
	 * the DWC core initialization code can't parse the num-lanes attribute
	 * in the Root Port. Before entering the DWC core initialization code,
	 * the platform driver code parses the Root Port node. The ESWIN only
	 * supports one Root Port node, and the num-lanes attribute is suitable
	 * for the case of one Root Port.
	 */
	/* [한국어] 레인 수 속성을 읽는다. 옆의 상류 TODO 주석이 밝히듯, 루트 포트 노드가
	 * 분리되면서 DWC 코어가 이 속성을 해석하지 못하게 된 데 대한 임시 우회로다. */
	if (!of_property_read_u32(node, "num-lanes", &port->num_lanes))
		/* [한국어] 읽은 값을 코어의 필드에 넣는다. 링크를 세우기 전이라 유효하다. */
		pcie->pci.num_lanes = port->num_lanes;

	/* [한국어] 연결 고리를 초기화한다. 바로 다음 줄이 덮으므로 사실상 방어적이다. */
	INIT_LIST_HEAD(&port->list);
	/* [한국어] 리스트 꼬리에 붙인다. 디바이스 트리의 순서가 그대로 유지된다. */
	list_add_tail(&port->list, &pcie->ports);

	/* [한국어] 여기까지 오면 이 포트는 준비가 끝났다. */
	return 0;
}

/* [한국어]
 * eswin_pcie_parse_ports - 사용 가능한 자식 노드를 모두 포트로 훑는다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 디바이스 트리의 자식 노드를 돌며 포트를 만든다.
 *
 * **노드 종류를 가리지 않는다.** 같은 구조를 가진 pcie-nxp-s32g.c 는
 * device_type 이 "pci" 인 자식만 포트로 보는데, 이 파일은 사용 가능한
 * 자식이면 모두 포트로 취급한다. 그 트리에 포트 아닌 자식이 오면 PERST#
 * 확보에서 실패하게 되는 구조인데, 그렇게 둔 이유는 코드에 적혀 있지 않다.
 *
 * _scoped 순회 판을 쓰므로 자식 노드의 참조를 손수 내려놓을 필요가 없다.
 *
 * 되감기가 이 함수 안에 있다. 실패하면 그때까지 붙인 포트의 PERST# 를
 * 모두 내려놓고 리스트를 비운다 — devm 이 아닌 판으로 얻었기 때문이다.
 * 포트 구조체 자체는 devm 할당이라 메모리는 그대로 남는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 자식에서 실패하든 그때까지의 것을 되감고 그 오류를
 * 올려보낸다.
 *
 * 호출 체인:
 *   eswin_pcie_probe() → [이 함수]
 *     → eswin_pcie_parse_port() → (실패 시) reset_control_put() → list_del()
 */
static int eswin_pcie_parse_ports(struct eswin_pcie *pcie)
{
	struct eswin_pcie_port *port, *tmp;
	struct device *dev = pcie->pci.dev;
	/* [한국어] 각 자식의 파싱 결과. */
	int ret;

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		/* [한국어] 자식 노드 하나를 포트로 만든다. */
		ret = eswin_pcie_parse_port(pcie, of_port);
		/* [한국어] 이 자식을 포트로 만들지 못했다. */
		if (ret)
			/* [한국어] 되감기로 간다. **종류를 가리지 않고 모든 자식을 포트로 보므로,
			 * 포트가 아닌 자식이 있으면 여기로 온다.** */
			goto err_port;
	}

	return 0;

err_port:
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		reset_control_put(port->perst);
		list_del(&port->list);
	}

	return ret;
}

/* [한국어]
 * eswin_pcie_host_init - 클럭·리셋·PHY·ID 를 순서대로 세운다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, 음수 오류.
 *
 * **이 파일의 거의 전부** 이며, 하드웨어를 깨우는 순서가 여기 다 있다.
 * DWC 코어가 링크를 세우기 전에 부른다.
 *
 * 일곱 단계다.
 * 1. 클럭을 모두 켠다.
 * 2. pwr 과 dbi 리셋을 푼다. 컨트롤러와 DBI 레지스터가 여기서 살아난다.
 * 3. 장치 종류를 루트 포트로 놓는다.
 * 4. 포트마다 PERST# 를 걸었다 100ms 뒤 푼다. 링크 반대편까지 초기화된다.
 * 5. PHY 리셋 유지 비트를 지워 PHY 를 놓아 준다.
 * 6. **보조 클럭에서 본 클럭으로 전환** 되기를 기다린다. 상태 레지스터의
 *    해당 비트가 내려갈 때까지 최대 20ms 폴링한다. 상류 주석이 밝히듯
 *    PHY 는 5번 이후 20ms 안에 준비된다.
 * 7. 벤더·장치 ID 를 손수 써 넣는다. 옆의 상류 주석대로 기본값이 유효하지
 *    않기 때문이며, 이 값들이 있어야 리눅스가 루트 포트를 제대로 식별한다.
 *    config 공간의 그 필드는 읽기 전용이라 보호를 풀고 써야 한다.
 *
 * **되감기 라벨이 셋이고 아래로 흘러내린다.** 실패 지점이 뒤일수록 되감을
 * 것이 많아지는 구조다 — 6·7번에서 실패하면 PERST#·묶음 리셋·클럭을 모두
 * 되감고, 4번이면 묶음 리셋과 클럭을, 2번이면 클럭만 되감는다.
 *
 * 마지막에 포트 리스트를 비우는 것이 세 라벨 모두의 공통 꼬리다.
 * PERST# 가 devm 이 아닌 판이라 손수 내려놓아야 하기 때문이다.
 *
 * [상류 코드 관찰] 그 결과 **초기화가 한 번 실패하면 포트 리스트가
 * 비워진다.** 이 콜백이 다시 불릴 경우 4번의 PERST# 루프가 아무것도 하지
 * 않게 되는데, 실제로 다시 불리는 경로가 있는지는 DWC 코어 쪽 사정이라
 * 이 트리에서 확인 못 함. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트이며 100ms 잠들고
 * 최대 20ms 폴링한다.
 *
 * 에러 경로: 각 단계의 실패가 해당 라벨로 모여 되감고 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [이 함수]
 *     → clk_bulk_prepare_enable() → reset_control_bulk_deassert()
 *     → eswin_pcie_perst_reset() → readl_poll_timeout()
 *     → dw_pcie_dbi_ro_wr_en() → dw_pcie_writew_dbi() ×2
 *     → dw_pcie_dbi_ro_wr_dis()
 */
static int eswin_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct eswin_pcie *pcie = to_eswin_pcie(pci);
	/* [한국어] 포트 순회 커서와, 리스트를 비울 때 쓸 임시 포인터. */
	struct eswin_pcie_port *port, *tmp;
	/* [한국어] 레지스터 값을 담고 폴링 결과도 받는 자리. */
	u32 val;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	/* [한국어] 클럭을 켜지 못했다. 아무것도 하지 못한다. */
	if (ret)
		/* [한국어] 되감을 것이 없어 곧바로 돌아간다. */
		return ret;

	/*
	 * The PWR and DBI reset signals are respectively used to reset the
	 * PCIe controller and the DBI register.
	 *
	 * The PERST# signal is a reset signal that simultaneously controls the
	 * PCIe controller, PHY, and Endpoint. Before configuring the PHY, the
	 * PERST# signal must first be deasserted.
	 *
	 * The external reference clock is supplied simultaneously to the PHY
	 * and EP. When the PHY is configurable, the entire chip already has
	 * stable power and reference clock. The PHY will be ready within 20ms
	 * after writing app_hold_phy_rst register bit of ELBI register space.
	 */
	ret = reset_control_bulk_deassert(ESWIN_NUM_RSTS, pcie->resets);
	if (ret) {
		/* [한국어] 실패를 알린다. */
		dev_err(pcie->pci.dev, "Failed to deassert resets\n");
		/* [한국어] 클럭만 되감는 라벨로 간다. */
		goto err_deassert;
	}

	/* Configure Root Port type */
	val = readl_relaxed(pci->elbi_base + PCIEELBI_CTRL0_OFFSET);
	val &= ~PCIEELBI_CTRL0_DEV_TYPE;
	/* [한국어] 장치 종류를 루트 포트로 놓는다. 엔드포인트 모드와 갈리는 지점이다. */
	val |= FIELD_PREP(PCIEELBI_CTRL0_DEV_TYPE, PCI_EXP_TYPE_ROOT_PORT);
	/* [한국어] 고친 값을 되쓴다. 링크를 세우기 **전** 이라야 유효한 설정이다. */
	writel_relaxed(val, pci->elbi_base + PCIEELBI_CTRL0_OFFSET);

	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] 이 포트의 PERST# 를 걸었다 100ms 뒤 푼다. */
		ret = eswin_pcie_perst_reset(port, pcie);
		/* [한국어] PERST# 조작이 실패했다. */
		if (ret)
			/* [한국어] 묶음 리셋과 클럭을 되감는 라벨로 간다. */
			goto err_perst;
	}

	/* Configure app_hold_phy_rst */
	val = readl_relaxed(pci->elbi_base + PCIEELBI_CTRL0_OFFSET);
	val &= ~PCIEELBI_APP_HOLD_PHY_RST;
	/* [한국어] 고친 값을 되쓴다. 이 순간부터 PHY 가 스스로 준비를 시작한다. */
	writel_relaxed(val, pci->elbi_base + PCIEELBI_CTRL0_OFFSET);

	/* The maximum waiting time for the clock switch lock is 20ms */
	ret = readl_poll_timeout(pci->elbi_base + PCIEELBI_STATUS0_OFFSET, val,
				 !(val & PCIEELBI_PM_SEL_AUX_CLK), 1000,
				 20000);
	if (ret) {
		/* [한국어] 20ms 안에 전환이 끝나지 않았다. PHY 나 클럭 쪽 문제다. */
		dev_err(pci->dev, "Timeout waiting for PM_SEL_AUX_CLK ready\n");
		/* [한국어] PERST# 까지 모두 되감는 라벨로 간다. */
		goto err_phy_init;
	}

	/*
	 * Configure ESWIN VID:DID for Root Port as the default values are
	 * invalid.
	 */
	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, PCI_VENDOR_ID_ESWIN);
	/* [한국어] 장치 ID 도 마찬가지다. 이 둘이 있어야 루트 포트가 제대로 식별된다. */
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, PCI_DEVICE_ID_ESWIN_EIC7700);
	/* [한국어] 읽기 전용 보호를 다시 건다. 반드시 짝을 맞춰야 한다. */
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;

err_phy_init:
	list_for_each_entry(port, &pcie->ports, list)
		reset_control_assert(port->perst);
err_perst:
	reset_control_bulk_assert(ESWIN_NUM_RSTS, pcie->resets);
err_deassert:
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		/* [한국어] PERST# 참조를 내려놓는다. devm 판이 아니라 손수 챙겨야 하는
		 * 자리이며, 이 파일에 같은 코드가 세 번 나온다. */
		reset_control_put(port->perst);
		list_del(&port->list);
	}

	return ret;
}

/* [한국어]
 * eswin_pcie_host_deinit - 모든 리셋을 걸고 클럭을 끈다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * eswin_pcie_host_init() 의 짝이며 두 줄이다 — 리셋을 모두 걸고 클럭을 끈다.
 *
 * **순서가 중요하다.** 클럭을 먼저 끄면 리셋 조작이 하드웨어에 닿지 않을 수
 * 있으므로, 리셋을 걸어 컨트롤러를 세운 뒤에 클럭을 끊는다.
 *
 * host_init 이 했던 일 중 **되돌리지 않는 것** 이 둘 있다 — 벤더·장치 ID
 * 쓰기와 PHY 리셋 해제다. 둘 다 리셋을 걸면 하드웨어가 알아서 초기값으로
 * 돌아가기 때문이다.
 *
 * 포트 리스트를 비우지 않는다. host_init 의 되감기 경로와 다른 점인데,
 * 정상 해제 시에는 리스트가 그대로 남는다.
 *
 * 실행 컨텍스트: dw_pcie_host_deinit() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_deinit() → dw_pcie_host_ops.deinit == [이 함수]
 *     → eswin_pcie_assert() → clk_bulk_disable_unprepare()
 */
static void eswin_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	/* [한국어] 코어의 문맥에서 이 드라이버의 상태를 되찾는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 를 거쳐 변환한다 — 위 매크로 참조. */
	struct eswin_pcie *pcie = to_eswin_pcie(pci);

	/* [한국어] 리셋을 먼저 모두 건다. 클럭을 먼저 끄면 리셋 조작이 하드웨어에
	 * 닿지 않을 수 있다. */
	eswin_pcie_assert(pcie);
	/* [한국어] 그다음에 클럭을 끊는다. host_init 의 1번을 되돌리는 것이다. */
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
}

/* [한국어]
 * eswin_pcie_pme_turn_off - 이름과 달리 L2/L3 대기를 건너뛰라는 표시만 세운다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * **콜백 이름과 하는 일이 어긋나 보이는 함수다.** 원래 이 콜백은 절전
 * 진입 시 PME_Turn_Off 메시지를 보내라는 자리인데, 여기서는 메시지를
 * 보내지 않고 코어의 표시 하나만 세운다.
 *
 * 이유는 옆의 상류 주석이 밝힌다. EIC7700 SoC 에는 L2/L3 저전력 링크
 * 상태를 위한 하드웨어 지원이 없어, PME_Turn_Off / PME_To_Ack 핸드셰이크로
 * L2/L3 Ready 상태에 들어갈 수 없다. 응답이 오지 않으므로 코어가 그
 * 대기를 아예 건너뛰게 만드는 것이다.
 *
 * 즉 이 콜백을 **하드웨어 능력을 알리는 통로로 전용** 하고 있다. 코어가
 * 절전 경로에서 이 콜백을 부르는 시점이 그 표시를 세우기에 알맞기 때문이다.
 *
 * 값을 매칭 데이터에서 가져오므로, 앞으로 L2/L3 를 지원하는 SoC 가
 * 나오면 표만 고치면 된다. 지금은 항목이 하나뿐이고 그 값이 참이다.
 *
 * 실행 컨텍스트: 절전 진입. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어의 절전 경로 → dw_pcie_host_ops.pme_turn_off == [이 함수]
 */
static void eswin_pcie_pme_turn_off(struct dw_pcie_rp *pp)
{
	/* [한국어] 코어의 문맥에서 이 드라이버의 상태를 되찾는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 매칭 데이터를 읽기 위해 드라이버 상태로 변환한다. */
	struct eswin_pcie *pcie = to_eswin_pcie(pci);

	/*
	 * The ESWIN EIC7700 SoC lacks hardware support for the L2/L3 low-power
	 * link states. It cannot enter the L2/L3 Ready state through the
	 * PME_Turn_Off/PME_To_Ack handshake protocol. To avoid this problem,
	 * the skip_l23_ready has been set.
	 */
	pp->skip_l23_ready = pcie->data->skip_l23;
}

static const struct dw_pcie_host_ops eswin_pcie_host_ops = {
	/* [한국어] 링크를 세우기 전에 코어가 이 콜백을 부른다. */
	.init = eswin_pcie_host_init,
	/* [한국어] 해제할 때 부른다. 이 콜백을 두지 않는 글루도 많다. */
	.deinit = eswin_pcie_host_deinit,
	.pme_turn_off = eswin_pcie_pme_turn_off,
};

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크를 세워야 할 때 코어가 이것을 부른다. */
	.start_link = eswin_pcie_start_link,
	/* [한국어] 링크가 섰는지 물을 때 부른다. stop_link 는 두지 않았다. */
	.link_up = eswin_pcie_link_up,
};

/* [한국어]
 * eswin_pcie_probe - 매칭 데이터·클럭·리셋·포트를 모아 DWC 호스트를 세운다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며 일곱 단계다.
 *
 * 1. 매칭 데이터를 얻는다. 없으면 -ENODATA 로 곧바로 실패한다 — 이
 *    드라이버는 SoC 별 차이를 그 데이터로만 알기 때문이다.
 * 2. 상태 구조체를 할당하고 포트 리스트를 초기화한다.
 * 3. **콜백 표 두 벌을 미리 건다.** 아래 단계가 실패하더라도 코어가
 *    잘못된 표를 볼 일이 없게 하는 배치다.
 * 4. 클럭을 개수를 모른 채 전부 얻는다. 다만 **여기서 켜지는 않는다** —
 *    켜는 것은 host_init 의 몫이다.
 * 5. 리셋 둘을 이름표로 채워 묶음으로 얻는다. 이름은 파일 앞의 표에 있다.
 * 6. 자식 노드를 포트로 훑는다.
 * 7. 런타임 PM 참조를 잡고 DWC 호스트를 등록한다.
 *
 * [상류 코드 관찰] 되감기 라벨의 **이름과 하는 일이 서로 바뀌어 있다.**
 * `err_pm_runtime_put` 라벨은 포트 리스트를 정리하고, `err_init` 라벨은
 * 런타임 PM 참조를 내려놓는다. 흘러내리는 구조라 앞 라벨로 들어오면 둘 다
 * 실행되므로 동작 자체는 맞지만, 이름만 보고는 반대로 읽힌다.
 * 그리고 그 배치의 결과로, 호스트 등록이 실패해 `err_init` 로 들어오면
 * **포트의 PERST# 참조가 정리되지 않는다.** host_init 콜백이 실패한
 * 경우라면 그쪽이 이미 정리했으므로 문제가 없지만, 콜백이 성공한 뒤
 * 코어의 다른 단계에서 실패하면 참조가 남는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * devm_pm_runtime_enable() 의 반환값도 확인하지 않는다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 링크 대기와
 * 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 6번까지의 실패는 dev_err_probe 로 기록하고 그대로 돌아가며,
 * 7번의 실패만 되감기 라벨을 거친다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → of_device_get_match_data() → devm_clk_bulk_get_all()
 *     → devm_reset_control_bulk_get_exclusive() → eswin_pcie_parse_ports()
 *     → pm_runtime_get_sync() → dw_pcie_host_init()
 */
static int eswin_pcie_probe(struct platform_device *pdev)
{
	const struct eswin_pcie_data *data;
	struct eswin_pcie_port *port, *tmp;
	/* [한국어] 오류 기록과 자원 확보의 기준이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 드라이버의 상태 구조체. */
	struct eswin_pcie *pcie;
	/* [한국어] 그 안의 DWC 문맥을 가리킬 지름길. */
	struct dw_pcie *pci;
	/* [한국어] 반환값과 리셋 이름표 루프의 첨자. */
	int ret, i;

	data = of_device_get_match_data(dev);
	/* [한국어] 매칭 데이터가 없다. 디바이스 트리 표에 data 를 빠뜨린 경우다. */
	if (!data)
		/* [한국어] -ENODATA 로 곧바로 실패한다. 이 드라이버는 SoC 별 차이를 그
		 * 데이터로만 알기 때문에 없으면 진행할 수 없다. */
		return dev_err_probe(dev, -ENODATA, "No platform data\n");

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!pcie)
		/* [한국어] 아직 잡은 자원이 없어 곧바로 돌아간다. */
		return -ENOMEM;

	INIT_LIST_HEAD(&pcie->ports);

	pci = &pcie->pci;
	/* [한국어] 코어가 오류를 기록할 때 쓸 device 를 심는다. */
	pci->dev = dev;
	/* [한국어] 링크 제어 콜백 표를 건다. */
	pci->ops = &dw_pcie_ops;
	/* [한국어] 호스트 콜백 표도 **여기서** 건다. 아래 단계가 실패하더라도 코어가
	 * 잘못된 표를 볼 일이 없게 하는 배치다. */
	pci->pp.ops = &eswin_pcie_host_ops;
	/* [한국어] 매칭 데이터를 보관한다. pme_turn_off 가 나중에 읽는다. */
	pcie->data = data;

	pcie->num_clks = devm_clk_bulk_get_all(dev, &pcie->clks);
	/* [한국어] **음수면 오류다.** 0 이나 양수는 얻은 클럭 개수다. */
	if (pcie->num_clks < 0)
		/* [한국어] 그 오류를 기록과 함께 올려보낸다. */
		return dev_err_probe(dev, pcie->num_clks,
				     "Failed to get pcie clocks\n");

	for (i = 0; i < ESWIN_NUM_RSTS; i++)
		/* [한국어] 리셋 이름표를 배열에 채운다. 묶음 API 가 이 이름으로 찾는다. */
		pcie->resets[i].id = eswin_pcie_rsts[i];

	ret = devm_reset_control_bulk_get_exclusive(dev, ESWIN_NUM_RSTS,
						    pcie->resets);
	if (ret)
		/* [한국어] 리셋을 못 얻었으면 기록과 함께 올려보낸다. */
		return dev_err_probe(dev, ret, "Failed to get resets\n");

	ret = eswin_pcie_parse_ports(pcie);
	/* [한국어] 포트를 하나도 만들지 못했거나 중간에 실패했다. */
	if (ret)
		/* [한국어] 그 함수가 이미 되감았으므로 오류만 올려보낸다. */
		return dev_err_probe(dev, ret, "Failed to parse Root Port\n");

	platform_set_drvdata(pdev, pcie);

	pm_runtime_no_callbacks(dev);
	devm_pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	/* [한국어] 참조 획득 실패다. **음수만 실패로 본다** — 장치가 이미 켜져 있으면
	 * 양수를 돌려주는데 그것은 성공이다. */
	if (ret < 0)
		/* [한국어] 포트 정리와 PM 참조 반납을 모두 거치는 라벨로 간다. */
		goto err_pm_runtime_put;

	ret = dw_pcie_host_init(&pci->pp);
	/* [한국어] 호스트 등록이 실패했다. */
	if (ret) {
		/* [한국어] 실패를 알린다. */
		dev_err(dev, "Failed to init host\n");
		/* [한국어] PM 참조만 내려놓는 라벨로 간다. **위 [상류 코드 관찰] 대로 이
		 * 경로는 포트의 PERST# 참조를 정리하지 않는다.** */
		goto err_init;
	}

	return 0;

err_pm_runtime_put:
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		reset_control_put(port->perst);
		list_del(&port->list);
	}
err_init:
	pm_runtime_put(dev);

	return ret;
}

/* [한국어]
 * eswin_pcie_suspend_noirq - 절전 진입을 DWC 코어에 그대로 넘긴다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 코어가 돌려준 값.
 *
 * drvdata 에서 상태를 꺼내 코어의 공용 절전 함수를 부르는 것이 전부다.
 *
 * SoC 고유의 일은 이 함수가 아니라, 코어가 그 안에서 부르는
 * eswin_pcie_pme_turn_off() 콜백을 통해 이뤄진다. 즉 **진입점은 얇고
 * 실제 개입은 콜백으로** 하는 구조다.
 *
 * noirq 단계에서 도는 이유는 코어의 구현이 그 단계를 전제하기 때문이다.
 *
 * 실행 컨텍스트: 시스템 절전의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 코어의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq == [이 함수] → dw_pcie_suspend_noirq()
 */
static int eswin_pcie_suspend_noirq(struct device *dev)
{
	struct eswin_pcie *pcie = dev_get_drvdata(dev);

	return dw_pcie_suspend_noirq(&pcie->pci);
}

/* [한국어]
 * eswin_pcie_resume_noirq - 절전 복귀를 DWC 코어에 그대로 넘긴다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 코어가 돌려준 값.
 *
 * 위 함수의 짝이며 구조가 같다.
 *
 * 복귀 과정에서 코어가 링크를 다시 세우는데, 그때 start_link 콜백을 통해
 * 이 파일의 LTSSM 켜기가 불린다.
 *
 * **host_init 을 다시 부르지 않는다.** 절전 중에도 클럭과 리셋이 그대로
 * 유지되기 때문이며, pci-exynos.c 가 복귀 경로에서 초기화를 손수 재현하는
 * 것과 대비된다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 코어의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq == [이 함수] → dw_pcie_resume_noirq()
 */
static int eswin_pcie_resume_noirq(struct device *dev)
{
	/* [한국어] drvdata 에서 상태를 되찾는다. */
	struct eswin_pcie *pcie = dev_get_drvdata(dev);

	/* [한국어] 코어의 공용 복귀 구현에 맡긴다. 그 안에서 링크를 다시 세울 때
	 * 이 파일의 start_link 콜백이 불린다. */
	return dw_pcie_resume_noirq(&pcie->pci);
}

/* [한국어] 절전 진입·복귀를 noirq 단계에 거는 매크로. 두 함수를 그 단계의
 * 콜백으로 묶어 dev_pm_ops 를 만든다. */
static DEFINE_NOIRQ_DEV_PM_OPS(eswin_pcie_pm, eswin_pcie_suspend_noirq,
				eswin_pcie_resume_noirq);

static const struct eswin_pcie_data eswin_eic7700_data = {
	/* [한국어] EIC7700 은 L2/L3 를 지원하지 않으므로 참이다. */
	.skip_l23 = true,
};

static const struct of_device_id eswin_pcie_of_match[] = {
	/* [한국어] 이 드라이버가 지원하는 유일한 SoC 와 그 매칭 데이터. */
	{ .compatible = "eswin,eic7700-pcie", .data = &eswin_eic7700_data },
	/* [한국어] 표의 끝 표시. */
	{}
};

static struct platform_driver eswin_pcie_driver = {
	/* [한국어] 장치가 붙을 때 부를 진입점. remove 는 두지 않았다. */
	.probe = eswin_pcie_probe,
	/* [한국어] 드라이버 이름과 매칭 표, 바인딩 제한, 절전 연결. */
	.driver = {
		.name = "eswin-pcie",
		/* [한국어] 위의 디바이스 트리 매칭 표를 건다. */
		.of_match_table = eswin_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = &eswin_pcie_pm,
	},
};
builtin_platform_driver(eswin_pcie_driver);

MODULE_DESCRIPTION("ESWIN PCIe Root Complex driver");
MODULE_AUTHOR("Yu Ning <ningyu@eswincomputing.com>");
MODULE_AUTHOR("Senchuan Zhang <zhangsenchuan@eswincomputing.com>");
MODULE_AUTHOR("Yanghui Ou <ouyanghui@eswincomputing.com>");
MODULE_LICENSE("GPL");
