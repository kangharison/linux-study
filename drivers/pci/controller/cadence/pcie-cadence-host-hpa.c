// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence PCIe host controller driver.
 *
 * Copyright (c) 2024, Cadence Design Systems
 * Author: Manikandan K Pillai <mpillai@cadence.com>
 */
/*
 * [한국어 설명] 신형(HPA) 레지스터 배치의 호스트 구현 (pcie-cadence-host-hpa.c)
 *
 * === 파일의 역할 ===
 * pcie-cadence-host.c 와 하는 일이 같고 레지스터 배치만 다르다.
 * Cadence 가 IP 를 개정하며 만든 신형 맵(HPA)을 쓰는 호스트 코드이며,
 * 구형(LGA)과의 관계는 pcie-cadence-hpa.c 와 pcie-cadence.c 의 관계와 같다.
 *
 * 하는 일이 넷이다.
 *   config 접근 — 루트 버스는 레지스터 베이스로 직접, 그 아래는
 *     아웃바운드 창 0번을 config 타입으로 바꿔 가며 접근한다.
 *   루트 포트 초기화 — 자기 config 헤더(클래스 코드 등)를 채운다.
 *   주소 변환 — dma-ranges 를 인바운드 BAR 에, 자원 창을 아웃바운드에 건다.
 *   링크 대기 — 트레이닝이 끝나기를 기다린다.
 *
 * config 접근 방식이 이 파일에서 가장 눈여겨볼 부분이다. 아웃바운드
 * 창 0번 하나를 config 전용으로 잡아 두고, 접근할 때마다 그 창의
 * 목적지(버스·devfn)와 TLP 타입을 다시 쓴다. 즉 config 읽기 한 번이
 * 레지스터 쓰기 여러 번을 동반한다.
 * 창을 매번 고쳐야 하므로 동시 접근을 직렬화해야 하는데, 그 보호는
 * 이 파일이 아니라 PCI 코어가 한다 — map_bus 는 access.c 의
 * pci_lock(raw spinlock, 인터럽트 끔) 아래에서 불린다.
 *
 * 구형과 다른 점으로 확인되는 것:
 *   - 레지스터 뱅크를 함께 지정한다(REG_BANK_AXI_SLAVE, _IP_REG,
 *     _AXI_MASTER). 인바운드는 AXI_MASTER, 아웃바운드는 AXI_SLAVE 다 —
 *     방향에 따라 이 IP 가 AXI 버스의 어느 쪽으로 동작하는지를 반영한다.
 *   - requester ID 공급을 CTRL0 의 SUPPLY_BUS / SUPPLY_DEV_FN 로 제어한다.
 *   - PTM(Precision Time Measurement) 응답을 켜는 코드가 있다.
 *     구형 판에는 대응하는 함수가 없다.
 *   - 링크 대기가 cdns_pcie_host_start_link() 를 거치지 않고
 *     cdns_pcie_host_wait_for_link() 를 직접 부른다. 그래서 구형에만
 *     있는 Gen2 재트레이닝 quirk 를 타지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 신형 IP 를 쓰는 SoC 드라이버(pci-sky1.c)의 probe
 *   -> cdns_pcie_hpa_host_setup() [이 파일]
 *      -> cdns_pcie_hpa_host_init() 로 루트 포트와 주소 변환 설정
 *         -> cdns_pcie_hpa_set_outbound_region() [pcie-cadence-hpa.c]
 *         -> cdns_pcie_host_map_dma_ranges() [pcie-cadence-host-common.c]
 *            (인바운드 설정 방법으로 이 파일의 bar_ib_config 를 넘긴다)
 *      -> cdns_pcie_hpa_host_link_setup() 으로 링크를 올리고 기다린다
 *      -> pci_host_probe() -> PCI 코어 열거
 *
 * 실행 컨텍스트: 초기화는 프로세스 컨텍스트. map_bus 는 PCI 코어의
 *   pci_lock 아래(인터럽트 꺼진 상태)에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-sky1.c 등 신형 IP 를 쓰는 SoC 드라이버.
 * 아래쪽: pcie-cadence-hpa.c(신형 창 설정), pcie-cadence-host-common.c
 *   (링크 대기와 BAR 배정 알고리즘), pcie-cadence.h(뱅크 인식 접근자).
 * 공유 상태: struct cdns_pcie_rc — 인바운드 BAR 가용 표(avail_ib_bar),
 *   config 창의 가상 주소(cfg_base), 그리고 공통 컨트롤러 구조체.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — cdns_ 심볼 호출 0건). 관계는 토폴로지상의 것이다.
 *
 * 다만 config 접근 방식이 성능에 영향을 준다는 점은 짚어 둘 만하다.
 * 창을 매번 고쳐 쓰므로 config 읽기 하나가 여러 번의 MMIO 가 되고,
 * 그동안 pci_lock 을 쥔다. NVMe 열거처럼 config 접근이 많은 구간에서는
 * 그 비용이 쌓인다 — ECAM 을 쓰는 호스트가 이 점에서 유리하다.
 *
 * === 주요 함수/구조체 요약 ===
 * cdns_pci_hpa_map_bus()    : config 접근 주소를 만든다. 아웃바운드 창
 *                             0번을 목적지에 맞게 다시 설정한다.
 * cdns_pcie_hpa_host_ops    : 그 map_bus 와 범용 읽기·쓰기를 묶은 표.
 * cdns_pcie_hpa_host_enable_ptm_response() : PTM 응답을 켠다.
 * cdns_pcie_hpa_host_bar_ib_config() : 인바운드 BAR 하나를 설정한다.
 *                             공통 코드에 함수 포인터로 넘겨진다.
 * cdns_pcie_hpa_host_init_root_port() : 루트 포트의 config 헤더를 채운다.
 * cdns_pcie_hpa_create_region_for_cfg() : config 전용 아웃바운드 창을 만든다.
 * cdns_pcie_hpa_host_init_address_translation() : 자원 창과 dma-ranges 를
 *                             각각 아웃바운드·인바운드에 건다.
 * cdns_pcie_hpa_host_init() : 위 초기화들을 묶는다.
 * cdns_pcie_hpa_host_link_setup() : 링크를 올리고 기다린다.
 * cdns_pcie_hpa_host_setup() : 전체 진입점. SoC 드라이버가 부른다.
 * bar_aperture_mask[]       : BAR 별 크기 필드 마스크.
 */

/* [한국어] 지연 함수. 다만 이 파일이 직접 쓰는 delay 심볼은 확인되지
 * 않았다 — 구형 판에서 옮겨 온 형태로 보인다. */
#include <linux/delay.h>
/* [한국어] ilog2 등 기본 커널 매크로. 아래 aperture 계산에 쓴다. */
#include <linux/kernel.h>
/* [한국어] list_sort. 다만 정렬은 공통 코드(pcie-cadence-host-common.c)가
 * 하므로 이 파일이 직접 쓰는 심볼은 확인되지 않았다. */
#include <linux/list_sort.h>
/* [한국어] 디바이스 트리 주소 파싱. */
#include <linux/of_address.h>
/* [한국어] PCI 관련 디바이스 트리 헬퍼. */
#include <linux/of_pci.h>
/* [한국어] 디바이스 트리 인터럽트 파싱. */
#include <linux/of_irq.h>
/* [한국어] 플랫폼 장치 자원 조회. */
#include <linux/platform_device.h>

/* [한국어] struct cdns_pcie_rc, 뱅크 인식 접근자, 신형 레지스터 상수. */
#include "pcie-cadence.h"
/* [한국어] 구형·신형이 공유하는 링크 대기와 BAR 배정 함수의 선언,
 * 그리고 함수 포인터 타입 둘. 이 파일이 bar_ib_config 를 그쪽에
 * 넘기기 위해 필요하다. */
#include "pcie-cadence-host-common.h"

/* [한국어] BAR 별 크기(aperture) 필드의 마스크.
 * 0x3F 는 6비트이므로 2^63 까지 표현할 수 있다는 뜻이다.
 * RP_NO_BAR 항목이 없는 점에 주의 — 아래 bar_ib_config 가 그 경우를
 * 따로 처리한다.
 * 같은 이름의 배열이 구형 판(pcie-cadence-host.c)에도 있는데, 둘 다
 * static 이라 이름이 겹쳐도 문제가 없다. */
static u8 bar_aperture_mask[] = {
	[RP_BAR0] = 0x3F,
	[RP_BAR1] = 0x3F,
};

/* [한국어]
 * cdns_pci_hpa_map_bus - config 접근을 위한 주소를 만든다
 *
 * @bus: 접근할 버스.
 * @devfn: 그 버스에서의 장치·펑션 번호.
 * @where: config space 안의 오프셋.
 * @return: 실제로 읽고 쓸 가상 주소. 접근할 수 없으면 NULL.
 *
 * PCI 코어의 config 접근이 이 함수를 거친다. 이름은 "주소를 만든다"
 * 지만 실제로는 부수효과가 크다 — 루트 버스가 아니면 아웃바운드 창
 * 0번을 이번 목적지에 맞게 다시 설정한다.
 *
 * 두 갈래로 나뉜다.
 *   루트 버스 — 루트 포트 자신의 config 다. 창을 쓸 필요 없이
 *     레지스터 베이스에서 바로 읽는다. devfn 이 0 이 아니면 NULL 을
 *     돌려주는데, 루트 버스에는 루트 포트 하나만 있기 때문이다.
 *   그 아래 — 아웃바운드 창 0번을 config TLP 로 설정하고, 목적지
 *     버스·devfn 을 그 창에 실은 뒤 cfg_base 로 접근한다.
 *
 * 타입 0 과 타입 1 을 가르는 기준이 중요하다. 바로 아래 버스면
 * 타입 0(그 버스의 장치에 직접 전달), 더 아래면 타입 1(중간 브리지가
 * 다시 전달)이다. PCIe 규격이 정한 구분이다.
 *
 * 실행 컨텍스트: PCI 코어의 pci_lock(raw spinlock, 인터럽트 끔) 아래.
 *   그래서 창을 고치는 동안 다른 config 접근이 끼어들지 않는다.
 *   잠들 수 없으므로 이 함수 안에서 대기하면 안 된다.
 *
 * 에러 경로: NULL 을 돌려주면 PCI 코어가 그 접근을 실패로 처리하고
 *   모두 1(0xFFFFFFFF)을 읽은 것처럼 다룬다 — 장치 없음과 같은 결과다.
 *
 * 호출 체인:
 *   pci_generic_config_read/write [access.c] → [이 함수]
 *     → cdns_pcie_hpa_writel()
 */
void __iomem *cdns_pci_hpa_map_bus(struct pci_bus *bus, unsigned int devfn,
				   int where)
{
	/* [한국어] 버스에서 거슬러 올라가 이 컨트롤러의 구조체를 찾는다. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct cdns_pcie_rc *rc = pci_host_bridge_priv(bridge);
	struct cdns_pcie *pcie = &rc->pcie;
	unsigned int busn = bus->number;
	/* [한국어] 창 0번에 쓸 값들. addr0 은 목적지, desc 는 TLP 종류,
	 * ctrl0 은 requester ID 공급 방식이다. */
	u32 addr0, desc0, desc1, ctrl0;
	/* [한국어] 링크 다운 상태를 읽어 지울 때 쓰는 임시 변수. */
	u32 regval;

	if (pci_is_root_bus(bus)) {
		/*
		 * Only the root port (devfn == 0) is connected to this bus.
		 * All other PCI devices are behind some bridge hence on another
		 * bus.
		 */
		/* [한국어] 상류 주석대로 루트 버스에는 루트 포트 하나뿐이다.
		 * 열거 중에 PCI 코어가 devfn 1~255 도 훑어 보는데, 그때
		 * NULL 을 돌려주면 "없는 장치" 로 처리되어 넘어간다. */
		if (devfn)
			return NULL;

		/* [한국어] 루트 포트 자신의 config 는 창을 거치지 않고
		 * 레지스터 베이스에서 직접 읽는다. 0xfff 마스크는 config
		 * space 가 4KB 라는 뜻이다(확장 config 포함).
		 *
		 * 참고: 이 베이스는 cdns_pcie_hpa_rp_ 계열 접근자가 쓰는
		 * CDNS_PCIE_RP_BASE 오프셋과는 다른 별칭으로 보인다.
		 * 두 경로의 관계를 이 트리에서 확인하지는 못했다. */
		return pcie->reg_base + (where & 0xfff);
	}

	/* Clear AXI link-down status */
	/* [한국어] 링크가 끊겼던 흔적을 지운다. 이것을 지우지 않으면
	 * 이후 접근이 막히거나 오류로 보고될 수 있기 때문이다.
	 * 읽어서 비트 0 만 지우고 다시 쓰는 방식이라, 다른 상태 비트는
	 * 보존된다. */
	regval = cdns_pcie_hpa_readl(pcie, REG_BANK_AXI_SLAVE, CDNS_PCIE_HPA_AT_LINKDOWN);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE, CDNS_PCIE_HPA_AT_LINKDOWN,
			     (regval & ~GENMASK(0, 0)));

	/* Update Output registers for AXI region 0 */
	/* [한국어] 창 0번의 목적지를 이번 접근에 맞게 다시 쓴다.
	 * 크기를 12비트(4KB)로 고정하는 것은 config space 하나가 4KB 라
	 * 그만큼만 열면 충분하기 때문이다.
	 * 목적지 주소 대신 버스·devfn 을 싣는 점이 일반 창과 다르다 —
	 * config TLP 는 주소가 아니라 그 둘로 대상을 지정한다. */
	addr0 = CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_NBITS(12) |
		CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) |
		CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0_BUS(busn);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR0(0), addr0);

	/* [한국어] DESC1 은 읽어서 devfn 필드만 고친다. 여기 들어가는 것은
	 * 목적지가 아니라 요청자(루트 포트 자신)의 펑션 번호라 0 이다. */
	desc1 = cdns_pcie_hpa_readl(pcie, REG_BANK_AXI_SLAVE,
				    CDNS_PCIE_HPA_AT_OB_REGION_DESC1(0));
	desc1 &= ~CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN_MASK;
	desc1 |= CDNS_PCIE_HPA_AT_OB_REGION_DESC1_DEVFN(0);
	/* [한국어] 두 SUPPLY 비트를 켜 소프트웨어가 넣은 버스·devfn 을
	 * 하드웨어가 쓰게 한다. 신형에서 새로 생긴 제어다. */
	ctrl0 = CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_BUS |
		CDNS_PCIE_HPA_AT_OB_REGION_CTRL0_SUPPLY_DEV_FN;

	/* [한국어] 타입 0 과 타입 1 의 구분. 목적지가 루트 포트 바로 아래
	 * 버스면 타입 0 — 그 버스의 장치에 곧바로 전달된다.
	 * 더 아래면 타입 1 — 중간 브리지가 받아 다시 아래로 전달한다.
	 * bridge->busnr 은 루트 버스 번호이므로 +1 이 바로 아래 버스다. */
	if (busn == bridge->busnr + 1)
		desc0 = CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0;
	else
		desc0 = CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1;

	/* [한국어] 세 레지스터를 차례로 반영한다. 이 쓰기들이 끝나야
	 * 아래 cfg_base 접근이 올바른 목적지로 나간다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC0(0), desc0);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC1(0), desc1);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CTRL0(0), ctrl0);

	/* [한국어] config 전용 창에 매핑해 둔 가상 주소에 오프셋을 더해
	 * 돌려준다. PCI 코어가 이 주소로 실제 읽기·쓰기를 한다. */
	return rc->cfg_base + (where & 0xfff);
}

/* [한국어] 이 컨트롤러의 config 접근 표.
 * 주소 계산만 이 파일이 하고 실제 읽기·쓰기는 커널 범용 구현에 맡긴다.
 * 그럴 수 있는 이유는 map_bus 가 돌려준 주소가 평범한 MMIO 라
 * 임의 폭 접근이 가능하기 때문이다 — 같은 IP 를 쓰는 pcie-sg2042.c 가
 * 루트 버스에만 32비트 전용 ops 를 따로 두었던 것과 대비된다. */
static struct pci_ops cdns_pcie_hpa_host_ops = {
	.map_bus	= cdns_pci_hpa_map_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

/* [한국어]
 * cdns_pcie_hpa_host_enable_ptm_response - PTM 응답 기능을 켠다
 *
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 *
 * PTM(Precision Time Measurement)은 PCIe 규격의 시간 동기화 기능이다.
 * 엔드포인트가 호스트에게 "지금 몇 시냐" 를 묻고, 그 왕복 지연까지
 * 감안해 자기 시계를 호스트와 맞춘다. 오디오나 네트워크 타임스탬프처럼
 * 장치와 호스트의 시각이 일치해야 하는 용도에 쓰인다.
 *
 * 루트 포트 쪽은 그 질문에 답하는 역할이라 "응답(response) 활성화" 다.
 * 이 비트를 켜지 않으면 아래 장치가 PTM 을 쓸 수 없다.
 *
 * 구형 판(pcie-cadence-host.c)에는 대응하는 함수가 없다. 신형에서
 * 추가된 것으로 보이나, 구형 IP 가 PTM 을 지원하지 않아서인지
 * 단지 구현되지 않은 것인지는 이 트리에서 확인하지 못했다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_hpa_host_init_root_port() → [이 함수]
 */
static void cdns_pcie_hpa_host_enable_ptm_response(struct cdns_pcie *pcie)
{
	u32 val;

	/* [한국어] 읽어서 비트 하나만 켠다. 이 레지스터에 다른 PTM 설정도
	 * 있을 수 있어 통째로 덮어쓰지 않는다. */
	val = cdns_pcie_hpa_readl(pcie, REG_BANK_IP_REG, CDNS_PCIE_HPA_LM_PTM_CTRL);
	cdns_pcie_hpa_writel(pcie, REG_BANK_IP_REG, CDNS_PCIE_HPA_LM_PTM_CTRL,
			     val | CDNS_PCIE_HPA_LM_PTM_CTRL_PTMRSEN);
}

/* [한국어]
 * cdns_pcie_hpa_host_bar_ib_config - 인바운드 BAR 하나를 설정한다
 *
 * @rc: 루트 컴플렉스.
 * @bar: 설정할 BAR. RP_NO_BAR 는 "BAR 없이 통과" 를 뜻하는 특별 항목이다.
 * @cpu_addr: 들어온 요청이 닿을 이쪽 주소.
 * @size: 창 크기.
 * @flags: 자원 플래그. IORESOURCE_PREFETCH 를 본다.
 * @return: 0 이면 성공. 그 BAR 가 이미 쓰이고 있으면 -ENODEV.
 *
 * 공통 코드(pcie-cadence-host-common.c)에 함수 포인터로 넘겨지는 함수다.
 * 그쪽이 dma-ranges 를 크기 순으로 정렬해 BAR 를 골라 주면, 실제
 * 레지스터를 쓰는 것은 이 함수다.
 *
 * 구형 판(pcie-cadence-host.c 의 cdns_pcie_host_bar_ib_config)과 비교하면
 * 눈에 띄는 차이가 있다. prefetch 처리다.
 *
 *   구형: MEM_64BITS(0x6)를 조건부로, PREF_MEM_64BITS(0x7)를 무조건 OR
 *         한다. 두 값이 같은 3비트 필드에 들어가고 0x6|0x7 = 0x7 이라
 *         결과가 언제나 0x7 이 되어, IORESOURCE_PREFETCH 검사가 최종
 *         값에 영향을 주지 못한다.
 *   신형: 상수가 MEM_64BITS(0x5)와 PREFETCH_MEM_64BITS(0xD)로, 두 값이
 *         비트 3(0x8) 하나만 다르다. 그리고 이 코드는 MEM 을 무조건,
 *         PREF 를 조건부로 OR 한다. 0x5 | 0xD = 0xD 이므로 prefetch 일
 *         때만 비트 3 이 서고, 아니면 0x5 로 남는다.
 *   즉 신형에서는 prefetch 구분이 실제로 동작한다.
 *   (32비트 쪽도 같다 — 0x1 과 0x9, 역시 비트 3 차이.)
 *   상수는 pcie-cadence-hpa-regs.h:75-80 에서 확인했다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_hpa_host_init_address_translation()
 *     → cdns_pcie_host_map_dma_ranges() [pcie-cadence-host-common.c]
 *       → cdns_pcie_host_bar_config() → [이 함수]
 */
static int cdns_pcie_hpa_host_bar_ib_config(struct cdns_pcie_rc *rc,
					    enum cdns_pcie_rp_bar bar,
					    u64 cpu_addr, u64 size,
					    unsigned long flags)
{
	struct cdns_pcie *pcie = &rc->pcie;
	u32 addr0, addr1, aperture, value;

	/* [한국어] 이미 다른 범위에 배정된 BAR 는 쓸 수 없다.
	 * 공통 코드가 이 표를 보고 고르지만, 경합을 막기 위해 여기서도 확인한다. */
	if (!rc->avail_ib_bar[bar])
		return -ENODEV;

	/* [한국어] 이제 이 BAR 는 쓰였다고 표시한다. */
	rc->avail_ib_bar[bar] = false;

	/* [한국어] 크기를 비트 수로 바꾼다. 크기가 2의 거듭제곱임을
	 * 전제하며, 그 보장은 공통 코드가 한다. */
	aperture = ilog2(size);
	if (bar == RP_NO_BAR) {
		/* [한국어] "BAR 없이 통과" 항목은 크기를 주소 레지스터에 함께
		 * 담는다. 하위 8비트를 잘라 내는 것은 그 자리가 크기 필드이기
		 * 때문이다 — 아웃바운드 창과 같은 방식이다. */
		addr0 = CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR0_NBITS(aperture) |
			(lower_32_bits(cpu_addr) & GENMASK(31, 8));
		addr1 = upper_32_bits(cpu_addr);
	} else {
		/* [한국어] 실제 BAR 는 크기를 아래 BAR_CFG 레지스터에 따로
		 * 적으므로, 주소 레지스터에는 주소만 넣는다. */
		addr0 = lower_32_bits(cpu_addr);
		addr1 = upper_32_bits(cpu_addr);
	}
	/* [한국어] 인바운드 주소를 쓴다. 뱅크가 AXI_MASTER 인 것은 이 방향에서 IP 가
	 * AXI 버스의 마스터로 동작하기 때문이다 — 들어온 PCIe 요청을 AXI 로
	 * 내보내는 쪽이다. 아웃바운드가 AXI_SLAVE 인 것과 대비된다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_MASTER,
			     CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR0(bar), addr0);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_MASTER,
			     CDNS_PCIE_HPA_AT_IB_RP_BAR_ADDR1(bar), addr1);

	/* [한국어] RP_NO_BAR 는 BAR_CFG 레지스터에 자기 자리가 없어 0번
	 * 자리를 빌려 쓴다. 이 재지정이 있어야 아래
	 * bar_aperture_mask[bar] 인덱싱이 배열 범위(RP_BAR0, RP_BAR1) 안에
	 * 머문다 — 그러지 않으면 범위를 벗어난 읽기가 된다. */
	if (bar == RP_NO_BAR)
		bar = (enum cdns_pcie_rp_bar)BAR_0;

	/* [한국어] BAR 설정 레지스터를 읽어 이 BAR 의 필드만 고친다.
	 * 여러 BAR 의 설정이 한 레지스터에 모여 있어 통째로 덮으면 안 된다. */
	value = cdns_pcie_hpa_readl(pcie, REG_BANK_IP_CFG_CTRL_REG, CDNS_PCIE_HPA_LM_RC_BAR_CFG);
	/* [한국어] 네 가지 타입 값과 크기 필드를 모두 지운다.
	 * 타입 상수들이 서로 겹치는 비트를 쓰므로 하나씩 지우는 대신
	 * 전부 OR 해서 한 번에 지운다.
	 * 크기 쪽 마스크에 +7 을 더하는 것은 마스크 전체를 덮는 값을
	 * 만들기 위한 것으로 보이는데, 구형 판이 +2 를 쓰는 것과 다르다.
	 * 두 상수의 근거는 Cadence IP 문서 소관이라 확인하지 못했다. */
	value &= ~(HPA_LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar) |
		   HPA_LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar) |
		   HPA_LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar) |
		   HPA_LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar) |
		   HPA_LM_RC_BAR_CFG_APERTURE(bar, bar_aperture_mask[bar] + 7));
	/* [한국어] 범위의 끝이 4GB 를 넘으면 64비트 BAR 여야 한다.
	 * 32비트 BAR 로는 그 위 주소를 표현할 수 없기 때문이다. */
	if (size + cpu_addr >= SZ_4G) {
		value |= HPA_LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar);
		/* [한국어] prefetchable 이면 비트 3 을 더한다. 위 함수 주석에
		 * 적었듯 신형은 이 구분이 실제로 동작한다(0x5 대 0xD). */
		if ((flags & IORESOURCE_PREFETCH))
			value |= HPA_LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar);
	} else {
		value |= HPA_LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar);
		/* [한국어] 32비트 쪽도 같은 방식(0x1 대 0x9). */
		if ((flags & IORESOURCE_PREFETCH))
			value |= HPA_LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar);
	}

	/* [한국어] 마지막으로 크기를 넣고 한 번에 반영한다. */
	value |= HPA_LM_RC_BAR_CFG_APERTURE(bar, aperture);
	cdns_pcie_hpa_writel(pcie, REG_BANK_IP_CFG_CTRL_REG, CDNS_PCIE_HPA_LM_RC_BAR_CFG, value);

	return 0;
}

/* [한국어]
 * cdns_pcie_hpa_host_init_root_port - 루트 포트 자신의 config 를 채운다
 *
 * @rc: 루트 컴플렉스.
 * @return: 항상 0. 실패할 수 있는 동작이 없다.
 *
 * 루트 포트도 PCI 장치이므로 config 헤더가 있어야 한다. 호스트 쪽
 * 소프트웨어(lspci 등)가 이 값을 읽어 이것이 무엇인지 판단한다.
 *
 * 채우는 것이 셋이다.
 *   BAR 설정 — 루트 포트의 BAR 0, 1 을 꺼 둔다. 루트 포트는 자기
 *     메모리를 노출할 이유가 없기 때문이다. 대신 prefetch 와 I/O
 *     창의 종류만 지정한다.
 *   식별 정보 — 벤더·디바이스 ID 는 디바이스 트리에서 지정했을 때만
 *     덮어쓴다(0xffff 는 "지정 안 함" 을 뜻하는 약속이다).
 *   클래스 코드 — PCI_CLASS_BRIDGE_PCI 로 못 박는다. 이 값이 있어야
 *     커널이 이것을 브리지로 인식해 그 아래를 열거한다.
 *
 * 마지막으로 Command 레지스터의 세 비트를 켠다. Bus Master 가
 * 특히 중요한데, 그것이 없으면 이 포트가 DMA 를 중계하지 못한다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_hpa_host_init() → [이 함수]
 *     → cdns_pcie_hpa_rp_writew/writeb, cdns_pcie_hpa_writel
 */
static int cdns_pcie_hpa_host_init_root_port(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] value 는 레지스터에 쓸 값, ctrl 은 BAR 를 끄는 상수를 담는다. */
	u32 value, ctrl;

	/*
	 * Set the root port BAR configuration register:
	 * - disable both BAR0 and BAR1
	 * - enable Prefetchable Memory Base and Limit registers in type 1
	 *   config space (64 bits)
	 * - enable IO Base and Limit registers in type 1 config
	 *   space (32 bits)
	 */

	/* [한국어] 루트 포트의 BAR 0 과 1 을 꺼 둔다. 루트 포트는 자기
	 * 메모리를 호스트에 노출할 이유가 없기 때문이다.
	 * 위 bar_ib_config 가 쓰는 것과 같은 레지스터인데, 여기서는
	 * 통째로 덮어쓴다 — 초기화 시점이라 보존할 이전 값이 없다. */
	ctrl = CDNS_PCIE_HPA_LM_BAR_CFG_CTRL_DISABLED;
	value = CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR0_CTRL(ctrl) |
		CDNS_PCIE_HPA_LM_RC_BAR_CFG_BAR1_CTRL(ctrl) |
		/* [한국어] BAR 는 껐지만 prefetch 창과 I/O 창의 종류는
		 * 지정해 둔다. 이것은 루트 포트가 아래로 내려보낼 창의
		 * 성질이지 자기 BAR 가 아니다. */
		CDNS_PCIE_HPA_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE |
		CDNS_PCIE_HPA_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS |
		CDNS_PCIE_HPA_LM_RC_BAR_CFG_IO_ENABLE |
		CDNS_PCIE_HPA_LM_RC_BAR_CFG_IO_32BITS;
	cdns_pcie_hpa_writel(pcie, REG_BANK_IP_CFG_CTRL_REG,
			     CDNS_PCIE_HPA_LM_RC_BAR_CFG, value);

	/* [한국어] 0xffff 는 "디바이스 트리가 지정하지 않았다" 는 표시다.
	 * 그때는 IP 의 기본값을 그대로 둔다. */
	if (rc->vendor_id != 0xffff)
		cdns_pcie_hpa_rp_writew(pcie, PCI_VENDOR_ID, rc->vendor_id);

	if (rc->device_id != 0xffff)
		cdns_pcie_hpa_rp_writew(pcie, PCI_DEVICE_ID, rc->device_id);

	/* [한국어] 리비전과 프로그래밍 인터페이스를 0 으로 둔다. */
	cdns_pcie_hpa_rp_writeb(pcie, PCI_CLASS_REVISION, 0);
	cdns_pcie_hpa_rp_writeb(pcie, PCI_CLASS_PROG, 0);
	/* [한국어] 클래스 코드를 PCI-to-PCI 브리지로 못 박는다. 이 값이
	 * 있어야 커널이 이것을 브리지로 보고 그 아래 버스를 열거한다.
	 * 앞서 pci-ep-cfs.c 주석에서 본 것과 같은 원리다 — 클래스 코드가
	 * 상대편에서 어떤 드라이버가 붙을지를 결정한다. */
	cdns_pcie_hpa_rp_writew(pcie, PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_PCI);

	/* Enable bus mastering */
	/* [한국어] Command 레지스터의 세 비트를 켠다.
	 *   MEMORY — 메모리 공간 접근을 받아들인다.
	 *   IO     — I/O 공간 접근을 받아들인다.
	 *   MASTER — 이 포트가 버스 마스터가 되어 DMA 를 중계할 수 있다.
	 * 마지막이 특히 중요하다. 이것이 없으면 아래 장치들의 DMA 가
	 * 호스트 메모리에 닿지 못한다 — 엔드포인트 쪽에서 호스트가
	 * pci_set_master() 를 불러 줘야 하는 것과 같은 이치다.
	 *
	 * 읽어서 OR 하는 이유는 이 레지스터의 다른 비트(오류 보고 설정 등)를
	 * 보존하기 위해서다. */
	value = cdns_pcie_hpa_readl(pcie, REG_BANK_RP, PCI_COMMAND);
	value |= (PCI_COMMAND_MEMORY | PCI_COMMAND_IO | PCI_COMMAND_MASTER);
	cdns_pcie_hpa_writel(pcie, REG_BANK_RP, PCI_COMMAND, value);
	return 0;
}

/* [한국어]
 * cdns_pcie_hpa_create_region_for_cfg - config 전용 아웃바운드 창을 만든다
 *
 * @rc: 루트 컴플렉스.
 * @return: 없음.
 *
 * 아웃바운드 창 0번을 config 접근 전용으로 잡아 둔다. 이후 config
 * 접근이 있을 때마다 cdns_pci_hpa_map_bus() 가 이 창의 목적지만
 * 바꿔 쓴다 — 창 자체를 매번 새로 만들지는 않는다.
 *
 * ECAM 을 지원하는 구성에서는 이 함수를 부르지 않는다. ECAM 은
 * config space 전체가 메모리에 펼쳐져 있어 창을 돌려 쓸 필요가 없고,
 * 그편이 훨씬 빠르다(창을 고치는 MMIO 가 없어진다).
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_hpa_host_init_address_translation() → [이 함수]
 */
static void cdns_pcie_hpa_create_region_for_cfg(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rc);
	/* [한국어] 디바이스 트리에서 얻은 config 영역의 물리 주소 범위. */
	struct resource *cfg_res = rc->cfg_res;
	struct resource_entry *entry;
	u64 cpu_addr = cfg_res->start;
	u32 addr0, addr1, desc1;
	int busnr = 0;

	/* [한국어] 이 브리지가 관리할 버스 번호 범위의 시작을 찾는다.
	 * 없으면 0 으로 두는데, 디바이스 트리에 bus-range 가 없는 경우다. */
	entry = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	if (entry)
		busnr = entry->res->start;

	/* [한국어] 태그 관리 레지스터를 설정한다. PCIe 는 완료를 기다리는
	 * 요청마다 태그를 붙여 구분하는데, 그 관리 방식을 정하는 값으로
	 * 보인다. 0x01000000 이라는 매직 넘버의 의미는 Cadence IP 문서
	 * 소관이라 이 트리에서 확인하지 못했다 — 코드에 있는 그대로 적는다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_TAG_MANAGEMENT, 0x01000000);
	/*
	 * Reserve region 0 for PCI configure space accesses:
	 * OB_REGION_PCI_ADDR0 and OB_REGION_DESC0 are updated dynamically by
	 * cdns_pci_map_bus(), other region registers are set here once for all
	 */
	/* [한국어] 요청자 쪽 버스 번호를 담아 둔다. map_bus 가 나중에
	 * 목적지 devfn 만 고쳐 쓰므로 여기서 기본값을 넣는 셈이다. */
	desc1 = CDNS_PCIE_HPA_AT_OB_REGION_DESC1_BUS(busnr);
	/* [한국어] 목적지 PCI 주소의 상위 워드를 0 으로 지운다.
	 * config TLP 는 주소가 아니라 버스·devfn 으로 대상을 지정하므로
	 * 이 값은 쓰이지 않지만, 남은 값이 오해를 부르지 않게 비운다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_PCI_ADDR1(0), 0x0);
	/* Type-1 CFG */
	/* [한국어] 상류 주석대로 타입 1 config 로 설정한다. map_bus 가
	 * 접근할 때마다 타입 0/1 을 다시 정하므로 여기 값은 초기값이다.
	 * 0x05000000 이라는 매직 넘버를 쓰는데, 같은 파일의 map_bus 는
	 * CDNS_PCIE_HPA_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1 상수를 쓴다 —
	 * 두 값이 같은지는 확인하지 못했다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC0(0), 0x05000000);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_DESC1(0), desc1);

	/* [한국어] CPU 쪽 주소와 창 크기를 설정한다. 12비트(4KB)는
	 * config space 하나의 크기다. map_bus 도 같은 값을 쓴다. */
	addr0 = CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0_NBITS(12) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);
	/* [한국어] CTRL0 로 마무리한다. 이 쓰기로 config 창이 실제로 동작한다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR0(0), addr0);
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CPU_ADDR1(0), addr1);
	/* [한국어] CTRL0 에 0x06000000 을 쓴다. map_bus 가 같은 레지스터에
	 * SUPPLY_BUS | SUPPLY_DEV_FN 을 쓰는 것과 다른 형태의 매직 넘버라,
	 * 두 값의 관계를 이 트리에서 확인하지 못했다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_AXI_SLAVE,
			     CDNS_PCIE_HPA_AT_OB_REGION_CTRL0(0), 0x06000000);
}

/* [한국어] cdns_pcie_hpa_host_init_address_translation - 주소 창을 전부 건다
 * 
 * @rc: 루트 컴플렉스.
 * @return: 0 이면 성공. 인바운드 배정이 실패하면 그 오류.
 * 
 * 아웃바운드와 인바운드를 모두 설정한다.
 *   아웃바운드 — 창 0번은 config 전용으로 잡아 두고(ECAM 이 아닐 때),
 *     그다음 메시지 창, 그리고 디바이스 트리의 자원 창들을 차례로 건다.
 *   인바운드 — dma-ranges 를 공통 코드에 맡겨 BAR 에 배정한다.
 * 
 * 창 번호를 r 로 세어 나가는 방식이라, 앞에서 몇 개를 썼느냐에 따라
 * 뒤 창의 번호가 밀린다. 하드웨어의 창 개수가 유한하므로 자원 창이
 * 많으면 모자랄 수 있는데, 이 함수는 그것을 검사하지 않는다 —
 * 창 설정 함수가 범위를 넘으면 메시지만 남기고 물러난다.
 * 
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 * 
 * 호출 체인:
 *   cdns_pcie_hpa_host_init() → [이 함수]
 *     → cdns_pcie_hpa_create_region_for_cfg()
 *     → cdns_pcie_hpa_set_outbound_region() 계열
 *     → cdns_pcie_host_map_dma_ranges() [pcie-cadence-host-common.c] */
static int cdns_pcie_hpa_host_init_address_translation(struct cdns_pcie_rc *rc)
{
	/* [한국어] 공통 컨트롤러 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] 자원 창 목록을 가진 브리지. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rc);
	/* [한국어] 자원 목록 순회용 커서. */
	struct resource_entry *entry;
	/* [한국어] r 은 다음에 쓸 아웃바운드 창 번호, busnr 은 이 브리지의 시작 버스 번호. */
	int r = 0, busnr = 0;

	/* [한국어] ECAM 을 쓰면 config 창이 필요 없다 — config space 전체가 이미
	 * 메모리에 펼쳐져 있기 때문이다. 그때는 창 0번을 다른 용도로 쓸 수
	 * 있을 텐데, 아래에서 r 을 무조건 1 로 올리므로 실제로는 비워 둔다. */
	if (!rc->ecam_supported)
		cdns_pcie_hpa_create_region_for_cfg(rc);

	/* [한국어] 디바이스 트리의 bus-range 에서 시작 버스 번호를 얻는다. */
	entry = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	/* [한국어] 없으면 0 으로 둔다. */
	if (entry)
		/* [한국어] 이 값이 아웃바운드 창의 requester ID 에 실린다. */
		busnr = entry->res->start;

	/* [한국어] 창 0번을 건너뛴다. config 전용으로 잡아 두었기 때문이며,
	 * ECAM 을 쓰는 경우에도 마찬가지로 건너뛴다. */
	r++;
	/* [한국어] 메시지 자원이 디바이스 트리에 있으면 메시지 TLP 전용 창도 만든다.
	 * INTx 나 전원 관리 이벤트를 보내려면 필요하다. */
	if (pcie->msg_res) {
		/* [한국어] 메시지 창을 설정한다. */
		cdns_pcie_hpa_set_outbound_region_for_normal_msg(pcie, busnr, 0, r,
								 pcie->msg_res->start);

		/* [한국어] 그만큼 창 번호를 밀어 둔다. */
		r++;
	}
	/* [한국어] 디바이스 트리가 정의한 자원 창들을 하나씩 건다. */
	resource_list_for_each_entry(entry, &bridge->windows) {
		/* [한국어] 이 항목의 주소 범위. */
		struct resource *res = entry->res;
		/* [한국어] PCI 쪽 주소를 구한다. offset 은 CPU 주소와 PCI 주소의 차이이므로
		 * 빼면 저쪽에서 보는 주소가 된다. */
		u64 pci_addr = res->start - entry->offset;

		/* [한국어] I/O 공간과 메모리 공간은 TLP 타입이 달라 갈라 부른다. */
		if (resource_type(res) == IORESOURCE_IO)
			/* [한국어] I/O 창. pci_pio_to_address 로 논리 I/O 포트 번호를 실제 물리
			 * 주소로 바꾼다 — 커널이 I/O 공간을 가상화해 다루기 때문이다. */
			cdns_pcie_hpa_set_outbound_region(pcie, busnr, 0, r,
							  true,
							  pci_pio_to_address(res->start),
							  pci_addr,
							  resource_size(res));
		else
			/* [한국어] 메모리 창은 시작 주소를 그대로 쓴다. */
			cdns_pcie_hpa_set_outbound_region(pcie, busnr, 0, r,
							  false,
							  res->start,
							  pci_addr,
							  resource_size(res));

		/* [한국어] 다음 창으로. */
		r++;
	}

	/* [한국어] 일부 구성은 인바운드 매핑을 하지 않는다. 그런 보드는 장치의 DMA 가
	 * 다른 경로로 처리되거나 아예 필요 없는 것으로 보이나, 그 근거를
	 * 이 트리에서 확인하지는 못했다. */
	if (rc->no_inbound_map)
		return 0;
	else
		return cdns_pcie_host_map_dma_ranges(rc, cdns_pcie_hpa_host_bar_ib_config);
}

/* [한국어]
 * cdns_pcie_hpa_host_init - 루트 포트와 주소 변환을 차례로 초기화한다
 *
 * @rc: 루트 컴플렉스.
 * @return: 0 이면 성공. 어느 단계든 실패하면 그 오류.
 *
 * 두 초기화를 묶는 얇은 함수다. 순서가 정해져 있다 — config 헤더를
 * 먼저 채워 이것이 브리지임을 밝힌 뒤에 주소 창을 걸어야, 그 창으로
 * 나가는 요청이 올바른 requester ID 를 갖는다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트.
 *
 * 에러 경로: 앞 단계가 실패하면 뒤를 하지 않는다. 되돌리기는 하지
 *   않는데, 실패하면 컨트롤러 전체가 정리되기 때문이다.
 *
 * 호출 체인:
 *   cdns_pcie_hpa_host_setup() → [이 함수]
 *     → cdns_pcie_hpa_host_init_root_port()
 *     → cdns_pcie_hpa_host_init_address_translation()
 */
static int cdns_pcie_hpa_host_init(struct cdns_pcie_rc *rc)
{
	int err;

	/* [한국어] 먼저 루트 포트의 config 헤더를 채운다. */
	err = cdns_pcie_hpa_host_init_root_port(rc);
	if (err)
		return err;

	/* [한국어] 그다음 주소 창을 건다. */
	return cdns_pcie_hpa_host_init_address_translation(rc);
}

/* [한국어]
 * cdns_pcie_hpa_host_link_setup - 링크를 올리고 올라오기를 기다린다
 *
 * @rc: 루트 컴플렉스.
 * @return: 0 이면 링크가 올라왔다. 시작 실패나 대기 시간 초과 시 음수.
 *
 * 링크를 세우는 진입점이다. 세 단계를 거친다.
 *   보드에 quirk 가 걸려 있으면 Detect.Quiet 최소 대기를 늘린다.
 *   PTM 응답을 켠다.
 *   링크 트레이닝을 시작하고 기다린다.
 *
 * 구형 경로와 다른 점이 여기 있다. 구형(pcie-cadence-host.c:1183)은
 * cdns_pcie_host_start_link() 를 부르는데, 그 함수는 대기 후에
 * Gen2 재트레이닝 quirk 까지 처리한다. 이 신형 경로는 그것을 거치지
 * 않고 cdns_pcie_host_wait_for_link() 를 직접 부르므로, 재트레이닝
 * quirk 를 타지 않는다. 신형 IP 에 그 결함이 없어서인지 단지
 * 구현되지 않은 것인지는 이 트리에서 확인하지 못했다.
 *
 * 또 하나. 여기서 넘기는 것은 cdns_pcie_hpa_link_up() 자체이지
 * ops 를 거치는 디스패처(cdns_pcie_link_up)가 아니다. 그래서
 * SoC 가 ops->link_up 을 채워도 이 경로에서는 쓰이지 않는다 —
 * pci-sky1.c 의 sky1_pcie_link_up() 이 그런 경우다.
 *
 * 링크가 안 올라온 것을 dev_err 이 아니라 dev_dbg 로 남기는 점도
 * 눈여겨볼 만하다. 슬롯이 비어 있으면 당연히 링크가 없으므로
 * 오류로 시끄럽게 알릴 일이 아니라는 판단이다. 다만 반환값은
 * 그대로 오류라, 호출자인 host_setup 은 그 경우 물러난다.
 *
 * 실행 컨텍스트: 호스트 초기화 중 프로세스 컨텍스트. 대기에서 잠든다.
 *
 * 호출 체인:
 *   cdns_pcie_hpa_host_setup() → [이 함수]
 *     → cdns_pcie_start_link() → cdns_pcie_host_wait_for_link()
 */
int cdns_pcie_hpa_host_link_setup(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct device *dev = rc->pcie.dev;
	int ret;

	/* [한국어] 이 보드가 Detect.Quiet 대기를 늘려야 하는 경우에만.
	 * 그 필요는 SoC 드라이버가 판단해 플래그로 알린다. */
	if (rc->quirk_detect_quiet_flag)
		cdns_pcie_hpa_detect_quiet_min_delay_set(&rc->pcie);

	/* [한국어] PTM 응답을 켠다. 링크 트레이닝 전에 해 두어야
	 * 아래 장치가 협상 직후부터 PTM 을 쓸 수 있다. */
	cdns_pcie_hpa_host_enable_ptm_response(pcie);

	/* [한국어] 링크 트레이닝을 시작한다. 이것도 디스패처라
	 * ops->start_link 가 있으면 그것을 쓴다 — sky1 은 실제로
	 * sky1_pcie_start_link() 를 제공한다. */
	ret = cdns_pcie_start_link(pcie);
	if (ret) {
		dev_err(dev, "Failed to start link\n");
		return ret;
	}

	/* [한국어] 링크가 올라오기를 기다린다. 공통 코드의 대기 루프를
	 * 쓰되 상태 판정 방법으로 신형 전용 함수를 직접 넘긴다. */
	ret = cdns_pcie_host_wait_for_link(pcie, cdns_pcie_hpa_link_up);
	if (ret)
		/* [한국어] 슬롯이 비어 있는 정상적인 경우도 여기 오므로
		 * dev_dbg 다. 다만 ret 은 그대로 돌려주므로 호출자는 물러난다. */
		dev_dbg(dev, "PCIe link never came up\n");

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_hpa_host_link_setup);

/* [한국어]
 * cdns_pcie_hpa_host_setup - 신형 IP 호스트의 전체 초기화 진입점
 *
 * @rc: SoC 드라이버가 준비한 루트 컴플렉스.
 * @return: 0 이면 성공. 실패 시 음수.
 *
 * SoC 드라이버(pci-sky1.c 등)가 부르는 함수이며, 여기서부터 PCI 코어의
 * 열거까지 이어진다. 순서가 이렇다.
 *   1) 레지스터와 config 영역을 매핑한다. 이미 SoC 드라이버가 매핑해
 *      두었으면(ECAM 을 쓰는 경우처럼) 건너뛴다 — !pcie->reg_base 와
 *      !rc->cfg_base 검사가 그 뜻이다.
 *   2) EROM BAR 를 0 으로 지운다.
 *   3) 링크를 올린다.
 *   4) 인바운드 BAR 를 전부 "사용 가능" 으로 표시한다.
 *   5) 루트 포트와 주소 변환을 설정한다.
 *   6) config 접근 표를 걸고 PCI 코어에 넘긴다.
 *
 * 3번이 5번보다 먼저인 점이 눈에 띈다. 링크를 올린 뒤에 주소 창을
 * 거는 셈인데, 그 사이에 아래 장치가 요청을 보내면 갈 곳이 없다.
 * 다만 호스트가 config 를 읽어 주기 전에는 장치가 먼저 요청을 내지
 * 않으므로 실제로 문제가 되지는 않는 것으로 보인다 — 그 근거를
 * 이 트리에서 확인하지는 못했다.
 *
 * 실행 컨텍스트: SoC 드라이버의 probe — 프로세스 컨텍스트.
 *   링크 대기와 열거로 잠든다.
 *
 * 에러 경로: 어느 단계든 실패하면 곧바로 물러난다. 매핑은 devm 이라
 *   자동 해제되고, 나머지는 컨트롤러가 정리될 때 함께 사라진다.
 *
 * 호출 체인:
 *   SoC 드라이버 probe → [이 함수]
 *     → cdns_pcie_hpa_host_link_setup() → cdns_pcie_hpa_host_init()
 *     → pci_host_probe() [probe.c] → PCI 코어 열거
 */
int cdns_pcie_hpa_host_setup(struct cdns_pcie_rc *rc)
{
	struct device *dev = rc->pcie.dev;
	/* [한국어] 자원 조회를 위해 플랫폼 장치로 되돌린다. */
	struct platform_device *pdev = to_platform_device(dev);
	struct pci_host_bridge *bridge;
	enum   cdns_pcie_rp_bar bar;
	/* [한국어] 공통 컨트롤러 구조체. 아래에서 rc 안의 것을 가리키게 한다. */
	struct cdns_pcie *pcie;
	struct resource *res;
	int    ret;

	/* [한국어] rc 가 브리지의 private 영역에 있으므로 거꾸로 찾는다. */
	bridge = pci_host_bridge_from_priv(rc);
	if (!bridge)
		return -ENOMEM;

	pcie = &rc->pcie;
	/* [한국어] 이 IP 를 호스트로 쓴다고 표시한다. 아웃바운드 창 설정이
	 * 이 값을 보고 requester ID 를 소프트웨어가 채울지 정한다
	 * (pcie-cadence-hpa.c 의 is_rc 분기). */
	pcie->is_rc = true;

	/* [한국어] SoC 드라이버가 이미 매핑해 두었으면 건너뛴다.
	 * sky1 처럼 자기 방식으로 매핑하는 드라이버가 있기 때문이다. */
	if (!pcie->reg_base) {
		pcie->reg_base = devm_platform_ioremap_resource_byname(pdev, "reg");
		if (IS_ERR(pcie->reg_base)) {
			/* [한국어] 디바이스 트리에 reg 항목이 없으면 레지스터에 접근할 수 없다. */
			dev_err(dev, "missing \"reg\"\n");
			/* [한국어] 매핑 실패를 그대로 전한다. */
			return PTR_ERR(pcie->reg_base);
		}
	}

	/* ECAM config space is remapped at glue layer */
	/* [한국어] 상류 주석대로 ECAM 을 쓰는 구성에서는 SoC 드라이버가
	 * 이미 config 공간을 매핑해 두었으므로 건너뛴다.
	 * devm_pci_remap_cfg_resource 는 일반 ioremap 과 달리 config
	 * 접근에 맞는 캐시 속성으로 매핑해 준다. */
	if (!rc->cfg_base) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
		rc->cfg_base = devm_pci_remap_cfg_resource(dev, res);
		if (IS_ERR(rc->cfg_base))
			return PTR_ERR(rc->cfg_base);
		/* [한국어] 자원 자체도 보관한다. create_region_for_cfg 가
		 * 그 시작 주소로 config 창을 만든다. */
		rc->cfg_res = res;
	}

	/* Put EROM Bar aperture to 0 */
	/* [한국어] Expansion ROM BAR 를 꺼 둔다. 루트 포트가 옵션 ROM 을
	 * 노출할 이유가 없기 때문이다. 남겨 두면 호스트가 그것을 읽으려
	 * 시도할 수 있다. */
	cdns_pcie_hpa_writel(pcie, REG_BANK_IP_CFG_CTRL_REG, CDNS_PCIE_EROM, 0x0);

	/* [한국어] 링크를 올린다. 실패하면 그 아래에 아무것도 없으므로
	 * 더 진행할 이유가 없다. */
	ret = cdns_pcie_hpa_host_link_setup(rc);
	if (ret)
		return ret;

	/* [한국어] 인바운드 BAR 를 전부 비어 있음으로 표시한다.
	 * 아래 host_init 이 dma-ranges 를 배정하며 이 표를 소비한다.
	 * RP_NO_BAR 까지 포함하는 점에 주의 — 그것도 배정 가능한 자리다. */
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++)
		rc->avail_ib_bar[bar] = true;

	/* [한국어] 루트 포트 config 와 주소 변환을 설정한다. */
	ret = cdns_pcie_hpa_host_init(rc);
	if (ret)
		return ret;

	/* [한국어] SoC 드라이버가 자기 ops 를 걸어 두었으면 그것을 존중한다.
	 * pcie-sg2042.c 처럼 config 접근 폭 제약이 있는 SoC 가 그렇게 한다.
	 * 없을 때만 이 파일의 기본 표를 건다. */
	if (!bridge->ops)
		bridge->ops = &cdns_pcie_hpa_host_ops;

	/* [한국어] PCI 코어에 넘긴다. 이 안에서 버스가 만들어지고 열거가
	 * 시작되어, 발견된 장치들에 드라이버가 붙는다. */
	return pci_host_probe(bridge);
}
EXPORT_SYMBOL_GPL(cdns_pcie_hpa_host_setup);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe host controller driver");
