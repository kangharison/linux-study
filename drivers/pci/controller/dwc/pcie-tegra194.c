// SPDX-License-Identifier: GPL-2.0+
/*
 * PCIe host controller driver for the following SoCs
 * Tegra194
 * Tegra234
 *
 * Copyright (C) 2019-2022 NVIDIA Corporation.
 *
 * Author: Vidya Sagar <vidyas@nvidia.com>
 */

/*
 * [한국어 설명] NVIDIA Tegra194/234 DesignWare PCIe 접착 드라이버 (pcie-tegra194.c)
 *
 * === 파일의 역할 ===
 * Tegra194 와 Tegra234 의 PCIe 컨트롤러를 구동한다. 앞 세대와 결정적으로
 * 다른 점은 **컨트롤러 코어가 NVIDIA 자체 설계가 아니라 Synopsys DesignWare
 * (DWC) IP** 라는 것이다. 그래서 이 파일은 컨트롤러를 처음부터 끝까지
 * 구현하지 않고, DWC 공통 코드(pcie-designware.c, pcie-designware-host.c,
 * pcie-designware-ep.c)가 하지 못하는 일만 맡는 **접착 계층** 이다.
 *
 * 이 파일이 실제로 하는 일은 네 갈래다. 첫째, DWC 코어가 요구하는 콜백을
 * 채운다(링크를 올리고 내리고 상태를 보고하는 세 개, 그리고 호스트 초기화
 * 하나). 둘째, IP 코어 바깥에 NVIDIA 가 덧붙인 APPL 레지스터 블록을
 * 프로그래밍한다 -- 인터럽트 분배, RP/EP 모드 선택, DBI 와 iATU 블록의
 * 기준 주소 알려 주기, PERST 와 CLKREQ 핀 제어가 여기 있다. 셋째, SoC
 * 고유의 전원 자원을 다룬다(regulator, 클록, 리셋, p2u PHY, 그리고 UPHY 를
 * 켜 달라고 BPMP 펌웨어에 보내는 메시지). 넷째, 앞 세대에 없던
 * **엔드포인트 모드** 를 지원한다.
 *
 * 자체 IP 시절의 모습은 drivers/pci/controller/pci-tegra.c 에 있다. 그
 * 파일과 이 파일을 견주면 IP 를 갈아탈 때 드라이버에서 무엇이 사라지는지가
 * 그대로 보이며, 아래 부가 절에서 항목별로 짚는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층이 세 겹이다.
 *
 *   PCI 코어 (drivers/pci)
 *     ↑
 *   DWC 공통 계층 (drivers/pci/controller/dwc/pcie-designware-host.c 등)
 *     -- config 접근, iATU 주소 변환, iMSI-RX 기반 MSI, INTx 도메인을 담당
 *     ↑
 *   [이 파일] SoC 접착 계층
 *     -- APPL 레지스터, 전원/클록/리셋, UPHY(BPMP), 링크 기동 절차
 *     ↑
 *   Tegra194/234 하드웨어
 *
 * 이 파일은 위아래 양쪽으로 붙는다. 아래로는 하드웨어를 직접 만지고,
 * 위로는 struct dw_pcie 를 자기 구조체 안에 품어(struct tegra_pcie_dw 의
 * pci 필드) DWC 코어가 그것을 통해 이 드라이버를 부르게 한다.
 * 두 방향 변환은 to_tegra_pcie(container_of)와 to_dw_pcie_from_pp 다.
 *
 * 부팅 시 호출 흐름 (RC 모드):
 *   플랫폼 버스
 *     → tegra_pcie_dw_probe
 *         → tegra_pcie_dw_parse_dt        (ASPM 파라미터, 레인 수, 컨트롤러 ID)
 *         → 자원 획득 (regulator, 클록, 리셋, APPL/DBI/atu_dma 창, p2u PHY, BPMP)
 *         → tegra_pcie_config_rp
 *             → tegra_pcie_init_controller
 *                 → tegra_pcie_config_controller  (BPMP, 전원, PHY, APPL 설정)
 *                 → dw_pcie_host_init             ← **여기서 DWC 코어로 넘어간다**
 *                     → tegra_pcie_dw_host_init   (host_ops.init 콜백으로 되돌아옴)
 *                     → dw_pcie_setup_rc, MSI 도메인, iATU 창 -- 전부 코어가 처리
 *                     → tegra_pcie_dw_start_link  (dw_pcie_ops.start_link 콜백)
 *
 * **제어가 드라이버 → 코어 → 드라이버로 왕복하는 것이 접착 계층의 특징**
 * 이다. dw_pcie_host_init 을 부르고 나면 그 안에서 이 파일의 콜백이 다시
 * 불린다. 앞 세대 pci-tegra.c 처럼 한 함수가 처음부터 끝까지 순서를 쥐고
 * 있지 않다.
 *
 * 실행 컨텍스트: probe 와 PM 콜백은 프로세스 컨텍스트다. 인터럽트는 모드에
 * 따라 갈리는데, RC 는 일반 핸들러 하나(tegra_pcie_rp_irq_handler)이고
 * EP 는 하드 핸들러와 스레드 핸들러의 짝(tegra_pcie_ep_hard_irq /
 * tegra_pcie_ep_irq_thread)이다. EP 가 스레드를 쓰는 것은 링크가 올라온 뒤
 * 잠들 수 있는 작업(LTR 메시지 폴링, 전원 관리)을 해야 하기 때문이다.
 * PERST GPIO 인터럽트도 스레드 핸들러다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 **의존하는** 쪽:
 *   pcie-designware.h / -host.c / -ep.c : 이 파일의 존재 이유. 아래 세
 *     콜백 묶음을 통해 맞물린다.
 *       struct dw_pcie_ops      -- 멤버 여덟 개 중 셋만 채운다
 *         (link_up, start_link, stop_link). cpu_addr_fixup 이나 read_dbi
 *         계열을 채우지 않는다는 것은 DBI 접근과 주소 변환이 표준 그대로라는
 *         뜻이다.
 *       struct dw_pcie_host_ops -- 멤버 다섯 개 중 하나만 채운다(init).
 *         특히 **msi_init 을 채우지 않는다** -- 그래서 DWC 코어가 자기
 *         iMSI-RX 경로를 그대로 쓴다. 이 대비는 pci-keystone.c 를 보면
 *         분명한데, 그쪽은 msi_init 을 채워 코어의 iMSI-RX 를 건너뛴다.
 *       struct dw_pcie_ep_ops   -- 멤버 다섯 개 중 둘(raise_irq, get_features).
 *   drivers/pci/access.c : pci_generic_config_read / _write. 이 파일의
 *     read/write 콜백이 우회 검사만 하고 그대로 넘긴다.
 *   soc/tegra/bpmp.h, bpmp-abi.h : UPHY 제어를 펌웨어에 요청하는 메시지
 *     인터페이스. **이 두 헤더는 이 스파스 체크아웃에 없다**
 *     (include/soc/tegra 가 존재하지 않음). tegra_bpmp_transfer, MRQ_UPHY,
 *     CMD_UPHY_PCIE_CONTROLLER_STATE 등의 정의를 확인하지 못했으므로
 *     쓰임새로만 설명한다.
 *   interconnect 프레임워크 : 링크 속도와 폭에 맞춰 메모리 대역폭을
 *     요청한다(tegra_pcie_icc_set). 앞 세대에 없던 연결이다.
 *   clk / reset / regulator / phy / gpio / pinctrl 프레임워크.
 *
 * 이 파일에 **의존하는** 쪽: 내보내는 심볼이 없다. DWC 코어가 콜백으로,
 * PCI 코어가 pci_ops 로 간접 진입할 뿐이다.
 *
 * 데이터 흐름 -- 레지스터 창이 셋이다:
 *   appl_base (APPL)  : NVIDIA 가 IP 바깥에 덧붙인 접착 레지스터. 이 파일이
 *     직접 읽고 쓰는 거의 전부가 여기다. appl_readl / appl_writel 로 접근한다.
 *   pci.dbi_base (DBI): DWC IP 의 설정 레지스터. 이 파일은 주소를 직접
 *     계산하지 않고 dw_pcie_readl_dbi 계열 도우미로만 접근한다.
 *   pci.atu_base      : iATU 와 eDMA 레지스터. 이 파일은 주소만 잡아 코어에
 *     넘기고 내용은 건드리지 않는다.
 *
 * 공유 상태: struct tegra_pcie_dw 가 전부다. 그 안에 struct dw_pcie 를
 * 값으로 품고 있어, 코어가 dw_pcie 포인터를 넘겨 주면 container_of 로 이
 * 구조체를 되찾는다. SoC 와 모드(RC/EP)별 차이는 struct tegra_pcie_dw_of_data
 * 하나에 모여 있고, 장치 트리 compatible 이 그중 하나를 고른다.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수:
 *   tegra_pcie_dw_start_link      : dw_pcie_ops.start_link 콜백. PERST 를
 *     흔들고 LTSSM 을 켜 링크를 세운다. 링크가 안 서면 DLF 를 끄고 **한 번
 *     더 시도** 하는 우회가 들어 있다.
 *   tegra_pcie_dw_host_init       : dw_pcie_host_ops.init 콜백. 브리지 창
 *     디코딩, RRS 응답 형식, Gen3/Gen4 이퀄라이저 프리셋, ASPM 파라미터를
 *     설정한다. probe 와 resume 이 공유한다.
 *   tegra_pcie_config_controller  : 전원 인가 순서 전체. BPMP 로 컨트롤러를
 *     켜고, regulator 와 클록을 올리고, PHY 를 켜고, APPL 에 DBI/iATU 기준
 *     주소를 알려 준다.
 *   tegra_pcie_rp_irq_handler     : RC 인터럽트. APPL 의 2단 상태 레지스터
 *     (L0 → L1_x)를 훑어 링크 상태 변화, 대역폭 변화, CDM 검사 결과를 처리한다.
 *   tegra_pcie_ep_hard_irq / _ep_irq_thread : EP 인터럽트의 짝. 하드 쪽이
 *     상태를 읽어 스레드를 깨울지 정하고, 스레드 쪽이 링크업 통보와 LTR
 *     메시지를 처리한다.
 *   pex_ep_event_pex_rst_deassert : EP 모드의 실질적인 초기화 전체. 호스트가
 *     PERST 를 풀면 이 함수가 컨트롤러를 처음부터 세운다.
 *   tegra_pcie_bpmp_set_ctrl_state / _set_pll_state : UPHY 를 펌웨어에
 *     맡기는 두 요청. 앞 세대가 PADS 레지스터를 직접 만지던 자리를 대신한다.
 *   tegra_pcie_icc_set            : 링크 속도와 폭을 읽어 메모리 대역폭과
 *     코어 클록을 그에 맞게 조정한다.
 *
 * 구조체:
 *   struct tegra_pcie_dw          : 드라이버 전역 상태. **struct dw_pcie 를
 *     값으로 품는다** -- 이 한 줄이 DWC 코어와의 접합점이다.
 *   struct tegra_pcie_dw_of_data  : SoC 와 모드별 차이. IP 버전, RC/EP 모드,
 *     하드웨어 결함 우회 여부 불리언 다섯, CDM 인터럽트 비트, Gen4 프리셋
 *     벡터, N_FTS 값을 담는다. 이 파일의 모든 세대 분기가 여기를 통과한다.
 *   tegra_pcie_epc_features       : EP 모드에서 BAR 배치를 EPC 코어에 알린다.
 *     BAR2 는 MSI-X 표와 PBA 가, BAR4 는 DMA 레지스터가 차지하고 있어
 *     예약(BAR_RESERVED)으로 표시한다.
 *
 * === 자체 IP 에서 DesignWare 로 -- pci-tegra.c 와의 대비 ===
 * 이 파일을 읽는 가장 좋은 방법은 앞 세대와 견주는 것이다.
 *
 * **사라진 것** (앞 세대에 있었으나 이제 DWC 코어가 대신한다):
 *   config 주소 계산 : pci-tegra.c 는 자기만의 주소 인코딩을 풀어내고
 *     (tegra_pcie_conf_offset), 4KiB 창을 접근할 때마다 옮기는
 *     map_bus 를 갖고 있었다. 이 파일의 map_bus 는
 *     dw_pcie_own_conf_map_bus(pcie-designware-host.c:1951) 한 줄이다.
 *     남아 있는 read/write 콜백은 주소를 계산하지 않으며, ASPM-L1 상태에서
 *     접근하면 시스템이 멈추는 레지스터 하나를 건너뛰는 우회일 뿐이다.
 *   아웃바운드 주소 변환 : 앞 세대의 tegra_pcie_setup_translations 는 AXI
 *     BAR 여섯 개와 FPCI BAR 여섯 개를 손으로 짝지어 채웠다. 이 파일에는
 *     그런 함수가 아예 없다. iATU 를 코어가 관리하고, 이 파일은 APPL 에
 *     iATU 블록의 물리 기준 주소만 알려 준다(APPL_CFG_IATU_DMA_BASE_ADDR).
 *   MSI 구현 전체 : 앞 세대는 비트맵, irq_chip, 도메인 생성, 목적지 페이지
 *     DMA 할당, 체인 핸들러를 모두 직접 가졌다. 이 파일에는 MSI 벡터를
 *     다루는 코드가 한 줄도 없다. probe 에서 pp->num_vectors 를 지정하고,
 *     APPL 쪽에서 MSI 인터럽트 전달을 켜 주는 것이 전부다
 *     (tegra_pcie_enable_msi_interrupts).
 *   PHY/PLL 직접 제어 : 앞 세대의 tegra_pcie_phy_enable 은 PADS 레지스터로
 *     PLL 을 재우고 깨우고 잠금을 폴링했다. 여기서는 UPHY 를 드라이버가
 *     만지지 않고 BPMP 펌웨어에 요청한다.
 *   레인 배분(xbar) : 앞 세대는 포트 여러 개에 레인을 어떻게 나눌지를
 *     드라이버가 정했다. 이 파일은 컨트롤러 인스턴스마다 별도의 장치
 *     노드를 가지므로 그 개념 자체가 없다.
 *
 * **새로 생긴 것**:
 *   콜백 채우기 : 위의 타 모듈 절에 적은 세 묶음. 무엇을 채우지 **않았는가**
 *     가 무엇을 채웠는가 만큼 중요하다 -- 채우지 않은 자리는 코어의 기본
 *     동작을 그대로 쓴다는 선언이기 때문이다.
 *   엔드포인트 모드 : 앞 세대에 없던 기능. 이 파일 분량의 상당 부분이
 *     EP 경로이며, PERST GPIO 인터럽트를 받아 컨트롤러를 세우고 허무는
 *     구조다(pex_ep_event_pex_rst_assert / _deassert).
 *   BPMP 연동, interconnect 대역폭 요청, CDM 레지스터 무결성 검사,
 *     RAS-DES 이벤트 카운터 기반 ASPM 통계(debugfs).
 *
 * **양쪽에 다 있는 것**: SoC 별 기술자 구조체, regulator 다루기,
 * debugfs 로 상태 보여 주기, 그리고 무엇보다 **IP 코어 바깥의 벤더 접착
 * 레지스터 블록** 이다. 앞 세대의 AFI 가 여기서는 APPL 로 이름만 바뀌었다.
 * IP 를 사 오더라도 칩 바깥의 배선, 전원, 핀 제어는 여전히 벤더 몫이라는
 * 사실이 이 대비에서 가장 분명하게 드러난다.
 *
 * === 값의 근거에 대하여 ===
 * 확인할 수 없는 것을 단정하지 않기 위해 아래 원칙으로 적었다.
 *   TEGRA194_DWC_IP_VER 과 TEGRA234_DWC_IP_VER 에 들어가는
 *     DW_PCIE_VER_500A / DW_PCIE_VER_562A 는 **이 트리에 정의가 없다.**
 *     of_data 의 version 필드를 채워 SoC 를 구별하는 데만 쓰이므로,
 *     그 용도로만 설명한다.
 *   PCIE_PME_TO_L2_TIMEOUT_US 와 MAX_MSI_IRQS 도 이 트리에 정의가 없다.
 *     각각 L2 진입 폴링 상한과 MSI 벡터 수 상한으로 쓰이는 것만 확인된다.
 *   APPL 레지스터의 오프셋과 비트 위치는 이 파일의 정의가 유일한 근거다.
 *     공개 문서가 트리에 없으므로, 각 비트가 어떻게 쓰이는지(어느 함수가
 *     세우고 지우는지, 어떤 조건에서 읽는지)로만 설명한다.
 *   Gen4 프리셋 벡터(0x360, 0x340)와 N_FTS 값(52, 80)은 위의 원문 주석이
 *     어느 프리셋이 켜지는지를 밝히고 있어 그 범위에서만 적는다.
 *   커널 공통 PCI 상수(PCI_EXP_LNKSTA_ 계열 등)를 정의한 헤더도 이 스파스
 *     체크아웃에 없다. 쓰임새로만 설명한다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 부르지 않는다(이 트리에서 전수
 * 확인: 0건). 이 드라이버가 세우는 것은 버스다.
 *
 * 다만 앞 세대와 견주면 NVMe 독자에게 의미 있는 변화가 뚜렷하다. 앞 세대의
 * 상한이 Gen2 였던 데 비해 이쪽은 **Gen4** 를 다룬다 -- 코어 클록 표
 * (pcie_gen_freq)가 Gen4 까지 있고, host_init 이 클록을 GEN4_CORE_CLK_FREQ 로
 * 올리며, Gen3/Gen4 이퀄라이저 프리셋을 프로그래밍하는 함수가 따로 있다.
 * 또 tegra_pcie_icc_set 이 협상된 속도와 폭을 읽어 메모리 대역폭을 그에
 * 맞춰 요청하는데, 이는 고속 SSD 가 실제로 대역폭을 낼 수 있으려면 PCIe
 * 링크뿐 아니라 SoC 내부 메모리 경로도 함께 열려야 하기 때문이다.
 * 그 위에 붙는 NVMe SSD 의 실질 성능에 이 파일이 관여하는 지점은 그 둘이다.
 *
 * 한편 이 파일은 반대 방향도 지원한다 -- EP 모드로 동작하면 Tegra 자신이
 * 다른 호스트에 매달리는 엔드포인트가 된다. 그때 이 파일이 제공하는 것은
 * PCIe 함수의 껍데기(BAR 배치, 인터럽트 발생 수단)이며, 그 위에서 어떤
 * 장치인 척할지는 EPF 드라이버가 정한다. NVMe 엔드포인트를 흉내 내는
 * 코드가 이 파일에 있는 것은 아니다.
 */

/* [한국어] FIELD_PREP / FIELD_GET. 링크 상태의 속도·폭 필드를 뽑고,
 * 이퀄라이저 프리셋 벡터와 LTR 값을 조립하는 데 쓴다 */
#include <linux/bitfield.h>
/* [한국어] clk_prepare_enable 과 clk_set_rate. **클록이 둘뿐이다**(core, core_m) --
 * 앞 세대 pci-tegra.c 가 넷을 다루던 것과 대비되며, 클록 관리의 상당
 * 부분이 BPMP 펌웨어로 넘어갔기 때문이다 */
#include <linux/clk.h>
/* [한국어] debugfs_create_dir 와 debugfs_create_devm_seqfile.
 * ASPM 상태 전이 횟수를 보여 주는 파일에 쓴다 */
#include <linux/debugfs.h>
/* [한국어] msleep / usleep_range / udelay. 링크 기동의 100ms 대기,
 * PERST 펄스, SBR 우회의 1us 지연이 여기서 온다 */
#include <linux/delay.h>
/* [한국어] gpiod_get_value / gpiod_set_debounce / gpiod_to_irq.
 * **EP 모드의 핵심 의존성** 이다 -- PERST GPIO 를 인터럽트로 바꿔
 * 호스트의 신호를 받는다 */
#include <linux/gpio/consumer.h>
/* [한국어] icc_set_bw 와 devm_of_icc_get. **앞 세대에 없던 연결이다** --
 * 링크 속도와 폭에 맞춰 SoC 내부 메모리 대역폭을 요청한다 */
#include <linux/interconnect.h>
/* [한국어] devm_request_irq / devm_request_threaded_irq 와 IRQ 반환값들.
 * RC 는 일반 핸들러, EP 는 하드+스레드 짝을 쓴다 */
#include <linux/interrupt.h>
/* [한국어] readl_poll_timeout. LTSSM 상태와 L2 진입을 기다리는 데 쓴다 */
#include <linux/iopoll.h>
/* [한국어] ARRAY_SIZE, upper_32_bits 등 기본 매크로 */
#include <linux/kernel.h>
/* [한국어] MODULE_DEVICE_TABLE, MODULE_LICENSE 등. **이 드라이버는 모듈로 빌드될 수
 * 있다** -- 앞 세대가 builtin_platform_driver 인 것과 다르며, 그래서
 * remove 경로가 필요하다 */
#include <linux/module.h>
/* [한국어] 장치 트리 속성 읽기. of_property_read_u32 계열이 ASPM 파라미터와
 * 레인 수와 컨트롤러 ID 를 읽는다 */
#include <linux/of.h>
/* [한국어] PCI 관련 장치 트리 도우미 */
#include <linux/of_pci.h>
/* [한국어] struct pci_bus, struct pci_ops, PCI_EXP_ 계열 상수.
 * PCI 코어와 맞물리는 타입이 여기서 온다 */
#include <linux/pci.h>
/* [한국어] phy_init / phy_power_on / phy_calibrate. p2u PHY 를 다룬다.
 * **phy_calibrate 는 EP 모드에서만 부른다** */
#include <linux/phy/phy.h>
/* [한국어] pinctrl_pm_select_default_state. RC 기동 시 측대역 핀을 설정한다 */
#include <linux/pinctrl/consumer.h>
/* [한국어] platform_get_resource_byname, platform_get_irq_byname,
 * module_platform_driver. 이 컨트롤러는 플랫폼 장치로 등록된다 */
#include <linux/platform_device.h>
/* [한국어] pm_runtime_enable / _get_sync / _resume_and_get / _put_sync.
 * **앞 세대와 쓰임이 다르다** -- pci-tegra.c 는 runtime PM 콜백에
 * 하드웨어 기동 전체를 넣었지만, 이 드라이버는 콜백을 등록하지 않고
 * 전원 도메인 확보 용도로만 쓴다 */
#include <linux/pm_runtime.h>
/* [한국어] 난수 관련. **이 파일에서 쓰는 함수가 보이지 않는다** --
 * 과거에 쓰였다가 남은 것으로 보이나 근거는 확인하지 못했다 */
#include <linux/random.h>
/* [한국어] reset_control_assert / _deassert. 리셋이 둘이다 --
 * core_apb_rst 는 APPL 접근을 열고, core_rst 는 IP 코어를 동작시킨다 */
#include <linux/reset.h>
/* [한국어] struct resource. DBI, APPL, atu_dma 세 영역을 다루는 데 필요하다 */
#include <linux/resource.h>
/* [한국어] 기본 타입 정의 */
#include <linux/types.h>
/* [한국어] **이 파일의 존재 이유.** struct dw_pcie, dw_pcie_ops,
 * dw_pcie_host_ops, dw_pcie_ep_ops 와 dw_pcie_readl_dbi 계열 도우미가
 * 모두 여기서 온다. 이 헤더를 include 하는 순간 이 드라이버는
 * "컨트롤러 구현" 이 아니라 "DWC 접착 계층" 이 된다 */
#include "pcie-designware.h"
/* [한국어] tegra_bpmp_get / _put / _transfer 와 struct tegra_bpmp_message.
 * UPHY 제어를 펌웨어에 맡기는 통로다.
 * **이 헤더는 이 스파스 체크아웃에 없다**(include/soc/tegra 부재) */
#include <soc/tegra/bpmp.h>
/* [한국어] MRQ_UPHY, CMD_UPHY_PCIE_CONTROLLER_STATE 등 펌웨어 메시지 정의.
 * **이 헤더도 이 트리에 없어** 상수 값을 확인하지 못했다 */
#include <soc/tegra/bpmp-abi.h>
/* [한국어] PCI 서브시스템 내부 헤더. 여기서 쓰는 것은
 * PCIE_PME_TO_L2_TIMEOUT_US 로 보이나, **그 상수의 정의를 이 트리에서
 * 찾지 못했다** -- pcie-designware-host.c:2819 도 같은 상수를 쓴다 */
#include "../../pci.h"

/* [한국어] Tegra194 의 DWC IP 버전 식별자. of_data 의 version 필드를 채워
 * **SoC 를 구별하는 데만 쓰인다** -- 세 곳에서 이 값과 비교해 동작을
 * 가른다(BPMP 컨트롤러 5 예외, 외부 REFCLK 처리, SRNS 속성 읽기).
 * **DW_PCIE_VER_500A 의 정의는 이 트리에 없어** 값을 확인하지 못했다 */
#define TEGRA194_DWC_IP_VER			DW_PCIE_VER_500A
/* [한국어] Tegra234 의 DWC IP 버전 식별자. 역시 SoC 구별에만 쓰이며,
 * **DW_PCIE_VER_562A 의 정의도 이 트리에 없다** */
#define TEGRA234_DWC_IP_VER			DW_PCIE_VER_562A

/* [한국어] APPL 의 핀 제어 레지스터. **PERST 와 CLKREQ 와 REFCLK 출력이 모두
 * 여기 있다** -- 링크 기동, 절전 진입, SRNS 구성에서 반복해서 쓰인다 */
#define APPL_PINMUX				0x0
/* [한국어] PERST 출력 비트. **1이 해제, 0이 어서트** 다 --
 * tegra_pcie_dw_start_link 가 지워서 리셋에 넣고 세워서 푼다.
 * 앞 세대 pci-tegra.c 의 AFI_PEX_CTRL_RST 와 같은 극성이다 */
#define APPL_PINMUX_PEX_RST			BIT(0)
/* [한국어] CLKREQ 덮어쓰기 활성화와 그 값. 짝으로 쓰이며,
 * CLKREQ 를 지원하지 않는 보드에서 신호를 고정하거나(config_controller)
 * 절전 시 강제로 당기는 데(pme_turnoff) 쓴다 */
#define APPL_PINMUX_CLKREQ_OVERRIDE_EN		BIT(2)
#define APPL_PINMUX_CLKREQ_OVERRIDE		BIT(3)
/* [한국어] REFCLK 출력 덮어쓰기 활성화와 그 값.
 * **RC 와 EP 가 반대로 쓴다** -- RC 는 외부 클록을 받을 때 출력을 막고
 * (값을 0 으로), EP 는 입력을 받으려고 켠다(값을 1 로) */
#define APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE_EN	BIT(4)
#define APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE	BIT(5)
/* [한국어] CLKREQ 기본값 비트. CLKREQ 미지원 보드에서 지운다 */
#define APPL_PINMUX_CLKREQ_DEFAULT_VALUE	BIT(13)

/* [한국어] APPL 의 주 제어 레지스터. **LTSSM 스위치와 핫 리셋 모드가 여기 있다** */
#define APPL_CTRL				0x4
/* [한국어] 사전 감지 상태 비트. RC 와 EP 초기화가 모두 세운다.
 * 이름으로 미루어 링크 훈련 전 단계와 관련되나, 근거 문서는 이 트리에 없다 */
#define APPL_CTRL_SYS_PRE_DET_STATE		BIT(6)
/* [한국어] **LTSSM 활성화 비트.** 이 비트를 세우는 순간 링크 훈련이 시작된다.
 * 이 파일에서 가장 결정적인 비트이며, 다섯 곳에서 세우거나 지운다 --
 * 링크 기동, 핫 리셋 완료, EP 초기화, 절전 진입, EP 해제다 */
#define APPL_CTRL_LTSSM_EN			BIT(7)
/* [한국어] 하드웨어 핫 리셋 활성화. RC 는 조건부로, EP 는 무조건 켠다 --
 * 엔드포인트는 호스트가 언제든 리셋을 걸 수 있기 때문이다 */
#define APPL_CTRL_HW_HOT_RST_EN			BIT(20)
/* [한국어] 핫 리셋 모드 필드(2비트)와 그 시프트(22).
 * **마스크가 시프트되지 않은 형태** 라, 쓰는 쪽에서 직접 밀어야 한다 --
 * 그래서 코드에 (MASK << SHIFT) 라는 표현이 반복된다 */
#define APPL_CTRL_HW_HOT_RST_MODE_MASK		GENMASK(1, 0)
#define APPL_CTRL_HW_HOT_RST_MODE_SHIFT		22
/* [한국어] 즉시 리셋 모드. tegra_pcie_dw_resume_early 가
 * 절전에서 깨어난 뒤 이 값으로 되돌린다 */
#define APPL_CTRL_HW_HOT_RST_MODE_IMDT_RST	0x1
/* [한국어] 즉시 리셋 후 LTSSM 재활성화 모드.
 * RC 의 config_controller 와 EP 초기화가 이 값을 쓴다 --
 * 리셋 뒤 자동으로 링크를 다시 세우게 한다 */
#define APPL_CTRL_HW_HOT_RST_MODE_IMDT_RST_LTSSM_EN	0x2

/* [한국어] **최상위 인터럽트 활성화 레지스터.** APPL 인터럽트는 2단 구조라,
 * 이 레지스터가 범주를 켜고 각 범주의 L1_x 레지스터가 세부를 켠다.
 * 앞 세대 pci-tegra.c 의 AFI_INTR_MASK 와 AFI_AFI_INTR_ENABLE 이
 * 이루던 관계와 같다 */
#define APPL_INTR_EN_L0_0			0x8
/* [한국어] 링크 상태 변화 범주 */
#define APPL_INTR_EN_L0_0_LINK_STATE_INT_EN	BIT(0)
/* [한국어] MSI 수신 범주. 이 비트와 아래의 SYS_MSI_INTR_EN 둘이
 * tegra_pcie_enable_msi_interrupts 가 켜는 전부다 -- **이 파일의 MSI
 * 관련 코드가 사실상 그것뿐이다** */
#define APPL_INTR_EN_L0_0_MSI_RCV_INT_EN	BIT(4)
/* [한국어] INT 범주. INTx, 대역폭, eDMA, AER 이 모두 이 범주에 속한다 */
#define APPL_INTR_EN_L0_0_INT_INT_EN		BIT(8)
/* [한국어] PCI 명령 레지스터 변화 범주. **EP 모드에서만 켠다** --
 * 호스트가 버스 마스터 비트를 세우면 LTR 을 보내야 하기 때문이다 */
#define APPL_INTR_EN_L0_0_PCI_CMD_EN_INT_EN	BIT(15)
/* [한국어] CDM 검사 범주. **이 상수는 이 파일에서 쓰이지 않는다** --
 * 비트 위치가 SoC 마다 달라 of_data 의 cdm_chk_int_en_bit 을 대신 쓰기
 * 때문이다. 정의만 남아 레지스터 지도 역할을 한다 */
#define APPL_INTR_EN_L0_0_CDM_REG_CHK_INT_EN	BIT(19)
/* [한국어] 시스템 인터럽트 전달. INTx 범주와 함께 켜진다 */
#define APPL_INTR_EN_L0_0_SYS_INTR_EN		BIT(30)
/* [한국어] MSI 인터럽트 전달 */
#define APPL_INTR_EN_L0_0_SYS_MSI_INTR_EN	BIT(31)

/* [한국어] 최상위 인터럽트 상태. 두 핸들러가 가장 먼저 읽어 어느 범주에
 * 사건이 있는지 확인한다. **1을 쓰면 지워진다** */
#define APPL_INTR_STATUS_L0			0xC
/* [한국어] 각 범주의 상태 비트들. 활성화 레지스터와 비트 위치가 같다.
 * PEX_RST_INT(비트 16)는 **이 파일에서 쓰이지 않는다** -- EP 의 PERST 는
 * APPL 인터럽트가 아니라 GPIO 인터럽트로 받기 때문이다 */
#define APPL_INTR_STATUS_L0_LINK_STATE_INT	BIT(0)
#define APPL_INTR_STATUS_L0_INT_INT		BIT(8)
#define APPL_INTR_STATUS_L0_PCI_CMD_EN_INT	BIT(15)
#define APPL_INTR_STATUS_L0_PEX_RST_INT		BIT(16)
#define APPL_INTR_STATUS_L0_CDM_REG_CHK_INT	BIT(18)

/* [한국어] 링크 상태 범주의 세부 활성화 레지스터 */
#define APPL_INTR_EN_L1_0_0				0x1C
/* [한국어] 링크 리셋 요청 알림.
 * **has_sbr_reset_fix 가 없는 SoC(Tegra194)에서만 켠다** --
 * 수정된 SoC 는 하드웨어가 알아서 처리하므로 알림이 필요 없다 */
#define APPL_INTR_EN_L1_0_0_LINK_REQ_RST_NOT_INT_EN	BIT(1)
/* [한국어] 링크 업 알림. **EP 모드에서만 켠다** --
 * 호스트와 링크가 맺어졌음을 알아야 EP 코어에 통보할 수 있다 */
#define APPL_INTR_EN_L1_0_0_RDLH_LINK_UP_INT_EN		BIT(3)
/* [한국어] 핫 리셋 완료 알림. 역시 EP 전용이다 */
#define APPL_INTR_EN_L1_0_0_HOT_RESET_DONE_INT_EN	BIT(30)

/* [한국어] 링크 상태 범주의 세부 상태. 두 핸들러가 읽고 **곧바로 되써서 지운다** */
#define APPL_INTR_STATUS_L1_0_0				0x20
/* [한국어] 링크 리셋 요청 변화. RC 핸들러가
 * 이 비트를 보고 SBR 우회를 적용한다 */
#define APPL_INTR_STATUS_L1_0_0_LINK_REQ_RST_NOT_CHGED	BIT(1)
/* [한국어] 링크 업 상태 변화. EP 하드 핸들러가
 * 이 비트를 보고 실제 링크 상태를 확인한 뒤 스레드를 깨운다 */
#define APPL_INTR_STATUS_L1_0_0_RDLH_LINK_UP_CHGED	BIT(3)
/* [한국어] 핫 리셋 완료. EP 하드 핸들러가
 * pex_ep_event_hot_rst_done 을 부르는 조건이다 */
#define APPL_INTR_STATUS_L1_0_0_HOT_RESET_DONE		BIT(30)

/* [한국어] 상태 레지스터 L1_1 부터 L1_17 까지. **개별 비트를 읽는 곳이 거의 없고**
 * 대부분 0xFFFFFFFF 로 지우는 대상으로만 등장한다 -- 초기화 시
 * 쌓인 상태를 모두 버리기 위해서다. 그 목록이 세 함수에 반복된다 */
#define APPL_INTR_STATUS_L1_1			0x2C
#define APPL_INTR_STATUS_L1_2			0x30
#define APPL_INTR_STATUS_L1_3			0x34
#define APPL_INTR_STATUS_L1_6			0x3C
#define APPL_INTR_STATUS_L1_7			0x40
/* [한국어] 버스 마스터 활성화 비트 변화.
 * **L1_15 레지스터 안의 비트인데 정의가 L1_7 옆에 있다** -- 이름 순서가
 * 선언 위치와 어긋나지만, 값은 BIT(1) 로 정확하다.
 * EP 하드 핸들러가 이 비트를 보고 스레드를 깨운다 */
#define APPL_INTR_STATUS_L1_15_CFG_BME_CHGED	BIT(1)

/* [한국어] INT 범주의 세부 활성화. INTx, 대역폭, eDMA, AER 이 여기 모여 있다 */
#define APPL_INTR_EN_L1_8_0			0x44
/* [한국어] 대역폭 관리 이벤트 알림 */
#define APPL_INTR_EN_L1_8_BW_MGT_INT_EN		BIT(2)
/* [한국어] 자동 대역폭 변경 알림. RC 핸들러가 이 사건에
 * apply_bad_link_workaround 를 부른다 */
#define APPL_INTR_EN_L1_8_AUTO_BW_INT_EN	BIT(3)
/* [한국어] eDMA 인터럽트. **RC 와 EP 양쪽에서 켜지만 이 파일이 처리하지 않는다** --
 * EP 하드 핸들러의 원문 주석대로 DMA 드라이버가 따로 처리한다 */
#define APPL_INTR_EN_L1_8_EDMA_INT_EN		BIT(6)
/* [한국어] INTx 인터럽트 */
#define APPL_INTR_EN_L1_8_INTX_EN		BIT(11)
/* [한국어] AER 인터럽트. **커널이 AER 을 지원할 때만 켠다** --
 * 받아도 처리할 코드가 없으면 의미가 없기 때문이다 */
#define APPL_INTR_EN_L1_8_AER_INT_EN		BIT(15)

/* [한국어] INT 범주의 세부 상태 */
#define APPL_INTR_STATUS_L1_8_0			0x4C
/* [한국어] eDMA 인터럽트 비트들(비트 11:6, 여섯 채널).
 * EP 하드 핸들러가 이것을 보고 "미확인 인터럽트가 아니다" 라고만
 * 표시하고 처리는 하지 않는다 */
#define APPL_INTR_STATUS_L1_8_0_EDMA_INT_MASK	GENMASK(11, 6)
/* [한국어] 대역폭 관리 상태 */
#define APPL_INTR_STATUS_L1_8_0_BW_MGT_INT_STS	BIT(2)
/* [한국어] 자동 대역폭 변경 상태 */
#define APPL_INTR_STATUS_L1_8_0_AUTO_BW_INT_STS	BIT(3)

/* [한국어] 나머지 상태 레지스터들. 위의 L1_1 과 같은 이유로 대부분
 * 지우기 대상으로만 쓰인다 */
#define APPL_INTR_STATUS_L1_9			0x54
#define APPL_INTR_STATUS_L1_10			0x58
#define APPL_INTR_STATUS_L1_11			0x64
#define APPL_INTR_STATUS_L1_13			0x74
#define APPL_INTR_STATUS_L1_14			0x78
#define APPL_INTR_STATUS_L1_15			0x7C
#define APPL_INTR_STATUS_L1_17			0x88

/* [한국어] CDM 검사 범주의 세부 활성화 */
#define APPL_INTR_EN_L1_18				0x90
/* [한국어] CDM 검사의 세 결과 활성화 비트.
 * **완료(CMPLT) 비트는 켜지 않는다** -- tegra_pcie_enable_system_interrupts 가
 * 오류 둘만 켠다. 완료 알림은 필요 없다는 판단으로 보인다 */
#define APPL_INTR_EN_L1_18_CDM_REG_CHK_CMPLT		BIT(2)
#define APPL_INTR_EN_L1_18_CDM_REG_CHK_CMP_ERR		BIT(1)
#define APPL_INTR_EN_L1_18_CDM_REG_CHK_LOGIC_ERR	BIT(0)

/* [한국어] CDM 검사 결과 상태 */
#define APPL_INTR_STATUS_L1_18				0x94
/* [한국어] CDM 검사의 세 결과 상태 비트.
 * RC 핸들러가 셋을 각각 확인해 DBI 쪽 상태 레지스터에 대응 비트를
 * 세워 준다. **활성화하지 않은 완료 비트도 읽는다** -- 다른 원인으로
 * 인터럽트가 왔을 때 함께 서 있을 수 있기 때문이다 */
#define APPL_INTR_STATUS_L1_18_CDM_REG_CHK_CMPLT	BIT(2)
#define APPL_INTR_STATUS_L1_18_CDM_REG_CHK_CMP_ERR	BIT(1)
#define APPL_INTR_STATUS_L1_18_CDM_REG_CHK_LOGIC_ERR	BIT(0)

/* [한국어] EP 가 MSI 를 올릴 때 쓰는 레지스터.
 * tegra_pcie_ep_raise_msi_irq 가 BIT(irq - 1) 을 쓴다 */
#define APPL_MSI_CTRL_1				0xAC

/* [한국어] MSI 관련 상태로 보인다. **pex_ep_event_hot_rst_done 에서만
 * 0xFFFFFFFF 로 지운다** -- 다른 두 지우기 목록에는 없다 */
#define APPL_MSI_CTRL_2				0xB0

/* [한국어] EP 가 INTx 를 올릴 때 쓰는 레지스터.
 * tegra_pcie_ep_raise_intx_irq 가 1 을 썼다가 1~2ms 뒤 0 을 써
 * **소프트웨어로 펄스를 만든다** */
#define APPL_LEGACY_INTX			0xB8

/* [한국어] LTR 메시지의 값 필드들. EP 초기화가 스누프와 논스누프 각각에
 * 110us(값 110, 축척 2)를 채워 둔다 */
#define APPL_LTR_MSG_1				0xC4
/* [한국어] LTR 메시지 요청 비트와 논스누프 요청 비트. 위 레지스터에 값과 함께
 * 세워 둔다 */
#define LTR_MSG_REQ				BIT(15)
#define LTR_NOSNOOP_MSG_REQ			BIT(31)

/* [한국어] LTR 전송 제어 레지스터 */
#define APPL_LTR_MSG_2				0xC8
/* [한국어] LTR 전송 요청 상태.
 * **두 가지로 쓰인다** -- has_ltr_req_fix 인 SoC 는 초기화 때 한 번 세워
 * 두고, 그렇지 않은 SoC 는 tegra_pcie_ep_irq_thread 가 매번 세운 뒤
 * 내려가기를 폴링한다 */
#define APPL_LTR_MSG_2_LTR_MSG_REQ_STATE	BIT(3)

/* [한국어] 링크 상태 레지스터 */
#define APPL_LINK_STATUS			0xCC
/* [한국어] 링크 업 비트. EP 하드 핸들러가 링크 업 변화 알림을
 * 받은 뒤 실제 상태를 이 비트로 확인하고, tegra_pcie_dw_start_link 의
 * DLF 판정에도 쓰인다 */
#define APPL_LINK_STATUS_RDLH_LINK_UP		BIT(0)

/* [한국어] 디버그 상태 레지스터. **LTSSM 상태와 L2 진입 여부를 담고 있어**
 * 링크 진단의 중심이다 */
#define APPL_DEBUG				0xD0
/* [한국어] L2 진입 완료 비트. tegra_pcie_try_link_l2 가
 * 이 비트가 서기를 폴링한다 */
#define APPL_DEBUG_PM_LINKST_IN_L2_LAT		BIT(21)
/* [한국어] L0 상태 값. **이 상수는 이 파일에서 쓰이지 않는다** --
 * tegra_pcie_dw_start_link 가 같은 판정을 하면서 0x11 을 직접 쓴다.
 * 값이 같으므로 상수를 쓰지 않은 것은 실수로 보이나, 코드는 손대지 않는다 */
#define APPL_DEBUG_PM_LINKST_IN_L0		0x11
/* [한국어] LTSSM 상태 필드(비트 8:3)와 시프트.
 * **마스크와 시프트를 쓰는 방식이 두 가지로 갈린다** --
 * tegra_pcie_dw_start_link 는 마스크 후 시프트해 0x11 과 비교하고,
 * 절전/EP 경로는 시프트하지 않고 마스크만 한 값을 LTSSM_STATE_ 상수와
 * 직접 비교한다. 그래서 아래 상수들은 이미 시프트된 값이다 */
#define APPL_DEBUG_LTSSM_STATE_MASK		GENMASK(8, 3)
#define APPL_DEBUG_LTSSM_STATE_SHIFT		3
/* [한국어] LTSSM 상태 값들. **이미 3비트 시프트된 형태다** --
 * 예컨대 DETECT_ACT 가 0x08 인데 이는 실제 상태 번호 1 을 8배 한 값이다.
 * 그래서 마스크만 한 값과 직접 비교할 수 있다.
 * 네 detect 계열 값은 "링크가 확실히 내려갔다" 를 판정하는 데 함께 쓰이고,
 * L2_IDLE 은 EP 해제 경로에서 하나 더 허용되는 상태다 */
#define LTSSM_STATE_DETECT_QUIET		0x00
#define LTSSM_STATE_DETECT_ACT			0x08
#define LTSSM_STATE_PRE_DETECT_QUIET		0x28
#define LTSSM_STATE_DETECT_WAIT			0x30
#define LTSSM_STATE_L2_IDLE			0xa8

/* [한국어] 전원 관리 요청 레지스터 */
#define APPL_RADM_STATUS			0xE4
/* [한국어] PME_Turn_Off 전송 비트. tegra_pcie_try_link_l2 가
 * 세우면 하드웨어가 L2 진입 흐름을 시작한다.
 * 앞 세대 pci-tegra.c 의 AFI_PCIE_PME turnoff 비트와 같은 역할이되,
 * **포트별 비트가 아니라 하나뿐이다** -- 컨트롤러마다 링크가 하나이기
 * 때문이다 */
#define APPL_PM_XMT_TURNOFF_STATE		BIT(0)

/* [한국어] **이 코어를 RC 로 쓸지 EP 로 쓸지 정하는 레지스터.**
 * 같은 IP 가 양쪽으로 동작할 수 있다는 사실이 이 한 레지스터에 드러난다.
 * 앞 세대 pci-tegra.c 에는 이런 선택이 없다 -- RC 전용이었다 */
#define APPL_DM_TYPE				0x100
/* [한국어] 모드 필드(하위 4비트) */
#define APPL_DM_TYPE_MASK			GENMASK(3, 0)
/* [한국어] 루트 포트 모드 값. tegra_pcie_config_controller 가 **필드를 지우지 않고
 * 그대로 덮어쓴다** */
#define APPL_DM_TYPE_RP				0x4
/* [한국어] 엔드포인트 모드 값 0. pex_ep_event_pex_rst_deassert 는
 * **필드를 지운 뒤 넣는다** -- 값이 0 이라 지우기만 해도 되지만
 * 형태를 갖춘 것이다 */
#define APPL_DM_TYPE_EP				0x0

/* [한국어] **DBI 블록의 물리 기준 주소를 알려 주는 레지스터.**
 * 접착 계층의 전형적인 일이다 -- IP 코어가 주소 공간 어디에 놓여 있는지를
 * 벤더 로직에 가르쳐 준다 */
#define APPL_CFG_BASE_ADDR			0x104
/* [한국어] 주소의 유효 비트(31:12). 4KiB 정렬을 전제한다 */
#define APPL_CFG_BASE_ADDR_MASK			GENMASK(31, 12)

/* [한국어] **iATU 와 eDMA 블록의 물리 기준 주소.**
 * 위의 DBI 주소와 짝을 이루며, 이 두 줄이 "주소 변환을 코어에 맡긴다"
 * 는 사실을 드러낸다 -- 앞 세대 pci-tegra.c 라면 창을 직접 프로그래밍했을
 * 자리다 */
#define APPL_CFG_IATU_DMA_BASE_ADDR		0x108
/* [한국어] 주소의 유효 비트(31:18). 256KiB 정렬을 전제한다 */
#define APPL_CFG_IATU_DMA_BASE_ADDR_MASK	GENMASK(31, 18)

/* [한국어] 기타 설정. 캐시 속성과 EP 슬레이브 모드가 여기 있다 */
#define APPL_CFG_MISC				0x110
/* [한국어] 슬레이브 EP 모드 비트. **EP 초기화만 세운다** */
#define APPL_CFG_MISC_SLV_EP_MODE		BIT(14)
/* [한국어] AXI 읽기 캐시 속성 필드(비트 13:10)와 시프트와 값(3).
 * RC 와 EP 가 모두 같은 값을 넣는다. AXI 의 ARCACHE 신호에 대응하는
 * 것으로 보이나, 값 3 의 의미는 이 트리에서 확인할 수 없다.
 * **마스크는 정의만 있고 쓰이지 않는다** -- 코드가 필드를 지우지 않고
 * OR 로만 넣기 때문이다 */
#define APPL_CFG_MISC_ARCACHE_MASK		GENMASK(13, 10)
#define APPL_CFG_MISC_ARCACHE_SHIFT		10
#define APPL_CFG_MISC_ARCACHE_VAL		3

/* [한국어] 클록 게이팅 우회 레지스터. RC 와 EP 초기화가 모두 0 을 써
 * 우회를 끈다 -- 즉 하드웨어의 기본 클록 게이팅을 그대로 쓴다 */
#define APPL_CFG_SLCG_OVERRIDE			0x114
/* [한국어] 마스터 클록 게이팅 활성화 비트.
 * **정의만 있고 쓰이지 않는다** -- 레지스터에 0 을 통째로 쓰기 때문이다 */
#define APPL_CFG_SLCG_OVERRIDE_SLCG_EN_MASTER	BIT(0)

/* [한국어] 코어 리셋 덮어쓰기 레지스터 */
#define APPL_CAR_RESET_OVRD				0x12C
/* [한국어] 코어 리셋 덮어쓰기 비트.
 * RC 핸들러의 SBR 우회가 이 비트를 지웠다 1us 뒤 세워 **소프트웨어로
 * 코어 리셋 펄스를 만든다.** has_sbr_reset_fix 가 없는 SoC 전용이다 */
#define APPL_CAR_RESET_OVRD_CYA_OVERRIDE_CORE_RST_N	BIT(0)

/* [한국어] 브리지의 IO 디코딩 비트 둘. tegra_pcie_dw_host_init 이 **지운다** --
 * 이 루트 포트는 IO 공간을 하류로 전달하지 않는다 */
#define IO_BASE_IO_DECODE				BIT(0)
#define IO_BASE_IO_DECODE_BIT8				BIT(8)

/* [한국어] prefetchable 메모리 디코딩 비트 둘.
 * IO 와 반대로 **세운다** -- 그 공간은 전달한다 */
#define CFG_PREF_MEM_LIMIT_BASE_MEM_DECODE		BIT(0)
#define CFG_PREF_MEM_LIMIT_BASE_MEM_LIMIT_DECODE	BIT(16)

/* [한국어] 타이머 제어 레지스터와 ACK/NAK 시프트(19).
 * update_fc_fixup 이 켜진 보드에서 RC 와 EP 양쪽이 그 자리에 1 을 세운다.
 * 장치 트리가 지정하는 보드별 우회이며, 그 목적은 코드에 적혀 있지 않다 */
#define CFG_TIMER_CTRL_MAX_FUNC_NUM_OFF	0x718
#define CFG_TIMER_CTRL_ACK_NAK_SHIFT	(19)

/* [한국어] FTS(Fast Training Sequence) 개수 관련 상수 둘, 모두 52.
 * **이 파일 어디에서도 쓰이지 않는다** -- 실제 값은 of_data 의 n_fts
 * 배열에서 오며, 그 값도 52 로 같다. 정의만 남은 셈이다 */
#define N_FTS_VAL					52
#define FTS_VAL						52

/* [한국어] AMBA 오류 응답 형식 레지스터 */
#define PORT_LOGIC_AMBA_ERROR_RESPONSE_DEFAULT	0x8D0
/* [한국어] RRS 응답 형식 필드의 시프트와 마스크.
 * RRS(Request Retry Status)는 장치가 아직 준비되지 않았을 때 보내는
 * 응답이며, 이 필드가 그때 CPU 에게 돌려줄 값의 형식을 정한다 */
#define AMBA_ERROR_RESPONSE_RRS_SHIFT		3
#define AMBA_ERROR_RESPONSE_RRS_MASK		GENMASK(1, 0)
/* [한국어] 세 가지 응답 형식.
 * **tegra_pcie_dw_host_init 은 OKAY_FFFF0001 을 고른다** --
 * "벤더 ID 자리는 유효하고 나머지는 1" 이라는 특수 패턴이라,
 * PCI 코어가 재시도해야 함을 알아챌 수 있다.
 * 나머지 둘(OKAY, OKAY_FFFFFFFF)은 쓰이지 않는다 */
#define AMBA_ERROR_RESPONSE_RRS_OKAY		0
#define AMBA_ERROR_RESPONSE_RRS_OKAY_FFFFFFFF	1
#define AMBA_ERROR_RESPONSE_RRS_OKAY_FFFF0001	2

/* [한국어] **EP 의 MSI-X 수신 주소 매칭 레지스터.**
 * pex_ep_event_pex_rst_deassert 가 EP 코어의 MSI 메모리 물리 주소를
 * 여기 등록하면, tegra_pcie_ep_raise_msix_irq 가 그 메모리에 쓸 때
 * 하드웨어가 가로채 MSI-X 트랜잭션으로 바꾼다 */
#define MSIX_ADDR_MATCH_LOW_OFF			0x940
/* [한국어] 매칭 활성화 비트 */
#define MSIX_ADDR_MATCH_LOW_OFF_EN		BIT(0)
/* [한국어] 하위 주소의 유효 비트(31:2) */
#define MSIX_ADDR_MATCH_LOW_OFF_MASK		GENMASK(31, 2)

/* [한국어] 같은 주소의 상위 32비트 */
#define MSIX_ADDR_MATCH_HIGH_OFF		0x944
/* [한국어] 상위 주소 전체를 쓴다 */
#define MSIX_ADDR_MATCH_HIGH_OFF_MASK		GENMASK(31, 0)

/* [한국어] **시스템을 멈추게 하는 문제의 레지스터.**
 * 엔드포인트 모드용인데 RC 모드에서도 보이고, 링크가 ASPM-L1 일 때
 * 접근하면 시스템이 멈춘다. 그래서 tegra_pcie_dw_rd_own_conf 와
 * _wr_own_conf 가 has_msix_doorbell_access_fix 가 없는 SoC(Tegra194)에서
 * 이 오프셋에 대한 접근을 건너뛴다 */
#define PORT_LOGIC_MSIX_DOORBELL			0x948

/* [한국어] Secondary PCI Express 확장 능력의 레인 제어 오프셋.
 * config_gen3_gen4_eq_presets 가 **레인마다 2바이트씩** 떨어진 자리에
 * 프리셋을 쓴다 */
#define CAP_SPCIE_CAP_OFF			0x154
/* [한국어] 하류 방향 송신 프리셋 필드(하위 4비트) */
#define CAP_SPCIE_CAP_OFF_DSP_TX_PRESET0_MASK	GENMASK(3, 0)
/* [한국어] 상류 방향 송신 프리셋 필드(비트 11:8)와 그 시프트.
 * 두 방향에 같은 값을 넣는다 */
#define CAP_SPCIE_CAP_OFF_USP_TX_PRESET0_MASK	GENMASK(11, 8)
#define CAP_SPCIE_CAP_OFF_USP_TX_PRESET0_SHIFT	8

/* [한국어] LTSSM 상태 폴링 간격 10ms 와 상한 120ms.
 * EP 해제와 절전 진입의 detect 대기에 함께 쓰인다 */
#define LTSSM_DELAY_US		10000	/* 10 ms */
#define LTSSM_TIMEOUT_US	120000	/* 120 ms */

/* [한국어] 이퀄라이제이션 초기 프리셋 값 5.
 * **Gen3 부터 생긴 절차** 라 Gen2 가 상한이던 앞 세대에는 없던 개념이다.
 * 이 값이 무엇을 뜻하는지는 PCIe 규격의 프리셋 표가 정하며,
 * 이 트리에서는 확인할 수 없다 */
#define GEN3_GEN4_EQ_PRESET_INIT	5

/* [한국어] 세대별 코어 클록 주파수 넷. Gen1 62.5MHz 에서 Gen4 500MHz 까지
 * 두 배씩 오른다. 링크 속도가 오르면 코어도 그만큼 빨리 돌아야 하기
 * 때문이며, **Gen4 까지 있다는 것이 앞 세대(Gen2 상한)와의 결정적 차이다** */
#define GEN1_CORE_CLK_FREQ	62500000
#define GEN2_CORE_CLK_FREQ	125000000
#define GEN3_CORE_CLK_FREQ	250000000
#define GEN4_CORE_CLK_FREQ	500000000

/* [한국어] LTR 메시지 전송 완료를 기다리는 상한 100ms.
 * tegra_pcie_ep_irq_thread 가 1ms 간격으로 폴링한다 */
#define LTR_MSG_TIMEOUT		(100 * 1000)

/* [한국어] PERST GPIO 디바운스 시간 5ms. 신호 잡음으로 인터럽트가
 * 여러 번 걸리는 것을 막는다 */
#define PERST_DEBOUNCE_TIME	(5 * 1000)

/* [한국어] 엔드포인트 상태 값 둘. pcie->ep_state 에 담기며,
 * pex_ep_event_pex_rst_assert 와 _deassert 가 서로의 중복 실행을 막는
 * 데 쓴다 */
#define EP_STATE_DISABLED	0
#define EP_STATE_ENABLED	1

static const unsigned int pcie_gen_freq[] = {
	GEN1_CORE_CLK_FREQ,	/* PCI_EXP_LNKSTA_CLS == 0; undefined */
	GEN1_CORE_CLK_FREQ,
	GEN2_CORE_CLK_FREQ,
	GEN3_CORE_CLK_FREQ,
	GEN4_CORE_CLK_FREQ
};

struct tegra_pcie_dw_of_data {
	/* [한국어] 이 SoC 의 DWC IP 버전 식별자.
	 * 설정자: 정적 초기화. TEGRA194_DWC_IP_VER 또는 TEGRA234_DWC_IP_VER.
	 * 읽는 자: tegra_pcie_dw_parse_dt(외부 REFCLK 처리와 SRNS 속성 읽기),
	 *   tegra_pcie_bpmp_set_ctrl_state(컨트롤러 5 예외 판정).
	 * 값 범위: **두 상수의 정의가 이 트리에 없어 실제 값을 확인하지 못했다.**
	 *   세 곳 모두 == 비교로만 쓰므로 값 자체는 중요하지 않고 서로 다르기만
	 *   하면 된다.
	 * 동기화: 읽기 전용 상수 */
	u32 version;
	/* [한국어] 이 인스턴스가 RC 인지 EP 인지.
	 * 설정자: 정적 초기화. DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE.
	 * 읽는 자: **이 파일에서 가장 많이 검사되는 필드다** -- probe 의 분기,
	 *   start_link 와 stop_link, parse_dt 의 EP 전용 절, init_host_aspm 의
	 *   L1.2 우회, 그리고 PM 콜백 다섯 중 넷이 모두 이 값을 본다.
	 * 값 범위: 두 값 중 하나. 장치 트리 compatible 넷이 RC/EP × 두 세대에
	 *   대응하므로 **compatible 이 곧 모드를 정한다.**
	 * 동기화: 읽기 전용 상수 */
	enum dw_pcie_device_mode mode;
	/* [한국어] MSI-X 도어벨 접근 결함이 고쳐졌는가.
	 * 설정자: 정적 초기화. **Tegra234 RC 만 true.**
	 * 읽는 자: tegra_pcie_dw_rd_own_conf 와 _wr_own_conf.
	 * 의미: false 면 그 레지스터에 ASPM-L1 상태에서 접근할 때 시스템이
	 *   멈추므로, config 접근 경로가 그 오프셋을 건너뛴다.
	 * 주의: Tegra234 **EP** 는 이 값이 false 다 -- 필드 이름이 결함 유무를
	 *   뜻하는데 EP 에서는 그 경로 자체가 쓰이지 않아 무의미하기 때문으로
	 *   보인다.
	 * 동기화: 읽기 전용 상수 */
	bool has_msix_doorbell_access_fix;
	/* [한국어] 보조 버스 리셋 결함이 고쳐졌는가.
	 * 설정자: 정적 초기화. **Tegra234 RC 만 true.**
	 * 읽는 자: tegra_pcie_rp_irq_handler(SBR 우회 적용 여부),
	 *   tegra_pcie_enable_system_interrupts(링크 리셋 알림을 켤지),
	 *   tegra_pcie_config_controller(핫 리셋 모드를 무조건 켤지),
	 *   tegra_pcie_dw_suspend_late 와 _resume_early(핫 리셋 모드 조작).
	 * **다섯 곳에서 검사되는 가장 널리 퍼진 우회 플래그다.**
	 * 동기화: 읽기 전용 상수 */
	bool has_sbr_reset_fix;
	/* [한국어] L1 서브상태 탈출 결함이 고쳐졌는가.
	 * 설정자: 정적 초기화. **Tegra234 의 RC 와 EP 둘 다 true** --
	 *   세대로 갈리는 몇 안 되는 플래그다.
	 * 읽는 자: tegra_pcie_dw_host_init 과 pex_ep_event_pex_rst_deassert.
	 * 의미: false 면 GEN3_RELATED_OFF 의 ZRXDC_NONCOMPL 비트를 지운다.
	 *   그 비트의 의미는 이 트리에서 확인할 수 없고, 이름이 수신 종단 규격
	 *   미준수와 관련됨을 가리킬 뿐이다.
	 * 동기화: 읽기 전용 상수 */
	bool has_l1ss_exit_fix;
	/* [한국어] LTR 요청을 하드웨어가 알아서 하는가.
	 * 설정자: 정적 초기화. **Tegra234 EP 만 true.**
	 * 읽는 자: tegra_pcie_ep_irq_thread(true 면 LTR 전송을 건너뛴다),
	 *   pex_ep_event_pex_rst_deassert(true 면 초기화 때 한 번 켜 둔다).
	 * 의미: 두 읽는 자가 정확히 반대로 동작한다 -- 하드웨어가 처리하면
	 *   초기화 때 권한만 주고, 아니면 매번 소프트웨어가 요청한다.
	 * 동기화: 읽기 전용 상수 */
	bool has_ltr_req_fix;
	/* [한국어] L1.2 능력 광고를 꺼야 하는가.
	 * 설정자: 정적 초기화. **Tegra234 EP 만 true.**
	 * 읽는 자: init_host_aspm. EP 모드일 때만 실제로 적용한다.
	 * 왜 필요한가: 그 함수 안의 원문 주석이 상세히 밝히는데, L1.2 를
	 *   빠져나올 때 REFCLK 가 안정되기 전에 UPHY PLL 이 켜져 주파수를 못
	 *   잡고 링크가 끊기는 하드웨어 결함이 있다. 고칠 방법이 없어 능력
	 *   광고에서 아예 빼, 호스트가 그 상태로 들어가려 하지 않게 만든다.
	 * 동기화: 읽기 전용 상수 */
	bool disable_l1_2;
	/* [한국어] CDM 검사 인터럽트 활성화 비트의 위치.
	 * 설정자: 정적 초기화. **Tegra194 는 BIT(19), Tegra234 는 BIT(18).**
	 * 읽는 자: tegra_pcie_enable_system_interrupts.
	 * 왜 필드인가: 비트 위치가 세대마다 달라 상수를 쓸 수 없기 때문이다.
	 *   그래서 APPL_INTR_EN_L0_0_CDM_REG_CHK_INT_EN 상수는 정의만 있고
	 *   쓰이지 않는다.
	 * 동기화: 읽기 전용 상수 */
	u32 cdm_chk_int_en_bit;
	/* [한국어] Gen4 이퀄라이제이션 프리셋 벡터.
	 * 설정자: 정적 초기화. **Tegra194 는 0x360, Tegra234 는 0x340.**
	 * 읽는 자: config_gen3_gen4_eq_presets 가 GEN3_EQ_CONTROL_OFF 의
	 *   요청 벡터 필드에 넣는다.
	 * 값 범위: 각 정의 위의 원문 주석이 어느 프리셋이 켜지는지 밝힌다 --
	 *   Tegra194 는 5, 6, 8, 9 번이고 Tegra234 는 6, 8, 9 번이다.
	 *   즉 뒷세대가 5번 프리셋을 뺐다.
	 * 동기화: 읽기 전용 상수 */
	u32 gen4_preset_vec;
	/* [한국어] FTS(Fast Training Sequence) 개수. 두 원소다.
	 * 설정자: 정적 초기화. **Tegra194 는 { 52, 52 }, Tegra234 는 { 52, 80 }.**
	 * 읽는 자: tegra_pcie_dw_probe 가 pci->n_fts[0] 과 [1] 에 복사하면
	 *   그다음은 DWC 코어가 쓴다 -- 이 파일은 값을 직접 쓰지 않는다.
	 * 값 범위: 두 원소가 서로 다른 속도 등급용으로 보이며, 뒷세대가
	 *   두 번째를 80 으로 늘렸다.
	 * 동기화: 읽기 전용 상수 */
	u8 n_fts[2];
};

struct tegra_pcie_dw {
	/* [한국어] 이 컨트롤러의 플랫폼 device.
	 * 설정자: tegra_pcie_dw_probe.
	 * 읽는 자: 로그, devm 자원 획득, pm_runtime, BPMP 획득 등 거의 모든 함수.
	 * 주의: pci.dev 에도 같은 값이 들어 있어 두 경로로 접근할 수 있다 --
	 *   이 파일은 두 가지를 섞어 쓴다.
	 * 동기화: 설정 후 불변 */
	struct device *dev;
	/* [한국어] APPL 레지스터 영역의 리소스 기술자.
	 * 설정자: tegra_pcie_dw_probe 가 이름 "appl" 로 얻는다.
	 * 읽는 자: 매핑할 때 한 번 쓰고 그 뒤로는 읽히지 않는다.
	 * 동기화: 설정 후 불변 */
	struct resource *appl_res;
	/* [한국어] DBI 영역의 리소스 기술자.
	 * 설정자: tegra_pcie_dw_parse_dt 가 이름 "dbi" 로 얻는다.
	 * 읽는 자: **tegra_pcie_config_controller 와
	 *   pex_ep_event_pex_rst_deassert 가 그 물리 시작 주소를
	 *   APPL_CFG_BASE_ADDR 에 써 넣는다** -- IP 코어의 위치를 벤더 로직에
	 *   알려 주는 것이다.
	 * 특기할 점: 이 파일은 DBI 를 직접 매핑하지 않는다. 매핑은 DWC 코어가
	 *   하고, 이 필드는 물리 주소를 알기 위해서만 보관된다.
	 * 동기화: 설정 후 불변 */
	struct resource *dbi_res;
	/* [한국어] iATU 와 eDMA 영역의 리소스 기술자.
	 * 설정자: tegra_pcie_dw_probe 가 이름 "atu_dma" 로 얻는다.
	 * 읽는 자: tegra_pcie_config_controller 와 pex_ep_event_pex_rst_deassert 가
	 *   그 물리 시작 주소를 APPL_CFG_IATU_DMA_BASE_ADDR 에 써 넣는다.
	 * 특기할 점: **이 영역은 probe 가 직접 매핑해 pci->atu_base 에 넣는다** --
	 *   DBI 와 달리 매핑을 코어에게 넘기는 것이 아니라 이쪽이 준비해 준다.
	 *   다만 그 안의 내용(주소 변환 창)은 DWC 코어가 관리하며, 이 파일은
	 *   한 줄도 건드리지 않는다. 앞 세대 pci-tegra.c 의
	 *   tegra_pcie_setup_translations 가 AXI BAR 여섯 개를 손으로 채우던
	 *   자리가 여기서 통째로 사라진 셈이다.
	 * 동기화: 설정 후 불변 */
	struct resource *atu_dma_res;
	/* [한국어] APPL 레지스터의 가상 시작 주소.
	 * 설정자: tegra_pcie_dw_probe 가 devm_ioremap_resource 로 얻는다.
	 * 읽는 자: appl_readl / appl_writel 을 통해서만 접근한다. 다만
	 *   tegra_pcie_try_link_l2 와 _pme_turnoff 의 일부가 raw readl/writel 로
	 *   직접 쓰기도 한다.
	 * **이 파일이 직접 읽고 쓰는 거의 전부가 이 주소를 통한다** --
	 *   IP 코어 자신의 레지스터(DBI)는 dw_pcie_ 도우미로만 접근하며,
	 *   그 구분이 이 파일을 읽는 기준선이다.
	 * 동기화: devm 이 관리. 설정 후 불변 */
	void __iomem *appl_base;
	/* [한국어] 코어 클록.
	 * 설정자: tegra_pcie_dw_probe 가 이름 "core" 로 얻는다.
	 * 읽는 자: 전원 경로들이 켜고 끄며, **tegra_pcie_icc_set 과
	 *   두 초기화 함수가 clk_set_rate 로 주파수를 바꾼다** -- 링크 세대에
	 *   맞춰 62.5MHz 에서 500MHz 사이를 오간다.
	 * 동기화: 클록 프레임워크 내부 락 */
	struct clk *core_clk;
	/* [한국어] 코어 모니터 클록.
	 * 설정자: tegra_pcie_dw_probe 가 이름 "core_m" 으로 **optional** 획득.
	 * 읽는 자: tegra_pcie_dw_host_init 이 켜고, tegra_pcie_deinit_controller 와
	 *   _suspend_noirq 가 끈다. tegra_pcie_dw_start_link 의 DLF 재시도 경로도
	 *   끄는데, host_init 이 다시 켤 것이라 짝을 맞추는 것이다.
	 * 값 범위: NULL 일 수 있다. 클록 API 가 NULL 을 무시한다.
	 * 동기화: 클록 프레임워크 내부 락 */
	struct clk *core_clk_m;
	/* [한국어] APB 리셋.
	 * 설정자: tegra_pcie_dw_probe 가 이름 "apb" 로 얻는다.
	 * 읽는 자: 전원 경로들.
	 * 의미: **이 리셋이 풀려야 APPL 레지스터에 접근할 수 있다.**
	 *   그래서 tegra_pcie_config_controller 가 이것을 푼 다음부터 APPL 설정을
	 *   시작한다.
	 * 동기화: 리셋 프레임워크 내부 락 */
	struct reset_control *core_apb_rst;
	/* [한국어] 코어 리셋.
	 * 설정자: tegra_pcie_dw_probe 가 이름 "core" 로 얻는다.
	 * 읽는 자: 전원 경로들, 그리고 **tegra_pcie_dw_start_link 의 DLF 재시도가
	 *   코어를 리셋했다 푼다** -- 그 때문에 레지스터가 초기화되어 설정을
	 *   다시 해야 한다.
	 * 의미: APB 리셋이 APPL 접근을 열었다면 이것은 IP 코어 자체를 동작시킨다.
	 *   그래서 두 초기화 함수 모두 이것을 **가장 마지막에** 푼다.
	 * 동기화: 리셋 프레임워크 내부 락 */
	struct reset_control *core_rst;
	/* [한국어] **DWC 코어 구조체. 이 필드 하나가 접착 계층의 접합점이다.**
	 * 설정자: tegra_pcie_dw_probe 가 dev, ops, n_fts, pp.num_vectors,
	 *   atu_base, atu_size 를 채운다. 나머지는 DWC 코어가 관리한다.
	 * 읽는 자: DWC 코어의 모든 함수, 그리고 이 파일이 dw_pcie_ 도우미를
	 *   부를 때마다.
	 * **포인터가 아니라 값으로 박혀 있다.** 그래서 코어가 dw_pcie 포인터를
	 *   넘겨 주면 to_tegra_pcie(container_of)로 이 구조체를 되찾을 수 있다.
	 *   앞 세대 pci-tegra.c 에는 이에 해당하는 것이 없다 -- 컨트롤러 구현이
	 *   통째로 자기 코드였기 때문이다.
	 * 동기화: 내부는 DWC 코어가 관리 */
	struct dw_pcie pci;
	/* [한국어] BPMP 펌웨어와 통신하는 핸들.
	 * 설정자: tegra_pcie_dw_probe 가 tegra_bpmp_get 으로 얻는다.
	 * 읽는 자: tegra_pcie_bpmp_set_ctrl_state 와 _set_pll_state 두 함수뿐이다.
	 * **앞 세대에 없던 의존성이다** -- pci-tegra.c 는 PADS 레지스터를 직접
	 *   만져 PHY 와 PLL 을 다뤘지만, 여기서는 UPHY 를 펌웨어에 맡긴다.
	 * 주의: devm 이 아니라 직접 얻고 놓는다. remove 와 probe 실패 경로가
	 *   tegra_bpmp_put 을 부른다.
	 * **struct tegra_bpmp 의 정의는 이 스파스 체크아웃에 없다.**
	 * 동기화: BPMP 계층이 내부적으로 처리 */
	struct tegra_bpmp *bpmp;

	/* [한국어] SoC 와 모드별 기술자.
	 * 설정자: tegra_pcie_dw_probe 가 of_device_get_match_data 로 얻는다.
	 * 읽는 자: **이 파일의 모든 SoC/모드 분기가 이 포인터를 통과한다.**
	 * 값 범위: 네 정적 구조체 중 하나(tegra194 RC/EP, tegra234 RC/EP).
	 * 주의: const 를 떼어 저장한다 -- probe 에서 명시적으로 캐스팅하는데,
	 *   실제로 쓰기는 하지 않으므로 const 를 유지해도 됐을 자리다.
	 * 동기화: 읽기 전용 상수를 가리키며 설정 후 불변 */
	struct tegra_pcie_dw_of_data *of_data;

	/* [한국어] 보드가 CLKREQ 신호를 지원하는가.
	 * 설정자: tegra_pcie_dw_parse_dt 가 "supports-clkreq" 속성으로 정한다.
	 * 읽는 자: init_host_aspm(L1 서브상태를 쓸 수 있다고 표시할지),
	 *   tegra_pcie_config_controller(CLKREQ 핀을 덮어쓸지).
	 * 의미: L1.1 과 L1.2 는 CLKREQ 신호가 있어야 성립하므로, 이 값이
	 *   거짓이면 그 상태들을 쓸 수 없다.
	 * 동기화: probe 때 정하고 이후 불변 */
	bool supports_clkreq;
	/* [한국어] CDM 레지스터 무결성 검사를 켤 것인가.
	 * 설정자: tegra_pcie_dw_parse_dt 가 "snps,enable-cdm-check" 로 정한다.
	 * 읽는 자: tegra_pcie_enable_system_interrupts.
	 * 의미: IP 의 설정 레지스터가 손상되었는지 하드웨어가 검사하게 하고,
	 *   결과를 인터럽트로 받는다. **앞 세대에 없던 기능이다.**
	 *   속성 이름이 snps 로 시작하는 데서 이것이 DWC IP 공통 기능임을 알 수 있다.
	 * 동기화: probe 때 정하고 이후 불변 */
	bool enable_cdm_check;
	/* [한국어] 상하류가 서로 다른 참조 클록을 쓰는 구성인가.
	 * 설정자: tegra_pcie_dw_parse_dt 가 **Tegra234 에서만** "nvidia,enable-srns"
	 *   속성으로 정한다. Tegra194 에서는 항상 거짓이다.
	 * 읽는 자: tegra_pcie_dw_host_init 과 pex_ep_event_pex_rst_deassert(슬롯
	 *   클록 구성 비트를 지울지), tegra_pcie_config_controller(REFCLK 출력을
	 *   막을지).
	 * 의미: SRNS 구성에서는 "상대와 같은 클록을 쓴다" 는 표시를 지워야 하고,
	 *   자기 클록을 하류에 공급하지도 말아야 한다.
	 * 동기화: probe 때 정하고 이후 불변 */
	bool enable_srns;
	/* [한국어] **RC 모드에서 링크가 섰는가. 이후 모든 경로의 스위치다.**
	 * 설정자: tegra_pcie_config_rp 가 tegra_pcie_dw_link_up 의 결과로 채운다.
	 *   **EP 모드에서는 아무도 채우지 않아 false 로 남는다.**
	 * 읽는 자: tegra_pcie_dw_remove, _suspend_late, _suspend_noirq,
	 *   _resume_noirq, _resume_early, _shutdown -- 여섯 곳이 모두 이 값이
	 *   거짓이면 곧바로 돌아간다.
	 * 왜 필요한가: probe 가 -ENOMEDIUM 을 성공으로 처리하므로 링크 없이도
	 *   드라이버가 붙어 있을 수 있는데, 그 경우 기동한 것이 없어 정리할
	 *   것도 없기 때문이다.
	 * 동기화: probe 때 정하고 이후 불변 */
	bool link_state;
	/* [한국어] 흐름 제어 타이머 우회를 적용할 것인가.
	 * 설정자: tegra_pcie_dw_parse_dt 가 "nvidia,update-fc-fixup" 속성으로 정한다.
	 * 읽는 자: tegra_pcie_dw_host_init 과 pex_ep_event_pex_rst_deassert 가
	 *   CFG_TIMER_CTRL 의 ACK/NAK 자리에 1 을 세운다.
	 * 의미: **SoC 가 아니라 보드가 정하는 우회다** -- of_data 가 아니라
	 *   장치 트리에서 오기 때문이다. 앞 세대 pci-tegra.c 의 update_fc_timer 가
	 *   SoC 기술자에 있던 것과 대비된다.
	 * 동기화: probe 때 정하고 이후 불변 */
	bool update_fc_fixup;
	/* [한국어] 외부 참조 클록을 쓰는가.
	 * 설정자: tegra_pcie_dw_parse_dt. **세대에 따라 정하는 방식이 다르다** --
	 *   Tegra194 는 속성을 읽지 않고 EP 모드이면 무조건 참으로 놓고,
	 *   Tegra234 는 "nvidia,enable-ext-refclk" 속성을 읽는다. 위의 원문
	 *   주석대로 외부 REFCLK 를 쓰는 RC 는 Tegra234 부터 지원되기 때문이다.
	 * 읽는 자: tegra_pcie_config_controller, _unconfig_controller,
	 *   pex_ep_event_pex_rst_assert 와 _deassert 가 **UPHY PLL 을 BPMP 로
	 *   켜고 끌지** 정한다. 자기 클록을 쓰면 PLL 을 따로 다룰 필요가 없다.
	 * 동기화: probe 때 정하고 이후 불변 */
	bool enable_ext_refclk;
	/* [한국어] 링크가 처음 섰을 때의 폭.
	 * 설정자: tegra_pcie_enable_system_interrupts 가 링크가 선 뒤 읽어 둔다.
	 * 읽는 자: apply_bad_link_workaround 가 현재 폭과 비교해 링크가
	 *   나빠졌는지 판단한다.
	 * 값 범위: 협상된 레인 수(1, 2, 4, 8 등).
	 * 왜 필요한가: **"줄었는가" 를 판단하려면 기준이 있어야 하기 때문이다.**
	 *   앞 세대에는 이런 동적 감시가 없다.
	 * 동기화: 링크 기동 때 한 번 정하고 이후 읽기 전용 */
	u8 init_link_width;
	/* [한국어] **이 파일 어디에서도 쓰이지 않는 필드다.**
	 * 설정자: 없음. 읽는 자: 없음.
	 * 이름으로 미루어 MSI 제어 인터럽트 관련이었을 것이나, 현재 코드에서는
	 * 어떤 함수도 이 필드를 건드리지 않는다. 과거 구현의 잔재로 보이며,
	 * 그 근거는 이 트리에서 확인할 수 없다.
	 * 동기화: 해당 없음 */
	u32 msi_ctrl_int;
	/* [한국어] 이 포트의 레인 수.
	 * 설정자: tegra_pcie_dw_parse_dt 가 "num-lanes" 속성에서 읽는다. **필수** 라
	 *   없으면 probe 가 실패한다.
	 * 읽는 자: config_gen3_gen4_eq_presets 가 레인마다 이퀄라이저 프리셋을
	 *   쓰는 루프의 횟수로 쓴다. **그것이 유일한 용도다.**
	 * 주의: 앞 세대 pci-tegra.c 는 이 값을 하드웨어에 직접 반영했지만
	 *   (xbar 설정), 여기서는 링크 폭 자체를 하드웨어와 DWC 코어가 정하고
	 *   이 값은 프리셋 순회에만 쓰인다.
	 * 동기화: probe 때 정하고 이후 불변 */
	u32 num_lanes;
	/* [한국어] 이 컨트롤러의 번호.
	 * 설정자: tegra_pcie_dw_parse_dt 가 "nvidia,bpmp" 속성의 두 번째 원소에서
	 *   읽는다 -- 첫 번째는 BPMP 노드 참조이고 두 번째가 컨트롤러 ID 다.
	 * 읽는 자: **BPMP 메시지의 인자로 쓰인다** -- 펌웨어에게 어느 컨트롤러를
	 *   켜고 끌지 알리는 값이다. tegra_pcie_config_ep 가 인터럽트 이름을
	 *   지을 때도 쓴다.
	 * 값 범위: SoC 의 PCIe 컨트롤러 번호. Tegra194 의 5번이
	 *   tegra_pcie_bpmp_set_ctrl_state 에서 특별 취급된다.
	 * 동기화: probe 때 정하고 이후 불변 */
	u32 cid;
	/* [한국어] RAS-DES 벤더 확장 능력의 config 공간 오프셋.
	 * 설정자: **init_host_aspm 이 찾아 저장한다.**
	 * 읽는 자: event_counter_prog 가 이벤트 카운터 레지스터의 기준으로 쓴다.
	 * 값 범위: 유효한 확장 능력 오프셋. 능력이 없으면 0 이 되지만,
	 *   그 경우를 검사하는 코드는 없다.
	 * 주의: **CONFIG_PCIEASPM 이 꺼지면 init_host_aspm 이 빈 함수라 이 필드가
	 *   채워지지 않는다.** 다만 읽는 함수들도 같은 조건부 컴파일 안에 있어
	 *   함께 사라지므로 문제가 되지 않는다.
	 * 동기화: 초기화 때 정하고 이후 읽기 전용 */
	u32 ras_des_cap;
	/* [한국어] PCIe 능력 구조의 config 공간 오프셋.
	 * 설정자: tegra_pcie_dw_host_init(**아직 없을 때만**) 또는
	 *   pex_ep_event_pex_rst_deassert(무조건).
	 * 읽는 자: 링크 상태나 링크 제어 레지스터를 읽고 쓰는 모든 곳 --
	 *   tegra_pcie_icc_set, apply_bad_link_workaround,
	 *   tegra_pcie_rp_irq_handler, tegra_pcie_enable_system_interrupts,
	 *   tegra_pcie_dw_link_up 등.
	 * 왜 조건부인가: RC 경로에서 host_init 이 resume 때 다시 불리므로,
	 *   이미 찾았으면 중복 탐색을 피한다.
	 * 동기화: 초기화 때 정하고 이후 읽기 전용 */
	u32 pcie_cap_base;
	/* [한국어] ASPM T_cmrt 값(마이크로초).
	 * 설정자: tegra_pcie_dw_parse_dt 가 "nvidia,aspm-cmrt-us" 에서 읽는다.
	 *   **ASPM 파라미터 셋 중 이것만 필수** 라, 없으면 probe 가 실패한다.
	 * 읽는 자: init_host_aspm 이 L1 서브상태 능력 레지스터의 비트 8 자리에
	 *   넣는다.
	 * 의미: 보드가 알려 주는 값을 하드웨어 능력으로 광고해, 상대 장치가
	 *   그 시간을 지키게 한다.
	 * 동기화: probe 때 정하고 이후 불변 */
	u32 aspm_cmrt;
	/* [한국어] ASPM 전원 인가 시간(마이크로초).
	 * 설정자: tegra_pcie_dw_parse_dt 가 "nvidia,aspm-pwr-on-t-us" 에서 읽는다.
	 *   **선택 사항이라 없으면 로그만 찍고 0 으로 남는다.**
	 * 읽는 자: init_host_aspm 이 비트 19 자리에 넣는다.
	 * 주의: 0 이 의도된 기본값인지는 코드에 적혀 있지 않다.
	 * 동기화: probe 때 정하고 이후 불변 */
	u32 aspm_pwr_on_t;
	/* [한국어] L0s 진입 지연(마이크로초).
	 * 설정자: tegra_pcie_dw_parse_dt 가
	 *   "nvidia,aspm-l0s-entrance-latency-us" 에서 읽는다. 역시 선택 사항이다.
	 * 읽는 자: init_host_aspm 이 PCIE_PORT_AFR 의 L0s 진입 지연 필드에 넣는다.
	 * 동기화: probe 때 정하고 이후 불변 */
	u32 aspm_l0s_enter_lat;

	/* [한국어] 컨트롤러 IO 제어 전원.
	 * 설정자: tegra_pcie_dw_probe 가 이름 "vddio-pex-ctl" 로 얻는다.
	 *   **optional 이 아니라 필수** 다.
	 * 읽는 자: tegra_pcie_config_controller 가 켜고 _unconfig_controller 가 끈다.
	 * 특기할 점: 앞 세대 pci-tegra.c 가 SoC 마다 2~8개의 regulator 를 bulk 로
	 *   다루던 것과 달리, 여기서는 컨트롤러 전원이 이것 하나다 -- 전원 관리의
	 *   상당 부분이 BPMP 펌웨어로 넘어갔기 때문이다.
	 * 동기화: regulator 프레임워크 내부 락 */
	struct regulator *pex_ctl_supply;
	/* [한국어] 슬롯 3.3V 전원.
	 * 설정자: tegra_pcie_get_slot_regulators 가 optional 로 얻는다.
	 * 읽는 자: tegra_pcie_enable_slot_regulators 가 **먼저 켜고**,
	 *   _disable_slot_regulators 가 **나중에 끈다** -- 전원 시퀀싱 관례다.
	 * 값 범위: NULL 일 수 있다. 슬롯이 없거나 전원이 항상 켜져 있는 보드다.
	 * 동기화: regulator 프레임워크 내부 락 */
	struct regulator *slot_ctl_3v3;
	/* [한국어] 슬롯 12V 전원.
	 * 설정자: tegra_pcie_get_slot_regulators 가 optional 로 얻는다.
	 * 읽는 자: 3.3V 와 반대 순서로 켜고 끈다 -- 나중에 켜고 먼저 끈다.
	 * 값 범위: NULL 일 수 있다.
	 * 특기할 점: 둘 중 하나라도 켜면 100ms 를 기다린다. PCIe CEM 스펙의
	 *   T_PVPERL 이며, 그 대기가 있어야 링크를 세울 때 상대가 준비되어 있다.
	 * 동기화: regulator 프레임워크 내부 락 */
	struct regulator *slot_ctl_12v;

	/* [한국어] p2u PHY 의 개수.
	 * 설정자: tegra_pcie_dw_parse_dt 가 "phy-names" 속성의 문자열 개수로 정한다.
	 * 읽는 자: tegra_pcie_enable_phy 와 _disable_phy 의 순회 횟수,
	 *   probe 의 배열 할당 크기.
	 * 주의: disable 쪽은 이 값을 **지역 변수에 복사해** 감소시킨다 --
	 *   필드를 직접 줄이면 다음 호출에서 0 이 되어 버리기 때문이다.
	 * 동기화: probe 때 정하고 이후 불변 */
	unsigned int phy_count;
	/* [한국어] p2u PHY 배열.
	 * 설정자: tegra_pcie_dw_probe 가 "p2u-0", "p2u-1" 형식의 이름으로 하나씩
	 *   얻어 채운다.
	 * 읽는 자: tegra_pcie_enable_phy 와 _disable_phy.
	 * 값 범위: phy_count 개의 원소.
	 * 특기할 점: 앞 세대 pci-tegra.c 가 PHY 를 세 갈래(컨트롤러 단위 /
	 *   레인별 / PADS 레지스터 직접 조작)로 다루던 것과 달리 여기서는
	 *   종류가 하나뿐이다. 대신 UPHY PLL 은 BPMP 펌웨어가 따로 관리한다.
	 * 동기화: devm 이 관리. 설정 후 불변 */
	struct phy **phys;

	/* [한국어] debugfs 디렉터리 핸들.
	 * 설정자: init_debugfs 가 채운다. **CONFIG_PCIEASPM 이 꺼지면 빈 함수라
	 *   NULL 로 남는다.**
	 * 읽는 자: 파일을 만들 때 부모로 쓰고, remove 와 shutdown 이
	 *   debugfs_remove_recursive 로 지운다.
	 * 특기할 점: 디렉터리 이름을 **장치 트리 경로** 로 짓는다. 이 SoC 에
	 *   PCIe 컨트롤러가 여럿이기 때문이며, 앞 세대가 "pcie" 라는 고정 이름을
	 *   쓴 것과 대비된다.
	 * 동기화: 없음 */
	struct dentry *debugfs;

	/* Endpoint mode specific */
	/* [한국어] PERST GPIO. **EP 모드 전용이다.**
	 * 설정자: tegra_pcie_dw_parse_dt 가 EP 모드에서만 **입력** 으로 얻는다.
	 *   RC 모드에서는 얻지 않아 NULL 로 남는다.
	 * 읽는 자: tegra_pcie_config_ep 가 디바운스를 걸고 인터럽트로 바꾸며,
	 *   tegra_pcie_ep_pex_rst_irq 가 값을 읽어 어서트인지 해제인지 판단한다.
	 * **이 GPIO 가 EP 모드의 진입점이다** -- 호스트의 신호를 받는 통로이며,
	 *   엔드포인트는 이 신호를 기다렸다가 비로소 자기를 세운다.
	 *   RC 모드는 PERST 를 APPL 레지스터로 **내보내므로** 방향이 반대다.
	 * 동기화: GPIO 프레임워크 내부 락 */
	struct gpio_desc *pex_rst_gpiod;
	/* [한국어] REFCLK 소스 선택 GPIO. EP 모드 전용.
	 * 설정자: tegra_pcie_dw_parse_dt 가 optional 로 얻는다. 실패해도 NULL 로
	 *   두고 진행한다.
	 * 읽는 자: tegra_pcie_dw_probe 가 1 로 올리고, _remove 가 0 으로 내린다.
	 * 값 범위: NULL 일 수 있다.
	 * 의미: 이름으로 미루어 참조 클록의 출처를 고르는 신호이나,
	 *   probe 에서 한 번 올리고 remove 에서 내리는 것 외에 다른 조작이 없어
	 *   구체적 의미는 이 트리에서 확인할 수 없다.
	 * 동기화: GPIO 프레임워크 내부 락 */
	struct gpio_desc *pex_refclk_sel_gpiod;
	/* [한국어] PERST GPIO 에서 파생된 인터럽트 번호.
	 * 설정자: tegra_pcie_config_ep 가 gpiod_to_irq 로 얻는다.
	 * 읽는 자: **켜고 끄는 곳이 넷이다** -- tegra_pcie_dw_start_link(켬),
	 *   _stop_link(끔), _suspend(끔), _resume_early(켬), _remove 와
	 *   _shutdown(끔).
	 * 특기할 점: IRQ_NOAUTOEN 으로 등록해 request 시점에 자동으로 켜지지
	 *   않게 한다. DWC 코어가 start_link 를 부를 때 비로소 켜진다.
	 * 동기화: IRQ 코어가 관리 */
	unsigned int pex_rst_irq;
	/* [한국어] 엔드포인트가 현재 켜져 있는가.
	 * 설정자: tegra_pcie_config_ep 가 DISABLED 로 초기화하고,
	 *   pex_ep_event_pex_rst_assert 와 _deassert 가 각각 DISABLED/ENABLED 로
	 *   바꾼다.
	 * 읽는 자: 그 두 함수가 맨 앞에서 확인해 **중복 실행을 막는다** --
	 *   PERST 가 여러 번 흔들릴 수 있기 때문이다. tegra_pcie_dw_suspend 도
	 *   읽어, 동작 중이면 절전을 거부한다.
	 * 값 범위: EP_STATE_DISABLED(0) 또는 EP_STATE_ENABLED(1).
	 * 동기화: **락이 없다.** PERST 인터럽트가 IRQF_ONESHOT 스레드라
	 *   자기 자신과 겹치지 않고, suspend 는 인터럽트를 끈 뒤 읽는다는
	 *   전제로 보인다. */
	int ep_state;
	/* [한국어] 링크 업 사건을 하드 핸들러에서 스레드로 전달하는 비트.
	 * 설정자: tegra_pcie_ep_hard_irq 가 set_bit 으로 비트 0 을 세운다.
	 * 읽는 자: tegra_pcie_ep_irq_thread 가 test_and_clear_bit 으로 확인하고 지운다.
	 * 값 범위: 비트 0 만 쓴다.
	 * **원자적 비트 연산을 쓰는 이유** 가 여기 있다 -- 하드 인터럽트
	 *   컨텍스트와 스레드 컨텍스트가 같은 변수를 건드리므로, 락 없이
	 *   안전하게 주고받으려면 원자 연산이 필요하다. 타입이 long 인 것도
	 *   비트 연산 API 의 요구다.
	 * 동기화: 원자적 비트 연산 자체가 동기화 수단이다 */
	long link_status;
	/* [한국어] interconnect 대역폭 요청 경로.
	 * 설정자: tegra_pcie_dw_probe 가 devm_of_icc_get("write")으로 얻는다.
	 * 읽는 자: tegra_pcie_icc_set 이 icc_set_bw 로 필요한 대역폭을 요청한다.
	 * **앞 세대에 없던 연결이다.** PCIe 링크가 아무리 빨라도 SoC 내부
	 *   메모리 경로가 열려 있지 않으면 실제 대역폭이 나오지 않으므로,
	 *   협상된 속도와 폭에 맞춰 요청한다.
	 * 주의: 이름이 "write" 인 경로 하나만 얻는다 -- 읽기 방향은 별도로
	 *   요청하지 않는데, 그 이유는 코드에 적혀 있지 않다.
	 * 동기화: interconnect 프레임워크 내부 락 */
	struct icc_path *icc_path;
};

/* [한국어]
 * to_tegra_pcie - DWC 코어가 넘겨 준 포인터에서 이 드라이버 상태를 되찾는다
 *
 * @pci: struct tegra_pcie_dw 안에 값으로 박혀 있는 pci 필드의 주소.
 * @return: 그 pci 를 품고 있는 struct tegra_pcie_dw 포인터.
 *
 * **이 함수가 DWC 접착 계층의 접합점이다.** struct dw_pcie 는 독립된
 * 할당물이 아니라 struct tegra_pcie_dw 의 필드이고, DWC 코어는 그 안쪽
 * 포인터만 알고 있다. 그래서 콜백이 불릴 때마다 container_of 로 바깥
 * 구조체를 계산해 나온다.
 *
 * DWC 코어에서 오는 포인터가 두 종류라 변환도 두 단계인 경우가 많다.
 *   dw_pcie_rp 로 오면 : to_dw_pcie_from_pp 로 dw_pcie 를 얻고,
 *     다시 이 함수로 tegra_pcie_dw 를 얻는다.
 *   dw_pcie_ep 로 오면 : to_dw_pcie_from_ep 를 거쳐 같은 식으로 온다.
 * 그래서 이 파일의 여러 함수가 첫 두세 줄을 그 변환에 쓴다.
 *
 * 앞 세대 pci-tegra.c 의 msi_to_pcie 가 같은 관용구를 쓰지만 목적이
 * 다르다. 그쪽은 자기 안의 MSI 상태에서 거슬러 올라가는 것이고,
 * 이쪽은 **남의 코드(DWC 코어)가 아는 부분에서 자기 전체로** 나오는 것이다.
 *
 * 실행 컨텍스트: 어디서나. 포인터 산술뿐이다.
 *
 * 호출 체인:
 *   DWC 코어의 모든 콜백 → [이 함수]
 */
static inline struct tegra_pcie_dw *to_tegra_pcie(struct dw_pcie *pci)
{
	return container_of(pci, struct tegra_pcie_dw, pci);
}

/* [한국어]
 * appl_writel - APPL 레지스터에 쓴다
 *
 * @pcie:  드라이버 상태. appl_base 가 매핑되어 있어야 한다.
 * @value: 쓸 값.
 * @reg:   APPL 블록 안의 오프셋(APPL_ 로 시작하는 상수들).
 * @return: 없음.
 *
 * APPL 은 **NVIDIA 가 DWC IP 바깥에 덧붙인 접착 레지스터 블록** 이다.
 * 이 파일이 직접 읽고 쓰는 거의 전부가 여기이며, IP 코어 자신의
 * 레지스터(DBI)는 dw_pcie_readl_dbi 계열 도우미로만 접근한다.
 * 그 구분이 이 파일을 읽는 기준선이다 -- appl_ 로 시작하면 벤더 영역,
 * dw_pcie_ 로 시작하면 IP 영역이다.
 *
 * 앞 세대 pci-tegra.c 의 AFI 블록이 이 자리에 있었다. IP 를 갈아탔어도
 * "칩 바깥 배선과 전원은 벤더 몫" 이라는 구조가 그대로 남은 셈이다.
 *
 * **writel_relaxed 를 쓴다.** 앞 세대의 afi_writel 이 완화되지 않은
 * writel 을 쓰는 것과 다르다. 순서가 중요한 자리에서는 호출자가
 * 읽기를 끼워 넣거나 지연으로 보장한다.
 *
 * 인자 순서에 주의 -- 값이 먼저, 오프셋이 나중이다.
 *
 * 실행 컨텍스트: 어디서나. 인터럽트 핸들러도 이 함수를 쓴다.
 *
 * 호출 체인:
 *   이 파일의 대부분의 함수 → [이 함수] → writel_relaxed
 */
static inline void appl_writel(struct tegra_pcie_dw *pcie, const u32 value,
			       const u32 reg)
{
	writel_relaxed(value, pcie->appl_base + reg);
}

/* [한국어]
 * appl_readl - APPL 레지스터를 읽는다
 *
 * @pcie: 드라이버 상태.
 * @reg:  APPL 블록 안의 오프셋.
 * @return: 읽은 32비트 값.
 *
 * appl_writel 의 짝이다. 이 파일의 레지스터 갱신은 대부분
 * "읽고 → 비트 고치고 → 쓰기" 세 줄이라 두 함수가 늘 붙어 다닌다.
 *
 * 인터럽트 핸들러가 상태 레지스터를 읽는 데도 쓰이는데, APPL 의 인터럽트
 * 상태는 **2단 구조** 다 -- L0 레지스터가 어느 범주인지 알려 주고,
 * 그 범주의 L1_x 레지스터가 세부를 알려 준다. 그래서 핸들러가 이 함수를
 * 연달아 두 번 부르는 모양이 반복된다.
 *
 * 실행 컨텍스트: 어디서나.
 *
 * 호출 체인:
 *   이 파일의 대부분의 함수 → [이 함수] → readl_relaxed
 */
static inline u32 appl_readl(struct tegra_pcie_dw *pcie, const u32 reg)
{
	return readl_relaxed(pcie->appl_base + reg);
}

/* [한국어]
 * tegra_pcie_icc_set - 협상된 링크 속도와 폭에 맞춰 메모리 대역폭과 코어 클록을 조정한다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음. 대역폭 요청이 실패해도 로그만 찍는다.
 *
 * **앞 세대에 없던 연결이다.** PCIe 링크가 아무리 빨라도 SoC 내부의
 * 메모리 경로가 그만큼 열려 있지 않으면 실제 대역폭이 나오지 않는다.
 * 그래서 링크가 선 뒤 실제 속도와 폭을 읽어, 필요한 만큼을 interconnect
 * 프레임워크에 요청한다.
 *
 * 동작:
 *   1) 링크 상태 레지스터에서 협상된 속도와 폭을 읽는다.
 *   2) 필요한 대역폭을 계산한다 -- 레인 수 × 레인당 Mbps.
 *      PCIE_SPEED2MBS_ENC 가 속도 코드를 Mbps 로 바꾸고,
 *      pcie_get_link_speed(drivers/pci/probe.c:2017)가 세대 코드를 속도
 *      enum 으로 바꾼다. **PCIE_SPEED2MBS_ENC 와 Mbps_to_icc 의 정의는
 *      이 스파스 체크아웃에 없어 확인하지 못했다** -- 이름과 쓰임으로만
 *      설명한다.
 *   3) 코어 클록을 그 세대에 맞는 주파수로 바꾼다. pcie_gen_freq 표가
 *      Gen1 62.5MHz 부터 Gen4 500MHz 까지를 담고 있다.
 *      범위를 넘는 속도 코드는 0(Gen1 주파수)으로 눌러 배열 밖 접근을 막는다.
 *
 * **NVMe 관점에서 의미 있는 함수다.** 링크가 Gen4 x4 로 섰다면 그만큼의
 * 메모리 대역폭이 확보되어야 SSD 가 제 성능을 낸다.
 *
 * RC 와 EP 양쪽에서 불린다 -- RC 는 링크가 선 직후
 * (tegra_pcie_dw_start_link), EP 는 호스트와 링크가 맺어진 뒤
 * (tegra_pcie_ep_irq_thread)다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 스레드 인터럽트 핸들러.
 * clk_set_rate 가 잠들 수 있어 하드 인터럽트 컨텍스트에서 불리면 안 된다 --
 * EP 경로가 스레드 핸들러인 이유 중 하나다.
 *
 * 호출 체인:
 *   tegra_pcie_dw_start_link / tegra_pcie_ep_irq_thread → [이 함수]
 *     → dw_pcie_readw_dbi, icc_set_bw, clk_set_rate
 */
static void tegra_pcie_icc_set(struct tegra_pcie_dw *pcie)
{
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 레지스터 값과 협상된 속도·폭을 담을 변수들 */
	u32 val, speed, width;

	/* [한국어] **실제로 협상된 결과를 읽는다** -- 요청한 값이 아니라 상대와 합의된
	 * 값이라야 필요한 대역폭을 정확히 계산할 수 있다 */
	val = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base + PCI_EXP_LNKSTA);

	/* [한국어] 협상된 링크 속도를 뽑는다 */
	speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, val);
	/* [한국어] 협상된 링크 폭을 뽑는다 */
	width = FIELD_GET(PCI_EXP_LNKSTA_NLW, val);

	/* [한국어] **레인 수 × 레인당 Mbps 로 필요한 대역폭을 계산한다.**
	 * pcie_get_link_speed(drivers/pci/probe.c:2017)가 세대 코드를 속도
	 * enum 으로 바꾸고, PCIE_SPEED2MBS_ENC 가 그것을 Mbps 로 바꾼다.
	 * 뒤 매크로의 정의는 이 트리에 없어 이름과 쓰임으로만 설명한다 */
	val = width * PCIE_SPEED2MBS_ENC(pcie_get_link_speed(speed));

	/* [한국어] **필요한 대역폭을 interconnect 에 요청한다.** 앞 세대에 없던
	 * 연결이며, PCIe 링크가 빨라도 SoC 내부 메모리 경로가 열려 있지 않으면
	 * 실제 대역폭이 나오지 않기 때문이다.
	 * Mbps_to_icc 의 정의는 이 스파스 체크아웃에 없어 확인하지 못했다 */
	if (icc_set_bw(pcie->icc_path, Mbps_to_icc(val), 0))
		/* [한국어] 대역폭 요청 실패를 알린다. **실패해도 진행한다** -- 링크 자체는
		 * 동작하기 때문이다 */
		dev_err(pcie->dev, "can't set bw[%u]\n", val);

	/* [한국어] 표 범위를 벗어나는지 확인한다 */
	if (speed >= ARRAY_SIZE(pcie_gen_freq))
		/* [한국어] **범위를 넘는 속도 코드는 0 으로 눌러** 배열 밖 접근을 막는다.
		 * 인덱스 0 은 Gen1 주파수라 안전한 기본값이 된다 */
		speed = 0;

	clk_set_rate(pcie->core_clk, pcie_gen_freq[speed]);
}

/* [한국어]
 * apply_bad_link_workaround - 링크 폭이 줄었으면 Gen1 으로 낮춰 재훈련한다
 *
 * @pp: DWC 루트 포트 구조체.
 * @return: 없음.
 *
 * 링크 품질이 나빠 폭이 줄어든 상황을 다룬다. 링크가 처음 섰을 때의 폭
 * (init_link_width)보다 지금 폭이 좁으면, 속도를 2.5GT/s 로 낮추고
 * 재훈련을 요청한다. 느리더라도 안정적인 링크가 낫다는 판단이다.
 *
 * **대역폭 관리 인터럽트가 이 함수를 부른다.** 링크 폭이나 속도가 자동으로
 * 바뀌면 하드웨어가 인터럽트를 올리고, tegra_pcie_rp_irq_handler 가
 * 그것을 받아 이 함수로 온다.
 *
 * 위의 원문 주석이 중요한 선택을 밝힌다 -- 실제로 Gen2 로 내려갔는지
 * 확인하지 않고 넘어간다. 흔치 않은 상황이고 링크가 어차피 불안정하므로
 * 기다릴 가치가 없다는 것이다.
 *
 * LBMS(Link Bandwidth Management Status) 비트를 먼저 확인하는데, 그것이
 * "대역폭이 자동으로 바뀌었다" 는 표시다. 그 비트가 없으면 아무것도 하지
 * 않는다.
 *
 * 앞 세대 pci-tegra.c 에는 이런 동적 대응이 없다. 그쪽은 부팅 때
 * Gen1 에서 Gen2 로 한 번 올리고 끝이다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(tegra_pcie_rp_irq_handler 안).
 *
 * 호출 체인:
 *   tegra_pcie_rp_irq_handler → [이 함수] → dw_pcie_readw_dbi / _writew_dbi
 */
static void apply_bad_link_workaround(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);
	/* [한국어] 현재 링크 폭을 담을 변수 */
	u32 current_link_width;
	/* [한국어] 링크 상태 값. 16비트라 val_w 계열과 형이 같다 */
	u16 val;

	/*
	 * NOTE:- Since this scenario is uncommon and link as such is not
	 * stable anyway, not waiting to confirm if link is really
	 * transitioning to Gen-2 speed
	 */
	val = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base + PCI_EXP_LNKSTA);
	/* [한국어] **대역폭이 자동으로 바뀌었다는 표시가 있을 때만 처리한다** */
	if (val & PCI_EXP_LNKSTA_LBMS) {
		/* [한국어] 현재 링크 폭을 뽑는다 */
		current_link_width = FIELD_GET(PCI_EXP_LNKSTA_NLW, val);
		/* [한국어] **처음 섰을 때보다 좁아졌는지 본다.** 그 기준값은
		 * tegra_pcie_enable_system_interrupts 가 링크가 선 직후 기록해 둔 것이다 */
		if (pcie->init_link_width > current_link_width) {
			/* [한국어] 링크 품질이 나빠졌음을 알린다 */
			dev_warn(pci->dev, "PCIe link is bad, width reduced\n");
			/* [한국어] 링크 제어 2 레지스터를 읽는다 */
			val = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base +
						PCI_EXP_LNKCTL2);
			/* [한국어] 기존 목표 속도를 지운다 */
			val &= ~PCI_EXP_LNKCTL2_TLS;
			/* [한국어] 2.5GT/s(Gen1)를 넣는다 -- **느리더라도 안정적인 링크가 낫다는
			 * 판단이다** */
			val |= PCI_EXP_LNKCTL2_TLS_2_5GT;
			/* [한국어] 목표 속도를 쓴다 */
			dw_pcie_writew_dbi(pci, pcie->pcie_cap_base +
					   PCI_EXP_LNKCTL2, val);

			/* [한국어] 링크 제어 레지스터를 읽는다 */
			val = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base +
						PCI_EXP_LNKCTL);
			/* [한국어] Retrain Link 비트를 세운다 */
			val |= PCI_EXP_LNKCTL_RL;
			/* [한국어] 재훈련을 요청한다 */
			dw_pcie_writew_dbi(pci, pcie->pcie_cap_base +
					   PCI_EXP_LNKCTL, val);
		}
	}
}

/* [한국어]
 * tegra_pcie_rp_irq_handler - 루트 포트 모드의 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: devm_request_irq 에 넘긴 struct tegra_pcie_dw.
 * @return: 항상 IRQ_HANDLED.
 *
 * APPL 의 **2단 인터럽트 상태 구조** 를 훑는다. L0 레지스터가 어느 범주에
 * 사건이 있는지 알려 주고, 각 범주의 L1_x 레지스터가 세부를 알려 준다.
 * 그래서 이 함수는 "L0 비트를 확인 → 해당 L1 을 읽음 → 처리" 를 세 번
 * 반복하는 모양이다.
 *
 * 세 범주:
 *   링크 상태 변화 : L1_0_0 을 읽고 곧바로 되써서 지운다(1을 쓰면 지워지는
 *     방식). has_sbr_reset_fix 가 없는 SoC(Tegra194)에서만 우회를 적용하는데,
 *     코어 리셋을 소프트웨어로 한 번 흔들고 속도 변경을 요청한다.
 *     주석이 "SBR & Surprise Link Down WAR" 라고 밝히듯 보조 버스 리셋과
 *     갑작스러운 링크 단절을 다루는 우회다.
 *   INT 범주 : 대역폭 관련 둘을 처리한다. 자동 대역폭 변경이면
 *     apply_bad_link_workaround 를 부르고, 대역폭 관리 이벤트면
 *     LBMS 비트를 지우고 현재 속도를 디버그 로그로 남긴다.
 *     **상태 비트를 지우는 순서가 둘이 다르다** -- 앞은 지운 뒤 처리하고,
 *     뒤는 DBI 쪽 비트를 먼저 지운 뒤 APPL 비트를 지운다.
 *   CDM 검사 : IP 의 설정 레지스터 무결성 검사 결과다. 완료, 비교 불일치,
 *     논리 오류 셋을 각각 확인해 DBI 쪽 상태 레지스터에 되쓰고,
 *     오류 주소를 찍는다. 앞 세대에 없던 기능이다.
 *
 * 항상 IRQ_HANDLED 를 돌려주는 데 주의한다. IRQF_SHARED 로 요청되지만
 * 자기 것이 아닌 인터럽트를 걸러 내지 않는다 -- 앞 세대 tegra_pcie_isr 이
 * INTx 코드에 IRQ_NONE 을 돌려주는 것과 대비된다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 스레드 핸들러가 아니므로 잠들 수 없다.
 * EP 쪽이 스레드를 쓰는 것과 대비되는데, RC 쪽은 잠들어야 하는 작업이
 * 없기 때문이다.
 *
 * 호출 체인:
 *   APPL 인터럽트 → 커널 → [이 함수]
 *     → apply_bad_link_workaround, dw_pcie_read/writel_dbi
 */
static irqreturn_t tegra_pcie_rp_irq_handler(int irq, void *arg)
{
	struct tegra_pcie_dw *pcie = arg;
	/* [한국어] DBI 접근에 쓸 코어 구조체 */
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 루트 포트 구조체. apply_bad_link_workaround 에 넘긴다 */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 레지스터 값과 두 층의 상태를 담을 변수들 */
	u32 val, status_l0, status_l1;
	/* [한국어] 16비트 레지스터 값 임시 변수 */
	u16 val_w;

	/* [한국어] **L0 상태를 먼저 읽어 어느 범주인지 확인한다.** 아래에서 세 범주를
	 * 차례로 검사하는 2단 구조의 출발점이다 */
	status_l0 = appl_readl(pcie, APPL_INTR_STATUS_L0);
	/* [한국어] 링크 상태 변화 범주에 사건이 있는지 본다 */
	if (status_l0 & APPL_INTR_STATUS_L0_LINK_STATE_INT) {
		/* [한국어] 링크 상태 범주의 세부 상태를 읽는다 */
		status_l1 = appl_readl(pcie, APPL_INTR_STATUS_L1_0_0);
		/* [한국어] 읽은 값을 되써서 지운다 */
		appl_writel(pcie, status_l1, APPL_INTR_STATUS_L1_0_0);
		/* [한국어] **결함이 고쳐지지 않은 SoC(Tegra194)에서 링크 리셋 요청이 왔을 때만
		 * 우회를 적용한다.** 주석이 밝히듯 보조 버스 리셋과 갑작스러운 링크
		 * 단절을 다루는 우회다 */
		if (!pcie->of_data->has_sbr_reset_fix &&
		    status_l1 & APPL_INTR_STATUS_L1_0_0_LINK_REQ_RST_NOT_CHGED) {
			/* SBR & Surprise Link Down WAR */
			val = appl_readl(pcie, APPL_CAR_RESET_OVRD);
			/* [한국어] 코어 리셋 덮어쓰기 비트를 지운다 */
			val &= ~APPL_CAR_RESET_OVRD_CYA_OVERRIDE_CORE_RST_N;
			/* [한국어] 덮어쓰기를 해제한다 */
			appl_writel(pcie, val, APPL_CAR_RESET_OVRD);
			udelay(1);
			/* [한국어] 현재 값을 다시 읽는다 */
			val = appl_readl(pcie, APPL_CAR_RESET_OVRD);
			/* [한국어] 덮어쓰기 비트를 다시 세운다 */
			val |= APPL_CAR_RESET_OVRD_CYA_OVERRIDE_CORE_RST_N;
			/* [한국어] 덮어쓰기를 되돌린다 -- **이 세 줄이 소프트웨어로 만든 코어 리셋
			 * 펄스다** */
			appl_writel(pcie, val, APPL_CAR_RESET_OVRD);

			/* [한국어] IP 의 링크 폭/속도 제어 레지스터를 읽는다 */
			val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
			/* [한국어] 속도 변경 비트를 세운다 */
			val |= PORT_LOGIC_SPEED_CHANGE;
			/* [한국어] 속도 변경을 요청한다 */
			dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);
		}
	}

	/* [한국어] INT 범주에 사건이 있는지 본다 */
	if (status_l0 & APPL_INTR_STATUS_L0_INT_INT) {
		/* [한국어] INT 범주의 세부 상태를 읽는다 */
		status_l1 = appl_readl(pcie, APPL_INTR_STATUS_L1_8_0);
		/* [한국어] 자동 대역폭 변경인지 본다 */
		if (status_l1 & APPL_INTR_STATUS_L1_8_0_AUTO_BW_INT_STS) {
			appl_writel(pcie,
				    APPL_INTR_STATUS_L1_8_0_AUTO_BW_INT_STS,
				    APPL_INTR_STATUS_L1_8_0);
			apply_bad_link_workaround(pp);
		}
		/* [한국어] 대역폭 관리 이벤트인지 본다. **위의 자동 변경과 상태를 지우는
		 * 순서가 다르다** -- 앞은 지운 뒤 처리하고, 뒤는 IP 쪽 비트를 먼저
		 * 지운 뒤 APPL 비트를 지운다 */
		if (status_l1 & APPL_INTR_STATUS_L1_8_0_BW_MGT_INT_STS) {
			/* [한국어] 링크 상태를 읽는다 */
			val_w = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base +
						  PCI_EXP_LNKSTA);
			/* [한국어] 대역폭 관리 상태 비트를 세운다 -- **1을 쓰면 지워지는 자리다** */
			val_w |= PCI_EXP_LNKSTA_LBMS;
			/* [한국어] LBMS 비트를 되써서 지운다 */
			dw_pcie_writew_dbi(pci, pcie->pcie_cap_base +
					   PCI_EXP_LNKSTA, val_w);

			appl_writel(pcie,
				    APPL_INTR_STATUS_L1_8_0_BW_MGT_INT_STS,
				    APPL_INTR_STATUS_L1_8_0);

			/* [한국어] **지운 뒤 다시 읽는다** -- 로그에 찍을 최신 속도를 얻기 위해서다 */
			val_w = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base +
						  PCI_EXP_LNKSTA);
			/* [한국어] 현재 링크 속도를 디버그 로그로 남긴다 */
			dev_dbg(pci->dev, "Link Speed : Gen-%u\n", val_w &
				PCI_EXP_LNKSTA_CLS);
		}
	}

	/* [한국어] CDM 검사 결과 범주를 본다. **앞 세대에 없던 기능이다** */
	if (status_l0 & APPL_INTR_STATUS_L0_CDM_REG_CHK_INT) {
		/* [한국어] CDM 검사의 세부 상태를 읽는다. **이 범주만 상태를 되써서 지우지
		 * 않는다** -- 아래에서 IP 쪽 레지스터에 결과를 쓰는 것이 그 역할을 한다 */
		status_l1 = appl_readl(pcie, APPL_INTR_STATUS_L1_18);
		/* [한국어] IP 쪽 CDM 상태 레지스터를 읽는다. 확인한 결과를 여기에 되쓴다 */
		val = dw_pcie_readl_dbi(pci, PCIE_PL_CHK_REG_CONTROL_STATUS);
		/* [한국어] **활성화하지 않은 완료 비트도 읽는다** --
		 * tegra_pcie_enable_system_interrupts 가 오류 둘만 켜지만, 다른 원인으로
		 * 인터럽트가 왔을 때 이 비트가 함께 서 있을 수 있기 때문이다 */
		if (status_l1 & APPL_INTR_STATUS_L1_18_CDM_REG_CHK_CMPLT) {
			/* [한국어] 검사 완료를 알린다 */
			dev_info(pci->dev, "CDM check complete\n");
			/* [한국어] 완료 비트를 모아 둔다 */
			val |= PCIE_PL_CHK_REG_CHK_REG_COMPLETE;
		}
		/* [한국어] 비교 불일치가 있었는지 본다 */
		if (status_l1 & APPL_INTR_STATUS_L1_18_CDM_REG_CHK_CMP_ERR) {
			/* [한국어] 비교 불일치를 알린다 */
			dev_err(pci->dev, "CDM comparison mismatch\n");
			/* [한국어] 비교 불일치 비트를 모아 둔다 */
			val |= PCIE_PL_CHK_REG_CHK_REG_COMPARISON_ERROR;
		}
		/* [한국어] 논리 오류가 있었는지 본다 */
		if (status_l1 & APPL_INTR_STATUS_L1_18_CDM_REG_CHK_LOGIC_ERR) {
			/* [한국어] 논리 오류를 알린다 */
			dev_err(pci->dev, "CDM Logic error\n");
			/* [한국어] 논리 오류 비트를 모아 둔다 */
			val |= PCIE_PL_CHK_REG_CHK_REG_LOGIC_ERROR;
		}
		/* [한국어] 확인한 결과들을 IP 쪽 상태 레지스터에 되쓴다 */
		dw_pcie_writel_dbi(pci, PCIE_PL_CHK_REG_CONTROL_STATUS, val);
		/* [한국어] 오류 주소를 읽는다 */
		val = dw_pcie_readl_dbi(pci, PCIE_PL_CHK_REG_ERR_ADDR);
		/* [한국어] 오류가 난 레지스터의 오프셋을 찍는다. **CDM 검사의 결과물이며,
		 * 어느 레지스터가 손상되었는지 알려 준다** */
		dev_err(pci->dev, "CDM Error Address Offset = 0x%08X\n", val);
	}

	return IRQ_HANDLED;
}

/* [한국어]
 * pex_ep_event_hot_rst_done - 엔드포인트가 핫 리셋을 마친 뒤 정리하고 LTSSM 을 다시 켠다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음.
 *
 * 호스트가 이 엔드포인트에 핫 리셋을 걸면 하드웨어가 리셋을 수행하고
 * 완료를 알리는데, 그때 이 함수가 뒷정리를 한다.
 *
 * **APPL 의 인터럽트 상태 레지스터를 전부 0xFFFFFFFF 로 지운다.**
 * 열여섯 줄이 이어지는데, 리셋 과정에서 쌓인 상태를 모두 버리는 것이다.
 * 1을 쓰면 지워지는 방식이라 전체 비트를 세워 쓴다.
 * 같은 목록이 tegra_pcie_enable_interrupts 와
 * pex_ep_event_pex_rst_deassert 에도 나온다 -- 다만 이 함수만
 * APPL_MSI_CTRL_2 를 함께 지운다.
 *
 * 그 뒤 LTSSM 을 다시 켠다. 핫 리셋으로 꺼졌던 링크 훈련을 재개하는
 * 것이며, 이 한 줄이 이 함수의 실질적 목적이다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(EP 하드 핸들러 안).
 *
 * 호출 체인:
 *   tegra_pcie_ep_hard_irq → [이 함수] → appl_writel
 */
static void pex_ep_event_hot_rst_done(struct tegra_pcie_dw *pcie)
{
	u32 val;

	/* [한국어] **리셋 과정에서 쌓인 상태를 모두 버린다.** 1을 쓰면 지워지는
	 * 방식이라 전체 비트를 세워 쓴다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L0);
	/* [한국어] 링크 상태 범주를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_0_0);
	/* [한국어] L1_1 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_1);
	/* [한국어] L1_2 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_2);
	/* [한국어] L1_3 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_3);
	/* [한국어] L1_6 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_6);
	/* [한국어] L1_7 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_7);
	/* [한국어] INT 범주 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_8_0);
	/* [한국어] L1_9 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_9);
	/* [한국어] L1_10 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_10);
	/* [한국어] L1_11 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_11);
	/* [한국어] L1_13 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_13);
	/* [한국어] L1_14 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_14);
	/* [한국어] L1_15 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_15);
	/* [한국어] 마지막 상태 레지스터 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_17);
	/* [한국어] **이 함수만 MSI 제어 상태를 하나 더 지운다** -- 다른 두 지우기
	 * 목록에는 없는 줄이다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_MSI_CTRL_2);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_CTRL);
	/* [한국어] LTSSM 을 다시 켠다. **핫 리셋으로 꺼졌던 링크 훈련을 재개하는
	 * 것이며, 이 한 줄이 이 함수의 실질적 목적이다** */
	val |= APPL_CTRL_LTSSM_EN;
	appl_writel(pcie, val, APPL_CTRL);
}

/* [한국어]
 * tegra_pcie_ep_irq_thread - 엔드포인트 인터럽트의 스레드 절반
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: struct tegra_pcie_dw.
 * @return: 항상 IRQ_HANDLED.
 *
 * 하드 핸들러(tegra_pcie_ep_hard_irq)가 IRQ_WAKE_THREAD 를 돌려주면
 * 커널이 이 함수를 스레드 컨텍스트에서 부른다. **잠들 수 있는 작업만
 * 여기 모아 둔 것** 이 두 함수를 나눈 이유다.
 *
 * 세 가지를 한다.
 *   1) 링크업 통보. 하드 핸들러가 세워 둔 비트를 test_and_clear 로
 *      원자적으로 확인하고 지운 뒤, DWC EP 코어에 링크가 섰다고 알린다.
 *      원자 연산을 쓰는 것은 하드 핸들러와 이 스레드가 같은 변수를
 *      건드리기 때문이다.
 *   2) 대역폭과 클록을 협상 결과에 맞춘다.
 *   3) **LTR 메시지를 상류로 보낸다.** 이것이 잠들 수 있는 작업이다 --
 *      아래에서 최대 100ms 를 폴링한다.
 *
 * LTR 을 보내는 조건이 세 겹이다.
 *   has_ltr_req_fix 인 SoC(Tegra234 EP)는 아예 건너뛴다. 하드웨어가
 *     알아서 하므로 소프트웨어가 나설 필요가 없다는 뜻이며, 대신
 *     pex_ep_event_pex_rst_deassert 가 초기화 때 한 번 설정해 둔다.
 *   L1 서브상태를 지원하지 않으면 LTR 이 의미가 없어 건너뛴다.
 *   버스 마스터 비트가 서 있어야 한다 -- 호스트가 이 장치를 실제로
 *     쓰기 시작했다는 표시다.
 *
 * 폴링 루프가 요청 비트가 내려가기를 기다리되, 시간이 다 되면 그냥
 * 빠져나온다. 그 뒤 비트가 아직 서 있으면 실패를 알린다.
 * 루프 안에서 시간 초과를 확인하고 **그 뒤에 잠드는** 순서라, 마지막
 * 바퀴에서 불필요하게 한 번 더 자지 않는다.
 *
 * 실행 컨텍스트: 스레드 인터럽트 컨텍스트. usleep_range 로 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_pcie_ep_hard_irq(IRQ_WAKE_THREAD) → 커널 → [이 함수]
 *     → dw_pcie_ep_linkup, tegra_pcie_icc_set
 */
static irqreturn_t tegra_pcie_ep_irq_thread(int irq, void *arg)
{
	struct tegra_pcie_dw *pcie = arg;
	/* [한국어] 링크업 통보에 쓸 EP 구조체 */
	struct dw_pcie_ep *ep = &pcie->pci.ep;
	/* [한국어] DBI 접근에 쓸 코어 구조체 */
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* [한국어] **하드 핸들러가 세워 둔 비트를 원자적으로 확인하고 지운다.**
	 * 두 컨텍스트가 같은 변수를 건드리므로 락 없이 안전하게 주고받으려면
	 * 원자 연산이 필요하다 */
	if (test_and_clear_bit(0, &pcie->link_status))
		dw_pcie_ep_linkup(ep);

	tegra_pcie_icc_set(pcie);

	/* [한국어] **하드웨어가 알아서 하는 SoC(Tegra234 EP)는 여기서 끝낸다.**
	 * 대신 pex_ep_event_pex_rst_deassert 가 초기화 때 한 번 권한을 켜 둔다 */
	if (pcie->of_data->has_ltr_req_fix)
		return IRQ_HANDLED;

	/* If EP doesn't advertise L1SS, just return */
	if (!pci->l1ss_support)
		return IRQ_HANDLED;

	/* Check if BME is set to '1' */
	val = dw_pcie_readl_dbi(pci, PCI_COMMAND);
	/* [한국어] **버스 마스터 비트가 서 있어야 한다** -- 호스트가 이 장치를 실제로
	 * 쓰기 시작했다는 표시이며, 그때에야 LTR 이 의미를 갖는다 */
	if (val & PCI_COMMAND_MASTER) {
		/* [한국어] ktime 기반 시한 */
		ktime_t timeout;

		/* Send LTR upstream */
		val = appl_readl(pcie, APPL_LTR_MSG_2);
		/* [한국어] LTR 전송 요청 비트를 세운다 */
		val |= APPL_LTR_MSG_2_LTR_MSG_REQ_STATE;
		/* [한국어] 요청을 내보낸다 */
		appl_writel(pcie, val, APPL_LTR_MSG_2);

		/* [한국어] 100ms 시한을 잡는다 */
		timeout = ktime_add_us(ktime_get(), LTR_MSG_TIMEOUT);
		/* [한국어] **무한 루프로 두고 안에서 두 가지 탈출 조건을 본다** */
		for (;;) {
			/* [한국어] 현재 상태를 읽는다 */
			val = appl_readl(pcie, APPL_LTR_MSG_2);
			/* [한국어] 요청 비트가 내려가면 전송이 끝난 것이다 */
			if (!(val & APPL_LTR_MSG_2_LTR_MSG_REQ_STATE))
				break;
			/* [한국어] 시간이 다 됐으면 그냥 빠져나온다 */
			if (ktime_after(ktime_get(), timeout))
				break;
			/* [한국어] 1~1.1ms 잠든다. **시간 초과 확인 뒤에 자므로** 마지막 바퀴에서
			 * 불필요하게 한 번 더 자지 않는다 */
			usleep_range(1000, 1100);
		}
		/* [한국어] 루프를 빠져나온 뒤 비트가 아직 서 있으면 시간 초과였다는 뜻이다 */
		if (val & APPL_LTR_MSG_2_LTR_MSG_REQ_STATE)
			/* [한국어] LTR 전송 실패를 알린다 */
			dev_err(pcie->dev, "Failed to send LTR message\n");
	}

	return IRQ_HANDLED;
}

/* [한국어]
 * tegra_pcie_ep_hard_irq - 엔드포인트 인터럽트의 하드 절반
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: struct tegra_pcie_dw.
 * @return: IRQ_HANDLED, 또는 스레드가 필요하면 IRQ_WAKE_THREAD.
 *
 * APPL L0 상태를 읽어 **스레드를 깨울지 여기서 끝낼지 정한다.**
 *
 *   링크 상태 변화 : L1_0_0 을 읽고 되써서 지운다.
 *     핫 리셋 완료면 곧바로 정리하고 LTSSM 을 켠다 -- 잠들지 않는
 *       작업이라 여기서 처리한다.
 *     링크가 올라왔으면 **비트를 세우고 스레드를 깨운다.** 뒤이은 작업
 *       (EP 코어 통보, 대역폭 조정, LTR)이 모두 잠들 수 있기 때문이다.
 *   PCI 명령 변화 : 버스 마스터 활성화 비트가 바뀌었으면 스레드를 깨운다.
 *     호스트가 장치를 쓰기 시작했다는 뜻이라 LTR 을 보내야 한다.
 *   INT 범주 : eDMA 인터럽트면 **처리하지 않는다.** 위의 원문 주석대로
 *     DMA 드라이버가 따로 처리하므로, 여기서는 "미확인 인터럽트가 아니다"
 *     라고만 표시한다.
 *
 * spurious 플래그가 이 함수의 구조를 만든다. 아는 범주를 하나라도 만나면
 * 0 으로 내리고, 끝까지 1 이면 알 수 없는 인터럽트로 보고 L0 상태를
 * 통째로 지운다. 원인을 모르는 채 인터럽트가 계속 올라오는 것을 막는
 * 방어다.
 *
 * **주의할 점**: 링크업이나 BME 변화로 IRQ_WAKE_THREAD 를 돌려주면
 * 그 아래 범주는 검사하지 않고 함수를 빠져나간다. 같은 인터럽트에
 * eDMA 사건이 함께 있었다면 이번에는 spurious 판정을 거치지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들 수 없다.
 *
 * 호출 체인:
 *   APPL 인터럽트 → 커널 → [이 함수]
 *     → pex_ep_event_hot_rst_done, (필요시) tegra_pcie_ep_irq_thread
 */
static irqreturn_t tegra_pcie_ep_hard_irq(int irq, void *arg)
{
	struct tegra_pcie_dw *pcie = arg;
	/* [한국어] **미확인 인터럽트 판정 플래그.** 아는 범주를 하나라도 만나면 0 으로
	 * 내리고, 끝까지 1 이면 원인을 모르는 인터럽트로 보고 상태를 지운다 */
	int spurious = 1;
	/* [한국어] L0/L1 상태와 링크 상태를 담을 변수들 */
	u32 status_l0, status_l1, link_status;

	/* [한국어] **L0 상태를 먼저 읽어 어느 범주인지 확인한다.** APPL 인터럽트가
	 * 2단 구조라 이 값이 아래 L1_x 읽기의 길잡이가 된다 */
	status_l0 = appl_readl(pcie, APPL_INTR_STATUS_L0);
	/* [한국어] 링크 상태 변화 범주에 사건이 있는지 본다 */
	if (status_l0 & APPL_INTR_STATUS_L0_LINK_STATE_INT) {
		/* [한국어] 링크 상태 범주의 세부 상태를 읽는다 */
		status_l1 = appl_readl(pcie, APPL_INTR_STATUS_L1_0_0);
		/* [한국어] 읽은 값을 되써서 지운다 */
		appl_writel(pcie, status_l1, APPL_INTR_STATUS_L1_0_0);

		/* [한국어] **핫 리셋 완료는 여기서 처리한다** -- 잠들지 않는 작업이라
		 * 스레드를 깨울 필요가 없다 */
		if (status_l1 & APPL_INTR_STATUS_L1_0_0_HOT_RESET_DONE)
			pex_ep_event_hot_rst_done(pcie);

		/* [한국어] 링크 업 상태가 바뀌었는지 본다 */
		if (status_l1 & APPL_INTR_STATUS_L1_0_0_RDLH_LINK_UP_CHGED) {
			/* [한국어] **알림만으로는 오른 것인지 내린 것인지 알 수 없어** 실제 상태를
			 * 따로 읽는다 */
			link_status = appl_readl(pcie, APPL_LINK_STATUS);
			/* [한국어] 실제로 링크가 올라왔는지 확인한다 */
			if (link_status & APPL_LINK_STATUS_RDLH_LINK_UP) {
				/* [한국어] 링크가 올라왔음을 디버그 로그로 남긴다 */
				dev_dbg(pcie->dev, "Link is up with Host\n");
				/* [한국어] **비트를 세워 스레드에게 알린다.** 원자적 비트 연산을 쓰는 것은
				 * 하드 인터럽트 컨텍스트와 스레드가 같은 변수를 건드리기 때문이다 */
				set_bit(0, &pcie->link_status);
				return IRQ_WAKE_THREAD;
			}
		}

		/* [한국어] 아는 범주를 만났으므로 미확인이 아니다.
		 * **다만 위에서 IRQ_WAKE_THREAD 로 빠져나가면 이 줄에 닿지 않는다** */
		spurious = 0;
	}

	/* [한국어] PCI 명령 레지스터 변화 범주를 본다 */
	if (status_l0 & APPL_INTR_STATUS_L0_PCI_CMD_EN_INT) {
		/* [한국어] PCI 명령 변화의 세부 상태를 읽는다 */
		status_l1 = appl_readl(pcie, APPL_INTR_STATUS_L1_15);
		/* [한국어] 읽은 값을 되써서 지운다 */
		appl_writel(pcie, status_l1, APPL_INTR_STATUS_L1_15);

		/* [한국어] **버스 마스터 비트가 바뀌었으면 스레드를 깨운다.** 호스트가 장치를
		 * 쓰기 시작했다는 뜻이라 LTR 을 보내야 하기 때문이다 */
		if (status_l1 & APPL_INTR_STATUS_L1_15_CFG_BME_CHGED)
			return IRQ_WAKE_THREAD;

		/* [한국어] 아는 범주를 만났으므로 미확인이 아니다 */
		spurious = 0;
	}

	/* [한국어] INT 범주에 사건이 있는지 본다 */
	if (status_l0 & APPL_INTR_STATUS_L0_INT_INT) {
		/* [한국어] INT 범주의 세부 상태를 읽는다. **상태를 지우지 않는 데 주의** --
		 * 위의 두 범주가 곧바로 되써서 지우는 것과 다르다 */
		status_l1 = appl_readl(pcie, APPL_INTR_STATUS_L1_8_0);

		/*
		 * Interrupt is handled by DMA driver; don't treat it as
		 * spurious
		 */
		if (status_l1 & APPL_INTR_STATUS_L1_8_0_EDMA_INT_MASK)
			/* [한국어] **"미확인이 아니다" 라고만 표시하고 처리는 하지 않는다.**
			 * 위의 원문 주석대로 DMA 드라이버가 따로 처리한다 */
			spurious = 0;
	}

	/* [한국어] 아는 범주를 하나도 만나지 못했으면 참이다 */
	if (spurious) {
		/* [한국어] 알 수 없는 인터럽트임을 알린다 */
		dev_warn(pcie->dev, "Random interrupt (STATUS = 0x%08X)\n",
			 status_l0);
		/* [한국어] **원인을 모르는 상태를 통째로 지운다.** 그러지 않으면 같은
		 * 인터럽트가 계속 올라온다 */
		appl_writel(pcie, status_l0, APPL_INTR_STATUS_L0);
	}

	return IRQ_HANDLED;
}

/* [한국어]
 * tegra_pcie_dw_rd_own_conf - 루트 포트 자신의 config 읽기. 위험한 레지스터 하나를 건너뛴다
 *
 * @bus:   대상 버스. sysdata 에 struct dw_pcie_rp 가 들어 있다.
 * @devfn: 장치/함수 번호.
 * @where: config 오프셋.
 * @size:  읽을 바이트 수.
 * @val:   읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 커널 공통 함수의 반환값.
 *
 * **이 함수가 주소를 계산하지 않는다는 점이 앞 세대와의 가장 큰 대비다.**
 * 앞 세대 pci-tegra.c 는 자기만의 주소 인코딩을 풀고 4KiB 창을 옮기는
 * 코드를 갖고 있었지만, 여기서는 map_bus 가
 * dw_pcie_own_conf_map_bus(pcie-designware-host.c:1951)로 되어 있어
 * DWC 코어가 알아서 한다.
 *
 * 그래서 이 함수에 남은 일은 우회 하나뿐이다. 위의 원문 주석이 배경을
 * 밝히는데, MSI-X 도어벨 레지스터는 엔드포인트 모드용인데도 루트 포트
 * 모드에서 보이고, 링크가 ASPM-L1 상태일 때 접근하면 **시스템이 멈춘다.**
 * 그래서 그 오프셋에 대한 읽기는 하드웨어에 닿기 전에 0 을 돌려주고 끝낸다.
 *
 * 조건이 세 겹이다 -- 수정되지 않은 SoC 이고(Tegra194), 슬롯 0 이고
 * (루트 포트 자신), 그 오프셋일 때만이다. Tegra234 는
 * has_msix_doorbell_access_fix 가 true 라 이 우회를 타지 않는다.
 *
 * 실행 컨텍스트: PCI 코어의 config 락 안.
 *
 * 호출 체인:
 *   pci_read_config_ 계열 → PCI 코어 → tegra_pci_ops.read → [이 함수]
 *     → pci_generic_config_read
 */
static int tegra_pcie_dw_rd_own_conf(struct pci_bus *bus, u32 devfn, int where,
				     int size, u32 *val)
{
	struct dw_pcie_rp *pp = bus->sysdata;
	/* [한국어] sysdata 에 든 것이 struct dw_pcie_rp 라 두 단계로 변환한다 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다. of_data 의 우회 플래그를 보기 위해서다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);

	/*
	 * This is an endpoint mode specific register happen to appear even
	 * when controller is operating in root port mode and system hangs
	 * when it is accessed with link being in ASPM-L1 state.
	 * So skip accessing it altogether
	 */
	if (!pcie->of_data->has_msix_doorbell_access_fix &&
	    !PCI_SLOT(devfn) && where == PORT_LOGIC_MSIX_DOORBELL) {
		*val = 0x00000000;
		return PCIBIOS_SUCCESSFUL;
	}

	return pci_generic_config_read(bus, devfn, where, size, val);
}

/* [한국어]
 * tegra_pcie_dw_wr_own_conf - 루트 포트 자신의 config 쓰기. 같은 레지스터를 건너뛴다
 *
 * @bus:   대상 버스.
 * @devfn: 장치/함수 번호.
 * @where: config 오프셋.
 * @size:  쓸 바이트 수.
 * @val:   쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 커널 공통 함수의 반환값.
 *
 * tegra_pcie_dw_rd_own_conf 와 같은 우회를 쓰기 쪽에 적용한다. 조건도
 * 같고 이유도 같다 -- 링크가 ASPM-L1 일 때 그 레지스터를 건드리면
 * 시스템이 멈춘다.
 *
 * 읽기 쪽은 0 을 채워 주지만 이쪽은 **아무것도 하지 않고 성공을 보고한다.**
 * 쓰기는 돌려줄 값이 없으므로 그것으로 충분하다.
 *
 * 주소 계산이 없는 것은 읽기 쪽과 마찬가지다. size 를 바꾸지 않고 그대로
 * 넘기는 것에도 주의 -- pcie-mediatek-gen3.c 가 하드웨어 byte enable 때문에
 * size 를 4 로 바꾸던 것과 달리, 여기서는 DWC 코어가 표준 방식으로 처리한다.
 *
 * 실행 컨텍스트: config 락 안.
 *
 * 호출 체인:
 *   pci_write_config_ 계열 → PCI 코어 → tegra_pci_ops.write → [이 함수]
 *     → pci_generic_config_write
 */
static int tegra_pcie_dw_wr_own_conf(struct pci_bus *bus, u32 devfn, int where,
				     int size, u32 val)
{
	struct dw_pcie_rp *pp = bus->sysdata;
	/* [한국어] **두 단계 변환의 첫 단계** -- sysdata 의 dw_pcie_rp 에서 dw_pcie 로 간다 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);

	/*
	 * This is an endpoint mode specific register happen to appear even
	 * when controller is operating in root port mode and system hangs
	 * when it is accessed with link being in ASPM-L1 state.
	 * So skip accessing it altogether
	 */
	if (!pcie->of_data->has_msix_doorbell_access_fix &&
	    !PCI_SLOT(devfn) && where == PORT_LOGIC_MSIX_DOORBELL)
		return PCIBIOS_SUCCESSFUL;

	/* [한국어] **우회에 걸리지 않으면 커널 공통 구현에 그대로 넘긴다.**
	 * size 를 바꾸지 않는 데 주의 -- pcie-mediatek-gen3.c 가 하드웨어
	 * byte enable 때문에 4 로 바꾸던 것과 달리, 여기서는 DWC 코어가
	 * 표준 방식으로 처리한다 */
	return pci_generic_config_write(bus, devfn, where, size, val);
}

/* [한국어] 루트 포트 자신의 config 접근 방법. tegra_pcie_dw_host_init 이
 * 브리지에 꽂아 DWC 코어의 기본값을 덮어쓴다 */
static struct pci_ops tegra_pci_ops = {
	/* [한국어] **map_bus 가 DWC 공통 구현 한 줄이다**
	 * (pcie-designware-host.c:1951). 앞 세대 pci-tegra.c 가 자기 주소
	 * 인코딩을 풀고 4KiB 창을 옮기던 자리가 통째로 사라진 지점이다 */
	.map_bus = dw_pcie_own_conf_map_bus,
	.read = tegra_pcie_dw_rd_own_conf,
	.write = tegra_pcie_dw_wr_own_conf,
};

/* [한국어] **ASPM 관련 코드 전체를 조건부로 감싼다.** ASPM 을 쓰지 않는 커널에서는
 * 이벤트 카운터도 L1 서브상태 설정도 의미가 없으므로 통째로 뺀다.
 * 앞 세대 pci-tegra.c 에는 ASPM 을 다루는 코드가 아예 없어 이런 구분도
 * 없다 -- 링크 전원 관리를 세밀하게 다루는 것은 이 세대에서 생긴 일이다 */
#if defined(CONFIG_PCIEASPM)
/* [한국어]
 * event_counter_prog - RAS-DES 이벤트 카운터 하나를 골라 값을 읽는다
 *
 * @pcie:  드라이버 상태.
 * @event: 읽을 이벤트 번호(EVENT_COUNTER_EVENT_ 계열).
 * @return: 그 이벤트의 카운터 값.
 *
 * DWC IP 의 벤더 확장 능력(RAS-DES)에 들어 있는 이벤트 카운터를 읽는다.
 * 카운터가 여럿인데 레지스터 창은 하나라, **먼저 어느 이벤트를 볼지
 * 선택한 뒤 값을 읽는** 두 단계 구조다.
 *
 * 제어 레지스터에 세 가지를 함께 쓴다 -- 그룹 5 선택, 이벤트 번호,
 * 그리고 전체 활성화. 그룹 5 가 무엇인지는 이 트리에서 확인할 수 없으나,
 * 호출자들이 모두 ASPM 상태 전이 횟수를 보는 데 쓰는 것으로 보아
 * 전원 관리 관련 그룹이다.
 *
 * 기존 이벤트 선택 필드를 지운 뒤 새 값을 넣는데, 그룹 선택 필드는
 * 지우지 않고 OR 로 겹친다. 그룹 값이 매번 같아 문제가 되지 않는다.
 *
 * **앞 세대에 없던 진단 수단이다.** pci-tegra.c 의 debugfs 는 링크가
 * 올라왔는지만 보여 주는 반면, 이쪽은 ASPM 상태에 몇 번 들어갔는지까지
 * 센다.
 *
 * CONFIG_PCIEASPM 이 켜져 있을 때만 컴파일된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(debugfs 읽기). 링크와 전원이 살아
 * 있어야 DBI 접근이 유효하다.
 *
 * 호출 체인:
 *   aspm_state_cnt → [이 함수] → dw_pcie_readl_dbi, dw_pcie_writel_dbi
 */
static inline u32 event_counter_prog(struct tegra_pcie_dw *pcie, u32 event)
{
	u32 val;

	/* [한국어] 현재 제어 값을 읽는다 */
	val = dw_pcie_readl_dbi(&pcie->pci, pcie->ras_des_cap +
				PCIE_RAS_DES_EVENT_COUNTER_CONTROL);
	/* [한국어] 기존 이벤트 선택을 지운다. **그룹 선택 필드는 지우지 않고 OR 로
	 * 겹친다** -- 그룹 값이 매번 같아 문제가 되지 않는다 */
	val &= ~(EVENT_COUNTER_EVENT_SEL_MASK << EVENT_COUNTER_EVENT_SEL_SHIFT);
	/* [한국어] **그룹 5 를 고른다.** 그 그룹이 무엇인지는 이 트리에서 확인할 수
	 * 없으나, 호출자들이 모두 ASPM 상태 전이를 보는 데 쓰는 것으로 보아
	 * 전원 관리 관련 그룹이다 */
	val |= EVENT_COUNTER_GROUP_5 << EVENT_COUNTER_GROUP_SEL_SHIFT;
	/* [한국어] 볼 이벤트 번호를 넣는다 */
	val |= event << EVENT_COUNTER_EVENT_SEL_SHIFT;
	/* [한국어] 모든 카운터를 켠다 */
	val |= EVENT_COUNTER_ENABLE_ALL << EVENT_COUNTER_ENABLE_SHIFT;
	/* [한국어] 선택을 확정한다 */
	dw_pcie_writel_dbi(&pcie->pci, pcie->ras_des_cap +
			   PCIE_RAS_DES_EVENT_COUNTER_CONTROL, val);
	/* [한국어] **선택한 이벤트의 값을 읽는다.** 카운터가 여럿인데 레지스터 창은
	 * 하나라, 먼저 고른 뒤 읽는 두 단계 구조다 */
	val = dw_pcie_readl_dbi(&pcie->pci, pcie->ras_des_cap +
				PCIE_RAS_DES_EVENT_COUNTER_DATA);

	return val;
}

/* [한국어]
 * aspm_state_cnt - ASPM 상태 진입 횟수를 debugfs 로 보여 준다
 *
 * @s:    seq_file 문맥. private 에 device 가 들어 있다.
 * @data: 쓰지 않는다.
 * @return: 항상 0.
 *
 * 전원 관리 상태 다섯 가지의 진입 횟수를 찍는다 -- 송신 L0s, 수신 L0s,
 * L1, L1.1, L1.2 다. 링크가 실제로 절전 상태에 들어가고 있는지 확인하는
 * 진단 창이며, ASPM 설정이 의도대로 동작하는지 보는 데 쓴다.
 *
 * **출력한 뒤 카운터를 모두 지우고 다시 켠다.** 그래서 이 파일을 두 번
 * 읽으면 두 번째 값은 그 사이의 증가분이다. 누적값이 아니라는 점에
 * 주의해야 한다.
 *
 * 다시 켤 때 그룹 5 선택을 함께 쓰는데, 지우기 명령이 그룹 선택도
 * 날렸을 것을 감안한 복구로 보인다.
 *
 * private 에서 device 를 꺼내 다시 drvdata 로 드라이버 상태를 얻는
 * 두 단계를 거친다. debugfs_create_devm_seqfile 이 device 를 private 로
 * 넘기기 때문이다.
 *
 * CONFIG_PCIEASPM 이 켜져 있을 때만 컴파일된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자가 debugfs 파일을 읽을 때).
 *
 * 호출 체인:
 *   read(2) → seq_file 코어 → [이 함수] → event_counter_prog
 */
static int aspm_state_cnt(struct seq_file *s, void *data)
{
	struct tegra_pcie_dw *pcie = (struct tegra_pcie_dw *)
				     dev_get_drvdata(s->private);
	/* [한국어] 카운터를 다시 켤 때 쓸 값 */
	u32 val;

	/* [한국어] 송신 방향 L0s 진입 횟수. **다섯 상태를 차례로 찍는다** --
	 * 링크가 실제로 절전 상태에 들어가고 있는지 확인하는 진단 창이며,
	 * 앞 세대 pci-tegra.c 의 debugfs 가 링크 유무만 보여 주던 것과 대비된다 */
	seq_printf(s, "Tx L0s entry count : %u\n",
		   event_counter_prog(pcie, EVENT_COUNTER_EVENT_Tx_L0S));

	/* [한국어] 수신 방향 L0s 진입 횟수 */
	seq_printf(s, "Rx L0s entry count : %u\n",
		   event_counter_prog(pcie, EVENT_COUNTER_EVENT_Rx_L0S));

	/* [한국어] L1 진입 횟수 */
	seq_printf(s, "Link L1 entry count : %u\n",
		   event_counter_prog(pcie, EVENT_COUNTER_EVENT_L1));

	/* [한국어] L1.1 진입 횟수 */
	seq_printf(s, "Link L1.1 entry count : %u\n",
		   event_counter_prog(pcie, EVENT_COUNTER_EVENT_L1_1));

	/* [한국어] L1.2 진입 횟수 */
	seq_printf(s, "Link L1.2 entry count : %u\n",
		   event_counter_prog(pcie, EVENT_COUNTER_EVENT_L1_2));

	/* Clear all counters */
	dw_pcie_writel_dbi(&pcie->pci, pcie->ras_des_cap +
			   PCIE_RAS_DES_EVENT_COUNTER_CONTROL,
			   EVENT_COUNTER_ALL_CLEAR);

	/* Re-enable counting */
	val = EVENT_COUNTER_ENABLE_ALL << EVENT_COUNTER_ENABLE_SHIFT;
	/* [한국어] **그룹 선택을 함께 쓴다.** 위의 지우기 명령이 그룹 선택도 날렸을
	 * 것을 감안한 복구로 보인다 */
	val |= EVENT_COUNTER_GROUP_5 << EVENT_COUNTER_GROUP_SEL_SHIFT;
	/* [한국어] 카운터 제어 레지스터에 되쓴다 */
	dw_pcie_writel_dbi(&pcie->pci, pcie->ras_des_cap +
			   PCIE_RAS_DES_EVENT_COUNTER_CONTROL, val);

	return 0;
}

/* [한국어]
 * init_host_aspm - ASPM 관련 능력과 지연 값을 설정한다
 *
 * @pcie: 드라이버 상태. ras_des_cap 을 여기서 찾아 저장한다.
 * @return: 없음.
 *
 * ASPM(자동 전력 절감 링크 상태)이 제대로 동작하도록 IP 의 관련
 * 레지스터를 손본다. RC 와 EP 양쪽 초기화 경로가 모두 이 함수를 부른다.
 *
 *   1) L1 서브상태 능력과 RAS-DES 벤더 능력의 위치를 찾는다.
 *      **ras_des_cap 을 여기서 저장하는 것이 중요하다** -- 이벤트 카운터를
 *      읽는 함수들이 그 값에 의존한다.
 *   2) 이벤트 카운터를 켠다. 그래야 aspm_state_cnt 가 의미 있는 값을 본다.
 *   3) 장치 트리에서 읽은 T_cmrt 와 T_pwr_on 을 L1 서브상태 능력
 *      레지스터에 써 넣는다. 시프트 8 과 19 는 그 레지스터의 필드 위치다.
 *      **보드가 알려 주는 값을 하드웨어 능력으로 광고하는 것** 이라,
 *      상대 장치가 그 시간을 지켜 준다.
 *   4) CLKREQ 를 지원하면 L1 서브상태를 쓸 수 있다고 표시한다. L1.1 과
 *      L1.2 는 CLKREQ 신호가 있어야 성립하기 때문이다.
 *   5) **Tegra234 EP 모드에서 L1.2 광고를 끈다.** 위의 원문 주석이
 *      상세히 밝히는데, L1.2 를 빠져나올 때 REFCLK 가 안정되기 전에
 *      UPHY PLL 이 켜져 주파수를 못 잡고 링크가 끊기는 하드웨어 결함이
 *      있다. 고칠 방법이 없어 아예 능력 광고에서 빼, 호스트가 그 상태로
 *      들어가려 하지 않게 만든다.
 *   6) L0s 진입 지연을 쓰고 ASPM 진입을 허용한다.
 *
 * 앞 세대 pci-tegra.c 에는 ASPM 관련 코드가 없다. 링크 전원 관리를
 * 세밀하게 다루는 것은 이 세대에서 생긴 일이다.
 *
 * **이 함수는 CONFIG_PCIEASPM 이 꺼져 있으면 빈 함수로 대체된다** --
 * 아래 #else 절의 정의를 보라.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화 경로).
 *
 * 호출 체인:
 *   tegra_pcie_dw_host_init / pex_ep_event_pex_rst_deassert → [이 함수]
 *     → dw_pcie_find_ext_capability, dw_pcie_read/writel_dbi
 */
static void init_host_aspm(struct tegra_pcie_dw *pcie)
{
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] L1 서브상태 능력 오프셋과 레지스터 값 */
	u32 l1ss, val;

	/* [한국어] L1 서브상태 능력의 위치를 찾는다 */
	l1ss = dw_pcie_find_ext_capability(pci, PCI_EXT_CAP_ID_L1SS);

	/* [한국어] **RAS-DES 능력 위치를 찾아 저장한다.** 이벤트 카운터를 읽는 함수들이
	 * 이 값에 의존한다 */
	pcie->ras_des_cap = dw_pcie_find_ext_capability(&pcie->pci,
							PCI_EXT_CAP_ID_VNDR);

	/* Enable ASPM counters */
	val = EVENT_COUNTER_ENABLE_ALL << EVENT_COUNTER_ENABLE_SHIFT;
	/* [한국어] 그룹 5 를 고른다 */
	val |= EVENT_COUNTER_GROUP_5 << EVENT_COUNTER_GROUP_SEL_SHIFT;
	/* [한국어] 카운터 제어 레지스터에 쓴다 */
	dw_pcie_writel_dbi(pci, pcie->ras_des_cap +
			   PCIE_RAS_DES_EVENT_COUNTER_CONTROL, val);

	/* Program T_cmrt and T_pwr_on values */
	val = dw_pcie_readl_dbi(pci, l1ss + PCI_L1SS_CAP);
	/* [한국어] 두 시간 필드를 지운다 */
	val &= ~(PCI_L1SS_CAP_CM_RESTORE_TIME | PCI_L1SS_CAP_P_PWR_ON_VALUE);
	/* [한국어] **T_cmrt 를 비트 8 자리에 넣는다.** 보드가 알려 주는 값을 하드웨어
	 * 능력으로 광고해, 상대 장치가 그 시간을 지키게 한다 */
	val |= (pcie->aspm_cmrt << 8);
	/* [한국어] T_pwr_on 을 비트 19 자리에 넣는다 */
	val |= (pcie->aspm_pwr_on_t << 19);
	/* [한국어] 능력 레지스터에 쓴다 */
	dw_pcie_writel_dbi(pci, l1ss + PCI_L1SS_CAP, val);

	/* [한국어] 보드가 CLKREQ 를 지원하는지 확인한다 */
	if (pcie->supports_clkreq)
		/* [한국어] **CLKREQ 가 있어야 L1 서브상태를 쓸 수 있다** -- L1.1 과 L1.2 가
		 * 그 신호에 의존하기 때문이다 */
		pci->l1ss_support = true;

	/*
	 * Disable L1.2 capability advertisement for Tegra234 Endpoint mode.
	 * Tegra234 has a hardware bug where during L1.2 exit, the UPHY PLL is
	 * powered up immediately without waiting for REFCLK to stabilize. This
	 * causes the PLL to fail to lock to the correct frequency, resulting in
	 * PCIe link loss. Since there is no hardware fix available, we prevent
	 * the Endpoint from advertising L1.2 support by clearing the L1.2 bits
	 * in the L1 PM Substates Capabilities register. This ensures the host
	 * will not attempt to enter L1.2 state with this Endpoint.
	 */
	if (pcie->of_data->disable_l1_2 &&
	    pcie->of_data->mode == DW_PCIE_EP_TYPE) {
		/* [한국어] 현재 능력 값을 읽는다 */
		val = dw_pcie_readl_dbi(pci, l1ss + PCI_L1SS_CAP);
		/* [한국어] **L1.2 관련 두 비트를 지워 능력 광고에서 뺀다.** 위의 원문 주석대로
		 * 고칠 수 없는 하드웨어 결함이 있어, 호스트가 그 상태로 들어가려
		 * 하지 않게 만드는 것이다 */
		val &= ~(PCI_L1SS_CAP_PCIPM_L1_2 | PCI_L1SS_CAP_ASPM_L1_2);
		/* [한국어] 되쓴다 */
		dw_pcie_writel_dbi(pci, l1ss + PCI_L1SS_CAP, val);
	}

	/* Program L0s and L1 entrance latencies */
	val = dw_pcie_readl_dbi(pci, PCIE_PORT_AFR);
	/* [한국어] 기존 지연 필드를 지운다 */
	val &= ~PORT_AFR_L0S_ENTRANCE_LAT_MASK;
	/* [한국어] L0s 진입 지연을 넣는다 */
	val |= (pcie->aspm_l0s_enter_lat << PORT_AFR_L0S_ENTRANCE_LAT_SHIFT);
	/* [한국어] ASPM 진입을 허용한다 */
	val |= PORT_AFR_ENTER_ASPM;
	dw_pcie_writel_dbi(pci, PCIE_PORT_AFR, val);
}

/* [한국어]
 * init_debugfs - ASPM 통계 debugfs 파일을 만든다
 *
 * @pcie: 드라이버 상태. debugfs 필드를 채운다.
 * @return: 없음. 이름 할당이 실패하면 조용히 돌아간다.
 *
 * debugfs 에 디렉터리를 만들고 그 안에 aspm_state_cnt 파일을 둔다.
 *
 * **디렉터리 이름을 장치 트리 경로로 짓는 것이 앞 세대와 다르다.**
 * %pOFP 형식이 노드의 전체 경로를 문자열로 만들어 주는데, 이렇게 하는
 * 이유는 이 SoC 에 PCIe 컨트롤러가 **여럿** 이기 때문이다. 앞 세대
 * pci-tegra.c 는 인스턴스가 하나뿐이라 "pcie" 라는 고정 이름을 썼다.
 *
 * debugfs_create_devm_seqfile 을 쓰므로 device 수명에 묶인다. 다만
 * tegra_pcie_dw_remove 와 _shutdown 이 debugfs_remove_recursive 로
 * 명시적으로도 지운다 -- 앞 세대가 정리 코드를 아예 두지 않은 것과 대비된다.
 *
 * RC 경로에서만 불린다(tegra_pcie_config_rp). EP 모드에서는 ASPM 통계를
 * 보여 주지 않는다.
 *
 * **CONFIG_PCIEASPM 이 꺼져 있으면 빈 함수로 대체된다.**
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_config_rp → [이 함수]
 *     → devm_kasprintf, debugfs_create_dir, debugfs_create_devm_seqfile
 */
static void init_debugfs(struct tegra_pcie_dw *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 디렉터리 이름을 담을 문자열 */
	char *name;

	/* [한국어] %pOFP 형식이 노드의 전체 경로를 문자열로 만들어 준다 */
	name = devm_kasprintf(dev, GFP_KERNEL, "%pOFP", dev->of_node);
	/* [한국어] 할당 실패면 조용히 돌아간다 -- debugfs 가 없다고 드라이버가 실패할
	 * 이유는 없다 */
	if (!name)
		return;

	/* [한국어] **디렉터리 이름을 장치 트리 경로로 짓는다.** 이 SoC 에 PCIe 컨트롤러가
	 * 여럿이기 때문이며, 앞 세대 pci-tegra.c 가 "pcie" 라는 고정 이름을
	 * 쓴 것과 대비된다 */
	pcie->debugfs = debugfs_create_dir(name, NULL);

	debugfs_create_devm_seqfile(dev, "aspm_state_cnt", pcie->debugfs,
				    aspm_state_cnt);
}
/* [한국어] **CONFIG_PCIEASPM 이 꺼진 커널을 위한 대체 정의가 여기서 시작한다.**
 * 위의 진짜 구현 넷(event_counter_prog, aspm_state_cnt, init_host_aspm,
 * init_debugfs) 중 바깥에서 불리는 둘만 빈 함수로 대체하면 된다 --
 * 나머지 둘은 이 블록 안에서만 쓰이므로 함께 사라진다 */
#else
/* [한국어] [한국어]
 * init_host_aspm - ASPM 설정의 빈 구현
 * 
 * @pcie: 드라이버 상태. 쓰지 않는다.
 * @return: 없음.
 * 
 * CONFIG_PCIEASPM 이 꺼져 있을 때 쓰이는 대체 정의다. ASPM 을 쓰지
 * 않으므로 L1 서브상태 능력이나 진입 지연을 설정할 이유가 없다.
 * 
 * **한 가지 부작용이 있다** -- 위의 진짜 구현은 pcie->ras_des_cap 을
 * 채우는데, 이 빈 구현은 그러지 않는다. 다만 그 필드를 읽는 함수들
 * (event_counter_prog, aspm_state_cnt)도 같은 조건부 컴파일 안에 있어
 * 함께 사라지므로 문제가 되지 않는다.
 * 
 * inline 으로 선언해 컴파일러가 호출 자체를 지우게 한다.
 * 
 * 실행 컨텍스트: 초기화 경로. 실제로는 아무 일도 하지 않는다.
 * 
 * 호출 체인:
 *   tegra_pcie_dw_host_init / pex_ep_event_pex_rst_deassert → [이 함수] */
static inline void init_host_aspm(struct tegra_pcie_dw *pcie) { return; }
/* [한국어] [한국어]
 * init_debugfs - ASPM 통계 debugfs 파일 생성의 빈 구현
 * 
 * @pcie: 드라이버 상태. 쓰지 않는다.
 * @return: 없음.
 * 
 * CONFIG_PCIEASPM 이 꺼져 있을 때 쓰이는 대체 정의다. 보여 줄 ASPM
 * 통계 자체가 없으므로 아무것도 하지 않는다.
 * 
 * 호출자(tegra_pcie_config_rp)에 #ifdef 를 넣지 않기 위한 관용구다 --
 * 조건부 컴파일을 정의 쪽에 몰아 두면 호출 지점이 깨끗해진다.
 * 
 * inline 으로 선언해 컴파일러가 호출 자체를 지우게 한다.
 * 
 * 실행 컨텍스트: probe 경로. 실제로는 아무 일도 하지 않는다.
 * 
 * 호출 체인:
 *   tegra_pcie_config_rp → [이 함수] */
static inline void init_debugfs(struct tegra_pcie_dw *pcie) { return; }
/* [한국어] 조건부 컴파일 끝. 위의 두 정의 묶음 중 하나만 커널에 들어간다 */
#endif

/* [한국어]
 * tegra_pcie_enable_system_interrupts - 링크 상태와 CDM 검사 인터럽트를 켠다
 *
 * @pp: DWC 루트 포트 구조체.
 * @return: 없음.
 *
 * 세 가지를 한다.
 *   1) 링크 상태 인터럽트를 켠다. 링크가 오르내릴 때 알림을 받는다.
 *   2) has_sbr_reset_fix 가 없는 SoC(Tegra194)에서만 링크 리셋 요청
 *      인터럽트를 켠다. 그 우회가 tegra_pcie_rp_irq_handler 에 있고,
 *      수정된 SoC 는 하드웨어가 알아서 처리하므로 알림이 필요 없다.
 *   3) enable_cdm_check 가 켜져 있으면 CDM 관련 인터럽트를 켠다.
 *      **활성화 비트가 SoC 마다 다르다** -- of_data 의 cdm_chk_int_en_bit 이
 *      Tegra194 는 BIT(19), Tegra234 는 BIT(18) 이다. 그래서 상수를 쓰지
 *      않고 기술자에서 가져온다.
 *
 * 마지막 두 줄이 인터럽트와 무관한 일을 한다.
 *   링크가 처음 섰을 때의 폭을 기억해 둔다. apply_bad_link_workaround 가
 *     나중에 이 값과 현재 폭을 비교해 링크가 나빠졌는지 판단한다.
 *   대역폭 관리 인터럽트를 활성화한다(LNKCTL 의 LBMIE). 이것이 켜져야
 *     폭이나 속도가 바뀔 때 하드웨어가 알려 준다.
 *
 * **이 함수는 링크가 선 뒤에 불려야 한다.** 초기 폭을 읽어야 하기 때문이며,
 * 실제로 tegra_pcie_dw_start_link 가 링크 확인 후에 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(링크 기동 경로).
 *
 * 호출 체인:
 *   tegra_pcie_enable_interrupts → [이 함수] → appl_readl/writel, dw_pcie_read/writew_dbi
 */
static void tegra_pcie_enable_system_interrupts(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다. **두 단계 변환** 이다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;
	/* [한국어] 16비트 레지스터 값 임시 변수 */
	u16 val_w;

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_INTR_EN_L0_0);
	/* [한국어] 링크 상태 변화 범주를 켠다 */
	val |= APPL_INTR_EN_L0_0_LINK_STATE_INT_EN;
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_INTR_EN_L0_0);

	/* [한국어] **결함이 고쳐지지 않은 SoC(Tegra194)에서만 켠다** --
	 * 수정된 SoC 는 하드웨어가 알아서 처리하므로 알림이 필요 없다 */
	if (!pcie->of_data->has_sbr_reset_fix) {
		/* [한국어] 현재 값을 읽는다 */
		val = appl_readl(pcie, APPL_INTR_EN_L1_0_0);
		/* [한국어] 링크 리셋 요청 알림을 켠다 */
		val |= APPL_INTR_EN_L1_0_0_LINK_REQ_RST_NOT_INT_EN;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_INTR_EN_L1_0_0);
	}

	/* [한국어] 장치 트리가 CDM 검사를 요청했을 때만 켠다 */
	if (pcie->enable_cdm_check) {
		/* [한국어] 현재 값을 읽는다 */
		val = appl_readl(pcie, APPL_INTR_EN_L0_0);
		/* [한국어] **비트 위치가 SoC 마다 달라 기술자에서 가져온다** --
		 * Tegra194 는 BIT(19), Tegra234 는 BIT(18) 이다. 그래서
		 * APPL_INTR_EN_L0_0_CDM_REG_CHK_INT_EN 상수는 정의만 있고 쓰이지 않는다 */
		val |= pcie->of_data->cdm_chk_int_en_bit;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_INTR_EN_L0_0);

		/* [한국어] 현재 값을 읽는다 */
		val = appl_readl(pcie, APPL_INTR_EN_L1_18);
		/* [한국어] 비교 불일치 알림을 켠다. **완료(CMPLT) 알림은 켜지 않는다** --
		 * 오류만 알면 되기 때문이다 */
		val |= APPL_INTR_EN_L1_18_CDM_REG_CHK_CMP_ERR;
		/* [한국어] 논리 오류 알림을 켠다 */
		val |= APPL_INTR_EN_L1_18_CDM_REG_CHK_LOGIC_ERR;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_INTR_EN_L1_18);
	}

	/* [한국어] 링크 상태를 읽는다 */
	val_w = dw_pcie_readw_dbi(&pcie->pci, pcie->pcie_cap_base +
				  PCI_EXP_LNKSTA);
	/* [한국어] **링크가 처음 섰을 때의 폭을 기억해 둔다.**
	 * apply_bad_link_workaround 가 나중에 이 값과 현재 폭을 비교해 링크가
	 * 나빠졌는지 판단한다 -- 그래서 이 함수는 링크가 선 뒤에 불려야 한다 */
	pcie->init_link_width = FIELD_GET(PCI_EXP_LNKSTA_NLW, val_w);

	/* [한국어] 링크 제어 레지스터를 읽는다 */
	val_w = dw_pcie_readw_dbi(&pcie->pci, pcie->pcie_cap_base +
				  PCI_EXP_LNKCTL);
	/* [한국어] **대역폭 관리 인터럽트를 활성화한다.** 이것이 켜져야 폭이나 속도가
	 * 바뀔 때 하드웨어가 알려 준다 */
	val_w |= PCI_EXP_LNKCTL_LBMIE;
	/* [한국어] 설정을 쓴다 */
	dw_pcie_writew_dbi(&pcie->pci, pcie->pcie_cap_base + PCI_EXP_LNKCTL,
			   val_w);
}

/* [한국어]
 * tegra_pcie_enable_intx_interrupts - INTx 와 그에 딸린 여러 인터럽트를 켠다
 *
 * @pp: DWC 루트 포트 구조체.
 * @return: 없음.
 *
 * 이름은 INTx 지만 **실제로는 INT 범주 전체를 켠다.** APPL 의 인터럽트
 * 분류에서 INTx, 대역폭 관리, 자동 대역폭 변경, eDMA, AER 이 모두
 * 같은 L1_8 레지스터에 모여 있기 때문이다.
 *
 * 두 층으로 켠다.
 *   L0 쪽 : 시스템 인터럽트 전달과 INT 범주를 켠다.
 *   L1_8 쪽 : 그 범주 안의 개별 사건들을 켠다.
 * 두 층이 모두 켜져야 인터럽트가 실제로 전달된다 -- 앞 세대
 * pci-tegra.c 의 AFI_INTR_MASK 와 AFI_AFI_INTR_ENABLE 이 이루던 관계와
 * 같은 구조다.
 *
 * AER 만 조건부인데, 커널이 AER 을 지원하도록 빌드되었을 때만 켠다.
 * 받아도 처리할 코드가 없으면 의미가 없기 때문이다.
 *
 * **INTx 도메인을 만들지 않는다는 점이 앞 세대와 다르다.** DWC 코어가
 * INTx 도메인을 관리하므로 이 드라이버는 APPL 쪽에서 전달을 켜 주기만
 * 한다. pci-tegra.c 가 map_irq 콜백으로 직접 해결하던 것과도 다르다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(링크 기동 경로).
 *
 * 호출 체인:
 *   tegra_pcie_enable_interrupts → [이 함수] → appl_readl, appl_writel
 */
static void tegra_pcie_enable_intx_interrupts(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* Enable INTX interrupt generation */
	val = appl_readl(pcie, APPL_INTR_EN_L0_0);
	/* [한국어] 시스템 인터럽트 전달 */
	val |= APPL_INTR_EN_L0_0_SYS_INTR_EN;
	/* [한국어] INT 범주. **이름은 INTx 지만 실제로는 대역폭과 eDMA와 AER 도 이
	 * 범주에 함께 들어 있다** */
	val |= APPL_INTR_EN_L0_0_INT_INT_EN;
	/* [한국어] L0 층 설정을 쓴다 */
	appl_writel(pcie, val, APPL_INTR_EN_L0_0);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_INTR_EN_L1_8_0);
	/* [한국어] INTx 인터럽트 */
	val |= APPL_INTR_EN_L1_8_INTX_EN;
	/* [한국어] 자동 대역폭 변경. RC 핸들러가 이 사건에 우회를 적용한다 */
	val |= APPL_INTR_EN_L1_8_AUTO_BW_INT_EN;
	/* [한국어] 대역폭 관리 이벤트 */
	val |= APPL_INTR_EN_L1_8_BW_MGT_INT_EN;
	/* [한국어] eDMA 인터럽트. **이 파일이 처리하지 않고 DMA 드라이버가 한다** */
	val |= APPL_INTR_EN_L1_8_EDMA_INT_EN;
	/* [한국어] AER 지원 여부를 확인한다 */
	if (IS_ENABLED(CONFIG_PCIEAER))
		/* [한국어] **커널이 AER 을 지원할 때만 켠다** -- 받아도 처리할 코드가 없으면
		 * 의미가 없기 때문이다 */
		val |= APPL_INTR_EN_L1_8_AER_INT_EN;
	appl_writel(pcie, val, APPL_INTR_EN_L1_8_0);
}

/* [한국어]
 * tegra_pcie_enable_msi_interrupts - MSI 인터럽트 전달을 켠다
 *
 * @pp: DWC 루트 포트 구조체.
 * @return: 없음.
 *
 * **이 파일의 MSI 관련 코드가 사실상 이것뿐이다.** 세 줄로 APPL 의
 * MSI 전달 비트 둘을 켜는 것이 전부다.
 *
 * 앞 세대 pci-tegra.c 와 견주면 차이가 뚜렷하다. 그쪽은 비트맵, irq_chip,
 * 도메인 생성, 목적지 페이지 DMA 할당, 체인 핸들러까지 여덟 함수 남짓을
 * 직접 갖고 있었다. 여기서는 그 전부를 DWC 코어의 iMSI-RX 가 대신하고,
 * 드라이버는 probe 에서 pp->num_vectors 를 지정한 뒤 이 함수로 APPL
 * 쪽 전달만 열어 준다.
 *
 * 그것이 가능한 이유는 이 드라이버가 dw_pcie_host_ops 의 msi_init 을
 * 채우지 **않기** 때문이다. 채우면 DWC 코어가 자기 iMSI-RX 경로를
 * 건너뛰는데(pci-keystone.c 가 그렇게 한다), 여기서는 채우지 않아
 * 코어의 기본 동작을 그대로 쓴다.
 *
 * CONFIG_PCI_MSI 가 켜져 있을 때만 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(링크 기동 경로).
 *
 * 호출 체인:
 *   tegra_pcie_enable_interrupts → [이 함수] → appl_readl, appl_writel
 */
static void tegra_pcie_enable_msi_interrupts(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* Enable MSI interrupt generation */
	val = appl_readl(pcie, APPL_INTR_EN_L0_0);
	/* [한국어] MSI 인터럽트 전달을 켠다 */
	val |= APPL_INTR_EN_L0_0_SYS_MSI_INTR_EN;
	/* [한국어] MSI 수신 범주를 켠다 */
	val |= APPL_INTR_EN_L0_0_MSI_RCV_INT_EN;
	appl_writel(pcie, val, APPL_INTR_EN_L0_0);
}

/* [한국어]
 * tegra_pcie_enable_interrupts - 상태를 모두 지우고 세 범주의 인터럽트를 켠다
 *
 * @pp: DWC 루트 포트 구조체.
 * @return: 없음.
 *
 * **켜기 전에 지우는 것** 이 이 함수의 순서다. 링크가 서는 과정에서 쌓인
 * 상태가 남아 있으면 켜는 순간 처리할 수 없는 인터럽트가 밀려들기
 * 때문이다.
 *
 * L0 과 L1_x 상태 레지스터 열다섯 개에 0xFFFFFFFF 를 쓴다. 1을 쓰면
 * 지워지는 방식이라 전체 비트를 세워 쓴다. 같은 목록이
 * pex_ep_event_hot_rst_done 과 pex_ep_event_pex_rst_deassert 에도 나오며,
 * 그 둘은 APPL_MSI_CTRL_2 를 하나 더 지운다.
 *
 * 그다음 세 범주를 순서대로 켠다 -- 시스템(링크 상태와 CDM), INT(INTx 와
 * 대역폭과 eDMA와 AER), MSI. MSI 만 커널 설정에 따라 조건부다.
 *
 * 앞 세대 pci-tegra.c 의 tegra_pcie_enable_controller 가 같은 자리를
 * 맡았지만, 그쪽은 인터럽트 설정과 레인 배분과 클래스 코드가 한 함수에
 * 섞여 있었다. 여기서는 인터럽트만 따로 떼어 세 함수로 나뉘어 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(링크가 선 직후).
 *
 * 호출 체인:
 *   tegra_pcie_dw_start_link → [이 함수]
 *     → tegra_pcie_enable_system_interrupts, _enable_intx_interrupts,
 *       _enable_msi_interrupts
 */
static void tegra_pcie_enable_interrupts(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);

	/* Clear interrupt statuses before enabling interrupts */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L0);
	/* [한국어] 링크 상태 범주를 지운다. **켜기 전에 지우는 것이 이 함수의 순서다** --
	 * 링크가 서는 과정에서 쌓인 상태가 남아 있으면 켜는 순간 처리할 수
	 * 없는 인터럽트가 밀려든다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_0_0);
	/* [한국어] L1_1 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_1);
	/* [한국어] L1_2 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_2);
	/* [한국어] L1_3 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_3);
	/* [한국어] L1_6 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_6);
	/* [한국어] L1_7 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_7);
	/* [한국어] INT 범주 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_8_0);
	/* [한국어] L1_9 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_9);
	/* [한국어] L1_10 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_10);
	/* [한국어] L1_11 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_11);
	/* [한국어] L1_13 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_13);
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_14);
	/* [한국어] L1_15 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_15);
	/* [한국어] 마지막 상태 레지스터 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_17);

	tegra_pcie_enable_system_interrupts(pp);
	tegra_pcie_enable_intx_interrupts(pp);
	/* [한국어] MSI 를 쓰는 커널에서만 켠다 */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		tegra_pcie_enable_msi_interrupts(pp);
}

/* [한국어]
 * config_gen3_gen4_eq_presets - Gen3/Gen4 링크 이퀄라이제이션 프리셋을 설정한다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음.
 *
 * Gen3 이상에서는 링크를 세울 때 송수신단 이퀄라이저를 협상하는데,
 * 그 출발점이 될 프리셋을 미리 정해 둔다. **Gen3 부터 생긴 절차** 라
 * Gen2 가 상한이던 앞 세대에는 이런 함수가 없다.
 *
 * 두 부분으로 나뉜다.
 *
 * 레인별 프리셋 (앞의 루프):
 *   레인마다 두 곳에 같은 값(GEN3_GEN4_EQ_PRESET_INIT, 5)을 쓴다.
 *   하나는 Secondary PCI Express 확장 능력의 레인 제어 필드이고,
 *   다른 하나는 16GT/s 능력의 레인 이퀄라이제이션 제어 필드다.
 *   각각 하류(DSP)와 상류(USP) 방향을 따로 설정한다.
 *   **레인마다 오프셋이 다르다** -- 앞은 2바이트씩(16비트 항목), 뒤는
 *   1바이트씩(8비트 항목) 떨어져 있어 곱하는 값이 다르다.
 *
 * 속도별 프리셋 벡터 (뒤의 세 묶음):
 *   GEN3_RELATED_OFF 의 "rate shadow select" 필드로 **어느 속도의 설정을
 *   볼지 고른 뒤** GEN3_EQ_CONTROL_OFF 를 쓴다. 같은 주소가 선택에 따라
 *   다른 레지스터를 가리키는 구조다.
 *     선택 0 (Gen3) : 프리셋 벡터 0x3ff -- 열 개 프리셋을 모두 허용한다.
 *     선택 1 (Gen4) : of_data 의 gen4_preset_vec. Tegra194 는 0x360,
 *       Tegra234 는 0x340 이며, 각 정의 위의 원문 주석이 어느 프리셋이
 *       켜지는지 밝힌다.
 *   마지막에 선택을 0 으로 되돌리는데, 다른 코드가 이 레지스터를 읽을 때
 *   예상 밖의 설정을 보지 않게 하는 정리로 보인다.
 *
 * FB_MODE 를 두 번 모두 지우는 것에 주의 -- 되먹임 모드를 끄는 것으로
 * 보이나, 그 의미의 근거 문서는 이 트리에 없다.
 *
 * RC 와 EP 양쪽 초기화가 모두 이 함수를 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화 경로). 링크가 서기 전이어야 한다.
 *
 * 호출 체인:
 *   tegra_pcie_dw_host_init / pex_ep_event_pex_rst_deassert → [이 함수]
 *     → dw_pcie_read/write 계열, dw_pcie_find_ext_capability
 */
static void config_gen3_gen4_eq_presets(struct tegra_pcie_dw *pcie)
{
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 레지스터 값, 능력 오프셋, 레인 첨자 */
	u32 val, offset, i;

	/* Program init preset */
	for (i = 0; i < pcie->num_lanes; i++) {
		/* [한국어] **레인마다 2바이트씩 떨어져 있다** -- 16비트 항목이기 때문이다 */
		val = dw_pcie_readw_dbi(pci, CAP_SPCIE_CAP_OFF + (i * 2));
		/* [한국어] 하류 필드를 지운다 */
		val &= ~CAP_SPCIE_CAP_OFF_DSP_TX_PRESET0_MASK;
		/* [한국어] 하류 방향 프리셋을 넣는다 */
		val |= GEN3_GEN4_EQ_PRESET_INIT;
		/* [한국어] 상류 필드를 지운다 */
		val &= ~CAP_SPCIE_CAP_OFF_USP_TX_PRESET0_MASK;
		/* [한국어] 상류 방향에도 같은 값을 넣는다 */
		val |= (GEN3_GEN4_EQ_PRESET_INIT <<
			   CAP_SPCIE_CAP_OFF_USP_TX_PRESET0_SHIFT);
		/* [한국어] 레인 프리셋을 쓴다 */
		dw_pcie_writew_dbi(pci, CAP_SPCIE_CAP_OFF + (i * 2), val);

		/* [한국어] 16GT/s 능력의 레인 이퀄라이제이션 제어 오프셋을 찾는다 */
		offset = dw_pcie_find_ext_capability(pci,
						     PCI_EXT_CAP_ID_PL_16GT) +
				PCI_PL_16GT_LE_CTRL;
		/* [한국어] **레인마다 1바이트씩 떨어져 있다** -- 위의 2바이트 간격과 다르다 */
		val = dw_pcie_readb_dbi(pci, offset + i);
		/* [한국어] 하류 필드를 지운다 */
		val &= ~PCI_PL_16GT_LE_CTRL_DSP_TX_PRESET_MASK;
		/* [한국어] 하류 방향 프리셋을 넣는다 */
		val |= GEN3_GEN4_EQ_PRESET_INIT;
		/* [한국어] 상류 필드를 지운다 */
		val &= ~PCI_PL_16GT_LE_CTRL_USP_TX_PRESET_MASK;
		/* [한국어] 상류 방향에도 같은 값을 넣는다 */
		val |= (GEN3_GEN4_EQ_PRESET_INIT <<
			PCI_PL_16GT_LE_CTRL_USP_TX_PRESET_SHIFT);
		/* [한국어] 16GT/s 프리셋을 쓴다 */
		dw_pcie_writeb_dbi(pci, offset + i, val);
	}

	/* [한국어] 현재 값을 읽는다 */
	val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
	/* [한국어] **속도 선택 필드를 0(Gen3)으로 둔다.** 이 필드가 아래 레지스터가
	 * 어느 속도의 설정을 가리킬지 정한다 -- 같은 주소가 선택에 따라 다른
	 * 레지스터를 가리키는 구조다 */
	val &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
	/* [한국어] 선택을 쓴다 */
	dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);

	/* [한국어] 선택이 0(Gen3)인 상태의 레지스터를 읽는다 */
	val = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
	/* [한국어] 기존 벡터를 지운다 */
	val &= ~GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC;
	/* [한국어] **Gen3 은 프리셋 열 개를 모두 허용한다**(0x3ff) */
	val |= FIELD_PREP(GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC, 0x3ff);
	/* [한국어] 되먹임 모드를 끈다 */
	val &= ~GEN3_EQ_CONTROL_OFF_FB_MODE;
	/* [한국어] Gen3 설정을 쓴다 */
	dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, val);

	/* [한국어] 현재 값을 읽는다 */
	val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
	/* [한국어] 선택 필드를 지운다 */
	val &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
	/* [한국어] **선택을 1(Gen4)로 바꾼다** */
	val |= (0x1 << GEN3_RELATED_OFF_RATE_SHADOW_SEL_SHIFT);
	/* [한국어] 선택을 쓴다 */
	dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);

	/* [한국어] **같은 주소인데 위와 다른 레지스터를 가리킨다** -- 바로 위에서
	 * 선택을 1 로 바꿨기 때문이다 */
	val = dw_pcie_readl_dbi(pci, GEN3_EQ_CONTROL_OFF);
	/* [한국어] 기존 벡터를 지운다 */
	val &= ~GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC;
	/* [한국어] **SoC 별 Gen4 프리셋 벡터를 넣는다** -- Tegra194 는 0x360,
	 * Tegra234 는 0x340 이며, 각 정의 위의 원문 주석이 어느 프리셋이
	 * 켜지는지 밝힌다 */
	val |= FIELD_PREP(GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC,
			  pcie->of_data->gen4_preset_vec);
	/* [한국어] 되먹임 모드를 끈다. 그 의미의 근거는 이 트리에 없다 */
	val &= ~GEN3_EQ_CONTROL_OFF_FB_MODE;
	/* [한국어] Gen4 설정을 쓴다 */
	dw_pcie_writel_dbi(pci, GEN3_EQ_CONTROL_OFF, val);

	/* [한국어] 현재 값을 읽는다 */
	val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
	/* [한국어] **선택을 0 으로 되돌린다.** 다른 코드가 이 레지스터를 읽을 때
	 * 예상 밖의 설정을 보지 않게 하는 정리로 보인다 */
	val &= ~GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK;
	dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);
}

/* [한국어]
 * tegra_pcie_dw_host_init - dw_pcie_host_ops.init 콜백. 루트 포트 설정을 마친다
 *
 * @pp: DWC 루트 포트 구조체.
 * @return: 항상 0. 실패를 보고하는 경로가 없다.
 *
 * **이 드라이버가 DWC 코어에 채워 준 유일한 host_ops 콜백이다.**
 * dw_pcie_host_init 이 내부에서 이 함수를 불러 준다 -- 제어가
 * 드라이버 → 코어 → 드라이버로 왕복하는 접착 계층의 전형적인 모습이다.
 *
 *   1) 브리지의 pci_ops 를 이 파일의 것으로 바꾼다. DWC 코어가 기본값을
 *      넣어 두었을 텐데, MSI-X 도어벨 우회가 필요해 덮어쓴다.
 *   2) PCIe 능력 구조의 위치를 찾아 저장한다. 이 값이 없으면 링크 상태를
 *      읽을 수 없으므로, 아직 없을 때만 찾는다 -- resume 경로에서 다시
 *      불릴 때 중복 탐색을 피하는 것이다.
 *   3) IO 디코딩을 끄고 prefetchable 메모리 디코딩을 켠다. 브리지가 어떤
 *      주소 범위를 하류로 전달할지 정하는 표준 설정이다.
 *   4) BAR0 을 0 으로 지운다. 루트 포트는 자기 BAR 를 쓰지 않는다.
 *   5) **RRS 응답 형식을 0xFFFF0001 로 정한다.** RRS(Request Retry Status)는
 *      장치가 아직 준비되지 않았을 때 보내는 응답인데, 그때 CPU 에게
 *      어떤 값을 돌려줄지를 고른다. 0xFFFF0001 은 "벤더 ID 는 유효하고
 *      나머지는 1" 이라는 특수 패턴으로, PCI 코어가 재시도해야 함을
 *      알아챌 수 있게 한다.
 *   6) SRNS 구성이면 슬롯 클록 구성 비트를 지운다. SRNS 는 상하류가 서로
 *      다른 참조 클록을 쓰는 구성이라, "같은 클록을 쓴다" 는 표시를
 *      지워야 한다.
 *   7) 이퀄라이저 프리셋과 ASPM 을 설정한다.
 *   8) has_l1ss_exit_fix 가 없는 SoC 는 GEN3_RELATED_OFF 의 비트 하나를
 *      지운다. 이름(ZRXDC_NONCOMPL)이 수신 종단 규격 미준수와 관련됨을
 *      가리키나, 근거 문서는 이 트리에 없다.
 *   9) update_fc_fixup 이면 흐름 제어 타이머를 조정한다. 장치 트리가
 *      지정하는 값이라 보드별 우회다.
 *  10) 코어 클록을 Gen4 주파수로 올리고 모니터 클록을 켠다.
 *
 * **probe 와 resume 이 모두 이 함수를 부르며, start_link 의 DLF 재시도
 * 경로도 부른다.** 그래서 여러 번 불려도 안전해야 하고, 2)의 조건 검사가
 * 그 배려다. 다만 10)의 clk_prepare_enable 은 그런 배려가 없어,
 * start_link 가 다시 부르기 전에 clk_disable_unprepare 로 짝을 맞춘다 --
 * 그 함수 안의 원문 주석이 그 사정을 밝힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_host_init → pp->ops->init → [이 함수]
 *     → config_gen3_gen4_eq_presets, init_host_aspm, clk_set_rate
 */
static int tegra_pcie_dw_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	/* [한국어] 드라이버 상태를 되찾는다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;
	/* [한국어] 16비트 레지스터 값 임시 변수 */
	u16 val_16;

	/* [한국어] **브리지의 pci_ops 를 이 파일의 것으로 바꾼다.** DWC 코어가 기본값을
	 * 넣어 두었을 텐데, MSI-X 도어벨 우회가 필요해 덮어쓴다 */
	pp->bridge->ops = &tegra_pci_ops;

	/* [한국어] **아직 없을 때만 찾는다.** 이 함수가 resume 과 DLF 재시도에서 다시
	 * 불리므로 중복 탐색을 피한다 */
	if (!pcie->pcie_cap_base)
		/* [한국어] PCIe 능력 구조의 위치를 찾는다 */
		pcie->pcie_cap_base = dw_pcie_find_capability(&pcie->pci,
							      PCI_CAP_ID_EXP);

	/* [한국어] 현재 값을 읽는다 */
	val = dw_pcie_readl_dbi(pci, PCI_IO_BASE);
	/* [한국어] **IO 디코딩을 끈다** -- 이 루트 포트는 IO 공간을 하류로 전달하지
	 * 않는다. 바로 아래에서 메모리는 켜는 것과 대비된다 */
	val &= ~(IO_BASE_IO_DECODE | IO_BASE_IO_DECODE_BIT8);
	/* [한국어] 되쓴다 */
	dw_pcie_writel_dbi(pci, PCI_IO_BASE, val);

	/* [한국어] 현재 값을 읽는다 */
	val = dw_pcie_readl_dbi(pci, PCI_PREF_MEMORY_BASE);
	/* [한국어] prefetchable 메모리 디코딩을 켠다 */
	val |= CFG_PREF_MEM_LIMIT_BASE_MEM_DECODE;
	/* [한국어] 한계 디코딩도 켠다 */
	val |= CFG_PREF_MEM_LIMIT_BASE_MEM_LIMIT_DECODE;
	/* [한국어] 되쓴다 */
	dw_pcie_writel_dbi(pci, PCI_PREF_MEMORY_BASE, val);

	/* [한국어] **루트 포트는 자기 BAR 를 쓰지 않으므로 0 으로 지운다** */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 0);

	/* Enable as 0xFFFF0001 response for RRS */
	val = dw_pcie_readl_dbi(pci, PORT_LOGIC_AMBA_ERROR_RESPONSE_DEFAULT);
	/* [한국어] 기존 형식 필드를 지운다 */
	val &= ~(AMBA_ERROR_RESPONSE_RRS_MASK << AMBA_ERROR_RESPONSE_RRS_SHIFT);
	/* [한국어] **0xFFFF0001 형식을 고른다.** "벤더 ID 자리는 유효하고 나머지는 1"
	 * 이라는 특수 패턴이라, PCI 코어가 재시도해야 함을 알아챌 수 있다 */
	val |= (AMBA_ERROR_RESPONSE_RRS_OKAY_FFFF0001 <<
		AMBA_ERROR_RESPONSE_RRS_SHIFT);
	/* [한국어] RRS 형식을 쓴다 */
	dw_pcie_writel_dbi(pci, PORT_LOGIC_AMBA_ERROR_RESPONSE_DEFAULT, val);

	/* Clear Slot Clock Configuration bit if SRNS configuration */
	if (pcie->enable_srns) {
		/* [한국어] 링크 상태를 읽는다 */
		val_16 = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base +
					   PCI_EXP_LNKSTA);
		/* [한국어] 슬롯 클록 구성 비트를 지운다 -- **"상대와 같은 클록을 쓴다" 는 표시를
		 * 지우는 것이다** */
		val_16 &= ~PCI_EXP_LNKSTA_SLC;
		/* [한국어] 되쓴다 */
		dw_pcie_writew_dbi(pci, pcie->pcie_cap_base + PCI_EXP_LNKSTA,
				   val_16);
	}

	config_gen3_gen4_eq_presets(pcie);

	init_host_aspm(pcie);

	/* [한국어] 결함이 고쳐지지 않은 SoC 에서만 우회한다 */
	if (!pcie->of_data->has_l1ss_exit_fix) {
		/* [한국어] 현재 값을 읽는다 */
		val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
		/* [한국어] 수신 종단 규격 관련 비트를 지운다 */
		val &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;
		/* [한국어] 되쓴다 */
		dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);
	}

	/* [한국어] 보드가 지정하는 우회다 */
	if (pcie->update_fc_fixup) {
		/* [한국어] 현재 값을 읽는다 */
		val = dw_pcie_readl_dbi(pci, CFG_TIMER_CTRL_MAX_FUNC_NUM_OFF);
		/* [한국어] ACK/NAK 자리에 1 을 세운다 */
		val |= 0x1 << CFG_TIMER_CTRL_ACK_NAK_SHIFT;
		/* [한국어] 되쓴다 */
		dw_pcie_writel_dbi(pci, CFG_TIMER_CTRL_MAX_FUNC_NUM_OFF, val);
	}

	/* [한국어] 코어 클록을 Gen4 주파수로 올린다 */
	clk_set_rate(pcie->core_clk, GEN4_CORE_CLK_FREQ);
	/* [한국어] 모니터 클록을 켠다. **start_link 의 DLF 재시도가 이 함수를 다시
	 * 부르기 전에 clk_disable_unprepare 로 짝을 맞춘다** --
	 * 그 함수 안의 원문 주석이 그 사정을 밝힌다 */
	if (clk_prepare_enable(pcie->core_clk_m))
		/* [한국어] 모니터 클록 실패는 로그만 찍고 진행한다 */
		dev_err(pci->dev, "Failed to enable core monitor clock\n");

	return 0;
}

/* [한국어]
 * tegra_pcie_dw_start_link - dw_pcie_ops.start_link 콜백. 링크를 세운다
 *
 * @pci: DWC 코어 구조체.
 * @return: 항상 0. 링크가 안 서도 0 을 돌려준다.
 *
 * **EP 모드에서는 아무것도 하지 않는다.** PERST 인터럽트를 켜고 곧바로
 * 반환하는데, 엔드포인트는 호스트가 PERST 를 풀어 줄 때까지 기다리는
 * 쪽이기 때문이다. 실제 초기화는 그 인터럽트가 오면
 * pex_ep_event_pex_rst_deassert 가 한다.
 *
 * RC 모드의 링크 기동 순서:
 *   1) PERST 를 어서트한다(비트를 지워서).
 *   2) 100~200us 기다린다.
 *   3) LTSSM 을 켠다. **PERST 가 눌린 상태에서 켜는 것** 이 요점이다.
 *   4) PERST 를 푼다. 이제 상대가 링크 훈련을 시작한다.
 *   5) 100ms 기다린 뒤 링크를 확인한다.
 *
 * 링크가 안 서면 **DLF 를 끄고 한 번 더 시도한다.** 이것이 이 함수의
 * 핵심이다. 위의 원문 주석이 배경을 밝히는데, 루트 포트가 DLF(Data Link
 * Feature)를 켜 두면 링크를 못 세우는 엔드포인트가 있다.
 *
 * 다만 무작정 재시도하지 않는다. LTSSM 상태가 0x11 이고 링크가 안
 * 올라온 상태 -- 즉 데이터 링크 계층에서 막힌 경우에만 DLF 를 의심한다.
 * 그 밖의 경우는 "정당한 이유로 링크가 없는 것" 으로 보고 그대로 끝낸다
 * (카드가 안 꽂혀 있는 등).
 *
 * 재시도 준비가 꽤 무겁다 -- LTSSM 을 끄고, 코어를 리셋했다 풀고,
 * DLF 능력에서 교환 활성화 비트를 지우고, host_init 과 setup_rc 를 다시
 * 부른다. 코어 리셋이 레지스터를 초기화하므로 설정을 다시 해야 하기
 * 때문이다. 그 사이에 모니터 클록을 끄는데, host_init 이 다시 켤 것이라
 * 짝을 맞추는 것이며 원문 주석이 그 사정을 적고 있다.
 *
 * retry 플래그로 한 번만 재시도하도록 막는다.
 *
 * 링크가 서면 대역폭을 맞추고 인터럽트를 켠다. **인터럽트를 링크가 선
 * 뒤에 켜는 것** 은 tegra_pcie_enable_system_interrupts 가 초기 링크 폭을
 * 읽어야 하기 때문이다.
 *
 * **링크가 없어도 0 을 돌려주는 데 주의한다.** 링크 실패를 오류로 보지
 * 않으며, RC 경로에서는 tegra_pcie_config_rp 가 별도로 링크 상태를
 * 확인해 -ENOMEDIUM 을 만든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:
 *   dw_pcie_host_init / tegra_pcie_dw_resume_noirq → pci->ops->start_link
 *     → [이 함수] → dw_pcie_wait_for_link, tegra_pcie_dw_host_init,
 *       dw_pcie_setup_rc, tegra_pcie_icc_set, tegra_pcie_enable_interrupts
 */
static int tegra_pcie_dw_start_link(struct dw_pcie *pci)
{
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);
	/* [한국어] 루트 포트 구조체. DLF 재시도 경로에서 host_init 에 넘긴다 */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 레지스터 값과 능력 오프셋과 임시 변수 */
	u32 val, offset, tmp;
	/* [한국어] 한 번만 재시도하도록 막는 플래그 */
	bool retry = true;

	/* [한국어] **EP 모드는 아무것도 하지 않는다.** PERST 인터럽트만 켜고 반환하는데,
	 * 엔드포인트는 호스트가 PERST 를 풀어 줄 때까지 기다리는 쪽이기 때문이다.
	 * 실제 초기화는 그 인터럽트가 오면 pex_ep_event_pex_rst_deassert 가 한다 */
	if (pcie->of_data->mode == DW_PCIE_EP_TYPE) {
		enable_irq(pcie->pex_rst_irq);
		return 0;
	}

retry_link:
	/* Assert RST */
	val = appl_readl(pcie, APPL_PINMUX);
	/* [한국어] PERST 비트를 지운다 -- 0이 어서트다 */
	val &= ~APPL_PINMUX_PEX_RST;
	/* [한국어] PERST 를 건다 */
	appl_writel(pcie, val, APPL_PINMUX);

	/* [한국어] 100~200us 유지한다 */
	usleep_range(100, 200);

	/* Enable LTSSM */
	val = appl_readl(pcie, APPL_CTRL);
	/* [한국어] **PERST 가 눌린 상태에서 LTSSM 을 켜는 것이 요점이다** */
	val |= APPL_CTRL_LTSSM_EN;
	/* [한국어] LTSSM 을 켠다 */
	appl_writel(pcie, val, APPL_CTRL);

	/* De-assert RST */
	val = appl_readl(pcie, APPL_PINMUX);
	/* [한국어] PERST 비트를 세운다 -- 1이 해제다 */
	val |= APPL_PINMUX_PEX_RST;
	/* [한국어] PERST 를 푼다. **이제 상대가 링크 훈련을 시작한다** */
	appl_writel(pcie, val, APPL_PINMUX);

	msleep(100);

	/* [한국어] 링크가 섰는지 확인한다. **DWC 코어가 기다려 준다** --
	 * 그 안에서 tegra_pcie_dw_link_up 콜백을 반복해서 부른다 */
	if (dw_pcie_wait_for_link(pci)) {
		/* [한국어] **이미 재시도했으면 그대로 끝낸다** */
		if (!retry)
			return 0;
		/*
		 * There are some endpoints which can't get the link up if
		 * root port has Data Link Feature (DLF) enabled.
		 * Refer Spec rev 4.0 ver 1.0 sec 3.4.2 & 7.7.4 for more info
		 * on Scaled Flow Control and DLF.
		 * So, need to confirm that is indeed the case here and attempt
		 * link up once again with DLF disabled.
		 */
		val = appl_readl(pcie, APPL_DEBUG);
		/* [한국어] LTSSM 상태 필드만 남긴다 */
		val &= APPL_DEBUG_LTSSM_STATE_MASK;
		/* [한국어] 상태 번호로 만든다. **여기서만 시프트한다** -- 절전과 EP 경로는
		 * 시프트하지 않고 이미 시프트된 상수와 직접 비교한다 */
		val >>= APPL_DEBUG_LTSSM_STATE_SHIFT;
		/* [한국어] 링크 상태를 읽는다 */
		tmp = appl_readl(pcie, APPL_LINK_STATUS);
		/* [한국어] 링크 업 비트만 남긴다 */
		tmp &= APPL_LINK_STATUS_RDLH_LINK_UP;
		if (!(val == 0x11 && !tmp)) {
			/* Link is down for all good reasons */
			return 0;
		}

		/* [한국어] 데이터 링크 계층에서 막혔음을 알린다 */
		dev_info(pci->dev, "Link is down in DLL");
		dev_info(pci->dev, "Trying again with DLFE disabled\n");
		/* Disable LTSSM */
		val = appl_readl(pcie, APPL_CTRL);
		/* [한국어] LTSSM 활성화 비트를 지운다 */
		val &= ~APPL_CTRL_LTSSM_EN;
		/* [한국어] LTSSM 을 끈다 */
		appl_writel(pcie, val, APPL_CTRL);

		reset_control_assert(pcie->core_rst);
		reset_control_deassert(pcie->core_rst);

		/* [한국어] DLF 능력의 위치를 찾는다 */
		offset = dw_pcie_find_ext_capability(pci, PCI_EXT_CAP_ID_DLF);
		/* [한국어] 현재 값을 읽는다 */
		val = dw_pcie_readl_dbi(pci, offset + PCI_DLF_CAP);
		/* [한국어] 교환 활성화 비트를 지운다 */
		val &= ~PCI_DLF_EXCHANGE_ENABLE;
		/* [한국어] DLF 설정을 쓴다 */
		dw_pcie_writel_dbi(pci, offset + PCI_DLF_CAP, val);

		/*
		 * core_clk_m is enabled as part of host_init callback in
		 * dw_pcie_host_init(). Disable the clock since below
		 * tegra_pcie_dw_host_init() will enable it again.
		 */
		clk_disable_unprepare(pcie->core_clk_m);
		tegra_pcie_dw_host_init(pp);
		dw_pcie_setup_rc(pp);

		/* [한국어] **한 번만 재시도하도록 막는다** */
		retry = false;
		goto retry_link;
	}

	tegra_pcie_icc_set(pcie);

	tegra_pcie_enable_interrupts(pp);

	return 0;
}

/* [한국어]
 * tegra_pcie_dw_link_up - dw_pcie_ops.link_up 콜백. 링크가 섰는지 보고한다
 *
 * @pci: DWC 코어 구조체.
 * @return: 데이터 링크 계층이 활성이면 참.
 *
 * DWC 코어가 링크 상태를 알아야 할 때마다 부르는 콜백이다.
 * dw_pcie_wait_for_link 가 이 함수를 반복해서 부르며 링크를 기다린다.
 *
 * **표준 링크 상태 레지스터의 DLLLA 비트 하나만 본다.** 앞 세대
 * pci-tegra.c 의 tegra_pcie_port_check_link 이 벤더 확장 비트와 표준
 * 비트 두 가지를 각각 200회씩 폴링하며 3번 재시도하던 것과 견주면
 * 극적으로 단순하다. 기다리는 일은 DWC 코어가 하고, 이 드라이버는
 * "지금 섰는가" 라는 질문에만 답하기 때문이다.
 *
 * pcie_cap_base 가 필요하므로 tegra_pcie_dw_host_init 이 그것을 찾은
 * 뒤에만 유효하다.
 *
 * 이 파일 안에서도 여러 곳이 직접 부른다 -- tegra_pcie_try_link_l2,
 * tegra_pcie_dw_pme_turnoff, tegra_pcie_config_rp 가 링크 유무를 확인할 때다.
 *
 * 실행 컨텍스트: 어디서나. DBI 읽기 하나뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   DWC 코어(dw_pcie_wait_for_link 등) / 이 파일의 여러 함수 → [이 함수]
 *     → dw_pcie_readw_dbi
 */
static bool tegra_pcie_dw_link_up(struct dw_pcie *pci)
{
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);
	/* [한국어] **표준 링크 상태 레지스터의 비트 하나만 본다.** 앞 세대
	 * pci-tegra.c 의 tegra_pcie_port_check_link 이 두 조건을 각각 200회씩
	 * 폴링하며 3번 재시도하던 것과 견주면 극적으로 단순하다 --
	 * 기다리는 일은 DWC 코어가 하고 이 함수는 "지금 섰는가" 에만 답한다 */
	u32 val = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base + PCI_EXP_LNKSTA);

	return val & PCI_EXP_LNKSTA_DLLLA;
}

/* [한국어]
 * tegra_pcie_dw_stop_link - dw_pcie_ops.stop_link 콜백. 링크 기동을 멈춘다
 *
 * @pci: DWC 코어 구조체.
 * @return: 없음.
 *
 * **EP 모드에서만 실제로 무언가 한다** -- PERST 인터럽트를 끈다.
 * tegra_pcie_dw_start_link 가 EP 모드에서 그것을 켜는 것과 정확히 짝을
 * 이룬다. 인터럽트가 꺼지면 호스트가 PERST 를 흔들어도 반응하지 않으므로
 * 엔드포인트가 사실상 잠잠해진다.
 *
 * **RC 모드에서는 아무것도 하지 않는다.** LTSSM 을 끄거나 PERST 를 거는
 * 코드가 없는데, RC 쪽에서 링크를 실제로 내리는 일은
 * tegra_pcie_dw_pme_turnoff 가 절전 경로에서 따로 처리하기 때문이다.
 *
 * 세 dw_pcie_ops 콜백 중 가장 짧다. 채워야 하는 자리라 채웠으되 할 일이
 * 많지 않은 경우이며, 반대로 채우지 않은 다섯 자리(cpu_addr_fixup,
 * read_dbi 계열, get_ltssm)는 코어의 기본 동작을 그대로 쓴다는 선언이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   DWC 코어 → pci->ops->stop_link → [이 함수] → disable_irq
 */
static void tegra_pcie_dw_stop_link(struct dw_pcie *pci)
{
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);

	/* [한국어] **EP 모드에서만 실제로 무언가 한다** -- PERST 인터럽트를 끈다.
	 * RC 모드에서 링크를 실제로 내리는 일은 tegra_pcie_dw_pme_turnoff 가
	 * 절전 경로에서 따로 처리한다 */
	if (pcie->of_data->mode == DW_PCIE_EP_TYPE)
		disable_irq(pcie->pex_rst_irq);
}

/* [한국어] DWC 코어가 이 드라이버를 부르는 통로 */
static const struct dw_pcie_ops tegra_dw_pcie_ops = {
	/* [한국어] **여덟 자리 중 셋만 채운다.** cpu_addr_fixup 이나 read_dbi 계열을
	 * 채우지 않는다는 것은 DBI 접근과 주소 변환이 표준 그대로라는 뜻이다 */
	.link_up = tegra_pcie_dw_link_up,
	.start_link = tegra_pcie_dw_start_link,
	.stop_link = tegra_pcie_dw_stop_link,
};

/* [한국어] **init 하나만 채운다.** 특히 msi_init 을 채우지 않아 DWC 코어가
 * 자기 iMSI-RX 경로를 그대로 쓴다 -- pci-keystone.c 가 그것을 채워
 * 코어의 경로를 건너뛰는 것과 대비되며, 이 파일에 MSI 코드가 거의
 * 없는 근본 이유가 여기 있다 */
static const struct dw_pcie_host_ops tegra_pcie_dw_host_ops = {
	.init = tegra_pcie_dw_host_init,
};

/* [한국어]
 * tegra_pcie_disable_phy - p2u PHY 를 모두 끈다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음. 실패를 보고하지 않는다.
 *
 * **역순으로 순회한다.** while (phy_count--) 는 마지막 PHY 부터 첫 PHY 까지
 * 내려가며, 켤 때의 순서를 정확히 뒤집는다.
 *
 * PHY 마다 전원을 끄고 초기화를 해제한다. PHY 프레임워크가 그 둘을
 * 나눠 두었기 때문이며, tegra_pcie_enable_phy 가 init → power_on 순으로
 * 켠 것을 power_off → exit 순으로 되돌린다.
 *
 * **앞 세대와의 대비**: pci-tegra.c 는 phy_exit 을 여기 두지 않고
 * tegra_pcie_phys_put 에서 드라이버가 떨어질 때만 했다. 여기서는 전원을
 * 내릴 때마다 exit 까지 하므로, 다시 켤 때 init 부터 다시 한다.
 *
 * phy_count 를 지역 변수에 복사해 쓰는 것에 주의 -- 구조체 필드를 직접
 * 감소시키면 다음 호출에서 0 이 되어 버린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_unconfig_controller / pex_ep_event_pex_rst_assert /
 *   pex_ep_event_pex_rst_deassert(오류) → [이 함수]
 */
static void tegra_pcie_disable_phy(struct tegra_pcie_dw *pcie)
{
	unsigned int phy_count = pcie->phy_count;

	/* [한국어] **역순으로 순회한다.** 마지막 PHY 부터 내려가며 켤 때의 순서를
	 * 정확히 뒤집는다. 필드가 아니라 지역 변수를 감소시키는 데 주의 --
	 * 구조체 필드를 직접 줄이면 다음 호출에서 0 이 되어 버린다 */
	while (phy_count--) {
		phy_power_off(pcie->phys[phy_count]);
		phy_exit(pcie->phys[phy_count]);
	}
}

/* [한국어]
 * tegra_pcie_enable_phy - p2u PHY 를 모두 켠다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 성공, 음수 오류.
 *
 * PHY 마다 초기화하고 전원을 넣는다. **EP 모드에서는 보정까지 한다** --
 * phy_calibrate 는 RC 모드에서 부르지 않는데, 엔드포인트가 호스트의
 * 클록에 맞춰야 하기 때문으로 보이나 그 이유가 코드에 적혀 있지는 않다.
 *
 * **정리 경로의 goto 배치가 특이하다.** phy_power_off 라벨이 while 루프
 * 바깥에 있고 phy_exit 라벨이 그 루프 **안쪽** 에 있어, 두 진입점이
 * 같은 루프를 공유한다.
 *   phy_init 이 실패하면 → phy_exit 라벨로 뛰어 i 번째의 exit 부터
 *     시작하는 것이 아니라, while (i--) 를 건너뛴 채 루프 몸통에 진입한다.
 *   phy_power_on 이 실패하면 → phy_power_off 라벨로 뛰어 while (i--) 부터
 *     시작하므로 이미 성공한 것들만 되돌린다.
 * C 의 라벨이 블록 구조를 무시한다는 성질을 이용한 압축이며, 읽기는
 * 어렵지만 두 실패 지점의 정리 범위가 정확히 갈린다.
 *
 * PHY 이름이 "p2u-0", "p2u-1" 형식이고 probe 가 그것을 얻어 둔다.
 * p2u 는 이 SoC 의 PHY 종류 이름으로 보이나, 그 확장의 근거는 이 트리에
 * 없다.
 *
 * **앞 세대와의 대비**: pci-tegra.c 는 PHY 를 세 갈래(컨트롤러 단위 /
 * 레인별 / PADS 직접 조작)로 다뤘지만 여기서는 하나뿐이다. 대신
 * UPHY PLL 은 BPMP 펌웨어가 따로 관리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_config_controller / pex_ep_event_pex_rst_deassert → [이 함수]
 */
static int tegra_pcie_enable_phy(struct tegra_pcie_dw *pcie)
{
	unsigned int i;
	/* [한국어] 오류 코드 보관용 */
	int ret;

	/* [한국어] 장치 트리가 알려 준 개수만큼 순회한다 */
	for (i = 0; i < pcie->phy_count; i++) {
		/* [한국어] PHY 를 초기화한다 */
		ret = phy_init(pcie->phys[i]);
		/* [한국어] **초기화 실패면 phy_exit 라벨로 뛴다.** 그 라벨이 while 루프 안쪽에
		 * 있어, while (i--) 를 건너뛴 채 몸통에 진입한다 -- i 번째의 exit 만
		 * 하고 끝나는 구조다 */
		if (ret < 0)
			goto phy_power_off;

		/* [한국어] PHY 에 전원을 넣는다 */
		ret = phy_power_on(pcie->phys[i]);
		/* [한국어] 전원 인가 실패면 power_off 라벨로 간다 */
		if (ret < 0)
			goto phy_exit;

		/* [한국어] **EP 모드에서만 PHY 를 보정한다.** 엔드포인트가 호스트의 클록에
		 * 맞춰야 하기 때문으로 보이나, 그 이유가 코드에 적혀 있지는 않다 */
		if (pcie->of_data->mode == DW_PCIE_EP_TYPE)
			phy_calibrate(pcie->phys[i]);
	}

	return 0;

phy_power_off:
	while (i--) {
		phy_power_off(pcie->phys[i]);
phy_exit:
		phy_exit(pcie->phys[i]);
	}

	return ret;
}

/* [한국어]
 * tegra_pcie_dw_parse_dt - 장치 트리에서 설정을 읽는다
 *
 * @pcie: 드라이버 상태. 여러 필드를 채운다.
 * @return: 0 성공, 음수 오류(-EPROBE_DEFER 포함).
 *
 * 필수와 선택이 섞여 있고, **실패를 다루는 방식이 항목마다 다르다.**
 *
 * 반드시 있어야 하는 것 (없으면 실패):
 *   dbi 레지스터 영역, nvidia,aspm-cmrt-us, num-lanes, nvidia,bpmp 의
 *   컨트롤러 ID, phy-names 의 개수.
 *
 * 없어도 되는 것 (로그만 찍고 진행):
 *   nvidia,aspm-pwr-on-t-us, nvidia,aspm-l0s-entrance-latency-us.
 *   **이 둘만 dev_info 를 찍고 반환하지 않는다.** 값이 0 으로 남아
 *   init_host_aspm 이 그대로 쓰는데, 그것이 의도된 기본값인지는 코드에
 *   적혀 있지 않다.
 *
 * 주의할 점: nvidia,aspm-cmrt-us 는 실패 시 **반환한다.** 바로 아래 두
 * 항목과 형태가 비슷해 보이지만 처리가 다르므로, ASPM 파라미터 중
 * 이것만 필수인 셈이다.
 *
 * 불리언 속성들:
 *   nvidia,update-fc-fixup, supports-clkreq, snps,enable-cdm-check 는
 *   단순히 있는지만 본다.
 *   nvidia,enable-srns 는 **Tegra234 에서만** 읽는다.
 *   nvidia,enable-ext-refclk 도 SoC 에 따라 다른데, Tegra194 는 속성을
 *   읽지 않고 **EP 모드이면 무조건 true** 로 놓는다. 위의 원문 주석대로
 *   외부 REFCLK 를 쓰는 RC 는 Tegra234 부터 지원되기 때문이다.
 *
 * RC 모드는 여기서 끝난다. **아래는 EP 전용이다** -- PERST GPIO 는
 * 반드시 있어야 하고(입력으로 얻어 인터럽트를 걸 것이다), REFCLK 선택
 * GPIO 는 없어도 된다.
 *
 * 두 GPIO 의 오류 처리에 dev_printk 와 KERN_DEBUG 를 쓰는 관용구가 있는데,
 * -EPROBE_DEFER 일 때만 로그 수준을 낮춰 재시도 중 로그가 쌓이지 않게
 * 하는 것이다. dev_err_probe 와 같은 목적을 손으로 구현한 셈이다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_dw_probe → [이 함수]
 */
static int tegra_pcie_dw_parse_dt(struct tegra_pcie_dw *pcie)
{
	struct platform_device *pdev = to_platform_device(pcie->dev);
	/* [한국어] 컨트롤러의 장치 트리 노드 */
	struct device_node *np = pcie->dev->of_node;
	/* [한국어] 오류 코드 보관용 */
	int ret;

	pcie->dbi_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	/* [한국어] 영역을 못 찾았는지 확인한다 */
	if (!pcie->dbi_res) {
		/* [한국어] DBI 영역이 없으면 IP 코어의 위치를 알 수 없다 */
		dev_err(pcie->dev, "Failed to find \"dbi\" region\n");
		return -ENODEV;
	}

	/* [한국어] ASPM T_cmrt 를 읽는다 */
	ret = of_property_read_u32(np, "nvidia,aspm-cmrt-us", &pcie->aspm_cmrt);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] **이 항목만 실패 시 반환한다.** 바로 아래 두 항목과 형태가 비슷해
		 * 보이지만 처리가 달라, ASPM 파라미터 중 이것만 필수인 셈이다.
		 * 메시지가 dev_info 인 것도 아래와 같아 헷갈리기 쉽다 */
		dev_info(pcie->dev, "Failed to read ASPM T_cmrt: %d\n", ret);
		return ret;
	}

	/* [한국어] ASPM 전원 인가 시간을 읽는다 */
	ret = of_property_read_u32(np, "nvidia,aspm-pwr-on-t-us",
				   &pcie->aspm_pwr_on_t);
	/* [한국어] 실패를 확인하되 멈추지 않는다 */
	if (ret < 0)
		/* [한국어] 역시 선택 사항이라 로그만 찍는다 */
		dev_info(pcie->dev, "Failed to read ASPM Power On time: %d\n",
			 ret);

	/* [한국어] L0s 진입 지연을 읽는다 */
	ret = of_property_read_u32(np, "nvidia,aspm-l0s-entrance-latency-us",
				   &pcie->aspm_l0s_enter_lat);
	/* [한국어] **선택 사항이라 로그만 찍고 진행한다.** 값이 0 으로 남아
	 * init_host_aspm 이 그대로 쓰는데, 그것이 의도된 기본값인지는
	 * 코드에 적혀 있지 않다 */
	if (ret < 0)
		dev_info(pcie->dev,
			 "Failed to read ASPM L0s Entrance latency: %d\n", ret);

	/* [한국어] 레인 수를 읽는다. **필수 항목이다** */
	ret = of_property_read_u32(np, "num-lanes", &pcie->num_lanes);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 레인 수를 못 읽으면 이퀄라이저 프리셋을 쓸 수 없다 */
		dev_err(pcie->dev, "Failed to read num-lanes: %d\n", ret);
		return ret;
	}

	/* [한국어] **속성의 두 번째 원소를 읽는다** -- 첫 번째는 BPMP 노드 참조이고
	 * 두 번째가 컨트롤러 ID 다. 그 ID 가 BPMP 메시지의 인자가 된다 */
	ret = of_property_read_u32_index(np, "nvidia,bpmp", 1, &pcie->cid);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] 컨트롤러 ID 를 못 읽으면 BPMP 에 무엇을 요청할지 알 수 없다 */
		dev_err(pcie->dev, "Failed to read Controller-ID: %d\n", ret);
		return ret;
	}

	/* [한국어] **PHY 이름의 개수를 센다.** 실제 핸들은 probe 가 "p2u-0" 형식으로 얻는다 */
	ret = of_property_count_strings(np, "phy-names");
	/* [한국어] 개수를 못 세면 오류다 */
	if (ret < 0) {
		/* [한국어] PHY 항목이 없으면 링크를 세울 수 없다 */
		dev_err(pcie->dev, "Failed to find PHY entries: %d\n",
			ret);
		return ret;
	}
	/* [한국어] PHY 개수를 보관한다. 이 값이 probe 의 배열 크기와 순회 횟수를 정한다 */
	pcie->phy_count = ret;

	/* [한국어] 속성이 있는지만 본다 */
	if (of_property_read_bool(np, "nvidia,update-fc-fixup"))
		/* [한국어] 흐름 제어 타이머 우회 요청. **SoC 기술자가 아니라 장치 트리에서
		 * 오므로 보드가 정하는 우회다** */
		pcie->update_fc_fixup = true;

	/* RP using an external REFCLK is supported only in Tegra234 */
	if (pcie->of_data->version == TEGRA194_DWC_IP_VER) {
		/* [한국어] EP 모드인지 확인한다 */
		if (pcie->of_data->mode == DW_PCIE_EP_TYPE)
			/* [한국어] **Tegra194 는 속성을 읽지 않고 EP 모드이면 무조건 참으로 놓는다.**
			 * 위의 원문 주석대로 외부 REFCLK 를 쓰는 RC 는 Tegra234 부터
			 * 지원되기 때문이다 */
			pcie->enable_ext_refclk = true;
	} else {
		/* [한국어] Tegra234 는 속성을 읽어 정한다 -- RC 도 외부 클록을 쓸 수 있다 */
		pcie->enable_ext_refclk =
			of_property_read_bool(pcie->dev->of_node,
					      "nvidia,enable-ext-refclk");
	}

	/* [한국어] CLKREQ 지원 여부. L1.1 과 L1.2 가 그 신호에 의존하므로,
	 * 이 값이 거짓이면 그 절전 상태들을 쓸 수 없다 */
	pcie->supports_clkreq =
		of_property_read_bool(pcie->dev->of_node, "supports-clkreq");

	/* [한국어] CDM 레지스터 무결성 검사 요청 여부. **속성 이름이 snps 로 시작하는
	 * 데서 이것이 DWC IP 공통 기능임을 알 수 있다** */
	pcie->enable_cdm_check =
		of_property_read_bool(np, "snps,enable-cdm-check");

	/* [한국어] **Tegra234 에서만 이 속성을 읽는다.** Tegra194 에서는 항상 거짓으로
	 * 남으므로, 그 세대는 상하류가 같은 참조 클록을 쓴다고 전제한다 */
	if (pcie->of_data->version == TEGRA234_DWC_IP_VER)
		/* [한국어] SRNS 여부를 읽는다 */
		pcie->enable_srns =
			of_property_read_bool(np, "nvidia,enable-srns");

	/* [한국어] **RC 모드는 여기서 끝난다.** 아래는 전부 EP 전용 항목이다 */
	if (pcie->of_data->mode == DW_PCIE_RC_TYPE)
		return 0;

	/* Endpoint mode specific DT entries */
	pcie->pex_rst_gpiod = devm_gpiod_get(pcie->dev, "reset", GPIOD_IN);
	/* [한국어] **PERST GPIO 는 필수다** -- 없으면 EP 모드를 쓸 수 없으므로
	 * 오류를 그대로 올린다 */
	if (IS_ERR(pcie->pex_rst_gpiod)) {
		/* [한국어] 오류 코드를 꺼낸다 */
		int err = PTR_ERR(pcie->pex_rst_gpiod);
		/* [한국어] 기본 로그 수준 */
		const char *level = KERN_ERR;

		/* [한국어] -EPROBE_DEFER 인지 확인한다 */
		if (err == -EPROBE_DEFER)
			/* [한국어] 재시도 중이면 디버그 수준으로 낮춘다 */
			level = KERN_DEBUG;

		/* [한국어] 수준을 골라 찍는다. dev_err_probe 와 같은 목적을 손으로 구현한 관용구다 */
		dev_printk(level, pcie->dev,
			   dev_fmt("Failed to get PERST GPIO: %d\n"),
			   err);
		return err;
	}

	/* [한국어] REFCLK 소스 선택 GPIO 를 얻는다. **optional 이다** */
	pcie->pex_refclk_sel_gpiod = devm_gpiod_get_optional(pcie->dev,
							     "nvidia,refclk-select",
							     GPIOD_OUT_HIGH);
	/* [한국어] 오류 포인터를 확인한다 */
	if (IS_ERR(pcie->pex_refclk_sel_gpiod)) {
		/* [한국어] 오류 코드를 꺼낸다 */
		int err = PTR_ERR(pcie->pex_refclk_sel_gpiod);
		/* [한국어] 기본 로그 수준 */
		const char *level = KERN_ERR;

		/* [한국어] -EPROBE_DEFER 인지 확인한다 */
		if (err == -EPROBE_DEFER)
			/* [한국어] 재시도 중이면 디버그 수준으로 낮춘다 */
			level = KERN_DEBUG;

		/* [한국어] 수준을 골라 찍는다 */
		dev_printk(level, pcie->dev,
			   dev_fmt("Failed to get REFCLK select GPIOs: %d\n"),
			   err);
		/* [한국어] **없으면 NULL 로 두고 진행한다** -- 이 GPIO 는 선택 사항이라
		 * 오류를 내지 않는다 */
		pcie->pex_refclk_sel_gpiod = NULL;
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_bpmp_set_ctrl_state - BPMP 펌웨어에 컨트롤러를 켜거나 끄라고 요청한다
 *
 * @pcie:   드라이버 상태. cid 로 어느 컨트롤러인지 알린다.
 * @enable: 켤 것인지 끌 것인지.
 * @return: 0 성공. 전송 실패나 펌웨어가 오류를 돌려주면 음수.
 *
 * **이 함수가 앞 세대와 가장 크게 갈리는 지점 중 하나다.** pci-tegra.c 는
 * PADS 레지스터를 직접 만져 PHY 와 PLL 을 재우고 깨웠지만, 여기서는
 * UPHY 를 드라이버가 만지지 않고 BPMP(Boot and Power Management Processor)
 * 펌웨어에 메시지를 보내 맡긴다.
 *
 * 메시지 구조가 정형적이다 -- 요청과 응답 구조체를 0 으로 밀고, 명령과
 * 인자를 채우고, 전송 구조체에 양쪽 버퍼와 크기를 지정한 뒤
 * tegra_bpmp_transfer 로 보낸다. 오류를 두 겹으로 확인하는데, 전송 자체의
 * 실패(err)와 펌웨어가 돌려준 결과(msg.rx.ret)가 별개이기 때문이다.
 *
 * **Tegra194 의 컨트롤러 5 만 예외다.** 위의 원문 주석대로 그 조합은
 * BPMP 가 상태를 정해 줄 필요가 없어 곧바로 성공을 돌려준다.
 *
 * 메시지 상수들(MRQ_UPHY, CMD_UPHY_PCIE_CONTROLLER_STATE)과
 * tegra_bpmp_transfer 의 정의는 **이 스파스 체크아웃에 없다**
 * (include/soc/tegra 가 존재하지 않음). 이름과 쓰임으로만 설명한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 펌웨어 응답을 기다리므로 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_pcie_config_controller / _unconfig_controller /
 *   pex_ep_event_pex_rst_assert / _deassert → [이 함수] → tegra_bpmp_transfer
 */
static int tegra_pcie_bpmp_set_ctrl_state(struct tegra_pcie_dw *pcie,
					  bool enable)
{
	struct mrq_uphy_response resp;
	/* [한국어] 전송 구조체. **이 타입의 정의는 이 스파스 체크아웃에 없다** */
	struct tegra_bpmp_message msg;
	/* [한국어] 요청 구조체 */
	struct mrq_uphy_request req;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/*
	 * Controller-5 doesn't need to have its state set by BPMP-FW in
	 * Tegra194
	 */
	if (pcie->of_data->version == TEGRA194_DWC_IP_VER && pcie->cid == 5)
		return 0;

	/* [한국어] 요청 구조체를 0 으로 민다. **보내지 않는 필드가 쓰레기 값이 되지
	 * 않게 하는 것이며, 펌웨어와 주고받는 구조체라 특히 중요하다** */
	memset(&req, 0, sizeof(req));
	/* [한국어] 응답 구조체를 0 으로 민다 */
	memset(&resp, 0, sizeof(resp));

	/* [한국어] 컨트롤러 상태 변경 명령 */
	req.cmd = CMD_UPHY_PCIE_CONTROLLER_STATE;
	/* [한국어] 어느 컨트롤러인지 알린다 */
	req.controller_state.pcie_controller = pcie->cid;
	/* [한국어] **켤지 끌지를 인자로 실어 보낸다** -- PLL 쪽이 명령 자체를 나누는
	 * 것과 대비된다 */
	req.controller_state.enable = enable;

	/* [한국어] 전송 구조체를 0 으로 민다 */
	memset(&msg, 0, sizeof(msg));
	/* [한국어] UPHY 관련 요청임을 알린다 */
	msg.mrq = MRQ_UPHY;
	/* [한국어] 요청 버퍼 */
	msg.tx.data = &req;
	/* [한국어] 요청 크기 */
	msg.tx.size = sizeof(req);
	/* [한국어] 응답 버퍼 */
	msg.rx.data = &resp;
	/* [한국어] 응답 버퍼 크기 */
	msg.rx.size = sizeof(resp);

	/* [한국어] BPMP 에 메시지를 보낸다 */
	err = tegra_bpmp_transfer(pcie->bpmp, &msg);
	/* [한국어] 전송 자체의 실패를 확인한다 */
	if (err)
		return err;
	/* [한국어] 펌웨어가 오류를 돌려줬는지 확인한다 */
	if (msg.rx.ret)
		return -EINVAL;

	return 0;
}

/* [한국어]
 * tegra_pcie_bpmp_set_pll_state - BPMP 펌웨어에 UPHY PLL 을 켜거나 끄라고 요청한다
 *
 * @pcie:   드라이버 상태.
 * @enable: 켤 것인지 끌 것인지.
 * @return: 0 성공, 음수 오류.
 *
 * tegra_pcie_bpmp_set_ctrl_state 와 같은 메시지 구조를 쓰되 명령이 다르다.
 * **켤 때와 끌 때 명령 자체가 갈린다** -- INIT 과 OFF 로, 하나의 명령에
 * 불리언을 실어 보내는 방식이 아니다. 그래서 인자를 담는 공용체 필드도
 * 각각 다르다(ep_ctrlr_pll_init 과 ep_ctrlr_pll_off).
 *
 * 명령 이름에 EP 가 들어 있지만 **RC 모드에서도 불린다** --
 * tegra_pcie_config_controller 가 enable_ext_refclk 일 때 부른다.
 * 이름과 실제 쓰임이 어긋나는 셈인데, 원래 EP 용으로 만들어진 인터페이스를
 * 외부 REFCLK 를 쓰는 RC 에도 재사용하는 것으로 보인다.
 *
 * **enable_ext_refclk 일 때만 불린다는 점이 중요하다.** 컨트롤러가
 * 자기 클록을 쓰면 PLL 을 따로 켤 필요가 없고, 외부 참조 클록을 쓸 때만
 * UPHY PLL 을 별도로 초기화해야 한다.
 *
 * 여기서도 오류를 두 겹으로 확인한다. 상수와 전송 함수의 정의는
 * 이 트리에 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_pcie_config_controller / _unconfig_controller /
 *   pex_ep_event_pex_rst_assert / _deassert → [이 함수] → tegra_bpmp_transfer
 */
static int tegra_pcie_bpmp_set_pll_state(struct tegra_pcie_dw *pcie,
					 bool enable)
{
	struct mrq_uphy_response resp;
	/* [한국어] 전송 구조체 */
	struct tegra_bpmp_message msg;
	/* [한국어] 요청 구조체. **이 타입의 정의는 이 트리에 없다** */
	struct mrq_uphy_request req;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 요청 구조체를 0 으로 민다 */
	memset(&req, 0, sizeof(req));
	/* [한국어] 응답 구조체를 0 으로 민다 */
	memset(&resp, 0, sizeof(resp));

	/* [한국어] **켤 때와 끌 때 명령 자체가 갈린다** -- 하나의 명령에 불리언을
	 * 실어 보내는 방식이 아니다 */
	if (enable) {
		/* [한국어] PLL 초기화 명령 */
		req.cmd = CMD_UPHY_PCIE_EP_CONTROLLER_PLL_INIT;
		/* [한국어] 켤 때의 인자 */
		req.ep_ctrlr_pll_init.ep_controller = pcie->cid;
	} else {
		/* [한국어] PLL 끄기 명령 */
		req.cmd = CMD_UPHY_PCIE_EP_CONTROLLER_PLL_OFF;
		/* [한국어] 끌 때의 인자. **공용체 필드가 켤 때와 다르다** */
		req.ep_ctrlr_pll_off.ep_controller = pcie->cid;
	}

	/* [한국어] 전송 구조체를 0 으로 민다 */
	memset(&msg, 0, sizeof(msg));
	/* [한국어] UPHY 관련 요청임을 알린다 */
	msg.mrq = MRQ_UPHY;
	/* [한국어] 요청 버퍼 */
	msg.tx.data = &req;
	/* [한국어] 요청 크기 */
	msg.tx.size = sizeof(req);
	/* [한국어] 응답 버퍼 */
	msg.rx.data = &resp;
	/* [한국어] 응답 버퍼 크기 */
	msg.rx.size = sizeof(resp);

	/* [한국어] BPMP 에 메시지를 보낸다. 응답을 기다리므로 잠들 수 있다 */
	err = tegra_bpmp_transfer(pcie->bpmp, &msg);
	/* [한국어] 전송 자체의 실패를 확인한다 */
	if (err)
		return err;
	/* [한국어] 펌웨어가 오류를 돌려줬는지 확인한다. **전송 성공과 별개다** */
	if (msg.rx.ret)
		return -EINVAL;

	return 0;
}

/* [한국어]
 * tegra_pcie_get_slot_regulators - 슬롯 전원 regulator 를 얻는다 (있으면)
 *
 * @pcie: 드라이버 상태.
 * @return: 0 성공. -ENODEV 가 아닌 오류면 그 값.
 *
 * 슬롯에 공급할 3.3V 와 12V regulator 를 얻는다. **둘 다 없어도 된다** --
 * 보드에 슬롯이 없거나 전원이 항상 켜져 있는 구성일 수 있기 때문이다.
 *
 * devm_regulator_get_optional 을 쓰면서도 -ENODEV 를 직접 걸러 NULL 로
 * 바꾸는 관용구를 쓴다. optional 판이 없을 때 -ENODEV 를 돌려주므로,
 * 그것만 "없음" 으로 해석하고 나머지 오류(예: -EPROBE_DEFER)는 그대로
 * 올린다.
 *
 * **앞 세대와의 대비**: pci-tegra.c 는 regulator 목록이 SoC 마다 달라
 * compatible 로 갈라 이름을 나열하고 bulk 로 얻었다. 여기서는 컨트롤러
 * 전원(vddio-pex-ctl)은 probe 가 직접 하나 얻고, 슬롯 전원만 이 함수가
 * 둘 얻는다. 훨씬 단순한데, 전원 관리의 상당 부분이 BPMP 펌웨어 쪽으로
 * 넘어갔기 때문이다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_dw_probe → [이 함수] → devm_regulator_get_optional
 */
static int tegra_pcie_get_slot_regulators(struct tegra_pcie_dw *pcie)
{
	pcie->slot_ctl_3v3 = devm_regulator_get_optional(pcie->dev, "vpcie3v3");
	/* [한국어] 오류 포인터를 확인한다 */
	if (IS_ERR(pcie->slot_ctl_3v3)) {
		/* [한국어] **optional 판이 없을 때 -ENODEV 를 돌려주므로 그것만 "없음"으로
		 * 해석한다** */
		if (PTR_ERR(pcie->slot_ctl_3v3) != -ENODEV)
			/* [한국어] 그 밖의 오류는 그대로 올린다 */
			return PTR_ERR(pcie->slot_ctl_3v3);

		/* [한국어] **없는 것은 오류가 아니다** -- NULL 로 두면 켜고 끄는 곳이 건너뛴다 */
		pcie->slot_ctl_3v3 = NULL;
	}

	/* [한국어] 12V 슬롯 전원을 얻는다 */
	pcie->slot_ctl_12v = devm_regulator_get_optional(pcie->dev, "vpcie12v");
	/* [한국어] 오류 포인터를 확인한다 */
	if (IS_ERR(pcie->slot_ctl_12v)) {
		/* [한국어] -ENODEV 인지 확인한다 */
		if (PTR_ERR(pcie->slot_ctl_12v) != -ENODEV)
			/* [한국어] 그 밖의 오류는 그대로 올린다 -- -EPROBE_DEFER 일 수 있다 */
			return PTR_ERR(pcie->slot_ctl_12v);

		/* [한국어] 없으면 NULL 로 두고 진행한다 */
		pcie->slot_ctl_12v = NULL;
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_enable_slot_regulators - 슬롯 전원을 켜고 규격 시간만큼 기다린다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 성공, 음수 오류.
 *
 * 3.3V 를 먼저 켜고 12V 를 나중에 켠다. 각각 없으면(NULL) 건너뛴다.
 *
 * **전원을 하나라도 켰으면 100ms 기다린다.** 위의 원문 주석이 근거를
 * 밝히는데, PCIe CEM 스펙 1.1 의 표 2.4 가 정한 T_PVPERL -- 전원이
 * 안정된 뒤 PERST 를 풀기까지의 최소 시간이다. 이 대기가 있어야 링크를
 * 세울 때 상대 장치가 준비되어 있다.
 *
 * 같은 상수를 앞 세대는 PCIE_T_PVPERL_MS(drivers/pci/pci.h:107)로 참조하고
 * pcie-mediatek-gen3.c 도 그렇게 쓰는데, **이 파일은 100 을 직접 쓴다.**
 * 값은 같다.
 *
 * 정리 경로가 하나뿐이다 -- 12V 가 실패하면 3.3V 를 되돌린다. 3.3V 가
 * 실패하면 켠 것이 없으므로 그대로 반환한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_config_controller → [이 함수] → regulator_enable, msleep
 */
static int tegra_pcie_enable_slot_regulators(struct tegra_pcie_dw *pcie)
{
	int ret;

	/* [한국어] 3.3V 가 있을 때만 */
	if (pcie->slot_ctl_3v3) {
		/* [한국어] 3.3V 를 먼저 켠다 */
		ret = regulator_enable(pcie->slot_ctl_3v3);
		/* [한국어] 3.3V 활성화 실패를 확인한다 */
		if (ret < 0) {
			dev_err(pcie->dev,
				"Failed to enable 3.3V slot supply: %d\n", ret);
			return ret;
		}
	}

	/* [한국어] 12V 가 있을 때만 */
	if (pcie->slot_ctl_12v) {
		/* [한국어] 12V 를 켠다. **3.3V 다음이다** */
		ret = regulator_enable(pcie->slot_ctl_12v);
		/* [한국어] 12V 활성화 실패를 확인한다 */
		if (ret < 0) {
			dev_err(pcie->dev,
				"Failed to enable 12V slot supply: %d\n", ret);
			goto fail_12v_enable;
		}
	}

	/*
	 * According to PCI Express Card Electromechanical Specification
	 * Revision 1.1, Table-2.4, T_PVPERL (Power stable to PERST# inactive)
	 * should be a minimum of 100ms.
	 */
	if (pcie->slot_ctl_3v3 || pcie->slot_ctl_12v)
		msleep(100);

	return 0;

fail_12v_enable:
	if (pcie->slot_ctl_3v3)
		regulator_disable(pcie->slot_ctl_3v3);
	return ret;
}

/* [한국어]
 * tegra_pcie_disable_slot_regulators - 슬롯 전원을 끈다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음. 실패를 보고하지 않는다.
 *
 * **켠 순서의 역순이다** -- 12V 를 먼저 끄고 3.3V 를 나중에 끈다.
 * 전원 시퀀싱에서 흔한 관례로, 높은 전압을 먼저 내려 장치가 불완전한
 * 전원 조합을 보는 시간을 줄인다.
 *
 * 각각 NULL 검사를 하므로 없는 전원은 건너뛴다.
 *
 * 켤 때와 달리 대기가 없다. 내리는 쪽은 시간 제약이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_config_controller(오류) / tegra_pcie_unconfig_controller
 *     → [이 함수] → regulator_disable
 */
static void tegra_pcie_disable_slot_regulators(struct tegra_pcie_dw *pcie)
{
	if (pcie->slot_ctl_12v)
		regulator_disable(pcie->slot_ctl_12v);
	/* [한국어] 3.3V 를 켰으면 되돌린다 */
	if (pcie->slot_ctl_3v3)
		regulator_disable(pcie->slot_ctl_3v3);
}

/* [한국어]
 * tegra_pcie_config_controller - 전원을 올리고 APPL 을 RC 모드로 설정한다
 *
 * @pcie:          드라이버 상태.
 * @en_hw_hot_rst: 하드웨어 핫 리셋 모드를 켤 것인가. probe 는 false,
 *   절전 복귀는 true 를 준다.
 * @return: 0 성공, 음수 오류.
 *
 * **RC 모드 하드웨어 기동의 전부가 이 함수에 있다.** 순서가 정해져 있다.
 *
 * 전원 올리기:
 *   1) BPMP 에 컨트롤러를 켜 달라고 요청한다. **가장 먼저다** --
 *      UPHY 가 살아나야 나머지가 의미를 갖는다.
 *   2) 외부 REFCLK 를 쓰면 UPHY PLL 도 BPMP 에 요청한다.
 *   3) 슬롯 전원을 켠다(100ms 대기 포함).
 *   4) 컨트롤러 전원(vddio-pex-ctl)을 켠다.
 *   5) 코어 클록을 켜고 APB 리셋을 푼다. **이때부터 APPL 레지스터에
 *      접근할 수 있다.**
 *
 * APPL 설정 (여기부터 레지스터를 만진다):
 *   6) 핫 리셋 모드를 켠다. en_hw_hot_rst 이거나 has_sbr_reset_fix 인
 *      SoC 일 때만이다.
 *   7) PHY 를 켠다.
 *   8) **DBI 블록의 물리 기준 주소를 APPL 에 알려 준다.** 이것이 접착
 *      계층의 전형적인 일이다 -- IP 코어가 어디에 놓여 있는지를 벤더
 *      로직에 가르쳐 준다.
 *   9) 이 코어를 RC 로 동작시키라고 지정한다.
 *  10) 클록 게이팅 우회를 끄고, 사전 감지 상태와 캐시 속성을 설정한다.
 *  11) SRNS 이거나 외부 REFCLK 를 쓰면 REFCLK 출력 패드를 막는다.
 *      위의 원문 주석대로, 외부 클록을 받아 쓰는 RC 는 같은 클록을
 *      하류에 공급할 수 없기 때문이다.
 *  12) CLKREQ 를 지원하지 않으면 관련 핀을 덮어쓴다.
 *  13) **iATU/DMA 블록의 기준 주소도 알려 준다.** 8)과 짝을 이루며,
 *      이 두 줄이 "주소 변환을 코어에 맡긴다" 는 사실을 드러낸다 --
 *      앞 세대라면 창을 직접 프로그래밍했을 자리다.
 *  14) 코어 리셋을 푼다. 이제 IP 가 동작할 수 있다.
 *
 * **정리 경로가 다섯 라벨** 로 층져 있고, 각 라벨이 그 직전까지 성공한
 * 것만 되돌린다. fail_pll_init 이 조건부로 PLL 을 끄는 것에 주의 --
 * 켜지 않았을 수도 있기 때문이다.
 *
 * 반환할 때 ret 을 쓰는데, 이 시점의 ret 은 마지막 성공한 호출의 0 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. BPMP 통신과 msleep 으로 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_init_controller / tegra_pcie_dw_resume_noirq → [이 함수]
 *     → tegra_pcie_bpmp_set_ctrl_state, _set_pll_state,
 *       tegra_pcie_enable_slot_regulators, tegra_pcie_enable_phy
 */
static int tegra_pcie_config_controller(struct tegra_pcie_dw *pcie,
					bool en_hw_hot_rst)
{
	int ret;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* [한국어] **BPMP 에 컨트롤러를 켜 달라고 요청한다. 가장 먼저다** --
	 * UPHY 가 살아나야 나머지가 의미를 갖는다 */
	ret = tegra_pcie_bpmp_set_ctrl_state(pcie, true);
	/* [한국어] 컨트롤러 활성화 실패 */
	if (ret) {
		dev_err(pcie->dev,
			"Failed to enable controller %u: %d\n", pcie->cid, ret);
		return ret;
	}

	/* [한국어] 외부 참조 클록을 쓸 때만 PLL 을 따로 켠다 -- 자기 클록을 쓰면
	 * 필요 없기 때문이다 */
	if (pcie->enable_ext_refclk) {
		/* [한국어] UPHY PLL 도 BPMP 에 요청한다 */
		ret = tegra_pcie_bpmp_set_pll_state(pcie, true);
		/* [한국어] 실패를 확인한다 */
		if (ret) {
			/* [한국어] UPHY 초기화 실패 */
			dev_err(pcie->dev, "Failed to init UPHY: %d\n", ret);
			goto fail_pll_init;
		}
	}

	/* [한국어] 슬롯 전원을 켠다. 그 안에서 100ms 를 기다린다 */
	ret = tegra_pcie_enable_slot_regulators(pcie);
	/* [한국어] 슬롯 전원 실패면 UPHY 부터 되감는다 */
	if (ret < 0)
		goto fail_slot_reg_en;

	/* [한국어] 컨트롤러 IO 제어 전원을 켠다 */
	ret = regulator_enable(pcie->pex_ctl_supply);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] regulator 활성화 실패 */
		dev_err(pcie->dev, "Failed to enable regulator: %d\n", ret);
		goto fail_reg_en;
	}

	/* [한국어] 코어 클록을 켠다 */
	ret = clk_prepare_enable(pcie->core_clk);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] 코어 클록 활성화 실패 */
		dev_err(pcie->dev, "Failed to enable core clock: %d\n", ret);
		goto fail_core_clk;
	}

	/* [한국어] **APB 리셋을 푼다. 이 아래부터 APPL 레지스터에 접근할 수 있다** --
	 * 이 줄이 전원 올리기와 APPL 설정을 가르는 경계다 */
	ret = reset_control_deassert(pcie->core_apb_rst);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] APB 리셋 해제 실패 */
		dev_err(pcie->dev, "Failed to deassert core APB reset: %d\n",
			ret);
		goto fail_core_apb_rst;
	}

	if (en_hw_hot_rst || pcie->of_data->has_sbr_reset_fix) {
		/* Enable HW_HOT_RST mode */
		val = appl_readl(pcie, APPL_CTRL);
		/* [한국어] 모드 필드를 지운다 */
		val &= ~(APPL_CTRL_HW_HOT_RST_MODE_MASK <<
			 APPL_CTRL_HW_HOT_RST_MODE_SHIFT);
		/* [한국어] 즉시 리셋 후 LTSSM 재활성화 모드를 넣는다 */
		val |= (APPL_CTRL_HW_HOT_RST_MODE_IMDT_RST_LTSSM_EN <<
			APPL_CTRL_HW_HOT_RST_MODE_SHIFT);
		/* [한국어] 핫 리셋을 활성화한다 */
		val |= APPL_CTRL_HW_HOT_RST_EN;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_CTRL);
	}

	/* [한국어] PHY 를 켠다. **RC 모드라 보정은 하지 않는다** */
	ret = tegra_pcie_enable_phy(pcie);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] PHY 활성화 실패 */
		dev_err(pcie->dev, "Failed to enable PHY: %d\n", ret);
		goto fail_phy;
	}

	/* Update CFG base address */
	appl_writel(pcie, pcie->dbi_res->start & APPL_CFG_BASE_ADDR_MASK,
		    APPL_CFG_BASE_ADDR);

	/* Configure this core for RP mode operation */
	appl_writel(pcie, APPL_DM_TYPE_RP, APPL_DM_TYPE);

	/* [한국어] 클록 게이팅 우회를 끈다 -- 하드웨어 기본 동작을 그대로 쓴다 */
	appl_writel(pcie, 0x0, APPL_CFG_SLCG_OVERRIDE);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_CTRL);
	/* [한국어] 사전 감지 상태를 켠다. **읽은 값에 OR 해 한 줄로 쓴다** --
	 * EP 경로가 여러 줄로 나누는 것과 형태가 다르다 */
	appl_writel(pcie, val | APPL_CTRL_SYS_PRE_DET_STATE, APPL_CTRL);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_CFG_MISC);
	/* [한국어] AXI 읽기 캐시 속성을 넣는다. **필드를 지우지 않고 OR 로만 넣는다** --
	 * 그래서 ARCACHE_MASK 상수가 정의만 있고 쓰이지 않는다 */
	val |= (APPL_CFG_MISC_ARCACHE_VAL << APPL_CFG_MISC_ARCACHE_SHIFT);
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_CFG_MISC);

	if (pcie->enable_srns || pcie->enable_ext_refclk) {
		/*
		 * When Tegra PCIe RP is using external clock, it cannot supply
		 * same clock to its downstream hierarchy. Hence, gate PCIe RP
		 * REFCLK out pads when RP & EP are using separate clocks or RP
		 * is using an external REFCLK.
		 */
		val = appl_readl(pcie, APPL_PINMUX);
		/* [한국어] REFCLK 출력 덮어쓰기를 켠다 */
		val |= APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE_EN;
		/* [한국어] **출력 값을 0 으로 둔다** -- EP 경로가 1 로 두는 것과 반대 방향이다 */
		val &= ~APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_PINMUX);
	}

	/* [한국어] **CLKREQ 를 지원하지 않는 보드에서는 신호를 고정한다** */
	if (!pcie->supports_clkreq) {
		/* [한국어] 현재 값을 읽는다 */
		val = appl_readl(pcie, APPL_PINMUX);
		/* [한국어] CLKREQ 덮어쓰기를 켠다 */
		val |= APPL_PINMUX_CLKREQ_OVERRIDE_EN;
		/* [한국어] 덮어쓸 값을 0 으로 둔다 */
		val &= ~APPL_PINMUX_CLKREQ_OVERRIDE;
		/* [한국어] 기본값 비트도 지운다 */
		val &= ~APPL_PINMUX_CLKREQ_DEFAULT_VALUE;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_PINMUX);
	}

	/* Update iATU_DMA base address */
	appl_writel(pcie,
		    pcie->atu_dma_res->start & APPL_CFG_IATU_DMA_BASE_ADDR_MASK,
		    APPL_CFG_IATU_DMA_BASE_ADDR);

	reset_control_deassert(pcie->core_rst);

	return ret;

fail_phy:
	reset_control_assert(pcie->core_apb_rst);
fail_core_apb_rst:
	clk_disable_unprepare(pcie->core_clk);
fail_core_clk:
	regulator_disable(pcie->pex_ctl_supply);
fail_reg_en:
	tegra_pcie_disable_slot_regulators(pcie);
fail_slot_reg_en:
	if (pcie->enable_ext_refclk)
		/* [한국어] 켰던 경우에만 UPHY PLL 을 끈다 */
		tegra_pcie_bpmp_set_pll_state(pcie, false);
fail_pll_init:
	tegra_pcie_bpmp_set_ctrl_state(pcie, false);

	return ret;
}

/* [한국어]
 * tegra_pcie_unconfig_controller - 전원을 내리고 컨트롤러를 끈다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음. 실패는 모두 로그만 찍는다.
 *
 * tegra_pcie_config_controller 의 역순이다 -- 코어 리셋, PHY, APB 리셋,
 * 클록, 컨트롤러 전원, 슬롯 전원, UPHY PLL, BPMP 컨트롤러 순이다.
 *
 * **모든 실패를 로그만 찍고 계속 진행한다.** 되돌리는 경로에서 하나가
 * 실패했다고 나머지를 포기하면 자원이 더 많이 새기 때문이다. 켜는 쪽이
 * goto 로 즉시 중단하는 것과 대비된다.
 *
 * APPL 레지스터를 되돌리지 않는 데 주의한다. 코어 리셋과 전원 차단으로
 * 어차피 초기화되므로, 다시 켤 때 tegra_pcie_config_controller 가 전부
 * 다시 쓴다.
 *
 * BPMP 요청이 가장 마지막인 것은 켤 때 가장 먼저였던 것의 대칭이다.
 *
 * 불리는 곳이 넷이다 -- 호스트 초기화 실패, 정상 해제, 절전 진입,
 * 그리고 복귀 실패다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. BPMP 통신으로 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_pcie_init_controller(오류) / tegra_pcie_deinit_controller /
 *   tegra_pcie_dw_suspend_noirq / _resume_noirq(오류) / _shutdown → [이 함수]
 */
static void tegra_pcie_unconfig_controller(struct tegra_pcie_dw *pcie)
{
	int ret;

	/* [한국어] 코어 리셋을 건다. 켤 때 가장 마지막에 풀었던 것의 대칭이다 */
	ret = reset_control_assert(pcie->core_rst);
	/* [한국어] **실패해도 계속 진행한다.** 되돌리는 경로에서 하나가 실패했다고
	 * 나머지를 포기하면 자원이 더 많이 새기 때문이며, 켜는 쪽이 goto 로
	 * 즉시 중단하는 것과 대비된다 */
	if (ret)
		/* [한국어] 코어 리셋 실패 */
		dev_err(pcie->dev, "Failed to assert \"core\" reset: %d\n", ret);

	tegra_pcie_disable_phy(pcie);

	/* [한국어] APB 리셋을 건다. 이제 APPL 레지스터에 접근할 수 없다 */
	ret = reset_control_assert(pcie->core_apb_rst);
	/* [한국어] 실패를 확인하되 멈추지 않는다 */
	if (ret)
		/* [한국어] APB 리셋 실패 */
		dev_err(pcie->dev, "Failed to assert APB reset: %d\n", ret);

	clk_disable_unprepare(pcie->core_clk);

	/* [한국어] 컨트롤러 전원을 끈다 */
	ret = regulator_disable(pcie->pex_ctl_supply);
	/* [한국어] 실패를 확인하되 멈추지 않는다 */
	if (ret)
		/* [한국어] regulator 차단 실패 */
		dev_err(pcie->dev, "Failed to disable regulator: %d\n", ret);

	tegra_pcie_disable_slot_regulators(pcie);

	/* [한국어] 켤 때 켰던 경우에만 끈다 */
	if (pcie->enable_ext_refclk) {
		/* [한국어] UPHY PLL 을 끈다 */
		ret = tegra_pcie_bpmp_set_pll_state(pcie, false);
		/* [한국어] 실패를 확인하되 멈추지 않는다 */
		if (ret)
			/* [한국어] UPHY 해제 실패 */
			dev_err(pcie->dev, "Failed to deinit UPHY: %d\n", ret);
	}

	/* [한국어] **BPMP 에 컨트롤러를 꺼 달라고 요청한다.** 켤 때 가장 먼저였던 것의
	 * 대칭으로 가장 마지막이다 */
	ret = tegra_pcie_bpmp_set_ctrl_state(pcie, false);
	/* [한국어] 실패를 확인하되 멈추지 않는다 */
	if (ret)
		/* [한국어] 컨트롤러 비활성화 실패 */
		dev_err(pcie->dev, "Failed to disable controller %d: %d\n",
			pcie->cid, ret);
}

/* [한국어]
 * tegra_pcie_init_controller - 하드웨어를 켜고 DWC 코어에 넘긴다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 성공, 음수 오류.
 *
 * 두 단계뿐인 짧은 함수지만 **접착 계층의 경계가 여기 있다.**
 *
 *   1) tegra_pcie_config_controller 로 하드웨어를 켠다. 여기까지가
 *      이 드라이버의 영역이다.
 *   2) pp->ops 에 이 파일의 host_ops 를 꽂고 dw_pcie_host_init 을 부른다.
 *      **여기서 DWC 코어로 제어가 넘어간다.** 그 함수 안에서 코어가
 *      iATU 창을 설정하고, MSI 도메인을 만들고, INTx 도메인을 만들고,
 *      루트 포트 config 를 설정하고, 버스를 스캔한다. 그리고 그 도중에
 *      이 파일의 tegra_pcie_dw_host_init 과 tegra_pcie_dw_start_link 를
 *      콜백으로 되부른다.
 *
 * 앞 세대 pci-tegra.c 에서 이 자리를 tegra_pcie_pm_resume 이 맡았는데,
 * 그쪽은 변환 창부터 MSI 활성화까지 아홉 단계를 손으로 나열했다.
 * 여기서는 그 대부분이 dw_pcie_host_init 한 줄 뒤로 사라진다.
 *
 * 정리 경로가 하나뿐이다 -- 코어 초기화가 실패하면 하드웨어를 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_config_rp → [이 함수]
 *     → tegra_pcie_config_controller, dw_pcie_host_init
 */
static int tegra_pcie_init_controller(struct tegra_pcie_dw *pcie)
{
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 코어의 루트 포트 구조체 주소를 잡는다 */
	struct dw_pcie_rp *pp = &pci->pp;
	/* [한국어] 오류 코드 보관용 */
	int ret;

	/* [한국어] **여기까지가 이 드라이버의 영역이다** -- 하드웨어를 켠다 */
	ret = tegra_pcie_config_controller(pcie, false);
	if (ret < 0)
		return ret;

	/* [한국어] **host_ops 를 꽂는다.** init 하나만 채운 구조체이며, 특히
	 * msi_init 을 채우지 않아 코어가 자기 iMSI-RX 경로를 그대로 쓴다 */
	pp->ops = &tegra_pcie_dw_host_ops;

	/* [한국어] **여기서 DWC 코어로 제어가 넘어간다.** 이 함수 안에서 코어가
	 * iATU 창을 설정하고, MSI 도메인을 만들고, INTx 도메인을 만들고,
	 * 루트 포트 config 를 설정하고 버스를 스캔한다. 그리고 그 도중에
	 * 이 파일의 host_init 과 start_link 를 콜백으로 되부른다.
	 * 앞 세대 pci-tegra.c 의 tegra_pcie_pm_resume 이 아홉 단계를 손으로
	 * 나열하던 자리가 이 한 줄로 줄었다 */
	ret = dw_pcie_host_init(pp);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 코어 초기화 실패 */
		dev_err(pcie->dev, "Failed to add PCIe port: %d\n", ret);
		goto fail_host_init;
	}

	return 0;

fail_host_init:
	tegra_pcie_unconfig_controller(pcie);
	return ret;
}

/* [한국어]
 * tegra_pcie_try_link_l2 - 링크를 L2 로 내리고 결과를 기다린다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 이면 L2 진입 성공. 링크가 없으면 0. 시간 초과면 -ETIMEDOUT.
 *
 * 절전에 들어가기 전에 링크를 규격에 맞게 내린다. 전원을 그냥 끊으면
 * 상대가 링크 단절을 오류로 볼 수 있으므로, PM_Enter_L23 흐름을 거친다.
 *
 * **링크가 이미 없으면 아무것도 하지 않고 성공을 돌려준다.** 내릴 링크가
 * 없으니 목적이 이미 달성된 셈이다.
 *
 * APPL_RADM_STATUS 의 turnoff 비트를 세우면 하드웨어가 흐름을 시작하고,
 * 소프트웨어는 APPL_DEBUG 의 L2 대기 비트가 서기를 폴링한다.
 * 간격이 상한의 1/10 인데, 이 관용구는 drivers/pci/controller/dwc 의
 * 다른 드라이버(pci-imx6.c:1384)에도 나타난다.
 *
 * **PCIE_PME_TO_L2_TIMEOUT_US 의 정의는 이 스파스 체크아웃에 없다.**
 * pcie-designware-host.c:2819 도 같은 상수를 쓰는 것으로 보아 DWC 공통
 * 헤더에 있을 것이나, 값은 확인하지 못했다.
 *
 * 앞 세대 pci-tegra.c 의 tegra_pcie_pme_turnoff 가 같은 자리를 맡았는데,
 * 그쪽은 AFI 레지스터의 포트별 비트를 쓰고 응답 비트를 기다렸다.
 * 포트가 여럿이라 포트마다 불러야 했던 것과 달리, 여기서는 컨트롤러
 * 인스턴스마다 링크가 하나라 한 번이면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(절전 경로).
 *
 * 호출 체인:
 *   tegra_pcie_dw_pme_turnoff → [이 함수] → readl_poll_timeout
 */
static int tegra_pcie_try_link_l2(struct tegra_pcie_dw *pcie)
{
	u32 val;

	/* [한국어] **링크가 없으면 성공으로 돌아간다** -- 내릴 링크가 없으니 목적이
	 * 이미 달성된 셈이다 */
	if (!tegra_pcie_dw_link_up(&pcie->pci))
		return 0;

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_RADM_STATUS);
	/* [한국어] PME_Turn_Off 전송 비트를 세운다 */
	val |= APPL_PM_XMT_TURNOFF_STATE;
	/* [한국어] 요청을 내보낸다. 하드웨어가 PM_Enter_L23 흐름을 시작한다 */
	appl_writel(pcie, val, APPL_RADM_STATUS);

	/* [한국어] L2 진입 완료 비트가 서기를 폴링한다. **간격이 상한의 1/10 인데**
	 * 이 관용구는 같은 디렉터리의 pci-imx6.c:1384 에도 나타난다.
	 * PCIE_PME_TO_L2_TIMEOUT_US 의 정의는 이 트리에서 찾지 못했다 */
	return readl_poll_timeout(pcie->appl_base + APPL_DEBUG, val,
				  val & APPL_DEBUG_PM_LINKST_IN_L2_LAT,
				  PCIE_PME_TO_L2_TIMEOUT_US/10,
				  PCIE_PME_TO_L2_TIMEOUT_US);
}

/* [한국어]
 * tegra_pcie_dw_pme_turnoff - 링크를 내리고 클록을 끊는다. 실패하면 강제로 detect 로 보낸다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음.
 *
 * 절전 전 링크 정리의 전체 절차다. 앞 세대보다 방어가 두껍다.
 *
 *   1) **링크가 없으면 곧바로 돌아간다.**
 *   2) **모든 인터럽트를 끈다.** 위의 원문 주석이 이유를 자세히 밝히는데,
 *      L2 진입에 실패하면 PERST 를 걸게 되고 그것이 갑작스러운 링크 단절
 *      AER 을 유발할 수 있다. 그런데 이 함수는 suspend_noirq 에서 불려
 *      AER 인터럽트가 처리되지 않으므로, 아예 발생하지 않게 막는다.
 *   3) L2 진입을 시도한다.
 *   4) **실패하면 PERST 를 걸어 강제로 detect 상태로 보낸다.**
 *      위의 원문 주석대로 TX 레인 클록 주파수는 링크가 L2 나 detect 일
 *      때만 Gen1 으로 되돌아가는데, 그래야 다음에 깨어날 때 정상적으로
 *      협상할 수 있기 때문이다.
 *      LTSSM 상태가 네 detect 계열 값 중 하나가 되기를 폴링하고,
 *      시간이 초과되어도 로그만 찍고 진행한다.
 *   5) LTSSM 을 끈다. 원문 주석대로 상태가 Polling 과 Detect 사이를
 *      오가는 것을 막기 위해서다.
 *   6) CLKREQ 를 덮어쓰고 슬롯으로 나가는 REFCLK 를 끊는다.
 *      **이 뒤로는 DBI 레지스터에 접근하지 못할 수 있다** -- 원문 주석이
 *      그 사실을 경고하는데, 엔드포인트가 CLKREQ 를 어떻게 당기느냐에
 *      따라 PLLE 가 내려가기 때문이다.
 *
 * 5)에서 appl_readl / appl_writel 대신 raw readl / writel 을 쓰는 데
 * 주의한다. 같은 주소를 같은 방식으로 접근하지만 도우미를 거치지 않으며,
 * 그 이유는 코드에 적혀 있지 않다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 주로 noirq 단계.
 *
 * 호출 체인:
 *   tegra_pcie_deinit_controller / tegra_pcie_dw_suspend_noirq /
 *   _shutdown → [이 함수] → tegra_pcie_dw_link_up, tegra_pcie_try_link_l2
 */
static void tegra_pcie_dw_pme_turnoff(struct tegra_pcie_dw *pcie)
{
	u32 data;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 링크 유무를 먼저 확인한다 */
	if (!tegra_pcie_dw_link_up(&pcie->pci)) {
		/* [한국어] 링크가 없으면 내릴 것도 없다 */
		dev_dbg(pcie->dev, "PCIe link is not up...!\n");
		return;
	}

	/*
	 * PCIe controller exits from L2 only if reset is applied, so
	 * controller doesn't handle interrupts. But in cases where
	 * L2 entry fails, PERST# is asserted which can trigger surprise
	 * link down AER. However this function call happens in
	 * suspend_noirq(), so AER interrupt will not be processed.
	 * Disable all interrupts to avoid such a scenario.
	 */
	appl_writel(pcie, 0x0, APPL_INTR_EN_L0_0);

	/* [한국어] **L2 진입에 실패했을 때의 대비책이다.** 위의 원문 주석대로 TX 레인
	 * 클록 주파수는 링크가 L2 나 detect 일 때만 Gen1 으로 되돌아가므로,
	 * 강제로 detect 로 보내야 다음에 깨어날 때 정상적으로 협상할 수 있다 */
	if (tegra_pcie_try_link_l2(pcie)) {
		dev_info(pcie->dev, "Link didn't transition to L2 state\n");
		/*
		 * TX lane clock freq will reset to Gen1 only if link is in L2
		 * or detect state.
		 * So apply pex_rst to end point to force RP to go into detect
		 * state
		 */
		data = appl_readl(pcie, APPL_PINMUX);
		/* [한국어] PERST 비트를 지운다 -- **0이 어서트다** */
		data &= ~APPL_PINMUX_PEX_RST;
		/* [한국어] PERST 를 건다 */
		appl_writel(pcie, data, APPL_PINMUX);

		/* [한국어] LTSSM 이 detect 계열 네 상태 중 하나가 되기를 기다린다.
		 * **EP 해제 경로가 L2_IDLE 도 허용하는 것과 달리 여기는 네 가지뿐이다** */
		err = readl_poll_timeout(pcie->appl_base + APPL_DEBUG, data,
			((data & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_DETECT_QUIET) ||
			((data & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_DETECT_ACT) ||
			((data & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_PRE_DETECT_QUIET) ||
			((data & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_DETECT_WAIT),
			LTSSM_DELAY_US, LTSSM_TIMEOUT_US);
		/* [한국어] 시간 초과를 확인한다 */
		if (err)
			/* [한국어] 시간이 초과되어도 로그만 찍고 진행한다 */
			dev_info(pcie->dev, "LTSSM state: 0x%x detect timeout: %d\n", data, err);

		/*
		 * Deassert LTSSM state to stop the state toggling between
		 * Polling and Detect.
		 */
		data = readl(pcie->appl_base + APPL_CTRL);
		/* [한국어] LTSSM 을 끈다. 원문 주석대로 Polling 과 Detect 사이를 오가는 것을 막는다 */
		data &= ~APPL_CTRL_LTSSM_EN;
		/* [한국어] **raw writel 을 쓴다** -- appl_writel 도우미를 거치지 않는다.
		 * 같은 주소를 같은 방식으로 접근하지만 형태가 다르며, 그 이유는
		 * 코드에 적혀 있지 않다 */
		writel(data, pcie->appl_base + APPL_CTRL);
	}
	/*
	 * DBI registers may not be accessible after this as PLL-E would be
	 * down depending on how CLKREQ is pulled by end point
	 */
	data = appl_readl(pcie, APPL_PINMUX);
	data |= (APPL_PINMUX_CLKREQ_OVERRIDE_EN | APPL_PINMUX_CLKREQ_OVERRIDE);
	/* Cut REFCLK to slot */
	data |= APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE_EN;
	/* [한국어] **슬롯으로 나가는 REFCLK 를 끊는다.** 위의 원문 주석이 경고하듯,
	 * 이 뒤로는 엔드포인트가 CLKREQ 를 어떻게 당기느냐에 따라 PLLE 가
	 * 내려가 DBI 레지스터에 접근하지 못할 수 있다 */
	data &= ~APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE;
	appl_writel(pcie, data, APPL_PINMUX);
}

/* [한국어]
 * tegra_pcie_deinit_controller - 코어와 하드웨어를 순서대로 내린다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음.
 *
 * 네 줄로 해제 순서를 정한다.
 *   1) 모니터 클록을 끈다. tegra_pcie_dw_host_init 이 켠 것의 짝이다.
 *   2) DWC 코어의 호스트 부분을 해제한다. 버스와 도메인이 여기서 사라진다.
 *   3) 링크를 L2 로 내린다.
 *   4) 하드웨어 전원을 내린다.
 *
 * **2)가 3)보다 먼저인 것이 중요하다.** 링크를 내리기 전에 소프트웨어
 * 쪽 구조를 먼저 정리해, 링크가 사라진 뒤 코어가 그것을 참조하지 않게
 * 한다.
 *
 * tegra_pcie_dw_suspend_noirq 는 이 함수를 부르지 않고 같은 네 단계 중
 * 1), 3), 4)만 직접 수행한다 -- 절전에서는 DWC 코어의 호스트 구조를
 * 유지해야 깨어날 때 다시 만들 필요가 없기 때문이다. 그 차이가 해제와
 * 절전을 가르는 지점이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_config_rp(오류) / tegra_pcie_dw_remove → [이 함수]
 *     → dw_pcie_host_deinit, tegra_pcie_dw_pme_turnoff,
 *       tegra_pcie_unconfig_controller
 */
static void tegra_pcie_deinit_controller(struct tegra_pcie_dw *pcie)
{
	clk_disable_unprepare(pcie->core_clk_m);
	dw_pcie_host_deinit(&pcie->pci.pp);
	tegra_pcie_dw_pme_turnoff(pcie);
	tegra_pcie_unconfig_controller(pcie);
}

/* [한국어]
 * tegra_pcie_config_rp - 루트 포트 모드 전체를 기동한다
 *
 * @pcie: 드라이버 상태.
 * @return: 0 성공. 링크가 없으면 -ENOMEDIUM. 그 밖의 실패는 음수.
 *
 * RC 경로의 최상위 함수다.
 *
 *   1) runtime PM 을 켜고 동기 get 을 한다. **앞 세대와 달리 이것이
 *      하드웨어를 기동하지 않는다** -- pci-tegra.c 는 이 호출이
 *      tegra_pcie_pm_resume 을 불러 모든 것을 했지만, 이 드라이버는
 *      runtime PM 콜백을 등록하지 않아 전원 도메인 확보의 의미만 갖는다.
 *   2) 핀을 기본 상태로 돌린다.
 *   3) 하드웨어를 켜고 DWC 코어에 넘긴다.
 *   4) **링크가 실제로 섰는지 확인한다.** 없으면 -ENOMEDIUM 을 돌려준다.
 *   5) debugfs 를 만든다.
 *
 * **-ENOMEDIUM 의 처리가 특이하다.** probe 가 그 값을 오류로 보지 않고
 * 성공으로 처리한다(tegra_pcie_dw_probe 의 `ret && ret != -ENOMEDIUM`).
 * 즉 링크가 없어도 드라이버는 붙은 채로 남는다. 다만 link_state 가
 * false 이므로 remove 와 shutdown 이 곧바로 돌아가고, 절전 콜백들도
 * 아무것도 하지 않는다. **link_state 하나가 이후 모든 경로의 스위치가
 * 되는 셈이다.**
 *
 * 4)에서 실패하면 fail_host_init 으로 가는데, 그 라벨이
 * tegra_pcie_deinit_controller 를 부르므로 링크가 없어도 코어와 하드웨어가
 * 정리된다. 그 뒤 pm_runtime 까지 되돌린다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_dw_probe → [이 함수]
 *     → tegra_pcie_init_controller, tegra_pcie_dw_link_up, init_debugfs
 */
static int tegra_pcie_config_rp(struct tegra_pcie_dw *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 오류 코드 보관용 */
	int ret;

	pm_runtime_enable(dev);

	/* [한국어] **앞 세대와 달리 이 호출이 하드웨어를 기동하지 않는다.**
	 * pci-tegra.c 는 이것이 tegra_pcie_pm_resume 을 불러 모든 것을 했지만,
	 * 이 드라이버는 runtime PM 콜백을 등록하지 않아 전원 도메인 확보의
	 * 의미만 갖는다 */
	ret = pm_runtime_get_sync(dev);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] runtime PM 획득 실패 */
		dev_err(dev, "Failed to get runtime sync for PCIe dev: %d\n",
			ret);
		goto fail_pm_get_sync;
	}

	/* [한국어] 측대역 핀을 기본 상태로 설정한다 */
	ret = pinctrl_pm_select_default_state(dev);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 핀 설정 실패 */
		dev_err(dev, "Failed to configure sideband pins: %d\n", ret);
		goto fail_pm_get_sync;
	}

	/* [한국어] **하드웨어를 켜고 DWC 코어에 넘긴다.** 여기서 제어가 코어로 갔다가
	 * 콜백으로 되돌아온다 */
	ret = tegra_pcie_init_controller(pcie);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 초기화 실패 */
		dev_err(dev, "Failed to initialize controller: %d\n", ret);
		goto fail_pm_get_sync;
	}

	/* [한국어] **링크 상태를 기록한다. 이 값이 이후 여섯 함수의 스위치가 된다** --
	 * remove, suspend_late, suspend_noirq, resume_noirq, resume_early,
	 * shutdown 이 모두 이것이 거짓이면 곧바로 돌아간다 */
	pcie->link_state = tegra_pcie_dw_link_up(&pcie->pci);
	/* [한국어] **링크가 없으면 -ENOMEDIUM 으로 간다.** 다만 probe 가 그 값을 오류로
	 * 보지 않으므로 드라이버는 붙은 채로 남는다 */
	if (!pcie->link_state) {
		ret = -ENOMEDIUM;
		goto fail_host_init;
	}

	init_debugfs(pcie);

	return ret;

fail_host_init:
	tegra_pcie_deinit_controller(pcie);
fail_pm_get_sync:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	return ret;
}

/* [한국어]
 * pex_ep_event_pex_rst_assert - 호스트가 PERST 를 걸면 엔드포인트를 허문다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음. 실패는 로그만 찍는다.
 *
 * **엔드포인트 모드의 절반을 이루는 함수** 다. 호스트가 리셋을 걸거나
 * 전원을 내리면 PERST GPIO 가 눌리고, 그 인터럽트가 이 함수로 온다.
 *
 * 이미 꺼져 있으면(EP_STATE_DISABLED) 곧바로 돌아간다 -- PERST 가
 * 여러 번 흔들릴 수 있기 때문이다.
 *
 *   1) LTSSM 이 detect 계열이나 L2.idle 로 갈 때까지 폴링한다.
 *      **다섯 상태 중 하나면 된다.** 링크가 이미 내려갔음을 확인하는
 *      것이며, 시간이 초과되어도 로그만 찍고 진행한다.
 *   2) LTSSM 을 끈다. 원문 주석대로 Polling 과 Detect 사이를 오가는 것을
 *      막기 위해서다.
 *   3) 코어 리셋을 걸고, PHY 를 끄고, APB 리셋을 걸고, 클록을 끈다.
 *   4) runtime PM 을 놓는다.
 *   5) 외부 REFCLK 를 쓰면 UPHY PLL 을 BPMP 로 끈다.
 *   6) BPMP 로 컨트롤러를 끈다.
 *   7) 상태를 DISABLED 로 표시한다.
 *
 * **이 순서가 pex_ep_event_pex_rst_deassert 의 정확한 역순** 이다.
 * 두 함수가 짝을 이루며 엔드포인트를 켜고 끈다.
 *
 * tegra_pcie_unconfig_controller 와 하는 일이 겹치지만 별도로 존재하는데,
 * EP 경로는 슬롯 regulator 와 컨트롤러 regulator 를 다루지 않고
 * runtime PM 을 직접 놓는 점이 다르다.
 *
 * remove 와 shutdown 도 이 함수를 부른다 -- 그때는 PERST 가 실제로
 * 눌리지 않았지만 같은 정리가 필요하기 때문이다.
 *
 * 실행 컨텍스트: 스레드 인터럽트 컨텍스트, 또는 프로세스 컨텍스트
 * (remove/shutdown 경로). 폴링과 BPMP 통신으로 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_pcie_ep_pex_rst_irq / tegra_pcie_dw_remove / _shutdown
 *     → [이 함수] → tegra_pcie_disable_phy, tegra_pcie_bpmp_set_ 계열
 */
static void pex_ep_event_pex_rst_assert(struct tegra_pcie_dw *pcie)
{
	u32 val;
	/* [한국어] 오류 코드 보관용 */
	int ret;

	/* [한국어] **이미 꺼져 있으면 곧바로 돌아간다.** PERST 가 여러 번 흔들릴 수 있다 */
	if (pcie->ep_state == EP_STATE_DISABLED)
		return;

	/* [한국어] **LTSSM 이 다섯 상태 중 하나가 되기를 기다린다** -- detect 계열 넷과
	 * L2.idle 이다. 링크가 확실히 내려갔음을 확인하는 것이며,
	 * 이 상수들은 이미 시프트된 형태라 마스크만 한 값과 직접 비교한다 */
	ret = readl_poll_timeout(pcie->appl_base + APPL_DEBUG, val,
		((val & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_DETECT_QUIET) ||
		((val & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_DETECT_ACT) ||
		((val & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_PRE_DETECT_QUIET) ||
		((val & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_DETECT_WAIT) ||
		((val & APPL_DEBUG_LTSSM_STATE_MASK) == LTSSM_STATE_L2_IDLE),
		LTSSM_DELAY_US, LTSSM_TIMEOUT_US);
	/* [한국어] 시간 초과를 확인한다 */
	if (ret)
		/* [한국어] 시간이 초과되어도 **로그만 찍고 진행한다** -- 어차피 내리는 중이므로
		 * 멈출 이유가 없다 */
		dev_info(pcie->dev, "LTSSM state: 0x%x detect timeout: %d\n", val, ret);

	/*
	 * Deassert LTSSM state to stop the state toggling between
	 * Polling and Detect.
	 */
	val = appl_readl(pcie, APPL_CTRL);
	/* [한국어] **LTSSM 을 끈다.** 위의 원문 주석대로 상태가 Polling 과 Detect 사이를
	 * 오가는 것을 막기 위해서다 */
	val &= ~APPL_CTRL_LTSSM_EN;
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_CTRL);

	reset_control_assert(pcie->core_rst);

	tegra_pcie_disable_phy(pcie);

	reset_control_assert(pcie->core_apb_rst);

	clk_disable_unprepare(pcie->core_clk);

	pm_runtime_put_sync(pcie->dev);

	/* [한국어] 켤 때 켰던 경우에만 끈다 */
	if (pcie->enable_ext_refclk) {
		/* [한국어] UPHY PLL 을 BPMP 로 끈다 */
		ret = tegra_pcie_bpmp_set_pll_state(pcie, false);
		/* [한국어] 실패를 확인하되 멈추지 않는다 */
		if (ret)
			/* [한국어] UPHY 차단 실패 */
			dev_err(pcie->dev, "Failed to turn off UPHY: %d\n",
				ret);
	}

	/* [한국어] **BPMP 에 컨트롤러를 꺼 달라고 요청한다.** 켤 때 가장 먼저였던 것의
	 * 대칭으로 가장 마지막이다 */
	ret = tegra_pcie_bpmp_set_ctrl_state(pcie, false);
	/* [한국어] 실패를 확인하되 멈추지 않는다 */
	if (ret)
		/* [한국어] 컨트롤러 비활성화 실패. 되돌리는 경로라 로그만 찍는다 */
		dev_err(pcie->dev, "Failed to disable controller: %d\n", ret);

	/* [한국어] 상태를 꺼짐으로 표시한다. 이제 deassert 경로가 실제로 초기화를 수행한다 */
	pcie->ep_state = EP_STATE_DISABLED;
	dev_dbg(pcie->dev, "Uninitialization of endpoint is completed\n");
}

/* [한국어]
 * pex_ep_event_pex_rst_deassert - 호스트가 PERST 를 풀면 엔드포인트를 세운다
 *
 * @pcie: 드라이버 상태.
 * @return: 없음. 실패는 정리 후 조용히 돌아간다.
 *
 * **엔드포인트 모드의 실질적인 초기화 전체가 여기 있다.** RC 모드라면
 * probe 가 할 일을, EP 모드는 호스트가 PERST 를 풀어 줄 때까지 미뤘다가
 * 이 함수에서 한다. 그것이 EP 모드의 근본적인 성격이다 -- 자기 마음대로
 * 켜지는 것이 아니라 호스트의 신호를 기다린다.
 *
 * 이미 켜져 있으면 곧바로 돌아간다.
 *
 * 전원과 클록:
 *   1) runtime PM 을 얻는다.
 *   2) BPMP 로 컨트롤러를 켠다.
 *   3) 외부 REFCLK 를 쓰면 UPHY PLL 도 켠다. EP 모드는 Tegra194 에서도
 *      항상 이 값이 참이다(tegra_pcie_dw_parse_dt 참조).
 *   4) 코어 클록, APB 리셋, PHY 를 차례로 켠다.
 *
 * APPL 설정:
 *   5) 인터럽트 상태 열다섯 개를 모두 지운다.
 *   6) **이 코어를 EP 로 지정한다.** RC 경로가 APPL_DM_TYPE_RP 를 쓰는
 *      자리에 APPL_DM_TYPE_EP 를 쓴다. 기존 필드를 지우고 넣는 것에도
 *      주의 -- RC 쪽은 그냥 덮어쓴다.
 *   7) 핫 리셋 모드를 켠다. **EP 는 조건 없이 켠다** -- 호스트가 언제든
 *      리셋을 걸 수 있기 때문이다.
 *   8) 슬레이브 EP 모드와 캐시 속성을 설정한다.
 *   9) REFCLK 입력 덮어쓰기를 켠다. RC 가 출력을 막는 것과 반대 방향이다.
 *  10) DBI 와 iATU 기준 주소를 알려 준다.
 *  11) 인터럽트 셋을 켠다 -- 링크 상태, PCI 명령 변화, INT, eDMA.
 *  12) **LTR 메시지 값을 미리 채운다.** 110us 를 값 110 과 축척 2 로
 *      표현하는데, 그 조합의 의미는 PCIe LTR 형식이 정한다.
 *  13) 코어 리셋을 푼다.
 *
 * IP 초기화:
 *  14) EPC 코어에 해제를 알리고 DWC EP 상태를 정리한다. 원문 주석대로
 *      REFCLK 와 코어 리셋이 풀린 뒤여야 가능한 정리다.
 *  15) 속도 변경 비트를 지우고, 흐름 제어와 이퀄라이저와 ASPM 을 설정한다.
 *      **RC 경로의 tegra_pcie_dw_host_init 과 겹치는 부분이 여기다** --
 *      config_gen3_gen4_eq_presets 와 init_host_aspm 을 양쪽이 모두 부른다.
 *  16) PCIe 능력 위치를 찾고, SRNS 면 슬롯 클록 비트를 지운다.
 *  17) 코어 클록을 Gen4 주파수로 올린다.
 *  18) **MSI-X 주소 매칭 레지스터를 설정한다.** EP 가 MSI-X 를 보낼 때
 *      쓸 주소이며, tegra_pcie_ep_raise_msix_irq 가 그 메모리에 쓰면
 *      인터럽트가 나간다.
 *  19) DWC EP 레지스터 초기화를 마치고 EPC 코어에 알린다.
 *  20) has_ltr_req_fix 인 SoC 는 LTR 전송 권한을 미리 켠다. 그렇지
 *      않은 SoC 는 tegra_pcie_ep_irq_thread 가 매번 요청한다.
 *  21) LTSSM 을 켠다. **이제 호스트와 링크를 맺기 시작한다.**
 *  22) 상태를 ENABLED 로 표시한다.
 *
 * 정리 경로가 다섯 라벨이다. **반환 타입이 void 라 실패를 알릴 곳이
 * 없다** -- 정리만 하고 조용히 돌아가며, 상태가 DISABLED 로 남아 다음
 * PERST 해제 때 다시 시도된다.
 *
 * 실행 컨텍스트: 스레드 인터럽트 컨텍스트. 여러 곳에서 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_ep_pex_rst_irq → [이 함수]
 *     → tegra_pcie_bpmp_set_ 계열, tegra_pcie_enable_phy,
 *       config_gen3_gen4_eq_presets, init_host_aspm,
 *       dw_pcie_ep_init_registers, pci_epc_init_notify
 */
static void pex_ep_event_pex_rst_deassert(struct tegra_pcie_dw *pcie)
{
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 코어의 EP 구조체. MSI 메모리 주소를 얻는 데 쓴다 */
	struct dw_pcie_ep *ep = &pci->ep;
	/* [한국어] 로그의 기준 */
	struct device *dev = pcie->dev;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;
	/* [한국어] 오류 코드 보관용 */
	int ret;
	/* [한국어] 16비트 레지스터 값 임시 변수 */
	u16 val_16;

	/* [한국어] **이미 켜져 있으면 곧바로 돌아간다.** PERST 가 여러 번 흔들릴 수
	 * 있기 때문이며, ep_state 가 그 중복을 막는다 */
	if (pcie->ep_state == EP_STATE_ENABLED)
		return;

	/* [한국어] **runtime PM 을 얻는다.** assert 경로가 put 하는 것의 짝이다 */
	ret = pm_runtime_resume_and_get(dev);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] runtime PM 획득 실패 */
		dev_err(dev, "Failed to get runtime sync for PCIe dev: %d\n",
			ret);
		return;
	}

	/* [한국어] **BPMP 에 컨트롤러를 켜 달라고 요청한다.** UPHY 가 살아나야
	 * 나머지가 의미를 갖는다 */
	ret = tegra_pcie_bpmp_set_ctrl_state(pcie, true);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] 컨트롤러 활성화 실패 */
		dev_err(pcie->dev, "Failed to enable controller %u: %d\n",
			pcie->cid, ret);
		goto fail_set_ctrl_state;
	}

	/* [한국어] **EP 모드는 Tegra194 에서도 이 값이 항상 참이다**
	 * (tegra_pcie_dw_parse_dt 참조) */
	if (pcie->enable_ext_refclk) {
		/* [한국어] UPHY PLL 도 BPMP 에 요청한다 */
		ret = tegra_pcie_bpmp_set_pll_state(pcie, true);
		/* [한국어] 실패를 확인한다 */
		if (ret) {
			/* [한국어] UPHY PLL 초기화 실패 */
			dev_err(dev, "Failed to init UPHY for PCIe EP: %d\n",
				ret);
			goto fail_pll_init;
		}
	}

	/* [한국어] 코어 클록을 켠다 */
	ret = clk_prepare_enable(pcie->core_clk);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] 클록 활성화 실패 */
		dev_err(dev, "Failed to enable core clock: %d\n", ret);
		goto fail_core_clk_enable;
	}

	/* [한국어] **APB 리셋을 푼다. 이제 APPL 레지스터에 접근할 수 있다** */
	ret = reset_control_deassert(pcie->core_apb_rst);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] APB 리셋 해제 실패 */
		dev_err(dev, "Failed to deassert core APB reset: %d\n", ret);
		goto fail_core_apb_rst;
	}

	/* [한국어] PHY 를 켠다. **EP 모드라 보정(phy_calibrate)까지 한다** */
	ret = tegra_pcie_enable_phy(pcie);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] PHY 활성화 실패 */
		dev_err(dev, "Failed to enable PHY: %d\n", ret);
		goto fail_phy;
	}

	/* Clear any stale interrupt statuses */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L0);
	/* [한국어] 링크 상태 범주를 지운다. **같은 목록이 세 함수에 반복된다** --
	 * 이 함수, tegra_pcie_enable_interrupts, pex_ep_event_hot_rst_done 이며,
	 * 마지막 것만 APPL_MSI_CTRL_2 를 하나 더 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_0_0);
	/* [한국어] L1_1 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_1);
	/* [한국어] L1_2 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_2);
	/* [한국어] L1_3 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_3);
	/* [한국어] L1_6 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_6);
	/* [한국어] L1_7 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_7);
	/* [한국어] INT 범주 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_8_0);
	/* [한국어] L1_9 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_9);
	/* [한국어] L1_10 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_10);
	/* [한국어] L1_11 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_11);
	/* [한국어] L1_13 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_13);
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_14);
	/* [한국어] L1_15 상태를 지운다 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_15);
	/* [한국어] 마지막 상태 레지스터 */
	appl_writel(pcie, 0xFFFFFFFF, APPL_INTR_STATUS_L1_17);

	/* configure this core for EP mode operation */
	val = appl_readl(pcie, APPL_DM_TYPE);
	/* [한국어] **모드 필드를 지운 뒤 넣는다.** RC 경로가 그냥 덮어쓰는 것과 다르다 --
	 * 값이 0 이라 지우기만 해도 되지만 형태를 갖춘 것이다 */
	val &= ~APPL_DM_TYPE_MASK;
	/* [한국어] 엔드포인트 모드 값을 넣는다 */
	val |= APPL_DM_TYPE_EP;
	/* [한국어] 모드를 쓴다 */
	appl_writel(pcie, val, APPL_DM_TYPE);

	/* [한국어] 클록 게이팅 우회를 끈다 */
	appl_writel(pcie, 0x0, APPL_CFG_SLCG_OVERRIDE);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_CTRL);
	/* [한국어] 사전 감지 상태를 켠다 */
	val |= APPL_CTRL_SYS_PRE_DET_STATE;
	/* [한국어] **핫 리셋을 조건 없이 켠다.** RC 는 조건부인데 EP 는 호스트가
	 * 언제든 리셋을 걸 수 있으므로 항상 필요하다 */
	val |= APPL_CTRL_HW_HOT_RST_EN;
	/* [한국어] 모드 필드를 지운다 */
	val &= ~(APPL_CTRL_HW_HOT_RST_MODE_MASK << APPL_CTRL_HW_HOT_RST_MODE_SHIFT);
	/* [한국어] 즉시 리셋 후 LTSSM 재활성화 모드를 넣는다 */
	val |= (APPL_CTRL_HW_HOT_RST_MODE_IMDT_RST_LTSSM_EN << APPL_CTRL_HW_HOT_RST_MODE_SHIFT);
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_CTRL);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_CFG_MISC);
	/* [한국어] **슬레이브 EP 모드를 켠다.** RC 경로에 없는 비트다 */
	val |= APPL_CFG_MISC_SLV_EP_MODE;
	/* [한국어] 캐시 속성을 넣는다. RC 와 같은 값이다 */
	val |= (APPL_CFG_MISC_ARCACHE_VAL << APPL_CFG_MISC_ARCACHE_SHIFT);
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_CFG_MISC);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_PINMUX);
	/* [한국어] REFCLK 덮어쓰기를 켠다 */
	val |= APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE_EN;
	/* [한국어] **값도 1 로 세운다.** RC 경로는 활성화만 켜고 값을 0 으로 두는데,
	 * EP 는 REFCLK 를 받아야 하므로 방향이 반대다 */
	val |= APPL_PINMUX_CLK_OUTPUT_IN_OVERRIDE;
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_PINMUX);

	/* [한국어] **DBI 블록의 물리 기준 주소를 알려 준다.** RC 경로와 같은 일이다 */
	appl_writel(pcie, pcie->dbi_res->start & APPL_CFG_BASE_ADDR_MASK,
		    APPL_CFG_BASE_ADDR);

	/* [한국어] iATU 블록의 물리 기준 주소도 알려 준다 */
	appl_writel(pcie, pcie->atu_dma_res->start &
		    APPL_CFG_IATU_DMA_BASE_ADDR_MASK,
		    APPL_CFG_IATU_DMA_BASE_ADDR);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_INTR_EN_L0_0);
	/* [한국어] 시스템 인터럽트 전달 */
	val |= APPL_INTR_EN_L0_0_SYS_INTR_EN;
	/* [한국어] 링크 상태 범주 */
	val |= APPL_INTR_EN_L0_0_LINK_STATE_INT_EN;
	/* [한국어] **PCI 명령 변화 범주. EP 전용이다** -- 호스트가 버스 마스터 비트를
	 * 세우면 LTR 을 보내야 하기 때문이다 */
	val |= APPL_INTR_EN_L0_0_PCI_CMD_EN_INT_EN;
	/* [한국어] INT 범주 */
	val |= APPL_INTR_EN_L0_0_INT_INT_EN;
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_INTR_EN_L0_0);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_INTR_EN_L1_0_0);
	/* [한국어] 핫 리셋 완료 알림을 켠다 */
	val |= APPL_INTR_EN_L1_0_0_HOT_RESET_DONE_INT_EN;
	/* [한국어] 링크 업 알림을 켠다. 호스트와 링크가 맺어졌음을 알아야 EP 코어에
	 * 통보할 수 있다 */
	val |= APPL_INTR_EN_L1_0_0_RDLH_LINK_UP_INT_EN;
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_INTR_EN_L1_0_0);

	/* [한국어] 현재 값을 읽는다 */
	val = appl_readl(pcie, APPL_INTR_EN_L1_8_0);
	/* [한국어] eDMA 인터럽트를 켠다. **처리는 DMA 드라이버가 한다** */
	val |= APPL_INTR_EN_L1_8_EDMA_INT_EN;
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_INTR_EN_L1_8_0);

	/* 110us for both snoop and no-snoop */
	val = FIELD_PREP(PCI_LTR_VALUE_MASK, 110) |
	      /* [한국어] 축척 2. 값 110 과 합쳐 110us 를 뜻한다 */
	      FIELD_PREP(PCI_LTR_SCALE_MASK, 2) |
	      /* [한국어] 요청 비트를 세운다 */
	      LTR_MSG_REQ |
	      /* [한국어] 논스누프 값도 110 */
	      FIELD_PREP(PCI_LTR_NOSNOOP_VALUE, 110) |
	      /* [한국어] 논스누프 축척 */
	      FIELD_PREP(PCI_LTR_NOSNOOP_SCALE, 2) |
	      /* [한국어] 논스누프 요청 비트 */
	      LTR_NOSNOOP_MSG_REQ;
	/* [한국어] LTR 값을 채워 둔다. 실제 전송은 나중에 요청 비트로 촉발한다 */
	appl_writel(pcie, val, APPL_LTR_MSG_1);

	reset_control_deassert(pcie->core_rst);

	/* Perform cleanup that requires refclk and core reset deasserted */
	pci_epc_deinit_notify(pcie->pci.ep.epc);
	dw_pcie_ep_cleanup(&pcie->pci.ep);

	/* [한국어] 현재 값을 읽는다 */
	val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
	/* [한국어] 속도 변경 요청 비트를 지운다 */
	val &= ~PORT_LOGIC_SPEED_CHANGE;
	/* [한국어] 되쓴다 */
	dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);

	/* [한국어] **보드가 지정하는 우회다** -- SoC 기술자가 아니라 장치 트리에서 온다 */
	if (pcie->update_fc_fixup) {
		/* [한국어] 현재 값을 읽는다 */
		val = dw_pcie_readl_dbi(pci, CFG_TIMER_CTRL_MAX_FUNC_NUM_OFF);
		/* [한국어] ACK/NAK 자리에 1 을 세운다 */
		val |= 0x1 << CFG_TIMER_CTRL_ACK_NAK_SHIFT;
		/* [한국어] 되쓴다 */
		dw_pcie_writel_dbi(pci, CFG_TIMER_CTRL_MAX_FUNC_NUM_OFF, val);
	}

	config_gen3_gen4_eq_presets(pcie);

	init_host_aspm(pcie);

	/* [한국어] 결함이 고쳐지지 않은 SoC 에서만 우회한다 */
	if (!pcie->of_data->has_l1ss_exit_fix) {
		/* [한국어] 현재 값을 읽는다 */
		val = dw_pcie_readl_dbi(pci, GEN3_RELATED_OFF);
		/* [한국어] 수신 종단 규격 관련 비트를 지운다. 이름이 그것을 가리키나
		 * 근거 문서는 이 트리에 없다 */
		val &= ~GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL;
		/* [한국어] 되쓴다 */
		dw_pcie_writel_dbi(pci, GEN3_RELATED_OFF, val);
	}

	/* [한국어] **PCIe 능력 위치를 찾는다.** RC 경로가 조건부로 찾는 것과 달리
	 * 여기서는 무조건 찾는다 -- EP 는 코어 리셋 뒤라 매번 다시 찾아야
	 * 하기 때문으로 보인다 */
	pcie->pcie_cap_base = dw_pcie_find_capability(&pcie->pci,
						      PCI_CAP_ID_EXP);

	/* Clear Slot Clock Configuration bit if SRNS configuration */
	if (pcie->enable_srns) {
		/* [한국어] 링크 상태를 읽는다 */
		val_16 = dw_pcie_readw_dbi(pci, pcie->pcie_cap_base +
					   PCI_EXP_LNKSTA);
		/* [한국어] 슬롯 클록 구성 비트를 지운다 */
		val_16 &= ~PCI_EXP_LNKSTA_SLC;
		/* [한국어] 되쓴다 */
		dw_pcie_writew_dbi(pci, pcie->pcie_cap_base + PCI_EXP_LNKSTA,
				   val_16);
	}

	/* [한국어] 코어 클록을 Gen4 주파수로 올린다 */
	clk_set_rate(pcie->core_clk, GEN4_CORE_CLK_FREQ);

	/* [한국어] **MSI-X 수신 주소를 등록한다.** EP 코어가 잡아 둔 MSI 메모리의
	 * 물리 주소이며, tegra_pcie_ep_raise_msix_irq 가 그 메모리에 쓰면
	 * 하드웨어가 이 주소와 맞춰 보고 MSI-X 로 바꾼다 */
	val = (ep->msi_mem_phys & MSIX_ADDR_MATCH_LOW_OFF_MASK);
	/* [한국어] 매칭 활성화 비트를 세운다 */
	val |= MSIX_ADDR_MATCH_LOW_OFF_EN;
	/* [한국어] 하위 주소와 활성화 비트를 함께 쓴다 */
	dw_pcie_writel_dbi(pci, MSIX_ADDR_MATCH_LOW_OFF, val);
	/* [한국어] 상위 주소를 꺼낸다 */
	val = (upper_32_bits(ep->msi_mem_phys) & MSIX_ADDR_MATCH_HIGH_OFF_MASK);
	/* [한국어] 상위 32비트를 쓴다 */
	dw_pcie_writel_dbi(pci, MSIX_ADDR_MATCH_HIGH_OFF, val);

	/* [한국어] **DWC EP 레지스터 초기화를 마친다.** BAR 설정 등 코어가 담당하는
	 * 부분이다 */
	ret = dw_pcie_ep_init_registers(ep);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] EP 레지스터 초기화 실패 */
		dev_err(dev, "Failed to complete initialization: %d\n", ret);
		goto fail_init_complete;
	}

	pci_epc_init_notify(ep->epc);

	/* Program the private control to allow sending LTR upstream */
	if (pcie->of_data->has_ltr_req_fix) {
		/* [한국어] 현재 값을 읽는다 */
		val = appl_readl(pcie, APPL_LTR_MSG_2);
		/* [한국어] LTR 전송 권한을 켠다 */
		val |= APPL_LTR_MSG_2_LTR_MSG_REQ_STATE;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_LTR_MSG_2);
	}

	/* Enable LTSSM */
	val = appl_readl(pcie, APPL_CTRL);
	/* [한국어] **LTSSM 을 켠다. 이제 호스트와 링크를 맺기 시작한다.**
	 * 이 한 줄이 이 긴 함수의 최종 목적이다 */
	val |= APPL_CTRL_LTSSM_EN;
	/* [한국어] 설정을 쓴다 */
	appl_writel(pcie, val, APPL_CTRL);

	/* [한국어] 상태를 켜짐으로 표시한다. 이제 assert 경로가 실제로 정리를 수행한다 */
	pcie->ep_state = EP_STATE_ENABLED;
	/* [한국어] 초기화 완료를 디버그 로그로 남긴다 */
	dev_dbg(dev, "Initialization of endpoint is completed\n");

	return;

fail_init_complete:
	reset_control_assert(pcie->core_rst);
	tegra_pcie_disable_phy(pcie);
fail_phy:
	reset_control_assert(pcie->core_apb_rst);
fail_core_apb_rst:
	clk_disable_unprepare(pcie->core_clk);
fail_core_clk_enable:
	tegra_pcie_bpmp_set_pll_state(pcie, false);
fail_pll_init:
	tegra_pcie_bpmp_set_ctrl_state(pcie, false);
fail_set_ctrl_state:
	pm_runtime_put_sync(dev);
}

/* [한국어]
 * tegra_pcie_ep_pex_rst_irq - PERST GPIO 인터럽트. 방향을 보고 두 함수로 나눈다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: struct tegra_pcie_dw.
 * @return: 항상 IRQ_HANDLED.
 *
 * **EP 모드의 진입점이다.** PERST GPIO 가 양쪽 에지로 등록되어 있어
 * (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING) 걸릴 때와 풀릴 때 모두
 * 이 함수가 불린다. 그래서 지금 값을 읽어 어느 쪽인지 판단한다.
 *
 *   값이 1(어서트)이면 → 엔드포인트를 허문다.
 *   값이 0(해제)이면 → 엔드포인트를 세운다.
 *
 * **스레드 핸들러다.** devm_request_threaded_irq 에 하드 핸들러를 NULL 로
 * 주고 이 함수를 스레드 자리에 넣었는데, 두 하위 함수가 BPMP 통신과
 * 폴링으로 오래 잠들기 때문이다.
 *
 * IRQF_ONESHOT 으로 등록되어 스레드가 끝날 때까지 인터럽트가 다시 걸리지
 * 않는다. PERST 가 빠르게 흔들려도 두 함수가 겹쳐 실행되지 않게 하는
 * 보호이며, 두 함수가 각각 상태를 먼저 확인해 중복 실행을 막는 것과
 * 함께 이중의 방어가 된다.
 *
 * GPIO 디바운스가 5ms 로 설정되어 있어(tegra_pcie_config_ep) 신호 잡음이
 * 걸러진다.
 *
 * 실행 컨텍스트: 스레드 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   PERST GPIO 에지 → 커널 → [이 함수]
 *     → pex_ep_event_pex_rst_assert 또는 _deassert
 */
static irqreturn_t tegra_pcie_ep_pex_rst_irq(int irq, void *arg)
{
	struct tegra_pcie_dw *pcie = arg;

	/* [한국어] **GPIO 값으로 방향을 판단한다.** 양쪽 에지로 등록되어 있어
	 * 걸릴 때와 풀릴 때 모두 이 함수가 불리기 때문이다 */
	if (gpiod_get_value(pcie->pex_rst_gpiod))
		pex_ep_event_pex_rst_assert(pcie);
	else
		pex_ep_event_pex_rst_deassert(pcie);

	return IRQ_HANDLED;
}

/* [한국어]
 * tegra_pcie_ep_raise_intx_irq - 엔드포인트가 INTx 를 올린다
 *
 * @pcie: 드라이버 상태.
 * @irq:  INTx 번호. **1(INTA)만 유효하다.**
 * @return: 0 성공, 범위를 벗어나면 -EINVAL.
 *
 * 엔드포인트가 호스트에게 레거시 인터럽트를 보낸다. PCIe 에는 물리적인
 * INTx 선이 없으므로 실제로는 Assert_INTA 와 Deassert_INTA 메시지가 나간다.
 *
 * **Tegra194 는 INTA 만 지원한다.** 위의 원문 주석이 그 사실을 밝히며,
 * 그래서 1 을 넘는 번호를 거른다. 다만 검사가 `irq > 1` 이라 0 도
 * 통과하는데, 그 경우 어떻게 되는지는 코드에서 알 수 없다.
 *
 * 레지스터에 1 을 썼다가 1~2ms 뒤 0 을 쓴다. **소프트웨어가 펄스를 직접
 * 만드는 구조** 이며, 그 사이의 지연이 어서트 상태를 유지하는 시간이다.
 *
 * usleep_range 를 쓰므로 이 함수는 잠들 수 있다. dw_pcie_ep_ops.raise_irq
 * 콜백이 프로세스 컨텍스트에서 불린다는 전제가 깔려 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(EPF 드라이버의 인터럽트 요청).
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq → tegra_pcie_ep_raise_irq → [이 함수]
 */
static int tegra_pcie_ep_raise_intx_irq(struct tegra_pcie_dw *pcie, u16 irq)
{
	/* Tegra194 supports only INTA */
	if (irq > 1)
		return -EINVAL;

	/* [한국어] 1 을 써서 어서트한다. PCIe 에는 물리적 INTx 선이 없으므로 실제로는
	 * Assert_INTA 메시지가 나간다 */
	appl_writel(pcie, 1, APPL_LEGACY_INTX);
	/* [한국어] 1~2ms 유지한다. **이 지연이 어서트 상태를 유지하는 시간이다** */
	usleep_range(1000, 2000);
	/* [한국어] 0 을 써서 디어서트한다 */
	appl_writel(pcie, 0, APPL_LEGACY_INTX);
	return 0;
}

/* [한국어]
 * tegra_pcie_ep_raise_msi_irq - 엔드포인트가 MSI 를 올린다
 *
 * @pcie: 드라이버 상태.
 * @irq:  MSI 번호. **1부터 센다.**
 * @return: 0 성공, 32 를 넘으면 -EINVAL.
 *
 * APPL_MSI_CTRL_1 에 해당 비트를 세우면 하드웨어가 MSI 를 내보낸다.
 *
 * **번호가 1부터라 BIT(irq - 1) 로 변환한다.** PCI 규격의 인터럽트 번호
 * 관례를 따르는 것이며, 호출자인 EPF 드라이버가 1 기반으로 넘긴다.
 *
 * 경계 검사가 `irq > 32` 인데, 레지스터가 32비트이므로 유효한 값은
 * 1~32 다. irq 가 0 이면 BIT(-1) 이 되어 정의되지 않은 동작이지만
 * 검사가 그것을 막지 않는다 -- INTx 쪽과 같은 형태의 느슨함이다.
 *
 * unlikely 를 쓴 것은 정상 경로에서 이 조건이 거의 참이 아니기 때문이다.
 *
 * INTx 와 달리 펄스를 만들지 않는다. MSI 는 메모리 쓰기 한 번이므로
 * 어서트/디어서트 개념이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq → tegra_pcie_ep_raise_irq → [이 함수]
 */
static int tegra_pcie_ep_raise_msi_irq(struct tegra_pcie_dw *pcie, u16 irq)
{
	if (unlikely(irq > 32))
		return -EINVAL;

	/* [한국어] **번호가 1부터라 1 을 빼서 비트 자리를 만든다.** PCI 규격의 인터럽트
	 * 번호 관례를 따르는 것이며, 호출자인 EPF 드라이버가 1 기반으로 넘긴다 */
	appl_writel(pcie, BIT(irq - 1), APPL_MSI_CTRL_1);

	return 0;
}

/* [한국어]
 * tegra_pcie_ep_raise_msix_irq - 엔드포인트가 MSI-X 를 올린다
 *
 * @pcie: 드라이버 상태.
 * @irq:  MSI-X 벡터 번호.
 * @return: 항상 0.
 *
 * **MSI 와 방식이 완전히 다르다.** APPL 레지스터에 비트를 세우는 것이
 * 아니라, EP 코어가 매핑해 둔 메모리 창(ep->msi_mem)에 벡터 번호를
 * 그대로 쓴다.
 *
 * 그 메모리 창의 물리 주소는 pex_ep_event_pex_rst_deassert 가
 * MSIX_ADDR_MATCH_LOW/HIGH_OFF 레지스터에 등록해 둔 것이다. 하드웨어가
 * 그 주소로 가는 쓰기를 가로채 MSI-X 트랜잭션으로 바꾼다.
 *
 * 즉 **주소가 인터럽트임을 알리고 데이터가 벡터를 고른다** -- 호스트
 * 쪽 MSI 수신 구조와 대칭인 방식이며, 이 파일에서 EP 가 그 반대편에
 * 서 있음을 잘 보여 주는 대목이다.
 *
 * 경계 검사가 없다. 벡터 번호의 유효성은 EPC 코어가 확인한다는 전제로
 * 보인다.
 *
 * writel 을 쓰는 것에 주의 -- appl_writel 이 아니다. 대상이 APPL 블록이
 * 아니라 EP 의 MSI 메모리 창이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq → tegra_pcie_ep_raise_irq → [이 함수]
 */
static int tegra_pcie_ep_raise_msix_irq(struct tegra_pcie_dw *pcie, u16 irq)
{
	struct dw_pcie_ep *ep = &pcie->pci.ep;

	/* [한국어] **메모리에 벡터 번호를 쓴다.** 그 메모리의 물리 주소는
	 * pex_ep_event_pex_rst_deassert 가 MSIX_ADDR_MATCH 레지스터에 등록해
	 * 둔 것이고, 하드웨어가 그 주소로 가는 쓰기를 가로채 MSI-X 로 바꾼다.
	 * appl_writel 이 아니라 raw writel 인 것은 대상이 APPL 블록이 아니라
	 * EP 의 MSI 메모리 창이기 때문이다 */
	writel(irq, ep->msi_mem);

	return 0;
}

/* [한국어]
 * tegra_pcie_ep_raise_irq - dw_pcie_ep_ops.raise_irq 콜백. 종류에 따라 갈라 보낸다
 *
 * @ep:            DWC 엔드포인트 구조체.
 * @func_no:       함수 번호. **이 드라이버는 쓰지 않는다** -- 물리 함수가
 *   하나뿐이기 때문으로 보인다.
 * @type:          인터럽트 종류(INTX, MSI, MSIX).
 * @interrupt_num: 벡터 또는 핀 번호.
 * @return: 하위 함수의 반환값. 알 수 없는 종류면 -EPERM.
 *
 * **이 드라이버가 채운 두 ep_ops 콜백 중 하나다**(다른 하나는
 * get_features). 나머지 세 자리는 채우지 않아 DWC EP 코어의 기본 동작을
 * 그대로 쓴다.
 *
 * 세 종류를 세 함수로 나눠 보내는 분배기다. 각 방식이 하드웨어적으로
 * 전혀 달라 -- INTx 는 레지스터 펄스, MSI 는 레지스터 비트, MSI-X 는
 * 메모리 쓰기 -- 함수를 나눌 만하다.
 *
 * default 절이 -EPERM 을 돌려주는데, 권한 오류를 뜻하는 코드라
 * 의미상 -EINVAL 이 더 어울릴 자리다. 코드를 손대지 않고 사실만 적어 둔다.
 *
 * switch 아래의 return 0 은 **도달하지 않는다** -- 모든 case 가 반환하고
 * default 도 반환하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq → ep->ops->raise_irq → [이 함수]
 *     → tegra_pcie_ep_raise_intx_irq / _msi_irq / _msix_irq
 */
static int tegra_pcie_ep_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				   unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* [한국어] 드라이버 상태를 되찾는다. **두 단계 변환** 이다 --
	 * to_dw_pcie_from_ep 로 코어 구조체를 얻고 다시 이 함수로 나온다 */
	struct tegra_pcie_dw *pcie = to_tegra_pcie(pci);

	/* [한국어] **세 방식이 하드웨어적으로 전혀 달라** 함수를 나눌 만하다 */
	switch (type) {
	/* [한국어] INTx. 레지스터 펄스로 보낸다 */
	case PCI_IRQ_INTX:
		return tegra_pcie_ep_raise_intx_irq(pcie, interrupt_num);

	/* [한국어] MSI. 레지스터 비트로 보낸다 */
	case PCI_IRQ_MSI:
		return tegra_pcie_ep_raise_msi_irq(pcie, interrupt_num);

	/* [한국어] MSI-X. 메모리 쓰기로 보낸다 */
	case PCI_IRQ_MSIX:
		return tegra_pcie_ep_raise_msix_irq(pcie, interrupt_num);

	default:
		dev_err(pci->dev, "Unknown IRQ type\n");
		return -EPERM;
	}

	return 0;
}

/* [한국어] BAR2 안의 예약 영역 둘. MSI-X 표가 처음 64KiB, PBA 가 그다음
 * 64KiB 를 차지한다. **EPC 코어에게 이 영역들을 알려 줘야** EPF 가
 * 겹치는 자리를 요청하지 않는다 */
static const struct pci_epc_bar_rsvd_region tegra194_bar2_rsvd[] = {
	{
		/* MSI-X table structure */
		.type = PCI_EPC_BAR_RSVD_MSIX_TBL_RAM,
		.offset = 0x0,
		.size = SZ_64K,
	},
	{
		/* MSI-X PBA structure */
		.type = PCI_EPC_BAR_RSVD_MSIX_PBA_RAM,
		.offset = 0x10000,
		.size = SZ_64K,
	},
};

/* [한국어] BAR4 안의 예약 영역. DMA 제어 레지스터가 처음 4KiB 를 차지한다 */
static const struct pci_epc_bar_rsvd_region tegra194_bar4_rsvd[] = {
	{
		/* DMA_CAP (BAR4: DMA Port Logic Structure) */
		.type = PCI_EPC_BAR_RSVD_DMA_CTRL_MMIO,
		.offset = 0x0,
		.size = SZ_4K,
	},
};

/* Tegra EP: BAR0 = 64-bit programmable BAR,  BAR2 = 64-bit MSI-X table, BAR4 = 64-bit DMA regs. */
static const struct pci_epc_features tegra_pcie_epc_features = {
	DWC_EPC_COMMON_FEATURES,
	.linkup_notifier = true,
	.msi_capable = true,
	.bar[BAR_0] = { .only_64bit = true, },
	.bar[BAR_2] = {
		/* [한국어] **BAR2 는 예약이다.** MSI-X 표와 PBA 가 하드웨어적으로 그 안에
		 * 자리 잡고 있어 EPF 가 다른 용도로 쓸 수 없다 */
		.type = BAR_RESERVED,
		.only_64bit = true,
		.nr_rsvd_regions = ARRAY_SIZE(tegra194_bar2_rsvd),
		.rsvd_regions = tegra194_bar2_rsvd,
	},
	.bar[BAR_4] = {
		/* [한국어] **BAR4 도 예약이다.** DMA 레지스터가 자리 잡고 있다 */
		.type = BAR_RESERVED,
		.only_64bit = true,
		.nr_rsvd_regions = ARRAY_SIZE(tegra194_bar4_rsvd),
		.rsvd_regions = tegra194_bar4_rsvd,
	},
	.align = SZ_64K,
};

/* [한국어]
 * tegra_pcie_ep_get_features - dw_pcie_ep_ops.get_features 콜백. BAR 배치를 알린다
 *
 * @ep: DWC 엔드포인트 구조체. 쓰지 않는다.
 * @return: 이 하드웨어의 EP 능력 기술자. 항상 같은 정적 구조체다.
 *
 * EPC 코어가 "이 엔드포인트는 무엇을 할 수 있고 BAR 를 어떻게 쓸 수
 * 있는가" 를 물을 때 답한다. EPF 드라이버가 BAR 를 요청할 때 코어가
 * 이 정보로 가능 여부를 판단한다.
 *
 * 돌려주는 tegra_pcie_epc_features 의 내용이 이 하드웨어의 제약을
 * 그대로 담고 있다.
 *   BAR0 은 64비트 전용이고 EPF 가 자유롭게 쓸 수 있다.
 *   **BAR2 와 BAR4 는 예약이다** -- BAR2 에는 MSI-X 표와 PBA 가,
 *     BAR4 에는 DMA 레지스터가 하드웨어적으로 자리 잡고 있어 EPF 가
 *     다른 용도로 쓸 수 없다. 각각의 rsvd_regions 배열이 그 안에서
 *     어느 오프셋에 무엇이 있는지까지 알려 준다.
 *   정렬 요구가 64KiB 다.
 *   링크업 통보를 지원한다 -- tegra_pcie_ep_irq_thread 가
 *     dw_pcie_ep_linkup 을 부르는 것이 그 근거다.
 *
 * 인자를 전혀 쓰지 않고 상수를 돌려주는 것은, 이 하드웨어의 능력이
 * 인스턴스마다 달라지지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   EPC 코어 / EPF 드라이버 → ep->ops->get_features → [이 함수]
 */
static const struct pci_epc_features*
tegra_pcie_ep_get_features(struct dw_pcie_ep *ep)
{
	return &tegra_pcie_epc_features;
}

/* [한국어] DWC 엔드포인트 동작 정의 */
static const struct dw_pcie_ep_ops pcie_ep_ops = {
	/* [한국어] **두 자리만 채운다.** 나머지 셋을 비워 둔 것은 DWC EP 코어의 기본
	 * 동작을 그대로 쓰겠다는 선언이다 -- 채우지 않은 것이 채운 것만큼
	 * 의미를 갖는 접착 계층의 특징이다 */
	.raise_irq = tegra_pcie_ep_raise_irq,
	.get_features = tegra_pcie_ep_get_features,
};

/* [한국어]
 * tegra_pcie_config_ep - 엔드포인트 모드를 준비한다
 *
 * @pcie: 드라이버 상태.
 * @pdev: 플랫폼 장치. **이 함수는 쓰지 않는다.**
 * @return: 0 성공, 음수 오류.
 *
 * EP 모드 probe 경로의 최상위다. **하드웨어를 켜지 않는다** --
 * 그것은 호스트가 PERST 를 풀어 줄 때 pex_ep_event_pex_rst_deassert 가
 * 한다. 이 함수가 하는 것은 그 신호를 받을 준비다.
 *
 *   1) EP ops 와 페이지 크기를 지정한다. 64KiB 는 위의 epc_features 의
 *      align 값과 같다.
 *   2) PERST GPIO 디바운스를 5ms 로 설정한다. 신호 잡음을 거른다.
 *   3) 그 GPIO 를 인터럽트로 바꾼다.
 *   4) 인터럽트 이름을 만든다. 컨트롤러 ID 를 넣어 여러 인스턴스를
 *      구별할 수 있게 한다.
 *   5) **IRQ_NOAUTOEN 을 설정한다.** 이것이 중요한데, request 시점에
 *      자동으로 켜지지 않게 막는다. 실제로 켜지는 것은
 *      tegra_pcie_dw_start_link 가 EP 모드에서 enable_irq 를 부를 때다.
 *      즉 DWC 코어가 "링크를 시작하라" 고 할 때 비로소 PERST 를 듣기
 *      시작한다.
 *   6) 상태를 DISABLED 로 초기화한다.
 *   7) 양쪽 에지 스레드 인터럽트로 등록한다.
 *   8) runtime PM 을 켠다.
 *   9) DWC EP 하위 시스템을 초기화한다.
 *
 * **tegra_pcie_config_rp 와 비교하면 구조가 뒤집혀 있다.** RC 는
 * probe 에서 전부 켜고 링크까지 세우지만, EP 는 준비만 하고 기다린다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_dw_probe → [이 함수]
 *     → gpiod_set_debounce, gpiod_to_irq, devm_request_threaded_irq,
 *       dw_pcie_ep_init
 */
static int tegra_pcie_config_ep(struct tegra_pcie_dw *pcie,
				struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	/* [한국어] 로그와 devm 자원 획득의 기준 */
	struct device *dev = pcie->dev;
	/* [한국어] DWC 엔드포인트 구조체 포인터 */
	struct dw_pcie_ep *ep;
	/* [한국어] 인터럽트 이름을 만들 임시 문자열 */
	char *name;
	/* [한국어] 오류 코드 보관용 */
	int ret;

	/* [한국어] 코어의 EP 구조체 주소를 잡는다 */
	ep = &pci->ep;
	/* [한국어] **EP ops 를 꽂는다.** raise_irq 와 get_features 두 자리만 채운
	 * 구조체이며, 나머지 셋은 DWC EP 코어의 기본 동작을 쓴다 */
	ep->ops = &pcie_ep_ops;

	/* [한국어] 페이지 크기를 64KiB 로 정한다. 위 epc_features 의 align 값과 같다 */
	ep->page_size = SZ_64K;

	/* [한국어] 5ms 디바운스를 건다. 신호 잡음으로 인터럽트가 여러 번 걸리는 것을 막는다 */
	ret = gpiod_set_debounce(pcie->pex_rst_gpiod, PERST_DEBOUNCE_TIME);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 디바운스 설정 실패 */
		dev_err(dev, "Failed to set PERST GPIO debounce time: %d\n",
			ret);
		return ret;
	}

	/* [한국어] **PERST GPIO 를 인터럽트로 바꾼다.** 이것이 EP 모드의 진입점이다 */
	ret = gpiod_to_irq(pcie->pex_rst_gpiod);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] GPIO 를 인터럽트로 못 바꾸면 EP 모드를 쓸 수 없다 */
		dev_err(dev, "Failed to get IRQ for PERST GPIO: %d\n", ret);
		return ret;
	}
	/* [한국어] 인터럽트 번호를 보관한다 */
	pcie->pex_rst_irq = (unsigned int)ret;

	/* [한국어] **컨트롤러 ID 를 넣어 이름을 짓는다.** 이 SoC 에 PCIe 인스턴스가
	 * 여럿이라 /proc/interrupts 에서 구별할 수 있어야 하기 때문이다 */
	name = devm_kasprintf(dev, GFP_KERNEL, "tegra_pcie_%u_pex_rst_irq",
			      pcie->cid);
	/* [한국어] 할당 실패를 확인한다 */
	if (!name) {
		/* [한국어] 이름 할당 실패 */
		dev_err(dev, "Failed to create PERST IRQ string\n");
		return -ENOMEM;
	}

	/* [한국어] **자동 활성화를 막는다.** request 시점에 켜지지 않고,
	 * tegra_pcie_dw_start_link 가 EP 모드에서 enable_irq 를 부를 때 비로소
	 * 켜진다. 즉 DWC 코어가 "링크를 시작하라" 고 할 때부터 PERST 를 듣는다 */
	irq_set_status_flags(pcie->pex_rst_irq, IRQ_NOAUTOEN);

	/* [한국어] 아직 켜지지 않은 상태로 초기화한다 */
	pcie->ep_state = EP_STATE_DISABLED;

	/* [한국어] **하드 핸들러를 NULL 로 주고 스레드만 등록한다.** 두 하위 함수가
	 * BPMP 통신과 폴링으로 오래 잠들기 때문이다 */
	ret = devm_request_threaded_irq(dev, pcie->pex_rst_irq, NULL,
					tegra_pcie_ep_pex_rst_irq,
					IRQF_TRIGGER_RISING |
					/* [한국어] **양쪽 에지로 등록한다.** PERST 가 걸릴 때와 풀릴 때 모두 받아야
					 * 하기 때문이다. IRQF_ONESHOT 은 스레드가 끝날 때까지 인터럽트가 다시
					 * 걸리지 않게 해, 두 하위 함수가 겹쳐 실행되는 것을 막는다 */
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					name, (void *)pcie);
	if (ret < 0) {
		/* [한국어] PERST 인터럽트 등록 실패. **이것이 실패하면 EP 모드를 쓸 수 없다** --
		 * 호스트의 신호를 받을 통로가 없기 때문이다 */
		dev_err(dev, "Failed to request IRQ for PERST: %d\n", ret);
		return ret;
	}

	pm_runtime_enable(dev);

	/* [한국어] **DWC 엔드포인트 하위 시스템을 초기화한다.** 여기서 EPC 가 등록되고
	 * EPF 드라이버가 붙을 수 있게 된다 */
	ret = dw_pcie_ep_init(ep);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		/* [한국어] DWC EP 초기화 실패 */
		dev_err(dev, "Failed to initialize DWC Endpoint subsystem: %d\n",
			ret);
		pm_runtime_disable(dev);
		return ret;
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_dw_probe - 플랫폼 드라이버 진입점. RC/EP 를 갈라 기동한다
 *
 * @pdev: 장치 트리 매칭으로 만들어진 플랫폼 장치.
 * @return: 0 성공, 음수 오류.
 *
 *   1) SoC/모드 기술자를 고른다. compatible 네 가지가 RC/EP × 두 세대에
 *      대응하므로, **이 한 줄이 모드까지 결정한다.**
 *   2) 드라이버 상태를 잡는다. **pci_host_bridge 와 함께 잡지 않는 데
 *      주의** -- 앞 세대 pci-tegra.c 는 devm_pci_alloc_host_bridge 로
 *      브리지 뒤에 붙였지만, 여기서는 그냥 devm_kzalloc 이다. 브리지
 *      할당은 DWC 코어가 dw_pcie_host_init 안에서 한다.
 *   3) DWC 코어 구조체를 채운다 -- dev, ops, n_fts, num_vectors.
 *      **pp->num_vectors 를 여기서 지정하는 것이 MSI 설정의 전부다.**
 *      앞 세대가 도메인과 비트맵과 irq_chip 을 직접 만들던 자리를
 *      이 한 줄이 대신한다.
 *   4) 장치 트리를 읽는다.
 *   5) 슬롯 regulator, REFCLK 선택 GPIO, 컨트롤러 regulator, 클록 둘,
 *      APPL 창, APB 리셋, p2u PHY 들, atu_dma 창, 코어 리셋, 인터럽트,
 *      BPMP 핸들, interconnect 경로를 차례로 얻는다.
 *   6) **모드에 따라 갈라진다.**
 *        RC : 일반 핸들러를 걸고 tegra_pcie_config_rp 로 기동한다.
 *          -ENOMEDIUM(링크 없음)은 오류로 보지 않고 성공 처리한다.
 *        EP : 하드+스레드 핸들러 짝을 걸고 tegra_pcie_config_ep 로
 *          준비만 한다.
 *
 * **atu_base 를 여기서 매핑해 코어에 넘기는 것** 이 특징이다. 앞 세대라면
 * 드라이버가 그 레지스터를 직접 썼겠지만, 여기서는 주소만 잡아 주고
 * 내용은 DWC 코어가 관리한다.
 *
 * 정리 경로가 하나뿐이다(fail 라벨). BPMP 핸들만 되돌리는데, 나머지가
 * 모두 devm 이기 때문이다. 다만 interconnect 획득 실패 시에는 그 라벨을
 * 쓰지 않고 직접 tegra_bpmp_put 을 부르는데, 형태가 어긋나지만 결과는 같다.
 *
 * switch 안의 break 들은 **도달하지 않는다** -- 각 case 가 return 또는
 * goto 로 끝나기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수]
 *     → tegra_pcie_dw_parse_dt, tegra_pcie_get_slot_regulators,
 *       tegra_pcie_config_rp 또는 tegra_pcie_config_ep
 */
static int tegra_pcie_dw_probe(struct platform_device *pdev)
{
	const struct tegra_pcie_dw_of_data *data;
	/* [한국어] 로그와 devm 자원 획득의 기준 */
	struct device *dev = &pdev->dev;
	/* [한국어] iATU 영역 리소스 임시 변수 */
	struct resource *atu_dma_res;
	/* [한국어] 이 드라이버의 상태 */
	struct tegra_pcie_dw *pcie;
	/* [한국어] 루트 포트 구조체 포인터 */
	struct dw_pcie_rp *pp;
	/* [한국어] DWC 코어 구조체 포인터 */
	struct dw_pcie *pci;
	/* [한국어] PHY 배열 임시 변수 */
	struct phy **phys;
	/* [한국어] PHY 이름을 만들 임시 문자열 */
	char *name;
	/* [한국어] 오류 코드 보관용 */
	int ret;
	/* [한국어] PHY 순회 첨자 */
	u32 i;

	/* [한국어] **compatible 이 SoC 와 모드를 함께 정한다** */
	data = of_device_get_match_data(dev);

	/* [한국어] **브리지와 함께 잡지 않는다.** 앞 세대 pci-tegra.c 는
	 * devm_pci_alloc_host_bridge 로 브리지 뒤에 붙였지만, 여기서는 브리지
	 * 할당을 DWC 코어가 dw_pcie_host_init 안에서 한다 */
	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	/* [한국어] 할당 실패 */
	if (!pcie)
		return -ENOMEM;

	/* [한국어] **품고 있는 DWC 코어 구조체의 주소를 잡는다.** 이 한 줄에서
	 * 접착이 시작된다 */
	pci = &pcie->pci;
	/* [한국어] 코어 쪽 device 포인터. pcie->dev 와 같은 값이라 두 경로로 접근할 수 있다 */
	pci->dev = &pdev->dev;
	/* [한국어] **DWC 코어에 콜백 셋을 알려 준다.** link_up, start_link, stop_link
	 * 세 자리만 채운 구조체이며, 나머지 다섯 자리를 비워 둔 것은 코어의
	 * 기본 동작을 그대로 쓰겠다는 선언이다 */
	pci->ops = &tegra_dw_pcie_ops;
	/* [한국어] 이 드라이버 쪽 device 포인터 */
	pcie->dev = &pdev->dev;
	/* [한국어] SoC/모드 기술자를 보관한다. const 를 떼어 저장하지만 실제로 쓰지는
	 * 않으므로, const 를 유지해도 됐을 자리다 */
	pcie->of_data = (struct tegra_pcie_dw_of_data *)data;
	/* [한국어] **FTS 값을 코어 구조체로 복사한다.** 이 파일은 그 값을 직접 쓰지
	 * 않고 코어가 쓴다 */
	pci->n_fts[0] = pcie->of_data->n_fts[0];
	/* [한국어] 두 번째 FTS 값도 복사한다. Tegra234 만 80 이고 나머지는 52 다 */
	pci->n_fts[1] = pcie->of_data->n_fts[1];
	/* [한국어] 루트 포트 구조체의 주소를 잡아 둔다 */
	pp = &pci->pp;
	/* [한국어] **MSI 벡터 수를 지정한다. 이 파일의 MSI 설정이 사실상 이 한 줄이다.**
	 * 앞 세대 pci-tegra.c 가 비트맵, irq_chip, 도메인, 목적지 페이지 할당,
	 * 체인 핸들러를 직접 갖고 있던 자리를 DWC 코어의 iMSI-RX 가 대신하며,
	 * 드라이버는 개수만 알려 준다.
	 * **MAX_MSI_IRQS 의 정의는 이 스파스 체크아웃에 없어** 값을 확인하지 못했다 */
	pp->num_vectors = MAX_MSI_IRQS;

	/* [한국어] 장치 트리를 읽는다. ASPM 파라미터, 레인 수, 컨트롤러 ID, PHY 개수,
	 * 그리고 EP 모드면 PERST GPIO 까지 얻는다 */
	ret = tegra_pcie_dw_parse_dt(pcie);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 기본 로그 수준 */
		const char *level = KERN_ERR;

		/* [한국어] -EPROBE_DEFER 인지 확인한다 */
		if (ret == -EPROBE_DEFER)
			/* [한국어] 재시도 중이면 디버그 수준으로 낮춘다 */
			level = KERN_DEBUG;

		/* [한국어] dev_printk 로 수준을 골라 찍는다. dev_err_probe 와 같은 목적을
		 * 손으로 구현한 관용구다 */
		dev_printk(level, dev,
			   dev_fmt("Failed to parse device tree: %d\n"),
			   ret);
		return ret;
	}

	/* [한국어] 슬롯 전원 regulator 를 얻는다. 둘 다 없어도 된다 */
	ret = tegra_pcie_get_slot_regulators(pcie);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 기본 로그 수준 */
		const char *level = KERN_ERR;

		/* [한국어] -EPROBE_DEFER 인지 확인한다 */
		if (ret == -EPROBE_DEFER)
			/* [한국어] 재시도 중이면 로그 수준을 낮춘다 */
			level = KERN_DEBUG;

		/* [한국어] 슬롯 regulator 획득 실패를 알린다 */
		dev_printk(level, dev,
			   dev_fmt("Failed to get slot regulators: %d\n"),
			   ret);
		return ret;
	}

	/* [한국어] EP 모드에서 이 GPIO 가 있으면 설정한다 */
	if (pcie->pex_refclk_sel_gpiod)
		/* [한국어] REFCLK 소스 선택 신호를 올린다. remove 가 0 으로 내린다 */
		gpiod_set_value(pcie->pex_refclk_sel_gpiod, 1);

	/* [한국어] 컨트롤러 IO 제어 전원을 얻는다. **optional 이 아니라 필수다** */
	pcie->pex_ctl_supply = devm_regulator_get(dev, "vddio-pex-ctl");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->pex_ctl_supply)) {
		/* [한국어] 오류 코드를 꺼낸다 */
		ret = PTR_ERR(pcie->pex_ctl_supply);
		/* [한국어] 재시도 중 로그가 쌓이지 않게 한다 */
		if (ret != -EPROBE_DEFER)
			/* [한국어] -EPROBE_DEFER 가 아닐 때만 오류를 찍는다 */
			dev_err(dev, "Failed to get regulator: %ld\n",
				PTR_ERR(pcie->pex_ctl_supply));
		return ret;
	}

	/* [한국어] **코어 클록을 얻는다.** 링크 세대에 따라 주파수를 바꿀 대상이다 */
	pcie->core_clk = devm_clk_get(dev, "core");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->core_clk)) {
		/* [한국어] 실패 이유를 알린다 */
		dev_err(dev, "Failed to get core clock: %ld\n",
			PTR_ERR(pcie->core_clk));
		/* [한국어] 코어 클록 획득 실패 */
		return PTR_ERR(pcie->core_clk);
	}

	/* [한국어] 모니터 클록을 얻는다. **optional 이다** -- 아래 core 클록이 필수인
	 * 것과 대비된다 */
	pcie->core_clk_m = devm_clk_get_optional(dev, "core_m");
	/* [한국어] optional 이라 없으면 NULL 이지만 오류 포인터는 걸러야 한다 */
	if (IS_ERR(pcie->core_clk_m))
		/* [한국어] 모니터 클록 획득 실패. dev_err_probe 를 쓰면 -EPROBE_DEFER 일 때
		 * 로그가 억제된다 */
		return dev_err_probe(dev, PTR_ERR(pcie->core_clk_m),
				     "Failed to get monitor clock\n");

	/* [한국어] APPL 영역을 이름으로 찾는다 */
	pcie->appl_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						      "appl");
	/* [한국어] 영역을 못 찾았는지 확인한다 */
	if (!pcie->appl_res) {
		/* [한국어] APPL 영역이 없으면 진행할 수 없다 */
		dev_err(dev, "Failed to find \"appl\" region\n");
		return -ENODEV;
	}

	/* [한국어] **APPL 블록을 매핑한다.** 이 파일이 직접 읽고 쓰는 거의 전부가
	 * 이 주소를 통한다 */
	pcie->appl_base = devm_ioremap_resource(dev, pcie->appl_res);
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->appl_base))
		/* [한국어] 매핑 실패 */
		return PTR_ERR(pcie->appl_base);

	/* [한국어] **APB 리셋을 얻는다.** 이 리셋이 풀려야 APPL 레지스터에 접근할 수 있다 */
	pcie->core_apb_rst = devm_reset_control_get(dev, "apb");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->core_apb_rst)) {
		/* [한국어] 실패 이유를 알린다 */
		dev_err(dev, "Failed to get APB reset: %ld\n",
			PTR_ERR(pcie->core_apb_rst));
		/* [한국어] APB 리셋 획득 실패 */
		return PTR_ERR(pcie->core_apb_rst);
	}

	/* [한국어] PHY 개수만큼 배열을 잡는다 */
	phys = devm_kcalloc(dev, pcie->phy_count, sizeof(*phys), GFP_KERNEL);
	/* [한국어] 할당 실패 */
	if (!phys)
		return -ENOMEM;

	/* [한국어] 장치 트리가 알려 준 개수만큼 순회한다 */
	for (i = 0; i < pcie->phy_count; i++) {
		/* [한국어] "p2u-0", "p2u-1" 형식의 이름을 만든다. p2u 는 이 SoC 의 PHY 종류
		 * 이름으로 보이나, 그 확장의 근거는 이 트리에 없다 */
		name = kasprintf(GFP_KERNEL, "p2u-%u", i);
		/* [한국어] 할당 실패를 확인한다 */
		if (!name) {
			/* [한국어] 이름 할당 실패 */
			dev_err(dev, "Failed to create P2U string\n");
			return -ENOMEM;
		}
		/* [한국어] **이름으로 PHY 를 얻는다.** 이름 문자열은 조회에만 쓰이므로 곧바로
		 * 버려도 된다 */
		phys[i] = devm_phy_get(dev, name);
		kfree(name);
		/* [한국어] 실패를 확인한다 */
		if (IS_ERR(phys[i])) {
			/* [한국어] 오류 코드를 꺼낸다 */
			ret = PTR_ERR(phys[i]);
			/* [한국어] -EPROBE_DEFER 면 로그를 억제한다 -- 재시도 중 로그가 쌓이지 않게 하는
			 * 관용구다 */
			if (ret != -EPROBE_DEFER)
				/* [한국어] PHY 획득 실패 */
				dev_err(dev, "Failed to get PHY: %d\n", ret);
			return ret;
		}
	}

	/* [한국어] 완성된 배열을 보관한다 */
	pcie->phys = phys;

	/* [한국어] iATU 와 eDMA 영역을 이름으로 찾는다 */
	atu_dma_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   "atu_dma");
	/* [한국어] 영역을 못 찾았는지 확인한다 */
	if (!atu_dma_res) {
		/* [한국어] 이 영역이 없으면 주소 변환을 할 수 없다 */
		dev_err(dev, "Failed to find \"atu_dma\" region\n");
		return -ENODEV;
	}
	/* [한국어] 물리 주소를 알기 위해 리소스를 보관한다. 두 초기화 함수가 그 시작
	 * 주소를 APPL 레지스터에 써 넣는다 */
	pcie->atu_dma_res = atu_dma_res;

	/* [한국어] 영역 크기도 코어에 알려 준다 */
	pci->atu_size = resource_size(atu_dma_res);
	/* [한국어] **iATU 영역을 매핑해 코어 구조체에 직접 넣는다.** DBI 와 달리
	 * 이쪽은 드라이버가 매핑해 주지만, 그 안의 내용은 코어가 관리한다 */
	pci->atu_base = devm_ioremap_resource(dev, atu_dma_res);
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pci->atu_base))
		/* [한국어] 매핑 실패 */
		return PTR_ERR(pci->atu_base);

	/* [한국어] 코어 리셋을 얻는다. IP 코어 자체를 동작시키는 리셋이다 */
	pcie->core_rst = devm_reset_control_get(dev, "core");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->core_rst)) {
		/* [한국어] 실패 이유를 알린다 */
		dev_err(dev, "Failed to get core reset: %ld\n",
			PTR_ERR(pcie->core_rst));
		/* [한국어] 코어 리셋 획득 실패 */
		return PTR_ERR(pcie->core_rst);
	}

	/* [한국어] **컨트롤러 인터럽트를 DWC 코어의 필드에 직접 넣는다.**
	 * pp->irq 는 코어가 관리하는 자리인데, 이 드라이버가 채워 준다 */
	pp->irq = platform_get_irq_byname(pdev, "intr");
	/* [한국어] 실패를 확인한다 */
	if (pp->irq < 0)
		/* [한국어] IRQ 를 못 얻으면 그대로 반환한다 */
		return pp->irq;

	/* [한국어] **BPMP 핸들을 얻는다.** devm 이 아니라 직접 얻으므로 remove 와 실패
	 * 경로가 tegra_bpmp_put 을 불러야 한다 -- 이 파일에서 devm 이 아닌
	 * 유일한 자원이다 */
	pcie->bpmp = tegra_bpmp_get(dev);
	if (IS_ERR(pcie->bpmp))
		/* [한국어] BPMP 를 못 얻으면 -EPROBE_DEFER 일 수 있다 */
		return PTR_ERR(pcie->bpmp);

	/* [한국어] 플랫폼 장치에 드라이버 상태를 매단다. PM 콜백이 dev_get_drvdata 로
	 * 되찾는다 */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] **대역폭 요청 경로를 얻는다.** 이름이 "write" 인 것 하나뿐이며,
	 * 읽기 방향을 따로 요청하지 않는 이유는 코드에 적혀 있지 않다 */
	pcie->icc_path = devm_of_icc_get(&pdev->dev, "write");
	/* [한국어] 오류 포인터를 정수 오류 코드로 바꾼다. NULL 이나 유효 포인터면 0 이다 */
	ret = PTR_ERR_OR_ZERO(pcie->icc_path);
	/* [한국어] 실패를 확인한다 */
	if (ret) {
		tegra_bpmp_put(pcie->bpmp);
		/* [한국어] interconnect 획득 실패. **fail 라벨을 쓰지 않고 직접 BPMP 를 놓는다** --
		 * 형태가 아래 switch 의 오류 처리와 어긋나지만 결과는 같다 */
		dev_err_probe(&pdev->dev, ret, "failed to get write interconnect\n");
		return ret;
	}

	/* [한국어] **모드에 따라 인터럽트 방식과 기동 경로가 갈린다.**
	 * RC 는 일반 핸들러 하나, EP 는 하드+스레드 짝이다 -- EP 쪽이 링크가
	 * 올라온 뒤 잠들 수 있는 작업을 해야 하기 때문이다 */
	switch (pcie->of_data->mode) {
	/* [한국어] 루트 포트 모드 */
	case DW_PCIE_RC_TYPE:
		ret = devm_request_irq(dev, pp->irq, tegra_pcie_rp_irq_handler,
				       IRQF_SHARED, "tegra-pcie-intr", pcie);
		/* [한국어] 실패를 확인한다 */
		if (ret) {
			/* [한국어] IRQ 요청 실패 */
			dev_err(dev, "Failed to request IRQ %d: %d\n", pp->irq,
				ret);
			goto fail;
		}

		/* [한국어] RC 는 여기서 하드웨어를 켜고 링크까지 세운다 */
		ret = tegra_pcie_config_rp(pcie);
		/* [한국어] **-ENOMEDIUM 은 오류로 보지 않는다.** 링크가 없어도 드라이버는 붙은
		 * 채로 남으며, link_state 가 false 라 이후 모든 경로가 곧바로 돌아간다 */
		if (ret && ret != -ENOMEDIUM)
			goto fail;
		else
			return 0;
		break;

	/* [한국어] 엔드포인트 모드 */
	case DW_PCIE_EP_TYPE:
		ret = devm_request_threaded_irq(dev, pp->irq,
						tegra_pcie_ep_hard_irq,
						tegra_pcie_ep_irq_thread,
						IRQF_SHARED,
						"tegra-pcie-ep-intr", pcie);
		/* [한국어] 실패를 확인한다 */
		if (ret) {
			/* [한국어] IRQ 요청 실패 */
			dev_err(dev, "Failed to request IRQ %d: %d\n", pp->irq,
				ret);
			goto fail;
		}

		/* [한국어] **EP 는 준비만 한다.** 하드웨어 기동은 호스트가 PERST 를 풀 때
		 * pex_ep_event_pex_rst_deassert 가 하며, RC 경로와 구조가 뒤집혀 있다 */
		ret = tegra_pcie_config_ep(pcie, pdev);
		/* [한국어] EP 준비 실패면 BPMP 를 놓고 끝낸다 */
		if (ret < 0)
			goto fail;
		else
			return 0;
		break;

	default:
		dev_err(dev, "Invalid PCIe device type %d\n",
			pcie->of_data->mode);
		ret = -EINVAL;
	}

fail:
	tegra_bpmp_put(pcie->bpmp);
	return ret;
}

/* [한국어]
 * tegra_pcie_dw_remove - 드라이버를 떼어 낸다
 *
 * @pdev: 제거되는 플랫폼 장치.
 * @return: 없음.
 *
 * 모드에 따라 정리가 갈린다.
 *
 * RC 모드:
 *   **링크가 없었으면 곧바로 돌아간다.** probe 가 -ENOMEDIUM 을 성공으로
 *   처리했으므로 링크 없이도 드라이버가 붙어 있을 수 있는데, 그 경우
 *   기동한 것이 없어 정리할 것도 없다. link_state 가 그 스위치다.
 *   링크가 있었으면 debugfs, 코어와 하드웨어, runtime PM 을 정리한다.
 *
 * EP 모드:
 *   PERST 인터럽트를 끄고, 엔드포인트를 허물고, DWC EP 를 해제한다.
 *   **PERST 가 실제로 눌리지 않았어도 assert 경로를 부른다** -- 같은
 *   정리가 필요하기 때문이며, 그 함수가 상태를 확인해 중복을 막는다.
 *
 * 공통 정리 셋 -- runtime PM 끄기, BPMP 놓기, REFCLK 선택 GPIO 내리기 --
 * 는 마지막에 함께 한다. 다만 **RC 경로가 링크 없이 조기 반환하면 이
 * 셋도 실행되지 않는다.** BPMP 핸들이 남는 셈인데, 코드가 그렇게 되어
 * 있으므로 사실만 적어 둔다.
 *
 * 앞 세대 pci-tegra.c 에는 remove 함수가 아예 없다 -- builtin 드라이버이고
 * suppress_bind_attrs 로 언바인드를 막았기 때문이다. 이 드라이버는
 * 모듈로 뺄 수 있어 정리 경로가 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 버스(언바인드) → [이 함수]
 *     → tegra_pcie_deinit_controller 또는 pex_ep_event_pex_rst_assert,
 *       dw_pcie_ep_deinit, tegra_bpmp_put
 */
static void tegra_pcie_dw_remove(struct platform_device *pdev)
{
	struct tegra_pcie_dw *pcie = platform_get_drvdata(pdev);
	/* [한국어] EP 해제에 쓸 DWC 엔드포인트 구조체 */
	struct dw_pcie_ep *ep = &pcie->pci.ep;

	/* [한국어] 모드에 따라 정리가 갈린다 */
	if (pcie->of_data->mode == DW_PCIE_RC_TYPE) {
		/* [한국어] **링크가 없었으면 곧바로 돌아간다.** probe 가 -ENOMEDIUM 을 성공으로
		 * 처리하므로 링크 없이도 드라이버가 붙어 있을 수 있는데, 그 경우
		 * 기동한 것이 없다.
		 * 주의: 여기서 반환하면 아래의 공통 정리(runtime PM, BPMP 놓기)도
		 * 실행되지 않아 BPMP 핸들이 남는다. 코드가 그렇게 되어 있으므로
		 * 사실만 적어 둔다 */
		if (!pcie->link_state)
			return;

		debugfs_remove_recursive(pcie->debugfs);
		tegra_pcie_deinit_controller(pcie);
		pm_runtime_put_sync(pcie->dev);
	} else {
		disable_irq(pcie->pex_rst_irq);
		pex_ep_event_pex_rst_assert(pcie);
		dw_pcie_ep_deinit(ep);
	}

	pm_runtime_disable(pcie->dev);
	tegra_bpmp_put(pcie->bpmp);
	/* [한국어] REFCLK 선택 GPIO 를 내린다. probe 가 올린 것의 짝이다 */
	if (pcie->pex_refclk_sel_gpiod)
		gpiod_set_value(pcie->pex_refclk_sel_gpiod, 0);
}

/* [한국어]
 * tegra_pcie_dw_suspend - 절전 첫 단계. EP 모드의 절전 가능 여부를 판정한다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 허용. EP 가 동작 중이면 -EPERM.
 *
 * **EP 모드에서만 의미가 있다.** RC 모드는 곧바로 0 을 돌려준다.
 *
 * EP 가 활성 상태(EP_STATE_ENABLED)면 **절전을 거부한다.** 호스트와
 * 링크를 맺고 동작 중인 엔드포인트가 잠들면 호스트 쪽에서 장치가
 * 사라진 것으로 보이기 때문이다. -EPERM 을 돌려주면 PM 코어가 시스템
 * 절전 전체를 중단한다.
 *
 * 활성이 아니면 PERST 인터럽트만 끄고 허용한다. 절전 중에 호스트가
 * PERST 를 흔들어도 반응하지 않게 하는 것이며,
 * tegra_pcie_dw_resume_early 가 다시 켠다.
 *
 * 이 드라이버의 PM 콜백은 다섯 단계에 걸쳐 있는데(suspend, suspend_late,
 * suspend_noirq, resume_noirq, resume_early), 각 단계가 서로 다른 일을
 * 맡는다. 이 함수는 가장 이른 단계로, 아직 자식 장치들이 살아 있을 때다.
 *
 * **앞 세대와의 대비**: pci-tegra.c 는 콜백이 둘뿐이고(suspend, resume)
 * 그것을 런타임과 시스템 절전 양쪽에 등록했다. 여기서는 다섯 단계로
 * 나뉘고 런타임 PM 콜백은 등록하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 절전 초기 단계).
 *
 * 호출 체인:
 *   PM 코어 → tegra_pcie_dw_pm_ops.suspend → [이 함수]
 */
static int tegra_pcie_dw_suspend(struct device *dev)
{
	struct tegra_pcie_dw *pcie = dev_get_drvdata(dev);

	/* [한국어] **이 콜백은 EP 모드에서만 의미가 있다** */
	if (pcie->of_data->mode == DW_PCIE_EP_TYPE) {
		/* [한국어] 엔드포인트가 활성 상태인지 확인한다 */
		if (pcie->ep_state == EP_STATE_ENABLED) {
			/* [한국어] **동작 중인 엔드포인트는 절전할 수 없다.** 호스트와 링크를 맺고
			 * 있는데 잠들면 호스트 쪽에서 장치가 사라진 것으로 보이기 때문이다.
			 * -EPERM 을 돌려주면 PM 코어가 시스템 절전 전체를 중단한다 */
			dev_err(dev, "Tegra PCIe is in EP mode, suspend not allowed\n");
			return -EPERM;
		}

		disable_irq(pcie->pex_rst_irq);
		return 0;
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_dw_suspend_late - 절전 전에 하드웨어 핫 리셋 모드를 켠다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0.
 *
 * 링크가 없으면 곧바로 돌아간다 -- link_state 가 여기서도 스위치다.
 *
 * **has_sbr_reset_fix 가 없는 SoC(Tegra194)에서만** 핫 리셋 모드를 켠다.
 * 모드 필드를 지우고 활성화 비트만 세우는데, 필드를 0 으로 두는 것이
 * tegra_pcie_dw_resume_early 가 IMDT_RST 로 되돌리는 것과 대비된다.
 *
 * 이 설정의 목적이 코드에 적혀 있지 않다. 확실한 것은
 * tegra_pcie_dw_resume_early 가 정확히 반대 동작을 한다는 것 --
 * 그쪽은 모드를 IMDT_RST 로 놓고 활성화 비트를 **지운다.** 즉 절전
 * 구간에만 이 모드를 켜 두는 것이며, 수정된 SoC(Tegra234)는 필요 없다.
 *
 * **EP 모드 검사가 없다는 점에 주의.** 다른 PM 콜백들은 모드를 먼저
 * 확인하지만 이 함수는 link_state 만 본다. EP 모드에서는 link_state 가
 * 설정되지 않아(tegra_pcie_config_rp 만 채운다) 0 으로 남으므로 결과적으로
 * 곧바로 반환한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(suspend 다음 단계).
 *
 * 호출 체인:
 *   PM 코어 → tegra_pcie_dw_pm_ops.suspend_late → [이 함수]
 */
static int tegra_pcie_dw_suspend_late(struct device *dev)
{
	struct tegra_pcie_dw *pcie = dev_get_drvdata(dev);
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* [한국어] 링크가 없었으면 설정할 것도 없다.
	 * **EP 모드 검사가 없는 데 주의** -- 다른 PM 콜백들은 모드를 먼저
	 * 확인하지만 이 함수는 link_state 만 본다. EP 에서는 그 값이 채워지지
	 * 않아 0 으로 남으므로 결과적으로 곧바로 반환한다 */
	if (!pcie->link_state)
		return 0;

	/* Enable HW_HOT_RST mode */
	if (!pcie->of_data->has_sbr_reset_fix) {
		/* [한국어] 현재 값을 읽는다 */
		val = appl_readl(pcie, APPL_CTRL);
		/* [한국어] 모드 필드를 지운다 */
		val &= ~(APPL_CTRL_HW_HOT_RST_MODE_MASK <<
			 APPL_CTRL_HW_HOT_RST_MODE_SHIFT);
		/* [한국어] **활성화 비트만 세우고 모드 필드는 0 으로 둔다.**
		 * tegra_pcie_dw_resume_early 가 정확히 반대로 -- 모드를 IMDT_RST 로 놓고
		 * 활성화 비트를 지운다 -- 하므로, 절전 구간에만 이 설정을 쓰는 셈이다 */
		val |= APPL_CTRL_HW_HOT_RST_EN;
		/* [한국어] 설정을 쓴다 */
		appl_writel(pcie, val, APPL_CTRL);
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_dw_suspend_noirq - 링크를 내리고 하드웨어 전원을 끊는다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0.
 *
 * **실질적인 절전이 여기서 일어난다.** noirq 단계인 것은 자식 장치들이
 * 이미 잠들어 링크를 내려도 안전하고, 인터럽트가 꺼져 있어 링크 단절이
 * 유발하는 AER 을 걱정하지 않아도 되기 때문이다.
 *
 * EP 모드나 링크가 없으면 곧바로 돌아간다.
 *
 * 세 줄이 전부다 -- 모니터 클록을 끄고, 링크를 L2 로 내리고, 하드웨어
 * 전원을 끊는다.
 *
 * **tegra_pcie_deinit_controller 와 한 줄이 다르다.** 그쪽은
 * dw_pcie_host_deinit 을 불러 DWC 코어의 호스트 구조까지 허물지만,
 * 여기서는 부르지 않는다. 절전에서는 버스와 도메인을 유지해야 깨어날 때
 * 다시 만들 필요가 없기 때문이며, 그 한 줄의 차이가 "해제" 와 "절전" 을
 * 가른다.
 *
 * 그래서 tegra_pcie_dw_resume_noirq 도 dw_pcie_host_init 을 부르지 않고
 * host_init 콜백과 setup_rc 와 start_link 를 직접 부른다 -- 코어가
 * 이미 알고 있는 것을 다시 만들지 않기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 꺼짐.
 *
 * 호출 체인:
 *   PM 코어 → tegra_pcie_dw_pm_ops.suspend_noirq → [이 함수]
 *     → tegra_pcie_dw_pme_turnoff, tegra_pcie_unconfig_controller
 */
static int tegra_pcie_dw_suspend_noirq(struct device *dev)
{
	struct tegra_pcie_dw *pcie = dev_get_drvdata(dev);

	/* [한국어] EP 모드는 이 단계에서 할 일이 없다 */
	if (pcie->of_data->mode == DW_PCIE_EP_TYPE)
		return 0;

	/* [한국어] 링크가 없었으면 내릴 것도 없다 */
	if (!pcie->link_state)
		return 0;

	clk_disable_unprepare(pcie->core_clk_m);
	tegra_pcie_dw_pme_turnoff(pcie);
	tegra_pcie_unconfig_controller(pcie);

	return 0;
}

/* [한국어]
 * tegra_pcie_dw_resume_noirq - 하드웨어를 다시 켜고 링크를 세운다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 성공, 음수 오류.
 *
 * tegra_pcie_dw_suspend_noirq 의 역순이되, **DWC 코어를 다시 초기화하지
 * 않는다.**
 *
 *   1) 하드웨어를 켠다. **en_hw_hot_rst 에 true 를 준다** -- probe 가
 *      false 를 주는 것과 다른데, 절전에서 깨어날 때는 핫 리셋 모드가
 *      필요하다는 뜻으로 보이나 그 이유가 코드에 적혀 있지는 않다.
 *   2) host_init 콜백을 직접 부른다. 브리지 창 디코딩, RRS 형식,
 *      이퀄라이저, ASPM 을 다시 설정한다 -- 전원이 끊겨 레지스터가
 *      초기화되었기 때문이다.
 *   3) dw_pcie_setup_rc 로 코어 쪽 루트 포트 설정을 다시 시킨다.
 *   4) start_link 로 링크를 세운다.
 *
 * **2)~4)가 dw_pcie_host_init 이 내부에서 하던 일의 일부** 다. 그 함수를
 * 통째로 다시 부르지 않는 것은 도메인과 버스를 새로 만들면 안 되기
 * 때문이며, 필요한 하드웨어 설정만 골라 다시 하는 것이다. 이것이
 * suspend_noirq 가 dw_pcie_host_deinit 을 부르지 않은 것과 짝을 이룬다.
 *
 * EP 모드나 링크가 없었으면 곧바로 돌아간다.
 *
 * 실패하면 하드웨어를 되돌린다. 2)와 4) 어느 쪽이 실패해도 같은 라벨로
 * 간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 인터럽트 꺼짐.
 *
 * 호출 체인:
 *   PM 코어 → tegra_pcie_dw_pm_ops.resume_noirq → [이 함수]
 *     → tegra_pcie_config_controller, tegra_pcie_dw_host_init,
 *       dw_pcie_setup_rc, tegra_pcie_dw_start_link
 */
static int tegra_pcie_dw_resume_noirq(struct device *dev)
{
	struct tegra_pcie_dw *pcie = dev_get_drvdata(dev);
	/* [한국어] 오류 코드 보관용 */
	int ret;

	/* [한국어] EP 모드는 복귀 시 할 일이 없다 -- 호스트가 PERST 를 풀면
	 * GPIO 인터럽트가 알아서 처리한다 */
	if (pcie->of_data->mode == DW_PCIE_EP_TYPE)
		return 0;

	/* [한국어] 링크가 없었으면 다시 세울 것도 없다 */
	if (!pcie->link_state)
		return 0;

	/* [한국어] **en_hw_hot_rst 에 true 를 준다** -- probe 경로가 false 를 주는 것과
	 * 다르다. 절전 복귀에는 핫 리셋 모드가 필요하다는 뜻으로 보이나,
	 * 그 이유가 코드에 적혀 있지는 않다 */
	ret = tegra_pcie_config_controller(pcie, true);
	/* [한국어] 하드웨어를 못 켜면 여기서 끝낸다 */
	if (ret < 0)
		return ret;

	/* [한국어] **host_init 콜백을 직접 부른다.** 전원이 끊겨 레지스터가 초기화되었으므로
	 * 브리지 창 디코딩, RRS 형식, 이퀄라이저, ASPM 을 다시 설정해야 한다 */
	ret = tegra_pcie_dw_host_init(&pcie->pci.pp);
	/* [한국어] 실패를 확인한다 */
	if (ret < 0) {
		/* [한국어] 호스트 설정 실패 */
		dev_err(dev, "Failed to init host: %d\n", ret);
		goto fail_host_init;
	}

	dw_pcie_setup_rc(&pcie->pci.pp);

	/* [한국어] **start_link 를 직접 부른다.** 정상 경로에서는 DWC 코어가 콜백으로
	 * 부르지만, 여기서는 dw_pcie_host_init 을 거치지 않으므로 손으로 부른다 */
	ret = tegra_pcie_dw_start_link(&pcie->pci);
	/* [한국어] 링크 기동 실패면 하드웨어를 되돌린다 */
	if (ret < 0)
		goto fail_host_init;

	return 0;

fail_host_init:
	tegra_pcie_unconfig_controller(pcie);
	return ret;
}

/* [한국어]
 * tegra_pcie_dw_resume_early - 핫 리셋 모드를 되돌리고 EP 인터럽트를 켠다
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 항상 0.
 *
 * **EP 모드에서는 PERST 인터럽트를 켜고 끝난다.**
 * tegra_pcie_dw_suspend 가 끈 것의 짝이다. 이제 다시 호스트의 PERST 를
 * 들을 수 있다.
 *
 * RC 모드에서는 link_state 를 확인한 뒤, has_sbr_reset_fix 가 없는
 * SoC 에서만 핫 리셋 모드를 되돌린다.
 * tegra_pcie_dw_suspend_late 와 정확히 반대다 --
 *   suspend_late : 모드 필드를 0 으로, 활성화 비트를 **세움**
 *   이 함수      : 모드 필드를 IMDT_RST 로, 활성화 비트를 **지움**
 * 즉 절전 구간 동안만 다른 설정을 쓰는 구조다.
 *
 * **단계 배치에 주의**: 이 함수는 resume_early 이므로 resume_noirq 보다
 * **나중** 에 불린다. 즉 하드웨어가 이미 켜지고 링크가 선 뒤에 이
 * 설정을 되돌린다. 대칭적으로 suspend_late 는 suspend_noirq 보다 먼저,
 * 즉 하드웨어가 살아 있을 때 설정한다. 두 함수 모두 APPL 레지스터에
 * 접근하므로 전원이 있어야 하는데, 그 조건이 양쪽에서 지켜진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(resume_noirq 다음 단계).
 *
 * 호출 체인:
 *   PM 코어 → tegra_pcie_dw_pm_ops.resume_early → [이 함수]
 */
static int tegra_pcie_dw_resume_early(struct device *dev)
{
	struct tegra_pcie_dw *pcie = dev_get_drvdata(dev);
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;

	/* [한국어] **EP 모드는 PERST 인터럽트를 다시 켜고 끝난다.**
	 * tegra_pcie_dw_suspend 가 끈 것의 짝이며, 이제 다시 호스트의 신호를
	 * 들을 수 있다 */
	if (pcie->of_data->mode == DW_PCIE_EP_TYPE) {
		enable_irq(pcie->pex_rst_irq);
		return 0;
	}

	/* [한국어] 링크가 없었으면 되돌릴 설정도 없다 */
	if (!pcie->link_state)
		return 0;

	/* Disable HW_HOT_RST mode */
	if (!pcie->of_data->has_sbr_reset_fix) {
		/* [한국어] 현재 값을 읽는다 */
		val = appl_readl(pcie, APPL_CTRL);
		/* [한국어] 모드 필드를 지운다 */
		val &= ~(APPL_CTRL_HW_HOT_RST_MODE_MASK <<
			 APPL_CTRL_HW_HOT_RST_MODE_SHIFT);
		/* [한국어] 모드를 즉시 리셋으로 되돌린다 */
		val |= APPL_CTRL_HW_HOT_RST_MODE_IMDT_RST <<
		       APPL_CTRL_HW_HOT_RST_MODE_SHIFT;
		/* [한국어] 필드 자리로 민다 */
		val &= ~APPL_CTRL_HW_HOT_RST_EN;
		/* [한국어] 되돌린 값을 쓴다 */
		appl_writel(pcie, val, APPL_CTRL);
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_dw_shutdown - 시스템 종료 시 컨트롤러를 안전하게 멈춘다
 *
 * @pdev: 종료되는 플랫폼 장치.
 * @return: 없음.
 *
 * 재부팅이나 전원 종료 직전에 불린다. 하드웨어를 안전한 상태로 두어
 * 다음 부팅이나 킥스택 커널이 깨끗하게 시작할 수 있게 한다.
 *
 * RC 모드:
 *   링크가 없으면 곧바로 돌아간다.
 *   debugfs 를 지우고, **인터럽트를 명시적으로 끈다** -- 컨트롤러
 *   인터럽트와 MSI 인터럽트 둘 다다. 종료 중에 인터럽트가 올라오면
 *   이미 해제된 자료구조를 건드릴 수 있기 때문이다.
 *   그다음 링크를 내리고 하드웨어를 끄고 runtime PM 을 놓는다.
 *
 * EP 모드:
 *   PERST 인터럽트를 끄고 엔드포인트를 허문다.
 *
 * **tegra_pcie_dw_remove 와 거의 같되 두 가지가 다르다.**
 *   이 함수는 인터럽트를 직접 끄지만 remove 는 그러지 않는다 --
 *     remove 는 devm 이 정리해 주지만, shutdown 은 드라이버가 떨어지는
 *     것이 아니라 자료구조가 그대로 남기 때문이다.
 *   이 함수는 dw_pcie_host_deinit / dw_pcie_ep_deinit 을 부르지 않는다 --
 *     종료 중이라 소프트웨어 구조를 정리할 필요가 없기 때문이다.
 *   또한 runtime PM 을 끄지도(pm_runtime_disable) BPMP 를 놓지도 않는다.
 *
 * pp.msi_irq[0] 에 접근하는 것이 이 파일에서 MSI 관련 필드를 직접
 * 건드리는 유일한 곳이다. DWC 코어가 만든 MSI 인터럽트를 끄기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 종료).
 *
 * 호출 체인:
 *   커널 종료 경로 → 플랫폼 버스 → [이 함수]
 *     → tegra_pcie_dw_pme_turnoff, tegra_pcie_unconfig_controller
 *       또는 pex_ep_event_pex_rst_assert
 */
static void tegra_pcie_dw_shutdown(struct platform_device *pdev)
{
	struct tegra_pcie_dw *pcie = platform_get_drvdata(pdev);

	/* [한국어] 종료 정리도 모드에 따라 갈린다 */
	if (pcie->of_data->mode == DW_PCIE_RC_TYPE) {
		/* [한국어] 링크가 없었으면 기동한 것이 없으므로 정리할 것도 없다 */
		if (!pcie->link_state)
			return;

		debugfs_remove_recursive(pcie->debugfs);

		disable_irq(pcie->pci.pp.irq);
		/* [한국어] **MSI 인터럽트도 끈다.** pp.msi_irq[0] 에 접근하는 것이 이 파일에서
		 * DWC 코어의 MSI 관련 필드를 직접 건드리는 유일한 곳이다 --
		 * 코어가 만든 인터럽트를 종료 전에 막기 위해서다 */
		if (IS_ENABLED(CONFIG_PCI_MSI))
			disable_irq(pcie->pci.pp.msi_irq[0]);

		tegra_pcie_dw_pme_turnoff(pcie);
		tegra_pcie_unconfig_controller(pcie);
		pm_runtime_put_sync(pcie->dev);
	} else {
		disable_irq(pcie->pex_rst_irq);
		pex_ep_event_pex_rst_assert(pcie);
	}
}

/* [한국어] Tegra194 루트 포트 모드용 기술자 */
static const struct tegra_pcie_dw_of_data tegra194_pcie_dw_rc_of_data = {
	/* [한국어] Tegra194 RC. **결함 플래그가 하나도 없다** -- 즉 이 파일의 모든
	 * 우회 경로를 타는 기준선이며, 뒷세대 기술자와 견주면 무엇이 고쳐졌는지가
	 * 그대로 드러난다 */
	.version = TEGRA194_DWC_IP_VER,
	.mode = DW_PCIE_RC_TYPE,
	.cdm_chk_int_en_bit = BIT(19),
	/* Gen4 - 5, 6, 8 and 9 presets enabled */
	.gen4_preset_vec = 0x360,
	.n_fts = { 52, 52 },
};

/* [한국어] Tegra194 엔드포인트 모드용 기술자 */
static const struct tegra_pcie_dw_of_data tegra194_pcie_dw_ep_of_data = {
	/* [한국어] Tegra194 EP. RC 와 값이 **모드만 빼고 완전히 같다** --
	 * 세대 차이가 모드 차이보다 크다는 뜻이다 */
	.version = TEGRA194_DWC_IP_VER,
	.mode = DW_PCIE_EP_TYPE,
	.cdm_chk_int_en_bit = BIT(19),
	/* Gen4 - 5, 6, 8 and 9 presets enabled */
	.gen4_preset_vec = 0x360,
	.n_fts = { 52, 52 },
};

/* [한국어] Tegra234 루트 포트 모드용 기술자 */
static const struct tegra_pcie_dw_of_data tegra234_pcie_dw_rc_of_data = {
	/* [한국어] Tegra234 RC. **세 결함이 모두 고쳐진 유일한 조합이다** --
	 * has_msix_doorbell_access_fix, has_sbr_reset_fix, has_l1ss_exit_fix 가
	 * 모두 true 다. 그만큼 우회 코드를 타지 않는다 */
	.version = TEGRA234_DWC_IP_VER,
	.mode = DW_PCIE_RC_TYPE,
	.has_msix_doorbell_access_fix = true,
	.has_sbr_reset_fix = true,
	.has_l1ss_exit_fix = true,
	.cdm_chk_int_en_bit = BIT(18),
	/* Gen4 - 6, 8 and 9 presets enabled */
	.gen4_preset_vec = 0x340,
	.n_fts = { 52, 80 },
};

/* [한국어] Tegra234 엔드포인트 모드용 기술자 */
static const struct tegra_pcie_dw_of_data tegra234_pcie_dw_ep_of_data = {
	/* [한국어] Tegra234 EP. **가장 많은 우회 플래그를 갖는다** --
	 * has_l1ss_exit_fix, has_ltr_req_fix, disable_l1_2 셋이다.
	 * 특히 disable_l1_2 는 고칠 수 없는 하드웨어 결함 때문에 L1.2 능력
	 * 광고 자체를 끄는 것으로, init_host_aspm 안의 원문 주석이 상세히 밝힌다 */
	.version = TEGRA234_DWC_IP_VER,
	.mode = DW_PCIE_EP_TYPE,
	.has_l1ss_exit_fix = true,
	.has_ltr_req_fix = true,
	.disable_l1_2 = true,
	.cdm_chk_int_en_bit = BIT(18),
	/* Gen4 - 6, 8 and 9 presets enabled */
	.gen4_preset_vec = 0x340,
	.n_fts = { 52, 80 },
};

/* [한국어] 이 드라이버가 다루는 SoC 와 모드의 조합 */
static const struct of_device_id tegra_pcie_dw_of_match[] = {
	{
		/* [한국어] Tegra194 RC. **compatible 네 개가 RC/EP × 두 세대에 대응한다** --
		 * 즉 장치 트리의 compatible 하나가 SoC 와 동작 모드를 함께 정한다 */
		.compatible = "nvidia,tegra194-pcie",
		.data = &tegra194_pcie_dw_rc_of_data,
	},
	{
		/* [한국어] Tegra194 EP */
		.compatible = "nvidia,tegra194-pcie-ep",
		.data = &tegra194_pcie_dw_ep_of_data,
	},
	{
		/* [한국어] Tegra234 RC */
		.compatible = "nvidia,tegra234-pcie",
		.data = &tegra234_pcie_dw_rc_of_data,
	},
	{
		/* [한국어] Tegra234 EP */
		.compatible = "nvidia,tegra234-pcie-ep",
		.data = &tegra234_pcie_dw_ep_of_data,
	},
	{}
};

/* [한국어] 시스템 절전 콜백 등록 */
static const struct dev_pm_ops tegra_pcie_dw_pm_ops = {
	/* [한국어] **절전 콜백이 다섯 단계에 걸쳐 있다.** 앞 세대가 suspend/resume 둘로
	 * 끝내고 그것을 런타임과 시스템 절전 양쪽에 등록한 것과 크게 다르다.
	 * 각 단계가 맡는 일이 나뉘어 있다 -- suspend 는 EP 절전 허용 판정,
	 * suspend_late 는 핫 리셋 모드 설정, suspend_noirq 는 실제 전원 차단,
	 * resume_noirq 는 재기동, resume_early 는 핫 리셋 모드 복원이다.
	 * **런타임 PM 콜백은 등록하지 않는다** */
	.suspend = tegra_pcie_dw_suspend,
	.suspend_late = tegra_pcie_dw_suspend_late,
	.suspend_noirq = tegra_pcie_dw_suspend_noirq,
	.resume_noirq = tegra_pcie_dw_resume_noirq,
	.resume_early = tegra_pcie_dw_resume_early,
};

/* [한국어] 플랫폼 드라이버 정의. 아래 module_platform_driver 로 등록되므로
 * 모듈로 뺄 수 있다 */
static struct platform_driver tegra_pcie_dw_driver = {
	/* [한국어] probe/remove/shutdown 셋을 모두 등록한다. **앞 세대 pci-tegra.c 가
	 * probe 하나만 두고 suppress_bind_attrs 로 해제를 막은 것과 대비된다** */
	.probe = tegra_pcie_dw_probe,
	.remove = tegra_pcie_dw_remove,
	.shutdown = tegra_pcie_dw_shutdown,
	.driver = {
		/* [한국어] 드라이버 이름. **파일명과 달리 tegra234 를 포함하지 않는다** --
		 * 처음 Tegra194 용으로 만들어진 뒤 뒷세대를 같은 드라이버에 더한 흔적이다 */
		.name	= "tegra194-pcie",
		.pm = &tegra_pcie_dw_pm_ops,
		.of_match_table = tegra_pcie_dw_of_match,
	},
};
module_platform_driver(tegra_pcie_dw_driver);

MODULE_DEVICE_TABLE(of, tegra_pcie_dw_of_match);

MODULE_AUTHOR("Vidya Sagar <vidyas@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA PCIe host controller driver");
MODULE_LICENSE("GPL v2");
