// SPDX-License-Identifier: GPL-2.0
/*
 * DWC PCIe RC driver for Toshiba Visconti ARM SoC
 *
 * Copyright (C) 2021 Toshiba Electronic Device & Storage Corporation
 * Copyright (C) 2021 TOSHIBA CORPORATION
 *
 * Nobuhiro Iwamatsu <nobuhiro1.iwamatsu@toshiba.co.jp>
 */

/*
 * [한국어 설명] Toshiba Visconti SoC 의 DesignWare PCIe 호스트 글루 (pcie-visconti.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 Toshiba Visconti ARM SoC 에 붙이는 글루
 * 드라이버다. config 접근, ATU 설정, MSI, 버스 스캔은 모두
 * pcie-designware-host.c 가 하고, 이 파일은 **DWC 코어가 알 수 없는 SoC
 * 고유의 것** 만 맡는다.
 *
 * 이 SoC 에서 그것이 유난히 많아, 레지스터 창을 셋이나 다룬다.
 *   ulreg — PCIe 래퍼 레지스터. 모드 선택(RC/EP), PERST# 제어, PHY SRAM
 *           초기화, LTSSM 시작, 링크 상태가 모두 여기 있다.
 *   smu   — 시스템 관리 유닛. 클럭 게이트와 리셋 해제를 담당한다.
 *   mpu   — 메모리 보호 유닛. 링크가 선 뒤에야 PCIe 쪽 접근을 열어 준다.
 *
 * 그 셋을 다루는 순서가 이 파일의 내용 대부분이다. 클럭을 켜고, 리셋을
 * 단계적으로 풀고, PHY 의 SRAM 초기화를 기다리고, 모드를 정하고,
 * PERST# 를 내렸다 올리고, 코어 리셋이 풀리기를 기다린다. 그 순서를
 * 어기면 링크가 서지 않는다.
 *
 * 주소 변환도 이 파일이 맡는다. 이 SoC 는 CPU 버스 주소에서 0x40000000 을
 * 뺀 값을 PCIe 버스로 내보내므로, DWC 코어가 ATU 를 설정할 때 그 차이를
 * 보정해 줘야 한다(visconti_pcie_cpu_addr_fixup).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> visconti_pcie_probe()
 *     -> 레지스터 창 셋과 클럭 셋을 얻는다
 *     -> visconti_add_pcie_port() -> dw_pcie_host_init()
 *        -> 그 안에서 콜백 -> [이 파일] visconti_pcie_host_init()
 *           -> smu 로 클럭 게이트를 열고 리셋을 단계적으로 푼다
 *           -> RC 모드로 설정하고 PERST# 를 조작한다
 *           -> PHY SRAM 초기화와 코어 리셋 해제를 폴링으로 기다린다
 *        -> DWC 코어가 이어서 start_link 콜백을 부른다
 *           -> [이 파일] visconti_pcie_start_link()
 *              -> LTSSM 을 켜고 L0 을 기다린 뒤 mpu 를 열어 준다
 *        -> DWC 코어가 ATU 를 설정하며 cpu_addr_fixup 콜백을 부른다
 *        -> 버스 스캔
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 이 파일에는 인터럽트 핸들러가
 * 없다 — INTx 와 MSI 를 모두 DWC 코어가 다루고, 이 파일은 SoC 레벨 인터럽트
 * 마스크(PCIE_UL_REG_S_INT_EVENT_MASK1)를 한 번 쓰기만 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c. 접점이 두 벌의
 *   콜백 표다 — dw_pcie_ops(코어 동작 넷)와 dw_pcie_host_ops(호스트 초기화 하나).
 *   dw_pcie_ops 에 콜백이 넷이나 있는 것이 앞의 sophgo 판(콜백 하나)과
 *   대비되며, 이 SoC 가 DWC 표준에서 그만큼 더 벗어나 있다는 뜻이다.
 * 옆쪽: 없다. 이 파일은 다른 드라이버와 직접 이어지지 않는다.
 *
 * 데이터 흐름:
 *   디바이스 트리("ulreg"/"smu"/"mpu" 자원, ref/core/aux 클럭, "intr" 인터럽트)
 *     -> probe -> struct visconti_pcie
 *   주소: CPU 주소 -> cpu_addr_fixup -> PCIe 버스 주소(0x40000000 만큼 낮다)
 *   상태: ulreg 의 PHY_ST_02 -> link_up 판정 -> DWC 코어의 링크 대기
 *
 * 공유 상태: struct visconti_pcie 하나. probe 후 모두 불변이며,
 *   잠금이 하나도 없다 — 이 파일의 모든 함수가 probe 경로에서만 불리기 때문이다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면, 그 BAR 접근이 위 주소 변환을
 * 거친다. nvme_probe() 가 ioremap 한 주소로 doorbell 을 쓰면 CPU 버스에서
 * 그 주소가 나가고, ATU 가 0x40000000 을 뺀 PCIe 주소로 바꿔 컨트롤러에
 * 전달한다. 그 뺄셈이 이 파일의 cpu_addr_fixup 이 코어에 알려 준 규칙이다.
 *
 * === 주요 함수/구조체 요약 ===
 * visconti_pcie_host_init()      : 이 파일에서 가장 긴 함수. 클럭·리셋·PHY
 *                                  초기화 순서가 전부 여기 있다.
 * visconti_pcie_start_link()     : LTSSM 을 켜고 L0 을 기다린 뒤 mpu 를 연다.
 * visconti_pcie_stop_link()      : 그 반대. LTSSM 을 끄고 mpu 를 닫는다.
 * visconti_pcie_link_up()        : ulreg 의 PHY 상태로 링크 여부를 답한다.
 * visconti_pcie_cpu_addr_fixup() : CPU 주소를 PCIe 버스 주소로 바꾼다.
 * visconti_get_resources()       : 레지스터 창 셋과 클럭 셋을 얻는다.
 * struct visconti_pcie           : dw_pcie 를 맨 앞에 둔 이 드라이버의 상태.
 *                                  레지스터 창 셋과 클럭 셋을 담는다.
 */

/* [한국어] struct clk 와 devm_clk_get(). 다만 이 파일은 클럭을 얻기만 하고 켜지 않는다. */
#include <linux/clk.h>
/* [한국어] ndelay()/udelay(). host_init 의 정해진 대기 두 곳이 쓴다. */
#include <linux/delay.h>
/* [한국어] GPIO 헤더. 이 파일에서 GPIO 를 쓰는 곳은 없다(전수 확인) — 상류가 포함해 두었다. */
#include <linux/gpio.h>
/* [한국어] 인터럽트 헤더. 역시 이 파일은 핸들러를 두지 않는다. */
#include <linux/interrupt.h>
/* [한국어] __init 표시. */
#include <linux/init.h>
/* [한국어] readl_relaxed_poll_timeout(). 이 파일의 세 폴링이 모두 이것이다. */
#include <linux/iopoll.h>
/* [한국어] 기본 커널 관용구. */
#include <linux/kernel.h>
/* [한국어] of_platform 헤더. */
#include <linux/of_platform.h>
/* [한국어] PCI 코어 타입들. */
#include <linux/pci.h>
/* [한국어] struct platform_device 와 platform_get_irq_byname(). */
#include <linux/platform_device.h>
/* [한국어] struct resource. */
#include <linux/resource.h>
/* [한국어] u32, u64 등 기본 타입. */
#include <linux/types.h>

/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_ops, dw_pcie_host_init(). */
#include "pcie-designware.h"
/* [한국어] drivers/pci 안에서만 쓰는 선언들. */
#include "../../pci.h"

/* [한국어] 이 드라이버의 상태 전부. */
struct visconti_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **맨 앞에 두어** 이 구조체 포인터가 그대로
	 * struct dw_pcie 포인터로 쓰인다.
	 * 설정자: probe 가 dev 와 ops 를 채우고, DWC 코어가 나머지를 채운다.
	 * 읽는 자: DWC 코어 전체.
	 * 값 범위: DWC 코어가 정의한 구조체.
	 * 동기화: 코어가 관리한다. */
	struct dw_pcie pci;
	/* [한국어] PCIe 래퍼(ulreg) 창의 가상 주소.
	 * 설정자: visconti_get_resources() 의 ioremap("ulreg").
	 * 읽는 자: visconti_ulreg_readl()/writel() 과 세 폴링.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. 이 창에 모드·PERST#·PHY 초기화·LTSSM·링크 상태가 다 있다. */
	void __iomem *ulreg_base;
	/* [한국어] 시스템 관리 유닛(smu) 창의 가상 주소.
	 * 설정자: visconti_get_resources() 의 ioremap("smu").
	 * 읽는 자: visconti_smu_writel() 뿐 — 이 창은 쓰기만 한다.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. */
	void __iomem *smu_base;
	/* [한국어] 메모리 보호 유닛(mpu) 창의 가상 주소.
	 * 설정자: visconti_get_resources() 의 ioremap("mpu").
	 * 읽는 자: visconti_mpu_readl()/writel().
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. 이 창의 비트를 start_link 가 열고 stop_link 가 닫는다. */
	void __iomem *mpu_base;
	/* [한국어] 참조 클럭.
	 * 설정자: visconti_get_resources() 의 devm_clk_get("ref").
	 * 읽는 자: **없다.** 이 파일 전수 확인 결과 얻기만 하고 prepare/enable 하지 않으며,
	 * 원본(1f0e418bb6)에서도 같다. 클럭 게이트는 host_init 이 smu 레지스터로 직접 여는데,
	 * 그 둘이 같은 클럭을 가리키는지는 이 트리에서 확인 못 함.
	 * 값 범위: 유효한 clk 포인터. devres 가 관리한다.
	 * 동기화: probe 후 불변. */
	struct clk *refclk;
	/* [한국어] 코어 클럭. 위 refclk 와 같다 — 얻기만 하고 쓰는 곳이 없다.
	 * 설정자: devm_clk_get("core").
	 * 읽는 자: 없다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: probe 후 불변. */
	struct clk *coreclk;
	/* [한국어] 보조 클럭. 위 둘과 같다 — 얻기만 하고 쓰는 곳이 없다.
	 * 설정자: devm_clk_get("aux").
	 * 읽는 자: 없다. 다만 host_init 이 smu 의 PISMU_CKON_PCIE_AUX_CLK 비트를
	 * 직접 열어 주므로, 같은 클럭을 다른 경로로 켜는 것일 수 있다(확인 못 함).
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: probe 후 불변. */
	struct clk *auxclk;
/* [한국어] 이 드라이버의 상태 전부. 레지스터 창 셋과 클럭 셋을 담는다. */
};

/* [한국어] 모드 선택 레지스터. */
#define PCIE_UL_REG_S_PCIE_MODE		0x00F4
/* [한국어] 엔드포인트 모드. 이 파일에서 읽는 곳은 없다 — 이 드라이버는 RC 전용이다. */
#define  PCIE_UL_REG_S_PCIE_MODE_EP	0x00
/* [한국어] 루트 컴플렉스 모드. host_init 이 이 값을 쓴다. */
#define  PCIE_UL_REG_S_PCIE_MODE_RC	0x04

/* [한국어] PERST#(하위 장치 리셋) 제어 레지스터. */
#define PCIE_UL_REG_S_PERSTN_CTRL	0x00F8
/* [한국어] IOM 쪽 PERST# 입력을 허용한다. */
#define  PCIE_UL_IOM_PCIE_PERSTN_I_EN	BIT(3)
/* [한국어] 직접 제어를 허용한다 — 아래 DIRECT_PERSTN 이 실제로 동작하게 하는 조건이다. */
#define  PCIE_UL_DIRECT_PERSTN_EN	BIT(2)
/* [한국어] PERST# 출력 값. host_init 이 이 비트를 나중에 세워 하위 장치의 리셋을 푼다. */
#define  PCIE_UL_PERSTN_OUT		BIT(1)
/* [한국어] 직접 제어할 PERST# 값. */
#define  PCIE_UL_DIRECT_PERSTN		BIT(0)
/* [한국어] host_init 이 처음 쓰는 조합. 출력 비트가 빠져 있어,
 * 이 값을 쓰면 하위 장치에 리셋이 걸린 상태가 된다. */
#define  PCIE_UL_REG_S_PERSTN_CTRL_INIT	(PCIE_UL_IOM_PCIE_PERSTN_I_EN | \
					 PCIE_UL_DIRECT_PERSTN_EN | \
					 PCIE_UL_DIRECT_PERSTN)

/* [한국어] PHY 초기화 레지스터 2 — SRAM 로드 완료를 표시하는 자리다. */
#define PCIE_UL_REG_S_PHY_INIT_02	0x0104
/* [한국어] SRAM 외부 로드 완료. 커널이 따로 로드할 것이 없다는 표시다. */
#define  PCIE_UL_PHY0_SRAM_EXT_LD_DONE	BIT(0)

/* [한국어] PHY 초기화 레지스터 3 — SRAM 초기화 상태를 읽는 자리다. */
#define PCIE_UL_REG_S_PHY_INIT_03	0x0108
/* [한국어] SRAM 초기화 완료. host_init 이 이 비트를 폴링한다. */
#define  PCIE_UL_PHY0_SRAM_INIT_DONE	BIT(0)

/* [한국어] SoC 레벨 인터럽트 마스크. */
#define PCIE_UL_REG_S_INT_EVENT_MASK1	0x0138
/* [한국어] config 공간을 통한 PME 인터럽트. */
#define  PCIE_UL_CFG_PME_INT		BIT(0)
/* [한국어] 링크 이퀄라이제이션 요청 인터럽트. */
#define  PCIE_UL_CFG_LINK_EQ_REQ_INT	BIT(1)
/* [한국어] eDMA 채널 0 인터럽트. */
#define  PCIE_UL_EDMA_INT0		BIT(2)
/* [한국어] eDMA 채널 1. */
#define  PCIE_UL_EDMA_INT1		BIT(3)
/* [한국어] eDMA 채널 2. */
#define  PCIE_UL_EDMA_INT2		BIT(4)
/* [한국어] eDMA 채널 3. */
#define  PCIE_UL_EDMA_INT3		BIT(5)
/* [한국어] 위 여섯을 모두 허용하는 조합. start_link 가 링크를 세운 뒤 이 값을 쓴다. */
#define  PCIE_UL_S_INT_EVENT_MASK1_ALL  (PCIE_UL_CFG_PME_INT | \
					 PCIE_UL_CFG_LINK_EQ_REQ_INT | \
					 PCIE_UL_EDMA_INT0 | \
					 PCIE_UL_EDMA_INT1 | \
					 PCIE_UL_EDMA_INT2 | \
					 PCIE_UL_EDMA_INT3)

/* [한국어] 사이드밴드 감시 레지스터. 이 파일에서 읽는 곳은 없다(전수 확인). */
#define PCIE_UL_REG_S_SB_MON		0x0198
/* [한국어] 신호 감시 레지스터. */
#define PCIE_UL_REG_S_SIG_MON		0x019C
/* [한국어] 코어 리셋 해제 감시 비트. host_init 의 마지막 폴링이 이것을 기다린다. */
#define  PCIE_UL_CORE_RST_N_MON		BIT(0)

/* [한국어] SII 디버그 레지스터. 이 파일에서 읽는 곳은 없다(전수 확인). */
#define PCIE_UL_REG_V_SII_DBG_00	0x0844
/* [한국어] SII 일반 제어 레지스터 1. */
#define PCIE_UL_REG_V_SII_GEN_CTRL_01	0x0860
/* [한국어] LTSSM 시작 비트. start_link 가 세우고 stop_link 가 지운다. */
#define  PCIE_UL_APP_LTSSM_ENABLE	BIT(0)

/* [한국어] PHY 상태 레지스터 0. 이 파일에서 읽는 곳은 없다(전수 확인). */
#define PCIE_UL_REG_V_PHY_ST_00		0x0864
/* [한국어] SMLH 링크 업 비트. 위 레지스터를 읽지 않으므로 이 비트도 쓰이지 않는다. */
#define  PCIE_UL_SMLH_LINK_UP		BIT(0)

/* [한국어] PHY 상태 레지스터 2 — 링크 상태 기계의 현재 상태가 여기 있다. */
#define PCIE_UL_REG_V_PHY_ST_02		0x0868
/* [한국어] 감지 활성 상태. 이 파일에서 읽는 곳은 없다(전수 확인).
 * 다만 값이 0x01 이라, 아래 L0(0x11)과 **비트가 겹친다** —
 * link_up 이 동등 비교가 아니라 AND 로 판정하므로 이 상태에서도 참이 된다. */
#define  PCIE_UL_S_DETECT_ACT		0x01
/* [한국어] L0 상태 — 링크가 완전히 동작하는 상태다. 값이 0x11 로 두 비트짜리이며,
 * link_up 과 start_link 가 이것을 AND 마스크로 쓴다. */
#define  PCIE_UL_S_L0			0x11

/* [한국어] PCIe 클럭 게이트 레지스터(smu 창). */
#define PISMU_CKON_PCIE			0x0038
/* [한국어] 보조 클럭 게이트. host_init 이 이 비트를 연다.
 * 같은 이름의 auxclk 를 devm_clk_get 으로도 얻어 두는데,
 * 그쪽은 쓰이지 않는다 — 둘이 같은 클럭인지는 이 트리에서 확인 못 함. */
#define  PISMU_CKON_PCIE_AUX_CLK	BIT(1)
/* [한국어] 마스터 AXI 클럭 게이트. 위와 함께 열린다. */
#define  PISMU_CKON_PCIE_MSTR_ACLK	BIT(0)

/* [한국어] PCIe 리셋 해제 레지스터(smu 창). */
#define PISMU_RSOFF_PCIE		0x0538
/* [한국어] ulreg 블록의 리셋 해제. 이 비트를 써야 ulreg 창에 접근할 수 있다. */
#define  PISMU_RSOFF_PCIE_ULREG_RST_N	BIT(1)
/* [한국어] 전원 상승 리셋 해제. PERST# 조작이 끝난 뒤에 쓴다.
 * 두 비트를 **따로** 쓰는 것이 요점으로, 한 번에 쓰면 순서가 무너진다. */
#define  PISMU_RSOFF_PCIE_PWR_UP_RST_N	BIT(0)

/* [한국어] 메모리 보호 유닛의 활성화 레지스터(mpu 창). 오프셋이 0 이다. */
#define PCIE_MPU_REG_MP_EN		0x0
/* [한국어] 비활성화 비트 — 이름이 가리키는 대로 세우면 막히고 지우면 열린다.
 * start_link 가 지우고 stop_link 가 세운다. */
#define  MPU_MP_EN_DISABLE		BIT(0)

/* Access registers in PCIe ulreg */
/* [한국어]
 * visconti_ulreg_writel - PCIe 래퍼(ulreg) 레지스터에 쓴다
 *
 * @pcie: 드라이버 상태.
 * @val: 쓸 값.
 * @reg: ulreg 창 안의 오프셋.
 *
 * 이 파일이 다루는 세 창 중 가장 많이 쓰이는 것이다. 모드 선택, PERST# 제어,
 * PHY SRAM 초기화, LTSSM 시작이 모두 이 창에 있다(옆의 상류 주석).
 *
 * _relaxed 판인 것이 이 파일 전체에 일관된다. 다만 초기화 순서가 중요한
 * 함수들이라, 순서를 보장하는 것은 배리어가 아니라 그 사이의 지연과 폴링이다.
 *
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   visconti_pcie_start_link() / stop_link() / host_init() → [이 함수]
 *     → writel_relaxed()
 */
static void visconti_ulreg_writel(struct visconti_pcie *pcie, u32 val, u32 reg)
{
	writel_relaxed(val, pcie->ulreg_base + reg);
}

/* [한국어]
 * visconti_ulreg_readl - PCIe 래퍼(ulreg) 레지스터를 읽는다
 *
 * @pcie: 드라이버 상태.
 * @reg: ulreg 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * visconti_ulreg_writel() 의 짝이다.
 *
 * 이 파일에서 이 함수를 쓰는 곳은 stop_link 하나뿐이다. 나머지 읽기는
 * readl_relaxed_poll_timeout() 이 주소를 직접 받아 처리하거나,
 * link_up 이 readl_relaxed 를 직접 부른다 — 상류 코드가 그렇게 섞여 있다.
 *
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   visconti_pcie_stop_link() → [이 함수] → readl_relaxed()
 */
static u32 visconti_ulreg_readl(struct visconti_pcie *pcie, u32 reg)
{
	return readl_relaxed(pcie->ulreg_base + reg);
}

/* Access registers in PCIe smu */
/* [한국어]
 * visconti_smu_writel - 시스템 관리 유닛(smu) 레지스터에 쓴다
 *
 * @pcie: 드라이버 상태.
 * @val: 쓸 값.
 * @reg: smu 창 안의 오프셋.
 *
 * smu 는 PCIe 블록 바깥에 있는 SoC 차원의 관리 유닛이다(옆의 상류 주석).
 * 클럭 게이트를 열고 리셋을 푸는 두 가지 일만 이 파일에서 쓰인다.
 *
 * 읽기 짝이 없다. 이 창의 두 레지스터를 쓰기만 하고 확인하지 않는데,
 * 확인은 ulreg 쪽의 상태 레지스터를 폴링하는 것으로 대신한다 — 리셋이
 * 실제로 풀렸는지는 코어 리셋 감시 비트가 알려 준다.
 *
 * 실행 컨텍스트: host_init 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   visconti_pcie_host_init() → [이 함수] → writel_relaxed()
 */
static void visconti_smu_writel(struct visconti_pcie *pcie, u32 val, u32 reg)
{
	writel_relaxed(val, pcie->smu_base + reg);
}

/* Access registers in PCIe mpu */
/* [한국어]
 * visconti_mpu_writel - 메모리 보호 유닛(mpu) 레지스터에 쓴다
 *
 * @pcie: 드라이버 상태.
 * @val: 쓸 값.
 * @reg: mpu 창 안의 오프셋.
 *
 * mpu 는 PCIe 쪽에서 오는 접근을 막거나 여는 관문이다(옆의 상류 주석).
 *
 * 이 파일에서 쓰는 곳이 둘이며 방향이 반대다 — 링크가 서면 열고(start_link),
 * 링크를 끊으면 닫는다(stop_link). 링크가 없을 때 닫아 두는 것이 안전한데,
 * 그 상태에서 들어오는 접근은 정상적인 것일 수 없기 때문이다.
 *
 * 실행 컨텍스트: start_link / stop_link 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   visconti_pcie_start_link() / visconti_pcie_stop_link() → [이 함수]
 *     → writel_relaxed()
 */
static void visconti_mpu_writel(struct visconti_pcie *pcie, u32 val, u32 reg)
{
	writel_relaxed(val, pcie->mpu_base + reg);
}

/* [한국어]
 * visconti_mpu_readl - 메모리 보호 유닛(mpu) 레지스터를 읽는다
 *
 * @pcie: 드라이버 상태.
 * @reg: mpu 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * visconti_mpu_writel() 의 짝이며, 언제나 읽기-수정-쓰기의 첫 단계로 쓰인다.
 * 같은 레지스터의 다른 비트를 보존해야 하기 때문이다.
 *
 * 실행 컨텍스트: start_link / stop_link 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   visconti_pcie_start_link() / visconti_pcie_stop_link() → [이 함수]
 *     → readl_relaxed()
 */
static u32 visconti_mpu_readl(struct visconti_pcie *pcie, u32 reg)
{
	return readl_relaxed(pcie->mpu_base + reg);
}

/* [한국어]
 * visconti_pcie_link_up - 링크가 서 있는지 답한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: true = 링크가 섰다, false = 아니다.
 *
 * DWC 코어가 링크를 기다릴 때 반복해 부르는 콜백이다.
 *
 * ulreg 의 PHY 상태 레지스터를 읽어 L0 상태인지 본다. L0 은 링크가 완전히
 * 동작하는 상태를 뜻하며, 그보다 앞선 단계(감지, 훈련)에서는 아직 config
 * 접근이 되지 않는다.
 *
 * [상류 코드 관찰] 판정이 **동등 비교가 아니라 비트 마스크** 다. L0 의 값이
 * 0x11 이므로 `val & 0x11` 은 비트 0 이나 비트 4 중 하나만 서 있어도 참이
 * 된다. 같은 파일에 정의된 DETECT_ACT 가 0x01 이라, 그 상태에서도 이 함수가
 * true 를 돌려주는 셈이다. 원본(1f0e418bb6:130)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * drvdata 로 드라이버 상태를 되찾는 것이 이 파일의 관용이다. dw_pcie 가
 * 구조체 맨 앞에 있어 container_of 로도 가능하지만, 상류가 drvdata 를 쓴다.
 *
 * 실행 컨텍스트: DWC 코어의 링크 대기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.link_up == [이 함수] → readl_relaxed()
 */
static bool visconti_pcie_link_up(struct dw_pcie *pci)
{
	struct visconti_pcie *pcie = dev_get_drvdata(pci->dev);
	void __iomem *addr = pcie->ulreg_base;
	/* [한국어] PHY 상태 레지스터를 직접 읽는다. 위 visconti_ulreg_readl() 을 두고도
	 * readl_relaxed 를 직접 부르는데, 상류 코드가 그렇게 섞여 있다. */
	u32 val = readl_relaxed(addr + PCIE_UL_REG_V_PHY_ST_02);

	return val & PCIE_UL_S_L0;
}

/* [한국어]
 * visconti_pcie_start_link - LTSSM 을 켜고 링크가 선 뒤 mpu 를 연다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 0 = 성공, -ETIMEDOUT.
 *
 * host_init 이 하드웨어를 준비한 뒤 DWC 코어가 부르는 콜백이다.
 *
 * 세 단계다.
 * 1. LTSSM(Link Training and Status State Machine)을 켠다. 이 비트를 세우기
 *    전에는 하드웨어가 링크 훈련을 시작하지 않는다.
 * 2. L0 이 될 때까지 기다린다. 90ms 간격으로 100ms 까지 — 즉 사실상 한 번
 *    자고 한 번 더 확인하는 셈이다. 링크 훈련이 그 안에 끝나지 않으면
 *    -ETIMEDOUT 이다.
 * 3. SoC 레벨 인터럽트 마스크를 열고, 링크가 실제로 섰으면 mpu 를 연다.
 *
 * 3번의 순서가 요점이다. mpu 를 **링크가 선 뒤에** 여는데, 그 전에 열면
 * 링크가 없는 상태에서 들어온 접근이 통과하게 된다.
 *
 * dw_pcie_link_up() 을 다시 확인하는 것이 눈에 띈다. 위에서 이미 L0 을
 * 기다렸는데도 코어의 판정을 한 번 더 거치는데, 그쪽은 DWC 표준 레지스터를
 * 보므로 ulreg 의 판정과 근거가 다르다. 둘 다 참이어야 mpu 를 연다.
 *
 * 실행 컨텍스트: DWC 코어의 초기화. 폴링 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 링크 훈련이 시간 안에 끝나지 않으면 -ETIMEDOUT 이며,
 * 그때 mpu 는 닫힌 채로 남는다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수]
 *     → visconti_ulreg_writel() → readl_relaxed_poll_timeout()
 *     → dw_pcie_link_up() → visconti_mpu_writel()
 */
static int visconti_pcie_start_link(struct dw_pcie *pci)
{
	struct visconti_pcie *pcie = dev_get_drvdata(pci->dev);
	void __iomem *addr = pcie->ulreg_base;
	/* [한국어] 폴링이 읽어 담을 값. */
	u32 val;
	/* [한국어] 폴링 결과. */
	int ret;

	visconti_ulreg_writel(pcie, PCIE_UL_APP_LTSSM_ENABLE,
			      /* [한국어] LTSSM 을 켠다. 이 비트를 세우기 전에는 하드웨어가 링크 훈련을 시작하지 않는다. */
			      PCIE_UL_REG_V_SII_GEN_CTRL_01);

	ret = readl_relaxed_poll_timeout(addr + PCIE_UL_REG_V_PHY_ST_02,
					 /* [한국어] L0 이 될 때까지 기다린다 — 90ms 간격으로 100ms 까지이므로,
					  * 사실상 한 번 자고 한 번 더 확인하는 셈이다. */
					 val, (val & PCIE_UL_S_L0),
					 90000, 100000);
	if (ret)
		/* [한국어] 링크 훈련이 시간 안에 끝나지 않으면 그대로 물러난다. mpu 는 닫힌 채로 남는다. */
		return ret;

	visconti_ulreg_writel(pcie, PCIE_UL_S_INT_EVENT_MASK1_ALL,
			      /* [한국어] SoC 레벨 인터럽트를 모두 허용한다 — PME, 링크 EQ 요청, eDMA 넷. */
			      PCIE_UL_REG_S_INT_EVENT_MASK1);

	if (dw_pcie_link_up(pci)) {
		/* [한국어] **링크가 실제로 섰을 때만** mpu 를 연다. 위에서 이미 L0 을 기다렸는데도
		 * 코어의 판정을 한 번 더 거치는데, 그쪽은 DWC 표준 레지스터를 보므로
		 * ulreg 의 판정과 근거가 다르다. */
		val = visconti_mpu_readl(pcie, PCIE_MPU_REG_MP_EN);
		/* [한국어] 비활성화 비트를 지워 PCIe 쪽 접근을 통과시킨다. */
		visconti_mpu_writel(pcie, val & ~MPU_MP_EN_DISABLE,
				    /* [한국어] 읽기-수정-쓰기라 이 레지스터의 다른 비트가 보존된다. */
				    PCIE_MPU_REG_MP_EN);
	}

	return 0;
}

/* [한국어]
 * visconti_pcie_stop_link - LTSSM 을 끄고 mpu 를 닫는다
 *
 * @pci: DWC 코어의 문맥.
 *
 * visconti_pcie_start_link() 의 짝이다.
 *
 * 순서가 시작 쪽과 반대다 — LTSSM 을 먼저 끄고 mpu 를 나중에 닫는다.
 * 시작할 때 mpu 를 마지막에 열었으므로, 닫는 것도 마지막이 대칭이다.
 *
 * 두 동작 모두 읽기-수정-쓰기다. LTSSM 비트와 mpu 비활성화 비트가 각각
 * 다른 비트와 레지스터를 공유하므로 통째로 쓸 수 없다.
 *
 * 반환값이 없다. 링크를 끊는 것은 실패해도 되돌릴 수 없다.
 *
 * 실행 컨텍스트: DWC 코어의 정리 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.stop_link == [이 함수]
 *     → visconti_ulreg_readl() → visconti_ulreg_writel()
 *     → visconti_mpu_readl() → visconti_mpu_writel()
 */
static void visconti_pcie_stop_link(struct dw_pcie *pci)
{
	struct visconti_pcie *pcie = dev_get_drvdata(pci->dev);
	u32 val;

	val = visconti_ulreg_readl(pcie, PCIE_UL_REG_V_SII_GEN_CTRL_01);
	/* [한국어] LTSSM 을 끈다. 시작 쪽과 달리 읽기-수정-쓰기인 것은,
	 * 시작할 때는 이 레지스터에 다른 비트가 없다고 보고 통째로 썼기 때문이다. */
	val &= ~PCIE_UL_APP_LTSSM_ENABLE;
	/* [한국어] 되쓴다. */
	visconti_ulreg_writel(pcie, val, PCIE_UL_REG_V_SII_GEN_CTRL_01);

	val = visconti_mpu_readl(pcie, PCIE_MPU_REG_MP_EN);
	/* [한국어] mpu 를 닫는다. 시작할 때 마지막에 열었으므로 닫는 것도 마지막이 대칭이다. */
	visconti_mpu_writel(pcie, val | MPU_MP_EN_DISABLE, PCIE_MPU_REG_MP_EN);
/* [한국어] 링크 중단 끝. 반환값이 없어 실패를 알릴 방법이 없다. */
}

/*
 * In this SoC specification, the CPU bus outputs the offset value from
 * 0x40000000 to the PCIe bus, so 0x40000000 is subtracted from the CPU
 * bus address. This 0x40000000 is also based on io_base from DT.
 */
/* [한국어]
 * visconti_pcie_cpu_addr_fixup - CPU 주소를 PCIe 버스 주소로 바꾼다
 *
 * @pci: DWC 코어의 문맥.
 * @cpu_addr: CPU 쪽 주소.
 * @return: PCIe 버스 쪽 주소.
 *
 * 위 영어 주석이 근거를 밝힌다 — 이 SoC 는 CPU 버스 주소에서 0x40000000 을
 * 뺀 값을 PCIe 버스로 내보내고, 그 0x40000000 이 디바이스 트리의 io_base 와
 * 같다.
 *
 * 그래서 뺄셈 대신 **AND NOT** 을 쓴다. io_base 가 0x40000000 처럼 하나의
 * 비트만 선 값이라 그 비트를 지우는 것이 빼는 것과 결과가 같기 때문이다.
 * io_base 가 그런 형태가 아닌 트리에서는 두 연산의 결과가 달라진다.
 *
 * DWC 코어가 ATU(주소 변환 유닛)를 설정할 때 이 콜백을 불러, 창의 양쪽
 * 주소를 맞춘다. 이것이 없으면 CPU 가 쓴 주소가 그대로 PCIe 로 나가
 * 0x40000000 만큼 어긋난 자리를 가리킨다.
 *
 * 실행 컨텍스트: DWC 코어의 ATU 설정. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어의 ATU 설정 → dw_pcie_ops.cpu_addr_fixup == [이 함수]
 */
static u64 visconti_pcie_cpu_addr_fixup(struct dw_pcie *pci, u64 cpu_addr)
{
	struct dw_pcie_rp *pp = &pci->pp;

	return cpu_addr & ~pp->io_base;
/* [한국어] io_base 가 0x40000000 처럼 한 비트만 선 값이라 AND NOT 이 뺄셈과 같은 결과를 낸다.
 * 그런 형태가 아닌 트리에서는 두 연산이 갈린다. */
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 이 SoC 는 CPU 주소와 PCIe 주소가 달라 변환 콜백이 필요하다. */
	.cpu_addr_fixup = visconti_pcie_cpu_addr_fixup,
	/* [한국어] 링크 판정도 표준 레지스터가 아니라 ulreg 를 봐야 해 콜백으로 둔다.
	 * 이 표에 콜백이 넷이나 있는 것이 DWC 표준에서 그만큼 벗어나 있다는 뜻이다. */
	.link_up = visconti_pcie_link_up,
	.start_link = visconti_pcie_start_link,
	.stop_link = visconti_pcie_stop_link,
};

/* [한국어]
 * visconti_pcie_host_init - 클럭·리셋·PHY 를 정해진 순서로 초기화한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, -ETIMEDOUT.
 *
 * 이 파일에서 가장 긴 함수이며, 내용이 사실상 **순서** 하나다. 이 SoC 의
 * 전원·리셋 시퀀스가 데이터시트로 정해져 있어 그대로 따라야 하고, 어기면
 * 링크가 서지 않는다.
 *
 * 단계가 이렇다.
 * 1. smu 로 aux 클럭과 마스터 AXI 클럭의 게이트를 연다.
 * 2. 250ns 기다린다. 클럭이 안정될 시간이다.
 * 3. smu 로 ulreg 리셋을 푼다. 이제 ulreg 창에 접근할 수 있다.
 * 4. RC(루트 컴플렉스) 모드로 설정한다. 이 SoC 는 EP 로도 동작할 수 있어
 *    모드를 명시해야 한다.
 * 5. PERST# 제어를 초기값으로 두고 100µs 기다린다.
 * 6. PERST# 출력 비트를 세우고 다시 100µs 기다린다. 이 두 단계가 하위
 *    장치에 리셋을 걸었다 푸는 동작이다.
 * 7. smu 로 전원 상승 리셋을 푼다.
 * 8. PHY 의 SRAM 초기화가 끝나기를 기다린다. PHY 안의 마이크로코드가
 *    올라오는 시간이며, 이것을 기다리지 않으면 PHY 가 동작하지 않는다.
 * 9. SRAM 외부 로드 완료를 표시한다 — 커널이 따로 로드할 것이 없다는 뜻이다.
 * 10. 코어 리셋이 풀렸는지 확인한다.
 *
 * 지연 값이 두 종류인 것에 주의할 만하다. ndelay(250)과 udelay(100)은
 * 바쁜 대기이고, 두 폴링은 100µs 간격으로 1ms 까지 기다린다. 앞의 둘은
 * 확인할 상태 비트가 없어 정해진 시간만큼 기다리는 수밖에 없다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 바쁜 대기와 폴링이 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 두 폴링 중 하나라도 시간이 다하면 -ETIMEDOUT 을 올려보내고,
 * DWC 코어가 그것을 probe 실패로 전한다. 되감기는 없다 — 이미 푼 리셋을
 * 다시 걸지 않는다.
 *
 * 호출 체인:
 *   visconti_add_pcie_port() → dw_pcie_host_init()
 *     → dw_pcie_host_ops.init == [이 함수]
 *     → visconti_smu_writel() → visconti_ulreg_writel()
 *     → readl_relaxed_poll_timeout()
 */
static int visconti_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct visconti_pcie *pcie = dev_get_drvdata(pci->dev);
	/* [한국어] 폴링에 넘길 주소. */
	void __iomem *addr;
	/* [한국어] 폴링 결과. */
	int err;
	/* [한국어] 폴링이 읽어 담을 값. */
	u32 val;

	visconti_smu_writel(pcie,
			    PISMU_CKON_PCIE_AUX_CLK | PISMU_CKON_PCIE_MSTR_ACLK,
			    PISMU_CKON_PCIE);
	ndelay(250);

	visconti_smu_writel(pcie, PISMU_RSOFF_PCIE_ULREG_RST_N,
			    /* [한국어] ulreg 리셋을 푼다. 이 뒤에야 ulreg 창에 접근할 수 있다. */
			    PISMU_RSOFF_PCIE);
	visconti_ulreg_writel(pcie, PCIE_UL_REG_S_PCIE_MODE_RC,
			      /* [한국어] RC(루트 컴플렉스) 모드로 설정한다. 이 SoC 는 EP 로도 동작할 수 있어
			       * 모드를 명시해야 한다. */
			      PCIE_UL_REG_S_PCIE_MODE);

	val = PCIE_UL_REG_S_PERSTN_CTRL_INIT;
	/* [한국어] PERST# 제어를 초기값으로 둔다 — 이 시점에는 출력 비트가 꺼져 있어
	 * 하위 장치에 리셋이 걸린 상태다. */
	visconti_ulreg_writel(pcie, val, PCIE_UL_REG_S_PERSTN_CTRL);
	/* [한국어] 100µs 기다린다. 확인할 상태 비트가 없어 정해진 시간만큼 기다리는 수밖에 없다. */
	udelay(100);

	val |= PCIE_UL_PERSTN_OUT;
	/* [한국어] 출력 비트를 세워 하위 장치의 리셋을 푼다. */
	visconti_ulreg_writel(pcie, val, PCIE_UL_REG_S_PERSTN_CTRL);
	/* [한국어] 다시 100µs 기다린다. 하위 장치가 리셋에서 깨어날 시간이다. */
	udelay(100);

	visconti_smu_writel(pcie, PISMU_RSOFF_PCIE_PWR_UP_RST_N,
			    /* [한국어] 전원 상승 리셋을 푼다. */
			    PISMU_RSOFF_PCIE);

	addr = pcie->ulreg_base + PCIE_UL_REG_S_PHY_INIT_03;
	/* [한국어] PHY 의 SRAM 초기화가 끝나기를 기다린다. */
	err = readl_relaxed_poll_timeout(addr, val,
					 /* [한국어] 완료 비트가 설 때까지 — PHY 안의 마이크로코드가 올라오는 시간이며,
					  * 이것을 기다리지 않으면 PHY 가 동작하지 않는다. */
					 (val & PCIE_UL_PHY0_SRAM_INIT_DONE),
					 100, 1000);
	if (err)
		/* [한국어] 시간이 다하면 그대로 물러난다. 되감기는 없다 — 이미 푼 리셋을 다시 걸지 않는다. */
		return err;

	visconti_ulreg_writel(pcie, PCIE_UL_PHY0_SRAM_EXT_LD_DONE,
			      /* [한국어] SRAM 외부 로드 완료를 표시한다 — 커널이 따로 로드할 것이 없다는 뜻이다. */
			      PCIE_UL_REG_S_PHY_INIT_02);

	addr = pcie->ulreg_base + PCIE_UL_REG_S_SIG_MON;
	/* [한국어] 마지막으로 코어 리셋이 풀렸는지 확인한다. */
	return readl_relaxed_poll_timeout(addr, val,
					  /* [한국어] 감시 비트가 설 때까지 100µs 간격으로 1ms 까지 기다린다. */
					  (val & PCIE_UL_CORE_RST_N_MON), 100,
					  1000);
}

static const struct dw_pcie_host_ops visconti_pcie_host_ops = {
	/* [한국어] 이 SoC 의 초기화 순서를 코어에 맡길 수 없어 콜백으로 둔다. */
	.init = visconti_pcie_host_init,
};

/* [한국어]
 * visconti_get_resources - 레지스터 창 셋과 클럭 셋을 얻는다
 *
 * @pdev: 플랫폼 장치.
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 디바이스 트리에서 이 드라이버가 쓸 자원을 모두 확보한다.
 *
 * 창 셋을 이름으로 찾는다 — ulreg, smu, mpu. DWC 코어가 따로 얻는 dbi 와
 * config 창은 여기 없다.
 *
 * [상류 코드 관찰] 클럭 셋을 devm_clk_get() 으로 얻기만 하고 이 파일 어디에서도
 * prepare/enable 하지 않는다. 전수 확인 결과 refclk·coreclk·auxclk 를 읽는
 * 코드가 없으며, 원본(1f0e418bb6)에서도 같다. 클럭 게이트는 위
 * visconti_pcie_host_init() 이 smu 레지스터로 직접 여는데, 그 둘이 같은
 * 클럭을 가리키는지는 이 트리에서 확인 못 함. 코드는 고치지 않았다.
 *
 * 창 셋과 클럭 셋의 오류 처리가 다르다. 창은 오류 코드만 올려보내고
 * (devm_platform_ioremap_resource_byname 이 이미 기록을 남긴다), 클럭은
 * dev_err_probe 로 어느 클럭이었는지 남긴다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 오류를 그대로 올려보낸다. 되감기는 devres 가 맡는다.
 *
 * 호출 체인:
 *   visconti_pcie_probe() → [이 함수]
 *     → devm_platform_ioremap_resource_byname() → devm_clk_get()
 */
static int visconti_get_resources(struct platform_device *pdev,
				  struct visconti_pcie *pcie)
{
	struct device *dev = &pdev->dev;

	pcie->ulreg_base = devm_platform_ioremap_resource_byname(pdev, "ulreg");
	/* [한국어] ulreg 창 매핑이 실패하면, */
	if (IS_ERR(pcie->ulreg_base))
		/* [한국어] 그 오류를 올려보낸다. 기록을 남기지 않는 것은 아래 헬퍼가 이미 남기기 때문이다. */
		return PTR_ERR(pcie->ulreg_base);

	pcie->smu_base = devm_platform_ioremap_resource_byname(pdev, "smu");
	/* [한국어] smu 창 매핑이 실패하면, */
	if (IS_ERR(pcie->smu_base))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pcie->smu_base);

	pcie->mpu_base = devm_platform_ioremap_resource_byname(pdev, "mpu");
	/* [한국어] mpu 창 매핑이 실패하면, */
	if (IS_ERR(pcie->mpu_base))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pcie->mpu_base);

	pcie->refclk = devm_clk_get(dev, "ref");
	/* [한국어] 참조 클럭을 얻지 못하면, */
	if (IS_ERR(pcie->refclk))
		/* [한국어] 어느 클럭이었는지와 함께 */
		return dev_err_probe(dev, PTR_ERR(pcie->refclk),
				     /* [한국어] 올려보낸다. 창 쪽과 달리 여기서는 기록을 남기는데, 상류 코드가 그렇게 되어 있다. */
				     "Failed to get ref clock\n");

	pcie->coreclk = devm_clk_get(dev, "core");
	/* [한국어] 코어 클럭을 얻지 못하면, */
	if (IS_ERR(pcie->coreclk))
		/* [한국어] 어느 클럭이었는지와 함께 */
		return dev_err_probe(dev, PTR_ERR(pcie->coreclk),
				     /* [한국어] 올려보낸다. */
				     "Failed to get core clock\n");

	pcie->auxclk = devm_clk_get(dev, "aux");
	/* [한국어] 보조 클럭을 얻지 못하면, */
	if (IS_ERR(pcie->auxclk))
		/* [한국어] 어느 클럭이었는지와 함께 */
		return dev_err_probe(dev, PTR_ERR(pcie->auxclk),
				     /* [한국어] 올려보낸다. */
				     "Failed to get aux clock\n");

	return 0;
}

/* [한국어]
 * visconti_add_pcie_port - 인터럽트와 콜백 표를 걸고 DWC 코어에 넘긴다
 *
 * @pcie: 드라이버 상태.
 * @pdev: 플랫폼 장치.
 * @return: dw_pcie_host_init() 의 결과.
 *
 * 이 드라이버가 DWC 코어에 제어를 넘기는 지점이다.
 *
 * 인터럽트 번호를 pp->irq 에 담는 것이 요점이다. DWC 코어가 그 번호로
 * 자기 MSI 핸들러를 걸며, 이 파일은 인터럽트를 직접 다루지 않는다.
 * 이름이 "intr" 인 것은 디바이스 트리의 interrupt-names 가 정한다.
 *
 * 콜백 표를 건 뒤 코어를 부르면, 그 안에서 이 파일의 host_init 이 불리고
 * 이어서 start_link 와 cpu_addr_fixup 이 불린다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 링크 대기와 버스 스캔으로
 * 오래 걸린다.
 *
 * 에러 경로: 인터럽트를 못 얻으면 그 오류를, 그 밖은 코어의 오류를 올려보낸다.
 *
 * 호출 체인:
 *   visconti_pcie_probe() → [이 함수]
 *     → platform_get_irq_byname() → dw_pcie_host_init()
 */
static int visconti_add_pcie_port(struct visconti_pcie *pcie,
				  struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;

	pp->irq = platform_get_irq_byname(pdev, "intr");
	/* [한국어] 인터럽트 번호를 얻지 못하면, */
	if (pp->irq < 0)
		/* [한국어] 그 오류를 올려보낸다. 이 번호로 DWC 코어가 자기 MSI 핸들러를 건다. */
		return pp->irq;

	pp->ops = &visconti_pcie_host_ops;
/* [한국어] 콜백 표를 건다. 아래 코어 호출 도중에 이 표의 init 이 불린다. */

	return dw_pcie_host_init(pp);
}

/* [한국어]
 * visconti_pcie_probe - 자원을 얻고 DWC 호스트를 초기화한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * dw_pcie 를 채우는 세 줄이 이 함수의 핵심이다 — dev 와 ops 를 넣는데,
 * 그 ops 가 이 파일의 코어 콜백 넷(cpu_addr_fixup, link_up, start_link,
 * stop_link)을 담은 표다. 그 넷이 DWC 코어가 이 SoC 에 대해 물어보는
 * 질문의 전부다.
 *
 * drvdata 를 자원 확보 **뒤** 에 매다는 순서에 주의할 만하다. 이 파일의
 * 콜백들이 drvdata 로 상태를 되찾는데, 그 콜백이 처음 불리는 것은 아래
 * dw_pcie_host_init() 안이므로 그 전에만 매달면 된다.
 *
 * 되감기 코드가 없다. 잡는 자원이 모두 devm 판이라 실패하면 코어가
 * 자동으로 되돌린다.
 *
 * builtin_platform_driver 로 등록되어 모듈로 뺄 수 없고, 드라이버 구조체의
 * suppress_bind_attrs 가 sysfs 언바인드도 막는다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 오류를 그대로 올려보내며, 되감기는 devres 가 맡는다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → visconti_get_resources() → visconti_add_pcie_port()
 */
static int visconti_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct visconti_pcie *pcie;
	/* [한국어] DWC 코어가 다룰 부분을 가리킬 포인터. */
	struct dw_pcie *pci;
	/* [한국어] 각 단계의 결과. */
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 상태 구조를 잡지 못하면, */
	if (!pcie)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	pci = &pcie->pci;
	/* [한국어] DWC 코어가 이 값으로 디바이스 트리와 로그를 다룬다. */
	pci->dev = dev;
	/* [한국어] 코어 콜백 표를 건다 — cpu_addr_fixup, link_up, start_link, stop_link 넷이다. */
	pci->ops = &dw_pcie_ops;

	ret = visconti_get_resources(pdev, pcie);
	/* [한국어] 자원 확보가 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 올려보낸다. 되감기가 없는 것은 잡는 자원이 모두 devm 판이기 때문이다. */
		return ret;

	platform_set_drvdata(pdev, pcie);
/* [한국어] 이 파일의 콜백들이 drvdata 로 상태를 되찾으므로, 콜백이 처음 불리는
 * 아래 호출보다 먼저 매달아야 한다. */

	return visconti_add_pcie_port(pcie, pdev);
/* [한국어] 이 뒤로는 DWC 코어가 host_init, start_link, cpu_addr_fixup 을 차례로 부른다. */
}

static const struct of_device_id visconti_pcie_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다. */
	{ .compatible = "toshiba,visconti-pcie" },
	/* [한국어] 표의 끝 표시. */
	{},
};

static struct platform_driver visconti_pcie_driver = {
	/* [한국어] remove 콜백이 없다 — 아래 suppress_bind_attrs 와 builtin 등록이
	 * 이 드라이버를 뗄 수 없게 만들기 때문이다. */
	.probe = visconti_pcie_probe,
	/* [한국어] 드라이버 정보. */
	.driver = {
		.name = "visconti-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = visconti_pcie_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(visconti_pcie_driver);
