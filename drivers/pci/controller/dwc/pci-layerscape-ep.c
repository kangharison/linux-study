// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe controller EP driver for Freescale Layerscape SoCs
 *
 * Copyright (C) 2018 NXP Semiconductor.
 *
 * Author: Xiaowei Bao <xiaowei.bao@nxp.com>
 */

/* [한국어] 기본 커널 유틸. */
/*
 * [한국어 설명] NXP Layerscape SoC 의 DesignWare PCIe 엔드포인트 드라이버 (pci-layerscape-ep.c)
 *
 * === 파일의 역할 ===
 * Freescale/NXP Layerscape SoC 를 PCIe **엔드포인트** 로 동작시킨다.
 * 컨트롤러 IP 는 Synopsys DesignWare 이므로, 이 파일은 그 공용 EP 코어에
 * SoC 고유 부분만 끼워 넣는 얇은 글루다.
 * 그 고유 부분의 중심에 한 가지 하드웨어 사정이 있다. **링크가 내려가거나
 * 핫 리셋이 오면 Link Capabilities 레지스터의 최대 레인 폭과 지원 속도가
 * 사라진다.** 그대로 두면 링크가 다시 올라올 때 축소된 능력으로 협상되므로,
 * probe 가 부팅 시점의 값(RCW 가 설정한 것)을 저장해 두고 인터럽트 핸들러가
 * 링크 업마다 되쓴다. 이 드라이버에서 가장 중요한 상태가 그 lnkcap 필드다.
 * 나머지 콜백 넷은 대부분 위임이다. raise_irq 는 종류에 맞는 DWC 코어 함수를
 * 고르기만 하고, get_features 는 미리 채워 둔 서술자를 돌려주며,
 * get_dbi_offset 은 SoC 별 간격에 기능 번호를 곱한다. init 만 실제 정보를
 * 더하는데, MSI/MSI-X 지원 여부는 코어가 config 공간을 초기화한 뒤에야
 * 알 수 있어 probe 에서 채울 수 없기 때문이다.
 * 엔디언 접근자 한 쌍도 이 SoC 의 사정이다. Layerscape 는 같은 IP 가 빅
 * 엔디언과 리틀 엔디언 양쪽 보드에 쓰여, DT 의 big-endian 속성으로 갈린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 PCIe 엔드포인트 스택의 맨 아래에 있다. 위로는
 * pcie-designware-ep.c 가 EP 초기화와 BAR·인터럽트 처리를 지휘하고,
 * 그 위로 drivers/pci/endpoint 의 EPC/EPF 계층이 있어 사용자가 정의한
 * 기능(NTB, 테스트 등)을 이 EP 에 올린다.
 * probe 흐름:
 *   구조체 셋 할당 → drvdata 조회 → BAR 제약과 linkup_notifier 설정
 *   → "regs" 매핑 → big-endian 확인 → drvdata 연결
 *   → **Link Capabilities 저장(DWC 초기화 전)**
 *   → dw_pcie_ep_init() → dw_pcie_ep_init_registers()(여기서 init 훅)
 *   → pci_epc_init_notify() → 마지막에 인터럽트 등록
 * 동작 중 흐름은 인터럽트 하나뿐이다.
 *   PME 인터럽트 → ls_pcie_ep_event_handler()
 *     → 상태를 읽고 곧바로 지움
 *     → 링크 업이면: Link Capabilities 복원 → CFG_READY 세움
 *        → dw_pcie_ep_linkup() 으로 코어에 전파
 *     → 링크 다운이면: dw_pcie_ep_linkdown()
 *     → 핫 리셋이면: 로그만
 * 실행 컨텍스트는 둘이다. probe 는 프로세스 컨텍스트(__init)이고,
 * 이벤트 핸들러는 하드 IRQ 라 잠들 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-designware.h 의 struct dw_pcie / dw_pcie_ep / dw_pcie_ep_ops
 * 규약과 pcie-designware-ep.c 의 dw_pcie_ep_init(),
 * dw_pcie_ep_init_registers(), dw_pcie_ep_linkup/linkdown(),
 * dw_pcie_ep_raise_intx/msi/msix_irq*().
 * 옆쪽: DBI 접근자(dw_pcie_readl/writel_dbi)와 읽기 전용 잠금
 * (dw_pcie_dbi_ro_wr_en/dis) — Link Capabilities 복원이 그 잠금을 푼다.
 * 아래쪽: ioread32/iowrite32 와 그 빅 엔디언 판, platform_get_irq_byname(),
 * devm_pci_remap_cfg_resource().
 * DT 바인딩: compatible 다섯(ls1028a/ls1046a/ls1088a/ls2088a/lx2160ar2),
 * reg 항목 이름 "regs", 인터럽트 이름 "pme", 선택 속성 "big-endian".
 * NXP 확장 레지스터(PEX_PF0_ 계열, 0xC0000 대역)는 별도 창이 아니라
 * **DBI 안에** 있어, 접근자가 pci->dbi_base 를 기준으로 삼는다.
 * 공유 상태: struct ls_pcie_ep 하나(필드 여섯)와 그것이 가리키는 세 구조체.
 * 전역 변수는 없고 상수 표 다섯(ep_ops, drvdata 셋, of_match)만 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - ls_pcie_pf_lut_readl() / _writel(): 엔디언 분기를 가둔 접근자 한 쌍.
 *   나머지 코드가 바이트 순서를 신경 쓰지 않게 해 준다.
 * - ls_pcie_ep_event_handler(): 이 드라이버의 몸통. 상태를 읽고 **곧바로**
 *   지운 뒤 처리하므로, 오래 걸리는 링크 업 처리 중에 온 이벤트를 놓치지
 *   않는다. 링크 업에서 Link Capabilities 를 복원하고 CFG_READY 를 세운다.
 * - ls_pcie_ep_interrupt_init(): 핸들러를 먼저 걸고 나중에 허용하는 순서.
 *   probe 의 마지막 단계라, 모든 준비가 끝난 뒤 이벤트를 받기 시작한다.
 * - ls_pcie_ep_init(): 코어가 config 공간을 초기화한 뒤에만 알 수 있는
 *   MSI/MSI-X 지원 여부를 서술자에 덧붙인다. 능력 서술자를 정적 상수로
 *   둘 수 없는 이유가 이것이다.
 * - ls_pcie_ep_raise_irq(): 종류를 함수로 옮기는 분배뿐. SoC 고유 처리가 없다.
 * - ls_pcie_ep_get_dbi_offset(): 다중 기능 EP 의 기능별 config 공간 간격.
 *   LS1 은 0(단일 기능), LS2 는 0x20000, LX2 는 0x8000 — 세 drvdata 상수의
 *   차이가 이 값 하나뿐이다.
 * - ls_pcie_ep_probe(): 구조체 셋을 따로 할당하고, DWC 초기화 **전에**
 *   Link Capabilities 를 저장하며, drvdata 연결도 그 전에 한다(초기화 중
 *   불리는 콜백이 그 값을 쓰기 때문이다).
 * - builtin_platform_driver_probe(): probe 가 __init 이라 부팅 후 해제된다.
 *   즉 나중에 나타나는 장치에는 이 드라이버가 붙지 않는다 — 내장 장치 전용이다.
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - struct ls_pcie_ep_drvdata 의 dw_pcie_ops 필드를 세 drvdata 상수 중
 *   어느 것도 채우지 않는다. probe 가 그것을 pci->ops 에 대입하므로(:247)
 *   pci->ops 는 언제나 NULL 이다. link_up/start_link 를 쓰지 않는 EP
 *   경로라 지금은 문제가 되지 않는 것으로 보인다.
 * - 같은 구조체의 ops 필드는 세 drvdata 모두 채우지만 읽는 곳이 없다.
 *   probe 가 pci->ep.ops 에 전역 &ls_pcie_ep_ops 를 직접 대입한다(:261).
 * - 핫 리셋 경로가 Link Capabilities 를 복원하지 않는다. 상류 영어 주석은
 *   핫 리셋으로도 그 값이 사라진다고 밝히는데, 핫 리셋 뒤 링크가 다시
 *   올라오며 오는 LUD 이벤트가 복원하는 것으로 보인다.
 * - dma_set_mask_and_coherent() 의 반환값을 확인하지 않는다.
 *
 * === NVMe 관점 ===
 * 방향이 반대라는 점이 이 파일의 NVMe 관련성을 정한다. 이 드라이버는
 * Layerscape 보드를 **엔드포인트** 로 만들므로, 이 보드가 NVMe SSD 를
 * 붙이는 호스트가 되는 것이 아니라 상대 호스트에게 PCIe 장치로 보인다.
 * 그 위에 어떤 기능을 올릴지는 drivers/pci/endpoint 의 EPF 드라이버가 정하며,
 * NVMe 컨트롤러를 흉내 내는 EPF 를 올리면 이 보드가 상대에게 SSD 처럼
 * 보이게 만들 수도 있다.
 * Link Capabilities 복원이 그 구성에서 실질적인 의미를 갖는다. 그것이
 * 없으면 링크가 한 번 끊긴 뒤 축소된 레인 폭으로 재협상되어, 상대 호스트가
 * 보는 대역폭이 조용히 줄어든다.
 */

#include <linux/kernel.h>
/* [한국어] __init 섹션 표시. 아래 probe 가 __init 인 것과 짝이다. */
#include <linux/init.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/of_pci.h>
/* [한국어] of_device_get_match_data(). */
#include <linux/of_platform.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/of_address.h>
/* [한국어] PCI_CAP_ID_EXP, PCI_EXP_LNKCAP, PCI_IRQ_ 계열 상수. */
#include <linux/pci.h>
/* [한국어] platform_get_irq_byname(), platform_get_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/resource.h>

/* [한국어] DesignWare PCIe 코어. struct dw_pcie / dw_pcie_ep / dw_pcie_ep_ops,
 * dw_pcie_ep_init(), DBI 접근자, 그리고 dw_pcie_ep_raise_*_irq(). */
#include "pcie-designware.h"

/* [한국어] PF0 설정 레지스터의 DBI 오프셋. 0xC0000 대역은 DesignWare IP 바깥에
 * NXP 가 덧붙인 레지스터 구역이지만, 별도 창이 아니라 DBI 안에 있다. */
#define PEX_PF0_CONFIG			0xC0014
/* [한국어] "설정이 끝났다" 를 호스트에 알리는 비트. 링크가 올라온 뒤 세운다. */
#define PEX_PF0_CFG_READY		BIT(0)

/* PEX PFa PCIE PME and message interrupt registers*/
/* [한국어] PME·메시지 인터럽트 상태 레지스터. */
#define PEX_PF0_PME_MES_DR		0xC0020
/* [한국어] 링크 업(Link Up Detected). */
#define PEX_PF0_PME_MES_DR_LUD		BIT(7)
/* [한국어] 링크 다운(Link Down Detected). */
#define PEX_PF0_PME_MES_DR_LDD		BIT(9)
/* [한국어] 핫 리셋(Hot Reset Detected). 세 이벤트가 이 EP 가 관심 갖는 전부다. */
#define PEX_PF0_PME_MES_DR_HRD		BIT(10)

/* [한국어] 같은 세 이벤트의 허용 레지스터. 상태와 허용이 같은 비트 배치를 쓴다. */
#define PEX_PF0_PME_MES_IER		0xC0028
/* [한국어] 링크 업 허용. */
#define PEX_PF0_PME_MES_IER_LUDIE	BIT(7)
/* [한국어] 링크 다운 허용. */
#define PEX_PF0_PME_MES_IER_LDDIE	BIT(9)
/* [한국어] 핫 리셋 허용. */
#define PEX_PF0_PME_MES_IER_HRDIE	BIT(10)

/* [한국어] dw_pcie 에서 이 드라이버의 상태를 되찾는다. container_of 가 아니라
 * drvdata 조회인 것은 dw_pcie 를 내장하지 않고 포인터로 두었기 때문이다. */
#define to_ls_pcie_ep(x)	dev_get_drvdata((x)->dev)

struct ls_pcie_ep_drvdata {
	/* [한국어] 다중 기능(multi-function) EP 에서 기능 하나가 차지하는 DBI 오프셋 간격.
	 * 설정자: 아래 세 drvdata 상수 중 하나가 SoC 에 맞는 값을 준다.
	 * 읽는 자: ls_pcie_ep_get_dbi_offset() 이 기능 번호에 곱한다.
	 * 값 범위: LS1 계열은 0(단일 기능), LS2 는 0x20000, LX2 는 0x8000.
	 * 동기화: 상수 표라 필요 없다. */
	u32				func_offset;
	/* [한국어] 이 SoC 용 EP 콜백 표.
	 * 설정자: 세 drvdata 모두 같은 &ls_pcie_ep_ops 를 넣는다.
	 * [상류 코드 관찰] 읽는 곳이 없다. probe 가 pci->ep.ops 에 전역을 직접
	 *   대입하므로(:261) 이 필드는 채워지기만 하고 쓰이지 않는다.
	 * 값 범위: 세 drvdata 모두 같은 값.
	 * 동기화: 상수 표. */
	const struct dw_pcie_ep_ops	*ops;
	/* [한국어] 이 SoC 용 dw_pcie 콜백 표.
	 * 설정자: **어느 drvdata 도 이 필드를 채우지 않는다.**
	 * 읽는 자: probe 가 pci->ops 에 대입한다(:247).
	 * [상류 코드 관찰] 세 drvdata 가 모두 이 필드를 생략하므로 언제나 NULL 이고,
	 *   따라서 pci->ops 도 언제나 NULL 이다. DWC 코어가 link_up/start_link 를
	 *   쓰지 않는 EP 경로라 지금은 문제가 되지 않는 것으로 보인다.
	 * 값 범위: NULL.
	 * 동기화: 상수 표. */
	const struct dw_pcie_ops	*dw_pcie_ops;
};

struct ls_pcie_ep {
	/* [한국어] DWC 코어가 다루는 공용 컨트롤러 객체. 내장이 아니라 포인터라,
	 *   위 to_ls_pcie_ep 매크로가 drvdata 조회다.
	 * 설정자: probe 가 따로 할당해 연결한다.
	 * 읽는 자: 이 파일의 모든 콜백과 DWC 코어.
	 * 값 범위: 유효한 dw_pcie 포인터.
	 * 동기화: probe 후 불변. */
	struct dw_pcie			*pci;
	/* [한국어] 이 EP 가 호스트에 보고할 능력 서술자.
	 * 설정자: probe 가 할당하고 BAR 제약과 linkup_notifier 를 채운 뒤,
	 *   ls_pcie_ep_init() 이 MSI/MSI-X 지원 여부를 덧붙인다.
	 * 읽는 자: ls_pcie_ep_get_features() 가 그대로 돌려준다.
	 * 값 범위: 유효한 pci_epc_features 포인터.
	 * 동기화: probe 와 init 콜백에서만 쓰고 이후 읽기만 한다. */
	struct pci_epc_features		*ls_epc;
	/* [한국어] 이 SoC 의 상수 표. 위 struct 참조.
	 * 설정자: probe 의 of_device_get_match_data().
	 * 읽는 자: get_dbi_offset 과 probe.
	 * 값 범위: 세 상수 중 하나.
	 * 동기화: probe 후 불변. */
	const struct ls_pcie_ep_drvdata *drvdata;
	/* [한국어] PME 인터럽트 번호.
	 * 설정자: ls_pcie_ep_interrupt_init() 의 platform_get_irq_byname("pme").
	 * 읽는 자: 그 자리에서 devm_request_irq 에 넘기는 것이 전부다.
	 * 값 범위: 유효한 IRQ 번호.
	 * 동기화: probe 후 불변. */
	int				irq;
	/* [한국어] 부팅 시점의 Link Capabilities 레지스터 값 사본.
	 * 설정자: probe 가 DWC 초기화 **전에** 읽어 둔다.
	 * 읽는 자: 인터럽트 핸들러가 링크 업 때마다 이 값을 되쓴다.
	 * 값 범위: RCW(Reset Configuration Word)가 설정한 최대 레인 폭과 속도.
	 * 동기화: probe 후 읽기 전용.
	 * 이 필드가 이 드라이버에서 가장 중요한 상태다 — 링크가 내려가거나
	 *   핫 리셋이 오면 하드웨어가 그 레지스터를 잃어버리기 때문이다. */
	u32				lnkcap;
	/* [한국어] 레지스터 접근을 빅 엔디언으로 할지.
	 * 설정자: probe 가 DT 의 "big-endian" 속성을 읽는다.
	 * 읽는 자: 아래 두 접근자.
	 * 값 범위: true/false. Layerscape SoC 가 두 엔디언 모두로 쓰이기 때문에
	 *   필요한 구분이다.
	 * 동기화: probe 후 불변. */
	bool				big_endian;
};

/* [한국어]
 * ls_pcie_pf_lut_readl - NXP 확장 레지스터를 엔디언에 맞게 읽는다
 *
 * @pcie: 이 드라이버의 상태.
 * @offset: DBI 창 안에서의 오프셋.
 * @return: 읽은 32비트 값.
 *
 * Layerscape SoC 는 같은 IP 가 빅 엔디언과 리틀 엔디언 양쪽 보드에 쓰이므로,
 * 레지스터 접근마다 바이트 순서를 골라야 한다. 이 접근자 한 쌍이 그 분기를
 * 가둬 두어, 나머지 코드가 엔디언을 신경 쓰지 않아도 되게 한다.
 *
 * 읽는 대상이 DBI 창 안이라는 점에 주의할 만하다. 이름의 PF_LUT 은 NXP 가
 * DesignWare IP 바깥에 덧붙인 레지스터 구역을 가리키지만, 별도 창이 아니라
 * DBI 의 0xC0000 대역에 놓여 있다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러와 probe 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_ep_event_handler() / ls_pcie_ep_interrupt_init() → [이 함수]
 *     → ioread32be() 또는 ioread32()
 */
static u32 ls_pcie_pf_lut_readl(struct ls_pcie_ep *pcie, u32 offset)
{
	/* [한국어] 공용 객체에서 DBI 창을 얻는다. */
	struct dw_pcie *pci = pcie->pci;

	/* [한국어] 빅 엔디언이면, */
	if (pcie->big_endian)
		/* [한국어] 바이트 순서를 바꿔 읽는 판을 쓴다. */
		return ioread32be(pci->dbi_base + offset);
	else
		/* [한국어] 아니면 평범한 판. 이 두 접근자가 있어 나머지 코드가 엔디언을 신경 쓰지
		 * 않아도 된다. */
		return ioread32(pci->dbi_base + offset);
}

/* [한국어]
 * ls_pcie_pf_lut_writel - NXP 확장 레지스터를 엔디언에 맞게 쓴다
 *
 * @pcie: 이 드라이버의 상태.
 * @offset: DBI 창 안에서의 오프셋.
 * @value: 쓸 값.
 *
 * 읽기 쪽과 대칭이며 같은 이유로 존재한다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러와 probe 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_ep_event_handler() / ls_pcie_ep_interrupt_init() → [이 함수]
 *     → iowrite32be() 또는 iowrite32()
 */
static void ls_pcie_pf_lut_writel(struct ls_pcie_ep *pcie, u32 offset, u32 value)
{
	/* [한국어] 공용 객체. */
	struct dw_pcie *pci = pcie->pci;

	/* [한국어] 빅 엔디언이면, */
	if (pcie->big_endian)
		/* [한국어] 바꿔 쓰는 판. */
		iowrite32be(value, pci->dbi_base + offset);
	else
		/* [한국어] 아니면 평범한 판. */
		iowrite32(value, pci->dbi_base + offset);
}

/* [한국어]
 * ls_pcie_ep_event_handler - 링크 업/다운/핫 리셋을 받아 처리한다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @dev_id: 등록 시 넘겨 둔 드라이버 상태.
 * @return: IRQ_HANDLED 또는 IRQ_NONE.
 *
 * 이 드라이버의 몸통이다. 세 이벤트 중 링크 업만 실제 작업이 있다.
 *
 * 상태를 읽고 **곧바로** 되써서 지우는 순서가 눈에 띈다. 처리보다 먼저
 * 지우므로, 아래 링크 업 처리가 도는 동안 새로 오는 이벤트를 놓치지 않는다.
 * 읽은 값을 그대로 쓰는 W1C 방식이라 그 사이에 새로 선 비트는 지워지지 않는다.
 *
 * 링크 업 처리의 핵심은 Link Capabilities 복원이다. 함수 안의 영어 주석이
 * 밝히듯, 링크가 내려가거나 핫 리셋이 오면 하드웨어가 최대 레인 폭과 지원
 * 속도를 잃어버린다. probe 가 부팅 시점에 저장해 둔 RCW 설정값을 DBI 읽기
 * 전용 잠금을 풀고 되쓴다.
 *
 * 그 뒤 PEX_PF0_CFG_READY 를 세워 호스트에 "설정이 끝났다" 고 알리고,
 * DWC 코어에 링크 업을 전파한다.
 *
 * [상류 코드 관찰] 핫 리셋 경로는 로그만 남기고 Link Capabilities 를 복원하지
 * 않는다. 위 영어 주석은 핫 리셋으로도 그 값이 사라진다고 밝히는데,
 * 핫 리셋 뒤 링크가 다시 올라오며 LUD 이벤트가 와서 그때 복원되는 것으로
 * 보인다.
 *
 * IRQF_SHARED 로 등록하므로 상태가 0 이면 IRQ_NONE 을 돌려주어야 한다.
 *
 * 실행 컨텍스트: 하드 IRQ. 잠들 수 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 *     → ls_pcie_pf_lut_readl/writel() → dw_pcie_find_capability()
 *     → dw_pcie_dbi_ro_wr_en/dis() → dw_pcie_ep_linkup/linkdown()
 */
static irqreturn_t ls_pcie_ep_event_handler(int irq, void *dev_id)
{
	/* [한국어] 등록 시 넘겨 둔 드라이버 상태. */
	struct ls_pcie_ep *pcie = dev_id;
	/* [한국어] 공용 객체. */
	struct dw_pcie *pci = pcie->pci;
	/* [한국어] 상태 값과 설정 레지스터 값. */
	u32 val, cfg;
	/* [한국어] PCIe capability 오프셋. */
	u8 offset;

	/* [한국어] 어떤 이벤트가 왔는지 읽고, */
	val = ls_pcie_pf_lut_readl(pcie, PEX_PF0_PME_MES_DR);
	/* [한국어] **곧바로** 그 값을 되써서 지운다. 처리보다 먼저 지우는 순서인데,
	 * 아래 링크 업 처리가 오래 걸려 그 사이의 새 이벤트를 놓치지 않으려는
	 * 것으로 보인다. 읽은 값을 그대로 쓰는 W1C 방식이라 그 사이에 새로 선
	 * 비트는 지워지지 않는다. */
	ls_pcie_pf_lut_writel(pcie, PEX_PF0_PME_MES_DR, val);

	/* [한국어] 아무 비트도 서 있지 않으면 우리 인터럽트가 아니다. IRQF_SHARED 로
	 * 등록하므로 IRQ_NONE 을 돌려주어야 커널이 다른 핸들러를 시도한다. */
	if (!val)
		return IRQ_NONE;

	/* [한국어] 링크가 올라왔으면, */
	if (val & PEX_PF0_PME_MES_DR_LUD) {

		/* [한국어] PCIe capability 위치를 찾는다. */
		offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);

		/*
		 * The values of the Maximum Link Width and Supported Link
		 * Speed from the Link Capabilities Register will be lost
		 * during link down or hot reset. Restore initial value
		 * that configured by the Reset Configuration Word (RCW).
		 */
		/* [한국어] DBI 의 읽기 전용 잠금을 푼다. */
		dw_pcie_dbi_ro_wr_en(pci);
		/* [한국어] 위 영어 주석이 이 함수의 존재 이유를 밝힌다 — 링크가 내려가거나 핫 리셋이
		 * 오면 Link Capabilities 의 최대 레인 폭과 지원 속도가 사라진다.
		 * probe 가 부팅 시점에 저장해 둔 값(RCW 가 설정한 것)을 되쓴다. */
		dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, pcie->lnkcap);
		/* [한국어] 다시 잠근다. */
		dw_pcie_dbi_ro_wr_dis(pci);

		/* [한국어] 설정 레지스터를 읽어, */
		cfg = ls_pcie_pf_lut_readl(pcie, PEX_PF0_CONFIG);
		/* [한국어] 준비 완료 비트를 세우고, */
		cfg |= PEX_PF0_CFG_READY;
		/* [한국어] 되쓴다. 이 쓰기로 호스트가 이 EP 를 쓸 수 있게 된다. */
		ls_pcie_pf_lut_writel(pcie, PEX_PF0_CONFIG, cfg);
		/* [한국어] DWC 코어에 링크 업을 알린다. 코어가 EPC 계층으로 전파한다. */
		dw_pcie_ep_linkup(&pci->ep);

		/* [한국어] 디버그 로그. */
		dev_dbg(pci->dev, "Link up\n");
	/* [한국어] 링크가 내려갔으면, */
	} else if (val & PEX_PF0_PME_MES_DR_LDD) {
		/* [한국어] 기록하고, */
		dev_dbg(pci->dev, "Link down\n");
		/* [한국어] 코어에 알린다. 링크 업과 달리 레지스터 복원이 없는데, 내려간 상태에서는
		 * 쓸 수 없고 다시 올라올 때 위 경로가 복원하기 때문이다. */
		dw_pcie_ep_linkdown(&pci->ep);
	/* [한국어] 핫 리셋이면, */
	} else if (val & PEX_PF0_PME_MES_DR_HRD) {
		/* [한국어] 기록만 한다.
		 * [상류 코드 관찰] Link Capabilities 는 핫 리셋으로도 사라지는데(위 영어
		 *   주석이 그렇게 밝힌다) 이 경로에서는 복원하지 않는다. 핫 리셋 뒤
		 *   링크가 다시 올라오면서 LUD 이벤트가 오고 그때 복원되는 것으로 보인다. */
		dev_dbg(pci->dev, "Hot reset\n");
	}

	/* [한국어] 셋 중 하나라도 처리했으면 우리 인터럽트다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * ls_pcie_ep_interrupt_init - PME 인터럽트를 걸고 세 이벤트를 허용한다
 *
 * @pcie: 이 드라이버의 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, platform_get_irq_byname() 이나 devm_request_irq() 의 오류.
 *
 * 순서가 이 함수의 요점이다. 핸들러를 **먼저** 걸고 그 다음에 이벤트를
 * 허용한다. 반대로 하면 허용된 인터럽트를 받을 준비가 안 된 창이 생긴다.
 *
 * 인터럽트를 인덱스가 아니라 "pme" 라는 이름으로 찾으므로, DT 에서 순서가
 * 바뀌어도 깨지지 않는다.
 *
 * IRQF_SHARED 로 등록하는 것은 이 SoC 에서 PME 선이 다른 장치와 공유되기
 * 때문이며, 그래서 핸들러가 IRQ_NONE 을 제대로 돌려주는 것이 중요해진다.
 *
 * 허용 레지스터를 읽기-수정-쓰기로 다루어 다른 비트를 보존한다.
 *
 * probe 의 **마지막** 단계로 불린다. DWC 초기화와 EPC 알림이 모두 끝난 뒤에
 * 인터럽트를 여는 순서다.
 *
 * 실행 컨텍스트: probe 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 두 실패 모두 그대로 올려보낸다. devm_ 이라 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   ls_pcie_ep_probe() → [이 함수]
 *     → platform_get_irq_byname("pme") → devm_request_irq()
 *     → ls_pcie_pf_lut_readl/writel(PEX_PF0_PME_MES_IER)
 */
static int ls_pcie_ep_interrupt_init(struct ls_pcie_ep *pcie,
				     struct platform_device *pdev)
{
	/* [한국어] 허용 레지스터 값. */
	u32 val;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] DT 에서 "pme" 라는 이름의 인터럽트를 찾는다. 인덱스가 아니라 이름으로
	 * 찾으므로 DT 에서 순서가 바뀌어도 안전하다. */
	pcie->irq = platform_get_irq_byname(pdev, "pme");
	/* [한국어] 없으면, */
	if (pcie->irq < 0)
		return pcie->irq;

	/* [한국어] 핸들러를 건다. devm_ 이라 해제는 자동이다. */
	ret = devm_request_irq(&pdev->dev, pcie->irq, ls_pcie_ep_event_handler,
			       /* [한국어] 공유 인터럽트로 등록한다. 이 SoC 에서 PME 선이 다른 장치와 공유되기
			        * 때문이며, 그래서 핸들러가 IRQ_NONE 을 제대로 돌려주어야 한다. */
			       IRQF_SHARED, pdev->name, pcie);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 기록하고, */
		dev_err(&pdev->dev, "Can't register PCIe IRQ\n");
		return ret;
	}

	/* Enable interrupts */
	/* [한국어] 허용 레지스터를 읽어, */
	val = ls_pcie_pf_lut_readl(pcie, PEX_PF0_PME_MES_IER);
	/* [한국어] 세 이벤트를 모두 허용하고, */
	val |=  PEX_PF0_PME_MES_IER_LDDIE | PEX_PF0_PME_MES_IER_HRDIE |
		PEX_PF0_PME_MES_IER_LUDIE;
	/* [한국어] 되쓴다. 읽기-수정-쓰기라 다른 비트를 보존한다. 핸들러를 **먼저** 걸고
	 * 나중에 허용하는 순서라, 허용하는 순간 인터럽트가 와도 받을 준비가 되어 있다. */
	ls_pcie_pf_lut_writel(pcie, PEX_PF0_PME_MES_IER, val);

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * ls_pcie_ep_get_features - 이 EP 의 능력 서술자를 돌려준다
 *
 * @ep: DWC 코어의 EP 객체.
 * @return: probe 와 init 이 채워 둔 서술자.
 *
 * 미리 채워 둔 것을 그대로 넘기는 한 줄 함수다. 매번 만들지 않는 이유는
 * 내용이 두 시점에 걸쳐 채워지기 때문이다 — probe 가 BAR 제약과
 * linkup_notifier 를, ls_pcie_ep_init() 이 MSI/MSI-X 지원 여부를 채운다.
 *
 * 정적 상수로 둘 수 없는 것도 그래서다. MSI 지원 여부는 DWC 코어가 config
 * 공간을 초기화한 뒤에야 알 수 있다.
 *
 * 실행 컨텍스트: EPC 계층의 조회. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_epc_get_features() → dw_pcie_ep_get_features() → [이 함수]
 */
static const struct pci_epc_features*
ls_pcie_ep_get_features(struct dw_pcie_ep *ep)
{
	/* [한국어] EP 객체에서 공용 객체를 얻고, */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 거기서 이 드라이버의 상태를 되찾는다. */
	struct ls_pcie_ep *pcie = to_ls_pcie_ep(pci);

	/* [한국어] probe 와 init 이 채워 둔 서술자를 그대로 돌려준다. 매번 만들지 않고
	 * 미리 채워 두는 방식이다. */
	return pcie->ls_epc;
}

/* [한국어]
 * ls_pcie_ep_init - config 공간 초기화 뒤 MSI 지원 여부를 서술자에 채운다
 *
 * @ep: DWC 코어의 EP 객체.
 *
 * DWC 코어가 config 공간을 초기화한 뒤 부르는 훅이다. 그 시점이어야 하는
 * 이유가 이 함수의 전부다 — MSI 와 MSI-X capability 가 그때 만들어지므로,
 * probe 에서는 지원 여부를 알 수 없다.
 *
 * 기능 0 의 정보만 본다. 다중 기능 EP 라도 MSI 지원 여부는 기능마다 다르지
 * 않다는 전제다.
 *
 * 기능을 얻지 못하면 아무것도 하지 않고 돌아간다. 반환값이 없어 실패를
 * 알릴 방법이 없으며, 그 경우 서술자의 두 필드가 false 로 남아 호스트가
 * MSI 를 쓸 수 없다고 판단한다.
 *
 * 실행 컨텍스트: probe 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_ep_init_registers() → ep->ops->init → [이 함수]
 *     → dw_pcie_ep_get_func_from_ep()
 */
static void ls_pcie_ep_init(struct dw_pcie_ep *ep)
{
	/* [한국어] 공용 객체. */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 드라이버 상태. */
	struct ls_pcie_ep *pcie = to_ls_pcie_ep(pci);
	/* [한국어] 기능 0 의 EP 정보. */
	struct dw_pcie_ep_func *ep_func;

	/* [한국어] 기능 0 을 얻는다. */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, 0);
	/* [한국어] 없으면, */
	if (!ep_func)
		/* [한국어] 아무것도 하지 않고 돌아간다. 반환값이 없어 실패를 알릴 방법이 없다. */
		return;

	/* [한국어] MSI capability 가 있으면 지원한다고 표시한다. */
	pcie->ls_epc->msi_capable = ep_func->msi_cap ? true : false;
	/* [한국어] MSI-X 도 마찬가지. probe 가 채운 BAR 제약에 이 둘을 덧붙이는 것이
	 * 이 콜백의 전부다 — 그 정보는 DWC 코어가 config 공간을 초기화한 뒤에야
	 * 알 수 있으므로 probe 에서 미리 채울 수 없다. */
	pcie->ls_epc->msix_capable = ep_func->msix_cap ? true : false;
}

/* [한국어]
 * ls_pcie_ep_raise_irq - 인터럽트 종류에 맞는 DWC 코어 함수로 넘긴다
 *
 * @ep: DWC 코어의 EP 객체.
 * @func_no: 기능 번호.
 * @type: PCI_IRQ_INTX / MSI / MSIX.
 * @interrupt_num: 벡터 번호. INTx 에서는 쓰이지 않는다.
 * @return: 코어 함수의 결과, 또는 -EINVAL.
 *
 * 세 경우 모두 코어 함수를 그대로 부르므로, 이 콜백이 하는 일은 종류를
 * 함수로 옮기는 분배뿐이다. SoC 고유 처리가 하나도 없다.
 *
 * 그래도 이 콜백이 필요한 이유는 dw_pcie_ep_ops 규약이 raise_irq 를
 * 필수로 요구하기 때문이다. 코어가 종류별 함수를 직접 고르지 않고 드라이버에
 * 맡기는 것은, SoC 에 따라 특정 종류를 지원하지 않거나 다르게 처리해야
 * 하는 경우가 있어서다.
 *
 * 실행 컨텍스트: EPC 계층의 인터럽트 발생 요청. 프로세스 컨텍스트.
 *
 * 에러 경로: 알 수 없는 종류만 -EINVAL 이며 로그를 남긴다.
 *
 * 호출 체인:
 *   pci_epc_raise_irq() → dw_pcie_ep_raise_irq() → [이 함수]
 *     → dw_pcie_ep_raise_intx_irq() / _msi_irq() / _msix_irq_doorbell()
 */
static int ls_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				unsigned int type, u16 interrupt_num)
{
	/* [한국어] 공용 객체. 오류 로그에만 쓴다. */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* [한국어] 인터럽트 종류에 따라 갈린다. */
	switch (type) {
	/* [한국어] 레거시 INTx 면, */
	case PCI_IRQ_INTX:
		/* [한국어] DWC 코어의 INTx 발생 함수로 넘긴다. */
		return dw_pcie_ep_raise_intx_irq(ep, func_no);
	/* [한국어] MSI 면, */
	case PCI_IRQ_MSI:
		/* [한국어] MSI 판으로. */
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	/* [한국어] MSI-X 면, */
	case PCI_IRQ_MSIX:
		/* [한국어] MSI-X 판으로. 세 경우 모두 코어 함수를 그대로 부르므로, 이 콜백은
		 * 종류를 함수로 옮기는 것이 전부다. */
		return dw_pcie_ep_raise_msix_irq_doorbell(ep, func_no,
							  interrupt_num);
	/* [한국어] 알 수 없는 종류면, */
	default:
		/* [한국어] 기록하고, */
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
		/* [한국어] 잘못된 인자로 답한다. */
		return -EINVAL;
	}
}

/* [한국어]
 * ls_pcie_ep_get_dbi_offset - 다중 기능 EP 에서 기능별 DBI 오프셋을 계산한다
 *
 * @ep: DWC 코어의 EP 객체.
 * @func_no: 기능 번호.
 * @return: 그 기능의 DBI 오프셋.
 *
 * 다중 기능 EP 는 기능마다 별도의 config 공간을 가지며, 이 SoC 에서는 그것이
 * DBI 창 안에 일정 간격으로 나열되어 있다. 간격은 SoC 마다 달라
 * drvdata 상수가 알려 준다 — LS1 은 0(단일 기능), LS2 는 0x20000,
 * LX2 는 0x8000.
 *
 * WARN_ON 이 잡는 것은 단일 기능 SoC 에 기능 0 이 아닌 요청이 온 경우다.
 * 간격이 0 이라 곱셈 결과가 언제나 0 이 되어, 모든 기능이 같은 config 공간을
 * 가리키게 된다.
 *
 * 기능 0 이면 어느 SoC 든 0 이 나오므로, 단일 기능 경로에서도 올바르게
 * 동작한다.
 *
 * 실행 컨텍스트: DBI 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다. 잘못된 조합은 경고만 남기고 0 을 반환한다.
 *
 * 호출 체인:
 *   DWC 코어의 기능별 DBI 접근 → ep->ops->get_dbi_offset → [이 함수]
 */
static unsigned int ls_pcie_ep_get_dbi_offset(struct dw_pcie_ep *ep, u8 func_no)
{
	/* [한국어] 공용 객체. */
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 드라이버 상태. */
	struct ls_pcie_ep *pcie = to_ls_pcie_ep(pci);

	/* [한국어] 기능 번호가 0 이 아닌데 간격이 0 이면 다중 기능을 지원하지 않는 SoC 에
	 * 그 요청이 온 것이다. 계산이 무의미해지므로 경고한다. */
	WARN_ON(func_no && !pcie->drvdata->func_offset);
	/* [한국어] 간격에 기능 번호를 곱해 오프셋을 만든다. 기능 0 이면 0 이라
	 * 단일 기능 SoC 에서도 올바르게 동작한다. */
	return pcie->drvdata->func_offset * func_no;
}

static const struct dw_pcie_ep_ops ls_pcie_ep_ops = {
	/* [한국어] config 공간 초기화 뒤의 마무리 훅. */
	.init = ls_pcie_ep_init,
	/* [한국어] 인터럽트 발생. */
	.raise_irq = ls_pcie_ep_raise_irq,
	/* [한국어] 능력 서술자 조회. */
	.get_features = ls_pcie_ep_get_features,
	/* [한국어] 다중 기능 DBI 오프셋 계산. */
	.get_dbi_offset = ls_pcie_ep_get_dbi_offset,
};

static const struct ls_pcie_ep_drvdata ls1_ep_drvdata = {
	/* [한국어] LS1 계열. func_offset 을 생략하므로 0 — 단일 기능이다. */
	.ops = &ls_pcie_ep_ops,
};

static const struct ls_pcie_ep_drvdata ls2_ep_drvdata = {
	/* [한국어] LS2 계열은 기능 간격이 0x20000. */
	.func_offset = 0x20000,
	.ops = &ls_pcie_ep_ops,
};

static const struct ls_pcie_ep_drvdata lx2_ep_drvdata = {
	/* [한국어] LX2 계열은 0x8000. 세 상수의 차이가 이 값 하나뿐이다. */
	.func_offset = 0x8000,
	.ops = &ls_pcie_ep_ops,
};

static const struct of_device_id ls_pcie_ep_of_match[] = {
	/* [한국어] LS1028A. */
	{ .compatible = "fsl,ls1028a-pcie-ep", .data = &ls1_ep_drvdata },
	/* [한국어] LS1046A. 두 SoC 가 같은 상수를 공유한다. */
	{ .compatible = "fsl,ls1046a-pcie-ep", .data = &ls1_ep_drvdata },
	/* [한국어] LS1088A. */
	{ .compatible = "fsl,ls1088a-pcie-ep", .data = &ls2_ep_drvdata },
	/* [한국어] LS2088A. */
	{ .compatible = "fsl,ls2088a-pcie-ep", .data = &ls2_ep_drvdata },
	/* [한국어] LX2160A 리비전 2. */
	{ .compatible = "fsl,lx2160ar2-pcie-ep", .data = &lx2_ep_drvdata },
	/* [한국어] 배열 끝. */
	{ },
};

/* [한국어]
 * ls_pcie_ep_probe - 자원을 모으고 Link Capabilities 를 저장한 뒤 DWC 에 넘긴다
 *
 * @pdev: 매치된 플랫폼 장치.
 * @return: 0 = 성공, -ENOMEM, 매핑 오류, 또는 DWC 초기화 오류.
 *
 * 세 구조체를 따로 할당한다 — 드라이버 상태, 공용 dw_pcie 객체, 능력 서술자.
 * 대부분의 DWC 드라이버가 dw_pcie 를 내장하는 것과 다른 방식이며, 그래서
 * to_ls_pcie_ep 매크로가 container_of 가 아니라 drvdata 조회다.
 *
 * 능력 서술자를 정적 상수가 아니라 할당으로 두는 이유는
 * ls_pcie_ep_init() 이 나중에 MSI 지원 여부를 덧붙이기 때문이다.
 *
 * 순서에서 가장 중요한 것은 lnkcap 저장이다. DWC 초기화 **전에** Link
 * Capabilities 를 읽어 두는데, 지금 값이 RCW(Reset Configuration Word)가
 * 설정한 원래 값이기 때문이다. 인터럽트 핸들러가 링크 업마다 이 값으로
 * 복원한다.
 *
 * platform_set_drvdata() 도 DWC 초기화 전에 해야 한다. 그 안에서 불리는
 * ls_pcie_ep_init() 콜백이 to_ls_pcie_ep 로 이 값을 쓰기 때문이다.
 *
 * 인터럽트를 가장 마지막에 거는 것도 의도적이다. 모든 준비가 끝난 뒤에
 * 이벤트를 받기 시작한다.
 *
 * [상류 코드 관찰] 두 가지가 눈에 띈다. pci->ops 에 대입하는
 * drvdata->dw_pcie_ops 는 세 drvdata 모두 채우지 않아 언제나 NULL 이고,
 * dma_set_mask_and_coherent() 의 반환값을 확인하지 않는다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. __init 이라 부팅 후 해제된다.
 *
 * 에러 경로: 할당과 매핑 실패는 그대로 반환한다. 레지스터 초기화 실패만
 * dw_pcie_ep_deinit() 으로 되감는다.
 *
 * 호출 체인:
 *   builtin_platform_driver_probe → 플랫폼 버스 매치 → [이 함수]
 *     → devm_kzalloc() ×3 → of_device_get_match_data()
 *     → devm_pci_remap_cfg_resource() → dw_pcie_readl_dbi(LNKCAP)
 *     → dw_pcie_ep_init() → dw_pcie_ep_init_registers()
 *     → pci_epc_init_notify() → ls_pcie_ep_interrupt_init()
 */
static int __init ls_pcie_ep_probe(struct platform_device *pdev)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 할당할 공용 객체. */
	struct dw_pcie *pci;
	/* [한국어] 할당할 드라이버 상태. */
	struct ls_pcie_ep *pcie;
	/* [한국어] 할당할 능력 서술자. */
	struct pci_epc_features *ls_epc;
	/* [한국어] DBI 창 자원. */
	struct resource *dbi_base;
	/* [한국어] PCIe capability 오프셋. */
	u8 offset;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 드라이버 상태를 할당한다. */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!pcie)
		return -ENOMEM;

	/* [한국어] 공용 객체를 **따로** 할당한다. 대부분의 DWC 드라이버가 내장하는 것과
	 * 다른 방식이다. */
	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!pci)
		return -ENOMEM;

	/* [한국어] 능력 서술자도 따로 할당한다. 이것을 정적 상수로 두지 않는 이유는
	 * ls_pcie_ep_init() 이 MSI 지원 여부를 나중에 덧붙이기 때문이다. */
	ls_epc = devm_kzalloc(dev, sizeof(*ls_epc), GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!ls_epc)
		return -ENOMEM;

	/* [한국어] 매치된 SoC 의 상수 표를 얻는다. */
	pcie->drvdata = of_device_get_match_data(dev);

	/* [한국어] DWC 코어가 쓸 device. */
	pci->dev = dev;
	/* [한국어] dw_pcie 콜백 표를 연결한다.
	 * [상류 코드 관찰] 세 drvdata 모두 dw_pcie_ops 를 채우지 않으므로
	 *   이 대입은 언제나 NULL 을 넣는다. */
	pci->ops = pcie->drvdata->dw_pcie_ops;

	/* [한국어] BAR2 는 64비트 전용이라고 호스트에 알린다. */
	ls_epc->bar[BAR_2].only_64bit = true;
	/* [한국어] BAR4 도 마찬가지. 64비트 BAR 은 두 칸을 쓰므로, 그 제약을 미리 알려야
	 * 호스트가 32비트로 설정하려 들지 않는다. */
	ls_epc->bar[BAR_4].only_64bit = true;
	/* [한국어] 링크 상태 변화를 알림으로 받겠다고 표시한다. 위 인터럽트 핸들러가
	 * dw_pcie_ep_linkup/linkdown 을 부르는 것과 짝이다. */
	ls_epc->linkup_notifier = true;

	/* [한국어] 양방향 연결의 한쪽. */
	pcie->pci = pci;
	/* [한국어] 능력 서술자를 매단다. */
	pcie->ls_epc = ls_epc;

	/* [한국어] DT 에서 "regs" 라는 이름의 자원을 찾는다. */
	dbi_base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "regs");
	/* [한국어] config 전용 매핑 함수로 매핑한다. 평범한 ioremap 이 아닌 이유는
	 * 아키텍처에 따라 config 공간이 다른 메모리 속성을 요구하기 때문이다. */
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, dbi_base);
	/* [한국어] 실패하면, */
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);

	/* [한국어] EP 콜백 표를 연결한다. drvdata->ops 를 거치지 않고 전역을 직접 쓴다. */
	pci->ep.ops = &ls_pcie_ep_ops;

	/* [한국어] DT 의 big-endian 속성을 읽는다. 이 값이 위 두 접근자의 분기를 정한다. */
	pcie->big_endian = of_property_read_bool(dev->of_node, "big-endian");

	/* [한국어] 64비트 DMA 를 쓸 수 있다고 알린다.
	 * [상류 코드 관찰] 반환값을 확인하지 않는다. */
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	/* [한국어] 반대 방향 연결. 이제 to_ls_pcie_ep 매크로가 동작한다. DWC 초기화 **전에**
	 * 해야 하는데, 그 안에서 불리는 콜백들이 이 값을 쓰기 때문이다. */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] PCIe capability 위치를 찾아, */
	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* [한국어] Link Capabilities 를 읽어 둔다. **DWC 초기화 전** 이 시점이 중요한데,
	 * 지금 값이 RCW 가 설정한 원래 값이고 이후 링크 업마다 이것으로 복원하기
	 * 때문이다. */
	pcie->lnkcap = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);

	/* [한국어] DWC EP 코어를 초기화한다. */
	ret = dw_pcie_ep_init(&pci->ep);
	/* [한국어] 실패하면, */
	if (ret)
		return ret;

	/* [한국어] config 공간 레지스터를 초기화한다. 이 단계에서 MSI/MSI-X capability 가
	 * 만들어지고, 그것을 ls_pcie_ep_init() 콜백이 확인한다. */
	ret = dw_pcie_ep_init_registers(&pci->ep);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 기록하고, */
		dev_err(dev, "Failed to initialize DWC endpoint registers\n");
		/* [한국어] 앞 단계를 되감은 뒤, */
		dw_pcie_ep_deinit(&pci->ep);
		return ret;
	}

	/* [한국어] EPC 계층에 초기화 완료를 알린다. */
	pci_epc_init_notify(pci->ep.epc);

	/* [한국어] 마지막으로 인터럽트를 건다. 모든 준비가 끝난 뒤에 여는 순서다. */
	return ls_pcie_ep_interrupt_init(pcie, pdev);
}

static struct platform_driver ls_pcie_ep_driver = {
	.driver = {
		/* [한국어] sysfs 와 로그에 보일 이름. */
		.name = "layerscape-pcie-ep",
		/* [한국어] 위 compatible 표. */
		.of_match_table = ls_pcie_ep_of_match,
		/* [한국어] sysfs 로 bind/unbind 를 막는다. */
		.suppress_bind_attrs = true,
	},
};
/* [한국어] probe 를 __init 로 두는 형태다. 부팅 후 probe 코드가 해제되므로,
 * 나중에 나타나는 장치는 이 드라이버가 붙지 않는다 — 내장 장치 전용이라는 뜻이다. */
builtin_platform_driver_probe(ls_pcie_ep_driver, ls_pcie_ep_probe);
