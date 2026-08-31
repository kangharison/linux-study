/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2017 Cadence
// Cadence PCIe controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

/*
 * [한국어 설명] cadence/ 전체가 공유하는 자료구조와 접근자 (pcie-cadence.h)
 *
 * === 파일의 역할 ===
 * cadence/ 디렉터리의 모든 파일이 include 하는 중심 헤더다. 여기에
 * 세 가지가 모여 있다.
 *   구조체 — struct cdns_pcie(공통), _rc(호스트), _ep(엔드포인트)
 *   레지스터 접근자 — 구형(LGA)과 신형(HPA) 두 벌의 인라인 함수
 *   디스패처 — ops 가 있으면 그것을, 없으면 기본 동작을 쓰는 래퍼들
 *
 * 이 헤더를 읽을 때 핵심은 **구형과 신형이 나란히 존재한다**는 점이다.
 * Cadence 가 IP 를 개정하며 레지스터 맵을 새로 짰고, 커널은 둘을
 * LGA(구형)와 HPA(신형)로 구분해 접근자를 두 벌 둔다.
 *   cdns_pcie_readl / writel        — 구형. 오프셋 하나로 접근.
 *   cdns_pcie_hpa_readl / writel    — 신형. 뱅크를 함께 지정.
 * 신형이 뱅크를 요구하는 이유는 레지스터가 기능별로 나뉘어 각 뱅크의
 * 시작 오프셋이 SoC 마다 다를 수 있기 때문이며, 그 오프셋 표가
 * struct cdns_plat_pcie_of_data 다.
 *
 * 또 하나 중요한 것이 상속 구조다. struct cdns_pcie 를 struct cdns_pcie_rc
 * 와 _ep 가 각각 맨 앞에 통째로 품는다. 그래서 공통 코드는 cdns_pcie 만
 * 받고, 호스트·엔드포인트 코드는 container_of 로 바깥을 되짚는다.
 * SoC 별 드라이버는 다시 그 _rc 나 _ep 를 품는 식으로 세 겹이 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더를 include 하는 곳: cadence/ 의 모든 .c 파일.
 * 이 헤더가 include 하는 것:
 *   pcie-cadence-lga-regs.h — 구형 레지스터 상수
 *   pcie-cadence-hpa-regs.h — 신형 레지스터 상수
 * 즉 두 레지스터 맵이 이 헤더를 통해 함께 딸려 온다. 한 빌드에
 * 둘 다 들어가는 셈인데, 상수 이름이 겹치지 않게(HPA 접두어) 지어
 * 두어 충돌하지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: cadence/ 의 모든 파일.
 * 아래쪽: 커널의 readl/writel, PCI 코어의 타입, endpoint 프레임워크의
 *   struct pci_epf_bar, PHY 서브시스템의 struct phy.
 * 공유 상태: 여기 정의된 세 구조체가 cadence/ 전체의 공유 상태다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 헤더를 include 하지 않고 여기 선언된 함수도
 * 부르지 않는다(drivers/nvme 트리 전수 확인 — cdns_ 심볼 호출 0건).
 * 관계는 토폴로지상의 것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * enum cdns_pcie_rp_bar     : 루트 포트의 인바운드 BAR 번호.
 * enum cdns_pcie_reg_bank   : 신형의 레지스터 뱅크 종류.
 * struct cdns_pcie_ops      : SoC 별로 갈아 끼우는 콜백 넷.
 * struct cdns_plat_pcie_of_data : 뱅크별 시작 오프셋 표.
 * struct cdns_pcie          : 공통 상태. 다른 둘이 이것을 품는다.
 * struct cdns_pcie_rc       : 호스트 모드 상태.
 * struct cdns_pcie_ep       : 엔드포인트 모드 상태.
 * struct cdns_pcie_epf      : 엔드포인트 함수 하나의 BAR 정보.
 * cdns_pcie_readl / writel 계열   : 구형 레지스터 접근.
 * cdns_pcie_hpa_readl / writel 계열 : 신형 레지스터 접근.
 * cdns_pcie_rp_ 계열 / cdns_pcie_ep_fn_ 계열 : 루트 포트와 엔드포인트
 *                             함수의 config space 접근 편의 함수.
 * cdns_pcie_start_link() / _stop_link() / _link_up() : ops 디스패처.
 */

/* [한국어] 헤더 중복 포함 방지 가드. */
#ifndef _PCIE_CADENCE_H
#define _PCIE_CADENCE_H

/* [한국어] 기본 커널 매크로. */
#include <linux/kernel.h>
/* [한국어] 모듈 관련 매크로. 이 헤더의 인라인 함수들이 여러 모듈에서
 * 쓰이므로 함께 포함한다. */
#include <linux/module.h>
/* [한국어] struct pci_host_bridge, PCI_STD_NUM_BARS 등. */
#include <linux/pci.h>
/* [한국어] struct pci_epf_bar. 아래 struct cdns_pcie_epf 가 쓴다.
 * 엔드포인트 모드를 지원하므로 endpoint 프레임워크의 타입이 필요하다. */
#include <linux/pci-epf.h>
/* [한국어] struct phy 와 struct device_link. PHY 배열을 담기 위해서다. */
#include <linux/phy/phy.h>
/* [한국어] 구형(LGA) 레지스터 오프셋과 비트 상수. */
#include "pcie-cadence-lga-regs.h"
/* [한국어] 신형(HPA) 레지스터 오프셋과 비트 상수.
 * 두 맵이 한 빌드에 함께 들어가지만 상수 이름에 HPA 접두어가 붙어
 * 충돌하지 않는다. */
#include "pcie-cadence-hpa-regs.h"

/* [한국어] 루트 포트의 인바운드 BAR 번호.
 * RP_BAR_UNDEFINED 가 -1 인 것이 요점이다. BAR 고르기 함수들
 * (cdns_pcie_host_find_min_bar 등)이 "찾지 못함" 을 이 값으로 알린다.
 * RP_NO_BAR 는 실제 BAR 가 아니라 "BAR 를 거치지 않고 통과시킨다" 는
 * 특별 항목으로, bar_max_size 에서 2^63 이라는 사실상 무제한 크기를
 * 갖는다. */
enum cdns_pcie_rp_bar {
	RP_BAR_UNDEFINED = -1,
	RP_BAR0,
	RP_BAR1,
	RP_NO_BAR
};

/* [한국어] 인바운드 BAR 하나의 상태를 담는 구조체.
 * 다만 확인해 보면 cadence 트리 안에서 이 타입을 쓰는 곳을 찾지 못했다 —
 * 실제 가용 여부는 struct cdns_pcie_rc 의 avail_ib_bar[] 불리언 배열로
 * 관리한다. 옛 설계의 흔적으로 보이나 근거를 확인하지는 못했다. */
struct cdns_pcie_rp_ib_bar {
	u64 size;
	/* [한국어] 이 BAR 의 크기.
	 * 설정자/읽는 자: 이 트리에서 찾지 못했다.
	 * 값 범위: 바이트 단위 크기.
	 * 동기화: 쓰이지 않으므로 해당 없음. */

	bool free;
	/* [한국어] 이 BAR 가 비어 있는지.
	 * 설정자/읽는 자: 이 트리에서 찾지 못했다.
	 * 값 범위: true/false.
	 * 동기화: 위와 같다. */
};

/* [한국어] 전방 선언. 아래 ops 의 함수 포인터들이 이 타입의 포인터를
 * 인자로 받는데, 구조체 정의는 그보다 아래에 있기 때문이다. */
struct cdns_pcie;
struct cdns_pcie_rc;

/* [한국어] 신형(HPA) 레지스터의 뱅크 종류.
 * 신형은 레지스터를 기능별로 나눠 두었고, 각 뱅크의 시작 오프셋이
 * SoC 마다 다를 수 있어 이 열거값으로 지정한다.
 *   REG_BANK_RP              — 루트 포트의 config space
 *   REG_BANK_IP_REG          — IP 자체의 제어·상태
 *   REG_BANK_IP_CFG_CTRL_REG — IP 설정 제어(BAR 구성 등)
 *   REG_BANK_AXI_MASTER      — 인바운드(PCIe → AXI) 주소 변환
 *   REG_BANK_AXI_SLAVE       — 아웃바운드(AXI → PCIe) 주소 변환
 * 나머지(COMMON, HLS, RAS, DTI)는 이 트리의 코드에서 쓰는 곳을
 * 찾지 못했다.
 * REG_BANKS_MAX 는 개수를 나타내는 관용적 마지막 항목이다. */
enum cdns_pcie_reg_bank {
	REG_BANK_RP,
	REG_BANK_IP_REG,
	REG_BANK_IP_CFG_CTRL_REG,
	REG_BANK_AXI_MASTER_COMMON,
	REG_BANK_AXI_MASTER,
	REG_BANK_AXI_SLAVE,
	REG_BANK_AXI_HLS,
	REG_BANK_AXI_RAS,
	REG_BANK_AXI_DTI,
	REG_BANKS_MAX,
};

/* [한국어] SoC 별로 갈아 끼우는 콜백 넷.
 * 아래 디스패처들(cdns_pcie_start_link 등)이 이 표를 보고, 콜백이
 * 있으면 그것을 없으면 기본 동작을 쓴다. */
struct cdns_pcie_ops {
	int     (*start_link)(struct cdns_pcie *pcie);
	/* [한국어] 링크 트레이닝을 시작한다.
	 * 설정자: SoC 드라이버가 자기 ops 표에 채운다.
	 * 읽는 자: cdns_pcie_start_link() 디스패처.
	 * 값 범위: NULL 이면 기본 동작(아무것도 하지 않고 0 반환).
	 * 동기화: 초기화 중에만 불리므로 별도 보호가 없다. */

	void    (*stop_link)(struct cdns_pcie *pcie);
	/* [한국어] 링크를 내린다.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: cdns_pcie_stop_link() 디스패처.
	 * 값 범위: NULL 이면 아무것도 하지 않는다.
	 * 동기화: 위와 같다. */

	bool    (*link_up)(struct cdns_pcie *pcie);
	/* [한국어] 링크가 올라왔는지 판정한다.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: cdns_pcie_link_up() 디스패처.
	 * 값 범위: NULL 이면 IP 의 표준 방식(cdns_pcie_linkup)을 쓴다.
	 * 참고: 이 디스패처를 실제로 넘기는 곳은 구형 경로인
	 *   pcie-cadence-host.c 하나뿐이다. 신형(HPA) 경로는
	 *   cdns_pcie_hpa_link_up() 을 직접 넘기므로 이 콜백을 거치지
	 *   않는다 — pci-sky1.c 가 이 콜백을 채우지만 쓰이지 않는 이유다.
	 * 동기화: 링크 대기 루프에서 반복 호출되며 별도 보호가 없다. */

	u64     (*cpu_addr_fixup)(struct cdns_pcie *pcie, u64 cpu_addr);
	/* [한국어] CPU 물리 주소를 이 보드의 버스 주소로 보정한다.
	 * SoC 마다 주소 배치가 달라 상위 비트를 잘라 내야 하는 경우가 있다
	 * (pcie-cadence-plat.c 의 CDNS_PLAT_CPU_TO_BUS_ADDR 참고).
	 * 설정자: SoC 드라이버.
	 * 읽는 자: cdns_pcie_set_outbound_region() 계열이 창을 설정할 때.
	 * 값 범위: NULL 이면 보정 없이 그대로 쓴다.
	 * 동기화: 초기화 중에만 불린다. */
};

/**
 * struct cdns_plat_pcie_of_data - Register bank offset for a platform
 * @is_rc: controller is a RC
 * @ip_reg_bank_offset: ip register bank start offset
 * @ip_cfg_ctrl_reg_offset: ip config control register start offset
 * @axi_mstr_common_offset: AXI master common register start offset
 * @axi_slave_offset: AXI slave start offset
 * @axi_master_offset: AXI master start offset
 * @axi_hls_offset: AXI HLS offset start
 * @axi_ras_offset: AXI RAS offset
 * @axi_dti_offset: AXI DTI offset
 */
/* [한국어] (위 상류 kernel-doc 이 각 필드의 이름을 설명한다)
 *
 * 이 구조체가 두 가지 역할을 겸한다는 점이 헷갈리기 쉽다.
 *   pcie-cadence-plat.c 는 이것을 compatible 매칭 데이터로 써서
 *     is_rc 만 본다(RC 모드인지 EP 모드인지).
 *   신형(HPA) 접근자들은 이것을 뱅크 오프셋 표로 써서 나머지 필드를 본다.
 * 즉 한 구조체가 서로 다른 두 문맥에서 다른 필드만 쓰인다.
 */
struct cdns_plat_pcie_of_data {
	u32 is_rc:1;
	/* [한국어] 이 컨트롤러를 루트 컴플렉스로 쓰는지.
	 * 설정자: pcie-cadence-plat.c 의 of_match 표가 compatible 마다
	 *   정적으로 지정한다(cdns_plat_pcie_host_of_data 등).
	 * 읽는 자: cdns_plat_pcie_probe() 가 RC/EP 갈래를 정할 때.
	 *   pci-sky1.c 는 이 필드를 채우지 않아 0 으로 남는데, 그 값을
	 *   읽는 코드를 이 트리에서 찾지 못했다.
	 * 값 범위: 0(EP) 또는 1(RC).
	 * 동기화: 정적 데이터라 변경되지 않는다. */

	u32 ip_reg_bank_offset;
	/* [한국어] REG_BANK_IP_REG 뱅크의 시작 오프셋.
	 * 설정자: SoC 드라이버가 자기 오프셋 표에 채운다.
	 * 읽는 자: cdns_pcie_hpa_readl/writel 이 뱅크를 주소로 바꿀 때.
	 * 값 범위: 레지스터 베이스로부터의 바이트 오프셋.
	 * 동기화: 정적 데이터. */

	u32 ip_cfg_ctrl_reg_offset;
	/* [한국어] REG_BANK_IP_CFG_CTRL_REG 뱅크의 시작 오프셋.
	 * BAR 구성 레지스터가 이 뱅크에 있다.
	 * 설정자/읽는 자/값 범위/동기화: 위와 같다. */

	u32 axi_mstr_common_offset;
	/* [한국어] REG_BANK_AXI_MASTER_COMMON 뱅크의 오프셋.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: 뱅크 변환. 다만 이 뱅크를 실제로 쓰는 코드는
	 *   이 트리에서 찾지 못했다.
	 * 값 범위/동기화: 위와 같다. */

	u32 axi_slave_offset;
	/* [한국어] REG_BANK_AXI_SLAVE 뱅크의 오프셋. 아웃바운드 주소 변환
	 * 레지스터가 여기 있다 — CPU 가 AXI 슬레이브로 접근하면 PCIe 로
	 * 나가는 방향이라 이 이름이다.
	 * 설정자/읽는 자/값 범위/동기화: 위와 같다. */

	u32 axi_master_offset;
	/* [한국어] REG_BANK_AXI_MASTER 뱅크의 오프셋. 인바운드 주소 변환
	 * 레지스터가 여기 있다 — 들어온 PCIe 요청을 IP 가 AXI 마스터로서
	 * 내보내는 방향이다.
	 * 설정자/읽는 자/값 범위/동기화: 위와 같다. */

	u32 axi_hls_offset;
	/* [한국어] REG_BANK_AXI_HLS 뱅크의 오프셋.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: 이 뱅크를 쓰는 코드를 이 트리에서 찾지 못했다.
	 *   HLS 의 원어도 확인하지 못했다.
	 * 값 범위/동기화: 위와 같다. */

	u32 axi_ras_offset;
	/* [한국어] REG_BANK_AXI_RAS 뱅크의 오프셋. RAS 는 통상
	 * Reliability/Availability/Serviceability 를 뜻하지만, 이 트리에서
	 * 그 확인이나 이 뱅크를 쓰는 코드를 찾지 못했다.
	 * 설정자/값 범위/동기화: 위와 같다. */

	u32 axi_dti_offset;
	/* [한국어] REG_BANK_AXI_DTI 뱅크의 오프셋.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: 이 뱅크를 쓰는 코드도 DTI 의 원어도 확인하지 못했다.
	 * 값 범위/동기화: 위와 같다. */
};

/**
 * struct cdns_pcie - private data for Cadence PCIe controller drivers
 * @reg_base: IO mapped register base
 * @mem_res: start/end offsets in the physical system memory to map PCI accesses
 * @msg_res: Region for send message to map PCI accesses
 * @dev: PCIe controller
 * @is_rc: tell whether the PCIe controller mode is Root Complex or Endpoint.
 * @phy_count: number of supported PHY devices
 * @phy: list of pointers to specific PHY control blocks
 * @link: list of pointers to corresponding device link representations
 * @ops: Platform-specific ops to control various inputs from Cadence PCIe
 *       wrapper
 * @cdns_pcie_reg_offsets: Register bank offsets for different SoC
 */
struct cdns_pcie {
	void __iomem		             *reg_base;
	/* [한국어] 이 IP 의 레지스터가 매핑된 커널 가상 주소.
	 * 모든 레지스터 접근이 여기서 출발한다. 신형은 여기에 뱅크
	 * 오프셋을 더하고, 구형은 오프셋을 직접 더한다.
	 * 설정자: 호스트/EP setup 이 devm_platform_ioremap_resource 로
	 *   매핑하거나, SoC 드라이버가 미리 채워 둔다.
	 * 읽는 자: 모든 레지스터 접근자.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 초기화 후 불변이라 보호가 필요 없다. */

	struct resource		             *mem_res;
	/* [한국어] 이 컨트롤러가 PCIe 접근에 쓸 물리 주소 범위.
	 * 아웃바운드 창이 이 안에 놓인다.
	 * 설정자: 호스트/EP setup 이 디바이스 트리에서 얻는다.
	 * 읽는 자: 엔드포인트 코드가 아웃바운드 창 주소를 계산할 때.
	 * 값 범위: 유효한 자원 포인터.
	 * 동기화: 초기화 후 불변. */

	struct resource                      *msg_res;
	/* [한국어] 메시지 TLP 전용 창에 쓸 주소 범위.
	 * INTx 나 전원 관리 이벤트를 보내려면 필요하다. 디바이스 트리에
	 * 없으면 NULL 이고, 그때는 메시지 창을 만들지 않는다.
	 * 설정자: 호스트 setup 이 디바이스 트리에서 얻는다.
	 * 읽는 자: cdns_pcie_hpa_host_init_address_translation() 등이
	 *   NULL 여부를 보고 메시지 창 생성을 정한다.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: 초기화 후 불변. */

	struct device		             *dev;
	/* [한국어] 이 컨트롤러의 device. 오류 메시지와 devres, 디바이스
	 * 트리 노드 접근에 두루 쓰인다.
	 * 설정자: SoC 드라이버가 probe 에서 채운다.
	 * 읽는 자: 거의 모든 함수.
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: 초기화 후 불변. */

	bool			             is_rc;
	/* [한국어] 이 IP 를 호스트로 쓰는지 엔드포인트로 쓰는지.
	 * 아웃바운드 창 설정이 이 값을 보고 requester ID 를 소프트웨어가
	 * 채울지(RC) 하드웨어에 맡길지(EP) 정한다.
	 * 설정자: cdns_pcie_host_setup() 이 true 로, EP setup 이 false 로.
	 * 읽는 자: cdns_pcie_set_outbound_region() 계열.
	 * 값 범위: true(RC) / false(EP).
	 * 동기화: 초기화 후 불변. */

	int			             phy_count;
	/* [한국어] 이 컨트롤러가 쓰는 PHY 개수. 레인마다 하나인 구성이 흔하다.
	 * 설정자: cdns_pcie_init_phy() 가 디바이스 트리의 phy-names 개수로 채운다.
	 * 읽는 자: enable_phy / disable_phy 의 순회 상한.
	 * 값 범위: 0 이상. 0 이면 PHY 가 컨트롤러에 통합된 SoC 다.
	 * 동기화: 초기화 후 불변. */

	struct phy		             **phy;
	/* [한국어] PHY 핸들 배열. 개수를 실행 시점에 알아 동적으로 잡는다.
	 * 설정자: cdns_pcie_init_phy() 가 devm_kcalloc 로 잡아 채운다.
	 * 읽는 자: enable_phy / disable_phy.
	 * 값 범위: phy_count 개의 유효한 포인터. phy_count 가 0 이면 NULL.
	 * 동기화: 초기화 후 불변. */

	struct device_link	             **link;
	/* [한국어] 각 PHY 에 건 device link 배열. 절전·제거 순서를 커널이
	 * 지켜 주게 하는 장치다.
	 * 설정자: cdns_pcie_init_phy().
	 * 읽는 자: 정리 경로가 device_link_del 로 끊을 때.
	 * 값 범위: phy 배열과 같은 개수.
	 * 동기화: 초기화 후 불변. */

	const  struct cdns_pcie_ops          *ops;
	/* [한국어] SoC 별 콜백 표. 위 struct cdns_pcie_ops 참고.
	 * 설정자: SoC 드라이버가 probe 에서 채운다.
	 * 읽는 자: 아래 디스패처들과 아웃바운드 창 설정.
	 * 값 범위: NULL 일 수 있다. 디스패처들은 ops 자체의 NULL 을
	 *   확인하지 않으므로, 이 IP 를 쓰는 드라이버는 반드시 채워야 한다.
	 *   확인해 보면 cadence/ 의 모든 SoC 드라이버가 채운다.
	 * 동기화: 초기화 후 불변. */

	const  struct cdns_plat_pcie_of_data *cdns_pcie_reg_offsets;
	/* [한국어] 신형(HPA) 뱅크 오프셋 표.
	 * 설정자: 신형 IP 를 쓰는 SoC 드라이버(pci-sky1.c)가 채운다.
	 * 읽는 자: cdns_pcie_hpa_readl/writel 이 뱅크를 주소로 바꿀 때.
	 * 값 범위: 구형 IP 만 쓰는 드라이버에서는 NULL 로 남는다 —
	 *   그쪽은 뱅크 개념이 없어 이 표를 보지 않는다.
	 * 동기화: 초기화 후 불변. */
};

/**
 * struct cdns_pcie_rc - private data for this PCIe Root Complex driver
 * @pcie: Cadence PCIe controller
 * @cfg_res: start/end offsets in the physical system memory to map PCI
 *           configuration space accesses
 * @cfg_base: IO mapped window to access the PCI configuration space of a
 *            single function at a time
 * @vendor_id: PCI vendor ID
 * @device_id: PCI device ID
 * @avail_ib_bar: Status of RP_BAR0, RP_BAR1 and RP_NO_BAR if it's free or
 *                available
 * @quirk_retrain_flag: Retrain link as quirk for PCIe Gen2
 * @quirk_detect_quiet_flag: LTSSM Detect Quiet min delay set as quirk
 * @ecam_supported: Whether the ECAM is supported
 * @no_inbound_map: Whether inbound mapping is supported
 * @quirk_broken_aspm_l0s: Disable ASPM L0s support as quirk
 * @quirk_broken_aspm_l1: Disable ASPM L1 support as quirk
 */
struct cdns_pcie_rc {
	struct cdns_pcie	pcie;
	/* [한국어] 공통 컨트롤러 상태를 통째로 품는다. 맨 앞에 두어야
	 * container_of 없이도 &rc->pcie 로 오갈 수 있고, 반대로
	 * pci_host_bridge_priv() 가 돌려준 포인터를 그대로 rc 로 쓸 수 있다.
	 * 설정자: SoC 드라이버와 cdns_pcie_host_setup().
	 * 읽는 자: cadence/ 의 거의 모든 코드.
	 * 값 범위: 위 struct cdns_pcie 참고.
	 * 동기화: 초기화 후 대부분 불변. */

	struct resource		*cfg_res;
	/* [한국어] config space 접근에 쓸 물리 주소 범위.
	 * 설정자: host_setup 이 디바이스 트리의 "cfg" 자원에서 얻는다.
	 * 읽는 자: create_region_for_cfg 가 그 시작 주소로 config 창을 만든다.
	 * 값 범위: 유효한 자원 포인터. ECAM 을 쓰면 SoC 드라이버가 미리 채운다.
	 * 동기화: 초기화 후 불변. */

	void __iomem		*cfg_base;
	/* [한국어] 그 범위를 매핑한 커널 가상 주소.
	 * map_bus 가 여기에 오프셋을 더해 돌려주고, PCI 코어가 그 주소로
	 * 실제 config 읽기·쓰기를 한다.
	 * 설정자: host_setup 이 devm_pci_remap_cfg_resource 로 매핑하거나
	 *   SoC 드라이버가 미리 채운다.
	 * 읽는 자: cdns_pci_map_bus() 와 cdns_pci_hpa_map_bus().
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 초기화 후 불변. 다만 그 주소로 하는 접근은 pci_lock 아래다. */

	u32			vendor_id;
	/* [한국어] 루트 포트의 config 헤더에 적을 벤더 ID.
	 * 설정자: SoC 드라이버가 디바이스 트리에서 읽어 채운다.
	 * 읽는 자: host_init_root_port 가 config 에 쓸 때.
	 * 값 범위: 0xffff 는 "지정하지 않음" 을 뜻해 IP 기본값을 그대로 둔다.
	 * 동기화: 초기화 후 불변. */

	u32			device_id;
	/* [한국어] 마찬가지로 디바이스 ID.
	 * 설정자/읽는 자/값 범위/동기화: 위와 같다. */

	bool			avail_ib_bar[CDNS_PCIE_RP_MAX_IB];
	/* [한국어] 인바운드 BAR 가 아직 비어 있는지를 나타내는 표.
	 * dma-ranges 를 BAR 에 배정하는 알고리즘이 이 표를 보고 고르며,
	 * 배정할 때마다 해당 항목을 false 로 바꾼다.
	 * 설정자: host_setup 이 전부 true 로 초기화하고,
	 *   bar_ib_config 가 하나씩 false 로 바꾼다.
	 * 읽는 자: cdns_pcie_host_find_min_bar() / _find_max_bar().
	 * 값 범위: RP_BAR0, RP_BAR1, RP_NO_BAR 세 자리.
	 * 동기화: 초기화 중에만 다뤄지므로 별도 보호가 없다. */

	unsigned int		quirk_retrain_flag:1;
	/* [한국어] Gen2 트레이닝 결함이 있는 보드인지.
	 * 켜져 있으면 cdns_pcie_host_start_link() 가 링크가 올라온 뒤
	 * 재트레이닝을 걸어 제 속도를 되찾는다.
	 * 설정자: SoC 드라이버가 자기 하드웨어를 알고 세운다.
	 * 읽는 자: cdns_pcie_host_start_link().
	 * 값 범위: 0/1. 참고로 신형(HPA) 경로는 start_link 를 거치지 않아
	 *   이 플래그를 보지 않는다.
	 * 동기화: 초기화 후 불변. */

	unsigned int		quirk_detect_quiet_flag:1;
	/* [한국어] LTSSM 의 Detect.Quiet 최소 대기를 늘려야 하는 보드인지.
	 * 그 구간이 짧으면 안정되지 않은 신호를 오인해 트레이닝이 실패한다.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: 구형·신형 양쪽의 link_setup.
	 * 값 범위: 0/1.
	 * 동기화: 초기화 후 불변. */

	unsigned int            ecam_supported:1;
	/* [한국어] 이 보드가 ECAM 으로 config 에 접근하는지.
	 * 참이면 config space 전체가 이미 메모리에 펼쳐져 있어, 아웃바운드
	 * 창을 config 용으로 잡아 돌려 쓰지 않아도 된다 — 접근마다 창을
	 * 고치는 MMIO 가 사라져 훨씬 빠르다.
	 * 설정자: SoC 드라이버(pci-sky1.c).
	 * 읽는 자: cdns_pcie_hpa_host_init_address_translation().
	 * 값 범위: 0/1.
	 * 동기화: 초기화 후 불변. */

	unsigned int            no_inbound_map:1;
	/* [한국어] 인바운드 매핑을 하지 않는 구성인지.
	 * 참이면 dma-ranges 를 BAR 에 배정하지 않는다. 그런 보드에서
	 * 장치 DMA 가 어떻게 처리되는지는 이 트리에서 확인하지 못했다.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: host_init_address_translation.
	 * 값 범위: 0/1.
	 * 동기화: 초기화 후 불변. */

	unsigned int            quirk_broken_aspm_l0s:1;
	/* [한국어] 이 보드의 ASPM L0s 가 망가져 쓰면 안 되는지.
	 * 설정자: SoC 드라이버(pcie-sg2042.c 가 이것과 아래를 둘 다 세운다).
	 * 읽는 자: 이 트리에서 이 필드를 읽는 코드를 찾지 못했다 —
	 *   PCI 코어나 다른 계층이 읽을 가능성이 있으나 확인하지 못했다.
	 * 값 범위: 0/1.
	 * 동기화: 초기화 후 불변. */

	unsigned int            quirk_broken_aspm_l1:1;
	/* [한국어] 마찬가지로 L1 이 망가진 경우.
	 * 설정자/읽는 자/값 범위/동기화: 위와 같다. */
};

/**
 * struct cdns_pcie_epf - Structure to hold info about endpoint function
 * @epf: Info about virtual functions attached to the physical function
 * @epf_bar: reference to the pci_epf_bar for the six Base Address Registers
 */
struct cdns_pcie_epf {
	struct cdns_pcie_epf *epf;
	/* [한국어] 이 물리 함수에 딸린 가상 함수(VF)들의 정보 배열.
	 * SR-IOV 를 쓸 때 PF 하나가 VF 여럿을 갖는 구조를 표현한다.
	 * 설정자: 엔드포인트 setup 이 VF 개수만큼 잡아 연결한다.
	 * 읽는 자: BAR 설정 함수가 VF 번호로 인덱싱할 때.
	 * 값 범위: VF 가 없으면 NULL.
	 * 동기화: 초기화 후 불변. */

	struct pci_epf_bar *epf_bar[PCI_STD_NUM_BARS];
	/* [한국어] 이 함수의 BAR 여섯 개 각각에 대한 정보.
	 * 엔드포인트 프레임워크의 struct pci_epf_bar 를 가리키며,
	 * BAR 를 해제할 때 그 크기와 주소를 되찾는 데 쓴다.
	 * 설정자: cdns_pcie_ep_set_bar() 가 설정 시 기록한다.
	 * 읽는 자: cdns_pcie_ep_clear_bar() 가 해제할 때.
	 * 값 범위: 설정되지 않은 BAR 는 NULL.
	 * 동기화: EPC 코어의 epc->lock 뮤텍스 아래에서 다뤄진다. */
};

/**
 * struct cdns_pcie_ep - private data for this PCIe endpoint controller driver
 * @pcie: Cadence PCIe controller
 * @max_regions: maximum number of regions supported by hardware
 * @ob_region_map: bitmask of mapped outbound regions
 * @ob_addr: base addresses in the AXI bus where the outbound regions start
 * @irq_phys_addr: base address on the AXI bus where the MSI/INTX IRQ
 *		   dedicated outbound regions is mapped.
 * @irq_cpu_addr: base address in the CPU space where a write access triggers
 *		  the sending of a memory write (MSI) / normal message (INTX
 *		  IRQ) TLP through the PCIe bus.
 * @irq_pci_addr: used to save the current mapping of the MSI/INTX IRQ
 *		  dedicated outbound region.
 * @irq_pci_fn: the latest PCI function that has updated the mapping of
 *		the MSI/INTX IRQ dedicated outbound region.
 * @irq_pending: bitmask of asserted INTX IRQs.
 * @lock: spin lock to disable interrupts while modifying PCIe controller
 *        registers fields (RMW) accessible by both remote RC and EP to
 *        minimize time between read and write
 * @epf: Structure to hold info about endpoint function
 * @quirk_detect_quiet_flag: LTSSM Detect Quiet min delay set as quirk
 * @quirk_disable_flr: Disable FLR (Function Level Reset) quirk flag
 */
struct cdns_pcie_ep {
	struct cdns_pcie	pcie;
	/* [한국어] 공통 컨트롤러 상태. rc 와 마찬가지로 맨 앞에 품는다.
	 * 설정자: SoC 드라이버와 cdns_pcie_ep_setup().
	 * 읽는 자: cadence/ 의 공통 코드.
	 * 값 범위/동기화: struct cdns_pcie 참고. */

	u32			max_regions;
	/* [한국어] 이 하드웨어가 가진 아웃바운드 창의 개수.
	 * 설정자: ep_setup 이 디바이스 트리에서 읽는다.
	 * 읽는 자: cdns_pcie_ep_map_addr() 이 상한 검사에 쓴다.
	 *   다만 그 검사가 r >= max_regions - 1 이라, 0번(IRQ 예약) 외에
	 *   마지막 한 창이 더 버려진다. 그 이유는 확인하지 못했다.
	 * 값 범위: 하드웨어에 따라 다르다.
	 * 동기화: 초기화 후 불변. */

	unsigned long		ob_region_map;
	/* [한국어] 어느 아웃바운드 창이 쓰이는지를 나타내는 비트맵.
	 * 설정자: map_addr 이 창을 잡을 때 세우고 unmap_addr 이 지운다.
	 * 읽는 자: 빈 창을 찾는 코드.
	 * 값 범위: 비트 하나가 창 하나. unsigned long 이라 32/64개가 상한이다.
	 * 동기화: EPC 코어의 뮤텍스 아래에서 다뤄진다. */

	phys_addr_t		*ob_addr;
	/* [한국어] 각 아웃바운드 창이 대응하는 AXI 물리 주소의 배열.
	 * 창 번호로 인덱싱해 그 창의 CPU 쪽 시작 주소를 얻는다.
	 * 설정자: ep_setup 이 max_regions 개만큼 잡아 채운다.
	 * 읽는 자: map_addr 이 호출자에게 돌려줄 주소를 계산할 때.
	 * 값 범위: max_regions 개의 물리 주소.
	 * 동기화: 초기화 후 불변. */

	phys_addr_t		irq_phys_addr;
	/* [한국어] MSI/INTx 전용 아웃바운드 창의 AXI 물리 주소.
	 * 인터럽트를 올리려면 이 주소에 쓰는데, 그 접근이 PCIe TLP 가 되어
	 * 호스트에 전달된다.
	 * 설정자: ep_setup 이 창 0번을 이 용도로 잡으며 채운다.
	 * 읽는 자: 인터럽트를 올리는 함수들.
	 * 값 범위: 유효한 물리 주소.
	 * 동기화: 초기화 후 불변. */

	void __iomem		*irq_cpu_addr;
	/* [한국어] 그 물리 주소를 매핑한 커널 가상 주소.
	 * 상류 kernel-doc 이 밝히듯, 여기에 쓰면 MSI(메모리 쓰기 TLP)나
	 * INTx(일반 메시지 TLP)가 나간다.
	 * 설정자: ep_setup 의 ioremap.
	 * 읽는 자: 인터럽트를 올리는 함수들이 writel 로 쓴다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 초기화 후 불변. */

	u64			irq_pci_addr;
	/* [한국어] 그 IRQ 전용 창이 현재 어느 PCI 주소에 연결되어 있는지.
	 * 창 하나를 여러 목적으로 돌려 쓰므로, 목적지가 바뀌면 창을 다시
	 * 설정해야 한다. 이 값이 "지금 어디로 향해 있는가" 를 기억해
	 * 같은 목적지면 재설정을 건너뛰게 한다.
	 * 설정자/읽는 자: 인터럽트를 올리는 함수들.
	 * 값 범위: PCI 주소, 또는 아직 설정되지 않았음을 뜻하는 약속된 값.
	 * 동기화: 아래 lock 이 보호한다. */

	u8			irq_pci_fn;
	/* [한국어] 그 창을 마지막으로 갱신한 PCI 함수 번호.
	 * 다중 함수 구성에서 어느 함수의 인터럽트를 위해 설정된 창인지
	 * 구분해야 하기 때문이다.
	 * 설정자/읽는 자: 위와 같다.
	 * 값 범위: 함수 번호.
	 * 동기화: 위와 같다. */

	u8			irq_pending;
	/* [한국어] 현재 어서트된 INTx 인터럽트의 비트맵.
	 * INTx 는 레벨 트리거라 어서트와 디어서트를 짝지어야 하며,
	 * 그 상태를 여기 기억한다.
	 * 설정자/읽는 자: INTx 를 올리고 내리는 함수들.
	 * 값 범위: INTA~INTD 에 대응하는 비트들.
	 * 동기화: 아래 lock 이 보호한다. */

	/* protect writing to PCI_STATUS while raising INTX interrupts */
	spinlock_t		lock;
	/* [한국어] 상류 주석이 이 락의 목적을 정확히 밝히고 있다.
	 * INTx 를 올릴 때 PCI_STATUS 레지스터를 읽고-고쳐-쓰는데, 그 사이에
	 * 저쪽 호스트도 같은 레지스터를 건드릴 수 있다. 그래서 읽기와
	 * 쓰기 사이를 최대한 짧게 하려고 인터럽트를 끈 채 처리한다.
	 * 뮤텍스가 아니라 스핀락인 것은 그 구간이 짧고 잠들면 안 되기 때문이다.
	 * 설정자: ep_setup 의 spin_lock_init.
	 * 읽는 자: INTx 관련 함수들.
	 * 값 범위: 스핀락.
	 * 동기화: 이것이 동기화 수단 자체다. */

	struct cdns_pcie_epf	*epf;
	/* [한국어] 이 컨트롤러에 붙은 엔드포인트 함수들의 배열.
	 * 함수 번호로 인덱싱해 그 함수의 BAR 정보를 얻는다.
	 * 설정자: ep_setup 이 max_functions 개만큼 잡는다.
	 * 읽는 자: BAR 설정·해제 함수들.
	 * 값 범위: 유효한 배열 포인터.
	 * 동기화: 초기화 후 배열 자체는 불변. 각 항목은 EPC 뮤텍스 아래. */

	unsigned int		quirk_detect_quiet_flag:1;
	/* [한국어] rc 쪽과 같은 의미. Detect.Quiet 대기를 늘려야 하는 보드인지.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: ep_setup.
	 * 값 범위: 0/1.
	 * 동기화: 초기화 후 불변. */

	unsigned int		quirk_disable_flr:1;
	/* [한국어] FLR(Function Level Reset)을 쓰지 못하는 보드인지.
	 * FLR 은 호스트가 이 함수 하나만 리셋하는 기능인데, 하드웨어
	 * 구현에 문제가 있으면 꺼야 한다.
	 * 설정자: SoC 드라이버.
	 * 읽는 자: ep_setup 이 config 에서 FLR 능력 비트를 지울 때.
	 * 값 범위: 0/1.
	 * 동기화: 초기화 후 불변. */
};

/* [한국어]
 * cdns_reg_bank_to_off - 뱅크 번호를 레지스터 베이스로부터의 오프셋으로 바꾼다
 *
 * @pcie: 대상 컨트롤러. 오프셋 표(cdns_pcie_reg_offsets)를 여기서 얻는다.
 * @bank: 뱅크 종류.
 * @return: 그 뱅크의 시작 오프셋. 모르는 뱅크면 0.
 *
 * 신형(HPA) 레지스터 접근의 첫 단계다. 신형은 레지스터를 기능별
 * 뱅크로 나누어 두었고, 각 뱅크가 어디서 시작하는지는 SoC 마다 다르다.
 * 그래서 SoC 드라이버가 채워 둔 표를 찾아보는 것이다.
 *
 * REG_BANK_RP 만 표를 보지 않고 0 을 쓰는 점이 눈에 띈다. 루트 포트의
 * config 는 레지스터 베이스 바로 그 자리에서 시작한다는 뜻이다.
 *
 * 주의할 점이 하나 있다. 이 함수는 pcie->cdns_pcie_reg_offsets 가
 * NULL 인지 확인하지 않는다. 구형(LGA) IP 만 쓰는 드라이버는 그 표를
 * 채우지 않으므로 NULL 인데, 그런 드라이버는 신형 접근자를 부르지
 * 않으므로 이 함수에 닿지 않는다 — 그 전제 위에서 성립한다.
 *
 * 실행 컨텍스트: 레지스터 접근 경로. 순수 조회.
 *
 * 호출 체인:
 *   cdns_pcie_hpa_readl() / cdns_pcie_hpa_writel() → [이 함수]
 */
static inline u32 cdns_reg_bank_to_off(struct cdns_pcie *pcie, enum cdns_pcie_reg_bank bank)
{
	/* [한국어] 모르는 뱅크가 오면 0 이 그대로 반환된다 — 레지스터
	 * 베이스 자체를 가리키게 되므로 안전한 값은 아니지만, 열거값이
	 * 아닌 것이 들어올 일이 없다는 전제다. */
	u32 offset = 0x0;

	switch (bank) {
	case REG_BANK_RP:
		/* [한국어] 루트 포트 config 는 베이스 그 자리에서 시작한다. */
		offset = 0;
		break;
	case REG_BANK_IP_REG:
		/* [한국어] IP 자체의 제어·상태 레지스터. PHY 디버그 상태와
		 * LTSSM 설정이 여기 있다. */
		offset = pcie->cdns_pcie_reg_offsets->ip_reg_bank_offset;
		break;
	case REG_BANK_IP_CFG_CTRL_REG:
		/* [한국어] IP 설정 제어. BAR 구성 레지스터가 여기 있다. */
		offset = pcie->cdns_pcie_reg_offsets->ip_cfg_ctrl_reg_offset;
		break;
	case REG_BANK_AXI_MASTER_COMMON:
		/* [한국어] 이 뱅크를 실제로 쓰는 코드를 이 트리에서 찾지 못했다. */
		offset = pcie->cdns_pcie_reg_offsets->axi_mstr_common_offset;
		break;
	case REG_BANK_AXI_MASTER:
		/* [한국어] 인바운드 주소 변환. 들어온 PCIe 요청을 IP 가
		 * AXI 마스터로서 내보내는 방향이다. */
		offset = pcie->cdns_pcie_reg_offsets->axi_master_offset;
		break;
	case REG_BANK_AXI_SLAVE:
		/* [한국어] 아웃바운드 주소 변환. CPU 가 AXI 슬레이브로
		 * 접근하면 PCIe 로 나가는 방향이다. */
		offset = pcie->cdns_pcie_reg_offsets->axi_slave_offset;
		break;
	case REG_BANK_AXI_HLS:
		/* [한국어] 쓰는 코드를 찾지 못했다. HLS 의 원어도 확인하지 못했다. */
		offset = pcie->cdns_pcie_reg_offsets->axi_hls_offset;
		break;
	case REG_BANK_AXI_RAS:
		/* [한국어] 쓰는 코드를 찾지 못했다. */
		offset = pcie->cdns_pcie_reg_offsets->axi_ras_offset;
		break;
	case REG_BANK_AXI_DTI:
		/* [한국어] 쓰는 코드도 DTI 의 원어도 확인하지 못했다. */
		offset = pcie->cdns_pcie_reg_offsets->axi_dti_offset;
		break;
	default:
		/* [한국어] REG_BANKS_MAX 나 범위 밖 값. offset 이 0 으로 남는다.
		 * 컴파일러가 열거값 누락 경고를 내지 않도록 두는 관용적 처리다. */
		break;
	}
	return offset;
}

/* Register access */
/* [한국어]
 * cdns_pcie_writel - 구형(LGA) 배치의 32비트 레지스터 쓰기
 *
 * @pcie: 대상 컨트롤러.
 * @reg: 레지스터 베이스로부터의 오프셋.
 * @value: 쓸 값.
 * @return: 없음.
 *
 * 구형은 뱅크 개념이 없어 오프셋을 그대로 더하면 된다. 그래서 이
 * 함수가 한 줄로 끝난다 — 아래 신형 판이 뱅크 변환을 한 단계 더
 * 거치는 것과 대비된다.
 *
 * 실행 컨텍스트: 어디서든. 순수 MMIO 라 잠들지 않는다.
 *
 * 호출 체인:
 *   pcie-cadence.c, pcie-cadence-host.c 등의 구형 경로 → [이 함수]
 */
static inline void cdns_pcie_writel(struct cdns_pcie *pcie, u32 reg, u32 value)
{
	writel(value, pcie->reg_base + reg);
}

/* [한국어]
 * cdns_pcie_readl - 구형(LGA) 배치의 32비트 레지스터 읽기
 *
 * @pcie: 대상 컨트롤러.
 * @reg: 오프셋.
 * @return: 읽은 값.
 *
 * 위 writel 의 짝이다.
 *
 * 실행 컨텍스트: 어디서든. 순수 MMIO.
 *
 * 호출 체인:
 *   구형 경로의 모든 레지스터 읽기 → [이 함수]
 */
static inline u32 cdns_pcie_readl(struct cdns_pcie *pcie, u32 reg)
{
	return readl(pcie->reg_base + reg);
}

/* [한국어]
 * cdns_pcie_hpa_writel - 신형(HPA) 배치의 32비트 레지스터 쓰기
 *
 * @pcie: 대상 컨트롤러.
 * @bank: 어느 뱅크인지.
 * @reg: 그 뱅크 안에서의 오프셋.
 * @value: 쓸 값.
 * @return: 없음.
 *
 * 구형과 다른 점은 뱅크 변환 한 단계뿐이다. 뱅크의 시작 오프셋을
 * 찾아 더한 뒤 평소처럼 쓴다.
 *
 * 인자가 하나 더 붙는 것이 신형 코드를 읽을 때 가장 먼저 눈에 띄는
 * 차이이며, 그 때문에 구형과 신형의 함수를 서로 바꿔 쓸 수 없다.
 *
 * 실행 컨텍스트: 어디서든. 순수 MMIO.
 *
 * 호출 체인:
 *   pcie-cadence-hpa.c, pcie-cadence-host-hpa.c → [이 함수]
 *     → cdns_reg_bank_to_off()
 */
static inline void cdns_pcie_hpa_writel(struct cdns_pcie *pcie,
					enum cdns_pcie_reg_bank bank,
					u32 reg,
					u32 value)
{
	/* [한국어] 이 뱅크가 어디서 시작하는지 찾는다. */
	u32 offset = cdns_reg_bank_to_off(pcie, bank);

	/* [한국어] 그만큼 밀어 최종 오프셋을 만든다. */
	reg += offset;
	writel(value, pcie->reg_base + reg);
}

/* [한국어]
 * cdns_pcie_hpa_readl - 신형(HPA) 배치의 32비트 레지스터 읽기
 *
 * @pcie: 대상 컨트롤러.
 * @bank: 어느 뱅크인지.
 * @reg: 그 뱅크 안에서의 오프셋.
 * @return: 읽은 값.
 *
 * 위 writel 의 짝이다.
 *
 * 실행 컨텍스트: 어디서든. 순수 MMIO.
 *
 * 호출 체인:
 *   신형 경로의 모든 레지스터 읽기 → [이 함수] → cdns_reg_bank_to_off()
 */
static inline u32 cdns_pcie_hpa_readl(struct cdns_pcie *pcie,
				      enum cdns_pcie_reg_bank bank,
				      u32 reg)
{
	u32 offset = cdns_reg_bank_to_off(pcie, bank);

	/* [한국어] 뱅크 오프셋만큼 밀어 최종 오프셋을 만든다. */
	reg += offset;
	return readl(pcie->reg_base + reg);
}

/* [한국어]
 * cdns_pcie_read_sz - 32비트 접근만 되는 레지스터에서 1/2/4바이트를 읽는다
 *
 * @addr: 읽을 주소.
 * @size: 1, 2, 4 중 하나.
 * @return: 읽은 값. 정렬이 맞지 않으면 0.
 *
 * 이 IP 의 config 레지스터는 32비트 단위로만 접근할 수 있다. 그런데
 * PCI config 는 1바이트나 2바이트 읽기를 요구하는 일이 많다
 * (예: PCI_CLASS_REVISION 은 1바이트, PCI_VENDOR_ID 는 2바이트).
 *
 * 그래서 이렇게 한다.
 *   주소를 4바이트 경계로 내려 32비트를 통째로 읽고,
 *   원래 주소가 그 안에서 몇 바이트째였는지 계산해,
 *   그만큼 시프트한 뒤 필요한 폭만 마스크로 잘라 낸다.
 *
 * size > 2 이면 시프트 없이 그대로 돌려주는데, 4바이트 읽기는 이미
 * 정렬되어 있어 offset 이 0 이기 때문이다.
 *
 * 정렬 검사가 읽기 "뒤" 에 오는 점이 눈에 띈다. 이미 readl 을 한
 * 뒤에 검사하므로, 정렬이 어긋난 경우에도 MMIO 접근 자체는 일어난다.
 * 다만 aligned_addr 로 읽으므로 그 접근 자체는 정렬되어 있어
 * 버스 오류가 나지는 않는다.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 MMIO 라 잠들지 않는다.
 *
 * 에러 경로: 정렬 오류는 pr_warn 을 남기고 0 을 돌려준다. 호출자가
 *   정상적으로 읽은 0 과 구분할 수 없다는 점은 알아 두어야 한다.
 *
 * 호출 체인:
 *   cdns_pcie_rp_readw/readl, cdns_pcie_ep_fn_readw 등 → [이 함수]
 */
static inline u32 cdns_pcie_read_sz(void __iomem *addr, int size)
{
	/* [한국어] 4바이트 경계로 내린 주소. 실제 MMIO 는 여기서 한다. */
	void __iomem *aligned_addr = PTR_ALIGN_DOWN(addr, 0x4);
	/* [한국어] 원래 주소가 그 워드 안에서 몇 바이트째인지(0~3). */
	unsigned int offset = (unsigned long)addr & 0x3;
	/* [한국어] 32비트를 통째로 읽어 둔다. */
	u32 val = readl(aligned_addr);

	/* [한국어] 요청한 크기에 맞게 정렬되어 있는지 확인한다.
	 * 2바이트 읽기는 짝수 주소여야 하고 4바이트는 4의 배수여야 한다.
	 * 위에서 이미 읽은 뒤라 이 검사가 MMIO 를 막지는 못하지만,
	 * aligned_addr 로 읽었으므로 그 접근 자체는 안전하다. */
	if (!IS_ALIGNED((uintptr_t)addr, size)) {
		pr_warn("Address %p and size %d are not aligned\n", addr, size);
		return 0;
	}

	/* [한국어] 4바이트 읽기는 offset 이 0 이므로 그대로 돌려준다. */
	if (size > 2)
		return val;

	/* [한국어] 원하는 바이트 위치까지 시프트한 뒤 그 폭만 남긴다.
	 * 예를 들어 offset 2, size 1 이면 val 을 16비트 오른쪽으로 밀고
	 * 하위 8비트만 취한다. */
	return (val >> (8 * offset)) & ((1 << (size * 8)) - 1);
}

/* [한국어]
 * cdns_pcie_write_sz - 32비트 접근만 되는 레지스터에 1/2/4바이트를 쓴다
 *
 * @addr: 쓸 주소.
 * @size: 1, 2, 4 중 하나.
 * @value: 쓸 값.
 * @return: 없음.
 *
 * 위 read_sz 의 짝이지만 쓰기 쪽이 훨씬 까다롭다. 읽기는 통째로 읽어
 * 필요한 부분만 꺼내면 그만이지만, 쓰기는 나머지 바이트를 건드리지
 * 않아야 하기 때문이다.
 *
 * 그래서 읽고-고쳐-쓰기를 한다.
 *   32비트를 읽고,
 *   쓸 자리만 마스크로 지우고,
 *   그 자리에 새 값을 시프트해 넣어,
 *   다시 32비트로 쓴다.
 *
 * 이 방식에는 대가가 있다. 읽기와 쓰기 사이가 원자적이지 않아, 그
 * 사이에 다른 주체가 같은 워드의 다른 바이트를 바꾸면 그 변경이
 * 사라진다. 특히 저쪽 호스트가 같은 레지스터를 건드릴 수 있는
 * 엔드포인트 모드에서 문제가 되는데, pcie-cadence-ep.c 가 그런
 * 구간을 spinlock 으로 감싸는 이유가 이것이다
 * (struct cdns_pcie_ep 의 lock 필드 주석 참고).
 *
 * 또 write-1-to-clear 레지스터에는 이 방식을 쓰면 안 된다. 읽은 값을
 * 그대로 다시 쓰므로 서 있던 상태 비트가 의도치 않게 지워진다.
 *
 * 4바이트 쓰기는 읽고-고치기 없이 곧바로 쓴다. 그때는 원자적이라
 * 위 문제가 없다.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 MMIO.
 *
 * 에러 경로: 정렬 오류는 pr_warn 만 남기고 아무것도 쓰지 않는다.
 *   void 라 호출자에게 알릴 방법이 없다.
 *
 * 호출 체인:
 *   cdns_pcie_rp_writeb/writew/writel, cdns_pcie_ep_fn_writeb 등 → [이 함수]
 */
static inline void cdns_pcie_write_sz(void __iomem *addr, int size, u32 value)
{
	void __iomem *aligned_addr = PTR_ALIGN_DOWN(addr, 0x4);
	unsigned int offset = (unsigned long)addr & 0x3;
	/* [한국어] 쓸 자리를 지울 마스크. */
	u32 mask;
	/* [한국어] 읽어서 고칠 워드. */
	u32 val;

	/* [한국어] read_sz 와 달리 여기서는 MMIO 전에 검사한다 —
	 * 아직 아무것도 읽지 않았기 때문이다. */
	if (!IS_ALIGNED((uintptr_t)addr, size)) {
		pr_warn("Address %p and size %d are not aligned\n", addr, size);
		return;
	}

	/* [한국어] 4바이트 쓰기는 읽고-고치기가 필요 없다. 원자적이며
	 * write-1-to-clear 레지스터에도 안전하다.
	 * aligned_addr 이 아니라 addr 을 쓰는데, 정렬 검사를 통과했으므로
	 * 둘이 같은 값이다. */
	if (size > 2) {
		writel(value, addr);
		return;
	}

	/* [한국어] 쓸 자리를 0 으로 만드는 마스크를 만든다.
	 * (1 << size*8) - 1 이 그 폭만큼의 1 이고, offset*8 만큼 밀어
	 * 자리를 맞춘 뒤 반전하면 그 자리만 0 인 마스크가 된다. */
	mask = ~(((1 << (size * 8)) - 1) << (offset * 8));
	/* [한국어] 현재 값을 읽어 그 자리만 지운다. 나머지 바이트는 보존된다. */
	val = readl(aligned_addr) & mask;
	/* [한국어] 새 값을 그 자리에 끼워 넣는다. */
	val |= value << (offset * 8);
	/* [한국어] 통째로 다시 쓴다. 이 읽기와 쓰기 사이가 원자적이지
	 * 않다는 점이 위 주석에서 설명한 문제다. */
	writel(val, aligned_addr);
}

/* [한국어] cdns_pcie_read_cfg_byte - 컨트롤러 config space 에서 1바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @where: config 오프셋. @value: 결과를 담을 곳.
 * @return: 항상 0. 실패를 알리지 않는다.
 * 이 함수와 아래 두 형제는 PCI_FIND_NEXT_CAP 매크로에 넘겨져
 * capability 목록을 훑는 데 쓰인다 — 그 매크로가 요구하는 시그니처를
 * 맞추려고 반환 타입이 int 다.
 * 실행 컨텍스트: 초기화 중. 순수 MMIO.
 * 호출 체인: cdns_pcie_find_capability() → PCI_FIND_NEXT_CAP → [이 함수] */
static inline int cdns_pcie_read_cfg_byte(struct cdns_pcie *pcie, int where,
					  u8 *val)
{
	/* [한국어] config 오프셋을 레지스터 베이스에 더한다. 이 IP 는 자기 config 가
	 * 레지스터 공간에 그대로 노출되어 있다. */
	void __iomem *addr = pcie->reg_base + where;

	*val = cdns_pcie_read_sz(addr, 0x1);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어] cdns_pcie_read_cfg_word - 같은 방식으로 2바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @where: 오프셋. @value: 결과.
 * @return: 항상 0.
 * capability ID 와 다음 항목 포인터가 2바이트라 확장 capability 탐색에
 * 쓰인다.
 * 호출 체인: cdns_pcie_find_ext_capability() → PCI_FIND_NEXT_EXT_CAP → [이 함수] */
static inline int cdns_pcie_read_cfg_word(struct cdns_pcie *pcie, int where,
					  u16 *val)
{
	/* [한국어] 위와 같은 주소 계산. */
	void __iomem *addr = pcie->reg_base + where;

	*val = cdns_pcie_read_sz(addr, 0x2);
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어] cdns_pcie_read_cfg_dword - 4바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @where: 오프셋. @value: 결과.
 * @return: 항상 0.
 * 확장 capability 헤더가 4바이트라 그 탐색에 쓰인다.
 * 앞의 둘과 달리 read_sz 를 거치지 않고 cdns_pcie_readl 을 바로 쓴다 —
 * 4바이트는 원래 정렬되어 있어 잘라 낼 것이 없기 때문이다. */
static inline int cdns_pcie_read_cfg_dword(struct cdns_pcie *pcie, int where,
					   u32 *val)
{
	*val = cdns_pcie_readl(pcie, where);
	return PCIBIOS_SUCCESSFUL;
}

/* Root Port register access */
/* [한국어] cdns_pcie_rp_writeb - 루트 포트 config 에 1바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @reg: 루트 포트 config 오프셋. @value: 쓸 값.
 * @return: 없음.
 * 리비전 ID, 프로그래밍 인터페이스, 서브클래스처럼 1바이트인 필드에 쓴다.
 * PCI 버스는 바이트 단위 쓰기를 지원하지만 이 IP 의 레지스터 창은 32비트
 * 접근만 받으므로, cdns_pcie_write_sz() 가 읽고-고쳐-쓰기로 흉내 낸다.
 * 그 사이에 다른 문맥이 같은 워드를 건드리면 변경이 사라질 수 있는데,
 * 이 계열은 루트 포트 초기화 경로에서만 쓰여 실제로 겹치지 않는다.
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 * 호출 체인: cdns_pcie_host_init_root_port() 계열 → [이 함수]
 *   → cdns_pcie_write_sz() */
static inline void cdns_pcie_rp_writeb(struct cdns_pcie *pcie,
				       u32 reg, u8 value)
{
	/* [한국어] 루트 포트 config 의 실제 주소. CDNS_PCIE_RP_BASE 를 더하는 것이
	 * 앞의 read_cfg_ 계열과 다른 점이다 — 그쪽은 IP 자신의 config 를,
	 * 이쪽은 루트 포트가 PCI 장치로서 노출하는 config 를 본다. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_RP_BASE + reg;

	/* [한국어] 1바이트 쓰기라 읽고-고쳐-쓰기를 거친다. */
	cdns_pcie_write_sz(addr, 0x1, value);
}

/* [한국어] cdns_pcie_rp_writew - 루트 포트 config 에 2바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @reg: 루트 포트 config 오프셋. @value: 쓸 값.
 * @return: 없음.
 * 벤더 ID, 디바이스 ID, 클래스 코드처럼 2바이트인 필드에 쓴다.
 * 호출 체인: cdns_pcie_host_init_root_port() → [이 함수] → cdns_pcie_write_sz() */
static inline void cdns_pcie_rp_writew(struct cdns_pcie *pcie,
				       u32 reg, u16 value)
{
	/* [한국어] 루트 포트 config 의 실제 주소. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_RP_BASE + reg;

	/* [한국어] 2바이트 쓰기도 읽고-고쳐-쓰기를 거친다. */
	cdns_pcie_write_sz(addr, 0x2, value);
}

/* [한국어] cdns_pcie_rp_readw - 루트 포트 config 에서 2바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @reg: 오프셋.
 * @return: 읽은 값.
 * Link Status 나 Link Control 처럼 2바이트인 PCIe capability 레지스터를
 * 읽는 데 쓴다.
 * 호출 체인: cdns_pcie_host_training_complete(), cdns_pcie_retrain() → [이 함수] */
static inline u16 cdns_pcie_rp_readw(struct cdns_pcie *pcie, u32 reg)
{
	/* [한국어] 루트 포트 config 의 실제 주소. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_RP_BASE + reg;

	/* [한국어] 32비트를 읽어 필요한 2바이트만 꺼낸다. */
	return cdns_pcie_read_sz(addr, 0x2);
}

/* [한국어] cdns_pcie_rp_writel - 루트 포트 config 에 4바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @reg: 오프셋. @value: 쓸 값.
 * @return: 없음.
 * 4바이트라 write_sz 안에서 읽고-고치기 없이 곧바로 쓴다.
 * 호출 체인: 루트 포트 config 를 다루는 코드 → [이 함수] */
static inline void cdns_pcie_rp_writel(struct cdns_pcie *pcie,
				       u32 reg, u32 value)
{
	/* [한국어] 루트 포트 config 의 실제 주소. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_RP_BASE + reg;

	/* [한국어] 4바이트 경로라 원자적으로 쓰인다. */
	cdns_pcie_write_sz(addr, 0x4, value);
}

/* [한국어] cdns_pcie_rp_readl - 루트 포트 config 에서 4바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @reg: 오프셋.
 * @return: 읽은 값.
 * 호출 체인: 루트 포트 config 를 읽는 코드 → [이 함수] */
static inline u32 cdns_pcie_rp_readl(struct cdns_pcie *pcie, u32 reg)
{
	/* [한국어] 루트 포트 config 의 실제 주소. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_RP_BASE + reg;

	/* [한국어] 4바이트라 시프트 없이 그대로 돌려받는다. */
	return cdns_pcie_read_sz(addr, 0x4);
}

/* [한국어] cdns_pcie_hpa_rp_writeb - 신형 배치에서 루트 포트 config 에 1바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @reg: 오프셋. @value: 쓸 값.
 * @return: 없음.
 * 구형 판과 하는 일이 같고 베이스 상수만 다르다 —
 * CDNS_PCIE_RP_BASE 대신 CDNS_PCIE_HPA_RP_BASE 를 쓴다.
 * 뱅크 인자를 받지 않는 점에 주의: 루트 포트 config 의 위치가
 * 고정이라 뱅크 변환이 필요 없기 때문이다.
 * 호출 체인: cdns_pcie_hpa_host_init_root_port() → [이 함수] */
static inline void cdns_pcie_hpa_rp_writeb(struct cdns_pcie *pcie,
					   u32 reg, u8 value)
{
	/* [한국어] 신형의 루트 포트 config 베이스를 더한다. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_HPA_RP_BASE + reg;

	/* [한국어] 1바이트라 읽고-고쳐-쓰기. */
	cdns_pcie_write_sz(addr, 0x1, value);
}

/* [한국어] cdns_pcie_hpa_rp_writew - 신형에서 루트 포트 config 에 2바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @reg: 오프셋. @value: 쓸 값.
 * @return: 없음.
 * 벤더·디바이스 ID 와 클래스 코드를 쓰는 데 쓰인다.
 * 호출 체인: cdns_pcie_hpa_host_init_root_port() → [이 함수] */
static inline void cdns_pcie_hpa_rp_writew(struct cdns_pcie *pcie,
					   u32 reg, u16 value)
{
	/* [한국어] 신형의 루트 포트 config 베이스. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_HPA_RP_BASE + reg;

	/* [한국어] 2바이트 쓰기. */
	cdns_pcie_write_sz(addr, 0x2, value);
}

/* [한국어] cdns_pcie_hpa_rp_readw - 신형에서 루트 포트 config 의 2바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @reg: 오프셋.
 * @return: 읽은 값.
 * 호출 체인: 신형 경로의 루트 포트 config 읽기 → [이 함수] */
static inline u16 cdns_pcie_hpa_rp_readw(struct cdns_pcie *pcie, u32 reg)
{
	/* [한국어] 신형의 루트 포트 config 베이스. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_HPA_RP_BASE + reg;

	/* [한국어] 32비트를 읽어 2바이트만 꺼낸다. */
	return cdns_pcie_read_sz(addr, 0x2);
}

/* Endpoint Function register access */
/* [한국어] cdns_pcie_ep_fn_writeb - 엔드포인트 함수 fn 의 config 에 1바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @fn: 물리 함수 번호. @reg: 그 함수 config 안의 오프셋.
 * @value: 쓸 값.
 * @return: 없음.
 * 루트 포트용 rp_write* 계열과 구조는 같지만 기준 주소가 다르다 --
 * 엔드포인트는 함수마다 자기 config 영역을 갖고, 그 자리를
 * CDNS_PCIE_EP_FUNC_BASE(fn) 이 계산한다(함수당 4KiB 간격).
 * 아래 _writew/_writel 과 함께 EP 초기화가 벤더/디바이스 ID, 클래스 코드
 * 같은 신원 정보를 써 넣는 통로다.
 * 실행 컨텍스트: EP 초기화 시점의 프로세스 문맥.
 * 호출 체인: cdns_pcie_ep_* 초기화 경로 → [이 함수] → cdns_pcie_write_sz() */
static inline void cdns_pcie_ep_fn_writeb(struct cdns_pcie *pcie, u8 fn,
					  u32 reg, u8 value)
{
	/* [한국어] 이 엔드포인트 함수의 config 베이스. 함수마다 자기 config 영역이
	 * 따로 있어 EP_FUNC_BASE(fn) 로 그 자리를 계산한다. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg;

	/* [한국어] 1바이트 쓰기. */
	cdns_pcie_write_sz(addr, 0x1, value);
}

/* [한국어] cdns_pcie_ep_fn_writew - 엔드포인트 함수 config 에 2바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @fn: 함수 번호. @reg: 오프셋. @value: 쓸 값.
 * @return: 없음.
 * 엔드포인트 모드에서 각 함수의 벤더 ID, 디바이스 ID 등을 쓴다.
 * 호출 체인: cdns_pcie_ep_write_header() 등 → [이 함수] */
static inline void cdns_pcie_ep_fn_writew(struct cdns_pcie *pcie, u8 fn,
					  u32 reg, u16 value)
{
	/* [한국어] 그 함수의 config 베이스. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg;

	/* [한국어] 2바이트 쓰기. */
	cdns_pcie_write_sz(addr, 0x2, value);
}

/* [한국어] cdns_pcie_ep_fn_writel - 엔드포인트 함수 config 에 4바이트를 쓴다.
 * @pcie: 대상 컨트롤러. @fn: 함수 번호. @reg: 오프셋. @value: 쓸 값.
 * @return: 없음.
 * 앞의 형제들과 달리 write_sz 를 거치지 않고 writel 을 바로 쓴다 —
 * 4바이트는 정렬이 보장되어 읽고-고치기가 필요 없기 때문이다.
 * 호출 체인: BAR 설정 등 엔드포인트 config 조작 → [이 함수] */
static inline void cdns_pcie_ep_fn_writel(struct cdns_pcie *pcie, u8 fn,
					  u32 reg, u32 value)
{
	/* [한국어] 베이스 계산과 쓰기를 한 줄에서 처리한다. */
	writel(value, pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

/* [한국어] cdns_pcie_ep_fn_readw - 엔드포인트 함수 config 에서 2바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @fn: 함수 번호. @reg: 오프셋.
 * @return: 읽은 값.
 * 호스트가 설정한 MSI 개수 등을 되읽는 데 쓴다.
 * 호출 체인: cdns_pcie_ep_get_msi() 등 → [이 함수] */
static inline u16 cdns_pcie_ep_fn_readw(struct cdns_pcie *pcie, u8 fn, u32 reg)
{
	/* [한국어] 그 함수의 config 베이스. */
	void __iomem *addr = pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg;

	/* [한국어] 32비트를 읽어 2바이트만 꺼낸다. */
	return cdns_pcie_read_sz(addr, 0x2);
}

/* [한국어] cdns_pcie_ep_fn_readl - 엔드포인트 함수 config 에서 4바이트를 읽는다.
 * @pcie: 대상 컨트롤러. @fn: 함수 번호. @reg: 오프셋.
 * @return: 읽은 값.
 * writel 판과 마찬가지로 read_sz 를 거치지 않는다.
 * 호출 체인: 엔드포인트 config 를 읽는 코드 → [이 함수] */
static inline u32 cdns_pcie_ep_fn_readl(struct cdns_pcie *pcie, u8 fn, u32 reg)
{
	/* [한국어] 베이스 계산과 읽기를 한 줄에서 처리한다. */
	return readl(pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

/* [한국어] cdns_pcie_start_link - 링크 트레이닝 시작을 SoC 구현에 위임한다
 * @pcie: 대상 컨트롤러.
 * @return: SoC 구현의 결과. 구현이 없으면 0(성공).
 * 이 파일의 디스패처 셋 중 하나다. ops 에 콜백이 있으면 그것을,
 * 없으면 기본 동작을 쓴다.
 * ops 자체의 NULL 도 함께 확인하는 점이 아래 두 형제와 같다 —
 * mobiveil 의 같은 자리가 ops 를 확인하지 않는 것과 대비된다.
 * 실행 컨텍스트: 초기화 중.
 * 호출 체인: 호스트/EP link_setup → [이 함수] → ops->start_link */
static inline int cdns_pcie_start_link(struct cdns_pcie *pcie)
{
	/* [한국어] ops 와 콜백이 모두 있을 때만 부른다. */
	if (pcie->ops && pcie->ops->start_link)
		/* [한국어] SoC 구현에 맡긴다. */
		return pcie->ops->start_link(pcie);

	return 0;
}

/* [한국어] cdns_pcie_stop_link - 링크 내리기를 SoC 구현에 위임한다
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 * 구현이 없으면 아무것도 하지 않는다.
 * 확인해 보면 이 디스패처의 호출처는 cdns_pcie_host_link_disable()
 * 하나뿐이고, 그것은 구형 경로에만 있다 — 신형 SoC 가 stop_link 를
 * 채워도 도달하지 않는 이유다.
 * 실행 컨텍스트: 정리 경로.
 * 호출 체인: cdns_pcie_host_link_disable() → [이 함수] → ops->stop_link */
static inline void cdns_pcie_stop_link(struct cdns_pcie *pcie)
{
	/* [한국어] ops 와 콜백이 모두 있을 때만. */
	if (pcie->ops && pcie->ops->stop_link)
		pcie->ops->stop_link(pcie);
}

/* [한국어] cdns_pcie_link_up - 링크 상태 판정을 SoC 구현에 위임한다
 * @pcie: 대상 컨트롤러.
 * @return: 링크가 올라왔으면 true.
 * 구현이 없으면 IP 의 표준 방식(cdns_pcie_linkup)을 쓴다.
 * 이 디스패처를 실제로 넘기는 곳은 pcie-cadence-host.c 하나뿐이며,
 * 신형 경로는 cdns_pcie_hpa_link_up() 을 직접 넘겨 이것을 거치지
 * 않는다.
 * 실행 컨텍스트: 링크 대기 루프.
 * 호출 체인: cdns_pcie_host_start_link() → [이 함수] → ops->link_up */
static inline bool cdns_pcie_link_up(struct cdns_pcie *pcie)
{
	/* [한국어] ops 와 콜백이 모두 있을 때만. */
	if (pcie->ops && pcie->ops->link_up)
		/* [한국어] SoC 구현에 맡긴다. */
		return pcie->ops->link_up(pcie);

	return true;
}

#if IS_ENABLED(CONFIG_PCIE_CADENCE_HOST)
/* [한국어] 아래 선언들은 CONFIG_PCIE_CADENCE_HOST 가 켜졌을 때의 실제 함수다.
 * cdns_pcie_host_link_setup - 구형 경로의 링크 설정. */
int cdns_pcie_host_link_setup(struct cdns_pcie_rc *rc);
/* [한국어] cdns_pcie_host_init - 구형 경로의 루트 포트·주소 변환 초기화. */
int cdns_pcie_host_init(struct cdns_pcie_rc *rc);
/* [한국어] cdns_pcie_host_setup - 구형 경로의 전체 진입점. SoC 드라이버가 부른다. */
int cdns_pcie_host_setup(struct cdns_pcie_rc *rc);
/* [한국어] cdns_pcie_host_disable - 호스트를 내린다. 링크를 끊고 열거를 걷어낸다. */
void cdns_pcie_host_disable(struct cdns_pcie_rc *rc);
/* [한국어] cdns_pci_map_bus - 구형 경로의 config 접근 주소 계산. */
void __iomem *cdns_pci_map_bus(struct pci_bus *bus, unsigned int devfn,
			       int where);
/* [한국어] cdns_pcie_hpa_host_setup - 신형 경로의 전체 진입점. */
int cdns_pcie_hpa_host_setup(struct cdns_pcie_rc *rc);
#else
/* [한국어] 아래는 CONFIG_PCIE_CADENCE_HOST 가 꺼졌을 때의 빈 구현이다.
 * 이렇게 두면 호출자가 #ifdef 로 감싸지 않아도 되고, 링크 오류 대신
 * 컴파일 시점에 사라진다. 커널에서 흔한 관용이다. */
static inline int cdns_pcie_host_link_setup(struct cdns_pcie_rc *rc)
{
	return 0;
}

/* [한국어] 빈 구현. 0(성공)을 돌려주므로 호출자가 계속 진행한다. */
static inline int cdns_pcie_host_init(struct cdns_pcie_rc *rc)
{
	return 0;
}

/* [한국어] 빈 구현. */
static inline int cdns_pcie_host_setup(struct cdns_pcie_rc *rc)
{
	return 0;
}

/* [한국어] 빈 구현. */
static inline int cdns_pcie_hpa_host_setup(struct cdns_pcie_rc *rc)
{
	return 0;
}

/* [한국어] 빈 구현. void 라 돌려줄 것이 없다. */
static inline void cdns_pcie_host_disable(struct cdns_pcie_rc *rc)
{
}

/* [한국어] 빈 구현. NULL 을 돌려주면 PCI 코어가 접근 실패로 처리한다. */
static inline void __iomem *cdns_pci_map_bus(struct pci_bus *bus, unsigned int devfn,
					     int where)
{
	return NULL;
}
#endif

#if IS_ENABLED(CONFIG_PCIE_CADENCE_EP)
/* [한국어] 아래는 CONFIG_PCIE_CADENCE_EP 가 켜졌을 때의 엔드포인트 함수들.
 * cdns_pcie_ep_setup - 구형 경로의 엔드포인트 초기화. */
int cdns_pcie_ep_setup(struct cdns_pcie_ep *ep);
/* [한국어] cdns_pcie_ep_disable - 엔드포인트를 내린다. */
void cdns_pcie_ep_disable(struct cdns_pcie_ep *ep);
/* [한국어] cdns_pcie_hpa_ep_setup - 신형 경로의 엔드포인트 초기화. */
int cdns_pcie_hpa_ep_setup(struct cdns_pcie_ep *ep);
#else
/* [한국어] 엔드포인트 지원이 꺼졌을 때의 빈 구현들. */
static inline int cdns_pcie_ep_setup(struct cdns_pcie_ep *ep)
{
	return 0;
}

/* [한국어] 빈 구현. */
static inline void cdns_pcie_ep_disable(struct cdns_pcie_ep *ep)
{
}

/* [한국어] 빈 구현. */
static inline int cdns_pcie_hpa_ep_setup(struct cdns_pcie_ep *ep)
{
	return 0;
}

#endif

/* [한국어] 아래는 설정과 무관하게 항상 있는 공통 함수들의 선언이다.
 * cdns_pcie_find_capability - IP 자신의 config 에서 capability 를 찾는다. */
u8   cdns_pcie_find_capability(struct cdns_pcie *pcie, u8 cap);
/* [한국어] cdns_pcie_find_ext_capability - 확장 capability 를 찾는다. */
u16  cdns_pcie_find_ext_capability(struct cdns_pcie *pcie, u8 cap);
/* [한국어] cdns_pcie_linkup - IP 표준 방식의 링크 판정. EXPORT 되어 있으나
 * 이 트리 안에서 호출자가 0건이다 — 위 cdns_pcie_link_up 디스패처의
 * 기본 동작으로만 쓰인다. */
bool cdns_pcie_linkup(struct cdns_pcie *pcie);

/* [한국어] cdns_pcie_detect_quiet_min_delay_set - 구형의 LTSSM Detect.Quiet 대기 설정. */
void cdns_pcie_detect_quiet_min_delay_set(struct cdns_pcie *pcie);

/* [한국어] cdns_pcie_set_outbound_region - 구형의 아웃바운드 창 설정. */
void cdns_pcie_set_outbound_region(struct cdns_pcie *pcie, u8 busnr, u8 fn,
				   u32 r, bool is_io,
				   u64 cpu_addr, u64 pci_addr, size_t size);

/* [한국어] cdns_pcie_set_outbound_region_for_normal_msg - 구형의 메시지 TLP 창. */
void cdns_pcie_set_outbound_region_for_normal_msg(struct cdns_pcie *pcie,
						  u8 busnr, u8 fn,
						  u32 r, u64 cpu_addr);

/* [한국어] cdns_pcie_reset_outbound_region - 구형의 창 지우기. 신형에는 대응 함수가 없다. */
void cdns_pcie_reset_outbound_region(struct cdns_pcie *pcie, u32 r);
/* [한국어] cdns_pcie_disable_phy - PHY 를 모두 끈다. 구형·신형 공통이다. */
void cdns_pcie_disable_phy(struct cdns_pcie *pcie);
/* [한국어] cdns_pcie_enable_phy - PHY 를 모두 켠다. */
int  cdns_pcie_enable_phy(struct cdns_pcie *pcie);
/* [한국어] cdns_pcie_init_phy - 디바이스 트리에서 PHY 를 찾아 확보하고 켠다. */
int  cdns_pcie_init_phy(struct device *dev, struct cdns_pcie *pcie);
/* [한국어] cdns_pcie_hpa_detect_quiet_min_delay_set - 신형의 Detect.Quiet 대기 설정. */
void cdns_pcie_hpa_detect_quiet_min_delay_set(struct cdns_pcie *pcie);
/* [한국어] cdns_pcie_hpa_set_outbound_region - 신형의 아웃바운드 창 설정. */
void cdns_pcie_hpa_set_outbound_region(struct cdns_pcie *pcie, u8 busnr, u8 fn,
				       u32 r, bool is_io,
				       u64 cpu_addr, u64 pci_addr, size_t size);
/* [한국어] cdns_pcie_hpa_set_outbound_region_for_normal_msg - 신형의 메시지 창. */
void cdns_pcie_hpa_set_outbound_region_for_normal_msg(struct cdns_pcie *pcie,
						      u8 busnr, u8 fn,
						      u32 r, u64 cpu_addr);
/* [한국어] cdns_pcie_hpa_host_link_setup - 신형의 링크 설정. */
int  cdns_pcie_hpa_host_link_setup(struct cdns_pcie_rc *rc);
/* [한국어] cdns_pci_hpa_map_bus - 신형의 config 접근 주소 계산. */
void __iomem *cdns_pci_hpa_map_bus(struct pci_bus *bus, unsigned int devfn,
				   int where);
/* [한국어] cdns_pcie_hpa_host_start_link - 선언은 있으나 이 트리에서 정의와
 * 호출자를 찾지 못했다. */
int  cdns_pcie_hpa_host_start_link(struct cdns_pcie_rc *rc);
/* [한국어] cdns_pcie_hpa_start_link - 마찬가지로 정의를 찾지 못했다. */
int  cdns_pcie_hpa_start_link(struct cdns_pcie *pcie);
/* [한국어] cdns_pcie_hpa_stop_link - 마찬가지. */
void cdns_pcie_hpa_stop_link(struct cdns_pcie *pcie);
/* [한국어] cdns_pcie_hpa_link_up - 신형의 링크 판정. pcie-cadence-hpa.c 에 정의가
 * 있고, 신형 호스트 경로가 이것을 wait_for_link 에 직접 넘긴다. */
bool cdns_pcie_hpa_link_up(struct cdns_pcie *pcie);

/* [한국어] cdns_pcie_pm_ops - 절전 시 PHY 를 끄고 켜는 전원 관리 표.
 * pcie-cadence.c 에 정의되어 있고 SoC 드라이버들이 자기 driver 구조체
 * 에서 가리킨다. */
extern const struct dev_pm_ops cdns_pcie_pm_ops;

#endif /* _PCIE_CADENCE_H */
