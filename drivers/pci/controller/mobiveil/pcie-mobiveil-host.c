// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Mobiveil PCIe Host controller
 *
 * Copyright (c) 2018 Mobiveil Inc.
 * Copyright 2019-2020 NXP
 *
 * Author: Subrahmanya Lingappa <l.subrahmanya@mobiveil.co.in>
 *	   Hou Zhiqiang <Zhiqiang.Hou@nxp.com>
 */

/*
 * [한국어 설명] Mobiveil PCIe IP 의 호스트 브리지 계층 (pcie-mobiveil-host.c)
 *
 * === 파일의 역할 ===
 * 이 디렉터리 세 층 가운데 가운데 층이다. 아래층(pcie-mobiveil.c)이
 * 레지스터와 주소 창을 다룬다면, 이 파일은 그것을 재료로 '리눅스가 아는
 * PCI 호스트 브리지'를 만들어 낸다. 하는 일은 크게 넷이다.
 *
 * 1) config 공간 접근. PCI 코어에 넘길 pci_ops 를 제공한다. 특이한 점은
 *    이 IP 에 config 전용 주소 창이 따로 없다는 것이다. 대신 아웃바운드
 *    0번 창을 CFG 종류로 잡아 두고, 접근할 때마다 그 창의 목적지
 *    레지스터에 대상 BDF 를 실어 조준을 바꾼다. 그래서 config 접근 한 번이
 *    '창 조준 쓰기 + 실제 접근' 두 단계가 되고, 그 사이의 원자성은
 *    PCI 코어의 접근 락에 전적으로 의존한다.
 *
 * 2) 호스트 초기화. 버스 번호, command 레지스터, 두 종류의 PIO,
 *    config 창과 기본 인바운드 창, DT ranges 에서 온 아웃바운드 창들,
 *    그리고 클래스 코드까지를 mobiveil_host_init() 한 함수가 세운다.
 *    이 함수는 리셋 복구에서도 다시 불리므로 재진입 가능하게 쓰여 있다.
 *
 * 3) 인터럽트. INTx 네 핀과 MSI 16벡터가 상위 인터럽트 컨트롤러의
 *    단 한 선으로 올라온다. 그래서 체인 핸들러 하나가 두 종류의 상태
 *    레지스터를 모두 확인하고 각각의 도메인으로 나눠 보낸다.
 *    INTx 는 직접 만든 irq_chip 으로, MSI 는 부모 도메인으로 처리한다.
 *
 * 4) 갈림길. 인터럽트 초기화만은 SoC 가 대신할 수 있게 콜백을 둔다.
 *    MSI 를 외부 컨트롤러에 맡기는 SoC(Layerscape Gen4)가 그 경로를 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   SoC 층   pcie-layerscape-gen4.c / pcie-mobiveil-plat.c
 *              — probe 에서 struct mobiveil_pcie 를 채워 넘긴다
 *                |
 *                v
 *   호스트 층 [이 파일]  mobiveil_pcie_host_probe()
 *                |         1. mobiveil_pcie_parse_dt()
 *                |         2. mobiveil_pcie_is_bridge()
 *                |         3. mobiveil_host_init()
 *                |         4. mobiveil_pcie_interrupt_init()
 *                |         5. bridge->sysdata / bridge->ops 설정
 *                |         6. mobiveil_bringup_link()
 *                |         7. pci_host_probe()
 *                v
 *   레지스터 층  pcie-mobiveil.c
 *                — mobiveil_csr_read/write, program_ob/ib_windows,
 *                  mobiveil_bringup_link
 *
 * 런타임에는 두 경로가 따로 돈다.
 *
 *   config 접근 : PCI 코어 → pci_generic_config_read/write
 *                   → mobiveil_pcie_map_bus() → BDF 창 조준 → MMIO
 *   인터럽트    : 상위 IRQ 컨트롤러 → mobiveil_pcie_isr()
 *                   → INTx 도메인 또는 MSI 도메인 → 장치 핸들러
 *
 * 실행 컨텍스트가 셋 섞인다. probe 와 도메인 할당은 프로세스 컨텍스트이고,
 * ISR 과 INTx 마스크·언마스크는 인터럽트 컨텍스트다. config 접근은
 * 코어의 락 안에서 어느 쪽에서든 일어날 수 있다.
 * 이 파일이 부르는 mobiveil_csr_ 접근자들은 내부적으로 PAB_CTRL 페이지
 * 선택 쓰기를 동반하는데, 그 두 단계 사이의 상호 배제는 이 파일에도
 * 아래층에도 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 두 SoC 드라이버가 mobiveil_pcie_host_probe() 를 부른다.
 *   그리고 pcie-layerscape-gen4.c 는 mobiveil_host_init() 을 리셋 복구에서
 *   직접 다시 부른다.
 * 아래쪽: pcie-mobiveil.c 의 레지스터 접근자와 창 설정 함수, 링크 대기.
 * 옆쪽: PCI 코어(pci_host_probe, pci_generic_config_read/write),
 *   IRQ 도메인 코어, 그리고 irq-msi-lib.
 *
 * 데이터 흐름. DT 의 두 리소스가 mobiveil_pcie_parse_dt() 를 거쳐
 * 가상 주소가 되고, DT 의 ranges 는 PCI 코어가 파싱해 bridge->windows 로
 * 넘어온 뒤 mobiveil_host_init() 에서 아웃바운드 창이 된다.
 * 반대 방향으로는 PAB 인터럽트 상태 비트와 MSI FIFO 가 ISR 을 거쳐
 * 두 IRQ 도메인으로 흘러 장치 핸들러에 닿는다.
 *
 * 공유 상태는 struct mobiveil_pcie 하나다. bridge->sysdata 로도,
 * irq_desc 의 handler_data 로도, irq_chip_data 로도, 도메인의 host_data 로도
 * 같은 포인터가 심어져 있어 어느 경로에서든 되찾을 수 있다.
 *
 * === NVMe 관점 ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 부르지 않는다(트리 전수 확인).
 * 관계는 토폴로지상의 것이지만, 그 관계가 가장 짙은 파일이 여기다.
 *
 *   - NVMe 컨트롤러를 열거하고 BAR 를 읽는 config 접근이 전부
 *     mobiveil_pcie_map_bus() 를 지난다. 접근 하나에 BDF 쓰기가 하나씩 붙는다.
 *   - NVMe 의 doorbell 쓰기와 완료 큐 DMA 는 mobiveil_host_init() 이
 *     잡아 둔 아웃바운드·인바운드 창을 지난다. 인바운드 창은 0 에서
 *     시작하는 256GB 항등 매핑이라 DMA 주소가 곧 시스템 물리 주소다.
 *   - NVMe 의 완료 인터럽트(MSI/MSI-X)는 내장 경로에서라면
 *     mobiveil_pcie_isr() 의 MSI FIFO 루프를 지난다. 다만 벡터가 16개뿐이라,
 *     CPU 마다 큐를 하나씩 두는 NVMe 의 통상적인 구성에서는 벡터가
 *     일찍 바닥날 수 있다. 그때 pci_alloc_irq_vectors 는 더 적은 수로
 *     물러나게 된다.
 *   - 아웃바운드 창도 apio_wins(기본 8) 개로 제한된다. config 창이
 *     하나를 이미 쓰므로 DT ranges 가 많으면 남는 창이 더 줄어든다.
 *
 * 이들은 모두 이 파일의 코드에서 직접 읽어낸 제약이며, NVMe 쪽 코드와의
 * 호출 관계는 존재하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * mobiveil_pcie_host_probe() : 이 파일의 진입점. 위 7단계를 순서대로 진행한다.
 * mobiveil_pcie_map_bus()    : config 접근 주소를 만든다. EP 접근이면
 *                              창의 목적지 레지스터에 BDF 를 싣는다.
 * mobiveil_pcie_valid_device(): 존재할 수 없는 (버스, devfn) 조합을 걸러낸다.
 * mobiveil_host_init()       : 창·PIO·버스 번호·클래스 코드를 세운다.
 *                              리셋 복구에서 reinit=true 로 다시 불린다.
 * mobiveil_pcie_isr()        : INTx 와 MSI 를 함께 처리하는 체인 핸들러.
 * mobiveil_mask_intx_irq() / _unmask_ : INTx 활성화 비트를 락 아래에서 갱신.
 * mobiveil_irq_msi_domain_alloc() / _free() : 16벡터 비트맵 관리.
 * mobiveil_pcie_interrupt_init() : SoC 콜백과 내장 경로의 갈림길.
 * mobiveil_pcie_integrated_interrupt_init() : 내장 INTx/MSI 경로 전체 구성.
 *
 * === 이 파일을 읽을 때 알아 둘 점 ===
 * 코드에서 확인되는, 설명이 필요한 지점이 둘 있다. 해당 줄 주석에 근거와
 * 함께 자세히 적어 두었고 여기서는 위치만 밝힌다.
 *   - mobiveil_pcie_interrupt_init(): rp->ops 자체의 NULL 검사 없이
 *     rp->ops->interrupt_init 을 읽는다. 이 트리에서 rp.ops 를 채우는 곳은
 *     pcie-layerscape-gen4.c 한 군데뿐이고, pcie-mobiveil-plat.c 는 채우지
 *     않은 채 같은 경로로 들어온다.
 *   - mobiveil_pcie_isr(): INTx hwirq 로 bit + 1(1~4)을 넘기는데,
 *     intx_domain 은 크기 PCI_NUM_INTX(4)로 만들어져 유효 hwirq 가 0~3 이다.
 * 둘 다 하드웨어 사정이나 트리 밖 근거가 있어야 옳고 그름을 판단할 수 있어,
 * 여기서는 관찰된 사실과 그 근거만 적는다.
 */

/* [한국어] __init 등 초기화 섹션 지정자. */
#include <linux/init.h>
/* [한국어] irqreturn_t, IRQ 요청 API. 이 파일은 체인 핸들러를 쓰지만 인터럽트
 * 관련 기본 타입이 여기서 온다. */
#include <linux/interrupt.h>
/* [한국어] struct irq_chip, irq_set_chip_and_handler, handle_level_irq 등
 * irqchip 을 직접 구현하는 데 필요한 것들. 이 파일은 INTx 용 irq_chip 을 만든다. */
#include <linux/irq.h>
/* [한국어] msi_lib_init_dev_msi_info 와 MSI 부모 도메인 헬퍼.
 * Kconfig 의 PCIE_MOBIVEIL_HOST 가 IRQ_MSI_LIB 를 select 하는 이유가 이것이다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] chained_irq_enter / chained_irq_exit. 이 컨트롤러의 인터럽트는 상위
 * 인터럽트 컨트롤러의 한 선에 매달린 체인 구조라, 진입·이탈 시
 * 상위 chip 을 마스크하고 EOI 를 보내야 한다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] irq_domain_create_linear, generic_handle_domain_irq, irq_domain_set_info 등
 * IRQ 도메인 API. INTx 도메인과 MSI 도메인을 모두 이 API 로 만든다. */
#include <linux/irqdomain.h>
/* [한국어] 기본 커널 매크로. */
#include <linux/kernel.h>
/* [한국어] 모듈 관련 매크로. 이 파일은 라이브러리 성격이라 모듈 메타데이터를 두지
 * 않지만 상류가 포함해 둔 것을 그대로 둔다. */
#include <linux/module.h>
/* [한국어] struct msi_msg, struct msi_parent_ops, MSI_FLAG_ 상수들. */
#include <linux/msi.h>
/* [한국어] of_property_read_u32 등 DT 속성 읽기 헬퍼. apio-wins / ppio-wins 를 읽는다. */
#include <linux/of_pci.h>
/* [한국어] PCI_COMMAND, PCI_PRIMARY_BUS, PCI_HEADER_TYPE 같은 config 공간 상수와
 * pci_generic_config_read / write, pci_host_probe 같은 코어 API. */
#include <linux/pci.h>
/* [한국어] platform 리소스와 IRQ 획득 API. */
#include <linux/platform_device.h>
/* [한국어] 슬랩 할당자 선언. 이 파일에서 직접 kmalloc 계열을 부르는 곳은 보이지
 * 않으나 상류가 넣어 둔 것을 그대로 둔다. */
#include <linux/slab.h>

/* [한국어] 이 디렉터리 공통 헤더 — struct mobiveil_pcie 와 PAB_ 레지스터 상수,
 * 그리고 mobiveil_csr_ 접근자 래퍼. */
#include "pcie-mobiveil.h"

/* [한국어] mobiveil_pcie_valid_device - 이 (버스, devfn) 조합에 config 접근을 해도 되는지 판정한다.
 * 
 * @bus: 접근하려는 PCI 버스.
 * @devfn: 장치·기능 번호(상위 5비트 device, 하위 3비트 function).
 * @return: 접근해도 되면 true, 아니면 false(호출자가 NULL 을 돌려 접근을 막는다).
 * 
 * 왜 필요한가: PCIe 는 포인트 투 포인트라 한 링크 아래에는 장치가 하나뿐이다.
 * 그런데 PCI 열거는 버스마다 device 0~31 을 모두 훑으므로, 걸러 주지 않으면
 * 존재하지 않는 장치에 대한 config 접근이 하드웨어로 나간다.
 * 그 결과는 보통 타임아웃이나 오류이고, 일부 구현에서는 잘못된 장치가
 * 중복으로 보이기도 한다.
 * 
 * 동작: 두 가지를 막는다.
 *   1) 루트 버스에서는 devfn 0 만 허용한다 — 루트 포트 자신뿐이다.
 *   2) RC 에 직접 붙은 버스에서는 device 0 만 허용한다 — 링크 반대편의
 *      장치 하나뿐이다. function 은 걸러내지 않으므로 다기능 장치는 통과한다.
 * 
 * 실행 컨텍스트: config 접근 경로. 열거 시점의 프로세스 컨텍스트가 대부분이지만,
 * config 접근은 다른 컨텍스트에서도 일어날 수 있다.
 * 호출자: mobiveil_pcie_map_bus().
 * 피호출자: pci_is_root_bus(), PCI_SLOT(), to_pci_host_bridge().
 * 에러 경로: 오류를 만들지 않고 false 만 돌려준다.
 * 
 * 호출 체인:
 *   PCI 코어 config 접근 → pci_generic_config_read/write
 *     → mobiveil_pcie_map_bus() → [이 함수] */
static bool mobiveil_pcie_valid_device(struct pci_bus *bus, unsigned int devfn)
{
	/* Only one device down on each root port */
	/* [한국어] 루트 버스(버스 0)에서는 루트 포트 자신인 devfn 0 만 유효하다.
	 * 그 외의 devfn 은 존재할 수 없으므로 접근을 막는다. */
	if (pci_is_root_bus(bus) && (devfn > 0))
		/* [한국어] 유효하지 않음. 호출자가 NULL 주소를 돌려주고 코어는 이를
		 * '장치 없음'으로 다룬다. */
		return false;

	/*
	 * Do not read more than one device on the bus directly
	 * attached to RC
	 */
	/* [한국어] 이 버스의 상위(primary) 버스 번호가 호스트 브리지의 시작 버스 번호와
	 * 같다면, 이 버스는 RC 에 직접 붙은 첫 버스라는 뜻이다.
	 * 그런 버스에는 링크 반대편 장치 하나만 있을 수 있으므로 device 번호가
	 * 0 이 아닌 접근을 막는다. PCI_SLOT 은 devfn 에서 상위 5비트(device)를 뽑는다.
	 * function 은 검사하지 않으므로 다기능 장치의 fn1~7 은 그대로 통과한다. */
	if ((bus->primary == to_pci_host_bridge(bus->bridge)->busnr) && (PCI_SLOT(devfn) > 0))
		/* [한국어] 유효하지 않음. */
		return false;

	/* [한국어] 위 두 조건에 걸리지 않았으면 접근을 허용한다. */
	return true;
}

/*
 * mobiveil_pcie_map_bus - routine to get the configuration base of either
 * root port or endpoint
 */
/* [한국어] mobiveil_pcie_map_bus - config 접근이 향할 MMIO 주소를 만든다.
 * 
 * @bus: 접근 대상 버스.
 * @devfn: 접근 대상 장치·기능 번호.
 * @where: config 공간 안의 레지스터 오프셋.
 * @return: 접근할 __iomem 주소. 유효하지 않은 대상이면 NULL.
 * 
 * 왜 필요한가: pci_generic_config_read / write 는 '주소를 만드는 일'만
 * 드라이버에 맡기고 실제 읽고 쓰기는 자기가 한다. 이 함수가 그 주소 계산이다.
 * 
 * 동작: 대상이 루트 버스면 RC 자신의 레지스터 영역(csr_axi_slave)이 곧
 * config 공간이므로 거기에 오프셋만 더한다.
 * 그 아래 장치라면 아웃바운드 0번 창이 config 창으로 잡혀 있으므로,
 * 그 창의 PCIe 쪽 목적지 레지스터에 대상 BDF 를 실어 창의 조준점을 바꾼 뒤
 * config 창의 가상 주소를 돌려준다.
 * 
 * 여기가 이 드라이버에서 가장 미묘한 곳이다. 주소를 돌려준 뒤 실제 접근이
 * 일어나기까지, 다른 CPU 가 같은 창의 BDF 를 바꾸면 엉뚱한 장치에 접근하게
 * 된다. 이 함수는 락을 잡지 않고 PCI 코어의 config 접근 직렬화에 기댄다 —
 * 바로 위 상류 주석의 'Relies on pci_lock serialization' 이 그 뜻이다.
 * 
 * 실행 컨텍스트: config 접근 경로. 코어가 잡은 락 안에서 불린다.
 * 호출자: pci_generic_config_read() / pci_generic_config_write()
 *   (아래 mobiveil_pcie_ops 를 통해).
 * 피호출자: mobiveil_pcie_valid_device(), mobiveil_csr_writel().
 * 에러 경로: 유효하지 않은 대상이면 NULL 을 돌려주고, 코어는 이를
 *   '장치 없음'으로 처리한다.
 * 
 * 호출 체인:
 *   PCI 코어 → pci_generic_config_read/write → [이 함수]
 *     → mobiveil_csr_writel() → mobiveil_csr_write() */
static void __iomem *mobiveil_pcie_map_bus(struct pci_bus *bus,
					   unsigned int devfn, int where)
{
	/* [한국어] PCI 코어는 버스의 sysdata 로 컨트롤러 문맥을 되돌려 준다.
	 * 이 값은 mobiveil_pcie_host_probe() 가 bridge->sysdata 에 넣어 둔 것이다. */
	struct mobiveil_pcie *pcie = bus->sysdata;
	/* [한국어] 루트 포트 상태 — EP config 창의 가상 주소가 여기 있다. */
	struct mobiveil_root_port *rp = &pcie->rp;
	/* [한국어] 조립할 BDF 값을 담을 변수. */
	u32 value;

	/* [한국어] 존재할 수 없는 대상이면 하드웨어에 접근조차 하지 않는다. */
	if (!mobiveil_pcie_valid_device(bus, devfn))
		/* [한국어] NULL 을 돌려주면 코어가 접근을 포기하고 장치 없음으로 다룬다. */
		return NULL;

	/* RC config access */
	/* [한국어] 루트 버스, 즉 RC 자신에 대한 접근인 경우. */
	if (pci_is_root_bus(bus))
		/* [한국어] RC 의 config 헤더는 csr_axi_slave 영역 앞부분에 그대로 놓여 있다.
		 * 창 조작이 필요 없으므로 베이스에 오프셋만 더해 돌려준다.
		 * 같은 이유로 mobiveil_csr_read/write 도 이 영역으로 config 헤더를 다룬다. */
		return pcie->csr_axi_slave_base + where;

	/*
	 * EP config access (in Config/APIO space)
	 * Program PEX Address base (31..16 bits) with appropriate value
	 * (BDF) in PAB_AXI_AMAP_PEX_WIN_L0 Register.
	 * Relies on pci_lock serialization
	 */
	/* [한국어] 대상 BDF 를 창 레지스터가 요구하는 배치로 조립한다.
	 * 버스는 24비트, device 는 19비트, function 은 16비트 자리로 간다. */
	value = bus->number << PAB_BUS_SHIFT |
		/* [한국어] PCI_SLOT 이 devfn 에서 device 번호(상위 5비트)를 뽑는다. */
		PCI_SLOT(devfn) << PAB_DEVICE_SHIFT |
		/* [한국어] PCI_FUNC 이 devfn 에서 function 번호(하위 3비트)를 뽑는다. */
		PCI_FUNC(devfn) << PAB_FUNCTION_SHIFT;

	/* [한국어] 아웃바운드 0번 창의 PCIe 쪽 목적지 레지스터에 BDF 를 쓴다.
	 * 이 창은 mobiveil_host_init() 이 CFG_WINDOW_TYPE 으로 잡아 둔 것이라,
	 * 이 쓰기 하나로 '앞으로의 config 접근이 향할 장치'가 바뀐다.
	 * 주의: 이 쓰기와 뒤이은 실제 config 접근 사이가 원자적이지 않다.
	 * 직렬화는 PCI 코어의 config 접근 락에 전적으로 의존한다. */
	mobiveil_csr_writel(pcie, value, PAB_AXI_AMAP_PEX_WIN_L(WIN_NUM_0));

	/* [한국어] 조준을 마쳤으니 config 창의 가상 주소에 레지스터 오프셋을 더해 돌려준다.
	 * 실제 읽고 쓰기는 호출자인 pci_generic_config_read/write 가 한다. */
	return rp->config_axi_slave_base + where;
}

/* [한국어] PCI 코어에 넘길 config 접근 연산 표.
 * mobiveil_pcie_host_probe() 가 bridge->ops 에 걸어 준다. */
static struct pci_ops mobiveil_pcie_ops = {
	/* [한국어] 주소 계산만 이 드라이버가 맡는다. */
	.map_bus = mobiveil_pcie_map_bus,
	/* [한국어] 읽기는 코어의 범용 구현을 그대로 쓴다 — map_bus 가 돌려준 주소에
	 * 크기에 맞는 MMIO 읽기를 하고 접근 락을 관리한다. */
	.read = pci_generic_config_read,
	/* [한국어] 쓰기도 마찬가지다. 이 '주소 계산만 위임' 구조 덕분에
	 * BDF 조작과 실제 접근이 같은 락 안에서 일어난다. */
	.write = pci_generic_config_write,
};

/* [한국어] mobiveil_pcie_isr - INTx 와 MSI 를 함께 처리하는 체인 인터럽트 핸들러.
 * 
 * @desc: 이 체인 핸들러가 매달린 상위 IRQ 의 서술자.
 *   handler_data 로 struct mobiveil_pcie 가 들어 있다.
 * @return: 없음(체인 핸들러는 반환값이 없다).
 * 
 * 왜 필요한가: 이 IP 는 INTx 네 핀과 MSI 를 상위 인터럽트 컨트롤러의 단 한 선으로
 * 올린다(상류 주석이 그 사실을 밝히고 있다). 그래서 하나의 핸들러가 두 종류의
 * 상태 레지스터를 모두 확인하고 각각의 도메인으로 나눠 보내야 한다.
 * 
 * 동작 단계:
 *   1) chained_irq_enter 로 상위 chip 을 마스크한다.
 *   2) 상태와 활성화 레지스터를 AND 해서 '켜 둔 소스 중 실제로 온 것'을 얻는다.
 *   3) INTx 비트가 있으면, 남은 비트가 없어질 때까지 돌며 각 핀을 도메인에 넘기고
 *      그때마다 해당 상태 비트를 지운다.
 *   4) MSI FIFO 가 빌 때까지 데이터와 주소를 꺼내며 도메인에 넘긴다.
 *   5) 처음에 latch 한 상태 비트를 되써서 지우고 chained_irq_exit 로 이탈한다.
 * 
 * 실행 컨텍스트: 인터럽트 컨텍스트. 여기서 부르는 mobiveil_csr_ 접근자들이
 * 내부적으로 PAB_CTRL 페이지 선택 쓰기를 동반하는데, 초기화 경로도 같은
 * 레지스터를 쓰므로 그 사이의 상호 배제는 이 파일에 없다.
 * 호출자: 커널 IRQ 처리부(irq_set_chained_handler_and_data 로 등록됨).
 * 피호출자: chained_irq_enter/exit, mobiveil_csr_readl/writel,
 *   readl_relaxed, generic_handle_domain_irq.
 * 에러 경로: 도메인이 hwirq 를 풀지 못하면 generic_handle_domain_irq 가
 *   음수를 돌려주고, 그때만 dev_err_ratelimited 로 남긴다.
 * 
 * 호출 체인:
 *   상위 IRQ 컨트롤러 → 커널 IRQ 처리부 → [이 함수]
 *     → generic_handle_domain_irq() → 장치 드라이버의 핸들러 */
static void mobiveil_pcie_isr(struct irq_desc *desc)
{
	/* [한국어] 상위 IRQ 를 담당하는 irq_chip. 체인 진입·이탈에서 마스크와 EOI 에 쓴다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] irq_set_chained_handler_and_data 로 함께 등록해 둔 컨트롤러 문맥을 되찾는다. */
	struct mobiveil_pcie *pcie = irq_desc_get_handler_data(desc);
	/* [한국어] 오류 로그 대상. */
	struct device *dev = &pcie->pdev->dev;
	/* [한국어] INTx 도메인이 들어 있는 루트 포트 상태. */
	struct mobiveil_root_port *rp = &pcie->rp;
	/* [한국어] MSI 도메인이 들어 있는 MSI 상태 묶음. */
	struct mobiveil_msi *msi = &rp->msi;
	/* [한국어] MSI FIFO 에서 꺼낼 데이터와 주소 상하위를 담을 변수들. */
	u32 msi_data, msi_addr_lo, msi_addr_hi;
	/* [한국어] PAB 인터럽트 상태와 MSI 상태를 담을 변수들. */
	u32 intr_status, msi_status;
	/* [한국어] for_each_set_bit 이 unsigned long 포인터를 요구하므로,
	 * INTx 상태를 담는 변수만 타입이 다르다. */
	unsigned long shifted_status;
	/* [한국어] 루프 변수와 읽은 상태·마스크 값을 담을 변수들. */
	u32 bit, val, mask;

	/*
	 * The core provides a single interrupt for both INTx/MSI messages.
	 * So we'll read both INTx and MSI status
	 */

	/* [한국어] 체인 진입. 상위 chip 이 fasteoi 면 아무것도 하지 않고, level 방식이면
	 * 상위 인터럽트를 마스크하고 ack 한다. 이렇게 해야 아래에서 하위 소스를
	 * 처리하는 동안 같은 선이 재진입하지 않는다. */
	chained_irq_enter(chip, desc);

	/* read INTx status */
	/* [한국어] 현재 세워진 인터럽트 상태 비트 전체. */
	val = mobiveil_csr_readl(pcie, PAB_INTP_AMBA_MISC_STAT);
	/* [한국어] 우리가 켜 둔 소스의 마스크. */
	mask = mobiveil_csr_readl(pcie, PAB_INTP_AMBA_MISC_ENB);
	/* [한국어] 둘을 AND 해서 '켜 둔 것 중 실제로 온 것'만 남긴다.
	 * 켜지 않은 소스의 상태 비트까지 지워 버리는 일을 피하려는 것이다. */
	intr_status = val & mask;

	/* Handle INTx */
	/* [한국어] INTA~INTD 중 하나라도 왔는지 확인한다. */
	if (intr_status & PAB_INTP_INTX_MASK) {
		/* [한국어] 상태 레지스터를 다시 읽는다 — 위에서 읽은 값이 아니라 최신 값을 쓰기
		 * 위해서다. 처리 도중에도 새 INTx 가 들어올 수 있다. */
		shifted_status = mobiveil_csr_readl(pcie,
						    PAB_INTP_AMBA_MISC_STAT);
		/* [한국어] INTx 네 비트만 남긴다. */
		shifted_status &= PAB_INTP_INTX_MASK;
		/* [한국어] INTA 가 0번 비트가 되도록 오른쪽으로 민다.
		 * 이제 이 변수의 비트 0~3 이 INTA~INTD 에 대응한다. */
		shifted_status >>= PAB_INTX_START;
		/* [한국어] 루프를 한 바퀴 돌고 다시 읽어 남은 것이 없을 때까지 반복한다.
		 * level 방식 INTx 는 원인이 해소될 때까지 계속 서 있으므로, 한 번만
		 * 처리하고 나가면 곧바로 같은 인터럽트가 다시 올라온다.
		 * 반대로 원인이 끝내 해소되지 않으면 이 루프는 인터럽트 컨텍스트에서
		 * 계속 돌게 되는데, 그것을 끊는 장치는 이 코드에 없다. */
		do {
			/* [한국어] 세워진 비트를 하나씩 순회한다. bit 는 0~3(INTA~INTD). */
			for_each_set_bit(bit, &shifted_status, PCI_NUM_INTX) {
				/* [한국어] generic_handle_domain_irq 의 반환값을 담을 변수. */
				int ret;
				/* [한국어] 해당 INTx 핀을 담당하는 Linux IRQ 핸들러를 부른다.
				 * hwirq 로 bit + 1 을 넘기므로 이 드라이버의 INTx hwirq 는 1~4 다
				 * (0 은 쓰지 않는다). 마스크·언마스크 함수도 같은 1 기반 규약을 따른다.
				 * 
				 * 다만 intx_domain 은 mobiveil_pcie_init_irq_domain() 에서 크기
				 * PCI_NUM_INTX(4)의 선형 도메인으로 만들어진다. 선형 도메인은 hwirq 를
				 * 0 부터 크기-1 까지만 받으므로(irq_domain_associate_locked 가
				 * hwirq >= hwirq_max 를 WARN 과 함께 거부한다), INTD 에 해당하는 hwirq 4 는
				 * 이 도메인에 매핑될 수 없다. drivers/pci/controller 아래에서
				 * 확인해 본 다른 INTx 처리 드라이버들(pci-mvebu,
				 * pcie-mediatek-gen3, pcie-xilinx-nwl, plda 등)은 모두
				 * 0 기반 hwirq 를 넘긴다 — 다만 전수 조사는 아니다.
				 * 이것이 의도된 것인지는 이 트리 안에서 확인할 근거를 찾지 못했다. */
				ret = generic_handle_domain_irq(rp->intx_domain,
								bit + 1);
				/* [한국어] 도메인이 hwirq 를 풀지 못한 경우(매핑이 없거나 범위를 벗어남). */
				if (ret)
					/* [한국어] 어느 핀에서 예상 밖의 인터럽트가 왔는지 남긴다.
					 * ratelimited 판인 것은, 원인이 해소되지 않으면 위 루프가 계속 돌아
					 * 로그가 폭주할 수 있기 때문이다. */
					dev_err_ratelimited(dev, "unexpected IRQ, INT%d\n",
							    bit);

				/* clear interrupt handled */
				/* [한국어] 처리한 핀의 상태 비트를 지운다. 상태 레지스터는 write-1-to-clear 라
				 * 지울 비트만 1 로 세운 값을 쓴다.
				 * bit 는 0 기반이므로 PAB_INTX_START(5)를 더해 원래 자리로 되돌린다. */
				mobiveil_csr_writel(pcie,
						    1 << (PAB_INTX_START + bit),
						    PAB_INTP_AMBA_MISC_STAT);
			}

			/* [한국어] 한 바퀴 도는 동안 새로 들어온 INTx 가 있는지 다시 읽는다. */
			shifted_status = mobiveil_csr_readl(pcie,
							    PAB_INTP_AMBA_MISC_STAT);
			/* [한국어] 다시 INTx 비트만 남기고, */
			shifted_status &= PAB_INTP_INTX_MASK;
			/* [한국어] 다시 0 기반으로 민다. 이 값이 0 이면 while 조건이 거짓이 되어 루프가 끝난다. */
			shifted_status >>= PAB_INTX_START;
		} while (shifted_status != 0);
	}

	/* read extra MSI status register */
	/* [한국어] MSI 상태는 PAB 영역이 아니라 별도의 apb_csr 영역에 있다.
	 * 그래서 페이지 방식 접근자가 아니라 readl_relaxed 로 직접 읽는다.
	 * _relaxed 판이라 메모리 배리어가 붙지 않는데, 뒤이은 접근들도 같은
	 * 장치의 순서 보장에 기대고 있다. */
	msi_status = readl_relaxed(pcie->apb_csr_base + MSI_STATUS_OFFSET);

	/* handle MSI interrupts */
	/* [한국어] bit0 이 1이면 FIFO 에 아직 꺼낼 MSI 가 남아 있다는 뜻이다.
	 * 여러 MSI 가 몰려 들어왔을 수 있으므로 빌 때까지 돈다. */
	while (msi_status & 1) {
		/* [한국어] FIFO 에서 MSI 데이터를 꺼낸다. 이 드라이버는 메시지 데이터로
		 * 벡터 번호를 그대로 싣게 해 두었으므로(mobiveil_compose_msi_msg 참조),
		 * 여기서 나오는 값이 곧 hwirq 다. */
		msi_data = readl_relaxed(pcie->apb_csr_base + MSI_DATA_OFFSET);

		/*
		 * MSI_STATUS_OFFSET register gets updated to zero
		 * once we pop not only the MSI data but also address
		 * from MSI hardware FIFO. So keeping these following
		 * two dummy reads.
		 */
		/* [한국어] 주소 하위 32비트를 꺼낸다. 값 자체는 쓰지 않지만, 데이터만 꺼내면
		 * FIFO 항목이 완전히 빠지지 않아 상태 비트가 내려가지 않는다 —
		 * 바로 위 상류 주석이 그 이유를 밝히고 있다. */
		msi_addr_lo = readl_relaxed(pcie->apb_csr_base +
					    MSI_ADDR_L_OFFSET);
		/* [한국어] 같은 이유로 주소 상위 32비트도 꺼낸다. */
		msi_addr_hi = readl_relaxed(pcie->apb_csr_base +
					    MSI_ADDR_H_OFFSET);
		/* [한국어] 디버그 빌드에서 어떤 벡터와 주소가 왔는지 확인할 수 있게 남긴다.
		 * 기본 빌드에서는 컴파일되지 않으므로 위 두 읽기가 '쓰이지 않는 값'이 되지만,
		 * MMIO 읽기 자체가 목적이라 최적화로 사라지지 않는다. */
		dev_dbg(dev, "MSI registers, data: %08x, addr: %08x:%08x\n",
			msi_data, msi_addr_hi, msi_addr_lo);

		/* [한국어] 꺼낸 벡터 번호로 해당 MSI 핸들러를 부른다.
		 * 여기서는 INTx 쪽과 달리 반환값을 확인하지 않는다. */
		generic_handle_domain_irq(msi->dev_domain, msi_data);

		/* [한국어] 다음 항목이 남았는지 다시 확인한다. */
		msi_status = readl_relaxed(pcie->apb_csr_base +
					   MSI_STATUS_OFFSET);
	}

	/* Clear the interrupt status */
	/* [한국어] 맨 앞에서 latch 해 둔 상태 비트를 되써서 지운다.
	 * INTx 비트는 위 루프에서 이미 지웠으므로 사실상 MSI 비트를 지우는 역할이고,
	 * 이미 지워진 비트를 다시 1 로 쓰는 것은 무해하다.
	 * 주의: 처리 도중 새로 세워진 비트는 이 값에 없으므로 지워지지 않는다 —
	 * 다음 인터럽트에서 처리된다. */
	mobiveil_csr_writel(pcie, intr_status, PAB_INTP_AMBA_MISC_STAT);
	/* [한국어] 체인 이탈. 상위 chip 이 fasteoi 면 EOI 를 보내고, level 방식이면
	 * 진입 때 마스크한 상위 인터럽트를 다시 연다. */
	chained_irq_exit(chip, desc);
}

/* [한국어] mobiveil_pcie_parse_dt - DT 에서 리소스와 창 개수를 읽어 컨트롤러를 채운다.
 * 
 * @pcie: 채울 대상 컨트롤러(pdev 는 이미 설정돼 있어야 한다).
 * @return: 0 성공, 음수 errno 면 리소스 매핑 실패.
 * 
 * 왜 필요한가: 이 IP 는 서로 다른 두 개의 MMIO 영역을 쓴다.
 * 하나는 EP 의 config 공간으로 나가는 창(config_axi_slave)이고,
 * 다른 하나는 컨트롤러 자신의 레지스터(csr_axi_slave)다.
 * 둘 다 매핑해 두어야 이후의 모든 접근이 성립한다.
 * 
 * 동작 단계:
 *   1) 'config_axi_slave' 리소스를 매핑하고, 그 리소스 자체도 보관한다 —
 *      나중에 아웃바운드 0번 창의 CPU 쪽 범위로 쓰인다.
 *   2) 'csr_axi_slave' 리소스를 매핑하고, 그 물리 시작 주소도 보관한다 —
 *      MSI 메시지 주소의 기준이 된다.
 *   3) apio-wins / ppio-wins 를 읽는다. 없으면 MAX_PIO_WINDOWS(8).
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: mobiveil_pcie_host_probe() (가장 먼저 불린다).
 * 피호출자: platform_get_resource_byname, devm_pci_remap_cfg_resource,
 *   of_property_read_u32.
 * 에러 경로: 리소스가 없으면 devm_pci_remap_cfg_resource 가 IS_ERR 포인터를
 *   돌려주므로, 리소스 부재와 매핑 실패가 같은 경로로 걸러진다.
 *   창 개수 속성은 없어도 오류가 아니다.
 * 
 * 호출 체인:
 *   각 SoC probe → mobiveil_pcie_host_probe() → [이 함수] */
static int mobiveil_pcie_parse_dt(struct mobiveil_pcie *pcie)
{
	/* [한국어] 오류 로그와 devm 매핑의 주인이 될 device. */
	struct device *dev = &pcie->pdev->dev;
	/* [한국어] 리소스를 꺼낼 platform 장치. */
	struct platform_device *pdev = pcie->pdev;
	/* [한국어] 창 개수 속성을 읽을 DT 노드. */
	struct device_node *node = dev->of_node;
	/* [한국어] 매핑 결과를 넣을 루트 포트 상태. */
	struct mobiveil_root_port *rp = &pcie->rp;
	/* [한국어] 리소스 포인터를 잠시 담을 변수. 두 리소스에 재사용된다. */
	struct resource *res;

	/* map config resource */
	/* [한국어] 이름으로 리소스를 찾는다 — DT 의 reg-names 에 'config_axi_slave' 로
	 * 적힌 항목이다. 순서가 아니라 이름으로 찾으므로 DT 배치에 의존하지 않는다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "config_axi_slave");
	/* [한국어] config 공간용으로 매핑한다. 일반 ioremap 이 아니라 config 전용 판인 것은,
	 * config 공간이 쓰기 결합(write-combining)이나 예측 읽기를 허용하지 않는
	 * 특별한 메모리 속성을 요구하기 때문이다.
	 * 리소스가 없어 res 가 NULL 이어도 이 함수가 -EINVAL 오류 포인터를 돌려주므로
	 * 별도의 NULL 검사가 필요 없다. */
	rp->config_axi_slave_base = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] IS_ERR 로 실패를 판별한다 — 반환값이 포인터라 NULL 검사만으로는 부족하다. */
	if (IS_ERR(rp->config_axi_slave_base))
		/* [한국어] 오류 코드를 포인터에서 꺼내 그대로 올린다. */
		return PTR_ERR(rp->config_axi_slave_base);
	/* [한국어] 리소스 자체도 보관한다. mobiveil_host_init() 이 이 리소스의 시작 주소와
	 * 크기로 아웃바운드 0번 창(config 창)을 설정한다. */
	rp->ob_io_res = res;

	/* map csr resource */
	/* [한국어] 두 번째 리소스 — 컨트롤러 자신의 레지스터 영역. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "csr_axi_slave");
	/* [한국어] 역시 config 전용 매핑을 쓴다. 이 영역 앞부분이 RC 자신의 config 헤더
	 * 역할을 하므로 같은 메모리 속성이 필요하다. */
	pcie->csr_axi_slave_base = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] 매핑 실패 확인. */
	if (IS_ERR(pcie->csr_axi_slave_base))
		/* [한국어] 오류를 그대로 올린다. */
		return PTR_ERR(pcie->csr_axi_slave_base);
	/* [한국어] 물리 시작 주소를 따로 보관한다. 가상 주소가 아니라 물리 주소가 필요한
	 * 곳이 있기 때문이다 — MSI 메시지는 장치가 버스로 써야 하는 주소다. */
	pcie->pcie_reg_base = res->start;

	/* read the number of windows requested */
	/* [한국어] DT 에 아웃바운드 창 개수가 적혀 있으면 그것을 쓴다.
	 * of_property_read_u32 는 성공 시 0 을 돌려주므로, 참이 되는 것은 실패한 경우다. */
	if (of_property_read_u32(node, "apio-wins", &pcie->apio_wins))
		/* [한국어] 속성이 없으면 이 IP 의 최대치인 8 을 가정한다. */
		pcie->apio_wins = MAX_PIO_WINDOWS;

	/* [한국어] 인바운드 창 개수도 같은 방식으로 읽는다. */
	if (of_property_read_u32(node, "ppio-wins", &pcie->ppio_wins))
		/* [한국어] 없으면 8. */
		pcie->ppio_wins = MAX_PIO_WINDOWS;

	/* [한국어] 두 리소스를 모두 매핑했다. 이 시점부터 mobiveil_csr_ 접근자가 안전해진다. */
	return 0;
}

/* [한국어] mobiveil_pcie_enable_msi - MSI 하드웨어 블록을 설정하고 켠다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 없음.
 * 
 * 왜 필요한가: 이 IP 의 MSI 수신부는 '어느 주소 범위로 오는 쓰기를 MSI 로
 * 볼 것인가'를 알아야 한다. 이 함수가 그 범위(기준 주소와 크기)를 알려 주고
 * 수신부를 켠다.
 * 
 * 동작: 컨트롤러 레지스터 영역의 물리 주소를 MSI 창의 기준으로 삼는다.
 * 장치가 그 주소로 쓰기를 보내면 하드웨어가 그것을 MSI 로 낚아채
 * FIFO 에 넣고, ISR 이 꺼내 간다.
 * 
 * 주의할 점은 순서다. 이 함수가 num_of_vectors 를 설정하고,
 * 뒤이어 불리는 mobiveil_allocate_msi_domains() 가 그 값을 도메인 크기로 쓴다.
 * 순서가 뒤집히면 크기 0 짜리 도메인이 만들어진다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: mobiveil_pcie_integrated_interrupt_init().
 * 피호출자: writel_relaxed (apb_csr 영역 직접 접근).
 * 에러 경로: 없다 — 실패할 수 있는 동작이 없다.
 * 
 * 호출 체인:
 *   mobiveil_pcie_host_probe() → mobiveil_pcie_interrupt_init()
 *     → mobiveil_pcie_integrated_interrupt_init() → [이 함수] */
static void mobiveil_pcie_enable_msi(struct mobiveil_pcie *pcie)
{
	/* [한국어] MSI 메시지가 향할 기준 물리 주소 — 컨트롤러 레지스터 영역의 시작이다.
	 * 실제 레지스터가 있는 곳과 같은 주소를 MSI 목적지로 쓰는 셈인데,
	 * MSI 쓰기는 PCIe 쪽에서 들어오므로 하드웨어가 이를 별도 경로로 가로챈다. */
	phys_addr_t msg_addr = pcie->pcie_reg_base;
	/* [한국어] 채울 MSI 상태 묶음. */
	struct mobiveil_msi *msi = &pcie->rp.msi;

	/* [한국어] 이 드라이버가 지원할 벡터 수를 확정한다.
	 * 아래 도메인 생성이 이 값을 크기로 쓰므로 반드시 먼저 설정돼야 한다. */
	msi->num_of_vectors = PCI_NUM_MSI;
	/* [한국어] 같은 주소를 보관해 둔다. 다만 이 필드를 다시 읽는 코드는
	 * 이 트리에서 찾지 못했다 — 실제 메시지 주소는
	 * mobiveil_compose_msi_msg() 가 pcie_reg_base 로 직접 계산한다.
	 * 캐스팅은 이미 phys_addr_t 인 값에 붙은 것이라 형식적이다. */
	msi->msi_pages_phys = (phys_addr_t)msg_addr;

	/* [한국어] MSI 창 기준 주소의 하위 32비트를 알린다. */
	writel_relaxed(lower_32_bits(msg_addr),
		       pcie->apb_csr_base + MSI_BASE_LO_OFFSET);
	/* [한국어] 상위 32비트를 알린다 — 32비트 물리 주소 체계라면 0 이 들어간다. */
	writel_relaxed(upper_32_bits(msg_addr),
		       pcie->apb_csr_base + MSI_BASE_HI_OFFSET);
	/* [한국어] MSI 창의 크기를 4096 바이트로 정한다.
	 * 벡터 하나가 4바이트를 차지하므로(compose_msi_msg 의 hwirq x sizeof(int))
	 * 16개 벡터에는 64바이트면 충분하지만, 한 페이지를 통째로 잡아 둔다. */
	writel_relaxed(4096, pcie->apb_csr_base + MSI_SIZE_OFFSET);
	/* [한국어] MSI 수신을 켠다. 이 쓰기 뒤부터 하드웨어가 해당 범위의 쓰기를
	 * MSI 로 인식한다. */
	writel_relaxed(1, pcie->apb_csr_base + MSI_ENABLE_OFFSET);
}

/* [한국어] mobiveil_host_init - 창·PIO·버스 번호·클래스 코드를 세워 RC 를 동작 가능 상태로 만든다.
 * 
 * @pcie: 대상 컨트롤러(DT 파싱이 끝나 있어야 한다).
 * @reinit: false 면 첫 초기화, true 면 리셋 뒤 재초기화.
 *   재초기화 때는 버스 번호 설정을 건너뛴다 — 이미 열거된 버스 번호를
 *   유지해야 상위 계층의 장치 정보와 어긋나지 않기 때문이다.
 * @return: 이 구현은 언제나 0. 호출자들은 그럼에도 반환값을 확인한다.
 * 
 * 왜 필요한가: 전원이 켜진 직후의 이 IP 는 아무 주소 변환도, 아무 PIO 경로도
 * 열려 있지 않다. config 접근조차 창이 잡혀 있어야 가능하다.
 * 이 함수가 그 최소 설정을 모두 한다.
 * 
 * 동작 단계:
 *   1) 창 사용량 카운터를 0 으로 되돌린다(재초기화에서도 처음부터 다시 잡는다).
 *   2) (첫 초기화만) 버스 번호를 primary=0, secondary=1, subordinate=0xff 로 둔다.
 *   3) config 헤더의 command 레지스터에서 I/O, 메모리, 버스 마스터를 켠다.
 *   4) PAB_CTRL 의 AMBA/PEX PIO 를 켠다.
 *   5) AXI PIO 와 PEX PIO 를 각각 켠다.
 *   6) config 접근용 아웃바운드 0번 창과 기본 인바운드 0번 창을 잡는다.
 *   7) DT 의 ranges 를 훑어 메모리·I/O 영역마다 아웃바운드 창을 추가로 잡는다.
 *   8) 클래스 코드를 PCI 브리지로 고쳐 넣는다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. probe 경로와 gen4 의 리셋 복구 워크
 * 양쪽에서 불린다. 두 경우 모두 인터럽트 컨텍스트가 아니다.
 * 호출자: mobiveil_pcie_host_probe()(reinit=false),
 *   pcie-layerscape-gen4.c 의 ls_g4_pcie_reinit_hw()(reinit=true).
 * 피호출자: mobiveil_csr_readl/writel, program_ob_windows, program_ib_windows,
 *   resource_list_for_each_entry.
 * 에러 경로: 창이 모자라면 program_ob_windows 가 로그만 남기고 조용히
 *   돌아가므로, 이 함수는 그것을 알아채지 못하고 0 을 돌려준다.
 * 
 * 호출 체인:
 *   mobiveil_pcie_host_probe() → [이 함수] → program_ob_windows() /
 *     program_ib_windows() → mobiveil_csr_write() */
int mobiveil_host_init(struct mobiveil_pcie *pcie, bool reinit)
{
	/* [한국어] 창 개수 한도와 config 리소스가 들어 있는 루트 포트 상태. */
	struct mobiveil_root_port *rp = &pcie->rp;
	/* [한국어] DT 의 ranges 가 변환돼 들어 있는 호스트 브리지.
	 * SoC 드라이버가 probe 에서 rp.bridge 에 넣어 준 것이다. */
	struct pci_host_bridge *bridge = rp->bridge;
	/* [한국어] 레지스터 값과 창 종류를 담을 지역 변수들. */
	u32 value, pab_ctrl, type;
	/* [한국어] bridge->windows 리스트를 순회할 때 쓸 항목 포인터. */
	struct resource_entry *win;

	/* [한국어] 인바운드 창 사용량을 0 으로 되돌린다. 재초기화에서도 창을 처음부터
	 * 다시 잡으므로 카운터도 리셋해야 한다. */
	pcie->ib_wins_configured = 0;
	/* [한국어] 아웃바운드 창 사용량도 0 으로. 이 값이 곧 다음에 쓸 창 번호가 된다. */
	pcie->ob_wins_configured = 0;

	/* [한국어] 첫 초기화일 때만 버스 번호를 설정한다. */
	if (!reinit) {
		/* setup bus numbers */
		/* [한국어] config 헤더의 버스 번호 3인조(primary/secondary/subordinate)와
		 * secondary latency timer 가 한 32비트 워드에 들어 있다. */
		value = mobiveil_csr_readl(pcie, PCI_PRIMARY_BUS);
		/* [한국어] 상위 8비트(secondary latency timer)만 남기고 버스 번호들을 지운다. */
		value &= 0xff000000;
		/* [한국어] primary=0x00, secondary=0x01, subordinate=0xff 를 넣는다.
		 * subordinate 를 최대값으로 열어 두어야 아래쪽 버스 열거가 막히지 않고,
		 * 실제 값은 나중에 PCI 코어가 열거 결과로 다시 쓴다. */
		value |= 0x00ff0100;
		/* [한국어] 고친 값을 되쓴다. */
		mobiveil_csr_writel(pcie, value, PCI_PRIMARY_BUS);
	}

	/*
	 * program Bus Master Enable Bit in Command Register in PAB Config
	 * Space
	 */
	/* [한국어] config 헤더의 command 레지스터를 읽는다. */
	value = mobiveil_csr_readl(pcie, PCI_COMMAND);
	/* [한국어] I/O 응답, 메모리 응답, 그리고 버스 마스터를 켠다.
	 * 버스 마스터가 없으면 아래 장치들이 DMA 도 MSI 도 보낼 수 없다 —
	 * MSI 역시 메모리 쓰기 트랜잭션이기 때문이다. */
	value |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	/* [한국어] 고친 값을 되쓴다. */
	mobiveil_csr_writel(pcie, value, PCI_COMMAND);

	/*
	 * program PIO Enable Bit to 1 (and PEX PIO Enable to 1) in PAB_CTRL
	 * register
	 */
	/* [한국어] PAB 전역 제어 레지스터를 읽는다. */
	pab_ctrl = mobiveil_csr_readl(pcie, PAB_CTRL);
	/* [한국어] AMBA(AXI) 쪽과 PEX(PCIe) 쪽 PIO 를 함께 켠다.
	 * 읽고-고치고-쓰기인 것은 이 레지스터에 페이지 선택 필드도 함께 있어
	 * 통째로 덮어쓰면 안 되기 때문이다. */
	pab_ctrl |= (1 << AMBA_PIO_ENABLE_SHIFT) | (1 << PEX_PIO_ENABLE_SHIFT);
	/* [한국어] 고친 값을 되쓴다. 이 쓰기 자체도 내부적으로 페이지 선택을 거친다. */
	mobiveil_csr_writel(pcie, pab_ctrl, PAB_CTRL);

	/*
	 * program PIO Enable Bit to 1 and Config Window Enable Bit to 1 in
	 * PAB_AXI_PIO_CTRL Register
	 */
	/* [한국어] AXI PIO 제어 레지스터를 읽는다. */
	value = mobiveil_csr_readl(pcie, PAB_AXI_PIO_CTRL);
	/* [한국어] PIO 활성화와 config 창 활성화를 한꺼번에 켠다(하위 4비트). */
	value |= APIO_EN_MASK;
	/* [한국어] 고친 값을 되쓴다. 이 시점부터 CPU 쪽에서 PCIe 로 나가는 길이 열린다. */
	mobiveil_csr_writel(pcie, value, PAB_AXI_PIO_CTRL);

	/* Enable PCIe PIO master */
	/* [한국어] PCIe PIO 제어 레지스터를 읽는다. */
	value = mobiveil_csr_readl(pcie, PAB_PEX_PIO_CTRL);
	/* [한국어] PIO 활성화 비트를 켠다. */
	value |= 1 << PIO_ENABLE_SHIFT;
	/* [한국어] 고친 값을 되쓴다. 이 시점부터 PCIe 쪽에서 들어오는 접근이 처리된다. */
	mobiveil_csr_writel(pcie, value, PAB_PEX_PIO_CTRL);

	/*
	 * we'll program one outbound window for config reads and
	 * another default inbound window for all the upstream traffic
	 * rest of the outbound windows will be configured according to
	 * the "ranges" field defined in device tree
	 */

	/* config outbound translation window */
	/* [한국어] config 접근용 아웃바운드 0번 창을 잡는다.
	 * CPU 쪽 범위는 DT 의 config_axi_slave 리소스 그대로이고, PCIe 쪽 목적지는
	 * 0 으로 두는데 — 실제 목적지는 config 접근마다 mobiveil_pcie_map_bus() 가
	 * BDF 로 덮어쓰기 때문이다. */
	program_ob_windows(pcie, WIN_NUM_0, rp->ob_io_res->start, 0,
			   CFG_WINDOW_TYPE, resource_size(rp->ob_io_res));

	/* memory inbound translation window */
	/* [한국어] 기본 인바운드 0번 창을 잡는다. CPU 쪽과 PCIe 쪽 모두 0 에서 시작하는
	 * 256GB 창이라, 장치가 보내는 DMA 주소가 그대로 시스템 물리 주소가 된다.
	 * 즉 이 드라이버는 인바운드 주소 변환을 사실상 하지 않는다 —
	 * IOMMU 가 있다면 그쪽에서 다시 걸러진다. */
	program_ib_windows(pcie, WIN_NUM_0, 0, 0, MEM_WINDOW_TYPE, IB_WIN_SIZE);

	/* Get the I/O and memory ranges from DT */
	/* [한국어] DT 의 ranges 에서 온 창 목록을 순회한다.
	 * PCI 코어가 이미 파싱해 bridge->windows 에 넣어 둔 것이다. */
	resource_list_for_each_entry(win, &bridge->windows) {
		/* [한국어] 메모리 영역인 경우. */
		if (resource_type(win->res) == IORESOURCE_MEM)
			/* [한국어] 메모리 트랜잭션용 창 종류를 고른다. */
			type = MEM_WINDOW_TYPE;
		/* [한국어] I/O 영역인 경우. */
		else if (resource_type(win->res) == IORESOURCE_IO)
			/* [한국어] I/O 트랜잭션용 창 종류를 고른다. */
			type = IO_WINDOW_TYPE;
		/* [한국어] 그 밖의 항목(버스 번호 범위 등)인 경우. */
		else
			/* [한국어] 주소 창으로 잡을 대상이 아니므로 건너뛴다. */
			continue;

		/* configure outbound translation window */
		/* [한국어] 아웃바운드 창을 하나 추가로 잡는다. 창 번호로 현재 사용량을 그대로
		 * 넘기는데, program_ob_windows() 안에서 성공 시 이 값이 증가하므로
		 * 다음 순회에서는 자연히 다음 창 번호가 된다. */
		program_ob_windows(pcie, pcie->ob_wins_configured,
				   /* [한국어] CPU 쪽 시작 주소는 리소스의 시작이다. */
				   win->res->start,
				   /* [한국어] PCIe 쪽 목적지는 CPU 주소에서 offset 을 뺀 값이다.
				    * DT 의 ranges 는 (PCI 주소, CPU 주소, 크기) 삼중항으로 적히고 코어가
				    * 그 차이를 offset 으로 계산해 두므로, 여기서 되돌리면 PCI 주소가 나온다. */
				   win->res->start - win->offset,
				   /* [한국어] 창 종류와 크기를 넘긴다. 창이 모자라면 하위 함수가 로그만 남기고
				    * 조용히 돌아가므로, 이 루프는 그 사실을 알지 못한 채 계속 돈다. */
				   type, resource_size(win->res));
	}

	/* fixup for PCIe class register */
	/* [한국어] 클래스 코드 shadow 레지스터를 읽는다. */
	value = mobiveil_csr_readl(pcie, PAB_INTP_AXI_PIO_CLASS);
	/* [한국어] 하위 8비트(리비전 ID)만 남기고 클래스 코드 자리를 비운다. */
	value &= 0xff;
	/* [한국어] PCI-to-PCI 브리지(0x0604) 클래스에 prog-if 0 을 붙인 0x060400 을
	 * 8비트 왼쪽으로 밀어 [31:8] 자리에 넣는다.
	 * 이렇게 해 두지 않으면 RC 가 브리지로 보이지 않아, 아래에 있는 장치들이
	 * PCI 코어의 열거에서 제대로 다뤄지지 않는다. */
	value |= PCI_CLASS_BRIDGE_PCI_NORMAL << 8;
	/* [한국어] 고친 값을 되쓴다. */
	mobiveil_csr_writel(pcie, value, PAB_INTP_AXI_PIO_CLASS);

	/* [한국어] 이 구현에는 실패로 빠지는 경로가 없어 언제나 성공을 알린다. */
	return 0;
}

/* [한국어] mobiveil_mask_intx_irq - 하나의 INTx 핀을 마스크한다.
 * 
 * @data: 마스크할 IRQ 의 irq_data. hwirq 로 어느 핀인지, chip_data 로
 *   컨트롤러 문맥을 얻는다.
 * @return: 없음.
 * 
 * 왜 필요한가: 커널이 특정 INTx 를 잠시 막아야 할 때(핸들러 해제, 공유 처리 등)
 * 하드웨어 쪽에서도 그 소스를 꺼 주어야 한다. 그렇지 않으면 level 인터럽트가
 * 계속 서서 CPU 를 잡아먹는다.
 * 
 * 동작: 활성화 레지스터에서 해당 핀의 비트만 지운다.
 * 읽고-고치고-쓰기이므로 락으로 감싼다.
 * 
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수 있다. 그래서 일반 spinlock 이 아니라
 * raw spinlock 을 irqsave 판으로 잡는다 — RT 커널에서도 잠들지 않는다.
 * 호출자: IRQ 코어(irq_chip 의 irq_mask / irq_disable 콜백).
 * 피호출자: mobiveil_csr_readl/writel, raw_spin_lock_irqsave/unlock_irqrestore.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   disable_irq() 등 → IRQ 코어 → chip->irq_mask → [이 함수] */
static void mobiveil_mask_intx_irq(struct irq_data *data)
{
	/* [한국어] 도메인 생성 때 host_data 로 넣어 둔 컨트롤러 문맥을 되찾는다.
	 * mobiveil_pcie_intx_map() 이 irq_set_chip_data 로 심어 준 값이다. */
	struct mobiveil_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* [한국어] 락과 레지스터 접근에 쓸 루트 포트 상태. */
	struct mobiveil_root_port *rp;
	/* [한국어] 인터럽트 상태 저장용. 이 함수가 이미 인터럽트가 꺼진 상태에서
	 * 불릴 수도 있어 무조건 켜는 대신 원래 상태로 되돌린다. */
	unsigned long flags;
	/* [한국어] 지울 비트와 읽어 온 레지스터 값을 담을 변수들. */
	u32 mask, shifted_val;

	/* [한국어] 루트 포트 상태 주소. */
	rp = &pcie->rp;
	/* [한국어] hwirq(1~4)를 레지스터 비트 자리로 옮긴다.
	 * PAB_INTX_START(5)를 더하고 1을 빼므로 hwirq 1 은 비트 5(INTA),
	 * hwirq 4 는 비트 8(INTD)이 된다. */
	mask = 1 << ((data->hwirq + PAB_INTX_START) - 1);
	/* [한국어] 읽고-고치고-쓰기를 원자적으로 만든다. 이 락이 없으면 두 핀을 동시에
	 * 마스크할 때 한쪽 갱신이 상대의 되쓰기에 덮여 사라질 수 있다.
	 * 주의: 이 락은 이 레지스터 접근만 보호한다. 같은 접근이 내부적으로
	 * 동반하는 PAB_CTRL 페이지 선택 쓰기까지는 보호하지 못한다. */
	raw_spin_lock_irqsave(&rp->intx_mask_lock, flags);
	/* [한국어] 현재 활성화 마스크를 읽는다. */
	shifted_val = mobiveil_csr_readl(pcie, PAB_INTP_AMBA_MISC_ENB);
	/* [한국어] 해당 핀의 비트만 지운다 — 다른 핀과 MSI 설정은 보존된다. */
	shifted_val &= ~mask;
	/* [한국어] 고친 마스크를 되쓴다. 이 시점부터 그 핀은 인터럽트를 올리지 못한다. */
	mobiveil_csr_writel(pcie, shifted_val, PAB_INTP_AMBA_MISC_ENB);
	/* [한국어] 락을 풀고 인터럽트 상태를 원래대로 되돌린다. */
	raw_spin_unlock_irqrestore(&rp->intx_mask_lock, flags);
}

/* [한국어] mobiveil_unmask_intx_irq - 하나의 INTx 핀 마스크를 푼다.
 * 
 * @data: 언마스크할 IRQ 의 irq_data.
 * @return: 없음.
 * 
 * 위 마스크 함수의 짝이다. 비트를 지우는 대신 세우는 것만 다르다.
 * request_irq 로 핸들러가 붙을 때, 그리고 처리 후 다시 열 때 불린다.
 * 
 * 실행 컨텍스트: 인터럽트 컨텍스트일 수 있으므로 같은 raw spinlock 을 쓴다.
 * 호출자: IRQ 코어(irq_chip 의 irq_unmask / irq_enable 콜백).
 * 피호출자: mobiveil_csr_readl/writel, raw_spin_lock_irqsave/unlock_irqrestore.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   request_irq() / enable_irq() 등 → IRQ 코어 → chip->irq_unmask → [이 함수] */
static void mobiveil_unmask_intx_irq(struct irq_data *data)
{
	/* [한국어] 컨트롤러 문맥을 되찾는다. */
	struct mobiveil_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* [한국어] 루트 포트 상태를 담을 변수. */
	struct mobiveil_root_port *rp;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 읽어 온 값과 세울 비트를 담을 변수들. */
	u32 shifted_val, mask;

	/* [한국어] 루트 포트 상태 주소. */
	rp = &pcie->rp;
	/* [한국어] 마스크 함수와 똑같은 규칙으로 비트 자리를 계산한다.
	 * 두 함수가 같은 식을 쓰는 것이 중요하다 — 어긋나면 엉뚱한 핀이 열린다. */
	mask = 1 << ((data->hwirq + PAB_INTX_START) - 1);
	/* [한국어] 읽고-고치고-쓰기를 보호한다. */
	raw_spin_lock_irqsave(&rp->intx_mask_lock, flags);
	/* [한국어] 현재 활성화 마스크를 읽는다. */
	shifted_val = mobiveil_csr_readl(pcie, PAB_INTP_AMBA_MISC_ENB);
	/* [한국어] 해당 핀의 비트를 세운다. */
	shifted_val |= mask;
	/* [한국어] 고친 마스크를 되쓴다. 이 시점부터 그 핀의 인터럽트가 올라온다. */
	mobiveil_csr_writel(pcie, shifted_val, PAB_INTP_AMBA_MISC_ENB);
	/* [한국어] 락을 풀고 인터럽트 상태를 되돌린다. */
	raw_spin_unlock_irqrestore(&rp->intx_mask_lock, flags);
}

/* [한국어] INTx 네 핀이 공유하는 irq_chip.
 * irq_chip_data 에 struct mobiveil_pcie 가, hwirq 에 핀 번호가 들어가므로
 * 한 인스턴스로 네 핀을 모두 다룰 수 있다. */
static struct irq_chip intx_irq_chip = {
	/* [한국어] /proc/interrupts 등에 보이는 이름. */
	.name = "mobiveil_pcie:intx",
	/* [한국어] enable 은 unmask 와 같다 — 이 하드웨어에는 '켜기'와 '마스크 풀기'를
	 * 구분할 별도 수단이 없다. */
	.irq_enable = mobiveil_unmask_intx_irq,
	/* [한국어] disable 도 mask 와 같다. */
	.irq_disable = mobiveil_mask_intx_irq,
	/* [한국어] level 방식 핀을 잠시 막을 때 쓰인다. */
	.irq_mask = mobiveil_mask_intx_irq,
	/* [한국어] 막았던 핀을 다시 열 때 쓰인다. */
	.irq_unmask = mobiveil_unmask_intx_irq,
};

/* routine to setup the INTx related data */
/* [한국어] mobiveil_pcie_intx_map - INTx 도메인에 새 매핑이 생길 때 불리는 콜백.
 * 
 * @domain: INTx irq_domain.
 * @irq: 커널이 할당한 Linux IRQ 번호(virq).
 * @hwirq: 이 드라이버 규약의 INTx 핀 번호. bit + 1 규약이므로 1~4 를 의도한다.
 * @return: 언제나 0(실패 경로가 없다).
 * 
 * 왜 필요한가: DT 의 interrupt-map 을 따라 장치의 INTx 가 이 도메인으로
 * 풀릴 때, 커널은 virq 하나를 새로 만들고 이 콜백으로 '이 virq 를 어떻게
 * 다룰지'를 드라이버에 묻는다.
 * 
 * 동작: 위 intx_irq_chip 과 level 흐름 핸들러를 붙이고, 도메인 생성 때
 * 넣어 둔 host_data(컨트롤러 문맥)를 chip_data 로 심는다.
 * 그 chip_data 를 마스크·언마스크 함수가 되찾아 쓴다.
 * 
 * handle_level_irq 를 쓰는 것은 INTx 가 레벨 트리거이기 때문이다 —
 * 핸들러 전에 마스크하고 끝난 뒤 다시 여는 흐름이 필요하다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(매핑이 만들어지는 시점).
 * 호출자: IRQ 도메인 코어(irq_domain_associate 경유).
 * 피호출자: irq_set_chip_and_handler(), irq_set_chip_data().
 * 에러 경로: 없다.
 * 
 * 참고: hwirq 4(INTD)는 이 도메인에 매핑될 수 없다.
 * 도메인이 크기 4 로 만들어져 유효 hwirq 가 0~3 이기 때문이다.
 * 자세한 것은 mobiveil_pcie_isr() 의 hwirq 계산 주석 참조.
 * 
 * 호출 체인:
 *   DT interrupt-map 해석 → IRQ 도메인 코어 → domain->ops->map → [이 함수] */
static int mobiveil_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	/* [한국어] 이 virq 에 INTx 용 chip 과 레벨 트리거 흐름 핸들러를 건다.
	 * handle_level_irq 는 핸들러 호출 전에 마스크하고 끝난 뒤 언마스크하므로,
	 * 원인이 해소되지 않은 레벨 인터럽트가 폭주하는 것을 막아 준다. */
	irq_set_chip_and_handler(irq, &intx_irq_chip, handle_level_irq);
	/* [한국어] 도메인 생성 때 넘긴 host_data — 즉 struct mobiveil_pcie 를 chip_data 로 심는다.
	 * 마스크·언마스크 함수가 irq_data_get_irq_chip_data() 로 이 값을 되찾는다. */
	irq_set_chip_data(irq, domain->host_data);

	/* [한국어] 이 콜백에는 실패할 여지가 없어 언제나 성공을 알린다. */
	return 0;
}

/* INTx domain operations structure */
/* [한국어] INTx 도메인의 연산 표. map 콜백 하나만 둔다. */
static const struct irq_domain_ops intx_domain_ops = {
	/* [한국어] 새 매핑이 생길 때 위 함수를 부르게 한다.
	 * xlate 를 두지 않았으므로 DT 의 인터럽트 지정자 해석은 도메인 코어의
	 * 기본 동작을 따르고, 그 결과 첫 셀 값이 그대로 hwirq 가 된다
	 * (PCI 의 INTx 관례에서는 INTA=1 ~ INTD=4). */
	.map = mobiveil_pcie_intx_map,
};

/* [한국어] 이 컨트롤러의 MSI 부모 도메인이 자식(장치별) 도메인에게 반드시
 * 요구하는 플래그 묶음. */
#define MOBIVEIL_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS		| \
				     MSI_FLAG_USE_DEF_CHIP_OPS		| \
				     MSI_FLAG_NO_AFFINITY)

/* [한국어] 이 부모 도메인이 자식에게 허용하는 플래그의 상한.
 * MSI_GENERIC_FLAGS_MASK 는 하위 16비트의 범용 플래그 전부이고,
 * 거기에 PCI MSI-X 지원을 더한다. */
#define MOBIVEIL_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		| \
				      MSI_FLAG_PCI_MSIX)

/* [한국어] MSI 계층 구조에서 이 컨트롤러가 '부모' 역할을 할 때의 규약 표.
 * 장치별 MSI 도메인이 만들어질 때 코어가 이 표를 보고 자식의 성질을 정한다. */
static const struct msi_parent_ops mobiveil_msi_parent_ops = {
	/* [한국어] 위에서 정의한 필수 플래그. */
	.required_flags		= MOBIVEIL_MSI_FLAGS_REQUIRED,
	/* [한국어] 위에서 정의한 허용 플래그. */
	.supported_flags	= MOBIVEIL_MSI_FLAGS_SUPPORTED,
	/* [한국어] 이 부모가 받아 주는 자식 도메인의 종류 — PCI MSI 다.
	 * 같은 fwnode 에 여러 종류의 MSI 도메인이 매달릴 수 있어 이 토큰으로 구분한다. */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	/* [한국어] 만들어지는 도메인 이름 앞에 붙는 문자열. /proc/interrupts 등에서
	 * 어느 컨트롤러의 도메인인지 알아보기 쉽게 한다. */
	.prefix			= "Mobiveil-",
	/* [한국어] 자식 도메인 정보를 채우는 표준 구현을 그대로 쓴다.
	 * irq-msi-lib 가 제공하며, Kconfig 가 IRQ_MSI_LIB 를 select 하는 이유다. */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/* [한국어] mobiveil_compose_msi_msg - 장치가 쓸 MSI 메시지(주소와 데이터)를 만든다.
 * 
 * @data: 대상 MSI 인터럽트의 irq_data. hwirq 가 벡터 번호다.
 * @msg: 채워 넣을 메시지 구조체. 코어가 이 값을 장치의 MSI 캐퍼빌리티에
 *   써 넣는다.
 * @return: 없음.
 * 
 * 왜 필요한가: MSI 는 결국 '정해진 주소에 정해진 값을 쓰는' 메모리 쓰기다.
 * 어느 주소에 무엇을 쓸지는 컨트롤러마다 다르므로 드라이버가 정해 줘야 한다.
 * 
 * 동작: 벡터마다 4바이트씩 떨어진 주소를 배정하고, 데이터로는 벡터 번호를
 * 그대로 싣는다. 그래서 ISR 이 FIFO 에서 꺼낸 데이터가 곧 hwirq 가 된다.
 * 주소를 벡터마다 다르게 하는 것은 하드웨어가 그 주소로도 벡터를 구분할 수
 * 있게 하려는 것으로 보이나, 이 드라이버의 ISR 은 데이터만 쓴다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트(MSI 설정 시점).
 * 호출자: MSI 코어(irq_chip 의 irq_compose_msi_msg 콜백).
 * 피호출자: lower_32_bits / upper_32_bits.
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   pci_alloc_irq_vectors() → MSI 코어 → chip->irq_compose_msi_msg → [이 함수] */
static void mobiveil_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	/* [한국어] 도메인 생성 때 host_data 로 넣어 둔 컨트롤러 문맥을 되찾는다. */
	struct mobiveil_pcie *pcie = irq_data_get_irq_chip_data(data);
	/* [한국어] 벡터별 목적지 주소 — MSI 창 기준 주소에 벡터 번호 x 4바이트를 더한다.
	 * 가상 주소가 아니라 물리 주소인 점이 중요하다. 장치는 이 값을 버스
	 * 주소로 그대로 쓴다. */
	phys_addr_t addr = pcie->pcie_reg_base + (data->hwirq * sizeof(int));

	/* [한국어] 메시지 주소의 하위 32비트. */
	msg->address_lo = lower_32_bits(addr);
	/* [한국어] 상위 32비트. 32비트 주소라면 0 이 되고, 그 경우 코어가 32비트 MSI 로 다룬다. */
	msg->address_hi = upper_32_bits(addr);
	/* [한국어] 메시지 데이터로 벡터 번호를 그대로 싣는다.
	 * 이 규약 덕분에 ISR 이 FIFO 에서 꺼낸 값을 곧바로 hwirq 로 쓸 수 있다. */
	msg->data = data->hwirq;

	/* [한국어] 디버그 빌드에서 어떤 벡터에 어떤 주소가 배정됐는지 확인할 수 있게 남긴다. */
	dev_dbg(&pcie->pdev->dev, "msi#%d address_hi %#x address_lo %#x\n",
		(int)data->hwirq, msg->address_hi, msg->address_lo);
}

/* [한국어] MSI 계층에서 가장 아래(bottom) 에 놓이는 irq_chip.
 * 실제 마스크·언마스크 콜백이 없는데, 이 하드웨어에는 개별 MSI 벡터를
 * 막는 수단이 없어 보인다 — 마스킹은 장치 쪽 MSI 캐퍼빌리티에 맡겨진다. */
static struct irq_chip mobiveil_msi_bottom_irq_chip = {
	/* [한국어] /proc/interrupts 등에 보이는 이름. */
	.name			= "Mobiveil MSI",
	/* [한국어] 메시지 조립만 이 드라이버가 맡는다. 나머지 콜백은
	 * MSI_FLAG_USE_DEF_CHIP_OPS 덕분에 코어의 기본 구현이 채워진다. */
	.irq_compose_msi_msg	= mobiveil_compose_msi_msg,
};

/* [한국어] mobiveil_irq_msi_domain_alloc - MSI 벡터 하나를 할당한다.
 * 
 * @domain: MSI 부모 도메인.
 * @virq: 코어가 이미 잡아 둔 Linux IRQ 번호.
 * @nr_irqs: 요청 개수. 이 드라이버는 1 만 지원한다.
 * @args: 코어가 넘기는 추가 인자(이 구현은 쓰지 않는다).
 * @return: 0 성공, -ENOSPC 면 남은 벡터가 없음.
 * 
 * 왜 필요한가: 이 컨트롤러의 MSI 벡터는 16개뿐이다. 어느 것이 쓰이고 있는지
 * 비트맵으로 추적하고, 새 요청에 빈 자리를 배정해야 한다.
 * 
 * 동작: 뮤텍스를 잡고 비트맵에서 첫 빈 비트를 찾아 표시한 뒤,
 * 그 비트 번호를 hwirq 로 삼아 virq 에 chip 과 흐름 핸들러를 연결한다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다 —
 * 인터럽트 컨텍스트에서는 불릴 수 없다. MSI 할당은 항상 장치 드라이버의
 * 초기화 경로에서 일어나므로 문제되지 않는다.
 * 호출자: MSI 도메인 코어(irq_domain_alloc_irqs 경유).
 * 피호출자: find_first_zero_bit, set_bit, irq_domain_set_info.
 * 에러 경로: 빈 벡터가 없으면 락을 풀고 -ENOSPC 를 돌려준다.
 *   코어는 이를 장치 드라이버에 전달하고, 드라이버는 보통 INTx 로 물러난다.
 * 
 * 호출 체인:
 *   pci_alloc_irq_vectors() → MSI 코어 → domain->ops->alloc → [이 함수] */
static int mobiveil_irq_msi_domain_alloc(struct irq_domain *domain,
					 unsigned int virq,
					 unsigned int nr_irqs, void *args)
{
	/* [한국어] 도메인 생성 때 host_data 로 넣은 컨트롤러 문맥. */
	struct mobiveil_pcie *pcie = domain->host_data;
	/* [한국어] 벡터 비트맵이 들어 있는 MSI 상태 묶음. */
	struct mobiveil_msi *msi = &pcie->rp.msi;
	/* [한국어] find_first_zero_bit 의 반환 타입에 맞춘 변수. */
	unsigned long bit;

	/* [한국어] 이 구현은 한 번에 한 벡터만 다룬다. 여러 개를 요청받으면 경고를 남기되
	 * 계속 진행하므로, 그 경우 실제로는 첫 벡터만 제대로 설정된다.
	 * 다만 MSI_FLAG_ 설정상 다중 벡터 요청이 여기까지 오는지는
	 * 이 트리에서 확인하지 못했다. */
	WARN_ON(nr_irqs != 1);
	/* [한국어] 비트맵 조작을 직렬화한다. 두 장치가 동시에 MSI 를 요청하면
	 * 같은 빈 비트를 둘 다 집어갈 수 있기 때문이다. */
	mutex_lock(&msi->lock);

	/* [한국어] 아직 쓰이지 않은 첫 벡터 번호를 찾는다. */
	bit = find_first_zero_bit(msi->msi_irq_in_use, msi->num_of_vectors);
	/* [한국어] 찾은 값이 벡터 수 이상이라는 것은 빈 자리가 없다는 뜻이다
	 * (find_first_zero_bit 는 못 찾으면 상한을 돌려준다). */
	if (bit >= msi->num_of_vectors) {
		/* [한국어] 실패 경로에서도 반드시 락을 풀어야 한다. */
		mutex_unlock(&msi->lock);
		/* [한국어] 남은 벡터가 없음을 알린다. */
		return -ENOSPC;
	}

	/* [한국어] 찾은 비트를 사용 중으로 표시한다. 락 안이므로 원자적 판을 쓸 필요는
	 * 없지만 커널 관례를 따른다. */
	set_bit(bit, msi->msi_irq_in_use);

	/* [한국어] 표시가 끝났으니 락을 푼다. 아래 도메인 설정은 이 비트맵을 건드리지 않는다. */
	mutex_unlock(&msi->lock);

	/* [한국어] virq 에 bottom chip 과 레벨 흐름 핸들러를 걸고 hwirq 로 벡터 번호를 심는다.
	 * 이 hwirq 가 곧 mobiveil_compose_msi_msg() 가 쓰는 벡터 번호이자
	 * ISR 이 FIFO 에서 꺼내는 값이 된다. */
	irq_domain_set_info(domain, virq, bit, &mobiveil_msi_bottom_irq_chip,
			    /* [한국어] chip_data 로 컨트롤러 문맥을 넘겨, compose_msi_msg 가 되찾을 수 있게 한다. */
			    domain->host_data, handle_level_irq, NULL, NULL);
	/* [한국어] 할당 성공. */
	return 0;
}

/* [한국어] mobiveil_irq_msi_domain_free - MSI 벡터 하나를 반납한다.
 * 
 * @domain: MSI 부모 도메인.
 * @virq: 반납할 Linux IRQ 번호.
 * @nr_irqs: 반납 개수(이 구현은 쓰지 않는다).
 * @return: 없음.
 * 
 * 위 alloc 의 짝이다. virq 로부터 hwirq(벡터 번호)를 되찾아 비트맵에서 지운다.
 * 
 * 이미 비어 있는 벡터를 반납하려 하면 지우지 않고 오류만 남긴다 —
 * 이중 해제로 남의 벡터를 지우는 사고를 막으려는 것이다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다.
 * 호출자: MSI 도메인 코어(irq_domain_free_irqs 경유).
 * 피호출자: irq_domain_get_irq_data, test_bit, __clear_bit.
 * 에러 경로: 이중 해제는 로그만 남기고 조용히 넘어간다 — 반환값이 없어
 *   호출자에게 알릴 방법이 없기 때문이다.
 * 
 * 호출 체인:
 *   pci_free_irq_vectors() → MSI 코어 → domain->ops->free → [이 함수] */
static void mobiveil_irq_msi_domain_free(struct irq_domain *domain,
					 unsigned int virq,
					 unsigned int nr_irqs)
{
	/* [한국어] virq 에 대응하는 irq_data 를 얻는다. 여기에 hwirq(벡터 번호)가 들어 있다. */
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	/* [한국어] alloc 이 심어 둔 chip_data 에서 컨트롤러 문맥을 되찾는다. */
	struct mobiveil_pcie *pcie = irq_data_get_irq_chip_data(d);
	/* [한국어] 벡터 비트맵이 있는 MSI 상태 묶음. */
	struct mobiveil_msi *msi = &pcie->rp.msi;

	/* [한국어] 비트맵 조작을 직렬화한다. */
	mutex_lock(&msi->lock);

	/* [한국어] 반납하려는 벡터가 실제로 쓰이고 있었는지 확인한다. */
	if (!test_bit(d->hwirq, msi->msi_irq_in_use))
		/* [한국어] 쓰이지 않던 벡터를 반납하려 한 경우 — 이중 해제 같은 상위 계층의
		 * 버그를 알린다. 비트는 건드리지 않는다. */
		dev_err(&pcie->pdev->dev, "trying to free unused MSI#%lu\n",
			d->hwirq);
	else
		/* [한국어] 정상 반납. 락 안이므로 원자적이지 않은 __clear_bit 로 충분하다. */
		__clear_bit(d->hwirq, msi->msi_irq_in_use);

	/* [한국어] 락을 푼다. */
	mutex_unlock(&msi->lock);
}
/* [한국어] MSI 부모 도메인의 연산 표. 할당과 반납 둘만 둔다.
 * 나머지는 msi_create_parent_irq_domain 이 채운다. */
static const struct irq_domain_ops msi_domain_ops = {
	/* [한국어] 벡터 할당. */
	.alloc	= mobiveil_irq_msi_domain_alloc,
	/* [한국어] 벡터 반납. */
	.free	= mobiveil_irq_msi_domain_free,
};

/* [한국어] mobiveil_allocate_msi_domains - MSI 부모 IRQ 도메인을 만든다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 0 성공, -ENOMEM 이면 도메인 생성 실패.
 * 
 * 왜 필요한가: 장치가 MSI 를 요청하면 커널은 계층 구조를 따라 도메인을
 * 찾아 내려온다. 이 함수가 그 계층의 한 마디 — 이 컨트롤러를 대표하는
 * 부모 도메인 — 를 만들어 등록한다.
 * 
 * 동작: 벡터 비트맵을 지킬 뮤텍스를 초기화하고, 도메인 정보를 채워
 * msi_create_parent_irq_domain() 에 넘긴다. 크기로는 앞서
 * mobiveil_pcie_enable_msi() 가 설정한 num_of_vectors 를 쓴다 —
 * 그래서 두 함수의 호출 순서가 중요하다.
 * 
 * fwnode 로 이 컨트롤러의 device 노드를 쓰므로, DT 에서 msi-parent 가
 * 이 노드를 가리키면 자식 장치의 MSI 요청이 여기로 찾아온다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: mobiveil_pcie_init_irq_domain().
 * 피호출자: mutex_init(), msi_create_parent_irq_domain().
 * 에러 경로: 실패하면 로그를 남기고 -ENOMEM. 위로 전파되어 probe 가 접힌다.
 *   이미 초기화한 뮤텍스는 별도 정리가 필요 없다.
 * 
 * 호출 체인:
 *   mobiveil_pcie_integrated_interrupt_init() → mobiveil_pcie_init_irq_domain()
 *     → [이 함수] → msi_create_parent_irq_domain() */
static int mobiveil_allocate_msi_domains(struct mobiveil_pcie *pcie)
{
	/* [한국어] 오류 로그와 fwnode 를 얻을 device. */
	struct device *dev = &pcie->pdev->dev;
	/* [한국어] 초기화할 MSI 상태 묶음. */
	struct mobiveil_msi *msi = &pcie->rp.msi;

	/* [한국어] 벡터 비트맵을 지킬 뮤텍스를 초기화한다.
	 * 도메인을 만들기 전에 해야 한다 — 도메인이 등록되는 순간부터
	 * alloc 콜백이 불릴 수 있기 때문이다. */
	mutex_init(&msi->lock);

	/* [한국어] 도메인 정보를 지역 변수로 조립한다. 선언이 실행문 뒤에 오는데,
	 * 커널이 C11 을 허용하면서 가능해진 형태다. */
	struct irq_domain_info info = {
		/* [한국어] 이 컨트롤러의 firmware 노드. DT 의 msi-parent 가 이 노드를 가리키면
		 * 자식 장치의 MSI 요청이 이 도메인으로 온다. */
		.fwnode		= dev_fwnode(dev),
		/* [한국어] 위에서 정의한 alloc / free 콜백 표. */
		.ops		= &msi_domain_ops,
		/* [한국어] 콜백들이 되찾아 쓸 컨트롤러 문맥. */
		.host_data	= pcie,
		/* [한국어] 도메인 크기 — 곧 지원 벡터 수다.
		 * mobiveil_pcie_enable_msi() 가 먼저 불려 이 값이 채워져 있어야 한다. */
		.size		= msi->num_of_vectors,
	};

	/* [한국어] 부모 MSI 도메인을 만든다. 위 mobiveil_msi_parent_ops 가 함께 등록되어,
	 * 나중에 장치별 자식 도메인이 만들어질 때 그 규약이 적용된다. */
	msi->dev_domain = msi_create_parent_irq_domain(&info, &mobiveil_msi_parent_ops);
	/* [한국어] 생성 실패 — 메모리 부족이나 fwnode 중복 등이 원인일 수 있다. */
	if (!msi->dev_domain) {
		/* [한국어] 실패를 남긴다. */
		dev_err(dev, "failed to create MSI domain\n");
		/* [한국어] MSI 없이는 이 드라이버가 제구실을 못하므로 probe 를 접게 한다. */
		return -ENOMEM;
	}

	/* [한국어] MSI 도메인 준비 완료. */
	return 0;
}

/* [한국어] mobiveil_pcie_init_irq_domain - INTx 와 MSI 도메인을 모두 만든다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 0 성공, -ENOMEM 이면 둘 중 하나의 도메인 생성 실패.
 * 
 * 왜 필요한가: 이 컨트롤러 아래의 장치들이 인터럽트를 쓰려면 두 가지
 * 경로가 모두 열려 있어야 한다 — 레거시 INTx 와 MSI.
 * 이 함수가 그 둘의 도메인을 차례로 만든다.
 * 
 * 동작: 크기 PCI_NUM_INTX(4)의 선형 INTx 도메인을 만들고,
 * INTx 마스크 락을 초기화한 뒤, MSI 도메인 생성을 위임한다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: mobiveil_pcie_integrated_interrupt_init().
 * 피호출자: irq_domain_create_linear(), raw_spin_lock_init(),
 *   mobiveil_allocate_msi_domains().
 * 에러 경로: INTx 도메인 생성 실패 시 로그를 남기고 -ENOMEM.
 *   MSI 쪽 실패는 하위 함수의 반환값을 그대로 올린다.
 *   주의: MSI 쪽이 실패해도 이미 만든 INTx 도메인을 되돌리는 코드가 없다.
 *   다만 그 경우 probe 전체가 실패해 드라이버가 붙지 않는다.
 * 
 * 호출 체인:
 *   mobiveil_pcie_integrated_interrupt_init() → [이 함수]
 *     → mobiveil_allocate_msi_domains() */
static int mobiveil_pcie_init_irq_domain(struct mobiveil_pcie *pcie)
{
	/* [한국어] fwnode 와 로그에 쓸 device. */
	struct device *dev = &pcie->pdev->dev;
	/* [한국어] 도메인과 락이 들어갈 루트 포트 상태. */
	struct mobiveil_root_port *rp = &pcie->rp;

	/* setup INTx */
	/* [한국어] INTx 용 선형 도메인을 만든다. 크기 PCI_NUM_INTX 는 4 이므로
	 * 이 도메인이 받아 주는 hwirq 는 0~3 이다.
	 * 반면 이 드라이버의 ISR 과 마스크 함수는 hwirq 를 1~4 로 다룬다 —
	 * 그 어긋남에 대해서는 mobiveil_pcie_isr() 의 hwirq 계산 주석 참조.
	 * host_data 로 컨트롤러 문맥을 넘겨 map 콜백이 chip_data 로 심게 한다. */
	rp->intx_domain = irq_domain_create_linear(dev_fwnode(dev), PCI_NUM_INTX, &intx_domain_ops,
						   pcie);
	/* [한국어] 도메인 생성 실패. */
	if (!rp->intx_domain) {
		/* [한국어] 실패를 남긴다. */
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		/* [한국어] INTx 없이는 진행할 수 없으므로 probe 를 접게 한다. */
		return -ENOMEM;
	}

	/* [한국어] INTx 마스크 레지스터 접근을 지킬 락을 초기화한다.
	 * 도메인을 만든 뒤이지만, 마스크 함수가 불리는 것은 실제로 매핑이
	 * 생긴 뒤라 순서상 문제가 없다. */
	raw_spin_lock_init(&rp->intx_mask_lock);

	/* setup MSI */
	/* [한국어] MSI 도메인 생성은 별도 함수에 맡기고 그 결과를 그대로 올린다. */
	return mobiveil_allocate_msi_domains(pcie);
}

/* [한국어] mobiveil_pcie_integrated_interrupt_init - 이 IP 내장 인터럽트 블록으로 초기화한다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 0 성공, 음수 errno 실패.
 * 
 * 왜 필요한가: SoC 가 별도의 인터럽트 컨트롤러를 두지 않는 경우, INTx 와 MSI 를
 * 이 IP 자신이 처리해야 한다. 이 함수가 그 경로를 통째로 세운다.
 * 
 * 동작 단계:
 *   1) 'apb_csr' 리소스를 매핑한다 — MSI 레지스터 블록이다.
 *      DT 파싱 함수가 아니라 여기서 매핑하는 것은, 이 리소스가
 *      내장 인터럽트 경로에서만 필요하기 때문이다.
 *   2) MSI 하드웨어를 켠다(벡터 수도 여기서 정해진다).
 *   3) 상위 인터럽트 컨트롤러가 준 IRQ 번호를 얻는다.
 *   4) INTx / MSI 도메인을 만든다.
 *   5) 그 IRQ 에 체인 핸들러를 건다.
 *   6) INTx 와 MSI 소스를 활성화 레지스터에서 켠다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: mobiveil_pcie_interrupt_init() (SoC 콜백이 없을 때의 기본 경로).
 * 피호출자: devm_pci_remap_cfg_resource, mobiveil_pcie_enable_msi,
 *   platform_get_irq, mobiveil_pcie_init_irq_domain,
 *   irq_set_chained_handler_and_data, mobiveil_csr_writel.
 * 에러 경로: 각 단계에서 바로 반환한다. 이미 만든 도메인을 되돌리는 코드는
 *   없지만, 실패하면 probe 전체가 접히므로 드라이버가 붙지 않는다.
 * 
 * 호출 체인:
 *   mobiveil_pcie_host_probe() → mobiveil_pcie_interrupt_init() → [이 함수] */
static int mobiveil_pcie_integrated_interrupt_init(struct mobiveil_pcie *pcie)
{
	/* [한국어] 리소스와 IRQ 를 꺼낼 platform 장치. */
	struct platform_device *pdev = pcie->pdev;
	/* [한국어] 매핑과 로그의 주인이 될 device. */
	struct device *dev = &pdev->dev;
	/* [한국어] IRQ 번호와 도메인이 들어갈 루트 포트 상태. */
	struct mobiveil_root_port *rp = &pcie->rp;
	/* [한국어] 리소스 포인터를 담을 변수. */
	struct resource *res;
	/* [한국어] 하위 함수의 반환값을 담을 변수. */
	int ret;

	/* map MSI config resource */
	/* [한국어] MSI 레지스터 블록에 해당하는 리소스를 이름으로 찾는다. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "apb_csr");
	/* [한국어] 매핑한다. 이 베이스는 mobiveil_pcie_enable_msi() 와 ISR 의 MSI 처리에서
	 * readl_relaxed / writel_relaxed 로 직접 쓰인다 — 즉 이 영역은 페이지
	 * 방식 접근자를 거치지 않는다. */
	pcie->apb_csr_base = devm_pci_remap_cfg_resource(dev, res);
	/* [한국어] 리소스 부재와 매핑 실패가 함께 걸린다. */
	if (IS_ERR(pcie->apb_csr_base))
		/* [한국어] 오류를 그대로 올린다. */
		return PTR_ERR(pcie->apb_csr_base);

	/* setup MSI hardware registers */
	/* [한국어] MSI 창 주소와 크기를 하드웨어에 알리고 수신을 켠다.
	 * 반드시 도메인 생성보다 먼저 와야 한다 — 여기서 정해지는
	 * num_of_vectors 가 도메인 크기가 되기 때문이다. */
	mobiveil_pcie_enable_msi(pcie);

	/* [한국어] 상위 인터럽트 컨트롤러가 이 컨트롤러에 배정한 IRQ 번호를 얻는다.
	 * 이름이 아니라 인덱스 0 으로 찾는데, 이 경로에서는 인터럽트가 하나뿐이기 때문이다. */
	rp->irq = platform_get_irq(pdev, 0);
	/* [한국어] 실패했거나 아직 상위 컨트롤러가 준비되지 않은 경우 음수가 온다. */
	if (rp->irq < 0)
		/* [한국어] -EPROBE_DEFER 를 포함할 수 있으므로 값을 그대로 전달한다 —
		 * 다른 errno 로 바꾸면 나중에 재시도할 기회를 잃는다. */
		return rp->irq;

	/* initialize the IRQ domains */
	/* [한국어] INTx 와 MSI 도메인을 만든다. */
	ret = mobiveil_pcie_init_irq_domain(pcie);
	/* [한국어] 도메인 생성 실패. */
	if (ret) {
		/* [한국어] 실패를 남긴다. */
		dev_err(dev, "Failed creating IRQ Domain\n");
		/* [한국어] probe 를 접게 한다. */
		return ret;
	}

	/* [한국어] 이 IRQ 를 체인 핸들러로 전환한다. 일반 request_irq 와 달리,
	 * 상위 인터럽트가 올라오면 이 핸들러가 하위 소스를 판별해 각각의
	 * 도메인으로 다시 분배하는 구조가 된다.
	 * 마지막 인자 pcie 가 ISR 안에서 irq_desc_get_handler_data() 로 되돌아온다. */
	irq_set_chained_handler_and_data(rp->irq, mobiveil_pcie_isr, pcie);

	/* Enable interrupts */
	/* [한국어] INTx 네 핀과 MSI 를 활성화 레지스터에서 켠다.
	 * 핸들러를 먼저 걸고 나서 켜는 순서가 중요하다 — 반대였다면
	 * 핸들러가 없는 상태에서 인터럽트가 올라올 수 있다.
	 * 읽고-고치고-쓰기가 아니라 덮어쓰기이므로, 이 값이 곧 켜지는 소스의 전부다. */
	mobiveil_csr_writel(pcie, (PAB_INTP_INTX_MASK | PAB_INTP_MSI_MASK),
			    PAB_INTP_AMBA_MISC_ENB);


	/* [한국어] 인터럽트 경로 준비 완료. */
	return 0;
}

/* [한국어] mobiveil_pcie_interrupt_init - 인터럽트 초기화 경로를 고른다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 고른 경로가 돌려준 값 그대로.
 * 
 * 왜 필요한가: SoC 에 따라 인터럽트를 처리하는 주체가 다르다.
 * MSI 를 외부 컨트롤러에 맡기는 SoC 는 자체 도메인을 만들 필요가 없다.
 * 이 함수가 그 갈림길이다.
 * 
 * 동작: rp->ops->interrupt_init 이 있으면 그것을, 없으면 내장 경로를 부른다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: mobiveil_pcie_host_probe().
 * 피호출자: rp->ops->interrupt_init 또는
 *   mobiveil_pcie_integrated_interrupt_init().
 * 에러 경로: 고른 경로의 반환값을 그대로 올린다.
 * 
 * 호출 체인:
 *   mobiveil_pcie_host_probe() → [이 함수]
 *     → ls_g4_pcie_interrupt_init() 또는
 *       mobiveil_pcie_integrated_interrupt_init() */
static int mobiveil_pcie_interrupt_init(struct mobiveil_pcie *pcie)
{
	/* [한국어] 콜백 표가 들어 있는 루트 포트 상태. */
	struct mobiveil_root_port *rp = &pcie->rp;

	/* [한국어] SoC 가 인터럽트 초기화를 대신할 콜백을 두었는지 본다.
	 * 
	 * 주의(코드에서 확인되는 사실): 이 줄은 rp->ops 자체가 NULL 인지 검사하지
	 * 않고 곧바로 rp->ops->interrupt_init 을 읽는다.
	 * mobiveil 디렉터리 전체를 주석 제거 후 훑으면 rp.ops 에 값을 넣는 곳은
	 * pcie-layerscape-gen4.c 의 ls_g4_pcie_probe() 한 군데뿐이다.
	 * 같은 디렉터리의 pcie-mobiveil-plat.c(mbvl,gpex40-pcie)는 rp.ops 를
	 * 채우지 않는데, 그 드라이버도 mobiveil_pcie_host_probe() 를 통해
	 * 이 함수에 도달한다. 브리지 private 영역은 0 으로 채워져 할당되므로
	 * 그 경로에서 rp->ops 는 NULL 이다.
	 * 같은 형태가 pcie-mobiveil.c 의 mobiveil_pcie_link_up() 에도 있다
	 * (pcie->ops->link_up).
	 * 이것이 실제로 도달 가능한 경로인지 — 즉 mbvl,gpex40-pcie 가 쓰이는
	 * 하드웨어가 있는지 — 는 이 트리 안에서 판단할 근거를 찾지 못했다. */
	if (rp->ops->interrupt_init)
		/* [한국어] SoC 구현에 맡기고 그 결과를 그대로 올린다.
		 * gen4 의 경우 'intr' IRQ 하나만 잡고 끝난다 — 도메인은 만들지 않는다. */
		return rp->ops->interrupt_init(pcie);

	/* [한국어] 콜백이 없으면 이 IP 내장 인터럽트 블록으로 초기화한다. */
	return mobiveil_pcie_integrated_interrupt_init(pcie);
}

/* [한국어] mobiveil_pcie_is_bridge - 이 컨트롤러가 PCI 브리지 헤더를 내보이는지 확인한다.
 * 
 * @pcie: 대상 컨트롤러.
 * @return: 헤더 타입이 브리지(1)면 true.
 * 
 * 왜 필요한가: 이 드라이버는 RC 를 PCI-to-PCI 브리지로 다룬다.
 * 하드웨어가 엉뚱한 헤더 타입을 내보이면 이후의 버스 번호 설정과 열거가
 * 의미를 잃으므로, probe 초반에 확인하고 아니면 물러난다.
 * 
 * 동작: config 헤더의 header type 바이트를 읽어 multi-function 비트(0x80)를
 * 제외한 하위 7비트를 브리지 코드와 비교한다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트. DT 파싱이 끝나
 * csr_axi_slave_base 가 매핑된 뒤에만 유효하다.
 * 호출자: mobiveil_pcie_host_probe().
 * 피호출자: mobiveil_csr_readb().
 * 에러 경로: 판정만 하고 오류는 만들지 않는다. 거짓이면 호출자가
 *   -ENODEV 로 probe 를 접는다.
 * 
 * 호출 체인:
 *   mobiveil_pcie_host_probe() → [이 함수] → mobiveil_csr_readb() */
static bool mobiveil_pcie_is_bridge(struct mobiveil_pcie *pcie)
{
	/* [한국어] 읽은 헤더 타입 바이트를 담을 변수. */
	u32 header_type;

	/* [한국어] config 헤더의 0x0e 오프셋을 1바이트로 읽는다.
	 * 루트 버스 접근이므로 csr_axi_slave 영역이 곧 config 공간 역할을 한다. */
	header_type = mobiveil_csr_readb(pcie, PCI_HEADER_TYPE);
	/* [한국어] 최상위 비트(0x80)는 다기능 장치 표시라 헤더 타입 자체와 무관하다.
	 * 그것을 뺀 하위 7비트만 남긴다. */
	header_type &= PCI_HEADER_TYPE_MASK;

	/* [한국어] 헤더 타입 1 은 PCI-to-PCI 브리지를 뜻한다(0 은 일반 장치, 2 는 CardBus). */
	return header_type == PCI_HEADER_TYPE_BRIDGE;
}

/* [한국어] mobiveil_pcie_host_probe - 공통 호스트 초기화 전 과정을 진행한다.
 * 
 * @pcie: SoC 드라이버가 pdev 와 rp.bridge (그리고 쓸 경우 두 ops)를
 *   채워 넘긴 컨트롤러.
 * @return: 0 성공, 음수 errno 실패.
 * 
 * 왜 필요한가: 두 SoC 드라이버가 공유하는 초기화 절차를 한곳에 모은 것이다.
 * SoC 드라이버는 자기 고유의 준비만 하고 나머지를 이 함수에 위임한다.
 * 
 * 동작 단계:
 *   1) DT 에서 리소스와 창 개수를 읽는다.
 *   2) 하드웨어가 브리지 헤더를 내보이는지 확인한다.
 *   3) 창과 PIO 를 설정한다(mobiveil_host_init, reinit=false).
 *   4) 인터럽트 경로를 세운다(SoC 콜백이 있으면 그쪽).
 *   5) 브리지에 sysdata 와 config 연산 표를 건다.
 *   6) 링크가 올라오기를 기다린다.
 *   7) PCI 코어에 넘겨 버스를 열거한다.
 * 
 * 순서에 이유가 있다. 3)이 4)보다 먼저인 것은 인터럽트 초기화가
 * config 접근을 하지 않기 때문이 아니라, 창이 없으면 아무 접근도
 * 성립하지 않기 때문이다. 5)가 7) 직전인 것은 pci_host_probe() 가
 * bridge->ops 로 config 접근을 시작하기 때문이다.
 * 
 * 실행 컨텍스트: probe 중 프로세스 컨텍스트.
 * 호출자: pcie-layerscape-gen4.c 의 ls_g4_pcie_probe(),
 *   pcie-mobiveil-plat.c 의 mobiveil_pcie_probe().
 * 피호출자: mobiveil_pcie_parse_dt, mobiveil_pcie_is_bridge,
 *   mobiveil_host_init, mobiveil_pcie_interrupt_init,
 *   mobiveil_bringup_link, pci_host_probe.
 * 에러 경로: 각 단계에서 바로 반환한다. 리소스는 devm_ 으로 잡았으므로
 *   되돌림 코드가 없고, 만들어 둔 IRQ 도메인은 정리되지 않는다 —
 *   다만 실패하면 드라이버가 붙지 않으므로 그 도메인에 도달할 경로도 없다.
 * 
 * 호출 체인:
 *   각 SoC probe → [이 함수] → mobiveil_host_init() /
 *     mobiveil_pcie_interrupt_init() / mobiveil_bringup_link() / pci_host_probe() */
int mobiveil_pcie_host_probe(struct mobiveil_pcie *pcie)
{
	/* [한국어] SoC 드라이버가 채워 준 루트 포트 상태. */
	struct mobiveil_root_port *rp = &pcie->rp;
	/* [한국어] SoC 드라이버가 할당해 rp.bridge 에 넣어 둔 호스트 브리지. */
	struct pci_host_bridge *bridge = rp->bridge;
	/* [한국어] 오류 로그 대상. */
	struct device *dev = &pcie->pdev->dev;
	/* [한국어] 각 단계의 반환값을 담을 변수. */
	int ret;

	/* [한국어] 먼저 DT 에서 두 MMIO 영역을 매핑한다. 이것이 끝나야
	 * 이후의 모든 레지스터 접근이 성립한다. */
	ret = mobiveil_pcie_parse_dt(pcie);
	/* [한국어] 리소스가 없거나 매핑에 실패한 경우. */
	if (ret) {
		/* [한국어] 실패를 남긴다. ret 를 16진수로 찍는 것은 상류의 선택이다. */
		dev_err(dev, "Parsing DT failed, ret: %x\n", ret);
		/* [한국어] 오류를 그대로 올린다. */
		return ret;
	}

	/* [한국어] 하드웨어가 브리지 헤더를 내보이지 않으면 이 드라이버가 다룰 대상이 아니다.
	 * 창이나 인터럽트를 건드리기 전에 확인하는 것이 안전하다. */
	if (!mobiveil_pcie_is_bridge(pcie))
		/* [한국어] 장치 없음으로 물러난다. 이 경우에는 로그를 남기지 않는다. */
		return -ENODEV;

	/*
	 * configure all inbound and outbound windows and prepare the RC for
	 * config access
	 */
	/* [한국어] 창과 PIO 를 세운다. false 는 첫 초기화라는 뜻이라 버스 번호도 함께 설정된다. */
	ret = mobiveil_host_init(pcie, false);
	/* [한국어] 이 구현은 언제나 0 을 돌려주므로 이 분기는 사실상 죽은 코드지만,
	 * 나중에 실패 경로가 생겨도 호출자가 대응할 수 있게 남겨 둔 형태다. */
	if (ret) {
		/* [한국어] 실패를 남긴다. */
		dev_err(dev, "Failed to initialize host\n");
		/* [한국어] 오류를 올린다. */
		return ret;
	}

	/* [한국어] 인터럽트 경로를 세운다. SoC 콜백이 있으면 그쪽으로 갈라진다. */
	ret = mobiveil_pcie_interrupt_init(pcie);
	/* [한국어] IRQ 를 못 얻었거나 도메인 생성에 실패한 경우. */
	if (ret) {
		/* [한국어] 실패를 남긴다. */
		dev_err(dev, "Interrupt init failed\n");
		/* [한국어] 오류를 올린다. -EPROBE_DEFER 였다면 그대로 전달되어 나중에 재시도된다. */
		return ret;
	}

	/* Initialize bridge */
	/* [한국어] config 접근 콜백이 bus->sysdata 로 되찾을 문맥을 심는다.
	 * 이 대입이 없으면 mobiveil_pcie_map_bus() 가 NULL 을 역참조한다. */
	bridge->sysdata = pcie;
	/* [한국어] config 접근 연산 표를 건다. 아래 pci_host_probe() 가 이 표로
	 * 버스를 열거하기 시작하므로, 반드시 그 전에 걸려 있어야 한다. */
	bridge->ops = &mobiveil_pcie_ops;

	/* [한국어] 링크가 올라오기를 기다린다. 열거보다 먼저 해야 하는 이유는 분명하다 —
	 * 링크가 없으면 아래 장치의 config 접근이 모두 실패한다. */
	ret = mobiveil_bringup_link(pcie);
	/* [한국어] 시간 안에 링크가 올라오지 않은 경우. */
	if (ret) {
		/* [한국어] dev_err 가 아니라 dev_info 로 남긴다. 슬롯이 비어 있어 링크가
		 * 올라오지 않는 것은 정상적인 상황일 수 있기 때문이다. */
		dev_info(dev, "link bring-up failed\n");
		/* [한국어] 그럼에도 probe 는 실패로 끝난다 — 즉 장치가 없는 슬롯이면
		 * 이 컨트롤러는 아예 등록되지 않는다. */
		return ret;
	}

	/* [한국어] PCI 코어에 넘겨 버스를 열거하고 장치들을 등록한다.
	 * 이 호출 안에서 위에서 건 mobiveil_pcie_ops 를 통해 수많은 config 접근이
	 * 일어나고, 그 하나하나가 mobiveil_pcie_map_bus() 를 거쳐 BDF 창을 다시 조준한다.
	 * 반환값이 곧 이 함수의 결과가 된다. */
	return pci_host_probe(bridge);
}
