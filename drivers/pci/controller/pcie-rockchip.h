/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Rockchip AXI PCIe controller driver
 *
 * Copyright (c) 2018 Rockchip, Inc.
 *
 * Author: Shawn Lin <shawn.lin@rock-chips.com>
 *
 */

/*
 * [한국어 설명] Rockchip AXI PCIe 컨트롤러의 레지스터 지도와 공용 선언 (pcie-rockchip.h)
 *
 * === 파일의 역할 ===
 * Rockchip SoC(RK3399 계열)에 내장된 AXI PCIe 컨트롤러의 레지스터 오프셋과
 * 비트 정의, 그리고 세 .c 파일이 공유하는 구조체와 함수 선언을 모아 둔
 * 사설 헤더다. 실행 코드는 레지스터 접근용 인라인 함수 둘뿐이다.
 *
 * 이 헤더가 따로 존재하는 이유는 pcie-rcar.h 와 같다 — 같은 하드웨어를
 * 두 드라이버가 정반대 역할로 쓰기 때문이다. pcie-rockchip-host.c 는
 * 루트 컴플렉스로, pcie-rockchip-ep.c 는 엔드포인트로 몰고, 둘이 공통으로
 * 쓰는 초기화 코드가 pcie-rockchip.c 에 있다. 모드는 struct rockchip_pcie 의
 * is_rc 필드 하나로 갈리며, 공용 코드가 그 값을 보고 분기한다
 * (pcie-rockchip.c:35, :90, :160).
 *
 * 레지스터가 오프셋 대역별로 뚜렷이 묶여 있다.
 *   0x000000  PCIE_CLIENT_*   - 모드 선택, 링크 훈련 시작, LTSSM 상태,
 *                               인터럽트 마스크와 상태. 하위 16비트를 쓰려면
 *                               상위 16비트에 쓰기 마스크를 함께 넣어야 한다.
 *   0x400000  PCIE_RC_RP_ATS_BASE
 *   0x800000  PCIE_RC_CONFIG_NORMAL_BASE / PCIE_EP_PF_CONFIG_REGS_BASE
 *   0x900000  PCIE_CORE_*     - 링크 상태, FTS, 크레딧, 코어 인터럽트, BAR 설정
 *   0xa00000  PCIE_RC_CONFIG_BASE / PCIE_EP_CONFIG_BASE - config space 창
 *   0xc00000  PCIE_CORE_AXI_* - 바깥(OB)·안쪽(IB) 주소 변환 영역
 *
 * 같은 오프셋에 이름이 둘씩 붙은 대목이 여럿이다(예: 0x800000 이
 * PCIE_RC_CONFIG_NORMAL_BASE 이자 PCIE_EP_PF_CONFIG_REGS_BASE). 호스트로
 * 쓸 때와 엔드포인트로 쓸 때 같은 창의 의미가 달라지기 때문이며,
 * 어느 이름을 쓰느냐가 곧 어느 모드의 코드인지를 드러낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더 자체는 실행되지 않는다. 세 .c 파일이 모두 include 하며,
 * Makefile 이 그 조합을 정한다.
 *
 *   pcie-rockchip.c        공용 초기화. Makefile:32(CONFIG_PCIE_ROCKCHIP).
 *                          아래 선언된 일곱 함수가 전부 여기 정의된다.
 *   pcie-rockchip-host.c   루트 컴플렉스 정책. Makefile:34.
 *                          is_rc 를 true 로 세운다(:944).
 *   pcie-rockchip-ep.c     엔드포인트 정책. Makefile:33.
 *                          is_rc 를 false 로 세운다(:852).
 *
 * 두 드라이버 중 어느 쪽이 붙든 pcie-rockchip.o 가 함께 빌드되어,
 * DT 파싱 → 전원/클럭/리셋 → PHY → 포트 초기화까지의 공통 경로를 제공한다.
 *
 * 실행 컨텍스트: 헤더라 해당 없음. 아래 인라인 두 함수는 readl/writel 한 번이라
 * 문맥 제약이 없고, 선언된 일곱 함수는 전부 프로세스 컨텍스트 전용이다
 * (클럭·리셋·레귤레이터·PHY 를 다루며 잠들 수 있다).
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더가 의존하는 것: 위 #include 여섯 줄이 명시한다 — clk.h(clk_bulk_data),
 * hw_bitfield.h(FIELD_PREP_WM16 계열), kernel.h, pci.h, pci-ecam.h,
 * reset.h(reset_control_bulk_data). pcie-rcar.h 나 pcie-iproc.h 와 달리
 * 이 헤더는 필요한 것을 스스로 include 하므로 포함 순서에 의존하지 않는다.
 * 다만 struct phy / gpio_desc / regulator / irq_domain 은 포인터로만 쓰여
 * 앞선 선언 없이도 성립한다.
 * 이 헤더에 의존하는 것: 위 세 .c 파일뿐이다.
 * 공유 상태: struct rockchip_pcie 하나. rcar 나 iproc 과 달리 이 구조체가
 * 감싸이지 않고 그대로 쓰인다 — host 판과 ep 판이 각자 더 큰 구조체를
 * 두는 대신 이 하나에 모든 필드를 담아 두고, 자기 모드에서 쓰지 않는
 * 필드는 그냥 비워 둔다. 아래 필드 주석에 어느 파일이 쓰는지를 적어 두었다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct rockchip_pcie   : 컨트롤러 하나의 모든 상태. 23개 필드가 공용·호스트
 *                          전용·엔드포인트 전용으로 갈린다.
 * rockchip_pcie_read/write() : apb_base 기준 레지스터 접근. 이 헤더의 유일한
 *                          실행 코드이며, 세 파일에서 100회 넘게 쓰인다.
 * rockchip_pcie_parse_dt()   : DT 에서 자원·전원·GPIO·레인 수를 읽는다.
 * rockchip_pcie_init_port()  : 리셋 해제, PHY 켜기, 링크 훈련까지의 공통 절차.
 * rockchip_pcie_get_phys() / _deinit_phys() : 레인별 PHY 획득과 해제.
 * rockchip_pcie_enable_clocks() / _disable_clocks() : 클럭 일괄 제어.
 * rockchip_pcie_cfg_configuration_accesses() : config 접근 종류를 설정한다.
 * rockchip_pci_pm_rsts[] / rockchip_pci_core_rsts[] : 리셋 이름 목록.
 *                          후자는 상류 주석이 해제 순서를 바꾸지 말라고 못박았다.
 *
 * === 값의 근거에 대하여 ===
 * 아래 주석은 pcie-rockchip.c / -host.c / -ep.c 에서 각 매크로와 필드가
 * 실제로 어떻게 쓰이는지를 근거로 적었다. 다만 -host.c 와 -ep.c 는 아직
 * 주석 작업을 하지 않은 파일이라 그 안의 맥락까지 깊이 확인하지는 않았고,
 * 사용 여부와 직접적인 쓰임만 grep 으로 확인했다. 사용처가 없거나 코드만으로
 * 뜻을 확정할 수 없는 것은 추측하지 않고 그렇게 밝혀 두었다.
 * Rockchip 의 하드웨어 매뉴얼은 이 트리에 없다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 헤더의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 헤더는 호스트 컨트롤러의 내부 레지스터 지도이고, NVMe 드라이버는
 * 그 컨트롤러가 만든 버스 위에 열거되는 장치를 다룰 뿐이라 계층이 다르다.
 */

/* [한국어] 헤더 가드. 세 .c 파일이 모두 include 한다. */
#ifndef _PCIE_ROCKCHIP_H
#define _PCIE_ROCKCHIP_H

/* [한국어] clk.h — struct clk_bulk_data. 아래 clks 필드가 클럭을 묶음으로 다룬다. */
#include <linux/clk.h>
/* [한국어] hw_bitfield.h — FIELD_PREP_WM16 / FIELD_PREP_WM16_CONST.
 * 아래 HWORD_SET_BIT/CLR_BIT 의 바탕이며, 이 헤더가 이 include 를 필요로 하는
 * 가장 직접적인 이유다. */
#include <linux/hw_bitfield.h>
/* [한국어] kernel.h — ARRAY_SIZE(아래 ROCKCHIP_NUM_* 가 쓴다), USEC_PER_MSEC. */
#include <linux/kernel.h>
/* [한국어] pci.h — PCI 표준 상수와 타입. */
#include <linux/pci.h>
/* [한국어] pci-ecam.h — PCIE_ECAM_OFFSET(). host 판이 config 접근 주소를 만들 때 쓴다
 * (pcie-rockchip-host.c:151, :184). */
#include <linux/pci-ecam.h>
/* [한국어] reset.h — struct reset_control_bulk_data. 아래 pm_rsts / core_rsts 가
 * 리셋 선을 묶음으로 다룬다. */
#include <linux/reset.h>

/*
 * The upper 16 bits of PCIE_CLIENT_CONFIG are a write mask for the lower 16
 * bits.  This allows atomic updates of the register without locking.
 */
/* [한국어] 하위 16비트의 비트를 1 로 만드는 값을 만든다. 상류 주석이 그 원리를 밝히듯,
 * 이 컨트롤러의 PCIE_CLIENT_CONFIG 는 상위 16비트가 하위 16비트의 쓰기 마스크라
 * 원하는 비트만 골라 바꿀 수 있다. 그래서 읽고-고쳐-쓰기 없이, 따라서 락 없이
 * 원자적 갱신이 된다. */
#define HWORD_SET_BIT(val)		(FIELD_PREP_WM16_CONST((val), 1))
/* [한국어] 같은 방식으로 비트를 0 으로 만드는 값. 마스크는 세우되 데이터 비트를 0 으로 둔다. */
#define HWORD_CLR_BIT(val)		(FIELD_PREP_WM16_CONST((val), 0))

/* [한국어] 레인 수를 레지스터 인코딩으로 바꾼다. x>>1 & 3 이므로 1→0, 2→1, 4→2 가 되어
 * 2의 거듭제곱 레인 수를 2비트 필드에 담는다. 8레인이면 3 이 되겠지만
 * 아래 MAX_LANE_NUM 이 4 라 그 경우는 없다. */
#define ENCODE_LANES(x)			((((x) >> 1) & 3))
/* [한국어] 이 컨트롤러가 지원하는 최대 레인 수. 아래 phys[] 배열의 크기이기도 하다. */
#define MAX_LANE_NUM			4
/* [한국어] 바깥 방향 주소 변환 영역의 최대 번호. 아래
 * ROCKCHIP_PCIE_AT_OB_REGION_* 매크로가 (r) & 0x1f 로 자르는 것과 맞는다.
 * 이 헤더 안에는 사용처가 없고, 쓰는 곳은 -host.c / -ep.c 다. */
#define MAX_REGION_LIMIT		32
/* [한국어] 엔드포인트 BAR 의 최소 aperture 지수로 보인다. 이 헤더 안에는 사용처가 없어
 * 정확한 의미는 확인 못 함. */
#define MIN_EP_APERTURE			28
/* [한국어] 링크 훈련 대기 상한(500ms 를 마이크로초로). 이 헤더 안에는 사용처가 없고
 * 공용 초기화 코드가 쓴다. */
#define LINK_TRAIN_TIMEOUT		(500 * USEC_PER_MSEC)

/* [한국어] PCIE_CLIENT 대역의 기준 오프셋. 0 이라 아래 상수들의 값이 곧 오프셋이지만,
 * 대역을 이름으로 드러내려고 명시적으로 더한다. */
#define PCIE_CLIENT_BASE		0x0
/* [한국어] 클라이언트 설정 레지스터. 모드(RC/EP), 링크 훈련 시작, 레인 수, 세대 선택이
 * 모두 이 한 워드에 모여 있다. 위 쓰기 마스크 방식 덕분에 각 설정을 따로 쓸 수 있다. */
#define PCIE_CLIENT_CONFIG		(PCIE_CLIENT_BASE + 0x00)
/* [한국어] 컨트롤러 활성(0번 비트를 1로). */
#define   PCIE_CLIENT_CONF_ENABLE		HWORD_SET_BIT(0x0001)
/* [한국어] 컨트롤러 비활성(0번 비트를 0으로). */
#define   PCIE_CLIENT_CONF_DISABLE		HWORD_CLR_BIT(0x0001)
/* [한국어] 링크 훈련 시작(1번 비트를 1로). 이 비트가 LTSSM 을 돌린다. */
#define   PCIE_CLIENT_LINK_TRAIN_ENABLE		HWORD_SET_BIT(0x0002)
/* [한국어] 링크 훈련 중지(1번 비트를 0으로). */
#define   PCIE_CLIENT_LINK_TRAIN_DISABLE	HWORD_CLR_BIT(0x0002)
/* [한국어] ARI(Alternative Routing-ID) 활성(3번 비트). 한 장치가 8개 넘는 기능을
 * 가질 수 있게 하는 확장이다. */
#define   PCIE_CLIENT_ARI_ENABLE		HWORD_SET_BIT(0x0008)
/* [한국어] 레인 수 필드(5:4)에 인코딩된 값을 넣는다. 위 ENCODE_LANES 로 변환한 뒤
 * 쓰기 마스크와 함께 채운다. */
#define   PCIE_CLIENT_CONF_LANE_NUM(x)		FIELD_PREP_WM16(0x0030, ENCODE_LANES(x))
/* [한국어] 루트 컴플렉스 모드(6번 비트를 1로). host 판이 쓰는 값이다. */
#define   PCIE_CLIENT_MODE_RC			HWORD_SET_BIT(0x0040)
/* [한국어] 엔드포인트 모드(6번 비트를 0으로). ep 판이 쓰는 값이며,
 * 이 한 비트가 두 드라이버의 하드웨어적 갈림길이다. */
#define   PCIE_CLIENT_MODE_EP			HWORD_CLR_BIT(0x0040)
/* [한국어] Gen1(2.5GT/s) 선택(7번 비트를 0으로). */
#define   PCIE_CLIENT_GEN_SEL_1			HWORD_CLR_BIT(0x0080)
/* [한국어] Gen2(5GT/s) 선택(7번 비트를 1로). 아래 link_gen 필드가 어느 쪽을 쓸지 정한다. */
#define   PCIE_CLIENT_GEN_SEL_2			HWORD_SET_BIT(0x0080)
/* [한국어] 레거시 INTx 제어 레지스터. 엔드포인트 모드에서 INTx 를 직접 올리고 내리는 데 쓴다. */
#define PCIE_CLIENT_LEGACY_INT_CTRL	(PCIE_CLIENT_BASE + 0x0c)
/* [한국어] INTx assert(1번 비트를 1로). */
#define   PCIE_CLIENT_INT_IN_ASSERT		HWORD_SET_BIT(0x0002)
/* [한국어] INTx deassert(1번 비트를 0으로). 이 둘을 짝지어 한 번의 INTx 를 만든다. */
#define   PCIE_CLIENT_INT_IN_DEASSERT		HWORD_CLR_BIT(0x0002)
/* [한국어] 인터럽트 대기 상태를 pending 으로 표시(0번 비트를 1로). */
#define   PCIE_CLIENT_INT_PEND_ST_PEND		HWORD_SET_BIT(0x0001)
/* [한국어] 같은 비트를 normal 로 되돌린다. */
#define   PCIE_CLIENT_INT_PEND_ST_NORMAL	HWORD_CLR_BIT(0x0001)
/* [한국어] 사이드밴드 상태 레지스터. */
#define PCIE_CLIENT_SIDE_BAND_STATUS	(PCIE_CLIENT_BASE + 0x20)
/* [한국어] PHY 상태 비트(12번). PHY 가 준비됐는지 확인하는 데 쓰인다. */
#define   PCIE_CLIENT_PHY_ST			BIT(12)
/* [한국어] 디버그 출력 레지스터 0. LTSSM 의 현재 상태를 읽는 창이다. */
#define PCIE_CLIENT_DEBUG_OUT_0		(PCIE_CLIENT_BASE + 0x3c)
/* [한국어] LTSSM 상태 필드 마스크(5:0). 아래 PCIE_LINK_IS_L2 매크로가 이 마스크를 쓴다. */
#define   PCIE_CLIENT_DEBUG_LTSSM_MASK		GENMASK(5, 0)
/* [한국어] LTSSM 이 L1 절전 상태일 때의 값. 이 헤더 안에는 사용처가 없다. */
#define   PCIE_CLIENT_DEBUG_LTSSM_L1		0x18
/* [한국어] LTSSM 이 L2 상태일 때의 값. 아래 PCIE_LINK_IS_L2 가 이 값과 비교한다. */
#define   PCIE_CLIENT_DEBUG_LTSSM_L2		0x19
/* [한국어] 기본 상태 레지스터 0. 협상된 링크 폭과 속도가 여기 있다. */
#define PCIE_CLIENT_BASIC_STATUS0	(PCIE_CLIENT_BASE + 0x44)
/* [한국어] 협상된 링크 폭 필드 마스크(7:6). */
#define   PCIE_CLIENT_NEG_LINK_WIDTH_MASK	GENMASK(7, 6)
/* [한국어] 그 필드의 시작 비트(6). 마스크와 시프트를 쌍으로 두는 이 파일의 관례다. */
#define   PCIE_CLIENT_NEG_LINK_WIDTH_SHIFT	6
/* [한국어] 협상된 링크 속도 비트(5). 서 있으면 Gen2 로 붙었다는 뜻으로 보이나,
 * 이 헤더 안에는 사용처가 없어 확인 못 함. */
#define   PCIE_CLIENT_NEG_LINK_SPEED		BIT(5)
/* [한국어] 기본 상태 레지스터 1. 링크가 올라왔는지를 담는다. */
#define PCIE_CLIENT_BASIC_STATUS1	(PCIE_CLIENT_BASE + 0x48)
/* [한국어] 링크 업 상태값(0x00300000). 아래 PCIE_LINK_UP 이 이 값과 비교한다. */
#define   PCIE_CLIENT_LINK_STATUS_UP		0x00300000
/* [한국어] 그 판정에 쓸 마스크. 값과 마스크가 같은데, 해당 비트가 모두 서야
 * 링크 업으로 보기 때문이다. */
#define   PCIE_CLIENT_LINK_STATUS_MASK		0x00300000
/* [한국어] 클라이언트 인터럽트 마스크 레지스터. 아래 상태 비트들과 같은 배치를 갖는다. */
#define PCIE_CLIENT_INT_MASK		(PCIE_CLIENT_BASE + 0x4c)
/* [한국어] 클라이언트 인터럽트 상태 레지스터. 아래 비트들이 이 레지스터의 자리다. */
#define PCIE_CLIENT_INT_STATUS		(PCIE_CLIENT_BASE + 0x50)
/* [한국어] INTx 네 개가 모여 있는 필드의 마스크(8:5). */
#define   PCIE_CLIENT_INTR_MASK			GENMASK(8, 5)
/* [한국어] 그 필드의 시작 비트(5). INTA 가 5번이라는 뜻이다. */
#define   PCIE_CLIENT_INTR_SHIFT		5
/* [한국어] 레거시 인터럽트 전송 완료(15번). */
#define   PCIE_CLIENT_INT_LEGACY_DONE		BIT(15)
/* [한국어] 메시지 수신(14번). */
#define   PCIE_CLIENT_INT_MSG			BIT(14)
/* [한국어] 핫 리셋 수신(13번). */
#define   PCIE_CLIENT_INT_HOT_RST		BIT(13)
/* [한국어] DPA(Dynamic Power Allocation) 관련(12번). */
#define   PCIE_CLIENT_INT_DPA			BIT(12)
/* [한국어] 치명적 오류(11번). */
#define   PCIE_CLIENT_INT_FATAL_ERR		BIT(11)
/* [한국어] 치명적이지 않은 오류(10번). */
#define   PCIE_CLIENT_INT_NFATAL_ERR		BIT(10)
/* [한국어] 정정 가능한 오류(9번). */
#define   PCIE_CLIENT_INT_CORR_ERR		BIT(9)
/* [한국어] INTD(8번). */
#define   PCIE_CLIENT_INT_INTD			BIT(8)
/* [한국어] INTC(7번). */
#define   PCIE_CLIENT_INT_INTC			BIT(7)
/* [한국어] INTB(6번). */
#define   PCIE_CLIENT_INT_INTB			BIT(6)
/* [한국어] INTA(5번). 위 INTR_SHIFT 가 5 인 근거이며, 네 INTx 가 5~8번에 연속 배치된다. */
#define   PCIE_CLIENT_INT_INTA			BIT(5)
/* [한국어] 로컬 인터럽트(4번). */
#define   PCIE_CLIENT_INT_LOCAL			BIT(4)
/* [한국어] UDMA 관련(3번). */
#define   PCIE_CLIENT_INT_UDMA			BIT(3)
/* [한국어] PHY 인터럽트(2번). */
#define   PCIE_CLIENT_INT_PHY			BIT(2)
/* [한국어] 핫플러그(1번). */
#define   PCIE_CLIENT_INT_HOT_PLUG		BIT(1)
/* [한국어] 전원 상태 변경(0번). */
#define   PCIE_CLIENT_INT_PWR_STCG		BIT(0)

/* [한국어] 위 네 INTx 비트를 한데 묶은 값. 레거시 인터럽트를 통째로 켜고 끌 때 쓴다. */
#define PCIE_CLIENT_INT_LEGACY \
	(PCIE_CLIENT_INT_INTA | PCIE_CLIENT_INT_INTB | \
	PCIE_CLIENT_INT_INTC | PCIE_CLIENT_INT_INTD)

/* [한국어] 클라이언트 인터럽트 중 드라이버가 실제로 다루는 것들을 묶은 값.
 * 오류 계열, 핫 리셋, 메시지, 레거시 완료, INTx 넷, PHY 를 포함한다.
 * LOCAL/UDMA/HOT_PLUG/PWR_STCG 가 빠져 있는데, 그 사건들을 이 드라이버가
 * 처리하지 않는다는 뜻으로 보인다. */
#define PCIE_CLIENT_INT_CLI \
	(PCIE_CLIENT_INT_CORR_ERR | PCIE_CLIENT_INT_NFATAL_ERR | \
	PCIE_CLIENT_INT_FATAL_ERR | PCIE_CLIENT_INT_DPA | \
	PCIE_CLIENT_INT_HOT_RST | PCIE_CLIENT_INT_MSG | \
	PCIE_CLIENT_INT_LEGACY_DONE | PCIE_CLIENT_INT_LEGACY | \
	PCIE_CLIENT_INT_PHY)

/* [한국어] PCIE_CORE 관리 대역의 기준 오프셋. 클라이언트 대역과 달리 0 이 아니다. */
#define PCIE_CORE_CTRL_MGMT_BASE	0x900000
/* [한국어] 코어 제어 레지스터. 링크 훈련 완료 여부, 협상 속도와 폭이 여기 있다. */
#define PCIE_CORE_CTRL			(PCIE_CORE_CTRL_MGMT_BASE + 0x000)
/* [한국어] 링크 훈련 상태 필드 마스크. 아래 PCIE_LINK_TRAINING_DONE 이 쓴다. */
#define   PCIE_CORE_PL_CONF_LS_MASK		0x00000001
/* [한국어] 훈련 완료를 뜻하는 값. 마스크와 같은 값이라 0번 비트가 서면 완료다. */
#define   PCIE_CORE_PL_CONF_LS_READY		0x00000001
/* [한국어] 5GT/s(Gen2)로 붙었을 때의 속도 필드 값. */
#define   PCIE_CORE_PL_CONF_SPEED_5G		0x00000008
/* [한국어] 속도 필드 마스크(4:3). 아래 PCIE_LINK_IS_GEN2 가 이 둘을 함께 쓴다. */
#define   PCIE_CORE_PL_CONF_SPEED_MASK		0x00000018
/* [한국어] 레인 수 필드 마스크(2:1). */
#define   PCIE_CORE_PL_CONF_LANE_MASK		0x00000006
/* [한국어] 그 필드의 시작 비트(1). */
#define   PCIE_CORE_PL_CONF_LANE_SHIFT		1
/* [한국어] 물리 계층 설정 레지스터 1. FTS 개수를 담는다. */
#define PCIE_CORE_CTRL_PLC1		(PCIE_CORE_CTRL_MGMT_BASE + 0x004)
/* [한국어] FTS(Fast Training Sequence) 개수 필드 마스크(23:8). */
#define   PCIE_CORE_CTRL_PLC1_FTS_MASK		GENMASK(23, 8)
/* [한국어] 그 필드의 시작 비트(8). */
#define   PCIE_CORE_CTRL_PLC1_FTS_SHIFT		8
/* [한국어] 설정할 FTS 개수(0xffff). 최대값을 쓰는데, 절전에서 복귀할 때 링크가
 * 안정적으로 재동기화되도록 훈련 시퀀스를 넉넉히 보내려는 것으로 보인다. */
#define   PCIE_CORE_CTRL_PLC1_FTS_CNT		0xffff
/* [한국어] 송신 크레딧 설정 레지스터 1. */
#define PCIE_CORE_TXCREDIT_CFG1		(PCIE_CORE_CTRL_MGMT_BASE + 0x020)
/* [한국어] MUI(Maximum Update Interval) 필드 마스크(31:16). */
#define   PCIE_CORE_TXCREDIT_CFG1_MUI_MASK	0xFFFF0000
/* [한국어] 그 필드의 시작 비트(16). */
#define   PCIE_CORE_TXCREDIT_CFG1_MUI_SHIFT	16
/* [한국어] 크레딧 갱신 간격을 인코딩한다. x>>3 으로 8로 나눈 뒤 자리로 민다 —
 * 하드웨어가 8단위로만 표현하기 때문으로 보이나, 그 근거는 확인 못 함. */
#define   PCIE_CORE_TXCREDIT_CFG1_MUI_ENCODE(x) \
		(((x) >> 3) << PCIE_CORE_TXCREDIT_CFG1_MUI_SHIFT)
/* [한국어] 레인 매핑 레지스터. */
#define PCIE_CORE_LANE_MAP             (PCIE_CORE_CTRL_MGMT_BASE + 0x200)
/* [한국어] 레인 맵 필드 마스크(3:0). 레인 하나에 비트 하나씩이며 MAX_LANE_NUM 과 맞는다. */
#define   PCIE_CORE_LANE_MAP_MASK              0x0000000f
/* [한국어] 레인 순서 반전 비트(16번). 보드 배선이 뒤집혀 있을 때 쓴다. */
#define   PCIE_CORE_LANE_MAP_REVERSE           BIT(16)
/* [한국어] 코어 인터럽트 상태 레지스터. 아래 비트들이 이 레지스터의 자리다. */
#define PCIE_CORE_INT_STATUS		(PCIE_CORE_CTRL_MGMT_BASE + 0x20c)
/* [한국어] Posted Request FIFO Parity Error(0번). */
#define   PCIE_CORE_INT_PRFPE			BIT(0)
/* [한국어] Completion Request FIFO Parity Error(1번). */
#define   PCIE_CORE_INT_CRFPE			BIT(1)
/* [한국어] Replay Request FIFO Parity Error(2번). */
#define   PCIE_CORE_INT_RRPE			BIT(2)
/* [한국어] Posted Request FIFO Overflow(3번). */
#define   PCIE_CORE_INT_PRFO			BIT(3)
/* [한국어] Completion Request FIFO Overflow(4번). */
#define   PCIE_CORE_INT_CRFO			BIT(4)
/* [한국어] Replay Timer(5번). */
#define   PCIE_CORE_INT_RT			BIT(5)
/* [한국어] Replay Timer Rollover(6번). */
#define   PCIE_CORE_INT_RTR			BIT(6)
/* [한국어] Phy Error(7번). */
#define   PCIE_CORE_INT_PE			BIT(7)
/* [한국어] Malformed TLP Received(8번). */
#define   PCIE_CORE_INT_MTR			BIT(8)
/* [한국어] Unexpected Completion Received(9번). */
#define   PCIE_CORE_INT_UCR			BIT(9)
/* [한국어] Flow Control Error(10번). */
#define   PCIE_CORE_INT_FCE			BIT(10)
/* [한국어] Completion Timeout(11번). */
#define   PCIE_CORE_INT_CT			BIT(11)
/* [한국어] Unsupported Type Completion(18번). 12~17번이 비어 있다. */
#define   PCIE_CORE_INT_UTC			BIT(18)
/* [한국어] Multiple Message Vector Change(19번). 이름은 축약형이라 풀이가 확실하지 않으며,
 * 이 트리에서 근거를 확인하지 못했다. */
#define   PCIE_CORE_INT_MMVC			BIT(19)
/* [한국어] 코어 벤더 ID 설정 레지스터. 오프셋이 0x44 로 위 INT_STATUS(0x20c)보다
 * 앞인데 정의는 뒤에 있다 — 상류 배치 그대로 둔다. */
#define PCIE_CORE_CONFIG_VENDOR		(PCIE_CORE_CTRL_MGMT_BASE + 0x44)
/* [한국어] 코어 인터럽트 마스크 레지스터. 위 상태 레지스터와 같은 비트 배치를 갖는다. */
#define PCIE_CORE_INT_MASK		(PCIE_CORE_CTRL_MGMT_BASE + 0x210)
/* [한국어] PHY 기능 설정 레지스터. 이 헤더 안에는 사용처가 없다. */
#define PCIE_CORE_PHY_FUNC_CFG		(PCIE_CORE_CTRL_MGMT_BASE + 0x2c0)
/* [한국어] 루트 컴플렉스 BAR 설정 레지스터. 아래 여섯 상수가 이 레지스터에 넣을 값이다. */
#define PCIE_RC_BAR_CONF		(PCIE_CORE_CTRL_MGMT_BASE + 0x300)
/* [한국어] BAR 사용 안 함(0). */
#define ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_DISABLED		0x0
/* [한국어] 32비트 I/O BAR(1). */
#define ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_IO_32BITS		0x1
/* [한국어] 32비트 메모리 BAR(4). 2, 3 이 비어 있는 것은 PCI 규격의 BAR 타입 인코딩을
 * 그대로 따르기 때문으로 보인다. */
#define ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_MEM_32BITS		0x4
/* [한국어] 32비트 prefetchable 메모리 BAR(5). */
#define ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_PREFETCH_MEM_32BITS	0x5
/* [한국어] 64비트 메모리 BAR(6). */
#define ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_MEM_64BITS		0x6
/* [한국어] 64비트 prefetchable 메모리 BAR(7). */
#define ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_PREFETCH_MEM_64BITS	0x7

/* [한국어] 코어 인터럽트 중 드라이버가 다루는 것들을 묶은 값.
 * 위에 정의된 열넷 중 PCIE_CORE_INT_PRFO 하나만 빠져 있다 —
 * 의도인지 누락인지는 이 트리에서 확인하지 못했다. */
#define PCIE_CORE_INT \
		(PCIE_CORE_INT_PRFPE | PCIE_CORE_INT_CRFPE | \
		 PCIE_CORE_INT_RRPE | PCIE_CORE_INT_CRFO | \
		 PCIE_CORE_INT_RT | PCIE_CORE_INT_RTR | \
		 PCIE_CORE_INT_PE | PCIE_CORE_INT_MTR | \
		 PCIE_CORE_INT_UCR | PCIE_CORE_INT_FCE | \
		 PCIE_CORE_INT_CT | PCIE_CORE_INT_UTC | \
		 PCIE_CORE_INT_MMVC)

/* [한국어] 루트 포트 ATS(Address Translation Services) 대역. 이 헤더 안에는 사용처가 없다. */
#define PCIE_RC_RP_ATS_BASE		0x400000
/* [한국어] 루트 컴플렉스 모드에서 본 config 창의 기준. */
#define PCIE_RC_CONFIG_NORMAL_BASE	0x800000
/* [한국어] 엔드포인트 모드에서 본 물리 기능 config 창의 기준. 위와 값이 같은데,
 * 같은 하드웨어 창을 모드에 따라 다르게 부르는 것이다. 어느 이름을 쓰느냐가
 * 곧 그 코드가 어느 모드용인지를 드러낸다. */
#define PCIE_EP_PF_CONFIG_REGS_BASE	0x800000
/* [한국어] 루트 컴플렉스의 자기 config space 창. */
#define PCIE_RC_CONFIG_BASE		0xa00000
/* [한국어] 엔드포인트의 config space 창. 역시 값이 같고 이름만 다르다. */
#define PCIE_EP_CONFIG_BASE		0xa00000
/* [한국어] 엔드포인트의 Device ID / Vendor ID 자리. */
#define PCIE_EP_CONFIG_DID_VID		(PCIE_EP_CONFIG_BASE + 0x00)
/* [한국어] 엔드포인트의 Link Control/Status 자리(0xd0). */
#define PCIE_EP_CONFIG_LCS		(PCIE_EP_CONFIG_BASE + 0xd0)
/* [한국어] 루트 컴플렉스의 Revision ID / Class Code 자리(0x08). */
#define PCIE_RC_CONFIG_RID_CCR		(PCIE_RC_CONFIG_BASE + 0x08)
/* [한국어] 루트 컴플렉스의 Command 레지스터 자리(0xc0)로 보이나,
 * 표준 config 에서 0xc0 은 capability 영역이라 이 트리에서 확정하지 못했다. */
#define PCIE_RC_CONFIG_CR		(PCIE_RC_CONFIG_BASE + 0xc0)
/* [한국어] L1 하위 상태 제어 레지스터 2. */
#define PCIE_RC_CONFIG_L1_SUBSTATE_CTRL2 (PCIE_RC_CONFIG_BASE + 0x90c)
/* [한국어] THP(TLP Processing Hints) capability 자리. */
#define PCIE_RC_CONFIG_THP_CAP		(PCIE_RC_CONFIG_BASE + 0x274)
/* [한국어] 그 capability 의 next 포인터 필드 마스크(31:20). capability 목록을
 * 끊거나 이어 붙일 때 쓰는 것으로 보인다. */
#define   PCIE_RC_CONFIG_THP_CAP_NEXT_MASK	GENMASK(31, 20)

/* [한국어] 루트 포트에서 쓸 수 있는 안쪽 방향 AXI 영역 개수(3). */
#define MAX_AXI_IB_ROOTPORT_REGION_NUM		3
/* [한국어] AXI 주소에서 변환 없이 그대로 통과시킬 하위 비트 수(8).
 * 즉 256바이트 단위로 변환이 이뤄진다. */
#define MIN_AXI_ADDR_BITS_PASSED		8
/* [한국어] 그 위쪽 비트를 덮는 마스크(63:8). 아래 OB/IB 영역의 주소 필드가 이 마스크를 쓴다. */
#define PCIE_ADDR_MASK			GENMASK_ULL(63, MIN_AXI_ADDR_BITS_PASSED)
/* [한국어] AXI 주소 변환 설정 대역의 기준 오프셋. */
#define PCIE_CORE_AXI_CONF_BASE		0xc00000
/* [한국어] 바깥 방향 영역 0 의 주소 레지스터 0(하위). */
#define PCIE_CORE_OB_REGION_ADDR0	(PCIE_CORE_AXI_CONF_BASE + 0x0)
/* [한국어] 그 안의 "통과시킬 비트 수" 필드 마스크(0x3f, 6비트).
 * 영역 크기를 비트 수로 표현하는 방식이다. */
#define   PCIE_CORE_OB_REGION_ADDR0_NUM_BITS	0x3f
/* [한국어] 같은 레지스터의 주소 필드. 위 PCIE_ADDR_MASK 를 그대로 쓴다. */
#define   PCIE_CORE_OB_REGION_ADDR0_LO_ADDR	PCIE_ADDR_MASK
/* [한국어] 바깥 방향 영역 0 의 주소 레지스터 1(상위 32비트). */
#define PCIE_CORE_OB_REGION_ADDR1	(PCIE_CORE_AXI_CONF_BASE + 0x4)
/* [한국어] 바깥 방향 영역 0 의 서술자 0. 트랜잭션 종류(아래 AXI_WRAPPER_*)가 여기 들어간다. */
#define PCIE_CORE_OB_REGION_DESC0	(PCIE_CORE_AXI_CONF_BASE + 0x8)
/* [한국어] 바깥 방향 영역 0 의 서술자 1. */
#define PCIE_CORE_OB_REGION_DESC1	(PCIE_CORE_AXI_CONF_BASE + 0xc)

/* [한국어] 안쪽 방향 AXI 변환 대역의 기준 오프셋. */
#define PCIE_CORE_AXI_INBOUND_BASE	0xc00800
/* [한국어] 루트 포트의 안쪽 방향 주소 레지스터 0(하위). */
#define PCIE_RP_IB_ADDR0		(PCIE_CORE_AXI_INBOUND_BASE + 0x0)
/* [한국어] 그 안의 비트 수 필드 마스크. 바깥 쪽과 같은 형식이다. */
#define   PCIE_CORE_IB_REGION_ADDR0_NUM_BITS	0x3f
/* [한국어] 주소 필드. 역시 같은 마스크를 쓴다. */
#define   PCIE_CORE_IB_REGION_ADDR0_LO_ADDR	PCIE_ADDR_MASK
/* [한국어] 루트 포트의 안쪽 방향 주소 레지스터 1(상위). */
#define PCIE_RP_IB_ADDR1		(PCIE_CORE_AXI_INBOUND_BASE + 0x4)

/* Size of one AXI Region (not Region 0) */
/* [한국어] 영역 0 을 제외한 AXI 영역 하나의 크기(1MB). 상류 주석이 그 단서를 준다. */
#define AXI_REGION_SIZE				BIT(20)
/* Size of Region 0, equal to sum of sizes of other regions */
/* [한국어] 영역 0 의 크기(32MB). 상류 주석대로 나머지 영역들의 크기 합과 같다 —
 * 1MB × 32 다. */
#define AXI_REGION_0_SIZE			(32 * (0x1 << 20))
/* [한국어] 바깥 영역 크기 필드의 시작 비트(5). */
#define OB_REG_SIZE_SHIFT			5
/* [한국어] 루트 포트 안쪽 영역 크기 필드의 시작 비트(3). */
#define IB_ROOT_PORT_REG_SIZE_SHIFT		3
/* [한국어] AXI 래퍼 트랜잭션 종류 - I/O 쓰기(0x6). */
#define AXI_WRAPPER_IO_WRITE			0x6
/* [한국어] 메모리 쓰기(0x2). */
#define AXI_WRAPPER_MEM_WRITE			0x2
/* [한국어] type 0 config(0xa). */
#define AXI_WRAPPER_TYPE0_CFG			0xa
/* [한국어] type 1 config(0xb). */
#define AXI_WRAPPER_TYPE1_CFG			0xb
/* [한국어] 일반 메시지(0xc). 위 서술자 레지스터에 이 값을 넣어 그 영역으로 나가는
 * 트랜잭션의 종류를 정한다. */
#define AXI_WRAPPER_NOR_MSG			0xc

/* [한국어] PME_Turn_Off 메시지를 보내는 레지스터 오프셋. 다른 상수들과 달리 대역
 * 기준이 붙지 않은 절대 오프셋이다. */
#define PCIE_RC_SEND_PME_OFF			0x11960
/* [한국어] LTSSM 이 L2 상태인지 판정한다. 위 DEBUG_LTSSM_MASK 로 필드를 떼어
 * DEBUG_LTSSM_L2 와 비교한다. */
#define PCIE_LINK_IS_L2(x) \
	(((x) & PCIE_CLIENT_DEBUG_LTSSM_MASK) == PCIE_CLIENT_DEBUG_LTSSM_L2)
/* [한국어] 링크 훈련이 끝났는지 판정한다. 코어 제어 레지스터의 LS 필드를 본다. */
#define PCIE_LINK_TRAINING_DONE(x) \
	(((x) & PCIE_CORE_PL_CONF_LS_MASK) == PCIE_CORE_PL_CONF_LS_READY)
/* [한국어] 링크가 올라왔는지 판정한다. 클라이언트 기본 상태 1 의 필드를 본다.
 * 위 세 판정이 서로 다른 레지스터를 보는 점이 눈에 띈다 — 훈련 완료,
 * 링크 업, LTSSM 상태가 각각 다른 관점의 정보이기 때문이다. */
#define PCIE_LINK_UP(x) \
	(((x) & PCIE_CLIENT_LINK_STATUS_MASK) == PCIE_CLIENT_LINK_STATUS_UP)
/* [한국어] 협상 속도가 Gen2 인지 판정한다. */
#define PCIE_LINK_IS_GEN2(x) \
	(((x) & PCIE_CORE_PL_CONF_SPEED_MASK) == PCIE_CORE_PL_CONF_SPEED_5G)

/* [한국어] 영역 0 의 주소 변환 상위 워드 값(0). 변환 없이 통과시킨다는 뜻이다. */
#define RC_REGION_0_ADDR_TRANS_H		0x00000000
/* [한국어] 같은 하위 워드 값(0). */
#define RC_REGION_0_ADDR_TRANS_L		0x00000000
/* [한국어] 영역 0 이 통과시킬 비트 수(24). 25 - 1 로 적은 것은 하드웨어가 0 기반으로
 * 세기 때문이다. 2^25 = 32MB 로 위 AXI_REGION_0_SIZE 와 맞아떨어진다. */
#define RC_REGION_0_PASS_BITS			(25 - 1)
#define RC_REGION_0_TYPE_MASK			GENMASK(3, 0)
#define MAX_AXI_WRAPPER_REGION_NUM		33

#define ROCKCHIP_PCIE_MSG_ROUTING_MASK			GENMASK(7, 5)
#define ROCKCHIP_PCIE_MSG_ROUTING(route) \
	(((route) << 5) & ROCKCHIP_PCIE_MSG_ROUTING_MASK)
#define ROCKCHIP_PCIE_MSG_CODE_MASK			GENMASK(15, 8)
#define ROCKCHIP_PCIE_MSG_CODE(code) \
	(((code) << 8) & ROCKCHIP_PCIE_MSG_CODE_MASK)
#define ROCKCHIP_PCIE_MSG_NO_DATA			BIT(16)

#define ROCKCHIP_PCIE_EP_CMD_STATUS			0x4
#define   ROCKCHIP_PCIE_EP_CMD_STATUS_IS		BIT(19)
#define ROCKCHIP_PCIE_EP_MSI_CTRL_REG			0x90
#define   ROCKCHIP_PCIE_EP_MSI_CP1_OFFSET		8
#define   ROCKCHIP_PCIE_EP_MSI_CP1_MASK			GENMASK(15, 8)
#define   ROCKCHIP_PCIE_EP_MSI_FLAGS_OFFSET		16
#define   ROCKCHIP_PCIE_EP_MSI_CTRL_MMC_OFFSET		17
#define   ROCKCHIP_PCIE_EP_MSI_CTRL_MMC_MASK		GENMASK(19, 17)
#define   ROCKCHIP_PCIE_EP_MSI_CTRL_MME_OFFSET		20
#define   ROCKCHIP_PCIE_EP_MSI_CTRL_MME_MASK		GENMASK(22, 20)
#define   ROCKCHIP_PCIE_EP_MSI_CTRL_ME				BIT(16)
#define   ROCKCHIP_PCIE_EP_MSI_CTRL_MASK_MSI_CAP	BIT(24)
#define ROCKCHIP_PCIE_EP_MSIX_CAP_REG			0xb0
#define   ROCKCHIP_PCIE_EP_MSIX_CAP_CP_OFFSET		8
#define   ROCKCHIP_PCIE_EP_MSIX_CAP_CP_MASK		GENMASK(15, 8)
#define ROCKCHIP_PCIE_EP_DUMMY_IRQ_ADDR				0x1
#define ROCKCHIP_PCIE_EP_PCI_LEGACY_IRQ_ADDR		0x3

#define ROCKCHIP_PCIE_AT_MIN_NUM_BITS	8
#define ROCKCHIP_PCIE_AT_MAX_NUM_BITS	20
#define ROCKCHIP_PCIE_AT_SIZE_ALIGN	(1UL << ROCKCHIP_PCIE_AT_MIN_NUM_BITS)

#define ROCKCHIP_PCIE_EP_FUNC_BASE(fn) \
	(PCIE_EP_PF_CONFIG_REGS_BASE + (((fn) << 12) & GENMASK(19, 12)))
#define ROCKCHIP_PCIE_EP_VIRT_FUNC_BASE(fn) \
	(PCIE_EP_PF_CONFIG_REGS_BASE + 0x10000 + (((fn) << 12) & GENMASK(19, 12)))

#define ROCKCHIP_PCIE_AT_MIN_NUM_BITS  8
#define ROCKCHIP_PCIE_AT_MAX_NUM_BITS  20
#define ROCKCHIP_PCIE_AT_SIZE_ALIGN    (1UL << ROCKCHIP_PCIE_AT_MIN_NUM_BITS)

#define ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar) \
	(PCIE_CORE_AXI_CONF_BASE + 0x0828 + (fn) * 0x0040 + (bar) * 0x0008)
#define ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar) \
	(PCIE_CORE_AXI_CONF_BASE + 0x082c + (fn) * 0x0040 + (bar) * 0x0008)
#define ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK	GENMASK(19, 12)
#define ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) \
	(((devfn) << 12) & \
		 ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK)
#define ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0_BUS_MASK	GENMASK(27, 20)
#define ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0_BUS(bus) \
		(((bus) << 20) & ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0_BUS_MASK)
#define PCIE_RC_EP_ATR_OB_REGIONS_1_32 (PCIE_CORE_AXI_CONF_BASE + 0x0020)
#define ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0(r) \
		(PCIE_RC_EP_ATR_OB_REGIONS_1_32 + 0x0000 + ((r) & 0x1f) * 0x0020)
#define ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR1(r) \
		(PCIE_RC_EP_ATR_OB_REGIONS_1_32 + 0x0004 + ((r) & 0x1f) * 0x0020)
#define ROCKCHIP_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID	BIT(23)
#define ROCKCHIP_PCIE_AT_OB_REGION_DESC0_DEVFN_MASK	GENMASK(31, 24)
#define ROCKCHIP_PCIE_AT_OB_REGION_DESC0_DEVFN(devfn) \
		(((devfn) << 24) & ROCKCHIP_PCIE_AT_OB_REGION_DESC0_DEVFN_MASK)
#define ROCKCHIP_PCIE_AT_OB_REGION_DESC0(r) \
		(PCIE_RC_EP_ATR_OB_REGIONS_1_32 + 0x0008 + ((r) & 0x1f) * 0x0020)
#define ROCKCHIP_PCIE_AT_OB_REGION_DESC1(r) \
		(PCIE_RC_EP_ATR_OB_REGIONS_1_32 + 0x000c + ((r) & 0x1f) * 0x0020)
#define ROCKCHIP_PCIE_AT_OB_REGION_DESC2(r) \
		(PCIE_RC_EP_ATR_OB_REGIONS_1_32 + 0x0010 + ((r) & 0x1f) * 0x0020)

#define ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG0(fn) \
		(PCIE_CORE_CTRL_MGMT_BASE + 0x0240 + (fn) * 0x0008)
#define ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG1(fn) \
		(PCIE_CORE_CTRL_MGMT_BASE + 0x0244 + (fn) * 0x0008)
#define ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) \
		(GENMASK(4, 0) << ((b) * 8))
#define ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE(b, a) \
		(((a) << ((b) * 8)) & \
		 ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b))
#define ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b) \
		(GENMASK(7, 5) << ((b) * 8))
#define ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL(b, c) \
		(((c) << ((b) * 8 + 5)) & \
		 ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b))

#define ROCKCHIP_NUM_PM_RSTS   ARRAY_SIZE(rockchip_pci_pm_rsts)
#define ROCKCHIP_NUM_CORE_RSTS ARRAY_SIZE(rockchip_pci_core_rsts)

/* [한국어] 전원 관리 계열 리셋 라인의 이름 배열. reset_control_bulk_get 계열 API 가
 * 이름 배열을 받아 한 번에 여러 리셋을 잡아 주므로, DT 의 reset-names 에
 * 적힌 문자열을 그대로 나열해 둔다. 배열 크기가 곧 ROCKCHIP_NUM_PM_RSTS 다. */
static const char * const rockchip_pci_pm_rsts[] = {
	/* [한국어] "pm" — 전원 도메인 리셋. */
	"pm",
	"pclk",
	"aclk",
};

/* NOTE: Do not reorder the deassert sequence of the following reset pins */
static const char * const rockchip_pci_core_rsts[] = {
	/* [한국어] "pipe" — PIPE 인터페이스(PHY 와 컨트롤러 사이의 표준 인터페이스) 리셋.
	 * 바로 위 영어 주석이 경고하듯 이 배열의 순서를 바꾸면 안 된다 —
	 * 리셋 해제 순서가 그대로 이 배열 순서를 따르고, 하드웨어가 요구하는
	 * 해제 순서가 정해져 있기 때문이다. */
	"pipe",
	"mgmt",
	"core",
	"mgmt-sticky",
};

/* [한국어] 호스트(RC)와 엔드포인트(EP)가 함께 쓰는 컨트롤러 상태 구조체.
 * 두 모드가 같은 IP 를 서로 다른 방향으로 쓰기 때문에 필드도 대부분 공유하고,
 * 갈라지는 지점은 is_rc 하나다. */
struct rockchip_pcie {
	/* [한국어] AXI 쪽 레지스터 창의 가상 주소(DT 의 axi-base). 옆의 상류 주석이 근거다.
	 * 설정자: pcie-rockchip.c 의 rockchip_pcie_parse_dt().
	 * 읽는 자: config 공간 접근과 아웃바운드 창 설정 경로.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 없음. probe 와 config 접근 경로에서만 쓰인다. */
	void	__iomem *reg_base;		/* DT axi-base */
	/* [한국어] APB 쪽 레지스터 창의 가상 주소(DT 의 apb-base). 옆의 상류 주석이 근거다.
	 * 설정자: rockchip_pcie_parse_dt().
	 * 읽는 자: 아래 rockchip_pcie_read()/write() 가 이 창만 사용한다 —
	 *   즉 이 헤더가 제공하는 접근자는 전부 APB 창 전용이고, AXI 창은
	 *   각 .c 파일이 직접 다룬다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 없음. */
	void	__iomem *apb_base;		/* DT apb-base */
	/* [한국어] 구형 PHY 바인딩을 쓰는 보드인지 여부.
	 * 설정자: pcie-rockchip.c:214 가 레인별 PHY 를 못 찾았을 때 true 로 둔다.
	 * 읽는 자: pcie-rockchip-host.c:260 등이 PHY 조작 방식을 가른다.
	 * 값 범위: true/false. 기본값 false(구조체가 0 초기화된다).
	 * 동기화: probe 초반에 정해진 뒤 읽기 전용. */
	bool    legacy_phy;
	/* [한국어] 레인별 PHY 포인터 배열. 신형 바인딩에서는 레인마다 PHY 가 따로 있다.
	 * 설정자: rockchip_pcie_get_phys().
	 * 읽는 자: init_port/deinit_phys 의 PHY 초기화·해제 순회.
	 * 값 범위: 원소 MAX_LANE_NUM(4)개. legacy_phy 인 보드에서는 0번만 유효하다.
	 * 동기화: PHY 프레임워크가 자체 락으로 보호한다. */
	struct  phy *phys[MAX_LANE_NUM];
	/* [한국어] 전원 관리 계열 리셋들을 한 번에 다루기 위한 bulk 서술자 배열.
	 * 설정자: rockchip_pcie_parse_dt() 가 위 rockchip_pci_pm_rsts 이름 배열로
	 *   reset_control_bulk_get 계열을 불러 채운다.
	 * 읽는 자: init_port 의 리셋 어서트·해제 순회.
	 * 값 범위: 원소 ROCKCHIP_NUM_PM_RSTS 개.
	 * 동기화: 리셋 프레임워크가 처리한다. */
	struct  reset_control_bulk_data pm_rsts[ROCKCHIP_NUM_PM_RSTS];
	/* [한국어] 코어 계열 리셋들의 bulk 서술자 배열. pm 쪽과 구조는 같지만 해제 순서가
	 * 하드웨어 요구사항이라는 점이 다르다(위 영어 주석 참조).
	 * 설정자: rockchip_pcie_parse_dt().
	 * 읽는 자: init_port.
	 * 값 범위: 원소 ROCKCHIP_NUM_CORE_RSTS 개.
	 * 동기화: 리셋 프레임워크가 처리한다. */
	struct  reset_control_bulk_data core_rsts[ROCKCHIP_NUM_CORE_RSTS];
	/* [한국어] 이 컨트롤러가 쓰는 모든 클럭의 bulk 배열.
	 * 설정자: pcie-rockchip.c:100 의 devm_clk_bulk_get_all() — 이름을 하나하나
	 *   지정하지 않고 DT 에 적힌 클럭 전부를 통째로 가져온다.
	 * 읽는 자: enable_clocks/disable_clocks 가 clk_bulk_* 에 그대로 넘긴다.
	 * 값 범위: 유효 포인터. 개수는 아래 num_clks 에 담긴다.
	 * 동기화: 클럭 프레임워크가 처리한다. */
	struct  clk_bulk_data *clks;
	/* [한국어] 위 clks 배열의 원소 수.
	 * 설정자: devm_clk_bulk_get_all() 의 반환값(:100). 음수면 오류다(:101).
	 * 읽는 자: clk_bulk_prepare_enable(:265) / clk_bulk_disable_unprepare(:276).
	 * 값 범위: 0 이상. 오류일 때만 잠시 음수를 담았다가 곧바로 반환된다.
	 * 동기화: 설정 후 읽기 전용. */
	int	num_clks;
	/* [한국어] 12V 전원 레귤레이터(옆의 상류 주석). 슬롯에 보조 전원을 공급하는 보드용이다.
	 * 설정자: rockchip_pcie_parse_dt() 의 optional 조회.
	 * 읽는 자: 전원 인가·차단 경로.
	 * 값 범위: 유효 포인터 또는 NULL(그 전원이 없는 보드).
	 * 동기화: 레귤레이터 프레임워크가 처리한다. */
	struct	regulator *vpcie12v; /* 12V power supply */
	/* [한국어] 3.3V 전원 레귤레이터(옆의 상류 주석). PCIe 슬롯의 주 전원이다.
	 * 설정자·읽는 자·동기화는 vpcie12v 와 같다. */
	struct	regulator *vpcie3v3; /* 3.3V power supply */
	/* [한국어] 1.8V 전원 레귤레이터(옆의 상류 주석). PHY 아날로그부에 쓰인다.
	 * 설정자·읽는 자·동기화는 vpcie12v 와 같다. */
	struct	regulator *vpcie1v8; /* 1.8V power supply */
	/* [한국어] 0.9V 전원 레귤레이터(옆의 상류 주석). PHY 코어 전압이다.
	 * 설정자·읽는 자·동기화는 vpcie12v 와 같다. */
	struct	regulator *vpcie0v9; /* 0.9V power supply */
	/* [한국어] PERST# 신호를 내보낼 GPIO.
	 * 설정자: pcie-rockchip.c:91~94 — 모드에 따라 DT 이름이 다르다.
	 *   EP 모드면 "ep", RC 모드면 "reset" 이름으로 optional 조회한다.
	 *   같은 신호가 방향에 따라 다른 이름을 갖는 셈이다.
	 * 읽는 자: 링크 시작 전 PERST# 펄스를 만드는 경로.
	 * 값 범위: 유효 포인터 또는 NULL(GPIO 가 없는 보드).
	 * 동기화: GPIO 프레임워크가 처리한다. */
	struct	gpio_desc *perst_gpio;
	/* [한국어] 이 링크가 쓰는 레인 수.
	 * 설정자: rockchip_pcie_parse_dt() 가 DT 의 num-lanes 에서 읽는다.
	 * 읽는 자: 링크 훈련 설정과 레인별 PHY 순회.
	 * 값 범위: 1 이상 MAX_LANE_NUM(4) 이하.
	 * 동기화: 설정 후 읽기 전용. */
	u32	lanes;
	/* [한국어] 실제로 링크가 올라온 레인들의 비트맵.
	 * 설정자: pcie-rockchip-host.c:539 가 rockchip_pcie_lane_map() 결과로 채운다.
	 * 읽는 자: 같은 파일 :541 이 BIT(i) 로 레인별 유효성을 확인한다.
	 * 값 범위: 하위 MAX_LANE_NUM 비트. 0 이면 어떤 레인도 훈련되지 않은 것이다.
	 * 동기화: 링크 훈련 후 읽기 전용. */
	u8      lanes_map;
	/* [한국어] 목표 링크 세대(1 = Gen1, 2 = Gen2).
	 * 설정자: rockchip_pcie_parse_dt() 가 DT 의 max-link-speed 에서 읽는다.
	 * 읽는 자: 링크 훈련 시 속도 상한 설정.
	 * 값 범위: 1 또는 2.
	 * 동기화: 설정 후 읽기 전용. */
	int	link_gen;
	/* [한국어] 이 컨트롤러의 struct device. 로그와 devm 할당의 기준이다.
	 * 설정자: host/ep 각각의 probe.
	 * 읽는 자: 공용 코드(pcie-rockchip.c) 전체.
	 * 값 범위: 항상 유효.
	 * 동기화: 읽기 전용. */
	struct	device *dev;
	/* [한국어] 레거시 INTx 를 리눅스 IRQ 로 사상하는 도메인. RC 모드 전용이다.
	 * 설정자: pcie-rockchip-host.c:873 의 irq_domain_create_linear(PCI_NUM_INTX).
	 * 읽는 자: :706 이 generic_handle_domain_irq() 로 인터럽트를 분배하고,
	 *   :1175 가 제거한다.
	 * 값 범위: 유효 포인터 또는 NULL(EP 모드에서는 만들지 않는다).
	 * 동기화: IRQ 코어가 관리한다. */
	struct	irq_domain *irq_domain;
	/* [한국어] [상류 코드 관찰, 수정하지 않음] 이 필드를 읽거나 쓰는 코드가 이 트리에
	 * 하나도 없다(pcie-rockchip.c / -host.c / -ep.c 전수 확인). 죽은 필드다. */
	int     offset;
	/* [한국어] PME 메시지를 보낼 때 쓰는 특수 창의 가상 주소. RC 모드 전용이다.
	 * 설정자: pcie-rockchip-host.c:1153 의 devm_ioremap(msg_bus_addr, SZ_1M).
	 * 읽는 자: :1030 이 PCIE_RC_SEND_PME_OFF 오프셋에 0 을 써서 PME 를 내보낸다.
	 * 값 범위: 유효 포인터. 매핑 실패는 :1154 에서 걸러진다.
	 * 동기화: 없음. */
	void    __iomem *msg_region;
	/* [한국어] 위 msg_region 이 매핑할 버스 주소.
	 * 설정자: pcie-rockchip-host.c:972 가 아웃바운드 창의 PCI 주소로 초기화하고,
	 *   :1020 이 (reg_no + offset) << 20 만큼 더해 최종 위치를 정한다.
	 *   20비트 시프트는 창 하나가 1MB 라는 뜻이며, 그래서 위 ioremap 크기도 SZ_1M 이다.
	 * 읽는 자: :1153 의 매핑.
	 * 값 범위: 유효한 버스 주소.
	 * 동기화: probe 경로에서만 갱신된다. */
	phys_addr_t msg_bus_addr;
	/* [한국어] 이 인스턴스가 루트 컴플렉스인지 엔드포인트인지 — 두 모드가 갈라지는 유일한 필드다.
	 * 설정자: pcie-rockchip-host.c:1125 가 true 로 두고, EP probe 는 두지 않아
	 *   0 초기화된 false 가 그대로 남는다.
	 * 읽는 자: 공용 코드의 모든 분기 — pcie-rockchip.c:35(자원 이름 선택),
	 *   :90(PERST# GPIO 이름 선택), :160 등.
	 * 값 범위: true = RC, false = EP.
	 * 동기화: probe 초반에 정해진 뒤 읽기 전용. */
	bool is_rc;
	/* [한국어] config 공간용 메모리 자원. RC 모드에서만 채운다.
	 * 설정자: pcie-rockchip.c:43 이 is_rc 일 때만 조회하고, :46 에서 부재를 검사한다.
	 * 읽는 자: config 접근 창 계산.
	 * 값 범위: 유효 포인터(RC) 또는 NULL(EP).
	 * 동기화: 설정 후 읽기 전용. */
	struct resource *mem_res;
};

/* [한국어]
 * rockchip_pcie_read - APB 레지스터 창에서 32비트를 읽는다
 *
 * @rockchip: 컨트롤러 객체. apb_base 가 창의 시작이다.
 * @reg: 창 안에서의 바이트 오프셋(이 헤더가 정의하는 PCIE_* 상수들).
 * @return: 읽은 32비트 값.
 *
 * 이 컨트롤러는 두 개의 레지스터 창을 갖는다. AXI 창(reg_base, DT 의 axi-base)은
 * PCI config 공간 접근에 쓰이고, APB 창(apb_base, DT 의 apb-base)은 컨트롤러
 * 자체의 제어·상태 레지스터에 쓰인다. 이 헤더가 제공하는 접근자 두 개는
 * 오직 APB 창만 다루며, AXI 창은 host/ep 각 파일이 직접 readl/writel 로 접근한다.
 * 그 구분을 알아야 이 헤더의 PCIE_* 오프셋 상수들이 어느 창의 것인지 헷갈리지 않는다.
 *
 * _relaxed 가 아닌 readl() 을 쓰는 것도 의도적이다. 링크 상태나 인터럽트 상태를
 * 폴링하는 곳에서 쓰이므로, 배리어가 없으면 컴파일러나 CPU 가 이전 값을
 * 재사용해 무한 루프가 될 수 있다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 핸들러 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie-rockchip.c / -host.c / -ep.c 의 모든 제어 레지스터 접근
 *     → [rockchip_pcie_read] → readl()
 */
static u32 rockchip_pcie_read(struct rockchip_pcie *rockchip, u32 reg)
{
	/* [한국어] APB 창에서 32비트를 읽는다. AXI 창(reg_base)이 아니라 APB 창(apb_base)만
	 * 쓴다는 점이 중요하다 — 이 헤더가 제공하는 접근자는 컨트롤러 제어
	 * 레지스터 전용이고, config 공간 접근은 각 .c 파일이 AXI 창으로 직접 한다.
	 * _relaxed 가 아닌 readl 이라 배리어를 포함하며, 링크 상태 폴링에서
	 * 낡은 값을 보지 않게 해 준다. */
	return readl(rockchip->apb_base + reg);
}

/* [한국어]
 * rockchip_pcie_write - APB 레지스터 창에 32비트를 쓴다
 *
 * @rockchip: 컨트롤러 객체.
 * @val: 쓸 값.
 * @reg: 창 안에서의 바이트 오프셋.
 *
 * 읽기 쪽과 대칭이며 같은 APB 창만 다룬다. 인자 순서가 (val, reg) 로
 * 표준 writel(val, addr) 의 배치를 따르지만, 짝인 rockchip_pcie_read(rockchip, reg)
 * 와 나란히 놓으면 두 번째 인자의 의미가 달라 헷갈리기 쉽다.
 *
 * 이 컨트롤러의 여러 제어 레지스터가 상위 16비트를 쓰기 마스크로 쓰는
 * "write-enable" 방식이라(이 헤더의 HIWORD_UPDATE 계열 매크로 참조),
 * 그런 레지스터에는 값과 마스크를 함께 조립해 넘겨야 한다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 핸들러 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie-rockchip.c / -host.c / -ep.c 의 모든 제어 레지스터 쓰기
 *     → [rockchip_pcie_write] → writel()
 */
static void rockchip_pcie_write(struct rockchip_pcie *rockchip, u32 val,
				u32 reg)
{
	/* [한국어] APB 창에 32비트를 쓴다. 인자 순서가 (val, reg) 로 readl 쪽의 (reg) 와
	 * 짝이 맞지 않아 보이지만, 이 파일 전체에서 일관되게 쓰인다. */
	writel(val, rockchip->apb_base + reg);
}

/* [한국어] DT 에서 레지스터 창·클럭·리셋·레귤레이터·GPIO 를 모두 읽어 구조체를 채운다.
 * 정의는 공용 파일 pcie-rockchip.c 에 있고 host/ep 양쪽이 부른다. */
int rockchip_pcie_parse_dt(struct rockchip_pcie *rockchip);
/* [한국어] 리셋 시퀀스와 PHY 초기화를 거쳐 컨트롤러를 동작 가능한 상태로 만든다.
 * 리셋 해제 순서가 rockchip_pci_core_rsts 배열 순서를 따른다. */
int rockchip_pcie_init_port(struct rockchip_pcie *rockchip);
/* [한국어] 레인별(또는 구형 바인딩의 단일) PHY 를 가져온다. */
int rockchip_pcie_get_phys(struct rockchip_pcie *rockchip);
/* [한국어] PHY 를 되돌린다. get_phys 의 짝이다. */
void rockchip_pcie_deinit_phys(struct rockchip_pcie *rockchip);
/* [한국어] clk_bulk_prepare_enable 로 모든 클럭을 한 번에 켠다. */
int rockchip_pcie_enable_clocks(struct rockchip_pcie *rockchip);
/* [한국어] 그 짝. clk_bulk_disable_unprepare 를 부른다. */
void rockchip_pcie_disable_clocks(struct rockchip_pcie *rockchip);
/* [한국어] config 접근 방식(type 0 / type 1)을 하드웨어에 알린다. 루트 버스 바로 아래
 * 장치는 type 0, 그보다 깊은 곳은 type 1 로 접근해야 하므로 접근 직전에
 * 매번 이 설정을 바꾼다. */
void rockchip_pcie_cfg_configuration_accesses(
		struct rockchip_pcie *rockchip, u32 type);

#endif /* _PCIE_ROCKCHIP_H */
