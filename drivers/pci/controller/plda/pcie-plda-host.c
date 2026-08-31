// SPDX-License-Identifier: GPL-2.0
/*
 * PLDA PCIe XpressRich host controller driver
 *
 * Copyright (C) 2023 Microchip Co. Ltd
 *		      StarFive Co. Ltd
 *
 * Author: Daire McNamara <daire.mcnamara@microchip.com>
 */

/*
 * [한국어 설명] PLDA XpressRich PCIe 호스트 컨트롤러 공용 코어 (pcie-plda-host.c)
 *
 * === 파일의 역할 ===
 * PLDA XpressRich IP 를 쓰는 SoC 드라이버들이 공유하는 실제 구현 본체다. 하는
 * 일은 크게 세 덩어리다. (1) 인터럽트 -- 이벤트/INTx/MSI 세 개의 irq_domain 을
 * 만들고, SoC 의 단일 부모 인터럽트 하나에서 시작해 세 갈래로 갈라지는 chained
 * handler 사슬을 세운다. (2) 주소 변환 -- AXI 주소를 PCIe 주소로 바꾸는 ATR
 * (Address Translation) 창을 config 용 0번과 메모리 자원용 1번 이후로 프로그래밍한다.
 * (3) 호스트 등록 -- APB/cfg 레지스터 창을 ioremap 하고 pci_host_bridge 를 만들어
 * pci_host_probe() 로 커널 PCI 코어에 넘긴다. 이 파일은 CONFIG_PCIE_PLDA_HOST 로
 * 빌드되며 그 심볼은 bool 이라 항상 커널 내장이고, 일곱 개의 함수를
 * EXPORT_SYMBOL_GPL 로 모듈(pcie-starfive.ko)에 노출한다.
 * 코드 자체에는 SoC 이름이나 SoC 전용 레지스터가 전혀 나오지 않는다 -- 그런 것은
 * 전부 pcie-plda.h 의 ops 훅을 통해 바깥에서 주입된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 위에서 아래로: [PCI 코어 pci_host_probe/probe.c] - [이 파일] - [SoC 드라이버] -
 * [PLDA 하드웨어]. 다만 진입 방향은 SoC 마다 다르다.
 *  - StarFive 경로: starfive_pcie_probe() -> plda_pcie_host_init() -> (SoC host_init
 *    훅) -> ATR 창 -> plda_init_interrupts() -> pci_host_probe(). 이 파일이 probe 의
 *    주도권을 갖는 "프레임워크" 로 동작한다.
 *  - Microchip 경로: mc_host_probe() -> pci_host_common_probe()(ECAM 공용 코어) ->
 *    pci_ecam_create() -> ops->init = mc_platform_init() -> 여기서 이 파일의
 *    plda_pcie_setup_window(), plda_pcie_setup_iomems(), plda_init_interrupts() 만
 *    골라 부른다. 이 파일은 "헬퍼 라이브러리" 로만 쓰이고 plda_pcie_host_init() 은
 *    아예 호출되지 않는다.
 * 인터럽트 관점에서 이 파일은 SoC 인터럽트 컨트롤러(RISC-V PLIC 또는 ARM GIC)와
 * 개별 PCIe 장치 드라이버 사이의 중계자다. 실행 컨텍스트는 두 종류로 뚜렷이
 * 갈린다 -- probe/remove 계열은 프로세스 컨텍스트에서 잠들 수 있고, plda_handle_
 * 로 시작하는 세 체인 핸들러와 irq_chip 콜백들은 하드 인터럽트 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * 아래 방향 의존: pcie-plda.h(레지스터 맵과 자료구조), linux/irqdomain.h 와
 * linux/msi.h(도메인 계층), irqchip/chained_irq.h(체인 핸들러 진입/퇴장 규약),
 * irqchip/irq-msi-lib.h(MSI 부모 도메인 표준 구현), linux/pci-ecam.h(config 주소
 * 계산 매크로), 그리고 drivers/pci/probe.c 의 pci_host_probe().
 * 위 방향 의존자: pcie-starfive.c(일곱 EXPORT 심볼 중 map_bus, host_init,
 * host_deinit 사용)와 pcie-microchip-host.c(setup_window, setup_iomems,
 * init_interrupts 사용). EXPORT 된 일곱 심볼 중
 * plda_pcie_setup_inbound_address_translation() 만 이 트리 안에 호출자가 없다.
 * 데이터 흐름: SoC 드라이버가 채운 struct plda_pcie_rp 가 아래로 내려오고, 이
 * 파일은 거기에 irq 도메인 포인터들과 매핑된 레지스터 주소를 되채운다. 인터럽트
 * 데이터는 반대로 ISTATUS_LOCAL 레지스터 -> 이벤트 비트맵 -> event_domain ->
 * (INTx/MSI 인 경우 한 단계 더) intx_domain / msi.dev_domain -> 장치 핸들러 순으로
 * 흐른다. NVMe 관련: 이 파일에 nvme 식별자는 한 건도 없다. PLDA 루트 포트에 꽂힌
 * NVMe SSD 는 여기서 만든 MSI 부모 도메인 아래에 자기 MSI 도메인을 만들게 되지만,
 * 그 연결은 커널 irqdomain 계층이 중개하는 것이지 이 파일이 nvme 코드를 부르는
 * 것이 아니다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - plda_pcie_host_init() : StarFive 경로의 진입점. ioremap -> SoC host_init ->
 *    ATR 창 -> 인터럽트 -> pci_host_probe 까지 한 번에 수행한다.
 *  - plda_init_interrupts() : 이벤트/INTx/MSI 도메인 생성과 체인 핸들러 연결.
 *    두 SoC 모두가 반드시 거치는 공통 지점이다.
 *  - plda_handle_event() / plda_handle_intx() / plda_handle_msi() : 체인 핸들러 3종.
 *    각각 상위 이벤트, INTA~INTD, MSI 벡터를 자기 도메인으로 분배한다.
 *  - plda_get_events() / plda_hwirq_to_mask() : 레지스터 비트 <-> 이벤트 hwirq
 *    번호 사이의 정방향/역방향 변환. INTx 4비트를 1비트로 접는 규칙이 핵심이다.
 *  - plda_pcie_setup_window() : ATR 창 하나를 프로그래밍하는 핵심 주소 변환 함수.
 *  - plda_msi_bottom_irq_chip / plda_intx_irq_chip / plda_event_irq_chip :
 *    세 계층 각각의 ack/mask/unmask 구현 테이블.
 */

/* [한국어] ALIGN_DOWN() 매크로 -- plda_pcie_setup_window() 이 ATR 소스 주소를 4KiB 경계로
 * 내림해 하위 12비트를 비우는 데 쓴다. 그 빈 자리에 크기 필드와 enable 비트가 들어간다. */
#include <linux/align.h>
/* [한국어] FIELD_PREP()/FIELD_GET() -- ATR_SIZE_MASK(비트 6:1) 같은 비트필드에 값을 넣을 때
 * 시프트 계산을 손으로 하지 않기 위해 필요하다. */
#include <linux/bitfield.h>
/* [한국어] chained_irq_enter()/chained_irq_exit() -- 부모 인터럽트를 물고 자식 인터럽트를
 * 직접 처리하는 'chained handler' 의 진입/퇴장 규약. 부모 irq_chip 의 mask/ack 를
 * 대신 호출해 주므로 세 handle_ 함수가 모두 이 짝을 쓴다. */
#include <linux/irqchip/chained_irq.h>
/* [한국어] msi_lib_init_dev_msi_info() -- MSI 부모 도메인이 자식(장치별) MSI 도메인을 만들
 * 때 쓰는 표준 구현. 이것을 쓰면 이 드라이버가 MSI 도메인 정보 구성을 직접
 * 작성하지 않아도 된다. */
#include <linux/irqchip/irq-msi-lib.h>
/* [한국어] irq_domain_create_linear(), irq_domain_set_info(), generic_handle_domain_irq() 등
 * irq 도메인 API 전체. 이 파일의 인터럽트 절반은 이 헤더에 의존한다. */
#include <linux/irqdomain.h>
/* [한국어] struct msi_msg, struct msi_parent_ops, msi_create_parent_irq_domain() 등 MSI 계층.
 * plda_compose_msi_msg() 가 채우는 구조체가 여기 정의되어 있다. */
#include <linux/msi.h>
/* [한국어] PCI 규격 레지스터 오프셋/비트 정의. 이 파일에서는 PCI_NUM_INTX 를 통해
 * 간접적으로 쓰이며, pcie-plda.h 의 PCI_BASE_ADDRESS_0 등도 여기서 온다. */
#include <linux/pci_regs.h>
/* [한국어] PCIE_ECAM_OFFSET() 매크로 -- plda_pcie_map_bus() 가 (bus, devfn, where) 를
 * ECAM 규칙(버스 20비트 시프트, devfn 12비트 시프트)에 따라 오프셋으로 바꾼다. */
#include <linux/pci-ecam.h>
/* [한국어] lower_32_bits()/upper_32_bits() -- 64비트 주소를 두 개의 32비트 레지스터에
 * 나눠 쓸 때마다 필요하다. ATR 설정과 MSI 메시지 주소 구성에 쓰인다. */
#include <linux/wordpart.h>

/* [한국어] 이 디렉터리의 공용 헤더. 레지스터 오프셋, 이벤트 번호 체계, struct plda_pcie_rp
 * 등 이 파일이 다루는 모든 것의 정의가 들어 있다. */
#include "pcie-plda.h"

/*
 * [한국어]
 * plda_pcie_map_bus - (버스, devfn, 오프셋) 을 config space 접근 주소로 바꾼다
 *
 * @bus: 접근 대상 PCI 버스. bus->sysdata 에 struct plda_pcie_rp 가 들어 있다
 *       (plda_pcie_host_init() 이 bridge->sysdata = port 로 심어 둔 것).
 * @devfn: 대상 장치/함수 번호(상위 5비트 device, 하위 3비트 function).
 * @where: config space 안의 바이트 오프셋(0..4095).
 * @return: 그 config 레지스터에 해당하는 커널 가상 주소. NULL 을 돌려주는 경로는
 *          없다 -- 즉 이 드라이버는 존재하지 않는 장치에 대한 접근도 일단 주소를
 *          만들어 주고, 실제 응답 없음은 하드웨어가 all-ones 로 처리한다.
 *
 * 왜 필요한가: 커널 PCI 코어는 config 읽기/쓰기를 pci_ops 의 map_bus -> read/write
 * 순서로 처리한다. 이 함수가 map_bus 역할이며, 실제 데이터 전송은
 * pci_generic_config_read / _write 가 담당한다. PLDA 브리지는 config 창을 ECAM
 * (Enhanced Configuration Access Mechanism, PCIe Base Spec 7.2.2) 규칙대로
 * 배치하므로, 주소 계산이 PCIE_ECAM_OFFSET 매크로 한 줄로 끝난다.
 *
 * 동작: config_base(= 'cfg' 리소스를 ioremap 한 주소)에
 * PCIE_ECAM_OFFSET(bus, devfn, where) = (bus << 20) | (devfn << 12) | (where & 0xfff)
 * 를 더한다. 버스 20비트, devfn 12비트 시프트는 ECAM 규격이 정한 값이다.
 *
 * 실행 컨텍스트: config 접근 경로 어디서나 불린다 -- 열거(프로세스 컨텍스트)뿐
 * 아니라 드라이버가 런타임에 config 를 읽을 때도 온다. 락은 잡지 않으며,
 * 직렬화는 상위 PCI 코어의 pci_lock 이 담당한다. 순수 주소 계산이라 재진입 안전하다.
 * 호출자: pci_generic_config_read()/write() 안에서 bus->ops->map_bus 로 간접 호출.
 * 이 트리에서 이 함수를 pci_ops 에 꽂는 곳은 pcie-starfive.c 의 starfive_pcie_ops
 * 하나뿐이다(Microchip 은 ECAM 코어의 pci_ecam_map_bus 를 쓴다).
 * 피호출자: 없음(매크로 연산뿐).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   PCI 코어 config 접근 -> starfive_pcie_config_read/write()
 *     -> pci_generic_config_read/write() -> [이 함수]
 */
void __iomem *plda_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				int where)
{
	struct plda_pcie_rp *pcie = bus->sysdata;

	/* [한국어] ECAM 규칙으로 오프셋을 계산해 config_base 에 더한다.
	 * PCIE_ECAM_OFFSET = (bus << 20) | (devfn << 12) | (where & 0xfff).
	 * bus->number 를 그대로 쓰므로 루트 버스 번호가 0 이 아닌 도메인에서는
	 * 창 배치가 그에 맞게 잡혀 있어야 한다. */
	return pcie->config_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);
}
EXPORT_SYMBOL_GPL(plda_pcie_map_bus);

/*
 * [한국어]
 * plda_handle_msi - MSI 묶음 인터럽트를 받아 벡터별 핸들러로 분배하는 체인 핸들러
 *
 * @desc: 'MSI 이벤트' virq 의 irq_desc. handler_data 에 struct plda_pcie_rp 가
 *        들어 있다(plda_init_interrupts 가 irq_set_chained_handler_and_data 로 심음).
 * @return: 없음(체인 핸들러는 반환값을 갖지 않는다).
 *
 * 왜 필요한가: PLDA 브리지는 최대 32개의 MSI 벡터를 받아도 CPU 에는 인터럽트
 * 한 줄만 올린다. 어느 벡터가 왔는지는 ISTATUS_MSI 레지스터를 읽어야 알 수 있다.
 * 이 함수가 그 "한 줄 -> 여러 벡터" 분해를 담당한다.
 *
 * 동작 단계:
 *  1. chained_irq_enter() -- 부모 irq_chip 에게 이 인터럽트를 잠시 막고 ack 하라고
 *     알린다. 이것을 빼먹으면 레벨 트리거 부모에서 인터럽트 폭풍이 난다.
 *  2. ISTATUS_LOCAL 을 읽어 MSI 비트(bit28)가 서 있는지 본다.
 *  3. 서 있으면 그 비트만 되써서 지운다(write-1-to-clear). 벡터 상태를 읽기 "전에"
 *     묶음 비트를 지우는 순서인데, 이렇게 해야 읽는 도중 새로 도착한 벡터가
 *     다음 인터럽트로 다시 보고되어 유실되지 않는다.
 *  4. ISTATUS_MSI 를 읽어 도착한 벡터 비트맵을 얻고, 설정된 비트마다
 *     generic_handle_domain_irq() 로 MSI 도메인의 해당 hwirq 를 실행한다.
 *     각 벡터의 ISTATUS_MSI 비트 클리어는 irq_ack 콜백(plda_msi_bottom_irq_ack)이
 *     handle_edge_irq 흐름 안에서 대신 해 준다.
 *  5. 매핑되지 않은 벡터면 generic_handle_domain_irq 가 음수를 돌려주므로
 *     ratelimited 로 로그만 남긴다(폭주 방지).
 *  6. chained_irq_exit() -- 부모 인터럽트를 다시 연다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 잠들 수 없고 msi->lock(뮤텍스)도 잡을 수
 * 없다. 그래서 여기서는 used 비트맵을 보지 않고 하드웨어 상태만 본다.
 * 호출자: 커널 IRQ 코어가 msi_irq virq 에 대해 호출한다. 그 virq 는
 * plda_init_interrupts() 가 event_domain 에서 event->msi_event 로 매핑한 것이다.
 * 피호출자: chained_irq_enter/exit, readl_relaxed/writel_relaxed,
 * generic_handle_domain_irq, dev_err_ratelimited.
 * 에러 경로: 벡터 분배 실패는 로그만 남기고 계속 진행한다 -- 인터럽트를 삼키면
 * 레벨 트리거에서 멈춰 버리므로 나머지 벡터라도 처리해야 한다.
 *
 * 호출 체인:
 *   SoC PLIC/GIC -> plda_handle_event() -> event_domain(hwirq=msi_event)
 *     -> [이 함수] -> msi.dev_domain(hwirq=벡터번호) -> 장치 드라이버 핸들러
 */
static void plda_handle_msi(struct irq_desc *desc)
{
	struct plda_pcie_rp *port = irq_desc_get_handler_data(desc);
	/* [한국어] 부모 인터럽트의 irq_chip. chained_irq_enter/exit 에 넘겨 부모 쪽 mask/ack 를
	 * 대신 수행하게 하려면 필요하다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 에러 로그용 device. port->dev 를 지역 변수로 빼 두어 반복 역참조를 줄인다. */
	struct device *dev = port->dev;
	/* [한국어] MSI 상태 묶음. 아래에서 num_vectors 와 dev_domain 을 쓴다. */
	struct plda_msi *msi = &port->msi;
	/* [한국어] 브리지 레지스터 창 기준 주소. 이 함수에서 두 번 쓰이므로 지역 변수로 캐시한다. */
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] ISTATUS 값을 담을 변수. for_each_set_bit 가 unsigned long 비트맵 포인터를
	 * 요구하므로 u32 가 아니라 unsigned long 이어야 한다. */
	unsigned long status;
	/* [한국어] for_each_set_bit 가 돌려주는 비트 번호(= MSI 벡터 번호)를 받을 변수. */
	u32 bit;
	/* [한국어] generic_handle_domain_irq 의 반환값. 매핑되지 않은 벡터를 걸러내는 데 쓴다. */
	int ret;

	/* [한국어] 체인 핸들러 진입 규약. 부모 irq_chip 이 요구하는 mask/ack 를 대신 호출해 준다.
	 * 이것 없이 자식 인터럽트를 처리하면 레벨 트리거 부모에서 인터럽트가 재발화해
	 * 무한 루프에 빠진다. */
	chained_irq_enter(chip, desc);

	/* [한국어] ISTATUS_LOCAL 을 읽어 어떤 이벤트가 대기 중인지 본다. 이 핸들러는 MSI 비트만
	 * 관심 있지만 레지스터는 하나이므로 통째로 읽는다. */
	status = readl_relaxed(bridge_base_addr + ISTATUS_LOCAL);
	/* [한국어] MSI 묶음 비트(bit28)가 서 있을 때만 진입한다. 서 있지 않다면 이 체인 핸들러가
	 * 불릴 이유가 없었던 것이므로 그냥 빠져나간다. */
	if (status & PM_MSI_INT_MSI_MASK) {
		/* [한국어] MSI 묶음 비트만 write-1-to-clear 로 지운다. 아래에서 ISTATUS_MSI 를 읽기 '전에'
		 * 지우는 순서가 중요하다 -- 읽는 도중 새 벡터가 도착하면 묶음 비트가 다시 서서
		 * 다음 인터럽트로 보고되므로 유실이 없다. 반대 순서였다면 그 사이 도착분이
		 * 지워져 사라진다. */
		writel_relaxed(status & PM_MSI_INT_MSI_MASK,
			       bridge_base_addr + ISTATUS_LOCAL);
		/* [한국어] 이제 어떤 벡터가 도착했는지 ISTATUS_MSI(0x194)에서 읽는다. 비트 n = 벡터 n. */
		status = readl_relaxed(bridge_base_addr + ISTATUS_MSI);
		/* [한국어] 설정된 벡터마다 순회한다. 상한이 num_vectors 이므로 하드웨어가 쓰지 않는
		 * 상위 비트는 보지 않는다. */
		for_each_set_bit(bit, &status, msi->num_vectors) {
			/* [한국어] MSI 도메인의 hwirq=bit 에 해당하는 IRQ 를 실행한다. 그 안에서 handle_edge_irq 가
			 * plda_msi_bottom_irq_ack 로 ISTATUS_MSI 비트를 지우고 장치 핸들러를 부른다. */
			ret = generic_handle_domain_irq(msi->dev_domain, bit);
			/* [한국어] 매핑이 없는 벡터면 음수가 돌아온다 -- 장치가 해제된 뒤 늦게 도착한 MSI 등. */
			if (ret)
				/* [한국어] ratelimited 로 남긴다. 매핑 없는 벡터는 계속 반복될 수 있어 로그 폭주를 막아야 한다. */
				dev_err_ratelimited(dev, "bad MSI IRQ %d\n",
						    bit);
		}
	}

	/* [한국어] 체인 핸들러 퇴장 규약. 부모 인터럽트의 마스크를 풀어 다음 인터럽트를 받게 한다. */
	chained_irq_exit(chip, desc);
}

/*
 * [한국어]
 * plda_msi_bottom_irq_ack - 처리한 MSI 벡터의 ISTATUS_MSI 비트를 지운다
 *
 * @data: 처리 중인 MSI 벡터의 irq_data. hwirq 가 벡터 번호(0..num_vectors-1)이고,
 *        chip_data 에 struct plda_pcie_rp 가 들어 있다
 *        (plda_irq_msi_domain_alloc 이 irq_domain_set_info 로 심음).
 * @return: 없음.
 *
 * 왜 필요한가: MSI 는 edge 성격의 인터럽트이고(handle_edge_irq 를 쓴다), 하드웨어는
 * 도착한 벡터마다 ISTATUS_MSI 의 비트를 세워 둔다. 이 비트를 지우지 않으면 같은
 * 벡터가 다시 왔는지 구별할 수 없다. handle_edge_irq 는 실제 핸들러를 부르기
 * "전에" 이 ack 콜백을 부르므로, 핸들러 실행 중 도착한 인터럽트도 놓치지 않는다.
 *
 * 동작: BIT(hwirq) 한 비트만 ISTATUS_MSI(0x194)에 쓴다. 이 레지스터는
 * write-1-to-clear 라 1 을 쓰는 것이 "지우기" 다. 읽고 고쳐 쓰지 않으므로
 * 다른 벡터 비트를 건드리지 않고, 따라서 락도 필요 없다 -- 이것이 IMASK_LOCAL
 * 조작(락 필요)과 결정적으로 다른 점이다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트, handle_edge_irq 내부.
 * 호출자: 커널 IRQ 코어(handle_edge_irq)가 irq_chip.irq_ack 로 간접 호출.
 * 피호출자: writel_relaxed 하나.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_msi() -> generic_handle_domain_irq() -> handle_edge_irq()
 *     -> chip->irq_ack = [이 함수]
 */
static void plda_msi_bottom_irq_ack(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] ISTATUS_MSI 를 쓸 기준 주소. */
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] 이 벡터의 하드웨어 번호. MSI 도메인에서는 hwirq 가 곧 ISTATUS_MSI 의 비트 위치다. */
	u32 bitpos = data->hwirq;

	/* [한국어] 그 한 비트만 쓴다. write-1-to-clear 이므로 이것이 '지우기' 이고,
	 * 읽지 않으므로 다른 벡터 비트에 영향을 주지 않아 락이 필요 없다. */
	writel_relaxed(BIT(bitpos), bridge_base_addr + ISTATUS_MSI);
}

/*
 * [한국어]
 * plda_compose_msi_msg - 장치 config space 에 써 넣을 MSI 메시지(주소+데이터)를 만든다
 *
 * @data: 이 벡터의 irq_data. hwirq 가 곧 벡터 번호이고 chip_data 가 컨트롤러다.
 * @msg: 커널이 채워 달라고 넘겨준 출력 구조체. 나중에 PCI 코어가 이 값을 장치의
 *       MSI capability 레지스터에 그대로 써 넣는다.
 * @return: 없음.
 *
 * 왜 필요한가: MSI 는 "장치가 특정 주소에 특정 값을 쓰는 것" 이 인터럽트다. 따라서
 * 커널은 장치에게 "이 주소에 이 값을 써라" 를 알려 줘야 하고, 그 값이 컨트롤러마다
 * 다르므로 irq_chip 콜백으로 뺀다. PLDA 의 규약은 아주 단순하다 -- 주소는 컨트롤러
 * 공통(msi.vector_phy)이고 데이터가 곧 벡터 번호다. 그래서 브리지는 도착한 쓰기의
 * 데이터 값을 보고 ISTATUS_MSI 의 몇 번 비트를 세울지 정한다.
 *
 * 동작: vector_phy 를 하위/상위 32비트로 쪼개 address_lo/address_hi 에 넣고,
 * hwirq 를 data 에 넣는다. 그리고 dev_dbg 로 남긴다(기본 빌드에서는 컴파일만 되고
 * 출력되지 않는다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. MSI 를 할당하는 pci_alloc_irq_vectors() 경로
 * 안에서 불린다 -- 인터럽트 컨텍스트가 아니다.
 * 호출자: 커널 MSI 코어가 irq_chip.irq_compose_msi_msg 로 간접 호출.
 * 피호출자: lower_32_bits/upper_32_bits, dev_dbg.
 * 에러 경로: 없음 -- 실패할 수 있는 동작이 없다.
 *
 * 참고: affinity(어느 CPU 로 보낼지)를 바꾸는 콜백이 없다. 주소가 하나로 고정이라
 * CPU 별로 다른 주소를 줄 수 없기 때문이며, 그래서 plda_msi_parent_ops 가
 * MSI_FLAG_NO_AFFINITY 를 필수 플래그로 선언한다.
 *
 * 호출 체인:
 *   장치 드라이버 pci_alloc_irq_vectors() -> MSI 코어 -> [이 함수]
 */
static void plda_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] 이 컨트롤러의 MSI 수신 주소. 벡터마다 다르지 않고 컨트롤러 하나에 하나뿐이다 --
	 * 그래서 affinity 를 지원할 수 없고 MSI_FLAG_NO_AFFINITY 를 선언한다. */
	phys_addr_t addr = port->msi.vector_phy;

	/* [한국어] 주소 하위 32비트를 메시지에 넣는다. PCI 코어가 나중에 장치의 MSI capability
	 * Message Address 레지스터에 그대로 쓴다. */
	msg->address_lo = lower_32_bits(addr);
	/* [한국어] 주소 상위 32비트. 장치가 64비트 MSI 를 지원하지 않으면 이 값은 0 이어야 한다. */
	msg->address_hi = upper_32_bits(addr);
	/* [한국어] 메시지 데이터에 벡터 번호를 넣는다. 브리지는 도착한 쓰기의 데이터 값을 보고
	 * ISTATUS_MSI 의 몇 번 비트를 세울지 정하므로, 이 값이 곧 벡터 식별자다. */
	msg->data = data->hwirq;

	/* [한국어] 디버그 로그. dynamic debug 가 꺼진 기본 빌드에서는 출력되지 않는다. */
	dev_dbg(port->dev, "msi#%x address_hi %#x address_lo %#x\n",
		(int)data->hwirq, msg->address_hi, msg->address_lo);
}

/*
 * [한국어] MSI 벡터 계층의 irq_chip -- MSI 하나하나에 붙는 하드웨어 조작 테이블.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_irq_msi_domain_alloc() 이
 * irq_domain_set_info 로 각 벡터에 꽂는다.
 * mask/unmask 콜백이 "없다" 는 점이 INTx/이벤트 chip 과 결정적으로 다르다.
 * PLDA 브리지에는 벡터 단위 마스크 레지스터가 없기 때문이며, 개별 벡터 차단이
 * 필요하면 PCI 표준 MSI capability 의 마스크 비트를 상위 계층이 쓴다.
 * 동기화: 상수 테이블이므로 없다.
 */
static struct irq_chip plda_msi_bottom_irq_chip = {
	.name = "PLDA MSI",
	.irq_ack = plda_msi_bottom_irq_ack,
	.irq_compose_msi_msg = plda_compose_msi_msg,
};

/*
 * [한국어]
 * plda_irq_msi_domain_alloc - MSI 벡터 하나를 비트맵에서 골라 virq 에 연결한다
 *
 * @domain: MSI 부모 도메인. host_data 에 struct plda_pcie_rp 가 들어 있다
 *          (plda_allocate_msi_domains 의 irq_domain_info.host_data).
 * @virq: 커널이 이미 할당해 둔 가상 IRQ 번호. 여기에 하드웨어 벡터를 붙여 준다.
 * @nr_irqs: 요청된 연속 벡터 개수. 이 구현은 이 값을 "무시하고" 항상 한 개만
 *           할당한다 -- 아래 주의 참조.
 * @args: 상위 계층이 넘긴 인자. 이 구현은 쓰지 않는다.
 * @return: 0 성공, -ENOSPC 는 빈 벡터가 없음.
 *
 * 왜 필요한가: 커널은 MSI 벡터를 irq_domain 의 alloc/free 로 관리한다. PLDA 는
 * 벡터 번호가 곧 하드웨어 비트 번호이므로, 어느 번호가 비었는지를 소프트웨어
 * 비트맵(msi->used)으로 직접 장부 관리해야 한다.
 *
 * 동작 단계:
 *  1. msi->lock 뮤텍스를 잡는다. used 비트맵의 "검사 후 설정" 이 원자적이어야
 *     하기 때문이다 -- find_first_zero_bit 와 set_bit 사이에 다른 스레드가 끼면
 *     같은 벡터를 두 장치에 줄 수 있다.
 *  2. find_first_zero_bit 로 빈 벡터를 찾는다. num_vectors 이상이면 자리가 없다.
 *  3. 실패하면 반드시 락을 풀고 -ENOSPC 를 돌려준다(early return 이므로 여기서만
 *     별도의 unlock 이 필요하다).
 *  4. set_bit 으로 점유 표시. 락 안이지만 원자적 판을 쓴다.
 *  5. irq_domain_set_info 로 virq <-> hwirq(=bit) 매핑을 등록하고, irq_chip 을
 *     plda_msi_bottom_irq_chip, flow handler 를 handle_edge_irq 로 지정한다.
 *     MSI 가 edge 인 이유는 "쓰기 한 번 = 인터럽트 한 번" 이라 레벨 개념이 없기 때문.
 *  6. 락을 풀고 0 반환.
 *
 * 주의(상류 구현의 한계): nr_irqs 가 2 이상이어도 벡터 하나만 잡아 주므로, 다중
 * MSI(MSI Multiple Message)를 요구하는 장치에는 맞지 않는다. 이 코드가 그렇게
 * 되어 있다는 사실만 적어 두며, 상위 계층이 어떻게 이를 막는지는 이 파일만으로는
 * 확인할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(뮤텍스를 잡으므로 필수). 장치 드라이버의
 * pci_alloc_irq_vectors() 경로에서 불린다.
 * 호출자: 커널 irq 도메인 코어가 msi_domain_ops.alloc 으로 간접 호출.
 * 피호출자: mutex_lock/unlock, find_first_zero_bit, set_bit, irq_domain_set_info.
 * 에러 경로: -ENOSPC 만 있으며, 그 경로에서도 락을 반드시 푼다.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors() -> MSI 코어 -> msi_domain_ops.alloc = [이 함수]
 */
static int plda_irq_msi_domain_alloc(struct irq_domain *domain,
				     unsigned int virq,
				     unsigned int nr_irqs,
				     void *args)
{
	struct plda_pcie_rp *port = domain->host_data;
	/* [한국어] MSI 장부. 아래에서 used 비트맵과 num_vectors, lock 을 모두 쓴다. */
	struct plda_msi *msi = &port->msi;
	/* [한국어] 찾아낸 빈 벡터 번호. find_first_zero_bit 가 unsigned long 을 돌려주므로 타입을 맞춘다. */
	unsigned long bit;

	mutex_lock(&msi->lock);
	/* [한국어] used 비트맵에서 처음으로 0 인 비트를 찾는다 -- 즉 아직 아무도 쓰지 않는 벡터.
	 * 반드시 락 안에서 해야 한다. 찾은 뒤 set_bit 하기 전에 다른 스레드가 끼어들면
	 * 같은 번호를 두 장치에 줄 수 있기 때문이다. */
	bit = find_first_zero_bit(msi->used, msi->num_vectors);
	/* [한국어] 빈 자리가 없으면 find_first_zero_bit 가 상한값을 그대로 돌려준다. */
	if (bit >= msi->num_vectors) {
		mutex_unlock(&msi->lock);
		return -ENOSPC;
	}

	/* [한국어] 찾은 벡터를 점유 표시한다. 이 시점부터 다른 요청은 이 번호를 못 가져간다. */
	set_bit(bit, msi->used);

	/* [한국어] virq <-> hwirq(=bit) 매핑을 등록하면서 irq_chip 과 flow handler 를 함께 지정한다.
	 * domain->host_data(= port)가 chip_data 로 들어가 ack 콜백이 컨트롤러를 되찾는 근거가
	 * 된다. handle_edge_irq 를 쓰는 이유는 MSI 가 '쓰기 한 번 = 인터럽트 한 번' 인
	 * 에지 성격이기 때문이다. */
	irq_domain_set_info(domain, virq, bit, &plda_msi_bottom_irq_chip,
			    domain->host_data, handle_edge_irq, NULL, NULL);

	mutex_unlock(&msi->lock);

	return 0;
}

/*
 * [한국어]
 * plda_irq_msi_domain_free - MSI 벡터 하나를 비트맵에서 회수한다
 *
 * @domain: MSI 부모 도메인.
 * @virq: 반납할 가상 IRQ 번호.
 * @nr_irqs: 반납할 개수. alloc 과 마찬가지로 이 구현은 무시하고 하나만 지운다.
 * @return: 없음 -- 실패를 보고할 방법이 없다.
 *
 * 왜 필요한가: 장치가 사라지거나 MSI 를 해제하면 벡터 번호를 다시 쓸 수 있게
 * 돌려놓아야 한다. 그렇지 않으면 반복적인 bind/unbind 로 벡터가 고갈된다.
 *
 * 동작 단계:
 *  1. virq 로부터 irq_data 를 얻고, 거기서 chip_data(컨트롤러)를 꺼낸다.
 *     alloc 과 달리 domain->host_data 를 직접 쓰지 않고 irq_data 를 경유하는데,
 *     결과는 같다(irq_domain_set_info 가 host_data 를 chip_data 로 심었기 때문).
 *  2. msi->lock 을 잡는다.
 *  3. test_bit 으로 정말 할당된 벡터인지 확인한다. 이미 비어 있으면 이중 해제이므로
 *     비트맵을 건드리지 않고 dev_err 로 알린다 -- 조용히 지우면 다른 장치가 쓰고
 *     있는 벡터를 빼앗을 수 있어 더 위험하다.
 *  4. 할당돼 있으면 __clear_bit 으로 지운다. 원자적이지 않은 판을 쓰는 것은
 *     이미 뮤텍스 안이기 때문이다.
 *  5. 락 해제.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(뮤텍스).
 * 호출자: 커널 irq 도메인 코어가 msi_domain_ops.free 로 간접 호출.
 * 피호출자: irq_domain_get_irq_data, irq_data_get_irq_chip_data, mutex_lock/unlock,
 * test_bit, __clear_bit, dev_err.
 * 에러 경로: 이중 해제는 로그만 남긴다.
 *
 * 호출 체인:
 *   pci_free_irq_vectors() -> MSI 코어 -> msi_domain_ops.free = [이 함수]
 */
static void plda_irq_msi_domain_free(struct irq_domain *domain,
				     unsigned int virq,
				     unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	/* [한국어] irq_data 에서 chip_data 를 꺼내 컨트롤러를 되찾는다. alloc 이 심어 둔 값이다. */
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(d);
	/* [한국어] MSI 장부. 아래에서 lock 과 used 를 쓴다. */
	struct plda_msi *msi = &port->msi;

	mutex_lock(&msi->lock);

	/* [한국어] 정말 할당된 벡터인지 먼저 확인한다. 이중 해제를 조용히 통과시키면 남이 쓰는
	 * 벡터를 빼앗게 되어 더 위험하다. */
	if (test_bit(d->hwirq, msi->used))
		/* [한국어] 비트를 지워 벡터를 반납한다. 이미 뮤텍스 안이므로 원자적이지 않은 판을 쓴다. */
		__clear_bit(d->hwirq, msi->used);
	else
		/* [한국어] 이중 해제는 명백한 버그이므로 ratelimit 없이 그대로 남긴다. */
		dev_err(port->dev, "trying to free unused MSI%lu\n", d->hwirq);

	mutex_unlock(&msi->lock);
}

/*
 * [한국어] MSI 부모 도메인의 도메인 연산 -- 벡터 할당과 해제 두 개뿐이다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_allocate_msi_domains() 이
 * irq_domain_info.ops 로 넘긴다.
 * .map 이 없는 이유: MSI 는 hwirq 를 미리 알 수 없고 요청 시점에 비어 있는 번호를
 * 골라 줘야 하므로, 선형 매핑(.map)이 아니라 계층형 할당(.alloc/.free)을 쓴다.
 * 동기화: 상수 테이블. 내부에서 msi->lock 을 잡는 것은 콜백 쪽 책임이다.
 */
static const struct irq_domain_ops msi_domain_ops = {
	.alloc	= plda_irq_msi_domain_alloc,
	.free	= plda_irq_msi_domain_free,
};

/*
 * [한국어] 이 MSI 부모 도메인 아래에 만들어질 "장치별 자식 도메인" 이 반드시
 * 가져야 할 플래그 조합.
 *  - MSI_FLAG_USE_DEF_DOM_OPS  : 자식 도메인의 도메인 연산을 MSI 코어 기본값으로
 *    채운다(직접 작성하지 않겠다는 선언).
 *  - MSI_FLAG_USE_DEF_CHIP_OPS : 자식 irq_chip 의 빈 콜백을 코어 기본값으로 채운다.
 *  - MSI_FLAG_NO_AFFINITY      : 이 컨트롤러는 인터럽트 목적지 CPU 를 바꿀 수 없다.
 *    plda_compose_msi_msg() 가 항상 같은 주소 하나만 넣는 구조라서 affinity 를
 *    지원할 방법이 없기 때문이다.
 * 읽는 자: plda_msi_parent_ops.required_flags -> msi_lib_init_dev_msi_info().
 */
#define PLDA_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
				 MSI_FLAG_USE_DEF_CHIP_OPS	| \
				 MSI_FLAG_NO_AFFINITY)
/*
 * [한국어] 이 부모 도메인이 자식에게 허용하는 플래그의 상한.
 *  - MSI_GENERIC_FLAGS_MASK : 아키텍처 독립적인 일반 MSI 플래그 전부(비트 15:0).
 *  - MSI_FLAG_PCI_MSIX      : MSI-X 도 허용한다.
 * 여기에 없는 플래그를 자식이 요구하면 도메인 생성이 거부된다.
 * 읽는 자: plda_msi_parent_ops.supported_flags -> msi_lib_init_dev_msi_info().
 */
#define PLDA_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
				  MSI_FLAG_PCI_MSIX)

/*
 * [한국어] MSI 부모 도메인의 성격을 커널 MSI 코어에 선언하는 테이블.
 *
 * 각 필드:
 *  - required_flags / supported_flags : 위 두 매크로. 자식 도메인 생성 시 검사된다.
 *  - chip_flags = MSI_CHIP_FLAG_SET_ACK : "자식 irq_chip 의 irq_ack 를 기본값으로
 *    채워 달라" 는 요청. 이 드라이버가 handle_edge_irq 를 쓰므로 ack 가 반드시
 *    있어야 한다.
 *  - bus_select_token = DOMAIN_BUS_PCI_MSI : 이 도메인이 PCI MSI 용임을 표시해
 *    irq_domain 의 select 단계에서 올바르게 골라지게 한다.
 *  - prefix = "PLDA-" : /proc/interrupts 와 도메인 이름 앞에 붙는 접두사.
 *  - init_dev_msi_info = msi_lib_init_dev_msi_info : 자식 도메인 초기화를 커널
 *    공용 구현에 위임한다. 이 한 줄 덕분에 드라이버가 MSI 도메인 정보 구성 코드를
 *    직접 갖지 않아도 된다(include 한 irq-msi-lib.h 가 이것 때문에 필요하다).
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: msi_create_parent_irq_domain().
 * 동기화: 상수.
 */
static const struct msi_parent_ops plda_msi_parent_ops = {
	.required_flags		= PLDA_MSI_FLAGS_REQUIRED,
	.supported_flags	= PLDA_MSI_FLAGS_SUPPORTED,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,
	.prefix			= "PLDA-",
	.init_dev_msi_info	= msi_lib_init_dev_msi_info,
};

/*
 * [한국어]
 * plda_allocate_msi_domains - 이 컨트롤러의 MSI 부모 irq_domain 을 만든다
 *
 * @port: MSI 벡터 수(msi.num_vectors)와 dev 가 이미 채워진 컨트롤러.
 * @return: 0 성공, -ENOMEM 은 도메인 생성 실패.
 *
 * 왜 필요한가: 커널 MSI 계층은 2단 구조다 -- 컨트롤러가 "부모(parent) 도메인" 을
 * 하나 만들어 두면, 장치마다 필요한 "자식(per-device) 도메인" 은 MSI 코어가
 * 그때그때 만들어 붙인다. 이 함수가 그 부모 도메인을 만든다.
 *
 * 동작 단계:
 *  1. msi.lock 뮤텍스를 초기화한다. 도메인이 생기는 순간부터 alloc/free 콜백이
 *     들어올 수 있으므로 그 전에 초기화해야 한다.
 *  2. irq_domain_info 를 스택에 구성한다. fwnode 는 컨트롤러 device 의 것,
 *     ops 는 alloc/free 두 개만 가진 msi_domain_ops, host_data 는 port,
 *     size 는 벡터 개수다. (선언과 초기화가 문장 중간에 있는 C99 스타일이라
 *     mutex_init 뒤에 나온다.)
 *  3. msi_create_parent_irq_domain() 으로 도메인을 만들고 plda_msi_parent_ops 를
 *     붙인다. 이 한 번의 호출로 도메인 이름 접두사, 지원 플래그, 자식 도메인
 *     초기화 콜백까지 모두 등록된다.
 *  4. 실패하면 dev_err 후 -ENOMEM.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 메모리 할당을 하므로 잠들 수 있다.
 * 호출자: plda_pcie_init_irq_domains() 의 마지막 줄(return 문)에서 호출된다.
 * 피호출자: mutex_init, dev_fwnode, msi_create_parent_irq_domain, dev_err.
 * 에러 경로: 실패 시 -ENOMEM 이 plda_pcie_init_irq_domains -> plda_init_interrupts
 * -> plda_pcie_host_init 까지 그대로 전파되어 probe 가 중단된다. 다만 그 시점에
 * 이미 만들어진 event_domain / intx_domain 은 여기서 정리되지 않는다는 점에 주의
 * (실패한 probe 의 도메인 누수 가능성 -- 코드가 그렇게 되어 있다는 사실만 기술한다).
 *
 * 호출 체인:
 *   plda_init_interrupts() -> plda_pcie_init_irq_domains() -> [이 함수]
 */
static int plda_allocate_msi_domains(struct plda_pcie_rp *port)
{
	struct device *dev = port->dev;
	/* [한국어] 이 컨트롤러의 MSI 장부. 아래 info.size 에 벡터 개수를 넣는 데 쓴다. */
	struct plda_msi *msi = &port->msi;

	mutex_init(&port->msi.lock);

	/* [한국어] 도메인 생성 파라미터. 선언이 mutex_init 뒤에 오는 C99 스타일이다 --
	 * 락 초기화가 도메인 생성보다 먼저여야 하기 때문에 순서가 이렇게 되어 있다. */
	struct irq_domain_info info = {
		/* [한국어] 도메인을 이 device 의 fwnode 에 묶는다. INTx/이벤트 도메인이 devicetree 자식
		 * 노드에 묶이는 것과 달리, MSI 부모 도메인은 컨트롤러 device 자체에 묶인다. */
		.fwnode		= dev_fwnode(dev),
		.ops		= &msi_domain_ops,
		.host_data	= port,
		.size		= msi->num_vectors,
	};

	/* [한국어] MSI 부모 도메인을 만든다. 이 한 번의 호출로 도메인 등록, 이름 접두사,
	 * 지원 플래그, 자식 도메인 초기화 콜백까지 모두 설정된다. */
	msi->dev_domain = msi_create_parent_irq_domain(&info, &plda_msi_parent_ops);
	/* [한국어] 생성 실패는 사실상 메모리 부족이다. */
	if (!msi->dev_domain) {
		/* [한국어] 실패 원인을 남긴다. probe 실패 메시지로 이어진다. */
		dev_err(dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}

	return 0;
}

/*
 * [한국어]
 * plda_handle_intx - INTx 묶음 인터럽트를 INTA~INTD 각각으로 분배하는 체인 핸들러
 *
 * @desc: 'INTx 이벤트' virq 의 irq_desc. handler_data 가 struct plda_pcie_rp.
 * @return: 없음.
 *
 * 왜 필요한가: PLDA 는 INTA/INTB/INTC/INTD 네 개의 legacy 인터럽트를 ISTATUS_LOCAL
 * 의 비트 27:24 에 모아 두고 CPU 에는 한 줄로 올린다. 커널은 이 넷을 별개의 IRQ 로
 * 다루므로 여기서 풀어 준다.
 *
 * 동작 단계:
 *  1. chained_irq_enter() 로 부모 인터럽트를 정리한다.
 *  2. ISTATUS_LOCAL 을 읽는다.
 *  3. INTx 4비트 중 하나라도 서 있으면 PM_MSI_INT_INTX_MASK 로 잘라 내고
 *     PM_MSI_INT_INTX_SHIFT(24)만큼 오른쪽으로 밀어 0..3 범위로 정규화한다.
 *     이 정규화 덕분에 다음 줄의 for_each_set_bit 비트 번호가 곧 INTx 도메인의
 *     hwirq(0=INTA .. 3=INTD)가 된다.
 *  4. 설정된 비트마다 generic_handle_domain_irq(intx_domain, bit) 로 넘긴다.
 *  5. 실패하면 ratelimited 로그.
 *  6. chained_irq_exit().
 *
 * MSI 핸들러와의 결정적 차이: 여기서는 ISTATUS_LOCAL 비트를 지우지 "않는다".
 * INTx 는 레벨 트리거이고(handle_level_irq 를 쓴다), 상태 비트 클리어는
 * plda_ack_intx_irq() 가 flow handler 안에서 처리한다. 레벨 인터럽트는 장치가
 * 요인을 없앨 때까지 계속 서 있으므로 여기서 미리 지워 봐야 소용이 없다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 잠들 수 없다. 락도 잡지 않는다 --
 * 여기서는 상태 레지스터를 읽기만 하기 때문이다.
 * 호출자: 커널 IRQ 코어가 port->intx_irq 에 대해 호출.
 * 피호출자: chained_irq_enter/exit, readl_relaxed, generic_handle_domain_irq.
 * 에러 경로: 매핑 없는 INTx 는 로그만 남기고 계속 진행.
 *
 * 호출 체인:
 *   SoC PLIC/GIC -> plda_handle_event() -> event_domain(hwirq=intx_event)
 *     -> [이 함수] -> intx_domain(hwirq=0..3) -> 장치 드라이버 핸들러
 */
static void plda_handle_intx(struct irq_desc *desc)
{
	struct plda_pcie_rp *port = irq_desc_get_handler_data(desc);
	/* [한국어] 부모 irq_chip. chained_irq_enter/exit 에 필요하다. */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	/* [한국어] 에러 로그용 device. */
	struct device *dev = port->dev;
	/* [한국어] 브리지 레지스터 기준 주소. */
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] ISTATUS 값. for_each_set_bit 때문에 unsigned long 이어야 한다. */
	unsigned long status;
	/* [한국어] 설정된 비트 번호(= 정규화된 INTx 번호 0..3)를 받을 변수. */
	u32 bit;
	/* [한국어] generic_handle_domain_irq 의 반환값. */
	int ret;

	/* [한국어] 체인 핸들러 진입. 부모 인터럽트를 정리한다. */
	chained_irq_enter(chip, desc);

	/* [한국어] ISTATUS_LOCAL 을 읽는다. 이 핸들러는 INTx 4비트만 본다. */
	status = readl_relaxed(bridge_base_addr + ISTATUS_LOCAL);
	/* [한국어] INTA~INTD 중 하나라도 서 있을 때만 진입한다. */
	if (status & PM_MSI_INT_INTX_MASK) {
		/* [한국어] INTx 4비트만 남긴다. */
		status &= PM_MSI_INT_INTX_MASK;
		/* [한국어] 24비트 오른쪽으로 밀어 0..3 범위로 정규화한다. 이 정규화 덕분에 아래
		 * for_each_set_bit 의 비트 번호가 곧 intx_domain 의 hwirq(0=INTA .. 3=INTD)가 된다. */
		status >>= PM_MSI_INT_INTX_SHIFT;
		/* [한국어] 상한이 PCI_NUM_INTX(4)이므로 정확히 네 비트만 본다. */
		for_each_set_bit(bit, &status, PCI_NUM_INTX) {
			/* [한국어] INTx 도메인의 해당 IRQ 를 실행한다. 그 안의 handle_level_irq 가
			 * plda_ack_intx_irq 로 ISTATUS_LOCAL 비트를 지우고 장치 핸들러를 부른다.
			 * MSI 핸들러와 달리 여기서 상태 비트를 미리 지우지 않는 이유가 그것이다. */
			ret = generic_handle_domain_irq(port->intx_domain, bit);
			/* [한국어] 매핑 없는 INTx -- devicetree 의 interrupt-map 이 그 핀을 연결하지 않은 경우. */
			if (ret)
				/* [한국어] 폭주 방지를 위해 ratelimited 로 남긴다. */
				dev_err_ratelimited(dev, "bad INTx IRQ %d\n",
						    bit);
		}
	}

	/* [한국어] 체인 핸들러 퇴장. */
	chained_irq_exit(chip, desc);
}

/*
 * [한국어]
 * plda_ack_intx_irq - 처리한 INTx 한 줄의 ISTATUS_LOCAL 비트를 지운다
 *
 * @data: INTx irq_data. hwirq 는 0(INTA)~3(INTD), chip_data 는 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 컨트롤러가 래치해 둔 INTx 상태 비트를 지워야 다음 어서션을
 * 감지할 수 있다. 레벨 트리거라도 브리지 내부의 상태 래치는 명시적으로 지워야 한다.
 *
 * 동작: hwirq 에 PM_MSI_INT_INTX_SHIFT(24)를 더해 레지스터 비트 위치를 복원하고,
 * 그 한 비트만 ISTATUS_LOCAL 에 쓴다(write-1-to-clear). plda_handle_intx() 가
 * 오른쪽으로 밀어 정규화한 것을 여기서 다시 왼쪽으로 되돌리는 셈이다.
 * 읽고 고쳐 쓰는 것이 아니므로 락이 필요 없다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(handle_level_irq 내부).
 * 호출자: 커널 IRQ 코어가 plda_intx_irq_chip.irq_ack 로 간접 호출.
 * 피호출자: writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_intx() -> generic_handle_domain_irq() -> handle_level_irq()
 *     -> chip->irq_ack = [이 함수]
 */
static void plda_ack_intx_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] ISTATUS_LOCAL 을 쓸 기준 주소. */
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] hwirq(0..3)에 24 를 더해 레지스터 비트 위치로 되돌린다.
	 * plda_handle_intx 가 오른쪽으로 밀어 정규화한 것의 역연산이다. */
	u32 mask = BIT(data->hwirq + PM_MSI_INT_INTX_SHIFT);

	/* [한국어] 그 한 비트만 써서 상태를 지운다(write-1-to-clear). 읽고 고쳐 쓰는 것이 아니라
	 * 락이 필요 없다 -- 아래 mask/unmask 가 락을 잡는 것과 대비된다. */
	writel_relaxed(mask, bridge_base_addr + ISTATUS_LOCAL);
}

/*
 * [한국어]
 * plda_mask_intx_irq - INTx 한 줄을 IMASK_LOCAL 에서 꺼 인터럽트를 막는다
 *
 * @data: INTx irq_data(hwirq 0..3), chip_data 는 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 커널이 disable_irq() 하거나 handle_level_irq 가 처리 중 재진입을
 * 막을 때 이 콜백으로 하드웨어 마스크를 건다. 마스크가 없으면 레벨 인터럽트가
 * 계속 재발화해 CPU 를 잡아먹는다.
 *
 * 동작 단계:
 *  1. hwirq + 24 로 레지스터 비트 위치를 구한다.
 *  2. raw_spin_lock_irqsave 로 port->lock 을 잡는다. 여기서 락이 반드시 필요한
 *     이유는 IMASK_LOCAL 이 read-modify-write 대상이고, 같은 레지스터를
 *     이벤트 쪽 mask/unmask 도 건드리기 때문이다. 락이 없으면 한쪽의 갱신이
 *     다른 쪽에 의해 통째로 덮어써진다(lost update).
 *     irqsave 판을 쓰는 것은 이 콜백이 인터럽트가 열린 프로세스 컨텍스트
 *     (disable_irq 등)에서도 불릴 수 있어, 그 사이 인터럽트가 들어와 같은 락을
 *     잡으려 하면 자기 자신과 데드락에 빠지기 때문이다.
 *  3. IMASK_LOCAL 을 읽어 해당 비트만 AND-NOT 으로 지우고 되쓴다.
 *  4. 락 해제(인터럽트 상태 복원).
 *
 * 실행 컨텍스트: 프로세스 또는 인터럽트 컨텍스트 양쪽. raw 스핀락이므로
 * PREEMPT_RT 에서도 잠들지 않는다.
 * 호출자: 커널 IRQ 코어(handle_level_irq, disable_irq 등)가 irq_mask 로 간접 호출.
 * 피호출자: raw_spin_lock_irqsave/unlock_irqrestore, readl_relaxed, writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   disable_irq() 또는 handle_level_irq() -> chip->irq_mask = [이 함수]
 */
static void plda_mask_intx_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] IMASK_LOCAL 을 읽고 쓸 기준 주소. */
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] raw_spin_lock_irqsave 가 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 끌 비트의 위치. hwirq + 24. */
	u32 mask = BIT(data->hwirq + PM_MSI_INT_INTX_SHIFT);
	/* [한국어] read-modify-write 를 위한 임시 변수. */
	u32 val;

	/* [한국어] IMASK_LOCAL 은 읽고-고치고-쓰는 대상이고 이벤트 쪽 mask/unmask 도 같은
	 * 레지스터를 건드리므로 반드시 배타적이어야 한다. irqsave 를 쓰는 이유는
	 * 이 콜백이 인터럽트가 열린 프로세스 컨텍스트(disable_irq 등)에서도 불릴 수 있어,
	 * 그 사이 인터럽트가 들어와 같은 락을 다시 잡으려 하면 자기 자신과 데드락에
	 * 빠지기 때문이다. */
	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] 현재 마스크 값을 읽는다. */
	val = readl_relaxed(bridge_base_addr + IMASK_LOCAL);
	/* [한국어] 해당 비트를 지운다 -- IMASK_LOCAL 은 1 이 '허용' 이므로 0 이 마스크다. */
	val &= ~mask;
	/* [한국어] 되쓴다. 이 순간부터 그 INTx 는 CPU 로 올라오지 않는다. */
	writel_relaxed(val, bridge_base_addr + IMASK_LOCAL);
	/* [한국어] 락 해제와 인터럽트 상태 복원. */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

/*
 * [한국어]
 * plda_unmask_intx_irq - INTx 한 줄을 IMASK_LOCAL 에서 켜 인터럽트를 허용한다
 *
 * @data: INTx irq_data(hwirq 0..3), chip_data 는 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: request_irq() 시점과 handle_level_irq 가 처리를 끝낸 시점에 다시
 * 인터럽트를 열어 주어야 한다. plda_mask_intx_irq 의 정확한 짝이다.
 *
 * 동작: mask 판과 모든 것이 같고 비트 연산만 AND-NOT 대신 OR 다. 락 규칙(왜
 * irqsave 인지, 왜 raw 인지)도 동일하다 -- 위 plda_mask_intx_irq 주석 참조.
 *
 * 실행 컨텍스트: 프로세스 또는 인터럽트 컨텍스트.
 * 호출자: 커널 IRQ 코어가 irq_unmask 로 간접 호출.
 * 피호출자: raw_spin_lock_irqsave/unlock_irqrestore, readl_relaxed, writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   request_irq()/enable_irq() 또는 handle_level_irq() 종료 -> chip->irq_unmask
 *     = [이 함수]
 */
static void plda_unmask_intx_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] IMASK_LOCAL 기준 주소. */
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 켤 비트의 위치. hwirq + 24. */
	u32 mask = BIT(data->hwirq + PM_MSI_INT_INTX_SHIFT);
	/* [한국어] read-modify-write 임시 변수. */
	u32 val;

	/* [한국어] mask 판과 같은 이유로 irqsave 판 raw 스핀락을 쓴다. */
	raw_spin_lock_irqsave(&port->lock, flags);
	/* [한국어] 현재 마스크 값을 읽는다. */
	val = readl_relaxed(bridge_base_addr + IMASK_LOCAL);
	/* [한국어] 해당 비트를 세운다 -- 1 이 '허용' 이다. */
	val |= mask;
	/* [한국어] 되쓴다. 이 순간부터 그 INTx 가 CPU 로 올라온다. */
	writel_relaxed(val, bridge_base_addr + IMASK_LOCAL);
	/* [한국어] 락 해제와 인터럽트 상태 복원. */
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

/*
 * [한국어] INTx(INTA~INTD) 계층의 irq_chip.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_pcie_intx_map() 이 각 virq 에 붙인다.
 * MSI chip 과 달리 mask/unmask 가 있다 -- IMASK_LOCAL 의 비트 27:24 로 INTx 를
 * 개별 차단할 수 있기 때문이다. 그래서 handle_level_irq 가 정상 동작한다.
 * 동기화: 상수 테이블이지만 콜백 내부에서 port->lock 을 잡는다.
 */
static struct irq_chip plda_intx_irq_chip = {
	.name = "PLDA PCIe INTx",
	.irq_ack = plda_ack_intx_irq,
	.irq_mask = plda_mask_intx_irq,
	.irq_unmask = plda_unmask_intx_irq,
};

/*
 * [한국어]
 * plda_pcie_intx_map - INTx 도메인의 virq 하나에 irq_chip 과 flow handler 를 붙인다
 *
 * @domain: intx_domain. host_data 가 struct plda_pcie_rp.
 * @irq: 커널이 새로 만든 가상 IRQ 번호.
 * @hwirq: 이 도메인에서의 하드웨어 번호(0=INTA .. 3=INTD). 이 함수는 쓰지 않는다.
 * @return: 항상 0(실패 경로 없음).
 *
 * 왜 필요한가: irq_domain 은 "hwirq 를 virq 로 매핑할 때 무엇을 해야 하는가" 를
 * .map 콜백으로 묻는다. 여기서 chip 과 handler 를 붙이지 않으면 그 IRQ 는
 * 발생해도 아무 일도 하지 않는다.
 *
 * 동작:
 *  1. irq_set_chip_and_handler 로 plda_intx_irq_chip(ack/mask/unmask)과
 *     handle_level_irq 를 지정한다. legacy INTx 는 규격상 레벨 트리거이므로
 *     handle_level_irq 여야 한다(MSI 쪽이 handle_edge_irq 인 것과 대비된다).
 *  2. irq_set_chip_data 로 domain->host_data(= port)를 chip_data 에 심는다.
 *     그래야 ack/mask/unmask 콜백이 irq_data_get_irq_chip_data 로 컨트롤러를
 *     되찾을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. irq_create_mapping()/of_irq 해석 경로에서
 * 장치가 INTx 를 요구할 때 불린다.
 * 호출자: 커널 irq 도메인 코어가 intx_domain_ops.map 으로 간접 호출.
 * 피호출자: irq_set_chip_and_handler, irq_set_chip_data.
 * 에러 경로: 없음(항상 0).
 *
 * 호출 체인:
 *   pci_assign_irq()/of_irq_parse_pci -> irq_create_fwspec_mapping()
 *     -> intx_domain_ops.map = [이 함수]
 */
static int plda_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &plda_intx_irq_chip, handle_level_irq);
	/* [한국어] domain->host_data(= struct plda_pcie_rp)를 chip_data 로 심는다.
	 * 그래야 ack/mask/unmask 콜백이 irq_data_get_irq_chip_data 로 컨트롤러를 찾는다. */
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

/*
 * [한국어] INTx 도메인의 도메인 연산 -- .map 하나뿐이다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: irq_domain_create_linear() 에 넘겨진다.
 * MSI 와 달리 hwirq 가 0..3 으로 고정이라 할당 개념이 필요 없고, 매핑 시점에
 * chip 과 handler 만 붙이면 되므로 .map 만 있으면 충분하다.
 * 동기화: 상수.
 */
static const struct irq_domain_ops intx_domain_ops = {
	.map = plda_pcie_intx_map,
};

/*
 * [한국어]
 * plda_get_events - ISTATUS_LOCAL 을 읽어 이벤트 hwirq 비트맵으로 변환한다(코어 기본 구현)
 *
 * @port: bridge_addr 이 매핑된 컨트롤러.
 * @return: 이벤트 번호를 비트 위치로 하는 32비트 비트맵. 유효 폭은 num_events.
 *
 * 왜 필요한가: 레지스터 비트 배치와 커널 이벤트 hwirq 번호가 INTx 때문에 어긋난다
 * (레지스터에서 INTA~INTD 는 4비트, 이벤트 번호로는 1개). 이 함수가 그 접기
 * 변환의 정방향이고, plda_hwirq_to_mask() 가 역방향이다.
 *
 * 동작 단계(변환 규칙 세 조각):
 *  1. origin = ISTATUS_LOCAL 원본 값.
 *  2. 상위 4비트(31:28 = SYS_ERR/EVENTS/AER/MSI)를 SYS_AND_MSI_MASK 로 잘라
 *     28만큼 내린 뒤, (28 - 4 + 1) = 25 만큼 올려 이벤트 비트 25..28 에 놓는다.
 *     3칸 앞당겨지는 이유가 바로 INTx 4비트가 1비트로 접혔기 때문이다.
 *  3. INTx 4비트 중 하나라도 서 있으면 이벤트 비트 24 하나만 세운다.
 *  4. 비트 23:0(DMA + ATR 이벤트)은 번호가 그대로이므로 마스크해서 OR 한다.
 *     경계값 P_ATR_EVT_DOORBELL_SHIFT(23)가 "여기까지는 1:1" 이라는 뜻이다.
 *
 * 반환값의 비트 번호는 아직 SoC 오프셋(PLDA_NUM_DMA_EVENTS 등)이 더해지지 않은
 * 것처럼 보이지만, 실제로는 레지스터 비트 위치가 그대로 hwirq 이므로 이미 오프셋이
 * 포함된 값이다 -- 예컨대 비트 16 은 EVENT 번호 16 = PLDA_AXI_POST_ERR + 16 이다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(plda_handle_event 안). 락 없음(읽기 전용).
 * 호출자: plda_handle_event() 가 port->event_ops->get_events 로 간접 호출.
 * 코어 기본값이라 SoC 가 event_ops 를 지정하지 않은 경우에만 쓰인다 -- 즉 이
 * 트리에서는 StarFive 경로에서만 실행되고, Microchip 은 mc_get_events() 를 쓴다.
 * 피호출자: readl_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> event_ops->get_events = [이 함수]
 */
static u32 plda_get_events(struct plda_pcie_rp *port)
{
	u32 events, val, origin;

	/* [한국어] ISTATUS_LOCAL 원본 값. 아래 세 조각의 변환이 모두 이 값에서 나온다. */
	origin = readl_relaxed(port->bridge_addr + ISTATUS_LOCAL);

	/* MSI event and sys events */
	val = (origin & SYS_AND_MSI_MASK) >> PM_MSI_INT_MSI_SHIFT;
	/* [한국어] (28 - 4 + 1) = 25 만큼 왼쪽으로 밀어 이벤트 비트 25..28 에 놓는다.
	 * 3칸 앞당겨지는 이유는 레지스터의 INTx 4비트가 이벤트 번호에서는 1비트로
	 * 접히기 때문이다. PCI_NUM_INTX 가 그 4 이고, 1 을 더하는 것은 접힌 뒤에도
	 * INTx 자리 한 칸이 남기 때문이다. */
	events = val << (PM_MSI_INT_MSI_SHIFT - PCI_NUM_INTX + 1);

	/* INTx events */
	if (origin & PM_MSI_INT_INTX_MASK)
		/* [한국어] INTA~INTD 중 하나라도 서 있으면 이벤트 비트 24 하나만 세운다.
		 * 어느 핀인지는 여기서 구별하지 않는다 -- 그 구별은 plda_handle_intx 가 한다. */
		events |= BIT(PM_MSI_INT_INTX_SHIFT);

	/* remains are same with register */
	events |= origin & GENMASK(P_ATR_EVT_DOORBELL_SHIFT, 0);

	/* [한국어] 완성된 이벤트 비트맵을 돌려준다. 호출자는 events_bitmap 과 AND 해서 쓴다. */
	return events;
}

/*
 * [한국어]
 * plda_event_handler - 아무 일도 하지 않고 인터럽트를 소진시키는 기본 이벤트 핸들러
 *
 * @irq: 발생한 virq 번호. 쓰지 않는다.
 * @dev_id: devm_request_irq 에 넘긴 port 포인터. 쓰지 않는다.
 * @return: 항상 IRQ_HANDLED.
 *
 * 왜 필요한가: plda_init_interrupts() 는 events_bitmap 에 켜진 모든 이벤트 hwirq 에
 * 대해 IRQ 를 "요청" 해야 한다. 요청하지 않은 IRQ 가 발생하면 커널이 spurious 로
 * 판단해 그 라인을 꺼 버리기 때문이다. 그런데 PLDA 코어 자체는 대부분의 이벤트에
 * 대해 딱히 할 일이 없다(오류 카운팅이나 복구 정책이 없다). 그래서 "받았다" 고만
 * 답하는 빈 핸들러를 둔다. Microchip 은 이 자리에 mc_event_handler() 를 넣어
 * 이벤트 이름을 로그로 남긴다.
 *
 * 동작: IRQ_HANDLED 반환뿐. IRQ_NONE 을 돌려주면 커널이 반복적으로 처리되지 않는
 * 인터럽트로 보고 결국 라인을 비활성화하므로, 반드시 HANDLED 여야 한다.
 * 실제 상태 비트 클리어는 이 함수가 아니라 irq_chip 의 irq_ack
 * (plda_ack_event_irq)이 flow handler 안에서 수행한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(handle_level_irq 가 부르는 action 핸들러).
 * 호출자: 커널 IRQ 코어. plda_init_interrupts() 가 event->request_event_irq 가
 * NULL 일 때 devm_request_irq 로 등록한다 -- 즉 StarFive 경로 전용이다.
 * 피호출자: 없음.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> generic_handle_domain_irq() -> handle_level_irq()
 *     -> [이 함수]
 */
static irqreturn_t plda_event_handler(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

/*
 * [한국어]
 * plda_handle_event - 컨트롤러의 최상위 인터럽트를 받아 이벤트별로 분배하는 체인 핸들러
 *
 * @desc: 컨트롤러 부모 IRQ(port->irq)의 irq_desc. handler_data 가 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: SoC 인터럽트 컨트롤러(PLIC/GIC)에서 보면 이 PCIe 컨트롤러는
 * 인터럽트 한 줄이다. 그 한 줄 안에 주소 변환 오류, DMA 오류, AER, INTx, MSI 가
 * 전부 들어 있어 여기서 첫 갈래를 친다. 이 함수는 인터럽트 사슬의 뿌리다.
 *
 * 동작 단계:
 *  1. chained_irq_enter() 로 부모 컨트롤러 쪽을 정리한다.
 *  2. port->event_ops->get_events(port) 로 현재 대기 중인 이벤트 비트맵을 얻는다.
 *     이 간접 호출 하나가 PLDA 기본판과 Microchip 확장판을 갈라 준다.
 *  3. port->events_bitmap 과 AND 한다 -- SoC 가 쓰지 않기로 한 이벤트(예: StarFive 가
 *     제외한 두 도어벨)는 여기서 걸러진다. 하드웨어가 보고해도 무시하는 것이다.
 *  4. 남은 비트마다 generic_handle_domain_irq(event_domain, bit) 를 호출한다.
 *     이 안에서 handle_level_irq 가 돌며 chip->irq_ack 로 상태를 지우고,
 *     등록된 action 핸들러(plda_event_handler 또는 mc_event_handler)를 부른다.
 *     INTx/MSI 이벤트 비트의 경우 그 virq 에는 action 대신 체인 핸들러가 걸려 있어
 *     plda_handle_intx()/plda_handle_msi() 로 한 단계 더 내려간다.
 *  5. 반환값을 확인하지 않는다 -- INTx/MSI 와 달리 여기서는 매핑되지 않은 이벤트가
 *     events_bitmap 에 의해 이미 걸러졌다고 보기 때문이다.
 *  6. chained_irq_exit().
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 락 없음(읽기와 도메인 분배뿐).
 * events 를 unsigned long 으로 받는 이유는 for_each_set_bit 가 비트맵 포인터를
 * 요구하기 때문이다.
 * 호출자: 커널 IRQ 코어가 port->irq 에 대해 호출. 그 연결은 plda_init_interrupts()
 * 의 마지막 irq_set_chained_handler_and_data(port->irq, plda_handle_event, port).
 * 피호출자: chained_irq_enter/exit, event_ops->get_events, generic_handle_domain_irq.
 * 에러 경로: 없음(반환값 무시).
 *
 * 호출 체인:
 *   SoC PLIC/GIC -> [이 함수] -> event_domain -> (INTx/MSI 면) plda_handle_intx()
 *     또는 plda_handle_msi() -> 최종 장치 핸들러
 */
static void plda_handle_event(struct irq_desc *desc)
{
	struct plda_pcie_rp *port = irq_desc_get_handler_data(desc);
	/* [한국어] 이벤트 비트맵. for_each_set_bit 때문에 unsigned long 이어야 한다. */
	unsigned long events;
	/* [한국어] 설정된 이벤트 번호를 받을 변수. */
	u32 bit;
	/* [한국어] 부모 irq_chip. chained_irq_enter/exit 용. */
	struct irq_chip *chip = irq_desc_get_chip(desc);

	/* [한국어] 체인 핸들러 진입. 이 함수가 인터럽트 사슬의 뿌리이므로 여기서의 부모는
	 * SoC 인터럽트 컨트롤러(PLIC/GIC)다. */
	chained_irq_enter(chip, desc);

	/* [한국어] SoC 별 이벤트 수집 훅을 간접 호출한다. 이 한 줄이 PLDA 기본판
	 * (plda_get_events)과 Microchip 확장판(mc_get_events)을 갈라 주는 지점이다. */
	events = port->event_ops->get_events(port);

	/* [한국어] SoC 가 쓰지 않기로 한 이벤트를 걸러낸다. 예컨대 StarFive 는 두 개의 도어벨
	 * 비트를 events_bitmap 에서 빼 두어 하드웨어가 보고해도 무시된다. */
	events &= port->events_bitmap;
	/* [한국어] 남은 이벤트마다 순회한다. 상한 num_events 는 SoC 가 지정한 값이다. */
	for_each_set_bit(bit, &events, port->num_events)
		/* [한국어] 이벤트 도메인의 해당 IRQ 를 실행한다. INTx/MSI 이벤트라면 그 virq 에
		 * 체인 핸들러가 걸려 있어 plda_handle_intx/plda_handle_msi 로 한 단계 더 내려간다.
		 * 반환값을 보지 않는 것은 events_bitmap 이 이미 유효한 이벤트만 남겼기 때문이다. */
		generic_handle_domain_irq(port->event_domain, bit);

	/* [한국어] 체인 핸들러 퇴장. */
	chained_irq_exit(chip, desc);
}

/*
 * [한국어]
 * plda_hwirq_to_mask - 이벤트 hwirq 번호를 IMASK_LOCAL/ISTATUS_LOCAL 비트 마스크로 되돌린다
 *
 * @hwirq: 이벤트 도메인의 하드웨어 번호(0..PLDA_MAX_EVENT_NUM-1).
 * @return: 그 이벤트에 해당하는 레지스터 비트 마스크. INTx 만 4비트짜리다.
 *
 * 왜 필요한가: plda_get_events() 가 레지스터 -> 이벤트로 접었으므로, ack/mask/unmask
 * 는 반대로 펴야 한다. 이 함수가 그 역변환이며 세 콜백이 모두 첫 줄에서 호출한다.
 *
 * 변환 규칙(세 구간):
 *  1. hwirq < EVENT_PM_MSI_INT_INTX(24) : 레지스터 비트와 번호가 같으므로 BIT(hwirq).
 *     상류 영문 주석 "hwirq 23 - 0 are the same with register" 그대로다.
 *  2. hwirq == 24 : INTx 하나가 레지스터에서는 네 비트이므로 PM_MSI_INT_INTX_MASK
 *     (0x0f000000)를 통째로 돌려준다. 즉 INTx 를 마스크하면 INTA~INTD 가 한꺼번에
 *     막힌다 -- 개별 INTx 마스크는 이 이벤트 계층이 아니라 아래 intx_domain 의
 *     plda_mask_intx_irq() 가 담당한다.
 *  3. hwirq > 24 : BIT(hwirq + PCI_NUM_INTX - 1) = BIT(hwirq + 3). 접히면서 3칸
 *     당겨졌던 것을 다시 3칸 민다. 25->28(MSI), 26->29(AER), 27->30(기타),
 *     28->31(SYS_ERR).
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트 또는 프로세스 컨텍스트 -- 부르는 콜백에
 * 따라 다르다. 순수 계산이라 어느 쪽이든 안전하고 재진입 가능하다.
 * 호출자: plda_ack_event_irq(), plda_mask_event_irq(), plda_unmask_event_irq().
 * Microchip 은 이 함수를 쓰지 않는다 -- 이벤트마다 레지스터가 달라 event_descs[]
 * 표를 따로 두었기 때문이다.
 * 피호출자: 없음.
 * 에러 경로: 없음. 범위를 벗어난 hwirq 에 대한 검사는 없으나, 호출자가 항상
 * 도메인에서 온 값을 넘기므로 num_events 미만이 보장된다.
 *
 * 호출 체인:
 *   handle_level_irq() -> plda_event_irq_chip 의 ack/mask/unmask -> [이 함수]
 */
static u32 plda_hwirq_to_mask(int hwirq)
{
	u32 mask;

	/* hwirq 23 - 0 are the same with register */
	if (hwirq < EVENT_PM_MSI_INT_INTX)
		/* [한국어] hwirq 23 이하는 레지스터 비트 번호와 같으므로 그대로 BIT() 한다. */
		mask = BIT(hwirq);
	/* [한국어] hwirq 24 는 INTx -- 유일하게 레지스터에서 여러 비트를 차지하는 이벤트다. */
	else if (hwirq == EVENT_PM_MSI_INT_INTX)
		/* [한국어] INTA~INTD 네 비트(0x0f000000)를 통째로 돌려준다. 따라서 이 이벤트를 마스크하면
		 * 네 개의 legacy 인터럽트가 한꺼번에 막힌다. 개별 차단은 아래 계층인
		 * intx_domain 의 plda_mask_intx_irq 가 담당한다. */
		mask = PM_MSI_INT_INTX_MASK;
	else
		/* [한국어] hwirq 25 이상은 접히면서 3칸 앞당겨졌으므로 다시 3칸 민다.
		 * PCI_NUM_INTX - 1 = 3. 25->비트28(MSI), 26->29(AER), 27->30(기타), 28->31(SYS_ERR). */
		mask = BIT(hwirq + PCI_NUM_INTX - 1);

	/* [한국어] 완성된 레지스터 비트 마스크를 돌려준다. */
	return mask;
}

/*
 * [한국어]
 * plda_ack_event_irq - 처리한 이벤트의 ISTATUS_LOCAL 비트를 지운다
 *
 * @data: 이벤트 irq_data. hwirq 가 이벤트 번호, chip_data 가 컨트롤러
 *        (plda_pcie_event_map 이 심어 둔 값).
 * @return: 없음.
 *
 * 왜 필요한가: ISTATUS_LOCAL 은 래치형 상태 레지스터라 지우지 않으면 같은 이벤트가
 * 계속 보고되어 인터럽트가 끝나지 않는다.
 *
 * 동작: plda_hwirq_to_mask(hwirq) 로 비트 마스크를 만들어 ISTATUS_LOCAL 에 쓴다.
 * write-1-to-clear 이므로 읽지 않고 그 비트만 쓴다 -- 다른 비트를 0 으로 쓰는 것은
 * 아무 효과가 없으므로 락 없이 안전하다. 이것이 IMASK_LOCAL(mask/unmask)과 달리
 * 락이 필요 없는 이유다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트(handle_level_irq 내부).
 * 호출자: 커널 IRQ 코어가 plda_event_irq_chip.irq_ack 로 간접 호출.
 * 피호출자: plda_hwirq_to_mask, writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_handle_event() -> generic_handle_domain_irq() -> handle_level_irq()
 *     -> chip->irq_ack = [이 함수]
 */
static void plda_ack_event_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);

	writel_relaxed(plda_hwirq_to_mask(data->hwirq),
		       port->bridge_addr + ISTATUS_LOCAL);
}

/*
 * [한국어]
 * plda_mask_event_irq - 이벤트 하나를 IMASK_LOCAL 에서 꺼 인터럽트를 막는다
 *
 * @data: 이벤트 irq_data(hwirq = 이벤트 번호), chip_data 는 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: handle_level_irq 는 핸들러 실행 전에 반드시 소스를 마스크한다.
 * 그렇지 않으면 레벨 인터럽트가 즉시 재발화해 무한 루프에 빠진다.
 *
 * 동작 단계:
 *  1. plda_hwirq_to_mask 로 비트 마스크를 얻는다.
 *  2. raw_spin_lock(&port->lock) -- IMASK_LOCAL 은 read-modify-write 이고
 *     INTx 쪽 mask/unmask 와 같은 레지스터를 공유하므로 반드시 배타적이어야 한다.
 *  3. 읽고, 해당 비트를 AND-NOT 으로 지우고, 되쓴다.
 *  4. raw_spin_unlock.
 *
 * INTx 판(plda_mask_intx_irq)과 달리 irqsave 가 아닌 그냥 raw_spin_lock 을 쓴다.
 * 이벤트 irq_chip 콜백은 항상 인터럽트가 이미 막힌 상태(chained handler 아래의
 * handle_level_irq)에서만 불린다는 전제 때문이다. 반대로 INTx 는 장치 드라이버가
 * 임의 시점에 disable_irq() 로 부를 수 있어 irqsave 가 필요하다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트.
 * 호출자: 커널 IRQ 코어가 irq_mask 로 간접 호출.
 * 피호출자: plda_hwirq_to_mask, raw_spin_lock/unlock, readl_relaxed, writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   handle_level_irq() -> chip->irq_mask = [이 함수]
 */
static void plda_mask_event_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] mask 는 조작할 비트, val 은 read-modify-write 임시 변수. */
	u32 mask, val;

	/* [한국어] 이벤트 번호를 레지스터 비트 마스크로 되돌린다. */
	mask = plda_hwirq_to_mask(data->hwirq);

	raw_spin_lock(&port->lock);
	/* [한국어] 현재 마스크 값을 읽는다. */
	val = readl_relaxed(port->bridge_addr + IMASK_LOCAL);
	/* [한국어] 해당 비트를 지운다(1 = 허용이므로 0 이 차단). */
	val &= ~mask;
	/* [한국어] 되쓴다. 이 이벤트는 이제 CPU 로 올라오지 않는다. */
	writel_relaxed(val, port->bridge_addr + IMASK_LOCAL);
	raw_spin_unlock(&port->lock);
}

/*
 * [한국어]
 * plda_unmask_event_irq - 이벤트 하나를 IMASK_LOCAL 에서 켜 인터럽트를 허용한다
 *
 * @data: 이벤트 irq_data(hwirq = 이벤트 번호), chip_data 는 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: plda_mask_event_irq 의 짝. 핸들러가 끝난 뒤, 그리고 최초
 * request_irq 시점에 이 콜백으로 이벤트가 처음 켜진다 -- 즉 IMASK_LOCAL 의 초기
 * 상태가 0(전부 마스크)이어도 request_irq 가 필요한 비트를 하나씩 열어 준다.
 *
 * 동작: mask 판과 동일하되 비트를 OR 로 세운다. 락 규칙(왜 irqsave 가 아닌지)도
 * plda_mask_event_irq 주석과 동일하다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트 또는 request_irq 경로(프로세스 컨텍스트).
 * 호출자: 커널 IRQ 코어가 irq_unmask 로 간접 호출.
 * 피호출자: plda_hwirq_to_mask, raw_spin_lock/unlock, readl_relaxed, writel_relaxed.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   request_irq() 또는 handle_level_irq() 종료 -> chip->irq_unmask = [이 함수]
 */
static void plda_unmask_event_irq(struct irq_data *data)
{
	struct plda_pcie_rp *port = irq_data_get_irq_chip_data(data);
	/* [한국어] mask 는 조작할 비트, val 은 read-modify-write 임시 변수. */
	u32 mask, val;

	/* [한국어] 이벤트 번호를 레지스터 비트 마스크로 되돌린다. */
	mask = plda_hwirq_to_mask(data->hwirq);

	raw_spin_lock(&port->lock);
	/* [한국어] 현재 마스크 값을 읽는다. */
	val = readl_relaxed(port->bridge_addr + IMASK_LOCAL);
	/* [한국어] 해당 비트를 세워 이벤트를 허용한다. request_irq 시점에도 이 경로로 처음 켜진다. */
	val |= mask;
	/* [한국어] 되쓴다. */
	writel_relaxed(val, port->bridge_addr + IMASK_LOCAL);
	raw_spin_unlock(&port->lock);
}

/*
 * [한국어] PLDA 기본판 이벤트 계층의 irq_chip.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_init_interrupts() 가 port->event_irq_chip
 * 이 비어 있을 때 기본값으로 꽂고, plda_pcie_event_map() 이 그것을 각 virq 에 붙인다.
 * 세 콜백 모두 plda_hwirq_to_mask() 로 이벤트 번호를 레지스터 비트로 되돌린 뒤
 * ISTATUS_LOCAL(ack) 또는 IMASK_LOCAL(mask/unmask)을 조작한다.
 * Microchip 은 이벤트마다 대상 레지스터와 마스크 극성이 달라 이 chip 대신
 * mc_event_irq_chip 을 쓴다.
 * 동기화: 상수 테이블이지만 mask/unmask 내부에서 port->lock 을 잡는다.
 */
static struct irq_chip plda_event_irq_chip = {
	.name = "PLDA PCIe EVENT",
	.irq_ack = plda_ack_event_irq,
	.irq_mask = plda_mask_event_irq,
	.irq_unmask = plda_unmask_event_irq,
};

/*
 * [한국어] PLDA 기본판 이벤트 수집 ops -- get_events 하나만 채운다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_init_interrupts() 가 port->event_ops 가
 * NULL 일 때 기본값으로 꽂는다. 즉 이 트리에서는 StarFive 경로에서만 쓰이며,
 * Microchip 은 mc_event_ops 를 미리 지정해 이 기본값이 적용되지 않는다.
 * 동기화: 상수.
 */
static const struct plda_event_ops plda_event_ops = {
	.get_events = plda_get_events,
};

/*
 * [한국어]
 * plda_pcie_event_map - 이벤트 도메인의 virq 하나에 irq_chip 과 flow handler 를 붙인다
 *
 * @domain: event_domain. host_data 가 struct plda_pcie_rp.
 * @irq: 새로 만들어진 가상 IRQ 번호.
 * @hwirq: 이벤트 번호. 이 함수는 값을 쓰지 않는다(모든 이벤트가 같은 chip 을 쓰므로).
 * @return: 항상 0.
 *
 * 왜 필요한가: INTx 판(plda_pcie_intx_map)과 같은 역할이되, 붙이는 irq_chip 이
 * 고정 상수가 아니라 port->event_irq_chip 이라는 점이 다르다. 그 덕분에 Microchip 이
 * 자기 mc_event_irq_chip 을 끼워 넣을 수 있다 -- 이 한 줄이 코어를 SoC 중립으로
 * 유지하는 핵심이다.
 *
 * 동작:
 *  1. domain->host_data 를 struct plda_pcie_rp 로 캐스팅한다. (void 캐스팅을 거치는
 *     것은 host_data 가 void 포인터이기 때문이다.)
 *  2. irq_set_chip_and_handler 로 port->event_irq_chip 과 handle_level_irq 지정.
 *     이벤트는 상태 래치 기반이므로 레벨 처리다.
 *  3. irq_set_chip_data 로 port 를 chip_data 에 심어 ack/mask/unmask 가
 *     컨트롤러를 되찾을 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. plda_init_interrupts() 의 irq_create_mapping()
 * 안에서 이벤트마다 한 번씩 불린다.
 * 호출자: 커널 irq 도메인 코어가 plda_event_domain_ops.map 으로 간접 호출.
 * 피호출자: irq_set_chip_and_handler, irq_set_chip_data.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_init_interrupts() -> irq_create_mapping() -> [이 함수]
 */
static int plda_pcie_event_map(struct irq_domain *domain, unsigned int irq,
			       irq_hw_number_t hwirq)
{
	struct plda_pcie_rp *port = (void *)domain->host_data;

	/* [한국어] port->event_irq_chip 을 붙인다 -- 상수가 아니라 필드를 참조하는 것이 핵심으로,
	 * 이 덕분에 Microchip 이 자기 mc_event_irq_chip 을 끼워 넣을 수 있다.
	 * handle_level_irq 인 것은 이벤트가 상태 래치 기반(레벨)이기 때문이다. */
	irq_set_chip_and_handler(irq, port->event_irq_chip, handle_level_irq);
	/* [한국어] port 를 chip_data 로 심어 ack/mask/unmask 가 컨트롤러를 되찾게 한다. */
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

/*
 * [한국어] 이벤트 도메인의 도메인 연산 -- .map 하나뿐이다.
 *
 * 설정자: 컴파일 타임 상수. 읽는 자: plda_pcie_init_irq_domains() 의
 * irq_domain_create_linear().
 * 이벤트는 hwirq 가 0..num_events-1 로 미리 정해져 있어 선형 매핑으로 충분하다.
 * SoC 별 차이는 여기가 아니라 .map 이 참조하는 port->event_irq_chip 이 흡수한다.
 * 동기화: 상수.
 */
static const struct irq_domain_ops plda_event_domain_ops = {
	.map = plda_pcie_event_map,
};

/*
 * [한국어]
 * plda_pcie_init_irq_domains - 이벤트/INTx/MSI 세 개의 irq_domain 을 만든다
 *
 * @port: dev 와 num_events 가 채워진 컨트롤러.
 * @return: 0 성공. -EINVAL 은 devicetree 에 인터럽트 컨트롤러 자식 노드가 없음,
 *          -ENOMEM 은 도메인 생성 실패. 반환값은 그대로 probe 실패로 이어진다.
 *
 * 왜 필요한가: 커널이 이 컨트롤러의 인터럽트를 devicetree 의 interrupt-map/
 * interrupts 로 참조하려면, 해당 fwnode 에 묶인 irq_domain 이 있어야 한다.
 * PLDA 는 계층이 둘(이벤트 위에 INTx/MSI)이므로 도메인도 여러 개가 필요하다.
 *
 * 동작 단계:
 *  1. of_get_next_child(node, NULL) 로 PCIe 노드의 "첫 번째 자식" 을 가져온다.
 *     devicetree 관례상 이 자식이 legacy 인터럽트 컨트롤러 노드(보통 이름이
 *     interrupt-controller)다. 이름으로 찾지 않고 첫 자식을 쓴다는 점이 함정이다.
 *     이 호출은 참조 카운트를 올리므로 모든 탈출 경로에서 of_node_put 이 필요하다.
 *  2. 그 fwnode 위에 크기 port->num_events 인 선형 도메인(event_domain)을 만든다.
 *     선형(linear)은 hwirq 0..size-1 을 배열로 직접 인덱싱한다는 뜻으로, 번호가
 *     작고 조밀할 때 가장 빠르다.
 *  3. 실패하면 of_node_put 후 -ENOMEM.
 *  4. irq_domain_update_bus_token(DOMAIN_BUS_NEXUS) -- 같은 fwnode 에 도메인이
 *     두 개(이벤트, INTx) 붙으므로, 커널이 fwspec 을 해석할 때 어느 쪽인지
 *     구별할 수 있게 버스 토큰으로 꼬리표를 단다. 이 두 줄이 없으면 두 도메인이
 *     충돌한다. NEXUS 는 "다른 도메인들을 묶는 중간 도메인" 을 뜻한다.
 *  5. 같은 fwnode 로 크기 PCI_NUM_INTX(4)인 intx_domain 을 만들고, 실패 시 정리 후
 *     -ENOMEM.
 *  6. intx_domain 에는 DOMAIN_BUS_WIRED 토큰을 단다 -- legacy INTx 는 물리적으로
 *     배선된 인터럽트라는 의미이고, devicetree 의 interrupt-map 해석이 이 토큰으로
 *     이 도메인을 찾아온다.
 *  7. of_node_put 으로 참조를 반납하고 port->lock 을 초기화한다. 락 초기화가 여기
 *     있는 이유는 다음 줄에서 MSI 도메인이 만들어지는 순간부터 irq_chip 콜백이
 *     들어올 수 있기 때문이다.
 *  8. plda_allocate_msi_domains(port) 의 결과를 그대로 반환한다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 메모리 할당과 devicetree 순회를 하므로
 * 잠들 수 있다.
 * 호출자: plda_init_interrupts() 하나뿐.
 * 피호출자: of_get_next_child, irq_domain_create_linear, of_fwnode_handle,
 * irq_domain_update_bus_token, of_node_put, raw_spin_lock_init,
 * plda_allocate_msi_domains.
 * 에러 경로: 각 실패마다 of_node_put 을 호출해 노드 참조는 새지 않는다. 다만
 * intx_domain 생성 실패 시 이미 만든 event_domain 은 제거하지 않는다 -- 코드가
 * 그렇게 되어 있다는 사실만 적어 둔다.
 *
 * 호출 체인:
 *   plda_pcie_host_init() 또는 mc_platform_init() -> plda_init_interrupts()
 *     -> [이 함수] -> plda_allocate_msi_domains()
 */
static int plda_pcie_init_irq_domains(struct plda_pcie_rp *port)
{
	struct device *dev = port->dev;
	/* [한국어] 이 컨트롤러의 devicetree 노드. 아래에서 자식 노드를 찾는 데 쓴다. */
	struct device_node *node = dev->of_node;
	/* [한국어] legacy 인터럽트 컨트롤러 자식 노드. event_domain 과 intx_domain 이 모두
	 * 이 노드의 fwnode 에 묶인다. */
	struct device_node *pcie_intc_node;

	/* Setup INTx */
	pcie_intc_node = of_get_next_child(node, NULL);
	/* [한국어] 자식 노드가 없으면 devicetree 가 잘못된 것이다. 이름으로 찾지 않고 '첫 자식' 을
	 * 쓰는 관례라, 다른 성격의 자식 노드가 앞에 오면 오동작할 수 있다. */
	if (!pcie_intc_node) {
		/* [한국어] 실패 원인을 남긴다. */
		dev_err(dev, "failed to find PCIe Intc node\n");
		return -EINVAL;
	}

	/* [한국어] 이벤트 도메인을 만든다. linear 는 hwirq 0..size-1 을 배열로 직접 인덱싱하는
	 * 방식으로, 번호가 작고 조밀한 이 경우에 가장 빠르다. of_fwnode_handle 로
	 * devicetree 노드를 fwnode 로 바꿔 넘긴다. */
	port->event_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node),
						      port->num_events, &plda_event_domain_ops,
						      port);
	/* [한국어] 도메인 생성 실패는 메모리 부족이다. */
	if (!port->event_domain) {
		/* [한국어] 실패 원인을 남긴다. */
		dev_err(dev, "failed to get event domain\n");
		of_node_put(pcie_intc_node);
		return -ENOMEM;
	}

	/* [한국어] 같은 fwnode 에 도메인이 둘(이벤트, INTx) 붙으므로 버스 토큰으로 구별한다.
	 * 이 두 줄이 없으면 fwspec 해석이 잘못된 도메인을 고를 수 있다.
	 * NEXUS 는 '다른 도메인들을 묶는 중간 도메인' 이라는 뜻이다. */
	irq_domain_update_bus_token(port->event_domain, DOMAIN_BUS_NEXUS);

	/* [한국어] 같은 fwnode 위에 INTx 도메인을 만든다. 크기는 PCI_NUM_INTX(4) 고정 --
	 * legacy 인터럽트는 규격상 네 개뿐이다. */
	port->intx_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						     &intx_domain_ops, port);
	/* [한국어] 생성 실패. */
	if (!port->intx_domain) {
		/* [한국어] 실패 원인을 남긴다. */
		dev_err(dev, "failed to get an INTx IRQ domain\n");
		of_node_put(pcie_intc_node);
		return -ENOMEM;
	}

	/* [한국어] INTx 도메인에는 WIRED 토큰을 단다 -- 물리적으로 배선된 인터럽트라는 뜻이며,
	 * devicetree 의 interrupt-map 해석이 이 토큰으로 이 도메인을 찾아온다. */
	irq_domain_update_bus_token(port->intx_domain, DOMAIN_BUS_WIRED);

	of_node_put(pcie_intc_node);
	raw_spin_lock_init(&port->lock);

	/* [한국어] MSI 부모 도메인 생성 결과를 그대로 반환한다. 세 도메인 중 마지막이다. */
	return plda_allocate_msi_domains(port);
}

/*
 * [한국어]
 * plda_init_interrupts - 인터럽트 도메인 생성부터 체인 핸들러 연결까지 전부 수행한다
 *
 * @pdev: 이 컨트롤러의 플랫폼 디바이스. platform_get_irq 와 devm_request_irq 의
 *        기준이 된다.
 * @port: dev/num_events/events_bitmap 이 채워진 컨트롤러. event_ops 와
 *        event_irq_chip 은 비어 있어도 되며 이 함수가 기본값을 넣어 준다.
 * @event: SoC 가 알려 주는 INTx/MSI 이벤트 번호와 IRQ 요청 방식.
 * @return: 0 성공. -ENODEV(부모 IRQ 없음), -ENXIO(매핑 실패),
 *          그 밖에 도메인 생성/IRQ 요청 실패 코드.
 *
 * 왜 필요한가: 두 SoC 드라이버가 공통으로 거치는 유일한 인터럽트 설정 지점이다.
 * 여기서 만들어지는 구조가 이 드라이버 인터럽트 아키텍처의 전부다:
 *
 *   SoC PLIC/GIC (port->irq)
 *     └─ plda_handle_event (chained)
 *          └─ event_domain (hwirq 0..num_events-1)
 *               ├─ 대부분의 이벤트 → plda_event_handler / mc_event_handler
 *               ├─ intx_event → plda_handle_intx (chained)
 *               │                └─ intx_domain (INTA..INTD) → 장치 핸들러
 *               └─ msi_event  → plda_handle_msi (chained)
 *                                └─ msi.dev_domain (벡터) → 장치 핸들러
 *
 * 동작 단계:
 *  1. event_ops 가 비어 있으면 코어 기본 plda_event_ops 를 꽂는다(Microchip 은
 *     이미 채워 두었으므로 건드리지 않는다).
 *  2. event_irq_chip 도 같은 방식으로 기본값 plda_event_irq_chip 을 채운다.
 *  3. plda_pcie_init_irq_domains() 로 세 도메인을 만든다.
 *  4. platform_get_irq(pdev, 0) 으로 컨트롤러의 부모 IRQ 를 얻는다. 음수면 -ENODEV.
 *  5. events_bitmap 에 켜진 이벤트마다:
 *     - irq_create_mapping 으로 event_domain 안의 virq 를 만든다(이때
 *       plda_pcie_event_map 이 불려 chip 과 handler 가 붙는다).
 *     - SoC 가 request_event_irq 를 주었으면 그것을, 아니면 devm_request_irq 로
 *       plda_event_handler 를 등록한다. 등록 자체가 목적이며(spurious 방지),
 *       이 과정에서 irq_unmask 가 불려 IMASK_LOCAL 의 해당 비트가 실제로 켜진다.
 *  6. intx_event 번호를 event_domain 에 매핑하고, 그 virq 에
 *     irq_set_chained_handler_and_data 로 plda_handle_intx 를 건다. 이 virq 는
 *     5번 루프에서 이미 devm_request_irq 된 상태일 수 있는데, 체인 핸들러 설정이
 *     flow handler 를 갈아끼우므로 결과적으로 체인 처리가 이긴다.
 *  7. msi_event 도 같은 방식으로 plda_handle_msi 를 건다.
 *  8. 마지막으로 부모 IRQ(port->irq)에 plda_handle_event 를 건다. 이 줄이 실행되는
 *     순간부터 실제로 인터럽트가 들어오기 시작하므로, 아래 계층이 전부 준비된
 *     맨 마지막에 두는 것이 중요하다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. devm_ 할당과 도메인 생성으로 잠들 수 있다.
 * 호출자: plda_pcie_host_init()(StarFive 경로), mc_platform_init()(Microchip 경로).
 * 피호출자: plda_pcie_init_irq_domains, platform_get_irq, irq_create_mapping,
 * devm_request_irq 또는 event->request_event_irq, irq_set_chained_handler_and_data.
 * 에러 경로: 중간에 실패하면 그 자리에서 음수를 반환한다. devm_ 로 요청한 IRQ 는
 * 디바이스 해제 시 자동 반납되지만, 이미 만든 irq 매핑과 도메인은 여기서 되돌리지
 * 않는다. 정리는 호출자 쪽 plda_pcie_irq_domain_deinit() 이 담당한다.
 *
 * 호출 체인:
 *   starfive_pcie_probe() -> plda_pcie_host_init() -> [이 함수]
 *   mc_host_probe() -> pci_host_common_probe() -> mc_platform_init() -> [이 함수]
 */
int plda_init_interrupts(struct platform_device *pdev,
			 struct plda_pcie_rp *port,
			 const struct plda_event *event)
{
	struct device *dev = &pdev->dev;
	/* [한국어] event_irq 는 irq_create_mapping 이 돌려주는 virq, ret 은 에러 코드. */
	int event_irq, ret;
	/* [한국어] for_each_set_bit 의 인덱스. events_bitmap 의 비트 번호이자 이벤트 hwirq 다. */
	u32 i;

	/* [한국어] SoC 가 이벤트 수집 훅을 지정하지 않았으면 */
	if (!port->event_ops)
		/* [한국어] 코어 기본 구현을 꽂는다. Microchip 은 이미 mc_event_ops 를 채워 두어 그대로 남는다. */
		port->event_ops = &plda_event_ops;

	/* [한국어] SoC 가 이벤트 irq_chip 을 지정하지 않았으면 */
	if (!port->event_irq_chip)
		/* [한국어] 코어 기본 chip 을 꽂는다. 이 두 쌍의 기본값 주입이 '코어는 SoC 를 모른다' 는
		 * 설계를 성립시킨다. */
		port->event_irq_chip = &plda_event_irq_chip;

	/* [한국어] 이벤트/INTx/MSI 세 도메인을 만든다. 이후 모든 단계가 이 도메인들에 의존한다. */
	ret = plda_pcie_init_irq_domains(port);
	/* [한국어] 도메인 생성 실패는 회복 불가이므로 그대로 probe 를 접는다. */
	if (ret) {
		/* [한국어] 실패 원인을 남긴다. */
		dev_err(dev, "failed creating IRQ domains\n");
		return ret;
	}

	/* [한국어] devicetree 의 첫 번째 인터럽트를 얻는다. 이것이 컨트롤러 전체를 대표하는
	 * 부모 IRQ 이고, 맨 마지막에 plda_handle_event 가 여기에 걸린다. */
	port->irq = platform_get_irq(pdev, 0);
	/* [한국어] 음수는 인터럽트가 지정되지 않았다는 뜻 -- 이 드라이버는 인터럽트 없이 동작할 수 없다. */
	if (port->irq < 0)
		return -ENODEV;

	/* [한국어] SoC 가 쓰기로 한 이벤트마다 IRQ 를 매핑하고 요청한다. 요청하지 않은 IRQ 가
	 * 발생하면 커널이 spurious 로 판단해 그 라인을 꺼 버리므로, 실제로 할 일이 없는
	 * 이벤트도 반드시 등록해야 한다. */
	for_each_set_bit(i, &port->events_bitmap, port->num_events) {
		/* [한국어] 이벤트 도메인 안에 virq 를 만든다. 이때 plda_pcie_event_map 이 불려
		 * irq_chip 과 handle_level_irq 가 붙는다. */
		event_irq = irq_create_mapping(port->event_domain, i);
		/* [한국어] 0 은 매핑 실패를 뜻한다(유효한 virq 는 1 이상). */
		if (!event_irq) {
			/* [한국어] 실패한 hwirq 번호를 남긴다. */
			dev_err(dev, "failed to map hwirq %d\n", i);
			return -ENXIO;
		}

		/* [한국어] SoC 가 IRQ 요청 방식을 지정했으면 */
		if (event->request_event_irq)
			/* [한국어] 그것을 쓴다. Microchip 이 이벤트 이름 문자열을 /proc/interrupts 에 남기려고 쓴다. */
			ret = event->request_event_irq(port, event_irq, i);
		else
			/* [한국어] 아니면 코어 기본 -- 아무 일도 하지 않는 plda_event_handler 를 등록한다.
			 * 이름(devname)에 NULL 을 넘기므로 /proc/interrupts 에는 이름 없이 나온다.
			 * 이 devm_request_irq 안에서 irq_unmask 가 불려 IMASK_LOCAL 의 해당 비트가
			 * 실제로 켜진다 -- 즉 여기가 이벤트가 하드웨어적으로 활성화되는 지점이다. */
			ret = devm_request_irq(dev, event_irq,
					       plda_event_handler,
					       0, NULL, port);

		/* [한국어] 요청 실패는 회복 불가. */
		if (ret) {
			/* [한국어] 실패한 virq 를 남긴다. */
			dev_err(dev, "failed to request IRQ %d\n", event_irq);
			return ret;
		}
	}

	/* [한국어] INTx 묶음 이벤트의 hwirq 를 virq 로 매핑한다. 이 번호는 SoC 마다 다르므로
	 * struct plda_event 로 전달받는다(StarFive 24, Microchip 23). */
	port->intx_irq = irq_create_mapping(port->event_domain,
					    event->intx_event);
	/* [한국어] 매핑 실패. */
	if (!port->intx_irq) {
		/* [한국어] 실패 원인을 남긴다. */
		dev_err(dev, "failed to map INTx interrupt\n");
		return -ENXIO;
	}

	/* Plug the INTx chained handler */
	irq_set_chained_handler_and_data(port->intx_irq, plda_handle_intx, port);

	/* [한국어] MSI 묶음 이벤트의 hwirq 를 virq 로 매핑한다(StarFive 25, Microchip 24). */
	port->msi_irq = irq_create_mapping(port->event_domain,
					   event->msi_event);
	/* [한국어] 매핑 실패. INTx 와 달리 로그 없이 곧장 -ENXIO 다. */
	if (!port->msi_irq)
		return -ENXIO;

	/* Plug the MSI chained handler */
	irq_set_chained_handler_and_data(port->msi_irq, plda_handle_msi, port);

	/* Plug the main event chained handler */
	irq_set_chained_handler_and_data(port->irq, plda_handle_event, port);

	return 0;
}
EXPORT_SYMBOL_GPL(plda_init_interrupts);

/*
 * [한국어]
 * plda_pcie_setup_window - 아웃바운드(AXI -> PCIe) 주소 변환 창 하나를 프로그래밍한다
 *
 * @bridge_base_addr: 브리지 APB 레지스터 창의 커널 가상 주소.
 * @index: 창 번호. 0 은 관례적으로 config space 전용이고 1 이상이 메모리 창이다.
 * @axi_addr: CPU/AXI 쪽에서 이 창이 차지하는 시작 물리 주소(변환의 입력).
 * @pci_addr: PCIe 버스 쪽 시작 주소(변환의 출력).
 * @size: 창 크기. 반드시 2의 거듭제곱이어야 한다 -- 하드웨어가 크기를 로그값으로만
 *        표현하기 때문이며, 이 함수는 그 검사를 하지 않는다.
 * @return: 없음.
 *
 * 왜 필요한가: PLDA 브리지는 CPU 가 낸 AXI 주소를 그대로 PCIe 로 내보내지 않고,
 * ATR(Address Translation) 테이블을 거쳐 재사상한다. 이 테이블을 채우지 않으면
 * CPU 가 PCIe 메모리 공간이나 config space 에 전혀 접근할 수 없다. PCI 코어가
 * devicetree 의 ranges 로 얻은 자원을 실제 하드웨어에 반영하는 지점이 여기다.
 *
 * 동작 단계:
 *  1. atr_sz = ilog2(size) - 1. 하드웨어의 크기 필드는 "2^(값+1) 바이트" 규약이다.
 *  2. index 가 0 이면 TRSL_PARAM 에 PCIE_CONFIG_INTERFACE(1)를, 아니면
 *     PCIE_TX_RX_INTERFACE(0)를 쓴다. 이 한 비트가 "이 창의 접근을 config 요청
 *     TLP 로 만들 것인가, 일반 메모리 TLP 로 만들 것인가" 를 가른다.
 *  3. 소스 주소 파라미터: axi_addr 의 하위 32비트를 4KiB 로 내림 정렬하고
 *     (하위 12비트를 비움), 그 빈 자리에 크기 필드(비트 6:1)와
 *     ATR_IMPL_ENABLE(비트 0)을 OR 해 한 워드로 쓴다. 4KiB 정렬이 강제되는 이유가
 *     바로 이 필드 겹침이다.
 *  4. 소스 주소 상위 32비트를 별도 레지스터에 쓴다.
 *  5. 변환 목적지(PCIe) 주소의 하위/상위 32비트를 각각 쓴다.
 *  모든 레지스터 오프셋에 index * ATR_ENTRY_SIZE(32) 를 더해 창별 항목을 고른다.
 *  여기서는 relaxed 가 아닌 writel 을 쓴다 -- 이 설정이 끝난 뒤 곧바로 config 접근이
 *  일어나므로 쓰기가 하드웨어에 확실히 도달하는 순서 보장이 필요하기 때문이다.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음(초기화 중 단독 접근).
 * 호출자: plda_pcie_host_init()(0번 창), plda_pcie_setup_iomems()(1번 이후),
 * mc_platform_init()(Microchip 의 0번 창).
 * 피호출자: ilog2, ALIGN_DOWN, FIELD_PREP, lower_32_bits/upper_32_bits, writel.
 * 에러 경로: 없음 -- 잘못된 size 나 index 를 검사하지 않는다.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> [이 함수] (index 0, config 창)
 *   plda_pcie_setup_iomems() -> [이 함수] (index 1.., 메모리 창)
 *   mc_platform_init() -> [이 함수] (index 0)
 */
void plda_pcie_setup_window(void __iomem *bridge_base_addr, u32 index,
			    phys_addr_t axi_addr, phys_addr_t pci_addr,
			    size_t size)
{
	u32 atr_sz = ilog2(size) - 1;
	/* [한국어] 각 ATR 레지스터에 쓸 값을 만드는 임시 변수. 다섯 번 재사용된다. */
	u32 val;

	/* [한국어] 0번 창은 config space 전용이라는 관례 */
	if (index == 0)
		/* [한국어] TRSL_PARAM 에 1 을 써 이 창의 접근이 PCIe Configuration Request TLP 로
		 * 만들어지게 한다. */
		val = PCIE_CONFIG_INTERFACE;
	else
		/* [한국어] 그 외 창은 일반 메모리 TLP(TX/RX 인터페이스)로 나간다. */
		val = PCIE_TX_RX_INTERFACE;

	/* [한국어] 인터페이스 선택을 먼저 쓴다. 기준 주소에 index * 32 를 더해 해당 창의 항목을 고른다. */
	writel(val, bridge_base_addr + (index * ATR_ENTRY_SIZE) +
	       ATR0_AXI4_SLV0_TRSL_PARAM);

	/* [한국어] 소스(AXI) 주소 하위 32비트를 4KiB 로 내림 정렬해 하위 12비트를 비운다.
	 * 그 빈 자리에 다음 두 줄의 크기 필드와 enable 비트가 겹쳐 들어가기 때문이다. */
	val = ALIGN_DOWN(lower_32_bits(axi_addr), SZ_4K);
	/* [한국어] 크기 필드(비트 6:1)에 log2(size)-1 을 넣는다. 하드웨어는 크기를 2^(값+1) 로 해석한다. */
	val |= FIELD_PREP(ATR_SIZE_MASK, atr_sz);
	/* [한국어] bit0 을 세워 이 변환 창을 실제로 켠다. 이 비트가 없으면 창이 무시된다. */
	val |= ATR_IMPL_ENABLE;
	/* [한국어] 합쳐진 한 워드를 SRCADDR_PARAM 에 쓴다. */
	writel(val, bridge_base_addr + (index * ATR_ENTRY_SIZE) +
	       ATR0_AXI4_SLV0_SRCADDR_PARAM);

	/* [한국어] 소스(AXI) 주소 상위 32비트. */
	val = upper_32_bits(axi_addr);
	/* [한국어] 별도 레지스터(SRC_ADDR)에 쓴다. 하위 32비트는 앞의 SRCADDR_PARAM 에 들어 있다. */
	writel(val, bridge_base_addr + (index * ATR_ENTRY_SIZE) +
	       ATR0_AXI4_SLV0_SRC_ADDR);

	/* [한국어] 변환 결과(PCIe) 주소 하위 32비트. */
	val = lower_32_bits(pci_addr);
	/* [한국어] TRSL_ADDR_LSB 에 쓴다. */
	writel(val, bridge_base_addr + (index * ATR_ENTRY_SIZE) +
	       ATR0_AXI4_SLV0_TRSL_ADDR_LSB);

	/* [한국어] 변환 결과(PCIe) 주소 상위 32비트. */
	val = upper_32_bits(pci_addr);
	/* [한국어] TRSL_ADDR_UDW 에 쓴다. 여기까지 다섯 개 레지스터를 채우면 창 하나가 완성된다.
	 * relaxed 가 아닌 writel 을 쓰는 이유는 이 설정 직후 곧바로 config/메모리 접근이
	 * 이어지므로 쓰기가 하드웨어에 확실히 도달해야 하기 때문이다. */
	writel(val, bridge_base_addr + (index * ATR_ENTRY_SIZE) +
	       ATR0_AXI4_SLV0_TRSL_ADDR_UDW);
}
EXPORT_SYMBOL_GPL(plda_pcie_setup_window);

/*
 * [한국어]
 * plda_pcie_setup_inbound_address_translation - 인바운드(PCIe -> AXI) 0번 창의 크기를 설정한다
 *
 * @port: bridge_addr 이 매핑된 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가(라고 되어 있는가): 엔드포인트가 DMA 로 보낸 PCIe 주소를 시스템 메모리
 * 주소로 되돌리는 것이 인바운드 변환이다. 이 함수는 0번 인바운드 창의 크기 필드에
 * ATR0_PCIE_ATR_SIZE(0x25)를 넣어 2^38 = 256GiB 를 덮게 하고, 소스 주소 상위 32비트를
 * 0 으로 만든다.
 *
 * 중요한 사실: 이 함수는 EXPORT_SYMBOL_GPL 로 노출되어 있으나 "이 트리 안에
 * 호출자가 한 곳도 없다"(grep 결과 0건). Microchip 은 자체 구현
 * mc_pcie_setup_inbound_atr() 을 쓰고, StarFive 는 인바운드 변환을 아예 건드리지
 * 않는다. 트리 밖에 사용자가 있는지는 이 저장소만으로는 확인할 수 없다.
 * 또 하나 눈에 띄는 점: ATR_IMPL_ENABLE(비트 0)을 세우지 않는다. 즉 창을 켜는 것이
 * 아니라 이미 켜져 있는 창의 크기 필드만 OR 로 덧씌운다. 게다가 OR 만 하고
 * 기존 크기 필드를 지우지 않으므로, 하드웨어 초기값이 0 이 아니면 의도와 다른
 * 값이 될 수 있다. 이 동작이 의도된 것인지는 데이터시트 없이 판단할 수 없어
 * 사실만 적어 둔다.
 *
 * 동작 단계:
 *  1. ATR0_PCIE_WIN0_SRCADDR_PARAM(0x600)을 읽는다.
 *  2. 0x25 를 1비트 왼쪽으로 밀어(= 0x4a) OR 한다. 크기 필드가 비트 6:1 이므로
 *     시프트 1 이 맞다.
 *  3. 되쓴다.
 *  4. ATR0_PCIE_WIN0_SRC_ADDR(0x604)에 0 을 써 소스 주소 상위 32비트를 0 으로 둔다.
 *
 * 실행 컨텍스트: (호출자가 없으므로 실제 실행되지 않지만) 설계상 probe 프로세스
 * 컨텍스트용이다. 락 없음.
 * 호출자: 이 트리에는 없음.
 * 피호출자: readl, writel.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (이 트리에서는 연결되지 않음)
 */
void plda_pcie_setup_inbound_address_translation(struct plda_pcie_rp *port)
{
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] read-modify-write 임시 변수. */
	u32 val;

	/* [한국어] 인바운드 0번 창의 소스 주소/파라미터 레지스터를 읽는다. */
	val = readl(bridge_base_addr + ATR0_PCIE_WIN0_SRCADDR_PARAM);
	/* [한국어] 크기 필드에 0x25(= 2^38 = 256GiB)를 OR 한다. 기존 크기 필드를 지우지 않고
	 * OR 만 하므로 하드웨어 초기값이 0 이 아니면 의도와 다른 값이 될 수 있다.
	 * ATR_IMPL_ENABLE(bit0)도 세우지 않는다 -- 창을 켜는 것이 아니라 이미 켜진 창의
	 * 크기만 손보는 코드다. */
	val |= (ATR0_PCIE_ATR_SIZE << ATR0_PCIE_ATR_SIZE_SHIFT);
	/* [한국어] 되쓴다. */
	writel(val, bridge_base_addr + ATR0_PCIE_WIN0_SRCADDR_PARAM);
	/* [한국어] 소스(PCIe) 주소 상위 32비트를 0 으로 만든다. 즉 인바운드 창이 PCIe 주소 0 부터
	 * 시작하게 한다. */
	writel(0, bridge_base_addr + ATR0_PCIE_WIN0_SRC_ADDR);
}
EXPORT_SYMBOL_GPL(plda_pcie_setup_inbound_address_translation);

/*
 * [한국어]
 * plda_pcie_setup_iomems - 브리지의 모든 메모리 자원 창을 ATR 테이블에 채운다
 *
 * @bridge: devicetree 의 ranges 가 이미 파싱되어 windows 리스트에 담긴 호스트 브리지.
 * @port: bridge_addr 이 매핑된 컨트롤러.
 * @return: 항상 0. 실패할 수 있는 동작이 없어 반환값이 사실상 형식적이다.
 *
 * 왜 필요한가: devicetree 의 ranges 속성은 "CPU 주소 X 부터 N 바이트가 PCIe 주소 Y 에
 * 대응한다" 를 여러 줄 적을 수 있다. PCI 코어가 그것을 bridge->windows 리스트로
 * 만들어 주면, 그 각각을 하드웨어 ATR 창으로 옮겨 심어야 실제로 접근이 된다.
 * 이 함수가 그 옮겨 심기다.
 *
 * 동작 단계:
 *  1. index 를 1 부터 시작한다 -- 0번 창은 config space 전용으로 이미 예약되어 있다.
 *  2. bridge->windows 를 순회하며 IORESOURCE_MEM 인 항목만 고른다(IORESOURCE_BUS 나
 *     IORESOURCE_IO 는 건너뛴다).
 *  3. pci_addr = res->start - entry->offset 로 PCIe 쪽 주소를 계산한다.
 *     resource_entry 의 offset 은 "CPU 주소 - 버스 주소" 이므로 빼면 버스 주소가
 *     나온다. 1:1 매핑이면 offset 이 0 이라 pci_addr == res->start 가 된다.
 *  4. plda_pcie_setup_window 로 그 창을 프로그래밍하고 index 를 올린다.
 *
 * 주의: 사용 가능한 ATR 창 개수를 검사하지 않는다. ranges 항목이 하드웨어 창 수보다
 * 많으면 존재하지 않는 오프셋에 쓰게 된다(Microchip 은 인바운드 쪽에 대해
 * MC_MAX_NUM_INBOUND_WINDOWS 검사를 두었지만 아웃바운드에는 그런 검사가 없다).
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 락 없음.
 * 호출자: plda_pcie_host_init()(StarFive 경로), mc_platform_init()(Microchip 경로).
 * 피호출자: resource_list_for_each_entry, resource_type, resource_size,
 * plda_pcie_setup_window.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   plda_pcie_host_init() -> [이 함수] -> plda_pcie_setup_window() (창마다 1회)
 */
int plda_pcie_setup_iomems(struct pci_host_bridge *bridge,
			   struct plda_pcie_rp *port)
{
	void __iomem *bridge_base_addr = port->bridge_addr;
	/* [한국어] bridge->windows 리스트를 순회할 커서. */
	struct resource_entry *entry;
	/* [한국어] 계산해 낸 PCIe 쪽 시작 주소. */
	u64 pci_addr;
	/* [한국어] 1 부터 시작한다 -- 0번 창은 config space 가 이미 차지하고 있다. */
	u32 index = 1;

	/* [한국어] devicetree 의 ranges 에서 만들어진 자원 창들을 순회한다. */
	resource_list_for_each_entry(entry, &bridge->windows) {
		/* [한국어] 메모리 창만 고른다. IORESOURCE_BUS(버스 번호 범위)나 IORESOURCE_IO 는 ATR 대상이 아니다. */
		if (resource_type(entry->res) == IORESOURCE_MEM) {
			/* [한국어] resource_entry 의 offset 은 'CPU 주소 - 버스 주소' 이므로 빼면 PCIe 버스 주소가
			 * 나온다. 1:1 매핑이면 offset 이 0 이라 두 값이 같아진다. */
			pci_addr = entry->res->start - entry->offset;
			/* [한국어] 그 창을 ATR 테이블에 프로그래밍한다. 창 개수 한계를 검사하지 않는다는 점에 주의. */
			plda_pcie_setup_window(bridge_base_addr, index,
					       entry->res->start, pci_addr,
					       resource_size(entry->res));
			/* [한국어] 다음 창 번호로 넘어간다. */
			index++;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(plda_pcie_setup_iomems);

/*
 * [한국어]
 * plda_pcie_irq_domain_deinit - 체인 핸들러를 떼고 세 irq_domain 을 제거한다
 *
 * @pcie: 도메인과 virq 가 이미 만들어져 있는 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: 드라이버가 내려갈 때 인터럽트 사슬을 안전한 순서로 해체해야 한다.
 * 순서가 중요하다 -- 핸들러를 먼저 떼야 도메인 제거 도중 인터럽트가 들어와
 * 이미 사라진 자료구조를 참조하는 일이 없다.
 *
 * 동작 단계:
 *  1. 부모 IRQ, MSI 이벤트 virq, INTx 이벤트 virq 순으로
 *     irq_set_chained_handler_and_data(irq, NULL, NULL) 를 호출해 체인 핸들러를 뗀다.
 *     부모(port->irq)를 먼저 떼는 것이 핵심이다 -- 이 시점 이후로는 새 인터럽트가
 *     아예 이 드라이버로 들어오지 않는다.
 *  2. MSI 부모 도메인을 제거한다.
 *  3. INTx 도메인, 이벤트 도메인 순으로 제거한다. 자식(INTx)을 부모(이벤트)보다
 *     먼저 지우는 방향이다.
 *
 * 실행 컨텍스트: remove 또는 probe 실패 경로의 프로세스 컨텍스트.
 * 호출자: plda_pcie_host_init() 의 err_probe 라벨, plda_pcie_host_deinit().
 * 피호출자: irq_set_chained_handler_and_data, irq_domain_remove.
 * 에러 경로: 없음 -- 되돌릴 수 없는 정리 작업이라 실패를 보고하지 않는다.
 * 참고: devm_request_irq 로 잡힌 이벤트 IRQ 들은 여기서 풀지 않는다. devm 이
 * 디바이스 해제 시 자동으로 반납하기 때문이다.
 *
 * 호출 체인:
 *   starfive_pcie_remove() -> plda_pcie_host_deinit() -> [이 함수]
 *   (실패 시) plda_pcie_host_init() -> err_probe -> [이 함수]
 */
static void plda_pcie_irq_domain_deinit(struct plda_pcie_rp *pcie)
{
	irq_set_chained_handler_and_data(pcie->irq, NULL, NULL);
	/* [한국어] MSI 이벤트 virq 의 체인 핸들러를 뗀다. */
	irq_set_chained_handler_and_data(pcie->msi_irq, NULL, NULL);
	/* [한국어] INTx 이벤트 virq 의 체인 핸들러를 뗀다. 부모를 먼저 떼었으므로 이 시점에는
	 * 이미 새 인터럽트가 들어오지 않는 상태다. */
	irq_set_chained_handler_and_data(pcie->intx_irq, NULL, NULL);

	irq_domain_remove(pcie->msi.dev_domain);

	irq_domain_remove(pcie->intx_domain);
	irq_domain_remove(pcie->event_domain);
}

/*
 * [한국어]
 * plda_pcie_host_init - 레지스터 매핑부터 PCI 버스 등록까지 한 번에 수행하는 통합 진입점
 *
 * @port: dev, num_events, events_bitmap, host_ops 가 채워진 컨트롤러.
 * @ops: 이 호스트 브리지가 쓸 pci_ops(config 읽기/쓰기/map_bus).
 *       StarFive 는 starfive_pcie_ops 를 넘긴다.
 * @plda_event: INTx/MSI 이벤트 번호와 IRQ 요청 방식.
 * @return: pci_host_probe() 의 반환값(성공 시 0). 실패 시 음수 errno.
 *
 * 왜 필요한가: StarFive 같은 드라이버가 probe 에서 해야 할 공통 작업 -- 레지스터
 * 창 두 개 매핑, 호스트 브리지 할당, SoC 초기화 훅 호출, ATR 창 설정, 인터럽트
 * 설정, PCI 코어 등록 -- 을 한 함수로 묶는다. Microchip 은 ECAM 코어가 같은 일을
 * 자기 방식으로 하기 때문에 이 함수를 쓰지 않는다.
 *
 * 동작 단계:
 *  1. dev 로부터 platform_device 를 얻는다(선언에서 한 번, 본문에서 또 한 번 --
 *     상류 코드의 중복 대입이다. 결과는 같다).
 *  2. 'apb' 이름의 리소스를 ioremap 해 bridge_addr 로 삼는다. 이 창이 있어야
 *     이 파일의 모든 레지스터 접근이 가능하다.
 *  3. 'cfg' 리소스를 얻어 config_base 로 ioremap 한다. cfg_res 는 아래 ATR 0번 창
 *     설정에서 시작 주소와 크기로 다시 쓰이므로 리소스 자체도 들고 있어야 한다.
 *  4. devm_pci_alloc_host_bridge 로 호스트 브리지를 만든다. devm 이므로 실패
 *     경로에서 따로 해제하지 않아도 된다.
 *  5. host_ops->host_init 이 있으면 호출해 SoC 쪽 전원/클럭/PHY/링크를 올린다.
 *     이 순서가 중요하다 -- 링크가 올라오기 전에 config 접근을 하면 안 되기 때문에
 *     ATR 창 설정보다 앞에 둔다.
 *  6. ATR 0번 창을 config space 로 설정한다. axi_addr 과 pci_addr 을 각각
 *     cfg_res->start 와 0 으로 주는데, config 접근에서 PCIe 쪽 "주소" 는 사실
 *     버스/장치/함수/오프셋 인코딩이므로 기준이 0 이어야 한다.
 *  7. plda_pcie_setup_iomems 로 나머지 메모리 창을 채운다.
 *  8. plda_set_default_msi 로 MSI 기본값을 넣고 plda_init_interrupts 로
 *     인터럽트 전체를 세운다. 실패하면 err_host 로 간다.
 *  9. bridge->ops 와 bridge->sysdata 를 채운다. sysdata 에 port 를 넣는 것이
 *     plda_pcie_map_bus() 가 bus->sysdata 로 컨트롤러를 되찾는 근거다.
 * 10. pci_host_probe 로 커널 PCI 코어에 넘긴다. 이 안에서 버스 스캔과 자원 할당,
 *     장치 드라이버 바인딩이 일어난다. 실패하면 err_probe.
 *
 * 실행 컨텍스트: probe 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: 이 트리에서는 starfive_pcie_probe() 하나뿐이다.
 * 피호출자: devm_platform_ioremap_resource_byname, platform_get_resource_byname,
 * devm_ioremap_resource, devm_pci_alloc_host_bridge, host_ops->host_init,
 * plda_pcie_setup_window, plda_pcie_setup_iomems, plda_set_default_msi,
 * plda_init_interrupts, pci_host_probe.
 * 에러 경로: 두 라벨로 갈린다. err_probe 는 인터럽트까지 성공한 뒤 실패한
 * 경우라 도메인 해체(plda_pcie_irq_domain_deinit)를 먼저 하고 host_deinit 으로
 * 떨어진다. err_host 는 인터럽트 설정 실패라 도메인 해체를 건너뛰고 곧장
 * host_deinit 만 부른다. 그 앞의 실패들은 devm_ 자원만 잡힌 상태라 그냥 반환한다.
 *
 * 호출 체인:
 *   starfive_pcie_probe() -> [이 함수] -> starfive_pcie_host_init()
 *     -> plda_init_interrupts() -> pci_host_probe() -> (버스 스캔)
 */
int plda_pcie_host_init(struct plda_pcie_rp *port, struct pci_ops *ops,
			const struct plda_event *plda_event)
{
	struct device *dev = port->dev;
	/* [한국어] 커널 PCI 코어에 넘길 호스트 브리지 객체. */
	struct pci_host_bridge *bridge;
	/* [한국어] device 로부터 platform_device 를 얻는다. 아래에서 한 번 더 같은 대입이
	 * 반복되는데(상류 코드의 중복), 결과는 동일하다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 'cfg' 리소스. ioremap 뿐 아니라 ATR 0번 창의 시작 주소와 크기로도 다시 쓰이므로
	 * 리소스 구조체 자체를 들고 있어야 한다. */
	struct resource *cfg_res;
	/* [한국어] 에러 코드 임시 변수. */
	int ret;

	/* [한국어] 위 선언에서 이미 한 것과 같은 대입. 상류 코드에 남아 있는 중복이다. */
	pdev = to_platform_device(dev);

	/* [한국어] 'apb' 이름의 리소스를 ioremap 해 브리지 레지스터 창을 연다.
	 * 이 창이 없으면 이 파일의 모든 레지스터 접근이 불가능하므로 가장 먼저 한다.
	 * devm_ 판이라 실패 경로에서 따로 해제할 필요가 없다. */
	port->bridge_addr =
		devm_platform_ioremap_resource_byname(pdev, "apb");

	/* [한국어] ioremap 실패 검사. devm_platform_ioremap_resource_byname 은 NULL 이 아니라
	 * ERR_PTR 을 돌려주므로 IS_ERR 로 봐야 한다. */
	if (IS_ERR(port->bridge_addr))
		/* [한국어] -EPROBE_DEFER 면 조용히, 그 외에는 에러로 남기고 그대로 반환한다. */
		return dev_err_probe(dev, PTR_ERR(port->bridge_addr),
				     "failed to map reg memory\n");

	/* [한국어] 'cfg' 리소스를 얻는다. ioremap 과 분리한 것은 아래에서 start/size 를 써야 하기 때문. */
	cfg_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	/* [한국어] 리소스가 없으면 devicetree 가 불완전한 것이다. */
	if (!cfg_res)
		/* [한국어] -ENODEV 로 probe 실패. */
		return dev_err_probe(dev, -ENODEV,
				     "failed to get config memory\n");

	/* [한국어] config space 창을 매핑한다. 이 주소가 plda_pcie_map_bus 의 기준이 된다. */
	port->config_base = devm_ioremap_resource(dev, cfg_res);
	/* [한국어] 매핑 실패 검사. */
	if (IS_ERR(port->config_base))
		/* [한국어] 실패 원인을 남기고 반환. */
		return dev_err_probe(dev, PTR_ERR(port->config_base),
				     "failed to map config memory\n");

	/* [한국어] 호스트 브리지를 할당한다. 두 번째 인자 0 은 드라이버 전용 여분 공간 크기로,
	 * 이 드라이버는 sysdata 에 port 포인터만 넣으므로 추가 공간이 필요 없다. */
	bridge = devm_pci_alloc_host_bridge(dev, 0);
	/* [한국어] 할당 실패는 메모리 부족. */
	if (!bridge)
		return -ENOMEM;

	/* [한국어] SoC 초기화 훅이 있으면 -- host_ops 자체와 그 안의 함수 포인터를 두 겹으로 검사한다. */
	if (port->host_ops && port->host_ops->host_init) {
		/* [한국어] SoC 전원/클럭/리셋/PHY 를 올리고 링크를 기다린다.
		 * 이 호출이 ATR 창 설정보다 앞에 있는 것이 중요하다 -- 링크가 올라오기 전에
		 * config 접근을 하면 안 되기 때문이다. */
		ret = port->host_ops->host_init(port);
		/* [한국어] SoC 초기화 실패는 그대로 반환한다. 아직 인터럽트를 세우지 않았으므로
		 * err_host 라벨로 갈 필요가 없다(host_deinit 을 부르지 않는 것은 host_init 이
		 * 자기 실패 경로를 이미 정리했다고 보기 때문이다). */
		if (ret)
			return ret;
	}

	/* [한국어] 브리지 포인터를 컨트롤러에 보관한다. plda_pcie_host_deinit 이 여기서 bus 를 찾는다. */
	port->bridge = bridge;
	/* [한국어] ATR 0번 창을 config space 로 설정한다. axi_addr 은 cfg 리소스의 시작 주소,
	 * pci_addr 은 0 이다 -- config 접근에서 PCIe 쪽 '주소' 는 실제로는
	 * 버스/장치/함수/오프셋 인코딩이므로 기준이 0 이어야 한다. */
	plda_pcie_setup_window(port->bridge_addr, 0, cfg_res->start, 0,
			       resource_size(cfg_res));
	/* [한국어] 나머지 메모리 창(index 1 이상)을 채운다. 반환값을 보지 않는데, 이 함수는
	 * 항상 0 을 돌려주므로 실질적인 차이는 없다. */
	plda_pcie_setup_iomems(bridge, port);
	plda_set_default_msi(&port->msi);
	/* [한국어] 이벤트/INTx/MSI 인터럽트 전체를 세운다. 이 줄이 끝나면 인터럽트가 들어오기
	 * 시작하므로, PCI 버스 스캔보다 반드시 먼저여야 한다. */
	ret = plda_init_interrupts(pdev, port, plda_event);
	/* [한국어] 인터럽트 설정 실패. */
	if (ret)
		goto err_host;

	/* Set default bus ops */
	bridge->ops = ops;
	/* [한국어] sysdata 에 컨트롤러를 심는다. plda_pcie_map_bus 가 bus->sysdata 로 이것을
	 * 되찾는 것이 config 접근의 출발점이다. */
	bridge->sysdata = port;

	/* [한국어] 커널 PCI 코어에 브리지를 넘긴다. 이 안에서 버스 스캔, BAR 자원 할당,
	 * 장치 드라이버 바인딩이 모두 일어난다 -- 즉 이 줄에서 실제 PCIe 장치들이
	 * 발견되고 동작을 시작한다. */
	ret = pci_host_probe(bridge);
	/* [한국어] 스캔/등록 실패. */
	if (ret < 0) {
		/* [한국어] 원인을 남기고 err_probe 로 간다. 여기서는 인터럽트까지 이미 세워졌으므로
		 * 도메인 해체가 필요하다. */
		dev_err_probe(dev, ret, "failed to probe pci host\n");
		goto err_probe;
	}

	return ret;

err_probe:
	plda_pcie_irq_domain_deinit(port);
err_host:
	if (port->host_ops && port->host_ops->host_deinit)
		port->host_ops->host_deinit(port);

	return ret;
}
EXPORT_SYMBOL_GPL(plda_pcie_host_init);

/*
 * [한국어]
 * plda_pcie_host_deinit - PCI 버스를 떼고 인터럽트를 해체한 뒤 SoC 전원을 내린다
 *
 * @port: plda_pcie_host_init() 으로 올라와 있던 컨트롤러.
 * @return: 없음.
 *
 * 왜 필요한가: plda_pcie_host_init() 의 정확한 역순 해체다. 순서를 지키지 않으면
 * 아직 살아 있는 장치 드라이버가 사라진 레지스터 창에 접근하게 된다.
 *
 * 동작 단계:
 *  1. pci_stop_root_bus -- 이 버스 아래 모든 장치 드라이버를 언바인드해 더 이상
 *     하드웨어를 건드리지 않게 만든다. 반드시 첫 단계여야 한다.
 *  2. pci_remove_root_bus -- 장치 객체와 버스 자체를 제거한다.
 *  3. plda_pcie_irq_domain_deinit -- 체인 핸들러를 떼고 도메인 셋을 지운다.
 *     장치가 다 사라진 뒤이므로 이제 인터럽트가 올 일이 없다.
 *  4. host_ops->host_deinit 이 있으면 호출해 PHY/클럭/리셋/레귤레이터를 내린다.
 *     레지스터 창(bridge_addr, config_base)은 devm 이 자동으로 해제한다.
 *
 * 실행 컨텍스트: remove 프로세스 컨텍스트. 잠들 수 있다.
 * 호출자: 이 트리에서는 starfive_pcie_remove() 하나뿐이다.
 * 피호출자: pci_stop_root_bus, pci_remove_root_bus, plda_pcie_irq_domain_deinit,
 * host_ops->host_deinit.
 * 에러 경로: 없음 -- 해체는 실패를 보고할 수 없다.
 *
 * 호출 체인:
 *   starfive_pcie_remove() -> [이 함수] -> starfive_pcie_host_deinit()
 */
void plda_pcie_host_deinit(struct plda_pcie_rp *port)
{
	pci_stop_root_bus(port->bridge->bus);
	pci_remove_root_bus(port->bridge->bus);

	plda_pcie_irq_domain_deinit(port);

	/* [한국어] SoC 해제 훅이 있으면 호출해 PHY/클럭/리셋/레귤레이터를 내린다.
	 * 레지스터 창은 devm 이 자동 해제하므로 여기서 다루지 않는다. */
	if (port->host_ops && port->host_ops->host_deinit)
		port->host_ops->host_deinit(port);
}
EXPORT_SYMBOL_GPL(plda_pcie_host_deinit);
