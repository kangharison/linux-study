// SPDX-License-Identifier: GPL-2.0+
/*
 * APM X-Gene PCIe Driver
 *
 * Copyright (c) 2014 Applied Micro Circuits Corporation.
 *
 * Author: Tanmay Inamdar <tinamdar@apm.com>.
 */
/*
 * [한국어 설명] APM X-Gene SoC 내장 PCIe 호스트 브리지 드라이버 (pci-xgene.c)
 *
 * === 파일의 역할 ===
 * AppliedMicro(APM) X-Gene SoC 에 들어 있는 PCIe 루트 컴플렉스를 초기화하고
 * 리눅스 PCI 서브시스템에 등록한다. DesignWare 같은 남의 IP 가 아니라 자체
 * 컨트롤러라, 설정공간 접근 방식과 주소 변환 창을 이 파일이 직접 구현한다.
 * 하는 일은 넷이다. (1) 아웃바운드 창 세 개(OMR1/OMR2/OMR3 계열 레지스터)로
 * CPU 주소를 PCI 주소로 옮기고, 인바운드 창 세 개(IBAR/IR/PIM 계열)로 PCI
 * 주소를 로컬 DDR 주소로 되옮긴다. 후자가 없으면 엔드포인트의 DMA 가 시스템
 * 메모리에 닿지 못한다. (2) 설정공간 접근을 중계한다 — 이 하드웨어는 ECAM 이
 * 아니라 "접근하기 전에 버스/장치/함수 번호를 RTDID 레지스터에 써 두고, 고정된
 * 창을 통해 읽고 쓴다" 는 방식이다. (3) v1 실리콘의 RRS(Configuration Request
 * Retry Status) 로직 버그를 설정공간 읽기 경로에서 우회한다. (4) 링크 폭과
 * 세대를 읽어 부팅 로그에 남긴다.
 * 이 드라이버는 부팅 경로가 둘이다 — 디바이스 트리로 오는 플랫폼 드라이버
 * 경로와, ACPI 로 오는 pci_ecam_ops 경로다. 두 경로의 갈림길은 사실상
 * pcie_bus_to_port() 의 acpi_disabled 검사 한 줄뿐이며, 그 아래의 설정공간
 * 접근 코드는 양쪽이 그대로 공유한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 스택의 최하단, 실제 레지스터를 두드리는 호스트 컨트롤러 드라이버 자리다.
 * DT 경로: DT 매칭 → xgene_pcie_probe() 가 브리지와 사설 데이터를 한 덩어리로
 * 할당하고, csr/cfg 두 레지스터 창을 매핑하고, 클럭을 켜고,
 * xgene_pcie_setup() 으로 펌웨어가 남긴 설정을 지운 뒤 벤더/디바이스 ID 와
 * 아웃바운드·인바운드 창을 새기고 링크를 확인한 다음, pci_host_probe() 로
 * 코어에 넘긴다.
 * ACPI 경로: ECAM 창을 만드는 도중 ops->init 후크(xgene_v1_pcie_ecam_init 또는
 * xgene_v2_pcie_ecam_init)가 불려 CSR 창만 따로 매핑하고 버전을 기록한다.
 * 이쪽에는 창 설정이 없다 — 펌웨어가 이미 해 두었다는 전제이며, 그래서 ACPI
 * 경로의 초기화 코드가 DT 경로보다 훨씬 짧다.
 * 등록 이후에는 모든 설정공간 접근이 xgene_pcie_map_bus() 를 지난다. 그 안에서
 * RTDID 를 갱신하고 창 주소를 돌려주면, 읽기는 xgene_pcie_config_read32() 가,
 * 쓰기는 표준 구현이 처리한다.
 * 실행 컨텍스트: probe 경로는 프로세스 컨텍스트다. 설정공간 접근 경로는 PCI
 * 코어의 pci_lock 을 쥔 상태에서 불리므로 잠들 수 없고, 동시에 그 락 덕분에
 * "RTDID 를 써 두고 창을 읽는" 두 단계가 원자적으로 보장된다 — 이 하드웨어의
 * 설정공간 접근 방식이 성립하는 근거가 바로 그 락이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어(devm_pci_alloc_host_bridge, pci_host_bridge_priv,
 * pci_host_bridge_from_priv, pci_host_probe)와 설정공간 표준 구현
 * (pci_generic_config_read32, pci_generic_config_write32,
 * pci_generic_config_write). 아웃바운드 창 목록은 코어가 DT 의 ranges 를 파싱해
 * bridge->windows 에 채워 둔 것을 그대로 받아 쓴다.
 * 아래쪽: 클럭 프레임워크, OF 범위 파서(of_pci_dma_range_parser_init 과
 * for_each_of_pci_range), 그리고 ACPI 자원 파서(acpi_dev_get_resources).
 * 옆쪽: xgene_check_pcie_msi_ready() 가 별도 MSI 드라이버가 준비되었는지
 * 확인한다. 그 드라이버(pci-xgene-msi.c)는 같은 디렉터리에 있지만 이 파일과는
 * DT 노드와 IRQ 도메인을 통해서만 이어진다 — 직접 호출은 없다.
 * 데이터 흐름은 양방향이다. 아웃바운드는 DT 의 ranges → xgene_pcie_map_ranges()
 * → OMR 계열 레지스터, 인바운드는 DT 의 dma-ranges →
 * xgene_pcie_parse_map_dma_ranges() → IBAR/IR/PIM 계열 레지스터다.
 * NVMe 와의 접점: 이 파일은 특정 장치를 알지 못한다. 다만 인바운드 창을 설명하는
 * 상류 주석이 "DDR 영역 전체를 PCIe 공간에 매핑해 EP 장치의 DMA 가 닿게 한다" 고
 * 밝히고 있고, NVMe 컨트롤러가 PRP/SGL 로 가리키는 호스트 메모리 접근도 결국
 * 그 창을 지난다. 그 이상의 직접 결합은 코드에 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - xgene_pcie_probe(): DT 경로의 진입점. 자원 매핑 → 클럭 → 창 설정 →
 *   pci_host_probe() 위임.
 * - xgene_pcie_map_bus(): 모든 설정공간 접근의 관문. 존재하지 않는 장치와
 *   숨겨야 할 루트 BAR 를 걸러 내고, RTDID 를 갱신한 뒤 접근 주소를 돌려준다.
 * - xgene_pcie_config_read32(): 표준 읽기를 감싸 v1 실리콘의 RRS 버그를 우회한다.
 * - xgene_pcie_setup_ob_reg() / xgene_pcie_setup_ib_reg(): 아웃바운드와 인바운드
 *   주소 변환 창을 각각 레지스터에 새긴다.
 * - xgene_pcie_select_ib_reg(): 인바운드 창 세 개 중 크기에 맞는 것을 고른다.
 * - xgene_pcie_ecam_init(): ACPI 경로의 초기화. CSR 창만 매핑하고 버전을 기록한다.
 * - struct xgene_pcie: 이 컨트롤러 하나. CSR 창과 설정공간 창, 링크 상태, 버전.
 *
 * === 이 트리에서 확인할 수 없는 것 ===
 * 이 저장소는 sparse checkout 이라 drivers/{block,nvme,pci,s390,vfio} 만 있다.
 * 따라서 다음은 확인 대상 밖이다. 첫째, 비정적으로 노출된
 * xgene_v1_pcie_ecam_ops 와 xgene_v2_pcie_ecam_ops 를 참조하는 곳을 이 트리
 * 안에서 찾지 못했다 — ACPI 쪽 소비자가 drivers/acpi 에 있을 텐데 그 디렉터리가
 * 없다. 둘째, PCI_EXP_RTCTL / PCI_EXP_RTCAP_RRS_SV / PCI_BASE_ADDRESS_MEM_MASK /
 * PCI_VENDOR_ID_AMCC 같은 표준 상수의 숫자 값은 include/ 아래에 있어 볼 수 없다.
 * 셋째, X-Gene 컨트롤러의 레지스터 사양서는 트리에 없다 — 아래 레지스터 설명은
 * 전부 이 파일 안의 사용처에서 읽어 낼 수 있는 범위로만 적었다.
 * 넷째, 아래 include 주석에서 "직접 쓰는 이름을 찾지 못했다" 고 적은 헤더들도,
 * 다른 헤더를 끌어오기 위해 필요할 수 있다 — 그 포함 관계는 include/ 가 없어
 * 확인 못 함이므로 불필요하다고 단정하지 않는다.
 */

/* [한국어] clk_get() 과 clk_prepare_enable() 을 위해 포함한다. 이 컨트롤러는
 * SoC 가 공급하는 클럭이 켜져야 동작한다. */
#include <linux/clk.h>
/* [한국어] 지연 함수(msleep/udelay 계열)를 위한 헤더지만, 이 파일에서 그 이름을
 * 직접 쓰는 곳을 찾지 못했다. */
#include <linux/delay.h>
/* [한국어] readl()/writel() 을 위해 포함한다. 이 파일의 레지스터 접근은 전부
 * relaxed 가 아닌 보통 판이라 접근마다 배리어가 들어간다 — RTDID 를 써 두고
 * 창을 읽는 순서가 지켜져야 하는 하드웨어라 그 편이 안전하다. */
#include <linux/io.h>
/* [한국어] jiffies 관련 헤더지만, 이 파일에서 그 이름을 직접 쓰는 곳을 찾지 못했다. */
#include <linux/jiffies.h>
/* [한국어] 부팅 초기 메모리 할당자 헤더지만, 이 파일에서 memblock 계열 이름을
 * 직접 쓰는 곳을 찾지 못했다. */
#include <linux/memblock.h>
/* [한국어] __init 계열 표시를 위한 헤더지만, 이 파일에는 그 표시가 붙은 심볼이 없다. */
#include <linux/init.h>
/* [한국어] irq_find_matching_host() 와 irq_domain_is_msi_parent() 를 위해
 * 포함한다. MSI 드라이버가 도메인을 이미 등록했는지 확인하는 데 쓴다. */
#include <linux/irqdomain.h>
/* [한국어] of_find_compatible_node(), of_node_get()/of_node_put() 을 위해
 * 포함한다. DT 경로의 노드 참조 관리와 MSI 노드 탐색에 쓴다. */
#include <linux/of.h>
/* [한국어] OF 주소 변환 헬퍼 헤더지만, 이 파일에서 그 이름을 직접 쓰는 곳을
 * 찾지 못했다. */
#include <linux/of_address.h>
/* [한국어] struct of_pci_range 와 of_pci_dma_range_parser_init(),
 * for_each_of_pci_range 를 위해 포함한다. 인바운드 창을 DT 의 dma-ranges 에서
 * 읽어 오는 데 쓴다. */
#include <linux/of_pci.h>
/* [한국어] PCI 코어 API 전반 — struct pci_bus/pci_ops, PCI_SLOT/PCI_FUNC,
 * pci_generic_config_read32() 등. */
#include <linux/pci.h>
/* [한국어] acpi_disabled 전역과 to_acpi_device(), acpi_dev_get_resources() 를
 * 위해 포함한다. 이 파일이 DT 와 ACPI 두 경로를 함께 담기 때문에 필요하다. */
#include <linux/pci-acpi.h>
/* [한국어] struct pci_ecam_ops 와 struct pci_config_window 를 위해 포함한다.
 * ACPI 경로에서 이 컨트롤러가 ECAM 프레임워크 위에 얹히기 때문이다. */
#include <linux/pci-ecam.h>
/* [한국어] platform_get_resource_byname() 과 builtin_platform_driver() 를
 * 위해 포함한다. DT 경로에서 이 컨트롤러는 플랫폼 디바이스다. */
#include <linux/platform_device.h>
/* [한국어] 슬랩 할당자 헤더지만, 이 파일에서 kmalloc/kfree 계열 이름을 직접
 * 쓰는 곳을 찾지 못했다(할당은 devm_kzalloc 하나뿐이다). */
#include <linux/slab.h>

/* [한국어] drivers/pci 내부 전용 헤더. 꺾쇠가 아니라 상대 경로 따옴표인 것은
 * 커널 전역 헤더가 아니기 때문이다. 이 파일이 여기서 무엇을 쓰는지는 코드만으로
 * 특정되지 않는다 — 쓰이는 이름 대부분이 <linux/pci.h> 에도 있어 어느 쪽에서
 * 온 선언인지 이 트리만으로는 갈라낼 수 없다. */
#include "../pci.h"

/* [한국어] 아래 오프셋 뭉치는 이 컨트롤러의 CSR(Control and Status Register) 창
 * 기준 위치다. 이 파일의 xgene_pcie_readl()/xgene_pcie_writel() 이 언제나
 * port->csr_base 에 더해 접근하므로, 여기 나오는 값은 모두 그 창 안의 오프셋으로
 * 읽으면 된다. 예외는 xgene_pcie_setup_ib_reg() 의 인바운드 창 0 처리인데,
 * 거기서는 CSR 이 아니라 설정공간 창(cfg_base)에 직접 BAR 를 쓴다.
 * 레지스터 사양서가 트리에 없으므로, 아래 설명은 전부 이 파일 안의 사용처에서
 * 읽어 낼 수 있는 범위로만 적었다.
 *
 * PCIECORE_CTLANDSTATUS — 코어의 제어/상태 레지스터. 링크가 올라왔는지와
 * 협상된 링크 속도(PHY rate)를 여기서 읽는다. */
#define PCIECORE_CTLANDSTATUS		0x50
/* [한국어] 인바운드 창 1 의 목적지 주소(하위 워드). PIM 은 PCI 쪽 주소를 로컬
 * 버스(DDR) 주소로 바꿀 때 그 결과가 놓일 위치를 담는다. 창 하나가 네 개의
 * 32비트 워드를 쓰며(주소 하/상, 크기 하/상), 그 배치는
 * xgene_pcie_setup_pims() 가 쓰는 오프셋 0x00/0x04/0x10/0x14 에서 드러난다. */
#define PIM1_1L				0x80
/* [한국어] 인바운드 창 2 의 BAR — 이 창이 잡을 PCI 주소 범위의 시작을 담는다. */
#define IBAR2				0x98
/* [한국어] 인바운드 창 2 의 마스크 — 창 크기를 나타낸다. 크기 s 에 대해
 * ~(s-1) 형태로, BAR 의 크기 결정 방식과 같은 관용이다. */
#define IR2MSK				0x9c
/* [한국어] 인바운드 창 2 의 목적지 주소(하위 워드). */
#define PIM2_1L				0xa0
/* [한국어] 인바운드 창 3 의 BAR 하위 워드. 이 창만 상위 워드(+0x4)까지 써서
 * 64비트 주소를 담는다 — 창 2 가 32비트만 다루는 것과 대비된다. */
#define IBAR3L				0xb4
/* [한국어] 인바운드 창 3 의 마스크 하위 워드. 역시 +0x4 에 상위 워드가 이어진다. */
#define IR3MSKL				0xbc
/* [한국어] 인바운드 창 3 의 목적지 주소(하위 워드). */
#define PIM3_1L				0xc4
/* [한국어] 아웃바운드 창 1 의 시작 레지스터. 이 드라이버는 비prefetchable
 * 메모리 창을 여기에 새긴다. 창 하나가 여섯 개의 32비트 워드를 연달아 쓰며
 * (CPU 주소 하/상, 마스크 하/상, PCI 주소 하/상), 그 배치는
 * xgene_pcie_setup_ob_reg() 가 쓰는 오프셋 0x00~0x14 에서 드러난다. */
#define OMR1BARL			0x100
/* [한국어] 아웃바운드 창 2 의 시작 레지스터. prefetchable 메모리 창을 새긴다.
 * 앞 창과 0x18(= 6워드)만큼 떨어져 있어 배치 해석과 들어맞는다. */
#define OMR2BARL			0x118
/* [한국어] 아웃바운드 창 3 의 시작 레지스터. IO 창을 새긴다. 역시 0x18 간격이다. */
#define OMR3BARL			0x130
/* [한국어] 설정공간 창의 물리 주소(하위 32비트)를 컨트롤러에 알려 주는 레지스터. */
#define CFGBARL				0x154
/* [한국어] 같은 주소의 상위 32비트. */
#define CFGBARH				0x158
/* [한국어] 설정공간 창을 활성화하는 제어 레지스터. 여기에 EN_REG 를 써야
 * 그 창을 통한 설정공간 접근이 동작한다. */
#define CFGCTL				0x15c
/* [한국어] 설정 요청에 실릴 버스/장치/함수 번호를 담는 레지스터. 이 하드웨어에는
 * ECAM 처럼 주소에 번호가 실리는 방식이 없어서, 접근 직전에 이 레지스터를
 * 갱신해야 한다(옆의 상류 주석이 그 사실을 밝힌다). 그래서 설정공간 접근이
 * '레지스터 쓰기 + 창 접근' 의 두 단계가 되고, 그 두 단계가 원자적이어야 한다. */
#define RTDID				0x160
/* [한국어] 루트 브리지 자신의 설정공간을 CSR 창 쪽에서 들여다보는 창의 시작.
 * 이 드라이버는 여기에 벤더 ID 와 디바이스 ID 를 써 넣는다. */
#define BRIDGE_CFG_0			0x2000
/* [한국어] 그 창의 0x10 만큼 뒤 — 인바운드 창 0 의 마스크를 쓰는 자리로 쓰인다
 * (xgene_pcie_set_ib_mask 의 유일한 호출 대상이다). */
#define BRIDGE_CFG_4			0x2010
/* [한국어] 브리지 상태 레지스터. 협상된 링크 폭(레인 수)을 상위 비트에서 읽는다. */
#define BRIDGE_STATUS_0			0x2600

/* [한국어] PCIECORE_CTLANDSTATUS 의 비트 8 — 링크가 올라왔음을 나타낸다. */
#define LINK_UP_MASK			0x00000100
/* [한국어] 설정공간 창 주소의 비트 16. 옆에 붙은 상류 주석대로, 주소 비트 [17:16]
 * 이 2'b01 이면 컨트롤러가 그 접근을 type 1 설정 사이클로 만들어 하위 버스의
 * 장치로 전달한다. 즉 루트 브리지 자신을 볼 때와 그 아래 장치를 볼 때가
 * 같은 창의 서로 다른 절반으로 갈린다. */
#define AXI_EP_CFG_ACCESS		0x10000
/* [한국어] 인바운드 창의 목적지 상위 워드에 함께 세우는 비트들(상위 4비트).
 * 이름이 말하듯 캐시 일관성(coherency)을 켜는 설정으로 보이며, 인바운드 DMA 가
 * CPU 캐시와 일관되게 보이도록 만드는 것으로 읽힌다. 정확한 비트별 의미는
 * 레지스터 사양서가 필요해 이 트리에서 확인 못 함. */
#define EN_COHERENCY			0xF0000000
/* [한국어] 창 마스크 워드의 비트 0 — 그 창을 활성화한다. 마스크와 활성화 비트가
 * 한 워드에 겹쳐 있으므로, 마스크가 0 이면 활성화 비트도 0 이 되어 창이 꺼진다.
 * 아웃바운드 창 설정에서 크기가 최소치에 못 미치면 그런 일이 실제로 일어난다. */
#define EN_REG				0x00000001
/* [한국어] 아웃바운드 창 마스크의 비트 1 — 그 창이 메모리가 아니라 IO 공간용임을
 * 나타낸다. IO 창에만 얹는다. */
#define OB_LO_IO			0x00000002
/* [한국어] 루트 브리지가 스스로 보고할 디바이스 ID. 벤더 ID 와 합쳐
 * BRIDGE_CFG_0 에 한 번에 써 넣는다. 펌웨어가 남긴 값을 덮어써서 커널이
 * 이 브리지를 제대로 알아보게 하려는 것이다. */
#define XGENE_PCIE_DEVICEID		0xE004
/* [한국어] PCIECORE_CTLANDSTATUS 에서 협상된 PHY 링크 속도를 뽑는 매크로.
 * 마스크 0xc000 은 비트 15..14 이고, 시프트 0xe 는 십진수 14 를 16진수로 적은
 * 것이다 — 즉 두 비트를 0 부터 시작하는 값으로 내린다. 호출자가 여기에 1 을
 * 더해 'gen-N' 으로 출력하므로, 0 이 gen1 에 대응한다. */
#define PIPE_PHY_RATE_RD(src)		((0xc000 & (u32)(src)) >> 0xe)

/* [한국어] v1 실리콘에서 PCI Express capability 구조체가 설정공간의 어디에
 * 놓이는지를 고정 값으로 못박은 것이다. 보통은 capability 리스트를 따라가
 * 찾지만, 아래 RRS 우회 코드는 읽기 경로 한복판에서 오프셋을 비교해야 하므로
 * 리스트 순회를 할 수 없어 이렇게 상수로 둔다. */
#define XGENE_V1_PCI_EXP_CAP		0x40

/* PCIe IP version */
/* [한국어] v1 실리콘(옆의 상류 주석이 말하는 IP 버전). RRS 버그 우회가 이
 * 버전에서만 적용된다. */
#define XGENE_PCIE_IP_VER_1		1
/* [한국어] v2 실리콘. 이 값은 ACPI 경로의 xgene_v2_pcie_ecam_init() 에서만
 * 설정된다 — DT 경로는 언제나 v1 로 고정한다(xgene_pcie_probe 참조). */
#define XGENE_PCIE_IP_VER_2		2

/* [한국어]
 * struct xgene_pcie - 이 컨트롤러 하나(= 루트 컴플렉스 하나)의 상태
 *
 * 할당되는 곳이 경로에 따라 다르다는 점이 이 구조체의 특징이다. DT 경로에서는
 * devm_pci_alloc_host_bridge(dev, sizeof(*port)) 로 호스트 브리지 뒤에 함께
 * 잡히므로 pci_host_bridge_from_priv() 로 브리지를 되찾을 수 있다. ACPI
 * 경로에서는 devm_kzalloc() 으로 따로 잡혀 pci_config_window 의 priv 에 걸린다
 * — 그쪽에서는 브리지로 거슬러 올라갈 수 없고, 실제로 그럴 필요도 없다
 * (창 설정을 하지 않으므로 bridge->windows 를 볼 일이 없다).
 * 그 두 경로를 흡수하는 것이 pcie_bus_to_port() 다.
 */
struct xgene_pcie {
	/* [한국어] 이 컨트롤러의 DT 노드.
	 * 설정자: xgene_pcie_probe() 이 of_node_get() 으로 참조를 하나 올려 담는다.
	 *   ACPI 경로에서는 채워지지 않아 NULL 로 남는다.
	 * 읽는 자: xgene_pcie_parse_map_dma_ranges() 가 dma-ranges 를 읽을 때만.
	 * 값 범위: 유효한 device_node 포인터 또는 NULL(ACPI 경로).
	 * 동기화: 설정 후 읽기 전용.
	 * [상류 코드 관찰] of_node_get() 으로 올린 참조를 놓아 주는 of_node_put() 이
	 *   이 포인터에 대해서는 없다. 이 드라이버가 언바인드를 막아 두어
	 *   해제 경로 자체가 없는 것과 맞물린다. */
	struct device_node	*node;
	/* [한국어] 로그와 devm 할당의 기준 device.
	 * 설정자: DT 경로는 xgene_pcie_probe(), ACPI 경로는 이 필드를 채우지 않는다.
	 * 읽는 자: 창 설정 함수들의 dev_dbg/dev_warn/dev_err.
	 * 값 범위: 유효 포인터 또는 NULL(ACPI 경로).
	 * 동기화: 설정 후 읽기 전용.
	 * [상류 코드 관찰] ACPI 경로에서 NULL 로 남지만, 그쪽 경로에서 이 필드를 읽는
	 *   함수(창 설정 계열)가 불리지 않으므로 실제로 문제가 되지 않는다. */
	struct device		*dev;
	/* [한국어] 컨트롤러 동작에 필요한 클럭.
	 * 설정자: xgene_pcie_init_port() 의 clk_get(). devm 판이 아니다.
	 * 읽는 자: 같은 함수의 clk_prepare_enable() 이 유일하다.
	 * 값 범위: 유효한 clk 포인터.
	 * 동기화: 클럭 프레임워크가 내부적으로 처리한다.
	 * [상류 코드 관찰] clk_get() 의 짝인 clk_put() 도, clk_prepare_enable() 의 짝인
	 *   clk_disable_unprepare() 도 이 파일 어디에도 없다. */
	struct clk		*clk;
	/* [한국어] CSR 레지스터 창의 커널 가상 주소. 이 파일의 거의 모든 레지스터
	 * 접근이 이 주소를 기준으로 삼는다.
	 * 설정자: DT 경로는 xgene_pcie_map_reg() 이 "csr" 이름 자원에서, ACPI 경로는
	 *   xgene_pcie_ecam_init() 이 ACPI 의 _CRS 첫 메모리 자원에서 매핑한다.
	 * 읽는 자: xgene_pcie_readl()/xgene_pcie_writel().
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: probe 경로는 단독 실행이고, 설정공간 경로의 RTDID 접근은 코어의
	 *   pci_lock 이 직렬화한다. */
	void __iomem		*csr_base;
	/* [한국어] 설정공간 접근에 쓰는 창의 커널 가상 주소.
	 * 설정자: DT 경로는 xgene_pcie_map_reg() 이 "cfg" 이름 자원을 매핑하고,
	 *   ACPI 경로는 ECAM 프레임워크가 이미 만들어 둔 cfg->win 을 그대로 받는다.
	 * 읽는 자: xgene_pcie_get_cfg_base() 와, 인바운드 창 0 을 설정할 때의 BAR 쓰기.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 설정공간 접근은 pci_lock 아래에서만 일어난다. */
	void __iomem		*cfg_base;
	/* [한국어] 같은 설정공간 창의 물리 주소.
	 * 설정자: xgene_pcie_map_reg() 만 채운다 — ACPI 경로에서는 0 으로 남는다.
	 * 읽는 자: xgene_pcie_setup_cfg_reg() 가 컨트롤러에게 창 위치를 알려 줄 때.
	 * 값 범위: 물리 주소 또는 0.
	 * 동기화: 설정 후 읽기 전용.
	 * 가상 주소와 물리 주소를 둘 다 두는 이유: 접근에는 가상 주소가, 컨트롤러
	 *   레지스터에 새길 때는 물리 주소가 필요하기 때문이다. */
	unsigned long		cfg_addr;
	/* [한국어] 링크가 올라왔는지.
	 * 설정자: xgene_pcie_linkup() 이 매번 false 로 초기화한 뒤 상태 비트를 보고 정한다.
	 * 읽는 자: xgene_pcie_setup() 의 부팅 로그 분기 하나뿐이다.
	 * 값 범위: true/false.
	 * 동기화: probe 경로에서 한 번만 다뤄진다. */
	bool			link_up;
	/* [한국어] 이 컨트롤러의 IP 버전.
	 * 설정자: DT 경로는 xgene_pcie_probe() 이 항상 v1 로 고정하고, ACPI 경로는
	 *   ops->init 후크가 v1 또는 v2 로 정한다.
	 * 읽는 자: xgene_pcie_config_read32() 의 RRS 우회 조건 하나뿐이다.
	 * 값 범위: XGENE_PCIE_IP_VER_1 또는 XGENE_PCIE_IP_VER_2.
	 * 동기화: 설정 후 읽기 전용.
	 * [상류 코드 관찰] DT 경로가 v1 로 못박혀 있으므로, DT 로 부팅하는 v2 하드웨어가
	 *   있다면 필요 없는 우회가 적용된다. 그런 조합이 실제로 존재하는지는
	 *   이 트리에서 확인 못 함. */
	u32			version;
};

/* [한국어]
 * xgene_pcie_readl - CSR 창의 레지스터 하나를 32비트로 읽는다
 *
 * @port: 컨트롤러 객체.
 * @reg: CSR 창 기준 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 이 파일의 모든 CSR 읽기가 이 한 줄을 지난다. 베이스 주소를 더하는 일을
 * 한곳에 모아 두면, 호출부에서는 오프셋 상수만 보이게 되어 어떤 레지스터를
 * 만지는지가 눈에 잘 들어온다.
 * relaxed 판이 아닌 보통 readl 이라 접근마다 배리어가 들어간다 — 순서가
 * 중요한 하드웨어라 그 편이 안전하다.
 * 실행 컨텍스트: probe 경로와 설정공간 접근 경로 양쪽. 잠들지 않는다.
 * 에러 경로: 없다 — MMIO 읽기는 실패를 알려 주지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_set_ib_mask() / xgene_pcie_linkup() / xgene_pcie_set_rtdid_reg()
 *     → [xgene_pcie_readl] → readl()
 */
static u32 xgene_pcie_readl(struct xgene_pcie *port, u32 reg)
{
	/* [한국어] CSR 베이스에 오프셋을 더해 읽는다. */
	return readl(port->csr_base + reg);
}

/* [한국어]
 * xgene_pcie_writel - CSR 창의 레지스터 하나에 32비트를 쓴다
 *
 * @port: 컨트롤러 객체.
 * @reg: CSR 창 기준 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 읽기 쪽의 짝이며 존재 이유가 같다. 인자 순서가 writel(val, addr) 과 반대로
 * (reg, val) 인 점에 주의 — 이 파일 안에서는 일관되지만 표준 writel 과는
 * 순서가 뒤바뀌어 있다.
 * 실행 컨텍스트: probe 경로와 설정공간 접근 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   창 설정 함수들과 xgene_pcie_set_rtdid_reg() → [xgene_pcie_writel] → writel()
 */
static void xgene_pcie_writel(struct xgene_pcie *port, u32 reg, u32 val)
{
	/* [한국어] CSR 베이스에 오프셋을 더해 쓴다. 표준 writel 의 인자 순서에 맞춰
	 * 값이 먼저, 주소가 나중이다. */
	writel(val, port->csr_base + reg);
}

/* [한국어]
 * pcie_bar_low_val - BAR 하위 워드에 넣을 값을 만든다 (주소 + 타입 플래그)
 *
 * @addr: 이 BAR 가 가리킬 주소의 하위 32비트.
 * @flags: BAR 하위 4비트에 들어갈 타입/prefetch 표시.
 * @return: 둘을 합친 BAR 값.
 *
 * PCI 규격에서 메모리 BAR 의 하위 4비트는 주소가 아니라 종류 표시다
 * (비트 0 = IO 여부, 비트 2:1 = 32/64비트, 비트 3 = prefetchable).
 * 그래서 주소에서 그 4비트를 지우고 플래그를 대신 얹어야 한다.
 * PCI_BASE_ADDRESS_MEM_MASK 가 바로 그 하위 4비트를 지우는 마스크다 —
 * 실제 숫자 값은 include/uapi/linux/pci_regs.h 에 있고 이 트리에는 없다.
 * 실행 컨텍스트: probe 경로의 순수 계산. 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_setup_ib_reg() → [pcie_bar_low_val]
 */
static inline u32 pcie_bar_low_val(u32 addr, u32 flags)
{
	/* [한국어] 주소에서 하위 4비트(타입 필드)를 지우고 플래그를 얹는다. */
	return (addr & PCI_BASE_ADDRESS_MEM_MASK) | flags;
}

/* [한국어]
 * pcie_bus_to_port - PCI 버스에서 이 드라이버의 컨트롤러 객체를 되찾는다
 *
 * @bus: 접근 대상 PCI 버스.
 * @return: 그 버스를 관장하는 xgene_pcie 객체.
 *
 * 이 한 함수가 DT 경로와 ACPI 경로를 갈라 흡수한다. 두 경로에서 bus->sysdata 에
 * 걸리는 것이 서로 다르기 때문이다 — DT 경로는 xgene_pcie_probe() 이
 * bridge->sysdata = port 로 이 객체를 직접 걸고, ACPI 경로는 ECAM 프레임워크가
 * pci_config_window 를 걸어 두므로 그 안의 priv 를 한 번 더 거쳐야 한다.
 * 그 판별을 acpi_disabled 전역으로 한다 — ACPI 로 부팅하지 않았으면 참이다.
 * 덕분에 아래 설정공간 접근 코드는 두 경로를 구분하지 않아도 된다.
 * 실행 컨텍스트: 설정공간 접근 경로(pci_lock 아래). 잠들지 않는다.
 * 에러 경로: 없다 — sysdata 가 올바르게 걸려 있음을 전제한다.
 *
 * 호출 체인:
 *   xgene_pcie_get_cfg_base() / xgene_pcie_set_rtdid_reg() /
 *   xgene_pcie_config_read32() → [pcie_bus_to_port]
 */
static inline struct xgene_pcie *pcie_bus_to_port(struct pci_bus *bus)
{
	/* [한국어] ACPI 경로에서 sysdata 가 가리키는 ECAM 창 객체. */
	struct pci_config_window *cfg;

	/* [한국어] ACPI 로 부팅하지 않았다면 DT 경로다. */
	if (acpi_disabled)
		/* [한국어] 그때는 sysdata 가 곧 이 객체다. */
		return (struct xgene_pcie *)(bus->sysdata);

	/* [한국어] ACPI 경로에서는 sysdata 가 ECAM 창이므로 */
	cfg = bus->sysdata;
	/* [한국어] 그 창의 priv 에 걸어 둔 객체를 꺼낸다.
	 * xgene_pcie_ecam_init() 이 cfg->priv = port 로 심어 둔 값이다. */
	return (struct xgene_pcie *)(cfg->priv);
}

/*
 * When the address bit [17:16] is 2'b01, the Configuration access will be
 * treated as Type 1 and it will be forwarded to external PCIe device.
 */
/* [한국어]
 * xgene_pcie_get_cfg_base - 이 버스의 설정공간 접근에 쓸 창 주소를 고른다
 *
 * @bus: 접근 대상 버스.
 * @return: 설정공간 창의 시작 가상 주소(루트용 또는 하위 버스용).
 *
 * 위 상류 주석이 말하듯 이 컨트롤러는 창 주소의 비트 [17:16] 으로 설정 사이클의
 * 종류를 가린다. 그래서 같은 창을 두 구역으로 나눠 쓴다 — 앞쪽은 루트 브리지
 * 자신을 보는 type 0 접근, AXI_EP_CFG_ACCESS 만큼 떨어진 뒤쪽은 하위 버스의
 * 장치를 보는 type 1 접근이다.
 * 판정은 'bus->number 가 부모 버스 번호보다 큰가' 로 한다. 루트 버스에서는
 * number 와 primary 가 같아 거짓이 되고, 그 아래 버스에서는 참이 된다.
 * 실행 컨텍스트: 설정공간 접근 경로(pci_lock 아래). 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_map_bus() → [xgene_pcie_get_cfg_base] → pcie_bus_to_port()
 */
static void __iomem *xgene_pcie_get_cfg_base(struct pci_bus *bus)
{
	/* [한국어] 창 주소를 가진 컨트롤러 객체를 되찾는다. */
	struct xgene_pcie *port = pcie_bus_to_port(bus);

	/* [한국어] 부모 버스보다 아래에 있는 버스인가 — 즉 루트 브리지 자신이 아닌가. */
	if (bus->number >= (bus->primary + 1))
		/* [한국어] 그렇다면 type 1 사이클이 되는 뒤쪽 구역을 쓴다. */
		return port->cfg_base + AXI_EP_CFG_ACCESS;

	/* [한국어] 루트 버스라면 앞쪽 구역(type 0)을 그대로 쓴다. */
	return port->cfg_base;
}

/*
 * For Configuration request, RTDID register is used as Bus Number,
 * Device Number and Function number of the header fields.
 */
/* [한국어]
 * xgene_pcie_set_rtdid_reg - 다음 설정 요청에 실릴 버스/장치/함수 번호를 새긴다
 *
 * @bus: 접근 대상 버스.
 * @devfn: 장치·함수 번호.
 * @return: 없음.
 *
 * 위 상류 주석이 밝히듯, 이 컨트롤러는 설정 요청 헤더의 버스/장치/함수 필드를
 * 주소에서 뽑지 않고 RTDID 레지스터에서 가져온다. 그래서 설정공간 접근은
 * 반드시 두 단계다 — 먼저 여기에 번호를 써 두고, 그 다음 창을 읽거나 쓴다.
 * 이 두 단계 사이에 다른 장치를 향한 접근이 끼어들면 엉뚱한 장치를 건드리게
 * 되는데, PCI 코어가 설정공간 접근 전체를 pci_lock 으로 감싸기 때문에 그런 일이
 * 생기지 않는다.
 * 루트 버스일 때 0 을 쓰는 것은, 그 경우 대상이 컨트롤러 자신이라 번호를 실을
 * 필요가 없기 때문이다.
 * 실행 컨텍스트: 설정공간 접근 경로(pci_lock 아래). 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_map_bus() → [xgene_pcie_set_rtdid_reg]
 *     → xgene_pcie_writel(), xgene_pcie_readl()
 */
static void xgene_pcie_set_rtdid_reg(struct pci_bus *bus, uint devfn)
{
	/* [한국어] RTDID 레지스터를 가진 컨트롤러 객체를 되찾는다. */
	struct xgene_pcie *port = pcie_bus_to_port(bus);
	/* [한국어] 버스·장치·함수 번호를 담을 지역 변수. */
	unsigned int b, d, f;
	/* [한국어] 루트 버스일 때 그대로 쓰일 기본값 0 으로 초기화한다. */
	u32 rtdid_val = 0;

	/* [한국어] 버스 번호. */
	b = bus->number;
	/* [한국어] devfn 의 상위 5비트가 장치 번호다. */
	d = PCI_SLOT(devfn);
	/* [한국어] 하위 3비트가 함수 번호다. */
	f = PCI_FUNC(devfn);

	/* [한국어] 루트 버스가 아닐 때만 번호를 싣는다 — 루트 버스 대상은 컨트롤러
	 * 자신이라 번호가 필요 없다. */
	if (!pci_is_root_bus(bus))
		/* [한국어] 설정 요청 헤더의 배치와 같은 모양으로 조립한다: 버스는 비트 15..8,
		 * 장치는 7..3, 함수는 2..0. */
		rtdid_val = (b << 8) | (d << 3) | f;

	/* [한국어] 레지스터에 새긴다. 이 쓰기가 끝나야 뒤이은 창 접근이 올바른 장치로 간다. */
	xgene_pcie_writel(port, RTDID, rtdid_val);
	/* read the register back to ensure flush */
	/* [한국어] 옆의 상류 주석대로 되읽어 쓰기가 하드웨어에 도달했음을 보장한다.
	 * 쓰기가 버퍼에 머문 채 창 접근이 먼저 나가면 이전 장치를 건드리게 되므로,
	 * 이 되읽기가 두 단계의 순서를 강제하는 장치다. 반환값은 쓰지 않는다. */
	xgene_pcie_readl(port, RTDID);
}

/*
 * X-Gene PCIe port uses BAR0-BAR1 of RC's configuration space as
 * the translation from PCI bus to native BUS.  Entire DDR region
 * is mapped into PCIe space using these registers, so it can be
 * reached by DMA from EP devices.  The BAR0/1 of bridge should be
 * hidden during enumeration to avoid the sizing and resource allocation
 * by PCIe core.
 */
/* [한국어]
 * xgene_pcie_hide_rc_bars - 루트 브리지의 BAR0/BAR1 접근을 숨겨야 하는지 판정한다
 *
 * @bus: 접근 대상 버스.
 * @offset: 설정공간 오프셋.
 * @return: 숨겨야 하면 참.
 *
 * 위 상류 주석이 이유를 설명한다. 이 하드웨어는 루트 브리지 설정공간의
 * BAR0/BAR1 을 일반적인 BAR 가 아니라 '인바운드 주소 변환 창' 으로 쓴다.
 * DDR 영역 전체가 그 창을 통해 PCIe 공간에 노출되고, 그래야 엔드포인트의 DMA 가
 * 시스템 메모리에 닿는다. 그런데 PCI 코어는 BAR 를 보면 크기를 재고 주소를
 * 다시 배정하려 든다 — 그러면 애써 새긴 변환 창이 깨진다. 그래서 열거 과정에서
 * 이 두 BAR 를 아예 없는 것처럼 가린다.
 * 실행 컨텍스트: 설정공간 접근 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_map_bus() → [xgene_pcie_hide_rc_bars]
 */
static bool xgene_pcie_hide_rc_bars(struct pci_bus *bus, int offset)
{
	/* [한국어] 루트 버스이면서 오프셋이 BAR0 또는 BAR1 인 접근만 가린다.
	 * 하위 버스의 장치 BAR 는 정상적으로 보여야 하므로 조건에 루트 버스 검사가 붙는다. */
	if (pci_is_root_bus(bus) && ((offset == PCI_BASE_ADDRESS_0) ||
				     (offset == PCI_BASE_ADDRESS_1)))
		/* [한국어] 가려야 한다고 알린다. 호출자가 NULL 을 돌려 접근 자체를 실패시킨다. */
		return true;

	/* [한국어] 그 밖의 접근은 그대로 통과시킨다. */
	return false;
}

/* [한국어]
 * xgene_pcie_map_bus - 설정공간 접근의 관문. 걸러 내고, 번호를 새기고, 주소를 준다
 *
 * @bus: 접근 대상 버스.
 * @devfn: 장치·함수 번호.
 * @offset: 설정공간 오프셋.
 * @return: 접근할 가상 주소, 또는 NULL(접근을 막아야 하는 경우).
 *
 * 이 컨트롤러의 모든 설정공간 접근이 여기를 지난다. 하는 일이 셋이다.
 * (1) 존재하지 않는 대상을 걸러 낸다 — 루트 버스에는 컨트롤러 자신(devfn 0)
 * 하나뿐이므로 다른 devfn 은 없는 장치다. 루트 버스 접근에서는 RTDID 가 0 이라
 * 어느 devfn 으로 접근하든 같은 설정공간이 보이므로, 이 검사가 없으면 열거가
 * 여러 devfn 자리에서 같은 장치를 반복해 발견하게 된다.
 * (2) 인바운드 창으로 쓰이는 루트 BAR 를 가린다.
 * (3) 통과한 접근에 대해 RTDID 를 갱신하고 창 주소를 돌려준다.
 * NULL 을 돌려주면 호출자인 표준 접근 함수가 PCIBIOS_DEVICE_NOT_FOUND 를
 * 반환한다(drivers/pci/access.c 의 pci_generic_config_read32 참조).
 * 실행 컨텍스트: pci_lock 을 쥔 상태. 잠들 수 없고, 그 락이 RTDID 갱신과
 * 뒤이은 창 접근을 원자적으로 묶어 준다.
 *
 * 호출 체인:
 *   pci_generic_config_read32() / pci_generic_config_write32() /
 *   pci_generic_config_write() → [xgene_pcie_map_bus]
 *     → xgene_pcie_hide_rc_bars(), xgene_pcie_set_rtdid_reg(),
 *       xgene_pcie_get_cfg_base()
 */
static void __iomem *xgene_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
					int offset)
{
	/* [한국어] 루트 버스의 0 이 아닌 devfn 은 존재하지 않는 장치이고, */
	if ((pci_is_root_bus(bus) && devfn != 0) ||
	    /* [한국어] 루트 BAR 접근은 가려야 하는 대상이다. 둘 중 하나라도 걸리면 */
	    xgene_pcie_hide_rc_bars(bus, offset))
		/* [한국어] NULL 을 돌려 접근을 없는 장치로 만든다. */
		return NULL;

	/* [한국어] 통과한 접근에 대해 대상 번호를 레지스터에 새긴다. 반드시 아래 주소를
	 * 쓰기 전에 해야 한다. */
	xgene_pcie_set_rtdid_reg(bus, devfn);
	/* [한국어] 사이클 종류에 맞는 창 구역을 고르고 오프셋을 더해 돌려준다. */
	return xgene_pcie_get_cfg_base(bus) + offset;
}

/* [한국어]
 * xgene_pcie_config_read32 - 설정공간을 읽되 v1 실리콘의 RRS 버그를 우회한다
 *
 * @bus: 대상 버스.
 * @devfn: 장치·함수 번호.
 * @where: 설정공간 오프셋.
 * @size: 읽을 바이트 수(1/2/4).
 * @val: 읽은 값을 담을 곳.
 * @return: PCIBIOS_ 계열 상태값.
 *
 * 표준 읽기 함수를 그대로 쓰지 못하는 이유는 아래 상류 주석이 설명하는 v1
 * 실리콘의 버그 때문이다. 요약하면, RRS(Configuration Request Retry Status)
 * 소프트웨어 가시성이 켜져 있을 때 없는 장치의 벤더/디바이스 ID 를 읽으면
 * 컨트롤러가 "장치는 있는데 아직 준비되지 않았다" 는 응답을 지어내고, PCI 코어는
 * 그것을 믿고 시간이 다 될 때까지 재시도한다. 그래서 이 드라이버는 아예
 * "우리는 RRS 가시성을 지원하지 않는다" 고 보고하도록 그 능력 비트를 지운다.
 *
 * 구현상 눈여겨볼 점은 표준 함수를 부를 때 오프셋을 4의 배수로 내리고 크기를
 * 4 로 강제한다는 것이다. 그래야 능력 비트가 있는 32비트 워드 전체를 손에 쥐고
 * 고칠 수 있기 때문이다. 그 대가로 1/2바이트 요청에 대한 자리 맞추기를 표준
 * 함수 대신 이 함수가 직접 해야 하고, 그것이 마지막의 시프트·마스크 두 줄이다
 * — 표준 구현(drivers/pci/access.c)과 같은 계산이 여기에 한 벌 더 있는 셈이다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 상태. 잠들지 않는다.
 * 에러 경로: 표준 함수가 실패하면(없는 장치 등) 그 상태값을 그대로 전달한다.
 *
 * 호출 체인:
 *   (PCI 코어의 설정공간 읽기) → [xgene_pcie_config_read32]
 *     → pci_generic_config_read32() → xgene_pcie_map_bus()
 */
static int xgene_pcie_config_read32(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 *val)
{
	/* [한국어] IP 버전을 알아야 우회 여부를 정할 수 있으므로 컨트롤러 객체를 되찾는다. */
	struct xgene_pcie *port = pcie_bus_to_port(bus);
	/* [한국어] 표준 읽기의 결과. */
	int ret;

	/* [한국어] 오프셋을 4의 배수로 내리고 크기를 4 로 못박아 32비트 워드를 통째로
	 * 읽는다. 아래에서 그 워드의 특정 비트를 고쳐야 하기 때문이다.
	 * 크기를 4 로 주었으므로 표준 함수 안의 자리 맞추기는 건너뛰어진다. */
	ret = pci_generic_config_read32(bus, devfn, where & ~0x3, 4, val);
	/* [한국어] 없는 장치이거나 접근이 막힌 경우 — 상태값을 그대로 올려 보낸다. */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	/*
	 * The v1 controller has a bug in its Configuration Request Retry
	 * Status (RRS) logic: when RRS Software Visibility is enabled and
	 * we read the Vendor and Device ID of a non-existent device, the
	 * controller fabricates return data of 0xFFFF0001 ("device exists
	 * but is not ready") instead of 0xFFFFFFFF (PCI_ERROR_RESPONSE)
	 * ("device does not exist").  This causes the PCI core to retry
	 * the read until it times out.  Avoid this by not claiming to
	 * support RRS SV.
	 */
	/* [한국어] 위 상류 주석이 설명한 우회의 적용 조건이다. 셋을 모두 만족할 때만
	 * 손댄다 — 루트 버스이고, v1 실리콘이고, 방금 읽은 워드가 PCI Express
	 * capability 안의 루트 제어 레지스터 자리일 때. */
	if (pci_is_root_bus(bus) && (port->version == XGENE_PCIE_IP_VER_1) &&
	    ((where & ~0x3) == XGENE_V1_PCI_EXP_CAP + PCI_EXP_RTCTL))
		/* [한국어] 읽어 온 워드에서 RRS 소프트웨어 가시성 '지원' 비트를 지운다.
		 * 16비트 왼쪽으로 미는 것은, 루트 제어(RTCTL)와 루트 능력(RTCAP)이 같은 32비트
		 * 워드에 이웃해 있어 dword 로 읽으면 능력 쪽이 상위 16비트에 오기 때문이다.
		 * 이렇게 하면 PCI 코어가 그 기능을 켜지 않아, 문제의 재시도 폭주 자체가 생기지
		 * 않는다. 두 상수의 실제 오프셋/비트 값은 include/ 아래에 있어 확인 못 함. */
		*val &= ~(PCI_EXP_RTCAP_RRS_SV << 16);

	/* [한국어] 호출자가 1바이트나 2바이트를 요청했다면, 통째로 읽은 워드에서 해당
	 * 부분을 뽑아내야 한다. 표준 함수에 크기 4 를 주었기 때문에 이 일이 여기로 왔다. */
	if (size <= 2)
		/* [한국어] where & 3 = 정렬 지점으로부터의 바이트 오프셋 → 8을 곱해 비트
		 * 오프셋으로 만들고 오른쪽으로 민 뒤, 요청한 폭만큼의 마스크로 상위를 지운다.
		 * PCI 설정공간이 리틀 엔디언이고 readl 이 CPU 바이트 순서로 이미 바꿔 주므로
		 * 이 산술이 성립한다. */
		*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);

	/* [한국어] 성공을 알린다. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어] 여기부터 #endif 까지가 ACPI 부팅 경로다. ACPI 지원과 PCI quirk 지원이
 * 둘 다 켜져 있을 때만 컴파일된다 — 이 코드가 결국 '이 하드웨어만의 예외 처리를
 * ECAM 프레임워크에 끼워 넣는 일' 이라서 quirk 쪽 설정에도 묶여 있는 것으로
 * 읽히지만, 그 결합의 근거를 밝힌 주석이나 커밋 설명은 코드에 없다.
 * 이 구역이 꺼지면 아래의 xgene_v1/xgene_v2 ECAM 연산 표도 함께 사라지고,
 * DT 경로만 남는다. */
#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)
/* [한국어]
 * xgene_get_csr_resource - ACPI 의 _CRS 에서 CSR 창 자원을 한 개 뽑아낸다
 *
 * @adev: 이 컨트롤러의 ACPI 디바이스.
 * @res: 뽑아낸 자원을 복사해 담을 곳.
 * @return: 0 성공, 음수 실패(_CRS 파싱 실패 또는 메모리 자원 없음).
 *
 * DT 경로에서는 "csr" 이라는 이름표가 붙은 자원을 골라 오지만, ACPI 에는
 * 그런 이름표가 없다. 그래서 _CRS(Current Resource Settings)가 나열한 자원 중
 * 메모리 종류만 걸러 낸 뒤 그 첫 번째를 CSR 창으로 삼는다 — 순서에 의존하는
 * 방식이며, 펌웨어가 CSR 창을 먼저 나열한다는 전제 위에 서 있다.
 * 자원 목록은 ACPI 코어가 할당하므로 다 쓰고 나면 반드시 풀어 주어야 한다.
 * 실행 컨텍스트: ECAM 창 생성 도중(프로세스 컨텍스트). 할당이 있어 잠들 수 있다.
 * 에러 경로: 파싱 실패와 '메모리 자원 없음' 을 각각 로그와 함께 돌려준다.
 *
 * [상류 코드 관찰] 실패로 되돌아가는 두 경로에서는 acpi_dev_free_resource_list()
 * 를 부르지 않는다. 다만 첫 번째는 파싱 자체가 실패한 경우이고 두 번째는 목록이
 * 비어 있는 경우라, 실제로 풀어 줄 것이 있는지는 acpi_dev_get_resources() 의
 * 구현에 달렸고 그 코드는 drivers/acpi 가 이 트리에 없어 확인 못 함.
 *
 * 호출 체인:
 *   xgene_pcie_ecam_init() → [xgene_get_csr_resource]
 *     → acpi_dev_get_resources(), acpi_dev_free_resource_list()
 */
static int xgene_get_csr_resource(struct acpi_device *adev,
				  struct resource *res)
{
	/* [한국어] 로그 대상. */
	struct device *dev = &adev->dev;
	/* [한국어] 목록에서 꺼낼 첫 항목. */
	struct resource_entry *entry;
	/* [한국어] 자원 목록의 머리. 지역 변수라 함수가 끝나기 전에 반드시 비워야 한다. */
	struct list_head list;
	/* [한국어] 걸러 낼 자원 종류를 담아 콜백에 넘길 값. */
	unsigned long flags;
	/* [한국어] 파싱 결과 — 음수면 오류, 0 이상이면 얻은 자원 개수다. */
	int ret;

	/* [한국어] 목록을 빈 상태로 초기화한다. */
	INIT_LIST_HEAD(&list);
	/* [한국어] 메모리 종류만 받겠다고 지정한다. */
	flags = IORESOURCE_MEM;
	/* [한국어] _CRS 를 파싱해 자원을 목록에 채운다. 표준 필터 콜백에 위 플래그를
	 * 불투명 인자로 넘겨, 메모리가 아닌 자원은 버리게 한다. */
	ret = acpi_dev_get_resources(adev, &list,
				     acpi_dev_filter_resource_type_cb,
				     (void *) flags);
	/* [한국어] 파싱 자체가 실패했다. */
	if (ret < 0) {
		/* [한국어] 어떤 오류였는지 남기고 */
		dev_err(dev, "failed to parse _CRS method, error code %d\n",
			ret);
		return ret;
	}

	/* [한국어] 파싱은 됐는데 메모리 자원이 하나도 없었다. */
	if (ret == 0) {
		/* [한국어] 그 사실을 남기고 */
		dev_err(dev, "no IO and memory resources present in _CRS\n");
		/* [한국어] 잘못된 펌웨어 서술로 보고 거절한다. */
		return -EINVAL;
	}

	/* [한국어] 목록의 첫 항목을 CSR 창으로 삼는다. 순서에 의존하는 선택이다. */
	entry = list_first_entry(&list, struct resource_entry, node);
	/* [한국어] 자원 서술을 값으로 복사해 둔다. 아래에서 목록을 풀어 버릴 것이므로
	 * 포인터를 들고 있으면 안 되기 때문이다. */
	*res = *entry->res;
	/* [한국어] ACPI 코어가 할당한 목록을 반납한다. */
	acpi_dev_free_resource_list(&list);
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * xgene_pcie_ecam_init - ACPI 경로의 초기화 후크. CSR 창만 매핑하고 버전을 기록한다
 *
 * @cfg: 방금 만들어진 ECAM 설정공간 창.
 * @ipversion: 이 하드웨어의 IP 버전(v1 또는 v2). 호출한 얇은 껍데기가 정해 준다.
 * @return: 0 성공, -ENOMEM 또는 CSR 자원 획득 실패.
 *
 * DT 경로의 probe 와 견주면 하는 일이 훨씬 적다. 아웃바운드·인바운드 창 설정도,
 * 클럭도, 벤더 ID 쓰기도 없다 — ACPI 로 부팅하는 시스템에서는 펌웨어가 그것을
 * 이미 해 두었다는 전제이기 때문이다. 그래서 여기서는 설정공간 접근에 꼭 필요한
 * 두 가지, 즉 CSR 창 주소(RTDID 를 쓰려면 필요하다)와 IP 버전(RRS 우회 판정에
 * 필요하다)만 갖춰 둔다.
 * 설정공간 창은 ECAM 프레임워크가 이미 만들어 둔 것을 그대로 물려받는다.
 * 실행 컨텍스트: ECAM 창 생성 도중(프로세스 컨텍스트). 할당이 있어 잠들 수 있다.
 * 에러 경로: 각 단계에서 곧장 되돌아가며, devm 할당은 자동 정리된다.
 *
 * 호출 체인:
 *   (ACPI 의 ECAM 창 생성) → xgene_v1_pcie_ecam_init() 또는
 *   xgene_v2_pcie_ecam_init() → [xgene_pcie_ecam_init]
 *     → xgene_get_csr_resource(), devm_pci_remap_cfg_resource()
 */
static int xgene_pcie_ecam_init(struct pci_config_window *cfg, u32 ipversion)
{
	/* [한국어] ECAM 창을 만든 device — 곧 이 컨트롤러의 ACPI 디바이스다. */
	struct device *dev = cfg->parent;
	/* [한국어] 그 device 를 ACPI 디바이스로 되돌린다. _CRS 를 읽으려면 필요하다. */
	struct acpi_device *adev = to_acpi_device(dev);
	/* [한국어] 새로 만들 컨트롤러 객체. */
	struct xgene_pcie *port;
	/* [한국어] _CRS 에서 뽑아낼 CSR 창 자원. */
	struct resource csr;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 컨트롤러 객체를 0 으로 채워 할당한다. DT 경로와 달리 브리지 뒤가
	 * 아니라 따로 잡히므로, 이 객체에서 브리지로 거슬러 올라갈 수는 없다.
	 * 그래도 문제가 없는 것은 ACPI 경로에서 창 설정을 하지 않아 브리지의 창 목록을
	 * 볼 일이 없기 때문이다. */
	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!port)
		return -ENOMEM;

	/* [한국어] _CRS 에서 CSR 창 자원을 얻는다. */
	ret = xgene_get_csr_resource(adev, &csr);
	/* [한국어] 못 얻으면 RTDID 를 쓸 방법이 없으므로 진행할 수 없다. */
	if (ret) {
		/* [한국어] 원인을 남기고 */
		dev_err(dev, "can't get CSR resource\n");
		return ret;
	}
	/* [한국어] CSR 창을 매핑한다. 일반 ioremap 이 아니라 설정공간용 판을 쓰는 것은,
	 * 아키텍처에 따라 설정공간 접근에 다른 메모리 속성이 필요할 수 있기 때문이다. */
	port->csr_base = devm_pci_remap_cfg_resource(dev, &csr);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(port->csr_base))
		return PTR_ERR(port->csr_base);

	/* [한국어] 설정공간 창은 ECAM 프레임워크가 이미 만들어 둔 것을 그대로 쓴다.
	 * DT 경로가 "cfg" 자원을 직접 매핑하는 것과 다른 점이다. */
	port->cfg_base = cfg->win;
	/* [한국어] 호출한 껍데기가 정해 준 IP 버전을 기록한다. RRS 우회 판정에 쓰인다. */
	port->version = ipversion;

	/* [한국어] 이 객체를 ECAM 창의 사설 포인터로 걸어 둔다. 이후 설정공간 접근에서
	 * pcie_bus_to_port() 이 이 값을 되찾는다. */
	cfg->priv = port;
	/* [한국어] 초기화 성공. */
	return 0;
}

/* [한국어]
 * xgene_v1_pcie_ecam_init - v1 하드웨어용 ECAM 초기화 껍데기
 *
 * @cfg: ECAM 설정공간 창.
 * @return: 공통 초기화 함수의 결과를 그대로 전달한다.
 *
 * 공통 초기화 함수는 IP 버전을 인자로 받는데, ECAM 프레임워크의 init 후크는
 * 인자를 하나만 넘길 수 있다. 그 간극을 메우려고 버전을 상수로 박은 껍데기를
 * 버전마다 하나씩 둔 것이다 — 아래 v2 판과 이 함수의 차이는 그 상수 하나뿐이다.
 * 실행 컨텍스트: ECAM 창 생성 도중(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   (ACPI 의 ECAM 창 생성) → [xgene_v1_pcie_ecam_init] → xgene_pcie_ecam_init()
 */
static int xgene_v1_pcie_ecam_init(struct pci_config_window *cfg)
{
	/* [한국어] v1 임을 못박아 공통 초기화로 넘긴다. */
	return xgene_pcie_ecam_init(cfg, XGENE_PCIE_IP_VER_1);
}

/* [한국어] v1 하드웨어용 ECAM 연산 표. static 이 아닌 것은 ACPI 쪽에서 이 이름을
 * 직접 참조해 쓰기 때문인데, 그 참조처를 이 트리 안에서는 찾지 못했다 —
 * drivers/acpi 가 sparse checkout 에 없어 확인 못 함. */
const struct pci_ecam_ops xgene_v1_pcie_ecam_ops = {
	/* [한국어] ECAM 창을 만든 뒤 불릴 초기화 후크. */
	.init		= xgene_v1_pcie_ecam_init,
	/* [한국어] 설정공간 접근 연산들. */
	.pci_ops	= {
		/* [한국어] 접근 주소를 만드는 후크. DT 경로와 같은 함수를 그대로 쓴다 —
		 * 두 경로가 공유하는 부분이 바로 여기다. */
		.map_bus	= xgene_pcie_map_bus,
		/* [한국어] 읽기도 DT 경로와 같은 함수. RRS 우회가 두 경로 모두에 적용된다. */
		.read		= xgene_pcie_config_read32,
		/* [한국어] 쓰기는 표준 구현을 그대로 쓴다.
		 * [상류 코드 관찰] 아래 DT 경로의 연산 표는 같은 자리에
		 * pci_generic_config_write32(1/2바이트 쓰기를 32비트 읽고-고쳐-쓰기로 흉내 내는
		 * 판)를 건다. 같은 하드웨어의 같은 창을 쓰는데 쓰기 함수만 두 경로가 다르며,
		 * 그 차이의 이유는 코드에 적혀 있지 않다. */
		.write		= pci_generic_config_write,
	}
};

/* [한국어]
 * xgene_v2_pcie_ecam_init - v2 하드웨어용 ECAM 초기화 껍데기
 *
 * @cfg: ECAM 설정공간 창.
 * @return: 공통 초기화 함수의 결과를 그대로 전달한다.
 *
 * v1 판과 완전히 같고 넘기는 버전 상수만 다르다. 이 값이 v2 이면
 * xgene_pcie_config_read32() 의 RRS 우회 조건이 성립하지 않아, 그 우회가
 * 적용되지 않는다 — 즉 버전 상수의 유일한 쓰임새가 그 판정이다.
 * 실행 컨텍스트: ECAM 창 생성 도중(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   (ACPI 의 ECAM 창 생성) → [xgene_v2_pcie_ecam_init] → xgene_pcie_ecam_init()
 */
static int xgene_v2_pcie_ecam_init(struct pci_config_window *cfg)
{
	/* [한국어] v2 임을 못박아 공통 초기화로 넘긴다. */
	return xgene_pcie_ecam_init(cfg, XGENE_PCIE_IP_VER_2);
}

/* [한국어] v2 하드웨어용 ECAM 연산 표. 위 v1 판과 init 후크만 다르다.
 * 이쪽 역시 참조처를 이 트리 안에서 찾지 못했다. */
const struct pci_ecam_ops xgene_v2_pcie_ecam_ops = {
	/* [한국어] v2 용 초기화 후크. */
	.init		= xgene_v2_pcie_ecam_init,
	/* [한국어] 설정공간 접근 연산들 — 아래 셋은 v1 판과 완전히 같다. */
	.pci_ops	= {
		/* [한국어] 접근 주소를 만드는 후크. */
		.map_bus	= xgene_pcie_map_bus,
		/* [한국어] RRS 우회가 들어간 읽기(v2 에서는 조건이 성립하지 않아 우회가 걸리지 않는다). */
		.read		= xgene_pcie_config_read32,
		/* [한국어] 표준 쓰기. */
		.write		= pci_generic_config_write,
	}
};
/* [한국어] ACPI 경로 구역의 끝. 여기부터는 DT 경로와 공통 코드다. */
#endif

/* [한국어]
 * xgene_pcie_set_ib_mask - 인바운드 창 0 의 크기 마스크를 16비트씩 나눠 새긴다
 *
 * @port: 컨트롤러 객체.
 * @addr: 마스크가 놓일 레지스터 구역의 시작 오프셋(호출처는 BRIDGE_CFG_4 하나뿐).
 * @flags: 마스크 하위 4비트에 함께 실을 BAR 타입 표시.
 * @size: 창 크기(2의 거듭제곱).
 * @return: 만들어진 64비트 마스크. [상류 코드 관찰] 유일한 호출자가 이 반환값을
 *          쓰지 않는다.
 *
 * 이 함수가 이상하게 생긴 이유는 마스크 필드가 레지스터 경계에 맞춰 놓여
 * 있지 않기 때문이다. 코드를 그대로 따라가면 64비트 마스크가 이렇게 흩어진다 —
 * 마스크 비트 15..0 은 addr 의 상위 16비트에, 31..16 은 addr+4 의 하위 16비트에,
 * 47..32 는 addr+4 의 상위 16비트에, 63..48 은 addr+8 의 하위 16비트에 놓인다.
 * 즉 마스크 필드 전체가 레지스터 배열 안에서 16비트만큼 밀려 있다. 그래서
 * 각 워드마다 '기존 절반은 남기고 나머지 절반만 갈아 끼우는' 읽고-고쳐-쓰기가
 * 필요하고, 그것이 네 번 반복된다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xgene_pcie_setup_ib_reg() → [xgene_pcie_set_ib_mask]
 *     → xgene_pcie_readl(), xgene_pcie_writel()
 */
static u64 xgene_pcie_set_ib_mask(struct xgene_pcie *port, u32 addr,
				  u32 flags, u64 size)
{
	/* [한국어] 크기 s 에 대해 ~(s-1) 이 곧 BAR 크기 마스크다. 하위 4비트는 주소가
	 * 아니라 타입 필드이므로 지우고, 대신 호출자가 준 플래그를 얹는다. */
	u64 mask = (~(size - 1) & PCI_BASE_ADDRESS_MEM_MASK) | flags;
	/* [한국어] 읽고-고쳐-쓰기에서 '읽은 값' 을 담는 변수. */
	u32 val32 = 0;
	/* [한국어] 되쓸 값을 조립하는 변수. */
	u32 val;

	/* [한국어] 첫 워드를 읽는다. */
	val32 = xgene_pcie_readl(port, addr);
	/* [한국어] 하위 16비트는 그대로 두고 상위 16비트만 마스크 비트 15..0 으로 채운다. */
	val = (val32 & 0x0000ffff) | (lower_32_bits(mask) << 16);
	/* [한국어] 되쓴다. */
	xgene_pcie_writel(port, addr, val);

	/* [한국어] 다음 워드를 읽는다. */
	val32 = xgene_pcie_readl(port, addr + 0x04);
	/* [한국어] 상위 16비트는 그대로 두고 하위 16비트를 마스크 비트 31..16 으로 채운다. */
	val = (val32 & 0xffff0000) | (lower_32_bits(mask) >> 16);
	/* [한국어] 되쓴다. */
	xgene_pcie_writel(port, addr + 0x04, val);

	/* [한국어] 같은 워드를 다시 읽는다 — 방금 쓴 값을 포함한 현재 내용이 필요하기
	 * 때문이다. 한 워드의 두 절반을 각각 다른 단계에서 채우는 구조라 이렇게 된다. */
	val32 = xgene_pcie_readl(port, addr + 0x04);
	/* [한국어] 이번에는 그 워드의 상위 16비트를 마스크 비트 47..32 로 채운다. */
	val = (val32 & 0x0000ffff) | (upper_32_bits(mask) << 16);
	/* [한국어] 되쓴다. */
	xgene_pcie_writel(port, addr + 0x04, val);

	/* [한국어] 마지막 워드를 읽는다. */
	val32 = xgene_pcie_readl(port, addr + 0x08);
	/* [한국어] 하위 16비트를 마스크 비트 63..48 로 채운다. */
	val = (val32 & 0xffff0000) | (upper_32_bits(mask) >> 16);
	/* [한국어] 되쓴다. 이로써 64비트 마스크가 모두 자리를 잡았다. */
	xgene_pcie_writel(port, addr + 0x08, val);

	/* [한국어] 만든 마스크를 돌려준다(호출자는 쓰지 않는다). */
	return mask;
}

/* [한국어]
 * xgene_pcie_linkup - 링크가 올라왔는지와 협상된 폭·속도를 읽는다
 *
 * @port: 컨트롤러 객체. link_up 필드를 이 함수가 정한다.
 * @lanes: 협상된 레인 수를 담을 곳.
 * @speed: 협상된 링크 속도(0 부터 시작하는 인코딩)를 담을 곳.
 * @return: 없음 — 결과는 port->link_up 과 두 출력 인자로 전달된다.
 *
 * 링크가 죽어 있어도 probe 는 실패하지 않는다. 슬롯이 비어 있는 것이 정상인
 * 경우가 많기 때문이며, 그래서 이 함수는 판단하지 않고 사실만 보고한다.
 * 링크가 죽어 있으면 lanes/speed 를 건드리지 않는데, 호출자가 그 변수를 0 으로
 * 초기화해 두었으므로 쓰레기 값이 출력되지는 않는다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_setup() → [xgene_pcie_linkup] → xgene_pcie_readl()
 */
static void xgene_pcie_linkup(struct xgene_pcie *port,
			      u32 *lanes, u32 *speed)
{
	/* [한국어] 읽은 레지스터 값을 담는 변수. 두 레지스터에 재사용된다. */
	u32 val32;

	/* [한국어] 먼저 '링크 없음' 으로 두고 시작한다 — 아래에서 확인될 때만 뒤집는다. */
	port->link_up = false;
	/* [한국어] 코어의 제어/상태 레지스터를 읽는다. */
	val32 = xgene_pcie_readl(port, PCIECORE_CTLANDSTATUS);
	/* [한국어] 링크 업 비트가 서 있는가. */
	if (val32 & LINK_UP_MASK) {
		/* [한국어] 서 있으면 링크가 살아 있다고 기록한다. */
		port->link_up = true;
		/* [한국어] 같은 값의 비트 15..14 에서 협상된 PHY 속도를 뽑는다. */
		*speed = PIPE_PHY_RATE_RD(val32);
		/* [한국어] 레인 수는 다른 레지스터에 있으므로 한 번 더 읽는다. */
		val32 = xgene_pcie_readl(port, BRIDGE_STATUS_0);
		/* [한국어] 상위 6비트(비트 31..26)가 레인 수다. 마스크 없이 시프트만 하는 것은
		 * 그 위에 다른 필드가 없다는 뜻이다. */
		*lanes = val32 >> 26;
	}
}

/* [한국어]
 * xgene_pcie_init_port - 컨트롤러 클럭을 얻어 켠다
 *
 * @port: 컨트롤러 객체. clk 필드를 이 함수가 채운다.
 * @return: 0 성공, -ENODEV(클럭 없음), 또는 활성화 실패 코드.
 *
 * 이름은 거창하지만 실제로 하는 일은 클럭 하나를 켜는 것뿐이다. 이 클럭이
 * 없으면 아래의 창 설정과 링크 확인이 모두 무의미하므로, 자원 매핑 다음이자
 * 하드웨어를 만지기 직전에 놓여 있다.
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). 클럭 프레임워크 호출이 잠들 수 있다.
 * 에러 경로: 두 실패 모두 곧장 되돌아간다.
 *
 * [상류 코드 관찰] devm_clk_get() 이 아니라 clk_get() 을 쓰는데, 짝이 되는
 * clk_put() 이 이 파일 어디에도 없다. clk_prepare_enable() 의 짝인
 * clk_disable_unprepare() 도 없다. 이 드라이버는 언바인드를 막아 두어
 * (아래 suppress_bind_attrs) 정상 제거 경로가 없으므로, 그 누락이 실제로
 * 드러나지는 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_probe() → [xgene_pcie_init_port]
 *     → clk_get(), clk_prepare_enable()
 */
static int xgene_pcie_init_port(struct xgene_pcie *port)
{
	/* [한국어] 로그와 클럭 조회의 기준 device. */
	struct device *dev = port->dev;
	/* [한국어] 클럭 활성화 결과. */
	int rc;

	/* [한국어] 이름 없이(NULL) 클럭을 요청한다 — DT 에 clocks 항목이 하나뿐이라고
	 * 가정하는 것이다. devm 판이 아니라는 점은 위 관찰 참조. */
	port->clk = clk_get(dev, NULL);
	/* [한국어] 클럭이 없으면 컨트롤러를 동작시킬 수 없다. */
	if (IS_ERR(port->clk)) {
		/* [한국어] 원인을 남기고 */
		dev_err(dev, "clock not available\n");
		/* [한국어] 장치가 없다는 뜻으로 실패시킨다 — 반환값이 IS_ERR 에 실린 실제 오류가
		 * 아니라 고정된 -ENODEV 라는 점에 주의. */
		return -ENODEV;
	}

	/* [한국어] 클럭을 준비시키고 켠다. 이 호출부터 하드웨어가 살아난다. */
	rc = clk_prepare_enable(port->clk);
	/* [한국어] 켜지지 않으면 진행할 수 없다. */
	if (rc) {
		/* [한국어] 원인을 남기고 */
		dev_err(dev, "clock enable failed\n");
		return rc;
	}

	/* [한국어] 클럭 준비 완료. */
	return 0;
}

/* [한국어]
 * xgene_pcie_map_reg - DT 가 준 두 레지스터 창(csr, cfg)을 매핑한다
 *
 * @port: 컨트롤러 객체. csr_base / cfg_base / cfg_addr 을 이 함수가 채운다.
 * @pdev: 자원을 가진 플랫폼 디바이스.
 * @return: 0 성공, 매핑 실패 시 그 오류.
 *
 * DT 경로에서만 쓰인다. 창이 둘인 것이 이 하드웨어의 구조를 그대로 보여 준다 —
 * 컨트롤러를 제어하는 CSR 창과, 설정공간을 들여다보는 별도의 창이다. ACPI
 * 경로에서는 후자를 ECAM 프레임워크가 대신 만들어 준다.
 * 자원을 인덱스가 아니라 이름("csr", "cfg")으로 찾는 것은 DT 의 reg 순서 변경에
 * 흔들리지 않기 위해서다.
 * 설정공간 창은 물리 주소도 함께 보관하는데, 나중에 컨트롤러에게 그 창의 위치를
 * 알려 주어야 하기 때문이다(xgene_pcie_setup_cfg_reg 참조).
 * 실행 컨텍스트: probe 경로. 매핑이 잠들 수 있다.
 *
 * [상류 코드 관찰] platform_get_resource_byname() 의 반환값을 NULL 검사 없이
 * 그대로 매핑 함수에 넘기고, cfg 쪽은 그 뒤 res->start 로 역참조한다. DT 에
 * 그 이름의 자원이 없으면 NULL 을 따라가게 되는데, 매핑 함수가 NULL 을 걸러
 * 주는지는 그 구현(lib/devres.c 계열)이 이 트리에 없어 확인 못 함 — 다만
 * res->start 역참조는 그 함수들이 무엇을 하든 이 파일 안에서 일어난다.
 *
 * 호출 체인:
 *   xgene_pcie_probe() → [xgene_pcie_map_reg]
 *     → platform_get_resource_byname(), devm_pci_remap_cfg_resource(),
 *       devm_ioremap_resource()
 */
static int xgene_pcie_map_reg(struct xgene_pcie *port,
			      struct platform_device *pdev)
{
	/* [한국어] 매핑의 기준 device. */
	struct device *dev = port->dev;
	/* [한국어] 찾은 자원 서술. 두 창에 재사용된다. */
	struct resource *res;

	/* [한국어] 이름표 "csr" 이 붙은 메모리 자원을 찾는다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "csr");
	/* [한국어] 설정공간용 매핑 함수로 CSR 창을 매핑한다. 일반 ioremap 이 아닌 판을
	 * 쓰는 것은 아키텍처에 따라 다른 메모리 속성이 필요할 수 있기 때문이다. */
	port->csr_base = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(port->csr_base))
		return PTR_ERR(port->csr_base);

	/* [한국어] 이름표 "cfg" 가 붙은 설정공간 창 자원을 찾는다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	/* [한국어] 이쪽은 일반 ioremap 판으로 매핑한다.
	 * [상류 코드 관찰] 같은 파일 안에서 두 창이 서로 다른 매핑 함수를 쓰는데,
	 * 그 차이의 이유는 코드에 적혀 있지 않다. */
	port->cfg_base = devm_ioremap_resource(dev, res);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(port->cfg_base))
		return PTR_ERR(port->cfg_base);
	/* [한국어] 물리 주소도 보관해 둔다. 나중에 컨트롤러에게 이 창의 위치를 알려
	 * 주어야 하는데, 그때는 가상 주소가 아니라 물리 주소가 필요하기 때문이다. */
	port->cfg_addr = res->start;

	/* [한국어] 두 창 모두 준비 완료. */
	return 0;
}

/* [한국어]
 * xgene_pcie_setup_ob_reg - 아웃바운드 창 하나를 레지스터 여섯 개에 새긴다
 *
 * @port: 컨트롤러 객체.
 * @res: 이 창이 담당할 자원(크기와 종류를 여기서 얻는다).
 * @offset: 창 레지스터 묶음의 시작 오프셋(OMR1/OMR2/OMR3 중 하나).
 * @cpu_addr: CPU 쪽 시작 주소.
 * @pci_addr: 그것이 옮겨질 PCI 쪽 시작 주소.
 * @return: 없음.
 *
 * 아웃바운드 창은 CPU 가 낸 주소를 PCI 주소로 옮긴다 — 드라이버가 장치의 BAR 를
 * 읽고 쓰는 길이 이 창이다. 창 하나가 32비트 레지스터 여섯 개를 연달아 쓰며,
 * 코드가 쓰는 오프셋(0x00~0x14)에서 그 배치가 드러난다: CPU 주소 하위/상위,
 * 마스크 하위/상위, PCI 주소 하위/상위.
 * 크기 검사가 중요한 이유: 마스크와 활성화 비트가 같은 워드에 겹쳐 있어서,
 * 크기가 최소치에 못 미치면 mask 가 0 인 채로 남고 그 결과 활성화 비트도 0 이
 * 되어 창이 아예 꺼진다. 경고만 남기고 계속 진행하지만 그 창은 동작하지 않는다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 없다 — 크기 문제도 경고에 그친다.
 *
 * 호출 체인:
 *   xgene_pcie_map_ranges() → [xgene_pcie_setup_ob_reg] → xgene_pcie_writel()
 */
static void xgene_pcie_setup_ob_reg(struct xgene_pcie *port,
				    struct resource *res, u32 offset,
				    u64 cpu_addr, u64 pci_addr)
{
	/* [한국어] 경고 로그 대상. */
	struct device *dev = port->dev;
	/* [한국어] 이 창이 덮을 크기. */
	resource_size_t size = resource_size(res);
	/* [한국어] 자원 종류(메모리인지 IO인지). 아래에서 최소 크기와 플래그가 갈린다. */
	u64 restype = resource_type(res);
	/* [한국어] 창 마스크. 0 으로 시작하는 것이 중요하다 — 크기가 모자라면 이 값이
	 * 그대로 쓰여 창이 꺼진다. */
	u64 mask = 0;
	/* [한국어] 이 종류에 요구되는 최소 창 크기. */
	u32 min_size;
	/* [한국어] 마스크에 얹을 플래그. 활성화 비트로 시작한다. */
	u32 flag = EN_REG;

	/* [한국어] 메모리 창인가. */
	if (restype == IORESOURCE_MEM) {
		/* [한국어] 메모리 창의 최소 크기는 128MB 다. 이 하드웨어의 창 입자가 그만큼
		 * 크다는 뜻으로 읽히며, 정확한 근거는 레지스터 사양서가 필요해 확인 못 함. */
		min_size = SZ_128M;
	/* [한국어] 메모리가 아니면 IO 창이다. */
	} else {
		/* [한국어] IO 창의 최소 크기는 128바이트로 훨씬 작다. */
		min_size = 128;
		/* [한국어] 이 창이 IO 공간용임을 마스크에 표시한다. */
		flag |= OB_LO_IO;
	}

	/* [한국어] 크기가 최소치를 넘을 때만 제대로 된 마스크를 만든다. */
	if (size >= min_size)
		/* [한국어] 크기 s 에 대한 마스크 ~(s-1) 에 플래그를 얹는다. 활성화 비트가
		 * 여기서 함께 들어간다. */
		mask = ~(size - 1) | flag;
	/* [한국어] 모자라면 마스크가 0 인 채로 남는다 — 그 결과 이 창은 꺼진다. */
	else
		/* [한국어] 창이 동작하지 않을 것임을 경고한다. 오류로 만들지 않아 probe 는 계속된다. */
		dev_warn(dev, "res size 0x%llx less than minimum 0x%x\n",
			 (u64)size, min_size);

	/* [한국어] CPU 쪽 시작 주소의 하위 32비트. */
	xgene_pcie_writel(port, offset, lower_32_bits(cpu_addr));
	/* [한국어] 그 상위 32비트. */
	xgene_pcie_writel(port, offset + 0x04, upper_32_bits(cpu_addr));
	/* [한국어] 마스크의 하위 32비트(활성화 비트가 여기 들어 있다). */
	xgene_pcie_writel(port, offset + 0x08, lower_32_bits(mask));
	/* [한국어] 마스크의 상위 32비트. */
	xgene_pcie_writel(port, offset + 0x0c, upper_32_bits(mask));
	/* [한국어] 옮겨질 PCI 쪽 주소의 하위 32비트. */
	xgene_pcie_writel(port, offset + 0x10, lower_32_bits(pci_addr));
	/* [한국어] 그 상위 32비트. 여섯 워드를 모두 채우면 창이 살아난다. */
	xgene_pcie_writel(port, offset + 0x14, upper_32_bits(pci_addr));
}

/* [한국어]
 * xgene_pcie_setup_cfg_reg - 설정공간 창의 위치를 컨트롤러에게 알려 준다
 *
 * @port: 컨트롤러 객체.
 * @return: 없음.
 *
 * 드라이버가 매핑한 설정공간 창은 CPU 쪽 이야기일 뿐이고, 컨트롤러도 그 창이
 * 어디에 있는지 알아야 그 주소로 들어오는 접근을 설정 사이클로 바꿔 준다.
 * 그래서 물리 주소를 두 워드에 나눠 써 넣고 활성화 비트를 세운다.
 * 아웃바운드 창을 모두 새긴 뒤 마지막에 불리는데, 이 창도 결국 아웃바운드
 * 변환의 일종이기 때문이다.
 * ACPI 경로에서는 불리지 않는다 — 펌웨어가 이미 해 두었다는 전제다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_map_ranges() → [xgene_pcie_setup_cfg_reg] → xgene_pcie_writel()
 */
static void xgene_pcie_setup_cfg_reg(struct xgene_pcie *port)
{
	/* [한국어] xgene_pcie_map_reg() 이 보관해 둔 설정공간 창의 물리 주소. */
	u64 addr = port->cfg_addr;

	/* [한국어] 하위 32비트를 새긴다. */
	xgene_pcie_writel(port, CFGBARL, lower_32_bits(addr));
	/* [한국어] 상위 32비트를 새긴다. */
	xgene_pcie_writel(port, CFGBARH, upper_32_bits(addr));
	/* [한국어] 활성화 비트를 세운다. 이 쓰기 이후에야 그 창을 통한 설정공간 접근이
	 * 동작한다. */
	xgene_pcie_writel(port, CFGCTL, EN_REG);
}

/* [한국어]
 * xgene_pcie_map_ranges - DT 의 ranges 를 훑어 아웃바운드 창 세 개를 채운다
 *
 * @port: 컨트롤러 객체.
 * @return: 0 성공, -EINVAL(다룰 수 없는 자원 종류).
 *
 * PCI 코어가 DT 의 ranges 를 파싱해 브리지의 창 목록에 담아 두었으므로, 여기서는
 * 그 목록을 훑어 종류에 맞는 창에 새기기만 한다. 용도가 미리 정해져 있다 —
 * 비prefetchable 메모리는 창 1, prefetchable 메모리는 창 2, IO 는 창 3 이다.
 * IO 자원만 pci_pio_to_address() 를 거치는데, 리눅스에서 IO 자원의 start 는
 * 물리 주소가 아니라 논리 포트 번호이기 때문이다. 메모리 자원은 그대로 쓴다.
 * PCI 쪽 주소는 세 경우 모두 'CPU 주소 - 변환 오프셋' 으로 얻는다.
 * 사설 데이터에서 브리지로 거슬러 올라가는 pci_host_bridge_from_priv() 가
 * 여기서 쓰이는데, 이는 DT 경로에서 둘이 한 덩어리로 할당되기 때문에 성립한다
 * — 그래서 이 함수는 ACPI 경로에서 부를 수 없고, 실제로 부르지도 않는다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 알 수 없는 자원 종류를 만나면 -EINVAL 로 probe 를 실패시킨다.
 *
 * 호출 체인:
 *   xgene_pcie_setup() → [xgene_pcie_map_ranges]
 *     → xgene_pcie_setup_ob_reg(), xgene_pcie_setup_cfg_reg()
 */
static int xgene_pcie_map_ranges(struct xgene_pcie *port)
{
	/* [한국어] 사설 데이터 포인터에서 그것을 품고 있는 호스트 브리지를 되찾는다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(port);
	/* [한국어] 창 목록 순회 커서. */
	struct resource_entry *window;
	/* [한국어] 로그 대상. */
	struct device *dev = port->dev;

	/* [한국어] 코어가 미리 파싱해 둔 아웃바운드 창 목록을 순서대로 훑는다. */
	resource_list_for_each_entry(window, &bridge->windows) {
		/* [한국어] 이 항목이 서술하는 자원. */
		struct resource *res = window->res;
		/* [한국어] 자원 플래그에서 종류를 뽑는다. */
		u64 restype = resource_type(res);

		/* [한국어] 어떤 창을 세우는지 디버그 로그로 남긴다. %pR 은 자원 범위를 출력하는
		 * 커널 포맷 지정자다. */
		dev_dbg(dev, "%pR\n", res);

		/* [한국어] 종류에 따라 쓸 창이 갈린다. */
		switch (restype) {
		/* [한국어] IO 공간 — 창 3 이 담당한다. */
		case IORESOURCE_IO:
			/* [한국어] CPU 주소 자리에는 논리 포트 번호를 물리 주소로 되돌린 값을 주고,
			 * PCI 주소 자리에는 변환 오프셋을 뺀 값을 준다. IO 자원에만 이 변환이 필요한
			 * 것은 리눅스가 IO 포트를 논리 번호로 다루기 때문이다. */
			xgene_pcie_setup_ob_reg(port, res, OMR3BARL,
						pci_pio_to_address(res->start),
						res->start - window->offset);
			break;
		/* [한국어] 메모리 공간 — prefetch 여부로 창이 갈린다. */
		case IORESOURCE_MEM:
			/* [한국어] 미리 읽어도 부작용이 없는 영역인가. 이 성질은 DT 의 ranges 첫 셀에
			 * 실려 오고 코어가 플래그로 옮겨 놓는다. */
			if (res->flags & IORESOURCE_PREFETCH)
				/* [한국어] prefetchable 메모리는 창 2 에 새긴다. 메모리 자원의 start 는 이미
				 * 물리 주소라 변환 없이 그대로 쓴다. */
				xgene_pcie_setup_ob_reg(port, res, OMR2BARL,
							res->start,
							res->start -
							window->offset);
			/* [한국어] 아니면 보통 메모리다. */
			else
				/* [한국어] 비prefetchable 메모리는 창 1 에 새긴다. */
				xgene_pcie_setup_ob_reg(port, res, OMR1BARL,
							res->start,
							res->start -
							window->offset);
			/* [한국어] 메모리 창 처리 끝. */
			break;
		/* [한국어] 버스 번호 범위 항목 — 주소 변환과 무관하므로 */
		case IORESOURCE_BUS:
			/* [한국어] 아무것도 하지 않는다. 코어가 버스 번호 배정에 알아서 쓴다. */
			break;
		/* [한국어] 그 밖의 종류는 이 하드웨어가 다룰 수 없다. */
		default:
			/* [한국어] 어떤 자원이 문제인지 남기고 */
			dev_err(dev, "invalid resource %pR\n", res);
			/* [한국어] probe 를 실패시킨다. 위 pci-v3-semi 계열 드라이버가 같은 상황을
			 * 경고로만 넘기는 것과 달리, 이쪽은 오류로 다룬다. */
			return -EINVAL;
		}
	}
	/* [한국어] 아웃바운드 창을 모두 새긴 뒤 설정공간 창 위치도 알려 준다. */
	xgene_pcie_setup_cfg_reg(port);
	/* [한국어] 아웃바운드 설정 완료. */
	return 0;
}

/* [한국어]
 * xgene_pcie_setup_pims - 인바운드 창의 목적지 주소와 크기를 새긴다
 *
 * @port: 컨트롤러 객체.
 * @pim_reg: 이 창의 목적지 레지스터 묶음 시작 오프셋.
 * @pim: PCI 주소를 옮겨 놓을 로컬(DDR) 쪽 주소.
 * @size: 창 크기 마스크(호출자가 ~(size-1) 형태로 넘긴다).
 * @return: 없음.
 *
 * 인바운드 창은 두 부분으로 나뉜다 — '어떤 PCI 주소를 잡을 것인가'(BAR 와
 * 마스크)와 '그것을 어디로 옮길 것인가'(여기서 다루는 목적지)다. 이 함수는
 * 후자를 맡으며, 창 세 개가 모두 같은 배치를 쓰기 때문에 하나로 묶여 있다.
 * 코드가 쓰는 오프셋에서 배치가 드러난다: 0x00/0x04 가 목적지 주소 하위/상위,
 * 0x10/0x14 가 크기 하위/상위다. 0x08 과 0x0c 는 건드리지 않는다.
 * 목적지 상위 워드에 캐시 일관성 비트를 함께 얹는 것이 이 함수의 특징이다 —
 * 들어오는 DMA 가 CPU 캐시와 일관되게 보이도록 하는 설정으로 읽힌다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_setup_ib_reg() → [xgene_pcie_setup_pims] → xgene_pcie_writel()
 */
static void xgene_pcie_setup_pims(struct xgene_pcie *port, u32 pim_reg,
				  u64 pim, u64 size)
{
	/* [한국어] 목적지 주소의 하위 32비트. */
	xgene_pcie_writel(port, pim_reg, lower_32_bits(pim));
	/* [한국어] 상위 32비트에 캐시 일관성 비트를 함께 얹어 쓴다. 세 창 모두 이 비트가
	 * 붙으므로, 이 드라이버가 여는 인바운드 경로는 전부 일관성 있는 접근이 된다. */
	xgene_pcie_writel(port, pim_reg + 0x04,
			  upper_32_bits(pim) | EN_COHERENCY);
	/* [한국어] 크기의 하위 32비트. 오프셋이 0x08 이 아니라 0x10 인 점에 주의 —
	 * 목적지와 크기 사이에 이 드라이버가 쓰지 않는 워드 두 개가 있다. */
	xgene_pcie_writel(port, pim_reg + 0x10, lower_32_bits(size));
	/* [한국어] 크기의 상위 32비트. */
	xgene_pcie_writel(port, pim_reg + 0x14, upper_32_bits(size));
}

/*
 * X-Gene PCIe support maximum 3 inbound memory regions
 * This function helps to select a region based on size of region
 */
/* [한국어]
 * xgene_pcie_select_ib_reg - 크기에 맞는 인바운드 창 번호를 고르고 예약한다
 *
 * @ib_reg_mask: 이미 쓰인 창을 비트로 표시해 둔 값. 이 함수가 갱신한다.
 * @size: 필요한 창 크기.
 * @return: 고른 창 번호(0, 1, 2) 또는 -EINVAL(맞는 빈 창이 없음).
 *
 * 위 상류 주석대로 인바운드 창은 최대 세 개이고, 창마다 다룰 수 있는 크기 범위가
 * 다르다. 그래서 dma-ranges 항목이 올 때마다 크기를 보고 알맞은 빈 창을 고른다.
 * 검사 순서가 곧 우선순위다 — 작은 범위 전용인 창 1 을 먼저 시도하고, 그 다음
 * 넓은 범위를 다루는 창 0, 마지막으로 창 2 다. 작은 요청이 넓은 창을 먼저
 * 차지해 버리지 않게 하려는 배치로 읽힌다.
 * 조건의 경계값(창 1 의 상한 16MB, 창 0 의 하한 1KB, 창 2 의 하한 1MB, 공통
 * 상한 1TB)은 하드웨어 제약일 텐데, 그 근거가 되는 사양서는 이 트리에 없다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 맞는 창이 없으면 -EINVAL 을 돌려주고, 호출자는 경고만 남긴 뒤
 * 그 범위를 건너뛴다.
 *
 * [상류 코드 관찰] 비교가 모두 배타적(초과/미만)이라 경계값 자체는 어느 창에도
 * 걸리지 않는다. 예컨대 크기가 정확히 16MB 이면 창 1 에 들어가지 못하고 창 0 으로
 * 넘어간다. 또 창 0 의 하한이 1KB 인데 창 1 의 하한은 4바이트로 훨씬 낮다.
 *
 * 호출 체인:
 *   xgene_pcie_setup_ib_reg() → [xgene_pcie_select_ib_reg]
 */
static int xgene_pcie_select_ib_reg(u8 *ib_reg_mask, u64 size)
{
	/* [한국어] 창 1 은 작은 범위 전용이다(16MB 미만). 아직 쓰이지 않았다면 여기부터
	 * 시도한다 — 넓은 창을 작은 요청에 낭비하지 않으려는 순서다. */
	if ((size > 4) && (size < SZ_16M) && !(*ib_reg_mask & (1 << 1))) {
		/* [한국어] 창 1 을 쓴 것으로 표시한다. */
		*ib_reg_mask |= (1 << 1);
		/* [한국어] 창 번호 1 을 돌려준다. */
		return 1;
	}

	/* [한국어] 창 0 은 1KB 부터 1TB 까지 넓은 범위를 다룬다. 인바운드 창 중 유일하게
	 * 루트 브리지의 BAR 를 직접 쓰는 창이기도 하다. */
	if ((size > SZ_1K) && (size < SZ_1T) && !(*ib_reg_mask & (1 << 0))) {
		/* [한국어] 창 0 을 쓴 것으로 표시한다. */
		*ib_reg_mask |= (1 << 0);
		/* [한국어] 창 번호 0 을 돌려준다. */
		return 0;
	}

	/* [한국어] 창 2 는 1MB 부터 1TB 까지 — 창 0 과 겹치지만 하한이 더 높다.
	 * 창 0 이 이미 쓰였을 때의 대안이다. */
	if ((size > SZ_1M) && (size < SZ_1T) && !(*ib_reg_mask & (1 << 2))) {
		/* [한국어] 창 2 를 쓴 것으로 표시한다. */
		*ib_reg_mask |= (1 << 2);
		/* [한국어] 창 번호 2 를 돌려준다. */
		return 2;
	}

	/* [한국어] 세 창 모두 쓰였거나 크기가 어느 범위에도 맞지 않는다. */
	return -EINVAL;
}

/* [한국어]
 * xgene_pcie_setup_ib_reg - dma-ranges 항목 하나를 인바운드 창에 새긴다
 *
 * @port: 컨트롤러 객체.
 * @range: DT 의 dma-ranges 에서 온 항목 하나.
 * @ib_reg_mask: 이미 쓰인 창 표시. 창을 고르면서 갱신된다.
 * @return: 없음 — 실패해도 경고만 남기고 조용히 돌아간다.
 *
 * 인바운드 창은 엔드포인트의 DMA 가 시스템 메모리에 닿는 길이다. 이 함수가
 * 그 길 하나를 뚫는다.
 * 창 번호에 따라 쓰는 레지스터가 완전히 달라지는 것이 특징이다.
 *  - 창 0: 루트 브리지 '설정공간' 의 BAR0/BAR1 을 직접 쓴다. 그래서 CSR 창이
 *    아니라 설정공간 창(cfg_base)에 writel 을 하며, 마스크는 별도 레지스터
 *    묶음에 16비트씩 흩어 넣는다(xgene_pcie_set_ib_mask). 이 BAR 를 나중에
 *    PCI 코어가 건드리지 못하도록 가리는 것이 xgene_pcie_hide_rc_bars() 다.
 *  - 창 1: CSR 창의 BAR/마스크 레지스터 한 쌍만 쓴다. 32비트만 다룬다.
 *  - 창 2: 같은 방식이되 상위 워드까지 써서 64비트 주소를 다룬다.
 * 세 경우 모두 마지막에 목적지 주소와 크기를 함께 새긴다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 맞는 창이 없으면 경고만 남기고 돌아간다 — 그 DMA 범위는 조용히
 * 동작하지 않게 된다.
 *
 * [상류 코드 관찰] 두 가지. 첫째, switch 에 default 가 없어 pim_reg 가 세 case
 * 밖에서는 설정되지 않는다. 다만 바로 위에서 음수를 걸러 냈고 창 선택 함수가
 * 0/1/2 아니면 음수만 돌려주므로 실제로 그런 경로는 없다. 둘째, CPU 주소를
 * pcie_bar_low_val() 에 넘길 때 (u32) 로 잘라 하위 32비트만 쓰는데, 상위 32비트는
 * 창 0 과 창 2 에서만 따로 써 준다 — 창 1 은 상위를 쓰지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_parse_map_dma_ranges() → [xgene_pcie_setup_ib_reg]
 *     → xgene_pcie_select_ib_reg(), pcie_bar_low_val(),
 *       xgene_pcie_set_ib_mask(), xgene_pcie_setup_pims()
 */
static void xgene_pcie_setup_ib_reg(struct xgene_pcie *port,
				    struct of_pci_range *range, u8 *ib_reg_mask)
{
	/* [한국어] 창 0 에서 BAR 를 직접 쓸 설정공간 창 주소. CSR 창이 아니라는 점이
	 * 이 함수에서 가장 헷갈리는 부분이다. */
	void __iomem *cfg_base = port->cfg_base;
	/* [한국어] 경고 로그 대상. */
	struct device *dev = port->dev;
	/* [한국어] 창 0 에서 BAR 를 쓸 최종 주소. */
	void __iomem *bar_addr;
	/* [한국어] 고른 창의 목적지 레지스터 묶음 오프셋. */
	u32 pim_reg;
	/* [한국어] 이 범위의 로컬(DDR) 쪽 시작 주소 — 인바운드에서는 이것이 '목적지' 다. */
	u64 cpu_addr = range->cpu_addr;
	/* [한국어] 이 범위의 PCI 쪽 시작 주소. */
	u64 pci_addr = range->pci_addr;
	/* [한국어] 범위 크기. */
	u64 size = range->size;
	/* [한국어] 크기 마스크에 활성화 비트를 얹은 값. 창 1 과 창 2 의 마스크
	 * 레지스터에 그대로 들어간다. */
	u64 mask = ~(size - 1) | EN_REG;
	/* [한국어] BAR 하위 4비트에 실을 타입 표시. 64비트 메모리 BAR 로 선언한다. */
	u32 flags = PCI_BASE_ADDRESS_MEM_TYPE_64;
	/* [한국어] 조립이 끝난 BAR 하위 워드 값. */
	u32 bar_low;
	/* [한국어] 고른 창 번호. */
	int region;

	/* [한국어] 크기에 맞는 빈 창을 고르고 예약한다. */
	region = xgene_pcie_select_ib_reg(ib_reg_mask, range->size);
	/* [한국어] 맞는 창이 없으면 */
	if (region < 0) {
		/* [한국어] 경고만 남기고 */
		dev_warn(dev, "invalid pcie dma-range config\n");
		/* [한국어] 이 범위를 포기한다. 오류로 만들지 않으므로 probe 는 계속된다. */
		return;
	}

	/* [한국어] DT 가 이 범위를 prefetchable 로 서술했다면 */
	if (range->flags & IORESOURCE_PREFETCH)
		/* [한국어] BAR 타입 표시에 그 성질을 더한다. */
		flags |= PCI_BASE_ADDRESS_MEM_PREFETCH;

	/* [한국어] 목적지(로컬) 주소의 하위 32비트에 타입 표시를 얹어 BAR 값을 만든다.
	 * 위 관찰대로 여기서 상위 32비트가 잘려 나가며, 그것은 case 별로 따로 쓴다. */
	bar_low = pcie_bar_low_val((u32)cpu_addr, flags);
	/* [한국어] 고른 창에 따라 쓰는 레지스터가 달라진다. */
	switch (region) {
	/* [한국어] 창 0 — 루트 브리지 설정공간의 BAR0/BAR1 을 그대로 쓴다. */
	case 0:
		/* [한국어] 마스크는 BAR 옆이 아니라 CSR 창의 별도 묶음에 16비트씩 흩어 넣는다. */
		xgene_pcie_set_ib_mask(port, BRIDGE_CFG_4, flags, size);
		/* [한국어] BAR0 의 주소를 계산한다. 대상이 CSR 창이 아니라 설정공간 창이므로
		 * 이 파일의 xgene_pcie_writel() 을 쓰지 못하고 아래에서 writel 을 직접 부른다. */
		bar_addr = cfg_base + PCI_BASE_ADDRESS_0;
		/* [한국어] BAR0 에 하위 워드를 쓴다. */
		writel(bar_low, bar_addr);
		/* [한국어] BAR1 자리(BAR0 + 4)에 상위 32비트를 쓴다. 64비트 BAR 는 연속된 두
		 * BAR 를 한 쌍으로 쓰기 때문이며, 상류 주석이 "BAR0-BAR1 을 변환에 쓴다" 고
		 * 말한 것이 바로 이 구조다. */
		writel(upper_32_bits(cpu_addr), bar_addr + 0x4);
		/* [한국어] 창 0 의 목적지 레지스터 묶음을 고른다. */
		pim_reg = PIM1_1L;
		break;
	/* [한국어] 창 1 — CSR 창의 BAR/마스크 한 쌍만 쓴다. */
	case 1:
		/* [한국어] BAR 하위 워드를 쓴다. 상위 워드를 쓰지 않는 것이 다른 두 창과 다르다. */
		xgene_pcie_writel(port, IBAR2, bar_low);
		/* [한국어] 마스크 하위 워드를 쓴다. 활성화 비트가 여기 들어 있다. */
		xgene_pcie_writel(port, IR2MSK, lower_32_bits(mask));
		/* [한국어] 창 1 의 목적지 레지스터 묶음을 고른다. */
		pim_reg = PIM2_1L;
		break;
	/* [한국어] 창 2 — 64비트 주소를 온전히 다룬다. */
	case 2:
		/* [한국어] BAR 하위 워드. */
		xgene_pcie_writel(port, IBAR3L, bar_low);
		/* [한국어] BAR 상위 워드 — 목적지 주소의 상위 32비트를 여기서 채운다. */
		xgene_pcie_writel(port, IBAR3L + 0x4, upper_32_bits(cpu_addr));
		/* [한국어] 마스크 하위 워드(활성화 비트 포함). */
		xgene_pcie_writel(port, IR3MSKL, lower_32_bits(mask));
		/* [한국어] 마스크 상위 워드. */
		xgene_pcie_writel(port, IR3MSKL + 0x4, upper_32_bits(mask));
		/* [한국어] 창 2 의 목적지 레지스터 묶음을 고른다. */
		pim_reg = PIM3_1L;
		break;
	}

	/* [한국어] 세 경우 공통으로, 이 창이 잡은 PCI 주소를 어디로 옮길지와 크기를
	 * 새긴다. 크기는 여기서도 ~(size-1) 마스크 형태로 넘긴다. */
	xgene_pcie_setup_pims(port, pim_reg, pci_addr, ~(size - 1));
}

/* [한국어]
 * xgene_pcie_parse_map_dma_ranges - DT 의 dma-ranges 를 훑어 인바운드 창을 채운다
 *
 * @port: 컨트롤러 객체.
 * @return: 0 성공, -EINVAL(dma-ranges 속성이 없음).
 *
 * 아웃바운드 쪽과 달리 PCI 코어가 미리 파싱해 둔 목록을 쓰지 않고, OF 범위
 * 파서를 직접 돌려 DT 노드에서 읽는다. 그래서 이 함수만 port->node 를 쓴다.
 * dma-ranges 가 아예 없으면 실패로 다룬다 — 인바운드 창이 없으면 엔드포인트의
 * DMA 가 시스템 메모리에 닿지 못해 장치가 쓸모없어지기 때문이다.
 * 반대로 개별 범위가 창에 들어가지 못하는 경우는 오류가 아니라 경고다
 * (xgene_pcie_setup_ib_reg 참조).
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_setup() → [xgene_pcie_parse_map_dma_ranges]
 *     → of_pci_dma_range_parser_init(), xgene_pcie_setup_ib_reg()
 */
static int xgene_pcie_parse_map_dma_ranges(struct xgene_pcie *port)
{
	/* [한국어] 이 컨트롤러의 DT 노드. probe 가 참조를 올려 담아 둔 것이다. */
	struct device_node *np = port->node;
	/* [한국어] 파서가 채워 줄 범위 하나. */
	struct of_pci_range range;
	/* [한국어] 범위 순회 상태를 담는 파서 객체. */
	struct of_pci_range_parser parser;
	/* [한국어] 로그 대상. */
	struct device *dev = port->dev;
	/* [한국어] 이미 쓰인 인바운드 창을 비트로 표시한다. 0 으로 시작해 창을 고를
	 * 때마다 갱신된다 — 이 변수 하나가 세 창의 배정 상태 전부다. */
	u8 ib_reg_mask = 0;

	/* [한국어] dma-ranges 를 읽을 준비를 한다. 속성이 없으면 실패를 돌려준다. */
	if (of_pci_dma_range_parser_init(&parser, np)) {
		/* [한국어] 인바운드 창 없이는 DMA 가 불가능하므로 */
		dev_err(dev, "missing dma-ranges property\n");
		/* [한국어] probe 를 실패시킨다. */
		return -EINVAL;
	}

	/* Get the dma-ranges from DT */
	/* [한국어] 옆의 상류 주석대로 DT 의 dma-ranges 를 하나씩 꺼내 온다. */
	for_each_of_pci_range(&parser, &range) {
		/* [한국어] 로그에 쓸 이 범위의 끝 주소. 시작 + 크기 - 1 이다. */
		u64 end = range.cpu_addr + range.size - 1;

		/* [한국어] 어떤 CPU 주소 구간이 어떤 PCI 주소로 대응되는지 남긴다. 인바운드 창은
		 * 눈에 보이지 않아 디버깅이 어렵기 때문에 이 로그가 유용하다. */
		dev_dbg(dev, "0x%08x 0x%016llx..0x%016llx -> 0x%016llx\n",
			range.flags, range.cpu_addr, end, range.pci_addr);
		/* [한국어] 이 범위를 알맞은 인바운드 창에 새긴다. 실패는 그 안에서 경고로 처리된다. */
		xgene_pcie_setup_ib_reg(port, &range, &ib_reg_mask);
	}
	/* [한국어] 모든 범위 처리 완료. */
	return 0;
}

/* [한국어]
 * xgene_pcie_clear_config - 펌웨어가 남긴 창 설정을 통째로 지운다
 *
 * @port: 컨트롤러 객체.
 * @return: 없음.
 *
 * 옆의 상류 주석대로, 부트로더나 펌웨어가 이미 설정해 둔 창이 남아 있을 수 있다.
 * 그것을 그대로 두고 새 설정을 얹으면 쓰지 않는 창이 살아 있는 채로 남아
 * 엉뚱한 주소를 잡을 수 있으므로, 새로 새기기 전에 백지로 만든다.
 * 지우는 범위는 인바운드 창 1 의 목적지부터 설정공간 창 제어까지다 — 이 파일이
 * 정의한 오프셋 기준으로 인바운드 창 셋, 아웃바운드 창 셋, 설정공간 창이 모두
 * 그 사이에 들어간다. 사이에 있는 이름 없는 레지스터들도 함께 0 이 된다.
 * RTDID 는 이 범위 밖(더 뒤)이라 지워지지 않는데, 설정공간 접근 때마다
 * 갱신되므로 초기값이 의미가 없기 때문으로 읽힌다.
 * 실행 컨텍스트: probe 경로의 가장 앞. 잠들지 않는다.
 *
 * 호출 체인:
 *   xgene_pcie_setup() → [xgene_pcie_clear_config] → xgene_pcie_writel()
 */
/* clear BAR configuration which was done by firmware */
static void xgene_pcie_clear_config(struct xgene_pcie *port)
{
	/* [한국어] 순회할 오프셋. 바이트 단위다. */
	int i;

	/* [한국어] 인바운드 창 1 의 목적지 레지스터부터 설정공간 창 제어 레지스터까지,
	 * 4바이트(32비트 레지스터 하나)씩 건너뛰며 훑는다. */
	for (i = PIM1_1L; i <= CFGCTL; i += 4)
		/* [한국어] 0 으로 덮는다 — 창 마스크가 0 이 되면 활성화 비트도 함께 꺼지므로,
		 * 이 한 번의 쓰기로 그 창이 죽는다. */
		xgene_pcie_writel(port, i, 0);
}

/* [한국어]
 * xgene_pcie_setup - 컨트롤러를 백지에서 다시 세우고 링크를 확인한다
 *
 * @port: 컨트롤러 객체.
 * @return: 0 성공, 창 설정 단계의 오류.
 *
 * DT 경로에서 하드웨어를 실제로 구성하는 함수다. 순서가 의미를 갖는다.
 *  1) 펌웨어가 남긴 창 설정을 모두 지워 백지로 만든다.
 *  2) 루트 브리지가 스스로 보고할 벤더/디바이스 ID 를 새긴다. 이것이 없으면
 *     커널이 이 브리지를 제대로 알아보지 못한다.
 *  3) 아웃바운드 창(드라이버가 장치에 닿는 길)을 새긴다.
 *  4) 인바운드 창(장치가 메모리에 닿는 길)을 새긴다.
 *  5) 링크 상태를 읽어 로그로 남긴다.
 * 링크가 죽어 있어도 실패로 다루지 않는다 — 슬롯이 비어 있는 것이 정상인 경우가
 * 많고, 그때는 그 아래에서 장치가 열거되지 않을 뿐이다.
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트). 잠들지 않는다.
 * 에러 경로: 창 설정이 실패하면 그대로 되돌아가 probe 가 실패한다.
 *
 * 호출 체인:
 *   xgene_pcie_probe() → [xgene_pcie_setup]
 *     → xgene_pcie_clear_config(), xgene_pcie_map_ranges(),
 *       xgene_pcie_parse_map_dma_ranges(), xgene_pcie_linkup()
 */
static int xgene_pcie_setup(struct xgene_pcie *port)
{
	/* [한국어] 로그 대상. */
	struct device *dev = port->dev;
	/* [한국어] val 은 ID 레지스터에 쓸 값, lanes/speed 는 링크 상태를 받을 곳이다.
	 * 링크가 죽어 있으면 xgene_pcie_linkup() 이 두 변수를 건드리지 않으므로,
	 * 여기서 0 으로 초기화해 두는 것이 쓰레기 값 출력을 막는다. */
	u32 val, lanes = 0, speed = 0;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 먼저 펌웨어가 남긴 설정을 지워 백지에서 시작한다. */
	xgene_pcie_clear_config(port);

	/* setup the vendor and device IDs correctly */
	/* [한국어] 옆의 상류 주석대로 벤더/디바이스 ID 를 바로잡는다. 설정공간 헤더에서
	 * 벤더 ID 가 하위 16비트, 디바이스 ID 가 상위 16비트를 차지하므로 그 배치대로
	 * 조립한다. 벤더 상수의 실제 값은 include/linux/pci_ids.h 에 있어 확인 못 함. */
	val = (XGENE_PCIE_DEVICEID << 16) | PCI_VENDOR_ID_AMCC;
	/* [한국어] 루트 브리지 설정공간 창의 첫 워드에 써 넣는다. */
	xgene_pcie_writel(port, BRIDGE_CFG_0, val);

	/* [한국어] 아웃바운드 창과 설정공간 창을 새긴다. */
	ret = xgene_pcie_map_ranges(port);
	/* [한국어] 다룰 수 없는 자원이 있으면 여기서 멈춘다. */
	if (ret)
		return ret;

	/* [한국어] 인바운드 창을 새긴다. */
	ret = xgene_pcie_parse_map_dma_ranges(port);
	/* [한국어] dma-ranges 가 없으면 여기서 멈춘다. */
	if (ret)
		return ret;

	/* [한국어] 링크 상태를 읽는다. 이 시점에는 이미 링크 훈련이 끝나 있다는 전제다
	 * — 이 드라이버에는 링크를 출발시키거나 기다리는 코드가 없다. */
	xgene_pcie_linkup(port, &lanes, &speed);
	/* [한국어] 링크가 없으면 */
	if (!port->link_up)
		/* [한국어] 그 사실만 알리고 성공으로 돌아간다. 슬롯이 비어 있는 것이 정상일 수
		 * 있기 때문이다. */
		dev_info(dev, "(rc) link down\n");
	/* [한국어] 링크가 있으면 */
	else
		/* [한국어] 폭과 세대를 알린다. 속도 인코딩이 0 부터 시작하므로 1 을 더해
		 * 사람이 읽는 'gen-N' 으로 바꾼다. */
		dev_info(dev, "(rc) x%d gen-%d link up\n", lanes, speed + 1);
	/* [한국어] 구성 완료. */
	return 0;
}

/* [한국어] DT 경로의 설정공간 접근 연산 표. ACPI 경로의 두 표와 map_bus/read 는
 * 같고 write 만 다르다(위 v1 연산 표의 관찰 참조). */
static struct pci_ops xgene_pcie_ops = {
	/* [한국어] 접근 주소를 만드는 후크. RTDID 갱신까지 겸한다. */
	.map_bus = xgene_pcie_map_bus,
	/* [한국어] RRS 우회가 들어간 읽기. */
	.read = xgene_pcie_config_read32,
	/* [한국어] 32비트 접근만 되는 하드웨어를 위한 표준 쓰기 — 1/2바이트 쓰기를
	 * 32비트 읽고-고쳐-쓰기로 흉내 낸다. */
	.write = pci_generic_config_write32,
};

/* [한국어]
 * xgene_check_pcie_msi_ready - MSI 드라이버가 먼저 준비되었는지 확인한다
 *
 * @return: 준비되었거나 애초에 필요 없으면 참, 아직이면 거짓.
 *
 * 이 SoC 의 MSI 는 별도 드라이버(같은 디렉터리의 X-Gene MSI 드라이버)가 담당한다.
 * 그 드라이버가 IRQ 도메인을 등록하기 전에 이 호스트 브리지가 버스를 열거해
 * 버리면, 그 위에 붙은 장치들이 MSI 를 배정받지 못한다. 그래서 probe 맨 앞에서
 * 준비 여부를 확인하고, 아직이면 나중에 다시 시도하도록 미룬다.
 * 참을 돌려주는 경우가 셋이다 — MSI 지원이 아예 빌드에서 빠졌거나, DT 에 그
 * 노드가 없거나(그 SoC 가 아니거나 MSI 를 안 쓰는 구성), 도메인이 이미 등록되어
 * MSI 부모 역할을 할 준비가 된 경우다. 앞의 둘은 '기다릴 대상이 없다' 는 뜻이라
 * 참으로 처리한다.
 * 실행 컨텍스트: probe 진입 직후(프로세스 컨텍스트). DT 순회가 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   xgene_pcie_probe() → [xgene_check_pcie_msi_ready]
 *     → of_find_compatible_node(), irq_find_matching_host(), of_node_put()
 */
static bool xgene_check_pcie_msi_ready(void)
{
	/* [한국어] 찾을 MSI 컨트롤러의 DT 노드. */
	struct device_node *np;
	/* [한국어] 그 노드에 등록된 IRQ 도메인. */
	struct irq_domain *d;

	/* [한국어] MSI 드라이버가 아예 빌드되지 않았다면 기다릴 대상이 없다. */
	if (!IS_ENABLED(CONFIG_PCI_XGENE_MSI))
		/* [한국어] 준비된 것으로 보고 그대로 진행시킨다. */
		return true;

	/* [한국어] DT 에서 MSI 컨트롤러 노드를 찾는다. 이 호출은 찾은 노드의 참조를
	 * 하나 올려 주므로 아래에서 반드시 놓아야 한다. */
	np = of_find_compatible_node(NULL, NULL, "apm,xgene1-msi");
	/* [한국어] 노드가 없으면 이 시스템은 그 MSI 컨트롤러를 쓰지 않는다. */
	if (!np)
		/* [한국어] 기다릴 대상이 없으므로 준비된 것으로 본다. */
		return true;

	/* [한국어] 그 노드에 PCI MSI 용 도메인이 이미 등록되었는지 묻는다. 등록 전이면
	 * NULL 이 돌아온다. */
	d = irq_find_matching_host(np, DOMAIN_BUS_PCI_MSI);
	/* [한국어] 노드 참조를 놓는다. 아래에서 d 만 쓰므로 여기서 놓아도 안전하다. */
	of_node_put(np);

	/* [한국어] 도메인이 있고, 그것이 MSI 부모 역할을 할 수 있어야 준비된 것이다.
	 * 도메인만 있고 부모 역할이 아직 안 되는 중간 상태를 걸러 내는 검사다. */
	return d && irq_domain_is_msi_parent(d);
}

/* [한국어]
 * xgene_pcie_probe - DT 경로의 진입점. 자원과 클럭을 갖추고 창을 세운 뒤 위임한다
 *
 * @pdev: DT 와 매칭된 플랫폼 디바이스.
 * @return: 0 성공, -EPROBE_DEFER(MSI 드라이버 대기), -ENOMEM, 하위 단계의 오류.
 *
 * 순서가 의미를 갖는다.
 *  1) MSI 드라이버가 준비되었는지 먼저 본다. 아직이면 아무것도 하지 않고 미룬다
 *     — 자원을 잡기 전에 확인해야 되돌릴 것이 없다.
 *  2) 브리지와 사설 데이터를 한 덩어리로 할당한다. 이 배치 덕분에
 *     pci_host_bridge_from_priv() 로 브리지를 되찾을 수 있고,
 *     xgene_pcie_map_ranges() 가 창 목록을 볼 수 있다.
 *  3) DT 노드 참조를 잡고 버전을 v1 로 고정한다.
 *  4) 레지스터 창을 매핑하고, 클럭을 켜고, 창을 새긴다.
 *  5) 설정공간 접근 연산 표와 사설 데이터를 브리지에 걸고 코어에 넘긴다.
 * 마지막 두 줄이 4번 뒤에 오는 것이 중요하다 — 코어에 넘기는 순간 버스 스캔이
 * 시작되어 설정공간 접근이 쏟아지는데, 그전에 설정공간 창이 살아 있어야 한다.
 * 실행 컨텍스트: 드라이버 바인딩(프로세스 컨텍스트). 클럭과 DT 순회에서
 * 잠들 수 있다.
 * 에러 경로: 모든 실패가 곧장 되돌아가고 devm 자원은 자동 정리된다. 다만
 * 클럭과 DT 노드 참조는 devm 이 아니어서 정리되지 않는다(위 관찰들 참조).
 *
 * 호출 체인:
 *   (플랫폼 버스의 DT 매칭) → [xgene_pcie_probe]
 *     → xgene_check_pcie_msi_ready(), devm_pci_alloc_host_bridge(),
 *       xgene_pcie_map_reg(), xgene_pcie_init_port(), xgene_pcie_setup(),
 *       pci_host_probe()
 */
static int xgene_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 할당의 기준 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 DT 노드. */
	struct device_node *dn = dev->of_node;
	/* [한국어] 브리지 뒤에 붙을 컨트롤러 객체. */
	struct xgene_pcie *port;
	/* [한국어] 만들 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] MSI 드라이버가 아직이면 */
	if (!xgene_check_pcie_msi_ready())
		/* [한국어] -EPROBE_DEFER 로 물러난다. 커널이 나중에 다시 부르며, 그때 MSI
		 * 도메인이 등록되어 있으면 진행된다. 이 검사를 자원 획득보다 앞에 둔 덕분에
		 * 되돌릴 것이 없다. */
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "MSI driver not ready\n");

	/* [한국어] 브리지와 사설 영역을 한 번에 할당한다. 이 한 덩어리 할당이
	 * pci_host_bridge_priv() 와 pci_host_bridge_from_priv() 양방향 이동의 근거다.
	 * 그리고 이 배치가 DT 경로와 ACPI 경로의 결정적 차이이기도 하다 —
	 * ACPI 쪽은 devm_kzalloc 으로 따로 잡아 브리지와 이어지지 않는다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*port));
	/* [한국어] 메모리 부족. */
	if (!bridge)
		return -ENOMEM;

	/* [한국어] 브리지 뒤의 사설 영역을 가리킨다. */
	port = pci_host_bridge_priv(bridge);

	/* [한국어] DT 노드 참조를 하나 올려 보관한다. 인바운드 창 설정이 이 노드에서
	 * dma-ranges 를 읽기 때문이다. 놓아 주는 짝이 없는 점은 필드 주석의 관찰 참조. */
	port->node = of_node_get(dn);
	/* [한국어] 로그와 자원 조회의 기준 device 를 심는다. */
	port->dev = dev;
	/* [한국어] DT 경로는 언제나 v1 로 고정한다. 버전을 DT 에서 읽거나 하드웨어에서
	 * 알아내는 코드는 없다 — v2 는 ACPI 경로에서만 나타난다. */
	port->version = XGENE_PCIE_IP_VER_1;

	/* [한국어] csr 와 cfg 두 레지스터 창을 매핑한다. */
	ret = xgene_pcie_map_reg(port, pdev);
	/* [한국어] 매핑 실패. */
	if (ret)
		return ret;

	/* [한국어] 클럭을 켠다. 창을 새기기 전에 하드웨어가 살아 있어야 한다. */
	ret = xgene_pcie_init_port(port);
	/* [한국어] 클럭 실패. */
	if (ret)
		return ret;

	/* [한국어] 창을 모두 새기고 링크를 확인한다. */
	ret = xgene_pcie_setup(port);
	/* [한국어] 구성 실패. */
	if (ret)
		return ret;

	/* [한국어] 설정공간 접근 함수들이 bus->sysdata 로 되찾을 객체를 심는다.
	 * pcie_bus_to_port() 의 DT 분기가 이 값을 그대로 읽는다. */
	bridge->sysdata = port;
	/* [한국어] 설정공간 접근 연산 표를 건다. 이 두 줄을 창 설정 뒤에 두는 이유는
	 * 아래 호출이 곧바로 버스 스캔을 시작하기 때문이다. */
	bridge->ops = &xgene_pcie_ops;

	/* [한국어] 코어에 넘겨 버스를 스캔하게 한다. 이 안에서 일어나는 모든 설정공간
	 * 접근이 xgene_pcie_map_bus() 를 지난다. */
	return pci_host_probe(bridge);
}

/* [한국어] DT compatible 문자열 매칭 표. 이 드라이버가 DT 로 상대하는 하드웨어는
 * 하나뿐이고, 세대별 분기가 없어 .data 도 달려 있지 않다 — 그래서 DT 경로의
 * IP 버전이 코드에 상수로 박혀 있다. */
static const struct of_device_id xgene_pcie_match_table[] = {
	/* [한국어] X-Gene PCIe 호스트 브리지. */
	{.compatible = "apm,xgene-pcie",},
	/* [한국어] 표의 끝을 알리는 빈 항목. 이것이 없으면 매칭 코드가 배열을 넘어간다. */
	{},
};

/* [한국어] 플랫폼 드라이버 서술. 이 컨트롤러는 PCI 장치가 아니라 SoC 내부
 * 블록이므로 플랫폼 드라이버로 등록된다. */
static struct platform_driver xgene_pcie_driver = {
	/* [한국어] 드라이버 속성 묶음. */
	.driver = {
		/* [한국어] 드라이버 이름. sysfs 경로와 로그에 쓰인다. */
		.name = "xgene-pcie",
		/* [한국어] 위 DT 매칭 표를 건다. */
		.of_match_table = xgene_pcie_match_table,
		/* [한국어] sysfs 로 수동 바인드/언바인드하는 속성을 만들지 않는다. 이 드라이버에
		 * remove 경로가 없고 클럭·DT 참조·창 설정을 되돌리는 코드도 없으므로,
		 * 언바인드를 아예 막는 것이 안전하기 때문이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] 바인딩 시 불릴 진입점. */
	.probe = xgene_pcie_probe,
};
/* [한국어] 커널에 내장되는 드라이버로 등록한다. 모듈 진입/종료 함수를 만들지
 * 않으므로 MODULE_LICENSE 같은 모듈용 매크로도 이 파일에 없다. */
builtin_platform_driver(xgene_pcie_driver);
