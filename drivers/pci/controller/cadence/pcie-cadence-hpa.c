// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence PCIe controller driver.
 *
 * Copyright (c) 2024, Cadence Design Systems
 * Author: Manikandan K Pillai <mpillai@cadence.com>
 */
/*
 * [한국어 설명] Cadence 신형(HPA) 레지스터 배치용 코어 (pcie-cadence-hpa.c)
 *
 * === 파일의 역할 ===
 * 같은 디렉터리의 pcie-cadence.c 와 하는 일이 거의 같다. 링크 상태를
 * 읽고, LTSSM 대기 시간을 설정하고, 아웃바운드 주소 변환 창을 만든다.
 * 차이는 레지스터 배치다.
 *
 * Cadence 가 IP 를 개정하면서 레지스터 맵을 새로 짰고, 커널은 그 둘을
 * LGA 와 HPA 로 구분한다. 파일 이름과 상수 접두어가 그것을 반영한다.
 *   pcie-cadence-lga-regs.h — 구형 배치의 레지스터 정의
 *   pcie-cadence-hpa-regs.h — 신형 배치의 레지스터 정의
 *   pcie-cadence.c          — 구형 배치를 쓰는 코어 함수들
 *   pcie-cadence-hpa.c      — 이 파일. 신형 배치를 쓰는 같은 함수들
 *
 * 신형에서 눈에 띄는 구조적 변화가 둘이다.
 *
 * 첫째, 레지스터가 뱅크로 나뉘었다. 구형은 오프셋 하나로 접근했지만
 * 신형은 뱅크를 함께 지정한다(REG_BANK_IP_REG, REG_BANK_AXI_SLAVE 등).
 * 그래서 접근자도 cdns_pcie_readl 이 아니라 cdns_pcie_hpa_readl 로,
 * 인자가 하나 더 붙는다.
 *
 * 둘째, requester ID 를 채우는 방식이 바뀌었다. 구형은 DESC0 의 비트 23
 * 하나로 "소프트웨어가 채운다" 를 표시했는데, 신형은 CTRL0 라는 별도
 * 레지스터에 SUPPLY_BUS 와 SUPPLY_DEV_FN 두 비트를 둔다. 버스와
 * 디바이스·펑션을 따로 제어할 수 있게 세분화된 것이다.
 * 필드 위치도 달라져서, 구형이 DESC0 비트 26:24 에 펑션 번호를 두었다면
 * 신형은 DESC1 비트 31:24 에 둔다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 신형 IP 를 쓰는 SoC 드라이버의 probe
 *   -> pcie-cadence-host-hpa.c 또는 대응하는 ep 코드
 *      -> [이 파일] cdns_pcie_hpa_set_outbound_region() 등
 *         -> cdns_pcie_hpa_writel() [pcie-cadence.h 의 인라인 함수]
 *
 * 구형과 신형 중 어느 쪽을 쓸지는 SoC 드라이버가 자기 ops 표에
 * 어느 함수를 넣느냐로 정한다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie-cadence-host-hpa.c 와 신형 IP 를 쓰는 SoC 드라이버들.
 * 아래쪽: pcie-cadence.h 의 뱅크 인식 레지스터 접근자,
 *   pcie-cadence-hpa-regs.h 의 상수 정의.
 * 공유 상태: struct cdns_pcie — 구형과 같은 구조체를 쓴다. 레지스터
 *   배치만 다를 뿐 컨트롤러의 논리적 상태는 같기 때문이다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — cdns_ 로 시작하는 심볼 호출 0건).
 *
 * 구형 판인 pcie-cadence.c 와 마찬가지로 관계는 토폴로지상의 것이다.
 * 이 IP 를 쓰는 SoC 에 NVMe 를 붙이면 이 코드가 만든 주소 변환 창을
 * 통해 config 접근과 MMIO 가 오간다.
 *
 * === 주요 함수/구조체 요약 ===
 * cdns_pcie_hpa_link_up()  : 신형 배치에서 링크 상태를 읽는다.
 *                            구형의 cdns_pcie_linkup() 에 대응한다.
 *                            다만 구형 경로가 실제로 넘기는 것은
 *                            cdns_pcie_link_up() 디스패처 쪽이다.
 * cdns_pcie_hpa_detect_quiet_min_delay_set() : LTSSM Detect.Quiet 최소
 *                            대기를 설정한다. 구형 판과 값이 같다.
 * cdns_pcie_hpa_set_outbound_region() : 아웃바운드 창 설정.
 *                            이 파일의 본체이며 CTRL0 처리가 구형과 다르다.
 * cdns_pcie_hpa_set_outbound_region_for_normal_msg() : 메시지 TLP 전용 창.
 *
 * 참고: 구형 판에 있는 cdns_pcie_reset_outbound_region() 에 대응하는
 * 함수가 이 파일에는 없다. 신형에서 창을 지우는 코드가 어디에 있는지는
 * 이 트리에서 확인하지 못했다.
 */

/* [한국어] fls64, ilog2 등 비트 연산 헬퍼. 창 크기 계산에 쓴다. */
#include <linux/kernel.h>
/* [한국어] 디바이스 트리 접근. 다만 이 파일이 직접 쓰는 of_* 심볼은
 * 확인되지 않았다 — 구형 판과 형태를 맞춘 것으로 보인다. */
#include <linux/of.h>

/* [한국어] struct cdns_pcie 와 cdns_pcie_hpa_readl / writel 접근자,
 * 그리고 REG_BANK_* 뱅크 상수. 신형 레지스터 정의는 이 헤더가
 * pcie-cadence-hpa-regs.h 를 포함해 가져온다. */
#include "pcie-cadence.h"

/* [한국어]
 * cdns_pcie_hpa_link_up - 신형 레지스터 배치에서 링크 상태를 읽는다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 링크가 살아 있으면 true.
 *
 * 구형의 cdns_pcie_linkup() 과 판정 방식은 같다(다만 구형 호스트 경로가
 * 실제로 넘기는 것은 cdns_pcie_link_up() 디스패처다) — 어떤 레지스터의
 * 최하위 비트를 본다. 다른 것은 그 레지스터의 위치다.
 *   구형: CDNS_PCIE_LM_BASE (Local Management 블록)
 *   신형: REG_BANK_IP_REG 뱅크의 PHY_DBG_STS_REG0 (PHY 디버그 상태)
 * 뱅크를 함께 지정해야 하는 것이 신형 접근자의 특징이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트, 또는 링크 대기 루프 안.
 *
 * 호출 체인:
 *   pcie-cadence-host-hpa.c 의 링크 대기 → [이 함수]
 */
bool cdns_pcie_hpa_link_up(struct cdns_pcie *pcie)
{
	u32 pl_reg_val;

	/* [한국어] IP 레지스터 뱅크의 PHY 디버그 상태 레지스터를 읽는다. */
	pl_reg_val = cdns_pcie_hpa_readl(pcie, REG_BANK_IP_REG, CDNS_PCIE_HPA_PHY_DBG_STS_REG0);
	/* [한국어] 비트 0 이 링크 상태다. 구형 판과 같은 표기를 쓴다. */
	if (pl_reg_val & GENMASK(0, 0))
		return true;
	return false;
}
EXPORT_SYMBOL_GPL(cdns_pcie_hpa_link_up);

/* [한국어]
 * cdns_pcie_hpa_detect_quiet_min_delay_set - LTSSM Detect.Quiet 대기 설정
 *
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 *
 * 구형 판(cdns_pcie_detect_quiet_min_delay_set)과 목적도 값도 같다.
 * Detect.Quiet 은 LTSSM 이 선로가 조용해지기를 기다리는 구간이며,
 * 너무 짧으면 안정되지 않은 신호를 오인해 트레이닝이 실패할 수 있다.
 *
 * 레지스터 위치만 다르다 — 구형은 LTSSM_CONTROL_CAP, 신형은 IP 레지스터
 * 뱅크의 PHY_LAYER_CFG0 다. 변수 이름이 ltssm_control_cap 으로 남아 있는
 * 것은 구형 판에서 옮겨 온 흔적으로 보인다.
 *
 * 실행 컨텍스트: 링크 트레이닝 시작 전 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   신형 IP 를 쓰는 SoC 드라이버의 초기화 → [이 함수]
 */
void cdns_pcie_hpa_detect_quiet_min_delay_set(struct cdns_pcie *pcie)
{
	/* [한국어] 2ms 에 해당하는 인코딩. 구형 판과 같은 값이며, 그 대응
	 * 자체의 근거는 Cadence IP 문서에 있어 이 트리에서 확인할 수 없다. */
	u32 delay = 0x3;
	u32 ltssm_control_cap;

	/* Set the LTSSM Detect Quiet state min. delay to 2ms */
	/* [한국어] 읽어서 해당 필드만 고치고 다시 쓴다. 같은 레지스터에
	 * 다른 PHY 설정도 들어 있어 통째로 덮으면 안 된다. */
	ltssm_control_cap = cdns_pcie_hpa_readl(pcie, REG_BANK_IP_REG,
						CDNS_PCIE_HPA_PHY_LAYER_CFG0);
	ltssm_control_cap = ((ltssm_control_cap &
			    /* [한국어] 해당 필드만 지우고 새 값을 끼워 넣는다. */
			    ~CDNS_PCIE_HPA_DETECT_QUIET_MIN_DELAY_MASK) |
			    CDNS_PCIE_HPA_DETECT_QUIET_MIN_DELAY(delay));

	/* [한국어] 고친 값을 다시 쓴다. 링크 트레이닝 전에 반영되어야 한다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_IP_REG,
			     CDNS_PCIE_HPA_PHY_LAYER_CFG0, ltssm_control_cap);
}
EXPORT_SYMBOL_GPL(cdns_pcie_hpa_detect_quiet_min_delay_set);

/* [한국어]
 * cdns_pcie_hpa_set_outbound_region - 신형 배치에서 아웃바운드 창을 설정한다
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
 * 구형의 cdns_pcie_set_outbound_region() 에 대응한다. 크기를 비트 수로
 * 표현하는 것, 하위 8비트를 잘라 내는 것, 최소 8비트 보정까지 모두 같다.
 *
 * 다른 점은 requester ID 처리다. 상류 주석이 밝히듯 신형은
 *   비트 위치가 바뀌었고 — 펑션 번호가 DESC0 이 아니라 DESC1 의 31:24 에,
 *     버스 번호도 DESC1 에, "소프트웨어가 채운다" 표시가 DESC0 비트 23 이
 *     아니라 비트 26 에 대응하는 자리로 옮겼다.
 *   제어가 CTRL0 로 분리되었다 — SUPPLY_BUS 와 SUPPLY_DEV_FN 을 따로
 *     켤 수 있어, 버스만 소프트웨어가 주고 디바이스·펑션은 하드웨어가
 *     채우는 식의 조합도 가능해졌다.
 *
 * 또 구형과 달리 cpu_addr_fixup 을 부르지 않는다. 신형 IP 를 쓰는
 * SoC 들이 그런 보정을 필요로 하지 않아서인지, 아니면 다른 곳에서
 * 처리하는지는 이 트리에서 확인하지 못했다 — 코드에 있는 그대로만 적는다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie-cadence-host-hpa.c 등의 초기화 → [이 함수]
 *     → cdns_pcie_hpa_writel()
 */
void cdns_pcie_hpa_set_outbound_region(struct cdns_pcie *pcie, u8 busnr, u8 fn,
				       u32 r, bool is_io,
				       u64 cpu_addr, u64 pci_addr, size_t size)
{
	/*
	 * roundup_pow_of_two() returns an unsigned long, which is not suited
	 * for 64bit values
	 */
	/* [한국어] 구형 판과 같은 계산이다. roundup_pow_of_two() 가
	 * unsigned long 을 돌려줘 32비트 아키텍처에서 64비트 크기를 다룰 수
	 * 없으므로 fls64 로 직접 구한다. */
	u64 sz = 1ULL << fls64(size - 1);
	/* [한국어] 크기를 비트 수로 바꾼다. 이 IP 는 창 크기를 그렇게 표현한다. */
	int nbits = ilog2(sz);
	/* [한국어] ctrl0 이 구형에는 없던 변수다. 신형에서 requester ID
	 * 공급 여부를 이 레지스터로 제어한다. */
	u32 addr0, addr1, desc0, desc1, ctrl0;

	/* [한국어] 최소 8비트(256바이트). 아래에서 주소의 하위 8비트를
	 * 잘라 내는 것과 짝이 맞는다. */
	if (nbits < 8)
		nbits = 8;

	/* Set the PCI address */
	/* [한국어] 목적지 PCIe 주소와 창 크기를 한 워드에 담는다.
	 * 하위 8비트가 크기 필드라 주소에서 잘라 낸다. */
	addr0 = CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_NBITS(nbits) |
		(lower_32_bits(pci_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(pci_addr);

	/* [한국어] 목적지 PCIe 주소의 하위 워드(크기 필드 포함)를 쓴다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0(r), addr0);
	/* [한국어] 상위 32비트를 쓴다. 64비트 주소 지원. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR1(r), addr1);

	/* Set the PCIe header descriptor */
	/* [한국어] TLP 타입을 정한다. I/O 와 메모리는 헤더 타입이 다르다. */
	if (is_io)
		desc0 = CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_IO;
	else
		desc0 = CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_MEM;
	/* [한국어] 아래 분기에서 채운다. RC 모드면 버스·디바이스·펑션이
	 * 전부, EP 모드면 펑션만 들어간다. */
	desc1 = 0;
	/* [한국어] 0 이면 하드웨어가 requester ID 를 채운다는 뜻이다.
	 * EP 모드에서는 이 값이 그대로 남는다. */
	ctrl0 = 0;

	/*
	 * Whether Bit [26] is set or not inside DESC0 register of the outbound
	 * PCIe descriptor, the PCI function number must be set into
	 * Bits [31:24] of DESC1 anyway.
	 *
	 * In Root Complex mode, the function number is always 0 but in Endpoint
	 * mode, the PCIe controller may support more than one function. This
	 * function number needs to be set properly into the outbound PCIe
	 * descriptor.
	 *
	 * Besides, setting Bit [26] is mandatory when in Root Complex mode:
	 * then the driver must provide the bus, resp. device, number in
	 * Bits [31:24] of DESC1, resp. Bits[23:16] of DESC0. Like the function
	 * number, the device number is always 0 in Root Complex mode.
	 *
	 * However when in Endpoint mode, we can clear Bit [26] of DESC0, hence
	 * the PCIe controller will use the captured values for the bus and
	 * device numbers.
	 */
	if (pcie->is_rc) {
		/* The device and function numbers are always 0 */
		/* [한국어] 루트 컴플렉스 모드. 소프트웨어가 requester ID 를
		 * 전부 채운다. 루트 포트 자신은 항상 디바이스 0, 펑션 0 이다. */
		desc1 = CDNS_PCIE_HPA_AT_OB_REGION_DESC1_BUS(busnr) |
			CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN(0);
		/* [한국어] 신형에서 새로 생긴 부분이다. 두 SUPPLY 비트를 켜야
		 * 하드웨어가 위에서 넣은 값을 실제로 쓴다. 구형이 DESC0 의
		 * 비트 하나로 하던 일을 버스와 디바이스·펑션으로 나눠 놓았다. */
		ctrl0 = CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_BUS |
			CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_DEV_FN;
	} else {
		/*
		 * Use captured values for bus and device numbers but still
		 * need to set the function number
		 */
		/* [한국어] 엔드포인트 모드. ctrl0 을 0 으로 두어 버스와
		 * 디바이스 번호는 하드웨어가 호스트에게서 받아 기억해 둔
		 * 값을 쓰게 한다 — 엔드포인트의 번호는 호스트가 열거하며
		 * 정해 주므로 소프트웨어가 미리 알 수 없다.
		 * 다만 펑션 번호는 여전히 직접 넣어야 한다. */
		desc1 |= CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN(fn);
	}

	/* [한국어] TLP 타입과 requester ID 를 쓴다. 뱅크가 AXI_SLAVE 인
	 * 것은 이 창들이 AXI 슬레이브 쪽 주소 변환이기 때문이다 —
	 * CPU 가 AXI 를 통해 이 컨트롤러에 접근하면 그것이 PCIe 로 나간다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC0(r), desc0);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC1(r), desc1);

	/* [한국어] CPU 쪽 주소도 같은 형식이다. 크기를 다시 넣는 것은
	 * 양쪽 창의 크기가 같아야 변환이 성립하기 때문이다. */
	addr0 = CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0_NBITS(nbits) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);

	/* [한국어] CPU 쪽 주소의 하위 워드를 쓴다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0(r), addr0);
	/* [한국어] 상위 32비트. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR1(r), addr1);
	/* [한국어] 마지막으로 CTRL0 를 쓴다. 이 쓰기로 requester ID 공급 방식이
	 * 확정되고 창이 실제로 동작하기 시작한다 — 앞의 설정이 모두 자리
	 * 잡은 뒤여야 하므로 순서가 중요하다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CTRL0(r), ctrl0);
}
EXPORT_SYMBOL_GPL(cdns_pcie_hpa_set_outbound_region);

/* [한국어]
 * cdns_pcie_hpa_set_outbound_region_for_normal_msg - 메시지 TLP 전용 창
 *
 * @pcie: 대상 컨트롤러.
 * @busnr: RC 모드에서 쓸 버스 번호.
 * @fn: 펑션 번호.
 * @r: 몇 번째 창인지.
 * @cpu_addr: 이 창에 대응할 CPU 주소.
 * @return: 없음.
 *
 * 구형의 cdns_pcie_set_outbound_region_for_normal_msg() 에 대응한다.
 * PCIe 의 메시지 TLP(INTx 인터럽트, 전원 관리 이벤트, 오류 보고 등)를
 * 보내기 위한 창이다.
 *
 * 일반 창과 다른 점은 구형과 같다 — PCI 주소를 0 으로 두고(메시지에는
 * 목적지 주소가 없다), 크기를 17비트로 고정한다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   신형 IP 를 쓰는 host/ep 초기화 → [이 함수]
 */
void cdns_pcie_hpa_set_outbound_region_for_normal_msg(struct cdns_pcie *pcie,
						      u8 busnr, u8 fn,
						      u32 r, u64 cpu_addr)
{
	u32 addr0, addr1, desc0, desc1, ctrl0;

	/* [한국어] 이 창으로 나가는 접근을 메시지 TLP 로 만든다. */
	desc0 = CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_NORMAL_MSG;
	desc1 = 0;
	ctrl0 = 0;

	/* See cdns_pcie_set_outbound_region() comments above */
	/* [한국어] requester ID 처리는 위 일반 창 함수와 완전히 같다.
	 * 상류 주석이 그쪽을 보라고 하고 있어 반복하지 않는다. */
	if (pcie->is_rc) {
		desc1 = CDNS_PCIE_HPA_AT_OB_REGION_DESC1_BUS(busnr) |
			CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN(0);
		/* [한국어] 메시지 창에서도 두 SUPPLY 비트를 켜야 위에서 넣은 버스·디바이스
		 * 번호가 실제로 쓰인다. */
		ctrl0 = CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_BUS |
			/* [한국어] 디바이스·펑션 공급도 함께 켠다. */
			CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_DEV_FN;
	} else {
		desc1 |= CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN(fn);
	}

	/* [한국어] 크기가 17비트(128KB)로 고정이다. 구형 판과 같은 값이며
	 * 그 근거는 Cadence IP 문서에 있어 이 트리에서 확인할 수 없다. */
	addr0 = CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0_NBITS(17) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);

	/* [한국어] PCI 주소를 0 으로 지운다. 메시지 TLP 에는 목적지 주소가 없으므로
	 * 남은 값이 오해를 부르지 않도록 명시적으로 비운다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0(r), 0);
	/* [한국어] 상위 워드도 지운다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR1(r), 0);
	/* [한국어] 메시지 타입을 쓴다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC0(r), desc0);
	/* [한국어] requester ID 를 쓴다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC1(r), desc1);
	/* [한국어] CPU 주소의 하위 워드. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0(r), addr0);
	/* [한국어] 상위 32비트. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR1(r), addr1);
	/* [한국어] CTRL0 로 마무리한다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CTRL0(r), ctrl0);
}
EXPORT_SYMBOL_GPL(cdns_pcie_hpa_set_outbound_region_for_normal_msg);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe controller driver");
