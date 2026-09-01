// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the PCIe Controller in QiLai from Andes
 *
 * Copyright (C) 2026 Andes Technology Corporation
 */

/* [한국어] FIELD_GET() / FIELD_MODIFY() — 마스크 하나로 비트 필드를 읽고 쓰는 매크로.
 * 이 파일은 레지스터 비트를 손으로 시프트하지 않고 전부 이 방식으로 다룬다. */
/*
 * [한국어 설명] Andes QiLai SoC 의 DesignWare PCIe 루트 포트 드라이버 (pcie-andes-qilai.c)
 *
 * === 파일의 역할 ===
 * Andes Technology 의 QiLai SoC 에 들어간 PCIe 컨트롤러를 다룬다. 컨트롤러
 * IP 자체는 Synopsys DesignWare 이므로, 이 파일이 하는 일은 그 공용 코어에
 * SoC 고유 부분만 끼워 넣는 것이다. 197줄, 함수 일곱 개로 DWC 계열 드라이버
 * 중에서도 특히 얇다.
 * 끼워 넣는 SoC 고유 부분은 정확히 네 가지다. (1) 링크가 섰는지 읽는 방법,
 * (2) 링크 훈련을 시작시키는 방법, (3) MSI 인터럽트가 CPU 까지 오게 하는
 * 래퍼 게이트 열기, (4) IOCP 의 AXI 캐시 속성 설정. 그 밖의 모든 것 —
 * 클록과 리셋 확보, iATU 주소 변환, MSI 도메인 생성, 버스 열거 — 은 DWC
 * 코어가 처리하며 이 파일에는 한 줄도 없다.
 * 특히 클록·리셋 코드가 없는 것은 probe 의 dw_pcie_cap_set(REQ_RES) 한 줄
 * 때문이다. 그 capability 가 켜져 있으면 코어의 dw_pcie_get_resources() 가
 * 클록과 리셋을 대신 확보해 준다(pcie-designware.c:465).
 * 레지스터 창이 둘로 나뉘어 있다는 점이 이 파일을 읽는 열쇠다. DesignWare
 * IP 자체의 레지스터는 DBI 에 있고(코히런시 제어 0x8e8 하나만 여기서 쓴다),
 * Andes 가 IP 바깥에 덧붙인 래퍼 레지스터는 APB 창에 있다(인터럽트 제어
 * 0x15c, 일반 제어 0x54, SII 전원 상태 0xc0). 접근자가 달라지는 것도
 * 그 때문이다 — DBI 는 dw_pcie_readl_dbi(), APB 는 평범한 readl().
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 PCI 스택에서 이 파일은 맨 아래, 하드웨어 바로 위에 있다.
 * 위로는 pcie-designware-host.c 의 dw_pcie_host_init() 이 초기화 전체를
 * 지휘하고, 그 위로 PCI 코어의 pci_host_probe() 가 버스를 열거하며,
 * 다시 그 위에 nvme 같은 장치 드라이버가 붙는다.
 * 제어 흐름은 위임과 콜백의 왕복이다. probe 가 판을 깔고
 * dw_pcie_host_init() 을 부르면, 그 안에서 코어가 이 파일의 네 콜백을
 * 되부른다 — 먼저 pp->ops->init(MSI 게이트 열기), 이어서 자원 준비와
 * pci->ops->start_link(LTSSM 켜기), 그 뒤 pci->ops->link_up 을 폴링하며 대기,
 * 열거가 끝난 뒤 마지막으로 pp->ops->post_init(캐시 정책).
 * 그 네 콜백의 순서가 이 드라이버 초기화의 전부라고 해도 과언이 아니다.
 * MSI 게이트는 코어가 인터럽트를 설정하기 전에 열려야 하고, 캐시 정책은
 * 버스 열거가 끝난 뒤에 걸어야 하기 때문에 전(前)·후(後) 훅으로 나뉜다.
 * 실행 컨텍스트는 전부 probe 경로의 프로세스 컨텍스트다. 다만 link_up 만은
 * 코어의 폴링 루프 안에서 불리므로 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-designware.h 의 struct dw_pcie / dw_pcie_ops / dw_pcie_rp /
 * dw_pcie_host_ops 규약, 그리고 pcie-designware-host.c 의 dw_pcie_host_init().
 * post_init 훅이 불리는 자리는 pcie-designware-host.c:1628 로,
 * pci_host_probe() 바로 다음이다.
 * 옆쪽: pcie-designware.c 의 dw_pcie_get_resources() 가 REQ_RES 를 보고
 * 클록·리셋을 확보하고(:465), use_parent_dt_ranges 를 보고 주소 오프셋
 * 경고를 낼지 정한다(:2966 및 헤더 :2101 의 설명).
 * 아래쪽: readl/writel(APB 창)과 dw_pcie_readl_dbi/writel_dbi(DBI 창),
 * dw_pcie_dbi_ro_wr_en/dis(읽기 전용 잠금), 그리고 런타임 PM API.
 * 하드웨어 쪽: AMBA AXI 규격의 AxCACHE 인코딩(표 A4-5). IOCP(IO Coherence
 * Port)가 그 값을 실어 DMA 트랜잭션을 SoC 최종 레벨 캐시(L2)로 보낸다.
 * 데이터 흐름: DT 의 "apb" reg 항목 → probe 가 매핑 → apb_base →
 * 세 함수가 래퍼 레지스터를 읽고 씀. 그와 별개로 DBI 경로는 DWC 코어가
 * 소유하며 이 파일은 코히런시 레지스터 하나만 거기서 만진다.
 * 공유 상태: struct qilai_pcie 하나. 첫 필드가 struct dw_pcie 라
 * container_of 되돌리기(to_qilai_pcie)가 성립하며, 코어가 콜백에 dw_pcie 만
 * 넘겨 주는 규약과 이 배치가 맞물린다. 전역 변수는 상수 표 셋뿐이다.
 * NVMe 와의 접점: 직접적인 코드 연결은 없다. 다만 IOCP 캐시 정책을 되쓰기 +
 * 읽기·쓰기 할당으로 거는 것은 NVMe 처럼 DMA 를 많이 하는 장치에서
 * 캐시 미스로 인한 메모리 왕복을 줄이는 방향의 설정이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct qilai_pcie: 필드 둘. 첫 필드 pci(struct dw_pcie)가 코어와의
 *   접점이자 container_of 앵커이고, apb_base 가 SoC 래퍼 창이다.
 * - qilai_pcie_link_up(): SMLH(물리 계층)와 RDLH(데이터 링크 계층) 두 비트를
 *   AND 로 묶는다. 물리 계층만 서면 TLP 를 보낼 수 없기 때문이다.
 * - qilai_pcie_start_link(): APB 의 LTSSM 허용 비트 하나를 세우는 것이 전부다.
 *   언제나 0 을 반환하며, 훈련 성공 여부는 코어가 link_up 폴링으로 판단한다.
 * - qilai_pcie_iocp_cache_setup(): 이 파일에서 유일하게 DBI 를 만지는 함수.
 *   dbi_ro_wr_en/dis 로 감싸고 MODE·VALUE 네 필드를 FIELD_MODIFY 로 고친 뒤
 *   쓰기는 한 번만 해서 중간 상태를 하드웨어에 보이지 않게 한다.
 * - qilai_pcie_enable_msi(): DWC 코어가 MSI 를 다 준비해도 이 래퍼 비트가
 *   꺼져 있으면 인터럽트가 CPU 까지 오지 않는다. 코어는 이 비트의 존재를
 *   알 수 없으므로 SoC 드라이버가 켠다.
 * - qilai_pcie_host_init() / qilai_pcie_host_post_init(): 전·후 두 훅.
 *   전자는 코어의 인터럽트 설정 전에, 후자는 버스 열거 후에 불린다.
 * - qilai_pcie_probe(): 상태 할당 → ops 두 표 연결 → use_parent_dt_ranges 와
 *   REQ_RES 설정 → "apb" 창 매핑 → 런타임 PM → dw_pcie_host_init() 위임.
 * - 레지스터 상수: APB 쪽 PCIE_INTR_CONTROL1(0x15c) / PCIE_GEN_CONTROL2(0x54) /
 *   PCIE_REGS_PCIE_SII_PM_STATE(0xc0), DBI 쪽
 *   PCIE_LOGIC_COHERENCY_CONTROL3(0x8e8). 창이 다르면 접근자도 다르다.
 */

#include <linux/bitfield.h>
/* [한국어] BIT() 과 GENMASK() — 아래 레지스터 비트 정의가 모두 이 둘로 되어 있다. */
#include <linux/bits.h>
/* [한국어] 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] MODULE_AUTHOR/DESCRIPTION/LICENSE 와 MODULE_DEVICE_TABLE 매크로. */
#include <linux/module.h>
/* [한국어] PCI 코어 공개 API. */
#include <linux/pci.h>
/* [한국어] platform_driver, devm_platform_ioremap_resource_byname(). */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_set_active() / pm_runtime_no_callbacks() / devm_pm_runtime_enable(). */
#include <linux/pm_runtime.h>
/* [한국어] 기본 타입 정의. */
#include <linux/types.h>

/* [한국어] DesignWare PCIe 코어 헤더. struct dw_pcie, dw_pcie_ops, dw_pcie_rp,
 * dw_pcie_host_ops, dw_pcie_host_init(), DBI 접근자가 모두 여기서 온다.
 * 이 드라이버는 그 코어에 SoC 고유 부분만 끼워 넣는 형태다. */
#include "pcie-designware.h"

/* [한국어] 인터럽트 제어 레지스터 1 의 APB 오프셋. DBI 가 아니라 APB 창 기준이다. */
#define PCIE_INTR_CONTROL1			0x15c
/* [한국어] 그 레지스터의 MSI 인터럽트 허용 비트(28번). 이 비트를 켜야 DWC 코어가
 * 만든 MSI 가 실제로 CPU 까지 올라온다. */
#define PCIE_MSI_CTRL_INT_EN			BIT(28)

/* [한국어] 코히런시 제어 레지스터 3 의 **DBI** 오프셋. 위 두 개와 달리 이것은
 * DesignWare 코어의 config 공간(DBI)에 있어 접근자가 다르다. */
#define PCIE_LOGIC_COHERENCY_CONTROL3		0x8e8

/*
 * Refer to Table A4-5 (Memory type encoding) in the
 * AMBA AXI and ACE Protocol Specification.
 *
 * The selected value corresponds to the Memory type field:
 * "Write-back, Read and Write-allocate".
 *
 * The last three rows in the table A4-5 in
 * AMBA AXI and ACE Protocol Specification:
 * ARCACHE        AWCACHE        Memory type
 * ------------------------------------------------------------------
 * 1111 (0111)    0111           Write-back Read-allocate
 * 1011           1111 (1011)    Write-back Write-allocate
 * 1111           1111           Write-back Read and Write-allocate (selected)
 */
/* [한국어] AR(읽기) 트랜잭션에 실을 AxCACHE 값. 위 영어 주석의 표에서 마지막 행,
 * 즉 쓰기 되쓰기 + 읽기·쓰기 할당(Write-back, Read and Write-allocate)에
 * 해당하는 인코딩이다. */
#define IOCP_ARCACHE				0b1111
/* [한국어] AW(쓰기) 트랜잭션 쪽도 같은 값. 읽기와 쓰기 모두 같은 캐시 정책을 쓴다. */
#define IOCP_AWCACHE				0b1111

/* [한국어] AR 캐시 모드 필드(6~3비트). "모드" 는 어느 비트를 아래 VALUE 로 덮어쓸지
 * 고르는 마스크로 쓰인다 — 모드와 값을 같은 인코딩으로 함께 쓰는 것이
 * 이 하드웨어의 방식이다. */
#define PCIE_CFG_MSTR_ARCACHE_MODE		GENMASK(6, 3)
/* [한국어] AW 캐시 모드 필드(14~11비트). */
#define PCIE_CFG_MSTR_AWCACHE_MODE		GENMASK(14, 11)
/* [한국어] AR 캐시 값 필드(22~19비트). 실제로 트랜잭션에 실릴 값이 여기 들어간다. */
#define PCIE_CFG_MSTR_ARCACHE_VALUE		GENMASK(22, 19)
/* [한국어] AW 캐시 값 필드(30~27비트). */
#define PCIE_CFG_MSTR_AWCACHE_VALUE		GENMASK(30, 27)

/* [한국어] 일반 제어 레지스터 2 의 APB 오프셋. */
#define PCIE_GEN_CONTROL2			0x54
/* [한국어] LTSSM(Link Training and Status State Machine) 허용 비트. 이 비트를 켜는
 * 순간 하드웨어가 링크 훈련을 시작한다 — start_link 콜백이 하는 일의 전부다. */
#define PCIE_CFG_LTSSM_EN			BIT(0)

/* [한국어] SII(System Interface) 전원 상태 레지스터의 APB 오프셋. 링크 상태를
 * 여기서 읽는다. */
#define PCIE_REGS_PCIE_SII_PM_STATE		0xc0
/* [한국어] 물리 계층(MAC) 링크 업 비트(6번). */
#define SMLH_LINK_UP				BIT(6)
/* [한국어] 데이터 링크 계층 링크 업 비트(7번). 링크가 쓸 만하려면 둘 다 서야 하며,
 * link_up 콜백이 두 비트를 AND 로 묶는 이유가 그것이다. */
#define RDLH_LINK_UP				BIT(7)

struct qilai_pcie {
	/* [한국어] DesignWare 코어가 다루는 공용 컨트롤러 객체. 이 구조체의 **첫 필드**라
	 *   container_of 로 되돌리는 아래 to_qilai_pcie() 가 성립한다.
	 * 설정자: probe 가 dev / ops / pp.ops / use_parent_dt_ranges 를 채우고,
	 *   이후로는 DWC 코어가 소유해 나머지 필드를 채운다.
	 * 읽는 자: DWC 코어 전체, 그리고 이 파일의 콜백들이 to_qilai_pcie() 로
	 *   자기 자신을 되찾는 출발점.
	 * 값 범위: probe 에서 devm_kzalloc 으로 0 초기화된 뒤 채워진다.
	 * 동기화: probe 시점에만 쓰고 이후에는 DWC 코어의 규약을 따른다. */
	struct dw_pcie pci;
	/* [한국어] APB(Advanced Peripheral Bus) 레지스터 창의 매핑된 주소.
	 * 설정자: probe 가 devm_platform_ioremap_resource_byname(pdev, "apb") 로 채운다.
	 * 읽는 자: qilai_pcie_link_up() / start_link() / enable_msi() 세 함수.
	 * 값 범위: 유효한 __iomem 포인터. 실패는 IS_ERR 로 probe 에서 걸러진다.
	 * 동기화: probe 이후 읽기 전용이라 락이 없다.
	 * 이 창이 DBI 와 별개라는 점이 중요하다 — SoC 가 DesignWare IP 바깥에
	 *   덧붙인 래퍼 레지스터들이 여기 모여 있고, IP 자체의 레지스터는 DBI 에 있다. */
	void __iomem *apb_base;
};

/* [한국어] dw_pcie 포인터에서 그것을 품은 qilai_pcie 를 되찾는다. DWC 코어의 콜백은
 * 언제나 struct dw_pcie 만 넘겨 주므로, SoC 드라이버가 자기 상태(apb_base)에
 * 닿으려면 이 되돌리기가 필요하다. pci 가 첫 필드라 오프셋은 0 이다. */
#define to_qilai_pcie(_pci) container_of(_pci, struct qilai_pcie, pci)

/* [한국어]
 * qilai_pcie_link_up - 링크가 실제로 올라왔는지 APB 레지스터로 확인한다
 *
 * @pci: DWC 코어가 넘긴 공용 컨트롤러 객체.
 * @return: true = 물리 계층과 데이터 링크 계층이 모두 올라옴, false = 아직.
 *
 * DWC 코어는 링크 상태를 읽는 방법을 모른다. SoC 마다 그 정보를 노출하는
 * 레지스터가 다르기 때문이며, 이 콜백이 그 차이를 메운다.
 *
 * 두 비트를 AND 로 묶는 것이 핵심이다. SMLH_LINK_UP 은 물리 계층(MAC)이,
 * RDLH_LINK_UP 은 데이터 링크 계층이 올라왔다는 뜻인데, 물리 계층만 서고
 * 데이터 링크가 아직이면 TLP 를 보낼 수 없다. 둘 다 서야 config 접근이
 * 의미를 갖는다.
 *
 * 읽는 곳이 DBI 가 아니라 APB 창이라는 점도 이 드라이버의 구조를 보여 준다 —
 * DesignWare IP 자체의 레지스터는 DBI 에 있고, SoC 가 덧붙인 래퍼 레지스터는
 * APB 에 있다.
 *
 * 실행 컨텍스트: dw_pcie_wait_for_link() 의 폴링 루프. 잠들지 않는다.
 *
 * 에러 경로: 없다. 실패는 false 로 표현되고, 판단은 호출자가 한다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_wait_for_link() → pci->ops->link_up
 *     → [이 함수] → readl(apb_base + PCIE_REGS_PCIE_SII_PM_STATE)
 */
static bool qilai_pcie_link_up(struct dw_pcie *pci)
{
	/* [한국어] 콜백이 받은 공용 객체에서 이 드라이버의 상태를 되찾는다. */
	struct qilai_pcie *pcie = to_qilai_pcie(pci);
	/* [한국어] 읽어 올 레지스터 값. */
	u32 val;

	/* [한국어] APB 창의 전원 상태 레지스터를 읽는다. DBI 가 아니라 APB 인 이유는
	 * 링크 상태를 SoC 래퍼가 따로 노출하기 때문이다. */
	val = readl(pcie->apb_base + PCIE_REGS_PCIE_SII_PM_STATE);

	/* [한국어] 두 계층이 **모두** 올라와야 링크가 선 것으로 본다. 물리 계층만 서고
	 * 데이터 링크가 아직이면 TLP 를 보낼 수 없으므로, AND 로 묶는 것이 맞다. */
	return FIELD_GET(SMLH_LINK_UP, val) && FIELD_GET(RDLH_LINK_UP, val);
}

/* [한국어]
 * qilai_pcie_start_link - LTSSM 을 켜서 링크 훈련을 시작시킨다
 *
 * @pci: DWC 코어가 넘긴 공용 컨트롤러 객체.
 * @return: 언제나 0.
 *
 * LTSSM(Link Training and Status State Machine)은 링크 속도와 레인 폭을
 * 상대와 협상하는 하드웨어 상태 기계다. 이 함수는 APB 의 허용 비트 하나를
 * 세우는 것이 전부이고, 그 뒤로는 하드웨어가 알아서 진행한다.
 *
 * 언제나 0 을 반환하는 것이 이상해 보이지만 규약상 맞다. 이 함수는 "시작하라"
 * 는 지시를 내릴 뿐이고, 훈련이 실제로 성공했는지는 DWC 코어가
 * qilai_pcie_link_up() 을 폴링해 판단한다. 두 콜백이 한 쌍으로 동작한다.
 *
 * 읽기-수정-쓰기로 비트를 세우는 이유는 같은 레지스터의 다른 비트를 보존해야
 * 하기 때문이다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 훈련 실패는 이후 link_up 폴링의 시간 초과로 드러난다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → dw_pcie_start_link() → pci->ops->start_link
 *     → [이 함수] → readl/writel(apb_base + PCIE_GEN_CONTROL2)
 */
static int qilai_pcie_start_link(struct dw_pcie *pci)
{
	/* [한국어] 이 드라이버의 상태를 되찾는다. */
	struct qilai_pcie *pcie = to_qilai_pcie(pci);
	/* [한국어] 읽고 고칠 값. */
	u32 val;

	/* [한국어] 일반 제어 레지스터 2 를 읽는다. */
	val = readl(pcie->apb_base + PCIE_GEN_CONTROL2);
	/* [한국어] LTSSM 허용 비트를 세운다. */
	val |= PCIE_CFG_LTSSM_EN;
	/* [한국어] 되쓴다. 이 쓰기가 링크 훈련의 방아쇠다 — 이후 하드웨어가 스스로
	 * 속도와 레인 폭을 협상하고, DWC 코어는 link_up 콜백을 폴링하며 기다린다. */
	writel(val, pcie->apb_base + PCIE_GEN_CONTROL2);

	/* [한국어] 언제나 성공을 보고한다. 훈련이 실제로 성공했는지는 link_up 이 답한다. */
	return 0;
}

static const struct dw_pcie_ops qilai_pcie_ops = {
	/* [한국어] 링크가 섰는지 묻는 콜백. DWC 코어가 dw_pcie_wait_for_link() 에서 폴링한다. */
	.link_up = qilai_pcie_link_up,
	/* [한국어] 링크 훈련을 시작하는 콜백. 이 표에 둘만 있고 stop_link 가 없다는 것은
	 * 이 드라이버가 링크를 되돌릴 일이 없다는 뜻이다. */
	.start_link = qilai_pcie_start_link,
};

/*
 * Set up the QiLai PCIe IOCP (IO Coherence Port) Read/Write Behaviors to the
 * Write-Back, Read and Write Allocate mode.
 *
 * The IOCP HW target is SoC last-level cache (L2 Cache), which serves as the
 * system cache. The IOCP HW helps maintain cache monitoring, ensuring that
 * the device can snoop data from/to the cache.
 */
/* [한국어]
 * qilai_pcie_iocp_cache_setup - IOCP 의 AXI 캐시 속성을 되쓰기·할당 모드로 건다
 *
 * @pp: DWC 코어의 루트 포트 객체. 여기서 dw_pcie 를 거쳐 DBI 접근자를 얻는다.
 *
 * 위 영어 주석이 배경을 밝힌다. IOCP(IO Coherence Port)는 PCIe 장치의 DMA 가
 * SoC 최종 레벨 캐시(L2)를 거치게 해 주는 하드웨어이고, 그 덕분에 장치가
 * 캐시 안의 데이터를 스누프할 수 있다. 이 함수는 그 포트가 AXI 버스에 실을
 * AxCACHE 인코딩을 정한다.
 *
 * AMBA AXI 표 A4-5 에서 고른 값이 "Write-back, Read and Write-allocate" 다.
 * 읽기와 쓰기 모두 캐시 라인을 할당하고 되쓰기 정책을 쓴다는 뜻으로,
 * NVMe 처럼 DMA 를 많이 하는 장치에서 캐시 미스로 인한 메모리 왕복을 줄인다.
 *
 * MODE 와 VALUE 두 쌍, 총 네 필드를 고치는 것이 이 하드웨어의 방식이다.
 * MODE 는 어느 비트를 덮어쓸지 고르고 VALUE 는 실제 실릴 값인데, 여기서는
 * 둘에 같은 인코딩을 넣는다. 네 번의 FIELD_MODIFY 가 지역 변수를 고친 뒤
 * 쓰기는 한 번뿐이라, 중간 상태가 하드웨어에 보이지 않는다.
 *
 * dw_pcie_dbi_ro_wr_en/dis 로 감싸는 이유는 이 레지스터가 config 공간 규약상
 * 읽기 전용 영역에 있기 때문이다. 열어 두고 나가면 이후 평범한 config 쓰기가
 * 읽기 전용 레지스터를 건드릴 수 있으므로 반드시 닫는다.
 *
 * 실행 컨텍스트: post_init 훅. 버스 열거가 끝난 뒤 프로세스 컨텍스트에서 불린다.
 *
 * 에러 경로: 없다. 반환값도 없고 실패를 감지할 방법도 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → pci_host_probe() → pp->ops->post_init
 *     → qilai_pcie_host_post_init() → [이 함수]
 *     → dw_pcie_dbi_ro_wr_en() → dw_pcie_readl_dbi() → FIELD_MODIFY ×4
 *     → dw_pcie_writel_dbi() → dw_pcie_dbi_ro_wr_dis()
 */
static void qilai_pcie_iocp_cache_setup(struct dw_pcie_rp *pp)
{
	/* [한국어] dw_pcie_rp 에서 그것을 품은 dw_pcie 를 얻는다. DWC 코어가 제공하는
	 * 역방향 변환이다. */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 읽고 고칠 값. */
	u32 val;

	/* [한국어] DBI 의 읽기 전용 레지스터에 쓸 수 있게 잠금을 푼다. 아래 코히런시
	 * 레지스터가 표준 config 공간 규약상 읽기 전용 영역에 있기 때문이다. */
	dw_pcie_dbi_ro_wr_en(pci);

	/* [한국어] 현재 값을 읽는다. 네 필드만 고칠 것이므로 나머지는 보존해야 한다. */
	val = dw_pcie_readl_dbi(pci, PCIE_LOGIC_COHERENCY_CONTROL3);
	/* [한국어] AR 캐시 모드 필드를 IOCP_ARCACHE 로 바꾼다. FIELD_MODIFY 는 마스크가
	 * 가리키는 자리만 새 값으로 갈아 끼운다(이 트리에는 그 정의 헤더가 없다). */
	FIELD_MODIFY(PCIE_CFG_MSTR_ARCACHE_MODE, &val, IOCP_ARCACHE);
	/* [한국어] AW 캐시 모드 필드도 같은 값으로. */
	FIELD_MODIFY(PCIE_CFG_MSTR_AWCACHE_MODE, &val, IOCP_AWCACHE);
	/* [한국어] AR 캐시 값 필드. 모드와 값에 같은 인코딩을 넣는 것이 이 하드웨어 방식이다. */
	FIELD_MODIFY(PCIE_CFG_MSTR_ARCACHE_VALUE, &val, IOCP_ARCACHE);
	/* [한국어] AW 캐시 값 필드. */
	FIELD_MODIFY(PCIE_CFG_MSTR_AWCACHE_VALUE, &val, IOCP_AWCACHE);
	/* [한국어] 네 필드를 한꺼번에 되쓴다. 읽기-수정-쓰기를 한 번으로 묶어
	 * 중간 상태가 하드웨어에 보이지 않게 한다. */
	dw_pcie_writel_dbi(pci, PCIE_LOGIC_COHERENCY_CONTROL3, val);

	/* [한국어] 다시 잠근다. 열어 둔 채로 두면 이후 평범한 config 쓰기가 읽기 전용
	 * 레지스터를 건드릴 수 있다. */
	dw_pcie_dbi_ro_wr_dis(pci);
}

/* [한국어]
 * qilai_pcie_enable_msi - SoC 래퍼의 MSI 인터럽트 허용 비트를 켠다
 *
 * @pcie: 이 드라이버의 상태. apb_base 만 쓴다.
 *
 * DWC 코어가 MSI 컨트롤러와 IRQ 도메인을 모두 준비해 주지만, 그렇게 만들어진
 * 인터럽트가 CPU 까지 올라오려면 SoC 래퍼 쪽 게이트가 열려 있어야 한다.
 * 이 함수가 그 게이트 하나를 연다.
 *
 * 읽기-수정-쓰기인 이유는 같은 인터럽트 제어 레지스터에 다른 비트들이 있기
 * 때문이다. 통째로 쓰면 그것들이 지워진다.
 *
 * DWC 코어가 아니라 SoC 드라이버가 이 일을 하는 이유는, 이 비트가 DesignWare
 * IP 의 레지스터가 아니라 Andes 가 그 바깥에 덧붙인 APB 레지스터이기 때문이다.
 * 코어는 그 존재를 알 수 없다.
 *
 * 실행 컨텍스트: init 훅. probe 경로의 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → pp->ops->init → qilai_pcie_host_init()
 *     → [이 함수] → readl/writel(apb_base + PCIE_INTR_CONTROL1)
 */
static void qilai_pcie_enable_msi(struct qilai_pcie *pcie)
{
	/* [한국어] 읽고 고칠 값. */
	u32 val;

	/* [한국어] APB 의 인터럽트 제어 레지스터를 읽는다. */
	val = readl(pcie->apb_base + PCIE_INTR_CONTROL1);
	/* [한국어] MSI 인터럽트 허용 비트를 세운다. */
	val |= PCIE_MSI_CTRL_INT_EN;
	/* [한국어] 되쓴다. DWC 코어가 MSI 컨트롤러를 이미 준비해 두었어도 이 SoC 래퍼
	 * 비트가 꺼져 있으면 인터럽트가 CPU 까지 오지 않는다. */
	writel(val, pcie->apb_base + PCIE_INTR_CONTROL1);
}

/* [한국어]
 * qilai_pcie_host_init - 루트 포트 초기화 전(前) 단계 훅
 *
 * @pp: DWC 코어의 루트 포트 객체.
 * @return: 언제나 0.
 *
 * DWC 코어가 자기 초기화를 시작하기 전에 부르는 훅이다. 여기서 하는 일은
 * MSI 래퍼 비트를 켜는 것 하나뿐이다.
 *
 * 두 단계의 변환을 거치는 것이 눈에 띈다. 코어는 dw_pcie_rp 만 넘겨 주므로
 * to_dw_pcie_from_pp() 로 공용 객체를 얻고, 다시 to_qilai_pcie() 로 이 드라이버의
 * 상태를 되찾는다. apb_base 에 닿기 위해 필요한 두 걸음이다.
 *
 * 이 훅과 post_init 훅의 순서 차이가 이 드라이버 초기화의 전부다.
 * MSI 게이트는 코어가 인터럽트를 설정하기 전에 열어야 하고, 캐시 정책은
 * 버스 열거가 끝난 뒤에 걸어야 한다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 실패할 수 있는 동작이 없어 0 을 고정 반환한다.
 *
 * 호출 체인:
 *   qilai_pcie_probe() → dw_pcie_host_init() → pp->ops->init → [이 함수]
 *     → qilai_pcie_enable_msi()
 */
static int qilai_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* [한국어] 공용 객체를 얻고, */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 거기서 이 드라이버의 상태를 되찾는다. apb_base 가 필요하기 때문이다. */
	struct qilai_pcie *pcie = to_qilai_pcie(pci);

	/* [한국어] MSI 래퍼 비트를 켠다. 이 콜백에서 하는 일은 이것 하나뿐이다. */
	qilai_pcie_enable_msi(pcie);

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * qilai_pcie_host_post_init - 루트 포트 초기화 후(後) 단계 훅
 *
 * @pp: DWC 코어의 루트 포트 객체. 그대로 넘긴다.
 *
 * DWC 코어가 pci_host_probe() 로 버스 열거까지 마친 뒤에 부르는 훅이다
 * (pcie-designware-host.c:1628). 한 줄짜리 위임 함수로,
 * qilai_pcie_iocp_cache_setup() 을 그대로 부른다.
 *
 * 굳이 나누어 둔 이유는 이 자리가 dw_pcie_host_ops 표의 항목이어야 하고,
 * 실제 작업 함수는 dw_pcie_rp 가 아니라 캐시 설정이라는 자기 이름을 갖는 편이
 * 읽기 좋기 때문이다.
 *
 * 캐시 정책을 init 이 아니라 여기서 거는 것이 중요하다. DBI 읽기 전용 잠금을
 * 푸는 작업이라, 코어의 초기화와 열거가 끝나 config 접근이 안정된 뒤에 하는
 * 편이 안전하다.
 *
 * 실행 컨텍스트: probe 경로 끝, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없어 실패를 보고할 방법 자체가 없다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() → pci_host_probe() → pp->ops->post_init → [이 함수]
 *     → qilai_pcie_iocp_cache_setup()
 */
static void qilai_pcie_host_post_init(struct dw_pcie_rp *pp)
{
	/* [한국어] IOCP 캐시 정책을 설정한다. 이것을 init 이 아니라 post_init 에서 하는
	 * 이유는 DBI 읽기 전용 잠금 해제가 DWC 코어의 초기화와 버스 열거가 끝난
	 * 뒤에 이루어져야 안전하기 때문이다 — post_init 은 pci_host_probe() 뒤에
	 * 불린다. */
	qilai_pcie_iocp_cache_setup(pp);
}

static const struct dw_pcie_host_ops qilai_pcie_host_ops = {
	/* [한국어] MSI 래퍼 비트를 켜는 전(前) 단계 훅. */
	.init = qilai_pcie_host_init,
	/* [한국어] 버스 열거 뒤 캐시 정책을 거는 후(後) 단계 훅. 두 훅의 순서 차이가
	 * 이 드라이버 초기화의 전부다. */
	.post_init = qilai_pcie_host_post_init,
};

/* [한국어]
 * qilai_pcie_probe - 드라이버 상태를 꾸리고 DWC 코어에 넘긴다
 *
 * @pdev: 매치된 플랫폼 장치.
 * @return: 0 = 성공, -ENOMEM, APB 매핑 오류, 또는 dw_pcie_host_init() 의 오류.
 *
 * 이 파일에서 유일하게 긴 함수지만, 실제로 하는 일은 "DWC 코어가 일할 수 있게
 * 판을 깔고 넘기는 것" 뿐이다. 클록도, 리셋도, iATU 도, MSI 도메인도, 버스
 * 열거도 이 함수에 없다.
 *
 * 깔아 주는 판은 다섯 가지다.
 * 1. 상태 할당과 drvdata 등록.
 * 2. ops 두 표 연결 — link_up/start_link 는 코어가 링크를 다루는 데 쓰고,
 *    init/post_init 은 코어가 SoC 고유 작업을 끼워 넣는 자리다.
 * 3. use_parent_dt_ranges = true — cpu_addr_fixup 훅이 없는 플랫폼에서 코어가
 *    내는 주소 오프셋 경고를 끄는 플래그다.
 * 4. REQ_RES capability — 이 한 줄 덕분에 코어의 dw_pcie_get_resources() 가
 *    클록과 리셋을 대신 확보한다(pcie-designware.c:465). 이 드라이버에
 *    클록·리셋 코드가 한 줄도 없는 이유가 여기 있다.
 * 5. APB 창 매핑과 런타임 PM 설정.
 *
 * APB 창을 인덱스가 아니라 "apb" 라는 이름으로 찾는 것은 DT 에서 reg 항목
 * 순서가 바뀌어도 깨지지 않게 하려는 것이다.
 *
 * 런타임 PM 설정 셋(set_active / no_callbacks / devm_enable)은 "하드웨어는
 * 이미 켜져 있고, 이 드라이버는 전원 전환을 하지 않으며, 참조 계수만
 * 쓰겠다" 는 선언이다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe, 프로세스 컨텍스트. 비동기 probe 를
 * 선호하도록 지정되어 있어 다른 드라이버 probe 와 병행될 수 있다.
 *
 * 에러 경로: 모든 할당이 devm_ 이라 되감을 것이 없다. 마지막 실패만
 * dev_err_probe() 로 감싸는데, 이 함수가 -EPROBE_DEFER 를 조용히 처리해
 * 주기 때문이다.
 *
 * 호출 체인:
 *   builtin_platform_driver → 플랫폼 버스 매치 → [이 함수]
 *     → devm_kzalloc() → dw_pcie_cap_set(REQ_RES)
 *     → devm_platform_ioremap_resource_byname("apb")
 *     → devm_pm_runtime_enable() → dw_pcie_host_init()
 *     → (코어가 start_link / link_up / init / post_init 을 되부른다)
 */
static int qilai_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 할당할 드라이버 상태. */
	struct qilai_pcie *pcie;
	/* [한국어] 그 안의 공용 객체를 가리킬 지역 포인터. */
	struct dw_pcie *pci;
	/* [한국어] 플랫폼 장치의 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 오류 코드. */
	int ret;

	/* [한국어] 드라이버 상태를 할당한다. devm_ 이라 해제는 자동이다. */
	pcie = devm_kzalloc(&pdev->dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 할당 실패면, */
	if (!pcie)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] platform_get_drvdata() 로 되찾을 수 있게 매단다. */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] 내장된 공용 객체를 가리킨다. */
	pci = &pcie->pci;
	/* [한국어] DWC 코어가 로그와 자원 조회에 쓸 device 를 넘긴다. */
	pcie->pci.dev = dev;
	/* [한국어] link_up / start_link 콜백 표를 연결한다. */
	pcie->pci.ops = &qilai_pcie_ops;
	/* [한국어] 루트 포트용 init / post_init 훅 표를 연결한다. */
	pcie->pci.pp.ops = &qilai_pcie_host_ops;
	/* [한국어] 부모 노드의 디바이스 트리 ranges 를 그대로 믿는다는 표시. cpu_addr_fixup
	 * 훅이 없는 플랫폼에서 DWC 코어가 내는 오프셋 경고를 끄는 플래그다
	 * (pcie-designware.h 의 해당 필드 주석 참조). */
	pci->use_parent_dt_ranges = true;

	/* [한국어] REQ_RES capability 를 켠다. 이렇게 하면 DWC 코어의 dw_pcie_get_resources()
	 * 가 클록과 리셋을 대신 확보해 준다(pcie-designware.c:465). 이 드라이버에
	 * 클록·리셋 코드가 한 줄도 없는 이유가 이 한 줄이다. */
	dw_pcie_cap_set(&pcie->pci, REQ_RES);

	/* [한국어] DT 에서 "apb" 라는 이름의 reg 항목을 찾아 매핑한다. 인덱스가 아니라
	 * 이름으로 찾으므로 DT 에서 순서가 바뀌어도 안전하다. */
	pcie->apb_base = devm_platform_ioremap_resource_byname(pdev, "apb");
	/* [한국어] 매핑 실패면, */
	if (IS_ERR(pcie->apb_base))
		/* [한국어] 오류 포인터를 코드로 바꿔 반환한다. */
		return PTR_ERR(pcie->apb_base);

	/* [한국어] 런타임 PM 상 이미 활성 상태라고 표시한다. 하드웨어는 이미 켜져 있기
	 * 때문에, 이렇게 해 두지 않으면 PM 코어가 상태를 잘못 추적한다. */
	pm_runtime_set_active(dev);
	/* [한국어] suspend/resume 콜백이 없음을 알린다. 이 드라이버는 런타임 PM 을
	 * 참조 계수 용도로만 쓰고 실제 전원 전환은 하지 않는다. */
	pm_runtime_no_callbacks(dev);
	/* [한국어] 런타임 PM 을 켠다. devm_ 이라 해제 시 자동으로 꺼진다. */
	devm_pm_runtime_enable(dev);

	/* [한국어] 여기서부터는 DWC 코어가 전부 맡는다 — 자원 파싱, iATU 설정, MSI 도메인
	 * 생성, start_link 호출과 링크 대기, 버스 열거, 그리고 post_init 훅까지. */
	ret = dw_pcie_host_init(&pcie->pci.pp);
	/* [한국어] 실패면, */
	if (ret)
		/* [한국어] probe 지연(-EPROBE_DEFER)이면 조용히, 진짜 오류면 메시지와 함께 반환한다. */
		return dev_err_probe(dev, ret, "Failed to initialize PCIe host\n");

	/* [한국어] 성공. */
	return 0;
}

static const struct of_device_id qilai_pcie_of_match[] = {
	/* [한국어] 이 드라이버가 맡는 유일한 compatible 문자열. */
	{ .compatible = "andestech,qilai-pcie" },
	/* [한국어] 배열 끝. */
	{},
};
/* [한국어] 모듈 자동 로딩용 별칭을 만든다. 아래 builtin_platform_driver 로 내장
 * 빌드되더라도 이 표는 남겨 둔다. */
MODULE_DEVICE_TABLE(of, qilai_pcie_of_match);

static struct platform_driver qilai_pcie_driver = {
	/* [한국어] 위 probe 함수. */
	.probe = qilai_pcie_probe,
	.driver = {
		/* [한국어] sysfs 와 로그에 보일 이름. */
		.name	= "qilai-pcie",
		/* [한국어] 위 compatible 표. */
		.of_match_table = qilai_pcie_of_match,
		/* [한국어] 비동기 probe 를 선호한다. PCIe 링크 훈련이 수십~수백 ms 걸릴 수 있어
		 * 부팅을 직렬로 붙잡지 않게 하려는 것이다. */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

/* [한국어] 모듈이 아니라 내장 드라이버로 등록한다. */
builtin_platform_driver(qilai_pcie_driver);

/* [한국어] 작성자. */
MODULE_AUTHOR("Randolph Lin <randolph@andestech.com>");
/* [한국어] 드라이버 설명. */
MODULE_DESCRIPTION("Andes QiLai PCIe driver");
/* [한국어] 라이선스. 이것이 없으면 GPL 전용 심볼을 쓸 수 없다. */
MODULE_LICENSE("GPL");
