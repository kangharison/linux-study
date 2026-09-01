// SPDX-License-Identifier: GPL-2.0+
/*
 * ACPI quirks for Tegra194 PCIe host controller
 *
 * Copyright (C) 2021 NVIDIA Corporation.
 *
 * Author: Vidya Sagar <vidyas@nvidia.com>
 */

/* [한국어] PCI 코어 공개 API — struct pci_bus, PCI_SLOT/PCI_FUNC,
 * pci_generic_config_read/write. */
/*
 * [한국어 설명] ACPI 부팅용 Tegra194 PCIe config 접근 quirk (pcie-tegra194-acpi.c)
 *
 * === 파일의 역할 ===
 * NVIDIA Tegra194 의 PCIe 호스트 컨트롤러를 ACPI 로 부팅하는 시스템에서
 * config 공간에 접근할 수 있게 해 주는 얇은 quirk 파일이다. 파일 첫머리의
 * 영어 주석이 밝히듯 "ACPI quirks" 가 전부이고, 링크 훈련이나 전원 관리 같은
 * 실제 컨트롤러 초기화는 하지 않는다 — ACPI 부팅에서는 펌웨어가 이미 그것을
 * 끝내 놓았다고 전제하기 때문이다.
 * 왜 quirk 가 필요한가: 표준 ECAM 이라면 주소 계산만으로 config 에 닿을 수
 * 있어 이런 파일이 아예 필요 없다. 그런데 이 컨트롤러는 DesignWare IP 라
 * 하위 장치의 config 접근이 iATU(주소 변환 유닛)를 거쳐야 하고, 루트 포트
 * 자신은 또 다른 블록(DBI)에 있다. 그 두 가지를 표준 ECAM 틀 안에서
 * 처리하려면 map_bus 콜백을 직접 구현하는 수밖에 없다.
 * 이 파일이 다루는 또 하나의 사실은 배치다. DT 부팅에서는 config·iATU·DBI 를
 * 각각의 reg 항목으로 받지만, ACPI 부팅에서는 MCFG 테이블이 창 하나만
 * 알려 준다. 그래서 그 창 안에서 0 / +256KB / +512KB 오프셋으로 세 블록을
 * 나눠 쓰며, 그 약속이 펌웨어의 MCFG 서술과 맞아야 동작한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층으로 보면 ACPI PCI 열거 코드 → ECAM 코어(drivers/pci/ecam.c) →
 * 이 파일의 pci_ecam_ops → 하드웨어다. DT 부팅용 본체 드라이버인
 * pcie-tegra194.c 와는 같은 하드웨어를 다루면서도 코드를 공유하지 않는다 —
 * 이 파일은 struct dw_pcie 를 전혀 쓰지 않고 pcie-designware.h 에서
 * 레지스터 오프셋 상수만 빌려 온다.
 * 진입 경로는 둘뿐이다. (1) ECAM 창이 만들어질 때 init 콜백이 한 번 불려
 * 세 블록 주소를 계산해 둔다. (2) 이후 모든 config 접근마다 map_bus 가 불려
 * 주소를 계산하고, 하위 장치라면 iATU 창 0번을 그 접근에 맞게 다시 쓴다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트이며, map_bus 는 PCI 코어의 전역
 * pci_lock 을 쥔 상태로 불린다. 창 0번을 매번 덮어쓰는 구조가 안전한 것이
 * 전적으로 그 락 덕분이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci-ecam.h 의 struct pci_ecam_ops / pci_config_window 와
 * 코어의 pci_generic_config_read/write — 읽기·쓰기는 그대로 위임하고
 * 이 파일은 map_bus 만 구현한다.
 * 옆쪽: pcie-designware.h 의 iATU 레지스터 상수(PCIE_ATU_UNROLL_BASE,
 * PCIE_ATU_LOWER_BASE/UPPER_BASE/LOWER_TARGET/UPPER_TARGET/LIMIT,
 * PCIE_ATU_REGION_CTRL1/CTRL2, PCIE_ATU_ENABLE, PCIE_ATU_TYPE_CFG0/CFG1,
 * PCIE_ATU_BUS/DEV/FUNC). 헤더를 포함하되 그 자료구조는 쓰지 않는 관계다.
 * 아래쪽: ACPI 펌웨어가 MCFG 로 서술한 MMIO 창 하나.
 * 데이터 흐름: MCFG 창 → pci_config_window.win → init 이 세 블록으로 분할 →
 * cfg->priv 에 저장 → map_bus 가 접근마다 그 값으로 주소를 계산하고
 * iATU 를 프로그래밍 → 하드웨어가 config TLP 를 만들어 링크로 내보낸다.
 * 공유 상태: struct tegra194_pcie_ecam 하나뿐이고 전역 변수는 없다.
 * iATU 창 0번이 사실상 공유 자원이지만, 위에서 말한 대로 pci_lock 이 보호한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct tegra194_pcie_ecam: 창 안의 세 블록 주소(config_base / iatu_base /
 *   dbi_base)를 담는다. 이 파일의 유일한 상태다.
 * - tegra194_acpi_init(): 창 시작에서 0 / +256KB / +512KB 를 계산해 위 구조체에
 *   담고 cfg->priv 에 매단다. ECAM 창 생성 직후 한 번만 불린다.
 * - tegra194_map_bus(): 세 갈래 분기. 범위 밖이면 NULL, 루트 포트면 DBI 에서
 *   직접(iATU 를 거치지 않아 링크 이전에도 읽을 수 있다), 하위 장치면
 *   Type 0/Type 1 을 골라 iATU 창 0번을 다시 프로그래밍한 뒤 config 창 주소를
 *   돌려준다.
 * - program_outbound_atu(): 일곱 레지스터를 순서대로 쓰되 활성화를 마지막에 둔다.
 *   config 접근에서는 "PCI 주소" 자리에 버스·장치·함수를 넣는데, config TLP 의
 *   목적지가 메모리 주소가 아니라 BDF 이기 때문이다.
 * - atu_reg_write(): UNROLL 방식 iATU 의 주소 계산을 모아 둔 헬퍼. 창마다
 *   레지스터 묶음이 독립적이라 창 선택 레지스터가 필요 없다.
 * - tegra194_pcie_ops: init 과 map_bus 만 채우고 읽기·쓰기는 코어 범용 구현을
 *   쓰는 ECAM ops. 이 파일이 export 하는 유일한 심볼이다.
 */

#include <linux/pci.h>
/* [한국어] ACPI 기반 PCI 지원. 이 파일이 DT 가 아니라 ACPI 로 부팅하는 시스템을
 * 위한 것임을 알려 주는 포함이다. */
#include <linux/pci-acpi.h>
/* [한국어] ECAM 지원 — struct pci_ecam_ops, pci_config_window.
 * ACPI 시스템은 MCFG 테이블로 ECAM 창을 알려 주므로 그 틀에 맞춰야 한다. */
#include <linux/pci-ecam.h>

/* [한국어] DesignWare PCIe 공용 헤더. iATU 레지스터 오프셋 상수
 * (PCIE_ATU_UNROLL_BASE, PCIE_ATU_LOWER_BASE, PCIE_ATU_REGION_CTRL1 등)를
 * 쓰기 위해 포함한다. 다만 이 파일은 struct dw_pcie 를 전혀 쓰지 않는다 —
 * 레지스터 지도만 빌려 쓰는 셈이다. */
#include "pcie-designware.h"

struct tegra194_pcie_ecam  {
	/* [한국어] ECAM 창 안에서 config 공간이 시작되는 가상 주소.
	 * 설정자: tegra194_acpi_init() 이 cfg->win 을 그대로 넣는다.
	 * 읽는 자: tegra194_map_bus() 가 하위 장치 접근 주소를 만들 때.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 없다. config 접근은 PCI 코어의 pci_lock 아래에서 직렬화된다. */
	void __iomem *config_base;
	/* [한국어] 같은 창 안에서 iATU 레지스터 블록이 시작되는 주소.
	 * 설정자: init 이 cfg->win + SZ_256K 로 계산한다.
	 * 읽는 자: atu_reg_write() 만이 역참조한다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 없다. */
	void __iomem *iatu_base;
	/* [한국어] 같은 창 안에서 DBI(DesignWare 자체 config) 블록이 시작되는 주소.
	 * 설정자: init 이 cfg->win + SZ_512K 로 계산한다.
	 * 읽는 자: map_bus 가 루트 포트 자신의 config 접근에 쓴다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 없다. */
	void __iomem *dbi_base;
};

/* [한국어]
 * tegra194_acpi_init - ECAM 창 안의 세 블록 주소를 계산해 매달아 둔다
 *
 * @cfg: ECAM 코어가 만든 창 객체. cfg->win 이 매핑된 창의 시작이다.
 * @return: 0 = 성공, -ENOMEM = 할당 실패.
 *
 * 이 컨트롤러의 특이점은 하나의 MMIO 창 안에 세 가지가 연속으로 들어 있다는
 * 것이다 — 맨 앞이 하위 장치용 config 접근 창, 256KB 뒤가 iATU 레지스터,
 * 512KB 뒤가 DBI(루트 포트 자신의 config)다. 그 배치를 미리 계산해 창에
 * 매달아 두면 map_bus 가 매번 다시 계산하지 않아도 된다.
 *
 * 이 배치는 펌웨어가 MCFG 테이블로 서술한 창 크기와 맞아떨어져야 성립한다.
 * DT 부팅에서는 세 영역을 각각의 reg 항목으로 받지만, ACPI 부팅에서는
 * MCFG 가 창 하나만 알려 주므로 이렇게 오프셋으로 나눠 쓴다.
 *
 * 실행 컨텍스트: ECAM 창 생성 직후, 프로세스 컨텍스트. GFP_KERNEL 할당이라
 * 잠들 수 있다.
 *
 * 에러 경로: 할당 실패 하나뿐이다. devm 이라 정리할 것이 없다.
 *
 * 호출 체인:
 *   ACPI PCI 열거 → pci_ecam_create() → pci_ecam_ops.init == [이 함수]
 */
static int tegra194_acpi_init(struct pci_config_window *cfg)
{
	/* [한국어] ECAM 창을 만든 디바이스. devm 할당의 기준이 된다. */
	struct device *dev = cfg->parent;
	/* [한국어] 이 파일이 창에 매달아 둘 private 구조체. */
	struct tegra194_pcie_ecam *pcie_ecam;

	/* [한국어] 세 주소를 담을 구조체를 0 초기화 할당한다. */
	pcie_ecam = devm_kzalloc(dev, sizeof(*pcie_ecam), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!pcie_ecam)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] config 공간은 창의 맨 앞에 있다. */
	pcie_ecam->config_base = cfg->win;
	/* [한국어] iATU 는 256KB 뒤에 있다. 세 블록이 하나의 MMIO 창 안에 연속으로 배치되어
	 * 있다는 것이 이 하드웨어의 전제이며, 그 배치는 펌웨어의 MCFG 서술과
	 * 맞춰져 있어야 한다. */
	pcie_ecam->iatu_base = cfg->win + SZ_256K;
	/* [한국어] DBI 는 512KB 뒤에 있다. */
	pcie_ecam->dbi_base = cfg->win + SZ_512K;
	/* [한국어] ECAM 코어가 map_bus 콜백에 넘겨 줄 수 있도록 창에 매달아 둔다. */
	cfg->priv = pcie_ecam;

	/* [한국어] 초기화 성공. */
	return 0;
}

/* [한국어]
 * atu_reg_write - 지정한 아웃바운드 iATU 창의 레지스터에 값을 쓴다
 *
 * @pcie_ecam: 세 블록 주소를 담은 private 구조체.
 * @index: 창 번호.
 * @val: 쓸 값.
 * @reg: 창 레지스터 블록 안에서의 오프셋(PCIE_ATU_LOWER_BASE 등).
 *
 * UNROLL 방식 iATU 의 주소 계산을 한 곳으로 모은 헬퍼다. UNROLL 은 창마다
 * 레지스터 묶음이 독립적으로 존재하는 배치로, 예전의 뷰포트(viewport) 방식과
 * 달리 "지금 어느 창을 볼지" 고르는 선택 레지스터가 없다. 그래서 창 번호가
 * 곧 주소 오프셋이 되고, 서로 다른 창을 만지는 코드끼리 경쟁하지 않는다.
 *
 * 실행 컨텍스트: config 접근 경로. PCI 코어의 pci_lock 아래에서 불린다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   program_outbound_atu() → [이 함수] → writel(iatu_base + ...)
 */
static void atu_reg_write(struct tegra194_pcie_ecam *pcie_ecam, int index,
			  u32 val, u32 reg)
{
	/* [한국어] 아웃바운드 방향 index 번 창의 레지스터 블록 오프셋을 계산한다.
	 * UNROLL 방식은 창마다 레지스터 묶음이 독립적으로 존재하는 배치로,
	 * 예전의 뷰포트(viewport) 방식과 달리 창을 고르는 선택 레지스터가 필요 없다. */
	u32 offset = PCIE_ATU_UNROLL_BASE(PCIE_ATU_REGION_DIR_OB, index) +
		     PCIE_ATU_VIEWPORT_BASE;

	/* [한국어] 그 블록 안의 reg 오프셋에 값을 쓴다. */
	writel(val, pcie_ecam->iatu_base + offset + reg);
}

/* [한국어]
 * program_outbound_atu - 아웃바운드 iATU 창 하나를 config 접근용으로 설정한다
 *
 * @pcie_ecam: private 구조체.
 * @index: 창 번호. 이 파일은 언제나 0 을 쓴다.
 * @type: config 트랜잭션 종류(PCIE_ATU_TYPE_CFG0 또는 CFG1).
 * @cpu_addr: CPU 쪽 주소 — ECAM 창의 물리 시작이다.
 * @pci_addr: 변환 목표. 여기서는 실제 주소가 아니라 버스·장치·함수를 합성한 값이다.
 * @size: 창 크기.
 *
 * 일곱 개의 레지스터를 순서대로 쓴다 — CPU 주소 하위·상위, PCI 목표 하위,
 * 창 끝 주소, PCI 목표 상위, 트랜잭션 종류, 그리고 마지막에 활성화 비트.
 * 활성화를 맨 마지막에 세우는 순서가 중요하다. 주소와 종류가 모두 확정된 뒤에
 * 켜야 중간 상태로 트랜잭션이 나가지 않는다.
 *
 * config 접근에서 "PCI 주소" 자리에 버스·장치·함수를 넣는 것이 이 용법의
 * 핵심이다. config TLP 의 목적지는 메모리 주소가 아니라 BDF 이므로,
 * iATU 에 그것을 목표로 지정하면 하드웨어가 config TLP 를 만들어 보낸다.
 *
 * 실행 컨텍스트: config 접근 경로, PCI 코어의 pci_lock 아래.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   tegra194_map_bus() → [이 함수] → atu_reg_write() ×7
 */
static void program_outbound_atu(struct tegra194_pcie_ecam *pcie_ecam,
				 int index, int type, u64 cpu_addr,
				 u64 pci_addr, u64 size)
{
	/* [한국어] CPU(AXI) 주소 하위 32비트를 창의 시작으로 설정한다. */
	atu_reg_write(pcie_ecam, index, lower_32_bits(cpu_addr),
		      PCIE_ATU_LOWER_BASE);
	/* [한국어] 그 상위 32비트. */
	atu_reg_write(pcie_ecam, index, upper_32_bits(cpu_addr),
		      PCIE_ATU_UPPER_BASE);
	/* [한국어] PCI 주소 하위 32비트를 변환 목표로 설정한다. */
	atu_reg_write(pcie_ecam, index, lower_32_bits(pci_addr),
		      PCIE_ATU_LOWER_TARGET);
	/* [한국어] 창의 끝 주소. 시작 + 크기 - 1 이라 포함 구간이다. */
	atu_reg_write(pcie_ecam, index, lower_32_bits(cpu_addr + size - 1),
		      PCIE_ATU_LIMIT);
	/* [한국어] PCI 주소 상위 32비트. */
	atu_reg_write(pcie_ecam, index, upper_32_bits(pci_addr),
		      PCIE_ATU_UPPER_TARGET);
	/* [한국어] 트랜잭션 종류(CFG0 또는 CFG1)를 설정한다. 이 값이 config 접근이
	 * 어느 형식의 TLP 로 나갈지를 결정한다. */
	atu_reg_write(pcie_ecam, index, type, PCIE_ATU_REGION_CTRL1);
	/* [한국어] 마지막으로 활성화 비트를 세운다. 순서가 중요하다 — 주소와 종류를 모두
	 * 설정한 뒤에 켜야 중간 상태로 트랜잭션이 나가지 않는다. */
	atu_reg_write(pcie_ecam, index, PCIE_ATU_ENABLE, PCIE_ATU_REGION_CTRL2);
}

/* [한국어]
 * tegra194_map_bus - config 접근 주소를 계산하고 필요하면 iATU 를 다시 프로그래밍한다
 *
 * @bus: 접근 대상 버스. sysdata 에 ECAM 창이 심어져 있다.
 * @devfn: 장치·함수 번호.
 * @where: config space 안의 바이트 오프셋.
 * @return: 실제로 읽거나 쓸 MMIO 주소, 또는 NULL(존재할 수 없는 장치).
 *
 * 이 파일의 중심이다. 세 갈래로 나뉜다.
 *
 *   1) 버스 번호가 이 창의 범위 밖이면 NULL.
 *   2) 루트 버스면 대상이 루트 포트 자신이므로 DBI 블록에서 직접 읽는다.
 *      iATU 를 거치지 않는 이 경로가 있어야 링크가 올라오기 전에도 루트 포트의
 *      config 를 읽을 수 있다. 장치 0 이 아니면 NULL — 루트 포트는 하나뿐이다.
 *   3) 하위 장치면 iATU 로 변환해야 한다. 버스·장치·함수를 합성하고,
 *      부모가 루트 버스인지에 따라 Type 0(링크 바로 건너편, 버스 번호를
 *      해석하지 않음)과 Type 1(더 깊은 버스, 중간 브리지가 전달)을 고른 뒤,
 *      창 0번을 그 설정으로 다시 프로그래밍하고 config 창 주소를 돌려준다.
 *
 * config 접근마다 매번 창을 다시 쓰는 구조라, 여러 CPU 가 동시에 접근하면
 * 서로의 설정을 덮어쓸 수 있다. PCI 코어가 전역 pci_lock 으로 config 접근을
 * 직렬화해 주기 때문에 성립하는 설계다.
 *
 * 실행 컨텍스트: PCI 코어의 config 접근 경로, pci_lock 을 쥔 상태.
 *
 * 에러 경로: 세 곳에서 NULL 을 돌려준다. 코어는 그것을 보고 해당 자리를
 * 비어 있는 것으로 처리한다.
 *
 * 호출 체인:
 *   pci_read_config_dword() 등 → pci_generic_config_read()
 *     → pci_ecam_ops.pci_ops.map_bus == [이 함수] → program_outbound_atu()
 */
static void __iomem *tegra194_map_bus(struct pci_bus *bus,
				      unsigned int devfn, int where)
{
	/* [한국어] ECAM 코어가 심어 둔 창 객체. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] 그 창에 매달아 둔 세 주소 구조체. */
	struct tegra194_pcie_ecam *pcie_ecam = cfg->priv;
	/* [한국어] iATU 목표 주소로 쓸 버스·장치·함수 조합. */
	u32 busdev;
	/* [한국어] config 트랜잭션 종류. */
	int type;

	/* [한국어] 요청한 버스 번호가 이 창이 담당하는 범위 밖이면, */
	if (bus->number < cfg->busr.start || bus->number > cfg->busr.end)
		/* [한국어] NULL 을 돌려준다 — PCI 코어는 이것을 "접근 불가"로 해석한다. */
		return NULL;

	/* [한국어] 루트 버스(창의 첫 버스)에 대한 접근이면 대상이 루트 포트 자신이다. */
	if (bus->number == cfg->busr.start) {
		/* [한국어] 루트 포트는 장치 0 하나뿐이므로, */
		if (PCI_SLOT(devfn) == 0)
			/* [한국어] DBI 블록에서 직접 읽는다. iATU 를 거치지 않는 이 경로가 있어야
			 * 링크가 올라오기 전에도 루트 포트의 config 를 읽을 수 있다. */
			return pcie_ecam->dbi_base + where;
		else
			/* [한국어] 그 밖의 장치 번호는 존재할 수 없으므로 NULL. */
			return NULL;
	}

	/* [한국어] 하위 장치 접근이면 iATU 로 변환해야 한다. 버스·장치·함수를 iATU 가
	 * 이해하는 형식으로 합성한다. */
	busdev = PCIE_ATU_BUS(bus->number) | PCIE_ATU_DEV(PCI_SLOT(devfn)) |
		 PCIE_ATU_FUNC(PCI_FUNC(devfn));

	/* [한국어] 부모가 루트 버스면 이 장치는 링크 바로 건너편에 있다. */
	if (bus->parent->number == cfg->busr.start) {
		/* [한국어] 그 경우에도 장치 0 만 존재한다 — PCIe 는 점대점 링크이기 때문이다. */
		if (PCI_SLOT(devfn) == 0)
			/* [한국어] Type 0 config — 버스 번호를 해석하지 않고 상대에게 직접 전달한다. */
			type = PCIE_ATU_TYPE_CFG0;
		else
			/* [한국어] 장치 0 이 아니면 존재할 수 없다. */
			return NULL;
	/* [한국어] 더 깊은 버스(스위치 아래)면, */
	} else {
		/* [한국어] Type 1 config — 중간 브리지가 버스 번호를 보고 전달한다. */
		type = PCIE_ATU_TYPE_CFG1;
	}

	/* [한국어] 아웃바운드 창 0번을 이번 접근에 맞게 다시 프로그래밍한다.
	 * config 접근마다 매번 창을 다시 쓰는 구조라, 여러 CPU 가 동시에 접근하면
	 * 서로의 설정을 덮어쓸 수 있다 — PCI 코어가 전역 pci_lock 으로 config
	 * 접근을 직렬화해 주기 때문에 성립하는 설계다. */
	program_outbound_atu(pcie_ecam, 0, type, cfg->res.start, busdev,
			     SZ_256K);

	/* [한국어] 변환이 준비된 config 창의 주소를 돌려준다. 실제 읽기/쓰기는 코어의
	 * pci_generic_config_read/write 가 이 주소로 수행한다. */
	return pcie_ecam->config_base + where;
}

const struct pci_ecam_ops tegra194_pcie_ops = {
	/* [한국어] ECAM 창 생성 직후 불릴 초기화 콜백. */
	.init		= tegra194_acpi_init,
	.pci_ops	= {
		/* [한국어] 주소 계산만 이 파일이 맡는다. */
		.map_bus	= tegra194_map_bus,
		/* [한국어] 읽기는 코어의 범용 구현. */
		.read		= pci_generic_config_read,
		/* [한국어] 쓰기도 마찬가지. map_bus 하나만 구현하면 되는 것이 ECAM 틀의 장점이다. */
		.write		= pci_generic_config_write,
	}
};
