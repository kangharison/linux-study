// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Marvell Armada 370 and Armada XP SoCs
 *
 * Author: Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 */

/*
 * [한국어 설명] 브리지를 소프트웨어로 흉내 내는 Marvell mvebu PCIe 드라이버 (pci-mvebu.c)
 *
 * === 파일의 역할 ===
 * Marvell Armada 370 / XP 계열 SoC 의 PCIe 컨트롤러를 루트 컴플렉스로 모는
 * 플랫폼 드라이버다. 다른 호스트 드라이버와 결정적으로 다른 점이 하나 있고,
 * 이 파일의 구조 대부분이 거기서 나온다.
 *
 * 이 하드웨어에는 PCI-to-PCI 브리지가 없다. 포트마다 링크가 하나씩 있을 뿐,
 * 커널이 기대하는 "루트 포트 = 브리지 config space" 가 하드웨어에 존재하지
 * 않는다. 그런데 PCI 코어는 버스를 열거하려면 브리지가 있어야 한다 —
 * 하위 버스 번호, 메모리·I/O 창(base/limit), 링크 상태를 브리지 config
 * 에서 읽어야 하기 때문이다.
 *
 * 그래서 이 드라이버는 브리지를 소프트웨어로 만들어 낸다. 공용 기반이
 * drivers/pci/pci-bridge-emul.c 이고, 이 파일은 거기에 여섯 개의 콜백을
 * 붙여 "가짜 브리지의 config 를 읽고 쓰면 실제 하드웨어 레지스터가
 * 움직이도록" 잇는다. 그 잇는 방식이 이 파일의 핵심이다.
 *
 *   PCI 코어가 브리지 config 를 읽음
 *     -> pci_bridge_emul 이 에뮬레이트 버퍼에서 답하거나
 *        -> 이 파일의 read 콜백이 실제 레지스터를 읽어 답한다
 *   PCI 코어가 브리지 config 에 씀(예: 메모리 창 base/limit)
 *     -> 이 파일의 write 콜백이 그 값을 받아
 *        -> mbus 주소 창(mvebu_pcie_set_window)으로 번역해 하드웨어에 반영
 *
 * 즉 커널이 "브리지 창을 설정한다" 고 믿고 하는 일이, 실제로는 Marvell 의
 * mbus 주소 디코딩 창을 여는 일로 바뀐다. 그 번역이
 * mvebu_pcie_handle_iobase_change() / _membase_change() 에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DT 에 "marvell,armada-xp-pcie" 같은 compatible 이 있으면 플랫폼 버스가
 * 이 드라이버를 붙인다.
 *
 *   platform_driver -> mvebu_pcie_probe()
 *     -> mvebu_pcie_parse_request_resources()  자원 목록 준비
 *     -> 포트마다 mvebu_pcie_parse_port()      DT 에서 포트 하나를 읽는다
 *     -> mvebu_pcie_powerup()                  클럭·리셋 해제
 *     -> mvebu_pcie_setup_hw()                 RC 모드, 창 초기화, INTx 언마스크
 *     -> mvebu_pci_bridge_emul_init()          가짜 브리지를 세운다
 *     -> mvebu_pcie_init_irq_domain()          INTx 용 irq_domain
 *     -> pci_host_probe()                      PCI 코어에 열거를 넘긴다
 *
 * 열거 중 PCI 코어가 config 를 읽고 쓸 때마다 두 갈래로 갈린다.
 *   루트 버스(가짜 브리지)  -> mvebu_pcie_rd_conf/wr_conf
 *                              -> pci_bridge_emul_conf_read/write
 *                                 -> 이 파일의 여섯 콜백
 *   그 아래 실제 장치       -> mvebu_pcie_child_rd_conf/wr_conf
 *                              -> PCIE_CONF_ADDR_OFF / PCIE_CONF_DATA_OFF
 *                                 간접 창으로 실제 config 접근
 *
 * 실행 컨텍스트: probe/resume 과 config 접근 콜백은 프로세스 컨텍스트다.
 * INTx 체인 핸들러 mvebu_pcie_irq_handler() 와 irq_chip 콜백은 인터럽트
 * 컨텍스트에서 돌며 raw_spinlock 인 port->irq_lock 으로 보호된다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c 의 pci_host_probe(), drivers/pci/access.c 가
 *   pci_lock 을 쥔 채 부르는 config 콜백.
 * 옆쪽(핵심): ../pci-bridge-emul.h 의 struct pci_bridge_emul 과
 *   struct pci_bridge_emul_ops, pci_bridge_emul_init(),
 *   pci_bridge_emul_conf_read/write(). 이 파일이 그 기반 위에 얹혀 있다.
 * 아래쪽: linux/mbus.h 의 mvebu_mbus_add_window_remap_by_id() 와
 *   mvebu_mbus_del_window() — Marvell 고유의 주소 디코딩 창 API 다.
 *   그 밖에 클럭, GPIO(리셋), DT 파서, irq_domain.
 * 공유 상태: struct mvebu_pcie 가 컨트롤러 전체를, struct mvebu_pcie_port 가
 *   포트 하나를 담는다. 포트마다 독립적이며, 포트 안의 memwin/iowin 이
 *   현재 열려 있는 mbus 창을 기억해 두어 창을 바꿀 때 먼저 지우는 데 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * mvebu_pcie_probe()          : 진입점. 포트들을 세우고 PCI 코어에 넘긴다.
 * mvebu_pci_bridge_emul_init(): 가짜 브리지를 만들고 여섯 콜백을 건다.
 *                               이 파일을 읽을 때 가장 먼저 볼 함수다.
 * mvebu_pci_bridge_emul_base_conf_read/write   : 표준 헤더 영역 콜백.
 * mvebu_pci_bridge_emul_pcie_conf_read/write   : PCIe capability 영역 콜백.
 * mvebu_pci_bridge_emul_ext_conf_read/write    : 확장(AER) 영역 콜백.
 * mvebu_pcie_handle_iobase_change() / _membase_change()
 *                             : 브리지 창 설정을 mbus 창으로 번역한다.
 *                               에뮬레이션과 실제 하드웨어가 만나는 지점이다.
 * mvebu_pcie_set_window()     : mbus 창 하나를 실제로 열고 닫는다.
 * mvebu_pcie_child_rd_conf/wr_conf : 아래 장치의 진짜 config 접근.
 * mvebu_pcie_irq_handler()    : INTx 체인 핸들러.
 * struct mvebu_pcie           : 컨트롤러 전체(포트 배열과 주소 공간).
 * struct mvebu_pcie_port      : 포트 하나의 모든 상태.
 * struct mvebu_pcie_window    : 현재 열려 있는 mbus 창 하나의 기억.
 *
 * === NVMe 관점 ===
 * drivers/nvme/ 에서 이 파일의 심볼을 쓰는 곳은 없다(전수 grep 확인).
 * 이 드라이버는 버스를 만드는 쪽이고 NVMe 는 그 위에 열거되는 장치라
 * 계층이 다르다. 다만 이 파일의 브리지 에뮬레이션은 NVMe 에도 그대로
 * 영향을 준다 — Armada 보드에 NVMe SSD 를 붙이면 커널이 그 SSD 의 BAR 를
 * 배정하려고 "브리지 메모리 창" 을 설정하는데, 그 쓰기가 여기서 mbus 창
 * 설정으로 번역되어야 비로소 CPU 가 SSD 의 레지스터에 닿는다.
 * 그 경로에 NVMe 에 특화된 처리는 없고 모든 PCIe 장치에 똑같이 적용된다.
 */

/* [한국어] kernel.h — round_up, max_t, __ffs64 등. 창 크기·정렬 계산이 많은 파일이다. */
#include <linux/kernel.h>
/* [한국어] module.h — 모듈 메타데이터와 module_platform_driver(). */
#include <linux/module.h>
/* [한국어] pci.h — PCI 표준 상수(PCI_EXP_ 계열, PCI_BRIDGE_CTL_ 계열), pci_ops,
 * pci_host_probe(). 브리지 에뮬레이션이 이 상수들을 그대로 쓴다. */
#include <linux/pci.h>
/* [한국어] bitfield.h — FIELD_PREP / FIELD_GET. 슬롯 용량 필드 조립에 쓴다. */
#include <linux/bitfield.h>
/* [한국어] clk.h — 포트 클럭 제어. */
#include <linux/clk.h>
/* [한국어] delay.h — udelay / msleep. 리셋 해제 지연에 쓴다. */
#include <linux/delay.h>
/* [한국어] gpio/consumer.h — gpiod_set_value_cansleep(). 리셋 GPIO 를 다룬다. */
#include <linux/gpio/consumer.h>
/* [한국어] init.h — 초기화 섹션 매크로. */
#include <linux/init.h>
/* [한국어] irqchip/chained_irq.h — chained_irq_enter/exit. INTx 체인 핸들러의 규약이다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irqdomain.h — INTx 용 irq_domain 생성과 매핑. */
#include <linux/irqdomain.h>
/* [한국어] mbus.h — 이 드라이버의 핵심 의존이다. mvebu_mbus_add_window_remap_by_id(),
 * mvebu_mbus_del_window(), mv_mbus_dram_info() 가 여기서 온다. 브리지 창
 * 설정이 결국 이 API 호출로 번역된다. */
#include <linux/mbus.h>
/* [한국어] slab.h — 슬랩 할당자. */
#include <linux/slab.h>
/* [한국어] platform_device.h — 플랫폼 드라이버 뼈대. */
#include <linux/platform_device.h>
/* [한국어] of_address.h — of_address_to_resource(), of_pci_range 파서. */
#include <linux/of_address.h>
/* [한국어] of_irq.h — DT 인터럽트 헬퍼. */
#include <linux/of_irq.h>
/* [한국어] of_pci.h — of_pci_get_devfn(), of_pci_get_slot_power_limit(). */
#include <linux/of_pci.h>
/* [한국어] of_platform.h — DT 플랫폼 헬퍼. */
#include <linux/of_platform.h>

/* [한국어] ../pci.h — PCI 코어 내부 선언. */
#include "../pci.h"
/* [한국어] ../pci-bridge-emul.h — 이 파일의 존재 이유가 담긴 헤더다.
 * struct pci_bridge_emul, struct pci_bridge_emul_ops, pci_bridge_emul_init(),
 * pci_bridge_emul_conf_read/write(), PCI_BRIDGE_EMUL_HANDLED 등이 여기 있다.
 * 브리지가 없는 하드웨어에 브리지를 만들어 주는 공용 기반이다. */
#include "../pci-bridge-emul.h"

/*
 * PCIe unit register offsets.
 */
/* [한국어] 장치 ID 레지스터. 브리지 에뮬레이션이 가짜 브리지의 vendor/device ID 를
 * 여기서 읽어 채운다 — 커널이 보는 브리지가 진짜 하드웨어의 신원을 갖는다. */
#define PCIE_DEV_ID_OFF		0x0000
/* [한국어] Command 레지스터. IO/MEM/BusMaster 활성 비트가 들어 있다. */
#define PCIE_CMD_OFF		0x0004
/* [한국어] Device/Revision 레지스터. 하위 바이트가 revision 이다. */
#define PCIE_DEV_REV_OFF	0x0008
/* [한국어] BAR n 의 하위 워드. n 을 3비트 밀어 8바이트 간격을 만든다.
 * 루트 컴플렉스 모드에서는 자기 BAR 가 필요 없어 초기화 때 지운다. */
#define PCIE_BAR_LO_OFF(n)	(0x0010 + ((n) << 3))
/* [한국어] BAR n 의 상위 워드. */
#define PCIE_BAR_HI_OFF(n)	(0x0014 + ((n) << 3))
/* [한국어] 서브시스템 ID 레지스터. 가짜 브리지의 subsystem 신원으로 쓰인다. */
#define PCIE_SSDEV_ID_OFF	0x002c
/* [한국어] PCIe capability 의 기준 오프셋. 브리지 에뮬레이션의 pcie_conf 콜백들이
 * 이 값에 표준 오프셋을 더해 실제 레지스터를 읽고 쓴다. */
#define PCIE_CAP_PCIEXP		0x0060
/* [한국어] AER(확장 오류 보고) capability 의 기준 오프셋. ext_conf 콜백들이 쓴다. */
#define PCIE_CAP_PCIERR_OFF	0x0100
/* [한국어] BAR n 의 제어 레지스터. n 이 1부터라 (n-1) 을 쓴다. */
#define PCIE_BAR_CTRL_OFF(n)	(0x1804 + (((n) - 1) * 4))
/* [한국어] 주소 창 0~4 의 제어 레지스터. 창 하나가 16바이트 간격이다. */
#define PCIE_WIN04_CTRL_OFF(n)	(0x1820 + ((n) << 4))
/* [한국어] 같은 창의 base 레지스터. */
#define PCIE_WIN04_BASE_OFF(n)	(0x1824 + ((n) << 4))
/* [한국어] 같은 창의 remap 레지스터. 안쪽 창에서는 쓰지 않고 바깥 창에서만 쓴다. */
#define PCIE_WIN04_REMAP_OFF(n)	(0x182c + ((n) << 4))
/* [한국어] 창 5 는 앞의 다섯과 오프셋 규칙이 달라 따로 정의한다. */
#define PCIE_WIN5_CTRL_OFF	0x1880
/* [한국어] 창 5 의 base. */
#define PCIE_WIN5_BASE_OFF	0x1884
/* [한국어] 창 5 의 remap. */
#define PCIE_WIN5_REMAP_OFF	0x188c
/* [한국어] config 접근의 주소 레지스터. 이 컨트롤러에 ECAM 이 없어,
 * 여기에 BDF 와 오프셋을 써 두고 아래 데이터 레지스터로 주고받는다. */
#define PCIE_CONF_ADDR_OFF	0x18f8
/* [한국어] 주소 레지스터의 활성 비트(31번). 이것이 서야 접근이 일어난다. */
#define  PCIE_CONF_ADDR_EN		0x80000000
/* [한국어] 확장 config 오프셋을 두 조각으로 나눠 담는다. 하위 8비트(0xfc, 워드 정렬)와
 * 확장 영역 비트(0xf00)를 16비트 밀어 상위에 얹는다. 표준 256바이트를 넘는
 * 확장 config 를 지원하기 위한 배치다. */
#define  PCIE_CONF_REG(r)		((((r) & 0xf00) << 16) | ((r) & 0xfc))
/* [한국어] 버스 번호를 16비트 자리로. 0xff 로 먼저 자르는 것은 인자가 넘쳐도
 * 이웃 필드를 침범하지 않게 하려는 것이다. */
#define  PCIE_CONF_BUS(b)		(((b) & 0xff) << 16)
/* [한국어] 장치 번호를 11비트 자리로. PCI 장치 번호가 5비트라 0x1f 로 자른다. */
#define  PCIE_CONF_DEV(d)		(((d) & 0x1f) << 11)
/* [한국어] 기능 번호를 8비트 자리로. 3비트다. */
#define  PCIE_CONF_FUNC(f)		(((f) & 0x7) << 8)
/*
 * [한국어] 위 넷을 한데 조립해 주소 레지스터에 쓸 값을 만든다.
 *
 * 첫 줄이 버스와 장치 번호, 둘째 줄이 기능 번호와 레지스터 오프셋,
 * 마지막 줄이 활성 비트다. 이 값을 주소 레지스터에 쓴 뒤
 * PCIE_CONF_DATA_OFF 를 읽고 써서 config 접근이 완성된다.
 */
#define  PCIE_CONF_ADDR(bus, devfn, where) \
	(PCIE_CONF_BUS(bus) | PCIE_CONF_DEV(PCI_SLOT(devfn))    | \
	 PCIE_CONF_FUNC(PCI_FUNC(devfn)) | PCIE_CONF_REG(where) | \
	 PCIE_CONF_ADDR_EN)
/* [한국어] config 접근의 데이터 레지스터. 주소를 세운 뒤 여기를 읽고 쓴다. */
#define PCIE_CONF_DATA_OFF	0x18fc
/* [한국어] 인터럽트 원인 레지스터. 어떤 사건이 있었는지를 담는다. */
#define PCIE_INT_CAUSE_OFF	0x1900
/* [한국어] 인터럽트 언마스크 레지스터. 비트를 세워야 그 인터럽트가 올라온다. */
#define PCIE_INT_UNMASK_OFF	0x1910
/* [한국어] INTx i 번의 비트(24+i). INTA~INTD 가 24~27번에 연속 배치된다. */
#define  PCIE_INT_INTX(i)		BIT(24+i)
/* [한국어] PM_PME 인터럽트 비트(28번). 이 파일 안에는 사용처가 없다(전수 grep 확인). */
#define  PCIE_INT_PM_PME		BIT(28)
/* [한국어] 모든 비트를 덮는 마스크. 초기화와 제거 때 인터럽트를 통째로 지우거나
 * 막는 데 쓴다. */
#define  PCIE_INT_ALL_MASK		GENMASK(31, 0)
/* [한국어] 컨트롤러 제어 레지스터. 모드와 리셋이 여기 있다. */
#define PCIE_CTRL_OFF		0x1a00
/* [한국어] x1 모드 비트. 이 파일 안에는 사용처가 없다. */
#define  PCIE_CTRL_X1_MODE		0x0001
/* [한국어] 루트 컴플렉스 모드 비트(1번). mvebu_pcie_setup_hw() 가 이 비트를 세워
 * 포트를 루트 컴플렉스로 만든다. */
#define  PCIE_CTRL_RC_MODE		BIT(1)
/* [한국어] 마스터 핫 리셋 비트(24번). 브리지 에뮬레이션이 커널의
 * PCI_BRIDGE_CTL_BUS_RESET 요청을 이 비트로 옮긴다 — 가짜 브리지의
 * 리셋 지시가 실제 링크 리셋이 되는 지점이다. */
#define  PCIE_CTRL_MASTER_HOT_RESET	BIT(24)
/* [한국어] 상태 레지스터. 로컬 버스·장치 번호와 링크 상태가 함께 들어 있다. */
#define PCIE_STAT_OFF		0x1a04
/* [한국어] 로컬 버스 번호 필드(0xff00). 브리지 에뮬레이션이 secondary bus 번호를
 * 이 필드에서 읽고 여기에 쓴다. */
#define  PCIE_STAT_BUS                  0xff00
/* [한국어] 로컬 장치 번호 필드(0x1f0000). */
#define  PCIE_STAT_DEV                  0x1f0000
/* [한국어] 링크 다운 비트(0번). mvebu_pcie_link_up() 이 이 비트를 부정해 판정한다. */
#define  PCIE_STAT_LINK_DOWN		BIT(0)
/* [한국어] 슬롯 전력 제한 레지스터. DT 의 슬롯 전력 값이 여기로 내려간다. */
#define PCIE_SSPL_OFF		0x1a0c
/* [한국어] 전력 값 필드의 시작 비트(0). */
#define  PCIE_SSPL_VALUE_SHIFT		0
/* [한국어] 그 필드의 마스크(7:0). */
#define  PCIE_SSPL_VALUE_MASK		GENMASK(7, 0)
/* [한국어] 전력 배율 필드의 시작 비트(8). */
#define  PCIE_SSPL_SCALE_SHIFT		8
/* [한국어] 그 필드의 마스크(9:8). PCIe 규격의 Slot Power Limit Scale 과 같은 인코딩이다. */
#define  PCIE_SSPL_SCALE_MASK		GENMASK(9, 8)
/* [한국어] 슬롯 전력 제한 활성 비트(16번). 브리지 에뮬레이션의 SLTCTL 콜백이
 * 이 비트를 읽고 써서 ASPL_DISABLE 를 흉내 낸다. */
#define  PCIE_SSPL_ENABLE		BIT(16)
/* [한국어] 루트 상태 레지스터. 브리지 에뮬레이션의 PCI_EXP_RTSTA 가 그대로 대응한다. */
#define PCIE_RC_RTSTA		0x1a14
/* [한국어] 디버그 제어 레지스터. */
#define PCIE_DEBUG_CTRL         0x1a60
/* [한국어] 소프트 리셋 비트(20번). 이 파일 안에는 사용처가 없다. */
#define  PCIE_DEBUG_SOFT_RESET		BIT(20)

/* [한국어] 아래 struct mvebu_pcie 가 이 타입의 포인터를 갖고, 정의는 그 뒤에 나오므로
 * 앞선 선언이 필요하다. */
struct mvebu_pcie_port;

/* Structure representing all PCIe interfaces */
/* [한국어] 컨트롤러 전체를 담는 구조체. 포트 배열과 공유 주소 공간을 갖는다. */
struct mvebu_pcie {
	/* [한국어] 이 드라이버가 붙은 플랫폼 장치.
	 * 설정자: mvebu_pcie_probe().
	 * 읽는 자: 로그(dev)와 자원 요청 전반.
	 * 값 범위: 유효한 포인터.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	struct platform_device *pdev;
	/* [한국어] 포트 배열. 포트 하나가 가짜 브리지 하나에 대응한다.
	 * 설정자: probe 가 DT 자식 수만큼 devm 으로 할당해 채운다.
	 * 읽는 자: 거의 모든 함수가 포트를 훑거나 인덱스로 접근한다.
	 * 값 범위: nports 개짜리 배열.
	 * 동기화: probe 이후 배열 자체는 바뀌지 않는다. */
	struct mvebu_pcie_port *ports;
	/* [한국어] I/O 공간의 CPU 물리 주소 구간.
	 * 설정자: mvebu_pcie_parse_request_resources().
	 * 읽는 자: 자원 목록 등록과 창 계산.
	 * 값 범위: I/O 를 쓰지 않는 보드에서는 채워지지 않는다.
	 * 동기화: probe 에서만 설정된다. */
	struct resource io;
	/* [한국어] 같은 I/O 공간을 PCI 쪽 I/O 번호로 본 구간.
	 * 설정자/읽는 자: 위와 같다.
	 * 값 범위: 보통 0 부터 시작하는 작은 구간.
	 * 동기화: probe 에서만 설정된다.
	 * io 와 둘로 나뉜 이유는 CPU 물리 주소와 PCI I/O 번호가 다르기 때문이며,
	 * 그 차이를 pci_add_resource_offset() 으로 코어에 알려 준다. */
	struct resource realio;
	/* [한국어] 메모리 공간 구간. I/O 와 달리 필수다.
	 * 설정자: mvebu_pcie_parse_request_resources().
	 * 읽는 자: 자원 목록 등록.
	 * 값 범위: DT 의 ranges 에서 온 유효한 구간.
	 * 동기화: probe 에서만 설정된다. */
	struct resource mem;
	/* [한국어] 실제로 쓸 수 있는 포트 수.
	 * 설정자: probe 가 parse_port 성공 횟수로 센다. DT 자식 수보다 적을 수 있다 —
	 *   disabled 이거나 필수 속성이 없는 노드를 건너뛰기 때문이다.
	 * 읽는 자: 포트를 훑는 모든 루프의 상한.
	 * 동기화: probe 에서만 설정된다. */
	int nports;
};

/* [한국어] 현재 열려 있는 mbus 창 하나를 기억하는 구조체. 창을 바꿀 때
 * "지금 무엇이 열려 있는가" 를 알아야 먼저 지울 수 있어서 둔다. */
struct mvebu_pcie_window {
	/* [한국어] 창의 CPU 쪽 시작 주소.
	 * 설정자/읽는 자: mvebu_pcie_set_window().
	 * 값 범위: 물리 주소. size 가 0 이면 의미 없다.
	 * 동기화: config 쓰기 경로에서만 바뀌며 pci_lock 이 직렬화한다. */
	phys_addr_t base;
	/* [한국어] 창이 가리킬 PCI 쪽 주소. I/O 창에서만 의미가 있고 메모리 창은
	 * MVEBU_MBUS_NO_REMAP 을 쓴다.
	 * 설정자/읽는 자/동기화: base 와 같다. */
	phys_addr_t remap;
	/* [한국어] 창 크기. 0 이면 창이 닫혀 있다는 뜻이다.
	 * 설정자/읽는 자/동기화: base 와 같다. */
	size_t size;
};

/* Structure representing one PCIe interface */
/* [한국어] 포트 하나의 모든 상태. 이 파일의 중심 구조체다. */
struct mvebu_pcie_port {
	/* [한국어] 로그에 쓰는 이름("pcieA.B" 형식).
	 * 설정자: mvebu_pcie_parse_port() 가 devm_kasprintf 로 만든다.
	 * 읽는 자: dev_info/dev_err 메시지 전반.
	 * 값 범위: 유효한 문자열.
	 * 동기화: probe 에서만 설정된다. */
	char *name;
	/* [한국어] 이 포트의 레지스터 블록 가상 주소. 이 파일의 모든 오프셋이 여기에 더해진다.
	 * 설정자: mvebu_pcie_map_registers().
	 * 읽는 자: mvebu_readl/writel 을 통해 사실상 모든 코드.
	 * 값 범위: 유효한 __iomem 포인터. NULL 이면 초기화되지 못한 포트라
	 *   suspend/resume 이 건너뛴다.
	 * 동기화: probe 에서 한 번 설정된 뒤 읽기 전용이다. */
	void __iomem *base;
	/* [한국어] DT 의 marvell,pcie-port 값. 이름 조립과 슬롯 번호에 쓴다.
	 * 설정자: mvebu_pcie_parse_port().
	 * 읽는 자: 이름 생성, 브리지 에뮬레이션의 물리 슬롯 번호(port+1).
	 * 동기화: probe 에서만 설정된다. */
	u32 port;
	/* [한국어] DT 의 marvell,pcie-lane 값. 이름 조립에 쓴다.
	 * 설정자/읽는 자/동기화: port 와 같다. */
	u32 lane;
	/* [한국어] num-lanes 가 4 인가.
	 * 설정자: mvebu_pcie_parse_port().
	 * 읽는 자: 링크 설정 경로.
	 * 값 범위: true/false.
	 * 동기화: probe 에서만 설정된다. */
	bool is_x4;
	/* [한국어] 루트 버스에서 이 포트가 차지할 devfn.
	 * 설정자: parse_port 가 of_pci_get_devfn() 으로 reg 에서 뽑는다.
	 * 읽는 자: mvebu_pcie_find_port() 가 루트 버스 조회에 쓴다.
	 * 값 범위: 유효한 devfn. 포트마다 서로 달라야 한다.
	 * 동기화: probe 에서만 설정된다. */
	int devfn;
	/* [한국어] 메모리 창의 mbus 타깃 ID.
	 * 설정자: parse_port 가 mvebu_get_tgt_attr() 로 DT ranges 에서 찾는다.
	 * 읽는 자: mvebu_pcie_handle_membase_change() 가 창을 열 때.
	 * 값 범위: mbus 타깃 번호. 못 찾으면 -1 이지만 메모리는 필수라 실패 처리된다.
	 * 동기화: probe 에서만 설정된다. */
	unsigned int mem_target;
	/* [한국어] 메모리 창의 mbus 속성.
	 * 설정자/읽는 자/동기화: mem_target 과 같다. */
	unsigned int mem_attr;
	/* [한국어] I/O 창의 mbus 타깃 ID.
	 * 설정자: parse_port. 없으면 -1 로 남는다.
	 * 읽는 자: mvebu_has_ioport() 의 판정 기준이자 창 설정의 인자.
	 * 값 범위: 유효한 타깃 번호 또는 -1(I/O 미지원).
	 * 동기화: probe 에서만 설정된다. */
	unsigned int io_target;
	/* [한국어] I/O 창의 mbus 속성.
	 * 설정자/읽는 자/동기화: io_target 과 같다. */
	unsigned int io_attr;
	/* [한국어] 포트 클럭.
	 * 설정자: parse_port 가 얻고 devm 액션으로 반납을 예약한다.
	 * 읽는 자: mvebu_pcie_powerup/powerdown.
	 * 값 범위: 유효한 클럭 포인터.
	 * 동기화: probe 에서만 설정된다. */
	struct clk *clk;
	/* [한국어] 리셋 GPIO(선택).
	 * 설정자: parse_port 가 reset-gpios 에서 얻는다. 없으면 NULL.
	 * 읽는 자: powerup/powerdown.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: probe 에서만 설정된다. */
	struct gpio_desc *reset_gpio;
	/* [한국어] 그 GPIO 에 붙일 이름 문자열.
	 * 설정자: parse_port 가 devm_kasprintf 로 만든다.
	 * 읽는 자: gpiod 요청 시 라벨로 쓰인다.
	 * 동기화: probe 에서만 설정된다. */
	char *reset_name;
	/* [한국어] 이 포트의 가짜 브리지. 포인터가 아니라 값으로 박아 두어 별도 할당이 없다.
	 * 설정자: mvebu_pci_bridge_emul_init() 이 채우고 pci_bridge_emul_init() 에 넘긴다.
	 * 읽는 자: 여섯 콜백이 bridge->data 로 포트를 되찾고, conf 버퍼를 직접 읽는다.
	 * 값 범위: 초기화된 pci_bridge_emul 구조체.
	 * 동기화: config 접근 경로에서 쓰이며 pci_lock 이 직렬화한다.
	 * 이 필드가 이 드라이버의 핵심이다 — 하드웨어에 없는 브리지가 여기 산다. */
	struct pci_bridge_emul bridge;
	/* [한국어] 이 포트의 DT 노드.
	 * 설정자: mvebu_pcie_parse_port().
	 * 읽는 자: powerup 이 reset-delay-us 를 읽을 때.
	 * 동기화: probe 에서만 설정된다. */
	struct device_node *dn;
	/* [한국어] 이 포트를 품은 컨트롤러로 되돌아가는 포인터.
	 * 설정자: probe.
	 * 읽는 자: 로그(pdev->dev)와 주소 공간 참조.
	 * 동기화: probe 에서만 설정된다. */
	struct mvebu_pcie *pcie;
	/* [한국어] 현재 열려 있는 메모리 mbus 창의 기억.
	 * 설정자/읽는 자: mvebu_pcie_set_window() 가 비교하고 갱신한다.
	 * 값 범위: 위 struct mvebu_pcie_window 참조.
	 * 동기화: config 쓰기 경로에서만 바뀌며 pci_lock 이 직렬화한다. */
	struct mvebu_pcie_window memwin;
	/* [한국어] 현재 열려 있는 I/O mbus 창의 기억.
	 * 설정자/읽는 자/값 범위/동기화: memwin 과 같다. */
	struct mvebu_pcie_window iowin;
	/* [한국어] 절전 전에 저장해 둔 PCIE_STAT_OFF 값.
	 * 설정자: mvebu_pcie_suspend().
	 * 읽는 자: mvebu_pcie_resume() 이 되쓴다.
	 * 값 범위: 버스·장치 번호가 담긴 32비트 값.
	 * 동기화: 절전/재개 경로에서만 쓰이며 PM 코어가 직렬화한다. */
	u32 saved_pcie_stat;
	/* [한국어] 이 포트의 레지스터 블록 자원(물리 주소와 크기).
	 * 설정자: mvebu_pcie_map_registers().
	 * 읽는 자: /proc/iomem 표시와 물리 주소가 필요한 곳.
	 * 동기화: probe 에서만 설정된다. */
	struct resource regs;
	/* [한국어] DT 에서 읽은 슬롯 전력 제한 값.
	 * 설정자: parse_port 가 of_pci_get_slot_power_limit() 으로 읽는다.
	 * 읽는 자: bridge_emul_init 이 슬롯 용량에 넣고, SLTCTL 콜백이
	 *   "DT 지정이 있었는가" 를 이 값이 0 인지로 판정한다.
	 * 값 범위: 0(지정 없음) 또는 PCIe 규격의 8비트 값.
	 * 동기화: probe 에서만 설정된다. */
	u8 slot_power_limit_value;
	/* [한국어] 같은 전력 제한의 배율.
	 * 설정자/읽는 자/동기화: value 와 같다. */
	u8 slot_power_limit_scale;
	/* [한국어] 이 포트의 INTx irq_domain.
	 * 설정자: mvebu_pcie_init_irq_domain().
	 * 읽는 자: 체인 핸들러가 generic_handle_domain_irq() 에 넘기고,
	 *   map_irq 가 irq_create_mapping() 에 쓴다.
	 * 값 범위: 유효한 포인터 또는 NULL(INTx 미사용 포트).
	 * 동기화: probe 에서 설정된 뒤 읽기 전용이다. */
	struct irq_domain *intx_irq_domain;
	/* [한국어] INTx 언마스크 레지스터 갱신을 보호하는 raw spinlock.
	 * 설정자: init_irq_domain 이 초기화한다.
	 * 읽는 자: mvebu_pcie_intx_irq_mask/unmask.
	 * 값 범위: raw spinlock.
	 * 동기화: 이 경로가 인터럽트 문맥에서 불려 잠들 수 없으므로 raw 계열이어야 한다.
	 *   네 INTx 의 활성 비트가 한 워드에 모여 있어 읽고-고쳐-쓰기를 보호해야 한다. */
	raw_spinlock_t irq_lock;
	/* [한국어] 이 포트의 상위(체인) IRQ 번호.
	 * 설정자: probe 가 DT 에서 얻는다.
	 * 읽는 자: 체인 핸들러 등록과 remove 의 해제.
	 * 값 범위: 유효한 virq 또는 0/음수(INTx 미사용).
	 * 동기화: probe 에서만 설정된다. */
	int intx_irq;
};

/* [한국어]
 * mvebu_writel - 포트의 레지스터에 32비트를 쓴다
 *
 * @port: 대상 포트.   @val: 쓸 값.   @reg: 포트 기준 오프셋.
 * @return: 없음.
 *
 * port->base 에 오프셋을 더해 writel 한다. 인자 순서가 (값, 오프셋)이라
 * writel 과 같다.
 *
 * 포트마다 base 가 다르므로 이 래퍼가 포트 문맥을 함께 들고 다니는 셈이다.
 * 이 파일의 모든 하드웨어 접근이 이 함수와 아래 mvebu_readl() 을 거친다.
 *
 * 실행 컨텍스트: 제약 없음. 인터럽트 문맥에서도 불린다.
 *
 * 호출 체인:  이 파일 전반 → [이 함수] → writel()
 */
static inline void mvebu_writel(struct mvebu_pcie_port *port, u32 val, u32 reg)
{
	/* [한국어] 포트 기준 주소에 오프셋을 더해 쓴다. 인자 순서가 (값, 오프셋)이라
	 * writel 과 같다. */
	writel(val, port->base + reg);
}

/* [한국어]
 * mvebu_readl - 포트의 레지스터에서 32비트를 읽는다
 *
 * @port: 대상 포트.   @reg: 포트 기준 오프셋.
 * @return: 읽은 값.
 *
 * 위 쓰기 판의 짝이다. 브리지 에뮬레이션의 read 콜백들이 "가짜 브리지의
 * config 를 읽어 달라" 는 요청을 실제 레지스터 읽기로 바꿀 때 전부 이
 * 함수를 쓴다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  이 파일 전반 → [이 함수] → readl()
 */
static inline u32 mvebu_readl(struct mvebu_pcie_port *port, u32 reg)
{
	/* [한국어] 포트 기준 주소에서 읽는다. */
	return readl(port->base + reg);
}

/* [한국어]
 * mvebu_has_ioport - 이 포트가 I/O 공간을 지원하는가
 *
 * @port: 대상 포트.
 * @return: io_target 이 -1 이 아니면 true.
 *
 * DT 의 ranges 에 I/O 항목이 있어야 io_target/io_attr 이 채워진다.
 * 없으면 mvebu_pcie_parse_port() 가 -1 로 남겨 두고, 이 함수가 그것을 판정한다.
 *
 * I/O 를 쓰지 않는 보드에서 I/O 창을 열려 하면 mbus 에 엉뚱한 창이 생기므로,
 * 창 설정과 브리지 에뮬레이션 양쪽에서 먼저 이 값을 확인한다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  mvebu_pcie_setup_hw() / handle_iobase_change() /
 *             bridge emul 콜백들 → [이 함수]
 */
static inline bool mvebu_has_ioport(struct mvebu_pcie_port *port)
{
	/* [한국어] 둘 다 -1 이 아니어야 I/O 를 쓸 수 있다. mvebu_get_tgt_attr() 이
	 * 못 찾았을 때 -1 로 남겨 두는 것과 짝을 이룬다. */
	return port->io_target != -1 && port->io_attr != -1;
}

/* [한국어]
 * mvebu_pcie_link_up - 링크가 올라와 있는지 본다
 *
 * @port: 대상 포트.
 * @return: 링크가 살아 있으면 true.
 *
 * PCIE_STAT_OFF 의 PCIE_STAT_LINK_DOWN 비트를 읽어 부정한다. 비트 이름이
 * "내려감" 이므로 그것이 서 있지 않아야 링크가 살아 있는 것이다.
 *
 * 브리지 에뮬레이션에서 이 값이 중요하다. PCI_EXP_LNKCTL 읽기 콜백이
 * 이 함수의 결과를 PCI_EXP_LNKSTA_DLLLA(Data Link Layer Link Active)
 * 비트로 바꿔 커널에 알린다 — 하드웨어에 그 비트가 없어 소프트웨어가
 * 만들어 주는 것이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  bridge emul 콜백 / mvebu_pcie_setup_hw() 등 → [이 함수] → mvebu_readl()
 */
static bool mvebu_pcie_link_up(struct mvebu_pcie_port *port)
{
	return !(mvebu_readl(port, PCIE_STAT_OFF) & PCIE_STAT_LINK_DOWN);
}

/* [한국어]
 * mvebu_pcie_get_local_bus_nr - 이 포트에 배정된 로컬 버스 번호를 읽는다
 *
 * @port: 대상 포트.
 * @return: 버스 번호(0~255).
 *
 * PCIE_STAT_OFF 의 PCIE_STAT_BUS 필드(0xff00)를 떼어 8비트 밀어 낸다.
 *
 * 브리지 에뮬레이션에서 쓰인다. 커널이 가짜 브리지의 PCI_PRIMARY_BUS
 * 워드를 읽으면, 그중 secondary bus 자리만 이 함수가 읽은 실제 값으로
 * 채워 주고 나머지 바이트는 에뮬레이트 버퍼에서 가져온다 — 상류 주석이
 * 그 사정을 적어 두었다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  mvebu_pci_bridge_emul_base_conf_read() 등 → [이 함수] → mvebu_readl()
 */
static u8 mvebu_pcie_get_local_bus_nr(struct mvebu_pcie_port *port)
{
	return (mvebu_readl(port, PCIE_STAT_OFF) & PCIE_STAT_BUS) >> 8;
}

/* [한국어]
 * mvebu_pcie_set_local_bus_nr - 이 포트의 로컬 버스 번호를 바꾼다
 *
 * @port: 대상 포트.   @nr: 새 버스 번호.
 * @return: 없음.
 *
 * PCIE_STAT_OFF 의 버스 번호 필드만 읽고-고쳐-쓴다. 다른 비트(장치 번호,
 * 링크 상태)를 보존해야 하므로 통째로 쓸 수 없다.
 *
 * 커널이 가짜 브리지의 secondary bus 번호를 쓰면 그 값이 여기까지 내려와
 * 하드웨어에 반영된다. 그래야 이 포트가 그 번호로 오는 config 트랜잭션을
 * 자기 것으로 받아들인다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 쓰기 경로).
 *
 * 호출 체인:  mvebu_pci_bridge_emul_base_conf_write() → [이 함수] → mvebu_readl/writel()
 */
static void mvebu_pcie_set_local_bus_nr(struct mvebu_pcie_port *port, int nr)
{
	u32 stat;

	stat = mvebu_readl(port, PCIE_STAT_OFF);
	/* [한국어] 버스 번호 필드만 지운다. 같은 워드의 장치 번호와 링크 상태를 보존해야 한다. */
	stat &= ~PCIE_STAT_BUS;
	/* [한국어] 새 번호를 8비트 자리로 밀어 얹는다. */
	stat |= nr << 8;
	/* [한국어] 고친 워드를 되쓴다. */
	mvebu_writel(port, stat, PCIE_STAT_OFF);
}

/* [한국어]
 * mvebu_pcie_set_local_dev_nr - 이 포트의 로컬 장치 번호를 바꾼다
 *
 * @port: 대상 포트.   @nr: 새 장치 번호.
 * @return: 없음.
 *
 * 위 버스 번호 판과 같은 방식으로 PCIE_STAT_DEV 필드(0x1f0000)만 고친다.
 * 장치 번호는 5비트라 마스크가 0x1f 다.
 *
 * mvebu_pcie_setup_hw() 가 초기화 때 한 번 불러 장치 번호를 0 으로 맞춘다.
 * 루트 컴플렉스가 자기 자신을 devfn 0 으로 보이게 하려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화).
 *
 * 호출 체인:  mvebu_pcie_setup_hw() → [이 함수] → mvebu_readl/writel()
 */
static void mvebu_pcie_set_local_dev_nr(struct mvebu_pcie_port *port, int nr)
{
	u32 stat;

	stat = mvebu_readl(port, PCIE_STAT_OFF);
	/* [한국어] 장치 번호 필드만 지운다. */
	stat &= ~PCIE_STAT_DEV;
	/* [한국어] 새 번호를 16비트 자리로 밀어 얹는다. */
	stat |= nr << 16;
	/* [한국어] 고친 워드를 되쓴다. */
	mvebu_writel(port, stat, PCIE_STAT_OFF);
}

/* [한국어]
 * mvebu_pcie_disable_wins - 포트의 모든 주소 창을 끈다
 *
 * @port: 대상 포트.   @return: 없음.
 *
 * 부트로더가 남긴 창 설정을 백지로 되돌린다. 살아 있는 창이 커널이 만들
 * 창과 겹치면 엉뚱한 주소로 트랜잭션이 나가기 때문이다.
 *
 * 창 0~3 은 PCIE_WIN04_ 계열 레지스터 세 개씩(ctrl/base/remap)을 쓰고,
 * 창 5 는 별도 레지스터(PCIE_WIN5_ 계열)를 쓴다. 하드웨어 배치가 그렇게
 * 나뉘어 있어 루프와 개별 처리가 함께 있다.
 *
 * BAR 레지스터도 함께 지운다. 루트 컴플렉스 모드에서는 자기 BAR 가
 * 필요 없으므로 열려 있으면 오히려 방해가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화).
 *
 * 호출 체인:  mvebu_pcie_setup_hw() → [이 함수] → mvebu_writel()
 */
static void mvebu_pcie_disable_wins(struct mvebu_pcie_port *port)
{
	int i;

	mvebu_writel(port, 0, PCIE_BAR_LO_OFF(0));
	/* [한국어] BAR0 의 상위 워드도 지운다(윗줄에서 하위를 지웠다). 루트 컴플렉스
	 * 모드에서는 자기 BAR 가 필요 없고, 열려 있으면 오히려 방해가 된다. */
	mvebu_writel(port, 0, PCIE_BAR_HI_OFF(0));

	for (i = 1; i < 3; i++) {
		/* [한국어] BAR n 의 제어 레지스터를 끈다. */
		mvebu_writel(port, 0, PCIE_BAR_CTRL_OFF(i));
		/* [한국어] 하위 주소를 지운다. */
		mvebu_writel(port, 0, PCIE_BAR_LO_OFF(i));
		/* [한국어] 상위 주소를 지운다. BAR0 은 위에서 따로 처리했고 여기는 1번부터다 —
		 * PCIE_BAR_CTRL_OFF 가 (n-1) 을 쓰는 것과 맞물린다. */
		mvebu_writel(port, 0, PCIE_BAR_HI_OFF(i));
	/* [한국어] BAR 루프 끝. */
	}

	for (i = 0; i < 5; i++) {
		/* [한국어] 창 n 의 제어를 끈다. 이것만 꺼도 창은 죽지만, */
		mvebu_writel(port, 0, PCIE_WIN04_CTRL_OFF(i));
		/* [한국어] base 와 */
		mvebu_writel(port, 0, PCIE_WIN04_BASE_OFF(i));
		/* [한국어] remap 도 함께 지워 다음에 열 때 옛 값이 섞이지 않게 한다. */
		mvebu_writel(port, 0, PCIE_WIN04_REMAP_OFF(i));
	/* [한국어] 창 0~4 루프 끝. */
	}

	mvebu_writel(port, 0, PCIE_WIN5_CTRL_OFF);
	/* [한국어] 창 5 는 오프셋 규칙이 달라 루프 밖에서 따로 지운다(윗줄이 제어). */
	mvebu_writel(port, 0, PCIE_WIN5_BASE_OFF);
	/* [한국어] 창 5 의 remap 까지 지우면 모든 창이 백지가 된다. */
	mvebu_writel(port, 0, PCIE_WIN5_REMAP_OFF);
/* [한국어] 함수 끝. 이 시점에 포트는 부트로더가 남긴 설정이 모두 지워진 상태다. */
}

/*
 * Setup PCIE BARs and Address Decode Wins:
 * BAR[0] -> internal registers (needed for MSI)
 * BAR[1] -> covers all DRAM banks
 * BAR[2] -> Disabled
 * WIN[0-3] -> DRAM bank[0-3]
 */
/* [한국어]
 * mvebu_pcie_setup_wins - 내부 메모리를 가리키는 안쪽 창을 연다
 *
 * @port: 대상 포트.   @return: 없음.
 *
 * 장치가 DMA 로 시스템 메모리에 닿으려면 그 구간을 덮는 창이 있어야 한다.
 * mbus 의 dram 정보(mv_mbus_dram_info)에서 DRAM 영역 목록을 받아
 * 창을 하나씩 연다.
 *
 * 각 영역마다 base/ctrl 두 레지스터를 채우는데, ctrl 에는 크기 마스크와
 * mbus target/attr, 그리고 활성 비트가 들어간다. 크기를 마스크로 표현하는
 * 방식이라 영역 크기가 2의 거듭제곱이어야 한다.
 *
 * remap 레지스터는 쓰지 않는다 — 안쪽 방향은 주소를 그대로 통과시키기
 * 때문이다. 반대로 바깥 방향 창(mvebu_pcie_set_window)은 remap 을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(초기화).
 *
 * 호출 체인:  mvebu_pcie_setup_hw() → [이 함수] → mvebu_writel()
 */
static void mvebu_pcie_setup_wins(struct mvebu_pcie_port *port)
{
	const struct mbus_dram_target_info *dram;
	u32 size;
	/* [한국어] DRAM 영역 반복자. */
	int i;

	dram = mv_mbus_dram_info();
/* [한국어] mbus 에서 DRAM 영역 목록을 받는다. 이 정보가 있어야 장치의 DMA 가
 * 시스템 메모리에 닿는 안쪽 창을 열 수 있다. */

	/* First, disable and clear BARs and windows. */
	mvebu_pcie_disable_wins(port);

	/* Setup windows for DDR banks.  Count total DDR size on the fly. */
	size = 0;
	for (i = 0; i < dram->num_cs; i++) {
		/* [한국어] 이번 DRAM 영역. */
		const struct mbus_dram_window *cs = dram->cs + i;

		mvebu_writel(port, cs->base & 0xffff0000,
			     /* [한국어] 영역의 시작 주소를 창 base 에 쓴다. */
			     PCIE_WIN04_BASE_OFF(i));
		mvebu_writel(port, 0, PCIE_WIN04_REMAP_OFF(i));
		/* [한국어] 제어 레지스터에는 세 정보를 한 워드로 조립해 넣는다. */
		mvebu_writel(port,
			     ((cs->size - 1) & 0xffff0000) |
			     (cs->mbus_attr << 8) |
			     /* [한국어] 크기 마스크와 mbus 타깃 ID(4비트 밀어), 그리고 활성 비트 1 이다.
			      * 크기를 마스크로 표현하므로 영역 크기가 2의 거듭제곱이어야 한다. */
			     (dram->mbus_dram_target_id << 4) | 1,
			     /* [한국어] 창 n 의 제어 레지스터에 쓴다. remap 은 건드리지 않는데,
			      * 안쪽 방향은 주소를 그대로 통과시키기 때문이다. */
			     PCIE_WIN04_CTRL_OFF(i));

		size += cs->size;
	/* [한국어] DRAM 영역 루프 끝. */
	}

	/* Round up 'size' to the nearest power of two. */
	if ((size & (size - 1)) != 0)
		size = 1 << fls(size);
/* [한국어] 전체 DRAM 크기를 누적해 두었다가(윗줄) BAR1 의 크기로 쓴다. */

	/* Setup BAR[1] to all DRAM banks. */
	mvebu_writel(port, dram->cs[0].base, PCIE_BAR_LO_OFF(1));
	mvebu_writel(port, 0, PCIE_BAR_HI_OFF(1));
	/* [한국어] BAR1 을 열어 그 크기만큼의 안쪽 접근을 받아들이게 한다.
	 * (size-1) 의 상위 16비트가 크기 마스크이고 하위 비트 1 이 활성이다. */
	mvebu_writel(port, ((size - 1) & 0xffff0000) | 1,
		     /* [한국어] BAR1 의 제어 레지스터. 루트 컴플렉스가 DMA 를 받으려면 이 BAR 가 필요하다. */
		     PCIE_BAR_CTRL_OFF(1));

	/*
	 * Point BAR[0] to the device's internal registers.
	 */
	mvebu_writel(port, round_down(port->regs.start, SZ_1M), PCIE_BAR_LO_OFF(0));
	mvebu_writel(port, 0, PCIE_BAR_HI_OFF(0));
}

/* [한국어]
 * mvebu_pcie_setup_hw - 포트를 루트 컴플렉스로 세우고 초기 상태를 만든다
 *
 * @port: 대상 포트.   @return: 없음.
 *
 * 포트 하나의 하드웨어 초기화를 모아 둔 함수다.
 *   1) PCIE_CTRL_OFF 에 RC 모드를 세운다. 이 비트가 이 포트를 루트
 *      컴플렉스로 만든다.
 *   2) Command 레지스터에서 IO/MEM/BusMaster 를 켠다. 켜지 않으면
 *      트랜잭션이 나가지 않는다.
 *   3) 장치 번호를 0 으로 맞춘다.
 *   4) 기존 창을 모두 끄고(disable_wins) 안쪽 창을 새로 연다(setup_wins).
 *   5) INTx 인터럽트 원인을 지우고 마스크를 푼다. 지우는 것이 먼저인 이유는
 *      부트로더가 남긴 대기 인터럽트가 켜자마자 쏟아지는 것을 막기 위해서다.
 *
 * 이 함수가 끝나면 포트는 링크를 맺을 준비가 되고, 그 뒤 브리지 에뮬레이션이
 * 얹힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  mvebu_pcie_probe() → [이 함수] → mvebu_pcie_disable_wins()
 *               → mvebu_pcie_setup_wins() → mvebu_writel()
 */
static void mvebu_pcie_setup_hw(struct mvebu_pcie_port *port)
{
	u32 ctrl, lnkcap, cmd, dev_rev, unmask, sspl;

	/* Setup PCIe controller to Root Complex mode. */
	ctrl = mvebu_readl(port, PCIE_CTRL_OFF);
	ctrl |= PCIE_CTRL_RC_MODE;
	/* [한국어] RC 모드 비트를 세운 값을 쓴다(윗줄에서 조립했다).
	 * 이 한 줄이 포트를 루트 컴플렉스로 만든다. */
	mvebu_writel(port, ctrl, PCIE_CTRL_OFF);

	/*
	 * Set Maximum Link Width to X1 or X4 in Root Port's PCIe Link
	 * Capability register. This register is defined by PCIe specification
	 * as read-only but this mvebu controller has it as read-write and must
	 * be set to number of SerDes PCIe lanes (1 or 4). If this register is
	 * not set correctly then link with endpoint card is not established.
	 */
	lnkcap = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCAP);
	lnkcap &= ~PCI_EXP_LNKCAP_MLW;
	/* [한국어] 링크 능력의 최대 링크 폭 필드를 DT 가 말한 레인 수로 채운다.
	 * is_x4 면 4, 아니면 1 이다. */
	lnkcap |= FIELD_PREP(PCI_EXP_LNKCAP_MLW, port->is_x4 ? 4 : 1);
	/* [한국어] 고친 값을 PCIe capability 의 LNKCAP 에 쓴다. */
	mvebu_writel(port, lnkcap, PCIE_CAP_PCIEXP + PCI_EXP_LNKCAP);

	/* Disable Root Bridge I/O space, memory space and bus mastering. */
	cmd = mvebu_readl(port, PCIE_CMD_OFF);
	cmd &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	/* [한국어] IO/MEM/BusMaster 를 켠 Command 를 쓴다(윗줄에서 조립했다).
	 * 켜지 않으면 이 포트에서 트랜잭션이 나가지 않는다. */
	mvebu_writel(port, cmd, PCIE_CMD_OFF);

	/*
	 * Change Class Code of PCI Bridge device to PCI Bridge (0x6004)
	 * because default value is Memory controller (0x5080).
	 *
	 * Note that this mvebu PCI Bridge does not have compliant Type 1
	 * Configuration Space. Header Type is reported as Type 0 and it
	 * has format of Type 0 config space.
	 *
	 * Moreover Type 0 BAR registers (ranges 0x10 - 0x28 and 0x30 - 0x34)
	 * have the same format in Marvell's specification as in PCIe
	 * specification, but their meaning is totally different and they do
	 * different things: they are aliased into internal mvebu registers
	 * (e.g. PCIE_BAR_LO_OFF) and these should not be changed or
	 * reconfigured by pci device drivers.
	 *
	 * Therefore driver uses emulation of PCI Bridge which emulates
	 * access to configuration space via internal mvebu registers or
	 * emulated configuration buffer. Driver access these PCI Bridge
	 * directly for simplification, but these registers can be accessed
	 * also via standard mvebu way for accessing PCI config space.
	 */
	dev_rev = mvebu_readl(port, PCIE_DEV_REV_OFF);
	dev_rev &= ~0xffffff00;
	/* [한국어] 클래스 코드를 PCI-to-PCI 브리지로 만든다. 8비트 미는 것은 이 레지스터에서
	 * 클래스가 그 자리이기 때문이며, 하위 바이트의 revision 은 보존된다. */
	dev_rev |= PCI_CLASS_BRIDGE_PCI_NORMAL << 8;
	/* [한국어] 고친 값을 되쓴다. 커널이 이 포트를 브리지로 인식하게 하는 설정이다. */
	mvebu_writel(port, dev_rev, PCIE_DEV_REV_OFF);

	/* Point PCIe unit MBUS decode windows to DRAM space. */
	mvebu_pcie_setup_wins(port);

	/*
	 * Program Root Port to automatically send Set_Slot_Power_Limit
	 * PCIe Message when changing status from Dl_Down to Dl_Up and valid
	 * slot power limit was specified.
	 */
	sspl = mvebu_readl(port, PCIE_SSPL_OFF);
	sspl &= ~(PCIE_SSPL_VALUE_MASK | PCIE_SSPL_SCALE_MASK | PCIE_SSPL_ENABLE);
	/* [한국어] DT 에 슬롯 전력 제한이 지정된 경우에만 하드웨어에 반영한다. */
	if (port->slot_power_limit_value) {
		/* [한국어] 전력 값을 자리에 넣고, */
		sspl |= port->slot_power_limit_value << PCIE_SSPL_VALUE_SHIFT;
		/* [한국어] 배율도 넣고, */
		sspl |= port->slot_power_limit_scale << PCIE_SSPL_SCALE_SHIFT;
		/* [한국어] 활성 비트를 세운다. 지정이 없으면 sspl 이 0 인 채로 남아 기능이 꺼진다 —
		 * 브리지 에뮬레이션의 SLTCTL 콜백이 그 두 경우를 구분하는 근거가 된다. */
		sspl |= PCIE_SSPL_ENABLE;
	/* [한국어] 조건 블록 끝. */
	}
	mvebu_writel(port, sspl, PCIE_SSPL_OFF);
/* [한국어] 조립한 값을 슬롯 전력 제한 레지스터에 쓴다. */

	/* Mask all interrupt sources. */
	mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_UNMASK_OFF);

	/* Clear all interrupt causes. */
	mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_CAUSE_OFF);

	/* Check if "intx" interrupt was specified in DT. */
	if (port->intx_irq > 0)
		return;

	/*
	 * Fallback code when "intx" interrupt was not specified in DT:
	 * Unmask all legacy INTx interrupts as driver does not provide a way
	 * for masking and unmasking of individual legacy INTx interrupts.
	 * Legacy INTx are reported via one shared GIC source and therefore
	 * kernel cannot distinguish which individual legacy INTx was triggered.
	 * These interrupts are shared, so it should not cause any issue. Just
	 * performance penalty as every PCIe interrupt handler needs to be
	 * called when some interrupt is triggered.
	 */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);
	unmask |= PCIE_INT_INTX(0) | PCIE_INT_INTX(1) |
		  /* [한국어] INTA~INTD 네 비트를 모두 세운다(윗줄에서 시작했다). */
		  PCIE_INT_INTX(2) | PCIE_INT_INTX(3);
	/* [한국어] 언마스크 레지스터에 써서 INTx 수신을 연다. 바로 앞에서 원인을 지웠으므로
	 * 부트로더가 남긴 대기 인터럽트가 쏟아지지 않는다. */
	mvebu_writel(port, unmask, PCIE_INT_UNMASK_OFF);
}

/* [한국어] 아래 mvebu_pcie_find_port() 의 앞선 선언. 정의는 이 파일 뒤쪽에 있는데,
 * config 접근 콜백들이 그보다 먼저 나와 이 선언이 필요하다. */
static struct mvebu_pcie_port *mvebu_pcie_find_port(struct mvebu_pcie *pcie,
						    struct pci_bus *bus,
						    int devfn);

/* [한국어]
 * mvebu_pcie_child_rd_conf - 루트 버스 아래 실제 장치의 config 를 읽는다
 *
 * @bus, @devfn: 대상 장치.   @where: 오프셋.   @size: 1, 2, 4.   @val: 결과.
 * @return: PCIBIOS_* 코드.
 *
 * 가짜 브리지가 아니라 진짜 장치를 읽는 경로다. 이 컨트롤러에는 ECAM 이
 * 없어 주소·데이터 레지스터 쌍으로 된 간접 창을 쓴다.
 *   1) 포트를 찾는다. 못 찾으면 그 자리에 장치가 없다는 뜻이다.
 *   2) 링크가 없으면 읽어 봐야 응답이 없다.
 *   3) PCIE_CONF_ADDR_OFF 에 BDF 와 오프셋을 조립해 쓴다.
 *      PCIE_CONF_ADDR 매크로가 그 조립을 맡으며 활성 비트도 함께 넣는다.
 *   4) PCIE_CONF_DATA_OFF 에서 요청 크기만큼 읽는다.
 *
 * 크기별로 readl/readw/readb 를 골라 쓰는 점이 특징이다. 다른 컨트롤러처럼
 * 워드를 읽어 잘라 내는 방식이 아니라 하드웨어가 바이트·반워드 접근을
 * 직접 지원한다.
 *
 * 실패 시 PCI_SET_ERROR_RESPONSE 로 *val 을 전부 1 로 채워, 호출자가
 * 쓰레기를 실제 데이터로 오해하지 않게 한다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 상태. 잠들지 않는다.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수] → mvebu_pcie_find_port()
 *               → mvebu_writel() → readl/readw/readb()
 */
static int mvebu_pcie_child_rd_conf(struct pci_bus *bus, u32 devfn, int where,
				    int size, u32 *val)
{
	struct mvebu_pcie *pcie = bus->sysdata;
	struct mvebu_pcie_port *port;
	/* [한국어] 데이터 레지스터의 가상 주소를 담을 곳. */
	void __iomem *conf_data;

	port = mvebu_pcie_find_port(pcie, bus, devfn);
	/* [한국어] 담당 포트를 못 찾았으면 */
	if (!port)
		/* [한국어] 그 자리에 장치가 없다는 뜻이다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (!mvebu_pcie_link_up(port))
		/* [한국어] 링크가 없으면 읽어 봐야 응답이 오지 않는다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	conf_data = port->base + PCIE_CONF_DATA_OFF;
/* [한국어] BDF 와 오프셋을 조립해(윗줄) 주소 레지스터에 쓴다. */

	mvebu_writel(port, PCIE_CONF_ADDR(bus->number, devfn, where),
		     /* [한국어] 이 쓰기가 "다음에 데이터 레지스터를 만지면 이 대상" 이라는 뜻이 된다. */
		     PCIE_CONF_ADDR_OFF);

	switch (size) {
	/* [한국어] 요청 크기별로 접근 폭을 고른다. 이 하드웨어는 바이트·반워드 접근을
	 * 직접 지원해, 워드를 읽어 잘라 내는 방식이 필요 없다. */
	case 1:
		*val = readb_relaxed(conf_data + (where & 3));
		break;
	case 2:
		*val = readw_relaxed(conf_data + (where & 2));
		break;
	case 4:
		*val = readl_relaxed(conf_data);
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * mvebu_pcie_child_wr_conf - 루트 버스 아래 실제 장치의 config 에 쓴다
 *
 * @bus, @devfn: 대상.   @where: 오프셋.   @size: 1, 2, 4.   @val: 쓸 값.
 * @return: PCIBIOS_* 코드.
 *
 * 읽기 판의 짝이며 절차가 같다 — 포트 찾기, 링크 확인, 주소 쓰기, 데이터 쓰기.
 *
 * 읽기와 마찬가지로 크기별로 writel/writew/writeb 를 고른다. 그래서 다른
 * 컨트롤러에서 흔한 읽고-고쳐-쓰기가 필요 없다.
 *
 * 데이터 레지스터의 오프셋에 where & 3 을 더하는 점을 눈여겨볼 만하다.
 * 바이트 단위 접근에서 워드 안 어느 바이트인지를 주소로 지정하는 방식이다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 상태.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수] → mvebu_pcie_find_port()
 *               → mvebu_writel() → writel/writew/writeb()
 */
static int mvebu_pcie_child_wr_conf(struct pci_bus *bus, u32 devfn,
				    int where, int size, u32 val)
{
	struct mvebu_pcie *pcie = bus->sysdata;
	struct mvebu_pcie_port *port;
	/* [한국어] 데이터 레지스터의 가상 주소. */
	void __iomem *conf_data;

	port = mvebu_pcie_find_port(pcie, bus, devfn);
	/* [한국어] 담당 포트를 못 찾았으면 */
	if (!port)
		/* [한국어] 장치 없음으로 답한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (!mvebu_pcie_link_up(port))
		/* [한국어] 링크가 없으면 쓸 수 없다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	conf_data = port->base + PCIE_CONF_DATA_OFF;
/* [한국어] 주소를 조립해(윗줄) 세운다. */

	mvebu_writel(port, PCIE_CONF_ADDR(bus->number, devfn, where),
		     /* [한국어] 주소 레지스터에 쓴다. */
		     PCIE_CONF_ADDR_OFF);

	switch (size) {
	/* [한국어] 1바이트 요청이면 */
	case 1:
		/* [한국어] 워드 안 바이트 위치를 주소에 더해 writeb 한다. 하드웨어가 바이트 쓰기를
		 * 직접 받으므로 읽고-고쳐-쓰기가 필요 없다. */
		writeb(val, conf_data + (where & 3));
		break;
	case 2:
		/* [한국어] 2바이트 요청이면 짝수 오프셋 기준으로 writew. */
		writew(val, conf_data + (where & 2));
		break;
	case 4:
		/* [한국어] 4바이트면 그대로 writel. */
		writel(val, conf_data);
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops mvebu_pcie_child_ops = {
	/* [한국어] 루트 버스 아래 실제 장치용 읽기 콜백. */
	.read = mvebu_pcie_child_rd_conf,
	/* [한국어] 같은 쓰기 콜백. 이 표가 bridge->child_ops 에 걸려, 가짜 브리지용
	 * ops 와 구분되어 쓰인다. */
	.write = mvebu_pcie_child_wr_conf,
};

/*
 * Remove windows, starting from the largest ones to the smallest
 * ones.
 */
/* [한국어]
 * mvebu_pcie_del_windows - 열려 있던 mbus 창들을 지운다
 *
 * @port: 대상 포트.   @base: 지울 구간의 시작.   @size: 크기.
 * @return: 없음.
 *
 * mbus 창은 크기가 2의 거듭제곱이어야 하고 시작 주소가 그 크기에 정렬돼
 * 있어야 한다. 그래서 임의 구간 하나가 여러 창으로 쪼개져 있을 수 있고,
 * 지울 때도 같은 방식으로 쪼개 가며 지워야 한다.
 *
 * 루프가 그 쪼개기를 한다. 남은 크기에 맞는 가장 큰 2의 거듭제곱을 고르되
 * 시작 주소의 정렬을 넘지 않게 하고, 그만큼 지운 뒤 전진한다. 아래
 * mvebu_pcie_add_windows() 의 정확한 역순이라 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(창 재설정).
 *
 * 호출 체인:  mvebu_pcie_set_window() → [이 함수] → mvebu_mbus_del_window()
 */
static void mvebu_pcie_del_windows(struct mvebu_pcie_port *port,
				   phys_addr_t base, size_t size)
{
	while (size) {
		size_t sz = 1 << (fls(size) - 1);
/* [한국어] 이번에 지운 만큼 전진한다(윗줄에서 크기를 정했다). */

		mvebu_mbus_del_window(base, sz);
		/* [한국어] 시작 주소를 밀고 */
		base += sz;
		/* [한국어] 남은 크기를 줄인다. add_windows 의 쪼개기와 정확히 같은 규칙이라 짝이 맞는다. */
		size -= sz;
	/* [한국어] 지우기 루프 끝. */
	}
}

/*
 * MBus windows can only have a power of two size, but PCI BARs do not
 * have this constraint. Therefore, we have to split the PCI BAR into
 * areas each having a power of two size. We start from the largest
 * one (i.e highest order bit set in the size).
 */
/* [한국어]
 * mvebu_pcie_add_windows - 임의 구간을 mbus 창 여러 개로 쪼개 연다
 *
 * @port:   대상 포트.
 * @target: mbus 타깃 ID.   @attr: mbus 속성.
 * @base:   구간 시작.   @size: 크기.   @remap: 재사상할 PCI 쪽 주소(또는 MVEBU_MBUS_NO_REMAP).
 * @return: 0 성공, 음수 errno.
 *
 * mbus 창의 제약(2의 거듭제곱 크기, 크기에 정렬된 시작 주소)을 만족시키려
 * 구간을 여러 창으로 나눈다.
 *
 * 매 회차의 크기 결정이 요점이다.
 *   - 시작 주소의 정렬(1 << __ffs64)이 상한 하나이고,
 *   - 남은 크기를 2의 거듭제곱으로 내림한 것이 다른 상한이다.
 *   둘 중 작은 쪽을 이번 창 크기로 삼는다. 그래야 정렬과 크기 제약을
 *   동시에 만족한다.
 *
 * remap 이 주어지면 그것도 함께 전진시킨다. CPU 주소와 PCI 주소가 다른
 * 구간을 여러 창으로 쪼개도 대응 관계가 유지되어야 하기 때문이다.
 *
 * 실패하면 그때까지 연 창을 되돌리고(del_windows) 오류를 전한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  mvebu_pcie_set_window() → [이 함수]
 *               → mvebu_mbus_add_window_remap_by_id()
 */
static int mvebu_pcie_add_windows(struct mvebu_pcie_port *port,
				   unsigned int target, unsigned int attribute,
				   phys_addr_t base, size_t size,
				   phys_addr_t remap)
{
	size_t size_mapped = 0;

	while (size) {
		/* [한국어] 남은 크기를 2의 거듭제곱으로 내림한 값. fls 로 최상위 비트를 찾아 만든다.
		 * 위에서 시작 주소 정렬과 비교해 작은 쪽을 골랐다. */
		size_t sz = 1 << (fls(size) - 1);
		/* [한국어] 창 추가 결과. */
		int ret;

		ret = mvebu_mbus_add_window_remap_by_id(target, attribute, base,
							/* [한국어] mbus 창을 실제로 연다(윗줄에서 인자를 넘겼다). */
							sz, remap);
		if (ret) {
			/* [한국어] 실패했을 때 로그에 찍을 끝 주소. */
			phys_addr_t end = base + sz - 1;

			dev_err(&port->pcie->pdev->dev,
				"Could not create MBus window at [mem %pa-%pa]: %d\n",
				&base, &end, ret);
			mvebu_pcie_del_windows(port, base - size_mapped,
					       /* [한국어] 지금까지 연 창을 되돌린다. 부분적으로 열린 채 두면 다음 시도와 겹친다. */
					       size_mapped);
			return ret;
		}

		size -= sz;
		/* [한국어] 성공했으니 누적 크기를 늘리고 */
		size_mapped += sz;
		/* [한국어] 시작 주소를 전진시킨다. */
		base += sz;
		/* [한국어] remap 을 쓰는 창이면 */
		if (remap != MVEBU_MBUS_NO_REMAP)
			/* [한국어] 그것도 함께 전진시킨다. 여러 창으로 쪼개도 CPU 주소와 PCI 주소의
			 * 대응 관계가 유지되어야 하기 때문이다. */
			remap += sz;
	/* [한국어] 추가 루프 끝. */
	}

	return 0;
}

/* [한국어]
 * mvebu_pcie_set_window - 창 하나를 원하는 상태로 바꾼다(지우고 다시 연다)
 *
 * @port:      대상 포트.
 * @target, @attr: mbus 타깃과 속성.
 * @desired:   원하는 새 창 상태.
 * @cur:       현재 창 상태. 이 함수가 갱신한다.
 * @return: 0 성공, 음수 errno.
 *
 * 브리지 에뮬레이션과 실제 하드웨어가 만나는 지점의 아래쪽 절반이다.
 * 커널이 브리지 창을 바꾸면 결국 여기로 내려온다.
 *
 * 먼저 원하는 상태와 현재 상태가 같으면 아무것도 하지 않는다. 열거 중
 * 같은 값이 여러 번 쓰이는 일이 흔해 이 검사가 실질적으로 중요하다.
 *
 * 다르면 현재 창을 지우고 새로 연다. mbus 창은 부분 수정이 안 되기
 * 때문이다. 크기가 0 이면 지우기만 하고 끝낸다 — 창을 닫는 요청이다.
 *
 * cur 을 갱신해 두는 것이 다음 호출의 비교 근거가 된다. 그 기억이
 * port->memwin / port->iowin 이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 쓰기 경로에서 내려온다).
 *
 * 호출 체인:  mvebu_pcie_handle_iobase_change() / _membase_change()
 *               → [이 함수] → mvebu_pcie_del_windows() / _add_windows()
 */
static int mvebu_pcie_set_window(struct mvebu_pcie_port *port,
				  unsigned int target, unsigned int attribute,
				  const struct mvebu_pcie_window *desired,
				  struct mvebu_pcie_window *cur)
{
	int ret;

	if (desired->base == cur->base && desired->remap == cur->remap &&
	    /* [한국어] 원하는 상태와 현재 상태가 완전히 같으면(윗줄부터 이어지는 조건)
	     * 아무것도 하지 않는다. 열거 중 같은 값이 여러 번 쓰이는데 그때마다
	     * mbus 창을 다시 열면 링크가 흔들린다. */
	    desired->size == cur->size)
		return 0;

	if (cur->size != 0) {
		/* [한국어] 현재 열려 있는 창을 지운다. mbus 창은 부분 수정이 안 되기 때문이다. */
		mvebu_pcie_del_windows(port, cur->base, cur->size);
		/* [한국어] 기억도 함께 비운다. */
		cur->size = 0;
		/* [한국어] 지우는 데 실패해도 기억은 비워 둔다 — 다음 시도가 옛 값으로
		 * 엉뚱한 구간을 지우지 않게 하려는 것이다. */
		cur->base = 0;
/* [한국어] 지우기 블록 끝. */

		/*
		 * If something tries to change the window while it is enabled
		 * the change will not be done atomically. That would be
		 * difficult to do in the general case.
		 */
	}

	if (desired->size == 0)
		/* [한국어] 원하는 크기가 0 이면 창을 닫으라는 요청이라 여기서 끝난다. */
		return 0;

	ret = mvebu_pcie_add_windows(port, target, attribute, desired->base,
				     /* [한국어] 새 창을 연다(윗줄에서 인자를 넘겼다). */
				     desired->size, desired->remap);
	if (ret) {
		/* [한국어] 열기에 실패했으면 기억을 비우고 */
		cur->size = 0;
		/* [한국어] 시작 주소도 지운 뒤 */
		cur->base = 0;
		/* [한국어] 오류를 전한다. */
		return ret;
	}

	*cur = *desired;
	return 0;
}

/* [한국어]
 * mvebu_pcie_handle_iobase_change - 브리지의 I/O 창 설정을 mbus 창으로 번역한다
 *
 * @port: 대상 포트.
 * @return: 0 성공, 음수 errno.
 *
 * 커널이 가짜 브리지의 IO_BASE/IO_LIMIT 를 쓰면 이 함수가 불린다.
 * PCI 규격의 브리지 창 표현을 Marvell 의 mbus 창으로 옮기는 것이 일이다.
 *
 *   1) I/O 를 지원하지 않는 포트면 창을 닫고 끝낸다.
 *   2) 에뮬레이트 버퍼에서 iobase/iolimit 를 읽는다. 상위 16비트가
 *      상위 주소 확장이고 하위 8비트가 4KB 단위 base/limit 다.
 *   3) limit < base 면 창을 닫으라는 뜻이다. PCI 규격에서 그것이
 *      "창 없음" 의 표현이다.
 *   4) 아니면 base/limit 를 실제 주소로 펼쳐 크기를 구하고
 *      mvebu_pcie_set_window() 로 넘긴다.
 *
 * remap 을 함께 넘기는 것이 요점이다. I/O 창은 CPU 쪽 물리 주소와 PCI 쪽
 * I/O 주소가 다르므로, mbus 가 그 변환까지 해 주어야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(config 쓰기 콜백에서 불린다).
 *
 * 호출 체인:  mvebu_pci_bridge_emul_base_conf_write() → [이 함수]
 *               → mvebu_pcie_set_window()
 */
static int mvebu_pcie_handle_iobase_change(struct mvebu_pcie_port *port)
{
	struct mvebu_pcie_window desired = {};
	struct pci_bridge_emul_conf *conf = &port->bridge.conf;
/* [한국어] I/O 를 지원하지 않는 포트면 창을 닫는 요청으로 바꿔(윗줄) 넘긴다. */

	/* Are the new iobase/iolimit values invalid? */
	if (conf->iolimit < conf->iobase ||
	    le16_to_cpu(conf->iolimitupper) < le16_to_cpu(conf->iobaseupper))
		return mvebu_pcie_set_window(port, port->io_target, port->io_attr,
					     /* [한국어] 크기 0 인 desired 를 넘기면 set_window 가 현재 창을 지우고 끝낸다. */
					     &desired, &port->iowin);

	/*
	 * We read the PCI-to-PCI bridge emulated registers, and
	 * calculate the base address and size of the address decoding
	 * window to setup, according to the PCI-to-PCI bridge
	 * specifications. iobase is the bus address, port->iowin_base
	 * is the CPU address.
	 */
	desired.remap = ((conf->iobase & 0xF0) << 8) |
			(le16_to_cpu(conf->iobaseupper) << 16);
	/* [한국어] CPU 쪽 시작 주소는 I/O 공간의 기준에 remap 을 더해 구한다.
	 * 두 주소 공간이 다르므로 이 변환이 필요하다. */
	desired.base = port->pcie->io.start + desired.remap;
	/* [한국어] 크기는 limit 에서 base 를 빼 구한다. 하위 12비트가 항상 1 인 것은
	 * I/O 창이 4KB 단위이고 limit 가 그 구간의 마지막 주소이기 때문이다. */
	desired.size = ((0xFFF | ((conf->iolimit & 0xF0) << 8) |
			 /* [한국어] 상위 16비트 확장분도 함께 펼친다. */
			 (le16_to_cpu(conf->iolimitupper) << 16)) -
			/* [한국어] base 를 빼고 */
			desired.remap) +
		       1;

	return mvebu_pcie_set_window(port, port->io_target, port->io_attr, &desired,
				     /* [한국어] 그 결과를 set_window 에 넘겨 mbus 창으로 만든다. */
				     &port->iowin);
}

/* [한국어]
 * mvebu_pcie_handle_membase_change - 브리지의 메모리 창 설정을 mbus 창으로 번역한다
 *
 * @port: 대상 포트.
 * @return: 0 성공, 음수 errno.
 *
 * 위 I/O 판과 같은 일을 메모리 창에 대해 한다. 커널이 가짜 브리지의
 * MEMORY_BASE/MEMORY_LIMIT 를 쓰면 불린다.
 *
 * I/O 판과 다른 점이 둘이다.
 *   - 메모리 창은 1MB 단위다(I/O 는 4KB). 그래서 펼치는 시프트가 다르다.
 *   - remap 을 쓰지 않는다(MVEBU_MBUS_NO_REMAP). 메모리 창은 CPU 주소와
 *     PCI 주소가 같게 두기 때문이다.
 *
 * limit < base 면 창을 닫는 것도 같다.
 *
 * 이 함수가 NVMe 를 포함한 모든 엔드포인트의 BAR 접근이 통과하는 길을
 * 만든다 — 커널이 BAR 를 배정하고 브리지 창을 그에 맞게 설정하면,
 * 그 설정이 여기서 mbus 창으로 바뀌어야 CPU 가 장치 레지스터에 닿는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  mvebu_pci_bridge_emul_base_conf_write() → [이 함수]
 *               → mvebu_pcie_set_window()
 */
static int mvebu_pcie_handle_membase_change(struct mvebu_pcie_port *port)
{
	struct mvebu_pcie_window desired = {.remap = MVEBU_MBUS_NO_REMAP};
	struct pci_bridge_emul_conf *conf = &port->bridge.conf;
/* [한국어] 메모리 창이 닫혀 있어야 하면(윗줄) 크기 0 으로 넘긴다. */

	/* Are the new membase/memlimit values invalid? */
	if (le16_to_cpu(conf->memlimit) < le16_to_cpu(conf->membase))
		return mvebu_pcie_set_window(port, port->mem_target, port->mem_attr,
					     /* [한국어] set_window 가 현재 창을 지운다. */
					     &desired, &port->memwin);

	/*
	 * We read the PCI-to-PCI bridge emulated registers, and
	 * calculate the base address and size of the address decoding
	 * window to setup, according to the PCI-to-PCI bridge
	 * specifications.
	 */
	desired.base = ((le16_to_cpu(conf->membase) & 0xFFF0) << 16);
	desired.size = (((le16_to_cpu(conf->memlimit) & 0xFFF0) << 16) | 0xFFFFF) -
		       /* [한국어] 메모리 창 크기를 구한다. limit 에서 base 를 빼고 1 을 더하는데,
		        * limit 가 구간의 마지막 주소라 크기는 그보다 하나 크기 때문이다. */
		       desired.base + 1;

	return mvebu_pcie_set_window(port, port->mem_target, port->mem_attr, &desired,
				     /* [한국어] 메모리 창을 연다. I/O 와 달리 remap 을 쓰지 않아 CPU 주소와 PCI 주소가 같다. */
				     &port->memwin);
}

/* [한국어]
 * mvebu_pci_bridge_emul_base_conf_read - 가짜 브리지의 표준 헤더 읽기 콜백
 * 
 * @bridge: 에뮬레이트 브리지.   @reg: 읽는 오프셋.   @value: 결과를 담을 곳.
 * @return: PCI_BRIDGE_EMUL_HANDLED 면 이 함수가 답했다는 뜻이고,
 *          PCI_BRIDGE_EMUL_NOT_HANDLED 면 공용 기반이 에뮬레이트 버퍼에서 답한다.
 * 
 * 브리지 에뮬레이션의 규약이 이 반환값에 담겨 있다. 하드웨어에서 읽어야
 * 의미가 있는 레지스터만 여기서 처리하고, 나머지는 "안 다룬다" 고 답해
 * pci-bridge-emul.c 가 소프트웨어 버퍼로 답하게 맡긴다.
 * 
 * 세 갈래를 다룬다.
 *   PCI_COMMAND      - 실제 Command 레지스터를 그대로 읽는다.
 *   PCI_PRIMARY_BUS  - 상류 주석대로 이 워드에서 하드웨어가 아는 것은
 *                      secondary bus 번호뿐이다. 그 바이트만 실제 값으로
 *                      덮고 나머지는 에뮬레이트 버퍼 값을 쓴다.
 *   PCI_INTERRUPT_LINE - 역시 상류 주석대로 이 워드에서 하드웨어가 아는 것은
 *                      PCI_BRIDGE_CTL_BUS_RESET 한 비트뿐이다. 그 비트만
 *                      PCIE_CTRL_OFF 의 MASTER_HOT_RESET 상태로 채운다.
 * 
 * 두 경우 모두 버퍼를 먼저 읽고 해당 비트만 고치는 방식이라, 커널이 앞서
 * 써 둔 값이 보존된다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(config 읽기 경로).
 * 
 * 호출 체인:  PCI 코어 → mvebu_pcie_rd_conf() → pci_bridge_emul_conf_read()
 *               → [이 함수] → mvebu_readl()
 */
static pci_bridge_emul_read_status_t
mvebu_pci_bridge_emul_base_conf_read(struct pci_bridge_emul *bridge,
				     int reg, u32 *value)
{
	struct mvebu_pcie_port *port = bridge->data;

	switch (reg) {
	/* [한국어] Command 레지스터는 실물이 있으므로 그대로 읽어 준다. */
	case PCI_COMMAND:
		*value = mvebu_readl(port, PCIE_CMD_OFF);
		break;

	case PCI_PRIMARY_BUS: {
		/*
		 * From the whole 32bit register we support reading from HW only
		 * secondary bus number which is mvebu local bus number.
		 * Other bits are retrieved only from emulated config buffer.
		 */
		__le32 *cfgspace = (__le32 *)&bridge->conf;
		u32 val = le32_to_cpu(cfgspace[PCI_PRIMARY_BUS / 4]);
		/* [한국어] 버퍼 값에서 secondary bus 바이트만 지우고 */
		val &= ~0xff00;
		/* [한국어] 하드웨어의 실제 로컬 버스 번호로 채운다. 상류 주석대로 이 워드에서
		 * 하드웨어가 아는 것은 그 바이트뿐이라, 나머지는 에뮬레이트 버퍼 값을 쓴다. */
		val |= mvebu_pcie_get_local_bus_nr(port) << 8;
		*value = val;
		break;
	}

	case PCI_INTERRUPT_LINE: {
		/*
		 * From the whole 32bit register we support reading from HW only
		 * one bit: PCI_BRIDGE_CTL_BUS_RESET.
		 * Other bits are retrieved only from emulated config buffer.
		 */
		__le32 *cfgspace = (__le32 *)&bridge->conf;
		u32 val = le32_to_cpu(cfgspace[PCI_INTERRUPT_LINE / 4]);
		/* [한국어] 하드웨어의 MASTER_HOT_RESET 이 서 있으면 */
		if (mvebu_readl(port, PCIE_CTRL_OFF) & PCIE_CTRL_MASTER_HOT_RESET)
			/* [한국어] 커널이 보는 BUS_RESET 비트를 세우고 */
			val |= PCI_BRIDGE_CTL_BUS_RESET << 16;
		/* [한국어] 아니면 지운다. 역시 이 워드에서 하드웨어가 아는 것은 그 한 비트뿐이다. */
		else
			val &= ~(PCI_BRIDGE_CTL_BUS_RESET << 16);
		*value = val;
		break;
	}

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;
	}

	return PCI_BRIDGE_EMUL_HANDLED;
}

/* [한국어]
 * mvebu_pci_bridge_emul_pcie_conf_read - 가짜 브리지의 PCIe capability 읽기 콜백
 * 
 * @bridge: 에뮬레이트 브리지.   @reg: capability 안의 오프셋.   @value: 결과.
 * @return: HANDLED 또는 NOT_HANDLED.
 * 
 * PCIe capability 영역은 대부분 하드웨어에 실물이 있어(PCIE_CAP_PCIEXP 기준)
 * 그대로 읽어 주면 되지만, 세 자리는 손을 봐야 한다.
 * 
 *   PCI_EXP_LNKCAP - 상류 주석대로 두 가지를 고친다. Clock Power Management
 *                    비트는 규격상 하류 포트에서 0 이어야 하는데 하드웨어가
 *                    1 을 돌려주므로 지우고, DLLLARC(링크 활성 보고 가능)는
 *                    실제로 제공되므로 세워 준다.
 *   PCI_EXP_LNKCTL - 상위 16비트인 링크 상태에 DLLLA(링크 활성) 비트를
 *                    mvebu_pcie_link_up() 결과로 만들어 얹는다. 하드웨어에
 *                    그 비트가 없어 소프트웨어가 합성하는 것이다.
 *   PCI_EXP_SLTCTL - 슬롯 전력 제한이 DT 에 없으면 ASPL_DISABLE 를 버퍼에서만
 *                    읽고, 있으면 PCIE_SSPL_ENABLE 의 실제 상태를 반영한다.
 *                    상류 주석이 그 두 갈래를 밝혀 두었다. 상위 16비트에는
 *                    슬롯 상태를 얹는데, 이 콜백이 32비트 단위라 한 번에
 *                    두 레지스터를 돌려주어야 하기 때문이다.
 * 
 * 나머지(DEVCAP, DEVCTL, RTSTA, DEVCAP2, DEVCTL2, LNKCTL2)는 실제 값을
 * 그대로 전달한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 
 * 호출 체인:  pci_bridge_emul_conf_read() → [이 함수] → mvebu_readl()
 */
static pci_bridge_emul_read_status_t
mvebu_pci_bridge_emul_pcie_conf_read(struct pci_bridge_emul *bridge,
				     int reg, u32 *value)
{
	struct mvebu_pcie_port *port = bridge->data;

	switch (reg) {
	/* [한국어] 아래 case 들은 PCIe capability 의 실물 레지스터를 그대로 읽어 준다. */
	case PCI_EXP_DEVCAP:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCAP);
		break;

	case PCI_EXP_DEVCTL:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL);
		break;

	case PCI_EXP_LNKCAP:
		/*
		 * PCIe requires that the Clock Power Management capability bit
		 * is hard-wired to zero for downstream ports but HW returns 1.
		 * Additionally enable Data Link Layer Link Active Reporting
		 * Capable bit as DL_Active indication is provided too.
		 */
		*value = (mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCAP) &
			  ~PCI_EXP_LNKCAP_CLKPM) | PCI_EXP_LNKCAP_DLLLARC;
		break;

	case PCI_EXP_LNKCTL:
		/* DL_Active indication is provided via PCIE_STAT_OFF */
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL) |
			 (mvebu_pcie_link_up(port) ?
			  (PCI_EXP_LNKSTA_DLLLA << 16) : 0);
		break;

	case PCI_EXP_SLTCTL: {
		/* [한국어] 에뮬레이트 버퍼의 슬롯 제어 값. */
		u16 slotctl = le16_to_cpu(bridge->pcie_conf.slotctl);
		/* [한국어] 같은 버퍼의 슬롯 상태 값. */
		u16 slotsta = le16_to_cpu(bridge->pcie_conf.slotsta);
		/* [한국어] 조립할 결과. */
		u32 val = 0;
		/*
		 * When slot power limit was not specified in DT then
		 * ASPL_DISABLE bit is stored only in emulated config space.
		 * Otherwise reflect status of PCIE_SSPL_ENABLE bit in HW.
		 */
		if (!port->slot_power_limit_value)
			val |= slotctl & PCI_EXP_SLTCTL_ASPL_DISABLE;
		/* [한국어] DT 지정이 있으면 하드웨어의 활성 비트를 보고(윗줄은 지정이 없는 경우) */
		else if (!(mvebu_readl(port, PCIE_SSPL_OFF) & PCIE_SSPL_ENABLE))
			/* [한국어] 꺼져 있으면 ASPL_DISABLE 로 알린다. 두 경로가 같은 의미를 서로 다른
			 * 출처에서 만들어 내는 셈이다. */
			val |= PCI_EXP_SLTCTL_ASPL_DISABLE;
		/* This callback is 32-bit and in high bits is slot status. */
		val |= slotsta << 16;
		*value = val;
		break;
	}

	case PCI_EXP_RTSTA:
		*value = mvebu_readl(port, PCIE_RC_RTSTA);
		break;

	case PCI_EXP_DEVCAP2:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCAP2);
		break;

	case PCI_EXP_DEVCTL2:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL2);
		break;

	case PCI_EXP_LNKCTL2:
		*value = mvebu_readl(port, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL2);
		break;

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;
	}

	return PCI_BRIDGE_EMUL_HANDLED;
}

/* [한국어]
 * mvebu_pci_bridge_emul_ext_conf_read - 가짜 브리지의 확장(AER) 영역 읽기 콜백
 * 
 * @bridge: 에뮬레이트 브리지.   @reg: AER capability 안의 오프셋.   @value: 결과.
 * @return: HANDLED 또는 NOT_HANDLED.
 * 
 * AER(Advanced Error Reporting) capability 는 하드웨어에 실물이 통째로
 * 있으므로(PCIE_CAP_PCIERR_OFF 기준) 가공 없이 그대로 읽어 준다.
 * 
 * case 목록이 긴 것은 "이 오프셋들만 유효하다" 를 명시하기 위해서다.
 * 목록에 없는 오프셋은 NOT_HANDLED 로 답해 에뮬레이트 버퍼가 답하게 한다 —
 * AER capability 안의 정의되지 않은 자리를 하드웨어에서 읽으면 무엇이
 * 나올지 알 수 없기 때문이다.
 * 
 * case 0 이 capability 헤더 자리이고, PCI_ERR_HEADER_LOG 는 16바이트라
 * 네 워드로 나눠 적혀 있다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 
 * 호출 체인:  pci_bridge_emul_conf_read() → [이 함수] → mvebu_readl()
 */
static pci_bridge_emul_read_status_t
mvebu_pci_bridge_emul_ext_conf_read(struct pci_bridge_emul *bridge,
				    int reg, u32 *value)
{
	struct mvebu_pcie_port *port = bridge->data;

	switch (reg) {
	/* [한국어] capability 헤더 자리(오프셋 0). */
	case 0:
	/* [한국어] 아래 case 목록이 "AER 에서 읽어도 되는 오프셋" 을 명시한다.
	 * 목록 밖은 NOT_HANDLED 로 답해 에뮬레이트 버퍼가 답하게 한다. */
	case PCI_ERR_UNCOR_STATUS:
	case PCI_ERR_UNCOR_MASK:
	case PCI_ERR_UNCOR_SEVER:
	case PCI_ERR_COR_STATUS:
	case PCI_ERR_COR_MASK:
	case PCI_ERR_CAP:
	case PCI_ERR_HEADER_LOG+0:
	case PCI_ERR_HEADER_LOG+4:
	case PCI_ERR_HEADER_LOG+8:
	case PCI_ERR_HEADER_LOG+12:
	case PCI_ERR_ROOT_COMMAND:
	case PCI_ERR_ROOT_STATUS:
	case PCI_ERR_ROOT_ERR_SRC:
		*value = mvebu_readl(port, PCIE_CAP_PCIERR_OFF + reg);
		break;

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;
	}

	return PCI_BRIDGE_EMUL_HANDLED;
}

/* [한국어]
 * mvebu_pci_bridge_emul_base_conf_write - 가짜 브리지의 표준 헤더 쓰기 콜백
 * 
 * @bridge: 에뮬레이트 브리지.   @reg: 쓰는 오프셋.
 * @old: 쓰기 전 값.   @new: 쓴 뒤 값.   @mask: 실제로 바뀐 비트.
 * @return: 없음.
 * 
 * 이 파일에서 브리지 에뮬레이션과 실제 하드웨어가 만나는 가장 중요한
 * 지점이다. 커널이 브리지 config 에 쓴 것을 하드웨어 동작으로 번역한다.
 * 
 *   PCI_COMMAND      - Command 레지스터에 그대로 쓴다. 다만 I/O 를 지원하지
 *                      않는 포트면 IO 비트를 지운다 — 켜 봐야 통로가 없다.
 *   PCI_PRIMARY_BUS  - secondary bus 번호가 바뀌었으면 하드웨어에 반영한다.
 *                      그래야 포트가 그 번호의 config 트랜잭션을 받아들인다.
 *   PCI_IO_BASE / PCI_IO_BASE_UPPER16 - I/O 창이 바뀌었으니
 *                      handle_iobase_change() 로 mbus 창을 다시 연다.
 *   PCI_MEMORY_BASE  - 같은 방식으로 메모리 창을 다시 연다.
 *   PCI_INTERRUPT_LINE - 상위 16비트의 BUS_RESET 비트를 PCIE_CTRL_OFF 의
 *                      MASTER_HOT_RESET 으로 옮긴다. 커널이 브리지 리셋을
 *                      지시하면 실제로 링크가 리셋된다.
 * 
 * mask 를 보고 실제로 바뀐 필드만 처리하는 것이 요점이다. 열거 중 같은
 * 값이 여러 번 쓰이는데, 그때마다 mbus 창을 다시 열면 링크가 흔들린다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(config 쓰기 경로).
 * 
 * 호출 체인:  PCI 코어 → mvebu_pcie_wr_conf() → pci_bridge_emul_conf_write()
 *               → [이 함수] → mvebu_pcie_handle_membase_change() 등
 */
static void
mvebu_pci_bridge_emul_base_conf_write(struct pci_bridge_emul *bridge,
				      int reg, u32 old, u32 new, u32 mask)
{
	struct mvebu_pcie_port *port = bridge->data;
	struct pci_bridge_emul_conf *conf = &bridge->conf;
/* [한국어] 아래 case 들이 커널의 브리지 config 쓰기를 하드웨어 동작으로 번역한다.
 * 이 파일에서 에뮬레이션과 실물이 만나는 지점이다. */

	switch (reg) {
	/* [한국어] Command 쓰기. */
	case PCI_COMMAND:
		/* [한국어] 그대로 하드웨어에 반영한다. 위에서 I/O 미지원 포트의 IO 비트를 걸러 냈다. */
		mvebu_writel(port, new, PCIE_CMD_OFF);
		break;

	case PCI_IO_BASE:
		/* [한국어] I/O 창 관련 비트가 실제로 바뀌었고, I/O 를 지원하며, */
		if ((mask & 0xffff) && mvebu_has_ioport(port) &&
		    mvebu_pcie_handle_iobase_change(port)) {
			/* On error disable IO range */
			conf->iobase &= ~0xf0;
			conf->iolimit &= ~0xf0;
			/* [한국어] I/O 를 지원하지 않을 때는 base 를 limit 보다 크게 만들어(0xf0) */
			conf->iobase |= 0xf0;
			/* [한국어] 상위 확장도 0 으로 두어 */
			conf->iobaseupper = cpu_to_le16(0x0000);
			/* [한국어] 커널이 "창 없음" 으로 읽게 한다. PCI 규격에서 limit < base 가 그 표현이다. */
			conf->iolimitupper = cpu_to_le16(0x0000);
		/* [한국어] 조건 블록 끝. */
		}
		break;

	case PCI_MEMORY_BASE:
		/* [한국어] 메모리 창을 mbus 창으로 반영해 본다. */
		if (mvebu_pcie_handle_membase_change(port)) {
			/* On error disable mem range */
			conf->membase = cpu_to_le16(le16_to_cpu(conf->membase) & ~0xfff0);
			conf->memlimit = cpu_to_le16(le16_to_cpu(conf->memlimit) & ~0xfff0);
			/* [한국어] 실패했으면 base 를 limit 보다 크게 만들어 커널이 "창 없음" 으로 읽게 한다.
			 * 창을 못 열었는데 열린 것처럼 보이면 그 구간으로 접근이 나가 버린다. */
			conf->membase = cpu_to_le16(le16_to_cpu(conf->membase) | 0xfff0);
		/* [한국어] 메모리 창 실패 처리 끝. */
		}
		break;

	case PCI_IO_BASE_UPPER16:
		/* [한국어] I/O 를 지원하고, */
		if (mvebu_has_ioport(port) &&
		    mvebu_pcie_handle_iobase_change(port)) {
			/* On error disable IO range */
			conf->iobase &= ~0xf0;
			conf->iolimit &= ~0xf0;
			/* [한국어] I/O 창 반영에 실패했으면 base 를 limit 보다 크게 만들고 */
			conf->iobase |= 0xf0;
			/* [한국어] 상위 확장도 0 으로, */
			conf->iobaseupper = cpu_to_le16(0x0000);
			/* [한국어] limit 상위도 0 으로 두어 창이 없는 것으로 보이게 한다. */
			conf->iolimitupper = cpu_to_le16(0x0000);
		/* [한국어] I/O 창 실패 처리 끝. */
		}
		break;

	case PCI_PRIMARY_BUS:
		/* [한국어] secondary bus 바이트가 바뀌었으면 */
		if (mask & 0xff00)
			mvebu_pcie_set_local_bus_nr(port, conf->secondary_bus);
		/* [한국어] 하드웨어의 로컬 버스 번호를 갱신한다(윗줄). 그래야 이 포트가 그 번호로
		 * 오는 config 트랜잭션을 자기 것으로 받아들인다. */
		break;

	case PCI_INTERRUPT_LINE:
		/* [한국어] BUS_RESET 비트가 실제로 바뀌었을 때만 손댄다. */
		if (mask & (PCI_BRIDGE_CTL_BUS_RESET << 16)) {
			u32 ctrl = mvebu_readl(port, PCIE_CTRL_OFF);
			/* [한국어] 커널이 리셋을 걸라고 했으면 */
			if (new & (PCI_BRIDGE_CTL_BUS_RESET << 16))
				/* [한국어] MASTER_HOT_RESET 을 세우고 */
				ctrl |= PCIE_CTRL_MASTER_HOT_RESET;
			/* [한국어] 풀라고 했으면 */
			else
				ctrl &= ~PCIE_CTRL_MASTER_HOT_RESET;
			/* [한국어] 지운 값을 되쓴다(윗줄). 가짜 브리지의 리셋 지시가 실제 링크 리셋이 되는 지점이다. */
			mvebu_writel(port, ctrl, PCIE_CTRL_OFF);
		/* [한국어] 리셋 처리 끝. */
		}
		break;

	default:
		break;
	}
}

/* [한국어]
 * mvebu_pci_bridge_emul_pcie_conf_write - 가짜 브리지의 PCIe capability 쓰기 콜백
 * 
 * @bridge: 에뮬레이트 브리지.   @reg: capability 안의 오프셋.
 * @old, @new, @mask: 쓰기 전후 값과 바뀐 비트.
 * @return: 없음.
 * 
 * 읽기 판과 대칭이며, 하드웨어에 실물이 있는 레지스터에만 쓴다.
 * 
 *   PCI_EXP_DEVCTL   - 오류 보고 활성 비트들을 걸러 낸 뒤 쓴다. 걸러 내는
 *                      이유는 이 하드웨어가 그 비트들을 제대로 다루지 못하기
 *                      때문으로 보이나, 그 근거는 이 트리에서 확인 못 함.
 *   PCI_EXP_LNKCTL   - 링크 제어를 쓰되, 상위 16비트(링크 상태)는 쓰기
 *                      대상이 아니라 잘라 낸다.
 *   PCI_EXP_SLTCTL   - 슬롯 전력 제한이 DT 에 지정된 경우에만 하드웨어의
 *                      PCIE_SSPL_ENABLE 을 켜고 끈다. 지정이 없으면
 *                      에뮬레이트 버퍼에만 남긴다 — 읽기 판과 같은 규칙이다.
 *   PCI_EXP_RTSTA    - 루트 상태 레지스터에 그대로 쓴다(보통 비트 지우기다).
 *   DEVCTL2, LNKCTL2 - 그대로 전달한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 
 * 호출 체인:  pci_bridge_emul_conf_write() → [이 함수] → mvebu_writel()
 */
static void
mvebu_pci_bridge_emul_pcie_conf_write(struct pci_bridge_emul *bridge,
				      int reg, u32 old, u32 new, u32 mask)
{
	struct mvebu_pcie_port *port = bridge->data;

	switch (reg) {
	/* [한국어] 장치 제어 쓰기. */
	case PCI_EXP_DEVCTL:
		/* [한국어] 오류 보고 비트를 걸러 낸 값을(윗줄) 하드웨어에 쓴다.
		 * 걸러 내는 이유는 이 하드웨어가 그 비트들을 제대로 다루지 못하기 때문으로
		 * 보이나, 그 근거는 이 트리에서 확인 못 함. */
		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL);
		break;

	case PCI_EXP_LNKCTL:
		/*
		 * PCIe requires that the Enable Clock Power Management bit
		 * is hard-wired to zero for downstream ports but HW allows
		 * to change it.
		 */
		new &= ~PCI_EXP_LNKCTL_CLKREQ_EN;

		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL);
		/* [한국어] 링크 제어 쓰기 끝. 상위 16비트(링크 상태)는 읽기 전용이라 잘라 냈다. */
		break;

	case PCI_EXP_SLTCTL:
		/*
		 * Allow to change PCIE_SSPL_ENABLE bit only when slot power
		 * limit was specified in DT and configured into HW.
		 */
		if ((mask & PCI_EXP_SLTCTL_ASPL_DISABLE) &&
		    port->slot_power_limit_value) {
			u32 sspl = mvebu_readl(port, PCIE_SSPL_OFF);
			/* [한국어] 커널이 ASPL_DISABLE 를 세웠으면 */
			if (new & PCI_EXP_SLTCTL_ASPL_DISABLE)
				/* [한국어] 하드웨어의 슬롯 전력 활성 비트를 내리고 */
				sspl &= ~PCIE_SSPL_ENABLE;
			/* [한국어] 아니면 */
			else
				sspl |= PCIE_SSPL_ENABLE;
			/* [한국어] 올린 값을 쓴다(윗줄). DT 지정이 있는 포트에서만 이 경로를 타며,
			 * 지정이 없으면 에뮬레이트 버퍼에만 남는다 — 읽기 콜백과 같은 규칙이다. */
			mvebu_writel(port, sspl, PCIE_SSPL_OFF);
		/* [한국어] 슬롯 제어 처리 끝. */
		}
		break;

	case PCI_EXP_RTSTA:
		/*
		 * PME Status bit in Root Status Register (PCIE_RC_RTSTA)
		 * is read-only and can be cleared only by writing 0b to the
		 * Interrupt Cause RW0C register (PCIE_INT_CAUSE_OFF). So
		 * clear PME via Interrupt Cause.
		 */
		if (new & PCI_EXP_RTSTA_PME)
			mvebu_writel(port, ~PCIE_INT_PM_PME, PCIE_INT_CAUSE_OFF);
		/* [한국어] 루트 상태 쓰기 끝. 보통 비트를 지우는 용도다. */
		break;

	case PCI_EXP_DEVCTL2:
		/* [한국어] 장치 제어 2 를 그대로 하드웨어에 전달한다. */
		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_DEVCTL2);
		break;

	case PCI_EXP_LNKCTL2:
		/* [한국어] 링크 제어 2 도 그대로 전달한다. */
		mvebu_writel(port, new, PCIE_CAP_PCIEXP + PCI_EXP_LNKCTL2);
		break;

	default:
		break;
	}
}

/* [한국어]
 * mvebu_pci_bridge_emul_ext_conf_write - 가짜 브리지의 확장(AER) 영역 쓰기 콜백
 * 
 * @bridge: 에뮬레이트 브리지.   @reg: AER capability 안의 오프셋.
 * @old, @new, @mask: 쓰기 전후 값과 바뀐 비트.
 * @return: 없음.
 * 
 * 읽기 판과 마찬가지로 AER 은 하드웨어에 실물이 있어 그대로 쓴다.
 * 쓰기가 허용된 오프셋만 case 로 열거하고 나머지는 무시한다.
 * 
 * 읽기 목록과 비교하면 몇 자리가 빠져 있다 — 읽기 전용 레지스터
 * (PCI_ERR_CAP 의 일부, HEADER_LOG, ROOT_ERR_SRC)는 쓸 수 없기 때문이다.
 * 그 구분이 case 목록의 차이로 드러난다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트.
 * 
 * 호출 체인:  pci_bridge_emul_conf_write() → [이 함수] → mvebu_writel()
 */
static void
mvebu_pci_bridge_emul_ext_conf_write(struct pci_bridge_emul *bridge,
				     int reg, u32 old, u32 new, u32 mask)
{
	struct mvebu_pcie_port *port = bridge->data;

	switch (reg) {
	/* These are W1C registers, so clear other bits */
	case PCI_ERR_UNCOR_STATUS:
	case PCI_ERR_COR_STATUS:
	case PCI_ERR_ROOT_STATUS:
		new &= mask;
		fallthrough;
/* [한국어] 아래 case 목록이 "AER 에서 써도 되는 오프셋" 이다. */

	case PCI_ERR_UNCOR_MASK:
	/* [한국어] 읽기 목록과 비교하면 몇 자리가 빠져 있다 — HEADER_LOG 와 ROOT_ERR_SRC 처럼
	 * 읽기 전용인 레지스터는 쓸 수 없기 때문이다. */
	case PCI_ERR_UNCOR_SEVER:
	case PCI_ERR_COR_MASK:
	case PCI_ERR_CAP:
	case PCI_ERR_HEADER_LOG+0:
	case PCI_ERR_HEADER_LOG+4:
	case PCI_ERR_HEADER_LOG+8:
	case PCI_ERR_HEADER_LOG+12:
	case PCI_ERR_ROOT_COMMAND:
	case PCI_ERR_ROOT_ERR_SRC:
		mvebu_writel(port, new, PCIE_CAP_PCIERR_OFF + reg);
		break;

	default:
		break;
	}
}

static const struct pci_bridge_emul_ops mvebu_pci_bridge_emul_ops = {
	/* [한국어] 표준 헤더 영역 읽기 콜백. */
	.read_base = mvebu_pci_bridge_emul_base_conf_read,
	/* [한국어] 같은 쓰기 콜백. 이 표가 브리지 에뮬레이션과 이 파일을 잇는 유일한 접점이다. */
	.write_base = mvebu_pci_bridge_emul_base_conf_write,
	.read_pcie = mvebu_pci_bridge_emul_pcie_conf_read,
	.write_pcie = mvebu_pci_bridge_emul_pcie_conf_write,
	.read_ext = mvebu_pci_bridge_emul_ext_conf_read,
	.write_ext = mvebu_pci_bridge_emul_ext_conf_write,
};

/*
 * Initialize the configuration space of the PCI-to-PCI bridge
 * associated with the given PCIe interface.
 */
/* [한국어]
 * mvebu_pci_bridge_emul_init - 가짜 브리지를 만들어 하드웨어에 잇는다
 *
 * @port: 대상 포트.
 * @return: pci_bridge_emul_init() 의 결과. 0 성공, 음수 errno.
 *
 * 이 파일을 읽을 때 가장 먼저 볼 함수다. "브리지가 없는 하드웨어에
 * 브리지를 만들어 준다" 는 이 드라이버의 핵심이 여기서 조립된다.
 *
 * 에뮬레이트 브리지의 config 초기값을 실제 하드웨어에서 읽어 채운다 —
 * vendor/device ID, revision, subsystem ID 를 실물에서 가져오므로
 * 커널이 보는 가짜 브리지가 진짜 하드웨어의 신원을 갖는다.
 *
 * 플래그 둘이 동작을 좌우한다.
 *   NO_PREFMEM_FORWARD - prefetchable 메모리 창을 지원하지 않는다고 알린다.
 *                        이 하드웨어에 그 창이 없기 때문이다.
 *   NO_IO_FORWARD      - I/O 를 지원하지 않는 포트에만 추가로 붙인다.
 *                        지원하면 대신 iobase/iolimit 에 32비트 타입을 적어
 *                        커널이 I/O 창을 쓸 수 있게 한다.
 *
 * PCIe capability 도 세운다. cap 필드에 하드웨어에서 읽은 버전과
 * PCI_EXP_FLAGS_SLOT(슬롯 있음)을 넣고, 슬롯 용량에 DT 에서 읽은 전력 제한과
 * 물리 슬롯 번호(port+1)를 채운다. 슬롯 상태에 PDS(장치 있음)를 미리
 * 세워 두는데, 이 포트에는 링크가 고정 연결되어 있어 항상 장치가 있는
 * 것으로 보이게 하려는 것이다.
 *
 * 마지막 세 줄이 연결 고리다. data 에 port 를 넣어 콜백들이 문맥을 되찾게
 * 하고, ops 에 여섯 콜백 표를 걸고, pci_bridge_emul_init() 으로 공용 기반에
 * 등록한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  mvebu_pcie_probe() → [이 함수] → pci_bridge_emul_init()
 *               [drivers/pci/pci-bridge-emul.c]
 */
static int mvebu_pci_bridge_emul_init(struct mvebu_pcie_port *port)
{
	unsigned int bridge_flags = PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD;
	struct pci_bridge_emul *bridge = &port->bridge;
	/* [한국어] 가짜 브리지의 신원을 실제 하드웨어에서 읽어 온다. vendor/device ID 다. */
	u32 dev_id = mvebu_readl(port, PCIE_DEV_ID_OFF);
	/* [한국어] revision 도 실물에서. */
	u32 dev_rev = mvebu_readl(port, PCIE_DEV_REV_OFF);
	/* [한국어] subsystem ID 도 실물에서. */
	u32 ssdev_id = mvebu_readl(port, PCIE_SSDEV_ID_OFF);
	/* [한국어] PCIe capability 의 첫 워드. 여기서 버전을 꺼낸다. */
	u32 pcie_cap = mvebu_readl(port, PCIE_CAP_PCIEXP);
	/* [한국어] capability 버전 필드. 16비트 밀어 마스크한다. */
	u8 pcie_cap_ver = ((pcie_cap >> 16) & PCI_EXP_FLAGS_VERS);

	bridge->conf.vendor = cpu_to_le16(dev_id & 0xffff);
	/* [한국어] 상위 16비트가 device ID 다(윗줄이 vendor). */
	bridge->conf.device = cpu_to_le16(dev_id >> 16);
	/* [한국어] revision 은 하위 바이트만 쓴다. 클래스 코드는 에뮬레이션 기반이
	 * 브리지 값으로 채워 주므로 여기서는 넣지 않는다. */
	bridge->conf.class_revision = cpu_to_le32(dev_rev & 0xff);

	if (mvebu_has_ioport(port)) {
		/* We support 32 bits I/O addressing */
		bridge->conf.iobase = PCI_IO_RANGE_TYPE_32;
		bridge->conf.iolimit = PCI_IO_RANGE_TYPE_32;
	/* [한국어] I/O 를 지원하지 않는 포트면(윗줄은 지원하는 경우) */
	} else {
		bridge_flags |= PCI_BRIDGE_EMUL_NO_IO_FORWARD;
	/* [한국어] NO_IO_FORWARD 플래그를 붙여 커널이 I/O 창을 쓰지 않게 한다. */
	}

	/*
	 * Older mvebu hardware provides PCIe Capability structure only in
	 * version 1. New hardware provides it in version 2.
	 * Enable slot support which is emulated.
	 */
	bridge->pcie_conf.cap = cpu_to_le16(pcie_cap_ver | PCI_EXP_FLAGS_SLOT);

	/*
	 * Set Presence Detect State bit permanently as there is no support for
	 * unplugging PCIe card from the slot. Assume that PCIe card is always
	 * connected in slot.
	 *
	 * Set physical slot number to port+1 as mvebu ports are indexed from
	 * zero and zero value is reserved for ports within the same silicon
	 * as Root Port which is not mvebu case.
	 *
	 * Also set correct slot power limit.
	 */
	bridge->pcie_conf.slotcap = cpu_to_le32(
		FIELD_PREP(PCI_EXP_SLTCAP_SPLV, port->slot_power_limit_value) |
		FIELD_PREP(PCI_EXP_SLTCAP_SPLS, port->slot_power_limit_scale) |
		/* [한국어] 물리 슬롯 번호를 port+1 로 채운다. 포트 번호가 0부터라 1 을 더해
		 * 슬롯 번호가 1부터 시작하게 한다. */
		FIELD_PREP(PCI_EXP_SLTCAP_PSN, port->port+1));
	/* [한국어] 슬롯 상태에 PDS(장치 있음)를 미리 세운다. 이 포트에는 링크가 고정
	 * 연결되어 있어 항상 장치가 있는 것으로 보이게 하려는 것이다. */
	bridge->pcie_conf.slotsta = cpu_to_le16(PCI_EXP_SLTSTA_PDS);

	bridge->subsystem_vendor_id = ssdev_id & 0xffff;
	/* [한국어] 상위 16비트가 subsystem ID(윗줄이 vendor). */
	bridge->subsystem_id = ssdev_id >> 16;
	/* [한국어] PCIe capability 를 가진 브리지임을 알린다. */
	bridge->has_pcie = true;
	/* [한국어] 그 capability 가 실물에서 시작하는 오프셋. 콜백들이 이 값을 기준으로
	 * 실제 레지스터를 읽고 쓴다. */
	bridge->pcie_start = PCIE_CAP_PCIEXP;
	/* [한국어] 콜백들이 bridge->data 로 되찾을 포트 포인터. 이 한 줄이 에뮬레이션과
	 * 하드웨어 문맥을 잇는다. */
	bridge->data = port;
	/* [한국어] 여섯 콜백 표를 건다. */
	bridge->ops = &mvebu_pci_bridge_emul_ops;
/* [한국어] 공용 기반에 등록하면 가짜 브리지가 완성된다(윗줄). */

	return pci_bridge_emul_init(bridge, bridge_flags);
}

/* [한국어]
 * sys_to_pcie - pci_sys_data 에서 컨트롤러 상태를 꺼낸다
 *
 * @sys: ARM PCI 계층이 넘기는 sys 자료.
 * @return: 그 안의 private 포인터를 struct mvebu_pcie 로 본 것.
 *
 * ARM 32비트의 옛 PCI 계층이 쓰던 관용구다. sys->private_data 에 넣어 둔
 * 포인터를 되찾는다.
 *
 * 이 파일 안에 호출자가 없다(전수 grep 확인). 옛 경로가 정리되면서 남은
 * 것으로 보이며, 코드는 고치지 않고 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:  (이 트리에 호출자 없음)
 */
static inline struct mvebu_pcie *sys_to_pcie(struct pci_sys_data *sys)
{
	return sys->private_data;
}

/* [한국어]
 * mvebu_pcie_find_port - 버스와 devfn 으로 담당 포트를 찾는다
 *
 * @pcie:  컨트롤러 전체.
 * @bus:   대상 버스.   @devfn: 대상 devfn.
 * @return: 담당 포트. 없으면 NULL.
 *
 * 이 컨트롤러는 포트마다 링크가 하나씩이고 포트가 곧 가짜 브리지 하나다.
 * 그래서 "이 config 접근이 어느 포트의 일인가" 를 정하는 조회가 필요하다.
 *
 * 포트 배열을 훑으며 두 조건 중 하나를 만족하는 포트를 찾는다.
 *   루트 버스면      - 포트의 devfn 과 대상 devfn 이 같은 포트.
 *                      각 포트가 루트 버스에서 서로 다른 devfn 을 차지한다.
 *   그 아래 버스면   - 그 버스 번호가 이 포트의 secondary~subordinate 범위에
 *                      드는 포트. 그 두 번호는 가짜 브리지의 에뮬레이트
 *                      config 버퍼에서 읽으므로, 커널이 배정한 값이 그대로
 *                      조회 기준이 된다.
 *
 * base 가 NULL 인 포트는 건너뛴다 — DT 에 있으나 초기화되지 못한 포트다.
 *
 * 못 찾으면 NULL 이고, 호출자는 그것을 "장치 없음" 으로 답한다.
 *
 * 실행 컨텍스트: config 접근 경로. pci_lock 을 쥔 상태로 불린다.
 *
 * 호출 체인:  mvebu_pcie_child_rd_conf/wr_conf, mvebu_pcie_rd_conf/wr_conf
 *               → [이 함수]
 */
static struct mvebu_pcie_port *mvebu_pcie_find_port(struct mvebu_pcie *pcie,
						    struct pci_bus *bus,
						    int devfn)
{
	int i;

	for (i = 0; i < pcie->nports; i++) {
		/* [한국어] 이번에 검사할 포트. */
		struct mvebu_pcie_port *port = &pcie->ports[i];

		if (!port->base)
			/* [한국어] 초기화되지 못한 포트는 건너뛴다. */
			continue;

		if (bus->number == 0 && port->devfn == devfn)
			/* [한국어] 루트 버스면 devfn 이 일치하는 포트가 담당이다(윗줄이 그 조건).
			 * 각 포트가 루트 버스에서 서로 다른 devfn 을 차지한다. */
			return port;
		/* [한국어] 루트 버스가 아니면 */
		if (bus->number != 0 &&
		    /* [한국어] 그 버스 번호가 이 포트의 secondary~subordinate 범위에 드는지 본다.
		     * 두 번호를 에뮬레이트 config 버퍼에서 읽으므로, 커널이 배정한 값이
		     * 그대로 조회 기준이 된다. */
		    bus->number >= port->bridge.conf.secondary_bus &&
		    bus->number <= port->bridge.conf.subordinate_bus)
			return port;
	/* [한국어] 포트 루프 끝. 끝까지 못 찾으면 NULL 이 반환된다. */
	}

	return NULL;
}

/* PCI configuration space write function */
/* [한국어]
 * mvebu_pcie_wr_conf - 루트 버스(가짜 브리지) config 쓰기 진입점
 *
 * @bus, @devfn: 대상.   @where: 오프셋.   @size: 1, 2, 4.   @val: 쓸 값.
 * @return: PCIBIOS_* 코드.
 *
 * 이 파일의 pci_ops 는 두 벌이다. 이 함수가 붙은 쪽이 루트 버스용이고,
 * mvebu_pcie_child_wr_conf() 가 그 아래 실제 장치용이다.
 *
 * 여기서는 하드웨어를 직접 만지지 않는다. 담당 포트를 찾아
 * pci_bridge_emul_conf_write() 에 넘기면, 공용 기반이 에뮬레이트 버퍼를
 * 갱신하고 필요하면 이 파일의 write 콜백을 되부른다. 그 콜백이 비로소
 * 하드웨어를 움직인다.
 *
 * 포트를 못 찾으면 장치 없음으로 답한다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 상태.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수] → mvebu_pcie_find_port()
 *               → pci_bridge_emul_conf_write() → 이 파일의 write 콜백
 */
static int mvebu_pcie_wr_conf(struct pci_bus *bus, u32 devfn,
			      int where, int size, u32 val)
{
	struct mvebu_pcie *pcie = bus->sysdata;
	struct mvebu_pcie_port *port;
/* [한국어] 담당 포트를 찾는다(윗줄). */

	port = mvebu_pcie_find_port(pcie, bus, devfn);
	/* [한국어] 못 찾았으면 */
	if (!port)
		/* [한국어] 장치 없음으로 답한다. 읽기 판과 달리 *val 을 채울 것이 없다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	return pci_bridge_emul_conf_write(&port->bridge, where, size, val);
/* [한국어] 찾았으면 공용 기반에 넘긴다(윗줄). 거기서 버퍼를 갱신하고 필요하면
 * 이 파일의 write 콜백을 되부른다. */
}

/* PCI configuration space read function */
/* [한국어]
 * mvebu_pcie_rd_conf - 루트 버스(가짜 브리지) config 읽기 진입점
 *
 * @bus, @devfn: 대상.   @where: 오프셋.   @size: 1, 2, 4.   @val: 결과.
 * @return: PCIBIOS_* 코드.
 *
 * 쓰기 판의 짝이다. 포트를 찾아 pci_bridge_emul_conf_read() 에 넘기면,
 * 공용 기반이 에뮬레이트 버퍼에서 답하거나 이 파일의 read 콜백을 되불러
 * 실제 레지스터 값을 섞어 답한다.
 *
 * 포트를 못 찾으면 *val 에 오류 응답(전부 1)을 채우고 장치 없음으로
 * 답한다 — 커널이 쓰레기를 실제 데이터로 오해하지 않게 하는 관례다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 상태.
 *
 * 호출 체인:  PCI 코어(access.c) → [이 함수] → mvebu_pcie_find_port()
 *               → pci_bridge_emul_conf_read() → 이 파일의 read 콜백
 */
static int mvebu_pcie_rd_conf(struct pci_bus *bus, u32 devfn, int where,
			      int size, u32 *val)
{
	struct mvebu_pcie *pcie = bus->sysdata;
	struct mvebu_pcie_port *port;
/* [한국어] 담당 포트를 찾는다(윗줄). */

	port = mvebu_pcie_find_port(pcie, bus, devfn);
	/* [한국어] 못 찾았으면 */
	if (!port)
		/* [한국어] 오류 응답 값을 채우고 장치 없음으로 답한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	return pci_bridge_emul_conf_read(&port->bridge, where, size, val);
/* [한국어] 찾았으면 공용 기반에 넘긴다(윗줄). */
}

static struct pci_ops mvebu_pcie_ops = {
	/* [한국어] 루트 버스(가짜 브리지)용 읽기 콜백. */
	.read = mvebu_pcie_rd_conf,
	/* [한국어] 같은 쓰기 콜백. 위 child_ops 와 두 벌로 나뉘어, PCI 코어가 버스 깊이에
	 * 따라 알아서 갈라 쓴다. */
	.write = mvebu_pcie_wr_conf,
};

/* [한국어]
 * mvebu_pcie_intx_irq_mask - INTx 하나를 마스크한다
 *
 * @d: IRQ 코어가 넘기는 irq_data. hwirq 가 INTx 번호(0~3)다.
 * @return: 없음.
 *
 * PCIE_INT_UNMASK_OFF 에서 해당 INTx 비트를 내린다. 레지스터 이름이
 * "unmask" 이므로 비트를 내리는 것이 마스크다.
 *
 * 읽고-고쳐-쓰기라 락이 필요하다. 네 INTx 의 활성 비트가 한 워드에
 * 모여 있어, 동시에 고치면 갱신 하나가 사라지기 때문이다. raw_spinlock 을
 * 쓰는 이유는 이 경로가 인터럽트 문맥에서 불려 잠들 수 없어서다.
 *
 * 실행 컨텍스트: 인터럽트 또는 프로세스. 잠들지 않는다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → mvebu_readl/writel()
 */
static void mvebu_pcie_intx_irq_mask(struct irq_data *d)
{
	struct mvebu_pcie_port *port = d->domain->host_data;
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	/* [한국어] 인터럽트 상태를 저장할 곳. raw spinlock 을 irqsave 로 잡기 때문에 필요하다. */
	unsigned long flags;
	/* [한국어] 읽고-고쳐-쓸 언마스크 값. */
	u32 unmask;

	raw_spin_lock_irqsave(&port->irq_lock, flags);
	/* [한국어] 현재 언마스크 비트맵을 읽는다. */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);
	/* [한국어] 이 INTx 의 비트를 내린다 — 레지스터가 "unmask" 이므로 내리는 것이 마스크다. */
	unmask &= ~PCIE_INT_INTX(hwirq);
	/* [한국어] 고친 값을 되쓴다. */
	mvebu_writel(port, unmask, PCIE_INT_UNMASK_OFF);
	/* [한국어] 락 해제와 인터럽트 상태 복원. 네 INTx 가 한 워드를 공유해
	 * 읽고-고쳐-쓰기를 보호해야 한다. */
	raw_spin_unlock_irqrestore(&port->irq_lock, flags);
}

/* [한국어]
 * mvebu_pcie_intx_irq_unmask - INTx 하나의 마스크를 푼다
 *
 * @d: IRQ 코어가 넘기는 irq_data.
 * @return: 없음.
 *
 * 위 mask 의 짝이다. 같은 레지스터의 같은 비트를 올린다. 락을 잡는 이유와
 * 방식도 동일하다.
 *
 * 두 함수를 나란히 두는 것이 의도적이다 — 한쪽만 고치는 실수를 막는다.
 *
 * 실행 컨텍스트: 인터럽트 또는 프로세스.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → mvebu_readl/writel()
 */
static void mvebu_pcie_intx_irq_unmask(struct irq_data *d)
{
	struct mvebu_pcie_port *port = d->domain->host_data;
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 읽고-고쳐-쓸 언마스크 값. */
	u32 unmask;

	raw_spin_lock_irqsave(&port->irq_lock, flags);
	/* [한국어] 현재 값을 읽는다. */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);
	/* [한국어] 이 INTx 의 비트를 올린다. mask 와 정확히 대칭이다. */
	unmask |= PCIE_INT_INTX(hwirq);
	/* [한국어] 되쓴다. */
	mvebu_writel(port, unmask, PCIE_INT_UNMASK_OFF);
	/* [한국어] 락 해제. */
	raw_spin_unlock_irqrestore(&port->irq_lock, flags);
/* [한국어] 함수 끝. */
}

static struct irq_chip intx_irq_chip = {
	/* [한국어] /proc/interrupts 에 표시될 이름. */
	.name = "mvebu-INTx",
	/* [한국어] 마스크 콜백. 아래 언마스크와 짝을 이룬다. */
	.irq_mask = mvebu_pcie_intx_irq_mask,
	.irq_unmask = mvebu_pcie_intx_irq_unmask,
};

/* [한국어]
 * mvebu_pcie_intx_irq_map - INTx 가상 IRQ 하나를 설정한다
 *
 * @h:      irq_domain.
 * @virq:   배정된 가상 IRQ 번호.   @hwirq: INTx 번호(0~3).
 * @return: 항상 0.
 *
 * irq_domain_ops 의 map 콜백이다. 도메인이 새 매핑을 만들 때 불려
 * irq_chip 과 흐름 제어 함수를 연결한다.
 *
 * handle_level_irq 를 쓰는 것이 요점이다. INTx 는 레벨 방식이라 원인이
 * 사라질 때까지 신호가 유지되므로, edge 용 흐름 제어를 쓰면 인터럽트가
 * 반복해서 뜬다.
 *
 * chip_data 에 port 를 넣어 두어 위 mask/unmask 콜백이 문맥을 되찾는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(매핑 생성).
 *
 * 호출 체인:  irq_domain 코어 → [이 함수] → irq_set_chip_and_handler()
 */
static int mvebu_pcie_intx_irq_map(struct irq_domain *h,
				   unsigned int virq, irq_hw_number_t hwirq)
{
	struct mvebu_pcie_port *port = h->host_data;

	irq_set_status_flags(virq, IRQ_LEVEL);
	/* [한국어] irq_chip 과 흐름 제어 함수를 건다. handle_level_irq 인 이유는 INTx 가
	 * 레벨 방식이라, edge 용을 쓰면 원인이 사라질 때까지 인터럽트가 반복해서 뜬다. */
	irq_set_chip_and_handler(virq, &intx_irq_chip, handle_level_irq);
	/* [한국어] chip_data 에 포트를 넣어 mask/unmask 콜백이 문맥을 되찾게 한다. */
	irq_set_chip_data(virq, port);
/* [한국어] 항상 0 을 돌려주므로 이 매핑은 실패하지 않는다. */

	return 0;
}

static const struct irq_domain_ops mvebu_pcie_intx_irq_domain_ops = {
	/* [한국어] 매핑 콜백. */
	.map = mvebu_pcie_intx_irq_map,
	/* [한국어] DT 의 인터럽트 지정 한 칸을 hwirq 로 해석하는 표준 헬퍼.
	 * INTx 가 번호 하나로 표현되므로 onecell 판이 맞는다. */
	.xlate = irq_domain_xlate_onecell,
};

/* [한국어]
 * mvebu_pcie_init_irq_domain - 이 포트의 INTx irq_domain 을 만든다
 *
 * @port: 대상 포트.
 * @return: 0 성공, -ENODEV 는 DT 에 interrupt-controller 노드가 없는 경우,
 *          -ENOMEM 은 도메인 생성 실패.
 *
 * DT 의 자식 노드에서 "interrupt-controller" 를 찾아 그것을 도메인의
 * 식별자(fwnode)로 삼는다. 그 노드가 없으면 이 포트는 INTx 를 쓰지 않는
 * 구성이라는 뜻이다.
 *
 * 도메인 크기는 PCI_NUM_INTX(4)다. INTA~INTD 네 개뿐이므로 선형 도메인으로
 * 충분하다.
 *
 * raw_spinlock 을 여기서 초기화한다 — 위 mask/unmask 가 쓸 락이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  mvebu_pcie_probe() → [이 함수] → irq_domain_create_linear()
 */
static int mvebu_pcie_init_irq_domain(struct mvebu_pcie_port *port)
{
	struct device *dev = &port->pcie->pdev->dev;
	struct device_node *pcie_intc_node;
/* [한국어] DT 자식에서 interrupt-controller 노드를 찾는다(윗줄). */

	raw_spin_lock_init(&port->irq_lock);

	pcie_intc_node = of_get_next_child(port->dn, NULL);
	/* [한국어] 없으면 이 포트는 INTx 를 쓰지 않는 구성이다. */
	if (!pcie_intc_node) {
		/* [한국어] 어느 포트인지 남기고 */
		dev_err(dev, "No PCIe Intc node found for %s\n", port->name);
		/* [한국어] 장치 없음으로 답한다. probe 는 이것을 치명적으로 다루지 않는다. */
		return -ENODEV;
	}

	port->intx_irq_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node),
							 /* [한국어] 도메인 크기는 PCI_NUM_INTX(4). INTA~INTD 뿐이라 선형 도메인으로 충분하다. */
							 PCI_NUM_INTX,
							 &mvebu_pcie_intx_irq_domain_ops, port);
	of_node_put(pcie_intc_node);
	if (!port->intx_irq_domain) {
		/* [한국어] 도메인 생성에 실패했으면 남기고 */
		dev_err(dev, "Failed to get INTx IRQ domain for %s\n", port->name);
		/* [한국어] 메모리 부족으로 답한다. */
		return -ENOMEM;
	}

	return 0;
}

/* [한국어]
 * mvebu_pcie_irq_handler - 이 포트의 INTx 체인 인터럽트 핸들러
 *
 * @desc: 상위 IRQ 의 descriptor. handler_data 로 port 를 되찾는다.
 * @return: 없음.
 *
 * 포트의 인터럽트 선 하나에 INTA~INTD 네 개가 묶여 있으므로, 이 핸들러가
 * 원인 레지스터를 읽어 어느 INTx 인지 가려 하위 핸들러로 넘긴다.
 *
 * chained_irq_enter/exit 로 감싸는 것이 체인 핸들러의 규약이다. 그 사이에
 * 상위 인터럽트 컨트롤러가 이 선을 마스크해 두어 재진입이 없다.
 *
 * cause 와 unmask 를 AND 하는 것이 요점이다. 원인 비트가 서 있어도
 * 마스크된 INTx 는 처리하면 안 되기 때문이다.
 *
 * generic_handle_domain_irq() 가 -EINVAL 을 주면 매핑되지 않은 INTx 라는
 * 뜻이라 로그만 남긴다. ratelimited 인 이유는 레벨 인터럽트라 원인이
 * 사라지지 않으면 같은 메시지가 쏟아질 수 있어서다.
 *
 * 원인 비트를 지우지 않는 점을 짚어 둔다. INTx 는 레벨이라 소스가
 * 스스로 신호를 내려야 하며, 컨트롤러 쪽에서 지울 것이 없다.
 *
 * 실행 컨텍스트: 인터럽트. 잠들지 않는다.
 *
 * 호출 체인:  IRQ 코어 → [이 함수] → generic_handle_domain_irq()
 *               → 장치 드라이버의 INTx 핸들러
 */
static void mvebu_pcie_irq_handler(struct irq_desc *desc)
{
	struct mvebu_pcie_port *port = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 로그용 device. */
	struct device *dev = &port->pcie->pdev->dev;
	/* [한국어] 원인·언마스크·실제 처리할 상태. */
	u32 cause, unmask, status;
	/* [한국어] INTx 반복자. */
	int i;

	chained_irq_enter(chip, desc);
/* [한국어] 체인 핸들러 진입. 이 사이에 상위 컨트롤러가 이 선을 마스크해 재진입이 없다. */

	cause = mvebu_readl(port, PCIE_INT_CAUSE_OFF);
	/* [한국어] 언마스크 비트맵을 읽는다(윗줄에서 원인을 읽었다). */
	unmask = mvebu_readl(port, PCIE_INT_UNMASK_OFF);
	/* [한국어] 둘을 AND 한다. 원인이 서 있어도 마스크된 INTx 는 처리하면 안 된다. */
	status = cause & unmask;

	/* Process legacy INTx interrupts */
	for (i = 0; i < PCI_NUM_INTX; i++) {
		if (!(status & PCIE_INT_INTX(i)))
			/* [한국어] 이 INTx 의 비트가 서 있지 않으면 건너뛴다(윗줄이 그 조건). */
			continue;

		if (generic_handle_domain_irq(port->intx_irq_domain, i) == -EINVAL)
			/* [한국어] 매핑되지 않은 INTx 면 로그만 남긴다. ratelimited 인 이유는 레벨
			 * 인터럽트라 원인이 사라지지 않으면 같은 메시지가 쏟아질 수 있어서다.
			 * 원인 비트를 지우지 않는 것은 레벨 방식이라 소스가 스스로 내려야 하기 때문이다. */
			dev_err_ratelimited(dev, "unexpected INT%c IRQ\n", (char)i+'A');
	/* [한국어] INTx 루프 끝. */
	}

	chained_irq_exit(chip, desc);
}

/* [한국어]
 * mvebu_pcie_map_irq - 장치의 INTx 핀을 가상 IRQ 번호로 사상한다
 *
 * @dev:  IRQ 가 필요한 장치.
 * @slot: 슬롯 번호. 쓰지 않는다.
 * @pin:  INTx 핀 번호(1=INTA .. 4=INTD).
 * @return: 가상 IRQ 번호. 실패하면 0.
 *
 * pci_host_bridge.map_irq 콜백이다. 장치가 속한 포트를 찾아 그 포트의
 * 도메인에서 핀에 해당하는 IRQ 를 만들어 준다.
 *
 * pin 이 1부터인데 도메인의 hwirq 는 0부터라 1 을 빼서 넘긴다. 이
 * off-by-one 변환이 이 함수의 실질적 내용이다.
 *
 * slot 을 쓰지 않는 이유는 이 컨트롤러의 포트마다 장치가 하나뿐이라
 * 슬롯으로 구분할 것이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(자원 배정 단계).
 *
 * 호출 체인:  pci_assign_irq() → bridge->map_irq → [이 함수]
 *               → irq_create_mapping()
 */
static int mvebu_pcie_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	/* Interrupt support on mvebu emulated bridges is not implemented yet */
	if (dev->bus->number == 0)
		return 0; /* Proper return code 0 == NO_IRQ */
/* [한국어] 핀 번호가 1부터인데 도메인의 hwirq 는 0부터라 1 을 빼서 매핑한다(윗줄). */

	return of_irq_parse_and_map_pci(dev, slot, pin);
}

/* [한국어]
 * mvebu_pcie_align_resource - 자원 정렬 요구를 mbus 창 제약에 맞춘다
 *
 * @dev:   대상 장치.
 * @res:   배정할 자원.   @start: 후보 시작 주소.   @size: 크기.
 * @align: 원래 요구 정렬. 쓰지 않는다.
 * @return: 조정된 시작 주소.
 *
 * PCI 코어가 BAR 주소를 고를 때 부르는 콜백이다. 보통은 자원 자체의 정렬만
 * 맞추면 되지만, 이 하드웨어에서는 그 자원이 결국 mbus 창으로 표현되어야
 * 한다는 추가 제약이 있다.
 *
 * mbus 창은 크기가 2의 거듭제곱이고 시작 주소가 그 크기에 정렬돼 있어야
 * 한다. 그래서 상류 주석대로 I/O 는 최소 64KB, 메모리는 최소 1MB 로
 * 올림하고, 크기가 그보다 크면 크기를 2의 거듭제곱으로 내림한 값에 맞춘다.
 *
 * max_t 로 최소값과 크기 기반 정렬 중 큰 쪽을 고르는 것이 그 두 조건을
 * 함께 만족시키는 방법이다.
 *
 * 그 밖의 자원 종류는 손대지 않고 start 를 그대로 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(자원 배정).
 *
 * 호출 체인:  PCI 코어의 자원 배정 → bridge->align_resource → [이 함수]
 */
static resource_size_t mvebu_pcie_align_resource(struct pci_dev *dev,
						 const struct resource *res,
						 resource_size_t start,
						 resource_size_t size,
						 resource_size_t align)
{
	if (dev->bus->number != 0)
		return start;
/* [한국어] 상류 주석대로 mbus 창 제약을 자원 정렬에 반영한다. */

	/*
	 * On the PCI-to-PCI bridge side, the I/O windows must have at
	 * least a 64 KB size and the memory windows must have at
	 * least a 1 MB size. Moreover, MBus windows need to have a
	 * base address aligned on their size, and their size must be
	 * a power of two. This means that if the BAR doesn't have a
	 * power of two size, several MBus windows will actually be
	 * created. We need to ensure that the biggest MBus window
	 * (which will be the first one) is aligned on its size, which
	 * explains the rounddown_pow_of_two() being done here.
	 */
	if (res->flags & IORESOURCE_IO)
		return round_up(start, max_t(resource_size_t, SZ_64K,
					     /* [한국어] I/O 는 최소 64KB(윗줄) 또는 크기를 2의 거듭제곱으로 내림한 값 중 큰 쪽에
					      * 맞춘다. mbus 창이 2의 거듭제곱 크기이고 그 크기에 정렬돼야 하기 때문이다. */
					     rounddown_pow_of_two(size)));
	else if (res->flags & IORESOURCE_MEM)
		/* [한국어] 메모리는 최소 1MB 기준으로 같은 계산을 한다. */
		return round_up(start, max_t(resource_size_t, SZ_1M,
					     /* [한국어] 둘 다 아니면 아래에서 start 를 그대로 돌려준다. */
					     rounddown_pow_of_two(size)));
	else
		return start;
}

/* [한국어]
 * mvebu_pcie_map_registers - 포트의 레지스터 블록을 매핑한다
 *
 * @pdev: 플랫폼 장치.   @np: 포트의 DT 노드.   @port: 대상 포트.
 * @return: 매핑된 가상 주소, 실패하면 ERR_PTR.
 *
 * DT 노드의 reg 속성에서 자원을 읽어 devm 으로 매핑한다.
 *
 * 자원을 port->regs 에 보관해 두는 것이 요점이다. 이름을 붙여 두면
 * /proc/iomem 에서 어느 포트의 것인지 구분되고, 뒤의 코드가 물리 주소를
 * 다시 알아낼 수 있다.
 *
 * of_address_to_resource() 실패를 그대로 ERR_PTR 로 감싸 돌려주므로
 * 호출자는 IS_ERR() 로 검사한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  mvebu_pcie_parse_port() → [이 함수] → devm_ioremap_resource()
 */
static void __iomem *mvebu_pcie_map_registers(struct platform_device *pdev,
					      struct device_node *np,
					      struct mvebu_pcie_port *port)
{
	int ret = 0;

	ret = of_address_to_resource(np, 0, &port->regs);
	/* [한국어] 자원을 얻지 못했으면 */
	if (ret)
		/* [한국어] 오류를 ERR_PTR 로 감싸 돌려준다. 반환형이 포인터라 캐스팅이 필요하다. */
		return (void __iomem *)ERR_PTR(ret);
/* [한국어] 얻었으면 아래에서 매핑한다. */

	return devm_ioremap_resource(&pdev->dev, &port->regs);
}

/* [한국어]
 * mvebu_get_tgt_attr - DT 의 ranges 에서 이 포트의 mbus 타깃과 속성을 찾는다
 *
 * @np:   부모 노드(컨트롤러).
 * @devfn: 이 포트의 devfn.   @type: 찾는 자원 종류(IORESOURCE_MEM 또는 _IO).
 * @tgt:  찾은 mbus 타깃 ID 를 담을 곳.   @attr: 찾은 속성을 담을 곳.
 * @return: 0 성공, -EINVAL 은 ranges 파서 초기화 실패, -ENOENT 는 항목 없음.
 *
 * mbus 창을 열려면 "어느 타깃의 어느 속성" 인지를 알아야 하는데, 그 정보가
 * DT 의 ranges 항목 안에 인코딩되어 있다. 이 함수가 그것을 꺼낸다.
 *
 * 찾는 방식이 특이하다. ranges 의 bus_addr 상위 32비트를 슬롯 번호로 보고
 * 이 포트의 슬롯과 비교하며, 동시에 자원 종류도 맞는지 본다. 즉 한 컨트롤러의
 * ranges 안에 포트별·종류별 항목이 섞여 있고 그중 내 것을 골라내는 것이다.
 *
 * 찾으면 parent_bus_addr 의 상위 바이트에서 타깃(56비트 시프트)과
 * 속성(48비트 시프트)을 뽑아낸다. Marvell 의 DT 바인딩이 그 자리에
 * mbus 정보를 실어 두기 때문이다.
 *
 * 시작할 때 *tgt 와 *attr 을 -1 로 채워 두므로, 못 찾았을 때 호출자가
 * mvebu_has_ioport() 처럼 -1 을 "없음" 으로 판정할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  mvebu_pcie_parse_port() → [이 함수] → for_each_of_range()
 */
static int mvebu_get_tgt_attr(struct device_node *np, int devfn,
			      unsigned long type,
			      unsigned int *tgt,
			      unsigned int *attr)
{
	struct of_range range;
	struct of_range_parser parser;
/* [한국어] 파서를 초기화한다(윗줄). */

	*tgt = -1;
	*attr = -1;

	if (of_pci_range_parser_init(&parser, np))
		/* [한국어] ranges 를 해석할 수 없으면 진행할 수 없다. */
		return -EINVAL;

	for_each_of_range(&parser, &range) {
		/* [한국어] 이 항목의 bus_addr 상위 32비트를 슬롯 번호로 본다. */
		u32 slot = upper_32_bits(range.bus_addr);
/* [한국어] Marvell DT 바인딩이 그 자리에 슬롯을 실어 두기 때문이다. */

		if (slot == PCI_SLOT(devfn) &&
		    /* [한국어] 슬롯이 내 것이고 자원 종류도 맞으면(윗줄) 내 항목이다. */
		    type == (range.flags & IORESOURCE_TYPE_BITS)) {
			*tgt = (range.parent_bus_addr >> 56) & 0xFF;
			*attr = (range.parent_bus_addr >> 48) & 0xFF;
			return 0;
		}
	}

	return -ENOENT;
}

/* [한국어]
 * mvebu_pcie_suspend - 절전 전에 포트별 상태 레지스터를 저장한다
 *
 * @dev: 컨트롤러의 device.
 * @return: 항상 0.
 *
 * 포트마다 PCIE_STAT_OFF 를 읽어 saved_pcie_stat 에 담아 둔다. 그 레지스터에
 * 로컬 버스 번호와 장치 번호가 들어 있는데, 절전에서 돌아오면 초기값으로
 * 날아가기 때문이다.
 *
 * base 가 NULL 인 포트는 건너뛴다. DT 에 있으나 실제로 초기화되지 못한
 * 포트가 있을 수 있어서다.
 *
 * 다른 상태(창 설정, 브리지 에뮬레이션 버퍼)는 저장하지 않는다. 창은
 * resume 뒤 커널이 다시 열거하며 설정하고, 에뮬레이션 버퍼는 메모리에
 * 있어 절전과 무관하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 절전).
 *
 * 호출 체인:  PM 코어 → [이 함수] → mvebu_readl()
 */
static int mvebu_pcie_suspend(struct device *dev)
{
	struct mvebu_pcie *pcie;
	int i;
/* [한국어] 포트 반복자. */

	pcie = dev_get_drvdata(dev);
	/* [한국어] 모든 포트를 훑는다. */
	for (i = 0; i < pcie->nports; i++) {
		/* [한국어] 이번 포트. */
		struct mvebu_pcie_port *port = pcie->ports + i;
		/* [한국어] 초기화되지 못한 포트는 */
		if (!port->base)
			/* [한국어] 건너뛴다. */
			continue;
		port->saved_pcie_stat = mvebu_readl(port, PCIE_STAT_OFF);
	/* [한국어] 상태 레지스터를 저장한다(윗줄). 버스·장치 번호가 절전 중 날아가기 때문이다. */
	}

	return 0;
}

/* [한국어]
 * mvebu_pcie_resume - 절전에서 돌아와 포트를 다시 세운다
 *
 * @dev: 컨트롤러의 device.
 * @return: 항상 0.
 *
 * 포트마다 두 가지를 한다.
 *   1) mvebu_pcie_setup_hw() 로 RC 모드·창·INTx 를 다시 초기화한다.
 *      절전 중에 전부 날아갔기 때문이다.
 *   2) 저장해 둔 PCIE_STAT_OFF 를 되쓴다. 그래야 버스·장치 번호가
 *      절전 전과 같아져, 커널이 기억하고 있는 버스 구조와 어긋나지 않는다.
 *
 * 순서가 중요하다. setup_hw 가 먼저인 이유는 그 안에서 장치 번호를 0 으로
 * 맞추는데, 그 뒤에 저장값을 덮어써야 원래 값이 남기 때문이다.
 *
 * suspend 와 마찬가지로 base 가 NULL 인 포트는 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(시스템 재개).
 *
 * 호출 체인:  PM 코어 → [이 함수] → mvebu_pcie_setup_hw() → mvebu_writel()
 */
static int mvebu_pcie_resume(struct device *dev)
{
	struct mvebu_pcie *pcie;
	int i;
/* [한국어] 포트 반복자. */

	pcie = dev_get_drvdata(dev);
	/* [한국어] 모든 포트를 훑는다. */
	for (i = 0; i < pcie->nports; i++) {
		/* [한국어] 이번 포트. */
		struct mvebu_pcie_port *port = pcie->ports + i;
		/* [한국어] 초기화되지 못한 포트는 */
		if (!port->base)
			/* [한국어] 건너뛴다. */
			continue;
		mvebu_writel(port, port->saved_pcie_stat, PCIE_STAT_OFF);
		/* [한국어] RC 모드·창·INTx 를 다시 초기화한다. 그 뒤에 저장값을 되써야
		 * 장치 번호가 원래대로 남는다 — setup_hw 가 장치 번호를 0 으로 맞추기 때문이다. */
		mvebu_pcie_setup_hw(port);
	}

	return 0;
}

/* [한국어]
 * mvebu_pcie_port_clk_put - devm 정리 시 포트 클럭 참조를 반납한다
 *
 * @data: devm_add_action_or_reset() 에 넘겼던 포인터. 실제로는 mvebu_pcie_port.
 * @return: 없음.
 *
 * 포트 클럭은 devm_clk_get 이 아니라 of_clk_get_by_name 계열로 얻으므로
 * 자동 반납되지 않는다. 그래서 이 콜백을 devm 액션으로 걸어 두어 장치
 * 해제 시 clk_put 이 불리게 한다.
 *
 * 포트별로 하나씩 걸리며, probe 가 중간에 실패해도 이미 걸린 것들은
 * 커널이 되돌려 준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(장치 해제 또는 probe 실패 되돌리기).
 *
 * 호출 체인:  devres 언와인드 → [이 함수] → clk_put()
 */
static void mvebu_pcie_port_clk_put(void *data)
{
	struct mvebu_pcie_port *port = data;

	clk_put(port->clk);
}

/* [한국어]
 * mvebu_pcie_parse_port - DT 자식 노드 하나에서 포트 정보를 읽어 채운다
 *
 * @pcie: 컨트롤러 전체.   @port: 채울 포트.   @child: 그 포트의 DT 노드.
 * @return: 0 성공, 1 은 이 노드를 건너뛰라는 뜻, 음수는 오류.
 *
 * 이 파일에서 가장 긴 함수 중 하나이며, DT 한 노드를 struct mvebu_pcie_port
 * 하나로 옮기는 일을 한다.
 *
 * 반환값 1 이 특이하다. status 가 disabled 인 노드나 필수 속성이 없는
 * 노드를 만나면 오류가 아니라 "건너뛰라" 는 뜻으로 1 을 돌려주고,
 * 호출자는 그 포트를 세지 않고 다음으로 넘어간다. 보드마다 쓰지 않는
 * 포트가 DT 에 남아 있는 것이 정상이기 때문이다.
 *
 * 읽는 것이 여럿이다.
 *   marvell,pcie-port / -lane  - 포트와 레인 번호. 이름(pcieA.B)의 재료다.
 *   num-lanes                  - 4 면 is_x4 를 세운다.
 *   devfn                      - 루트 버스에서 이 포트가 차지할 자리.
 *                                of_pci_get_devfn() 으로 reg 에서 뽑는다.
 *   mem/io 타깃과 속성          - mvebu_get_tgt_attr() 로 ranges 에서 찾는다.
 *                                I/O 는 없어도 되며, 없으면 -1 로 남아
 *                                mvebu_has_ioport() 가 false 를 준다.
 *   reset-gpios                - 있으면 리셋 GPIO 를 잡고 이름을 붙인다.
 *   슬롯 전력 제한             - of_pci_get_slot_power_limit() 로 읽어
 *                                브리지 에뮬레이션의 슬롯 용량에 쓴다.
 *   클럭과 레지스터 블록        - 얻어서 포트에 건다.
 *
 * 이름을 devm_kasprintf 로 만들어 두는 것이 로그 가독성에 중요하다 —
 * 이후 모든 메시지가 "pcie0.0" 같은 이름을 달고 나온다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  mvebu_pcie_probe() → [이 함수] → mvebu_get_tgt_attr()
 *               → mvebu_pcie_map_registers() → of_pci_get_slot_power_limit()
 */
static int mvebu_pcie_parse_port(struct mvebu_pcie *pcie,
	struct mvebu_pcie_port *port, struct device_node *child)
{
	struct device *dev = &pcie->pdev->dev;
	u32 slot_power_limit;
	/* [한국어] 하위 호출 결과. */
	int ret;
	/* [한국어] num-lanes 값을 받을 변수. */
	u32 num_lanes;

	port->pcie = pcie;
/* [한국어] 포트 번호를 읽는다(윗줄). */

	if (of_property_read_u32(child, "marvell,pcie-port", &port->port)) {
		/* [한국어] 필수 속성이 없으면 경고를 남기고 */
		dev_warn(dev, "ignoring %pOF, missing pcie-port property\n",
			 /* [한국어] 이 노드를 건너뛰라는 뜻으로 1 을 돌려준다. 오류가 아닌 이유는
			  * 보드마다 쓰지 않는 포트가 DT 에 남아 있는 것이 정상이기 때문이다. */
			 child);
		goto skip;
	}

	if (of_property_read_u32(child, "marvell,pcie-lane", &port->lane))
		/* [한국어] 레인 번호가 없으면 0 으로 둔다. 포트 번호와 달리 필수가 아니다. */
		port->lane = 0;

	if (!of_property_read_u32(child, "num-lanes", &num_lanes) && num_lanes == 4)
		port->is_x4 = true;
/* [한국어] 이름을 "pcieA.B" 형식으로 만든다(윗줄에서 시작). */

	port->name = devm_kasprintf(dev, GFP_KERNEL, "pcie%d.%d", port->port,
				    /* [한국어] 포트 번호와 레인 번호를 넣는다. 이후 모든 로그가 이 이름을 달고 나온다. */
				    port->lane);
	if (!port->name) {
		/* [한국어] 이름 할당에 실패하면 메모리 부족으로 답한다. */
		ret = -ENOMEM;
		goto err;
	}

	port->devfn = of_pci_get_devfn(child);
	/* [한국어] reg 속성에서 devfn 을 얻지 못했으면 */
	if (port->devfn < 0)
		/* [한국어] 이 노드를 건너뛴다. */
		goto skip;
	if (PCI_FUNC(port->devfn) != 0) {
		/* [한국어] 기능 번호가 0 이 아니면 지원하지 않는다. */
		dev_err(dev, "%s: invalid function number, must be zero\n",
			/* [한국어] 어느 포트인지 남기고 건너뛴다 — 포트마다 기능이 하나뿐인 구조다. */
			port->name);
		goto skip;
	}

	ret = mvebu_get_tgt_attr(dev->of_node, port->devfn, IORESOURCE_MEM,
				 /* [한국어] 메모리 창의 mbus 타깃과 속성을 DT ranges 에서 찾는다(윗줄). */
				 &port->mem_target, &port->mem_attr);
	if (ret < 0) {
		/* [한국어] 못 찾았으면 메모리 창을 열 수 없다. */
		dev_err(dev, "%s: cannot get tgt/attr for mem window\n",
			/* [한국어] 메모리는 필수라 이 포트를 건너뛴다. */
			port->name);
		goto skip;
	}

	if (resource_size(&pcie->io) != 0) {
		/* [한국어] I/O 창도 같은 방식으로 찾는다. */
		mvebu_get_tgt_attr(dev->of_node, port->devfn, IORESOURCE_IO,
				   /* [한국어] 다만 실패해도 오류가 아니다. */
				   &port->io_target, &port->io_attr);
	} else {
		port->io_target = -1;
		/* [한국어] 못 찾았으면 -1 로 남겨 mvebu_has_ioport() 가 false 를 주게 한다
		 * (윗줄이 target). I/O 를 쓰지 않는 보드가 정상이기 때문이다. */
		port->io_attr = -1;
	/* [한국어] I/O 처리 끝. */
	}

	/*
	 * Old DT bindings do not contain "intx" interrupt
	 * so do not fail probing driver when interrupt does not exist.
	 */
	port->intx_irq = of_irq_get_byname(child, "intx");
	if (port->intx_irq == -EPROBE_DEFER) {
		/* [한국어] INTx IRQ 를 얻지 못했으면 그 errno 를 담고 */
		ret = port->intx_irq;
		/* [한국어] 공통 오류 경로로 간다. */
		goto err;
	}
	if (port->intx_irq <= 0) {
		/* [한국어] IRQ 는 있으나 DT 에 intx 인터럽트 지정이 없는 경우 경고한다. */
		dev_warn(dev, "%s: legacy INTx interrupts cannot be masked individually, "
			      /* [한국어] 그러면 INTx 를 개별로 마스크할 수 없다는 뜻이며, */
			      "%pOF does not contain intx interrupt\n",
			 /* [한국어] 어느 노드가 그런지 함께 남긴다. 오류로 만들지는 않는다. */
			 port->name, child);
	}

	port->reset_name = devm_kasprintf(dev, GFP_KERNEL, "%s-reset",
					  /* [한국어] 리셋 GPIO 에 붙일 이름을 만든다(윗줄). */
					  port->name);
	if (!port->reset_name) {
		/* [한국어] 실패하면 메모리 부족. */
		ret = -ENOMEM;
		goto err;
	}

	port->reset_gpio = devm_fwnode_gpiod_get(dev, of_fwnode_handle(child),
						 /* [한국어] reset-gpios 를 얻는다. GPIOD_OUT_HIGH 로 요청하는 것은 처음에
						  * 리셋을 걸어 둔 상태로 시작한다는 뜻이다 — powerup 이 나중에 푼다. */
						 "reset", GPIOD_OUT_HIGH,
						 port->name);
	ret = PTR_ERR_OR_ZERO(port->reset_gpio);
	/* [한국어] 얻기에 실패했는데 */
	if (ret) {
		/* [한국어] "없음"(-ENOENT)이 아니면 진짜 오류다. */
		if (ret != -ENOENT)
			/* [한국어] 오류 경로로 간다. */
			goto err;
		/* reset gpio is optional */
		port->reset_gpio = NULL;
		devm_kfree(dev, port->reset_name);
		/* [한국어] 없으면 이름도 비워 둔다 — 리셋 GPIO 가 선택 사항이기 때문이다. */
		port->reset_name = NULL;
	/* [한국어] GPIO 처리 끝. */
	}

	slot_power_limit = of_pci_get_slot_power_limit(child,
				/* [한국어] 슬롯 전력 제한을 읽는다(윗줄). */
				&port->slot_power_limit_value,
				&port->slot_power_limit_scale);
	if (slot_power_limit)
		/* [한국어] 값이 있으면 로그에 남긴다. */
		dev_info(dev, "%s: Slot power limit %u.%uW\n",
			 /* [한국어] 어느 포트의 몇 W 인지 함께 찍는다. */
			 port->name,
			 slot_power_limit / 1000,
			 (slot_power_limit / 100) % 10);

	port->clk = of_clk_get_by_name(child, NULL);
	/* [한국어] 클럭을 얻지 못했으면 */
	if (IS_ERR(port->clk)) {
		/* [한국어] 남기고 */
		dev_err(dev, "%s: cannot get clock\n", port->name);
		/* [한국어] 이 포트를 건너뛴다. 클럭 없이는 포트를 켤 수 없다. */
		goto skip;
	}

	ret = devm_add_action_or_reset(dev, mvebu_pcie_port_clk_put, port);
	/* [한국어] 클럭 반납을 devm 액션으로 예약한다(윗줄). of_clk_get 계열은 자동
	 * 반납되지 않기 때문이다. */
	if (ret < 0)
		/* [한국어] 예약에 실패하면 오류 경로로. */
		goto err;

	return 1;
/* [한국어] 여기까지 오면 이 포트는 쓸 수 있다. */

skip:
	ret = 0;

	/* In the case of skipping, we need to free these */
	devm_kfree(dev, port->reset_name);
	port->reset_name = NULL;
	/* [한국어] 건너뛰는 포트의 이름을 미리 반납한다. 아래에서 1 을 돌려주면 호출자가
	 * 이 포트를 세지 않으므로, 이름이 남아 있으면 낭비다. */
	devm_kfree(dev, port->name);
	/* [한국어] NULL 로 만들어 두어 뒤의 코드가 이 포트를 초기화되지 않은 것으로 본다. */
	port->name = NULL;
/* [한국어] 아래에서 1 을 돌려준다 — 오류가 아니라 "건너뛰라" 는 뜻이다. */

err:
	return ret;
}

/*
 * Power up a PCIe port.  PCIe requires the refclk to be stable for 100µs
 * prior to releasing PERST.  See table 2-4 in section 2.6.2 AC Specifications
 * of the PCI Express Card Electromechanical Specification, 1.1.
 */
/* [한국어]
 * mvebu_pcie_powerup - 포트의 클럭을 켜고 리셋을 푼다
 *
 * @port: 대상 포트.
 * @return: 0 성공, clk_prepare_enable() 의 errno.
 *
 * 클럭을 먼저 켜고, 리셋 GPIO 가 있으면 그것을 푼다.
 *
 * 리셋 해제 절차에 지연이 둘 있다.
 *   udelay(100)      - GPIO 를 내리기 전의 짧은 대기. 클럭이 안정될 시간을
 *                      주는 것으로 보이나 그 근거는 이 트리에서 확인 못 함.
 *   msleep(...)      - 리셋을 푼 뒤의 대기. 기본값이 PCI_PM_D3COLD_WAIT
 *                      (규격이 정한 D3cold 복귀 대기)이고, DT 의
 *                      reset-delay-us 로 보드마다 늘릴 수 있다.
 *
 * GPIO 값 0 이 "리셋 해제" 인 것은 DT 에서 active-low 로 선언되기 때문이다 —
 * gpiod 계층이 극성을 흡수하므로 코드는 논리값만 다룬다.
 *
 * cansleep 판을 쓰는 이유는 GPIO 가 I2C 확장기 같은 느린 버스 뒤에 있을 수
 * 있어서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:  mvebu_pcie_probe() → [이 함수] → clk_prepare_enable()
 *               → gpiod_set_value_cansleep()
 */
static int mvebu_pcie_powerup(struct mvebu_pcie_port *port)
{
	int ret;

	ret = clk_prepare_enable(port->clk);
	/* [한국어] 클럭 켜기에 실패했으면 */
	if (ret < 0)
		/* [한국어] 그 errno 를 전한다. 리셋을 풀기 전이라 되돌릴 것이 없다. */
		return ret;

	if (port->reset_gpio) {
		/* [한국어] 리셋 해제 뒤 대기 시간. 기본값이 규격의 D3cold 복귀 대기(마이크로초)다. */
		u32 reset_udelay = PCI_PM_D3COLD_WAIT * 1000;
/* [한국어] 보드마다 다를 수 있어 아래에서 DT 값으로 덮는다. */

		of_property_read_u32(port->dn, "reset-delay-us",
				     /* [한국어] DT 의 reset-delay-us 가 있으면 그것을 쓴다. */
				     &reset_udelay);

		udelay(100);

		gpiod_set_value_cansleep(port->reset_gpio, 0);
		/* [한국어] 리셋을 푼 뒤(윗줄) 그만큼 잠든다. 엔드포인트가 깨어나 링크 학습을
		 * 시작할 시간을 주는 것이다. */
		msleep(reset_udelay / 1000);
	/* [한국어] 리셋 해제 블록 끝. */
	}

	return 0;
}

/*
 * Power down a PCIe port.  Strictly, PCIe requires us to place the card
 * in D3hot state before asserting PERST#.
 */
/* [한국어]
 * mvebu_pcie_powerdown - 포트를 리셋 상태로 두고 클럭을 끈다
 *
 * @port: 대상 포트.   @return: 없음.
 *
 * powerup 의 역순이다. 리셋을 걸고(GPIO 값 1) 클럭을 끈다.
 *
 * 순서가 중요하다. 클럭을 먼저 끄면 리셋 신호가 제대로 전달되지 않을 수
 * 있어, 리셋을 건 뒤에 클럭을 내린다.
 *
 * reset_gpio 가 NULL 이어도 gpiod_set_value_cansleep() 이 무해하게
 * 처리하므로 별도 분기가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:  mvebu_pcie_probe() 의 에러 경로 / mvebu_pcie_remove()
 *               → [이 함수] → gpiod_set_value_cansleep() → clk_disable_unprepare()
 */
static void mvebu_pcie_powerdown(struct mvebu_pcie_port *port)
{
	gpiod_set_value_cansleep(port->reset_gpio, 1);

	clk_disable_unprepare(port->clk);
}

/*
 * devm_of_pci_get_host_bridge_resources() only sets up translatable resources,
 * so we need extra resource setup parsing our special DT properties encoding
 * the MEM and IO apertures.
 */
/* [한국어]
 * mvebu_pcie_parse_request_resources - 브리지의 자원 목록을 세운다
 *
 * @pcie: 컨트롤러 전체.
 * @return: 0 성공, 음수 errno.
 *
 * PCI 코어가 BAR 를 배정하려면 쓸 수 있는 주소 공간 목록이 있어야 한다.
 * 그 목록을 여기서 만든다.
 *
 * 메모리 공간은 필수다. DT 의 ranges 에서 얻지 못하면 진행할 수 없다.
 * I/O 공간은 선택이며, 있으면 io 와 realio 두 자원을 준비한다 — 전자는
 * CPU 물리 주소 구간이고 후자는 PCI I/O 번호 공간이라, 둘의 대응을
 * pci_add_resource_offset() 으로 알려 준다.
 *
 * 버스 번호 자원도 함께 넣는다. 이것이 없으면 코어가 버스 번호를
 * 배정할 범위를 모른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe).
 *
 * 호출 체인:  mvebu_pcie_probe() → [이 함수] → devm_request_resource()
 *               → pci_add_resource_offset()
 */
static int mvebu_pcie_parse_request_resources(struct mvebu_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);
	/* [한국어] 하위 호출 결과. */
	int ret;

	/* Get the PCIe memory aperture */
	mvebu_mbus_get_pcie_mem_aperture(&pcie->mem);
	if (resource_size(&pcie->mem) == 0) {
		/* [한국어] 메모리 구간을 얻지 못했으면 남기고 */
		dev_err(dev, "invalid memory aperture size\n");
		/* [한국어] 진행할 수 없다. 메모리 공간은 필수다. */
		return -EINVAL;
	}

	pcie->mem.name = "PCI MEM";
	/* [한국어] 메모리 구간을 브리지의 창 목록에 넣는다. 이것이 있어야 PCI 코어가
	 * BAR 를 배정할 주소를 안다. */
	pci_add_resource(&bridge->windows, &pcie->mem);
	/* [한국어] 같은 구간을 커널 자원 트리에 예약해 다른 주체와 충돌하지 않게 한다. */
	ret = devm_request_resource(dev, &iomem_resource, &pcie->mem);
	/* [한국어] 예약에 실패하면 */
	if (ret)
		/* [한국어] 그 errno 를 전한다. */
		return ret;

	/* Get the PCIe IO aperture */
	mvebu_mbus_get_pcie_io_aperture(&pcie->io);

	if (resource_size(&pcie->io) != 0) {
		/* [한국어] I/O 는 선택이다. 있으면 PCI 쪽 번호 공간을 따로 만든다. */
		pcie->realio.flags = pcie->io.flags;
		/* [한국어] 시작은 규격이 정한 최소 I/O 번호부터. */
		pcie->realio.start = PCIBIOS_MIN_IO;
		/* [한국어] 끝은 두 상한 중 작은 쪽으로 정한다. */
		pcie->realio.end = min_t(resource_size_t,
					 /* [한국어] 아키텍처의 I/O 공간 상한에서 64KB 를 뺀 값과 */
					 IO_SPACE_LIMIT - SZ_64K,
					 resource_size(&pcie->io) - 1);
		pcie->realio.name = "PCI I/O";
/* [한국어] 실제 구간 크기 중 작은 쪽이다(윗줄에서 비교). */

		ret = devm_pci_remap_iospace(dev, &pcie->realio, pcie->io.start);
		/* [한국어] I/O 공간을 CPU 주소 공간에 매핑한다. */
		if (ret)
			/* [한국어] 실패하면 그 errno 를 전한다. */
			return ret;

		pci_add_resource(&bridge->windows, &pcie->realio);
		/* [한국어] PCI I/O 번호 공간도 자원 트리에 예약한다. */
		ret = devm_request_resource(dev, &ioport_resource, &pcie->realio);
		/* [한국어] 실패하면 */
		if (ret)
			/* [한국어] errno 를 전한다. */
			return ret;
	}

	return 0;
}

/* [한국어]
 * mvebu_pcie_probe - 진입점. 포트들을 세우고 PCI 코어에 열거를 넘긴다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 성공, 음수 errno.
 *
 * 이 파일의 전체 흐름이 담긴 함수다.
 *
 *   1) 브리지와 private 영역(struct mvebu_pcie)을 함께 할당한다.
 *   2) mbus 가 준비됐는지 확인한다. 이 드라이버는 mbus 창 API 에 전적으로
 *      기대므로 그것 없이는 시작할 수 없다.
 *   3) 자원 목록을 세운다.
 *   4) DT 자식 노드를 세어 포트 배열을 할당한다.
 *   5) 노드마다 mvebu_pcie_parse_port() 를 부른다. 1 을 돌려주면 그 포트는
 *      세지 않고 건너뛰므로, 실제 포트 수(nports)가 노드 수보다 적을 수 있다.
 *   6) 포트마다 powerup → setup_hw → bridge_emul_init → init_irq_domain 을
 *      차례로 한다. 여기서 가짜 브리지가 완성된다.
 *   7) INTx 체인 핸들러를 건다.
 *   8) 브리지에 ops(두 벌 중 루트 버스용), align_resource, map_irq 를 꽂고
 *      pci_host_probe() 로 넘긴다.
 *
 * ops 를 두 벌 쓰는 것이 이 드라이버의 특징이다. bridge->ops 가 루트 버스
 * (가짜 브리지)용이고 bridge->child_ops 가 그 아래 실제 장치용이라,
 * PCI 코어가 버스 깊이에 따라 알아서 갈라 쓴다.
 *
 * 포트 하나가 실패해도 나머지로 계속 진행하는 구조라, 에러 경로가
 * 포트 단위로 되돌리는 형태를 띤다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 바인딩). 링크 대기에서 잠든다.
 *
 * 호출 체인:  플랫폼 버스 → [이 함수] → mvebu_pcie_parse_port()
 *               → mvebu_pcie_powerup() → mvebu_pcie_setup_hw()
 *               → mvebu_pci_bridge_emul_init() → pci_host_probe()
 */
static int mvebu_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mvebu_pcie *pcie;
	/* [한국어] PCI 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] 컨트롤러의 DT 노드. */
	struct device_node *np = dev->of_node;
	/* [한국어] 포트 노드 반복자. */
	struct device_node *child;
	/* [한국어] num 은 자식 수, i 는 실제 포트 인덱스, ret 는 결과. */
	int num, i, ret;
/* [한국어] 둘을 나누는 이유는 건너뛴 노드가 있어 두 값이 달라지기 때문이다. */

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(struct mvebu_pcie));
	/* [한국어] 브리지와 private 영역을 함께 할당한다(윗줄). */
	if (!bridge)
		/* [한국어] 실패하면 메모리 부족. */
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	/* [한국어] 이후 로그와 자원 요청의 기준이 된다. */
	pcie->pdev = pdev;
	/* [한국어] remove 와 PM 콜백이 되찾을 수 있게 걸어 둔다. */
	platform_set_drvdata(pdev, pcie);
/* [한국어] 아래에서 자원 목록을 세운다. */

	ret = mvebu_pcie_parse_request_resources(pcie);
	/* [한국어] 자원 준비에 실패하면 */
	if (ret)
		/* [한국어] 그 errno 를 전한다. */
		return ret;

	num = of_get_available_child_count(np);
/* [한국어] DT 자식 수를 세어(윗줄) 포트 배열 크기를 정한다. */

	pcie->ports = devm_kcalloc(dev, num, sizeof(*pcie->ports), GFP_KERNEL);
	/* [한국어] 배열 할당에 실패하면 */
	if (!pcie->ports)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	i = 0;
	/* [한국어] 활성화된 자식 노드만 훑는다. status 가 disabled 인 노드는 여기서 걸러진다. */
	for_each_available_child_of_node(np, child) {
		/* [한국어] 이번 포트가 들어갈 자리. */
		struct mvebu_pcie_port *port = &pcie->ports[i];

		ret = mvebu_pcie_parse_port(pcie, port, child);
		/* [한국어] 노드를 파싱한다(윗줄). */
		if (ret < 0) {
			/* [한국어] 음수면 진짜 오류라 노드 참조를 반납하고 실패로 끝낸다. */
			of_node_put(child);
			return ret;
		} else if (ret == 0) {
			/* [한국어] 1 이면 건너뛰라는 뜻이라 i 를 올리지 않고 다음 노드로 간다.
			 * 그래서 nports 가 자식 수보다 작아질 수 있다. */
			continue;
		}

		port->dn = child;
		/* [한국어] 성공한 포트만 인덱스를 올린다. */
		i++;
	/* [한국어] 파싱 루프 끝. */
	}
	pcie->nports = i;
/* [한국어] 실제로 쓸 수 있는 포트 수가 확정된다(윗줄). */

	for (i = 0; i < pcie->nports; i++) {
		/* [한국어] 이번 포트. */
		struct mvebu_pcie_port *port = &pcie->ports[i];
		/* [한국어] 이 포트의 INTx IRQ 번호. */
		int irq = port->intx_irq;

		child = port->dn;
		/* [한국어] DT 노드가 없는(건너뛴) 포트는 */
		if (!child)
			/* [한국어] 넘어간다. */
			continue;

		ret = mvebu_pcie_powerup(port);
		/* [한국어] 전원을 켜는 데 실패하면 이 포트는 포기하고 다음으로 간다.
		 * 포트 하나가 실패해도 나머지로 진행하는 것이 이 드라이버의 방침이다. */
		if (ret < 0)
			continue;

		port->base = mvebu_pcie_map_registers(pdev, child, port);
		/* [한국어] 레지스터 매핑에 실패했으면 */
		if (IS_ERR(port->base)) {
			/* [한국어] 남기고 */
			dev_err(dev, "%s: cannot map registers\n", port->name);
			/* [한국어] base 를 NULL 로 만들어 이후 코드가 이 포트를 건너뛰게 한다. */
			port->base = NULL;
			/* [한국어] 전원을 되돌린다. */
			mvebu_pcie_powerdown(port);
			continue;
		}

		ret = mvebu_pci_bridge_emul_init(port);
		/* [한국어] 브리지 에뮬레이션 초기화에 실패했으면 */
		if (ret < 0) {
			/* [한국어] 남기고 */
			dev_err(dev, "%s: cannot init emulated bridge\n",
				/* [한국어] 어느 포트인지 함께 찍는다. */
				port->name);
			devm_iounmap(dev, port->base);
			/* [한국어] 역시 base 를 비우고 */
			port->base = NULL;
			/* [한국어] 전원을 내린다. 가짜 브리지가 없으면 이 포트는 열거될 수 없다. */
			mvebu_pcie_powerdown(port);
			continue;
		}

		if (irq > 0) {
			/* [한국어] INTx IRQ 가 있는 포트만 도메인을 만든다(윗줄이 그 조건). */
			ret = mvebu_pcie_init_irq_domain(port);
			/* [한국어] 실패했으면 */
			if (ret) {
				/* [한국어] 남기고 */
				dev_err(dev, "%s: cannot init irq domain\n",
					/* [한국어] 어느 포트인지 찍는다. */
					port->name);
				pci_bridge_emul_cleanup(&port->bridge);
				devm_iounmap(dev, port->base);
				/* [한국어] base 를 비우고 */
				port->base = NULL;
				/* [한국어] 전원을 내린다. */
				mvebu_pcie_powerdown(port);
				continue;
			}
			irq_set_chained_handler_and_data(irq,
							 mvebu_pcie_irq_handler,
							 port);
		}

		/*
		 * PCIe topology exported by mvebu hw is quite complicated. In
		 * reality has something like N fully independent host bridges
		 * where each host bridge has one PCIe Root Port (which acts as
		 * PCI Bridge device). Each host bridge has its own independent
		 * internal registers, independent access to PCI config space,
		 * independent interrupt lines, independent window and memory
		 * access configuration. But additionally there is some kind of
		 * peer-to-peer support between PCIe devices behind different
		 * host bridges limited just to forwarding of memory and I/O
		 * transactions (forwarding of error messages and config cycles
		 * is not supported). So we could say there are N independent
		 * PCIe Root Complexes.
		 *
		 * For this kind of setup DT should have been structured into
		 * N independent PCIe controllers / host bridges. But instead
		 * structure in past was defined to put PCIe Root Ports of all
		 * host bridges into one bus zero, like in classic multi-port
		 * Root Complex setup with just one host bridge.
		 *
		 * This means that pci-mvebu.c driver provides "virtual" bus 0
		 * on which registers all PCIe Root Ports (PCI Bridge devices)
		 * specified in DT by their BDF addresses and virtually routes
		 * PCI config access of each PCI bridge device to specific PCIe
		 * host bridge.
		 *
		 * Normally PCI Bridge should choose between Type 0 and Type 1
		 * config requests based on primary and secondary bus numbers
		 * configured on the bridge itself. But because mvebu PCI Bridge
		 * does not have registers for primary and secondary bus numbers
		 * in its config space, it determinates type of config requests
		 * via its own custom way.
		 *
		 * There are two options how mvebu determinate type of config
		 * request.
		 *
		 * 1. If Secondary Bus Number Enable bit is not set or is not
		 * available (applies for pre-XP PCIe controllers) then Type 0
		 * is used if target bus number equals Local Bus Number (bits
		 * [15:8] in register 0x1a04) and target device number differs
		 * from Local Device Number (bits [20:16] in register 0x1a04).
		 * Type 1 is used if target bus number differs from Local Bus
		 * Number. And when target bus number equals Local Bus Number
		 * and target device equals Local Device Number then request is
		 * routed to Local PCI Bridge (PCIe Root Port).
		 *
		 * 2. If Secondary Bus Number Enable bit is set (bit 7 in
		 * register 0x1a2c) then mvebu hw determinate type of config
		 * request like compliant PCI Bridge based on primary bus number
		 * which is configured via Local Bus Number (bits [15:8] in
		 * register 0x1a04) and secondary bus number which is configured
		 * via Secondary Bus Number (bits [7:0] in register 0x1a2c).
		 * Local PCI Bridge (PCIe Root Port) is available on primary bus
		 * as device with Local Device Number (bits [20:16] in register
		 * 0x1a04).
		 *
		 * Secondary Bus Number Enable bit is disabled by default and
		 * option 2. is not available on pre-XP PCIe controllers. Hence
		 * this driver always use option 1.
		 *
		 * Basically it means that primary and secondary buses shares
		 * one virtual number configured via Local Bus Number bits and
		 * Local Device Number bits determinates if accessing primary
		 * or secondary bus. Set Local Device Number to 1 and redirect
		 * all writes of PCI Bridge Secondary Bus Number register to
		 * Local Bus Number (bits [15:8] in register 0x1a04).
		 *
		 * So when accessing devices on buses behind secondary bus
		 * number it would work correctly. And also when accessing
		 * device 0 at secondary bus number via config space would be
		 * correctly routed to secondary bus. Due to issues described
		 * in mvebu_pcie_setup_hw(), PCI Bridges at primary bus (zero)
		 * are not accessed directly via PCI config space but rarher
		 * indirectly via kernel emulated PCI bridge driver.
		 */
		mvebu_pcie_setup_hw(port);
		mvebu_pcie_set_local_dev_nr(port, 1);
		/* [한국어] 로컬 버스 번호를 0 으로 맞춘다. 커널이 열거하며 제 값을 배정할 것이므로
		 * 초기값을 통일해 두는 것이다. */
		mvebu_pcie_set_local_bus_nr(port, 0);
	/* [한국어] 포트 초기화 루프 끝. */
	}

	bridge->sysdata = pcie;
	/* [한국어] 루트 버스(가짜 브리지)용 콜백 표. */
	bridge->ops = &mvebu_pcie_ops;
	/* [한국어] 그 아래 실제 장치용 콜백 표. 두 벌을 쓰는 것이 이 드라이버의 특징이며,
	 * PCI 코어가 버스 깊이에 따라 알아서 갈라 쓴다. */
	bridge->child_ops = &mvebu_pcie_child_ops;
	/* [한국어] 자원 정렬 콜백. mbus 창 제약을 BAR 배정에 반영한다. */
	bridge->align_resource = mvebu_pcie_align_resource;
	/* [한국어] INTx 사상 콜백. */
	bridge->map_irq = mvebu_pcie_map_irq;
/* [한국어] 아래에서 PCI 코어에 열거를 넘긴다. */

	return pci_host_probe(bridge);
}

/* [한국어]
 * mvebu_pcie_remove - 버스를 걷어내고 포트들을 정리한다
 *
 * @pdev: 제거되는 플랫폼 장치.   @return: 없음.
 *
 * probe 의 역순으로 정리한다.
 *
 *   1) pci_stop_root_bus() 와 pci_remove_root_bus() 로 버스를 해체한다.
 *      아래 장치의 드라이버가 먼저 떨어져야 그들이 잡은 자원이 풀린다.
 *   2) 포트마다
 *      - INTx 를 전부 마스크하고 원인을 지운다. 핸들러를 떼기 전에
 *        인터럽트를 막아야 떼는 도중 들어오지 않는다.
 *      - 체인 핸들러를 뗀다.
 *      - Command 레지스터에서 IO/MEM/BusMaster 를 끈다. 이 포트가 더 이상
 *        트랜잭션을 내지 않게 하는 것이다.
 *      - 브리지 에뮬레이션을 정리하고 irq_domain 을 없앤다.
 *      - mbus 창을 지우고 전원을 내린다.
 *
 * 창을 지우는 것이 중요하다. 남겨 두면 다음에 이 주소 구간을 쓰려는
 * 다른 주체와 충돌한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(언바인딩).
 *
 * 호출 체인:  플랫폼 버스 → [이 함수] → pci_stop_root_bus()
 *               → pci_bridge_emul_cleanup() → mvebu_pcie_powerdown()
 */
static void mvebu_pcie_remove(struct platform_device *pdev)
{
	struct mvebu_pcie *pcie = platform_get_drvdata(pdev);
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie);
	/* [한국어] Command 와 슬롯 전력 값을 담을 변수. */
	u32 cmd, sspl;
	/* [한국어] 포트 반복자. */
	int i;

	/* Remove PCI bus with all devices. */
	pci_lock_rescan_remove();
	pci_stop_root_bus(bridge->bus);
	pci_remove_root_bus(bridge->bus);
	pci_unlock_rescan_remove();

	for (i = 0; i < pcie->nports; i++) {
		/* [한국어] 이번 포트. */
		struct mvebu_pcie_port *port = &pcie->ports[i];
		/* [한국어] 이 포트의 INTx IRQ. */
		int irq = port->intx_irq;

		if (!port->base)
			/* [한국어] 초기화되지 못한 포트는 건너뛴다. */
			continue;

		/* Disable Root Bridge I/O space, memory space and bus mastering. */
		cmd = mvebu_readl(port, PCIE_CMD_OFF);
		cmd &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
		/* [한국어] IO/MEM/BusMaster 를 끈 Command 를 쓴다(윗줄에서 조립).
		 * 이 포트가 더 이상 트랜잭션을 내지 않게 하는 것이다. */
		mvebu_writel(port, cmd, PCIE_CMD_OFF);
/* [한국어] 아래에서 인터럽트와 창을 정리한다. */

		/* Mask all interrupt sources. */
		mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_UNMASK_OFF);

		/* Clear all interrupt causes. */
		mvebu_writel(port, ~PCIE_INT_ALL_MASK, PCIE_INT_CAUSE_OFF);

		if (irq > 0)
			/* [한국어] 체인 핸들러를 뗀다. 바로 앞에서 INTx 를 전부 마스크했으므로
			 * 떼는 도중 인터럽트가 들어오지 않는다. */
			irq_set_chained_handler_and_data(irq, NULL, NULL);

		/* Remove IRQ domains. */
		if (port->intx_irq_domain)
			irq_domain_remove(port->intx_irq_domain);

		/* Free config space for emulated root bridge. */
		pci_bridge_emul_cleanup(&port->bridge);

		/* Disable sending Set_Slot_Power_Limit PCIe Message. */
		sspl = mvebu_readl(port, PCIE_SSPL_OFF);
		sspl &= ~(PCIE_SSPL_VALUE_MASK | PCIE_SSPL_SCALE_MASK | PCIE_SSPL_ENABLE);
		/* [한국어] 슬롯 전력 제한을 끈 값을 쓴다(윗줄에서 활성 비트를 지웠다). */
		mvebu_writel(port, sspl, PCIE_SSPL_OFF);
/* [한국어] 아래에서 mbus 창을 지운다. */

		/* Disable and clear BARs and windows. */
		mvebu_pcie_disable_wins(port);

		/* Delete PCIe IO and MEM windows. */
		if (port->iowin.size)
			mvebu_pcie_del_windows(port, port->iowin.base, port->iowin.size);
		/* [한국어] 메모리 창이 열려 있으면 */
		if (port->memwin.size)
			/* [한국어] 지운다. 남겨 두면 다음에 이 주소 구간을 쓰려는 주체와 충돌한다. */
			mvebu_pcie_del_windows(port, port->memwin.base, port->memwin.size);
/* [한국어] 아래에서 I/O 창도 같은 방식으로 정리한다. */

		/* Power down card and disable clocks. Must be the last step. */
		mvebu_pcie_powerdown(port);
	}
}

static const struct of_device_id mvebu_pcie_of_match_table[] = {
	/* [한국어] Armada XP 용 compatible. */
	{ .compatible = "marvell,armada-xp-pcie", },
	/* [한국어] Armada 370 용. 이 표에 없는 노드에는 붙지 않는다. */
	{ .compatible = "marvell,armada-370-pcie", },
	{ .compatible = "marvell,dove-pcie", },
	{ .compatible = "marvell,kirkwood-pcie", },
	{},
};
MODULE_DEVICE_TABLE(of, mvebu_pcie_of_match_table);

static const struct dev_pm_ops mvebu_pcie_pm_ops = {
	/* [한국어] NOIRQ 단계에서 절전/재개를 처리한다. 인터럽트가 꺼진 단계여야
	 * 버스 번호를 되쓰는 동안 config 접근이 끼어들지 않는다. */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(mvebu_pcie_suspend, mvebu_pcie_resume)
/* [한국어] PM 콜백 표 끝. */
};

static struct platform_driver mvebu_pcie_driver = {
	/* [한국어] 플랫폼 드라이버 서술자. */
	.driver = {
		/* [한국어] 드라이버 이름. */
		.name = "mvebu-pcie",
		/* [한국어] 위 compatible 표를 건다. */
		.of_match_table = mvebu_pcie_of_match_table,
		.pm = &mvebu_pcie_pm_ops,
	},
	.probe = mvebu_pcie_probe,
	.remove = mvebu_pcie_remove,
};
module_platform_driver(mvebu_pcie_driver);

MODULE_AUTHOR("Thomas Petazzoni <thomas.petazzoni@bootlin.com>");
MODULE_AUTHOR("Pali Rohár <pali@kernel.org>");
MODULE_DESCRIPTION("Marvell EBU PCIe controller");
MODULE_LICENSE("GPL v2");
