// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe controller driver for CIX's sky1 SoCs
 *
 * Copyright 2025 Cix Technology Group Co., Ltd.
 * Author: Hans Zhang <hans.zhang@cixtech.com>
 */

/*
 * [한국어 설명] CIX(Cixtech) Sky1 SoC 용 Cadence PCIe 글루 드라이버 (pci-sky1.c)
 *
 * === 파일의 역할 ===
 * Sky1 SoC 에 들어간 Cadence PCIe 컨트롤러 IP 를 커널에 등록하는 얇은 글루
 * 드라이버다. 같은 디렉터리의 pci-j721e.c 와 목적은 같지만 훨씬 단순한데,
 * 이 파일에는 클럭·리셋·PHY·GPIO·전원 도메인을 다루는 코드가 전혀 없다.
 * 이 드라이버가 하는 일은 네 가지뿐이다 — (1) DT 에서 레지스터 창 다섯 개를
 * 찾아 매핑하고, (2) ECAM(Enhanced Configuration Access Mechanism) config
 * 공간을 pci_ecam_create() 로 만들고, (3) 이 SoC 가 쓰는 Cadence 신형(HPA)
 * 레지스터 뱅크들의 오프셋 표를 채워 공통 코어에 넘기고, (4) 링크 트레이닝을
 * 켜고 끄는 콜백 세 개를 제공하는 것이다.
 * 나머지(주소 변환, 루트 포트 설정, 링크 대기, 버스 열거)는 모두
 * pcie-cadence-host-hpa.c 의 cdns_pcie_hpa_host_setup() 이 처리한다.
 * 엔드포인트 모드는 지원하지 않는다 — compatible 이 "cix,sky1-pcie-host"
 * 하나뿐이고 코드에도 EP 경로가 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 커널 모듈이며 진입점은 platform_driver 의 probe 다.
 * 부팅 시 흐름:
 *   devicetree "cix,sky1-pcie-host" 매칭
 *     -> sky1_pcie_probe()                [이 파일]
 *        -> sky1_pcie_resource_get()      [이 파일] 레지스터 창 5개 확보
 *        -> pci_ecam_create()             [drivers/pci/ecam.c] config 창 매핑
 *        -> cdns_pcie_hpa_host_setup()    [pcie-cadence-host-hpa.c]
 *           -> cdns_pcie_hpa_host_link_setup() 링크를 올린다
 *              -> cdns_pcie_start_link()  [pcie-cadence.h 의 디스패처]
 *                 -> sky1_pcie_start_link()  [이 파일로 되돌아온다]
 *              -> cdns_pcie_host_wait_for_link(pcie, cdns_pcie_hpa_link_up)
 *           -> cdns_pcie_hpa_host_init()  주소 변환과 루트 포트 설정
 *           -> pci_host_probe()           PCI 버스 열거
 * 중요한 갈림길이 하나 있다. Cadence 코어에는 구형(LGA) 레지스터 배치와
 * 신형(HPA) 배치 두 갈래가 있는데, 이 파일은 신형 쪽이다. 그래서 config
 * 접근도 IP 의 자체 창이 아니라 표준 ECAM 을 쓰고(rc->ecam_supported = 1),
 * 레지스터 접근도 뱅크 번호를 함께 넘기는 cdns_pcie_hpa_readl 계열을 쓴다.
 * 참고로 이 트리에서 확인한 바로는, 신형 호스트 경로는 링크 상태를 읽을 때
 * ops->link_up 디스패처가 아니라 cdns_pcie_hpa_link_up() 을 직접 넘긴다
 * (pcie-cadence-host-hpa.c:304). 따라서 아래 sky1_pcie_link_up() 은 ops 에
 * 등록되어 있지만 이 트리 안에서 그것을 부르는 경로를 찾지 못했다.
 * 다만 두 함수는 읽는 레지스터가 사실상 같다 — 자세한 것은 그 함수의 주석에
 * 적었다.
 *
 * === 타 모듈과의 연결 ===
 * 아래로는 두 곳에 의존한다. 하나는 Cadence 신형 공통 코어
 * (pcie-cadence-hpa.c, pcie-cadence-host-hpa.c, 그리고 공용 헤더
 * pcie-cadence.h / pcie-cadence-host-common.h)이고, 다른 하나는 PCI 코어의
 * ECAM 지원(drivers/pci/ecam.c)이다. 클럭·PHY·리셋 계층에는 의존하지 않는데,
 * 그 초기화는 이 SoC 에서 커널 밖(펌웨어)에서 이루어지는 것으로 보인다 —
 * 다만 그 근거를 이 트리 안에서는 확인하지 못했고, 코드에 해당 처리가
 * 없다는 사실만 확인했다.
 * 데이터 흐름: DT -> sky1_pcie_resource_get() 이 다섯 개의 창(reg, cfg,
 * rcsu_strap, rcsu_status, msg)을 struct sky1_pcie 에 모은다 -> probe 가
 * 그중 reg_base 와 msg_res 를 struct cdns_pcie 로, cfg 관련은
 * struct cdns_pcie_rc 로 옮겨 담는다 -> 공통 코어가 그것만 보고 동작한다.
 * 즉 struct sky1_pcie 는 "DT 에서 공통 코어로 건너가는 중간 보관소" 에 가깝다.
 * 공유 상태는 struct cdns_pcie 와 cdns_pcie_rc, 그리고 뱅크 오프셋 표인
 * struct cdns_plat_pcie_of_data 다.
 * 이 컨트롤러 아래에 붙는 NVMe 등의 장치는 이 파일을 코드로 부르지 않는다.
 * 관계는 버스 토폴로지상의 것이며 함수 호출 관계가 아니다
 * (drivers/nvme 에서 cdns_ 심볼 호출은 0건).
 *
 * === 주요 함수/구조체 요약 ===
 * sky1_pcie_probe()          : 진입점. 자원 확보 -> ECAM 생성 -> 뱅크 오프셋
 *                              표 작성 -> 벤더/디바이스 ID 와 quirk 설정 ->
 *                              cdns_pcie_hpa_host_setup() 호출.
 * sky1_pcie_resource_get()   : DT reg 항목을 이름으로 찾아 다섯 개를 확보한다.
 *                              cfg 와 msg 는 매핑 시점이 달라 resource 만
 *                              들고 있거나 따로 ioremap 한다.
 * sky1_pcie_ops              : 공통 코어가 되부르는 링크 제어 콜백 세 개.
 * sky1_pcie_start_link()     : rcsu_strap 창의 레지스터 1번 비트 0 을 세워
 *                              링크 트레이닝을 시작한다.
 * sky1_pcie_link_up()        : IP 레지스터 뱅크의 디버그 상태 레지스터
 *                              비트 0 으로 링크 완료를 판정한다.
 * struct sky1_pcie           : DT 에서 얻은 자원 다섯 벌과 공통 코어 구조체
 *                              두 개에 대한 역참조를 담는 인스턴스 상태.
 */

/* [한국어] 커널 공통 매크로 모음(BIT(), 각종 헬퍼). 아래 LINK_TRAINING_ENABLE 등이
 * 쓰는 BIT() 매크로가 여기(또는 여기가 끌어오는 헤더)에서 온다. */
#include <linux/kernel.h>
/* [한국어] MODULE_LICENSE/DESCRIPTION/AUTHOR/DEVICE_TABLE 과 module_platform_driver.
 * 이 드라이버는 모듈로 빌드될 수 있다. */
#include <linux/module.h>
/* [한국어] struct of_device_id, of_match_table 매칭에 필요한 정의.
 * devm_pci_alloc_host_bridge() 가 안에서 하는 DT ranges 파싱도 이 계층이다. */
#include <linux/of.h>
/* [한국어] of_device_get_match_data() 등 DT 매칭 데이터 API 헤더.
 * 이 파일의 매칭 표에는 .data 가 없어 그 함수를 부르지 않으므로,
 * 여기서 직접 쓰는 심볼을 찾지 못했다 — 상류에 남아 있는 포함으로 보인다. */
#include <linux/of_device.h>
/* [한국어] struct pci_host_bridge, struct pci_ops, pci_host_bridge_priv(),
 * devm_pci_alloc_host_bridge(), resource_list_first_type(), IORESOURCE_BUS 등
 * PCI 코어와 맞물리는 부분 전부. */
#include <linux/pci.h>
/* [한국어] struct pci_config_window, pci_ecam_create(), pci_ecam_free(),
 * pci_generic_ecam_ops — 이 드라이버가 표준 ECAM 으로 config 공간을 여는
 * 핵심 근거다. 구형 Cadence 경로가 IP 자체 config 창을 쓰는 것과 대비된다. */
#include <linux/pci-ecam.h>
/* [한국어] PCI 벤더/디바이스 ID 상수 모음. 다만 CIX 의 ID 는 이 헤더에 없어서
 * 아래에서 이 파일이 직접 정의한다(1f0e418bb6 스냅숏의 pci_ids.h 에
 * CIX 항목이 없음을 확인했다). 따라서 이 포함이 무엇을 위해 필요한지는
 * 확인하지 못했다. */
#include <linux/pci_ids.h>

/* [한국어] Cadence 공통 코어의 인터페이스. struct cdns_pcie / cdns_pcie_rc /
 * cdns_pcie_ops / cdns_plat_pcie_of_data, 신형 레지스터 접근자
 * cdns_pcie_hpa_readl 계열과 뱅크 enum(REG_BANK_IP_REG 등), 그리고
 * cdns_pcie_hpa_host_setup() 선언이 모두 여기 있다. */
#include "pcie-cadence.h"
/* [한국어] 구형/신형이 공유하는 호스트 헬퍼 선언(cdns_pcie_host_wait_for_link 등).
 * 이 파일이 그 함수들을 직접 부르지는 않으며, 공통 코어가 쓰는 선언을
 * 같이 끌어오는 형태다. */
#include "pcie-cadence-host-common.h"

/* [한국어] CIX Technology 의 PCI 벤더 ID 0x1f6c. include/linux/pci_ids.h 에
 * 등재되어 있지 않아 여기서 직접 정의한다.
 * 루트 포트의 config space 에 이 값을 써 넣어야 OS 와 도구가 이 브리지를
 * 제조사별로 식별할 수 있다. */
#define PCI_VENDOR_ID_CIX		0x1f6c
/* [한국어] Sky1 루트 포트의 디바이스 ID 0x0001.
 * 위 벤더 ID 와 짝을 이뤄 rc->vendor_id / rc->device_id 로 전달되고,
 * cdns_pcie_hpa_host_init_root_port() 가 루트 포트 config space 의
 * PCI_VENDOR_ID / PCI_DEVICE_ID 에 써 넣는다. */
#define PCI_DEVICE_ID_CIX_SKY1		0x0001

/* [한국어] rcsu_strap 창 안에서 n 번째 32비트 레지스터의 바이트 오프셋.
 * 레지스터 폭이 4바이트라 인덱스에 0x04 를 곱한다.
 * 이 파일은 STRAP_REG(1)(= 0x04) 하나만 쓴다. */
#define STRAP_REG(n)			((n) * 0x04)
/* [한국어] rcsu_status 창 안에서 n 번째 32비트 레지스터의 바이트 오프셋.
 * 정의는 되어 있지만 이 파일 안에서 이 매크로를 쓰는 코드를 찾지 못했다 —
 * 링크 상태는 status 창이 아니라 IP 레지스터 뱅크에서 읽기 때문이다. */
#define STATUS_REG(n)			((n) * 0x04)
/* [한국어] STRAP_REG(1) 의 비트 0. 세우면 LTSSM 이 링크 트레이닝을 시작하고,
 * 지우면 멈춘다. j721e 의 같은 이름 상수와 값은 같지만 전혀 다른
 * 레지스터 창에 있다는 점에 유의. */
#define LINK_TRAINING_ENABLE		BIT(0)
/* [한국어] IP_REG_I_DBG_STS_0 의 비트 0. 링크 초기화가 끝났음을 뜻한다.
 * 공통 코어의 cdns_pcie_hpa_link_up() 이 같은 위치를 GENMASK(0, 0) 으로
 * 검사하므로 의미가 동일하다. */
#define LINK_COMPLETE			BIT(0)

/* [한국어] reg 창 안에서 IP 레지스터 뱅크가 시작하는 오프셋 0x1000.
 * 아래 여덟 개 상수는 Cadence 신형 IP 의 레지스터 뱅크들이 이 SoC 에서
 * 어디에 배치되었는지를 나타내며, probe 가 통째로
 * struct cdns_plat_pcie_of_data 에 복사해 공통 코어에 넘긴다.
 * 공통 코어의 cdns_reg_bank_to_off() 가 뱅크 enum 을 이 값으로 바꾼다. */
#define SKY1_IP_REG_BANK		0x1000
/* [한국어] IP config control 레지스터 뱅크의 오프셋 0x4c00.
 * 루트 포트 BAR 설정과 EROM 창 설정이 이 뱅크에 있다. */
#define SKY1_IP_CFG_CTRL_REG_BANK	0x4c00
/* [한국어] AXI 마스터 공통 레지스터 뱅크의 오프셋 0xf000. */
#define SKY1_IP_AXI_MASTER_COMMON	0xf000
/* [한국어] AXI 슬레이브 레지스터 뱅크의 오프셋 0x9000.
 * CPU -> PCIe 방향(아웃바운드) 주소 변환 창 설정이 여기 있다. */
#define SKY1_AXI_SLAVE			0x9000
/* [한국어] AXI 마스터 레지스터 뱅크의 오프셋 0xb000.
 * PCIe -> 메모리 방향(인바운드) 쪽이다. */
#define SKY1_AXI_MASTER			0xb000
/* [한국어] AXI HLS 레지스터 뱅크의 오프셋 0xc000. */
#define SKY1_AXI_HLS_REGISTERS		0xc000
/* [한국어] AXI RAS(Reliability, Availability, Serviceability) 레지스터 뱅크의
 * 오프셋 0xe000. 서버 칩이라 오류 보고 기능이 별도 뱅크로 있다. */
#define SKY1_AXI_RAS_REGISTERS		0xe000
/* [한국어] DTI 레지스터 뱅크의 오프셋 0xd000.
 * 주소 순서상 RAS(0xe000) 앞이지만 정의 순서만 뒤에 있을 뿐이다. */
#define SKY1_DTI_REGISTERS		0xd000

/* [한국어] IP 레지스터 뱅크 안에서 디버그 상태 레지스터 0 의 오프셋 0x420.
 * 실제 접근 주소는 reg_base + SKY1_IP_REG_BANK + 0x420 = reg_base + 0x1420.
 * 공통 코어의 CDNS_PCIE_HPA_PHY_DBG_STS_REG0 과 같은 오프셋이다. */
#define IP_REG_I_DBG_STS_0		0x420

/* [한국어]
 * struct sky1_pcie - 이 드라이버 인스턴스 하나의 상태
 *
 * probe 에서 devm_kzalloc 으로 할당해 dev_set_drvdata() 로 device 에 매단다.
 * 그래서 ops 콜백들이 cdns_pcie->dev 만 들고도 dev_get_drvdata() 로 이
 * 구조체를 되찾을 수 있다.
 * 성격은 "DT 에서 읽은 것을 모아 두었다가 공통 코어 구조체로 옮겨 담는
 * 중간 보관소" 에 가깝다. 옮겨 담은 뒤에도 남겨 두는 이유는 (a) 링크 제어
 * 콜백이 strap_base 를 계속 필요로 하고, (b) remove 가 pci_config_window 를
 * 해제해야 하기 때문이다.
 */
struct sky1_pcie {
	struct cdns_pcie *cdns_pcie;
	/* [한국어] Cadence 공통 코어가 다루는 컨트롤러 구조체(rc->pcie 의 주소).
	 * 설정자: sky1_pcie_probe() — cdns_pcie 의 각 필드를 채운 뒤 저장한다.
	 * 읽는 자: 이 트리 안에서 이 필드를 다시 읽는 코드를 찾지 못했다.
	 *   ops 콜백들은 인자로 cdns_pcie 를 직접 받으므로 이 역참조가 필요 없다.
	 * 값 범위: probe 성공 후 유효한 포인터.
	 * 동기화: probe 에서만 쓰므로 락 불필요. */

	struct cdns_pcie_rc *cdns_pcie_rc;
	/* [한국어] pci_host_bridge 의 private 영역에 놓인 Cadence 루트 컴플렉스
	 * 구조체.
	 * 설정자: sky1_pcie_probe().
	 * 읽는 자: 위와 마찬가지로 이 트리 안에서 다시 읽는 코드를 찾지 못했다.
	 *   rc 가 필요한 곳은 probe 안이고, 거기서는 지역 변수 rc 를 그대로 쓴다.
	 * 값 범위: probe 성공 후 유효한 포인터.
	 * 동기화: 락 불필요. */

	struct resource *cfg_res;
	/* [한국어] DT reg 항목 "cfg" 의 물리 주소 범위. ECAM config 공간이다.
	 * 여기서 바로 ioremap 하지 않고 resource 만 들고 있는 이유는,
	 * ECAM 매핑을 pci_ecam_create() 가 버스 번호 범위까지 고려해 직접
	 * 수행해야 하기 때문이다.
	 * 설정자: sky1_pcie_resource_get() 의
	 *   platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg").
	 * 읽는 자: sky1_pcie_probe() 가 pci_ecam_create() 의 인자로 넘긴다.
	 * 값 범위: 유효한 IORESOURCE_MEM resource 포인터(NULL 이면 probe 실패).
	 * 동기화: probe 에서만 접근. */

	struct resource *msg_res;
	/* [한국어] DT reg 항목 "msg" 의 물리 주소 범위. PCIe 메시지(INTx 등
	 * TLP 메시지)를 내보내기 위한 아웃바운드 창의 주소로 쓰인다.
	 * 설정자: sky1_pcie_resource_get().
	 * 읽는 자: probe 가 cdns_pcie->msg_res 로 옮겨 담고, 실제 사용은
	 *   pcie-cadence-host-hpa.c:244 부근이 그 start 주소로 메시지용
	 *   아웃바운드 영역을 만드는 곳이다.
	 * 값 범위: 유효한 resource 포인터.
	 * 동기화: probe 에서만 접근. */

	struct pci_config_window *cfg;
	/* [한국어] pci_ecam_create() 가 만들어 준 ECAM 창 서술자. 매핑된 주소
	 * (cfg->win)와 점유한 resource(cfg->res), 버스 번호 범위(cfg->busr)를
	 * 함께 들고 있다.
	 * 설정자: sky1_pcie_probe() 의 pci_ecam_create().
	 * 읽는 자: probe 가 rc->cfg_base/cfg_res 와 bridge->sysdata 에 나눠 담고,
	 *   sky1_pcie_remove() 와 probe 의 실패 경로가 pci_ecam_free() 로 해제한다.
	 * 값 범위: 유효한 포인터. devm 이 아니므로 반드시 손으로 해제해야 한다 —
	 *   이 파일에서 유일하게 수동 해제가 필요한 자원이다.
	 * 동기화: probe/remove 에서만 접근. */

	void __iomem *strap_base;
	/* [한국어] "rcsu_strap" 창의 가상 주소. RCSU 는 이 SoC 의 루트 컴플렉스
	 * 주변 제어 블록으로 보이며, 여기에 링크 트레이닝 enable 비트가 있다.
	 * 설정자: sky1_pcie_resource_get() 의 devm_platform_ioremap_resource_byname.
	 * 읽는 자: sky1_pcie_start_link() 와 sky1_pcie_stop_link() — 이 구조체에서
	 *   probe 이후에도 실제로 쓰이는 몇 안 되는 필드다.
	 * 값 범위: ioremap 된 유효 주소. readl/writel 로만 접근.
	 * 동기화: read-modify-write 를 하지만 락이 없다. 링크 제어 경로가
	 *   probe 와 공통 코어에서만 시작되어 직렬화되기 때문이다. */

	void __iomem *status_base;
	/* [한국어] "rcsu_status" 창의 가상 주소. 위 STATUS_REG(n) 매크로가 이 창을
	 * 겨냥해 정의되어 있다.
	 * 설정자: sky1_pcie_resource_get().
	 * 읽는 자: 이 트리 안에서 이 필드를 읽는 코드를 찾지 못했다 —
	 *   sky1_pcie_link_up() 은 이 창이 아니라 IP 레지스터 뱅크를 읽는다.
	 *   DT 가 이 창을 필수로 요구하므로 확보만 해 두는 셈이다.
	 * 값 범위: ioremap 된 유효 주소.
	 * 동기화: 현재 접근하는 코드가 없다. */

	void __iomem *reg_base;
	/* [한국어] "reg" 창의 가상 주소 — Cadence IP 본체의 레지스터 공간이다.
	 * 이 하나의 창 안에 IP_REG, IP_CFG_CTRL, AXI_MASTER/SLAVE 등 여러 뱅크가
	 * 아래 SKY1_* 오프셋만큼 떨어져 배치되어 있다.
	 * 설정자: sky1_pcie_resource_get().
	 * 읽는 자: probe 가 cdns_pcie->reg_base 로 옮겨 담고, 그 뒤로는 공통 코어의
	 *   cdns_pcie_hpa_readl/writel 이 (뱅크 오프셋 + 레지스터 오프셋)을 더해
	 *   접근한다. sky1_pcie_link_up() 도 그 경로를 탄다.
	 * 값 범위: ioremap 된 유효 주소.
	 * 동기화: 공통 코어가 관리한다. */

	void __iomem *cfg_base;
	/* [한국어] ECAM 창의 매핑된 가상 주소(cfg->win 의 복사본).
	 * 설정자: sky1_pcie_probe() — rc->cfg_base 를 채운 뒤 같은 값을 여기에도
	 *   넣어 둔다.
	 * 읽는 자: 이 트리 안에서 이 필드를 읽는 코드를 찾지 못했다. 실제 config
	 *   접근은 bridge->sysdata 에 담긴 cfg 를 통해 pci_ecam_map_bus() 가 한다.
	 * 값 범위: 유효한 __iomem 주소.
	 * 동기화: probe 에서만 접근. */

	void __iomem *msg_base;
	/* [한국어] "msg" 창을 ioremap 한 가상 주소.
	 * 설정자: sky1_pcie_resource_get() 의 devm_ioremap_resource().
	 * 읽는 자: 이 트리 안에서 이 필드를 읽는 코드를 찾지 못했다. 공통 코어가
	 *   쓰는 것은 가상 주소가 아니라 물리 주소(msg_res->start)이기 때문이다 —
	 *   메시지 창은 CPU 가 직접 읽고 쓰는 대상이 아니라 아웃바운드 주소 변환의
	 *   목적지로 등록되는 영역이다. 그래도 여기서 매핑해 두면 그 물리 범위가
	 *   다른 드라이버에게 점유당하지 않는 효과가 있다.
	 * 값 범위: ioremap 된 유효 주소.
	 * 동기화: 현재 접근하는 코드가 없다. */
};

/* [한국어]
 * sky1_pcie_resource_get - DT 의 레지스터 창 다섯 개를 이름으로 확보한다
 *
 * @pdev: platform 버스가 DT 노드로부터 만든 디바이스. reg / reg-names 속성이
 *        여기에 붙어 있다.
 * @pcie: 결과를 채워 넣을 인스턴스 상태. 이 함수가 여섯 개 필드를 채운다.
 * @return: 0 성공, 음수 errno 실패(dev_err_probe 가 그대로 돌려주는 값).
 *
 * 왜 필요한가: 이 컨트롤러는 서로 성격이 다른 주소 창을 다섯 개 쓴다 —
 * IP 레지스터(reg), ECAM config(cfg), RCSU 스트랩(rcsu_strap), RCSU 상태
 * (rcsu_status), 메시지(msg). probe 를 읽기 쉽게 하려고 그 확보를 이 함수로
 * 몰아 두었다.
 * 세 가지 처리 방식이 섞여 있다는 점이 이 함수의 핵심이다.
 *   (a) reg / rcsu_strap / rcsu_status — devm_platform_ioremap_resource_byname
 *       으로 확보와 매핑을 한 번에 한다. CPU 가 직접 읽고 쓰는 창이다.
 *   (b) cfg — platform_get_resource_byname 으로 resource 만 받아 둔다.
 *       ECAM 매핑은 버스 번호 범위를 알아야 크기가 정해지므로
 *       pci_ecam_create() 가 나중에 직접 한다.
 *   (c) msg — resource 를 받아 두고(공통 코어가 물리 주소를 쓰므로) 매핑도
 *       따로 한다.
 * 이름(byname)으로 찾는 이유는 DT 에서 reg 항목의 순서가 바뀌어도 깨지지
 * 않게 하려는 것이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). ioremap 은 잠들 수 있다.
 * 호출자: sky1_pcie_probe() 한 곳.
 * 피호출자: devm_platform_ioremap_resource_byname(),
 * platform_get_resource_byname(), devm_ioremap_resource(), dev_err_probe().
 * 에러 경로: 어느 창이든 실패하면 dev_err_probe() 로 로그를 남기고 그 errno 를
 * 즉시 돌려준다. 이미 매핑한 창은 devm_ 이라 코어가 자동으로 푼다.
 * dev_err_probe 를 쓰므로 EPROBE_DEFER 일 때 로그가 조용해진다.
 *
 * 호출 체인:
 *   sky1_pcie_probe() → [이 함수] → devm_platform_ioremap_resource_byname()
 */
static int sky1_pcie_resource_get(struct platform_device *pdev,
				  struct sky1_pcie *pcie)
{
	/* [한국어] 로그와 devm 자원 관리에 쓸 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] platform_get_resource_byname() 결과를 잠시 받는 임시 변수.
	 * cfg 와 msg 두 창에 재사용한다. */
	struct resource *res;
	/* [한국어] ioremap 결과를 잠시 받는 임시 변수. reg / rcsu_strap / rcsu_status 세 창에
	 * 재사용한다. 각각 곧바로 pcie 의 해당 필드로 옮기므로 겹칠 일이 없다. */
	void __iomem *base;

	/* [한국어] DT reg 항목 중 이름이 "reg" 인 창을 확보하고 ioremap 까지 한 번에 한다.
	 * 이것이 Cadence IP 본체의 레지스터 공간이며, 아래 SKY1_* 뱅크 오프셋들이
	 * 모두 이 창 기준이다. */
	base = devm_platform_ioremap_resource_byname(pdev, "reg");
	/* [한국어] 이 API 는 실패 시 NULL 이 아니라 ERR_PTR 을 돌려주므로 IS_ERR 로 검사한다. */
	if (IS_ERR(base))
		/* [한국어] dev_err_probe 는 errno 를 그대로 돌려주면서 로그를 남기고,
		 * EPROBE_DEFER 일 때는 로그 수준을 낮춰 준다. 그래서 return 과 한 줄로 합칠 수 있다. */
		return dev_err_probe(dev, PTR_ERR(base),
				     "unable to find \"reg\" registers\n");
	/* [한국어] 확보한 IP 레지스터 base 를 저장한다. probe 가 이것을 cdns_pcie->reg_base 로
	 * 옮겨야 공통 코어의 뱅크 접근자가 동작한다. */
	pcie->reg_base = base;

	/* [한국어] "cfg" 창은 resource 만 받아 둔다. 여기서 ioremap 하지 않는 이유는
	 * ECAM 매핑 크기가 버스 번호 범위에 따라 정해져서, pci_ecam_create() 가
	 * 직접 매핑해야 하기 때문이다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	/* [한국어] 이쪽 API 는 실패 시 NULL 을 돌려주므로 IS_ERR 이 아니라 NULL 검사를 쓴다. */
	if (!res)
		/* [한국어] 돌려줄 errno 가 없으므로 -ENODEV 를 직접 만들어 넘긴다. */
		return dev_err_probe(dev, -ENODEV, "unable to get \"cfg\" resource\n");
	/* [한국어] probe 가 pci_ecam_create() 에 넘길 수 있도록 저장한다. */
	pcie->cfg_res = res;

	/* [한국어] "rcsu_strap" 창을 확보하고 매핑한다. 링크 트레이닝 enable 비트가 여기 있다. */
	base = devm_platform_ioremap_resource_byname(pdev, "rcsu_strap");
	/* [한국어] ioremap 실패. */
	if (IS_ERR(base))
		/* [한국어] 어느 창이 문제인지 알 수 있도록 이름을 그대로 로그에 넣는다. */
		return dev_err_probe(dev, PTR_ERR(base),
				     "unable to find \"rcsu_strap\" registers\n");
	/* [한국어] start_link/stop_link 콜백이 쓸 base 를 확정한다.
	 * 이 줄 이후에야 그 콜백들이 안전하게 불릴 수 있다. */
	pcie->strap_base = base;

	/* [한국어] "rcsu_status" 창을 확보하고 매핑한다. */
	base = devm_platform_ioremap_resource_byname(pdev, "rcsu_status");
	/* [한국어] ioremap 실패. */
	if (IS_ERR(base))
		/* [한국어] 실패한 창 이름을 로그에 남긴다. */
		return dev_err_probe(dev, PTR_ERR(base),
				     "unable to find \"rcsu_status\" registers\n");
	/* [한국어] 저장은 하지만, 앞서 적었듯 이 트리 안에서 이 base 를 읽는 코드는 없다. */
	pcie->status_base = base;

	/* [한국어] "msg" 창의 resource 를 받는다. 공통 코어가 쓰는 것은 가상 주소가 아니라
	 * 이 resource 의 물리 시작 주소다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "msg");
	/* [한국어] NULL 검사. */
	if (!res)
		/* [한국어] -ENODEV 로 실패를 알린다. */
		return dev_err_probe(dev, -ENODEV, "unable to get \"msg\" resource\n");
	/* [한국어] 공통 코어에 넘길 물리 범위를 저장한다. */
	pcie->msg_res = res;
	/* [한국어] resource 를 별도로 ioremap 한다. 위 세 창과 달리 byname 헬퍼를 못 쓰는
	 * 이유는 resource 를 이미 손에 들고 있기 때문이다.
	 * 이 매핑 자체를 읽고 쓰는 코드는 이 트리에 없지만, 매핑해 두면 해당 물리
	 * 범위가 다른 드라이버에게 점유당하지 않는 효과가 있다. */
	pcie->msg_base = devm_ioremap_resource(dev, res);
	/* [한국어] ioremap 실패. */
	if (IS_ERR(pcie->msg_base)) {
		/* [한국어] 실패 사유를 남기고 그대로 돌려준다. */
		return dev_err_probe(dev, PTR_ERR(pcie->msg_base),
				     "unable to ioremap msg resource\n");
	}

	/* [한국어] 다섯 창을 모두 확보했다. */
	return 0;
}

/* [한국어]
 * sky1_pcie_start_link - 링크 트레이닝을 시작시킨다 (ops->start_link 구현)
 *
 * @cdns_pcie: 공통 코어의 컨트롤러 구조체. 우리 상태는 여기서 dev 를 꺼내
 *             dev_get_drvdata() 로 되찾는다 — probe 가
 *             cdns_pcie_hpa_host_setup() 을 부르기 전에 dev_set_drvdata()
 *             를 해 두었기 때문에 이 되찾기가 성립한다.
 * @return: 항상 0. 레지스터 비트 하나를 세우는 것이 전부라 실패할 여지가 없다.
 *          int 를 돌려주는 것은 struct cdns_pcie_ops 의 시그니처 때문이다.
 *
 * 공통 코어는 "링크를 올려라" 만 알고, 그 비트가 어디 있는지는 SoC 마다
 * 다르다. Sky1 에서는 IP 레지스터가 아니라 RCSU 스트랩 창에 있다.
 * 동작: strap_base + STRAP_REG(1)(= 오프셋 0x04)를 읽어
 * LINK_TRAINING_ENABLE(비트 0)만 세워 되쓴다. read-modify-write 를 쓰는 것은
 * 같은 레지스터의 다른 스트랩 비트를 뭉개지 않기 위해서다.
 * 트레이닝 완료를 기다리는 것은 이 함수가 아니라 호출자 쪽의
 * cdns_pcie_host_wait_for_link() 다.
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 에서 이어진 호출). 잠들지 않는다.
 * 호출자: pcie-cadence.h:418 의 static inline 디스패처 cdns_pcie_start_link().
 * 그것을 부르는 곳 중 이 파일과 이어지는 것은
 * cdns_pcie_hpa_host_link_setup()(pcie-cadence-host-hpa.c:298)이다.
 * 피호출자: readl(), writel().
 * 에러 경로: 없음. 링크가 안 올라오면 대기 함수 쪽에서 드러난다.
 *
 * 호출 체인:
 *   sky1_pcie_probe() → cdns_pcie_hpa_host_setup() →
 *   cdns_pcie_hpa_host_link_setup() → cdns_pcie_start_link()[디스패처]
 *     → [이 함수] → writel()
 */
static int sky1_pcie_start_link(struct cdns_pcie *cdns_pcie)
{
	/* [한국어] 공통 코어가 넘긴 cdns_pcie 의 dev 를 통해 우리 인스턴스 상태를 되찾는다.
	 * probe 가 cdns_pcie_hpa_host_setup() 을 부르기 전에 dev_set_drvdata() 를
	 * 해 두었기에 성립한다. */
	struct sky1_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	/* [한국어] strap 레지스터 값을 담을 임시 변수. */
	u32 val;

	/* [한국어] STRAP_REG(1)(창 기준 오프셋 0x04)을 읽는다.
	 * 같은 레지스터의 다른 스트랩 비트를 보존하려고 통째로 읽어 온다. */
	val = readl(pcie->strap_base + STRAP_REG(1));
	/* [한국어] LINK_TRAINING_ENABLE(비트 0)만 세운다.
	 * 이 순간 LTSSM 이 링크 파트너를 찾기 시작한다. */
	val |= LINK_TRAINING_ENABLE;
	/* [한국어] 고친 값을 되쓴다. 완료 대기는 호출자 쪽의
	 * cdns_pcie_host_wait_for_link() 가 맡는다. */
	writel(val, pcie->strap_base + STRAP_REG(1));

	/* [한국어] 레지스터 비트 하나를 세우는 것이 전부라 실패할 수 없다. */
	return 0;
}

/* [한국어]
 * sky1_pcie_stop_link - 링크 트레이닝을 멈춘다 (ops->stop_link 구현)
 *
 * @cdns_pcie: 공통 코어의 컨트롤러 구조체. drvdata 로 우리 상태를 되찾는다.
 * @return: 없음(콜백 시그니처가 void).
 *
 * start_link 의 정확한 대칭이다. STRAP_REG(1) 을 읽어
 * LINK_TRAINING_ENABLE 비트만 지워 되쓰면 LTSSM 이 트레이닝을 멈추고
 * 링크가 내려간다.
 * 다만 이 트리에서 확인한 바로는 이 콜백이 실제로 불리는 경로가 없다.
 * 디스패처 cdns_pcie_stop_link() 을 부르는 곳은
 * cdns_pcie_host_link_disable()(pcie-cadence-host.c:1110) 하나인데, 그것은
 * 구형(LGA) 경로의 cdns_pcie_host_disable() 에서만 불리고, 이 파일의
 * sky1_pcie_remove() 는 그 함수를 부르지 않는다(신형 경로에는 대응하는
 * hpa 판 disable 함수가 이 트리에 없다). 즉 지금은 대칭성을 위해 구현해 둔
 * 상태이며, 앞으로 신형 경로에 disable 이 추가되면 그때 연결될 자리다.
 * 실행 컨텍스트: 불린다면 프로세스 컨텍스트일 것이다. 잠들지 않는다.
 * 호출자: 디스패처 cdns_pcie_stop_link() — 위에 적은 이유로 현재 도달 경로 없음.
 * 피호출자: readl(), writel().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   cdns_pcie_stop_link()[디스패처] → [이 함수] → writel()
 *   (이 트리에서 디스패처까지 이어지는 실제 호출자는 확인되지 않았다)
 */
static void sky1_pcie_stop_link(struct cdns_pcie *cdns_pcie)
{
	/* [한국어] 우리 인스턴스 상태를 되찾는다. */
	struct sky1_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	/* [한국어] strap 레지스터 값을 담을 임시 변수. */
	u32 val;

	/* [한국어] 현재 값을 읽는다. */
	val = readl(pcie->strap_base + STRAP_REG(1));
	/* [한국어] LINK_TRAINING_ENABLE 비트만 지운다.
	 * ~ 로 뒤집은 마스크와 AND 하므로 다른 스트랩 비트는 그대로 남는다. */
	val &= ~LINK_TRAINING_ENABLE;
	/* [한국어] 고친 값을 되쓴다. 트레이닝이 멈추고 링크가 내려간다. */
	writel(val, pcie->strap_base + STRAP_REG(1));
}

/* [한국어]
 * sky1_pcie_link_up - 링크가 올라왔는지 읽는다 (ops->link_up 구현)
 *
 * @cdns_pcie: 공통 코어의 컨트롤러 구조체. 이 함수는 위 두 콜백과 달리
 *             drvdata 로 우리 상태를 되찾지 않는다 — 읽을 레지스터가 RCSU 가
 *             아니라 IP 본체에 있어서, 공통 코어의 접근자만으로 충분하기
 *             때문이다.
 * @return: 링크가 올라왔으면 true.
 *
 * 동작: cdns_pcie_hpa_readl(pcie, REG_BANK_IP_REG, IP_REG_I_DBG_STS_0) 으로
 * IP 레지스터 뱅크의 디버그 상태 레지스터 0(뱅크 안 오프셋 0x420)을 읽고,
 * LINK_COMPLETE(비트 0)를 검사한다. 실제 주소는 probe 가 채워 둔 뱅크 오프셋
 * SKY1_IP_REG_BANK(0x1000)를 더해 reg_base + 0x1420 이 된다.
 * 반환형이 bool 이므로 `val & LINK_COMPLETE` 의 0/비0 이 그대로 false/true 로
 * 변환된다.
 * 주의할 점 — 이 함수는 ops 에 등록되어 있지만, 이 트리에서 그것을 부르는
 * 경로를 찾지 못했다. ops->link_up 을 역참조하는 곳은 pcie-cadence.h:432 의
 * 디스패처 cdns_pcie_link_up() 하나뿐이고, 그 디스패처를 넘기는 곳은
 * 구형(LGA) 경로인 pcie-cadence-host.c:1183 하나인데, 이 파일은 신형(HPA)
 * 경로를 쓴다. 신형 경로(pcie-cadence-host-hpa.c:304)는 대신
 * cdns_pcie_hpa_link_up() 을 직접 넘긴다.
 * 다만 두 함수는 사실상 같은 것을 읽는다 — cdns_pcie_hpa_link_up() 은
 * REG_BANK_IP_REG 의 CDNS_PCIE_HPA_PHY_DBG_STS_REG0(0x0420)에서
 * GENMASK(0, 0) 을 검사하는데, 이 파일의 IP_REG_I_DBG_STS_0 도 같은 0x420 이고
 * LINK_COMPLETE 도 같은 비트 0 이다. 그래서 지금 이 콜백이 안 불려도 동작에
 * 차이가 없다.
 * 실행 컨텍스트: 불린다면 폴링 루프 안의 프로세스 컨텍스트. 잠들지 않는다.
 * 호출자: 디스패처 cdns_pcie_link_up() — 위 이유로 현재 도달 경로 없음.
 * 피호출자: cdns_pcie_hpa_readl()(pcie-cadence.h 의 static inline).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   cdns_pcie_link_up()[디스패처] → [이 함수] → cdns_pcie_hpa_readl()
 *   (이 트리에서 디스패처까지 이어지는 실제 호출자는 확인되지 않았다)
 */
static bool sky1_pcie_link_up(struct cdns_pcie *cdns_pcie)
{
	/* [한국어] 디버그 상태 레지스터 값을 담을 임시 변수. */
	u32 val;

	/* [한국어] IP 레지스터 뱅크(REG_BANK_IP_REG)의 디버그 상태 레지스터 0 을 읽는다.
	 * 첫 인자로 우리 상태가 아니라 cdns_pcie 를 그대로 넘기는 점에 주의 —
	 * 이 접근자가 pcie->cdns_pcie_reg_offsets 에서 뱅크 오프셋(0x1000)을 꺼내
	 * reg_base 에 더하기 때문이다. 그래서 probe 가 그 오프셋 표를 채워 두지 않으면
	 * 이 한 줄이 NULL 역참조로 죽는다. */
	val = cdns_pcie_hpa_readl(cdns_pcie, REG_BANK_IP_REG,
				  IP_REG_I_DBG_STS_0);
	/* [한국어] 비트 0(LINK_COMPLETE)이 서 있으면 링크 초기화가 끝난 것이다.
	 * 반환형이 bool 이라 0/비0 이 그대로 false/true 로 변환된다. */
	return val & LINK_COMPLETE;
}

/* [한국어]
 * sky1_pcie_ops - Cadence 공통 코어가 되부르는 SoC 별 링크 제어 콜백 표
 *
 * probe 가 cdns_pcie->ops 에 이 표를 꽂아 둔다. 세 콜백 중 신형(HPA) 호스트
 * 경로가 실제로 쓰는 것은 start_link 하나이며, stop_link 와 link_up 은 각
 * 함수의 주석에 적은 이유로 이 트리 안에 도달 경로가 없다.
 * 네 번째 멤버 cpu_addr_fixup 은 채우지 않는다 — CPU 물리 주소를 그대로
 * 아웃바운드 창 주소로 쓸 수 있다는 뜻이다. 디스패처가 NULL 을 확인하고
 * 원래 주소를 그대로 쓴다.
 * const 정적 데이터라 읽기 전용이고 동기화가 필요 없다.
 */
static const struct cdns_pcie_ops sky1_pcie_ops = {
	/* [한국어] 링크 트레이닝 시작 콜백. 신형 호스트 경로가 실제로 쓰는 유일한 콜백이다. */
	.start_link = sky1_pcie_start_link,
	/* [한국어] 링크 트레이닝 중단 콜백. 이 트리에는 도달 경로가 없다. */
	.stop_link = sky1_pcie_stop_link,
	/* [한국어] 링크 완료 판정 콜백. 이 트리에는 도달 경로가 없다. */
	.link_up = sky1_pcie_link_up,
};

/* [한국어]
 * sky1_pcie_probe - 드라이버 진입점. 컨트롤러를 세우고 버스를 열거한다
 *
 * @pdev: DT 노드 "cix,sky1-pcie-host" 로부터 만들어진 platform 디바이스.
 * @return: 0 성공, 음수 errno 실패.
 *
 * pci-j721e.c 의 probe 와 견주면 없는 것이 많다 — 클럭, 리셋, PHY, GPIO,
 * runtime PM, 모드 분기가 전부 없다. 이 SoC 에서는 그 초기화가 커널 밖에서
 * 이루어지는 것으로 보이며(이 트리 안에서 그 근거를 확인하지는 못했다),
 * 커널이 할 일은 "이미 살아 있는 컨트롤러를 PCI 코어에 등록하는 것" 뿐이다.
 * 동작 단계:
 *   (1) 인스턴스 상태와 pci_host_bridge(뒤에 cdns_pcie_rc 를 붙여)를 할당한다.
 *   (2) sky1_pcie_resource_get() 으로 DT 창 다섯 개를 확보한다.
 *   (3) bridge->windows 에서 버스 번호 범위를 찾아 pci_ecam_create() 로
 *       ECAM config 창을 만든다. 버스 범위가 있어야 창 크기가 정해진다.
 *   (4) bridge->ops 를 표준 ECAM 접근자로 고정한다. 여기서 채워 두면 나중에
 *       cdns_pcie_hpa_host_setup() 이 자기 기본 표로 덮어쓰지 않는다.
 *   (5) cdns_pcie / cdns_pcie_rc 의 필드를 채운다 — reg_base, msg_res,
 *       is_rc, ops, cfg_base/cfg_res, ecam_supported.
 *   (6) 신형 레지스터 뱅크 오프셋 표를 만들어 cdns_pcie 에 꽂는다.
 *       이것이 없으면 cdns_pcie_hpa_readl/writel 이 뱅크 주소를 계산하지
 *       못하므로, 공통 코어를 부르기 전에 반드시 채워야 한다.
 *   (7) 벤더/디바이스 ID 와 no_inbound_map quirk 를 설정한다.
 *   (8) drvdata 를 매단 뒤 cdns_pcie_hpa_host_setup() 을 부른다. 그 안에서
 *       링크가 올라가고(우리 start_link 콜백이 되불린다) 주소 변환이 서고
 *       pci_host_probe() 로 버스가 열거된다.
 * 순서에서 중요한 것은 (6)과 (8) 사이의 관계, 그리고 (8) 직전의
 * dev_set_drvdata() 다 — 후자가 없으면 start_link 콜백이 NULL 을 역참조한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. driver.probe_type 이
 * PROBE_PREFER_ASYNCHRONOUS 라 부팅 시 다른 드라이버와 병렬로 실행될 수 있다.
 * 호출자: 드라이버 코어(really_probe).
 * 피호출자: devm_kzalloc, devm_pci_alloc_host_bridge, sky1_pcie_resource_get,
 * pci_ecam_create, pci_host_bridge_priv, cdns_pcie_hpa_host_setup.
 * 에러 경로: goto 라벨이 없고 각 지점에서 직접 반환한다. pci_ecam_create 가
 * 성공한 뒤의 실패 두 곳(reg_off 할당 실패, host_setup 실패)에서는
 * pci_ecam_free() 를 손으로 불러야 한다 — ECAM 창은 이 파일에서 유일하게
 * devm_ 이 아닌 자원이기 때문이다. 나머지는 모두 devm_ 이라 코어가 푼다.
 *
 * 호출 체인:
 *   드라이버 코어 → [이 함수] → sky1_pcie_resource_get() → pci_ecam_create()
 *     → cdns_pcie_hpa_host_setup() → (내부에서) sky1_pcie_start_link()
 */
static int sky1_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 신형 레지스터 뱅크 오프셋 표. devm_kzalloc 으로 만들어 아래에서 채운다. */
	struct cdns_plat_pcie_of_data *reg_off;
	/* [한국어] 로그와 devm 자원 관리에 쓸 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 할당할 PCI 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 공통 코어에 넘길 컨트롤러 구조체 포인터. */
	struct cdns_pcie *cdns_pcie;
	/* [한국어] bridge->windows 에서 찾아낼 버스 번호 범위 항목. */
	struct resource_entry *bus;
	/* [한국어] 브리지 private 영역에 놓일 Cadence 루트 컴플렉스 구조체. */
	struct cdns_pcie_rc *rc;
	/* [한국어] 이 파일의 인스턴스 상태. */
	struct sky1_pcie *pcie;
	/* [한국어] 각 단계 반환값. */
	int ret;

	/* [한국어] 인스턴스 상태를 0 으로 채워 할당한다. devm_ 이라 자동 해제된다. */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 메모리 부족. 아직 확보한 자원이 없어 그냥 반환하면 된다. */
	if (!pcie)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] 호스트 브리지를 할당하면서 private 영역을 cdns_pcie_rc 크기만큼 더 잡는다.
	 * 브리지와 rc 가 한 덩어리에 놓여 pci_host_bridge_priv() 와
	 * pci_host_bridge_from_priv() 로 서로를 오갈 수 있게 된다.
	 * 이 함수는 안에서 DT 의 ranges/bus-range 도 파싱해 bridge->windows 를 채우는데,
	 * 바로 아래 줄이 그 결과에 의존한다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	/* [한국어] 메모리 부족. */
	if (!bridge)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] DT 창 다섯 개를 확보한다. 이 시점부터 pcie->cfg_res 가 유효해진다. */
	ret = sky1_pcie_resource_get(pdev, pcie);
	/* [한국어] 자원 확보 실패. 로그는 이미 그 함수가 남겼다. */
	if (ret < 0)
		/* [한국어] 자원 확보 실패. 로그는 sky1_pcie_resource_get 이 이미 남겼다. */
		return ret;

	/* [한국어] 위에서 파싱된 windows 목록에서 버스 번호 범위(IORESOURCE_BUS) 항목을 찾는다.
	 * ECAM 창 크기가 '버스 개수 × 버스당 크기' 로 정해지므로 이 값이 반드시 필요하다. */
	bus = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	/* [한국어] DT 에 bus-range 가 없으면 여기 걸린다. */
	if (!bus)
		/* [한국어] DT 에 bus-range 가 없어 ECAM 창 크기를 정할 수 없다. */
		return -ENODEV;

	/* [한국어] ECAM config 창을 만든다. cfg_res 의 물리 범위를 버스 범위에 맞춰 잘라
	 * pci_remap_cfgspace() 로 매핑하고, iomem_resource 에 등록해 점유를 선언한다.
	 * 네 번째 인자 pci_generic_ecam_ops 는 표준 ECAM 주소 계산
	 * (버스/장치/함수를 비트로 이어 붙이는 방식)을 쓰겠다는 뜻이다. */
	pcie->cfg = pci_ecam_create(dev, pcie->cfg_res, bus->res,
				    &pci_generic_ecam_ops);
	/* [한국어] ECAM 창 생성 실패. 주소 충돌이나 매핑 실패가 여기 걸린다. */
	if (IS_ERR(pcie->cfg))
		/* [한국어] 이 시점까지 확보한 것은 모두 devm_ 이므로 손으로 풀 것이 없다.
		 * 반대로 이 줄 아래부터는 pci_ecam_free() 를 직접 불러야 한다. */
		return PTR_ERR(pcie->cfg);

	/* [한국어] config 접근자를 표준 ECAM 판으로 고정한다.
	 * 캐스팅이 필요한 이유는 pci_ecam_ops 안의 pci_ops 가 const 인데
	 * bridge->ops 는 비-const 포인터이기 때문이다.
	 * 여기서 채워 두면 나중에 cdns_pcie_hpa_host_setup() 이 자기 기본 표로
	 * 덮어쓰지 않는다(그 함수는 bridge->ops 가 비어 있을 때만 채운다). */
	bridge->ops = (struct pci_ops *)&pci_generic_ecam_ops.pci_ops;
	/* [한국어] 브리지 뒤에 붙여 둔 private 영역을 cdns_pcie_rc 로 해석한다. */
	rc = pci_host_bridge_priv(bridge);
	/* [한국어] 공통 코어에게 '이 컨트롤러는 ECAM 을 쓴다' 고 알린다.
	 * pcie-cadence-host-hpa.c 가 이 플래그를 보고 IP 자체 config 창을 만드는
	 * 단계를 건너뛴다. */
	rc->ecam_supported = 1;
	/* [한국어] 공통 코어가 참조할 config 공간 가상 주소. pci_ecam_create 가 만든 매핑이다. */
	rc->cfg_base = pcie->cfg->win;
	/* [한국어] 같은 창의 물리 resource. 공통 코어가 주소 변환 창을 세울 때 쓴다.
	 * cfg->res 는 pci_config_window 안에 들어 있는 값이라 그 주소를 넘긴다 —
	 * 따라서 pci_ecam_free() 이후에는 이 포인터가 유효하지 않다. */
	rc->cfg_res = &pcie->cfg->res;

	/* [한국어] cdns_pcie_rc 의 첫 멤버가 struct cdns_pcie 이므로 그 주소를 그대로 쓴다. */
	cdns_pcie = &rc->pcie;
	/* [한국어] 공통 코어가 로그와 devm 자원 관리에 쓸 device 를 꽂는다. */
	cdns_pcie->dev = dev;
	/* [한국어] 링크 제어 콜백 표를 꽂는다. 이 순간부터 공통 코어의 디스패처가
	 * sky1_pcie_start_link() 로 내려올 수 있게 된다. */
	cdns_pcie->ops = &sky1_pcie_ops;
	/* [한국어] IP 레지스터 base 를 넘긴다. 이 값이 있어야 cdns_pcie_hpa_readl/writel 이
	 * 동작한다. 참고로 cdns_pcie_hpa_host_setup() 은 이 값이 비어 있으면
	 * 스스로 "reg" 를 ioremap 하지만, 여기서 미리 채워 두므로 그 경로는 안 탄다. */
	cdns_pcie->reg_base = pcie->reg_base;
	/* [한국어] 메시지용 아웃바운드 창의 물리 범위를 넘긴다.
	 * pcie-cadence-host-hpa.c 가 이 값이 있을 때만 메시지 영역을 만든다. */
	cdns_pcie->msg_res = pcie->msg_res;
	/* [한국어] 이 컨트롤러가 Root Complex 임을 표시한다.
	 * cdns_pcie_hpa_host_setup() 이 어차피 다시 true 로 세우지만,
	 * 그전에 불릴 수 있는 코드가 올바른 값을 보게 하려고 미리 채운다. */
	cdns_pcie->is_rc = true;

	/* [한국어] 신형 레지스터 뱅크 오프셋 표를 할당한다. devm_ 이라 자동 해제된다.
	 * const 정적 데이터로 두어도 될 내용이지만, 필드가 const 포인터라
	 * 동적 할당 후 채우는 형태를 쓴다. */
	reg_off = devm_kzalloc(dev, sizeof(*reg_off), GFP_KERNEL);
	/* [한국어] 메모리 부족. 여기서는 ECAM 창이 이미 만들어진 뒤라 그냥 반환하면 안 된다. */
	if (!reg_off) {
		/* [한국어] ECAM 창은 devm_ 이 아니므로 손으로 풀어야 한다.
		 * 이 줄을 빠뜨리면 매핑과 iomem_resource 점유가 그대로 남는다. */
		pci_ecam_free(pcie->cfg);
		/* [한국어] 메모리 부족을 알린다. */
		return -ENOMEM;
	}

	/* [한국어] IP 레지스터 뱅크 오프셋. cdns_reg_bank_to_off() 가 REG_BANK_IP_REG 를
	 * 이 값으로 바꾼다. sky1_pcie_link_up() 이 의존하는 값이다. */
	reg_off->ip_reg_bank_offset = SKY1_IP_REG_BANK;
	/* [한국어] IP config control 뱅크 오프셋. REG_BANK_IP_CFG_CTRL_REG 에 대응하며,
	 * 루트 포트 BAR 설정과 EROM 창 초기화가 이 뱅크를 쓴다. */
	reg_off->ip_cfg_ctrl_reg_offset = SKY1_IP_CFG_CTRL_REG_BANK;
	/* [한국어] AXI 마스터 공통 뱅크 오프셋. REG_BANK_AXI_MASTER_COMMON 에 대응. */
	reg_off->axi_mstr_common_offset = SKY1_IP_AXI_MASTER_COMMON;
	/* [한국어] AXI 슬레이브 뱅크 오프셋. REG_BANK_AXI_SLAVE 에 대응하며,
	 * 아웃바운드(CPU -> PCIe) 주소 변환 창 설정이 여기 있다. */
	reg_off->axi_slave_offset = SKY1_AXI_SLAVE;
	/* [한국어] AXI 마스터 뱅크 오프셋. REG_BANK_AXI_MASTER 에 대응. */
	reg_off->axi_master_offset = SKY1_AXI_MASTER;
	/* [한국어] AXI HLS 뱅크 오프셋. REG_BANK_AXI_HLS 에 대응. */
	reg_off->axi_hls_offset = SKY1_AXI_HLS_REGISTERS;
	/* [한국어] AXI RAS 뱅크 오프셋. REG_BANK_AXI_RAS 에 대응. */
	reg_off->axi_ras_offset = SKY1_AXI_RAS_REGISTERS;
	/* [한국어] AXI DTI 뱅크 오프셋. REG_BANK_AXI_DTI 에 대응.
	 * 표의 is_rc 필드는 채우지 않아 0 으로 남는데, 이 트리에서
	 * cdns_pcie_reg_offsets->is_rc 를 읽는 코드를 찾지 못했다
	 * (그 필드를 쓰는 것은 pcie-cadence-plat.c 가 자기 매칭 데이터로 쓸 때뿐이다). */
	reg_off->axi_dti_offset = SKY1_DTI_REGISTERS;
	/* [한국어] 완성된 표를 공통 코어에 꽂는다. 이 줄이 아래 host_setup 보다 앞에 와야
	 * 한다 — 그 안의 모든 레지스터 접근이 이 표로 주소를 계산하기 때문이다. */
	cdns_pcie->cdns_pcie_reg_offsets = reg_off;

	/* [한국어] 공통 코어 구조체로 되돌아갈 역참조를 저장한다. */
	pcie->cdns_pcie = cdns_pcie;
	/* [한국어] 루트 컴플렉스 구조체로 되돌아갈 역참조를 저장한다. */
	pcie->cdns_pcie_rc = rc;
	/* [한국어] ECAM 매핑 주소를 인스턴스 상태에도 복사해 둔다. */
	pcie->cfg_base = rc->cfg_base;
	/* [한국어] PCI 코어가 config 접근 때 참조할 sysdata 를 꽂는다.
	 * pci_generic_ecam_ops 의 map_bus(pci_ecam_map_bus)가 bus->sysdata 를
	 * struct pci_config_window 로 해석해 주소를 계산하므로, 이 줄이 없으면
	 * config 접근이 엉뚱한 주소를 짚는다. */
	bridge->sysdata = pcie->cfg;

	/* [한국어] 루트 포트 config space 에 써 넣을 벤더 ID.
	 * cdns_pcie_hpa_host_init_root_port() 가 0xffff 가 아닐 때만 실제로 쓴다.
	 * 구형 경로가 DT 의 vendor-id 속성을 읽는 것과 달리, 신형 경로에는 그
	 * 파싱이 없어 여기서 상수로 박아 넣는다. */
	rc->vendor_id = PCI_VENDOR_ID_CIX;
	/* [한국어] 같은 방식으로 디바이스 ID 를 지정한다. */
	rc->device_id = PCI_DEVICE_ID_CIX_SKY1;
	/* [한국어] 인바운드(PCIe -> 메모리) 주소 변환 창을 만들지 말라는 quirk.
	 * 이 SoC 는 PCIe 주소와 시스템 메모리 주소가 1:1 이라 변환 창이 필요 없다.
	 * 공통 코어(pcie-cadence-host-hpa.c:270)가 이 플래그를 보고
	 * DMA range 매핑 단계를 통째로 건너뛴다. */
	rc->no_inbound_map = 1;

	/* [한국어] 인스턴스 상태를 device 에 매단다.
	 * 이 줄이 반드시 아래 host_setup 보다 앞에 와야 한다 —
	 * host_setup 안에서 되불리는 sky1_pcie_start_link() 가
	 * dev_get_drvdata() 로 strap_base 를 찾기 때문이다. */
	dev_set_drvdata(dev, pcie);

	/* [한국어] Cadence 신형 공통 코어에 넘긴다. 이 한 번의 호출 안에서 EROM 창 초기화,
	 * 링크 트레이닝(우리 start_link 콜백 사용)과 완료 대기, 인바운드 BAR 가용
	 * 표시 초기화, 루트 포트와 주소 변환 설정, 그리고 pci_host_probe() 에 의한
	 * 버스 열거가 모두 일어난다. */
	ret = cdns_pcie_hpa_host_setup(rc);
	/* [한국어] 공통 코어 초기화 실패. 여기서는 ECAM 창을 손으로 풀어 줘야 한다. */
	if (ret < 0) {
		/* [한국어] 공통 코어 초기화가 실패했으므로 ECAM 창을 풀어 준다. */
		pci_ecam_free(pcie->cfg);
		/* [한국어] 실패 사유를 드라이버 코어에 전달한다. */
		return ret;
	}

	/* [한국어] 여기까지 왔으면 링크가 올라갔고 PCI 버스 열거까지 끝났다. */
	return 0;
}

/* [한국어]
 * of_sky1_pcie_match - devicetree compatible 매칭 표
 *
 * pci-j721e.c 의 표와 달리 항목이 하나뿐이고 .data 도 붙어 있지 않다.
 * SoC 변형이 하나이고 그 특성이 코드에 상수로 박혀 있어서(SKY1_* 오프셋,
 * PCI_VENDOR_ID_CIX 등) 매칭 데이터를 따로 나를 필요가 없기 때문이다.
 * 그래서 probe 도 of_device_get_match_data() 를 부르지 않는다.
 * 마지막 {} 는 표의 끝을 알리는 sentinel 로 반드시 있어야 한다.
 */
static const struct of_device_id of_sky1_pcie_match[] = {
	/* [한국어] 이 드라이버가 매칭하는 유일한 compatible. .data 가 없어 probe 는
	 * of_device_get_match_data() 를 부르지 않는다. */
	{ .compatible = "cix,sky1-pcie-host", },
	/* [한국어] 표의 끝을 알리는 sentinel. */
	{},
};
/* [한국어] 위 매칭 표를 모듈 바이너리의 별도 섹션에 복사해 둔다.
 * depmod 가 그것으로 modules.alias 를 만들고, udev 가 DT 노드를 보고
 * 이 모듈을 자동으로 올릴 수 있게 된다. */
MODULE_DEVICE_TABLE(of, of_sky1_pcie_match);

/* [한국어]
 * sky1_pcie_remove - 드라이버를 내린다
 *
 * @pdev: 대상 platform 디바이스. drvdata 로 우리 상태를 되찾는다.
 * @return: 없음(최신 커널의 platform remove 콜백은 void 다).
 *
 * 하는 일이 pci_ecam_free() 한 줄뿐인데, 그 이유는 이 드라이버가 확보하는
 * 자원 중 devres 가 관리하지 않는 것이 ECAM 창 하나뿐이기 때문이다.
 * 레지스터 매핑, 호스트 브리지, 인스턴스 상태, 뱅크 오프셋 표는 모두
 * devm_ 계열로 얻어서 코어가 자동으로 푼다.
 * 다만 j721e 의 remove 와 견주면 빠진 것이 눈에 띈다 — 링크를 끊는 처리
 * (stop_link)와 PCI 열거를 걷어내는 처리(host disable)가 없다. 이 트리에는
 * 신형(HPA) 경로용 host disable 함수가 아직 없어서, sky1_pcie_ops 에 등록된
 * stop_link 도 여기서 불리지 않는다. 그 사실만 확인했을 뿐, 그것이 의도된
 * 설계인지 미완성인지는 이 트리 안에서 판단할 근거를 찾지 못했다.
 * 실행 컨텍스트: 프로세스 컨텍스트. pci_ecam_free 는 iounmap 과 resource
 * 해제를 하므로 잠들 수 있다.
 * 호출자: 드라이버 코어(rmmod 또는 언바인드).
 * 피호출자: platform_get_drvdata(), pci_ecam_free().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   드라이버 코어 → [이 함수] → pci_ecam_free()
 */
static void sky1_pcie_remove(struct platform_device *pdev)
{
	/* [한국어] probe 가 매달아 둔 인스턴스 상태를 되찾는다. */
	struct sky1_pcie *pcie = platform_get_drvdata(pdev);

	/* [한국어] ECAM 매핑을 풀고 iomem_resource 점유를 반납한 뒤 구조체를 해제한다.
	 * 이 드라이버가 손으로 해제해야 하는 유일한 자원이다. */
	pci_ecam_free(pcie->cfg);
}

/* [한국어]
 * sky1_pcie_driver - 커널 platform 버스에 등록하는 드라이버 서술자
 *
 * pci-j721e.c 의 것과 두 가지가 다르다. 첫째, .pm 이 없다 — 이 드라이버는
 * 시스템 절전 콜백을 제공하지 않는다. 둘째, suppress_bind_attrs 대신
 * probe_type 에 PROBE_PREFER_ASYNCHRONOUS 를 준다. 즉 sysfs 를 통한 수동
 * 언바인드를 막지 않는 대신, 부팅 시 probe 를 다른 드라이버와 병렬로
 * 돌려 부팅 시간을 줄인다. 서버 칩이라 PCIe 컨트롤러가 여러 개일 수 있고,
 * 각각의 링크 트레이닝 대기가 직렬로 쌓이면 부팅이 눈에 띄게 느려지기
 * 때문으로 보인다(그 의도를 이 트리 안에서 확인하지는 못했다).
 */
static struct platform_driver sky1_pcie_driver = {
	/* [한국어] 바인딩 시 불릴 진입점. */
	.probe  = sky1_pcie_probe,
	/* [한국어] 언바인딩 시 불릴 정리 함수. */
	.remove = sky1_pcie_remove,
	/* [한국어] 드라이버 코어에 등록할 공통 속성 묶음. */
	.driver = {
		/* [한국어] 드라이버 이름. /sys/bus/platform/drivers/ 아래에 이 이름으로 디렉터리가
		 * 생기고 로그 접두사에도 쓰인다. */
		.name = "sky1-pcie",
		/* [한국어] 이 표와 DT compatible 이 맞아야 probe 가 불린다. */
		.of_match_table = of_sky1_pcie_match,
		/* [한국어] 부팅 시 이 드라이버의 probe 를 다른 드라이버와 병렬로 돌린다.
		 * 링크 트레이닝 대기가 부팅 경로를 직렬로 막지 않게 하려는 것으로 보인다.
		 * j721e 와 달리 suppress_bind_attrs 를 쓰지 않아 sysfs 언바인드가 열려 있다. */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
/* [한국어] module_init/module_exit 보일러플레이트를 만들어 준다.
 * 모듈이 올라올 때 platform_driver_register(&sky1_pcie_driver) 를,
 * 내려갈 때 platform_driver_unregister() 를 부른다. */
module_platform_driver(sky1_pcie_driver);

/* [한국어] 모듈 라이선스를 GPL 로 선언한다. 이 선언이 있어야 커널이 모듈을
 * tainted 로 보지 않고, EXPORT_SYMBOL_GPL 로 내보낸 심볼
 * (cdns_pcie_hpa_host_setup, pci_ecam_create/free 등)을 링크할 수 있다. */
MODULE_LICENSE("GPL");
/* [한국어] modinfo 에 보이는 한 줄 설명. */
MODULE_DESCRIPTION("PCIe controller driver for CIX's sky1 SoCs");
/* [한국어] modinfo 에 보이는 원 저자 정보. */
MODULE_AUTHOR("Hans Zhang <hans.zhang@cixtech.com>");
