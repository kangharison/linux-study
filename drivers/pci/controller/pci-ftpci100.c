// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Faraday Technology FTPC100 PCI Controller
 *
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 *
 * Based on the out-of-tree OpenWRT patch for Cortina Gemini:
 * Copyright (C) 2009 Janos Laube <janos.dev@gmail.com>
 * Copyright (C) 2009 Paulius Zaleckas <paulius.zaleckas@teltonika.lt>
 * Based on SL2312 PCI controller code
 * Storlink (C) 2003
 */

/*
 * [한국어 설명] Faraday Technology FTPCI100 PCI(비-Express) 호스트 컨트롤러
 * 드라이버 — Cortina Gemini 등에 얹히는 옛 세대 IP (pci-ftpci100.c)
 *
 * === 파일의 역할 ===
 * Faraday 의 FTPCI100 PCI 호스트 브리지 IP 를 다루는 드라이버다. 이름에
 * "PCIe" 가 없는 데서 드러나듯 PCI Express 가 아니라 **병렬 버스 PCI** 이며,
 * 같은 디렉터리의 pci-ixp4xx.c 와 같은 세대에 속한다. 상류 헤더가 밝히듯
 * Cortina Gemini 용 OpenWRT 패치에서 출발한 코드다.
 *
 * 세대의 성격은 pci-ixp4xx.c 와 같다 — 링크 훈련도 LTSSM 도 MSI 도 없고,
 * 설정공간 접근은 주소 레지스터에 쓰고 데이터 레지스터를 읽는 간접 방식이다.
 * 다만 같은 세대라도 구현 선택이 여러 곳에서 갈리며, 그 대비가 이 파일을
 * 읽는 좋은 축이다(아래 별도 절 참조).
 *
 * 이 파일이 맡는 것은 넷이다.
 *   1. 설정공간 접근. CF8 형식 주소를 FTPCI_CONFIG 에 쓰고 FTPCI_DATA 를
 *      읽거나 쓴다. 컨트롤러 자신의 레지스터도 같은 통로로 만진다 —
 *      버스 0, devfn 0 으로 자기 자신을 지목하는 것이며, 별도의 창구를 둔
 *      pci-ixp4xx.c 와 다른 점이다.
 *   2. INTx 인터럽트. 브리지 안에 인터럽트 컨트롤러가 들어 있어, 네 INTx 선이
 *      상위 IRQ 하나로 모여 올라온다. 그것을 연쇄 핸들러로 받아 도메인으로
 *      흩는다. irq_chip 의 ack/mask/unmask 가 모두 **설정공간 접근** 으로
 *      구현된다는 점이 이 파일에서 가장 독특한 부분이다.
 *   3. 주소 창. dma-ranges 를 읽어 PCI 쪽에서 들어오는 접근이 닿을 메모리
 *      구간 셋을 설정한다. 크기가 1MB~2GB 의 2의 거듭제곱이어야 한다.
 *   4. 버스 클럭. 33MHz 로 도는 버스가 66MHz 를 감당할 수 있으면 올린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시 흐름은 이렇다.
 *
 *   플랫폼 드라이버 코어 → faraday_pci_probe()
 *     → of_device_get_match_data()      : 판본(연쇄 인터럽트 유무)
 *     → devm_pci_alloc_host_bridge()    : 브리지와 이 드라이버의 상태를 함께 잡는다
 *     → devm_clk_get_enabled() x2       : PCLK 와 PCICLK
 *     → devm_platform_ioremap_resource(): 유일한 레지스터 창
 *     → IO 창 크기 설정                  : DT 의 IO ranges → FTPCI_IOSIZE
 *     → FTPCI_CTRL 에 IO/MEM/MASTER 켜기
 *     → 인터럽트 상태 지우고 전부 마스크
 *     → (판본에 따라) faraday_pci_setup_cascaded_irq()
 *     → 33MHz/66MHz 판정과 클럭 올리기
 *     → faraday_pci_parse_map_dma_ranges()
 *     → pci_scan_root_bus_bridge() → pci_bus_assign_resources() → pci_bus_add_devices()
 *
 * 설정공간 접근 경로는 하나뿐이며 안팎을 가리지 않는다.
 *
 *   (바깥 장치) PCI 코어 → faraday_pci_ops.read/write
 *                → faraday_pci_read_config() / write_config()
 *                → faraday_raw_pci_read_config() / write_config()
 *                → FTPCI_CONFIG 에 주소 쓰기 → FTPCI_DATA 읽기/쓰기
 *
 *   (자기 자신) 이 파일 안에서 직접
 *                → faraday_raw_pci_read_config(p, 0, 0, ...) 처럼
 *                  버스 0·devfn 0 을 지정해 같은 통로로
 *
 * 인터럽트 경로는 이렇다.
 *
 *   상위 인터럽트 컨트롤러 → faraday_pci_irq_handler()  [연쇄 핸들러]
 *     → 자기 설정공간의 CTRL2 를 읽어 비트 31:28 에서 INTD~INTA 상태를 뽑고
 *     → generic_handle_domain_irq(p->irqdomain, 0~3)
 *     → 하위 장치 드라이버의 핸들러
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트다. 연쇄 핸들러와 irq_chip 콜백
 * 셋은 인터럽트 컨텍스트에서 돌며, 그 안에서 설정공간 읽기-수정-쓰기를 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. pci_scan_root_bus_bridge() 로 버스를 만들고, 자원 배정과
 *   장치 등록을 이 파일이 직접 이어서 부른다 — pci_host_probe() 한 줄로
 *   끝내는 요즘 드라이버들과 다른 옛 방식이다.
 * 아래쪽: 없다. 레지스터를 직접 두드린다.
 * 옆쪽: 클럭 프레임워크(PCLK/PCICLK)와 irqdomain/irqchip. ARM 아키텍처 코드에
 *   직접 손을 뻗지 않으므로 이 파일에는 #ifdef 가 하나도 없다 —
 *   예외 핸들러를 거느라 CONFIG_ARM 갈래가 넷이나 있는 pci-ixp4xx.c 와
 *   대비된다.
 *
 * 데이터 흐름:
 *   DT compatible → variant.cascaded_irq → 인터럽트 설정 여부
 *   DT reg → p->base → 설정공간 창구와 IO 크기 레지스터
 *   DT ranges(IO) → faraday_res_to_memcfg() → FTPCI_IOSIZE
 *   DT dma-ranges → faraday_res_to_memcfg() → MEM1/2/3 BASE_SIZE
 *   CTRL2 의 비트 31:28 → INTx 상태 → 도메인 hwirq 0~3
 *
 * 공유 상태: struct faraday_pci 하나. probe 후 사실상 불변이며 잠금이 하나도
 *   없다. 다만 irq_chip 콜백 셋이 CTRL2 를 읽기-수정-쓰기 하는데 그것을
 *   보호하는 잠금이 없다 — 같은 자리를 raw 스핀락으로 감싸는
 *   pcie-xilinx-cpm.c 와 대비되는 지점이다.
 *
 * === 주요 함수/구조체 요약 ===
 * faraday_pci_probe()              : 진입점. 이 파일에서 가장 길다.
 * faraday_raw_pci_read_config()    : 간접 설정공간 읽기. 안팎 모두 이것을 쓴다.
 * faraday_raw_pci_write_config()   : 그 쓰기 판. 크기별로 다른 MMIO 접근을 쓴다.
 * faraday_res_to_memcfg()          : 주소·크기를 이 하드웨어의 창 설정값으로 옮긴다.
 * faraday_pci_ack_irq()            : INTx 상태 비트를 지운다(설정공간 접근으로).
 * faraday_pci_mask_irq()/unmask()  : INTx 마스크 비트를 지우고 세운다.
 * faraday_pci_irq_handler()        : 연쇄 핸들러. CTRL2 상태를 도메인으로 흩는다.
 * faraday_pci_setup_cascaded_irq() : 도메인을 만들고 연쇄 핸들러를 꽂는다.
 * faraday_pci_parse_map_dma_ranges(): 인바운드 창 셋을 설정한다.
 * struct faraday_pci               : 이 드라이버의 상태. 필드가 다섯이다.
 * struct faraday_pci_variant       : 판본 차이. 필드가 하나뿐이다.
 *
 * === 같은 세대인 pci-ixp4xx.c 와 견주어 본 이 파일 ===
 * 두 파일 다 PCI_CONF1_ADDRESS 매크로로 CF8 형식 주소를 만들지만, 그 뒤에
 * 하는 일이 다르다.
 *   - 이 파일: 매크로가 만든 값을 **손대지 않고 그대로** FTPCI_CONFIG 에
 *     쓴다. enable 비트를 지우지도 않고, type 0/type 1 을 가르지도 않으며,
 *     IDSEL 주소선을 세우지도 않는다. 버스 번호와 슬롯 번호가 언제나 CF8 의
 *     제자리에 그대로 들어간다.
 *   - pci-ixp4xx.c: enable 비트를 지운 뒤, 버스 0 이면 슬롯 번호를
 *     BIT(32 - 슬롯) 이라는 IDSEL 비트로 바꿔 얹고, 아니면 맨 아래 비트에 1 을
 *     세워 type 1 임을 표시한다.
 * 즉 "같은 매크로를 쓴다" 는 말은 맞지만 "같은 방식으로 쓴다" 고 하기는
 * 어렵다. 슬롯 선택(IDSEL)을 컨트롤러가 알아서 하느냐 드라이버가 주소로
 * 만들어 주느냐가 갈리는 것으로 보이나, 그 근거를 이 트리에서 확인할 수는 없다.
 *
 * 그 밖에도 여러 곳이 갈린다.
 *   - 바이트 단위 접근: 이 파일은 크기에 따라 writel/writew/writeb 를 골라
 *     쓰고 데이터 레지스터 주소에 (config & 3) 을 더한다. pci-ixp4xx.c 는
 *     바이트 인에이블 비트를 계산해 명령 레지스터에 실어 보낸다. 읽기는
 *     두 파일 다 워드를 통째로 받아 소프트웨어로 자른다.
 *   - 자기 설정공간: 이 파일은 바깥 장치와 같은 통로를 쓴다.
 *     pci-ixp4xx.c 는 CRP 라는 별도 창구를 둔다.
 *   - 오류 처리: 이 파일에는 마스터 어보트 확인이 없다. pci-ixp4xx.c 는
 *     매 접근마다 ISR 을 확인하고 ARM 예외 핸들러까지 건다.
 *   - 인터럽트: 이 파일은 브리지 내장 인터럽트 컨트롤러의 INTx 를 도메인으로
 *     다룬다. pci-ixp4xx.c 에는 인터럽트 도메인이 아예 없다.
 */

/* [한국어] __init 계열 헤더. 이 파일이 직접 쓰는 이름은 없다(전수 확인). */
#include <linux/init.h>
/* [한국어] 인터럽트 헤더. 연쇄 핸들러와 irq_chip 을 다루므로 필요하다. */
#include <linux/interrupt.h>
/* [한국어] readl()/writel()/writew()/writeb(). 이 컨트롤러는 PCI 쪽과 같은
 * 리틀엔디안 접근을 쓰므로 표준 접근자로 충분하다 — CPU 엔디안에 따라
 * __raw_ 판을 써야 했던 pci-ixp4xx.c 와 다른 점이다. */
#include <linux/io.h>
/* [한국어] 기본 커널 관용구. */
#include <linux/kernel.h>
/* [한국어] of_device_get_match_data() 와 of_get_next_child(). 판본 갈림과
 * 인터럽트 컨트롤러 자식 노드 찾기가 여기서 온다. */
#include <linux/of.h>
/* [한국어] of_irq_get(). 자식 노드에 적힌 상위 인터럽트를 가져온다. */
#include <linux/of_irq.h>
/* [한국어] of_pci 계열 헤더. 이 파일이 직접 쓰는 이름은 없다(전수 확인). */
#include <linux/of_pci.h>
/* [한국어] PCI_SLOT()/PCI_FUNC(), PCI_CONF1_ADDRESS, PCI_COMMAND_ 계열 비트,
 * PCI_SPEED_33MHz/66MHz, PCIBIOS_ 반환 코드. */
#include <linux/pci.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource(),
 * builtin_platform_driver() 매크로. */
#include <linux/platform_device.h>
/* [한국어] slab 헤더. 이 파일은 직접 할당하지 않는다 — 상태 구조체를
 * 브리지 뒤에 딸려 잡기 때문이다(전수 확인). */
#include <linux/slab.h>
/* [한국어] irq_domain_create_linear(), irq_create_mapping(),
 * generic_handle_domain_irq(). 이 파일은 도메인을 하나 만든다(INTx). */
#include <linux/irqdomain.h>
/* [한국어] chained_irq_enter()/exit(). 연쇄 핸들러가 상위 컨트롤러의 처리를
 * 열고 닫는 데 쓴다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] BIT() 매크로. 인터럽트 상태·마스크 비트 계산에 쓴다. */
#include <linux/bitops.h>
/* [한국어] irq_set_chip_and_handler(), handle_level_irq(), irqd_to_hwirq() 등
 * irq_chip 계층의 기본 정의. */
#include <linux/irq.h>
/* [한국어] clk_get_rate()/clk_set_rate()/devm_clk_get_enabled().
 * 33MHz 버스를 66MHz 로 올리는 판정에 쓴다. */
#include <linux/clk.h>

/* [한국어] drivers/pci 내부 선언. 이 파일이 여기서 얻는 것은 없어 보이지만
 * 상류가 포함해 두었다. */
#include "../pci.h"

/*
 * Special configuration registers directly in the first few words
 * in I/O space.
 */
/* [한국어] 위 상류 주석대로 아래 여섯은 컨트롤러 창(p->base) 맨 앞의
 * 특수 레지스터들이다. 설정공간 오프셋이 아니라 MMIO 오프셋이라는 점에
 * 주의할 것 — 아래 FARADAY_PCI_ 무리와 성격이 다르다. */
#define FTPCI_IOSIZE	0x00
/* [한국어] IO 공간 크기 설정. probe 가 DT 의 IO ranges 를 옮겨 담는다. */
#define FTPCI_PROT	0x04 /* AHB protection */
/* [한국어] AHB 보호 설정(옆의 상류 주석). 이 파일에서 쓰이지 않는다(전수 확인). */
#define FTPCI_CTRL	0x08 /* PCI control signal */
/* [한국어] PCI 제어 신호(옆의 상류 주석). probe 가 여기에 IO/메모리/마스터
 * 비트를 세워 컨트롤러를 버스 마스터로 만든다. */
#define FTPCI_SOFTRST	0x10 /* Soft reset counter and response error enable */
/* [한국어] 소프트 리셋 카운터와 응답 오류 인에이블(옆의 상류 주석).
 * 이 파일에서 쓰이지 않는다 — 이 드라이버는 버스 리셋을 걸지 않는다. */
#define FTPCI_CONFIG	0x28 /* PCI configuration command register */
/* [한국어] 설정 사이클 주소 레지스터(옆의 상류 주석). CF8 형식 주소를 여기
 * 쓰면 설정 사이클이 준비된다. 이 파일의 모든 설정공간 접근이 여기서 시작한다. */
#define FTPCI_DATA	0x2C
/* [한국어] 설정 사이클 데이터 레지스터. 위 주소를 쓴 뒤 이것을 읽거나 쓰면
 * 실제 트랜잭션이 나간다. 쓰기에서는 크기에 따라 이 주소에 (오프셋 & 3) 을
 * 더해 writew/writeb 로 접근하는 것이 이 하드웨어의 바이트 지정 방식이다. */

/* [한국어] 아래 여덟은 컨트롤러 자신의 **설정공간** 오프셋이다. 위 FTPCI_
 * 무리(MMIO 오프셋)와 헷갈리지 말 것 — 이쪽은 버스 0·devfn 0 을 지목한
 * 설정 사이클로 접근한다. */
#define FARADAY_PCI_STATUS_CMD		0x04 /* Status and command */
/* [한국어] 전원관리 능력 구조(설정공간 0x40). 이 파일에서 쓰이지 않는다. */
#define FARADAY_PCI_PMC			0x40 /* Power management control */
/* [한국어] 전원관리 상태·제어(0x44). 역시 쓰이지 않는다. */
#define FARADAY_PCI_PMCSR		0x44 /* Power management status */
/* [한국어] 제어 레지스터 1(0x48). 쓰이지 않는다. */
#define FARADAY_PCI_CTRL1		0x48 /* Control register 1 */
/* [한국어] 제어 레지스터 2(0x4C). 이 파일에서 가장 많이 쓰이는 레지스터로,
 * INTx 의 상태·마스크가 모두 여기 있다. irq_chip 콜백 셋과 연쇄 핸들러가
 * 이것 하나를 두고 움직인다. */
#define FARADAY_PCI_CTRL2		0x4C /* Control register 2 */
/* [한국어] 인바운드 메모리 창 1 의 베이스·크기(0x50). dma-ranges 의 첫 항목이 온다. */
#define FARADAY_PCI_MEM1_BASE_SIZE	0x50 /* Memory base and size #1 */
/* [한국어] 인바운드 메모리 창 2(0x54). */
#define FARADAY_PCI_MEM2_BASE_SIZE	0x54 /* Memory base and size #2 */
/* [한국어] 인바운드 메모리 창 3(0x58). 창이 셋뿐이라 dma-ranges 도 셋까지만 받는다. */
#define FARADAY_PCI_MEM3_BASE_SIZE	0x58 /* Memory base and size #3 */

/* [한국어] 표준 PCI 상태 레지스터의 66MHz 지원 비트(21). probe 가 이 비트를
 * 보고 버스를 33MHz 에서 66MHz 로 올릴지 판단한다. */
#define PCI_STATUS_66MHZ_CAPABLE	BIT(21)

/* [한국어] 아래는 위 CTRL2 레지스터의 비트 배치다. 상류 주석이 필드별로
 * 자리를 적어 두었다. */
/* Bits 31..28 gives INTD..INTA status */
/* [한국어] INTx 상태 필드의 시작 비트(28). 옆의 상류 주석대로 비트 31~28 이
 * INTD~INTA 이며, hwirq 0~3 에 이 값을 더하면 그 자리가 된다.
 * write-1-to-clear 라 1 을 쓰면 지워진다. */
#define PCI_CTRL2_INTSTS_SHIFT		28
/* [한국어] 명령 오류 인터럽트 마스크(비트 27). 이 파일에서 쓰이지 않는다. */
#define PCI_CTRL2_INTMASK_CMDERR	BIT(27)
/* [한국어] 패리티 오류 인터럽트 마스크(비트 26). 쓰이지 않는다. */
#define PCI_CTRL2_INTMASK_PARERR	BIT(26)
/* Bits 25..22 masks INTD..INTA */
/* [한국어] INTx 마스크 필드의 시작 비트(22). 옆의 상류 주석대로 비트 25~22 가
 * INTD~INTA 를 가린다. 이 필드에서는 비트를 **세워야** 인터럽트가 통과하므로,
 * mask 콜백이 지우고 unmask 콜백이 세운다. */
#define PCI_CTRL2_INTMASK_SHIFT		22
/* [한국어] 마스터 어보트 수신 인터럽트 마스크(비트 21). 쓰이지 않는다 —
 * 이 드라이버는 마스터 어보트를 인터럽트로도 예외로도 받지 않는다. */
#define PCI_CTRL2_INTMASK_MABRT_RX	BIT(21)
/* [한국어] 타깃 어보트 수신 인터럽트 마스크(비트 20). 쓰이지 않는다. */
#define PCI_CTRL2_INTMASK_TABRT_RX	BIT(20)
/* [한국어] 타깃 어보트 송신 인터럽트 마스크(비트 19). 쓰이지 않는다. */
#define PCI_CTRL2_INTMASK_TABRT_TX	BIT(19)
/* [한국어] 재시도 4회 초과 인터럽트 마스크(비트 18). 쓰이지 않는다. */
#define PCI_CTRL2_INTMASK_RETRY4	BIT(18)
/* [한국어] SERR# 수신 인터럽트 마스크(비트 17). 쓰이지 않는다. */
#define PCI_CTRL2_INTMASK_SERR_RX	BIT(17)
/* [한국어] PERR# 수신 인터럽트 마스크(비트 16). 쓰이지 않는다.
 * [상류 코드 관찰] 위 오류 관련 마스크 여덟 개가 모두 정의만 되어 있고
 * 이 파일 어디에서도 쓰이지 않는다(전수 확인). probe 가 CTRL2 의 상위 절반에
 * 0xF000 을 통째로 써서 이 비트들을 한꺼번에 0 으로 만들 뿐이다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
#define PCI_CTRL2_INTMASK_PERR_RX	BIT(16)
/* Bit 15 reserved */
/* [한국어] 아래 일곱은 AHB 마스터 요청 0~6 의 우선순위 비트(14~8)다.
 * 일곱 모두 이 파일에서 쓰이지 않는다 — 버스 중재 우선순위를 이 드라이버가
 * 건드리지 않고 부트로더나 하드웨어 기본값에 맡긴다는 뜻이다.
 * 요청 6 의 우선순위(비트 14). */
#define PCI_CTRL2_MSTPRI_REQ6		BIT(14)
/* [한국어] 요청 5 의 우선순위(비트 13). */
#define PCI_CTRL2_MSTPRI_REQ5		BIT(13)
/* [한국어] 요청 4 의 우선순위(비트 12). */
#define PCI_CTRL2_MSTPRI_REQ4		BIT(12)
/* [한국어] 요청 3 의 우선순위(비트 11). */
#define PCI_CTRL2_MSTPRI_REQ3		BIT(11)
/* [한국어] 요청 2 의 우선순위(비트 10). */
#define PCI_CTRL2_MSTPRI_REQ2		BIT(10)
/* [한국어] 요청 1 의 우선순위(비트 9). */
#define PCI_CTRL2_MSTPRI_REQ1		BIT(9)
/* [한국어] 요청 0 의 우선순위(비트 8). */
#define PCI_CTRL2_MSTPRI_REQ0		BIT(8)
/* Bits 7..4 reserved */
/* Bits 3..0 TRDYW */

/*
 * Memory configs:
 * Bit 31..20 defines the PCI side memory base
 * Bit 19..16 (4 bits) defines the size per below
 */
/* [한국어] 위 상류 주석대로 메모리 창 설정값은 한 워드에 두 필드를 담는다 —
 * 상위 12비트가 PCI 쪽 베이스 주소, 그 아래 4비트가 크기 코드다.
 * 베이스가 차지하는 비트 31:20 의 마스크. 1MB 단위로만 표현되므로 그보다
 * 잘게 나눈 주소는 아래 faraday_res_to_memcfg() 가 경고와 함께 잘라 낸다. */
#define FARADAY_PCI_MEMBASE_MASK	0xfff00000
/* [한국어] 아래 열둘은 크기 코드다. 1MB 부터 2GB 까지 2의 거듭제곱으로만
 * 표현되며, 그 사이 값은 표현할 수 없어 -EINVAL 이 된다. 코드 0 = 1MB. */
#define FARADAY_PCI_MEMSIZE_1MB		0x0
/* [한국어] 코드 1 = 2MB. */
#define FARADAY_PCI_MEMSIZE_2MB		0x1
/* [한국어] 코드 2 = 4MB. */
#define FARADAY_PCI_MEMSIZE_4MB		0x2
/* [한국어] 코드 3 = 8MB. */
#define FARADAY_PCI_MEMSIZE_8MB		0x3
/* [한국어] 코드 4 = 16MB. */
#define FARADAY_PCI_MEMSIZE_16MB	0x4
/* [한국어] 코드 5 = 32MB. */
#define FARADAY_PCI_MEMSIZE_32MB	0x5
/* [한국어] 코드 6 = 64MB. */
#define FARADAY_PCI_MEMSIZE_64MB	0x6
/* [한국어] 코드 7 = 128MB. */
#define FARADAY_PCI_MEMSIZE_128MB	0x7
/* [한국어] 코드 8 = 256MB. */
#define FARADAY_PCI_MEMSIZE_256MB	0x8
/* [한국어] 코드 9 = 512MB. */
#define FARADAY_PCI_MEMSIZE_512MB	0x9
/* [한국어] 코드 0xa = 1GB. */
#define FARADAY_PCI_MEMSIZE_1GB		0xa
/* [한국어] 코드 0xb = 2GB. 이것이 이 하드웨어가 표현할 수 있는 최대 창이다. */
#define FARADAY_PCI_MEMSIZE_2GB		0xb
/* [한국어] 위 크기 코드가 들어갈 자리의 시작 비트(16). 상류 주석의
 * "비트 19:16" 과 맞는다. */
#define FARADAY_PCI_MEMSIZE_SHIFT	16

/*
 * The DMA base is set to 0x0 for all memory segments, it reflects the
 * fact that the memory of the host system starts at 0x0.
 */
/* [한국어] 위 상류 주석이 이 세 상수의 뜻을 설명한다 — 호스트 메모리가
 * 0x0 에서 시작하므로 DMA 베이스도 0 이라는 것이다.
 * [상류 코드 관찰] 셋 다 이 파일 어디에서도 쓰이지 않는다(전수 확인).
 * 실제 베이스는 DT 의 dma-ranges 에서 계산되며, 이 상수들은 그 사실을
 * 문서화해 두는 역할만 한다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는
 * 손대지 않았다.
 * 창 1 의 DMA 베이스. */
#define FARADAY_PCI_DMA_MEM1_BASE	0x00000000
/* [한국어] 창 2 의 DMA 베이스. */
#define FARADAY_PCI_DMA_MEM2_BASE	0x00000000
/* [한국어] 창 3 의 DMA 베이스. */
#define FARADAY_PCI_DMA_MEM3_BASE	0x00000000

/**
 * struct faraday_pci_variant - encodes IP block differences
 * @cascaded_irq: this host has cascaded IRQs from an interrupt controller
 *	embedded in the host bridge.
 */
/* [한국어] 위 상류 kernel-doc 이 이 구조체와 그 유일한 필드를 설명한다.
 *
 * 판본 차이를 데이터로 표현하는 표이며, 필드가 하나뿐이라는 것이 이 IP 의
 * 두 판본이 그만큼 가깝다는 뜻이다. 인스턴스가 둘 있고(faraday_regular,
 * faraday_dual) of_device_get_match_data() 가 그중 하나를 고른다. */
struct faraday_pci_variant {
	/* [한국어] 이 판본이 브리지 내장 인터럽트 컨트롤러를 갖는지. 위 상류
	 * kernel-doc 이 그 뜻을 적어 두었다.
	 * 설정자: faraday_regular 는 true, faraday_dual 은 false.
	 * 읽는 자: faraday_pci_probe() 의 조건문 하나뿐이다.
	 * 값 범위: true 또는 false.
	 * 동기화: 정적 상수 표라 불변.
	 *
	 * false 인 판본에서는 도메인도 연쇄 핸들러도 만들지 않으므로, INTx 는
	 * DT 의 interrupt-map 을 통해 SoC 의 다른 인터럽트 컨트롤러가 직접 받는
	 * 구성으로 보인다. 다만 그 근거를 이 트리에서 확인할 수는 없다. */
	bool cascaded_irq;
};

/* [한국어] 이 드라이버의 상태 전부. 따로 할당하지 않고 호스트 브리지 뒤에
 * 딸려 잡히므로 이 파일에는 kzalloc 도 kfree 도 없다. */
struct faraday_pci {
	/* [한국어] 이 컨트롤러의 장치. 로그와 자원·클럭 조회에 쓴다.
	 * 설정자: faraday_pci_probe().
	 * 읽는 자: 거의 모든 함수의 dev_err/dev_info 와 DT 조회.
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: probe 후 불변. */
	struct device *dev;
	/* [한국어] 유일한 레지스터 창의 가상 주소.
	 * 설정자: faraday_pci_probe() 의 devm_platform_ioremap_resource(pdev, 0).
	 * 읽는 자: 설정공간 접근 두 함수(FTPCI_CONFIG/FTPCI_DATA)와
	 *   probe 의 FTPCI_IOSIZE/FTPCI_CTRL 접근.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. 잠금이 없다 — 설정공간 접근이 주소를 쓰고 데이터를
	 *   읽는 두 단계인데도 그렇다. 상위 PCI 코어가 설정공간 접근을 직렬화한다는
	 *   전제로 보이나, irq_chip 콜백들이 인터럽트 컨텍스트에서 같은 통로를 쓰므로
	 *   그 전제만으로 충분한지는 이 파일에 적혀 있지 않다. */
	void __iomem *base;
	/* [한국어] INTx IRQ 도메인. 크기가 PCI_NUM_INTX(4)다.
	 * 설정자: faraday_pci_setup_cascaded_irq() 의 irq_domain_create_linear().
	 * 읽는 자: 같은 함수의 irq_create_mapping() 과
	 *   faraday_pci_irq_handler() 의 generic_handle_domain_irq().
	 * 값 범위: 유효한 도메인 포인터. cascaded_irq 가 false 인 판본에서는
	 *   만들어지지 않아 NULL 로 남는다.
	 * 동기화: probe 에서 만들고 이후 불변. 없애는 코드는 이 파일에 없다 —
	 *   remove 경로가 없기 때문이다. */
	struct irq_domain *irqdomain;
	/* [한국어] 스캔으로 만들어진 루트 버스.
	 * 설정자: faraday_pci_probe() 가 pci_scan_root_bus_bridge() 뒤에 채운다.
	 * 읽는 자: 같은 함수가 곧바로 버스 속도를 기록하고 자원 배정·장치 등록에 쓴다.
	 * 값 범위: 유효한 pci_bus 포인터.
	 * 동기화: probe 후 불변.
	 *
	 * 이 필드가 있다는 것 자체가 옛 방식의 흔적이다. pci_host_probe() 한 줄로
	 * 스캔·배정·등록을 끝내는 요즘 드라이버들은 버스를 따로 들고 있을 필요가 없다. */
	struct pci_bus *bus;
	/* [한국어] PCI 버스 클럭("PCICLK").
	 * 설정자: faraday_pci_probe() 의 devm_clk_get_enabled().
	 * 읽는 자: 같은 함수의 clk_get_rate()/clk_set_rate() — 33MHz 버스를 66MHz 로
	 *   올릴 수 있는지 판정하고 실제로 올린다.
	 * 값 범위: 유효한 clk 포인터. 얻지 못하면 프로브가 실패하므로 오류 포인터가
	 *   이 필드에 남는 일은 없다.
	 * 동기화: probe 후 불변.
	 *
	 * 함께 얻는 PCLK 는 지역 변수에만 담긴다 — 켜 두기만 하면 되고 이후
	 * 만질 일이 없기 때문이다. 이 클럭만 구조체에 남기는 이유가 그 차이다. */
	struct clk *bus_clk;
};

/* [한국어]
 * faraday_res_to_memcfg - 주소·크기 한 쌍을 이 하드웨어의 창 설정값으로 옮긴다
 *
 * @mem_base: PCI 버스 쪽에서 본 창의 시작 주소.
 * @mem_size: 창의 크기. 1MB~2GB 의 2의 거듭제곱이어야 한다.
 * @val: 계산된 설정값을 담을 곳.
 * @return: 0 성공, -EINVAL 이면 표현할 수 없는 크기다.
 *
 * 이 하드웨어의 창 설정 레지스터는 한 워드에 두 필드를 담는다 — 상위 12비트가
 * 1MB 단위의 베이스 주소, 비트 19:16 이 크기 코드다. 이 함수가 그 워드를 만든다.
 *
 * 크기를 코드로 옮기는 방법이 switch 로 하나하나 나열하는 형태다. 2의 거듭제곱
 * 지수를 계산하면 될 일이지만, 상류 코드가 표를 그대로 펼쳐 두었다. 그 덕에
 * 지원 범위(1MB~2GB)가 코드에 그대로 드러난다.
 *
 * 쓰이는 곳이 둘이다. probe 가 IO 창 크기를 정할 때와,
 * faraday_pci_parse_map_dma_ranges() 가 인바운드 메모리 창 셋을 정할 때다.
 * 즉 아웃바운드 IO 와 인바운드 메모리가 같은 인코딩을 공유한다.
 *
 * [상류 코드 관찰] 로그에 dev_warn/dev_dbg 가 아니라 pr_warn/pr_debug 를 쓴다.
 * 인자로 struct device 를 받지 않아 장치 문맥이 없기 때문이며, 그래서 어느
 * 컨트롤러의 경고인지 로그만으로는 알 수 없다. 원본 스냅숏(1f0e418bb6)에서
 * 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] 베이스가 1MB 정렬이 아니면 옆의 상류 주석("This is probably
 * not good")과 함께 경고만 남기고 하위 비트를 조용히 버린 채 계속 진행한다.
 * 오류로 처리하지 않는다.
 *
 * 실행 컨텍스트: probe. 순수 계산이며 잠들지 않는다.
 *
 * 에러 경로: 표현할 수 없는 크기면 -EINVAL. 그 경우 val 은 채워지지 않는다.
 *
 * 호출 체인:
 *   faraday_pci_probe() / faraday_pci_parse_map_dma_ranges() → [이 함수]
 */
static int faraday_res_to_memcfg(resource_size_t mem_base,
				 resource_size_t mem_size, u32 *val)
{
	/* [한국어] 조립 중인 설정값. 크기 코드를 먼저 담았다가 베이스를 얹는다. */
	u32 outval;

	/* [한국어] 크기를 코드로 옮긴다. 2의 거듭제곱이 아닌 값은 아래 default 로 떨어진다. */
	switch (mem_size) {
	/* [한국어] 1MB. 이 하드웨어가 표현할 수 있는 가장 작은 창이다. */
	case SZ_1M:
		/* [한국어] 코드 0. */
		outval = FARADAY_PCI_MEMSIZE_1MB;
		break;
	/* [한국어] 2MB. */
	case SZ_2M:
		/* [한국어] 코드 1. */
		outval = FARADAY_PCI_MEMSIZE_2MB;
		break;
	/* [한국어] 4MB. */
	case SZ_4M:
		/* [한국어] 코드 2. */
		outval = FARADAY_PCI_MEMSIZE_4MB;
		break;
	/* [한국어] 8MB. */
	case SZ_8M:
		/* [한국어] 코드 3. */
		outval = FARADAY_PCI_MEMSIZE_8MB;
		break;
	/* [한국어] 16MB. */
	case SZ_16M:
		/* [한국어] 코드 4. */
		outval = FARADAY_PCI_MEMSIZE_16MB;
		break;
	/* [한국어] 32MB. */
	case SZ_32M:
		/* [한국어] 코드 5. */
		outval = FARADAY_PCI_MEMSIZE_32MB;
		break;
	/* [한국어] 64MB. */
	case SZ_64M:
		/* [한국어] 코드 6. */
		outval = FARADAY_PCI_MEMSIZE_64MB;
		break;
	/* [한국어] 128MB. */
	case SZ_128M:
		/* [한국어] 코드 7. */
		outval = FARADAY_PCI_MEMSIZE_128MB;
		break;
	/* [한국어] 256MB. */
	case SZ_256M:
		/* [한국어] 코드 8. */
		outval = FARADAY_PCI_MEMSIZE_256MB;
		break;
	/* [한국어] 512MB. */
	case SZ_512M:
		/* [한국어] 코드 9. */
		outval = FARADAY_PCI_MEMSIZE_512MB;
		break;
	/* [한국어] 1GB. */
	case SZ_1G:
		/* [한국어] 코드 0xa. */
		outval = FARADAY_PCI_MEMSIZE_1GB;
		break;
	/* [한국어] 2GB. 이 하드웨어가 표현할 수 있는 가장 큰 창이다. */
	case SZ_2G:
		/* [한국어] 코드 0xb. */
		outval = FARADAY_PCI_MEMSIZE_2GB;
		break;
	/* [한국어] 2의 거듭제곱이 아니거나 범위를 벗어난 크기. */
	default:
		/* [한국어] 표현할 수 없으므로 호출자가 프로브를 끊게 한다. */
		return -EINVAL;
	}
	/* [한국어] 크기 코드를 제자리(비트 19:16)로 민다. */
	outval <<= FARADAY_PCI_MEMSIZE_SHIFT;

	/* This is probably not good */
	/* [한국어] 위 상류 주석이 걱정하는 지점 — 베이스가 1MB 정렬이 아니면
	 * 아래 마스크가 하위 비트를 버리게 된다. */
	if (mem_base & ~(FARADAY_PCI_MEMBASE_MASK))
		/* [한국어] 경고만 남기고 계속한다. 장치 문맥이 없어 pr_warn 이다. */
		pr_warn("truncated PCI memory base\n");
	/* Translate to bridge side address space */
	/* [한국어] 옆의 상류 주석대로 베이스를 브리지 쪽 주소 공간 표현으로 옮긴다.
	 * 상위 12비트만 남기므로 사실상 1MB 단위로 잘린다. */
	outval |= (mem_base & FARADAY_PCI_MEMBASE_MASK);
	/* [한국어] 무엇을 무엇으로 옮겼는지 남긴다. 창 설정이 어긋났을 때 쫓을 단서다. */
	pr_debug("Translated pci base @%pap, size %pap to config %08x\n",
		 &mem_base, &mem_size, outval);

	/* [한국어] 계산 결과를 호출자에게 준다. */
	*val = outval;
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * faraday_raw_pci_read_config - 간접 창구로 설정공간 워드를 읽고 크기에 맞게 자른다
 *
 * @p: 이 드라이버의 상태.
 * @bus_number: 대상 버스 번호.
 * @fn: 장치·기능 번호(devfn).
 * @config: 설정공간 안의 바이트 오프셋.
 * @size: 1/2/4 바이트.
 * @value: 읽은 값을 담을 곳.
 * @return: 언제나 PCIBIOS_SUCCESSFUL.
 *
 * 이 파일의 모든 설정공간 읽기가 결국 이 함수로 모인다. 바깥 장치를 읽을 때도,
 * 컨트롤러가 자기 CTRL2 를 읽을 때도 같은 함수를 쓰며, 후자는 버스 0·devfn 0 을
 * 넘겨 자기 자신을 지목한다 — 별도 창구(CRP)를 둔 pci-ixp4xx.c 와 갈리는 지점이다.
 *
 * 절차는 옛 방식 그대로 두 단계다. CF8 형식 주소를 FTPCI_CONFIG 에 쓰고,
 * FTPCI_DATA 를 읽으면 32비트 워드가 나온다.
 *
 * 주소를 만드는 방법이 pci-ixp4xx.c 와 크게 다르다. 이쪽은 PCI_CONF1_ADDRESS 가
 * 만든 값을 손대지 않고 그대로 쓴다 — enable 비트를 지우지도, type 0/type 1 을
 * 가르지도, IDSEL 주소선을 세우지도 않는다. 슬롯 선택을 컨트롤러가 알아서
 * 하는 구조로 보이나 그 근거는 이 트리에 없다.
 *
 * 자르기는 소프트웨어가 한다. 하드웨어는 언제나 워드를 돌려주므로 1/2바이트
 * 요청이면 목표 바이트를 맨 아래로 밀어 마스크한다. 이 점은 pci-ixp4xx.c 와 같다.
 *
 * [상류 코드 관찰] 마스터 어보트를 확인하지 않고 언제나 성공을 돌려준다.
 * 빈 슬롯을 읽으면 하드웨어가 전부 1 을 돌려주는 것에 기대는 구조로 보이며,
 * 매 접근마다 ISR 을 확인하는 pci-ixp4xx.c 와 대비된다. 원본
 * 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] size 를 검증하지 않는다. 1 도 2 도 아니면 두 if 를 모두
 * 건너뛰어 워드가 통째로 나가므로, size 3 같은 값도 조용히 4바이트 읽기가 된다.
 *
 * 실행 컨텍스트: 설정공간 접근 경로와 인터럽트 컨텍스트(irq_chip 콜백) 양쪽.
 * 잠들지 않는다. 두 단계 절차인데도 잠금이 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   faraday_pci_read_config() / faraday_pci_ack_irq() / mask_irq() /
 *   unmask_irq() / irq_handler() / probe() → [이 함수] → writel(), readl()
 */
static int faraday_raw_pci_read_config(struct faraday_pci *p, int bus_number,
				       unsigned int fn, int config, int size,
				       u32 *value)
{
	/* [한국어] 1단계 — CF8 형식 주소를 설정 사이클 주소 레지스터에 쓴다.
	 * 매크로가 만든 값을 그대로 쓰는 것이 이 파일의 특징이다. */
	writel(PCI_CONF1_ADDRESS(bus_number, PCI_SLOT(fn),
				 PCI_FUNC(fn), config),
			p->base + FTPCI_CONFIG);

	/* [한국어] 2단계 — 데이터 레지스터를 읽으면 32비트 워드가 나온다.
	 * 이 읽기 자체가 설정 사이클을 일으키는 방아쇠다. */
	*value = readl(p->base + FTPCI_DATA);

	/* [한국어] 1바이트 요청. */
	if (size == 1)
		/* [한국어] 목표 바이트를 맨 아래로 밀고 8비트만 남긴다. */
		*value = (*value >> (8 * (config & 3))) & 0xFF;
	/* [한국어] 2바이트 요청. */
	else if (size == 2)
		/* [한국어] 같은 방식으로 16비트만 남긴다. 4바이트면 두 갈래를 모두 건너뛰어
		 * 워드가 그대로 나간다. */
		*value = (*value >> (8 * (config & 3))) & 0xFFFF;

	/* [한국어] 위 [상류 코드 관찰] 대로 실패라는 개념이 없다. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * faraday_pci_read_config - 설정공간 읽기 (pci_ops.read)
 *
 * @bus: 대상 버스. sysdata 에 이 드라이버의 상태가 들어 있다.
 * @fn: 장치·기능 번호.
 * @config: 설정공간 오프셋.
 * @size: 1/2/4 바이트.
 * @value: 읽은 값을 담을 곳.
 * @return: 아래 raw 함수의 반환값(언제나 PCIBIOS_SUCCESSFUL).
 *
 * PCI 코어가 부르는 진입점이며, 하는 일은 디버그 로그를 남기고 raw 함수에
 * 넘기는 것뿐이다. raw 함수를 따로 둔 이유는 이 파일 안에서 컨트롤러 자신의
 * 설정공간을 만질 때 pci_bus 없이 부를 수 있어야 하기 때문이다.
 *
 * [상류 코드 관찰] dev_dbg 가 *value 를 출력하는데, 그 값이 채워지는 것은
 * 다음 줄의 raw 함수 호출에서다. 즉 읽기 전의 값 — 호출자가 넘긴 버퍼의
 * 이전 내용 — 이 찍힌다. 읽은 결과를 보여 주려던 의도로 보이나 순서가
 * 어긋나 있다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 * (짝이 되는 쓰기 함수는 value 를 값으로 받으므로 같은 문제가 없다.)
 *
 * 실행 컨텍스트: PCI 코어의 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI 코어 → bus->ops->read → [이 함수] → faraday_raw_pci_read_config()
 */
static int faraday_pci_read_config(struct pci_bus *bus, unsigned int fn,
				   int config, int size, u32 *value)
{
	/* [한국어] 브리지에 걸어 둔 sysdata 가 곧 이 드라이버의 상태다. */
	struct faraday_pci *p = bus->sysdata;

	/* [한국어] 위 [상류 코드 관찰] 이 가리키는 지점 — 아직 채워지지 않은 *value 를
	 * 출력한다. */
	dev_dbg(&bus->dev,
		"[read]  slt: %.2d, fnc: %d, cnf: 0x%.2X, val (%d bytes): 0x%.8X\n",
		PCI_SLOT(fn), PCI_FUNC(fn), config, size, *value);

	/* [한국어] 실제 읽기는 raw 함수가 한다. 반환값을 그대로 전한다. */
	return faraday_raw_pci_read_config(p, bus->number, fn, config, size, value);
}

/* [한국어]
 * faraday_raw_pci_write_config - 간접 창구로 설정공간에 쓴다
 *
 * @p: 이 드라이버의 상태.
 * @bus_number: 대상 버스 번호.
 * @fn: 장치·기능 번호(devfn).
 * @config: 설정공간 안의 바이트 오프셋.
 * @size: 1/2/4 바이트.
 * @value: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL, 또는 크기가 잘못됐으면 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 읽기 판의 거울이지만 바이트 지정 방식이 아주 다르다. 이 파일에서 가장
 * 눈여겨볼 부분이다.
 *
 * pci-ixp4xx.c 는 "어느 바이트가 유효한가" 를 바이트 인에이블 비트로 계산해
 * 명령 레지스터에 실어 보낸다. 반면 이 파일은 **접근 폭 자체를 바꾼다** —
 * 4바이트면 writel, 2바이트면 writew, 1바이트면 writeb 를 쓰고, 데이터
 * 레지스터 주소에 (오프셋 & 3) 을 더해 워드 안의 자리를 고른다. 즉 바이트
 * 선택을 소프트웨어 계산이 아니라 버스 접근 폭에 맡기는 구조다.
 *
 * 그래서 읽기 쪽처럼 값을 밀어 줄 필요도 없다. 좁은 폭으로 그 자리에 직접
 * 쓰므로 값이 자연히 제자리에 들어간다.
 *
 * [상류 코드 관찰] 읽기 판과 달리 이쪽은 크기를 검증해 default 에서
 * PCIBIOS_BAD_REGISTER_NUMBER 를 돌려준다. 두 함수의 엄격함이 갈리는 지점이다.
 *
 * [상류 코드 관찰] 크기가 잘못된 경우에도 주소 쓰기는 이미 끝난 뒤다 —
 * switch 앞에서 FTPCI_CONFIG 에 쓰기 때문이다. 데이터 쓰기만 건너뛴다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: 설정공간 접근 경로와 인터럽트 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 위 관찰 참조.
 *
 * 호출 체인:
 *   faraday_pci_write_config() / faraday_pci_ack_irq() / mask_irq() /
 *   unmask_irq() / parse_map_dma_ranges() / probe() → [이 함수]
 *     → writel(), writew(), writeb()
 */
static int faraday_raw_pci_write_config(struct faraday_pci *p, int bus_number,
					 unsigned int fn, int config, int size,
					 u32 value)
{
	/* [한국어] 기본값은 성공. 아래 switch 의 default 만 이 값을 바꾼다. */
	int ret = PCIBIOS_SUCCESSFUL;

	/* [한국어] 1단계 — CF8 형식 주소를 쓴다. 크기 검증보다 앞서 실행된다. */
	writel(PCI_CONF1_ADDRESS(bus_number, PCI_SLOT(fn),
				 PCI_FUNC(fn), config),
			p->base + FTPCI_CONFIG);

	/* [한국어] 2단계 — 접근 폭으로 바이트 자리를 고른다. */
	switch (size) {
	/* [한국어] 워드 전체. */
	case 4:
		/* [한국어] 데이터 레지스터에 32비트를 그대로 쓴다. 오프셋 보정이 필요 없다. */
		writel(value, p->base + FTPCI_DATA);
		break;
	/* [한국어] 2바이트. */
	case 2:
		/* [한국어] 워드 안의 자리만큼 주소를 옮겨 16비트 쓰기를 한다. 바이트
		 * 인에이블을 계산하지 않고 접근 폭으로 대신하는 것이 이 하드웨어의 방식이다. */
		writew(value, p->base + FTPCI_DATA + (config & 3));
		break;
	/* [한국어] 1바이트. */
	case 1:
		/* [한국어] 같은 방식으로 8비트 쓰기를 한다. */
		writeb(value, p->base + FTPCI_DATA + (config & 3));
		break;
	/* [한국어] 그 밖의 크기는 표현할 수 없다. */
	default:
		/* [한국어] 위 관찰대로 주소는 이미 쓴 뒤이며 데이터 쓰기만 건너뛴다. */
		ret = PCIBIOS_BAD_REGISTER_NUMBER;
	}

	/* [한국어] 성공이거나 잘못된 크기 표시다. */
	return ret;
}

/* [한국어]
 * faraday_pci_write_config - 설정공간 쓰기 (pci_ops.write)
 *
 * @bus: 대상 버스.
 * @fn: 장치·기능 번호.
 * @config: 설정공간 오프셋.
 * @size: 1/2/4 바이트.
 * @value: 쓸 값.
 * @return: 아래 raw 함수의 반환값.
 *
 * 읽기 쪽과 같은 짜임이다. 디버그 로그를 남기고 raw 함수에 넘긴다.
 *
 * 읽기 판과 달리 로그에 문제가 없다 — value 를 포인터가 아니라 값으로 받으므로
 * 출력하는 것이 실제로 쓸 값이기 때문이다.
 *
 * 실행 컨텍스트: PCI 코어의 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI 코어 → bus->ops->write → [이 함수] → faraday_raw_pci_write_config()
 */
static int faraday_pci_write_config(struct pci_bus *bus, unsigned int fn,
				    int config, int size, u32 value)
{
	/* [한국어] sysdata 에서 이 드라이버의 상태를 되찾는다. */
	struct faraday_pci *p = bus->sysdata;

	/* [한국어] 어떤 쓰기였는지 남긴다. 읽기 판과 달리 값이 이미 확정되어 있다. */
	dev_dbg(&bus->dev,
		"[write] slt: %.2d, fnc: %d, cnf: 0x%.2X, val (%d bytes): 0x%.8X\n",
		PCI_SLOT(fn), PCI_FUNC(fn), config, size, value);

	/* [한국어] 실제 쓰기는 raw 함수가 한다. */
	return faraday_raw_pci_write_config(p, bus->number, fn, config, size,
					    value);
}

/* [한국어] PCI 코어가 설정공간을 만질 때 쓸 함수 표. map_bus 가 없고 read/write 를
 * 직접 구현한 것은, 간접 창구 방식이라 "접근할 주소" 라는 개념 자체가 성립하지
 * 않기 때문이다 — pci-ixp4xx.c 와 같은 이유이며, ECAM 이라 map_bus 하나로 끝나는
 * pcie-xilinx.c 와 대비된다. */
static struct pci_ops faraday_pci_ops = {
	/* [한국어] 설정공간 읽기. */
	.read	= faraday_pci_read_config,
	/* [한국어] 설정공간 쓰기. */
	.write	= faraday_pci_write_config,
};

/* [한국어]
 * faraday_pci_ack_irq - INTx 한 선의 상태 비트를 지운다 (irq_chip.irq_ack)
 *
 * @d: 이 IRQ 의 irq_data. hwirq 가 0~3(INTA~INTD)이고, chip_data 에
 *     이 드라이버의 상태가 들어 있다.
 * @return: 없음.
 *
 * 이 파일에서 가장 독특한 부분이 여기서 시작된다 — irq_chip 콜백이 MMIO 가
 * 아니라 **설정공간 접근** 으로 구현된다. 인터럽트 상태·마스크가 컨트롤러
 * 자신의 설정공간 레지스터(CTRL2)에 있기 때문이며, 그래서 버스 0·devfn 0 을
 * 지목한 raw 함수 호출이 콜백 안에 들어온다.
 *
 * 지우는 방법에 요령이 있다. 상태 비트 넷은 write-1-to-clear 이므로, 읽은
 * 값을 그대로 되쓰면 걸려 있던 다른 선까지 함께 지워진다. 그래서 먼저
 * 네 상태 비트를 모두 0 으로 만든 뒤 지우려는 한 비트만 세워 쓴다.
 *
 * [상류 코드 관찰] 읽기-수정-쓰기가 두 번의 설정 사이클로 이루어지는데
 * 그것을 보호하는 잠금이 없다. 같은 CTRL2 를 mask/unmask 콜백과 연쇄
 * 핸들러도 만지므로 겹칠 여지가 있는 구조이나, 실제로 그런 동시 접근이
 * 일어나는지는 이 트리만으로 단정할 수 없다. 같은 자리를 raw 스핀락으로
 * 감싸는 pcie-xilinx-cpm.c 와 대비된다.
 *
 * 실행 컨텍스트: IRQ 코어가 부르는 콜백. handle_level_irq 흐름이 핸들러 전에
 * 부른다. 잠들지 않는다.
 *
 * 에러 경로: raw 함수의 반환값을 보지 않는다.
 *
 * 호출 체인:
 *   IRQ 코어(handle_level_irq) → irq_chip->irq_ack → [이 함수]
 *     → faraday_raw_pci_read_config(), faraday_raw_pci_write_config()
 */
static void faraday_pci_ack_irq(struct irq_data *d)
{
	/* [한국어] 도메인 생성 시 host_data 로 넘긴 p 를 chip_data 에서 되찾는다. */
	struct faraday_pci *p = irq_data_get_irq_chip_data(d);
	/* [한국어] CTRL2 의 읽기-수정-쓰기용 임시 값. */
	unsigned int reg;

	/* [한국어] 버스 0·devfn 0 — 곧 컨트롤러 자신의 설정공간에서 CTRL2 를 읽는다. */
	faraday_raw_pci_read_config(p, 0, 0, FARADAY_PCI_CTRL2, 4, &reg);
	/* [한국어] 상태 비트 넷을 모두 0 으로 만든다. 이렇게 하지 않으면 되쓸 때
	 * 걸려 있던 다른 선까지 함께 지워진다(write-1-to-clear 이기 때문). */
	reg &= ~(0xF << PCI_CTRL2_INTSTS_SHIFT);
	/* [한국어] 지우려는 선의 비트 하나만 세운다. hwirq 0~3 에 28 을 더한 자리다. */
	reg |= BIT(irqd_to_hwirq(d) + PCI_CTRL2_INTSTS_SHIFT);
	/* [한국어] 되쓰면 그 한 비트만 지워진다. */
	faraday_raw_pci_write_config(p, 0, 0, FARADAY_PCI_CTRL2, 4, reg);
}

/* [한국어]
 * faraday_pci_mask_irq - INTx 한 선을 마스크한다 (irq_chip.irq_mask)
 *
 * @d: 이 IRQ 의 irq_data.
 * @return: 없음.
 *
 * CTRL2 의 마스크 필드(비트 25:22)에서 해당 비트를 지워 그 선의 인터럽트가
 * 올라오지 않게 한다. 이 필드는 비트를 **세워야** 통과하는 극성이라,
 * 마스크가 곧 "지우기" 다.
 *
 * 상태 비트 넷을 함께 0 으로 만드는 것은 ack 판과 같은 이유다 — 되쓸 때
 * 실수로 다른 선의 상태를 지우지 않기 위해서다. 즉 이 함수는 마스크만
 * 바꾸고 상태는 건드리지 않으려 한다.
 *
 * 실행 컨텍스트: IRQ 코어가 부르는 콜백. 잠들지 않는다. 잠금이 없는 것은
 * ack 판과 같다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   IRQ 코어(disable_irq, handle_level_irq 등) → irq_chip->irq_mask → [이 함수]
 *     → faraday_raw_pci_read_config(), faraday_raw_pci_write_config()
 */
static void faraday_pci_mask_irq(struct irq_data *d)
{
	/* [한국어] chip_data 에서 이 드라이버의 상태를 되찾는다. */
	struct faraday_pci *p = irq_data_get_irq_chip_data(d);
	/* [한국어] CTRL2 의 읽기-수정-쓰기용 임시 값. */
	unsigned int reg;

	/* [한국어] 컨트롤러 자신의 CTRL2 를 읽는다. */
	faraday_raw_pci_read_config(p, 0, 0, FARADAY_PCI_CTRL2, 4, &reg);
	/* [한국어] 상태 비트 넷을 0 으로 만들고(되쓰기 부작용 방지), 동시에
	 * 이 선의 마스크 비트를 지워 인터럽트를 막는다. 두 가지를 한 번의 AND 로 처리한다. */
	reg &= ~((0xF << PCI_CTRL2_INTSTS_SHIFT)
		 | BIT(irqd_to_hwirq(d) + PCI_CTRL2_INTMASK_SHIFT));
	/* [한국어] 되쓴다. 이 순간부터 그 선의 인터럽트가 올라오지 않는다. */
	faraday_raw_pci_write_config(p, 0, 0, FARADAY_PCI_CTRL2, 4, reg);
}

/* [한국어]
 * faraday_pci_unmask_irq - INTx 한 선의 마스크를 푼다 (irq_chip.irq_unmask)
 *
 * @d: 이 IRQ 의 irq_data.
 * @return: 없음.
 *
 * mask 판의 거울이다. 마스크 필드의 비트를 지우는 대신 세운다.
 *
 * 하위 장치 드라이버가 request_irq() 로 INTx 를 잡으면 IRQ 코어가 이 콜백을
 * 불러 그 선을 연다. 반대로 probe 는 CTRL2 의 상위 절반을 통째로 써서 네 선을
 * 모두 마스크해 두므로, 실제로 열리는 것은 장치가 붙은 선뿐이다.
 *
 * 실행 컨텍스트: IRQ 코어가 부르는 콜백. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   IRQ 코어(enable_irq, request_irq 등) → irq_chip->irq_unmask → [이 함수]
 *     → faraday_raw_pci_read_config(), faraday_raw_pci_write_config()
 */
static void faraday_pci_unmask_irq(struct irq_data *d)
{
	/* [한국어] chip_data 에서 이 드라이버의 상태를 되찾는다. */
	struct faraday_pci *p = irq_data_get_irq_chip_data(d);
	/* [한국어] CTRL2 의 읽기-수정-쓰기용 임시 값. */
	unsigned int reg;

	/* [한국어] 컨트롤러 자신의 CTRL2 를 읽는다. */
	faraday_raw_pci_read_config(p, 0, 0, FARADAY_PCI_CTRL2, 4, &reg);
	/* [한국어] 상태 비트 넷만 0 으로 만든다. mask 판이 여기서 마스크 비트까지
	 * 함께 지웠던 것과 달리, 이쪽은 아래에서 세울 것이므로 지우지 않는다. */
	reg &= ~(0xF << PCI_CTRL2_INTSTS_SHIFT);
	/* [한국어] 이 선의 마스크 비트를 세워 인터럽트를 통과시킨다. */
	reg |= BIT(irqd_to_hwirq(d) + PCI_CTRL2_INTMASK_SHIFT);
	/* [한국어] 되쓴다. */
	faraday_raw_pci_write_config(p, 0, 0, FARADAY_PCI_CTRL2, 4, reg);
}

/* [한국어]
 * faraday_pci_irq_handler - 연쇄 핸들러. 걸린 INTx 선을 도메인으로 흩는다
 *
 * @desc: 이 연쇄 핸들러가 걸린 상위 IRQ 의 서술자. handler_data 에 p 가 있다.
 * @return: 없음.
 *
 * 브리지 안의 인터럽트 컨트롤러가 네 INTx 선을 상위 IRQ 하나로 모아 올리므로,
 * 그것을 받아 어느 선인지 가려 다시 흩는 것이 이 함수의 일이다.
 *
 * 상태를 CTRL2 의 비트 31:28 에서 읽는데, 그 읽기 자체가 설정공간 접근이라는
 * 점이 이 파일의 특징이다 — 인터럽트 컨텍스트에서 설정 사이클을 두 번(주소
 * 쓰기, 데이터 읽기) 내보내는 셈이다.
 *
 * 상태 비트를 여기서 지우지 않는다. ack 콜백이 따로 있고 흐름 핸들러가
 * handle_level_irq 라, 지우기는 그쪽에서 이루어진다. 이 함수는 "누가
 * 걸렸는가" 만 판단해 넘긴다.
 *
 * [상류 코드 관찰] CTRL2 읽기가 chained_irq_enter() **앞에** 있다. 보통은
 * 상위 컨트롤러의 처리를 연 뒤 상태를 읽는데, 여기서는 순서가 뒤바뀌어 있다.
 * 같은 계열의 다른 드라이버들(pcie-xilinx-cpm.c 등)은 enter 를 먼저 부른다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들지 않는다.
 *
 * 에러 경로: generic_handle_domain_irq() 의 반환값을 보지 않는다.
 *
 * 호출 체인:
 *   IRQ 코어 → [이 함수]
 *     → faraday_raw_pci_read_config(), generic_handle_domain_irq()
 *     → 하위 장치 드라이버의 핸들러
 */
static void faraday_pci_irq_handler(struct irq_desc *desc)
{
	/* [한국어] irq_set_chained_handler_and_data 로 함께 넘긴 p 를 되찾는다. */
	struct faraday_pci *p = irq_desc_get_handler_data(desc);
	/* [한국어] 상위 컨트롤러의 irq_chip. 아래 enter/exit 가 이것을 조작한다. */
	struct irq_chip *irqchip = irq_desc_get_chip(desc);
	/* [한국어] irq_stat 은 4비트로 내린 상태, reg 는 읽은 CTRL2 원값,
	 * i 는 훑는 커서이자 곧 hwirq 다. */
	unsigned int irq_stat, reg, i;

	/* [한국어] 위 [상류 코드 관찰] 이 가리키는 지점 — enter 보다 먼저 상태를 읽는다.
	 * 컨트롤러 자신의 CTRL2 를 설정 사이클로 읽어 온다. */
	faraday_raw_pci_read_config(p, 0, 0, FARADAY_PCI_CTRL2, 4, &reg);
	/* [한국어] 비트 31:28 을 0~3 자리로 내린다. 상위 비트를 따로 마스크하지 않지만
	 * 아래에서 BIT(i) 로 네 자리만 보므로 결과는 같다. */
	irq_stat = reg >> PCI_CTRL2_INTSTS_SHIFT;

	/* [한국어] 상위 컨트롤러의 처리를 연다. 연쇄 핸들러의 규약이다. */
	chained_irq_enter(irqchip, desc);

	/* [한국어] INTA~INTD 넷을 차례로 본다. */
	for (i = 0; i < 4; i++) {
		/* [한국어] 이 선은 걸리지 않았다. */
		if ((irq_stat & BIT(i)) == 0)
			/* [한국어] 다음 선으로 넘어간다. */
			continue;
		/* [한국어] 도메인의 그 hwirq 를 부른다. 여기서 하위 장치 드라이버의 핸들러로
		 * 넘어가며, 상태 비트 지우기는 ack 콜백이 맡는다. */
		generic_handle_domain_irq(p->irqdomain, i);
	}

	/* [한국어] 상위 컨트롤러의 처리를 닫는다. */
	chained_irq_exit(irqchip, desc);
}

/* [한국어] INTx 도메인이 쓸 irq_chip. 콜백 셋이 모두 설정공간 접근으로
 * 구현된다는 것이 이 파일에서 가장 독특한 점이다.
 * ack 가 있다는 점이 pcie-xilinx-cpm.c 의 INTx irq_chip(mask/unmask 뿐)과
 * 다른데, 이 하드웨어의 상태 비트가 소프트웨어로 지워야 하는 종류이기 때문이다. */
static struct irq_chip faraday_pci_irq_chip = {
	/* [한국어] /proc/interrupts 등에 표시될 이름. */
	.name = "PCI",
	/* [한국어] 상태 비트를 지운다. */
	.irq_ack = faraday_pci_ack_irq,
	/* [한국어] 그 선을 닫는다. */
	.irq_mask = faraday_pci_mask_irq,
	/* [한국어] 그 선을 연다. */
	.irq_unmask = faraday_pci_unmask_irq,
};

/* [한국어]
 * faraday_pci_irq_map - 도메인의 새 매핑에 irq_chip 을 매단다 (irq_domain_ops.map)
 *
 * @domain: INTx IRQ 도메인.
 * @irq: 가상 IRQ 번호.
 * @hwirq: 하드웨어 인터럽트 번호(0~3, INTA~INTD).
 * @return: 언제나 0.
 *
 * 도메인에 새 hwirq 매핑이 생길 때 IRQ 코어가 한 번 불러, 그 가상 IRQ 에
 * irq_chip 과 흐름 핸들러를 매단다.
 *
 * handle_level_irq 를 고른 것이 맞는 선택이다. INTx 는 레벨 트리거 신호이고,
 * 이 흐름은 핸들러 전에 마스크·ack 를 하고 끝난 뒤 언마스크하므로 위
 * irq_chip 의 콜백 셋이 모두 제 역할을 하게 된다. 마스크 기능이 없어
 * handle_simple_irq 를 쓸 수밖에 없었던 pcie-xilinx.c 와 대비된다.
 *
 * 실행 컨텍스트: 매핑이 만들어질 때(setup_cascaded_irq 안). 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   irq_create_mapping() → domain->ops->map → [이 함수]
 *     → irq_set_chip_and_handler(), irq_set_chip_data()
 */
static int faraday_pci_irq_map(struct irq_domain *domain, unsigned int irq,
			       irq_hw_number_t hwirq)
{
	/* [한국어] 이 가상 IRQ 에 위 irq_chip 과 레벨 트리거 흐름 핸들러를 매단다. */
	irq_set_chip_and_handler(irq, &faraday_pci_irq_chip, handle_level_irq);
	/* [한국어] 도메인의 host_data(= p)를 chip_data 로 옮겨, 콜백 셋이
	 * irq_data_get_irq_chip_data 로 되찾게 한다. */
	irq_set_chip_data(irq, domain->host_data);

	/* [한국어] 언제나 성공이다. */
	return 0;
}

/* [한국어] INTx 도메인의 동작. map 하나뿐이고 xlate 가 없다 —
 * pcie-xilinx.c 가 pci_irqd_intx_xlate 를 채우는 것과 다르며,
 * pcie-xilinx-cpm.c 와는 같다. */
static const struct irq_domain_ops faraday_pci_irqdomain_ops = {
	/* [한국어] 새 매핑에 irq_chip 과 흐름 핸들러를 매단다. */
	.map = faraday_pci_irq_map,
};

/* [한국어]
 * faraday_pci_setup_cascaded_irq - INTx 도메인을 만들고 연쇄 핸들러를 꽂는다
 *
 * @p: 이 드라이버의 상태.
 * @return: 0 성공, 실패 시 음수.
 *
 * cascaded_irq 판본에서만 불린다. 브리지 안에 인터럽트 컨트롤러가 들어 있는
 * 구성이므로, DT 의 자식 노드가 그 컨트롤러를 나타내고 그 노드의 인터럽트가
 * 네 INTx 를 모아 올리는 상위 IRQ 다.
 *
 * 절차는 넷이다.
 *   1. DT 첫 자식(인터럽트 컨트롤러 노드)을 찾는다.
 *   2. 그 노드에 적힌 상위 IRQ 를 얻는다 — 옆의 상류 주석대로 모든 PCI IRQ 가
 *      이 하나로 모인다.
 *   3. 그 노드를 fwnode 로 삼아 크기 4 의 선형 도메인을 만든다.
 *   4. 상위 IRQ 에 연쇄 핸들러를 꽂고, 네 hwirq 의 매핑을 미리 만들어 둔다.
 *
 * 4번의 마지막이 특징이다. 보통은 하위 장치가 인터럽트를 요구할 때 매핑이
 * 만들어지는데, 여기서는 넷을 미리 만들어 둔다. 그래야 각 가상 IRQ 에
 * irq_chip 이 매달려 mask/unmask 가 동작할 수 있기 때문으로 보이나, 그 의도가
 * 코드에 적혀 있지는 않다.
 *
 * [상류 코드 관찰] irq_create_mapping() 의 반환값을 확인하지 않는다.
 * 실패하면 0 이 돌아오지만 그대로 지나간다. 원본 스냅숏(1f0e418bb6)에서
 * 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 자식 노드가 없거나 IRQ 를 못 얻거나 도메인 생성이 실패하면
 * 각각 오류를 올린다. 노드 참조는 각 경로에서 빠짐없이 놓아 준다.
 *
 * 호출 체인:
 *   faraday_pci_probe() → [이 함수]
 *     → of_get_next_child(), of_irq_get(), irq_domain_create_linear(),
 *       irq_set_chained_handler_and_data(), irq_create_mapping()
 */
static int faraday_pci_setup_cascaded_irq(struct faraday_pci *p)
{
	/* [한국어] 브리지 안 인터럽트 컨트롤러를 나타내는 DT 자식 노드.
	 * 참조 카운트가 올라가므로 모든 경로에서 놓아 주어야 한다. */
	struct device_node *intc = of_get_next_child(p->dev->of_node, NULL);
	/* [한국어] 네 INTx 가 모여 올라오는 상위 IRQ 번호. */
	int irq;
	/* [한국어] 매핑을 미리 만드는 루프의 커서. */
	int i;

	/* [한국어] 자식이 없다 = DT 에 인터럽트 컨트롤러 노드가 빠졌다. */
	if (!intc) {
		/* [한국어] DT 가 잘못된 경우라 알린다. */
		dev_err(p->dev, "missing child interrupt-controller node\n");
		/* [한국어] 아직 참조를 얻지 못했으므로 of_node_put 없이 반환한다. */
		return -EINVAL;
	}

	/* All PCI IRQs cascade off this one */
	/* [한국어] 옆의 상류 주석대로 모든 PCI IRQ 가 이 하나에서 갈라진다. */
	irq = of_irq_get(intc, 0);
	/* [한국어] 0 이하면 얻지 못한 것이다 — of_irq_get 은 실패 시 음수나 0 을 준다. */
	if (irq <= 0) {
		/* [한국어] 알린다. */
		dev_err(p->dev, "failed to get parent IRQ\n");
		/* [한국어] 얻어 둔 노드 참조를 놓는다. */
		of_node_put(intc);
		/* [한국어] 음수면 그 오류를, 0 이면 -EINVAL 을 올린다. 0 은 오류 코드가
		 * 아니므로 그대로 돌려주면 성공으로 오해되기 때문이다. */
		return irq ?: -EINVAL;
	}

	/* [한국어] 자식 노드를 fwnode 로 삼아 도메인을 만든다. 크기 PCI_NUM_INTX(4)가
	 * INTA~INTD 넷이고, 마지막 인자 p 가 host_data 로 저장되어 map 콜백이
	 * chip_data 로 옮긴다. */
	p->irqdomain = irq_domain_create_linear(of_fwnode_handle(intc), PCI_NUM_INTX,
						&faraday_pci_irqdomain_ops, p);
	/* [한국어] 성공하든 실패하든 노드 참조를 먼저 놓는다 — 아래 검사보다 앞서
	 * 놓아 두어 실패 경로마다 되풀이하지 않게 한 배치다. */
	of_node_put(intc);
	/* [한국어] 도메인 생성 실패. */
	if (!p->irqdomain) {
		/* [한국어] 메시지에 "Gemini" 가 박혀 있는 것은 이 코드의 출신(상류 헤더가
		 * 밝힌 Cortina Gemini 용 패치)이 남은 흔적이다. */
		dev_err(p->dev, "failed to create Gemini PCI IRQ domain\n");
		/* [한국어] 메모리 부족이 흔한 원인이지만 -EINVAL 로 돌려준다. */
		return -EINVAL;
	}

	/* [한국어] 상위 IRQ 에 연쇄 핸들러를 꽂는다. 함께 넘긴 p 를 그 핸들러가
	 * handler_data 로 되찾는다. 이 줄부터 PCI INTx 가 이 파일로 들어온다. */
	irq_set_chained_handler_and_data(irq, faraday_pci_irq_handler, p);

	/* [한국어] 네 hwirq 의 매핑을 미리 만들어 둔다. 그래야 각 가상 IRQ 에
	 * irq_chip 이 매달려 mask/unmask 가 동작한다. */
	for (i = 0; i < 4; i++)
		/* [한국어] 매핑을 만들면 map 콜백이 불려 irq_chip 이 매달린다.
		 * 위 [상류 코드 관찰] 대로 반환값을 확인하지 않는다. */
		irq_create_mapping(p->irqdomain, i);

	/* [한국어] 인터럽트 경로가 준비됐다. */
	return 0;
}

/* [한국어]
 * faraday_pci_parse_map_dma_ranges - DT 의 dma-ranges 를 읽어 인바운드 창 셋을 설정한다
 *
 * @p: 이 드라이버의 상태.
 * @return: 0 성공, -EINVAL 이면 표현할 수 없는 크기의 창이 있다.
 *
 * PCI 쪽 장치가 DMA 로 내보내는 접근이 시스템 메모리의 어느 구간에 닿을지를
 * 정한다. 이 하드웨어에는 그런 창이 셋 있고, 각각 설정공간의 MEM1/MEM2/MEM3
 * BASE_SIZE 레지스터로 설정한다.
 *
 * 창마다 베이스와 크기를 한 워드로 눌러 담는 인코딩은 faraday_res_to_memcfg()
 * 가 맡는다. 크기가 1MB~2GB 의 2의 거듭제곱이어야 한다는 제약이 거기서 온다.
 *
 * 같은 세대인 pci-ixp4xx.c 와 견주면, 그쪽은 64MB 하나를 16MB 짜리 넷으로
 * 펴서 한 레지스터에 담고 크기를 64MB 로 못박는다. 이 파일은 크기가 제각각인
 * 창 셋을 각각의 레지스터에 담는다 — 더 유연한 대신 레지스터를 셋 쓴다.
 *
 * [상류 코드 관찰] 항목 수 검사가 루프 안의 `if (i <= 2)` 로 되어 있어,
 * 넷째 항목에 이르러서야 "extraneous" 로그를 남기고 빠져나온다. 그런데 그
 * 로그 바로 위의 dev_info 는 이미 그 넷째 항목까지 출력한 뒤다 — 즉 무시할
 * 항목도 일단 안내 로그에는 나타난다. 원본 스냅숏(1f0e418bb6)에서 확인했으며
 * 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] faraday_raw_pci_write_config() 의 반환값을 확인하지 않는다.
 * 크기 4 로만 부르므로 그 함수가 실패를 돌려줄 일이 없기는 하다.
 *
 * 실행 컨텍스트: probe. 잠들지 않는다.
 *
 * 에러 경로: 크기를 표현할 수 없으면 -EINVAL 로 프로브를 끊는다.
 *
 * 호출 체인:
 *   faraday_pci_probe() → [이 함수]
 *     → faraday_res_to_memcfg(), faraday_raw_pci_write_config()
 */
static int faraday_pci_parse_map_dma_ranges(struct faraday_pci *p)
{
	/* [한국어] 로그에 쓸 장치. */
	struct device *dev = p->dev;
	/* [한국어] 이 드라이버의 상태가 브리지 뒤에 붙어 있으므로 거꾸로 브리지를
	 * 찾아 올라간다. DT 가 파싱해 둔 dma_ranges 목록이 브리지에 달려 있기 때문이다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(p);
	/* [한국어] 목록을 훑는 커서. */
	struct resource_entry *entry;
	/* [한국어] 창 셋에 대응하는 설정공간 오프셋 표. 아래에서 첨자로 꺼내 쓴다. */
	u32 confreg[3] = {
		FARADAY_PCI_MEM1_BASE_SIZE,
		FARADAY_PCI_MEM2_BASE_SIZE,
		FARADAY_PCI_MEM3_BASE_SIZE,
	};
	/* [한국어] 몇 번째 창인지. 배열 첨자이자 상한 검사의 기준이다. */
	int i = 0;
	/* [한국어] 인코딩된 창 설정값. */
	u32 val;

	/* [한국어] DT 의 dma-ranges 항목을 차례로 훑는다. */
	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		/* [한국어] CPU 주소에서 offset 을 빼면 PCI 버스 주소가 된다. offset 이
		 * 두 좌표계의 차이를 담고 있기 때문이다. */
		u64 pci_addr = entry->res->start - entry->offset;
		/* [한국어] 같은 방식으로 끝 주소도 옮긴다. 아래 로그에만 쓰인다. */
		u64 end = entry->res->end - entry->offset;
		/* [한국어] 인코딩 결과. */
		int ret;

		/* [한국어] 베이스와 크기를 한 워드로 눌러 담는다. */
		ret = faraday_res_to_memcfg(pci_addr,
					    resource_size(entry->res), &val);
		/* [한국어] 2의 거듭제곱이 아니거나 범위를 벗어난 크기다. */
		if (ret) {
			/* [한국어] 몇 번째 항목이 문제인지 알린다. */
			dev_err(dev,
				"DMA range %d: illegal MEM resource size\n", i);
			/* [한국어] 창을 설정할 수 없으므로 프로브를 끊는다. */
			return -EINVAL;
		}

		/* [한국어] 어떤 창을 어떻게 설정하는지 남긴다. 위 [상류 코드 관찰] 대로
		 * 무시될 넷째 항목도 여기까지는 출력된다. */
		dev_info(dev, "DMA MEM%d BASE: 0x%016llx -> 0x%016llx config %08x\n",
			 i + 1, pci_addr, end, val);
		/* [한국어] 창이 셋뿐이므로 첨자가 0~2 일 때만 실제로 설정한다. */
		if (i <= 2) {
			/* [한국어] 컨트롤러 자신의 설정공간에 창 설정값을 쓴다. 버스 0·devfn 0 이
			 * 곧 자기 자신이다. */
			faraday_raw_pci_write_config(p, 0, 0, confreg[i],
						     4, val);
		/* [한국어] 넷째 이후 항목. */
		} else {
			/* [한국어] 담을 레지스터가 없다고 알린다. */
			dev_err(dev, "ignore extraneous dma-range %d\n", i);
			/* [한국어] 더 볼 것이 없으므로 루프를 끝낸다. */
			break;
		}

		/* [한국어] 다음 창으로. break 로 빠져나온 경우에는 실행되지 않는다. */
		i++;
	}

	/* [한국어] 항목이 하나도 없어도 성공이다 — DMA 를 쓰지 않는 구성이 있을 수
	 * 있기 때문으로 보이나, 그 판단 근거가 코드에 적혀 있지는 않다. */
	return 0;
}

/* [한국어]
 * faraday_pci_probe - 이 컨트롤러를 붙인다 (플랫폼 드라이버 진입점)
 *
 * @pdev: 플랫폼 코어가 DT 노드와 매칭해 넘겨준 디바이스.
 * @return: 0 성공, 음수 실패.
 *
 * 이 파일에서 가장 긴 함수이며 순서가 곧 내용이다.
 *   1. 판본을 읽고 브리지와 이 드라이버의 상태를 한 덩어리로 잡는다.
 *   2. 클럭 둘(PCLK, PCICLK)을 얻어 켠다.
 *   3. 유일한 레지스터 창을 매핑한다.
 *   4. DT 의 IO 창을 읽어 FTPCI_IOSIZE 에 크기를 알린다.
 *   5. FTPCI_CTRL 에 IO·메모리·마스터 비트를 세워 컨트롤러를 버스 마스터로 만든다.
 *   6. CTRL2 의 상위 절반을 통째로 써서 INTx 상태를 지우고 전부 마스크한다.
 *   7. (판본에 따라) 인터럽트 도메인과 연쇄 핸들러를 준비한다.
 *   8. 33MHz 버스가 66MHz 를 감당하는지 보고, 그렇다면 클럭을 올린다.
 *   9. dma-ranges 로 인바운드 창 셋을 설정한다.
 *  10. 버스를 스캔하고, 속도를 기록하고, 자원을 배정하고, 장치를 등록한다.
 *
 * 6번이 7번보다 앞서는 것이 중요하다. 연쇄 핸들러를 꽂기 전에 모든 선을
 * 마스크해 두어야, 핸들러가 준비되지 않은 상태에서 인터럽트가 올라오지 않는다.
 * 같은 자리에서 순서가 뒤집혀 있는 pcie-xilinx.c 와 대비되는 지점이다.
 *
 * 10번이 pci_host_probe() 한 줄이 아니라 네 호출로 나뉘어 있다. 스캔과 자원
 * 배정 사이에 버스 속도를 기록해야 하기 때문이며, 그 때문에 이 드라이버는
 * struct faraday_pci 에 bus 필드를 따로 들고 있다.
 *
 * [상류 코드 관찰] of_device_get_match_data() 의 결과를 NULL 검사 없이
 * variant->cascaded_irq 로 역참조한다. of_match_table 의 두 항목이 모두
 * .data 를 채우고 있어 실제로는 NULL 이 될 수 없는 구조이나, 검사가 없는
 * 것은 사실이다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] 클럭 두 개를 얻는 곳의 상류 주석은 "optional clocks" 라고
 * 적혀 있지만, 코드는 실패하면 곧바로 PTR_ERR 을 반환한다. 즉 실제로는 둘 다
 * 필수다.
 *
 * [상류 코드 관찰] 8번의 `if (!IS_ERR(p->bus_clk))` 는 언제나 참이다.
 * 바로 위에서 IS_ERR 이면 이미 반환했기 때문이며, 클럭이 optional 이던 시절의
 * 잔재로 보인다.
 *
 * [상류 코드 관찰] 되감기 라벨이 하나도 없고 remove 도 없다. 실패하면 그대로
 * 반환하며, 잡은 것은 모두 devm 이라 자동으로 풀린다. 다만 7번에서 꽂은 연쇄
 * 핸들러와 만든 도메인은 devm 이 아니어서 풀리지 않는데, 아래
 * builtin_platform_driver 로 등록되어 이 드라이버가 떨어지지 않으므로 문제가
 * 되지 않는 구조다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 위 설명대로 되감기 없이 그대로 반환한다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_pci_alloc_host_bridge(), devm_clk_get_enabled(),
 *       devm_platform_ioremap_resource(), faraday_res_to_memcfg(),
 *       faraday_raw_pci_write_config(), faraday_pci_setup_cascaded_irq(),
 *       faraday_pci_parse_map_dma_ranges(), pci_scan_root_bus_bridge(),
 *       pci_bus_assign_resources(), pci_bus_add_devices()
 */
static int faraday_pci_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 자원·클럭 조회에 두루 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] compatible 에 딸린 판본 표. 위 [상류 코드 관찰] 대로 NULL 검사 없이
	 * 아래에서 역참조된다. */
	const struct faraday_pci_variant *variant =
		of_device_get_match_data(dev);
	/* [한국어] DT 의 IO 창 항목. */
	struct resource_entry *win;
	/* [한국어] 이 드라이버의 상태. 브리지 뒤의 자리를 가리키게 된다. */
	struct faraday_pci *p;
	/* [한국어] 그 항목의 자원 서술자. */
	struct resource *io;
	/* [한국어] PCI 코어가 관리하는 호스트 브리지 객체. */
	struct pci_host_bridge *host;
	/* [한국어] PCLK 핸들. 켜 두기만 하면 되므로 지역 변수에만 담는다 —
	 * 나중에 rate 를 만져야 하는 PCICLK 만 구조체에 남는다. */
	struct clk *clk;
	/* [한국어] 이 버스가 낼 수 있는 최대 속도. 기본은 33MHz 이고 아래 판정에서
	 * 66MHz 로 올라갈 수 있다. */
	unsigned char max_bus_speed = PCI_SPEED_33MHz;
	/* [한국어] 지금 실제로 도는 속도. 클럭 올리기가 실패할 수 있으므로 되읽어
	 * 확인한 값을 담는다. */
	unsigned char cur_bus_speed = PCI_SPEED_33MHz;
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] 레지스터 읽기-수정-쓰기와 창 설정값에 두루 쓰는 임시 값. */
	u32 val;

	/* [한국어] 브리지와 이 드라이버의 상태를 한 번에 잡는다. sizeof(*p) 만큼
	 * 여분을 붙여 달라는 뜻이며, 0 초기화도 여기서 보장된다. */
	host = devm_pci_alloc_host_bridge(dev, sizeof(*p));
	/* [한국어] 할당 실패. */
	if (!host)
		/* [한국어] 메모리 부족을 그대로 알린다 — 같은 자리에서 -ENODEV 를 쓰는
		 * pcie-xilinx.c 와 다른 선택이다. */
		return -ENOMEM;

	/* [한국어] 설정공간 접근 함수 표를 건다. */
	host->ops = &faraday_pci_ops;
	/* [한국어] 브리지 뒤에 붙은 여분 공간의 주소를 얻는다. */
	p = pci_host_bridge_priv(host);
	/* [한국어] 설정공간 접근 함수들이 bus->sysdata 로 되찾을 수 있게 심어 둔다. */
	host->sysdata = p;
	/* [한국어] 로그에 쓸 장치를 채운다. */
	p->dev = dev;

	/* Retrieve and enable optional clocks */
	/* [한국어] AHB 쪽 클럭. 옆의 상류 주석은 optional 이라 하지만 아래 검사가
	 * 실패를 그대로 올리므로 실제로는 필수다 — 위 [상류 코드 관찰] 참조.
	 * _enabled 판이라 얻는 동시에 켜지고, devres 가 끌 때까지 켜져 있다. */
	clk = devm_clk_get_enabled(dev, "PCLK");
	/* [한국어] 얻지 못했다. */
	if (IS_ERR(clk))
		/* [한국어] 그대로 올린다. */
		return PTR_ERR(clk);
	/* [한국어] PCI 버스 클럭. 아래에서 rate 를 읽고 올려야 하므로 구조체에 남긴다. */
	p->bus_clk = devm_clk_get_enabled(dev, "PCICLK");
	/* [한국어] 얻지 못했다. */
	if (IS_ERR(p->bus_clk))
		/* [한국어] 그대로 올린다. 이 반환 때문에 아래의 IS_ERR 재검사가 무의미해진다. */
		return PTR_ERR(p->bus_clk);

	/* [한국어] 유일한 레지스터 창을 매핑한다. 이름이 아니라 첨자 0 으로 얻는 것은
	 * 자원이 하나뿐이라 이름을 붙일 이유가 없기 때문이다. */
	p->base = devm_platform_ioremap_resource(pdev, 0);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(p->base))
		/* [한국어] 코드를 꺼내 올린다. */
		return PTR_ERR(p->base);

	/* [한국어] DT 창 목록에서 첫 IO 창을 찾는다. */
	win = resource_list_first_type(&host->windows, IORESOURCE_IO);
	/* [한국어] 있으면 크기를 하드웨어에 알린다. IO 창이 없는 구성도 정상이라
	 * else 갈래가 없다. */
	if (win) {
		/* [한국어] 자원 서술자를 꺼낸다. */
		io = win->res;
		/* [한국어] CPU 주소에서 offset 을 빼 PCI 버스 주소로 옮긴 뒤, 인바운드 메모리
		 * 창과 같은 인코딩으로 베이스·크기를 한 워드에 담는다. */
		if (!faraday_res_to_memcfg(io->start - win->offset,
					   resource_size(io), &val)) {
			/* setup I/O space size */
			/* [한국어] 옆의 상류 주석대로 IO 공간 크기를 설정한다. 이것은 설정공간이
			 * 아니라 컨트롤러 창의 MMIO 레지스터다. */
			writel(val, p->base + FTPCI_IOSIZE);
		/* [한국어] 인코딩에 실패했다 = 크기가 2의 거듭제곱이 아니거나 범위 밖이다. */
		} else {
			/* [한국어] 알린다. */
			dev_err(dev, "illegal IO mem size\n");
			/* [한국어] 창을 설정할 수 없으므로 프로브를 끊는다. */
			return -EINVAL;
		}
	}

	/* Setup hostbridge */
	/* [한국어] 옆의 상류 주석대로 호스트 브리지를 설정한다. 제어 레지스터를 읽어
	 * 다른 비트를 보존한다. */
	val = readl(p->base + FTPCI_CTRL);
	/* [한국어] IO 공간 응답을 켠다. */
	val |= PCI_COMMAND_IO;
	/* [한국어] 메모리 공간 응답을 켠다. */
	val |= PCI_COMMAND_MEMORY;
	/* [한국어] 버스 마스터를 켠다. 이것이 있어야 컨트롤러가 버스로 트랜잭션을 낸다. */
	val |= PCI_COMMAND_MASTER;
	/* [한국어] 세 비트를 한 번에 반영한다. 이름은 PCI_COMMAND_ 계열이지만
	 * 쓰는 곳은 설정공간이 아니라 컨트롤러 창의 FTPCI_CTRL 이다 — 비트 배치가
	 * 표준 PCI Command 와 같아 그 상수를 재활용한 것으로 보인다. */
	writel(val, p->base + FTPCI_CTRL);
	/* Mask and clear all interrupts */
	/* [한국어] 옆의 상류 주석대로 인터럽트를 지우고 전부 마스크한다.
	 * CTRL2 의 상위 절반(오프셋 +2)에 16비트로 0xF000 을 쓰는데, 그러면
	 * 레지스터의 비트 31~28(INTx 상태)이 1 이 되어 write-1-to-clear 로 지워지고,
	 * 비트 27~16(마스크와 오류 마스크들)은 모두 0 이 되어 전부 막힌다.
	 * 연쇄 핸들러를 꽂기 전에 해 두는 것이 요점이다. */
	faraday_raw_pci_write_config(p, 0, 0, FARADAY_PCI_CTRL2 + 2, 2, 0xF000);
	/* [한국어] 브리지 안에 인터럽트 컨트롤러가 있는 판본인가. */
	if (variant->cascaded_irq) {
		/* [한국어] 도메인을 만들고 연쇄 핸들러를 꽂는다. */
		ret = faraday_pci_setup_cascaded_irq(p);
		/* [한국어] 실패. */
		if (ret) {
			/* [한국어] 알린다. */
			dev_err(dev, "failed to setup cascaded IRQ\n");
			/* [한국어] 그대로 올린다. */
			return ret;
		}
	}

	/* Check bus clock if we can gear up to 66 MHz */
	/* [한국어] 옆의 상류 주석대로 66MHz 로 올릴 수 있는지 본다.
	 * 위 [상류 코드 관찰] 대로 이 조건은 언제나 참이다 — 오류면 이미 반환했다. */
	if (!IS_ERR(p->bus_clk)) {
		/* [한국어] 클럭 주파수를 담을 곳. */
		unsigned long rate;
		/* [한국어] 바깥 val 을 가리는 지역 변수다. 여기서는 설정공간에서 읽은
		 * 상태·명령 레지스터 값을 담는다. */
		u32 val;

		/* [한국어] 컨트롤러 자신의 표준 상태·명령 레지스터를 읽는다. */
		faraday_raw_pci_read_config(p, 0, 0,
					    FARADAY_PCI_STATUS_CMD, 4, &val);
		/* [한국어] 지금 버스 클럭이 몇인지 묻는다. */
		rate = clk_get_rate(p->bus_clk);

		/* [한국어] 33MHz 로 돌고 있는데 하드웨어가 66MHz 를 감당한다고 광고한다. */
		if ((rate == 33000000) && (val & PCI_STATUS_66MHZ_CAPABLE)) {
			/* [한국어] 올릴 수 있음을 알린다. */
			dev_info(dev, "33MHz bus is 66MHz capable\n");
			/* [한국어] 최대 속도를 66MHz 로 기록한다. */
			max_bus_speed = PCI_SPEED_66MHz;
			/* [한국어] 실제로 클럭을 올려 본다. */
			ret = clk_set_rate(p->bus_clk, 66000000);
			/* [한국어] 올리지 못했다. */
			if (ret)
				/* [한국어] 알리기만 하고 계속 진행한다 — 33MHz 로도 동작하기 때문이다. */
				dev_err(dev, "failed to set bus clock\n");
		/* [한국어] 66MHz 를 감당하지 못하거나 이미 다른 속도로 돈다. */
		} else {
			/* [한국어] 33MHz 전용임을 알린다. */
			dev_info(dev, "33MHz only bus\n");
			/* [한국어] 최대 속도를 33MHz 로 둔다. */
			max_bus_speed = PCI_SPEED_33MHz;
		}

		/* Bumping the clock may fail so read back the rate */
		/* [한국어] 옆의 상류 주석대로 올리기가 실패했을 수 있으므로 되읽는다. */
		rate = clk_get_rate(p->bus_clk);
		/* [한국어] 33MHz 로 확인됐다. */
		if (rate == 33000000)
			/* [한국어] 현재 속도를 그렇게 기록한다. */
			cur_bus_speed = PCI_SPEED_33MHz;
		/* [한국어] 66MHz 로 확인됐다. */
		if (rate == 66000000)
			/* [한국어] 현재 속도를 그렇게 기록한다. 둘 다 아니면 초기값 33MHz 가 남는다. */
			cur_bus_speed = PCI_SPEED_66MHz;
	}

	/* [한국어] 인바운드 창 셋을 설정한다. */
	ret = faraday_pci_parse_map_dma_ranges(p);
	/* [한국어] 표현할 수 없는 크기의 창이 있었다. */
	if (ret)
		/* [한국어] 그대로 올린다. */
		return ret;

	/* [한국어] 버스를 만들고 장치를 훑는다. 여기서 처음으로 설정공간 접근이
	 * 바깥 장치를 향해 나간다. */
	ret = pci_scan_root_bus_bridge(host);
	/* [한국어] 스캔 실패. */
	if (ret) {
		/* [한국어] 오류 코드까지 남긴다. */
		dev_err(dev, "failed to scan host: %d\n", ret);
		/* [한국어] 그대로 올린다. */
		return ret;
	}
	/* [한국어] 만들어진 루트 버스를 보관한다. 아래 세 줄이 이것을 쓴다. */
	p->bus = host->bus;
	/* [한국어] 앞에서 판정한 최대 속도를 버스에 기록한다. lspci 등이 이 값을 보여 준다. */
	p->bus->max_bus_speed = max_bus_speed;
	/* [한국어] 현재 속도도 기록한다. 스캔과 자원 배정 사이에 이것을 넣어야 해서
	 * pci_host_probe() 한 줄로 끝내지 못하고 네 호출로 나눈 것이다. */
	p->bus->cur_bus_speed = cur_bus_speed;

	/* [한국어] 스캔에서 찾은 장치들에 메모리·IO 자원을 배정한다. */
	pci_bus_assign_resources(p->bus);
	/* [한국어] 배정이 끝난 장치들을 커널에 등록해 드라이버가 붙게 한다.
	 * 이 줄로 초기화가 끝난다. */
	pci_bus_add_devices(p->bus);

	/* [한국어] 성공. 되감기 라벨이 하나도 없는 함수다. */
	return 0;
}

/*
 * We encode bridge variants here, we have at least two so it doesn't
 * hurt to have infrastructure to encompass future variants as well.
 */
/* [한국어] 위 상류 주석대로 판본을 표로 정리해 둔다. 필드가 하나뿐이라
 * 표라기보다 불리언 둘이지만, 나중에 판본이 늘어날 자리를 마련해 둔 것이다.
 *
 * 일반 판본. 브리지 안에 인터럽트 컨트롤러가 있어 INTx 를 이 드라이버가 다룬다. */
static const struct faraday_pci_variant faraday_regular = {
	/* [한국어] 연쇄 인터럽트를 설정한다. */
	.cascaded_irq = true,
};

/* [한국어] dual 판본. 인터럽트 컨트롤러가 없어 이 드라이버는 INTx 를 다루지
 * 않는다 — DT 의 interrupt-map 을 통해 SoC 쪽 컨트롤러가 직접 받는 구성으로
 * 보이나, 그 근거를 이 트리에서 확인할 수는 없다. */
static const struct faraday_pci_variant faraday_dual = {
	/* [한국어] 연쇄 인터럽트를 설정하지 않는다. */
	.cascaded_irq = false,
};

/* [한국어] DT compatible 과 위 두 표를 잇는 매칭 목록.
 * [상류 코드 관찰] MODULE_DEVICE_TABLE 선언이 없다. 아래
 * builtin_platform_driver 로 커널에 붙박이로 들어가므로 모듈 자동 로딩용
 * 별칭이 필요 없기 때문이다. */
static const struct of_device_id faraday_pci_of_match[] = {
	/* [한국어] 첫 항목: 일반 FTPCI100. */
	{
		/* [한국어] DT 노드가 이 문자열을 쓰면 매칭된다. */
		.compatible = "faraday,ftpci100",
		/* [한국어] 연쇄 인터럽트를 쓰는 표. */
		.data = &faraday_regular,
	},
	/* [한국어] 둘째 항목: dual 판본. */
	{
		/* [한국어] 접미사 "-dual" 이 판본을 가른다. */
		.compatible = "faraday,ftpci100-dual",
		/* [한국어] 연쇄 인터럽트를 쓰지 않는 표. */
		.data = &faraday_dual,
	},
	/* [한국어] 목록의 끝을 알리는 빈 항목. */
	{},
};

/* [한국어] 플랫폼 드라이버 등록 정보.
 * [상류 코드 관찰] .remove 가 없다. suppress_bind_attrs 로 sysfs 언바인드도
 * 막혀 있고 아래 builtin_platform_driver 로 등록되므로, 이 드라이버는 한 번
 * 붙으면 떨어지지 않는다. 그래서 probe 에 되감기 라벨이 하나도 없어도
 * 문제가 되지 않는다. */
static struct platform_driver faraday_pci_driver = {
	/* [한국어] 드라이버 코어에 전달할 공통 정보. */
	.driver = {
		/* [한국어] sysfs 와 로그에 나타나는 드라이버 이름. */
		.name = "ftpci100",
		/* [한국어] 위의 DT 매칭 목록. */
		.of_match_table = faraday_pci_of_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다. PCI 호스트 브리지를
		 * 임의로 떼면 그 아래 장치가 모두 사라지므로 막아 둔 것이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] 매칭된 디바이스마다 불릴 진입점. remove 짝은 두지 않았다. */
	.probe  = faraday_pci_probe,
};
/* [한국어] 모듈이 아니라 커널에 붙박이로 등록한다. 같은 세대인
 * pci-ixp4xx.c 가 __init probe 때문에 builtin_platform_driver_probe() 를
 * 써야 했던 것과 달리, 이 파일의 probe 는 __init 이 아니라서 보통의
 * builtin_platform_driver() 로 충분하다. */
builtin_platform_driver(faraday_pci_driver);
