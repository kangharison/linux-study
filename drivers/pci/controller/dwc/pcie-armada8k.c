// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Marvell Armada-8K SoCs
 *
 * Armada-8K PCIe Glue Layer Source Code
 *
 * Copyright (C) 2016 Marvell Technology Group Ltd.
 *
 * Author: Yehuda Yitshak <yehuday@marvell.com>
 * Author: Shadi Ammouri <shadi@marvell.com>
 */

/*
 * [한국어 설명] Marvell Armada-8K SoC 의 DesignWare PCIe 호스트 글루 (pcie-armada8k.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 Marvell Armada-8K 계열 SoC 에 붙이는 글루
 * 드라이버다(옆의 상류 주석이 "Glue Layer" 라고 부른다). config 접근,
 * ATU 설정, MSI, 버스 스캔은 모두 pcie-designware-host.c 가 하고, 이 파일은
 * DWC 코어가 알 수 없는 SoC 고유의 것만 맡는다.
 *
 * 이 SoC 의 특징은 **벤더 전용 레지스터가 DBI 창 안에 있다는 것** 이다.
 * 다른 글루 드라이버들이 별도의 레지스터 창을 매핑하는 것과 달리, 여기서는
 * DBI 창의 0x8000 오프셋부터가 Marvell 이 정의한 영역이라 dw_pcie_readl_dbi()
 * 로 그대로 읽고 쓴다. 그래서 이 파일에는 창 매핑이 하나뿐이다.
 *
 * 맡는 일이 넷이다.
 *   1) PHY 레인 관리 — 최대 4레인을 각각 초기화하고 켠다. 레인 수가 보드마다
 *      달라 없는 레인은 NULL 로 두고 건너뛴다.
 *   2) 모드 설정과 링크 시작 — 벤더 레지스터로 RC 모드를 지정하고 LTSSM 을 켠다.
 *   3) AXI 속성 설정 — 이 컨트롤러가 시스템 메모리로 내는 DMA 의 캐시 속성과
 *      공유 도메인을 정한다. 이것이 없으면 캐시 일관성이 깨진다.
 *   4) INTx 인터럽트 처리 — 다만 실제로 하는 일은 래치된 원인 비트를 지우는
 *      것뿐이다(해당 자리의 상류 주석 참조).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> armada8k_pcie_probe()
 *     -> 클럭 둘과 DBI 창을 얻는다
 *     -> armada8k_pcie_setup_phys() -> 레인마다 PHY 를 얻어 켠다
 *     -> armada8k_add_pcie_port()
 *        -> 인터럽트를 걸고 dw_pcie_host_init()  [pcie-designware-host.c]
 *           -> 그 안에서 콜백 -> [이 파일] armada8k_pcie_host_init()
 *              -> LTSSM 을 끄고, RC 모드를 지정하고, AXI 속성을 세우고,
 *                 INTx 를 허용한다
 *           -> 코어가 start_link 콜백을 부른다 -> [이 파일] LTSSM 켜기
 *           -> 링크 훈련, ATU 설정, 버스 스캔
 *
 * 실행 컨텍스트: probe 와 host_init 은 프로세스 컨텍스트,
 * armada8k_pcie_irq_handler() 는 인터럽트 문맥이다. 잠금이 하나도 없는데,
 * 그 핸들러가 읽고 쓰는 레지스터를 다른 경로가 건드리지 않기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c 와 pcie-designware.c. 접점은 두 벌의 콜백
 *   표다 — dw_pcie_ops(link_up, start_link)와 dw_pcie_host_ops(init 하나).
 *   벤더 레지스터 접근도 DWC 의 dw_pcie_readl_dbi()/writel_dbi() 를 그대로 쓴다.
 * 옆쪽: PHY 계층(drivers/phy)과 클럭 계층. 둘 다 이 트리에 없어 그쪽 내부는
 *   확인 대상 밖이며, 이 파일에서 읽히는 호출 규약까지만 적었다.
 *
 * 데이터 흐름:
 *   디바이스 트리("ctrl" 자원, 클럭 둘, PHY 최대 4개, 인터럽트) -> probe
 *     -> struct armada8k_pcie
 *   벤더 레지스터: DBI 창 + PCIE_VENDOR_REGS_OFFSET(0x8000) + 각 오프셋
 *   AXI 속성: 상수 두 개와 공유 도메인 값이 컨트롤러의 DMA 동작을 정한다.
 *
 * 공유 상태: struct armada8k_pcie 하나. probe 후 불변이며 잠금이 없다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러가 붙으면, 그 컨트롤러가 내는 DMA 가 이
 * 파일이 설정한 AXI 속성을 그대로 따른다. ARCACHE/AWCACHE 를 write-back
 * 할당으로 두고 공유 도메인을 outer shareable 로 두는 것이 그 설정이며,
 * 그래야 NVMe 가 쓴 완료 큐 항목을 CPU 가 캐시 무효화 없이 볼 수 있다.
 * 이 설정이 틀리면 드라이버가 완료를 놓치거나 낡은 데이터를 읽는다.
 *
 * === 주요 함수/구조체 요약 ===
 * armada8k_pcie_host_init()   : DWC 코어가 부르는 콜백. RC 모드·AXI 속성·
 *                               INTx 허용이 여기 모여 있다.
 * armada8k_pcie_setup_phys()  : 레인마다 PHY 를 얻고 켠다. 없는 레인은 건너뛴다.
 * armada8k_pcie_enable_phys() : 그 켜기의 실제 부분.
 * armada8k_pcie_start_link()  : LTSSM 비트 하나를 세운다.
 * armada8k_pcie_link_up()     : 벤더 상태 레지스터의 두 비트를 함께 본다.
 * struct armada8k_pcie        : dw_pcie 를 **포인터로** 들고 있는 상태 구조체.
 *                               다른 글루가 값으로 품는 것과 다르며, 그래서
 *                               to_armada8k_pcie() 가 drvdata 를 거친다.
 */

/* [한국어] clk_prepare_enable()/clk_disable_unprepare(). 클럭 둘을 켜고 끈다. */
#include <linux/clk.h>
/* [한국어] 지연 헬퍼. 이 파일에서 직접 쓰는 곳은 없다(전수 확인). */
#include <linux/delay.h>
/* [한국어] devm_request_irq() 와 IRQF_SHARED, irqreturn_t. */
#include <linux/interrupt.h>
/* [한국어] 기본 커널 관용구. */
#include <linux/kernel.h>
/* [한국어] __init 표시. */
#include <linux/init.h>
/* [한국어] of_node 접근. PHY 를 자식 인덱스로 찾는 데 쓴다. */
#include <linux/of.h>
/* [한국어] PCI 코어 타입들. */
#include <linux/pci.h>
/* [한국어] phy_init()/phy_power_on()/phy_set_mode_ext(). 레인마다 이것을 부른다. */
#include <linux/phy/phy.h>
/* [한국어] struct platform_device 와 platform_get_irq(). */
#include <linux/platform_device.h>
/* [한국어] struct resource. */
#include <linux/resource.h>
/* [한국어] of_pci 헬퍼. 이 파일에서 직접 쓰는 곳은 없다(전수 확인). */
#include <linux/of_pci.h>

/* [한국어] DWC 코어의 선언 전부 — struct dw_pcie, dw_pcie_readl_dbi(), PCIE_LNK_X4 등. */
#include "pcie-designware.h"

/* [한국어] 이 컨트롤러가 가질 수 있는 최대 레인 수. DWC 코어가 정의한 x4 상수를
 * 그대로 쓴다 — 이 SoC 의 상한이 4레인이라는 뜻이다. */
#define ARMADA8K_PCIE_MAX_LANES PCIE_LNK_X4

/* [한국어] 이 드라이버의 상태 전부. */
struct armada8k_pcie {
	/* [한국어] DWC 코어가 다루는 부분. **포인터로 든다** — 값으로 품는 다른 글루들과
	 * 달라, 이 구조체 포인터가 dw_pcie 포인터가 되지 않는다.
	 * 설정자: probe 가 따로 할당해 매단다.
	 * 읽는 자: 이 파일의 모든 함수.
	 * 값 범위: 유효한 dw_pcie 포인터.
	 * 동기화: probe 후 불변. */
	struct dw_pcie *pci;
	/* [한국어] 필수 코어 클럭.
	 * 설정자: probe 의 devm_clk_get(dev, NULL).
	 * 읽는 자: probe 가 켜고 되감기 경로가 끈다.
	 * 값 범위: 유효한 clk 포인터. 얻지 못하면 probe 가 실패한다.
	 * 동기화: probe 후 불변. */
	struct clk *clk;
	/* [한국어] 선택 사항인 레지스터 클럭.
	 * 설정자: probe 의 devm_clk_get(dev, "reg").
	 * 읽는 자: probe 가 켜고, 되감기 경로가 끈다.
	 * 값 범위: 유효한 clk 포인터, 또는 이 클럭이 없는 보드에서는 오류 포인터.
	 * **오류 포인터가 그대로 남을 수 있다** — probe 는 -EPROBE_DEFER 만 실패로 다룬다.
	 * 동기화: probe 후 불변. */
	struct clk *clk_reg;
	/* [한국어] 레인별 PHY. 최대 4개다.
	 * 설정자: armada8k_pcie_setup_phys() 가 인덱스로 얻는다. 없는 레인은 NULL.
	 * 읽는 자: enable_phys()/disable_phys().
	 * 값 범위: 유효한 PHY 포인터 또는 NULL. **NULL 이 섞여 있다는 것이 전제다.**
	 * 동기화: probe 후 불변. */
	struct phy *phy[ARMADA8K_PCIE_MAX_LANES];
	/* [한국어] 실제로 찾은 PHY 개수.
	 * 설정자: armada8k_pcie_setup_phys().
	 * 읽는 자: 하나도 없는지 판단하는 데 쓰고, phy_set_mode_ext() 에 링크 폭으로 넘긴다.
	 * 값 범위: 0~4. 0 이면 경고만 남기고 진행한다.
	 * 동기화: probe 후 불변. */
	unsigned int phy_count;
/* [한국어] 이 드라이버의 상태 전부. **dw_pcie 를 포인터로 든다** —
 * 값으로 품는 다른 글루들과 다르며, 그래서 변환 매크로가 drvdata 를 거친다. */
};

/* [한국어] 벤더 전용 레지스터 영역의 시작 오프셋. **DBI 창 안에 있다** —
 * 별도의 창을 매핑하지 않고 dw_pcie_readl_dbi() 로 그대로 접근한다. */
#define PCIE_VENDOR_REGS_OFFSET		0x8000

/* [한국어] 전역 제어 레지스터. LTSSM 비트와 장치 종류 필드가 함께 들어 있어,
 * 이 파일이 언제나 읽기-수정-쓰기로 다룬다. */
#define PCIE_GLOBAL_CONTROL_REG		(PCIE_VENDOR_REGS_OFFSET + 0x0)
/* [한국어] LTSSM 시작 비트. start_link 가 세우고 host_init 이 필요할 때 지운다. */
#define PCIE_APP_LTSSM_EN		BIT(2)
/* [한국어] 장치 종류 필드의 시작 비트 위치. */
#define PCIE_DEVICE_TYPE_SHIFT		4
/* [한국어] 그 필드의 폭(4비트). */
#define PCIE_DEVICE_TYPE_MASK		0xF
/* [한국어] 루트 컴플렉스 값(옆의 상류 주석). host_init 이 이 값을 넣는다. */
#define PCIE_DEVICE_TYPE_RC		0x4 /* Root complex */

/* [한국어] 전역 상태 레지스터. 링크 여부를 여기서 읽는다. */
#define PCIE_GLOBAL_STATUS_REG		(PCIE_VENDOR_REGS_OFFSET + 0x8)
/* [한국어] 데이터 링크 계층의 링크 확립 비트. */
#define PCIE_GLB_STS_RDLH_LINK_UP	BIT(1)
/* [한국어] PHY 계층의 링크 확립 비트. **두 비트를 모두** 봐야 config 접근이 가능하다. */
#define PCIE_GLB_STS_PHY_LINK_UP	BIT(9)

/* [한국어] 인터럽트 원인 레지스터. 핸들러가 읽어 그대로 되써 지운다. */
#define PCIE_GLOBAL_INT_CAUSE1_REG	(PCIE_VENDOR_REGS_OFFSET + 0x1C)
/* [한국어] 인터럽트 마스크 레지스터. host_init 이 INTx 넷을 허용한다. */
#define PCIE_GLOBAL_INT_MASK1_REG	(PCIE_VENDOR_REGS_OFFSET + 0x20)
/* [한국어] INTA 어서션. */
#define PCIE_INT_A_ASSERT_MASK		BIT(9)
/* [한국어] INTB. */
#define PCIE_INT_B_ASSERT_MASK		BIT(10)
/* [한국어] INTC. */
#define PCIE_INT_C_ASSERT_MASK		BIT(11)
/* [한국어] INTD. 네 비트가 연속이라 한 번에 세울 수 있다. */
#define PCIE_INT_D_ASSERT_MASK		BIT(12)

/* [한국어] 읽기 트랜잭션의 AXI 캐시 속성 레지스터. */
#define PCIE_ARCACHE_TRC_REG		(PCIE_VENDOR_REGS_OFFSET + 0x50)
/* [한국어] 쓰기 트랜잭션의 AXI 캐시 속성 레지스터. */
#define PCIE_AWCACHE_TRC_REG		(PCIE_VENDOR_REGS_OFFSET + 0x54)
/* [한국어] 읽기 트랜잭션의 AXI user 신호 레지스터. 공유 도메인이 여기 들어간다. */
#define PCIE_ARUSER_REG			(PCIE_VENDOR_REGS_OFFSET + 0x5C)
/* [한국어] 쓰기 트랜잭션의 AXI user 신호 레지스터. */
#define PCIE_AWUSER_REG			(PCIE_VENDOR_REGS_OFFSET + 0x60)
/*
 * AR/AW Cache defaults: Normal memory, Write-Back, Read / Write
 * allocate
 */
/* [한국어] 읽기 캐시 속성의 기본값(위 상류 주석이 그 뜻을 밝힌다). */
#define ARCACHE_DEFAULT_VALUE		0x3511
/* [한국어] 쓰기 캐시 속성의 기본값. 읽기와 값이 다른 것은 AxCACHE 인코딩에서
 * 읽기 할당과 쓰기 할당 비트의 자리가 다르기 때문이다. */
#define AWCACHE_DEFAULT_VALUE		0x5311

/* [한국어] 공유 도메인 값 — outer shareable. CPU 캐시와 일관성을 유지하는 설정이다. */
#define DOMAIN_OUTER_SHAREABLE		0x2
/* [한국어] 그 필드의 폭(2비트). */
#define AX_USER_DOMAIN_MASK		0x3
/* [한국어] 그 필드의 시작 비트 위치. */
#define AX_USER_DOMAIN_SHIFT		4

/* [한국어] dw_pcie 포인터에서 이 드라이버의 상태를 되찾는 통로.
 * container_of 가 아니라 drvdata 인 것은, 이 드라이버가 dw_pcie 를
 * **포인터로** 들고 있어 두 구조체가 한 덩어리가 아니기 때문이다.
 * 전제: probe 가 platform_set_drvdata 로 상태를 미리 심어 두어야 한다. */
#define to_armada8k_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어]
 * armada8k_pcie_disable_phys - 모든 레인의 PHY 를 끈다
 *
 * @pcie: 드라이버 상태.
 *
 * armada8k_pcie_enable_phys() 의 짝이며, 레인마다 전원을 끄고 초기화를 되돌린다.
 *
 * **NULL 검사를 하지 않는다.** setup_phys 가 없는 레인을 NULL 로 두므로 이
 * 배열에는 NULL 이 섞여 있는데, phy_power_off()/phy_exit() 가 NULL 서술자를
 * 어떻게 다루는지는 이 트리에 drivers/phy 가 없어 확인 못 함. 선택적 PHY 를
 * NULL 로 두는 관용 자체는 흔하다.
 *
 * 순서가 켜는 쪽의 역순이다 — 전원을 먼저 끄고 초기화를 되돌린다.
 *
 * 이 함수를 부르는 곳이 probe 의 disable_phy 라벨 하나뿐이다(전수 확인).
 * 즉 PHY 준비가 **성공한 뒤** 그 다음 단계가 실패했을 때만 불린다.
 *
 * 실행 컨텍스트: probe 의 되감기. PHY 조작이 잠들 수 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   armada8k_pcie_probe() 의 disable_phy → [이 함수]
 *     → phy_power_off() → phy_exit()
 */
static void armada8k_pcie_disable_phys(struct armada8k_pcie *pcie)
{
	int i;

	for (i = 0; i < ARMADA8K_PCIE_MAX_LANES; i++) {
		/* [한국어] 전원을 먼저 끄고 아래에서 초기화를 되돌린다 — 켜는 쪽의 역순이다. */
		phy_power_off(pcie->phy[i]);
		phy_exit(pcie->phy[i]);
	}
}

/* [한국어]
 * armada8k_pcie_enable_phys - 모든 레인의 PHY 를 초기화하고 켠다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 레인마다 세 단계를 거친다 — 초기화, PCIe 모드 지정, 전원 켜기.
 *
 * 모드 지정에 **레인 수를 함께 넘기는 것** 이 요점이다. PHY 드라이버가 그
 * 값으로 이 링크가 몇 레인짜리인지 알고 그에 맞게 설정한다.
 *
 * [상류 코드 관찰] 되감기가 **실패한 레인에만** 적용된다. 예를 들어 레인 0~1
 * 을 완전히 켠 뒤 레인 2 의 전원 켜기가 실패하면, 레인 2 만 phy_exit 하고
 * 돌아간다 — 레인 0~1 은 켜진 채로 남는다. 호출자인 setup_phys 는 오류를
 * 그대로 올려보내고, probe 의 그 경로(fail_clkreg)는
 * armada8k_pcie_disable_phys() 를 부르지 않는다. 원본(1f0e418bb6)에서
 * 확인했으며, disable_phys 를 부르는 곳은 probe 의 disable_phy 라벨 하나뿐이다.
 * 코드는 고치지 않았다.
 *
 * phy_init 이 실패한 경우에는 그 레인조차 되돌리지 않는데, 초기화 자체가
 * 실패했으므로 되돌릴 것이 없다고 본 것이다.
 *
 * 실행 컨텍스트: probe. PHY 조작이 잠들 수 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 각 단계의 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   armada8k_pcie_setup_phys() → [이 함수]
 *     → phy_init() → phy_set_mode_ext() → phy_power_on()
 */
static int armada8k_pcie_enable_phys(struct armada8k_pcie *pcie)
{
	int ret;
	int i;
/* [한국어] 레인을 하나씩 켠다. */

	for (i = 0; i < ARMADA8K_PCIE_MAX_LANES; i++) {
		/* [한국어] 먼저 PHY 를 초기화한다. */
		ret = phy_init(pcie->phy[i]);
		/* [한국어] 실패하면 — */
		if (ret)
			/* [한국어] 되돌릴 것이 없으므로 그대로 물러난다. */
			return ret;

		ret = phy_set_mode_ext(pcie->phy[i], PHY_MODE_PCIE,
				       /* [한국어] **레인 수를 함께 넘긴다** — PHY 드라이버가 이 링크의 폭을 알아야 한다. */
				       pcie->phy_count);
		if (ret) {
			/* [한국어] 모드 지정이 실패하면 방금 초기화한 것만 되돌린다. */
			phy_exit(pcie->phy[i]);
			return ret;
		}

		ret = phy_power_on(pcie->phy[i]);
		/* [한국어] 전원 켜기가 실패하면, */
		if (ret) {
			/* [한국어] 역시 이 레인만 되돌린다 — 앞선 레인은 켜진 채로 남는다(위 함수 블록의 관찰 참조). */
			phy_exit(pcie->phy[i]);
			return ret;
		}
	}

	return 0;
}

/* [한국어]
 * armada8k_pcie_setup_phys - 디바이스 트리에서 레인별 PHY 를 얻어 켠다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 = 성공, 음수 오류.
 *
 * 최대 레인 수만큼 인덱스로 PHY 를 찾는다. 보드마다 실제 레인 수가 달라,
 * 없는 레인은 -ENODEV 로 돌아오고 그때 포인터를 NULL 로 두어 건너뛴다.
 *
 * -ENODEV 만 정상으로 다루는 것이 요점이다. 그 밖의 오류 — 예를 들어
 * -EPROBE_DEFER — 는 나중에 다시 시도하면 성공할 수 있으므로 실패로 올려보낸다.
 *
 * phy_count 를 세는 이유는 두 가지다. 하나라도 있는지 판단하는 데 쓰고,
 * armada8k_pcie_enable_phys() 가 PHY 드라이버에 링크 폭을 알리는 데도 쓴다.
 *
 * PHY 가 하나도 없어도 **경고만 하고 진행한다**(옆의 상류 주석). 옛 디바이스
 * 트리 바인딩에 PHY 항목이 없기 때문이며, 그 경우 PHY 가 펌웨어에 의해
 * 이미 설정돼 있다는 전제다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: PHY 조회의 -ENODEV 가 아닌 오류와 켜기 실패를 올려보낸다.
 * 켜기 실패는 어느 단계였는지 기록에 남긴다.
 *
 * 호출 체인:
 *   armada8k_pcie_probe() → [이 함수]
 *     → devm_of_phy_get_by_index() → armada8k_pcie_enable_phys()
 */
static int armada8k_pcie_setup_phys(struct armada8k_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = pci->dev;
	/* [한국어] PHY 를 찾을 디바이스 트리 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] 각 단계의 결과. */
	int ret = 0;
	/* [한국어] 레인 순회 인덱스. */
	int i;
/* [한국어] 레인마다 PHY 를 찾는다. */

	for (i = 0; i < ARMADA8K_PCIE_MAX_LANES; i++) {
		/* [한국어] 이름이 아니라 **인덱스로** 찾는다 — 레인 순서가 곧 인덱스다. */
		pcie->phy[i] = devm_of_phy_get_by_index(dev, node, i);
		/* [한국어] 찾지 못했으면, */
		if (IS_ERR(pcie->phy[i])) {
			/* [한국어] -ENODEV 가 아닌 오류는 — 예를 들어 -EPROBE_DEFER 는 — */
			if (PTR_ERR(pcie->phy[i]) != -ENODEV)
				/* [한국어] 실패로 올려보낸다. 나중에 다시 시도하면 성공할 수 있기 때문이다. */
				return PTR_ERR(pcie->phy[i]);
/* [한국어] -ENODEV 였다면 그 레인이 없는 것이므로, */

			pcie->phy[i] = NULL;
			/* [한국어] NULL 로 두고 다음 레인으로 넘어간다. */
			continue;
		}

		pcie->phy_count++;
	/* [한국어] 이 레인 처리 끝. */
	}

	/* Old bindings miss the PHY handle, so just warn if there is no PHY */
	if (!pcie->phy_count)
		dev_warn(dev, "No available PHY\n");
/* [한국어] PHY 가 하나도 없어도 진행한다(옆의 상류 주석) — 옛 바인딩에는
 * PHY 항목이 없어, 그 경우 펌웨어가 이미 설정해 두었다는 전제다. */

	ret = armada8k_pcie_enable_phys(pcie);
	/* [한국어] 켜기가 실패하면, */
	if (ret)
		/* [한국어] 어느 단계였는지 남긴다. 그 뒤 아래에서 그 오류가 나간다. */
		dev_err(dev, "Failed to initialize PHY(s) (%d)\n", ret);
/* [한국어] 성공했으면 0 이 나간다. */

	return ret;
}

/* [한국어]
 * armada8k_pcie_link_up - 링크가 서 있는지 답한다
 *
 * @pci: DWC 코어의 문맥.
 * @return: true = 링크가 섰다, false = 아니다.
 *
 * DWC 코어가 링크를 기다릴 때 반복해 부르는 콜백이다.
 *
 * **두 비트를 모두** 확인하는 것이 이 함수의 요점이다. PHY 계층의 링크와
 * 데이터 링크 계층(RDLH)의 링크가 따로 보고되는데, 둘 다 서야 config 접근이
 * 가능하다. PHY 만 서 있는 상태는 훈련이 끝나지 않은 중간 단계다.
 *
 * 실패했을 때 상태 레지스터 값을 통째로 찍는다. 어느 비트가 서지 않았는지를
 * 사람이 보고 판단할 수 있게 하려는 것이며, 디버그 수준이라 평소에는 나오지 않는다.
 *
 * 이 파일의 벤더 레지스터가 DBI 창 안에 있어 dw_pcie_readl_dbi() 를 그대로 쓴다.
 *
 * 실행 컨텍스트: DWC 코어의 링크 대기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.link_up == [이 함수] → dw_pcie_readl_dbi()
 */
static bool armada8k_pcie_link_up(struct dw_pcie *pci)
{
	u32 reg;
	u32 mask = PCIE_GLB_STS_RDLH_LINK_UP | PCIE_GLB_STS_PHY_LINK_UP;
/* [한국어] PHY 링크와 데이터 링크 두 비트를 함께 본다. */

	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_STATUS_REG);
/* [한국어] 둘 다 서 있으면 — */

	if ((reg & mask) == mask)
		/* [한국어] 링크가 완전히 섰다는 뜻이다. */
		return true;

	dev_dbg(pci->dev, "No link detected (Global-Status: 0x%08x).\n", reg);
	/* [한국어] 하나라도 빠졌으면 아직 훈련 중이거나 실패한 상태다. */
	return false;
}

/* [한국어]
 * armada8k_pcie_start_link - LTSSM 을 켜 링크 훈련을 시작시킨다
 *
 * @pci: DWC 코어의 문맥.
 * @return: 언제나 0.
 *
 * 벤더 전역 제어 레지스터의 LTSSM 비트 하나를 세운다.
 *
 * 읽기-수정-쓰기인 것이 필수다. 같은 레지스터에 장치 종류(RC/EP) 필드가
 * 있어, 통째로 쓰면 host_init 이 설정해 둔 모드가 지워진다.
 *
 * host_init 이 링크가 서 있지 않을 때 이 비트를 **끄는** 것과 짝을 이룬다 —
 * 설정하는 동안 링크 훈련이 돌면 안 되기 때문이다.
 *
 * 실행 컨텍스트: DWC 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 성공을 답한다.
 *
 * 호출 체인:
 *   DWC 코어 → dw_pcie_ops.start_link == [이 함수]
 *     → dw_pcie_readl_dbi() → dw_pcie_writel_dbi()
 */
static int armada8k_pcie_start_link(struct dw_pcie *pci)
{
	u32 reg;

	/* Start LTSSM */
	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_CONTROL_REG);
	reg |= PCIE_APP_LTSSM_EN;
	/* [한국어] LTSSM 비트를 세워 되쓴다. 읽기-수정-쓰기라 같은 레지스터의
	 * 장치 종류 필드가 보존된다. */
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_CONTROL_REG, reg);

	return 0;
}

/* [한국어]
 * armada8k_pcie_host_init - RC 모드와 AXI 속성을 세우고 INTx 를 허용한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 언제나 0.
 *
 * 이 파일이 DWC 코어와 만나는 유일한 콜백이며, SoC 고유의 설정이 모두 여기 있다.
 *
 * 네 단계다.
 * 1. 링크가 서 있지 않으면 LTSSM 을 끈다. 옆의 상류 주석이 이유를 밝힌다 —
 *    설정을 하려면 상태 기계가 멈춰 있어야 한다. 이미 링크가 서 있으면
 *    건드리지 않는데, 펌웨어가 세워 둔 링크를 끊지 않으려는 것이다.
 * 2. 장치 종류를 RC 로 지정한다. 마스크로 지운 뒤 값을 넣는 읽기-수정-쓰기다.
 * 3. **AXI 속성을 세운다.** 이 파일에서 가장 실질적인 부분으로, 두 갈래다.
 *    - AxCACHE: 이 컨트롤러가 내는 DMA 의 캐시 속성. 위 상수의 상류 주석이
 *      그 뜻을 밝힌다 — 일반 메모리, write-back, 읽기·쓰기 할당이다.
 *    - AxDOMAIN: 공유 도메인을 outer shareable 로 둔다. 그래야 CPU 캐시와
 *      일관성이 유지되어, 장치가 쓴 데이터를 CPU 가 무효화 없이 볼 수 있다.
 *    읽기 쪽(AR)과 쓰기 쪽(AW)을 각각 설정해야 한다.
 * 4. INTx 네 개를 허용한다.
 *
 * 세 번째가 틀리면 증상이 미묘하다 — 링크는 서고 config 도 읽히지만
 * DMA 데이터가 어긋난다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 성공을 답한다.
 *
 * 호출 체인:
 *   armada8k_add_pcie_port() → dw_pcie_host_init()
 *     → dw_pcie_host_ops.init == [이 함수]
 *     → dw_pcie_readl_dbi() → dw_pcie_writel_dbi()
 */
static int armada8k_pcie_host_init(struct dw_pcie_rp *pp)
{
	u32 reg;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
/* [한국어] 링크가 서 있지 않을 때만 LTSSM 을 끈다 — 펌웨어가 세워 둔 링크를
 * 끊지 않으려는 것이다. */

	if (!dw_pcie_link_up(pci)) {
		/* Disable LTSSM state machine to enable configuration */
		reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_CONTROL_REG);
		reg &= ~(PCIE_APP_LTSSM_EN);
		/* [한국어] LTSSM 을 끈 값을 되쓴다. */
		dw_pcie_writel_dbi(pci, PCIE_GLOBAL_CONTROL_REG, reg);
	/* [한국어] 이제 설정을 바꿔도 안전하다. */
	}

	/* Set the device to root complex mode */
	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_CONTROL_REG);
	reg &= ~(PCIE_DEVICE_TYPE_MASK << PCIE_DEVICE_TYPE_SHIFT);
	/* [한국어] RC 모드 값을 그 자리에 넣는다. */
	reg |= PCIE_DEVICE_TYPE_RC << PCIE_DEVICE_TYPE_SHIFT;
	/* [한국어] 되쓴다. 이 한 줄이 이 컨트롤러를 루트 컴플렉스로 만든다. */
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_CONTROL_REG, reg);
/* [한국어] 이제 AXI 속성을 세운다. */

	/* Set the PCIe master AxCache attributes */
	dw_pcie_writel_dbi(pci, PCIE_ARCACHE_TRC_REG, ARCACHE_DEFAULT_VALUE);
	dw_pcie_writel_dbi(pci, PCIE_AWCACHE_TRC_REG, AWCACHE_DEFAULT_VALUE);
/* [한국어] 쓰기 쪽 캐시 속성도 같은 방식으로 정한다. 두 상수의 뜻은 위 상류 주석에 있다. */

	/* Set the PCIe master AxDomain attributes */
	reg = dw_pcie_readl_dbi(pci, PCIE_ARUSER_REG);
	reg &= ~(AX_USER_DOMAIN_MASK << AX_USER_DOMAIN_SHIFT);
	/* [한국어] outer shareable 로 지정한다 — 그래야 CPU 캐시와 일관성이 유지되어,
	 * 장치가 쓴 데이터를 CPU 가 무효화 없이 볼 수 있다. */
	reg |= DOMAIN_OUTER_SHAREABLE << AX_USER_DOMAIN_SHIFT;
	/* [한국어] 읽기 쪽 도메인을 되쓴다. */
	dw_pcie_writel_dbi(pci, PCIE_ARUSER_REG, reg);
/* [한국어] 쓰기 쪽도 같은 설정이 필요하다. */

	reg = dw_pcie_readl_dbi(pci, PCIE_AWUSER_REG);
	/* [한국어] 기존 도메인 값을 지우고, */
	reg &= ~(AX_USER_DOMAIN_MASK << AX_USER_DOMAIN_SHIFT);
	/* [한국어] 같은 outer shareable 을 넣는다. */
	reg |= DOMAIN_OUTER_SHAREABLE << AX_USER_DOMAIN_SHIFT;
	/* [한국어] 쓰기 쪽 도메인을 되쓴다. */
	dw_pcie_writel_dbi(pci, PCIE_AWUSER_REG, reg);
/* [한국어] AXI 속성 설정이 끝났다. 이것이 틀리면 링크는 서지만 DMA 데이터가 어긋난다. */

	/* Enable INT A-D interrupts */
	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_INT_MASK1_REG);
	reg |= PCIE_INT_A_ASSERT_MASK | PCIE_INT_B_ASSERT_MASK |
	       /* [한국어] INTC 와 INTD 까지 네 비트를 모두 세운다. */
	       PCIE_INT_C_ASSERT_MASK | PCIE_INT_D_ASSERT_MASK;
	/* [한국어] 되쓴다. 이제 INTx 가 이 컨트롤러까지 올라온다. */
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_INT_MASK1_REG, reg);
/* [한국어] SoC 고유의 설정이 모두 끝났다. */

	return 0;
}

/* [한국어]
 * armada8k_pcie_irq_handler - 래치된 인터럽트 원인 비트를 지운다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: 등록 시 넘겨 둔 드라이버 상태.
 * @return: 언제나 IRQ_HANDLED.
 *
 * **하는 일이 지우기뿐이다.** 위 상류 주석이 그 이유를 밝힌다 — INTx 는
 * 장치 드라이버가 직접 처리하고, 이 컨트롤러는 그것을 래치만 해 둔다.
 * 지우지 않으면 같은 인터럽트가 계속 다시 올라온다.
 *
 * 읽은 값을 그대로 되쓰는 것이 write-1-to-clear 동작이다.
 *
 * 공유 인터럽트로 등록되는데도 언제나 IRQ_HANDLED 를 돌려준다. 자기 원인
 * 비트가 하나도 서 있지 않아도 그렇게 답하므로, 같은 선을 쓰는 다른
 * 핸들러에게 차례가 넘어가지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들 수 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 → [이 함수] → dw_pcie_readl_dbi() → dw_pcie_writel_dbi()
 */
static irqreturn_t armada8k_pcie_irq_handler(int irq, void *arg)
{
	struct armada8k_pcie *pcie = arg;
	struct dw_pcie *pci = pcie->pci;
	/* [한국어] 읽어 낸 원인 비트들. */
	u32 val;
/* [한국어] 위 상류 주석이 이 핸들러가 하는 일의 전부를 밝힌다. */

	/*
	 * Interrupts are directly handled by the device driver of the
	 * PCI device. However, they are also latched into the PCIe
	 * controller, so we simply discard them.
	 */
	val = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_INT_CAUSE1_REG);
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_INT_CAUSE1_REG, val);
/* [한국어] 읽은 값을 그대로 되써 지운다(write-1-to-clear). 지우지 않으면
 * 같은 인터럽트가 계속 다시 올라온다. */

	return IRQ_HANDLED;
}

static const struct dw_pcie_host_ops armada8k_pcie_host_ops = {
	/* [한국어] 이 SoC 가 DWC 표준에서 벗어나는 부분이 초기화 하나뿐이다. */
	.init = armada8k_pcie_host_init,
};

/* [한국어]
 * armada8k_add_pcie_port - 인터럽트를 걸고 DWC 호스트를 초기화한다
 *
 * @pcie: 드라이버 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버가 DWC 코어에 제어를 넘기는 지점이다.
 *
 * 인터럽트 번호를 pp->irq 에 담는 것이 눈에 띈다. DWC 코어도 그 필드를 보므로,
 * 같은 선을 이 파일의 핸들러와 코어가 나눠 쓰는 셈이다 — IRQF_SHARED 로
 * 거는 것이 그 때문이다.
 *
 * 콜백 표를 건 뒤 코어를 부르면, 그 안에서 이 파일의 host_init 과
 * start_link 가 차례로 불린다.
 *
 * 되감기가 없다. 인터럽트는 devm 판이라 자동으로 풀리고, 코어 초기화가
 * 실패하면 probe 의 disable_phy 경로가 나머지를 되돌린다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 링크 대기와 버스 스캔으로
 * 오래 걸린다.
 *
 * 에러 경로: 인터럽트를 못 얻거나 못 걸면 그 오류를, 코어 초기화가
 * 실패하면 그 오류를 기록과 함께 올려보낸다.
 *
 * 호출 체인:
 *   armada8k_pcie_probe() → [이 함수]
 *     → platform_get_irq() → devm_request_irq() → dw_pcie_host_init()
 */
static int armada8k_add_pcie_port(struct armada8k_pcie *pcie,
				  struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 로그와 인터럽트 등록에 쓸 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 콜백 표를 건다. */

	pp->ops = &armada8k_pcie_host_ops;
/* [한국어] 아래 코어 호출 도중에 이 표의 init 이 불린다. */

	pp->irq = platform_get_irq(pdev, 0);
	/* [한국어] 인터럽트 번호를 얻지 못하면, */
	if (pp->irq < 0)
		/* [한국어] 그 오류를 올려보낸다. */
		return pp->irq;

	ret = devm_request_irq(dev, pp->irq, armada8k_pcie_irq_handler,
			       /* [한국어] **공유 인터럽트로 건다** — 같은 선을 DWC 코어도 쓰기 때문이다. */
			       IRQF_SHARED, "armada8k-pcie", pcie);
	if (ret) {
		/* [한국어] 등록이 실패하면 어느 번호였는지 남기고, */
		dev_err(dev, "failed to request irq %d\n", pp->irq);
		/* [한국어] 그 오류를 올려보낸다. */
		return ret;
	}

	ret = dw_pcie_host_init(pp);
	/* [한국어] 코어 초기화가 실패하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "failed to initialize host: %d\n", ret);
		/* [한국어] 그 오류를 올려보낸다. 되감기는 호출자의 disable_phy 경로가 한다. */
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크 판정도 벤더 레지스터를 봐야 해 콜백으로 둔다. */
	.link_up = armada8k_pcie_link_up,
	/* [한국어] 링크 시작도 마찬가지다. 이 표에 둘뿐인 것이
	 * 이 SoC 가 DWC 표준에 가깝다는 뜻이다. */
	.start_link = armada8k_pcie_start_link,
};

/* [한국어]
 * armada8k_pcie_probe - 클럭과 DBI 창과 PHY 를 얻어 DWC 호스트를 세운다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * **dw_pcie 를 따로 할당한다.** 다른 글루 드라이버들이 그것을 자기 구조체
 * 안에 값으로 품는 것과 달리, 여기서는 포인터로 들고 있어 할당이 두 번이다.
 * 그 때문에 to_armada8k_pcie() 매크로도 container_of 가 아니라 drvdata 를 거친다.
 *
 * 클럭이 둘이고 성격이 다르다. 첫 번째는 필수이고, "reg" 클럭은 선택 사항이라
 * 없어도 진행한다 — 다만 -EPROBE_DEFER 만은 실패로 다루는데, 그 클럭을
 * 제공할 드라이버가 아직 준비되지 않았을 뿐 나중에는 얻을 수 있기 때문이다.
 *
 * DBI 창을 "ctrl" 이라는 이름으로 얻는다. 이 창 하나가 표준 DBI 영역과
 * 벤더 전용 영역을 모두 담고 있어, 이 파일에 매핑이 하나뿐이다.
 *
 * 되감기가 계단이며 세 라벨이 있다. PHY 준비까지 성공한 뒤의 실패만
 * PHY 를 되돌리고, 그 전의 실패는 클럭만 되돌린다.
 *
 * [상류 코드 관찰] fail_clkreg 라벨이 clk_reg 를 조건 없이 끄는데, "reg"
 * 클럭 조회가 -EPROBE_DEFER 가 아닌 오류로 실패했다면 그 포인터는 오류
 * 포인터다. clk_disable_unprepare() 가 오류 포인터를 어떻게 다루는지는
 * 이 트리에 drivers/clk 가 없어 확인 못 함. 원본(1f0e418bb6)에서 코드는
 * 확인했으며 고치지 않았다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 실패를 goto 로 해당 라벨에 넣어 역순으로 되감는다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_clk_get() → devm_pci_remap_cfg_resource()
 *     → armada8k_pcie_setup_phys() → armada8k_add_pcie_port()
 */
static int armada8k_pcie_probe(struct platform_device *pdev)
{
	struct dw_pcie *pci;
	struct armada8k_pcie *pcie;
	/* [한국어] 로그와 자원 조회에 쓸 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] DBI 창 자원. */
	struct resource *base;
	/* [한국어] 각 단계의 결과. */
	int ret;
/* [한국어] 먼저 상태 구조를 잡는다. */

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 잡지 못하면, */
	if (!pcie)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] **dw_pcie 를 따로 잡는다** — 이 드라이버가 그것을 포인터로 들기 때문이다. */
	if (!pci)
		/* [한국어] 잡지 못하면 메모리 부족으로 물러난다. */
		return -ENOMEM;

	pci->dev = dev;
	/* [한국어] 코어 콜백 표를 건다. */
	pci->ops = &dw_pcie_ops;
/* [한국어] 두 구조를 이어 둔다. */

	pcie->pci = pci;
/* [한국어] 필수 클럭을 얻는다. */

	pcie->clk = devm_clk_get(dev, NULL);
	/* [한국어] 얻지 못하면, */
	if (IS_ERR(pcie->clk))
		/* [한국어] 그 오류를 올려보낸다. 이 클럭 없이는 레지스터에 접근조차 되지 않는다. */
		return PTR_ERR(pcie->clk);
/* [한국어] 그 클럭을 켠다. */

	ret = clk_prepare_enable(pcie->clk);
	/* [한국어] 켜지 못하면, */
	if (ret)
		/* [한국어] 물러난다. 여기까지는 되돌릴 것이 없다. */
		return ret;

	pcie->clk_reg = devm_clk_get(dev, "reg");
	/* [한국어] **-EPROBE_DEFER 만 실패로 다룬다** — 그 클럭을 제공할 드라이버가
	 * 아직 준비되지 않았을 뿐 나중에는 얻을 수 있기 때문이다. */
	if (pcie->clk_reg == ERR_PTR(-EPROBE_DEFER)) {
		/* [한국어] 그 경우 다시 시도하도록 알린다. */
		ret = -EPROBE_DEFER;
		goto fail;
	}
	if (!IS_ERR(pcie->clk_reg)) {
		/* [한국어] 정상적으로 얻었으면 켠다. 오류 포인터면 이 갈래에 들어오지 않는다. */
		ret = clk_prepare_enable(pcie->clk_reg);
		/* [한국어] 켜지 못하면, */
		if (ret)
			/* [한국어] 되감기 경로로 간다. */
			goto fail_clkreg;
	}

	/* Get the dw-pcie unit configuration/control registers base. */
	base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ctrl");
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, base);
	/* [한국어] DBI 창 매핑이 실패하면, */
	if (IS_ERR(pci->dbi_base)) {
		/* [한국어] 오류 코드를 꺼내, */
		ret = PTR_ERR(pci->dbi_base);
		/* [한국어] 클럭부터 되돌린다. */
		goto fail_clkreg;
	}

	ret = armada8k_pcie_setup_phys(pcie);
	/* [한국어] PHY 준비가 실패하면, */
	if (ret)
		/* [한국어] 역시 클럭부터 되돌린다 — **PHY 는 되돌리지 않는다**(위 enable_phys 의 관찰 참조). */
		goto fail_clkreg;

	platform_set_drvdata(pdev, pcie);
/* [한국어] 이 파일의 변환 매크로가 drvdata 를 거치므로 아래 호출보다 먼저 매달아야 한다. */

	ret = armada8k_add_pcie_port(pcie, pdev);
	/* [한국어] 호스트 초기화가 실패하면, */
	if (ret)
		/* [한국어] **여기서만** PHY 를 되돌린다 — PHY 준비가 성공한 뒤의 유일한 실패 경로다. */
		goto disable_phy;

	return 0;

disable_phy:
	armada8k_pcie_disable_phys(pcie);
fail_clkreg:
	clk_disable_unprepare(pcie->clk_reg);
fail:
	clk_disable_unprepare(pcie->clk);

	return ret;
}

static const struct of_device_id armada8k_pcie_of_match[] = {
	/* [한국어] 디바이스 트리에서 이 문자열로 매칭한다. */
	{ .compatible = "marvell,armada8k-pcie", },
	/* [한국어] 표의 끝 표시. */
	{},
};

static struct platform_driver armada8k_pcie_driver = {
	/* [한국어] remove 콜백이 없다 — 아래 suppress_bind_attrs 와 builtin 등록이
	 * 이 드라이버를 뗄 수 없게 만든다. */
	.probe		= armada8k_pcie_probe,
	/* [한국어] 드라이버 정보. */
	.driver = {
		.name	= "armada8k-pcie",
		/* [한국어] 위 매칭 표. */
		.of_match_table = armada8k_pcie_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(armada8k_pcie_driver);
