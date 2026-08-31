// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Intel Gateway SoCs
 *
 * Copyright (c) 2019 Intel Corporation.
 */

/*
 * [한국어 설명] 인텔 Gateway SoC 의 PCIe (pcie-intel-gw.c)
 *
 * === 파일의 역할 ===
 * 인텔 Gateway 는 가정용 라우터나 게이트웨이 장비에 들어가는 SoC 다
 * (원래 랜틱/인피니언 계열). x86 이 아니라 MIPS 나 ARM 기반이며,
 * DesignWare PCIe IP 를 써서 Wi-Fi 카드나 스토리지를 붙인다.
 *
 * 이 드라이버가 하는 일은 다른 DesignWare 파생 드라이버와 같다 — 주변부
 * 준비. 다만 이 SoC 에 고유한 부분이 몇 가지 있다.
 *
 * 하나는 애플리케이션 레지스터(app 레지스터)다. DesignWare IP 의 표준
 * 레지스터와 별개로, 이 SoC 가 IP 를 감싸며 추가한 제어 레지스터 묶음이
 * 있다. 링크 트레이닝을 시작하거나 인터럽트를 다루는 일부가 그쪽에 있어서
 * 표준 경로만으로는 안 된다.
 *
 * 다른 하나는 링크 속도와 폭을 디바이스 트리에서 제한하는 기능이다.
 * 보드 배선 품질이 좋지 않으면 Gen3 로 올리려다 실패해 링크가 계속
 * 재트레이닝될 수 있어서, 아예 낮은 속도로 고정하는 편이 안정적이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 intel,lgm-pcie 가 있으면
 *   -> [이 파일] intel_pcie_probe()
 *      -> 클럭/리셋/PHY 준비, app 레지스터 설정
 *      -> dw_pcie_host_init() [pcie-designware-host.c]
 *         -> PCI 코어 열거
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: pcie-designware.c / -host.c, PHY·클럭·리셋·GPIO 프레임워크.
 * 공유 상태: struct intel_pcie 가 struct dw_pcie 를 품는다.
 *
 * === 주요 함수/구조체 요약 ===
 * intel_pcie_probe()      : 전체 초기화.
 * intel_pcie_host_setup()  : 클럭·리셋·PHY 를 켜고 링크를 올린다.
 * intel_pcie_link_setup()  : DBI(설정공간)의 LNKCTL 에서 Link Disable 과
 *                           ASPM Control 을 지운다. app 레지스터가 아니고,
 *                           속도·폭 제한과도 무관하다 -- 부트로더가 남겼을
 *                           수 있는 링크 비활성을 풀고, 학습 중 저전력
 *                           진입을 막는 것이 목적이다.
 * intel_pcie_rc_init()    : DWC 코어가 되부르는 훅. 하는 일은
 *                           intel_pcie_host_setup() 위임뿐이다.
 * intel_pcie_ltssm_enable() / intel_pcie_ltssm_disable() : LTSSM(Link
 *                           Training and Status State Machine)을 켜고 끈다.
 *                           app 레지스터를 통해야 하는 대표적인 조작이다.
 * intel_pcie_init_n_fts() : N_FTS(Fast Training Sequence 개수)를 설정한다.
 *                           L0s 에서 깨어날 때 링크를 다시 맞추는 데 쓰는
 *                           시퀀스 수로, 이 값이 부족하면 복귀가 실패한다.
 * intel_pcie_device_rst_assert() / _deassert() : 장치 쪽 PERST# 제어.
 * intel_pcie_core_rst_assert() / _deassert() : 컨트롤러 쪽 리셋.
 * intel_pcie_wait_l2()    : 절전 진입 시 링크가 L2 에 들어가기를 기다린다.
 * intel_pcie_turn_off()   : 링크를 L2 로 내리고 PERST# 를 걸고 PCI_COMMAND 의
 *                           MEMORY 비트를 끈다. **클록은 끄지 않는다** --
 *                           그것은 __intel_pcie_remove() 의 몫이다.
 * intel_pcie_suspend_noirq() / intel_pcie_resume_noirq() : 절전 전후.
 * intel_pcie_get_resources() : 디바이스 트리에서 클럭·리셋·GPIO 를 얻는다.
 * struct intel_pcie       : dw_pcie 와 이 SoC 의 app 레지스터·GPIO 핸들.
 *
 * === NVMe 관점 (필수 4섹션에 대한 부가 절) ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 호출하지 않는다(이 트리에서 전수 확인).
 * 이 드라이버가 세우는 것은 버스이고, NVMe 는 그 위에서 열거되는 장치 중
 * 하나일 뿐이다. 코드상의 접점은 없다.
 *
 * 다만 이 파일이 정하는 두 값이 버스에 붙은 모든 장치의 성능 상한이 된다:
 * DT 의 max-link-speed 로 정해지는 pci->max_link_speed 와, DWC 코어가
 * 읽어 쓰는 레인 수다. 드라이브가 더 높은 속도를 낼 수 있어도 보드가
 * 낮게 고정해 두었다면 그것이 상한이다.
 *
 * 참고로 max_link_speed 는 성능뿐 아니라 두 곳의 **동작 분기**에도 쓰인다:
 *   - intel_pcie_init_n_fts() 가 Gen3/Gen4 에서 FTS 개수를 늘린다.
 *   - intel_pcie_wait_l2() 가 Gen3 미만이면 L2 진입 절차를 건너뛴다.
 */

/* [한국어] [한국어] BIT() 매크로를 쓰기 위해. 아래 APP 레지스터 비트 정의가 전부 이것으로
 * 표현된다. (FIELD_GET/FIELD_PREP 은 이 파일에서 쓰지 않는다.) */
#include <linux/bitfield.h>
/* [한국어] [한국어] clk_prepare_enable / clk_disable_unprepare. 코어 클록 하나를 켜고 끈다. */
#include <linux/clk.h>
/* [한국어] [한국어] gpiod_* API. PERST# 를 GPIO 로 제어한다. */
#include <linux/gpio/consumer.h>
/* [한국어] [한국어] readl_poll_timeout. L2 진입을 20us 간격으로 5초까지 기다리는 데 쓴다. */
#include <linux/iopoll.h>
/* [한국어] [한국어] struct of_device_id. 아래 of_intel_pcie_match 표 정의에 필요하다. */
#include <linux/mod_devicetable.h>
/* [한국어] [한국어] PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_LD/ASPMC, PCI_COMMAND_MEMORY 같은
 * 규약 상수를 쓰기 위해. */
#include <linux/pci_regs.h>
/* [한국어] [한국어] phy_init / phy_power / phy_exit. 물리 계층을 켜고 끈다. */
#include <linux/phy/phy.h>
/* [한국어] [한국어] platform_driver 등록과 devm_platform_ioremap_resource_byname 을 위해. */
#include <linux/platform_device.h>
/* [한국어] [한국어] device_property_read_u32. DT 의 'reset-assert-ms' 를 읽는다. */
#include <linux/property.h>
/* [한국어] [한국어] reset_control_assert / _deassert. 컨트롤러 코어 리셋을 제어한다. */
#include <linux/reset.h>

/* [한국어] [한국어] PCI 코어 내부 헤더. SZ_128 등 크기 상수와 공통 정의를 가져온다. */
#include "../../pci.h"
/* [한국어] [한국어] DWC 코어의 공개 인터페이스. struct dw_pcie, dw_pcie_host_init,
 * dw_pcie_setup_rc, DBI 접근자가 전부 여기 있다. */
#include "pcie-designware.h"

/* [한국어] [한국어] Gen1/Gen2 용 FTS(Fast Training Sequence) 기본 개수 = 127.
 * SZ_128 - 1 로 쓴 것은 이 필드가 7비트라 최대값이 127 이기 때문이다.
 * FTS 는 L0s 에서 깨어날 때 보내는 학습 시퀀스로, 모자라면 복귀에 실패한다. */
#define PORT_AFR_N_FTS_GEN12_DFT	(SZ_128 - 1)
/* [한국어] [한국어] Gen3(8GT/s)용 180개. 속도가 높을수록 수신단이 비트 동기를 되찾기
 * 어려워 더 많은 시퀀스가 필요하다. */
#define PORT_AFR_N_FTS_GEN3		180
/* [한국어] [한국어] Gen4(16GT/s)용 196개. */
#define PORT_AFR_N_FTS_GEN4		196
/* PCIe Application logic Registers */
/* [한국어] [한국어] APP 창의 Core Control Register 오프셋. Intel 이 DWC IP 바깥에
 * 덧붙인 제어면이라 DBI 와는 전혀 다른 주소 공간이다. */
#define PCIE_APP_CCR			0x10
/* [한국어] [한국어] LTSSM 활성 비트. 이것을 세워야 링크 학습이 시작된다. */
#define PCIE_APP_CCR_LTSSM_ENABLE	BIT(0)
/* [한국어] [한국어] 전원 관리 메시지 제어 레지스터. */
#define PCIE_APP_MSG_CR			0x30
/* [한국어] [한국어] PM_Turn_Off 메시지를 보내라는 요청 비트. 하드웨어가 메시지 전송과
 * 상대의 응답 처리까지 알아서 하고, 결과만 아래 IN_L2 비트로 알려 준다. */
#define PCIE_APP_MSG_XMT_PM_TURNOFF	BIT(0)

/* [한국어] [한국어] 전원 관리 상태 레지스터. */
#define PCIE_APP_PMC			0x44
/* [한국어] [한국어] 링크가 L2 에 들어갔다는 표시. intel_pcie_wait_l2 가 이 비트를 폴링한다. */
#define PCIE_APP_PMC_IN_L2		BIT(20)

/* [한국어] [한국어] 인터럽트 활성(enable) 레지스터. 0 을 쓰면 전부 막힌다. */
#define PCIE_APP_IRNEN			0xF4
/* [한국어] [한국어] 인터럽트 상태(capture) 레지스터. 1 을 써서 지운다. */
#define PCIE_APP_IRNCR			0xF8
/* [한국어] [한국어] AER 오류 보고 인터럽트. */
#define PCIE_APP_IRN_AER_REPORT		BIT(0)
/* [한국어] [한국어] PME(Power Management Event) 인터럽트. */
#define PCIE_APP_IRN_PME		BIT(2)
/* [한국어] [한국어] 벤더 정의 메시지 수신 인터럽트. */
#define PCIE_APP_IRN_RX_VDM_MSG		BIT(4)
/* [한국어] [한국어] PM_Turn_Off 에 대한 응답(PM_TO_Ack) 수신 인터럽트. */
#define PCIE_APP_IRN_PM_TO_ACK		BIT(9)
/* [한국어] [한국어] 링크 자동 대역폭 변경 상태 인터럽트. */
#define PCIE_APP_IRN_LINK_AUTO_BW_STAT	BIT(11)
/* [한국어] [한국어] 대역폭 관리 인터럽트. */
#define PCIE_APP_IRN_BW_MGT		BIT(12)
/* [한국어] [한국어] INTA 레거시 인터럽트. 이 네 개(A~D)가 비트 13~16 에 연속으로 놓인다. */
#define PCIE_APP_IRN_INTA		BIT(13)
/* [한국어] [한국어] INTB. */
#define PCIE_APP_IRN_INTB		BIT(14)
/* [한국어] [한국어] INTC. */
#define PCIE_APP_IRN_INTC		BIT(15)
/* [한국어] [한국어] INTD. */
#define PCIE_APP_IRN_INTD		BIT(16)
/* [한국어] [한국어] LTR(Latency Tolerance Reporting) 메시지 수신 인터럽트. */
#define PCIE_APP_IRN_MSG_LTR		BIT(18)
/* [한국어] [한국어] 루트 컴플렉스 시스템 오류 인터럽트. */
#define PCIE_APP_IRN_SYS_ERR_RC		BIT(29)
/* [한국어] [한국어] INTx 비트들이 시작하는 자리(13). 위 INTA~INTD 정의와 같은 값을
 * 따로 둔 것인데, **이 파일 안에서 참조하는 곳이 없다.** */
#define PCIE_APP_INTX_OFST		12

/* [한국어] [한국어] 이 드라이버가 다루는 인터럽트 비트를 모두 모은 마스크.
 * IRNEN 을 열 때와 IRNCR 을 지울 때 이 하나로 처리해, 쓰지 않는 비트는
 * 건드리지 않는다. */
#define PCIE_APP_IRN_INT \
	(PCIE_APP_IRN_AER_REPORT | PCIE_APP_IRN_PME | \
	PCIE_APP_IRN_RX_VDM_MSG | PCIE_APP_IRN_SYS_ERR_RC | \
	PCIE_APP_IRN_PM_TO_ACK | PCIE_APP_IRN_MSG_LTR | \
	PCIE_APP_IRN_BW_MGT | PCIE_APP_IRN_LINK_AUTO_BW_STAT | \
	PCIE_APP_IRN_INTA | PCIE_APP_IRN_INTB | \
	PCIE_APP_IRN_INTC | PCIE_APP_IRN_INTD)

/* [한국어] [한국어] PERST# 유지 시간의 기본값(100ms). DT 에 'reset-assert-ms' 가 없으면
 * 이 값이 쓰인다. 짧으면 장치가 리셋을 마치지 못해 열거되지 않고, 길면
 * 부팅이 느려지므로 보드마다 조정하는 값이다. */
#define RESET_INTERVAL_MS		100

struct intel_pcie {
	/* [한국어] [한국어] DWC 코어의 인스턴스. **포인터가 아니라 값으로** 품고 있다.
	 * 설정자: probe 가 dev / use_parent_dt_ranges / ops 를, host_setup 이 atu_base 를 채운다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 모든 함수(&pcie->pci 로 접근).
	 * 값 범위: 항상 유효한 임베디드 구조체 -- NULL 이 될 수 없다.
	 * 동기화: 첫 필드라 두 주소가 같지만, 이 파일은 역방향 변환에 container_of
	 * 대신 dev_get_drvdata 를 쓴다(intel_pcie_rc_init 참조). */
	struct dw_pcie		pci;
	/* [한국어] [한국어] Intel 이 DWC IP 바깥에 덧붙인 APP 제어 창의 가상 주소.
	 * 설정자: intel_pcie_get_resources 가 DT 의 'app' 자원을 ioremap.
	 * 읽는 자: pcie_app_wr / pcie_app_wr_mask 만이 직접 쓴다.
	 * 값 범위: 유효한 __iomem 포인터. 실패 시 프로브가 중단된다.
	 * 동기화: 없음. 갱신이 pcie_update_bits 의 읽기-수정-쓰기라 잠금이 없지만,
	 * 호출자가 모두 직렬화된 경로(프로브/서스펜드/재개)라 실제로 겹치지 않는다. */
	void __iomem		*app_base;
	/* [한국어] [한국어] PERST# 신호를 내보내는 GPIO 서술자.
	 * 설정자: intel_pcie_ep_rst_init 이 devm_gpiod_get 으로 얻는다.
	 * 읽는 자: intel_pcie_device_rst_assert / _deassert.
	 * 값 범위: **NULL 이 될 수 없다** -- optional 판이 아니라서 GPIO 가 없으면
	 * 프로브가 실패한다. 이 SoC 는 PERST# 를 반드시 소프트웨어가 제어한다.
	 * 동기화: 링크 기동/정지 경로에서만 다뤄져 경쟁이 없다. */
	struct gpio_desc	*reset_gpio;
	/* [한국어] [한국어] PERST# 를 assert 한 채로 유지할 시간(밀리초).
	 * 설정자: intel_pcie_get_resources 가 DT 의 'reset-assert-ms' 를 읽거나,
	 * 없으면 RESET_INTERVAL_MS(100)를 넣는다.
	 * 읽는 자: intel_pcie_device_rst_deassert 의 msleep.
	 * 값 범위: DT 가 정하는 임의의 u32. 짧으면 장치가 열거되지 않고 길면
	 * 부팅이 느려지므로 보드마다 조정한다.
	 * 동기화: 프로브에서 한 번 쓰고 이후 읽기만 한다. */
	u32			rst_intrvl;
	/* [한국어] [한국어] 컨트롤러 코어 클록.
	 * 설정자: intel_pcie_get_resources 가 이름 없이(NULL) 하나 얻는다 --
	 * 이 SoC 는 PCIe 클록이 하나뿐이다.
	 * 읽는 자: host_setup 의 clk_prepare_enable, 되감기/서스펜드의
	 * clk_disable_unprepare.
	 * 값 범위: 유효한 clk 핸들. devm 이라 해제는 자동이다.
	 * 동기화: 프로브·서스펜드·재개 경로에서만 다뤄진다. */
	struct clk		*core_clk;
	/* [한국어] [한국어] 컨트롤러 코어 리셋 라인.
	 * 설정자: intel_pcie_get_resources 가 이름 없이 하나 얻는다.
	 * 읽는 자: intel_pcie_core_rst_assert / _deassert.
	 * 값 범위: 유효한 핸들.
	 * 동기화: 위와 같다. 실패 경로에서 assert 로 되돌리는 것이 규칙이다 --
	 * 실패한 컨트롤러는 리셋 상태로 두는 편이 안전하다. */
	struct reset_control	*core_rst;
	struct phy		*phy;
};
/* [한국어]
 * pcie_update_bits - MMIO 레지스터의 일부 비트만 읽고-고쳐-쓴다
 *
 * @base: 레지스터 창의 시작 가상 주소(APB 또는 DBI).
 * @ofs: 그 창 안의 바이트 오프셋.
 * @mask: 바꿀 비트만 1 로 세운 마스크.
 * @val: 넣을 값. mask 밖의 비트는 무시된다.
 * @return: 없음.
 *
 * 이 파일의 거의 모든 레지스터 갱신이 이 함수를 거친다. Rockchip 처럼
 * '상위 16비트가 쓰기 마스크' 인 하드웨어와 달리 이 IP 는 평범한 레지스터라,
 * 일부 비트만 바꾸려면 읽기-수정-쓰기가 불가피하다.
 *
 * 마지막 `if (val != old)` 가 요점이다. 값이 바뀌지 않으면 아예 쓰지 않는다.
 * MMIO 쓰기는 버스를 오가는 비용이 크고, 레지스터에 따라 쓰기 자체가
 * 부작용(예: 상태 소거)을 일으킬 수 있어 불필요한 쓰기를 피하는 편이 안전하다.
 *
 * **잠금이 없다.** 서로 다른 문맥이 같은 레지스터의 다른 비트를 동시에
 * 고치면 한쪽 변경이 사라질 수 있다. 이 파일에서는 호출자가 모두 프로브·
 * 서스펜드·재개 같은 직렬화된 경로라 실제로 겹치지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pcie_app_wr_mask / pcie_rc_cfg_wr_mask → [이 함수] → readl/writel
 */
static void pcie_update_bits(void __iomem *base, u32 ofs, u32 mask, u32 val)
{
	u32 old;

	old = readl(base + ofs);
	/* [한국어] [한국어] 마스크 안의 비트만 새 값으로 갈아 끼운다. val 에도 마스크를 씌우는
	 * 것은 호출자가 마스크 밖의 비트를 실수로 담아도 무시하기 위해서다. */
	val = (old & ~mask) | (val & mask);

	if (val != old)
		/* [한국어] [한국어] **값이 바뀔 때만 쓴다.** MMIO 쓰기는 버스를 오가는 비용이 크고,
		 * 레지스터에 따라 쓰기 자체가 부작용(상태 소거 등)을 일으킬 수 있다. */
		writel(val, base + ofs);
}

/* [한국어]
 * pcie_app_wr - SoC 전용 APP 레지스터에 32비트를 통째로 쓴다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @ofs: APP 창 안의 오프셋(PCIE_APP_* 상수).
 * @val: 쓸 값.
 * @return: 없음.
 *
 * APP 창은 Intel 이 DesignWare IP 바깥에 덧붙인 제어 회로다 -- LTSSM 스위치,
 * PM 메시지 전송, 인터럽트 마스크/상태가 여기 있다. DWC 코어가 다루는 DBI
 * 창과는 완전히 별개의 주소 공간이다.
 *
 * 마스크 없이 통째로 쓰는 판이라, 다른 비트를 보존할 필요가 없는 자리에만
 * 쓴다 -- 이 파일에서는 인터럽트 마스크를 전부 끄거나(IRNEN = 0) 상태를
 * 한꺼번에 지우는(IRNCR = PCIE_APP_IRN_INT) 두 곳뿐이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_core_irq_disable → [이 함수] → writel
 */
static inline void pcie_app_wr(struct intel_pcie *pcie, u32 ofs, u32 val)
{
	writel(val, pcie->app_base + ofs);
}

/* [한국어]
 * pcie_app_wr_mask - APP 레지스터의 일부 비트만 바꾼다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @ofs: APP 창 안의 오프셋.
 * @mask: 바꿀 비트.
 * @val: 넣을 값.
 * @return: 없음.
 *
 * pcie_app_wr 과 달리 다른 비트를 보존한다. LTSSM 활성 비트 하나만 켜고 끌
 * 때(PCIE_APP_CCR), PM_TURNOFF 전송을 요청할 때(PCIE_APP_MSG_CR), 그리고
 * 인터럽트 마스크를 다시 열 때 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_ltssm_enable/_disable, intel_pcie_wait_l2,
 *   intel_pcie_host_setup → [이 함수] → pcie_update_bits
 */
static void pcie_app_wr_mask(struct intel_pcie *pcie, u32 ofs,
			     u32 mask, u32 val)
{
	pcie_update_bits(pcie->app_base, ofs, mask, val);
}

/* [한국어]
 * pcie_rc_cfg_rd - 루트 컴플렉스 자신의 설정공간을 DBI 로 읽는다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @ofs: 설정공간 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 이름이 'cfg' 인 이유: DWC 는 루트 포트 자신의 PCI 설정공간을 DBI 창에
 * 그대로 노출하므로, DBI 를 읽는 것이 곧 자기 설정공간을 읽는 것이다.
 * 링크 건너편 장치의 설정공간과는 다른 이야기다.
 *
 * dw_pcie_readl_dbi 를 감싸기만 하는 이유는 이 파일 안에서 APP 접근과
 * 이름 모양을 맞춰 어느 창을 건드리는지 한눈에 보이게 하기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_link_setup → [이 함수] → dw_pcie_readl_dbi
 */
static inline u32 pcie_rc_cfg_rd(struct intel_pcie *pcie, u32 ofs)
{
	return dw_pcie_readl_dbi(&pcie->pci, ofs);
}

/* [한국어]
 * pcie_rc_cfg_wr - 루트 컴플렉스 자신의 설정공간에 DBI 로 쓴다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @ofs: 설정공간 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * pcie_rc_cfg_rd 의 짝. 읽기 전용 레지스터를 건드릴 때는 호출자가
 * dw_pcie_dbi_ro_wr_en 으로 감싸야 하지만, 이 파일이 쓰는 LNKCTL 은
 * 쓰기 가능한 레지스터라 그럴 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_link_setup → [이 함수] → dw_pcie_writel_dbi
 */
static inline void pcie_rc_cfg_wr(struct intel_pcie *pcie, u32 ofs, u32 val)
{
	dw_pcie_writel_dbi(&pcie->pci, ofs, val);
}

/* [한국어]
 * pcie_rc_cfg_wr_mask - 설정공간의 일부 비트만 바꾼다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @ofs: 설정공간 오프셋.
 * @mask: 바꿀 비트.
 * @val: 넣을 값.
 * @return: 없음.
 *
 * 위 두 함수와 달리 dw_pcie_ 접근자를 거치지 않고 pci.dbi_base 를 직접
 * pcie_update_bits 에 넘긴다. 그래서 SoC 가 등록한 자체 DBI 구현
 * (pci->ops->read_dbi/write_dbi)을 우회한다 -- 이 드라이버는 그것을
 * 등록하지 않으므로 결과는 같지만, 접근 경로가 다르다는 점은 기록해 둔다.
 *
 * 이 파일에서 쓰이는 곳은 한 군데 -- intel_pcie_turn_off 가 PCI_COMMAND 의
 * MEMORY 비트를 끄는 자리다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_turn_off → [이 함수] → pcie_update_bits
 */
static void pcie_rc_cfg_wr_mask(struct intel_pcie *pcie, u32 ofs,
				u32 mask, u32 val)
{
	pcie_update_bits(pcie->pci.dbi_base, ofs, mask, val);
}

/* [한국어]
 * intel_pcie_ltssm_enable - 링크 학습을 시작시킨다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * APP 창의 CCR 레지스터에서 LTSSM 활성 비트 하나만 켠다. 이 비트를 세우기
 * 전까지 하드웨어는 링크 학습을 시작하지 않으므로, 설정을 모두 마친 뒤
 * 마지막에 여는 스위치다.
 *
 * host_setup 에서 device_rst_deassert 바로 뒤에 오는 순서가 중요하다 --
 * 상대 장치의 리셋을 풀어 준 직후에 학습을 시작해야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_host_setup → [이 함수] → pcie_app_wr_mask
 */
static void intel_pcie_ltssm_enable(struct intel_pcie *pcie)
{
	pcie_app_wr_mask(pcie, PCIE_APP_CCR, PCIE_APP_CCR_LTSSM_ENABLE,
			 PCIE_APP_CCR_LTSSM_ENABLE);
}

/* [한국어]
 * intel_pcie_ltssm_disable - 링크 학습을 멈춘다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * enable 의 정확한 반대. 같은 비트에 0 을 쓴다.
 *
 * host_setup 이 **설정을 시작하기 전에** 이것을 부르는 것이 요점이다.
 * 링크가 학습 중인 상태에서 링크 제어나 FTS 값을 바꾸면 협상이 어긋나므로,
 * 먼저 멈춰 두고 설정을 마친 뒤 다시 켠다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_host_setup → [이 함수] → pcie_app_wr_mask
 */
static void intel_pcie_ltssm_disable(struct intel_pcie *pcie)
{
	pcie_app_wr_mask(pcie, PCIE_APP_CCR, PCIE_APP_CCR_LTSSM_ENABLE, 0);
}

/* [한국어]
 * intel_pcie_link_setup - 링크 제어 레지스터에서 링크 비활성과 ASPM 을 끈다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * 두 비트를 지운다:
 *  - PCI_EXP_LNKCTL_LD (Link Disable): 이 비트가 서 있으면 링크가 아예 서지
 *    않는다. 부트로더가 남겼을 수 있으므로 명시적으로 지운다.
 *  - PCI_EXP_LNKCTL_ASPMC (ASPM Control): ASPM 을 꺼 둔다. 링크 학습 중에
 *    저전력 상태로 들어가면 협상이 불안정해지기 때문이다. 이후 ASPM 정책은
 *    PCI 코어의 aspm.c 가 열거 뒤에 다시 정한다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): dw_pcie_find_capability 의 반환값
 * offset 이 0 인 경우(능력 구조 없음)를 검사하지 않는다. PCIe 장치라면
 * 반드시 존재하므로 실제로 도달하는 경로는 아니다.
 *
 * 실행 컨텍스트: 프로세스 문맥. LTSSM 이 꺼진 상태에서 불려야 한다.
 *
 * 호출 체인:
 *   intel_pcie_host_setup → [이 함수] → pcie_rc_cfg_rd / pcie_rc_cfg_wr
 */
static void intel_pcie_link_setup(struct intel_pcie *pcie)
{
	u32 val;
	u8 offset = dw_pcie_find_capability(&pcie->pci, PCI_CAP_ID_EXP);
	/* [한국어] [한국어] 현재 링크 제어 값을 읽는다. 아래에서 두 비트를 지운 뒤 되쓴다. */
	val = pcie_rc_cfg_rd(pcie, offset + PCI_EXP_LNKCTL);

	val &= ~(PCI_EXP_LNKCTL_LD | PCI_EXP_LNKCTL_ASPMC);
	/* [한국어] [한국어] Link Disable 과 ASPM Control 을 함께 지운 값을 되쓴다.
	 * 부트로더가 Link Disable 을 남겼으면 링크가 아예 서지 않으므로 필수다. */
	pcie_rc_cfg_wr(pcie, offset + PCI_EXP_LNKCTL, val);
}

/* [한국어]
 * intel_pcie_init_n_fts - 링크 속도에 맞는 FTS 개수를 정한다
 *
 * @pci: DWC 코어 인스턴스. n_fts[] 를 채워 두면 dw_pcie_setup 이 레지스터에 쓴다.
 * @return: 없음.
 *
 * FTS(Fast Training Sequence)는 링크가 L0s 에서 깨어날 때 보내는 학습
 * 시퀀스다. 속도가 높을수록 수신단이 비트 동기를 되찾기 어려워 더 많은
 * 시퀀스가 필요하고, 모자라면 복귀에 실패해 링크가 내려간다.
 *
 * 그래서 Gen3 은 180, Gen4 는 196 으로 늘리고, Gen1/Gen2 는 기본값
 * (SZ_128 - 1 = 127)을 쓴다.
 *
 * n_fts[0] 과 [1] 의 구분에 유의: [0] 은 Gen1/Gen2 용, [1] 은 Gen3 이상용이다.
 * 그래서 switch 는 [1] 만 정하고, 마지막 줄이 [0] 을 무조건 기본값으로 둔다 --
 * 어떤 속도로 설정하든 저속 구간은 기본값을 쓴다는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. dw_pcie_setup_rc 보다 먼저 불려야 값이 반영된다.
 *
 * 호출 체인:
 *   intel_pcie_host_setup → [이 함수]
 */
static void intel_pcie_init_n_fts(struct dw_pcie *pci)
{
	switch (pci->max_link_speed) {
	case 3:
		/* [한국어] [한국어] Gen3(8GT/s)는 180개. 속도가 높을수록 수신단이 비트 동기를 되찾기
		 * 어려워 더 많은 시퀀스가 필요하다. */
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN3;
		break;
	case 4:
		/* [한국어] [한국어] Gen4(16GT/s)는 196개로 더 늘린다. */
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN4;
		break;
	default:
		pci->n_fts[1] = PORT_AFR_N_FTS_GEN12_DFT;
		break;
	}
	pci->n_fts[0] = PORT_AFR_N_FTS_GEN12_DFT;
}

/* [한국어]
 * intel_pcie_ep_rst_init - 엔드포인트 리셋 GPIO 를 얻어 assert 상태로 둔다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 성공, 음수는 GPIO 조회 실패값(-EPROBE_DEFER 포함).
 *
 * PERST# 선을 GPIO 로 제어한다. GPIOD_OUT_LOW 로 얻는 것이 요점 -- 서술자
 * 관례상 논리값 0 이 '비활성' 이지만, 이 드라이버는 아래
 * intel_pcie_device_rst_assert() 가 값 1 을 assert 로 쓰므로 여기서는
 * 리셋이 풀린 상태로 시작한다.
 *
 * 이름과 달리 optional 이 아니다 -- devm_gpiod_get 이라 GPIO 가 없으면
 * 프로브가 실패한다. 이 SoC 는 PERST# 를 반드시 소프트웨어가 제어한다는 뜻이다.
 *
 * -EPROBE_DEFER 일 때 로그를 남기지 않는 조건문이 붙어 있다. GPIO 공급자가
 * 아직 프로브되지 않은 정상 상황이라 부팅 로그를 재시도 메시지로 덮지 않기
 * 위해서다(dev_err_probe 가 도입되기 전의 관용구다).
 *
 * usleep_range(100, 200) 은 GPIO 설정이 실제 전기 레벨에 반영될 시간을 준다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_probe → [이 함수] → devm_gpiod_get → usleep_range
 */
static int intel_pcie_ep_rst_init(struct intel_pcie *pcie)
{
	struct device *dev = pcie->pci.dev;
	int ret;

	pcie->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	/* [한국어] [한국어] GPIO 조회 실패. optional 이 아니라 없으면 여기 걸린다. */
	if (IS_ERR(pcie->reset_gpio)) {
		/* [한국어] [한국어] 오류 포인터에서 코드를 꺼낸다. */
		ret = PTR_ERR(pcie->reset_gpio);
		/* [한국어] [한국어] -EPROBE_DEFER 는 GPIO 공급자가 아직 프로브되지 않은 정상 상황이므로
		 * 로그를 남기지 않는다. 부팅 로그가 재시도 메시지로 덮이는 것을 막는
		 * 관용구다(dev_err_probe 도입 전 방식). */
		if (ret != -EPROBE_DEFER)
			/* [한국어] [한국어] 진짜 오류만 로그로 남긴다. */
			dev_err(dev, "Failed to request PCIe GPIO: %d\n", ret);
		/* [한국어] [한국어] 실패값을 그대로 올려 프로브를 접는다. */
		return ret;
	}

	/* Make initial reset last for 100us */
	usleep_range(100, 200);

	return 0;
}

/* [한국어]
 * intel_pcie_core_rst_assert - 컨트롤러 코어를 리셋 상태로 묶는다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음. 반환값을 버리는 얇은 래퍼다.
 *
 * host_setup 이 가장 먼저 부르고, 실패 되감기와 remove 에서도 부른다.
 * 컨트롤러가 리셋 상태여야 PHY 초기화와 클록 전환이 안전하다.
 *
 * reset_control_assert 의 반환값을 검사하지 않는 것은 상류 그대로다 --
 * 되감기 경로에서도 쓰이므로 실패해도 할 수 있는 일이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_host_setup / __intel_pcie_remove → [이 함수]
 *     → reset_control_assert
 */
static void intel_pcie_core_rst_assert(struct intel_pcie *pcie)
{
	reset_control_assert(pcie->core_rst);
}

/* [한국어]
 * intel_pcie_core_rst_deassert - 리셋을 풀고 하드웨어가 안정될 때까지 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * 앞뒤의 지연이 이 함수의 실체다. 상류 주석이 근거를 적어 두었다:
 *  - udelay(1) -- 리셋을 풀기 **전에** 최소 홀드 타임을 채운다. 리셋 펄스가
 *    너무 짧으면 내부 상태가 완전히 지워지지 않는다.
 *  - usleep_range(1000, 2000) -- 리셋을 푼 **뒤** 하드웨어가 깨어날 시간을
 *    준다. 이 대기 없이 곧바로 레지스터를 건드리면 값이 반영되지 않는다.
 *
 * 두 지연의 종류가 다른 점에 유의: 앞은 1us 라 바쁜 대기(udelay)가 싸고,
 * 뒤는 1ms 라 잠들 수 있는 usleep_range 가 맞다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   intel_pcie_host_setup → [이 함수] → udelay → reset_control_deassert
 *     → usleep_range
 */
static void intel_pcie_core_rst_deassert(struct intel_pcie *pcie)
{
	/*
	 * One micro-second delay to make sure the reset pulse
	 * wide enough so that core reset is clean.
	 */
	udelay(1);
	reset_control_deassert(pcie->core_rst);

	/*
	 * Some SoC core reset also reset PHY, more delay needed
	 * to make sure the reset process is done.
	 */
	usleep_range(1000, 2000);
}

/* [한국어]
 * intel_pcie_device_rst_assert - 링크 건너편 장치에 PERST# 를 건다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * GPIO 값 1 이 이 드라이버에서 assert 다(DT 의 active-low 설정에 따라 실제
 * 전기 레벨은 뒤집힐 수 있다). cansleep 판을 쓰는 것은 GPIO 가 I2C 확장기
 * 같은 느린 버스 뒤에 있을 수 있기 때문이다.
 *
 * host_setup 초입과 turn_off 에서 불린다 -- 링크를 세우기 전과 내린 뒤
 * 양쪽에서 장치를 리셋 상태로 둔다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   intel_pcie_host_setup / intel_pcie_turn_off → [이 함수]
 *     → gpiod_set_value_cansleep
 */
static void intel_pcie_device_rst_assert(struct intel_pcie *pcie)
{
	gpiod_set_value_cansleep(pcie->reset_gpio, 1);
}

/* [한국어]
 * intel_pcie_device_rst_deassert - 리셋 유지 시간을 채운 뒤 PERST# 를 놓는다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * msleep 이 **먼저** 오는 순서가 핵심이다. 리셋을 풀기 전에 rst_intrvl
 * 밀리초 동안 assert 상태를 유지해야 상대 장치가 리셋을 완전히 마친다.
 * 그 값은 DT 의 "reset-assert-ms" 로 조정할 수 있고, 없으면
 * RESET_INTERVAL_MS(100)를 쓴다(intel_pcie_get_resources 참조).
 *
 * 보드마다 이 시간이 다른 이유: 전원 레일이 안정되는 속도와 장치의 리셋
 * 회로가 제각각이라, 짧으면 장치가 열거되지 않고 길면 부팅이 느려진다.
 *
 * 실행 컨텍스트: 프로세스 문맥. msleep 이 있어 반드시 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   intel_pcie_host_setup → [이 함수] → msleep → gpiod_set_value_cansleep
 */
static void intel_pcie_device_rst_deassert(struct intel_pcie *pcie)
{
	msleep(pcie->rst_intrvl);
	gpiod_set_value_cansleep(pcie->reset_gpio, 0);
}

/* [한국어]
 * intel_pcie_core_irq_disable - APP 인터럽트를 모두 막고 걸린 상태를 지운다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * 순서가 중요하다. 먼저 IRNEN(enable)에 0 을 써 새 인터럽트를 막고, 그
 * 다음 IRNCR(상태)에 PCIE_APP_IRN_INT 를 써 이미 걸린 것을 지운다.
 * 반대로 하면 지우는 사이에 새 인터럽트가 들어와 남는다.
 *
 * IRNCR 에 쓰는 값이 '전부 1' 이 아니라 PCIE_APP_IRN_INT 인 점에 유의 --
 * 이 드라이버가 사용하는 비트만 모은 마스크다. 쓰지 않는 비트를 건드리지
 * 않는다.
 *
 * 서스펜드와 remove 양쪽에서 불린다. 하드웨어를 재우기 전에 인터럽트가
 * 남아 있으면 사라진 컨트롤러를 향한 핸들러가 돌 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __intel_pcie_remove / intel_pcie_suspend_noirq → [이 함수] → pcie_app_wr
 */
static void intel_pcie_core_irq_disable(struct intel_pcie *pcie)
{
	pcie_app_wr(pcie, PCIE_APP_IRNEN, 0);
	pcie_app_wr(pcie, PCIE_APP_IRNCR, PCIE_APP_IRN_INT);
}

/* [한국어]
 * intel_pcie_get_resources - 클록·리셋·APP 창·PHY 와 리셋 유지 시간을 확보한다
 *
 * @pdev: 플랫폼 디바이스. 모든 자원의 출처다.
 * @return: 0 성공, 음수는 각 조회의 실패값.
 *
 * 다섯 가지를 챙긴다:
 *  1. 코어 클록 -- 이름 없이(NULL) 하나만 얻는다. 이 SoC 는 PCIe 클록이
 *     하나뿐이라 클록 목록을 다룰 필요가 없다.
 *  2. 코어 리셋 -- 마찬가지로 이름 없이 하나.
 *  3. "reset-assert-ms" 속성 -- PERST# 유지 시간. **실패해도 진행한다** --
 *     ret 을 검사해 기본값 RESET_INTERVAL_MS(100)를 넣을 뿐이다.
 *     그래서 이 속성은 선택이다.
 *  4. "app" 메모리 자원 -- SoC 전용 제어 레지스터 창. 없으면 실패한다.
 *  5. "pcie" PHY -- 물리 계층. 없으면 실패한다.
 *
 * 1, 2, 5 가 모두 같은 관용구를 쓴다: 실패값을 꺼내 -EPROBE_DEFER 가
 * 아닐 때만 로그를 남기고 반환한다. 4 만 로그 없이 바로 반환하는데,
 * devm_platform_ioremap_resource_byname 이 자체적으로 오류를 찍기 때문이다.
 *
 * platform_get_drvdata 로 인스턴스를 되찾는 점에 유의 -- probe 가
 * platform_set_drvdata 를 먼저 부른 뒤 이 함수를 호출한다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_probe → [이 함수] → devm_clk_get → devm_reset_control_get
 *     → device_property_read_u32 → devm_platform_ioremap_resource_byname
 *       → devm_phy_get
 */
static int intel_pcie_get_resources(struct platform_device *pdev)
{
	struct intel_pcie *pcie = platform_get_drvdata(pdev);
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] [한국어] 오류 로그의 주체. */
	struct device *dev = pci->dev;
	/* [한국어] [한국어] 각 조회의 실패값을 담을 변수. */
	int ret;

	pcie->core_clk = devm_clk_get(dev, NULL);
	/* [한국어] [한국어] 클록 조회 실패. */
	if (IS_ERR(pcie->core_clk)) {
		/* [한국어] [한국어] 오류 코드를 꺼낸다. */
		ret = PTR_ERR(pcie->core_clk);
		/* [한국어] [한국어] 아래 세 자원이 모두 같은 -EPROBE_DEFER 관용구를 쓴다. */
		if (ret != -EPROBE_DEFER)
			/* [한국어] [한국어] 진짜 오류만 남긴다. */
			dev_err(dev, "Failed to get clks: %d\n", ret);
		/* [한국어] [한국어] devm 이라 되감을 것이 없다. */
		return ret;
	}

	pcie->core_rst = devm_reset_control_get(dev, NULL);
	/* [한국어] [한국어] 리셋 조회 실패. */
	if (IS_ERR(pcie->core_rst)) {
		/* [한국어] [한국어] 오류 코드를 꺼낸다. */
		ret = PTR_ERR(pcie->core_rst);
		/* [한국어] [한국어] 같은 관용구. */
		if (ret != -EPROBE_DEFER)
			/* [한국어] [한국어] 리셋 공급자 문제일 수 있다. */
			dev_err(dev, "Failed to get resets: %d\n", ret);
		/* [한국어] [한국어] 실패값을 올린다. */
		return ret;
	}

	ret = device_property_read_u32(dev, "reset-assert-ms",
				       /* [한국어] [한국어] PERST# 유지 시간을 DT 에서 읽는다. */
				       &pcie->rst_intrvl);
	if (ret)
		/* [한국어] [한국어] **실패해도 진행한다** -- 이 속성은 선택이므로 없으면 기본값 100ms 를
		 * 넣는다. 위 세 자원과 달리 반환하지 않는 것이 요점이다. */
		pcie->rst_intrvl = RESET_INTERVAL_MS;

	pcie->app_base = devm_platform_ioremap_resource_byname(pdev, "app");
	/* [한국어] [한국어] APP 창 매핑 실패. */
	if (IS_ERR(pcie->app_base))
		/* [한국어] [한국어] 여기만 로그가 없다 -- devm_platform_ioremap_resource_byname 이
		 * 자체적으로 오류를 찍기 때문이다. */
		return PTR_ERR(pcie->app_base);

	pcie->phy = devm_phy_get(dev, "pcie");
	/* [한국어] [한국어] PHY 조회 실패. */
	if (IS_ERR(pcie->phy)) {
		/* [한국어] [한국어] 오류 코드를 꺼낸다. */
		ret = PTR_ERR(pcie->phy);
		/* [한국어] [한국어] 같은 관용구. */
		if (ret != -EPROBE_DEFER)
			/* [한국어] [한국어] PHY 공급자가 늦게 올라오는 경우가 흔하다. */
			dev_err(dev, "Couldn't get pcie-phy: %d\n", ret);
		/* [한국어] [한국어] 실패값을 올린다. */
		return ret;
	}

	return 0;
}

/* [한국어]
 * intel_pcie_wait_l2 - PM_TURNOFF 를 보내고 링크가 L2 에 들어가기를 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 성공(또는 해당 없음), -ETIMEDOUT 는 5초 안에 L2 에 못 들어감.
 *
 * 시스템을 재우기 전에 링크를 L2(가장 깊은 링크 상태)로 내려야 전원을
 * 안전하게 끊을 수 있다. 그 절차가 'PM_Turn_Off 메시지 전송 → 상대의
 * 응답 → L2 진입' 인데, 이 IP 는 그 전부를 하드웨어가 처리하고 결과만
 * PCIE_APP_PMC 의 IN_L2 비트로 알려 준다.
 *
 * 맨 앞의 `max_link_speed < 3` 조기 반환이 특이하다. Gen3 미만에서는
 * 이 절차를 아예 밟지 않는다 -- 상류에 근거 주석이 없어 이 트리만으로는
 * 이유를 확인하지 못했다. (하드웨어 제약이거나, 저속 링크에서는 L2 절차가
 * 불필요하다는 판단으로 보인다.)
 *
 * readl_poll_timeout 이 20us 간격으로 최대 5초를 기다린다. 5초는 규약이
 * 요구하는 값보다 훨씬 넉넉한데, 응답하지 않는 장치가 있어도 시스템 전체를
 * 멈추지 않으려는 상한으로 보인다.
 *
 * 타임아웃이면 로그를 남기고 실패를 올린다. 호출자 중 suspend_noirq 는
 * 이 실패로 서스펜드를 취소하고, turn_off 는 반환값을 무시한다.
 *
 * 실행 컨텍스트: 서스펜드·제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_turn_off / intel_pcie_suspend_noirq → [이 함수]
 *     → pcie_app_wr_mask → readl_poll_timeout
 */
static int intel_pcie_wait_l2(struct intel_pcie *pcie)
{
	u32 value;
	int ret;
	/* [한국어] [한국어] max_link_speed 를 보기 위해 DWC 인스턴스를 꺼낸다. */
	struct dw_pcie *pci = &pcie->pci;

	if (pci->max_link_speed < 3)
		/* [한국어] [한국어] Gen3 미만에서는 L2 절차를 아예 밟지 않는다. 상류에 근거 주석이
		 * 없어 이 트리만으로는 이유를 확인하지 못했다 -- 하드웨어 제약이거나,
		 * 저속 링크에서는 이 절차가 불필요하다는 판단으로 보인다. */
		return 0;

	/* Send PME_TURN_OFF message */
	pcie_app_wr_mask(pcie, PCIE_APP_MSG_CR, PCIE_APP_MSG_XMT_PM_TURNOFF,
			 PCIE_APP_MSG_XMT_PM_TURNOFF);

	/* Read PMC status and wait for falling into L2 link state */
	ret = readl_poll_timeout(pcie->app_base + PCIE_APP_PMC, value,
				 value & PCIE_APP_PMC_IN_L2, 20,
				 jiffies_to_usecs(5 * HZ));
	if (ret)
		/* [한국어] [한국어] 5초 안에 L2 에 못 들어갔다. 응답하지 않는 장치가 있어도 시스템
		 * 전체를 멈추지 않으려는 상한이다. */
		dev_err(pcie->pci.dev, "PCIe link enter L2 timeout!\n");
/* [한국어] [한국어] 실패를 그대로 올린다. suspend_noirq 는 이 값으로 서스펜드를
 * 취소하고, turn_off 는 무시한다. */

	return ret;
}

/* [한국어]
 * intel_pcie_turn_off - 링크를 재우고 장치를 리셋 상태로 되돌린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * 세 단계:
 *  1. 링크가 서 있을 때만 L2 진입을 시도한다. 이미 내려가 있으면 보낼
 *     상대가 없다. **반환값을 검사하지 않는다** -- 제거 경로라 실패해도
 *     계속 진행해야 하기 때문이다.
 *  2. PERST# 를 assert 해 상대 장치를 리셋 상태로 묶는다.
 *  3. PCI_COMMAND 의 MEMORY 비트를 끈다. 루트 포트가 더 이상 메모리
 *     트랜잭션을 디코딩하지 않게 해, 이후 주소 공간이 재배치돼도 엉뚱한
 *     접근이 일어나지 않게 한다.
 *
 * 실행 컨텍스트: 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   __intel_pcie_remove → [이 함수] → dw_pcie_link_up → intel_pcie_wait_l2
 *     → intel_pcie_device_rst_assert → pcie_rc_cfg_wr_mask
 */
static void intel_pcie_turn_off(struct intel_pcie *pcie)
{
	if (dw_pcie_link_up(&pcie->pci))
		intel_pcie_wait_l2(pcie);

	/* Put endpoint device in reset state */
	intel_pcie_device_rst_assert(pcie);
	pcie_rc_cfg_wr_mask(pcie, PCI_COMMAND, PCI_COMMAND_MEMORY, 0);
}

/* [한국어]
 * intel_pcie_host_setup - 리셋부터 링크 기동까지 하드웨어를 통째로 세운다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 성공, 음수는 각 단계의 실패값.
 *
 * 프로브(dw_pcie_host_init → rc_init)와 재개(resume_noirq) 양쪽에서 불린다.
 * 그래서 '전원을 잃은 하드웨어를 처음 상태로 만드는 데 필요한 전부' 가
 * 이 한 함수에 모여 있다.
 *
 * 순서와 근거:
 *  1. 코어 리셋 assert + 장치 PERST# assert -- 양쪽을 모두 리셋 상태로
 *     묶은 뒤 시작한다.
 *  2. phy_init -- 리셋이 걸린 상태에서 물리 계층을 설정한다.
 *  3. 코어 리셋 deassert -- 컨트롤러를 깨운다(홀드/안정 지연 포함).
 *  4. 코어 클록 인가 -- 리셋이 풀린 뒤라야 클록이 의미가 있다.
 *  5. **pci->atu_base = pci->dbi_base + 0xC0000** -- 이 IP 의 iATU 는 DBI
 *     로부터 고정 오프셋에 있고 별도 reg 로 노출되지 않는다. DWC 코어가
 *     스스로 알 수 없으므로 글루가 알려 준다. dw_pcie_setup_rc 보다 앞에
 *     있어야 창 배분이 올바른 주소에 닿는다.
 *  6. LTSSM 을 끈다 -- 아래 설정 중에 링크가 학습을 시작하면 안 된다.
 *  7. 링크 제어 정비(Link Disable/ASPM 끄기)와 FTS 개수 설정.
 *  8. dw_pcie_setup_rc -- DWC 코어가 설정공간과 iATU 를 프로그래밍한다.
 *  9. dw_pcie_upconfig_setup -- 다중 레인 업컨피그를 켠다.
 * 10. 장치 PERST# 해제(유지 시간 경과 후) → LTSSM 활성 -- 이 순서로
 *     상대가 깨어난 뒤 학습이 시작된다.
 * 11. 링크 대기.
 * 12. 마지막에 APP 인터럽트를 연다. 링크가 선 뒤에 여는 것이 요점 --
 *     그 전에 열면 학습 중의 과도 상태가 인터럽트로 올라온다.
 *
 * 에러 되감기가 두 라벨로 갈린다:
 *  - app_init_err: 클록까지 잡힌 뒤의 실패 -- 클록을 끄고 clk_err 로 흘러간다.
 *  - clk_err: 클록 인가 자체가 실패한 경우 -- 코어 리셋을 다시 걸고 PHY 를 되돌린다.
 * 장치 PERST# 는 되돌리지 않는데, 실패한 컨트롤러 뒤의 장치는 리셋 상태로
 * 두는 편이 안전하기 때문이다.
 *
 * 실행 컨텍스트: 프로브와 재개의 프로세스 문맥. 여러 지연이 있어 잠들 수
 * 있어야 한다.
 *
 * 호출 체인:
 *   intel_pcie_rc_init / intel_pcie_resume_noirq → [이 함수]
 *     → phy_init → clk_prepare_enable → dw_pcie_setup_rc
 *       → intel_pcie_ltssm_enable → dw_pcie_wait_for_link
 */
static int intel_pcie_host_setup(struct intel_pcie *pcie)
{
	int ret;
	struct dw_pcie *pci = &pcie->pci;
/* [한국어] [한국어] 아래가 하드웨어를 세우는 본체다. 순서 하나하나가 의존 관계를 따른다. */

	intel_pcie_core_rst_assert(pcie);
	intel_pcie_device_rst_assert(pcie);

	ret = phy_init(pcie->phy);
	/* [한국어] [한국어] PHY 초기화 실패. */
	if (ret)
		/* [한국어] [한국어] 아직 클록도 리셋도 건드리기 전이라 되감을 것이 없다. */
		return ret;

	intel_pcie_core_rst_deassert(pcie);

	ret = clk_prepare_enable(pcie->core_clk);
	/* [한국어] [한국어] 코어 클록 인가 실패. */
	if (ret) {
		/* [한국어] [한국어] 어느 단계에서 막혔는지 알 수 있게 남긴다. */
		dev_err(pcie->pci.dev, "Core clock enable failed: %d\n", ret);
		/* [한국어] [한국어] 클록을 잡지 못했으므로 clk_err 로 -- 코어 리셋과 PHY 만 되돌린다. */
		goto clk_err;
	}

	pci->atu_base = pci->dbi_base + 0xC0000;
/* [한국어] [한국어] 여기부터가 DWC 코어에 넘기기 전의 IP 설정이다. */

	intel_pcie_ltssm_disable(pcie);
	intel_pcie_link_setup(pcie);
	intel_pcie_init_n_fts(pci);

	ret = dw_pcie_setup_rc(&pci->pp);
	/* [한국어] [한국어] DWC 코어의 RC 설정 실패. */
	if (ret)
		/* [한국어] [한국어] 클록까지 잡혔으므로 app_init_err 로 -- 클록부터 되돌린다. */
		goto app_init_err;

	dw_pcie_upconfig_setup(pci);

	intel_pcie_device_rst_deassert(pcie);
	intel_pcie_ltssm_enable(pcie);

	ret = dw_pcie_wait_for_link(pci);
	/* [한국어] [한국어] 링크 대기 실패. */
	if (ret)
		/* [한국어] [한국어] 같은 라벨로 전부 되돌린다. */
		goto app_init_err;

	/* Enable integrated interrupts */
	pcie_app_wr_mask(pcie, PCIE_APP_IRNEN, PCIE_APP_IRN_INT,
			 PCIE_APP_IRN_INT);

	return 0;

app_init_err:
	clk_disable_unprepare(pcie->core_clk);
clk_err:
	intel_pcie_core_rst_assert(pcie);
	phy_exit(pcie->phy);

	return ret;
}

/* [한국어]
 * __intel_pcie_remove - 하드웨어 쪽 정리만 담당하는 내부 함수
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 없음.
 *
 * host_setup 의 역순이다: 인터럽트 차단 → 링크 정리 → 클록 차단 →
 * 코어 리셋 → PHY 해제.
 *
 * 이름 앞의 밑줄 두 개는 '버스 계층 정리를 하지 않는 하위 판' 이라는 표시다.
 * 버스 제거(dw_pcie_host_deinit)는 호출자인 intel_pcie_remove 가 **먼저**
 * 수행한다 -- 순서가 반대면 아직 살아 있는 장치 드라이버가 이미 꺼진
 * 컨트롤러에 접근한다.
 *
 * 실행 컨텍스트: 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   intel_pcie_remove → [이 함수] → intel_pcie_core_irq_disable
 *     → intel_pcie_turn_off → clk_disable_unprepare
 *       → intel_pcie_core_rst_assert → phy_exit
 */
static void __intel_pcie_remove(struct intel_pcie *pcie)
{
	intel_pcie_core_irq_disable(pcie);
	intel_pcie_turn_off(pcie);
	clk_disable_unprepare(pcie->core_clk);
	intel_pcie_core_rst_assert(pcie);
	phy_exit(pcie->phy);
}

/* [한국어]
 * intel_pcie_remove - 플랫폼 드라이버 제거 진입점
 *
 * @pdev: 제거되는 플랫폼 디바이스.
 * @return: 없음.
 *
 * 두 단계뿐이다. 먼저 dw_pcie_host_deinit 이 버스를 걷어내고 하위 장치
 * 드라이버를 떼어 낸 뒤, __intel_pcie_remove 가 하드웨어를 끈다.
 * 이 순서가 유일한 요점이다.
 *
 * 실행 컨텍스트: 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → dw_pcie_host_deinit → __intel_pcie_remove
 */
static void intel_pcie_remove(struct platform_device *pdev)
{
	struct intel_pcie *pcie = platform_get_drvdata(pdev);
	struct dw_pcie_rp *pp = &pcie->pci.pp;
/* [한국어] [한국어] 아래는 제거·전원관리 경로다. */

	dw_pcie_host_deinit(pp);
	__intel_pcie_remove(pcie);
}

/* [한국어]
 * intel_pcie_suspend_noirq - 링크를 L2 로 내리고 PHY·클록을 끈다
 *
 * @dev: 이 컨트롤러의 device. drvdata 로 인스턴스를 되찾는다.
 * @return: 0 성공, 음수면 서스펜드가 취소된다.
 *
 * noirq 단계인 이유는 여기서 인터럽트를 막고 하드웨어를 재우기 때문이다.
 *
 * 순서:
 *  1. APP 인터럽트를 막고 걸린 상태를 지운다. **가장 먼저** 해야 아래에서
 *     하드웨어가 꺼지는 동안 인터럽트가 들어오지 않는다.
 *  2. L2 진입을 기다린다. **실패하면 그대로 반환해 서스펜드를 취소한다** --
 *     링크가 정리되지 않은 채 전원을 내릴 수 없기 때문이다. 이것이
 *     intel_pcie_turn_off 가 같은 함수의 반환값을 무시하는 것과 대조된다.
 *  3. PHY 와 클록을 끈다.
 *
 * 코어 리셋은 걸지 않는 점에 유의 -- 재개 시 host_setup 이 어차피 다시
 * 걸고 푼다.
 *
 * 실행 컨텍스트: 시스템 서스펜드의 noirq 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq → [이 함수]
 *     → intel_pcie_core_irq_disable → intel_pcie_wait_l2 → phy_exit
 *       → clk_disable_unprepare
 */
static int intel_pcie_suspend_noirq(struct device *dev)
{
	struct intel_pcie *pcie = dev_get_drvdata(dev);
	int ret;
	/* [한국어] [한국어] **가장 먼저** 인터럽트를 막는다. 아래에서 하드웨어가 꺼지는 동안
	 * 인터럽트가 들어오면 사라진 컨트롤러를 향한 핸들러가 돈다. */
	intel_pcie_core_irq_disable(pcie);
	ret = intel_pcie_wait_l2(pcie);
	/* [한국어] [한국어] L2 진입 실패. */
	if (ret)
		/* [한국어] [한국어] **여기서는 실패를 올려 서스펜드를 취소한다.** 링크가 정리되지 않은
		 * 채 전원을 내릴 수 없기 때문이다. intel_pcie_turn_off 가 같은 함수의
		 * 반환값을 무시하는 것과 대조된다 -- 그쪽은 제거 경로라 물러설 곳이 없다. */
		return ret;

	phy_exit(pcie->phy);
	clk_disable_unprepare(pcie->core_clk);
	return ret;
}

/* [한국어]
 * intel_pcie_resume_noirq - 재개 시 하드웨어를 처음부터 다시 세운다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: intel_pcie_host_setup 의 반환값 그대로.
 *
 * 서스펜드에서 PHY 와 클록을 껐으므로, 재개는 프로브와 똑같은 초기화가
 * 필요하다. 그래서 host_setup 을 그대로 다시 부르는 것이 전부다 --
 * 이 함수가 존재하는 이유가 곧 host_setup 을 재사용 가능하게 만든 이유다.
 *
 * dw_pcie_resume_noirq 를 쓰지 않는 점에 유의. 이 드라이버는 DWC 코어의
 * 서스펜드/재개 헬퍼 대신 자체 L2 절차를 쓰므로, pci->suspended 플래그
 * 경로와 무관하게 동작한다.
 *
 * 실행 컨텍스트: 시스템 재개의 noirq 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq → [이 함수] → intel_pcie_host_setup
 */
static int intel_pcie_resume_noirq(struct device *dev)
{
	struct intel_pcie *pcie = dev_get_drvdata(dev);
	return intel_pcie_host_setup(pcie);
}

/* [한국어]
 * intel_pcie_rc_init - DWC 호스트 코어가 되부르는 SoC 초기화 훅
 *
 * @pp: DWC 루트 포트.
 * @return: intel_pcie_host_setup 의 반환값.
 *
 * dw_pcie_host_init 이 브리지와 자원을 갖춘 뒤, 링크를 올리기 전에 부른다.
 *
 * 인스턴스를 되찾는 경로가 특이하다: to_dw_pcie_from_pp 로 dw_pcie 를 얻은
 * 뒤 **dev_get_drvdata(pci->dev)** 로 intel_pcie 를 꺼낸다. container_of
 * 로도 가능했겠지만(첫 필드가 pci 다), 상류는 drvdata 방식을 택했다.
 * probe 가 platform_set_drvdata 를 일찍 부르는 이유가 여기 있다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init → pp->ops->init → [이 함수] → intel_pcie_host_setup
 */
static int intel_pcie_rc_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct intel_pcie *pcie = dev_get_drvdata(pci->dev);
	/* [한국어] [한국어] 서스펜드에서 PHY 와 클록을 껐으므로 프로브와 똑같은 초기화가 필요하다.
	 * 그래서 host_setup 을 그대로 다시 부르는 것이 전부다. */
	return intel_pcie_host_setup(pcie);
/* [한국어] [한국어] DWC 코어의 dw_pcie_resume_noirq 를 쓰지 않는 점에 유의 -- 이
 * 드라이버는 자체 L2 절차를 쓰므로 pci->suspended 경로와 무관하다. */
}

static const struct dw_pcie_ops intel_pcie_ops = {
/* [한국어] [한국어] **비어 있는 ops 테이블.** link_up/start_link/stop_link 를 하나도
 * 등록하지 않는다. 링크 기동을 DWC 코어에 맡기지 않고 host_setup 이 직접
 * LTSSM 을 켜기 때문이고, 링크 판정은 코어의 기본 구현(PORT_DEBUG1 비트)이
 * 그대로 쓰인다. */
};

static const struct dw_pcie_host_ops intel_pcie_dw_ops = {
	/* [한국어] [한국어] 되부를 훅은 init 하나뿐이다. 이 SoC 는 자체 MSI 도, post_init 도 없다. */
	.init = intel_pcie_rc_init,
};

/* [한국어]
 * intel_pcie_probe - 드라이버 진입점
 *
 * @pdev: 매칭된 플랫폼 디바이스.
 * @return: 0 성공, 음수는 각 단계의 실패값.
 *
 * 하드웨어를 직접 만지지 않는다 -- 자원만 챙기고 DWC 코어에 넘긴다.
 * 실제 초기화는 코어가 되부르는 intel_pcie_rc_init 에서 일어난다.
 *
 * 단계:
 *  1. 인스턴스 할당(devm)과 platform_set_drvdata. **다른 어떤 초기화보다
 *     먼저** 심어야 한다 -- get_resources 와 rc_init 이 모두 drvdata 로
 *     인스턴스를 되찾기 때문이다.
 *  2. pci->dev 를 채운다.
 *  3. **pci->use_parent_dt_ranges = true** -- 이 컨트롤러의 주소 변환
 *     정보가 자기 노드가 아니라 부모 노드의 ranges 에 기술되어 있다는
 *     표시다. DWC 코어가 이 플래그를 보고 어느 노드를 읽을지 정한다.
 *  4. 자원 확보(클록/리셋/APP 창/PHY/리셋 시간).
 *  5. PERST# GPIO 확보.
 *  6. ops 를 걸고 dw_pcie_host_init 호출. 이 호출이 돌아올 때는 이미
 *     링크가 서고 버스 열거까지 끝나 있다.
 *
 * 되감기 라벨이 없는 점에 유의 -- 잡는 자원이 모두 devm 이라 실패 시
 * 자동으로 풀린다. 다만 dw_pcie_host_init 이 실패하면 그 안에서 이미
 * host_setup 이 걸어 둔 클록·PHY 를 되감은 뒤다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수] → intel_pcie_get_resources
 *     → intel_pcie_ep_rst_init → dw_pcie_host_init
 *       → (되돌아) intel_pcie_rc_init
 */
static int intel_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct intel_pcie *pcie;
	/* [한국어] [한국어] DWC 루트 포트를 가리킬 포인터. */
	struct dw_pcie_rp *pp;
	/* [한국어] [한국어] DWC 인스턴스를 가리킬 포인터. */
	struct dw_pcie *pci;
	/* [한국어] [한국어] 각 단계의 실패값. */
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] [한국어] 인스턴스 할당 실패. */
	if (!pcie)
		/* [한국어] [한국어] devm 이라 이후 자동 해제된다. */
		return -ENOMEM;

	platform_set_drvdata(pdev, pcie);
	/* [한국어] [한국어] 임베디드 DWC 인스턴스의 주소를 잡아 둔다. */
	pci = &pcie->pci;
	/* [한국어] [한국어] DWC 코어가 로그와 DT 접근에 쓸 device. */
	pci->dev = dev;
	/* [한국어] [한국어] 이 컨트롤러의 주소 변환 정보가 자기 노드가 아니라 **부모 노드의**
	 * ranges 에 기술되어 있다는 표시. DWC 코어가 이 플래그를 보고 어느 노드를
	 * 읽을지 정한다. */
	pci->use_parent_dt_ranges = true;
	/* [한국어] [한국어] 루트 포트 구조체의 주소를 잡아 둔다. */
	pp = &pci->pp;

	ret = intel_pcie_get_resources(pdev);
	/* [한국어] [한국어] 자원 확보 실패. */
	if (ret)
		/* [한국어] [한국어] devm 이라 되감을 것이 없다. */
		return ret;

	ret = intel_pcie_ep_rst_init(pcie);
	/* [한국어] [한국어] PERST# GPIO 확보 실패. */
	if (ret)
		/* [한국어] [한국어] 마찬가지로 devm 이 처리한다. */
		return ret;

	pci->ops = &intel_pcie_ops;
	/* [한국어] [한국어] 호스트 훅을 건다. 이것이 있어야 코어가 rc_init 을 되부른다. */
	pp->ops = &intel_pcie_dw_ops;

	ret = dw_pcie_host_init(pp);
	/* [한국어] [한국어] 호스트 초기화 실패. 이 안에서 rc_init → host_setup 이 불리고,
	 * 실패하면 그쪽이 이미 클록·PHY 를 되감은 뒤다. */
	if (ret) {
		/* [한국어] [한국어] 어느 단계에서 막혔는지 구별할 수 있게 남긴다. */
		dev_err(dev, "Cannot initialize host\n");
		/* [한국어] [한국어] 실패값을 올려 프로브를 접는다. */
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops intel_pcie_pm_ops = {
	/* [한국어] [한국어] noirq 단계에만 콜백을 등록하는 매크로. 일반 suspend/resume 은
	 * 두지 않는데, 이 드라이버가 하는 일이 모두 인터럽트가 꺼진 뒤에 해야 하는
	 * 것이기 때문이다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(intel_pcie_suspend_noirq,
				  intel_pcie_resume_noirq)
};

static const struct of_device_id of_intel_pcie_match[] = {
	/* [한국어] [한국어] Intel LGM(Lightning Mountain) SoC 의 PCIe 컨트롤러.
	 * 이 드라이버가 지원하는 유일한 compatible 이다. */
	{ .compatible = "intel,lgm-pcie" },
	/* [한국어] [한국어] 표의 끝을 알리는 빈 항목. */
	{}
};

static struct platform_driver intel_pcie_driver = {
	/* [한국어] [한국어] 프로브 진입점. */
	.probe = intel_pcie_probe,
	/* [한국어] [한국어] 제거 진입점. 반환형이 void 인 최신 규약을 따른다. */
	.remove = intel_pcie_remove,
	.driver = {
		.name = "intel-gw-pcie",
		/* [한국어] [한국어] 위 표를 걸어, DT 노드의 compatible 이 맞으면 probe 가 불린다. */
		.of_match_table = of_intel_pcie_match,
		.pm = &intel_pcie_pm_ops,
	},
};
builtin_platform_driver(intel_pcie_driver);
