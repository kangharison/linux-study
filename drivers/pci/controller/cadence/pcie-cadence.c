// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

/*
 * [한국어 설명] Cadence PCIe IP 의 공통 코어 (pcie-cadence.c)
 *
 * === 파일의 역할 ===
 * Cadence 는 DesignWare 와 함께 PCIe 컨트롤러 IP 시장을 나눠 갖는 회사다.
 * TI 의 J721e, Sophgo 의 SG2042, Cixtech 의 Sky1 같은 SoC 가 이 IP 를
 * 라이선스해 쓰고, 그 드라이버들이 cadence/ 디렉터리에 모여 있다.
 * 이 파일은 그 전부가 공유하는 부분이다.
 *
 * dwc/ 와 구조가 같다. SoC 마다 다른 것은 클럭·리셋·전원·PHY 같은
 * 주변부이고, 링크와 주소 변환의 핵심은 IP 가 같으므로 동일하다.
 *
 * 이 파일의 중심 개념은 아웃바운드 영역(outbound region)이다.
 * DesignWare 의 iATU 에 해당하는 것으로, CPU 가 보는 주소를 PCIe 버스
 * 주소로 변환하는 창이다. 창마다 네 가지를 설정한다.
 *   PCI_ADDR0/1 — 나갈 PCIe 주소와 창 크기(비트 수로 표현)
 *   CPU_ADDR0/1 — 들어올 CPU 주소
 *   DESC0/1     — 어떤 종류의 TLP 로 내보낼지(메모리/IO/메시지)와
 *                 requester ID 를 어떻게 채울지
 *
 * DESC 레지스터의 처리가 이 IP 의 특징이다. 호스트 모드와 엔드포인트
 * 모드에서 requester ID 를 채우는 방식이 다른데, 그 사정을 상류 주석이
 * 길게 설명하고 있고 아래 해당 함수에 옮겨 두었다.
 *
 * 또 하나 눈여겨볼 것은 PHY 관리다. SoC 하나에 PHY 가 여러 개일 수 있어
 * (레인마다 하나인 구성) 배열로 다루며, 초기화 중 실패하면 그때까지 켠
 * 것을 역순으로 끄는 처리가 들어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SoC 별 드라이버(pci-j721e.c, pcie-sg2042.c, pci-sky1.c,
 *   pcie-cadence-plat.c)의 probe
 *   -> cdns_pcie_init_phy() [이 파일] 로 PHY 준비
 *   -> 호스트 모드면 cdns_pcie_host_setup() [pcie-cadence-host.c]
 *      엔드포인트 모드면 cdns_pcie_ep_setup() [pcie-cadence-ep.c]
 *      -> 그 안에서 [이 파일] cdns_pcie_set_outbound_region() 등을 부른다
 *
 * 실행 컨텍스트: 대부분 probe 시점의 프로세스 컨텍스트.
 *   레지스터 접근 함수들은 상위 계층의 잠금 아래에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: cadence/ 아래의 SoC 별 드라이버와 host/ep 파일들.
 * 아래쪽: PHY 서브시스템, 그리고 PCI 코어(../../pci.h 의 내부 매크로).
 * 공유 상태: struct cdns_pcie — 레지스터 베이스, PHY 배열, 모드 플래그.
 *   SoC 드라이버가 이것을 자기 구조체에 품고 container_of 로 오간다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — cdns_pcie_* 호출 0건).
 *
 * TI J721e 같은 자동차용 SoC 나 SG2042 같은 RISC-V 서버 칩에 NVMe 를
 * 붙이면 그 아래에 이 IP 가 있다. 다만 관계는 토폴로지상의 것이고
 * 코드 호출이 아니다 — 이 드라이버가 호스트 브리지를 만들면 PCI 코어가
 * 그 아래를 열거하고, 그 결과로 nvme_probe() 가 불린다.
 *
 * 성능 진단에서 의미가 있는 부분은 아웃바운드 영역의 개수다. 창이
 * 부족하면 큰 BAR 를 가진 장치를 여럿 붙일 때 자원 배정이 실패할 수
 * 있는데, 그 창 설정이 여기서 이뤄진다.
 *
 * === 주요 함수/구조체 요약 ===
 * cdns_pcie_find_capability() / _find_ext_capability() : 이 IP 자신의
 *                          config space 에서 capability 를 찾는다.
 * cdns_pcie_linkup()       : 링크가 올라왔는지 확인한다. EXPORT 되어
 *                          있으나 이 트리 안에서 호출자는 0건이다 —
 *                          실제로 쓰이는 것은 pcie-cadence.h 의
 *                          cdns_pcie_link_up() 디스패처 쪽이다.
 * cdns_pcie_detect_quiet_min_delay_set() : LTSSM 의 Detect.Quiet 최소
 *                          대기 시간을 설정한다.
 * cdns_pcie_set_outbound_region() : 아웃바운드 주소 변환 창을 설정한다.
 *                          이 파일에서 가장 중요한 함수다.
 * cdns_pcie_set_outbound_region_for_normal_msg() : 메시지 TLP 전용 창.
 * cdns_pcie_reset_outbound_region() : 창을 지운다.
 * cdns_pcie_init_phy() / _enable_phy() / _disable_phy() : PHY 관리.
 * cdns_pcie_suspend_noirq() / _resume_noirq() : 절전 시 PHY 를 끄고 켠다.
 * cdns_pcie_pm_ops         : 그 둘을 묶은 전원 관리 표.
 */

/* [한국어] fls64, ilog2 등 비트 연산 헬퍼. 창 크기를 비트 수로 바꾸는
 * 계산에 쓴다. */
#include <linux/kernel.h>
/* [한국어] EXPORT_SYMBOL_GPL 과 MODULE_* 매크로. 이 파일이 별도 모듈로
 * 빌드되어 SoC 별 드라이버들이 링크하기 때문에 필요하다. */
#include <linux/module.h>
/* [한국어] of_property_count_strings 등 디바이스 트리 접근.
 * PHY 이름 목록을 읽는 데 쓴다. */
#include <linux/of.h>

/* [한국어] struct cdns_pcie 정의와 CDNS_PCIE_* 레지스터 상수,
 * 그리고 cdns_pcie_readl / writel 접근자. */
#include "pcie-cadence.h"
/* [한국어] PCI 코어 내부 헤더. PCI_FIND_NEXT_CAP 같은 내부 매크로를
 * 쓰기 위해 필요하다 — 이 IP 의 config space 를 직접 훑어야 하므로
 * 일반 pci_find_capability() 를 쓸 수 없다(아직 PCI 장치로 등록되기
 * 전이거나 자기 자신을 보는 것이기 때문이다). */
#include "../../pci.h"

/* [한국어]
 * cdns_pcie_find_capability - 이 IP 의 config space 에서 capability 를 찾는다
 *
 * @pcie: 대상 컨트롤러.
 * @cap: 찾을 capability ID (PCI_CAP_ID_EXP 등).
 * @return: 그 capability 의 오프셋. 없으면 0.
 *
 * 커널의 pci_find_capability() 를 쓸 수 없는 이유가 있다. 그 함수는
 * struct pci_dev 를 받는데, 여기서 보려는 것은 아직 PCI 장치로 등록되지
 * 않은(또는 등록될 일이 없는) 컨트롤러 자신의 config space 다.
 *
 * 그래서 PCI 코어 내부 매크로 PCI_FIND_NEXT_CAP 에 이 IP 전용 읽기
 * 함수(cdns_pcie_read_cfg)를 넘겨 같은 탐색 논리를 재사용한다.
 * capability 는 연결 리스트라 그 순회 규칙이 같기 때문이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cadence/ 의 host/ep 초기화 → [이 함수] → PCI_FIND_NEXT_CAP()
 */
u8 cdns_pcie_find_capability(struct cdns_pcie *pcie, u8 cap)
{
	/* [한국어] PCI_CAPABILITY_LIST(0x34)가 capability 연결 리스트의
	 * 시작 포인터가 들어 있는 자리다. 거기서부터 따라간다. */
	return PCI_FIND_NEXT_CAP(cdns_pcie_read_cfg, PCI_CAPABILITY_LIST,
				 cap, NULL, pcie);
}
EXPORT_SYMBOL_GPL(cdns_pcie_find_capability);

/* [한국어]
 * cdns_pcie_find_ext_capability - 확장 capability 를 찾는다
 *
 * @pcie: 대상 컨트롤러.
 * @cap: 찾을 확장 capability ID (PCI_EXT_CAP_ID_ERR 등).
 * @return: 그 오프셋. 없으면 0.
 *
 * 위 함수의 확장판이다. 확장 capability 는 config space 의 0x100 이후에
 * 별도의 연결 리스트로 놓여 있어 탐색 시작점과 항목 형식이 다르다.
 * 시작 오프셋으로 0 을 넘기면 매크로가 0x100 부터 시작한다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cadence/ 의 host/ep 초기화 → [이 함수] → PCI_FIND_NEXT_EXT_CAP()
 */
u16 cdns_pcie_find_ext_capability(struct cdns_pcie *pcie, u8 cap)
{
	return PCI_FIND_NEXT_EXT_CAP(cdns_pcie_read_cfg, 0, cap, NULL, pcie);
}
EXPORT_SYMBOL_GPL(cdns_pcie_find_ext_capability);

/* [한국어]
 * cdns_pcie_linkup - 링크가 올라왔는지 확인한다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 링크가 살아 있으면 true.
 *
 * Local Management 레지스터의 최하위 비트가 링크 상태를 나타낸다.
 * 링크 트레이닝을 시작한 뒤 이 함수를 반복해 호출하며 기다리는 것이
 * 상위 계층의 일반적인 사용법이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트, 또는 링크 대기 루프 안.
 *
 * 호출 체인:
 *   SoC 드라이버의 start_link 이후 대기 루프 → [이 함수]
 */
bool cdns_pcie_linkup(struct cdns_pcie *pcie)
{
	u32 pl_reg_val;

	/* [한국어] Local Management 블록의 첫 레지스터를 읽는다. */
	pl_reg_val = cdns_pcie_readl(pcie, CDNS_PCIE_LM_BASE);
	/* [한국어] GENMASK(0, 0) 은 비트 0 하나만 고르는 마스크다.
	 * 그냥 1 이나 BIT(0) 을 써도 되지만, 이 파일의 다른 곳에서 쓰는
	 * 비트 범위 표기와 형태를 맞춘 것으로 보인다. */
	if (pl_reg_val & GENMASK(0, 0))
		return true;
	return false;
}
EXPORT_SYMBOL_GPL(cdns_pcie_linkup);

/* [한국어]
 * cdns_pcie_detect_quiet_min_delay_set - LTSSM Detect.Quiet 최소 대기를 늘린다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 *
 * LTSSM(Link Training and Status State Machine)의 첫 상태가 Detect 이고,
 * 그 안의 Detect.Quiet 은 "선로가 조용해지기를 기다리는" 구간이다.
 * 이 구간이 너무 짧으면 아직 안정되지 않은 신호를 장치가 있는 것으로
 * 오인해 트레이닝이 실패할 수 있다.
 *
 * 상류 주석대로 그 최소 대기를 2ms 로 설정한다. delay 값 0x3 이 2ms 에
 * 대응하는 인코딩인데, 그 대응표는 Cadence 의 IP 문서에 있고 이 트리
 * 안에서는 확인할 수 없다 — 코드에 있는 그대로만 기록한다.
 *
 * 실행 컨텍스트: 링크 트레이닝 시작 전 프로세스 컨텍스트. 이 설정은
 *   트레이닝이 시작되기 전에 들어가 있어야 효과가 있다.
 *
 * 호출 체인:
 *   SoC 드라이버의 초기화 → [이 함수]
 */
void cdns_pcie_detect_quiet_min_delay_set(struct cdns_pcie *pcie)
{
	/* [한국어] 2ms 에 해당하는 인코딩 값. 상류 주석이 그 의미를 밝히고
	 * 있으며, 하드웨어 문서 없이는 이 숫자 자체의 근거를 확인할 수 없다. */
	u32 delay = 0x3;
	u32 ltssm_control_cap;

	/*
	 * Set the LTSSM Detect Quiet state min. delay to 2ms.
	 */
	/* [한국어] 읽어서 고치고 다시 쓴다. 이 레지스터에 다른 설정도
	 * 함께 들어 있어 통째로 덮어쓰면 안 되기 때문이다. */
	ltssm_control_cap = cdns_pcie_readl(pcie, CDNS_PCIE_LTSSM_CONTROL_CAP);
	/* [한국어] 해당 필드만 지우고(~MASK) 새 값을 끼워 넣는다. */
	ltssm_control_cap = ((ltssm_control_cap &
			    ~CDNS_PCIE_DETECT_QUIET_MIN_DELAY_MASK) |
			    CDNS_PCIE_DETECT_QUIET_MIN_DELAY(delay));

	/* [한국어] 고친 값을 다시 쓴다. 링크 트레이닝 전에 반영되어야 한다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LTSSM_CONTROL_CAP, ltssm_control_cap);
}
EXPORT_SYMBOL_GPL(cdns_pcie_detect_quiet_min_delay_set);

/* [한국어] cdns_pcie_set_outbound_region - 아웃바운드 주소 변환 창 하나를 설정한다
 * 
 * @pcie: 대상 컨트롤러.
 * @busnr: RC 모드에서 나가는 TLP 에 실을 버스 번호.
 * @fn: 펑션 번호. EP 모드에서 특히 중요하다.
 * @r: 몇 번째 창인지.
 * @is_io: I/O 공간이면 true, 메모리면 false.
 * @cpu_addr: 이 창에 대응할 CPU 주소.
 * @pci_addr: 그 접근이 나갈 PCIe 버스 주소.
 * @size: 창 크기.
 * @return: 없음.
 * 
 * 이 파일에서 가장 중요한 함수다. DesignWare 의 iATU 아웃바운드 설정에
 * 해당하며, CPU 가 특정 주소 범위에 접근하면 그것을 PCIe TLP 로 바꿔
 * 내보내도록 하드웨어를 설정한다.
 * 
 * 창 하나에 여섯 레지스터를 쓴다 — PCI 주소 둘, CPU 주소 둘, 서술자 둘.
 * 크기를 바이트가 아니라 비트 수로 표현하는 점과, requester ID 를 채우는
 * 방식이 모드에 따라 갈리는 점이 이 IP 의 특징이다.
 * 
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. 상위 계층이 창 번호가
 * 겹치지 않게 관리할 책임을 진다.
 * 
 * 호출 체인:
 *   cadence/ 의 host/ep 초기화 → [이 함수] → cdns_pcie_writel() */
void cdns_pcie_set_outbound_region(struct cdns_pcie *pcie, u8 busnr, u8 fn,
				   u32 r, bool is_io,
				   u64 cpu_addr, u64 pci_addr, size_t size)
{
	/*
	 * roundup_pow_of_two() returns an unsigned long, which is not suited
	 * for 64bit values.
	 */
	/* [한국어] 상류 주석대로 roundup_pow_of_two() 를 쓸 수 없어 직접
	 * 계산한다. 그 함수가 unsigned long 을 돌려주는데 32비트 아키텍처
	 * 에서는 그것이 32비트라 64비트 크기를 다룰 수 없기 때문이다.
	 * fls64(size-1) 은 size 를 덮는 가장 작은 2의 거듭제곱의 지수를 준다. */
	u64 sz = 1ULL << fls64(size - 1);
	/* [한국어] 그 크기를 다시 비트 수로 바꾼다. 이 IP 는 창 크기를
	 * 바이트가 아니라 "주소의 하위 몇 비트를 창 안에서 쓸 것인가" 로
	 * 표현하기 때문이다. */
	int nbits = ilog2(sz);
	u32 addr0, addr1, desc0, desc1;

	/* [한국어] 최소 8비트(256바이트). 그보다 작은 창은 이 IP 가
	 * 표현할 수 없다 — 아래에서 주소의 하위 8비트를 마스크로 잘라
	 * 버리는 것과 짝이 맞는다. */
	if (nbits < 8)
		nbits = 8;

	/* Set the PCI address */
	/* [한국어] 목적지 PCIe 주소와 창 크기를 한 워드에 담는다.
	 * GENMASK(31, 8) 로 하위 8비트를 잘라 내는 이유는 그 자리가
	 * 주소가 아니라 크기(nbits) 필드이기 때문이다. 그래서 창의 시작
	 * 주소는 항상 256바이트 경계에 맞아야 한다. */
	addr0 = CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(nbits) |
		(lower_32_bits(pci_addr) & GENMASK(31, 8));
	/* [한국어] 상위 32비트는 통째로 들어간다. 64비트 주소 지원. */
	addr1 = upper_32_bits(pci_addr);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r), addr1);

	/* Set the PCIe header descriptor */
	/* [한국어] 이 창으로 나가는 접근을 어떤 종류의 TLP 로 만들지 정한다.
	 * I/O 공간과 메모리 공간은 TLP 헤더의 타입 필드가 다르다. */
	if (is_io)
		desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_IO;
	else
		/* [한국어] 메모리 공간이 기본이다. 대부분의 접근이 이쪽이다. */
		desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_MEM;
	/* [한국어] DESC1 은 아래 RC 모드 분기에서만 채워진다. 우선 0 으로 둔다. */
	desc1 = 0;

	/*
	 * Whatever Bit [23] is set or not inside DESC0 register of the outbound
	 * PCIe descriptor, the PCI function number must be set into
	 * Bits [26:24] of DESC0 anyway.
	 *
	 * In Root Complex mode, the function number is always 0 but in Endpoint
	 * mode, the PCIe controller may support more than one function. This
	 * function number needs to be set properly into the outbound PCIe
	 * descriptor.
	 *
	 * Besides, setting Bit [23] is mandatory when in Root Complex mode:
	 * then the driver must provide the bus, resp. device, number in
	 * Bits [7:0] of DESC1, resp. Bits[31:27] of DESC0. Like the function
	 * number, the device number is always 0 in Root Complex mode.
	 *
	 * However when in Endpoint mode, we can clear Bit [23] of DESC0, hence
	 * the PCIe controller will use the captured values for the bus and
	 * device numbers.
	 */
	/* [한국어] 위 상류 주석이 설명한 대로 모드에 따라 갈린다.
	 * 요약하면, 나가는 TLP 의 requester ID 를 누가 정하느냐의 문제다. */
	if (pcie->is_rc) {
		/* The device and function numbers are always 0. */
		/* [한국어] 루트 컴플렉스 모드에서는 HARDCODED_RID 비트를
		 * 반드시 세워야 하고(상류 주석의 "mandatory"), 그러면
		 * 소프트웨어가 버스·디바이스·펑션 번호를 직접 채워야 한다.
		 * 루트 포트 자신은 항상 디바이스 0, 펑션 0 이므로 그렇게 넣고,
		 * 버스 번호만 인자로 받은 값을 쓴다. */
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID |
			 CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(0);
		desc1 |= CDNS_PCIE_AT_OB_REGION_DESC1_BUS(busnr);
	} else {
		/*
		 * Use captured values for bus and device numbers but still
		 * need to set the function number.
		 */
		/* [한국어] 엔드포인트 모드에서는 HARDCODED_RID 를 세우지
		 * 않는다. 그러면 하드웨어가 호스트에게서 받아 기억해 둔
		 * (captured) 버스·디바이스 번호를 쓴다 — 엔드포인트의 번호는
		 * 호스트가 열거하며 정해 주는 것이라 소프트웨어가 미리 알 수 없다.
		 *
		 * 다만 펑션 번호만은 여전히 직접 넣어야 한다. 상류 주석이
		 * 강조하듯 HARDCODED_RID 여부와 무관하게 그 자리는 채워야 하며,
		 * 다중 함수 장치라면 어느 함수가 낸 요청인지를 이것으로 구분한다. */
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(fn);
	}

	/* [한국어] TLP 타입과 requester ID 설정을 반영한다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(r), desc0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(r), desc1);

	/* Set the CPU address */
	/* [한국어] SoC 에 따라 CPU 물리 주소와 이 IP 가 보는 주소가 다를 수
	 * 있다. 예컨대 상위 비트에 고정된 오프셋이 붙는 구성이 그렇다.
	 * 그런 SoC 는 cpu_addr_fixup 을 제공해 여기서 보정한다. */
	if (pcie->ops && pcie->ops->cpu_addr_fixup)
		cpu_addr = pcie->ops->cpu_addr_fixup(pcie, cpu_addr);

	/* [한국어] CPU 쪽도 같은 형식이다. 크기(nbits)를 다시 넣는 것이
	 * 눈에 띄는데, 양쪽 창의 크기가 같아야 변환이 성립하기 때문이다. */
	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(nbits) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);

	/* [한국어] 마지막으로 CPU 쪽 주소를 쓴다. 이 쓰기로 창이 실제로 열린다 —
	 * 앞의 설정이 모두 자리 잡은 뒤여야 하므로 순서가 중요하다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r), addr0);
	/* [한국어] 상위 32비트까지 쓰면 설정이 완료된다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r), addr1);
}
EXPORT_SYMBOL_GPL(cdns_pcie_set_outbound_region);

/* [한국어]
 * cdns_pcie_set_outbound_region_for_normal_msg - 메시지 TLP 전용 창 설정
 *
 * @pcie: 대상 컨트롤러.
 * @busnr: RC 모드에서 쓸 버스 번호.
 * @fn: 펑션 번호.
 * @r: 몇 번째 아웃바운드 창인지.
 * @cpu_addr: 이 창에 대응할 CPU 주소.
 * @return: 없음.
 *
 * PCIe 에는 메모리·I/O·config 말고도 메시지(Message) TLP 가 있다.
 * INTx 인터럽트, 전원 관리 이벤트, 오류 보고 등이 이것으로 전달된다.
 * 그런 메시지를 보내려면 메시지 타입으로 설정된 창이 필요하고,
 * 이 함수가 그것을 만든다.
 *
 * 위 일반 창 설정과 다른 점이 셋이다.
 *   PCI 주소를 0 으로 둔다 — 메시지는 목적지 주소가 없다.
 *   크기가 17비트로 고정이다 — 메시지 창의 크기가 정해져 있다.
 *   타입이 NORMAL_MSG 다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cadence/ 의 host/ep 초기화 → [이 함수]
 */
void cdns_pcie_set_outbound_region_for_normal_msg(struct cdns_pcie *pcie,
						  u8 busnr, u8 fn,
						  u32 r, u64 cpu_addr)
{
	u32 addr0, addr1, desc0, desc1;

	/* [한국어] 이 창으로 나가는 접근을 메시지 TLP 로 만든다. */
	desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_NORMAL_MSG;
	desc1 = 0;

	/* See cdns_pcie_set_outbound_region() comments above. */
	/* [한국어] requester ID 처리는 위 함수와 완전히 같다.
	 * 상류 주석이 그쪽을 보라고 하고 있어 여기서는 반복하지 않는다. */
	if (pcie->is_rc) {
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID |
			 CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(0);
		/* [한국어] RC 모드에서는 버스 번호를 DESC1 에 직접 넣어야 한다. */
		desc1 |= CDNS_PCIE_AT_OB_REGION_DESC1_BUS(busnr);
	} else {
		/* [한국어] EP 모드에서는 펑션 번호만 넣고 나머지는 하드웨어가 채운다. */
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(fn);
	}

	/* Set the CPU address */
	if (pcie->ops && pcie->ops->cpu_addr_fixup)
		cpu_addr = pcie->ops->cpu_addr_fixup(pcie, cpu_addr);

	/* [한국어] 크기가 17비트(128KB)로 고정이다. 위 함수처럼 인자로
	 * 받지 않는데, 메시지 창의 크기가 이 IP 에서 정해져 있기 때문이다.
	 * 그 값의 근거는 Cadence IP 문서에 있고 이 트리에서는 확인할 수 없다. */
	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(17) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);

	/* [한국어] PCI 주소를 0 으로 지운다. 메시지 TLP 에는 목적지 주소
	 * 필드가 없으므로 이 값은 쓰이지 않으며, 남은 값이 오해를 부르지
	 * 않도록 명시적으로 지운다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(r), desc0);
	/* [한국어] 서술자 둘을 쓴다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(r), desc1);
	/* [한국어] CPU 주소를 쓴다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r), addr0);
	/* [한국어] 상위 32비트까지 쓰면 메시지 창이 열린다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r), addr1);
}
EXPORT_SYMBOL_GPL(cdns_pcie_set_outbound_region_for_normal_msg);

/* [한국어]
 * cdns_pcie_reset_outbound_region - 아웃바운드 창을 완전히 지운다
 *
 * @pcie: 대상 컨트롤러.
 * @r: 지울 창 번호.
 * @return: 없음.
 *
 * 여섯 레지스터를 모두 0 으로 만든다. 그러면 그 창은 아무 변환도 하지
 * 않게 되어, 해당 CPU 주소 범위에 접근해도 PCIe 로 나가지 않는다.
 *
 * 창을 재사용하기 전에 반드시 거쳐야 하는 절차다. 이전 설정이 일부라도
 * 남으면 새 설정과 섞여 엉뚱한 주소로 나갈 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cadence/ 의 host/ep 정리 경로 → [이 함수]
 */
void cdns_pcie_reset_outbound_region(struct cdns_pcie *pcie, u32 r)
{
	/* [한국어] 목적지 PCIe 주소와 크기를 지운다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r), 0);

	/* [한국어] TLP 타입과 requester ID 설정을 지운다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(r), 0);

	/* [한국어] 마지막으로 CPU 쪽 주소를 지운다. 이 값이 0 이 되면
	 * 창이 실질적으로 닫힌다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r), 0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r), 0);
}
EXPORT_SYMBOL_GPL(cdns_pcie_reset_outbound_region);

/* [한국어]
 * cdns_pcie_disable_phy - 이 컨트롤러의 PHY 를 모두 끈다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 *
 * SoC 하나에 PHY 가 여러 개일 수 있다 — 레인마다 하나인 구성이 흔하다.
 * 그래서 배열로 다루며, 역순으로 끈다.
 *
 * 역순인 이유는 켠 순서의 반대여야 의존 관계가 어긋나지 않기 때문이다.
 * PHY 사이에 순서 의존이 있는 하드웨어가 있을 수 있고, 없더라도
 * 대칭을 지키는 편이 안전하다.
 *
 * PHY 하나에 대해 power_off 와 exit 를 모두 부르는 것은 두 단계가
 * 다르기 때문이다 — power_off 는 전원만 끄고, exit 는 PHY 드라이버가
 * 잡아 둔 자원까지 놓는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PHY 조작이 잠들 수 있다.
 *
 * 호출 체인:
 *   cdns_pcie_suspend_noirq() → [이 함수]
 *   SoC 드라이버의 remove → [이 함수]
 */
void cdns_pcie_disable_phy(struct cdns_pcie *pcie)
{
	/* [한국어] 개수에서 시작해 아래로 내려간다. */
	int i = pcie->phy_count;

	/* [한국어] 후위 감소라 i 가 phy_count-1 부터 0 까지 돈다.
	 * i 가 0 일 때 조건이 거짓이 되어 루프를 빠져나가고, 그때 i 는
	 * -1 이 된다 — 배열 접근에는 쓰이지 않으므로 무해하다. */
	while (i--) {
		phy_power_off(pcie->phy[i]);
		phy_exit(pcie->phy[i]);
	}
}
EXPORT_SYMBOL_GPL(cdns_pcie_disable_phy);

/* [한국어]
 * cdns_pcie_enable_phy - 이 컨트롤러의 PHY 를 모두 켠다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 0 이면 성공. 하나라도 실패하면 그 오류.
 *
 * disable 의 반대다. PHY 마다 init 과 power_on 을 차례로 부른다.
 *
 * 중간 실패 처리가 이 함수에서 눈여겨볼 부분이다. 두 갈래가 있다.
 *   phy_init 이 실패하면 — 그 PHY 는 아무것도 잡지 않았으므로
 *     되돌릴 것이 없다. 곧바로 앞의 것들을 정리하러 간다.
 *   phy_power_on 이 실패하면 — 그 PHY 의 init 은 성공했으므로
 *     그것만 먼저 되돌리고(phy_exit) 앞의 것들을 정리하러 간다.
 * 이 비대칭이 err_phy 레이블에서 i 를 선감소(--i)로 시작하는 이유다 —
 * 실패한 회차는 이미 처리했거나 처리할 것이 없으므로 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. PHY 조작이 잠들 수 있다.
 *
 * 호출 체인:
 *   cdns_pcie_init_phy() → [이 함수]
 *   cdns_pcie_resume_noirq() → [이 함수]
 */
int cdns_pcie_enable_phy(struct cdns_pcie *pcie)
{
	int ret;
	int i;

	for (i = 0; i < pcie->phy_count; i++) {
		/* [한국어] PHY 드라이버가 자원을 잡고 하드웨어를 준비한다. */
		ret = phy_init(pcie->phy[i]);
		if (ret < 0)
			/* [한국어] 이 회차는 잡은 것이 없으므로 그냥 나간다. */
			goto err_phy;

		/* [한국어] 그다음 전원을 넣는다. */
		ret = phy_power_on(pcie->phy[i]);
		if (ret < 0) {
			/* [한국어] init 은 성공했으므로 이 회차만 되돌린다.
			 * 아래 정리 루프는 이 회차를 건너뛴다. */
			phy_exit(pcie->phy[i]);
			goto err_phy;
		}
	}

	return 0;

err_phy:
	/* [한국어] 선감소로 시작해 실패한 회차를 건너뛴다.
	 * disable_phy() 와 같은 순서(역순)로 정리한다. */
	while (--i >= 0) {
		phy_power_off(pcie->phy[i]);
		phy_exit(pcie->phy[i]);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_enable_phy);

/* [한국어]
 * cdns_pcie_init_phy - 디바이스 트리에서 PHY 들을 찾아 확보하고 켠다
 *
 * @dev: 이 컨트롤러의 device. 디바이스 트리 노드와 devres 의 주체다.
 * @pcie: 결과를 담을 컨트롤러 구조체.
 * @return: 0 이면 성공(PHY 가 없는 경우도 성공이다). 실패 시 음수.
 *
 * 하는 일이 셋이다.
 *   1) 디바이스 트리의 phy-names 속성으로 PHY 개수와 이름을 알아낸다.
 *   2) 이름마다 PHY 를 확보하고 device link 를 건다.
 *   3) 전부 켠다.
 *
 * 2번의 device link 가 이 함수에서 덜 자명한 부분이다. PHY 가 이
 * 컨트롤러보다 먼저 준비되고 나중에 정리되어야 하는데, 링크를 걸어 두면
 * 커널이 그 순서를 지켜 준다. 특히 절전 진입·복귀 순서에서 중요하다.
 * DL_FLAG_STATELESS 는 "드라이버 바인딩 상태에 따라 링크를 자동으로
 * 없애지 말라" 는 뜻으로, 이 코드가 직접 관리하겠다는 선언이다.
 *
 * PHY 가 하나도 없어도 오류가 아니라는 점도 중요하다. 일부 SoC 는 PHY 가
 * 별도 드라이버가 아니라 컨트롤러에 통합되어 있어 디바이스 트리에
 * 나타나지 않는다.
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트.
 *
 * 에러 경로: err_phy 레이블에서 확보한 PHY 와 링크를 역순으로 되돌린다.
 *   배열 자체는 devm_kcalloc 이라 드라이버가 떨어질 때 자동 해제된다.
 *
 * 호출 체인:
 *   SoC 드라이버의 probe → [이 함수]
 *     → devm_phy_get() → device_link_add() → cdns_pcie_enable_phy()
 */
int cdns_pcie_init_phy(struct device *dev, struct cdns_pcie *pcie)
{
	struct device_node *np = dev->of_node;
	int phy_count;
	/* [한국어] PHY 핸들 배열. 이중 포인터인 것은 배열을 동적으로 잡기
	 * 때문이다 — 개수를 디바이스 트리에서 읽어야 알 수 있다. */
	struct phy **phy;
	/* [한국어] device link 배열. PHY 하나에 링크 하나씩 대응한다. */
	struct device_link **link;
	int i;
	int ret;
	const char *name;

	/* [한국어] phy-names 속성의 문자열 개수가 곧 PHY 개수다. */
	phy_count = of_property_count_strings(np, "phy-names");
	if (phy_count < 1) {
		/* [한국어] PHY 가 없는 것은 오류가 아니다. PHY 가 컨트롤러에
		 * 통합된 SoC 도 있기 때문이다. 다만 흔한 경우는 아니라
		 * dev_dbg 가 아니라 dev_info 로 남겨 나중에 진단할 때
		 * 실마리가 되게 한다. */
		dev_info(dev, "no \"phy-names\" property found; PHY will not be initialized\n");
		pcie->phy_count = 0;
		return 0;
	}

	/* [한국어] devm 계열이라 드라이버가 떨어질 때 자동 해제된다.
	 * 그래서 아래 에러 경로에서 이 배열들을 직접 해제하지 않는다. */
	phy = devm_kcalloc(dev, phy_count, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	/* [한국어] device link 배열도 같은 개수만큼 잡는다. */
	link = devm_kcalloc(dev, phy_count, sizeof(*link), GFP_KERNEL);
	/* [한국어] 메모리 부족. 앞서 잡은 phy 배열은 devm 이라 자동 해제된다. */
	if (!link)
		return -ENOMEM;

	for (i = 0; i < phy_count; i++) {
		/* [한국어] i 번째 PHY 의 이름을 읽는다. 반환값을 확인하지
		 * 않는데, 위에서 개수를 셌으므로 그만큼은 반드시 있기 때문이다. */
		of_property_read_string_index(np, "phy-names", i, &name);
		/* [한국어] 그 이름으로 PHY 를 확보한다. 해당 PHY 드라이버가
		 * 아직 준비되지 않았으면 -EPROBE_DEFER 가 나오고, 커널이
		 * 나중에 이 probe 를 다시 시도한다. */
		phy[i] = devm_phy_get(dev, name);
		if (IS_ERR(phy[i])) {
			ret = PTR_ERR(phy[i]);
			goto err_phy;
		}
		/* [한국어] 이 컨트롤러가 그 PHY 에 의존함을 커널에 알린다.
		 * 그러면 절전 진입·복귀와 제거 순서가 올바르게 지켜진다. */
		link[i] = device_link_add(dev, &phy[i]->dev, DL_FLAG_STATELESS);
		if (!link[i]) {
			/* [한국어] 링크 생성이 실패했으면 방금 얻은 PHY 를
			 * 직접 놓는다. 아래 정리 루프는 이 회차를 건너뛴다. */
			devm_phy_put(dev, phy[i]);
			ret = -EINVAL;
			goto err_phy;
		}
	}

	/* [한국어] 전부 확보했으므로 이제 컨트롤러 구조체에 기록한다.
	 * 이 시점 이후로 disable_phy() 등이 이 배열을 쓸 수 있다. */
	pcie->phy_count = phy_count;
	pcie->phy = phy;
	pcie->link = link;

	/* [한국어] 확보한 PHY 들을 실제로 켠다. */
	ret =  cdns_pcie_enable_phy(pcie);
	if (ret)
		goto err_phy;

	return 0;

err_phy:
	/* [한국어] 선감소로 실패한 회차를 건너뛴다. 링크를 먼저 끊고
	 * PHY 를 놓는 순서인데, 링크가 살아 있는 채로 PHY 를 놓으면
	 * 커널이 없는 장치에 대한 의존을 들고 있게 되기 때문이다. */
	while (--i >= 0) {
		device_link_del(link[i]);
		devm_phy_put(dev, phy[i]);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_init_phy);

/* [한국어]
 * cdns_pcie_suspend_noirq - 절전 진입 시 PHY 를 끈다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0.
 *
 * PHY 는 전력을 꽤 쓰므로 절전 상태에서는 꺼야 한다. noirq 단계인 것은
 * 이 시점에 인터럽트가 이미 막혀 있어 PHY 를 꺼도 그 사이에 접근이
 * 들어오지 않기 때문이다.
 *
 * 실행 컨텍스트: 시스템 절전 진입의 noirq 단계. 인터럽트가 막혀 있지만
 *   프로세스 컨텍스트라 잠들 수 있다.
 *
 * 호출 체인:
 *   (시스템 절전) → PM 코어 → cdns_pcie_pm_ops → [이 함수]
 */
static int cdns_pcie_suspend_noirq(struct device *dev)
{
	/* [한국어] SoC 드라이버가 probe 에서 저장해 둔 컨트롤러 구조체. */
	struct cdns_pcie *pcie = dev_get_drvdata(dev);

	cdns_pcie_disable_phy(pcie);

	return 0;
}

/* [한국어]
 * cdns_pcie_resume_noirq - 절전 복귀 시 PHY 를 다시 켠다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 이면 성공. PHY 를 못 켜면 그 오류.
 *
 * suspend 의 반대다. 인터럽트가 다시 열리기 전에 PHY 가 준비되어 있어야
 * 하므로 noirq 단계에서 처리한다.
 *
 * suspend 와 달리 오류를 전한다. PHY 가 켜지지 않으면 링크를 되살릴 수
 * 없고, 그러면 그 아래 장치들이 전부 사라진 것과 같기 때문이다.
 *
 * 실행 컨텍스트: 시스템 절전 복귀의 noirq 단계.
 *
 * 호출 체인:
 *   (시스템 복귀) → PM 코어 → cdns_pcie_pm_ops → [이 함수]
 */
static int cdns_pcie_resume_noirq(struct device *dev)
{
	struct cdns_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] enable_phy 의 결과이자 이 함수의 반환값. */
	int ret;

	/* [한국어] PHY 를 다시 켠다. 절전 중에 전원이 끊겼으므로 처음부터 다시 해야 한다. */
	ret = cdns_pcie_enable_phy(pcie);
	/* [한국어] 실패하면 링크를 되살릴 수 없다. */
	if (ret) {
		/* [한국어] 복귀 실패는 그 아래 장치가 전부 사라지는 것과 같아 반드시 알린다. */
		dev_err(dev, "failed to enable PHY\n");
		return ret;
	}

	return 0;
}

/* [한국어] 이 IP 의 전원 관리 표. NOIRQ_SYSTEM_SLEEP_PM_OPS 매크로가 위 두
 * 함수를 suspend_noirq / resume_noirq 자리에 넣어 준다.
 * SoC 별 드라이버들이 자기 driver 구조체에서 이것을 가리킨다. */
const struct dev_pm_ops cdns_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(cdns_pcie_suspend_noirq,
				  cdns_pcie_resume_noirq)
};
EXPORT_SYMBOL_GPL(cdns_pcie_pm_ops);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe controller driver");
MODULE_AUTHOR("Cyrille Pitchen <cyrille.pitchen@free-electrons.com>");
