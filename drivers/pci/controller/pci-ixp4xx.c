// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Intel IXP4xx PCI host controller
 *
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 *
 * Based on the IXP4xx arch/arm/mach-ixp4xx/common-pci.c driver
 * Copyright (C) 2002 Intel Corporation
 * Copyright (C) 2003 Greg Ungerer <gerg@linux-m68k.org>
 * Copyright (C) 2003-2004 MontaVista Software, Inc.
 * Copyright (C) 2005 Deepak Saxena <dsaxena@plexity.net>
 * Copyright (C) 2005 Alessandro Zummo <a.zummo@towertech.it>
 *
 * TODO:
 * - Test IO-space access
 * - DMA support
 */

/*
 * [한국어 설명] Intel IXP4xx 네트워크 프로세서의 PCI(비-Express) 호스트
 * 컨트롤러 드라이버 (pci-ixp4xx.c)
 *
 * === 파일의 역할 ===
 * 2000년대 초 ARM 네트워크 프로세서인 Intel IXP4xx 에 내장된 PCI 호스트
 * 브리지를 다루는 드라이버다. 같은 디렉터리의 다른 파일들과 세대가 근본적으로
 * 다르다 — 이것은 PCI Express 가 아니라 **병렬 버스 PCI** 이며, 위 상류
 * 헤더가 밝히듯 원래 arch/arm/mach-ixp4xx/common-pci.c 에 있던 코드를
 * drivers/pci 로 옮겨 온 것이다.
 *
 * 세대 차이가 코드 곳곳에 드러난다.
 *   - 링크 훈련이 없다. LTSSM 도, 링크 업을 기다리는 루프도, 링크 상태
 *     레지스터도 없다. 병렬 버스라 전기적으로 이어져 있으면 그것으로 끝이다.
 *   - 설정공간 접근이 옛 방식이다. ECAM 처럼 메모리에 펼쳐 놓은 창이 아니라,
 *     주소 레지스터(NP_AD)에 주소를 쓰고 명령 레지스터(NP_CBE)에 명령을 쓴 뒤
 *     데이터 레지스터(NP_RDATA/NP_WDATA)를 읽거나 쓰는 간접 방식이다.
 *   - MSI 가 없다. 이 파일에는 인터럽트 도메인도 irq_chip 도 없다. 장치
 *     인터럽트는 SoC 의 별도 인터럽트 컨트롤러가 처리하고, 이 파일이 다루는
 *     인터럽트는 오직 "PCI 오류" 뿐이다. 그마저도 IRQ 가 아니라 ARM 의
 *     **데이터 어보트 예외** 로 올라온다.
 *   - IDSEL 방식의 장치 선택이 남아 있다. PCIe 가 BDF 로 장치를 지목하는
 *     것과 달리, 병렬 PCI 의 type 0 설정 사이클은 주소선 하나를 골라
 *     세우는 것으로 장치를 고른다. ixp4xx_config_addr() 이 그 자리를 만든다.
 *
 * 이 파일이 다루는 창은 하나(p->base)이며, 그 안에 설정공간 접근 창구,
 * 컨트롤러 자신의 설정공간 창구(CRP), 상태·제어 레지스터, 그리고 주소 창
 * 설정 레지스터가 모두 들어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅 시 흐름은 이렇다.
 *
 *   플랫폼 드라이버 코어 → ixp4xx_pci_probe()   [__init 이다]
 *     → devm_pci_alloc_host_bridge()  : 브리지와 이 드라이버의 상태를 함께 잡는다
 *     → compatible 검사               : ixp42x 면 errata_hammer 를 켠다
 *     → devm_platform_ioremap_resource() : 유일한 레지스터 창
 *     → CSR 읽기                      : 호스트 모드인지 옵션(장치) 모드인지 판정
 *     → hook_fault_code()             : ARM 데이터 어보트 핸들러를 건다
 *     → ixp4xx_pci_parse_map_ranges()     : 아웃바운드(AHB → PCI) 창 설정
 *     → ixp4xx_pci_parse_map_dma_ranges() : 인바운드(PCI → AHB) 창 설정
 *     → (호스트 모드면) CRP 로 자기 BAR 여섯 개와 타임아웃을 설정
 *     → 인터럽트 상태 지우기, CSR 에 Initialize Complete 세우기
 *     → CRP 로 PCI_COMMAND 에 마스터/메모리 비트 세우기
 *     → pci_host_probe()              : 버스 스캔
 *
 * 설정공간 접근은 두 갈래로 완전히 나뉜다. 이 갈림이 이 파일을 읽는 축이다.
 *
 *   (바깥 장치)  PCI 코어 → ixp4xx_pci_ops.read/write
 *                 → ixp4xx_pci_read_config() / write_config()
 *                 → ixp4xx_config_addr()  [IDSEL 또는 type 1 주소를 만든다]
 *                 → ixp4xx_pci_read_indirect() / write_indirect()
 *                 → NP_AD / NP_CBE / NP_RDATA / NP_WDATA
 *
 *   (자기 자신)  이 파일 안에서만 → ixp4xx_crp_read_config() / write_config()
 *                 → CRP_AD_CBE / CRP_RDATA / CRP_WDATA
 *
 * 후자를 상류 주석은 "Controller Configuration Port" 라 부른다. 컨트롤러
 * 자신의 설정공간은 바깥 버스로 나가지 않고 이 별도 창구로만 만질 수 있다.
 *
 * 오류 보고 경로도 요즘 드라이버와 다르다.
 *
 *   PCI 마스터 어보트 → AHB 버스 오류 → ARM 데이터 어보트 예외
 *     → hook_fault_code 로 등록된 ixp4xx_pci_abort_handler()
 *     → ISR 과 자기 PCI_STATUS 를 읽어 로그를 남기고 비트를 지운 뒤,
 *       imprecise abort 였으면 복귀 주소를 명령어 하나만큼 밀어 준다.
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트이며 __init 이다. 설정공간
 * 접근 함수들은 PCI 코어가 부르는 대로 따라간다. 어보트 핸들러만 예외
 * 컨텍스트에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. pci_host_probe() 가 버스를 스캔하고, 설정공간 접근은
 *   host->ops 로 걸어 둔 ixp4xx_pci_ops 가 처리한다.
 * 아래쪽: 없다. 이 파일 아래에 다른 컨트롤러 층이 없고, 레지스터를 직접 두드린다.
 * 옆쪽: ARM 아키텍처 코드와 이어진다 — hook_fault_code() 로 데이터 어보트
 *   핸들러를 걸고, 핸들러 안에서 regs->ARM_pc 를 직접 고친다. 그 둘은
 *   arch/arm 에 있어 이 트리에서 확인 못 함. 그래서 그 부분이 모두
 *   CONFIG_ARM 으로 감싸여 있고, 그것이 이 파일에 #ifdef 가 있는 유일한 이유다.
 * 엔디안: 위 상류 주석(ixp4xx_readl 앞)이 설명하듯, 이 SoC 의 주변장치는
 *   CPU 엔디안을 따르고 PCI 쪽은 언제나 리틀엔디안이다. 그래서 컨트롤러
 *   레지스터는 __raw_readl/__raw_writel 로, PCI 장치는 readl/writel 로
 *   접근해야 한다 — 이 파일이 자체 접근자를 두는 이유가 그것이다.
 *
 * 데이터 흐름:
 *   DT compatible → errata_hammer(ixp42x 만 true)
 *   DT reg → p->base → 이 파일의 모든 레지스터 접근
 *   DT ranges(IORESOURCE_MEM/IO) → PCIMEMBASE / AHBIOBASE 레지스터
 *   DT dma-ranges → AHBMEMBASE 레지스터
 *   버스·슬롯·기능·오프셋 → ixp4xx_config_addr() → NP_AD
 *
 * 공유 상태: struct ixp4xx_pci 하나. probe 후 불변이며 잠금이 하나도 없다.
 *   여기에 파일 스코프 전역이 하나 더 있는데(ixp4xx_pci_abort_singleton),
 *   ARM 어보트 핸들러가 문맥 인자를 받지 못해 어쩔 수 없이 둔 것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * ixp4xx_pci_probe()             : 진입점. __init 이며 이 파일에서 가장 길다.
 * ixp4xx_config_addr()           : 버스/슬롯/기능/오프셋을 하드웨어 주소로 옮긴다.
 *                                  type 0 에서 IDSEL 비트를 만드는 곳이다.
 * ixp4xx_pci_read_indirect()     : 주소를 쓰고 명령을 쓴 뒤 데이터를 읽는 옛 방식.
 * ixp4xx_pci_write_indirect()    : 그 쓰기 판. NP_WDATA 쓰기가 실행 방아쇠다.
 * ixp4xx_crp_read_config()       : 컨트롤러 자신의 설정공간 읽기(별도 창구).
 * ixp4xx_crp_write_config()      : 그 쓰기 판. probe 가 자기 BAR 를 세울 때 쓴다.
 * ixp4xx_byte_lane_enable_bits() : 1/2/4바이트 접근을 바이트 인에이블로 옮긴다.
 * ixp4xx_pci_abort_handler()     : ARM 데이터 어보트로 올라온 PCI 오류를 수습한다.
 * ixp4xx_pci_addr_to_64mconf()   : 64MB 창 하나를 16MB 짜리 넷의 설정값으로 편다.
 * struct ixp4xx_pci              : 이 드라이버의 상태. 필드가 넷뿐이다.
 *
 * === 이 시대 하드웨어가 요즘 드라이버와 다른 점 ===
 * - 바이트 인에이블: PCIe 는 요청에 크기를 담지만, 병렬 PCI 는 32비트 워드를
 *   놓고 "어느 바이트를 쓸 것인가" 를 네 개의 C/BE# 신호로 따로 알린다.
 *   그래서 이 파일에는 크기(1/2/4)를 그 네 비트로 옮기는 함수가 둘이나 있고,
 *   읽을 때는 워드를 통째로 받아 소프트웨어가 잘라 쓴다.
 * - 오류 보고: AER 도 인터럽트 도메인도 없다. 마스터 어보트는 CPU 예외로
 *   올라오고, 그것을 처리하지 않으면 커널이 죽는다. 그래서 probe 가 예외
 *   핸들러를 거는 것이며, 그 때문에 이 드라이버는 모듈이 될 수 없다
 *   (아래 platform_driver 앞의 상류 주석이 그 사정을 밝힌다).
 * - 창 크기가 고정이다. 아웃바운드도 인바운드도 정확히 64MB 여야 하고,
 *   아니면 probe 가 -EINVAL 로 끝난다.
 * - 호스트/옵션 모드: 이 컨트롤러는 부팅 시 핀 설정에 따라 호스트가 될 수도
 *   장치(옵션 보드)가 될 수도 있다. CSR 의 비트 0 이 그것을 알려 주며,
 *   호스트일 때만 자기 BAR 를 설정한다.
 */

/* [한국어] __init 표시. 이 파일의 probe 가 __init 이라 필요하다. */
#include <linux/init.h>
/* [한국어] __raw_readl()/__raw_writel(). 위 상류 주석이 설명하듯 이 SoC 의
 * 컨트롤러 레지스터는 CPU 네이티브 엔디안으로 접근해야 해서 raw 판을 쓴다. */
#include <linux/io.h>
/* [한국어] U8_MAX/U16_MAX/U32_MAX 등 기본 관용구. 읽은 워드를 크기에 맞게
 * 자를 때 쓴다. */
#include <linux/kernel.h>
/* [한국어] of_device_is_compatible(). ixp42x 인지 가려 errata 회피를 켠다 —
 * 이 파일에서 세대를 가르는 유일한 지점이다. */
#include <linux/of.h>
/* [한국어] of_pci 계열 헤더. 이 파일이 직접 쓰는 이름은 없다(전수 확인). */
#include <linux/of_pci.h>
/* [한국어] PCI_SLOT()/PCI_FUNC(), PCI_CONF1_ADDRESS/PCI_CONF1_ENABLE,
 * PCI_BASE_ADDRESS_0 등 설정공간 상수, PCIBIOS_ 반환 코드. */
#include <linux/pci.h>
/* [한국어] struct platform_device 와 devm_platform_ioremap_resource(). */
#include <linux/platform_device.h>
/* [한국어] slab 헤더. 이 파일은 직접 할당하지 않는다 — 상태 구조체를
 * 브리지 뒤에 딸려 잡기 때문이다(전수 확인). */
#include <linux/slab.h>
/* [한국어] BIT() 매크로. IDSEL 비트와 바이트 인에이블 계산에 쓴다. */
#include <linux/bits.h>
/* [한국어] drivers/pci 내부 선언. pci_pio_to_address() 등이 여기서 온다. */
#include "../pci.h"

/* [한국어] 아래는 모두 이 컨트롤러의 유일한 레지스터 창(p->base) 안의
 * 오프셋이다. 크게 네 무리로 나뉜다 — 바깥 장치 설정공간 창구(NP_),
 * 자기 설정공간 창구(CRP_), 상태·제어(CSR/ISR/INTEN), 주소 창 설정. */
/* Register offsets */
/* [한국어] NP(Non-Prefetch) 주소 레지스터. 간접 접근의 1단계로, 여기에
 * 목표 주소를 쓴다. "NP access unit" 이 바깥 PCI 버스로 나가는 창구다. */
#define IXP4XX_PCI_NP_AD		0x00
/* [한국어] NP 명령/바이트 인에이블 레지스터. 하위 4비트가 PCI 명령(설정 읽기,
 * 설정 쓰기 등)이고 비트 7:4 가 바이트 인에이블이다. 읽기에서는 여기에 쓰는
 * 것이 곧 트랜잭션 실행 신호가 된다. */
#define IXP4XX_PCI_NP_CBE		0x04
/* [한국어] NP 쓰기 데이터. 쓰기에서는 여기에 쓰는 것이 실행 방아쇠다
 * (ixp4xx_pci_write_indirect 의 상류 주석이 그렇게 밝힌다). */
#define IXP4XX_PCI_NP_WDATA		0x08
/* [한국어] NP 읽기 데이터. 명령을 쓴 뒤 이 레지스터를 읽으면 32비트 워드가 나온다. */
#define IXP4XX_PCI_NP_RDATA		0x0c
/* [한국어] CRP(Controller Configuration Port) 주소/바이트 인에이블 레지스터.
 * 컨트롤러 자신의 설정공간을 바깥 버스를 거치지 않고 만지는 창구다.
 * 비트 16 이 쓰기 표시이고 비트 23:20 이 바이트 인에이블이다. */
#define IXP4XX_PCI_CRP_AD_CBE		0x10
/* [한국어] CRP 쓰기 데이터. */
#define IXP4XX_PCI_CRP_WDATA		0x14
/* [한국어] CRP 읽기 데이터. */
#define IXP4XX_PCI_CRP_RDATA		0x18
/* [한국어] 제어·상태 레지스터. 호스트 모드 여부, 엔디안 스와핑, 초기화 완료
 * 표시가 모두 여기 있다. */
#define IXP4XX_PCI_CSR			0x1c
/* [한국어] 인터럽트 상태 레지스터. 오류 사건이 비트로 나타나며 write-1-to-clear 다.
 * 다만 이 파일은 이것을 IRQ 로 받지 않고, ARM 어보트 핸들러 안에서 읽는다. */
#define IXP4XX_PCI_ISR			0x20
/* [한국어] 인터럽트 인에이블 레지스터.
 * [상류 코드 관찰] 이 오프셋도, 아래 INTEN_ 비트 여덟 개도 이 파일 어디에서도
 * 쓰이지 않는다(전수 확인). 즉 이 드라이버는 PCI 오류를 인터럽트로 받도록
 * 설정하지 않으며, 오류는 오직 ARM 데이터 어보트 경로로만 올라온다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
#define IXP4XX_PCI_INTEN		0x24
/* [한국어] DMA 제어. 파일 맨 위 상류 헤더의 "TODO: DMA support" 가 가리키는
 * 기능이며, 이 파일 어디에서도 쓰이지 않는다. */
#define IXP4XX_PCI_DMACTRL		0x28
/* [한국어] AHB 메모리 베이스. PCI 쪽에서 들어오는 접근(인바운드)이 AHB 의
 * 어느 주소로 갈지 정한다. parse_map_dma_ranges 가 쓴다. */
#define IXP4XX_PCI_AHBMEMBASE		0x2c
/* [한국어] AHB IO 베이스. IO 공간 접근이 AHB 로 나갈 때의 기준 주소이며,
 * parse_map_ranges 가 상위 24비트를 여기 쓴다. */
#define IXP4XX_PCI_AHBIOBASE		0x30
/* [한국어] PCI 메모리 베이스. AHB 쪽에서 나가는 접근(아웃바운드)이 PCI 의
 * 어느 주소로 갈지 정한다. parse_map_ranges 가 쓴다. */
#define IXP4XX_PCI_PCIMEMBASE		0x34
/* [한국어] AHB 쪽 도어벨. 아래 PCI 쪽 도어벨과 짝이며, 옵션 모드에서
 * 호스트와 주고받는 신호로 보인다. 이 파일에서는 쓰이지 않는다. */
#define IXP4XX_PCI_AHBDOORBELL		0x38
/* [한국어] PCI 쪽 도어벨. 역시 쓰이지 않는다. */
#define IXP4XX_PCI_PCIDOORBELL		0x3c
/* [한국어] 아래 여섯은 AHB-to-PCI DMA 채널 둘의 설정 레지스터다. 채널마다
 * AHB 주소, PCI 주소, 길이를 하나씩 갖는다. 채널 0 의 AHB 주소.
 * 여섯 모두 이 파일에서 쓰이지 않는다 — 상류 헤더의 "TODO: DMA support" 다. */
#define IXP4XX_PCI_ATPDMA0_AHBADDR	0x40
/* [한국어] 채널 0 의 PCI 주소. */
#define IXP4XX_PCI_ATPDMA0_PCIADDR	0x44
/* [한국어] 채널 0 의 전송 길이. */
#define IXP4XX_PCI_ATPDMA0_LENADDR	0x48
/* [한국어] 채널 1 의 AHB 주소. */
#define IXP4XX_PCI_ATPDMA1_AHBADDR	0x4c
/* [한국어] 채널 1 의 PCI 주소. */
#define IXP4XX_PCI_ATPDMA1_PCIADDR	0x50
/* [한국어] 채널 1 의 전송 길이. */
#define IXP4XX_PCI_ATPDMA1_LENADDR	0x54

/* [한국어] 아래 아홉은 CSR 레지스터의 비트다. 이 중 다섯만 쓰인다. */
/* CSR bit definitions */
/* [한국어] 호스트 모드 표시(비트 0). 부팅 시 핀 설정으로 정해지는 읽기 값이며,
 * probe 가 이것으로 자기 BAR 를 설정할지 판단한다. 병렬 PCI 시대에는 같은
 * 칩이 호스트가 될 수도 옵션 보드가 될 수도 있었다. */
#define IXP4XX_PCI_CSR_HOST		BIT(0)
/* [한국어] 아비터 활성(비트 1). 이 파일에서는 쓰이지 않는다 — 부트로더가
 * 이미 세워 두는 것으로 보이나 근거는 이 트리에 없다. */
#define IXP4XX_PCI_CSR_ARBEN		BIT(1)
/* [한국어] AHB 데이터 스와핑(비트 2). 빅엔디안 빌드에서만 세운다. */
#define IXP4XX_PCI_CSR_ADS		BIT(2)
/* [한국어] PCI 데이터 스와핑(비트 3). 위와 짝이며 함께 세운다.
 * 리틀엔디안 PCI 와 빅엔디안 AHB 사이의 바이트 레인 교환을 켜는 것이다. */
#define IXP4XX_PCI_CSR_PDS		BIT(3)
/* [한국어] AHB 버스 오류 인에이블(비트 4). 이것을 세워야 PCI 쪽 오류가
 * AHB 버스 오류로 올라오고, 그것이 다시 ARM 데이터 어보트가 된다.
 * 이 드라이버의 오류 경로 전체가 이 한 비트에 매달려 있다. */
#define IXP4XX_PCI_CSR_ABE		BIT(4)
/* [한국어] 도어벨 관련 비트(비트 5). 쓰이지 않는다. */
#define IXP4XX_PCI_CSR_DBT		BIT(5)
/* [한국어] 주소 스텝 인에이블(비트 8). 쓰이지 않는다. */
#define IXP4XX_PCI_CSR_ASE		BIT(8)
/* [한국어] Initialize Complete(비트 15). 이 비트를 세워야 컨트롤러가 설정
 * 사이클을 만들어 낸다 — 아래 probe 의 상류 주석이 그렇게 밝힌다.
 * 즉 이것이 "이제 버스를 스캔해도 된다" 는 선언이다. */
#define IXP4XX_PCI_CSR_IC		BIT(15)
/* [한국어] PCI 리셋(비트 16). 쓰이지 않는다 — 이 드라이버는 버스 리셋을
 * 걸지 않고 부트로더가 남긴 상태를 그대로 쓴다. */
#define IXP4XX_PCI_CSR_PRST		BIT(16)

/* [한국어] 아래 여덟은 ISR(인터럽트 상태)의 비트다. 앞 넷만 쓰인다. */
/* ISR (Interrupt status) Register bit definitions */
/* [한국어] PCI 마스터 패리티 오류(비트 0). probe 가 시작할 때 지운다. */
#define IXP4XX_PCI_ISR_PSE		BIT(0)
/* [한국어] PCI 마스터 기능 오류(비트 1). 실질적으로 "마스터 어보트" 표시로,
 * 이 파일에서 가장 중요한 비트다 — 설정 사이클에 응답하는 장치가 없으면
 * 이 비트가 서고, ixp4xx_pci_check_master_abort() 가 그것으로 "장치 없음" 을
 * 판정한다. 버스 스캔이 빈 슬롯을 걸러 내는 근거가 바로 이 비트다. */
#define IXP4XX_PCI_ISR_PFE		BIT(1)
/* [한국어] PCI 패리티 오류(비트 2). probe 가 시작할 때 지운다. */
#define IXP4XX_PCI_ISR_PPE		BIT(2)
/* [한국어] AHB 버스 오류(비트 3). probe 가 시작할 때 지운다. */
#define IXP4XX_PCI_ISR_AHBE		BIT(3)
/* [한국어] AHB 도어벨 완료(비트 4). 쓰이지 않는다. */
#define IXP4XX_PCI_ISR_APDC		BIT(4)
/* [한국어] PCI 도어벨 완료(비트 5). 쓰이지 않는다. */
#define IXP4XX_PCI_ISR_PADC		BIT(5)
/* [한국어] AHB 도어벨(비트 6). 쓰이지 않는다. */
#define IXP4XX_PCI_ISR_ADB		BIT(6)
/* [한국어] PCI 도어벨(비트 7). 쓰이지 않는다. */
#define IXP4XX_PCI_ISR_PDB		BIT(7)

/* [한국어] 아래 여덟은 INTEN(인터럽트 인에이블)의 비트로, 위 ISR 과 자리가
 * 하나씩 대응한다.
 * [상류 코드 관찰] 여덟 모두 이 파일에서 쓰이지 않는다(전수 확인). ISR 쪽
 * 이름과 값이 같은 짝을 이루도록 표만 갖춰 둔 것이며, 원본
 * 스냅숏(1f0e418bb6)에서도 같다. 코드는 손대지 않았다. */
/* INTEN (Interrupt Enable) Register bit definitions */
/* [한국어] PCI 마스터 패리티 오류 인터럽트 허용(비트 0). */
#define IXP4XX_PCI_INTEN_PSE		BIT(0)
/* [한국어] PCI 마스터 기능 오류 인터럽트 허용(비트 1). */
#define IXP4XX_PCI_INTEN_PFE		BIT(1)
/* [한국어] PCI 패리티 오류 인터럽트 허용(비트 2). */
#define IXP4XX_PCI_INTEN_PPE		BIT(2)
/* [한국어] AHB 버스 오류 인터럽트 허용(비트 3). */
#define IXP4XX_PCI_INTEN_AHBE		BIT(3)
/* [한국어] AHB 도어벨 완료 인터럽트 허용(비트 4). */
#define IXP4XX_PCI_INTEN_APDC		BIT(4)
/* [한국어] PCI 도어벨 완료 인터럽트 허용(비트 5). */
#define IXP4XX_PCI_INTEN_PADC		BIT(5)
/* [한국어] AHB 도어벨 인터럽트 허용(비트 6). */
#define IXP4XX_PCI_INTEN_ADB		BIT(6)
/* [한국어] PCI 도어벨 인터럽트 허용(비트 7). */
#define IXP4XX_PCI_INTEN_PDB		BIT(7)

/* [한국어] NP 명령 레지스터에서 바이트 인에이블 필드가 시작하는 자리(비트 4).
 * [상류 코드 관찰] 이 이름은 이 파일에서 쓰이지 않는다. 정작 그 값이 필요한
 * ixp4xx_byte_lane_enable_bits() 는 이 매크로 대신 숫자 4 를 직접 적어 두었다 —
 * 아래 CRP 쪽 짝(CRP_AD_CBE_BESL)은 매크로를 쓰는 것과 대조된다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다. */
/* Shift value for byte enable on NP cmd/byte enable register */
#define IXP4XX_PCI_NP_CBE_BESL		4

/* [한국어] 아래 여섯은 NP 명령 레지스터 하위 4비트에 넣는 PCI 버스 명령이다.
 * 값은 PCI 규격이 정한 C/BE# 인코딩 그대로다. */
/* PCI commands supported by NP access unit */
/* [한국어] IO 읽기(0x2). 쓰이지 않는다 — 상류 헤더의 "TODO: Test IO-space
 * access" 가 가리키는 미완성 부분이다. */
#define NP_CMD_IOREAD			0x2
/* [한국어] IO 쓰기(0x3). 역시 쓰이지 않는다. */
#define NP_CMD_IOWRITE			0x3
/* [한국어] 설정 읽기(0xa). ixp4xx_pci_read_config() 가 쓰는 유일한 명령이다. */
#define NP_CMD_CONFIGREAD		0xa
/* [한국어] 설정 쓰기(0xb). ixp4xx_pci_write_config() 가 쓴다. */
#define NP_CMD_CONFIGWRITE		0xb
/* [한국어] 메모리 읽기(0x6). 쓰이지 않는다 — 메모리 접근은 이 창구가 아니라
 * 아웃바운드 주소 창을 통해 CPU 가 직접 하기 때문이다. */
#define NP_CMD_MEMREAD			0x6
/* [한국어] 메모리 쓰기(0x7). 역시 쓰이지 않는다. */
#define	NP_CMD_MEMWRITE			0x7

/* [한국어] 아래 둘은 CRP(컨트롤러 자기 설정공간) 창구의 비트 배치다. */
/* Constants for CRP access into local config space */
/* [한국어] CRP 주소 레지스터에서 바이트 인에이블 필드가 시작하는 자리(비트 20).
 * NP 쪽(비트 4)과 자리가 다르다 — 같은 개념이지만 레지스터 배치가 달라
 * 바이트 인에이블 계산 함수도 둘로 갈라져 있다. */
#define CRP_AD_CBE_BESL         20
/* [한국어] 쓰기 표시 비트(비트 16). 이 비트가 서 있으면 CRP_WDATA 쓰기가
 * 설정 쓰기로 나가고, 없으면 CRP_RDATA 읽기가 설정 읽기가 된다. */
#define CRP_AD_CBE_WRITE	0x00010000

/* [한국어] 이 컨트롤러에만 있는 비표준 설정공간 레지스터. */
/* Special PCI configuration space registers for this controller */
/* [한국어] Retry Timeout / Transfer Ready Timeout 레지스터(설정공간 오프셋 0x40).
 * 표준 PCI 헤더에서 0x40 부터는 벤더가 자유롭게 쓰는 영역이다.
 * probe 가 호스트 모드에서 CRP 로 0x000080ff 를 써 넣는다. */
#define IXP4XX_PCI_RTOTTO		0x40

/* [한국어] 이 드라이버의 상태 전부. 필드가 넷뿐인 것이 이 시대 컨트롤러의
 * 단순함을 보여 준다 — 클럭도 리셋도 PHY 도 인터럽트 도메인도 없다.
 * 따로 할당하지 않고 호스트 브리지 뒤에 딸려 잡힌다. */
struct ixp4xx_pci {
	/* [한국어] 이 컨트롤러의 장치. 로그와 자원 조회에 쓴다.
	 * 설정자: ixp4xx_pci_probe().
	 * 읽는 자: 거의 모든 함수의 dev_dbg/dev_err.
	 * 값 범위: 유효한 device 포인터.
	 * 동기화: probe 후 불변. */
	struct device *dev;
	/* [한국어] 유일한 레지스터 창의 가상 주소.
	 * 설정자: ixp4xx_pci_probe() 의 devm_platform_ioremap_resource(pdev, 0).
	 * 읽는 자: ixp4xx_readl()/ixp4xx_writel() 뿐 — 이 창은 그 두 헬퍼로만 접근한다.
	 * 값 범위: 유효한 iomem 포인터.
	 * 동기화: probe 후 불변. 잠금이 없다 — 설정공간 접근이 NP_AD 와 NP_CBE 를
	 *   연달아 쓰는 다단계 절차인데도 그렇다. 상위 PCI 코어가 설정공간 접근을
	 *   직렬화한다는 전제로 보이나, 그 전제가 이 파일에 적혀 있지는 않다.
	 *
	 * 창이 하나뿐이라는 점이 요즘 드라이버와 다르다. 바깥 장치 설정공간 창구,
	 * 자기 설정공간 창구, 상태·제어, 주소 창 설정이 모두 이 하나에 들어 있다. */
	void __iomem *base;
	/* [한국어] ixp42x 판본의 설정공간 접근 errata 회피를 켤지 여부.
	 * 설정자: ixp4xx_pci_probe() 가 compatible 이 "intel,ixp42x-pci" 일 때만 true.
	 * 읽는 자: ixp4xx_pci_read_indirect() 하나뿐이다. 쓰기 쪽에는 이 갈림이 없다.
	 * 값 범위: true(ixp42x) 또는 false(ixp43x).
	 * 동기화: probe 후 불변.
	 *
	 * 이 필드가 이 파일에서 두 SoC 판본을 가르는 유일한 표식이다. 켜지면 같은
	 * 읽기를 열여섯 번 되풀이하는데, 그 사정은 read_indirect 의 상류 주석에 있다. */
	bool errata_hammer;
	/* [한국어] 이 컨트롤러가 호스트로 동작하는지, 옵션(장치) 보드로 동작하는지.
	 * 설정자: ixp4xx_pci_probe() 가 CSR 의 비트 0 을 읽어 채운다. 소프트웨어가
	 *   정하는 것이 아니라 부팅 시 핀 설정으로 하드웨어가 정해 주는 값이다.
	 * 읽는 자: probe 가 로그 한 줄과, 자기 BAR 여섯 개를 설정할지 판단하는 데 쓴다.
	 * 값 범위: true(호스트) 또는 false(옵션).
	 * 동기화: probe 후 불변.
	 *
	 * [상류 코드 관찰] 옵션 모드로 판정돼도 이 드라이버는 그대로 진행해
	 * pci_host_probe() 까지 부른다 — 호스트가 아닌데 버스를 스캔하는 셈이다.
	 * 자기 BAR 설정만 건너뛸 뿐이다. 원본 스냅숏(1f0e418bb6)에서 확인했으며
	 * 코드는 손대지 않았다. */
	bool host_mode;
};

/*
 * The IXP4xx has a peculiar address bus that will change the
 * byte order on SoC peripherals depending on whether the device
 * operates in big-endian or little-endian mode. That means that
 * readl() and writel() that always use little-endian access
 * will not work for SoC peripherals such as the PCI controller
 * when used in big-endian mode. The accesses to the individual
 * PCI devices on the other hand, are always little-endian and
 * can use readl() and writel().
 *
 * For local AHB bus access we need to use __raw_[readl|writel]()
 * to make sure that we access the SoC devices in the CPU native
 * endianness.
 */
/* [한국어] 위 상류 주석이 이 파일에서 가장 중요한 배경 하나를 설명한다 —
 * 왜 표준 readl/writel 을 쓰지 않는가.
 *
 * 요지는 이렇다. 이 SoC 의 주소 버스는 칩이 빅엔디안으로 도는지 리틀엔디안으로
 * 도는지에 따라 주변장치 접근의 바이트 순서를 바꿔 버린다. readl/writel 은
 * 언제나 리틀엔디안으로 접근하므로, 빅엔디안 빌드에서는 PCI 컨트롤러 같은
 * SoC 내부 주변장치에 쓸 수 없다. 반면 PCI 버스 저편의 장치는 언제나
 * 리틀엔디안이라 readl/writel 이 맞다.
 *
 * 그래서 이 파일은 컨트롤러 레지스터 전용 접근자를 따로 두고 __raw_ 판을 쓴다.
 * 이 두 헬퍼 밖에서 p->base 를 직접 두드리는 곳은 이 파일에 없다.
 *
 * [한국어]
 * ixp4xx_readl - 컨트롤러 레지스터를 CPU 네이티브 엔디안으로 읽는다
 *
 * @p: 이 드라이버의 상태. base 가 채워져 있어야 한다.
 * @reg: 레지스터 창 안의 오프셋.
 * @return: 읽은 32비트 값.
 *
 * __raw_readl 은 엔디안 변환도 배리어도 하지 않는 가장 날것의 접근이다.
 * 여기서는 변환을 하지 않는다는 점이 목적이고, 배리어가 없다는 점은
 * 부작용이다 — 이 파일의 간접 접근 절차(주소 쓰기 → 명령 쓰기 → 데이터 읽기)는
 * 순서가 중요한데 그 순서를 보장하는 것이 배리어가 아니라 같은 장치의 같은
 * 창에 대한 접근이라는 사실뿐이다.
 *
 * 실행 컨텍스트: probe 와 설정공간 접근, 그리고 어보트 핸들러의 예외 컨텍스트.
 * 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → __raw_readl()
 */
static inline u32 ixp4xx_readl(struct ixp4xx_pci *p, u32 reg)
{
	/* [한국어] 창 시작에 오프셋을 더한 주소를 엔디안 변환 없이 읽는다. */
	return __raw_readl(p->base + reg);
}

/* [한국어]
 * ixp4xx_writel - 컨트롤러 레지스터에 CPU 네이티브 엔디안으로 쓴다
 *
 * @p: 이 드라이버의 상태.
 * @reg: 레지스터 창 안의 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * ixp4xx_readl() 의 짝이며 배경은 위 상류 주석과 같다.
 *
 * 인자 순서가 (오프셋, 값)이라는 점에 유의할 것 — 감싸고 있는
 * __raw_writel(값, 주소)와 순서가 반대다. 이 파일 안에서는 일관되지만,
 * 커널의 흔한 관용구와는 어긋난다.
 *
 * 실행 컨텍스트: probe 와 설정공간 접근, 어보트 핸들러. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 함수 → [이 함수] → __raw_writel()
 */
static inline void ixp4xx_writel(struct ixp4xx_pci *p, u32 reg, u32 val)
{
	/* [한국어] 인자 순서를 뒤집어 __raw_writel 에 넘긴다. */
	__raw_writel(val, p->base + reg);
}

/* [한국어]
 * ixp4xx_pci_check_master_abort - 방금 낸 트랜잭션이 마스터 어보트로 끝났는지 본다
 *
 * @p: 이 드라이버의 상태.
 * @return: 0 이면 정상, -EINVAL 이면 마스터 어보트가 났다.
 *
 * 이 파일에서 "그 자리에 장치가 있는가" 를 판정하는 유일한 수단이다.
 * 병렬 PCI 에서 설정 사이클을 냈는데 응답하는 장치가 없으면 마스터 어보트가
 * 일어나고, 컨트롤러가 ISR 의 PFE 비트를 세운다. 버스 스캔이 빈 슬롯을
 * 걸러 내는 근거가 바로 이 비트다.
 *
 * 그래서 이 함수가 -EINVAL 을 돌려주는 것은 대개 오류가 아니라 정상적인
 * "여기엔 장치가 없다" 이며, 호출자들은 그것을 PCIBIOS_DEVICE_NOT_FOUND 로
 * 옮겨 PCI 코어에 전한다.
 *
 * 비트를 지우는 것이 함께 이루어져야 한다는 점이 중요하다. 지우지 않으면
 * 다음 접근이 성공해도 이 함수가 계속 어보트라고 답하게 된다.
 *
 * 실행 컨텍스트: 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다 — 반환값 자체가 판정 결과다.
 *
 * 호출 체인:
 *   ixp4xx_pci_read_indirect() / ixp4xx_pci_write_indirect() → [이 함수]
 *     → ixp4xx_readl(), ixp4xx_writel()
 */
static int ixp4xx_pci_check_master_abort(struct ixp4xx_pci *p)
{
	/* [한국어] 인터럽트 상태 레지스터를 읽는다. 인터럽트로 받지 않고 이렇게
	 * 직접 폴링하는 것이 이 드라이버의 방식이다. */
	u32 isr = ixp4xx_readl(p, IXP4XX_PCI_ISR);

	/* [한국어] 마스터 기능 오류 = 응답한 장치가 없다. */
	if (isr & IXP4XX_PCI_ISR_PFE) {
		/* Make sure the master abort bit is reset */
		/* [한국어] 옆의 상류 주석대로 그 비트를 되써서 지운다(write-1-to-clear).
		 * 지우지 않으면 다음 접근까지 어보트로 오판된다. */
		ixp4xx_writel(p, IXP4XX_PCI_ISR, IXP4XX_PCI_ISR_PFE);
		/* [한국어] 빈 슬롯을 훑을 때마다 나오는 흔한 상황이라 dev_dbg 로만 남긴다. */
		dev_dbg(p->dev, "master abort detected\n");
		/* [한국어] 호출자가 PCIBIOS_DEVICE_NOT_FOUND 로 옮긴다. */
		return -EINVAL;
	}

	/* [한국어] 어보트가 없었다 = 장치가 응답했다. */
	return 0;
}

/* [한국어]
 * ixp4xx_pci_read_indirect - NP 창구로 설정공간 워드 하나를 읽는다
 *
 * @p: 이 드라이버의 상태.
 * @addr: ixp4xx_config_addr() 이 만든 하드웨어 주소.
 * @cmd: PCI 명령과 바이트 인에이블이 합쳐진 값.
 * @data: 읽은 32비트 워드를 담을 곳.
 * @return: 0 성공, -EINVAL 이면 마스터 어보트(장치 없음).
 *
 * 병렬 PCI 시대의 간접 설정공간 접근이 이 함수에 그대로 담겨 있다. ECAM 처럼
 * 주소를 메모리에 펼쳐 두는 방식이 아니라, 주소 레지스터에 목표를 쓰고
 * 명령 레지스터에 명령을 써서 트랜잭션을 일으킨 뒤 데이터 레지스터를 읽는다.
 * 즉 레지스터 세 개를 정해진 순서로 두드려야 한 번의 읽기가 완성된다.
 *
 * errata_hammer 갈래가 이 함수의 절반을 차지한다. 위 상류 주석이 밝히듯
 * ixp42x 판본에서는 이 경로가 미덥지 않아, 같은 명령을 여덟 번 되풀이하고
 * 매번 데이터를 두 번씩 읽어(합쳐서 열여섯 번) 마지막 값을 쓴다. 상류 주석이
 * 이 회피책의 전제도 함께 적어 두었다 — NP 공간 읽기에 부작용이 없어야만
 * 통한다는 것이다. 설정공간 읽기는 그 전제를 만족한다.
 *
 * [상류 코드 관찰] 되풀이 루프가 두 번 읽은 값을 모두 같은 곳에 덮어쓰므로,
 * 첫 번째 읽기의 결과는 언제나 버려진다. 버리는 것 자체가 목적인 코드이며
 * 상류 주석의 "read twice" 가 그것을 가리킨다. 원본 스냅숏(1f0e418bb6)에서
 * 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: 설정공간 읽기 경로. 잠들지 않는다. 세 레지스터를 순서대로
 * 두드리는 다단계 절차인데도 잠금이 없다 — 상위 PCI 코어가 설정공간 접근을
 * 직렬화한다는 전제로 보이나, 그 전제가 이 파일에 적혀 있지는 않다.
 *
 * 에러 경로: 마스터 어보트 판정을 그대로 돌려준다.
 *
 * 호출 체인:
 *   ixp4xx_pci_read_config() → [이 함수]
 *     → ixp4xx_writel(), ixp4xx_readl(), ixp4xx_pci_check_master_abort()
 */
static int ixp4xx_pci_read_indirect(struct ixp4xx_pci *p, u32 addr, u32 cmd, u32 *data)
{
	/* [한국어] 1단계 — 주소 레지스터에 목표를 쓴다. 아직 버스로 나가지는 않는다. */
	ixp4xx_writel(p, IXP4XX_PCI_NP_AD, addr);

	/* [한국어] ixp42x 판본이면 아래의 되풀이 회피책을 쓴다. */
	if (p->errata_hammer) {
		/* [한국어] 되풀이 횟수 커서. */
		int i;

		/*
		 * PCI workaround - only works if NP PCI space reads have
		 * no side effects. Hammer the register and read twice 8
		 * times. last one will be good.
		 */
		/* [한국어] 위 상류 주석대로 여덟 번 되풀이한다. */
		for (i = 0; i < 8; i++) {
			/* [한국어] 2단계 — 명령을 쓰면 그것이 트랜잭션 실행 신호가 된다.
			 * 되풀이 때마다 다시 쓴다. */
			ixp4xx_writel(p, IXP4XX_PCI_NP_CBE, cmd);
			/* [한국어] 3단계 — 데이터를 읽는다. 이 값은 곧바로 아래 줄에 덮여 버려진다. */
			*data = ixp4xx_readl(p, IXP4XX_PCI_NP_RDATA);
			/* [한국어] 한 번 더 읽는다. 상류 주석의 "read twice" 가 이것이며,
			 * 마지막 되풀이의 이 값만 실제로 쓰인다. */
			*data = ixp4xx_readl(p, IXP4XX_PCI_NP_RDATA);
		}
	/* [한국어] ixp43x 판본이면 정상 경로다. */
	} else {
		/* [한국어] 2단계 — 명령을 써서 트랜잭션을 일으킨다. */
		ixp4xx_writel(p, IXP4XX_PCI_NP_CBE, cmd);
		/* [한국어] 3단계 — 데이터를 한 번만 읽는다. */
		*data = ixp4xx_readl(p, IXP4XX_PCI_NP_RDATA);
	}

	/* [한국어] 응답한 장치가 있었는지 확인해 그 결과를 그대로 돌려준다. */
	return ixp4xx_pci_check_master_abort(p);
}

/* [한국어]
 * ixp4xx_pci_write_indirect - NP 창구로 설정공간 워드 하나를 쓴다
 *
 * @p: 이 드라이버의 상태.
 * @addr: ixp4xx_config_addr() 이 만든 하드웨어 주소.
 * @cmd: PCI 명령과 바이트 인에이블이 합쳐진 값.
 * @data: 쓸 32비트 워드. 호출자가 이미 바이트 자리에 맞춰 밀어 둔 값이다.
 * @return: 0 성공, -EINVAL 이면 마스터 어보트.
 *
 * 읽기 판의 거울이지만 실행 방아쇠가 다르다. 읽기에서는 명령 레지스터에 쓰는
 * 것이 트랜잭션을 일으켰지만, 쓰기에서는 명령까지 준비해 둔 뒤 데이터
 * 레지스터에 쓰는 순간 나간다 — 아래 상류 주석 둘이 그 두 단계를 각각
 * "Set up the write" 와 "Execute the write" 로 갈라 적어 두었다.
 *
 * errata_hammer 갈래가 없다는 점도 읽기와 다르다. 회피책의 전제가 "부작용이
 * 없는 읽기" 였으므로 쓰기에는 적용할 수 없다 — 같은 쓰기를 열여섯 번
 * 되풀이하면 그 자체가 부작용이다.
 *
 * 실행 컨텍스트: 설정공간 쓰기 경로. 잠들지 않으며 잠금이 없다.
 *
 * 에러 경로: 마스터 어보트 판정을 그대로 돌려준다.
 *
 * 호출 체인:
 *   ixp4xx_pci_write_config() → [이 함수]
 *     → ixp4xx_writel(), ixp4xx_pci_check_master_abort()
 */
static int ixp4xx_pci_write_indirect(struct ixp4xx_pci *p, u32 addr, u32 cmd, u32 data)
{
	/* [한국어] 1단계 — 주소 레지스터에 목표를 쓴다. */
	ixp4xx_writel(p, IXP4XX_PCI_NP_AD, addr);

	/* Set up the write */
	/* [한국어] 2단계 — 옆의 상류 주석대로 명령과 바이트 인에이블을 준비한다.
	 * 읽기와 달리 이 쓰기만으로는 트랜잭션이 나가지 않는다. */
	ixp4xx_writel(p, IXP4XX_PCI_NP_CBE, cmd);

	/* Execute the write by writing to NP_WDATA */
	/* [한국어] 3단계 — 옆의 상류 주석대로 데이터를 쓰는 순간 트랜잭션이 실행된다. */
	ixp4xx_writel(p, IXP4XX_PCI_NP_WDATA, data);

	/* [한국어] 응답한 장치가 있었는지 확인해 그 결과를 돌려준다. */
	return ixp4xx_pci_check_master_abort(p);
}

/* [한국어]
 * ixp4xx_config_addr - 버스/장치/기능/오프셋을 하드웨어가 받을 주소로 옮긴다
 *
 * @bus_num: 대상 버스 번호.
 * @devfn: 장치 번호와 기능 번호가 합쳐진 값.
 * @where: 설정공간 안의 바이트 오프셋.
 * @return: NP_AD 레지스터에 쓸 32비트 주소.
 *
 * 이 파일에서 병렬 PCI 시대의 성격이 가장 뚜렷하게 드러나는 함수다.
 * 설정 사이클이 두 종류로 나뉘고, 그 둘의 주소 만드는 법이 아예 다르다.
 *
 * type 1 (버스 0 이 아닐 때): 아래쪽 브리지가 받아 다시 풀어야 하는 사이클이다.
 *   CF8 형식 그대로 버스·장치·기능·오프셋을 담고 맨 아래 비트에 1 을 세워
 *   "이것은 type 1 이다" 라고 표시한다.
 *
 * type 0 (버스 0 일 때): 이 버스에 직접 붙은 장치를 고르는 사이클이며,
 *   여기서 장치를 지목하는 방법이 PCIe 와 근본적으로 다르다. 주소에 장치
 *   번호를 담는 것이 아니라, 상위 주소선 중 하나를 세워 그 선에 IDSEL 이
 *   이어진 장치가 스스로 응답하게 한다. 그래서 코드가 장치 번호 자리에는
 *   0 을 넣고 BIT(32 - 슬롯번호) 를 따로 얹는다.
 *
 * 그 산술을 그대로 읽으면 슬롯 1 이 비트 31, 슬롯 2 가 비트 30 … 슬롯 8 이
 * 비트 24 에 대응한다. 버스와 장치 번호를 0 으로 고정했으므로 CF8 형식의
 * 그 자리들이 비어 있고, 그래서 상위 자리를 IDSEL 로 전용할 수 있다.
 *
 * [상류 코드 관찰] 슬롯 번호가 0 이면 시프트 폭이 32 가 된다. 반환 타입이
 * u32 이므로 그 결과에는 IDSEL 비트가 하나도 남지 않아, 슬롯 0 을 향한 type 0
 * 사이클은 아무 장치도 고르지 못한다. 슬롯 번호가 9 이상이면 반대로 IDSEL
 * 비트가 CF8 의 버스 번호 자리(비트 23 이하)로 내려온다. 즉 이 하드웨어에서
 * 실제로 지목할 수 있는 슬롯은 1~8 로 좁혀지는 셈인데, 그 사실이 이 파일에
 * 적혀 있지는 않다. 원본 스냅숏(1f0e418bb6)에서 코드가 이대로임을 확인했으며
 * 손대지 않았다. (PCI_CONF1_ADDRESS 와 PCI_CONF1_ENABLE 의 정확한 비트 배치는
 * include/uapi/linux/pci_regs.h 에 있고 그 헤더는 이 트리에 없다. 같은 매크로를
 * 같은 방식으로 쓰는 예로 pci-ftpci100.c 와 pcie-mt7621.c 가 있다.)
 *
 * 두 갈래 모두 PCI_CONF1_ENABLE 을 지운다는 점도 눈여겨볼 것. 그 비트는
 * x86 의 CF8 포트 방식에서 "지금 이 주소가 유효하다" 를 뜻하는데, 이
 * 컨트롤러는 포트가 아니라 전용 레지스터를 쓰므로 필요하지 않다. type 0 에서는
 * 그 자리(비트 31)가 슬롯 1 의 IDSEL 로 다시 쓰이기까지 한다.
 *
 * 실행 컨텍스트: 설정공간 접근 경로. 순수 계산이라 부수효과가 없다.
 *
 * 에러 경로: 없다. 유효하지 않은 슬롯을 걸러 내지 않으며, 그 판정은 결과적으로
 * 마스터 어보트가 대신한다.
 *
 * 호출 체인:
 *   ixp4xx_pci_read_config() / ixp4xx_pci_write_config() → [이 함수]
 */
static u32 ixp4xx_config_addr(u8 bus_num, u16 devfn, int where)
{
	/* Root bus is always 0 in this hardware */
	/* [한국어] 옆의 상류 주석대로 이 하드웨어에서 루트 버스는 언제나 0 이다. */
	if (bus_num == 0) {
		/* type 0 */
		/* [한국어] 옆의 상류 주석이 가리키는 type 0 갈래. 버스와 장치 번호에 0 을 넣어
		 * 그 자리를 비우고, 기능 번호와 오프셋만 CF8 형식으로 담는다. 그런 뒤
		 * enable 비트를 지우고 슬롯에 대응하는 IDSEL 주소선 하나를 세운다 —
		 * 위 [상류 코드 관찰] 이 그 산술의 경계를 설명한다. */
		return (PCI_CONF1_ADDRESS(0, 0, PCI_FUNC(devfn), where) &
			~PCI_CONF1_ENABLE) | BIT(32-PCI_SLOT(devfn));
	/* [한국어] 버스 0 이 아니면 아래쪽 브리지를 거쳐야 한다. */
	} else {
		/* type 1 */
		/* [한국어] 옆의 상류 주석이 가리키는 type 1 갈래. 버스·장치·기능·오프셋을
		 * 모두 CF8 형식 그대로 담고, enable 비트를 지운 뒤 맨 아래 비트에 1 을 세워
		 * type 1 임을 표시한다. 이 사이클을 받은 브리지가 목표 버스에서 다시
		 * type 0 으로 풀어 준다. */
		return (PCI_CONF1_ADDRESS(bus_num, PCI_SLOT(devfn),
					  PCI_FUNC(devfn), where) &
			~PCI_CONF1_ENABLE) | 1;
	}
}

/*
 * CRP functions are "Controller Configuration Port" accesses
 * initiated from within this driver itself to read/write PCI
 * control information in the config space.
 */
/* [한국어] 위 상류 주석이 CRP 의 정체를 밝힌다 — 이 드라이버가 스스로
 * 일으키는, 컨트롤러 자신의 설정공간 접근이다. 바깥 버스로 나가지 않으므로
 * 마스터 어보트도 없고 IDSEL 도 필요 없다.
 *
 * [한국어]
 * ixp4xx_crp_byte_lane_enable_bits - CRP 접근의 바이트 인에이블 비트를 만든다
 *
 * @n: 워드 안에서의 바이트 오프셋(0~3). 호출자가 where % 4 로 구한다.
 * @size: 접근 크기(1, 2, 4).
 * @return: CRP 주소 레지스터에 얹을 바이트 인에이블 필드, 또는 0xffffffff
 *          (지원하지 않는 크기).
 *
 * 병렬 PCI 의 성격이 그대로 드러나는 계산이다. 이 버스에는 "몇 바이트를
 * 접근한다" 는 개념이 없고, 32비트 워드를 놓고 네 개의 C/BE# 신호로 "어느
 * 바이트가 유효한가" 를 알린다. 그 신호가 **액티브 로우** 라, 쓰고 싶은
 * 바이트의 비트를 0 으로 만들어야 한다 — 코드가 0xf 에서 해당 비트를 빼는
 * 모양(`0xf & ~BIT(n)`)인 이유가 그것이다.
 *
 * 크기별로 보면 이렇다.
 *   1바이트: n 번 바이트만 유효 → 그 비트만 0.
 *   2바이트: n 과 n+1 이 유효 → 두 비트가 0.
 *   4바이트: 네 바이트 모두 유효 → 전부 0, 그래서 그냥 0 을 돌려준다.
 *   그 밖: 0xffffffff 를 돌려주어 호출자가 오류로 가리게 한다. 이 값이
 *          "정상적인 바이트 인에이블일 수 없는 값" 이라 오류 표시로 쓰인다.
 *
 * 아래쪽의 ixp4xx_byte_lane_enable_bits() 와 본문이 같고 시프트 폭만 다르다 —
 * CRP 레지스터는 비트 20 부터, NP 레지스터는 비트 4 부터 이 필드를 둔다.
 *
 * [상류 코드 관찰] 2바이트 갈래에서 n 이 3 이면 BIT(4) 까지 건드려 계산이
 * 4비트 필드를 벗어나지만, 0xf 와의 AND 가 그것을 잘라 낸다. 결과적으로
 * 워드 경계를 넘는 2바이트 접근은 3번 바이트만 유효한 것으로 표현된다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: CRP 쓰기 경로. 순수 계산이다.
 *
 * 에러 경로: 지원하지 않는 크기에 0xffffffff.
 *
 * 호출 체인:
 *   ixp4xx_crp_write_config() → [이 함수]
 */
static u32 ixp4xx_crp_byte_lane_enable_bits(u32 n, int size)
{
	/* [한국어] 한 바이트만 쓴다. */
	if (size == 1)
		/* [한국어] 액티브 로우라 그 바이트의 비트만 0 으로 만든 뒤 CRP 필드 자리로 민다. */
		return (0xf & ~BIT(n)) << CRP_AD_CBE_BESL;
	/* [한국어] 두 바이트를 쓴다. */
	if (size == 2)
		/* [한국어] 인접한 두 비트를 0 으로 만든다. */
		return (0xf & ~(BIT(n) | BIT(n+1))) << CRP_AD_CBE_BESL;
	/* [한국어] 워드 전체를 쓴다. */
	if (size == 4)
		/* [한국어] 네 바이트 모두 유효하므로 인에이블 필드가 전부 0 이다. */
		return 0;
	/* [한국어] 그 밖의 크기는 이 버스가 표현할 수 없다. 호출자가 이 값을 보고
	 * PCIBIOS_BAD_REGISTER_NUMBER 를 돌려준다. */
	return 0xffffffff;
}

/* [한국어] 아래 함수는 이 파일에서 ixp4xx_pci_abort_handler() 하나만 쓰는데,
 * 그 핸들러 자체가 ARM 전용이라 함께 감싸 둔다. 감싸지 않으면 다른
 * 아키텍처(COMPILE_TEST)에서 "쓰이지 않는 함수" 경고가 난다.
 * 반면 짝이 되는 쓰기 함수는 probe 도 쓰므로 감싸지 않는다. */
#ifdef CONFIG_ARM
/* [한국어]
 * ixp4xx_crp_read_config - 컨트롤러 자신의 설정공간을 읽는다
 *
 * @p: 이 드라이버의 상태.
 * @where: 설정공간 안의 바이트 오프셋.
 * @size: 읽을 크기(1, 2, 4).
 * @value: 읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL, 또는 크기가 잘못됐으면 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 바깥 장치용 읽기(ixp4xx_pci_read_config)와 두 가지가 다르다.
 *   - 주소를 만들 필요가 없다. 대상이 자기 자신이라 버스·장치·기능이 없고,
 *     워드 정렬한 오프셋(where & ~3)이 곧 주소다.
 *   - 바이트 인에이블을 쓰지 않는다. 워드를 통째로 읽어 소프트웨어가 자른다.
 *     읽기에는 어차피 부작용이 없으므로 어느 바이트가 유효한지 하드웨어에
 *     알릴 이유가 없다 — 짝이 되는 쓰기 함수가 바이트 인에이블을 반드시
 *     계산해야 하는 것과 대비된다.
 *
 * 자르는 방법은 바깥 장치용 읽기와 똑같다. 워드를 오른쪽으로 8*n 만큼 밀어
 * 목표 바이트를 맨 아래로 내린 뒤 크기만큼 마스크한다.
 *
 * [상류 코드 관찰] 지역 변수 이름이 cmd 이지만 담기는 것은 명령이 아니라
 * 워드 정렬한 주소다. 짝이 되는 쓰기 함수에서는 같은 이름에 실제로 명령
 * 성격의 값(바이트 인에이블 + 주소 + 쓰기 표시)이 담겨, 두 함수에서 뜻이
 * 다르다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] 이 파일의 유일한 호출자는 크기 2 로만 부르므로, 아래
 * default 갈래와 그 오류 반환은 실제로는 도달하지 않는다.
 *
 * 실행 컨텍스트: ARM 데이터 어보트 예외 컨텍스트. 잠들지 않는다.
 *
 * 에러 경로: 크기가 1/2/4 가 아니면 로그를 남기고 오류를 돌려준다.
 *
 * 호출 체인:
 *   ixp4xx_pci_abort_handler() → [이 함수] → ixp4xx_writel(), ixp4xx_readl()
 */
static int ixp4xx_crp_read_config(struct ixp4xx_pci *p, int where, int size,
				  u32 *value)
{
	/* [한국어] n 은 워드 안의 바이트 오프셋, cmd 는 워드 정렬한 주소,
	 * val 은 읽어 온 워드다. */
	u32 n, cmd, val;

	/* [한국어] 오프셋의 하위 2비트가 워드 안에서의 바이트 자리다. */
	n = where % 4;
	/* [한국어] 하위 2비트를 떨어내 워드 경계로 맞춘다. 설정공간 접근은 언제나
	 * 워드 단위로 나간다. */
	cmd = where & ~3;

	/* [한국어] 어떤 접근이었는지 기록해 둔다. 어보트 처리 경로라 문제를 쫓을 때
	 * 필요한 정보다. */
	dev_dbg(p->dev, "%s from %d size %d cmd %08x\n",
		__func__, where, size, cmd);

	/* [한국어] CRP 주소 레지스터에 워드 주소를 쓴다. 쓰기 표시 비트가 없으므로
	 * 이것은 읽기 요청이 된다. */
	ixp4xx_writel(p, IXP4XX_PCI_CRP_AD_CBE, cmd);
	/* [한국어] CRP 읽기 데이터에서 워드를 받는다. */
	val = ixp4xx_readl(p, IXP4XX_PCI_CRP_RDATA);

	/* [한국어] 목표 바이트를 워드의 맨 아래로 내린다. */
	val >>= (8*n);
	/* [한국어] 크기에 맞게 자른다. */
	switch (size) {
	/* [한국어] 1바이트. */
	case 1:
		/* [한국어] 하위 8비트만 남긴다. */
		val &= U8_MAX;
		/* [한국어] 무엇을 읽었는지 기록한다. */
		dev_dbg(p->dev, "%s read byte %02x\n", __func__, val);
		break;
	/* [한국어] 2바이트. 이 파일의 유일한 호출자가 쓰는 크기다. */
	case 2:
		/* [한국어] 하위 16비트만 남긴다. */
		val &= U16_MAX;
		/* [한국어] 기록한다. */
		dev_dbg(p->dev, "%s read word %04x\n", __func__, val);
		break;
	/* [한국어] 4바이트. */
	case 4:
		/* [한국어] 워드 전체이므로 이 마스크는 값을 바꾸지 않는다. 나머지 두 갈래와
		 * 모양을 맞추려고 둔 줄이다. */
		val &= U32_MAX;
		/* [한국어] 기록한다. */
		dev_dbg(p->dev, "%s read long %08x\n", __func__, val);
		break;
	/* [한국어] 그 밖의 크기. 위 상류 주석이 "Should not happen" 이라고 못박았고,
	 * 실제로 이 파일의 호출자는 크기 2 만 쓴다. */
	default:
		/* Should not happen */
		/* [한국어] 그래도 조용히 넘어가지 않고 오류로 남긴다. */
		dev_err(p->dev, "%s illegal size\n", __func__);
		/* [한국어] 값을 채우지 않은 채 돌아간다 — 호출자가 반환값을 보고 걸러야 한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	/* [한국어] 잘라 낸 값을 호출자에게 준다. */
	*value = val;

	/* [한국어] PCI 코어의 관례를 따라 성공을 알린다. 이 함수는 코어가 부르지
	 * 않지만 반환값 관례는 그대로 따랐다. */
	return PCIBIOS_SUCCESSFUL;
}
/* [한국어] CONFIG_ARM 갈래 끝. */
#endif

/* [한국어]
 * ixp4xx_crp_write_config - 컨트롤러 자신의 설정공간에 쓴다
 *
 * @p: 이 드라이버의 상태.
 * @where: 설정공간 안의 바이트 오프셋.
 * @size: 쓸 크기(1, 2, 4).
 * @value: 쓸 값. 아직 바이트 자리에 맞춰 밀리지 않은 원래 값이다.
 * @return: PCIBIOS_SUCCESSFUL, 또는 크기가 잘못됐으면 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 이 파일에서 실질적으로 가장 많이 쓰이는 CRP 함수다. probe 가 자기 BAR
 * 여섯 개, 타임아웃 레지스터, PCI_COMMAND 를 모두 이것으로 세운다.
 *
 * 읽기 판과 달리 바이트 인에이블을 반드시 계산해야 한다. 쓰기는 부작용이
 * 있으므로, 워드 안에서 건드리지 말아야 할 바이트를 하드웨어에 알려야 하기
 * 때문이다. 그래서 cmd 에 세 가지가 합쳐진다 — 바이트 인에이블, 워드 정렬한
 * 주소, 그리고 쓰기 표시 비트.
 *
 * 값도 자리를 맞춰 밀어 준다. 워드의 n 번 바이트에 쓰려면 값을 8*n 만큼
 * 왼쪽으로 밀어야 하며, 그것이 읽기에서 오른쪽으로 밀던 것의 반대다.
 *
 * [상류 코드 관찰] CONFIG_ARM 으로 감싸이지 않았다. 읽기 판과 달리 probe 도
 * 이 함수를 쓰기 때문이며, 그래서 어보트 핸들러가 없는 빌드에서도 필요하다.
 *
 * [상류 코드 관찰] 이 파일의 호출자는 모두 크기 2 나 4 로만 부르므로,
 * 위 바이트 인에이블 계산이 0xffffffff 를 돌려주는 일이 없다. 즉 probe 안에서
 * 이 함수의 반환값을 확인하는 다섯 곳의 오류 처리는 실제로는 도달하지 않는다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe 와 ARM 어보트 예외 컨텍스트 양쪽. 잠들지 않는다.
 *
 * 에러 경로: 지원하지 않는 크기면 하드웨어를 건드리기 전에 돌아간다.
 *
 * 호출 체인:
 *   ixp4xx_pci_probe() / ixp4xx_pci_abort_handler() → [이 함수]
 *     → ixp4xx_crp_byte_lane_enable_bits(), ixp4xx_writel()
 */
static int ixp4xx_crp_write_config(struct ixp4xx_pci *p, int where, int size,
				   u32 value)
{
	/* [한국어] n 은 워드 안의 바이트 자리, cmd 는 인에이블+주소+쓰기표시,
	 * val 은 자리를 맞춰 민 값이다. */
	u32 n, cmd, val;

	/* [한국어] 오프셋의 하위 2비트가 워드 안에서의 바이트 자리다. */
	n = where % 4;
	/* [한국어] 그 자리와 크기로 바이트 인에이블 필드를 만든다. */
	cmd = ixp4xx_crp_byte_lane_enable_bits(n, size);
	/* [한국어] 표현할 수 없는 크기였다. */
	if (cmd == 0xffffffff)
		/* [한국어] 하드웨어를 건드리기 전에 돌아간다. 위 관찰대로 이 파일의
		 * 호출자로는 여기 닿지 않는다. */
		return PCIBIOS_BAD_REGISTER_NUMBER;
	/* [한국어] 워드 정렬한 주소를 같은 레지스터에 합친다. 바이트 인에이블은
	 * 비트 23:20 에 있어 하위 주소 비트와 겹치지 않는다. */
	cmd |= where & ~3;
	/* [한국어] 쓰기 표시 비트를 세운다. 이것이 없으면 읽기 요청이 된다. */
	cmd |= CRP_AD_CBE_WRITE;

	/* [한국어] 값을 목표 바이트 자리로 민다. 읽기에서 오른쪽으로 밀던 것의 반대다. */
	val = value << (8*n);

	/* [한국어] 어떤 쓰기였는지 기록해 둔다. */
	dev_dbg(p->dev, "%s to %d size %d cmd %08x val %08x\n",
		__func__, where, size, cmd, val);

	/* [한국어] 1단계 — 주소·인에이블·쓰기표시를 준비한다. */
	ixp4xx_writel(p, IXP4XX_PCI_CRP_AD_CBE, cmd);
	/* [한국어] 2단계 — 데이터를 쓰는 순간 실행된다. NP 창구의 쓰기와 같은 짜임이다. */
	ixp4xx_writel(p, IXP4XX_PCI_CRP_WDATA, val);

	/* [한국어] CRP 는 바깥 버스로 나가지 않으므로 마스터 어보트 확인이 필요 없다 —
	 * NP 쪽 쓰기가 반드시 확인하는 것과 대비된다. */
	return PCIBIOS_SUCCESSFUL;
}

/*
 * Then follows the functions that read and write from the common PCI
 * configuration space.
 */
/* [한국어] 위 상류 주석이 밝히듯 여기부터가 바깥 장치의 설정공간을 다루는
 * 함수들이다. 위쪽 CRP 무리와 짝을 이루며, 이름에 crp 가 없다는 것이 곧
 * "바깥 버스로 나간다" 는 표시다.
 *
 * [한국어]
 * ixp4xx_byte_lane_enable_bits - NP 접근의 바이트 인에이블 비트를 만든다
 *
 * @n: 워드 안에서의 바이트 오프셋(0~3).
 * @size: 접근 크기(1, 2, 4).
 * @return: NP 명령 레지스터에 얹을 바이트 인에이블 필드, 또는 0xffffffff.
 *
 * 위쪽 CRP 판(ixp4xx_crp_byte_lane_enable_bits)과 본문이 같고 시프트 폭만
 * 다르다. NP 명령 레지스터는 이 필드를 비트 7:4 에 두므로 4 만큼 민다.
 *
 * 액티브 로우라 유효한 바이트의 비트를 0 으로 만든다는 것도, 4바이트 접근이
 * 0 을 돌려준다는 것도, 지원하지 않는 크기에 0xffffffff 를 쓴다는 것도 같다.
 *
 * [상류 코드 관찰] 시프트 폭을 IXP4XX_PCI_NP_CBE_BESL 매크로가 아니라 숫자 4
 * 로 직접 적어 두었다. 그 매크로는 파일 위쪽에 정의만 되어 있고 이 파일
 * 어디에서도 쓰이지 않는데(전수 확인), 위 CRP 판이 CRP_AD_CBE_BESL 매크로를
 * 쓰는 것과 대조된다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지
 * 않았다.
 *
 * 실행 컨텍스트: 설정공간 접근 경로. 순수 계산이다.
 *
 * 에러 경로: 지원하지 않는 크기에 0xffffffff.
 *
 * 호출 체인:
 *   ixp4xx_pci_read_config() / ixp4xx_pci_write_config() → [이 함수]
 */
static u32 ixp4xx_byte_lane_enable_bits(u32 n, int size)
{
	/* [한국어] 한 바이트만 접근한다. */
	if (size == 1)
		/* [한국어] 그 바이트의 비트만 0 으로 만든 뒤 NP 필드 자리(비트 4)로 민다. */
		return (0xf & ~BIT(n)) << 4;
	/* [한국어] 두 바이트를 접근한다. */
	if (size == 2)
		/* [한국어] 인접한 두 비트를 0 으로 만든다. */
		return (0xf & ~(BIT(n) | BIT(n+1))) << 4;
	/* [한국어] 워드 전체를 접근한다. */
	if (size == 4)
		/* [한국어] 네 바이트 모두 유효하므로 인에이블 필드가 전부 0 이다. */
		return 0;
	/* [한국어] 그 밖의 크기는 표현할 수 없다. 호출자가 이 값을 보고
	 * PCIBIOS_BAD_REGISTER_NUMBER 를 돌려준다. */
	return 0xffffffff;
}

/* [한국어]
 * ixp4xx_pci_read_config - 바깥 장치의 설정공간을 읽는다 (pci_ops.read)
 *
 * @bus: 대상 버스. sysdata 에 이 드라이버의 상태가 들어 있다.
 * @devfn: 장치·기능 번호.
 * @where: 설정공간 오프셋.
 * @size: 1/2/4 바이트.
 * @value: 읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_BAD_REGISTER_NUMBER /
 *          PCIBIOS_DEVICE_NOT_FOUND.
 *
 * PCI 코어가 버스를 스캔할 때 부르는 진입점이다. 다섯 단계를 밟는다.
 *   1. 실패했을 때 보일 값으로 0xffffffff 를 미리 넣어 둔다 — 응답하지 않는
 *      장치를 읽으면 모든 비트가 1 로 보이는 것이 PCI 의 관례이며,
 *      코어의 열거 코드가 그것으로 빈 슬롯을 가린다.
 *   2. 크기를 바이트 인에이블로 옮긴다.
 *   3. 버스·슬롯·기능·오프셋을 하드웨어 주소로 옮긴다(IDSEL 또는 type 1).
 *   4. NP 창구로 워드를 읽는다.
 *   5. 워드를 크기에 맞게 잘라 내놓는다.
 *
 * 5단계의 자르기가 이 시대 버스의 성격을 보여 준다. 하드웨어는 언제나 32비트
 * 워드를 돌려주므로, 1바이트나 2바이트 요청이면 소프트웨어가 목표 바이트를
 * 맨 아래로 밀어 마스크해야 한다. 바이트 인에이블은 "어느 바이트가 유효한가"
 * 를 버스에 알릴 뿐 반환값을 정렬해 주지는 않는다.
 *
 * 마스터 어보트가 실패가 아니라 정상 결과일 수 있다는 점도 함께 볼 것.
 * 빈 슬롯을 훑을 때마다 4단계가 -EINVAL 을 돌려주고, 그것이
 * PCIBIOS_DEVICE_NOT_FOUND 가 되어 코어에 "여긴 없다" 고 전해진다.
 *
 * 실행 컨텍스트: PCI 코어의 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 크기가 잘못되면 하드웨어를 건드리기 전에 돌아가고, 장치가 없으면
 * DEVICE_NOT_FOUND 를, 크기 switch 의 default 에서도 같은 값을 돌려준다.
 *
 * 호출 체인:
 *   PCI 코어 → bus->ops->read → [이 함수]
 *     → ixp4xx_byte_lane_enable_bits(), ixp4xx_config_addr(),
 *       ixp4xx_pci_read_indirect()
 */
static int ixp4xx_pci_read_config(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *value)
{
	/* [한국어] 브리지에 걸어 둔 sysdata 가 곧 이 드라이버의 상태다.
	 * probe 의 host->sysdata = p 가 그것을 심어 두었다. */
	struct ixp4xx_pci *p = bus->sysdata;
	/* [한국어] n 은 워드 안의 바이트 자리, addr 은 하드웨어 주소,
	 * val 은 읽어 온 워드, cmd 는 명령과 바이트 인에이블이다. */
	u32 n, addr, val, cmd;
	/* [한국어] 버스 번호. PCI 코어가 버스 객체에 담아 준다. */
	u8 bus_num = bus->number;
	/* [한국어] 간접 읽기의 결과. */
	int ret;

	/* [한국어] 실패했을 때 보일 값을 미리 넣어 둔다. 응답 없는 장치가 전부 1 로
	 * 보이는 PCI 의 관례를 흉내 내는 것이며, 아래 어느 경로로 빠져나가도
	 * 호출자가 쓰레기 값을 보지 않게 한다. */
	*value = 0xffffffff;
	/* [한국어] 오프셋의 하위 2비트가 워드 안에서의 바이트 자리다. */
	n = where % 4;
	/* [한국어] 크기를 바이트 인에이블 필드로 옮긴다. */
	cmd = ixp4xx_byte_lane_enable_bits(n, size);
	/* [한국어] 표현할 수 없는 크기였다. */
	if (cmd == 0xffffffff)
		/* [한국어] 하드웨어를 건드리기 전에 돌아간다. */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 버스가 0 이면 IDSEL 방식의 type 0, 아니면 type 1 주소가 나온다. */
	addr = ixp4xx_config_addr(bus_num, devfn, where);
	/* [한국어] 바이트 인에이블 위에 설정 읽기 명령을 얹는다. 명령은 하위 4비트,
	 * 인에이블은 비트 7:4 라 겹치지 않는다. */
	cmd |= NP_CMD_CONFIGREAD;
	/* [한국어] 어떤 접근이었는지 기록해 둔다. 버스 스캔을 쫓을 때 필요한 정보다. */
	dev_dbg(p->dev, "read_config from %d size %d dev %d:%d:%d address: %08x cmd: %08x\n",
		where, size, bus_num, PCI_SLOT(devfn), PCI_FUNC(devfn), addr, cmd);

	/* [한국어] NP 창구로 워드를 읽는다. errata_hammer 갈래도 그 안에 있다. */
	ret = ixp4xx_pci_read_indirect(p, addr, cmd, &val);
	/* [한국어] 마스터 어보트 — 대개는 오류가 아니라 "그 자리에 장치가 없다" 다. */
	if (ret)
		/* [한국어] 코어에 없다고 알린다. value 는 위에서 넣어 둔 0xffffffff 로 남는다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 목표 바이트를 워드의 맨 아래로 내린다. */
	val >>= (8*n);
	/* [한국어] 크기에 맞게 자른다. CRP 읽기 판과 같은 짜임이다. */
	switch (size) {
	/* [한국어] 1바이트. */
	case 1:
		/* [한국어] 하위 8비트만 남긴다. */
		val &= U8_MAX;
		/* [한국어] 무엇을 읽었는지 기록한다. */
		dev_dbg(p->dev, "%s read byte %02x\n", __func__, val);
		break;
	/* [한국어] 2바이트. */
	case 2:
		/* [한국어] 하위 16비트만 남긴다. */
		val &= U16_MAX;
		/* [한국어] 기록한다. */
		dev_dbg(p->dev, "%s read word %04x\n", __func__, val);
		break;
	/* [한국어] 4바이트. */
	case 4:
		/* [한국어] 워드 전체라 이 마스크는 값을 바꾸지 않는다. 나머지와 모양을 맞춘 줄이다. */
		val &= U32_MAX;
		/* [한국어] 기록한다. */
		dev_dbg(p->dev, "%s read long %08x\n", __func__, val);
		break;
	/* [한국어] 그 밖의 크기. 위 상류 주석이 "Should not happen" 이라고 못박았고,
	 * 실제로 앞의 바이트 인에이블 계산이 이미 걸러 냈을 값이다. */
	default:
		/* Should not happen */
		/* [한국어] 그래도 오류로 남긴다. */
		dev_err(p->dev, "%s illegal size\n", __func__);
		/* [한국어] value 는 0xffffffff 인 채로 돌아간다. */
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	/* [한국어] 잘라 낸 값을 코어에 준다. */
	*value = val;

	/* [한국어] 코어에 성공을 알린다. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * ixp4xx_pci_write_config - 바깥 장치의 설정공간에 쓴다 (pci_ops.write)
 *
 * @bus: 대상 버스.
 * @devfn: 장치·기능 번호.
 * @where: 설정공간 오프셋.
 * @size: 1/2/4 바이트.
 * @value: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_BAD_REGISTER_NUMBER /
 *          PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 읽기 판의 거울이지만 뒷부분이 훨씬 짧다. 읽기는 워드를 받아 잘라야 했지만,
 * 쓰기는 값을 자리에 맞춰 밀어 넘기면 그 다음은 바이트 인에이블이 알아서
 * 한다 — 하드웨어가 유효하지 않은 바이트를 건드리지 않기 때문이다.
 * 그래서 크기별 switch 가 없다.
 *
 * 읽기와 달리 실패용 기본값을 준비하는 단계도 없다. 쓰기에는 돌려줄 값이
 * 없기 때문이다.
 *
 * 실행 컨텍스트: PCI 코어의 설정공간 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 크기가 잘못되면 하드웨어를 건드리기 전에, 장치가 없으면
 * 트랜잭션 뒤에 각각 오류를 돌려준다.
 *
 * 호출 체인:
 *   PCI 코어 → bus->ops->write → [이 함수]
 *     → ixp4xx_byte_lane_enable_bits(), ixp4xx_config_addr(),
 *       ixp4xx_pci_write_indirect()
 */
static int ixp4xx_pci_write_config(struct pci_bus *bus,  unsigned int devfn,
				   int where, int size, u32 value)
{
	/* [한국어] sysdata 에서 이 드라이버의 상태를 되찾는다. */
	struct ixp4xx_pci *p = bus->sysdata;
	/* [한국어] n 은 바이트 자리, addr 은 하드웨어 주소, val 은 자리를 맞춰 민 값,
	 * cmd 는 명령과 바이트 인에이블이다. */
	u32 n, addr, val, cmd;
	/* [한국어] 버스 번호. */
	u8 bus_num = bus->number;
	/* [한국어] 간접 쓰기의 결과. */
	int ret;

	/* [한국어] 오프셋의 하위 2비트가 워드 안에서의 바이트 자리다. */
	n = where % 4;
	/* [한국어] 크기를 바이트 인에이블로 옮긴다. */
	cmd = ixp4xx_byte_lane_enable_bits(n, size);
	/* [한국어] 표현할 수 없는 크기였다. */
	if (cmd == 0xffffffff)
		/* [한국어] 하드웨어를 건드리기 전에 돌아간다. */
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 버스 번호에 따라 type 0(IDSEL) 또는 type 1 주소를 만든다. */
	addr = ixp4xx_config_addr(bus_num, devfn, where);
	/* [한국어] 바이트 인에이블 위에 설정 쓰기 명령을 얹는다. */
	cmd |= NP_CMD_CONFIGWRITE;
	/* [한국어] 값을 목표 바이트 자리로 민다. 읽기에서 오른쪽으로 밀던 것의 반대이며,
	 * 이 한 줄이 읽기 쪽의 크기별 switch 를 대신한다. */
	val = value << (8*n);

	/* [한국어] 어떤 쓰기였는지 기록해 둔다. */
	dev_dbg(p->dev, "write_config_byte %#x to %d size %d dev %d:%d:%d addr: %08x cmd %08x\n",
		value, where, size, bus_num, PCI_SLOT(devfn), PCI_FUNC(devfn), addr, cmd);

	/* [한국어] NP 창구로 내보낸다. 데이터 레지스터 쓰기가 실행 방아쇠다. */
	ret = ixp4xx_pci_write_indirect(p, addr, cmd, val);
	/* [한국어] 마스터 어보트 — 그 자리에 장치가 없다. */
	if (ret)
		/* [한국어] 코어에 없다고 알린다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 코어에 성공을 알린다. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어] PCI 코어가 설정공간을 만질 때 쓸 함수 표. 항목이 둘뿐인 것이
 * 이 드라이버의 단순함을 보여 준다 — map_bus 방식이 아니라 read/write 를
 * 직접 구현했는데, 간접 접근이라 "주소를 돌려준다" 는 개념 자체가 성립하지
 * 않기 때문이다. add_bus 나 remove_bus 도 없다. */
static struct pci_ops ixp4xx_pci_ops = {
	/* [한국어] 설정공간 읽기. */
	.read = ixp4xx_pci_read_config,
	/* [한국어] 설정공간 쓰기. */
	.write = ixp4xx_pci_write_config,
};

/* [한국어]
 * ixp4xx_pci_addr_to_64mconf - 64MB 주소를 16MB 창 넷의 설정값으로 편다
 *
 * @addr: 창의 시작 물리 주소. 64MB 정렬이어야 뜻이 맞는다.
 * @return: 베이스 레지스터에 쓸 32비트 값.
 *
 * 이 하드웨어의 주소 창은 64MB 짜리 하나가 아니라 16MB 짜리 넷이며, 그
 * 넷의 시작 주소를 한 레지스터의 네 바이트에 하나씩 담는다. 아래
 * parse_map_dma_ranges 의 상류 주석이 그 배치를 그대로 설명한다.
 *
 * 각 바이트에 들어가는 것은 주소의 최상위 바이트, 즉 16MB 단위의 번호다.
 * 그래서 연속된 64MB 를 표현하려면 base, base+1, base+2, base+3 을 차례로
 * 넣으면 된다 — 이 함수가 하는 일이 그 산술 하나다.
 *
 * 넷을 따로 둔 덕에 원래는 흩어진 네 조각을 매핑할 수도 있지만, 이 드라이버는
 * 언제나 연속된 64MB 로만 쓴다. 그래서 호출자가 창 크기를 SZ_64M 으로
 * 강제하는 것이다.
 *
 * [상류 코드 관찰] base 가 u8 이라 addr 의 최상위 바이트만 남는다. 32비트를
 * 넘는 물리 주소는 표현할 수 없는데, 이 시대 SoC 에서는 문제가 되지 않는다.
 *
 * 실행 컨텍스트: probe. 순수 계산이다.
 *
 * 에러 경로: 없다. 정렬을 검사하지 않으므로, 64MB 정렬이 아닌 주소를 주면
 * 하위 비트가 조용히 버려진다.
 *
 * 호출 체인:
 *   ixp4xx_pci_parse_map_ranges() / ixp4xx_pci_parse_map_dma_ranges()
 *     → [이 함수]
 */
static u32 ixp4xx_pci_addr_to_64mconf(phys_addr_t addr)
{
	/* [한국어] 16MB 단위의 창 번호. u8 이라 최상위 바이트만 담긴다. */
	u8 base;

	/* [한국어] 주소의 최상위 바이트를 꺼낸다. 이것이 첫 16MB 창의 번호다. */
	base = ((addr & 0xff000000) >> 24);
	/* [한국어] 네 창의 번호를 한 워드의 네 바이트에 차례로 채운다.
	 * 연속된 64MB 이므로 번호가 1씩 늘어난다. */
	return (base << 24) | ((base + 1) << 16)
		| ((base + 2) << 8) | (base + 3);
}

/* [한국어]
 * ixp4xx_pci_parse_map_ranges - DT 의 ranges 를 읽어 아웃바운드 창을 설정한다
 *
 * @p: 이 드라이버의 상태.
 * @return: 0 성공, -EINVAL 이면 창 크기나 정렬이 맞지 않는다.
 *
 * CPU(AHB) 쪽에서 나가는 접근이 PCI 의 어느 주소로 갈지를 정한다. 메모리
 * 창과 IO 창 둘을 따로 다룬다.
 *
 * 메모리 창은 반드시 정확히 64MB 여야 한다. 위 addr_to_64mconf 가 16MB 짜리
 * 넷으로 펴는 방식이라 그보다 크거나 작으면 표현할 수 없기 때문이다.
 * 크기가 다르면 프로브가 실패한다.
 *
 * IO 창은 256바이트 정렬이어야 한다. 아래 상류 주석이 밝히듯 레지스터가
 * 주소의 상위 24비트만 담기 때문이며, 그래서 하위 8비트가 0 이어야 한다.
 *
 * res->name 을 여기서 바꿔 넣는 점도 눈여겨볼 것. /proc/iomem 에 보일 이름을
 * 정하는 것이며, 프리페치 가능 여부에 따라 다른 이름을 준다.
 *
 * [상류 코드 관찰] 두 창 모두 "없을 때" 를 오류로 처리하지 않는다. 메모리
 * 창이 없으면 dev_err 로 알리면서도 0 을 돌려주어 프로브가 계속되고, IO 창이
 * 없으면 dev_info 로만 남긴다. 즉 아웃바운드 메모리 창 없이도 이 드라이버는
 * 버스 스캔까지 진행한다. 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는
 * 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 잠들지 않는다.
 *
 * 에러 경로: 크기·정렬이 맞지 않을 때만 -EINVAL 을 돌려준다.
 *
 * 호출 체인:
 *   ixp4xx_pci_probe() → [이 함수]
 *     → resource_list_first_type(), ixp4xx_pci_addr_to_64mconf(),
 *       pci_pio_to_address(), ixp4xx_writel()
 */
static int ixp4xx_pci_parse_map_ranges(struct ixp4xx_pci *p)
{
	/* [한국어] 로그에 쓸 장치. */
	struct device *dev = p->dev;
	/* [한국어] 이 드라이버의 상태가 브리지 뒤에 붙어 있으므로 거꾸로 브리지를
	 * 찾아 올라간다. DT 가 파싱해 둔 창 목록이 브리지에 달려 있기 때문이다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(p);
	/* [한국어] 창 목록에서 꺼낸 항목. res 와 offset 을 함께 담는다. */
	struct resource_entry *win;
	/* [한국어] 그 항목의 자원 서술자. */
	struct resource *res;
	/* [한국어] PCI 버스 쪽에서 본 시작 주소. */
	phys_addr_t addr;

	/* [한국어] 창 목록에서 첫 메모리 창을 찾는다. */
	win = resource_list_first_type(&bridge->windows, IORESOURCE_MEM);
	/* [한국어] 있으면 설정한다. */
	if (win) {
		/* [한국어] 베이스 레지스터에 쓸 값. */
		u32 pcimembase;

		/* [한국어] 자원 서술자를 꺼낸다. */
		res = win->res;
		/* [한국어] CPU 주소에서 offset 을 빼면 PCI 버스 주소가 된다. offset 이
		 * 두 좌표계의 차이를 담고 있기 때문이다. */
		addr = res->start - win->offset;

		/* [한국어] 프리페치 가능한 창인가. */
		if (res->flags & IORESOURCE_PREFETCH)
			/* [한국어] /proc/iomem 에 이 이름으로 보인다. */
			res->name = "IXP4xx PCI PRE-MEM";
		/* [한국어] 아니면. */
		else
			/* [한국어] 프리페치 불가 창으로 이름 붙인다. */
			res->name = "IXP4xx PCI NON-PRE-MEM";

		/* [한국어] 어떤 창을 잡았는지 기록해 둔다. */
		dev_dbg(dev, "%s window %pR, bus addr %pa\n",
			res->name, res, &addr);
		/* [한국어] 위 addr_to_64mconf 가 16MB 짜리 넷으로 펴는 방식이라 정확히
		 * 64MB 여야만 한다. */
		if (resource_size(res) != SZ_64M) {
			/* [한국어] 크기가 다르면 표현할 수 없다. */
			dev_err(dev, "memory range is not 64MB\n");
			/* [한국어] 이 함수의 두 실패 중 하나다. */
			return -EINVAL;
		}

		/* [한국어] 64MB 시작 주소를 네 창의 설정값으로 편다. */
		pcimembase = ixp4xx_pci_addr_to_64mconf(addr);
		/* Commit configuration */
		/* [한국어] 옆의 상류 주석대로 실제 레지스터에 반영한다. */
		ixp4xx_writel(p, IXP4XX_PCI_PCIMEMBASE, pcimembase);
	/* [한국어] 메모리 창이 없다. */
	} else {
		/* [한국어] 위 [상류 코드 관찰] 이 가리키는 지점 — 오류로 알리면서도
		 * 프로브를 멈추지 않는다. */
		dev_err(dev, "no AHB memory mapping defined\n");
	}

	/* [한국어] 이어서 첫 IO 창을 찾는다. */
	win = resource_list_first_type(&bridge->windows, IORESOURCE_IO);
	/* [한국어] 있으면 설정한다. */
	if (win) {
		/* [한국어] 자원 서술자를 꺼낸다. */
		res = win->res;

		/* [한국어] 리소스에는 커널의 추상 PIO 번호가 들어 있으므로 진짜 물리 주소로
		 * 되돌린다. */
		addr = pci_pio_to_address(res->start);
		/* [한국어] 아래 상류 주석대로 상위 24비트만 레지스터에 담기므로 하위 8비트가
		 * 0 이어야 한다. */
		if (addr & 0xff) {
			/* [한국어] 정렬이 맞지 않으면 창을 정확히 표현할 수 없다. */
			dev_err(dev, "IO mem at uneven address: %pa\n", &addr);
			/* [한국어] 이 함수의 나머지 한 실패다. */
			return -EINVAL;
		}

		/* [한국어] /proc/ioport 계열 표시에 쓸 이름을 준다. */
		res->name = "IXP4xx PCI IO MEM";
		/*
		 * Setup I/O space location for PCI->AHB access, the
		 * upper 24 bits of the address goes into the lower
		 * 24 bits of this register.
		 */
		/* [한국어] 위 상류 주석대로 상위 24비트를 레지스터의 하위 24비트에 넣는다.
		 * 8비트 오른쪽 시프트가 그 옮김이다. */
		ixp4xx_writel(p, IXP4XX_PCI_AHBIOBASE, (addr >> 8));
	/* [한국어] IO 창이 없다. */
	} else {
		/* [한국어] 메모리 창과 달리 dev_info 다 — IO 공간은 없어도 정상인 구성이
		 * 흔하기 때문으로 보인다. */
		dev_info(dev, "no IO space AHB memory mapping defined\n");
	}

	/* [한국어] 크기·정렬 문제가 없었으면 성공이다. 창이 아예 없었어도 그렇다. */
	return 0;
}

/* [한국어]
 * ixp4xx_pci_parse_map_dma_ranges - DT 의 dma-ranges 를 읽어 인바운드 창을 설정한다
 *
 * @p: 이 드라이버의 상태.
 * @return: 0 성공, -EINVAL 이면 창 크기가 64MB 가 아니다.
 *
 * 위 parse_map_ranges 의 반대 방향이다. PCI 쪽 장치가 DMA 로 내보내는 접근이
 * AHB(시스템 메모리)의 어느 주소로 갈지를 정한다.
 *
 * 구조가 아웃바운드 메모리 창과 거의 같다 — 64MB 강제, addr_to_64mconf 로
 * 펴기, 레지스터 하나 쓰기. 다른 것은 목록이 bridge->windows 가 아니라
 * bridge->dma_ranges 라는 점과, 자원 이름을 바꾸지 않는다는 점이다.
 *
 * 아래 상류 주석이 16MB 창 넷이라는 배치를 명시적으로 적어 두어, 위
 * addr_to_64mconf 가 무엇을 하는 함수인지 이 자리에서 확인된다.
 *
 * [상류 코드 관찰] dma-ranges 가 없으면 dev_err 로 알리면서도 0 을 돌려주어
 * 프로브가 계속된다. 인바운드 창이 설정되지 않은 채 버스를 스캔하게 되므로
 * 그 아래 장치의 DMA 는 동작하지 않겠지만, 코드는 그것을 실패로 보지 않는다.
 * 원본 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * 실행 컨텍스트: probe. 잠들지 않는다.
 *
 * 에러 경로: 크기가 64MB 가 아닐 때만 -EINVAL.
 *
 * 호출 체인:
 *   ixp4xx_pci_probe() → [이 함수]
 *     → resource_list_first_type(), ixp4xx_pci_addr_to_64mconf(), ixp4xx_writel()
 */
static int ixp4xx_pci_parse_map_dma_ranges(struct ixp4xx_pci *p)
{
	/* [한국어] 로그에 쓸 장치. */
	struct device *dev = p->dev;
	/* [한국어] 상태 구조체에서 브리지로 거슬러 올라간다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(p);
	/* [한국어] dma_ranges 목록에서 꺼낸 항목. */
	struct resource_entry *win;
	/* [한국어] 그 항목의 자원 서술자. */
	struct resource *res;
	/* [한국어] PCI 버스 쪽에서 본 시작 주소. */
	phys_addr_t addr;
	/* [한국어] 베이스 레지스터에 쓸 값. 위 함수와 달리 블록 밖에 선언되어 있다. */
	u32 ahbmembase;

	/* [한국어] 아웃바운드와 달리 dma_ranges 목록을 본다. */
	win = resource_list_first_type(&bridge->dma_ranges, IORESOURCE_MEM);
	/* [한국어] 있으면 설정한다. */
	if (win) {
		/* [한국어] 자원 서술자를 꺼낸다. */
		res = win->res;
		/* [한국어] offset 을 빼서 PCI 버스 주소로 옮긴다. */
		addr = res->start - win->offset;

		/* [한국어] 아웃바운드와 같은 이유로 정확히 64MB 여야 한다. */
		if (resource_size(res) != SZ_64M) {
			/* [한국어] 크기가 다르면 표현할 수 없다. */
			dev_err(dev, "DMA memory range is not 64MB\n");
			/* [한국어] 이 함수의 유일한 실패다. */
			return -EINVAL;
		}

		/* [한국어] 어떤 창을 잡았는지 기록해 둔다. */
		dev_dbg(dev, "DMA MEM BASE: %pa\n", &addr);
		/*
		 * 4 PCI-to-AHB windows of 16 MB each, write the 8 high bits
		 * into each byte of the PCI_AHBMEMBASE register.
		 */
		/* [한국어] 위 상류 주석대로 상위 8비트씩을 네 바이트에 나눠 담는다. */
		ahbmembase = ixp4xx_pci_addr_to_64mconf(addr);
		/* Commit AHB membase */
		/* [한국어] 옆의 상류 주석대로 실제 레지스터에 반영한다. */
		ixp4xx_writel(p, IXP4XX_PCI_AHBMEMBASE, ahbmembase);
	/* [한국어] dma-ranges 가 없다. */
	} else {
		/* [한국어] 위 [상류 코드 관찰] 이 가리키는 지점 — 오류로 알리면서도
		 * 프로브를 멈추지 않는다. */
		dev_err(dev, "no DMA memory range defined\n");
	}

	/* [한국어] 크기 문제가 없었으면 성공이다. */
	return 0;
}

/* [한국어] 아래 전역 하나와 함수 하나는 ARM 아키텍처 전용이다.
 * hook_fault_code() 와 regs->ARM_pc 가 arch/arm 에만 있어 다른
 * 아키텍처에서는 컴파일되지 않기 때문이다. COMPILE_TEST 로 다른 아키텍처에서
 * 빌드할 때를 위해 통째로 감싸 두었다. */
#ifdef CONFIG_ARM
/* Only used to get context for abort handling */
/* [한국어] 어보트 핸들러가 이 드라이버의 상태를 찾을 유일한 통로.
 * 설정자: ixp4xx_pci_probe() 가 hook_fault_code 를 부르기 직전에 채운다.
 * 읽는 자: ixp4xx_pci_abort_handler() 의 첫 줄.
 * 값 범위: 유효한 ixp4xx_pci 포인터. 프로브 전에는 NULL 이다.
 * 동기화: 없다. 프로브에서 한 번 쓰고 예외 컨텍스트에서 읽기만 한다.
 *
 * 옆의 상류 주석이 밝히듯 문맥을 얻기 위한 것이다. ARM 의 어보트 핸들러
 * 원형이 드라이버가 넘긴 데이터를 받을 자리를 두지 않아, 전역 말고는 방법이
 * 없다. 그래서 이 드라이버는 컨트롤러가 둘 이상인 시스템을 다룰 수 없는데,
 * IXP4xx 에 PCI 호스트가 하나뿐이라 문제가 되지 않는 구조다. */
static struct ixp4xx_pci *ixp4xx_pci_abort_singleton;

/* [한국어]
 * ixp4xx_pci_abort_handler - PCI 오류로 생긴 ARM 데이터 어보트를 수습한다
 *
 * @addr: 어보트를 일으킨 접근의 주소.
 * @fsr: Fault Status Register 값. 비트 10 이 imprecise 여부를 알린다.
 * @regs: 예외가 난 지점의 레지스터 상태. 복귀 주소를 고칠 수 있다.
 * @return: 0 이면 "처리했으니 실행을 계속하라".
 *
 * 이 파일에서 요즘 드라이버와 가장 다른 부분이다. PCIe 라면 AER 인터럽트로
 * 올라왔을 오류가, 여기서는 CPU 의 데이터 어보트 예외로 올라온다.
 * 그 경로는 이렇다.
 *
 *   설정 사이클에 응답 없음 → PCI 마스터 어보트
 *     → CSR 의 ABE 비트가 켜져 있으므로 AHB 버스 오류로 전파
 *     → ARM 데이터 어보트 예외 → [이 함수]
 *
 * 하는 일은 셋이다. 상태를 읽어 로그로 남기고, 마스터 어보트 비트를 양쪽
 * (컨트롤러의 ISR 과 자기 PCI_STATUS)에서 지우고, imprecise abort 였으면
 * 복귀 주소를 명령어 하나만큼 밀어 준다.
 *
 * 마지막 것이 이 함수의 핵심이다. imprecise abort 는 어느 명령이 문제를
 * 일으켰는지 정확히 알 수 없는 종류라, 복귀 주소가 문제의 명령을 가리키고
 * 있을 수 있다. 그대로 돌아가면 같은 접근을 되풀이해 무한 루프가 되므로,
 * 4바이트(ARM 명령어 하나) 앞으로 밀어 그 명령을 건너뛴다. 아래 상류 주석이
 * 그 사정을 적어 두었다.
 *
 * 0 을 돌려주는 것은 "이 어보트는 내가 처리했다" 는 뜻이다. 처리하지 않으면
 * 커널이 그 접근을 치명적 오류로 보고 죽는다. 즉 이 핸들러가 없으면 빈 슬롯을
 * 훑는 것만으로도 부팅이 실패한다 — 그래서 probe 가 창을 매핑한 직후, 버스를
 * 스캔하기 전에 이것을 걸어 두는 것이다.
 *
 * [상류 코드 관찰] 전역 싱글턴에서 문맥을 가져오며 NULL 검사를 하지 않는다.
 * 이 핸들러는 probe 가 싱글턴을 채운 뒤에 걸리므로 실제로는 NULL 이 될 수
 * 없는 구조이나, 검사가 없는 것은 사실이다.
 *
 * [상류 코드 관찰] ixp4xx_crp_read_config 를 크기 2 로 부르므로 그 함수는
 * 언제나 성공한다. 따라서 바로 아래의 오류 처리는 도달하지 않는다.
 * 아래 crp_write_config 의 오류 처리도 같다.
 *
 * 실행 컨텍스트: ARM 데이터 어보트 예외 컨텍스트. 인터럽트 핸들러보다도
 * 제약이 심한 자리이며, 당연히 잠들 수 없다.
 *
 * 에러 경로: 하위 호출이 실패하면 로그를 남기는 것이 전부다. 첫 번째 실패만
 * -EINVAL 로 빠져나가는데, 그 경우 어보트가 처리되지 않은 것으로 취급된다.
 *
 * 호출 체인:
 *   ARM 데이터 어보트 예외 → hook_fault_code 로 등록된 [이 함수]
 *     → ixp4xx_readl(), ixp4xx_crp_read_config(), ixp4xx_writel(),
 *       ixp4xx_crp_write_config()
 */
static int ixp4xx_pci_abort_handler(unsigned long addr, unsigned int fsr,
				    struct pt_regs *regs)
{
	/* [한국어] 전역 싱글턴에서 문맥을 되찾는다. 어보트 핸들러 원형에 데이터를
	 * 실을 자리가 없어 이 방법뿐이다. */
	struct ixp4xx_pci *p = ixp4xx_pci_abort_singleton;
	/* [한국어] isr 은 컨트롤러의 인터럽트 상태, status 는 컨트롤러 자신의
	 * PCI_STATUS 레지스터 값이다. */
	u32 isr, status;
	/* [한국어] CRP 접근의 결과. */
	int ret;

	/* [한국어] 어떤 오류가 걸렸는지 컨트롤러 쪽에서 읽는다. */
	isr = ixp4xx_readl(p, IXP4XX_PCI_ISR);
	/* [한국어] 컨트롤러 자신의 설정공간 PCI_STATUS 도 읽는다. 표준 PCI 상태
	 * 레지스터라 마스터 어보트 수신 비트가 여기에도 선다. */
	ret = ixp4xx_crp_read_config(p, PCI_STATUS, 2, &status);
	/* [한국어] 읽기 실패. 위 [상류 코드 관찰] 대로 크기 2 로는 도달하지 않는다. */
	if (ret) {
		/* [한국어] 무엇이 문제인지 알 수 없으면 수습할 수도 없다. */
		dev_err(p->dev, "unable to read abort status\n");
		/* [한국어] 0 이 아닌 값을 돌려주면 커널이 어보트를 처리되지 않은 것으로 본다. */
		return -EINVAL;
	}

	/* [한국어] 주소와 두 상태를 함께 남긴다. 이 시대 하드웨어에서 PCI 문제를
	 * 쫓을 때 볼 수 있는 정보가 사실상 이것뿐이다. */
	dev_err(p->dev,
		"PCI: abort_handler addr = %#lx, isr = %#x, status = %#x\n",
		addr, isr, status);

	/* Make sure the Master Abort bit is reset */
	/* [한국어] 옆의 상류 주석대로 컨트롤러 쪽 마스터 어보트 비트를 지운다.
	 * 지우지 않으면 다음 설정공간 접근이 어보트로 오판된다. */
	ixp4xx_writel(p, IXP4XX_PCI_ISR, IXP4XX_PCI_ISR_PFE);
	/* [한국어] 자기 PCI_STATUS 쪽 비트도 지워야 한다. 이 레지스터는 write-1-to-clear
	 * 라, 읽은 값에 그 비트를 세워 되쓰는 것이 곧 지우는 동작이다. */
	status |= PCI_STATUS_REC_MASTER_ABORT;
	/* [한국어] CRP 창구로 되쓴다. */
	ret = ixp4xx_crp_write_config(p, PCI_STATUS, 2, status);
	/* [한국어] 쓰기 실패. 위 관찰대로 크기 2 로는 도달하지 않는다. */
	if (ret)
		/* [한국어] 지우지 못했다는 사실만 남기고 계속 진행한다 — 여기서 포기하면
		 * 어보트가 수습되지 않아 더 나쁘기 때문이다. */
		dev_err(p->dev, "unable to clear abort status bit\n");

	/*
	 * If it was an imprecise abort, then we need to correct the
	 * return address to be _after_ the instruction.
	 */
	/* [한국어] 위 상류 주석대로, imprecise abort 는 어느 명령이 문제였는지
	 * 정확히 알 수 없는 종류다. FSR 의 비트 10 이 그것을 알린다. */
	if (fsr & (1 << 10)) {
		/* [한국어] 그 사실을 남긴다. */
		dev_err(p->dev, "imprecise abort\n");
		/* [한국어] 복귀 주소를 ARM 명령어 하나(4바이트)만큼 민다. 이렇게 하지 않으면
		 * 문제의 접근으로 되돌아가 무한히 반복된다. */
		regs->ARM_pc += 4;
	}

	/* [한국어] "처리했으니 실행을 계속하라". 이 값을 돌려주지 않으면 커널이
	 * 이 접근을 치명적 오류로 보고 죽는다 — 빈 슬롯 스캔만으로 부팅이 실패하게 된다. */
	return 0;
}
/* [한국어] CONFIG_ARM 갈래 끝. */
#endif

/* [한국어]
 * ixp4xx_pci_probe - 이 컨트롤러를 붙인다 (플랫폼 드라이버 진입점)
 *
 * @pdev: 플랫폼 코어가 DT 노드와 매칭해 넘겨준 디바이스.
 * @return: 0 성공, 음수 실패.
 *
 * 이 파일에서 가장 긴 함수이며, 초기화 순서가 곧 내용 전부다.
 *   1. 브리지와 이 드라이버의 상태를 한 덩어리로 잡고 pci_ops 를 건다.
 *   2. compatible 로 ixp42x 인지 가려 errata 회피를 켠다.
 *   3. 유일한 레지스터 창을 매핑한다.
 *   4. CSR 을 읽어 호스트 모드인지 옵션 모드인지 판정한다.
 *   5. ARM 데이터 어보트 핸들러를 건다 — 버스를 스캔하기 전에 반드시.
 *   6. 아웃바운드/인바운드 주소 창을 설정한다.
 *   7. (호스트 모드면) CRP 로 자기 BAR 여섯 개와 타임아웃을 세운다.
 *   8. 인터럽트 상태를 지우고 CSR 에 Initialize Complete 를 세운다.
 *   9. CRP 로 PCI_COMMAND 에 마스터·메모리 비트를 세운다.
 *  10. 버스를 스캔한다.
 *
 * 5번의 위치가 중요하다. 빈 슬롯을 훑으면 마스터 어보트가 데이터 어보트로
 * 올라오는데, 그때 핸들러가 없으면 커널이 죽는다. 그래서 10번보다 반드시
 * 앞서야 한다.
 *
 * 8번의 Initialize Complete 도 마찬가지다. 아래 상류 주석이 밝히듯 그 비트를
 * 세워야 컨트롤러가 설정 사이클을 만들어 내므로, 역시 10번보다 앞서야 한다.
 *
 * 7번의 BAR 설정이 이 시대 하드웨어의 성격을 보여 준다. 이 컨트롤러는 PCI
 * 버스에서 보면 자기도 하나의 장치라, 다른 마스터가 시스템 메모리에 닿을 수
 * 있도록 자기 BAR 를 열어 주어야 한다. 16MB 짜리 넷으로 64MB 를 덮고,
 * 그 뒤에 CSR 창과 IO 창을 하나씩 더 둔다.
 *
 * 이 함수가 __init 인 것과 아래 platform_driver 에 .probe 가 없는 것이 짝을
 * 이룬다 — 아래 상류 주석이 그 사정을 설명하며, 요지는 5번 때문에 이 코드가
 * 모듈이 될 수 없다는 것이다.
 *
 * [상류 코드 관찰] 마지막 pci_host_probe() 의 반환값을 보지 않고 언제나 0 을
 * 돌려준다. 버스 스캔이 실패해도 프로브는 성공한 것으로 보고된다. 원본
 * 스냅숏(1f0e418bb6)에서 확인했으며 코드는 손대지 않았다.
 *
 * [상류 코드 관찰] dev_set_drvdata() 로 상태를 심어 두지만 이 파일에서 그것을
 * 읽는 곳이 없다(전수 확인). remove 경로가 없어 되찾을 일이 없기 때문이다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 프로세스 컨텍스트다.
 *
 * 에러 경로: 되감기 라벨이 하나도 없다. 실패하면 그대로 반환하며, 잡은 것은
 * 모두 devm 이라 자동으로 풀린다. 다만 5번에서 건 어보트 핸들러는 devm 이
 * 아니어서 풀리지 않는데, 이 드라이버가 떨어지지 않으므로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_pci_alloc_host_bridge(), devm_platform_ioremap_resource(),
 *       hook_fault_code(), ixp4xx_pci_parse_map_ranges(),
 *       ixp4xx_pci_parse_map_dma_ranges(), ixp4xx_crp_write_config(),
 *       pci_host_probe()
 */
static int __init ixp4xx_pci_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 자원 조회에 두루 쓸 장치. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 DT 노드. compatible 검사에 쓴다. */
	struct device_node *np = dev->of_node;
	/* [한국어] 이 드라이버의 상태. 브리지 뒤의 자리를 가리키게 된다. */
	struct ixp4xx_pci *p;
	/* [한국어] PCI 코어가 관리하는 호스트 브리지 객체. */
	struct pci_host_bridge *host;
	/* [한국어] 각 단계의 결과. */
	int ret;
	/* [한국어] CSR 을 읽고 쓰는 데 쓰는 임시 값. */
	u32 val;
	/* [한국어] 자기 BAR 에 넣을 물리 주소. 루프를 돌며 16MB 씩 늘어난다. */
	phys_addr_t addr;
	/* [한국어] 설정할 BAR 네 개의 설정공간 오프셋 표. 루프에서 첨자로 꺼내 쓴다. */
	u32 basereg[4] = {
		PCI_BASE_ADDRESS_0,
		PCI_BASE_ADDRESS_1,
		PCI_BASE_ADDRESS_2,
		PCI_BASE_ADDRESS_3,
	};
	/* [한국어] BAR 루프의 커서. */
	int i;

	/* [한국어] 브리지와 이 드라이버의 상태를 한 번에 잡는다. sizeof(*p) 만큼
	 * 여분을 붙여 달라는 뜻이며, 0 초기화도 여기서 보장된다. */
	host = devm_pci_alloc_host_bridge(dev, sizeof(*p));
	/* [한국어] 할당 실패. */
	if (!host)
		/* [한국어] 아직 잡은 것이 없으므로 그대로 반환한다. */
		return -ENOMEM;

	/* [한국어] 설정공간 접근 함수 표를 건다. */
	host->ops = &ixp4xx_pci_ops;
	/* [한국어] 브리지 뒤에 붙은 여분 공간의 주소를 얻는다. 이것이 이 드라이버의
	 * 상태 구조체다. */
	p = pci_host_bridge_priv(host);
	/* [한국어] 설정공간 접근 함수들이 bus->sysdata 로 되찾을 수 있게 심어 둔다. */
	host->sysdata = p;
	/* [한국어] 로그에 쓸 장치를 채운다. */
	p->dev = dev;
	/* [한국어] 위 [상류 코드 관찰] 대로 이 파일에서 되읽는 곳은 없다. */
	dev_set_drvdata(dev, p);

	/*
	 * Set up quirk for erratic behaviour in the 42x variant
	 * when accessing config space.
	 */
	/* [한국어] 위 상류 주석대로 ixp42x 판본에만 설정공간 접근 errata 가 있다.
	 * 이 검사가 이 파일에서 두 SoC 판본을 가르는 유일한 지점이다. */
	if (of_device_is_compatible(np, "intel,ixp42x-pci")) {
		/* [한국어] 읽기 경로가 같은 접근을 열여섯 번 되풀이하게 된다. */
		p->errata_hammer = true;
		/* [한국어] 성능에 영향이 있는 회피책이라 사용자에게 알린다. */
		dev_info(dev, "activate hammering errata\n");
	}

	/* [한국어] 유일한 레지스터 창을 매핑한다. 이름이 아니라 첨자 0 으로 얻는
	 * 것이 요즘 드라이버와 다른데, 자원이 하나뿐이라 이름을 붙일 이유가 없다. */
	p->base = devm_platform_ioremap_resource(pdev, 0);
	/* [한국어] 자원이 없거나 매핑 실패. */
	if (IS_ERR(p->base))
		/* [한국어] 코드를 꺼내 올린다. */
		return PTR_ERR(p->base);

	/* [한국어] 제어·상태 레지스터를 읽는다. */
	val = ixp4xx_readl(p, IXP4XX_PCI_CSR);
	/* [한국어] 비트 0 이 호스트 모드 표시다. 소프트웨어가 정하는 것이 아니라
	 * 부팅 시 핀 설정으로 하드웨어가 알려 주는 값이며, !! 로 bool 로 정규화한다. */
	p->host_mode = !!(val & IXP4XX_PCI_CSR_HOST);
	/* [한국어] 어느 모드인지 남긴다. 병렬 PCI 시대에는 같은 칩이 호스트가 될 수도
	 * 옵션 보드가 될 수도 있었다. */
	dev_info(dev, "controller is in %s mode\n",
		 p->host_mode ? "host" : "option");

/* [한국어] 어보트 핸들러 등록은 ARM 에서만 가능하다. 다른 아키텍처에서는
 * 이 블록이 통째로 빠지고, 그 빌드에서는 PCI 오류가 수습되지 않는다 —
 * COMPILE_TEST 용 구성이라 실동작을 전제하지 않는다. */
#ifdef CONFIG_ARM
	/* Hook in our fault handler for PCI errors */
	/* [한국어] 핸들러가 문맥을 찾을 유일한 통로를 채운다. hook_fault_code 보다
	 * 반드시 앞서야 한다 — 등록 직후 어보트가 나면 이 값을 읽기 때문이다. */
	ixp4xx_pci_abort_singleton = p;
	/* [한국어] 옆의 상류 주석대로 PCI 오류용 fault 핸들러를 건다.
	 * 16+6 은 ARM 의 fault status 코드로, 마지막 인자 문자열이 그것이 imprecise
	 * external abort 임을 알린다. SIGBUS 는 커널이 이 어보트를 유저 프로세스에
	 * 전할 때 쓸 신호다. 이 함수는 arch/arm 에 있어 이 트리에서 확인 못 함.
	 * 이 등록이 버스 스캔보다 앞서야 한다는 것이 이 함수 순서의 핵심이다. */
	hook_fault_code(16+6, ixp4xx_pci_abort_handler, SIGBUS, 0,
			"imprecise external abort");
/* [한국어] CONFIG_ARM 갈래 끝. */
#endif

	/* [한국어] 아웃바운드(AHB → PCI) 창을 설정한다. */
	ret = ixp4xx_pci_parse_map_ranges(p);
	/* [한국어] 창 크기나 정렬이 맞지 않았다. */
	if (ret)
		/* [한국어] devm 이라 되감을 것이 없어 그대로 반환한다. */
		return ret;

	/* [한국어] 인바운드(PCI → AHB) 창을 설정한다. */
	ret = ixp4xx_pci_parse_map_dma_ranges(p);
	/* [한국어] 창 크기가 맞지 않았다. */
	if (ret)
		/* [한국어] 그대로 반환한다. */
		return ret;

	/* This is only configured in host mode */
	/* [한국어] 옆의 상류 주석대로 아래 BAR 설정은 호스트 모드에서만 뜻이 있다.
	 * 옵션 모드에서는 호스트가 이 장치의 BAR 를 배정해 주기 때문이다. */
	if (p->host_mode) {
		/* [한국어] 커널 선형 매핑의 시작을 물리 주소로 바꾼다 — 곧 RAM 의 시작이다.
		 * 이 주소부터 64MB 를 PCI 마스터에게 열어 준다. */
		addr = __pa(PAGE_OFFSET);
		/* This is a noop (0x00) but explains what is going on */
		/* [한국어] 옆의 상류 주석대로 값이 0 이라 아무것도 바꾸지 않는다.
		 * 이 BAR 가 IO 가 아니라 메모리 공간임을 코드로 드러내려고 둔 줄이다. */
		addr |= PCI_BASE_ADDRESS_SPACE_MEMORY;

		/* [한국어] BAR0~BAR3 을 16MB 씩 네 구간으로 채운다. 위 인바운드 창이
		 * 16MB 짜리 넷이었던 것과 짝이 맞는다. */
		for (i = 0; i < 4; i++) {
			/* Write this directly into the config space */
			/* [한국어] 옆의 상류 주석대로 CRP 창구로 자기 설정공간에 직접 쓴다.
			 * 바깥 버스로 나가는 접근이 아니다. */
			ret = ixp4xx_crp_write_config(p, basereg[i], 4, addr);
			/* [한국어] 실패. 크기 4 로는 도달하지 않는다 — crp_write_config 의
			 * [상류 코드 관찰] 참조. */
			if (ret)
				/* [한국어] 어느 BAR 였는지 남긴다. */
				dev_err(dev, "failed to set up PCI_BASE_ADDRESS_%d\n", i);
			/* [한국어] 성공. */
			else
				/* [한국어] 어떤 주소를 넣었는지 남긴다. */
				dev_info(dev, "set PCI_BASE_ADDR_%d to %pa\n", i, &addr);
			/* [한국어] 다음 16MB 구간으로 옮긴다. */
			addr += SZ_16M;
		}

		/*
		 * Enable CSR window at 64 MiB to allow PCI masters to continue
		 * prefetching past the 64 MiB boundary, if all AHB to PCI
		 * windows are consecutive.
		 */
		/* [한국어] 위 상류 주석대로 BAR4 를 64MB 지점에 둔다. 루프가 끝난 뒤의
		 * addr 이 정확히 그 자리다. 앞의 넷과 이어져 있어야 마스터가 경계를 넘어
		 * 프리페치를 계속할 수 있다는 것이 그 주석의 요지다. */
		ret = ixp4xx_crp_write_config(p, PCI_BASE_ADDRESS_4, 4, addr);
		/* [한국어] 실패. 역시 도달하지 않는다. */
		if (ret)
			/* [한국어] 알린다. */
			dev_err(dev, "failed to set up PCI_BASE_ADDRESS_4\n");
		/* [한국어] 성공. */
		else
			/* [한국어] 어떤 주소를 넣었는지 남긴다. */
			dev_info(dev, "set PCI_BASE_ADDR_4 to %pa\n", &addr);

		/*
		 * Put the IO memory window at the very end of physical memory
		 * at 0xfffffc00. This is when the system is trying to access IO
		 * memory over AHB.
		 */
		/* [한국어] 위 상류 주석대로 IO 창은 물리 메모리의 맨 끝에 둔다.
		 * AHB 쪽에서 PCI IO 공간에 닿을 때 쓰는 자리다. */
		addr = 0xfffffc00;
		/* [한국어] 이 BAR 는 메모리가 아니라 IO 공간임을 표시한다. 위 BAR0~4 가
		 * 값 0 짜리 메모리 표시를 붙였던 것과 대비된다. */
		addr |= PCI_BASE_ADDRESS_SPACE_IO;
		/* [한국어] BAR5 에 써 넣는다. */
		ret = ixp4xx_crp_write_config(p, PCI_BASE_ADDRESS_5, 4, addr);
		/* [한국어] 실패. */
		if (ret)
			/* [한국어] 알린다. */
			dev_err(dev, "failed to set up PCI_BASE_ADDRESS_5\n");
		/* [한국어] 성공. */
		else
			/* [한국어] 어떤 주소를 넣었는지 남긴다. */
			dev_info(dev, "set PCI_BASE_ADDR_5 to %pa\n", &addr);

		/*
		 * Retry timeout to 0x80
		 * Transfer ready timeout to 0xff
		 */
		/* [한국어] 위 상류 주석대로 재시도 타임아웃 0x80, 전송 준비 타임아웃 0xff 를
		 * 한 워드로 써 넣는다. 설정공간 0x40 부터는 벤더 전용 영역이다. */
		ret = ixp4xx_crp_write_config(p, IXP4XX_PCI_RTOTTO, 4,
					      0x000080ff);
		/* [한국어] 실패. */
		if (ret)
			/* [한국어] 알린다. */
			dev_err(dev, "failed to set up TRDY limit\n");
		/* [한국어] 성공. */
		else
			/* [한국어] 어떤 값을 넣었는지 남긴다. */
			dev_info(dev, "set TRDY limit to 0x80ff\n");
	}

	/* Clear interrupts */
	/* [한국어] 옆의 상류 주석대로 오류 상태를 모두 지우고 시작한다. 부트로더가
	 * 남겼을 수 있는 비트를 치우는 단계이며, write-1-to-clear 라 세운 비트가 지워진다. */
	val = IXP4XX_PCI_ISR_PSE | IXP4XX_PCI_ISR_PFE | IXP4XX_PCI_ISR_PPE | IXP4XX_PCI_ISR_AHBE;
	/* [한국어] 실제로 지운다. */
	ixp4xx_writel(p, IXP4XX_PCI_ISR, val);

	/*
	 * Set Initialize Complete in PCI Control Register: allow IXP4XX to
	 * generate PCI configuration cycles. Specify that the AHB bus is
	 * operating in big-endian mode. Set up byte lane swapping between
	 * little-endian PCI and the big-endian AHB bus.
	 */
	/* [한국어] 위 상류 주석이 이 두 비트의 뜻을 설명한다. IC 는 초기화 완료 —
	 * 이 비트가 서야 컨트롤러가 설정 사이클을 만들어 내므로, 아래 버스 스캔보다
	 * 반드시 앞서야 한다. ABE 는 AHB 버스 오류를 켜는 것으로, 이 비트가 있어야
	 * 마스터 어보트가 데이터 어보트로 올라와 위 핸들러에 닿는다. */
	val = IXP4XX_PCI_CSR_IC | IXP4XX_PCI_CSR_ABE;
	/* [한국어] 빅엔디안으로 빌드된 커널인가. 컴파일 타임 상수라 최적화로 정리된다. */
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))
		/* [한국어] 위 상류 주석대로 리틀엔디안 PCI 와 빅엔디안 AHB 사이의 바이트
		 * 레인 교환을 켠다. 리틀엔디안 빌드에서는 교환이 필요 없어 켜지 않는다. */
		val |= (IXP4XX_PCI_CSR_PDS | IXP4XX_PCI_CSR_ADS);
	/* [한국어] 준비한 값을 CSR 에 쓴다. 이 순간부터 설정 사이클이 나갈 수 있다. */
	ixp4xx_writel(p, IXP4XX_PCI_CSR, val);

	/* [한국어] 컨트롤러 자신의 PCI_COMMAND 에 버스 마스터와 메모리 응답을 켠다.
	 * 마스터 비트가 있어야 이 컨트롤러가 버스로 트랜잭션을 낼 수 있고,
	 * 메모리 비트가 있어야 위에서 설정한 자기 BAR 로 들어오는 접근에 응답한다. */
	ret = ixp4xx_crp_write_config(p, PCI_COMMAND, 2, PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY);
	/* [한국어] 실패. 크기 2 로는 도달하지 않는다. */
	if (ret)
		/* [한국어] 알린다. */
		dev_err(dev, "unable to initialize master and command memory\n");
	/* [한국어] 성공. */
	else
		/* [한국어] 마스터로 동작할 준비가 됐음을 남긴다. */
		dev_info(dev, "initialized as master\n");

	/* [한국어] 버스를 스캔한다. 이 안에서 설정공간을 훑어 장치를 찾고, 없는
	 * 슬롯은 마스터 어보트로 걸러진다 — 그 어보트를 수습하는 것이 위에서 걸어 둔
	 * 핸들러다. 위 [상류 코드 관찰] 대로 반환값을 보지 않는다. */
	pci_host_probe(host);

	/* [한국어] 스캔 결과와 무관하게 성공으로 보고한다. */
	return 0;
}

/* [한국어] DT compatible 과 이 드라이버를 잇는 매칭 목록. 다른 드라이버들과
 * 달리 .data 가 비어 있다 — 판본별로 달라지는 것이 errata_hammer 하나뿐이고,
 * 그것을 probe 가 of_device_is_compatible() 로 직접 가리기 때문이다. */
static const struct of_device_id ixp4xx_pci_of_match[] = {
	/* [한국어] 첫 항목: IXP42x. 설정공간 접근 errata 가 있는 판본이다. */
	{
		/* [한국어] probe 가 이 문자열을 다시 검사해 errata_hammer 를 켠다. */
		.compatible = "intel,ixp42x-pci",
	},
	/* [한국어] 둘째 항목: IXP43x. errata 가 없는 판본이다. */
	{
		/* [한국어] 이 문자열이면 errata_hammer 가 false 로 남는다. */
		.compatible = "intel,ixp43x-pci",
	},
	/* [한국어] 목록의 끝을 알리는 빈 항목.
	 * [상류 코드 관찰] 이 파일에는 MODULE_DEVICE_TABLE 선언이 없다.
	 * 아래 상류 주석이 밝히듯 모듈이 될 수 없는 드라이버라 모듈 자동 로딩용
	 * 별칭이 필요 없기 때문이다. */
	{},
};

/*
 * This driver needs to be a builtin module with suppressed bind
 * attributes since the probe() is initializing a hard exception
 * handler and this can only be done from __init-tagged code
 * sections. This module cannot be removed and inserted at all.
 */
/* [한국어] 위 상류 주석이 이 드라이버 구조의 근본 이유를 밝힌다 —
 * probe 가 하드 예외 핸들러를 등록하고, 그 일은 __init 구역의 코드에서만 할 수
 * 있으므로 이 드라이버는 붙박이여야 하며 뺐다 넣었다 할 수 없다.
 *
 * 그래서 아래 표에 .probe 가 없다. __init 함수의 주소를 구조체에 담아 두면
 * 초기화가 끝난 뒤 그 메모리가 해제되어 죽은 포인터가 남기 때문이며,
 * 대신 맨 아래 builtin_platform_driver_probe() 가 probe 를 따로 받는다. */
static struct platform_driver ixp4xx_pci_driver = {
	/* [한국어] 드라이버 코어에 전달할 공통 정보. */
	.driver = {
		/* [한국어] sysfs 와 로그에 나타나는 드라이버 이름. */
		.name = "ixp4xx-pci",
		/* [한국어] 위 상류 주석대로 sysfs 의 bind/unbind 속성을 만들지 않는다.
		 * 손으로 언바인드하면 __init 구역에서 해제된 코드로 뛰게 된다. */
		.suppress_bind_attrs = true,
		/* [한국어] 위의 DT 매칭 목록. */
		.of_match_table = ixp4xx_pci_of_match,
	},
};
/* [한국어] 붙박이 등록의 특별한 판. 보통의 builtin_platform_driver 와 달리
 * probe 함수를 두 번째 인자로 따로 받는데, 그것이 __init 함수의 주소를
 * 구조체에 남기지 않고 초기화 시점에만 쓰기 위한 장치다.
 * 이 한 줄이 위 상류 주석이 말한 "붙박이여야 한다" 를 코드로 실현한다. */
builtin_platform_driver_probe(ixp4xx_pci_driver, ixp4xx_pci_probe);
