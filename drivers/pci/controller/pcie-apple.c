// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host bridge driver for Apple system-on-chips.
 *
 * The HW is ECAM compliant, so once the controller is initialized,
 * the driver mostly deals MSI mapping and handling of per-port
 * interrupts (INTx, management and error signals).
 *
 * Initialization requires enabling power and clocks, along with a
 * number of register pokes.
 *
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2021 Google LLC
 * Copyright (C) 2021 Corellium LLC
 * Copyright (C) 2021 Mark Kettenis <kettenis@openbsd.org>
 *
 * Author: Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Author: Marc Zyngier <maz@kernel.org>
 */

/*
 * [한국어 설명] Apple Silicon SoC 내장 PCIe 호스트 브리지 드라이버 (pcie-apple.c)
 *
 * === 파일의 역할 ===
 * Apple 이 자체 설계한 SoC(T8103 = 초대 M1 계열, T602x = 그 다음 세대) 안에 들어
 * 있는 PCIe 루트 컴플렉스를 초기화하고 리눅스 PCI 서브시스템에 등록한다. 설정공간
 * 접근 코드는 이 파일에 없다 — 하드웨어가 ECAM 규격을 그대로 지키므로
 * pci_ecam_map_bus() 와 pci_generic_config_read() / pci_generic_config_write() 를
 * 통째로 빌려 쓴다(파일 맨 위 상류 주석의 "ECAM compliant" 가 그 뜻이다).
 * 그래서 이 파일에 실제로 남는 일은 넷이다. (1) 포트마다 앱클럭을 켜고 PHY 와
 * refclk 요청/응답 핸드셰이크를 주고받은 뒤 PERST#(엔드포인트 리셋)를 규격이
 * 요구하는 시간만큼 잡았다 놓는 전원·리셋 시퀀스. (2) 포트마다 하나씩 있는 32비트
 * 인터럽트 상태/마스크 레지스터 쌍을 32칸짜리 선형 IRQ 도메인으로 노출해, 하위
 * 4비트는 INTx 레벨 인터럽트로, 나머지는 링크 업/다운 같은 에지 이벤트로 다루는 일.
 * (3) MSI 를 상위 인터럽트 컨트롤러가 미리 떼어 준 벡터 구간에 1:1 로 대응시키는
 * MSI 부모 도메인을 만드는 일. (4) 장치의 RID(Requester ID — 버스/장치/함수 번호를
 * 합친 16비트 식별자)를 IOMMU 의 SID(Stream ID)로 번역하는 포트별 RID2SID 표를
 * 장치가 붙고 떨어질 때마다 채우고 비우는 일.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 스택의 최하단, 실제 레지스터를 두드리는 호스트 컨트롤러 드라이버 자리다.
 * 부팅 흐름은 한 줄로 이어진다. DT 매칭으로 apple_pcie_probe() 가 불리면 호스트
 * 브리지와 사설 데이터를 한 덩어리로 할당하고, 공용 레지스터 창(DT reg 인덱스 1)을
 * 매핑하고, apple_msi_init() 으로 MSI 부모 도메인을 먼저 세운 뒤 pci_host_common_init()
 * 에 나머지를 위임한다. 그 안에서 pci_host_common_ecam_create() → pci_ecam_create()
 * 가 ECAM 창을 만들고 ops->init 후크(= apple_pcie_init())를 되부르며(ecam.c:298),
 * 그 후크가 DT 자식 노드마다 apple_pcie_setup_port() 를 돌려 포트를 살린다. 포트가
 * 모두 준비된 뒤에야 제어가 돌아와 pci_host_probe() 가 버스를 스캔한다.
 * 열거가 시작된 뒤에는 두 후크가 되불린다 — 장치가 활성화될 때
 * apple_pcie_enable_device(), 비활성화될 때 apple_pcie_disable_device() 다.
 * 실행 컨텍스트: probe 경로는 전부 프로세스 컨텍스트이며 msleep() 과 usleep_range()
 * 로 실제로 잠든다. 포트 인터럽트 핸들러 apple_port_irq_handler() 는 상위 IRQ 의
 * 하드 인터럽트 컨텍스트에서 체인 방식으로 불리고, 그 아래 apple_pcie_port_irq() 는
 * request_irq() 로 등록된 보통의 1차 핸들러다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일을 부르는 쪽): 같은 디렉터리의 공용 라이브러리다. "pci-host-common.h"
 * 의 pci_host_common_init() 이 유일한 위임처이고, ECAM 쪽은 drivers/pci/ecam.c 의
 * pci_ecam_create() 가 ops->init 을 되부른다. apple_pcie_lookup() 이
 * dev_get_drvdata() 로 브리지를 되찾을 수 있는 근거는 pci_host_common_init() 이
 * platform_set_drvdata() 로 브리지를 심어 두기 때문이다(pci-host-common.c:255).
 * cfg->parent 가 유효한 근거는 pci_ecam_create() 가 ops->init 을 부르기 전에
 * 그 필드를 채우기 때문이다(ecam.c:218 부근).
 * 아래쪽(이 파일이 부르는 쪽): irqdomain/MSI 프레임워크, GPIO consumer API, 그리고
 * of_map_id() 로 읽는 DT 의 iommu-map 이다. MSI 벡터의 실제 주인인 상위 인터럽트
 * 컨트롤러(Apple AIC)의 구현은 drivers/irqchip 에 있는데 이 트리에 그 디렉터리가
 * 없어 확인 못 함. IOMMU(Apple DART) 쪽도 drivers/iommu 가 없어 SID 가 실제로
 * 어떻게 쓰이는지 확인 못 함.
 * 데이터 흐름: DT 의 msi-ranges 가 "부모 컨트롤러 + 시작 인터럽트 지정자 + 벡터
 * 개수" 를 주면 pcie->fwspec 과 pcie->nvecs 에 담기고, 비트맵으로 벡터를 배분하며,
 * 포트 레지스터(도어벨 주소 / MSIMAP / MSICFG)에 도어벨 주소와 벡터 범위를 새긴다.
 * 반대 방향으로는 장치의 RID 가 of_map_id() 를 거쳐 SID 가 되어 RID2SID 표에 실린다.
 * NVMe 와의 접점: 이 파일은 특정 장치를 알지 못한다. 다만 여기서 만든 MSI 부모
 * 도메인이 없으면 아래 붙은 어떤 PCIe 장치도 MSI/MSI-X 를 받을 수 없다는 점에서,
 * NVMe 드라이버가 큐마다 벡터를 받는 pci_alloc_irq_vectors_affinity()
 * (drivers/nvme/host/pci.c) 호출의 하부 구조가 된다. 그 이상의 직접 결합은 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - apple_pcie_probe(): DT 매칭 진입점. 브리지 할당 → 공용 창 매핑 →
 *   apple_msi_init() → pci_host_common_init() 위임.
 * - apple_pcie_init(): ECAM 창을 만드는 도중 불리는 ops->init 후크. DT 자식 노드마다
 *   apple_pcie_setup_port() 를 돌린다.
 * - apple_pcie_setup_port(): 이 파일에서 가장 긴 함수. GPIO PERST#, 앱클럭,
 *   refclk 핸드셰이크, 포트 READY 폴링, IRQ 도메인 생성, RID2SID 표 크기 탐지,
 *   LTSSM 시작, 링크 업 대기까지 한 포트의 생애 시작을 모두 담는다.
 * - apple_msi_init(): DT 의 msi-ranges 를 읽어 벡터 비트맵과 MSI 부모 도메인을 만든다.
 *   벡터 하나가 곧 상위 컨트롤러의 인터럽트 하나다.
 * - apple_pcie_enable_device() / apple_pcie_disable_device(): 장치가 붙고 떨어질 때
 *   RID2SID 표의 한 칸을 잡고 놓는다.
 * - struct hw_info: 세대별 레지스터 오프셋 차이만 담은 상수표. 오프셋 0 을 "그
 *   레지스터가 이 세대에는 없음" 이라는 뜻으로 쓴다.
 * - struct apple_pcie: 컨트롤러 하나를 나타낸다. 뮤텍스, 공용 창, MSI 비트맵, 포트 목록.
 * - struct apple_pcie_port: 포트 하나. 자기 레지스터 창과 PHY 창, IRQ 도메인,
 *   RID2SID 사용 비트맵.
 *
 * === 이 트리에서 확인할 수 없는 것 ===
 * 이 저장소는 sparse checkout 이라 drivers/{block,nvme,pci,s390,vfio} 만 있다.
 * 따라서 다음은 모두 확인 대상 밖이며, 아래 주석에서 단정한 곳이 없다:
 * Apple AIC 인터럽트 컨트롤러 내부(drivers/irqchip 없음), DART IOMMU(drivers/iommu
 * 없음), GPIO 컨트롤러 구현(drivers/gpio 없음), irqdomain/MSI 코어 구현(kernel/irq
 * 없음), <linux/...> 헤더의 인라인 정의와 매크로 본문(include/ 없음), 그리고
 * 이 하드웨어의 레지스터 사양서(공개 문서가 트리에 없음).
 */

/* [한국어] FIELD_PREP() 를 위해 포함한다. 아래 MSIMAP 항목을 만들 때 벡터 번호를
 * PORT_MSIMAP_TARGET 필드 자리에 밀어 넣는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] devm_fwnode_gpiod_get() 과 gpiod_set_value_cansleep() 을 위해 포함한다.
 * 이 하드웨어에서 PERST#(엔드포인트 리셋)는 컨트롤러 레지스터가 아니라 별도의
 * GPIO 핀에도 걸려 있어, 레지스터 조작과 GPIO 조작을 짝지어 해야 한다. */
#include <linux/gpio/consumer.h>
/* [한국어] ilog2(), ARRAY_SIZE() 같은 범용 매크로를 위해 포함한다.
 * ilog2 는 MSI 벡터 개수를 log2 형태로 레지스터에 넣을 때 쓴다. */
#include <linux/kernel.h>
/* [한국어] readl_relaxed_poll_timeout() 을 위해 포함한다. refclk 응답 비트와
 * 포트 READY 비트를 정해진 시간 안에 폴링하는 데 쓴다 — 잠들 수 있는 판이라
 * 프로세스 컨텍스트(probe)에서만 부를 수 있다. */
#include <linux/iopoll.h>
/* [한국어] chained_irq_enter() / chained_irq_exit() 를 위해 포함한다. 포트 인터럽트를
 * 상위 IRQ 하나에 체인으로 매달아, 상위 핸들러 안에서 하위 도메인으로 분배한다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] MSI 부모 도메인 공용 구현(msi_lib_init_dev_msi_info)과 MSI_FLAG_ 계열
 * 플래그를 위해 포함한다. Kconfig 의 select IRQ_MSI_LIB 가 이 코드를 켜 준다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irq_domain_create_linear(), irq_domain_set_info(), irq_domain_alloc_irqs()
 * 등 IRQ 도메인 API 전반을 위해 포함한다. 이 파일은 도메인을 두 종류 만든다 —
 * 포트별 선형 도메인과 컨트롤러 단위 MSI 부모 도메인. */
#include <linux/irqdomain.h>
/* [한국어] struct list_head 와 list_add_tail()/list_for_each_entry() 를 위해 포함한다.
 * 한 컨트롤러에 딸린 포트들을 연결 리스트로 묶어 둔다. */
#include <linux/list.h>
/* [한국어] module_platform_driver() 와 MODULE_ 계열 매크로를 위해 포함한다.
 * 이 드라이버는 Kconfig 에서 tristate 라 모듈로도 빌드될 수 있다. */
#include <linux/module.h>
/* [한국어] struct msi_msg, msi_create_parent_irq_domain(), struct msi_parent_ops 를
 * 위해 포함한다. MSI 메시지의 주소/데이터를 채우는 콜백이 이 타입을 받는다. */
#include <linux/msi.h>
/* [한국어] irq_of_parse_and_map() 과 of_phandle_args_to_fwspec() 를 위해 포함한다.
 * 전자는 포트 인터럽트를, 후자는 msi-ranges 의 부모 지정자를 DT 에서 끌어온다. */
#include <linux/of_irq.h>
/* [한국어] struct pci_ecam_ops 와 pci_ecam_map_bus() 를 위해 포함한다. 하드웨어가
 * ECAM 을 지키므로 설정공간 접근 구현 전체를 이 헤더 쪽에 맡긴다. */
#include <linux/pci-ecam.h>

/* [한국어] 같은 디렉터리의 공용 라이브러리 헤더. pci_host_common_init() 원형이
 * 여기 있다. 꺾쇠(<>)가 아니라 따옴표인 것은 커널 전역 헤더가 아니라
 * drivers/pci/controller/ 안에서만 쓰이는 내부 헤더이기 때문이다. */
#include "pci-host-common.h"

/* [한국어] 아래 CORE_ / PHY_ / PORT_ 로 시작하는 오프셋 뭉치는 T8103 세대 기준의
 * 레지스터 지도다. 창이 셋으로 나뉘어 있다는 점이 이 하드웨어의 핵심 구조다 —
 * CORE_ 는 컨트롤러 공용 창(pcie->base) 기준, PHY_ 는 포트별 PHY 창(port->phy)
 * 기준, PORT_ 는 포트별 레지스터 창(port->base) 기준 오프셋이다. 같은 파일에서
 * 세 기준이 섞여 쓰이므로, 어떤 베이스에 더하는지를 매번 확인해야 한다.
 * 실제 레지스터 사양서는 공개되어 있지 않고 이 트리에도 없으므로, 아래 설명은
 * 전부 이 파일 안의 사용처에서 읽어 낼 수 있는 범위로만 적었다. */
/* T8103 (original M1) and related SoCs */
/* [한국어] 공용 창의 PHY 인터페이스 제어 레지스터. 이 파일 안에서 참조하지 않는다
 * (정의만 존재). 누가 대신 설정하는지는 이 트리에서 확인 못 함. */
#define CORE_RC_PHYIF_CTL		0x00024
/* [한국어] 위 레지스터의 실행 비트. 역시 이 파일에서 참조하지 않는다. */
#define   CORE_RC_PHYIF_CTL_RUN		BIT(0)
/* [한국어] 공용 창의 PHY 인터페이스 상태 레지스터. 이 파일에서 참조하지 않는다. */
#define CORE_RC_PHYIF_STAT		0x00028
/* [한국어] 그 상태의 refclk 준비 비트(비트 4). 이 파일에서 참조하지 않는다 —
 * refclk 핸드셰이크는 포트별 PHY 창의 PHY_LANE_CFG 쪽에서 이뤄진다. */
#define   CORE_RC_PHYIF_STAT_REFCLK	BIT(4)
/* [한국어] 공용 창의 루트 컴플렉스 제어 레지스터. 이 파일에서 참조하지 않는다. */
#define CORE_RC_CTL			0x00050
/* [한국어] 그 제어의 실행 비트. 이 파일에서 참조하지 않는다. */
#define   CORE_RC_CTL_RUN		BIT(0)
/* [한국어] 공용 창의 루트 컴플렉스 상태 레지스터. 이 파일에서 참조하지 않는다. */
#define CORE_RC_STAT			0x00058
/* [한국어] 그 상태의 준비 완료 비트. 이 파일에서 참조하지 않는다 — 드라이버가
 * 기다리는 준비 비트는 포트별 PORT_STATUS 쪽이다. */
#define   CORE_RC_STAT_READY		BIT(0)
/* [한국어] 공용 창의 패브릭 상태 레지스터. 이 파일에서 참조하지 않는다. */
#define CORE_FABRIC_STAT		0x04000
/* [한국어] 그 상태에서 의미 있는 비트만 남기는 마스크. 상위/하위 16비트에 각각
 * 5비트씩 놓인 모양이다. 이 파일에서 참조하지 않는다. */
#define   CORE_FABRIC_STAT_MASK		0x001F001F

/* [한국어] DT 에 "phyN" 이름의 자원이 없을 때 쓰는 포트 PHY 창의 기본 위치.
 * 공용 창 기준 0x84000 에서 시작해 포트마다 16KB(0x4000)씩 떨어져 있다.
 * apple_pcie_setup_port() 가 named resource 조회에 실패했을 때만 이 식을 쓴다. */
#define CORE_PHY_DEFAULT_BASE(port)	(0x84000 + 0x4000 * (port))

/* [한국어] PHY 창 기준 레인 설정 레지스터. refclk 요청/응답 핸드셰이크와 refclk
 * 인에이블, 클럭 게이팅 허용이 모두 이 한 레지스터에 모여 있다.
 * 오프셋이 0 이라는 점에 주의 — 아래 struct hw_info 는 오프셋 0 을 "없는
 * 레지스터" 라는 뜻으로 쓰는데, PHY 창에서는 0 이 실재하는 레지스터다. */
#define PHY_LANE_CFG			0x00000
/* [한국어] refclk 0 을 달라는 요청 비트. 드라이버가 세우면 하드웨어가 ACK 로 답한다. */
#define   PHY_LANE_CFG_REFCLK0REQ	BIT(0)
/* [한국어] refclk 1 을 달라는 요청 비트. 0 번 ACK 를 받은 뒤에 세운다. */
#define   PHY_LANE_CFG_REFCLK1REQ	BIT(1)
/* [한국어] refclk 0 요청이 받아들여졌음을 알리는 응답 비트. 폴링 대상이다. */
#define   PHY_LANE_CFG_REFCLK0ACK	BIT(2)
/* [한국어] refclk 1 요청의 응답 비트. 역시 폴링 대상이다. */
#define   PHY_LANE_CFG_REFCLK1ACK	BIT(3)
/* [한국어] refclk 출력을 실제로 켜는 비트 둘(9, 10)을 한꺼번에 세우는 마스크.
 * 두 refclk 의 ACK 를 모두 받은 뒤에 이 둘을 동시에 세운다. */
#define   PHY_LANE_CFG_REFCLKEN		(BIT(9) | BIT(10))
/* [한국어] refclk 클럭 게이팅을 허용하는 비트 둘(30, 31). T602x 세대에서 포트
 * 레지스터의 REFCLK 항목이 없을 때(hw->port_refclk == 0) 대신 이쪽을 세운다. */
#define   PHY_LANE_CFG_REFCLKCGEN	(BIT(30) | BIT(31))
/* [한국어] PHY 창 기준 레인 제어 레지스터. T8103 에서만 쓰인다. */
#define PHY_LANE_CTL			0x00004
/* [한국어] PHY 설정 접근 창을 여는 비트(15). refclk 핸드셰이크 앞뒤로 세웠다
 * 지운다. T602x 는 hw->phy_lane_ctl 이 0 이라 이 동작 자체를 건너뛴다. */
#define   PHY_LANE_CTL_CFGACC		BIT(15)

/* [한국어] 포트 창 기준 LTSSM(Link Training and Status State Machine, PCIe 링크
 * 훈련 상태 기계) 제어 레지스터. */
#define PORT_LTSSMCTL			0x00080
/* [한국어] 링크 훈련을 시작시키는 비트. 포트 준비가 끝난 맨 마지막에 쓴다 —
 * 이 쓰기가 곧 링크 업 인터럽트를 기다리기 시작하는 출발선이다. */
#define   PORT_LTSSMCTL_START		BIT(0)
/* [한국어] 포트 인터럽트 상태 레지스터. 비트 번호가 곧 포트 IRQ 도메인의 hwirq
 * 번호다. 그래서 아래 PORT_INT_ 계열 상수들은 마스크가 아니라 비트 '번호' 다. */
#define PORT_INTSTAT			0x00100
/* [한국어] 비트 31: 썬더볼트 터널 오류. 이 파일에서 참조하지 않는다. */
#define   PORT_INT_TUNNEL_ERR		31
/* [한국어] 비트 23: completion timeout — 요청에 대한 응답이 오지 않았다.
 * 이 파일에서 참조하지 않는다(도메인에는 노출되지만 이름으로는 쓰이지 않는다). */
#define   PORT_INT_CPL_TIMEOUT		23
/* [한국어] 비트 22: RID2SID 변환 실패. 매핑 표에 없는 RID 가 요청을 냈다는 뜻이다.
 * 이 파일에서 참조하지 않는다. */
#define   PORT_INT_RID2SID_MAPERR	22
/* [한국어] 비트 21: completion abort. 이 파일에서 참조하지 않는다. */
#define   PORT_INT_CPL_ABORT		21
/* [한국어] 비트 19: MSI 데이터가 허용 범위를 벗어났다. 이 파일에서 참조하지 않는다. */
#define   PORT_INT_MSI_BAD_DATA		19
/* [한국어] 비트 18: MSI 처리 오류. 이 파일에서 참조하지 않는다. */
#define   PORT_INT_MSI_ERR		18
/* [한국어] 비트 17: 요청 주소가 32비트를 넘었다. 이 파일에서 참조하지 않는다. */
#define   PORT_INT_REQADDR_GT32		17
/* [한국어] 비트 15: AF(주소 필터?) 타임아웃. 이름의 정확한 의미는 이 트리에서
 * 확인 못 함. 이 파일에서 참조하지 않는다. */
#define   PORT_INT_AF_TIMEOUT		15
/* [한국어] 비트 14: 링크 다운. apple_pcie_port_register_irqs() 가 이 번호로 IRQ 를
 * 잡고 apple_pcie_port_irq() 가 로그를 남긴다. */
#define   PORT_INT_LINK_DOWN		14
/* [한국어] 비트 12: 링크 업. 위와 같은 방식으로 잡히며, 이 인터럽트가 와야
 * apple_pcie_setup_port() 의 완료 대기가 풀린다. */
#define   PORT_INT_LINK_UP		12
/* [한국어] 비트 11: 링크 대역폭 관리 이벤트. 이 파일에서 참조하지 않는다. */
#define   PORT_INT_LINK_BWMGMT		11
/* [한국어] 비트 4~7 을 덮는 AER(Advanced Error Reporting) 관련 마스크. 위의 다른
 * PORT_INT_ 상수들이 비트 '번호' 인 것과 달리 이것만 마스크 값이다.
 * 이 파일에서 참조하지 않는다. */
#define   PORT_INT_AER_MASK		(15 << 4)
/* [한국어] 비트 4: 포트 오류. 위 AER 마스크의 첫 비트와 같은 자리다.
 * 이 파일에서 참조하지 않는다. */
#define   PORT_INT_PORT_ERR		4
/* [한국어] INTx(레거시 핀 인터럽트) i 번을 hwirq 번호로 바꾸는 매크로 — 항등식이다.
 * 즉 INTA/INTB/INTC/INTD 가 hwirq 0/1/2/3 에 그대로 대응한다.
 * 이 파일에서 참조하지 않는다(정의만 존재). */
#define   PORT_INT_INTx(i)		i
/* [한국어] 하위 4비트가 INTx 라는 사실을 담은 마스크. hwirq_is_intx() 가 이 값으로
 * 어떤 hwirq 가 레벨 트리거여야 하는지 판정한다. */
#define   PORT_INT_INTx_MASK		15
/* [한국어] 포트 인터럽트 마스크 레지스터. 1 이면 막힘. 드라이버는 이 레지스터를
 * 읽고-고쳐-쓰는 방식(rmw_set/rmw_clear)으로 개별 비트를 다룬다. */
#define PORT_INTMSK			0x00104
/* [한국어] 마스크 비트를 세우기만 하는 전용 레지스터. 읽고-고쳐-쓰기 없이
 * 원자적으로 막을 수 있는 창구지만 이 파일에서 참조하지 않는다. */
#define PORT_INTMSKSET			0x00108
/* [한국어] 마스크 비트를 지우기만 하는 전용 레지스터. 역시 참조하지 않는다.
 * [상류 코드 관찰] SET/CLR 레지스터가 있는데도 드라이버는 PORT_INTMSK 를
 * 읽고-고쳐-쓰고, 그 경쟁을 port->lock 스핀락으로 막는다. 왜 그렇게 했는지는
 * 코드와 커밋 메시지 어디에도 없어 이 트리에서 확인 못 함. */
#define PORT_INTMSKCLR			0x0010c
/* [한국어] MSI 설정 레지스터. 활성화 비트와 벡터 개수(log2)를 담는다. */
#define PORT_MSICFG			0x00124
/* [한국어] 그 레지스터의 MSI 활성화 비트. */
#define   PORT_MSICFG_EN		BIT(0)
/* [한국어] 벡터 개수의 log2 값을 넣는 자리(비트 4부터). 예컨대 벡터가 32개면
 * ilog2(32) = 5 가 여기에 들어간다. T602x 는 이 방식 대신 MSIMAP 표를 쓴다. */
#define   PORT_MSICFG_L2MSINUM_SHIFT	4
/* [한국어] 이 포트에 배정된 MSI 벡터 구간의 시작 번호를 담는 레지스터.
 * 모든 포트가 같은 벡터 풀을 공유하므로 드라이버는 항상 0 을 쓴다. */
#define PORT_MSIBASE			0x00128
/* [한국어] 그 레지스터의 두 번째 필드 시작 비트(16). 이 파일에서 참조하지 않는다. */
#define   PORT_MSIBASE_1_SHIFT		16
/* [한국어] T8103 세대의 MSI 도어벨 주소 레지스터(32비트). 장치가 MSI 를 쓸 때
 * 기록할 목적지 주소가 여기 들어간다. */
#define PORT_MSIADDR			0x00168
/* [한국어] 포트 링크 상태 레지스터. 이 파일에서 참조하지 않는다 — 드라이버는
 * 상태를 폴링하는 대신 링크 업/다운 인터럽트를 기다린다. */
#define PORT_LINKSTS			0x00208
/* [한국어] 그 상태의 링크 업 비트. 이 파일에서 참조하지 않는다. */
#define   PORT_LINKSTS_UP		BIT(0)
/* [한국어] 그 상태의 busy 비트. 이 파일에서 참조하지 않는다. */
#define   PORT_LINKSTS_BUSY		BIT(2)
/* [한국어] 링크 명령/상태 레지스터. 포트 IRQ 설정 시 ~0 을 써서 남아 있던 상태를
 * 한꺼번에 지운다(쓰기로 지우는 W1C 계열로 보인다). */
#define PORT_LINKCMDSTS			0x00210
/* [한국어] 미처리 non-posted 요청 카운터 레지스터. 이 파일에서 참조하지 않는다. */
#define PORT_OUTS_NPREQS		0x00284
/* [한국어] 그 카운터의 요청 필드 시작 비트(24). 참조하지 않는다. */
#define   PORT_OUTS_NPREQS_REQ		BIT(24)
/* [한국어] 그 카운터의 완료 필드 시작 비트(16). 참조하지 않는다. */
#define   PORT_OUTS_NPREQS_CPL		BIT(16)
/* [한국어] 수신 쓰기 FIFO 상태 레지스터. 참조하지 않는다. */
#define PORT_RXWR_FIFO			0x00288
/* [한국어] 그 FIFO 의 헤더 칸 수 필드(비트 15..10). 참조하지 않는다. */
#define   PORT_RXWR_FIFO_HDR		GENMASK(15, 10)
/* [한국어] 그 FIFO 의 데이터 칸 수 필드(비트 9..0). 참조하지 않는다. */
#define   PORT_RXWR_FIFO_DATA		GENMASK(9, 0)
/* [한국어] 수신 읽기 FIFO 상태 레지스터. 참조하지 않는다. */
#define PORT_RXRD_FIFO			0x0028C
/* [한국어] 그 FIFO 의 요청 칸 수 필드(비트 6..0). 참조하지 않는다. */
#define   PORT_RXRD_FIFO_REQ		GENMASK(6, 0)
/* [한국어] 미처리 completion 카운터 레지스터. 참조하지 않는다. */
#define PORT_OUTS_CPLS			0x00290
/* [한국어] 그 카운터의 공유 크레딧 필드(비트 14..8). 참조하지 않는다. */
#define   PORT_OUTS_CPLS_SHRD		GENMASK(14, 8)
/* [한국어] 그 카운터의 대기 중 completion 필드(비트 6..0). 참조하지 않는다. */
#define   PORT_OUTS_CPLS_WAIT		GENMASK(6, 0)
/* [한국어] 포트의 애플리케이션 클럭 제어 레지스터. 포트 레지스터에 접근하려면
 * 이 클럭이 먼저 살아 있어야 하므로, 포트 설정의 첫 동작이 이 비트 세우기다. */
#define PORT_APPCLK			0x00800
/* [한국어] 앱클럭을 켜는 비트. */
#define   PORT_APPCLK_EN		BIT(0)
/* [한국어] 앱클럭의 자동 게이팅을 '막는' 비트(CGDIS = clock gating disable).
 * 포트가 준비된 뒤 이 비트를 지워서 절전 게이팅을 다시 허용한다. */
#define   PORT_APPCLK_CGDIS		BIT(8)
/* [한국어] 포트 상태 레지스터. */
#define PORT_STATUS			0x00804
/* [한국어] 포트가 준비되었음을 알리는 비트. PERST# 해제 후 최대 250ms 동안
 * 이 비트를 폴링한다. */
#define   PORT_STATUS_READY		BIT(0)
/* [한국어] T8103 세대의 포트 refclk 제어 레지스터. T602x 에는 이 레지스터가 없어
 * hw_info 의 port_refclk 가 0 이 된다. */
#define PORT_REFCLK			0x00810
/* [한국어] 포트 refclk 를 켜는 비트. */
#define   PORT_REFCLK_EN		BIT(0)
/* [한국어] 포트 refclk 의 자동 게이팅을 막는 비트. 포트가 준비된 뒤 지운다. */
#define   PORT_REFCLK_CGDIS		BIT(8)
/* [한국어] T8103 세대의 PERST# 제어 레지스터. */
#define PORT_PERST			0x00814
/* [한국어] PERST# 를 푸는 비트(OFF = 리셋을 끈다). 이 비트를 세우는 것과 GPIO 를
 * 0 으로 내리는 것을 같이 해야 엔드포인트 리셋이 완전히 풀린다. */
#define   PORT_PERST_OFF		BIT(0)
/* [한국어] T8103 세대의 RID2SID 매핑 표 시작 오프셋. 표의 한 칸이 4바이트이고,
 * i 번 칸은 이 오프셋 + 4*i 에 놓인다. */
#define PORT_RID2SID			0x00828
/* [한국어] 매핑 칸이 유효함을 알리는 비트(31). 이 비트가 없으면 그 칸은 무시된다. */
#define   PORT_RID2SID_VALID		BIT(31)
/* [한국어] SID(IOMMU Stream ID)를 넣는 자리(비트 16부터). */
#define   PORT_RID2SID_SID_SHIFT	16
/* [한국어] RID 중 버스 번호를 넣는 자리(비트 8부터). 이 파일에서 참조하지 않는다 —
 * 드라이버는 pci_dev_id() 가 만든 16비트 RID 를 통째로 하위에 넣는다. */
#define   PORT_RID2SID_BUS_SHIFT	8
/* [한국어] RID 중 장치 번호 자리(비트 3부터). 참조하지 않는다(위와 같은 이유). */
#define   PORT_RID2SID_DEV_SHIFT	3
/* [한국어] RID 중 함수 번호 자리(비트 0부터). 참조하지 않는다(위와 같은 이유). */
#define   PORT_RID2SID_FUNC_SHIFT	0
/* [한국어] 미처리 posted 요청의 헤더 카운터. 참조하지 않는다. */
#define PORT_OUTS_PREQS_HDR		0x00980
/* [한국어] 그 카운터의 유효 비트 범위(9..0). 참조하지 않는다. */
#define   PORT_OUTS_PREQS_HDR_MASK	GENMASK(9, 0)
/* [한국어] 미처리 posted 요청의 데이터 카운터. 참조하지 않는다. */
#define PORT_OUTS_PREQS_DATA		0x00984
/* [한국어] 그 카운터의 유효 비트 범위(15..0). 참조하지 않는다. */
#define   PORT_OUTS_PREQS_DATA_MASK	GENMASK(15, 0)
/* [한국어] 썬더볼트 터널 제어 레지스터. 참조하지 않는다. */
#define PORT_TUNCTRL			0x00988
/* [한국어] 터널 쪽 PERST# 를 거는 비트. 참조하지 않는다. */
#define   PORT_TUNCTRL_PERST_ON		BIT(0)
/* [한국어] 터널 쪽 PERST# 응답을 요구하는 비트. 참조하지 않는다. */
#define   PORT_TUNCTRL_PERST_ACK_REQ	BIT(1)
/* [한국어] 썬더볼트 터널 상태 레지스터. 참조하지 않는다. */
#define PORT_TUNSTAT			0x0098c
/* [한국어] 터널 PERST# 가 걸려 있음을 알리는 비트. 참조하지 않는다. */
#define   PORT_TUNSTAT_PERST_ON		BIT(0)
/* [한국어] 터널 PERST# 응답이 밀려 있음을 알리는 비트. 참조하지 않는다. */
#define   PORT_TUNSTAT_PERST_ACK_PEND	BIT(1)
/* [한국어] prefetchable 메모리 창을 여는 레지스터. 참조하지 않는다.
 * [상류 코드 관찰] 위 PORT_INT_ 계열 다수와 이 레지스터를 포함해, 이 파일에는
 * 정의만 있고 한 번도 쓰이지 않는 매크로가 50개 넘게 있다. 하드웨어 지도를
 * 문서 삼아 통째로 적어 둔 것으로 보이지만, 그 의도를 밝힌 근거는 코드에 없다. */
#define PORT_PREFMEM_ENABLE		0x00994

/* [한국어] 아래 다섯 개는 T602x 세대에서 자리가 옮겨졌거나 새로 생긴 레지스터다.
 * 세대 차이를 #ifdef 가 아니라 struct hw_info 상수표로 흡수하는 것이 이 파일의
 * 설계다 — 그래서 코드에는 세대 분기가 if (hw->xxx) 형태로만 나타난다. */
/* T602x (M2-pro and co) */
/* [한국어] T602x 의 MSI 도어벨 주소 하위 32비트 레지스터. T8103 의 PORT_MSIADDR
 * 와 같은 역할이지만 오프셋이 다르다. */
#define PORT_T602X_MSIADDR	0x016c
/* [한국어] T602x 에만 있는 도어벨 주소 상위 32비트 레지스터. 드라이버는 도어벨을
 * 4GB 아래로 강제하므로 여기에는 항상 0 을 쓴다. */
#define PORT_T602X_MSIADDR_HI	0x0170
/* [한국어] T602x 의 PERST# 제어 레지스터. 비트 정의(PORT_PERST_OFF)는 T8103 과
 * 공유한다 — 자리만 옮겼고 뜻은 같다는 뜻이다. */
#define PORT_T602X_PERST	0x082c
/* [한국어] T602x 의 RID2SID 표 시작 오프셋. */
#define PORT_T602X_RID2SID	0x3000
/* [한국어] T602x 에만 있는 MSI 매핑 표. 벡터마다 한 칸씩 두어 목적지 번호를
 * 직접 적는 방식이라, T8103 의 '시작 번호 + log2(개수)' 방식보다 유연하다. */
#define PORT_T602X_MSIMAP	0x3800

/* [한국어] MSIMAP 한 칸을 유효하게 만드는 비트(31). */
#define PORT_MSIMAP_ENABLE	BIT(31)
/* [한국어] MSIMAP 한 칸에서 목적지 벡터 번호가 놓이는 필드(비트 7..0).
 * FIELD_PREP() 로 값을 이 자리에 밀어 넣는다. */
#define PORT_MSIMAP_TARGET	GENMASK(7, 0)

/*
 * The doorbell address is set to 0xfffff000, which by convention
 * matches what MacOS does, and it is possible to use any other
 * address (in the bottom 4GB, as the base register is only 32bit).
 * However, it has to be excluded from the IOVA range, and the DART
 * driver has to know about it.
 */
/* [한국어] MSI 도어벨의 물리 주소. Kconfig 에서 온다 —
 * drivers/pci/controller/Kconfig 의 PCIE_APPLE_MSI_DOORBELL_ADDR 이며 기본값은
 * 0xfffff000 이다(같은 파일 38~41행). 위 상류 주석이 설명하듯 이 주소는
 * IOVA 범위에서 빼 두어야 하고 DART 드라이버도 이를 알아야 하는데, 그 쪽
 * 코드는 이 트리에 없어 확인 못 함. */
#define DOORBELL_ADDR		CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR

/* [한국어]
 * struct hw_info - SoC 세대별 레지스터 배치 차이만 모아 둔 읽기 전용 상수표
 *
 * 이 구조체가 존재하는 이유: T8103 과 T602x 는 동작 절차가 거의 같고 레지스터
 * 위치와 MSI 배분 방식만 다르다. 그 차이를 코드 곳곳의 #ifdef 나 if (세대) 분기로
 * 흩뿌리는 대신, 오프셋 값 하나로 축약해 여기 모았다. 그래서 본문에는 세대 이름이
 * 한 번도 나오지 않고 if (pcie->hw->port_refclk) 같은 '값이 있느냐' 검사만 남는다.
 *
 * [상류 코드 관찰] 오프셋 0 을 "이 세대에는 그 레지스터가 없다" 는 뜻으로 쓴다.
 * 포트 창의 오프셋 0 이 실제로 무엇인지는 이 파일에 정의가 없어 확인 못 함이지만,
 * PHY 창에서는 오프셋 0(PHY_LANE_CFG)이 실재하는 레지스터이므로 이 관례가 창을
 * 넘나들며 일반화되지는 않는다.
 *
 * 설정자: 파일 끝의 apple_pcie_of_match 표에 .data 로 걸린 두 인스턴스
 *   (t8103_hw, t602x_hw)뿐이며 둘 다 const 다.
 * 읽는 자: apple_pcie_probe() 가 of_device_get_match_data() 로 꺼내 pcie->hw 에
 *   담고, 그 뒤 포트 설정/IRQ 설정/RID2SID 접근 코드가 읽기만 한다.
 * 동기화: 초기화 이후 변하지 않는 상수라 락이 필요 없다.
 */
struct hw_info {
	/* [한국어] PHY 창 기준 레인 제어 레지스터의 오프셋. 0 이면 그 세대에 없다는 뜻.
	 * 설정자: 두 상수표에서만(T8103 은 PHY_LANE_CTL, T602x 는 0).
	 * 읽는 자: apple_pcie_setup_refclk() 이 refclk 핸드셰이크 앞뒤로 설정 접근 비트를
	 *   세우고 지울 때만 쓴다. 0 이면 그 두 동작을 통째로 건너뛴다.
	 * 값 범위: 0 또는 PHY 창 안의 유효 오프셋.
	 * 동기화: 읽기 전용 상수. */
	u32 phy_lane_ctl;
	/* [한국어] MSI 도어벨 주소(하위 32비트) 레지스터의 포트 창 기준 오프셋.
	 * 설정자: 두 상수표(T8103 = 0x168, T602x = 0x16c).
	 * 읽는 자: apple_pcie_port_setup_irq() 이 도어벨 물리 주소를 쓸 때.
	 * 값 범위: 항상 0 이 아닌 유효 오프셋 — 두 세대 모두 이 레지스터를 갖는다.
	 * 동기화: 읽기 전용 상수. */
	u32 port_msiaddr;
	/* [한국어] 도어벨 주소 상위 32비트 레지스터의 오프셋. 0 이면 그 레지스터가 없다.
	 * 설정자: 두 상수표(T8103 = 0, T602x = 0x170).
	 * 읽는 자: apple_pcie_port_setup_irq() 이 0 이 아닐 때만 0 을 써 넣는다 —
	 *   도어벨이 4GB 아래로 고정되어 있어 상위 32비트는 언제나 0 이기 때문이다.
	 * 값 범위: 0 또는 유효 오프셋.
	 * 동기화: 읽기 전용 상수. */
	u32 port_msiaddr_hi;
	/* [한국어] 포트 refclk 제어 레지스터의 오프셋. 0 이면 그 세대에 없다.
	 * 설정자: 두 상수표(T8103 = 0x810, T602x = 0).
	 * 읽는 자: apple_pcie_setup_refclk() 이 refclk 를 켤 때, 그리고
	 *   apple_pcie_setup_port() 이 준비 완료 후 게이팅을 다시 허용할 때.
	 *   0 이면 후자는 PHY 창의 PHY_LANE_CFG_REFCLKCGEN 쪽으로 대체된다.
	 * 값 범위: 0 또는 유효 오프셋.
	 * 동기화: 읽기 전용 상수. */
	u32 port_refclk;
	/* [한국어] PERST# 제어 레지스터의 오프셋.
	 * 설정자: 두 상수표(T8103 = 0x814, T602x = 0x82c).
	 * 읽는 자: apple_pcie_setup_port() 이 PERST# 를 풀 때 한 번.
	 * 값 범위: 두 세대 모두 0 이 아니다.
	 * 동기화: 읽기 전용 상수. */
	u32 port_perst;
	/* [한국어] RID2SID 매핑 표의 시작 오프셋.
	 * 설정자: 두 상수표(T8103 = 0x828, T602x = 0x3000).
	 * 읽는 자: port_rid2sid_addr() 이 이 값 + 4*인덱스로 칸 주소를 만든다.
	 * 값 범위: 두 세대 모두 0 이 아니다.
	 * 동기화: 읽기 전용 상수. 표 자체의 동시 접근은 pcie->lock 이 막는다. */
	u32 port_rid2sid;
	/* [한국어] MSI 목적지 매핑 표의 시작 오프셋. 0 이면 그 방식이 없다는 뜻이고,
	 * 그때는 '시작 번호 + log2(개수)' 방식(MSIBASE/MSICFG)으로 대체된다.
	 * 설정자: 두 상수표(T8103 = 0, T602x = 0x3800).
	 * 읽는 자: apple_pcie_port_setup_irq() 의 MSI 설정 분기.
	 * 값 범위: 0 또는 유효 오프셋.
	 * 동기화: 읽기 전용 상수. */
	u32 port_msimap;
	/* [한국어] RID2SID 표에서 시도해 볼 최대 칸 수. 실제 칸 수는 이 값까지
	 * 하나씩 써 보고 되읽어 확인한다(RAZ/WI 탐지).
	 * 설정자: 두 상수표(T8103 = 64, T602x = 512).
	 * 읽는 자: apple_pcie_setup_port() 의 비트맵 할당 크기와 탐지 루프 상한.
	 * 값 범위: 양수. 실제 하드웨어 칸 수보다 크거나 같아야 한다.
	 * 동기화: 읽기 전용 상수. */
	u32 max_rid2sid;
};

/* [한국어] T8103(초대 M1) 세대의 레지스터 배치. 파일 끝 매칭 표에서
 * compatible "apple,pcie" 에 연결된다. */
static const struct hw_info t8103_hw = {
	/* [한국어] 이 세대에는 레인 제어 레지스터가 있어 refclk 핸드셰이크 전후로
	 * 설정 접근 비트를 여닫아야 한다. */
	.phy_lane_ctl		= PHY_LANE_CTL,
	/* [한국어] 도어벨 주소 레지스터는 0x168. */
	.port_msiaddr		= PORT_MSIADDR,
	/* [한국어] 0 = 상위 32비트 레지스터가 없다. 도어벨이 4GB 아래라 필요도 없다. */
	.port_msiaddr_hi	= 0,
	/* [한국어] 포트 refclk 제어 레지스터가 존재한다. */
	.port_refclk		= PORT_REFCLK,
	/* [한국어] PERST# 레지스터는 0x814. */
	.port_perst		= PORT_PERST,
	/* [한국어] RID2SID 표는 0x828 부터. */
	.port_rid2sid		= PORT_RID2SID,
	/* [한국어] 0 = MSIMAP 표가 없다. 대신 MSIBASE/MSICFG 방식으로 벡터를 배분한다. */
	.port_msimap		= 0,
	/* [한국어] 표 크기 탐지의 상한 64칸. 실제 칸 수는 탐지 루프가 정한다. */
	.max_rid2sid		= 64,
};

/* [한국어] T602x 세대의 레지스터 배치. 매칭 표에서 compatible
 * "apple,t6020-pcie" 에 연결된다. */
static const struct hw_info t602x_hw = {
	/* [한국어] 0 = 이 세대에는 레인 제어 레지스터가 없어 설정 접근 비트 조작을
	 * 통째로 건너뛴다. */
	.phy_lane_ctl		= 0,
	/* [한국어] 도어벨 주소 레지스터가 0x16c 로 옮겨졌다. */
	.port_msiaddr		= PORT_T602X_MSIADDR,
	/* [한국어] 상위 32비트 레지스터가 새로 생겼다. 값은 항상 0 을 쓴다. */
	.port_msiaddr_hi	= PORT_T602X_MSIADDR_HI,
	/* [한국어] 0 = 포트 refclk 제어 레지스터가 없다. 게이팅 재허용은 PHY 쪽
	 * PHY_LANE_CFG_REFCLKCGEN 으로 대신한다. */
	.port_refclk		= 0,
	/* [한국어] PERST# 레지스터가 0x82c 로 옮겨졌다. 비트 뜻은 그대로다. */
	.port_perst		= PORT_T602X_PERST,
	/* [한국어] RID2SID 표가 0x3000 으로 옮겨졌다. */
	.port_rid2sid		= PORT_T602X_RID2SID,
	/* [한국어] MSIMAP 표가 새로 생겼다. 벡터마다 목적지를 한 칸씩 적는 방식이다. */
	.port_msimap		= PORT_T602X_MSIMAP,
	/* [한국어] 옆의 상류 주석대로, 실제 칸 수는 16 인데 상한을 512 로 크게 잡아 두고
	 * 탐지 루프가 알아서 멈추게 한 것이다. 탐지는 0xbad1d 를 써 보고 그대로 되읽히는지
	 * 보는 방식이라, 없는 칸(RAZ/WI = 읽으면 0, 쓰면 무시)에서 저절로 끝난다. */
	/* 16 on t602x, guess for autodetect on future HW */
	.max_rid2sid		= 512,
};

/* [한국어]
 * struct apple_pcie - PCIe 컨트롤러 하나(= 이 드라이버 인스턴스 하나)의 상태
 *
 * 브리지의 사설 데이터 영역에 놓인다. devm_pci_alloc_host_bridge(dev, sizeof(*pcie))
 * 로 브리지 뒤에 함께 할당되므로, pci_host_bridge_priv() 로 브리지에서 이 구조체를,
 * 반대로는 컨테이너 관계로 브리지를 되찾을 수 있다. 수명은 브리지와 같다.
 *
 * 이 구조체가 담는 것은 크게 셋이다 — 컨트롤러 공용 자원(dev, base, hw),
 * MSI 벡터 배분 상태(bitmap, fwspec, nvecs), 그리고 포트 목록과 링크 업 대기용
 * 완료 객체(ports, event).
 */
struct apple_pcie {
	/* [한국어] MSI 벡터 비트맵과 포트별 RID2SID 비트맵을 함께 지키는 뮤텍스.
	 * 설정자: apple_pcie_probe() 의 mutex_init().
	 * 읽는 자: apple_msi_domain_alloc()/free() 가 MSI 비트맵을 만질 때,
	 *   apple_pcie_enable_device()/disable_device() 가 포트의 RID2SID 비트맵을 만질 때.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: 뮤텍스이므로 잠들 수 있는 문맥에서만 잡을 수 있다 — 위 네 함수가 모두
	 *   프로세스 컨텍스트라 성립한다. 포트별 비트맵까지 이 컨트롤러 단위 락 하나로
	 *   덮는 점에 주의(포트마다 따로 두지 않았다). */
	struct mutex		lock;
	/* [한국어] 플랫폼 디바이스의 struct device. devm 할당의 수명 기준이자 로그 대상.
	 * 설정자: apple_pcie_probe().
	 * 읽는 자: 이 파일의 거의 모든 함수(devm_ 할당, dev_err/dev_dbg, of_node 접근).
	 * 값 범위: 항상 유효(NULL 불가).
	 * 동기화: 설정 후 읽기 전용. */
	struct device		*dev;
	/* [한국어] 컨트롤러 공용 레지스터 창의 커널 가상 주소(DT reg 인덱스 1).
	 * 설정자: apple_pcie_probe() 의 devm_platform_ioremap_resource(pdev, 1).
	 *   인덱스가 1 인 이유는 reg 0번이 ECAM 설정공간 창이기 때문이다.
	 * 읽는 자: apple_pcie_setup_port() 이 DT 에 phyN 자원이 없을 때 기본 PHY 창
	 *   주소를 계산하는 데만 쓴다 — CORE_ 레지스터들이 모두 미사용이라 그 외 용도가 없다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 읽기 전용. */
	void __iomem            *base;
	/* [한국어] 이 SoC 세대의 레지스터 배치 상수표.
	 * 설정자: apple_pcie_probe() 의 of_device_get_match_data(). NULL 이면 -ENODEV.
	 * 읽는 자: refclk 설정, 포트 IRQ 설정, RID2SID 주소 계산 등 세대 차이가 있는 모든 곳.
	 * 값 범위: &t8103_hw 또는 &t602x_hw.
	 * 동기화: const 상수를 가리키므로 읽기 전용. */
	const struct hw_info	*hw;
	/* [한국어] MSI 벡터 사용 현황 비트맵. 1 비트가 벡터 하나다.
	 * 설정자: apple_msi_init() 의 devm_bitmap_zalloc(nvecs). 이후
	 *   apple_msi_domain_alloc()/free() 가 비트를 잡고 놓는다.
	 * 읽는 자: 같은 두 함수. bitmap_find_free_region() 은 2의 거듭제곱 개수를
	 *   정렬된 위치에서 찾아 주므로 multi-MSI(연속 벡터 요구)를 만족시킨다.
	 * 값 범위: nvecs 비트짜리 배열. 모두 0 에서 시작.
	 * 동기화: pcie->lock 뮤텍스 아래에서만 만진다. */
	unsigned long		*bitmap;
	/* [한국어] 이 컨트롤러에 딸린 apple_pcie_port 들의 머리.
	 * 설정자: apple_pcie_probe() 의 INIT_LIST_HEAD, apple_pcie_setup_port() 의 list_add_tail.
	 * 읽는 자: apple_pcie_get_port() 이 장치가 속한 루트 포트를 찾을 때 순회한다.
	 * 값 범위: 비어 있을 수도 있다(DT 에 자식 포트 노드가 없는 경우).
	 * 동기화: [상류 코드 관찰] 추가는 probe 경로에서만 일어나고 순회는 그 뒤에만
	 *   일어나므로 실질적 경쟁이 없지만, 이 리스트를 지키는 명시적 락은 코드에 없다.
	 *   순회하는 apple_pcie_get_port() 은 pcie->lock 을 잡지 않은 상태로 돈다. */
	struct list_head	ports;
	/* [한국어] 링크 업 인터럽트를 기다리는 완료 객체.
	 * 설정자: apple_pcie_setup_port() 이 포트마다 init_completion() 으로 새로 초기화하고,
	 *   apple_pcie_port_irq() 가 링크 업 인터럽트에서 complete_all() 로 깨운다.
	 * 읽는 자: apple_pcie_setup_port() 의 wait_for_completion_timeout(HZ/10).
	 * 값 범위: 초기화된 completion.
	 * 동기화: completion 자체가 내부 스핀락을 갖는다.
	 *   [상류 코드 관찰] 이 객체는 포트마다가 아니라 컨트롤러마다 하나뿐인데,
	 *   포트를 설정할 때마다 init_completion() 으로 다시 초기화한다. 포트를 순차로
	 *   하나씩 세우는 현재 흐름에서는 문제가 없지만, 이전 포트의 링크 업이 늦게
	 *   도착하면 다음 포트의 대기를 대신 풀어 줄 수 있는 구조다. */
	struct completion	event;
	/* [한국어] MSI 의 부모(상위 인터럽트 컨트롤러) 인터럽트 지정자 원본.
	 * 설정자: apple_msi_init() 이 DT 의 msi-ranges 첫 항목을
	 *   of_phandle_args_to_fwspec() 로 옮겨 담는다.
	 * 읽는 자: apple_msi_domain_alloc() 이 이것을 복사한 뒤 벡터 번호만 더해
	 *   부모 도메인에 할당을 요청한다.
	 * 값 범위: fwnode 와 param_count 개의 셀. 셀의 뜻은 부모 컨트롤러가 정하며,
	 *   그 컨트롤러 드라이버는 이 트리에 없어 확인 못 함.
	 * 동기화: 설정 후 읽기 전용(복사해서 쓰므로 원본은 변하지 않는다). */
	struct irq_fwspec	fwspec;
	/* [한국어] 이 컨트롤러가 쓸 수 있는 MSI 벡터의 총 개수.
	 * 설정자: apple_msi_init() 이 msi-ranges 의 마지막 셀에서 읽는다.
	 * 읽는 자: 비트맵 크기, MSI 도메인 크기, 그리고 포트 MSI 설정
	 *   (MSIMAP 표의 항목 수 또는 ilog2(nvecs)).
	 * 값 범위: 양수. MSIBASE/MSICFG 방식(T8103)에서는 ilog2 를 취하므로 2의 거듭제곱이어야
	 *   의미가 맞는다 — 그 검증은 코드에 없다.
	 * 동기화: probe 이후 변하지 않는다. */
	u32			nvecs;
};

/* [한국어]
 * struct apple_pcie_port - 루트 포트 하나의 상태
 *
 * 이 하드웨어의 특징이 그대로 드러나는 구조체다. 포트마다 (1) 자기만의 레지스터
 * 창, (2) 자기만의 PHY 창, (3) 자기만의 32칸 IRQ 도메인, (4) 자기만의 RID2SID
 * 표를 갖는다. 즉 '포트' 가 단순한 논리 단위가 아니라 하드웨어 자원 묶음이다.
 * devm_kzalloc 으로 잡히므로 수명은 컨트롤러 디바이스와 같고, 명시적 해제 경로가
 * 없다(이 드라이버는 suppress_bind_attrs 로 언바인드를 막아 둔다).
 */
struct apple_pcie_port {
	/* [한국어] 이 포트의 인터럽트 마스크 레지스터를 읽고-고쳐-쓰는 동안 잡는 스핀락.
	 * 설정자: apple_pcie_setup_port() 의 raw_spin_lock_init().
	 * 읽는 자: apple_port_irq_mask()/unmask() 가 guard(raw_spinlock_irqsave) 로 잡는다.
	 * 값 범위: 초기화된 raw 스핀락.
	 * 동기화: raw_ 판인 이유는 이 락이 인터럽트 처리 경로에서 잡히기 때문이다 —
	 *   PREEMPT_RT 에서도 잠들지 않는 진짜 스핀락이어야 한다. irqsave 판을 쓰는 것은
	 *   같은 CPU 에서 인터럽트가 끼어들어 재진입하는 것을 막기 위해서다. */
	raw_spinlock_t		lock;
	/* [한국어] 이 포트를 소유한 컨트롤러로 거슬러 올라가는 역포인터.
	 * 설정자: apple_pcie_setup_port().
	 * 읽는 자: 세대 상수표(hw), 뮤텍스, 완료 객체, dev 에 닿아야 하는 모든 곳.
	 * 값 범위: 항상 유효(NULL 불가).
	 * 동기화: 설정 후 읽기 전용. */
	struct apple_pcie	*pcie;
	/* [한국어] 이 포트를 서술한 DT 노드.
	 * 설정자: apple_pcie_setup_port() 이 대입하고, 성공 경로에서 of_node_get() 으로
	 *   참조 카운트를 하나 올려 둔다 — 순회용 스코프 매크로가 반복 끝에 노드를
	 *   놓아 버리기 때문에, 계속 들고 있으려면 별도 참조가 필요하다.
	 * 읽는 자: IRQ 도메인의 fwnode, 로그의 %pOF 출력.
	 * 값 범위: 유효한 device_node 포인터.
	 * 동기화: OF 코어가 참조 카운트로 관리한다. */
	struct device_node	*np;
	/* [한국어] 이 포트 전용 레지스터 창의 가상 주소.
	 * 설정자: apple_pcie_setup_port() 이 "portN" 이름 자원을 먼저 찾고, 없으면
	 *   인덱스 (포트번호 + 2) 의 MEM 자원으로 대신한다. +2 인 근거는 reg 0번이 ECAM,
	 *   1번이 공용 창이기 때문이다(probe 가 인덱스 1 을 매핑한다).
	 * 읽는 자: PORT_ 로 시작하는 모든 오프셋 접근.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 인터럽트 마스크 레지스터만 port->lock 으로, RID2SID 표는 pcie->lock 으로
	 *   보호된다. 나머지 접근은 probe 경로 단독이라 락이 없다. */
	void __iomem		*base;
	/* [한국어] 이 포트의 PHY 레지스터 창 가상 주소.
	 * 설정자: apple_pcie_setup_port() 이 "phyN" 이름 자원을 찾아 매핑하거나,
	 *   없으면 공용 창 기준 CORE_PHY_DEFAULT_BASE(idx) 오프셋으로 계산해 넣는다.
	 *   후자의 경우 별도 ioremap 없이 pcie->base 안쪽을 가리키는 포인터가 된다.
	 * 읽는 자: apple_pcie_setup_refclk() 과 게이팅 재허용 코드.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: probe 경로 단독. */
	void __iomem		*phy;
	/* [한국어] 이 포트의 32칸짜리 인터럽트 도메인. hwirq 번호가 곧 INTSTAT 의 비트 번호다.
	 * 설정자: apple_pcie_port_setup_irq() 의 irq_domain_create_linear(fwnode, 32, ...).
	 * 읽는 자: apple_port_irq_handler() 가 상태 비트마다 generic_handle_domain_irq() 로
	 *   분배할 때, apple_pcie_port_register_irqs() 가 링크 업/다운 IRQ 를 잡을 때.
	 * 값 범위: 유효한 도메인 포인터. 생성 실패 시 -ENOMEM 으로 포트 설정이 중단된다.
	 * 동기화: 도메인 내부 자료구조는 irqdomain 코어가 지킨다. */
	struct irq_domain	*domain;
	/* [한국어] pcie->ports 리스트에 이 포트를 매다는 노드.
	 * 설정자: apple_pcie_setup_port() 의 list_add_tail().
	 * 읽는 자: apple_pcie_get_port() 의 순회.
	 * 값 범위: 유효한 리스트 노드. 떼어 내는 경로는 이 파일에 없다.
	 * 동기화: 위 pcie->ports 설명 참조 — 명시적 락이 없다. */
	struct list_head	entry;
	/* [한국어] RID2SID 표에서 어떤 칸이 쓰이고 있는지 표시하는 비트맵.
	 * 설정자: apple_pcie_setup_port() 의 devm_bitmap_zalloc(hw->max_rid2sid).
	 *   이후 apple_pcie_enable_device() 가 칸을 잡고 disable_device() 가 놓는다.
	 * 읽는 자: 같은 두 함수. 해제 쪽은 for_each_set_bit 로 쓰이고 있는 칸만 훑는다.
	 * 값 범위: max_rid2sid 비트. 다만 실제로 쓰는 범위는 아래 sid_map_sz 까지다.
	 * 동기화: pcie->lock 뮤텍스(포트별 락이 아니라 컨트롤러 락이다) 아래에서만 만진다. */
	unsigned long		*sid_map;
	/* [한국어] 이 포트에 실제로 존재하는 RID2SID 칸의 개수.
	 * 설정자: apple_pcie_setup_port() 의 탐지 루프 — 0xbad1d 를 써 보고 그대로
	 *   되읽히지 않는 첫 칸에서 멈춘 뒤 그 인덱스를 저장한다. 없는 칸은 RAZ/WI
	 *   (Read As Zero / Write Ignored)라 되읽기가 어긋나므로 경계를 알 수 있다.
	 * 읽는 자: bitmap_find_free_region() 과 for_each_set_bit() 의 상한.
	 * 값 범위: 0 이상 hw->max_rid2sid 이하. 0 이면 이 포트는 매핑을 배정할 수 없다.
	 * 동기화: probe 시 한 번 정해지고 이후 읽기 전용. */
	int			sid_map_sz;
	/* [한국어] 이 포트의 번호. 루트 포트의 PCI 장치 번호와 같은 값이다.
	 * 설정자: apple_pcie_setup_port() 이 DT reg 의 첫 셀을 11비트 오른쪽으로 민 값.
	 *   그 셀은 OF PCI 바인딩의 주소 상위 워드이고 장치 번호가 비트 15..11 에 놓인다.
	 *   버스 번호가 0 인 루트 포트라 그 위 비트가 모두 0 이어서 >> 11 만으로 장치
	 *   번호가 된다. 바인딩 문서 자체는 이 트리에 없지만, apple_pcie_get_port() 이
	 *   이 값을 PCI_SLOT(port_pdev->devfn) 과 직접 비교한다는 점이 그 해석을 뒷받침한다.
	 * 읽는 자: 자원 이름 조합("portN"/"phyN"), 자원 인덱스(idx + 2), 포트 인터럽트
	 *   인덱스, 기본 PHY 창 계산, 그리고 위 장치 번호 비교.
	 * 값 범위: 0 이상. 상한 검사는 코드에 없다.
	 * 동기화: 설정 후 읽기 전용. */
	int			idx;
};

/* [한국어]
 * rmw_set - MMIO 레지스터의 특정 비트들을 세운다 (read-modify-write)
 *
 * @set: 세울 비트들의 마스크.
 * @addr: 대상 레지스터의 가상 주소.
 * @return: 없음.
 *
 * 이 하드웨어의 제어 레지스터 대부분은 여러 기능 비트를 한 워드에 모아 두므로,
 * 한 비트만 바꾸려면 읽고-고쳐-쓰기가 필요하다. 그 세 줄을 매번 쓰지 않으려고
 * 만든 한 줄짜리 도우미다.
 * relaxed 판(readl_relaxed/writel_relaxed)을 쓰는 이유는 이 접근들이 다른 메모리
 * 접근과의 순서를 강제할 필요가 없기 때문이다 — 순서가 중요한 곳에서는 뒤이어
 * 되읽기(apple_pcie_rid2sid_write)나 폴링으로 완료를 확인한다.
 * 실행 컨텍스트: 호출자에 따라 다르다. probe 경로에서도 불리고, 인터럽트
 * 마스크 조작 경로(apple_port_irq_mask)에서는 스핀락을 쥔 채 불린다. 이 함수
 * 자체는 잠들지 않는다.
 * 에러 경로: 없다 — MMIO 쓰기는 실패를 알려 주지 않는다.
 *
 * 호출 체인:
 *   apple_port_irq_mask() / apple_pcie_setup_refclk() / apple_pcie_setup_port()
 *     → [rmw_set] → readl_relaxed(), writel_relaxed()
 */
static void rmw_set(u32 set, void __iomem *addr)
{
	/* [한국어] 현재 값을 읽어 set 비트를 OR 로 얹은 뒤 같은 주소에 되쓴다.
	 * 이 연산은 원자적이지 않다 — 같은 레지스터를 두 문맥이 동시에 만지면 갱신이
	 * 유실될 수 있으므로, 인터럽트 마스크처럼 경쟁이 있는 곳은 호출자가 port->lock 을
	 * 잡고 부른다. */
	writel_relaxed(readl_relaxed(addr) | set, addr);
}

/* [한국어]
 * rmw_clear - MMIO 레지스터의 특정 비트들을 지운다 (read-modify-write)
 *
 * @clr: 지울 비트들의 마스크.
 * @addr: 대상 레지스터의 가상 주소.
 * @return: 없음.
 *
 * rmw_set() 의 짝이다. 존재 이유와 주의점(원자적이지 않음, relaxed 접근)이 모두
 * 같으므로 그쪽 설명을 참조하라.
 * 실행 컨텍스트: apple_port_irq_unmask() 에서는 port->lock 을 쥔 채, refclk 설정과
 * 게이팅 재허용에서는 probe 경로에서 불린다. 잠들지 않는다.
 *
 * 호출 체인:
 *   apple_port_irq_unmask() / apple_pcie_setup_refclk() / apple_pcie_setup_port()
 *     → [rmw_clear] → readl_relaxed(), writel_relaxed()
 */
static void rmw_clear(u32 clr, void __iomem *addr)
{
	/* [한국어] 현재 값에서 clr 비트만 떨어뜨려 되쓴다. ~clr 로 마스크를 뒤집어 AND 한다. */
	writel_relaxed(readl_relaxed(addr) & ~clr, addr);
}

/* [한국어]
 * apple_msi_compose_msg - 장치가 쓸 MSI 메시지(주소와 데이터)를 채운다
 *
 * @data: 이 MSI 인터럽트의 irq_data. hwirq 에 배정된 벡터 번호가 들어 있다.
 * @msg: 채워 넣을 MSI 메시지. 호출자가 장치의 MSI/MSI-X 설정공간에 써 넣는다.
 * @return: 없음.
 *
 * MSI 는 인터럽트를 전용 신호선이 아니라 '정해진 주소에 정해진 값을 쓰는 메모리
 * 트랜잭션' 으로 보내는 방식이다. 따라서 장치에게 (1) 어디에 쓸지(주소)와
 * (2) 무엇을 쓸지(데이터)를 알려 줘야 하고, 그 두 값을 만드는 것이 이 콜백이다.
 * 이 하드웨어에서는 목적지 주소가 컨트롤러 전체에 하나뿐인 고정 도어벨이고,
 * 어떤 인터럽트인지는 쓰는 '값' 으로 구분한다. 그래서 데이터에 벡터 번호가
 * 그대로 들어간다 — 포트 레지스터에 새겨 둔 도어벨 주소와 짝을 이루는 설계다.
 * 실행 컨텍스트: 장치가 MSI 를 요청할 때 MSI 코어가 부른다. 프로세스 컨텍스트.
 * 에러 경로: 없다 — 실패할 수 있는 동작이 없다.
 *
 * 호출 체인:
 *   (MSI 코어의 irq_chip_compose_msi_msg 경로) → [apple_msi_compose_msg]
 */
static void apple_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)
{
	/* [한국어] 도어벨 주소의 상위 32비트. 아래 BUILD_BUG_ON 이 도어벨을 4GB 아래로
	 * 강제하므로 이 값은 언제나 0 이다. 그래도 계산식을 남겨 둔 것은 도어벨 주소가
	 * Kconfig 로 바뀔 수 있는 값이기 때문이다. */
	msg->address_hi = upper_32_bits(DOORBELL_ADDR);
	/* [한국어] 도어벨 주소의 하위 32비트. 포트 레지스터에 써 넣은 값과 같아야 한다. */
	msg->address_lo = lower_32_bits(DOORBELL_ADDR);
	/* [한국어] 메시지 데이터에 벡터 번호(hwirq)를 그대로 싣는다. 장치가 이 값을
	 * 도어벨 주소에 쓰면 컨트롤러가 그것을 벡터 번호로 해석한다. */
	msg->data = data->hwirq;
}

/* [한국어] MSI 계층의 '아래쪽' irq_chip. MSI 도메인은 계층 구조라 이 칩이 실제
 * 하드웨어를 만지지 않고, 마스크/언마스크/EOI/어피니티를 모두 부모(상위 인터럽트
 * 컨트롤러)에 그대로 넘긴다. 이 파일이 직접 구현하는 것은 메시지 합성 하나뿐이다 —
 * 그것이 이 하드웨어에서 유일하게 고유한 부분이기 때문이다. */
static struct irq_chip apple_msi_bottom_chip = {
	/* [한국어] /proc/interrupts 등에 보일 칩 이름. */
	.name			= "MSI",
	/* [한국어] 마스크 요청을 부모 도메인으로 그대로 전달한다. */
	.irq_mask		= irq_chip_mask_parent,
	/* [한국어] 언마스크도 부모로 전달한다. */
	.irq_unmask		= irq_chip_unmask_parent,
	/* [한국어] EOI(인터럽트 처리 완료 통지)도 부모로 전달한다. 아래 msi_parent_ops 가
	 * MSI_CHIP_FLAG_SET_EOI 를 선언하는 것과 짝을 이룬다. */
	.irq_eoi		= irq_chip_eoi_parent,
	/* [한국어] 인터럽트를 처리할 CPU 지정도 부모가 결정한다. */
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	/* [한국어] 트리거 타입 설정도 부모로 전달한다. */
	.irq_set_type		= irq_chip_set_type_parent,
	/* [한국어] 유일하게 이 드라이버가 구현하는 콜백 — 도어벨 주소와 벡터 번호로
	 * MSI 메시지를 만든다. */
	.irq_compose_msi_msg	= apple_msi_compose_msg,
};

/* [한국어]
 * apple_msi_domain_alloc - MSI 벡터를 요청한 만큼 잡아 계층 도메인에 등록한다
 *
 * @domain: 이 드라이버가 만든 MSI 부모 도메인.
 * @virq: 코어가 배정한 리눅스 IRQ 번호의 시작값.
 * @nr_irqs: 요청한 인터럽트 개수. multi-MSI 면 2 이상이 온다.
 * @args: 쓰이지 않는다 — [상류 코드 관찰] 시그니처상 받지만 본문에서 참조하지
 *        않는다. 부모에게 넘길 지정자를 args 가 아니라 pcie->fwspec 사본에서
 *        만들기 때문이다.
 * @return: 0 성공, -ENOSPC(빈 벡터 없음), 또는 부모 할당 실패 코드.
 *
 * 이 함수가 필요한 이유: 이 하드웨어의 MSI 는 독립된 인터럽트가 아니라 상위
 * 인터럽트 컨트롤러의 벡터 몇 개를 미리 떼어 받은 것이다. 그래서 MSI 하나를
 * 만들려면 (1) 우리 비트맵에서 벡터 번호를 잡고, (2) 그 번호에 해당하는 상위
 * 컨트롤러 인터럽트를 부모 도메인에 요청하고, (3) 두 계층을 이어 붙여야 한다.
 * multi-MSI 는 연속되고 정렬된 벡터를 요구하므로 bitmap_find_free_region() 을
 * 쓴다 — 이 함수는 2의 거듭제곱 크기 영역을 그 크기에 정렬된 위치에서만 찾는다.
 * 실행 컨텍스트: 장치 드라이버가 MSI 를 요청할 때 프로세스 컨텍스트에서 불린다.
 * 뮤텍스를 잡으므로 잠들 수 있는 문맥이어야 한다.
 * 에러 경로: 벡터가 없으면 -ENOSPC 로 곧장 빠진다. 부모 할당이 실패하면 그 코드를
 * 그대로 돌려주는데, [상류 코드 관찰] 이때 이미 잡아 둔 비트맵 영역을 되돌리지
 * 않는다 — 실패한 벡터가 영구히 새는 구조로 보인다.
 *
 * 호출 체인:
 *   (MSI 코어의 도메인 할당) → [apple_msi_domain_alloc]
 *     → bitmap_find_free_region(), irq_domain_alloc_irqs_parent(),
 *       irq_domain_set_hwirq_and_chip()
 */
static int apple_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	/* [한국어] 도메인 생성 시 host_data 로 심어 둔 컨트롤러 객체를 되찾는다. */
	struct apple_pcie *pcie = domain->host_data;
	/* [한국어] 부모 지정자를 '값으로' 복사한다. 아래에서 벡터 번호를 더해 고칠 것이므로
	 * 원본(pcie->fwspec)을 건드리면 안 되기 때문이다. */
	struct irq_fwspec fwspec = pcie->fwspec;
	/* [한국어] 아래 등록 루프의 인덱스. */
	unsigned int i;
	/* [한국어] ret 은 부모 할당 결과, hwirq 는 비트맵에서 잡은 벡터 시작 번호.
	 * hwirq 가 int 인 것은 음수(-ENOSPC 등)로 실패를 나타내기 때문이다. */
	int ret, hwirq;

	/* [한국어] 벡터 비트맵을 만지기 전에 컨트롤러 뮤텍스를 잡는다. 여러 장치가 동시에
	 * MSI 를 요청하면 같은 벡터를 두 번 배정할 수 있기 때문이다. */
	mutex_lock(&pcie->lock);

	/* [한국어] 연속·정렬된 nr_irqs 개의 빈 벡터를 찾아 잡는다. order_base_2() 로
	 * 개수를 2의 거듭제곱 지수로 바꿔 넘기는 것은, multi-PCI-MSI 규격이 벡터 개수를
	 * 2의 거듭제곱으로 제한하고 시작 번호도 그 개수에 정렬되기를 요구하기 때문이다.
	 * 성공하면 시작 벡터 번호를, 빈 자리가 없으면 음수를 돌려준다. */
	hwirq = bitmap_find_free_region(pcie->bitmap, pcie->nvecs,
					order_base_2(nr_irqs));

	/* [한국어] 비트맵 조작이 끝났으니 곧바로 놓는다 — 아래 부모 할당은 오래 걸릴 수
	 * 있고 뮤텍스를 쥔 채 할 이유가 없다. */
	mutex_unlock(&pcie->lock);

	/* [한국어] 빈 벡터가 없었다면 여기서 끝난다. */
	if (hwirq < 0)
		return -ENOSPC;

	/* [한국어] 부모 지정자의 '마지막에서 두 번째' 셀에 벡터 번호를 더한다. 이것이
	 * "msi-ranges 가 준 시작 인터럽트에서 hwirq 만큼 떨어진 인터럽트" 를 가리키는
	 * 방법이다. 세 셀짜리 지정자(종류, 번호, 플래그)라면 '번호' 셀에 해당하는데,
	 * 부모 컨트롤러의 #interrupt-cells 실제 값은 DT 에 달렸고 그 드라이버가 이
	 * 트리에 없어 확인 못 함 — 코드가 param_count 에서 역산하는 이유도 그것이다. */
	fwspec.param[fwspec.param_count - 2] += hwirq;

	/* [한국어] 부모 도메인에 실제 인터럽트를 잡아 달라고 요청한다. 계층 도메인에서
	 * 아래 계층이 위 계층 자원을 확보하는 표준 절차다. */
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &fwspec);
	/* [한국어] 부모가 실패하면 그대로 전달한다(위 에러 경로 관찰 참조). */
	if (ret)
		return ret;

	/* [한국어] 잡은 벡터 하나하나를 이 도메인의 IRQ 에 연결한다. */
	for (i = 0; i < nr_irqs; i++) {
		/* [한국어] virq + i 번 IRQ 의 hwirq 를 hwirq + i 로, 칩을 apple_msi_bottom_chip 으로,
		 * 칩 데이터를 컨트롤러 객체로 설정한다. 이 등록이 있어야 나중에 메시지 합성
		 * 콜백이 올바른 벡터 번호를 볼 수 있다. */
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &apple_msi_bottom_chip, pcie);
	}

	/* [한국어] 전부 성공. */
	return 0;
}

/* [한국어]
 * apple_msi_domain_free - 잡았던 MSI 벡터를 비트맵에 돌려준다
 *
 * @domain: MSI 부모 도메인.
 * @virq: 해제할 IRQ 번호의 시작값.
 * @nr_irqs: 해제할 개수.
 * @return: 없음 — 해제 경로라 실패를 전할 곳이 없다.
 *
 * alloc 의 짝이다. 시작 벡터 번호를 인자로 받지 않으므로, virq 의 irq_data 에서
 * hwirq 를 되찾아 그 자리를 비운다.
 * 실행 컨텍스트: 장치가 MSI 를 반납할 때 프로세스 컨텍스트에서 불린다.
 * 뮤텍스를 잡으므로 잠들 수 있어야 한다.
 *
 * [상류 코드 관찰] 두 가지가 눈에 띈다. 첫째, irq_domain_get_irq_data() 의
 * 반환값을 NULL 검사 없이 곧바로 역참조한다. 둘째, alloc 이
 * irq_domain_alloc_irqs_parent() 로 부모 자원을 잡았는데 여기에는 대응하는
 * 부모 해제 호출이 없다. 계층 도메인 해제 규약이 부모까지 자동으로 내려가는지는
 * kernel/irq 가 이 트리에 없어 확인 못 함이므로, 누수인지 아닌지는 단정하지 않는다.
 *
 * 호출 체인:
 *   (MSI 코어의 도메인 해제) → [apple_msi_domain_free]
 *     → irq_domain_get_irq_data(), bitmap_release_region()
 */
static void apple_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	/* [한국어] 시작 IRQ 의 irq_data 를 얻는다. 여기 담긴 hwirq 가 곧 비트맵에서의
	 * 벡터 시작 번호다(alloc 이 그렇게 심어 두었다). */
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	/* [한국어] 비트맵을 가진 컨트롤러 객체. */
	struct apple_pcie *pcie = domain->host_data;

	/* [한국어] 비트맵을 만지므로 뮤텍스를 잡는다. */
	mutex_lock(&pcie->lock);

	/* [한국어] 잡을 때와 같은 지수로 영역을 통째로 비운다. order_base_2 를 다시
	 * 계산하는 것은 alloc 과 대칭을 맞추기 위해서다. */
	bitmap_release_region(pcie->bitmap, d->hwirq, order_base_2(nr_irqs));

	/* [한국어] 뮤텍스 해제. */
	mutex_unlock(&pcie->lock);
}

/* [한국어] MSI 부모 도메인의 연산 표. translate 가 없는 것은 이 도메인이 DT 로
 * 직접 참조되는 대상이 아니라 MSI 코어가 프로그램적으로 쓰는 도메인이기 때문이다. */
static const struct irq_domain_ops apple_msi_domain_ops = {
	/* [한국어] 벡터 할당 콜백. */
	.alloc	= apple_msi_domain_alloc,
	/* [한국어] 벡터 해제 콜백. */
	.free	= apple_msi_domain_free,
};

/* [한국어]
 * apple_port_irq_mask - 포트 인터럽트 한 줄을 막는다
 *
 * @data: 대상 인터럽트의 irq_data. hwirq 가 곧 INTSTAT/INTMSK 의 비트 번호다.
 * @return: 없음.
 *
 * 포트의 인터럽트 마스크 레지스터에서 해당 비트를 세우면 그 인터럽트가 막힌다.
 * 전용 SET 레지스터가 있는데도 읽고-고쳐-쓰기를 쓰기 때문에(위 PORT_INTMSKSET
 * 관찰 참조) 경쟁을 막을 락이 필요하고, 그것이 port->lock 이다.
 * 실행 컨텍스트: irq_chip 콜백이므로 인터럽트가 꺼진 상태에서 불릴 수 있다.
 * 그래서 잠들 수 없는 raw 스핀락을 인터럽트 저장 방식으로 잡는다.
 * guard() 를 쓰면 함수를 빠져나갈 때 자동으로 풀린다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   (IRQ 코어의 mask 경로) → [apple_port_irq_mask] → rmw_set()
 */
static void apple_port_irq_mask(struct irq_data *data)
{
	/* [한국어] 도메인 등록 시 chip_data 로 심어 둔 포트 객체를 되찾는다. */
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	/* [한국어] 읽고-고쳐-쓰기 구간을 스핀락으로 감싼다. irqsave 판이라 현재 CPU 의
	 * 인터럽트도 함께 막아, 같은 CPU 에서 인터럽트 핸들러가 끼어들어 같은 레지스터를
	 * 만지는 재진입을 방지한다. guard 이므로 별도의 해제 문장이 없다. */
	guard(raw_spinlock_irqsave)(&port->lock);
	/* [한국어] 마스크 레지스터의 hwirq 번째 비트를 세워 그 인터럽트를 막는다. */
	rmw_set(BIT(data->hwirq), port->base + PORT_INTMSK);
}

/* [한국어]
 * apple_port_irq_unmask - 포트 인터럽트 한 줄을 다시 허용한다
 *
 * @data: 대상 인터럽트의 irq_data.
 * @return: 없음.
 *
 * mask 의 짝이며 락과 실행 컨텍스트 사정이 완전히 같다. 마스크 비트를 지우는
 * 것이 곧 허용이다.
 *
 * 호출 체인:
 *   (IRQ 코어의 unmask 경로) → [apple_port_irq_unmask] → rmw_clear()
 */
static void apple_port_irq_unmask(struct irq_data *data)
{
	/* [한국어] chip_data 에서 포트 객체를 되찾는다. */
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	/* [한국어] mask 와 같은 이유로 같은 락을 잡는다. */
	guard(raw_spinlock_irqsave)(&port->lock);
	/* [한국어] 마스크 비트를 지워 인터럽트를 허용한다. */
	rmw_clear(BIT(data->hwirq), port->base + PORT_INTMSK);
}

/* [한국어]
 * hwirq_is_intx - 이 hwirq 가 레거시 INTx 핀 인터럽트인지 판정한다
 *
 * @hwirq: 포트 도메인의 하드웨어 인터럽트 번호(= INTSTAT 비트 번호).
 * @return: INTx(하위 4비트)면 참, 아니면 거짓.
 *
 * 이 판정이 필요한 이유는 트리거 방식이 갈리기 때문이다. PCI 규격상 INTx 는
 * 레벨 트리거(장치가 신호를 내리기 전까지 계속 걸려 있다)이고, 이 컨트롤러가
 * 자체적으로 올리는 나머지 이벤트(링크 업/다운, 오류 등)는 에지로 보인다.
 * 레벨 인터럽트는 상태 비트를 드라이버가 지워서는 안 되고 장치가 원인을 없애야
 * 하므로, ACK 처리도 이 판정에 따라 갈린다.
 * 실행 컨텍스트: 어디서든 불릴 수 있는 순수 계산 함수. 잠들지 않는다.
 *
 * 호출 체인:
 *   apple_port_irq_ack() / apple_port_irq_set_type() / apple_port_irq_domain_alloc()
 *     → [hwirq_is_intx]
 */
static bool hwirq_is_intx(unsigned int hwirq)
{
	/* [한국어] 비트 번호를 마스크로 바꿔 INTx 영역(하위 4비트)과 겹치는지 본다.
	 * hwirq 가 0~3 이면 참이 된다. */
	return BIT(hwirq) & PORT_INT_INTx_MASK;
}

/* [한국어]
 * apple_port_irq_ack - 처리한 인터럽트의 상태 비트를 지운다
 *
 * @data: 대상 인터럽트의 irq_data.
 * @return: 없음.
 *
 * 에지 트리거 인터럽트는 '일어났다' 는 사실이 상태 레지스터에 래치되므로,
 * 처리한 뒤 그 비트를 지워야 다음 이벤트를 받을 수 있다. 반대로 INTx 는 레벨이라
 * 상태 비트가 장치의 현재 신호를 그대로 비추므로, 드라이버가 지워도 의미가 없고
 * 오히려 원인이 남아 있는데 지운 것처럼 보일 수 있다. 그래서 INTx 는 건너뛴다.
 * 상태 레지스터에 '1 을 써서 그 비트를 지우는' W1C 방식이므로, 다른 비트를
 * 건드리지 않기 위해 읽고-고쳐-쓰기 없이 해당 비트만 쓴다 — 그래서 여기에는
 * 락이 필요 없다.
 * 실행 컨텍스트: IRQ 처리 흐름(handle_edge_irq) 안에서 불린다. 잠들지 않는다.
 *
 * 호출 체인:
 *   handle_edge_irq() → [apple_port_irq_ack] → writel_relaxed()
 */
static void apple_port_irq_ack(struct irq_data *data)
{
	/* [한국어] chip_data 에서 포트 객체를 되찾는다. */
	struct apple_pcie_port *port = irq_data_get_irq_chip_data(data);

	/* [한국어] INTx 가 아닐 때만 — 즉 에지로 다루는 이벤트일 때만 — 상태를 지운다. */
	if (!hwirq_is_intx(data->hwirq))
		/* [한국어] 해당 비트에 1 을 써서 래치된 상태를 지운다(W1C). */
		writel_relaxed(BIT(data->hwirq), port->base + PORT_INTSTAT);
}

/* [한국어]
 * apple_port_irq_set_type - 요청된 트리거 방식이 하드웨어와 맞는지 검사만 한다
 *
 * @data: 대상 인터럽트의 irq_data.
 * @type: 요청된 트리거 타입(IRQ_TYPE_LEVEL_ 계열 또는 IRQ_TYPE_EDGE_ 계열).
 * @return: 0 이면 받아들임, -EINVAL 이면 하드웨어와 맞지 않는 요청.
 *
 * 이 하드웨어에는 트리거 방식을 바꾸는 레지스터가 없다(옆의 상류 주석이 그렇게
 * 적어 두었다). 그래서 이 콜백은 설정이 아니라 '검증' 만 한다 — INTx 는 레벨,
 * 나머지는 에지라는 고정된 사실과 요청이 어긋나면 거절한다.
 * 실행 컨텍스트: irq_set_irq_type() 경로에서 불린다. 잠들지 않는다.
 *
 * 호출 체인:
 *   irq_set_irq_type() → [apple_port_irq_set_type] → irqd_set_trigger_type()
 */
static int apple_port_irq_set_type(struct irq_data *data, unsigned int type)
{
	/*
	 * It doesn't seem that there is any way to configure the
	 * trigger, so assume INTx have to be level (as per the spec),
	 * and the rest is edge (which looks likely).
	 */
	/* [한국어] 옆의 상류 주석대로 INTx 는 레벨, 나머지는 에지여야 한다. 배타적 논리합
	 * 으로 '둘이 어긋났는가' 를 한 번에 판정한다 — INTx 인데 레벨 요청이 아니거나,
	 * INTx 가 아닌데 레벨 요청이면 참이 되어 거절한다. !! 는 마스크 결과를 0/1 로
	 * 정규화해 XOR 이 성립하게 만든다. */
	if (hwirq_is_intx(data->hwirq) ^ !!(type & IRQ_TYPE_LEVEL_MASK))
		/* [한국어] 하드웨어가 줄 수 없는 트리거 방식이므로 거절한다. */
		return -EINVAL;

	/* [한국어] 요청이 실제와 맞으므로, IRQ 코어의 기록만 갱신한다. 하드웨어에
	 * 쓸 것이 없다. */
	irqd_set_trigger_type(data, type);
	/* [한국어] 성공. */
	return 0;
}

/* [한국어] 포트 인터럽트용 irq_chip. MSI 쪽 칩과 달리 이쪽은 실제 하드웨어
 * 레지스터를 직접 만진다 — 마스크/언마스크/ACK 가 모두 포트 레지스터 조작이다. */
static struct irq_chip apple_port_irqchip = {
	/* [한국어] /proc/interrupts 등에 보일 이름. */
	.name		= "PCIe",
	/* [한국어] 에지 인터럽트의 상태 비트를 지우는 콜백. */
	.irq_ack	= apple_port_irq_ack,
	/* [한국어] 마스크 콜백. */
	.irq_mask	= apple_port_irq_mask,
	/* [한국어] 언마스크 콜백. */
	.irq_unmask	= apple_port_irq_unmask,
	/* [한국어] 트리거 방식 검증 콜백(설정은 하지 않는다). */
	.irq_set_type	= apple_port_irq_set_type,
};

/* [한국어]
 * apple_port_irq_domain_alloc - 포트 도메인의 IRQ 를 잡고 흐름 핸들러까지 정한다
 *
 * @domain: 이 포트의 32칸 선형 도메인.
 * @virq: 코어가 배정한 리눅스 IRQ 번호의 시작값.
 * @nr_irqs: 잡을 개수.
 * @args: irq_fwspec 포인터. param[0] 이 시작 hwirq 번호다.
 * @return: 항상 0 — 이 함수에는 실패 경로가 없다.
 *
 * 계층 도메인이 아니라 평평한(leaf) 도메인이므로 부모에게 넘길 것이 없고,
 * hwirq 를 칩·데이터·흐름 핸들러에 이어 붙이는 일만 한다. 여기서 중요한 것은
 * hwirq 번호에 따라 흐름 핸들러가 갈린다는 점이다 — INTx 는 레벨 처리
 * (handle_level_irq: 처리 전에 막고 처리 후 푼다), 나머지는 에지 처리
 * (handle_edge_irq: 먼저 ACK 하고 처리한다). 이 선택이 위 ack/set_type 의
 * 동작과 짝을 이룬다.
 * 실행 컨텍스트: DT 로 인터럽트를 요청하거나 이 파일이 직접
 * irq_domain_alloc_irqs() 를 부를 때. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   irq_domain_alloc_irqs() → [apple_port_irq_domain_alloc]
 *     → hwirq_is_intx(), irq_domain_set_info(), irq_set_irq_type()
 */
static int apple_port_irq_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	/* [한국어] 도메인 생성 시 host_data 로 심어 둔 포트 객체. 아래에서 각 IRQ 의
	 * chip_data 로 다시 심는다. */
	struct apple_pcie_port *port = domain->host_data;
	/* [한국어] 호출자가 준 인터럽트 지정자. 이 도메인은 셀 한 개(hwirq 번호)만 쓴다
	 * — 연산 표의 translate 가 irq_domain_translate_onecell 인 것과 일치한다. */
	struct irq_fwspec *fwspec = args;
	/* [한국어] 반복 인덱스. */
	int i;

	/* [한국어] 요청된 개수만큼 연속된 hwirq 를 하나씩 설정한다. */
	for (i = 0; i < nr_irqs; i++) {
		/* [한국어] 기본값은 에지 처리 — 이 컨트롤러가 스스로 올리는 이벤트들이 그렇다. */
		irq_flow_handler_t flow = handle_edge_irq;
		/* [한국어] 그에 맞는 기본 트리거 타입. */
		unsigned int type = IRQ_TYPE_EDGE_RISING;

		/* [한국어] 다만 하위 4비트(INTx)는 PCI 규격상 레벨이므로 갈아탄다. */
		if (hwirq_is_intx(fwspec->param[0] + i)) {
			/* [한국어] 레벨용 흐름 핸들러 — 처리 전에 마스크하고 끝나면 언마스크한다. */
			flow = handle_level_irq;
			/* [한국어] 레벨 하이 트리거로 기록한다. */
			type = IRQ_TYPE_LEVEL_HIGH;
		}

		/* [한국어] IRQ 하나를 도메인에 등록한다: hwirq 번호, 칩, 칩 데이터(포트 객체),
		 * 흐름 핸들러를 한 번에 설정한다. 뒤의 두 NULL 은 이 도메인이 쓰지 않는
		 * per-IRQ 데이터와 핸들러 이름 자리다. */
		irq_domain_set_info(domain, virq + i, fwspec->param[0] + i,
				    &apple_port_irqchip, port, flow,
				    NULL, NULL);

		/* [한국어] 등록한 IRQ 의 트리거 타입을 기록한다. 위 set_type 콜백이 검증만
		 * 하므로, 여기서 주는 값은 반드시 하드웨어의 실제 방식과 같아야 한다. */
		irq_set_irq_type(virq + i, type);
	}

	/* [한국어] 실패할 일이 없으므로 항상 성공. */
	return 0;
}

/* [한국어]
 * apple_port_irq_domain_free - 포트 도메인의 IRQ 등록을 되돌린다
 *
 * @domain: 이 포트의 도메인.
 * @virq: 해제할 IRQ 번호의 시작값.
 * @nr_irqs: 해제할 개수.
 * @return: 없음.
 *
 * alloc 이 심어 둔 흐름 핸들러와 irq_data 를 지운다. 하드웨어를 만지지 않는
 * 이유는 이 도메인이 잡는 자원이 소프트웨어 등록뿐이기 때문이다 — 마스크 상태는
 * 코어가 해제 전에 mask 콜백으로 정리한다.
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * [상류 코드 관찰] 이 파일에는 이 콜백에 이르는 경로가 없다. 포트가 한번 세워지면
 * 떼어 내지 않고(드라이버가 suppress_bind_attrs 로 언바인드를 막는다), 링크
 * 업/다운 IRQ 도 반납하지 않는다.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() → [apple_port_irq_domain_free]
 *     → irq_set_handler(), irq_domain_reset_irq_data()
 */
static void apple_port_irq_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	/* [한국어] 반복 인덱스. */
	int i;

	/* [한국어] 요청된 IRQ 를 하나씩 되돌린다. */
	for (i = 0; i < nr_irqs; i++) {
		/* [한국어] 이 IRQ 의 도메인 데이터를 얻는다. */
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

		/* [한국어] 흐름 핸들러를 떼어 낸다. NULL 을 주면 코어가 기본 처리로 되돌린다. */
		irq_set_handler(virq + i, NULL);
		/* [한국어] hwirq/칩/칩데이터 연결을 지운다. */
		irq_domain_reset_irq_data(d);
	}
}

/* [한국어] 포트 인터럽트 도메인의 연산 표. */
static const struct irq_domain_ops apple_port_irq_domain_ops = {
	/* [한국어] DT 의 인터럽트 지정자를 hwirq 로 옮기는 표준 구현 — 셀이 한 개인
	 * 도메인용이다. 즉 이 포트의 인터럽트는 DT 에서 번호 하나로만 지정된다. */
	.translate	= irq_domain_translate_onecell,
	/* [한국어] IRQ 할당 콜백. */
	.alloc		= apple_port_irq_domain_alloc,
	/* [한국어] IRQ 해제 콜백. */
	.free		= apple_port_irq_domain_free,
};

/* [한국어]
 * apple_port_irq_handler - 상위 IRQ 하나에 몰린 포트 인터럽트를 하위로 분배한다
 *
 * @desc: 상위(체인된) IRQ 의 서술자. 핸들러 데이터로 포트 객체가 걸려 있다.
 * @return: 없음.
 *
 * 이 포트의 32가지 이벤트는 상위 인터럽트 컨트롤러에서 보면 IRQ 한 줄이다.
 * 그래서 그 한 줄에 이 함수를 '체인 핸들러' 로 걸어 두고, 여기서 상태 레지스터를
 * 읽어 실제로 올라온 비트들을 각각의 하위 IRQ 로 나눠 준다. 이것이 이 하드웨어의
 * 포트 인터럽트 구조가 IRQ 도메인으로 표현되는 이유다.
 * chained_irq_enter()/exit() 로 감싸는 것은 상위 컨트롤러의 마스크/EOI 규약을
 * 지키기 위해서다 — 분배하는 동안 상위 인터럽트가 다시 올라오지 않게 해 준다.
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 잠들 수 없다. 그래서 아래에서 잡히는
 * 락도 전부 raw 스핀락이어야 한다.
 * 에러 경로: 없다 — 처리할 비트가 없으면 아무것도 하지 않고 빠져나간다.
 *
 * 호출 체인:
 *   (상위 인터럽트 컨트롤러의 IRQ 처리) → [apple_port_irq_handler]
 *     → chained_irq_enter(), readl_relaxed(), generic_handle_domain_irq(),
 *       chained_irq_exit()
 */
static void apple_port_irq_handler(struct irq_desc *desc)
{
	/* [한국어] 체인 핸들러 등록 시 넘긴 포트 객체를 되찾는다. */
	struct apple_pcie_port *port = irq_desc_get_handler_data(desc);
	/* [한국어] 상위 IRQ 를 관리하는 irq_chip. 아래 enter/exit 가 이 칩의
	 * 마스크·EOI 콜백을 부르는 데 쓴다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 상태 레지스터 값. for_each_set_bit 이 unsigned long 비트맵을
	 * 요구하므로 u32 가 아니라 이 타입이다. */
	unsigned long stat;
	/* [한국어] 비트 순회 인덱스 = hwirq 번호. */
	int i;

	/* [한국어] 상위 컨트롤러 규약에 따라 진입 처리(필요하면 상위 IRQ 마스크/ACK). */
	chained_irq_enter(chip, desc);

	/* [한국어] 포트의 인터럽트 상태를 한 번에 읽는다. 여기서 1 인 비트가 곧 처리해야
	 * 할 이벤트다. 마스크된 비트는 하드웨어가 올리지 않으므로 별도 필터가 없다. */
	stat = readl_relaxed(port->base + PORT_INTSTAT);

	/* [한국어] 32비트를 훑으며 세워진 비트마다 하위 도메인의 해당 IRQ 를 실행한다.
	 * 상태 비트를 지우는 일은 각 IRQ 의 흐름 핸들러가 ack 콜백으로 처리한다. */
	for_each_set_bit(i, &stat, 32)
		/* [한국어] hwirq i 를 이 포트 도메인의 리눅스 IRQ 로 바꿔 처리한다.
		 * 등록되지 않은 hwirq 면 코어가 조용히 무시한다. */
		generic_handle_domain_irq(port->domain, i);

	/* [한국어] 상위 컨트롤러 규약에 따라 마무리(EOI, 마스크 해제). */
	chained_irq_exit(chip, desc);
}

/* [한국어]
 * apple_pcie_port_setup_irq - 포트의 IRQ 도메인을 만들고 MSI 수신 설정을 새긴다
 *
 * @port: 설정 중인 포트.
 * @return: 0 성공, -ENXIO(DT 에서 상위 IRQ 를 얻지 못함), -ENOMEM(도메인 생성 실패).
 *
 * 포트 하나가 인터럽트를 다룰 수 있게 만드는 준비를 한 번에 한다. 네 단계다.
 * (1) 이 포트가 매달릴 상위 IRQ 를 DT 에서 얻는다. (2) 32칸 선형 도메인을 만들어
 * 상태 레지스터의 비트 하나하나를 IRQ 로 노출한다. (3) 남아 있던 인터럽트를 모두
 * 막고 지운 뒤 체인 핸들러를 건다 — 순서가 중요하다. 핸들러를 먼저 걸면 아직
 * 초기화되지 않은 상태에서 인터럽트가 들어올 수 있다. (4) MSI 도어벨 주소와
 * 벡터 배분 방식을 포트 레지스터에 새긴다.
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). 도메인 생성이 메모리를 할당하므로
 * 잠들 수 있다.
 * 에러 경로: 두 실패 모두 곧장 되돌아가고, 이미 만든 도메인을 되돌리는 코드는 없다
 * — 실패하면 probe 전체가 실패하고 devm 이 정리하는 구조다.
 *
 * 호출 체인:
 *   apple_pcie_setup_port() → [apple_pcie_port_setup_irq]
 *     → irq_of_parse_and_map(), irq_domain_create_linear(),
 *       irq_set_chained_handler_and_data(), writel_relaxed()
 */
static int apple_pcie_port_setup_irq(struct apple_pcie_port *port)
{
	/* [한국어] 도메인의 소유자로 등록할 펌웨어 노드. DT 노드에서 얻는다 — 이렇게
	 * 해야 DT 의 interrupt-parent 지정이 이 도메인을 가리킬 수 있다. */
	struct fwnode_handle *fwnode = &port->np->fwnode;
	/* [한국어] 세대 상수표(hw)에 닿기 위한 컨트롤러 객체. */
	struct apple_pcie *pcie = port->pcie;
	/* [한국어] DT 에서 받아 올 상위 IRQ 번호. */
	unsigned int irq;
	/* [한국어] MSICFG 에 쓸 값. MSIMAP 방식이면 0 인 채로 남고, MSIBASE 방식이면
	 * 아래에서 log2(벡터 수)가 채워진다. 0 초기화가 그 두 갈래를 하나로 합쳐 준다. */
	u32 val = 0;

	/* FIXME: consider moving each interrupt under each port */
	/* [한국어] 옆의 상류 FIXME 대로, 포트 인터럽트가 각 포트 노드가 아니라 컨트롤러
	 * 노드의 interrupts 목록에 idx 번째로 들어 있다. 그래서 포트가 아니라 컨트롤러의
	 * DT 노드에서 포트 인덱스로 꺼낸다. 실패하면 0 을 돌려준다. */
	irq = irq_of_parse_and_map(to_of_node(dev_fwnode(port->pcie->dev)),
				   port->idx);
	/* [한국어] 0 은 유효한 IRQ 번호가 아니므로 매핑 실패로 본다. */
	if (!irq)
		return -ENXIO;

	/* [한국어] 32칸짜리 선형 도메인을 만든다. 32 인 이유는 인터럽트 상태 레지스터가
	 * 32비트이고 비트 번호를 그대로 hwirq 로 쓰기 때문이다. 선형 도메인은 hwirq 를
	 * 배열 첨자로 쓰므로 조회가 O(1) 이고, hwirq 공간이 작을 때 적합하다.
	 * host_data 로 포트 객체를 심어 두어 콜백들이 되찾을 수 있게 한다. */
	port->domain = irq_domain_create_linear(fwnode, 32,
						&apple_port_irq_domain_ops,
						port);
	/* [한국어] 도메인 생성 실패는 곧 메모리 부족이다. */
	if (!port->domain)
		return -ENOMEM;

	/* Disable all interrupts */
	/* [한국어] 옆의 상류 주석대로 먼저 모든 인터럽트를 막는다. 마스크 레지스터는
	 * 1 이 '막힘' 이므로 전 비트에 1 을 쓴다. */
	writel_relaxed(~0, port->base + PORT_INTMSK);
	/* [한국어] 그 다음 래치되어 있던 상태 비트를 전부 지운다(1 을 쓰면 지워지는 W1C).
	 * 부트로더가 남긴 잔여 이벤트로 첫 인터럽트가 잘못 뜨는 것을 막는다. */
	writel_relaxed(~0, port->base + PORT_INTSTAT);
	/* [한국어] 링크 명령/상태 레지스터에 쌓인 상태도 같은 방식으로 지운다. */
	writel_relaxed(~0, port->base + PORT_LINKCMDSTS);

	/* [한국어] 이제서야 상위 IRQ 에 체인 핸들러를 건다. 인터럽트를 모두 막고 지운
	 * 뒤에 거는 순서라 곧바로 가짜 인터럽트가 들어올 여지가 없다. 두 번째 인자가
	 * 핸들러, 세 번째가 핸들러 데이터(포트 객체)다. */
	irq_set_chained_handler_and_data(irq, apple_port_irq_handler, port);

	/* Configure MSI base address */
	/* [한국어] 옆의 상류 주석대로 MSI 도어벨 주소를 설정한다. 그 전에 컴파일 시점
	 * 단언으로 도어벨이 4GB 아래임을 강제한다 — 주소 레지스터가 32비트뿐이라
	 * 상위 비트가 있으면 조용히 잘려 나가기 때문이다. Kconfig 로 주소를 바꿀 수
	 * 있으므로 이 단언이 실질적인 방어선이다. */
	BUILD_BUG_ON(upper_32_bits(DOORBELL_ADDR));
	/* [한국어] 도어벨의 하위 32비트를 포트의 MSI 주소 레지스터에 쓴다. 오프셋은
	 * 세대마다 달라 상수표에서 가져온다. 이 값이 apple_msi_compose_msg() 가 장치에
	 * 알려 주는 주소와 같아야 인터럽트가 도착한다. */
	writel_relaxed(lower_32_bits(DOORBELL_ADDR),
		       port->base + pcie->hw->port_msiaddr);
	/* [한국어] 상위 32비트 레지스터가 있는 세대(T602x)에서만. */
	if (pcie->hw->port_msiaddr_hi)
		/* [한국어] 위 단언 덕분에 상위 32비트는 언제나 0 이다. 명시적으로 써 주는 것은
		 * 부트로더가 남긴 값이 있을 수 있기 때문이다. */
		writel_relaxed(0, port->base + pcie->hw->port_msiaddr_hi);

	/* Enable MSIs, shared between all ports */
	/* [한국어] 옆의 상류 주석대로 MSI 를 켠다. 방식이 두 갈래인데, MSIMAP 표가
	 * 있는 세대(T602x)에서는 벡터마다 목적지를 한 칸씩 적는다. */
	if (pcie->hw->port_msimap) {
		/* [한국어] 이 컨트롤러가 쓰는 벡터 전부에 대해 한 칸씩. 모든 포트가 같은 벡터
		 * 풀을 공유하므로(상류 주석의 "shared between all ports") 포트마다 같은 표를
		 * 똑같이 채운다. */
		for (int i = 0; i < pcie->nvecs; i++)
			/* [한국어] i 번 칸에 '목적지 = i' 를 적고 유효 비트를 세운다. 즉 벡터 i 를
			 * 상위 컨트롤러의 i 번째 자리로 그대로 보내는 항등 매핑이다. FIELD_PREP 이
			 * 값을 PORT_MSIMAP_TARGET 필드 자리로 옮겨 준다. 칸 주소는 표 시작 + 4*i. */
			writel_relaxed(FIELD_PREP(PORT_MSIMAP_TARGET, i) |
				       PORT_MSIMAP_ENABLE,
				       port->base + pcie->hw->port_msimap + 4 * i);
	/* [한국어] MSIMAP 이 없는 세대(T8103)는 '시작 번호 + 개수' 방식을 쓴다. */
	} else {
		/* [한국어] 벡터 구간의 시작 번호를 0 으로 둔다 — 모든 포트가 벡터 0 부터 시작하는
		 * 같은 풀을 본다는 뜻이다. */
		writel_relaxed(0, port->base + PORT_MSIBASE);
		/* [한국어] 벡터 개수를 log2 로 바꿔 MSICFG 의 해당 자리에 넣을 값을 만든다.
		 * 레지스터가 개수를 지수로 받으므로 nvecs 는 2의 거듭제곱이어야 하는데,
		 * 그 검증은 코드 어디에도 없다. */
		val = ilog2(pcie->nvecs) << PORT_MSICFG_L2MSINUM_SHIFT;
	}

	/* [한국어] 위에서 만든 값(MSIMAP 방식이면 0)에 활성화 비트를 얹어 MSICFG 에 쓴다.
	 * 이 쓰기로 포트의 MSI 수신이 켜진다. */
	writel_relaxed(val | PORT_MSICFG_EN, port->base + PORT_MSICFG);
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * apple_pcie_port_irq - 링크 업/다운 이벤트를 로그로 남기고 대기를 깨운다
 *
 * @irq: 발생한 리눅스 IRQ 번호.
 * @data: request_irq 에 넘겼던 포트 객체.
 * @return: IRQ_HANDLED(우리가 처리함) 또는 IRQ_NONE(우리 것이 아님).
 *
 * 포트 도메인의 32개 IRQ 중 이 드라이버가 실제로 잡아 쓰는 것은 링크 업과
 * 링크 다운 둘뿐이다. 링크 업은 특히 중요한데, apple_pcie_setup_port() 이
 * 이 인터럽트를 기다리며 완료 객체에서 잠들어 있기 때문이다 — 즉 이 핸들러가
 * probe 를 진행시키는 열쇠다.
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(request_irq 에 플래그 0 을 주었으므로
 * 스레드 핸들러가 아니다). 잠들 수 없다. complete_all() 은 내부 스핀락만 쓰므로
 * 이 문맥에서 안전하다.
 * 에러 경로: 알지 못하는 hwirq 면 IRQ_NONE 을 돌려주어 공유 IRQ 다음 핸들러에게
 * 넘긴다. 다만 이 드라이버는 이 두 hwirq 에만 이 핸들러를 걸므로 실제로는
 * 도달하지 않는다.
 *
 * 호출 체인:
 *   apple_port_irq_handler() → generic_handle_domain_irq() → (흐름 핸들러)
 *     → [apple_pcie_port_irq] → complete_all()
 */
static irqreturn_t apple_pcie_port_irq(int irq, void *data)
{
	/* [한국어] request_irq 에 넘겼던 포트 객체를 되찾는다. */
	struct apple_pcie_port *port = data;
	/* [한국어] 리눅스 IRQ 번호를 이 포트 도메인의 hwirq(= 상태 레지스터 비트 번호)로
	 * 되돌린다. 아래 switch 가 비트 번호로 이벤트를 구분하기 때문이다. */
	unsigned int hwirq = irq_domain_get_irq_data(port->domain, irq)->hwirq;

	/* [한국어] 어떤 이벤트인지 비트 번호로 가른다. */
	switch (hwirq) {
	/* [한국어] 비트 12 — 링크가 올라왔다. */
	case PORT_INT_LINK_UP:
		/* [한국어] 어느 포트인지 DT 노드 경로(%pOF)와 함께 남긴다. ratelimited 판인 것은
		 * 링크가 불안정해 업/다운을 반복할 때 로그가 폭주하는 것을 막기 위해서다. */
		dev_info_ratelimited(port->pcie->dev, "Link up on %pOF\n",
				     port->np);
		/* [한국어] 링크 업을 기다리며 잠들어 있는 apple_pcie_setup_port() 을 깨운다.
		 * complete_all 판을 쓰는 것은 대기자가 여럿일 수 있는 상황을 염두에 둔 것으로
		 * 보이지만, 현재 흐름에서 대기자는 하나다. */
		complete_all(&port->pcie->event);
		break;
	/* [한국어] 비트 14 — 링크가 내려갔다. */
	case PORT_INT_LINK_DOWN:
		/* [한국어] 로그만 남긴다. 재훈련을 시도하는 코드는 이 파일에 없다. */
		dev_info_ratelimited(port->pcie->dev, "Link down on %pOF\n",
				     port->np);
		break;
	/* [한국어] 그 밖의 비트는 이 핸들러가 등록되지 않은 것이다. */
	default:
		/* [한국어] 우리 인터럽트가 아니라고 알린다. */
		return IRQ_NONE;
	}

	/* [한국어] 위 두 경우는 우리가 처리했다고 알린다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * apple_pcie_port_register_irqs - 링크 업/다운 IRQ 를 잡아 핸들러를 건다
 *
 * @port: 대상 포트.
 * @return: 항상 0.
 *
 * 포트 도메인은 32칸이지만 드라이버가 실제로 쓰는 것은 두 칸이다. 그 둘을
 * 도메인에서 IRQ 로 잡아 apple_pcie_port_irq() 를 걸어 둔다. 이 등록이 끝나야
 * 링크 훈련을 시작할 수 있다 — 그래야 링크 업 인터럽트를 받을 수 있기 때문이다.
 * 실행 컨텍스트: probe 경로. irq_domain_alloc_irqs() 와 request_irq() 가 모두
 * 메모리를 할당하므로 잠들 수 있다.
 *
 * [상류 코드 관찰] 세 가지가 눈에 띈다.
 *  1. 실패를 전혀 전파하지 않는다. 할당 실패는 continue, request_irq 실패는
 *     WARN_ON 만 하고 넘어가며, 함수는 늘 0 을 돌려준다. 그래서 호출자가
 *     WARN_ON(ret) 로 감싸는 것은 실질적으로 죽은 검사다.
 *  2. irq_domain_alloc_irqs() 의 반환값을 unsigned int 에 담고 0 인지만 본다.
 *     음수로 오류를 돌려주는 API 라면 그 값이 아주 큰 양수가 되어 검사를
 *     통과한다. 이 API 의 반환 규약은 include/ 가 이 트리에 없어 확인 못 함.
 *  3. 잡은 IRQ 를 반납하는 짝(free_irq)이 이 파일에 없다.
 *
 * 호출 체인:
 *   apple_pcie_setup_port() → [apple_pcie_port_register_irqs]
 *     → irq_domain_alloc_irqs(), request_irq()
 */
static int apple_pcie_port_register_irqs(struct apple_pcie_port *port)
{
	/* [한국어] 잡을 인터럽트의 (비트 번호, 이름) 짝을 담은 지역 표. static 인 것은
	 * 호출마다 스택에 다시 만들 필요가 없기 때문이다. */
	static struct {
		/* [한국어] 인터럽트 상태 레지스터의 비트 번호 = 포트 도메인의 hwirq.
		 *   설정자: 아래 초기화 목록뿐.
		 *   읽는 자: fwspec 의 param[0] 로 들어가 도메인 할당의 hwirq 가 된다.
		 *   값 범위: 0~31. 여기서는 12(링크 업)와 14(링크 다운).
		 *   동기화: 읽기 전용 상수라 필요 없다. */
		unsigned int	hwirq;
		/* [한국어] /proc/interrupts 에 표시될 이름 문자열.
		 *   설정자: 아래 초기화 목록뿐.
		 *   읽는 자: request_irq() 의 이름 인자.
		 *   값 범위: 정적 문자열 리터럴이라 수명이 영구적이다 — request_irq 가 이
		 *     포인터를 그대로 보관하므로 지역 버퍼를 주면 안 되는 자리다.
		 *   동기화: 읽기 전용. */
		const char	*name;
	} port_irqs[] = {
		/* [한국어] 링크 업 이벤트. 이것이 probe 의 대기를 푸는 인터럽트다. */
		{ PORT_INT_LINK_UP,	"Link up",	},
		/* [한국어] 링크 다운 이벤트. 로그만 남긴다. */
		{ PORT_INT_LINK_DOWN,	"Link down",	},
	};
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] 표의 두 항목을 차례로 등록한다. */
	for (i = 0; i < ARRAY_SIZE(port_irqs); i++) {
		/* [한국어] 도메인에 넘길 인터럽트 지정자를 그때그때 만든다. 이 도메인의
		 * translate 가 셀 한 개짜리이므로 param_count 도 1 이다. */
		struct irq_fwspec fwspec = {
			/* [한국어] 어느 도메인의 지정자인지 알려 주는 펌웨어 노드 — 포트의 DT 노드다. */
			.fwnode		= &port->np->fwnode,
			/* [한국어] 셀이 하나뿐임을 알린다. */
			.param_count	= 1,
			/* [한국어] 셀 배열의 시작. */
			.param		= {
				/* [한국어] 유일한 셀에 hwirq(비트 번호)를 넣는다. */
				[0]	= port_irqs[i].hwirq,
			},
		};
		/* [한국어] 할당받을 리눅스 IRQ 번호. */
		unsigned int irq;
		/* [한국어] request_irq 결과. */
		int ret;

		/* [한국어] 이 지정자에 대응하는 IRQ 하나를 도메인에서 잡는다. NUMA_NO_NODE 는
		 * 특정 NUMA 노드에 자료구조를 붙이지 않겠다는 뜻이다. */
		irq = irq_domain_alloc_irqs(port->domain, 1, NUMA_NO_NODE,
					    &fwspec);
		/* [한국어] 실패하면 경고만 남기고 다음 항목으로 넘어간다(위 관찰 1, 2 참조). */
		if (WARN_ON(!irq))
			continue;

		/* [한국어] 잡은 IRQ 에 핸들러를 건다. 플래그 0 은 공유하지 않는 1차 핸들러라는
		 * 뜻이며, 그래서 apple_pcie_port_irq() 는 하드 인터럽트 컨텍스트에서 돈다.
		 * 마지막 인자(포트 객체)가 핸들러의 data 로 전달된다. */
		ret = request_irq(irq, apple_pcie_port_irq, 0,
				  port_irqs[i].name, port);
		/* [한국어] 실패해도 경고만 남기고 계속 간다 — 반환값은 여전히 0 이다. */
		WARN_ON(ret);
	}

	/* [한국어] 언제나 성공을 알린다(위 관찰 1 참조). */
	return 0;
}

/* [한국어]
 * apple_pcie_setup_refclk - PHY 와 refclk 요청/응답 핸드셰이크를 마친다
 *
 * @pcie: 세대 상수표를 가진 컨트롤러 객체.
 * @port: 대상 포트.
 * @return: 0 성공, readl_relaxed_poll_timeout 의 타임아웃 오류(-ETIMEDOUT) 전달.
 *
 * PCIe 링크는 송수신 양쪽이 같은 기준 클럭(reference clock)을 봐야 성립한다.
 * 이 하드웨어에서는 그 클럭을 드라이버가 '요청' 하고 하드웨어가 '응답' 하는
 * 핸드셰이크로 켠다. 순서가 정해져 있다 — refclk 0 을 요청해 응답을 받고, 그
 * 다음 refclk 1 을 요청해 응답을 받은 뒤에야 실제 출력을 켠다. 그래서 두 번의
 * 폴링이 직렬로 놓여 있다.
 * 이 함수가 PERST# 를 건 상태에서 불리는 것이 중요하다 — 호출자가 클럭을 켜기
 * 전에 PERST# 를 어서트하고, 이 함수가 끝난 뒤 규격이 요구하는 최소 시간을
 * 기다렸다가 PERST# 를 푼다.
 * 실행 컨텍스트: probe 경로. readl_relaxed_poll_timeout 이 잠들 수 있는 판이라
 * 프로세스 컨텍스트여야 한다. 폴링 간격 100us, 최대 50ms.
 * 에러 경로: 어느 한쪽 응답이 시한 안에 오지 않으면 그 오류를 그대로 돌려주고,
 * 이미 세워 둔 요청 비트나 설정 접근 비트는 되돌리지 않는다 — 실패하면 포트
 * 설정 전체가 중단되므로 되돌릴 이유가 없는 구조다.
 *
 * 호출 체인:
 *   apple_pcie_setup_port() → [apple_pcie_setup_refclk]
 *     → rmw_set(), readl_relaxed_poll_timeout(), rmw_clear()
 */
static int apple_pcie_setup_refclk(struct apple_pcie *pcie,
				   struct apple_pcie_port *port)
{
	/* [한국어] 폴링 매크로가 읽은 값을 담을 변수. 조건식에서 이 변수를 검사한다. */
	u32 stat;
	/* [한국어] 폴링 결과(0 또는 타임아웃 오류). */
	int res;

	/* [한국어] 레인 제어 레지스터가 있는 세대(T8103)에서만 설정 접근 창을 연다.
	 * T602x 는 이 값이 0 이라 통째로 건너뛴다. */
	if (pcie->hw->phy_lane_ctl)
		/* [한국어] 설정 접근 비트를 세워 PHY 설정 레지스터에 쓸 수 있게 만든다. */
		rmw_set(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);

	/* [한국어] refclk 0 을 달라고 요청한다. */
	rmw_set(PHY_LANE_CFG_REFCLK0REQ, port->phy + PHY_LANE_CFG);

	/* [한국어] 그 요청의 응답 비트가 설 때까지 100us 간격으로 최대 50ms 기다린다.
	 * 매크로가 stat 에 읽은 값을 담고 조건이 참이 되면 0 을, 시한을 넘기면 음수를
	 * 돌려준다. 이 매크로는 내부에서 잠들 수 있으므로 인터럽트 문맥에서 쓸 수 없다. */
	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK0ACK,
					 100, 50000);
	/* [한국어] 응답이 오지 않았다면 그대로 실패를 전달한다. */
	if (res < 0)
		return res;

	/* [한국어] 0번이 준비된 뒤에야 refclk 1 을 요청한다. 순서가 하드웨어 규약이다. */
	rmw_set(PHY_LANE_CFG_REFCLK1REQ, port->phy + PHY_LANE_CFG);
	/* [한국어] 1번 응답도 같은 조건으로 기다린다. */
	res = readl_relaxed_poll_timeout(port->phy + PHY_LANE_CFG,
					 stat, stat & PHY_LANE_CFG_REFCLK1ACK,
					 100, 50000);

	/* [한국어] 1번 응답 실패도 그대로 전달한다. */
	if (res < 0)
		return res;

	/* [한국어] 설정 접근이 끝났으므로 창을 닫는다. 연 세대에서만 닫는다. */
	if (pcie->hw->phy_lane_ctl)
		/* [한국어] 설정 접근 비트를 지운다. */
		rmw_clear(PHY_LANE_CTL_CFGACC, port->phy + pcie->hw->phy_lane_ctl);

	/* [한국어] 두 refclk 의 준비가 확인되었으니 실제 출력을 켠다. 비트 두 개를
	 * 한 번에 세운다. */
	rmw_set(PHY_LANE_CFG_REFCLKEN, port->phy + PHY_LANE_CFG);

	/* [한국어] 포트 쪽 refclk 제어 레지스터가 있는 세대에서는 그쪽도 켜 준다.
	 * T602x 는 이 레지스터가 없어 PHY 쪽 설정만으로 끝난다. */
	if (pcie->hw->port_refclk)
		/* [한국어] 포트 refclk 인에이블 비트를 세운다. */
		rmw_set(PORT_REFCLK_EN, port->base + pcie->hw->port_refclk);

	/* [한국어] 핸드셰이크 성공. */
	return 0;
}

/* [한국어]
 * port_rid2sid_addr - RID2SID 매핑 표에서 idx 번 칸의 주소를 계산한다
 *
 * @port: 표를 가진 포트.
 * @idx: 칸 번호.
 * @return: 그 칸의 MMIO 가상 주소.
 *
 * 표의 시작 오프셋이 세대마다 다르므로(T8103 은 0x828, T602x 는 0x3000) 그
 * 차이를 이 한 줄에 가둬 두었다. 칸 하나가 32비트이므로 인덱스에 4 를 곱한다.
 * 실행 컨텍스트: 잠들지 않는 순수 주소 계산. 호출자는 모두 pcie->lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   apple_pcie_rid2sid_write() / apple_pcie_disable_device()
 *     → [port_rid2sid_addr]
 */
static void __iomem *port_rid2sid_addr(struct apple_pcie_port *port, int idx)
{
	/* [한국어] 포트 창 기준 표 시작 오프셋에 칸 크기(4바이트) × 인덱스를 더한다. */
	return port->base + port->pcie->hw->port_rid2sid + 4 * idx;
}

/* [한국어]
 * apple_pcie_rid2sid_write - RID2SID 표의 한 칸을 쓰고 되읽어 확인한다
 *
 * @port: 대상 포트.
 * @idx: 칸 번호.
 * @val: 쓸 값(유효 비트 + SID + RID, 또는 지우기 위한 0).
 * @return: 쓴 직후 같은 칸에서 되읽은 값.
 *
 * 되읽기가 이 함수의 존재 이유다. 두 가지 목적이 겹쳐 있다. 첫째, 옆의 상류
 * 주석대로 쓰기가 하드웨어에 실제로 도달했음을 보장한다 — relaxed 쓰기는
 * 버퍼에 머물 수 있는데, 같은 주소를 읽으면 그보다 먼저 밀려 나가야 하기 때문이다.
 * 둘째, 반환값을 호출자가 비교할 수 있게 해 준다. apple_pcie_setup_port() 은
 * 이것을 이용해 표의 실제 크기를 알아낸다 — 없는 칸은 RAZ/WI(읽으면 0, 쓰면
 * 무시)라서 쓴 값이 되읽히지 않는다.
 * 실행 컨텍스트: probe 의 탐지 루프와, pcie->lock 을 쥔 장치 등록/해제 경로.
 * 잠들지 않는다.
 *
 * 호출 체인:
 *   apple_pcie_setup_port() / apple_pcie_enable_device() / apple_pcie_disable_device()
 *     → [apple_pcie_rid2sid_write] → port_rid2sid_addr(), writel_relaxed(), readl_relaxed()
 */
static u32 apple_pcie_rid2sid_write(struct apple_pcie_port *port,
				    int idx, u32 val)
{
	/* [한국어] 계산한 칸 주소에 값을 쓴다. */
	writel_relaxed(val, port_rid2sid_addr(port, idx));
	/* Read back to ensure completion of the write */
	/* [한국어] 옆의 상류 주석대로 같은 칸을 되읽어 쓰기 완료를 보장하고, 그 값을
	 * 그대로 반환해 호출자가 RAZ/WI 여부를 판정할 수 있게 한다. */
	return readl_relaxed(port_rid2sid_addr(port, idx));
}

/* [한국어]
 * apple_pcie_setup_port - 루트 포트 하나를 전원부터 링크 업까지 살려 낸다
 *
 * @pcie: 컨트롤러 객체.
 * @np: 이 포트를 서술한 DT 자식 노드.
 * @return: 0 성공, 음수 실패(GPIO/메모리/DT/타임아웃 등 원인별 오류 코드).
 *
 * 이 파일에서 가장 긴 함수이며, 하드웨어를 실제로 깨우는 순서가 전부 여기 있다.
 * 순서 자체가 규격과 하드웨어 요구에서 나온 것이라 바꿀 수 없다:
 *  1) PERST# GPIO 를 얻고 포트 객체와 RID2SID 비트맵을 잡는다.
 *  2) DT reg 첫 셀에서 포트 번호를 뽑고, 그 번호로 포트 창과 PHY 창을 매핑한다.
 *  3) 앱클럭을 켠다(포트 레지스터에 접근하려면 이것이 먼저다).
 *  4) PERST# 를 어서트한 상태에서 refclk 핸드셰이크를 마친다. 규격은 클럭이
 *     안정된 뒤 최소 100us(Tperst-clk) 지나서 PERST# 를 풀라고 요구한다.
 *  5) PERST# 를 풀고 100ms 를 기다린다(규격의 링크 훈련 대기).
 *  6) 포트 READY 를 최대 250ms 폴링한다.
 *  7) 클럭 게이팅을 다시 허용해 절전 상태로 되돌린다.
 *  8) IRQ 도메인과 MSI 설정을 세우고, RID2SID 표의 실제 크기를 탐지한다.
 *  9) 링크 업/다운 IRQ 를 등록한 뒤에야 LTSSM 을 출발시킨다 — 순서가 반대면
 *     링크 업 인터럽트를 놓칠 수 있다.
 * 10) 링크 업을 최대 100ms 기다린다. 실패해도 경고만 남기고 성공을 반환한다
 *     — 슬롯에 장치가 없을 수도 있기 때문이다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). msleep(100), usleep_range(),
 * 두 번의 폴링, 완료 대기까지 있어 실제로 여러 번 잠든다. 그래서 이 경로는
 * 인터럽트 문맥에서 부를 수 없다.
 * 에러 경로: 대부분 곧장 되돌아가고 devm 이 자원을 정리한다. 다만 목록에 이미
 * 넣은 포트를 빼는 코드는 없는데, 목록에 넣는 시점(9단계 직전) 이후로는 실패로
 * 되돌아가는 경로가 없으므로 실제로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   apple_pcie_init() → [apple_pcie_setup_port]
 *     → apple_pcie_setup_refclk(), apple_pcie_port_setup_irq(),
 *       apple_pcie_rid2sid_write(), apple_pcie_port_register_irqs()
 */
static int apple_pcie_setup_port(struct apple_pcie *pcie,
				 struct device_node *np)
{
	/* [한국어] 자원 조회 API 가 platform_device 를 요구하므로 device 에서 되돌린다. */
	struct platform_device *platform = to_platform_device(pcie->dev);
	/* [한국어] 이번에 만들 포트 객체. */
	struct apple_pcie_port *port;
	/* [한국어] PERST# 를 담당하는 GPIO 서술자. 포트 객체에 보관하지 않는 것은 리셋을
	 * 이 함수 안에서만 토글하고 이후에는 쓰지 않기 때문이다. */
	struct gpio_desc *reset;
	/* [한국어] 포트 창과 PHY 창을 찾을 때 재사용하는 자원 포인터. */
	struct resource *res;
	/* [한국어] "portN" / "phyN" 이름을 만드는 임시 버퍼. 16바이트면 충분하다. */
	char name[16];
	/* [한국어] stat 은 READY 폴링이 읽은 값, idx 는 DT reg 첫 셀의 원값. */
	u32 stat, idx;
	/* [한국어] ret 은 각 단계의 결과, i 는 RID2SID 탐지 루프의 인덱스이자 그 결과
	 * (루프를 빠져나온 지점이 곧 표의 칸 수)다. */
	int ret, i;

	/* [한국어] 이 포트 노드에 딸린 "reset" GPIO 를 얻는다. 출력·초기값 낮음으로
	 * 요청하고 소비자 이름을 "PERST#" 로 붙여 디버그 출력에서 알아보기 쉽게 한다.
	 * devm_ 판이라 실패/제거 시 자동 해제된다. GPIO 컨트롤러 쪽 구현은 drivers/gpio
	 * 가 이 트리에 없어 확인 못 함. */
	reset = devm_fwnode_gpiod_get(pcie->dev, of_fwnode_handle(np), "reset",
				      GPIOD_OUT_LOW, "PERST#");
	/* [한국어] GPIO 를 못 얻으면(없거나 오류) 포트를 세울 수 없다. 값이 아니라
	 * ERR_PTR 로 오류를 싣는 API 라 IS_ERR/PTR_ERR 쌍으로 다룬다. */
	if (IS_ERR(reset))
		return PTR_ERR(reset);

	/* [한국어] 포트 객체를 0 으로 채워 할당한다. devm 이라 컨트롤러 디바이스와 수명이
	 * 같다 — 이 드라이버에 포트를 해제하는 경로가 없는 것과 맞물린다. */
	port = devm_kzalloc(pcie->dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!port)
		return -ENOMEM;

	/* [한국어] RID2SID 표의 사용 현황 비트맵을 세대별 최대 칸 수만큼 잡는다. 실제
	 * 칸 수는 아래 탐지 루프가 정하지만, 탐지 전이라 상한으로 잡아 둔다. */
	port->sid_map = devm_bitmap_zalloc(pcie->dev, pcie->hw->max_rid2sid, GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!port->sid_map)
		return -ENOMEM;

	/* [한국어] DT reg 속성의 첫 셀을 읽는다. 이 셀은 OF PCI 바인딩의 주소 상위
	 * 워드이고, 그 안에 버스/장치/함수 번호가 들어 있다. */
	ret = of_property_read_u32_index(np, "reg", 0, &idx);
	/* [한국어] reg 가 없거나 형식이 틀리면 포트를 식별할 수 없다. */
	if (ret)
		return ret;

	/* Use the first reg entry to work out the port index */
	/* [한국어] 옆의 상류 주석대로 그 셀에서 포트 번호를 뽑는다. 장치 번호가 비트
	 * 15..11 에 놓이므로 11비트를 민다. 루트 포트라 버스 번호가 0 이어서 그 위
	 * 비트가 남지 않는다. 이 해석은 apple_pcie_get_port() 이 이 값을
	 * PCI_SLOT(devfn) 과 직접 비교한다는 사실로 뒷받침된다. */
	port->idx = idx >> 11;
	/* [한국어] 컨트롤러로 거슬러 올라갈 역포인터를 심는다. */
	port->pcie = pcie;
	/* [한국어] DT 노드를 보관한다. 참조 카운트는 아래 성공 경로에서 올린다. */
	port->np = np;

	/* [한국어] 인터럽트 마스크 읽고-고쳐-쓰기를 지킬 스핀락을 초기화한다.
	 * 인터럽트 문맥에서 잡히므로 raw 판이다. */
	raw_spin_lock_init(&port->lock);

	/* [한국어] "portN" 이름을 만든다. DT 가 reg-names 로 창에 이름을 붙였다면
	 * 인덱스가 아니라 이름으로 찾는 편이 안전하기 때문이다. */
	snprintf(name, sizeof(name), "port%d", port->idx);
	/* [한국어] 이름으로 포트 레지스터 창을 찾는다. */
	res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
	/* [한국어] 이름이 없는 옛 DT 를 위한 대비책. */
	if (!res)
		/* [한국어] 인덱스로 찾는다. +2 인 근거는 reg 0번이 ECAM 설정공간, 1번이 컨트롤러
		 * 공용 창이기 때문이다 — probe 가 인덱스 1 을 매핑하는 것이 그 증거다.
		 * [상류 코드 관찰] 이 조회도 실패하면 res 가 NULL 인 채로 아래 매핑에 넘어간다.
		 * devm_ioremap_resource() 이 NULL 을 어떻게 다루는지는 그 구현(lib/devres.c)이
		 * 이 트리에 없어 확인 못 함. */
		res = platform_get_resource(platform, IORESOURCE_MEM, port->idx + 2);

	/* [한국어] 포트 레지스터 창을 매핑한다. 이 호출이 자원 요청(request_mem_region)과
	 * ioremap 을 함께 해 주므로 중복 매핑을 코어가 막아 준다. */
	port->base = devm_ioremap_resource(&platform->dev, res);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(port->base))
		return PTR_ERR(port->base);

	/* [한국어] 이번에는 "phyN" 이름을 만든다. 같은 버퍼를 재사용한다. */
	snprintf(name, sizeof(name), "phy%d", port->idx);
	/* [한국어] PHY 창을 이름으로 찾는다. 있을 수도 없을 수도 있다. */
	res = platform_get_resource_byname(platform, IORESOURCE_MEM, name);
	/* [한국어] DT 가 PHY 창을 따로 서술한 경우. */
	if (res)
		/* [한국어] 그 창을 별도로 매핑한다. */
		port->phy = devm_ioremap_resource(&platform->dev, res);
	/* [한국어] 서술하지 않은 경우 — 옛 DT 이거나 PHY 가 공용 창 안에 있는 하드웨어다. */
	else
		/* [한국어] 공용 창 안의 기본 위치를 가리키게 한다. 별도 ioremap 없이 이미 매핑된
		 * 영역 안쪽을 가리키는 포인터라, 이 경우에는 자원 충돌 검사가 걸리지 않는다. */
		port->phy = pcie->base + CORE_PHY_DEFAULT_BASE(port->idx);

	/* [한국어] 포트의 앱클럭을 켠다. 이 클럭이 없으면 아래의 포트 레지스터 접근이
	 * 모두 무의미하므로, 하드웨어를 만지는 첫 동작이 이것이다. */
	rmw_set(PORT_APPCLK_EN, port->base + PORT_APPCLK);

	/* Assert PERST# before setting up the clock */
	/* [한국어] 옆의 상류 주석대로, 클럭을 세우기 전에 PERST# 를 어서트해 엔드포인트를
	 * 리셋 상태로 잡아 둔다. cansleep 판인 것은 GPIO 컨트롤러가 느린 버스 뒤에 있을
	 * 수도 있기 때문이며, 여기가 프로세스 컨텍스트라 쓸 수 있다. 값 1 은 전기적
	 * 레벨이 아니라 논리값 — DT 의 active-low 서술은 GPIO 코어가 반영한다. */
	gpiod_set_value_cansleep(reset, 1);

	/* [한국어] PHY refclk 핸드셰이크. 이 안에서 최대 50ms 씩 두 번 폴링한다. */
	ret = apple_pcie_setup_refclk(pcie, port);
	/* [한국어] 클럭을 세우지 못하면 링크를 만들 수 없다. */
	if (ret < 0)
		return ret;

	/* The minimal Tperst-clk value is 100us (PCIe CEM r5.0, 2.9.2) */
	/* [한국어] 옆의 상류 주석이 인용한 PCIe CEM 규격의 Tperst-clk — 기준 클럭이
	 * 안정된 뒤 PERST# 를 풀기까지 최소 100us 를 두어야 한다. 범위를 주는 판이라
	 * 커널이 다른 타이머와 묶어 처리할 여지가 생긴다. */
	usleep_range(100, 200);

	/* Deassert PERST# */
	/* [한국어] 옆의 상류 주석대로 PERST# 를 푼다. 먼저 컨트롤러 레지스터 쪽 비트를
	 * 세우고(OFF = 리셋 끔), */
	rmw_set(PORT_PERST_OFF, port->base + pcie->hw->port_perst);
	/* [한국어] 이어서 GPIO 쪽도 0 으로 내린다. 두 경로가 모두 풀려야 엔드포인트가
	 * 리셋에서 벗어난다 — 보드에 따라 둘 중 하나만 실제로 연결되어 있을 수 있다. */
	gpiod_set_value_cansleep(reset, 0);

	/* Wait for 100ms after PERST# deassertion (PCIe r5.0, 6.6.1) */
	/* [한국어] 옆의 상류 주석이 인용한 PCIe 규격의 대기 — PERST# 해제 후 100ms 는
	 * 엔드포인트가 준비될 시간을 준다. 그 전에 설정공간을 읽으면 응답하지 않는다. */
	msleep(100);

	/* [한국어] 포트가 준비되었다는 비트를 100us 간격으로 최대 250ms 폴링한다.
	 * 위 100ms 를 이미 기다렸으므로 여기서 오래 걸리면 하드웨어 문제다. */
	ret = readl_relaxed_poll_timeout(port->base + PORT_STATUS, stat,
					 stat & PORT_STATUS_READY, 100, 250000);
	/* [한국어] 시한 안에 준비되지 않았다. */
	if (ret < 0) {
		/* [한국어] 어느 포트인지 DT 경로와 함께 알린다. %pOF 는 device_node 를 경로로
		 * 출력하는 커널 포맷 지정자다. */
		dev_err(pcie->dev, "port %pOF ready wait timeout\n", np);
		return ret;
	}

	/* [한국어] 준비가 끝났으니 절전을 위해 클럭 게이팅을 다시 허용한다. 방식이
	 * 세대마다 다르다 — 포트 refclk 레지스터가 있는 세대는 그쪽 CGDIS 비트를 지운다. */
	if (pcie->hw->port_refclk)
		/* [한국어] '게이팅 금지' 비트를 지워 게이팅을 허용한다. */
		rmw_clear(PORT_REFCLK_CGDIS, port->base + pcie->hw->port_refclk);
	/* [한국어] 그 레지스터가 없는 세대(T602x)는 PHY 쪽으로 대신한다. */
	else
		/* [한국어] PHY 레인 설정의 클럭 게이팅 허용 비트 둘을 세운다. 한쪽은 비트를
		 * 지워서, 다른 쪽은 세워서 같은 효과를 낸다는 점에 주의. */
		rmw_set(PHY_LANE_CFG_REFCLKCGEN, port->phy + PHY_LANE_CFG);

	/* [한국어] 앱클럭의 게이팅 금지도 푼다. 이제 포트가 유휴일 때 클럭이 멈출 수 있다. */
	rmw_clear(PORT_APPCLK_CGDIS, port->base + PORT_APPCLK);

	/* [한국어] IRQ 도메인 생성과 MSI 수신 설정. 링크를 출발시키기 전에 인터럽트를
	 * 받을 준비를 끝내야 한다. */
	ret = apple_pcie_port_setup_irq(port);
	/* [한국어] 실패하면 포트를 포기한다. */
	if (ret)
		return ret;

	/* Reset all RID/SID mappings, and check for RAZ/WI registers */
	/* [한국어] 옆의 상류 주석대로 RID2SID 표를 전부 지우면서, 동시에 실제 칸 수를
	 * 알아낸다. 상한은 세대 상수표의 최대값이다. */
	for (i = 0; i < pcie->hw->max_rid2sid; i++) {
		/* [한국어] 표식 값 0xbad1d 를 쓰고 되읽는다. 없는 칸은 RAZ/WI(읽으면 0, 쓰면
		 * 무시)라 되읽은 값이 다르므로, 그 지점이 곧 표의 끝이다. 0xbad1d 는 유효
		 * 비트(31)가 꺼져 있어 하드웨어가 실제 매핑으로 오해하지 않는 값이다. */
		if (apple_pcie_rid2sid_write(port, i, 0xbad1d) != 0xbad1d)
			break;
		/* [한국어] 존재하는 칸이면 0 으로 지워 초기화한다. 부트로더가 남긴 매핑을
		 * 없애는 것이 이 루프의 또 다른 목적이다. */
		apple_pcie_rid2sid_write(port, i, 0);
	}

	/* [한국어] 탐지된 칸 수를 디버그 로그로 남긴다. 루프를 빠져나온 i 가 곧 개수다. */
	dev_dbg(pcie->dev, "%pOF: %d RID/SID mapping entries\n", np, i);

	/* [한국어] 그 개수를 보관한다. 이후 비트맵 탐색과 순회의 상한이 된다. */
	port->sid_map_sz = i;

	/* [한국어] 완성된 포트를 컨트롤러의 목록 꼬리에 매단다. 이후
	 * apple_pcie_get_port() 이 이 목록을 순회해 장치가 속한 포트를 찾는다. */
	list_add_tail(&port->entry, &pcie->ports);
	/* [한국어] 링크 업 대기용 완료 객체를 초기화한다. 컨트롤러마다 하나뿐인 객체를
	 * 포트를 세울 때마다 다시 초기화하는 구조다(struct apple_pcie 의 event 필드
	 * 설명에 적어 둔 관찰 참조). */
	init_completion(&pcie->event);

	/* In the success path, we keep a reference to np around */
	/* [한국어] 옆의 상류 주석대로, 성공 경로에서는 DT 노드 참조를 하나 더 잡아 둔다.
	 * 호출자가 자식 노드를 순회하는 스코프 매크로를 쓰기 때문에, 반복이 끝나면
	 * 노드 참조가 자동으로 풀린다 — 계속 들고 있으려면 여기서 올려 두어야 한다. */
	of_node_get(np);

	/* [한국어] 링크 업/다운 IRQ 를 등록한다. LTSSM 을 출발시키기 전에 해야 링크 업
	 * 인터럽트를 놓치지 않는다. */
	ret = apple_pcie_port_register_irqs(port);
	/* [한국어] 이 함수는 언제나 0 을 돌려주므로 이 검사는 실질적으로 죽어 있다
	 * (apple_pcie_port_register_irqs 의 관찰 1 참조). */
	WARN_ON(ret);

	/* [한국어] 링크 훈련을 출발시킨다. 이 쓰기 이후 하드웨어가 상대와 협상을 시작하고,
	 * 성공하면 링크 업 인터럽트가 올라온다. */
	writel_relaxed(PORT_LTSSMCTL_START, port->base + PORT_LTSSMCTL);

	/* [한국어] 링크 업을 최대 HZ/10(= 100ms) 기다린다. 반환값 0 이 시한 초과다. */
	if (!wait_for_completion_timeout(&pcie->event, HZ / 10))
		/* [한국어] 시한을 넘겨도 경고만 남기고 성공으로 돌아간다 — 슬롯이 비어 있는 것이
		 * 정상인 경우가 많기 때문이다. 링크가 없으면 그 아래에서 장치가 열거되지 않을 뿐이다. */
		dev_warn(pcie->dev, "%pOF link didn't come up\n", np);

	/* [한국어] 포트 준비 완료. */
	return 0;
}

/* [한국어] 이 컨트롤러가 MSI '부모' 도메인으로서 갖는 성질을 선언한다. MSI 코어는
 * 장치마다 자식 도메인을 즉석에서 만드는데, 그때 무엇을 지원하고 무엇을 반드시
 * 켜야 하는지를 이 표에서 읽는다. */
static const struct msi_parent_ops apple_msi_parent_ops = {
	/* [한국어] 지원 가능한 기능들 — 일반 플래그 전부에 더해 MSI-X 와 multi-MSI 를
	 * 지원한다. multi-MSI 는 장치 하나가 연속된 벡터 여러 개를 받는 방식이라,
	 * 할당 쪽에서 정렬된 연속 영역을 찾아야 하는 이유가 된다. */
	.supported_flags	= (MSI_GENERIC_FLAGS_MASK	|
				   MSI_FLAG_PCI_MSIX		|
				   MSI_FLAG_MULTI_PCI_MSI),
	/* [한국어] 자식 도메인에 반드시 켜야 하는 플래그들 — 기본 도메인 연산과 기본 칩
	 * 연산을 쓰고, PCI MSI 의 마스크 처리를 부모(이 계층)가 맡는다는 뜻이다. */
	.required_flags		= (MSI_FLAG_USE_DEF_DOM_OPS	|
				   MSI_FLAG_USE_DEF_CHIP_OPS	|
				   MSI_FLAG_PCI_MSI_MASK_PARENT),
	/* [한국어] 이 계층의 irq_chip 이 EOI 콜백을 갖고 있음을 알린다.
	 * apple_msi_bottom_chip 의 irq_eoi 설정과 짝을 이룬다. */
	.chip_flags		= MSI_CHIP_FLAG_SET_EOI,
	/* [한국어] 이 도메인이 PCI MSI 버스용임을 표시하는 토큰. 장치가 도메인을 찾을 때
	 * 이 토큰으로 걸러진다. */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	/* [한국어] 자식 도메인 정보를 채우는 공용 구현. irq-msi-lib 가 제공하며,
	 * 위 supported/required 플래그를 근거로 자동 처리해 준다. */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어]
 * apple_msi_init - DT 의 msi-ranges 를 읽어 MSI 부모 도메인을 세운다
 *
 * @pcie: 컨트롤러 객체. 이 함수가 fwspec, nvecs, bitmap 을 채운다.
 * @return: 0 성공, DT 파싱 오류, -ENOMEM, -ENXIO(부모 도메인 없음).
 *
 * 이 하드웨어의 MSI 는 독립된 인터럽트 컨트롤러가 아니라 상위 컨트롤러에서
 * 벡터 구간을 빌려 쓰는 형태다. 그 사실이 DT 의 msi-ranges 속성에 담겨 있다 —
 * "어느 컨트롤러의, 어느 인터럽트부터, 몇 개" 라는 세 정보다. 이 함수는 그
 * 셋을 각각 fwspec(어느 컨트롤러의 어느 인터럽트부터)과 nvecs(몇 개)로 옮기고,
 * 벡터 배분용 비트맵을 잡고, 부모 도메인을 찾아 그 위에 MSI 도메인을 얹는다.
 * 포트 설정보다 먼저 불려야 한다 — 포트 레지스터에 벡터 개수를 새기기 때문이다.
 * 실행 컨텍스트: probe 경로. 메모리 할당이 있어 잠들 수 있다.
 * 에러 경로: 각 단계에서 곧장 되돌아가며, devm 할당은 자동 정리된다.
 *
 * [상류 코드 관찰] 두 가지.
 *  1. info.size 를 초기화 목록에서 pcie->nvecs 로 채우는데, 그 시점의 nvecs 는
 *     아직 0 이다. 브리지 사설 영역이 kzalloc 으로 0 초기화되고(probe.c:1815),
 *     실제 값은 아래 of_property_read_u32_index() 로 나중에 채워지기 때문이다.
 *     즉 도메인은 size 0 으로 만들어진다. 그 결과가 무엇인지는 도메인 생성
 *     구현(kernel/irq)이 이 트리에 없어 확인 못 함.
 *  2. of_parse_phandle_with_args() 가 args.np 에 잡아 준 노드 참조를 놓아 주는
 *     of_node_put() 호출이 없다.
 *
 * 호출 체인:
 *   apple_pcie_probe() → [apple_msi_init]
 *     → of_parse_phandle_with_args(), of_property_read_u32_index(),
 *       of_phandle_args_to_fwspec(), irq_find_matching_fwspec(),
 *       msi_create_parent_irq_domain()
 */
static int apple_msi_init(struct apple_pcie *pcie)
{
	/* [한국어] 컨트롤러의 펌웨어 노드. DT 속성을 읽는 기준이자 만들 도메인의 소유자다. */
	struct fwnode_handle *fwnode = dev_fwnode(pcie->dev);
	/* [한국어] 만들 도메인의 서술. 필드를 미리 채워 두고 아래에서 parent 만 덧붙인다. */
	struct irq_domain_info info = {
		/* [한국어] 도메인의 소유 노드. */
		.fwnode		= fwnode,
		/* [한국어] 할당/해제 콜백 표. */
		.ops		= &apple_msi_domain_ops,
		/* [한국어] 도메인 크기. [상류 코드 관찰] 이 시점의 nvecs 는 아직 0 이다
		 * (위 함수 주석의 관찰 1 참조). */
		.size		= pcie->nvecs,
		/* [한국어] 콜백들이 되찾을 컨트롤러 객체. */
		.host_data	= pcie,
	};
	/* [한국어] msi-ranges 에서 뽑아 낼 phandle 인자. 빈 초기화로 잔여 값을 없앤다. */
	struct of_phandle_args args = {};
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] msi-ranges 의 첫 항목을 파싱한다. 인자 개수는 가리키는 컨트롤러의
	 * #interrupt-cells 가 결정하므로, 그 속성 이름을 함께 넘겨 준다. 결과로
	 * args.np(부모 컨트롤러 노드), args.args(인터럽트 지정자 셀들),
	 * args.args_count(셀 개수)가 채워진다. */
	ret = of_parse_phandle_with_args(to_of_node(fwnode), "msi-ranges",
					 "#interrupt-cells", 0, &args);
	/* [한국어] 속성이 없거나 형식이 틀리면 MSI 를 쓸 수 없다. */
	if (ret)
		return ret;

	/* [한국어] 같은 속성에서 벡터 개수를 읽는다. 인덱스가 args_count + 1 인 이유:
	 * 속성은 [phandle][지정자 셀 args_count 개][개수] 순서이므로, 0 번이 phandle,
	 * 1..args_count 가 지정자, 그 다음 칸이 개수다. */
	ret = of_property_read_u32_index(to_of_node(fwnode), "msi-ranges",
					 args.args_count + 1, &pcie->nvecs);
	/* [한국어] 개수를 읽지 못하면 벡터 배분을 할 수 없다. */
	if (ret)
		return ret;

	/* [한국어] 파싱한 부모 노드와 셀들을 fwspec 형태로 옮긴다. 이 사본이 앞으로
	 * 벡터를 할당할 때마다 복사되어 '시작 지정자 + 벡터 번호' 로 쓰인다. */
	of_phandle_args_to_fwspec(args.np, args.args, args.args_count,
				  &pcie->fwspec);

	/* [한국어] 벡터 사용 현황 비트맵을 nvecs 비트만큼 잡는다. devm 이라 자동 해제된다. */
	pcie->bitmap = devm_bitmap_zalloc(pcie->dev, pcie->nvecs, GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!pcie->bitmap)
		return -ENOMEM;

	/* [한국어] 이 지정자를 다룰 부모 도메인을 찾는다. DOMAIN_BUS_WIRED 는 '보통의
	 * 배선 인터럽트' 도메인을 뜻하며, 상위 컨트롤러의 일반 IRQ 도메인이 여기 걸린다.
	 * 그 도메인을 등록하는 드라이버는 drivers/irqchip 에 있어 이 트리에서 확인 못 함. */
	info.parent = irq_find_matching_fwspec(&pcie->fwspec, DOMAIN_BUS_WIRED);
	/* [한국어] 부모가 아직 등록되지 않았거나 지정자가 맞지 않는 경우.
	 * [상류 코드 관찰] 이때 -ENXIO 를 돌려주므로 probe 가 나중에 다시 시도되지 않는다
	 * — 부모가 늦게 등록되는 순서 문제라면 -EPROBE_DEFER 가 필요한 자리인데,
	 * 실제로 그런 순서가 생길 수 있는지는 이 트리에서 확인 못 함. */
	if (!info.parent) {
		/* [한국어] 원인을 로그로 남긴다. */
		dev_err(pcie->dev, "failed to find parent domain\n");
		return -ENXIO;
	}

	/* [한국어] 위 정보와 부모 연산 표로 MSI 부모 도메인을 만든다. 이 도메인이 있어야
	 * 아래 붙는 PCIe 장치들이 MSI/MSI-X 를 배정받을 수 있다. */
	if (!msi_create_parent_irq_domain(&info, &apple_msi_parent_ops)) {
		/* [한국어] 생성 실패는 사실상 메모리 부족이다. */
		dev_err(pcie->dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}
	/* [한국어] MSI 준비 완료. */
	return 0;
}

/* [한국어]
 * apple_pcie_lookup - 컨트롤러 device 에서 이 드라이버의 사설 객체를 되찾는다
 *
 * @dev: 컨트롤러의 struct device.
 * @return: 그 컨트롤러의 apple_pcie 객체.
 *
 * 한 줄짜리지만 두 단계의 간접이 겹쳐 있어 따로 떼어 두었다. 먼저 device 의
 * 드라이버 데이터에서 호스트 브리지를 꺼낸다 — 이것이 가능한 근거는
 * pci_host_common_init() 이 platform_set_drvdata() 로 브리지를 심어 두기
 * 때문이다(pci-host-common.c:255). 그 다음 브리지 뒤에 붙어 있는 사설 영역을
 * 얻는다 — apple_pcie_probe() 이 devm_pci_alloc_host_bridge(dev, sizeof(*pcie))
 * 로 브리지와 함께 할당했기 때문에 성립한다.
 * 실행 컨텍스트: 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   apple_pcie_init() / apple_pcie_get_port() → [apple_pcie_lookup]
 *     → dev_get_drvdata(), pci_host_bridge_priv()
 */
static struct apple_pcie *apple_pcie_lookup(struct device *dev)
{
	/* [한국어] drvdata(= 호스트 브리지)에서 사설 영역 포인터를 얻어 돌려준다. */
	return pci_host_bridge_priv(dev_get_drvdata(dev));
}

/* [한국어]
 * apple_pcie_get_port - PCI 장치가 매달린 루트 포트의 드라이버 객체를 찾는다
 *
 * @pdev: 방금 등장했거나 사라지는 PCI 장치.
 * @return: 그 장치가 속한 apple_pcie_port, 또는 NULL(루트 포트 자신이거나 못 찾음).
 *
 * RID2SID 표는 포트마다 따로 있으므로, 장치를 등록하려면 먼저 '어느 포트 아래
 * 있는가' 를 알아야 한다. 그 답을 세 단계로 찾는다. (1) 장치의 sysdata 로 걸린
 * ECAM 창에서 컨트롤러 device 를 얻고 사설 객체를 되찾는다. (2) PCI 코어에게
 * 이 장치의 루트 포트를 물어본다. (3) 그 루트 포트의 장치 번호와 같은 idx 를
 * 가진 포트를 목록에서 찾는다 — 포트의 idx 가 곧 루트 포트의 PCI 장치 번호라는
 * 사실이 여기서 쓰인다.
 * 루트 포트 자신이 대상일 때 NULL 을 돌려주는 이유: 루트 포트는 컨트롤러의
 * 일부이지 IOMMU 매핑이 필요한 엔드포인트가 아니기 때문이다.
 * 실행 컨텍스트: 장치 등록/해제 경로(프로세스 컨텍스트). 목록 순회에 락을 잡지
 * 않는데, 목록은 probe 때 다 만들어지고 이후 바뀌지 않으므로 성립한다.
 *
 * [상류 코드 관찰] WARN_ON(!pcie) 로 NULL 을 검사하지만,
 * pci_host_bridge_priv() 가 인자가 NULL 일 때 NULL 을 돌려주는지는 그 정의가
 * include/linux/pci.h 에 있어 이 트리에서 확인 못 함.
 *
 * 호출 체인:
 *   apple_pcie_enable_device() / apple_pcie_disable_device()
 *     → [apple_pcie_get_port] → apple_pcie_lookup(), pcie_find_root_port()
 */
static struct apple_pcie_port *apple_pcie_get_port(struct pci_dev *pdev)
{
	/* [한국어] ECAM 창 객체. PCI 코어가 이 브리지 아래 모든 장치의 sysdata 로
	 * 걸어 두었다(pci_host_common_init 이 bridge->sysdata = cfg 로 설정한다). */
	struct pci_config_window *cfg = pdev->sysdata;
	/* [한국어] 되찾을 컨트롤러 객체. */
	struct apple_pcie *pcie;
	/* [한국어] 이 장치의 루트 포트에 해당하는 PCI 장치. */
	struct pci_dev *port_pdev;
	/* [한국어] 목록 순회 커서. */
	struct apple_pcie_port *port;

	/* [한국어] ECAM 창의 parent(= 컨트롤러 device)에서 사설 객체를 되찾는다.
	 * cfg->parent 가 유효한 근거는 pci_ecam_create() 가 ops->init 을 부르기 전에
	 * 그 필드를 채우기 때문이다. */
	pcie = apple_pcie_lookup(cfg->parent);
	/* [한국어] 위 관찰 참조 — 실효성이 불확실한 방어 검사다. */
	if (WARN_ON(!pcie))
		return NULL;

	/* Find the root port this device is on */
	/* [한국어] 옆의 상류 주석대로 이 장치의 루트 포트를 찾는다. PCI 코어가 부모 사슬을
	 * 거슬러 올라가 루트 포트를 돌려준다. */
	port_pdev = pcie_find_root_port(pdev);

	/* If finding the port itself, nothing to do */
	/* [한국어] 옆의 상류 주석대로, 루트 포트를 못 찾았거나 대상이 루트 포트 자신이면
	 * 매핑할 것이 없다. 루트 포트는 IOMMU 뒤에 있는 엔드포인트가 아니기 때문이다. */
	if (WARN_ON(!port_pdev) || pdev == port_pdev)
		return NULL;

	/* [한국어] 이 컨트롤러의 포트 목록을 훑는다. 포트 수가 많아야 서너 개라
	 * 선형 탐색으로 충분하다. */
	list_for_each_entry(port, &pcie->ports, entry) {
		/* [한국어] 포트의 idx 는 루트 포트의 PCI 장치 번호와 같은 값이다
		 * (apple_pcie_setup_port 이 DT reg 첫 셀을 >> 11 해서 만든 값). 그래서
		 * PCI_SLOT(devfn) 과 직접 비교하면 짝이 맞는다. */
		if (port->idx == PCI_SLOT(port_pdev->devfn))
			/* [한국어] 찾았다. */
			return port;
	}

	/* [한국어] 목록에 없는 포트 번호 — 정상 흐름에서는 오지 않는다. */
	return NULL;
}

/* [한국어]
 * apple_pcie_enable_device - 새 PCI 장치의 RID 를 IOMMU SID 로 매핑한다
 *
 * @bridge: 호스트 브리지. [상류 코드 관찰] 콜백 시그니처 때문에 받지만 본문에서
 *          쓰지 않는다 — 필요한 정보를 전부 pdev 에서 거슬러 얻기 때문이다.
 * @pdev: 활성화되는 PCI 장치.
 * @return: 0 성공(또는 매핑이 필요 없는 경우), -ENOSPC(표가 가득), of_map_id 오류.
 *
 * PCIe 장치가 DMA 를 하면 그 트랜잭션에는 장치의 RID(버스/장치/함수를 합친
 * 16비트 식별자)가 실린다. IOMMU 는 그 요청을 SID(Stream ID) 단위로 구분해
 * 주소 변환을 적용하므로, 둘 사이의 대응표를 컨트롤러가 갖고 있어야 한다.
 * 그 표가 RID2SID 이고, 이 콜백이 장치가 등장할 때마다 한 칸을 채운다.
 * RID → SID 대응 규칙 자체는 하드코딩이 아니라 DT 의 iommu-map 속성에 있다
 * — 그래서 of_map_id() 로 물어본다.
 * 실행 컨텍스트: PCI 코어가 장치를 활성화할 때(프로세스 컨텍스트). 뮤텍스를
 * 잡으므로 잠들 수 있어야 한다.
 * 에러 경로: 포트를 못 찾으면(루트 포트 자신 등) 매핑 없이 성공으로 돌아간다.
 * DT 매핑이 없으면 그 오류를 전달해 장치 활성화가 실패한다.
 *
 * 호출 체인:
 *   (PCI 코어의 pci_enable_device 경로 → bridge->enable_device)
 *     → [apple_pcie_enable_device]
 *     → apple_pcie_get_port(), of_map_id(), apple_pcie_rid2sid_write()
 */
static int apple_pcie_enable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
{
	/* [한국어] sid 는 DT 에서 얻을 IOMMU 스트림 번호, rid 는 이 장치의 16비트
	 * Requester ID(버스<<8 | 장치<<3 | 함수). pci_dev_id() 가 그 조합을 만들어 준다. */
	u32 sid, rid = pci_dev_id(pdev);
	/* [한국어] 이 장치가 속한 포트. */
	struct apple_pcie_port *port;
	/* [한국어] idx 는 잡은 표 칸 번호, err 은 DT 매핑 결과. */
	int idx, err;

	/* [한국어] 어느 포트 아래인지 찾는다. */
	port = apple_pcie_get_port(pdev);
	/* [한국어] 루트 포트 자신이거나 찾지 못한 경우 — 매핑할 것이 없으므로 성공으로
	 * 돌아간다. 여기서 오류를 내면 루트 포트 자체가 활성화되지 못한다. */
	if (!port)
		return 0;

	/* [한국어] 어떤 버스에 어느 포트로 붙었는지 디버그 로그로 남긴다.
	 * pdev->bus->self 는 이 장치가 매달린 버스의 상위 브리지(= 루트 포트)다. */
	dev_dbg(&pdev->dev, "added to bus %s, index %d\n",
		pci_name(pdev->bus->self), port->idx);

	/* [한국어] DT 의 iommu-map / iommu-map-mask 를 따라 RID 를 SID 로 번역한다.
	 * 이 두 속성이 '어떤 RID 범위가 어떤 IOMMU 의 어떤 SID 로 가는가' 를 서술한다.
	 * 다섯 번째 인자 NULL 은 대상 IOMMU 노드를 돌려받지 않겠다는 뜻 — 이 드라이버는
	 * 번호만 필요하고 어느 IOMMU 인지는 하드웨어가 이미 고정하고 있기 때문이다.
	 * IOMMU(DART) 드라이버 자체는 drivers/iommu 가 이 트리에 없어 확인 못 함. */
	err = of_map_id(port->pcie->dev->of_node, rid, "iommu-map",
			"iommu-map-mask", NULL, &sid);
	/* [한국어] 매핑 규칙이 없으면 이 장치의 DMA 를 안전하게 다룰 수 없으므로 실패시킨다. */
	if (err)
		return err;

	/* [한국어] 표와 비트맵을 만지기 전에 컨트롤러 뮤텍스를 잡는다. 포트별 락이 아니라
	 * 컨트롤러 단위 락 하나로 모든 포트의 표를 지키는 구조다. */
	mutex_lock(&port->pcie->lock);

	/* [한국어] 실제로 존재하는 칸 범위(sid_map_sz) 안에서 빈 칸 하나를 잡는다.
	 * 마지막 인자 0 은 order 0, 즉 한 칸만 잡는다는 뜻이다. */
	idx = bitmap_find_free_region(port->sid_map, port->sid_map_sz, 0);
	/* [한국어] 빈 칸을 찾았을 때만 표를 쓴다. */
	if (idx >= 0) {
		/* [한국어] 그 칸에 매핑을 새긴다 — 유효 비트(31)를 세우고, SID 를 비트 16 부터
		 * 놓고, 하위 16비트에 RID 를 그대로 넣는다. 하위 필드를 버스/장치/함수로 나누는
		 * 시프트 상수들이 정의되어 있지만 쓰이지 않는 이유가 이것이다 — RID 가 이미
		 * 그 배치와 같은 16비트 값이라 통째로 넣으면 된다. */
		apple_pcie_rid2sid_write(port, idx,
					 PORT_RID2SID_VALID |
					 (sid << PORT_RID2SID_SID_SHIFT) | rid);

		/* [한국어] 어떤 RID 가 어떤 SID 의 몇 번 칸에 실렸는지 디버그 로그로 남긴다. */
		dev_dbg(&pdev->dev, "mapping RID%x to SID%x (index %d)\n",
			rid, sid, idx);
	}

	/* [한국어] 뮤텍스 해제. */
	mutex_unlock(&port->pcie->lock);

	/* [한국어] 칸을 잡았으면 성공, 표가 가득 찼으면 -ENOSPC 로 장치 활성화를 막는다
	 * — 매핑 없이 DMA 를 허용하면 IOMMU 가 그 요청을 막아 조용히 오동작하기 때문이다. */
	return idx >= 0 ? 0 : -ENOSPC;
}

/* [한국어]
 * apple_pcie_disable_device - 사라지는 장치의 RID2SID 매핑을 지운다
 *
 * @bridge: 호스트 브리지. [상류 코드 관찰] enable 쪽과 마찬가지로 쓰이지 않는다.
 * @pdev: 비활성화되는 PCI 장치.
 * @return: 없음 — 해제 경로라 실패를 전할 곳이 없다.
 *
 * enable 의 짝이다. 어느 칸에 넣었는지를 따로 기억해 두지 않으므로, 쓰이고 있는
 * 칸들을 훑으며 RID 가 일치하는 칸을 찾아 지운다. 표 크기가 수십~수백 칸이고
 * 장치 제거가 드문 일이라 선형 탐색으로 충분하다.
 * 실행 컨텍스트: PCI 코어의 장치 비활성화 경로(프로세스 컨텍스트). 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   (PCI 코어의 장치 비활성화 → bridge->disable_device)
 *     → [apple_pcie_disable_device]
 *     → apple_pcie_get_port(), port_rid2sid_addr(), apple_pcie_rid2sid_write()
 */
static void apple_pcie_disable_device(struct pci_host_bridge *bridge, struct pci_dev *pdev)
{
	/* [한국어] 이 장치가 속한 포트. */
	struct apple_pcie_port *port;
	/* [한국어] 지울 대상의 Requester ID. */
	u32 rid = pci_dev_id(pdev);
	/* [한국어] 순회 중인 칸 번호. */
	int idx;

	/* [한국어] 어느 포트 아래인지 찾는다. */
	port = apple_pcie_get_port(pdev);
	/* [한국어] 루트 포트 자신이거나 못 찾으면 지울 것도 없다. */
	if (!port)
		return;

	/* [한국어] 표와 비트맵을 만지므로 뮤텍스를 잡는다. */
	mutex_lock(&port->pcie->lock);

	/* [한국어] 쓰이고 있다고 표시된 칸만 훑는다. 비어 있는 칸은 볼 필요가 없다. */
	for_each_set_bit(idx, port->sid_map, port->sid_map_sz) {
		/* [한국어] 그 칸에서 읽은 값. */
		u32 val;

		/* [한국어] 칸의 현재 값을 읽는다. */
		val = readl_relaxed(port_rid2sid_addr(port, idx));
		/* [한국어] 하위 16비트가 RID 자리이므로 그 부분만 비교한다. 상위에는 유효 비트와
		 * SID 가 들어 있어 그대로 비교하면 맞지 않는다. */
		if ((val & 0xffff) == rid) {
			/* [한국어] 0 을 써서 매핑을 무효화한다(유효 비트가 함께 지워진다). */
			apple_pcie_rid2sid_write(port, idx, 0);
			/* [한국어] 비트맵에서도 그 칸을 반납한다. order 0 = 한 칸. */
			bitmap_release_region(port->sid_map, idx, 0);
			/* [한국어] 어떤 값을 어느 칸에서 지웠는지 남긴다. */
			dev_dbg(&pdev->dev, "Released %x (%d)\n", val, idx);
			/* [한국어] 한 장치는 칸 하나만 쓰므로 찾는 즉시 끝낸다. */
			break;
		}
	}

	/* [한국어] 뮤텍스 해제. 못 찾았더라도 조용히 끝난다 — 애초에 매핑이 없던
	 * 장치(루트 포트 아래가 아닌 경우 등)일 수 있기 때문이다. */
	mutex_unlock(&port->pcie->lock);
}

/* [한국어]
 * apple_pcie_init - ECAM 창을 만드는 도중 불려 모든 포트를 세운다
 *
 * @cfg: 방금 만들어진 ECAM 설정공간 창. parent 로 컨트롤러 device 를 얻는다.
 * @return: 0 성공, -ENOENT(사설 객체를 못 찾음), 포트 설정 실패 코드.
 *
 * pci_ecam_ops 의 init 후크다. 이 자리가 중요한 이유는 타이밍 때문이다 —
 * ECAM 창은 이미 매핑되었지만 아직 버스 스캔은 시작되지 않은 시점이므로,
 * 여기서 포트를 살려 두면 스캔이 시작될 때 링크가 이미 올라와 있다. 포트가
 * 죽어 있으면 스캔이 그 아래 장치를 찾지 못한다.
 * DT 자식 노드 하나가 루트 포트 하나에 대응하며, 그 순회를 스코프 매크로로
 * 돌기 때문에 반복이 끝나면 노드 참조가 자동으로 풀린다 —
 * apple_pcie_setup_port() 이 성공 경로에서 of_node_get() 을 부르는 이유다.
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). 포트마다 수백 ms 잠들 수 있다.
 * 에러 경로: 포트 하나라도 실패하면 그 오류를 그대로 올려 보내 ECAM 생성 자체가
 * 실패하고, 결국 probe 가 실패한다. 이미 세운 포트를 되돌리는 코드는 없다.
 *
 * 호출 체인:
 *   apple_pcie_probe() → pci_host_common_init() → pci_host_common_ecam_create()
 *     → pci_ecam_create() → [apple_pcie_init] → apple_pcie_setup_port()
 */
static int apple_pcie_init(struct pci_config_window *cfg)
{
	/* [한국어] ECAM 창을 만든 device — 곧 이 컨트롤러다. */
	struct device *dev = cfg->parent;
	/* [한국어] 되찾을 사설 객체. */
	struct apple_pcie *pcie;
	/* [한국어] 포트 설정 결과. */
	int ret;

	/* [한국어] drvdata → 브리지 → 사설 영역 순으로 되찾는다. */
	pcie = apple_pcie_lookup(dev);
	/* [한국어] 방어 검사(apple_pcie_get_port 의 관찰 참조). */
	if (WARN_ON(!pcie))
		return -ENOENT;

	/* [한국어] DT 에서 status 가 disabled 가 아닌 자식 노드만 순회한다. 스코프 판이라
	 * 반복 변수 of_port 의 참조가 반복 끝에 자동으로 풀린다. */
	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		/* [한국어] 자식 노드 하나 = 루트 포트 하나를 살린다. */
		ret = apple_pcie_setup_port(pcie, of_port);
		/* [한국어] 포트 하나라도 실패하면 전체를 실패시킨다. */
		if (ret) {
			/* [한국어] 어느 노드에서 무슨 오류가 났는지 남긴다. */
			dev_err(dev, "Port %pOF setup fail: %d\n", of_port, ret);
			return ret;
		}
	}

	/* [한국어] 모든 포트 준비 완료. 이제 코어가 버스를 스캔한다. */
	return 0;
}

/* [한국어] 이 컨트롤러의 ECAM 연산 표. 설정공간 접근은 전부 표준 구현을 그대로
 * 쓰고, 이 드라이버 고유의 동작은 init / enable_device / disable_device 세
 * 후크에만 들어간다. 이것이 파일 맨 위 상류 주석이 말하는 "ECAM compliant" 의
 * 실질적 의미다. */
static const struct pci_ecam_ops apple_pcie_cfg_ecam_ops = {
	/* [한국어] ECAM 창 생성 직후 불릴 후크 — 포트를 세운다. */
	.init		= apple_pcie_init,
	/* [한국어] 장치 활성화 시 RID2SID 매핑을 잡는 후크. */
	.enable_device	= apple_pcie_enable_device,
	/* [한국어] 장치 비활성화 시 그 매핑을 놓는 후크. */
	.disable_device	= apple_pcie_disable_device,
	/* [한국어] 설정공간 접근 연산들. 셋 다 표준 구현이다. */
	.pci_ops	= {
		/* [한국어] 버스/장치/함수/오프셋을 ECAM 주소로 바꾸는 표준 구현. ECAM 은
		 * 설정공간 전체를 물리 주소 공간에 평평하게 펼치므로 주소 계산만으로 접근된다. */
		.map_bus	= pci_ecam_map_bus,
		/* [한국어] 그 주소에서 1/2/4바이트를 읽는 표준 구현. */
		.read		= pci_generic_config_read,
		/* [한국어] 그 주소에 1/2/4바이트를 쓰는 표준 구현. */
		.write		= pci_generic_config_write,
	}
};

/* [한국어]
 * apple_pcie_probe - 드라이버 진입점. 브리지를 만들고 MSI 를 세운 뒤 위임한다
 *
 * @pdev: DT 와 매칭된 플랫폼 디바이스.
 * @return: 0 성공, -ENOMEM / -ENODEV / 하위 단계의 오류.
 *
 * 이 함수 자체는 짧다. 실제 하드웨어 초기화는 ECAM 창을 만드는 도중 불리는
 * apple_pcie_init() 에서 일어나기 때문이다. 여기서 하는 일은 네 가지다.
 * (1) 호스트 브리지와 사설 객체를 한 덩어리로 할당한다. (2) 매칭 표에서 세대
 * 상수표를 꺼낸다 — 이 값이 없으면 어느 세대인지 알 수 없어 진행할 수 없다.
 * (3) 컨트롤러 공용 창을 매핑하고 락과 목록을 초기화한다. (4) MSI 부모 도메인을
 * 먼저 세운 뒤 공용 라이브러리에 나머지를 넘긴다.
 * MSI 를 포트보다 먼저 세우는 이유: 포트 설정이 벡터 개수를 포트 레지스터에
 * 새기므로 nvecs 가 이미 정해져 있어야 한다.
 * 실행 컨텍스트: 드라이버 바인딩(프로세스 컨텍스트). 아래 단계에서 여러 번 잠든다.
 * 에러 경로: 모든 실패가 곧장 되돌아가고 devm 이 자원을 정리한다. 별도의 되감기
 * 코드가 없는 것은 이 드라이버가 언바인드를 막아 두어(아래 suppress_bind_attrs)
 * 정상 제거 경로가 없기 때문이다.
 *
 * 호출 체인:
 *   (플랫폼 버스의 DT 매칭) → [apple_pcie_probe]
 *     → devm_pci_alloc_host_bridge(), of_device_get_match_data(),
 *       devm_platform_ioremap_resource(), apple_msi_init(), pci_host_common_init()
 */
static int apple_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 의 기준 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 만들 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 그 브리지 뒤에 붙을 사설 객체. */
	struct apple_pcie *pcie;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 브리지와 사설 영역을 한 번에 할당한다. 이 한 덩어리 할당 덕분에
	 * apple_pcie_lookup() 의 두 단계 되찾기가 성립한다. kzalloc 기반이라
	 * 사설 영역도 0 으로 초기화된다(probe.c:1815). */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 메모리 부족. */
	if (!bridge)
		return -ENOMEM;

	/* [한국어] 브리지 뒤의 사설 영역 포인터를 얻는다. */
	pcie = pci_host_bridge_priv(bridge);
	/* [한국어] 로그와 devm 의 기준을 심어 둔다. */
	pcie->dev = dev;
	/* [한국어] 매칭 표의 .data 에서 이 SoC 세대의 레지스터 상수표를 꺼낸다.
	 * 아래 apple_pcie_of_match 의 두 항목 중 하나가 걸린다. */
	pcie->hw = of_device_get_match_data(dev);
	/* [한국어] 상수표가 없으면 어느 세대인지 알 수 없다 — 매칭 표에 .data 를 빠뜨린
	 * 항목이 추가되는 것을 막는 방어이기도 하다. */
	if (!pcie->hw)
		return -ENODEV;
	/* [한국어] 컨트롤러 공용 레지스터 창을 매핑한다. 인덱스 1 인 이유는 reg 0번이
	 * ECAM 설정공간 창이고 그것은 공용 라이브러리가 따로 매핑하기 때문이다.
	 * 포트별 창은 인덱스 2 부터라는 사실도 여기서 따라 나온다. */
	pcie->base = devm_platform_ioremap_resource(pdev, 1);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	/* [한국어] MSI 비트맵과 RID2SID 표를 함께 지킬 뮤텍스를 초기화한다. */
	mutex_init(&pcie->lock);
	/* [한국어] 포트 목록을 빈 상태로 초기화한다. 채우는 것은 apple_pcie_init() 이다. */
	INIT_LIST_HEAD(&pcie->ports);

	/* [한국어] MSI 부모 도메인을 먼저 세운다. 포트 설정이 nvecs 를 필요로 하기 때문이다. */
	ret = apple_msi_init(pcie);
	/* [한국어] MSI 준비 실패는 곧 probe 실패다 — 이 드라이버는 Kconfig 에서
	 * PCI_MSI 를 요구하므로 MSI 없이 동작하는 경로가 없다. */
	if (ret)
		return ret;

	/* [한국어] 나머지를 공용 라이브러리에 넘긴다. 이 안에서 ECAM 창이 만들어지고,
	 * 그 도중 apple_pcie_init() 이 불려 포트가 살아나며, 마지막에 버스 스캔이
	 * 시작된다. 즉 이 한 줄 뒤에서 이 파일의 나머지 절반이 실행된다. */
	return pci_host_common_init(pdev, bridge, &apple_pcie_cfg_ecam_ops);
}

/* [한국어] DT compatible 문자열과 세대 상수표를 잇는 매칭 표. .data 에 담긴
 * 포인터를 probe 가 of_device_get_match_data() 로 꺼낸다. */
static const struct of_device_id apple_pcie_of_match[] = {
	/* [한국어] T602x 세대(M2 Pro 계열). 레지스터 자리가 옮겨졌고 MSIMAP 표를 쓴다. */
	{ .compatible = "apple,t6020-pcie",	.data = &t602x_hw },
	/* [한국어] T8103 세대(초대 M1). compatible 이름에 세대가 없는 것은 이쪽이 먼저
	 * 정의되었기 때문이며, 새 세대가 나올 때마다 구체적인 이름을 앞에 덧붙이는 방식이다. */
	{ .compatible = "apple,pcie",		.data = &t8103_hw },
	/* [한국어] 표의 끝을 알리는 빈 항목. 이것이 없으면 매칭 코드가 배열을 넘어간다. */
	{ }
};
/* [한국어] 모듈로 빌드될 때 이 매칭 표를 모듈 정보에 심어, DT 노드만 보고도
 * 어떤 모듈을 올려야 하는지 사용자 공간(udev)이 알 수 있게 한다. */
MODULE_DEVICE_TABLE(of, apple_pcie_of_match);

/* [한국어] 플랫폼 드라이버 서술. 이 컨트롤러는 PCI 장치가 아니라 SoC 내부
 * 블록이므로 PCI 드라이버가 아니라 플랫폼 드라이버로 등록된다. */
static struct platform_driver apple_pcie_driver = {
	/* [한국어] 바인딩 시 불릴 진입점. */
	.probe	= apple_pcie_probe,
	/* [한국어] 드라이버 속성 묶음. */
	.driver	= {
		/* [한국어] 드라이버 이름. sysfs 경로와 로그에 쓰인다. */
		.name			= "pcie-apple",
		/* [한국어] 위 DT 매칭 표를 건다. */
		.of_match_table		= apple_pcie_of_match,
		/* [한국어] sysfs 로 수동 바인드/언바인드하는 속성을 만들지 않는다. 이 드라이버에
		 * remove 경로가 없고 포트/IRQ 를 되돌리는 코드도 없으므로, 언바인드를 아예
		 * 막는 것이 안전하기 때문이다. */
		.suppress_bind_attrs	= true,
	},
};
/* [한국어] probe/remove 를 가진 표준 모듈 진입/종료 함수를 자동으로 만들어 준다.
 * tristate 라 내장도 모듈도 될 수 있다. */
module_platform_driver(apple_pcie_driver);

/* [한국어] modinfo 에 보일 설명 문자열. */
MODULE_DESCRIPTION("Apple PCIe host bridge driver");
/* [한국어] 라이선스 선언. 이것이 없으면 커널이 모듈을 오염(tainted)으로 표시하고,
 * GPL 전용 심볼(EXPORT_SYMBOL_GPL, 예: pci_host_common_init)을 쓸 수 없다. */
MODULE_LICENSE("GPL v2");
