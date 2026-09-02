// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Freescale Layerscape SoCs
 *
 * Copyright (C) 2014 Freescale Semiconductor.
 * Copyright 2021 NXP
 *
 * Author: Minghuan Lian <Minghuan.Lian@freescale.com>
 */

/*
 * [한국어 설명] Freescale/NXP Layerscape SoC 의 DesignWare PCIe 호스트 글루
 * (pci-layerscape.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare PCIe 코어를 Layerscape 계열 SoC 아홉 종에 붙이는
 * 글루 드라이버다. config 접근, ATU 설정, 버스 스캔은
 * pcie-designware-host.c 가 맡고, 이 파일은 SoC 고유의 것만 처리한다.
 *
 * **다른 글루들과 성격이 확연히 다르다.** 클럭도, PHY 도, 리셋도 다루지
 * 않는다 — 그런 것은 모두 펌웨어 몫이다. 대신 두 가지에 집중한다.
 *
 *   1) **DBI 레지스터 다듬기** — 컨트롤러가 리눅스가 기대하는 모습으로
 *      보이도록 헤더 타입과 오류 응답 방식을 고친다. 초기화가 이것뿐이다.
 *   2) **L2 진입·탈출** — 절전 시 링크를 L2 상태로 내리고 복귀 시 꺼낸다.
 *      이 파일 분량의 절반 이상이 여기에 쓰인다.
 *
 * === L2 를 다루는 방식이 SoC 마다 셋으로 갈린다 ===
 * PCIe 의 L2 는 링크 전원이 사실상 꺼진 상태다. 들어가려면 루트 포트가
 * PME_Turn_Off 메시지를 보내고 상대의 응답을 기다려야 하며, 나오려면
 * 링크를 다시 깨워야 한다. Layerscape 는 그 조작 창구가 세대마다 다르다.
 *
 *   [LS1021A] SCFG(시스템 설정) 레지스터로 한다.
 *     진입: PMXMTTURNOFF 비트를 세웠다 지운다.
 *     탈출: PEX 래퍼를 소프트 리셋한다.
 *   [LS1043A] 역시 SCFG 로 진입하되 레지스터가 다르고,
 *     탈출: PF LUT 창의 디버그 레지스터로 소프트 리셋을 건다.
 *   [그 밖의 세대] SCFG 없이 **PF LUT 창의 메시지 명령 레지스터** 하나로
 *     진입과 탈출을 모두 처리한다. 가장 정돈된 방식이다.
 *
 * 이 차이를 struct ls_pcie_drvdata 가 흡수한다 — SoC 마다 콜백 표와
 * 탈출 함수를 다르게 걸어 두고, 공통 코드는 그것을 부르기만 한다.
 *
 * === 눈여겨볼 관용: 폴링용 한 인자 접근자 ===
 * readx_poll_timeout 매크로는 **인자를 하나만 받는 접근 함수** 를 요구하는데,
 * 이 파일의 레지스터 읽기 함수는 드라이버 상태와 오프셋 둘을 받는다.
 * 그 간극을 ls_pcie_pf_lut_readl_addr 매크로가 메운다 — 오프셋만 받고,
 * **드라이버 상태는 호출 지점의 지역 변수 `pcie` 를 그대로 집어 온다.**
 * 그래서 이 매크로는 `pcie` 라는 이름의 변수가 보이는 자리에서만 쓸 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시:
 *   플랫폼 드라이버 코어 -> ls_pcie_probe()
 *     -> 매칭 데이터로 SoC 세대 판정 -> DBI 창 매핑
 *       -> 엔디언 판정 -> PF LUT 창 위치 계산
 *         -> (필요한 세대만) SCFG regmap 확보
 *           -> **브리지 모드인지 확인** — 아니면 -ENODEV 로 물러난다
 *             -> dw_pcie_host_init()
 *                -> 콜백 -> [이 파일] ls_pcie_host_init()
 *                   -> 오류 응답 방식 고치기 -> 다중 기능 비트 지우기
 *                      -> 메시지 TLP 버리기
 *
 * 절전 진입:
 *   PM 코어 -> ls_pcie_suspend_noirq() -> DWC 코어의 공용 구현
 *     -> 콜백 -> [이 파일] 세대별 send_turnoff_msg()
 *
 * 절전 복귀:
 *   PM 코어 -> ls_pcie_resume_noirq()
 *     -> **먼저** 세대별 exit_from_l2() 로 링크를 깨우고
 *       -> 그다음 DWC 코어의 공용 복귀 구현
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 인터럽트 핸들러가 없다.
 * 다만 SCFG 경로의 대기가 mdelay(바쁜 대기)라 그 구간은 CPU 를 놓지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어가 dw_pcie_host_init() 을 거쳐 버스를 스캔한다.
 * 아래쪽: pcie-designware-host.c. 접점이 dw_pcie_host_ops 세 벌인데,
 *   **세 벌이 init 은 같고 pme_turn_off 만 다르다.** 그것이 이 파일의
 *   세대 차이를 보여 주는 가장 간명한 지표다.
 * 옆쪽: syscon/regmap 계층(SCFG 접근). 이 트리에 없다.
 *   drivers/pci/pci.h 의 PCIE_PME_TO_L2_TIMEOUT_US(10ms)를 대기 상한으로 쓴다.
 *
 * 데이터 흐름:
 *   디바이스 트리(매칭 데이터, "regs" 창, big-endian 여부, SCFG phandle)
 *     -> probe -> struct ls_pcie
 *   PF LUT 창은 별도 자원이 아니라 **DBI 창의 특정 오프셋** 이다 —
 *   세대마다 그 오프셋이 0, 0x10000, 0xc0000 으로 다르다.
 *
 * 공유 상태: struct ls_pcie 하나이며 probe 이후 불변이다.
 *
 * === NVMe 관점 ===
 * NVMe SSD 가 붙어 있으면 시스템 절전 때 이 파일의 L2 진입 경로를 탄다.
 * PME_Turn_Off 를 보낸 뒤 응답을 기다리는데, SCFG 경로에는 **응답을 확인할
 * 레지스터가 아예 없어** 옆의 상류 주석대로 10ms 를 무작정 기다린다.
 * 반대로 복귀는 exit_from_l2 가 먼저 돌아 링크를 깨운 뒤에야 NVMe 컨트롤러
 * 재초기화가 시작된다.
 *
 * === 주요 함수/구조체 요약 ===
 * ls_pcie_host_init()            : 초기화 전부. DBI 세 곳을 고치는 것이 끝이다.
 * ls_pcie_send_turnoff_msg()     : PF LUT 방식의 L2 진입. 완료를 폴링으로 확인한다.
 * ls_pcie_exit_from_l2()         : 그 짝. 10ms 상한은 실측으로 정한 값이다.
 * scfg_pcie_send_turnoff_msg()   : SCFG 방식의 L2 진입. 확인할 방법이 없어 무작정 기다린다.
 * ls1043a_pcie_exit_from_l2()    : 디버그 레지스터로 소프트 리셋. 읽기-수정-쓰기 네 번.
 * ls_pcie_probe()                : 세대 판정과 자원 확보, 그리고 브리지 모드 확인.
 * struct ls_pcie                 : 이 드라이버의 상태 전부.
 * struct ls_pcie_drvdata         : SoC 세대 차이를 흡수하는 표.
 */

/* [한국어] mdelay — SCFG 경로의 바쁜 대기에 쓴다. */
#include <linux/delay.h>
/* [한국어] 기본 커널 매크로들. */
#include <linux/kernel.h>
/* [한국어] [상류 코드 관찰] 이 헤더의 이름(irqreturn_t 등)을 쓰는 곳을 이 파일에서
 * 찾지 못했다. 이 드라이버에는 인터럽트 핸들러가 없다. */
#include <linux/interrupt.h>
/* [한국어] [상류 코드 관찰] __init 계열 표시를 쓰는 곳을 이 파일에서 찾지 못했다. */
#include <linux/init.h>
/* [한국어] readx_poll_timeout — PF LUT 방식의 완료 대기에 쓴다. */
#include <linux/iopoll.h>
/* [한국어] MODULE_ 계열 선언용. 이 드라이버는 모듈로도 빌드된다. */
#include <linux/module.h>
/* [한국어] [상류 코드 관찰] 이 헤더의 이름을 쓰는 곳을 이 파일에서 찾지 못했다. */
#include <linux/of_pci.h>
/* [한국어] [상류 코드 관찰] 이 헤더의 이름을 쓰는 곳을 이 파일에서 찾지 못했다. */
#include <linux/of_platform.h>
/* [한국어] [상류 코드 관찰] 이 헤더의 이름을 쓰는 곳을 이 파일에서 찾지 못했다. */
#include <linux/of_address.h>
/* [한국어] PCI_HEADER_TYPE 등 config 공간 상수. */
#include <linux/pci.h>
/* [한국어] platform_device 와 자원 확보. */
#include <linux/platform_device.h>
/* [한국어] struct resource — probe 가 DBI 창 자원을 받는 데 쓴다. */
#include <linux/resource.h>
/* [한국어] syscon_regmap_lookup_by_phandle_args — SCFG 를 얻는 통로. */
#include <linux/mfd/syscon.h>
/* [한국어] regmap_write_bits — SCFG 레지스터 접근. */
#include <linux/regmap.h>

/* [한국어] PCI 서브시스템 **내부** 헤더. PCIE_PME_TO_L2_TIMEOUT_US(10ms)가
 * 여기(:122) 있어 L2 대기 상한으로 쓴다. */
#include "../../pci.h"
/* [한국어] DWC 코어의 자료구조와 DBI 도우미. */
#include "pcie-designware.h"

/* PEX Internal Configuration Registers */
/* [한국어] 심볼 타이머·필터 마스크 레지스터. 어떤 TLP 를 받아들일지 정한다.
 * DBI 창 안의 벤더 영역 오프셋이다. */
#define PCIE_STRFMR1		0x71c /* Symbol Timer & Filter Mask Register1 */
/* [한국어] 브리지 슬레이브 오류 응답 레지스터. 오류를 전달할지 삼킬지 정한다. */
#define PCIE_ABSERR		0x8d0 /* Bridge Slave Error Response Register */
/* [한국어] 위 레지스터에 쓸 값. 옆의 상류 주석대로 논-포스티드 요청의 오류를
 * 전달하게 하는 설정이며, 비트별 의미는 이 트리에서 확인 못 함. */
#define PCIE_ABSERR_SETTING	0x9401 /* Forward error of non-posted request */

/* PF Message Command Register */
/* [한국어] PF 메시지 명령 레지스터. **PF LUT 창 안의** 오프셋이며 DBI 창 기준이
 * 아니다 — 창이 다르므로 값이 작아도 겹치지 않는다. */
#define LS_PCIE_PF_MCR		0x2c
/* [한국어] PME_Turn_Off 메시지 요청 비트. 세우면 하드웨어가 메시지를 내보내고,
 * 끝나면 스스로 지운다 — 그 자동 해제가 완료 신호 노릇을 한다. */
#define PF_MCR_PTOMR		BIT(0)
/* [한국어] L2 상태 탈출 요청 비트. 위와 같은 방식으로 스스로 지워진다. */
#define PF_MCR_EXL2S		BIT(1)

/* LS1021A PEXn PM Write Control Register */
/* [한국어] LS1021A 의 포트별 PM 쓰기 제어 레지스터. **포트마다 레지스터가
 * 따로** 있으며 0x64 바이트 간격이다. */
#define SCFG_PEXPMWRCR(idx)	(0x5c + (idx) * 0x64)
/* [한국어] 그 레지스터의 PME_Turn_Off 전송 비트. 포트마다 레지스터가 달라
 * 비트는 고정이다. */
#define PMXMTTURNOFF		BIT(31)
/* [한국어] LS1021A 의 소프트 리셋 제어 레지스터. 이쪽은 하나로 고정이다. */
#define SCFG_PEXSFTRSTCR	0x190
/* [한국어] 그 안의 포트별 리셋 비트. **진입 쪽과 반대 배치** 다 — 레지스터가
 * 하나이고 비트가 포트마다 다르다. */
#define PEXSR(idx)		BIT(idx)

/* LS1043A PEX PME control register */
/* [한국어] LS1043A 의 PME 제어 레지스터. 역시 하나로 고정이다. */
#define SCFG_PEXPMECR		0x144
/* [한국어] 그 안의 포트별 비트. 비트 31 에서 시작해 포트마다 4칸씩 내려간다 —
 * 포트 하나에 4비트가 할당되고 그중 최상위만 이 용도로 쓰이는 것으로
 * 보이나, 나머지 3비트의 용도는 이 트리에서 확인 못 함. */
#define PEXPME(idx)		BIT(31 - (idx) * 4)

/* LS1043A PEX LUT debug register */
/* [한국어] LS1043A 가 소프트 리셋에 쓰는 PF LUT 창의 디버그 레지스터. */
#define LS_PCIE_LDBG	0x7fc
/* [한국어] 소프트 리셋 비트. 세우면 PEX 모듈이 리셋에 들어간다. */
#define LDBG_SR		BIT(30)
/* [한국어] 쓰기 허용 비트. **이것이 서 있어야 위 리셋 비트를 쓸 수 있다** —
 * 실수로 리셋이 걸리는 것을 막는 열쇠다. */
#define LDBG_WE		BIT(31)

/* [한국어] [상류 코드 관찰] ATU 창 개수로 보이는 상수인데 이 파일 어디에서도
 * 참조하지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#define PCIE_IATU_NUM		6

struct ls_pcie_drvdata {
	/* [한국어] PF LUT 창이 DBI 창 시작에서 얼마나 떨어져 있는지.
	 * 설정자: 아래 세대별 표가 컴파일 시점에 정한다.
	 * 읽는 자: probe 가 DBI 기준 주소에 더해 pf_lut_base 를 만든다.
	 * 값 범위: LS1021A 는 값을 적지 않아 0, LS1043A 는 0x10000,
	 * 그 밖의 세대는 0xc0000 이다.
	 * 동기화: 상수이므로 필요 없다.
	 * LS1021A 는 PF LUT 를 쓰지 않으므로 0 이어도 문제가 없다. */
	const u32 pf_lut_off;
	/* [한국어] 이 세대가 쓸 호스트 콜백 표.
	 * 설정자: 아래 세대별 표.
	 * 읽는 자: probe 가 코어에 건다.
	 * 값 범위: 세 벌 중 하나. **셋 모두 init 은 같고 pme_turn_off 만 다르다.**
	 * 동기화: 상수를 가리킨다. */
	const struct dw_pcie_host_ops *ops;
	/* [한국어] 이 세대의 L2 탈출 함수.
	 * 설정자: 아래 세대별 표.
	 * 읽는 자: ls_pcie_resume_noirq() 하나뿐.
	 * 값 범위: 세 함수 중 하나. **콜백 표에 넣지 않고 여기 따로 둔 것** 이
	 * 눈에 띄는데, DWC 코어의 host_ops 에 그런 자리가 없어서다.
	 * 동기화: 상수를 가리킨다. */
	int (*exit_from_l2)(struct dw_pcie_rp *pp);
	/* [한국어] SCFG(시스템 설정) 레지스터를 쓰는 세대인지.
	 * 설정자: 아래 세대별 표.
	 * 읽는 자: probe 가 SCFG regmap 을 얻을지 판단한다.
	 * 값 범위: LS1021A 와 LS1043A 만 참.
	 * 동기화: 상수이므로 필요 없다. */
	bool scfg_support;
	/* [한국어] 절전을 지원하는 세대인지.
	 * 설정자: 아래 세대별 표.
	 * 읽는 자: 절전 진입·복귀 두 함수가 맨 앞에서 확인한다.
	 * 값 범위: **지금은 세 표 모두 참** 이라 거짓 갈래가 실제로 쓰이지 않는다.
	 * 표에 항목이 늘 때를 대비한 자리다.
	 * 동기화: 상수이므로 필요 없다. */
	bool pm_support;
};

struct ls_pcie {
	/* [한국어] DWC PCIe 코어의 공통 문맥.
	 * 설정자: probe 가 **따로 할당해** 가리킨다.
	 * 읽는 자: 이 파일의 거의 모든 함수. 특히 pci->dbi_base 로 레지스터에 닿는다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: probe 이후 불변.
	 * **포인터로 가리키는 구조** 라는 점이 이 파일의 특징이다. 같은 트리의
	 * 다른 글루 대부분은 dw_pcie 를 값으로 품어 container_of 로 되돌리는데,
	 * 여기는 할당이 두 번이고 변환도 drvdata 를 거친다. */
	struct dw_pcie *pci;
	/* [한국어] 이 SoC 세대의 특성 표.
	 * 설정자: probe 가 디바이스 트리 매칭 데이터를 담는다.
	 * 읽는 자: probe, 절전 진입·복귀.
	 * 값 범위: 아래 세 표 중 하나를 가리킨다.
	 * 동기화: 상수를 가리킨다. */
	const struct ls_pcie_drvdata *drvdata;
	/* [한국어] PF LUT 창의 가상 주소.
	 * 설정자: probe 가 DBI 기준 주소에 세대별 오프셋을 더해 만든다.
	 * 읽는 자: ls_pcie_pf_lut_readl()/writel().
	 * 값 범위: **별도 자원이 아니라 DBI 창 안의 한 지점** 이다. 그래서
	 * 매핑을 따로 하지 않는다.
	 * 동기화: probe 이후 불변. */
	void __iomem *pf_lut_base;
	/* [한국어] SCFG 레지스터 묶음.
	 * 설정자: probe 가 SCFG 를 쓰는 세대에서만 얻는다.
	 * 읽는 자: SCFG 방식의 진입·탈출 함수들.
	 * 값 범위: 유효한 regmap 포인터, 또는 쓰지 않는 세대에서는 NULL 인 채로 남는다.
	 * 동기화: regmap 계층이 자체 잠금을 갖는다. */
	struct regmap *scfg;
	/* [한국어] 이 컨트롤러가 SCFG 안에서 몇 번 포트인지.
	 * 설정자: probe 가 디바이스 트리 phandle 의 두 번째 인자에서 읽는다.
	 * SCFG 를 쓰지 않는 세대에서는 0 인 채로 남는다.
	 * 읽는 자: SCFG 방식의 레지스터·비트 계산.
	 * 값 범위: 0 이상. 상한은 SoC 의 포트 수이나 검사하지 않는다.
	 * 동기화: probe 이후 불변. */
	int index;
	/* [한국어] PF LUT 레지스터가 빅 엔디언인지.
	 * 설정자: probe 가 디바이스 트리의 "big-endian" 속성 유무로 정한다.
	 * 읽는 자: ls_pcie_pf_lut_readl()/writel() 이 접근할 때마다 확인한다.
	 * 값 범위: 참/거짓.
	 * 동기화: probe 이후 불변.
	 * **같은 SoC 계열 안에서도 보드마다 다를 수 있어** 컴파일 시점이 아니라
	 * 실행 시점에 가른다. */
	bool big_endian;
};

/* [한국어] 폴링용 한 인자 접근자. readx_poll_timeout 매크로가 인자 하나짜리
 * 읽기 함수를 요구하는데 이 파일의 읽기 함수는 둘을 받으므로, 그 간극을
 * 메운다. **드라이버 상태를 호출 지점의 지역 변수 `pcie` 에서 집어 오므로**,
 * 그 이름의 변수가 보이는 자리에서만 쓸 수 있다. */
#define ls_pcie_pf_lut_readl_addr(addr)	ls_pcie_pf_lut_readl(pcie, addr)
/* [한국어] DWC 코어가 주는 dw_pcie 포인터를 이 드라이버의 구조체로 되돌린다.
 * dw_pcie 를 값으로 품지 않아 container_of 를 쓸 수 없으므로 drvdata 를
 * 거친다 — 그래서 probe 가 drvdata 를 매단 뒤에만 유효하다. */
#define to_ls_pcie(x)	dev_get_drvdata((x)->dev)

/* [한국어]
 * ls_pcie_is_bridge - 이 컨트롤러가 브리지(루트 포트) 모드인지 확인한다
 *
 * @pcie: 드라이버 상태.
 * @return: true = 브리지 모드, false = 아니다.
 *
 * config 공간의 헤더 타입 필드를 읽어 브리지인지 본다. 다중 기능 비트를
 * 마스크로 떼어 낸 뒤 비교하는 것이 요점이다 — 그 비트가 서 있어도
 * 헤더 타입 자체는 같아야 하기 때문이다.
 *
 * **probe 가 이 결과로 물러날지 말지를 정한다.** Layerscape 컨트롤러는
 * 루트 포트로도 엔드포인트로도 쓸 수 있고, 그 선택은 펌웨어가 부팅 때
 * 확정한다. 엔드포인트로 설정돼 있으면 이 파일이 아니라 별도의 엔드포인트
 * 드라이버가 맡아야 하므로, 여기서 -ENODEV 로 비켜 준다.
 *
 * 즉 이 함수는 **드라이버가 자기 차례인지 판정하는 관문** 이다.
 *
 * 8비트 접근을 쓴다. 헤더 타입이 config 공간의 한 바이트이기 때문이다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 읽기가 실패할 여지는 고려하지 않는다.
 *
 * 호출 체인:
 *   ls_pcie_probe() → [이 함수] → ioread8()
 */
static bool ls_pcie_is_bridge(struct ls_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	u32 header_type;
/* [한국어] 헤더 타입 바이트를 읽는다. config 공간의 오프셋 0x0e 다. */

	header_type = ioread8(pci->dbi_base + PCI_HEADER_TYPE);
	/* [한국어] 다중 기능 비트를 떼어 낸다. 그 비트가 서 있어도 헤더 타입 자체는
	 * 같아야 하므로, 마스크 없이 비교하면 브리지를 놓친다. */
	header_type &= PCI_HEADER_TYPE_MASK;

	return header_type == PCI_HEADER_TYPE_BRIDGE;
}

/* Clear multi-function bit */
/* [한국어]
 * ls_pcie_clear_multifunction - 헤더 타입의 다중 기능 비트를 지운다
 *
 * @pcie: 드라이버 상태.
 *
 * 헤더 타입 바이트에 브리지 값만 써서 다중 기능 비트를 함께 지운다.
 * 읽기-수정-쓰기가 아니라 **통째로 덮어쓰는** 방식이라, 그 바이트의 다른
 * 내용이 남지 않는다.
 *
 * 왜 지워야 하는가. 이 컨트롤러가 다중 기능이라고 보고하면 리눅스가
 * 기능 1~7 을 탐색하는데, 실제로는 루트 포트 하나뿐이라 그 탐색이 유령
 * 장치를 만들거나 시간만 낭비한다.
 *
 * config 공간의 헤더 타입은 원래 읽기 전용이라, 호출자가
 * dw_pcie_dbi_ro_wr_en/dis() 로 감싸 준다. 그 감싸기가 이 함수 안이 아니라
 * 바깥에 있는 것은 host_init 이 그 구간을 자기 흐름의 일부로 다루기
 * 때문이다.
 *
 * 실행 컨텍스트: host_init 안, 읽기 전용 보호가 풀린 구간. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_host_init() → [이 함수] → iowrite8()
 */
static void ls_pcie_clear_multifunction(struct ls_pcie *pcie)
{
	/* [한국어] 코어 문맥에서 DBI 창 주소를 얻는다. */
	struct dw_pcie *pci = pcie->pci;

	/* [한국어] 헤더 타입 바이트를 브리지 값으로 **통째 덮어쓴다.** 그 값에는
	 * 다중 기능 비트가 없으므로 함께 지워진다. */
	iowrite8(PCI_HEADER_TYPE_BRIDGE, pci->dbi_base + PCI_HEADER_TYPE);
}

/* Drop MSG TLP except for Vendor MSG */
/* [한국어]
 * ls_pcie_drop_msg_tlp - 벤더 메시지를 뺀 나머지 메시지 TLP 를 버리게 한다
 *
 * @pcie: 드라이버 상태.
 *
 * 심볼 타이머·필터 마스크 레지스터의 비트 하나를 지워, 들어오는 메시지
 * TLP(Transaction Layer Packet) 중 벤더 정의 메시지만 통과시키고 나머지는
 * 버리게 한다.
 *
 * **마스크가 이름 없는 상수로 쓰여 있다.** 0xDFFFFFFF 는 비트 29 만 0 인
 * 값이므로 결국 비트 29 를 지우는 것인데, 그 비트에 이름이 붙어 있지 않아
 * 값을 직접 읽어야 뜻을 알 수 있다. 같은 파일의 다른 비트들이 모두
 * BIT() 매크로로 이름을 갖고 있는 것과 대비된다.
 *
 * 읽기-수정-쓰기이므로 그 레지스터의 다른 설정(심볼 타이머 값 등)은
 * 보존된다.
 *
 * 이 레지스터는 읽기 전용이 아니라, host_init 에서 보호를 푼 구간 **밖** 에서
 * 불린다.
 *
 * 실행 컨텍스트: host_init. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_host_init() → [이 함수] → ioread32() → iowrite32()
 */
static void ls_pcie_drop_msg_tlp(struct ls_pcie *pcie)
{
	u32 val;
	struct dw_pcie *pci = pcie->pci;

	val = ioread32(pci->dbi_base + PCIE_STRFMR1);
	/* [한국어] 비트 29 를 지운다. **이름이 붙어 있지 않은 마스크** 라 값을 직접
	 * 읽어야 뜻을 알 수 있다 — 같은 파일의 다른 비트들이 모두 BIT() 매크로로
	 * 이름을 가진 것과 대비된다. */
	val &= 0xDFFFFFFF;
	/* [한국어] 고친 값을 되쓴다. 이 레지스터의 다른 설정은 보존된다. */
	iowrite32(val, pci->dbi_base + PCIE_STRFMR1);
}

/* Forward error response of outbound non-posted requests */
/* [한국어]
 * ls_pcie_fix_error_response - 논-포스티드 요청의 오류 응답을 전달하게 한다
 *
 * @pcie: 드라이버 상태.
 *
 * 브리지 슬레이브 오류 응답 레지스터에 정해진 값을 써 넣는다.
 *
 * 무엇을 고치는가. 논-포스티드 요청(응답을 기다리는 읽기 등)이 실패했을
 * 때, 기본 설정에서는 그 오류가 요청자에게 전달되지 않는다. 그러면 잘못된
 * 데이터를 정상인 것으로 받아들이게 된다. 이 설정으로 오류를 그대로
 * 전달하게 만든다.
 *
 * 값이 통째 쓰기다 — 그 레지스터가 이 설정 전용이라는 전제이며, 그 안의
 * 비트별 의미는 이 트리에서 확인 못 함.
 *
 * **host_init 의 첫 단계** 다. 이후 단계들이 DBI 접근을 하므로, 오류가
 * 전달되는 상태를 먼저 만들어 두는 순서로 볼 수 있다.
 *
 * 실행 컨텍스트: host_init. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_host_init() → [이 함수] → iowrite32()
 */
static void ls_pcie_fix_error_response(struct ls_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;

	iowrite32(PCIE_ABSERR_SETTING, pci->dbi_base + PCIE_ABSERR);
}

/* [한국어]
 * ls_pcie_pf_lut_readl - PF LUT 창의 레지스터를 엔디언에 맞춰 읽는다
 *
 * @pcie: 드라이버 상태.
 * @off: 창 안의 오프셋.
 * @return: 읽은 값.
 *
 * **엔디언을 실행 시점에 가른다** 는 것이 이 함수의 존재 이유다.
 * Layerscape 는 같은 SoC 계열 안에서도 보드에 따라 레지스터가 빅 엔디언일
 * 수도 리틀 엔디언일 수도 있어, 디바이스 트리의 "big-endian" 속성으로
 * 그것을 알려 준다.
 *
 * PF LUT 창은 별도의 자원이 아니라 **DBI 창 안의 특정 오프셋** 이다.
 * 세대마다 그 오프셋이 0, 0x10000, 0xc0000 으로 다르며, probe 가 미리
 * 더해 pf_lut_base 에 담아 둔다.
 *
 * 폴링에 쓰이므로 아래 매크로가 이 함수를 한 인자 형태로 감싼다.
 *
 * 실행 컨텍스트: 절전 진입·복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_send_turnoff_msg() / ls_pcie_exit_from_l2() /
 *   ls1043a_pcie_exit_from_l2() → [이 함수] → ioread32be() 또는 ioread32()
 */
static u32 ls_pcie_pf_lut_readl(struct ls_pcie *pcie, u32 off)
{
	/* [한국어] 빅 엔디언이면 바이트 순서를 뒤집어 읽는다. */
	if (pcie->big_endian)
		return ioread32be(pcie->pf_lut_base + off);

	/* [한국어] 그렇지 않으면 그대로 읽는다. */
	return ioread32(pcie->pf_lut_base + off);
}

/* [한국어]
 * ls_pcie_pf_lut_writel - PF LUT 창의 레지스터에 엔디언에 맞춰 쓴다
 *
 * @pcie: 드라이버 상태.
 * @off: 창 안의 오프셋.
 * @val: 쓸 값.
 *
 * 위 읽기 판의 짝이며 같은 엔디언 판정을 한다.
 *
 * 읽기 판이 if 하나로 이른 반환을 쓰는 것과 달리 이쪽은 if/else 를 쓰는데,
 * 반환값이 없어서 생긴 차이다.
 *
 * 실행 컨텍스트: 절전 진입·복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_send_turnoff_msg() / ls_pcie_exit_from_l2() /
 *   ls1043a_pcie_exit_from_l2() → [이 함수] → iowrite32be() 또는 iowrite32()
 */
static void ls_pcie_pf_lut_writel(struct ls_pcie *pcie, u32 off, u32 val)
{
	if (pcie->big_endian)
		iowrite32be(val, pcie->pf_lut_base + off);
	/* [한국어] 리틀 엔디언 판이다. 읽기 쪽은 이른 반환을 써 else 가 없다. */
	else
		iowrite32(val, pcie->pf_lut_base + off);
}

/* [한국어]
 * ls_pcie_send_turnoff_msg - PF LUT 방식으로 L2 진입을 요청하고 완료를 기다린다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * 세대 셋 중 **가장 정돈된 방식** 이다. 메시지 명령 레지스터의 비트 하나로
 * 요청하고, 같은 비트가 하드웨어에 의해 내려가는 것으로 완료를 확인한다.
 *
 * 두 단계다.
 * 1. PTOMR(PME Turn-Off Message Request) 비트를 세운다. 하드웨어가 그것을
 *    보고 PME_Turn_Off 메시지를 링크로 내보낸다.
 * 2. 그 비트가 스스로 내려갈 때까지 기다린다. 하드웨어가 상대의 응답을
 *    받으면 지워 주기 때문이다.
 *
 * 폴링에 ls_pcie_pf_lut_readl_addr 매크로를 쓴다 — 위에서 설명한, 지역
 * 변수 `pcie` 를 집어 오는 그 매크로다.
 *
 * 대기 상한이 PCIE_PME_TO_L2_TIMEOUT_US(drivers/pci/pci.h:122, 10ms)이고,
 * 확인 주기는 그 십분의 일이다. 즉 최대 열 번 정도 확인한다.
 *
 * SCFG 방식(아래)과 견주면 **완료를 실제로 확인할 수 있다** 는 것이 결정적
 * 차이다. 그쪽은 확인할 레지스터가 없어 무작정 기다린다.
 *
 * 실행 컨텍스트: 절전 진입의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 시간이 다 되면 기록만 남기고 그냥 돌아간다. 콜백 반환형이
 * void 라 실패를 위로 알릴 방법이 없다.
 *
 * 호출 체인:
 *   DWC 코어의 절전 경로 → dw_pcie_host_ops.pme_turn_off == [이 함수]
 *     → ls_pcie_pf_lut_readl() → ls_pcie_pf_lut_writel()
 *     → readx_poll_timeout()
 */
static void ls_pcie_send_turnoff_msg(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ls_pcie *pcie = to_ls_pcie(pci);
	/* [한국어] 레지스터 값을 담고 폴링 결과도 받는 자리. */
	u32 val;
	/* [한국어] 폴링의 성패. */
	int ret;

	val = ls_pcie_pf_lut_readl(pcie, LS_PCIE_PF_MCR);
	/* [한국어] PME_Turn_Off 요청 비트를 세운다. */
	val |= PF_MCR_PTOMR;
	/* [한국어] 되쓴다. 이 쓰기 순간 하드웨어가 링크로 PME_Turn_Off 메시지를 내보낸다. */
	ls_pcie_pf_lut_writel(pcie, LS_PCIE_PF_MCR, val);

	ret = readx_poll_timeout(ls_pcie_pf_lut_readl_addr, LS_PCIE_PF_MCR,
				 /* [한국어] 하드웨어가 그 비트를 **스스로 지울** 때까지 기다린다. 지워진 것이
				  * 상대의 응답을 받았다는 뜻이다. */
				 val, !(val & PF_MCR_PTOMR),
				 PCIE_PME_TO_L2_TIMEOUT_US/10,
				 PCIE_PME_TO_L2_TIMEOUT_US);
	if (ret)
		/* [한국어] 시간이 다 됐다. 기록만 남기고 그냥 돌아간다 — 콜백 반환형이 void 라
		 * 실패를 위로 알릴 방법이 없다. */
		dev_err(pcie->pci->dev, "PME_Turn_off timeout\n");
}

/* [한국어]
 * ls_pcie_exit_from_l2 - PF LUT 방식으로 링크를 L2 에서 꺼낸다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 0 = 성공, 음수 = 시간 초과.
 *
 * ls_pcie_send_turnoff_msg() 의 짝이며 같은 레지스터의 다른 비트를 쓴다.
 * EXL2S(Exit L2 State) 비트를 세우면 하드웨어가 링크를 깨우고, 끝나면
 * 그 비트를 스스로 지운다.
 *
 * **대기 상한이 특이하다.** 옆의 상류 주석이 밝히듯 10ms 는 규격에 정의된
 * 값이 아니라 **실측으로 정한 값** 이다. 그래서 위 진입 쪽이 쓰는
 * PCIE_PME_TO_L2_TIMEOUT_US 상수를 쓰지 않고 숫자를 직접 적었다.
 * 확인 주기는 1ms 다.
 *
 * 진입 쪽과 달리 **반환값이 있다.** 복귀 경로가 이 결과를 확인해, 실패하면
 * DWC 코어의 복귀를 아예 시작하지 않는다 — 링크가 깨어나지 않은 상태에서
 * config 접근을 하면 시스템이 멈출 수 있기 때문이다.
 *
 * 실행 컨텍스트: 절전 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 시간이 다 되면 기록을 남기고 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   ls_pcie_resume_noirq() → drvdata->exit_from_l2 == [이 함수]
 *     → ls_pcie_pf_lut_readl() → ls_pcie_pf_lut_writel()
 *     → readx_poll_timeout()
 */
static int ls_pcie_exit_from_l2(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ls_pcie *pcie = to_ls_pcie(pci);
	/* [한국어] 레지스터 값을 담고 폴링 결과도 받는 자리. */
	u32 val;
	/* [한국어] 폴링의 성패. **이쪽은 반환값이 있어 호출자가 확인한다.** */
	int ret;

	/*
	 * Set PF_MCR_EXL2S bit in LS_PCIE_PF_MCR register for the link
	 * to exit L2 state.
	 */
	val = ls_pcie_pf_lut_readl(pcie, LS_PCIE_PF_MCR);
	val |= PF_MCR_EXL2S;
	/* [한국어] 되쓴다. 하드웨어가 링크를 깨우기 시작한다. */
	ls_pcie_pf_lut_writel(pcie, LS_PCIE_PF_MCR, val);

	/*
	 * L2 exit timeout of 10ms is not defined in the specifications,
	 * it was chosen based on empirical observations.
	 */
	ret = readx_poll_timeout(ls_pcie_pf_lut_readl_addr, LS_PCIE_PF_MCR,
				 val, !(val & PF_MCR_EXL2S),
				 1000,
				 10000);
	if (ret)
		/* [한국어] 시간이 다 됐다. 기록을 남기고 아래에서 그 오류를 올려보낸다. */
		dev_err(pcie->pci->dev, "L2 exit timeout\n");

	return ret;
}

/* [한국어]
 * ls_pcie_host_init - DBI 세 곳을 고치는 것으로 초기화를 마친다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 언제나 0.
 *
 * **이 드라이버의 초기화가 이것뿐이다.** 클럭도 PHY 도 리셋도 건드리지
 * 않는다 — 그런 것은 펌웨어가 이미 해 두었다는 전제다. 같은 트리의
 * pcie-eswin.c 나 pcie-nxp-s32g.c 가 그 모두를 손수 세우는 것과 극명하게
 * 대비된다.
 *
 * 세 가지를 한다.
 * 1. 논-포스티드 요청의 오류 응답을 전달하게 고친다.
 * 2. 다중 기능 비트를 지운다. **이것만 읽기 전용 보호를 풀어야** 하므로
 *    앞뒤로 감싸는 호출이 붙어 있다.
 * 3. 메시지 TLP 를 걸러 내게 한다.
 *
 * 2번만 감싸는 것이 요점이다. 1번과 3번이 건드리는 레지스터는 쓰기가
 * 가능한 벤더 영역이고, 헤더 타입만 PCI 규격상 읽기 전용이기 때문이다.
 *
 * 세대 셋의 host_ops 표가 모두 이 함수를 init 으로 걸고 있다 — 세대 차이는
 * init 이 아니라 pme_turn_off 에만 있다.
 *
 * 실행 컨텍스트: dw_pcie_host_init() 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 언제나 0 을 돌려준다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_host_ops.init == [이 함수]
 *     → ls_pcie_fix_error_response() → dw_pcie_dbi_ro_wr_en()
 *     → ls_pcie_clear_multifunction() → dw_pcie_dbi_ro_wr_dis()
 *     → ls_pcie_drop_msg_tlp()
 */
static int ls_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* [한국어] 코어의 문맥에서 DWC 구조체를 얻는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 를 거쳐 이 드라이버의 상태로 변환한다. */
	struct ls_pcie *pcie = to_ls_pcie(pci);

	/* [한국어] 먼저 오류 응답 방식을 고친다. 이후 접근의 실패가 전달되게 된다. */
	ls_pcie_fix_error_response(pcie);

	/* [한국어] 읽기 전용 보호를 푼다. 헤더 타입이 PCI 규격상 읽기 전용이기 때문이다. */
	dw_pcie_dbi_ro_wr_en(pci);
	/* [한국어] 다중 기능 비트를 지운다. 이 한 줄만 보호를 풀어야 한다. */
	ls_pcie_clear_multifunction(pcie);
	/* [한국어] 곧바로 다시 건다. 감싸는 구간을 최소로 유지한다. */
	dw_pcie_dbi_ro_wr_dis(pci);

	/* [한국어] 메시지 TLP 를 걸러 내게 한다. 이 레지스터는 벤더 영역이라 보호를
	 * 풀 필요가 없다. */
	ls_pcie_drop_msg_tlp(pcie);

	return 0;
}

/* [한국어]
 * scfg_pcie_send_turnoff_msg - SCFG 방식으로 L2 진입을 요청한다 (LS1021A/LS1043A 공용)
 *
 * @scfg: SCFG 레지스터 묶음(regmap).
 * @reg: 건드릴 레지스터 오프셋.
 * @mask: 그 안에서 세울 비트.
 *
 * 두 세대가 **같은 절차를 다른 레지스터에** 적용하므로, 레지스터와 비트를
 * 인자로 받아 공용화했다. LS1021A 와 LS1043A 의 래퍼가 각각 자기 값을
 * 넣어 부른다.
 *
 * 세 단계다.
 * 1. 비트를 세워 PME_Turn_Off 를 내보내게 한다.
 * 2. **무작정 기다린다.** 옆의 상류 주석이 밝히듯 상대의 응답을 확인할
 *    레지스터가 아예 없어, 안전하게 상한만큼 기다리는 수밖에 없다.
 * 3. 비트를 다시 지운다. 상류 주석대로 하드웨어 참조 매뉴얼이 권장하는
 *    절차이며, 이것으로 핸드셰이크가 마무리된다.
 *
 * [상류 코드 관찰] 2번이 msleep 이 아니라 **mdelay** 다 — 즉 CPU 를 놓지
 * 않고 10ms 를 바쁜 대기한다. 절전 진입 경로라 다른 할 일이 없기는 하나,
 * 잠들 수 있는 문맥에서도 CPU 를 쥐고 있는다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * regmap_write_bits 를 쓰므로 마스크 밖의 비트는 보존된다 — SCFG 레지스터가
 * 여러 포트의 설정을 함께 담고 있기 때문이다.
 *
 * 실행 컨텍스트: 절전 진입의 noirq 단계. mdelay 로 10ms 바쁜 대기한다.
 *
 * 에러 경로: 없다. 두 regmap 쓰기의 반환값을 모두 확인하지 않는다.
 *
 * 호출 체인:
 *   ls1021a_pcie_send_turnoff_msg() / ls1043a_pcie_send_turnoff_msg()
 *     → [이 함수] → regmap_write_bits() → mdelay() → regmap_write_bits()
 */
static void scfg_pcie_send_turnoff_msg(struct regmap *scfg, u32 reg, u32 mask)
{
	/* Send PME_Turn_Off message */
	regmap_write_bits(scfg, reg, mask, mask);

	/*
	 * There is no specific register to check for PME_To_Ack from endpoint.
	 * So on the safe side, wait for PCIE_PME_TO_L2_TIMEOUT_US.
	 */
	mdelay(PCIE_PME_TO_L2_TIMEOUT_US/1000);

	/*
	 * Layerscape hardware reference manual recommends clearing the PMXMTTURNOFF bit
	 * to complete the PME_Turn_Off handshake.
	 */
	regmap_write_bits(scfg, reg, mask, 0);
}

/* [한국어]
 * ls1021a_pcie_send_turnoff_msg - LS1021A 의 SCFG 레지스터로 L2 진입을 요청한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * 공용 함수에 LS1021A 의 레지스터와 비트를 넣어 부르는 껍데기다.
 *
 * 레지스터 오프셋이 포트 번호로 계산된다 — 포트 하나가 0x64 바이트씩
 * 떨어져 있고, 이 컨트롤러가 몇 번 포트인지는 probe 가 디바이스 트리의
 * SCFG phandle 인자에서 읽어 둔 index 로 안다.
 *
 * 비트는 PMXMTTURNOFF 하나로 고정이다 — 포트마다 레지스터가 따로 있어
 * 비트를 나눌 필요가 없기 때문이며, 아래 LS1043A 가 반대 방식을 쓰는 것과
 * 대비된다.
 *
 * 실행 컨텍스트: 절전 진입의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어의 절전 경로 → dw_pcie_host_ops.pme_turn_off == [이 함수]
 *     → scfg_pcie_send_turnoff_msg()
 */
static void ls1021a_pcie_send_turnoff_msg(struct dw_pcie_rp *pp)
{
	/* [한국어] 코어의 문맥에서 DWC 구조체를 얻는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 를 거쳐 상태로 변환한다. */
	struct ls_pcie *pcie = to_ls_pcie(pci);

	/* [한국어] 공용 함수에 LS1021A 의 값을 넣어 부른다. **레지스터가 포트 번호로
	 * 계산되고 비트는 고정** 이다. */
	scfg_pcie_send_turnoff_msg(pcie->scfg, SCFG_PEXPMWRCR(pcie->index), PMXMTTURNOFF);
}

/* [한국어]
 * scfg_pcie_exit_from_l2 - SCFG 방식으로 PEX 래퍼를 소프트 리셋한다
 *
 * @scfg: SCFG 레지스터 묶음(regmap).
 * @reg: 건드릴 레지스터 오프셋.
 * @mask: 그 안에서 세울 비트.
 * @return: 언제나 0.
 *
 * 리셋 비트를 세웠다 곧바로 지워 **펄스** 를 만든다. 그 펄스가 PEX 래퍼를
 * 초기화하고, 그 부작용으로 링크가 L2 에서 빠져나온다.
 *
 * 두 쓰기 사이에 지연이 없다. 하드웨어가 그 폭으로도 인식한다는 전제다.
 *
 * **완료를 확인하지 않는다.** 링크가 실제로 깨어났는지 알 방법이 없어,
 * 쓰기만 하고 성공을 가정한 채 0 을 돌려준다. PF LUT 방식이 비트가
 * 내려가기를 기다리는 것과 대비된다.
 *
 * [상류 코드 관찰] 반환형이 int 인데 언제나 0 이다. 세대별
 * exit_from_l2 콜백의 서명을 맞추기 위한 것으로 보인다. 두 regmap 쓰기의
 * 반환값도 확인하지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 지금은 LS1021A 만 이 함수를 쓴다 — LS1043A 는 SCFG 대신 디버그
 * 레지스터로 같은 일을 한다.
 *
 * 실행 컨텍스트: 절전 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls1021a_pcie_exit_from_l2() → [이 함수] → regmap_write_bits() ×2
 */
static int scfg_pcie_exit_from_l2(struct regmap *scfg, u32 reg, u32 mask)
{
	/* Reset the PEX wrapper to bring the link out of L2 */
	/* [한국어] 리셋 비트를 세운다. */
	regmap_write_bits(scfg, reg, mask, mask);
	/* [한국어] 곧바로 지워 펄스를 만든다. 두 쓰기 사이에 지연이 없어, 하드웨어가
	 * 그 폭으로도 인식한다는 전제다. */
	regmap_write_bits(scfg, reg, mask, 0);

	/* [한국어] 완료를 확인할 방법이 없어 성공을 가정하고 0 을 돌려준다.
	 * 반환형이 int 인 것은 콜백 서명을 맞추기 위한 것이다. */
	return 0;
}

/* [한국어]
 * ls1021a_pcie_exit_from_l2 - LS1021A 의 SCFG 소프트 리셋으로 링크를 깨운다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 언제나 0(공용 함수가 그렇게 돌려준다).
 *
 * 공용 함수에 LS1021A 의 소프트 리셋 레지스터와 포트별 비트를 넣어 부른다.
 *
 * 진입 쪽과 반대 배치라는 점이 눈에 띈다 — 진입은 포트마다 레지스터가
 * 다르고 비트가 고정인데, 여기는 **레지스터가 하나이고 포트마다 비트가
 * 다르다.**
 *
 * 실행 컨텍스트: 절전 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_resume_noirq() → drvdata->exit_from_l2 == [이 함수]
 *     → scfg_pcie_exit_from_l2()
 */
static int ls1021a_pcie_exit_from_l2(struct dw_pcie_rp *pp)
{
	/* [한국어] 코어의 문맥에서 DWC 구조체를 얻는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 를 거쳐 상태로 변환한다. */
	struct ls_pcie *pcie = to_ls_pcie(pci);

	/* [한국어] LS1021A 의 값을 넣어 부른다. **진입 쪽과 반대로 레지스터가 하나이고
	 * 비트가 포트마다 다르다.** */
	return scfg_pcie_exit_from_l2(pcie->scfg, SCFG_PEXSFTRSTCR, PEXSR(pcie->index));
}

/* [한국어]
 * ls1043a_pcie_send_turnoff_msg - LS1043A 의 SCFG 레지스터로 L2 진입을 요청한다
 *
 * @pp: DWC 의 루트 포트 문맥.
 *
 * LS1021A 판과 같은 공용 함수를 부르되 레지스터와 비트 계산이 다르다.
 *
 * 레지스터가 **하나로 고정** 이고 포트마다 비트가 다르다. 비트 자리가
 * 포트 번호에 따라 4칸씩 내려가는데(31, 27, 23, ...), 포트 하나에 4비트씩
 * 할당돼 있고 그중 최상위만 이 용도로 쓰인다는 뜻으로 보인다. 나머지
 * 3비트의 용도는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: 절전 진입의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   DWC 코어의 절전 경로 → dw_pcie_host_ops.pme_turn_off == [이 함수]
 *     → scfg_pcie_send_turnoff_msg()
 */
static void ls1043a_pcie_send_turnoff_msg(struct dw_pcie_rp *pp)
{
	/* [한국어] 코어의 문맥에서 DWC 구조체를 얻는다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] drvdata 를 거쳐 상태로 변환한다. */
	struct ls_pcie *pcie = to_ls_pcie(pci);

	/* [한국어] LS1043A 의 값을 넣어 부른다. 레지스터는 하나이고 비트가 포트마다
	 * 4칸씩 내려간다. */
	scfg_pcie_send_turnoff_msg(pcie->scfg, SCFG_PEXPMECR, PEXPME(pcie->index));
}

/* [한국어]
 * ls1043a_pcie_exit_from_l2 - 디버그 레지스터로 소프트 리셋을 걸어 링크를 깨운다
 *
 * @pp: DWC 의 루트 포트 문맥.
 * @return: 언제나 0.
 *
 * 세 방식 중 **가장 손이 많이 가는** 판이다. SCFG 를 쓰지 않고 PF LUT 창의
 * 디버그 레지스터를 직접 조작한다.
 *
 * 옆의 상류 주석이 두 비트의 역할을 밝힌다.
 *   LDBG_WE : 소프트 리셋 비트에 **쓰기를 허용** 하는 열쇠 비트.
 *   LDBG_SR : 실제 소프트 리셋 비트. 세우면 PEX 모듈이 리셋에 들어간다.
 *
 * 그래서 네 단계를 밟는다 — 열쇠 열기, 리셋 걸기, 리셋 풀기, 열쇠 잠그기.
 * 가운데 둘이 펄스를 만들고, 바깥 둘이 그 펄스를 낼 수 있게 해 준다.
 *
 * **네 단계가 각각 따로 읽고 쓴다.** 한 번 읽어 두고 값을 굴려도 될 자리인데
 * 매번 다시 읽는데, 하드웨어가 중간에 비트를 바꿀 수 있다고 본 것인지는
 * 코드에 적혀 있지 않다.
 *
 * 열쇠 비트를 마지막에 다시 잠그는 것이 중요하다 — 열어 둔 채로 두면
 * 이후의 실수로 PEX 모듈이 리셋될 수 있다.
 *
 * [상류 코드 관찰] 여기도 완료를 확인하지 않고 언제나 0 을 돌려준다.
 *
 * 실행 컨텍스트: 절전 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ls_pcie_resume_noirq() → drvdata->exit_from_l2 == [이 함수]
 *     → ls_pcie_pf_lut_readl() ×4 → ls_pcie_pf_lut_writel() ×4
 */
static int ls1043a_pcie_exit_from_l2(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ls_pcie *pcie = to_ls_pcie(pci);
	/* [한국어] 레지스터 값을 담을 자리. 네 단계가 이 하나를 돌려 쓴다. */
	u32 val;

	/*
	 * Reset the PEX wrapper to bring the link out of L2.
	 * LDBG_WE: allows the user to have write access to the PEXDBG[SR] for both setting and
	 *	    clearing the soft reset on the PEX module.
	 * LDBG_SR: When SR is set to 1, the PEX module enters soft reset.
	 */
	val = ls_pcie_pf_lut_readl(pcie, LS_PCIE_LDBG);
	val |= LDBG_WE;
	/* [한국어] 열쇠를 연다. 이제 아래의 리셋 비트 쓰기가 먹힌다. */
	ls_pcie_pf_lut_writel(pcie, LS_PCIE_LDBG, val);

	val = ls_pcie_pf_lut_readl(pcie, LS_PCIE_LDBG);
	/* [한국어] 리셋 비트를 세운다. PEX 모듈이 소프트 리셋에 들어간다. */
	val |= LDBG_SR;
	/* [한국어] 되쓴다. */
	ls_pcie_pf_lut_writel(pcie, LS_PCIE_LDBG, val);

	val = ls_pcie_pf_lut_readl(pcie, LS_PCIE_LDBG);
	/* [한국어] 리셋 비트를 지운다. 이 둘이 짝이 되어 리셋 펄스를 만든다. */
	val &= ~LDBG_SR;
	/* [한국어] 되쓴다. 이 순간 링크가 L2 에서 빠져나온다. */
	ls_pcie_pf_lut_writel(pcie, LS_PCIE_LDBG, val);

	val = ls_pcie_pf_lut_readl(pcie, LS_PCIE_LDBG);
	/* [한국어] 열쇠를 다시 잠근다. 열어 둔 채로 두면 이후의 실수로 PEX 모듈이
	 * 리셋될 수 있다. */
	val &= ~LDBG_WE;
	/* [한국어] 되쓴다. */
	ls_pcie_pf_lut_writel(pcie, LS_PCIE_LDBG, val);

	return 0;
}

static const struct dw_pcie_host_ops ls_pcie_host_ops = {
	/* [한국어] 초기화 콜백은 세 표가 모두 같다. */
	.init = ls_pcie_host_init,
	/* [한국어] PF LUT 방식의 L2 진입. 세대 차이가 이 한 줄에 있다. */
	.pme_turn_off = ls_pcie_send_turnoff_msg,
};

static const struct dw_pcie_host_ops ls1021a_pcie_host_ops = {
	/* [한국어] 같은 초기화 콜백. */
	.init = ls_pcie_host_init,
	/* [한국어] LS1021A 는 SCFG 방식의 진입을 쓴다. */
	.pme_turn_off = ls1021a_pcie_send_turnoff_msg,
};

static const struct ls_pcie_drvdata ls1021a_drvdata = {
	/* [한국어] 절전을 지원한다. */
	.pm_support = true,
	/* [한국어] SCFG 레지스터를 쓰는 세대다. */
	.scfg_support = true,
	.ops = &ls1021a_pcie_host_ops,
	.exit_from_l2 = ls1021a_pcie_exit_from_l2,
};

static const struct dw_pcie_host_ops ls1043a_pcie_host_ops = {
	/* [한국어] 같은 초기화 콜백. */
	.init = ls_pcie_host_init,
	/* [한국어] LS1043A 도 SCFG 방식이되 레지스터가 다르다. */
	.pme_turn_off = ls1043a_pcie_send_turnoff_msg,
};

static const struct ls_pcie_drvdata ls1043a_drvdata = {
	/* [한국어] PF LUT 창이 DBI 창 시작에서 0x10000 떨어져 있다. */
	.pf_lut_off = 0x10000,
	/* [한국어] 절전을 지원한다. */
	.pm_support = true,
	.scfg_support = true,
	.ops = &ls1043a_pcie_host_ops,
	.exit_from_l2 = ls1043a_pcie_exit_from_l2,
};

static const struct ls_pcie_drvdata layerscape_drvdata = {
	/* [한국어] PF LUT 창이 0xc0000 떨어져 있다. 세대마다 이 값만 다르고 나머지
	 * 구조는 같다. */
	.pf_lut_off = 0xc0000,
	/* [한국어] 절전을 지원한다. */
	.pm_support = true,
	.ops = &ls_pcie_host_ops,
	.exit_from_l2 = ls_pcie_exit_from_l2,
};

static const struct of_device_id ls_pcie_of_match[] = {
	/* [한국어] LS1012A — 공용 표를 쓴다. */
	{ .compatible = "fsl,ls1012a-pcie", .data = &layerscape_drvdata },
	/* [한국어] LS1021A — 전용 표를 쓴다. SCFG 방식이라 나머지와 다르다. */
	{ .compatible = "fsl,ls1021a-pcie", .data = &ls1021a_drvdata },
	{ .compatible = "fsl,ls1028a-pcie", .data = &layerscape_drvdata },
	{ .compatible = "fsl,ls1043a-pcie", .data = &ls1043a_drvdata },
	{ .compatible = "fsl,ls1046a-pcie", .data = &layerscape_drvdata },
	{ .compatible = "fsl,ls2080a-pcie", .data = &layerscape_drvdata },
	{ .compatible = "fsl,ls2085a-pcie", .data = &layerscape_drvdata },
	{ .compatible = "fsl,ls2088a-pcie", .data = &layerscape_drvdata },
	{ .compatible = "fsl,ls1088a-pcie", .data = &layerscape_drvdata },
	{ },
};

/* [한국어]
 * ls_pcie_probe - 세대를 판정해 자원을 잡고, 브리지 모드일 때만 호스트를 세운다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이며 여섯 단계다.
 *
 * 1. 상태 구조체 둘을 따로 할당한다. **dw_pcie 를 값으로 품지 않고
 *    포인터로 가리키는** 구조라 할당이 두 번이며, 같은 트리의 다른
 *    글루 대부분이 값으로 품는 것과 다르다.
 * 2. 매칭 데이터로 SoC 세대를 판정하고, 그 세대의 콜백 표를 코어에 건다.
 * 3. DBI 창을 **이름으로** 매핑한다("regs").
 * 4. 엔디언을 판정하고, PF LUT 창의 위치를 계산한다. 별도 자원이 아니라
 *    DBI 창에 세대별 오프셋을 더한 자리다.
 * 5. SCFG 를 쓰는 세대라면 그 regmap 과 포트 번호를 얻는다.
 * 6. **브리지 모드인지 확인한다.** 아니면 -ENODEV 로 물러난다 — 엔드포인트
 *    모드로 설정된 컨트롤러는 별도 드라이버의 몫이기 때문이다.
 *
 * drvdata 를 6번 **뒤** 에 매다는 것이 눈에 띈다. 물러날 경우 아무 흔적도
 * 남기지 않겠다는 뜻이다.
 *
 * 되감기 코드가 없다. 잡는 것이 모두 devm 판이라 코어가 되돌린다.
 *
 * [상류 코드 관찰] 3번에서 platform_get_resource_byname() 의 결과를
 * NULL 검사 없이 넘긴다. 다만 devm_pci_remap_cfg_resource()
 * (drivers/pci/devres.c:565)가 NULL 을 명시적으로 걸러 -EINVAL 을 돌려주므로
 * 안전하다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 링크 대기와
 * 버스 스캔으로 오래 걸린다.
 *
 * 에러 경로: 할당·매핑·SCFG 확보의 실패는 그 오류를, 브리지가 아니면
 * -ENODEV 를 돌려준다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → of_device_get_match_data() → platform_get_resource_byname()
 *     → devm_pci_remap_cfg_resource()
 *     → syscon_regmap_lookup_by_phandle_args()
 *     → ls_pcie_is_bridge() → dw_pcie_host_init()
 */
static int ls_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci;
	/* [한국어] 이 드라이버의 상태 구조체. */
	struct ls_pcie *pcie;
	/* [한국어] DBI 창 자원을 받을 자리. */
	struct resource *dbi_base;
	/* [한국어] SCFG phandle 의 인자 둘을 받을 배열. 두 번째가 포트 번호다. */
	u32 index[2];

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!pcie)
		/* [한국어] 아직 잡은 것이 없어 곧바로 돌아간다. */
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	/* [한국어] DWC 문맥도 못 만들었다. */
	if (!pci)
		/* [한국어] 앞서 할당한 것은 devm 판이라 코어가 정리한다. */
		return -ENOMEM;

	pcie->drvdata = of_device_get_match_data(dev);

	pci->dev = dev;
	/* [한국어] 두 구조체를 서로 연결한다. */
	pcie->pci = pci;
	/* [한국어] 세대별 콜백 표를 코어에 건다. **아래 단계가 실패하더라도 코어가
	 * 잘못된 표를 볼 일이 없게 하는 배치다.** */
	pci->pp.ops = pcie->drvdata->ops;

	dbi_base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "regs");
	/* [한국어] 자원을 매핑한다. 앞 줄의 결과를 NULL 검사 없이 넘기는데, 이 함수
	 * (drivers/pci/devres.c:565)가 NULL 을 명시적으로 걸러 -EINVAL 을 돌려주므로
	 * 안전하다. */
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, dbi_base);
	/* [한국어] 매핑이 실패했다. */
	if (IS_ERR(pci->dbi_base))
		/* [한국어] 그 오류를 올려보낸다. */
		return PTR_ERR(pci->dbi_base);

	pcie->big_endian = of_property_read_bool(dev->of_node, "big-endian");
/* [한국어] 빅 엔디언 여부를 판정한다. 같은 SoC 계열이라도 보드마다 다를 수
 * 있어 디바이스 트리로 알려 준다. */

	pcie->pf_lut_base = pci->dbi_base + pcie->drvdata->pf_lut_off;
/* [한국어] PF LUT 창의 위치를 계산한다. **매핑을 따로 하지 않는다** — DBI 창
 * 안의 한 지점이기 때문이다. */

	if (pcie->drvdata->scfg_support) {
		pcie->scfg =
			/* [한국어] SCFG regmap 과 인자 둘을 함께 얻는다. 인자 개수를 1 로 지정하는데,
			 * 배열은 둘을 받아 두 번째에 포트 번호가 들어온다. */
			syscon_regmap_lookup_by_phandle_args(dev->of_node,
							     "fsl,pcie-scfg", 1,
							     index);
		if (IS_ERR(pcie->scfg)) {
			/* [한국어] SCFG phandle 이 디바이스 트리에 없다. */
			dev_err(dev, "No syscfg phandle specified\n");
			/* [한국어] 이 세대는 SCFG 없이 동작할 수 없으므로 그 오류를 올려보낸다. */
			return PTR_ERR(pcie->scfg);
		}

		/* [한국어] phandle 인자의 두 번째 값이 이 컨트롤러의 포트 번호다. SCFG 방식의
		 * 레지스터·비트 계산이 모두 이 값에 기댄다. */
		pcie->index = index[1];
	}

	if (!ls_pcie_is_bridge(pcie))
		/* [한국어] 엔드포인트 모드로 설정돼 있다. 이 파일이 맡을 장치가 아니므로
		 * -ENODEV 로 비켜 준다 — 별도의 엔드포인트 드라이버가 맡는다. */
		return -ENODEV;

	platform_set_drvdata(pdev, pcie);
/* [한국어] **브리지 확인을 통과한 뒤에야** drvdata 를 매단다. 물러날 경우
 * 아무 흔적도 남기지 않겠다는 뜻이다. */

	return dw_pcie_host_init(&pci->pp);
}

/* [한국어]
 * ls_pcie_suspend_noirq - 절전 진입을 DWC 코어에 넘긴다 (지원 세대만)
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 = 성공 또는 미지원, 음수 오류.
 *
 * 절전을 지원하지 않는 세대라면 **아무것도 하지 않고 성공으로 답한다.**
 * 지금은 매칭 데이터 셋이 모두 지원한다고 표시하고 있어 이 갈래는
 * 실제로 쓰이지 않지만, 표에 항목이 늘 때를 대비한 자리다.
 *
 * 지원하는 경우 코어의 공용 구현을 부르며, SoC 고유의 일은 그 안에서
 * 불리는 pme_turn_off 콜백을 통해 이뤄진다. 세대별로 다른 그 콜백이
 * 이 파일의 L2 진입 갈래 셋 중 하나가 된다.
 *
 * 실행 컨텍스트: 시스템 절전의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: 코어의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.suspend_noirq == [이 함수] → dw_pcie_suspend_noirq()
 */
static int ls_pcie_suspend_noirq(struct device *dev)
{
	struct ls_pcie *pcie = dev_get_drvdata(dev);

	if (!pcie->drvdata->pm_support)
		/* [한국어] 절전을 지원하지 않는 세대다. 아무것도 하지 않고 성공으로 답한다.
		 * 지금은 세 표가 모두 지원한다고 표시해 이 갈래가 쓰이지 않는다. */
		return 0;

	return dw_pcie_suspend_noirq(pcie->pci);
}

/* [한국어]
 * ls_pcie_resume_noirq - 링크를 L2 에서 꺼낸 뒤 코어의 복귀를 부른다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 = 성공 또는 미지원, 음수 오류.
 *
 * 위 진입 함수의 짝이지만 **구조가 대칭이 아니다.** 진입은 코어에 통째로
 * 맡기는데, 복귀는 코어를 부르기 **전에** 이 파일이 먼저 일을 한다.
 *
 * 순서가 이래야 하는 이유가 분명하다. 링크가 L2 에 있으면 config 접근이
 * 통하지 않으므로, 코어가 복귀 작업을 시작하기 전에 링크를 깨워 두어야
 * 한다. 반대로 하면 죽은 링크에 접근하게 된다.
 *
 * exit_from_l2 는 세대별로 다른 함수가 걸려 있으며, 그 결과를 확인해
 * 실패하면 **코어의 복귀를 아예 시작하지 않는다.** 진입 쪽 콜백이 void 라
 * 실패를 알리지 못하는 것과 대비되는 대목이다.
 *
 * 실행 컨텍스트: 시스템 복귀의 noirq 단계. 프로세스 컨텍스트.
 *
 * 에러 경로: L2 탈출이 실패하면 그 오류를 올려보내고 코어를 부르지 않는다.
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops.resume_noirq == [이 함수]
 *     → drvdata->exit_from_l2() → dw_pcie_resume_noirq()
 */
static int ls_pcie_resume_noirq(struct device *dev)
{
	struct ls_pcie *pcie = dev_get_drvdata(dev);
	int ret;

	if (!pcie->drvdata->pm_support)
		/* [한국어] 절전을 지원하지 않는 세대다. */
		return 0;

	ret = pcie->drvdata->exit_from_l2(&pcie->pci->pp);
	/* [한국어] 링크를 L2 에서 꺼내지 못했다. */
	if (ret)
		/* [한국어] **코어의 복귀를 아예 시작하지 않는다.** 죽은 링크에 config 접근을
		 * 하면 시스템이 멈출 수 있기 때문이다. */
		return ret;

	return dw_pcie_resume_noirq(pcie->pci);
}

static const struct dev_pm_ops ls_pcie_pm_ops = {
	/* [한국어] 절전 진입·복귀를 noirq 단계에 건다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(ls_pcie_suspend_noirq, ls_pcie_resume_noirq)
};

/* [한국어]
 * ls_pcie_remove - DWC 호스트를 내린다
 *
 * @pdev: 플랫폼 장치.
 *
 * 한 줄짜리 함수다. 컨트롤러를 정지시키거나 자원을 되돌리는 코드가 없는데,
 * 이 드라이버가 애초에 클럭·PHY·리셋을 잡지 않고 나머지는 모두 devm 판이라
 * 되돌릴 것이 없기 때문이다.
 *
 * **같은 트리의 다른 글루들과 대비된다.** pci-exynos.c 의 remove 는 호스트를
 * 내린 뒤 코어 리셋을 걸고 PHY 와 전원까지 차례로 끄는데, 여기는 그 모두가
 * 펌웨어 소관이라 할 일이 없다.
 *
 * 호스트를 내리면 그 아래 장치들의 드라이버가 먼저 정리되고 버스가
 * 해체된다.
 *
 * suppress_bind_attrs 가 참이라 sysfs 로 언바인드할 수 없으므로, 이 함수는
 * 사실상 모듈 제거 때만 불린다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수] → dw_pcie_host_deinit()
 */
static void ls_pcie_remove(struct platform_device *pdev)
{
	struct ls_pcie *pcie = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&pcie->pci->pp);
}

static struct platform_driver ls_pcie_driver = {
	/* [한국어] 장치가 붙을 때 부를 진입점. */
	.probe = ls_pcie_probe,
	/* [한국어] 장치가 떨어질 때 부를 해제 함수. 한 줄짜리다. */
	.remove = ls_pcie_remove,
	.driver = {
		.name = "layerscape-pcie",
		/* [한국어] 디바이스 트리 매칭 표를 건다. */
		.of_match_table = ls_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = &ls_pcie_pm_ops,
	},
};
module_platform_driver(ls_pcie_driver);

MODULE_AUTHOR("Minghuan Lian <Minghuan.Lian@freescale.com>");
MODULE_DESCRIPTION("Layerscape PCIe host controller driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, ls_pcie_of_match);
