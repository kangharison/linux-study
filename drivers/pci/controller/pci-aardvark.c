// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Aardvark PCIe controller, used on Marvell Armada
 * 3700.
 *
 * Copyright (C) 2016 Marvell
 *
 * Author: Hezi Shahmoon <hezi.shahmoon@marvell.com>
 */

/*
 * [한국어 설명] Marvell Armada 3700 의 Aardvark PCIe 컨트롤러 드라이버 (pci-aardvark.c)
 *
 * === 파일의 역할 ===
 * Marvell 이 직접 설계한 PCIe 컨트롤러 IP("Aardvark")를 리눅스에 물리는
 * 드라이버다. 앞서 본 pci-imx6.c 같은 DesignWare 글루와 달리 **공용
 * 코어가 없다** — config 공간 접근, MSI, INTx, 아웃바운드 창, 링크 훈련,
 * 루트 포트 자신의 config 헤더까지 전부 이 파일 하나가 구현한다.
 * 그래서 파일이 하는 일의 폭이 넓고, 하드웨어 레지스터 정의가 파일
 * 앞부분의 3분의 1 가까이를 차지한다.
 * 특히 두 가지가 이 컨트롤러의 성격을 정한다. 하나는 config 접근이
 * ECAM 창 매핑이 아니라 **PIO(Programmed I/O) 레지스터 시퀀스**라는 것,
 * 다른 하나는 **루트 포트의 type-1 config 헤더가 하드웨어에 없어 소프트웨어로
 * 흉내 낸다**는 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DWC 계열의 두 층 구조와 대비된다.
 *
 *   [PCI 코어]  drivers/pci/probe.c, setup-bus.c, msi/ ...
 *        ^  struct pci_ops (read/write), irq_domain, msi_domain
 *   [이 파일]   pci-aardvark.c  — 공용 컨트롤러 층이 없다
 *        ^  advk_writel/advk_readl 로 레지스터 창에 직접
 *   [Aardvark 하드웨어] PIO 블록, 아웃바운드 창, 인터럽트 블록, PHY
 *
 * 위층과 맞닿는 지점이 넷이다.
 *   struct pci_ops advk_pcie_ops — config 읽기·쓰기. PCI 코어가 장치를
 *       열거할 때마다 여기로 들어온다.
 *   MSI irq_domain(msi_inner_domain) — 벡터 32개를 비트맵으로 나눠 준다.
 *   INTx irq_domain(irq_domain) — 레거시 인터럽트 넷을 중계한다.
 *   루트 포트 irq_domain(rp_irq_domain) — 루트 포트 자신이 내는
 *       인터럽트(PME 등)를 에뮬레이션된 브리지에 전달한다.
 * 아래로는 clk 대신 phy 서브시스템과 reset GPIO 만 쓴다.
 *
 * 실행 컨텍스트: probe/remove 와 config 접근은 프로세스 컨텍스트이고,
 * advk_pcie_irq_handler() 이하는 **인터럽트 컨텍스트**다. 그 둘이 같은
 * 레지스터를 만지므로 raw_spinlock 두 개(irq_lock, msi_irq_lock)와
 * 뮤텍스 하나(msi_used_lock)로 나눠 지킨다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 것:
 *   - ../pci-bridge-emul.h — **루트 포트의 config 헤더를 소프트웨어로
 *     흉내 내는 공용 코드.** 이 하드웨어에는 루트 포트용 type-1 헤더가
 *     없어서, PCI 코어가 0번 버스의 0번 장치를 읽으려 하면 이 에뮬레이션이
 *     답한다. 어떤 필드는 에뮬레이션 층이 직접 처리하고, 어떤 필드는
 *     이 파일의 콜백 여섯 개가 실제 하드웨어 레지스터로 옮겨 준다.
 *   - ../pci.h — PCI 코어 내부 전용 헤더.
 *   - linux/pci-ecam.h — PCIE_ECAM_OFFSET() 매크로. **주소 계산 방식만
 *     ECAM 과 같고**, 실제 접근은 그 주소를 PIO 레지스터에 실어 보낸다.
 *   - linux/irqchip/irq-msi-lib.h, linux/irqdomain.h, linux/msi.h —
 *     MSI 와 INTx 도메인을 직접 만든다.
 *   - phy 서브시스템, reset GPIO, 그리고 장치 트리 주소·PCI 헬퍼.
 * 이 파일에 의존하는 것: 없다. 심볼을 내보내지 않는 말단 플랫폼
 * 드라이버다(전수 grep 확인).
 *
 * === 주요 함수/구조체 요약 ===
 * advk_pcie_probe()          : 진입점. 자원·인터럽트·도메인을 세운다.
 * advk_pcie_setup_hw()       : 컨트롤러 레지스터를 초기 상태로 만든다.
 * advk_pcie_train_link()     : 세대를 정하고 PERST# 를 흔들고 링크를 기다린다.
 * advk_pcie_rd_conf/wr_conf(): PIO 시퀀스로 config 공간을 읽고 쓴다.
 * advk_pcie_check_pio_status(): PIO 완료 상태를 규격에 맞게 해석한다.
 * advk_sw_pci_bridge_init()  : 에뮬레이션 루트 포트를 세운다.
 * advk_pcie_handle_int()     : 하나의 인터럽트를 종류별로 나눠 보낸다.
 * advk_pcie_set_ob_win()     : 아웃바운드 주소 창 하나를 프로그래밍한다.
 * struct advk_pcie           : 이 드라이버의 인스턴스 상태 전부.
 *
 * === config 접근이 PIO 인 이유와 그 대가 ===
 * ECAM 을 쓰는 컨트롤러는 (버스, 장치, 함수, 오프셋)을 주소 비트로 바꾼
 * 메모리 창을 두어, CPU 가 그 주소를 읽고 쓰는 것만으로 config 트랜잭션이
 * 나간다. Aardvark 에는 그런 창이 없다. 대신 레지스터 다섯 벌을 차례로
 * 채우고 시작 비트를 세운 뒤 완료를 기다려야 한다.
 *   PIO_CTRL         트랜잭션 종류(TYPE0/TYPE1, 읽기/쓰기)
 *   PIO_ADDR_LS/MS   ECAM 방식으로 계산한 주소(4바이트 정렬)
 *   PIO_WR_DATA      쓸 값
 *   PIO_WR_DATA_STRB 그 값 중 어느 바이트가 유효한지(바이트 인에이블)
 *   PIO_START        1 을 쓰면 트랜잭션이 나간다
 *   PIO_ISR          완료 표시. 쓰기 전에 1 을 써서 지운다
 *   PIO_STAT         완료 상태(성공/UR/RRS/CA)와 오류 비트
 *   PIO_RD_DATA      읽은 값
 * 대가가 셋이다.
 *   1. **한 번에 하나만 처리된다.** advk_pcie_pio_is_running() 이 앞의
 *      트랜잭션이 아직 도는지 확인하고, 돌고 있으면 새 요청을 내지 않는다.
 *   2. **바이트 단위 접근을 소프트웨어가 만든다.** 읽기는 4바이트를 받아
 *      시프트·마스크로 잘라 내고, 쓰기는 strobe 로 유효 바이트를 알린다.
 *   3. **완료를 폴링으로 기다린다.** advk_pcie_wait_pio() 가 2us 간격으로
 *      최대 750000회(약 1.5초) 본다.
 *
 * === PIO 완료 상태를 읽는 규칙 ===
 * advk_pcie_check_pio_status() 안의 상류 주석이 하드웨어 명세의 다섯
 * 규칙을 그대로 옮겨 두었다. 요지는 완료 상태 필드만으로는 성패를 알 수
 * 없다는 것이다.
 *   - 상태가 성공(OK)이어도 오류 비트(PIO_ERR_STATUS)를 함께 봐야 한다.
 *   - UR(Unsupported Request)은 **쓰기에서만 오류**이고, 읽기에서는
 *     0xFFFFFFFF 를 읽은 정상 결과다.
 *   - RRS(Configuration Request Retry Status)도 쓰기에서만 오류이고,
 *     읽기에서는 0xFFFF0001 을 읽은 정상 결과다. 다만 그것이 정상으로
 *     인정되려면 루트 포트에서 RRS Software Visibility 가 켜져 있고
 *     읽는 대상이 Vendor ID 두 바이트를 포함해야 한다 — 그 조건을
 *     rd_conf 의 allow_rrs 가 계산한다. 켜져 있지 않으면 -EAGAIN 을
 *     돌려 호출자가 요청을 다시 내게 한다.
 *   - CA(Completer Abort)는 읽기·쓰기 모두 오류다.
 *
 * === 루트 포트를 소프트웨어로 흉내 내는 이유 ===
 * PCI 코어는 호스트 브리지 아래에 반드시 type-1 config 헤더를 가진
 * 브리지가 있다고 보고 열거를 시작한다. Aardvark 하드웨어에는 그 헤더가
 * 없어서, pci_bridge_emul 이 그 자리를 채운다. 이 파일이 하는 일은
 * 읽기·쓰기 콜백 여섯 개를 채워, 에뮬레이션 층이 다룰 수 없는 필드를
 * 실제 하드웨어 레지스터로 옮기는 것이다.
 *   base_conf_read/write — 명령·상태 레지스터와 BAR, 인터럽트 라인 등
 *   pcie_conf_read/write — PCI Express capability(링크 제어·상태 등)
 *   ext_conf_read/write  — 확장 capability(AER 등)
 * 그 결과 사용자에게는 평범한 루트 포트로 보이지만, 그 뒤에서 어떤
 * 필드는 소프트웨어 값이고 어떤 필드는 실제 레지스터다.
 *
 * === 인터럽트가 하나로 들어와 넷으로 갈라지는 구조 ===
 * 이 컨트롤러는 GIC 에 **요약 인터럽트 하나**만 낸다. 그래서 종류를
 * 가려 나눠 보내는 일을 소프트웨어가 한다.
 *
 *   GIC SPI 하나
 *     -> advk_pcie_irq_handler()   상위 상태 레지스터를 확인
 *       -> advk_pcie_handle_int()  아래 넷으로 나눈다
 *          - PIO 완료          : 폴링하는 쪽이 볼 수 있게 표시만 남긴다
 *          - MSI               : advk_pcie_handle_msi() -> 32개 중 어느 것인지
 *          - INTx(4개)         : irq_domain 을 거쳐 장치 드라이버로
 *          - PME               : advk_pcie_handle_pme() -> 에뮬레이션 브리지
 *
 * MSI 는 벡터가 32개(MSI_IRQ_NUM)뿐이라 비트맵으로 관리한다. 할당·해제는
 * 프로세스 컨텍스트라 뮤텍스(msi_used_lock)로, 마스크·언마스크는
 * 인터럽트 컨텍스트에서도 불려 raw_spinlock(msi_irq_lock)으로 지킨다.
 * INTx 쪽도 같은 이유로 irq_lock 을 따로 둔다.
 *
 * MSI 주소가 특이하다 — advk_msi_irq_compose_msi_msg() 가
 * virt_to_phys(pcie) 로 **드라이버 구조체 자신의 물리 주소**를 MSI 목적지로
 * 쓴다. 실제로 그 메모리에 쓰이는 것이 아니라, 컨트롤러가 그 주소로 가는
 * 쓰기를 가로채 MSI 로 해석하도록 setup_hw 에서 등록해 두기 때문이다.
 *
 * === 아웃바운드 창 ===
 * CPU 쪽 주소를 PCIe 쪽 주소로 옮기는 창이 OB_WIN_COUNT 개 있다.
 * setup_hw 의 상류 주석이 밝히듯 **메모리 접근은 기본 창 설정으로 투명하게
 * 처리**되므로 창을 따로 잡을 필요가 없고, I/O 처럼 메모리가 아닌 접근이나
 * 투명하지 않은 변환이 필요할 때만 창을 쓴다. probe 가 장치 트리의
 * ranges 를 읽어 필요한 것만 pcie->wins[] 에 채우고, 나머지 창은
 * advk_pcie_disable_ob_win() 으로 꺼 둔다.
 *
 * === 이 파일에서 눈에 띄는 것들 ===
 * 상류 주석과 코드가 스스로 밝히는 것만 적는다.
 *   - **링크 훈련 전에 PERST# 를 흔든다.** advk_pcie_train_link() 의
 *     상류 주석이 이유를 밝힌다 — 어떤 카드는 초기 상태가 아닐 때 링크
 *     훈련에서 검출되지 않는다. 그리고 규격(PCIe Base Spec 4.0, 6.6.1)이
 *     그런 리셋 뒤 config 요청까지 최소 100ms 를 요구하므로, 링크가
 *     설 때까지(최소 900ms) 기다리는 것으로 그 요구를 함께 채운다.
 *   - 링크 속도 설정을 두 곳에 쓴다. 상류 주석이 이유를 적는다 —
 *     Armada 3700 기능 명세는 Link Control 2 의 기본값이 SPEED_GEN 을
 *     따른다고 하지만 실제로는 늘 8.0GT/s 였다는 것이다.
 *   - PIO 재시도 상한이 PIO_RETRY_CNT(750000) x PIO_RETRY_DELAY(2us)로
 *     약 1.5초다. RRS 재시도도 같은 상한을 공유한다.
 *   - advk_pcie_wait_pio() 는 성공 시 **몇 번 만에 끝났는지**를 돌려주고,
 *     호출자가 그것을 재시도 횟수에 누적한다.
 *
 * === DesignWare 글루와의 대비 ===
 * 같은 디렉터리의 pci-imx6.c 와 나란히 놓으면 차이가 분명하다.
 *   config 접근  : imx6 는 DWC 코어가 iATU 창을 잡아 처리 / 여기는 PIO 시퀀스
 *   루트 포트    : imx6 는 하드웨어에 실제 헤더가 있음 / 여기는 소프트웨어 흉내
 *   MSI          : imx6 는 DWC 내장 컨트롤러나 ITS / 여기는 자체 도메인 32벡터
 *   링크 훈련    : imx6 는 LTSSM 비트를 켜고 DWC 코어가 대기 / 여기는 전부 자체
 *   세대 차이    : imx6 는 drvdata 표로 SoC 세대를 가름 / 여기는 칩이 하나뿐
 * 즉 imx6 는 "공용 코어에 SoC 사정을 붙이는" 파일이고, 이 파일은
 * "컨트롤러 전체를 혼자 구현하는" 파일이다.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 다만 Armada 3700 보드(예: EspressoBin, Turris Mox)에 M.2 NVMe SSD 를
 * 물리면 그 SSD 가 보이기까지의 경로가 이 파일이다. 특히 **NVMe 드라이버가
 * 쓰는 MSI-X 벡터가 이 컨트롤러에서는 32개 상한에 걸린다** — 이 파일의
 * MSI 도메인이 MSI_IRQ_NUM 개만 나눠 주므로, 큐를 CPU 수만큼 만들려는
 * NVMe 초기화가 그 상한에서 벡터를 덜 받게 된다. 또 config 접근이 PIO
 * 폴링이라 열거가 ECAM 컨트롤러보다 느리고, SSD 가 응답하지 않을 때의
 * RRS 재시도가 최대 1.5초까지 늘어날 수 있다.
 */

#include <linux/bitfield.h>	/* [한국어] FIELD_PREP/FIELD_GET/GENMASK — 이 파일의 레지스터 필드 조작이 전부 이 매크로로 이루어진다 */
#include <linux/delay.h>	/* [한국어] udelay/usleep_range — PIO 폴링과 링크 대기에 쓴다 */
#include <linux/gpio/consumer.h>	/* [한국어] gpiod_* — PERST# 리셋 신호를 GPIO 로 다룬다 */
#include <linux/interrupt.h>	/* [한국어] devm_request_irq, IRQF_SHARED 등. 요약 인터럽트 하나를 등록한다 */
#include <linux/irq.h>	/* [한국어] irq_set_chip_and_handler, handle_level_irq 등 IRQ 코어 API */
#include <linux/irqchip/irq-msi-lib.h>	/* [한국어] msi_lib_init_dev_msi_info — MSI 부모 도메인을 만들 때 쓰는 공용 헬퍼 */
#include <linux/irqdomain.h>	/* [한국어] irq_domain_create_linear 등. **이 파일은 도메인을 셋 만든다** */
#include <linux/kernel.h>	/* [한국어] 커널 일반 정의 */
#include <linux/module.h>	/* [한국어] 모듈 뼈대 */
#include <linux/pci.h>	/* [한국어] struct pci_ops, PCI_EXP_* 규격 상수, pci_host_probe() 등 */
#include <linux/pci-ecam.h>	/* [한국어] **PCIE_ECAM_OFFSET() 매크로.** 주소 계산 방식만 ECAM 과 같고 접근은 PIO 다 */
#include <linux/init.h>	/* [한국어] __init 표시 */
#include <linux/phy/phy.h>	/* [한국어] phy_init/phy_power_on/phy_set_mode 등 generic PHY API */
#include <linux/platform_device.h>	/* [한국어] 플랫폼 드라이버 뼈대 */
#include <linux/msi.h>	/* [한국어] struct msi_msg, msi_create_parent_irq_domain 등 MSI API */
#include <linux/of_address.h>	/* [한국어] of_address 헬퍼 */
#include <linux/of_pci.h>	/* [한국어] of_pci_get_max_link_speed, of_irq_parse_and_map_pci 등 */

#include "../pci.h"	/* [한국어] drivers/pci 안쪽 전용 헤더 */
#include "../pci-bridge-emul.h"	/* [한국어] **루트 포트 config 헤더를 소프트웨어로 흉내 내는 공용 코드.** 이 하드웨어에는 규격에 맞는 Type 1 헤더가 없어 반드시 필요하다 */

/* PCIe core registers */
#define PCIE_CORE_DEV_ID_REG					0x0	/* [한국어] 장치·벤더 ID. setup_hw 가 erratum 4.1 우회로 이 값을 바로잡는다 */
#define PCIE_CORE_CMD_STATUS_REG				0x4	/* [한국어] 명령·상태 레지스터. 에뮬레이션 브리지의 PCI_COMMAND 가 여기로 중계된다 */
#define PCIE_CORE_DEV_REV_REG					0x8	/* [한국어] 장치·개정 레지스터. 상위 24비트가 클래스 코드라 setup_hw 가 브리지 클래스로 바꾼다 */
#define PCIE_CORE_SSDEV_ID_REG					0x2c	/* [한국어] 서브시스템 장치·벤더 ID */
#define PCIE_CORE_PCIEXP_CAP					0xc0	/* [한국어] **PCI Express capability 의 시작 오프셋.** LNKCTL/DEVCTL 등을 여기 기준으로 읽고 쓴다 */
#define PCIE_CORE_PCIERR_CAP					0x100	/* [한국어] **AER capability 의 시작 오프셋.** 확장 config 콜백이 여기 기준으로 접근한다 */
#define PCIE_CORE_ERR_CAPCTL_REG				0x118	/* [한국어] AER 능력·제어 레지스터 */
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX			BIT(5)	/* [한국어] 송신 ECRC 검사 능력 */
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX_EN			BIT(6)	/* [한국어] 송신 ECRC 검사 활성화 */
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHCK			BIT(7)	/* [한국어] ECRC 검사 능력 */
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHCK_RCV			BIT(8)	/* [한국어] 수신 ECRC 검사 활성화. setup_hw 가 넷을 함께 켠다 */
/* PIO registers base address and register offsets */
#define PIO_BASE_ADDR				0x4000	/* [한국어] **PIO 블록의 시작 주소.** config 접근이 이 블록의 레지스터 시퀀스로 이루어진다 */
#define PIO_CTRL				(PIO_BASE_ADDR + 0x0)	/* [한국어] PIO 제어 — 트랜잭션 종류를 여기 적는다 */
#define   PIO_CTRL_TYPE_MASK			GENMASK(3, 0)	/* [한국어] 그 종류 필드. PCIE_CONFIG_RD/WR_TYPE0/1 중 하나가 들어간다 */
#define   PIO_CTRL_ADDR_WIN_DISABLE		BIT(24)	/* [한국어] **주소 창 매핑을 우회하는 비트.** setup_hw 의 상류 주석대로 PIO 는 자기 레지스터에 필요한 정보를 다 담으므로 창이 필요 없다 */
#define PIO_STAT				(PIO_BASE_ADDR + 0x4)	/* [한국어] PIO 완료 상태 레지스터 */
#define   PIO_COMPLETION_STATUS_SHIFT		7	/* [한국어] 완료 상태 필드의 비트 위치 */
#define   PIO_COMPLETION_STATUS_MASK		GENMASK(9, 7)	/* [한국어] 그 필드의 마스크(비트 9~7) */
#define   PIO_COMPLETION_STATUS_OK		0	/* [한국어] 완료 상태 — 성공. **다만 오류 비트를 함께 봐야 한다** */
#define   PIO_COMPLETION_STATUS_UR		1	/* [한국어] 완료 상태 — Unsupported Request. 쓰기에서만 오류이고 읽기에서는 전부 1 을 읽은 정상 결과다 */
#define   PIO_COMPLETION_STATUS_RRS		2	/* [한국어] 완료 상태 — Config Request Retry Status. 쓰기에서만 오류이며 읽기에서는 조건에 따라 정상 결과다 */
#define   PIO_COMPLETION_STATUS_CA		4	/* [한국어] 완료 상태 — Completer Abort. 읽기·쓰기 모두 오류다 */
#define   PIO_NON_POSTED_REQ			BIT(10)	/* [한국어] 이 트랜잭션이 non-posted 였는지. 오류 로그에 찍는다 */
#define   PIO_ERR_STATUS			BIT(11)	/* [한국어] **오류 비트.** 완료 상태가 성공이어도 이것이 서 있으면 실패다 */
#define PIO_ADDR_LS				(PIO_BASE_ADDR + 0x8)	/* [한국어] PIO 주소 하위 워드. ECAM 방식으로 계산한 주소를 여기 쓴다 */
#define PIO_ADDR_MS				(PIO_BASE_ADDR + 0xc)	/* [한국어] PIO 주소 상위 워드. 이 드라이버는 늘 0 을 쓴다 */
#define PIO_WR_DATA				(PIO_BASE_ADDR + 0x10)	/* [한국어] PIO 쓰기 데이터 */
#define PIO_WR_DATA_STRB			(PIO_BASE_ADDR + 0x14)	/* [한국어] **쓰기 바이트 인에이블.** PIO 는 4바이트 단위로 나가므로 어느 바이트가 유효한지 알려야 한다 */
#define PIO_RD_DATA				(PIO_BASE_ADDR + 0x18)	/* [한국어] PIO 읽기 데이터 */
#define PIO_START				(PIO_BASE_ADDR + 0x1c)	/* [한국어] **시작 비트.** 1 을 쓰면 트랜잭션이 나가고 완료되면 하드웨어가 스스로 지운다 */
#define PIO_ISR					(PIO_BASE_ADDR + 0x20)	/* [한국어] PIO 완료 인터럽트 상태. 시작 전에 1 을 써서 지운다 */
#define PIO_ISRM				(PIO_BASE_ADDR + 0x24)	/* [한국어] PIO 완료 인터럽트 마스크. 이 드라이버는 쓰지 않는다(전수 grep 확인) */

/* Aardvark Control registers */
#define CONTROL_BASE_ADDR			0x4800	/* [한국어] **컨트롤러 제어 블록의 시작 주소** */
#define PCIE_CORE_CTRL0_REG			(CONTROL_BASE_ADDR + 0x0)	/* [한국어] 제어 레지스터 0 — 세대·모드·레인·링크 훈련이 모두 여기 있다 */
#define     PCIE_GEN_SEL_MSK			0x3	/* [한국어] 세대 선택 필드의 마스크 */
#define     PCIE_GEN_SEL_SHIFT			0x0	/* [한국어] 그 필드의 비트 위치 */
#define     SPEED_GEN_1				0	/* [한국어] 세대 값 — Gen1(2.5GT/s) */
#define     SPEED_GEN_2				1	/* [한국어] 세대 값 — Gen2(5.0GT/s) */
#define     SPEED_GEN_3				2	/* [한국어] 세대 값 — Gen3(8.0GT/s) */
#define     IS_RC_MSK				1	/* [한국어] 루트 컴플렉스 모드 비트의 마스크 */
#define     IS_RC_SHIFT				2	/* [한국어] 그 비트의 위치 */
#define     LANE_CNT_MSK			0x18	/* [한국어] 레인 수 필드의 마스크 */
#define     LANE_CNT_SHIFT			0x3	/* [한국어] 그 필드의 비트 위치 */
#define     LANE_COUNT_1			(0 << LANE_CNT_SHIFT)	/* [한국어] 레인 1개. **setup_hw 가 이 값으로 고정한다** */
#define     LANE_COUNT_2			(1 << LANE_CNT_SHIFT)	/* [한국어] 레인 2개. 이 드라이버는 쓰지 않는다 */
#define     LANE_COUNT_4			(2 << LANE_CNT_SHIFT)	/* [한국어] 레인 4개. 이 드라이버는 쓰지 않는다 */
#define     LANE_COUNT_8			(3 << LANE_CNT_SHIFT)	/* [한국어] 레인 8개. 이 드라이버는 쓰지 않는다 */
#define     LINK_TRAINING_EN			BIT(6)	/* [한국어] **링크 훈련 활성화 비트.** train_link 가 켜고 remove 가 끈다 */
#define     LEGACY_INTA				BIT(28)	/* [한국어] 레거시 INTA 관련 비트. 이 파일에서 읽는 곳은 없다(전수 grep 확인) */
#define     LEGACY_INTB				BIT(29)	/* [한국어] INTB. 마찬가지로 쓰이지 않는다 */
#define     LEGACY_INTC				BIT(30)	/* [한국어] INTC. 마찬가지로 쓰이지 않는다 */
#define     LEGACY_INTD				BIT(31)	/* [한국어] INTD. 마찬가지로 쓰이지 않는다 */
#define PCIE_CORE_CTRL1_REG			(CONTROL_BASE_ADDR + 0x4)	/* [한국어] 제어 레지스터 1 */
#define     HOT_RESET_GEN			BIT(0)	/* [한국어] **핫 리셋 발생 비트.** 에뮬레이션 브리지의 Bridge Control BUS_RESET 이 여기로 중계된다 */
#define PCIE_CORE_CTRL2_REG			(CONTROL_BASE_ADDR + 0x8)	/* [한국어] 제어 레지스터 2 */
#define     PCIE_CORE_CTRL2_RESERVED		0x7	/* [한국어] 예약 비트들. setup_hw 가 함께 써 준다 */
#define     PCIE_CORE_CTRL2_TD_ENABLE		BIT(4)	/* [한국어] TD(TLP 다이제스트) 활성화 */
#define     PCIE_CORE_CTRL2_STRICT_ORDER_ENABLE	BIT(5)	/* [한국어] 엄격한 순서 강제. setup_hw 가 이 비트를 빼고 써서 끈다 */
#define     PCIE_CORE_CTRL2_OB_WIN_ENABLE	BIT(6)	/* [한국어] **아웃바운드 창 활성화.** 켜면 기본 창 설정으로 투명한 주소 변환이 이루어진다 */
#define     PCIE_CORE_CTRL2_MSI_ENABLE		BIT(10)	/* [한국어] MSI 활성화. remove 가 끈다 */
#define PCIE_CORE_REF_CLK_REG			(CONTROL_BASE_ADDR + 0x14)	/* [한국어] 레퍼런스 클럭 제어 레지스터 */
#define     PCIE_CORE_REF_CLK_TX_ENABLE		BIT(1)	/* [한국어] 클럭 송신 활성화. 컨트롤러가 카드로 내보내는 방향이라 켠다 */
#define     PCIE_CORE_REF_CLK_RX_ENABLE		BIT(2)	/* [한국어] 클럭 수신 활성화. 같은 이유로 끈다 */
#define PCIE_MSG_LOG_REG			(CONTROL_BASE_ADDR + 0x30)	/* [한국어] **마지막으로 들어온 인바운드 메시지의 로그.** PME 요청자 ID 를 여기서 꺼낸다 */
#define PCIE_ISR0_REG				(CONTROL_BASE_ADDR + 0x40)	/* [한국어] 인터럽트 상태 0 — PME·오류·MSI 대기가 들어 있다 */
#define PCIE_MSG_PM_PME_MASK			BIT(7)	/* [한국어] 그 안의 PM_PME 메시지 비트 */
#define PCIE_ISR0_MASK_REG			(CONTROL_BASE_ADDR + 0x44)	/* [한국어] 인터럽트 마스크 0 */
#define     PCIE_ISR0_MSI_INT_PENDING		BIT(24)	/* [한국어] MSI 대기 비트. 서 있으면 MSI 상태 레지스터를 봐야 한다 */
#define     PCIE_ISR0_CORR_ERR			BIT(11)	/* [한국어] 정정 가능 오류 */
#define     PCIE_ISR0_NFAT_ERR			BIT(12)	/* [한국어] 치명적이지 않은 오류 */
#define     PCIE_ISR0_FAT_ERR			BIT(13)	/* [한국어] 치명적 오류 */
#define     PCIE_ISR0_ERR_MASK			GENMASK(13, 11)	/* [한국어] 위 세 오류를 묶은 마스크. **에뮬레이션 브리지의 SERR 비트가 이 마스크를 뒤집어 만든다** */
#define     PCIE_ISR0_INTX_ASSERT(val)		BIT(16 + (val))	/* [한국어] INTx 어서트 비트(ISR0 쪽). 이 드라이버는 ISR1 쪽을 쓴다 */
#define     PCIE_ISR0_INTX_DEASSERT(val)	BIT(20 + (val))	/* [한국어] INTx 디어서트 비트. 이 파일에서 읽는 곳은 없다(전수 grep 확인) */
#define     PCIE_ISR0_ALL_MASK			GENMASK(31, 0)	/* [한국어] ISR0 전체 비트. remove 가 한꺼번에 마스크하고 지울 때 쓴다 */
#define PCIE_ISR1_REG				(CONTROL_BASE_ADDR + 0x48)	/* [한국어] 인터럽트 상태 1 — INTx 넷이 들어 있다 */
#define PCIE_ISR1_MASK_REG			(CONTROL_BASE_ADDR + 0x4C)	/* [한국어] 인터럽트 마스크 1. **INTx 마스크·언마스크가 이 레지스터를 만진다** */
#define     PCIE_ISR1_POWER_STATE_CHANGE	BIT(4)	/* [한국어] 전원 상태 변화. 이 파일에서 읽는 곳은 없다(전수 grep 확인) */
#define     PCIE_ISR1_FLUSH			BIT(5)	/* [한국어] 플러시. 마찬가지로 쓰이지 않는다 */
#define     PCIE_ISR1_INTX_ASSERT(val)		BIT(8 + (val))	/* [한국어] **INTx 어서트 비트(val=0~3 이 INTA~INTD).** 마스크와 상태 양쪽에서 쓴다 */
#define     PCIE_ISR1_ALL_MASK			GENMASK(31, 0)	/* [한국어] ISR1 전체 비트 */
#define PCIE_MSI_ADDR_LOW_REG			(CONTROL_BASE_ADDR + 0x50)	/* [한국어] **MSI 목적지 주소 하위 워드.** setup_hw 가 드라이버 구조체의 물리 주소를 여기 등록한다 */
#define PCIE_MSI_ADDR_HIGH_REG			(CONTROL_BASE_ADDR + 0x54)	/* [한국어] 그 상위 워드 */
#define PCIE_MSI_STATUS_REG			(CONTROL_BASE_ADDR + 0x58)	/* [한국어] MSI 상태 — 32비트가 벡터 32개에 대응한다 */
#define PCIE_MSI_MASK_REG			(CONTROL_BASE_ADDR + 0x5C)	/* [한국어] MSI 마스크 — 같은 대응이다 */
#define     PCIE_MSI_ALL_MASK			GENMASK(31, 0)	/* [한국어] MSI 전체 비트 */
#define PCIE_MSI_PAYLOAD_REG			(CONTROL_BASE_ADDR + 0x9C)	/* [한국어] MSI 페이로드. 이 파일에서 읽는 곳은 없다(전수 grep 확인) */
#define     PCIE_MSI_DATA_MASK			GENMASK(15, 0)	/* [한국어] 그 안의 데이터 필드. 마찬가지로 쓰이지 않는다 */

/* PCIe window configuration */
#define OB_WIN_BASE_ADDR			0x4c00	/* [한국어] **아웃바운드 창 블록의 시작 주소** */
#define OB_WIN_BLOCK_SIZE			0x20	/* [한국어] 창 하나가 차지하는 레지스터 블록의 크기(32바이트) */
#define OB_WIN_COUNT				8	/* [한국어] **창 개수. 이 값이 곧 다룰 수 있는 비투명 영역 수의 상한이다** */
/* [한국어] 창 번호와 오프셋으로 그 창의 레지스터 주소를 만든다.
 * 창 하나가 OB_WIN_BLOCK_SIZE(32바이트)씩 차지하므로 창 번호에 그것을
 * 곱하고 블록 안의 오프셋을 더한다. 줄 잇기 백슬래시로 세 줄에 걸쳐
 * 있어 각 줄에 끝 주석을 붙일 수 없다. */
#define OB_WIN_REG_ADDR(win, offset)		(OB_WIN_BASE_ADDR + \
						 OB_WIN_BLOCK_SIZE * (win) + \
						 (offset))	/* [한국어] 그 안의 오프셋을 더한다 */
#define OB_WIN_MATCH_LS(win)			OB_WIN_REG_ADDR(win, 0x00)	/* [한국어] 창의 match 하위 워드 */
#define     OB_WIN_ENABLE			BIT(0)	/* [한국어] **그 워드에 함께 실리는 활성화 비트.** 값 설정과 켜기를 한 번에 한다 */
#define OB_WIN_MATCH_MS(win)			OB_WIN_REG_ADDR(win, 0x04)	/* [한국어] match 상위 워드 */
#define OB_WIN_REMAP_LS(win)			OB_WIN_REG_ADDR(win, 0x08)	/* [한국어] remap 하위 워드 */
#define OB_WIN_REMAP_MS(win)			OB_WIN_REG_ADDR(win, 0x0c)	/* [한국어] remap 상위 워드 */
#define OB_WIN_MASK_LS(win)			OB_WIN_REG_ADDR(win, 0x10)	/* [한국어] mask 하위 워드. **하위 16비트가 0 이어야 해서 창 최소 크기가 64KiB 다** */
#define OB_WIN_MASK_MS(win)			OB_WIN_REG_ADDR(win, 0x14)	/* [한국어] mask 상위 워드 */
#define OB_WIN_ACTIONS(win)			OB_WIN_REG_ADDR(win, 0x18)	/* [한국어] 그 창의 트랜잭션 종류 */
#define OB_WIN_DEFAULT_ACTIONS			(OB_WIN_ACTIONS(OB_WIN_COUNT-1) + 0x4)	/* [한국어] **기본 창의 종류 레지스터.** 마지막 창 다음에 놓여 있으며, setup_hw 가 여기에 메모리 종류를 넣어 창 없이도 메모리 접근이 되게 한다 */
#define     OB_WIN_FUNC_NUM_MASK		GENMASK(31, 24)	/* [한국어] 함수 번호 필드 */
#define     OB_WIN_FUNC_NUM_SHIFT		24	/* [한국어] 그 필드의 비트 위치 */
#define     OB_WIN_FUNC_NUM_ENABLE		BIT(23)	/* [한국어] 함수 번호 비교 활성화 */
#define     OB_WIN_BUS_NUM_BITS_MASK		GENMASK(22, 20)	/* [한국어] 버스 번호 비트 수 필드 */
#define     OB_WIN_BUS_NUM_BITS_SHIFT		20	/* [한국어] 그 필드의 비트 위치 */
#define     OB_WIN_MSG_CODE_ENABLE		BIT(22)	/* [한국어] 메시지 코드 비교 활성화 */
#define     OB_WIN_MSG_CODE_MASK		GENMASK(21, 14)	/* [한국어] 메시지 코드 필드 */
#define     OB_WIN_MSG_CODE_SHIFT		14	/* [한국어] 그 필드의 비트 위치 */
#define     OB_WIN_MSG_PAYLOAD_LEN		BIT(12)	/* [한국어] 메시지 페이로드 길이 */
#define     OB_WIN_ATTR_ENABLE			BIT(11)	/* [한국어] 속성 비교 활성화 */
#define     OB_WIN_ATTR_TC_MASK			GENMASK(10, 8)	/* [한국어] 트래픽 클래스 필드 */
#define     OB_WIN_ATTR_TC_SHIFT		8	/* [한국어] 그 필드의 비트 위치 */
#define     OB_WIN_ATTR_RELAXED			BIT(7)	/* [한국어] relaxed ordering 속성 */
#define     OB_WIN_ATTR_NOSNOOP			BIT(6)	/* [한국어] no-snoop 속성 */
#define     OB_WIN_ATTR_POISON			BIT(5)	/* [한국어] 오염(poisoned) 속성 */
#define     OB_WIN_ATTR_IDO			BIT(4)	/* [한국어] ID 기반 순서 완화 속성 */
#define     OB_WIN_TYPE_MASK			GENMASK(3, 0)	/* [한국어] **창의 트랜잭션 종류 필드** */
#define     OB_WIN_TYPE_SHIFT			0	/* [한국어] 그 필드의 비트 위치 */
#define     OB_WIN_TYPE_MEM			0x0	/* [한국어] 종류 — 메모리. probe 가 메모리 자원에 이 값을 넣는다 */
#define     OB_WIN_TYPE_IO			0x4	/* [한국어] 종류 — I/O. probe 가 I/O 자원에 이 값을 넣는다 */
#define     OB_WIN_TYPE_CONFIG_TYPE0		0x8	/* [한국어] 종류 — config type 0. **이 드라이버는 쓰지 않는다** — probe 의 상류 주석대로 config 는 PIO 로만 처리한다 */
#define     OB_WIN_TYPE_CONFIG_TYPE1		0x9	/* [한국어] 종류 — config type 1. 같은 이유로 쓰지 않는다 */
#define     OB_WIN_TYPE_MSG			0xc	/* [한국어] 종류 — 메시지. 이 드라이버는 쓰지 않는다(전수 grep 확인) */

/* LMI registers base address and register offsets */
#define LMI_BASE_ADDR				0x6000	/* [한국어] **LMI 블록의 시작 주소.** 링크 상태와 벤더 ID 가 여기 있다 */
#define CFG_REG					(LMI_BASE_ADDR + 0x0)	/* [한국어] 설정 레지스터 — LTSSM 상태를 여기서 읽는다 */
#define     LTSSM_SHIFT				24	/* [한국어] LTSSM 상태 필드의 비트 위치 */
#define     LTSSM_MASK				0x3f	/* [한국어] 그 필드의 마스크(6비트) */
#define     RC_BAR_CONFIG			0x300	/* [한국어] 루트 컴플렉스 BAR 설정. 이 파일에서 읽는 곳은 없다(전수 grep 확인) */

/* LTSSM values in CFG_REG */
enum {	/* [한국어] **CFG_REG 에서 읽히는 LTSSM 상태 값들.** 링크가 섰는지·훈련 중인지 판별하는 세 함수가 이 값들의 구간으로 답한다 */
	LTSSM_DETECT_QUIET			= 0x0,	/* [한국어] 검출 — 조용한 상태. 링크가 아직 시작되지 않았다 */
	LTSSM_DETECT_ACTIVE			= 0x1,	/* [한국어] 검출 — 상대를 찾는 중 */
	LTSSM_POLLING_ACTIVE			= 0x2,	/* [한국어] 폴링 — 능동 */
	LTSSM_POLLING_COMPLIANCE		= 0x3,	/* [한국어] 폴링 — 규격 적합성 시험 */
	LTSSM_POLLING_CONFIGURATION		= 0x4,	/* [한국어] 폴링 — 설정 준비 */
	LTSSM_CONFIG_LINKWIDTH_START		= 0x5,	/* [한국어] **설정 시작.** advk_pcie_link_training() 의 하한이 이 값이다 */
	LTSSM_CONFIG_LINKWIDTH_ACCEPT		= 0x6,	/* [한국어] 설정 — 링크 폭 수락 */
	LTSSM_CONFIG_LANENUM_ACCEPT		= 0x7,	/* [한국어] 설정 — 레인 번호 수락 */
	LTSSM_CONFIG_LANENUM_WAIT		= 0x8,	/* [한국어] 설정 — 레인 번호 대기 */
	LTSSM_CONFIG_COMPLETE			= 0x9,	/* [한국어] 설정 완료 */
	LTSSM_CONFIG_IDLE			= 0xa,	/* [한국어] **설정 유휴.** advk_pcie_link_active() 의 하한이 이 값이다 — 규격이 Link Up 을 여기부터로 본다 */
	LTSSM_RECOVERY_RCVR_LOCK		= 0xb,	/* [한국어] 복구 — 수신기 잠금 */
	LTSSM_RECOVERY_SPEED			= 0xc,	/* [한국어] 복구 — 속도 변경 */
	LTSSM_RECOVERY_RCVR_CFG			= 0xd,	/* [한국어] 복구 — 수신기 설정 */
	LTSSM_RECOVERY_IDLE			= 0xe,	/* [한국어] 복구 — 유휴 */
	LTSSM_L0				= 0x10,	/* [한국어] **정상 동작(L0).** advk_pcie_link_up() 의 하한이자 link_training 의 상한이다. 값이 0xe 다음이 0x10 이라 0xf 가 비어 있다 */
	LTSSM_RX_L0S_ENTRY			= 0x11,	/* [한국어] 수신 L0s 진입 */
	LTSSM_RX_L0S_IDLE			= 0x12,	/* [한국어] 수신 L0s 유휴 */
	LTSSM_RX_L0S_FTS			= 0x13,	/* [한국어] 수신 L0s FTS */
	LTSSM_TX_L0S_ENTRY			= 0x14,	/* [한국어] 송신 L0s 진입 */
	LTSSM_TX_L0S_IDLE			= 0x15,	/* [한국어] 송신 L0s 유휴 */
	LTSSM_TX_L0S_FTS			= 0x16,	/* [한국어] 송신 L0s FTS */
	LTSSM_L1_ENTRY				= 0x17,	/* [한국어] L1 진입 */
	LTSSM_L1_IDLE				= 0x18,	/* [한국어] L1 유휴 */
	LTSSM_L2_IDLE				= 0x19,	/* [한국어] L2 유휴 */
	LTSSM_L2_TRANSMIT_WAKE			= 0x1a,	/* [한국어] L2 에서 깨우기 전송 */
	LTSSM_DISABLED				= 0x20,	/* [한국어] **꺼짐.** 위 세 판별 함수 모두 이 값 미만을 조건으로 삼는다 */
	LTSSM_LOOPBACK_ENTRY_MASTER		= 0x21,	/* [한국어] 루프백 — 마스터 진입 */
	LTSSM_LOOPBACK_ACTIVE_MASTER		= 0x22,	/* [한국어] 루프백 — 마스터 활성 */
	LTSSM_LOOPBACK_EXIT_MASTER		= 0x23,	/* [한국어] 루프백 — 마스터 탈출 */
	LTSSM_LOOPBACK_ENTRY_SLAVE		= 0x24,	/* [한국어] 루프백 — 슬레이브 진입 */
	LTSSM_LOOPBACK_ACTIVE_SLAVE		= 0x25,	/* [한국어] 루프백 — 슬레이브 활성 */
	LTSSM_LOOPBACK_EXIT_SLAVE		= 0x26,	/* [한국어] 루프백 — 슬레이브 탈출 */
	LTSSM_HOT_RESET				= 0x27,	/* [한국어] 핫 리셋 */
	LTSSM_RECOVERY_EQUALIZATION_PHASE0	= 0x28,	/* [한국어] **복구 이퀄라이제이션 0단계.** link_training 의 두 번째 구간이 여기서 시작한다 */
	LTSSM_RECOVERY_EQUALIZATION_PHASE1	= 0x29,	/* [한국어] 복구 이퀄라이제이션 1단계 */
	LTSSM_RECOVERY_EQUALIZATION_PHASE2	= 0x2a,	/* [한국어] 복구 이퀄라이제이션 2단계 */
	LTSSM_RECOVERY_EQUALIZATION_PHASE3	= 0x2b,	/* [한국어] 복구 이퀄라이제이션 3단계. link_training 의 두 번째 구간이 여기까지다 */
};

#define VENDOR_ID_REG				(LMI_BASE_ADDR + 0x44)	/* [한국어] **벤더 ID 레지스터.** setup_hw 가 erratum 4.1 우회로 여기에 올바른 값을 써서 읽기 전용 DEV_ID_REG 의 되읽기 값을 바로잡는다 */

/* PCIe core controller registers */
#define CTRL_CORE_BASE_ADDR			0x18000	/* [한국어] 컨트롤러 코어 제어 블록의 시작 주소 */
#define CTRL_CONFIG_REG				(CTRL_CORE_BASE_ADDR + 0x0)	/* [한국어] 설정 레지스터 — 컨트롤러 동작 모드를 정한다 */
#define     CTRL_MODE_SHIFT			0x0	/* [한국어] 모드 필드의 비트 위치 */
#define     CTRL_MODE_MASK			0x1	/* [한국어] 그 필드의 마스크 */
#define     PCIE_CORE_MODE_DIRECT		0x0	/* [한국어] 모드 — Direct. **setup_hw 가 이 값을 쓴다** */
#define     PCIE_CORE_MODE_COMMAND		0x1	/* [한국어] 모드 — Command. 이 드라이버는 쓰지 않는다 */

/* PCIe Central Interrupts Registers */
#define CENTRAL_INT_BASE_ADDR			0x1b000	/* [한국어] **중앙 인터럽트 블록의 시작 주소.** GIC 로 나가는 요약 인터럽트가 여기서 갈린다 */
#define HOST_CTRL_INT_STATUS_REG		(CENTRAL_INT_BASE_ADDR + 0x0)	/* [한국어] 상위 인터럽트 상태. **핸들러가 가장 먼저 읽는 레지스터다** */
#define HOST_CTRL_INT_MASK_REG			(CENTRAL_INT_BASE_ADDR + 0x4)	/* [한국어] 상위 인터럽트 마스크 */
#define     PCIE_IRQ_CMDQ_INT			BIT(0)	/* [한국어] 명령 큐 인터럽트 */
#define     PCIE_IRQ_MSI_STATUS_INT		BIT(1)	/* [한국어] MSI 상태 인터럽트 */
#define     PCIE_IRQ_CMD_SENT_DONE		BIT(3)	/* [한국어] 명령 전송 완료 */
#define     PCIE_IRQ_DMA_INT			BIT(4)	/* [한국어] DMA 인터럽트 */
#define     PCIE_IRQ_IB_DXFERDONE		BIT(5)	/* [한국어] 인바운드 데이터 전송 완료 */
#define     PCIE_IRQ_OB_DXFERDONE		BIT(6)	/* [한국어] 아웃바운드 데이터 전송 완료 */
#define     PCIE_IRQ_OB_RXFERDONE		BIT(7)	/* [한국어] 아웃바운드 읽기 전송 완료 */
#define     PCIE_IRQ_COMPQ_INT			BIT(12)	/* [한국어] 완료 큐 인터럽트 */
#define     PCIE_IRQ_DIR_RD_DDR_DET		BIT(13)	/* [한국어] DDR 직접 읽기 검출 */
#define     PCIE_IRQ_DIR_WR_DDR_DET		BIT(14)	/* [한국어] DDR 직접 쓰기 검출 */
#define     PCIE_IRQ_CORE_INT			BIT(16)	/* [한국어] **코어 인터럽트. 이 드라이버가 유일하게 쓰는 비트다** — 핸들러가 이 비트로 자기 인터럽트인지 가리고, 처리 뒤 이 비트를 지운다 */
#define     PCIE_IRQ_CORE_INT_PIO		BIT(17)	/* [한국어] 코어 인터럽트 중 PIO 관련. 이 파일에서 읽는 곳은 없다(전수 grep 확인) */
#define     PCIE_IRQ_DPMU_INT			BIT(18)	/* [한국어] DPMU 인터럽트 */
#define     PCIE_IRQ_PCIE_MIS_INT		BIT(19)	/* [한국어] PCIe 기타 인터럽트 */
#define     PCIE_IRQ_MSI_INT1_DET		BIT(20)	/* [한국어] MSI 인터럽트 1 검출 */
#define     PCIE_IRQ_MSI_INT2_DET		BIT(21)	/* [한국어] MSI 인터럽트 2 검출 */
#define     PCIE_IRQ_RC_DBELL_DET		BIT(22)	/* [한국어] 루트 컴플렉스 도어벨 검출 */
#define     PCIE_IRQ_EP_STATUS			BIT(23)	/* [한국어] 엔드포인트 상태 */
#define     PCIE_IRQ_ALL_MASK			GENMASK(31, 0)	/* [한국어] 상위 인터럽트 전체 비트 */
#define     PCIE_IRQ_ENABLE_INTS_MASK		PCIE_IRQ_CORE_INT	/* [한국어] **소프트웨어가 켤 인터럽트.** 코어 인터럽트 하나뿐이라, setup_hw 가 이것만 빼고 전부 마스크한다 */

/* Transaction types */
#define PCIE_CONFIG_RD_TYPE0			0x8	/* [한국어] PIO 트랜잭션 종류 — config 읽기 type 0(바로 아래 버스의 장치) */
#define PCIE_CONFIG_RD_TYPE1			0x9	/* [한국어] config 읽기 type 1(브리지를 거쳐 더 아래로) */
#define PCIE_CONFIG_WR_TYPE0			0xa	/* [한국어] config 쓰기 type 0 */
#define PCIE_CONFIG_WR_TYPE1			0xb	/* [한국어] config 쓰기 type 1 */

#define PIO_RETRY_CNT			750000	/* 1.5 s */ /* [한국어] **PIO 완료를 기다리는 최대 횟수.** 옆 상류 주석대로 아래 지연과 곱하면 1.5초다. RRS 재시도도 같은 상한을 공유한다 */
#define PIO_RETRY_DELAY			2	/* 2 us*/ /* [한국어] 그 폴링 간격(udelay) */

#define LINK_WAIT_MAX_RETRIES		10	/* [한국어] 링크가 서기를 기다리는 최대 횟수 */
#define LINK_WAIT_USLEEP_MIN		90000	/* [한국어] 그 대기의 하한 */
#define LINK_WAIT_USLEEP_MAX		100000	/* [한국어] 그 대기의 상한. **10회 x 90ms 로 최소 900ms** 이며, 규격이 PERST# 뒤 요구하는 100ms 를 함께 채운다 */
#define RETRAIN_WAIT_MAX_RETRIES	10	/* [한국어] 재훈련 시작을 기다리는 최대 횟수 */
#define RETRAIN_WAIT_USLEEP_US		2000	/* [한국어] 그 대기 간격. 이름은 usleep 을 가리키지만 실제로는 udelay 로 쓰인다 */

#define MSI_IRQ_NUM			32	/* [한국어] **MSI 벡터 수. 하드웨어의 MSI 상태·마스크 레지스터가 32비트인 데서 온 상한이다** */

#define CFG_RD_RRS_VAL			0xffff0001	/* [한국어] **RRS 읽기의 정상 결과값.** 규격(PCIe r6.0, 2.3.2)이 Vendor ID 자리에 0001h 를, 나머지에 전부 1 을 돌려주도록 정한다 */

struct advk_pcie {
	/* [한국어]
	 * struct platform_device *pdev;
	 * 이 드라이버가 붙은 플랫폼 장치.
	 * 설정자: advk_pcie_probe() 가 맨 처음 담는다.
	 * 읽는 자: 파일 전체에서 &pcie->pdev->dev 꼴로 오류 메시지를 낼 장치를
	 * 얻는 데 쓴다.
	 * 동기화: probe 이후 바뀌지 않는다.
	 */
	struct platform_device *pdev;
	/* [한국어]
	 * void __iomem *base;
	 * 컨트롤러 레지스터 창의 커널 가상 주소.
	 * 설정자: probe 의 devm_platform_ioremap_resource(pdev, 0).
	 * 읽는 자: advk_writel()/advk_readl() 둘뿐이며, **이 파일의 모든 레지스터
	 * 접근이 그 둘을 거친다.** 파일 앞부분의 레지스터 정의는 모두 이 주소
	 * 기준의 오프셋이다.
	 * 동기화: 포인터 자체는 바뀌지 않고, 가리키는 레지스터의 동시 접근은
	 * 필요한 곳에서 락으로 지킨다.
	 */
	void __iomem *base;
	/* [한국어]
	 * 익명 구조체 배열 wins[OB_WIN_COUNT];
	 * 아웃바운드 주소 창 설정을 담아 두는 자리. 하드웨어에 바로 쓰지 않고
	 * probe 가 여기에 모아 두었다가 setup_hw 가 옮겨 쓴다 — 계산과 적용을
	 * 나눈 것이다.
	 * 설정자: advk_pcie_probe() 의 ranges 순회.
	 * 읽는 자: advk_pcie_setup_hw() 가 advk_pcie_set_ob_win() 에 넘긴다.
	 */
	struct {
	/* [한국어]
	 * phys_addr_t match;
	 * 이 창이 가로챌 CPU 쪽 주소.
	 * 설정자: probe 가 자원 종류에 따라 다르게 채운다 — I/O 이면
	 * pci_pio_to_address() 로 바꾼 물리 주소를, 메모리이면 시작 주소를
	 * 그대로 넣는다.
	 * 읽는 자: advk_pcie_set_ob_win() 이 MATCH_LS/MS 레지스터에 쓴다.
	 */
		phys_addr_t match;
	/* [한국어]
	 * phys_addr_t remap;
	 * 그 주소를 PCIe 쪽에서 어느 주소로 바꿀지.
	 * 설정자: probe 가 (시작 주소 - 자원의 오프셋)으로 계산한다.
	 * 읽는 자: advk_pcie_set_ob_win() 이 REMAP_LS/MS 레지스터에 쓴다.
	 * 값 범위: **mask 에 든 비트만 세울 수 있다** — probe 가 그 조건을
	 * 확인하고 어긋나면 창 자르기를 멈춘다.
	 */
		phys_addr_t remap;
	/* [한국어]
	 * phys_addr_t mask;
	 * 창의 크기를 정하는 마스크. ~(크기 - 1) 꼴이다.
	 * 설정자: probe 가 계산한 창 크기에서 만든다.
	 * 읽는 자: advk_pcie_set_ob_win() 이 MASK_LS/MS 레지스터에 쓴다.
	 * 값 범위: **하위 16비트가 0 이어야 하므로 창 크기가 최소 64KiB** 다.
	 * probe 의 상류 주석이 그 제약을 밝힌다.
	 */
		phys_addr_t mask;
	/* [한국어]
	 * u32 actions;
	 * 이 창으로 나가는 트랜잭션의 종류.
	 * 설정자: probe 가 OB_WIN_TYPE_IO 또는 OB_WIN_TYPE_MEM 을 넣는다.
	 * 읽는 자: advk_pcie_set_ob_win() 이 ACTIONS 레지스터에 쓴다.
	 */
		u32 actions;
	} wins[OB_WIN_COUNT];
	/* [한국어]
	 * u8 wins_count;
	 * 위 배열에서 실제로 쓰이는 창의 개수.
	 * 설정자: probe 의 ranges 순회가 창을 하나 만들 때마다 늘린다.
	 * 읽는 자: advk_pcie_setup_hw() 가 여기까지는 설정하고 그 뒤부터
	 * OB_WIN_COUNT 까지는 advk_pcie_disable_ob_win() 으로 끈다.
	 * 값 범위: 0 ~ OB_WIN_COUNT. 대부분의 보드에서 작은 값이다 — 메모리
	 * 접근은 창 없이 투명하게 처리되기 때문이다.
	 */
	u8 wins_count;
	/* [한국어]
	 * struct irq_domain *rp_irq_domain;
	 * **에뮬레이션된 루트 포트 자신의 인터럽트 도메인.** 크기가 1 이다.
	 * 설정자: advk_pcie_init_rp_irq_domain().
	 * 읽는 자: advk_pcie_handle_pme() 와 advk_pcie_handle_int() 의 오류
	 * 처리가 generic_handle_domain_irq(..., 0) 으로 쓰고,
	 * advk_pcie_map_irq() 가 루트 버스 장치의 INTx 를 여기로 매핑한다.
	 * 왜 크기가 1 인가: 두 처리 함수의 상류 주석대로 Aardvark 하드웨어가
	 * PCI_EXP_FLAGS_IRQ 와 PCI_ERR_ROOT_AER_IRQ 를 모두 0 으로 돌려주므로
	 * PME 도 AER 도 인터럽트 0 번을 쓴다.
	 */
	struct irq_domain *rp_irq_domain;
	/* [한국어]
	 * struct irq_domain *irq_domain;
	 * INTx(레거시) 인터럽트 도메인. 크기가 PCI_NUM_INTX(4)다.
	 * 설정자: advk_pcie_init_irq_domain(). 장치 트리의 인터럽트 컨트롤러
	 * 자식 노드에 붙는다.
	 * 읽는 자: advk_pcie_handle_int() 가 세워진 INTx 비트마다
	 * generic_handle_domain_irq() 로 쓴다.
	 */
	struct irq_domain *irq_domain;
	/* [한국어]
	 * struct irq_chip irq_chip;
	 * INTx 용 irq_chip. **정적 구조체가 아니라 인스턴스마다 하나씩** 둔다.
	 * 설정자: advk_pcie_init_irq_domain() 이 이름을 devm_kasprintf() 로
	 * "<장치이름>-irq" 처럼 만들어 넣고 마스크·언마스크 함수를 매단다.
	 * 읽는 자: advk_pcie_irq_map() 이 가상 IRQ 에 매단다.
	 * 왜 인스턴스마다인가: 이름을 장치별로 다르게 지어 /proc/interrupts 에서
	 * 구분되게 하기 위해서다. MSI 쪽(advk_msi_bottom_irq_chip)과 루트 포트
	 * 쪽(advk_rp_irq_chip)은 정적 구조체를 공유한다.
	 */
	struct irq_chip irq_chip;
	/* [한국어]
	 * raw_spinlock_t irq_lock;
	 * INTx 마스크 레지스터(ISR1_MASK)를 지키는 락.
	 * 설정자: advk_pcie_init_irq_domain() 의 raw_spin_lock_init().
	 * 읽는 자: advk_pcie_irq_mask()/unmask() 가 irqsave 판으로 잡는다.
	 * 왜 raw 이고 irqsave 인가: 두 함수가 **인터럽트 컨텍스트에서도 불릴 수
	 * 있어** 잠들 수 없고, 읽고-고치고-쓰는 세 단계를 원자적으로 해야 하기
	 * 때문이다. MSI 쪽과 락을 나눈 이유는 서로 다른 레지스터를 만져 굳이
	 * 서로를 막을 필요가 없어서다.
	 */
	raw_spinlock_t irq_lock;
	/* [한국어]
	 * struct irq_domain *msi_inner_domain;
	 * MSI 부모 도메인. 크기가 MSI_IRQ_NUM(32)이다.
	 * 설정자: advk_pcie_init_msi_irq_domain() 의
	 * msi_create_parent_irq_domain().
	 * 읽는 자: advk_pcie_handle_msi() 가 벡터별로
	 * generic_handle_domain_irq() 를 부른다.
	 * 값 범위: 32개가 상한이며, 하드웨어의 MSI 상태·마스크 레지스터가
	 * 32비트인 데서 온다.
	 */
	struct irq_domain *msi_inner_domain;
	/* [한국어]
	 * raw_spinlock_t msi_irq_lock;
	 * MSI 마스크 레지스터(PCIE_MSI_MASK_REG)를 지키는 락.
	 * 설정자: advk_pcie_init_msi_irq_domain() 의 raw_spin_lock_init().
	 * 읽는 자: advk_msi_irq_mask()/unmask().
	 * irq_lock 과 같은 이유로 raw 이고 irqsave 다. 아래 msi_used_lock 과
	 * 다른 락인 이유는 지키는 대상과 실행 컨텍스트가 다르기 때문이다 —
	 * 이쪽은 하드웨어 레지스터를 인터럽트 컨텍스트에서도 만지고, 저쪽은
	 * 비트맵을 프로세스 컨텍스트에서만 만진다.
	 */
	raw_spinlock_t msi_irq_lock;
	/* [한국어]
	 * DECLARE_BITMAP(msi_used, MSI_IRQ_NUM);
	 * 32개 MSI 벡터 중 어느 것이 쓰이고 있는지를 담은 비트맵.
	 * 설정자: advk_msi_irq_domain_alloc() 의 bitmap_find_free_region() 이
	 * 구간을 잡고, advk_msi_irq_domain_free() 의 bitmap_release_region() 이
	 * 놓는다.
	 * 읽는 자: 같은 두 함수뿐이다.
	 * 값 범위: 비트 하나가 벡터 하나. **연속이고 2의 거듭제곱 개수인
	 * 구간으로만 잡힌다** — 다중 MSI 의 규격 제약 때문이다.
	 * 동기화: msi_used_lock 뮤텍스.
	 */
	DECLARE_BITMAP(msi_used, MSI_IRQ_NUM);
	/* [한국어]
	 * struct mutex msi_used_lock;
	 * 위 비트맵을 지키는 뮤텍스.
	 * 설정자: advk_pcie_init_msi_irq_domain() 의 mutex_init().
	 * 읽는 자: MSI 벡터 할당과 해제 두 곳뿐이다.
	 * 왜 스핀락이 아닌가: 그 두 경로가 프로세스 컨텍스트에서만 불려 잠들 수
	 * 있기 때문이다.
	 * [관찰] advk_pcie_handle_msi() 는 비트맵을 보지 않으므로 이 락과
	 * 무관하다.
	 */
	struct mutex msi_used_lock;
	/* [한국어]
	 * int link_gen;
	 * 장치 트리가 요구한 최대 링크 세대(1, 2, 3).
	 * 설정자: advk_pcie_probe() 가 of_pci_get_max_link_speed() 로 읽고,
	 * **1~3 이 아니면 3 을 기본값으로 쓴다.**
	 * 읽는 자: advk_pcie_train_link() 가 CTRL0 의 세대 선택 필드와
	 * Link Control 2 의 Target Link Speed 두 곳에 반영한다.
	 * [대비] pci-imx6.c 는 같은 상황에서 기본값을 Gen1 로 둔다.
	 */
	int link_gen;
	/* [한국어]
	 * struct pci_bridge_emul bridge;
	 * **소프트웨어로 흉내 내는 루트 포트의 상태 전부.** 에뮬레이션 config
	 * 버퍼와 콜백 묶음이 들어 있다.
	 * 설정자: advk_sw_pci_bridge_init() 가 초기값을 채우고
	 * pci_bridge_emul_init() 을 부른다. advk_pcie_remove() 가
	 * pci_bridge_emul_cleanup() 으로 놓는다.
	 * 읽는 자: 루트 버스에 대한 모든 config 접근(advk_pcie_rd_conf/wr_conf),
	 * 그리고 rd_conf 의 allow_rrs 계산(pcie_conf.rootctl),
	 * advk_pcie_handle_pme() 의 rootsta/rootctl 조작.
	 * 왜 필요한가: setup_hw 의 상류 주석대로 이 하드웨어에는 규격에 맞는
	 * Type 1 config 공간이 없고 Aardvark 의 config 접근 방식으로 읽을 수도
	 * 없다.
	 */
	struct pci_bridge_emul bridge;
	/* [한국어]
	 * struct gpio_desc *reset_gpio;
	 * PERST# 리셋 신호에 연결된 GPIO.
	 * 설정자: probe 의 devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW).
	 * 보드에 없으면 NULL 이다.
	 * 읽는 자: advk_pcie_issue_perst() 가 링크 훈련 전에 10ms 흔들고,
	 * advk_pcie_remove() 가 전원 차단 대비로 어서트한다.
	 * 값 범위: NULL 가능. issue_perst 는 NULL 이면 곧바로 나가고, remove 는
	 * NULL 검사를 따로 한다.
	 */
	struct gpio_desc *reset_gpio;
	/* [한국어]
	 * struct phy *phy;
	 * generic PHY 드라이버 핸들.
	 * 설정자: advk_pcie_setup_phy() 가 devm_of_phy_get() 으로 얻는다.
	 * **옛 장치 트리 바인딩에는 PHY 핸들이 없어** 못 얻어도 경고만 남기고
	 * NULL 로 둔 채 계속 진행한다.
	 * 읽는 자: advk_pcie_enable_phy()/disable_phy().
	 * 값 범위: NULL 가능. enable 쪽은 NULL 이면 곧바로 성공으로 나가고,
	 * disable 쪽은 phy_power_off(NULL) 이 안전하다는 데 기대 검사하지 않는다.
	 */
	struct phy *phy;
};

/* [한국어]
 * advk_writel - 컨트롤러 레지스터 창에 32비트를 쓴다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @val:  쓸 값.
 * @reg:  창 시작으로부터의 오프셋.
 *
 * 이 파일이 하드웨어를 만지는 두 통로 중 하나다. base 는 probe 가
 * devm_platform_ioremap_resource() 로 얻은 커널 가상 주소이며, 파일
 * 전체의 레지스터 정의가 모두 이 base 기준의 오프셋이다.
 *
 * 인자 순서가 커널의 writel(val, addr) 과 같아 값이 앞에 온다 —
 * 읽기 쪽 advk_readl(pcie, reg) 과 인자 개수가 달라 헷갈리기 쉬운 부분이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽. 그 자체로는
 * 직렬화하지 않으며, 필요한 곳에서 호출자가 락을 잡는다.
 *
 * 호출 체인:  이 파일의 거의 모든 함수 -> [이 함수] -> writel()
 */
static inline void advk_writel(struct advk_pcie *pcie, u32 val, u64 reg)
{
	writel(val, pcie->base + reg);	/* [한국어] 창 시작 주소에 오프셋을 더한 곳에 32비트를 쓴다 */
}

/* [한국어]
 * advk_readl - 컨트롤러 레지스터 창에서 32비트를 읽는다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @reg:  창 시작으로부터의 오프셋.
 * @return: 읽은 값.
 *
 * advk_writel() 의 짝이다. 이 컨트롤러의 레지스터는 모두 32비트 단위라
 * 바이트나 워드 접근 판이 따로 없고, 바이트 단위가 필요한 config 접근은
 * PIO 의 strobe 로 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:  이 파일의 거의 모든 함수 -> [이 함수] -> readl()
 */
static inline u32 advk_readl(struct advk_pcie *pcie, u64 reg)
{
	return readl(pcie->base + reg);	/* [한국어] 같은 방식으로 32비트를 읽어 돌려준다 */
}

/* [한국어]
 * advk_pcie_ltssm_state - LTSSM 이 지금 어느 상태인지 읽어 온다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: LTSSM 상태 값(파일 앞부분의 LTSSM_* 상수 중 하나).
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크의 상태
 * 기계이며, 링크가 섰는지·훈련 중인지·꺼졌는지를 이 값 하나로 알 수 있다.
 * 그래서 아래 세 판별 함수가 모두 이 함수를 기반으로 한다.
 *
 * CFG_REG 의 LTSSM_SHIFT 자리에서 LTSSM_MASK 만큼을 떼어 낸다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   advk_pcie_link_up()/link_active()/link_training() 및
 *   에뮬레이션 브리지의 링크 상태 읽기 -> [이 함수] -> advk_readl()
 */
static u8 advk_pcie_ltssm_state(struct advk_pcie *pcie)
{
	u32 val;	/* [한국어] 읽은 레지스터 값 */
	u8 ltssm_state;	/* [한국어] 떼어 낸 LTSSM 상태 */

	val = advk_readl(pcie, CFG_REG);	/* [한국어] LMI 블록의 설정 레지스터를 읽고 */
	ltssm_state = (val >> LTSSM_SHIFT) & LTSSM_MASK;	/* [한국어] **24번 비트부터 6비트를 떼어 낸다** — 그 자리에 LTSSM 상태가 있다 */
	return ltssm_state;	/* [한국어] 그 값을 돌려준다 */
}

/* [한국어]
 * advk_pcie_link_up - 링크가 정상 동작 중인지 판별한다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: true 면 링크가 서 있다.
 *
 * 상류 주석대로 LTSSM 이 정상 동작 상태(L* 중 하나)인지 본다. 판별
 * 기준은 L0 이상이면서 DISABLED 미만인 구간이다.
 *
 * 아래 advk_pcie_link_active() 와 다른 점은 시작 경계다. 이쪽은 L0
 * 부터라 링크가 완전히 선 뒤만 참이고, 저쪽은 Configuration.Idle 부터라
 * 데이터 링크가 올라오는 중에도 참이 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   advk_pcie_wait_for_link() / advk_pcie_valid_device() 등 -> [이 함수]
 *     -> advk_pcie_ltssm_state()
 */
static inline bool advk_pcie_link_up(struct advk_pcie *pcie)
{
	/* check if LTSSM is in normal operation - some L* state */
	u8 ltssm_state = advk_pcie_ltssm_state(pcie);	/* [한국어] LTSSM 상태를 읽어 */
	return ltssm_state >= LTSSM_L0 && ltssm_state < LTSSM_DISABLED;	/* [한국어] **L0 이상이면서 꺼짐 미만이면 링크가 서 있다.** 바로 위 상류 주석대로 정상 동작(L* 중 하나)인지 보는 것이다 */
}

/* [한국어]
 * advk_pcie_link_active - 데이터 링크가 활성 상태인지 판별한다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: true 면 데이터 링크가 활성이다.
 *
 * 바로 안쪽 상류 주석이 규격 근거를 밝힌다 — PCIe Base 3.0 의 Table 4-14
 * 가 LTSSM 상태를 링크 상태로 대응시키는데, Link Up 은 Configuration.Idle,
 * Recovery, L0, L0s, L1, L2 에 대응한다. 그리고 3.2.1 이 DL Up 을
 * DL Active 상태에서 보고하도록 정한다.
 *
 * 그래서 판별 구간이 Configuration.Idle 이상 DISABLED 미만이다.
 * advk_pcie_link_up() 보다 넓은 구간이며, 에뮬레이션 브리지가 링크 상태
 * 비트를 보고할 때 이쪽을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   advk_pci_bridge_emul_pcie_conf_read() 등 -> [이 함수]
 *     -> advk_pcie_ltssm_state()
 */
static inline bool advk_pcie_link_active(struct advk_pcie *pcie)
{
	/*
	 * According to PCIe Base specification 3.0, Table 4-14: Link
	 * Status Mapped to the LTSSM, and 4.2.6.3.6 Configuration.Idle
	 * is Link Up mapped to LTSSM Configuration.Idle, Recovery, L0,
	 * L0s, L1 and L2 states. And according to 3.2.1. Data Link
	 * Control and Management State Machine Rules is DL Up status
	 * reported in DL Active state.
	 */
	u8 ltssm_state = advk_pcie_ltssm_state(pcie);	/* [한국어] LTSSM 상태를 읽어 */
	return ltssm_state >= LTSSM_CONFIG_IDLE && ltssm_state < LTSSM_DISABLED;	/* [한국어] **Configuration.Idle 이상이면서 꺼짐 미만이면 데이터 링크가 활성이다.** link_up 보다 시작 경계가 낮은데, 바로 위 상류 주석이 인용한 규격 표가 Link Up 을 그 상태부터로 대응시키기 때문이다 */
}

/* [한국어]
 * advk_pcie_link_training - 링크가 지금 훈련 중인지 판별한다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: true 면 훈련 중이다.
 *
 * 바로 안쪽 상류 주석이 규격 근거를 밝힌다 — PCIe Base 3.0 의 Table 4-14
 * 가 Link Training 을 Configuration 과 Recovery 상태에 대응시킨다.
 *
 * 그래서 두 구간의 OR 이다 — Configuration 시작부터 L0 직전까지, 그리고
 * Recovery 의 이퀄라이제이션 단계 넷이다. 링크 재훈련을 지시한 뒤 그것이
 * 실제로 시작되었는지 확인하는 데 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   advk_pcie_wait_for_retrain() -> [이 함수] -> advk_pcie_ltssm_state()
 */
static inline bool advk_pcie_link_training(struct advk_pcie *pcie)
{
	/*
	 * According to PCIe Base specification 3.0, Table 4-14: Link
	 * Status Mapped to the LTSSM is Link Training mapped to LTSSM
	 * Configuration and Recovery states.
	 */
	u8 ltssm_state = advk_pcie_ltssm_state(pcie);	/* [한국어] LTSSM 상태를 읽어 */
	return ((ltssm_state >= LTSSM_CONFIG_LINKWIDTH_START &&	/* [한국어] **첫 구간** — 설정 시작부터 */
		 ltssm_state < LTSSM_L0) ||	/* [한국어] L0 직전까지. 바로 위 상류 주석대로 규격이 Link Training 을 Configuration 과 Recovery 에 대응시킨다 */
		(ltssm_state >= LTSSM_RECOVERY_EQUALIZATION_PHASE0 &&	/* [한국어] **둘째 구간** — 복구 이퀄라이제이션 0단계부터 */
		 ltssm_state <= LTSSM_RECOVERY_EQUALIZATION_PHASE3));	/* [한국어] 3단계까지. 둘 중 하나에 들면 훈련 중이다 */
}

/* [한국어]
 * advk_pcie_wait_for_link - 링크가 설 때까지 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 링크가 섰다. 시간이 지나면 -ETIMEDOUT.
 *
 * 90~100ms 간격으로 최대 10회 보므로 **최소 900ms 를 기다린다.**
 * 그 길이에는 뜻이 있다 — advk_pcie_train_link() 의 상류 주석이 밝히듯,
 * PCIe 규격이 PERST# 리셋 뒤 config 요청까지 최소 100ms 를 요구하는데
 * 이 대기가 그 요구를 함께 채운다.
 *
 * 첫 회차에서 이미 서 있으면 곧바로 0 을 돌려주므로 기다리지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). usleep 으로 잠든다.
 *
 * 호출 체인:  advk_pcie_train_link() -> [이 함수] -> advk_pcie_link_up()
 */
static int advk_pcie_wait_for_link(struct advk_pcie *pcie)
{
	int retries;	/* [한국어] 반복 횟수 */

	/* check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {	/* [한국어] 최대 10회 본다 */
		if (advk_pcie_link_up(pcie))	/* [한국어] 이미 서 있으면 */
			return 0;	/* [한국어] 곧바로 성공으로 나간다 — 기다리지 않는다 */

		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX);	/* [한국어] 90~100ms 쉬고 다시 본다. **10회면 최소 900ms** 이며, PERST# 뒤 규격이 요구하는 100ms 를 함께 채운다 */
	}

	return -ETIMEDOUT;	/* [한국어] 끝까지 안 서면 시간 초과다 */
}

/* [한국어]
 * advk_pcie_wait_for_retrain - 링크 재훈련이 실제로 시작되기를 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * 에뮬레이션 브리지가 Link Control 의 Retrain Link 비트 쓰기를 받으면
 * 하드웨어에 재훈련을 지시하는데, 그 지시가 반영되기까지 시간이 걸린다.
 * 그래서 LTSSM 이 훈련 상태로 들어갈 때까지 잠시 본다.
 *
 * 2ms 간격으로 최대 10회이므로 최대 20ms 다. **시간이 지나도 알리지
 * 않는다** — 반환값이 없어 호출자는 성패를 모른다.
 *
 * [관찰] udelay 로 바쁜 대기를 하는데 한 번이 2000us 다. 상수 이름은
 * RETRAIN_WAIT_USLEEP_US 로 usleep 을 가리키지만 실제로는 udelay 를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 쓰기 경로에서 불린다.
 *
 * 호출 체인:
 *   advk_pci_bridge_emul_pcie_conf_write() -> [이 함수]
 *     -> advk_pcie_link_training()
 */
static void advk_pcie_wait_for_retrain(struct advk_pcie *pcie)
{
	size_t retries;	/* [한국어] 반복 횟수 */

	for (retries = 0; retries < RETRAIN_WAIT_MAX_RETRIES; ++retries) {	/* [한국어] 최대 10회 본다 */
		if (advk_pcie_link_training(pcie))	/* [한국어] 재훈련이 실제로 시작되었으면 */
			break;	/* [한국어] 더 기다릴 것 없이 멈춘다 */
		udelay(RETRAIN_WAIT_USLEEP_US);	/* [한국어] 2ms 쉬고 다시 본다. **최대 20ms 이며 시간이 지나도 알리지 않는다** — 반환값이 없다 */
	}
}

/* [한국어]
 * advk_pcie_issue_perst - PERST# 리셋 신호를 10ms 동안 어서트한다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * 보드에 리셋 GPIO 가 없으면 곧바로 나간다.
 *
 * GPIO 를 1 로 올려 리셋을 걸고 10ms 유지한 뒤 0 으로 내린다. 상류
 * 주석이 그 길이의 이유를 밝힌다 — 어떤 카드에는 10ms 지연이 필요하다.
 *
 * **언제 부르는지가 중요하다.** advk_pcie_train_link() 가 링크 훈련을
 * 켠 직후에 부르는데, 그 자리의 상류 주석이 이유를 밝힌다 — 어떤 카드는
 * 초기 상태가 아닌 상태에 있으면 링크 훈련에서 검출되지 않으므로,
 * 카드를 확실히 초기 상태로 되돌리려는 것이다.
 *
 * 일어난 일을 dev_info 로 남긴다 — 디버그 수준이 아니라 정보 수준이라
 * 평소에도 보인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). usleep 으로 잠들며, GPIO
 * 조작도 잠들 수 있는 판을 쓴다.
 *
 * 호출 체인:  advk_pcie_train_link() -> [이 함수] -> gpiod_set_value_cansleep()
 */
static void advk_pcie_issue_perst(struct advk_pcie *pcie)
{
	if (!pcie->reset_gpio)	/* [한국어] 보드에 리셋 GPIO 가 없으면 */
		return;	/* [한국어] 할 일이 없다 */

	/* 10ms delay is needed for some cards */
	dev_info(&pcie->pdev->dev, "issuing PERST via reset GPIO for 10ms\n");	/* [한국어] 무엇을 하는지 알린다. debug 가 아니라 info 라 평소에도 보인다 */
	gpiod_set_value_cansleep(pcie->reset_gpio, 1);	/* [한국어] **PERST# 를 어서트한다** */
	usleep_range(10000, 11000);	/* [한국어] 바로 위 상류 주석대로 어떤 카드에는 10ms 지연이 필요하다 */
	gpiod_set_value_cansleep(pcie->reset_gpio, 0);	/* [한국어] PERST# 를 해제한다 */
}

/* [한국어]
 * advk_pcie_train_link - 링크 세대를 정하고 훈련을 켜고 링크가 서기를 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * setup_hw 의 마지막 단계다. 네 가지를 순서대로 한다.
 *
 *   1. **CTRL0 의 세대 선택 필드를 정한다.** 상류 주석대로 장치 트리의
 *      max-link-speed 에서 온 link_gen 값을 따르며, 이것이 최대 링크
 *      속도를 강제하는 역할도 한다.
 *   2. **같은 값을 Link Control 2 의 Target Link Speed 필드에도 쓴다.**
 *      상류 주석이 그 필요를 밝힌다 — Armada 3700 기능 명세는 이 필드의
 *      기본값이 SPEED_GEN 을 따른다고 하지만, 실제 시험에서는 늘
 *      8.0GT/s 였다는 것이다. 즉 명세와 실제가 달라 두 곳에 다 써 준다.
 *   3. **훈련을 켠다**(CTRL0 의 LINK_TRAINING_EN). 세대를 고른 뒤에
 *      켜야 한다는 것이 그 자리 상류 주석의 요지다.
 *   4. **PERST# 를 흔들고 링크를 기다린다.** 두 상류 주석이 각각 이유를
 *      밝힌다 — 어떤 카드는 초기 상태가 아니면 검출되지 않고, 규격
 *      (PCIe Base 4.0, 6.6.1 Conventional Reset)이 그런 리셋 뒤 config
 *      요청까지 최소 100ms 를 요구한다. wait_for_link() 가 최소 900ms 를
 *      기다리므로 그 요구를 함께 채운다.
 *
 * 링크가 서지 않아도 오류로 돌아가지 않는다 — 메시지만 남기고 probe 는
 * 계속된다. 카드가 없는 슬롯도 정상 상태이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). 최대 1초 가까이 잠든다.
 *
 * 호출 체인:
 *   advk_pcie_setup_hw() -> [이 함수] -> advk_pcie_issue_perst()
 *                                     -> advk_pcie_wait_for_link()
 */
static void advk_pcie_train_link(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	u32 reg;	/* [한국어] 읽고 고칠 레지스터 값 */
	int ret;	/* [한국어] 하위 호출 결과 */

	/*
	 * Setup PCIe rev / gen compliance based on device tree property
	 * 'max-link-speed' which also forces maximal link speed.
	 */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);	/* [한국어] **세대 선택 필드를 정한다.** 바로 위 상류 주석대로 장치 트리의 max-link-speed 를 따르며 그것이 최대 속도를 강제하는 역할도 한다 */
	reg &= ~PCIE_GEN_SEL_MSK;	/* [한국어] 기존 값을 지우고 */
	if (pcie->link_gen == 3)	/* [한국어] Gen3 을 요구했으면 */
		reg |= SPEED_GEN_3;	/* [한국어] 8.0GT/s 값을, */
	else if (pcie->link_gen == 2)	/* [한국어] Gen2 를 요구했으면 */
		reg |= SPEED_GEN_2;	/* [한국어] 5.0GT/s 값을, */
	else	/* [한국어] 그 밖이면 */
		reg |= SPEED_GEN_1;	/* [한국어] 2.5GT/s 값을 넣는다 */
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);	/* [한국어] 고친 값을 되쓴다 */

	/*
	 * Set maximal link speed value also into PCIe Link Control 2 register.
	 * Armada 3700 Functional Specification says that default value is based
	 * on SPEED_GEN but tests showed that default value is always 8.0 GT/s.
	 */
	reg = advk_readl(pcie, PCIE_CORE_PCIEXP_CAP + PCI_EXP_LNKCTL2);	/* [한국어] **같은 값을 Link Control 2 에도 쓴다.** 바로 위 상류 주석이 이유를 밝힌다 — 기능 명세는 기본값이 SPEED_GEN 을 따른다고 하지만 시험에서는 늘 8.0GT/s 였다 */
	reg &= ~PCI_EXP_LNKCTL2_TLS;	/* [한국어] 기존 목표 속도 필드를 지우고 */
	if (pcie->link_gen == 3)	/* [한국어] Gen3 이면 */
		reg |= PCI_EXP_LNKCTL2_TLS_8_0GT;	/* [한국어] 8.0GT/s, */
	else if (pcie->link_gen == 2)	/* [한국어] Gen2 이면 */
		reg |= PCI_EXP_LNKCTL2_TLS_5_0GT;	/* [한국어] 5.0GT/s, */
	else	/* [한국어] 그 밖이면 */
		reg |= PCI_EXP_LNKCTL2_TLS_2_5GT;	/* [한국어] 2.5GT/s 를 넣는다 */
	advk_writel(pcie, reg, PCIE_CORE_PCIEXP_CAP + PCI_EXP_LNKCTL2);	/* [한국어] 고친 값을 되쓴다 */

	/* Enable link training after selecting PCIe generation */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);	/* [한국어] **바로 위 상류 주석대로 세대를 고른 뒤에 훈련을 켠다** */
	reg |= LINK_TRAINING_EN;	/* [한국어] 링크 훈련 활성화 비트를 세워 */
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);	/* [한국어] 되쓴다. 이 순간부터 LTSSM 이 돈다 */

	/*
	 * Reset PCIe card via PERST# signal. Some cards are not detected
	 * during link training when they are in some non-initial state.
	 */
	advk_pcie_issue_perst(pcie);	/* [한국어] **PERST# 를 10ms 흔든다.** 바로 위 상류 주석대로 어떤 카드는 초기 상태가 아니면 링크 훈련에서 검출되지 않는다 */

	/*
	 * PERST# signal could have been asserted by pinctrl subsystem before
	 * probe() callback has been called or issued explicitly by reset gpio
	 * function advk_pcie_issue_perst(), making the endpoint going into
	 * fundamental reset. As required by PCI Express spec (PCI Express
	 * Base Specification, REV. 4.0 PCI Express, February 19 2014, 6.6.1
	 * Conventional Reset) a delay for at least 100ms after such a reset
	 * before sending a Configuration Request to the device is needed.
	 * So wait until PCIe link is up. Function advk_pcie_wait_for_link()
	 * waits for link at least 900ms.
	 */
	ret = advk_pcie_wait_for_link(pcie);	/* [한국어] **링크가 서기를 기다린다.** 바로 위 상류 주석대로 이 대기가 규격이 요구하는 100ms 를 함께 채운다 */
	if (ret < 0)	/* [한국어] 안 서면 */
		dev_err(dev, "link never came up\n");	/* [한국어] 그 사실을 알린다. **오류로 돌아가지는 않는다** — 카드가 없는 슬롯도 정상이기 때문이다 */
	else	/* [한국어] 섰으면 */
		dev_info(dev, "link up\n");	/* [한국어] 그 사실을 알린다 */
}

/*
 * Set PCIe address window register which could be used for memory
 * mapping.
 */
/* [한국어]
 * advk_pcie_set_ob_win - 아웃바운드 주소 창 하나를 프로그래밍한다
 *
 * @pcie:    이 드라이버 인스턴스.
 * @win_num: 창 번호(0 ~ OB_WIN_COUNT-1).
 * @match:   이 창이 가로챌 CPU 쪽 주소.
 * @remap:   그 주소를 PCIe 쪽에서 어느 주소로 바꿀지.
 * @mask:    창의 크기를 정하는 마스크.
 * @actions: 이 창으로 나가는 트랜잭션의 종류(메모리/IO/config 등).
 *
 * 바로 위 상류 주석대로 메모리 매핑에 쓸 수 있는 PCIe 주소 창
 * 레지스터를 설정한다.
 *
 * 주소가 64비트라 각 항목이 상위·하위 두 레지스터로 나뉘어 있다.
 * match 의 하위 워드에만 OB_WIN_ENABLE 비트를 함께 실어 창을 켠다 —
 * 즉 첫 쓰기 하나가 값 설정과 활성화를 겸한다.
 *
 * **대부분의 메모리 접근에는 이 창이 필요 없다.** setup_hw 의 상류
 * 주석대로 기본 아웃바운드 창 설정이 투명한 주소 변환을 해 주기 때문이며,
 * 이 함수는 메모리가 아닌 접근이나 투명하지 않은 변환이 필요한 범위에만
 * 쓰인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  advk_pcie_setup_hw() -> [이 함수] -> advk_writel() x7
 */
static void advk_pcie_set_ob_win(struct advk_pcie *pcie, u8 win_num,
				 phys_addr_t match, phys_addr_t remap,
				 phys_addr_t mask, u32 actions)
{
	advk_writel(pcie, OB_WIN_ENABLE |	/* [한국어] **match 하위 워드에 활성화 비트를 함께 실어** 값 설정과 켜기를 한 번에 한다 */
			  lower_32_bits(match), OB_WIN_MATCH_LS(win_num));	/* [한국어] 64비트 주소의 하위 절반 */
	advk_writel(pcie, upper_32_bits(match), OB_WIN_MATCH_MS(win_num));	/* [한국어] 상위 절반 */
	advk_writel(pcie, lower_32_bits(remap), OB_WIN_REMAP_LS(win_num));	/* [한국어] remap 하위 워드 */
	advk_writel(pcie, upper_32_bits(remap), OB_WIN_REMAP_MS(win_num));	/* [한국어] remap 상위 워드 */
	advk_writel(pcie, lower_32_bits(mask), OB_WIN_MASK_LS(win_num));	/* [한국어] mask 하위 워드. 하위 16비트가 0 이어야 하므로 창 최소 크기가 64KiB 다 */
	advk_writel(pcie, upper_32_bits(mask), OB_WIN_MASK_MS(win_num));	/* [한국어] mask 상위 워드 */
	advk_writel(pcie, actions, OB_WIN_ACTIONS(win_num));	/* [한국어] 이 창으로 나갈 트랜잭션의 종류(메모리 또는 I/O) */
}

/* [한국어]
 * advk_pcie_disable_ob_win - 아웃바운드 주소 창 하나를 끈다
 *
 * @pcie:    이 드라이버 인스턴스.
 * @win_num: 끌 창 번호.
 *
 * 일곱 레지스터를 모두 0 으로 쓴다. match 의 하위 워드에 있던
 * OB_WIN_ENABLE 비트도 함께 지워지므로 창이 꺼진다.
 *
 * 부르는 곳이 둘이다. setup_hw 가 장치 트리에서 온 창을 다 채운 뒤
 * 남은 창을 모두 끄고, probe 의 실패 경로와 remove 가 모든 창을 끈다 —
 * 남겨 두면 다음 부팅이나 다음 드라이버가 예상 밖 매핑을 보게 되기
 * 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   advk_pcie_setup_hw() / advk_pcie_probe() 실패 경로 / advk_pcie_remove()
 *     -> [이 함수] -> advk_writel() x7
 */
static void advk_pcie_disable_ob_win(struct advk_pcie *pcie, u8 win_num)
{
	advk_writel(pcie, 0, OB_WIN_MATCH_LS(win_num));	/* [한국어] **match 하위 워드를 0 으로 쓰면 활성화 비트도 함께 지워져 창이 꺼진다** */
	advk_writel(pcie, 0, OB_WIN_MATCH_MS(win_num));	/* [한국어] 나머지도 모두 0 으로 되돌린다 */
	advk_writel(pcie, 0, OB_WIN_REMAP_LS(win_num));	/* [한국어] remap 하위 */
	advk_writel(pcie, 0, OB_WIN_REMAP_MS(win_num));	/* [한국어] remap 상위 */
	advk_writel(pcie, 0, OB_WIN_MASK_LS(win_num));	/* [한국어] mask 하위 */
	advk_writel(pcie, 0, OB_WIN_MASK_MS(win_num));	/* [한국어] mask 상위 */
	advk_writel(pcie, 0, OB_WIN_ACTIONS(win_num));	/* [한국어] 종류까지 지운다 */
}

/* [한국어]
 * advk_pcie_setup_hw - 컨트롤러 레지스터 전체를 초기 상태로 만든다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * probe 가 자원을 모두 모은 뒤 부르는 하드웨어 초기화 본체다. 반환값이
 * 없어 실패를 알리지 않는다 — 레지스터 쓰기뿐이라 실패할 자리가 없다.
 *
 * 하는 일이 열 몇 가지이며, 상류 주석이 각 단계의 뜻을 밝힌다.
 *   - **레퍼런스 클럭 방향을 정한다.** 컨트롤러에서 엔드포인트 카드로
 *     나가는 방향이므로 송신을 켜고 수신을 끈다.
 *   - 컨트롤러를 Direct 모드로, 그리고 **RC(루트 컴플렉스) 모드**로 둔다.
 *   - **erratum 4.1 우회** — 장치·벤더 ID 값이 잘못되어 있다. 상류
 *     주석대로 잘못된 0x1b4b 를 올바른 0x11ab(Marvell)로 바꾼다.
 *     VENDOR_ID_REG 에 쓰면 읽기 전용인 DEV_ID_REG 의 벤더 ID 되읽기
 *     값이 바뀐다.
 *   - **클래스 코드를 PCI 브리지로 바꾼다.** 기본값이 대용량 저장 장치
 *     컨트롤러(0x010400)라 그대로 두면 PCI 코어가 루트 포트로 보지
 *     않는다. 그 자리의 긴 상류 주석이 이 드라이버가 브리지를 소프트웨어로
 *     흉내 내는 이유까지 함께 밝힌다 — 이 브리지는 규격에 맞는 Type 1
 *     config 공간이 없고, Aardvark 의 config 접근 방식으로 읽을 수조차
 *     없으며, 내부 레지스터의 0x10~0x34 범위는 전혀 다른 레지스터다.
 *   - 루트 브리지의 I/O·메모리 공간과 버스 마스터링을 **꺼 둔다.** 나중에
 *     에뮬레이션 브리지를 통해 PCI 코어가 켠다.
 *   - AER 의 ECRC 검사 넷을 켠다.
 *   - Device Control 에서 relaxed ordering 과 no-snoop 을 끄고,
 *     **최대 페이로드와 최대 읽기 요청을 512바이트로 정한다.**
 *   - Control 2 에서 strict ordering 을 끄고 TD(다이제스트)를 켠다.
 *   - **레인 수를 1 로 고정한다**(x1).
 *   - **MSI 목적지 주소를 등록한다.** virt_to_phys(pcie) 즉 이 드라이버
 *     구조체 자신의 물리 주소를 쓴다 — 실제로 그 메모리에 쓰이는 것이
 *     아니라, 컨트롤러가 그 주소로 가는 쓰기를 가로채 MSI 로 해석한다.
 *   - 인터럽트 상태를 지우고 마스크를 푼다.
 *   - **아웃바운드 창을 설정한다.** 상류 주석들이 밝히듯 기본 창 설정이
 *     투명한 주소 변환을 해 주므로 메모리 접근에는 창이 필요 없고,
 *     PIO 는 자기 레지스터에 정보를 다 담으므로 창 매핑을 우회한다.
 *     장치 트리에서 온 것만 채우고 나머지는 끈다.
 *   - 마지막으로 링크를 세운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). 마지막의 링크 훈련에서
 * 1초 가까이 잠든다.
 *
 * 호출 체인:
 *   advk_pcie_probe() -> [이 함수] -> advk_readl()/advk_writel()
 *     -> advk_pcie_set_ob_win() -> advk_pcie_disable_ob_win()
 *     -> advk_pcie_train_link()
 */
static void advk_pcie_setup_hw(struct advk_pcie *pcie)
{
	phys_addr_t msi_addr;	/* [한국어] MSI 목적지로 등록할 물리 주소 */
	u32 reg;	/* [한국어] 읽고 고칠 레지스터 값 */
	int i;	/* [한국어] 창 순회 인덱스 */

	/*
	 * Configure PCIe Reference clock. Direction is from the PCIe
	 * controller to the endpoint card, so enable transmitting of
	 * Reference clock differential signal off-chip and disable
	 * receiving off-chip differential signal.
	 */
	reg = advk_readl(pcie, PCIE_CORE_REF_CLK_REG);	/* [한국어] **레퍼런스 클럭 방향을 정한다.** 바로 위 상류 주석대로 컨트롤러에서 카드로 나가는 방향이다 */
	reg |= PCIE_CORE_REF_CLK_TX_ENABLE;	/* [한국어] 송신을 켜고 */
	reg &= ~PCIE_CORE_REF_CLK_RX_ENABLE;	/* [한국어] 수신을 끈다 */
	advk_writel(pcie, reg, PCIE_CORE_REF_CLK_REG);	/* [한국어] 고친 값을 되쓴다 */

	/* Set to Direct mode */
	reg = advk_readl(pcie, CTRL_CONFIG_REG);	/* [한국어] 컨트롤러 동작 모드를 정한다 */
	reg &= ~(CTRL_MODE_MASK << CTRL_MODE_SHIFT);	/* [한국어] 기존 모드 필드를 지우고 */
	reg |= ((PCIE_CORE_MODE_DIRECT & CTRL_MODE_MASK) << CTRL_MODE_SHIFT);	/* [한국어] Direct 모드 값을 넣는다 */
	advk_writel(pcie, reg, CTRL_CONFIG_REG);	/* [한국어] 고친 값을 되쓴다 */

	/* Set PCI global control register to RC mode */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);	/* [한국어] **컨트롤러를 루트 컴플렉스로 둔다** */
	reg |= (IS_RC_MSK << IS_RC_SHIFT);	/* [한국어] RC 비트를 세우고 */
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);	/* [한국어] 되쓴다 */

	/*
	 * Replace incorrect PCI vendor id value 0x1b4b by correct value 0x11ab.
	 * VENDOR_ID_REG contains vendor id in low 16 bits and subsystem vendor
	 * id in high 16 bits. Updating this register changes readback value of
	 * read-only vendor id bits in PCIE_CORE_DEV_ID_REG register. Workaround
	 * for erratum 4.1: "The value of device and vendor ID is incorrect".
	 */
	reg = (PCI_VENDOR_ID_MARVELL << 16) | PCI_VENDOR_ID_MARVELL;	/* [한국어] **erratum 4.1 우회** — 바로 위 상류 주석대로 장치·벤더 ID 값이 잘못되어 있다. 상위 16비트가 서브시스템 벤더, 하위 16비트가 벤더이므로 Marvell ID 를 양쪽에 넣는다 */
	advk_writel(pcie, reg, VENDOR_ID_REG);	/* [한국어] 여기 쓰면 읽기 전용인 DEV_ID_REG 의 벤더 ID 되읽기 값이 바뀐다 */

	/*
	 * Change Class Code of PCI Bridge device to PCI Bridge (0x600400),
	 * because the default value is Mass storage controller (0x010400).
	 *
	 * Note that this Aardvark PCI Bridge does not have compliant Type 1
	 * Configuration Space and it even cannot be accessed via Aardvark's
	 * PCI config space access method. Something like config space is
	 * available in internal Aardvark registers starting at offset 0x0
	 * and is reported as Type 0. In range 0x10 - 0x34 it has totally
	 * different registers.
	 *
	 * Therefore driver uses emulation of PCI Bridge which emulates
	 * access to configuration space via internal Aardvark registers or
	 * emulated configuration buffer.
	 */
	reg = advk_readl(pcie, PCIE_CORE_DEV_REV_REG);	/* [한국어] **클래스 코드를 PCI 브리지로 바꾼다.** 바로 위 상류 주석대로 기본값이 대용량 저장 장치 컨트롤러라 그대로 두면 PCI 코어가 루트 포트로 보지 않는다 */
	reg &= ~0xffffff00;	/* [한국어] 상위 24비트(클래스 코드 자리)를 지우고 */
	reg |= PCI_CLASS_BRIDGE_PCI_NORMAL << 8;	/* [한국어] 브리지 클래스를 8비트 밀어 넣는다 — 하위 8비트는 개정 번호라 건드리지 않는다 */
	advk_writel(pcie, reg, PCIE_CORE_DEV_REV_REG);	/* [한국어] 고친 값을 되쓴다 */

	/* Disable Root Bridge I/O space, memory space and bus mastering */
	reg = advk_readl(pcie, PCIE_CORE_CMD_STATUS_REG);	/* [한국어] **루트 브리지의 공간 디코딩과 버스 마스터링을 꺼 둔다.** 나중에 에뮬레이션 브리지를 통해 PCI 코어가 켠다 */
	reg &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);	/* [한국어] 세 비트를 지우고 */
	advk_writel(pcie, reg, PCIE_CORE_CMD_STATUS_REG);	/* [한국어] 되쓴다 */

	/* Set Advanced Error Capabilities and Control PF0 register */
	reg = PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX |	/* [한국어] **AER 의 ECRC 검사 넷을 함께 켠다.** 송신 검사 능력과 */
		PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX_EN |	/* [한국어] 그 활성화, */
		PCIE_CORE_ERR_CAPCTL_ECRC_CHCK |	/* [한국어] 검사 능력과 */
		PCIE_CORE_ERR_CAPCTL_ECRC_CHCK_RCV;	/* [한국어] 수신 검사 활성화 */
	advk_writel(pcie, reg, PCIE_CORE_ERR_CAPCTL_REG);	/* [한국어] 네 비트를 한 번에 쓴다 — 읽지 않고 덮어쓴다 */

	/* Set PCIe Device Control register */
	reg = advk_readl(pcie, PCIE_CORE_PCIEXP_CAP + PCI_EXP_DEVCTL);	/* [한국어] PCI Express Device Control 을 손본다 */
	reg &= ~PCI_EXP_DEVCTL_RELAX_EN;	/* [한국어] relaxed ordering 을 끄고 */
	reg &= ~PCI_EXP_DEVCTL_NOSNOOP_EN;	/* [한국어] no-snoop 도 끈다 — 캐시 일관성을 하드웨어에 맡기지 않겠다는 뜻이다 */
	reg &= ~PCI_EXP_DEVCTL_PAYLOAD;	/* [한국어] 기존 최대 페이로드 필드를 지우고 */
	reg &= ~PCI_EXP_DEVCTL_READRQ;	/* [한국어] 기존 최대 읽기 요청 필드도 지운 뒤 */
	reg |= PCI_EXP_DEVCTL_PAYLOAD_512B;	/* [한국어] **최대 페이로드 512바이트** */
	reg |= PCI_EXP_DEVCTL_READRQ_512B;	/* [한국어] **최대 읽기 요청 512바이트**로 정한다 */
	advk_writel(pcie, reg, PCIE_CORE_PCIEXP_CAP + PCI_EXP_DEVCTL);	/* [한국어] 고친 값을 되쓴다 */

	/* Program PCIe Control 2 to disable strict ordering */
	reg = PCIE_CORE_CTRL2_RESERVED |	/* [한국어] 제어 2 는 읽지 않고 덮어쓴다 — 예약 비트들과 */
		PCIE_CORE_CTRL2_TD_ENABLE;	/* [한국어] TD(TLP 다이제스트)만 켜고 **strict ordering 은 빼서 끈다** */
	advk_writel(pcie, reg, PCIE_CORE_CTRL2_REG);	/* [한국어] 그 값을 쓴다 */

	/* Set lane X1 */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);	/* [한국어] **레인 수를 고정한다** */
	reg &= ~LANE_CNT_MSK;	/* [한국어] 기존 레인 필드를 지우고 */
	reg |= LANE_COUNT_1;	/* [한국어] x1 을 넣는다 */
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);	/* [한국어] 고친 값을 되쓴다 */

	/* Set MSI address */
	msi_addr = virt_to_phys(pcie);	/* [한국어] **MSI 목적지 주소로 이 드라이버 구조체 자신의 물리 주소를 쓴다.** 실제로 그 메모리가 쓰이는 것이 아니라, 컨트롤러가 그 주소로 가는 쓰기를 가로채 MSI 로 해석한다. advk_msi_irq_compose_msi_msg() 가 장치에 같은 주소를 알려 준다 */
	advk_writel(pcie, lower_32_bits(msi_addr), PCIE_MSI_ADDR_LOW_REG);	/* [한국어] 하위 워드 */
	advk_writel(pcie, upper_32_bits(msi_addr), PCIE_MSI_ADDR_HIGH_REG);	/* [한국어] 상위 워드 */

	/* Enable MSI */
	reg = advk_readl(pcie, PCIE_CORE_CTRL2_REG);	/* [한국어] MSI 를 켠다 */
	reg |= PCIE_CORE_CTRL2_MSI_ENABLE;	/* [한국어] MSI 활성화 비트를 세워 */
	advk_writel(pcie, reg, PCIE_CORE_CTRL2_REG);	/* [한국어] 되쓴다 */

	/* Clear all interrupts */
	advk_writel(pcie, PCIE_MSI_ALL_MASK, PCIE_MSI_STATUS_REG);	/* [한국어] **먼저 남아 있던 인터럽트 상태를 모두 지운다** — MSI */
	advk_writel(pcie, PCIE_ISR0_ALL_MASK, PCIE_ISR0_REG);	/* [한국어] ISR0 */
	advk_writel(pcie, PCIE_ISR1_ALL_MASK, PCIE_ISR1_REG);	/* [한국어] ISR1 */
	advk_writel(pcie, PCIE_IRQ_ALL_MASK, HOST_CTRL_INT_STATUS_REG);	/* [한국어] 상위 상태까지 */

	/* Disable All ISR0/1 and MSI Sources */
	advk_writel(pcie, PCIE_ISR0_ALL_MASK, PCIE_ISR0_MASK_REG);	/* [한국어] **그 다음 전부 마스크한다** — ISR0 */
	advk_writel(pcie, PCIE_ISR1_ALL_MASK, PCIE_ISR1_MASK_REG);	/* [한국어] ISR1 */
	advk_writel(pcie, PCIE_MSI_ALL_MASK, PCIE_MSI_MASK_REG);	/* [한국어] MSI. 이렇게 해 두고 아래에서 필요한 것만 연다 */

	/* Unmask summary MSI interrupt */
	reg = advk_readl(pcie, PCIE_ISR0_MASK_REG);	/* [한국어] **MSI 대기 알림만 연다** */
	reg &= ~PCIE_ISR0_MSI_INT_PENDING;	/* [한국어] 그 비트의 마스크를 지워 */
	advk_writel(pcie, reg, PCIE_ISR0_MASK_REG);	/* [한국어] 되쓴다 */

	/* Unmask PME interrupt for processing of PME requester */
	reg = advk_readl(pcie, PCIE_ISR0_MASK_REG);	/* [한국어] **PME 알림도 연다** */
	reg &= ~PCIE_MSG_PM_PME_MASK;	/* [한국어] 그 비트의 마스크를 지워 */
	advk_writel(pcie, reg, PCIE_ISR0_MASK_REG);	/* [한국어] 되쓴다 */

	/* Enable summary interrupt for GIC SPI source */
	reg = PCIE_IRQ_ALL_MASK & (~PCIE_IRQ_ENABLE_INTS_MASK);	/* [한국어] **상위에서는 코어 인터럽트 하나만 연다.** 전체 마스크에서 그 비트만 빼는 방식이다 */
	advk_writel(pcie, reg, HOST_CTRL_INT_MASK_REG);	/* [한국어] 그 값을 마스크 레지스터에 쓴다 */

	/*
	 * Enable AXI address window location generation:
	 * When it is enabled, the default outbound window
	 * configurations (Default User Field: 0xD0074CFC)
	 * are used to transparent address translation for
	 * the outbound transactions. Thus, PCIe address
	 * windows are not required for transparent memory
	 * access when default outbound window configuration
	 * is set for memory access.
	 */
	reg = advk_readl(pcie, PCIE_CORE_CTRL2_REG);	/* [한국어] **아웃바운드 창 기능을 켠다.** 바로 위 상류 주석대로 켜면 기본 창 설정(Default User Field)이 투명한 주소 변환을 해 주므로, 메모리 접근에는 창을 따로 잡을 필요가 없다 */
	reg |= PCIE_CORE_CTRL2_OB_WIN_ENABLE;	/* [한국어] 그 비트를 세워 */
	advk_writel(pcie, reg, PCIE_CORE_CTRL2_REG);	/* [한국어] 되쓴다 */

	/*
	 * Set memory access in Default User Field so it
	 * is not required to configure PCIe address for
	 * transparent memory access.
	 */
	advk_writel(pcie, OB_WIN_TYPE_MEM, OB_WIN_DEFAULT_ACTIONS);	/* [한국어] **기본 창의 종류를 메모리로 둔다.** 바로 위 상류 주석대로 그러면 투명한 메모리 접근에 PCIe 주소를 따로 설정할 필요가 없다 */

	/*
	 * Bypass the address window mapping for PIO:
	 * Since PIO access already contains all required
	 * info over AXI interface by PIO registers, the
	 * address window is not required.
	 */
	reg = advk_readl(pcie, PIO_CTRL);	/* [한국어] **PIO 는 주소 창 매핑을 우회하게 한다.** 바로 위 상류 주석대로 PIO 접근은 자기 레지스터에 필요한 정보를 다 담으므로 창이 필요 없다 */
	reg |= PIO_CTRL_ADDR_WIN_DISABLE;	/* [한국어] 우회 비트를 세워 */
	advk_writel(pcie, reg, PIO_CTRL);	/* [한국어] 되쓴다 */

	/*
	 * Configure PCIe address windows for non-memory or
	 * non-transparent access as by default PCIe uses
	 * transparent memory access.
	 */
	for (i = 0; i < pcie->wins_count; i++)	/* [한국어] **장치 트리에서 온 창만 설정한다.** 바로 위 상류 주석대로 메모리가 아니거나 투명하지 않은 접근에만 창이 필요하다 */
		advk_pcie_set_ob_win(pcie, i,	/* [한국어] 창 하나를 프로그래밍한다 */
				     pcie->wins[i].match, pcie->wins[i].remap,	/* [한국어] match 와 remap */
				     pcie->wins[i].mask, pcie->wins[i].actions);	/* [한국어] mask 와 종류 */

	/* Disable remaining PCIe outbound windows */
	for (i = pcie->wins_count; i < OB_WIN_COUNT; i++)	/* [한국어] **나머지 창은 모두 끈다** — 남겨 두면 예상 밖 매핑이 살아 있게 된다 */
		advk_pcie_disable_ob_win(pcie, i);	/* [한국어] 하나씩 지운다 */

	advk_pcie_train_link(pcie);	/* [한국어] 마지막으로 링크를 세운다 */
}

/* [한국어]
 * advk_pcie_check_pio_status - PIO 트랜잭션의 완료 상태를 규격에 맞게 해석한다
 *
 * @pcie:      이 드라이버 인스턴스.
 * @allow_rrs: RRS 를 정상 결과로 볼 수 있는 상황인지. 읽기에서 Vendor ID
 *             두 바이트를 포함하고 루트 포트의 RRS Software Visibility 가
 *             켜져 있을 때만 참이다.
 * @val:       읽기 결과를 담을 자리. 쓰기 경로에서는 NULL 이다.
 * @return: 0 이면 성공. UR 이면 -EOPNOTSUPP, RRS 재시도가 필요하면
 *          -EAGAIN, CA 면 -ECANCELED, 오류 비트가 서 있으면 -EFAULT,
 *          알 수 없는 상태면 -EINVAL.
 *
 * **완료 상태 필드만으로는 성패를 알 수 없다.** 바로 안쪽 상류 주석이
 * 하드웨어 명세의 다섯 규칙을 그대로 옮겨 두었다.
 *   1. 상태가 성공이어도 오류 비트(bit11)를 함께 봐야 한다.
 *   2. UR 은 쓰기에서만 오류이고, 읽기에서는 0xFFFFFFFF 를 읽은 정상
 *      결과다.
 *   3. RRS 도 쓰기에서만 오류이고, 읽기에서는 0xFFFF0001 을 읽은 정상
 *      결과다.
 *   4. CA 는 읽기·쓰기 모두 오류다.
 *   5. 그 밖은 알 수 없음으로 다룬다.
 *
 * RRS 갈래의 두 상류 주석이 규격(PCIe r6.0, 2.3.2)을 인용한다. RRS
 * Software Visibility 가 켜져 있고 요청이 Vendor ID 두 바이트를 포함하면
 * 루트 컴플렉스가 Vendor ID 자리에 0001h 를, 나머지에 전부 1 을 돌려주어야
 * 한다 — 그것이 CFG_RD_RRS_VAL 이다. 켜져 있지 않거나 그 밖의 요청이면
 * 루트 컴플렉스가 요청을 새 요청으로 다시 내야 하며, 구현이 재시도 횟수를
 * 제한해도 된다고 되어 있다. 그래서 -EAGAIN 을 돌려주고 호출자가
 * PIO_RETRY_CNT 까지 다시 낸다.
 *
 * 오류가 있을 때만 마지막에 로그를 남기며, 그 트랜잭션이 posted 였는지
 * non-posted 였는지와 대상 주소를 함께 찍는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 *
 * 호출 체인:
 *   advk_pcie_rd_conf() / advk_pcie_wr_conf() -> [이 함수] -> advk_readl()
 */
static int advk_pcie_check_pio_status(struct advk_pcie *pcie, bool allow_rrs, u32 *val)
{
	struct device *dev = &pcie->pdev->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	u32 reg;	/* [한국어] 읽은 상태 레지스터 원본. 오류 비트와 posted 여부가 여기 남아 있다 */
	unsigned int status;	/* [한국어] 거기서 떼어 낸 완료 상태 값 */
	char *strcomp_status, *str_posted;	/* [한국어] 로그에 찍을 상태 이름과 posted 여부 문자열 */
	int ret;	/* [한국어] 호출자에게 돌려줄 값 */

	reg = advk_readl(pcie, PIO_STAT);	/* [한국어] PIO 완료 상태 레지스터를 읽고 */
	status = (reg & PIO_COMPLETION_STATUS_MASK) >>	/* [한국어] 완료 상태 필드만 떼어 */
		PIO_COMPLETION_STATUS_SHIFT;	/* [한국어] 제자리로 민다 */

	/*
	 * According to HW spec, the PIO status check sequence as below:
	 * 1) even if COMPLETION_STATUS(bit9:7) indicates successful,
	 *    it still needs to check Error Status(bit11), only when this bit
	 *    indicates no error happen, the operation is successful.
	 * 2) value Unsupported Request(1) of COMPLETION_STATUS(bit9:7) only
	 *    means a PIO write error, and for PIO read it is successful with
	 *    a read value of 0xFFFFFFFF.
	 * 3) value Config Request Retry Status(RRS) of COMPLETION_STATUS(bit9:7)
	 *    only means a PIO write error, and for PIO read it is successful
	 *    with a read value of 0xFFFF0001.
	 * 4) value Completer Abort (CA) of COMPLETION_STATUS(bit9:7) means
	 *    error for both PIO read and PIO write operation.
	 * 5) other errors are indicated as 'unknown'.
	 */
	switch (status) {	/* [한국어] **완료 상태에 따라 갈린다.** 바로 위 상류 주석이 하드웨어 명세의 다섯 규칙을 옮겨 두었다 */
	case PIO_COMPLETION_STATUS_OK:	/* [한국어] 상태가 성공이면 */
		if (reg & PIO_ERR_STATUS) {	/* [한국어] **그래도 오류 비트를 봐야 한다** — 규칙 1 */
			strcomp_status = "COMP_ERR";	/* [한국어] 완료 오류로 이름 붙이고 */
			ret = -EFAULT;	/* [한국어] -EFAULT 로 끝낸다 */
			break;	/* [한국어] 갈래를 벗어난다 */
		}
		/* Get the read result */
		if (val)	/* [한국어] 읽기 경로이면(val 이 있으면) */
			*val = advk_readl(pcie, PIO_RD_DATA);	/* [한국어] **읽은 값을 가져온다** */
		/* No error */
		strcomp_status = NULL;	/* [한국어] 로그를 남기지 않도록 이름을 비우고 */
		ret = 0;	/* [한국어] 성공으로 둔다 */
		break;	/* [한국어] 갈래를 벗어난다 */
	case PIO_COMPLETION_STATUS_UR:	/* [한국어] 상태가 UR 이면 — 규칙 2 */
		strcomp_status = "UR";	/* [한국어] 이름을 붙이고 */
		ret = -EOPNOTSUPP;	/* [한국어] 지원하지 않음으로 끝낸다. 읽기에서 전부 1 을 읽는 것은 호출자가 따로 다룬다 */
		break;	/* [한국어] 갈래를 벗어난다 */
	case PIO_COMPLETION_STATUS_RRS:	/* [한국어] 상태가 RRS 이면 — 규칙 3 */
		if (allow_rrs && val) {	/* [한국어] **정상으로 볼 수 있는 조건이면**(읽기이고 RRS 가시성이 켜져 있으면) */
			/* PCIe r6.0, sec 2.3.2, says:
			 * If Configuration RRS Software Visibility is enabled:
			 * For a Configuration Read Request that includes both
			 * bytes of the Vendor ID field of a device Function's
			 * Configuration Space Header, the Root Complex must
			 * complete the Request to the host by returning a
			 * read-data value of 0001h for the Vendor ID field and
			 * all '1's for any additional bytes included in the
			 * request.
			 *
			 * So RRS in this case is not an error status.
			 */
			*val = CFG_RD_RRS_VAL;	/* [한국어] 바로 위 상류 주석이 인용한 규격대로 Vendor ID 자리에 0001h, 나머지에 전부 1 인 값을 돌려준다 */
			strcomp_status = NULL;	/* [한국어] 로그를 남기지 않도록 이름을 비우고 */
			ret = 0;	/* [한국어] 성공으로 둔다 */
			break;	/* [한국어] 갈래를 벗어난다 */
		}
		/* PCIe r6.0, sec 2.3.2, says:
		 * If RRS Software Visibility is not enabled, the Root Complex
		 * must re-issue the Configuration Request as a new Request.
		 * If RRS Software Visibility is enabled: For a Configuration
		 * Write Request or for any other Configuration Read Request,
		 * the Root Complex must re-issue the Configuration Request as
		 * a new Request.
		 * A Root Complex implementation may choose to limit the number
		 * of Configuration Request/RRS Completion Status loops before
		 * determining that something is wrong with the target of the
		 * Request and taking appropriate action, e.g., complete the
		 * Request to the host as a failed transaction.
		 *
		 * So return -EAGAIN and caller (pci-aardvark.c driver) will
		 * re-issue request again up to the PIO_RETRY_CNT retries.
		 */
		strcomp_status = "RRS";	/* [한국어] 그 조건이 아니면 이름을 붙이고 */
		ret = -EAGAIN;	/* [한국어] **-EAGAIN 으로 재시도를 요청한다.** 바로 위 상류 주석대로 규격이 루트 컴플렉스에 요청을 새로 내도록 하며, 재시도 횟수를 제한해도 된다고 한다 */
		break;	/* [한국어] 갈래를 벗어난다 */
	case PIO_COMPLETION_STATUS_CA:	/* [한국어] 상태가 CA 이면 — 규칙 4 */
		strcomp_status = "CA";	/* [한국어] 이름을 붙이고 */
		ret = -ECANCELED;	/* [한국어] 취소됨으로 끝낸다. **읽기·쓰기 모두 오류다** */
		break;	/* [한국어] 갈래를 벗어난다 */
	default:	/* [한국어] 그 밖의 상태이면 — 규칙 5 */
		strcomp_status = "Unknown";	/* [한국어] 알 수 없음으로 이름 붙이고 */
		ret = -EINVAL;	/* [한국어] 인자 오류로 끝낸다 */
		break;	/* [한국어] 갈래를 벗어난다 */
	}

	if (!strcomp_status)	/* [한국어] **이름이 비어 있으면 정상 결과다** */
		return ret;	/* [한국어] 로그 없이 그대로 돌아간다 */

	if (reg & PIO_NON_POSTED_REQ)	/* [한국어] non-posted 요청이었으면 */
		str_posted = "Non-posted";	/* [한국어] 그렇게 적고 */
	else	/* [한국어] 아니면 */
		str_posted = "Posted";	/* [한국어] posted 로 적는다 */

	dev_dbg(dev, "%s PIO Response Status: %s, %#x @ %#x\n",	/* [한국어] 무엇이 어떻게 끝났는지 남긴다 */
		str_posted, strcomp_status, reg, advk_readl(pcie, PIO_ADDR_LS));	/* [한국어] posted 여부, 상태 이름, 원본 레지스터 값, 그리고 **대상 주소**를 함께 찍는다 */

	return ret;	/* [한국어] 담아 둔 결과를 돌려준다 */
}

/* [한국어]
 * advk_pcie_wait_pio - PIO 트랜잭션이 끝나기를 폴링으로 기다린다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 양수이면 성공이며 **몇 번 만에 끝났는지**를 돌려준다. 시간이
 *          지나면 -ETIMEDOUT.
 *
 * ECAM 컨트롤러와 달리 config 접근이 곧바로 끝나지 않으므로 완료를
 * 기다려야 한다. 완료 조건은 두 가지가 함께 성립하는 것이다 —
 * PIO_START 가 0 으로 내려갔고(하드웨어가 시작 비트를 스스로 지운다)
 * PIO_ISR 이 1 로 올라온 것(완료 인터럽트 상태).
 *
 * 2us 간격으로 최대 750000회이므로 **상한이 약 1.5초**다.
 * PIO_RETRY_CNT 정의 옆의 상류 주석이 그 값을 "1.5 s" 로 적어 두었다.
 *
 * **반환값이 횟수라는 점이 중요하다.** 호출자가 그것을 retry_count 에
 * 누적해, RRS 재시도를 포함한 전체 시도 횟수가 같은 상한을 넘지 않도록
 * 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. udelay 로 바쁜 대기를 하므로 최악의
 * 경우 1.5초 동안 CPU 를 점유한다.
 *
 * 호출 체인:
 *   advk_pcie_rd_conf() / advk_pcie_wr_conf() -> [이 함수] -> advk_readl()
 */
static int advk_pcie_wait_pio(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	int i;	/* [한국어] 반복 횟수. **1부터 세는 이유는 이 값을 그대로 돌려주기 때문이다** */

	for (i = 1; i <= PIO_RETRY_CNT; i++) {	/* [한국어] 최대 750000회 본다 */
		u32 start, isr;	/* [한국어] 시작 비트와 완료 상태를 담을 자리 */

		start = advk_readl(pcie, PIO_START);	/* [한국어] **시작 비트를 읽는다** — 완료되면 하드웨어가 스스로 지운다 */
		isr = advk_readl(pcie, PIO_ISR);	/* [한국어] 완료 인터럽트 상태도 읽는다 */
		if (!start && isr)	/* [한국어] **둘이 함께 성립해야 완료다** — 시작이 내려갔고 완료가 올라온 것 */
			return i;	/* [한국어] 몇 번 만에 끝났는지를 돌려준다. 호출자가 이 값을 재시도 횟수에 누적한다 */
		udelay(PIO_RETRY_DELAY);	/* [한국어] 2us 쉬고 다시 본다. 750000회면 약 1.5초다 */
	}

	dev_err(dev, "PIO read/write transfer time out\n");	/* [한국어] 상한 안에 안 끝나면 알리고 */
	return -ETIMEDOUT;	/* [한국어] 시간 초과로 돌아간다 */
}

static pci_bridge_emul_read_status_t
/* [한국어]
 * advk_pci_bridge_emul_base_conf_read - 에뮬레이션 브리지의 기본 config 헤더 읽기
 *
 * @bridge: 에뮬레이션 브리지 구조체. 그 안의 data 에 이 드라이버
 *          인스턴스가 들어 있다.
 * @reg:    읽을 오프셋.
 * @value:  읽은 값을 담을 자리.
 * @return: PCI_BRIDGE_EMUL_HANDLED 이면 이 함수가 처리했다는 뜻이고,
 *          PCI_BRIDGE_EMUL_NOT_HANDLED 이면 에뮬레이션 층이 자기
 *          버퍼에서 처리하라는 뜻이다.
 *
 * pci_bridge_emul 이 루트 포트의 type-1 헤더를 흉내 내는데, 그중 일부
 * 필드는 소프트웨어 버퍼가 아니라 **실제 하드웨어 레지스터**에서 와야
 * 한다. 그런 필드만 이 콜백이 가로채고 나머지는 NOT_HANDLED 로 넘긴다.
 *
 이 함수가 가로채는 것은 둘뿐이다.
 *   PCI_COMMAND        — 하드웨어의 CMD_STATUS 레지스터를 그대로 읽는다.
 *   PCI_INTERRUPT_LINE — 이 32비트 워드의 상위 절반이 Bridge Control 이다.
 *     그 자리의 상류 주석대로 **하드웨어에서 읽어 오는 것은 두 비트뿐**
 *     이고(BUS_RESET 과 SERR) 나머지는 에뮬레이션 버퍼 값을 쓴다.
 *     SERR 는 ISR0 마스크의 ERR 비트를 **뒤집어** 만든다 — 마스크가
 *     걸려 있으면 SERR 가 꺼진 것이다. BUS_RESET 은 CTRL1 의
 *     HOT_RESET_GEN 을 그대로 반영한다.
 *
 * 이 하드웨어의 내부 레지스터가 type-1 헤더와 배치가 달라 이런 중계가
 * 필요하다 — setup_hw 의 상류 주석이 그 사정을 밝힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 코어의 config 읽기).
 *
 * 호출 체인:
 *   PCI 코어 -> advk_pcie_rd_conf() -> pci_bridge_emul_conf_read()
 *     -> ops->read_base -> [이 함수] -> advk_readl()
 */
advk_pci_bridge_emul_base_conf_read(struct pci_bridge_emul *bridge,
				    int reg, u32 *value)
{
	struct advk_pcie *pcie = bridge->data;	/* [한국어] 에뮬레이션 브리지의 data 에 이 드라이버 인스턴스가 들어 있다 */

	switch (reg) {	/* [한국어] **오프셋별로 갈린다. 여기서 처리하지 않는 것은 에뮬레이션 층의 버퍼가 답한다** */
	case PCI_COMMAND:	/* [한국어] 명령·상태 레지스터이면 */
		*value = advk_readl(pcie, PCIE_CORE_CMD_STATUS_REG);	/* [한국어] 하드웨어 값을 그대로 읽어 */
		return PCI_BRIDGE_EMUL_HANDLED;	/* [한국어] 처리했다고 알린다 */

	case PCI_INTERRUPT_LINE: {	/* [한국어] 인터럽트 라인 워드이면 — **이 32비트의 상위 절반이 Bridge Control 이다** */
		/*
		 * From the whole 32bit register we support reading from HW only
		 * two bits: PCI_BRIDGE_CTL_BUS_RESET and PCI_BRIDGE_CTL_SERR.
		 * Other bits are retrieved only from emulated config buffer.
		 */
		__le32 *cfgspace = (__le32 *)&bridge->conf;	/* [한국어] 에뮬레이션 config 버퍼를 워드 배열로 본다 */
		u32 val = le32_to_cpu(cfgspace[PCI_INTERRUPT_LINE / 4]);	/* [한국어] 그 워드의 현재 값을 가져온다. 바로 위 상류 주석대로 하드웨어에서 읽는 것은 두 비트뿐이고 나머지는 이 버퍼 값을 쓴다 */
		if (advk_readl(pcie, PCIE_ISR0_MASK_REG) & PCIE_ISR0_ERR_MASK)	/* [한국어] **ISR0 마스크에 오류 마스크가 걸려 있으면** */
			val &= ~(PCI_BRIDGE_CTL_SERR << 16);	/* [한국어] SERR 를 끈 것으로 보고 그 비트를 지운다 — 마스크와 SERR 의 뜻이 반대다 */
		else	/* [한국어] 마스크가 없으면 */
			val |= PCI_BRIDGE_CTL_SERR << 16;	/* [한국어] SERR 가 켜진 것으로 보고 비트를 세운다 */
		if (advk_readl(pcie, PCIE_CORE_CTRL1_REG) & HOT_RESET_GEN)	/* [한국어] CTRL1 에 핫 리셋 비트가 서 있으면 */
			val |= PCI_BRIDGE_CTL_BUS_RESET << 16;	/* [한국어] Bridge Control 의 BUS_RESET 비트를 세우고 */
		else	/* [한국어] 아니면 */
			val &= ~(PCI_BRIDGE_CTL_BUS_RESET << 16);	/* [한국어] 지운다. 이쪽은 뜻이 그대로 대응한다 */
		*value = val;	/* [한국어] 만든 값을 돌려주고 */
		return PCI_BRIDGE_EMUL_HANDLED;	/* [한국어] 처리했다고 알린다 */
	}

	default:	/* [한국어] 그 밖의 오프셋은 */
		return PCI_BRIDGE_EMUL_NOT_HANDLED;	/* [한국어] 에뮬레이션 층이 자기 버퍼로 처리하게 넘긴다 */
	}
}

static void
/* [한국어]
 * advk_pci_bridge_emul_base_conf_write - 에뮬레이션 브리지의 기본 config 헤더 쓰기
 *
 * @bridge: 에뮬레이션 브리지 구조체.
 * @reg:    쓸 오프셋.
 * @old:    이전 값.
 * @new:    쓸 값.
 * @mask:   이번 쓰기에서 유효한 비트.
 *
 * 읽기 쪽의 짝이며 다루는 필드도 같다.
 *   PCI_COMMAND        — 새 값을 CMD_STATUS 레지스터에 그대로 쓴다.
 *   PCI_INTERRUPT_LINE — Bridge Control 의 두 비트를 각각 옮긴다.
 *     SERR 는 ISR0 마스크의 ERR 비트를 뒤집어 반영한다. 그 자리의 상류
 *     주석이 근거를 밝힌다 — PCIe 규격의 Figure 6-3(오류 메시지 제어의
 *     의사 논리도)에서 Bridge Control 의 SERR# Enable 이 ERR_* 메시지
 *     수신을 켜는 역할을 한다. BUS_RESET 은 CTRL1 의 HOT_RESET_GEN 에
 *     그대로 반영해 **핫 리셋을 실제로 발생시킨다.**
 *
 * **mask 를 함께 받는 이유**는 config 쓰기가 바이트 단위일 수 있기
 * 때문이다. 에뮬레이션 층이 이전 값과 새 값, 그리고 어느 비트가 실제로
 * 쓰이는지를 모두 넘겨 주므로, 콜백은 관심 있는 비트만 골라 반영할 수
 * 있다.
 *
 * 반환값이 없다 — 쓰기는 실패를 알릴 방법이 없고, 처리하지 않은 필드는
 * 에뮬레이션 층이 자기 버퍼에 그대로 담는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 코어의 config 쓰기).
 *
 * 호출 체인:
 *   PCI 코어 -> advk_pcie_wr_conf() -> pci_bridge_emul_conf_write()
 *     -> ops->write_base -> [이 함수] -> advk_writel()
 */
advk_pci_bridge_emul_base_conf_write(struct pci_bridge_emul *bridge,
				     int reg, u32 old, u32 new, u32 mask)
{
	struct advk_pcie *pcie = bridge->data;	/* [한국어] 에뮬레이션 브리지의 data 에서 이 드라이버 인스턴스를 꺼낸다 */

	switch (reg) {	/* [한국어] 오프셋별로 갈린다 */
	case PCI_COMMAND:	/* [한국어] 명령·상태 레지스터이면 */
		advk_writel(pcie, new, PCIE_CORE_CMD_STATUS_REG);	/* [한국어] 새 값을 하드웨어에 그대로 쓴다 */
		break;	/* [한국어] 갈래를 벗어난다 */

	case PCI_INTERRUPT_LINE:	/* [한국어] 인터럽트 라인 워드이면 상위 절반의 두 비트를 옮긴다 */
		/*
		 * According to Figure 6-3: Pseudo Logic Diagram for Error
		 * Message Controls in PCIe base specification, SERR# Enable bit
		 * in Bridge Control register enable receiving of ERR_* messages
		 */
		if (mask & (PCI_BRIDGE_CTL_SERR << 16)) {	/* [한국어] **SERR 비트가 이번 쓰기의 대상이면** */
			u32 val = advk_readl(pcie, PCIE_ISR0_MASK_REG);	/* [한국어] ISR0 마스크를 읽어 */
			if (new & (PCI_BRIDGE_CTL_SERR << 16))	/* [한국어] SERR 를 켜라는 것이면 */
				val &= ~PCIE_ISR0_ERR_MASK;	/* [한국어] 오류 마스크를 지우고 — 마스크를 푸는 것이 SERR 를 켜는 것이다. 바로 위 상류 주석이 규격의 Figure 6-3 을 근거로 든다 */
			else	/* [한국어] 끄라는 것이면 */
				val |= PCIE_ISR0_ERR_MASK;	/* [한국어] 오류 마스크를 건다 */
			advk_writel(pcie, val, PCIE_ISR0_MASK_REG);	/* [한국어] 고친 값을 되쓴다 */
		}
		if (mask & (PCI_BRIDGE_CTL_BUS_RESET << 16)) {	/* [한국어] **BUS_RESET 비트가 이번 쓰기의 대상이면** */
			u32 val = advk_readl(pcie, PCIE_CORE_CTRL1_REG);	/* [한국어] CTRL1 을 읽어 */
			if (new & (PCI_BRIDGE_CTL_BUS_RESET << 16))	/* [한국어] 리셋을 걸라는 것이면 */
				val |= HOT_RESET_GEN;	/* [한국어] 핫 리셋 비트를 세우고 — **실제로 핫 리셋이 발생한다** */
			else	/* [한국어] 풀라는 것이면 */
				val &= ~HOT_RESET_GEN;	/* [한국어] 그 비트를 지운다 */
			advk_writel(pcie, val, PCIE_CORE_CTRL1_REG);	/* [한국어] 고친 값을 되쓴다 */
		}
		break;	/* [한국어] 갈래를 벗어난다 */

	default:	/* [한국어] 그 밖의 오프셋은 */
		break;	/* [한국어] 에뮬레이션 층의 버퍼에만 남는다 */
	}
}

static pci_bridge_emul_read_status_t
/* [한국어]
 * advk_pci_bridge_emul_pcie_conf_read - 에뮬레이션 브리지의 PCI Express capability 읽기
 *
 * @bridge: 에뮬레이션 브리지 구조체.
 * @reg:    PCI Express capability 안에서의 오프셋.
 * @value:  읽은 값을 담을 자리.
 * @return: 처리했으면 HANDLED, 아니면 NOT_HANDLED.
 *
 * 가로채는 필드가 셋으로 나뉜다.
 *   PCI_EXP_LNKCAP — 하드웨어 값을 읽되 **DLLLARC(데이터 링크 계층 링크
 *     활성 보고 가능) 비트를 소프트웨어로 얹는다.**
 *   PCI_EXP_LNKCTL — 이 32비트 워드의 상위 절반이 Link Status 다.
 *     하드웨어의 LT(링크 훈련 중) 비트를 지우고 **LTSSM 을 직접 보고
 *     다시 만든다** — advk_pcie_link_training() 이 참이면 LT 를,
 *     advk_pcie_link_active() 가 참이면 DLLLA 를 세운다. 하드웨어의
 *     그 비트들을 그대로 믿지 않는다는 뜻이다.
 *   DEVCAP/DEVCTL/DEVCAP2/DEVCTL2/LNKCAP2/LNKCTL2 — 하드웨어 값을
 *     그대로 돌려준다.
 *
 * 이 컨트롤러의 PCI Express capability 는 내부 레지스터의
 * PCIE_CORE_PCIEXP_CAP 기준으로 놓여 있어, 오프셋에 그 기준을 더해 읽는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PCI 코어 -> advk_pcie_rd_conf() -> pci_bridge_emul_conf_read()
 *     -> ops->read_pcie -> [이 함수]
 *     -> advk_readl() / advk_pcie_link_active()
 */
advk_pci_bridge_emul_pcie_conf_read(struct pci_bridge_emul *bridge,
				    int reg, u32 *value)
{
	struct advk_pcie *pcie = bridge->data;	/* [한국어] 에뮬레이션 브리지의 data 에서 이 드라이버 인스턴스를 꺼낸다 */


	switch (reg) {	/* [한국어] **PCI Express capability 안의 오프셋별로 갈린다** */
	/*
	 * PCI_EXP_SLTCAP, PCI_EXP_SLTCTL, PCI_EXP_RTCTL and PCI_EXP_RTSTA are
	 * also supported, but do not need to be handled here, because their
	 * values are stored in emulated config space buffer, and we read them
	 * from there when needed.
	 */

	case PCI_EXP_LNKCAP: {	/* [한국어] 링크 능력 레지스터이면 */
		u32 val = advk_readl(pcie, PCIE_CORE_PCIEXP_CAP + reg);	/* [한국어] 하드웨어 값을 읽되 — 이 컨트롤러의 capability 는 PCIE_CORE_PCIEXP_CAP 기준에 놓여 있다 */
		/*
		 * PCI_EXP_LNKCAP_DLLLARC bit is hardwired in aardvark HW to 0.
		 * But support for PCI_EXP_LNKSTA_DLLLA is emulated via ltssm
		 * state so explicitly enable PCI_EXP_LNKCAP_DLLLARC flag.
		 */
		val |= PCI_EXP_LNKCAP_DLLLARC;	/* [한국어] **DLLLARC(데이터 링크 계층 링크 활성 보고 가능) 비트를 소프트웨어로 얹는다** — 아래 LNKCTL 갈래가 그 상태를 실제로 보고하기 때문이다 */
		*value = val;	/* [한국어] 만든 값을 돌려주고 */
		return PCI_BRIDGE_EMUL_HANDLED;	/* [한국어] 처리했다고 알린다 */
	}

	case PCI_EXP_LNKCTL: {	/* [한국어] 링크 제어 워드이면 — **상위 절반이 Link Status 다** */
		/* u32 contains both PCI_EXP_LNKCTL and PCI_EXP_LNKSTA */
		u32 val = advk_readl(pcie, PCIE_CORE_PCIEXP_CAP + reg) &	/* [한국어] 하드웨어 값을 읽되 */
			~(PCI_EXP_LNKSTA_LT << 16);	/* [한국어] **하드웨어의 훈련 중 비트는 지운다** — 그 값을 믿지 않고 아래에서 다시 만든다 */
		if (advk_pcie_link_training(pcie))	/* [한국어] LTSSM 을 직접 보아 훈련 중이면 */
			val |= (PCI_EXP_LNKSTA_LT << 16);	/* [한국어] 훈련 중 비트를 세우고 */
		if (advk_pcie_link_active(pcie))	/* [한국어] 데이터 링크가 활성이면 */
			val |= (PCI_EXP_LNKSTA_DLLLA << 16);	/* [한국어] DLLLA 비트를 세운다 */
		*value = val;	/* [한국어] 만든 값을 돌려주고 */
		return PCI_BRIDGE_EMUL_HANDLED;	/* [한국어] 처리했다고 알린다 */
	}

	case PCI_EXP_DEVCAP:	/* [한국어] 장치 능력 */
	case PCI_EXP_DEVCTL:	/* [한국어] 장치 제어 */
	case PCI_EXP_DEVCAP2:	/* [한국어] 장치 능력 2 */
	case PCI_EXP_DEVCTL2:	/* [한국어] 장치 제어 2 */
	case PCI_EXP_LNKCAP2:	/* [한국어] 링크 능력 2 */
	case PCI_EXP_LNKCTL2:	/* [한국어] 링크 제어 2 — 이 여섯은 */
		*value = advk_readl(pcie, PCIE_CORE_PCIEXP_CAP + reg);	/* [한국어] 하드웨어 값을 그대로 돌려주고 */
		return PCI_BRIDGE_EMUL_HANDLED;	/* [한국어] 처리했다고 알린다 */

	default:	/* [한국어] 그 밖의 오프셋은 */
		return PCI_BRIDGE_EMUL_NOT_HANDLED;	/* [한국어] 에뮬레이션 층이 자기 버퍼로 처리하게 넘긴다 */
	}

}

static void
/* [한국어]
 * advk_pci_bridge_emul_pcie_conf_write - 에뮬레이션 브리지의 PCI Express capability 쓰기
 *
 * @bridge: 에뮬레이션 브리지 구조체.
 * @reg:    PCI Express capability 안에서의 오프셋.
 * @old:    이전 값.
 * @new:    쓸 값.
 * @mask:   이번 쓰기에서 유효한 비트.
 *
 * 다루는 필드가 셋이다.
 *   PCI_EXP_LNKCTL — 새 값을 하드웨어에 쓴 뒤, **Retrain Link 비트가
 *     서 있으면 재훈련이 실제로 시작되기를 기다린다.**
 *   PCI_EXP_RTCTL — 하드웨어에 쓰지 않고 **에뮬레이션 버퍼만 손본다.**
 *     그 자리의 상류 주석대로 PMEIE 와 RRS_SVE 두 비트만 흉내 내므로,
 *     나머지 비트를 마스크로 지워 버린다. 그렇게 남은 RRS_SVE 값을
 *     나중에 advk_pcie_rd_conf() 가 allow_rrs 계산에 읽는다.
 *   DEVCTL/DEVCTL2/LNKCTL2 — 새 값을 하드웨어에 그대로 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 재훈련 대기에서 최대 20ms 를 쓴다.
 *
 * 호출 체인:
 *   PCI 코어 -> advk_pcie_wr_conf() -> pci_bridge_emul_conf_write()
 *     -> ops->write_pcie -> [이 함수] -> advk_pcie_wait_for_retrain()
 */
advk_pci_bridge_emul_pcie_conf_write(struct pci_bridge_emul *bridge,
				     int reg, u32 old, u32 new, u32 mask)
{
	struct advk_pcie *pcie = bridge->data;	/* [한국어] 에뮬레이션 브리지의 data 에서 이 드라이버 인스턴스를 꺼낸다 */

	switch (reg) {	/* [한국어] 오프셋별로 갈린다 */
	case PCI_EXP_LNKCTL:	/* [한국어] 링크 제어이면 */
		advk_writel(pcie, new, PCIE_CORE_PCIEXP_CAP + reg);	/* [한국어] 새 값을 하드웨어에 쓰고 */
		if (new & PCI_EXP_LNKCTL_RL)	/* [한국어] **Retrain Link 비트가 서 있으면** */
			advk_pcie_wait_for_retrain(pcie);	/* [한국어] 재훈련이 실제로 시작되기를 기다린다 */
		break;	/* [한국어] 갈래를 벗어난다 */

	case PCI_EXP_RTCTL: {	/* [한국어] 루트 제어이면 — **하드웨어에 쓰지 않고 에뮬레이션 버퍼만 손본다** */
		u16 rootctl = le16_to_cpu(bridge->pcie_conf.rootctl);	/* [한국어] 현재 버퍼 값을 가져와 */
		/* Only emulation of PMEIE and RRS_SVE bits is provided */
		rootctl &= PCI_EXP_RTCTL_PMEIE | PCI_EXP_RTCTL_RRS_SVE;	/* [한국어] 바로 위 상류 주석대로 PMEIE 와 RRS_SVE 두 비트만 흉내 내므로 나머지를 지운다 */
		bridge->pcie_conf.rootctl = cpu_to_le16(rootctl);	/* [한국어] 다시 버퍼에 담는다. **이 RRS_SVE 값을 나중에 rd_conf 가 allow_rrs 계산에 읽는다** */
		break;	/* [한국어] 갈래를 벗어난다 */
	}

	/*
	 * PCI_EXP_RTSTA is also supported, but does not need to be handled
	 * here, because its value is stored in emulated config space buffer,
	 * and we write it there when needed.
	 */

	case PCI_EXP_DEVCTL:	/* [한국어] 장치 제어 */
	case PCI_EXP_DEVCTL2:	/* [한국어] 장치 제어 2 */
	case PCI_EXP_LNKCTL2:	/* [한국어] 링크 제어 2 — 이 셋은 */
		advk_writel(pcie, new, PCIE_CORE_PCIEXP_CAP + reg);	/* [한국어] 새 값을 하드웨어에 그대로 쓴다 */
		break;	/* [한국어] 갈래를 벗어난다 */

	default:	/* [한국어] 그 밖의 오프셋은 */
		break;	/* [한국어] 에뮬레이션 층의 버퍼에만 남는다 */
	}
}

static pci_bridge_emul_read_status_t
/* [한국어]
 * advk_pci_bridge_emul_ext_conf_read - 에뮬레이션 브리지의 확장 config 공간 읽기
 *
 * @bridge: 에뮬레이션 브리지 구조체.
 * @reg:    확장 config 공간(0x100 이후)에서의 오프셋.
 * @value:  읽은 값을 담을 자리.
 * @return: 처리했으면 HANDLED, 아니면 NOT_HANDLED.
 *
 * 다루는 것은 **AER(Advanced Error Reporting) capability 뿐**이며,
 * 이 컨트롤러의 AER 레지스터는 내부의 PCIE_CORE_PCIERR_CAP 기준으로
 * 놓여 있다. 오류 상태·마스크·심각도, 헤더 로그 넷, 루트 명령·상태,
 * 오류 소스 같은 필드를 그 기준에 오프셋을 더해 읽는다.
 *
 * 기본 헤더나 PCI Express capability 와 콜백이 따로인 이유는 세 영역의
 * 오프셋 기준이 서로 다르기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PCI 코어 -> advk_pcie_rd_conf() -> pci_bridge_emul_conf_read()
 *     -> ops->read_ext -> [이 함수] -> advk_readl()
 */
advk_pci_bridge_emul_ext_conf_read(struct pci_bridge_emul *bridge,
				   int reg, u32 *value)
{
	struct advk_pcie *pcie = bridge->data;	/* [한국어] 에뮬레이션 브리지의 data 에서 이 드라이버 인스턴스를 꺼낸다 */

	switch (reg) {	/* [한국어] **확장 config 공간 안의 오프셋별로 갈린다** */
	case 0:	/* [한국어] 오프셋 0 은 확장 capability 헤더 자체다 */
		*value = advk_readl(pcie, PCIE_CORE_PCIERR_CAP + reg);	/* [한국어] 하드웨어 값을 읽되 */

		/*
		 * PCI_EXT_CAP_NEXT bits are set to offset 0x150, but Armada
		 * 3700 Functional Specification does not document registers
		 * at those addresses.
		 *
		 * Thus we clear PCI_EXT_CAP_NEXT bits to make Advanced Error
		 * Reporting Capability header the last Extended Capability.
		 * If we obtain documentation for those registers in the
		 * future, this can be changed.
		 */
		*value &= 0x000fffff;	/* [한국어] **다음 capability 포인터를 지운다.** 바로 위 상류 주석이 이유를 밝힌다 — 그 포인터가 0x150 을 가리키지만 Armada 3700 기능 명세에 그 주소의 레지스터가 문서화되어 있지 않으므로, AER 을 마지막 확장 capability 로 만든다. 하위 20비트만 남기면 상위의 다음 포인터가 0 이 된다 */
		return PCI_BRIDGE_EMUL_HANDLED;	/* [한국어] 처리했다고 알린다 */

	case PCI_ERR_UNCOR_STATUS:	/* [한국어] 정정 불가 오류 상태 */
	case PCI_ERR_UNCOR_MASK:	/* [한국어] 그 마스크 */
	case PCI_ERR_UNCOR_SEVER:	/* [한국어] 그 심각도 */
	case PCI_ERR_COR_STATUS:	/* [한국어] 정정 가능 오류 상태 */
	case PCI_ERR_COR_MASK:	/* [한국어] 그 마스크 */
	case PCI_ERR_CAP:	/* [한국어] AER 능력·제어 */
	case PCI_ERR_HEADER_LOG + 0:	/* [한국어] 헤더 로그 첫 워드 */
	case PCI_ERR_HEADER_LOG + 4:	/* [한국어] 둘째 워드 */
	case PCI_ERR_HEADER_LOG + 8:	/* [한국어] 셋째 워드 */
	case PCI_ERR_HEADER_LOG + 12:	/* [한국어] 넷째 워드 */
	case PCI_ERR_ROOT_COMMAND:	/* [한국어] 루트 오류 명령 */
	case PCI_ERR_ROOT_STATUS:	/* [한국어] 루트 오류 상태 */
	case PCI_ERR_ROOT_ERR_SRC:	/* [한국어] 오류 소스 식별 — 이 열셋은 */
		*value = advk_readl(pcie, PCIE_CORE_PCIERR_CAP + reg);	/* [한국어] 하드웨어 값을 그대로 돌려주고 */
		return PCI_BRIDGE_EMUL_HANDLED;	/* [한국어] 처리했다고 알린다 */

	default:	/* [한국어] 그 밖의 오프셋은 */
		return PCI_BRIDGE_EMUL_NOT_HANDLED;	/* [한국어] 에뮬레이션 층이 자기 버퍼로 처리하게 넘긴다 */
	}
}

static void
/* [한국어]
 * advk_pci_bridge_emul_ext_conf_write - 에뮬레이션 브리지의 확장 config 공간 쓰기
 *
 * @bridge: 에뮬레이션 브리지 구조체.
 * @reg:    확장 config 공간에서의 오프셋.
 * @old:    이전 값.
 * @new:    쓸 값.
 * @mask:   이번 쓰기에서 유효한 비트.
 *
 * 읽기 쪽의 짝이며, **W1C(1 을 써서 지우는) 레지스터를 따로 다룬다.**
 * 그 자리의 상류 주석이 그렇게 밝힌다 — UNCOR_STATUS, COR_STATUS,
 * ROOT_STATUS 세 가지는 새 값을 mask 로 한 번 걸러서 쓴다. 이번 쓰기에서
 * 유효하지 않은 비트까지 1 로 나가면 사용자가 지우려 하지 않은 오류
 * 기록까지 함께 지워지기 때문이다. 나머지 필드는 fallthrough 로 이어져
 * 같은 쓰기 문장을 공유한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PCI 코어 -> advk_pcie_wr_conf() -> pci_bridge_emul_conf_write()
 *     -> ops->write_ext -> [이 함수] -> advk_writel()
 */
advk_pci_bridge_emul_ext_conf_write(struct pci_bridge_emul *bridge,
				    int reg, u32 old, u32 new, u32 mask)
{
	struct advk_pcie *pcie = bridge->data;	/* [한국어] 에뮬레이션 브리지의 data 에서 이 드라이버 인스턴스를 꺼낸다 */

	switch (reg) {	/* [한국어] 오프셋별로 갈린다 */
	/* These are W1C registers, so clear other bits */
	case PCI_ERR_UNCOR_STATUS:	/* [한국어] 정정 불가 오류 상태 */
	case PCI_ERR_COR_STATUS:	/* [한국어] 정정 가능 오류 상태 */
	case PCI_ERR_ROOT_STATUS:	/* [한국어] 루트 오류 상태 — **바로 위 상류 주석대로 이 셋은 W1C 레지스터다** */
		new &= mask;	/* [한국어] **이번 쓰기의 유효 비트만 남긴다.** 그러지 않으면 사용자가 지우려 하지 않은 오류 기록까지 1 이 나가 함께 지워진다 */
		fallthrough;	/* [한국어] 아래 쓰기 문장을 공유하도록 이어진다 */

	case PCI_ERR_UNCOR_MASK:	/* [한국어] 정정 불가 오류 마스크 */
	case PCI_ERR_UNCOR_SEVER:	/* [한국어] 그 심각도 */
	case PCI_ERR_COR_MASK:	/* [한국어] 정정 가능 오류 마스크 */
	case PCI_ERR_CAP:	/* [한국어] AER 능력·제어 */
	case PCI_ERR_HEADER_LOG + 0:	/* [한국어] 헤더 로그 첫 워드 */
	case PCI_ERR_HEADER_LOG + 4:	/* [한국어] 둘째 워드 */
	case PCI_ERR_HEADER_LOG + 8:	/* [한국어] 셋째 워드 */
	case PCI_ERR_HEADER_LOG + 12:	/* [한국어] 넷째 워드 */
	case PCI_ERR_ROOT_COMMAND:	/* [한국어] 루트 오류 명령 */
	case PCI_ERR_ROOT_ERR_SRC:	/* [한국어] 오류 소스 식별 — 이들과 위 W1C 셋이 */
		advk_writel(pcie, new, PCIE_CORE_PCIERR_CAP + reg);	/* [한국어] 같은 문장으로 하드웨어에 쓰인다 */
		break;	/* [한국어] 갈래를 벗어난다 */

	default:	/* [한국어] 그 밖의 오프셋은 */
		break;	/* [한국어] 에뮬레이션 층의 버퍼에만 남는다 */
	}
}

static const struct pci_bridge_emul_ops advk_pci_bridge_emul_ops = {	/* [한국어] **에뮬레이션 층이 부를 콜백 여섯 개.** 세 영역의 오프셋 기준이 서로 달라 읽기·쓰기가 각각 셋으로 나뉜다 */
	.read_base = advk_pci_bridge_emul_base_conf_read,	/* [한국어] 기본 config 헤더 읽기 */
	.write_base = advk_pci_bridge_emul_base_conf_write,	/* [한국어] 그 쓰기 */
	.read_pcie = advk_pci_bridge_emul_pcie_conf_read,	/* [한국어] PCI Express capability 읽기 */
	.write_pcie = advk_pci_bridge_emul_pcie_conf_write,	/* [한국어] 그 쓰기 */
	.read_ext = advk_pci_bridge_emul_ext_conf_read,	/* [한국어] 확장 config 공간 읽기 */
	.write_ext = advk_pci_bridge_emul_ext_conf_write,	/* [한국어] 그 쓰기 */
};

/*
 * Initialize the configuration space of the PCI-to-PCI bridge
 * associated with the given PCIe interface.
 */
/* [한국어]
 * advk_sw_pci_bridge_init - 소프트웨어 루트 포트 브리지를 세운다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공, pci_bridge_emul_init() 의 오류 코드면 실패.
 *
 * **이 드라이버가 PCI 코어에 루트 포트로 보이게 만드는 자리다.**
 * setup_hw 의 상류 주석이 왜 필요한지 밝힌다 — 이 하드웨어에는 규격에
 * 맞는 Type 1 config 공간이 없고, Aardvark 의 config 접근 방식으로는
 * 그것을 읽을 수조차 없으며, 내부 레지스터의 0x10~0x34 범위는 전혀 다른
 * 레지스터다.
 *
 * 하는 일은 브리지 구조체의 초기값을 채우고 콜백 묶음을 매단 뒤
 * pci_bridge_emul_init() 을 부르는 것이다. 벤더·장치 ID 와 클래스는
 * 실제 하드웨어 레지스터에서 읽어 채우고, 그 밖의 필드는 코어가 기대하는
 * 값으로 둔다.
 *
 * 이 뒤로 PCI 코어가 0번 버스의 장치를 읽으면 advk_pcie_rd_conf() 가
 * pci_bridge_emul_conf_read() 로 넘기고, 그것이 다시 위의 콜백 여섯 개를
 * 거쳐 답한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   advk_pcie_probe() -> [이 함수] -> advk_readl()
 *     -> pci_bridge_emul_init()
 */
static int advk_sw_pci_bridge_init(struct advk_pcie *pcie)
{
	struct pci_bridge_emul *bridge = &pcie->bridge;	/* [한국어] 인스턴스가 품고 있는 에뮬레이션 브리지 구조체 */

	bridge->conf.vendor =	/* [한국어] **벤더 ID 는 실제 하드웨어에서 읽는다** */
		cpu_to_le16(advk_readl(pcie, PCIE_CORE_DEV_ID_REG) & 0xffff);	/* [한국어] DEV_ID 레지스터의 하위 16비트다. setup_hw 가 erratum 4.1 우회로 이미 바로잡아 둔 값이다 */
	bridge->conf.device =	/* [한국어] 장치 ID 도 마찬가지로 */
		cpu_to_le16(advk_readl(pcie, PCIE_CORE_DEV_ID_REG) >> 16);	/* [한국어] 같은 레지스터의 상위 16비트에서 읽는다 */
	bridge->conf.class_revision =	/* [한국어] 클래스와 개정도 */
		cpu_to_le32(advk_readl(pcie, PCIE_CORE_DEV_REV_REG) & 0xff);	/* [한국어] 하드웨어에서 읽는다. **하위 8비트만 남기는 이유**는 클래스 코드 자리를 비워 두어 에뮬레이션 층이 브리지 클래스를 채우게 하려는 것이다 */

	/* Support 32 bits I/O addressing */
	bridge->conf.iobase = PCI_IO_RANGE_TYPE_32;	/* [한국어] **I/O 창이 32비트 주소를 쓴다고 알린다.** 이 필드는 하드웨어에 없어 소프트웨어 값이다 */
	bridge->conf.iolimit = PCI_IO_RANGE_TYPE_32;	/* [한국어] 상한 쪽도 같은 종류로 둔다 */

	/* Support 64 bits memory pref */
	bridge->conf.pref_mem_base = cpu_to_le16(PCI_PREF_RANGE_TYPE_64);	/* [한국어] **프리페치 메모리 창이 64비트 주소를 쓴다고 알린다** */
	bridge->conf.pref_mem_limit = cpu_to_le16(PCI_PREF_RANGE_TYPE_64);	/* [한국어] 상한 쪽도 같은 종류로 둔다 */

	/* Support interrupt A for MSI feature */
	bridge->conf.intpin = PCI_INTERRUPT_INTA;	/* [한국어] **루트 포트가 INTA 를 쓴다고 알린다.** advk_pcie_map_irq() 가 이 핀 번호로 rp_irq_domain 을 매핑한다 */

	/*
	 * Aardvark HW provides PCIe Capability structure in version 2 and
	 * indicate slot support, which is emulated.
	 */
	bridge->pcie_conf.cap = cpu_to_le16(2 | PCI_EXP_FLAGS_SLOT);	/* [한국어] **PCI Express capability 의 버전을 2 로 두고 슬롯이 있다고 표시한다.** 슬롯 표시가 있어야 아래 슬롯 능력·상태가 의미를 갖는다 */

	/*
	 * Set Presence Detect State bit permanently since there is no support
	 * for unplugging the card nor detecting whether it is plugged. (If a
	 * platform exists in the future that supports it, via a GPIO for
	 * example, it should be implemented via this bit.)
	 *
	 * Set physical slot number to 1 since there is only one port and zero
	 * value is reserved for ports within the same silicon as Root Port
	 * which is not our case.
	 */
	bridge->pcie_conf.slotcap = cpu_to_le32(FIELD_PREP(PCI_EXP_SLTCAP_PSN,	/* [한국어] **물리 슬롯 번호를 1 로 둔다** */
							   1));	/* [한국어] 그 값 */
	bridge->pcie_conf.slotsta = cpu_to_le16(PCI_EXP_SLTSTA_PDS);	/* [한국어] **카드가 꽂혀 있다고 표시한다**(Presence Detect State). 이 컨트롤러에는 실제 존재 감지 신호가 없어 늘 있는 것으로 둔다 */

	/* Indicates supports for Completion Retry Status */
	bridge->pcie_conf.rootcap = cpu_to_le16(PCI_EXP_RTCAP_RRS_SV);	/* [한국어] **RRS Software Visibility 를 지원한다고 알린다.** 이것이 있어야 PCI 코어가 Root Control 의 RRS_SVE 를 켤 수 있고, 그래야 rd_conf 의 allow_rrs 가 참이 될 수 있다 */

	bridge->subsystem_vendor_id = advk_readl(pcie, PCIE_CORE_SSDEV_ID_REG) & 0xffff;	/* [한국어] 서브시스템 벤더 ID 를 하드웨어에서 읽고 */
	bridge->subsystem_id = advk_readl(pcie, PCIE_CORE_SSDEV_ID_REG) >> 16;	/* [한국어] 서브시스템 장치 ID 도 같은 레지스터의 상위 절반에서 읽는다 */
	bridge->has_pcie = true;	/* [한국어] **PCI Express capability 를 가진 브리지라고 알린다** */
	bridge->pcie_start = PCIE_CORE_PCIEXP_CAP;	/* [한국어] 그 capability 가 내부 레지스터의 어느 오프셋에 있는지 알린다 */
	bridge->data = pcie;	/* [한국어] 콜백에서 이 드라이버 인스턴스를 되찾을 수 있게 담아 둔다 */
	bridge->ops = &advk_pci_bridge_emul_ops;	/* [한국어] 읽기·쓰기 콜백 여섯 개를 매단다 */

	return pci_bridge_emul_init(bridge, 0);	/* [한국어] **에뮬레이션 층을 초기화한다.** 두 번째 인자 0 은 추가 플래그가 없다는 뜻이다 */
}

/* [한국어]
 * advk_pcie_valid_device - 이 (버스, devfn) 조합에 접근해도 되는지 가른다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @bus:  대상 버스.
 * @devfn: 대상 장치·함수 번호.
 * @return: true 면 접근해도 된다.
 *
 * PCI 코어는 열거할 때 있을 수 있는 모든 (버스, 장치, 함수) 조합을
 * 읽어 보는데, 이 컨트롤러의 구조상 애초에 존재할 수 없는 조합이 있다.
 * 그런 조합에 PIO 트랜잭션을 내면 시간만 낭비하고 오류 로그가 쌓이므로
 * 미리 거른다.
 *
 * 거르는 기준이 둘이다.
 *   - **루트 버스에서는 슬롯 0 만 유효하다.** 그 자리에 있는 것이
 *     에뮬레이션된 루트 포트 자신뿐이기 때문이다.
 *   - **루트 버스가 아니면 링크가 서 있어야 한다.** 그 자리의 긴 상류
 *     주석이 이유를 밝힌다 — 링크가 내려간 상태에서 PIO 요청을 내면
 *     **컨트롤러 전체가 동작 불능 상태로 굳어**, 링크가 다시 올라와도
 *     PIO 가 더는 동작하지 않고 컨트롤러를 통째로 리셋해야 한다.
 *     그래서 링크가 내려가 있는 동안에는 PIO 를 아예 내지 않는다.
 *     같은 주석이 검사와 실제 요청 사이에 링크가 내려가는 경우는
 *     여전히 문제로 남는다고 적어 두었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 *
 * 호출 체인:
 *   advk_pcie_rd_conf() / advk_pcie_wr_conf() -> [이 함수]
 */
static bool advk_pcie_valid_device(struct advk_pcie *pcie, struct pci_bus *bus,
				  int devfn)
{
	if (pci_is_root_bus(bus) && PCI_SLOT(devfn) != 0)	/* [한국어] **루트 버스에서는 슬롯 0 만 유효하다** — 그 자리에 있는 것이 에뮬레이션된 루트 포트 자신뿐이기 때문이다 */
		return false;	/* [한국어] 그 밖의 슬롯은 거절한다 */

	/*
	 * If the link goes down after we check for link-up, we have a problem:
	 * if a PIO request is executed while link-down, the whole controller
	 * gets stuck in a non-functional state, and even after link comes up
	 * again, PIO requests won't work anymore, and a reset of the whole PCIe
	 * controller is needed. Therefore we need to prevent sending PIO
	 * requests while the link is down.
	 */
	if (!pci_is_root_bus(bus) && !advk_pcie_link_up(pcie))	/* [한국어] **루트 버스가 아니면 링크가 서 있어야 한다.** 바로 위 상류 주석대로 링크가 내려간 상태에서 PIO 를 내면 컨트롤러 전체가 굳어 통째로 리셋해야 한다 */
		return false;	/* [한국어] 링크가 없으면 거절한다 */

	return true;	/* [한국어] 두 관문을 지났으면 접근해도 된다 */
}

/* [한국어]
 * advk_pcie_pio_is_running - 앞선 PIO 트랜잭션이 아직 도는 중인지 본다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: true 면 아직 도는 중이라 새 요청을 내면 안 된다.
 *
 * **PIO 는 한 번에 하나만 처리된다.** 그 자리의 긴 상류 주석이 어기면
 * 어떻게 되는지를 밝힌다 — 앞선 전송이 끝나기 전에 새 PIO 를 시작하면
 * CPU 에 External Abort 가 나고 커널 패닉으로 이어진다. 주석에 그때
 * 찍히는 메시지(SError Interrupt, Asynchronous SError Interrupt)까지
 * 적혀 있다. 일부 ARM Trusted Firmware 판은 그 어보트를 EL3 에서 받아
 * 마스크해 패닉을 막는데, 주석이 해당 TF-A 커밋 주소를 남겨 두었다.
 *
 * 같은 주석이 **왜 이 검사가 필요한지**도 밝힌다. rd_conf 와 wr_conf 는
 * PCI 코어의 pci_lock_config() 수준에서 raw_spin_lock_irqsave() 로
 * 보호되어 동시에 불리지 않는다. 그런데도 검사가 필요한 이유는,
 * 링크가 내려갔거나 카드가 빠졌을 때 PIO 하나가 1.5초까지 걸릴 수 있어
 * **advk_pcie_wait_pio() 가 완료를 기다리지 못하고 시간 초과로 돌아오는
 * 경우가 있기 때문**이다. 그렇게 남겨진 전송을 다음 호출이 만난다.
 *
 * 판별 근거는 PIO_START 가 아직 서 있는 것이다.
 *
 * 이 함수가 참을 돌려주면 읽기 경로는 RRS 를 돌려줄 수 있으면 그렇게
 * 하고(try_rrs), 쓰기 경로는 곧바로 실패한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 접근).
 *
 * 호출 체인:
 *   advk_pcie_rd_conf() / advk_pcie_wr_conf() -> [이 함수] -> advk_readl()
 */
static bool advk_pcie_pio_is_running(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;	/* [한국어] 경고를 낼 장치 */

	/*
	 * Trying to start a new PIO transfer when previous has not completed
	 * cause External Abort on CPU which results in kernel panic:
	 *
	 *     SError Interrupt on CPU0, code 0xbf000002 -- SError
	 *     Kernel panic - not syncing: Asynchronous SError Interrupt
	 *
	 * Functions advk_pcie_rd_conf() and advk_pcie_wr_conf() are protected
	 * by raw_spin_lock_irqsave() at pci_lock_config() level to prevent
	 * concurrent calls at the same time. But because PIO transfer may take
	 * about 1.5s when link is down or card is disconnected, it means that
	 * advk_pcie_wait_pio() does not always have to wait for completion.
	 *
	 * Some versions of ARM Trusted Firmware handles this External Abort at
	 * EL3 level and mask it to prevent kernel panic. Relevant TF-A commit:
	 * https://git.trustedfirmware.org/TF-A/trusted-firmware-a.git/commit/?id=3c7dcdac5c50
	 */
	if (advk_readl(pcie, PIO_START)) {	/* [한국어] **PIO_START 가 아직 서 있으면 앞선 전송이 도는 중이다** */
		dev_err(dev, "Previous PIO read/write transfer is still running\n");	/* [한국어] 그 사실을 알리고 */
		return true;	/* [한국어] 새 요청을 내면 안 된다고 답한다 */
	}

	return false;	/* [한국어] 비어 있으면 새 요청을 내도 된다 */
}

/* [한국어]
 * advk_pcie_rd_conf - PIO 시퀀스로 config 공간을 읽는다
 *
 * @bus:   대상 버스. sysdata 에 이 드라이버 인스턴스가 들어 있다.
 * @devfn: 대상 장치·함수 번호.
 * @where: 읽을 오프셋.
 * @size:  1, 2, 4 중 하나.
 * @val:   읽은 값을 담을 자리.
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_DEVICE_NOT_FOUND / PCIBIOS_SET_FAILED.
 *
 * struct pci_ops 의 read 로 등록되어 **PCI 코어의 모든 config 읽기가
 * 여기로 들어온다.** ECAM 컨트롤러라면 메모리 읽기 한 번으로 끝날 일을,
 * 이 컨트롤러에서는 레지스터 시퀀스로 만들어야 한다.
 *
 * 먼저 두 가지를 가른다.
 *   - 존재할 수 없는 (버스, devfn) 조합이면 장치 없음으로 끝낸다.
 *   - **루트 버스이면 하드웨어를 건드리지 않고 에뮬레이션 브리지가
 *     답한다** — 이 컨트롤러에는 루트 포트의 type-1 헤더가 없기 때문이다.
 *
 * 그 다음 allow_rrs 를 계산한다. RRS 를 정상 결과로 볼 수 있는 조건이
 * 셋이며 그 자리의 상류 주석이 밝힌다 — 읽는 오프셋이 Vendor ID 이고,
 * 크기가 2바이트 이상이며(즉 Vendor ID 두 바이트를 모두 포함),
 * 에뮬레이션 브리지의 Root Control 에 RRS Software Visibility 가 켜져
 * 있어야 한다.
 *
 * PIO 시퀀스는 이렇다.
 *   1. 앞선 트랜잭션이 도는 중이면 새 요청을 내지 않고 try_rrs 로 간다.
 *   2. PIO_CTRL 의 종류 필드를 정한다. **부모가 루트 버스이면 TYPE0,
 *      아니면 TYPE1** — 브리지를 거쳐 더 아래로 가는지에 따라 config
 *      트랜잭션 종류가 달라지기 때문이다.
 *   3. PIO_ADDR_LS 에 ECAM 방식으로 계산한 주소를 4바이트 정렬해 쓴다.
 *      주소 계산 방식만 ECAM 과 같고 접근은 PIO 다.
 *   4. 읽기이므로 strobe 에 0xf(네 바이트 모두)를 쓴다.
 *   5. PIO_ISR 을 지우고 PIO_START 를 세워 트랜잭션을 낸다.
 *   6. 완료를 기다리고 상태를 해석한다. RRS 로 -EAGAIN 이 오면 같은
 *      요청을 다시 내되, 누적 시도 횟수가 PIO_RETRY_CNT 를 넘지 않게 한다.
 *   7. **4바이트를 받았으므로 크기에 맞게 잘라 낸다** — 오프셋의 하위
 *      2비트만큼 시프트하고 마스크를 씌운다.
 *
 * 에러 경로가 둘이다. try_rrs 는 트랜잭션을 내지 못했거나 완료를 못
 * 기다린 경우로, 상류 주석대로 가능하면 RRS 값을 돌려주어 호출자가 다시
 * 시도하게 한다. fail 은 0xffffffff 를 담고 실패로 끝낸다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. udelay 폴링이라 최악의 경우 1.5초
 * 동안 CPU 를 점유한다.
 *
 * 호출 체인:
 *   PCI 코어 -> pci_ops.read -> [이 함수] -> advk_pcie_valid_device()
 *     -> pci_bridge_emul_conf_read() 또는
 *        advk_pcie_pio_is_running() -> advk_pcie_wait_pio()
 *        -> advk_pcie_check_pio_status()
 */
static int advk_pcie_rd_conf(struct pci_bus *bus, u32 devfn,
			     int where, int size, u32 *val)
{
	struct advk_pcie *pcie = bus->sysdata;	/* [한국어] 버스의 sysdata 에 이 드라이버 인스턴스가 들어 있다. probe 가 담아 둔 값이다 */
	int retry_count;	/* [한국어] PIO 재시도 누적 횟수 */
	bool allow_rrs;	/* [한국어] RRS 를 정상 결과로 볼 수 있는 상황인지 */
	u32 reg;	/* [한국어] PIO 레지스터에 쓸 값 */
	int ret;	/* [한국어] 하위 호출 결과 */

	if (!advk_pcie_valid_device(pcie, bus, devfn))	/* [한국어] **존재할 수 없는 조합이면** 애초에 PIO 를 내지 않는다 */
		return PCIBIOS_DEVICE_NOT_FOUND;	/* [한국어] 장치 없음으로 끝낸다 */

	if (pci_is_root_bus(bus))	/* [한국어] **루트 버스이면 하드웨어를 건드리지 않는다** */
		return pci_bridge_emul_conf_read(&pcie->bridge, where,	/* [한국어] 에뮬레이션 브리지가 답한다 — 이 컨트롤러에는 루트 포트의 type-1 헤더가 없다 */
						 size, val);	/* [한국어] 오프셋·크기·결과 자리를 그대로 넘긴다 */

	/*
	 * Configuration Request Retry Status (RRS) is possible to return
	 * only when reading both bytes from PCI_VENDOR_ID at once and
	 * RRS_SVE flag on Root Port is enabled.
	 */
	allow_rrs = (where == PCI_VENDOR_ID) && (size >= 2) &&	/* [한국어] **RRS 를 정상으로 볼 조건 셋.** 바로 위 상류 주석대로 Vendor ID 두 바이트를 한 번에 읽으면서 루트 포트의 RRS 가시성이 켜져 있어야 한다. 첫째, 오프셋이 Vendor ID 이고 */
		    (le16_to_cpu(pcie->bridge.pcie_conf.rootctl) &	/* [한국어] 둘째, 두 바이트 이상을 읽으며, 셋째로 에뮬레이션 브리지의 Root Control 에 */
		     PCI_EXP_RTCTL_RRS_SVE);	/* [한국어] RRS Software Visibility 가 켜져 있어야 한다 */

	if (advk_pcie_pio_is_running(pcie))	/* [한국어] **앞선 전송이 도는 중이면** 새 요청을 낼 수 없다 */
		goto try_rrs;	/* [한국어] RRS 를 돌려줄 수 있으면 그렇게 하는 경로로 간다 */

	/* Program the control register */
	reg = advk_readl(pcie, PIO_CTRL);	/* [한국어] PIO 제어 레지스터를 읽어 */
	reg &= ~PIO_CTRL_TYPE_MASK;	/* [한국어] 기존 종류 필드를 지우고 */
	if (pci_is_root_bus(bus->parent))	/* [한국어] **부모가 루트 버스이면** 바로 아래 버스의 장치이므로 */
		reg |= PCIE_CONFIG_RD_TYPE0;	/* [한국어] config 읽기 type 0 을, */
	else	/* [한국어] 아니면 브리지를 거쳐 더 아래로 가므로 */
		reg |= PCIE_CONFIG_RD_TYPE1;	/* [한국어] type 1 을 넣는다 */
	advk_writel(pcie, reg, PIO_CTRL);	/* [한국어] 고친 값을 되쓴다 */

	/* Program the address registers */
	reg = ALIGN_DOWN(PCIE_ECAM_OFFSET(bus->number, devfn, where), 4);	/* [한국어] **ECAM 방식으로 주소를 계산해 4바이트로 정렬한다.** 주소 계산 방식만 ECAM 과 같고 접근은 PIO 다 */
	advk_writel(pcie, reg, PIO_ADDR_LS);	/* [한국어] 하위 워드에 쓰고 */
	advk_writel(pcie, 0, PIO_ADDR_MS);	/* [한국어] 상위 워드는 늘 0 이다 */

	/* Program the data strobe */
	advk_writel(pcie, 0xf, PIO_WR_DATA_STRB);	/* [한국어] **읽기이므로 네 바이트 모두 유효하다고 알린다** */

	retry_count = 0;	/* [한국어] 재시도 횟수를 초기화한다 */
	do {	/* [한국어] RRS 재시도를 위해 반복한다 */
		/* Clear PIO DONE ISR and start the transfer */
		advk_writel(pcie, 1, PIO_ISR);	/* [한국어] **완료 표시를 먼저 지우고** */
		advk_writel(pcie, 1, PIO_START);	/* [한국어] 시작 비트를 세워 트랜잭션을 낸다 */

		ret = advk_pcie_wait_pio(pcie);	/* [한국어] 완료를 기다린다 */
		if (ret < 0)	/* [한국어] 못 기다렸으면 */
			goto try_rrs;	/* [한국어] RRS 를 돌려줄 수 있으면 그렇게 하는 경로로 간다 */

		retry_count += ret;	/* [한국어] **몇 번 만에 끝났는지를 누적한다** — RRS 재시도를 포함한 전체 시도가 같은 상한을 넘지 않게 한다 */

		/* Check PIO status and get the read result */
		ret = advk_pcie_check_pio_status(pcie, allow_rrs, val);	/* [한국어] 완료 상태를 해석하고 읽기 결과를 받는다 */
	} while (ret == -EAGAIN && retry_count < PIO_RETRY_CNT);	/* [한국어] **RRS 로 -EAGAIN 이 왔고 상한 안이면 같은 요청을 다시 낸다** */

	if (ret < 0)	/* [한국어] 그 밖의 오류이면 */
		goto fail;	/* [한국어] 실패 경로로 간다 */

	if (size == 1)	/* [한국어] **4바이트를 받았으므로 요청한 크기로 잘라 낸다.** 1바이트이면 */
		*val = (*val >> (8 * (where & 3))) & 0xff;	/* [한국어] 오프셋의 하위 2비트만큼 밀고 한 바이트만 남긴다 */
	else if (size == 2)	/* [한국어] 2바이트이면 */
		*val = (*val >> (8 * (where & 3))) & 0xffff;	/* [한국어] 같은 방식으로 두 바이트만 남긴다. 4바이트이면 그대로 둔다 */

	return PCIBIOS_SUCCESSFUL;	/* [한국어] 성공으로 끝낸다 */

try_rrs:	/* [한국어] **RRS 대체 경로** — 트랜잭션을 내지 못했거나 완료를 못 기다린 경우다 */
	/*
	 * If it is possible, return Configuration Request Retry Status so
	 * that caller tries to issue the request again instead of failing.
	 */
	if (allow_rrs) {	/* [한국어] 바로 위 상류 주석대로 가능하면 RRS 를 돌려주어 호출자가 실패로 보지 않고 다시 시도하게 한다 */
		*val = CFG_RD_RRS_VAL;	/* [한국어] 규격이 정한 RRS 읽기 값을 담고 */
		return PCIBIOS_SUCCESSFUL;	/* [한국어] 성공으로 끝낸다 */
	}

fail:	/* [한국어] **실패 경로** */
	*val = 0xffffffff;	/* [한국어] 응답이 없을 때의 관례대로 전부 1 을 담고 */
	return PCIBIOS_SET_FAILED;	/* [한국어] 실패로 끝낸다 */
}

/* [한국어]
 * advk_pcie_wr_conf - PIO 시퀀스로 config 공간에 쓴다
 *
 * @bus:   대상 버스.
 * @devfn: 대상 장치·함수 번호.
 * @where: 쓸 오프셋.
 * @size:  1, 2, 4 중 하나.
 * @val:   쓸 값.
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_DEVICE_NOT_FOUND / PCIBIOS_SET_FAILED.
 *
 * struct pci_ops 의 write 로 등록된다. 읽기 쪽과 구조가 같지만 세 가지가
 * 다르다.
 *
 *   1. **정렬을 요구한다.** where % size 가 0 이 아니면 곧바로 실패한다.
 *      읽기 쪽에는 이 검사가 없다.
 *   2. **strobe 를 계산해야 한다.** PIO 는 4바이트 단위로만 나가므로,
 *      그중 어느 바이트가 실제로 쓰일 것인지를 바이트 인에이블로 알려야
 *      한다. 오프셋의 하위 2비트만큼 값을 시프트해 자리를 맞추고,
 *      GENMASK(size-1, 0) 을 같은 만큼 밀어 strobe 를 만든다.
 *   3. **RRS 를 정상으로 보지 않는다.** check_pio_status 에 allow_rrs 를
 *      false 로 넘긴다 — 쓰기에서는 RRS 가 오류이며 재시도 대상이다.
 *      그래서 실패했을 때 돌려줄 대체 값도 없어, 앞선 트랜잭션이 도는
 *      중이면 곧바로 실패한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 읽기와 마찬가지로 udelay 폴링이다.
 *
 * 호출 체인:
 *   PCI 코어 -> pci_ops.write -> [이 함수] -> advk_pcie_valid_device()
 *     -> pci_bridge_emul_conf_write() 또는
 *        advk_pcie_wait_pio() -> advk_pcie_check_pio_status()
 */
static int advk_pcie_wr_conf(struct pci_bus *bus, u32 devfn,
				int where, int size, u32 val)
{
	struct advk_pcie *pcie = bus->sysdata;	/* [한국어] 버스의 sysdata 에서 이 드라이버 인스턴스를 꺼낸다 */
	u32 reg;	/* [한국어] PIO 레지스터에 쓸 값 */
	u32 data_strobe = 0x0;	/* [한국어] 유효 바이트를 알릴 strobe */
	int retry_count;	/* [한국어] PIO 재시도 누적 횟수 */
	int offset;	/* [한국어] 오프셋의 하위 2비트 */
	int ret;	/* [한국어] 하위 호출 결과 */

	if (!advk_pcie_valid_device(pcie, bus, devfn))	/* [한국어] 존재할 수 없는 조합이면 */
		return PCIBIOS_DEVICE_NOT_FOUND;	/* [한국어] 장치 없음으로 끝낸다 */

	if (pci_is_root_bus(bus))	/* [한국어] 루트 버스이면 */
		return pci_bridge_emul_conf_write(&pcie->bridge, where,	/* [한국어] 에뮬레이션 브리지가 처리한다 */
						  size, val);	/* [한국어] 오프셋·크기·값을 그대로 넘긴다 */

	if (where % size)	/* [한국어] **정렬을 요구한다** — 읽기 쪽에는 없는 검사다. 4바이트 안에서 크기의 배수 자리에 놓여야 strobe 계산이 성립한다 */
		return PCIBIOS_SET_FAILED;	/* [한국어] 어긋나면 실패로 끝낸다 */

	if (advk_pcie_pio_is_running(pcie))	/* [한국어] 앞선 전송이 도는 중이면 */
		return PCIBIOS_SET_FAILED;	/* [한국어] **곧바로 실패한다** — 쓰기에는 RRS 같은 대체 결과가 없다 */

	/* Program the control register */
	reg = advk_readl(pcie, PIO_CTRL);	/* [한국어] PIO 제어 레지스터를 읽어 */
	reg &= ~PIO_CTRL_TYPE_MASK;	/* [한국어] 기존 종류 필드를 지우고 */
	if (pci_is_root_bus(bus->parent))	/* [한국어] 부모가 루트 버스이면 */
		reg |= PCIE_CONFIG_WR_TYPE0;	/* [한국어] config 쓰기 type 0 을, */
	else	/* [한국어] 아니면 */
		reg |= PCIE_CONFIG_WR_TYPE1;	/* [한국어] type 1 을 넣는다 */
	advk_writel(pcie, reg, PIO_CTRL);	/* [한국어] 고친 값을 되쓴다 */

	/* Program the address registers */
	reg = ALIGN_DOWN(PCIE_ECAM_OFFSET(bus->number, devfn, where), 4);	/* [한국어] ECAM 방식으로 계산한 주소를 4바이트 정렬해 */
	advk_writel(pcie, reg, PIO_ADDR_LS);	/* [한국어] 하위 워드에 쓰고 */
	advk_writel(pcie, 0, PIO_ADDR_MS);	/* [한국어] 상위 워드는 0 으로 둔다 */

	/* Calculate the write strobe */
	offset      = where & 0x3;	/* [한국어] **오프셋의 하위 2비트가 4바이트 안에서의 자리다** */
	reg         = val << (8 * offset);	/* [한국어] 값을 그 자리로 밀어 올리고 */
	data_strobe = GENMASK(size - 1, 0) << offset;	/* [한국어] **크기만큼의 바이트 인에이블을 같은 자리로 민다.** PIO 는 4바이트 단위로만 나가므로 어느 바이트가 유효한지 이 값으로 알린다 */

	/* Program the data register */
	advk_writel(pcie, reg, PIO_WR_DATA);	/* [한국어] 자리 맞춘 값을 데이터 레지스터에 쓰고 */

	/* Program the data strobe */
	advk_writel(pcie, data_strobe, PIO_WR_DATA_STRB);	/* [한국어] 유효 바이트를 strobe 에 쓴다 */

	retry_count = 0;	/* [한국어] 재시도 횟수를 초기화한다 */
	do {	/* [한국어] RRS 재시도를 위해 반복한다 */
		/* Clear PIO DONE ISR and start the transfer */
		advk_writel(pcie, 1, PIO_ISR);	/* [한국어] 완료 표시를 먼저 지우고 */
		advk_writel(pcie, 1, PIO_START);	/* [한국어] 시작 비트를 세워 트랜잭션을 낸다 */

		ret = advk_pcie_wait_pio(pcie);	/* [한국어] 완료를 기다린다 */
		if (ret < 0)	/* [한국어] 못 기다렸으면 */
			return PCIBIOS_SET_FAILED;	/* [한국어] 실패로 끝낸다 */

		retry_count += ret;	/* [한국어] 몇 번 만에 끝났는지를 누적한다 */

		ret = advk_pcie_check_pio_status(pcie, false, NULL);	/* [한국어] **allow_rrs 에 false 를 넘긴다** — 쓰기에서는 RRS 가 오류이며 재시도 대상이다 */
	} while (ret == -EAGAIN && retry_count < PIO_RETRY_CNT);	/* [한국어] RRS 로 -EAGAIN 이 왔고 상한 안이면 다시 낸다 */

	return ret < 0 ? PCIBIOS_SET_FAILED : PCIBIOS_SUCCESSFUL;	/* [한국어] 오류이면 실패, 아니면 성공으로 답한다 */
}

static struct pci_ops advk_pcie_ops = {
	.read = advk_pcie_rd_conf,	/* [한국어] **PCI 코어의 모든 config 읽기가 이 함수로 들어온다** */
	.write = advk_pcie_wr_conf,	/* [한국어] 쓰기도 마찬가지다. 이 둘이 이 드라이버와 PCI 코어의 주된 접점이다 */
};

/* [한국어]
 * advk_msi_irq_compose_msi_msg - 장치에 알려 줄 MSI 주소와 데이터를 만든다
 *
 * @data: 이 MSI 인터럽트의 irq_data. hwirq 가 벡터 번호다.
 * @msg:  채울 MSI 메시지.
 *
 * 엔드포인트가 인터럽트를 낼 때 **어느 주소에 어떤 값을 쓸지**를 정하는
 * 자리다. 그 주소와 값이 장치의 MSI capability 에 프로그래밍된다.
 *
 * 주소가 특이하다 — virt_to_phys(pcie) 즉 **이 드라이버 구조체 자신의
 * 물리 주소**를 쓴다. 실제로 그 메모리가 쓰이는 것이 아니라,
 * advk_pcie_setup_hw() 가 같은 주소를 PCIE_MSI_ADDR_LOW/HIGH_REG 에
 * 등록해 두어 컨트롤러가 그 주소로 가는 쓰기를 가로채 MSI 로 해석하기
 * 때문이다. 즉 두 곳이 같은 값을 쓰기로 약속한 것이며, 주소 자체에
 * 의미가 있는 것이 아니라 **다른 어떤 것과도 겹치지 않는 물리 주소**면
 * 된다는 뜻이다.
 *
 * 데이터는 hwirq 즉 32개 중 몇 번째 벡터인지다. 그 값이 인터럽트가
 * 왔을 때 어느 벡터인지 되짚는 근거가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치의 MSI 설정 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_chip.irq_compose_msi_msg -> [이 함수] -> virt_to_phys()
 */
static void advk_msi_irq_compose_msi_msg(struct irq_data *data,
					 struct msi_msg *msg)
{
	struct advk_pcie *pcie = irq_data_get_irq_chip_data(data);	/* [한국어] irq_chip 데이터에서 이 드라이버 인스턴스를 꺼낸다 */
	phys_addr_t msi_addr = virt_to_phys(pcie);	/* [한국어] **이 드라이버 구조체 자신의 물리 주소를 MSI 목적지로 쓴다.** setup_hw 가 같은 주소를 컨트롤러에 등록해 두어, 그 주소로 가는 쓰기를 MSI 로 가로챈다 */

	msg->address_lo = lower_32_bits(msi_addr);	/* [한국어] 주소 하위 워드 */
	msg->address_hi = upper_32_bits(msi_addr);	/* [한국어] 상위 워드 */
	msg->data = data->hwirq;	/* [한국어] **데이터는 벡터 번호다.** 인터럽트가 왔을 때 어느 벡터인지 되짚는 근거가 된다 */
}

/* [한국어]
 * advk_msi_irq_mask - MSI 벡터 하나를 마스크한다
 *
 * @d: 마스크할 인터럽트의 irq_data.
 *
 * PCIE_MSI_MASK_REG 의 해당 비트를 세운다. **비트 번호가 곧 벡터 번호**
 * (hwirq)라 32개가 한 레지스터에 들어간다.
 *
 * **raw_spinlock 으로 감싸는 이유**는 읽고-고치고-쓰는 세 단계이기
 * 때문이다. 그 사이에 다른 CPU 나 인터럽트 핸들러가 같은 레지스터를
 * 고치면 한쪽의 변경이 사라진다. raw 판이면서 irqsave 인 것은 이 함수가
 * 인터럽트 컨텍스트에서도 불릴 수 있기 때문이다.
 *
 * msi_used_lock(뮤텍스)과 별개의 락인 이유가 여기 있다 — 벡터 할당은
 * 프로세스 컨텍스트에서만 일어나 잠들 수 있는 뮤텍스로 충분하지만,
 * 마스크·언마스크는 인터럽트 컨텍스트에서도 불려 잠들 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_mask -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> advk_readl()/advk_writel()
 */
static void advk_msi_irq_mask(struct irq_data *d)
{
	struct advk_pcie *pcie = d->domain->host_data;	/* [한국어] 도메인의 host_data 에 이 드라이버 인스턴스가 들어 있다 */
	irq_hw_number_t hwirq = irqd_to_hwirq(d);	/* [한국어] 몇 번째 벡터인지 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask;	/* [한국어] 읽고 고칠 마스크 값 */

	raw_spin_lock_irqsave(&pcie->msi_irq_lock, flags);	/* [한국어] **읽고-고치고-쓰기를 원자적으로 한다.** 인터럽트 컨텍스트에서도 불릴 수 있어 raw 판이며 irqsave 다 */
	mask = advk_readl(pcie, PCIE_MSI_MASK_REG);	/* [한국어] 현재 마스크를 읽고 */
	mask |= BIT(hwirq);	/* [한국어] **비트 번호가 곧 벡터 번호다** — 32개가 한 레지스터에 들어간다 */
	advk_writel(pcie, mask, PCIE_MSI_MASK_REG);	/* [한국어] 고친 값을 되쓴다 */
	raw_spin_unlock_irqrestore(&pcie->msi_irq_lock, flags);	/* [한국어] 락을 푼다 */
}

/* [한국어]
 * advk_msi_irq_unmask - MSI 벡터 하나의 마스크를 푼다
 *
 * @d: 마스크를 풀 인터럽트의 irq_data.
 *
 * advk_msi_irq_mask() 의 짝이며 비트를 지우는 것만 다르다. 같은
 * raw_spinlock 으로 읽고-고치고-쓰기를 보호한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_unmask -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> advk_readl()/advk_writel()
 */
static void advk_msi_irq_unmask(struct irq_data *d)
{
	struct advk_pcie *pcie = d->domain->host_data;	/* [한국어] 도메인의 host_data 에서 이 드라이버 인스턴스를 꺼낸다 */
	irq_hw_number_t hwirq = irqd_to_hwirq(d);	/* [한국어] 몇 번째 벡터인지 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask;	/* [한국어] 읽고 고칠 마스크 값 */

	raw_spin_lock_irqsave(&pcie->msi_irq_lock, flags);	/* [한국어] 같은 락으로 감싼다 */
	mask = advk_readl(pcie, PCIE_MSI_MASK_REG);	/* [한국어] 현재 마스크를 읽고 */
	mask &= ~BIT(hwirq);	/* [한국어] 해당 비트를 지운 뒤 */
	advk_writel(pcie, mask, PCIE_MSI_MASK_REG);	/* [한국어] 되쓴다 */
	raw_spin_unlock_irqrestore(&pcie->msi_irq_lock, flags);	/* [한국어] 락을 푼다 */
}

static struct irq_chip advk_msi_bottom_irq_chip = {	/* [한국어] **MSI 의 아래쪽 irq_chip.** 벡터마다 이 구조체가 매달린다 */
	.name			= "MSI",	/* [한국어] /proc/interrupts 에 보일 이름 */
	.irq_compose_msi_msg	= advk_msi_irq_compose_msi_msg,	/* [한국어] 장치에 알려 줄 주소와 데이터를 만든다 */
	.irq_mask		= advk_msi_irq_mask,	/* [한국어] 벡터 마스크 */
	.irq_unmask		= advk_msi_irq_unmask,	/* [한국어] 벡터 언마스크 */
};

/* [한국어]
 * advk_msi_irq_domain_alloc - MSI 벡터를 요청 개수만큼 잡아 준다
 *
 * @domain:  MSI 내부 도메인.
 * @virq:    커널이 배정한 가상 IRQ 번호의 시작.
 * @nr_irqs: 요청한 벡터 수.
 * @args:    쓰지 않는다.
 * @return: 0 이면 성공, 빈 자리가 없으면 -ENOSPC.
 *
 * **벡터가 32개(MSI_IRQ_NUM)뿐이라 비트맵으로 관리한다.**
 * bitmap_find_free_region() 이 요청 개수를 2의 거듭제곱으로 올림한
 * 크기의 연속 구간을 찾아 준다 — MSI 는 여러 벡터를 쓸 때 **연속이고
 * 2의 거듭제곱 개수여야** 하고 시작 번호도 그 크기에 정렬되어야 한다는
 * 규격 제약이 있기 때문이다. order_base_2(nr_irqs) 가 그 올림을 한다.
 *
 * 찾은 구간의 각 벡터에 대해 irq_domain_set_info() 로 hwirq 를 잇고
 * 아래쪽 irq_chip(advk_msi_bottom_irq_chip)과 handle_simple_irq 를
 * 매단다. MSI 는 에지 성격이라 레벨 핸들러가 필요 없다.
 *
 * 동기화: 비트맵 조작을 msi_used_lock 뮤텍스로 감싼다. 잠들 수 있는
 * 락을 쓸 수 있는 것은 이 경로가 프로세스 컨텍스트에서만 불리기
 * 때문이다.
 *
 * **NVMe 관점**: NVMe 드라이버는 CPU 수만큼 큐를 만들려고 MSI-X 벡터를
 * 많이 요청하는데, 이 컨트롤러에서는 32개 상한에 걸려 그보다 적게 받게
 * 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치의 MSI 설정 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_domain_ops.alloc -> [이 함수]
 *     -> bitmap_find_free_region() -> irq_domain_set_info()
 */
static int advk_msi_irq_domain_alloc(struct irq_domain *domain,
				     unsigned int virq,
				     unsigned int nr_irqs, void *args)
{
	struct advk_pcie *pcie = domain->host_data;	/* [한국어] 도메인의 host_data 에서 이 드라이버 인스턴스를 꺼낸다 */
	int hwirq, i;	/* [한국어] hwirq 는 잡은 구간의 시작 벡터, i 는 순회 인덱스 */

	mutex_lock(&pcie->msi_used_lock);	/* [한국어] **비트맵을 뮤텍스로 감싼다.** 이 경로는 프로세스 컨텍스트에서만 불려 잠들 수 있다 */
	hwirq = bitmap_find_free_region(pcie->msi_used, MSI_IRQ_NUM,	/* [한국어] **연속된 빈 구간을 찾는다** */
					order_base_2(nr_irqs));	/* [한국어] **요청 개수를 2의 거듭제곱으로 올린 크기**로 찾는다 — 다중 MSI 는 연속이고 2의 거듭제곱 개수여야 하며 시작도 그 크기에 정렬되어야 한다는 규격 제약 때문이다 */
	mutex_unlock(&pcie->msi_used_lock);	/* [한국어] 락을 푼다 */
	if (hwirq < 0)	/* [한국어] 빈 구간이 없으면 */
		return -ENOSPC;	/* [한국어] 공간 없음으로 알린다. **NVMe 처럼 벡터를 많이 요구하는 장치가 32개 상한에 걸리는 자리다** */

	for (i = 0; i < nr_irqs; i++)	/* [한국어] 잡은 구간의 벡터마다 */
		irq_domain_set_info(domain, virq + i, hwirq + i,	/* [한국어] 가상 IRQ 와 hwirq 를 잇고 */
				    &advk_msi_bottom_irq_chip,	/* [한국어] 아래쪽 irq_chip 을 매달고 */
				    domain->host_data, handle_simple_irq,	/* [한국어] chip_data 로 드라이버 인스턴스를, 핸들러로 handle_simple_irq 를 준다 — MSI 는 에지 성격이라 레벨 처리가 필요 없다 */
				    NULL, NULL);	/* [한국어] 나머지 두 인자는 쓰지 않는다 */

	return 0;	/* [한국어] 구간을 잡았으면 성공 */
}

/* [한국어]
 * advk_msi_irq_domain_free - 잡아 두었던 MSI 벡터를 놓는다
 *
 * @domain:  MSI 내부 도메인.
 * @virq:    놓을 가상 IRQ 번호의 시작.
 * @nr_irqs: 놓을 벡터 수.
 *
 * alloc 의 짝이다. irq_data 에서 hwirq 를 되찾아 그 자리부터
 * order_base_2(nr_irqs) 크기만큼을 비트맵에서 놓아 준다 — 잡을 때와
 * 같은 크기 계산을 써야 짝이 맞는다.
 *
 * 동기화: alloc 과 같은 뮤텍스로 감싼다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 제거 또는 MSI 해제 시).
 *
 * 호출 체인:
 *   MSI 코어 -> irq_domain_ops.free -> [이 함수]
 *     -> irq_domain_get_irq_data() -> bitmap_release_region()
 */
static void advk_msi_irq_domain_free(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);	/* [한국어] **해제 전에 irq_data 에서 hwirq 를 되찾는다** — 그것이 비트맵에서의 위치다 */
	struct advk_pcie *pcie = domain->host_data;	/* [한국어] 도메인의 host_data 에서 이 드라이버 인스턴스를 꺼낸다 */

	mutex_lock(&pcie->msi_used_lock);	/* [한국어] 할당 때와 같은 뮤텍스로 감싼다 */
	bitmap_release_region(pcie->msi_used, d->hwirq, order_base_2(nr_irqs));	/* [한국어] **잡을 때와 같은 크기 계산으로 놓는다** — 그래야 짝이 맞는다 */
	mutex_unlock(&pcie->msi_used_lock);	/* [한국어] 락을 푼다 */
}

static const struct irq_domain_ops advk_msi_domain_ops = {	/* [한국어] **MSI 부모 도메인의 동작.** 마스크·언마스크는 irq_chip 쪽에 있고 여기에는 할당과 해제만 있다 */
	.alloc = advk_msi_irq_domain_alloc,	/* [한국어] 벡터 잡기 */
	.free = advk_msi_irq_domain_free,	/* [한국어] 벡터 놓기 */
};

/* [한국어]
 * advk_pcie_irq_mask - INTx 인터럽트 하나를 마스크한다
 *
 * @d: 마스크할 인터럽트의 irq_data. hwirq 가 INTA~INTD 중 어느 것인지다.
 *
 * MSI 쪽과 구조가 같지만 대상 레지스터가 다르다 — INTx 는
 * PCIE_ISR1_MASK_REG 의 INTX_ASSERT 비트로 제어한다.
 *
 * **MSI 와 락을 따로 두는 이유**는 서로 다른 레지스터를 만지기 때문이다.
 * 같은 락을 쓰면 MSI 마스크가 INTx 마스크를 불필요하게 막게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽. INTx 는
 * 레벨 인터럽트라 핸들러가 처리 중에 마스크를 걸므로 인터럽트
 * 컨텍스트에서 불리는 일이 흔하다.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_mask -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> advk_readl()/advk_writel()
 */
static void advk_pcie_irq_mask(struct irq_data *d)
{
	struct advk_pcie *pcie = d->domain->host_data;	/* [한국어] 도메인의 host_data 에서 이 드라이버 인스턴스를 꺼낸다 */
	irq_hw_number_t hwirq = irqd_to_hwirq(d);	/* [한국어] INTA~INTD 중 몇 번째인지 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask;	/* [한국어] 읽고 고칠 마스크 값 */

	raw_spin_lock_irqsave(&pcie->irq_lock, flags);	/* [한국어] **MSI 와 다른 락이다** — 지키는 레지스터가 달라 서로를 막을 이유가 없다 */
	mask = advk_readl(pcie, PCIE_ISR1_MASK_REG);	/* [한국어] ISR1 마스크를 읽고 */
	mask |= PCIE_ISR1_INTX_ASSERT(hwirq);	/* [한국어] 해당 INTx 비트를 세운 뒤 */
	advk_writel(pcie, mask, PCIE_ISR1_MASK_REG);	/* [한국어] 되쓴다 */
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);	/* [한국어] 락을 푼다 */
}

/* [한국어]
 * advk_pcie_irq_unmask - INTx 인터럽트 하나의 마스크를 푼다
 *
 * @d: 마스크를 풀 인터럽트의 irq_data.
 *
 * advk_pcie_irq_mask() 의 짝이며 비트를 지우는 것만 다르다.
 *
 * 레벨 인터럽트에서는 장치가 원인을 없앤 뒤 이 함수가 불려야 다음
 * 인터럽트를 받을 수 있으므로, 이 함수가 실질적으로 인터럽트 처리
 * 주기를 닫는 역할을 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트와 인터럽트 컨텍스트 양쪽.
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_chip.irq_unmask -> [이 함수]
 *     -> raw_spin_lock_irqsave() -> advk_readl()/advk_writel()
 */
static void advk_pcie_irq_unmask(struct irq_data *d)
{
	struct advk_pcie *pcie = d->domain->host_data;	/* [한국어] 도메인의 host_data 에서 이 드라이버 인스턴스를 꺼낸다 */
	irq_hw_number_t hwirq = irqd_to_hwirq(d);	/* [한국어] INTA~INTD 중 몇 번째인지 */
	unsigned long flags;	/* [한국어] 인터럽트 상태를 담아 둘 자리 */
	u32 mask;	/* [한국어] 읽고 고칠 마스크 값 */

	raw_spin_lock_irqsave(&pcie->irq_lock, flags);	/* [한국어] 같은 락으로 감싼다 */
	mask = advk_readl(pcie, PCIE_ISR1_MASK_REG);	/* [한국어] ISR1 마스크를 읽고 */
	mask &= ~PCIE_ISR1_INTX_ASSERT(hwirq);	/* [한국어] 해당 INTx 비트를 지운 뒤 */
	advk_writel(pcie, mask, PCIE_ISR1_MASK_REG);	/* [한국어] 되쓴다. **레벨 인터럽트에서는 이 언마스크가 처리 주기를 닫는다** */
	raw_spin_unlock_irqrestore(&pcie->irq_lock, flags);	/* [한국어] 락을 푼다 */
}

/* [한국어]
 * advk_pcie_irq_map - INTx 가상 IRQ 하나를 이 컨트롤러에 잇는다
 *
 * @h:     INTx irq 도메인.
 * @virq:  커널이 배정한 가상 IRQ 번호.
 * @hwirq: INTA~INTD 중 몇 번째인지(0~3).
 * @return: 항상 0.
 *
 * 장치 트리의 interrupt-map 을 따라 어떤 장치의 INTx 가 이 도메인의
 * 어느 hwirq 로 오는지 정해지면, 커널이 그 가상 IRQ 를 만들면서 이
 * 콜백을 부른다.
 *
 * 셋을 한다.
 *   - **IRQ_LEVEL 상태 플래그를 세운다.** INTx 는 레벨 트리거이며,
 *     이 표시가 없으면 /proc/interrupts 표기와 일부 코어 처리가
 *     에지로 다뤄진다.
 *   - irq_chip 과 handle_level_irq 를 매단다. 레벨 핸들러는 처리 전에
 *     마스크를 걸고 처리 후 푸는 순서를 지켜 준다.
 *   - chip_data 에 드라이버 인스턴스를 담아 마스크·언마스크에서 꺼내
 *     쓰게 한다.
 *
 * **irq_chip 이 정적 구조체가 아니라 pcie->irq_chip 인 점**이 특이하다.
 * probe 가 그 안에 장치 이름을 넣어 두므로 인스턴스마다 다른 이름을
 * 갖는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(가상 IRQ 생성 시).
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_domain_ops.map -> [이 함수]
 *     -> irq_set_status_flags() -> irq_set_chip_and_handler()
 */
static int advk_pcie_irq_map(struct irq_domain *h,
			     unsigned int virq, irq_hw_number_t hwirq)
{
	struct advk_pcie *pcie = h->host_data;	/* [한국어] 도메인의 host_data 에서 이 드라이버 인스턴스를 꺼낸다 */

	irq_set_status_flags(virq, IRQ_LEVEL);	/* [한국어] **INTx 는 레벨 트리거임을 알린다.** 이 표시가 없으면 코어와 /proc/interrupts 가 에지로 다룬다 */
	irq_set_chip_and_handler(virq, &pcie->irq_chip,	/* [한국어] **인스턴스마다 다른 irq_chip 을 매단다** — 이름이 장치별로 지어져 있다 */
				 handle_level_irq);	/* [한국어] 레벨 핸들러는 처리 전에 마스크를 걸고 처리 후 푼다 */
	irq_set_chip_data(virq, pcie);	/* [한국어] 마스크·언마스크에서 꺼내 쓸 수 있게 인스턴스를 담아 둔다 */

	return 0;	/* [한국어] 매핑에 실패할 일이 없어 늘 성공이다 */
}

static const struct irq_domain_ops advk_pcie_irq_domain_ops = {	/* [한국어] **INTx 도메인의 동작** */
	.map = advk_pcie_irq_map,	/* [한국어] 가상 IRQ 를 만들 때 부를 함수 */
	.xlate = irq_domain_xlate_onecell,	/* [한국어] 장치 트리의 interrupt 속성이 셀 하나(0~3)로 INTx 를 가리킨다는 뜻이다 */
};

#define ADVK_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				 MSI_FLAG_USE_DEF_CHIP_OPS	| \
				 MSI_FLAG_PCI_MSI_MASK_PARENT	| \
				 MSI_FLAG_NO_AFFINITY)
#define ADVK_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				  MSI_FLAG_PCI_MSIX		| \
				  MSI_FLAG_MULTI_PCI_MSI)

static const struct msi_parent_ops advk_msi_parent_ops = {	/* [한국어] **MSI 부모 도메인의 성질을 커널 공용 코드에 알린다.** 이것을 보고 코드가 PCI MSI/MSI-X 자식 도메인을 만들어 준다 */
	.required_flags		= ADVK_MSI_FLAGS_REQUIRED,	/* [한국어] 반드시 필요한 플래그 묶음 — 기본 도메인·칩 동작을 쓰고, 마스크를 부모(이 파일)가 맡으며, **어피니티 설정을 지원하지 않는다** */
	.supported_flags	= ADVK_MSI_FLAGS_SUPPORTED,	/* [한국어] 지원 가능한 플래그 묶음 — 일반 플래그 전부와 MSI-X, 다중 MSI */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,	/* [한국어] PCI MSI 버스에 붙는 도메인이라고 알린다 */
	.prefix			= "advk-",	/* [한국어] 도메인 이름에 붙일 접두사 */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,	/* [한국어] 자식 도메인 정보를 채우는 공용 헬퍼 */
};

/* [한국어]
 * advk_pcie_init_msi_irq_domain - MSI 인터럽트 도메인을 만든다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공, 도메인을 못 만들면 -ENOMEM.
 *
 * 이 컨트롤러는 자체 MSI 컨트롤러를 가지고 있어, ITS 같은 상위 MSI
 * 컨트롤러에 기대지 않고 직접 벡터를 나눠 준다. 그 통로가 되는 도메인을
 * 여기서 만든다.
 *
 * 크기가 MSI_IRQ_NUM(32)로 고정이다 — 하드웨어의 MSI 상태·마스크
 * 레지스터가 32비트라 그 이상은 다룰 수 없다.
 *
 * msi_create_parent_irq_domain() 은 이 도메인을 **부모 도메인**으로 두고,
 * PCI MSI/MSI-X 쪽 자식 도메인은 커널의 공용 코드가 advk_msi_parent_ops
 * 의 플래그에 따라 만들어 준다. 그 플래그가 MSI-X 와 다중 MSI 지원 여부,
 * 그리고 어피니티 설정을 지원하지 않는다는 사실(MSI_FLAG_NO_AFFINITY)을
 * 알린다.
 *
 * 두 락을 여기서 초기화한다 — 마스크·언마스크용 raw_spinlock 과
 * 비트맵 할당용 뮤텍스다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   advk_pcie_probe() -> [이 함수] -> msi_create_parent_irq_domain()
 */
static int advk_pcie_init_msi_irq_domain(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;	/* [한국어] 도메인 이름에 쓸 fwnode 를 얻기 위한 장치 */

	raw_spin_lock_init(&pcie->msi_irq_lock);	/* [한국어] **마스크·언마스크용 스핀락을 초기화한다** */
	mutex_init(&pcie->msi_used_lock);	/* [한국어] **벡터 할당용 뮤텍스도 초기화한다.** 지키는 대상과 실행 컨텍스트가 달라 락이 둘이다 */

	struct irq_domain_info info = {	/* [한국어] 만들 도메인의 명세 */
		.fwnode		= dev_fwnode(dev),	/* [한국어] 장치 트리 노드에서 온 fwnode */
		.ops		= &advk_msi_domain_ops,	/* [한국어] 할당·해제 동작 */
		.host_data	= pcie,	/* [한국어] 콜백에서 꺼내 쓸 인스턴스 */
		.size		= MSI_IRQ_NUM,	/* [한국어] **벡터 32개. 하드웨어 레지스터가 32비트인 데서 온 상한이다** */
	};

	pcie->msi_inner_domain = msi_create_parent_irq_domain(&info, &advk_msi_parent_ops);	/* [한국어] **이 도메인을 부모로 두고 PCI MSI 자식 도메인까지 만들어 달라고 한다** */
	if (!pcie->msi_inner_domain)	/* [한국어] 못 만들면 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 알린다 */

	return 0;	/* [한국어] 만들었으면 성공 */
}

/* [한국어]
 * advk_pcie_remove_msi_irq_domain - MSI 인터럽트 도메인을 없앤다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * init 의 짝이다. 도메인 하나를 없애는 것이 전부이며, 그 안에 매달린
 * 가상 IRQ 들은 도메인 제거 과정에서 함께 정리된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로, remove).
 *
 * 호출 체인:
 *   advk_pcie_probe() 실패 경로 / advk_pcie_remove() -> [이 함수]
 *     -> irq_domain_remove()
 */
static void advk_pcie_remove_msi_irq_domain(struct advk_pcie *pcie)
{
	irq_domain_remove(pcie->msi_inner_domain);	/* [한국어] 도메인 하나를 없앤다. 그 안의 가상 IRQ 들은 함께 정리된다 */
}

/* [한국어]
 * advk_pcie_init_irq_domain - INTx(레거시) 인터럽트 도메인을 만든다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공. 장치 트리에 인터럽트 컨트롤러 자식 노드가 없으면
 *          -ENODEV, 할당이나 도메인 생성이 실패하면 -ENOMEM.
 *
 * INTA~INTD 네 개의 레거시 인터럽트를 장치 드라이버에게 중계하는
 * 도메인이다. MSI 도메인과 달리 **장치 트리의 자식 노드가 필요하다** —
 * 엔드포인트의 장치 트리 노드가 interrupt-parent 로 그 노드를 가리켜야
 * 어느 도메인으로 갈지 정해지기 때문이다. 그래서 of_get_next_child() 로
 * 첫 자식 노드를 찾고, 없으면 실패한다.
 *
 * irq_chip 은 **정적 구조체가 아니라 pcie->irq_chip 을 쓴다.** 이름을
 * devm_kasprintf() 로 "<장치이름>-irq" 처럼 만들어 넣기 때문이며, 그래서
 * /proc/interrupts 에 인스턴스마다 구분되는 이름이 보인다.
 *
 * 크기가 PCI_NUM_INTX(4)로 고정이고, xlate 가
 * irq_domain_xlate_onecell 이라 장치 트리의 interrupt 속성이 셀 하나
 * (0~3)로 INTx 를 가리킨다.
 *
 * 에러 경로: 어느 실패든 out_put_node 로 모여 찾은 자식 노드의 참조를
 * 반드시 놓아 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   advk_pcie_probe() -> [이 함수] -> of_get_next_child()
 *     -> devm_kasprintf() -> irq_domain_create_linear()
 */
static int advk_pcie_init_irq_domain(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;	/* [한국어] 오류 메시지를 낼 장치 */
	struct device_node *node = dev->of_node;	/* [한국어] 이 컨트롤러의 장치 트리 노드 */
	struct device_node *pcie_intc_node;	/* [한국어] 그 아래의 인터럽트 컨트롤러 자식 노드 */
	struct irq_chip *irq_chip;	/* [한국어] 채울 irq_chip 을 가리킬 포인터 */
	int ret = 0;	/* [한국어] 호출자에게 돌려줄 값 */

	raw_spin_lock_init(&pcie->irq_lock);	/* [한국어] **INTx 마스크용 스핀락을 초기화한다** */

	pcie_intc_node =  of_get_next_child(node, NULL);	/* [한국어] **장치 트리의 첫 자식 노드를 찾는다** — 엔드포인트가 interrupt-parent 로 그 노드를 가리켜야 이 도메인으로 온다 */
	if (!pcie_intc_node) {	/* [한국어] 없으면 장치 트리가 이 드라이버가 기대하는 모양이 아니다 */
		dev_err(dev, "No PCIe Intc node found\n");	/* [한국어] 그 사실을 알리고 */
		return -ENODEV;	/* [한국어] 장치 없음으로 돌아간다. **참조를 잡기 전이라 놓을 것이 없다** */
	}

	irq_chip = &pcie->irq_chip;	/* [한국어] 인스턴스가 품고 있는 irq_chip 을 가리킨다 */

	irq_chip->name = devm_kasprintf(dev, GFP_KERNEL, "%s-irq",	/* [한국어] **이름을 "<장치이름>-irq" 로 짓는다** — 그래서 /proc/interrupts 에 인스턴스마다 구분되는 이름이 보인다 */
					dev_name(dev));	/* [한국어] 장치 이름을 넣는다 */
	if (!irq_chip->name) {	/* [한국어] 못 잡으면 */
		ret = -ENOMEM;	/* [한국어] 메모리 부족을 담고 */
		goto out_put_node;	/* [한국어] 노드 참조를 놓는 공통 출구로 간다 */
	}

	irq_chip->irq_mask = advk_pcie_irq_mask;	/* [한국어] 마스크 함수를 매달고 */
	irq_chip->irq_unmask = advk_pcie_irq_unmask;	/* [한국어] 언마스크 함수도 매단다. 그 밖의 콜백은 없다 */

	pcie->irq_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,	/* [한국어] **INTx 도메인을 만든다.** 찾은 자식 노드에 붙이고 */
						    &advk_pcie_irq_domain_ops, pcie);	/* [한국어] 동작 묶음과 인스턴스를 준다. 크기가 PCI_NUM_INTX(4)다 */
	if (!pcie->irq_domain) {	/* [한국어] 못 만들면 */
		dev_err(dev, "Failed to get a INTx IRQ domain\n");	/* [한국어] 그 사실을 알리고 */
		ret = -ENOMEM;	/* [한국어] 메모리 부족을 담고 */
		goto out_put_node;	/* [한국어] 공통 출구로 간다 */
	}

out_put_node:	/* [한국어] **공통 출구** — 성공·실패 어느 쪽이든 노드 참조를 반드시 놓는다 */
	of_node_put(pcie_intc_node);	/* [한국어] of_get_next_child() 가 올린 참조를 되돌린다 */
	return ret;	/* [한국어] 담아 둔 결과를 돌려준다 */
}

/* [한국어]
 * advk_pcie_remove_irq_domain - INTx 인터럽트 도메인을 없앤다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * init 의 짝이다. irq_chip 의 이름은 devm_kasprintf() 로 잡았으므로
 * 장치가 사라질 때 커널이 자동으로 되돌리고, 자식 노드 참조는 init 가
 * 이미 놓았으므로 여기서는 도메인만 없앤다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로, remove).
 *
 * 호출 체인:
 *   advk_pcie_probe() 실패 경로 / advk_pcie_remove() -> [이 함수]
 *     -> irq_domain_remove()
 */
static void advk_pcie_remove_irq_domain(struct advk_pcie *pcie)
{
	irq_domain_remove(pcie->irq_domain);	/* [한국어] 도메인 하나를 없앤다. irq_chip 의 이름은 devm 으로 잡았으므로 커널이 자동 정리한다 */
}

static struct irq_chip advk_rp_irq_chip = {	/* [한국어] **루트 포트용 irq_chip.** 마스크·언마스크가 없는 빈 구조체다 */
	.name = "advk-RP",	/* [한국어] /proc/interrupts 에 보일 이름. 소프트웨어가 만들어 내는 인터럽트라 실제 하드웨어 선이 없다 */
};

/* [한국어]
 * advk_pcie_rp_irq_map - 루트 포트 자신의 가상 IRQ 를 잇는다
 *
 * @h:     루트 포트 irq 도메인.
 * @virq:  커널이 배정한 가상 IRQ 번호.
 * @hwirq: 이 도메인의 hwirq. 크기가 1 이라 늘 0 이다.
 * @return: 항상 0.
 *
 * **에뮬레이션된 루트 포트가 자기 인터럽트를 받기 위한 도메인**이다.
 * PME 와 AER 오류 보고가 여기로 들어온다. 진짜 하드웨어 인터럽트 선이
 * 아니라 소프트웨어가 만들어 내는 것이라, irq_chip 에 마스크·언마스크가
 * 없는 빈 구조체(advk_rp_irq_chip)를 쓴다.
 *
 * 핸들러가 handle_simple_irq 인 것도 같은 이유다 — 마스크를 걸고 푸는
 * 레벨 처리가 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(가상 IRQ 생성 시).
 *
 * 호출 체인:
 *   IRQ 코어 -> irq_domain_ops.map -> [이 함수]
 *     -> irq_set_chip_and_handler()
 */
static int advk_pcie_rp_irq_map(struct irq_domain *h,
				unsigned int virq, irq_hw_number_t hwirq)
{
	struct advk_pcie *pcie = h->host_data;	/* [한국어] 도메인의 host_data 에서 이 드라이버 인스턴스를 꺼낸다 */

	irq_set_chip_and_handler(virq, &advk_rp_irq_chip, handle_simple_irq);	/* [한국어] **빈 irq_chip 과 단순 핸들러를 매단다** — 마스크를 걸고 푸는 레벨 처리가 필요 없다 */
	irq_set_chip_data(virq, pcie);	/* [한국어] 인스턴스를 담아 둔다 */

	return 0;	/* [한국어] 매핑에 실패할 일이 없어 늘 성공이다 */
}

static const struct irq_domain_ops advk_pcie_rp_irq_domain_ops = {	/* [한국어] **루트 포트 도메인의 동작** */
	.map = advk_pcie_rp_irq_map,	/* [한국어] 가상 IRQ 를 만들 때 부를 함수 */
	.xlate = irq_domain_xlate_onecell,	/* [한국어] 셀 하나로 번호를 가리킨다. 크기가 1 이라 늘 0 이다 */
};

/* [한국어]
 * advk_pcie_init_rp_irq_domain - 루트 포트 인터럽트 도메인을 만든다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공, 못 만들면 -ENOMEM.
 *
 * **크기가 1 이다.** advk_pcie_handle_pme() 와 advk_pcie_handle_int() 의
 * 상류 주석이 그 이유를 밝힌다 — Aardvark 하드웨어가
 * PCI_EXP_FLAGS_IRQ 와 PCI_ERR_ROOT_AER_IRQ 를 모두 0 으로 돌려주므로,
 * PME 든 AER 이든 PCIe 인터럽트 0 번을 쓰기 때문이다. 즉 루트 포트가
 * 낼 수 있는 인터럽트가 하나뿐인 셈이다.
 *
 * fwnode 로 NULL 을 넘긴다 — 장치 트리에 대응하는 노드가 없는,
 * 소프트웨어만의 도메인이기 때문이다. INTx 도메인이 자식 노드를 요구한
 * 것과 대비된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   advk_pcie_probe() -> [이 함수] -> irq_domain_create_linear()
 */
static int advk_pcie_init_rp_irq_domain(struct advk_pcie *pcie)
{
	pcie->rp_irq_domain = irq_domain_create_linear(NULL, 1, &advk_pcie_rp_irq_domain_ops, pcie);	/* [한국어] **fwnode 로 NULL 을 넘긴다** — 장치 트리에 대응 노드가 없는 소프트웨어만의 도메인이고, 크기는 1 이다 */
	if (!pcie->rp_irq_domain) {	/* [한국어] 못 만들면 */
		dev_err(&pcie->pdev->dev, "Failed to add Root Port IRQ domain\n");	/* [한국어] 그 사실을 알리고 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다 */
	}

	return 0;	/* [한국어] 만들었으면 성공 */
}

/* [한국어]
 * advk_pcie_remove_rp_irq_domain - 루트 포트 인터럽트 도메인을 없앤다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * init 의 짝이며 도메인 하나를 없애는 것이 전부다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로, remove).
 *
 * 호출 체인:
 *   advk_pcie_probe() 실패 경로 / advk_pcie_remove() -> [이 함수]
 *     -> irq_domain_remove()
 */
static void advk_pcie_remove_rp_irq_domain(struct advk_pcie *pcie)
{
	irq_domain_remove(pcie->rp_irq_domain);	/* [한국어] 도메인 하나를 없앤다 */
}

/* [한국어]
 * advk_pcie_handle_pme - PME 메시지를 받아 에뮬레이션 브리지에 기록하고 알린다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * 엔드포인트가 저전력 상태에서 깨어나겠다고 보내는 PM_PME 메시지를
 * 처리한다. 진짜 루트 포트라면 하드웨어가 Root Status 에 요청자 ID 를
 * 기록하고 인터럽트를 내겠지만, 여기서는 **소프트웨어가 그 두 일을
 * 대신한다.**
 *
 *   1. PCIE_MSG_LOG_REG 의 상위 16비트에서 요청자 ID 를 꺼낸다.
 *   2. ISR0 의 PME 비트를 지운다.
 *   3. **아직 PME 가 어서트되지 않았을 때만** 기록한다. 그 자리의 상류
 *      주석이 이유를 밝힌다 — MSG_LOG_REG 는 마지막으로 들어온 메시지를
 *      담으므로, 이미 어서트된 상태에서 덮어쓰면 앞선 요청자 ID 를
 *      잃는다. 그리고 이미 어서트된 상태에서 인터럽트를 또 내지도 않는다.
 *   4. Root Control 의 PMEIE 비트가 켜져 있을 때만 인터럽트를 낸다.
 *      그 자리의 상류 주석대로 Aardvark 는 PCI_EXP_FLAGS_IRQ 를 0 으로
 *      돌려주므로 PCIe 인터럽트 0 번을 쓴다.
 *
 * **handle_int 가 이것을 가장 먼저 부른다** — 그 자리의 상류 주석이
 * 이유를 밝힌다. 요청자 ID 를 놓치지 않으려는 것이다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.**
 *
 * 호출 체인:
 *   advk_pcie_handle_int() -> [이 함수] -> generic_handle_domain_irq()
 */
static void advk_pcie_handle_pme(struct advk_pcie *pcie)
{
	u32 requester = advk_readl(pcie, PCIE_MSG_LOG_REG) >> 16;	/* [한국어] **마지막으로 들어온 메시지 로그의 상위 16비트가 요청자 ID 다** */

	advk_writel(pcie, PCIE_MSG_PM_PME_MASK, PCIE_ISR0_REG);	/* [한국어] ISR0 의 PME 비트를 지운다 — 처리했다는 표시다 */

	/*
	 * PCIE_MSG_LOG_REG contains the last inbound message, so store
	 * the requester ID only when PME was not asserted yet.
	 * Also do not trigger PME interrupt when PME is still asserted.
	 */
	if (!(le32_to_cpu(pcie->bridge.pcie_conf.rootsta) & PCI_EXP_RTSTA_PME)) {	/* [한국어] **아직 PME 가 어서트되지 않았을 때만 기록한다.** 바로 위 상류 주석대로 로그 레지스터는 마지막 메시지만 담으므로, 이미 어서트된 상태에서 덮어쓰면 앞선 요청자 ID 를 잃는다 */
		pcie->bridge.pcie_conf.rootsta = cpu_to_le32(requester | PCI_EXP_RTSTA_PME);	/* [한국어] 요청자 ID 와 PME 비트를 함께 Root Status 에 담는다 */

		/*
		 * Trigger PME interrupt only if PMEIE bit in Root Control is set.
		 * Aardvark HW returns zero for PCI_EXP_FLAGS_IRQ, so use PCIe interrupt 0.
		 */
		if (!(le16_to_cpu(pcie->bridge.pcie_conf.rootctl) & PCI_EXP_RTCTL_PMEIE))	/* [한국어] **PMEIE 가 꺼져 있으면 인터럽트를 내지 않는다** — 기록만 하고 끝낸다 */
			return;	/* [한국어] 그대로 나간다 */

		if (generic_handle_domain_irq(pcie->rp_irq_domain, 0) == -EINVAL)	/* [한국어] **PCIe 인터럽트 0 번으로 보낸다.** 바로 위 상류 주석대로 Aardvark 는 PCI_EXP_FLAGS_IRQ 를 0 으로 돌려주므로 0 번을 쓴다 */
			dev_err_ratelimited(&pcie->pdev->dev, "unhandled PME IRQ\n");	/* [한국어] 매핑이 없으면 속도 제한이 걸린 오류를 남긴다 */
	}
}

/* [한국어]
 * advk_pcie_handle_msi - 들어온 MSI 를 벡터별로 나눠 보낸다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * MSI 상태 레지스터에서 마스크되지 않은 비트만 골라, 세워진 비트마다
 * 그에 대응하는 가상 IRQ 를 부른다.
 *
 *   - 상태와 마스크를 각각 읽어 (상태 & ~마스크 & 전체마스크)로
 *     **실제로 처리해야 할 벡터만** 남긴다.
 *   - 32개를 훑으며 세워진 비트를 찾는다.
 *   - **먼저 그 비트를 지우고** 핸들러를 부른다. 순서가 반대이면
 *     핸들러가 도는 동안 온 같은 벡터의 인터럽트를 지워 버릴 수 있다.
 *   - generic_handle_domain_irq() 가 -EINVAL 을 돌려주면 그 벡터에
 *     매핑된 가상 IRQ 가 없다는 뜻이라, 속도 제한이 걸린 오류를 남긴다.
 *   - 마지막으로 ISR0 의 MSI 대기 비트를 지운다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.**
 *
 * [관찰] 이 함수는 msi_irq_lock 을 잡지 않고 마스크 레지스터를 읽는다.
 * 읽기만 하므로 읽고-고치고-쓰기의 경쟁 대상은 아니다.
 *
 * 호출 체인:
 *   advk_pcie_handle_int() -> [이 함수] -> generic_handle_domain_irq()
 */
static void advk_pcie_handle_msi(struct advk_pcie *pcie)
{
	u32 msi_val, msi_mask, msi_status, msi_idx;	/* [한국어] msi_val 은 상태 원본, msi_mask 는 마스크, msi_status 는 처리 대상, msi_idx 는 순회 인덱스 */

	msi_mask = advk_readl(pcie, PCIE_MSI_MASK_REG);	/* [한국어] 현재 마스크를 읽고 */
	msi_val = advk_readl(pcie, PCIE_MSI_STATUS_REG);	/* [한국어] 상태도 읽어 */
	msi_status = msi_val & ((~msi_mask) & PCIE_MSI_ALL_MASK);	/* [한국어] **마스크되지 않은 것만 남긴다** — 소프트웨어가 원치 않는 벡터는 건드리지 않는다 */

	for (msi_idx = 0; msi_idx < MSI_IRQ_NUM; msi_idx++) {	/* [한국어] 벡터 32개를 훑는다 */
		if (!(BIT(msi_idx) & msi_status))	/* [한국어] 세워지지 않은 벡터는 */
			continue;	/* [한국어] 건너뛴다 */

		advk_writel(pcie, BIT(msi_idx), PCIE_MSI_STATUS_REG);	/* [한국어] **먼저 그 비트를 지운다** — 핸들러가 도는 동안 온 같은 벡터의 인터럽트를 잃지 않기 위해서다 */
		if (generic_handle_domain_irq(pcie->msi_inner_domain, msi_idx) == -EINVAL)	/* [한국어] 그 다음 해당 가상 IRQ 를 부른다 */
			dev_err_ratelimited(&pcie->pdev->dev, "unexpected MSI 0x%02x\n", msi_idx);	/* [한국어] 매핑이 없으면 어느 벡터였는지 남긴다 */
	}

	advk_writel(pcie, PCIE_ISR0_MSI_INT_PENDING,	/* [한국어] **마지막으로 ISR0 의 MSI 대기 비트를 지운다** */
		    PCIE_ISR0_REG);	/* [한국어] 대상 레지스터 */
}

/* [한국어]
 * advk_pcie_handle_int - 하나로 들어온 인터럽트를 종류별로 나눠 보낸다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * **이 컨트롤러는 GIC 에 요약 인터럽트 하나만 낸다.** 그래서 무슨 일로
 * 온 것인지 가려 나눠 보내는 일을 이 함수가 한다.
 *
 * 상태 레지스터가 둘이다. ISR0 에 PME·오류·MSI 대기가, ISR1 에 INTx 넷이
 * 들어 있다. 각각 상태와 마스크를 읽어 (상태 & ~마스크 & 전체마스크)로
 * 처리 대상만 남긴다 — 마스크된 것은 소프트웨어가 원치 않는 것이므로
 * 건드리지 않는다.
 *
 * 처리 순서가 넷이다.
 *   1. **PME 를 가장 먼저** 처리한다. 그 자리의 상류 주석대로 PME 요청자
 *      ID 를 놓치지 않기 위해서다.
 *   2. 오류(ERR). 비트를 지우고 루트 포트 도메인의 0번으로 보낸다.
 *      그 자리의 상류 주석대로 Aardvark 는 PCI_ERR_ROOT_AER_IRQ 를
 *      0 으로 돌려주므로 PCIe 인터럽트 0 번을 쓴다 — PME 와 같은 번호다.
 *   3. MSI 대기 비트가 서 있으면 MSI 처리로 넘긴다.
 *   4. INTx 넷을 차례로 본다. 각각 비트를 먼저 지우고 INTx 도메인의
 *      해당 hwirq 로 보낸다. 매핑이 없으면 어느 핀이었는지를 'A'~'D' 로
 *      찍는다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.** 여기서 부르는
 * generic_handle_domain_irq() 가 장치 드라이버의 핸들러까지 이어진다.
 *
 * 호출 체인:
 *   advk_pcie_irq_handler() -> [이 함수] -> advk_pcie_handle_pme()
 *     -> advk_pcie_handle_msi() -> generic_handle_domain_irq()
 */
static void advk_pcie_handle_int(struct advk_pcie *pcie)
{
	u32 isr0_val, isr0_mask, isr0_status;	/* [한국어] ISR0 의 원본·마스크·처리 대상 */
	u32 isr1_val, isr1_mask, isr1_status;	/* [한국어] ISR1 의 원본·마스크·처리 대상 */
	int i;	/* [한국어] INTx 순회 인덱스 */

	isr0_val = advk_readl(pcie, PCIE_ISR0_REG);	/* [한국어] ISR0 상태를 읽고 */
	isr0_mask = advk_readl(pcie, PCIE_ISR0_MASK_REG);	/* [한국어] 그 마스크도 읽어 */
	isr0_status = isr0_val & ((~isr0_mask) & PCIE_ISR0_ALL_MASK);	/* [한국어] **마스크되지 않은 것만 남긴다** */

	isr1_val = advk_readl(pcie, PCIE_ISR1_REG);	/* [한국어] ISR1 상태를 읽고 */
	isr1_mask = advk_readl(pcie, PCIE_ISR1_MASK_REG);	/* [한국어] 그 마스크도 읽어 */
	isr1_status = isr1_val & ((~isr1_mask) & PCIE_ISR1_ALL_MASK);	/* [한국어] 같은 방식으로 처리 대상만 남긴다 */

	/* Process PME interrupt as the first one to do not miss PME requester id */
	if (isr0_status & PCIE_MSG_PM_PME_MASK)	/* [한국어] **PME 를 가장 먼저 본다.** 바로 위 상류 주석대로 요청자 ID 를 놓치지 않기 위해서다 */
		advk_pcie_handle_pme(pcie);	/* [한국어] PME 처리로 넘긴다 */

	/* Process ERR interrupt */
	if (isr0_status & PCIE_ISR0_ERR_MASK) {	/* [한국어] 다음은 오류다 */
		advk_writel(pcie, PCIE_ISR0_ERR_MASK, PCIE_ISR0_REG);	/* [한국어] **먼저 오류 비트를 지운다** */

		/*
		 * Aardvark HW returns zero for PCI_ERR_ROOT_AER_IRQ, so use
		 * PCIe interrupt 0
		 */
		if (generic_handle_domain_irq(pcie->rp_irq_domain, 0) == -EINVAL)	/* [한국어] **루트 포트 도메인의 0번으로 보낸다.** 바로 위 상류 주석대로 Aardvark 는 PCI_ERR_ROOT_AER_IRQ 를 0 으로 돌려주므로 0 번을 쓴다 — PME 와 같은 번호다 */
			dev_err_ratelimited(&pcie->pdev->dev, "unhandled ERR IRQ\n");	/* [한국어] 매핑이 없으면 오류를 남긴다 */
	}

	/* Process MSI interrupts */
	if (isr0_status & PCIE_ISR0_MSI_INT_PENDING)	/* [한국어] 다음은 MSI 대기다 */
		advk_pcie_handle_msi(pcie);	/* [한국어] MSI 처리로 넘긴다 */

	/* Process legacy interrupts */
	for (i = 0; i < PCI_NUM_INTX; i++) {	/* [한국어] 마지막으로 INTx 넷을 본다 */
		if (!(isr1_status & PCIE_ISR1_INTX_ASSERT(i)))	/* [한국어] 세워지지 않은 핀은 */
			continue;	/* [한국어] 건너뛴다 */

		advk_writel(pcie, PCIE_ISR1_INTX_ASSERT(i),	/* [한국어] **먼저 그 비트를 지우고** */
			    PCIE_ISR1_REG);	/* [한국어] 대상 레지스터 */

		if (generic_handle_domain_irq(pcie->irq_domain, i) == -EINVAL)	/* [한국어] 그 다음 INTx 도메인의 해당 hwirq 로 보낸다 */
			dev_err_ratelimited(&pcie->pdev->dev, "unexpected INT%c IRQ\n",	/* [한국어] 매핑이 없으면 */
					    (char)i + 'A');	/* [한국어] 어느 핀이었는지 A~D 로 찍는다 */
	}
}

/* [한국어]
 * advk_pcie_irq_handler - GIC 에서 오는 요약 인터럽트의 최상위 핸들러
 *
 * @irq: 리눅스 IRQ 번호. 쓰지 않는다.
 * @arg: devm_request_irq() 에 넘긴 드라이버 인스턴스.
 * @return: 이 컨트롤러가 낸 인터럽트였으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * probe 가 devm_request_irq() 로 등록한다. 하는 일이 셋이다.
 *
 *   1. **정말 이 컨트롤러가 낸 것인지 확인한다.** 상위 상태 레지스터의
 *      CORE_INT 비트를 보고, 없으면 IRQ_NONE 을 돌려준다 — 인터럽트 선을
 *      공유하는 경우 다른 장치의 것일 수 있기 때문이다.
 *   2. 종류별 처리로 넘긴다.
 *   3. **처리한 뒤 상위 비트를 지운다.** 순서가 중요하다 — 먼저 지우면
 *      처리 도중에 온 새 인터럽트가 사라진다.
 *
 * 실행 컨텍스트: **인터럽트 컨텍스트.** 이 아래로 이어지는 모든 함수가
 * 잠들 수 없다.
 *
 * 호출 체인:
 *   GIC -> IRQ 코어 -> [이 함수] -> advk_pcie_handle_int()
 */
static irqreturn_t advk_pcie_irq_handler(int irq, void *arg)
{
	struct advk_pcie *pcie = arg;	/* [한국어] devm_request_irq() 에 넘긴 드라이버 인스턴스 */
	u32 status;	/* [한국어] 상위 인터럽트 상태 */

	status = advk_readl(pcie, HOST_CTRL_INT_STATUS_REG);	/* [한국어] **상위 상태 레지스터를 읽는다** */
	if (!(status & PCIE_IRQ_CORE_INT))	/* [한국어] **코어 인터럽트 비트가 없으면 우리 것이 아니다** — 인터럽트 선을 공유할 수 있기 때문이다 */
		return IRQ_NONE;	/* [한국어] 다른 핸들러가 처리하도록 넘긴다 */

	advk_pcie_handle_int(pcie);	/* [한국어] 종류별 처리로 넘긴다 */

	/* Clear interrupt */
	advk_writel(pcie, PCIE_IRQ_CORE_INT, HOST_CTRL_INT_STATUS_REG);	/* [한국어] **처리한 뒤 상위 비트를 지운다.** 먼저 지우면 처리 도중에 온 새 인터럽트가 사라진다 */

	return IRQ_HANDLED;	/* [한국어] 우리가 처리했다고 알린다 */
}

/* [한국어]
 * advk_pcie_map_irq - 장치의 INTx 핀을 리눅스 가상 IRQ 로 잇는다
 *
 * @dev:  대상 PCI 장치.
 * @slot: 슬롯 번호. 루트 버스 경로에서는 쓰지 않는다.
 * @pin:  INTx 핀 번호(1=INTA, 2=INTB, 3=INTC, 4=INTD).
 * @return: 가상 IRQ 번호. 실패하면 0.
 *
 * pci_host_bridge 의 map_irq 콜백이다. PCI 코어가 장치의
 * Interrupt Pin 을 읽은 뒤 그것을 실제 IRQ 번호로 바꾸려고 부른다.
 *
 * **루트 버스인지에 따라 갈린다.** 그 자리의 상류 주석이 이유를 밝힌다 —
 * 에뮬레이션된 루트 브리지는 자기만의 irq chip 과 도메인을 가지므로
 * 그쪽으로 매핑한다. 이때 pin 이 1부터 시작하는 반면 hwirq 는 0부터라
 * 1 을 빼 준다.
 *
 * 루트 버스가 아니면 장치 트리의 interrupt-map 을 따라가는 공용
 * 헬퍼에 맡긴다 — 그 경로가 결국 advk_pcie_init_irq_domain() 이 만든
 * INTx 도메인으로 이어진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(PCI 열거).
 *
 * 호출 체인:
 *   PCI 코어 -> bridge->map_irq -> [이 함수] -> irq_create_mapping()
 *     또는 of_irq_parse_and_map_pci()
 */
static int advk_pcie_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct advk_pcie *pcie = dev->bus->sysdata;	/* [한국어] 버스의 sysdata 에서 이 드라이버 인스턴스를 꺼낸다 */

	/*
	 * Emulated root bridge has its own emulated irq chip and irq domain.
	 * Argument pin is the INTx pin (1=INTA, 2=INTB, 3=INTC, 4=INTD) and
	 * hwirq for irq_create_mapping() is indexed from zero.
	 */
	if (pci_is_root_bus(dev->bus))	/* [한국어] **루트 버스이면 에뮬레이션 루트 포트 자신의 인터럽트다** */
		return irq_create_mapping(pcie->rp_irq_domain, pin - 1);	/* [한국어] 바로 위 상류 주석대로 pin 은 1 부터, hwirq 는 0 부터라 1 을 뺀다 */
	else	/* [한국어] 그 아래 장치이면 */
		return of_irq_parse_and_map_pci(dev, slot, pin);	/* [한국어] 장치 트리의 interrupt-map 을 따라가는 공용 헬퍼에 맡긴다 — 그 경로가 INTx 도메인으로 이어진다 */
}

/* [한국어]
 * advk_pcie_disable_phy - PHY 전원을 내리고 해제한다
 *
 * @pcie: 이 드라이버 인스턴스.
 *
 * advk_pcie_enable_phy() 의 짝이며 순서가 반대다. NULL 검사가 없는데,
 * phy_power_off()/phy_exit() 이 NULL 을 조용히 받아들이므로 PHY 가 없는
 * 보드에서도 안전하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe 실패 경로, remove).
 *
 * 호출 체인:
 *   advk_pcie_probe() 실패 경로 / advk_pcie_remove() -> [이 함수]
 *     -> phy_power_off() -> phy_exit()
 */
static void advk_pcie_disable_phy(struct advk_pcie *pcie)
{
	phy_power_off(pcie->phy);	/* [한국어] PHY 전원을 내리고 */
	phy_exit(pcie->phy);	/* [한국어] PHY 를 해제한다. **NULL 검사가 없는데** 두 함수가 NULL 을 조용히 받아들여 PHY 가 없는 보드에서도 안전하다 */
}

/* [한국어]
 * advk_pcie_enable_phy - PHY 를 초기화하고 PCIe 모드로 켠다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공(PHY 가 없는 경우 포함), 실패면 그 단계의 오류 코드.
 *
 * 세 단계다 — phy_init -> phy_set_mode(PCIE) -> phy_power_on.
 * 이 SerDes 는 PCIe 말고 다른 용도로도 쓸 수 있어 모드를 명시해야 한다.
 *
 * **되돌리기가 계단식이 아니다.** 2단계와 3단계 실패 경로가 모두
 * phy_exit() 만 부른다 — 2단계에서는 아직 전원이 안 들어와 있고,
 * 3단계에서는 phy_power_on() 이 실패했으므로 끌 것이 없기 때문이다.
 *
 * PHY 가 없으면(pcie->phy 가 NULL) 곧바로 성공으로 나간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   advk_pcie_setup_phy() -> [이 함수] -> phy_init() -> phy_set_mode()
 *     -> phy_power_on()
 */
static int advk_pcie_enable_phy(struct advk_pcie *pcie)
{
	int ret;	/* [한국어] 하위 호출 결과 */

	if (!pcie->phy)	/* [한국어] PHY 가 없으면 */
		return 0;	/* [한국어] 할 일이 없으므로 성공으로 나간다 */

	ret = phy_init(pcie->phy);	/* [한국어] **1단계 초기화** */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 그 코드를 돌려준다. 아직 되돌릴 것이 없다 */

	ret = phy_set_mode(pcie->phy, PHY_MODE_PCIE);	/* [한국어] **2단계 모드 설정.** 이 SerDes 는 다른 용도로도 쓸 수 있어 PCIe 라고 알려야 한다 */
	if (ret) {	/* [한국어] 실패하면 */
		phy_exit(pcie->phy);	/* [한국어] 1단계만 되돌리고 — 아직 전원이 안 들어와 있다 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	ret = phy_power_on(pcie->phy);	/* [한국어] **3단계 전원 인가** */
	if (ret) {	/* [한국어] 실패하면 */
		phy_exit(pcie->phy);	/* [한국어] 역시 1단계만 되돌린다 — 전원 인가가 실패했으므로 끌 것이 없다 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	return 0;	/* [한국어] 세 단계를 모두 지났으면 성공 */
}

/* [한국어]
 * advk_pcie_setup_phy - 장치 트리에서 PHY 를 얻어 켠다. 없어도 계속 진행한다
 *
 * @pcie: 이 드라이버 인스턴스.
 * @return: 0 이면 성공(PHY 가 없는 경우 포함), PHY 초기화가 실패하면
 *          그 코드. probe 가 미뤄져야 하면 -EPROBE_DEFER.
 *
 * **세 갈래로 나뉜다.**
 *   - -EPROBE_DEFER 이면 PHY 드라이버가 아직 준비되지 않은 것이므로
 *     그대로 돌려주어 probe 를 나중에 다시 하게 한다.
 *   - 그 밖의 오류이면 그 자리의 상류 주석대로 **옛 바인딩에 PHY
 *     핸들이 빠져 있는 경우**로 보고, 경고만 남기고 NULL 로 둔 뒤
 *     성공으로 나간다. 오래된 장치 트리와의 호환을 위한 것이다.
 *   - PHY 를 얻었으면 켠다. 켜기가 실패하면 오류를 남기고 그 코드를
 *     돌려준다 — 이쪽은 진짜 실패다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:
 *   advk_pcie_probe() -> [이 함수] -> devm_of_phy_get()
 *     -> advk_pcie_enable_phy()
 */
static int advk_pcie_setup_phy(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;	/* [한국어] 경고·오류 메시지를 낼 장치 */
	struct device_node *node = dev->of_node;	/* [한국어] PHY phandle 을 찾을 장치 트리 노드 */
	int ret = 0;	/* [한국어] 호출자에게 돌려줄 값 */

	pcie->phy = devm_of_phy_get(dev, node, NULL);	/* [한국어] **장치 트리에서 PHY 를 얻는다** */
	if (IS_ERR(pcie->phy) && (PTR_ERR(pcie->phy) == -EPROBE_DEFER))	/* [한국어] PHY 드라이버가 아직 준비되지 않았으면 */
		return PTR_ERR(pcie->phy);	/* [한국어] 그 코드를 그대로 돌려 probe 를 나중에 다시 하게 한다 */

	/* Old bindings miss the PHY handle */
	if (IS_ERR(pcie->phy)) {	/* [한국어] 그 밖의 오류이면 — 바로 위 상류 주석대로 **옛 바인딩에 PHY 핸들이 빠져 있는 경우**다 */
		dev_warn(dev, "PHY unavailable (%ld)\n", PTR_ERR(pcie->phy));	/* [한국어] 경고만 남기고 */
		pcie->phy = NULL;	/* [한국어] 없는 것으로 두고 */
		return 0;	/* [한국어] 성공으로 나간다. 오래된 장치 트리와의 호환을 위한 것이다 */
	}

	ret = advk_pcie_enable_phy(pcie);	/* [한국어] PHY 를 얻었으면 켠다 */
	if (ret)	/* [한국어] 켜기가 실패하면 */
		dev_err(dev, "Failed to initialize PHY (%d)\n", ret);	/* [한국어] 그 사실을 알린다 — **이쪽은 진짜 실패라 아래에서 그 코드를 돌려준다** */

	return ret;	/* [한국어] 담아 둔 결과를 돌려준다 */
}

/* [한국어]
 * advk_pcie_probe - 진입점. 자원·인터럽트·도메인 셋을 세우고 버스를 연다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 이면 성공, 실패면 그 단계의 오류 코드.
 *
 * 이 파일에서 가장 긴 함수이며 여섯 묶음으로 나뉜다.
 *
 * **1. 호스트 브리지와 드라이버 상태를 함께 잡는다.**
 *   devm_pci_alloc_host_bridge() 가 pci_host_bridge 뒤에 이 드라이버의
 *   struct advk_pcie 를 붙여 한 번에 잡아 준다. 그래서
 *   pci_host_bridge_priv()/pci_host_bridge_from_priv() 로 서로를 오갈 수
 *   있다.
 *
 * **2. 장치 트리의 ranges 를 아웃바운드 창으로 옮긴다.** 이 부분이
 *   probe 의 절반을 차지한다. 세 상류 주석이 규칙을 밝힌다.
 *     - config 용 창은 만들지 않는다. 하드웨어가 지원하기는 하지만 이
 *       드라이버는 config 를 PIO 로만 처리하므로 창이 필요 없다.
 *     - **오프셋이 0 인 메모리 자원은 건너뛴다** — 기본 아웃바운드 설정이
 *       투명한 주소 변환을 해 주므로 창을 잡을 이유가 없다.
 *     - 창 하나는 (match, remap, mask) 세 값으로 정해지고 주소 A 가
 *       마스크 아래에서 match 와 같으면 그 창을 쓴다. 그래서 **창 크기가
 *       2의 거듭제곱이어야 하고 시작 주소가 그 크기에 정렬되어야 한다.**
 *       마스크의 하위 16비트가 0 이어야 해서 최소 크기가 64KiB 이고,
 *       remap 주소는 마스크에 든 비트만 세울 수 있다.
 *   안쪽 while 루프가 그 제약을 만족하는 **가장 큰 정렬된 창**을 잘라
 *   내며 자원 하나를 여러 창으로 나눈다. 크기의 최상위 비트와 시작
 *   주소의 최하위 비트 중 작은 쪽이 그 크기다. 창을 다 쓰거나 조건을
 *   만족 못 해 남는 부분이 있으면 잘못된 영역이라고 알리고 실패한다.
 *   I/O 자원은 pci_pio_to_address() 로 물리 주소로 바꿔 담는다.
 *
 * **3. 레지스터 창과 인터럽트를 얻는다.** 인터럽트는
 *   IRQF_SHARED | IRQF_NO_THREAD 로 등록한다 — 선을 공유할 수 있고,
 *   스레드로 미루지 않고 하드 인터럽트 컨텍스트에서 처리한다.
 *
 * **4. PERST# GPIO 와 링크 세대를 정한다.** GPIOD_OUT_LOW 로 잡아
 *   처음에는 리셋을 걸지 않은 상태로 둔다. 장치 트리의 max-link-speed 가
 *   1~3 이 아니면 **기본값 3(Gen3)** 을 쓴다 — imx6 가 기본값을 Gen1 로
 *   둔 것과 반대다.
 *
 * **5. PHY 를 켜고 하드웨어를 초기화하고 브리지를 흉내 낸다.**
 *   setup_hw 안에서 링크 훈련까지 끝난다.
 *
 * **6. 인터럽트 도메인 셋을 순서대로 만들고 버스를 연다.**
 *   INTx -> MSI -> 루트 포트 순이며, **되돌리기가 계단식이다** — 뒤쪽이
 *   실패하면 앞서 만든 것을 역순으로 없앤다. devm_ 으로 자동 정리되지
 *   않는 자원이라 손으로 되돌려야 한다. 마지막으로 bridge 에 sysdata,
 *   ops, map_irq 를 매달고 pci_host_probe() 로 열거를 시작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. setup_hw 의 링크 훈련에서 1초 가까이
 * 잠든다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 -> [이 함수] -> devm_pci_alloc_host_bridge()
 *     -> advk_pcie_setup_phy() -> advk_pcie_setup_hw()
 *     -> advk_sw_pci_bridge_init() -> advk_pcie_init_irq_domain()
 *     -> advk_pcie_init_msi_irq_domain() -> advk_pcie_init_rp_irq_domain()
 *     -> pci_host_probe()
 */
static int advk_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;	/* [한국어] 플랫폼 장치의 device 구조체 */
	struct advk_pcie *pcie;	/* [한국어] 이 드라이버의 인스턴스 상태 */
	struct pci_host_bridge *bridge;	/* [한국어] PCI 코어 쪽 호스트 브리지 */
	struct resource_entry *entry;	/* [한국어] 장치 트리 ranges 순회용 항목 */
	int ret, irq;	/* [한국어] ret 은 하위 호출 결과, irq 는 요약 인터럽트 번호 */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(struct advk_pcie));	/* [한국어] **호스트 브리지 뒤에 이 드라이버 상태를 붙여 한 번에 잡는다.** 그래서 둘을 서로 오갈 수 있다 */
	if (!bridge)	/* [한국어] 못 잡으면 */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 돌아간다 */

	pcie = pci_host_bridge_priv(bridge);	/* [한국어] 브리지 뒤에 붙은 이 드라이버 상태의 위치를 얻는다 */
	pcie->pdev = pdev;	/* [한국어] 플랫폼 장치를 담아 두고 */
	platform_set_drvdata(pdev, pcie);	/* [한국어] 인스턴스를 플랫폼 장치에 심는다 — remove 가 이 값을 꺼낸다 */

	resource_list_for_each_entry(entry, &bridge->windows) {	/* [한국어] **장치 트리의 ranges 를 아웃바운드 창으로 옮긴다.** probe 의 절반을 차지하는 부분이다 */
		resource_size_t start = entry->res->start;	/* [한국어] 이 자원의 시작 주소 */
		resource_size_t size = resource_size(entry->res);	/* [한국어] 그 크기 */
		unsigned long type = resource_type(entry->res);	/* [한국어] 종류(메모리 또는 I/O) */
		u64 win_size;	/* [한국어] 잘라 낼 창 하나의 크기 */

		/*
		 * Aardvark hardware allows to configure also PCIe window
		 * for config type 0 and type 1 mapping, but driver uses
		 * only PIO for issuing configuration transfers which does
		 * not use PCIe window configuration.
		 */
		if (type != IORESOURCE_MEM && type != IORESOURCE_IO)	/* [한국어] **메모리도 I/O 도 아니면 건너뛴다.** 바로 위 상류 주석대로 하드웨어는 config 용 창도 지원하지만 이 드라이버는 config 를 PIO 로만 처리한다 */
			continue;	/* [한국어] 다음 자원으로 */

		/*
		 * Skip transparent memory resources. Default outbound access
		 * configuration is set to transparent memory access so it
		 * does not need window configuration.
		 */
		if (type == IORESOURCE_MEM && entry->offset == 0)	/* [한국어] **오프셋이 0 인 메모리는 건너뛴다.** 바로 위 상류 주석대로 기본 아웃바운드 설정이 투명한 변환을 해 주므로 창이 필요 없다 */
			continue;	/* [한국어] 다음 자원으로 */

		/*
		 * The n-th PCIe window is configured by tuple (match, remap, mask)
		 * and an access to address A uses this window if A matches the
		 * match with given mask.
		 * So every PCIe window size must be a power of two and every start
		 * address must be aligned to window size. Minimal size is 64 KiB
		 * because lower 16 bits of mask must be zero. Remapped address
		 * may have set only bits from the mask.
		 */
		while (pcie->wins_count < OB_WIN_COUNT && size > 0) {	/* [한국어] **창이 남아 있고 아직 덮지 못한 크기가 있으면 계속 자른다** */
			/* Calculate the largest aligned window size */
			win_size = (1ULL << (fls64(size)-1)) |	/* [한국어] **가장 큰 정렬된 창 크기를 구한다** — 크기의 최상위 비트와 */
				   (start ? (1ULL << __ffs64(start)) : 0);	/* [한국어] 시작 주소의 최하위 비트를 함께 놓고 */
			win_size = 1ULL << __ffs64(win_size);	/* [한국어] 그중 작은 쪽(최하위 세워진 비트)을 고른다. 그래야 2의 거듭제곱이면서 시작 주소에 정렬된다 */
			if (win_size < 0x10000)	/* [한국어] **64KiB 보다 작으면 만들 수 없다** — 바로 위 상류 주석대로 마스크의 하위 16비트가 0 이어야 하기 때문이다 */
				break;	/* [한국어] 자르기를 멈춘다 */

			dev_dbg(dev,	/* [한국어] 어떤 창을 만드는지 남긴다 */
				"Configuring PCIe window %d: [0x%llx-0x%llx] as %lu\n",	/* [한국어] 창 번호와 구간 */
				pcie->wins_count, (unsigned long long)start,	/* [한국어] 창 번호와 시작 */
				(unsigned long long)start + win_size, type);	/* [한국어] 끝과 종류 */

			if (type == IORESOURCE_IO) {	/* [한국어] I/O 자원이면 */
				pcie->wins[pcie->wins_count].actions = OB_WIN_TYPE_IO;	/* [한국어] 종류를 I/O 로 두고 */
				pcie->wins[pcie->wins_count].match = pci_pio_to_address(start);	/* [한국어] **I/O 포트 번호를 물리 주소로 바꿔 담는다** */
			} else {	/* [한국어] 메모리 자원이면 */
				pcie->wins[pcie->wins_count].actions = OB_WIN_TYPE_MEM;	/* [한국어] 종류를 메모리로 두고 */
				pcie->wins[pcie->wins_count].match = start;	/* [한국어] 시작 주소를 그대로 담는다 */
			}
			pcie->wins[pcie->wins_count].remap = start - entry->offset;	/* [한국어] **PCIe 쪽 주소는 CPU 주소에서 자원의 오프셋을 뺀 값이다** */
			pcie->wins[pcie->wins_count].mask = ~(win_size - 1);	/* [한국어] 마스크는 창 크기의 보수다 */

			if (pcie->wins[pcie->wins_count].remap & (win_size - 1))	/* [한국어] **remap 주소가 창 크기에 정렬되어 있지 않으면** 바로 위 상류 주석대로 마스크에 든 비트만 세울 수 있다는 제약을 어긴다 */
				break;	/* [한국어] 자르기를 멈춘다 */

			start += win_size;	/* [한국어] 덮은 만큼 시작을 밀고 */
			size -= win_size;	/* [한국어] 남은 크기를 줄이고 */
			pcie->wins_count++;	/* [한국어] 창을 하나 썼다고 센다 */
		}

		if (size > 0) {	/* [한국어] **덮지 못하고 남은 부분이 있으면** 창이 모자라거나 제약을 못 맞춘 것이다 */
			dev_err(&pcie->pdev->dev,	/* [한국어] 그 사실을 알리고 */
				"Invalid PCIe region [0x%llx-0x%llx]\n",	/* [한국어] 문제가 된 구간을 */
				(unsigned long long)entry->res->start,	/* [한국어] 시작과 */
				(unsigned long long)entry->res->end + 1);	/* [한국어] 끝으로 찍은 뒤 */
			return -EINVAL;	/* [한국어] 인자 오류로 돌아간다 */
		}
	}

	pcie->base = devm_platform_ioremap_resource(pdev, 0);	/* [한국어] **컨트롤러 레지스터 창을 매핑한다** */
	if (IS_ERR(pcie->base))	/* [한국어] 실패하면 */
		return PTR_ERR(pcie->base);	/* [한국어] 그 오류를 돌려준다 */

	irq = platform_get_irq(pdev, 0);	/* [한국어] **요약 인터럽트 번호를 얻는다** — 이 컨트롤러는 하나만 낸다 */
	if (irq < 0)	/* [한국어] 없으면 */
		return irq;	/* [한국어] 그 오류를 돌려준다 */

	ret = devm_request_irq(dev, irq, advk_pcie_irq_handler,	/* [한국어] **핸들러를 등록한다** */
			       IRQF_SHARED | IRQF_NO_THREAD, "advk-pcie",	/* [한국어] **선을 공유할 수 있고, 스레드로 미루지 않고 하드 인터럽트 컨텍스트에서 처리한다** */
			       pcie);	/* [한국어] 핸들러에 넘길 인스턴스 */
	if (ret) {	/* [한국어] 등록에 실패하면 */
		dev_err(dev, "Failed to register interrupt\n");	/* [한국어] 그 사실을 알리고 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	pcie->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);	/* [한국어] **PERST# GPIO 를 얻는다.** GPIOD_OUT_LOW 라 처음에는 리셋을 걸지 않은 상태다 */
	ret = PTR_ERR_OR_ZERO(pcie->reset_gpio);	/* [한국어] 오류인지 확인한다. 없는 것(NULL)은 오류가 아니다 */
	if (ret) {	/* [한국어] 오류이면 */
		if (ret != -EPROBE_DEFER)	/* [한국어] probe 를 미루는 경우가 아닐 때만 */
			dev_err(dev, "Failed to get reset-gpio: %i\n", ret);	/* [한국어] 그 사실을 알린다 — 미루는 것은 정상 흐름이라 로그를 남기지 않는다 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	ret = gpiod_set_consumer_name(pcie->reset_gpio, "pcie1-reset");	/* [한국어] 디버깅에서 알아보기 쉽도록 이름을 붙인다 */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to set reset gpio name: %d\n", ret);	/* [한국어] 그 사실을 알리고 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	ret = of_pci_get_max_link_speed(dev->of_node);	/* [한국어] **장치 트리의 max-link-speed 를 읽는다** */
	if (ret <= 0 || ret > 3)	/* [한국어] 1~3 이 아니면(없거나 이상한 값이면) */
		pcie->link_gen = 3;	/* [한국어] **기본값 Gen3 을 쓴다** — pci-imx6.c 가 같은 상황에서 Gen1 을 기본으로 두는 것과 반대다 */
	else	/* [한국어] 유효한 값이면 */
		pcie->link_gen = ret;	/* [한국어] 그대로 쓴다 */

	ret = advk_pcie_setup_phy(pcie);	/* [한국어] **PHY 를 켠다.** 없어도 계속 진행하지만 켜기가 실패하면 접는다 */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */

	advk_pcie_setup_hw(pcie);	/* [한국어] **하드웨어를 초기화한다.** 이 안에서 링크 훈련까지 끝난다 */

	ret = advk_sw_pci_bridge_init(pcie);	/* [한국어] **루트 포트를 소프트웨어로 흉내 낸다.** 이것이 있어야 PCI 코어가 열거를 시작할 수 있다 */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to register emulated root PCI bridge\n");	/* [한국어] 그 사실을 알리고 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	ret = advk_pcie_init_irq_domain(pcie);	/* [한국어] **INTx 도메인을 먼저 만든다** */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to initialize irq\n");	/* [한국어] 그 사실을 알리고 */
		return ret;	/* [한국어] 그 코드를 돌려준다. 아직 되돌릴 도메인이 없다 */
	}

	ret = advk_pcie_init_msi_irq_domain(pcie);	/* [한국어] **그 다음 MSI 도메인** */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to initialize irq\n");	/* [한국어] 그 사실을 알리고 */
		advk_pcie_remove_irq_domain(pcie);	/* [한국어] **앞서 만든 INTx 도메인을 되돌린 뒤** */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	ret = advk_pcie_init_rp_irq_domain(pcie);	/* [한국어] **마지막으로 루트 포트 도메인** */
	if (ret) {	/* [한국어] 실패하면 */
		dev_err(dev, "Failed to initialize irq\n");	/* [한국어] 그 사실을 알리고 */
		advk_pcie_remove_msi_irq_domain(pcie);	/* [한국어] MSI 도메인과 */
		advk_pcie_remove_irq_domain(pcie);	/* [한국어] INTx 도메인을 역순으로 되돌린 뒤 */
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	bridge->sysdata = pcie;	/* [한국어] **config 접근에서 되찾을 수 있게 인스턴스를 담는다** — rd_conf/wr_conf 가 bus->sysdata 로 꺼낸다 */
	bridge->ops = &advk_pcie_ops;	/* [한국어] PIO 기반 config 읽기·쓰기를 매단다 */
	bridge->map_irq = advk_pcie_map_irq;	/* [한국어] INTx 핀을 가상 IRQ 로 잇는 함수를 매단다 */

	ret = pci_host_probe(bridge);	/* [한국어] **버스를 열고 장치를 열거한다.** 이 호출 안에서 rd_conf 가 수없이 불린다 */
	if (ret < 0) {	/* [한국어] 실패하면 */
		advk_pcie_remove_rp_irq_domain(pcie);	/* [한국어] 도메인 셋을 만든 역순으로 */
		advk_pcie_remove_msi_irq_domain(pcie);	/* [한국어] 모두 되돌린 뒤 */
		advk_pcie_remove_irq_domain(pcie);
		return ret;	/* [한국어] 그 코드를 돌려준다 */
	}

	return 0;	/* [한국어] 모든 단계를 지났으면 성공 */
}

/* [한국어]
 * advk_pcie_remove - 버스를 걷어내고 하드웨어를 조용한 상태로 되돌린다
 *
 * @pdev: 플랫폼 장치.
 *
 * probe 가 세운 것을 역순으로 허물되, **하드웨어를 끄는 순서가
 * 꼼꼼하다.** 남겨 두면 사라진 드라이버를 향해 인터럽트가 오거나 다음
 * 부팅이 예상 밖 상태를 만나기 때문이다.
 *
 *   1. **버스와 그 아래 장치를 모두 걷어낸다.** pci_lock_rescan_remove()
 *      로 PCI 코어의 장치 추가·제거와 직렬화한다.
 *   2. 루트 브리지의 I/O·메모리 공간과 버스 마스터링을 끈다 — setup_hw 가
 *      처음에 해 둔 상태로 되돌리는 것이다.
 *   3. MSI 를 끄고 **MSI 목적지 주소를 0 으로 지운다.** 그 주소가 이
 *      드라이버 구조체의 물리 주소였으므로, 지우지 않으면 해제된 메모리로
 *      쓰기가 갈 수 있다.
 *   4. **모든 인터럽트를 마스크한 뒤 지운다.** 순서가 중요하다 — 지우고
 *      마스크하면 그 사이에 온 인터럽트가 남는다.
 *   5. 인터럽트 도메인 셋을 만든 역순으로 없애고 에뮬레이션 브리지의
 *      config 공간을 놓아 준다.
 *   6. **PERST# 를 어서트한다.** 상류 주석대로 카드를 전원 차단에
 *      대비시키는 것이다.
 *   7. 링크 훈련을 끄고 아웃바운드 창을 모두 끈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 해제 또는 장치 언바인드).
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 -> [이 함수] -> pci_stop_root_bus()
 *     -> advk_pcie_remove_*_irq_domain() -> pci_bridge_emul_cleanup()
 *     -> advk_pcie_disable_ob_win()
 */
static void advk_pcie_remove(struct platform_device *pdev)
{
	struct advk_pcie *pcie = platform_get_drvdata(pdev);	/* [한국어] 플랫폼 장치에 심어 둔 이 드라이버 인스턴스 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);	/* [한국어] 그 인스턴스를 품고 있는 호스트 브리지를 되찾는다 */
	u32 val;	/* [한국어] 읽고 고칠 레지스터 값 */
	int i;	/* [한국어] 창 순회 인덱스 */

	/* Remove PCI bus with all devices */
	pci_lock_rescan_remove();	/* [한국어] **PCI 코어의 장치 추가·제거와 직렬화한다** */
	pci_stop_root_bus(bridge->bus);	/* [한국어] 루트 버스 아래 장치들을 멈추고 */
	pci_remove_root_bus(bridge->bus);	/* [한국어] 버스를 걷어낸다 */
	pci_unlock_rescan_remove();	/* [한국어] 잠금을 푼다 */

	/* Disable Root Bridge I/O space, memory space and bus mastering */
	val = advk_readl(pcie, PCIE_CORE_CMD_STATUS_REG);	/* [한국어] 명령·상태 레지스터를 읽어 */
	val &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);	/* [한국어] **I/O·메모리 디코딩과 버스 마스터링을 끈다** — setup_hw 가 처음에 해 둔 상태로 되돌린다 */
	advk_writel(pcie, val, PCIE_CORE_CMD_STATUS_REG);	/* [한국어] 되쓴다 */

	/* Disable MSI */
	val = advk_readl(pcie, PCIE_CORE_CTRL2_REG);	/* [한국어] 제어 2 를 읽어 */
	val &= ~PCIE_CORE_CTRL2_MSI_ENABLE;	/* [한국어] MSI 를 끄고 */
	advk_writel(pcie, val, PCIE_CORE_CTRL2_REG);	/* [한국어] 되쓴다 */

	/* Clear MSI address */
	advk_writel(pcie, 0, PCIE_MSI_ADDR_LOW_REG);	/* [한국어] **MSI 목적지 주소를 지운다.** 그 주소가 이 드라이버 구조체의 물리 주소였으므로, 남겨 두면 해제된 메모리로 쓰기가 갈 수 있다 */
	advk_writel(pcie, 0, PCIE_MSI_ADDR_HIGH_REG);	/* [한국어] 상위 워드도 지운다 */

	/* Mask all interrupts */
	advk_writel(pcie, PCIE_MSI_ALL_MASK, PCIE_MSI_MASK_REG);	/* [한국어] **먼저 전부 마스크한다** — MSI */
	advk_writel(pcie, PCIE_ISR0_ALL_MASK, PCIE_ISR0_MASK_REG);	/* [한국어] ISR0 */
	advk_writel(pcie, PCIE_ISR1_ALL_MASK, PCIE_ISR1_MASK_REG);	/* [한국어] ISR1 */
	advk_writel(pcie, PCIE_IRQ_ALL_MASK, HOST_CTRL_INT_MASK_REG);	/* [한국어] 상위까지 */

	/* Clear all interrupts */
	advk_writel(pcie, PCIE_MSI_ALL_MASK, PCIE_MSI_STATUS_REG);	/* [한국어] **그 다음 남아 있던 상태를 지운다.** 순서가 반대이면 그 사이에 온 인터럽트가 남는다 — MSI */
	advk_writel(pcie, PCIE_ISR0_ALL_MASK, PCIE_ISR0_REG);	/* [한국어] ISR0 */
	advk_writel(pcie, PCIE_ISR1_ALL_MASK, PCIE_ISR1_REG);	/* [한국어] ISR1 */
	advk_writel(pcie, PCIE_IRQ_ALL_MASK, HOST_CTRL_INT_STATUS_REG);	/* [한국어] 상위까지 */

	/* Remove IRQ domains */
	advk_pcie_remove_rp_irq_domain(pcie);	/* [한국어] **도메인을 만든 역순으로 없앤다** — 루트 포트 */
	advk_pcie_remove_msi_irq_domain(pcie);	/* [한국어] MSI */
	advk_pcie_remove_irq_domain(pcie);	/* [한국어] INTx */

	/* Free config space for emulated root bridge */
	pci_bridge_emul_cleanup(&pcie->bridge);	/* [한국어] 에뮬레이션 브리지의 config 공간을 놓아 준다 */

	/* Assert PERST# signal which prepares PCIe card for power down */
	if (pcie->reset_gpio)	/* [한국어] 리셋 GPIO 가 있으면 */
		gpiod_set_value_cansleep(pcie->reset_gpio, 1);	/* [한국어] **PERST# 를 어서트한다.** 바로 위 상류 주석대로 카드를 전원 차단에 대비시키는 것이다 */

	/* Disable link training */
	val = advk_readl(pcie, PCIE_CORE_CTRL0_REG);	/* [한국어] 제어 0 을 읽어 */
	val &= ~LINK_TRAINING_EN;	/* [한국어] 링크 훈련을 끄고 */
	advk_writel(pcie, val, PCIE_CORE_CTRL0_REG);	/* [한국어] 되쓴다 */

	/* Disable outbound address windows mapping */
	for (i = 0; i < OB_WIN_COUNT; i++)	/* [한국어] **아웃바운드 창을 모두 끈다** — 남겨 두면 다음 드라이버나 부팅이 예상 밖 매핑을 보게 된다 */
		advk_pcie_disable_ob_win(pcie, i);	/* [한국어] 하나씩 지운다 */

	/* Disable phy */
	advk_pcie_disable_phy(pcie);	/* [한국어] 마지막으로 PHY 전원을 내리고 해제한다 */
}

static const struct of_device_id advk_pcie_of_match_table[] = {	/* [한국어] **장치 트리의 compatible 문자열 표.** 이 드라이버가 맡는 칩은 하나뿐이다 */
	{ .compatible = "marvell,armada-3700-pcie", },	/* [한국어] Armada 3700 의 PCIe 컨트롤러 */
	{},	/* [한국어] 표의 끝을 알리는 빈 항목 */
};
MODULE_DEVICE_TABLE(of, advk_pcie_of_match_table);	/* [한국어] 모듈 자동 적재를 위해 이 표를 모듈 정보에 심는다 */

static struct platform_driver advk_pcie_driver = {	/* [한국어] **플랫폼 드라이버 등록 구조체** */
	.driver = {	/* [한국어] 드라이버 코어 쪽 정보 */
		.name = "advk-pcie",	/* [한국어] 드라이버 이름 */
		.of_match_table = advk_pcie_of_match_table,	/* [한국어] 위 compatible 표 */
	},
	.probe = advk_pcie_probe,	/* [한국어] 진입점 */
	.remove = advk_pcie_remove,	/* [한국어] 언바인드·모듈 해제 시 정리 */
};
module_platform_driver(advk_pcie_driver);	/* [한국어] module_init/exit 을 한 줄로 만들어 준다 */

MODULE_DESCRIPTION("Aardvark PCIe controller");	/* [한국어] 모듈 설명 */
MODULE_LICENSE("GPL v2");	/* [한국어] 라이선스 표시 */
