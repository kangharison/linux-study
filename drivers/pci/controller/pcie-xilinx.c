// SPDX-License-Identifier: GPL-2.0+
/*
 * PCIe host controller driver for Xilinx AXI PCIe Bridge
 *
 * Copyright (c) 2012 - 2014 Xilinx, Inc.
 *
 * Based on the Tegra PCIe driver
 *
 * Bits taken from Synopsys DesignWare Host controller driver and
 * ARM PCI Host generic driver.
 */

/*
 * [한국어 설명] Xilinx AXI PCIe Bridge 의 루트 포트 드라이버 —
 * Xilinx 계열 넷 중 가장 오래된 세대 (pcie-xilinx.c)
 *
 * === 파일의 역할 ===
 * Xilinx 의 AXI PCIe Bridge IP(7 시리즈·Zynq-7000 계열의 PL 에 얹는 소프트
 * IP)를 루트 포트로 동작시키는 드라이버다. DesignWare 코어를 쓰지 않고
 * Xilinx 가 컨트롤러를 통째로 구현한 것이라, 설정공간 접근·인터럽트 도메인·
 * 오류 보고가 모두 이 파일 안에 있거나 커널 공용 코드에 직접 이어진다.
 *
 * 이 파일이 맡는 것은 셋이다.
 *   1. 설정공간 접근. 표준 ECAM 배치를 그대로 따르므로 map_bus 하나만 두고
 *      읽기·쓰기는 공용 헬퍼에 맡긴다. 다만 "이 자리에 장치가 있을 수 있는가"
 *      를 가리는 판정(xilinx_pcie_valid_device)만 직접 넣었다.
 *   2. 인터럽트. 도메인을 둘 만든다 — INTx 넷과 MSI 128개. 사건 전체가
 *      IRQ 하나로 올라오고, 핸들러가 IDR 비트를 훑어 종류를 가른 뒤 INTx/MSI 는
 *      FIFO 레지스터(RPIFR1/RPIFR2)를 읽어 번호를 뽑아 도메인으로 넘긴다.
 *   3. 초기화. 링크 상태를 보고하고, 인터럽트를 정리한 뒤 다시 열고,
 *      브리지 활성 비트를 세운다.
 *
 * 레지스터 창이 하나뿐(reg_base)이라는 점이 이 세대의 특징이다. 브리지 자신의
 * 제어 레지스터(0x130~0x15c)와 ECAM 설정공간 창이 같은 매핑을 공유하며,
 * ECAM 오프셋 계산상 그 주소들은 루트 포트 자신의 확장 설정공간 자리에 놓인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시 흐름은 이렇다.
 *
 *   플랫폼 드라이버 코어 → xilinx_pcie_probe()
 *     → devm_pci_alloc_host_bridge()   : 브리지와 이 드라이버의 상태를 함께 잡는다
 *     → mutex_init()                   : MSI 비트맵 보호용
 *     → xilinx_pcie_parse_dt()         : reg 창을 매핑하고 IRQ 핸들러를 건다
 *     → xilinx_pcie_init_port()        : 링크 보고, 인터럽트 정리·활성, 브리지 켜기
 *     → xilinx_pcie_init_irq_domain()  : INTx 도메인 + MSI 부모 도메인, MSI 수신 주소 설정
 *     → pci_host_probe()               : 버스 스캔. 설정공간은 map_bus + 공용 헬퍼
 *
 * 인터럽트는 갈래가 하나로 시작해 셋으로 나뉜다.
 *
 *   상위 GIC → xilinx_pcie_intr_handler()   [일반 핸들러. 연쇄 핸들러가 아니다]
 *     → IDR 과 IMR 을 AND 해 걸린 사건만 남긴다
 *     ├ 오류·링크 사건들 → dev_warn 으로 보고만 한다 (AER 3종은 오류 FIFO 도 비운다)
 *     └ INTX 또는 MSI 비트 → RPIFR1 을 읽어 유효성과 종류를 가리고
 *          ├ MSI 면  RPIFR2 의 하위 16비트가 벡터 번호 → msi_domain
 *          └ INTx 면 RPIFR1 의 비트 28:27 이 INTA~INTD → leg_domain
 *        → generic_handle_domain_irq()
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트다. intr_handler 는 인터럽트
 * 컨텍스트에서 돌며 IRQF_NO_THREAD 로 등록되어 스레드화되지 않는다.
 * MSI 도메인의 alloc/free 만 mutex 를 잡으므로 프로세스 컨텍스트 전용이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. pci_host_probe() 가 버스를 스캔하고, 설정공간 접근은
 *   bridge->ops 로 걸어 둔 xilinx_pcie_ops 가 처리한다. MSI 는 커널의
 *   MSI 상위 계층이 이 파일의 부모 도메인 위에 자식 도메인을 만들어 쓴다.
 * 아래쪽: 없다. 이 드라이버 아래에 다른 컨트롤러 층이 없다.
 * 옆쪽: 같은 벤더의 형제가 셋 더 있다 — pcie-xilinx-nwl.c(Zynq UltraScale+),
 *   pcie-xilinx-dma-pl.c(PL 쪽 DMA 브리지), pcie-xilinx-cpm.c(Versal CPM).
 *   넷 다 IDR/IMR 이라는 같은 짜임의 인터럽트 레지스터를 갖지만, 이 파일만
 *   pcie-xilinx-common.h 를 포함하지 않는다 — 자세한 것은 아래 인터럽트
 *   상수 무리에 붙인 주석을 보라.
 *
 * 데이터 흐름:
 *   DT reg → reg_base → 브리지 레지스터와 ECAM 설정공간 양쪽
 *   DT 첫 자식 노드 → INTx 도메인의 fwnode
 *   IDR & IMR → 사건 종류 → dev_warn 또는 FIFO 읽기
 *   RPIFR1/RPIFR2 → INTx 번호(0~3) 또는 MSI 벡터(0~127) → 도메인
 *   struct xilinx_pcie 의 물리 주소 → MSIBASE1/2 → MSI 쓰기가 도달할 자리
 *
 * 공유 상태: struct xilinx_pcie 하나. msi_map 비트맵만 map_lock(mutex)이
 *   보호하고 나머지는 probe 후 불변이다. 인터럽트 핸들러와 경합하는
 *   레지스터 읽기-수정-쓰기가 없어 스핀락이 필요 없다 — 형제 판인
 *   pcie-xilinx-cpm.c 가 irq_chip 콜백에서 IMR 을 고치느라 raw 스핀락을
 *   두는 것과 대비된다.
 *
 * === 주요 함수/구조체 요약 ===
 * xilinx_pcie_intr_handler()   : 이 파일에서 가장 긴 함수. 사건 스물을 가른다.
 * xilinx_pcie_init_irq_domain(): INTx 도메인과 MSI 부모 도메인을 만들고
 *                                MSI 수신 주소를 하드웨어에 알린다.
 * xilinx_pcie_init_port()      : 링크 보고, 인터럽트 정리·활성, 브리지 켜기.
 * xilinx_pcie_map_bus()        : ECAM 규칙으로 설정공간 주소를 계산한다.
 * xilinx_pcie_valid_device()   : 있을 수 없는 자리를 걸러 유령 장치를 막는다.
 * xilinx_msi_domain_alloc()    : MSI 벡터를 비트맵에서 잡는다.
 * xilinx_compose_msi_msg()     : 장치가 쓸 MSI 주소·데이터를 만든다.
 * struct xilinx_pcie           : 이 드라이버의 상태 전부.
 *
 * === 같은 계열 넷과 견주어 본 이 파일 ===
 * - 설정공간: 이 파일과 dma-pl 판은 map_bus + 공용 헬퍼, cpm 판은 공용 ECAM
 *   ops 를 통째로 가져다 쓴다. 셋 다 직접 구현하지 않는다는 점은 같다.
 * - INTx irq_chip: 이 파일은 dummy_irq_chip 에 handle_simple_irq 라 마스크
 *   기능이 아예 없다. cpm 판은 IDRN_MASK 를 만지는 진짜 irq_chip 을 둔다.
 *   대신 이 파일은 intx_domain_ops 에 xlate 를 채우고(cpm 판에는 없다),
 *   그 점은 nwl 판과 같다.
 * - MSI: 이 파일은 부모 도메인을 직접 만들고 128 벡터를 비트맵으로 관리한다.
 *   cpm 판에는 MSI 코드가 아예 없다.
 * - 핸들러 모양: 이 파일은 IRQF_SHARED 로 건 보통의 인터럽트 핸들러 하나가
 *   사건을 전부 처리한다. cpm 판은 연쇄 핸들러 둘에 사건마다 별도 핸들러를
 *   거는 방식이라, 같은 IDR/IMR 짜임을 두고 구조가 크게 갈린다.
 */

/* [한국어] IRQ_HANDLED/IRQ_NONE 과 devm_request_irq(), IRQF_SHARED 플래그. */
#include <linux/interrupt.h>
/* [한국어] irq_set_chip_and_handler(), handle_simple_irq(), dummy_irq_chip,
 * irq_domain_set_info() 등 irq_chip 계층의 기본 정의. */
#include <linux/irq.h>
/* [한국어] msi_lib_init_dev_msi_info() — MSI 부모 도메인을 만들 때 쓰는 공용
 * 헬퍼다. 같은 계열의 nwl/dma-pl 판도 같은 것을 쓴다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irq_domain_create_linear(), msi_create_parent_irq_domain(),
 * generic_handle_domain_irq(). 이 파일은 도메인을 둘 만든다(INTx, MSI). */
#include <linux/irqdomain.h>
/* [한국어] ALIGN_DOWN(), lower_32_bits()/upper_32_bits() 등 기본 관용구. */
#include <linux/kernel.h>
/* [한국어] __init 표시 계열 헤더. 이 파일이 직접 쓰는 이름은 없다(전수 확인). */
#include <linux/init.h>
/* [한국어] struct msi_msg 와 msi_domain_info, msi_parent_ops. MSI 부모 도메인을
 * 만들려면 필요하다. */
#include <linux/msi.h>
/* [한국어] of_address_to_resource(). DT 의 reg 를 struct resource 로 옮긴다. */
#include <linux/of_address.h>
/* [한국어] pci_irqd_intx_xlate(). INTx 도메인의 xlate 로 쓴다 — 같은 계열
 * cpm 판에는 없는 항목이다. */
#include <linux/of_pci.h>
/* [한국어] of_platform 계열 헤더. 이 파일이 직접 쓰는 이름은 없다. */
#include <linux/of_platform.h>
/* [한국어] irq_of_parse_and_map(). DT 의 인터럽트를 가상 IRQ 번호로 옮긴다. */
#include <linux/of_irq.h>
/* [한국어] PCI_NUM_INTX, pci_is_root_bus(), pci_generic_config_read/write() 등
 * PCI 코어의 공통 타입과 헬퍼. */
#include <linux/pci.h>
/* [한국어] PCIE_ECAM_OFFSET() 매크로. (버스, devfn, 오프셋)을 표준 ECAM 규칙의
 * 바이트 오프셋으로 옮겨 준다. 이 헤더 자체는 이 트리에 없지만, 같은 매크로를
 * drivers/pci/ecam.c 와 여러 컨트롤러 드라이버가 같은 방식으로 쓴다. */
#include <linux/pci-ecam.h>
/* [한국어] struct platform_device 와 builtin_platform_driver() 매크로. */
#include <linux/platform_device.h>

/* [한국어] drivers/pci 내부 선언. 이 파일이 여기서 얻는 것은 없어 보이지만
 * 상류가 포함해 두었다. */
#include "../pci.h"

/* [한국어] 아래 열 개는 브리지 자신의 제어 레지스터 오프셋이다. 모두
 * reg_base 기준이며, ECAM 오프셋 계산으로 보면 버스 0·장치 0 의 확장 설정공간
 * 자리(0x130 이상)에 해당한다 — 즉 하나의 매핑이 브리지 레지스터 창과
 * ECAM 창을 겸한다. */
/* Register definitions */
/* [한국어] Bridge Info Register. ECAM 크기 정보를 담고 있다고 아래 비트
 * 정의가 말하지만, [상류 코드 관찰] 이 이름도 그 비트 매크로 둘도 이 파일
 * 어디에서도 쓰이지 않는다(전수 확인). 원본 스냅숏(1f0e418bb6)에서 확인했으며
 * 코드는 손대지 않았다. */
#define XILINX_PCIE_REG_BIR		0x00000130
/* [한국어] Interrupt Decode Register. 어떤 사건이 걸렸는지 알리는 상태
 * 레지스터이며 write-1-to-clear 다. 같은 계열 넷이 모두 이 짜임을 공유한다. */
#define XILINX_PCIE_REG_IDR		0x00000138
/* [한국어] Interrupt Mask Register. IDR 의 어느 비트를 실제 인터럽트로
 * 내보낼지 정한다. 이 파일에는 이것을 런타임에 고치는 irq_chip 콜백이 없다 —
 * init_port 가 한 번 세워 두면 끝이며, cpm 판이 mask/unmask 콜백으로 이
 * 레지스터를 계속 고치는 것과 대비된다. */
#define XILINX_PCIE_REG_IMR		0x0000013c
/* [한국어] PHY Status/Control Register. 링크 업 비트가 여기 있다. */
#define XILINX_PCIE_REG_PSCR		0x00000144
/* [한국어] Root Port Status/Control Register. 브리지 활성 비트(BEN)가 여기 있다. */
#define XILINX_PCIE_REG_RPSC		0x00000148
/* [한국어] MSI 수신 주소의 상위 32비트를 알리는 레지스터. 장치가 MSI 를 쏠 때
 * 이 주소로 메모리 쓰기를 내보내면 컨트롤러가 가로채 인터럽트로 바꾼다. */
#define XILINX_PCIE_REG_MSIBASE1	0x0000014c
/* [한국어] 그 하위 32비트. 두 레지스터를 init_irq_domain 이 함께 채운다. */
#define XILINX_PCIE_REG_MSIBASE2	0x00000150
/* [한국어] Root Port Error FIFO Read. AER 오류를 낸 요청자의 ID 를 담은
 * FIFO 창구다. */
#define XILINX_PCIE_REG_RPEFR		0x00000154
/* [한국어] Root Port Interrupt FIFO Read Register 1. INTx/MSI 가 올라왔을 때
 * "유효한가", "MSI 인가 INTx 인가", "INTx 면 몇 번인가" 를 담는다. */
#define XILINX_PCIE_REG_RPIFR1		0x00000158
/* [한국어] 같은 FIFO 의 두 번째 레지스터. MSI 일 때 벡터 번호(메시지 데이터)가
 * 하위 16비트에 실린다. */
#define XILINX_PCIE_REG_RPIFR2		0x0000015c

/* [한국어] 아래 인터럽트 상수 무리는 이름 때문에 반드시 짚고 넘어가야 한다.
 *
 * 같은 디렉터리의 pcie-xilinx-common.h 에 **글자 그대로 같은 이름들**이
 * 정의되어 있는데, 값의 형태가 다르다. 그쪽은 비트 "번호"(예: LINK_DOWN 이 0),
 * 이 파일은 비트 "마스크"(예: LINK_DOWN 이 BIT(0))다. 이 파일은 그 헤더를
 * 포함하지 않고 자기 안에서 따로 정의하므로 충돌은 나지 않지만, 두 파일을
 * 나란히 읽을 때 같은 이름을 같은 뜻으로 착각하기 쉽다.
 *
 * 형제 판들과 견주면 이렇게 갈린다.
 *   - pcie-xilinx-cpm.c / pcie-xilinx-dma-pl.c: 공용 헤더를 포함하고, 번호를
 *     IRQ 도메인의 hwirq 로 쓰거나 BIT() 로 감싸 마스크로 만든다.
 *   - 이 파일: 헤더를 포함하지 않고 처음부터 마스크로 정의해, 아래 핸들러가
 *     `status & XILINX_PCIE_INTR_...` 형태로 곧장 검사한다. 도메인 hwirq 로
 *     쓰이는 일이 없으므로 번호 형태가 필요 없다.
 *
 * 담고 있는 사건의 목록도 완전히 같지는 않다.
 *   - 이 파일에만 있는 것: ECRC_ERR(비트 1), STR_ERR(비트 2), MST_ERRP(비트 28).
 *   - 공용 헤더에만 있는 것: CFG_PCIE_TIMEOUT(4), CFG_ERR_POISON(12),
 *     PME_TO_ACK_RCVD(15), PM_PME_RCVD(17), SLV_PCIE_TIMEOUT(28).
 *   - 비트 28 이 특히 갈린다. 이 파일은 "마스터 오류 오염"(MST_ERRP)으로,
 *     공용 헤더는 "슬레이브 PCIe 타임아웃"으로 쓴다. 같은 자리를 세대마다
 *     다른 뜻으로 쓴다는 뜻이다.
 *   - 비트 17 은 이 파일에서 MSI 이고 공용 헤더에서는 MSI 와 PM_PME_RCVD 가
 *     같은 번호를 나눠 쓴다. */
/* Interrupt registers definitions */
/* [한국어] 링크가 끊어졌다(비트 0). */
#define XILINX_PCIE_INTR_LINK_DOWN	BIT(0)
/* [한국어] ECRC(End-to-end CRC) 검사가 실패했다(비트 1).
 * [상류 코드 관찰] 아래 두 마스크 어느 쪽에도 이 비트가 들어 있지 않다.
 * IMR 에는 ENABLE_MASK 만 쓰이므로 이 비트는 결코 활성화되지 않고, 따라서
 * 핸들러의 ECRC 검사는 도달할 수 없다. 자세한 것은 아래 마스크 주석 참조. */
#define XILINX_PCIE_INTR_ECRC_ERR	BIT(1)
/* [한국어] 스트리밍 오류(비트 2). AXI 스트림 인터페이스 쪽 오류로 보이나
 * 이 트리에 근거는 없다. */
#define XILINX_PCIE_INTR_STR_ERR	BIT(2)
/* [한국어] 호스트가 핫리셋을 걸었다(비트 3). */
#define XILINX_PCIE_INTR_HOT_RESET	BIT(3)
/* [한국어] 설정 트랜잭션이 시간 초과됐다(비트 8). 핸들러가 "ECAM access
 * timeout" 으로 보고한다. */
#define XILINX_PCIE_INTR_CFG_TIMEOUT	BIT(8)
/* [한국어] AER correctable 오류(비트 9). 링크가 스스로 복구한 오류다.
 * 아래 셋만 핸들러가 오류 FIFO 도 함께 비운다. */
#define XILINX_PCIE_INTR_CORRECTABLE	BIT(9)
/* [한국어] AER non-fatal 오류(비트 10). 그 트랜잭션은 실패했지만 링크는 살아 있다. */
#define XILINX_PCIE_INTR_NONFATAL	BIT(10)
/* [한국어] AER fatal 오류(비트 11). 링크 자체를 믿을 수 없는 상태다. */
#define XILINX_PCIE_INTR_FATAL		BIT(11)
/* [한국어] 레거시 INTx 가 도착했다(비트 16). 이 비트가 서면 핸들러가
 * RPIFR1 을 읽어 INTA~INTD 중 어느 것인지 가린다. */
#define XILINX_PCIE_INTR_INTX		BIT(16)
/* [한국어] MSI 가 도착했다(비트 17). 위 INTX 와 함께 검사되며, 둘을 가르는
 * 것은 RPIFR1 의 비트 30 이다. */
#define XILINX_PCIE_INTR_MSI		BIT(17)
/* [한국어] 아래 여섯은 컨트롤러가 AXI 슬레이브로 동작할 때 나는 오류다.
 * 지원하지 않는 요청을 받았다(비트 20). */
#define XILINX_PCIE_INTR_SLV_UNSUPP	BIT(20)
/* [한국어] 예상하지 못한 완료를 받았다(비트 21). */
#define XILINX_PCIE_INTR_SLV_UNEXP	BIT(21)
/* [한국어] 완료를 기다리다 시간이 초과됐다(비트 22). */
#define XILINX_PCIE_INTR_SLV_COMPL	BIT(22)
/* [한국어] 오염(poison) 표시가 붙은 응답을 받았다(비트 23). */
#define XILINX_PCIE_INTR_SLV_ERRP	BIT(23)
/* [한국어] 완료자 중단(Completer Abort)을 받았다(비트 24). */
#define XILINX_PCIE_INTR_SLV_CMPABT	BIT(24)
/* [한국어] 허용되지 않는 버스트 형태의 요청을 받았다(비트 25). */
#define XILINX_PCIE_INTR_SLV_ILLBUR	BIT(25)
/* [한국어] 아래 셋은 컨트롤러가 AXI 마스터로 동작할 때 나는 오류다.
 * 주소에 해당하는 대상이 없다(비트 26). */
#define XILINX_PCIE_INTR_MST_DECERR	BIT(26)
/* [한국어] 슬레이브가 오류 응답을 돌려주었다(비트 27). */
#define XILINX_PCIE_INTR_MST_SLVERR	BIT(27)
/* [한국어] 마스터 쪽 오류 오염(비트 28). 공용 헤더가 같은 비트를
 * SLV_PCIE_TIMEOUT 으로 쓰는 자리다 — 위 무리 주석 참조. */
#define XILINX_PCIE_INTR_MST_ERRP	BIT(28)
/* [한국어] 이 드라이버가 아는 사건 전체의 마스크. 값 0x1FF30FED 를 펴 보면
 * 비트 {0,2,3,5,6,7,8,9,10,11,16,17,20~28} 이다.
 *
 * 쓰이는 곳은 init_port 에서 IDR 의 걸린 비트를 지울 때 한 곳뿐이다.
 * IMR 에 쓰이는 것은 아래 ENABLE_MASK 이지 이 값이 아니다.
 *
 * [상류 코드 관찰] 이 마스크에 비트 5·6·7 이 들어 있는데 그 셋에 대응하는
 * 상수가 이 파일에 없다. 반대로 정의된 상수 중 ECRC_ERR(비트 1)만 이
 * 마스크에서 빠져 있다. 원본 스냅숏(1f0e418bb6)에서 값이 이대로임을
 * 확인했으며 코드는 손대지 않았다. */
#define XILINX_PCIE_IMR_ALL_MASK	0x1FF30FED
/* [한국어] 실제로 IMR 에 써 넣어 활성화하는 마스크. 값 0x1FF30F0D 는 위
 * ALL_MASK 에서 비트 5·6·7 만 뺀 것이다 — 즉 이름 없는 그 셋은 "알고는 있지만
 * 켜지는 않는" 사건이 된다.
 *
 * [상류 코드 관찰] ECRC_ERR(비트 1)은 여기에도 없다. IMR 을 고치는 다른 코드가
 * 이 파일에 없으므로(init_port 의 두 쓰기가 전부다) 그 비트는 결코 서지 않고,
 * 핸들러는 status = IDR & IMR 로 걸러진 값만 보므로 ECRC 검사 갈래는 도달할 수
 * 없다. 원본 스냅숏에서 확인했으며 코드는 손대지 않았다. */
#define XILINX_PCIE_IMR_ENABLE_MASK	0x1FF30F0D
/* [한국어] 32비트 전부. init_port 가 ~ 를 붙여 0 으로 만든 뒤 IMR 에 쓴다 —
 * "모든 인터럽트 끄기" 를 이렇게 에둘러 표현한다. 같은 관용구가 cpm 판에도 있다. */
#define XILINX_PCIE_IDR_ALL_MASK	0xFFFFFFFF

/* [한국어] 아래 셋은 오류 FIFO 레지스터의 비트 배치다. */
/* Root Port Error FIFO Read Register definitions */
/* [한국어] FIFO 에 읽을 오류 기록이 있음을 알리는 비트(18). */
#define XILINX_PCIE_RPEFR_ERR_VALID	BIT(18)
/* [한국어] 오류를 낸 요청자의 ID(비트 15:0). BDF 를 담는다. */
#define XILINX_PCIE_RPEFR_REQ_ID	GENMASK(15, 0)
/* [한국어] FIFO 를 비울 때 쓰는 값. 32비트 전부를 써서 지운다. */
#define XILINX_PCIE_RPEFR_ALL_MASK	0xFFFFFFFF

/* [한국어] 아래 다섯은 INTx/MSI 를 가리는 FIFO 레지스터 1 의 비트 배치다.
 * 이 다섯이 이 드라이버의 인터럽트 처리에서 가장 중요한 부분이다. */
/* Root Port Interrupt FIFO Read Register 1 definitions */
/* [한국어] 이 FIFO 항목이 유효한지(비트 31). 서 있지 않으면 핸들러가
 * "FIFO1 read error" 로 보고하고 error 라벨로 뛴다. */
#define XILINX_PCIE_RPIFR1_INTR_VALID	BIT(31)
/* [한국어] MSI 인지 INTx 인지 가르는 비트(30). 서 있으면 MSI 다. */
#define XILINX_PCIE_RPIFR1_MSI_INTR	BIT(30)
/* [한국어] INTx 일 때 어느 선인지 담는 필드(비트 28:27). 두 비트라 0~3,
 * 곧 INTA~INTD 다. */
#define XILINX_PCIE_RPIFR1_INTR_MASK	GENMASK(28, 27)
/* [한국어] FIFO 항목을 비울 때 쓰는 값. 32비트 전부를 써서 지운다. */
#define XILINX_PCIE_RPIFR1_ALL_MASK	0xFFFFFFFF
/* [한국어] 위 INTR_MASK 필드의 시작 비트(27). 핸들러가 마스킹 후 이만큼
 * 오른쪽으로 밀어 0~3 으로 만든다. FIELD_GET 을 쓰지 않고 마스크와 시프트를
 * 따로 둔 형태이며, cpm 판이 FIELD_GET 을 쓰는 것과 대비된다. */
#define XILINX_PCIE_RPIFR1_INTR_SHIFT	27

/* [한국어] 아래 둘은 Bridge Info Register 의 ECAM 크기 필드다.
 * [상류 코드 관찰] 둘 다, 그리고 위 XILINX_PCIE_REG_BIR 도 이 파일 어디에서도
 * 쓰이지 않는다(전수 확인). ECAM 크기를 하드웨어에서 읽어 확인하는 코드가
 * 없다는 뜻이다. 원본 스냅숏에서 확인했으며 코드는 손대지 않았다. */
/* Bridge Info Register definitions */
/* [한국어] ECAM 크기 필드(비트 18:16). */
#define XILINX_PCIE_BIR_ECAM_SZ_MASK	GENMASK(18, 16)
/* [한국어] 그 필드의 시작 비트(16). */
#define XILINX_PCIE_BIR_ECAM_SZ_SHIFT	16

/* [한국어] FIFO 레지스터 2 의 비트 배치. */
/* Root Port Interrupt FIFO Read Register 2 definitions */
/* [한국어] MSI 메시지 데이터(비트 15:0). 장치가 MSI 를 쏠 때 실어 보낸 값이며,
 * 이 드라이버에서는 그것이 곧 MSI 도메인의 hwirq(벡터 번호)다 —
 * xilinx_compose_msi_msg() 가 data 에 hwirq 를 넣어 주기 때문이다. */
#define XILINX_PCIE_RPIFR2_MSG_DATA	GENMASK(15, 0)

/* [한국어] 루트 포트 상태·제어 레지스터의 비트. */
/* Root Port Status/control Register definitions */
/* [한국어] Bridge Enable(비트 0). 이 비트를 세워야 브리지가 트랜잭션을
 * 통과시킨다. init_port 의 마지막 단계다. */
#define XILINX_PCIE_REG_RPSC_BEN	BIT(0)

/* [한국어] PHY 상태·제어 레지스터의 비트. */
/* Phy Status/Control Register definitions */
/* [한국어] 링크 업 비트(11). cpm 판과 같은 자리·같은 뜻이며, 이 계열이
 * 물리/데이터링크를 나눠 보지 않고 비트 하나로 판정한다는 점도 같다. */
#define XILINX_PCIE_REG_PSCR_LNKUP	BIT(11)

/* [한국어] 이 컨트롤러가 지원하는 MSI 벡터 수. */
/* Number of MSI IRQs */
/* [한국어] 128개. 아래 msi_map 비트맵의 크기와 MSI 도메인의 크기가 모두
 * 이 값이며, 벡터 번호가 그대로 MSI 메시지 데이터로 나간다. */
#define XILINX_NUM_MSI_IRQS		128

/**
 * struct xilinx_pcie - PCIe port information
 * @dev: Device pointer
 * @reg_base: IO Mapped Register Base
 * @msi_map: Bitmap of allocated MSIs
 * @map_lock: Mutex protecting the MSI allocation
 * @msi_domain: MSI IRQ domain pointer
 * @leg_domain: Legacy IRQ domain pointer
 * @resources: Bus Resources
 */
/* [한국어] 이 드라이버의 상태 전부. 위 상류 kernel-doc 이 각 필드를 한 줄로
 * 요약했고, 아래에 설정자·읽는 자·값 범위·동기화를 덧붙인다.
 *
 * 따로 할당되지 않는다. devm_pci_alloc_host_bridge() 에 크기를 넘겨 브리지 뒤에
 * 딸려 잡히고 pci_host_bridge_priv() 로 그 자리를 얻으므로, 이 파일에는
 * kzalloc 도 kfree 도 없다.
 *
 * 이 구조체의 **주소 자체**가 하드웨어에 쓰인다는 점이 특이하다. MSI 수신
 * 주소로 이 구조체가 놓인 페이지의 물리 주소를 쓰기 때문이며, 자세한 것은
 * xilinx_compose_msi_msg() 의 주석을 보라. */
struct xilinx_pcie {
	/* [한국어] 이 컨트롤러의 장치. 로그와 자원 조회에 쓴다.
	 * 설정자: xilinx_pcie_probe().
	 * 읽는 자: 거의 모든 함수의 dev_warn/dev_err/dev_info.
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: probe 후 불변. */
	struct device *dev;
	/* [한국어] 유일한 레지스터 창의 가상 주소.
	 * 설정자: xilinx_pcie_parse_dt() 의 devm_pci_remap_cfg_resource().
	 * 읽는 자: pcie_read()/pcie_write() 와 xilinx_pcie_map_bus().
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변.
	 *
	 * 이 창이 두 몫을 겸한다는 점이 요점이다. 낮은 오프셋(0x130~0x15c)은 브리지
	 * 제어 레지스터이고, map_bus 는 같은 기준 주소에 ECAM 오프셋을 더해 설정공간을
	 * 만든다. 설정공간 접근용 API 로 매핑하는 것(devm_pci_remap_cfg_resource)도
	 * 그 때문이다 — 그 헬퍼는 설정공간에 맞는 메모리 속성을 골라 준다. */
	void __iomem *reg_base;
	/* [한국어] MSI 벡터 128개의 사용 여부를 담은 비트맵.
	 * 설정자: xilinx_msi_domain_alloc() 이 bitmap_find_free_region 으로 잡고,
	 *   xilinx_msi_domain_free() 가 bitmap_release_region 으로 놓는다.
	 * 읽는 자: 위 두 함수뿐이다.
	 * 값 범위: 128비트. 0 이면 빈 자리다.
	 * 동기화: 아래 map_lock 이 보호한다. 두 함수 모두 잠금을 잡고 들어간다.
	 *
	 * region 단위(2의 거듭제곱 개수)로 잡고 놓는 것은 여러 벡터를 연속으로
	 * 요구하는 장치를 받기 위해서다. */
	unsigned long msi_map[BITS_TO_LONGS(XILINX_NUM_MSI_IRQS)];
	/* [한국어] 위 msi_map 비트맵을 보호하는 뮤텍스.
	 * 설정자: xilinx_pcie_probe() 의 mutex_init().
	 * 읽는 자: MSI 도메인의 alloc/free 콜백 둘.
	 * 값 범위: struct mutex.
	 * 동기화: 이 필드가 곧 동기화 수단이다.
	 *
	 * 스핀락이 아니라 뮤텍스라는 점이 두 콜백의 실행 컨텍스트를 말해 준다 —
	 * 잠들 수 있는 프로세스 컨텍스트에서만 불린다는 뜻이다. 인터럽트 핸들러는
	 * 이 잠금을 건드리지 않는다. */
	struct mutex map_lock;
	/* [한국어] MSI 부모 IRQ 도메인.
	 * 설정자: xilinx_allocate_msi_domains() 의 msi_create_parent_irq_domain().
	 * 읽는 자: xilinx_pcie_intr_handler() 가 MSI 를 넘길 때,
	 *   xilinx_free_irq_domains() 가 없앨 때.
	 * 값 범위: 유효한 도메인 포인터. CONFIG_PCI_MSI 가 꺼져 있으면 NULL 로 남지만,
	 *   이 드라이버의 Kconfig 가 PCI_MSI 를 depends 로 요구하므로(같은 디렉터리
	 *   Kconfig 에서 확인) 실제로는 언제나 만들어진다.
	 * 동기화: probe 경로에서 만들고 실패 시 없앤다. */
	struct irq_domain *msi_domain;
	/* [한국어] INTx(레거시) IRQ 도메인. 크기가 PCI_NUM_INTX(4)다.
	 * 설정자: xilinx_pcie_init_irq_domain() 의 irq_domain_create_linear().
	 * 읽는 자: xilinx_pcie_intr_handler() 가 INTx 를 넘길 때,
	 *   xilinx_free_irq_domains() 가 없앨 때.
	 * 값 범위: 유효한 도메인 포인터.
	 * 동기화: probe 경로에서 만들고 실패 시 없앤다.
	 *
	 * 이름이 leg_domain(legacy)인 것이 이 파일의 옛 이름 관행이며, 형제 판인
	 * cpm 은 같은 것을 intx_domain 이라 부른다. */
	struct irq_domain *leg_domain;
	/* [한국어] 버스 자원 목록 머리.
	 * 설정자: 없다.
	 * 읽는 자: 없다.
	 * 값 범위: 초기화조차 되지 않는다 — devm 할당의 0 초기화 상태 그대로다.
	 * 동기화: 해당 없음.
	 *
	 * [상류 코드 관찰] 이 필드는 선언과 위 kernel-doc 항목만 있을 뿐 이 파일
	 * 어디에서도 쓰이지 않는다(전수 확인). 자원 목록을 드라이버가 직접 들고
	 * 있던 옛 구조의 잔재로 보이며, 지금은 pci_host_bridge 가 windows 목록을
	 * 관리한다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
	struct list_head resources;
};

/* [한국어]
 * pcie_read - 브리지 레지스터를 읽는다
 *
 * @pcie: 이 드라이버의 상태. reg_base 가 채워져 있어야 한다.
 * @reg: 레지스터 창 안의 오프셋.
 * @return: 읽은 32비트 값.
 *
 * 이 파일이 브리지 레지스터에 닿는 두 통로 중 읽기 쪽이다. 같은 reg_base 를
 * map_bus 도 쓰지만 그쪽은 ECAM 오프셋을 더하므로 겹치지 않는다.
 *
 * 형제 판인 pcie-xilinx-cpm.c 가 같은 이름의 헬퍼를 readl_relaxed 로 두는 것과
 * 달리 이쪽은 배리어가 있는 readl 이다. 인터럽트 핸들러가 IDR 을 읽고 IMR 을
 * 읽은 뒤 IDR 에 되쓰는 순서를 지켜야 해서로 보이나, 그 의도가 코드에 적혀
 * 있지는 않다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다. MMIO 읽기에는 실패라는 개념이 없다.
 *
 * 호출 체인:
 *   xilinx_pcie_link_up() / clear_err_interrupts() / intr_handler() / init_port()
 *     → [이 함수] → readl()
 */
static inline u32 pcie_read(struct xilinx_pcie *pcie, u32 reg)
{
	/* [한국어] 창 시작에 오프셋을 더한 주소를 읽는다. */
	return readl(pcie->reg_base + reg);
}

/* [한국어]
 * pcie_write - 브리지 레지스터에 쓴다
 *
 * @pcie: 이 드라이버의 상태.
 * @val: 쓸 값.
 * @reg: 레지스터 창 안의 오프셋.
 * @return: 없음.
 *
 * pcie_read() 의 짝이다. 인자 순서가 (값, 오프셋)이라 읽기 쪽의 (상태, 오프셋)과
 * 자리가 어긋나는데, 감싸고 있는 writel(값, 주소) 관용구를 따른 것이다.
 * 형제 판 cpm 도 같은 순서를 쓴다.
 *
 * 실행 컨텍스트: probe 경로와 인터럽트 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   clear_err_interrupts() / intr_handler() / init_irq_domain() / init_port()
 *     → [이 함수] → writel()
 */
static inline void pcie_write(struct xilinx_pcie *pcie, u32 val, u32 reg)
{
	/* [한국어] 창 시작에 오프셋을 더한 주소에 쓴다. */
	writel(val, pcie->reg_base + reg);
}

/* [한국어]
 * xilinx_pcie_link_up - 링크가 서 있는지 본다
 *
 * @pcie: 이 드라이버의 상태.
 * @return: true 면 링크가 섰다.
 *
 * PHY 상태 레지스터의 비트 하나만 본다. 이 계열 넷이 모두 그렇듯 물리 계층과
 * 데이터 링크 계층을 나눠 보지 않으며, 그 비트가 어느 계층을 뜻하는지는 이
 * 트리에서 확인 못 함.
 *
 * 쓰이는 곳이 둘이다. init_port 는 로그를 남기는 데만 쓰고,
 * xilinx_pcie_valid_device() 는 링크가 없으면 하위 버스 접근을 아예 막는 데 쓴다.
 * 후자가 이 함수를 단순한 보고용이 아니라 설정공간 접근 경로의 일부로 만든다 —
 * 형제 판 cpm 이 링크 상태를 로그에만 쓰는 것과 다른 점이다.
 *
 * [상류 코드 관찰] 반환 타입이 bool 인데 `? 1 : 0` 삼항 연산이 붙어 있다.
 * bool 로 변환하면 어차피 0/1 로 정규화되므로 이 삼항은 결과를 바꾸지 않는다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe 경로와 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xilinx_pcie_valid_device() / xilinx_pcie_init_port() → [이 함수] → pcie_read()
 */
static inline bool xilinx_pcie_link_up(struct xilinx_pcie *pcie)
{
	/* [한국어] PHY 상태 레지스터를 읽어 링크 업 비트만 남기고, 위 관찰대로
	 * 불필요한 삼항으로 0/1 을 만든다. */
	return (pcie_read(pcie, XILINX_PCIE_REG_PSCR) &
		XILINX_PCIE_REG_PSCR_LNKUP) ? 1 : 0;
}

/**
 * xilinx_pcie_clear_err_interrupts - Clear Error Interrupts
 * @pcie: PCIe port information
 */
/* [한국어] 위 상류 kernel-doc 이 인자를 요약했고, 아래에 배경을 덧붙인다.
 *
 * AER 등급 오류(correctable/non-fatal/fatal) 셋이 올라왔을 때만 불린다.
 * 브리지가 오류를 낸 요청자의 ID 를 FIFO 에 담아 두는데, 그것을 읽어 디버그
 * 로그로 남기고 FIFO 를 비운다.
 *
 * "비운다" 가 곧 "32비트 전부를 되쓴다" 인 것이 이 계열의 관용구다 —
 * write-1-to-clear 라 무엇을 쓰든 세워진 비트가 지워진다. 형제 판인
 * pcie-xilinx-cpm.c 의 cpm_pcie_clear_err_interrupts() 와 본문이 사실상 같다.
 *
 * dev_dbg 라서 기본 빌드에서는 아무것도 출력되지 않는다. 즉 실질적인 효과는
 * FIFO 를 비워 다음 오류가 기록될 자리를 만드는 것이다.
 *
 * [상류 코드 관찰] 유효 비트가 서 있지 않으면 FIFO 를 비우지 않고 그냥
 * 돌아간다. AER 인터럽트가 걸렸는데 FIFO 가 비어 있으면 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(xilinx_pcie_intr_handler 안). 잠들지 않는다.
 * 잠금을 잡지 않는데, 이 레지스터를 만지는 곳이 여기 하나뿐이라 경합할 상대가 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xilinx_pcie_intr_handler() → [이 함수] → pcie_read(), pcie_write()
 */
static void xilinx_pcie_clear_err_interrupts(struct xilinx_pcie *pcie)
{
	/* [한국어] dev_dbg 에 쓸 장치. */
	struct device *dev = pcie->dev;
	/* [한국어] 오류 FIFO 창구를 읽는다. unsigned long 인 것은 아래 dev_dbg 의
	 * %lu 서식과 맞추기 위해서다. */
	unsigned long val = pcie_read(pcie, XILINX_PCIE_REG_RPEFR);

	/* [한국어] 유효 비트가 서 있을 때만 읽을 기록이 있다는 뜻이다. */
	if (val & XILINX_PCIE_RPEFR_ERR_VALID) {
		/* [한국어] 하위 16비트가 오류를 낸 요청자의 ID(BDF)다. 어느 장치가 오류를
		 * 냈는지 가리는 유일한 단서다. */
		dev_dbg(dev, "Requester ID %lu\n",
			val & XILINX_PCIE_RPEFR_REQ_ID);
		/* [한국어] 32비트 전부를 되써 FIFO 를 비운다. */
		pcie_write(pcie, XILINX_PCIE_RPEFR_ALL_MASK,
			   XILINX_PCIE_REG_RPEFR);
	}
}

/**
 * xilinx_pcie_valid_device - Check if a valid device is present on bus
 * @bus: PCI Bus structure
 * @devfn: device/function
 *
 * Return: 'true' on success and 'false' if invalid device is found
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * 설정공간 접근이 하드웨어에 닿기 전에 "이 자리에 장치가 있을 수 있는가" 를
 * 가리는 문지기다. 두 가지를 막는다.
 *
 *   1. 루트 버스에서 devfn 이 0 이 아닌 자리. 옆의 상류 주석대로 루트 포트
 *      아래에는 장치가 하나뿐이므로, 막지 않으면 열거가 같은 설정공간을 여러
 *      장치로 착각해 유령 장치를 만들어 낸다.
 *   2. 링크가 서지 않았을 때의 하위 버스 접근. 링크 없이 나간 설정 사이클은
 *      응답을 받지 못해 타임아웃으로 끝나고, 그것이 CFG_TIMEOUT 인터럽트를
 *      부른다. 미리 막아 그 비용을 피하는 것이다.
 *
 * devfn 을 통째로 견주는 점에 유의할 것 — 슬롯뿐 아니라 기능 번호까지 0 이어야
 * 통과한다. 즉 루트 포트는 단일 기능 장치로만 다뤄진다. 형제 판인
 * pcie-xilinx-cpm.c 나 pci-ixp4xx.c 가 PCI_SLOT() 만 보는 것과 다른 점이다.
 *
 * 실행 컨텍스트: 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다 — 반환값 자체가 판정 결과이며, false 면 map_bus 가 NULL 을
 * 돌려주어 PCI 코어가 그 자리를 없는 것으로 다룬다.
 *
 * 호출 체인:
 *   xilinx_pcie_map_bus() → [이 함수] → xilinx_pcie_link_up()
 */
static bool xilinx_pcie_valid_device(struct pci_bus *bus, unsigned int devfn)
{
	/* [한국어] 브리지에 걸어 둔 sysdata 가 곧 이 드라이버의 상태다. */
	struct xilinx_pcie *pcie = bus->sysdata;

	/* Check if link is up when trying to access downstream pcie ports */
	/* [한국어] 옆의 상류 주석대로 하위 버스로 나가는 접근이면 링크가 필요하다. */
	if (!pci_is_root_bus(bus)) {
		/* [한국어] 링크가 없으면 응답할 상대가 없다. */
		if (!xilinx_pcie_link_up(pcie))
			/* [한국어] 접근을 막아 타임아웃 비용을 피한다. */
			return false;
	/* [한국어] 루트 버스인데 devfn 이 0 이 아니다. */
	} else if (devfn > 0) {
		/* Only one device down on each root port */
		/* [한국어] 옆의 상류 주석대로 루트 포트 아래에는 장치가 하나뿐이므로
		 * 나머지 자리는 없는 것으로 답한다. */
		return false;
	}
	/* [한국어] 루트 버스의 devfn 0 이거나, 링크가 선 상태의 하위 버스다. */
	return true;
}

/**
 * xilinx_pcie_map_bus - Get configuration base
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 *
 * Return: Base address of the configuration space needed to be
 *	   accessed.
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * pci_ops.map_bus 콜백이다. PCI 코어가 설정공간을 읽거나 쓰기 전에 이 함수로
 * "그 주소가 어디냐" 를 묻고, 돌려받은 주소에 pci_generic_config_read/write 가
 * 생 MMIO 접근을 한다.
 *
 * 이 하드웨어의 설정공간이 표준 ECAM 배치를 그대로 따르기 때문에 이 방식이
 * 성립한다. 그래서 이 드라이버는 설정공간 읽기·쓰기를 한 줄도 직접 구현하지
 * 않는다 — 사이드밴드를 켰다 꺼야 해서 read/write 를 직접 구현할 수밖에 없었던
 * pcie-kirin.c 나, 간접 창구를 두드려야 하는 pci-ixp4xx.c 와 대비된다.
 *
 * 기준 주소가 reg_base 라는 점이 이 파일의 특징이다. 브리지 제어 레지스터와
 * 설정공간이 같은 매핑을 공유하며, ECAM 오프셋 계산상 그 레지스터들(0x130~)은
 * 루트 포트 자신의 확장 설정공간 자리에 놓인다.
 *
 * 실행 컨텍스트: 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 있을 수 없는 자리면 NULL 을 돌려준다. 공용 헬퍼가 그것을 보고
 * 읽기에는 전부 1 을 채워 돌려주므로, 열거가 빈 자리로 다룬다.
 *
 * 호출 체인:
 *   PCI 코어 → bus->ops->map_bus → [이 함수]
 *     → xilinx_pcie_valid_device(), PCIE_ECAM_OFFSET()
 */
static void __iomem *xilinx_pcie_map_bus(struct pci_bus *bus,
					 unsigned int devfn, int where)
{
	/* [한국어] sysdata 에서 이 드라이버의 상태를 되찾는다. */
	struct xilinx_pcie *pcie = bus->sysdata;

	/* [한국어] 있을 수 없는 자리인지 먼저 가린다. */
	if (!xilinx_pcie_valid_device(bus, devfn))
		/* [한국어] NULL 을 돌려주면 공용 헬퍼가 그 접근을 없는 자리로 처리한다. */
		return NULL;

	/* [한국어] 표준 ECAM 규칙으로 (버스, devfn, 오프셋)을 바이트 오프셋으로 옮겨
	 * 창 시작에 더한다. iATU 도 간접 창구도 개입하지 않는 가장 단순한 형태다. */
	return pcie->reg_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);
}

/* PCIe operations */
/* [한국어] PCI 코어가 설정공간을 만질 때 쓸 함수 표. map_bus 하나만 이 파일이
 * 구현하고 읽기·쓰기는 공용 헬퍼에 맡긴다 — 설정공간이 표준 ECAM 배치라
 * 주소만 알려 주면 되기 때문이다. */
static struct pci_ops xilinx_pcie_ops = {
	/* [한국어] 접근할 설정공간 주소를 계산해 준다. */
	.map_bus = xilinx_pcie_map_bus,
	/* [한국어] 그 주소에 생 MMIO 읽기를 하는 공용 헬퍼. */
	.read	= pci_generic_config_read,
	/* [한국어] 같은 쓰기 판. */
	.write	= pci_generic_config_write,
};

/* [한국어] 여기부터 MSI 관련 코드다. 이 드라이버는 MSI 부모 도메인을 직접
 * 만들어 벡터 128개를 비트맵으로 관리한다 — 형제 판 pcie-xilinx-cpm.c 에는
 * MSI 코드가 아예 없다는 점과 대비된다. */
/* MSI functions */

/* [한국어]
 * xilinx_msi_top_irq_ack - 상위 MSI irq_chip 의 ack 콜백 (의도적으로 빈 함수)
 *
 * @d: 이 IRQ 의 irq_data. 쓰지 않는다.
 * @return: 없음.
 *
 * 본문이 비어 있고 그 이유를 아래 상류 주석이 밝힌다 — 실제 ack(FIFO 비우기)는
 * 이미 xilinx_pcie_intr_handler() 가 RPIFR1 에 되쓰면서 끝냈기 때문이다.
 * 상류 주석은 그것을 언젠가 고쳐 INTx 와 MSI 각자의 콜백으로 옮겨야 한다고
 * 덧붙인다.
 *
 * 콜백을 아예 비워 두지 않고 빈 함수를 두는 이유가 있다. handle_edge_irq 흐름이
 * ack 를 반드시 부르므로, 콜백이 없으면 상위 계층의 기본 ack 가 불려 엉뚱한
 * 하드웨어를 건드릴 수 있다. 빈 함수가 "여기서는 할 일이 없다" 를 명시한다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(MSI 흐름 핸들러 안). 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 상위 도메인의 흐름 핸들러 → chip->irq_ack → [이 함수]
 */
static void xilinx_msi_top_irq_ack(struct irq_data *d)
{
	/*
	 * xilinx_pcie_intr_handler() will have performed the Ack.
	 * Eventually, this should be fixed and the Ack be moved in
	 * the respective callbacks for INTx and MSI.
	 */
}

/* [한국어]
 * xilinx_compose_msi_msg - 장치가 쓸 MSI 주소와 데이터를 만든다
 *
 * @data: 이 MSI 벡터의 irq_data. hwirq 가 벡터 번호(0~127)다.
 * @msg: 채워 넣을 MSI 메시지. 상위 계층이 이것을 장치의 설정공간에 써 준다.
 * @return: 없음.
 *
 * MSI 는 "정해진 주소에 정해진 값을 메모리 쓰기로 보내면 인터럽트가 된다" 는
 * 방식이다. 그 주소와 값을 정하는 것이 이 콜백이다.
 *
 * 이 드라이버가 고른 주소가 특이하다. 별도로 예약한 페이지가 아니라
 * **이 드라이버의 상태 구조체가 놓인 페이지의 물리 주소** 를 쓴다. 그래서
 * ALIGN_DOWN 으로 4KB 경계까지 내린다. 같은 계산을
 * xilinx_pcie_init_irq_domain() 이 한 번 더 해서 MSIBASE1/2 레지스터에 알려
 * 주므로, 컨트롤러가 그 주소로 오는 쓰기를 가로채 인터럽트로 바꾼다.
 *
 * 즉 그 페이지는 실제로 데이터가 쓰이는 곳이 아니라 컨트롤러가 알아보는
 * 표식일 뿐이다. 다만 그 페이지에 이 드라이버의 상태가 실제로 들어 있다는
 * 점은 짚어 둘 만하다.
 *
 * 데이터는 hwirq 를 그대로 쓴다. 그 값이 나중에 RPIFR2 의 하위 16비트로
 * 돌아오고, 핸들러가 그것을 다시 도메인의 hwirq 로 써서 짝이 맞는다.
 *
 * 실행 컨텍스트: MSI 를 설정하는 프로세스 컨텍스트. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 상위 계층 → chip->irq_compose_msi_msg → [이 함수]
 */
static void xilinx_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	/* [한국어] 도메인 생성 시 넘긴 host_data(= pcie)를 chip_data 에서 되찾는다. */
	struct xilinx_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* [한국어] 이 구조체가 놓인 페이지의 물리 주소를 4KB 경계로 내린다.
	 * init_irq_domain 이 같은 값을 MSIBASE1/2 에 써 두므로 컨트롤러가 이 주소로
	 * 오는 쓰기를 인터럽트로 인식한다. */
	phys_addr_t pa = ALIGN_DOWN(virt_to_phys(pcie), SZ_4K);

	/* [한국어] 주소의 하위 32비트. */
	msg->address_lo = lower_32_bits(pa);
	/* [한국어] 상위 32비트. 32비트 시스템이면 0 이 된다. */
	msg->address_hi = upper_32_bits(pa);
	/* [한국어] 데이터는 벡터 번호 그대로다. 이 값이 RPIFR2 로 돌아와 어느 벡터인지
	 * 가리는 근거가 된다. */
	msg->data = data->hwirq;
}

/* [한국어] MSI 부모 도메인의 하위 irq_chip. compose_msi_msg 하나만 채운다 —
 * 마스크·언마스크가 없는 것은 이 하드웨어가 벡터별 마스킹을 제공하지 않고,
 * 상위 계층이 장치 쪽 MSI 능력으로 그것을 대신하기 때문이다. */
static struct irq_chip xilinx_msi_bottom_chip = {
	/* [한국어] /proc/interrupts 등에 표시될 이름. */
	.name			= "Xilinx MSI",
	/* [한국어] 장치에 써 줄 MSI 주소와 데이터를 만든다. */
	.irq_compose_msi_msg	= xilinx_compose_msi_msg,
};

/* [한국어]
 * xilinx_msi_domain_alloc - MSI 벡터를 비트맵에서 잡는다 (irq_domain_ops.alloc)
 *
 * @domain: MSI 부모 도메인. host_data 에 이 드라이버의 상태가 있다.
 * @virq: 배정할 가상 IRQ 번호의 시작.
 * @nr_irqs: 요청한 벡터 개수.
 * @args: 상위 계층이 넘기는 인자. 이 구현은 쓰지 않는다.
 * @return: 0 성공, -ENOSPC 면 빈 자리가 없다.
 *
 * MSI 는 장치가 여러 벡터를 **연속으로** 요구할 수 있고 그 개수는 2의
 * 거듭제곱이어야 한다. 그래서 단순한 "빈 비트 찾기" 가 아니라 region 단위로
 * 잡는 bitmap_find_free_region() 을 쓰고, 요청 개수를 order_base_2() 로
 * 지수로 바꿔 넘긴다.
 *
 * 잠금 범위가 좁다는 점을 눈여겨볼 것 — 비트맵 조작만 감싸고, 그 뒤의
 * irq_domain_set_info() 반복은 잠금 밖에서 한다. 이미 자리를 확보했으므로
 * 다른 할당자와 겹칠 일이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. mutex 를 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 자리가 없으면 -ENOSPC. 그 경우 아무것도 잡지 않았으므로 되감을
 * 것이 없다.
 *
 * 호출 체인:
 *   MSI 상위 계층(pci_alloc_irq_vectors 등) → domain->ops->alloc → [이 함수]
 *     → bitmap_find_free_region(), irq_domain_set_info()
 */
static int xilinx_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs, void *args)
{
	/* [한국어] 도메인 생성 시 넘긴 host_data 가 이 드라이버의 상태다. */
	struct xilinx_pcie *pcie = domain->host_data;
	/* [한국어] hwirq 는 잡은 벡터의 시작 번호, i 는 반복 커서다. */
	int hwirq, i;

	/* [한국어] 비트맵을 고치는 동안 다른 할당·해제와 겹치지 않게 한다.
	 * 뮤텍스라 이 경로가 잠들 수 있는 문맥 전용임을 못박는다. */
	mutex_lock(&pcie->map_lock);

	/* [한국어] 2의 거듭제곱 크기의 연속 구간을 찾아 잡는다. MSI 규격이 여러 벡터를
	 * 연속으로 요구하고 개수가 2의 거듭제곱이어야 하기 때문이며, order_base_2 가
	 * 개수를 지수로 바꿔 준다. */
	hwirq = bitmap_find_free_region(pcie->msi_map, XILINX_NUM_MSI_IRQS, order_base_2(nr_irqs));

	/* [한국어] 비트맵 조작이 끝났으므로 곧바로 놓는다. */
	mutex_unlock(&pcie->map_lock);

	/* [한국어] 음수면 빈 구간이 없다. */
	if (hwirq < 0)
		/* [한국어] 공간 부족을 알린다. 상위 계층이 벡터 수를 줄여 다시 시도할 수 있다. */
		return -ENOSPC;

	/* [한국어] 잡은 구간의 벡터마다 가상 IRQ 와 hwirq 를 잇는다. */
	for (i = 0; i < nr_irqs; i++)
		/* [한국어] 가상 IRQ 와 hwirq 를 잇고 하위 irq_chip 과 흐름 핸들러를 매단다.
		 * handle_edge_irq 를 고른 것은 MSI 가 에지 성격의 신호이기 때문이다 —
		 * 메모리 쓰기 한 번이 사건 하나이며 계속 유지되는 레벨 신호가 아니다.
		 * host_data 를 chip_data 로 함께 넘겨 compose_msi_msg 가 되찾게 한다. */
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &xilinx_msi_bottom_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL);

	/* [한국어] 요청한 벡터를 모두 배정했다. */
	return 0;
}

/* [한국어]
 * xilinx_msi_domain_free - 잡아 둔 MSI 벡터를 놓는다 (irq_domain_ops.free)
 *
 * @domain: MSI 부모 도메인.
 * @virq: 놓을 가상 IRQ 번호의 시작.
 * @nr_irqs: 놓을 벡터 개수.
 * @return: 없음.
 *
 * alloc 의 거울이다. 가상 IRQ 로는 비트맵 위치를 알 수 없으므로 irq_data 를
 * 거쳐 hwirq 를 되찾고, 그것이 곧 비트맵에서의 자리다.
 *
 * 잡을 때와 같은 region 단위로 놓아야 한다. 그래서 여기서도 order_base_2 로
 * 개수를 지수로 바꿔 넘긴다 — 짝이 맞지 않으면 비트맵이 어긋난다.
 *
 * [상류 코드 관찰] irq_domain_get_irq_data() 의 반환을 NULL 검사 없이 곧바로
 * 역참조한다. 해제 대상 IRQ 는 이 도메인이 만든 것이라 실제로는 NULL 이 될 수
 * 없는 구조이나, 검사가 없는 것은 사실이다. 원본 스냅숏(1f0e418bb6)에서
 * 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. mutex 를 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 상위 계층(pci_free_irq_vectors 등) → domain->ops->free → [이 함수]
 *     → irq_domain_get_irq_data(), bitmap_release_region()
 */
static void xilinx_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				  unsigned int nr_irqs)
{
	/* [한국어] 해제 전에 irq_data 에서 hwirq 를 되찾는다 — 그것이 비트맵에서의
	 * 위치다. 위 [상류 코드 관찰] 대로 NULL 검사가 없다. */
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	/* [한국어] 비트맵을 담고 있는 이 드라이버의 상태. */
	struct xilinx_pcie *pcie = domain->host_data;

	/* [한국어] 잡을 때와 같은 잠금으로 감싼다. */
	mutex_lock(&pcie->map_lock);

	/* [한국어] 잡을 때와 같은 region 단위로 놓는다. 지수가 어긋나면 비트맵이 깨진다. */
	bitmap_release_region(pcie->msi_map, d->hwirq, order_base_2(nr_irqs));

	/* [한국어] 잠금을 푼다. */
	mutex_unlock(&pcie->map_lock);
}

/* [한국어] MSI 부모 도메인의 동작. 할당과 해제 둘뿐이며 map 이 없다 —
 * MSI 는 계층형 도메인이라 매핑이 alloc 안에서 함께 이루어지기 때문이다.
 * INTx 도메인이 map 만 두는 것과 정확히 반대 모양이다. */
static const struct irq_domain_ops xilinx_msi_domain_ops = {
	/* [한국어] 벡터 잡기. */
	.alloc	= xilinx_msi_domain_alloc,
	/* [한국어] 벡터 놓기. */
	.free	= xilinx_msi_domain_free,
};

/* [한국어]
 * xilinx_init_dev_msi_info - 상위 MSI 도메인을 만들 때 그 정보를 손본다
 *
 * @dev: MSI 를 쓸 장치.
 * @domain: 만들어지는 도메인.
 * @real_parent: 실제 부모 도메인.
 * @info: 채워질 MSI 도메인 정보.
 * @return: true 성공, false 면 공용 헬퍼가 실패했다.
 *
 * MSI 상위 계층은 장치마다 자식 도메인을 만드는데, 그때 부모 쪽이 끼어들어
 * 정보를 손볼 기회를 주는 것이 이 콜백이다.
 *
 * 하는 일은 두 단계다. 먼저 공용 헬퍼에게 표준적인 채우기를 맡기고, 그 결과
 * irq_chip 에 이 파일의 빈 ack 콜백을 덮어씌운다. 그 덮어쓰기가 이 함수의
 * 존재 이유 전부다 — 실제 ack 를 인터럽트 핸들러가 이미 해 버리기 때문이며,
 * 자세한 사정은 xilinx_msi_top_irq_ack() 위의 상류 주석에 있다.
 *
 * 실행 컨텍스트: MSI 도메인을 만드는 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 에러 경로: 공용 헬퍼가 false 를 돌려주면 그대로 전한다.
 *
 * 호출 체인:
 *   MSI 상위 계층 → parent_ops->init_dev_msi_info → [이 함수]
 *     → msi_lib_init_dev_msi_info()
 */
static bool xilinx_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				     struct irq_domain *real_parent, struct msi_domain_info *info)
{
	/* [한국어] 손볼 대상인 irq_chip. 아래에서 ack 콜백 하나만 바꾼다. */
	struct irq_chip *chip = info->chip;

	/* [한국어] 표준적인 채우기를 공용 헬퍼에 맡긴다. */
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		/* [한국어] 헬퍼가 실패하면 더 손볼 것이 없다. */
		return false;

	/* [한국어] 이 함수의 존재 이유 — 빈 ack 로 덮어써서 상위 계층의 기본 ack 가
	 * 엉뚱한 하드웨어를 건드리지 않게 한다. */
	chip->irq_ack = xilinx_msi_top_irq_ack;
	/* [한국어] 성공을 알린다. */
	return true;
}

/* [한국어] 이 드라이버가 MSI 상위 계층에 "반드시 이렇게 동작해야 한다" 고
 * 요구하는 플래그 셋. 줄 끝 백슬래시로 이어지는 매크로라 그 줄들에는 주석을
 * 붙일 수 없어(붙이면 연결이 끊긴다) 설명을 여기 모아 둔다.
 *   - MSI_FLAG_USE_DEF_DOM_OPS  : 도메인 동작을 공용 기본값으로 채운다.
 *   - MSI_FLAG_USE_DEF_CHIP_OPS : irq_chip 동작도 공용 기본값으로 채운다.
 *     이 둘 덕에 이 파일은 compose_msi_msg 와 ack 만 손대면 된다.
 *   - MSI_FLAG_NO_AFFINITY      : 벡터를 특정 CPU 에 묶는 기능을 제공하지
 *     않는다는 선언이다. 이 컨트롤러는 MSI 를 모두 자기 IRQ 하나로 모아
 *     올리므로, 벡터별로 CPU 를 고를 여지가 없다. */
#define XILINX_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				   MSI_FLAG_USE_DEF_CHIP_OPS	| \
				   MSI_FLAG_NO_AFFINITY)

/* [한국어] MSI 상위 계층이 이 부모 도메인을 다룰 때 쓰는 규약 표. */
static const struct msi_parent_ops xilinx_msi_parent_ops = {
	/* [한국어] 위에서 정의한 필수 플래그 셋. */
	.required_flags		= XILINX_MSI_FLAGS_REQUIRED,
	/* [한국어] 상위 계층이 요구할 수 있는 플래그의 상한. 공용 마스크를 그대로 써
	 * 일반적인 요구는 모두 받아들인다. */
	.supported_flags	= MSI_GENERIC_FLAGS_MASK,
	/* [한국어] 이 부모 도메인이 PCI MSI 용임을 알리는 표식. 상위 계층이 여러
	 * 부모 후보 중 맞는 것을 고르는 근거가 된다. */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	/* [한국어] 만들어질 자식 도메인 이름에 붙일 접두사. /proc/interrupts 등에서
	 * 어느 컨트롤러의 MSI 인지 구분된다. */
	.prefix			= "xilinx-",
	/* [한국어] 자식 도메인 정보를 손볼 콜백. 위에서 본 빈 ack 덮어쓰기를 한다. */
	.init_dev_msi_info	= xilinx_init_dev_msi_info,
};

/* [한국어]
 * xilinx_allocate_msi_domains - MSI 부모 도메인을 만든다
 *
 * @pcie: 이 드라이버의 상태.
 * @return: 0 성공, -ENOMEM 이면 도메인을 만들지 못했다.
 *
 * 도메인 명세를 지역 구조체에 채워 msi_create_parent_irq_domain() 에 넘기는
 * 것이 전부다. 그 헬퍼가 부모 도메인을 만들고, 이후 장치마다 자식 도메인이
 * 그 위에 붙는다.
 *
 * fwnode 로 컨트롤러 자신의 노드를 쓴다는 점을 눈여겨볼 것 — INTx 도메인이
 * DT 의 자식 인터럽트 컨트롤러 노드를 쓰는 것과 다르다. MSI 는 DT 의
 * interrupt-map 을 거치지 않고 PCI 계층이 직접 부모를 찾기 때문이다.
 *
 * 실행 컨텍스트: probe. 잠들 수 있다.
 *
 * 에러 경로: 실패하면 로그를 남기고 -ENOMEM. 호출자가 INTx 도메인을 되돌린다.
 *
 * 호출 체인:
 *   xilinx_pcie_init_irq_domain() → [이 함수] → msi_create_parent_irq_domain()
 */
static int xilinx_allocate_msi_domains(struct xilinx_pcie *pcie)
{
	/* [한국어] 만들 도메인의 명세. 아래 네 항목이 전부다. */
	struct irq_domain_info info = {
		/* [한국어] 컨트롤러 자신의 노드. INTx 쪽이 DT 자식 노드를 쓰는 것과 다르다. */
		.fwnode		= dev_fwnode(pcie->dev),
		/* [한국어] 위에서 정의한 alloc/free 동작. */
		.ops		= &xilinx_msi_domain_ops,
		/* [한국어] 콜백들이 되찾을 이 드라이버의 상태. */
		.host_data	= pcie,
		/* [한국어] 벡터 128개. msi_map 비트맵의 크기와 같아야 한다. */
		.size		= XILINX_NUM_MSI_IRQS,
	};

	/* [한국어] 부모 도메인을 만든다. 위 parent_ops 가 상위 계층과의 규약이다. */
	pcie->msi_domain = msi_create_parent_irq_domain(&info, &xilinx_msi_parent_ops);
	/* [한국어] 실패. */
	if (!pcie->msi_domain) {
		/* [한국어] MSI 없이는 이 드라이버가 쓸모가 없으므로 알린다. */
		dev_err(pcie->dev, "failed to create MSI domain\n");
		/* [한국어] 도메인 생성 실패는 대개 메모리 부족이다. */
		return -ENOMEM;
	}

	/* [한국어] 부모 도메인이 준비됐다. */
	return 0;
}

/* [한국어]
 * xilinx_free_irq_domains - 만든 IRQ 도메인 둘을 없앤다
 *
 * @pcie: 이 드라이버의 상태.
 * @return: 없음.
 *
 * probe 의 마지막 실패 경로에서만 불린다. 이 드라이버에는 remove 가 없으므로
 * 이것이 유일한 도메인 정리 경로다.
 *
 * [상류 코드 관찰] 두 도메인 모두 NULL 검사 없이 없앤다. CONFIG_PCI_MSI 가
 * 꺼져 있으면 msi_domain 이 NULL 로 남는 구조인데, 이 드라이버의 Kconfig 가
 * PCI_MSI 를 depends 로 요구하므로(같은 디렉터리 Kconfig 에서 확인) 실제로는
 * 그 상황이 생기지 않는다. 형제 판인 pcie-xilinx-cpm.c 의
 * xilinx_cpm_free_irq_domains() 가 NULL 검사를 두고 포인터까지 지우는 것과
 * 대비된다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe 실패 되감기. 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   xilinx_pcie_probe()(pci_host_probe 실패 시) → [이 함수] → irq_domain_remove()
 */
static void xilinx_free_irq_domains(struct xilinx_pcie *pcie)
{
	/* [한국어] MSI 부모 도메인을 없앤다. */
	irq_domain_remove(pcie->msi_domain);
	/* [한국어] INTx 도메인도 없앤다. 만든 순서의 반대다. */
	irq_domain_remove(pcie->leg_domain);
}

/* [한국어] 여기부터 INTx 관련 코드다. MSI 쪽이 계층형 도메인이었던 것과 달리
 * 이쪽은 map 하나만 두는 선형 도메인이다. */
/* INTx Functions */

/**
 * xilinx_pcie_intx_map - Set the handler for the INTx and mark IRQ as valid
 * @domain: IRQ domain
 * @irq: Virtual IRQ number
 * @hwirq: HW interrupt number
 *
 * Return: Always returns 0.
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * irq_domain_ops.map 콜백이다. 도메인에 새 hwirq 매핑이 생길 때 IRQ 코어가
 * 한 번 불러, 그 가상 IRQ 에 irq_chip 과 흐름 핸들러를 매단다.
 *
 * 고른 조합이 이 파일의 성격을 보여 준다.
 *   - dummy_irq_chip: 아무것도 하지 않는 공용 irq_chip 이다. 즉 이 드라이버는
 *     INTx 선을 개별적으로 마스크하는 기능을 제공하지 않는다. 하드웨어에
 *     그런 레지스터가 없어서인지 구현하지 않은 것인지는 이 트리에서 확인 못 함.
 *   - handle_simple_irq: 마스크·ack 를 하지 않는 가장 단순한 흐름이다.
 *     실제 ack(FIFO 비우기)를 인터럽트 핸들러가 이미 끝냈으므로 성립한다.
 *
 * 형제 판인 pcie-xilinx-cpm.c 는 같은 자리에 IDRN_MASK 를 읽고 고치는 진짜
 * irq_chip 과 handle_level_irq 를 둔다. 같은 벤더의 같은 INTx 를 두고 두
 * 세대의 구현이 크게 갈리는 지점이다.
 *
 * 실행 컨텍스트: 매핑이 만들어질 때. 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다. 위 상류 주석대로 언제나 0 을 돌려준다.
 *
 * 호출 체인:
 *   irq_create_mapping() / irq_domain 코어 → domain->ops->map → [이 함수]
 *     → irq_set_chip_and_handler(), irq_set_chip_data()
 */
static int xilinx_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	/* [한국어] 아무 동작도 없는 공용 irq_chip 과 가장 단순한 흐름 핸들러를 매단다. */
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	/* [한국어] 도메인의 host_data(= pcie)를 chip_data 로 옮겨 둔다. 다만 이
	 * 도메인의 irq_chip 은 dummy 라 그것을 되찾아 쓰는 콜백이 없다. */
	irq_set_chip_data(irq, domain->host_data);

	/* [한국어] 위 상류 주석대로 언제나 성공이다. */
	return 0;
}

/* INTx IRQ Domain operations */
/* [한국어] INTx 도메인의 동작. map 과 xlate 둘을 채운다.
 * xlate 가 있다는 것이 형제 판 pcie-xilinx-cpm.c 와 다른 점으로,
 * DT 의 interrupt-map 에 적힌 INTA~INTD 표기를 hwirq 0~3 으로 옮겨 준다.
 * 그것이 없으면 코어의 기본 해석에 기대게 된다. */
static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 새 매핑에 irq_chip 과 흐름 핸들러를 매단다. */
	.map = xilinx_pcie_intx_map,
	/* [한국어] DT 의 INTx 표기를 hwirq 로 옮기는 공용 헬퍼. nwl 판도 같은 것을 쓴다. */
	.xlate = pci_irqd_intx_xlate,
};

/* [한국어] 여기부터 하드웨어를 직접 다루는 함수들이다 — 인터럽트 핸들러,
 * 도메인 초기화, 포트 초기화, DT 파싱, probe. */
/* PCIe HW Functions */

/**
 * xilinx_pcie_intr_handler - Interrupt Service Handler
 * @irq: IRQ number
 * @data: PCIe port information
 *
 * Return: IRQ_HANDLED on success and IRQ_NONE on failure
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * 이 파일에서 가장 긴 함수이며, 이 컨트롤러의 인터럽트가 모두 여기로 들어온다.
 * 연쇄 핸들러가 아니라 devm_request_irq 로 건 보통의 핸들러라는 점이
 * 형제 판 pcie-xilinx-cpm.c 와 가장 크게 다른 부분이다 — cpm 은 연쇄 핸들러
 * 둘에 사건마다 별도 핸들러를 걸어 흩지만, 이 파일은 하나가 다 처리한다.
 *
 * 절차는 셋이다.
 *   1. IDR(상태)과 IMR(마스크)을 읽어 AND 한다. 걸렸고 또 열려 있는 사건만
 *      남기는 것이며, 아무것도 없으면 IRQ_NONE 을 돌려준다 — IRQF_SHARED 로
 *      등록되어 다른 장치와 IRQ 선을 공유할 수 있으므로 "내 것이 아니다" 를
 *      정확히 답해야 한다.
 *   2. 사건 종류별로 처리한다. 대부분은 dev_warn 한 줄이고, AER 3종만 오류
 *      FIFO 를 비우며, INTx/MSI 만 FIFO 를 읽어 도메인으로 넘긴다.
 *   3. 처리한 비트를 IDR 에 되써서 지운다(write-1-to-clear).
 *
 * INTx/MSI 갈래가 이 함수의 핵심이다. IDR 의 비트만으로는 "INTx 가 왔다" 까지만
 * 알 수 있고 몇 번 선인지는 모르므로, RPIFR1 을 한 번 더 읽어야 한다.
 * 그 레지스터의 비트 31 이 유효성, 비트 30 이 MSI/INTx 구분, 비트 28:27 이
 * INTx 번호이며, MSI 면 RPIFR2 의 하위 16비트가 벡터 번호다.
 *
 * [상류 코드 관찰] FIFO 가 유효하지 않을 때 뛰는 error 라벨이 함수 끝의
 * IDR 지우기 바로 앞에 있다. 그래서 그 경로로 뛰면 같은 status 에 함께 걸려
 * 있었을지도 모르는 AXI 슬레이브·마스터 오류 아홉 가지가 보고되지 않은 채
 * 지워진다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] ECRC_ERR 검사는 도달할 수 없다. 그 비트가
 * XILINX_PCIE_IMR_ENABLE_MASK 에 없어 IMR 에 결코 서지 않고, status 는
 * IDR & IMR 이기 때문이다. 자세한 것은 그 마스크 정의에 붙인 주석 참조.
 *
 * [상류 코드 관찰] init_port() 가 이 핸들러보다 먼저 IMR 을 열지만
 * 도메인 둘은 그 뒤의 init_irq_domain() 이 만든다. 그 사이에 INTx/MSI 가
 * 올라오면 generic_handle_domain_irq() 에 NULL 도메인이 넘어가는 배치인데,
 * NULL 을 그 함수가 어떻게 다루는지는 kernel/irq 가 이 트리에 없어 확인 못 함.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. IRQF_NO_THREAD 로 등록되어 스레드화되지
 * 않으므로 잠들 수 없다.
 *
 * 에러 경로: 걸린 사건이 없으면 IRQ_NONE, 그 밖에는 언제나 IRQ_HANDLED 다.
 *
 * 호출 체인:
 *   IRQ 코어 → [이 함수]
 *     → xilinx_pcie_clear_err_interrupts(), generic_handle_domain_irq()
 */
static irqreturn_t xilinx_pcie_intr_handler(int irq, void *data)
{
	/* [한국어] devm_request_irq 에 넘긴 dev_id 가 이 드라이버의 상태다. */
	struct xilinx_pcie *pcie = (struct xilinx_pcie *)data;
	/* [한국어] 로그에 쓸 장치. */
	struct device *dev = pcie->dev;
	/* [한국어] val 은 읽은 값(뒤에서 FIFO 값으로 재사용된다), mask 는 IMR,
	 * status 는 둘을 AND 한 "처리할 사건" 이다. */
	u32 val, mask, status;

	/* Read interrupt decode and mask registers */
	/* [한국어] 옆의 상류 주석대로 상태와 마스크를 함께 읽는다. 어떤 사건이 걸렸나. */
	val = pcie_read(pcie, XILINX_PCIE_REG_IDR);
	/* [한국어] 그중 어느 것이 열려 있나. */
	mask = pcie_read(pcie, XILINX_PCIE_REG_IMR);

	/* [한국어] 걸렸고 또 열려 있는 사건만 남긴다. 마스크된 사건까지 처리하면
	 * 닫아 둔 쪽의 기대가 깨진다. */
	status = val & mask;
	/* [한국어] 남은 것이 없다 = 이 인터럽트는 내 것이 아니다. */
	if (!status)
		/* [한국어] IRQF_SHARED 로 선을 공유하므로 정확히 답해야 다음 핸들러가 불린다. */
		return IRQ_NONE;

	/* [한국어] 링크가 끊어졌다. */
	if (status & XILINX_PCIE_INTR_LINK_DOWN)
		/* [한국어] 보고만 한다 — 이 드라이버는 링크 복구를 시도하지 않는다. */
		dev_warn(dev, "Link Down\n");

	/* [한국어] ECRC 검사 실패. 위 [상류 코드 관찰] 대로 이 갈래는 도달할 수 없다. */
	if (status & XILINX_PCIE_INTR_ECRC_ERR)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "ECRC failed\n");

	/* [한국어] 스트리밍 오류. */
	if (status & XILINX_PCIE_INTR_STR_ERR)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Streaming error\n");

	/* [한국어] 호스트가 핫리셋을 걸었다. */
	if (status & XILINX_PCIE_INTR_HOT_RESET)
		/* [한국어] 오류가 아니라 정상 절차일 수 있어 warn 이 아니라 info 다. */
		dev_info(dev, "Hot reset\n");

	/* [한국어] 설정 트랜잭션이 시간 초과됐다. */
	if (status & XILINX_PCIE_INTR_CFG_TIMEOUT)
		/* [한국어] 대개는 응답 없는 자리를 읽었다는 뜻이다.
		 * valid_device 가 링크 없는 하위 버스 접근을 미리 막는 것이 이 사건을 줄인다. */
		dev_warn(dev, "ECAM access timeout\n");

	/* [한국어] AER correctable. 아래 셋만 오류 FIFO 도 함께 비운다. */
	if (status & XILINX_PCIE_INTR_CORRECTABLE) {
		/* [한국어] 무슨 등급인지 알린다. */
		dev_warn(dev, "Correctable error message\n");
		/* [한국어] 오류를 낸 요청자 ID 를 읽어 남기고 FIFO 를 비운다. */
		xilinx_pcie_clear_err_interrupts(pcie);
	}

	/* [한국어] AER non-fatal. */
	if (status & XILINX_PCIE_INTR_NONFATAL) {
		/* [한국어] 알린다. */
		dev_warn(dev, "Non fatal error message\n");
		/* [한국어] FIFO 를 비운다. */
		xilinx_pcie_clear_err_interrupts(pcie);
	}

	/* [한국어] AER fatal. */
	if (status & XILINX_PCIE_INTR_FATAL) {
		/* [한국어] 알린다. */
		dev_warn(dev, "Fatal error message\n");
		/* [한국어] FIFO 를 비운다. 세 갈래가 같은 모양을 반복하는데, cpm 판이
		 * switch 의 fallthrough 로 한데 묶은 것과 대비된다. */
		xilinx_pcie_clear_err_interrupts(pcie);
	}

	/* [한국어] 여기가 이 함수의 핵심 갈래다. 두 비트를 한꺼번에 검사하는 것은,
	 * 둘 다 같은 FIFO 레지스터로 자세한 정보가 오기 때문이다. */
	if (status & (XILINX_PCIE_INTR_INTX | XILINX_PCIE_INTR_MSI)) {
		/* [한국어] 아래에서 INTx 냐 MSI 냐에 따라 고를 도메인. */
		struct irq_domain *domain;

		/* [한국어] FIFO 레지스터 1 을 읽는다. 여기에 유효성·종류·번호가 다 들어 있다. */
		val = pcie_read(pcie, XILINX_PCIE_REG_RPIFR1);

		/* Check whether interrupt valid */
		/* [한국어] 옆의 상류 주석대로 유효한 항목인지 먼저 본다. */
		if (!(val & XILINX_PCIE_RPIFR1_INTR_VALID)) {
			/* [한국어] IDR 은 사건이 있다는데 FIFO 는 비어 있는 모순 상황이다. */
			dev_warn(dev, "RP Intr FIFO1 read error\n");
			/* [한국어] 위 [상류 코드 관찰] 이 가리키는 지점 — 아래 AXI 오류 검사들을
			 * 건너뛰고 곧장 IDR 지우기로 간다. */
			goto error;
		}

		/* Decode the IRQ number */
		/* [한국어] 옆의 상류 주석대로 번호를 뽑는다. 비트 30 이 MSI 여부를 가른다. */
		if (val & XILINX_PCIE_RPIFR1_MSI_INTR) {
			/* [한국어] MSI 다. 벡터 번호는 FIFO 2 의 하위 16비트에 실려 있는데,
			 * 그 값은 xilinx_compose_msi_msg() 가 hwirq 를 그대로 데이터로 넣어 준 것이
			 * 장치를 거쳐 돌아온 것이다. */
			val = pcie_read(pcie, XILINX_PCIE_REG_RPIFR2) &
				XILINX_PCIE_RPIFR2_MSG_DATA;
			/* [한국어] MSI 부모 도메인으로 넘길 것이다. */
			domain = pcie->msi_domain;
		/* [한국어] MSI 가 아니면 INTx 다. */
		} else {
			/* [한국어] 비트 28:27 을 뽑아 0~3(INTA~INTD)으로 내린다. FIELD_GET 을 쓰지
			 * 않고 마스크와 시프트를 따로 둔 형태이며, cpm 판이 FIELD_GET 을 쓰는 것과
			 * 대비된다. */
			val = (val & XILINX_PCIE_RPIFR1_INTR_MASK) >>
				XILINX_PCIE_RPIFR1_INTR_SHIFT;
			/* [한국어] INTx 도메인으로 넘길 것이다. */
			domain = pcie->leg_domain;
		}

		/* Clear interrupt FIFO register 1 */
		/* [한국어] 옆의 상류 주석대로 FIFO 항목을 비운다. 도메인으로 넘기기 **전에**
		 * 비우는 순서라, 핸들러가 도는 동안 다음 항목이 들어올 자리가 열려 있다.
		 * 이것이 MSI 쪽 ack 를 이미 끝냈다고 보는 근거이며, 그래서
		 * xilinx_msi_top_irq_ack() 가 빈 함수다. */
		pcie_write(pcie, XILINX_PCIE_RPIFR1_ALL_MASK,
			   XILINX_PCIE_REG_RPIFR1);

		/* [한국어] 고른 도메인의 해당 hwirq 를 부른다. MSI 면 장치 드라이버의 MSI
		 * 핸들러로, INTx 면 그 선에 걸린 핸들러로 넘어간다. 반환값은 보지 않는다. */
		generic_handle_domain_irq(domain, val);
	}

	/* [한국어] 아래 아홉은 AXI 버스 쪽 오류들이다. 모두 보고만 하며, 위 error
	 * 라벨로 뛴 경우에는 건너뛰어진다. 슬레이브 쪽 — 지원하지 않는 요청. */
	if (status & XILINX_PCIE_INTR_SLV_UNSUPP)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Slave unsupported request\n");

	/* [한국어] 예상하지 못한 완료. */
	if (status & XILINX_PCIE_INTR_SLV_UNEXP)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Slave unexpected completion\n");

	/* [한국어] 완료 대기 시간 초과. */
	if (status & XILINX_PCIE_INTR_SLV_COMPL)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Slave completion timeout\n");

	/* [한국어] 오염 표시가 붙은 응답. */
	if (status & XILINX_PCIE_INTR_SLV_ERRP)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Slave Error Poison\n");

	/* [한국어] 완료자 중단. */
	if (status & XILINX_PCIE_INTR_SLV_CMPABT)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Slave Completer Abort\n");

	/* [한국어] 허용되지 않는 버스트. */
	if (status & XILINX_PCIE_INTR_SLV_ILLBUR)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Slave Illegal Burst\n");

	/* [한국어] 마스터 쪽 — 주소에 해당하는 대상이 없다. */
	if (status & XILINX_PCIE_INTR_MST_DECERR)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Master decode error\n");

	/* [한국어] 슬레이브가 오류 응답을 돌려주었다. */
	if (status & XILINX_PCIE_INTR_MST_SLVERR)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Master slave error\n");

	/* [한국어] 마스터 쪽 오류 오염. 공용 헤더가 같은 비트를 다른 뜻으로 쓰는
	 * 자리다 — 상수 정의 쪽 주석 참조. */
	if (status & XILINX_PCIE_INTR_MST_ERRP)
		/* [한국어] 보고만 한다. */
		dev_warn(dev, "Master error poison\n");

/* [한국어] FIFO 가 유효하지 않을 때 뛰어 들어오는 자리. 위 [상류 코드 관찰] 대로
 * AXI 오류 검사들을 건너뛴 채 여기 도착한다. */
error:
	/* Clear the Interrupt Decode register */
	/* [한국어] 옆의 상류 주석대로 처리한 비트를 되써서 지운다(write-1-to-clear).
	 * 처리 전이 아니라 후에 지우므로, 핸들러가 도는 동안 같은 사건이 다시 걸리면
	 * 그것도 함께 지워진다. */
	pcie_write(pcie, status, XILINX_PCIE_REG_IDR);

	/* [한국어] 사건이 하나라도 있었으면 처리했다고 답한다. */
	return IRQ_HANDLED;
}

/**
 * xilinx_pcie_init_irq_domain - Initialize IRQ domain
 * @pcie: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * 인터럽트 도메인 둘을 만든다. 둘의 성격이 아주 달라 만드는 방법도 다르다.
 *
 *   INTx: DT 의 첫 자식(인터럽트 컨트롤러 노드)을 fwnode 로 삼는 선형 도메인.
 *     하위 장치의 DT 가 interrupt-parent 로 그 노드를 가리키면 여기 이어진다.
 *   MSI:  컨트롤러 자신의 노드를 fwnode 로 삼는 계층형 부모 도메인. DT 를
 *     거치지 않고 PCI 계층이 직접 부모를 찾으므로 자식 노드가 필요 없다.
 *
 * MSI 쪽에는 한 단계가 더 있다. 도메인을 만든 뒤 MSIBASE1/2 레지스터에
 * "MSI 쓰기가 도달할 물리 주소" 를 알려 주어야 하는데, 그 주소가
 * xilinx_compose_msi_msg() 가 장치에 알려 줄 주소와 반드시 같아야 한다.
 * 그래서 두 곳이 똑같은 계산(구조체 주소를 4KB 경계로 내림)을 각각 한다.
 *
 * [상류 코드 관찰] IS_ENABLED(CONFIG_PCI_MSI) 검사가 있지만, 이 드라이버의
 * Kconfig 가 PCI_MSI 를 depends 로 요구하므로(같은 디렉터리 Kconfig 에서 확인)
 * 그 조건은 언제나 참이다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는
 * 손대지 않았다.
 *
 * [상류 코드 관찰] 이 함수가 probe 에서 xilinx_pcie_init_port() 보다 **뒤에**
 * 불린다. init_port 는 이미 IMR 을 열어 두었고 인터럽트 핸들러는 그보다도
 * 앞선 parse_dt 에서 걸렸으므로, 이 함수가 끝나기 전에 INTx/MSI 가 올라오면
 * 핸들러가 아직 NULL 인 도메인을 넘기게 된다. 그 배치의 의도는 코드에 적혀
 * 있지 않다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 자식 노드가 없거나 INTx 도메인 생성이 실패하면 -ENODEV,
 * MSI 도메인 생성이 실패하면 INTx 도메인을 되돌린 뒤 그 오류를 올린다.
 *
 * 호출 체인:
 *   xilinx_pcie_probe() → [이 함수]
 *     → of_get_next_child(), irq_domain_create_linear(),
 *       xilinx_allocate_msi_domains(), pcie_write()
 */
static int xilinx_pcie_init_irq_domain(struct xilinx_pcie *pcie)
{
	/* [한국어] 로그와 DT 조회에 쓸 장치. */
	struct device *dev = pcie->dev;
	/* [한국어] 인터럽트 컨트롤러 역할을 하는 DT 자식 노드. */
	struct device_node *pcie_intc_node;
	/* [한국어] MSI 도메인 생성의 결과. */
	int ret;

	/* Setup INTx */
	/* [한국어] 옆의 상류 주석대로 INTx 부터 만든다. 첫 자식을 가져오면 참조
	 * 카운트가 올라가므로 아래에서 반드시 놓아 주어야 한다. */
	pcie_intc_node = of_get_next_child(dev->of_node, NULL);
	/* [한국어] 자식이 없다 = DT 에 인터럽트 컨트롤러 노드가 빠졌다. */
	if (!pcie_intc_node) {
		/* [한국어] DT 가 잘못된 경우라 알린다. */
		dev_err(dev, "No PCIe Intc node found\n");
		/* [한국어] 아직 참조를 얻지 못했으므로 of_node_put 없이 반환한다. */
		return -ENODEV;
	}

	/* [한국어] INTx 도메인을 만든다. 크기 PCI_NUM_INTX(4)가 INTA~INTD 넷이고,
	 * 마지막 인자 pcie 가 host_data 로 저장되어 map 콜백이 chip_data 로 옮긴다. */
	pcie->leg_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						    &intx_domain_ops, pcie);
	/* [한국어] 성공하든 실패하든 노드 참조를 먼저 놓는다 — 아래 검사보다 앞서
	 * 놓아 두어 실패 경로마다 되풀이하지 않게 한 배치다. */
	of_node_put(pcie_intc_node);
	/* [한국어] 도메인 생성 실패. */
	if (!pcie->leg_domain) {
		/* [한국어] 알린다. */
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		/* [한국어] 메모리 부족이 흔한 원인이지만 -ENODEV 로 통일해 돌려준다. */
		return -ENODEV;
	}

	/* Setup MSI */
	/* [한국어] 옆의 상류 주석대로 이어서 MSI 를 준비한다. 위 [상류 코드 관찰] 대로
	 * 이 조건은 이 드라이버에서 언제나 참이다. */
	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		/* [한국어] MSI 쓰기가 도달할 주소. compose_msi_msg 가 장치에 알려 줄 값과
		 * 반드시 같아야 하므로 같은 계산을 되풀이한다. */
		phys_addr_t pa = ALIGN_DOWN(virt_to_phys(pcie), SZ_4K);

		/* [한국어] MSI 부모 도메인을 만든다. */
		ret = xilinx_allocate_msi_domains(pcie);
		/* [한국어] 실패. */
		if (ret) {
			/* [한국어] 이미 만든 INTx 도메인을 되돌린다. */
			irq_domain_remove(pcie->leg_domain);
			/* [한국어] 하위가 준 오류를 그대로 올린다. */
			return ret;
		}

		/* [한국어] 컨트롤러에 MSI 수신 주소의 상위 32비트를 알린다. */
		pcie_write(pcie, upper_32_bits(pa), XILINX_PCIE_REG_MSIBASE1);
		/* [한국어] 하위 32비트도 알린다. 이 두 쓰기 이후로 그 주소로 오는 메모리
		 * 쓰기가 인터럽트로 바뀐다. */
		pcie_write(pcie, lower_32_bits(pa), XILINX_PCIE_REG_MSIBASE2);
	}

	/* [한국어] 두 도메인이 준비됐다. */
	return 0;
}

/**
 * xilinx_pcie_init_port - Initialize hardware
 * @pcie: PCIe port information
 */
/* [한국어] 위 상류 kernel-doc 이 인자를 요약했고, 아래에 배경을 덧붙인다.
 *
 * 하드웨어를 실제로 켜는 함수다. 순서가 곧 의미다.
 *   1. 링크 상태를 읽어 로그만 남긴다. 기다리지도, 실패로 보지도 않는다.
 *   2. 모든 인터럽트를 끈다(IMR 에 0).
 *   3. 이미 걸려 있던 상태를 지운다.
 *   4. 이 드라이버가 처리하는 사건들을 연다(IMR 에 ENABLE_MASK).
 *   5. 브리지 활성 비트(BEN)를 세운다.
 *
 * 4번이 형제 판 pcie-xilinx-cpm.c 와 갈리는 지점이다. cpm 은 인터럽트를 다 꺼
 * 둔 채로 두고, 사건마다 devm_request_irq 를 부를 때 IRQ 코어가 unmask 콜백을
 * 통해 필요한 비트만 켜 준다. 이 파일에는 그런 콜백이 없으므로 여기서
 * 한꺼번에 열어 두는 것이며, 그래서 IMR 을 런타임에 고치는 코드가 없다.
 *
 * 5번이 마지막인 것도 의미가 있다. 이 한 줄이 있어야 브리지가 트랜잭션을
 * 통과시키므로, 인터럽트 준비가 끝난 뒤에 열어야 한다.
 *
 * [상류 코드 관찰] 2번의 "모두 끄기" 를 ~XILINX_PCIE_IDR_ALL_MASK 로 쓴다.
 * 그 상수가 0xFFFFFFFF 이므로 결과는 0 이며, IMR 에 0 을 쓰는 것과 같다.
 * cpm 판에도 똑같은 관용구가 있다. 원본 스냅숏(1f0e418bb6)에서 확인했으며
 * 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다. 반환값이 void 이며, 링크가 서지 않아도 그대로 진행한다.
 *
 * 호출 체인:
 *   xilinx_pcie_probe() → [이 함수] → xilinx_pcie_link_up(), pcie_read(), pcie_write()
 */
static void xilinx_pcie_init_port(struct xilinx_pcie *pcie)
{
	/* [한국어] 로그에 쓸 장치. */
	struct device *dev = pcie->dev;

	/* [한국어] 링크 상태를 확인한다. 결과는 로그로만 쓰인다 — 이 드라이버에는
	 * 링크를 기다리는 루프가 없다. */
	if (xilinx_pcie_link_up(pcie))
		/* [한국어] 섰다고 알린다. */
		dev_info(dev, "PCIe Link is UP\n");
	else
		/* [한국어] 서지 않았어도 초기화를 계속한다. 나중에 장치가 붙을 수 있고,
		 * valid_device 가 링크 없는 하위 접근을 알아서 막아 준다. */
		dev_info(dev, "PCIe Link is DOWN\n");

	/* Disable all interrupts */
	/* [한국어] 옆의 상류 주석대로 모든 인터럽트를 끈다. ~0xFFFFFFFF 는 0 이므로
	 * IMR 에 0 을 쓰는 것과 같다 — 위 [상류 코드 관찰] 참조. */
	pcie_write(pcie, ~XILINX_PCIE_IDR_ALL_MASK,
		   XILINX_PCIE_REG_IMR);

	/* Clear pending interrupts */
	/* [한국어] 옆의 상류 주석대로 이미 걸려 있던 사건을 지운다. IDR 을 읽어
	 * 이 드라이버가 아는 비트만 남긴 뒤 되쓴다(write-1-to-clear).
	 * 부팅 전 펌웨어가 남겼을 수 있는 상태를 치우는 단계다. */
	pcie_write(pcie, pcie_read(pcie, XILINX_PCIE_REG_IDR) &
			 XILINX_PCIE_IMR_ALL_MASK,
		   XILINX_PCIE_REG_IDR);

	/* Enable all interrupts we handle */
	/* [한국어] 옆의 상류 주석대로 처리할 사건들을 연다. 이 한 줄이 이 드라이버의
	 * 인터럽트 활성화 전부이며, 이후 IMR 을 고치는 코드가 없다. */
	pcie_write(pcie, XILINX_PCIE_IMR_ENABLE_MASK, XILINX_PCIE_REG_IMR);

	/* Enable the Bridge enable bit */
	/* [한국어] 옆의 상류 주석대로 브리지 활성 비트를 세운다. 다른 비트를 지키기
	 * 위해 읽기-수정-쓰기를 하며, 초기화의 마지막 단계로 놓였다. */
	pcie_write(pcie, pcie_read(pcie, XILINX_PCIE_REG_RPSC) |
			 XILINX_PCIE_REG_RPSC_BEN,
		   XILINX_PCIE_REG_RPSC);
}

/**
 * xilinx_pcie_parse_dt - Parse Device tree
 * @pcie: PCIe port information
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * DT 에서 둘을 가져온다 — 레지스터 창과 인터럽트.
 *
 * 창을 devm_pci_remap_cfg_resource() 로 매핑하는 점을 눈여겨볼 것. 보통의
 * ioremap 이 아니라 설정공간용 헬퍼인데, 이 창이 브리지 레지스터뿐 아니라
 * ECAM 설정공간까지 겸하기 때문이다. 그 헬퍼가 설정공간 접근에 맞는 메모리
 * 속성을 골라 준다.
 *
 * 인터럽트는 IRQF_SHARED 와 IRQF_NO_THREAD 로 건다. 전자는 이 IRQ 선을 다른
 * 장치와 나눠 쓸 수 있다는 뜻이고(그래서 핸들러가 IRQ_NONE 을 정확히 답해야
 * 한다), 후자는 강제 스레드화 커널에서도 이 핸들러를 인터럽트 컨텍스트에
 * 남겨 둔다는 뜻이다.
 *
 * 이 시점에 핸들러가 걸린다는 사실이 중요하다. probe 는 이 뒤에 init_port 로
 * 인터럽트를 열고 그 뒤에야 도메인을 만들므로, 핸들러는 도메인보다 먼저
 * 살아 있게 된다 — intr_handler 의 [상류 코드 관찰] 참조.
 *
 * [상류 코드 관찰] irq_of_parse_and_map() 의 반환값을 확인하지 않는다.
 * 매핑에 실패하면 0 이 돌아오는데, 그대로 devm_request_irq 에 넘어간다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 각 실패를 그대로 올린다. devm 이라 되감을 것이 없다.
 *
 * 호출 체인:
 *   xilinx_pcie_probe() → [이 함수]
 *     → of_address_to_resource(), devm_pci_remap_cfg_resource(),
 *       irq_of_parse_and_map(), devm_request_irq()
 */
static int xilinx_pcie_parse_dt(struct xilinx_pcie *pcie)
{
	/* [한국어] 로그와 자원 조회에 쓸 장치. */
	struct device *dev = pcie->dev;
	/* [한국어] 이 컨트롤러의 DT 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] DT 의 reg 를 옮겨 담을 자원 서술자. 지역 변수인 것은 매핑 뒤에는
	 * 필요 없기 때문이다. */
	struct resource regs;
	/* [한국어] DT 에서 얻은 가상 IRQ 번호. */
	unsigned int irq;
	/* [한국어] 각 단계의 결과. */
	int err;

	/* [한국어] DT 의 첫 reg 항목을 자원 서술자로 옮긴다. */
	err = of_address_to_resource(node, 0, &regs);
	/* [한국어] reg 속성이 없거나 형식이 틀리다. */
	if (err) {
		/* [한국어] 어느 속성이 문제인지 이름까지 남긴다. */
		dev_err(dev, "missing \"reg\" property\n");
		/* [한국어] 그대로 올린다. */
		return err;
	}

	/* [한국어] 설정공간용 매핑 헬퍼로 창을 잡는다. 이 창이 ECAM 설정공간을
	 * 겸하기 때문에 보통의 ioremap 이 아니라 이것을 쓴다. */
	pcie->reg_base = devm_pci_remap_cfg_resource(dev, &regs);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(pcie->reg_base))
		/* [한국어] ERR_PTR 에서 코드를 꺼내 올린다. */
		return PTR_ERR(pcie->reg_base);

	/* [한국어] DT 의 첫 인터럽트를 가상 IRQ 로 옮긴다. 위 [상류 코드 관찰] 대로
	 * 반환값을 확인하지 않는다. */
	irq = irq_of_parse_and_map(node, 0);
	/* [한국어] 그 IRQ 에 이 파일의 핸들러를 건다. IRQF_SHARED 는 선을 공유할 수
	 * 있다는 뜻, IRQF_NO_THREAD 는 스레드화하지 말라는 뜻이다.
	 * 이 줄이 실행되는 순간부터 인터럽트가 이 파일로 들어올 수 있다. */
	err = devm_request_irq(dev, irq, xilinx_pcie_intr_handler,
			       IRQF_SHARED | IRQF_NO_THREAD,
			       "xilinx-pcie", pcie);
	/* [한국어] 등록 실패. */
	if (err) {
		/* [한국어] 어느 IRQ 였는지 남긴다. */
		dev_err(dev, "unable to request irq %d\n", irq);
		/* [한국어] 그대로 올린다. */
		return err;
	}

	/* [한국어] 창과 인터럽트가 모두 준비됐다. */
	return 0;
}

/**
 * xilinx_pcie_probe - Probe function
 * @pdev: Platform device pointer
 *
 * Return: '0' on success and error value on failure
 */
/* [한국어] 위 상류 kernel-doc 이 인자와 반환값을 요약했고, 아래에 배경을 덧붙인다.
 *
 * 순서가 곧 내용이다.
 *   1. DT 노드가 있는지 본다 — 이 드라이버는 DT 전용이다.
 *   2. 브리지와 이 드라이버의 상태를 한 덩어리로 잡고, MSI 비트맵 잠금을 준비한다.
 *   3. parse_dt: 레지스터 창을 매핑하고 인터럽트 핸들러를 건다.
 *   4. init_port: 링크를 보고하고 인터럽트를 열고 브리지를 켠다.
 *   5. init_irq_domain: INTx·MSI 도메인을 만들고 MSI 수신 주소를 알린다.
 *   6. 브리지에 ops 와 sysdata 를 걸고 버스를 스캔한다.
 *
 * [상류 코드 관찰] 4번과 5번의 순서가 뒤집혀 있다고 볼 여지가 있다.
 * 3번에서 이미 핸들러가 걸리고 4번에서 인터럽트가 열리는데, 그것을 받아
 * 넘길 도메인은 5번에서야 생긴다. 그 사이의 INTx/MSI 는 NULL 도메인으로
 * 넘어가는 배치다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] devm_pci_alloc_host_bridge() 가 NULL 을 돌려줬을 때
 * -ENOMEM 이 아니라 -ENODEV 를 반환한다. 형제 판 pcie-xilinx-cpm.c 도 같은
 * 값을 쓴다.
 *
 * [상류 코드 관찰] 되감기 라벨이 하나도 없다. 3~5번의 실패는 그대로 반환하고,
 * 6번의 실패만 도메인을 되돌린다. 그런데 그때도 init_port 가 연 인터럽트는
 * 닫지 않는다 — 다만 이 드라이버에는 remove 가 없고 아래
 * builtin_platform_driver 로 등록되므로, 프로브 실패 후 장치가 다시 살아나는
 * 경로 자체가 없다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 위 설명대로다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_pci_alloc_host_bridge(), xilinx_pcie_parse_dt(),
 *       xilinx_pcie_init_port(), xilinx_pcie_init_irq_domain(), pci_host_probe()
 */
static int xilinx_pcie_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 자원 조회에 두루 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 드라이버의 상태. 브리지 뒤의 자리를 가리키게 된다. */
	struct xilinx_pcie *pcie;
	/* [한국어] PCI 코어가 관리하는 호스트 브리지 객체. */
	struct pci_host_bridge *bridge;
	/* [한국어] 각 단계의 결과. */
	int err;

	/* [한국어] 이 드라이버는 DT 전용이다. ACPI 등으로 붙을 길이 없다. */
	if (!dev->of_node)
		/* [한국어] 다룰 수 없는 장치라는 뜻으로. */
		return -ENODEV;

	/* [한국어] 브리지와 이 드라이버의 상태를 한 번에 잡는다. sizeof(*pcie) 만큼
	 * 여분을 붙여 달라는 뜻이며, 0 초기화도 여기서 보장된다. */
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	/* [한국어] 할당 실패. */
	if (!bridge)
		/* [한국어] 위 [상류 코드 관찰] 대로 메모리 부족인데 -ENODEV 를 돌려준다. */
		return -ENODEV;

	/* [한국어] 브리지 뒤에 붙은 여분 공간의 주소를 얻는다. */
	pcie = pci_host_bridge_priv(bridge);
	/* [한국어] MSI 비트맵을 보호할 뮤텍스를 준비한다. 도메인을 만들기 전에
	 * 해 두어야 alloc 콜백이 언제 불려도 안전하다. */
	mutex_init(&pcie->map_lock);
	/* [한국어] 로그에 쓸 장치를 채운다. */
	pcie->dev = dev;

	/* [한국어] 레지스터 창을 매핑하고 인터럽트 핸들러를 건다. 이 줄 이후
	 * 인터럽트가 이 파일로 들어올 수 있다. */
	err = xilinx_pcie_parse_dt(pcie);
	/* [한국어] 실패. */
	if (err) {
		/* [한국어] 어느 단계인지 뭉뚱그려 알린다. */
		dev_err(dev, "Parsing DT failed\n");
		/* [한국어] devm 이라 되감을 것이 없다. */
		return err;
	}

	/* [한국어] 하드웨어를 켠다. 이 안에서 IMR 이 열리므로, 아래 도메인 생성이
	 * 끝나기 전에도 인터럽트가 올라올 수 있다 — 위 [상류 코드 관찰] 참조. */
	xilinx_pcie_init_port(pcie);

	/* [한국어] INTx·MSI 도메인을 만들고 MSI 수신 주소를 하드웨어에 알린다. */
	err = xilinx_pcie_init_irq_domain(pcie);
	/* [한국어] 실패. */
	if (err) {
		/* [한국어] 알린다. */
		dev_err(dev, "Failed creating IRQ Domain\n");
		/* [한국어] 그대로 올린다. init_port 가 연 인터럽트는 닫지 않는다. */
		return err;
	}

	/* [한국어] 설정공간 접근 함수들이 bus->sysdata 로 되찾을 수 있게 심어 둔다. */
	bridge->sysdata = pcie;
	/* [한국어] map_bus + 공용 헬퍼 표를 건다. */
	bridge->ops = &xilinx_pcie_ops;

	/* [한국어] 버스를 스캔한다. 이 안에서 설정공간을 훑어 장치를 찾고, 자원을
	 * 배정하고, 하위 드라이버를 붙인다. */
	err = pci_host_probe(bridge);
	/* [한국어] 스캔 실패. */
	if (err)
		/* [한국어] 만든 도메인 둘을 되돌린다. 이 드라이버의 유일한 도메인 정리 경로다. */
		xilinx_free_irq_domains(pcie);

	/* [한국어] 성공이면 0, 실패면 그 오류가 그대로 나간다. */
	return err;
}

/* [한국어] DT compatible 과 이 드라이버를 잇는 매칭 목록. 항목이 하나뿐이고
 * .data 도 없다 — 판본별로 달라지는 것이 없다는 뜻이며, 형제 판
 * pcie-xilinx-cpm.c 가 네 판본을 표로 가르는 것과 대비된다.
 * [상류 코드 관찰] MODULE_DEVICE_TABLE 선언이 없다. 아래
 * builtin_platform_driver 로 커널에 붙박이로 들어가므로 모듈 자동 로딩용
 * 별칭이 필요 없기 때문이다. */
static const struct of_device_id xilinx_pcie_of_match[] = {
	/* [한국어] AXI PCIe Bridge IP 1.00.a 판. 버전까지 문자열에 박혀 있다. */
	{ .compatible = "xlnx,axi-pcie-host-1.00.a", },
	/* [한국어] 목록의 끝을 알리는 빈 항목. */
	{}
};

/* [한국어] 플랫폼 드라이버 등록 정보.
 * [상류 코드 관찰] .remove 가 없다. suppress_bind_attrs 로 sysfs 언바인드도
 * 막혀 있고 아래 builtin_platform_driver 로 등록되므로, 이 드라이버는 한 번
 * 붙으면 떨어지지 않는다. 형제 판 cpm 도 같은 구성이다. */
static struct platform_driver xilinx_pcie_driver = {
	/* [한국어] 드라이버 코어에 전달할 공통 정보. */
	.driver = {
		/* [한국어] sysfs 와 로그에 나타나는 드라이버 이름. */
		.name = "xilinx-pcie",
		/* [한국어] 위의 DT 매칭 목록. */
		.of_match_table = xilinx_pcie_of_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다. PCIe 호스트 브리지를
		 * 임의로 떼면 그 아래 장치가 모두 사라지므로 막아 둔 것이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] 매칭된 디바이스마다 불릴 진입점. remove 짝은 두지 않았다. */
	.probe = xilinx_pcie_probe,
};
/* [한국어] 모듈이 아니라 커널에 붙박이로 등록한다. module_platform_driver 와
 * 달리 모듈 언로드 경로를 만들지 않으므로, 위에 .remove 가 없는 것과 짝이 맞는다. */
builtin_platform_driver(xilinx_pcie_driver);
