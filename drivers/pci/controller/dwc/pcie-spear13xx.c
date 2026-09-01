// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for ST Microelectronics SPEAr13xx SoCs
 *
 * SPEAr13xx PCIe Glue Layer Source Code
 *
 * Copyright (C) 2010-2014 ST Microelectronics
 * Pratyush Anand <pratyush.anand@gmail.com>
 * Mohit Kumar <mohit.kumar.dhaka@gmail.com>
 */

/* [한국어] clk_prepare_enable() / devm_clk_get(). 이 드라이버는 DWC 코어의
 * REQ_RES capability 를 쓰지 않고 클록을 직접 다룬다. */
/*
 * [한국어 설명] ST SPEAr13xx SoC 의 DesignWare PCIe 글루 계층 (pcie-spear13xx.c)
 *
 * === 파일의 역할 ===
 * ST Microelectronics 의 SPEAr13xx SoC 에 들어간 PCIe 컨트롤러를 다룬다.
 * 컨트롤러 IP 자체는 Synopsys DesignWare 이므로, 이 파일은 그 공용 코어에
 * SoC 고유 부분만 끼워 넣는 글루 계층이다. 파일 이름 옆의 상류 주석도
 * 스스로를 "PCIe Glue Layer" 라 부른다.
 * 끼워 넣는 SoC 고유 부분은 네 가지다. 링크 훈련을 시작하는 방법
 * (애플리케이션 제어 레지스터에 네 비트), 링크 상태를 읽는 방법(상태
 * 레지스터의 비트 하나), 인터럽트를 받아 MSI 를 코어에 넘기는 핸들러,
 * 그리고 config 헤더를 손보는 host_init 훅.
 * 이 파일에서 가장 특징적인 것은 **애플리케이션 레지스터를 구조체로
 * 매핑** 하는 방식이다. 오프셋 상수를 스물한 개 정의하는 대신
 * struct pcie_app_reg 를 선언하고 필드 이름으로 접근하며, 각 필드 옆의
 * cr0~cr20 주석이 벤더 문서의 레지스터 번호와의 대응을 알려 준다. 실제로
 * 이름으로 쓰이는 필드는 다섯(app_ctrl_0, app_status_1, int_sts, int_clr,
 * int_mask)뿐이고 나머지 열여섯은 배치를 맞추기 위해 존재한다.
 * 그리고 이 창이 별도 reg 항목이 아니라 **DBI 창 안의 고정 오프셋 0x2000**
 * 에 있다는 점도 특이하다. 그래서 app_base 계산이 probe 가 아니라
 * host_init 에서 이루어진다 — DBI 매핑을 DWC 코어가 해 주기 때문이다.
 * 2010~2014년 코드라는 흔적도 여럿이다. 클록을 코어의 REQ_RES capability 에
 * 맡기지 않고 직접 다루고, dw_pcie 를 내장하지 않고 포인터로 두며,
 * 하드웨어 결함을 만나면 BUG_ON 으로 시스템을 세운다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 PCI 스택에서 이 파일은 맨 아래, 하드웨어 바로 위에 있다.
 * 위로는 pcie-designware-host.c 의 dw_pcie_host_init() 이 초기화를 지휘하고,
 * 그 위로 PCI 코어의 pci_host_probe() 가 버스를 열거한다.
 * 제어 흐름은 위임과 콜백의 왕복이다.
 *   probe → 구조체 둘 할당 → PHY·클록 확보 → drvdata 연결
 *     → spear13xx_add_pcie_port() → IRQ 등록 → dw_pcie_host_init()
 *        → pp->ops->init = host_init(app_base 계산, 읽기 크기 제한,
 *          벤더/장치 ID 덮어쓰기, MSI 마스크 열기)
 *        → pci->ops->start_link = start_link(LTSSM 켜기)
 *        → pci->ops->link_up = link_up 을 폴링하며 대기
 * 그 뒤 동작 중에는 인터럽트 경로만 남는다. 하드웨어 인터럽트가
 * spear13xx_pcie_irq_handler() 로 들어오고, MSI 면 dw_handle_msi_irq() 로
 * 넘긴 뒤 상태 비트를 지운다.
 * 실행 컨텍스트는 둘이다. probe 와 네 콜백 중 셋은 프로세스 컨텍스트이고,
 * IRQ 핸들러만 하드 IRQ 다. link_up 은 코어의 폴링 루프 안에서 불리므로
 * 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-designware.h 의 struct dw_pcie / dw_pcie_ops / dw_pcie_rp /
 * dw_pcie_host_ops 규약과 pcie-designware-host.c 의 dw_pcie_host_init(),
 * 그리고 MSI 를 실제로 처리하는 dw_handle_msi_irq().
 * 옆쪽: DBI 접근자 dw_pcie_readw_dbi/writew_dbi 와 dw_pcie_find_capability().
 * DBI 는 읽기 전용 보호를 우회해 config 헤더를 고칠 수 있는 창이며,
 * 이 파일이 벤더/장치 ID 를 덮어쓸 수 있는 근거다.
 * 아래쪽: readl/writel(애플리케이션 창), PHY 서브시스템(devm_phy_get,
 * phy_init), 클록 서브시스템(devm_clk_get, clk_prepare_enable),
 * 그리고 platform_get_irq / devm_request_irq.
 * DT 바인딩: compatible 은 "st,spear1340-pcie" 하나이며, 선택 속성으로
 * "st,pcie-is-gen1" 을 보고 pci->max_link_speed 를 1 로 제한한다.
 * 공유 상태: struct spear13xx_pcie 하나(필드 넷)와 그것이 가리키는
 * struct dw_pcie. 둘을 잇는 것은 spear13xx_pcie->pci 포인터와 drvdata 이며,
 * 그래서 to_spear13xx_pcie 매크로가 container_of 가 아니라 dev_get_drvdata 다.
 * 전역 변수는 없고 상수 표 셋(host_ops, dw_pcie_ops, of_match)만 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct spear13xx_pcie: 필드 넷. pci 는 공용 객체 포인터(내장이 아니다),
 *   app_base 는 DBI+0x2000 의 애플리케이션 창, phy 와 clk 는 probe 가
 *   확보하는 자원이다.
 * - struct pcie_app_reg: cr0~cr20 을 그대로 옮긴 21개 필드. 오프셋 상수를
 *   쓰지 않고 필드 이름으로 접근하기 위한 배치이며, 모든 레지스터가
 *   4바이트 간격으로 빈틈없이 이어져 있어야 성립한다.
 * - spear13xx_pcie_start_link(): 네 비트를 통째로 써서 LTSSM 을 켠다.
 *   31번 비트만 (u32)1 로 캐스팅하는데, int 인 1 을 31비트 밀면 부호 비트를
 *   침범해 정의되지 않은 동작이 되기 때문이다.
 * - spear13xx_pcie_link_up(): 비트 하나로 판정한다. 물리 계층과 데이터 링크
 *   계층을 따로 노출하지 않는 SoC 라 두 비트를 AND 로 묶을 필요가 없다.
 * - spear13xx_pcie_irq_handler(): 상태를 읽고 → MSI 를 코어에 넘기고 →
 *   읽은 값을 되써서 지운다. 처리 뒤에 지우는 순서라 그 사이에 도착한
 *   인터럽트를 놓치지 않는다.
 * - spear13xx_pcie_enable_interrupts(): 읽기-수정-쓰기로 MSI 비트만 더한다.
 *   start_link 가 통째로 쓰는 것과 대비된다.
 * - spear13xx_pcie_host_init(): app_base 계산, 최대 읽기 요청 크기를
 *   128바이트로 낮추기(하드웨어 한계와 capability 기본값의 불일치),
 *   벤더/장치 ID 를 ST 의 값으로 덮어쓰기, MSI 마스크 열기.
 * - spear13xx_add_pcie_port(): IRQ 를 IRQF_SHARED | IRQF_NO_THREAD 로 걸고,
 *   pp->msi_irq[0] = -ENODEV 로 코어가 MSI 전용 인터럽트를 따로 잡지 않게
 *   막는다. 이 SoC 는 MSI 도 같은 공유 인터럽트로 받기 때문이다.
 * - spear13xx_pcie_probe(): 구조체 둘 할당 → PHY(-EPROBE_DEFER 를 info 로
 *   구분해 로그) → 클록 직접 관리 → gen1 제한 → drvdata → 포트 추가.
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - spear13xx_pcie_irq_handler() 는 IRQF_SHARED 로 등록하면서도 상태가 0 일
 *   때 IRQ_NONE 을 반환하지 않고 언제나 IRQ_HANDLED 를 답한다.
 * - probe 가 phy_init() 의 반환값을 확인하지 않으며, 그 짝인 phy_exit() 가
 *   이 파일 어디에도 없다.
 * - remove 함수가 없고 builtin_platform_driver 로 등록되며
 *   suppress_bind_attrs 가 켜져 있다. 즉 이 드라이버는 뗄 수 없다.
 *
 * === NVMe 관점 ===
 * 직접적인 코드 접점은 없다. 다만 host_init 이 최대 읽기 요청 크기를
 * 128바이트로 낮추는 대목은 NVMe 독자에게 의미가 있다. NVMe 컨트롤러는
 * 큰 DMA 읽기를 선호하는데, 이 컨트롤러 뒤에 붙으면 요청이 128바이트
 * 단위로 쪼개져 트랜잭션 수가 늘고 대역폭이 떨어진다. 하드웨어의 한계가
 * 상위 프로토콜의 성능으로 그대로 이어지는 예다.
 */

#include <linux/clk.h>
/* [한국어] devm_request_irq() 와 IRQF_ 플래그, irqreturn_t. */
#include <linux/interrupt.h>
/* [한국어] 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/init.h>
/* [한국어] of_property_read_bool() — DT 의 st,pcie-is-gen1 을 확인한다. */
#include <linux/of.h>
/* [한국어] PCI_CAP_ID_EXP, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_READRQ, PCI_VENDOR_ID 등. */
#include <linux/pci.h>
/* [한국어] devm_phy_get() / phy_init(). PHY 를 별도 드라이버로 두는 구조다. */
#include <linux/phy/phy.h>
/* [한국어] platform_get_irq(), platform_set_drvdata(), builtin_platform_driver. */
#include <linux/platform_device.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/resource.h>

/* [한국어] DesignWare PCIe 코어. struct dw_pcie / dw_pcie_rp / dw_pcie_ops /
 * dw_pcie_host_ops, dw_pcie_host_init(), DBI 접근자, dw_handle_msi_irq(). */
#include "pcie-designware.h"

struct spear13xx_pcie {
	/* [한국어] DWC 코어가 다루는 공용 컨트롤러 객체.
	 * 설정자: probe 가 따로 할당해 연결한다. 다른 DWC 드라이버들이 dw_pcie 를
	 *   **내장** 하는 것과 달리 이 파일은 포인터로 둔다 — 그래서 아래
	 *   to_spear13xx_pcie 매크로가 container_of 가 아니라 drvdata 조회다.
	 * 읽는 자: DWC 코어 전체와 이 파일의 콜백들.
	 * 값 범위: 유효한 dw_pcie 포인터.
	 * 동기화: probe 시 정해지고 이후 불변. */
	struct dw_pcie		*pci;
	/* [한국어] SoC 가 DWC IP 바깥에 덧붙인 애플리케이션 레지스터 창의 시작 주소.
	 * 설정자: host_init 이 pci->dbi_base + 0x2000 으로 계산한다. 별도 reg 항목이
	 *   아니라 DBI 창 안의 고정 오프셋이라는 점이 이 SoC 의 특징이다.
	 * 읽는 자: start_link / link_up / irq_handler / enable_interrupts.
	 * 값 범위: 유효한 __iomem 포인터. struct pcie_app_reg 로 해석한다.
	 * 동기화: host_init 에서 한 번 쓰고 이후 읽기만 한다. */
	void __iomem		*app_base;
	/* [한국어] PCIe PHY. 별도 드라이버가 소유한다.
	 * 설정자: probe 의 devm_phy_get(dev, "pcie-phy").
	 * 읽는 자: probe 의 phy_init() 한 곳뿐이다.
	 * 값 범위: 유효한 phy 포인터. 실패는 IS_ERR 로 걸러진다.
	 * 동기화: probe 시 정해진다. */
	struct phy		*phy;
	/* [한국어] 컨트롤러 클록.
	 * 설정자: probe 의 devm_clk_get(dev, NULL) — 이름 없이 첫 클록을 받는다.
	 * 읽는 자: probe 의 활성화와 실패 경로의 비활성화.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: probe 시 정해진다. */
	struct clk		*clk;
};

/* [한국어] 애플리케이션 레지스터 창을 통째로 구조체로 매핑한다. 오프셋 상수를
 * 일일이 정의하는 대신 필드 이름으로 접근하는 방식으로, 옆의 cr0~cr20
 * 주석이 벤더 문서의 레지스터 번호와의 대응을 알려 준다.
 * 이 방식이 성립하려면 모든 레지스터가 4바이트 간격으로 빈틈없이 이어져
 * 있어야 하며, 실제로 cr0~cr20 이 그렇게 배치되어 있다. */
struct pcie_app_reg {
	/* [한국어] 애플리케이션 제어 0. LTSSM 활성화와 장치 종류를 여기서 정한다.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	app_ctrl_0;		/* cr0 */
	/* [한국어] 애플리케이션 제어 1.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	app_ctrl_1;		/* cr1 */
	/* [한국어] 애플리케이션 상태 0.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	app_status_0;		/* cr2 */
	/* [한국어] 애플리케이션 상태 1. 링크 업 비트가 여기 있다.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	app_status_1;		/* cr3 */
	/* [한국어] 메시지 상태.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	msg_status;		/* cr4 */
	/* [한국어] 메시지 페이로드.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	msg_payload;		/* cr5 */
	/* [한국어] 인터럽트 상태. IRQ 핸들러가 가장 먼저 읽는 레지스터다.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	int_sts;		/* cr6 */
	/* [한국어] 인터럽트 해제. 상태 레지스터에서 읽은 값을 그대로 되써서 지운다.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	int_clr;		/* cr7 */
	/* [한국어] 인터럽트 마스크. MSI 인터럽트를 여는 비트가 여기 있다.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	int_mask;		/* cr8 */
	/* [한국어] 마스터 버스 관련 잡다한 설정.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	mst_bmisc;		/* cr9 */
	/* [한국어] PHY 제어.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	phy_ctrl;		/* cr10 */
	/* [한국어] PHY 상태.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	phy_status;		/* cr11 */
	/* [한국어] 컨트롤러 디버그 정보 0.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	cxpl_debug_info_0;	/* cr12 */
	/* [한국어] 컨트롤러 디버그 정보 1.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	cxpl_debug_info_1;	/* cr13 */
	/* [한국어] 벤더 정의 메시지 제어 0.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	ven_msg_ctrl_0;		/* cr14 */
	/* [한국어] 벤더 정의 메시지 제어 1.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	ven_msg_ctrl_1;		/* cr15 */
	/* [한국어] 벤더 정의 메시지 데이터 0.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	ven_msg_data_0;		/* cr16 */
	/* [한국어] 벤더 정의 메시지 데이터 1.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	ven_msg_data_1;		/* cr17 */
	/* [한국어] 벤더 정의 MSI 0.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	ven_msi_0;		/* cr18 */
	/* [한국어] 벤더 정의 MSI 1.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	ven_msi_1;		/* cr19 */
	/* [한국어] 마스터 읽기 관련 잡다한 설정. 이 파일에서 이름으로 접근하는 필드는
	 * app_ctrl_0 / app_status_1 / int_sts / int_clr / int_mask 다섯뿐이고,
	 * 나머지는 구조체의 배치를 맞추기 위해 존재한다.
	 * 설정자: 하드웨어 또는 이 드라이버의 writel.
	 * 읽는 자: 아래 함수들이 &app_reg->필드 로 주소를 얻어 접근한다.
	 * 값 범위: 32비트 레지스터. 벤더 문서에만 있는 비트 배치라 이 트리에서
	 *   확인할 수 없다.
	 * 동기화: probe 와 IRQ 핸들러가 나눠 쓰지만 겹치는 필드가 없어 락이 없다. */
	u32	mst_rmisc;		/* cr20 */
};

/* CR0 ID */
/* [한국어] LTSSM(링크 훈련 상태 기계)을 켜는 비트의 **번호**. 아래에서 1 << 로
 * 마스크를 만든다 — 값이 아니라 위치를 상수로 두는 방식이다. */
#define APP_LTSSM_ENABLE_ID			3
/* [한국어] 장치 종류를 루트 컴플렉스로 지정하는 필드. 이쪽은 이미 시프트된 값이라
 * 위 상수들과 표기가 섞여 있다. */
#define DEVICE_TYPE_RC				(4 << 25)
/* [한국어] 기타 제어 활성화 비트 번호. */
#define MISCTRL_EN_ID				30
/* [한국어] 주소 변환 활성화 비트 번호(31). 부호 있는 1 을 31비트 밀면 정의되지 않은
 * 동작이라, 아래 코드가 여기만 (u32)1 로 캐스팅한다. */
#define REG_TRANSLATION_ENABLE			31

/* CR3 ID */
/* [한국어] 데이터 링크 계층이 올라왔음을 나타내는 비트. app_status_1(cr3)에 있다. */
#define XMLH_LINK_UP				(1 << 6)

/* CR6 */
/* [한국어] MSI 인터럽트가 도착했음을 나타내는 비트. int_sts 와 int_mask 양쪽에서
 * 같은 자리를 쓴다. */
#define MSI_CTRL_INT				(1 << 26)

/* [한국어] dw_pcie 에서 이 드라이버의 상태를 되찾는다. container_of 가 아니라
 * drvdata 조회인 것은 dw_pcie 를 내장하지 않고 포인터로 두었기 때문이다.
 * probe 의 platform_set_drvdata() 가 그 짝이다. */
#define to_spear13xx_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어]
 * spear13xx_pcie_start_link - 애플리케이션 제어 레지스터에 네 비트를 써서 링크 훈련을 시작한다
 *
 * @pci: DWC 코어가 넘긴 공용 컨트롤러 객체.
 * @return: 언제나 0.
 *
 * 한 번의 writel 로 네 가지를 동시에 정한다 — 장치 종류를 루트 컴플렉스로,
 * 기타 제어 활성화, LTSSM 활성화, 주소 변환 활성화.
 *
 * 읽기-수정-쓰기가 아니라 통째로 쓰는 것이 다른 함수들과 대비된다.
 * 이 시점은 초기 설정이라 그 레지스터의 다른 비트가 모두 0 이어야 하기
 * 때문이다. 반면 spear13xx_pcie_enable_interrupts() 는 이미 설정된 마스크를
 * 보존해야 해서 읽기-수정-쓰기를 쓴다.
 *
 * REG_TRANSLATION_ENABLE 만 (u32)1 로 캐스팅하는 이유가 있다. 그 비트가
 * 31번이라 int 인 1 을 그만큼 밀면 부호 비트를 침범해 정의되지 않은 동작이 된다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 훈련 실패는 이후 link_up 폴링의 시간 초과로 드러난다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_start_link() → pci->ops->start_link
 *     → [이 함수] → writel(app_ctrl_0)
 */
static int spear13xx_pcie_start_link(struct dw_pcie *pci)
{
	/* [한국어] 드라이버 상태를 되찾고, */
	struct spear13xx_pcie *spear13xx_pcie = to_spear13xx_pcie(pci);
	/* [한국어] 애플리케이션 레지스터 창을 구조체로 본다. */
	struct pcie_app_reg __iomem *app_reg = spear13xx_pcie->app_base;

	/* enable ltssm */
	/* [한국어] 네 비트를 한 번에 써서 LTSSM 을 켠다. 읽기-수정-쓰기가 아니라 통째로
	 * 쓰는 것은, 이 시점에 그 레지스터의 다른 비트가 모두 0 이어야 하는
	 * 초기 설정이기 때문이다. */
	writel(DEVICE_TYPE_RC | (1 << MISCTRL_EN_ID)
			| (1 << APP_LTSSM_ENABLE_ID)
			/* [한국어] 31번 비트만 (u32)1 로 캐스팅한다. int 인 1 을 31비트 밀면 부호 비트를
			 * 침범해 정의되지 않은 동작이 되기 때문이다. */
			| ((u32)1 << REG_TRANSLATION_ENABLE),
			&app_reg->app_ctrl_0);

	/* [한국어] 언제나 성공을 보고한다. 훈련이 실제로 성공했는지는 link_up 이 답한다. */
	return 0;
}

/* [한국어]
 * spear13xx_pcie_irq_handler - 애플리케이션 인터럽트를 받아 MSI 를 DWC 코어로 넘긴다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: 등록 시 넘겨 둔 드라이버 상태.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 상태를 읽고, MSI 면 코어에 넘기고, 읽은 값을 되써서 지운다.
 *
 * 처리한 **뒤에** 지우는 순서가 중요하다. 먼저 지우면 그 사이에 도착한
 * 인터럽트를 놓칠 수 있다. 읽은 값을 그대로 되쓰는 것은 W1C(1을 쓰면 지워짐)
 * 방식이라, 읽은 시점 이후에 새로 선 비트는 건드리지 않는다.
 *
 * BUG_ON 은 "결코 일어나지 않아야 하는" 조건에만 쓰는 강한 수단이다.
 * MSI 가 컴파일에서 빠졌는데 MSI 인터럽트가 왔다는 것은 하드웨어나 설정이
 * 근본적으로 어긋난 상황이라는 판단이다.
 *
 * [상류 코드 관찰] IRQF_SHARED 로 등록하면서도 상태가 0 일 때 IRQ_NONE 을
 * 반환하지 않고 언제나 IRQ_HANDLED 를 답한다. 공유 인터럽트에서 남의 것까지
 * 자기 것이라고 답하는 셈이라, 공유 상대의 인터럽트가 처리되지 않을 때
 * 커널이 그것을 감지하지 못한다. 코드는 고치지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 하드 IRQ. IRQF_NO_THREAD 로 강제 스레드화를 막아 두었다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수] → dw_handle_msi_irq()
 */
static irqreturn_t spear13xx_pcie_irq_handler(int irq, void *arg)
{
	/* [한국어] 등록 시 넘겨 둔 드라이버 상태. */
	struct spear13xx_pcie *spear13xx_pcie = arg;
	/* [한국어] 애플리케이션 레지스터 창. */
	struct pcie_app_reg __iomem *app_reg = spear13xx_pcie->app_base;
	/* [한국어] 공용 컨트롤러 객체. */
	struct dw_pcie *pci = spear13xx_pcie->pci;
	/* [한국어] 그 안의 루트 포트 객체. MSI 처리에 넘긴다. */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 인터럽트 상태. */
	unsigned int status;

	/* [한국어] 어떤 인터럽트가 왔는지 읽는다. */
	status = readl(&app_reg->int_sts);

	/* [한국어] MSI 라면, */
	if (status & MSI_CTRL_INT) {
		/* [한국어] MSI 가 컴파일에서 빠졌는데 MSI 인터럽트가 왔다는 것은 있을 수 없는
		 * 상황이므로 여기서 멈춘다. BUG_ON 은 시스템을 세우는 강한 수단이라
		 * "결코 일어나지 않아야 하는" 조건에만 쓴다. */
		BUG_ON(!IS_ENABLED(CONFIG_PCI_MSI));
		/* [한국어] DWC 코어의 MSI 처리기로 넘긴다. 실제 벡터 해석과 하위 IRQ 호출은
		 * 코어가 한다. */
		dw_handle_msi_irq(pp);
	}

	/* [한국어] 읽은 값을 그대로 해제 레지스터에 되쓴다. 세워진 비트만 지우는 W1C
	 * 방식이며, 처리한 뒤에 지우는 순서라 그 사이에 도착한 인터럽트를
	 * 놓치지 않는다. */
	writel(status, &app_reg->int_clr);

	/* [한국어] IRQF_SHARED 로 등록했지만 언제나 IRQ_HANDLED 를 반환한다.
	 * [상류 코드 관찰] 상태가 0 이어도 남의 인터럽트라고 답하지 않는다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * spear13xx_pcie_enable_interrupts - MSI 인터럽트 마스크를 연다
 *
 * @spear13xx_pcie: 이 드라이버의 상태.
 *
 * 마스크 레지스터에 MSI 비트를 더한다. 읽기-수정-쓰기라 이미 열려 있던
 * 다른 비트를 보존하며, start_link 가 통째로 쓰는 것과 대비된다.
 *
 * CONFIG_PCI_MSI 가 꺼진 커널에서는 아무 일도 하지 않는다. IS_ENABLED 는
 * 컴파일 시점 상수라 그런 빌드에서는 이 함수의 몸통이 통째로 사라진다.
 *
 * host_init 의 마지막 단계에서 불린다. 그 시점이면 DWC 코어의 MSI 준비가
 * 끝나 있어, 열자마자 인터럽트가 와도 처리할 수 있다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   spear13xx_pcie_host_init() → [이 함수] → readl/writel(int_mask)
 */
static void spear13xx_pcie_enable_interrupts(struct spear13xx_pcie *spear13xx_pcie)
{
	/* [한국어] 애플리케이션 레지스터 창. */
	struct pcie_app_reg __iomem *app_reg = spear13xx_pcie->app_base;

	/* Enable MSI interrupt */
	/* [한국어] MSI 가 컴파일에 포함된 경우에만, */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		/* [한국어] 마스크 레지스터에 MSI 비트를 더한다. 읽기-수정-쓰기라 다른 비트를
		 * 보존한다 — start_link 가 통째로 쓰는 것과 대비된다. */
		writel(readl(&app_reg->int_mask) |
				MSI_CTRL_INT, &app_reg->int_mask);
}

/* [한국어]
 * spear13xx_pcie_link_up - 애플리케이션 상태 레지스터에서 링크 업 비트를 읽는다
 *
 * @pci: DWC 코어가 넘긴 공용 컨트롤러 객체.
 * @return: 0 이 아니면 링크가 올라온 것.
 *
 * 비트 하나로 판정한다. 이 SoC 는 물리 계층과 데이터 링크 계층의 상태를
 * 따로 노출하지 않기 때문이며, 두 비트를 AND 로 묶는 다른 컨트롤러
 * (예: pcie-andes-qilai.c)와 대비되는 지점이다.
 *
 * DWC 코어가 dw_pcie_wait_for_link() 의 폴링 루프에서 반복해 부른다.
 *
 * 실행 컨텍스트: 폴링 루프. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_wait_for_link() → pci->ops->link_up → [이 함수]
 *     → readl(app_status_1)
 */
static bool spear13xx_pcie_link_up(struct dw_pcie *pci)
{
	/* [한국어] 드라이버 상태. */
	struct spear13xx_pcie *spear13xx_pcie = to_spear13xx_pcie(pci);
	/* [한국어] 애플리케이션 레지스터 창. */
	struct pcie_app_reg __iomem *app_reg = spear13xx_pcie->app_base;

	/* [한국어] 상태 레지스터의 링크 업 비트를 그대로 돌려준다. 이 SoC 는 물리 계층과
	 * 데이터 링크 계층을 따로 노출하지 않아 비트 하나로 판정한다. */
	return readl(&app_reg->app_status_1) & XMLH_LINK_UP;
}

/* [한국어]
 * spear13xx_pcie_host_init - 애플리케이션 창 주소를 정하고 config 헤더를 손본다
 *
 * @pp: DWC 코어의 루트 포트 객체.
 * @return: 언제나 0.
 *
 * 이 드라이버가 DWC 코어의 초기화에 끼워 넣는 유일한 훅이며, 네 가지를 한다.
 *
 * 첫째, app_base 를 pci->dbi_base + 0x2000 으로 정한다. 애플리케이션
 * 레지스터가 별도 reg 항목이 아니라 DBI 창 안의 고정 오프셋에 있다는 것이
 * 이 SoC 의 특징이고, DBI 매핑은 DWC 코어가 해 주므로 이 시점이 되어야
 * 주소를 알 수 있다. probe 가 아니라 host_init 에서 하는 이유가 그것이다.
 *
 * 둘째, 최대 읽기 요청 크기를 128바이트로 낮춘다. 함수 안의 영어 주석대로
 * 이 컨트롤러는 128바이트까지만 지원하는데 capability 기본값이 512바이트라,
 * 그대로 두면 상대가 감당할 수 없는 크기를 요청한다. 필드 값 0 이 128바이트다.
 *
 * 셋째, 벤더/장치 ID 를 ST 의 값으로 덮어쓴다. DBI 창이 읽기 전용 보호를
 * 우회하는 통로라 가능한 조작이며, 그러지 않으면 Synopsys 의 기본 ID 가
 * 그대로 보인다.
 *
 * 넷째, MSI 마스크를 연다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 0 을 고정 반환한다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → pp->ops->init → [이 함수]
 *     → dw_pcie_find_capability() → dw_pcie_readw/writew_dbi()
 *     → spear13xx_pcie_enable_interrupts()
 */
static int spear13xx_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* [한국어] 루트 포트 객체에서 공용 객체를 얻고, */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 거기서 드라이버 상태를 되찾는다. */
	struct spear13xx_pcie *spear13xx_pcie = to_spear13xx_pcie(pci);
	/* [한국어] PCIe capability 의 위치를 찾는다. 아래에서 그 안의 Device Control 을
	 * 고치는 데 쓴다. */
	u32 exp_cap_off = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* [한국어] 읽고 고칠 값. */
	u32 val;

	/* [한국어] 애플리케이션 레지스터 창을 DBI 창의 0x2000 오프셋으로 정한다.
	 * 별도 reg 항목이 아니라 DBI 안에 있다는 점이 이 SoC 의 특징이며,
	 * 그래서 host_init 이 되어야 이 주소를 알 수 있다 — DBI 매핑을 DWC 코어가
	 * 해 주기 때문이다. */
	spear13xx_pcie->app_base = pci->dbi_base + 0x2000;

	/*
	 * this controller support only 128 bytes read size, however its
	 * default value in capability register is 512 bytes. So force
	 * it to 128 here.
	 */
	/* [한국어] Device Control 을 읽는다. */
	val = dw_pcie_readw_dbi(pci, exp_cap_off + PCI_EXP_DEVCTL);
	/* [한국어] 최대 읽기 요청 크기 필드를 0 으로 만든다. 위 영어 주석대로 이 컨트롤러는
	 * 128바이트까지만 지원하는데 capability 기본값이 512바이트라, 그대로 두면
	 * 상대가 더 큰 요청을 보내 문제가 된다. 필드 값 0 이 128바이트를 뜻한다. */
	val &= ~PCI_EXP_DEVCTL_READRQ;
	/* [한국어] 되쓴다. */
	dw_pcie_writew_dbi(pci, exp_cap_off + PCI_EXP_DEVCTL, val);

	/* [한국어] 벤더 ID 를 직접 써 넣는다. DBI 는 읽기 전용 보호를 우회해 config 헤더를
	 * 고칠 수 있는 창이라 이런 조작이 가능하다. */
	dw_pcie_writew_dbi(pci, PCI_VENDOR_ID, 0x104A);
	/* [한국어] 장치 ID 도 마찬가지. 하드웨어가 기본값으로 Synopsys 의 ID 를 보고하므로,
	 * ST 의 ID(0x104A/0xCD80)로 바꿔 준다. */
	dw_pcie_writew_dbi(pci, PCI_DEVICE_ID, 0xCD80);

	/* [한국어] MSI 인터럽트 마스크를 연다. 이 시점이면 DWC 코어의 MSI 준비가 끝나 있다. */
	spear13xx_pcie_enable_interrupts(spear13xx_pcie);

	/* [한국어] 성공. */
	return 0;
}

static const struct dw_pcie_host_ops spear13xx_pcie_host_ops = {
	/* [한국어] 이 드라이버가 채우는 유일한 훅. post_init 은 두지 않는다. */
	.init = spear13xx_pcie_host_init,
};

/* [한국어]
 * spear13xx_add_pcie_port - 인터럽트를 걸고 DWC 호스트 초기화를 시작한다
 *
 * @spear13xx_pcie: 이 드라이버의 상태.
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, platform_get_irq() 나 devm_request_irq(),
 *   dw_pcie_host_init() 의 오류.
 *
 * probe 에서 떼어 낸 부분이다. 하는 일은 IRQ 확보와 등록, 훅 연결,
 * 그리고 코어 위임 넷이다.
 *
 * 인터럽트 플래그 조합이 이 함수의 판단을 보여 준다. IRQF_SHARED 로 공유를
 * 허용하고 IRQF_NO_THREAD 로 강제 스레드화를 막는데, 핸들러가 레지스터 두 번
 * 접근과 MSI 중계만 하므로 스레드로 옮길 값어치가 없다는 뜻이다.
 *
 * pp->msi_irq[0] = -ENODEV 가 중요하다. DWC 코어가 MSI 전용 인터럽트를 따로
 * 잡지 않게 막는 표시로, 이 SoC 는 MSI 도 위 공유 인터럽트 하나로 함께
 * 받기 때문이다. 실제로 핸들러가 MSI_CTRL_INT 를 보고 dw_handle_msi_irq() 를
 * 직접 부른다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 모든 실패를 그대로 올려보내며, probe 가 그것을 받아 클록을 끈다.
 *
 * 호출 체인:
 *   spear13xx_pcie_probe() → [이 함수] → platform_get_irq()
 *     → devm_request_irq() → dw_pcie_host_init()
 */
static int spear13xx_add_pcie_port(struct spear13xx_pcie *spear13xx_pcie,
				   struct platform_device *pdev)
{
	/* [한국어] 공용 컨트롤러 객체. */
	struct dw_pcie *pci = spear13xx_pcie->pci;
	/* [한국어] 그 안의 루트 포트 객체. */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] DT 의 첫 인터럽트를 가져온다. */
	pp->irq = platform_get_irq(pdev, 0);
	/* [한국어] 실패하면, */
	if (pp->irq < 0)
		return pp->irq;

	/* [한국어] 핸들러를 건다. devm_ 이라 해제는 자동이다. */
	ret = devm_request_irq(dev, pp->irq, spear13xx_pcie_irq_handler,
			       /* [한국어] IRQF_SHARED 로 공유를 허용하고, IRQF_NO_THREAD 로 강제 스레드화를 막는다.
			        * 핸들러가 레지스터 두 번 접근과 MSI 중계만 하므로 스레드로 옮길 값어치가
			        * 없다는 판단이다. */
			       IRQF_SHARED | IRQF_NO_THREAD,
			       "spear1340-pcie", spear13xx_pcie);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 어느 IRQ 였는지 남기고, */
		dev_err(dev, "failed to request irq %d\n", pp->irq);
		return ret;
	}

	/* [한국어] 루트 포트 훅 표를 연결한다. */
	pp->ops = &spear13xx_pcie_host_ops;
	/* [한국어] MSI IRQ 를 -ENODEV 로 표시한다. DWC 코어가 별도의 MSI 인터럽트를 잡지
	 * 않게 하는 것으로, 이 SoC 는 MSI 도 위 공유 인터럽트 하나로 함께 받기
	 * 때문이다. */
	pp->msi_irq[0] = -ENODEV;

	/* [한국어] 여기서부터는 DWC 코어가 맡는다. */
	ret = dw_pcie_host_init(pp);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 기록하고, */
		dev_err(dev, "failed to initialize host\n");
		return ret;
	}

	/* [한국어] 성공. */
	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	/* [한국어] 링크 상태를 읽는 콜백. */
	.link_up = spear13xx_pcie_link_up,
	/* [한국어] 링크 훈련을 시작하는 콜백. stop_link 가 없어 링크를 되돌릴 수단이 없다. */
	.start_link = spear13xx_pcie_start_link,
};

/* [한국어]
 * spear13xx_pcie_probe - 자원을 모으고 DWC 코어에 넘긴다
 *
 * @pdev: 매치된 플랫폼 장치.
 * @return: 0 = 성공, -ENOMEM, -EPROBE_DEFER, 또는 각 단계의 오류.
 *
 * 구조체를 **둘로 나눠** 할당하는 점이 다른 DWC 드라이버와 다르다.
 * 대부분은 자기 구조체 안에 dw_pcie 를 내장해 container_of 로 오가지만,
 * 이 파일은 포인터로 두고 drvdata 로 역방향을 만든다(to_spear13xx_pcie 매크로).
 * 그래서 platform_set_drvdata() 를 IRQ 등록 **전에** 해야 한다 —
 * 핸들러가 그 값을 쓰기 때문이다.
 *
 * PHY 획득의 오류 처리가 세심하다. -EPROBE_DEFER 는 PHY 드라이버가 아직
 * 준비되지 않았다는 뜻으로 나중에 다시 시도되므로, err 가 아니라 info 로
 * 남긴다. 부팅 로그가 오류로 채워지는 것을 막는 흔한 관용구다.
 *
 * 클록을 직접 다루는 것도 옛 방식이다. 최근 DWC 드라이버들은
 * dw_pcie_cap_set(REQ_RES) 로 코어에 맡기지만, 이 파일은 devm_clk_get 과
 * clk_prepare_enable 을 직접 부르고 실패 경로에서 clk_disable_unprepare 로
 * 되돌린다.
 *
 * [상류 코드 관찰] 두 가지가 눈에 띈다. phy_init() 의 반환값을 확인하지 않고,
 * 그 짝인 phy_exit() 가 이 파일 어디에도 없다. 되감기 라벨도 클록 하나뿐이다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe, 프로세스 컨텍스트.
 *
 * 에러 경로: PHY 와 클록 획득 실패는 그 자리에서 반환하고, 그 뒤의 실패만
 * fail_clk 로 모여 클록을 끈다.
 *
 * 호출 체인:
 *   builtin_platform_driver → 플랫폼 버스 매치 → [이 함수]
 *     → devm_kzalloc() ×2 → devm_phy_get() → phy_init()
 *     → devm_clk_get() → clk_prepare_enable()
 *     → platform_set_drvdata() → spear13xx_add_pcie_port()
 */
static int spear13xx_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 진단 메시지용 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 할당할 공용 객체. */
	struct dw_pcie *pci;
	/* [한국어] 할당할 드라이버 상태. */
	struct spear13xx_pcie *spear13xx_pcie;
	/* [한국어] DT 노드. 아래 gen1 속성 확인에 쓴다. */
	struct device_node *np = dev->of_node;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 드라이버 상태를 할당한다. */
	spear13xx_pcie = devm_kzalloc(dev, sizeof(*spear13xx_pcie), GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!spear13xx_pcie)
		return -ENOMEM;

	/* [한국어] 공용 객체를 **따로** 할당한다. 대부분의 DWC 드라이버가 자기 구조체 안에
	 * dw_pcie 를 내장하는 것과 다른 방식이다. */
	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] 실패하면, */
	if (!pci)
		return -ENOMEM;

	/* [한국어] DWC 코어가 쓸 device. */
	pci->dev = dev;
	/* [한국어] link_up / start_link 콜백 표. */
	pci->ops = &dw_pcie_ops;

	/* [한국어] 양방향 연결의 한쪽. 반대쪽은 아래 platform_set_drvdata 다. */
	spear13xx_pcie->pci = pci;

	/* [한국어] DT 에서 "pcie-phy" 라는 이름의 PHY 를 얻는다. */
	spear13xx_pcie->phy = devm_phy_get(dev, "pcie-phy");
	/* [한국어] 실패하면, */
	if (IS_ERR(spear13xx_pcie->phy)) {
		/* [한국어] 오류 코드를 꺼내, */
		ret = PTR_ERR(spear13xx_pcie->phy);
		/* [한국어] PHY 드라이버가 아직 준비되지 않은 것이면, */
		if (ret == -EPROBE_DEFER)
			/* [한국어] 오류가 아니라 정보로 남긴다. 나중에 다시 시도될 정상적인 상황이기 때문이다. */
			dev_info(dev, "probe deferred\n");
		else
			/* [한국어] 진짜 오류면 err 로 남긴다. */
			dev_err(dev, "couldn't get pcie-phy\n");
		/* [한국어] 어느 쪽이든 그 코드를 그대로 올려보낸다. */
		return ret;
	}

	/* [한국어] PHY 를 초기화한다.
	 * [상류 코드 관찰] 반환값을 확인하지 않는다. 실패해도 그대로 진행한다. */
	phy_init(spear13xx_pcie->phy);

	/* [한국어] 클록을 얻는다. 이름을 주지 않아 DT 의 첫 클록을 받는다. */
	spear13xx_pcie->clk = devm_clk_get(dev, NULL);
	/* [한국어] 실패하면, */
	if (IS_ERR(spear13xx_pcie->clk)) {
		/* [한국어] 기록하고, */
		dev_err(dev, "couldn't get clk for pcie\n");
		return PTR_ERR(spear13xx_pcie->clk);
	}
	/* [한국어] 클록을 켠다. DWC 코어의 REQ_RES capability 를 쓰지 않고 직접 다루는
	 * 옛 방식이다. */
	ret = clk_prepare_enable(spear13xx_pcie->clk);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 기록하고, */
		dev_err(dev, "couldn't enable clk for pcie\n");
		return ret;
	}

	/* [한국어] DT 가 Gen1 전용이라고 표시했으면, */
	if (of_property_read_bool(np, "st,pcie-is-gen1"))
		/* [한국어] 최대 링크 속도를 1 로 제한한다. 이후 DWC 코어가 이 값으로 협상 상한을 건다. */
		pci->max_link_speed = 1;

	/* [한국어] 반대 방향 연결. 이제 to_spear13xx_pcie 매크로가 동작한다.
	 * IRQ 를 걸기 **전에** 해야 하는데, 핸들러가 이 값을 쓰기 때문이다. */
	platform_set_drvdata(pdev, spear13xx_pcie);

	/* [한국어] IRQ 등록과 DWC 초기화를 한 함수에 맡긴다. */
	ret = spear13xx_add_pcie_port(spear13xx_pcie, pdev);
	/* [한국어] 실패하면, */
	if (ret < 0)
		/* [한국어] 켜 둔 클록을 되돌리러 간다. */
		goto fail_clk;

	/* [한국어] 성공. */
	return 0;

/* [한국어] 클록 되감기 라벨. */
fail_clk:
	/* [한국어] 클록을 끈다. devm_ 이 아닌 clk_prepare_enable 을 썼으므로 직접 되돌려야 한다.
	 * [상류 코드 관찰] phy_init() 에 대응하는 phy_exit() 는 여기에도, 어디에도 없다. */
	clk_disable_unprepare(spear13xx_pcie->clk);

	/* [한국어] 오류를 그대로 올려보낸다. */
	return ret;
}

static const struct of_device_id spear13xx_pcie_of_match[] = {
	/* [한국어] 이 드라이버가 맡는 유일한 compatible. */
	{ .compatible = "st,spear1340-pcie", },
	/* [한국어] 배열 끝. */
	{},
};

static struct platform_driver spear13xx_pcie_driver = {
	/* [한국어] 위 probe. */
	.probe		= spear13xx_pcie_probe,
	.driver = {
		/* [한국어] sysfs 와 로그에 보일 이름. */
		.name	= "spear-pcie",
		/* [한국어] 위 compatible 표. */
		.of_match_table = spear13xx_pcie_of_match,
		/* [한국어] sysfs 로 bind/unbind 를 막는다. 호스트 브리지를 런타임에 떼면 그 아래
		 * 모든 장치가 사라지기 때문이다. */
		.suppress_bind_attrs = true,
	},
};

/* [한국어] 모듈이 아니라 내장 드라이버로 등록한다. remove 함수가 없는 것도 같은
 * 맥락이다 — 이 드라이버는 뗄 수 없다. */
builtin_platform_driver(spear13xx_pcie_driver);
