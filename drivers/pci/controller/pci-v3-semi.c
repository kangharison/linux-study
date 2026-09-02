// SPDX-License-Identifier: GPL-2.0
/*
 * Support for V3 Semiconductor PCI Local Bus to PCI Bridge
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 *
 * Based on the code from arch/arm/mach-integrator/pci_v3.c
 * Copyright (C) 1999 ARM Limited
 * Copyright (C) 2000-2001 Deep Blue Solutions Ltd
 *
 * Contributors to the old driver include:
 * Russell King <linux@armlinux.org.uk>
 * David A. Rusling <david.rusling@linaro.org> (uHAL, ARM Firmware suite)
 * Rob Herring <robh@kernel.org>
 * Liviu Dudau <Liviu.Dudau@arm.com>
 * Grant Likely <grant.likely@secretlab.ca>
 * Arnd Bergmann <arnd@arndb.de>
 * Bjorn Helgaas <bhelgaas@google.com>
 */
/*
 * [한국어 설명] V3 Semiconductor 로컬 버스-PCI 브리지 호스트 드라이버 (pci-v3-semi.c)
 *
 * === 파일의 역할 ===
 * V3 Semiconductor 의 V960/V962 계열 브리지 칩(DT compatible "v3,v360epc-pci")을
 * 리눅스 PCI 호스트 컨트롤러로 등록한다. 이 칩은 CPU 쪽 '로컬 버스' 와 PCI 버스
 * 사이에 놓여 양방향 주소 변환 창을 제공하는 물건으로, 1990년대 말 ARM 개발
 * 보드(대표적으로 ARM Integrator/AP)에 쓰였다. 상류 헤더가 밝히듯 이 코드는
 * arch/arm/mach-integrator/pci_v3.c 에 있던 보드 코드를 드라이버로 옮겨 온 것이다.
 * 이 파일에서 하는 일은 셋이다. (1) probe 에서 칩의 잠금을 풀고, 로컬 버스 프로토콜과
 * FIFO 우선순위와 오류 인터럽트를 설정하고, DT 가 준 메모리/IO 창과 DMA 창을
 * 레지스터에 새긴다. (2) 설정공간(config space) 접근을 중계한다. (3) 브리지가
 * 올린 패리티/어보트 오류 인터럽트를 받아 로그로 남긴다.
 * 이 드라이버를 특징짓는 것은 (2)번의 방식이다. 로컬 버스 주소 창이 세 쌍
 * (LB_BASE0/MAP0, LB_BASE1/MAP1, LB_BASE2/MAP2)뿐이라 설정공간 전용 창이 남지
 * 않는다. 그래서 config 접근이 있을 때마다 0번 창을 512MB 로 늘려 1번 창을 가리고,
 * 그 1번 창을 설정공간용으로 다시 매핑해 쓴 뒤, 접근이 끝나면 원래대로 되돌린다.
 * 접근 한 번에 창을 다시 그리는 이 방식이 이 파일 전체 구조의 이유다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 스택의 최하단, 실제 레지스터를 두드리는 호스트 컨트롤러 드라이버 자리다.
 * 부팅 흐름: DT 매칭 → v3_pci_probe() 가 브리지를 할당하고 클럭을 켜고 두 개의
 * MEM 자원(0번 = V3 레지스터, 1번 = 16MB 설정공간 창)을 매핑한 뒤, 오류 IRQ 를
 * 잡고, 칩 잠금을 풀고, PCI 버스를 리셋에 넣은 상태에서 모든 창과 정책을 설정하고,
 * 마지막에 리셋을 풀고 pci_host_probe() 로 코어에 넘긴다.
 * 등록 이후에는 설정공간 접근이 세 함수의 조합으로 처리된다 —
 * v3_map_bus() 가 창을 설정공간용으로 바꾸고 접근할 주소를 돌려주면,
 * pci_generic_config_read()/write() 가 그 주소를 읽거나 쓰고,
 * v3_pci_read_config()/v3_pci_write_config() 가 마지막에 v3_unmap_bus() 로 창을
 * 원래대로 되돌린다. 즉 map/unmap 이 한 번의 config 접근을 감싸는 짝이다.
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트이며 Integrator 초기화에서 msleep(230)
 * 으로 잠들 수 있다. config 접근 경로는 PCI 코어의 pci_lock 아래에서 불리므로
 * 잠들 수 없고, 창을 바꿨다 되돌리는 동안 다른 접근이 끼어들지 못한다 —
 * 이 드라이버의 창 재매핑 기법이 성립하는 근거가 바로 그 락이다.
 * 오류 인터럽트 핸들러 v3_irq() 는 하드 인터럽트 컨텍스트에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. devm_pci_alloc_host_bridge() / pci_host_bridge_priv() /
 * pci_host_bridge_from_priv() / pci_host_probe() 와, 설정공간 접근의 표준 구현인
 * pci_generic_config_read() / pci_generic_config_write() 를 쓴다. 창 목록
 * (bridge->windows)과 인바운드 DMA 범위(bridge->dma_ranges)는 PCI 코어가 DT 의
 * ranges / dma-ranges 를 파싱해 미리 채워 둔 것을 그대로 받아 쓴다.
 * "../pci.h" 는 drivers/pci 내부 전용 헤더다 — 다만 이 파일이 그 헤더에서
 * 무엇을 쓰는지는 아래 include 주석에 적어 둔 대로 코드에서 특정되지 않는다.
 * 아래쪽: 클럭 프레임워크(devm_clk_get, clk_prepare_enable)와, ARM Integrator
 * 보드에서만 쓰이는 syscon/regmap(브리지를 리셋에서 꺼내고 인터럽트를 지우는
 * 보드 레벨 레지스터). 그 syscon 을 제공하는 쪽 코드는 이 트리에 없어 확인 못 함.
 * 데이터 흐름은 양방향이다. 아웃바운드(CPU → PCI)는 DT 의 ranges 가
 * v3_pci_setup_resource() 를 거쳐 LB_BASE/LB_MAP 레지스터가 되고, 인바운드
 * (PCI → 로컬 메모리, 즉 장치의 DMA)는 DT 의 dma-ranges 가
 * v3_get_dma_range_config() 를 거쳐 PCI_BASE/PCI_MAP 레지스터가 된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - v3_map_bus(): config 접근용 주소를 만들고, 그것을 위해 0번 창을 512MB 로
 *   넓혀 1번 창을 가린 뒤 1번 창을 설정공간에 겨눈다. 버스 0 이면 type 0,
 *   그 아래면 type 1 사이클을 만든다.
 * - v3_unmap_bus(): 위의 짝. 1번 창을 prefetchable 메모리로 되돌리고 0번 창을
 *   256MB 로 줄여 1번 창을 다시 드러낸다.
 * - v3_pci_read_config() / v3_pci_write_config(): 표준 접근 함수를 부른 뒤
 *   반드시 unmap 을 부르는 껍데기. 이 짝짓기가 없으면 다음 메모리 접근이 깨진다.
 * - v3_irq(): PCI 상태 레지스터와 로컬 버스 인터럽트 상태를 읽어 오류를 로그로
 *   남기고 지운다.
 * - v3_pci_probe(): 위 전체 초기화 순서를 담은 진입점.
 * - struct v3_pci: 레지스터 창 두 개와 세 가지 창의 시작 주소를 들고 있다.
 *   창을 되돌리려면 원래 값을 기억해야 하므로 이 필드들이 필요하다.
 *
 * === 같은 디렉터리의 다른 컨트롤러와의 대비 ===
 * 같은 폴더의 pcie-apple.c 는 설정공간 접근 코드를 아예 갖지 않는다 — ECAM
 * 규격 덕에 설정공간 전체가 물리 주소에 평평하게 펼쳐져 있어 주소 계산만으로
 * 닿기 때문이다. 반면 이 파일은 접근 한 번마다 주소 창을 다시 그린다. 두
 * 파일을 나란히 두면 '설정공간을 어떻게 CPU 주소 공간에 노출하는가' 라는 문제가
 * 하드웨어 세대에 따라 어떻게 달라졌는지가 그대로 보인다.
 *
 * === 이 트리에서 확인할 수 없는 것 ===
 * 이 저장소는 sparse checkout 이라 drivers/{block,nvme,pci,s390,vfio} 만 있다.
 * 따라서 V3 칩의 데이터시트(트리에 없음), ARM Integrator 보드의 syscon 구현,
 * 클럭/regmap 프레임워크 내부, <linux/...> 헤더의 매크로 정의는 모두 확인 대상
 * 밖이다. 아래 레지스터 설명은 이 파일 안의 사용처와, 이 파일이 표준 PCI 상수
 * (PCI_COMMAND_ 계열)를 같은 레지스터에 함께 쓴다는 사실에서 읽어 낼 수 있는
 * 범위로만 적었다.
 */

/* [한국어] __init 계열 표시와 초기화 관련 정의를 위해 포함한다. 다만 이 파일에
 * __init 표시가 붙은 심볼은 없다 — builtin_platform_driver 로 등록되면서 남은
 * 흔적으로 보이나, 그 경위는 이 트리에서 확인 못 함. */
#include <linux/init.h>
/* [한국어] irqreturn_t 와 IRQ_HANDLED, devm_request_irq() 를 위해 포함한다.
 * 이 브리지는 패리티/어보트 오류를 인터럽트로 알린다. */
#include <linux/interrupt.h>
/* [한국어] readb/readw/readl 과 writeb/writew/writel 을 위해 포함한다. 이 파일의
 * 레지스터 접근은 relaxed 판이 아닌 보통 판이라, 접근마다 메모리 배리어가
 * 들어간다 — 창을 바꾼 뒤 곧바로 그 창으로 접근하는 코드라 순서가 중요하다. */
#include <linux/io.h>
/* [한국어] 범용 커널 매크로(정수 타입, 로그 헬퍼 등)를 위해 포함한다. */
#include <linux/kernel.h>
/* [한국어] of_device_is_compatible() 을 위해 포함한다. Integrator 보드에서만
 * 필요한 초기화를 가려내는 데 쓴다. */
#include <linux/of.h>
/* [한국어] OF 의 PCI 관련 헬퍼를 위해 포함한다. [상류 코드 관찰] 이 파일에서
 * of_pci_ 로 시작하는 함수를 직접 부르는 곳은 없다. */
#include <linux/of_pci.h>
/* [한국어] struct pci_bus, struct pci_ops, PCI_COMMAND_ 계열 상수,
 * pci_generic_config_read()/write() 등 PCI 코어 API 를 위해 포함한다. */
#include <linux/pci.h>
/* [한국어] platform_get_resource(), devm_platform_get_and_ioremap_resource(),
 * builtin_platform_driver() 를 위해 포함한다. 이 컨트롤러는 PCI 장치가 아니라
 * 보드에 붙박이인 칩이라 플랫폼 드라이버로 등록된다. */
#include <linux/platform_device.h>
/* [한국어] 메모리 할당자를 위해 포함한다. [상류 코드 관찰] 이 파일에서 kmalloc
 * 계열을 직접 부르는 곳은 없다 — 할당은 모두 devm_ 판 안에서 일어난다. */
#include <linux/slab.h>
/* [한국어] BIT() 와 GENMASK() 를 위해 포함한다. 아래 레지스터 비트 정의가
 * 대부분 BIT() 로 표현된다. */
#include <linux/bitops.h>
/* [한국어] IRQ 관련 정의를 위해 포함한다. 이 파일은 도메인을 만들지 않고
 * devm_request_irq() 로 오류 IRQ 하나만 잡는다. */
#include <linux/irq.h>
/* [한국어] syscon_regmap_lookup_by_compatible() 을 위해 포함한다. Integrator
 * 보드에서 PCI 브리지를 리셋에서 꺼내는 레지스터가 이 칩이 아니라 보드의
 * 시스템 컨트롤러에 있기 때문에 필요하다. */
#include <linux/mfd/syscon.h>
/* [한국어] regmap_read()/regmap_write() 를 위해 포함한다. 위 syscon 을 통해
 * 보드 레지스터를 읽고 쓰는 통로다. */
#include <linux/regmap.h>
/* [한국어] devm_clk_get() 과 clk_prepare_enable() 을 위해 포함한다. 브리지가
 * 동작하려면 보드가 클럭을 공급해야 한다. */
#include <linux/clk.h>

/* [한국어] drivers/pci 내부 전용 헤더. 꺾쇠가 아니라 따옴표에 상대 경로인 것은
 * 커널 전역 헤더가 아니기 때문이다. [상류 코드 관찰] 이 파일이 이 헤더에서
 * 무엇을 쓰는지는 코드만으로 특정되지 않는다 — 여기서 쓰는 이름들이 대부분
 * <linux/pci.h> 에도 있어서, 어느 쪽에서 온 선언인지 이 트리만으로는 갈라낼 수 없다. */
#include "../pci.h"

/* [한국어] 아래 V3_ 로 시작하는 오프셋 뭉치는 V3 칩의 레지스터 창(v3->base)
 * 안에서의 위치다. 이름 앞머리가 세 갈래인 점이 이 칩의 구조를 그대로 보여 준다 —
 * V3_PCI_ 는 PCI 쪽에서 본 설정과 인바운드(PCI → 로컬 버스) 변환, V3_LB_ 는
 * 로컬 버스 쪽에서 본 아웃바운드(로컬 버스 → PCI) 변환, 나머지는 FIFO·DMA·메일박스
 * 같은 부가 기능이다.
 * 눈여겨볼 점: 앞부분 오프셋들이 PCI 표준 설정공간 헤더의 배치와 그대로 겹친다
 * (0x00 Vendor, 0x02 Device, 0x04 Command, 0x06 Status, 0x08 Class/Revision,
 * 0x10 이후 BAR, 0x2c Subsystem, 0x30 ROM). 이 칩이 PCI 버스에서는 하나의 장치로
 * 보이기 때문이며, 그 해석의 근거는 이 파일 자체에 있다 — 아래 probe 가
 * V3_PCI_CMD 를 표준 상수 PCI_COMMAND_IO / PCI_COMMAND_MEMORY /
 * PCI_COMMAND_MASTER / PCI_COMMAND_INVALIDATE 로 조작하고, v3_irq() 가
 * V3_PCI_STAT 을 표준 Status 비트 배치대로 해석한다.
 * 레지스터 폭은 오프셋 간격과 접근 함수에서 읽힌다 — writel 로 접근하는 것은
 * 32비트, writew 는 16비트, writeb 는 8비트다. 칩 데이터시트는 이 트리에 없다. */
/* [한국어] Vendor ID(표준 헤더 0x00 자리). 이 파일에서 참조하지 않는다. */
#define V3_PCI_VENDOR			0x00000000
/* [한국어] Device ID(표준 헤더 0x02 자리). 참조하지 않는다. */
#define V3_PCI_DEVICE			0x00000002
/* [한국어] Command 레지스터(표준 헤더 0x04 자리, 16비트). probe 가 이 레지스터로
 * IO/메모리 응답과 버스 마스터를 껐다 켠다. */
#define V3_PCI_CMD			0x00000004
/* [한국어] Status 레지스터(표준 헤더 0x06 자리, 16비트). v3_irq() 가 여기서
 * 패리티/시스템/어보트 오류를 읽고, 같은 값을 되써서 지운다(W1C). */
#define V3_PCI_STAT			0x00000006
/* [한국어] Class Code 와 Revision ID(표준 헤더 0x08 자리). 참조하지 않는다. */
#define V3_PCI_CC_REV			0x00000008
/* [한국어] 헤더 설정(캐시 라인/지연 타이머 등, 표준 0x0c 자리). 참조하지 않는다. */
#define V3_PCI_HDR_CFG			0x0000000C
/* [한국어] PCI 쪽에서 본 IO 베이스(표준 BAR0 자리). probe 가 0 을 써서
 * PCI → 호스트 IO 사이클을 막는다. */
#define V3_PCI_IO_BASE			0x00000010
/* [한국어] 인바운드 변환 창 0 의 베이스(표준 BAR1 자리). PCI 마스터가 이 주소
 * 범위에 걸면 로컬 버스로 넘어온다 — 즉 장치 DMA 가 들어오는 문이다. */
#define V3_PCI_BASE0			0x00000014
/* [한국어] 인바운드 변환 창 1 의 베이스(표준 BAR2 자리). 창이 둘뿐이라
 * dma-ranges 항목이 셋 이상이면 나머지는 무시된다. */
#define V3_PCI_BASE1			0x00000018
/* [한국어] Subsystem Vendor ID(표준 0x2c 자리). 참조하지 않는다. */
#define V3_PCI_SUB_VENDOR		0x0000002C
/* [한국어] Subsystem ID(표준 0x2e 자리). 참조하지 않는다. */
#define V3_PCI_SUB_ID			0x0000002E
/* [한국어] 확장 ROM 베이스(표준 0x30 자리). 참조하지 않는다. */
#define V3_PCI_ROM			0x00000030
/* [한국어] 브리지 파라미터(표준 헤더에서는 0x3c 의 Interrupt Line/Pin 자리).
 * 참조하지 않는다. */
#define V3_PCI_BPARAM			0x0000003C
/* [한국어] 인바운드 창 0 의 변환 규칙 — 어느 로컬 버스 주소로 옮길지와 크기,
 * 활성화 비트를 담는다. BASE0 와 짝이다. */
#define V3_PCI_MAP0			0x00000040
/* [한국어] 인바운드 창 1 의 변환 규칙. BASE1 과 짝이다. */
#define V3_PCI_MAP1			0x00000044
/* [한국어] PCI 쪽 인터럽트 상태. 참조하지 않는다. */
#define V3_PCI_INT_STAT			0x00000048
/* [한국어] PCI 쪽 인터럽트 설정. 참조하지 않는다. */
#define V3_PCI_INT_CFG			0x0000004C
/* [한국어] 아웃바운드 창 0 의 로컬 버스 베이스(32비트). 이 드라이버에서 이 창은
 * 비prefetchable 메모리를 담당하며, config 접근 동안에는 512MB 로 넓혀
 * 1번 창을 가리는 용도로도 쓰인다. */
#define V3_LB_BASE0			0x00000054
/* [한국어] 아웃바운드 창 1 의 로컬 버스 베이스(32비트). 평소에는 prefetchable
 * 메모리를 담당하고, config 접근 동안만 설정공간용으로 빌려 쓴다. */
#define V3_LB_BASE1			0x00000058
/* [한국어] 아웃바운드 창 0 의 PCI 쪽 목적지와 사이클 종류(16비트). 오프셋이
 * 0x5e 로 4바이트 경계에 맞지 않는 것은 이 레지스터가 16비트이기 때문이다. */
#define V3_LB_MAP0			0x0000005E
/* [한국어] 아웃바운드 창 1 의 목적지와 사이클 종류(16비트). config 접근 때는
 * 여기에 '설정 사이클' 종류를 써 넣는 것이 핵심 동작이다. */
#define V3_LB_MAP1			0x00000062
/* [한국어] 아웃바운드 창 2 의 로컬 버스 베이스(32비트). IO 공간 전용으로 쓴다. */
#define V3_LB_BASE2			0x00000064
/* [한국어] 아웃바운드 창 2 의 목적지(16비트). */
#define V3_LB_MAP2			0x00000066
/* [한국어] 창 크기 관련 레지스터. 참조하지 않는다 — 크기는 각 BASE 레지스터의
 * 크기 필드로 지정한다. */
#define V3_LB_SIZE			0x00000068
/* [한국어] V3 칩 자신이 로컬 버스에서 차지하는 물리 주소의 상위 16비트.
 * probe 가 이 값을 읽어 자기가 매핑한 주소와 맞는지 확인하고, Integrator
 * 초기화에서는 리셋 직후 이 값을 직접 써 넣는다. */
#define V3_LB_IO_BASE			0x0000006E
/* [한국어] FIFO 설정. 참조하지 않는다. */
#define V3_FIFO_CFG			0x00000070
/* [한국어] FIFO 우선순위. probe 가 쓰기를 읽기보다 우선하고 특정 조건에서
 * 읽기 FIFO 를 비우도록 설정한다. */
#define V3_FIFO_PRIORITY		0x00000072
/* [한국어] FIFO 상태. 참조하지 않는다. */
#define V3_FIFO_STAT			0x00000074
/* [한국어] 로컬 버스 인터럽트 상태(8비트). v3_irq() 가 읽어 원인을 로그로 남기고
 * 0 을 써서 지운다. */
#define V3_LB_ISTAT			0x00000076
/* [한국어] 로컬 버스 인터럽트 마스크(8비트). 1 이면 그 원인을 인터럽트로 올린다
 * — probe 가 두 단계에 걸쳐 허용 비트를 넓혀 가는 것으로 알 수 있다. */
#define V3_LB_IMASK			0x00000077
/* [한국어] 시스템 제어 레지스터(16비트). 레지스터 잠금과 PCI 버스 리셋 신호를
 * 담는다. probe 의 처음과 끝이 이 레지스터를 만진다. */
#define V3_SYSTEM			0x00000078
/* [한국어] 로컬 버스 프로토콜 설정(16비트). 바이트 인에이블 방향, 엔디언,
 * 타임아웃, 인터럽트 허용 등이 여기 모여 있다. */
#define V3_LB_CFG			0x0000007A
/* [한국어] PCI 쪽 동작 설정(16비트). IO 응답 금지, 3.3V 버퍼, 재시도 허용,
 * DMA 명령 타입 등을 담는다. */
#define V3_PCI_CFG			0x0000007C
/* [한국어] DMA 채널 0 의 PCI 주소. 참조하지 않는다 — 이 드라이버는 칩 내장 DMA
 * 엔진을 쓰지 않는다. */
#define V3_DMA_PCI_ADR0			0x00000080
/* [한국어] DMA 채널 1 의 PCI 주소. 참조하지 않는다. */
#define V3_DMA_PCI_ADR1			0x00000090
/* [한국어] DMA 채널 0 의 로컬 버스 주소. 참조하지 않는다. */
#define V3_DMA_LOCAL_ADR0		0x00000084
/* [한국어] DMA 채널 1 의 로컬 버스 주소. 참조하지 않는다. */
#define V3_DMA_LOCAL_ADR1		0x00000094
/* [한국어] DMA 채널 0 의 전송 길이. 참조하지 않는다. */
#define V3_DMA_LENGTH0			0x00000088
/* [한국어] DMA 채널 1 의 전송 길이. 참조하지 않는다. */
#define V3_DMA_LENGTH1			0x00000098
/* [한국어] DMA 채널 0 의 제어/상태. 참조하지 않는다. */
#define V3_DMA_CSR0			0x0000008B
/* [한국어] DMA 채널 1 의 제어/상태. 참조하지 않는다. */
#define V3_DMA_CSR1			0x0000009B
/* [한국어] DMA 채널 0 의 체인 제어 블록 주소. 참조하지 않는다. */
#define V3_DMA_CTLB_ADR0		0x0000008C
/* [한국어] DMA 채널 1 의 체인 제어 블록 주소. 참조하지 않는다. */
#define V3_DMA_CTLB_ADR1		0x0000009C
/* [한국어] DMA 지연 설정. 참조하지 않는다. */
#define V3_DMA_DELAY			0x000000E0
/* [한국어] 메일박스 데이터 영역. PCI 쪽과 로컬 버스 쪽이 서로 값을 주고받는
 * 창구다. Integrator 초기화에서 리셋 직후 칩이 응답하는지 확인하는 데 쓴다. */
#define V3_MAIL_DATA			0x000000C0
/* [한국어] PCI 쪽 메일박스 쓰기 인터럽트 허용. 참조하지 않는다. */
#define V3_PCI_MAIL_IEWR		0x000000D0
/* [한국어] PCI 쪽 메일박스 읽기 인터럽트 허용. 참조하지 않는다. */
#define V3_PCI_MAIL_IERD		0x000000D2
/* [한국어] 로컬 버스 쪽 메일박스 쓰기 인터럽트 허용. 참조하지 않는다. */
#define V3_LB_MAIL_IEWR			0x000000D4
/* [한국어] 로컬 버스 쪽 메일박스 읽기 인터럽트 허용. 참조하지 않는다. */
#define V3_LB_MAIL_IERD			0x000000D6
/* [한국어] 메일박스 쓰기 상태. 참조하지 않는다. */
#define V3_MAIL_WR_STAT			0x000000D8
/* [한국어] 메일박스 읽기 상태. 참조하지 않는다. */
#define V3_MAIL_RD_STAT			0x000000DA
/* [한국어] I2O 큐 베이스 주소 매핑. 참조하지 않는다 — 위 상류 주석이 말하듯
 * 이 드라이버는 I2O 매핑을 설정하지 않는다. */
#define V3_QBA_MAP			0x000000DC

/* PCI STATUS bits */
/* [한국어] 비트 15: 패리티 오류를 감지했다. PCI 표준 Status 레지스터의
 * Detected Parity Error 와 같은 자리다. */
#define V3_PCI_STAT_PAR_ERR		BIT(15)
/* [한국어] 비트 14: 시스템 오류(SERR#)를 냈다. 표준의 Signaled System Error 자리. */
#define V3_PCI_STAT_SYS_ERR		BIT(14)
/* [한국어] 비트 13: 마스터 어보트를 받았다 — 응답하는 장치가 없었다는 뜻이다.
 * 표준의 Received Master Abort 자리. */
#define V3_PCI_STAT_M_ABORT_ERR		BIT(13)
/* [한국어] 비트 12: 타깃 어보트를 받았다 — 상대가 거래를 중단시켰다는 뜻이다.
 * 표준의 Received Target Abort 자리. */
#define V3_PCI_STAT_T_ABORT_ERR		BIT(12)

/* LB ISTAT bits */
/* [한국어] 비트 7: 메일박스 인터럽트. 정보성이라 dev_info 로만 남긴다. */
#define V3_LB_ISTAT_MAILBOX		BIT(7)
/* [한국어] 비트 6: 로컬 버스 → PCI 읽기가 어보트되었다. */
#define V3_LB_ISTAT_PCI_RD		BIT(6)
/* [한국어] 비트 5: 로컬 버스 → PCI 쓰기가 어보트되었다. */
#define V3_LB_ISTAT_PCI_WR		BIT(5)
/* [한국어] 비트 4: PCI 핀 인터럽트(INTx)가 들어왔다. */
#define V3_LB_ISTAT_PCI_INT		BIT(4)
/* [한국어] 비트 3: PCI 패리티 오류. */
#define V3_LB_ISTAT_PCI_PERR		BIT(3)
/* [한국어] 비트 2: I2O 인바운드 포스트 큐 인터럽트. */
#define V3_LB_ISTAT_I2O_QWR		BIT(2)
/* [한국어] 비트 1: 내장 DMA 채널 1 완료. */
#define V3_LB_ISTAT_DMA1		BIT(1)
/* [한국어] 비트 0: 내장 DMA 채널 0 완료. */
#define V3_LB_ISTAT_DMA0		BIT(0)

/* PCI COMMAND bits */
/* [한국어] 아래 여섯 개는 V3_PCI_CMD 의 비트 정의이며 PCI 표준 Command 레지스터와
 * 같은 배치다. [상류 코드 관찰] 여섯 개 모두 이 파일에서 참조하지 않는다 —
 * 코드가 같은 자리를 표준 상수 PCI_COMMAND_ 계열로 조작하기 때문이다.
 * 즉 이 뭉치는 표준 정의와 중복된 채 남아 있는 셈이다.
 * 비트 9: fast back-to-back 전송 허용. */
#define V3_COMMAND_M_FBB_EN		BIT(9)
/* [한국어] 비트 8: SERR# 를 낼 수 있게 허용. 참조하지 않는다. */
#define V3_COMMAND_M_SERR_EN		BIT(8)
/* [한국어] 비트 6: 패리티 오류에 응답하도록 허용. 참조하지 않는다. */
#define V3_COMMAND_M_PAR_EN		BIT(6)
/* [한국어] 비트 2: 버스 마스터로 동작. 코드는 PCI_COMMAND_MASTER 를 쓴다. */
#define V3_COMMAND_M_MASTER_EN		BIT(2)
/* [한국어] 비트 1: 메모리 공간 응답. 코드는 PCI_COMMAND_MEMORY 를 쓴다. */
#define V3_COMMAND_M_MEM_EN		BIT(1)
/* [한국어] 비트 0: IO 공간 응답. 코드는 PCI_COMMAND_IO 를 쓴다. */
#define V3_COMMAND_M_IO_EN		BIT(0)

/* SYSTEM bits */
/* [한국어] 비트 15: PCI 버스 리셋 신호(RST#)의 상태. 0 이 리셋을 거는 것이고
 * 1 이 푸는 것이다 — probe 가 창을 설정하는 동안 0 으로 두었다가 마지막에 1 로 올린다. */
#define V3_SYSTEM_M_RST_OUT		BIT(15)
/* [한국어] 비트 14: 레지스터 잠금 상태. 1 이면 잠겨 있어 설정을 바꿀 수 없다.
 * probe 가 시작할 때 풀고 끝날 때 다시 건다 — 실수로 창이 바뀌는 것을 막는 장치다. */
#define V3_SYSTEM_M_LOCK		BIT(14)
/* [한국어] 잠금을 푸는 매직 값. 이 정확한 값을 V3_SYSTEM 에 써야만 잠금이 풀린다.
 * 값의 유래는 칩 데이터시트에 있을 텐데 그 문서는 이 트리에 없다. */
#define V3_SYSTEM_UNLOCK		0xa05f

/* PCI CFG bits */
/* [한국어] 비트 15: I2O 모드 활성화. 참조하지 않는다 — 위 상류 주석대로 I2O 를
 * 켜면 PCI 메모리 매핑 대부분이 무효가 되기 때문이다. */
#define V3_PCI_CFG_M_I2O_EN		BIT(15)
/* [한국어] 비트 14: PCI 쪽에서 이 칩의 레지스터를 IO 사이클로 보는 것을 막는다. */
#define V3_PCI_CFG_M_IO_REG_DIS		BIT(14)
/* [한국어] 비트 13: PCI → 호스트 IO 사이클 자체를 막는다. */
#define V3_PCI_CFG_M_IO_DIS		BIT(13)
/* [한국어] 비트 12: IO 버퍼를 3.3V 로 동작시킨다. */
#define V3_PCI_CFG_M_EN3V		BIT(12)
/* [한국어] 비트 10: 타깃이 준비되지 않았을 때 재시도를 허용한다. probe 가 창을
 * 세우기 전에 미리 켜 둔다. */
#define V3_PCI_CFG_M_RETRY_EN		BIT(10)
/* [한국어] 비트 9: 주소의 하위 비트 1 을 대신 지정하는 값. 참조하지 않는다. */
#define V3_PCI_CFG_M_AD_LOW1		BIT(9)
/* [한국어] 비트 8: 주소의 하위 비트 0 을 대신 지정하는 값. probe 가 켜 두지만
 * 옆의 상류 주석이 "쓰이지 않을 것" 이라고 적어 두었다. */
#define V3_PCI_CFG_M_AD_LOW0		BIT(8)
/*
 * This is the value applied to C/BE[3:1], with bit 0 always held 0
 * during DMA access.
 */
/* [한국어] 옆의 상류 주석대로, 아래 두 시프트는 DMA 접근 때 C/BE[3:1] 에 실릴
 * 명령 타입의 자리다(비트 0 은 항상 0 으로 고정된다). 읽기용 타입이 놓이는 자리. */
#define V3_PCI_CFG_M_RTYPE_SHIFT	5
/* [한국어] 쓰기용 명령 타입이 놓이는 자리. */
#define V3_PCI_CFG_M_WTYPE_SHIFT	1
/* [한국어] 그 자리에 넣을 기본 명령 타입 값. probe 가 읽기와 쓰기 양쪽에 이 값을
 * 넣는다. 값 3 이 어떤 PCI 명령을 뜻하는지는 데이터시트가 필요해 확인 못 함. */
#define V3_PCI_CFG_TYPE_DEFAULT		0x3

/* PCI BASE bits (PCI -> Local Bus) */
/* [한국어] 인바운드 창 베이스에서 주소 상위 12비트(31..20)가 놓이는 자리.
 * 창의 최소 단위가 1MB 라는 뜻이기도 하다 — 그래서 아래 검증 코드가
 * '31..20 비트만 허용' 이라고 말한다. */
#define V3_PCI_BASE_M_ADR_BASE		0xFFF00000U
/* [한국어] 그 아래 자리(19..8)의 마스크. 참조하지 않는다. */
#define V3_PCI_BASE_M_ADR_BASEL		0x000FFF00U
/* [한국어] 이 창을 prefetchable 로 표시하는 비트. 참조하지 않는다. */
#define V3_PCI_BASE_M_PREFETCH		BIT(3)
/* [한국어] 창의 타입 필드(비트 2..1). 참조하지 않는다. */
#define V3_PCI_BASE_M_TYPE		(3 << 1)
/* [한국어] 이 창이 메모리가 아니라 IO 임을 뜻하는 비트. 참조하지 않는다. */
#define V3_PCI_BASE_M_IO		BIT(0)

/* PCI MAP bits (PCI -> Local bus) */
/* [한국어] 인바운드 변환 결과의 로컬 버스 주소 상위 12비트(31..20) 자리.
 * BASE 쪽과 같은 폭이라, PCI 주소와 로컬 주소가 1MB 단위로 대응된다. */
#define V3_PCI_MAP_M_MAP_ADR		0xFFF00000U
/* [한국어] 비트 15: 읽기 포스팅을 금지한다. 참조하지 않는다. */
#define V3_PCI_MAP_M_RD_POST_INH	BIT(15)
/* [한국어] ROM 창 크기 필드(비트 11..10). 참조하지 않는다. */
#define V3_PCI_MAP_M_ROM_SIZE		(3 << 10)
/* [한국어] 바이트 스왑 방식 필드(비트 9..8). 참조하지 않는다. */
#define V3_PCI_MAP_M_SWAP		(3 << 8)
/* [한국어] 창 크기 필드(비트 7..4). [상류 코드 관찰] 이 상수는 참조되지 않는다.
 * 코드는 같은 자리에 아래 V3_LB_BASE_ADR_SIZE_ 계열 상수를 넣는데, 그쪽 값이
 * (n << 4) 라 비트 자리가 정확히 일치하기 때문에 결과는 맞는다.
 * 즉 이름은 로컬 버스 쪽 것을 빌려 쓰고 있다. */
#define V3_PCI_MAP_M_ADR_SIZE		0x000000F0U
/* [한국어] 비트 1: 이 창으로 칩 레지스터에 접근하는 것을 허용한다.
 * DMA 창 설정에서 활성화 비트와 함께 세운다. */
#define V3_PCI_MAP_M_REG_EN		BIT(1)
/* [한국어] 비트 0: 이 인바운드 창을 활성화한다. */
#define V3_PCI_MAP_M_ENABLE		BIT(0)

/* LB_BASE0,1 bits (Local bus -> PCI) */
/* [한국어] 아웃바운드 창 베이스의 주소 상위 12비트(31..20) 자리. 이것도 창의
 * 정렬 단위가 1MB 임을 뜻한다. */
#define V3_LB_BASE_ADR_BASE		0xfff00000U
/* [한국어] 바이트 스왑 방식 필드(비트 9..8). 참조하지 않는다. */
#define V3_LB_BASE_SWAP			(3 << 8)
/* [한국어] 창 크기 필드(비트 7..4)의 마스크. 참조하지 않는다 — 코드는 아래의
 * 크기별 상수를 직접 쓴다. */
#define V3_LB_BASE_ADR_SIZE		(15 << 4)
/* [한국어] 비트 3: 이 창을 prefetchable 로 표시한다. prefetchable 메모리 창
 * (창 1)에 세운다. */
#define V3_LB_BASE_PREFETCH		BIT(3)
/* [한국어] 비트 0: 이 아웃바운드 창을 활성화한다. */
#define V3_LB_BASE_ENABLE		BIT(0)

/* [한국어] 아래 열두 개는 창 크기를 나타내는 값이다. 크기가 두 배가 될 때마다
 * 값이 1 씩 커지는 지수 인코딩이며, 모두 (n << 4) 라 비트 7..4 에 놓인다.
 * 창 크기 1MB. */
#define V3_LB_BASE_ADR_SIZE_1MB		(0 << 4)
/* [한국어] 창 크기 2MB. */
#define V3_LB_BASE_ADR_SIZE_2MB		(1 << 4)
/* [한국어] 창 크기 4MB. */
#define V3_LB_BASE_ADR_SIZE_4MB		(2 << 4)
/* [한국어] 창 크기 8MB. */
#define V3_LB_BASE_ADR_SIZE_8MB		(3 << 4)
/* [한국어] 창 크기 16MB. 설정공간 창이 이 크기다. */
#define V3_LB_BASE_ADR_SIZE_16MB	(4 << 4)
/* [한국어] 창 크기 32MB. */
#define V3_LB_BASE_ADR_SIZE_32MB	(5 << 4)
/* [한국어] 창 크기 64MB. */
#define V3_LB_BASE_ADR_SIZE_64MB	(6 << 4)
/* [한국어] 창 크기 128MB. */
#define V3_LB_BASE_ADR_SIZE_128MB	(7 << 4)
/* [한국어] 창 크기 256MB. 이 드라이버가 요구하는 메모리 창 크기다. */
#define V3_LB_BASE_ADR_SIZE_256MB	(8 << 4)
/* [한국어] 창 크기 512MB. config 접근 동안 창 0 을 이 크기로 늘려 창 1 을 가린다. */
#define V3_LB_BASE_ADR_SIZE_512MB	(9 << 4)
/* [한국어] 창 크기 1GB. */
#define V3_LB_BASE_ADR_SIZE_1GB		(10 << 4)
/* [한국어] 창 크기 2GB. */
#define V3_LB_BASE_ADR_SIZE_2GB		(11 << 4)

/* [한국어] 로컬 버스 물리 주소에서 창 베이스 레지스터에 넣을 부분만 남긴다.
 * 하위 20비트를 버리는 것이라, 창 시작 주소는 1MB 에 정렬되어 있어야 한다. */
#define v3_addr_to_lb_base(a)	((a) & V3_LB_BASE_ADR_BASE)

/* LB_MAP0,1 bits (Local bus -> PCI) */
/* [한국어] 아웃바운드 창의 목적지 PCI 주소 상위 12비트가 놓이는 자리.
 * 이 레지스터가 16비트이므로 마스크도 16비트 폭이다. */
#define V3_LB_MAP_MAP_ADR		0xfff0U
/* [한국어] 사이클 종류 필드(비트 3..1). 참조하지 않는다 — 코드는 아래의
 * 종류별 상수를 직접 쓴다. */
#define V3_LB_MAP_TYPE			(7 << 1)
/* [한국어] 비트 0: PCI 주소의 하위 두 비트(A1, A0)를 로컬 버스 주소에서
 * 가져오게 한다. type 1 설정 사이클에서 켜야 하는 비트다. */
#define V3_LB_MAP_AD_LOW_EN		BIT(0)

/* [한국어] 인터럽트 확인(IACK) 사이클. 참조하지 않는다. */
#define V3_LB_MAP_TYPE_IACK		(0 << 1)
/* [한국어] IO 사이클. 참조하지 않는다 — IO 창은 창 2 를 쓰고 그쪽 레지스터에는
 * 종류 필드가 없다. */
#define V3_LB_MAP_TYPE_IO		(1 << 1)
/* [한국어] 메모리 사이클. 메모리 창 두 개가 모두 이 종류다. */
#define V3_LB_MAP_TYPE_MEM		(3 << 1)
/* [한국어] 설정(config) 사이클. v3_map_bus() 가 창 1 에 이 종류를 써서
 * 설정공간 접근을 만들어 낸다 — 이 드라이버의 핵심 동작이다. */
#define V3_LB_MAP_TYPE_CONFIG		(5 << 1)
/* [한국어] 다중 메모리 읽기 사이클. 참조하지 않는다. 다만 코드의 두 곳에
 * "was V3_LB_MAP_TYPE_MEM_MULTIPLE" 이라는 상류 주석이 남아 있어, 예전에는
 * 이 종류를 썼다가 보통 메모리 사이클로 바꾼 흔적임을 알 수 있다. */
#define V3_LB_MAP_TYPE_MEM_MULTIPLE	(6 << 1)

/* [한국어] PCI 버스 주소를 창 목적지 레지스터에 넣을 형태로 바꾼다. 16비트
 * 오른쪽 시프트 뒤 상위 12비트만 남기므로, 결국 주소의 31..20 비트가
 * 레지스터의 15..4 자리에 놓인다. */
#define v3_addr_to_lb_map(a)	(((a) >> 16) & V3_LB_MAP_MAP_ADR)

/* LB_BASE2 bits (Local bus -> PCI IO) */
/* [한국어] IO 전용 창(창 2)의 베이스 자리(비트 15..8). 위 두 창보다 필드가
 * 좁아 정렬 단위가 16MB 다. */
#define V3_LB_BASE2_ADR_BASE		0xff00U
/* [한국어] 자동 바이트 스왑 설정. 참조하지 않는다. */
#define V3_LB_BASE2_SWAP_AUTO		(3 << 6)
/* [한국어] 비트 0: IO 창을 활성화한다. */
#define V3_LB_BASE2_ENABLE		BIT(0)

/* [한국어] 로컬 버스 IO 창 시작 주소를 창 2 베이스 레지스터 형태로 바꾼다.
 * 16비트 시프트 뒤 상위 8비트만 남으므로 정렬 단위가 16MB 다. */
#define v3_addr_to_lb_base2(a)	(((a) >> 16) & V3_LB_BASE2_ADR_BASE)

/* LB_MAP2 bits (Local bus -> PCI IO) */
/* [한국어] IO 창의 목적지 PCI IO 주소 자리(비트 15..8). */
#define V3_LB_MAP2_MAP_ADR		0xff00U

/* [한국어] PCI IO 주소를 창 2 목적지 레지스터 형태로 바꾼다. */
#define v3_addr_to_lb_map2(a)	(((a) >> 16) & V3_LB_MAP2_MAP_ADR)

/* FIFO priority bits */
/* [한국어] 비트 12: 로컬 버스 쪽 FIFO 에 우선권을 준다. 참조하지 않는다. */
#define V3_FIFO_PRIO_LOCAL		BIT(12)
/* [한국어] 로컬 버스 읽기 FIFO 1 을 버스트 끝(EOB)에서 비운다. 참조하지 않는다. */
#define V3_FIFO_PRIO_LB_RD1_FLUSH_EOB	BIT(10)
/* [한국어] 같은 FIFO 를 aperture 1 접근에서 비운다. probe 가 이 방식을 고른다. */
#define V3_FIFO_PRIO_LB_RD1_FLUSH_AP1	BIT(11)
/* [한국어] 위 두 조건을 모두 적용. 참조하지 않는다. */
#define V3_FIFO_PRIO_LB_RD1_FLUSH_ANY	(BIT(10)|BIT(11))
/* [한국어] 로컬 버스 읽기 FIFO 0 을 버스트 끝에서 비운다. 참조하지 않는다. */
#define V3_FIFO_PRIO_LB_RD0_FLUSH_EOB	BIT(8)
/* [한국어] 같은 FIFO 를 aperture 1 접근에서 비운다. probe 가 고르는 값. */
#define V3_FIFO_PRIO_LB_RD0_FLUSH_AP1	BIT(9)
/* [한국어] 위 두 조건 모두. 참조하지 않는다. */
#define V3_FIFO_PRIO_LB_RD0_FLUSH_ANY	(BIT(8)|BIT(9))
/* [한국어] 비트 4: PCI 쪽 FIFO 에 우선권. 참조하지 않는다. */
#define V3_FIFO_PRIO_PCI		BIT(4)
/* [한국어] PCI 읽기 FIFO 1 을 버스트 끝에서 비운다. 참조하지 않는다. */
#define V3_FIFO_PRIO_PCI_RD1_FLUSH_EOB	BIT(2)
/* [한국어] 같은 FIFO 를 aperture 1 접근에서 비운다. probe 가 고르는 값. */
#define V3_FIFO_PRIO_PCI_RD1_FLUSH_AP1	BIT(3)
/* [한국어] 위 두 조건 모두. 참조하지 않는다. */
#define V3_FIFO_PRIO_PCI_RD1_FLUSH_ANY	(BIT(2)|BIT(3))
/* [한국어] PCI 읽기 FIFO 0 을 버스트 끝에서 비운다. 참조하지 않는다. */
#define V3_FIFO_PRIO_PCI_RD0_FLUSH_EOB	BIT(0)
/* [한국어] 같은 FIFO 를 aperture 1 접근에서 비운다. probe 가 고르는 값. */
#define V3_FIFO_PRIO_PCI_RD0_FLUSH_AP1	BIT(1)
/* [한국어] 위 두 조건 모두. 참조하지 않는다. */
#define V3_FIFO_PRIO_PCI_RD0_FLUSH_ANY	(BIT(0)|BIT(1))

/* Local bus configuration bits */
/* [한국어] 로컬 버스 타임아웃 64 사이클(값 0). 참조하지 않는다. */
#define V3_LB_CFG_LB_TO_64_CYCLES	0x0000
/* [한국어] 타임아웃 256 사이클. 참조하지 않는다. */
#define V3_LB_CFG_LB_TO_256_CYCLES	BIT(13)
/* [한국어] 타임아웃 512 사이클. 참조하지 않는다. */
#define V3_LB_CFG_LB_TO_512_CYCLES	BIT(14)
/* [한국어] 타임아웃 1024 사이클. 참조하지 않는다. */
#define V3_LB_CFG_LB_TO_1024_CYCLES	(BIT(13)|BIT(14))
/* [한국어] 비트 12: 로컬 버스 리셋. 참조하지 않는다. */
#define V3_LB_CFG_LB_RST		BIT(12)
/* [한국어] 비트 11: PPC403Gx 계열용 준비 신호 방식. probe 가 이 비트를 지우고,
 * 옆에 "PPC403Gx 에서는 1 로" 라는 상류 TODO 주석이 붙어 있다. */
#define V3_LB_CFG_LB_PPC_RDY		BIT(11)
/* [한국어] 비트 10: 로컬 버스 인터럽트를 허용한다. probe 가 오류 인터럽트를
 * 켜기 위해 세운다. */
#define V3_LB_CFG_LB_LB_INT		BIT(10)
/* [한국어] 비트 9: 오류 신호 허용. 참조하지 않는다. */
#define V3_LB_CFG_LB_ERR_EN		BIT(9)
/* [한국어] 비트 8: 준비 신호 허용. 참조하지 않는다. */
#define V3_LB_CFG_LB_RDY_EN		BIT(8)
/* [한국어] 비트 7: 바이트 인에이블 입력 모드. probe 가 세운다. */
#define V3_LB_CFG_LB_BE_IMODE		BIT(7)
/* [한국어] 비트 6: 바이트 인에이블 출력 모드. probe 가 세운다. */
#define V3_LB_CFG_LB_BE_OMODE		BIT(6)
/* [한국어] 비트 5: 엔디언 선택. probe 가 지워 리틀 엔디언으로 둔다 — ARM 쪽이
 * 리틀 엔디언이고 PCI 도 리틀 엔디언이라 변환이 필요 없기 때문이다. */
#define V3_LB_CFG_LB_ENDIAN		BIT(5)
/* [한국어] 비트 4: 버스 파킹 허용. 참조하지 않는다. */
#define V3_LB_CFG_LB_PARK_EN		BIT(4)
/* [한국어] 비트 2: fast back-to-back 금지. 참조하지 않는다. */
#define V3_LB_CFG_LB_FBB_DIS		BIT(2)

/* ARM Integrator-specific extended control registers */
/* [한국어] Integrator/AP 보드의 시스템 컨트롤러 안에서 PCI 제어 레지스터가
 * 놓인 오프셋. 이 레지스터는 V3 칩이 아니라 보드에 있으므로 syscon/regmap 으로
 * 접근한다. */
#define INTEGRATOR_SC_PCI_OFFSET	0x18
/* [한국어] 비트 0: PCI 브리지를 리셋에서 꺼내 동작시킨다. 이 비트가 꺼져 있었다면
 * 방금 리셋에서 나온 것이므로 초기화 코드가 추가 대기를 한다. */
#define INTEGRATOR_SC_PCI_ENABLE	BIT(0)
/* [한국어] 비트 1: 보드 쪽에 걸린 PCI 인터럽트를 지운다. 오류 인터럽트 핸들러가
 * 마지막에 이 비트를 함께 써서 보드 쪽 래치도 지운다. */
#define INTEGRATOR_SC_PCI_INTCLR	BIT(1)
/* [한국어] 실패한 접근의 주소를 담는 보드 레지스터의 오프셋. 참조하지 않는다. */
#define INTEGRATOR_SC_LBFADDR_OFFSET	0x20
/* [한국어] 실패한 접근의 코드(원인)를 담는 보드 레지스터의 오프셋. 참조하지 않는다. */
#define INTEGRATOR_SC_LBFCODE_OFFSET	0x24

/* [한국어]
 * struct v3_pci - V3 브리지 컨트롤러 하나의 상태
 *
 * 호스트 브리지의 사설 데이터 영역에 놓인다 —
 * devm_pci_alloc_host_bridge(dev, sizeof(*v3)) 로 브리지 뒤에 함께 할당되므로,
 * 브리지에서 이쪽으로는 pci_host_bridge_priv(), 반대로는
 * pci_host_bridge_from_priv() 로 오갈 수 있다. 수명은 브리지와 같다.
 *
 * 이 구조체가 세 창의 시작 주소를 굳이 들고 있는 이유가 이 드라이버의 성격을
 * 그대로 보여 준다. config 접근마다 창 0 과 창 1 을 임시로 다시 그렸다가
 * 되돌려야 하는데, 되돌리려면 원래 값을 어딘가 기억해 두어야 하기 때문이다.
 * 그 기억 장소가 non_pre_mem / pre_mem / pre_bus_addr 이다.
 */
struct v3_pci {
	/* [한국어] 플랫폼 디바이스의 struct device. 로그와 devm 할당의 기준이다.
	 * 설정자: v3_pci_probe().
	 * 읽는 자: v3_irq(), v3_integrator_init(), 자원 설정 함수들의 dev_err/dev_dbg.
	 * 값 범위: 항상 유효(NULL 불가).
	 * 동기화: 설정 후 읽기 전용. */
	struct device *dev;
	/* [한국어] V3 칩 레지스터 창(DT reg 인덱스 0)의 커널 가상 주소.
	 * 설정자: v3_pci_probe() 의 devm_platform_get_and_ioremap_resource(pdev, 0).
	 * 읽는 자: 이 파일의 모든 V3_ 오프셋 접근.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: config 경로의 접근은 PCI 코어의 pci_lock 아래에서 직렬화되고,
	 *   probe 경로는 단독 실행이다. 다만 오류 인터럽트 핸들러 v3_irq() 도 같은 창을
	 *   만지는데 그쪽과의 배타를 보장하는 락은 이 파일에 없다 — 다루는 레지스터가
	 *   서로 달라 실질적 충돌이 없는 구조로 보이나, 그 의도를 밝힌 근거는 코드에 없다. */
	void __iomem *base;
	/* [한국어] 16MB 설정공간 창(DT reg 인덱스 1)의 커널 가상 주소.
	 * 설정자: v3_pci_probe() 의 devm_ioremap_resource().
	 * 읽는 자: v3_map_bus() 가 여기에 계산한 오프셋을 더해 접근 주소를 만든다.
	 * 값 범위: 유효한 __iomem 포인터. 창 크기가 정확히 16MB 가 아니면 probe 가 실패한다.
	 * 동기화: 위 base 와 같다 — pci_lock 이 직렬화한다. */
	void __iomem *config_base;
	/* [한국어] 같은 설정공간 창의 물리(로컬 버스) 시작 주소.
	 * 설정자: v3_pci_probe() 이 자원의 start 를 그대로 담는다.
	 * 읽는 자: v3_map_bus() 가 창 1 의 베이스 레지스터에 쓸 값을 만들 때.
	 * 값 범위: 1MB 정렬된 32비트 물리 주소(v3_addr_to_lb_base 가 하위 20비트를 버린다).
	 * 동기화: probe 이후 읽기 전용.
	 * 가상 주소와 물리 주소를 둘 다 들고 있는 이유: 접근에는 가상 주소가 필요하고,
	 * 창 레지스터에 새길 때는 물리 주소가 필요하기 때문이다. */
	u32 config_mem;
	/* [한국어] 비prefetchable 메모리 창의 로컬 버스 시작 주소.
	 * 설정자: v3_pci_setup_resource() 가 DT 의 ranges 를 훑다가 prefetch 플래그가
	 *   없는 메모리 창을 만나면 담는다.
	 * 읽는 자: v3_map_bus() 와 v3_unmap_bus() 가 창 0 을 512MB 로 늘렸다가 256MB 로
	 *   되돌릴 때, 그리고 prefetchable 창이 바로 뒤에 이어 붙는지 검사할 때.
	 * 값 범위: 1MB 정렬된 물리 주소. 0 이면 아직 설정되지 않았다는 뜻으로도 쓰인다
	 *   — 인접성 검사가 이 값이 0 인지로 '아직 안 봤음' 을 판정한다.
	 * 동기화: probe 이후 읽기 전용. */
	u32 non_pre_mem;
	/* [한국어] prefetchable 메모리 창의 로컬 버스 시작 주소.
	 * 설정자: v3_pci_setup_resource() 의 prefetch 분기.
	 * 읽는 자: v3_unmap_bus() 가 창 1 을 원래 용도로 되돌릴 때.
	 * 값 범위: 1MB 정렬된 물리 주소. 반드시 non_pre_mem + 256MB 여야 한다
	 *   — 그렇지 않으면 창 0 을 512MB 로 늘리는 기법이 성립하지 않으므로 probe 가 거절한다.
	 * 동기화: probe 이후 읽기 전용. */
	u32 pre_mem;
	/* [한국어] 비prefetchable 창이 PCI 쪽에서 갖는 주소(= 로컬 주소 - 변환 오프셋).
	 * 설정자: v3_pci_setup_resource() 가 mem->start - win->offset 으로 계산한다.
	 * 읽는 자: 같은 함수가 창 0 의 목적지 레지스터(LB_MAP0)에 쓸 때. 그 뒤로는
	 *   읽히지 않는다 — 창 0 의 목적지는 config 접근 중에도 바뀌지 않기 때문이다
	 *   (상류 주석의 "MAP0 already correct").
	 * 값 범위: 물리 주소 타입이지만 실제로는 PCI 버스 주소를 담는다.
	 * 동기화: probe 이후 읽기 전용. */
	phys_addr_t non_pre_bus_addr;
	/* [한국어] prefetchable 창이 PCI 쪽에서 갖는 주소.
	 * 설정자: v3_pci_setup_resource() 의 prefetch 분기.
	 * 읽는 자: 같은 함수와 v3_unmap_bus() — 후자가 창 1 을 되돌릴 때마다 이 값을
	 *   다시 써 넣는다. 그래서 이 필드는 config 접근이 있을 때마다 읽힌다.
	 * 값 범위: 위와 같다.
	 * 동기화: probe 이후 읽기 전용. */
	phys_addr_t pre_bus_addr;
	/* [한국어] ARM Integrator/AP 보드의 시스템 컨트롤러 regmap.
	 * 설정자: v3_integrator_init() 이 syscon_regmap_lookup_by_compatible() 로 얻는다.
	 *   그 함수는 Integrator 계열 DT 일 때만 불리므로, 다른 보드에서는 이 필드가
	 *   0 초기화된 채(NULL) 남는다.
	 * 읽는 자: v3_irq() 가 인터럽트를 지울 때 NULL 검사 후 쓴다.
	 * 값 범위: 유효한 regmap 포인터 또는 NULL.
	 * 동기화: regmap 프레임워크가 내부적으로 처리한다.
	 * [상류 코드 관찰] v3_integrator_init() 은 lookup 실패 시 IS_ERR 로 판정해
	 *   -ENODEV 를 돌려주지만, 그때 이 필드에는 이미 ERR_PTR 값이 들어가 있다.
	 *   probe 가 곧바로 실패하므로 v3_irq() 가 그 값을 보는 경로는 없다. */
	struct regmap *map;
};

/*
 * The V3 PCI interface chip in Integrator provides several windows from
 * local bus memory into the PCI memory areas. Unfortunately, there
 * are not really enough windows for our usage, therefore we reuse
 * one of the windows for access to PCI configuration space. On the
 * Integrator/AP, the memory map is as follows:
 *
 * Local Bus Memory         Usage
 *
 * 40000000 - 4FFFFFFF      PCI memory.  256M non-prefetchable
 * 50000000 - 5FFFFFFF      PCI memory.  256M prefetchable
 * 60000000 - 60FFFFFF      PCI IO.  16M
 * 61000000 - 61FFFFFF      PCI Configuration. 16M
 *
 * There are three V3 windows, each described by a pair of V3 registers.
 * These are LB_BASE0/LB_MAP0, LB_BASE1/LB_MAP1 and LB_BASE2/LB_MAP2.
 * Base0 and Base1 can be used for any type of PCI memory access.   Base2
 * can be used either for PCI I/O or for I20 accesses.  By default, uHAL
 * uses this only for PCI IO space.
 *
 * Normally these spaces are mapped using the following base registers:
 *
 * Usage Local Bus Memory         Base/Map registers used
 *
 * Mem   40000000 - 4FFFFFFF      LB_BASE0/LB_MAP0
 * Mem   50000000 - 5FFFFFFF      LB_BASE1/LB_MAP1
 * IO    60000000 - 60FFFFFF      LB_BASE2/LB_MAP2
 * Cfg   61000000 - 61FFFFFF
 *
 * This means that I20 and PCI configuration space accesses will fail.
 * When PCI configuration accesses are needed (via the uHAL PCI
 * configuration space primitives) we must remap the spaces as follows:
 *
 * Usage Local Bus Memory         Base/Map registers used
 *
 * Mem   40000000 - 4FFFFFFF      LB_BASE0/LB_MAP0
 * Mem   50000000 - 5FFFFFFF      LB_BASE0/LB_MAP0
 * IO    60000000 - 60FFFFFF      LB_BASE2/LB_MAP2
 * Cfg   61000000 - 61FFFFFF      LB_BASE1/LB_MAP1
 *
 * To make this work, the code depends on overlapping windows working.
 * The V3 chip translates an address by checking its range within
 * each of the BASE/MAP pairs in turn (in ascending register number
 * order).  It will use the first matching pair.   So, for example,
 * if the same address is mapped by both LB_BASE0/LB_MAP0 and
 * LB_BASE1/LB_MAP1, the V3 will use the translation from
 * LB_BASE0/LB_MAP0.
 *
 * To allow PCI Configuration space access, the code enlarges the
 * window mapped by LB_BASE0/LB_MAP0 from 256M to 512M.  This occludes
 * the windows currently mapped by LB_BASE1/LB_MAP1 so that it can
 * be remapped for use by configuration cycles.
 *
 * At the end of the PCI Configuration space accesses,
 * LB_BASE1/LB_MAP1 is reset to map PCI Memory.  Finally the window
 * mapped by LB_BASE0/LB_MAP0 is reduced in size from 512M to 256M to
 * reveal the now restored LB_BASE1/LB_MAP1 window.
 *
 * NOTE: We do not set up I2O mapping.  I suspect that this is only
 * for an intelligent (target) device.  Using I2O disables most of
 * the mappings into PCI memory.
 */
/* [한국어]
 * v3_map_bus - 설정공간 접근용 주소를 만들고, 그것을 위해 주소 창을 다시 그린다
 *
 * @bus: 접근 대상 PCI 버스. sysdata 로 이 드라이버의 객체가 걸려 있다.
 * @devfn: 장치·함수 번호(상위 5비트 장치, 하위 3비트 함수).
 * @offset: 설정공간 안의 레지스터 오프셋.
 * @return: 실제로 읽거나 쓸 커널 가상 주소.
 *
 * 위 상류 주석 블록이 이 함수가 존재하는 이유를 길게 설명하고 있다. 요약하면,
 * 로컬 버스 → PCI 변환 창이 세 쌍뿐이라 설정공간에 상시 배정할 창이 없다.
 * 그래서 접근할 때마다 창 0 을 256MB 에서 512MB 로 늘려 창 1 을 가린 뒤
 * (V3 는 번호가 작은 창부터 검사해 처음 맞는 창을 쓰므로, 겹치면 창 0 이 이긴다),
 * 그 창 1 을 설정공간용으로 다시 겨눈다. 접근이 끝나면 v3_unmap_bus() 가 되돌린다.
 *
 * 주소를 만드는 방식은 대상 버스가 로컬 세그먼트인지에 따라 갈린다.
 *  - 버스 0(브리지에 직접 붙은 세그먼트): PCI 규격의 type 0 설정 사이클은 주소에
 *    장치 번호 필드가 없다. 대신 호스트가 장치마다 서로 다른 상위 주소선 하나를
 *    올려(one-hot) 그 장치의 IDSEL 을 때린다. 그래서 슬롯 n 은 주소 비트 (n+11)
 *    하나에 대응한다. 설정공간 창이 16MB(주소 24비트)라 비트 23 까지만 창 안의
 *    오프셋으로 표현할 수 있고, 그것이 슬롯 12(11+12 = 23)까지다. 슬롯 13 부터는
 *    주소 비트 24 이상이 필요한데, 창 목적지 레지스터의 비트 b 가 PCI 주소 비트
 *    b+16 에 대응하므로 BIT(slot - 5) 로 같은 비트를 지정한다. 두 분기의 식
 *    (slot + 11)과 (slot - 5)가 정확히 16 만큼 차이 나는 것이 그 대응의 증거다.
 *  - 버스 0 이 아닌 경우: type 1 설정 사이클이라 주소에 버스/장치/함수가 그대로
 *    들어간다. 하위 두 주소 비트를 로컬 버스에서 가져오도록 AD_LOW_EN 도 켠다.
 *
 * 실행 컨텍스트: PCI 코어의 config 접근 경로. 전역 pci_lock 을 쥔 상태이므로
 * 잠들 수 없고, 동시에 그 락 덕분에 '창을 바꿔 놓은 사이' 에 다른 접근이 끼어들지
 * 못한다 — 이 기법이 성립하는 근거다.
 * 에러 경로: 없다. 잘못된 슬롯 번호를 걸러 내는 검사가 없다.
 *
 * 호출 체인:
 *   pci_generic_config_read() / pci_generic_config_write() → [v3_map_bus]
 *     → writel(), writew()
 */
static void __iomem *v3_map_bus(struct pci_bus *bus,
				unsigned int devfn, int offset)
{
	/* [한국어] 브리지의 sysdata 로 걸어 둔 이 드라이버의 객체를 되찾는다
	 * (probe 가 host->sysdata = v3 로 설정했다). */
	struct v3_pci *v3 = bus->sysdata;
	/* [한국어] address 는 설정공간 창 안에서의 오프셋, mapaddress 는 창 1 목적지
	 * 레지스터에 쓸 값, busnr 는 대상 버스 번호. */
	unsigned int address, mapaddress, busnr;

	/* [한국어] 대상 버스 번호를 꺼낸다. 이 값이 0 인지가 사이클 종류를 가른다. */
	busnr = bus->number;
	/* [한국어] 버스 0 = 브리지에 직접 붙은 로컬 세그먼트 → type 0 설정 사이클. */
	if (busnr == 0) {
		/* [한국어] devfn 의 상위 5비트가 장치(슬롯) 번호다. */
		int slot = PCI_SLOT(devfn);

		/*
		 * local bus segment so need a type 0 config cycle
		 *
		 * build the PCI configuration "address" with one-hot in
		 * A31-A11
		 *
		 * mapaddress:
		 *  3:1 = config cycle (101)
		 *  0   = PCI A1 & A0 are 0 (0)
		 */
		/* [한국어] 옆의 상류 주석대로, 함수 번호를 주소 비트 10..8 에 놓는다.
		 * 하위 8비트는 호출자가 더할 레지스터 오프셋 자리로 남겨 둔다. */
		address = PCI_FUNC(devfn) << 8;
		/* [한국어] 창 1 의 목적지 종류를 '설정 사이클' 로 정한다. 여기에 슬롯의 one-hot
		 * 비트가 더해질 수 있다. */
		mapaddress = V3_LB_MAP_TYPE_CONFIG;

		/* [한국어] 슬롯 12 까지는 필요한 one-hot 비트가 23 이하라 16MB 창 안의 오프셋으로
		 * 표현되지만, 13 부터는 비트 24 이상이 필요해 창 오프셋으로는 닿지 않는다. */
		if (slot > 12)
			/*
			 * high order bits are handled by the MAP register
			 */
			/* [한국어] 옆의 상류 주석대로 상위 비트는 목적지 레지스터가 담당한다.
			 * 그 레지스터의 비트 b 가 PCI 주소 비트 b+16 에 대응하므로, 주소 비트 (slot+11)
			 * 을 만들려면 비트 (slot-5) 를 세우면 된다. */
			mapaddress |= BIT(slot - 5);
		else
			/*
			 * low order bits handled directly in the address
			 */
			/* [한국어] 옆의 상류 주석대로 하위 비트는 창 안 오프셋으로 직접 만든다.
			 * 슬롯 n 이 주소 비트 (n+11) 하나만 올리는 one-hot 방식이라, 이 비트가 그대로
			 * 그 슬롯의 IDSEL 신호가 된다. */
			address |= BIT(slot + 11);
	/* [한국어] 버스 0 이 아니면 브리지 너머의 장치이므로 type 1 설정 사이클이다. */
	} else {
		/*
		 * not the local bus segment so need a type 1 config cycle
		 *
		 * address:
		 *  23:16 = bus number
		 *  15:11 = slot number (7:3 of devfn)
		 *  10:8  = func number (2:0 of devfn)
		 *
		 * mapaddress:
		 *  3:1 = config cycle (101)
		 *  0   = PCI A1 & A0 from host bus (1)
		 */
		/* [한국어] 옆의 상류 주석대로 설정 사이클 종류에 더해, 주소 하위 두 비트를
		 * 로컬 버스에서 가져오도록 허용 비트를 켠다. type 1 사이클은 주소의 최하위
		 * 두 비트가 사이클 종류를 나타내는 자리라 호스트가 직접 실어야 하기 때문이다. */
		mapaddress = V3_LB_MAP_TYPE_CONFIG | V3_LB_MAP_AD_LOW_EN;
		/* [한국어] 옆의 상류 주석대로 버스 번호를 23..16 에, devfn(장치+함수)을 15..8 에
		 * 놓는다. one-hot 이 아니라 번호를 그대로 싣는 것이 type 0 과의 결정적 차이다. */
		address = (busnr << 16) | (devfn << 8);
	}

	/*
	 * Set up base0 to see all 512Mbytes of memory space (not
	 * prefetchable), this frees up base1 for re-use by
	 * configuration memory
	 */
	/* [한국어] 옆의 상류 주석대로 창 0 을 512MB 로 늘린다. 비prefetchable 메모리
	 * 창(256MB)과 그 뒤에 이어 붙은 prefetchable 창(256MB)을 통째로 덮으므로,
	 * 창 1 이 담당하던 영역이 창 0 에 가려진다 — V3 가 번호 순으로 검사해 먼저 맞는
	 * 창을 쓰기 때문에 가능한 일이다. 이렇게 해서 창 1 이 자유로워진다.
	 * 이 순간 prefetchable 접근은 창 0 을 타고 가는데, 두 창의 목적지 주소가
	 * 이어져 있어야 결과가 같다 — probe 의 인접성 검사가 그것을 강제한다. */
	writel(v3_addr_to_lb_base(v3->non_pre_mem) |
	       V3_LB_BASE_ADR_SIZE_512MB | V3_LB_BASE_ENABLE,
	       v3->base + V3_LB_BASE0);

	/*
	 * Set up base1/map1 to point into configuration space.
	 * The config mem is always 16MB.
	 */
	/* [한국어] 옆의 상류 주석대로 창 1 을 설정공간에 겨눈다. 크기 16MB 는
	 * 하드웨어가 정한 설정공간 창 크기이며, probe 가 DT 자원이 정확히 16MB 인지
	 * 확인해 둔다. */
	writel(v3_addr_to_lb_base(v3->config_mem) |
	       V3_LB_BASE_ADR_SIZE_16MB | V3_LB_BASE_ENABLE,
	       v3->base + V3_LB_BASE1);
	/* [한국어] 창 1 의 목적지에 위에서 만든 값을 쓴다 — 사이클 종류가 '설정' 이고,
	 * type 0 의 큰 슬롯 번호라면 one-hot 비트도 함께 실려 있다. 이 쓰기가 끝나야
	 * 아래 반환 주소로의 접근이 실제 설정 사이클이 된다. */
	writew(mapaddress, v3->base + V3_LB_MAP1);

	/* [한국어] 설정공간 창의 가상 주소에 슬롯/함수 오프셋과 레지스터 오프셋을 더해
	 * 최종 접근 주소를 만든다. 호출자(pci_generic_config_read/write)가 이 주소를
	 * 크기에 맞춰 읽거나 쓴다. */
	return v3->config_base + address + offset;
}

/* [한국어]
 * v3_unmap_bus - config 접근 때문에 바꿔 놓았던 창 0 과 창 1 을 되돌린다
 *
 * @v3: 컨트롤러 객체.
 * @return: 없음.
 *
 * v3_map_bus() 의 짝이며, 순서가 뒤집혀 있다는 점이 중요하다. 먼저 창 1 을
 * prefetchable 메모리로 되돌린 다음에 창 0 을 256MB 로 줄인다. 반대로 하면
 * 창 0 을 줄이는 순간 창 1 이 아직 설정공간을 가리키는 채로 드러나, 그 사이의
 * 메모리 접근이 엉뚱한 곳으로 간다.
 * 창 0 의 목적지(LB_MAP0)는 손대지 않는데, config 접근 중에도 바꾼 적이 없기
 * 때문이다 — 옆의 상류 주석이 그 사실을 밝히고 있다.
 * 실행 컨텍스트: config 접근 직후, 여전히 pci_lock 아래. 잠들지 않는다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   v3_pci_read_config() / v3_pci_write_config() → [v3_unmap_bus] → writel(), writew()
 */
static void v3_unmap_bus(struct v3_pci *v3)
{
	/*
	 * Reassign base1 for use by prefetchable PCI memory
	 */
	/* [한국어] 옆의 상류 주석대로 창 1 을 prefetchable 메모리 용도로 되돌린다.
	 * 크기 256MB, prefetch 표시, 활성화를 함께 쓴다. */
	writel(v3_addr_to_lb_base(v3->pre_mem) |
	       V3_LB_BASE_ADR_SIZE_256MB | V3_LB_BASE_PREFETCH |
	       V3_LB_BASE_ENABLE,
	       v3->base + V3_LB_BASE1);
	/* [한국어] 목적지도 원래의 PCI 주소와 메모리 사이클 종류로 되돌린다.
	 * 옆의 상류 주석은 예전에 다중 읽기 사이클을 쓰다가 보통 메모리 사이클로
	 * 바꾼 흔적을 남겨 두었다. */
	writew(v3_addr_to_lb_map(v3->pre_bus_addr) |
	       V3_LB_MAP_TYPE_MEM, /* was V3_LB_MAP_TYPE_MEM_MULTIPLE */
	       v3->base + V3_LB_MAP1);

	/*
	 * And shrink base0 back to a 256M window (NOTE: MAP0 already correct)
	 */
	/* [한국어] 옆의 상류 주석대로 창 0 을 256MB 로 되돌린다. 이 쓰기로 창 1 이 다시
	 * 드러나 prefetchable 영역을 담당하게 된다. 순서상 반드시 창 1 을 먼저 고친
	 * 뒤여야 한다. */
	writel(v3_addr_to_lb_base(v3->non_pre_mem) |
	       V3_LB_BASE_ADR_SIZE_256MB | V3_LB_BASE_ENABLE,
	       v3->base + V3_LB_BASE0);
}

/* [한국어]
 * v3_pci_read_config - 설정공간을 읽고 나서 반드시 창을 되돌린다
 *
 * @bus: 대상 버스.
 * @fn: 장치·함수 번호.
 * @config: 설정공간 오프셋.
 * @size: 읽을 바이트 수(1/2/4).
 * @value: 읽은 값을 담을 곳.
 * @return: PCIBIOS_ 계열 상태값. 표준 구현이 돌려준 것을 그대로 전달한다.
 *
 * 실제 읽기는 표준 구현이 하고, 이 함수는 그 앞뒤를 감싸는 껍데기다. 존재
 * 이유는 오직 하나 — 접근이 끝난 뒤 v3_unmap_bus() 를 부르기 위해서다.
 * 표준 구현이 map_bus 후크로 창을 바꾸게 만들 수는 있지만 되돌리게 할 수는
 * 없기 때문에, 되돌리기를 여기서 손으로 한다.
 * 실행 컨텍스트: PCI 코어가 pci_lock 을 쥐고 부른다. 잠들지 않는다.
 * 에러 경로: 표준 구현의 실패도 창은 반드시 되돌린다 — unmap 이 조건 없이
 * 반환 직전에 놓여 있어 그 순서가 보장된다.
 *
 * [상류 코드 관찰] 디버그 로그가 읽기 '전' 에 *value 를 출력한다. 그 시점의
 * value 는 아직 채워지지 않은 값이라, 로그에 찍히는 것은 호출자가 넘긴 초기값이다.
 *
 * 호출 체인:
 *   (PCI 코어의 설정공간 읽기) → [v3_pci_read_config]
 *     → pci_generic_config_read() → v3_map_bus(), v3_unmap_bus()
 */
static int v3_pci_read_config(struct pci_bus *bus, unsigned int fn,
			      int config, int size, u32 *value)
{
	/* [한국어] unmap 에 넘길 컨트롤러 객체를 sysdata 에서 되찾는다. */
	struct v3_pci *v3 = bus->sysdata;
	/* [한국어] 표준 구현의 반환값. */
	int ret;

	/* [한국어] 접근 내역을 디버그 로그로 남긴다. 위 관찰대로 *value 는 아직 읽기 전
	 * 값이다. */
	dev_dbg(&bus->dev,
		"[read]  slt: %.2d, fnc: %d, cnf: 0x%.2X, val (%d bytes): 0x%.8X\n",
		PCI_SLOT(fn), PCI_FUNC(fn), config, size, *value);
	/* [한국어] 표준 구현이 map_bus 후크(v3_map_bus)를 불러 주소를 얻고 크기에 맞춰
	 * 읽는다. 즉 창을 바꾸는 일은 이 호출 안에서 일어난다. */
	ret = pci_generic_config_read(bus, fn, config, size, value);
	/* [한국어] 창을 원래대로 되돌린다. 이것을 빠뜨리면 다음 메모리 접근이
	 * 설정공간으로 새어 나간다. */
	v3_unmap_bus(v3);
	/* [한국어] 표준 구현의 상태값을 그대로 전달한다. */
	return ret;
}

/* [한국어]
 * v3_pci_write_config - 설정공간에 쓰고 나서 반드시 창을 되돌린다
 *
 * @bus: 대상 버스.
 * @fn: 장치·함수 번호.
 * @config: 설정공간 오프셋.
 * @size: 쓸 바이트 수(1/2/4).
 * @value: 쓸 값.
 * @return: PCIBIOS_ 계열 상태값.
 *
 * 읽기 쪽과 완전히 대칭이며 존재 이유도 같다 — 접근 뒤 창을 되돌리기 위해서다.
 * 이쪽 로그는 실제로 쓸 값을 출력하므로 읽기 쪽과 달리 값이 유효하다.
 * 실행 컨텍스트: pci_lock 아래. 잠들지 않는다.
 *
 * 호출 체인:
 *   (PCI 코어의 설정공간 쓰기) → [v3_pci_write_config]
 *     → pci_generic_config_write() → v3_map_bus(), v3_unmap_bus()
 */
static int v3_pci_write_config(struct pci_bus *bus, unsigned int fn,
				    int config, int size, u32 value)
{
	/* [한국어] unmap 에 넘길 컨트롤러 객체. */
	struct v3_pci *v3 = bus->sysdata;
	/* [한국어] 표준 구현의 반환값. */
	int ret;

	/* [한국어] 무엇을 쓰는지 디버그 로그로 남긴다. 이쪽 value 는 인자로 받은 값이라
	 * 로그가 실제 내용과 일치한다. */
	dev_dbg(&bus->dev,
		"[write] slt: %.2d, fnc: %d, cnf: 0x%.2X, val (%d bytes): 0x%.8X\n",
		PCI_SLOT(fn), PCI_FUNC(fn), config, size, value);
	/* [한국어] 표준 구현이 v3_map_bus 로 주소를 얻어 크기에 맞춰 쓴다. */
	ret = pci_generic_config_write(bus, fn, config, size, value);
	/* [한국어] 창을 되돌린다. 읽기 쪽과 같은 이유로 생략할 수 없다. */
	v3_unmap_bus(v3);
	/* [한국어] 상태값 전달. */
	return ret;
}

/* [한국어] PCI 코어가 설정공간에 접근할 때 쓸 연산 표. map_bus 후크가 있는
 * 형태라, 주소 계산과 실제 접근이 분리되어 있다. */
static struct pci_ops v3_pci_ops = {
	/* [한국어] 접근 주소를 만드는 후크. 이 드라이버에서는 창 재매핑까지 겸한다. */
	.map_bus = v3_map_bus,
	/* [한국어] 읽기. 표준 구현을 감싸 unmap 을 덧붙인 판이다. */
	.read = v3_pci_read_config,
	/* [한국어] 쓰기. 마찬가지. */
	.write = v3_pci_write_config,
};

/* [한국어]
 * v3_irq - 브리지가 올린 오류/이벤트 인터럽트를 로그로 남기고 지운다
 *
 * @irq: 발생한 리눅스 IRQ 번호(쓰이지 않는다).
 * @data: devm_request_irq 에 넘겼던 컨트롤러 객체.
 * @return: 항상 IRQ_HANDLED.
 *
 * 이 브리지는 두 곳에 오류를 기록한다 — PCI 쪽 표준 Status 레지스터와, 로컬
 * 버스 쪽 인터럽트 상태 레지스터다. 그래서 핸들러도 두 덩어리로 나뉜다.
 * 하는 일은 진단뿐이다. 오류를 복구하거나 상위에 알리는 경로는 없고, 원인을
 * 로그로 남긴 뒤 상태를 지워 인터럽트가 계속 올라오지 않게 하는 것이 전부다.
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(플래그 0 으로 요청했으므로 스레드
 * 핸들러가 아니다). 잠들 수 없다. dev_err/dev_info 와 regmap 쓰기는 이 문맥에서
 * 쓸 수 있는 동작이다.
 *
 * [상류 코드 관찰] 세 가지.
 *  1. 상태 비트가 하나도 서 있지 않아도 언제나 IRQ_HANDLED 를 돌려준다. 공유
 *     IRQ 라면 다른 장치의 인터럽트를 가로채는 셈이 되지만, 요청 플래그가 0 이라
 *     이 IRQ 는 공유되지 않는다.
 *  2. PCI Status 는 읽은 값을 그대로 되써서 지우고(W1C), 로컬 버스 상태는 0 을
 *     써서 지운다. 두 레지스터의 지우는 방식이 서로 다르다.
 *  3. regmap 이 있는 보드(Integrator)에서는 인터럽트를 지울 때 활성화 비트를
 *     함께 다시 써 준다 — 그 비트를 빠뜨리면 브리지가 리셋에 들어가기 때문으로
 *     보이나, 그 근거가 되는 보드 문서는 이 트리에 없다.
 *
 * 호출 체인:
 *   (인터럽트 코어) → [v3_irq] → readw()/readb(), dev_err(), writew()/writeb(),
 *     regmap_write()
 */
static irqreturn_t v3_irq(int irq, void *data)
{
	/* [한국어] 요청 시 넘긴 컨트롤러 객체를 되찾는다. */
	struct v3_pci *v3 = data;
	/* [한국어] 로그 대상 device. */
	struct device *dev = v3->dev;
	/* [한국어] 두 상태 레지스터를 차례로 담을 변수. */
	u32 status;

	/* [한국어] PCI 표준 Status 레지스터를 16비트로 읽는다. */
	status = readw(v3->base + V3_PCI_STAT);
	/* [한국어] 비트 15 — 패리티 오류를 감지했다. 데이터가 전송 중 깨졌다는 뜻이다. */
	if (status & V3_PCI_STAT_PAR_ERR)
		dev_err(dev, "parity error interrupt\n");
	/* [한국어] 비트 14 — SERR# 를 냈다. 복구 불가능한 시스템 수준 오류다. */
	if (status & V3_PCI_STAT_SYS_ERR)
		dev_err(dev, "system error interrupt\n");
	/* [한국어] 비트 13 — 마스터 어보트. 주소에 응답하는 장치가 없었다는 뜻으로,
	 * 비어 있는 슬롯을 스캔할 때도 정상적으로 발생한다. */
	if (status & V3_PCI_STAT_M_ABORT_ERR)
		dev_err(dev, "master abort error interrupt\n");
	/* [한국어] 비트 12 — 타깃 어보트. 상대 장치가 거래를 중단시켰다. */
	if (status & V3_PCI_STAT_T_ABORT_ERR)
		dev_err(dev, "target abort error interrupt\n");
	/* [한국어] 읽은 값을 그대로 되써서 서 있던 비트만 지운다(W1C). 새로 생긴 비트를
	 * 실수로 지우지 않는 안전한 방식이다. */
	writew(status, v3->base + V3_PCI_STAT);

	/* [한국어] 로컬 버스 인터럽트 상태를 8비트로 읽는다. */
	status = readb(v3->base + V3_LB_ISTAT);
	/* [한국어] 비트 7 — 메일박스 인터럽트. 오류가 아니라 정보성이다. */
	if (status & V3_LB_ISTAT_MAILBOX)
		dev_info(dev, "PCI mailbox interrupt\n");
	/* [한국어] 비트 6 — 로컬 버스에서 PCI 로 나간 읽기가 어보트되었다. */
	if (status & V3_LB_ISTAT_PCI_RD)
		dev_err(dev, "PCI target LB->PCI READ abort interrupt\n");
	/* [한국어] 비트 5 — 같은 방향의 쓰기가 어보트되었다. */
	if (status & V3_LB_ISTAT_PCI_WR)
		dev_err(dev, "PCI target LB->PCI WRITE abort interrupt\n");
	/* [한국어] 비트 4 — PCI 핀 인터럽트(INTx)가 들어왔다. 정보성으로만 남기며,
	 * 이 드라이버는 INTx 를 장치 드라이버에게 전달하는 도메인을 만들지 않는다. */
	if (status &  V3_LB_ISTAT_PCI_INT)
		dev_info(dev, "PCI pin interrupt\n");
	/* [한국어] 비트 3 — PCI 패리티 오류. */
	if (status & V3_LB_ISTAT_PCI_PERR)
		dev_err(dev, "PCI parity error interrupt\n");
	/* [한국어] 비트 2 — I2O 인바운드 큐 인터럽트. 이 드라이버는 I2O 를 설정하지
	 * 않으므로 정상 동작에서는 오지 않는다. */
	if (status & V3_LB_ISTAT_I2O_QWR)
		dev_info(dev, "I2O inbound post queue interrupt\n");
	/* [한국어] 비트 1 — 칩 내장 DMA 채널 1 완료. 이 드라이버는 그 DMA 를 쓰지 않는다. */
	if (status & V3_LB_ISTAT_DMA1)
		dev_info(dev, "DMA channel 1 interrupt\n");
	/* [한국어] 비트 0 — 내장 DMA 채널 0 완료. 마찬가지. */
	if (status & V3_LB_ISTAT_DMA0)
		dev_info(dev, "DMA channel 0 interrupt\n");
	/* Clear all possible interrupts on the local bus */
	/* [한국어] 옆의 상류 주석대로 로컬 버스 인터럽트를 모두 지운다. PCI Status 와
	 * 달리 0 을 써서 지우는 방식이다(위 관찰 2). */
	writeb(0, v3->base + V3_LB_ISTAT);
	/* [한국어] Integrator 보드라면 보드 쪽 인터럽트 래치도 지워야 한다. 다른
	 * 보드에서는 이 필드가 NULL 이라 건너뛴다. */
	if (v3->map)
		/* [한국어] 활성화 비트와 인터럽트 지우기 비트를 함께 쓴다. 활성화 비트를 같이
		 * 써 주는 이유는 위 관찰 3 참조. */
		regmap_write(v3->map, INTEGRATOR_SC_PCI_OFFSET,
			     INTEGRATOR_SC_PCI_ENABLE |
			     INTEGRATOR_SC_PCI_INTCLR);

	/* [한국어] 이 IRQ 는 공유되지 않으므로 언제나 처리했다고 알린다(위 관찰 1). */
	return IRQ_HANDLED;
}

/* [한국어]
 * v3_integrator_init - ARM Integrator/AP 보드에서만 필요한 추가 초기화를 한다
 *
 * @v3: 컨트롤러 객체. 이 함수가 v3->map 을 채운다.
 * @return: 0 성공, -ENODEV(보드 시스템 컨트롤러를 못 찾음).
 *
 * V3 칩 자체의 설정은 probe 가 다 하지만, 이 보드에서는 브리지를 리셋에서
 * 꺼내는 스위치가 칩 밖 — 보드의 시스템 컨트롤러 레지스터 — 에 있다. 그래서
 * syscon 으로 그 레지스터를 찾아 켠다. 이 함수가 DT compatible 검사 뒤에서만
 * 불리는 이유가 그것이다.
 * 리셋에서 막 나온 경우에는 칩이 안정될 시간이 필요해 230ms 를 자고, 칩이
 * 자기 물리 베이스를 잊었으므로 다시 알려 주고, 메일박스로 응답을 확인한다.
 * 이미 켜져 있었다면(부트로더가 켜 둔 경우) 그 과정을 통째로 건너뛴다.
 * 실행 컨텍스트: probe 경로. msleep 이 있어 잠들 수 있다.
 *
 * [상류 코드 관찰] 세 가지.
 *  1. regmap_read() 의 반환값을 확인하지 않는다. 실패하면 val 이 초기화되지 않은
 *     채 아래 조건에 쓰인다.
 *  2. 메일박스 대기 루프가 0xaa 와 0x55 를 서로 다른 주소에 쓰고는, 되읽을 때는
 *     두 번 모두 같은 주소(V3_MAIL_DATA)를 읽는다. 두 번째 비교는 V3_MAIL_DATA + 4
 *     를 겨냥한 것으로 보이지만 코드는 그렇지 않다. && 로 묶여 있어 한쪽만 맞아도
 *     빠져나오므로, 실질적으로는 첫 비교만 작동한다. 이 판단은 코드만으로 한 것이며
 *     의도를 밝힌 문서는 이 트리에 없다.
 *  3. 실패 경로에서 v3->map 에 ERR_PTR 값이 남는다(struct v3_pci 의 map 필드 설명 참조).
 *
 * 호출 체인:
 *   v3_pci_probe() → [v3_integrator_init]
 *     → syscon_regmap_lookup_by_compatible(), regmap_read(), regmap_write(),
 *       msleep(), writel(), writeb(), readb()
 */
static int v3_integrator_init(struct v3_pci *v3)
{
	/* [한국어] 보드 레지스터의 현재 값을 담을 변수. 브리지가 이미 켜져 있었는지를
	 * 판정하는 데 쓴다. */
	unsigned int val;

	/* [한국어] Integrator/AP 의 시스템 컨트롤러 regmap 을 compatible 문자열로 찾는다.
	 * 이 레지스터는 V3 칩이 아니라 보드에 속하므로, 이 드라이버의 자원 창으로는
	 * 닿을 수 없다. 그 시스템 컨트롤러를 등록하는 코드는 이 트리에 없어 확인 못 함. */
	v3->map =
		syscon_regmap_lookup_by_compatible("arm,integrator-ap-syscon");
	/* [한국어] 못 찾으면 브리지를 리셋에서 꺼낼 방법이 없다. 오류가 ERR_PTR 에 실려
	 * 오므로 IS_ERR 로 판정한다. */
	if (IS_ERR(v3->map)) {
		/* [한국어] 원인을 로그로 남긴다. */
		dev_err(v3->dev, "no syscon\n");
		/* [한국어] 장치가 없다는 뜻으로 실패시킨다 — 반환값이 ERR_PTR 의 실제 오류가
		 * 아니라 고정된 -ENODEV 라는 점에 주의. */
		return -ENODEV;
	}

	/* [한국어] 브리지가 이미 켜져 있었는지 알아보려고 현재 값을 먼저 읽는다.
	 * 아래 쓰기가 그 상태를 덮어쓰므로 순서가 중요하다(위 관찰 1 참조). */
	regmap_read(v3->map, INTEGRATOR_SC_PCI_OFFSET, &val);
	/* Take the PCI bridge out of reset, clear IRQs */
	/* [한국어] 옆의 상류 주석대로 브리지를 리셋에서 꺼내고 걸려 있던 인터럽트를
	 * 지운다. 두 비트를 한 번의 쓰기로 함께 준다. */
	regmap_write(v3->map, INTEGRATOR_SC_PCI_OFFSET,
		     INTEGRATOR_SC_PCI_ENABLE |
		     INTEGRATOR_SC_PCI_INTCLR);

	/* [한국어] 방금 읽은 값에 활성화 비트가 없었다면, 위 쓰기가 브리지를 막 리셋에서
	 * 꺼낸 것이다. 그 경우에만 아래의 안정화 절차가 필요하다. */
	if (!(val & INTEGRATOR_SC_PCI_ENABLE)) {
		/* If we were in reset we need to sleep a bit */
		/* [한국어] 옆의 상류 주석대로 잠깐 잔다. 230ms 라는 값의 근거는 보드 문서에
		 * 있을 텐데 이 트리에서 확인 못 함. 프로세스 컨텍스트라 잠들 수 있다. */
		msleep(230);

		/* Set the physical base for the controller itself */
		/* [한국어] 옆의 상류 주석대로 칩에 자기 물리 베이스를 다시 알려 준다. 이 레지스터가
		 * 담는 것은 주소의 상위 16비트이므로 0x6200 은 0x62000000 을 뜻한다.
		 * 그 해석의 근거는 같은 파일의 probe 에 있다 — 거기서 이 레지스터를
		 * (regs->start >> 16) 과 비교한다. 값이 상수로 박혀 있다는 것은 이 코드가
		 * Integrator/AP 한 보드만 상대한다는 뜻이다. */
		writel(0x6200, v3->base + V3_LB_IO_BASE);

		/* Wait for the mailbox to settle after reset */
		/* [한국어] 옆의 상류 주석대로 리셋 뒤 메일박스가 안정될 때까지 기다린다.
		 * do-while 이라 최소 한 번은 쓴다. */
		do {
			/* [한국어] 메일박스 첫 바이트에 표식 0xaa 를 쓴다. */
			writeb(0xaa, v3->base + V3_MAIL_DATA);
			/* [한국어] 4바이트 뒤에 다른 표식 0x55 를 쓴다. */
			writeb(0x55, v3->base + V3_MAIL_DATA + 4);
		/* [한국어] 되읽어 표식이 살아 있는지 본다. 위 관찰 2 에 적었듯 두 비교가 모두
		 * 같은 주소를 읽으므로, 실질적으로는 첫 바이트가 0xaa 로 되읽히는지만 따진다.
		 * 되읽히면 칩이 응답한다는 뜻이라 루프를 빠져나온다. 타임아웃이 없어
		 * 칩이 끝내 응답하지 않으면 여기서 무한히 돈다. */
		} while (readb(v3->base + V3_MAIL_DATA) != 0xaa &&
			 readb(v3->base + V3_MAIL_DATA) != 0x55);
	}

	/* [한국어] 이 보드용 초기화가 끝났음을 알린다. */
	dev_info(v3->dev, "initialized PCI V3 Integrator/AP integration\n");

	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * v3_pci_setup_resource - DT 가 준 아웃바운드 창 하나를 레지스터에 새긴다
 *
 * @v3: 컨트롤러 객체. 창의 시작 주소를 여기에 기억해 둔다.
 * @host: 호스트 브리지. [상류 코드 관찰] 인자로 받지만 본문에서 쓰지 않는다.
 * @win: 처리할 창 하나(PCI 코어가 DT 의 ranges 를 파싱해 만든 항목).
 * @return: 0 성공, -EINVAL(창 크기나 배치가 이 하드웨어의 요구와 맞지 않음).
 *
 * 아웃바운드(CPU → PCI) 창은 세 쌍뿐이고 용도가 미리 정해져 있다 —
 * 창 0 은 비prefetchable 메모리, 창 1 은 prefetchable 메모리, 창 2 는 IO 다.
 * 그래서 이 함수는 자원 종류를 보고 어느 창에 새길지를 고른다.
 * 메모리 창에는 두 가지 제약이 붙는데, 둘 다 config 접근 기법 때문에 생긴 것이다.
 * 첫째, 각 창이 정확히 256MB 여야 한다. 둘째, prefetchable 창이 비prefetchable
 * 창 바로 뒤에 붙어 있어야 한다. v3_map_bus() 가 창 0 을 512MB 로 늘려 창 1 을
 * 가릴 때, 그 512MB 가 두 창을 정확히 덮고 목적지 주소도 이어져야 하기 때문이다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 제약을 어기면 -EINVAL 을 돌려 probe 전체를 실패시킨다.
 *
 * [상류 코드 관찰] 인접성 검사가 non_pre_mem 이 이미 채워졌을 때만 작동한다.
 * 즉 DT 의 ranges 에서 prefetchable 창이 먼저 나오면 검사가 통째로 건너뛰어진다.
 * 창을 훑는 순서를 강제하는 코드는 없다.
 *
 * 호출 체인:
 *   v3_pci_probe() → [v3_pci_setup_resource]
 *     → resource_type(), pci_pio_to_address(), writel(), writew()
 */
static int v3_pci_setup_resource(struct v3_pci *v3,
				 struct pci_host_bridge *host,
				 struct resource_entry *win)
{
	/* [한국어] 로그 대상. */
	struct device *dev = v3->dev;
	/* [한국어] 메모리 창일 때 쓸 지역 별칭. */
	struct resource *mem;
	/* [한국어] IO 창일 때 쓸 지역 별칭. */
	struct resource *io;

	/* [한국어] 자원 플래그에서 종류를 뽑아 분기한다. PCI 코어가 DT ranges 를 파싱해
	 * IO / MEM / BUS 종류로 나눠 놓은 상태다. */
	switch (resource_type(win->res)) {
	/* [한국어] IO 공간 창 — 창 2 가 담당한다. */
	case IORESOURCE_IO:
		/* [한국어] 별칭을 잡는다. */
		io = win->res;

		/* Setup window 2 - PCI I/O */
		/* [한국어] 옆의 상류 주석대로 창 2 를 설정한다. IO 자원의 start 는 리눅스의
		 * 논리 IO 포트 번호라, pci_pio_to_address() 로 실제 물리 주소로 되돌린 뒤
		 * 창 베이스 형태로 바꾼다 — 이 변환을 빠뜨리면 엉뚱한 주소가 들어간다. */
		writel(v3_addr_to_lb_base2(pci_pio_to_address(io->start)) |
		       V3_LB_BASE2_ENABLE,
		       v3->base + V3_LB_BASE2);
		/* [한국어] 창 2 의 목적지를 설정한다. 로컬 주소에서 변환 오프셋을 빼면 PCI 쪽
		 * IO 주소가 된다. 이쪽은 논리 포트 번호가 아니라 이미 주소이므로 변환이 없다. */
		writew(v3_addr_to_lb_map2(io->start - win->offset),
		       v3->base + V3_LB_MAP2);
		/* [한국어] IO 창 처리 끝. */
		break;
	/* [한국어] 메모리 공간 창 — prefetch 여부에 따라 창 1 또는 창 0 이다. */
	case IORESOURCE_MEM:
		/* [한국어] 별칭을 잡는다. */
		mem = win->res;
		/* [한국어] prefetchable(미리 읽어도 부작용이 없는) 메모리인지 본다. 이 성질은
		 * DT 의 ranges 첫 셀에 실려 오고 PCI 코어가 플래그로 옮겨 놓는다. */
		if (mem->flags & IORESOURCE_PREFETCH) {
			/* [한국어] /proc/iomem 에 보일 이름을 붙인다. 자원 구조체를 이 드라이버가
			 * 직접 고치는 몇 안 되는 곳이다. */
			mem->name = "V3 PCI PRE-MEM";
			/* [한국어] 창을 되돌릴 때 필요하므로 로컬 버스 시작 주소를 기억해 둔다. */
			v3->pre_mem = mem->start;
			/* [한국어] PCI 쪽 주소도 기억한다. v3_unmap_bus() 가 config 접근마다 이 값을
			 * 창 1 의 목적지에 다시 써 넣는다. */
			v3->pre_bus_addr = mem->start - win->offset;
			/* [한국어] 어떤 창을 어떤 버스 주소로 여는지 디버그 로그로 남긴다.
			 * %pR 은 자원 범위를, %pap 은 물리 주소를 출력하는 커널 포맷 지정자다. */
			dev_dbg(dev, "PREFETCHABLE MEM window %pR, bus addr %pap\n",
				mem, &v3->pre_bus_addr);
			/* [한국어] 크기가 정확히 256MB 여야 한다. config 접근 기법이 '256MB 두 개를
			 * 512MB 하나로 덮는다' 는 전제 위에 서 있기 때문이다. */
			if (resource_size(mem) != SZ_256M) {
				/* [한국어] 어긋나면 이유를 남기고 */
				dev_err(dev, "prefetchable memory range is not 256MB\n");
				return -EINVAL;
			}
			/* [한국어] 비prefetchable 창을 이미 봤다면, prefetchable 창이 그 바로 뒤
			 * (+256MB)에 붙어 있는지 확인한다. 위 관찰대로 순서가 반대면 이 검사는 건너뛴다. */
			if (v3->non_pre_mem &&
			    (mem->start != v3->non_pre_mem + SZ_256M)) {
				/* [한국어] 붙어 있지 않으면 창 0 을 512MB 로 늘려도 창 1 영역을 정확히 덮지
				 * 못하므로 거절한다. */
				dev_err(dev,
					"prefetchable memory is not adjacent to non-prefetchable memory\n");
				return -EINVAL;
			}
			/* Setup window 1 - PCI prefetchable memory */
			/* [한국어] 옆의 상류 주석대로 창 1 을 prefetchable 메모리로 설정한다. 크기
			 * 256MB, prefetch 표시, 활성화를 함께 쓴다. v3_unmap_bus() 가 config 접근마다
			 * 되돌리는 값이 바로 이것과 같다. */
			writel(v3_addr_to_lb_base(v3->pre_mem) |
			       V3_LB_BASE_ADR_SIZE_256MB |
			       V3_LB_BASE_PREFETCH |
			       V3_LB_BASE_ENABLE,
			       v3->base + V3_LB_BASE1);
			/* [한국어] 창 1 의 목적지를 PCI 주소와 메모리 사이클 종류로 설정한다.
			 * 옆의 상류 주석이 예전에 다중 읽기 종류를 썼던 흔적을 남겨 두었다. */
			writew(v3_addr_to_lb_map(v3->pre_bus_addr) |
			       V3_LB_MAP_TYPE_MEM, /* Was V3_LB_MAP_TYPE_MEM_MULTIPLE */
			       v3->base + V3_LB_MAP1);
		/* [한국어] prefetch 표시가 없는 보통 메모리 창 — 창 0 이 담당한다. */
		} else {
			/* [한국어] /proc/iomem 에 보일 이름. */
			mem->name = "V3 PCI NON-PRE-MEM";
			/* [한국어] 창 0 의 시작 주소를 기억한다. v3_map_bus()/v3_unmap_bus() 가 이 값으로
			 * 창 0 을 512MB 와 256MB 사이에서 오간다. */
			v3->non_pre_mem = mem->start;
			/* [한국어] PCI 쪽 주소도 기억한다. 다만 창 0 의 목적지는 config 접근 중에도
			 * 바뀌지 않으므로 이 값은 여기서 한 번만 쓰인다. */
			v3->non_pre_bus_addr = mem->start - win->offset;
			/* [한국어] 디버그 로그. */
			dev_dbg(dev, "NON-PREFETCHABLE MEM window %pR, bus addr %pap\n",
				mem, &v3->non_pre_bus_addr);
			/* [한국어] 이쪽도 정확히 256MB 여야 한다(같은 이유). */
			if (resource_size(mem) != SZ_256M) {
				/* [한국어] 이유를 남기고 */
				dev_err(dev,
					"non-prefetchable memory range is not 256MB\n");
				return -EINVAL;
			}
			/* Setup window 0 - PCI non-prefetchable memory */
			/* [한국어] 옆의 상류 주석대로 창 0 을 비prefetchable 메모리로 설정한다.
			 * prefetch 비트가 없다는 점만 창 1 과 다르다. */
			writel(v3_addr_to_lb_base(v3->non_pre_mem) |
			       V3_LB_BASE_ADR_SIZE_256MB |
			       V3_LB_BASE_ENABLE,
			       v3->base + V3_LB_BASE0);
			/* [한국어] 창 0 의 목적지를 설정한다. 이 값은 이후 바뀌지 않는다 —
			 * v3_unmap_bus() 의 상류 주석이 "MAP0 already correct" 라고 말하는 근거다. */
			writew(v3_addr_to_lb_map(v3->non_pre_bus_addr) |
			       V3_LB_MAP_TYPE_MEM,
			       v3->base + V3_LB_MAP0);
		}
		/* [한국어] 메모리 창 처리 끝. */
		break;
	/* [한국어] 버스 번호 범위 항목 — 창 레지스터와 무관하므로 */
	case IORESOURCE_BUS:
		/* [한국어] 아무것도 하지 않는다. PCI 코어가 버스 번호 배정에 알아서 쓴다. */
		break;
	/* [한국어] 그 밖의 종류는 이 하드웨어가 다룰 수 없다. */
	default:
		/* [한국어] 무시했다는 사실만 알린다 — 오류로 만들지 않는 것은 알 수 없는 항목이
		 * 있다고 해서 브리지 전체를 못 쓰게 할 이유가 없기 때문이다. */
		dev_info(dev, "Unknown resource type %lu\n",
			 resource_type(win->res));
		/* [한국어] 알 수 없는 종류 처리 끝. */
		break;
	}

	/* [한국어] 창 하나 처리 성공. */
	return 0;
}

/* [한국어]
 * v3_get_dma_range_config - dma-ranges 항목 하나를 인바운드 창 레지스터 값으로 바꾼다
 *
 * @v3: 컨트롤러 객체(로그용).
 * @entry: DT 의 dma-ranges 에서 온 항목 하나.
 * @pci_base: 만들어진 BASE 레지스터 값을 담을 곳.
 * @pci_map: 만들어진 MAP 레지스터 값을 담을 곳.
 * @return: 0 성공, -EINVAL(주소 정렬이나 크기가 하드웨어 제약을 어김).
 *
 * 인바운드 창은 아웃바운드와 방향이 반대다 — PCI 마스터(즉 장치)가 낸 주소를
 * 로컬 버스(시스템 메모리) 주소로 바꾼다. 장치의 DMA 가 시스템 메모리에 닿는
 * 길이 바로 이 창이다. 그 규칙을 DT 의 dma-ranges 가 서술하고, 이 함수가 그것을
 * 레지스터 두 개의 값으로 번역한다.
 * 하드웨어 제약이 두 가지다. 첫째, PCI 쪽과 CPU 쪽 주소 모두 상위 12비트
 * (31..20)만 지정할 수 있어 1MB 정렬이 필요하다. 둘째, 크기가 1MB 부터 2GB 까지의
 * 2의 거듭제곱 중 하나여야 한다 — 레지스터가 크기를 지수로 받기 때문이다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 제약을 어기면 -EINVAL 로 probe 를 실패시킨다.
 *
 * [상류 코드 관찰] 크기 필드에 넣는 상수들이 V3_LB_BASE_ADR_SIZE_ 계열이다.
 * 이름만 보면 아웃바운드(로컬 버스) 창의 것인데 여기서는 인바운드 MAP 레지스터에
 * 쓴다. 두 레지스터의 크기 필드가 모두 비트 7..4 라 값이 우연히 맞아떨어지며,
 * 정작 그 자리를 위해 정의된 V3_PCI_MAP_M_ADR_SIZE 는 쓰이지 않는다.
 *
 * 호출 체인:
 *   v3_pci_parse_map_dma_ranges() → [v3_get_dma_range_config] → resource_size()
 */
static int v3_get_dma_range_config(struct v3_pci *v3,
				   struct resource_entry *entry,
				   u32 *pci_base, u32 *pci_map)
{
	/* [한국어] 로그 대상. */
	struct device *dev = v3->dev;
	/* [한국어] 이 범위의 CPU(로컬 버스) 쪽 시작 주소. */
	u64 cpu_addr = entry->res->start;
	/* [한국어] CPU 쪽 끝 주소. 로그에만 쓰인다. */
	u64 cpu_end = entry->res->end;
	/* [한국어] PCI 쪽 끝 주소 — CPU 주소에서 변환 오프셋을 뺀 값이다.
	 * [상류 코드 관찰] 이 변수 역시 아래 디버그 로그에서만 쓰인다. */
	u64 pci_end = cpu_end - entry->offset;
	/* [한국어] PCI 쪽 시작 주소. 장치가 이 주소를 내면 이 창이 잡는다. */
	u64 pci_addr = entry->res->start - entry->offset;
	/* [한국어] 레지스터에 넣을 값을 조립하는 임시 변수. */
	u32 val;

	/* [한국어] PCI 주소에 상위 12비트 말고 다른 비트가 남아 있으면 이 하드웨어로는
	 * 표현할 수 없다. 마스크를 뒤집어 AND 하는 것이 그 검사다. */
	if (pci_addr & ~V3_PCI_BASE_M_ADR_BASE) {
		/* [한국어] 1MB 정렬을 어겼다는 뜻이므로 거절한다. */
		dev_err(dev, "illegal range, only PCI bits 31..20 allowed\n");
		return -EINVAL;
	}
	/* [한국어] 64비트 주소를 32비트로 자른 뒤 상위 12비트만 남긴다. 위 검사를
	 * 통과했으므로 잘려 나가는 정보는 없다. */
	val = ((u32)pci_addr) & V3_PCI_BASE_M_ADR_BASE;
	/* [한국어] BASE 레지스터에 쓸 값이 완성됐다. 활성화 비트는 BASE 가 아니라
	 * MAP 쪽에 있어 여기서는 주소만 담는다. */
	*pci_base = val;

	/* [한국어] CPU 쪽 주소도 같은 제약을 받는다. 마스크 이름만 PCI_MAP 쪽인데,
	 * 값은 위와 같은 0xFFF00000 이다. */
	if (cpu_addr & ~V3_PCI_MAP_M_MAP_ADR) {
		/* [한국어] 1MB 정렬을 어겼으므로 거절한다. */
		dev_err(dev, "illegal range, only CPU bits 31..20 allowed\n");
		return -EINVAL;
	}
	/* [한국어] 변환 결과가 될 로컬 버스 주소의 상위 12비트를 담는다. 아래에서
	 * 크기와 활성화 비트가 이 값에 얹힌다. */
	val = ((u32)cpu_addr) & V3_PCI_MAP_M_MAP_ADR;

	/* [한국어] 창 크기를 지수 인코딩으로 바꾼다. 2의 거듭제곱이 아니면 어느 case
	 * 에도 걸리지 않아 아래 default 로 떨어진다. */
	switch (resource_size(entry->res)) {
	/* [한국어] 1MB — 이 하드웨어가 표현할 수 있는 최소 창. */
	case SZ_1M:
		/* [한국어] 크기 필드에 해당 값을 얹는다(비트 7..4). */
		val |= V3_LB_BASE_ADR_SIZE_1MB;
		break;
	/* [한국어] 2MB. */
	case SZ_2M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_2MB;
		break;
	/* [한국어] 4MB. */
	case SZ_4M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_4MB;
		break;
	/* [한국어] 8MB. */
	case SZ_8M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_8MB;
		break;
	/* [한국어] 16MB. */
	case SZ_16M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_16MB;
		break;
	/* [한국어] 32MB. */
	case SZ_32M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_32MB;
		break;
	/* [한국어] 64MB. */
	case SZ_64M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_64MB;
		break;
	/* [한국어] 128MB. */
	case SZ_128M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_128MB;
		break;
	/* [한국어] 256MB. */
	case SZ_256M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_256MB;
		break;
	/* [한국어] 512MB. */
	case SZ_512M:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_512MB;
		break;
	/* [한국어] 1GB. */
	case SZ_1G:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_1GB;
		break;
	/* [한국어] 2GB — 이 하드웨어가 표현할 수 있는 최대 창. */
	case SZ_2G:
		/* [한국어] 크기 필드 설정. */
		val |= V3_LB_BASE_ADR_SIZE_2GB;
		break;
	/* [한국어] 2의 거듭제곱이 아니거나 범위를 벗어난 크기. */
	default:
		/* [한국어] 지수로 표현할 수 없으므로 거절한다. */
		dev_err(v3->dev, "illegal dma memory chunk size\n");
		return -EINVAL;
	}
	/* [한국어] 이 창으로 칩 레지스터 접근을 허용하는 비트와 창 활성화 비트를 얹는다.
	 * 활성화 비트가 MAP 쪽에 있으므로, BASE 를 먼저 쓰고 MAP 을 나중에 써야
	 * 반쯤 설정된 창이 잠깐이라도 살아나는 일이 없다 — 호출자가 그 순서를 지킨다. */
	val |= V3_PCI_MAP_M_REG_EN | V3_PCI_MAP_M_ENABLE;
	/* [한국어] MAP 레지스터에 쓸 값이 완성됐다. */
	*pci_map = val;

	/* [한국어] CPU 주소 범위와 PCI 주소 범위, 그리고 만들어진 레지스터 값 두 개를
	 * 한 줄로 남긴다. 인바운드 창 설정은 눈에 보이지 않아 디버깅이 어렵기 때문에
	 * 이 로그가 유용하다. */
	dev_dbg(dev,
		"DMA MEM CPU: 0x%016llx -> 0x%016llx => "
		"PCI: 0x%016llx -> 0x%016llx base %08x map %08x\n",
		cpu_addr, cpu_end,
		pci_addr, pci_end,
		*pci_base, *pci_map);

	/* [한국어] 값 두 개를 모두 만들었다. */
	return 0;
}

/* [한국어]
 * v3_pci_parse_map_dma_ranges - dma-ranges 를 훑어 인바운드 창 두 개를 채운다
 *
 * @v3: 컨트롤러 객체.
 * @np: [상류 코드 관찰] 인자로 받지만 본문에서 쓰지 않는다. 범위 목록을 DT 노드가
 *      아니라 이미 파싱된 bridge->dma_ranges 에서 얻기 때문이다.
 * @return: 0 성공, v3_get_dma_range_config() 의 오류.
 *
 * PCI 코어가 DT 의 dma-ranges 를 파싱해 브리지에 목록으로 달아 두었으므로,
 * 여기서는 그 목록을 순서대로 훑어 앞의 두 항목을 창 0 과 창 1 에 새긴다.
 * 이 창이 없으면 장치가 시스템 메모리에 DMA 를 할 수 없다.
 * 사설 데이터에서 브리지로 거슬러 올라가는 pci_host_bridge_from_priv() 가
 * 여기서 쓰이는데, 이는 probe 가 브리지와 사설 데이터를 한 덩어리로 할당했기
 * 때문에 성립한다.
 * 실행 컨텍스트: probe 경로. 잠들지 않는다.
 * 에러 경로: 항목 하나라도 변환에 실패하면 곧장 되돌아가 probe 를 실패시킨다.
 * 반면 창이 모자라는 경우(셋 이상)는 오류가 아니라 경고로만 처리한다.
 *
 * 호출 체인:
 *   v3_pci_probe() → [v3_pci_parse_map_dma_ranges] → v3_get_dma_range_config(), writel()
 */
static int v3_pci_parse_map_dma_ranges(struct v3_pci *v3,
				       struct device_node *np)
{
	/* [한국어] 사설 데이터 포인터에서 그것을 품고 있는 호스트 브리지를 되찾는다. */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(v3);
	/* [한국어] 로그 대상. */
	struct device *dev = v3->dev;
	/* [한국어] 목록 순회 커서. */
	struct resource_entry *entry;
	/* [한국어] 몇 번째 항목인지 — 그대로 창 번호가 된다. */
	int i = 0;

	/* [한국어] PCI 코어가 미리 파싱해 둔 인바운드 범위 목록을 순서대로 훑는다. */
	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		/* [한국어] 변환 결과. */
		int ret;
		/* [한국어] 만들어질 두 레지스터 값. */
		u32 pci_base, pci_map;

		/* [한국어] 항목 하나를 레지스터 값 두 개로 번역한다. */
		ret = v3_get_dma_range_config(v3, entry, &pci_base, &pci_map);
		/* [한국어] 정렬이나 크기가 하드웨어 제약을 어기면 여기서 실패시킨다. */
		if (ret)
			return ret;

		/* [한국어] 첫 항목은 인바운드 창 0 에 새긴다. */
		if (i == 0) {
			/* [한국어] 주소부터 쓴다. */
			writel(pci_base, v3->base + V3_PCI_BASE0);
			/* [한국어] 그 다음 크기와 활성화 비트가 든 값을 쓴다 — 활성화가 MAP 쪽에 있으므로
			 * 이 순서라야 창이 완전히 설정된 뒤에 살아난다. */
			writel(pci_map, v3->base + V3_PCI_MAP0);
		/* [한국어] 둘째 항목은 창 1 에. */
		} else if (i == 1) {
			/* [한국어] 주소를 쓴다. */
			writel(pci_base, v3->base + V3_PCI_BASE1);
			/* [한국어] 크기와 활성화를 쓴다. */
			writel(pci_map, v3->base + V3_PCI_MAP1);
		/* [한국어] 셋째부터는 새길 창이 없다. */
		} else {
			/* [한국어] 창이 둘뿐임을 알린다. */
			dev_err(dev, "too many ranges, only two supported\n");
			/* [한국어] 어느 항목이 무시되었는지도 남긴다. 오류로 만들지 않으므로 그 범위의
			 * DMA 는 조용히 동작하지 않게 된다. */
			dev_err(dev, "range %d ignored\n", i);
		}
		/* [한국어] 다음 항목으로. */
		i++;
	}
	/* [한국어] 모든 항목 처리 완료. */
	return 0;
}

/* [한국어]
 * v3_pci_probe - 브리지를 깨우고 모든 창과 정책을 세운 뒤 PCI 코어에 넘긴다
 *
 * @pdev: DT 와 매칭된 플랫폼 디바이스.
 * @return: 0 성공, -ENOMEM / -EINVAL / 하위 단계의 오류.
 *
 * 이 파일에서 가장 긴 함수이며, 순서 자체가 하드웨어 요구에서 나온 것이다.
 *  1) 브리지와 사설 데이터를 한 덩어리로 할당하고 연산 표를 건다.
 *  2) 클럭을 켠다 — 이것이 없으면 아래 레지스터 접근이 모두 무의미하다.
 *  3) V3 레지스터 창(reg 0)과 16MB 설정공간 창(reg 1)을 매핑한다.
 *  4) 오류 IRQ 를 잡는다.
 *  5) 레지스터 잠금을 풀고, 슬레이브 접근을 모두 끄고, PCI 버스를 리셋에 넣는다.
 *     설정을 바꾸는 동안 버스가 살아 있으면 안 되기 때문이다.
 *  6) 재시도 허용, 로컬 버스 프로토콜, 버스 마스터를 설정한다.
 *  7) DT 의 ranges(아웃바운드)와 dma-ranges(인바운드)를 창 레지스터에 새긴다.
 *  8) PCI 쪽 동작 설정, FIFO 우선순위, 오류 인터럽트 허용을 정한다.
 *  9) Integrator 보드라면 보드 전용 초기화를 덧붙인다.
 * 10) 메모리 응답을 켜고, 인터럽트 허용 범위를 넓히고, 버스 리셋을 풀고,
 *     마지막으로 레지스터를 다시 잠근다.
 * 11) pci_host_probe() 로 코어에 넘겨 버스를 스캔하게 한다.
 *
 * 레지스터를 읽고-고쳐-쓰는 패턴이 반복되는 것은 이 칩의 제어 레지스터들이
 * 여러 기능 비트를 한 워드에 모아 두었기 때문이다. 여기서는 probe 가 단독으로
 * 실행되므로 그 사이에 끼어들 문맥이 없어 락이 필요 없다.
 *
 * 실행 컨텍스트: 드라이버 바인딩(프로세스 컨텍스트). Integrator 초기화에서
 * msleep(230) 으로 잠들 수 있다.
 *
 * [상류 코드 관찰] 네 가지.
 *  1. clk_prepare_enable() 으로 켠 클럭을 끄는 코드가 이 파일 어디에도 없다.
 *     devm_clk_get() 은 클럭 참조만 자동 반납하지 활성화를 되돌리지는 않으므로,
 *     이 아래 어느 단계에서 실패하든 클럭은 켜진 채 남는다.
 *  2. 설정공간 자원을 platform_get_resource() 로 얻고 NULL 검사 없이
 *     resource_size(regs) 로 역참조한다. DT 에 두 번째 MEM 자원이 없으면
 *     NULL 을 따라가게 된다.
 *  3. 칩이 보고하는 자기 물리 베이스가 매핑한 주소와 다를 때 오류 로그만 남기고
 *     그대로 진행한다.
 *  4. .remove 콜백이 없고 suppress_bind_attrs 로 언바인드도 막혀 있다. 즉 정상
 *     제거 경로가 존재하지 않으며, 그래서 위 1번이 실제 문제로 드러나지 않는다.
 *
 * 호출 체인:
 *   (플랫폼 버스의 DT 매칭) → [v3_pci_probe]
 *     → devm_pci_alloc_host_bridge(), clk_prepare_enable(),
 *       devm_platform_get_and_ioremap_resource(), devm_request_irq(),
 *       v3_pci_setup_resource(), v3_pci_parse_map_dma_ranges(),
 *       v3_integrator_init(), pci_host_probe()
 */
static int v3_pci_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 할당의 기준 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 컨트롤러의 DT 노드. 보드 판별과 dma-ranges 전달에 쓴다. */
	struct device_node *np = dev->of_node;
	/* [한국어] 자원 조회 결과를 담는 포인터. 레지스터 창과 설정공간 창에 재사용된다. */
	struct resource *regs;
	/* [한국어] 아웃바운드 창 목록 순회 커서. */
	struct resource_entry *win;
	/* [한국어] 브리지 뒤에 붙을 사설 객체. */
	struct v3_pci *v3;
	/* [한국어] 만들 호스트 브리지. */
	struct pci_host_bridge *host;
	/* [한국어] 브리지 동작에 필요한 클럭. 위 관찰 1 대로 끄는 경로가 없다. */
	struct clk *clk;
	/* [한국어] 16비트 레지스터를 읽고-고쳐-쓸 때 쓰는 임시 변수. 이 칩의 제어
	 * 레지스터가 대부분 16비트라 타입이 u16 이다. */
	u16 val;
	/* [한국어] 오류 인터럽트 번호. */
	int irq;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 브리지와 사설 영역을 한 번에 할당한다. 이 한 덩어리 할당 덕분에
	 * pci_host_bridge_priv() 와 pci_host_bridge_from_priv() 로 양방향 이동이 된다. */
	host = devm_pci_alloc_host_bridge(dev, sizeof(*v3));
	/* [한국어] 메모리 부족. */
	if (!host)
		return -ENOMEM;

	/* [한국어] 설정공간 접근 연산 표를 건다. 이 시점부터 코어가 config 접근을
	 * 이 파일의 함수들로 보낼 준비가 된다. */
	host->ops = &v3_pci_ops;
	/* [한국어] 브리지 뒤의 사설 영역을 가리킨다. */
	v3 = pci_host_bridge_priv(host);
	/* [한국어] 그 사설 영역을 sysdata 로도 걸어 둔다. v3_map_bus() 가 bus->sysdata
	 * 로 이 값을 되찾기 때문이다 — 버스마다 브리지의 sysdata 가 물려 내려간다. */
	host->sysdata = v3;
	/* [한국어] 로그와 자원 조회의 기준 device 를 심는다. */
	v3->dev = dev;

	/* Get and enable host clock */
	/* [한국어] 옆의 상류 주석대로 클럭을 얻는다. 이름 없이(NULL) 요청하므로 DT 에
	 * clocks 항목이 하나만 있다고 가정하는 것이다. */
	clk = devm_clk_get(dev, NULL);
	/* [한국어] 클럭이 없으면 브리지를 동작시킬 수 없다. */
	if (IS_ERR(clk)) {
		/* [한국어] 원인을 남기고 */
		dev_err(dev, "clock not found\n");
		return PTR_ERR(clk);
	}
	/* [한국어] 클럭을 준비시키고 켠다. 위 관찰 1 대로 이후 어느 실패 경로에서도
	 * 이것을 되돌리지 않는다. */
	ret = clk_prepare_enable(clk);
	/* [한국어] 클럭을 켤 수 없으면 진행할 수 없다. */
	if (ret) {
		/* [한국어] 원인을 남기고 */
		dev_err(dev, "unable to enable clock\n");
		return ret;
	}

	/* [한국어] DT reg 인덱스 0 = V3 칩 레지스터 창을 매핑하면서, 그 자원 서술도
	 * regs 로 받아 온다. 아래에서 물리 주소를 비교하는 데 그 서술이 필요하다. */
	v3->base = devm_platform_get_and_ioremap_resource(pdev, 0, &regs);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(v3->base))
		return PTR_ERR(v3->base);
	/*
	 * The hardware has a register with the physical base address
	 * of the V3 controller itself, verify that this is the same
	 * as the physical memory we've remapped it from.
	 */
	/* [한국어] 옆의 상류 주석대로, 칩이 스스로 보고하는 자기 물리 베이스와 우리가
	 * 매핑한 주소가 맞는지 확인한다. 이 레지스터가 담는 것이 주소의 상위 16비트라
	 * 비교 대상을 16비트 오른쪽으로 민다 — v3_integrator_init() 이 같은 레지스터에
	 * 0x6200 을 써 넣는 것과 짝을 이루는 해석이다. */
	if (readl(v3->base + V3_LB_IO_BASE) != (regs->start >> 16))
		/* [한국어] 어긋나면 알리기만 하고 계속 진행한다(위 관찰 3). DT 의 reg 가
		 * 잘못되었을 가능성을 알려 주는 진단이다. */
		dev_err(dev, "V3_LB_IO_BASE = %08x but device is @%pR\n",
			readl(v3->base + V3_LB_IO_BASE), regs);

	/* Configuration space is 16MB directly mapped */
	/* [한국어] 옆의 상류 주석대로 두 번째 MEM 자원이 16MB 설정공간 창이다.
	 * [상류 코드 관찰] 이 조회가 실패하면 NULL 이 돌아오는데 아래에서 검사 없이
	 * 역참조한다(위 관찰 2). */
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	/* [한국어] 크기가 정확히 16MB 여야 한다. v3_map_bus() 가 창 1 을 16MB 크기로
	 * 고정해 설정공간에 겨누기 때문에, 그보다 작거나 크면 접근이 어긋난다. */
	if (resource_size(regs) != SZ_16M) {
		/* [한국어] 이유를 남기고 */
		dev_err(dev, "config mem is not 16MB!\n");
		return -EINVAL;
	}
	/* [한국어] 창 레지스터에 새길 물리 주소를 보관한다. */
	v3->config_mem = regs->start;
	/* [한국어] 접근에 쓸 가상 주소를 얻는다. 물리와 가상을 둘 다 들고 있는 이유는
	 * struct v3_pci 의 config_mem 필드 설명 참조. */
	v3->config_base = devm_ioremap_resource(dev, regs);
	/* [한국어] 매핑 실패. */
	if (IS_ERR(v3->config_base))
		return PTR_ERR(v3->config_base);

	/* Get and request error IRQ resource */
	/* [한국어] 옆의 상류 주석대로 오류 IRQ 를 얻는다. 이 브리지는 인터럽트를 오직
	 * 오류 보고에만 쓴다 — 장치의 INTx 를 전달하는 도메인은 만들지 않는다. */
	irq = platform_get_irq(pdev, 0);
	/* [한국어] IRQ 가 없으면 오류를 알 수 없으므로 실패시킨다. */
	if (irq < 0)
		return irq;

	/* [한국어] 핸들러를 등록한다. 플래그 0 은 공유하지 않는 1차 핸들러라는 뜻이라,
	 * v3_irq() 가 하드 인터럽트 컨텍스트에서 돈다. 마지막 인자가 핸들러의 data 로
	 * 전달되어 컨트롤러 객체가 된다. devm 판이라 자동 해제된다. */
	ret = devm_request_irq(dev, irq, v3_irq, 0,
			"PCIv3 error", v3);
	/* [한국어] 등록 실패. */
	if (ret < 0) {
		/* [한국어] 어느 IRQ 에서 무슨 오류가 났는지 남기고 */
		dev_err(dev,
			"unable to request PCIv3 error IRQ %d (%d)\n",
			irq, ret);
		return ret;
	}

	/*
	 * Unlock V3 registers, but only if they were previously locked.
	 */
	/* [한국어] 위 상류 주석대로, 잠겨 있을 때만 잠금을 푼다. 잠금 비트를 먼저 확인하는
	 * 것은 이미 풀려 있는데 매직 값을 쓰면 상태가 어떻게 되는지 알 수 없기 때문으로
	 * 보이나, 그 근거는 데이터시트에 있고 이 트리에서 확인 못 함. */
	if (readw(v3->base + V3_SYSTEM) & V3_SYSTEM_M_LOCK)
		/* [한국어] 매직 값을 써야만 잠금이 풀린다. 이제부터 창 레지스터를 고칠 수 있다. */
		writew(V3_SYSTEM_UNLOCK, v3->base + V3_SYSTEM);

	/* Disable all slave access while we set up the windows */
	/* [한국어] 옆의 상류 주석대로, 창을 세우는 동안 슬레이브 접근을 모두 막는다.
	 * 먼저 현재 Command 값을 읽고 */
	val = readw(v3->base + V3_PCI_CMD);
	/* [한국어] IO 응답, 메모리 응답, 버스 마스터 세 비트를 지운다. 표준 PCI 상수를
	 * 쓰는 것이 이 레지스터가 PCI 표준 Command 임을 보여 준다. */
	val &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	/* [한국어] 고친 값을 되쓴다. 이제 이 브리지는 PCI 버스에 응답하지 않는다. */
	writew(val, v3->base + V3_PCI_CMD);

	/* Put the PCI bus into reset */
	/* [한국어] 옆의 상류 주석대로 PCI 버스를 리셋에 넣는다. 현재 값을 읽고 */
	val = readw(v3->base + V3_SYSTEM);
	/* [한국어] 리셋 출력 비트를 지운다 — 이 비트는 0 이 '리셋을 건다' 는 뜻이다. */
	val &= ~V3_SYSTEM_M_RST_OUT;
	/* [한국어] 되쓴다. 버스에 붙은 장치들이 리셋 상태로 들어간다. */
	writew(val, v3->base + V3_SYSTEM);

	/* Retry until we're ready */
	/* [한국어] 옆의 상류 주석대로 준비될 때까지 재시도하게 만든다. 현재 값을 읽고 */
	val = readw(v3->base + V3_PCI_CFG);
	/* [한국어] 재시도 허용 비트를 세운다. 타깃이 아직 준비되지 않았을 때 거래를
	 * 실패시키는 대신 다시 시도하게 하는 설정이다. */
	val |= V3_PCI_CFG_M_RETRY_EN;
	/* [한국어] 되쓴다. */
	writew(val, v3->base + V3_PCI_CFG);

	/* [한국어] 아래 네 줄은 옆의 상류 주석대로 로컬 버스 프로토콜을 정한다.
	 * 읽고-고치기를 네 번 한 뒤 마지막에 한 번만 쓰는 형태다. 현재 값을 읽는다. */
	/* Set up the local bus protocol */
	val = readw(v3->base + V3_LB_CFG);
	/* [한국어] 바이트 인에이블 신호를 입력으로 쓰도록 켠다(옆의 상류 주석). */
	val |= V3_LB_CFG_LB_BE_IMODE; /* Byte enable input */
	/* [한국어] 바이트 인에이블 신호를 출력으로도 쓰도록 켠다(옆의 상류 주석). */
	val |= V3_LB_CFG_LB_BE_OMODE; /* Byte enable output */
	/* [한국어] 엔디언 비트를 지워 리틀 엔디언으로 둔다(옆의 상류 주석).
	 * ARM 과 PCI 가 모두 리틀 엔디언이라 변환이 필요 없기 때문이다. */
	val &= ~V3_LB_CFG_LB_ENDIAN; /* Little endian */
	/* [한국어] PPC403Gx 용 준비 신호 방식을 끈다. 옆의 상류 TODO 주석이 그 칩에서는
	 * 1 로 두어야 한다고 남겨 두었다 — 즉 이 코드는 아직 ARM 만 상대한다. */
	val &= ~V3_LB_CFG_LB_PPC_RDY; /* TODO: when using on PPC403Gx, set to 1 */
	/* [한국어] 네 번의 수정을 한 번에 되쓴다. */
	writew(val, v3->base + V3_LB_CFG);

	/* Enable the PCI bus master */
	/* [한국어] 옆의 상류 주석대로 버스 마스터만 먼저 켠다. 현재 값을 읽고 */
	val = readw(v3->base + V3_PCI_CMD);
	/* [한국어] 마스터 비트를 세운다. 메모리 응답은 아직 켜지 않는다 — 창 설정이
	 * 끝난 뒤인 함수 뒷부분에서 켠다. */
	val |= PCI_COMMAND_MASTER;
	/* [한국어] 되쓴다. */
	writew(val, v3->base + V3_PCI_CMD);

	/* Get the I/O and memory ranges from DT */
	/* [한국어] 옆의 상류 주석대로 DT 에서 온 아웃바운드 창 목록을 훑는다.
	 * PCI 코어가 ranges 를 파싱해 host->windows 에 담아 둔 것이다. */
	resource_list_for_each_entry(win, &host->windows) {
		/* [한국어] 창 하나를 종류에 맞는 레지스터에 새긴다. */
		ret = v3_pci_setup_resource(v3, host, win);
		/* [한국어] 크기나 배치가 하드웨어 제약을 어기면 여기서 멈춘다. */
		if (ret) {
			/* [한국어] 원인을 남기고 */
			dev_err(dev, "error setting up resources\n");
			return ret;
		}
	}
	/* [한국어] 인바운드(장치 DMA) 창도 새긴다. */
	ret = v3_pci_parse_map_dma_ranges(v3, np);
	/* [한국어] 실패하면 DMA 가 불가능하므로 진행하지 않는다. */
	if (ret)
		return ret;

	/*
	 * Disable PCI to host IO cycles, enable I/O buffers @3.3V,
	 * set AD_LOW0 to 1 if one of the LB_MAP registers choose
	 * to use this (should be unused).
	 */
	/* [한국어] 위 상류 주석대로 PCI → 호스트 IO 사이클을 막는다. IO 베이스에 0 을
	 * 써서 PCI 쪽에서 이 칩의 IO 창이 보이지 않게 한다. */
	writel(0x00000000, v3->base + V3_PCI_IO_BASE);
	/* [한국어] PCI 동작 설정 값을 새로 조립한다 — 읽고-고치기가 아니라 통째로
	 * 덮어쓰는 방식이라, 여기 열거되지 않은 비트는 모두 0 이 된다.
	 * 레지스터 IO 접근 금지, IO 응답 금지, 3.3V 버퍼, 그리고 상류 주석이 말한
	 * AD_LOW0 을 켠다. */
	val = V3_PCI_CFG_M_IO_REG_DIS | V3_PCI_CFG_M_IO_DIS |
		V3_PCI_CFG_M_EN3V | V3_PCI_CFG_M_AD_LOW0;
	/*
	 * DMA read and write from PCI bus commands types
	 */
	/* [한국어] 옆의 상류 주석대로 DMA 읽기에 쓸 PCI 명령 타입을 자리에 맞춰 넣는다. */
	val |=  V3_PCI_CFG_TYPE_DEFAULT << V3_PCI_CFG_M_RTYPE_SHIFT;
	/* [한국어] DMA 쓰기에 쓸 명령 타입도 같은 값으로 넣는다. */
	val |=  V3_PCI_CFG_TYPE_DEFAULT << V3_PCI_CFG_M_WTYPE_SHIFT;
	/* [한국어] 조립한 값을 한 번에 쓴다. */
	writew(val, v3->base + V3_PCI_CFG);

	/*
	 * Set the V3 FIFO such that writes have higher priority than
	 * reads, and local bus write causes local bus read fifo flush
	 * on aperture 1. Same for PCI.
	 */
	/* [한국어] 위 상류 주석대로 FIFO 우선순위를 정한다. 네 개의 읽기 FIFO 를 모두
	 * aperture 1 접근에서 비우도록 설정하는데, 이는 쓰기를 읽기보다 앞세우면서도
	 * 순서가 뒤집히지 않게 하려는 조합이다. 여기서도 읽고-고치기 없이 통째로 쓴다. */
	writew(V3_FIFO_PRIO_LB_RD1_FLUSH_AP1 |
	       V3_FIFO_PRIO_LB_RD0_FLUSH_AP1 |
	       V3_FIFO_PRIO_PCI_RD1_FLUSH_AP1 |
	       V3_FIFO_PRIO_PCI_RD0_FLUSH_AP1,
	       v3->base + V3_FIFO_PRIORITY);


	/*
	 * Clear any error interrupts, and enable parity and write error
	 * interrupts
	 */
	/* [한국어] 위 상류 주석대로, 인터럽트를 켜기 전에 남아 있던 로컬 버스 인터럽트
	 * 상태를 지운다. 순서가 반대면 부팅 직후 가짜 인터럽트가 뜬다. */
	writeb(0, v3->base + V3_LB_ISTAT);
	/* [한국어] 로컬 버스 설정을 읽어 */
	val = readw(v3->base + V3_LB_CFG);
	/* [한국어] 인터럽트 허용 비트를 세우고 */
	val |= V3_LB_CFG_LB_LB_INT;
	/* [한국어] 되쓴다. 이제 브리지가 인터럽트를 낼 수 있다. */
	writew(val, v3->base + V3_LB_CFG);
	/* [한국어] 마스크에 쓰기 어보트와 패리티 오류만 먼저 허용한다. 읽기 어보트는
	 * 아직 넣지 않는데, 버스 스캔 중 빈 슬롯을 읽으면 정상적으로 발생하기 때문이다
	 * — 함수 뒷부분에서 리셋을 풀기 직전에야 읽기 어보트도 허용한다. */
	writeb(V3_LB_ISTAT_PCI_WR | V3_LB_ISTAT_PCI_PERR,
	       v3->base + V3_LB_IMASK);

	/* Special Integrator initialization */
	/* [한국어] 옆의 상류 주석대로, Integrator/AP 보드일 때만 보드 전용 초기화를 한다.
	 * DT compatible 로 판별하므로 다른 보드에서는 통째로 건너뛴다. */
	if (of_device_is_compatible(np, "arm,integrator-ap-pci")) {
		/* [한국어] 보드의 시스템 컨트롤러를 통해 브리지를 리셋에서 꺼낸다. */
		ret = v3_integrator_init(v3);
		/* [한국어] 실패하면 브리지가 동작하지 않으므로 멈춘다. */
		if (ret)
			return ret;
	}

	/* Post-init: enable PCI memory and invalidate (master already on) */
	/* [한국어] 옆의 상류 주석대로, 창이 모두 준비되었으니 이제 메모리 응답을 켠다.
	 * 현재 Command 값을 읽고 */
	val = readw(v3->base + V3_PCI_CMD);
	/* [한국어] 메모리 응답과 캐시 라인 무효화 명령 사용을 켠다. 마스터는 앞에서
	 * 이미 켜 두었다. */
	val |= PCI_COMMAND_MEMORY | PCI_COMMAND_INVALIDATE;
	/* [한국어] 되쓴다. */
	writew(val, v3->base + V3_PCI_CMD);

	/* Clear pending interrupts */
	/* [한국어] 옆의 상류 주석대로 그동안 쌓인 인터럽트를 다시 한 번 지운다.
	 * 창 설정 과정에서 새로 생겼을 수 있기 때문이다. */
	writeb(0, v3->base + V3_LB_ISTAT);
	/* Read or write errors and parity errors cause interrupts */
	/* [한국어] 옆의 상류 주석대로 이제 읽기 어보트까지 포함해 세 가지 오류를 모두
	 * 인터럽트로 받는다. 앞의 마스크 설정보다 한 비트가 늘었다. */
	writeb(V3_LB_ISTAT_PCI_RD | V3_LB_ISTAT_PCI_WR | V3_LB_ISTAT_PCI_PERR,
	       v3->base + V3_LB_IMASK);

	/* Take the PCI bus out of reset so devices can initialize */
	/* [한국어] 옆의 상류 주석대로 PCI 버스를 리셋에서 꺼낸다. 현재 값을 읽고 */
	val = readw(v3->base + V3_SYSTEM);
	/* [한국어] 리셋 출력 비트를 세운다 — 1 이 '리셋을 푼다' 는 뜻이다. */
	val |= V3_SYSTEM_M_RST_OUT;
	/* [한국어] 되쓴다. 이제 버스에 붙은 장치들이 초기화를 시작한다. */
	writew(val, v3->base + V3_SYSTEM);

	/*
	 * Re-lock the system register.
	 */
	/* [한국어] 위 상류 주석대로 레지스터를 다시 잠근다. 현재 값을 읽고 */
	val = readw(v3->base + V3_SYSTEM);
	/* [한국어] 잠금 비트를 세우고 */
	val |= V3_SYSTEM_M_LOCK;
	/* [한국어] 되쓴다. 이 뒤로는 창 레지스터가 실수로 바뀌지 않는다.
	 * 다만 v3_map_bus() 는 config 접근마다 창 레지스터를 고치는데, 그 동작이
	 * 잠금과 어떻게 공존하는지는 데이터시트가 필요해 이 트리에서 확인 못 함. */
	writew(val, v3->base + V3_SYSTEM);

	/* [한국어] 준비가 끝났으니 코어에 넘긴다. 이 안에서 버스가 스캔되고, 그 스캔의
	 * 모든 설정공간 접근이 이 파일의 map/read/write/unmap 조합을 지난다. */
	return pci_host_probe(host);
}

/* [한국어] DT compatible 문자열 매칭 표. 이 드라이버가 상대하는 하드웨어는
 * 하나뿐이다. */
static const struct of_device_id v3_pci_of_match[] = {
	/* [한국어] 항목 하나의 시작. */
	{
		/* [한국어] V360EPC 는 V3 Semiconductor 의 PCI 브리지 칩 이름이다. 세대별 분기가
		 * 없으므로 .data 도 달려 있지 않다 — pcie-apple.c 가 세대 상수표를 .data 에
		 * 거는 것과 대비된다. */
		.compatible = "v3,v360epc-pci",
	},
	/* [한국어] 표의 끝을 알리는 빈 항목. 이것이 없으면 매칭 코드가 배열을 넘어간다. */
	{},
};

/* [한국어] 플랫폼 드라이버 서술. 이 브리지는 보드에 붙박이인 칩이라 PCI 장치가
 * 아니라 플랫폼 장치로 다뤄진다. */
static struct platform_driver v3_pci_driver = {
	/* [한국어] 드라이버 속성 묶음. */
	.driver = {
		/* [한국어] 드라이버 이름. sysfs 경로와 로그에 쓰인다. */
		.name = "pci-v3-semi",
		/* [한국어] 위 DT 매칭 표를 건다. */
		.of_match_table = v3_pci_of_match,
		/* [한국어] sysfs 로 수동 바인드/언바인드하는 속성을 만들지 않는다. 이 드라이버에
		 * remove 경로가 없고 클럭·창 설정을 되돌리는 코드도 없으므로, 언바인드를 아예
		 * 막는 것이 안전하기 때문이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] 바인딩 시 불릴 진입점. */
	.probe  = v3_pci_probe,
};
/* [한국어] 커널에 내장되는 드라이버로 등록한다. module_platform_driver 가 아니라
 * builtin_ 판인 것은 Kconfig 의 PCI_V3_SEMI 가 tristate 가 아니라 bool 이기
 * 때문이다(drivers/pci/controller/Kconfig:323~324). 그래서 이 파일에는
 * MODULE_LICENSE 나 MODULE_DEVICE_TABLE 같은 모듈용 매크로가 없다 —
 * pcie-apple.c 가 그것들을 갖고 있는 것과 대비된다. */
builtin_platform_driver(v3_pci_driver);
