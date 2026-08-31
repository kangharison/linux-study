// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe host controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

/*
 * [한국어 설명] Cadence PCIe IP 의 호스트(루트 컴플렉스) 구현 — 구형 레지스터
 * 배치 (pcie-cadence-host.c)
 *
 * === 파일의 역할 ===
 * Cadence PCIe 컨트롤러 IP 를 루트 컴플렉스, 즉 PCI 호스트 브리지로
 * 동작시키는 코드다. 같은 IP 를 엔드포인트로 쓰는 코드는 옆 파일
 * pcie-cadence-ep.c 에 있고, 이 파일은 그 반대편이다.
 *
 * 하는 일은 크게 셋이다. 첫째, config space 접근 경로를 만든다. 이 IP 는
 * ECAM(config space 전체를 메모리에 통째로 펼치는 방식)을 제공하지 않는다.
 * 대신 아웃바운드 주소 변환 창 하나(0번)를 config 전용으로 예약해 두고,
 * 접근할 때마다 그 창의 목적지 BDF 를 다시 써 넣는다. 그래서
 * cdns_pci_map_bus() 가 단순한 주소 계산이 아니라 레지스터 쓰기를 한다.
 *
 * 둘째, 루트 포트 자신의 config space 를 채운다. 클래스 코드를 PCI-to-PCI
 * 브리지로 세워야 PCI 코어가 그 아래를 열거하고, vendor/device ID 와 ASPM
 * quirk 도 여기서 정한다.
 *
 * 셋째, 주소 변환 창을 연다. 아웃바운드(CPU → PCIe)는 디바이스 트리의
 * ranges 에서 온 bridge->windows 목록을, 인바운드(PCIe → 시스템 메모리)는
 * dma-ranges 를 각각 창과 BAR 에 대응시킨다.
 *
 * 신형 레지스터 배치를 쓰는 판본은 pcie-cadence-host-hpa.c 에 따로 있고,
 * 두 벌이 공유하는 링크 대기와 BAR 배정 알고리즘은
 * pcie-cadence-host-common.c 로 빠져 있다. 이 파일은 그 공통 코드에
 * "레지스터를 이렇게 만져라" 를 함수 포인터로 넘겨 주는 쪽이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SoC 별 드라이버의 probe (pci-j721e.c / pcie-sg2042.c / pcie-cadence-plat.c)
 *   -> cdns_pcie_init_phy()                [pcie-cadence.c] PHY 준비
 *   -> cdns_pcie_host_setup()              [이 파일] 진입점
 *        -> cdns_pcie_host_link_setup()    [이 파일] 링크를 올린다
 *             -> cdns_pcie_host_start_link()  [pcie-cadence-host-common.c]
 *        -> cdns_pcie_host_init()          [이 파일]
 *             -> cdns_pcie_host_init_root_port()
 *             -> cdns_pcie_host_init_address_translation()
 *                  -> cdns_pcie_set_outbound_region()  [pcie-cadence.c]
 *                  -> cdns_pcie_host_map_dma_ranges()  [공통 코드]
 *                       -> cdns_pcie_host_bar_ib_config()  [이 파일, 콜백]
 *        -> pci_host_probe()               [PCI 코어] 버스 열거 시작
 *
 * 그 뒤로는 PCI 코어가 주도한다. 코어가 config 를 읽을 때마다
 * cdns_pcie_host_ops.map_bus (= cdns_pci_map_bus)가 불린다.
 *
 * 실행 컨텍스트가 두 갈래로 갈린다. setup/init/disable 계열은 probe,
 * remove, resume 시점의 프로세스 컨텍스트라 잠들어도 된다. 반면
 * cdns_pci_map_bus() 는 PCI 코어가 pci_lock(raw spinlock, 인터럽트 차단)을
 * 쥔 채로 부르므로 잠들면 안 된다 — drivers/pci/access.c 의 PCI_OP_READ /
 * PCI_OP_WRITE 매크로가 pci_lock_config() 안에서 bus->ops->read 를 부른다.
 * 그 덕에 아웃바운드 0번 창을 접근마다 고쳐 쓰는 방식이 성립한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: SoC 별 드라이버들(pci-j721e.c, pcie-sg2042.c, pcie-cadence-plat.c)이
 *   이 파일이 내보낸 cdns_pcie_host_setup() / _link_setup() / _init() /
 *   _disable() 와 cdns_pci_map_bus() 를 부른다. j721e 는 절전 복귀 경로에서
 *   setup 전체가 아니라 link_setup + init 만 다시 부른다.
 * 아래쪽: pcie-cadence.c 의 아웃바운드 창 설정과 링크 시작/정지,
 *   pcie-cadence-host-common.c 의 링크 대기와 dma-ranges 배정 알고리즘,
 *   그리고 PCI 코어(pci_host_probe, pci_generic_config_read 계열,
 *   pci_stop_root_bus / pci_remove_root_bus).
 * 공유 상태: struct cdns_pcie_rc 가 중심이다. 이것은 struct pci_host_bridge 의
 *   private 영역 안에 들어 있어, pci_host_bridge_priv() 와
 *   pci_host_bridge_from_priv() 로 양방향으로 오간다. 그 안의
 *   avail_ib_bar[] 배열이 인바운드 BAR 점유 상태이고, cfg_base / cfg_res 가
 *   config 접근 창의 CPU 쪽 정보다.
 * 데이터 흐름: 디바이스 트리(ranges / dma-ranges / bus-range / vendor-id /
 *   device-id) -> PCI 코어가 파싱해 bridge->windows 와 bridge->dma_ranges 로
 *   -> 이 파일이 그것을 아웃바운드 창과 인바운드 BAR 레지스터로 옮긴다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 에서 cdns_ 로 시작하는 심볼을 부르는 곳은 없다(전수 확인).
 * 관계는 코드 호출이 아니라 토폴로지상의 것이다 — 이 파일이 호스트 브리지를
 * 만들면 PCI 코어가 그 아래를 열거하고, 그 결과로 NVMe 드라이버의 probe 가
 * 불린다.
 *
 * 그래도 NVMe 를 공부하는 독자에게 의미 있는 접점이 둘 있다.
 * 하나는 인바운드 창이다. cdns_pcie_host_bar_ib_config() 가 여는 범위가 곧
 * 아래쪽 장치의 DMA 가 닿을 수 있는 시스템 메모리 범위다. NVMe 컨트롤러가
 * PRP 나 SGL 에 실어 보내는 주소가 이 범위 밖이면 그 메모리 쓰기 TLP 는
 * 브리지에서 변환되지 못한다.
 * 다른 하나는 아웃바운드 창이다. CPU 가 NVMe 컨트롤러의 BAR0(CAP/CC 같은
 * 레지스터와 doorbell)에 접근하는 통로가 여기서 열린 메모리 창이다.
 *
 * === 주요 함수/구조체 요약 ===
 * bar_aperture_mask[]  : RC BAR 의 aperture 필드 폭 표. 필드를 지울 마스크로만 쓴다.
 * cdns_pci_map_bus()   : config 접근마다 아웃바운드 0번 창을 대상 BDF 로
 *                        다시 조준하고, 접근할 가상 주소를 돌려준다.
 * cdns_pcie_host_ops   : PCI 코어에 넘기는 config 접근 방법 표.
 * cdns_pcie_host_init_root_port() / _deinit_root_port() : 루트 포트 자신의
 *                        config space 를 채우고 되돌린다.
 * cdns_pcie_host_bar_ib_config() : 인바운드 BAR 하나를 설정한다. 공통 코드가
 *                        함수 포인터로 부르는 콜백이다.
 * cdns_pcie_host_init_address_translation() / _deinit_... : 아웃바운드·인바운드
 *                        창 전체를 열고 닫는다.
 * cdns_pcie_host_link_setup() / _link_disable() : PTM 을 켜고 링크를 올린다.
 * cdns_pcie_host_setup() : SoC 드라이버가 부르는 진입점.
 * cdns_pcie_host_disable() : remove 경로의 역순 정리.
 */

/* [한국어] linux/delay.h — msleep/udelay 계열 선언을 얻으려는 include.
 * 다만 이 파일 안에서 그 심볼을 직접 쓰는 곳은 없다(전수 확인).
 * 링크 대기 루프가 pcie-cadence-host-common.c 로 분리되면서 남은 것으로 보인다. */
#include <linux/delay.h>
/* [한국어] linux/kernel.h — ilog2(), upper_32_bits()/lower_32_bits() 같은 기본 헬퍼.
 * 인바운드 BAR 크기를 비트 수로 바꾸고(ilog2) 64비트 주소를 두 워드로
 * 쪼개는 계산에 필요하다. */
#include <linux/kernel.h>
/* [한국어] linux/module.h — EXPORT_SYMBOL_GPL 과 MODULE_LICENSE/DESCRIPTION/AUTHOR.
 * 이 파일은 독립 모듈로 빌드되고, SoC 별 드라이버(pci-j721e, pcie-sg2042,
 * pcie-cadence-plat)가 여기서 내보낸 심볼을 링크해 쓴다. */
#include <linux/module.h>
/* [한국어] linux/list_sort.h — list_sort() 선언. dma-ranges 를 크기 내림차순으로
 * 정렬하는 데 쓰이지만, 그 코드는 pcie-cadence-host-common.c 로 옮겨 갔고
 * 이 파일에는 list_sort 호출이 남아 있지 않다(전수 확인). */
#include <linux/list_sort.h>
/* [한국어] linux/of_address.h — 디바이스 트리 주소 변환 헬퍼. 이 파일에는 of_address
 * 계열 호출이 남아 있지 않다(전수 확인). 역시 공통 코드 분리의 잔재로 보인다. */
#include <linux/of_address.h>
/* [한국어] linux/of_pci.h — of_pci_ 계열(버스 번호/장치 노드 파싱) 선언.
 * 이 파일에서 of_pci_ 로 시작하는 호출은 확인되지 않는다. */
#include <linux/of_pci.h>
/* [한국어] linux/platform_device.h — to_platform_device(),
 * platform_get_resource_byname(), devm_platform_ioremap_resource_byname().
 * cdns_pcie_host_setup() 이 디바이스 트리가 준 reg/cfg 두 자원을 꺼내는 데
 * 반드시 필요한 헤더다. */
#include <linux/platform_device.h>

/* [한국어] 이 IP 공통 헤더. struct cdns_pcie / cdns_pcie_rc 정의, CDNS_PCIE_* 레지스터
 * 상수(pcie-cadence-lga-regs.h 를 통해), cdns_pcie_readl/writel 과
 * cdns_pcie_rp_readl/writel 같은 접근자, 그리고 아웃바운드 창 설정 함수
 * cdns_pcie_set_outbound_region() 의 선언이 모두 여기서 온다. */
#include "pcie-cadence.h"
/* [한국어] 구형(이 파일)과 신형(pcie-cadence-host-hpa.c) 호스트가 공유하는 선언.
 * cdns_pcie_host_start_link() 와 cdns_pcie_host_map_dma_ranges(), 그리고
 * 레지스터 접근 부분만 갈라내기 위한 함수 포인터 타입
 * cdns_pcie_host_bar_ib_cfg / cdns_pcie_linkup_func 가 들어 있다. */
#include "pcie-cadence-host-common.h"

/* [한국어] bar_aperture_mask - 루트 컴플렉스 BAR 의 aperture(크기) 필드 폭 표
 * 
 * 역할: RC_BAR_CFG 레지스터 안에서 각 BAR 의 aperture 필드가 몇 비트인지를
 *   담는다. 실제 쓰임은 하나뿐이다 — 그 필드를 통째로 0 으로 지울 마스크를
 *   만드는 것. LM_RC_BAR_CFG_APERTURE(bar, a) 매크로가 (a - 2) 를 넣으므로,
 *   여기에 +2 한 값을 넘기면 뺄셈이 상쇄되어 마스크 값 자체가 그 자리에 놓인다.
 * 설정자: 컴파일 시점 고정. 런타임에 바뀌지 않는다.
 * 읽는 자: cdns_pcie_host_bar_ib_config() 과 cdns_pcie_host_unmap_dma_ranges().
 *   둘 다 RC_BAR_CFG 의 해당 BAR 필드를 지우는 용도로만 읽는다.
 * 값 범위: 인덱스는 enum cdns_pcie_rp_bar 의 RP_BAR0(0), RP_BAR1(1) 뿐이다.
 *   RP_NO_BAR(2) 자리는 없으므로 두 읽는 자 모두 그 앞에서 RP_NO_BAR 를
 *   걸러 낸 뒤에만 이 배열을 만진다 — 배열 범위를 넘지 않게 하는 장치다.
 * 동기화: 읽기 전용 상수이므로 락이 필요 없다. */
static u8 bar_aperture_mask[] = {
	/* [한국어] RP_BAR0 의 aperture 필드는 5비트(0x1F). 헤더의
	 * CDNS_PCIE_LM_RC_BAR_CFG_BAR0_APERTURE_MASK 는 GENMASK(5, 0) 로 6비트지만,
	 * 이 파일은 그 매크로를 쓰지 않고 이 표를 쓴다. */
	[RP_BAR0] = 0x1F,
	/* [한국어] RP_BAR1 의 aperture 필드는 4비트(0xF).
	 * LM_RC_BAR_CFG_APERTURE(1, 0xF + 2) 는 0xF << 8 이 되어 비트 [11:8] 을 지운다. */
	[RP_BAR1] = 0xF,
};

/* [한국어]
 * cdns_pci_map_bus - config space 접근 주소를 만든다 (pci_ops.map_bus 콜백)
 *
 * @bus: 접근 대상이 붙어 있는 PCI 버스. PCI 코어가 열거 중에 넘긴다.
 * @devfn: 대상의 장치·함수 번호(상위 5비트 장치, 하위 3비트 함수).
 * @where: 대상 config space 안의 바이트 오프셋.
 * @return: 호출자가 readb/readw/readl 또는 writeb 계열을 수행할 가상 주소.
 *          대상이 존재할 수 없거나 링크가 내려가 있으면 NULL. NULL 을 받으면
 *          pci_generic_config_read() 가 all-1(0xffffffff)을 돌려주고, PCI 코어는
 *          그것을 "장치 없음" 으로 해석해 열거를 건너뛴다.
 *
 * 이 파일에서 가장 중요한 함수다. ECAM 을 제공하는 브리지라면 map_bus 는
 * base + (bus << 20) + (devfn << 12) + where 같은 단순 계산으로 끝난다.
 * 그런데 이 IP 에는 그런 통짜 창이 없다. 아웃바운드 주소 변환 창 하나를
 * config 전용으로 예약해 두고(0번), 접근할 때마다 그 창이 겨냥할 버스·장치·
 * 함수 번호를 레지스터에 다시 써 넣는 방식이다. 그래서 이 함수는 주소를
 * 계산하는 함수가 아니라 하드웨어를 조준하는 함수에 가깝다.
 *
 * 동작 단계:
 *   1. 루트 버스에 대한 접근이면 루트 포트 자신을 보는 것이므로,
 *      링크 너머로 나갈 필요 없이 레지스터 창 선두를 그대로 돌려준다.
 *   2. 링크가 내려가 있으면 NULL — TLP 를 내보내 봐야 완료되지 않는다.
 *   3. AXI link-down 래치를 지운다.
 *   4. 아웃바운드 0번 창의 PCI 쪽 주소(PCI_ADDR0)에 대상 버스·devfn 을 쓴다.
 *   5. 대상이 루트 포트 바로 아래 버스면 Type 0, 더 깊으면 Type 1 config 로
 *      서술자(DESC0)를 설정한다.
 *   6. config 창의 CPU 쪽 가상 주소 + 오프셋을 돌려준다.
 *
 * 실행 컨텍스트: PCI 코어가 pci_lock(raw spinlock, 인터럽트 차단)을 쥔 채로
 *   부른다. 잠들면 안 된다. 동시에 두 config 접근이 겹치면 0번 창의 조준이
 *   서로 덮어써져 엉뚱한 장치를 읽게 되는데, 이 전역 락이 그것을 막는다 —
 *   이 방식이 성립하는 전제가 바로 그 직렬화다.
 *
 * 에러 경로: 오류 코드를 돌려줄 자리가 없다. NULL 이 유일한 실패 표현이고,
 *   그 의미는 "장치 없음" 으로 흡수된다.
 *
 * 호출 체인:
 *   PCI 코어의 pci_bus_read_config_dword() 등
 *     -> pci_generic_config_read() [drivers/pci/access.c]
 *       -> bus->ops->map_bus = [이 함수]
 *         -> cdns_pcie_readl() / cdns_pcie_writel() [pcie-cadence.h]
 */
void __iomem *cdns_pci_map_bus(struct pci_bus *bus, unsigned int devfn,
			       int where)
{
	/* [한국어] PCI 코어가 넘겨 준 버스에서 이 버스를 소유한 호스트 브리지를 거슬러 찾는다.
	 * map_bus 콜백은 bus 만 받으므로 드라이버 상태에 닿으려면 이 단계가 필요하다. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	/* [한국어] 호스트 브리지의 private 영역(pci_alloc_host_bridge 가 뒤에 붙여 준 공간)에
	 * 이 드라이버의 struct cdns_pcie_rc 가 통째로 들어 있다.
	 * cdns_pcie_host_setup() 쪽에서는 반대 방향 매크로 pci_host_bridge_from_priv()
	 * 를 쓴다 — 같은 메모리를 양방향으로 오가는 관례다. */
	struct cdns_pcie_rc *rc = pci_host_bridge_priv(bridge);
	/* [한국어] rc 안에 첫 멤버로 박혀 있는 공통 컨트롤러 구조체. 레지스터 베이스
	 * (reg_base)와 RC/EP 모드 플래그가 여기 들어 있다. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] 접근하려는 대상의 PCI 버스 번호. 아래에서 (1) 루트 버스인지 (2) 루트 포트
	 * 바로 아래 버스인지에 따라 config 트랜잭션 종류가 갈리는 판단에 쓰인다. */
	unsigned int busn = bus->number;
	/* [한국어] 아웃바운드 영역 0번에 다시 써 넣을 두 레지스터 값.
	 * addr0 은 목적지 BDF 와 창 크기, desc0 은 TLP 종류와 requester ID 다. */
	u32 addr0, desc0;

	/* [한국어] 루트 버스(브리지 자신이 만드는 최상위 버스)에 대한 접근인지 본다.
	 * 루트 버스에는 이 컨트롤러의 루트 포트 하나만 붙어 있으므로,
	 * 아래 config TLP 를 내보내는 경로를 아예 타지 않고 로컬 레지스터로 답한다. */
	if (pci_is_root_bus(bus)) {
		/*
		 * Only the root port (devfn == 0) is connected to this bus.
		 * All other PCI devices are behind some bridge hence on another
		 * bus.
		 */
		/* [한국어] 루트 버스에서 devfn 이 0 이 아니면 존재하지 않는 장치다(상류 주석 참고).
		 * NULL 을 돌려주면 pci_generic_config_read 가 all-1(0xffffffff)로 처리해
		 * 장치 없음으로 판정된다 — PCI 열거의 표준 규약이다. */
		if (devfn)
			return NULL;

		/* [한국어] 루트 포트 자신의 config space 는 레지스터 창 선두에 노출되어 있다.
		 * 0xfff 마스크는 config space 한 함수분(4KB)으로 오프셋을 자르는 것이다.
		 * 신형 배치 판 cdns_pci_hpa_map_bus() 도 같은 식으로 reg_base 를 쓴다.
		 * 다만 이 파일의 다른 곳에서 루트 포트 레지스터를 만질 때 쓰는
		 * cdns_pcie_rp_readl/writel 계열은 CDNS_PCIE_RP_BASE(0x00200000) 를 더한다.
		 * 두 별칭의 관계는 Cadence IP 문서 소관이라 이 트리에서는 확인할 수 없다. */
		return pcie->reg_base + (where & 0xfff);
	}
	/* Check that the link is up */
	/* [한국어] 여기부터는 링크 너머로 실제 config TLP 를 내보내는 경로다.
	 * 링크가 내려가 있으면 TLP 가 완료되지 않고 AXI 쪽에서 오류가 나므로 먼저 막는다.
	 * Local Management 블록의 첫 레지스터(CDNS_PCIE_LM_BASE) 비트 0 이 링크 상태이며,
	 * 이것은 pcie-cadence.c 의 cdns_pcie_linkup() 이 보는 비트와 같다. */
	if (!(cdns_pcie_readl(pcie, CDNS_PCIE_LM_BASE) & 0x1))
		return NULL;
	/* Clear AXI link-down status */
	/* [한국어] AXI link-down 상태 레지스터에 0 을 써서 래치된 표시를 지운다
	 * (상류 주석 "Clear AXI link-down status").
	 * config 접근을 시작하기 전에 매번 지워 두는 구조다.
	 * 이 레지스터의 비트별 정의는 Cadence IP 문서에 있고 이 트리에서는 확인할 수 없다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_LINKDOWN, 0x0);

	/* Update Output registers for AXI region 0. */
	/* [한국어] 아웃바운드 영역 0번의 PCIe 쪽 목적지 주소를 새로 만든다.
	 * config 요청 TLP 에서는 주소 필드가 곧 대상의 BDF + 레지스터 오프셋이므로,
	 * 접근할 장치가 바뀔 때마다 이 값을 다시 써 넣어야 한다.
	 * NBITS(12) 는 창 크기를 2^12 = 4KB 로 지정한다 — 한 함수의 config space 크기다.
	 * DEVFN(devfn) 은 비트 [19:12], BUS(busn) 은 비트 [27:20] 에 놓여,
	 * config 주소 형식(버스/장치/함수/오프셋)과 자리가 맞는다. */
	addr0 = CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(12) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS(busn);
	/* [한국어] 영역 0번의 PCI_ADDR0 에 반영한다. 이 쓰기 하나로 창의 목적지가 바뀐다.
	 * 영역 0번은 cdns_pcie_host_init_address_translation() 이 config 전용으로
	 * 예약해 둔 창이며, CPU 쪽 주소와 버스 번호(DESC1)는 그때 한 번만 설정된다.
	 * 
	 * 중요한 귀결: 창 하나를 접근마다 고쳐 쓰므로 두 config 접근이 겹치면 안 된다.
	 * 실제로 PCI 코어가 pci_lock(raw spinlock, IRQ off)을 쥐고 read/write 콜백을
	 * 부르므로 직렬화가 보장된다 — drivers/pci/access.c 의 PCI_OP_READ/WRITE 참고. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(0), addr0);

	/* Configuration Type 0 or Type 1 access. */
	/* [한국어] 나가는 config TLP 의 서술자. HARDCODED_RID(비트 23)는 RC 모드에서 필수이며,
	 * 이 비트를 세우면 requester ID(버스/장치/함수)를 소프트웨어가 직접 채워야 한다.
	 * DEVFN(0) 은 "요청자는 루트 포트, 장치 0 함수 0" 이라는 뜻이다.
	 * RC 모드에서 루트 포트의 장치/함수 번호는 항상 0 이다. */
	desc0 = CDNS_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID |
		CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(0);
	/*
	 * The bus number was already set once for all in desc1 by
	 * cdns_pcie_host_init_address_translation().
	 */
	/* [한국어] Type 0 과 Type 1 을 가르는 기준이다.
	 * bridge->busnr 는 루트 버스 번호이므로 그 +1 은 루트 포트 바로 아래(세컨더리)
	 * 버스다. 그 버스의 장치는 루트 포트가 직접 상대하므로 Type 0 config 를 쓴다.
	 * 더 아래 버스라면 중간 브리지가 한 번 더 중계해야 하므로 Type 1 을 쓴다
	 * (PCIe r6.0 sec 2.2.7 의 Configuration Request 규정). */
	if (busn == bridge->busnr + 1)
		/* [한국어] 세컨더리 버스 → Type 0. 수신 장치가 자기 config space 로 바로 받는다. */
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0;
	else
		/* [한국어] 그보다 깊은 버스 → Type 1. 아래쪽 브리지가 자기 세컨더리 버스 번호와
		 * 대조해 Type 0 으로 바꿔 전달하거나 그대로 더 내려보낸다. */
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1;
	/* [한국어] 서술자를 반영한다. addr0(목적지)과 desc0(TLP 종류) 두 쓰기로
	 * 영역 0번이 이번 접근 전용으로 다시 조준된 상태가 된다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(0), desc0);

	/* [한국어] CPU 쪽 창의 가상 주소를 돌려준다. cfg_base 는 cdns_pcie_host_setup() 이
	 * 디바이스 트리의 "cfg" 자원을 ioremap 한 것이고, 영역 0번의 CPU 주소가
	 * 바로 그 자원의 시작이다. 여기 4KB 안의 오프셋으로 읽고 쓰면 컨트롤러가
	 * 위에서 조준해 둔 BDF 로 config TLP 를 내보낸다.
	 * 실제 readl/writel 은 호출자인 pci_generic_config_read/write 가 수행한다. */
	return rc->cfg_base + (where & 0xfff);
}
/* [한국어] SoC 별 드라이버가 자기 pci_ops 에 이 함수를 그대로 꽂아 쓸 수 있도록 내보낸다.
 * 실제 사용처: pci-j721e.c 의 j721e_pcie_ops 와 pcie-sg2042.c 의 ops 표. */
EXPORT_SYMBOL_GPL(cdns_pci_map_bus);

/* [한국어] cdns_pcie_host_ops - PCI 코어에 넘기는 config space 접근 방법 표
 * 
 * 역할: PCI 코어는 struct pci_ops 를 통해서만 config 를 읽고 쓴다.
 *   이 표가 이 컨트롤러용 구현을 코어에 알려 준다.
 * 설정자: 컴파일 시점 고정.
 * 읽는 자: cdns_pcie_host_setup() 이 bridge->ops 가 비어 있을 때만 꽂는다.
 *   SoC 별 드라이버가 자기 표를 먼저 넣어 두었다면 그쪽이 우선한다
 *   (예: pci-j721e.c 는 cdns_ti_pcie_config_read/write 를 넣는데, 루트 버스에
 *   대해서만 pci_generic_config_read32 를 써서 4바이트 단위로 접근한다).
 * 값 범위: 세 콜백 모두 NULL 이 아니다.
 * 동기화: PCI 코어가 pci_lock 을 쥐고 콜백을 부른다. */
static struct pci_ops cdns_pcie_host_ops = {
	/* [한국어] 주소 계산 콜백. 이 IP 는 ECAM 이 아니어서 단순 계산이 아니라
	 * 레지스터를 다시 써서 창을 조준하는 일까지 여기서 한다. */
	.map_bus	= cdns_pci_map_bus,
	/* [한국어] 읽기는 PCI 코어의 표준 구현을 그대로 쓴다. map_bus 가 돌려준 주소에
	 * readb/readw/readl 을 하는 것이 전부다 — drivers/pci/access.c. */
	.read		= pci_generic_config_read,
	/* [한국어] 쓰기도 표준 구현. map_bus 가 창을 조준해 주므로 별도 처리가 필요 없다. */
	.write		= pci_generic_config_write,
};

/* [한국어]
 * cdns_pcie_host_disable_ptm_response - PTM 응답자 기능을 끈다
 *
 * @pcie: 대상 컨트롤러(루트 컴플렉스 모드).
 * @return: 없음.
 *
 * PTM(Precision Time Measurement, PCIe r6.0 sec 6.22)은 링크 양단이 서로의
 * 시각을 맞추는 기능이다. 루트 컴플렉스 쪽은 아래쪽 장치가 보내는 PTM
 * Request 에 답하는 응답자 역할을 맡는데, 그 기능을 켜고 끄는 비트가
 * PTM_CTRL 레지스터의 PTMRSEN(비트 17)이다.
 *
 * 링크를 내리는 길에서 불린다. 링크가 없어질 마당에 응답자를 켜 둘 이유가
 * 없고, 대칭적으로 cdns_pcie_host_link_setup() 이 켠 것을 되돌리는 것이다.
 *
 * 한 비트만 바꿔야 하므로 읽고-고쳐-쓰기를 한다. 이 레지스터를 동시에
 * 만지는 다른 경로는 이 파일에 없으며, 호출 시점이 probe/remove 의
 * 프로세스 컨텍스트라 별도 락을 쓰지 않는다.
 *
 * 호출 체인:
 *   cdns_pcie_host_disable() -> cdns_pcie_host_link_disable() -> [이 함수]
 *     -> cdns_pcie_readl() / cdns_pcie_writel()
 */
static void cdns_pcie_host_disable_ptm_response(struct cdns_pcie *pcie)
{
	/* [한국어] PTM_CTRL 레지스터의 현재 값을 담을 임시 변수.
	 * 한 비트만 바꾸는 read-modify-write 이므로 나머지 비트를 보존해야 한다. */
	u32 val;

	/* [한국어] Local Management 블록의 PTM Control 레지스터를 읽는다. */
	val = cdns_pcie_readl(pcie, CDNS_PCIE_LM_PTM_CTRL);
	/* [한국어] PTMRSEN(PTM Responder Enable, 비트 17)만 0 으로 만들어 되쓴다.
	 * PTM 은 Precision Time Measurement(PCIe r6.0 sec 6.22)로, 링크 양단의 시각을
	 * 맞추는 기능이다. 링크를 내리는 길이므로 응답자 기능을 먼저 끈다.
	 * 참고: 레지스터 매크로는 CDNS_PCIE_LM_PTM_CTRL 인데 비트 매크로만
	 * CDNS_PCIE_LM_TPM_CTRL_PTMRSEN 으로 철자가 다르다(상류 표기 그대로). */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_PTM_CTRL, val & ~CDNS_PCIE_LM_TPM_CTRL_PTMRSEN);
}

/* [한국어]
 * cdns_pcie_host_enable_ptm_response - PTM 응답자 기능을 켠다
 *
 * @pcie: 대상 컨트롤러(루트 컴플렉스 모드).
 * @return: 없음.
 *
 * 위 disable 쪽과 짝을 이루는 함수다. PTM_CTRL 의 PTMRSEN(비트 17)을 세워,
 * 아래쪽 장치가 보내는 PTM Request 에 이 루트 컴플렉스가 응답할 수 있게 한다.
 *
 * 링크를 올리기 "직전" 에 불린다는 순서가 중요하다. 링크가 올라오자마자
 * 아래쪽 장치가 PTM 협상을 시작할 수 있으므로, 그 전에 응답자가 준비되어
 * 있어야 한다.
 *
 * 실행 컨텍스트: probe 또는 절전 복귀 시점의 프로세스 컨텍스트.
 * 읽고-고쳐-쓰기이지만 경쟁자가 없어 락을 쓰지 않는다.
 *
 * 호출 체인:
 *   cdns_pcie_host_setup() -> cdns_pcie_host_link_setup() -> [이 함수]
 *     -> cdns_pcie_readl() / cdns_pcie_writel()
 */
static void cdns_pcie_host_enable_ptm_response(struct cdns_pcie *pcie)
{
	/* [한국어] 위 disable 쪽과 같은 read-modify-write 용 임시 변수. */
	u32 val;

	/* [한국어] 현재 PTM_CTRL 값을 읽는다. */
	val = cdns_pcie_readl(pcie, CDNS_PCIE_LM_PTM_CTRL);
	/* [한국어] PTMRSEN(비트 17)을 1 로 세워 되쓴다.
	 * 링크를 올리기 직전에 켜 두어야, 링크가 올라온 뒤 아래쪽 장치가 보내는
	 * PTM Request 에 이 루트 컴플렉스가 응답할 수 있다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_PTM_CTRL, val | CDNS_PCIE_LM_TPM_CTRL_PTMRSEN);
}

/* [한국어]
 * cdns_pcie_host_deinit_root_port - 루트 포트 config space 를 초기 상태로 되돌린다
 *
 * @rc: 대상 루트 컴플렉스.
 * @return: 없음.
 *
 * cdns_pcie_host_init_root_port() 의 대칭 함수다. 클래스 코드, 리비전,
 * vendor/device ID 를 모두 전부-1 값(0xff / 0xffff / 0xffffffff)으로 만든다.
 * PCI 에서 전부-1 은 "장치 없음" 을 뜻하는 관례적 값이므로, 이 상태로
 * 두면 이후 누군가 config 를 읽어도 유효한 브리지로 보이지 않는다.
 *
 * 왜 필요한가: 드라이버가 언로드되거나 remove 된 뒤에도 하드웨어의
 * 레지스터 값은 그대로 남는다. 클래스 코드가 브리지로 남아 있으면 다음
 * 초기화 때 어중간한 상태에서 열거가 시작될 수 있다.
 *
 * 주의할 점이 하나 있다. RC_BAR_CFG 에 쓰는 값이 비트 반전 식이라,
 * CTRL_DISABLED 가 0 인 탓에 실제로는 prefetch/IO 관련 네 비트만 0 이고
 * 나머지 비트는 모두 1 이 된다. 자세한 계산은 해당 라인 주석에 적었다.
 *
 * 실행 컨텍스트: remove 경로의 프로세스 컨텍스트. 이 시점에는 이미
 * pci_remove_root_bus() 로 아래쪽 장치가 모두 사라진 뒤다.
 *
 * 호출 체인:
 *   cdns_pcie_host_disable() -> cdns_pcie_host_deinit() -> [이 함수]
 *     -> cdns_pcie_rp_writeb/writew() / cdns_pcie_writel()
 */
static void cdns_pcie_host_deinit_root_port(struct cdns_pcie_rc *rc)
{
	/* [한국어] rc 에서 공통 컨트롤러 구조체를 꺼낸다. 레지스터 접근에 필요하다. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] value 는 RC_BAR_CFG 에 되쓸 값, ctrl 은 BAR 제어 필드에 넣을 코드. */
	u32 value, ctrl;

	/* [한국어] 루트 포트 config space 의 Class Code 상위 두 바이트를 0xffff 로 되돌린다.
	 * PCI 규약에서 0xff 는 "장치 없음/미정"에 해당하는 값이며,
	 * 아래 init 쪽이 PCI_CLASS_BRIDGE_PCI 로 채운 것을 원상태로 돌리는 것이다. */
	cdns_pcie_rp_writew(pcie, PCI_CLASS_DEVICE, 0xffff);
	/* [한국어] Programming Interface 바이트도 0xff 로 되돌린다. */
	cdns_pcie_rp_writeb(pcie, PCI_CLASS_PROG, 0xff);
	/* [한국어] Revision ID 바이트도 0xff 로 되돌린다. */
	cdns_pcie_rp_writeb(pcie, PCI_CLASS_REVISION, 0xff);
	/* [한국어] Local Management 의 ID 레지스터를 전부 1 로 만든다.
	 * 이 레지스터는 상위 16비트가 Subsystem Vendor ID, 하위 16비트가 Vendor ID 다
	 * (pcie-cadence-lga-regs.h 의 CDNS_PCIE_LM_ID_VENDOR/SUBSYS 참고).
	 * 0xffff 는 PCI 에서 "해당 장치 없음"을 뜻하는 값이다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_ID, 0xffffffff);
	/* [한국어] 루트 포트의 Device ID 도 0xffff 로 되돌린다. */
	cdns_pcie_rp_writew(pcie, PCI_DEVICE_ID, 0xffff);
	/* [한국어] BAR 제어 코드 0x0 = DISABLED. 아래 식에서 BAR0/BAR1 항의 값이 된다. */
	ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED;
	/* [한국어] RC_BAR_CFG 에 되쓸 값을 만든다. init 쪽 식을 그대로 비트 반전한 것이다.
	 * 주의할 점: CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED 가 0 이므로
	 * BAR0_CTRL(ctrl) 과 BAR1_CTRL(ctrl) 항은 실제로 0 이다. 따라서 이 식은
	 * PREFETCH_MEM_ENABLE(비트 17), PREFETCH_MEM_64BITS(18), IO_ENABLE(19),
	 * IO_32BITS(20) 네 비트만 0 이고 나머지 비트는 모두 1 인 값이 된다.
	 * init 이 켠 네 비트를 끈다는 뜻은 통하지만, 함께 1 로 채워지는 다른 비트
	 * (특히 aperture/ctrl 필드)의 의미는 이 트리에서 확인할 수 없다. */
	value = ~(CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_32BITS);
	/* [한국어] read-modify-write 가 아니라 통째로 덮어쓴다. 아래 init 쪽도 같다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);
}

/* [한국어]
 * cdns_pcie_host_init_root_port - 루트 포트 자신의 config space 를 채운다
 *
 * @rc: 대상 루트 컴플렉스. vendor_id / device_id / ASPM quirk 플래그를 읽는다.
 * @return: 항상 0. 실패할 수 있는 동작이 없어 오류 경로가 없지만,
 *          호출자 cdns_pcie_host_init() 이 반환값을 검사하는 형태를 유지하려고
 *          int 로 두었다.
 *
 * PCI 코어가 이 브리지를 열거하려면, 브리지 자신이 config space 에서 제대로
 * 된 PCI-to-PCI 브리지로 보여야 한다. 그 모습을 여기서 만든다.
 *
 * 동작 단계:
 *   1. RC_BAR_CFG 를 설정한다 — 루트 포트의 BAR0/BAR1 은 끄고,
 *      Type 1 헤더의 Prefetchable Memory Base/Limit 을 64비트 폭으로,
 *      I/O Base/Limit 을 32비트 폭으로 켠다. 이 두 필드가 브리지가 아래로
 *      통과시킬 주소 범위를 나타내는 표준 필드다.
 *   2. 디바이스 트리에 vendor-id / device-id 가 있으면 그 값을 넣는다.
 *   3. 클래스 코드를 PCI_CLASS_BRIDGE_PCI 로 세운다. 이것이 없으면 PCI 코어가
 *      아래를 열거하지 않으므로, 이 함수에서 가장 결정적인 한 줄이다.
 *   4. 보드에 ASPM 결함 quirk 가 걸려 있으면 LNKCAP 에서 해당 능력 비트를
 *      지워, OS 가 그 저전력 상태를 쓰지 않게 한다.
 *
 * 실행 컨텍스트: probe 또는 절전 복귀 시점의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_host_setup() -> cdns_pcie_host_init() -> [이 함수]
 *     -> cdns_pcie_writel() / cdns_pcie_rp_readl() / cdns_pcie_rp_writel() 계열
 */
static int cdns_pcie_host_init_root_port(struct cdns_pcie_rc *rc)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] value 는 RC_BAR_CFG 에 쓸 값, ctrl 은 BAR0/BAR1 제어 코드. */
	u32 value, ctrl;
	/* [한국어] Local Management ID 레지스터에 쓸 vendor/subsystem 합성 값. */
	u32 id;

	/*
	 * Set the root complex BAR configuration register:
	 * - disable both BAR0 and BAR1.
	 * - enable Prefetchable Memory Base and Limit registers in type 1
	 *   config space (64 bits).
	 * - enable IO Base and Limit registers in type 1 config
	 *   space (32 bits).
	 */
	/* [한국어] 루트 포트에는 BAR 를 쓰지 않으므로 제어 코드를 DISABLED(0x0)로 둔다.
	 * 루트 포트가 자기 BAR 로 메모리를 흡수하면 아래쪽 장치로 갈 트래픽을
	 * 가로채는 셈이 되므로, 브리지에서는 보통 꺼 둔다. */
	ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED;
	/* [한국어] 상류 주석이 이 식의 네 가지 의도를 이미 정리해 두었다.
	 * BAR0/BAR1 은 끄고, Type 1 config space 의 Prefetchable Memory Base/Limit 을
	 * 64비트 폭으로, I/O Base/Limit 을 32비트 폭으로 활성화한다.
	 * 이 둘은 브리지가 아래쪽으로 어떤 주소 범위를 통과시킬지 결정하는
	 * 표준 Type 1 헤더 필드이며(PCIe/PCI-to-PCI Bridge 규격), 64비트/32비트 폭을
	 * 켜야 4GB 위쪽 prefetchable 창과 32비트 I/O 창을 표현할 수 있다. */
	value = CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_32BITS;
	/* [한국어] 만든 값을 RC_BAR_CFG 에 통째로 쓴다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);

	/* Set root port configuration space */
	/* [한국어] 0xffff 는 cdns_pcie_host_setup() 이 넣어 둔 "디바이스 트리에 vendor-id
	 * 속성이 없었다" 표시다. 값이 있을 때만 하드웨어를 건드린다. */
	if (rc->vendor_id != 0xffff) {
		/* [한국어] Vendor ID(하위 16비트)와 Subsystem Vendor ID(상위 16비트)에 같은 값을 넣는다.
		 * 한 레지스터에 두 필드가 들어 있어 한 번의 쓰기로 처리한다. */
		id = CDNS_PCIE_LM_ID_VENDOR(rc->vendor_id) |
			CDNS_PCIE_LM_ID_SUBSYS(rc->vendor_id);
		/* [한국어] Local Management ID 레지스터에 반영한다. 루트 포트 config space 의
		 * Vendor ID 는 이 레지스터를 통해 정해진다. */
		cdns_pcie_writel(pcie, CDNS_PCIE_LM_ID, id);
	}

	/* [한국어] device-id 속성이 있었을 때만 덮어쓴다. */
	if (rc->device_id != 0xffff)
		/* [한국어] Device ID 는 루트 포트 config space 에 직접 쓴다 —
		 * Vendor ID 와 달리 Local Management 레지스터를 거치지 않는다. */
		cdns_pcie_rp_writew(pcie, PCI_DEVICE_ID, rc->device_id);

	/* [한국어] Revision ID 를 0 으로 초기화한다. deinit 이 0xff 로 만들어 둔 것을 되돌린다. */
	cdns_pcie_rp_writeb(pcie, PCI_CLASS_REVISION, 0);
	/* [한국어] Programming Interface 도 0. */
	cdns_pcie_rp_writeb(pcie, PCI_CLASS_PROG, 0);
	/* [한국어] Class Code 상위 16비트를 PCI_CLASS_BRIDGE_PCI(0x0604)로 세운다.
	 * 이것이 이 함수의 핵심이다 — PCI 코어는 config 를 읽어 클래스가 PCI-to-PCI
	 * 브리지일 때만 그 아래를 다시 열거한다. 이 값이 없으면 루트 포트 아래의
	 * 장치들이 발견되지 않는다. */
	cdns_pcie_rp_writew(pcie, PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_PCI);

	/* [한국어] PCI Express capability 안의 Link Capabilities 레지스터를 읽는다.
	 * CDNS_PCIE_RP_CAP_OFFSET(0xC0) 이 이 IP 에서 PCIe capability 가 놓인 자리다. */
	value = cdns_pcie_rp_readl(pcie, CDNS_PCIE_RP_CAP_OFFSET + PCI_EXP_LNKCAP);
	/* [한국어] 일부 보드는 ASPM L0s 진입이 깨져 있어 링크가 죽는다. SoC 드라이버가
	 * 이 quirk 플래그를 세워 두면 아예 능력 자체를 감춘다. */
	if (rc->quirk_broken_aspm_l0s)
		/* [한국어] LNKCAP 의 ASPM Support 필드에서 L0s 비트를 지운다.
		 * 능력이 없다고 보고하면 OS 의 ASPM 정책이 L0s 를 켜지 않는다. */
		value &= ~PCI_EXP_LNKCAP_ASPM_L0S;
	/* [한국어] L1 쪽도 같은 사정의 별도 quirk 다. */
	if (rc->quirk_broken_aspm_l1)
		/* [한국어] LNKCAP 에서 ASPM L1 비트를 지운다. */
		value &= ~PCI_EXP_LNKCAP_ASPM_L1;
	/* [한국어] 고친 LNKCAP 값을 되쓴다. 보통 LNKCAP 은 읽기 전용이지만, 이 IP 는
	 * 루트 포트 config space 를 레지스터로 노출해 초기화 중 쓰기를 허용한다. */
	cdns_pcie_rp_writel(pcie, CDNS_PCIE_RP_CAP_OFFSET + PCI_EXP_LNKCAP, value);

	return 0;
}

/* [한국어]
 * cdns_pcie_host_bar_ib_config - 인바운드(PCIe -> AXI) BAR 하나를 설정한다
 *
 * @rc: 대상 루트 컴플렉스. avail_ib_bar[] 로 점유 상태를 관리한다.
 * @bar: 쓸 창. RP_BAR0, RP_BAR1, 또는 RP_NO_BAR.
 * @cpu_addr: 이 창이 가리킬 AXI(시스템 메모리) 쪽 시작 주소.
 * @size: 창 크기. 호출자가 이미 2의 거듭제곱으로 맞춰 넘긴다.
 * @flags: resource 플래그. IORESOURCE_PREFETCH 만 본다.
 * @return: 0 이면 성공. 그 BAR 가 이미 쓰이고 있으면 -EBUSY 이고,
 *          호출자(공통 코드)는 다른 BAR 를 고르거나 범위를 쪼개 다시 시도한다.
 *
 * 아웃바운드가 CPU 의 접근을 PCIe 로 내보내는 창이라면, 인바운드는 그 반대다.
 * 아래쪽 장치가 보낸 메모리 쓰기/읽기 TLP 를 받아 시스템 메모리 주소로
 * 바꿔 주는 창이며, 곧 "이 브리지 아래의 장치가 DMA 로 닿을 수 있는 범위" 다.
 * 그 범위를 정하는 것이 디바이스 트리의 dma-ranges 이고, 이 함수는 그 항목
 * 하나를 레지스터에 옮긴다.
 *
 * 이 함수는 공통 코드에 함수 포인터로 넘겨진다. 배정 알고리즘(크기 내림차순
 * 정렬 후 큰 것부터 best-fit)은 pcie-cadence-host-common.c 에 한 벌만 있고,
 * 레지스터를 실제로 만지는 이 부분만 구형/신형이 따로 구현한다.
 *
 * 동작 단계:
 *   1. avail_ib_bar[] 로 중복 배정을 막고, 성공하면 점유로 표시한다.
 *   2. 크기를 비트 수로 바꿔 주소 변환 레지스터(ADDR0/ADDR1)에 쓴다.
 *   3. RP_NO_BAR 는 config space 상의 BAR 가 없으므로 여기서 끝난다.
 *   4. RC_BAR_CFG 에서 이 BAR 의 제어·크기 필드를 갱신한다. 4GB 를 넘는
 *      창이면 64비트 BAR 로, 아니면 32비트 BAR 로 설정한다.
 *
 * 실행 컨텍스트: probe 또는 절전 복귀 시점의 프로세스 컨텍스트.
 *   avail_ib_bar[] 를 락 없이 만지는데, 한 컨트롤러의 초기화가 한 스레드에서만
 *   진행되기 때문이다.
 *
 * 에러 경로: -EBUSY 하나뿐이고, 그 경우 아무 레지스터도 건드리지 않는다.
 *
 * 호출 체인:
 *   cdns_pcie_host_init_address_translation() [이 파일]
 *     -> cdns_pcie_host_map_dma_ranges() [pcie-cadence-host-common.c]
 *       -> pci_host_ib_config 콜백 = [이 함수] -> cdns_pcie_writel()
 */
int cdns_pcie_host_bar_ib_config(struct cdns_pcie_rc *rc,
				 enum cdns_pcie_rp_bar bar,
				 u64 cpu_addr,
				 u64 size,
				 unsigned long flags)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] addr0/addr1 은 인바운드 창의 CPU(AXI) 쪽 목적지 주소,
	 * aperture 는 창 크기를 비트 수로 표현한 값, value 는 RC_BAR_CFG 작업용. */
	u32 addr0, addr1, aperture, value;

	/* [한국어] 이 BAR 가 이미 다른 dma-ranges 항목에 배정되었는지 본다.
	 * avail_ib_bar[] 는 cdns_pcie_host_setup() 이 전부 true 로 초기화하고,
	 * 공통 코드 cdns_pcie_host_map_dma_ranges() 가 큰 범위부터 채워 나간다.
	 * -EBUSY 를 받으면 그 쪽이 다른 BAR 를 고르거나 범위를 쪼갠다. */
	if (!rc->avail_ib_bar[bar])
		return -EBUSY;

	/* [한국어] 점유 표시. 이 배열이 곧 인바운드 BAR 할당 상태의 전부다.
	 * 동기화 장치는 없는데, probe/resume 시점에 한 스레드만 만지기 때문이다. */
	rc->avail_ib_bar[bar] = false;

	/* [한국어] 크기를 비트 수로 바꾼다. 이 IP 는 창 크기를 바이트가 아니라
	 * "주소의 하위 몇 비트를 창 안에서 쓸 것인가"로 표현한다.
	 * 호출자(공통 코드)가 이미 2의 거듭제곱으로 맞춰 넘겨 준다. */
	aperture = ilog2(size);
	/* [한국어] 인바운드 창이 가리킬 AXI 쪽 주소의 하위 32비트와 크기를 한 워드에 담는다.
	 * GENMASK(31, 8) 로 하위 8비트를 잘라 내는 이유는 그 자리가 주소가 아니라
	 * NBITS 필드이기 때문이다 — 그래서 시작 주소는 256바이트 경계에 맞아야 한다.
	 * 아웃바운드 창(cdns_pcie_set_outbound_region)과 완전히 같은 형식이다. */
	addr0 = CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS(aperture) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	/* [한국어] 상위 32비트는 통째로 들어간다. 4GB 위쪽 DRAM 을 가리키기 위해 필요하다. */
	addr1 = upper_32_bits(cpu_addr);
	/* [한국어] 인바운드 BAR 의 주소 변환 레지스터에 하위 워드를 쓴다.
	 * 이 순간부터 아래쪽 장치가 이 BAR 범위로 보낸 메모리 쓰기 TLP 가
	 * 여기 적힌 AXI 주소로 변환되어 시스템 메모리에 닿는다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR0(bar), addr0);
	/* [한국어] 상위 워드까지 쓰면 변환이 완성된다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR1(bar), addr1);

	/* [한국어] RP_NO_BAR 는 실제 BAR 가 아니라 "BAR 에 맞지 않는 나머지를 받는 창"이다.
	 * 대응하는 config space BAR 가 없으니 아래의 RC_BAR_CFG 설정도 필요 없고,
	 * bar_aperture_mask[] 에도 그 자리가 없다 — 여기서 반드시 빠져나가야 한다. */
	if (bar == RP_NO_BAR)
		return 0;

	/* [한국어] RC_BAR_CFG 는 BAR0 와 BAR1 의 설정이 한 레지스터에 같이 들어 있으므로
	 * 다른 BAR 의 설정을 보존하려면 read-modify-write 를 해야 한다. */
	value = cdns_pcie_readl(pcie, CDNS_PCIE_LM_RC_BAR_CFG);
	/* [한국어] 이번 BAR 에 해당하는 제어/aperture 필드만 먼저 지운다.
	 * 제어 코드 네 가지를 모두 OR 해서 지우는 것은 3비트 필드 전체를 덮기 위함이고
	 * (0x4|0x5|0x6|0x7 = 0x7), APERTURE 항의 bar_aperture_mask[bar] + 2 는
	 * 매크로 안의 -2 와 상쇄되어 마스크 값 자체를 그 자리에 놓는다. */
	value &= ~(LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar) |
		   LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar) |
		   LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar) |
		   LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar) |
		   LM_RC_BAR_CFG_APERTURE(bar, bar_aperture_mask[bar] + 2));
	/* [한국어] 창의 끝이 4GB 를 넘는지 본다. 넘으면 32비트 BAR 로는 그 주소를 표현할 수
	 * 없으므로 64비트 BAR 로 설정해야 한다. */
	if (size + cpu_addr >= SZ_4G) {
		/* [한국어] IORESOURCE_PREFETCH 는 이 범위가 prefetchable 인지를 나타내는
		 * 디바이스 트리 유래 플래그다. prefetchable 이 아니면 일반 메모리 코드를 넣는다. */
		if (!(flags & IORESOURCE_PREFETCH))
			/* [한국어] 일반 64비트 메모리 BAR 코드(0x6). */
			value |= LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar);
		/* [한국어] prefetchable 64비트 코드(0x7)를 무조건 OR 한다.
		 * 주의: 두 매크로는 같은 3비트 필드에 놓이고 값이 0x6 과 0x7 이므로
		 * OR 결과는 언제나 0x7 이 된다. 즉 위 IORESOURCE_PREFETCH 분기가 최종 값에
		 * 영향을 주지 못한다. 이것은 pcie-cadence-lga-regs.h 의 상수만으로 확인되는
		 * 계산이며, 의도인지 결함인지는 이 트리에서 확인할 수 없다. */
		value |= LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar);
	} else {
		/* [한국어] 4GB 아래에 완전히 들어가는 창이면 32비트 BAR 로 충분하다. */
		if (!(flags & IORESOURCE_PREFETCH))
			/* [한국어] 일반 32비트 메모리 BAR 코드(0x4). */
			value |= LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar);
		/* [한국어] prefetchable 32비트 코드(0x5). 위 64비트 경우와 마찬가지로
		 * 0x4|0x5 = 0x5 이므로 결과는 항상 prefetchable 쪽이 된다. */
		value |= LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar);
	}

	/* [한국어] 크기 필드를 넣는다. 매크로가 (aperture - 2) 를 넣는 것은 이 IP 가
	 * 크기 코드 0 을 최소 크기 2^2 에 대응시키기 때문으로 읽히지만,
	 * 그 대응표의 근거는 Cadence IP 문서 소관이라 이 트리에서 확인할 수 없다. */
	value |= LM_RC_BAR_CFG_APERTURE(bar, aperture);
	/* [한국어] 제어 코드와 크기를 반영한다. 이 쓰기로 해당 RC BAR 가 활성화된다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);

	return 0;
}

/* [한국어]
 * cdns_pcie_host_unmap_dma_ranges - 쓰이던 인바운드 창을 모두 끈다
 *
 * @rc: 대상 루트 컴플렉스.
 * @return: 없음.
 *
 * cdns_pcie_host_bar_ib_config() 이 열어 둔 창들을 되돌린다.
 * avail_ib_bar[] 가 false 인 것, 즉 실제로 배정되었던 창만 손댄다.
 *
 * 왜 필요한가: 인바운드 창을 열어 둔 채로 브리지를 내리면 아래쪽 장치가
 * 여전히 시스템 메모리에 DMA 를 쓸 수 있다. 그래서 정리 경로에서 가장
 * 먼저 끊어야 하는 것이 이 경로다 —
 * cdns_pcie_host_deinit_address_translation() 이 이 함수를 맨 앞에 부른다.
 *
 * 두 가지 특징을 눈여겨볼 만하다. 첫째, RC_BAR_CFG 를 읽지 않고 마스크의
 * 비트 반전 값을 통째로 쓴다. 둘째, 그 쓰기가 반복문 안에 있어 매 회차마다
 * 레지스터 전체가 덮어써진다. 자세한 계산은 해당 라인 주석에 적었다.
 *
 * 참고로 avail_ib_bar[] 를 다시 true 로 돌리지는 않는다. 재초기화가 필요한
 * 경로(pci-j721e.c 의 절전 복귀)는 호출자 쪽에서 그 배열을 직접 초기화한다.
 *
 * 실행 컨텍스트: remove 또는 절전 진입 시점의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_host_disable() -> cdns_pcie_host_deinit()
 *     -> cdns_pcie_host_deinit_address_translation() -> [이 함수]
 *       -> cdns_pcie_writel()
 */
static void cdns_pcie_host_unmap_dma_ranges(struct cdns_pcie_rc *rc)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] RP_BAR0 부터 RP_NO_BAR 까지 훑을 반복 변수. */
	enum cdns_pcie_rp_bar bar;
	/* [한국어] RC_BAR_CFG 에 쓸 값. */
	u32 value;

	/* Reset inbound configuration for all BARs which were being used */
	/* [한국어] 세 인바운드 창을 모두 훑는다. RP_NO_BAR 도 주소 변환 레지스터는
	 * 가지고 있으므로 포함된다. */
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++) {
		/* [한국어] avail_ib_bar 가 true 면 애초에 쓰이지 않은 창이므로 건드릴 것이 없다.
		 * (설정한 것만 되돌린다는 뜻 — 상류 주석의 "which were being used".) */
		if (rc->avail_ib_bar[bar])
			continue;

		/* [한국어] 주소 변환 레지스터를 0 으로 지운다. 이 순간 이 창을 통한
		 * PCIe → AXI 변환이 끊긴다. */
		cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR0(bar), 0);
		/* [한국어] 상위 워드까지 지워야 완전히 꺼진다. */
		cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR1(bar), 0);

		/* [한국어] RP_NO_BAR 에는 대응하는 config space BAR 가 없으므로
		 * RC_BAR_CFG 를 손댈 것도, bar_aperture_mask[] 를 읽을 것도 없다. */
		if (bar == RP_NO_BAR)
			continue;

		/* [한국어] 설정할 때와 달리 read-modify-write 를 하지 않고, 마스크의 비트 반전 값을
		 * 통째로 쓴다. 그 결과 이번 BAR 의 제어/aperture 필드는 0 이 되지만
		 * 나머지 비트는 모두 1 이 된다. deinit_root_port() 의 같은 패턴과 마찬가지로
		 * 1 로 채워지는 비트들의 의미는 이 트리에서 확인할 수 없다.
		 * 또 반복문 안에서 매번 전체를 덮어쓰므로 마지막 회차의 값만 남는다. */
		value = ~(LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar) |
			  LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar) |
			  LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar) |
			  LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar) |
			  LM_RC_BAR_CFG_APERTURE(bar, bar_aperture_mask[bar] + 2));
		/* [한국어] 만든 값을 RC_BAR_CFG 에 쓴다. */
		cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);
	}
}

/* [한국어]
 * cdns_pcie_host_deinit_address_translation - 주소 변환 창을 모두 닫는다
 *
 * @rc: 대상 루트 컴플렉스.
 * @return: 없음.
 *
 * cdns_pcie_host_init_address_translation() 의 대칭 함수다. 인바운드를 먼저
 * 끊고, 그 다음 아웃바운드 창을 0번(config 전용)부터 차례로 지운다.
 *
 * 순서에 뜻이 있다. 인바운드를 먼저 끊어야 아래쪽 장치의 DMA 가 시스템
 * 메모리에 더는 닿지 않는다. 아웃바운드는 CPU 가 나가는 길이므로 CPU 쪽이
 * 더 이상 그 주소를 건드리지 않는 이상 급하지 않다.
 *
 * 아웃바운드 창 번호를 어떻게 아는가: 설정할 때와 똑같이 bridge->windows 를
 * 순회하며 1부터 세어 나간다. 목록이 그 사이에 바뀌지 않았다는 전제 위에서
 * 번호가 일치한다. 창 자체에 "몇 개를 썼는지" 를 기록해 두지는 않는다.
 *
 * 실행 컨텍스트: remove 또는 절전 진입 시점의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_host_disable() -> cdns_pcie_host_deinit() -> [이 함수]
 *     -> cdns_pcie_host_unmap_dma_ranges() [이 파일]
 *     -> cdns_pcie_reset_outbound_region() [pcie-cadence.c]
 */
static void cdns_pcie_host_deinit_address_translation(struct cdns_pcie_rc *rc)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] rc 는 호스트 브리지 private 영역에 들어 있으므로 거꾸로 브리지를 찾는다.
	 * bridge->windows 목록을 다시 훑어 아웃바운드 창 개수를 세기 위해 필요하다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rc);
	/* [한국어] bridge->windows 를 순회할 커서. */
	struct resource_entry *entry;
	/* [한국어] 아웃바운드 영역 번호. 설정할 때와 같은 순서로 세어야 짝이 맞는다. */
	int r;

	/* [한국어] 먼저 인바운드(PCIe → AXI) 쪽을 끈다. 아래쪽 장치의 DMA 가 시스템 메모리에
	 * 닿는 경로를 먼저 막는 것이 안전하다. */
	cdns_pcie_host_unmap_dma_ranges(rc);

	/*
	 * Reset outbound region 0 which was reserved for configuration space
	 * accesses.
	 */
	/* [한국어] config 접근 전용으로 예약해 둔 0번 창을 지운다.
	 * 이 창이 없어지면 cdns_pci_map_bus() 를 통한 config 접근이 더는 통하지 않는다. */
	cdns_pcie_reset_outbound_region(pcie, 0);

	/* Reset rest of the outbound regions */
	/* [한국어] 1번부터가 bridge->windows 에 대응하는 창들이다. 0번은 config 전용이므로 건너뛴다. */
	r = 1;
	/* [한국어] 설정 때와 똑같은 순서로 목록을 훑는다. 목록이 그동안 바뀌지 않았다는
	 * 전제 위에서 창 번호가 일치한다. */
	resource_list_for_each_entry(entry, &bridge->windows) {
		/* [한국어] 창 여섯 레지스터를 모두 0 으로 만든다(pcie-cadence.c). */
		cdns_pcie_reset_outbound_region(pcie, r);
		/* [한국어] 다음 창 번호로. 설정 쪽 루프와 증가 방식이 같아야 한다. */
		r++;
	}
}

/* [한국어]
 * cdns_pcie_host_init_address_translation - 아웃바운드·인바운드 창을 모두 연다
 *
 * @rc: 대상 루트 컴플렉스. cfg_res 와 브리지의 windows 목록을 읽는다.
 * @return: 0 이면 성공. 인바운드 배정이 실패하면 그 오류 코드를 그대로 전달한다.
 *
 * 이 브리지가 다루는 주소 변환을 한자리에서 설정한다. 세 부분으로 나뉜다.
 *
 * 첫째, 아웃바운드 0번 창을 config 접근 전용으로 예약한다. 이 창의 CPU 쪽
 * 주소는 디바이스 트리의 "cfg" 자원이고, 크기는 4KB(한 함수의 config space)다.
 * PCIe 쪽 목적지와 TLP 타입은 접근할 때마다 cdns_pci_map_bus() 가 다시 쓰므로
 * 여기서는 손대지 않고, 대신 바뀌지 않는 부분(상위 주소 0, requester 버스
 * 번호)만 한 번 박아 둔다. 상류 주석이 그 분담을 명시하고 있다.
 *
 * 둘째, bridge->windows 의 각 항목을 1번 창부터 차례로 아웃바운드 창에
 * 대응시킨다. 이것이 CPU 가 아래쪽 장치의 BAR 에 접근하는 통로가 된다.
 * I/O 창은 pci_pio_to_address() 로 포트 번호를 물리 주소로 되돌려 넣는다.
 *
 * 셋째, 인바운드는 공통 코드에 맡긴다. 이 파일의
 * cdns_pcie_host_bar_ib_config() 을 콜백으로 넘겨, 크기 정렬과 BAR 선택은
 * 공통 알고리즘이 하고 레지스터 쓰기만 이 파일이 하게 한다.
 *
 * 실행 컨텍스트: probe 또는 절전 복귀 시점의 프로세스 컨텍스트.
 *   이 함수가 끝나기 전에는 config 접근이 성립하지 않으므로, 반드시
 *   pci_host_probe() 보다 먼저 끝나야 한다.
 *
 * 에러 경로: 아웃바운드 설정은 실패하지 않는다(레지스터 쓰기뿐).
 *   인바운드에서 BAR 가 모자라면 공통 코드가 오류를 돌려주고, 그것이
 *   그대로 cdns_pcie_host_setup() 까지 올라가 probe 를 접는다.
 *
 * 호출 체인:
 *   cdns_pcie_host_setup() -> cdns_pcie_host_init() -> [이 함수]
 *     -> cdns_pcie_set_outbound_region() [pcie-cadence.c]
 *     -> cdns_pcie_host_map_dma_ranges() [pcie-cadence-host-common.c]
 */
static int cdns_pcie_host_init_address_translation(struct cdns_pcie_rc *rc)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] 브리지의 windows(아웃바운드로 열 CPU 주소 범위 목록)에 닿기 위해 필요하다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rc);
	/* [한국어] cdns_pcie_host_setup() 이 저장해 둔 "cfg" 자원. config 접근용 창의
	 * CPU 쪽 주소가 여기서 나온다. */
	struct resource *cfg_res = rc->cfg_res;
	/* [한국어] resource 목록 커서. */
	struct resource_entry *entry;
	/* [한국어] config 전용 창이 대응할 CPU 주소 = cfg 자원의 시작. */
	u64 cpu_addr = cfg_res->start;
	/* [한국어] 0번 창에 쓸 값들. addr0/addr1 은 CPU 쪽 주소, desc1 은 버스 번호. */
	u32 addr0, addr1, desc1;
	/* [한국어] r 은 아웃바운드 창 번호, busnr 은 이 브리지의 루트 버스 번호(기본 0). */
	int r, busnr = 0;

	/* [한국어] 디바이스 트리의 bus-range 로부터 온 IORESOURCE_BUS 항목을 찾는다. */
	entry = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	/* [한국어] bus-range 가 지정되지 않았으면 busnr 은 초기값 0 으로 남는다. */
	if (entry)
		/* [한국어] 지정되어 있으면 그 시작 번호가 이 브리지의 루트 버스 번호다.
		 * 아래에서 config TLP 의 requester 버스 번호로 쓰인다. */
		busnr = entry->res->start;

	/*
	 * Reserve region 0 for PCI configure space accesses:
	 * OB_REGION_PCI_ADDR0 and OB_REGION_DESC0 are updated dynamically by
	 * cdns_pci_map_bus(), other region registers are set here once for all.
	 */
	/* [한국어] config 창의 PCIe 쪽 상위 주소는 0 이다. 하위 주소는 접근할 때마다
	 * cdns_pci_map_bus() 가 다시 쓰지만, 상위 32비트는 config 주소가 32비트
	 * 안에 들어가므로 언제나 0 이면 된다(상류 주석의 "Should be programmed to zero"). */
	addr1 = 0; /* Should be programmed to zero. */
	/* [한국어] requester 의 버스 번호를 DESC1 에 한 번만 박아 둔다.
	 * cdns_pci_map_bus() 가 매번 쓰는 DESC0 에는 HARDCODED_RID 와 devfn 만 들어가고,
	 * 버스 번호는 여기서 정해진 값이 계속 쓰인다(cdns_pci_map_bus 위 상류 주석 참고). */
	desc1 = CDNS_PCIE_AT_OB_REGION_DESC1_BUS(busnr);
	/* [한국어] PCIe 쪽 상위 주소를 0 으로 쓴다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(0), addr1);
	/* [한국어] requester 버스 번호를 반영한다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(0), desc1);

	/* [한국어] SoC 에 따라 CPU 물리 주소와 이 IP 가 보는 주소가 다를 수 있다.
	 * 그런 SoC 는 cpu_addr_fixup 콜백을 제공해 여기서 보정한다. */
	if (pcie->ops && pcie->ops->cpu_addr_fixup)
		/* [한국어] 보정된 주소로 바꾼다. 예: 상위 비트에 고정 오프셋이 붙는 구성. */
		cpu_addr = pcie->ops->cpu_addr_fixup(pcie, cpu_addr);

	/* [한국어] 0번 창의 CPU 쪽 주소와 크기. NBITS(12) 로 4KB — 한 함수의 config space 다.
	 * GENMASK(31, 8) 로 하위 8비트를 자르는 것은 그 자리가 NBITS 필드이기 때문이며,
	 * cfg 자원의 시작 주소가 256바이트 경계에 맞아야 한다는 뜻이 된다. */
	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(12) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	/* [한국어] CPU 주소의 상위 32비트. */
	addr1 = upper_32_bits(cpu_addr);
	/* [한국어] CPU 쪽 하위 주소와 크기를 쓴다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(0), addr0);
	/* [한국어] 상위 주소까지 쓰면 config 전용 창이 열린다.
	 * 이 뒤로 cfg_base 에 접근하면 config TLP 가 나간다. */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(0), addr1);

	/* [한국어] 1번 창부터 일반 메모리/IO 창에 배정한다. 0번은 위에서 config 전용으로 썼다. */
	r = 1;
	/* [한국어] 브리지가 CPU 쪽에 여는 창 목록. PCI 코어가 디바이스 트리의 ranges 를
	 * 파싱해 채워 둔 것이다. */
	resource_list_for_each_entry(entry, &bridge->windows) {
		/* [한국어] 이번 항목이 나타내는 CPU(호스트) 쪽 주소 범위. */
		struct resource *res = entry->res;
		/* [한국어] 같은 범위를 PCIe 버스 주소로 환산한다. entry->offset 은 PCI 코어가
		 * 계산해 둔 "CPU 주소 - PCI 주소" 차이이므로, 빼면 PCI 주소가 된다. */
		u64 pci_addr = res->start - entry->offset;

		/* [한국어] I/O 공간 창과 메모리 창은 나가는 TLP 의 타입이 달라 분기한다. */
		if (resource_type(res) == IORESOURCE_IO)
			/* [한국어] I/O 창. is_io 를 true 로 넘겨 DESC0 의 타입을 IO(0x6)로 만들게 한다.
			 * CPU 주소로 pci_pio_to_address() 를 쓰는 이유는, 커널이 I/O 포트를 실제
			 * 물리 주소가 아니라 가상의 포트 번호 공간으로 관리하기 때문이다 —
			 * 그 번호를 다시 실제 물리 주소로 되돌려 창에 넣어야 한다. */
			cdns_pcie_set_outbound_region(pcie, busnr, 0, r,
						      true,
						      pci_pio_to_address(res->start),
						      pci_addr,
						      resource_size(res));
		else
			/* [한국어] 메모리 창. is_io 는 false 이고 CPU 주소는 res->start 를 그대로 쓴다.
			 * NVMe 를 포함해 아래쪽 장치의 BAR 는 이 창을 통해 CPU 에 보인다. */
			cdns_pcie_set_outbound_region(pcie, busnr, 0, r,
						      false,
						      res->start,
						      pci_addr,
						      resource_size(res));

		/* [한국어] 다음 창 번호. deinit 쪽 루프도 같은 순서로 센다. */
		r++;
	}

	/* [한국어] 마지막으로 인바운드(PCIe → AXI) 쪽을 연다.
	 * 실제 배정 알고리즘은 공통 코드에 있고, 레지스터를 만지는 부분만
	 * 이 파일의 cdns_pcie_host_bar_ib_config() 를 함수 포인터로 넘겨 처리한다.
	 * 신형 배치 판은 같은 자리에 자기 구현을 넘긴다. */
	return cdns_pcie_host_map_dma_ranges(rc, cdns_pcie_host_bar_ib_config);
}

/* [한국어]
 * cdns_pcie_host_deinit - cdns_pcie_host_init() 이 한 일을 역순으로 되돌린다
 *
 * @rc: 대상 루트 컴플렉스.
 * @return: 없음.
 *
 * 두 줄짜리 래퍼이지만 순서 자체가 이 함수의 내용이다.
 * init 이 "루트 포트 config -> 주소 변환" 순서였으므로,
 * deinit 은 "주소 변환 -> 루트 포트 config" 순서가 된다.
 *
 * 왜 그 순서인가: 주소 변환을 먼저 끊어야 아래쪽 장치의 DMA 와 CPU 의
 * 접근 경로가 모두 막힌다. 그 뒤에야 루트 포트의 정체성(클래스 코드 등)을
 * 지우는 것이 안전하다.
 *
 * init 과 달리 EXPORT 되지 않는다. 외부에서는 cdns_pcie_host_disable() 만
 * 부르며, 그 안에서 링크 정리까지 함께 처리하기 때문이다.
 *
 * 실행 컨텍스트: remove 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cdns_pcie_host_disable() -> [이 함수]
 *     -> cdns_pcie_host_deinit_address_translation()
 *     -> cdns_pcie_host_deinit_root_port()
 */
static void cdns_pcie_host_deinit(struct cdns_pcie_rc *rc)
{
	/* [한국어] 주소 변환을 먼저 끈다. 그래야 아래쪽 장치가 더는 시스템 메모리에 닿지 않는다. */
	cdns_pcie_host_deinit_address_translation(rc);
	/* [한국어] 그 다음 루트 포트 config space 를 되돌린다. 순서가 반대면 config 가
	 * 먼저 무효화되어 진행 중인 접근이 엉킬 수 있다 — init 의 역순이다. */
	cdns_pcie_host_deinit_root_port(rc);
}

/* [한국어]
 * cdns_pcie_host_init - 루트 포트 config space 와 주소 변환을 설정한다
 *
 * @rc: 대상 루트 컴플렉스.
 * @return: 0 이면 성공. 하위 단계의 오류 코드를 그대로 전달한다.
 *
 * cdns_pcie_host_setup() 안에서 불리는 것이 보통이지만, 별도로 내보내는
 * 이유가 있다. pci-j721e.c 는 절전 복귀(resume_noirq) 경로에서 probe 전체를
 * 다시 하지 않고, cdns_pcie_host_link_setup() 과 이 함수만 다시 부른다.
 * 절전 중에 컨트롤러 레지스터가 날아가므로 링크와 주소 변환만 복원하면
 * 되고, 브리지 등록이나 자원 매핑은 그대로 살아 있기 때문이다.
 *
 * 순서가 중요하다. 루트 포트의 클래스 코드가 브리지로 서 있어야 그 다음
 * 열거가 성립하므로 config space 를 먼저 채우고, 그 뒤에 주소 변환 창을 연다.
 *
 * 실행 컨텍스트: probe 또는 절전 복귀 시점의 프로세스 컨텍스트.
 *
 * 에러 경로: 첫 단계가 실패하면 두 번째를 시도하지 않고 바로 돌아간다.
 *   호출자는 probe 를 접거나(setup 경로) refclk 을 끄고 복귀를 실패시킨다
 *   (j721e resume 경로).
 *
 * 호출 체인:
 *   cdns_pcie_host_setup() 또는 j721e_pcie_resume_noirq() -> [이 함수]
 *     -> cdns_pcie_host_init_root_port()
 *     -> cdns_pcie_host_init_address_translation()
 */
int cdns_pcie_host_init(struct cdns_pcie_rc *rc)
{
	/* [한국어] 하위 두 단계의 오류 코드. */
	int err;

	/* [한국어] 먼저 루트 포트 자신의 config space 를 채운다.
	 * class code 가 브리지로 설정되어야 PCI 코어가 아래를 열거한다. */
	err = cdns_pcie_host_init_root_port(rc);
	if (err)
		return err;

	/* [한국어] 그 다음 주소 변환 창을 연다. 이 함수의 반환값이 곧 이 함수의 결과다. */
	return cdns_pcie_host_init_address_translation(rc);
}
/* [한국어] pci-j721e.c 가 resume 경로에서 이 함수만 따로 부르기 위해 내보낸다.
 * 전체 setup 을 다시 하지 않고 링크와 주소 변환만 복원하는 구조다. */
EXPORT_SYMBOL_GPL(cdns_pcie_host_init);

/* [한국어]
 * cdns_pcie_host_link_disable - 링크를 내리고 PTM 응답자를 끈다
 *
 * @rc: 대상 루트 컴플렉스.
 * @return: 없음.
 *
 * cdns_pcie_host_link_setup() 의 대칭 함수다. 그쪽이 "PTM 켜기 -> 링크 시작"
 * 이었으므로 이쪽은 "링크 정지 -> PTM 끄기" 가 된다.
 *
 * cdns_pcie_stop_link() 은 pcie-cadence.h 의 static inline 이며, SoC 별
 * ops->stop_link 콜백이 있을 때만 그것을 부른다. 콜백을 제공하지 않는
 * SoC(예: pcie-cadence-plat.c 의 기본 경로)에서는 아무 일도 하지 않는다 —
 * 링크를 소프트웨어로 내릴 수단이 없는 구성이 있기 때문이다.
 *
 * 실행 컨텍스트: remove 경로의 프로세스 컨텍스트. 이 시점에는 이미
 * 버스가 제거되고 주소 변환도 꺼진 뒤다.
 *
 * 호출 체인:
 *   cdns_pcie_host_disable() -> [이 함수]
 *     -> cdns_pcie_stop_link() [pcie-cadence.h]
 *     -> cdns_pcie_host_disable_ptm_response() [이 파일]
 */
static void cdns_pcie_host_link_disable(struct cdns_pcie_rc *rc)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;

	/* [한국어] SoC 별 stop_link 콜백을 부른다. 콜백이 없는 SoC 는 아무 일도 하지 않는다
	 * (pcie-cadence.h 의 static inline 이 ops 유무를 검사한다). */
	cdns_pcie_stop_link(pcie);
	/* [한국어] PTM 응답자 기능을 끈다. link_setup 이 켠 것을 대칭으로 되돌린다. */
	cdns_pcie_host_disable_ptm_response(pcie);
}

/* [한국어]
 * cdns_pcie_host_link_setup - PTM 을 켜고 링크를 올린 뒤 완료를 기다린다
 *
 * @rc: 대상 루트 컴플렉스. quirk 플래그들을 읽는다.
 * @return: 링크 시작 자체가 실패했을 때만 그 오류 코드. 그 밖에는 항상 0 이며,
 *          링크가 끝내 올라오지 않아도 0 이다.
 *
 * 반환값 규칙이 이 함수의 핵심이다. 링크가 안 올라온 것은 오류가 아니다 —
 * 핫플러그 슬롯이 비어 있거나 아직 장치가 꽂히지 않았을 수 있으므로,
 * 그 이유로 probe 를 실패시키면 나중에 장치를 꽂아도 쓸 수 없게 된다.
 * 그래서 dev_dbg 로만 남기고 0 을 돌려준다. 반대로 SoC 의 start_link 콜백이
 * 실패한 것은 하드웨어를 켜지 못한 것이므로 진짜 오류다.
 *
 * 동작 단계:
 *   1. Detect.Quiet quirk 가 걸린 IP 판본이면 LTSSM 최소 지연을 보정한다.
 *   2. PTM 응답자를 켠다 — 링크가 올라온 직후를 대비해 미리 준비한다.
 *   3. SoC 별 start_link 콜백으로 LTSSM 트레이닝을 시작한다.
 *   4. 공통 코드로 링크가 올라오기를 기다리고, Gen2 트레이닝 결함 quirk 가
 *      걸린 보드라면 재트레이닝까지 시도한다.
 *
 * 실행 컨텍스트: probe 또는 절전 복귀 시점의 프로세스 컨텍스트.
 *   4단계의 대기 루프가 잠들 수 있으므로 원자적 컨텍스트에서 부르면 안 된다.
 *
 * 호출 체인:
 *   cdns_pcie_host_setup() 또는 j721e_pcie_resume_noirq() -> [이 함수]
 *     -> cdns_pcie_detect_quiet_min_delay_set() [pcie-cadence.c]
 *     -> cdns_pcie_host_enable_ptm_response() [이 파일]
 *     -> cdns_pcie_start_link() [pcie-cadence.h -> SoC 별 ops]
 *     -> cdns_pcie_host_start_link() [pcie-cadence-host-common.c]
 */
int cdns_pcie_host_link_setup(struct cdns_pcie_rc *rc)
{
	/* [한국어] 레지스터 접근용 공통 구조체. */
	struct cdns_pcie *pcie = &rc->pcie;
	/* [한국어] 오류 메시지를 낼 device. rc->pcie.dev 는 SoC 드라이버가 채워 둔 값이다. */
	struct device *dev = rc->pcie.dev;
	/* [한국어] 링크 시작과 대기의 결과. */
	int ret;

	/* [한국어] 일부 IP 판본은 LTSSM 의 Detect.Quiet 최소 대기 시간이 너무 짧아
	 * 링크 트레이닝이 불안정하다. SoC 드라이버가 이 플래그를 세워 두면 보정한다. */
	if (rc->quirk_detect_quiet_flag)
		/* [한국어] LTSSM Detect.Quiet 최소 지연을 설정한다(pcie-cadence.c). */
		cdns_pcie_detect_quiet_min_delay_set(&rc->pcie);

	/* [한국어] 링크를 올리기 전에 PTM 응답자를 켠다. 링크가 올라온 뒤 아래쪽 장치가
	 * 보내는 PTM Request 에 답할 수 있게 하기 위함이다. */
	cdns_pcie_host_enable_ptm_response(pcie);

	/* [한국어] SoC 별 start_link 콜백. 보통 LTSSM 을 풀어 트레이닝을 시작시킨다.
	 * 콜백이 없는 SoC 는 0 을 돌려받는다. */
	ret = cdns_pcie_start_link(pcie);
	/* [한국어] 링크 시작 자체가 실패하면 더 진행할 수 없다. */
	if (ret) {
		/* [한국어] SoC 콜백 실패는 하드웨어를 켜지 못한 것이므로 오류로 남긴다. */
		dev_err(dev, "Failed to start link\n");
		/* [한국어] 호출자(cdns_pcie_host_setup 또는 j721e resume)가 probe 를 접는다. */
		return ret;
	}

	/* [한국어] 실제로 링크가 올라오기를 기다린다. 두 번째 인자는 링크 상태를 읽는 방법으로,
	 * 여기서는 pcie-cadence.h 의 static inline cdns_pcie_link_up() 을 넘긴다 —
	 * 그것이 다시 SoC 별 ops->link_up 콜백을 부르고, 콜백이 없으면 true 를 돌려준다.
	 * 이 트리에서 ops->link_up 을 실제로 채우는 SoC 는 pci-j721e.c 의
	 * j721e_pcie_link_up 과 pci-sky1.c 의 sky1_pcie_link_up 둘뿐이며,
	 * 나머지 SoC 는 콜백이 없어 언제나 true 를 받는다.
	 * 이름이 비슷한 cdns_pcie_linkup()(pcie-cadence.c, 레지스터를 직접 읽는
	 * 함수)은 export 되어 있지만 이 트리 안에서 부르는 곳이 없다(전수 확인). */
	ret = cdns_pcie_host_start_link(rc, cdns_pcie_link_up);
	/* [한국어] 링크가 안 올라온 경우. */
	if (ret)
		/* [한국어] 여기서 오류를 반환하지 않고 dev_dbg 로만 남기는 것이 중요하다.
		 * 핫플러그 슬롯처럼 지금 장치가 안 꽂혀 있을 수도 있으므로,
		 * 링크가 없다고 probe 를 실패시키지 않는다. */
		dev_dbg(dev, "PCIe link never came up\n");

	/* [한국어] 그래서 항상 0 을 돌려준다 — 링크 유무와 무관하게 초기화는 계속된다. */
	return 0;
}
/* [한국어] pci-j721e.c 의 resume 경로가 이 함수를 직접 부르기 위해 내보낸다. */
EXPORT_SYMBOL_GPL(cdns_pcie_host_link_setup);

/* [한국어]
 * cdns_pcie_host_disable - 호스트 브리지를 통째로 내린다 (remove 경로)
 *
 * @rc: 대상 루트 컴플렉스.
 * @return: 없음.
 *
 * cdns_pcie_host_setup() 이 한 일을 완전한 역순으로 되돌린다. 순서가 이
 * 함수의 전부라고 해도 좋다.
 *   1. pci_stop_root_bus() — 버스 위 장치들의 드라이버 remove 를 부른다.
 *      아직 하드웨어가 살아 있는 동안 드라이버가 정리할 기회를 준다.
 *      진행 중인 DMA 를 멈추는 것도 이 단계다.
 *   2. pci_remove_root_bus() — struct pci_dev 들을 실제로 없앤다.
 *   3. cdns_pcie_host_deinit() — 주소 변환을 끄고 루트 포트 config 를 되돌린다.
 *   4. cdns_pcie_host_link_disable() — 링크를 내리고 PTM 을 끈다.
 *
 * 1, 2 를 3 보다 먼저 하는 것이 특히 중요하다. 주소 변환을 먼저 끄면
 * 아직 살아 있는 장치의 DMA 나 MMIO 접근이 중간에 끊겨 버스 오류가 난다.
 *
 * 실행 컨텍스트: remove 경로의 프로세스 컨텍스트. 1, 2 단계가 다른 드라이버의
 *   remove 를 부르므로 당연히 잠들 수 있다.
 *
 * 호출 체인:
 *   j721e_pcie_remove() [pci-j721e.c] 또는 sg2042_pcie_remove() [pcie-sg2042.c]
 *     -> [이 함수] -> pci_stop_root_bus() / pci_remove_root_bus()
 *                  -> cdns_pcie_host_deinit() -> cdns_pcie_host_link_disable()
 */
void cdns_pcie_host_disable(struct cdns_pcie_rc *rc)
{
	/* [한국어] 루트 버스를 떼어 내기 위해 브리지가 필요하다. */
	struct pci_host_bridge *bridge;

	/* [한국어] rc 가 들어 있는 호스트 브리지를 거꾸로 찾는다. */
	bridge = pci_host_bridge_from_priv(rc);
	/* [한국어] 먼저 버스 위의 장치들을 정지시킨다(드라이버 remove 호출).
	 * 장치가 아직 살아 있는 상태에서 주소 변환을 끄면 진행 중인 DMA 가 깨진다. */
	pci_stop_root_bus(bridge->bus);
	/* [한국어] 그 다음 버스 자체를 제거한다. 이 시점에 아래쪽 장치의 struct pci_dev 가
	 * 모두 사라진다. */
	pci_remove_root_bus(bridge->bus);

	/* [한국어] 이제 안전하게 주소 변환과 루트 포트 config 를 되돌린다. */
	cdns_pcie_host_deinit(rc);
	/* [한국어] 마지막으로 링크를 내리고 PTM 을 끈다. 정리 순서는 setup 의 완전한 역순이다. */
	cdns_pcie_host_link_disable(rc);
}
/* [한국어] pci-j721e.c 의 remove 경로가 부르기 위해 내보낸다. */
EXPORT_SYMBOL_GPL(cdns_pcie_host_disable);

/* [한국어]
 * cdns_pcie_host_setup - SoC 드라이버가 부르는 호스트 초기화 진입점
 *
 * @rc: SoC 드라이버가 devm_pci_alloc_host_bridge() 로 잡은 브리지의 private
 *      영역에 놓인 구조체. 호출 전에 rc->pcie.dev 는 채워져 있어야 하고,
 *      quirk 플래그와 ops 도 SoC 드라이버가 미리 설정해 둔다.
 * @return: 0 이면 브리지 등록과 버스 열거까지 끝났다는 뜻. 실패하면 음수 오류.
 *
 * 이 파일의 시작점이자 SoC 드라이버와의 경계다. 하는 일은 순서대로 다음과 같다.
 *   1. 브리지를 거꾸로 찾고 컨트롤러를 RC 모드로 표시한다.
 *   2. 디바이스 트리에서 vendor-id / device-id 를 읽는다(없으면 0xffff).
 *   3. "reg"(레지스터 블록)와 "cfg"(config 접근 창) 두 자원을 매핑한다.
 *   4. 링크를 올린다 — config 접근이 링크 위에서 이뤄지므로 먼저 해야 한다.
 *   5. 인바운드 BAR 점유 표를 모두 비움으로 초기화한다.
 *   6. 루트 포트 config space 와 주소 변환 창을 설정한다.
 *   7. SoC 드라이버가 자기 pci_ops 를 넣어 두지 않았으면 기본 표를 꽂는다.
 *   8. pci_host_probe() 로 PCI 코어에 넘긴다. 이 안에서 버스 열거가 일어나며
 *      cdns_pci_map_bus() 가 반복 호출된다.
 *
 * 4번과 6번의 순서, 그리고 6번이 8번보다 앞서야 한다는 점이 핵심 제약이다.
 * 주소 변환 창이 열려 있지 않으면 config 접근 자체가 성립하지 않는다.
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트. 8번 단계가 아래쪽 장치들의
 *   드라이버 probe 를 연쇄적으로 부를 수 있으므로 오래 걸릴 수 있다.
 *
 * 에러 경로: 어느 단계에서 실패하든 음수를 그대로 돌려주고, 호출자인 SoC
 *   드라이버가 PHY 를 끄고 런타임 PM 을 되돌린다(pci-j721e.c 의 err_pcie_setup
 *   레이블 참고). 자원 매핑은 devm_ 계열이라 별도 해제가 필요 없다.
 *
 * 호출 체인:
 *   j721e_pcie_probe() / sg2042_pcie_probe() / cdns_plat_pcie_probe()
 *     -> [이 함수] -> cdns_pcie_host_link_setup() -> cdns_pcie_host_init()
 *                  -> pci_host_probe() [PCI 코어] -> cdns_pci_map_bus() 반복
 */
int cdns_pcie_host_setup(struct cdns_pcie_rc *rc)
{
	/* [한국어] 오류 메시지와 디바이스 트리 노드를 얻을 struct device. */
	struct device *dev = rc->pcie.dev;
	/* [한국어] 플랫폼 자원(reg/cfg)을 꺼내기 위해 platform_device 로 되돌린다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] vendor-id/device-id 속성을 읽을 디바이스 트리 노드. */
	struct device_node *np = dev->of_node;
	/* [한국어] rc 를 품고 있는 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 인바운드 BAR 를 모두 사용 가능 표시로 초기화할 때 쓸 반복 변수. */
	enum cdns_pcie_rp_bar bar;
	/* [한국어] 공통 컨트롤러 구조체 포인터. */
	struct cdns_pcie *pcie;
	/* [한국어] "cfg" 자원을 담을 임시 포인터. */
	struct resource *res;
	/* [한국어] 하위 단계들의 오류 코드. */
	int ret;

	/* [한국어] SoC 드라이버가 devm_pci_alloc_host_bridge() 로 잡아 둔 브리지를 거꾸로 찾는다. */
	bridge = pci_host_bridge_from_priv(rc);
	/* [한국어] rc 가 브리지 private 영역에서 나오지 않았다면 잘못 불린 것이다. */
	if (!bridge)
		return -ENOMEM;

	/* [한국어] 공통 구조체를 가리키게 한다. */
	pcie = &rc->pcie;
	/* [한국어] 이 컨트롤러를 루트 컴플렉스 모드로 표시한다.
	 * 이 플래그가 pcie-cadence.c 의 아웃바운드 창 설정에서 갈림길이 된다 —
	 * RC 면 HARDCODED_RID 를 세우고 소프트웨어가 requester ID 를 직접 채운다. */
	pcie->is_rc = true;

	/* [한국어] "속성 없음" 표시로 먼저 0xffff 를 넣는다. 아래 of_property_read_u32() 는
	 * 속성이 없으면 인자를 건드리지 않으므로 이 값이 그대로 남는다. */
	rc->vendor_id = 0xffff;
	/* [한국어] 디바이스 트리에서 vendor-id 를 읽는다. 반환값을 보지 않는 것은
	 * 위 초기값이 곧 "없음"의 의미를 담고 있기 때문이다. */
	of_property_read_u32(np, "vendor-id", &rc->vendor_id);

	/* [한국어] device-id 도 같은 방식. */
	rc->device_id = 0xffff;
	/* [한국어] 디바이스 트리에서 device-id 를 읽는다. */
	of_property_read_u32(np, "device-id", &rc->device_id);

	/* [한국어] "reg" 자원 — 이 IP 의 레지스터 블록을 매핑한다.
	 * Local Management, Root Port config, Address Translation 세 블록이
	 * 하나의 큰 창 안에 오프셋으로 나뉘어 들어 있다.
	 * devm_ 이므로 probe 실패나 remove 때 자동으로 해제된다. */
	pcie->reg_base = devm_platform_ioremap_resource_byname(pdev, "reg");
	/* [한국어] 레지스터 창이 없으면 아무것도 할 수 없다. */
	if (IS_ERR(pcie->reg_base)) {
		/* [한국어] 디바이스 트리에 reg 이름의 자원이 없다는 뜻이므로 원인을 밝혀 준다. */
		dev_err(dev, "missing \"reg\"\n");
		/* [한국어] IS_ERR 로 감싸인 오류 코드를 그대로 꺼내 돌려준다. */
		return PTR_ERR(pcie->reg_base);
	}

	/* [한국어] "cfg" 자원 — 아웃바운드 0번 창이 대응할 CPU 주소 범위.
	 * 여기에 접근하면 config TLP 가 나간다. 크기는 4KB 면 충분하다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	/* [한국어] config 공간 전용 매핑 함수. 일반 ioremap 과 달리 아키텍처에 따라
	 * config 접근에 맞는 메모리 속성으로 매핑한다. */
	rc->cfg_base = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] cfg 자원이 없거나 매핑에 실패한 경우. */
	if (IS_ERR(rc->cfg_base))
		return PTR_ERR(rc->cfg_base);
	/* [한국어] resource 자체도 보관한다. cdns_pcie_host_init_address_translation() 이
	 * 창을 설정할 때 시작 주소를 다시 꺼내 쓴다. */
	rc->cfg_res = res;

	/* [한국어] 먼저 링크를 올린다. 주소 변환보다 링크가 앞서는 이유는,
	 * config 접근이 링크 위에서 이뤄지므로 링크가 준비되어야 열거가 되기 때문이다.
	 * 링크가 안 올라와도 이 함수는 0 을 돌려준다(핫플러그 대비). */
	ret = cdns_pcie_host_link_setup(rc);
	if (ret)
		return ret;

	/* [한국어] 세 인바운드 창을 모두 "비어 있음"으로 표시한다.
	 * 공통 코드가 dma-ranges 를 배정할 때 이 표를 보고 고른다.
	 * pci-j721e.c 의 resume 경로도 cdns_pcie_host_init() 전에 같은 초기화를 한다. */
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++)
		/* [한국어] RP_BAR0, RP_BAR1, RP_NO_BAR 세 자리를 모두 true 로. */
		rc->avail_ib_bar[bar] = true;

	/* [한국어] 루트 포트 config space 와 주소 변환 창을 설정한다. */
	ret = cdns_pcie_host_init(rc);
	if (ret)
		return ret;

	/* [한국어] SoC 드라이버가 이미 자기 pci_ops 를 꽂아 두었으면 그것을 존중한다.
	 * 예: pci-j721e.c 의 j721e_pcie_ops 는 cdns_ti_pcie_config_read/write 를
	 * 쓰는데, 그 함수는 루트 버스(루트 포트 자신)에 대해서만
	 * pci_generic_config_read32 로 갈아탄다 — 그 영역은 4바이트 단위로만
	 * 접근할 수 있기 때문이다. 그 아래 버스는 표준 구현을 그대로 쓴다. */
	if (!bridge->ops)
		/* [한국어] 비어 있을 때만 이 파일의 기본 표를 꽂는다. */
		bridge->ops = &cdns_pcie_host_ops;

	/* [한국어] PCI 코어에 브리지를 등록한다. 이 안에서 버스 열거가 일어나고,
	 * cdns_pci_map_bus() 가 반복 호출되며 아래쪽 장치들이 발견된다.
	 * NVMe SSD 가 이 아래에 꽂혀 있다면 이 시점에 발견되어
	 * 나중에 nvme 드라이버의 probe 가 불린다. 다만 그 호출은 PCI 코어가 하는 것이지
	 * 이 파일이 nvme 를 직접 부르는 것은 아니다. */
	return pci_host_probe(bridge);
}
/* [한국어] SoC 별 드라이버들이 probe 에서 부르는 진입점이므로 내보낸다.
 * 실제 사용처: pci-j721e.c, pcie-sg2042.c, pcie-cadence-plat.c. */
EXPORT_SYMBOL_GPL(cdns_pcie_host_setup);

/* [한국어] GPL 모듈로 선언한다. EXPORT_SYMBOL_GPL 로 내보낸 심볼을 쓰려면
 * 사용하는 쪽도 GPL 이어야 한다. */
MODULE_LICENSE("GPL");
/* [한국어] modinfo 에 보이는 모듈 설명. */
MODULE_DESCRIPTION("Cadence PCIe host controller driver");
/* [한국어] 원저자 표기. */
MODULE_AUTHOR("Cyrille Pitchen <cyrille.pitchen@free-electrons.com>");
