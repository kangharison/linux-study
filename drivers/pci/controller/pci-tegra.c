// SPDX-License-Identifier: GPL-2.0+
/*
 * PCIe host controller driver for Tegra SoCs
 *
 * Copyright (c) 2010, CompuLab, Ltd.
 * Author: Mike Rapoport <mike@compulab.co.il>
 *
 * Based on NVIDIA PCIe driver
 * Copyright (c) 2008-2009, NVIDIA Corporation.
 *
 * Bits taken from arch/arm/mach-dove/pcie.c
 *
 * Author: Thierry Reding <treding@nvidia.com>
 */

/*
 * [한국어 설명] NVIDIA Tegra 자체 PCIe IP 호스트 브리지 드라이버 (pci-tegra.c)
 *
 * === 파일의 역할 ===
 * Tegra20 / Tegra30 / Tegra124 / Tegra210 / Tegra186 에 들어 있는 NVIDIA
 * 자체 설계 PCIe 컨트롤러를 구동해, SoC 를 PCIe 루트 컴플렉스로 만든다.
 * 이 드라이버가 담당하는 일은 네 갈래다 -- 전원과 클록과 리셋을 순서대로
 * 올려 컨트롤러를 깨우고, 내장 PHY(PADS 블록)의 PLL 을 잠그고, CPU 주소와
 * PCIe 주소 사이의 변환 창을 프로그래밍하고, config 접근과 인터럽트(INTx,
 * MSI, 그리고 컨트롤러 자신의 오류 인터럽트)를 PCI 코어에 연결한다.
 * 라이선스한 IP 를 쓰지 않으므로 **이 파일 하나에 컨트롤러의 모든 것이
 * 들어 있다.** 같은 벤더가 뒷세대에서 DesignWare IP 로 갈아탄 뒤의 모습은
 * pcie-tegra194.c 이며, 그쪽은 config 접근도 MSI 도 자기 코드가 아니다.
 * 두 파일을 나란히 읽으면 "자체 IP 를 버리면 드라이버에서 무엇이 사라지는가"
 * 가 그대로 보인다 -- 아래 부가 절에서 항목별로 짚는다.
 *
 * 이 컨트롤러는 루트 포트를 여러 개(SoC 에 따라 2개 또는 3개) 품고 있고,
 * 그 포트들에 레인을 어떻게 나눠 줄지가 장치 트리로 정해진다. 그래서 이
 * 파일에는 다른 호스트 브리지 드라이버에 없는 "레인 배분(xbar) 설정" 이라는
 * 단계가 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 PCI 계층에서 이 파일은 **호스트 브리지 드라이버** 자리에 있다.
 * 위로는 PCI 코어(drivers/pci/probe.c 의 버스 열거, drivers/pci/access.c 의
 * config 접근)가 이 파일이 채운 struct pci_ops 를 통해 하드웨어에 닿고,
 * 아래로는 클록/리셋/regulator/PHY/GPIO 프레임워크와 Tegra SoC 고유
 * 서비스(전원 게이트, cpuidle)를 부른다.
 *
 * 이 컨트롤러는 PCI 장치가 아니라 **플랫폼 장치** 로 등록된다. 자기 자신이
 * PCI 버스를 만들어 주는 쪽이므로 PCI 버스 위에 존재할 수 없기 때문이다.
 * 장치 트리의 compatible 이 tegra_pcie_of_match 의 한 항목과 맞으면
 * tegra_pcie_probe 가 불린다.
 *
 * 부팅 시 호출 흐름:
 *   플랫폼 버스
 *     → tegra_pcie_probe
 *         → tegra_pcie_parse_dt        (루트 포트 목록, 레인 배분, regulator)
 *         → tegra_pcie_get_resources   (클록/리셋/PHY/레지스터 창/오류 IRQ)
 *         → tegra_pcie_msi_setup       (MSI 도메인, MSI 목적지 페이지)
 *         → pm_runtime_get_sync
 *             → tegra_pcie_pm_resume   (아래의 하드웨어 기동 전부가 여기 있다)
 *         → pci_host_probe             (PCI 코어가 버스를 훑기 시작)
 *
 * **하드웨어 기동이 probe 가 아니라 runtime PM 의 resume 콜백에 들어 있는
 * 것이 이 드라이버의 구조적 특징이다.** tegra_pcie_pm_resume 하나가
 * 전원 인가, 컨트롤러 설정, 변환 창 설정, MSI 활성화, PEX 클록, PHY 전원,
 * 포트 기동을 순서대로 수행하며, probe 와 시스템 복귀가 같은 경로를 탄다.
 * 그래서 절전에서 깨어날 때 별도의 재설정 코드가 필요 없다.
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트(probe 와 PM 콜백)다. 예외는 두
 * 인터럽트 경로인데, tegra_pcie_isr 은 스레드가 아닌 일반 핸들러이고
 * tegra_pcie_msi_irq 는 체인 핸들러다. irq_chip 콜백들(마스크/언마스크/ack)은
 * 인터럽트 컨텍스트에서도 불릴 수 있어 raw 스핀락을 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 **의존하는** 쪽:
 *   drivers/pci/access.c   : pci_generic_config_read / _write 와 그 32비트
 *     전용 판. 이 파일의 read/write 콜백이 버스 번호에 따라 둘 중 하나를
 *     고른다(아래 config 접근 설명 참조).
 *   drivers/pci/probe.c    : pci_host_probe 가 버스를 훑는다.
 *   drivers/pci/of.c       : of_irq_parse_and_map_pci 로 INTx 를 해석한다.
 *   irqdomain / msi-lib    : msi_create_parent_irq_domain 으로 MSI 바닥
 *     도메인을 만들고, 그 위층은 커널이 tegra_msi_parent_ops 를 보고 붙인다.
 *   clk / reset / regulator / phy / gpio / pinctrl 프레임워크.
 *   soc/tegra/pmc.h, soc/tegra/cpuidle.h : 전원 게이트와 cpuidle 연동.
 *     **이 두 헤더는 이 스파스 체크아웃에 없다**(include/soc/tegra 가 존재하지
 *     않음). 따라서 tegra_powergate_power_on, TEGRA_POWERGATE_PCIE,
 *     tegra_cpuidle_pcie_irqs_in_use 의 정의는 확인하지 못했고, 이 파일에서는
 *     쓰임새로만 설명한다.
 *
 * 이 파일에 **의존하는** 쪽: 직접 심볼을 부르는 코드는 없다. PCI 코어가
 * struct pci_ops 와 map_irq 콜백을 통해 간접적으로 들어올 뿐이다. 이 파일이
 * 내보내는 심볼도 없다(모두 static 이거나 드라이버 등록용이다).
 *
 * 데이터 흐름 -- 하드웨어 블록 셋으로 나뉜다:
 *   pcie->afi  : 주소 변환과 인터럽트를 담당하는 레지스터 블록. 이름을
 *     풀어 쓴 곳이 이 트리에 없으나, 레지스터 이름이 AFI_AXI_BAR 계열과
 *     AFI_FPCI_BAR 계열로 짝지어 있고 tegra_pcie_setup_translations 위의
 *     원문 주석이 FPCI 주소 지도를 적고 있는 것으로 보아, SoC 쪽 버스(AXI)와
 *     PCIe 쪽 내부 주소 공간(FPCI) 사이를 잇는 블록이다. MSI 수신 주소와
 *     오류 인터럽트도 여기 있다.
 *   pcie->pads : 내장 PHY 와 PLL 을 다루는 블록. 컨트롤러 전체에 하나뿐이라
 *     포트별이 아니라 전역이다.
 *   pcie->cfg  : config 접근용 4KiB 창 하나.
 *   port->base : 루트 포트마다 하나씩 있는 레지스터 블록. 포트 자신의
 *     config 공간이자 NVIDIA 고유 확장 레지스터(RP_ 계열)가 놓인 곳이다.
 *
 * 공유 상태: struct tegra_pcie 가 컨트롤러 전역 상태이고,
 * struct tegra_pcie_port 가 포트별 상태로 pcie->ports 연결 리스트에 달린다.
 * SoC 별 차이는 struct tegra_pcie_soc 한 곳에 모아 두고 pcie->soc 로 참조한다.
 * PCI 코어에는 host->sysdata 로 struct tegra_pcie 를 넘겨, config 접근 때
 * bus->sysdata 로 되돌려 받는다.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수:
 *   tegra_pcie_map_bus            : config 접근의 핵심. 버스 0 이면 포트
 *     레지스터를 직접 가리키고, 그 밖이면 4KiB 창을 목표 주소로 옮긴 뒤
 *     그 창 안의 주소를 돌려준다. **창을 옮기는 부작용이 있는 map_bus** 다.
 *   tegra_pcie_setup_translations : 브리지의 IO/MEM 창을 AXI BAR 와 FPCI BAR
 *     짝에 프로그래밍한다. 아웃바운드 주소 변환 전부가 이 함수에 있다.
 *   tegra_pcie_enable_controller  : 레인 배분, 포트 활성화, Gen2 퓨즈,
 *     오류 인터럽트 활성화를 한 번에 처리한다.
 *   tegra_pcie_port_enable        : 포트 하나를 깨운다. 참조 클록, 리셋 펄스,
 *     루트 포트 기능(AER, 대역폭 최적화), 이퀄라이저, 소프트웨어 우회를
 *     차례로 적용한다.
 *   tegra_pcie_port_check_link    : 링크가 설 때까지 기다린다. 데이터 링크
 *     업과 링크 활성 두 조건을 각각 최대 200회 폴링하며 최대 3번 재시도한다.
 *   tegra_pcie_msi_irq            : MSI 체인 핸들러. 32비트 레지스터 8개를
 *     훑어 대기 벡터를 도메인에 넘긴다.
 *   tegra_pcie_isr                : 컨트롤러 오류 인터럽트. 오류 코드를
 *     err_msg[] 로 사람이 읽는 문장으로 바꿔 찍는다.
 *   tegra_pcie_pm_resume          : 하드웨어 기동 전체. probe 와 시스템 복귀가
 *     공유한다.
 *
 * 구조체:
 *   struct tegra_pcie       : 컨트롤러 전역 상태. 세 레지스터 블록의 가상
 *     주소, 클록 넷, 리셋 셋, PHY, MSI 상태, 포트 리스트, regulator 배열,
 *     SoC 기술자를 담는다.
 *   struct tegra_pcie_port  : 루트 포트 하나. 인덱스, 레인 수, 자기 레지스터
 *     블록, 레인별 PHY 배열, PERST GPIO 를 담는다.
 *   struct tegra_pcie_soc   : SoC 세대 차이를 모은 기술자. 포트 수, 기능
 *     유무 불리언 열 개 남짓, 참조 클록 패드 값, 이퀄라이저 값이 들어 있다.
 *     이 파일의 모든 세대 분기가 이 구조체 하나를 통과한다.
 *   struct tegra_msi        : MSI 벡터 비트맵, 도메인, 락 둘, 그리고 장치가
 *     쓰기를 보낼 목적지 페이지(virt/phys)를 담는다.
 *
 * === config 접근이 버스 번호에 따라 갈리는 이유 ===
 * 이 파일에서 가장 독특한 대목이므로 따로 적어 둔다.
 *
 * 버스 0(루트 포트 자신)은 port->base 에 놓인 레지스터로 직접 접근한다.
 * 슬롯 번호로 포트를 찾아 그 블록을 가리키고, 주소를 dword 로 정렬한다.
 * 그래서 read/write 콜백이 pci_generic_config_read32 / _write32 를 고른다 --
 * 그 판은 map_bus 에 (where & ~0x3) 을 넘기고 항상 32비트로 접근한다
 * (drivers/pci/access.c:438). 포트 레지스터가 dword 접근만 허용하기 때문이다.
 *
 * 버스 1 이상(그 아래 매달린 실제 장치)은 사정이 다르다. 이 하드웨어의
 * config 주소 인코딩은 ECAM 과 비슷하되 확장 레지스터 4비트의 자리가 달라서
 * (위 tegra_pcie_conf_offset 앞의 원문 주석이 비트 배치를 적고 있다),
 * 전체를 매핑하면 256MiB 의 가상 주소가 필요하다. 그래서 4KiB 창 하나만
 * 매핑해 두고, 접근할 때마다 AFI_FPCI_BAR0 에 새 기준 주소를 써서 그 창을
 * 목표 지점으로 **옮긴다**. 이쪽은 바이트 단위 접근이 가능하므로
 * pci_generic_config_read / _write(32 가 붙지 않은 판)를 쓰며, 그 판은
 * map_bus 에 정렬하지 않은 where 를 그대로 넘긴다(access.c:328).
 *
 * 즉 이 드라이버의 map_bus 는 **주소를 계산해 돌려주기만 하는 함수가 아니라
 * 하드웨어 상태를 바꾸는 함수** 다. PCI 코어의 config 락이 그 순서를
 * 직렬화해 주기 때문에 성립한다.
 *
 * === 자체 IP 에서 DesignWare 로 (pcie-tegra194.c 와의 대비) ===
 * 같은 벤더가 Tegra194 부터 Synopsys DesignWare IP 로 갈아탔고, 그쪽
 * 드라이버가 drivers/pci/controller/dwc/pcie-tegra194.c 다. 이 파일에 있는
 * 것 중 **그쪽에서 사라진 것**:
 *   config 접근  : 이 파일의 tegra_pcie_conf_offset / tegra_pcie_map_bus /
 *     config_read / config_write 네 함수가 하는 일을, 그쪽은
 *     dw_pcie_own_conf_map_bus(pcie-designware-host.c:1951) 한 줄로 끝낸다.
 *     그쪽에 남은 read/write 콜백은 특정 레지스터 하나를 건너뛰는 우회일 뿐,
 *     주소 계산은 하지 않는다.
 *   아웃바운드 창 : 이 파일의 tegra_pcie_setup_translations 가 AXI BAR 여섯
 *     개를 손으로 채우는 반면, 그쪽은 창 관리를 DWC 코어의 iATU 에 맡기고
 *     APPL 레지스터에 iATU 블록의 기준 주소만 알려 준다.
 *   MSI 구현     : 이 파일은 비트맵, irq_chip, 도메인, 목적지 페이지 할당,
 *     체인 핸들러까지 전부 직접 갖는다(여덟 함수 남짓). 그쪽에는 MSI
 *     코드가 없다 -- DWC 코어의 iMSI-RX 를 그대로 쓰고, 드라이버는
 *     pp->num_vectors 를 지정하고 APPL 쪽 MSI 인터럽트를 켜 줄 뿐이다.
 *   PHY/PLL 직접 제어 : 이 파일의 tegra_pcie_phy_enable 은 PADS 레지스터를
 *     직접 만져 PLL 을 재우고 깨우고 잠금을 기다린다. 그쪽은 UPHY 를
 *     드라이버가 직접 만지지 않고 BPMP 펌웨어에 메시지를 보내 맡긴다.
 *
 * 반대로 **그쪽에 새로 생긴 것**은 DWC 코어와 맞물리는 콜백 채우기
 * (dw_pcie_ops, dw_pcie_host_ops, dw_pcie_ep_ops)와, 엔드포인트 모드다.
 * 이 파일은 루트 컴플렉스 전용이며 EP 모드가 없다.
 *
 * 한편 **양쪽에 다 남아 있는 것**도 뚜렷하다 -- SoC 별 기능 차이를 담는
 * 기술자 구조체(이 파일의 tegra_pcie_soc, 그쪽의 tegra_pcie_dw_of_data),
 * regulator 다루기, 링크 상태를 debugfs 로 보여 주기, 그리고 IP 코어 바깥에
 * 벤더가 덧붙인 접착 레지스터 블록(이 파일의 AFI, 그쪽의 APPL)이다.
 * IP 를 바꿔도 "칩 바깥 배선과 전원" 은 여전히 벤더 몫이라는 뜻이다.
 *
 * === 값의 근거에 대하여 ===
 * 이 파일에는 근거를 확인할 수 없는 상수가 여럿 있다. 아래 원칙으로 적었다.
 *   PADS_REFCLK_CFG 계열에 들어가는 값(0xfa5cfa5c, 0x44ac44ac, 0x90b890b8,
 *     0x80b880b8, 0x000480b8)과 ectl 프리셋 값들은 **의미를 알 수 없다.**
 *     PADS_REFCLK_CFG 쪽은 위의 원문 주석이 "이 필드 정의와 값은 TRM 에
 *     없고 NVIDIA 에서 받은 것" 이라고 직접 밝히고 있다. 그래서 이 파일의
 *     주석도 각 값이 어느 레지스터의 어느 필드로 들어가는지까지만 적고,
 *     그것이 무엇을 뜻하는지는 단정하지 않는다.
 *   PCI_CLASS_BRIDGE_PCI_NORMAL, PCI_EXP_LNKSTA_ 계열, PCI_EXP_LNKCTL_RL 등
 *     커널 공통 PCI 상수를 정의한 헤더는 이 스파스 체크아웃에 없다. 값을
 *     확인하지 못했으므로 쓰임새로만 설명한다.
 *   Tegra SoC 서비스(전원 게이트, cpuidle)의 헤더도 없다. 위의 타 모듈 절에
 *     적은 대로다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 부르지 않는다(이 트리에서 전수
 * 확인: 0건). 이 드라이버가 만드는 것은 버스이고, NVMe SSD 는 그 버스 위에서
 * 열거되는 장치 중 하나일 뿐이다.
 *
 * 다만 이 파일이 정하는 것 중 그 위에 붙는 SSD 의 성능을 직접 좌우하는
 * 대목이 둘 있다. 하나는 **레인 배분**(tegra_pcie_get_xbar_config)으로,
 * 장치 트리가 지정한 조합에 따라 어느 포트가 x4 를 받고 어느 포트가 x1 을
 * 받는지가 갈린다. 다른 하나는 **링크 속도**로, 이 세대의 컨트롤러는
 * Gen2(5.0GT/s)가 상한이며 그나마도 tegra_pcie_apply_sw_fixup 이 처음에는
 * Gen1 만 광고하도록 낮춰 둔다. 위의 원문 주석이 그 이유를 밝히는데,
 * 일부 구형 엔드포인트가 Gen1 과 Gen2 를 함께 광고하면 링크를 세우지
 * 못하기 때문이다. 링크가 선 뒤에야 tegra_pcie_change_link_speed 가 Gen2 로
 * 재훈련한다. 따라서 이 세대에서 SSD 가 낼 수 있는 대역폭의 상한은
 * x4 Gen2 이며, Gen3 이상은 하드웨어에 없다.
 */

/* [한국어] clk_prepare_enable / clk_disable_unprepare 와 devm_clk_get.
 * 이 컨트롤러는 클록 넷(pex, afi, pll_e, cml)을 개별로 다루므로 bulk API 가
 * 아니라 하나씩 얻고 켠다 */
#include <linux/clk.h>
/* [한국어] scoped_guard 매크로. tegra_msi_irq_mask 와 _unmask 가 락을 블록 범위로
 * 잡는 데 쓴다 -- 명시적 unlock 호출이 없는 이유가 이것이다 */
#include <linux/cleanup.h>
/* [한국어] debugfs_create_dir / _create_file 와 DEFINE_SEQ_ATTRIBUTE.
 * 포트별 링크 상태를 보여 주는 debugfs 파일에 필요하다 */
#include <linux/debugfs.h>
/* [한국어] msleep / usleep_range. 리셋 펄스 폭(1~2ms), PLL 리셋 유지(20~100us),
 * PME 응답 대기(10~11ms) 등 이 파일의 모든 지연이 여기서 온다 */
#include <linux/delay.h>
/* [한국어] 모듈 심볼 내보내기 관련. 이 파일은 EXPORT_SYMBOL 을 쓰지 않으나,
 * DECLARE_PCI_FIXUP 계열 매크로가 섹션에 항목을 만드는 데 관련된다 */
#include <linux/export.h>
/* [한국어] gpiod_set_value 와 devm_fwnode_gpiod_get. PERST 신호를 GPIO 로 내보내는
 * 보드를 위해 필요하다 -- GPIO 가 없으면 AFI 레지스터로 대체한다 */
#include <linux/gpio/consumer.h>
/* [한국어] request_irq / free_irq 와 IRQF_SHARED. AFI 오류 인터럽트를 공유 선으로
 * 요청하는 데 필요하다 */
#include <linux/interrupt.h>
/* [한국어] readl_poll_timeout. tegra_pcie_pme_turnoff 가 PME 응답 비트를 기다리는
 * 데 한 번 쓴다. 다른 폴링들은 직접 루프를 돈다 */
#include <linux/iopoll.h>
/* [한국어] struct irq_desc 와 irq_domain 관련 기본 타입 */
#include <linux/irq.h>
/* [한국어] chained_irq_enter / chained_irq_exit. MSI 선 하나를 벡터 256개로
 * 나누는 체인 핸들러가 상위 컨트롤러의 ack 를 대신 처리하는 데 필요하다 */
#include <linux/irqchip/chained_irq.h>
/* [한국어] msi_lib_init_dev_msi_info. MSI 부모 도메인이 자식 도메인 정보를 채울 때
 * 쓰는 공통 구현이며, tegra_msi_parent_ops 가 그것을 그대로 가리킨다 */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irq_domain_set_info, irq_find_mapping, irq_domain_remove 등.
 * MSI 바닥 도메인이 이 판을 쓴다. INTx 는 도메인을 만들지 않고 map_irq
 * 콜백으로 처리하므로 그쪽에는 쓰이지 않는다 */
#include <linux/irqdomain.h>
/* [한국어] ARRAY_SIZE, upper_32_bits / lower_32_bits 등 기본 매크로 */
#include <linux/kernel.h>
/* [한국어] 초기화 관련 매크로. builtin_platform_driver 가 이 헤더에 의존한다 */
#include <linux/init.h>
/* [한국어] MODULE_DEVICE_TABLE, MODULE_LICENSE 등 */
#include <linux/module.h>
/* [한국어] struct msi_msg 와 struct msi_parent_ops. MSI 주소/데이터를 조립하는
 * tegra_compose_msi_msg 와 부모 도메인 ops 정의에 필요하다 */
#include <linux/msi.h>
/* [한국어] of_address_to_resource. 포트 자식 노드의 레지스터 주소를 읽는 데 쓴다 */
#include <linux/of_address.h>
/* [한국어] of_pci_get_devfn 과 of_irq_parse_and_map_pci. 앞의 것은 포트 노드에서
 * 슬롯 번호를 뽑고, 뒤의 것은 INTx 를 해석한다 */
#include <linux/of_pci.h>
/* [한국어] 장치 트리 순회 관련. for_each_child_of_node_scoped 로 포트 노드를 훑는다 */
#include <linux/of_platform.h>
/* [한국어] struct pci_bus, struct pci_ops, DECLARE_PCI_FIXUP 계열.
 * PCI 코어와 맞물리는 모든 타입이 여기서 온다 */
#include <linux/pci.h>
/* [한국어] phy_init / phy_power_on / phy_exit. 신형 바인딩에서 레인별 PHY 를
 * 다루는 데 쓴다. 구형 경로는 PADS 레지스터를 직접 만지므로 이 API 를
 * 쓰지 않는다 */
#include <linux/phy/phy.h>
/* [한국어] pinctrl_pm_select_default_state / _idle_state. 절전에 들어갈 때 PCIe 핀을
 * DPD 상태로 보내고 복귀할 때 되돌린다 */
#include <linux/pinctrl/consumer.h>
/* [한국어] platform_get_irq_byname, devm_platform_ioremap_resource_byname,
 * builtin_platform_driver. 이 컨트롤러는 PCI 장치가 아니라 플랫폼 장치다 --
 * 자기 자신이 PCI 버스를 제공하는 쪽이기 때문이다 */
#include <linux/platform_device.h>
/* [한국어] reset_control_assert / _deassert. 리셋 셋(pex, afi, pcie_x)을 서로 다른
 * 시점에 푸는 것이 이 드라이버 기동 순서의 뼈대다 */
#include <linux/reset.h>
/* [한국어] SZ_4K. config 창 크기를 4KiB 로 제한하는 데 쓴다 */
#include <linux/sizes.h>
/* [한국어] kasprintf / kfree. devm_of_phy_optional_get_index 가 "pcie-0" 같은
 * 이름을 만드는 데 쓴다 */
#include <linux/slab.h>
/* [한국어] 가상 메모리 관련. 이 파일이 직접 쓰는 함수는 보이지 않으나,
 * 매핑 관련 타입을 위해 포함된 것으로 보인다 */
#include <linux/vmalloc.h>
/* [한국어] regulator_bulk_enable / _disable 와 devm_regulator_bulk_get.
 * SoC 마다 필요한 전원이 다르고 개수도 달라 bulk API 를 쓴다 */
#include <linux/regulator/consumer.h>

/* [한국어] tegra_cpuidle_pcie_irqs_in_use. PCIe 인터럽트를 쓰기 시작했음을
 * cpuidle 에 알린다. **이 헤더는 이 스파스 체크아웃에 없어**
 * (include/soc/tegra 부재) 정확한 동작을 확인하지 못했다 */
#include <soc/tegra/cpuidle.h>
/* [한국어] tegra_powergate_power_on / _off 와 tegra_powergate_remove_clamping,
 * TEGRA_POWERGATE_PCIE. 전원 도메인 프레임워크가 없는 구성에서 PCIe 블록의
 * 전원 게이트를 직접 조작한다. **이 헤더도 이 트리에 없다** */
#include <soc/tegra/pmc.h>

/* [한국어] PCI 서브시스템 내부 헤더. 이 파일이 여기서 쓰는 것은
 * PCI_CLASS_BRIDGE_PCI_NORMAL 을 비롯한 공통 정의로 보이나, 그 상수를
 * 정의한 헤더는 이 스파스 체크아웃에 없어 값을 확인하지 못했다 */
#include "../pci.h"

/* [한국어] **MSI 벡터의 총 개수 256.** 8 × 32 라고 써 둔 것이 하드웨어 구조를
 * 그대로 반영한다 -- 상태 레지스터 AFI_MSI_VEC 와 활성화 레지스터
 * AFI_MSI_EN_VEC 가 각각 32비트짜리 8개씩이기 때문이다.
 * 이 값이 비트맵 크기이자 도메인 크기다 */
#define INT_PCI_MSI_NR (8 * 32)

/* register definitions */

/* [한국어] CPU(AXI) 쪽 주소 창의 크기 레지스터 여섯 개. **4KiB 단위** 라
 * tegra_pcie_setup_translations 가 바이트 크기를 12비트 오른쪽으로 밀어 넣는다.
 * BAR0 은 config 창, 1 은 IO, 2 는 prefetchable MEM, 3 은 non-prefetchable MEM,
 * 4 와 5 는 쓰지 않아 0 으로 지운다 */
#define AFI_AXI_BAR0_SZ	0x00
#define AFI_AXI_BAR1_SZ	0x04
#define AFI_AXI_BAR2_SZ	0x08
#define AFI_AXI_BAR3_SZ	0x0c
#define AFI_AXI_BAR4_SZ	0x10
#define AFI_AXI_BAR5_SZ	0x14

/* [한국어] 같은 창들의 CPU 쪽 시작 주소 레지스터 여섯 개.
 * 크기 레지스터와 짝을 이루며, 오프셋이 0x18 부터 4바이트 간격이다 */
#define AFI_AXI_BAR0_START	0x18
#define AFI_AXI_BAR1_START	0x1c
#define AFI_AXI_BAR2_START	0x20
#define AFI_AXI_BAR3_START	0x24
#define AFI_AXI_BAR4_START	0x28
#define AFI_AXI_BAR5_START	0x2c

/* [한국어] 같은 창들의 PCIe(FPCI) 쪽 대응 주소 레지스터 여섯 개.
 * AXI 쪽 두 레지스터와 합쳐 창 하나가 세 레지스터로 표현된다.
 * **BAR0 만 특별하다** -- tegra_pcie_map_bus 가 config 접근 때마다 이 값을
 * 고쳐 4KiB 창을 옮긴다. 나머지는 tegra_pcie_setup_translations 가 한 번
 * 설정하고 그대로 둔다 */
#define AFI_FPCI_BAR0	0x30
#define AFI_FPCI_BAR1	0x34
#define AFI_FPCI_BAR2	0x38
#define AFI_FPCI_BAR3	0x3c
#define AFI_FPCI_BAR4	0x40
#define AFI_FPCI_BAR5	0x44

/* [한국어] 상류(장치 → 메모리) 트랜잭션의 캐시 속성을 정하는 창.
 * **Tegra20 에만 있다**(has_cache_bars). tegra_pcie_setup_translations 가
 * 네 레지스터를 모두 0 으로 지우는데, 그 위의 원문 주석대로 상류
 * 트랜잭션을 전부 uncached 로 다루겠다는 뜻이다 */
#define AFI_CACHE_BAR0_SZ	0x48
#define AFI_CACHE_BAR0_ST	0x4c
#define AFI_CACHE_BAR1_SZ	0x50
#define AFI_CACHE_BAR1_ST	0x54

/* [한국어] MSI 수신 창의 크기. **4KiB 단위** 이므로 tegra_pcie_enable_msi 가 1 을
 * 쓴다 -- dma_alloc_attrs 로 PAGE_SIZE 한 페이지를 잡은 것과 맞는다 */
#define AFI_MSI_BAR_SZ		0x60
/* [한국어] MSI 수신 주소의 FPCI 쪽 값. tegra_pcie_enable_msi 가
 * msi->phys 를 soc->msi_base_shift 만큼 **오른쪽으로 밀어** 쓴다.
 * 그 시프트가 Tegra20 은 0, 나머지는 8 인데, 세대에 따라 이 레지스터가
 * 주소를 담는 축척이 다르기 때문이다. 그 축척의 근거 문서는 이 트리에 없다 */
#define AFI_MSI_FPCI_BAR_ST	0x64
/* [한국어] MSI 수신 주소의 AXI 쪽 값. 이쪽은 시프트 없이 msi->phys 를
 * 그대로 쓴다 -- FPCI 쪽과 대비되는 지점이다 */
#define AFI_MSI_AXI_BAR_ST	0x68

/* [한국어] MSI **대기 상태** 레지스터 배열. 0x6c 부터 4바이트 간격으로 8개이고
 * 각각 32비트라 벡터 256개를 덮는다.
 * **1을 쓰면 지워진다** -- tegra_msi_irq_ack 이 읽지 않고 BIT 만 쓰는 데서
 * 알 수 있으며, 그래서 그 함수에는 락이 없다.
 * 인자는 **레지스터 번호(0~7)** 이지 벡터 번호가 아니다 */
#define AFI_MSI_VEC(x)		(0x6c + ((x) * 4))
/* [한국어] MSI **활성화** 레지스터 배열. 0x8c 부터 4바이트 간격으로 8개다.
 * 위의 상태 레지스터와 짝을 이루는 별개의 배열이며, 이쪽은
 * 읽고-고쳐-쓰기를 하므로 mask_lock 이 필요하다.
 * tegra_pcie_enable_msi 가 절전 복귀 시 msi->used 비트맵으로부터 이
 * 레지스터들을 재구성한다 */
#define AFI_MSI_EN_VEC(x)	(0x8c + ((x) * 4))

/* [한국어] AFI 전역 설정. tegra_pcie_enable_controller 가 아래 두 비트를 함께 세운다 */
#define AFI_CONFIGURATION		0xac
/* [한국어] FPCI 를 켜는 비트. 이것이 없으면 주소 변환이 동작하지 않는다 */
#define  AFI_CONFIGURATION_EN_FPCI		(1 << 0)
/* [한국어] AFI 동적 클록 게이팅을 끄는 비트.
 * 원문 주석이 "Disable AFI dynamic clock gating and enable PCIe" 로
 * 두 비트의 목적을 함께 밝힌다 */
#define  AFI_CONFIGURATION_CLKEN_OVERRIDE	(1 << 31)

/* [한국어] FPCI 예외 마스크. tegra_pcie_enable_controller 가 0 을 써
 * 원문 주석대로 모든 예외를 끈다 */
#define AFI_FPCI_ERROR_MASKS	0xb0

/* [한국어] 컨트롤러 인터럽트의 최상위 마스크. 비트가 둘뿐이다 */
#define AFI_INTR_MASK		0xb4
/* [한국어] AFI 오류 인터럽트 허용 비트.
 * tegra_pcie_enable_controller 가 세우고 tegra_pcie_disable_interrupts 가 내린다 */
#define  AFI_INTR_MASK_INT_MASK	(1 << 0)
/* [한국어] MSI 인터럽트 허용 비트.
 * **컨트롤러 설정 때는 켜지 않는다** -- 그 함수의 원문 주석대로 필요할 때
 * tegra_pcie_enable_msi 가 켠다. tegra_pcie_disable_msi 가 내린다 */
#define  AFI_INTR_MASK_MSI_MASK	(1 << 8)

/* [한국어] 발생한 오류의 종류를 담는 레지스터. tegra_pcie_isr 이 읽고
 * 곧바로 0 을 써서 다음 오류를 받을 준비를 한다 */
#define AFI_INTR_CODE			0xb8
/* [한국어] 위 레지스터에서 코드만 뽑는 마스크(하위 4비트).
 * 코드가 1~14 라 4비트로 충분하다 */
#define  AFI_INTR_CODE_MASK		0xf
/* [한국어] 오류 코드 1~14. tegra_pcie_isr 의 err_msg[] 배열이 이 순서대로
 * 문자열을 담고 있어, 코드를 그대로 색인으로 쓴다. 배열 원소가 15개이고
 * 인덱스 0 이 "Unknown" 이라 코드 1~14 와 정확히 맞는다.
 * 이 중 AFI_INTR_LEGACY(6)만 특별한데, 그 코드가 오면 tegra_pcie_isr 이
 * IRQ_NONE 을 돌려줘 공유 선의 다음 핸들러에게 차례를 넘긴다 */
#define  AFI_INTR_INI_SLAVE_ERROR	1
#define  AFI_INTR_INI_DECODE_ERROR	2
#define  AFI_INTR_TARGET_ABORT		3
#define  AFI_INTR_MASTER_ABORT		4
#define  AFI_INTR_INVALID_WRITE		5
#define  AFI_INTR_LEGACY		6
#define  AFI_INTR_FPCI_DECODE_ERROR	7
#define  AFI_INTR_AXI_DECODE_ERROR	8
#define  AFI_INTR_FPCI_TIMEOUT		9
#define  AFI_INTR_PE_PRSNT_SENSE	10
#define  AFI_INTR_PE_CLKREQ_SENSE	11
#define  AFI_INTR_CLKCLAMP_SENSE	12
#define  AFI_INTR_RDY4PD_SENSE		13
#define  AFI_INTR_P2P_ERROR		14

/* [한국어] 오류에 딸린 부가 정보. 주소 관련 오류에서는 하위 주소로 해석되며,
 * tegra_pcie_isr 이 & 0xfffffffc 로 하위 2비트를 버리고 쓴다 */
#define AFI_INTR_SIGNATURE	0xbc
/* [한국어] 오류 주소의 상위 부분. tegra_pcie_isr 이 하위 8비트만 취해
 * 32비트 왼쪽으로 올린 뒤 서명과 합쳐 64비트 주소를 만든다 */
#define AFI_UPPER_FPCI_ADDRESS	0xc0
/* [한국어] INTx 어서트/디어서트 이벤트 활성화 레지스터.
 * tegra_pcie_enable_controller 가 0xffffffff 로 전부 켠다 -- 아래의 개별
 * 비트 정의들이 있는데도 통째로 켜므로, 그 정의들은 실제로 쓰이지 않는다 */
#define AFI_SM_INTR_ENABLE	0xc4
/* [한국어] INTA~INTD 의 어서트 비트 넷과 디어서트 비트 넷.
 * **이 파일 어디에서도 개별로 쓰이지 않는다** -- 위 레지스터를 통째로
 * 켜기 때문이다. 레지스터 지도를 완전하게 적어 둔 것으로 보인다 */
#define  AFI_SM_INTR_INTA_ASSERT	(1 << 0)
#define  AFI_SM_INTR_INTB_ASSERT	(1 << 1)
#define  AFI_SM_INTR_INTC_ASSERT	(1 << 2)
#define  AFI_SM_INTR_INTD_ASSERT	(1 << 3)
#define  AFI_SM_INTR_INTA_DEASSERT	(1 << 4)
#define  AFI_SM_INTR_INTB_DEASSERT	(1 << 5)
#define  AFI_SM_INTR_INTC_DEASSERT	(1 << 6)
#define  AFI_SM_INTR_INTD_DEASSERT	(1 << 7)

/* [한국어] AFI 오류 종류별 활성화 레지스터.
 * tegra_pcie_enable_controller 가 여섯 종류를 기본으로 켜고,
 * 지원하는 SoC 는 존재 감지도 더한다 */
#define AFI_AFI_INTR_ENABLE		0xc8
/* [한국어] 오류 종류별 활성화 비트들. 앞의 여섯(비트 0~5)은 항상 켜지고,
 * AFI_INTR_EN_AXI_DECERR 와 AFI_INTR_EN_FPCI_TIMEOUT(비트 6~7)은
 * **이 파일에서 쓰이지 않는다.** AFI_INTR_EN_PRSNT_SENSE(비트 8)만
 * has_intr_prsnt_sense 인 SoC 에서 조건부로 켜진다 */
#define  AFI_INTR_EN_INI_SLVERR		(1 << 0)
#define  AFI_INTR_EN_INI_DECERR		(1 << 1)
#define  AFI_INTR_EN_TGT_SLVERR		(1 << 2)
#define  AFI_INTR_EN_TGT_DECERR		(1 << 3)
#define  AFI_INTR_EN_TGT_WRERR		(1 << 4)
#define  AFI_INTR_EN_DFPCI_DECERR	(1 << 5)
#define  AFI_INTR_EN_AXI_DECERR		(1 << 6)
#define  AFI_INTR_EN_FPCI_TIMEOUT	(1 << 7)
#define  AFI_INTR_EN_PRSNT_SENSE	(1 << 8)

/* [한국어] 전원 관리 이벤트 레지스터. tegra_pcie_pme_turnoff 가 포트별
 * turnoff 비트를 세워 PME_Turn_Off 를 보내고, ack 비트가 서기를 기다린다.
 * 비트 위치는 SoC 와 포트마다 달라 tegra_pcie_port_soc 표를 참조한다 */
#define AFI_PCIE_PME		0xf0

/* [한국어] **레인 배분과 포트 활성화가 함께 들어 있는 레지스터.**
 * tegra_pcie_enable_controller 가 xbar 조합, 포트별 비활성화 비트,
 * CLKREQ GPIO 비트를 한 번에 쓴다 */
#define AFI_PCIE_CONFIG					0x0f8
/* [한국어] 포트별 비활성화 비트. 인덱스에 1 을 더한 자리이므로
 * 포트 0 이 비트 1 이다. **1이 비활성화** 라, 켤 포트는 이 비트를 지운다 */
#define  AFI_PCIE_CONFIG_PCIE_DISABLE(x)		(1 << ((x) + 1))
/* [한국어] 포트 셋을 모두 비활성화하는 값(비트 1~3).
 * tegra_pcie_enable_controller 가 이것을 먼저 세운 뒤 쓰는 포트만
 * 지우는 방식이라, 장치 트리에서 빠진 포트가 자동으로 꺼진다 */
#define  AFI_PCIE_CONFIG_PCIE_DISABLE_ALL		0xe
/* [한국어] 레인 배분 필드(비트 23:20).
 * 아래 상수들이 이 필드에 들어갈 값이다 */
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_MASK	(0xf << 20)
/* [한국어] 레인 배분 조합의 이름들.
 * **값이 겹치는 데 주의한다** -- SINGLE, 420, X2_X1, 401 이 모두
 * (0x0 << 20) 이고, DUAL, 222, X4_X1, 211 이 모두 (0x1 << 20) 이며,
 * 411 과 111 이 (0x2 << 20) 이다. 같은 레지스터 값에 SoC 별로 다른 이름을
 * 붙인 것이라, 이름이 다르다고 값이 다르지 않다.
 * tegra_pcie_get_xbar_config 가 compatible 과 레인 조합으로 이름을 고른다 */
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_SINGLE	(0x0 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_420	(0x0 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_X2_X1	(0x0 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_401	(0x0 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_DUAL	(0x1 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_222	(0x1 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_X4_X1	(0x1 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_211	(0x1 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_411	(0x2 << 20)
#define  AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_111	(0x2 << 20)
/* [한국어] 포트별 CLKREQ 핀을 GPIO 로 돌리는 비트.
 * 인덱스에 29 를 더한 자리다. tegra_pcie_port_disable 위의 원문 주석이
 * 이유를 밝히는데, CLKREQ 가 PCIe 기능으로 남아 있으면 PLLE 가 전원을
 * 내리지 못한다 */
#define  AFI_PCIE_CONFIG_PCIE_CLKREQ_GPIO(x)		(1 << ((x) + 29))
/* [한국어] 세 포트의 CLKREQ 를 모두 GPIO 로 돌리는 값(비트 31:29).
 * DISABLE_ALL 과 같은 방식으로, 먼저 전부 세우고 쓰는 포트만 지운다 */
#define  AFI_PCIE_CONFIG_PCIE_CLKREQ_GPIO_ALL		(0x7 << 29)

/* [한국어] 칩 퓨즈로 정해지는 값을 소프트웨어가 덮어쓰는 레지스터로 보인다 */
#define AFI_FUSE			0x104
/* [한국어] Gen2 금지 비트. tegra_pcie_enable_controller 가
 * has_gen2 면 **지우고** 아니면 세운다. 즉 이름대로 1이 금지다 */
#define  AFI_FUSE_PCIE_T0_GEN2_DIS	(1 << 2)

/* [한국어] 포트 0 의 제어 레지스터. 참조 클록과 리셋이 여기 있다 */
#define AFI_PEX0_CTRL			0x110
/* [한국어] 포트 1 의 제어 레지스터. 포트 2 의 것은 SoC 마다 달라
 * soc->afi_pex2_ctrl 에서 가져온다 -- tegra_pcie_port_get_pex_ctrl 참조 */
#define AFI_PEX1_CTRL			0x118
/* [한국어] 포트 리셋 비트. **0이 리셋 어서트** 라, tegra_pcie_port_reset 이
 * 지워서 리셋에 넣고 세워서 푼다. GPIO 방식과 극성이 반대인 지점이다 */
#define  AFI_PEX_CTRL_RST		(1 << 0)
/* [한국어] CLKREQ 활성화. has_pex_clkreq_en 인 SoC 에서만 다룬다 */
#define  AFI_PEX_CTRL_CLKREQ_EN		(1 << 1)
/* [한국어] 참조 클록 활성화. tegra_pcie_port_enable 이 세우고 _disable 이 지운다 */
#define  AFI_PEX_CTRL_REFCLK_EN		(1 << 3)
/* [한국어] 하드웨어 자동 제어 대신 소프트웨어 설정을 쓰게 하는 비트로 보인다.
 * tegra_pcie_port_enable 이 항상 세우며, 끌 때는 지우지 않는다 */
#define  AFI_PEX_CTRL_OVERRIDE_EN	(1 << 4)

/* [한국어] PLLE 전원 관리를 누가 할지 정하는 레지스터 */
#define AFI_PLLE_CONTROL		0x160
/* [한국어] 패드 제어 우회 비트.
 * tegra_pcie_enable_controller 가 **지워서** 우회를 끈다 */
#define  AFI_PLLE_CONTROL_BYPASS_PADS2PLLE_CONTROL (1 << 9)
/* [한국어] 패드가 PLLE 를 제어하게 하는 비트.
 * 위 비트를 지우고 이 비트를 세우는 짝이라, 패드 상태에 따라 PLLE 가
 * 자동으로 잠들 수 있게 된다. pcie->phy 가 있을 때만 설정한다 */
#define  AFI_PLLE_CONTROL_PADS2PLLE_CONTROL_EN (1 << 1)

/* [한국어] 슬롯 클록 바이어스 패드 제어. tegra_pcie_enable_controller 가
 * has_pex_bias_ctrl 인 SoC 에서 0 을 써 전원을 내린다 */
#define AFI_PEXBIAS_CTRL_0		0x168

/* [한국어] 수신단 이퀄라이저 레지스터 여덟 개(R1 조 넷, R2 조 넷).
 * **Tegra210 에서만 쓰인다**(soc->ectl.enable). 값의 의미는 이 트리에서
 * 확인할 수 없고, 필드 이름이 수신단 이퀄라이저와 클록 복원 관련임을
 * 가리킬 뿐이다. R1/R2 는 속도 등급별 설정으로 보이나 근거는 없다 */
#define RP_ECTL_2_R1	0x00000e84
#define  RP_ECTL_2_R1_RX_CTLE_1C_MASK		0xffff

#define RP_ECTL_4_R1	0x00000e8c
#define  RP_ECTL_4_R1_RX_CDR_CTRL_1C_MASK	(0xffff << 16)
#define  RP_ECTL_4_R1_RX_CDR_CTRL_1C_SHIFT	16

#define RP_ECTL_5_R1	0x00000e90
#define  RP_ECTL_5_R1_RX_EQ_CTRL_L_1C_MASK	0xffffffff

#define RP_ECTL_6_R1	0x00000e94
#define  RP_ECTL_6_R1_RX_EQ_CTRL_H_1C_MASK	0xffffffff

#define RP_ECTL_2_R2	0x00000ea4
#define  RP_ECTL_2_R2_RX_CTLE_1C_MASK	0xffff

#define RP_ECTL_4_R2	0x00000eac
#define  RP_ECTL_4_R2_RX_CDR_CTRL_1C_MASK	(0xffff << 16)
#define  RP_ECTL_4_R2_RX_CDR_CTRL_1C_SHIFT	16

#define RP_ECTL_5_R2	0x00000eb0
#define  RP_ECTL_5_R2_RX_EQ_CTRL_L_1C_MASK	0xffffffff

#define RP_ECTL_6_R2	0x00000eb4
#define  RP_ECTL_6_R2_RX_EQ_CTRL_H_1C_MASK	0xffffffff

/* [한국어] NVIDIA 벤더 확장 레지스터. **이 파일에서 가장 여러 용도로 쓰인다** --
 * 대역폭 최적화 비트를 켜고(enable_rp_features), 흐름 제어 임계값을
 * 바꾸고(apply_sw_fixup), 링크 상태를 읽는다(port_check_link, seq_show) */
#define RP_VEND_XP	0x00000f00
/* [한국어] 데이터 링크가 올라왔음을 나타내는 비트.
 * tegra_pcie_port_check_link 이 기다리는 첫 조건이고,
 * debugfs 의 "up" 표시도 이 비트다 */
#define  RP_VEND_XP_DL_UP			(1 << 30)
/* [한국어] 기회가 될 때 ACK 를 함께 실어 보내는 기능으로 보인다.
 * tegra_pcie_enable_rp_features 위의 원문 주석이 대역폭을 높이는 최적
 * 설정이라 밝힐 뿐, 정확한 동작의 근거는 이 트리에 없다 */
#define  RP_VEND_XP_OPPORTUNISTIC_ACK		(1 << 27)
/* [한국어] 흐름 제어 갱신에 대한 같은 기능으로 보인다.
 * 위 비트와 함께 켜진다 */
#define  RP_VEND_XP_OPPORTUNISTIC_UPDATEFC	(1 << 28)
/* [한국어] 흐름 제어 갱신 임계값 필드(비트 25:18).
 * tegra_pcie_apply_sw_fixup 이 update_fc_timer 인 SoC 에서 덮어쓴다 */
#define  RP_VEND_XP_UPDATE_FC_THRESHOLD_MASK	(0xff << 18)

/* [한국어] 벤더 제어 레지스터 0. 디스큐 리셋 펄스 폭만 쓴다 */
#define RP_VEND_CTL0	0x00000f44
/* [한국어] 디스큐 리셋 펄스 폭 필드(비트 15:12)와
 * 거기 넣을 값(0x9). Tegra210 의 디스큐 로직 불안정 문제를 우회하려고
 * 재시도 시간을 늘리는 설정이다 -- tegra_pcie_apply_sw_fixup 안의
 * 원문 주석이 배경을 밝힌다 */
#define  RP_VEND_CTL0_DSK_RST_PULSE_WIDTH_MASK	(0xf << 12)
#define  RP_VEND_CTL0_DSK_RST_PULSE_WIDTH	(0x9 << 12)

/* [한국어] 벤더 제어 레지스터 1 */
#define RP_VEND_CTL1	0x00000f48
/* [한국어] AER 활성화 비트. tegra_pcie_enable_rp_features 가 세운다.
 * 즉 이 하드웨어는 AER 이 기본으로 켜져 있지 않다 */
#define  RP_VEND_CTL1_ERPT	(1 << 13)

/* [한국어] BIST 관련 벤더 레지스터. 이름과 달리 여기서 쓰는 비트는
 * 전원 관리 진입 순서를 다룬다 */
#define RP_VEND_XP_BIST	0x00000f4c
/* [한국어] LTSSM 이 L1/L2 로 들어가기 전에
 * DLLP 가 끝나기를 기다리게 하는 비트. tegra_pcie_enable_rp_features 위의
 * 원문 주석대로, 전원 관리 메시지가 잘려 수신 오류가 되는 것을 막는다 */
#define  RP_VEND_XP_BIST_GOTO_L1_L2_AFTER_DLLP_DONE	(1 << 28)

/* [한국어] 벤더 제어 레지스터 2. PCA 비트만 쓴다 */
#define RP_VEND_CTL2 0x00000fa8
/* [한국어] PCA 활성화 비트. **Tegra210 에서만** 세운다
 * (soc->force_pca_enable). 이 약어가 무엇의 줄임인지는 이 트리에서
 * 확인할 수 없다 */
#define  RP_VEND_CTL2_PCA_ENABLE (1 << 7)

/* [한국어] 벤더 기타 레지스터. 존재 감지 덮어쓰기와 클록 클램프 설정이 함께 있다 */
#define RP_PRIV_MISC	0x00000fe0
/* [한국어] 존재 감지 강제 값. tegra_pcie_port_check_link 이
 * ABSNT(0xf)를 지우고 PRSNT(0xe)를 넣어, 슬롯 존재 신호가 배선되지 않은
 * 보드에서도 링크 훈련을 시도하게 만든다. 두 값이 최하위 비트만 다르다 */
#define  RP_PRIV_MISC_PRSNT_MAP_EP_PRSNT		(0xe << 0)
#define  RP_PRIV_MISC_PRSNT_MAP_EP_ABSNT		(0xf << 0)
/* [한국어] 컨트롤러와 TMS 쪽 클록 클램프
 * 임계값 필드와 활성화 비트들. tegra_pcie_enable_rp_features 가 활성화
 * 비트는 항상 세우고, 임계값은 update_clamp_threshold 인 SoC 에서만
 * 0xf 로 덮어쓴다 */
#define  RP_PRIV_MISC_CTLR_CLK_CLAMP_THRESHOLD_MASK	(0x7f << 16)
#define  RP_PRIV_MISC_CTLR_CLK_CLAMP_THRESHOLD		(0xf << 16)
#define  RP_PRIV_MISC_CTLR_CLK_CLAMP_ENABLE		(1 << 23)
#define  RP_PRIV_MISC_TMS_CLK_CLAMP_THRESHOLD_MASK	(0x7f << 24)
#define  RP_PRIV_MISC_TMS_CLK_CLAMP_THRESHOLD		(0xf << 24)
#define  RP_PRIV_MISC_TMS_CLK_CLAMP_ENABLE		(1 << 31)

/* [한국어] 표준 PCIe 링크 제어/상태 레지스터 자리(0x90).
 * RP_ 접두사가 붙었지만 벤더 확장이 아니라 config 공간의 표준 위치다 --
 * port->base 가 포트의 config 공간이기도 하기 때문이다.
 * 링크 활성 확인, 재훈련 요청, 훈련 중 확인에 쓰인다 */
#define RP_LINK_CONTROL_STATUS			0x00000090
/* [한국어] 데이터 링크 활성 비트.
 * tegra_pcie_port_check_link 의 두 번째 조건이고 debugfs 의 "active" 표시다 */
#define  RP_LINK_CONTROL_STATUS_DL_LINK_ACTIVE	0x20000000
/* [한국어] 링크 상태 필드 마스크.
 * **이 파일 어디에서도 쓰이지 않는다** -- 레지스터 지도용으로 보인다 */
#define  RP_LINK_CONTROL_STATUS_LINKSTAT_MASK	0x3fff0000

/* [한국어] 표준 Link Control 2 자리(0xb0). 목표 링크 속도를 담는다.
 * tegra_pcie_apply_sw_fixup 이 Gen1 으로 낮추고,
 * tegra_pcie_change_link_speed 가 링크가 선 뒤 Gen2 로 올린다 */
#define RP_LINK_CONTROL_STATUS_2		0x000000b0

/* [한국어] 레인 선택 레지스터. tegra_pcie_phy_enable 이 0 을 쓰며,
 * 그 위의 원문 주석은 최대 16레인까지 쓸 수 있게 한다고 적는다 */
#define PADS_CTL_SEL		0x0000009c

/* [한국어] 패드 제어 레지스터. IDDQ 와 TX/RX 데이터 활성화가 여기 있다 */
#define PADS_CTL		0x000000a0
/* [한국어] IDDQ 비트. 아날로그 블록을 저전력 상태로 두는 신호로,
 * PLL 을 설정하는 동안 켜 두었다가 잠금 후 끈다 */
#define  PADS_CTL_IDDQ_1L	(1 << 0)
/* [한국어] TX 와 RX 데이터 활성화 비트. 이 둘을 세워야 PHY 가
 * 실제로 신호를 내보내기 시작한다. tegra_pcie_phy_enable 의 마지막 단계다 */
#define  PADS_CTL_TX_DATA_EN_1L	(1 << 6)
#define  PADS_CTL_RX_DATA_EN_1L	(1 << 10)

/* [한국어] PLL 제어 레지스터의 오프셋. **세대에 따라 다르다** --
 * Tegra20 은 0xb8, Tegra30 이후는 0xb4 다. 그래서 soc->pads_pll_ctl 에
 * 담아 두고 참조한다 */
#define PADS_PLL_CTL_TEGRA20			0x000000b8
#define PADS_PLL_CTL_TEGRA30			0x000000b4
/* [한국어] PLL 리셋 비트. **1이 리셋 해제** 라,
 * tegra_pcie_phy_enable 이 지워서 리셋에 넣고 세워서 푼다 */
#define  PADS_PLL_CTL_RST_B4SM			(1 << 1)
/* [한국어] PLL 잠금 검출 비트. tegra_pcie_pll_wait 이 이 비트가
 * 서기를 최대 500ms 동안 폴링한다 */
#define  PADS_PLL_CTL_LOCKDET			(1 << 8)
/* [한국어] 참조 클록 선택 필드와 그 값들.
 * tegra_pcie_phy_enable 은 INTERNAL_CML 만 쓰며, CMOS 와 EXTERNAL 은
 * **이 파일에서 쓰이지 않는다** */
#define  PADS_PLL_CTL_REFCLK_MASK		(0x3 << 16)
#define  PADS_PLL_CTL_REFCLK_INTERNAL_CML	(0 << 16)
#define  PADS_PLL_CTL_REFCLK_INTERNAL_CMOS	(1 << 16)
#define  PADS_PLL_CTL_REFCLK_EXTERNAL		(2 << 16)
/* [한국어] TX 참조 클록 분주 필드와 값들.
 * soc->tx_ref_sel 이 DIV10(Tegra20) 또는 BUF_EN(나머지)을 지정한다.
 * DIV5 는 정의만 있고 쓰이지 않는다 */
#define  PADS_PLL_CTL_TXCLKREF_MASK		(0x1 << 20)
#define  PADS_PLL_CTL_TXCLKREF_DIV10		(0 << 20)
#define  PADS_PLL_CTL_TXCLKREF_DIV5		(1 << 20)
#define  PADS_PLL_CTL_TXCLKREF_BUF_EN		(1 << 22)

/* [한국어] 참조 클록 드라이버 설정 레지스터 둘과 바이어스 레지스터.
 * tegra_pcie_apply_pad_settings 가 앞의 둘에 soc 의 값을 그대로 쓴다.
 * PADS_REFCLK_BIAS 는 **이 파일에서 쓰이지 않는다** */
#define PADS_REFCLK_CFG0			0x000000c8
#define PADS_REFCLK_CFG1			0x000000cc
#define PADS_REFCLK_BIAS			0x000000d0

/*
 * Fields in PADS_REFCLK_CFG*. Those registers form an array of 16-bit
 * entries, one entry per PCIe port. These field definitions and desired
 * values aren't in the TRM, but do come from NVIDIA.
 */
/* [한국어] 위 레지스터 안의 네 필드 위치.
 * 바로 위의 원문 주석이 중요한 사실을 밝힌다 -- 이 레지스터들은 16비트
 * 항목의 배열이고 포트마다 한 항목이며, **이 필드 정의와 원하는 값은
 * TRM 에 없고 NVIDIA 에서 받은 것** 이다. 그래서 soc 에 저장된 값
 * (0xfa5cfa5c 등)의 의미를 이 트리에서 확인할 방법이 없다.
 * 네 시프트 값 자체도 **이 파일 어디에서도 쓰이지 않는다** -- 값을 통째로
 * 쓰기 때문이며, 필드 구조를 기록해 둔 문서 역할만 한다 */
#define PADS_REFCLK_CFG_TERM_SHIFT		2  /* 6:2 */
#define PADS_REFCLK_CFG_E_TERM_SHIFT		7
#define PADS_REFCLK_CFG_PREDI_SHIFT		8  /* 11:8 */
#define PADS_REFCLK_CFG_DRVI_SHIFT		12 /* 15:12 */

/* [한국어] PME 응답을 기다리는 상한. readl_poll_timeout 에 마이크로초 단위로
 * 넘기므로 10ms 다. tegra_pcie_pme_turnoff 가 1us 간격으로 폴링한다 */
#define PME_ACK_TIMEOUT 10000
/* [한국어] 링크 재훈련을 기다리는 상한. 주석대로 마이크로초 단위라 100ms 다.
 * tegra_pcie_change_link_speed 가 recovery 대기와 재훈련 대기 두 곳에 쓴다 */
#define LINK_RETRAIN_TIMEOUT 100000 /* in usec */

struct tegra_msi {
	/* [한국어] 어느 MSI 벡터가 쓰이는지 표시하는 비트맵.
	 * 설정자: tegra_msi_domain_alloc 이 bitmap_find_free_region 으로 자리를 잡고,
	 *   tegra_msi_domain_free 가 bitmap_release_region 으로 푼다.
	 * 읽는 자: 위 두 함수, 그리고 **tegra_pcie_enable_msi**.
	 * 값 범위: INT_PCI_MSI_NR(256)비트. 비트 번호가 곧 전역 hwirq 이며,
	 *   32로 나눈 몫이 레지스터 번호, 나머지가 그 안의 비트다.
	 * 특기할 점: 이 비트맵이 **절전 복원용 상태를 겸한다.** 다른 드라이버가
	 *   별도의 saved_irq_state 필드를 두는 자리를, 여기서는 할당 비트맵을
	 *   그대로 활성화 레지스터에 써서 대신한다. 그 결과 할당은 되었으나
	 *   마스크된 벡터도 복귀 후 켜진 상태가 된다.
	 * 동기화: map_lock(뮤텍스)이 지킨다 */
	DECLARE_BITMAP(used, INT_PCI_MSI_NR);
	/* [한국어] MSI 바닥 인터럽트 도메인.
	 * 설정자: tegra_allocate_domains 가 msi_create_parent_irq_domain 으로 만든다.
	 *   그 위의 장치별 도메인은 커널이 tegra_msi_parent_ops 를 보고 붙여 준다.
	 * 읽는 자: tegra_pcie_msi_irq 가 벡터를 넘길 때,
	 *   tegra_pcie_msi_teardown 이 매핑을 찾아 풀 때.
	 * 값 범위: 유효한 포인터. CONFIG_PCI_MSI 가 꺼져 있으면 만들어지지 않는다.
	 * host_data 에 struct tegra_msi 가 들어 있어 alloc 콜백이 그것을 꺼내
	 *   chip_data 로 심는다.
	 * 동기화: irqdomain 코어가 내부를 지킨다 */
	struct irq_domain *domain;
	/* [한국어] 위 비트맵을 지키는 뮤텍스.
	 * 설정자: tegra_pcie_msi_setup 이 초기화한다.
	 * 쥐는 곳: tegra_msi_domain_alloc 과 _free 두 곳뿐이다.
	 * 왜 뮤텍스인가: 두 함수는 프로세스 컨텍스트에서만 불리고 잠들어도 되기
	 *   때문이다. 레지스터를 지키는 mask_lock 과 종류를 달리한 이유가 여기 있다.
	 * 범위가 좁은 점에 주의 -- 비트맵 조작 직후 풀고, 이어지는
	 *   irq_domain_set_info 는 락 밖에서 한다.
	 * 동기화: 이 필드 자체가 동기화 수단이다 */
	struct mutex map_lock;
	/* [한국어] MSI 활성화 레지스터 접근을 지키는 raw 스핀락.
	 * 설정자: tegra_pcie_msi_setup 이 초기화한다.
	 * 쥐는 곳: tegra_msi_irq_mask 와 _unmask 두 곳. scoped_guard 로 블록
	 *   범위를 잡는다.
	 * 무엇을 지키는가: AFI_MSI_EN_VEC 레지스터의 읽고-고쳐-쓰기. 한 레지스터를
	 *   벡터 32개가 공유하므로 락이 없으면 한쪽의 변경이 사라진다.
	 *   1을 써서 지우는 상태 레지스터(AFI_MSI_VEC)는 읽지 않으므로 락이 필요
	 *   없고, 그래서 tegra_msi_irq_ack 에는 락이 없다.
	 * 왜 raw 인가: irq_chip 콜백이 인터럽트 컨텍스트에서 불리고 PREEMPT_RT
	 *   에서도 잠들면 안 되기 때문이다.
	 * 동기화: 이 필드 자체가 동기화 수단이다 */
	raw_spinlock_t mask_lock;
	/* [한국어] MSI 수신 페이지의 커널 가상 주소.
	 * 설정자: tegra_pcie_msi_setup 이 dma_alloc_attrs 로 얻는다.
	 * 읽는 자: **아무도 내용을 읽지 않는다.** dma_free_attrs 에 넘기는 것이
	 *   유일한 용도다.
	 * 값 범위: DMA_ATTR_NO_KERNEL_MAPPING 으로 잡았으므로 실제로 커널 매핑이
	 *   없을 수 있는 불투명 값이다. 역참조하면 안 된다.
	 * 왜 필요한가: 장치가 이 페이지의 주소로 보내는 쓰기를 컨트롤러가
	 *   가로채 인터럽트로 바꾸므로, 메모리에 실제로 닿지 않는다. 페이지를
	 *   잡는 것은 그 주소를 다른 용도로 쓰이지 않게 예약하는 의미다.
	 * 동기화: probe 때 정하고 이후 불변 */
	void *virt;
	/* [한국어] 같은 페이지의 DMA 주소.
	 * 설정자: dma_alloc_attrs 가 채워 준다.
	 * 읽는 자: tegra_compose_msi_msg 가 MSI 메시지 주소로 그대로 쓰고,
	 *   tegra_pcie_enable_msi 가 하드웨어의 MSI 수신 주소 레지스터에 쓴다.
	 * 값 범위: 32비트 안에 들어간다 -- tegra_pcie_msi_setup 이 DMA 마스크를
	 *   32비트로 제한하기 때문이며, 위의 원문 주석대로 32비트 MSI 목표
	 *   주소만 지원하는 엔드포인트와의 호환을 위해서다.
	 * 특기할 점: **모든 벡터가 같은 주소를 쓴다.** 벡터 구분은 데이터 값으로
	 *   한다. pcie-mediatek-gen3.c 가 세트마다 다른 주소를 쓰는 것과 대비된다.
	 * 동기화: probe 때 정하고 이후 불변 */
	dma_addr_t phys;
	/* [한국어] MSI 전용 인터럽트의 가상 IRQ 번호.
	 * 설정자: tegra_pcie_msi_setup 이 platform_get_irq_byname("msi")로 얻는다.
	 * 읽는 자: 체인 핸들러를 걸고 뗄 때.
	 * 값 범위: 양수.
	 * 특기할 점: AFI 오류 인터럽트(tegra_pcie 의 irq)와 **별개의 선** 이다.
	 *   장치 트리에 "intr" 과 "msi" 두 이름으로 따로 있다.
	 * 동기화: probe 때 정하고 이후 불변 */
	int irq;
};

/* used to differentiate between Tegra SoC generations */
struct tegra_pcie_port_soc {
	struct {
		/* [한국어] 이 포트의 PME_Turn_Off 요청 비트 위치.
		 * 설정자: 정적 초기화. SoC 별 tegra_pcie_port_soc 배열에 들어 있다.
		 * 읽는 자: tegra_pcie_pme_turnoff 가 AFI_PCIE_PME 에서 이 비트를 세우고
		 *   나중에 지운다.
		 * 값 범위: SoC 와 포트에 따라 0, 8, 12, 16 중 하나.
		 * 왜 표가 필요한가: 비트 배치가 규칙적이지 않기 때문이다. Tegra30 의
		 *   셋째 포트는 16 인데 Tegra186 의 셋째 포트는 12 다.
		 * 동기화: 읽기 전용 상수 */
		u8 turnoff_bit;
		/* [한국어] 이 포트의 PME 응답 비트 위치.
		 * 설정자: 정적 초기화.
		 * 읽는 자: tegra_pcie_pme_turnoff 가 이 비트가 서기를 폴링한다.
		 * 값 범위: 5, 10, 14, 18 중 하나. 대응하는 turnoff_bit 보다 항상 크다.
		 * 동기화: 읽기 전용 상수 */
		u8 ack_bit;
	} pme;
};

struct tegra_pcie_soc {
	/* [한국어] 이 SoC 가 가진 루트 포트의 개수.
	 * 설정자: 정적 초기화. 2 또는 3 이다.
	 * 읽는 자: tegra_pcie_parse_dt 가 장치 트리의 포트 번호를 검증하고,
	 *   tegra_pcie_apply_pad_settings 가 참조 클록 레지스터를 하나 쓸지
	 *   둘 쓸지 정한다.
	 * 값 범위: Tegra20/124/210 은 2, Tegra30/186 은 3.
	 * 동기화: 읽기 전용 상수 */
	unsigned int num_ports;
	/* [한국어] 포트별 PME 비트 표.
	 * 설정자: 정적 초기화.
	 * 읽는 자: tegra_pcie_pme_turnoff 가 port->index 로 색인한다.
	 * 값 범위: num_ports 개의 원소를 갖는 배열.
	 * 특기할 점: **Tegra124 와 Tegra210 이 tegra20_pcie_ports 를 공유한다** --
	 *   포트가 둘이고 비트 배치가 같기 때문이다. 표를 새로 만들지 않고
	 *   재사용한 것이다.
	 * 동기화: 읽기 전용 상수 */
	const struct tegra_pcie_port_soc *ports;
	/* [한국어] MSI 수신 주소를 FPCI 레지스터에 쓸 때의 오른쪽 시프트량.
	 * 설정자: 정적 초기화. Tegra20 만 0, 나머지는 8 이다.
	 * 읽는 자: tegra_pcie_enable_msi 하나뿐이다.
	 * 값 범위: 0 또는 8.
	 * 왜 필요한가: 세대에 따라 이 레지스터가 주소를 담는 축척이 다르기
	 *   때문이다. **그 축척의 근거 문서는 이 트리에 없다** -- 값이 두 가지뿐이고
	 *   Tegra20 만 다르다는 사실만 확인된다.
	 * 동기화: 읽기 전용 상수 */
	unsigned int msi_base_shift;
	/* [한국어] 세 번째 포트의 제어 레지스터 오프셋.
	 * 설정자: 정적 초기화. Tegra30 은 0x128, Tegra186 은 0x19c 다.
	 * 읽는 자: tegra_pcie_port_get_pex_ctrl 이 index 가 2 일 때 쓴다.
	 * 값 범위: 포트가 둘뿐인 SoC 에서는 0 으로 남는다(초기화하지 않음).
	 * 왜 이 필드만 있는가: 포트 0 과 1 의 제어 레지스터는 모든 SoC 에서
	 *   0x110 과 0x118 로 같은데 세 번째만 다르기 때문이다.
	 * 동기화: 읽기 전용 상수 */
	unsigned long afi_pex2_ctrl;
	/* [한국어] PLL 제어 레지스터의 오프셋.
	 * 설정자: 정적 초기화. Tegra20 은 PADS_PLL_CTL_TEGRA20(0xb8),
	 *   나머지는 PADS_PLL_CTL_TEGRA30(0xb4).
	 * 읽는 자: tegra_pcie_pll_wait, tegra_pcie_phy_enable, _phy_disable.
	 * 값 범위: 두 값 중 하나.
	 * 동기화: 읽기 전용 상수 */
	u32 pads_pll_ctl;
	/* [한국어] TX 참조 클록 선택 값.
	 * 설정자: 정적 초기화. Tegra20 만 TXCLKREF_DIV10, 나머지는 TXCLKREF_BUF_EN.
	 * 읽는 자: tegra_pcie_phy_enable 이 PLL 제어 레지스터에 넣는다.
	 * 특기할 점: tegra_pcie_phy_enable 위의 원문 주석은 "div10 으로 설정"
	 *   이라고 적지만, 실제로는 SoC 마다 다르다. 주석이 Tegra20 기준으로
	 *   남아 있는 셈이다.
	 * 동기화: 읽기 전용 상수 */
	u32 tx_ref_sel;
	/* [한국어] 첫 참조 클록 설정 레지스터에 쓸 값.
	 * 설정자: 정적 초기화. SoC 마다 다르다(0xfa5cfa5c, 0x44ac44ac,
	 *   0x90b890b8, 0x80b880b8).
	 * 읽는 자: tegra_pcie_apply_pad_settings.
	 * **값의 의미는 알 수 없다.** PADS_REFCLK_CFG 필드 정의 위의 원문 주석이
	 *   "이 필드 정의와 원하는 값은 TRM 에 없고 NVIDIA 에서 받은 것" 이라고
	 *   직접 밝히고 있다. 같은 16비트가 두 번 반복되는 형태인 것은 그 주석이
	 *   말한 "포트마다 한 항목" 구조와 맞아떨어진다.
	 * 동기화: 읽기 전용 상수 */
	u32 pads_refclk_cfg0;
	/* [한국어] 둘째 참조 클록 설정 레지스터에 쓸 값.
	 * 설정자: 정적 초기화. 포트가 셋인 SoC(Tegra30, Tegra186)만 채운다.
	 * 읽는 자: tegra_pcie_apply_pad_settings 가 num_ports > 2 일 때만 쓴다.
	 * 특기할 점: Tegra186 의 값 0x000480b8 만 상하위 16비트가 다르다.
	 *   셋째 포트만 설정이 다르다는 의미로 읽히나, 역시 근거 문서는 없다.
	 * 동기화: 읽기 전용 상수 */
	u32 pads_refclk_cfg1;
	/* [한국어] 흐름 제어 갱신 임계값.
	 * 설정자: 정적 초기화. **Tegra210 만 0x01800000 을 갖는다.**
	 * 읽는 자: tegra_pcie_apply_sw_fixup 이 update_fc_timer 일 때 쓴다.
	 * 값 범위: 위의 정의에 붙은 원문 주석이 "FC threshold is bit[25:18]"
	 *   이라 적고 있어, 0x01800000 은 그 필드에 6 이 들어가는 값이다.
	 * 동기화: 읽기 전용 상수 */
	u32 update_fc_threshold;
	/* [한국어] CLKREQ 활성화 비트를 쓸 수 있는가.
	 * 설정자: 정적 초기화. Tegra20 만 false.
	 * 읽는 자: tegra_pcie_port_enable 과 _port_disable 이 AFI 제어 레지스터의
	 *   CLKREQ_EN 비트를 건드릴지 정한다.
	 * 동기화: 읽기 전용 상수 */
	bool has_pex_clkreq_en;
	/* [한국어] 슬롯 클록 바이어스 패드 제어 레지스터가 있는가.
	 * 설정자: 정적 초기화. Tegra20 만 false.
	 * 읽는 자: tegra_pcie_enable_controller 가 AFI_PEXBIAS_CTRL_0 에 0 을
	 *   쓸지 정한다.
	 * 동기화: 읽기 전용 상수 */
	bool has_pex_bias_ctrl;
	/* [한국어] 슬롯 존재 감지 인터럽트를 지원하는가.
	 * 설정자: 정적 초기화. Tegra20 만 false.
	 * 읽는 자: tegra_pcie_enable_controller 가 오류 인터럽트 활성화 값에
	 *   AFI_INTR_EN_PRSNT_SENSE 를 더할지 정한다.
	 * 동기화: 읽기 전용 상수 */
	bool has_intr_prsnt_sense;
	/* [한국어] CML 클록이 있는가.
	 * 설정자: 정적 초기화. Tegra30/124/210 만 true 이고, Tegra20 과
	 *   **Tegra186 은 false** 다 -- 즉 세대순으로 단조롭지 않다.
	 * 읽는 자: tegra_pcie_clocks_get 이 클록을 얻을지, tegra_pcie_power_on 과
	 *   _power_off 가 켜고 끌지 정한다.
	 * 동기화: 읽기 전용 상수 */
	bool has_cml_clk;
	/* [한국어] Gen2(5.0GT/s)를 지원하는가.
	 * 설정자: 정적 초기화. Tegra20 과 Tegra30 이 false, Tegra124 이후 true.
	 * 읽는 자: 세 곳이다 -- tegra_pcie_enable_controller 가 Gen2 금지 퓨즈
	 *   비트를 지울지 세울지 정하고, tegra_pcie_enable_ports 가 링크가 선 뒤
	 *   속도를 올릴지 정하며, tegra_pcie_phys_get 이 구형 PHY 바인딩으로
	 *   갈지 판단하는 데 쓴다.
	 * 특기할 점: **이 값이 이 세대 하드웨어의 성능 상한을 나타낸다.**
	 *   true 여도 Gen2 까지이며 Gen3 이상은 이 파일에 없다.
	 * 동기화: 읽기 전용 상수 */
	bool has_gen2;
	/* [한국어] PCA 를 강제로 켜야 하는가.
	 * 설정자: 정적 초기화. **Tegra210 만 true.**
	 * 읽는 자: tegra_pcie_port_enable 이 RP_VEND_CTL2 의 PCA_ENABLE 비트를
	 *   세울지 정한다.
	 * 값 범위: 이 약어가 무엇의 줄임인지는 이 트리에서 확인할 수 없다.
	 *   확실한 것은 Tegra210 에서만 이 비트를 세운다는 사실뿐이다.
	 * 동기화: 읽기 전용 상수 */
	bool force_pca_enable;
	/* [한국어] 이 드라이버가 PHY 를 직접 다루는가.
	 * 설정자: 정적 초기화. **Tegra186 만 false**, 나머지는 모두 true.
	 * 읽는 자: tegra_pcie_get_resources 가 PHY 를 얻을지, PM 콜백들이 PHY
	 *   전원을 켜고 끌지 정한다.
	 * 의미: false 면 PHY 를 다른 주체가 관리한다는 뜻이다. Tegra186 이
	 *   뒷세대로 넘어가는 과도기임을 보여 주는 값이며, 뒷세대
	 *   pcie-tegra194.c 는 UPHY 를 아예 BPMP 펌웨어에 맡긴다.
	 * 동기화: 읽기 전용 상수 */
	bool program_uphy;
	/* [한국어] 클록 클램프 임계값을 바꿔야 하는가.
	 * 설정자: 정적 초기화. Tegra124 와 Tegra210 만 true.
	 * 읽는 자: tegra_pcie_enable_rp_features 가 RP_PRIV_MISC 의 두 임계값
	 *   필드를 덮어쓸지 정한다.
	 * 동기화: 읽기 전용 상수 */
	bool update_clamp_threshold;
	/* [한국어] 디스큐 재시도 시간을 늘려야 하는가.
	 * 설정자: 정적 초기화. **Tegra210 만 true.**
	 * 읽는 자: tegra_pcie_apply_sw_fixup.
	 * 왜 필요한가: 그 함수 안의 원문 주석이 밝히듯 레인 0 의 디스큐 로직이
	 *   불안정해 Gen2 에서 Gen1 으로 낮출 때 실패하는 하드웨어 문제가 있다.
	 * 동기화: 읽기 전용 상수 */
	bool program_deskew_time;
	/* [한국어] 흐름 제어 타이머를 조정해야 하는가.
	 * 설정자: 정적 초기화. **Tegra210 만 true.**
	 * 읽는 자: tegra_pcie_apply_sw_fixup 이 update_fc_threshold 를 쓸지 정한다.
	 * 동기화: 읽기 전용 상수 */
	bool update_fc_timer;
	/* [한국어] 캐시 BAR 레지스터가 있는가.
	 * 설정자: 정적 초기화. **Tegra20 만 true** -- 즉 가장 오래된 SoC 에만 있고
	 *   이후 세대에서 사라진 레지스터다.
	 * 읽는 자: tegra_pcie_setup_translations 가 캐시 BAR 넷을 0 으로 지울지
	 *   정한다. 그 위의 원문 주석대로 상류 트랜잭션을 모두 uncached 로
	 *   다루겠다는 뜻이다.
	 * 동기화: 읽기 전용 상수 */
	bool has_cache_bars;
	struct {
		struct {
			/* [한국어] 이퀄라이저 레지스터 RP_ECTL_2_R1 의 하위 16비트에 넣을 값.
			 * 설정자: 정적 초기화. Tegra210 만 값을 갖는다(0x0000000f).
			 * 읽는 자: tegra_pcie_program_ectl_settings.
			 * **의미는 알 수 없다.** 필드 이름(RX_CTLE)이 수신단 이퀄라이저 관련임을
			 *   가리킬 뿐이며, 근거 문서가 이 트리에 없다. R1 조와 R2 조가 서로 다른
			 *   값을 받는다는 사실만 확인된다.
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_2_r1;
			/* [한국어] RP_ECTL_4_R1 의 비트 31:16 에 넣을 값(0x00000067).
			 * **이 필드만 시프트가 필요하다** -- tegra_pcie_program_ectl_settings 가
			 *   RP_ECTL_4_R1_RX_CDR_CTRL_1C_SHIFT(16)만큼 왼쪽으로 밀어 넣는다.
			 * 필드 이름(RX_CDR_CTRL)이 클록·데이터 복원 관련임을 가리킨다.
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_4_r1;
			/* [한국어] RP_ECTL_5_R1 에 넣을 값(0x55010000). 마스크가 0xffffffff 라 레지스터
			 * 전체를 덮어쓴다. 필드 이름은 RX_EQ_CTRL_L 이다.
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_5_r1;
			/* [한국어] RP_ECTL_6_R1 에 넣을 값(0x00000001). 역시 레지스터 전체를 덮어쓴다.
			 * 필드 이름은 RX_EQ_CTRL_H 이며, 위 필드와 L/H 짝을 이룬다.
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_6_r1;
			/* [한국어] RP_ECTL_2_R2 에 넣을 값(0x0000008f).
			 * **R1 조의 대응 값(0x0f)과 다르다** -- 두 조가 별개의 설정임을 보여 준다.
			 * 이름의 R1/R2 가 무엇을 뜻하는지는 이 트리에서 확인할 수 없으나,
			 * 속도 등급별 설정으로 보인다.
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_2_r2;
			/* [한국어] RP_ECTL_4_R2 의 비트 31:16 에 넣을 값(0x000000c7).
			 * 이 필드도 시프트가 필요하다.
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_4_r2;
			/* [한국어] RP_ECTL_5_R2 에 넣을 값(0x55010000). R1 조의 대응 값과 **같다.**
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_5_r2;
			/* [한국어] RP_ECTL_6_R2 에 넣을 값(0x00000001). 역시 R1 조와 같다.
			 * 동기화: 읽기 전용 상수 */
			u32 rp_ectl_6_r2;
		} regs;
		/* [한국어] 위 여덟 값을 실제로 쓸 것인가.
		 * 설정자: 정적 초기화. **Tegra210 만 true**, 나머지 넷은 ectl.enable = false
		 *   로 명시한다.
		 * 읽는 자: tegra_pcie_port_enable 이 tegra_pcie_program_ectl_settings 를
		 *   부를지 정한다.
		 * 의미: false 인 SoC 에서는 위 여덟 필드가 모두 0 이지만 쓰이지 않으므로
		 *   문제가 되지 않는다.
		 * 동기화: 읽기 전용 상수 */
		bool enable;
	} ectl;
};

struct tegra_pcie {
	/* [한국어] 이 컨트롤러의 플랫폼 device 포인터.
	 * 설정자: tegra_pcie_probe 가 &pdev->dev 로 채운다.
	 * 읽는 자: 거의 모든 함수. 로그, devm 자원 획득, 장치 트리 노드 접근,
	 *   전원 도메인 검사(dev->pm_domain)에 쓰인다.
	 * 동기화: 설정 후 불변 */
	struct device *dev;

	/* [한국어] PADS 블록의 가상 시작 주소.
	 * 설정자: tegra_pcie_get_resources 가 이름 "pads" 로 매핑한다.
	 * 읽는 자: pads_readl / pads_writel 를 통해서만 접근한다.
	 * 무엇이 있는가: 내장 PHY 와 PLL 제어 레지스터. **컨트롤러 전체에 하나뿐**
	 *   이라 포트별이 아니다.
	 * 특기할 점: 뒷세대 pcie-tegra194.c 에는 이에 해당하는 블록이 없다 --
	 *   UPHY 를 BPMP 펌웨어가 관리하기 때문이다.
	 * 동기화: devm 이 관리. 설정 후 불변 */
	void __iomem *pads;
	/* [한국어] AFI 블록의 가상 시작 주소.
	 * 설정자: tegra_pcie_get_resources 가 이름 "afi" 로 매핑한다.
	 * 읽는 자: afi_readl / afi_writel 를 통해서만 접근한다.
	 * 무엇이 있는가: 주소 변환 창(AXI BAR 와 FPCI BAR 짝), 오류 인터럽트,
	 *   MSI 수신 주소와 벡터 상태/활성화, 포트별 제어 레지스터, PME.
	 *   **이 파일에서 가장 많이 쓰이는 블록** 이다.
	 * 동기화: devm 이 관리. 설정 후 불변 */
	void __iomem *afi;
	/* [한국어] config 접근용 4KiB 창의 가상 주소.
	 * 설정자: tegra_pcie_get_resources 가 이름 "cs" 로 매핑한다.
	 *   크기를 SZ_4K 로 줄인 뒤 매핑하는 것이 핵심이다.
	 * 읽는 자: tegra_pcie_map_bus 가 버스 1 이상 경로에서 기준으로 삼는다.
	 * 왜 4KiB 인가: 하드웨어 config 주소 인코딩을 그대로 펼치면 256MiB 가
	 *   필요하기 때문이다. 그래서 창 하나만 두고 접근할 때마다
	 *   AFI_FPCI_BAR0 를 고쳐 **옮겨 가며** 쓴다.
	 * 동기화: devm 이 관리. 창의 위치(하드웨어 쪽)는 config 락이 직렬화한다 */
	void __iomem *cfg;
	/* [한국어] AFI 오류 인터럽트의 가상 IRQ 번호.
	 * 설정자: tegra_pcie_get_resources 가 platform_get_irq_byname("intr")로 얻는다.
	 * 읽는 자: request_irq / free_irq, 그리고 **tegra_pcie_map_irq** 가
	 *   장치 트리에서 INTx 를 못 찾았을 때 대체값으로 돌려준다.
	 * 특기할 점: MSI 선(msi.irq)과 별개다. 그리고 IRQF_SHARED 로 요청되는데,
	 *   INTx 가 같은 선을 탈 수 있기 때문이다 -- tegra_pcie_isr 이
	 *   AFI_INTR_LEGACY 코드에 IRQ_NONE 을 돌려주는 이유가 그것이다.
	 * 동기화: probe 때 정하고 이후 불변 */
	int irq;

	/* [한국어] config 창의 리소스 기술자 **사본**.
	 * 설정자: tegra_pcie_get_resources 가 platform 리소스를 복사한 뒤
	 *   resource_set_size 로 4KiB 로 줄인다.
	 * 읽는 자: tegra_pcie_setup_translations 가 AXI BAR0 의 시작 주소와
	 *   크기로 쓴다.
	 * 왜 사본인가: 원본을 줄이면 플랫폼 장치의 리소스 목록이 훼손되므로,
	 *   값으로 복사해 두고 그 사본만 줄인다.
	 * 동기화: probe 때 정하고 이후 불변 */
	struct resource cs;

	/* [한국어] PCIe 코어 클록.
	 * 설정자: tegra_pcie_clocks_get 이 이름 "pex" 로 얻는다.
	 * 읽는 자: tegra_pcie_pm_resume 이 켜고 _pm_suspend 가 끈다.
	 * 특기할 점: **tegra_pcie_power_on / _power_off 는 이 클록을 다루지 않는다.**
	 *   다른 셋과 켜고 끄는 시점이 달라 PM 콜백이 직접 관리한다.
	 * 동기화: 클록 프레임워크 내부 락 */
	struct clk *pex_clk;
	/* [한국어] AFI 블록 클록.
	 * 설정자: tegra_pcie_clocks_get 이 이름 "afi" 로 얻는다.
	 * 읽는 자: tegra_pcie_power_on 이 켜고 _power_off 가 끈다.
	 * 동기화: 클록 프레임워크 내부 락 */
	struct clk *afi_clk;
	/* [한국어] PLLE 클록.
	 * 설정자: tegra_pcie_clocks_get 이 이름 "pll_e" 로 얻는다.
	 * 읽는 자: tegra_pcie_power_on 이 마지막으로 켜고 _power_off 가 먼저 끈다.
	 * 관련: tegra_pcie_enable_controller 가 AFI_PLLE_CONTROL 로 이 PLL 의
	 *   전원 관리를 패드 쪽에 넘기고, tegra_pcie_port_disable 이 CLKREQ 를
	 *   GPIO 로 돌려 PLLE 가 잠들 수 있게 한다.
	 * 동기화: 클록 프레임워크 내부 락 */
	struct clk *pll_e;
	/* [한국어] CML 클록.
	 * 설정자: tegra_pcie_clocks_get 이 has_cml_clk 인 SoC 에서만 얻는다.
	 * 읽는 자: tegra_pcie_power_on / _power_off 가 같은 조건 아래 켜고 끈다.
	 * 값 범위: 지원하지 않는 SoC(Tegra20, Tegra186)에서는 NULL 로 남는다.
	 * 동기화: 클록 프레임워크 내부 락 */
	struct clk *cml_clk;

	/* [한국어] PCIe 블록 리셋.
	 * 설정자: tegra_pcie_resets_get 이 이름 "pex" 로 exclusive 획득.
	 * 읽는 자: tegra_pcie_power_on 이 어서트하고, **tegra_pcie_pm_resume 이**
	 *   클록을 켠 뒤 푼다. _pm_suspend 가 다시 어서트한다.
	 * 특기할 점: 리셋 셋의 해제 시점이 모두 다르다 -- 이 필드는 중간이다.
	 * 동기화: 리셋 프레임워크 내부 락 */
	struct reset_control *pex_rst;
	/* [한국어] AFI 블록 리셋.
	 * 설정자: tegra_pcie_resets_get 이 이름 "afi" 로 얻는다.
	 * 읽는 자: tegra_pcie_power_on 이 어서트했다가 **함수 끝에서 푼다.**
	 *   셋 중 가장 먼저 풀리는데, AFI 레지스터를 만져야 나머지 설정이
	 *   가능하기 때문이다. tegra_pcie_power_off 가 맨 먼저 어서트한다.
	 * 동기화: 리셋 프레임워크 내부 락 */
	struct reset_control *afi_rst;
	/* [한국어] **링크 훈련 시작 스위치.**
	 * 설정자: tegra_pcie_resets_get 이 이름 "pcie_x" 로 얻는다.
	 * 읽는 자: tegra_pcie_power_on 이 어서트하고,
	 *   **tegra_pcie_enable_ports 가 모든 포트 설정을 마친 뒤 푼다.**
	 *   그 순간 LTSSM 이 돌기 시작하며, 그 함수의 원문 주석이
	 *   "Start LTSSM from Tegra side" 라고 그 사실을 적고 있다.
	 *   tegra_pcie_disable_ports 가 다시 어서트해 LTSSM 을 멈춘다.
	 * 특기할 점: 셋 중 가장 늦게 풀린다. 이 순서가 이 드라이버 기동 절차의
	 *   뼈대다.
	 * 동기화: 리셋 프레임워크 내부 락 */
	struct reset_control *pcie_xrst;

	/* [한국어] 구형 PHY 바인딩을 쓰는가.
	 * 설정자: tegra_pcie_phys_get_legacy 가 true 로 표시한다. 신형 경로에서는
	 *   건드리지 않아 0(false)으로 남는다.
	 * 읽는 자: tegra_pcie_phy_power_on, _phy_power_off, _phys_put 세 곳이
	 *   각각 구형/신형 경로를 가르는 데 쓴다.
	 * 판단 기준: tegra_pcie_phys_get 이 정하는데, Gen2 미지원 SoC 이거나
	 *   장치 트리 컨트롤러 노드에 phys 속성이 있으면 구형이다.
	 * 동기화: probe 때 정하고 이후 불변 */
	bool legacy_phy;
	/* [한국어] 구형 바인딩의 컨트롤러 단위 PHY.
	 * 설정자: tegra_pcie_phys_get_legacy 가 optional 로 얻는다.
	 * 읽는 자: tegra_pcie_phy_power_on / _off 가 legacy_phy 경로에서 쓰고,
	 *   **tegra_pcie_enable_controller 가 PLLE 전원 관리를 켤지 판단하는 데도
	 *   쓴다**(pcie->phy 가 있을 때만).
	 * 값 범위: **NULL 일 수 있다.** 그 경우 PHY 드라이버가 아예 없다는 뜻이고,
	 *   tegra_pcie_phy_power_on 이 tegra_pcie_phy_enable 로 PADS 레지스터를
	 *   직접 만지는 경로를 탄다. 즉 이 필드의 NULL 여부가 세 번째 갈래를 만든다.
	 * 동기화: devm 이 관리. 설정 후 불변 */
	struct phy *phy;

	/* [한국어] MSI 상태 전체.
	 * 설정자: tegra_pcie_msi_setup 이 초기화한다.
	 * 읽는 자: MSI 관련 모든 함수.
	 * 특기할 점: **포인터가 아니라 값으로 박혀 있다.** 그래서 msi_to_pcie 가
	 *   container_of 로 바깥 구조체를 되찾을 수 있고, irq_chip 콜백들이
	 *   chip_data 로 이 필드의 주소를 받아 컨트롤러까지 거슬러 올라간다.
	 * 동기화: 내부의 두 락이 각 부분을 지킨다 */
	struct tegra_msi msi;

	/* [한국어] 루트 포트들의 연결 리스트 머리.
	 * 설정자: tegra_pcie_probe 가 초기화하고 tegra_pcie_parse_dt 가 채운다.
	 * 읽는 자: 포트를 순회하는 모든 함수, 그리고 tegra_pcie_map_bus 가
	 *   버스 0 접근에서 슬롯에 맞는 포트를 찾는 데 쓴다.
	 * 특기할 점: **런타임에 줄어들 수 있다.** tegra_pcie_enable_ports 가 링크가
	 *   안 선 포트를 tegra_pcie_port_free 로 리스트에서 빼기 때문이다.
	 *   그 뒤로 그 포트는 config 접근에서도 debugfs 목록에서도 사라진다.
	 * 동기화: 락이 없다. 리스트를 바꾸는 것은 probe 와 절전 경로뿐이라
	 *   실질적 경쟁이 없다는 전제다 */
	struct list_head ports;
	/* [한국어] 레인 배분(xbar) 설정 값.
	 * 설정자: tegra_pcie_parse_dt 가 tegra_pcie_get_xbar_config 를 통해 채운다.
	 * 읽는 자: tegra_pcie_enable_controller 가 AFI_PCIE_CONFIG 의 xbar 필드에
	 *   그대로 넣는다.
	 * 값 범위: AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_ 계열 상수 중 하나.
	 *   그 상수들은 값이 겹치므로(SINGLE/420/X2_X1/401 이 모두 0x0 << 20),
	 *   이름이 다르다고 값이 다르지 않다.
	 * 특기할 점: **앞 세대 고유의 개념** 이다. 뒷세대는 컨트롤러마다 장치
	 *   노드가 따로라 이 개념이 없다.
	 * 동기화: probe 때 정하고 이후 불변 */
	u32 xbar_config;

	/* [한국어] 이 SoC 가 필요로 하는 regulator 배열.
	 * 설정자: tegra_pcie_get_regulators 또는 _get_legacy_regulators.
	 * 읽는 자: tegra_pcie_power_on / _power_off 가 bulk 로 켜고 끈다.
	 * 값 범위: num_supplies 개의 원소. SoC 마다 2~8개다.
	 * 특기할 점: 새 바인딩으로 잡았다가 장치 트리에 다 없으면 **devm_kfree 로
	 *   버리고** 구형 목록으로 다시 잡는 경로가 있다.
	 * 동기화: devm 이 관리 */
	struct regulator_bulk_data *supplies;
	/* [한국어] 위 배열의 원소 개수.
	 * 설정자: 두 regulator 획득 함수. 구형 경로로 넘어갈 때 **0 으로 되돌린 뒤**
	 *   다시 채운다.
	 * 읽는 자: regulator_bulk_enable / _disable 의 첫 인자.
	 * 값 범위: 2(Tegra20 구형)에서 8(Tegra30 신형, PEXA 와 PEXB 를 모두 쓸 때)까지.
	 * 동기화: probe 때 정하고 이후 불변 */
	unsigned int num_supplies;

	/* [한국어] 이 SoC 의 기술자 포인터.
	 * 설정자: tegra_pcie_probe 가 of_device_get_match_data 로 얻는다.
	 *   장치 트리의 compatible 이 어느 항목과 맞았느냐로 정해진다.
	 * 읽는 자: **이 파일의 모든 세대 분기가 이 포인터를 통과한다.**
	 * 값 범위: tegra20_pcie / tegra30_pcie / tegra124_pcie / tegra210_pcie /
	 *   tegra186_pcie 다섯 중 하나. of_match_table 에 없는 compatible 로는
	 *   probe 가 불리지 않으므로 NULL 이 될 수 없다.
	 * 동기화: 읽기 전용 상수를 가리키며 설정 후 불변 */
	const struct tegra_pcie_soc *soc;
	/* [한국어] debugfs 디렉터리 핸들.
	 * 설정자: tegra_pcie_debugfs_init 이 채운다(CONFIG_DEBUG_FS 일 때만).
	 * 읽는 자: 파일을 만들 때 부모로 쓴다.
	 * 특기할 점: **이 파일 어디에서도 정리하지 않는다.** debugfs_remove 호출이
	 *   없어, 드라이버가 떨어져도 디렉터리가 남는다. 다만 이 드라이버는
	 *   builtin_platform_driver 이고 suppress_bind_attrs 가 true 라 언바인드
	 *   경로가 사실상 없다. 뒷세대 pcie-tegra194.c 는
	 *   debugfs_remove_recursive 를 부른다.
	 * 동기화: 없음 */
	struct dentry *debugfs;
};

/* [한국어]
 * msi_to_pcie - MSI 상태에서 컨트롤러 상태를 되찾는다
 *
 * @msi: struct tegra_pcie 안에 값으로 박혀 있는 msi 필드의 주소.
 * @return: 그 msi 를 품고 있는 struct tegra_pcie 포인터.
 *
 * struct tegra_msi 는 독립된 할당물이 아니라 struct tegra_pcie 의 필드다.
 * 그래서 msi 포인터만 있으면 container_of 로 바깥 구조체를 계산할 수 있다.
 *
 * 이 함수가 필요한 이유는 irq_chip 콜백들이 chip_data 로 **msi** 를 받기
 * 때문이다(tegra_msi_domain_alloc 이 domain->host_data 를 심는데 그 값이
 * msi 다). 그런데 실제 레지스터 접근에는 pcie->afi 가 필요하므로,
 * 그때마다 이 함수로 바깥으로 나온다.
 *
 * 같은 관용구를 뒷세대 pcie-tegra194.c 의 to_tegra_pcie 가 쓴다. 그쪽은
 * struct dw_pcie 를 품고 있어 DWC 코어가 넘겨 준 포인터에서 바깥으로
 * 나오는 용도다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트 포함 어디서나. 포인터 산술뿐이라
 * 잠들지도 락을 쓰지도 않는다.
 *
 * 호출 체인:
 *   tegra_msi_irq_ack / _mask / _unmask, tegra_allocate_domains → [이 함수]
 */
static inline struct tegra_pcie *msi_to_pcie(struct tegra_msi *msi)
{
	return container_of(msi, struct tegra_pcie, msi);
}

struct tegra_pcie_port {
	/* [한국어] 이 포트를 품은 컨트롤러.
	 * 설정자: tegra_pcie_parse_dt 가 포트를 만들 때 채운다.
	 * 읽는 자: 포트 단위 함수들이 AFI 레지스터나 로그에 접근할 때 거슬러
	 *   올라가는 통로다.
	 * 동기화: 설정 후 불변 */
	struct tegra_pcie *pcie;
	/* [한국어] 이 포트의 장치 트리 노드.
	 * 설정자: tegra_pcie_parse_dt.
	 * 읽는 자: tegra_pcie_port_get_phys 가 레인별 PHY 를 찾는 데 쓴다.
	 * 주의: for_each_child_of_node_scoped 로 순회하며 저장한 포인터다.
	 *   그 매크로는 순회가 끝날 때 참조를 놓으므로, 이 필드는 참조를 쥐지
	 *   않은 포인터다. 실제로 쓰이는 곳이 probe 경로뿐이라 문제가 되지 않는다.
	 * 동기화: 설정 후 불변 */
	struct device_node *np;
	/* [한국어] pcie->ports 리스트에 매다는 고리.
	 * 설정자: tegra_pcie_parse_dt 가 INIT_LIST_HEAD 후 list_add_tail 한다.
	 * 읽는 자: 모든 포트 순회, 그리고 tegra_pcie_ports_seq_show 가
	 *   list_entry 로 이 고리에서 포트를 되찾는다.
	 * 특기할 점: tegra_pcie_port_free 가 list_del 로 뺀다.
	 * 동기화: 리스트에 락이 없다 */
	struct list_head list;
	/* [한국어] 이 포트의 레지스터 창 기술자.
	 * 설정자: tegra_pcie_parse_dt 가 of_address_to_resource 로 채운다.
	 * 읽는 자: tegra_pcie_port_free 가 영역을 놓을 때 시작 주소와 크기로 쓴다.
	 * 동기화: 설정 후 불변 */
	struct resource regs;
	/* [한국어] 이 포트의 레지스터 블록 가상 주소.
	 * 설정자: tegra_pcie_parse_dt 가 devm_pci_remap_cfg_resource 로 매핑한다.
	 * 읽는 자: **두 종류의 접근이 섞인다.** 하나는 포트 자신의 config 공간
	 *   으로서의 접근(tegra_pcie_map_bus 의 버스 0 경로)이고, 다른 하나는
	 *   NVIDIA 고유 확장 레지스터(RP_ 계열) 접근이다. 후자는
	 *   tegra_pcie_enable_rp_features, _program_ectl_settings, _apply_sw_fixup,
	 *   _port_check_link, _change_link_speed, _ports_seq_show 가 쓴다.
	 *   즉 표준 config 공간과 벤더 확장이 같은 창에 겹쳐 있다.
	 * 동기화: devm 이 관리. config 접근은 PCI 코어의 락이 직렬화한다 */
	void __iomem *base;
	/* [한국어] 포트 번호. **0부터 센다.**
	 * 설정자: tegra_pcie_parse_dt 가 장치 트리의 슬롯 번호에서 1 을 빼 넣는다.
	 * 읽는 자: 제어 레지스터 선택, PME 비트 표 색인, AFI_PCIE_CONFIG 의
	 *   포트별 비트, 로그, 그리고 tegra_pcie_map_bus 가 slot 과 비교할 때
	 *   다시 1 을 더한다.
	 * 값 범위: 0 ~ (soc->num_ports - 1).
	 * 주의: **장치 트리는 1부터 세고 드라이버는 0부터 센다.** 이 변환이
	 *   파싱과 config 접근 두 곳에 나뉘어 있어 헷갈리기 쉽다.
	 * 동기화: 설정 후 불변 */
	unsigned int index;
	/* [한국어] 이 포트에 배정된 레인 수.
	 * 설정자: tegra_pcie_parse_dt 가 장치 트리의 nvidia,num-lanes 에서 읽는다.
	 * 읽는 자: tegra_pcie_port_get_phys 가 PHY 배열 크기로,
	 *   tegra_pcie_port_phy_power_on / _off 가 순회 횟수로 쓴다. 로그에도 찍힌다.
	 * 값 범위: 1~16. 그 범위를 벗어나면 tegra_pcie_parse_dt 가 probe 를 중단한다.
	 * 특기할 점: 이 값 자체는 하드웨어에 직접 쓰이지 않는다. 포트별 레인 수는
	 *   xbar_config 로 뭉쳐서 전달된다.
	 * 동기화: 설정 후 불변 */
	unsigned int lanes;

	/* [한국어] 레인별 PHY 배열 (신형 바인딩).
	 * 설정자: tegra_pcie_port_get_phys 가 lanes 개만큼 잡아 채운다.
	 * 읽는 자: tegra_pcie_port_phy_power_on / _off, tegra_pcie_phys_put.
	 * 값 범위: lanes 개의 원소. 구형 바인딩(legacy_phy)에서는 **NULL 로 남고
	 *   아무도 읽지 않는다** -- 그쪽은 pcie->phy 하나를 쓰기 때문이다.
	 * 동기화: devm 이 관리. 설정 후 불변 */
	struct phy **phys;

	/* [한국어] 이 포트의 PERST GPIO.
	 * 설정자: tegra_pcie_parse_dt 가 devm_fwnode_gpiod_get 으로 얻는다.
	 * 읽는 자: tegra_pcie_port_reset 하나뿐이다.
	 * 값 범위: **NULL 일 수 있다.** 장치 트리에 reset-gpios 가 없으면
	 *   -ENOENT 가 오는데, 그것을 오류로 보지 않고 NULL 로 바꾼다. 그 위의
	 *   원문 주석이 그 처리와 대체 수단을 함께 밝힌다 -- GPIO 가 없으면
	 *   AFI 포트별 레지스터로 PERST SFIO 선을 흔든다.
	 * 주의: **두 방식의 극성이 반대다.** GPIO 는 1 을 써서 리셋에 넣지만,
	 *   AFI 비트는 지워서 리셋에 넣는다.
	 * 동기화: devm 이 관리. 설정 후 불변 */
	struct gpio_desc *reset_gpio;
};

/* [한국어]
 * afi_writel - AFI 블록 레지스터에 쓴다
 *
 * @pcie:   컨트롤러 상태. afi 가 매핑되어 있어야 한다.
 * @value:  쓸 값.
 * @offset: AFI 블록 안의 바이트 오프셋(AFI_ 로 시작하는 상수들).
 * @return: 없음.
 *
 * AFI 는 이 컨트롤러에서 주소 변환과 인터럽트를 담당하는 레지스터 블록이다.
 * 이름을 풀어 쓴 곳이 이 트리에 없으나, 레지스터 이름이 AFI_AXI_BAR 계열과
 * AFI_FPCI_BAR 계열로 짝지어 있는 것으로 보아 SoC 쪽 버스(AXI)와 PCIe 쪽
 * 내부 주소 공간(FPCI) 사이를 잇는 블록이다.
 *
 * 인자 순서에 주의 -- **값이 먼저, 오프셋이 나중** 이다. 커널의 writel 과
 * 같은 순서이지만, 오프셋을 먼저 쓰는 관례에 익숙하면 헷갈리기 쉽다.
 *
 * writel(완화되지 않은 판)을 쓰므로 앞선 메모리 접근에 대한 순서가 보장된다.
 * 리셋 해제나 전원 인가 직전후에 쓰이는 레지스터가 많아 순서가 중요하다.
 *
 * 실행 컨텍스트: 어디서나. 인터럽트 핸들러와 irq_chip 콜백도 이 함수를 쓴다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → writel
 */
static inline void afi_writel(struct tegra_pcie *pcie, u32 value,
			      unsigned long offset)
{
	writel(value, pcie->afi + offset);
}

/* [한국어]
 * afi_readl - AFI 블록 레지스터를 읽는다
 *
 * @pcie:   컨트롤러 상태.
 * @offset: AFI 블록 안의 바이트 오프셋.
 * @return: 읽은 32비트 값.
 *
 * afi_writel 의 짝이다. 이 파일의 레지스터 갱신은 대부분
 * "읽고 → 비트 고치고 → 쓰기" 세 줄이라 두 함수가 늘 붙어 다닌다.
 *
 * 실행 컨텍스트: 어디서나.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → readl
 */
static inline u32 afi_readl(struct tegra_pcie *pcie, unsigned long offset)
{
	return readl(pcie->afi + offset);
}

/* [한국어]
 * pads_writel - PADS 블록 레지스터에 쓴다
 *
 * @pcie:   컨트롤러 상태. pads 가 매핑되어 있어야 한다.
 * @value:  쓸 값.
 * @offset: PADS 블록 안의 바이트 오프셋(PADS_ 로 시작하는 상수들).
 * @return: 없음.
 *
 * PADS 는 내장 PHY 와 그 PLL 을 다루는 레지스터 블록이다. AFI 와 달리
 * **컨트롤러 전체에 하나뿐** 이라 포트별이 아니다 -- 그래서 인자가
 * tegra_pcie 이지 tegra_pcie_port 가 아니다.
 *
 * 이 블록을 쓰는 곳은 두 갈래뿐이다. 하나는 내장 PHY 를 직접 켜고 끄는
 * tegra_pcie_phy_enable / _disable 이고, 다른 하나는 참조 클록 드라이버
 * 설정을 쓰는 tegra_pcie_apply_pad_settings 다.
 *
 * 뒷세대 pcie-tegra194.c 에는 이런 함수가 없다. 그쪽은 UPHY 를 드라이버가
 * 직접 만지지 않고 BPMP 펌웨어에 맡기기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(전원 인가 경로)에서만 쓰인다.
 *
 * 호출 체인:
 *   tegra_pcie_phy_enable / _disable, tegra_pcie_apply_pad_settings
 *     → [이 함수] → writel
 */
static inline void pads_writel(struct tegra_pcie *pcie, u32 value,
			       unsigned long offset)
{
	writel(value, pcie->pads + offset);
}

/* [한국어]
 * pads_readl - PADS 블록 레지스터를 읽는다
 *
 * @pcie:   컨트롤러 상태.
 * @offset: PADS 블록 안의 바이트 오프셋.
 * @return: 읽은 32비트 값.
 *
 * pads_writel 의 짝이다. PLL 잠금 비트를 폴링하는 tegra_pcie_pll_wait 이
 * 이 함수를 반복해서 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_pll_wait, tegra_pcie_phy_enable / _disable → [이 함수] → readl
 */
static inline u32 pads_readl(struct tegra_pcie *pcie, unsigned long offset)
{
	return readl(pcie->pads + offset);
}

/*
 * The configuration space mapping on Tegra is somewhat similar to the ECAM
 * defined by PCIe. However it deviates a bit in how the 4 bits for extended
 * register accesses are mapped:
 *
 *    [27:24] extended register number
 *    [23:16] bus number
 *    [15:11] device number
 *    [10: 8] function number
 *    [ 7: 0] register number
 *
 * Mapping the whole extended configuration space would require 256 MiB of
 * virtual address space, only a small part of which will actually be used.
 *
 * To work around this, a 4 KiB region is used to generate the required
 * configuration transaction with relevant B:D:F and register offset values.
 * This is achieved by dynamically programming base address and size of
 * AFI_AXI_BAR used for end point config space mapping to make sure that the
 * address (access to which generates correct config transaction) falls in
 * this 4 KiB region.
 */
/* [한국어]
 * tegra_pcie_conf_offset - B:D:F 와 레지스터 번호를 하드웨어 주소로 인코딩한다
 *
 * @bus:   버스 번호(0~255).
 * @devfn: 장치/함수 번호. 상위 5비트가 장치, 하위 3비트가 함수다.
 * @where: config 공간 안의 바이트 오프셋(0~4095).
 * @return: 이 하드웨어가 요구하는 형식의 주소 오프셋.
 *
 * 위의 원문 주석이 비트 배치를 그대로 적고 있다. ECAM 과 비슷하되
 * **확장 레지스터 4비트의 자리가 다른** 것이 핵심이다.
 *   [27:24] 확장 레지스터 번호  ← where 의 비트 11:8
 *   [23:16] 버스 번호
 *   [15:11] 장치 번호
 *   [10: 8] 함수 번호
 *   [ 7: 0] 레지스터 번호       ← where 의 비트 7:0
 *
 * 그래서 where 를 두 조각으로 나눠 서로 다른 자리에 넣는다.
 * (where & 0xf00) << 16 이 확장 부분을 비트 27:24 로 올리고,
 * where & 0xff 가 하위 부분을 그대로 둔다. 표준 ECAM 이었다면 where 를
 * 통째로 하위에 두면 됐을 것이므로, 이 두 줄이 곧 "표준에서 벗어난
 * 부분" 이다.
 *
 * 이 인코딩을 그대로 주소 공간에 펼치면 비트 27:0 이므로 256MiB 가 필요하다.
 * 그래서 tegra_pcie_map_bus 가 4KiB 창 하나만 두고 옮겨 쓴다.
 *
 * 실행 컨텍스트: config 락 안(map_bus 안에서 불린다). 순수 계산이다.
 *
 * 호출 체인:
 *   tegra_pcie_map_bus → [이 함수]
 */
static unsigned int tegra_pcie_conf_offset(u8 bus, unsigned int devfn,
					   unsigned int where)
{
	return ((where & 0xf00) << 16) | (bus << 16) | (PCI_SLOT(devfn) << 11) |
	       (PCI_FUNC(devfn) << 8) | (where & 0xff);
}

/* [한국어]
 * tegra_pcie_map_bus - config 접근 주소를 만든다. 필요하면 창을 옮긴다
 *
 * @bus:   접근할 버스. sysdata 에 struct tegra_pcie 가 들어 있다.
 * @devfn: 장치/함수 번호.
 * @where: config 오프셋.
 * @return: 접근할 MMIO 주소. 버스 0 에서 해당 슬롯의 포트를 못 찾으면 NULL.
 *
 * **이 드라이버에서 가장 특이한 함수** 다. 보통의 map_bus 는 주소를
 * 계산해 돌려주기만 하지만, 이 함수는 경로에 따라 **하드웨어 상태를 바꾼다.**
 *
 * 버스 0 (루트 포트 자신):
 *   슬롯 번호로 포트를 찾는다. port->index + 1 == slot 인 것은 장치 트리가
 *   포트를 1부터 세기 때문이다(tegra_pcie_parse_dt 가 index-- 로 0부터로
 *   바꿔 저장한다). 찾으면 그 포트의 레지스터 블록에서 dword 정렬한
 *   주소를 돌려준다. 못 찾으면 addr 이 NULL 로 남아, 호출자인
 *   pci_generic_config_read32 가 PCIBIOS_DEVICE_NOT_FOUND 로 처리한다 --
 *   즉 **없는 포트를 조용히 걸러 내는 장치** 이기도 하다.
 *
 * 버스 1 이상 (아래 매달린 실제 장치):
 *   하드웨어 인코딩으로 오프셋을 만든 뒤, 그 오프셋이 속한 4KiB 페이지의
 *   기준 주소를 AFI_FPCI_BAR0 에 **써 넣어 창을 옮긴다.**
 *   0xfe100000 은 위의 원문 주석이 적은 FPCI 지도에서 "type 1 확장 config
 *   공간" 의 시작이고, >> 8 은 그 레지스터가 요구하는 축척이다
 *   (이 축척의 근거 문서는 이 트리에 없다). 그리고 창 안의 위치를 더해
 *   돌려준다.
 *
 * 순서 의존이 생긴다 -- 창을 옮기는 쓰기와 그 창에 대한 접근 사이에
 * 다른 BDF 의 접근이 끼어들면 엉뚱한 곳을 읽는다. PCI 코어의 config 락이
 * 그 구간을 직렬화해 주기 때문에 성립한다.
 *
 * **뒷세대와의 대비**: pcie-tegra194.c 의 map_bus 는
 * dw_pcie_own_conf_map_bus(pcie-designware-host.c:1951) 한 줄이다. 창을
 * 옮기는 개념 자체가 없다.
 *
 * 실행 컨텍스트: PCI 코어의 config 락 안. 잠들 수 없다.
 *
 * 호출 체인:
 *   pci_generic_config_read / _write 계열 → [이 함수]
 *     → tegra_pcie_conf_offset, afi_writel
 */
static void __iomem *tegra_pcie_map_bus(struct pci_bus *bus,
					unsigned int devfn,
					int where)
{
	struct tegra_pcie *pcie = bus->sysdata;
	/* [한국어] **NULL 로 시작한다.** 버스 0 에서 해당 슬롯의 포트를 못 찾으면 그대로
	 * 남고, 호출자인 pci_generic_config_read32 가 PCIBIOS_DEVICE_NOT_FOUND 로
	 * 처리한다 -- 즉 없는 포트를 조용히 걸러 내는 장치이기도 하다 */
	void __iomem *addr = NULL;

	/* [한국어] **버스 0 이면 루트 포트 자신이다** */
	if (bus->number == 0) {
		/* [한국어] devfn 에서 슬롯 번호를 뽑는다 */
		unsigned int slot = PCI_SLOT(devfn);
		/* [한국어] 찾을 포트 */
		struct tegra_pcie_port *port;

		/* [한국어] 포트 리스트에서 이 슬롯에 해당하는 포트를 찾는다 */
		list_for_each_entry(port, &pcie->ports, list) {
			/* [한국어] **장치 트리는 포트를 1부터 세므로 내부 번호에 1 을 더해 비교한다.**
			 * tegra_pcie_parse_dt 의 index-- 와 짝을 이루는 변환이다 */
			if (port->index + 1 == slot) {
				/* [한국어] **포트 레지스터를 직접 가리킨다.** dword 로 정렬하는 것은 이 블록이
				 * dword 접근만 허용하기 때문이며, 호출자가 32비트 전용 판을 쓰는 것과
				 * 의도가 겹친다 */
				addr = port->base + (where & ~3);
				break;
			}
		}
	} else {
		/* [한국어] 하드웨어 형식의 주소 오프셋 */
		unsigned int offset;
		/* [한국어] 옮길 창의 기준 주소 */
		u32 base;

		/* [한국어] 하드웨어 인코딩으로 오프셋을 만든다 */
		offset = tegra_pcie_conf_offset(bus->number, devfn, where);

		/* move 4 KiB window to offset within the FPCI region */
		base = 0xfe100000 + ((offset & ~(SZ_4K - 1)) >> 8);
		/* [한국어] **창을 목표 지점으로 옮긴다.** 이 쓰기가 이 함수를 단순한 주소 계산
		 * 함수가 아니게 만든다. PCI 코어의 config 락이 이 쓰기와 이어지는
		 * 창 접근 사이를 직렬화해 주기 때문에 성립한다 */
		afi_writel(pcie, base, AFI_FPCI_BAR0);

		/* move to correct offset within the 4 KiB page */
		addr = pcie->cfg + (offset & (SZ_4K - 1));
	}

	return addr;
}

/* [한국어]
 * tegra_pcie_config_read - config 읽기. 버스에 따라 접근 폭이 갈린다
 *
 * @bus:   대상 버스.
 * @devfn: 대상 장치/함수.
 * @where: config 오프셋.
 * @size:  읽을 바이트 수(1/2/4).
 * @value: 읽은 값을 담을 곳.
 * @return: 커널 공통 함수의 반환값.
 *
 * **버스 0 이냐 아니냐로 커널 공통 함수를 갈라 부르는 것이 요점이다.**
 *
 * 버스 0 은 pci_generic_config_read32(drivers/pci/access.c:431)를 쓴다.
 * 그 판은 map_bus 에 (where & ~0x3) 을 넘기고 항상 32비트를 읽은 뒤
 * 소프트웨어로 잘라 낸다. 루트 포트 레지스터 블록이 dword 접근만
 * 허용하기 때문이며, map_bus 자신도 버스 0 경로에서 (where & ~3) 으로
 * 정렬하고 있어 의도가 두 곳에 드러나 있다.
 *
 * 버스 1 이상은 pci_generic_config_read(access.c:321)를 쓴다. 그 판은
 * map_bus 에 정렬하지 않은 where 를 그대로 넘기고(access.c:328) 요청한
 * 크기 그대로 읽는다. config 창 쪽은 바이트 단위 접근이 가능하기 때문이다.
 *
 * 읽기는 부작용이 없으므로 넓게 읽어도 무방하지만, 여기서는 하드웨어
 * 제약이 이유라 크기를 줄이는 것이 아니라 늘리는 방향이다.
 *
 * 실행 컨텍스트: config 락 안.
 *
 * 호출 체인:
 *   pci_read_config_ 계열 → PCI 코어 → tegra_pcie_ops.read → [이 함수]
 */
static int tegra_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *value)
{
	if (bus->number == 0)
		/* [한국어] **버스 0 은 32비트 전용 판을 쓴다.** 그 판은 map_bus 에
		 * (where & ~0x3) 을 넘기고 항상 32비트를 읽은 뒤 소프트웨어로 잘라 낸다
		 * (drivers/pci/access.c:438). 루트 포트 레지스터가 dword 접근만 허용하기
		 * 때문이다 */
		return pci_generic_config_read32(bus, devfn, where, size,
						 value);

	return pci_generic_config_read(bus, devfn, where, size, value);
}

/* [한국어]
 * tegra_pcie_config_write - config 쓰기. 버스에 따라 접근 폭이 갈린다
 *
 * @bus:   대상 버스.
 * @devfn: 대상 장치/함수.
 * @where: config 오프셋.
 * @size:  쓸 바이트 수(1/2/4).
 * @value: 쓸 값.
 * @return: 커널 공통 함수의 반환값.
 *
 * tegra_pcie_config_read 와 같은 기준으로 갈린다.
 *
 * 버스 0 은 pci_generic_config_write32 를 쓴다. size 가 4 가 아니면 그 판이
 * **읽고-고쳐-쓰기** 를 한다. config 공간에는 읽으면 지워지는 비트가 있어
 * 일반적으로 위험한 방식이지만, 여기서는 하드웨어가 dword 접근만 허용하므로
 * 달리 방법이 없다. 이 점이 pcie-mediatek-gen3.c 와 대비되는데, 그쪽은
 * 하드웨어 byte enable 이 있어 size 를 4 로 바꿔 넘겨 그 경로를 피한다.
 *
 * 버스 1 이상은 pci_generic_config_write 를 써서 요청한 크기 그대로 쓴다.
 *
 * 실행 컨텍스트: config 락 안.
 *
 * 호출 체인:
 *   pci_write_config_ 계열 → PCI 코어 → tegra_pcie_ops.write → [이 함수]
 */
static int tegra_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 value)
{
	if (bus->number == 0)
		/* [한국어] **버스 0 은 32비트 전용 판을 쓴다.** size 가 4 가 아니면 그 판이
		 * 읽고-고쳐-쓰기를 하는데, 하드웨어가 dword 접근만 허용하므로
		 * 달리 방법이 없다 */
		return pci_generic_config_write32(bus, devfn, where, size,
						  value);

	/* [한국어] 버스 1 이상은 요청한 크기 그대로 쓴다 */
	return pci_generic_config_write(bus, devfn, where, size, value);
}

/* [한국어] PCI 코어가 이 하드웨어에 닿는 유일한 통로 */
static struct pci_ops tegra_pcie_ops = {
	/* [한국어] **map_bus 가 창을 옮기는 부작용을 갖는다.** 보통의 map_bus 는 주소를
	 * 계산해 돌려주기만 하는데, 이 드라이버는 다르다 */
	.map_bus = tegra_pcie_map_bus,
	.read = tegra_pcie_config_read,
	.write = tegra_pcie_config_write,
};

/* [한국어]
 * tegra_pcie_port_get_pex_ctrl - 포트 번호에 해당하는 제어 레지스터 오프셋을 고른다
 *
 * @port: 대상 포트. index 가 0, 1, 2 중 하나다.
 * @return: AFI 블록 안의 제어 레지스터 오프셋. 알 수 없는 인덱스면 0.
 *
 * 포트마다 참조 클록과 리셋을 다루는 제어 레지스터가 하나씩 있는데,
 * 그 오프셋이 규칙적이지 않다. 포트 0 과 1 은 고정값(AFI_PEX0_CTRL 0x110,
 * AFI_PEX1_CTRL 0x118)이지만 **포트 2 는 SoC 마다 다르다** --
 * Tegra30 은 0x128, Tegra186 은 0x19c 다. 그래서 세 번째만 soc 기술자에서
 * 가져온다.
 *
 * 인덱스가 셋 중 어느 것도 아니면 0 을 돌려주는데, 0 은 AFI_AXI_BAR0_SZ 의
 * 오프셋이라 그곳을 건드리면 주소 변환이 망가진다. 다만
 * tegra_pcie_parse_dt 가 인덱스를 soc->num_ports 이하로 검증하고 포트가
 * 최대 3개이므로 실제로는 도달하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 계산이다.
 *
 * 호출 체인:
 *   tegra_pcie_port_reset / _port_enable / _port_disable → [이 함수]
 */
static unsigned long tegra_pcie_port_get_pex_ctrl(struct tegra_pcie_port *port)
{
	const struct tegra_pcie_soc *soc = port->pcie->soc;
	/* [한국어] **기본값이 0 이다.** 0 은 AFI_AXI_BAR0_SZ 의 오프셋이라 그곳을 건드리면
	 * 주소 변환이 망가지지만, tegra_pcie_parse_dt 가 인덱스를 검증하고
	 * 포트가 최대 3개이므로 실제로는 도달하지 않는다 */
	unsigned long ret = 0;

	/* [한국어] 포트 번호로 갈린다 */
	switch (port->index) {
	/* [한국어] 포트 0 은 고정 오프셋이다 */
	case 0:
		ret = AFI_PEX0_CTRL;
		break;

	/* [한국어] 포트 1 은 고정 오프셋이다 */
	case 1:
		ret = AFI_PEX1_CTRL;
		break;

	/* [한국어] **세 번째 포트만 SoC 마다 오프셋이 다르다** -- Tegra30 은 0x128,
	 * Tegra186 은 0x19c 다. 그래서 이것만 기술자에서 가져온다 */
	case 2:
		ret = soc->afi_pex2_ctrl;
		break;
	}

	return ret;
}

/* [한국어]
 * tegra_pcie_port_reset - 포트에 리셋 펄스를 준다
 *
 * @port: 리셋할 포트.
 * @return: 없음.
 *
 * 슬롯에 꽂힌 장치로 나가는 PERST 신호를 내렸다가 올린다. 링크를 처음
 * 세울 때와, 링크가 안 서서 재시도할 때 쓰인다.
 *
 * **신호를 만드는 방법이 두 갈래** 인 것이 이 함수의 요점이다.
 *   reset_gpio 가 있으면 GPIO 를 직접 흔든다. 장치 트리에 reset-gpios 가
 *     있는 보드다.
 *   없으면 AFI 제어 레지스터의 리셋 비트를 흔든다. 이때 SFIO 로 나가는
 *     PERST 선을 쓰는 셈이다(tegra_pcie_parse_dt 위의 원문 주석이 이
 *     대체 관계를 밝히고 있다).
 *
 * 극성이 반대인 데 주의한다. GPIO 는 1 을 써서 리셋에 넣고 0 으로 푼다.
 * AFI 비트는 **지워서** 리셋에 넣고 세워서 푼다(AFI_PEX_CTRL_RST 를
 * &= ~ 로 지웠다가 |= 로 세운다). 같은 "리셋 어서트" 가 한쪽은 1,
 * 다른 쪽은 0 이다.
 *
 * 펄스 폭은 1~2ms 다. usleep_range 로 상한을 두 배 주는 것은 커널이
 * 타이머를 뭉쳐 처리할 여지를 주는 관례다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 * tegra_pcie_port_check_link 의 재시도 루프 안에서도 불린다.
 *
 * 호출 체인:
 *   tegra_pcie_port_enable / tegra_pcie_port_check_link → [이 함수]
 */
static void tegra_pcie_port_reset(struct tegra_pcie_port *port)
{
	unsigned long ctrl = tegra_pcie_port_get_pex_ctrl(port);
	/* [한국어] 레지스터 값 임시 변수 */
	unsigned long value;

	/* pulse reset signal */
	if (port->reset_gpio) {
		/* [한국어] **GPIO 는 1 을 써서 리셋에 넣는다** -- 아래 AFI 방식과 극성이 반대다 */
		gpiod_set_value(port->reset_gpio, 1);
	} else {
		/* [한국어] 현재 값을 읽는다 */
		value = afi_readl(port->pcie, ctrl);
		/* [한국어] **비트를 지우는 것이 리셋 어서트다** */
		value &= ~AFI_PEX_CTRL_RST;
		/* [한국어] 리셋을 어서트한다 */
		afi_writel(port->pcie, value, ctrl);
	}

	/* [한국어] 펄스 폭 1~2ms. 상한을 두 배 주는 것은 커널이 타이머를 뭉쳐 처리할
	 * 여지를 주는 관례다 */
	usleep_range(1000, 2000);

	/* [한국어] 다시 갈래를 확인한다 */
	if (port->reset_gpio) {
		gpiod_set_value(port->reset_gpio, 0);
	} else {
		/* [한국어] 현재 값을 읽는다 */
		value = afi_readl(port->pcie, ctrl);
		/* [한국어] **비트를 세우는 것이 리셋 해제다.** GPIO 방식과 극성이 반대다 */
		value |= AFI_PEX_CTRL_RST;
		/* [한국어] 리셋을 푼다 */
		afi_writel(port->pcie, value, ctrl);
	}
}

/* [한국어]
 * tegra_pcie_enable_rp_features - 루트 포트의 NVIDIA 고유 기능을 켠다
 *
 * @port: 대상 포트.
 * @return: 없음.
 *
 * 포트 레지스터 블록(port->base)의 RP_ 계열 레지스터를 만진다. 이들은
 * PCIe 규격 레지스터가 아니라 NVIDIA 가 덧붙인 벤더 확장이다.
 *
 * 네 가지를 켠다.
 *   AER : RP_VEND_CTL1 의 ERPT 비트. 이 비트를 세워야 AER 능력이 동작한다.
 *     즉 이 하드웨어에서는 AER 이 기본으로 켜져 있지 않다.
 *   대역폭 최적화 : RP_VEND_XP 의 OPPORTUNISTIC_ACK 와
 *     OPPORTUNISTIC_UPDATEFC. 위의 원문 주석이 대역폭을 높이는 최적 설정이라
 *     밝힌다. 이름으로 미루어 ACK 와 흐름 제어 갱신을 기회가 될 때 함께
 *     실어 보내는 기능으로 보이나, 정확한 동작의 근거 문서는 이 트리에 없다.
 *   DLLP 완료 대기 : RP_VEND_XP_BIST 의 GOTO_L1_L2_AFTER_DLLP_DONE.
 *     위의 원문 주석이 이유를 밝히는데, LTSSM 이 L1/L2 로 들어가기 전에
 *     DLLP 가 끝나기를 기다리게 해서 전원 관리 메시지가 잘려 수신 오류가
 *     되는 것을 막는다.
 *   클록 클램프 : RP_PRIV_MISC 의 CTLR 과 TMS 클램프 활성화 비트.
 *     update_clamp_threshold 가 켜진 SoC(Tegra124, Tegra210)는 임계값도
 *     함께 바꾼다.
 *
 * 마지막 블록만 읽고-고쳐-쓰기의 중간에 조건 분기가 들어가 있다 --
 * 활성화 비트를 세운 값에 임계값 수정을 겹친 뒤 한 번에 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 기동 경로).
 *
 * 호출 체인:
 *   tegra_pcie_port_enable → [이 함수]
 */
static void tegra_pcie_enable_rp_features(struct tegra_pcie_port *port)
{
	const struct tegra_pcie_soc *soc = port->pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	/* Enable AER capability */
	value = readl(port->base + RP_VEND_CTL1);
	/* [한국어] **AER 을 켠다.** 이 비트를 세워야 AER 능력이 동작하므로,
	 * 이 하드웨어는 AER 이 기본으로 켜져 있지 않다 */
	value |= RP_VEND_CTL1_ERPT;
	/* [한국어] AER 활성화를 쓴다 */
	writel(value, port->base + RP_VEND_CTL1);

	/* Optimal settings to enhance bandwidth */
	value = readl(port->base + RP_VEND_XP);
	/* [한국어] ACK 를 기회가 될 때 함께 실어 보낸다. 위의 원문 주석이 대역폭을
	 * 높이는 최적 설정이라 밝힐 뿐, 정확한 동작의 근거는 이 트리에 없다 */
	value |= RP_VEND_XP_OPPORTUNISTIC_ACK;
	/* [한국어] 흐름 제어 갱신도 기회가 될 때 함께 보낸다 */
	value |= RP_VEND_XP_OPPORTUNISTIC_UPDATEFC;
	/* [한국어] 두 비트를 함께 쓴다 */
	writel(value, port->base + RP_VEND_XP);

	/*
	 * LTSSM will wait for DLLP to finish before entering L1 or L2,
	 * to avoid truncation of PM messages which results in receiver errors
	 */
	value = readl(port->base + RP_VEND_XP_BIST);
	/* [한국어] **LTSSM 이 L1/L2 로 들어가기 전에 DLLP 가 끝나기를 기다리게 한다.**
	 * 위의 원문 주석대로 전원 관리 메시지가 잘려 수신 오류가 되는 것을 막는다 */
	value |= RP_VEND_XP_BIST_GOTO_L1_L2_AFTER_DLLP_DONE;
	/* [한국어] 설정을 쓴다 */
	writel(value, port->base + RP_VEND_XP_BIST);

	/* [한국어] **이 블록만 읽고-고쳐-쓰기 중간에 조건 분기가 들어간다** --
	 * 활성화 비트를 세운 값에 임계값 수정을 겹친 뒤 한 번에 쓴다 */
	value = readl(port->base + RP_PRIV_MISC);
	/* [한국어] 컨트롤러 쪽 클록 클램프를 켠다 */
	value |= RP_PRIV_MISC_CTLR_CLK_CLAMP_ENABLE;
	/* [한국어] TMS 쪽 클록 클램프도 켠다 */
	value |= RP_PRIV_MISC_TMS_CLK_CLAMP_ENABLE;

	/* [한국어] **Tegra124 와 Tegra210 만 임계값을 바꾼다.** 활성화 비트는 모든 SoC 가
	 * 세우지만 임계값은 조건부다 */
	if (soc->update_clamp_threshold) {
		/* [한국어] 두 임계값 필드를 함께 지운다 */
		value &= ~(RP_PRIV_MISC_CTLR_CLK_CLAMP_THRESHOLD_MASK |
				RP_PRIV_MISC_TMS_CLK_CLAMP_THRESHOLD_MASK);
		/* [한국어] 컨트롤러 쪽 임계값을 넣는다(0xf) */
		value |= RP_PRIV_MISC_CTLR_CLK_CLAMP_THRESHOLD |
			/* [한국어] TMS 쪽 임계값도 함께 넣는다 */
			RP_PRIV_MISC_TMS_CLK_CLAMP_THRESHOLD;
	}

	writel(value, port->base + RP_PRIV_MISC);
}

/* [한국어]
 * tegra_pcie_program_ectl_settings - 수신단 이퀄라이저 값을 여덟 레지스터에 쓴다
 *
 * @port: 대상 포트.
 * @return: 없음.
 *
 * 레지스터 여덟 개에 soc 기술자가 들고 있는 값을 그대로 밀어 넣는다.
 * 구조가 완전히 반복적이라 -- 읽고, 해당 필드를 지우고, 값을 넣고, 쓴다 --
 * 네 줄짜리 묶음이 여덟 번 이어진다.
 *
 * 레지스터 이름이 R1 넷과 R2 넷으로 짝지어 있다. 이름으로 미루어 속도
 * 등급별(Gen1/Gen2) 설정으로 보이나, 그 해석의 근거 문서는 이 트리에 없다.
 * 확실한 것은 R1 조와 R2 조가 서로 다른 값을 받는다는 사실뿐이다
 * (tegra210_pcie 를 보면 rp_ectl_2_r1 = 0x0f 인데 rp_ectl_2_r2 = 0x8f 다).
 *
 * **들어가는 값의 의미는 알 수 없다.** 필드 이름(RX_CTLE, RX_CDR_CTRL,
 * RX_EQ_CTRL_L, RX_EQ_CTRL_H)이 수신단 이퀄라이저와 클록 복원 관련임을
 * 가리킬 뿐이다. 이 파일에서 이 값을 쓰는 SoC 는 Tegra210 하나뿐이며,
 * 나머지는 ectl.enable 이 false 라 이 함수가 아예 불리지 않는다.
 *
 * 두 레지스터(4_R1, 4_R2)만 시프트가 필요한 데 주의한다. 그 필드가
 * 비트 31:16 에 있어 soc 에 저장된 값을 16비트 왼쪽으로 밀어야 한다.
 * 나머지 여섯은 필드가 최하위부터 시작해 그대로 넣는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 기동 경로).
 *
 * 호출 체인:
 *   tegra_pcie_port_enable → [이 함수] (soc->ectl.enable 일 때만)
 */
static void tegra_pcie_program_ectl_settings(struct tegra_pcie_port *port)
{
	const struct tegra_pcie_soc *soc = port->pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	/* [한국어] **여덟 레지스터에 같은 네 줄 묶음이 반복된다.** 읽고, 필드를 지우고,
	 * 값을 넣고, 쓴다 */
	value = readl(port->base + RP_ECTL_2_R1);
	/* [한국어] 해당 필드만 지운다 */
	value &= ~RP_ECTL_2_R1_RX_CTLE_1C_MASK;
	/* [한국어] SoC 가 정한 값을 넣는다 */
	value |= soc->ectl.regs.rp_ectl_2_r1;
	/* [한국어] 값을 쓴다 */
	writel(value, port->base + RP_ECTL_2_R1);

	/* [한국어] R1 조의 두 번째 레지스터를 읽는다 */
	value = readl(port->base + RP_ECTL_4_R1);
	/* [한국어] 필드를 지운다 */
	value &= ~RP_ECTL_4_R1_RX_CDR_CTRL_1C_MASK;
	/* [한국어] **이 필드만 시프트가 필요하다** -- 비트 31:16 에 있어 저장된 값을
	 * 16비트 왼쪽으로 밀어야 한다. 나머지 여섯은 필드가 최하위부터 시작해
	 * 그대로 넣는다 */
	value |= soc->ectl.regs.rp_ectl_4_r1 <<
				RP_ECTL_4_R1_RX_CDR_CTRL_1C_SHIFT;
	/* [한국어] 시프트한 값을 쓴다 */
	writel(value, port->base + RP_ECTL_4_R1);

	/* [한국어] R1 조의 세 번째 레지스터를 읽는다 */
	value = readl(port->base + RP_ECTL_5_R1);
	/* [한국어] 필드 마스크가 0xffffffff 라 레지스터 전체를 덮어쓴다 */
	value &= ~RP_ECTL_5_R1_RX_EQ_CTRL_L_1C_MASK;
	/* [한국어] SoC 가 정한 값을 넣는다 */
	value |= soc->ectl.regs.rp_ectl_5_r1;
	/* [한국어] 값을 쓴다 */
	writel(value, port->base + RP_ECTL_5_R1);

	/* [한국어] R1 조의 네 번째 레지스터를 읽는다 */
	value = readl(port->base + RP_ECTL_6_R1);
	/* [한국어] 필드를 지운다 */
	value &= ~RP_ECTL_6_R1_RX_EQ_CTRL_H_1C_MASK;
	/* [한국어] SoC 가 정한 값을 넣는다 */
	value |= soc->ectl.regs.rp_ectl_6_r1;
	/* [한국어] R1 조의 마지막 값을 쓴다 */
	writel(value, port->base + RP_ECTL_6_R1);

	/* [한국어] **여기서부터 R2 조 넷이다.** R1/R2 가 무엇을 뜻하는지는 이 트리에서
	 * 확인할 수 없으나, 속도 등급별 설정으로 보인다 */
	value = readl(port->base + RP_ECTL_2_R2);
	/* [한국어] 필드를 지운다 */
	value &= ~RP_ECTL_2_R2_RX_CTLE_1C_MASK;
	/* [한국어] SoC 가 정한 값을 넣는다. **R1 조의 대응 값과 다르다**(0x0f 대 0x8f) */
	value |= soc->ectl.regs.rp_ectl_2_r2;
	/* [한국어] R2 조의 첫 값을 쓴다 */
	writel(value, port->base + RP_ECTL_2_R2);

	/* [한국어] R2 조의 두 번째 레지스터를 읽는다 */
	value = readl(port->base + RP_ECTL_4_R2);
	/* [한국어] 필드를 지운다 */
	value &= ~RP_ECTL_4_R2_RX_CDR_CTRL_1C_MASK;
	/* [한국어] **R2 조에서도 이 필드만 시프트가 필요하다** -- 비트 31:16 에 있기 때문이다 */
	value |= soc->ectl.regs.rp_ectl_4_r2 <<
				RP_ECTL_4_R2_RX_CDR_CTRL_1C_SHIFT;
	/* [한국어] 시프트한 값을 쓴다 */
	writel(value, port->base + RP_ECTL_4_R2);

	/* [한국어] R2 조의 세 번째 레지스터를 읽는다 */
	value = readl(port->base + RP_ECTL_5_R2);
	/* [한국어] 필드 마스크가 0xffffffff 라 레지스터 전체를 덮어쓴다 */
	value &= ~RP_ECTL_5_R2_RX_EQ_CTRL_L_1C_MASK;
	/* [한국어] 필드를 지운다 */
	value |= soc->ectl.regs.rp_ectl_5_r2;
	/* [한국어] R2 조의 세 번째 값 */
	writel(value, port->base + RP_ECTL_5_R2);

	/* [한국어] R2 조의 네 번째 레지스터를 읽는다 */
	value = readl(port->base + RP_ECTL_6_R2);
	/* [한국어] 필드를 지운다 */
	value &= ~RP_ECTL_6_R2_RX_EQ_CTRL_H_1C_MASK;
	/* [한국어] R2 조의 마지막 값 */
	value |= soc->ectl.regs.rp_ectl_6_r2;
	writel(value, port->base + RP_ECTL_6_R2);
}

/* [한국어]
 * tegra_pcie_apply_sw_fixup - 하드웨어 결함을 소프트웨어로 우회한다
 *
 * @port: 대상 포트.
 * @return: 없음.
 *
 * 이름 그대로 하드웨어 문제를 피해 가는 설정 셋을 모아 놓았다. 앞의 둘은
 * SoC 별로 켜지고, 마지막 하나는 **모든 SoC 에 무조건** 적용된다.
 *
 * 디스큐 재시도 시간(program_deskew_time, Tegra210 만):
 *   위의 원문 주석이 이유를 밝힌다 -- 레인 0 의 디스큐 로직이 불안정해
 *   Gen2 에서 Gen1 으로 속도를 낮출 때 실패하는 일이 있어, 재시도 시간을
 *   늘린다. 값은 RP_VEND_CTL0_DSK_RST_PULSE_WIDTH(0x9 << 12)로 고정이다.
 *
 * 흐름 제어 타이머(update_fc_timer, Tegra210 만):
 *   RP_VEND_XP 의 임계값 필드를 soc->update_fc_threshold 로 바꾼다.
 *   tegra210_pcie 의 정의 위에 원문 주석이 "FC threshold is bit[25:18]"
 *   이라 적어 두었고 값은 0x01800000 이다.
 *
 * **Gen1 만 광고하기 (무조건)**:
 *   이 함수에서 가장 중요한 대목이다. 위의 원문 주석이 배경을 밝히는데,
 *   루트 포트가 Gen1 과 Gen2 를 함께 광고하면 일부 구형 엔드포인트가
 *   링크를 세우지 못한다. 그래서 처음에는 Link Control Status 2 의 속도
 *   필드를 2.5GT/s 로 낮춰 두고, 링크가 선 **뒤에**
 *   tegra_pcie_change_link_speed 가 5.0GT/s 로 재훈련한다.
 *   이 두 함수가 짝을 이루며, 이 순서 때문에 Gen2 장치도 부팅 초기에는
 *   Gen1 으로 링크를 맺는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 기동 경로).
 *
 * 호출 체인:
 *   tegra_pcie_port_enable → [이 함수]
 *   (그리고 링크가 선 뒤 tegra_pcie_enable_ports → tegra_pcie_change_link_speed)
 */
static void tegra_pcie_apply_sw_fixup(struct tegra_pcie_port *port)
{
	const struct tegra_pcie_soc *soc = port->pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	/*
	 * Sometimes link speed change from Gen2 to Gen1 fails due to
	 * instability in deskew logic on lane-0. Increase the deskew
	 * retry time to resolve this issue.
	 */
	if (soc->program_deskew_time) {
		/* [한국어] 현재 값을 읽는다 */
		value = readl(port->base + RP_VEND_CTL0);
		/* [한국어] 기존 폭 필드를 지운다 */
		value &= ~RP_VEND_CTL0_DSK_RST_PULSE_WIDTH_MASK;
		/* [한국어] 정해진 폭(0x9)을 넣는다 */
		value |= RP_VEND_CTL0_DSK_RST_PULSE_WIDTH;
		/* [한국어] 디스큐 설정을 쓴다 */
		writel(value, port->base + RP_VEND_CTL0);
	}

	/* [한국어] **Tegra210 만 이 조정을 한다** */
	if (soc->update_fc_timer) {
		/* [한국어] 현재 값을 읽는다 */
		value = readl(port->base + RP_VEND_XP);
		/* [한국어] 기존 임계값 필드를 지운다 */
		value &= ~RP_VEND_XP_UPDATE_FC_THRESHOLD_MASK;
		/* [한국어] SoC 가 정한 임계값을 넣는다 */
		value |= soc->update_fc_threshold;
		/* [한국어] 흐름 제어 설정을 쓴다 */
		writel(value, port->base + RP_VEND_XP);
	}

	/*
	 * PCIe link doesn't come up with few legacy PCIe endpoints if
	 * root port advertises both Gen-1 and Gen-2 speeds in Tegra.
	 * Hence, the strategy followed here is to initially advertise
	 * only Gen-1 and after link is up, retrain link to Gen-2 speed
	 */
	value = readl(port->base + RP_LINK_CONTROL_STATUS_2);
	/* [한국어] 기존 속도 필드를 지운다 */
	value &= ~PCI_EXP_LNKSTA_CLS;
	/* [한국어] **2.5GT/s(Gen1)만 광고한다.** 위의 원문 주석대로 일부 구형 엔드포인트가
	 * Gen1 과 Gen2 를 함께 광고하면 링크를 세우지 못하기 때문이다.
	 * 링크가 선 뒤 tegra_pcie_change_link_speed 가 Gen2 로 재훈련한다 */
	value |= PCI_EXP_LNKSTA_CLS_2_5GB;
	writel(value, port->base + RP_LINK_CONTROL_STATUS_2);
}

/* [한국어]
 * tegra_pcie_port_enable - 포트 하나를 깨운다
 *
 * @port: 대상 포트.
 * @return: 없음. 이 단계에서 실패할 수 있는 동작이 없다.
 *
 * 포트를 링크 훈련 직전 상태까지 올린다. 순서가 정해져 있다.
 *
 *   1) 참조 클록을 켠다. AFI 제어 레지스터의 REFCLK_EN 을 세우고,
 *      SoC 가 지원하면 CLKREQ_EN 도 함께 세운다. OVERRIDE_EN 은 항상
 *      세우는데, 이름으로 미루어 하드웨어 자동 제어 대신 소프트웨어가
 *      정한 값을 쓰게 하는 비트다.
 *   2) 리셋 펄스를 준다. 여기서 슬롯 장치가 리셋에서 풀린다.
 *   3) PCA 를 켠다(force_pca_enable 인 SoC 만, 즉 Tegra210).
 *      RP_VEND_CTL2 의 비트 7 이며, 이 약어의 근거는 이 트리에 없다.
 *   4) 루트 포트 고유 기능을 켠다.
 *   5) 이퀄라이저 값을 쓴다(ectl.enable 인 SoC 만).
 *   6) 소프트웨어 우회를 적용한다 -- 여기서 속도가 Gen1 으로 낮춰진다.
 *
 * 주의: 이 함수는 링크를 **세우지 않는다.** 실제 링크 훈련은
 * tegra_pcie_enable_ports 가 모든 포트에 이 함수를 부른 뒤
 * pcie_xrst 리셋을 풀 때 시작된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 2)에서 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_enable_ports → [이 함수]
 *     → tegra_pcie_port_get_pex_ctrl, tegra_pcie_port_reset,
 *       tegra_pcie_enable_rp_features, tegra_pcie_program_ectl_settings,
 *       tegra_pcie_apply_sw_fixup
 */
static void tegra_pcie_port_enable(struct tegra_pcie_port *port)
{
	unsigned long ctrl = tegra_pcie_port_get_pex_ctrl(port);
	/* [한국어] 여러 기능 불리언을 보기 위해 SoC 기술자가 필요하다 */
	const struct tegra_pcie_soc *soc = port->pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	unsigned long value;

	/* enable reference clock */
	value = afi_readl(port->pcie, ctrl);
	/* [한국어] 참조 클록을 켠다 */
	value |= AFI_PEX_CTRL_REFCLK_EN;

	/* [한국어] CLKREQ 비트를 다룰 수 있는지 본다 */
	if (soc->has_pex_clkreq_en)
		/* [한국어] 지원하는 SoC 만 CLKREQ 를 켠다 */
		value |= AFI_PEX_CTRL_CLKREQ_EN;

	/* [한국어] **항상 세운다.** 하드웨어 자동 제어 대신 소프트웨어 설정을 쓰게 하는
	 * 비트로 보이며, 끌 때는 지우지 않는다 */
	value |= AFI_PEX_CTRL_OVERRIDE_EN;

	/* [한국어] 참조 클록 설정을 쓴다 */
	afi_writel(port->pcie, value, ctrl);

	tegra_pcie_port_reset(port);

	/* [한국어] **Tegra210 만 이 설정을 한다** */
	if (soc->force_pca_enable) {
		/* [한국어] 현재 값을 읽는다 */
		value = readl(port->base + RP_VEND_CTL2);
		/* [한국어] PCA 를 켠다. 이 약어가 무엇의 줄임인지는 이 트리에서 확인할 수 없다 */
		value |= RP_VEND_CTL2_PCA_ENABLE;
		/* [한국어] PCA 비트를 쓴다 */
		writel(value, port->base + RP_VEND_CTL2);
	}

	tegra_pcie_enable_rp_features(port);

	/* [한국어] Tegra210 만 이퀄라이저 값을 쓴다 */
	if (soc->ectl.enable)
		tegra_pcie_program_ectl_settings(port);

	tegra_pcie_apply_sw_fixup(port);
}

/* [한국어]
 * tegra_pcie_port_disable - 포트 하나를 끈다
 *
 * @port: 대상 포트.
 * @return: 없음.
 *
 * tegra_pcie_port_enable 의 역순이되 더 짧다. 켤 때 했던 벤더 확장 설정은
 * 되돌리지 않는데, 전원이 끊기거나 리셋되면 어차피 초기화되기 때문이다.
 *
 *   1) 포트 리셋을 어서트한다(비트를 **지워서**).
 *   2) 참조 클록을 끈다. CLKREQ_EN 은 SoC 가 지원할 때만 지운다.
 *   3) 포트를 비활성화하고 CLKREQ 핀을 GPIO 로 돌린다. 위의 원문 주석이
 *      이유를 밝히는데, 그래야 PLLE 가 전원을 내릴 수 있다. 즉 CLKREQ 가
 *      PCIe 기능으로 남아 있으면 PLL 을 재울 수 없다는 뜻이다.
 *
 * 1)과 2)가 같은 레지스터를 두 번 나눠 읽는 데 주의한다. 한 번에 처리해도
 * 될 것을 나눈 것은 리셋을 먼저 확실히 적용하려는 의도로 보인다.
 *
 * 불리는 곳이 둘이다 -- 링크가 안 선 포트를 정리할 때
 * (tegra_pcie_enable_ports), 그리고 전체를 내릴 때
 * (tegra_pcie_disable_ports).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_enable_ports / tegra_pcie_disable_ports → [이 함수]
 */
static void tegra_pcie_port_disable(struct tegra_pcie_port *port)
{
	unsigned long ctrl = tegra_pcie_port_get_pex_ctrl(port);
	/* [한국어] has_pex_clkreq_en 을 보기 위해 SoC 기술자가 필요하다 */
	const struct tegra_pcie_soc *soc = port->pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	unsigned long value;

	/* assert port reset */
	value = afi_readl(port->pcie, ctrl);
	/* [한국어] **비트를 지우는 것이 리셋 어서트다.** GPIO 방식과 극성이 반대다 */
	value &= ~AFI_PEX_CTRL_RST;
	/* [한국어] 리셋을 어서트한다 */
	afi_writel(port->pcie, value, ctrl);

	/* disable reference clock */
	value = afi_readl(port->pcie, ctrl);

	/* [한국어] CLKREQ 비트를 다룰 수 있는지 본다 */
	if (soc->has_pex_clkreq_en)
		/* [한국어] 지원하는 SoC 만 CLKREQ 를 지운다 */
		value &= ~AFI_PEX_CTRL_CLKREQ_EN;

	/* [한국어] 참조 클록 활성화 비트를 지운다 */
	value &= ~AFI_PEX_CTRL_REFCLK_EN;
	/* [한국어] 참조 클록을 끈다 */
	afi_writel(port->pcie, value, ctrl);

	/* disable PCIe port and set CLKREQ# as GPIO to allow PLLE power down */
	value = afi_readl(port->pcie, AFI_PCIE_CONFIG);
	/* [한국어] 이 포트를 비활성화한다 */
	value |= AFI_PCIE_CONFIG_PCIE_DISABLE(port->index);
	/* [한국어] CLKREQ 핀을 GPIO 로 돌린다. 위의 원문 주석대로 그래야 PLLE 가
	 * 전원을 내릴 수 있다 */
	value |= AFI_PCIE_CONFIG_PCIE_CLKREQ_GPIO(port->index);
	afi_writel(port->pcie, value, AFI_PCIE_CONFIG);
}

/* [한국어]
 * tegra_pcie_port_free - 링크가 서지 않은 포트의 자원을 되돌린다
 *
 * @port: 없앨 포트.
 * @return: 없음.
 *
 * 링크가 안 선 포트는 쓸 일이 없으므로 매핑과 메모리를 놓고 리스트에서
 * 뺀다. 그 뒤로는 tegra_pcie_enable_ports 의 순회나
 * tegra_pcie_ports_seq_show 의 debugfs 출력에 나타나지 않는다.
 *
 * **devm 자원을 명시적으로 놓는 것** 이 이 함수의 특징이다. devm 은 보통
 * 드라이버가 떨어질 때 커널이 알아서 정리하지만, 여기서는 드라이버가
 * 계속 살아 있는 채로 포트 하나만 버리므로 직접 놓아야 한다. 그래서
 * devm_iounmap, devm_release_mem_region, devm_kfree 를 손으로 부른다.
 *
 * 호출자가 list_for_each_entry_safe 를 쓰는 이유가 여기 있다 -- 이 함수가
 * 현재 원소를 리스트에서 빼고 해제하므로, 다음 포인터를 미리 잡아 두지
 * 않으면 순회가 깨진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(포트 기동 경로).
 *
 * 호출 체인:
 *   tegra_pcie_enable_ports → [이 함수]
 */
static void tegra_pcie_port_free(struct tegra_pcie_port *port)
{
	struct tegra_pcie *pcie = port->pcie;
	/* [한국어] devm 자원의 주인 */
	struct device *dev = pcie->dev;

	/* [한국어] **devm 자원을 명시적으로 놓는다.** devm 은 보통 드라이버가 떨어질 때
	 * 커널이 정리하지만, 여기서는 드라이버가 계속 살아 있는 채로 포트
	 * 하나만 버리므로 직접 놓아야 한다 */
	devm_iounmap(dev, port->base);
	/* [한국어] 메모리 영역 예약을 놓는다 */
	devm_release_mem_region(dev, port->regs.start,
				resource_size(&port->regs));
	list_del(&port->list);
	/* [한국어] 포트 구조체를 놓는다 */
	devm_kfree(dev, port);
}

/* Tegra PCIE root complex wrongly reports device class */
/* [한국어]
 * tegra_pcie_fixup_class - 루트 컴플렉스가 잘못 보고한 클래스 코드를 고친다
 *
 * @dev: 방금 열거된 PCI 장치.
 * @return: 없음.
 *
 * 위의 원문 주석이 사실을 밝힌다 -- 이 루트 컴플렉스는 자기 클래스 코드를
 * 틀리게 보고한다. 브리지가 아닌 것으로 보고하면 PCI 코어가 그 아래로
 * 버스를 확장하지 않으므로, 열거 초기에 값을 바로잡는다.
 *
 * **이 함수는 드라이버가 부르지 않는다.** 아래의 DECLARE_PCI_FIXUP_EARLY 가
 * 벤더/장치 ID 별 표에 등록해 두면, PCI 코어가 장치를 발견한 직후 자동으로
 * 불러 준다. 등록이 네 줄인 것은 이 IP 가 SoC 세대에 따라 서로 다른 장치
 * ID(0x0bf0, 0x0bf1, 0x0e1c, 0x0e1d)로 나타나기 때문이다.
 *
 * EARLY 단계인 것이 중요하다. 클래스 코드는 버스 열거 자체에 영향을 주므로
 * 자원 할당보다 앞서 고쳐져야 한다.
 *
 * PCI_CLASS_BRIDGE_PCI_NORMAL 을 정의한 헤더는 이 스파스 체크아웃에 없어
 * 값을 확인하지 못했다. 이름과 쓰임으로 보아 PCI-to-PCI 브리지의
 * 클래스/서브클래스/프로그래밍 인터페이스를 합친 값이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거 경로).
 *
 * 호출 체인:
 *   pci_scan_device 계열 → PCI 코어의 fixup 실행 → [이 함수]
 */
static void tegra_pcie_fixup_class(struct pci_dev *dev)
{
	dev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;
}
/* [한국어] 클래스 코드 fixup 등록. **EARLY 단계** 인 것이 중요하다 -- 클래스 코드는
 * 버스 열거 자체에 영향을 주므로 자원 할당보다 앞서 고쳐져야 한다.
 * 네 줄인 것은 이 IP 가 SoC 세대에 따라 서로 다른 장치 ID 로 나타나기
 * 때문이다 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0bf0, tegra_pcie_fixup_class);
/* [한국어] 두 번째 장치 ID */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0bf1, tegra_pcie_fixup_class);
/* [한국어] 세 번째 장치 ID */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0e1c, tegra_pcie_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0e1d, tegra_pcie_fixup_class);

/* Tegra20 and Tegra30 PCIE requires relaxed ordering */
/* [한국어]
 * tegra_pcie_relax_enable - Tegra20/30 에서 relaxed ordering 을 켠다
 *
 * @dev: 열거를 마친 PCI 장치.
 * @return: 없음.
 *
 * 위의 원문 주석대로 Tegra20 과 Tegra30 의 PCIe 가 relaxed ordering 을
 * 요구한다. 장치 제어 레지스터의 해당 비트를 세운다.
 *
 * relaxed ordering 은 PCIe 트랜잭션의 순서 보장을 완화해 성능을 높이는
 * 기능이다. 보통은 선택 사항이지만 여기서는 하드웨어가 요구하는 것이므로,
 * 성능 최적화가 아니라 **동작을 위한 필수 설정** 으로 읽어야 한다.
 *
 * fixup_class 와 마찬가지로 드라이버가 직접 부르지 않는다. 아래의
 * DECLARE_PCI_FIXUP_FINAL 이 네 장치 ID 에 등록한다. FINAL 단계인 것은
 * 클래스 코드와 달리 이 설정이 열거 자체에는 영향을 주지 않기 때문이다.
 *
 * 주의: 등록된 네 장치 ID 는 fixup_class 와 같은 목록이라 Tegra124 등
 * 뒷세대에도 걸린다. 원문 주석은 Tegra20/30 만 언급하지만 실제 적용
 * 범위는 그 네 ID 전부다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거 마무리 경로).
 *
 * 호출 체인:
 *   PCI 코어의 fixup 실행 → [이 함수] → pcie_capability_set_word
 */
static void tegra_pcie_relax_enable(struct pci_dev *dev)
{
	pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);
}
/* [한국어] relaxed ordering 을 켜는 fixup 등록. **FINAL 단계** 인 것은 이 설정이
 * 열거 자체에는 영향을 주지 않기 때문이다.
 * 네 장치 ID 에 모두 걸리므로, 원문 주석이 Tegra20/30 만 언급하는 것과
 * 달리 실제 적용 범위는 그 넷 전부다 */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA, 0x0bf0, tegra_pcie_relax_enable);
/* [한국어] Tegra20 세대의 두 번째 장치 ID */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA, 0x0bf1, tegra_pcie_relax_enable);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA, 0x0e1c, tegra_pcie_relax_enable);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA, 0x0e1d, tegra_pcie_relax_enable);

/* [한국어]
 * tegra_pcie_map_irq - 장치의 INTx 핀을 가상 IRQ 번호로 바꾼다
 *
 * @pdev: 대상 장치.
 * @slot: 슬롯 번호. 이 함수는 쓰지 않고 아래 함수에 그대로 넘긴다.
 * @pin:  INTx 핀 번호(1~4 가 INTA~INTD). 역시 그대로 넘긴다.
 * @return: 가상 IRQ 번호.
 *
 * PCI 코어가 장치의 INTx 를 어느 인터럽트 선에 연결할지 물을 때 불린다.
 *
 * 두 가지를 한다. 첫째, cpuidle 에 PCIe 인터럽트가 쓰이기 시작했음을
 * 알린다. 깊은 절전 상태에서는 인터럽트를 놓칠 수 있으므로 그런 상태로
 * 들어가지 않게 막는 것으로 보이나, tegra_cpuidle_pcie_irqs_in_use 의
 * 정의가 이 트리에 없어(include/soc/tegra 부재) 정확한 동작은 확인하지
 * 못했다. 같은 호출이 tegra_msi_domain_alloc 에도 있어, MSI 를 할당할 때도
 * 같은 조치를 한다.
 *
 * 둘째, 장치 트리의 interrupt-map 을 따라 IRQ 를 찾는다. 실패하면
 * 컨트롤러 자신의 인터럽트(pcie->irq)로 대체한다 -- 즉 INTx 가 별도로
 * 배선되지 않은 보드에서는 AFI 오류 인터럽트와 같은 선을 공유한다.
 * tegra_pcie_isr 이 AFI_INTR_LEGACY 코드를 보면 IRQ_NONE 을 돌려주는
 * 이유가 여기 있다. 그 선을 공유하는 다른 핸들러에게 차례를 넘기기
 * 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거 경로).
 *
 * 호출 체인:
 *   pci_assign_irq 계열 → host->map_irq → [이 함수]
 *     → tegra_cpuidle_pcie_irqs_in_use, of_irq_parse_and_map_pci
 */
static int tegra_pcie_map_irq(const struct pci_dev *pdev, u8 slot, u8 pin)
{
	struct tegra_pcie *pcie = pdev->bus->sysdata;
	/* [한국어] 찾은 IRQ 번호 */
	int irq;

	tegra_cpuidle_pcie_irqs_in_use();

	/* [한국어] 장치 트리의 interrupt-map 을 따라 IRQ 를 찾는다 */
	irq = of_irq_parse_and_map_pci(pdev, slot, pin);
	/* [한국어] 매핑을 못 찾았는지 확인한다 */
	if (!irq)
		/* [한국어] **장치 트리에 INTx 가 없으면 컨트롤러 인터럽트를 대신 쓴다.**
		 * 그래서 AFI 오류 인터럽트와 INTx 가 같은 선을 공유할 수 있고,
		 * tegra_pcie_isr 이 IRQ_NONE 을 돌려주는 경로가 필요해진다 */
		irq = pcie->irq;

	return irq;
}

/* [한국어]
 * tegra_pcie_isr - 컨트롤러 오류 인터럽트를 해석해 로그로 남긴다
 *
 * @irq: 인터럽트 번호. 쓰지 않는다.
 * @arg: request_irq 에 넘긴 struct tegra_pcie.
 * @return: IRQ_HANDLED. 단 INTx 코드였으면 IRQ_NONE.
 *
 * AFI 가 감지한 오류를 사람이 읽을 수 있는 문장으로 바꾼다. **오류를
 * 고치지는 않고 보고만 한다** -- 복구 동작이 없다.
 *
 * 동작:
 *   1) 오류 코드와 서명을 읽고, 코드 레지스터를 0 으로 지워 다음 오류를
 *      받을 준비를 한다. 서명은 지우지 않는데, 다음 오류가 덮어쓸 것이기
 *      때문이다.
 *   2) 코드가 AFI_INTR_LEGACY 면 **IRQ_NONE 을 돌려준다.** 이 선은
 *      IRQF_SHARED 로 요청되어 있고 INTx 가 같은 선을 탈 수 있으므로
 *      (tegra_pcie_map_irq 참조), 자기 것이 아니라고 알려 다음 핸들러에게
 *      차례를 넘긴다.
 *   3) 코드를 문자열로 바꾼다. err_msg[] 는 원소 15개(인덱스 0~14)이고
 *      AFI_INTR 코드도 1~14 라 정확히 맞는다. 인덱스 0 은 "Unknown" 이며,
 *      범위를 넘는 코드는 0 으로 눌러 그 문자열을 쓴다.
 *   4) 로그 수준을 가른다. 마스터 어보트와 슬롯 존재 감지 변화는
 *      dev_dbg 로 낮춘다. 위의 원문 주석이 이유를 밝히는데, 열거 중에
 *      빈 슬롯을 찔러 보면 마스터 어보트가 대량으로 발생하기 때문이다.
 *      그것을 dev_err 로 찍으면 정상 부팅에도 로그가 오염된다.
 *   5) 주소 관련 오류 셋(타깃 어보트, 마스터 어보트, FPCI 디코드 오류)에는
 *      문제가 된 FPCI 주소를 덧붙인다. 상위 8비트는 별도 레지스터에서
 *      읽어 32비트 왼쪽으로 올리고, 하위는 서명에서 가져오되 & 0xfffffffc
 *      로 하위 2비트를 버린다 -- dword 정렬된 주소이므로 그 두 비트는
 *      주소가 아니라 다른 정보이거나 무의미하다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 스레드 핸들러가 아니므로 잠들 수 없다.
 * 로그 출력만 하므로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   AFI 오류 발생 → 커널 인터럽트 처리 → [이 함수]
 */
static irqreturn_t tegra_pcie_isr(int irq, void *arg)
{
	static const char * const err_msg[] = {
		/* [한국어] **인덱스 0 은 알 수 없는 코드용이다.** AFI_INTR 코드가 1부터 시작하므로,
		 * 이 배열은 원소 15개로 코드 1~14 를 그대로 색인할 수 있다 */
		"Unknown",
		"AXI slave error",
		"AXI decode error",
		"Target abort",
		"Master abort",
		"Invalid write",
		"Legacy interrupt",
		"Response decoding error",
		"AXI response decoding error",
		"Transaction timeout",
		"Slot present pin change",
		"Slot clock request change",
		"TMS clock ramp change",
		"TMS ready for power down",
		"Peer2Peer error",
	};
	/* [한국어] request_irq 에 넘긴 컨트롤러 상태 */
	struct tegra_pcie *pcie = arg;
	/* [한국어] 로그용 device */
	struct device *dev = pcie->dev;
	/* [한국어] 오류 코드와 서명 */
	u32 code, signature;

	/* [한국어] 오류 코드를 읽는다. 하위 4비트만 유효하다 */
	code = afi_readl(pcie, AFI_INTR_CODE) & AFI_INTR_CODE_MASK;
	/* [한국어] 오류에 딸린 부가 정보를 읽는다 */
	signature = afi_readl(pcie, AFI_INTR_SIGNATURE);
	/* [한국어] 코드 레지스터를 지워 다음 오류를 받을 준비를 한다.
	 * 서명은 지우지 않는데, 다음 오류가 덮어쓸 것이기 때문이다 */
	afi_writel(pcie, 0, AFI_INTR_CODE);

	/* [한국어] **INTx 코드면 IRQ_NONE 을 돌려준다.** 이 선은 IRQF_SHARED 로 요청되어
	 * INTx 가 같은 선을 탈 수 있으므로, 자기 것이 아니라고 알려 다음
	 * 핸들러에게 차례를 넘긴다 */
	if (code == AFI_INTR_LEGACY)
		return IRQ_NONE;

	/* [한국어] 배열 범위를 벗어나는지 확인한다 */
	if (code >= ARRAY_SIZE(err_msg))
		/* [한국어] 범위를 넘는 코드는 0("Unknown")으로 눌러 배열 밖 접근을 막는다 */
		code = 0;

	/*
	 * do not pollute kernel log with master abort reports since they
	 * happen a lot during enumeration
	 */
	if (code == AFI_INTR_MASTER_ABORT || code == AFI_INTR_PE_PRSNT_SENSE)
		/* [한국어] **흔한 두 오류는 dev_dbg 로 낮춘다.** 위의 원문 주석대로 열거 중에
		 * 빈 슬롯을 찔러 보면 마스터 어보트가 대량으로 발생하므로, dev_err 로
		 * 찍으면 정상 부팅에도 로그가 오염된다 */
		dev_dbg(dev, "%s, signature: %08x\n", err_msg[code], signature);
	else
		/* [한국어] 그 밖의 오류는 dev_err 로 찍는다 */
		dev_err(dev, "%s, signature: %08x\n", err_msg[code], signature);

	/* [한국어] 주소 관련 오류 셋에만 FPCI 주소를 덧붙인다 */
	if (code == AFI_INTR_TARGET_ABORT || code == AFI_INTR_MASTER_ABORT ||
	    code == AFI_INTR_FPCI_DECODE_ERROR) {
		/* [한국어] 주소 상위 부분을 읽는다. 하위 8비트만 유효하다 */
		u32 fpci = afi_readl(pcie, AFI_UPPER_FPCI_ADDRESS) & 0xff;
		/* [한국어] **64비트 주소를 조립한다.** 상위를 32비트 왼쪽으로 올리고 서명의
		 * 하위를 합친다. & 0xfffffffc 로 하위 2비트를 버리는 것은 dword 정렬된
		 * 주소이기 때문이다 */
		u64 address = (u64)fpci << 32 | (signature & 0xfffffffc);

		/* [한국어] 로그 수준을 다시 가른다 */
		if (code == AFI_INTR_MASTER_ABORT)
			/* [한국어] 마스터 어보트만 dev_dbg 로 낮춘다 */
			dev_dbg(dev, "  FPCI address: %10llx\n", address);
		else
			/* [한국어] 그 밖의 주소 오류는 dev_err 로 찍는다 */
			dev_err(dev, "  FPCI address: %10llx\n", address);
	}

	return IRQ_HANDLED;
}

/*
 * FPCI map is as follows:
 * - 0xfdfc000000: I/O space
 * - 0xfdfe000000: type 0 configuration space
 * - 0xfdff000000: type 1 configuration space
 * - 0xfe00000000: type 0 extended configuration space
 * - 0xfe10000000: type 1 extended configuration space
 */
/* [한국어]
 * tegra_pcie_setup_translations - CPU 주소와 PCIe 주소 사이의 변환 창을 프로그래밍한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음. 창을 못 넣는 경우가 없어 실패하지 않는다.
 *
 * **아웃바운드 주소 변환의 전부가 이 함수에 있다.** 위의 원문 주석이
 * FPCI 주소 지도를 적어 두었는데, IO 공간, type 0/1 config 공간, type 0/1
 * 확장 config 공간이 각각 고정된 FPCI 주소에 놓여 있다.
 *
 * BAR 짝의 구조: 창 하나마다 세 레지스터가 한 조다.
 *   AXI_BAR_START : CPU 쪽 시작 주소
 *   AXI_BAR_SZ    : 크기. **4KiB 단위** 라 바이트 크기를 12비트 오른쪽으로
 *                   민다(size >> 12).
 *   FPCI_BAR      : PCIe 쪽 대응 주소
 *
 * BAR 배정이 고정되어 있다.
 *   BAR0 : type 1 확장 config 공간. tegra_pcie_map_bus 가 옮겨 쓰는 그
 *     창이며, 여기서는 시작 주소와 크기만 잡아 둔다. FPCI 쪽은 여기서
 *     쓰지 않는데, map_bus 가 접근할 때마다 새로 쓰기 때문이다.
 *   BAR1 : 하류 IO 창. FPCI 주소가 0xfdfc0000 으로 고정이다.
 *     pci_pio_to_address 로 논리 포트 번호를 실제 주소로 바꾼다.
 *   BAR2 : prefetchable 메모리 창.
 *   BAR3 : non-prefetchable 메모리 창.
 *   BAR4, BAR5 : 쓰지 않으므로 0 으로 지운다.
 *
 * 메모리 창의 FPCI 주소 계산이 특이하다.
 *   (((res->start >> 12) & 0x0fffffff) << 4) | 0x1
 * 4KiB 페이지 번호로 바꾸고(>> 12), 28비트로 자르고, 다시 4비트 왼쪽으로
 * 민 뒤 최하위 비트를 세운다. 즉 이 레지스터는 주소를 그대로 담는 것이
 * 아니라 비트 31:4 에 페이지 번호를 두고 비트 0 을 활성화 표시로 쓰는
 * 형식이다. 그 형식의 근거 문서는 이 트리에 없으나, IO 창이 고정 상수를
 * 쓰는 것과 달리 메모리 창만 이 계산을 하는 데서 형식이 다름을 알 수 있다.
 *
 * 캐시 BAR 는 has_cache_bars 인 SoC(Tegra20 뿐)에서만 0 으로 지운다.
 * 위의 원문 주석대로 상류 트랜잭션을 모두 uncached 로 처리하겠다는 뜻이다.
 *
 * MSI 관련 레지스터도 여기서 0 으로 지워 둔다. 원문 주석대로 실제 설정은
 * 필요할 때 tegra_pcie_enable_msi 가 한다. 마지막 두 줄이 AFI_MSI_BAR_SZ 에
 * 두 번 0 을 쓰는데, 앞의 AXI 쪽과 짝을 맞추려는 반복으로 보인다.
 *
 * **뒷세대와의 대비**: pcie-tegra194.c 에는 이런 함수가 없다. DWC 코어의
 * iATU 가 창을 관리하고, 그 드라이버는 APPL 레지스터에 iATU 블록의 기준
 * 주소만 알려 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe 와 resume 이 공유한다.
 *
 * 호출 체인:
 *   tegra_pcie_pm_resume → [이 함수] → afi_writel, pci_pio_to_address
 */
static void tegra_pcie_setup_translations(struct tegra_pcie *pcie)
{
	u32 size;
	/* [한국어] 창 목록 순회용 반복자 */
	struct resource_entry *entry;
	/* [한국어] private 영역에서 브리지를 되찾는다. 창 목록이 브리지에 있기 때문이다 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);

	/* Bar 0: type 1 extended configuration space */
	size = resource_size(&pcie->cs);
	/* [한국어] config 창의 CPU 시작 주소. **FPCI 쪽은 여기서 쓰지 않는다** --
	 * tegra_pcie_map_bus 가 접근할 때마다 새로 쓰기 때문이다 */
	afi_writel(pcie, pcie->cs.start, AFI_AXI_BAR0_START);
	/* [한국어] config 창의 크기. **4KiB 단위** 라 12비트 오른쪽으로 민다 */
	afi_writel(pcie, size >> 12, AFI_AXI_BAR0_SZ);

	/* [한국어] 브리지의 창 목록을 순회한다 */
	resource_list_for_each_entry(entry, &bridge->windows) {
		/* [한국어] FPCI 쪽 주소와 AXI 쪽 주소를 담을 변수 */
		u32 fpci_bar, axi_address;
		/* [한국어] 이 창의 리소스 */
		struct resource *res = entry->res;

		/* [한국어] 이 창의 크기를 구한다 */
		size = resource_size(res);

		/* [한국어] 창의 종류로 갈린다. IORESOURCE_BUS 같은 그 밖의 종류는 어느 case 에도
		 * 걸리지 않아 조용히 건너뛴다 */
		switch (resource_type(res)) {
		case IORESOURCE_IO:
			/* Bar 1: downstream IO bar */
			fpci_bar = 0xfdfc0000;
			/* [한국어] **IO 는 논리 포트 번호로 표현되어 있어 실제 주소로 바꿔야 한다** */
			axi_address = pci_pio_to_address(res->start);
			/* [한국어] IO 창의 CPU 시작 주소를 쓴다 */
			afi_writel(pcie, axi_address, AFI_AXI_BAR1_START);
			/* [한국어] IO 창의 크기. **4KiB 단위** 라 12비트 오른쪽으로 민다 */
			afi_writel(pcie, size >> 12, AFI_AXI_BAR1_SZ);
			/* [한국어] IO 의 FPCI 주소를 쓴다 */
			afi_writel(pcie, fpci_bar, AFI_FPCI_BAR1);
			break;
		/* [한국어] 메모리 창은 prefetchable 여부로 다시 갈린다 */
		case IORESOURCE_MEM:
			fpci_bar = (((res->start >> 12) & 0x0fffffff) << 4) | 0x1;
			/* [한국어] 메모리는 CPU 주소를 그대로 쓴다. IO 가 pci_pio_to_address 로 변환해야
			 * 하는 것과 대비된다 */
			axi_address = res->start;

			if (res->flags & IORESOURCE_PREFETCH) {
				/* Bar 2: prefetchable memory BAR */
				afi_writel(pcie, axi_address, AFI_AXI_BAR2_START);
				/* [한국어] prefetchable 메모리 창의 크기 */
				afi_writel(pcie, size >> 12, AFI_AXI_BAR2_SZ);
				/* [한국어] prefetchable 메모리의 FPCI 주소 */
				afi_writel(pcie, fpci_bar, AFI_FPCI_BAR2);

			} else {
				/* Bar 3: non prefetchable memory BAR */
				afi_writel(pcie, axi_address, AFI_AXI_BAR3_START);
				/* [한국어] non-prefetchable 메모리 창의 크기 */
				afi_writel(pcie, size >> 12, AFI_AXI_BAR3_SZ);
				/* [한국어] non-prefetchable 메모리의 FPCI 주소 */
				afi_writel(pcie, fpci_bar, AFI_FPCI_BAR3);
			}
			break;
		}
	}

	/* NULL out the remaining BARs as they are not used */
	afi_writel(pcie, 0, AFI_AXI_BAR4_START);
	/* [한국어] BAR4 의 크기 */
	afi_writel(pcie, 0, AFI_AXI_BAR4_SZ);
	/* [한국어] BAR4 의 FPCI 주소 */
	afi_writel(pcie, 0, AFI_FPCI_BAR4);

	/* [한국어] BAR5 의 시작 주소 */
	afi_writel(pcie, 0, AFI_AXI_BAR5_START);
	/* [한국어] BAR5 의 크기 */
	afi_writel(pcie, 0, AFI_AXI_BAR5_SZ);
	/* [한국어] BAR5 의 FPCI 주소 */
	afi_writel(pcie, 0, AFI_FPCI_BAR5);

	if (pcie->soc->has_cache_bars) {
		/* map all upstream transactions as uncached */
		afi_writel(pcie, 0, AFI_CACHE_BAR0_ST);
		/* [한국어] 캐시 BAR0 크기 */
		afi_writel(pcie, 0, AFI_CACHE_BAR0_SZ);
		/* [한국어] 캐시 BAR1 시작 */
		afi_writel(pcie, 0, AFI_CACHE_BAR1_ST);
		/* [한국어] 캐시 BAR1 크기 */
		afi_writel(pcie, 0, AFI_CACHE_BAR1_SZ);
	}

	/* MSI translations are setup only when needed */
	afi_writel(pcie, 0, AFI_MSI_FPCI_BAR_ST);
	/* [한국어] MSI 창 크기를 지운다. **바로 아래에서 같은 레지스터에 또 0 을 쓴다** --
	 * AXI 쪽과 짝을 맞추려는 반복으로 보인다 */
	afi_writel(pcie, 0, AFI_MSI_BAR_SZ);
	/* [한국어] AXI 쪽 MSI 수신 주소도 지운다 */
	afi_writel(pcie, 0, AFI_MSI_AXI_BAR_ST);
	afi_writel(pcie, 0, AFI_MSI_BAR_SZ);
}

/* [한국어]
 * tegra_pcie_pll_wait - PHY PLL 이 잠길 때까지 기다린다
 *
 * @pcie:    컨트롤러 상태.
 * @timeout: 기다릴 시간(밀리초). 호출자가 500 을 준다.
 * @return: 0 이면 잠김. 시간 안에 안 잠기면 -ETIMEDOUT.
 *
 * PLL 이 목표 주파수에 고정(lock)되어야 PHY 가 동작한다. 잠금 검출 비트를
 * 반복해서 읽는다.
 *
 * **바쁜 대기(busy-wait)** 라는 점에 주의한다. 루프 안에 usleep 이나
 * cpu_relax 가 없어 CPU 를 계속 돌린다. 다른 폴링 함수들(예:
 * tegra_pcie_port_check_link)이 usleep_range 를 넣는 것과 대비된다.
 * PLL 잠금이 보통 매우 빨라 실제로는 몇 바퀴 만에 끝나기 때문으로 보이나,
 * 실패 경로에서는 500ms 동안 CPU 를 점유한다.
 *
 * jiffies 기반이라 해상도가 타이머 주기(보통 1~10ms)에 묶인다. 마이크로초
 * 단위 정밀도가 필요 없는 자리다.
 *
 * 레지스터 오프셋을 soc 에서 가져오는 데 주의 -- PLL 제어 레지스터가
 * Tegra20 은 0xb8, Tegra30 이후는 0xb4 로 다르다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 선점은 가능하지만 잠들지는 않는다.
 *
 * 호출 체인:
 *   tegra_pcie_phy_enable → [이 함수] → pads_readl
 */
static int tegra_pcie_pll_wait(struct tegra_pcie *pcie, unsigned long timeout)
{
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	/* [한국어] jiffies 기반 시한을 잡는다. 해상도가 타이머 주기(보통 1~10ms)에
	 * 묶이지만, 마이크로초 정밀도가 필요 없는 자리다 */
	timeout = jiffies + msecs_to_jiffies(timeout);

	/* [한국어] **바쁜 대기다.** 루프 안에 usleep 이나 cpu_relax 가 없어 CPU 를 계속
	 * 돌린다. PLL 잠금이 보통 매우 빨라 몇 바퀴 만에 끝나기 때문으로
	 * 보이나, 실패 경로에서는 500ms 동안 CPU 를 점유한다 */
	while (time_before(jiffies, timeout)) {
		/* [한국어] PLL 제어 레지스터를 읽는다. 오프셋이 SoC 마다 다르다 */
		value = pads_readl(pcie, soc->pads_pll_ctl);
		/* [한국어] 잠금 검출 비트가 서면 성공이다 */
		if (value & PADS_PLL_CTL_LOCKDET)
			return 0;
	}

	return -ETIMEDOUT;
}

/* [한국어]
 * tegra_pcie_phy_enable - 내장 PHY 를 직접 켠다 (구형 경로)
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공. PLL 이 안 잠기면 -ETIMEDOUT.
 *
 * PADS 레지스터를 직접 만져 PHY 를 살린다. **별도의 PHY 드라이버가 없는
 * 보드에서만** 쓰인다 -- tegra_pcie_phy_power_on 이 pcie->phy 가 NULL 일 때만
 * 이 함수를 부른다.
 *
 * 순서:
 *   1) PADS_CTL_SEL 에 0 을 써 레인 선택을 초기화한다. 위의 원문 주석은
 *      최대 16레인까지 쓸 수 있게 한다고 적는다.
 *   2) IDDQ 를 켠다. IDDQ 는 보통 아날로그 블록을 저전력 상태로 두는
 *      신호이므로, PLL 을 설정하는 동안 출력을 죽여 두는 것으로 읽힌다.
 *   3) PLL 입력을 고른다. 참조 클록으로 내부 CML 을 쓰고, TX 참조 분주비를
 *      soc->tx_ref_sel 로 정한다. 위의 원문 주석이 div10 을 쓴다고 적지만,
 *      실제 값은 SoC 마다 다르다 -- Tegra20 만 TXCLKREF_DIV10 이고
 *      Tegra30 이후는 TXCLKREF_BUF_EN 이다. 주석이 Tegra20 기준으로 남아
 *      있는 셈이다.
 *   4) PLL 을 리셋에 넣었다가(RST_B4SM 지우기) 20~100us 뒤 푼다.
 *   5) 잠금을 기다린다. 실패하면 여기서 끝낸다 -- 뒤의 정리를 하지 않고
 *      그대로 반환하므로 IDDQ 가 켜진 채 남는다.
 *   6) IDDQ 를 끄고 TX/RX 데이터를 켠다. 이 두 단계로 PHY 가 실제로
 *      신호를 내보내기 시작한다.
 *
 * **뒷세대와의 대비**: pcie-tegra194.c 는 이런 함수가 없다. UPHY 를
 * 드라이버가 직접 만지지 않고 BPMP 펌웨어에 메시지를 보내
 * (tegra_pcie_bpmp_set_pll_state) 맡긴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 4)에서 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_phy_power_on → [이 함수]
 *     → pads_readl, pads_writel, tegra_pcie_pll_wait
 */
static int tegra_pcie_phy_enable(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] pads_pll_ctl 오프셋과 tx_ref_sel 을 보기 위해 필요하다 */
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* initialize internal PHY, enable up to 16 PCIE lanes */
	pads_writel(pcie, 0x0, PADS_CTL_SEL);

	/* override IDDQ to 1 on all 4 lanes */
	value = pads_readl(pcie, PADS_CTL);
	/* [한국어] **PLL 을 설정하는 동안 출력을 죽여 둔다.** 아날로그 블록을 저전력
	 * 상태로 두는 신호로 읽힌다 */
	value |= PADS_CTL_IDDQ_1L;
	/* [한국어] IDDQ 를 켠다 */
	pads_writel(pcie, value, PADS_CTL);

	/*
	 * Set up PHY PLL inputs select PLLE output as refclock,
	 * set TX ref sel to div10 (not div5).
	 */
	value = pads_readl(pcie, soc->pads_pll_ctl);
	/* [한국어] 두 필드를 함께 지운다 */
	value &= ~(PADS_PLL_CTL_REFCLK_MASK | PADS_PLL_CTL_TXCLKREF_MASK);
	/* [한국어] 내부 CML 을 참조 클록으로 쓰고, TX 참조 분주비는 SoC 값을 쓴다.
	 * **위의 원문 주석은 div10 이라 적지만 실제 값은 SoC 마다 다르다** --
	 * Tegra20 만 DIV10 이고 Tegra30 이후는 BUF_EN 이다 */
	value |= PADS_PLL_CTL_REFCLK_INTERNAL_CML | soc->tx_ref_sel;
	/* [한국어] PLL 입력 설정을 쓴다 */
	pads_writel(pcie, value, soc->pads_pll_ctl);

	/* reset PLL */
	value = pads_readl(pcie, soc->pads_pll_ctl);
	/* [한국어] 리셋 해제 비트를 지운다 */
	value &= ~PADS_PLL_CTL_RST_B4SM;
	/* [한국어] PLL 을 리셋에 넣는다 */
	pads_writel(pcie, value, soc->pads_pll_ctl);

	/* [한국어] 20~100us 유지한다 */
	usleep_range(20, 100);

	/* take PLL out of reset  */
	value = pads_readl(pcie, soc->pads_pll_ctl);
	/* [한국어] 리셋 해제 비트를 세운다 */
	value |= PADS_PLL_CTL_RST_B4SM;
	/* [한국어] 리셋에서 꺼낸다 */
	pads_writel(pcie, value, soc->pads_pll_ctl);

	/* wait for the PLL to lock */
	err = tegra_pcie_pll_wait(pcie, 500);
	/* [한국어] **실패하면 여기서 끝낸다.** 뒤의 정리를 하지 않으므로 IDDQ 가
	 * 켜진 채 남는다 */
	if (err < 0) {
		/* [한국어] PLL 이 안 잠기면 PHY 를 쓸 수 없다 */
		dev_err(dev, "PLL failed to lock: %d\n", err);
		return err;
	}

	/* turn off IDDQ override */
	value = pads_readl(pcie, PADS_CTL);
	/* [한국어] PLL 이 잠겼으므로 저전력 상태를 푼다 */
	value &= ~PADS_CTL_IDDQ_1L;
	/* [한국어] IDDQ 를 끈다 */
	pads_writel(pcie, value, PADS_CTL);

	/* enable TX/RX data */
	value = pads_readl(pcie, PADS_CTL);
	/* [한국어] TX 와 RX 데이터를 함께 켠다 */
	value |= PADS_CTL_TX_DATA_EN_1L | PADS_CTL_RX_DATA_EN_1L;
	/* [한국어] **PHY 가 실제로 신호를 내보내기 시작한다.** 이것이 마지막 단계다 */
	pads_writel(pcie, value, PADS_CTL);

	return 0;
}

/* [한국어]
 * tegra_pcie_phy_disable - 내장 PHY 를 직접 끈다 (구형 경로)
 *
 * @pcie: 컨트롤러 상태.
 * @return: 항상 0. 실패할 여지가 없는데도 int 를 돌려주는 것은
 *   tegra_pcie_phy_power_off 가 phy_power_off 와 같은 자리에서 이 함수를
 *   부르며 반환값을 같은 방식으로 다루기 때문이다.
 *
 * tegra_pcie_phy_enable 의 역순이다.
 *   1) TX/RX 데이터를 끈다.
 *   2) IDDQ 를 켜 아날로그 블록을 저전력 상태로 둔다.
 *   3) PLL 을 리셋에 넣는다.
 *   4) 20~100us 기다린다. 리셋이 실제로 적용될 시간을 준다.
 *
 * 켤 때와 달리 PLL 을 리셋에서 꺼내지 않고 그대로 둔다 -- 끄는 것이
 * 목적이므로 당연하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 4)에서 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_phy_power_off → [이 함수] → pads_readl, pads_writel
 */
static int tegra_pcie_phy_disable(struct tegra_pcie *pcie)
{
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	/* disable TX/RX data */
	value = pads_readl(pcie, PADS_CTL);
	/* [한국어] 두 비트를 함께 지운다. 이것이 PHY 를 끄는 첫 단계다 */
	value &= ~(PADS_CTL_TX_DATA_EN_1L | PADS_CTL_RX_DATA_EN_1L);
	/* [한국어] TX/RX 데이터를 끈다 */
	pads_writel(pcie, value, PADS_CTL);

	/* override IDDQ */
	value = pads_readl(pcie, PADS_CTL);
	/* [한국어] 아날로그 블록을 저전력 상태로 둔다 */
	value |= PADS_CTL_IDDQ_1L;
	/* [한국어] IDDQ 를 켠다 */
	pads_writel(pcie, value, PADS_CTL);

	/* reset PLL */
	value = pads_readl(pcie, soc->pads_pll_ctl);
	/* [한국어] 리셋 해제 비트를 지운다. 1이 리셋 해제라 지우는 것이 리셋 어서트다 */
	value &= ~PADS_PLL_CTL_RST_B4SM;
	/* [한국어] PLL 을 리셋에 넣는다 */
	pads_writel(pcie, value, soc->pads_pll_ctl);

	/* [한국어] 리셋이 실제로 적용될 시간을 준다. **켤 때와 달리 리셋에서 꺼내지
	 * 않는다** -- 끄는 것이 목적이므로 그대로 둔다 */
	usleep_range(20, 100);

	return 0;
}

/* [한국어]
 * tegra_pcie_port_phy_power_on - 포트의 레인별 PHY 를 모두 켠다
 *
 * @port: 대상 포트.
 * @return: 0 성공. 하나라도 실패하면 그 오류를 올린다.
 *
 * 신형 경로다. 보드가 별도 PHY 드라이버를 쓰면 레인마다 PHY 하나씩이
 * 장치 트리에 있고, 이 함수가 그것들을 차례로 켠다.
 *
 * **실패해도 이미 켠 PHY 를 되돌리지 않는다.** 중간에서 반환해 버리므로
 * i-1 개가 켜진 채 남는다. 상위인 tegra_pcie_pm_resume 이 실패 시
 * tegra_pcie_power_off 로 전원 자체를 내리므로 결과적으로 정리되지만,
 * 이 함수만 놓고 보면 대칭이 아니다.
 *
 * phy_init 은 여기서 하지 않는다 -- 이미 tegra_pcie_port_get_phys 가
 * probe 때 끝냈다. 이 함수는 전원만 넣는다. PHY 프레임워크가 초기화와
 * 전원 인가를 나눠 두었기 때문이며, 초기화는 한 번이면 되지만 전원은
 * 절전마다 오르내리기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_phy_power_on → [이 함수] → phy_power_on
 */
static int tegra_pcie_port_phy_power_on(struct tegra_pcie_port *port)
{
	struct device *dev = port->pcie->dev;
	/* [한국어] 순회 첨자 */
	unsigned int i;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 레인 수만큼 순회한다 */
	for (i = 0; i < port->lanes; i++) {
		/* [한국어] 레인 하나의 PHY 전원을 켠다. **초기화는 이미 probe 때 끝났다** --
		 * PHY 프레임워크가 초기화와 전원 인가를 나눠 두었기 때문이며,
		 * 초기화는 한 번이면 되지만 전원은 절전마다 오르내린다 */
		err = phy_power_on(port->phys[i]);
		/* [한국어] 실패를 확인한다 */
		if (err < 0) {
			/* [한국어] 전원 인가 실패를 알린다 */
			dev_err(dev, "failed to power on PHY#%u: %d\n", i, err);
			return err;
		}
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_port_phy_power_off - 포트의 레인별 PHY 를 모두 끈다
 *
 * @port: 대상 포트.
 * @return: 0 성공. 하나라도 실패하면 그 오류를 올린다.
 *
 * tegra_pcie_port_phy_power_on 의 짝이다. 마찬가지로 중간에 실패하면
 * 나머지를 끄지 않고 반환한다 -- 되돌리는 경로에서 조기 반환은 남은
 * PHY 를 켜진 채로 남기지만, 상위가 전원을 내리므로 실질적 문제는 없다.
 *
 * phy_exit 은 여기서 하지 않는다. 그것은 드라이버가 떨어질 때
 * tegra_pcie_phys_put 이 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_phy_power_off → [이 함수] → phy_power_off
 */
static int tegra_pcie_port_phy_power_off(struct tegra_pcie_port *port)
{
	struct device *dev = port->pcie->dev;
	/* [한국어] 순회 첨자 */
	unsigned int i;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 레인 수만큼 순회한다 */
	for (i = 0; i < port->lanes; i++) {
		/* [한국어] 레인 하나의 PHY 전원을 끈다 */
		err = phy_power_off(port->phys[i]);
		/* [한국어] 실패를 확인한다 */
		if (err < 0) {
			/* [한국어] 전원 차단 실패를 알린다 */
			dev_err(dev, "failed to power off PHY#%u: %d\n", i,
				err);
			return err;
		}
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_phy_power_on - PHY 전원을 켠다. 구형/신형 경로를 가른다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * 이 파일의 PHY 처리가 세 갈래로 갈리는 지점이다.
 *
 *   legacy_phy 이고 pcie->phy 가 있으면 : 컨트롤러 전체에 PHY 하나가
 *     장치 트리에 있는 구성이다. 그 PHY 를 켠다.
 *   legacy_phy 인데 pcie->phy 가 NULL 이면 : PHY 드라이버가 아예 없는
 *     구성이다. tegra_pcie_phy_enable 이 PADS 레지스터를 직접 만진다.
 *   legacy_phy 가 아니면 : 포트마다 레인별 PHY 가 있는 신형 구성이다.
 *     포트를 순회하며 각각을 켠다.
 *
 * legacy_phy 는 tegra_pcie_phys_get 이 정한다 -- Gen2 를 지원하지 않는
 * SoC 이거나 장치 트리에 phys 속성이 없으면 구형으로 본다.
 *
 * 첫 번째 분기에서 err 이 초기화되지 않을 수 있어 보이지만, if/else 두
 * 갈래가 모두 err 에 대입하므로 실제로는 항상 정의된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_pm_resume → [이 함수]
 *     → phy_power_on / tegra_pcie_phy_enable / tegra_pcie_port_phy_power_on
 */
static int tegra_pcie_phy_power_on(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 순회할 포트 */
	struct tegra_pcie_port *port;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] **PHY 처리가 세 갈래로 갈리는 지점이다** */
	if (pcie->legacy_phy) {
		/* [한국어] PHY 핸들이 있는지 본다 */
		if (pcie->phy)
			/* [한국어] 컨트롤러 단위 PHY 하나를 켠다 */
			err = phy_power_on(pcie->phy);
		else
			/* [한국어] **PHY 드라이버가 없으면 PADS 레지스터를 직접 만진다.**
			 * 이것이 이 파일의 세 번째 PHY 경로다 */
			err = tegra_pcie_phy_enable(pcie);

		/* [한국어] 실패를 확인한다 */
		if (err < 0)
			/* [한국어] 구형 경로의 실패를 알린다 */
			dev_err(dev, "failed to power on PHY: %d\n", err);

		return err;
	}

	/* [한국어] 모든 포트를 순회한다 */
	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] 포트의 레인별 PHY 를 켠다 */
		err = tegra_pcie_port_phy_power_on(port);
		/* [한국어] 하나라도 실패하면 그대로 반환한다 */
		if (err < 0) {
			dev_err(dev,
				"failed to power on PCIe port %u PHY: %d\n",
				port->index, err);
			return err;
		}
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_phy_power_off - PHY 전원을 끈다. 구형/신형 경로를 가른다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * tegra_pcie_phy_power_on 과 정확히 같은 세 갈래로 갈린다. 켤 때 어느
 * 경로를 탔든 끌 때도 같은 경로를 타므로 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_pm_suspend → [이 함수]
 *     → phy_power_off / tegra_pcie_phy_disable / tegra_pcie_port_phy_power_off
 */
static int tegra_pcie_phy_power_off(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 순회할 포트 */
	struct tegra_pcie_port *port;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 구형 바인딩이면 여기서 갈린다 */
	if (pcie->legacy_phy) {
		/* [한국어] PHY 핸들이 있는지 본다 */
		if (pcie->phy)
			/* [한국어] 컨트롤러 단위 PHY 하나를 끈다 */
			err = phy_power_off(pcie->phy);
		else
			/* [한국어] **PHY 드라이버가 아예 없는 구성.** PADS 레지스터를 직접 만진다 */
			err = tegra_pcie_phy_disable(pcie);

		/* [한국어] 실패를 확인한다 */
		if (err < 0)
			/* [한국어] 구형 경로의 실패를 알린다 */
			dev_err(dev, "failed to power off PHY: %d\n", err);

		return err;
	}

	/* [한국어] 모든 포트를 순회한다 */
	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] 포트의 레인별 PHY 를 끈다 */
		err = tegra_pcie_port_phy_power_off(port);
		/* [한국어] 실패해도 나머지 포트를 마저 처리하지 않고 반환한다 */
		if (err < 0) {
			dev_err(dev,
				"failed to power off PCIe port %u PHY: %d\n",
				port->index, err);
			return err;
		}
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_enable_controller - 컨트롤러 전역 설정을 한 번에 적용한다
 *
 * @pcie: 컨트롤러 상태. 전원과 AFI 클록이 올라와 있어야 한다.
 * @return: 없음.
 *
 * 포트별 설정(tegra_pcie_port_enable)과 대비되는, 컨트롤러 전체에 걸린
 * 설정이다.
 *
 *   1) PLLE 전원 관리를 패드 쪽에 넘긴다(pcie->phy 가 있을 때만).
 *      BYPASS 를 지우고 PADS2PLLE_CONTROL_EN 을 세운다. 이름으로 미루어
 *      패드 상태에 따라 PLLE 를 자동으로 재울 수 있게 하는 설정이다.
 *   2) 슬롯 클록 바이어스 패드의 전원을 내린다(지원 SoC 만).
 *   3) **레인 배분과 포트 활성화.** 이 함수의 핵심이다.
 *      먼저 xbar 필드를 지우고 pcie->xbar_config 를 넣는다. 동시에
 *      "모든 포트 비활성화" 와 "모든 CLKREQ 를 GPIO 로" 를 세워 둔 뒤,
 *      실제로 쓰는 포트만 그 비트를 **지운다.** 즉 기본을 끔으로 두고
 *      쓰는 것만 켜는 방식이라, 장치 트리에서 빠진 포트가 자동으로 꺼진다.
 *   4) Gen2 퓨즈 비트를 SoC 능력에 맞춘다. has_gen2 면 "Gen2 금지" 비트를
 *      지우고, 아니면 세운다. 이름이 FUSE 인 것으로 보아 원래는 칩 퓨즈로
 *      정해지는 값을 소프트웨어가 덮어쓰는 레지스터로 보인다.
 *   5) AFI 동적 클록 게이팅을 끄고 FPCI 를 켠다. 원문 주석이 그 두 가지를
 *      함께 밝힌다.
 *   6) 오류 인터럽트를 켠다. 여섯 종류를 기본으로 켜고, 지원하는 SoC 는
 *      슬롯 존재 감지도 켠다. 그리고 SM 인터럽트는 0xffffffff 로 전부 켠다.
 *   7) **MSI 는 아직 켜지 않는다.** AFI_INTR_MASK 에 INT 마스크만 쓴다.
 *      원문 주석대로 필요할 때 tegra_pcie_enable_msi 가 MSI 비트를 더한다.
 *   8) FPCI 예외를 모두 끈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe 와 resume 이 공유한다.
 *
 * 호출 체인:
 *   tegra_pcie_pm_resume → [이 함수] → afi_readl, afi_writel
 */
static void tegra_pcie_enable_controller(struct tegra_pcie *pcie)
{
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 순회할 포트 */
	struct tegra_pcie_port *port;
	/* [한국어] 레지스터 값 임시 변수 */
	unsigned long value;

	/* enable PLL power down */
	if (pcie->phy) {
		/* [한국어] 현재 값을 읽는다 */
		value = afi_readl(pcie, AFI_PLLE_CONTROL);
		/* [한국어] 패드 제어 우회를 끈다 */
		value &= ~AFI_PLLE_CONTROL_BYPASS_PADS2PLLE_CONTROL;
		/* [한국어] 패드가 PLLE 를 제어하게 한다 */
		value |= AFI_PLLE_CONTROL_PADS2PLLE_CONTROL_EN;
		/* [한국어] PLLE 전원 관리 설정을 쓴다 */
		afi_writel(pcie, value, AFI_PLLE_CONTROL);
	}

	/* power down PCIe slot clock bias pad */
	if (soc->has_pex_bias_ctrl)
		/* [한국어] 슬롯 클록 바이어스 패드의 전원을 내린다 */
		afi_writel(pcie, 0, AFI_PEXBIAS_CTRL_0);

	/* configure mode and disable all ports */
	value = afi_readl(pcie, AFI_PCIE_CONFIG);
	/* [한국어] 기존 레인 배분 필드를 지운다 */
	value &= ~AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_MASK;
	/* [한국어] **기본을 "전부 끔"으로 두고** 레인 배분 값을 겹친다 */
	value |= AFI_PCIE_CONFIG_PCIE_DISABLE_ALL | pcie->xbar_config;
	/* [한국어] 세 포트의 CLKREQ 를 모두 GPIO 로 돌린다 */
	value |= AFI_PCIE_CONFIG_PCIE_CLKREQ_GPIO_ALL;

	/* [한국어] 장치 트리에 있는 포트만 순회한다 -- 없는 포트는 위에서 세운 비활성화
	 * 비트가 그대로 남아 꺼진 채로 있게 된다 */
	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] **이 포트를 활성화한다.** 비트를 지우는 것이 활성화다 */
		value &= ~AFI_PCIE_CONFIG_PCIE_DISABLE(port->index);
		/* [한국어] 이 포트의 CLKREQ 를 PCIe 기능으로 되돌린다 */
		value &= ~AFI_PCIE_CONFIG_PCIE_CLKREQ_GPIO(port->index);
	}

	/* [한국어] 레인 배분과 포트 활성화를 한 번에 쓴다 */
	afi_writel(pcie, value, AFI_PCIE_CONFIG);

	/* [한국어] SoC 가 Gen2 를 지원하는지에 따라 갈린다 */
	if (soc->has_gen2) {
		/* [한국어] 현재 값을 읽는다 */
		value = afi_readl(pcie, AFI_FUSE);
		/* [한국어] **Gen2 금지 비트를 지운다** -- 즉 Gen2 를 허용한다 */
		value &= ~AFI_FUSE_PCIE_T0_GEN2_DIS;
		/* [한국어] 퓨즈 레지스터에 쓴다 */
		afi_writel(pcie, value, AFI_FUSE);
	} else {
		/* [한국어] 현재 값을 읽는다 */
		value = afi_readl(pcie, AFI_FUSE);
		/* [한국어] **Gen2 를 금지한다.** 이름대로 1이 금지다 */
		value |= AFI_FUSE_PCIE_T0_GEN2_DIS;
		/* [한국어] 퓨즈 레지스터에 쓴다 */
		afi_writel(pcie, value, AFI_FUSE);
	}

	/* Disable AFI dynamic clock gating and enable PCIe */
	value = afi_readl(pcie, AFI_CONFIGURATION);
	/* [한국어] FPCI 를 켠다. 이것이 없으면 주소 변환이 동작하지 않는다 */
	value |= AFI_CONFIGURATION_EN_FPCI;
	/* [한국어] AFI 동적 클록 게이팅을 끈다 */
	value |= AFI_CONFIGURATION_CLKEN_OVERRIDE;
	/* [한국어] 두 비트를 함께 쓴다 */
	afi_writel(pcie, value, AFI_CONFIGURATION);

	/* [한국어] 개시자 쪽 오류 둘 */
	value = AFI_INTR_EN_INI_SLVERR | AFI_INTR_EN_INI_DECERR |
		/* [한국어] 타깃 쪽 오류 셋 */
		AFI_INTR_EN_TGT_SLVERR | AFI_INTR_EN_TGT_DECERR |
		/* [한국어] 여섯 종류를 기본으로 켠다. 정의된 아홉 중 AXI_DECERR 와 FPCI_TIMEOUT
		 * (비트 6~7)은 켜지 않는다 */
		AFI_INTR_EN_TGT_WRERR | AFI_INTR_EN_DFPCI_DECERR;

	/* [한국어] 존재 감지 인터럽트를 지원하는지 본다 */
	if (soc->has_intr_prsnt_sense)
		/* [한국어] 지원하는 SoC 만 슬롯 존재 감지를 더한다 */
		value |= AFI_INTR_EN_PRSNT_SENSE;

	/* [한국어] 오류 인터럽트 활성화를 쓴다 */
	afi_writel(pcie, value, AFI_AFI_INTR_ENABLE);
	/* [한국어] **INTx 어서트/디어서트 이벤트를 전부 켠다.** 개별 비트 정의가 위에
	 * 있는데도 통째로 켜므로, 그 정의들은 실제로 쓰이지 않는다 */
	afi_writel(pcie, 0xffffffff, AFI_SM_INTR_ENABLE);

	/* don't enable MSI for now, only when needed */
	afi_writel(pcie, AFI_INTR_MASK_INT_MASK, AFI_INTR_MASK);

	/* disable all exceptions */
	afi_writel(pcie, 0, AFI_FPCI_ERROR_MASKS);
}

/* [한국어]
 * tegra_pcie_power_off - 컨트롤러 전원을 내린다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음. 되돌리는 경로라 regulator 실패도 경고만 찍는다.
 *
 * tegra_pcie_power_on 의 역순이다 -- AFI 리셋, 클록 셋, 전원 게이트,
 * regulator 순으로 내린다.
 *
 * pex_clk 를 여기서 끄지 않는 데 주의한다. 그것은 상위인
 * tegra_pcie_pm_suspend 가 따로 끈다. 이 함수가 끄는 것은 afi_clk,
 * cml_clk(지원 SoC 만), pll_e 셋이다.
 *
 * 전원 게이트를 dev->pm_domain 이 없을 때만 만지는 이유: 전원 도메인
 * 프레임워크가 붙어 있으면 그쪽이 알아서 관리하므로 드라이버가 직접
 * 게이트를 조작하면 이중 관리가 된다. 같은 조건 검사가 이 파일의
 * 전원 관련 네 곳에 모두 있다.
 *
 * regulator 실패에 dev_warn 을 쓰는 것은 이미 내려가는 중이라 할 수 있는
 * 일이 없기 때문이다. 켤 때는 dev_err 로 찍고 진행한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_pm_suspend / tegra_pcie_pm_resume(오류) → [이 함수]
 */
static void tegra_pcie_power_off(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] has_cml_clk 를 보기 위해 SoC 기술자가 필요하다 */
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 오류 코드 보관용 */
	int err;

	reset_control_assert(pcie->afi_rst);

	clk_disable_unprepare(pcie->pll_e);
	/* [한국어] 지원하는 SoC 에서만 CML 클록을 끈다 */
	if (soc->has_cml_clk)
		clk_disable_unprepare(pcie->cml_clk);
	clk_disable_unprepare(pcie->afi_clk);

	/* [한국어] 전원 도메인 프레임워크가 없을 때만 게이트를 직접 내린다 */
	if (!dev->pm_domain)
		tegra_powergate_power_off(TEGRA_POWERGATE_PCIE);

	/* [한국어] regulator 를 끈다. 전원 인가의 역순에서 마지막이다 */
	err = regulator_bulk_disable(pcie->num_supplies, pcie->supplies);
	/* [한국어] 되돌리는 경로라 dev_warn 으로 낮춘다. 켤 때는 dev_err 로 찍는다 */
	if (err < 0)
		dev_warn(dev, "failed to disable regulators: %d\n", err);
}

/* [한국어]
 * tegra_pcie_power_on - 컨트롤러 전원을 올린다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * 전원 인가의 첫 단계다. 순서가 아래에서 위로 -- 리셋으로 붙들어 두고,
 * 전원을 넣고, 클록을 켜고, 마지막에 AFI 리셋만 푼다.
 *
 *   1) 리셋 셋을 모두 어서트한다. 전원을 넣는 동안 컨트롤러가 임의의
 *      상태로 동작하지 않게 한다.
 *   2) 전원 게이트를 내린다. 이미 켜져 있었을 수 있으므로 확실히 끈 뒤
 *      다시 켜는 방식이다.
 *   3) regulator 를 켠다. **실패해도 진행한다** -- dev_err 로 찍기만 하고
 *      goto 하지 않는다. 다른 실패와 다르게 다루는 이유는 코드에 적혀
 *      있지 않다.
 *   4) 전원 게이트를 켜고 클램프를 푼다. 클램프는 전원이 꺼진 블록의
 *      출력이 떠다니지 않게 붙들어 두는 회로이므로, 전원을 넣은 뒤
 *      풀어야 한다.
 *   5) 클록을 순서대로 켠다 -- AFI, CML(지원 SoC 만), PLLE.
 *   6) **AFI 리셋만 푼다.** pex_rst 와 pcie_xrst 는 어서트된 채로 남는다.
 *      pex_rst 는 tegra_pcie_pm_resume 이 나중에 풀고, pcie_xrst 는
 *      tegra_pcie_enable_ports 가 링크 훈련을 시작할 때 푼다. 이 세 리셋의
 *      해제 시점이 다른 것이 이 드라이버 기동 순서의 뼈대다.
 *
 * 정리 경로가 네 라벨로 층져 있다. 각 라벨은 그 직전까지 성공한 것만
 * 되돌린다. disable_cml_clk 는 has_cml_clk 를 다시 검사하는데, CML 클록을
 * 켜지 않은 SoC 에서 이 라벨로 떨어질 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_pm_resume → [이 함수]
 */
static int tegra_pcie_power_on(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] has_cml_clk 를 보기 위해 SoC 기술자가 필요하다 */
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 오류 코드 보관용 */
	int err;

	reset_control_assert(pcie->pcie_xrst);
	reset_control_assert(pcie->afi_rst);
	reset_control_assert(pcie->pex_rst);

	/* [한국어] **이미 켜져 있었을 수 있으므로 확실히 끈다.** 그 뒤 아래에서 다시 켠다 */
	if (!dev->pm_domain)
		tegra_powergate_power_off(TEGRA_POWERGATE_PCIE);

	/* enable regulators */
	err = regulator_bulk_enable(pcie->num_supplies, pcie->supplies);
	/* [한국어] 실패를 확인하되 멈추지 않는다 */
	if (err < 0)
		/* [한국어] **실패해도 진행한다** -- 로그만 찍고 goto 하지 않는다.
		 * 다른 실패와 다르게 다루는 이유는 코드에 적혀 있지 않다 */
		dev_err(dev, "failed to enable regulators: %d\n", err);

	/* [한국어] 전원 도메인 프레임워크가 없을 때만 게이트를 직접 조작한다.
	 * 있으면 그쪽이 관리하므로 이중 관리가 된다 */
	if (!dev->pm_domain) {
		/* [한국어] 전원 게이트를 켠다. **이 함수의 정의는 이 트리에 없다**
		 * (include/soc/tegra 부재) -- 이름과 쓰임으로만 설명한다 */
		err = tegra_powergate_power_on(TEGRA_POWERGATE_PCIE);
		/* [한국어] 실패를 확인한다 */
		if (err) {
			/* [한국어] 전원 게이트 해제 실패 */
			dev_err(dev, "failed to power ungate: %d\n", err);
			goto regulator_disable;
		}
		/* [한국어] **클램프를 푼다.** 클램프는 전원이 꺼진 블록의 출력이 떠다니지 않게
		 * 붙들어 두는 회로이므로, 전원을 넣은 뒤 풀어야 한다 */
		err = tegra_powergate_remove_clamping(TEGRA_POWERGATE_PCIE);
		/* [한국어] 실패를 확인한다 */
		if (err) {
			/* [한국어] 클램프 해제 실패 */
			dev_err(dev, "failed to remove clamp: %d\n", err);
			goto powergate;
		}
	}

	/* [한국어] **AFI 클록을 먼저 켠다.** AFI 레지스터를 만져야 나머지 설정이 가능하다 */
	err = clk_prepare_enable(pcie->afi_clk);
	/* [한국어] 실패하면 전원 게이트부터 되감는다 */
	if (err < 0) {
		/* [한국어] AFI 클록 실패 */
		dev_err(dev, "failed to enable AFI clock: %d\n", err);
		goto powergate;
	}

	/* [한국어] 지원하는 SoC 에서만 켠다 */
	if (soc->has_cml_clk) {
		/* [한국어] CML 클록을 켠다 */
		err = clk_prepare_enable(pcie->cml_clk);
		/* [한국어] 실패하면 AFI 클록부터 되감는다 */
		if (err < 0) {
			/* [한국어] CML 클록 실패 */
			dev_err(dev, "failed to enable CML clock: %d\n", err);
			goto disable_afi_clk;
		}
	}

	/* [한국어] PLLE 를 켠다. 클록 셋 중 마지막이다 */
	err = clk_prepare_enable(pcie->pll_e);
	/* [한국어] 실패하면 CML 클록부터 되감는다 */
	if (err < 0) {
		/* [한국어] PLLE 를 못 켜면 PHY 가 동작하지 않는다 */
		dev_err(dev, "failed to enable PLLE clock: %d\n", err);
		goto disable_cml_clk;
	}

	reset_control_deassert(pcie->afi_rst);

	return 0;

disable_cml_clk:
	if (soc->has_cml_clk)
		clk_disable_unprepare(pcie->cml_clk);
disable_afi_clk:
	clk_disable_unprepare(pcie->afi_clk);
powergate:
	if (!dev->pm_domain)
		tegra_powergate_power_off(TEGRA_POWERGATE_PCIE);
regulator_disable:
	regulator_bulk_disable(pcie->num_supplies, pcie->supplies);

	return err;
}

/* [한국어]
 * tegra_pcie_apply_pad_settings - 참조 클록 드라이버 설정을 패드에 쓴다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음.
 *
 * soc 기술자가 들고 있는 값을 PADS_REFCLK_CFG 레지스터에 그대로 밀어 넣는다.
 *
 * **들어가는 값의 의미는 알 수 없다.** PADS_REFCLK_CFG 필드 정의 위의
 * 원문 주석이 그 사실을 직접 밝히는데, "이 필드 정의와 원하는 값은 TRM 에
 * 없고 NVIDIA 에서 받은 것" 이라고 적혀 있다. 그 주석이 알려 주는 것은
 * 이 레지스터들이 16비트 항목의 배열이고 포트마다 한 항목이라는 사실,
 * 그리고 네 필드(TERM, E_TERM, PREDI, DRVI)의 비트 위치뿐이다.
 *
 * 값이 0xfa5cfa5c 처럼 같은 16비트가 두 번 반복되는 형태인 것이 그
 * "포트마다 한 항목" 구조와 맞아떨어진다 -- 두 포트에 같은 설정을 준다는
 * 뜻이다. Tegra186 의 cfg1 만 0x000480b8 로 상하위가 다른데, 셋째 포트만
 * 설정이 다르다는 의미로 읽힌다.
 *
 * 두 번째 레지스터는 포트가 셋 이상인 SoC 에서만 쓴다. 항목이 둘씩
 * 들어가므로 포트 3개면 두 레지스터가 필요하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 포트를 기동하기 직전에 불린다.
 *
 * 호출 체인:
 *   tegra_pcie_pm_resume → [이 함수] → pads_writel
 */
static void tegra_pcie_apply_pad_settings(struct tegra_pcie *pcie)
{
	const struct tegra_pcie_soc *soc = pcie->soc;

	/* Configure the reference clock driver */
	pads_writel(pcie, soc->pads_refclk_cfg0, PADS_REFCLK_CFG0);

	/* [한국어] **포트가 셋 이상인 SoC 에서만 두 번째 레지스터를 쓴다.**
	 * 항목이 둘씩 들어가므로 포트 3개면 두 레지스터가 필요하기 때문이다 */
	if (soc->num_ports > 2)
		pads_writel(pcie, soc->pads_refclk_cfg1, PADS_REFCLK_CFG1);
}

/* [한국어]
 * tegra_pcie_clocks_get - 필요한 클록 핸들을 모두 얻는다
 *
 * @pcie: 컨트롤러 상태. 네 클록 필드를 채운다.
 * @return: 0 성공. 하나라도 못 얻으면 그 오류(-EPROBE_DEFER 포함).
 *
 * 클록 넷을 이름으로 하나씩 얻는다.
 *   pex   : PCIe 코어 클록. tegra_pcie_pm_resume 이 켜고 _suspend 가 끈다.
 *   afi   : AFI 블록 클록. tegra_pcie_power_on 이 켠다.
 *   pll_e : PLLE. 역시 power_on 이 켠다.
 *   cml   : has_cml_clk 인 SoC 에서만. Tegra30/124/210 이 쓰고
 *           Tegra20 과 Tegra186 은 쓰지 않는다.
 *
 * 앞의 셋은 필수라 devm_clk_get 으로 얻고 실패하면 그대로 오류다.
 * 넷째만 조건부인데, 그 조건이 optional 판을 쓰는 것이 아니라 SoC 기술자의
 * 불리언으로 갈린다는 점에 주의한다 -- 즉 지원 SoC 에서는 반드시 있어야 한다.
 *
 * **뒷세대와의 대비**: pcie-tegra194.c 는 클록이 core 와 core_m 둘뿐이다.
 * 클록 관리의 상당 부분이 BPMP 펌웨어 쪽으로 넘어갔기 때문이다.
 *
 * 실행 컨텍스트: probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   tegra_pcie_get_resources → [이 함수] → devm_clk_get
 */
static int tegra_pcie_clocks_get(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] has_cml_clk 를 보기 위해 SoC 기술자가 필요하다 */
	const struct tegra_pcie_soc *soc = pcie->soc;

	/* [한국어] PCIe 코어 클록을 얻는다. **앞의 셋은 필수** 라 optional 판을 쓰지 않는다 */
	pcie->pex_clk = devm_clk_get(dev, "pex");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->pex_clk))
		/* [한국어] 획득 실패는 -EPROBE_DEFER 일 수 있다 */
		return PTR_ERR(pcie->pex_clk);

	/* [한국어] AFI 블록 클록을 얻는다 */
	pcie->afi_clk = devm_clk_get(dev, "afi");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->afi_clk))
		/* [한국어] 획득 실패 */
		return PTR_ERR(pcie->afi_clk);

	/* [한국어] PLLE 클록을 얻는다 */
	pcie->pll_e = devm_clk_get(dev, "pll_e");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->pll_e))
		/* [한국어] 획득 실패 */
		return PTR_ERR(pcie->pll_e);

	/* [한국어] **넷째만 조건부다.** optional 판을 쓰는 것이 아니라 SoC 기술자의
	 * 불리언으로 갈리므로, 지원 SoC 에서는 반드시 있어야 한다 */
	if (soc->has_cml_clk) {
		/* [한국어] CML 클록을 얻는다 */
		pcie->cml_clk = devm_clk_get(dev, "cml");
		/* [한국어] 실패를 확인한다 */
		if (IS_ERR(pcie->cml_clk))
			/* [한국어] 획득 실패 */
			return PTR_ERR(pcie->cml_clk);
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_resets_get - 리셋 핸들 셋을 얻는다
 *
 * @pcie: 컨트롤러 상태. 세 리셋 필드를 채운다.
 * @return: 0 성공, 못 얻으면 그 오류.
 *
 * 리셋 셋을 모두 exclusive 로 얻는다 -- 다른 장치와 나눠 쓰지 않는다는
 * 뜻이다. 셋의 역할과 해제 시점이 서로 다른 것이 이 드라이버 기동 순서의
 * 뼈대다.
 *
 *   pex_rst    : PCIe 블록 리셋. tegra_pcie_pm_resume 이 클록을 켠 뒤 푼다.
 *   afi_rst    : AFI 블록 리셋. tegra_pcie_power_on 이 마지막에 푼다.
 *                가장 먼저 풀리는데, AFI 레지스터를 만져야 나머지 설정이
 *                가능하기 때문이다.
 *   pcie_xrst  : **링크 훈련 시작 스위치.** tegra_pcie_enable_ports 가
 *                모든 포트 설정을 마친 뒤에 풀며, 그 순간 LTSSM 이 돈다.
 *                그 함수의 원문 주석이 "Start LTSSM from Tegra side" 라고
 *                그 사실을 적고 있다.
 *
 * 셋 다 optional 이 아니므로 장치 트리에 없으면 probe 가 실패한다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_get_resources → [이 함수] → devm_reset_control_get_exclusive
 */
static int tegra_pcie_resets_get(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;

	/* [한국어] PCIe 블록 리셋. **셋 다 exclusive 로 얻는다** -- 다른 장치와 나눠 쓰지
	 * 않는다는 뜻이다 */
	pcie->pex_rst = devm_reset_control_get_exclusive(dev, "pex");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->pex_rst))
		/* [한국어] 획득 실패 */
		return PTR_ERR(pcie->pex_rst);

	/* [한국어] AFI 블록 리셋. 셋 중 **가장 먼저 풀린다** -- AFI 레지스터를 만져야
	 * 나머지 설정이 가능하기 때문이다 */
	pcie->afi_rst = devm_reset_control_get_exclusive(dev, "afi");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->afi_rst))
		/* [한국어] 획득 실패 */
		return PTR_ERR(pcie->afi_rst);

	/* [한국어] **링크 훈련 시작 스위치.** tegra_pcie_enable_ports 가 모든 포트 설정을
	 * 마친 뒤 이것을 풀며, 그 순간 LTSSM 이 돈다 */
	pcie->pcie_xrst = devm_reset_control_get_exclusive(dev, "pcie_x");
	/* [한국어] 실패를 확인한다 */
	if (IS_ERR(pcie->pcie_xrst))
		/* [한국어] 획득 실패 */
		return PTR_ERR(pcie->pcie_xrst);

	return 0;
}

/* [한국어]
 * tegra_pcie_phys_get_legacy - 컨트롤러 전체에 하나뿐인 PHY 를 얻는다 (구형 바인딩)
 *
 * @pcie: 컨트롤러 상태. phy 와 legacy_phy 를 채운다.
 * @return: 0 성공, 음수 오류.
 *
 * 구형 장치 트리 바인딩 경로다. 레인마다 PHY 를 두지 않고 컨트롤러에
 * 하나만 두던 시절의 구성이다.
 *
 * optional 로 얻으므로 **PHY 가 아예 없어도 성공한다.** 그 경우 pcie->phy 가
 * NULL 로 남고, 나중에 tegra_pcie_phy_power_on 이 그것을 보고
 * tegra_pcie_phy_enable 로 PADS 레지스터를 직접 만지는 경로를 탄다.
 * 즉 이 함수의 세 갈래 결과(PHY 있음 / PHY 없음 / 오류)가 이후 PHY 처리
 * 경로를 정한다.
 *
 * phy_init 을 여기서 부르는 데 주의한다. 초기화는 probe 때 한 번이면 되고,
 * 전원 인가만 절전마다 오르내린다. phy_exit 은 tegra_pcie_phys_put 이 한다.
 *
 * legacy_phy 를 true 로 표시해 두는 것이 이 함수의 부수 효과이자 목적이다.
 * 이후 모든 PHY 관련 분기가 이 플래그를 본다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_phys_get → [이 함수] → devm_phy_optional_get, phy_init
 */
static int tegra_pcie_phys_get_legacy(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] **optional 이라 PHY 가 아예 없어도 성공한다.** 그 경우 NULL 이 남고,
	 * tegra_pcie_phy_power_on 이 그것을 보고 PADS 레지스터를 직접 만지는
	 * 세 번째 경로를 탄다 */
	pcie->phy = devm_phy_optional_get(dev, "pcie");
	/* [한국어] 오류 포인터를 걸러 낸다 */
	if (IS_ERR(pcie->phy)) {
		/* [한국어] 오류 코드를 꺼낸다 */
		err = PTR_ERR(pcie->phy);
		/* [한국어] 획득 실패. optional 이므로 없어서가 아니라 다른 이유다 */
		dev_err(dev, "failed to get PHY: %d\n", err);
		return err;
	}

	/* [한국어] 초기화한다. PHY 가 NULL 이어도 API 가 안전하게 처리한다 */
	err = phy_init(pcie->phy);
	/* [한국어] 실패를 확인한다 */
	if (err < 0) {
		/* [한국어] PHY 초기화 실패 */
		dev_err(dev, "failed to initialize PHY: %d\n", err);
		return err;
	}

	/* [한국어] **구형 바인딩임을 표시한다.** 이후 모든 PHY 관련 분기가 이 플래그를 본다 */
	pcie->legacy_phy = true;

	return 0;
}

/* [한국어]
 * devm_of_phy_optional_get_index - "이름-번호" 형식으로 PHY 를 얻는다
 *
 * @dev:      devm 자원의 주인이 될 device.
 * @np:       PHY 를 참조하는 장치 트리 노드(여기서는 포트 노드).
 * @consumer: 이름의 앞부분. 호출자가 "pcie" 를 준다.
 * @index:    이름의 뒷번호.
 * @return: PHY 포인터, 없으면 NULL, 오류면 ERR_PTR.
 *
 * 커널 공통 API 에 "이름에 번호를 붙여 찾는" 판이 없어서 만든 도우미다.
 * "pcie-0", "pcie-1" 같은 이름을 만들어 표준 함수에 넘긴다.
 *
 * kasprintf 로 문자열을 만들고 곧바로 kfree 하는 구조에 주의 --
 * devm_of_phy_optional_get 이 이름을 조회에만 쓰고 보관하지 않으므로
 * 호출이 끝나면 버려도 된다.
 *
 * 이름이 이 파일 안에서만 쓰이는 static 함수인데도 devm_ 접두사를 달고
 * 있다. 커널 공통 API 처럼 보이지만 아니므로, 다른 파일에서 찾으면
 * 없다는 점에 주의한다.
 *
 * 실행 컨텍스트: probe 경로. kasprintf 가 GFP_KERNEL 로 할당하므로 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_pcie_port_get_phys → [이 함수] → devm_of_phy_optional_get
 */
static struct phy *devm_of_phy_optional_get_index(struct device *dev,
						  struct device_node *np,
						  const char *consumer,
						  unsigned int index)
{
	struct phy *phy;
	/* [한국어] 조회에만 쓰고 곧 버릴 문자열 */
	char *name;

	/* [한국어] "이름-번호" 형식의 문자열을 만든다 */
	name = kasprintf(GFP_KERNEL, "%s-%u", consumer, index);
	/* [한국어] 할당 실패 */
	if (!name)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 만든 이름으로 표준 함수를 부른다 */
	phy = devm_of_phy_optional_get(dev, np, name);
	kfree(name);

	return phy;
}

/* [한국어]
 * tegra_pcie_port_get_phys - 포트의 레인별 PHY 를 모두 얻는다 (신형 바인딩)
 *
 * @port: 대상 포트. phys 배열을 채운다.
 * @return: 0 성공, 음수 오류.
 *
 * 신형 바인딩 경로다. 레인마다 PHY 가 하나씩 있으므로 포트의 레인 수만큼
 * 배열을 잡고 "pcie-0" 부터 차례로 얻는다.
 *
 * optional 판을 쓰지만 오류는 그대로 올린다 -- 즉 PHY 가 없으면 NULL 이
 * 배열에 들어가고, PHY 프레임워크가 NULL 을 무시하므로 이후 동작이
 * 깨지지 않는다.
 *
 * 여기서도 phy_init 을 함께 부른다. tegra_pcie_phys_get_legacy 와 같은
 * 이유이며, 전원 인가는 tegra_pcie_port_phy_power_on 이 따로 한다.
 *
 * **실패해도 이미 얻은 PHY 를 되돌리지 않는다.** 다만 배열과 PHY 핸들이
 * 모두 devm 이라 probe 가 실패하면 커널이 정리한다. 초기화한 PHY 의
 * phy_exit 은 호출되지 않은 채 남는데, 상위인 tegra_pcie_get_resources 의
 * phys_put 라벨이 tegra_pcie_phys_put 을 부르므로 결과적으로 정리된다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_phys_get → [이 함수]
 *     → devm_kcalloc, devm_of_phy_optional_get_index, phy_init
 */
static int tegra_pcie_port_get_phys(struct tegra_pcie_port *port)
{
	struct device *dev = port->pcie->dev;
	/* [한국어] 이번에 얻을 PHY 임시 변수 */
	struct phy *phy;
	/* [한국어] 순회 첨자 */
	unsigned int i;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 레인 수만큼 배열을 잡는다. sizeof(phy) 는 포인터 크기이므로
	 * sizeof(*port->phys) 와 같다 */
	port->phys = devm_kcalloc(dev, port->lanes, sizeof(phy), GFP_KERNEL);
	/* [한국어] 할당 실패 */
	if (!port->phys)
		return -ENOMEM;

	/* [한국어] 레인 수만큼 순회한다 */
	for (i = 0; i < port->lanes; i++) {
		/* [한국어] "pcie-0", "pcie-1" 같은 이름으로 레인별 PHY 를 얻는다 */
		phy = devm_of_phy_optional_get_index(dev, port->np, "pcie", i);
		/* [한국어] optional 이지만 오류 포인터는 걸러 낸다 */
		if (IS_ERR(phy)) {
			/* [한국어] PHY 획득 실패 */
			dev_err(dev, "failed to get PHY#%u: %ld\n", i,
				PTR_ERR(phy));
			return PTR_ERR(phy);
		}

		/* [한국어] **초기화는 여기서 한 번만 한다.** 전원 인가는
		 * tegra_pcie_port_phy_power_on 이 절전마다 따로 한다 */
		err = phy_init(phy);
		/* [한국어] 실패를 확인한다 */
		if (err < 0) {
			/* [한국어] PHY 초기화 실패 */
			dev_err(dev, "failed to initialize PHY#%u: %d\n", i,
				err);
			return err;
		}

		/* [한국어] 배열에 보관한다 */
		port->phys[i] = phy;
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_phys_get - 구형/신형 PHY 바인딩을 가려 PHY 를 얻는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * 이 파일의 PHY 처리가 두 갈래로 갈리는 분기점이다.
 *
 * 구형으로 판단하는 조건이 둘이고 OR 로 묶여 있다.
 *   soc->has_gen2 가 아니면 : Gen2 를 지원하지 않는 옛 SoC(Tegra20/30)는
 *     신형 바인딩이 나오기 전 세대이므로 무조건 구형이다.
 *   장치 트리에 "phys" 속성이 있으면 : 신형 SoC 라도 장치 트리가 구형
 *     방식으로 작성되어 있으면 그것을 따른다. 후방 호환을 위해서다.
 *
 * **두 번째 조건이 직관과 반대인 데 주의한다.** "phys 속성이 있으면 신형"
 * 이 아니라 "있으면 구형" 이다. 신형 바인딩은 PHY 를 컨트롤러 노드가 아니라
 * 각 포트 자식 노드에 두기 때문이며, 그래서 컨트롤러 노드에 phys 가
 * 있다는 것은 곧 구형 배치라는 뜻이 된다.
 *
 * 신형이면 포트를 순회하며 각각의 레인별 PHY 를 얻는다.
 *
 * 실행 컨텍스트: probe 경로. tegra_pcie_parse_dt 가 포트 리스트를 이미
 * 만들어 둔 뒤에 불려야 한다.
 *
 * 호출 체인:
 *   tegra_pcie_get_resources → [이 함수]
 *     → tegra_pcie_phys_get_legacy 또는 tegra_pcie_port_get_phys
 */
static int tegra_pcie_phys_get(struct tegra_pcie *pcie)
{
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 컨트롤러의 장치 트리 노드 */
	struct device_node *np = pcie->dev->of_node;
	/* [한국어] 순회할 포트 */
	struct tegra_pcie_port *port;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] **두 번째 조건이 직관과 반대인 데 주의한다.** "phys 속성이 있으면 신형"
	 * 이 아니라 "있으면 구형" 이다. 신형 바인딩은 PHY 를 컨트롤러 노드가
	 * 아니라 각 포트 자식 노드에 두기 때문이며, 그래서 컨트롤러 노드에
	 * phys 가 있다는 것은 곧 구형 배치라는 뜻이다.
	 * 첫 번째 조건은 Gen2 미지원 SoC(Tegra20/30)가 신형 바인딩이 나오기 전
	 * 세대라는 사정을 반영한다 */
	if (!soc->has_gen2 || of_property_present(np, "phys"))
		/* [한국어] 구형 경로로 넘어간다 */
		return tegra_pcie_phys_get_legacy(pcie);

	/* [한국어] 모든 포트를 순회한다 */
	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] 포트마다 레인별 PHY 를 얻는다 */
		err = tegra_pcie_port_get_phys(port);
		/* [한국어] 하나라도 실패하면 그대로 반환한다 */
		if (err < 0)
			return err;
	}

	return 0;
}

/* [한국어]
 * tegra_pcie_phys_put - PHY 초기화를 되돌린다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음. 실패해도 로그만 찍는다.
 *
 * tegra_pcie_phys_get 이 부른 phy_init 들의 짝인 phy_exit 을 부른다.
 * 구형/신형 갈래가 그대로 유지된다.
 *
 * **실패해도 계속 진행하는 것** 이 켜는 쪽과 다르다. 신형 경로의 이중
 * 루프에서 phy_exit 이 실패해도 오류를 찍고 다음 PHY 로 넘어간다.
 * 정리 경로에서는 하나가 실패했다고 나머지를 포기하면 자원이 더 많이
 * 새기 때문이다.
 *
 * 핸들 자체는 devm 이라 놓지 않는다. 이 함수가 하는 것은 초기화 해제뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 오류 경로 또는 드라이버 해제).
 *
 * 호출 체인:
 *   tegra_pcie_get_resources(오류) / tegra_pcie_put_resources → [이 함수]
 *     → phy_exit
 */
static void tegra_pcie_phys_put(struct tegra_pcie *pcie)
{
	struct tegra_pcie_port *port;
	/* [한국어] 로그용 device */
	struct device *dev = pcie->dev;
	/* [한국어] 오류 코드와 순회 첨자 */
	int err, i;

	/* [한국어] 구형 바인딩이면 여기서 끝난다 */
	if (pcie->legacy_phy) {
		/* [한국어] 컨트롤러 단위 PHY 하나만 되돌린다 */
		err = phy_exit(pcie->phy);
		/* [한국어] 실패를 확인한다 */
		if (err < 0)
			/* [한국어] 구형 경로의 해제 실패도 로그만 찍는다 */
			dev_err(dev, "failed to teardown PHY: %d\n", err);
		return;
	}

	/* [한국어] 모든 포트를 순회한다 */
	list_for_each_entry(port, &pcie->ports, list) {
		/* [한국어] 포트의 레인 수만큼 순회한다 */
		for (i = 0; i < port->lanes; i++) {
			/* [한국어] 레인별 PHY 의 초기화를 되돌린다 */
			err = phy_exit(port->phys[i]);
			/* [한국어] 실패를 확인하되 멈추지 않는다 */
			if (err < 0)
				/* [한국어] 해제 실패. **오류를 찍고 계속 진행한다** -- 하나가 실패했다고 나머지를
				 * 포기하면 자원이 더 많이 새기 때문이다 */
				dev_err(dev, "failed to teardown PHY#%u: %d\n",
					i, err);
		}
	}
}

/* [한국어]
 * tegra_pcie_get_resources - 하드웨어 자원을 모두 확보한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * probe 에서 tegra_pcie_parse_dt 다음으로 불린다. 하드웨어를 건드리기
 * 전에 필요한 것을 전부 손에 넣는 단계다.
 *
 * 순서대로 클록, 리셋, PHY(program_uphy 인 SoC 만), 세 레지스터 창,
 * 그리고 오류 인터럽트를 얻는다.
 *
 * 레지스터 창 셋을 모두 **이름으로** 찾는다("pads", "afi", "cs").
 * 번호가 아니라 이름을 쓰는 것은 장치 트리에 창이 여럿이기 때문이다.
 *
 * config 창(cs) 처리가 특이하다. platform_get_resource_byname 으로
 * 리소스를 얻은 뒤 **크기를 4KiB 로 줄여서** 매핑한다. 원문 주석이
 * "나중에 필요할 때 매핑" 이라 적었지만 실제로는 여기서 바로 매핑하며,
 * 줄이는 이유는 tegra_pcie_map_bus 가 창을 옮겨 가며 쓰기 때문이다 --
 * 전체를 매핑하면 256MiB 가 필요하다는 그 사정이다.
 *
 * program_uphy 검사에 주의: Tegra186 만 이 값이 false 다. 그 SoC 는 PHY 를
 * 이 드라이버가 다루지 않으므로 PHY 획득도 건너뛴다. 정리 경로의
 * phys_put 라벨에도 같은 검사가 있어 짝이 맞는다.
 *
 * 인터럽트는 devm 판이 아닌 request_irq 로 요청한다. 그래서
 * tegra_pcie_put_resources 가 free_irq 로 직접 놓아야 한다. 다른 자원이
 * 모두 devm 인 것과 대비된다.
 *
 * IRQF_SHARED 로 요청하는 이유는 INTx 가 같은 선을 탈 수 있기 때문이다
 * (tegra_pcie_map_irq 참조).
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_probe → [이 함수]
 *     → tegra_pcie_clocks_get, tegra_pcie_resets_get, tegra_pcie_phys_get,
 *       devm_platform_ioremap_resource_byname, request_irq
 */
static int tegra_pcie_get_resources(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 플랫폼 장치로 되돌린다. 리소스와 IRQ 를 얻으려면 이 타입이 필요하다 */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] config 창 리소스를 임시로 받을 곳 */
	struct resource *res;
	/* [한국어] program_uphy 를 보기 위해 SoC 기술자가 필요하다 */
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] **아직 아무 자원도 잡지 않았으므로** 실패해도 되돌릴 것이 없다 */
	err = tegra_pcie_clocks_get(pcie);
	/* [한국어] 클록을 못 얻으면 진행할 수 없다 */
	if (err) {
		/* [한국어] 클록 획득 실패는 -EPROBE_DEFER 일 수 있다 */
		dev_err(dev, "failed to get clocks: %d\n", err);
		return err;
	}

	/* [한국어] 리셋 셋을 얻는다 */
	err = tegra_pcie_resets_get(pcie);
	if (err) {
		/* [한국어] 리셋 획득 실패 */
		dev_err(dev, "failed to get resets: %d\n", err);
		return err;
	}

	/* [한국어] **Tegra186 만 이 값이 false 다.** 그 SoC 는 PHY 를 이 드라이버가
	 * 다루지 않으므로 획득 자체를 건너뛴다 */
	if (soc->program_uphy) {
		/* [한국어] 구형/신형 갈래를 가려 PHY 를 얻는다 */
		err = tegra_pcie_phys_get(pcie);
		/* [한국어] 실패하면 그대로 반환한다 */
		if (err < 0) {
			/* [한국어] PHY 획득 실패는 -EPROBE_DEFER 일 수 있다 */
			dev_err(dev, "failed to get PHYs: %d\n", err);
			return err;
		}
	}

	/* [한국어] PADS 블록을 매핑한다. 창을 **이름으로** 찾는 것은 장치 트리에 창이
	 * 여럿이기 때문이다 */
	pcie->pads = devm_platform_ioremap_resource_byname(pdev, "pads");
	/* [한국어] 매핑 실패를 확인한다 */
	if (IS_ERR(pcie->pads)) {
		/* [한국어] 매핑 실패 코드를 보관한다 */
		err = PTR_ERR(pcie->pads);
		goto phys_put;
	}

	/* [한국어] AFI 블록을 매핑한다. **이 파일에서 가장 많이 쓰이는 블록** 이다 */
	pcie->afi = devm_platform_ioremap_resource_byname(pdev, "afi");
	/* [한국어] 매핑 실패를 확인한다 */
	if (IS_ERR(pcie->afi)) {
		/* [한국어] 매핑 실패 코드를 보관한다 */
		err = PTR_ERR(pcie->afi);
		goto phys_put;
	}

	/* request configuration space, but remap later, on demand */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cs");
	/* [한국어] config 창이 없으면 진행할 수 없다 */
	if (!res) {
		err = -EADDRNOTAVAIL;
		goto phys_put;
	}

	/* [한국어] **리소스를 값으로 복사한다.** 원본을 줄이면 플랫폼 장치의 리소스
	 * 목록이 훼손되므로, 사본을 만들어 그것만 줄인다 */
	pcie->cs = *res;

	/* constrain configuration space to 4 KiB */
	resource_set_size(&pcie->cs, SZ_4K);

	/* [한국어] 줄인 사본으로 매핑한다. 위의 원문 주석은 "나중에 필요할 때 매핑"
	 * 이라 적었지만 실제로는 여기서 바로 한다 */
	pcie->cfg = devm_ioremap_resource(dev, &pcie->cs);
	/* [한국어] 매핑 실패를 확인한다 */
	if (IS_ERR(pcie->cfg)) {
		/* [한국어] 매핑 실패 코드를 보관한다 */
		err = PTR_ERR(pcie->cfg);
		goto phys_put;
	}

	/* request interrupt */
	err = platform_get_irq_byname(pdev, "intr");
	/* [한국어] IRQ 를 못 얻으면 PHY 를 되돌린다 */
	if (err < 0)
		goto phys_put;

	/* [한국어] IRQ 번호를 보관한다 */
	pcie->irq = err;

	/* [한국어] **IRQF_SHARED 로 요청한다.** INTx 가 같은 선을 탈 수 있기 때문이며,
	 * tegra_pcie_isr 이 AFI_INTR_LEGACY 코드에 IRQ_NONE 을 돌려주는 이유가
	 * 이것이다. devm 판이 아니라 tegra_pcie_put_resources 가 직접 놓아야 한다 */
	err = request_irq(pcie->irq, tegra_pcie_isr, IRQF_SHARED, "PCIE", pcie);
	/* [한국어] 실패하면 PHY 를 되돌린다 */
	if (err) {
		/* [한국어] IRQ 등록 실패 */
		dev_err(dev, "failed to register IRQ: %d\n", err);
		goto phys_put;
	}

	return 0;

phys_put:
	if (soc->program_uphy)
		tegra_pcie_phys_put(pcie);

	return err;
}

/* [한국어]
 * tegra_pcie_put_resources - devm 이 정리하지 못하는 자원을 놓는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 항상 0. 호출자가 반환값을 쓰지 않는다.
 *
 * tegra_pcie_get_resources 가 얻은 것 중 **devm 이 아닌 둘** 만 놓는다.
 *   인터럽트 : request_irq 로 얻었으므로 free_irq 가 필요하다.
 *   PHY 초기화 : phy_init 의 짝인 phy_exit 이 필요하다.
 *
 * 레지스터 매핑, 클록, 리셋은 모두 devm 이라 커널이 알아서 놓는다.
 * 그래서 이 함수가 짧다.
 *
 * irq > 0 검사는 방어적이다. platform_get_irq_byname 이 실패하면 probe 가
 * 여기까지 오지 않으므로 실제로는 항상 참이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 오류 경로).
 *
 * 호출 체인:
 *   tegra_pcie_probe(오류) → [이 함수] → free_irq, tegra_pcie_phys_put
 */
static int tegra_pcie_put_resources(struct tegra_pcie *pcie)
{
	const struct tegra_pcie_soc *soc = pcie->soc;

	/* [한국어] 방어적 검사다. IRQ 획득이 실패하면 probe 가 여기까지 오지 않으므로
	 * 실제로는 항상 참이다 */
	if (pcie->irq > 0)
		/* [한국어] request_irq 로 얻었으므로 직접 놓아야 한다 */
		free_irq(pcie->irq, pcie);

	/* [한국어] PHY 를 이 드라이버가 다루는 SoC 에서만 초기화를 되돌린다 */
	if (soc->program_uphy)
		tegra_pcie_phys_put(pcie);

	return 0;
}

/* [한국어]
 * tegra_pcie_pme_turnoff - 포트에 PME_Turn_Off 를 보내고 응답을 기다린다
 *
 * @port: 대상 포트.
 * @return: 없음. 응답이 없어도 오류를 찍고 진행한다.
 *
 * 절전에 들어가기 전에 링크를 규격에 맞게 내리는 절차다. 전원을 그냥
 * 끊으면 상대 장치가 링크 단절을 오류로 볼 수 있으므로, PME_Turn_Off
 * 메시지를 보내 양쪽이 합의한 상태로 만든다.
 *
 *   1) AFI_PCIE_PME 에서 이 포트의 turnoff 비트를 세운다. 그러면 하드웨어가
 *      메시지를 내보낸다.
 *   2) 같은 레지스터의 ack 비트가 설 때까지 1us 간격으로 10ms 까지 폴링한다.
 *   3) 10~11ms 더 기다린다. 상대가 실제로 링크를 내릴 시간을 주는 것으로
 *      보이나, 이 값의 근거는 코드에 없다.
 *   4) turnoff 비트를 도로 지운다. 다음 절전 때 다시 세워야 하므로,
 *      세운 채로 두면 에지가 만들어지지 않는다.
 *
 * **비트 위치가 포트마다, SoC 마다 다르다.** soc->ports[index].pme 에서
 * turnoff_bit 과 ack_bit 을 가져오는데, Tegra20 은 (0,5)와 (8,10),
 * Tegra30 은 거기에 (16,18)이 더해지고, Tegra186 은 셋째가 (12,14)로
 * 다르다. 그래서 표를 두고 참조한다.
 *
 * 응답이 없어도 진행하는 이유: 상대 장치가 이미 없거나 응답하지 않는
 * 상황에서 절전 자체를 막을 이유가 없기 때문이다. 뒷세대
 * pcie-tegra194.c 의 tegra_pcie_dw_pme_turnoff 는 여기서 한 걸음 더 나아가
 * L2 진입에 실패하면 PERST 를 걸어 강제로 detect 상태로 보낸다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(절전 경로). 2)와 3)에서 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_pm_suspend → [이 함수] → afi_readl, afi_writel, readl_poll_timeout
 */
static void tegra_pcie_pme_turnoff(struct tegra_pcie_port *port)
{
	struct tegra_pcie *pcie = port->pcie;
	/* [한국어] PME 비트 표를 얻기 위해 SoC 기술자가 필요하다 */
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] 오류 코드 보관용 */
	int err;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 val;
	/* [한국어] 응답 비트 위치를 담을 변수 */
	u8 ack_bit;

	/* [한국어] 현재 값을 읽는다 */
	val = afi_readl(pcie, AFI_PCIE_PME);
	/* [한국어] **이 포트의 turnoff 비트를 세운다.** 비트 위치가 SoC 와 포트마다 달라
	 * 표를 참조한다 */
	val |= (0x1 << soc->ports[port->index].pme.turnoff_bit);
	/* [한국어] 요청을 내보낸다. 하드웨어가 PME_Turn_Off 메시지를 보낸다 */
	afi_writel(pcie, val, AFI_PCIE_PME);

	/* [한국어] 응답 비트 위치를 표에서 가져온다 */
	ack_bit = soc->ports[port->index].pme.ack_bit;
	/* [한국어] ack 비트가 서기를 1us 간격으로 10ms 까지 폴링한다 */
	err = readl_poll_timeout(pcie->afi + AFI_PCIE_PME, val,
				 val & (0x1 << ack_bit), 1, PME_ACK_TIMEOUT);
	/* [한국어] 시간 안에 응답이 없었다 */
	if (err)
		/* [한국어] 응답이 없어도 오류만 찍고 진행한다 -- 상대가 이미 없거나 응답하지
		 * 않는 상황에서 절전 자체를 막을 이유가 없기 때문이다 */
		dev_err(pcie->dev, "PME Ack is not received on port: %d\n",
			port->index);

	/* [한국어] 10~11ms 더 기다린다. 상대가 실제로 링크를 내릴 시간을 주는 것으로
	 * 보이나, 이 값의 근거는 코드에 없다 */
	usleep_range(10000, 11000);

	/* [한국어] 현재 값을 다시 읽는다 */
	val = afi_readl(pcie, AFI_PCIE_PME);
	/* [한국어] **turnoff 비트를 도로 지운다.** 세운 채로 두면 다음 절전 때 에지가
	 * 만들어지지 않는다 */
	val &= ~(0x1 << soc->ports[port->index].pme.turnoff_bit);
	afi_writel(pcie, val, AFI_PCIE_PME);
}

/* [한국어]
 * tegra_pcie_msi_irq - MSI 체인 핸들러. 대기 벡터를 모두 처리한다
 *
 * @desc: MSI 인터럽트의 irq_desc. handler_data 에 struct tegra_pcie 가 있다.
 * @return: 없음.
 *
 * MSI 전용 인터럽트 선 하나에 붙어, 그것을 벡터 256개로 나눠 준다.
 * chained_irq_enter / _exit 로 감싸는 것은 상위 컨트롤러의 ack 를 대신
 * 처리하기 위해서다 -- 이 짝이 없으면 다음 인터럽트가 올라오지 않는다.
 *
 * 상태 레지스터가 32비트 8개(AFI_MSI_VEC(0) 부터 (7)까지)라 바깥 루프가
 * 8번 돌고, 각 레지스터 안에서 선 비트를 모두 훑는다.
 *
 * 안쪽 while 루프가 **매 바퀴 레지스터를 다시 읽는다.** 처리 도중 새 MSI 가
 * 도착해 다른 비트가 설 수 있기 때문이며, 상태가 0 이 되어야 빠져나온다.
 * 비트를 지우는 것은 각 벡터의 irq_ack(tegra_msi_irq_ack)이고, 그 흐름
 * 함수는 tegra_msi_domain_alloc 이 지정한 handle_edge_irq 가 부른다.
 *
 * 전역 벡터 번호는 i * 32 + offset 으로 만든다. i 가 레지스터 번호,
 * offset 이 그 안의 비트 위치다.
 *
 * **미처리 벡터의 정리 경로에 주목할 점이 있다.**
 * generic_handle_domain_irq 가 실패하면(그 벡터에 매핑된 핸들러가 없으면)
 * 직접 상태 비트를 지우는데, 그때 쓰는 레지스터가
 * AFI_MSI_VEC(index) -- 즉 **전역 벡터 번호** 를 레지스터 번호 자리에
 * 넣는다. 다른 모든 곳은 레지스터 번호를 쓴다: 이 함수의 읽기 두 곳이
 * AFI_MSI_VEC(i) 를 쓰고, tegra_msi_irq_ack 은 hwirq / 32 를 계산해 쓴다.
 * AFI_MSI_VEC(x) 가 0x6c + x*4 이므로, i 가 1 이상인 레지스터에서 이 경로를
 * 타면 계산되는 오프셋이 AFI_MSI_VEC(i) 와 달라진다(예: i=1, offset=0 이면
 * index=32 라 0xec 가 되고, AFI_MSI_VEC(1) 은 0x70 이다). 쓰는 비트는
 * index % 32 로 올바르다. 코드는 손대지 않고 이 산술적 사실만 적어 둔다.
 * 이 경로는 "unexpected MSI" 로그와 함께 도는 예외 경로다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   MSI 전용 선 → 상위 컨트롤러 → [이 함수]
 *     → generic_handle_domain_irq → handle_edge_irq → tegra_msi_irq_ack
 */
static void tegra_pcie_msi_irq(struct irq_desc *desc)
{
	struct tegra_pcie *pcie = irq_desc_get_handler_data(desc);
	/* [한국어] 상위 인터럽트 컨트롤러의 irq_chip. chained_irq_enter/exit 에 넘긴다 */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] MSI 상태 */
	struct tegra_msi *msi = &pcie->msi;
	/* [한국어] 로그용 device */
	struct device *dev = pcie->dev;
	/* [한국어] 순회 첨자 */
	unsigned int i;

	/* [한국어] 상위 컨트롤러에 대한 ack 를 대신 처리한다. 이 짝(enter/exit)이 없으면
	 * 다음 인터럽트가 올라오지 않는다 */
	chained_irq_enter(chip, desc);

	/* [한국어] **상태 레지스터가 32비트 8개** 라 여덟 번 돈다. 8 을 상수로 직접 쓰는데,
	 * INT_PCI_MSI_NR / 32 로도 같은 값이 나온다 */
	for (i = 0; i < 8; i++) {
		/* [한국어] 이 레지스터의 대기 상태를 읽는다 */
		unsigned long reg = afi_readl(pcie, AFI_MSI_VEC(i));

		/* [한국어] **상태가 0 이 될 때까지 돈다.** 처리 도중 새 MSI 가 도착해 다른 비트가
		 * 설 수 있기 때문이며, irq_ack 가 처리한 비트를 지우므로 루프가 끝난다 */
		while (reg) {
			/* [한국어] 선 비트 중 가장 낮은 자리를 찾는다 */
			unsigned int offset = find_first_bit(&reg, 32);
			/* [한국어] 전역 벡터 번호를 만든다. i 가 레지스터 번호, offset 이 그 안의 위치다 */
			unsigned int index = i * 32 + offset;
			/* [한국어] 반환값 보관용 */
			int ret;

			/* [한국어] 벡터를 도메인에 넘긴다. 여기서 handle_edge_irq 가 돌고,
			 * 그것이 irq_ack 로 상태 비트를 지운 뒤 핸들러를 부른다 */
			ret = generic_handle_domain_irq(msi->domain, index);
			if (ret) {
				/*
				 * that's weird who triggered this?
				 * just clear it
				 */
				dev_info(dev, "unexpected MSI\n");
				/* [한국어] **미처리 벡터의 상태 비트를 직접 지운다.**
				 * 여기 쓰는 레지스터 인자가 index -- 전역 벡터 번호다. 이 파일의 다른
				 * 모든 곳은 레지스터 번호를 쓴다(이 함수의 읽기 두 곳이 AFI_MSI_VEC(i),
				 * tegra_msi_irq_ack 은 hwirq / 32). AFI_MSI_VEC(x) 가 0x6c + x*4 이므로
				 * i 가 1 이상인 레지스터에서 이 경로를 타면 계산되는 오프셋이
				 * AFI_MSI_VEC(i) 와 달라진다(i=1, offset=0 이면 index=32 라 0xec 가 되고
				 * AFI_MSI_VEC(1) 은 0x70 이다). 쓰는 비트는 index % 32 로 올바르다.
				 * 코드는 손대지 않고 이 산술적 사실만 적어 둔다 */
				afi_writel(pcie, BIT(index % 32), AFI_MSI_VEC(index));
			}

			/* see if there's any more pending in this vector */
			reg = afi_readl(pcie, AFI_MSI_VEC(i));
		}
	}

	chained_irq_exit(chip, desc);
}

/* [한국어]
 * tegra_msi_irq_ack - 처리한 MSI 벡터의 대기 비트를 지운다
 *
 * @d: 이 벡터의 irq_data. chip_data 에 struct tegra_msi 가 들어 있다.
 * @return: 없음.
 *
 * 전역 벡터 번호(hwirq, 0~255)를 두 조각으로 나눈다 -- 32로 나눈 몫이
 * 레지스터 번호, 나머지가 그 안의 비트 위치다.
 *
 * 상태 레지스터는 **1을 쓰면 지워진다.** 읽지 않고 BIT 만 쓰는 데서 알 수
 * 있으며, 그래서 이 함수만 락이 없다. 마스크/언마스크는 읽고-고쳐-쓰기라
 * mask_lock 을 쥔다.
 *
 * handle_edge_irq 가 핸들러를 부르기 **전에** 이 함수를 불러 준다. MSI 는
 * 본질적으로 에지이므로, 미리 지워야 처리 중에 도착한 같은 벡터가 새
 * 인터럽트로 잡힌다. 지우지 않으면 tegra_pcie_msi_irq 의 while 루프가
 * 같은 비트를 계속 보고 무한히 돈다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_msi_irq → generic_handle_domain_irq → handle_edge_irq
 *     → [이 함수] → msi_to_pcie, afi_writel
 */
static void tegra_msi_irq_ack(struct irq_data *d)
{
	struct tegra_msi *msi = irq_data_get_irq_chip_data(d);
	/* [한국어] 컨트롤러 상태를 되찾는다 */
	struct tegra_pcie *pcie = msi_to_pcie(msi);
	/* [한국어] 레지스터 번호를 구한다 */
	unsigned int index = d->hwirq / 32;

	/* clear the interrupt */
	afi_writel(pcie, BIT(d->hwirq % 32), AFI_MSI_VEC(index));
}

/* [한국어]
 * tegra_msi_irq_mask - MSI 벡터 하나를 끈다
 *
 * @d: 이 벡터의 irq_data.
 * @return: 없음.
 *
 * 활성화 레지스터(AFI_MSI_EN_VEC)에서 해당 비트를 내린다. 상태
 * 레지스터와는 별개의 레지스터 배열이다.
 *
 * **읽고-고쳐-쓰기라 락이 필요하다.** 한 레지스터를 벡터 32개가 공유하므로,
 * 두 CPU 가 각기 다른 벡터를 동시에 마스크하면 한쪽의 변경이 사라진다.
 *
 * scoped_guard 를 쓰는 것에 주의 -- 블록을 벗어날 때 자동으로 락이 풀리는
 * 관용구다. 명시적인 unlock 호출이 없는 이유이며, linux/cleanup.h 를
 * include 하는 이유이기도 하다.
 *
 * raw 스핀락에 irqsave 판을 쓰는 것은 이 콜백이 인터럽트 컨텍스트에서도
 * 불릴 수 있고 PREEMPT_RT 에서도 잠들면 안 되기 때문이다. 비트맵을 지키는
 * map_lock(뮤텍스)과는 다른 락이다 -- 그쪽은 할당 경로에서만 쓰여 잠들어도
 * 되기 때문에 종류가 다르다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수도, 프로세스 컨텍스트일 수도 있다.
 *
 * 호출 체인:
 *   disable_irq 계열 / MSI 코어의 마스크 흐름 → [이 함수]
 */
static void tegra_msi_irq_mask(struct irq_data *d)
{
	struct tegra_msi *msi = irq_data_get_irq_chip_data(d);
	/* [한국어] 컨트롤러 상태를 되찾는다 */
	struct tegra_pcie *pcie = msi_to_pcie(msi);
	/* [한국어] 레지스터 번호를 구한다 */
	unsigned int index = d->hwirq / 32;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	/* [한국어] **읽고-고쳐-쓰기라 락이 필요하다.** 한 레지스터를 벡터 32개가
	 * 공유하므로, 락이 없으면 한쪽의 변경이 사라진다 */
	scoped_guard(raw_spinlock_irqsave, &msi->mask_lock) {
		/* [한국어] 현재 값을 읽는다 */
		value = afi_readl(pcie, AFI_MSI_EN_VEC(index));
		/* [한국어] 해당 벡터 비트를 내린다. 나머지는 32로 나눈 나머지, 즉 레지스터 안의
		 * 자리다 */
		value &= ~BIT(d->hwirq % 32);
		/* [한국어] 되쓴다 */
		afi_writel(pcie, value, AFI_MSI_EN_VEC(index));
	}
}

/* [한국어]
 * tegra_msi_irq_unmask - MSI 벡터 하나를 켠다
 *
 * @d: 이 벡터의 irq_data.
 * @return: 없음.
 *
 * tegra_msi_irq_mask 의 반대로 활성화 비트를 세운다. 락을 쓰는 이유와
 * 방식은 그쪽과 같다.
 *
 * 이 함수가 불려야 비로소 그 벡터의 인터럽트가 전달된다. 벡터를 할당하는
 * 것(tegra_msi_domain_alloc)과 켜는 것은 별개이며, 장치 드라이버가
 * request_irq 를 마친 뒤 커널이 켜 준다.
 *
 * 이 레지스터의 상태는 절전을 넘어 살아남지 않는다. 그래서
 * tegra_pcie_enable_msi 가 복귀할 때 소프트웨어 비트맵(msi->used)으로부터
 * 활성화 상태를 재구성해 되쓴다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수도, 프로세스 컨텍스트일 수도 있다.
 *
 * 호출 체인:
 *   request_irq / enable_irq → MSI 코어 → [이 함수]
 */
static void tegra_msi_irq_unmask(struct irq_data *d)
{
	struct tegra_msi *msi = irq_data_get_irq_chip_data(d);
	/* [한국어] 레지스터 접근에 pcie->afi 가 필요하므로 바깥 구조체로 나온다 */
	struct tegra_pcie *pcie = msi_to_pcie(msi);
	/* [한국어] 전역 벡터 번호를 32로 나눠 레지스터 번호를 구한다 */
	unsigned int index = d->hwirq / 32;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	/* [한국어] 락을 블록 범위로 잡는다. 벗어날 때 자동으로 풀리므로 명시적
	 * unlock 이 없다 */
	scoped_guard(raw_spinlock_irqsave, &msi->mask_lock) {
		/* [한국어] 현재 값을 읽는다 */
		value = afi_readl(pcie, AFI_MSI_EN_VEC(index));
		/* [한국어] 해당 벡터 비트를 세운다 */
		value |= BIT(d->hwirq % 32);
		/* [한국어] 되쓴다 */
		afi_writel(pcie, value, AFI_MSI_EN_VEC(index));
	}
}

/* [한국어]
 * tegra_compose_msi_msg - 장치에게 알려 줄 MSI 주소와 데이터를 만든다
 *
 * @data: 이 벡터의 irq_data. chip_data 에 struct tegra_msi 가 있다.
 * @msg:  채워 줄 메시지. 커널이 이 값을 장치의 MSI 능력 레지스터에 쓴다.
 * @return: 없음.
 *
 * MSI 는 인터럽트 선이 아니라 메모리 쓰기다. 장치가 msg->address 에
 * msg->data 를 쓰면 컨트롤러가 그것을 인터럽트로 바꾼다.
 *
 * 주소는 msi->phys -- tegra_pcie_msi_setup 이 dma_alloc_attrs 로 잡아 둔
 * 페이지의 **DMA 주소** 다. 데이터는 전역 벡터 번호(0~255)를 그대로 넣는다.
 *
 * **주소가 벡터마다 같고 데이터만 다른 것** 이 이 하드웨어의 구조다.
 * pcie-mediatek-gen3.c 가 세트마다 다른 주소를 쓰는 것과 대비된다.
 * 그래서 여기서는 hwirq 를 나눌 필요 없이 통째로 데이터에 담는다.
 *
 * 그 페이지에 실제로 값이 쓰이지는 않는다 -- DMA_ATTR_NO_KERNEL_MAPPING 으로
 * 잡은 데서 알 수 있듯 커널이 내용을 읽지 않고, 컨트롤러가 그 주소로 가는
 * 쓰기를 가로채 인터럽트로 바꾼다. 페이지는 그 주소를 다른 용도로 쓰이지
 * 않게 예약해 두는 의미다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(벡터 할당 경로). 인터럽트 핸들러가 아니다.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors 계열 → MSI 코어
 *     → tegra_msi_bottom_chip.irq_compose_msi_msg → [이 함수]
 */
static void tegra_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct tegra_msi *msi = irq_data_get_irq_chip_data(data);

	/* [한국어] 수신 주소 하위 32비트. tegra_pcie_msi_setup 이 잡아 둔 페이지의
	 * DMA 주소다 */
	msg->address_lo = lower_32_bits(msi->phys);
	/* [한국어] 수신 주소 상위 32비트. DMA 마스크를 32비트로 제한했으므로 0 이 된다 */
	msg->address_hi = upper_32_bits(msi->phys);
	/* [한국어] **데이터에 전역 벡터 번호를 그대로 넣는다.** 주소가 모든 벡터에서
	 * 같으므로 데이터만으로 구분해야 한다. 세트마다 다른 주소를 쓰는
	 * pcie-mediatek-gen3.c 와 대비되는 구조다 */
	msg->data = data->hwirq;
}

/* [한국어] MSI 벡터 하나에 붙는 irq_chip. ack 가 있는 것은 MSI 가 에지라
 * 핸들러 전에 상태를 지워야 하기 때문이다 */
static struct irq_chip tegra_msi_bottom_chip = {
	/* [한국어] /proc/interrupts 등에 나타날 이름 */
	.name			= "Tegra MSI",
	.irq_ack		= tegra_msi_irq_ack,
	.irq_mask		= tegra_msi_irq_mask,
	.irq_unmask		= tegra_msi_irq_unmask,
	.irq_compose_msi_msg	= tegra_compose_msi_msg,
};

/* [한국어]
 * tegra_msi_domain_alloc - MSI 벡터를 연속으로 떼어 준다
 *
 * @domain:  MSI 바닥 도메인. host_data 가 struct tegra_msi 다.
 * @virq:    커널이 이미 잡아 둔 가상 IRQ 번호의 시작값.
 * @nr_irqs: 요청 개수. MSI-X 는 보통 1, 다중 MSI 는 2의 거듭제곱이다.
 * @args:    상위 도메인이 넘기는 정보. 이 드라이버는 쓰지 않는다.
 * @return: 0 성공, 빈 벡터가 없으면 -ENOSPC.
 *
 * 256개 벡터 비트맵에서 빈 자리를 찾아 표시하고, 각 가상 IRQ 에 irq_chip 과
 * chip_data 를 연결한다.
 *
 * bitmap_find_free_region 을 쓰는 이유는 **다중 MSI** 때문이다. 다중 MSI 는
 * 벡터가 연속이어야 하고 시작 번호가 개수에 정렬되어야 한다 -- 장치가
 * 기준 데이터 값에 번호를 더해 쓰기 때문이다. 그 두 제약을 한 번에
 * 만족시키는 것이 이 함수이며, order_base_2(nr_irqs) 로 차수를 넘긴다.
 *
 * chip_data 로 domain->host_data, 즉 struct tegra_msi 를 심는다. 이후
 * irq_chip 콜백들이 그것을 꺼내 msi_to_pcie 로 컨트롤러까지 거슬러 올라간다.
 *
 * handle_edge_irq 를 흐름 함수로 지정한다. MSI 는 본질적으로 에지이며,
 * 그 판이 핸들러 전에 irq_ack 를 불러 준다.
 *
 * cpuidle 알림을 여기서도 하는 데 주의 -- tegra_pcie_map_irq 가 INTx 에
 * 대해 하는 것과 같은 조치다. MSI 를 쓰기 시작해도 깊은 절전으로 들어가면
 * 안 되기 때문으로 보인다.
 *
 * 락: 비트맵은 뮤텍스로 지킨다. 이 경로는 프로세스 컨텍스트에서만 불리고
 * 잠들어도 되기 때문이다. 비트맵 조작 직후 곧바로 풀며, 이어지는
 * irq_domain_set_info 는 락 밖에서 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors 계열 → MSI 코어 → irq_domain_alloc_irqs
 *     → [이 함수] → bitmap_find_free_region, irq_domain_set_info
 */
static int tegra_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	struct tegra_msi *msi = domain->host_data;
	/* [한국어] 순회 첨자 */
	unsigned int i;
	/* [한국어] 찾은 전역 벡터 번호. **int 라 음수(-ENOSPC)를 담을 수 있다** */
	int hwirq;

	mutex_lock(&msi->map_lock);

	/* [한국어] **연속되고 정렬된** 빈 영역을 찾아 표시한다. 다중 MSI 는 벡터가
	 * 연속이어야 하고 시작 번호가 개수에 정렬되어야 하는데,
	 * 이 함수가 두 제약을 한 번에 만족시킨다 */
	hwirq = bitmap_find_free_region(msi->used, INT_PCI_MSI_NR, order_base_2(nr_irqs));

	mutex_unlock(&msi->map_lock);

	/* [한국어] 빈 자리가 없으면 -ENOSPC. 벡터 256개를 다 쓴 경우다 */
	if (hwirq < 0)
		return -ENOSPC;

	/* [한국어] 요청 개수만큼 순회한다 */
	for (i = 0; i < nr_irqs; i++)
		/* [한국어] 각 가상 IRQ 에 irq_chip 과 chip_data 를 심는다.
		 * **chip_data 로 domain->host_data 즉 struct tegra_msi 를 넣는다** --
		 * 이후 콜백들이 그것을 꺼내 msi_to_pcie 로 컨트롤러까지 거슬러 올라간다.
		 * handle_edge_irq 는 MSI 가 에지이기 때문이며, 그 판이 핸들러 전에
		 * irq_ack 를 불러 준다 */
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &tegra_msi_bottom_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL);

	tegra_cpuidle_pcie_irqs_in_use();

	return 0;
}

/* [한국어]
 * tegra_msi_domain_free - MSI 벡터를 비트맵에 돌려준다
 *
 * @domain:  MSI 바닥 도메인.
 * @virq:    반납할 가상 IRQ 시작 번호.
 * @nr_irqs: 반납 개수.
 * @return: 없음.
 *
 * tegra_msi_domain_alloc 의 반대다. virq 로 irq_data 를 찾아 그 안의
 * hwirq 를 얻고, 그 자리를 비트맵에서 푼다.
 *
 * hwirq 를 인자로 받지 않고 irq_data 에서 꺼내는 이유는, 커널의 free 콜백
 * 규약이 가상 IRQ 번호만 넘기기 때문이다.
 *
 * **irq_domain_free_irqs_common 을 부르지 않는다.** pcie-mediatek-gen3.c 의
 * 같은 자리(mtk_msi_bottom_domain_free)는 그것을 부르는데 여기는 없다.
 * 매핑 정리를 상위 계층에 맡기는 것으로 보이나, 그 차이의 이유는 코드에
 * 적혀 있지 않다.
 *
 * 락: alloc 과 같은 뮤텍스다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_free_irq_vectors 계열 → MSI 코어 → [이 함수] → bitmap_release_region
 */
static void tegra_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	/* [한국어] 도메인의 host_data 에서 MSI 상태를 꺼낸다 */
	struct tegra_msi *msi = domain->host_data;

	mutex_lock(&msi->map_lock);

	/* [한국어] 비트맵에서 자리를 푼다. alloc 때와 같은 차수를 넘겨야 짝이 맞는다 */
	bitmap_release_region(msi->used, d->hwirq, order_base_2(nr_irqs));

	mutex_unlock(&msi->map_lock);
}

/* [한국어] MSI 바닥 도메인의 동작 정의 */
static const struct irq_domain_ops tegra_msi_domain_ops = {
	/* [한국어] alloc/free 만 있고 map 이 없다. MSI 벡터는 장치가 요청할 때 동적으로
	 * 떼어 주기 때문이다 */
	.alloc = tegra_msi_domain_alloc,
	.free = tegra_msi_domain_free,
};

/* [한국어] MSI 부모 도메인의 동작 정의. init_dev_msi_info 가
 * msi_lib_init_dev_msi_info 로 되어 있어 공통 구현을 그대로 쓴다 */
static const struct msi_parent_ops tegra_msi_parent_ops = {
	/* [한국어] MSI-X 를 지원한다 */
	.supported_flags	= (MSI_GENERIC_FLAGS_MASK	|
				   MSI_FLAG_PCI_MSIX),
	.required_flags		= (MSI_FLAG_USE_DEF_DOM_OPS	|
				   /* [한국어] 기본 도메인/칩 동작과 부모 마스크를 쓴다 */
				   MSI_FLAG_USE_DEF_CHIP_OPS	|
				   /* [한국어] 다중 MSI 지원을 뜻하는 플래그가 **없다** -- pcie-mediatek-gen3.c 가
				    * MSI_FLAG_MULTI_PCI_MSI 를 넣는 것과 대비된다.
				    * 다만 tegra_msi_domain_alloc 은 order_base_2(nr_irqs) 로 연속 할당을
				    * 지원하도록 짜여 있다 */
				   MSI_FLAG_PCI_MSI_MASK_PARENT	|
				   MSI_FLAG_NO_AFFINITY),
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어]
 * tegra_allocate_domains - MSI 바닥 도메인을 만든다
 *
 * @msi: MSI 상태. domain 필드를 채운다.
 * @return: 0 성공, 실패하면 -ENOMEM.
 *
 * msi_create_parent_irq_domain 으로 바닥 도메인만 만든다. 그 위의 장치별
 * 도메인은 커널이 tegra_msi_parent_ops 를 보고 붙여 주므로 드라이버가
 * 직접 만들지 않는다.
 *
 * fwnode 로 컨트롤러 자신의 것을 쓴다. INTx 처럼 장치 트리 자식 노드를
 * 쓰지 않는 이유는, MSI 가 장치 트리로 참조되는 대상이 아니기 때문이다.
 *
 * host_data 에 msi 를 넣어 두면 tegra_msi_domain_alloc 이 그것을 꺼내
 * chip_data 로 심는다.
 *
 * 크기는 INT_PCI_MSI_NR -- 8 * 32 = 256 이다. 상태 레지스터 8개 × 32비트와
 * 정확히 맞는다.
 *
 * **뒷세대와의 대비**: pcie-tegra194.c 에는 이 함수에 해당하는 것이 없다.
 * DWC 코어가 iMSI-RX 기반으로 도메인을 만들고, 그 드라이버는
 * pp->num_vectors 만 지정한다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_msi_setup → [이 함수] → msi_create_parent_irq_domain
 */
static int tegra_allocate_domains(struct tegra_msi *msi)
{
	struct tegra_pcie *pcie = msi_to_pcie(msi);
	/* [한국어] 이 컨트롤러의 fwnode */
	struct fwnode_handle *fwnode = dev_fwnode(pcie->dev);
	/* [한국어] 도메인 정보를 채운다. host_data 에 msi 를 넣어 두면 alloc 콜백이
	 * 그것을 꺼내 chip_data 로 심는다 */
	struct irq_domain_info info = {
		/* [한국어] **컨트롤러 자신의 fwnode 를 쓴다.** MSI 는 장치 트리로 참조되는
		 * 대상이 아니기 때문이다 */
		.fwnode		= fwnode,
		.ops		= &tegra_msi_domain_ops,
		.size		= INT_PCI_MSI_NR,
		.host_data	= msi,
	};

	/* [한국어] **바닥 도메인만 만든다.** 그 위의 장치별 도메인은 커널이
	 * tegra_msi_parent_ops 를 보고 붙여 준다 */
	msi->domain = msi_create_parent_irq_domain(&info, &tegra_msi_parent_ops);
	/* [한국어] 실패를 확인한다 */
	if (!msi->domain) {
		/* [한국어] 도메인 생성 실패 */
		dev_err(pcie->dev, "failed to create MSI domain\n");
		return -ENOMEM;
	}
	return 0;
}

/* [한국어]
 * tegra_free_domains - MSI 바닥 도메인을 지운다
 *
 * @msi: MSI 상태.
 * @return: 없음.
 *
 * tegra_allocate_domains 의 짝이다. 한 줄짜리 래퍼인데도 따로 함수를 둔
 * 것은, 부르는 곳이 셋이고(msi_setup 의 오류 경로, msi_teardown)
 * 모두 IS_ENABLED(CONFIG_PCI_MSI) 검사 안에 있어 대칭을 맞추기 위해서로
 * 보인다.
 *
 * NULL 검사가 없다. domain 이 NULL 이면 irq_domain_remove 가 어떻게
 * 동작하는지는 이 파일에서 알 수 없으나, 부르는 세 곳이 모두 도메인 생성
 * 성공 이후 경로라 실제로는 NULL 이 오지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   tegra_pcie_msi_setup(오류) / tegra_pcie_msi_teardown → [이 함수]
 */
static void tegra_free_domains(struct tegra_msi *msi)
{
	irq_domain_remove(msi->domain);
}

/* [한국어]
 * tegra_pcie_msi_setup - MSI 도메인과 수신 페이지를 준비한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 0 성공, 음수 오류.
 *
 * MSI 를 쓸 수 있게 만드는 소프트웨어 쪽 준비를 모두 한다. 하드웨어
 * 레지스터는 건드리지 않는다 -- 그것은 tegra_pcie_enable_msi 가 한다.
 *
 *   1) 락 둘을 초기화한다. 종류가 다른 이유는 각 함수 설명에 있다.
 *   2) 도메인을 만든다(CONFIG_PCI_MSI 일 때만).
 *   3) MSI 전용 인터럽트 번호를 얻고 체인 핸들러를 건다. 오류 인터럽트와
 *      **별개의 선** 이라는 점에 주의 -- 이름이 "msi" 로 따로 있다.
 *   4) DMA 마스크를 32비트로 제한한다. 위의 원문 주석이 이유를 밝히는데,
 *      컨트롤러 자체는 32비트를 넘는 주소를 쓸 수 있지만 32비트 MSI 목표
 *      주소만 지원하는 엔드포인트를 위해 일부러 낮춘다. 즉 하드웨어 능력이
 *      아니라 **호환성을 위한 제한** 이다.
 *   5) MSI 수신 페이지를 잡는다. DMA_ATTR_NO_KERNEL_MAPPING 을 쓰는 것은
 *      커널이 그 내용을 읽을 일이 없기 때문이다 -- 장치가 그 주소로 보내는
 *      쓰기를 컨트롤러가 가로채 인터럽트로 바꾸므로, 실제로 메모리에
 *      닿지 않는다. 페이지를 잡는 것은 그 주소를 예약해 두는 의미다.
 *
 * 정리 경로가 두 라벨로 층져 있다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_probe → [이 함수]
 *     → tegra_allocate_domains, platform_get_irq_byname,
 *       irq_set_chained_handler_and_data, dma_alloc_attrs
 */
static int tegra_pcie_msi_setup(struct tegra_pcie *pcie)
{
	struct platform_device *pdev = to_platform_device(pcie->dev);
	/* [한국어] MSI 상태. pcie 안에 값으로 박혀 있어 주소를 취한다 */
	struct tegra_msi *msi = &pcie->msi;
	/* [한국어] 로그와 DMA 할당의 기준 */
	struct device *dev = pcie->dev;
	/* [한국어] 오류 코드 보관용 */
	int err;

	mutex_init(&msi->map_lock);
	raw_spin_lock_init(&msi->mask_lock);

	/* [한국어] MSI 를 쓰지 않는 커널 설정에서는 도메인을 만들지 않는다 */
	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		/* [한국어] MSI 바닥 도메인을 만든다 */
		err = tegra_allocate_domains(msi);
		/* [한국어] 도메인 생성 실패면 그대로 반환한다 */
		if (err)
			return err;
	}

	/* [한국어] **MSI 전용 인터럽트 선을 얻는다.** AFI 오류 인터럽트("intr")와
	 * 별개의 이름("msi")이다 */
	err = platform_get_irq_byname(pdev, "msi");
	/* [한국어] IRQ 를 못 얻으면 도메인을 되돌린다 */
	if (err < 0)
		goto free_irq_domain;

	/* [한국어] MSI 전용 IRQ 번호를 보관한다 */
	msi->irq = err;

	/* [한국어] 체인 핸들러를 건다. **도메인이 준비된 뒤여야 한다** */
	irq_set_chained_handler_and_data(msi->irq, tegra_pcie_msi_irq, pcie);

	/* Though the PCIe controller can address >32-bit address space, to
	 * facilitate endpoints that support only 32-bit MSI target address,
	 * the mask is set to 32-bit to make sure that MSI target address is
	 * always a 32-bit address
	 */
	err = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	/* [한국어] 실패하면 체인 핸들러를 떼고 도메인을 지운다 */
	if (err < 0) {
		/* [한국어] DMA 마스크 설정 실패 */
		dev_err(dev, "failed to set DMA coherent mask: %d\n", err);
		goto free_irq;
	}

	/* [한국어] **MSI 수신 페이지를 잡는다.** DMA_ATTR_NO_KERNEL_MAPPING 을 쓰는 것은
	 * 커널이 내용을 읽을 일이 없기 때문이다 -- 장치가 이 주소로 보내는
	 * 쓰기를 컨트롤러가 가로채 인터럽트로 바꾸므로 실제로 메모리에 닿지 않는다 */
	msi->virt = dma_alloc_attrs(dev, PAGE_SIZE, &msi->phys, GFP_KERNEL,
				    DMA_ATTR_NO_KERNEL_MAPPING);
	/* [한국어] 할당 실패를 확인한다 */
	if (!msi->virt) {
		/* [한국어] 수신 페이지 할당 실패 */
		dev_err(dev, "failed to allocate DMA memory for MSI\n");
		err = -ENOMEM;
		goto free_irq;
	}

	return 0;

free_irq:
	irq_set_chained_handler_and_data(msi->irq, NULL, NULL);
free_irq_domain:
	if (IS_ENABLED(CONFIG_PCI_MSI))
		tegra_free_domains(msi);

	return err;
}

/* [한국어]
 * tegra_pcie_enable_msi - MSI 하드웨어를 켜고 마스크 상태를 복원한다
 *
 * @pcie: 컨트롤러 상태. msi.phys 가 이미 정해져 있어야 한다.
 * @return: 없음.
 *
 * tegra_pcie_msi_setup 이 소프트웨어 쪽을 마쳐 두면, 이 함수가 하드웨어
 * 레지스터에 그 결과를 반영한다.
 *
 *   1) MSI 수신 주소를 FPCI 쪽과 AXI 쪽 양쪽에 알려 준다. FPCI 쪽만
 *      soc->msi_base_shift 만큼 오른쪽으로 미는데, 그 값이 Tegra20 은 0,
 *      나머지는 8 이다. 즉 세대에 따라 이 레지스터가 주소를 담는 축척이
 *      다르며, 그 축척의 근거 문서는 이 트리에 없다.
 *   2) 창 크기를 1 로 쓴다. 원문 주석대로 이 레지스터는 4KiB 단위이므로
 *      4KiB 한 페이지라는 뜻이고, dma_alloc_attrs 로 PAGE_SIZE 만큼 잡은
 *      것과 맞아떨어진다.
 *   3) **마스크 상태를 소프트웨어 비트맵으로부터 재구성한다.** 이 대목이
 *      핵심이다. 절전에서 깨어나면 활성화 레지스터가 초기화되어 있는데,
 *      어느 벡터가 켜져 있었는지는 msi->used 비트맵에 남아 있다.
 *      bitmap_to_arr32 로 그것을 32비트 배열로 바꿔 여덟 레지스터에 그대로
 *      쓴다. 다른 드라이버들이 별도의 saved_irq_state 필드를 두는 것과
 *      달리, 여기서는 할당 비트맵이 그 역할을 겸한다.
 *      다만 그 결과 **할당되었으나 마스크된 벡터도 켜진 상태로 복원된다** --
 *      비트맵은 할당 여부만 알고 마스크 여부는 모르기 때문이다.
 *   4) 컨트롤러 인터럽트 마스크에서 MSI 비트를 연다.
 *      tegra_pcie_enable_controller 가 INT 비트만 켜 두고 MSI 는 미뤄
 *      두었던 것을 여기서 마저 켠다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(전원 인가 경로).
 *
 * 호출 체인:
 *   tegra_pcie_pm_resume → [이 함수] → afi_writel, bitmap_to_arr32
 */
static void tegra_pcie_enable_msi(struct tegra_pcie *pcie)
{
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] MSI 상태 */
	struct tegra_msi *msi = &pcie->msi;
	/* [한국어] 마스크 값과 비트맵을 옮겨 담을 배열. 크기가 256/32 = 8 로
	 * 레지스터 개수와 맞는다 */
	u32 reg, msi_state[INT_PCI_MSI_NR / 32];
	/* [한국어] 순회 첨자 */
	int i;

	/* [한국어] FPCI 쪽에는 **시프트해서** 쓴다. 그 값이 Tegra20 은 0, 나머지는 8 이며,
	 * 세대에 따라 이 레지스터가 주소를 담는 축척이 다르기 때문이다.
	 * 바로 아래의 AXI 쪽은 시프트 없이 그대로 쓰는 것과 대비된다 */
	afi_writel(pcie, msi->phys >> soc->msi_base_shift, AFI_MSI_FPCI_BAR_ST);
	afi_writel(pcie, msi->phys, AFI_MSI_AXI_BAR_ST);
	/* this register is in 4K increments */
	afi_writel(pcie, 1, AFI_MSI_BAR_SZ);

	/* Restore the MSI allocation state */
	bitmap_to_arr32(msi_state, msi->used, INT_PCI_MSI_NR);
	/* [한국어] 배열의 각 원소가 레지스터 하나에 대응한다 */
	for (i = 0; i < ARRAY_SIZE(msi_state); i++)
		/* [한국어] 여덟 레지스터에 그대로 쓴다 */
		afi_writel(pcie, msi_state[i], AFI_MSI_EN_VEC(i));

	/* and unmask the MSI interrupt */
	reg = afi_readl(pcie, AFI_INTR_MASK);
	/* [한국어] MSI 비트를 연다. tegra_pcie_enable_controller 가 INT 비트만 켜 두고
	 * MSI 는 미뤄 두었던 것을 여기서 마저 켠다 */
	reg |= AFI_INTR_MASK_MSI_MASK;
	afi_writel(pcie, reg, AFI_INTR_MASK);
}

/* [한국어]
 * tegra_pcie_msi_teardown - MSI 자원을 모두 놓는다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음.
 *
 * tegra_pcie_msi_setup 의 역순이다.
 *
 *   1) MSI 수신 페이지를 놓는다.
 *   2) **남아 있는 매핑을 모두 강제로 푼다.** 벡터 256개를 순회하며
 *      irq_find_mapping 으로 살아 있는 것을 찾아 해제한다. 정상적으로는
 *      장치 드라이버들이 이미 반납했겠지만, probe 실패 등으로 중간에
 *      끊긴 경우를 대비한 정리다.
 *   3) 체인 핸들러를 뗀다.
 *   4) 도메인을 지운다.
 *
 * 순서가 중요하다 -- 매핑을 먼저 풀고 도메인을 지워야 한다. 반대로 하면
 * 사라진 도메인을 참조하게 된다. 마찬가지로 핸들러를 도메인보다 먼저 떼어야
 * 인터럽트가 죽은 도메인으로 들어가지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 오류 경로).
 *
 * 호출 체인:
 *   tegra_pcie_probe(오류) → [이 함수]
 *     → dma_free_attrs, irq_find_mapping, irq_domain_free_irqs,
 *       tegra_free_domains
 */
static void tegra_pcie_msi_teardown(struct tegra_pcie *pcie)
{
	struct tegra_msi *msi = &pcie->msi;
	/* [한국어] 순회 첨자와 찾은 IRQ 번호 */
	unsigned int i, irq;

	/* [한국어] MSI 수신 페이지를 놓는다. 잡을 때와 같은 속성을 넘겨야 한다 */
	dma_free_attrs(pcie->dev, PAGE_SIZE, msi->virt, msi->phys,
		       DMA_ATTR_NO_KERNEL_MAPPING);

	/* [한국어] **벡터 256개를 모두 훑는다.** 정상적으로는 장치 드라이버들이 이미
	 * 반납했겠지만, probe 실패 등으로 중간에 끊긴 경우를 대비한 정리다 */
	for (i = 0; i < INT_PCI_MSI_NR; i++) {
		/* [한국어] 이 벡터에 매핑이 남아 있는지 본다 */
		irq = irq_find_mapping(msi->domain, i);
		/* [한국어] 매핑이 있으면 유효한 IRQ 번호가 온다 */
		if (irq > 0)
			/* [한국어] 살아 있는 매핑을 강제로 푼다 */
			irq_domain_free_irqs(irq, 1);
	}

	/* [한국어] 체인 핸들러를 뗀다. 도메인을 지우기 전에 해야 인터럽트가
	 * 죽은 도메인으로 들어가지 않는다 */
	irq_set_chained_handler_and_data(msi->irq, NULL, NULL);

	/* [한국어] 도메인을 지운다. **매핑을 모두 푼 뒤여야 한다** */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		tegra_free_domains(msi);
}

/* [한국어]
 * tegra_pcie_disable_msi - MSI 인터럽트를 마스크한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 항상 0. 호출자가 반환값을 쓰지 않는다.
 *
 * 컨트롤러 인터럽트 마스크에서 MSI 비트만 내린다. tegra_pcie_enable_msi 의
 * 마지막 단계에 대응하며, 그 앞의 주소 설정이나 벡터별 활성화는 건드리지
 * 않는다 -- 어차피 전원이 끊기면 사라지고, 복원은 enable 쪽이 비트맵으로부터
 * 재구성하기 때문이다.
 *
 * 도메인이나 비트맵은 그대로 둔다. 절전에서 깨어나면 같은 벡터를 계속
 * 써야 하므로, 여기서 지우면 안 된다. 그것을 지우는 것은
 * tegra_pcie_msi_teardown 이며 드라이버가 아예 떨어질 때만 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(절전 경로).
 *
 * 호출 체인:
 *   tegra_pcie_pm_suspend → [이 함수] (CONFIG_PCI_MSI 일 때만)
 */
static int tegra_pcie_disable_msi(struct tegra_pcie *pcie)
{
	u32 value;

	/* mask the MSI interrupt */
	value = afi_readl(pcie, AFI_INTR_MASK);
	/* [한국어] MSI 비트만 내린다. AFI 오류 비트는 건드리지 않는다 --
	 * 그것은 tegra_pcie_disable_interrupts 가 따로 처리한다 */
	value &= ~AFI_INTR_MASK_MSI_MASK;
	/* [한국어] 되쓴다 */
	afi_writel(pcie, value, AFI_INTR_MASK);

	return 0;
}

/* [한국어]
 * tegra_pcie_disable_interrupts - AFI 오류 인터럽트를 마스크한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음.
 *
 * 컨트롤러 인터럽트 마스크에서 INT 비트를 내린다. tegra_pcie_disable_msi 와
 * 같은 레지스터의 다른 비트를 다루는 짝이다.
 *
 * **왜 필요한지가 호출자 쪽에 적혀 있다.** tegra_pcie_pm_suspend 위의
 * 원문 주석이 밝히듯, AFI 인터럽트는 tegra_pcie_enable_controller 에서
 * 켜지는데 pex_rst 를 어서트하면 AFI 가 원치 않는 인터럽트를 올리기
 * 때문에 그 전에 막아야 한다.
 *
 * 읽고-고쳐-쓰기지만 락이 없다. 절전 경로에서만 불려 경쟁 상대가 없기
 * 때문이다. 같은 레지스터를 만지는 tegra_msi_irq_mask 계열은 다른
 * 레지스터(AFI_MSI_EN_VEC)를 다루므로 겹치지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(절전 경로).
 *
 * 호출 체인:
 *   tegra_pcie_pm_suspend → [이 함수] → afi_readl, afi_writel
 */
static void tegra_pcie_disable_interrupts(struct tegra_pcie *pcie)
{
	u32 value;

	/* [한국어] 현재 마스크를 읽는다 */
	value = afi_readl(pcie, AFI_INTR_MASK);
	/* [한국어] AFI 오류 인터럽트 비트를 내린다. 락이 없는데, 절전 경로에서만
	 * 불려 경쟁 상대가 없기 때문이다 */
	value &= ~AFI_INTR_MASK_INT_MASK;
	afi_writel(pcie, value, AFI_INTR_MASK);
}

/* [한국어]
 * tegra_pcie_get_xbar_config - 레인 배분 조합을 레지스터 값으로 바꾼다
 *
 * @pcie:  컨트롤러 상태. 로그와 장치 트리 노드를 얻는 데만 쓴다.
 * @lanes: 포트별 레인 수를 8비트씩 이어 붙인 값. tegra_pcie_parse_dt 가
 *   lanes |= value << (index << 3) 으로 만든다. 즉 포트 0 이 최하위 바이트다.
 * @xbar:  결과를 담을 곳(출력).
 * @return: 0 성공. 알 수 없는 조합이면 -EINVAL.
 *
 * **앞 세대 고유의 개념** 이다. 이 컨트롤러는 루트 포트 여럿이 레인 풀
 * 하나를 나눠 쓰므로, 어느 포트에 몇 레인을 줄지를 하드웨어에 알려야 한다.
 * 뒷세대 pcie-tegra194.c 에는 이 개념이 없다 -- 컨트롤러 인스턴스마다
 * 별도의 장치 노드를 갖기 때문이다.
 *
 * SoC 마다 지원하는 조합이 달라 compatible 로 먼저 갈린 뒤 lanes 값으로
 * 다시 갈린다. 예컨대 0x010004 는 포트 0 이 4레인, 포트 2 가 1레인이라는
 * 뜻이다(바이트 단위로 04, 00, 01).
 *
 * **Tegra186 만 default 절이 있다.** 알 수 없는 조합이 와도 실패시키지
 * 않고 경고를 찍은 뒤 2x1,1x1,1x1 로 대체한다. 나머지 SoC 는 default 가
 * 없어 switch 를 빠져나가 함수 끝의 -EINVAL 에 도달하고, 그러면
 * tegra_pcie_parse_dt 가 probe 를 중단시킨다.
 *
 * XBAR_CONFIG 상수들의 값이 겹치는 데 주의한다. SINGLE, 420, X2_X1, 401 이
 * 모두 (0x0 << 20) 이고, DUAL, 222, X4_X1, 211 이 모두 (0x1 << 20) 이다.
 * 같은 레지스터 값에 SoC 별로 다른 이름을 붙인 것이라, 이름이 다르다고
 * 값이 다르지 않다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_parse_dt → [이 함수] → of_device_is_compatible
 */
static int tegra_pcie_get_xbar_config(struct tegra_pcie *pcie, u32 lanes,
				      u32 *xbar)
{
	struct device *dev = pcie->dev;
	/* [한국어] 컨트롤러의 장치 트리 노드 */
	struct device_node *np = dev->of_node;

	/* [한국어] **SoC 마다 지원하는 조합이 다르다.** compatible 로 먼저 갈린 뒤
	 * 레인 값으로 다시 갈린다 */
	if (of_device_is_compatible(np, "nvidia,tegra186-pcie")) {
		/* [한국어] Tegra186 의 조합 */
		switch (lanes) {
		/* [한국어] 4레인 + 1레인. 바이트 단위로 04, 00, 01 이므로
		 * 포트 0 이 4레인, 포트 1 이 0레인, 포트 2 가 1레인이다 */
		case 0x010004:
			dev_info(dev, "4x1, 1x1 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_401;
			return 0;

		/* [한국어] 2레인 + 1레인 + 1레인 */
		case 0x010102:
			dev_info(dev, "2x1, 1X1, 1x1 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_211;
			return 0;

		/* [한국어] 1레인 셋 */
		case 0x010101:
			dev_info(dev, "1x1, 1x1, 1x1 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_111;
			return 0;

		default:
			dev_info(dev, "wrong configuration updated in DT, "
				 /* [한국어] **Tegra186 만 default 절이 있다.** 알 수 없는 조합이 와도 실패시키지
				  * 않고 경고를 찍은 뒤 2x1,1x1,1x1 로 대체한다. 다른 SoC 는 default 가
				  * 없어 -EINVAL 로 probe 가 중단된다 */
				 "switching to default 2x1, 1x1, 1x1 "
				 "configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_211;
			return 0;
		}
	/* [한국어] Tegra124 와 Tegra210 은 조합이 같아 함께 처리한다 */
	} else if (of_device_is_compatible(np, "nvidia,tegra124-pcie") ||
		   of_device_is_compatible(np, "nvidia,tegra210-pcie")) {
		/* [한국어] Tegra124/210 의 조합. **값이 7자리 16진수로 적혀 있으나 앞의 0 은
		 * 무의미하다** -- 0x104 와 같다 */
		switch (lanes) {
		/* [한국어] 4레인 + 1레인 */
		case 0x0000104:
			dev_info(dev, "4x1, 1x1 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_X4_X1;
			return 0;

		/* [한국어] 2레인 + 1레인 */
		case 0x0000102:
			dev_info(dev, "2x1, 1x1 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_X2_X1;
			return 0;
		}
	/* [한국어] Tegra30 인지 확인한다 */
	} else if (of_device_is_compatible(np, "nvidia,tegra30-pcie")) {
		/* [한국어] Tegra30 의 조합. **포트가 셋이라 세 바이트를 본다** */
		switch (lanes) {
		/* [한국어] 4레인 + 2레인 */
		case 0x00000204:
			dev_info(dev, "4x1, 2x1 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_420;
			return 0;

		/* [한국어] 2레인 셋 */
		case 0x00020202:
			dev_info(dev, "2x3 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_222;
			return 0;

		/* [한국어] 4레인 + 1레인 + 1레인 */
		case 0x00010104:
			dev_info(dev, "4x1, 1x2 configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_411;
			return 0;
		}
	/* [한국어] Tegra20 인지 확인한다 */
	} else if (of_device_is_compatible(np, "nvidia,tegra20-pcie")) {
		/* [한국어] Tegra20 의 조합 */
		switch (lanes) {
		/* [한국어] Tegra20 의 단일 모드 -- 4레인 하나 */
		case 0x00000004:
			dev_info(dev, "single-mode configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_SINGLE;
			return 0;

		/* [한국어] Tegra20 의 이중 모드 -- 2레인 둘 */
		case 0x00000202:
			dev_info(dev, "dual-mode configuration\n");
			*xbar = AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_DUAL;
			return 0;
		}
	}

	return -EINVAL;
}

/*
 * Check whether a given set of supplies is available in a device tree node.
 * This is used to check whether the new or the legacy device tree bindings
 * should be used.
 */
/* [한국어]
 * of_regulator_bulk_available - 요구하는 regulator 가 장치 트리에 다 있는지 본다
 *
 * @np:           검사할 장치 트리 노드.
 * @supplies:     확인할 regulator 이름 배열.
 * @num_supplies: 그 개수.
 * @return: 전부 있으면 true, 하나라도 없으면 false.
 *
 * 위의 원문 주석대로 **새 바인딩과 구형 바인딩 중 어느 쪽인지 판별하는**
 * 데 쓰인다. 실제로 regulator 를 얻지는 않고 속성 존재만 확인한다.
 *
 * 장치 트리 관례상 regulator 는 "<이름>-supply" 형태의 속성으로 참조되므로,
 * 이름 뒤에 "-supply" 를 붙여 그 속성이 있는지 본다. 버퍼가 32바이트라
 * 긴 이름은 잘릴 수 있으나, 이 파일에서 쓰는 가장 긴 이름
 * "vddio-pexctl-aud" 도 "-supply" 를 붙여 24바이트라 문제가 없다.
 *
 * **하나라도 없으면 곧바로 false** 다. 즉 새 바인딩은 전부 갖춰야 인정하며,
 * 부분적으로 갖춘 장치 트리는 구형으로 취급된다.
 *
 * 이름이 of_ 로 시작해 커널 공통 API 처럼 보이지만 이 파일 안의 static
 * 함수다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_get_regulators → [이 함수] → of_property_present
 */
static bool of_regulator_bulk_available(struct device_node *np,
					struct regulator_bulk_data *supplies,
					unsigned int num_supplies)
{
	char property[32];
	/* [한국어] 순회 첨자 */
	unsigned int i;

	/* [한국어] 목록의 모든 이름을 확인한다 */
	for (i = 0; i < num_supplies; i++) {
		/* [한국어] 장치 트리 관례대로 이름 뒤에 "-supply" 를 붙인다.
		 * 버퍼가 32바이트인데 이 파일의 가장 긴 이름도 24바이트라 잘리지 않는다 */
		snprintf(property, 32, "%s-supply", supplies[i].supply);

		/* [한국어] 하나라도 없으면 곧바로 false 다 -- 새 바인딩은 전부 갖춰야 인정한다 */
		if (!of_property_present(np, property))
			return false;
	}

	return true;
}

/*
 * Old versions of the device tree binding for this device used a set of power
 * supplies that didn't match the hardware inputs. This happened to work for a
 * number of cases but is not future proof. However to preserve backwards-
 * compatibility with old device trees, this function will try to use the old
 * set of supplies.
 */
/* [한국어]
 * tegra_pcie_get_legacy_regulators - 구형 바인딩의 regulator 목록을 얻는다
 *
 * @pcie: 컨트롤러 상태. supplies 와 num_supplies 를 채운다.
 * @return: 0 성공, 음수 오류.
 *
 * 위의 원문 주석이 배경을 밝힌다 -- 옛 장치 트리 바인딩이 하드웨어 입력과
 * 맞지 않는 regulator 집합을 썼는데, 그런 장치 트리와의 호환을 위해 남겨
 * 둔 경로다.
 *
 * Tegra20 은 2개("pex-clk", "vdd"), Tegra30 은 3개(거기에 "avdd" 추가)다.
 * 그 밖의 SoC 는 num_supplies 가 0 으로 남아 -ENODEV 로 실패한다 -- 즉
 * **구형 바인딩은 Tegra20/30 에서만 지원된다.**
 *
 * 배열을 채우는 방식에 주의: [0]과 [1]은 무조건 채우고 [2]만 조건부다.
 * num_supplies 가 2 이상임이 위의 검사로 보장되므로 안전하다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_get_regulators(대체 경로) → [이 함수]
 *     → devm_kcalloc, devm_regulator_bulk_get
 */
static int tegra_pcie_get_legacy_regulators(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 컨트롤러의 장치 트리 노드 */
	struct device_node *np = dev->of_node;

	/* [한국어] Tegra30 인지 확인한다 */
	if (of_device_is_compatible(np, "nvidia,tegra30-pcie"))
		/* [한국어] Tegra30 은 3개다 */
		pcie->num_supplies = 3;
	/* [한국어] Tegra20 인지 확인한다 */
	else if (of_device_is_compatible(np, "nvidia,tegra20-pcie"))
		/* [한국어] Tegra20 은 2개다 */
		pcie->num_supplies = 2;

	/* [한국어] 위 두 분기 어디에도 안 걸렸으면 0 으로 남는다 */
	if (pcie->num_supplies == 0) {
		/* [한국어] **구형 바인딩은 Tegra20/30 에서만 지원된다.** 그 밖의 SoC 가 여기
		 * 오면 지원할 방법이 없다 */
		dev_err(dev, "device %pOF not supported in legacy mode\n", np);
		return -ENODEV;
	}

	/* [한국어] 배열을 잡는다 */
	pcie->supplies = devm_kcalloc(dev, pcie->num_supplies,
				      sizeof(*pcie->supplies),
				      GFP_KERNEL);
	/* [한국어] 할당 실패 */
	if (!pcie->supplies)
		return -ENOMEM;

	/* [한국어] 두 SoC 공통의 첫 번째 */
	pcie->supplies[0].supply = "pex-clk";
	/* [한국어] 두 SoC 공통의 두 번째 */
	pcie->supplies[1].supply = "vdd";

	/* [한국어] 개수가 2보다 크면(즉 Tegra30 이면) 하나 더 넣는다 */
	if (pcie->num_supplies > 2)
		/* [한국어] Tegra30 만 세 번째를 갖는다 */
		pcie->supplies[2].supply = "avdd";

	/* [한국어] 구형 목록으로 regulator 를 얻는다 */
	return devm_regulator_bulk_get(dev, pcie->num_supplies, pcie->supplies);
}

/*
 * Obtains the list of regulators required for a particular generation of the
 * IP block.
 *
 * This would've been nice to do simply by providing static tables for use
 * with the regulator_bulk_*() API, but unfortunately Tegra30 is a bit quirky
 * in that it has two pairs or AVDD_PEX and VDD_PEX supplies (PEXA and PEXB)
 * and either seems to be optional depending on which ports are being used.
 */
/* [한국어]
 * tegra_pcie_get_regulators - SoC 에 맞는 regulator 목록을 얻는다
 *
 * @pcie:      컨트롤러 상태.
 * @lane_mask: 실제로 쓰이는 레인의 비트마스크. Tegra30 에서만 쓴다.
 * @return: 0 성공, 음수 오류.
 *
 * 위의 원문 주석이 왜 정적 표로 못 하는지를 밝힌다 -- Tegra30 이
 * AVDD_PEX 와 VDD_PEX 를 PEXA/PEXB 두 벌 갖고 있고, 어느 포트를 쓰느냐에
 * 따라 둘 중 하나가 없어도 되기 때문이다.
 *
 * SoC 별 목록:
 *   Tegra186 : 4개
 *   Tegra210 : 3개
 *   Tegra124 : 4개
 *   Tegra30  : 4개 + 조건부 최대 4개. lane_mask 의 하위 4비트가 서 있으면
 *     (레인 0~3 사용) PEXA 쌍을, 비트 4~5 가 서 있으면 (레인 4~5 사용)
 *     PEXB 쌍을 더한다. 원문 주석이 그 대응을 적고 있다.
 *   Tegra20  : 5개
 *
 * 그 밖의 compatible 이면 어느 분기에도 들어가지 않아 supplies 가 NULL,
 * num_supplies 가 0 인 채로 아래로 내려간다. 그러면
 * of_regulator_bulk_available 이 0개를 검사해 true 를 돌려주고
 * devm_regulator_bulk_get(dev, 0, NULL) 이 불린다. 다만
 * tegra_pcie_of_match 에 없는 compatible 로는 probe 가 불리지 않으므로
 * 실제로는 도달하지 않는다.
 *
 * **마지막의 대체 경로가 이 함수의 요점이다.** 위에서 만든 새 바인딩
 * 목록이 장치 트리에 다 있는지 확인해서, 없으면 방금 잡은 배열을 버리고
 * 구형 경로로 넘어간다. dev_info 로 그 사실을 알리므로 로그에서 어느
 * 바인딩을 썼는지 확인할 수 있다.
 *
 * Tegra30 과 Tegra20 만 구형 대체가 실제로 가능하다 -- 다른 SoC 는
 * tegra_pcie_get_legacy_regulators 가 -ENODEV 를 돌려준다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_parse_dt → [이 함수]
 *     → devm_kcalloc, of_regulator_bulk_available,
 *       devm_regulator_bulk_get 또는 tegra_pcie_get_legacy_regulators
 */
static int tegra_pcie_get_regulators(struct tegra_pcie *pcie, u32 lane_mask)
{
	struct device *dev = pcie->dev;
	/* [한국어] 컨트롤러의 장치 트리 노드 */
	struct device_node *np = dev->of_node;
	/* [한국어] **증가 첨자.** 이름을 순서대로 넣으면서 자동으로 자리를 옮긴다.
	 * 조건부 항목이 있는 Tegra30 에서 특히 유용하다 */
	unsigned int i = 0;

	/* [한국어] **SoC 마다 필요한 전원의 이름과 개수가 다르다.** compatible 로 갈라
	 * 각각의 목록을 만든다 */
	if (of_device_is_compatible(np, "nvidia,tegra186-pcie")) {
		/* [한국어] Tegra186 은 4개다 */
		pcie->num_supplies = 4;

		/* [한국어] 배열을 잡는다. **dev 대신 pcie->dev 를 쓴다** -- 같은 값이지만
		 * 아래 분기들과 표기가 다르다 */
		pcie->supplies = devm_kcalloc(pcie->dev, pcie->num_supplies,
					      sizeof(*pcie->supplies),
					      GFP_KERNEL);
		/* [한국어] 할당 실패 */
		if (!pcie->supplies)
			return -ENOMEM;

		/* [한국어] 디지털 전원 */
		pcie->supplies[i++].supply = "dvdd-pex";
		/* [한국어] PLL 용 고전압 전원 */
		pcie->supplies[i++].supply = "hvdd-pex-pll";
		/* [한국어] 고전압 전원 */
		pcie->supplies[i++].supply = "hvdd-pex";
		/* [한국어] 오디오와 공유하는 IO 제어 전원 */
		pcie->supplies[i++].supply = "vddio-pexctl-aud";
	/* [한국어] Tegra210 인지 확인한다 */
	} else if (of_device_is_compatible(np, "nvidia,tegra210-pcie")) {
		/* [한국어] Tegra210 은 3개다 */
		pcie->num_supplies = 3;

		/* [한국어] 배열을 잡는다 */
		pcie->supplies = devm_kcalloc(pcie->dev, pcie->num_supplies,
					      sizeof(*pcie->supplies),
					      GFP_KERNEL);
		/* [한국어] 할당 실패 */
		if (!pcie->supplies)
			return -ENOMEM;

		/* [한국어] 고전압 IO 전원 */
		pcie->supplies[i++].supply = "hvddio-pex";
		/* [한국어] 디지털 IO 전원 */
		pcie->supplies[i++].supply = "dvddio-pex";
		/* [한국어] IO 제어 전원 */
		pcie->supplies[i++].supply = "vddio-pex-ctl";
	/* [한국어] Tegra124 인지 확인한다 */
	} else if (of_device_is_compatible(np, "nvidia,tegra124-pcie")) {
		/* [한국어] Tegra124 는 4개다 */
		pcie->num_supplies = 4;

		/* [한국어] 배열을 잡는다 */
		pcie->supplies = devm_kcalloc(dev, pcie->num_supplies,
					      sizeof(*pcie->supplies),
					      GFP_KERNEL);
		/* [한국어] 할당 실패 */
		if (!pcie->supplies)
			return -ENOMEM;

		/* [한국어] 아날로그 IO 전원. **Tegra210 의 hvddio-pex 자리에 avddio-pex 가 온다** --
		 * 두 SoC 의 목록이 이름 하나만 다르다 */
		pcie->supplies[i++].supply = "avddio-pex";
		/* [한국어] 디지털 IO 전원 */
		pcie->supplies[i++].supply = "dvddio-pex";
		/* [한국어] 고전압 전원 */
		pcie->supplies[i++].supply = "hvdd-pex";
		/* [한국어] IO 제어 전원 */
		pcie->supplies[i++].supply = "vddio-pex-ctl";
	/* [한국어] Tegra30 인지 확인한다 */
	} else if (of_device_is_compatible(np, "nvidia,tegra30-pcie")) {
		/* [한국어] PEXA 와 PEXB 쌍이 각각 필요한지 표시할 변수 */
		bool need_pexa = false, need_pexb = false;

		/* VDD_PEXA and AVDD_PEXA supply lanes 0 to 3 */
		if (lane_mask & 0x0f)
			/* [한국어] 하위 4비트가 서 있으면 레인 0~3 을 쓴다는 뜻이다.
			 * 위의 원문 주석이 레인과 전원의 대응을 밝힌다 */
			need_pexa = true;

		/* VDD_PEXB and AVDD_PEXB supply lanes 4 to 5 */
		if (lane_mask & 0x30)
			/* [한국어] 비트 4~5 가 서 있으면 레인 4~5 를 쓴다는 뜻이다 */
			need_pexb = true;

		/* [한국어] **개수가 조건부로 정해진다.** 기본 4개에 필요한 쌍마다 2개씩 더한다 --
		 * 최대 8개다. 위의 원문 주석이 정적 표로 못 하는 이유로 든 것이 이것이다 */
		pcie->num_supplies = 4 + (need_pexa ? 2 : 0) +
					 (need_pexb ? 2 : 0);

		/* [한국어] 배열을 잡는다 */
		pcie->supplies = devm_kcalloc(dev, pcie->num_supplies,
					      sizeof(*pcie->supplies),
					      GFP_KERNEL);
		/* [한국어] 할당 실패 */
		if (!pcie->supplies)
			return -ENOMEM;

		/* [한국어] PLL 아날로그 전원 */
		pcie->supplies[i++].supply = "avdd-pex-pll";
		/* [한국어] 고전압 전원 */
		pcie->supplies[i++].supply = "hvdd-pex";
		/* [한국어] IO 제어 전원 */
		pcie->supplies[i++].supply = "vddio-pex-ctl";
		/* [한국어] PLLE 아날로그 전원 */
		pcie->supplies[i++].supply = "avdd-plle";

		/* [한국어] 레인 0~3 을 쓰면 PEXA 쌍을 더한다 */
		if (need_pexa) {
			/* [한국어] 레인 0~3 쪽 AVDD */
			pcie->supplies[i++].supply = "avdd-pexa";
			/* [한국어] 레인 0~3 쪽 VDD */
			pcie->supplies[i++].supply = "vdd-pexa";
		}

		/* [한국어] 레인 4~5 를 쓰면 PEXB 쌍을 더한다 */
		if (need_pexb) {
			/* [한국어] 레인 4~5 쪽 AVDD */
			pcie->supplies[i++].supply = "avdd-pexb";
			/* [한국어] 레인 4~5 쪽 VDD */
			pcie->supplies[i++].supply = "vdd-pexb";
		}
	/* [한국어] Tegra20 인지 확인한다 */
	} else if (of_device_is_compatible(np, "nvidia,tegra20-pcie")) {
		/* [한국어] Tegra20 은 5개다 */
		pcie->num_supplies = 5;

		/* [한국어] 배열을 잡는다 */
		pcie->supplies = devm_kcalloc(dev, pcie->num_supplies,
					      sizeof(*pcie->supplies),
					      GFP_KERNEL);
		/* [한국어] 할당 실패 */
		if (!pcie->supplies)
			return -ENOMEM;

		/* [한국어] Tegra20 은 5개. **이 분기만 i 를 쓰지 않고 첨자를 직접 쓴다** --
		 * 조건부 항목이 없어 개수가 고정이기 때문이다 */
		pcie->supplies[0].supply = "avdd-pex";
		/* [한국어] Tegra20 의 두 번째 */
		pcie->supplies[1].supply = "vdd-pex";
		/* [한국어] Tegra20 의 세 번째 */
		pcie->supplies[2].supply = "avdd-pex-pll";
		/* [한국어] Tegra20 의 네 번째 */
		pcie->supplies[3].supply = "avdd-plle";
		/* [한국어] Tegra20 의 다섯 번째 */
		pcie->supplies[4].supply = "vddio-pex-clk";
	}

	/* [한국어] **이 검사가 이 함수의 요점이다.** 위에서 만든 목록이 장치 트리에
	 * 다 있으면 새 바인딩, 하나라도 없으면 구형 바인딩이다 */
	if (of_regulator_bulk_available(dev->of_node, pcie->supplies,
					pcie->num_supplies))
		/* [한국어] 새 바인딩으로 regulator 를 얻는다 */
		return devm_regulator_bulk_get(dev, pcie->num_supplies,
					       pcie->supplies);

	/*
	 * If not all regulators are available for this new scheme, assume
	 * that the device tree complies with an older version of the device
	 * tree binding.
	 */
	dev_info(dev, "using legacy DT binding for power supplies\n");

	/* [한국어] **방금 잡은 배열을 버린다.** 새 바인딩이 맞지 않았으므로 쓸모가 없다 */
	devm_kfree(dev, pcie->supplies);
	/* [한국어] 개수도 0 으로 되돌린다. 구형 경로가 이 값을 다시 채운다 */
	pcie->num_supplies = 0;

	return tegra_pcie_get_legacy_regulators(pcie);
}

/* [한국어]
 * tegra_pcie_parse_dt - 장치 트리에서 포트 목록과 전원 정보를 읽는다
 *
 * @pcie: 컨트롤러 상태. ports 리스트, xbar_config, supplies 를 채운다.
 * @return: 0 성공, 음수 오류.
 *
 * probe 의 첫 단계다. 하드웨어를 건드리기 전에 구성을 파악한다.
 *
 * 자식 노드를 순회하며 루트 포트를 하나씩 만든다. 각 포트에 대해:
 *   1) 장치 트리의 주소에서 devfn 을 얻고 슬롯 번호를 뽑는다.
 *      **장치 트리는 포트를 1부터 세지만 드라이버는 0부터 센다** --
 *      그래서 범위를 1~num_ports 로 검사한 뒤 index-- 로 바꾼다.
 *      tegra_pcie_map_bus 가 port->index + 1 == slot 으로 되돌리는 것이
 *      이 변환의 짝이다.
 *   2) 레인 수를 읽어 lanes 값의 해당 바이트 자리에 넣는다. 16 을 넘으면
 *      오류다.
 *   3) **비활성 포트는 여기서 끝난다.** of_device_is_available 이 거짓이면
 *      레인 위치만 진행시키고 continue 한다. 즉 lanes 에는 반영되지만
 *      mask 에는 반영되지 않고 포트 구조체도 만들지 않는다. lanes 는
 *      xbar 조합을 고르는 데 쓰이므로 꺼진 포트의 레인도 세어야 하고,
 *      mask 는 regulator 를 고르는 데 쓰이므로 실제 쓰는 레인만 세는
 *      것이다. **두 변수의 역할이 다르다.**
 *   4) 포트 구조체를 만들고 레지스터 창을 매핑한다.
 *   5) PERST GPIO 를 얻는다. 원문 주석대로 없으면(-ENOENT) NULL 로 두고,
 *      tegra_pcie_port_reset 이 AFI 레지스터로 대체한다. 그 밖의 오류는
 *      probe 를 중단시킨다.
 *
 * 포트를 다 읽은 뒤 레인 조합으로 xbar 설정을 정하고, 레인 마스크로
 * regulator 목록을 정한다.
 *
 * for_each_child_of_node_scoped 를 쓰므로 노드 참조가 자동으로 놓인다.
 * 중간에 return 해도 새지 않는다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_probe → [이 함수]
 *     → of_pci_get_devfn, devm_pci_remap_cfg_resource, devm_fwnode_gpiod_get,
 *       tegra_pcie_get_xbar_config, tegra_pcie_get_regulators
 */
static int tegra_pcie_parse_dt(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 컨트롤러의 장치 트리 노드 */
	struct device_node *np = dev->of_node;
	/* [한국어] SoC 기술자. 포트 번호 검증에 num_ports 를 쓴다 */
	const struct tegra_pcie_soc *soc = pcie->soc;
	/* [한국어] lanes 는 xbar 조합 식별용(꺼진 포트 포함), mask 는 regulator 선택용
	 * (실제 쓰는 레인만). **두 변수의 역할이 다르다** */
	u32 lanes = 0, mask = 0;
	/* [한국어] **지금까지 소비한 레인 수의 누계.** mask 에서 이 포트의 자리를 정하는 데
	 * 쓰인다. 비활성 포트도 이 값은 진행시킨다 */
	unsigned int lane = 0;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* parse root ports */
	for_each_child_of_node_scoped(np, port) {
		/* [한국어] 이번에 만들 포트 구조체 */
		struct tegra_pcie_port *rp;
		/* [한국어] 0부터 세는 내부 포트 번호 */
		unsigned int index;
		/* [한국어] 이 포트의 레인 수 임시 변수 */
		u32 value;
		/* [한국어] GPIO 요청에 붙일 이름 */
		char *label;

		/* [한국어] 포트 노드의 주소에서 devfn 을 얻는다 */
		err = of_pci_get_devfn(port);
		if (err < 0)
			/* [한국어] 주소를 못 읽으면 장치 트리가 잘못된 것이다 */
			return dev_err_probe(dev, err, "failed to parse address\n");

		/* [한국어] devfn 에서 슬롯 번호를 뽑는다 */
		index = PCI_SLOT(err);

		/* [한국어] **장치 트리는 포트를 1부터 센다.** 그래서 하한이 1 이고 상한이
		 * soc->num_ports 다 */
		if (index < 1 || index > soc->num_ports)
			/* [한국어] 범위를 벗어난 포트 번호다 */
			return dev_err_probe(dev, -EINVAL,
					     "invalid port number: %d\n", index);

		/* [한국어] **장치 트리 번호(1부터)를 내부 번호(0부터)로 바꾼다.**
		 * tegra_pcie_map_bus 가 port->index + 1 == slot 으로 되돌리는 것이 짝이다 */
		index--;

		/* [한국어] 이 포트의 레인 수를 읽는다 */
		err = of_property_read_u32(port, "nvidia,num-lanes", &value);
		if (err < 0)
			/* [한국어] 레인 수를 못 읽으면 포트를 설정할 수 없다 */
			return dev_err_probe(dev, err,
					     "failed to parse # of lanes\n");

		/* [한국어] 레인 수 상한을 검사한다 */
		if (value > 16)
			/* [한국어] PCIe 링크 폭의 최대가 16 이므로 그것을 넘으면 잘못된 값이다 */
			return dev_err_probe(dev, -EINVAL,
					     "invalid # of lanes: %u\n", value);

		/* [한국어] 레인 수를 lanes 의 해당 바이트 자리에 넣는다. index << 3 은 8을 곱한
		 * 것이라, 포트 0 이 최하위 바이트다. tegra_pcie_get_xbar_config 가
		 * 0x010004 같은 값으로 조합을 식별하는 근거가 이 인코딩이다 */
		lanes |= value << (index << 3);

		/* [한국어] **비활성 포트는 여기서 끝난다.** 포트 구조체를 만들지 않는다 */
		if (!of_device_is_available(port)) {
			/* [한국어] 레인 위치만 진행시킨다. **mask 에는 반영하지 않는다** --
			 * 꺼진 포트의 레인은 전원을 줄 필요가 없기 때문이다 */
			lane += value;
			continue;
		}

		/* [한국어] **이 포트가 실제로 쓰는 레인들을 마스크에 표시한다.**
		 * ((1 << value) - 1) 이 레인 수만큼의 연속 비트를 만들고, lane 만큼
		 * 왼쪽으로 밀어 자리를 맞춘다. Tegra30 의 regulator 선택이 이 값을 본다 */
		mask |= ((1 << value) - 1) << lane;
		/* [한국어] 레인 위치를 진행시킨다 */
		lane += value;

		/* [한국어] 포트 구조체를 잡는다. devm 이라 probe 가 실패하면 커널이 놓는다 */
		rp = devm_kzalloc(dev, sizeof(*rp), GFP_KERNEL);
		/* [한국어] 할당 실패 */
		if (!rp)
			return -ENOMEM;

		/* [한국어] 포트의 레지스터 창 주소를 읽는다 */
		err = of_address_to_resource(port, 0, &rp->regs);
		if (err < 0)
			/* [한국어] 주소를 못 읽으면 장치 트리가 잘못된 것이다 */
			return dev_err_probe(dev, err, "failed to parse address\n");

		INIT_LIST_HEAD(&rp->list);
		/* [한국어] **0부터 세는 번호로 저장한다.** 위에서 index-- 로 바꾼 값이다 */
		rp->index = index;
		/* [한국어] 레인 수를 보관한다. PHY 배열 크기와 순회 횟수로 쓰인다 */
		rp->lanes = value;
		/* [한국어] 컨트롤러로 거슬러 올라가는 통로 */
		rp->pcie = pcie;
		/* [한국어] 장치 트리 노드를 보관한다. tegra_pcie_port_get_phys 가 레인별 PHY 를
		 * 찾는 데 쓴다 */
		rp->np = port;

		/* [한국어] 포트의 레지스터 블록을 매핑한다. **config 공간용 함수를 쓰는 데 주의** --
		 * 이 블록이 포트 자신의 config 공간이기도 하기 때문이다 */
		rp->base = devm_pci_remap_cfg_resource(dev, &rp->regs);
		if (IS_ERR(rp->base))
			/* [한국어] 매핑 실패 */
			return PTR_ERR(rp->base);

		/* [한국어] GPIO 요청에 붙일 이름을 만든다. **여기서는 index 를 그대로 쓴다** --
		 * 장치 트리 번호가 아니라 0부터 세는 내부 번호라, 로그에 나타나는
		 * 이름과 장치 트리의 포트 번호가 1 만큼 다르다 */
		label = devm_kasprintf(dev, GFP_KERNEL, "pex-reset-%u", index);
		/* [한국어] 이름 할당 실패 */
		if (!label)
			return -ENOMEM;

		/*
		 * Returns -ENOENT if reset-gpios property is not populated
		 * and in this case fall back to using AFI per port register
		 * to toggle PERST# SFIO line.
		 */
		rp->reset_gpio = devm_fwnode_gpiod_get(dev,
						       of_fwnode_handle(port),
						       "reset",
						       GPIOD_OUT_LOW,
						       label);
		/* [한국어] GPIO 획득 결과를 검사한다 */
		if (IS_ERR(rp->reset_gpio)) {
			/* [한국어] 속성이 없을 때만 오는 코드다 */
			if (PTR_ERR(rp->reset_gpio) == -ENOENT)
				/* [한국어] **GPIO 가 없는 것은 오류가 아니다.** NULL 로 두면
				 * tegra_pcie_port_reset 이 AFI 레지스터로 PERST 를 흔든다 */
				rp->reset_gpio = NULL;
			else
				/* [한국어] 그 밖의 오류는 probe 를 중단한다 */
				return dev_err_probe(dev, PTR_ERR(rp->reset_gpio),
						     "failed to get reset GPIO\n");
		}

		/* [한국어] 포트를 리스트 끝에 매단다. 이 순서가 이후 모든 순회의 순서다 */
		list_add_tail(&rp->list, &pcie->ports);
	}

	/* [한국어] **lanes 를 넘긴다** -- 꺼진 포트의 레인까지 포함한 값이다.
	 * 하드웨어의 레인 배분은 포트 활성 여부와 무관하게 물리적으로 정해지기
	 * 때문이며, 바로 아래에서 mask 를 쓰는 것과 대비된다 */
	err = tegra_pcie_get_xbar_config(pcie, lanes, &pcie->xbar_config);
	/* [한국어] 알 수 없는 레인 조합이면 실패다 */
	if (err < 0)
		/* [한국어] 어느 조합도 맞지 않으면 probe 를 중단한다.
		 * 단 Tegra186 은 default 절이 있어 여기 도달하지 않는다 */
		return dev_err_probe(dev, err,
				     "invalid lane configuration\n");

	/* [한국어] **mask 를 넘긴다** -- 실제로 쓰이는 레인만 담긴 값이라,
	 * Tegra30 이 PEXA/PEXB 중 어느 쪽이 필요한지 판단하는 근거가 된다 */
	err = tegra_pcie_get_regulators(pcie, mask);
	/* [한국어] regulator 획득 실패는 -EPROBE_DEFER 일 수 있다 */
	if (err < 0)
		return err;

	return 0;
}

/*
 * FIXME: If there are no PCIe cards attached, then calling this function
 * can result in the increase of the bootup time as there are big timeout
 * loops.
 */
#define TEGRA_PCIE_LINKUP_TIMEOUT	200	/* up to 1.2 seconds */
/* [한국어]
 * tegra_pcie_port_check_link - 링크가 설 때까지 기다린다
 *
 * @port: 대상 포트.
 * @return: 링크가 서면 true, 세 번 재시도 후에도 안 서면 false.
 *
 * 위의 원문 주석이 FIXME 로 이 함수의 문제를 밝힌다 -- 카드가 꽂혀 있지
 * 않으면 긴 시간 초과 루프 때문에 부팅이 느려진다. 최악의 경우
 * 200 × 2ms × 2단계 × 3회 = 약 2.4초가 걸릴 수 있다(주석은 1.2초로 적는데,
 * 그것은 단계 하나 기준으로 보인다).
 *
 * 먼저 존재 감지를 덮어쓴다. RP_PRIV_MISC 의 PRSNT_MAP 필드를 "장치 있음"
 * 으로 강제하는데, 슬롯 존재 감지 신호가 배선되지 않은 보드에서도 링크
 * 훈련을 시도하게 만드는 조치다. ABSNT 마스크(0xf)를 지우고 PRSNT(0xe)를
 * 넣으므로 최하위 비트만 0 으로 바뀌는 셈이다.
 *
 * 그다음 두 단계를 차례로 기다린다.
 *   1) RP_VEND_XP 의 DL_UP -- 데이터 링크 계층이 올라왔는가. 벤더 확장
 *      레지스터다.
 *   2) RP_LINK_CONTROL_STATUS 의 DL_LINK_ACTIVE -- 링크가 활성인가.
 *      이쪽은 표준 PCIe 링크 상태 레지스터 자리다.
 *
 * 두 단계를 모두 통과해야 true 다. 1)에서 실패하면 곧바로 재시도로 가고,
 * 2)에서 시간이 다 되면 루프를 빠져나와 retry 라벨로 떨어진다 -- 즉
 * 2)의 실패도 재시도 대상이다.
 *
 * 재시도는 포트 리셋 펄스를 다시 주는 것이다. do-while(--retries) 이므로
 * 총 3회 시도한다.
 *
 * **뒷세대와의 대비**: pcie-tegra194.c 의 tegra_pcie_dw_start_link 도
 * 재시도를 하지만 성격이 다르다. 그쪽은 무작정 다시 하지 않고, LTSSM
 * 상태를 보고 DLF 가 원인이라고 판단될 때만 DLF 를 끄고 한 번 더 시도한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_enable_ports → [이 함수] → tegra_pcie_port_reset
 */
static bool tegra_pcie_port_check_link(struct tegra_pcie_port *port)
{
	struct device *dev = port->pcie->dev;
	/* [한국어] **재시도 3회.** 각 시도가 포트 리셋 펄스로 시작한다 */
	unsigned int retries = 3;
	/* [한국어] 레지스터 값 임시 변수 */
	unsigned long value;

	/* override presence detection */
	value = readl(port->base + RP_PRIV_MISC);
	/* [한국어] "장치 없음" 표시를 지운다 */
	value &= ~RP_PRIV_MISC_PRSNT_MAP_EP_ABSNT;
	/* [한국어] "장치 있음"(0xe)을 넣는다. 위의 ABSNT(0xf)와 최하위 비트만 다르다 */
	value |= RP_PRIV_MISC_PRSNT_MAP_EP_PRSNT;
	/* [한국어] 존재 감지를 강제한다 */
	writel(value, port->base + RP_PRIV_MISC);

	do {
		/* [한국어] **단계마다 시한을 새로 잡는다.** 두 단계 각각 200회씩이다 */
		unsigned int timeout = TEGRA_PCIE_LINKUP_TIMEOUT;

		do {
			/* [한국어] 첫 번째 조건을 읽는다 */
			value = readl(port->base + RP_VEND_XP);

			/* [한국어] 벤더 확장의 데이터 링크 업 비트가 서면 다음 단계로 간다 */
			if (value & RP_VEND_XP_DL_UP)
				break;

			/* [한국어] 1~2ms 간격으로 폴링한다. 200회면 최대 400ms 다 */
			usleep_range(1000, 2000);
		} while (--timeout);

		/* [한국어] 시한이 0 이 되었으면 실패다 */
		if (!timeout) {
			/* [한국어] 데이터 링크가 안 올라왔다. dev_dbg 로 낮추는 것은 빈 슬롯에서 흔한
			 * 일이라 정상 부팅에도 로그가 오염되지 않게 하기 위해서다 */
			dev_dbg(dev, "link %u down, retrying\n", port->index);
			goto retry;
		}

		/* [한국어] 시한을 다시 200 으로 놓는다 */
		timeout = TEGRA_PCIE_LINKUP_TIMEOUT;

		do {
			/* [한국어] 두 번째 조건을 읽는다 */
			value = readl(port->base + RP_LINK_CONTROL_STATUS);

			/* [한국어] 표준 링크 상태 레지스터의 활성 비트가 서면 성공이다 */
			if (value & RP_LINK_CONTROL_STATUS_DL_LINK_ACTIVE)
				return true;

			/* [한국어] 1~2ms 간격으로 폴링한다 */
			usleep_range(1000, 2000);
		} while (--timeout);

retry:
		tegra_pcie_port_reset(port);
	} while (--retries);

	return false;
}

/* [한국어]
 * tegra_pcie_change_link_speed - 링크가 선 뒤 Gen2 로 재훈련한다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음. 실패해도 경고나 오류만 찍는다.
 *
 * tegra_pcie_apply_sw_fixup 이 일부러 Gen1 으로 낮춰 둔 것을 되돌리는
 * 함수다. 두 함수가 짝을 이루며, 그 사이에 링크가 선다.
 *
 * 포트마다 네 단계를 밟는다.
 *   1) 목표 속도를 5.0GT/s 로 올린다. 위의 원문 주석이 중요한 사실을
 *      밝히는데, 이 하드웨어는 "지원 링크 속도 벡터"(Link Capabilities 2)를
 *      지원하지 않는다. 그래서 상대가 Gen2 를 지원하는지 확인할 수단이
 *      없지만, 이 함수는 has_gen2 인 칩에서만 불리므로 문제되지 않는다고
 *      주석이 설명한다.
 *   2) 링크가 recovery 에서 나오기를 기다린다. 원문 주석대로 경쟁 조건을
 *      피하기 위해서다 -- 링크가 이미 상태 전이 중인데 재훈련을 요청하면
 *      결과가 불확실해진다.
 *   3) 재훈련을 요청한다(LNKCTL 의 Retrain Link 비트).
 *   4) 재훈련이 끝나기를 기다린다.
 *
 * 2)와 4)가 같은 폴링 구조를 쓴다 -- ktime 기반으로 100ms 까지, 2~3ms
 * 간격이다. jiffies 대신 ktime 을 쓰는 것은 밀리초 단위 정밀도가
 * 필요하기 때문으로 보인다.
 *
 * 실패해도 진행하는 데 주의한다. 2)에서 시간이 다 되면 경고만 찍고
 * 그대로 재훈련을 요청하고, 4)에서 실패하면 오류를 찍고 다음 포트로
 * 넘어간다. Gen1 으로라도 동작하는 것이 아예 못 쓰는 것보다 낫기 때문이다.
 *
 * **이 함수가 이 세대의 성능 상한을 정한다.** Gen2(5.0GT/s)가 최대이며
 * Gen3 이상을 다루는 코드가 없다. 뒷세대 pcie-tegra194.c 는 Gen4 까지
 * 다룬다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. usleep_range 로 잠든다.
 *
 * 호출 체인:
 *   tegra_pcie_enable_ports → [이 함수] (soc->has_gen2 일 때만)
 */
static void tegra_pcie_change_link_speed(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 순회할 포트 */
	struct tegra_pcie_port *port;
	/* [한국어] ktime 기반 시한. jiffies 대신 쓰는 것은 밀리초 단위 정밀도가
	 * 필요하기 때문으로 보인다 */
	ktime_t deadline;
	/* [한국어] 레지스터 값 임시 변수 */
	u32 value;

	list_for_each_entry(port, &pcie->ports, list) {
		/*
		 * "Supported Link Speeds Vector" in "Link Capabilities 2"
		 * is not supported by Tegra. tegra_pcie_change_link_speed()
		 * is called only for Tegra chips which support Gen2.
		 * So there no harm if supported link speed is not verified.
		 */
		value = readl(port->base + RP_LINK_CONTROL_STATUS_2);
		/* [한국어] 기존 속도 필드를 지운다 */
		value &= ~PCI_EXP_LNKSTA_CLS;
		/* [한국어] 5.0GT/s 를 넣는다. **이것이 이 세대의 상한이다** */
		value |= PCI_EXP_LNKSTA_CLS_5_0GB;
		/* [한국어] 목표 속도를 쓴다 */
		writel(value, port->base + RP_LINK_CONTROL_STATUS_2);

		/*
		 * Poll until link comes back from recovery to avoid race
		 * condition.
		 */
		deadline = ktime_add_us(ktime_get(), LINK_RETRAIN_TIMEOUT);

		/* [한국어] 위의 원문 주석대로 경쟁 조건을 피하려고 recovery 에서 나오기를 기다린다 --
		 * 이미 상태 전이 중인데 재훈련을 요청하면 결과가 불확실해진다 */
		while (ktime_before(ktime_get(), deadline)) {
			/* [한국어] 링크 상태를 읽는다 */
			value = readl(port->base + RP_LINK_CONTROL_STATUS);
			/* [한국어] 훈련 중 비트가 내려가면 안정된 상태다 */
			if ((value & PCI_EXP_LNKSTA_LT) == 0)
				break;

			/* [한국어] 2~3ms 간격으로 폴링한다 */
			usleep_range(2000, 3000);
		}

		/* [한국어] 시간이 다 됐는데도 recovery 면 경고만 찍고 **그대로 진행한다** */
		if (value & PCI_EXP_LNKSTA_LT)
			dev_warn(dev, "PCIe port %u link is in recovery\n",
				 port->index);

		/* Retrain the link */
		value = readl(port->base + RP_LINK_CONTROL_STATUS);
		/* [한국어] Retrain Link 비트를 세운다 */
		value |= PCI_EXP_LNKCTL_RL;
		/* [한국어] 재훈련을 요청한다 */
		writel(value, port->base + RP_LINK_CONTROL_STATUS);

		/* [한국어] 다시 100ms 시한을 잡는다 */
		deadline = ktime_add_us(ktime_get(), LINK_RETRAIN_TIMEOUT);

		/* [한국어] 재훈련이 끝나기를 기다린다 */
		while (ktime_before(ktime_get(), deadline)) {
			/* [한국어] 링크 상태를 다시 읽는다 */
			value = readl(port->base + RP_LINK_CONTROL_STATUS);
			/* [한국어] 훈련 중 비트가 내려가면 끝난 것이다 */
			if ((value & PCI_EXP_LNKSTA_LT) == 0)
				break;

			/* [한국어] 2~3ms 간격으로 폴링한다 */
			usleep_range(2000, 3000);
		}

		/* [한국어] 시간이 다 됐는데도 훈련 중이면 실패다 */
		if (value & PCI_EXP_LNKSTA_LT)
			/* [한국어] 재훈련 실패. 오류만 찍고 다음 포트로 넘어간다 -- Gen1 으로라도
			 * 동작하는 것이 아예 못 쓰는 것보다 낫기 때문이다 */
			dev_err(dev, "failed to retrain link of port %u\n",
				port->index);
	}
}

/* [한국어]
 * tegra_pcie_enable_ports - 모든 포트를 켜고 링크를 세운다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음.
 *
 * 포트 기동의 전체 순서를 쥔 함수다. 세 국면으로 나뉜다.
 *
 *   1) 모든 포트를 설정한다. 아직 링크는 서지 않는다.
 *   2) **pcie_xrst 리셋을 푼다.** 원문 주석이 "Start LTSSM from Tegra side"
 *      라고 적은 그 줄이며, 이 순간 모든 포트가 동시에 링크 훈련을
 *      시작한다. 리셋 셋 중 이것만 여기까지 남겨 둔 이유가 이것이다.
 *   3) 포트마다 링크가 섰는지 확인하고, 안 선 포트는 끄고 리스트에서 뺀다.
 *      그 뒤로는 이 포트가 존재하지 않는 것처럼 다뤄진다 --
 *      tegra_pcie_map_bus 가 슬롯을 못 찾아 config 접근이 실패하고,
 *      debugfs 목록에도 나오지 않는다.
 *   4) Gen2 지원 칩이면 속도를 올린다.
 *
 * 두 순회 모두 list_for_each_entry_safe 를 쓴다. 첫 번째 순회는 리스트를
 * 바꾸지 않으므로 _safe 가 필요 없지만, 두 번째는
 * tegra_pcie_port_free 가 원소를 지우므로 반드시 필요하다.
 *
 * **링크가 안 선 포트를 조용히 버리는 것** 이 이 드라이버의 정책이다.
 * dev_info 로 알리기만 하고 probe 는 계속 진행한다. 포트 하나가 비어
 * 있다고 나머지를 못 쓰게 할 이유가 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 3)에서 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_pcie_pm_resume → [이 함수]
 *     → tegra_pcie_port_enable, tegra_pcie_port_check_link,
 *       tegra_pcie_port_disable, tegra_pcie_port_free,
 *       tegra_pcie_change_link_speed
 */
static void tegra_pcie_enable_ports(struct tegra_pcie *pcie)
{
	struct device *dev = pcie->dev;
	/* [한국어] 순회용 포인터와 임시 보관용 */
	struct tegra_pcie_port *port, *tmp;

	/* [한국어] 모든 포트를 먼저 설정한다. 아직 링크는 서지 않는다 */
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		/* [한국어] 어느 포트를 몇 레인으로 시도하는지 알린다 */
		dev_info(dev, "probing port %u, using %u lanes\n",
			 port->index, port->lanes);

		tegra_pcie_port_enable(port);
	}

	/* Start LTSSM from Tegra side */
	reset_control_deassert(pcie->pcie_xrst);

	/* [한국어] **두 번째 순회는 _safe 가 필수다** -- 아래에서 원소를 지우기 때문이다 */
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		/* [한국어] 링크가 섰으면 그대로 둔다 */
		if (tegra_pcie_port_check_link(port))
			continue;

		/* [한국어] 링크가 안 선 포트를 알린다. **probe 를 실패시키지 않는다** --
		 * 포트 하나가 비어 있다고 나머지를 못 쓰게 할 이유가 없기 때문이다 */
		dev_info(dev, "link %u down, ignoring\n", port->index);

		tegra_pcie_port_disable(port);
		tegra_pcie_port_free(port);
	}

	/* [한국어] Gen2 지원 칩이면 링크가 선 뒤 속도를 올린다.
	 * tegra_pcie_apply_sw_fixup 이 Gen1 으로 낮춰 둔 것을 되돌리는 짝이다 */
	if (pcie->soc->has_gen2)
		tegra_pcie_change_link_speed(pcie);
}

/* [한국어]
 * tegra_pcie_disable_ports - 모든 포트를 끈다
 *
 * @pcie: 컨트롤러 상태.
 * @return: 없음.
 *
 * tegra_pcie_enable_ports 의 역순이다. 먼저 pcie_xrst 를 어서트해 LTSSM 을
 * 멈춘 뒤 포트를 하나씩 끈다. 순서가 중요한데, 링크가 도는 중에 포트
 * 설정을 건드리면 상태가 불확실해지기 때문이다.
 *
 * _safe 판을 쓰지만 리스트를 바꾸지 않는다. tegra_pcie_enable_ports 와
 * 형태를 맞춘 것으로 보인다.
 *
 * 링크가 안 서서 이미 제거된 포트는 리스트에 없으므로 자연히 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(절전 경로).
 *
 * 호출 체인:
 *   tegra_pcie_pm_suspend → [이 함수] → tegra_pcie_port_disable
 */
static void tegra_pcie_disable_ports(struct tegra_pcie *pcie)
{
	struct tegra_pcie_port *port, *tmp;

	reset_control_assert(pcie->pcie_xrst);

	/* [한국어] 포트를 하나씩 끈다. _safe 판을 쓰지만 리스트를 바꾸지 않는다 --
	 * tegra_pcie_enable_ports 와 형태를 맞춘 것으로 보인다 */
	list_for_each_entry_safe(port, tmp, &pcie->ports, list)
		tegra_pcie_port_disable(port);
}

/* [한국어] 포트별 PME 비트 표. 비트 배치가 규칙적이지 않아 표로 둔다 */
static const struct tegra_pcie_port_soc tegra20_pcie_ports[] = {
	/* [한국어] Tegra20 의 PME 비트 표. **Tegra124 와 Tegra210 도 이 표를 공유한다** --
	 * 포트가 둘이고 비트 배치가 같기 때문이다 */
	{ .pme.turnoff_bit = 0, .pme.ack_bit =  5 },
	{ .pme.turnoff_bit = 8, .pme.ack_bit = 10 },
};

/* [한국어] Tegra20 용 기술자. **가장 오래된 세대** 라 기능 불리언이 거의 다 false 이고,
 * 오직 has_cache_bars 만 true 다 -- 이후 세대에서 사라진 레지스터다.
 * 다른 기술자들과 견주는 기준선이 된다 */
static const struct tegra_pcie_soc tegra20_pcie = {
	/* [한국어] Tegra20 은 포트가 둘이다 */
	.num_ports = 2,
	.ports = tegra20_pcie_ports,
	.msi_base_shift = 0,
	.pads_pll_ctl = PADS_PLL_CTL_TEGRA20,
	.tx_ref_sel = PADS_PLL_CTL_TXCLKREF_DIV10,
	.pads_refclk_cfg0 = 0xfa5cfa5c,
	.has_pex_clkreq_en = false,
	.has_pex_bias_ctrl = false,
	.has_intr_prsnt_sense = false,
	.has_cml_clk = false,
	.has_gen2 = false,
	.force_pca_enable = false,
	.program_uphy = true,
	.update_clamp_threshold = false,
	.program_deskew_time = false,
	.update_fc_timer = false,
	.has_cache_bars = true,
	.ectl.enable = false,
};

/* [한국어] Tegra30 은 포트가 셋이라 별도 표가 필요하다 */
static const struct tegra_pcie_port_soc tegra30_pcie_ports[] = {
	/* [한국어] Tegra30 의 PME 비트 표. 포트가 셋이라 항목이 하나 더 있다 */
	{ .pme.turnoff_bit =  0, .pme.ack_bit =  5 },
	{ .pme.turnoff_bit =  8, .pme.ack_bit = 10 },
	{ .pme.turnoff_bit = 16, .pme.ack_bit = 18 },
};

/* [한국어] Tegra30 용 기술자. Tegra20 에 없던 기능이 여럿 켜지지만
 * 아직 Gen2 는 없다 */
static const struct tegra_pcie_soc tegra30_pcie = {
	/* [한국어] Tegra30 은 포트가 셋이다 */
	.num_ports = 3,
	.ports = tegra30_pcie_ports,
	.msi_base_shift = 8,
	.afi_pex2_ctrl = 0x128,
	.pads_pll_ctl = PADS_PLL_CTL_TEGRA30,
	.tx_ref_sel = PADS_PLL_CTL_TXCLKREF_BUF_EN,
	.pads_refclk_cfg0 = 0xfa5cfa5c,
	.pads_refclk_cfg1 = 0xfa5cfa5c,
	.has_pex_clkreq_en = true,
	.has_pex_bias_ctrl = true,
	.has_intr_prsnt_sense = true,
	.has_cml_clk = true,
	.has_gen2 = false,
	.force_pca_enable = false,
	.program_uphy = true,
	.update_clamp_threshold = false,
	.program_deskew_time = false,
	.update_fc_timer = false,
	.has_cache_bars = false,
	.ectl.enable = false,
};

/* [한국어] Tegra124 용 기술자. **Gen2 를 지원하는 첫 세대** 이며,
 * 포트 표를 tegra20_pcie_ports 와 공유한다 -- 포트가 둘이고 PME 비트
 * 배치가 같기 때문이다 */
static const struct tegra_pcie_soc tegra124_pcie = {
	/* [한국어] Tegra124 는 포트가 둘이다 */
	.num_ports = 2,
	.ports = tegra20_pcie_ports,
	.msi_base_shift = 8,
	.pads_pll_ctl = PADS_PLL_CTL_TEGRA30,
	.tx_ref_sel = PADS_PLL_CTL_TXCLKREF_BUF_EN,
	.pads_refclk_cfg0 = 0x44ac44ac,
	.has_pex_clkreq_en = true,
	.has_pex_bias_ctrl = true,
	.has_intr_prsnt_sense = true,
	.has_cml_clk = true,
	.has_gen2 = true,
	.force_pca_enable = false,
	.program_uphy = true,
	.update_clamp_threshold = true,
	.program_deskew_time = false,
	.update_fc_timer = false,
	.has_cache_bars = false,
	.ectl.enable = false,
};

/* [한국어] Tegra210 용 기술자. **이 파일에서 가장 많은 우회를 요구하는 SoC** 다 --
 * force_pca_enable, program_deskew_time, update_fc_timer, ectl.enable 이
 * 모두 이 SoC 에서만 true 다 */
static const struct tegra_pcie_soc tegra210_pcie = {
	/* [한국어] Tegra210 은 포트가 둘이다 */
	.num_ports = 2,
	.ports = tegra20_pcie_ports,
	.msi_base_shift = 8,
	.pads_pll_ctl = PADS_PLL_CTL_TEGRA30,
	.tx_ref_sel = PADS_PLL_CTL_TXCLKREF_BUF_EN,
	.pads_refclk_cfg0 = 0x90b890b8,
	/* FC threshold is bit[25:18] */
	.update_fc_threshold = 0x01800000,
	.has_pex_clkreq_en = true,
	.has_pex_bias_ctrl = true,
	.has_intr_prsnt_sense = true,
	.has_cml_clk = true,
	.has_gen2 = true,
	.force_pca_enable = true,
	.program_uphy = true,
	.update_clamp_threshold = true,
	.program_deskew_time = true,
	.update_fc_timer = true,
	.has_cache_bars = false,
	.ectl = {
		/* [한국어] 이퀄라이저 레지스터 값 묶음 */
		.regs = {
			/* [한국어] **Tegra210 만 이퀄라이저 값을 갖는다.** 여덟 값의 의미는 이 트리에서
			 * 확인할 수 없으며, 필드 이름이 수신단 이퀄라이저와 클록 복원 관련임을
			 * 가리킬 뿐이다 */
			.rp_ectl_2_r1 = 0x0000000f,
			.rp_ectl_4_r1 = 0x00000067,
			.rp_ectl_5_r1 = 0x55010000,
			.rp_ectl_6_r1 = 0x00000001,
			.rp_ectl_2_r2 = 0x0000008f,
			.rp_ectl_4_r2 = 0x000000c7,
			.rp_ectl_5_r2 = 0x55010000,
			.rp_ectl_6_r2 = 0x00000001,
		},
		.enable = true,
	},
};

static const struct tegra_pcie_port_soc tegra186_pcie_ports[] = {
	/* [한국어] Tegra186 의 PME 비트 표. 셋째 포트가 (12,14)로 Tegra30 의 (16,18)과
	 * 다르다 -- 그래서 표를 재사용하지 못하고 따로 두었다 */
	{ .pme.turnoff_bit =  0, .pme.ack_bit =  5 },
	{ .pme.turnoff_bit =  8, .pme.ack_bit = 10 },
	{ .pme.turnoff_bit = 12, .pme.ack_bit = 14 },
};

/* [한국어] Tegra186 용 기술자. **program_uphy 가 false 인 유일한 SoC** 로,
 * PHY 를 이 드라이버가 다루지 않는다. 뒷세대로 넘어가는 과도기임을
 * 보여 주는 값이다 */
static const struct tegra_pcie_soc tegra186_pcie = {
	/* [한국어] Tegra186 은 포트가 셋이다 */
	.num_ports = 3,
	.ports = tegra186_pcie_ports,
	.msi_base_shift = 8,
	.afi_pex2_ctrl = 0x19c,
	.pads_pll_ctl = PADS_PLL_CTL_TEGRA30,
	.tx_ref_sel = PADS_PLL_CTL_TXCLKREF_BUF_EN,
	.pads_refclk_cfg0 = 0x80b880b8,
	.pads_refclk_cfg1 = 0x000480b8,
	.has_pex_clkreq_en = true,
	.has_pex_bias_ctrl = true,
	.has_intr_prsnt_sense = true,
	.has_cml_clk = false,
	.has_gen2 = true,
	.force_pca_enable = false,
	.program_uphy = false,
	.update_clamp_threshold = false,
	.program_deskew_time = false,
	.update_fc_timer = false,
	.has_cache_bars = false,
	.ectl.enable = false,
};

/* [한국어] 이 드라이버가 다루는 SoC 목록 */
static const struct of_device_id tegra_pcie_of_match[] = {
	/* [한국어] 장치 트리 compatible 과 SoC 기술자의 대응.
	 * **최신 세대부터 나열되어 있다** -- of_match 는 순서와 무관하게 정확히
	 * 일치하는 것을 찾으므로 동작에는 영향이 없다 */
	{ .compatible = "nvidia,tegra186-pcie", .data = &tegra186_pcie },
	{ .compatible = "nvidia,tegra210-pcie", .data = &tegra210_pcie },
	{ .compatible = "nvidia,tegra124-pcie", .data = &tegra124_pcie },
	{ .compatible = "nvidia,tegra30-pcie", .data = &tegra30_pcie },
	{ .compatible = "nvidia,tegra20-pcie", .data = &tegra20_pcie },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_pcie_of_match);

/* [한국어]
 * tegra_pcie_ports_seq_start - debugfs 포트 목록 순회를 시작한다
 *
 * @s:   seq_file 문맥. private 에 struct tegra_pcie 가 들어 있다.
 * @pos: 시작 위치(출력 겸용). seq_file 코어가 관리한다.
 * @return: 첫 원소의 포인터. 포트가 없으면 NULL.
 *
 * debugfs 의 pcie/ports 파일을 읽을 때 seq_file 코어가 부르는 네 콜백 중
 * 첫째다. 사용자가 cat 으로 읽으면 start → show → next → show → ... → stop
 * 순으로 불린다.
 *
 * 포트 리스트가 비어 있으면 NULL 을 돌려줘 아무것도 출력하지 않는다.
 * 링크가 안 선 포트는 tegra_pcie_port_free 가 이미 리스트에서 뺐으므로,
 * 여기 나오는 것은 실제로 동작하는 포트뿐이다.
 *
 * **헤더 줄을 여기서 찍는다.** show 콜백이 아니라 start 에서 찍는 것은
 * 그래야 한 번만 나오기 때문이다.
 *
 * seq_list_start 가 pos 만큼 건너뛴 원소를 돌려준다. 큰 출력이 여러 번에
 * 나눠 읽힐 때 이어서 볼 수 있게 하는 seq_file 의 규약이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자가 debugfs 파일을 읽을 때).
 *
 * 호출 체인:
 *   read(2) → seq_file 코어 → tegra_pcie_ports_sops.start → [이 함수]
 */
static void *tegra_pcie_ports_seq_start(struct seq_file *s, loff_t *pos)
{
	struct tegra_pcie *pcie = s->private;

	/* [한국어] 포트가 없으면 아무것도 출력하지 않는다. 링크가 안 선 포트는
	 * tegra_pcie_port_free 가 이미 리스트에서 뺐으므로, 여기 나오는 것은
	 * 실제로 동작하는 포트뿐이다 */
	if (list_empty(&pcie->ports))
		return NULL;

	/* [한국어] **헤더 줄을 start 에서 찍는다.** show 가 아니라 여기서 찍어야
	 * 한 번만 나온다 */
	seq_puts(s, "Index  Status\n");

	return seq_list_start(&pcie->ports, *pos);
}

/* [한국어]
 * tegra_pcie_ports_seq_next - 다음 포트로 넘어간다
 *
 * @s:   seq_file 문맥.
 * @v:   현재 원소.
 * @pos: 위치(입출력). seq_list_next 가 증가시킨다.
 * @return: 다음 원소, 끝이면 NULL.
 *
 * seq_list_next 에 그대로 위임한다. 리스트 순회의 표준 관용구다.
 *
 * 이 함수가 NULL 을 돌려주면 순회가 끝나고 stop 이 불린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   seq_file 코어 → [이 함수] → seq_list_next
 */
static void *tegra_pcie_ports_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct tegra_pcie *pcie = s->private;

	return seq_list_next(v, &pcie->ports, pos);
}

/* [한국어]
 * tegra_pcie_ports_seq_stop - 순회를 마친다
 *
 * @s: seq_file 문맥.
 * @v: 마지막 원소(또는 NULL).
 * @return: 없음.
 *
 * **본문이 비어 있다.** 보통 이 콜백은 start 에서 잡은 락을 푸는 자리인데,
 * 이 드라이버는 순회 중 락을 잡지 않으므로 할 일이 없다.
 *
 * 그래도 함수를 두는 이유는 struct seq_operations 가 네 콜백을 모두
 * 요구하기 때문이다. NULL 을 넣어도 되는지는 이 파일에서 알 수 없으나,
 * 빈 함수를 두는 쪽을 택했다.
 *
 * 락을 잡지 않는다는 것은 순회 중 포트 리스트가 바뀌면 위험하다는 뜻이다.
 * 다만 리스트를 바꾸는 것은 probe 와 절전 경로뿐이고 debugfs 읽기와
 * 동시에 일어날 일이 드물어, 실용적으로 문제가 되지 않는 선택으로 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   seq_file 코어 → [이 함수]
 */
static void tegra_pcie_ports_seq_stop(struct seq_file *s, void *v)
{
}

/* [한국어]
 * tegra_pcie_ports_seq_show - 포트 하나의 링크 상태를 출력한다
 *
 * @s: seq_file 문맥.
 * @v: 현재 원소. list_entry 로 struct tegra_pcie_port 를 꺼낸다.
 * @return: 항상 0.
 *
 * 포트 번호와 링크 상태를 한 줄로 찍는다.
 *
 * **두 상태를 각각 다른 레지스터에서 읽는다.**
 *   up     : RP_VEND_XP 의 DL_UP -- 데이터 링크가 올라왔는가(벤더 확장).
 *   active : RP_LINK_CONTROL_STATUS 의 DL_LINK_ACTIVE -- 링크가 활성인가.
 * tegra_pcie_port_check_link 이 기다리는 두 조건과 정확히 같다. 즉 이
 * debugfs 파일은 그 함수가 무엇을 보고 판단했는지를 사후에 확인하는
 * 창이다.
 *
 * 출력 형식이 조건부라 조금 복잡하다. 둘 다 참이면 "up, active",
 * up 만이면 "up", active 만이면 "active", 둘 다 아니면 번호와 공백만 나온다.
 * 쉼표는 up 이 이미 찍혔을 때만 넣는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 레지스터를 읽으므로 컨트롤러 전원이
 * 올라와 있어야 한다.
 *
 * 호출 체인:
 *   seq_file 코어 → [이 함수] → readl
 */
static int tegra_pcie_ports_seq_show(struct seq_file *s, void *v)
{
	bool up = false, active = false;
	/* [한국어] 현재 순회 중인 포트 */
	struct tegra_pcie_port *port;
	/* [한국어] 레지스터 값 임시 변수 */
	unsigned int value;

	/* [한국어] 리스트 고리에서 포트 구조체를 되찾는다 */
	port = list_entry(v, struct tegra_pcie_port, list);

	/* [한국어] **두 상태를 각각 다른 레지스터에서 읽는다.**
	 * tegra_pcie_port_check_link 이 기다리는 두 조건과 정확히 같다 --
	 * 이 파일은 그 함수가 무엇을 보고 판단했는지 사후에 확인하는 창이다 */
	value = readl(port->base + RP_VEND_XP);

	/* [한국어] 벤더 확장 레지스터의 데이터 링크 업 비트를 본다 */
	if (value & RP_VEND_XP_DL_UP)
		/* [한국어] 데이터 링크 업 표시 */
		up = true;

	/* [한국어] 두 번째 상태를 읽는다 */
	value = readl(port->base + RP_LINK_CONTROL_STATUS);

	/* [한국어] 표준 링크 상태 레지스터의 활성 비트를 본다 */
	if (value & RP_LINK_CONTROL_STATUS_DL_LINK_ACTIVE)
		/* [한국어] 링크 활성 표시 */
		active = true;

	/* [한국어] 포트 번호를 폭 2로 찍는다 */
	seq_printf(s, "%2u     ", port->index);

	/* [한국어] up 이면 먼저 찍는다 */
	if (up)
		/* [한국어] 데이터 링크 업 표시 */
		seq_puts(s, "up");

	/* [한국어] 링크 활성이면 표시한다 */
	if (active) {
		/* [한국어] 앞에 무언가 찍혔는지 확인한다 */
		if (up)
			/* [한국어] up 이 이미 찍혔을 때만 쉼표를 넣는다 */
			seq_puts(s, ", ");

		/* [한국어] 활성 표시 */
		seq_puts(s, "active");
	}

	/* [한국어] 줄바꿈으로 마무리한다. 둘 다 거짓이면 번호와 공백만 찍힌 줄이 된다 */
	seq_puts(s, "\n");
	return 0;
}

/* [한국어] debugfs 포트 목록의 순회 동작 정의 */
static const struct seq_operations tegra_pcie_ports_sops = {
	/* [한국어] 네 콜백 등록. seq_file 코어가 start → show → next → ... → stop 순으로 부른다 */
	.start = tegra_pcie_ports_seq_start,
	.next = tegra_pcie_ports_seq_next,
	.stop = tegra_pcie_ports_seq_stop,
	.show = tegra_pcie_ports_seq_show,
};

DEFINE_SEQ_ATTRIBUTE(tegra_pcie_ports);

/* [한국어]
 * tegra_pcie_debugfs_init - debugfs 에 포트 상태 파일을 만든다
 *
 * @pcie: 컨트롤러 상태. debugfs 필드를 채운다.
 * @return: 없음. debugfs 생성 실패는 무시한다.
 *
 * debugfs 에 "pcie" 디렉터리를 만들고 그 안에 "ports" 파일을 둔다.
 * 사용자가 그것을 읽으면 위의 seq 콜백 넷이 돌아 포트별 링크 상태를 찍는다.
 *
 * **디렉터리를 debugfs 루트에 만드는 데 주의한다.** 부모가 NULL 이므로
 * 경로가 /sys/kernel/debug/pcie 가 된다. 컨트롤러가 여러 개면 이름이
 * 충돌하겠지만, 이 드라이버는 SoC 에 인스턴스가 하나뿐이라 문제가 되지
 * 않는다. 뒷세대 pcie-tegra194.c 는 컨트롤러가 여럿이라 장치 트리 경로를
 * 이름으로 써서 구별한다(init_debugfs).
 *
 * 파일 권한이 읽기 전용이다.
 *
 * 반환값을 검사하지 않는 것이 debugfs 의 관례다 -- 디버그 기능이 없다고
 * 드라이버가 실패할 이유가 없기 때문이며, debugfs 함수들이 오류 포인터를
 * 받아도 안전하게 동작하도록 설계되어 있다.
 *
 * probe 에서 IS_ENABLED(CONFIG_DEBUG_FS) 검사 안에서 불린다.
 *
 * 실행 컨텍스트: probe 경로.
 *
 * 호출 체인:
 *   tegra_pcie_probe → [이 함수] → debugfs_create_dir, debugfs_create_file
 */
static void tegra_pcie_debugfs_init(struct tegra_pcie *pcie)
{
	pcie->debugfs = debugfs_create_dir("pcie", NULL);

	/* [한국어] 포트 상태 파일을 만든다. 권한이 읽기 전용이고, private 로 pcie 를 넘겨
	 * seq 콜백들이 s->private 로 되찾는다 */
	debugfs_create_file("ports", S_IFREG | S_IRUGO, pcie->debugfs, pcie,
			    &tegra_pcie_ports_fops);
}

/* [한국어]
 * tegra_pcie_probe - 플랫폼 드라이버 진입점
 *
 * @pdev: 장치 트리 매칭으로 만들어진 플랫폼 장치.
 * @return: 0 성공, 음수 오류.
 *
 *   1) 브리지와 드라이버 상태를 한 덩어리로 잡는다. 뒤에 붙는 private
 *      영역이 struct tegra_pcie 이고, host->sysdata 로 그것을 심어 두면
 *      config 접근 경로에서 bus->sysdata 로 되돌아온다.
 *   2) SoC 기술자를 고른다. 이 파일의 모든 세대 분기가 이 포인터를 통과한다.
 *   3) 장치 트리를 읽는다(포트 목록, 레인 배분, regulator).
 *   4) 하드웨어 자원을 확보한다(클록, 리셋, PHY, 레지스터 창, 오류 IRQ).
 *   5) MSI 소프트웨어 쪽을 준비한다.
 *   6) **runtime PM 을 켜고 동기 get 을 한다.** 이 한 줄이 실제로
 *      tegra_pcie_pm_resume 을 불러 하드웨어 전체를 기동한다 -- 전원, 컨트롤러
 *      설정, 변환 창, MSI 활성화, PHY, 포트 기동이 모두 그 안에서 일어난다.
 *      **이 드라이버의 구조적 특징이 여기 있다.** probe 가 기동 순서를 직접
 *      쥐지 않고 PM 콜백에 위임하므로, 부팅과 절전 복귀가 같은 코드를 탄다.
 *   7) pci_ops 와 map_irq 를 꽂고 PCI 코어에 넘긴다.
 *   8) debugfs 를 만든다.
 *
 * map_irq 를 지정하는 것에 주의 -- INTx 를 별도 도메인으로 다루지 않고
 * 콜백 하나로 해결한다. 이 파일에 INTx irq_domain 이 없는 이유이며,
 * pcie-mediatek-gen3.c 같은 드라이버가 전용 도메인을 만드는 것과 대비된다.
 *
 * 정리 경로가 두 라벨이다. pm_runtime_put 라벨은 6) 이후의 실패를 다루고,
 * put_resources 는 그 전을 다룬다. 다만 pm_runtime_put 경로가
 * tegra_pcie_put_resources 까지 흘러가므로 두 라벨이 이어져 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 버스 → [이 함수]
 *     → tegra_pcie_parse_dt, tegra_pcie_get_resources, tegra_pcie_msi_setup,
 *       pm_runtime_get_sync(→ tegra_pcie_pm_resume), pci_host_probe
 */
static int tegra_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* [한국어] PCI 코어에 넘길 브리지 */
	struct pci_host_bridge *host;
	/* [한국어] 브리지 뒤에 붙을 이 드라이버의 상태 */
	struct tegra_pcie *pcie;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 브리지와 드라이버 상태를 한 덩어리로 잡는다. devm 이라 실패 경로나
	 * 해제에서 따로 놓을 필요가 없다 */
	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 할당 실패는 메모리 부족뿐이다 */
	if (!host)
		return -ENOMEM;

	/* [한국어] 브리지 뒤에 붙은 private 영역이 struct tegra_pcie 다 */
	pcie = pci_host_bridge_priv(host);
	/* [한국어] **sysdata 로 컨트롤러 상태를 심는다.** 이 값이 config 접근 경로에서
	 * bus->sysdata 로 되돌아와 tegra_pcie_map_bus 등이 쓴다 */
	host->sysdata = pcie;
	/* [한국어] 플랫폼 장치에 드라이버 상태를 매단다. PM 콜백이 dev_get_drvdata 로
	 * 이 값을 되찾는다 */
	platform_set_drvdata(pdev, pcie);

	/* [한국어] **SoC 기술자를 고른다.** 장치 트리의 compatible 이 어느 항목과 맞았느냐로
	 * 정해지며, 이 파일의 모든 세대 분기가 이 포인터를 통과한다 */
	pcie->soc = of_device_get_match_data(dev);
	INIT_LIST_HEAD(&pcie->ports);
	/* [한국어] 이후 모든 로그와 자원 획득의 기준이 될 device 포인터 */
	pcie->dev = dev;

	/* [한국어] 장치 트리를 읽는다. 포트 목록, 레인 배분, regulator 목록이 정해진다 */
	err = tegra_pcie_parse_dt(pcie);
	/* [한국어] 장치 트리가 잘못되었으면 그대로 반환한다 */
	if (err < 0)
		return err;

	/* [한국어] 클록, 리셋, PHY, 레지스터 창, 오류 인터럽트를 확보한다 */
	err = tegra_pcie_get_resources(pcie);
	/* [한국어] 실패하면 그대로 반환한다 -- 그 함수가 자기 안에서 PHY 를 정리한다 */
	if (err < 0) {
		/* [한국어] 자원 획득 실패. -EPROBE_DEFER 일 수 있다 */
		dev_err(dev, "failed to request resources: %d\n", err);
		return err;
	}

	/* [한국어] MSI 소프트웨어 쪽을 준비한다. 도메인, 락, 수신 페이지까지이며
	 * 하드웨어 레지스터는 아직 건드리지 않는다 */
	err = tegra_pcie_msi_setup(pcie);
	/* [한국어] 실패하면 자원만 되돌린다 */
	if (err < 0) {
		/* [한국어] MSI 준비 실패 */
		dev_err(dev, "failed to enable MSI support: %d\n", err);
		goto put_resources;
	}

	pm_runtime_enable(pcie->dev);
	/* [한국어] **이 한 줄이 tegra_pcie_pm_resume 을 불러 하드웨어 전체를 기동한다.**
	 * 전원, 컨트롤러 설정, 변환 창, MSI, PHY, 포트 기동이 모두 그 안에서
	 * 일어난다. probe 가 기동 순서를 직접 쥐지 않고 PM 콜백에 위임하는
	 * 이 구조 덕분에 부팅과 절전 복귀가 같은 코드를 탄다 */
	err = pm_runtime_get_sync(pcie->dev);
	/* [한국어] 실패하면 MSI 와 자원을 되감는다 */
	if (err < 0) {
		/* [한국어] 하드웨어 기동 실패 */
		dev_err(dev, "fail to enable pcie controller: %d\n", err);
		goto pm_runtime_put;
	}

	/* [한국어] config 접근 방법을 브리지에 꽂는다 */
	host->ops = &tegra_pcie_ops;
	/* [한국어] **INTx 를 콜백 하나로 해결한다.** 전용 irq_domain 을 만들지 않는 것이
	 * 이 드라이버의 선택이며, MSI 쪽은 도메인을 만드는 것과 대비된다 */
	host->map_irq = tegra_pcie_map_irq;

	/* [한국어] PCI 코어에 넘긴다. 이 호출 안에서 버스가 스캔되고,
	 * 그 과정에서 tegra_pcie_ops 의 config 접근이 처음 쓰인다 */
	err = pci_host_probe(host);
	/* [한국어] 실패하면 하드웨어를 되감는다 */
	if (err < 0) {
		/* [한국어] 버스 열거 실패 */
		dev_err(dev, "failed to register host: %d\n", err);
		goto pm_runtime_put;
	}

	/* [한국어] debugfs 파일을 만든다. **PCI 코어에 넘긴 뒤에** 하는 것에 주의 --
	 * 포트 리스트가 확정된 뒤여야 목록이 정확하다 */
	if (IS_ENABLED(CONFIG_DEBUG_FS))
		tegra_pcie_debugfs_init(pcie);

	return 0;

pm_runtime_put:
	pm_runtime_put_sync(pcie->dev);
	pm_runtime_disable(pcie->dev);
	tegra_pcie_msi_teardown(pcie);
put_resources:
	tegra_pcie_put_resources(pcie);
	return err;
}

/* [한국어]
 * tegra_pcie_pm_suspend - 컨트롤러를 재운다
 *
 * @dev: 이 컨트롤러의 device. drvdata 에 struct tegra_pcie 가 있다.
 * @return: 항상 0. 중간 실패는 로그만 찍고 계속 진행한다.
 *
 * **runtime PM 의 suspend 이자 시스템 절전의 suspend_noirq 로 둘 다
 * 등록되어 있다**(아래 tegra_pcie_pm_ops 참조). 그래서 런타임 절전과 시스템
 * 절전이 같은 코드를 탄다.
 *
 * 순서:
 *   1) 포트마다 PME_Turn_Off 를 보내 링크를 규격대로 내린다.
 *   2) LTSSM 을 멈추고 포트를 끈다.
 *   3) **AFI 오류 인터럽트를 마스크한다.** 위의 원문 주석이 이유를
 *      밝히는데, 이 인터럽트는 tegra_pcie_enable_controller 에서 켜지며
 *      아래에서 pex_rst 를 어서트하면 AFI 가 원치 않는 인터럽트를 올리기
 *      때문이다. 즉 4) 이후를 위한 사전 조치다.
 *   4) PHY 전원을 내린다(program_uphy 인 SoC 만).
 *   5) PEX 리셋을 어서트하고 PEX 클록을 끈다.
 *   6) MSI 인터럽트를 마스크한다.
 *   7) 핀을 idle 상태로 돌리고 컨트롤러 전원을 내린다.
 *
 * 3)이 4)~5)보다 먼저여야 한다는 순서 의존이 이 함수에서 가장 미묘한
 * 대목이며, 그래서 원문 주석이 붙어 있다.
 *
 * 실패해도 계속 진행하는 것이 되돌리는 경로의 관례다. PHY 전원 차단이
 * 실패해도 오류만 찍고 나머지를 마저 내린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. noirq 단계로도 불리므로 그때는
 * 인터럽트가 꺼져 있다.
 *
 * 호출 체인:
 *   PM 코어 → tegra_pcie_pm_ops → [이 함수]
 *     → tegra_pcie_pme_turnoff, tegra_pcie_disable_ports,
 *       tegra_pcie_disable_interrupts, tegra_pcie_phy_power_off,
 *       tegra_pcie_disable_msi, tegra_pcie_power_off
 */
static int tegra_pcie_pm_suspend(struct device *dev)
{
	struct tegra_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] 순회할 포트 */
	struct tegra_pcie_port *port;
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 포트마다 PME_Turn_Off 를 보내 링크를 규격대로 내린다.
	 * 전원을 그냥 끊으면 상대가 링크 단절을 오류로 볼 수 있기 때문이다 */
	list_for_each_entry(port, &pcie->ports, list)
		tegra_pcie_pme_turnoff(port);

	tegra_pcie_disable_ports(pcie);

	/*
	 * AFI_INTR is unmasked in tegra_pcie_enable_controller(), mask it to
	 * avoid unwanted interrupts raised by AFI after pex_rst is asserted.
	 */
	tegra_pcie_disable_interrupts(pcie);

	/* [한국어] PHY 를 이 드라이버가 다루는 SoC 에서만 한다 */
	if (pcie->soc->program_uphy) {
		/* [한국어] PHY 전원을 내린다 */
		err = tegra_pcie_phy_power_off(pcie);
		/* [한국어] 실패해도 나머지를 마저 내린다 */
		if (err < 0)
			/* [한국어] PHY 전원 차단 실패. 되돌리는 경로라 로그만 찍고 계속 진행한다 */
			dev_err(dev, "failed to power off PHY(s): %d\n", err);
	}

	reset_control_assert(pcie->pex_rst);
	clk_disable_unprepare(pcie->pex_clk);

	/* [한국어] MSI 인터럽트를 마스크한다. 도메인과 비트맵은 그대로 둔다 --
	 * 깨어나면 같은 벡터를 계속 써야 하기 때문이다 */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		tegra_pcie_disable_msi(pcie);

	pinctrl_pm_select_idle_state(dev);
	tegra_pcie_power_off(pcie);

	return 0;
}

/* [한국어]
 * tegra_pcie_pm_resume - 컨트롤러를 기동한다. probe 와 복귀가 공유하는 경로
 *
 * @dev: 이 컨트롤러의 device.
 * @return: 0 성공, 음수 오류.
 *
 * **이 드라이버의 하드웨어 기동 전체가 이 함수에 있다.** probe 가
 * pm_runtime_get_sync 로 이 함수를 부르고, 시스템 복귀도 같은 함수를
 * 탄다. 그래서 부팅 경로와 복귀 경로가 갈라지지 않는다 -- 다른 여러
 * 호스트 브리지 드라이버가 probe 와 resume 에 비슷한 코드를 두 벌 두는
 * 것과 대비되는 구조다.
 *
 * 순서:
 *   1) 전원을 올린다(regulator, 전원 게이트, 클록, AFI 리셋 해제).
 *   2) 핀을 기본 상태로 돌린다. 오류 메시지가 "PCIe IO DPD 해제 실패"
 *      인 것으로 보아, idle 상태에서 핀이 DPD(deep power down)에 들어가
 *      있던 것을 푸는 의미다.
 *   3) 컨트롤러 전역 설정(레인 배분, 포트 활성화, 오류 인터럽트).
 *   4) 주소 변환 창을 프로그래밍한다.
 *   5) MSI 를 켠다 -- 수신 주소, 마스크 복원, 인터럽트 활성화.
 *   6) PEX 클록을 켜고 PEX 리셋을 푼다.
 *   7) PHY 전원을 켠다(program_uphy 인 SoC 만).
 *   8) 참조 클록 패드 설정을 쓴다.
 *   9) 포트를 켜고 링크를 세운다. 여기서 pcie_xrst 가 풀려 LTSSM 이 돈다.
 *
 * 리셋 셋의 해제 시점이 여기서 모두 드러난다 -- afi_rst 는 1)에서,
 * pex_rst 는 6)에서, pcie_xrst 는 9) 안에서 풀린다.
 *
 * 정리 경로가 세 라벨로 층져 있다. 각 라벨은 그 직전까지 성공한 것만
 * 되돌린다. 다만 5)까지 진행한 뒤 6)에서 실패하면 MSI 설정이 켜진 채
 * 남는데, poweroff 라벨이 전원을 내리므로 결과적으로 무의미해진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 9)에서 링크를 기다리며 오래 잠들 수 있다.
 *
 * 호출 체인:
 *   PM 코어 / pm_runtime_get_sync(probe) → [이 함수]
 *     → tegra_pcie_power_on, tegra_pcie_enable_controller,
 *       tegra_pcie_setup_translations, tegra_pcie_enable_msi,
 *       tegra_pcie_phy_power_on, tegra_pcie_apply_pad_settings,
 *       tegra_pcie_enable_ports
 */
static int tegra_pcie_pm_resume(struct device *dev)
{
	struct tegra_pcie *pcie = dev_get_drvdata(dev);
	/* [한국어] 오류 코드 보관용 */
	int err;

	/* [한국어] 전원 인가의 첫 단계. AFI 리셋만 여기서 풀리고 나머지 둘은 나중이다 */
	err = tegra_pcie_power_on(pcie);
	/* [한국어] regulator, 전원 게이트, 클록, AFI 리셋 해제가 모두 실패할 수 있다 */
	if (err) {
		/* [한국어] 전원 인가 실패. 여기서 실패하면 되돌릴 것이 없어 그대로 반환한다 */
		dev_err(dev, "tegra pcie power on fail: %d\n", err);
		return err;
	}

	/* [한국어] 핀을 기본 상태로 돌린다. 절전에서 idle 상태로 보냈던 것의 짝이다 */
	err = pinctrl_pm_select_default_state(dev);
	/* [한국어] 실패하면 전원을 되돌린다 */
	if (err < 0) {
		/* [한국어] 핀 설정 실패. 메시지가 "IO DPD 해제 실패" 인 것으로 보아 idle 상태에서
		 * 핀이 깊은 절전에 들어가 있던 것을 푸는 의미다 */
		dev_err(dev, "failed to disable PCIe IO DPD: %d\n", err);
		goto poweroff;
	}

	tegra_pcie_enable_controller(pcie);
	tegra_pcie_setup_translations(pcie);

	/* [한국어] MSI 를 켠다. 수신 주소를 다시 깔고, 비트맵으로부터 활성화 상태를
	 * 재구성하고, 인터럽트 마스크를 연다 */
	if (IS_ENABLED(CONFIG_PCI_MSI))
		tegra_pcie_enable_msi(pcie);

	/* [한국어] PEX 클록을 켠다. tegra_pcie_power_on 이 다루지 않고 남겨 둔 클록이며,
	 * 바로 아래에서 PEX 리셋을 푸는 것과 짝을 이룬다 */
	err = clk_prepare_enable(pcie->pex_clk);
	/* [한국어] 실패하면 핀을 idle 로 돌리고 전원을 내린다 */
	if (err) {
		/* [한국어] PEX 클록을 못 켜면 링크를 세울 수 없다 */
		dev_err(dev, "failed to enable PEX clock: %d\n", err);
		goto pex_dpd_enable;
	}

	reset_control_deassert(pcie->pex_rst);

	/* [한국어] PHY 를 이 드라이버가 다루는 SoC 에서만 한다. Tegra186 은 건너뛴다 */
	if (pcie->soc->program_uphy) {
		/* [한국어] PHY 전원을 켠다. 구형/신형 갈래는 그 함수가 가른다 */
		err = tegra_pcie_phy_power_on(pcie);
		/* [한국어] 실패하면 PEX 클록과 리셋까지 되감는다 */
		if (err < 0) {
			/* [한국어] PHY 전원 인가 실패 */
			dev_err(dev, "failed to power on PHY(s): %d\n", err);
			goto disable_pex_clk;
		}
	}

	tegra_pcie_apply_pad_settings(pcie);
	tegra_pcie_enable_ports(pcie);

	return 0;

disable_pex_clk:
	reset_control_assert(pcie->pex_rst);
	clk_disable_unprepare(pcie->pex_clk);
pex_dpd_enable:
	pinctrl_pm_select_idle_state(dev);
poweroff:
	tegra_pcie_power_off(pcie);

	return err;
}

/* [한국어] **같은 함수 짝이 런타임 PM 과 시스템 절전 양쪽에 등록된다.**
 * 그래서 런타임 절전과 시스템 절전이 갈라지지 않고 한 경로를 탄다 */
static const struct dev_pm_ops tegra_pcie_pm_ops = {
	/* [한국어] 런타임 PM 콜백 등록. 세 번째 인자(idle)는 NULL 이다.
	 * **probe 가 pm_runtime_get_sync 로 이 resume 을 불러 하드웨어를 기동한다** --
	 * 이 파일 기동 구조의 핵심이다 */
	RUNTIME_PM_OPS(tegra_pcie_pm_suspend, tegra_pcie_pm_resume, NULL)
	/* [한국어] 시스템 절전에는 **noirq 단계** 로 붙는다. 그때는 자식 장치가 이미 잠들어
	 * 링크를 내려도 안전하고 인터럽트가 꺼져 있다 */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(tegra_pcie_pm_suspend, tegra_pcie_pm_resume)
};

/* [한국어] 플랫폼 드라이버 정의. 아래 builtin_platform_driver 로 등록되므로
 * 모듈로 뺄 수 없고 커널에 내장된다 */
static struct platform_driver tegra_pcie_driver = {
	/* [한국어] 드라이버 속성. suppress_bind_attrs 가 true 라 sysfs 로 수동 언바인드를
	 * 막는다 -- 그래서 이 드라이버는 사실상 해제되지 않으며,
	 * tegra_pcie_debugfs_init 이 만든 디렉터리를 정리하지 않아도 문제가 되지 않는다 */
	.driver = {
		/* [한국어] 드라이버 이름. sysfs 와 로그에 이 이름으로 나타난다 */
		.name = "tegra-pcie",
		.of_match_table = tegra_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = &tegra_pcie_pm_ops,
	},
	.probe = tegra_pcie_probe,
};
builtin_platform_driver(tegra_pcie_driver);
MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA PCI host controller driver");
MODULE_LICENSE("GPL");
