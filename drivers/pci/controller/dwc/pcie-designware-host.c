// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare PCIe host controller driver
 *
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 */

/*
 * [한국어 설명] DesignWare IP 를 루트 컴플렉스로 쓸 때 (pcie-designware-host.c)
 *
 * === 파일의 역할 ===
 * 같은 DesignWare IP 를 호스트(RC)로도 엔드포인트(EP)로도 쓸 수 있다.
 * 이 파일은 호스트 쪽 절반이다 — 버스를 만들고, config 접근을 제공하고,
 * 하위 장치의 인터럽트를 받는다.
 *
 * 이 파일에서 가장 특이한 부분이 MSI 처리다. 보통의 PCIe 호스트는 장치가
 * 보낸 MSI 쓰기가 그대로 인터럽트 컨트롤러에 도달하지만, DesignWare 는
 * IP 안에 자체 MSI 수신기를 두었다. 장치들이 IP 가 정한 한 주소로 MSI 를
 * 쓰면, IP 가 그것을 모아 하나의 인터럽트 선으로 CPU 에 알린다.
 *
 * 그래서 이 파일은 chained IRQ 구조를 만든다.
 *   장치의 MSI 쓰기 -> IP 의 MSI 수신기 -> 하나의 부모 인터럽트
 *     -> dw_chained_msi_isr() 가 어느 벡터인지 레지스터로 확인
 *        -> 해당 가상 IRQ 로 분배
 * 이 구조 때문에 MSI 벡터 개수가 IP 구성에 묶인다. 보통 32~256 개다.
 *
 * config 접근도 눈여겨볼 만하다. 버스 0(루트 포트 자신)은 DBI 로 직접
 * 접근하지만, 그 아래 장치는 iATU 창을 config 타입으로 설정한 뒤
 * 그 창을 통해 읽고 쓴다. 창이 하나뿐이면 접근할 때마다 재설정해야 해서,
 * 그 부분에 잠금이 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SoC 별 드라이버의 probe
 *   -> dw_pcie_host_init() [이 파일]
 *      -> MSI 도메인 구성, iATU 창 배치
 *      -> pcie-designware.c 의 링크 대기
 *      -> pci_host_probe() -> PCI 코어의 열거
 *         -> 발견된 NVMe 등에 드라이버 바인딩
 *
 * 실행 컨텍스트: 초기화는 프로세스 컨텍스트. config 접근은 잠금 아래.
 * MSI 분배는 하드 IRQ 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: dwc/ 의 SoC 별 드라이버들.
 * 아래쪽: pcie-designware.c 의 공통 코어, 커널 IRQ 도메인 계층,
 *   그리고 PCI 코어의 열거(probe.c).
 * 공유 상태: struct dw_pcie_rp — 루트 포트 정보와 MSI 상태.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 실무에서 부딪히는 지점은 MSI 벡터 수다. NVMe 는 CPU 코어마다 큐를
 * 하나씩 두고 싶어 하므로 벡터를 많이 요청하는데, DesignWare 의 자체 MSI
 * 컨트롤러는 그 수가 IP 합성 시점에 정해져 있다. 요청보다 적으면
 * pci_alloc_irq_vectors() 가 줄여서 돌려주고, NVMe 는 그만큼 큐를 줄인다.
 * ARM SoC 에서 NVMe 큐 수가 코어 수보다 적게 잡히는 이유가 대개 이것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * dw_pcie_host_init()     : 호스트 초기화 전체. 이 파일의 입구.
 * dw_pcie_host_deinit()   : 그 반대.
 * dw_pcie_setup_rc()      : 루트 포트 자신의 config 를 설정한다.
 * dw_pcie_rd_other_conf() / dw_pcie_wr_other_conf() : 하위 장치 config 접근.
 *                           iATU 창을 config 타입으로 바꿔 쓴다.
 * dw_pcie_own_conf_map_bus() : 버스 0(루트 포트 자신) 접근을 DBI 로 돌린다.
 * dw_pcie_other_conf_map_bus() : 그 아래 버스는 iATU 창을 거치게 한다.
 * dw_pcie_ecam_conf_map_bus() / dw_pcie_create_ecam_window() : ECAM 을 쓸 수
 *                           있는 구성에서는 창 재설정 없이 접근할 수 있다.
 * dw_chained_msi_isr() / dw_handle_msi_irq() : MSI 수신과 분배.
 *                           앞의 것이 부모 인터럽트를 받아 뒤의 것을 부른다.
 * dw_pcie_msi_init()      : IP 의 MSI 수신 주소를 설정한다.
 * dw_pcie_allocate_domains() : MSI IRQ 도메인 생성.
 * struct dw_pcie_rp       : 루트 포트 상태. MSI 비트맵이 여기 있다.
 */

#include <linux/align.h>			/* PCI/NVMe: 정렬 매크로, NVMe BAR/queue 정렬 시 사용 */
#include <linux/iopoll.h>			/* PCI/NVMe: 레지스터 폴링, NVMe 링크 업/다운 감지 시 사용 */
#include <linux/irqchip/chained_irq.h>	/* PCI/NVMe: 연결형 IRQ 진입, NVMe MSI-X/MSI 경로에 연결 */
#include <linux/irqchip/irq-msi-lib.h>	/* PCI/NVMe: MSI 라이브러리, NVMe 장치의 MSI 메시지 처리 지원 */
#include <linux/irqdomain.h>		/* PCI/NVMe: IRQ 도메인, NVMe MSI/MSI-X 벡터를 Linux IRQ에 매핑 */
#include <linux/msi.h>			/* PCI/NVMe: MSI/MSI-X API, NVMe가 요구하는 MSI-X 설정에 사용 */
#include <linux/of_address.h>		/* PCI/NVMe: DT 주소 파싱, NVMe 컨트롤러의 메모리 영역 확보 */
#include <linux/of_pci.h>		/* PCI/NVMe: DT PCIe 파싱, NVMe 장치 열거에 필요 */
#include <linux/pci_regs.h>		/* PCI/NVMe: PCI 레지스터 정의, NVMe PCIe capability 접근 */
#include <linux/platform_device.h>	/* PCI/NVMe: 플랫폼 디바이스, NVMe PCIe 호스트의 리소스 등록 */

#include "../../pci.h"			/* PCI/NVMe: PCI 핵심 헤더, NVMe 열거/바인딩 내포 */
#include "pcie-designware.h"		/* PCI/NVMe: DesignWare PCIe 정의, NVMe 호스트와의 연결점 */

static struct pci_ops dw_pcie_ops;		/* PCI/NVMe: RC 자체 config 접근 ops, NVMe 루트 포트 설정에 사용 */
static struct pci_ops dw_pcie_ecam_ops;		/* PCI/NVMe: ECAM config 접근 ops, NVMe 버스 스캔 시 사용 */
static struct pci_ops dw_child_pcie_ops;	/* PCI/NVMe: 하위 버스 config 접근 ops, NVMe 장치 탐색 시 사용 */

#ifdef CONFIG_SMP
/* [한국어]
 * dw_irq_noop - 아무 일도 하지 않는 irq_ack 자리채우기 (SMP 전용)
 *
 * @d: 대상 인터럽트의 irq_data. 쓰지 않는다.
 * @return: 없음.
 *
 * SMP 빌드에서만 존재하는 이유가 핵심이다. 이 컨트롤러의 MSI 상태 비트를
 * 지우는 실제 동작은 dw_pci_bottom_ack() 인데, SMP 에서는 그것을 irq_ack 가
 * 아니라 irq_pre_redirect 자리에 건다(dw_pci_msi_bottom_irq_chip 참조).
 * affinity 를 바꿀 때 인터럽트가 옛 CPU 와 새 CPU 양쪽에 걸쳐 뜨는 구간이
 * 있어, 상태 비트 소거를 재지향(redirect) 시점과 맞물리게 해야 하기 때문이다.
 * 그러면 irq_ack 자리가 비는데, irq 코어는 handle_edge_irq 경로에서 irq_ack 를
 * 무조건 부르므로 NULL 을 둘 수 없다. 그래서 빈 함수를 채워 넣는다.
 *
 * 비-SMP 빌드에서는 재지향이 없으므로 irq_ack 에 dw_pci_bottom_ack 이 그대로
 * 들어가고, 이 함수는 #ifdef 로 아예 컴파일되지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥(하드 IRQ).
 *
 * 호출 체인:
 *   handle_edge_irq → chip->irq_ack → [이 함수]
 */
static void dw_irq_noop(struct irq_data *d) { }	/* NVMe: SMP 시 irq_ack no-op, NVMe MSI affinity 이동 시 불필요한 ack 방지 */
#endif

/* [한국어]
 * dw_pcie_init_dev_msi_info - 자식 MSI 도메인을 만들 때 irq_chip 을 이 IP 에 맞게 손본다
 *
 * @dev: MSI 를 요청한 PCI 디바이스.
 * @domain: 새로 만들어지는 자식(디바이스별) MSI 도메인.
 * @real_parent: 실제 부모 도메인 — 여기서는 pp->irq_domain.
 * @info: 자식 도메인의 msi_domain_info. 이 함수가 info->chip 을 고쳐 쓴다.
 * @return: true 면 도메인 생성을 계속 진행, false 면 실패로 접는다.
 *
 * 커널의 MSI 부모/자식 도메인 구조에서, 부모(DWC iMSI-RX)는 도메인을 하나만
 * 만들어 두고 디바이스가 MSI 를 요청할 때마다 자식 도메인이 파생된다. 이
 * 콜백은 그 파생 순간에 불려, 공통 라이브러리가 채운 기본값 위에 이 IP 만의
 * 차이를 덧씌우는 자리다.
 *
 * 먼저 msi_lib_init_dev_msi_info() 로 irq-msi-lib 의 표준 초기화를 돌린다.
 * 실패하면 그대로 false 를 돌려 준다 — 덧씌울 대상 자체가 없기 때문이다.
 *
 * 그 위에 얹는 차이는 ack 의 위치다:
 *   - CONFIG_SMP: irq_ack 는 무해한 dw_irq_noop, 진짜 상태 소거는
 *     irq_pre_redirect(= irq_chip_pre_redirect_parent → 부모의
 *     dw_pci_bottom_ack)로 미룬다. affinity 변경 중 인터럽트 유실을 막기 위해서다.
 *   - 비-SMP: 재지향이 없으므로 irq_ack 를 그냥 부모로 넘긴다
 *     (irq_chip_ack_parent).
 *
 * 실행 컨텍스트: 디바이스가 pci_alloc_irq_vectors 등으로 MSI 를 요청하는
 * 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_create_device_irq_domain → parent_ops->init_dev_msi_info
 *     → [이 함수] → msi_lib_init_dev_msi_info
 */
static bool dw_pcie_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				      struct irq_domain *real_parent, struct msi_domain_info *info)
{
	/* PCI/NVMe: MSI 도메인 정보 초기화, NVMe 장치의 MSI/MSI-X 할당 전 설정 */
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		return false;	/* NVMe: MSI 라이브러리 초기화 실패 시 NVMe MSI 할당 불가 */

#ifdef CONFIG_SMP
	info->chip->irq_ack = dw_irq_noop;	/* NVMe: SMP에서는 ack 무시, NVMe IRQ migration 중 이중 ack 방지 */
	info->chip->irq_pre_redirect = irq_chip_pre_redirect_parent;	/* NVMe: affinity 변경 전 parent ack, NVMe ISR 안정성 향상 */
#else
	info->chip->irq_ack = irq_chip_ack_parent;	/* NVMe: UP에서는 parent ack, NVMe MSI 수신 완료 처리 */
#endif
	return true;	/* NVMe: MSI chip 준비 완료, NVMe 장치에 MSI 메시지 전달 가능 */
}

#define DW_PCIE_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS		| \
				    MSI_FLAG_USE_DEF_CHIP_OPS		| \
				    MSI_FLAG_PCI_MSI_MASK_PARENT)	/* PCI/NVMe: 필수 MSI 플래그, NVMe MSI-X 사용 시 parent masking 지원 */
#define DW_PCIE_MSI_FLAGS_SUPPORTED (MSI_FLAG_MULTI_PCI_MSI		| \
				     MSI_FLAG_PCI_MSIX			| \
				     MSI_GENERIC_FLAGS_MASK)	/* PCI/NVMe: 지원 MSI 플래그, NVMe가 필요로 하는 다중 MSI/MSI-X 지원 */

#define IS_256MB_ALIGNED(x) IS_ALIGNED(x, SZ_256M)	/* PCI/NVMe: ECAM 256MB 정렬 검사, NVMe 장치의 256MB bus 단위 접근에 사용 */

static const struct msi_parent_ops dw_pcie_msi_parent_ops = {
	.required_flags		= DW_PCIE_MSI_FLAGS_REQUIRED,	/* NVMe: 필수 MSI 플래그, NVMe MSI 메시지 처리 보장 */
	.supported_flags	= DW_PCIE_MSI_FLAGS_SUPPORTED,	/* NVMe: NVMe MSI/MSI-X 기능 지원 선언 */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI,		/* NVMe: PCI MSI 버스 선택, NVMe MSI 도메인과 연결 */
	.prefix			= "DW-",			/* NVMe: IRQ 이름 prefix, NVMe MSI IRQ 식별에 도움 */
	.init_dev_msi_info	= dw_pcie_init_dev_msi_info,	/* NVMe: NVMe 장치별 MSI info 초기화 콜백 */
};

/* MSI int handler */
/* [한국어]
 * dw_handle_msi_irq - iMSI-RX 상태 레지스터를 훑어 걸린 MSI 를 커널로 올린다
 *
 * @pp: 이 루트 포트. 컨트롤러 레지스터와 IRQ 도메인이 여기 달려 있다.
 * @return: 없음.
 *
 * DWC 의 iMSI-RX 모듈은 호스트 메모리로 향하는 MSI 쓰기(MWr TLP)를 가로채
 * AXI 로 내보내지 않고 자기 상태 레지스터의 비트로 바꿔 둔다. 이 함수가 그
 * 비트들을 읽어 커널 IRQ 로 되돌리는 다리다.
 *
 * 벡터는 32개(MAX_MSI_IRQS_PER_CTRL)씩 묶여 "컨트롤 블록" 단위로 관리된다.
 * 블록 i 의 레지스터는 PCIE_MSI_INTR0_* 에서 i * MSI_REG_CTRL_BLOCK_SIZE(=12)
 * 바이트만큼 떨어져 있다 — 한 블록이 ENABLE/MASK/STATUS 세 개의 32비트
 * 레지스터를 쓰기 때문에 12바이트다. 그래서 num_vectors 를 32로 나눈 개수만큼
 * 블록을 돌며 STATUS 를 읽고, 세워진 비트마다
 * generic_handle_demux_domain_irq() 로 hwirq(블록번호*32 + 비트) 를 올린다.
 *
 * status 가 0 인 블록은 곧바로 건너뛴다 — 대부분의 인터럽트에서 한두 블록만
 * 세워지므로 이 조기 탈출이 실질적인 비용을 줄인다.
 *
 * 여기서 상태 비트를 지우지 않는 점에 유의. 소거는 irq 코어가 각 벡터를
 * 처리하며 부르는 dw_pci_bottom_ack() 이 맡는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 이 파일의 dw_chained_msi_isr 이 부르지만,
 * EXPORT 되어 있어 자체 ISR 을 가진 개별 SoC 드라이버도 직접 부른다.
 *
 * 호출 체인:
 *   dw_chained_msi_isr(또는 SoC 드라이버 ISR) → [이 함수]
 *     → generic_handle_demux_domain_irq → 디바이스 드라이버 핸들러
 */
void dw_handle_msi_irq(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare MSI 인터럽트 핸들러, NVMe SSD가 발생시킨 MSI 메시지 처리 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie 구조체 획득, NVMe 호스트 레지스터 접근 기반 */
	unsigned int i, num_ctrls;			/* NVMe: MSI 컨트롤러 인덱스, NVMe MSI 벡터 그룹 관리 */

	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 벡터당 컨트롤러 수, NVMe MSI-X 다중 벡터 지원 규모 계산 */

	for (i = 0; i < num_ctrls; i++) {	/* NVMe: 각 MSI 컨트롤러 순회, NVMe 다중 큐 MSI 벡터 처리 */
		unsigned int reg_off = i * MSI_REG_CTRL_BLOCK_SIZE;	/* NVMe: 상태 레지스터 오프셋, NVMe MSI 상태 확인 위치 */
		unsigned int irq_off = i * MAX_MSI_IRQS_PER_CTRL;	/* NVMe: IRQ 번호 오프셋, NVMe 큐별 IRQ 번호 계산 */
		unsigned long status, pos;				/* NVMe: MSI 상태/비트 위치, NVMe 활성 큐 식별 */

		status = dw_pcie_readl_dbi(pci, PCIE_MSI_INTR0_STATUS + reg_off);	/* NVMe: MSI 상태 레지스터 읽기, NVMe에서 복귀한 MSI TLP 감지 */
		if (!status)
			continue;	/* NVMe: 활성 MSI 없으면 다음 컨트롤러, NVMe 다른 큐 확인 */

		for_each_set_bit(pos, &status, MAX_MSI_IRQS_PER_CTRL)	/* NVMe: 활성 비트 순회, NVMe submission/completion 큐별 IRQ 분배 */
			generic_handle_demux_domain_irq(pp->irq_domain, irq_off + pos);	/* NVMe: Linux IRQ로 디먹스, NVMe 큐 ISR(nvme_irq) 호출 */
	}
}

/* Chained MSI interrupt service routine */
/* [한국어]
 * dw_chained_msi_isr - MSI 부모 IRQ 에 걸리는 연쇄(chained) 핸들러
 *
 * @desc: 부모 IRQ 의 irq_desc. 핸들러 데이터로 dw_pcie_rp 가 매달려 있다.
 * @return: 없음.
 *
 * "연쇄 핸들러" 는 하나의 상위 인터럽트 선을 여러 개의 하위 인터럽트로
 * 분해하는 형태다. SoC 는 MSI 그룹 하나당 GIC 선 하나를 주고, 그 선이
 * 울리면 이 함수가 상태 레지스터를 읽어 실제 벡터로 나눠 준다.
 *
 * chained_irq_enter/exit 로 감싸는 이유: 부모 인터럽트 컨트롤러(GIC 등)에
 * 따라 진입 시 마스크나 EOI 처리가 필요한데, 그 차이를 이 두 헬퍼가 흡수한다.
 * 이 쌍을 빠뜨리면 부모 선이 계속 울리거나 반대로 다시 울리지 않는다.
 *
 * irq_desc_get_handler_data() 로 pp 를 되찾는데, 이 값은
 * dw_pcie_msi_host_init() 이 irq_set_chained_handler_and_data() 로 심어 둔 것이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   GIC → 부모 irq 핸들러 → [이 함수] → dw_handle_msi_irq
 */
static void dw_chained_msi_isr(struct irq_desc *desc)
{
	/* NVMe: 연결형 MSI ISR, NVMe MSI 하드웨어 IRQ에서 상위 IRQ로 연결 */
	struct irq_chip *chip = irq_desc_get_chip(desc);	/* NVMe: 상위 IRQ chip 획득, NVMe MSI 호스트 IRQ 제어 */
	struct dw_pcie_rp *pp;					/* NVMe: DesignWare RC 포트, NVMe 연결된 루트 포트 */

	chained_irq_enter(chip, desc);	/* NVMe: 연결형 IRQ 진입, NVMe MSI 처리 중 상위 mask 보장 */

	pp = irq_desc_get_handler_data(desc);	/* NVMe: 핸들러 데이터에서 RC 포트 획득, NVMe MSI 소속 포트 식별 */
	dw_handle_msi_irq(pp);			/* NVMe: 실제 MSI 처리, NVMe 큐별 인터럽트 디스패치 */

	chained_irq_exit(chip, desc);	/* NVMe: 연결형 IRQ 종료, NVMe MSI 처리 후 상위 unmask */
}

/* [한국어]
 * dw_pci_setup_msi_msg - 디바이스가 써야 할 MSI 주소/데이터를 조립한다
 *
 * @d: 이 벡터의 irq_data. chip_data 에 dw_pcie_rp 가, hwirq 에 벡터 번호가 있다.
 * @msg: 채워 넣을 MSI 메시지. 커널이 이 값을 디바이스 설정공간의
 *       MSI/MSI-X 능력 구조에 기록한다.
 * @return: 없음.
 *
 * MSI 는 결국 "정해진 주소에 정해진 값을 쓰는 메모리 쓰기" 다. 그래서 이
 * 함수는 주소로 pp->msi_data(iMSI-RX 가 가로챌 목적지)를, 데이터로 hwirq
 * (벡터 번호)를 넣는다. 디바이스가 그 주소에 그 값을 쓰면, iMSI-RX 가
 * TLP 를 잡아 해당 번호의 상태 비트를 세운다.
 *
 * 주소를 lo/hi 로 쪼개는 것은 MSI 능력 구조가 32비트 두 칸으로 주소를 담기
 * 때문이다. pp->msi_data 는 dw_pcie_msi_host_init() 이 정하는데, 64비트
 * 메시지를 못 쓰는 주변 장치를 위해 되도록 4GB 아래로 잡는다.
 *
 * 실행 컨텍스트: 디바이스가 MSI 벡터를 할당받는 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_msi 코어 → chip->irq_compose_msi_msg → [이 함수]
 */
static void dw_pci_setup_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	/* NVMe: MSI 메시지 구성, NVMe 장치가 MSI 메모리 쓰기할 주소/데이터 설정 */
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);	/* NVMe: IRQ chip 데이터에서 RC 포트, NVMe MSI 소속 확인 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);		/* NVMe: dw_pcie 획득, NVMe 호스트 레지스터 접근 */
	u64 msi_target = (u64)pp->msi_data;			/* NVMe: MSI 타겟 주소, NVMe가 쓸 메모리 주소 */

	msg->address_lo = lower_32_bits(msi_target);	/* NVMe: MSI 주소 하위 32비트, 32-bit NVMe 장치 전달 */
	msg->address_hi = upper_32_bits(msi_target);	/* NVMe: MSI 주소 상위 32비트, 64-bit NVMe 장치 전달 */
	msg->data = d->hwirq;				/* NVMe: MSI 데이터, NVMe 큐별 벡터 번호 */

	dev_dbg(pci->dev, "msi#%d address_hi %#x address_lo %#x\n",
		(int)d->hwirq, msg->address_hi, msg->address_lo);	/* NVMe: 디버그 로그, NVMe MSI 메시지 트레이스 */
}

/* [한국어]
 * dw_pci_bottom_mask - 벡터 하나를 iMSI-RX 마스크 레지스터에서 막는다
 *
 * @d: 막을 벡터의 irq_data. hwirq 가 전역 벡터 번호다.
 * @return: 없음.
 *
 * hwirq 를 블록 번호(ctrl)와 블록 내 비트로 나눈다. 벡터는 32개씩 묶이므로
 * ctrl = hwirq / 32, bit = hwirq % 32 이고, 블록 i 의 레지스터 오프셋은
 * i * MSI_REG_CTRL_BLOCK_SIZE(=12) 다.
 *
 * 하드웨어 MASK 레지스터는 읽고-고치고-쓰기를 해야 하는데, 그러면 이웃 벡터의
 * 마스크와 경쟁이 생긴다. 그래서 pp->irq_mask[] 에 소프트웨어 사본을 두고
 * 그것을 고친 뒤 통째로 써 넣는다 -- 읽기 단계가 사라져 레지스터 왕복이 준다.
 * 사본과 레지스터가 어긋나지 않도록 pp->lock(raw_spinlock)으로 감싼다.
 * raw 스핀락인 이유는 이 경로가 인터럽트 문맥에서도 불리고 PREEMPT_RT 에서도
 * 잠들면 안 되기 때문이다. guard() 매크로가 함수 반환 시 자동 해제한다.
 *
 * 실행 컨텍스트: disable_irq 계열의 프로세스 문맥과 인터럽트 문맥 양쪽.
 *
 * 호출 체인:
 *   irq 코어(mask) → chip->irq_mask → [이 함수] → dw_pcie_writel_dbi
 */
static void dw_pci_bottom_mask(struct irq_data *d)
{
	/* NVMe: MSI 벡터 마스크, NVMe 큐 ISR 일시 비활성화(예: MSI-X per-vector mask) */
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);	/* NVMe: RC 포트, NVMe MSI 소속 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);		/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	unsigned int res, bit, ctrl;				/* NVMe: 레지스터/비트/컨트롤러, NVMe 벡터 위치 */

	guard(raw_spinlock)(&pp->lock);	/* NVMe: RC 포트 lock, NVMe MSI mask 레지스터 동시 접근 보호 */
	ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 번호, NVMe 벡터가 속한 MSI 그룹 */
	res = ctrl * MSI_REG_CTRL_BLOCK_SIZE;		/* NVMe: MASK 레지스터 오프셋, NVMe 벡터 마스크 위치 */
	bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;		/* NVMe: 비트 위치, NVMe 큐에 대응하는 MSI 벡터 */

	pp->irq_mask[ctrl] |= BIT(bit);	/* NVMe: 소프트웨어 마스크 비트 설정, NVMe 해당 큐 MSI 차단 */
	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK + res, pp->irq_mask[ctrl]);	/* NVMe: 하드웨어 마스크 레지스터 갱신, NVMe MSI 수신 차단 */
}

/* [한국어]
 * dw_pci_bottom_unmask - 벡터 하나의 마스크를 풀어 다시 받게 한다
 *
 * @d: 풀 벡터의 irq_data.
 * @return: 없음.
 *
 * dw_pci_bottom_mask 의 정확한 반대다. 같은 방식으로 블록/비트를 계산하고,
 * 같은 pp->lock 아래에서 소프트웨어 사본의 해당 비트를 지운 뒤 MASK
 * 레지스터에 통째로 써 넣는다.
 *
 * 마스크 사본이 하나뿐이므로 mask/unmask 가 같은 잠금을 공유해야 한다 --
 * 두 경로가 서로 다른 잠금을 쓰면 read-modify-write 가 겹쳐 한쪽의 변경이
 * 사라진다.
 *
 * 실행 컨텍스트: enable_irq 계열의 프로세스 문맥과 인터럽트 문맥 양쪽.
 *
 * 호출 체인:
 *   irq 코어(unmask) → chip->irq_unmask → [이 함수] → dw_pcie_writel_dbi
 */
static void dw_pci_bottom_unmask(struct irq_data *d)
{
	/* NVMe: MSI 벡터 언마스크, NVMe 큐 ISR 다시 활성화 */
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);	/* NVMe: RC 포트, NVMe MSI 소속 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);		/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	unsigned int res, bit, ctrl;				/* NVMe: 레지스터/비트/컨트롤러, NVMe 벡터 위치 */

	guard(raw_spinlock)(&pp->lock);	/* NVMe: RC 포트 lock, NVMe MSI unmask 동시 접근 보호 */
	ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 번호, NVMe 벡터 그룹 */
	res = ctrl * MSI_REG_CTRL_BLOCK_SIZE;		/* NVMe: MASK 레지스터 오프셋, NVMe 벡터 언마스크 위치 */
	bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;		/* NVMe: 비트 위치, NVMe 큐 대응 벡터 */

	pp->irq_mask[ctrl] &= ~BIT(bit);	/* NVMe: 소프트웨어 마스크 비트 해제, NVMe 해당 큐 MSI 허용 */
	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK + res, pp->irq_mask[ctrl]);	/* NVMe: 하드웨어 마스크 레지스터 갱신, NVMe MSI 수신 허용 */
}

/* [한국어]
 * dw_pci_bottom_ack - 처리한 벡터의 STATUS 비트를 write-1-to-clear 로 지운다
 *
 * @d: 처리 중인 벡터의 irq_data.
 * @return: 없음.
 *
 * PCIE_MSI_INTR0_STATUS 는 write-1-to-clear 레지스터다. 그래서 지우려는
 * 비트 하나만 세운 값 BIT(bit) 을 그대로 써 넣는다 -- 읽고 고쳐 쓸 필요가
 * 없고, 그래서 mask/unmask 와 달리 **잠금도 필요 없다**. 다른 비트에 0 을
 * 쓰는 것은 그 비트에 아무 영향이 없기 때문이다.
 *
 * 상태 비트를 여기서 지우는 이유: dw_handle_msi_irq 는 STATUS 를 읽기만 하고
 * 지우지 않는다. irq 코어가 각 벡터를 handle_edge_irq 로 처리하면서 이
 * 콜백을 부르는 시점에 지워야, 처리 도중 새로 도착한 같은 벡터의 MSI 를
 * 잃지 않는다.
 *
 * SMP 빌드에서는 이 함수가 irq_ack 가 아니라 irq_pre_redirect 자리에 걸린다
 * (dw_pci_msi_bottom_irq_chip 참조). affinity 변경 구간에서 소거 시점을
 * 재지향과 맞물리게 하기 위해서다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   handle_edge_irq → chip->irq_ack (SMP 에서는 irq_pre_redirect)
 *     → [이 함수] → dw_pcie_writel_dbi
 */
static void dw_pci_bottom_ack(struct irq_data *d)
{
	/* NVMe: MSI 상태 비트 클리어, NVMe MSI 처리 완료 알림 */
	struct dw_pcie_rp *pp  = irq_data_get_irq_chip_data(d);	/* NVMe: RC 포트, NVMe MSI 소속 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);			/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	unsigned int res, bit, ctrl;					/* NVMe: 레지스터/비트/컨트롤러, NVMe 벡터 위치 */

	ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 번호, NVMe 벡터 그룹 */
	res = ctrl * MSI_REG_CTRL_BLOCK_SIZE;		/* NVMe: STATUS 레지스터 오프셋, NVMe MSI 완료 위치 */
	bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;		/* NVMe: 비트 위치, NVMe 큐 대응 벡터 */

	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_STATUS + res, BIT(bit));	/* NVMe: 해당 MSI 상태 비트 클리어, NVMe 다음 MSI 준비 */
}

static struct irq_chip dw_pci_msi_bottom_irq_chip = {
	.name			= "DWPCI-MSI",		/* NVMe: chip 이름, NVMe MSI IRQ 식별 */
	.irq_compose_msi_msg	= dw_pci_setup_msi_msg,	/* NVMe: MSI 메시지 구성, NVMe 장치에 전달할 주소/데이터 */
	.irq_mask		= dw_pci_bottom_mask,	/* NVMe: 벡터 마스크, NVMe MSI-X per-vector mask 대응 */
	.irq_unmask		= dw_pci_bottom_unmask,	/* NVMe: 벡터 언마스크, NVMe 큐 ISR 활성화 */
#ifdef CONFIG_SMP
	.irq_ack		= dw_irq_noop,		/* NVMe: SMP ack no-op, NVMe affinity 이동 시 parent ack 사용 */
	.irq_pre_redirect	= dw_pci_bottom_ack,	/* NVMe: affinity 변경 전 ack, NVMe ISR 안정성 */
	.irq_set_affinity	= irq_chip_redirect_set_affinity,	/* NVMe: affinity 재지정, NVMe 큐 IRQ CPU 이동 */
#else
	.irq_ack		= dw_pci_bottom_ack,	/* NVMe: UP에서는 bottom ack, NVMe MSI 처리 완료 */
#endif
};

/* [한국어]
 * dw_pcie_irq_domain_alloc - MSI 비트맵에서 벡터를 잡아 virq 에 연결한다
 *
 * @domain: 부모 MSI 도메인. host_data 에 dw_pcie_rp 가 들어 있다.
 * @virq: 커널이 배정한 첫 가상 IRQ 번호.
 * @nr_irqs: 연속으로 요청된 벡터 개수. multi-MSI 는 2의 거듭제곱이다.
 * @args: 이 도메인에서는 쓰지 않는다.
 * @return: 0 성공, -ENOSPC 면 남은 벡터가 없다.
 *
 * multi-MSI 는 "연속되고 자연 정렬된" 벡터 묶음을 요구한다. 디바이스가
 * 받는 것은 시작 번호 하나뿐이고 나머지는 하위 비트를 바꿔 가며 만들기
 * 때문이다. bitmap_find_free_region(..., order_base_2(nr_irqs)) 이 바로 그
 * 제약을 지키는 할당기다 -- 크기를 2의 지수로 올림하고 그 경계에 맞춰 잡는다.
 *
 * 비트맵은 여러 CPU 가 동시에 건드릴 수 있으므로 pp->lock 으로 감싼다.
 * scoped_guard 로 범위를 좁힌 것은, 뒤따르는 irq_domain_set_info() 가 잠금을
 * 필요로 하지 않고 오히려 그 안에서 잠들 수 있기 때문이다 -- raw 스핀락을
 * 쥔 채 부르면 안 된다.
 *
 * 잡은 뒤에는 벡터마다 irq_domain_set_info() 로 hwirq(bit + i), irq_chip,
 * chip_data(pp), 흐름 핸들러(handle_edge_irq)를 연결한다. MSI 가 엣지인
 * 이유는 메시지 한 번이 곧 한 번의 사건이고 되풀이 신호가 없기 때문이다.
 *
 * 실행 컨텍스트: 벡터 할당을 요청하는 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_alloc_irq_vectors → msi 코어 → domain_ops->alloc → [이 함수]
 *     → bitmap_find_free_region → irq_domain_set_info
 */
static int dw_pcie_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs, void *args)
{
	/* NVMe: MSI IRQ 도메인 할당, NVMe 큐당 MSI/MSI-X 벡터 배정 */
	struct dw_pcie_rp *pp = domain->host_data;	/* NVMe: 호스트 데이터에서 RC 포트, NVMe 연결 포트 */
	int bit;					/* NVMe: 할당된 MSI 비트, NVMe 큐 벡터 번호 */

	scoped_guard (raw_spinlock_irq, &pp->lock) {	/* NVMe: RC 포트 lock, NVMe MSI 벡터 할당 동시 접근 보호 */
		bit = bitmap_find_free_region(pp->msi_irq_in_use, pp->num_vectors,
					      order_base_2(nr_irqs));	/* NVMe: 연속된 비트 영역 탐색, NVMe 다중 MSI/MSI-X 벡터 그룹 할당 */
	}

	if (bit < 0)
		return -ENOSPC;	/* NVMe: MSI 벡터 고갈, NVMe 큐 수 초과 시 실패 */

	for (unsigned int i = 0; i < nr_irqs; i++) {	/* NVMe: 요청된 수만큼 벡터 설정, NVMe submission/completion 큐별 할당 */
		irq_domain_set_info(domain, virq + i, bit + i, pp->msi_irq_chip,
				    pp, handle_edge_irq, NULL, NULL);	/* NVMe: virq-hwirq 매핑, NVMe 큐 ISR 등록 */
	}
	return 0;	/* NVMe: MSI 벡터 할당 성공, NVMe 큐 인터럽트 준비 완료 */
}

/* [한국어]
 * dw_pcie_irq_domain_free - 할당했던 MSI 벡터 묶음을 비트맵에 되돌린다
 *
 * @domain: 부모 MSI 도메인.
 * @virq: 반납할 첫 가상 IRQ 번호.
 * @nr_irqs: 반납할 개수.
 * @return: 없음.
 *
 * 되돌릴 위치는 virq 가 아니라 hwirq 다. 그래서 첫 virq 의 irq_data 를 꺼내
 * d->hwirq 를 얻고, alloc 때와 똑같이 order_base_2(nr_irqs) 크기로
 * bitmap_release_region() 한다 -- 잡을 때와 반납할 때의 차수가 어긋나면
 * 비트맵이 조용히 망가진다.
 *
 * alloc 과 같은 pp->lock 을 쓴다. 여기서는 뒤따르는 잠들 수 있는 호출이
 * 없으므로 scoped_guard 대신 함수 전체를 덮는 guard 로 충분하다.
 *
 * 실행 컨텍스트: pci_free_irq_vectors 등의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_free_irq_vectors → msi 코어 → domain_ops->free → [이 함수]
 */
static void dw_pcie_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs)
{
	/* NVMe: MSI IRQ 도메인 해제, NVMe 큐 제거 시 벡터 반납 */
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);	/* NVMe: 첫 virq의 irq_data, NVMe 첫 큐 벡터 정보 */
	struct dw_pcie_rp *pp = domain->host_data;			/* NVMe: RC 포트, NVMe 연결 포트 */

	guard(raw_spinlock_irq)(&pp->lock);	/* NVMe: RC 포트 lock, NVMe MSI 벡터 해제 동시 접근 보호 */
	bitmap_release_region(pp->msi_irq_in_use, d->hwirq, order_base_2(nr_irqs));	/* NVMe: 벡터 비트맵 반납, NVMe 큐 벡터 재사용 가능 */
}

static const struct irq_domain_ops dw_pcie_msi_domain_ops = {
	.alloc	= dw_pcie_irq_domain_alloc,	/* NVMe: MSI 벡터 할당, NVMe 큐당 IRQ 생성 */
	.free	= dw_pcie_irq_domain_free,	/* NVMe: MSI 벡터 해제, NVMe 큐 제거 시 */
};

/* [한국어]
 * dw_pcie_allocate_domains - iMSI-RX 를 대표하는 부모 MSI 도메인을 만든다
 *
 * @pp: 이 루트 포트. num_vectors 가 도메인 크기를 정하고, 만들어진 도메인이
 *      pp->irq_domain 에 저장된다.
 * @return: 0 성공, -ENOMEM 생성 실패.
 *
 * 커널의 MSI 계층은 "부모 도메인 하나 + 디바이스마다 파생되는 자식 도메인"
 * 구조다. 이 함수는 그 부모를 만든다. 자식은 나중에
 * dw_pcie_init_dev_msi_info() 를 거쳐 파생된다.
 *
 * irq_domain_info 에 담기는 것:
 *   - fwnode: 이 컨트롤러의 펌웨어 노드. DT 의 msi-parent 참조가 이 노드를
 *     가리켜야 자식이 부모를 찾아온다.
 *   - ops: alloc/free 쌍(dw_pcie_msi_domain_ops).
 *   - size: 전체 벡터 수. 비트맵과 hwirq 범위의 상한이 된다.
 *   - host_data: pp. alloc/free 가 domain->host_data 로 되찾는다.
 *
 * msi_create_parent_irq_domain() 에 dw_pcie_msi_parent_ops 를 함께 넘겨,
 * 자식 도메인이 요구/지원하는 플래그(멀티 MSI, MSI-X 등)와 이름 접두사
 * "DW-" 를 등록한다.
 *
 * EXPORT 되어 있어, 자체 MSI 구현을 쓰는 SoC 드라이버가 도메인만 빌려 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_msi_host_init(또는 SoC 드라이버) → [이 함수]
 *     → msi_create_parent_irq_domain
 */
int dw_pcie_allocate_domains(struct dw_pcie_rp *pp)
{
	/* NVMe: MSI parent IRQ 도메인 생성, NVMe 장치의 MSI/MSI-X 할당 인프라 구축 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct irq_domain_info info = {
		.fwnode		= dev_fwnode(pci->dev),	/* NVMe: firmware node, NVMe MSI 도메인과 DT 연결 */
		.ops		= &dw_pcie_msi_domain_ops,	/* NVMe: MSI 도메인 ops, NVMe 벡터 alloc/free */
		.size		= pp->num_vectors,		/* NVMe: 최대 MSI 벡터 수, NVMe 큐 수 제한 */
		.host_data	= pp,				/* NVMe: RC 포트를 host data로, NVMe MSI 핸들러에서 포트 식별 */
	};

	pp->irq_domain = msi_create_parent_irq_domain(&info, &dw_pcie_msi_parent_ops);	/* NVMe: parent IRQ 도메인 생성, NVMe MSI 하드웨어 추상화 */
	if (!pp->irq_domain) {
		dev_err(pci->dev, "Failed to create IRQ domain\n");	/* NVMe: 도메인 생성 실패, NVMe MSI 할당 불가 */
		return -ENOMEM;	/* NVMe: 메모리 부족, NVMe 초기화 실패 */
	}

	return 0;	/* NVMe: IRQ 도메인 생성 완료, NVMe MSI 사용 가능 */
}
EXPORT_SYMBOL_GPL(dw_pcie_allocate_domains);

/* [한국어]
 * dw_pcie_free_msi - 연쇄 핸들러를 떼고 MSI 도메인을 없앤다
 *
 * @pp: 정리할 루트 포트.
 * @return: 없음.
 *
 * 순서가 중요하다. 먼저 등록해 둔 모든 msi_irq 에서
 * irq_set_chained_handler_and_data(irq, NULL, NULL) 로 핸들러와 데이터를
 * 떼어 낸다. 이걸 먼저 하지 않고 도메인을 없애면, 남아 있던 인터럽트가
 * 이미 해제된 도메인을 참조할 수 있다.
 *
 * 루프가 num_ctrls 가 아니라 MAX_MSI_CTRLS 까지 도는 점에 유의. num_vectors
 * 가 도중에 줄어들었더라도 배열에 남아 있는 IRQ 를 빠뜨리지 않기 위해서다.
 * msi_irq[ctrl] > 0 검사는 등록되지 않은 칸(0)과 오류값(음수)을 함께 거른다.
 *
 * 실행 컨텍스트: 프로브 실패 되감기 또는 제거 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init 에러 경로 / dw_pcie_host_deinit → [이 함수]
 *     → irq_set_chained_handler_and_data → irq_domain_remove
 */
void dw_pcie_free_msi(struct dw_pcie_rp *pp)
{
	/* NVMe: MSI 인프라 해제, NVMe 장치 제거/호스트 종료 시 */
	u32 ctrl;	/* NVMe: MSI 컨트롤러 인덱스, NVMe 다중 MSI IRQ 해제 */

	for (ctrl = 0; ctrl < MAX_MSI_CTRLS; ctrl++) {	/* NVMe: 각 MSI 컨트롤러 순회, NVMe 연결된 모든 MSI IRQ */
		if (pp->msi_irq[ctrl] > 0)
			irq_set_chained_handler_and_data(pp->msi_irq[ctrl], NULL, NULL);	/* NVMe: 연결형 핸들러 제거, NVMe MSI IRQ 분리 */
	}

	irq_domain_remove(pp->irq_domain);	/* NVMe: MSI IRQ 도메인 제거, NVMe 벡터 매핑 해제 */
}
EXPORT_SYMBOL_GPL(dw_pcie_free_msi);

/* [한국어]
 * dw_pcie_msi_init - iMSI-RX 하드웨어에 마스크/활성/목적지 주소를 써 넣는다
 *
 * @pp: 이 루트 포트.
 * @return: 없음.
 *
 * dw_pcie_msi_host_init 이 소프트웨어 쪽(도메인, 비트맵, 목적지 주소 확보)을
 * 준비했다면, 이 함수는 그 결과를 실제 레지스터에 반영한다. 두 단계가 나뉜
 * 이유는 재개(resume)나 링크 재설정 후 하드웨어만 다시 프로그래밍하면 되기
 * 때문이다 -- 그래서 dw_pcie_setup_rc() 가 매번 이것을 부른다.
 *
 * 앞머리의 조기 탈출이 중요하다. pci_msi_enabled() 가 거짓이면 커널이 MSI 를
 * 쓰지 않고, use_imsi_rx 가 거짓이면 MSI 를 외부 컨트롤러(GIC ITS 등)가
 * 맡으므로 이 IP 의 iMSI-RX 를 건드리면 안 된다.
 *
 * 블록마다 두 가지를 쓴다:
 *   - MASK 에는 소프트웨어 사본 pp->irq_mask[ctrl] 을 그대로 -- 재개 시
 *     서스펜드 전의 마스크 상태가 복원된다.
 *   - ENABLE 에는 ~0 (전부 허용). 실제 차단은 MASK 가 담당하므로 ENABLE 은
 *     열어 두고 MASK 한 겹으로만 제어하는 설계다.
 *
 * 마지막으로 MSI 목적지 주소를 LO/HI 두 레지스터에 나눠 쓴다. 디바이스가 이
 * 주소로 쓰기를 보내면 iMSI-RX 가 가로챈다.
 *
 * 실행 컨텍스트: 프로브 및 재개 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_setup_rc → [이 함수] → dw_pcie_writel_dbi
 */
void dw_pcie_msi_init(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare MSI 하드웨어 초기화, NVMe 장치가 MSI TLP 본문 전송 가능하도록 설정 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	u64 msi_target = (u64)pp->msi_data;		/* NVMe: MSI 타겟 주소, NVMe가 메모리 쓰기할 주소 */
	u32 ctrl, num_ctrls;				/* NVMe: 컨트롤러 인덱스/개수, NVMe MSI 벡터 그룹 */

	if (!pci_msi_enabled() || !pp->use_imsi_rx)
		return;	/* NVMe: MSI 미지원 또는 외부 MSI 컨트롤러 사용 시, NVMe MSI 초기화 불필요 */

	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 초기화할 컨트롤러 수, NVMe MSI 벡터 그룹 계산 */

	/* Initialize IRQ Status array */
	for (ctrl = 0; ctrl < num_ctrls; ctrl++) {	/* NVMe: 각 MSI 컨트롤러 초기화, NVMe MSI 수신 가능 상태 설정 */
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK +
				    (ctrl * MSI_REG_CTRL_BLOCK_SIZE),
				    pp->irq_mask[ctrl]);	/* NVMe: 마스크 레지스터 설정, NVMe MSI 벡터 마스크 초기 상태 */
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_ENABLE +
				    (ctrl * MSI_REG_CTRL_BLOCK_SIZE),
				    ~0);	/* NVMe: MSI 인터럽트 활성화, NVMe MSI 수신 가능 */
	}

	/* Program the msi_data */
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_LO, lower_32_bits(msi_target));	/* NVMe: MSI 타겟 주소 LO, 32-bit NVMe 장치 전달 */
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_HI, upper_32_bits(msi_target));	/* NVMe: MSI 타겟 주소 HI, 64-bit NVMe 장치 전달 */
}
EXPORT_SYMBOL_GPL(dw_pcie_msi_init);

/* [한국어]
 * dw_pcie_parse_split_msi_irq - "msi0".."msi7" 로 쪼개진 인터럽트 선을 모은다
 *
 * @pp: 이 루트 포트. 찾은 IRQ 를 pp->msi_irq[] 에 채우고 num_vectors 를 조정한다.
 * @return: 0 성공, -ENXIO 는 "msi0" 조차 없다(= 분리형이 아니다), 그 외 음수는
 *          DT 파싱 오류로 dev_err_probe 를 거친 값.
 *
 * SoC 에 따라 MSI 그룹마다 별도의 인터럽트 선을 뽑아 놓는다. 그러면 32개
 * 벡터를 담당하는 선이 여러 개가 되고, DT 에는 interrupt-names 로
 * "msi0", "msi1", ... 이 나타난다. 이 함수가 그것을 순서대로 긁는다.
 *
 * msi_name[] = "msiX" 를 만들어 두고 msi_name[3] 에 '0' + ctrl 을 넣는 방식이
 * 라 **ctrl 이 9를 넘으면 이름이 깨진다**. 다만 MAX_MSI_CTRLS 는
 * MAX_MSI_IRQS(256) / MAX_MSI_IRQS_PER_CTRL(32) = 8 이므로 그 범위 안에서는
 * 안전하다.
 *
 * -ENXIO 에서 break 하는 것이 정상 종료다: 이름이 더 없다 = 선이 그만큼뿐.
 * 다른 음수는 진짜 오류이므로 그대로 올린다. 한 바퀴도 못 돌았으면(ctrl == 0)
 * 분리형이 아니라는 뜻의 -ENXIO 를 돌려주고, 호출자는 그것을 오류가 아니라
 * "단일 선 방식" 신호로 받아들인다.
 *
 * 마지막의 벡터 수 조정: 찾은 선의 개수 * 32 가 실제 상한이다. DT 가 그보다
 * 많이 요구하면 경고와 함께 깎고, 아예 지정하지 않았으면 상한으로 채운다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_msi_host_init → [이 함수] → platform_get_irq_byname_optional
 */
static int dw_pcie_parse_split_msi_irq(struct dw_pcie_rp *pp)
{
	/* NVMe: DT의 분할 MSI IRQ 파싱, NVMe 큐 수가 많을 때 다중 MSI IRQ 사용 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct platform_device *pdev = to_platform_device(dev);	/* NVMe: 플랫폼 디바이스, NVMe PCIe 호스트 리소스 */
	u32 ctrl, max_vectors;				/* NVMe: 컨트롤러 인덱스/최대 벡터, NVMe MSI 규모 */
	int irq;					/* NVMe: 파싱된 IRQ 번호, NVMe MSI 하드웨어 IRQ */

	/* Parse any "msiX" IRQs described in the devicetree */
	for (ctrl = 0; ctrl < MAX_MSI_CTRLS; ctrl++) {	/* NVMe: msi0..msiN 순회, NVMe 다중 MSI IRQ 등록 */
		char msi_name[] = "msiX";		/* NVMe: IRQ 이름 버퍼, NVMe MSI IRQ 이름 구성 */

		msi_name[3] = '0' + ctrl;	/* NVMe: msi0/msi1/... 이름 생성, NVMe MSI 컨트롤러별 IRQ */
		irq = platform_get_irq_byname_optional(pdev, msi_name);	/* NVMe: 이름으로 IRQ 획득, NVMe MSI 하드웨어 라인 */
		if (irq == -ENXIO)
			break;	/* NVMe: 더 이상 msiX 없음, NVMe MSI IRQ 파싱 종료 */
		if (irq < 0)
			return dev_err_probe(dev, irq,
					     "Failed to parse MSI IRQ '%s'\n",
					     msi_name);	/* NVMe: IRQ 파싱 오류, NVMe 초기화 실패 */

		pp->msi_irq[ctrl] = irq;	/* NVMe: 컨트롤러 IRQ 저장, NVMe MSI 인터럽트 라인 등록 */
	}

	/* If no "msiX" IRQs, caller should fallback to "msi" IRQ */
	if (ctrl == 0)
		return -ENXIO;	/* NVMe: 분할 MSI 없음, NVMe 단일 "msi" IRQ로 폴백 */

	max_vectors = ctrl * MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 사용 가능한 최대 벡터 수, NVMe 큐 수 상한 */
	if (pp->num_vectors > max_vectors) {
		dev_warn(dev, "Exceeding number of MSI vectors, limiting to %u\n",
			 max_vectors);	/* NVMe: 요청 벡터 초과 경고, NVMe 큐 수 제한 */
		pp->num_vectors = max_vectors;	/* NVMe: 벡터 수 제한, NVMe 큐 수 조정 */
	}
	if (!pp->num_vectors)
		pp->num_vectors = max_vectors;	/* NVMe: 기본값으로 최대 벡터, NVMe 큐 수 자동 설정 */

	return 0;	/* NVMe: 분할 MSI 파싱 완료, NVMe 다중 MSI IRQ 준비 */
}

/* [한국어]
 * dw_pcie_msi_host_init - 내장 iMSI-RX 를 쓰는 MSI 경로 전체를 세운다
 *
 * @pp: 이 루트 포트.
 * @return: 0 성공. 음수는 IRQ 조회 실패나 목적지 주소 확보 실패.
 *
 * 이 함수가 하는 일은 네 가지다: 인터럽트 선 확보, 벡터 수 확정, IRQ 도메인
 * 생성과 핸들러 연결, 그리고 MSI 목적지 주소 확보.
 *
 * (1) 마스크 사본을 전부 ~0 으로 초기화한다. 즉 처음에는 모든 벡터가 막혀
 *     있고, 디바이스가 벡터를 요청해 unmask 할 때 비로소 열린다.
 *
 * (2) 인터럽트 선: msi_irq[0] 이 비어 있으면 먼저 분리형("msi0"..)을 시도하고,
 *     그마저 없으면 단일 "msi" 를, 그것도 없으면 인덱스 0의 인터럽트를 쓴다.
 *     이 3단 폴백은 오래된 DT 와 새 DT 를 모두 받아들이기 위한 것이다.
 *     SoC 드라이버가 msi_irq[0] 을 미리 채워 두었다면 전부 건너뛴다.
 *
 * (3) 도메인을 만들고, 확보한 선마다 dw_chained_msi_isr 을 연쇄 핸들러로
 *     건다. 이때 핸들러 데이터로 pp 를 심어, ISR 이 그것을 되찾는다.
 *
 * (4) 목적지 주소: 상류 주석이 근거를 자세히 적어 두었다. 요지는 iMSI-RX 가
 *     64비트를 지원해도 **주변 디바이스 쪽이 64비트 메시지를 못 쓸 수 있어**
 *     목적지를 4GB 아래로 두는 편이 안전하다는 것, 그리고 이 주소로 향하는
 *     쓰기는 iMSI-RX 에서 종결되어 AXI 버스에 나타나지 않으므로 **실제
 *     메모리를 뒤에 둘 필요가 없다**는 것이다(DWC databook r6.21a §3.10.2.3).
 *     그래서 순서가 이렇다:
 *       a. cfg0_base 가 32비트 안이면 그것을 그대로 쓴다 -- 할당 없이 끝난다.
 *       b. 아니면 coherent 마스크를 32비트로 낮춰 dmam_alloc_coherent 시도.
 *       c. 그래도 실패하면 64비트로 되돌려 다시 시도.
 *     마지막까지 실패하면 방금 만든 도메인/핸들러를 dw_pcie_free_msi 로
 *     되감고 -ENOMEM 을 올린다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init → [이 함수] → dw_pcie_parse_split_msi_irq
 *     → dw_pcie_allocate_domains → irq_set_chained_handler_and_data
 *       → dmam_alloc_coherent
 */
int dw_pcie_msi_host_init(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare MSI 호스트 초기화, NVMe 장치가 사용할 MSI/MSI-X 인프라 구축 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct platform_device *pdev = to_platform_device(dev);	/* NVMe: 플랫폼 디바이스, NVMe PCIe 호스트 리소스 */
	u64 *msi_vaddr = NULL;				/* NVMe: MSI 주소 가상 주소, NVMe MSI 타겟 메모리 */
	int ret;					/* NVMe: 반환값, NVMe 초기화 결과 */
	u32 ctrl, num_ctrls;				/* NVMe: 컨트롤러 인덱스/개수, NVMe MSI 그룹 */

	for (ctrl = 0; ctrl < MAX_MSI_CTRLS; ctrl++)
		pp->irq_mask[ctrl] = ~0;	/* NVMe: 초기 마스크 전체 설정, NVMe MSI 벡터 기본 차단 */

	if (!pp->msi_irq[0]) {
		ret = dw_pcie_parse_split_msi_irq(pp);	/* NVMe: 분할 MSI IRQ 파싱, NVMe 다중 MSI IRQ 지원 */
		if (ret < 0 && ret != -ENXIO)
			return ret;	/* NVMe: 치명적 오류, NVMe MSI 초기화 중단 */
	}

	if (!pp->num_vectors)
		pp->num_vectors = MSI_DEF_NUM_VECTORS;	/* NVMe: 기본 벡터 수, NVMe 큐 수 기본값 */
	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;	/* NVMe: 컨트롤러 수, NVMe MSI 그룹 계산 */

	if (!pp->msi_irq[0]) {
		pp->msi_irq[0] = platform_get_irq_byname_optional(pdev, "msi");	/* NVMe: "msi" 이름 IRQ 획득, NVMe MSI 하드웨어 라인 */
		if (pp->msi_irq[0] < 0) {
			pp->msi_irq[0] = platform_get_irq(pdev, 0);	/* NVMe: 이름 없으면 인덱스 0, NVMe MSI 폴백 */
			if (pp->msi_irq[0] < 0)
				return pp->msi_irq[0];	/* NVMe: MSI IRQ 없음, NVMe MSI 사용 불가 */
		}
	}

	dev_dbg(dev, "Using %d MSI vectors\n", pp->num_vectors);	/* NVMe: 사용 벡터 수 로그, NVMe 큐 수 트레이스 */

	pp->msi_irq_chip = &dw_pci_msi_bottom_irq_chip;	/* NVMe: bottom IRQ chip 연결, NVMe MSI mask/ack/unmask */

	ret = dw_pcie_allocate_domains(pp);	/* NVMe: MSI IRQ 도메인 생성, NVMe 벡터 할당 준비 */
	if (ret)
		return ret;	/* NVMe: 도메인 생성 실패, NVMe MSI 초기화 중단 */

	for (ctrl = 0; ctrl < num_ctrls; ctrl++) {	/* NVMe: 각 MSI 컨트롤러에 연결형 핸들러 등록, NVMe MSI IRQ 라우팅 */
		if (pp->msi_irq[ctrl] > 0)
			irq_set_chained_handler_and_data(pp->msi_irq[ctrl],
						    dw_chained_msi_isr, pp);	/* NVMe: 하드웨어 IRQ에서 MSI ISR 연결, NVMe 인터럽트 디스패치 */
	}

	/*
	 * Even though the iMSI-RX Module supports 64-bit addresses some
	 * peripheral PCIe devices may lack 64-bit message support. In
	 * order not to miss MSI TLPs from those devices the MSI target
	 * address has to be within the lowest 4GB.
	 *
	 * Per DWC databook r6.21a, section 3.10.2.3, the incoming MWr TLP
	 * targeting the MSI_CTRL_ADDR is terminated by the iMSI-RX and never
	 * appears on the AXI bus. So MSI_CTRL_ADDR address doesn't need to be
	 * mapped and can be any memory that doesn't get allocated for the BAR
	 * memory. Since most of the platforms provide 32-bit address for
	 * 'config' region, try cfg0_base as the first option for the MSI target
	 * address if it's a 32-bit address. Otherwise, try 32-bit and 64-bit
	 * coherent memory allocation one by one.
	 */
	if (!(pp->cfg0_base & GENMASK_ULL(63, 32))) {
		pp->msi_data = pp->cfg0_base;	/* NVMe: 32-bit cfg0_base를 MSI 주소로 재사용, NVMe 32-bit MSI 장치 지원 */
		return 0;	/* NVMe: MSI 주소 설정 완료, NVMe MSI TLP 수신 준비 */
	}

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));	/* NVMe: 32-bit coherent DMA 마스크 시도, NVMe 32-bit MSI 주소 할당 */
	if (!ret)
		msi_vaddr = dmam_alloc_coherent(dev, sizeof(u64), &pp->msi_data,
						GFP_KERNEL);	/* NVMe: 32-bit coherent MSI 주소 할당, NVMe MSI 타겟 메모리 */

	if (!msi_vaddr) {
		dev_warn(dev, "Failed to allocate 32-bit MSI address\n");	/* NVMe: 32-bit 할당 실패, NVMe 64-bit MSI 장치로 폴백 */
		dma_set_coherent_mask(dev, DMA_BIT_MASK(64));	/* NVMe: 64-bit coherent 마스크, NVMe 64-bit MSI 주소 준비 */
		msi_vaddr = dmam_alloc_coherent(dev, sizeof(u64), &pp->msi_data,
						GFP_KERNEL);	/* NVMe: 64-bit coherent MSI 주소 할당, NVMe MSI 타겟 메모리 */
		if (!msi_vaddr) {
			dev_err(dev, "Failed to allocate MSI address\n");	/* NVMe: MSI 주소 할당 실패, NVMe MSI 사용 불가 */
			dw_pcie_free_msi(pp);	/* NVMe: MSI 인프라 해제, NVMe MSI 초기화 롤백 */
			return -ENOMEM;	/* NVMe: 메모리 부족, NVMe 초기화 실패 */
		}
	}

	return 0;	/* NVMe: MSI 호스트 초기화 완료, NVMe 장치 MSI 사용 가능 */
}
EXPORT_SYMBOL_GPL(dw_pcie_msi_host_init);

/* [한국어]
 * dw_pcie_host_request_msg_tlp_res - MSG TLP 를 쏘기 위한 주소 창 한 칸을 떼어 놓는다
 *
 * @pp: 이 루트 포트. 확보한 자원이 pp->msg_res 에 저장된다.
 * @return: 없음. 실패해도 msg_res 가 NULL 로 남을 뿐 프로브를 막지 않는다.
 *
 * PCIe 의 MSG(메시지) TLP 는 데이터가 아니라 신호를 실어 나르는 패킷이다.
 * DWC 의 iATU 는 특정 주소 창에 대한 쓰기를 MSG TLP 로 바꿔 내보내는 모드를
 * 지원하므로(PCIE_ATU_TYPE_MSG), 그 창으로 쓸 주소 공간이 필요하다. 이
 * 함수가 브리지의 첫 MEM 윈도 **맨 끝**에서 region_align 만큼을 잘라 낸다.
 *
 * 끝에서 자르는 이유: 앞쪽은 실제 디바이스 BAR 할당에 쓰이므로, 뒤쪽을
 * 떼어 내야 연속 영역이 덜 조각난다. 크기를 region_align 으로 잡는 것은
 * iATU 창의 최소 단위가 그 값이기 때문이다.
 *
 * devm_request_resource() 로 부모(win->res) 아래에 자식 자원으로 정식 등록해
 * PCI 코어가 같은 범위를 디바이스에 배정하지 못하게 막는다. IORESOURCE_BUSY
 * 를 세우는 것도 같은 목적이다. 등록에 성공했을 때만 pp->msg_res 를 채우므로,
 * 실패하면 이후 경로가 자연히 MSG 기능을 쓰지 않는다.
 *
 * 이 자원이 실제로 쓰이는 곳은 두 군데다: dw_pcie_iatu_setup() 이 msg 용
 * 창 번호를 예약하고, dw_pcie_pme_turn_off() 가 그 창에 써서 PME_Turn_Off
 * 메시지를 내보낸다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init → [이 함수] → devm_request_resource
 */
static void dw_pcie_host_request_msg_tlp_res(struct dw_pcie_rp *pp)
{
	/* NVMe: MSG TLP 전송용 메모리 영역 할당, NVMe PME/PM 메시지 전송 시 사용 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct resource_entry *win;			/* NVMe: 호스트 브리지 윈도우, NVMe 메모리 공간 */
	struct resource *res;				/* NVMe: 할당된 MSG 자원, NVMe PME TLP 주소 */

	win = resource_list_first_type(&pp->bridge->windows, IORESOURCE_MEM);	/* NVMe: 첫 MEM 윈도우 획득, NVMe 메모리 공간 내 MSG 영역 배치 */
	if (win) {
		res = devm_kzalloc(pci->dev, sizeof(*res), GFP_KERNEL);	/* NVMe: MSG 자원 구조체 할당, NVMe PME TLP 리소스 */
		if (!res)
			return;	/* NVMe: 메모리 부족, NVMe MSG TLP 기능 비활성화 */

		/*
		 * Allocate MSG TLP region of size 'region_align' at the end of
		 * the host bridge window.
		 */
		res->start = win->res->end - pci->region_align + 1;	/* NVMe: MSG 영역 시작, NVMe PME TLP 주소 범위 */
		res->end = win->res->end;				/* NVMe: MSG 영역 끝, NVMe PME TLP 주소 범위 */
		res->name = "msg";					/* NVMe: 자원 이름, NVMe PME/MSG TLP 식별 */
		res->flags = win->res->flags | IORESOURCE_BUSY;	/* NVMe: BUSY 플래그, NVMe BAR 할당에서 제외 */

		if (!devm_request_resource(pci->dev, win->res, res))
			pp->msg_res = res;	/* NVMe: MSG 자원 등록 성공, NVMe PME TLP 전송 가능 */
	}
}

/* [한국어]
 * dw_pcie_config_ecam_iatu - ECAM 모드에서 설정공간 접근용 iATU 창 두 개를 세운다
 *
 * @pp: 이 루트 포트.
 * @return: 0 성공, 음수는 dw_pcie_prog_outbound_atu 의 실패값.
 *
 * ECAM 은 "주소 비트에 버스/장치/함수를 그대로 인코딩" 하는 방식이라,
 * CPU 가 계산한 주소를 그대로 설정 TLP 로 바꿔 줄 창이 필요하다. 상류 주석이
 * 세 층으로 나눠 설명한다:
 *   - 호스트 브리지 바로 아래의 루트 버스는 창이 필요 없다. 루트 포트 자신의
 *     설정공간은 DBI 영역으로 직접 접근하기 때문이다
 *     (dw_pcie_ecam_conf_map_bus 가 busn==0 을 dbi_base 로 보낸다).
 *   - 루트 버스 바로 밑의 버스는 Type 0 설정 트랜잭션이어야 한다.
 *   - 그보다 아래의 모든 버스는 Type 1 이어야 한다.
 *
 * 그래서 창 0 은 cfg0_base + 1MiB 에서 시작하는 1MiB 를 CFG0 으로 잡는다.
 * 1MiB 인 이유는 상류 주석대로 버스 1개 * 장치 32개 * 함수 8개 * 4KiB 이기
 * 때문이다. 창 1 은 cfg0_base + 2MiB 부터 나머지 버스 전부를 CFG1 로 잡는다.
 * 버스 범위가 2개 미만이면 CFG1 창 자체가 필요 없으므로 그냥 0 을 돌려준다.
 *
 * 두 창 모두 ctrl2 에 PCIE_ATU_CFG_SHIFT_MODE_ENABLE 을 넣는다. 이 비트가
 * 켜져야 iATU 가 주소 하위 비트를 BDF 로 해석해 설정 TLP 를 만든다 -- ECAM
 * 의 주소 인코딩과 하드웨어를 이어 주는 스위치다.
 *
 * bus 를 NULL 검사 없이 bus->res 로 쓰는데, 이 함수는 ecam_enabled 가 참일
 * 때만 불리고 그 판정 자체가 버스 범위 존재를 전제하므로 도달 가능한
 * NULL 은 아니다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_setup_rc → dw_pcie_iatu_setup → [이 함수]
 *     → dw_pcie_prog_outbound_atu
 */
static int dw_pcie_config_ecam_iatu(struct dw_pcie_rp *pp)
{
	/* NVMe: ECAM 모드에서 iATU CFG 설정, NVMe 버스/장치 config 접근 경로 설정 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = {0};		/* NVMe: outbound ATU 설정, NVMe config 트랜잭션 경로 */
	resource_size_t bus_range_max;			/* NVMe: 버스 범위 최대, NVMe 탐색 버스 수 */
	struct resource_entry *bus;			/* NVMe: 버스 범위 엔트리, NVMe PCIe 버스 범위 */
	int ret;					/* NVMe: 반환값, NVMe iATU 프로그래밍 결과 */

	bus = resource_list_first_type(&pp->bridge->windows, IORESOURCE_BUS);	/* NVMe: 버스 범위 획득, NVMe 장치가 속한 PCIe 버스 범위 */

	/*
	 * Root bus under the host bridge doesn't require any iATU configuration
	 * as DBI region will be used to access root bus config space.
	 * Immediate bus under Root Bus, needs type 0 iATU configuration and
	 * remaining buses need type 1 iATU configuration.
	 */
	atu.index = 0;				/* NVMe: outbound iATU 인덱스 0, NVMe type 0 config 전용 */
	atu.type = PCIE_ATU_TYPE_CFG0;		/* NVMe: type 0 config, NVMe 루트 버스 하위 장치 접근 */
	atu.parent_bus_addr = pp->cfg0_base + SZ_1M;	/* NVMe: CPU 주소, NVMe config space 매핑 */
	/* 1MiB is to cover 1 (bus) * 32 (devices) * 8 (functions) */
	atu.size = SZ_1M;				/* NVMe: 1MB 크기, NVMe 1 bus * 32 dev * 8 fn 커버 */
	atu.ctrl2 = PCIE_ATU_CFG_SHIFT_MODE_ENABLE;	/* NVMe: CFG shift 모드, NVMe BDF 자동 변환 */
	ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: iATU 프로그램, NVMe type 0 config 경로 활성화 */
	if (ret)
		return ret;	/* NVMe: iATU 설정 실패, NVMe config 접근 불가 */

	bus_range_max = resource_size(bus->res);	/* NVMe: 버스 범위 크기, NVMe 탐색할 버스 수 */

	if (bus_range_max < 2)
		return 0;	/* NVMe: 버스 범위가 1개면 type 1 불필요, NVMe 추가 버스 없음 */

	/* Configure remaining buses in type 1 iATU configuration */
	atu.index = 1;				/* NVMe: outbound iATU 인덱스 1, NVMe type 1 config 전용 */
	atu.type = PCIE_ATU_TYPE_CFG1;		/* NVMe: type 1 config, NVMe 다중 버스 하위 장치 접근 */
	atu.parent_bus_addr = pp->cfg0_base + SZ_2M;	/* NVMe: CPU 주소, NVMe type 1 config space 시작 */
	atu.size = (SZ_1M * bus_range_max) - SZ_2M;	/* NVMe: 나머지 버스 커버, NVMe 추가 버스 config 접근 */
	atu.ctrl2 = PCIE_ATU_CFG_SHIFT_MODE_ENABLE;	/* NVMe: CFG shift 모드, NVMe BDF 자동 변환 */

	return dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: iATU 프로그램, NVMe type 1 config 경로 활성화 */
}

/* [한국어]
 * dw_pcie_create_ecam_window - 공용 ECAM 설정공간 매핑 객체를 만든다
 *
 * @pp: 이 루트 포트. 만들어진 pci_config_window 가 pp->cfg 에 저장된다.
 * @res: "config" reg 영역. ECAM 창 전체가 여기에 들어간다.
 * @return: 0 성공, -ENODEV 는 버스 범위 없음, 그 외 음수는 pci_ecam_create 실패.
 *
 * pci_ecam_create() 는 drivers/pci/ecam.c 의 공용 헬퍼로, 버스 범위에 맞춰
 * 설정공간을 ioremap 하고 버스 번호 → 가상 주소 변환표를 갖춘
 * struct pci_config_window 를 돌려준다. DWC 가 이것을 그대로 쓰는 덕에
 * 설정공간 읽기/쓰기는 pci_generic_config_read/write 로 끝난다.
 *
 * &pci_generic_ecam_ops 를 넘기지만 실제로 쓰이는 것은 그 안의 매핑 규칙뿐이다.
 * 브리지에 걸리는 ops 는 호출자(dw_pcie_host_get_resources)가
 * dw_pcie_ecam_ops 로 따로 지정하는데, 루트 버스만 DBI 로 돌리는 예외가
 * 필요하기 때문이다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_get_resources → [이 함수] → pci_ecam_create
 */
static int dw_pcie_create_ecam_window(struct dw_pcie_rp *pp, struct resource *res)
{
	/* NVMe: ECAM config 윈도우 생성, NVMe 버스 스캔 시 ECAM 기반 config 접근 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct resource_entry *bus;			/* NVMe: 버스 범위 엔트리, NVMe PCIe 버스 범위 */

	bus = resource_list_first_type(&pp->bridge->windows, IORESOURCE_BUS);	/* NVMe: 버스 범위 획득, NVMe 장치 버스 범위 */
	if (!bus)
		return -ENODEV;	/* NVMe: 버스 범위 없음, NVMe PCIe 열거 불가 */

	pp->cfg = pci_ecam_create(dev, res, bus->res, &pci_generic_ecam_ops);	/* NVMe: ECAM 윈도우 생성, NVMe config space 매핑 */
	if (IS_ERR(pp->cfg))
		return PTR_ERR(pp->cfg);	/* NVMe: ECAM 생성 실패, NVMe config 접근 불가 */

	return 0;	/* NVMe: ECAM 윈도우 생성 완료, NVMe 버스 스캔 가능 */
}

/* [한국어]
 * dw_pcie_ecam_enabled - 이 하드웨어/DT 조합에서 ECAM 방식을 쓸 수 있는지 판정한다
 *
 * @pp: 이 루트 포트.
 * @config_res: DT 의 "config" reg 영역.
 * @return: true 면 ECAM 경로(dw_pcie_ecam_ops), false 면 기존 iATU 창 방식
 *          (dw_pcie_ops + dw_child_pcie_ops).
 *
 * 세 가지 관문을 모두 통과해야 한다.
 *
 * (1) pp->native_ecam 이 참이면 곧바로 false. 벤더 글루 드라이버가 자기만의
 *     ECAM 을 구현했다는 표시라, 코어가 끼어들면 안 된다.
 *
 * (2) 시작 주소가 256MiB 정렬이어야 한다. 상류 주석의 근거가 명확하다 --
 *     PCIe spec r6.0 §7.2.2 는 ECAM 기준 주소를 2^(n+20) 경계에 맞추라고
 *     하고(n = BDF 의 버스 비트 수), DWC 코어는 버스를 항상 8비트로 쓰므로
 *     2^28 = 256MiB 다. 그래서 IS_256MB_ALIGNED 한 겹으로 검사한다.
 *
 * (3) config 영역이 DT 가 선언한 버스 범위 전부를 담을 만큼 커야 한다.
 *     영역 크기를 버스 하나당 크기로 나눈 값(nr_buses)이 버스 개수 이상이어야
 *     한다. 하나라도 모자라면 ECAM 주소 계산이 영역 밖으로 나가므로 false 다.
 *     (PCIE_ECAM_BUS_SHIFT 의 정의는 이 스파스 체크아웃에 없다. 다만 같은
 *      파일의 dw_pcie_config_ecam_iatu 가 버스 하나를 SZ_1M 으로 다루므로
 *      앞뒤가 맞으려면 20이어야 한다.)
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): (3)의 bus_range 는
 * resource_list_first_type(...)->res 로 **먼저 역참조한 뒤** NULL 을 검사한다.
 * 같은 파일의 다른 두 곳(dw_pcie_host_request_msg_tlp_res 의 win,
 * dw_pcie_host_get_resources 의 win)은 반환된 항목 자체를 NULL 검사하므로,
 * 이 헬퍼의 반환이 NULL 일 수 있다는 전제는 이 파일 안에서도 인정된다.
 * 즉 IORESOURCE_BUS 항목이 아예 없으면 검사가 걸리기 전에 역참조가 일어난다.
 * 실제로는 PCI 코어가 버스 범위를 항상 채워 주므로 도달하기 어려운 경로다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_get_resources → [이 함수]
 */
static bool dw_pcie_ecam_enabled(struct dw_pcie_rp *pp, struct resource *config_res)
{
	/* NVMe: ECAM 사용 가능 여부 판단, NVMe config 접근 방식 결정 */
	struct resource *bus_range;	/* NVMe: 버스 범위, NVMe PCIe 버스 범위 */
	u64 nr_buses;			/* NVMe: config 리소스의 버스 수, NVMe 탐색 가능 버스 수 */

	/* Vendor glue drivers may implement their own ECAM mechanism */
	if (pp->native_ecam)
		return false;	/* NVMe: vendor ECAM 사용 시 generic ECAM 불필요, NVMe config 접근 vendor 담당 */

	/*
	 * PCIe spec r6.0, sec 7.2.2 mandates the base address used for ECAM to
	 * be aligned on a 2^(n+20) byte boundary, where n is the number of bits
	 * used for representing 'bus' in BDF. Since the DWC cores always use 8
	 * bits for representing 'bus', the base address has to be aligned to
	 * 2^28 byte boundary, which is 256 MiB.
	 */
	if (!IS_256MB_ALIGNED(config_res->start))
		return false;	/* NVMe: 256MB 미정렬, NVMe ECAM 사용 불가, iATU 사용 */

	bus_range = resource_list_first_type(&pp->bridge->windows, IORESOURCE_BUS)->res;
	if (!bus_range)
		return false;	/* NVMe: 버스 범위 없음, NVMe ECAM 사용 불가 */

	nr_buses = resource_size(config_res) >> PCIE_ECAM_BUS_SHIFT;	/* NVMe: config 리소스 크기에서 버스 수, NVMe ECAM 커버 버스 수 */

	return nr_buses >= resource_size(bus_range);	/* NVMe: ECAM이 버스 범위 커버 시 true, NVMe ECAM 기반 열거 결정 */
}

/* [한국어]
 * dw_pcie_host_get_resources - DT 자원을 읽어 설정공간 접근 방식을 확정한다
 *
 * @pp: 이 루트 포트.
 * @return: 0 성공, -ENODEV 는 "config" reg 없음, 그 외 음수는 하위 실패값.
 *
 * 이 함수가 끝나면 "설정공간을 어떻게 읽을 것인가" 가 확정된다. 두 갈래다.
 *
 * ECAM 경로 (dw_pcie_ecam_enabled 가 참):
 *   pci_ecam_create 로 매핑 객체를 만들고, 브리지 ops 를 dw_pcie_ecam_ops 로,
 *   sysdata 를 pp->cfg 로 둔다. child_ops 는 두지 않는다 -- 하나의 map_bus 가
 *   루트 버스와 하위 버스를 모두 처리하기 때문이다. cfg->priv 에 pp 를 심어
 *   dw_pcie_ecam_conf_map_bus 가 되찾을 수 있게 한다.
 *
 * 기존 iATU 경로:
 *   config 영역을 devm_pci_remap_cfg_resource 로 매핑해 va_cfg0_base 에 둔다.
 *   브리지 ops 는 dw_pcie_ops(루트 버스 = DBI 직접), child_ops 는
 *   dw_child_pcie_ops(하위 버스 = iATU 창을 매번 다시 프로그래밍)로 나뉜다.
 *   sysdata 는 pp 자신이다.
 *
 * 그 다음 dw_pcie_get_resources(pci) 로 DBI/ATU 등 IP 공통 자원을 잡는다.
 * 여기서 실패하면 방금 만든 ECAM 매핑을 pci_ecam_free 로 되감는다 -- 그것만이
 * devm 이 아니어서 수동 해제가 필요하다.
 *
 * IO 윈도가 있으면 크기/버스주소/CPU 주소를 따로 챙긴다. io_base 를
 * pci_pio_to_address() 로 변환하는 이유는, 리소스에는 커널의 추상 PIO 번호가
 * 들어 있고 iATU 에는 진짜 물리 주소를 줘야 하기 때문이다.
 *
 * 마지막으로 parent_bus_offset 을 구한다. CPU 가 보는 주소와 IP 가 보는
 * 부모 버스 주소가 다를 수 있어, 이후 모든 iATU 설정이 이 차이를 뺀다.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init → [이 함수] → dw_pcie_ecam_enabled
 *     → dw_pcie_create_ecam_window / devm_pci_remap_cfg_resource
 *       → dw_pcie_get_resources → dw_pcie_parent_bus_offset
 */
static int dw_pcie_host_get_resources(struct dw_pcie_rp *pp)
{
	/* NVMe: 호스트 리소스 파싱/매핑, NVMe PCIe 호스트의 메모리/config/IO 설정 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct platform_device *pdev = to_platform_device(dev);	/* NVMe: 플랫폼 디바이스, NVMe PCIe 리소스 */
	struct resource_entry *win;			/* NVMe: 리소스 엔트리, NVMe 메모리/IO 윈도우 */
	struct resource *res;				/* NVMe: 플랫폼 리소스, NVMe config 공간 */
	int ret;					/* NVMe: 반환값, NVMe 리소스 설정 결과 */

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "config");	/* NVMe: "config" 메모리 리소스 획득, NVMe PCIe config 공간 */
	if (!res) {
		dev_err(dev, "Missing \"config\" reg space\n");	/* NVMe: config 리소스 누락, NVMe PCIe 초기화 불가 */
		return -ENODEV;	/* NVMe: 리소스 부재, NVMe 호스트 초기화 실패 */
	}

	pp->cfg0_size = resource_size(res);	/* NVMe: config 공간 크기, NVMe config 접근 범위 */
	pp->cfg0_base = res->start;		/* NVMe: config 공간 물리 주소, NVMe config mapping 기준 */

	pp->ecam_enabled = dw_pcie_ecam_enabled(pp, res);	/* NVMe: ECAM 사용 여부, NVMe config 접근 방식 */
	if (pp->ecam_enabled) {
		ret = dw_pcie_create_ecam_window(pp, res);	/* NVMe: ECAM 윈도우 생성, NVMe config ECAM 매핑 */
		if (ret)
			return ret;	/* NVMe: ECAM 생성 실패, NVMe config 접근 불가 */

		pp->bridge->ops = &dw_pcie_ecam_ops;	/* NVMe: ECAM config ops 설정, NVMe 버스 스캔 */
		pp->bridge->sysdata = pp->cfg;		/* NVMe: ECAM sysdata, NVMe config 접근 컨텍스트 */
		pp->cfg->priv = pp;			/* NVMe: dw_pcie_rp 역참조, NVMe ECAM->host 매핑 */
	} else {
		pp->va_cfg0_base = devm_pci_remap_cfg_resource(dev, res);	/* NVMe: config 공간 가상 매핑, NVMe config 레지스터 직접 접근 */
		if (IS_ERR(pp->va_cfg0_base))
			return PTR_ERR(pp->va_cfg0_base);	/* NVMe: 매핑 실패, NVMe config 접근 불가 */

		/* Set default bus ops */
		pp->bridge->ops = &dw_pcie_ops;		/* NVMe: 기본 RC config ops, NVMe 루트 포트 config 접근 */
		pp->bridge->child_ops = &dw_child_pcie_ops;	/* NVMe: 하위 버스 config ops, NVMe 장치 탐색 */
		pp->bridge->sysdata = pp;			/* NVMe: RC 포트 sysdata, NVMe config 접근 컨텍스트 */
	}

	ret = dw_pcie_get_resources(pci);	/* NVMe: DesignWare 공통 리소스 획득, NVMe 호스트 DBI/ATU 등 */
	if (ret) {
		if (pp->cfg)
			pci_ecam_free(pp->cfg);	/* NVMe: ECAM 해제, NVMe 리소스 롤백 */
		return ret;	/* NVMe: 리소스 획득 실패, NVMe 초기화 실패 */
	}

	/* Get the I/O range from DT */
	win = resource_list_first_type(&pp->bridge->windows, IORESOURCE_IO);	/* NVMe: DT에서 IO 범위 획득, NVMe IO 공간 설정 */
	if (win) {
		pp->io_size = resource_size(win->res);	/* NVMe: IO 공간 크기, NVMe IO 매핑 범위 */
		pp->io_bus_addr = win->res->start - win->offset;	/* NVMe: 버스 상대 IO 주소, NVMe IO 트랜잭션 주소 */
		pp->io_base = pci_pio_to_address(win->res->start);	/* NVMe: CPU PIO 주소, NVMe IO 공간 가상 주소 */
	}

	/*
	 * visconti_pcie_cpu_addr_fixup() uses pp->io_base, so we have to
	 * call dw_pcie_parent_bus_offset() after setting pp->io_base.
	 */
	pci->parent_bus_offset = dw_pcie_parent_bus_offset(pci, "config",
							   pp->cfg0_base);	/* NVMe: 부모 버스 오프셋, NVMe CPU/PCI 주소 변환 */
	return 0;	/* NVMe: 호스트 리소스 설정 완료, NVMe PCIe 열거 준비 */
}

/* [한국어]
 * dw_pcie_host_init - DWC 루트 포트를 처음부터 끝까지 세우는 주 진입점
 *
 * @pp: SoC 글루 드라이버가 채워 둔 루트 포트. ops, num_vectors 등 SoC 별
 *      설정이 미리 들어 있다.
 * @return: 0 성공. 음수면 단계별 goto 라벨을 타고 전부 되감긴 상태다.
 *
 * 모든 DWC 기반 SoC 드라이버(qcom, rockchip, fu740, intel-gw, keembay,
 * histb, hisi, dw-plat ...)의 프로브가 마지막에 부르는 함수다. 여기서
 * 브리지 할당 → 자원 파악 → SoC 초기화 → MSI → IP 판별 → iATU → 링크
 * 기동 → 버스 열거까지 한 번에 진행된다.
 *
 * 단계와 그 이유:
 *  1. raw_spin_lock_init(&pp->lock) -- MSI 마스크/비트맵을 지킬 잠금.
 *  2. devm_pci_alloc_host_bridge -- PCI 코어가 요구하는 브리지 객체.
 *     DT 의 ranges/dma-ranges 가 이때 bridge->windows/dma_ranges 로 파싱된다.
 *  3. dw_pcie_host_get_resources -- 설정공간 접근 방식 확정(위 참조).
 *  4. pp->ops->init -- SoC 글루의 클록/PHY/리셋 해제. 실패 시 err_free_ecam.
 *  5. MSI: use_imsi_rx 는 "SoC 가 자체 msi_init 을 갖지도 않고, DT 에
 *     msi-parent/msi-map 도 없을 때" 참이다. 즉 **외부 MSI 컨트롤러가
 *     없을 때만** 내장 iMSI-RX 를 쓴다. 외부를 쓰면서 num_vectors 가 0 이면
 *     기본값을 넣고, MAX_MSI_IRQS 를 넘으면 오류다.
 *  6. dw_pcie_version_detect / dw_pcie_iatu_detect -- IP 버전과 iATU 창
 *     개수/크기를 하드웨어에서 읽어 온다. 이 값이 없으면 이후 창 배분을
 *     할 수 없으므로 순서가 앞이다.
 *  7. num_lanes 가 안 정해졌으면 링크 능력에서 최대 폭을 읽어 채운다. 바로
 *     다음의 이퀄라이제이션 프리셋 개수가 레인 수에 비례하기 때문이다.
 *  8. of_pci_get_equalization_presets -- 8GT/s 이상에서 쓸 프리셋 표를 DT 에서.
 *  9. use_atu_msg 면 MSG TLP 용 주소 창을 떼어 둔다.
 * 10. dw_pcie_edma_detect -- 내장 DMA 엔진 등록.
 * 11. dw_pcie_setup_rc -- RC 설정공간과 iATU 를 실제로 프로그래밍.
 * 12. 링크가 아직이면 dw_pcie_start_link 로 기동.
 * 13. dw_pcie_wait_for_link -- **-ETIMEDOUT 만** 실패로 본다. 다른 값이나
 *     타임아웃이 아닌 경우에는 그대로 진행해, 나중에 붙는 장치를 놓치지 않는다.
 * 14. pci_host_probe -- 여기서 버스 열거가 일어나고 하위 장치 드라이버가 붙는다.
 * 15. post_init 훅과 debugfs 등록.
 *
 * 에러 되감기는 라벨 사슬로 되어 있어, 실패 지점부터 아래로 흘러가며 그때까지
 * 잡은 것만 정확히 푼다: stop_link → edma_remove → free_msi(내장 MSI 일 때만)
 * → ops->deinit → pci_ecam_free.
 *
 * 실행 컨텍스트: 프로브 시점의 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   SoC 글루 probe → [이 함수] → dw_pcie_host_get_resources
 *     → pp->ops->init → dw_pcie_msi_host_init → dw_pcie_setup_rc
 *       → dw_pcie_start_link → pci_host_probe
 */
int dw_pcie_host_init(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare RC 호스트 초기화, NVMe SSD를 위한 PCIe 루트 컴플렉스 구축 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */
	struct device *dev = pci->dev;			/* NVMe: 디바이스, NVMe 호스트 디바이스 */
	struct device_node *np = dev->of_node;		/* NVMe: DT 노드, NVMe 호스트 DT 속성 */
	struct pci_host_bridge *bridge;			/* NVMe: PCI 호스트 브리지, NVMe PCIe 버스 트리 루트 */
	int ret;					/* NVMe: 반환값, NVMe 초기화 결과 */

	raw_spin_lock_init(&pp->lock);	/* NVMe: RC 포트 spinlock 초기화, NVMe MSI/리소스 동시 접근 보호 */

	bridge = devm_pci_alloc_host_bridge(dev, 0);	/* NVMe: PCI 호스트 브리지 할당, NVMe PCIe 버스 트리 생성 */
	if (!bridge)
		return -ENOMEM;	/* NVMe: 브리지 할당 실패, NVMe PCIe 초기화 불가 */

	pp->bridge = bridge;	/* NVMe: RC 포트에 브리지 연결, NVMe 호스트 상태 저장 */

	ret = dw_pcie_host_get_resources(pp);	/* NVMe: 호스트 리소스 파싱, NVMe config/메모리/IO 설정 */
	if (ret)
		return ret;	/* NVMe: 리소스 설정 실패, NVMe PCIe 초기화 중단 */

	if (pp->ops->init) {
		ret = pp->ops->init(pp);	/* NVMe: vendor 초기화 콜백, NVMe SoC 특화 설정 */
		if (ret)
			goto err_free_ecam;	/* NVMe: vendor 초기화 실패, NVMe 호스트 정리 */
	}

	if (pci_msi_enabled()) {
		/* NVMe: MSI 사용 가능 시, NVMe 장치의 MSI/MSI-X 활성화 여부 결정 */
		pp->use_imsi_rx = !(pp->ops->msi_init ||
				     of_property_present(np, "msi-parent") ||
				     of_property_present(np, "msi-map"));	/* NVMe: 내장 iMSI-RX 사용 여부, NVMe MSI 컨트롤러 선택 */

		/*
		 * For the use_imsi_rx case the default assignment is handled
		 * in the dw_pcie_msi_host_init().
		 */
		if (!pp->use_imsi_rx && !pp->num_vectors) {
			pp->num_vectors = MSI_DEF_NUM_VECTORS;	/* NVMe: 외부 MSI 시 기본 벡터, NVMe 큐 수 기본값 */
		} else if (pp->num_vectors > MAX_MSI_IRQS) {
			dev_err(dev, "Invalid number of vectors\n");	/* NVMe: 벡터 수 초과, NVMe 큐 수 제한 위반 */
			ret = -EINVAL;	/* NVMe: 잘못된 인자, NVMe MSI 초기화 실패 */
			goto err_deinit_host;	/* NVMe: 호스트 deinit 후 종료, NVMe 정리 */
		}

		if (pp->ops->msi_init) {
			ret = pp->ops->msi_init(pp);	/* NVMe: vendor MSI 초기화, NVMe SoC 특화 MSI 설정 */
			if (ret < 0)
				goto err_deinit_host;	/* NVMe: MSI 초기화 실패, NVMe 정리 */
		} else if (pp->use_imsi_rx) {
			ret = dw_pcie_msi_host_init(pp);	/* NVMe: 내장 iMSI-RX 초기화, NVMe MSI 인프라 구축 */
			if (ret < 0)
				goto err_deinit_host;	/* NVMe: MSI 초기화 실패, NVMe 정리 */
		}
	}

	dw_pcie_version_detect(pci);	/* NVMe: DWC IP 버전 감지, NVMe 호스트 기능 결정 */

	dw_pcie_iatu_detect(pci);	/* NVMe: iATU 윈도우 수/제한 감지, NVMe 메모리/IO 매핑 능력 */

	if (pci->num_lanes < 1)
		pci->num_lanes = dw_pcie_link_get_max_link_width(pci);	/* NVMe: 링크 폭 기본값, NVMe 대역폭 결정 */

	ret = of_pci_get_equalization_presets(dev, &pp->presets, pci->num_lanes);	/* NVMe: equalization preset 파싱, NVMe Gen3/4/5/7 링크 품질 */
	if (ret)
		goto err_free_msi;	/* NVMe: preset 파싱 실패, NVMe 정리 */

	/*
	 * Allocate the resource for MSG TLP before programming the iATU
	 * outbound window in dw_pcie_setup_rc(). Since the allocation depends
	 * on the value of 'region_align', this has to be done after
	 * dw_pcie_iatu_detect().
	 *
	 * Glue drivers need to set 'use_atu_msg' before dw_pcie_host_init() to
	 * make use of the generic MSG TLP implementation.
	 */
	if (pp->use_atu_msg)
		dw_pcie_host_request_msg_tlp_res(pp);	/* NVMe: MSG TLP 영역 할당, NVMe PME/PM 메시지 전송 준비 */

	ret = dw_pcie_edma_detect(pci);	/* NVMe: eDMA 감지, NVMe 고성능 DMA 엔진 사용 가능 여부 */
	if (ret)
		goto err_free_msi;	/* NVMe: eDMA 감지 실패, NVMe 정리 */

	ret = dw_pcie_setup_rc(pp);	/* NVMe: RC 모드 설정, NVMe PCIe 루트 포트 구성 */
	if (ret)
		goto err_remove_edma;	/* NVMe: RC 설정 실패, NVMe 정리 */

	if (!dw_pcie_link_up(pci)) {
		ret = dw_pcie_start_link(pci);	/* NVMe: PCIe 링크 시작, NVMe SSD와 물리 연결 설정 */
		if (ret)
			goto err_remove_edma;	/* NVMe: 링크 시작 실패, NVMe 연결 불가 */
	}

	/*
	 * Only fail on timeout error. Other errors indicate the device may
	 * become available later, so continue without failing.
	 */
	ret = dw_pcie_wait_for_link(pci);	/* NVMe: 링크 업 대기, NVMe SSD 연결 완료 확인 */
	if (ret == -ETIMEDOUT)
		goto err_stop_link;	/* NVMe: 타임아웃 시 실패, NVMe 장치 미연결 */

	ret = pci_host_probe(bridge);	/* NVMe: PCI 호스트 프로브, NVMe PCIe 버스 스캔 및 장치 열거 */
	if (ret)
		goto err_stop_link;	/* NVMe: 버스 프로브 실패, NVMe 열거 불가 */

	if (pp->ops->post_init)
		pp->ops->post_init(pp);	/* NVMe: vendor 후처리, NVMe SoC 특화 마무리 */

	dwc_pcie_debugfs_init(pci, DW_PCIE_RC_TYPE);	/* NVMe: debugfs 초기화, NVMe 런타임 디버깅 인프라 */

	return 0;	/* NVMe: 호스트 초기화 완료, NVMe PCIe 사용 가능 */

err_stop_link:
	dw_pcie_stop_link(pci);	/* NVMe: 링크 정지, NVMe 연결 해제 */

err_remove_edma:
	dw_pcie_edma_remove(pci);	/* NVMe: eDMA 제거, NVMe DMA 엔진 정리 */

err_free_msi:
	if (pp->use_imsi_rx)
		dw_pcie_free_msi(pp);	/* NVMe: MSI 인프라 해제, NVMe MSI 벡터 정리 */

err_deinit_host:
	if (pp->ops->deinit)
		pp->ops->deinit(pp);	/* NVMe: vendor deinit, NVMe SoC 정리 */

err_free_ecam:
	if (pp->cfg)
		pci_ecam_free(pp->cfg);	/* NVMe: ECAM 윈도우 해제, NVMe config 매핑 정리 */

	return ret;	/* NVMe: 초기화 실패 코드 반환, NVMe 호스트 초기화 종료 */
}
EXPORT_SYMBOL_GPL(dw_pcie_host_init);

/* [한국어]
 * dw_pcie_host_deinit - dw_pcie_host_init 이 세운 것을 역순으로 모두 되돌린다
 *
 * @pp: 정리할 루트 포트.
 * @return: 없음.
 *
 * 순서가 init 의 정확한 역순이다. 특히 버스를 먼저 걷어내는 것이 중요하다:
 * pci_stop_root_bus 로 하위 장치 드라이버를 떼고 pci_remove_root_bus 로
 * 버스 객체를 없앤 뒤에야 링크를 내리고 자원을 푼다. 반대로 하면 아직 살아
 * 있는 드라이버가 사라진 설정공간에 접근한다.
 *
 * dwc_pcie_debugfs_deinit 을 맨 앞에 두는 것도 같은 이유다 -- 사용자가
 * debugfs 파일을 열어 둔 채 하드웨어가 사라지는 상황을 먼저 막는다.
 *
 * free_msi 를 use_imsi_rx 일 때만 부르는 점에 유의. 외부 MSI 컨트롤러를 쓰는
 * SoC 에서는 이 파일이 도메인을 만들지 않았으므로 없앨 것도 없다.
 *
 * 실행 컨텍스트: 드라이버 remove 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   SoC 글루 remove → [이 함수] → pci_stop_root_bus → pci_remove_root_bus
 *     → dw_pcie_stop_link → dw_pcie_edma_remove → dw_pcie_free_msi
 *       → pp->ops->deinit → pci_ecam_free
 */
void dw_pcie_host_deinit(struct dw_pcie_rp *pp)
{
	/* NVMe: DesignWare RC 호스트 해제, NVMe SSD 제거/시스템 종료 시 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터/디바이스 접근 */

	dwc_pcie_debugfs_deinit(pci);	/* NVMe: debugfs 해제, NVMe 디버깅 인프라 정리 */

	pci_stop_root_bus(pp->bridge->bus);	/* NVMe: 루트 버스 정지, NVMe 버스 트래픽 중단 */
	pci_remove_root_bus(pp->bridge->bus);	/* NVMe: 루트 버스 제거, NVMe PCIe 장치 언바인딩 */

	dw_pcie_stop_link(pci);	/* NVMe: PCIe 링크 정지, NVMe 물리 연결 해제 */

	dw_pcie_edma_remove(pci);	/* NVMe: eDMA 제거, NVMe DMA 엔진 정리 */

	if (pp->use_imsi_rx)
		dw_pcie_free_msi(pp);	/* NVMe: MSI 인프라 해제, NVMe MSI 벡터 정리 */

	if (pp->ops->deinit)
		pp->ops->deinit(pp);	/* NVMe: vendor deinit, NVMe SoC 정리 */

	if (pp->cfg)
		pci_ecam_free(pp->cfg);	/* NVMe: ECAM 윈도우 해제, NVMe config 매핑 정리 */
}
EXPORT_SYMBOL_GPL(dw_pcie_host_deinit);

/* [한국어]
 * dw_pcie_other_conf_map_bus - 하위 버스 설정 접근마다 iATU 창을 다시 겨눈다
 *
 * @bus: 접근할 버스. sysdata 에 dw_pcie_rp 가 있다.
 * @devfn: 장치/함수 번호.
 * @where: 설정공간 안의 바이트 오프셋.
 * @return: 접근할 가상 주소. NULL 이면 접근 불가(링크 다운 또는 iATU 실패).
 *
 * ECAM 이 아닌 경로의 핵심이다. iATU 창 하나(창 0)를 설정 트랜잭션 전용으로
 * 두고, **접근할 때마다** 그 창의 목적지 BDF 를 바꿔 겨눈다. 창이 부족한
 * 구형 IP 에서도 임의의 버스에 닿기 위한 방식이다.
 *
 * 먼저 링크 상태를 본다. 링크가 내려가 있으면 설정 TLP 가 응답 없이 타임아웃
 * 되거나 버스 오류로 번지므로, 아예 NULL 을 돌려 코어가 0xffffffff 로
 * 처리하게 한다.
 *
 * busdev 는 버스/장치/함수를 iATU 가 요구하는 자리로 옮겨 담은 값이다.
 * 이 값이 창의 "PCI 쪽 주소" 가 되어, CPU 가 va_cfg0_base + where 를 건드리면
 * iATU 가 그 BDF 를 향한 설정 TLP 로 바꿔 낸다.
 *
 * 타입 선택이 PCIe 규약 그대로다: 부모가 루트 버스면 대상은 링크 바로 건너편
 * 이므로 Type 0(CFG0), 그보다 깊으면 스위치가 중계해야 하므로 Type 1(CFG1).
 *
 * 창을 매번 다시 쓰기 때문에 설정 접근끼리는 직렬화가 필요하지만, PCI 코어가
 * 설정 접근을 pci_lock 아래에서 수행하므로 여기서 따로 잠그지 않는다.
 *
 * 실행 컨텍스트: 열거 및 이후 설정 접근의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_generic_config_read/write → ops->map_bus → [이 함수]
 *     → dw_pcie_link_up → dw_pcie_prog_outbound_atu
 */
static void __iomem *dw_pcie_other_conf_map_bus(struct pci_bus *bus,
						unsigned int devfn, int where)
{
	/* NVMe: 하위 버스 config 공간 버스 매핑, NVMe 장치의 PCIe config 접근 경로 설정 */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe config 트랜잭션 경로 */
	int type, ret;					/* NVMe: ATU 타입/반환값, NVMe config 경로 설정 */
	u32 busdev;					/* NVMe: BDF 조합, NVMe 장치/함수 선택 */

	/*
	 * Checking whether the link is up here is a last line of defense
	 * against platforms that forward errors on the system bus as
	 * SError upon PCI configuration transactions issued when the link
	 * is down. This check is racy by definition and does not stop
	 * the system from triggering an SError if the link goes down
	 * after this check is performed.
	 */
	if (!dw_pcie_link_up(pci))
		return NULL;	/* NVMe: 링크 다운 시 NULL, NVMe config 접근 방지(SError 방지) */

	busdev = PCIE_ATU_BUS(bus->number) | PCIE_ATU_DEV(PCI_SLOT(devfn)) |
		 PCIE_ATU_FUNC(PCI_FUNC(devfn));	/* NVMe: BDF를 ATU 포맷으로, NVMe 대상 장치/함수 지정 */

	if (pci_is_root_bus(bus->parent))
		type = PCIE_ATU_TYPE_CFG0;	/* NVMe: 루트 버스 직속은 type 0, NVMe 직접 연결 장치 */
	else
		type = PCIE_ATU_TYPE_CFG1;	/* NVMe: 다단 버스는 type 1, NVMe 스위치 하위 장치 */

	atu.type = type;				/* NVMe: ATU 타입 설정, NVMe config 트랜잭션 유형 */
	atu.parent_bus_addr = pp->cfg0_base - pci->parent_bus_offset;	/* NVMe: CPU config 주소, NVMe config 공간 시작 */
	atu.pci_addr = busdev;							/* NVMe: PCI BDF 주소, NVMe 대상 장치 */
	atu.size = pp->cfg0_size;						/* NVMe: config 공간 크기, NVMe config 접근 범위 */

	ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: outbound ATU 프로그램, NVMe config 접근 경로 설정 */
	if (ret)
		return NULL;	/* NVMe: ATU 설정 실패, NVMe config 접근 불가 */

	return pp->va_cfg0_base + where;	/* NVMe: 가상 config 주소 반환, NVMe 레지스터 read/write */
}

/* [한국어]
 * dw_pcie_rd_other_conf - 하위 버스 설정공간을 읽고, 필요하면 IO 창을 복구한다
 *
 * @bus, @devfn, @where, @size: 표준 설정 읽기 인자.
 * @val: 읽은 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 오류 코드.
 *
 * 실제 읽기는 pci_generic_config_read 가 map_bus 를 거쳐 처리한다. 이 함수가
 * 따로 존재하는 이유는 그 **뒤처리** 때문이다.
 *
 * cfg0_io_shared 는 iATU 창이 모자라 설정용 창과 IO 용 창을 하나로 겸용하고
 * 있다는 표시다(dw_pcie_iatu_setup 이 세운다). 방금 map_bus 가 그 창을
 * 설정 접근용으로 덮어썼으므로, 여기서 IO 용으로 되돌려 놓지 않으면 다음
 * 포트 IO 접근이 엉뚱한 곳으로 간다. 그래서 읽기가 끝난 직후 창을
 * PCIE_ATU_TYPE_IO 로 다시 프로그래밍한다.
 *
 * 되돌리기에 실패하면 읽기 자체는 성공했더라도 PCIBIOS_SET_FAILED 를 돌려
 * 준다 -- 창 상태가 어긋난 채로 진행하는 것보다 오류를 알리는 편이 안전하다.
 *
 * 실행 컨텍스트: 설정 접근의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_read_config_* → ops->read → [이 함수] → pci_generic_config_read
 *     → dw_pcie_prog_outbound_atu(IO 창 복구)
 */
static int dw_pcie_rd_other_conf(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 *val)
{
	/* NVMe: 하위 버스 config read, NVMe 장치의 PCIe capability/레지스터 읽기 */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe IO 공간 복원 */
	int ret;					/* NVMe: 반환값, NVMe config read 결과 */

	ret = pci_generic_config_read(bus, devfn, where, size, val);	/* NVMe: 표준 PCI config read, NVMe 장치 레지스터 읽기 */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;	/* NVMe: read 실패, NVMe 레지스터 읽기 오류 */

	if (pp->cfg0_io_shared) {
		atu.type = PCIE_ATU_TYPE_IO;	/* NVMe: IO 공간 ATU 타입, NVMe IO 복원 */
		atu.parent_bus_addr = pp->io_base - pci->parent_bus_offset;	/* NVMe: CPU IO 주소, NVMe IO 매핑 */
		atu.pci_addr = pp->io_bus_addr;	/* NVMe: 버스 IO 주소, NVMe IO 트랜잭션 */
		atu.size = pp->io_size;		/* NVMe: IO 공간 크기, NVMe IO 범위 */

		ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: IO ATU 복원, NVMe config 후 IO 공간 복구 */
		if (ret)
			return PCIBIOS_SET_FAILED;	/* NVMe: IO 복원 실패, NVMe IO 접근 불가 */
	}

	return PCIBIOS_SUCCESSFUL;	/* NVMe: config read 성공, NVMe 레지스터 값 획득 */
}

/* [한국어]
 * dw_pcie_wr_other_conf - 하위 버스 설정공간에 쓰고, 필요하면 IO 창을 복구한다
 *
 * @bus, @devfn, @where, @size, @val: 표준 설정 쓰기 인자.
 * @return: PCIBIOS_SUCCESSFUL 또는 오류 코드.
 *
 * dw_pcie_rd_other_conf 와 대칭이다. pci_generic_config_write 가 map_bus 를
 * 거쳐 실제 쓰기를 하고, cfg0_io_shared 일 때 겸용 창을 IO 용으로 되돌린다.
 *
 * 읽기와 쓰기 양쪽에 같은 뒤처리를 둔 이유: map_bus 는 읽기든 쓰기든 창을
 * 설정용으로 덮어쓰므로, 어느 쪽으로 들어와도 되돌려야 한다.
 *
 * 실행 컨텍스트: 설정 접근의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_write_config_* → ops->write → [이 함수] → pci_generic_config_write
 *     → dw_pcie_prog_outbound_atu(IO 창 복구)
 */
static int dw_pcie_wr_other_conf(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 val)
{
	/* NVMe: 하위 버스 config write, NVMe 장치의 PCIe 레지스터 쓰기(예: BAR, command) */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe IO 공간 복원 */
	int ret;					/* NVMe: 반환값, NVMe config write 결과 */

	ret = pci_generic_config_write(bus, devfn, where, size, val);	/* NVMe: 표준 PCI config write, NVMe 장치 레지스터 쓰기 */
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;	/* NVMe: write 실패, NVMe 레지스터 쓰기 오류 */

	if (pp->cfg0_io_shared) {
		atu.type = PCIE_ATU_TYPE_IO;	/* NVMe: IO 공간 ATU 타입, NVMe IO 복원 */
		atu.parent_bus_addr = pp->io_base - pci->parent_bus_offset;	/* NVMe: CPU IO 주소, NVMe IO 매핑 */
		atu.pci_addr = pp->io_bus_addr;	/* NVMe: 버스 IO 주소, NVMe IO 트랜잭션 */
		atu.size = pp->io_size;		/* NVMe: IO 공간 크기, NVMe IO 범위 */

		ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: IO ATU 복원, NVMe config 후 IO 공간 복구 */
		if (ret)
			return PCIBIOS_SET_FAILED;	/* NVMe: IO 복원 실패, NVMe IO 접근 불가 */
	}

	return PCIBIOS_SUCCESSFUL;	/* NVMe: config write 성공, NVMe 레지스터 갱신 완료 */
}

static struct pci_ops dw_child_pcie_ops = {
	.map_bus = dw_pcie_other_conf_map_bus,	/* NVMe: 하위 버스 매핑, NVMe 장치 config 접근 경로 */
	.read = dw_pcie_rd_other_conf,		/* NVMe: 하위 버스 read, NVMe 레지스터 읽기 */
	.write = dw_pcie_wr_other_conf,		/* NVMe: 하위 버스 write, NVMe 레지스터 쓰기 */
};

/* [한국어]
 * dw_pcie_own_conf_map_bus - 루트 포트 자신의 설정공간을 DBI 로 바로 매핑한다
 *
 * @bus: 루트 버스.
 * @devfn: 장치/함수. 슬롯 0 만 유효하다.
 * @where: 설정공간 오프셋.
 * @return: dbi_base + where, 또는 슬롯 0 이 아니면 NULL.
 *
 * 루트 포트 자신은 링크 건너편이 아니라 IP 내부에 있다. DWC 는 그 설정공간을
 * DBI(Data Bus Interface) 창에 그대로 노출하므로, 설정 TLP 를 만들 필요 없이
 * dbi_base + where 를 읽고 쓰면 된다 -- iATU 도 링크도 개입하지 않는다.
 *
 * PCI_SLOT(devfn) > 0 을 NULL 로 막는 이유: 루트 버스에는 이 루트 포트 하나만
 * 존재한다. 막지 않으면 열거가 같은 DBI 영역을 32개 장치로 착각해 유령
 * 장치를 만들어 낸다.
 *
 * EXPORT 되어 있어, 브리지 ops 를 자체적으로 구성하는 SoC 드라이버도 루트
 * 포트 접근만큼은 이 함수를 그대로 빌려 쓴다.
 *
 * 실행 컨텍스트: 열거 및 설정 접근의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_generic_config_read/write → ops->map_bus → [이 함수]
 */
void __iomem *dw_pcie_own_conf_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	/* NVMe: RC 자체 config 공간 매핑, NVMe 루트 포트 capability 접근 */
	struct dw_pcie_rp *pp = bus->sysdata;	/* NVMe: RC 포트, NVMe 연결된 루트 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 DBI 접근 */

	if (PCI_SLOT(devfn) > 0)
		return NULL;	/* NVMe: RC는 devfn 0만 유효, NVMe 루트 포트 외 접근 거부 */

	return pci->dbi_base + where;	/* NVMe: DBI 레지스터 주소 반환, NVMe 루트 포트 설정 */
}
EXPORT_SYMBOL_GPL(dw_pcie_own_conf_map_bus);

/* [한국어]
 * dw_pcie_ecam_conf_map_bus - ECAM 매핑을 쓰되 루트 버스만 DBI 로 우회시킨다
 *
 * @bus: 접근할 버스. sysdata 는 pci_config_window 다(ECAM 경로이므로).
 * @devfn: 장치/함수 번호.
 * @where: 설정공간 오프셋.
 * @return: 접근할 가상 주소, 또는 NULL.
 *
 * 버스 번호가 0 보다 크면 공용 pci_ecam_map_bus() 에 그대로 넘긴다. ECAM 은
 * 주소 산술만으로 위치가 정해지므로 추가로 할 일이 없다.
 *
 * 버스 0(루트 버스)만 예외다. 루트 포트 자신의 설정공간은 ECAM 창이 아니라
 * DBI 로 접근해야 하므로 dbi_base + where 를 돌려준다. 그래서
 * dw_pcie_config_ecam_iatu 도 창을 cfg0_base + 1MiB 부터 잡는다 -- 첫 1MiB
 * (버스 0의 몫)는 애초에 쓰지 않기 때문이다.
 *
 * pp 를 되찾는 경로가 다른 map_bus 들과 다르다. 여기서는 sysdata 가 pp 가
 * 아니라 pci_config_window 이므로, dw_pcie_create_ecam_window 호출부에서
 * cfg->priv 에 심어 둔 pp 를 꺼낸다.
 *
 * 슬롯 검사는 dw_pcie_own_conf_map_bus 와 같은 이유다.
 *
 * 실행 컨텍스트: 열거 및 설정 접근의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_generic_config_read/write → ops->map_bus → [이 함수]
 *     → pci_ecam_map_bus (버스 0 이 아닐 때)
 */
static void __iomem *dw_pcie_ecam_conf_map_bus(struct pci_bus *bus, unsigned int devfn, int where)
{
	/* NVMe: ECAM 모드 config 공간 매핑, NVMe 버스/장치 레지스터 접근 */
	struct pci_config_window *cfg = bus->sysdata;	/* NVMe: ECAM 윈도우, NVMe config 공간 매핑 */
	struct dw_pcie_rp *pp = cfg->priv;		/* NVMe: RC 포트, NVMe 연결된 포트 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 DBI 접근 */
	unsigned int busn = bus->number;		/* NVMe: 현재 버스 번호, NVMe 대상 버스 */

	if (busn > 0)
		return pci_ecam_map_bus(bus, devfn, where);	/* NVMe: bus > 0은 ECAM 사용, NVMe 다단 버스 장치 접근 */

	if (PCI_SLOT(devfn) > 0)
		return NULL;	/* NVMe: 루트 버스는 devfn 0만, NVMe 루트 포트 외 거부 */

	return pci->dbi_base + where;	/* NVMe: DBI로 루트 포트 접근, NVMe 루트 포트 설정 */
}

static struct pci_ops dw_pcie_ops = {
	.map_bus = dw_pcie_own_conf_map_bus,	/* NVMe: RC 자체 매핑, NVMe 루트 포트 config 접근 */
	.read = pci_generic_config_read,	/* NVMe: 표준 config read, NVMe 루트 포트 레지스터 읽기 */
	.write = pci_generic_config_write,	/* NVMe: 표준 config write, NVMe 루트 포트 레지스터 쓰기 */
};

static struct pci_ops dw_pcie_ecam_ops = {
	.map_bus = dw_pcie_ecam_conf_map_bus,	/* NVMe: ECAM 매핑, NVMe 버스/장치 config 접근 */
	.read = pci_generic_config_read,	/* NVMe: 표준 config read, NVMe 장치 레지스터 읽기 */
	.write = pci_generic_config_write,	/* NVMe: 표준 config write, NVMe 장치 레지스터 쓰기 */
};

/* [한국어]
 * dw_pcie_iatu_setup - 한정된 iATU 창을 MEM/IO/MSG/DMA 범위에 나눠 배분한다
 *
 * @pp: 이 루트 포트.
 * @return: 0 성공, -EINVAL 은 아웃바운드 창이 하나도 없음, -ENOMEM 은 창 부족,
 *          그 외 음수는 창 프로그래밍 실패값.
 *
 * iATU 창은 IP 합성 시점에 개수가 고정되는 희소 자원이다(보통 2~16개).
 * 이 함수는 그 예산을 DT 가 요구한 모든 범위에 배분하는 자리이고, 이 파일에서
 * 가장 실패하기 쉬운 지점이기도 하다.
 *
 * 시작 전에 모든 아웃바운드/인바운드 창을 명시적으로 끈다. 부트로더가 남긴
 * 설정이 그대로 살아 있으면 새 배분과 겹쳐 엉뚱한 주소로 트랜잭션이 새기
 * 때문이다.
 *
 * 시작 인덱스가 모드에 따라 다르다:
 *   - ECAM 모드: 창 0,1 을 설정 접근이 이미 가져갔으므로 2부터.
 *   - 기존 모드: 창 0 은 dw_pcie_other_conf_map_bus 가 매번 다시 겨누는
 *     설정 전용 창이므로 1부터.
 *
 * 아웃바운드 MEM: 브리지의 MEM 윈도를 순회한다. 창 하나가 덮을 수 있는 최대
 * 크기는 region_limit + 1 이므로, 그보다 큰 범위는 while 루프로 여러 창에
 * 쪼개 배분한다. msg_res 가 이 윈도에서 잘려 나갔다면 그만큼 크기를 빼야
 * 겹치지 않는다. 도중에 창이 떨어지면 -ENOMEM 이다.
 *
 * 아웃바운드 IO: 남은 창이 있으면 정상 배분한다. **없을 때의 처리가 갈린다** --
 *   - ECAM 모드에서는 설정 창을 겸용할 수 없으므로 곧바로 -ENOMEM.
 *   - 기존 모드에서는 cfg0_io_shared = true 로 표시하고 넘어간다. 그러면
 *     설정 전용 창 0 을 IO 와 번갈아 쓰게 되고, 그 뒤처리를
 *     dw_pcie_rd_other_conf / dw_pcie_wr_other_conf 가 맡는다.
 *
 * MSG: use_atu_msg 면 창 번호 하나를 예약만 해 둔다(msg_atu_index). 실제
 * 프로그래밍은 PME_Turn_Off 를 보낼 때 dw_pcie_pme_turn_off 가 한다.
 *
 * 인바운드: dma_ranges 를 순회하며 같은 방식으로 쪼개 배분한다. 방향이
 * 반대이므로 base 는 PCI 쪽에서 알아볼 범위, target 은 CPU 메모리다. 이것이
 * 없으면 엔드포인트의 DMA 가 시스템 메모리에 닿지 못한다.
 *
 * 실행 컨텍스트: 프로브 및 재개 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_setup_rc → [이 함수] → dw_pcie_disable_atu
 *     → dw_pcie_config_ecam_iatu → dw_pcie_prog_outbound_atu
 *       → dw_pcie_prog_inbound_atu
 */
static int dw_pcie_iatu_setup(struct dw_pcie_rp *pp)
{
	/* NVMe: iATU 인바운드/아웃바운드 윈도우 설정, NVMe BAR 매핑 및 DMA 메모리 공간 구성 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };		/* NVMe: outbound ATU 설정, NVMe MEM/IO/CFG 매핑 */
	struct resource_entry *entry;			/* NVMe: 리소스 엔트리, NVMe 메모리/DMA 범위 */
	int ob_iatu_index;				/* NVMe: outbound iATU 인덱스, NVMe 메모리/IO/MSG 할당 */
	int ib_iatu_index;				/* NVMe: inbound iATU 인덱스, NVMe DMA 메모리 매핑 */
	int i, ret;					/* NVMe: 루프 인덱스/반환값, NVMe ATU 설정 */

	if (!pci->num_ob_windows) {
		dev_err(pci->dev, "No outbound iATU found\n");	/* NVMe: outbound 윈도우 없음, NVMe BAR/IO 접근 불가 */
		return -EINVAL;	/* NVMe: 잘못된 상태, NVMe 초기화 실패 */
	}

	/*
	 * Ensure all out/inbound windows are disabled before proceeding with
	 * the MEM/IO (dma-)ranges setups.
	 */
	for (i = 0; i < pci->num_ob_windows; i++)
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_OB, i);	/* NVMe: outbound 윈도우 비활성화, NVMe 이전 매핑 클리어 */

	for (i = 0; i < pci->num_ib_windows; i++)
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_IB, i);	/* NVMe: inbound 윈도우 비활성화, NVMe 이전 DMA 매핑 클리어 */

	/*
	 * NOTE: For outbound address translation, outbound iATU at index 0 is
	 * reserved for CFG IOs (dw_pcie_other_conf_map_bus()), thus start at
	 * index 1.
	 *
	 * If using ECAM, outbound iATU at index 0 and index 1 is reserved for
	 * CFG IOs.
	 */
	if (pp->ecam_enabled) {
		ob_iatu_index = 2;	/* NVMe: ECAM 시 0,1은 CFG용, NVMe MEM/IO는 2부터 */
		ret = dw_pcie_config_ecam_iatu(pp);	/* NVMe: ECAM CFG iATU 설정, NVMe config 접근 경로 */
		if (ret) {
			dev_err(pci->dev, "Failed to configure iATU in ECAM mode\n");	/* NVMe: ECAM iATU 실패, NVMe config 접근 불가 */
			return ret;	/* NVMe: 초기화 실패, NVMe 열거 불가 */
		}
	} else {
		ob_iatu_index = 1;	/* NVMe: non-ECAM 시 0은 CFG용, NVMe MEM/IO는 1부터 */
	}

	resource_list_for_each_entry(entry, &pp->bridge->windows) {	/* NVMe: 호스트 브리지 윈도우 순회, NVMe BAR 메모리 공간 매핑 */
		resource_size_t res_size;	/* NVMe: 리소스 크기, NVMe BAR 매핑 크기 */

		if (resource_type(entry->res) != IORESOURCE_MEM)
			continue;	/* NVMe: MEM이 아니면 skip, NVMe IO는 별도 처리 */

		atu.type = PCIE_ATU_TYPE_MEM;	/* NVMe: MEM 타입 ATU, NVMe BAR 메모리 매핑 */
		atu.parent_bus_addr = entry->res->start - pci->parent_bus_offset;	/* NVMe: CPU 물리 주소, NVMe 호스트 메모리 공간 */
		atu.pci_addr = entry->res->start - entry->offset;	/* NVMe: PCI 버스 주소, NVMe 장치가 보는 BAR 주소 */

		/* Adjust iATU size if MSG TLP region was allocated before */
		if (pp->msg_res && pp->msg_res->parent == entry->res)
			res_size = resource_size(entry->res) -
					resource_size(pp->msg_res);	/* NVMe: MSG 영역 제외, NVMe PME TLP 주소는 BAR 할당에서 제외 */
		else
			res_size = resource_size(entry->res);	/* NVMe: 전체 리소스 크기, NVMe BAR 매핑 크기 */

		while (res_size > 0) {
			/*
			 * Return failure if we run out of windows in the
			 * middle. Otherwise, we would end up only partially
			 * mapping a single resource.
			 */
			if (ob_iatu_index >= pci->num_ob_windows) {
				dev_err(pci->dev, "Cannot add outbound window for region: %pr\n",
					entry->res);	/* NVMe: outbound 윈도우 고갈, NVMe BAR 일부 매핑 실패 */
				return -ENOMEM;	/* NVMe: 메모리/리소스 부족, NVMe 메모리 매핑 불완전 */
			}

			atu.index = ob_iatu_index;	/* NVMe: 현재 outbound 인덱스, NVMe MEM 윈도우 할당 */
			atu.size = MIN(pci->region_limit + 1, res_size);	/* NVMe: ATU 윈도우 크기, NVMe BAR 매핑 단위 */

			ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: outbound ATU 프로그램, NVMe 장치 메모리 접근 가능 */
			if (ret) {
				dev_err(pci->dev, "Failed to set MEM range %pr\n",
					entry->res);	/* NVMe: MEM ATU 설정 실패, NVMe BAR 접근 불가 */
				return ret;	/* NVMe: 초기화 실패, NVMe 메모리 매핑 실패 */
			}

			ob_iatu_index++;	/* NVMe: 다음 outbound 인덱스, NVMe 다음 MEM 세그먼트 */
			atu.parent_bus_addr += atu.size;	/* NVMe: 다음 CPU 주소, NVMe 다음 MEM 세그먼트 */
			atu.pci_addr += atu.size;		/* NVMe: 다음 PCI 주소, NVMe 다음 MEM 세그먼트 */
			res_size -= atu.size;			/* NVMe: 남은 크기, NVMe MEM 매핑 진행 */
		}
	}

	if (pp->io_size) {
		if (ob_iatu_index < pci->num_ob_windows) {
			atu.index = ob_iatu_index;	/* NVMe: IO용 outbound 인덱스, NVMe IO 공간 매핑 */
			atu.type = PCIE_ATU_TYPE_IO;	/* NVMe: IO 타입 ATU, NVMe IO 트랜잭션 */
			atu.parent_bus_addr = pp->io_base - pci->parent_bus_offset;	/* NVMe: CPU IO 주소, NVMe IO 매핑 */
			atu.pci_addr = pp->io_bus_addr;	/* NVMe: PCI IO 주소, NVMe 장치 IO 공간 */
			atu.size = pp->io_size;		/* NVMe: IO 공간 크기, NVMe IO 범위 */

			ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: IO ATU 프로그램, NVMe IO 접근 가능 */
			if (ret) {
				dev_err(pci->dev, "Failed to set IO range %pr\n",
					entry->res);	/* NVMe: IO ATU 설정 실패, NVMe IO 접근 불가 */
				return ret;	/* NVMe: 초기화 실패, NVMe IO 매핑 실패 */
			}
			ob_iatu_index++;	/* NVMe: 다음 outbound 인덱스, NVMe MSG/추가 공간 */
		} else {
			/*
			 * If there are not enough outbound windows to give I/O
			 * space its own iATU, the outbound iATU at index 0 will
			 * be shared between I/O space and CFG IOs, by
			 * temporarily reconfiguring the iATU to CFG space, in
			 * order to do a CFG IO, and then immediately restoring
			 * it to I/O space. This is only implemented when using
			 * dw_pcie_other_conf_map_bus(), which is not the case
			 * when using ECAM.
			 */
			if (pp->ecam_enabled) {
				dev_err(pci->dev, "Cannot add outbound window for I/O\n");
				return -ENOMEM;	/* NVMe: ECAM 시 IO 공유 불가, NVMe IO 매핑 실패 */
			}
			pp->cfg0_io_shared = true;	/* NVMe: CFG/IO 공유 플래그, NVMe config 후 IO 복원 */
		}
	}

	if (pp->use_atu_msg) {
		if (ob_iatu_index >= pci->num_ob_windows) {
			dev_err(pci->dev, "Cannot add outbound window for MSG TLP\n");	/* NVMe: MSG TLP 윈도우 부족, NVMe PME 메시지 전송 불가 */
			return -ENOMEM;	/* NVMe: 리소스 부족, NVMe 전원 관리 기능 제한 */
		}
		pp->msg_atu_index = ob_iatu_index++;	/* NVMe: MSG TLP 전용 ATU 인덱스, NVMe PME/PM 메시지 전송 */
	}

	ib_iatu_index = 0;	/* NVMe: inbound iATU 인덱스 0부터, NVMe DMA 메모리 매핑 */
	resource_list_for_each_entry(entry, &pp->bridge->dma_ranges) {	/* NVMe: dma-ranges 순회, NVMe 장치 DMA 주소 변환 설정 */
		resource_size_t res_start, res_size, window_size;	/* NVMe: DMA 범위 시작/크기/윈도우, NVMe DMA 메모리 공간 */

		if (resource_type(entry->res) != IORESOURCE_MEM)
			continue;	/* NVMe: MEM이 아니면 skip, NVMe DMA는 메모리 공간 */

		res_size = resource_size(entry->res);	/* NVMe: DMA 범위 크기, NVMe DMA 가능 메모리 크기 */
		res_start = entry->res->start;		/* NVMe: DMA 범위 시작, NVMe DMA 물리 주소 시작 */
		while (res_size > 0) {
			/*
			 * Return failure if we run out of windows in the
			 * middle. Otherwise, we would end up only partially
			 * mapping a single resource.
			 */
			if (ib_iatu_index >= pci->num_ib_windows) {
				dev_err(pci->dev, "Cannot add inbound window for region: %pr\n",
					entry->res);	/* NVMe: inbound 윈도우 고갈, NVMe DMA 메모리 일부 매핑 실패 */
				return -ENOMEM;	/* NVMe: 리소스 부족, NVMe DMA 매핑 불완전(IOMMU/오류 가능) */
			}

			window_size = MIN(pci->region_limit + 1, res_size);	/* NVMe: inbound 윈도우 크기, NVMe DMA 매핑 단위 */
			ret = dw_pcie_prog_inbound_atu(pci, ib_iatu_index,
						       PCIE_ATU_TYPE_MEM, res_start,
						       res_start - entry->offset, window_size);	/* NVMe: inbound ATU 프로그램, NVMe 장치 DMA 주소->CPU 주소 변환 */
			if (ret) {
				dev_err(pci->dev, "Failed to set DMA range %pr\n",
					entry->res);	/* NVMe: DMA ATU 설정 실패, NVMe DMA 불가 */
				return ret;	/* NVMe: 초기화 실패, NVMe DMA 메모리 매핑 실패 */
			}

			ib_iatu_index++;	/* NVMe: 다음 inbound 인덱스, NVMe 다음 DMA 세그먼트 */
			res_start += window_size;	/* NVMe: 다음 DMA 시작 주소, NVMe 다음 세그먼트 */
			res_size -= window_size;	/* NVMe: 남은 DMA 크기, NVMe DMA 매핑 진행 */
		}
	}

	return 0;	/* NVMe: iATU 설정 완료, NVMe 메모리/IO/DMA 매핑 완료 */
}

/* [한국어]
 * dw_pcie_program_presets - 한 속도 등급의 레인별 이퀄라이제이션 프리셋을 써 넣는다
 *
 * @pp: 이 루트 포트. DT 에서 읽어 둔 pp->presets 를 쓴다.
 * @speed: 프로그래밍할 속도 등급(8/16/32/64 GT/s).
 * @return: 없음. 프리셋이 없거나 능력 구조가 없으면 조용히 돌아간다.
 *
 * 8GT/s(Gen3) 이상에서는 링크 학습 때 송신단 이퀄라이저 계수를 맞춰야 하고,
 * 보드 배선이 나쁘면 하드웨어가 스스로 찾은 값으로는 링크가 불안정할 수 있다.
 * 그래서 보드 설계자가 DT 에 레인별 프리셋을 적어 두고, 이 함수가 그것을
 * 설정공간의 해당 능력 구조에 써 넣는다.
 *
 * 속도마다 능력 구조와 레지스터 배치가 다르다:
 *   - 8GT/s: Secondary PCI Express 확장 능력(PCI_EXT_CAP_ID_SECPCI) 안의
 *     PCI_SECPCI_LE_CTRL. 레인당 **2바이트** -- 상하류 프리셋을 각각 담기
 *     때문이다(lane_reg_size = 0x2).
 *   - 16/32/64GT/s: 각 속도의 전용 Physical Layer 확장 능력. 레인당 1바이트.
 *
 * presets[0] == PCI_EQ_RESV 는 DT 가 이 속도에 대해 프리셋을 주지 않았다는
 * 표시이므로 건너뛴다. 능력 구조를 못 찾아도 마찬가지다.
 *
 * 쓰기가 dw_pcie_writeb_dbi(바이트 단위)인 것은 레인 하나가 1~2바이트를
 * 차지하기 때문이고, 반복 횟수가 num_lanes * lane_reg_size 인 것도 같은
 * 이유다. 그래서 dw_pcie_host_init 이 이 호출 전에 num_lanes 를 반드시
 * 확정해 둔다.
 *
 * 이 함수는 DBI 읽기전용 쓰기 허용(dw_pcie_dbi_ro_wr_en) 구간 안에서 불린다 --
 * 이 레지스터들은 평소 읽기 전용이기 때문이다.
 *
 * 실행 컨텍스트: 프로브 및 재개 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_setup_rc → dw_pcie_config_presets → [이 함수]
 *     → dw_pcie_find_ext_capability → dw_pcie_writeb_dbi
 */
static void dw_pcie_program_presets(struct dw_pcie_rp *pp, enum pci_bus_speed speed)
{
	/* NVMe: 링크 equalization preset 값 프로그램, NVMe Gen3/4/5/7 링크 품질/안정성 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	u8 lane_eq_offset, lane_reg_size, cap_id;	/* NVMe: EQ 레지스터 오프셋/크기/cap ID, NVMe 링크 속도별 설정 */
	u8 *presets;					/* NVMe: preset 배열, NVMe equalization 값 */
	u32 cap;					/* NVMe: capability 오프셋, NVMe EQ capability 위치 */
	int i;						/* NVMe: 레인 인덱스, NVMe 다중 레인 preset 설정 */

	if (speed == PCIE_SPEED_8_0GT) {
		presets = (u8 *)pp->presets.eq_presets_8gts;	/* NVMe: 8GT/s preset, NVMe Gen3 링크 */
		lane_eq_offset =  PCI_SECPCI_LE_CTRL;		/* NVMe: 8GT/s EQ 레지스터, NVMe Gen3 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_SECPCI;			/* NVMe: Secondary PCI capability, NVMe Gen3 EQ */
		/* For data rate of 8 GT/S each lane equalization control is 16bits wide*/
		lane_reg_size = 0x2;	/* NVMe: 8GT/s 레인당 2바이트, NVMe Gen3 preset 크기 */
	} else if (speed == PCIE_SPEED_16_0GT) {
		presets = pp->presets.eq_presets_Ngts[EQ_PRESET_TYPE_16GTS - 1];	/* NVMe: 16GT/s preset, NVMe Gen4 링크 */
		lane_eq_offset = PCI_PL_16GT_LE_CTRL;					/* NVMe: 16GT/s EQ 레지스터, NVMe Gen4 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_PL_16GT;					/* NVMe: 16GT PHY capability, NVMe Gen4 EQ */
		lane_reg_size = 0x1;	/* NVMe: 16GT/s 레인당 1바이트, NVMe Gen4 preset 크기 */
	} else if (speed == PCIE_SPEED_32_0GT) {
		presets =  pp->presets.eq_presets_Ngts[EQ_PRESET_TYPE_32GTS - 1];	/* NVMe: 32GT/s preset, NVMe Gen5 링크 */
		lane_eq_offset = PCI_PL_32GT_LE_CTRL;					/* NVMe: 32GT/s EQ 레지스터, NVMe Gen5 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_PL_32GT;					/* NVMe: 32GT PHY capability, NVMe Gen5 EQ */
		lane_reg_size = 0x1;	/* NVMe: 32GT/s 레인당 1바이트, NVMe Gen5 preset 크기 */
	} else if (speed == PCIE_SPEED_64_0GT) {
		presets =  pp->presets.eq_presets_Ngts[EQ_PRESET_TYPE_64GTS - 1];	/* NVMe: 64GT/s preset, NVMe Gen7 링크 */
		lane_eq_offset = PCI_PL_64GT_LE_CTRL;					/* NVMe: 64GT/s EQ 레지스터, NVMe Gen7 레인 제어 */
		cap_id = PCI_EXT_CAP_ID_PL_64GT;					/* NVMe: 64GT PHY capability, NVMe Gen7 EQ */
		lane_reg_size = 0x1;	/* NVMe: 64GT/s 레인당 1바이트, NVMe Gen7 preset 크기 */
	} else {
		return;	/* NVMe: 지원하지 않는 속도, NVMe preset 설정 불필요 */
	}

	if (presets[0] == PCI_EQ_RESV)
		return;	/* NVMe: 예약 값이면 skip, NVMe platform preset 미지정 */

	cap = dw_pcie_find_ext_capability(pci, cap_id);	/* NVMe: EQ extended capability 탐색, NVMe 링크 품질 설정 위치 */
	if (!cap)
		return;	/* NVMe: capability 없음, NVMe preset 설정 불가 */

	/*
	 * Write preset values to the registers byte-by-byte for the given
	 * number of lanes and register size.
	 */
	for (i = 0; i < pci->num_lanes * lane_reg_size; i++)
		dw_pcie_writeb_dbi(pci, cap + lane_eq_offset + i, presets[i]);	/* NVMe: 레인별 preset 쓰기, NVMe 링크 equalization 적용 */
}

/* [한국어]
 * dw_pcie_config_presets - 링크가 낼 수 있는 모든 속도 등급의 프리셋을 차례로 넣는다
 *
 * @pp: 이 루트 포트.
 * @return: 없음.
 *
 * 링크는 학습 과정에서 속도를 단계적으로 올린다(2.5 → 5 → 8 → 16 → ...).
 * 그래서 최대 속도가 32GT/s 라면 8, 16, 32 모두의 프리셋이 필요하다 --
 * 중간 단계를 건너뛰고 최고 속도로 바로 가지 않기 때문이다. if 문을 else 로
 * 잇지 않고 >= 비교를 네 번 나열한 것이 바로 그 누적 적용이다.
 *
 * pcie_get_link_speed(pci->max_link_speed) 로 DT 가 지정한 최대 속도를 열거형
 * 등급으로 바꾼 뒤, 8GT/s 미만이면 어떤 분기에도 들어가지 않는다 -- 프리셋
 * 개념 자체가 Gen3 부터이기 때문이다.
 *
 * 실행 컨텍스트: 프로브 및 재개 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_setup_rc → [이 함수] → dw_pcie_program_presets (최대 4회)
 */
static void dw_pcie_config_presets(struct dw_pcie_rp *pp)
{
	/* NVMe: 지원하는 속도별 preset 구성, NVMe 링크 협상/품질 최적화 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	enum pci_bus_speed speed = pcie_get_link_speed(pci->max_link_speed);	/* NVMe: 최대 링크 속도, NVMe Gen3/4/5/7 지원 */

	/*
	 * Lane equalization settings need to be applied for all data rates the
	 * controller supports and for all supported lanes.
	 */

	if (speed >= PCIE_SPEED_8_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_8_0GT);	/* NVMe: Gen3(8GT/s) preset, NVMe Gen3 SSD 링크 안정화 */

	if (speed >= PCIE_SPEED_16_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_16_0GT);	/* NVMe: Gen4(16GT/s) preset, NVMe Gen4 SSD 링크 안정화 */

	if (speed >= PCIE_SPEED_32_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_32_0GT);	/* NVMe: Gen5(32GT/s) preset, NVMe Gen5 SSD 링크 안정화 */

	if (speed >= PCIE_SPEED_64_0GT)
		dw_pcie_program_presets(pp, PCIE_SPEED_64_0GT);	/* NVMe: Gen7(64GT/s) preset, NVMe Gen7 SSD 링크 안정화 */
}

/* [한국어]
 * dw_pcie_setup_rc - 루트 컴플렉스의 설정공간과 주소 변환을 실제로 프로그래밍한다
 *
 * @pp: 이 루트 포트.
 * @return: 0 성공, 음수는 dw_pcie_iatu_setup 실패값.
 *
 * 프로브에서 한 번, 재개(resume)에서 다시 불린다. 그래서 "하드웨어를 처음
 * 상태로 만드는 데 필요한 모든 쓰기" 가 이 한 함수에 모여 있다.
 *
 * 전체를 dw_pcie_dbi_ro_wr_en / _dis 로 감싸는 것이 전제다. 여기서 건드리는
 * 상당수(클래스 코드, 프리셋, 링크 제어)가 규약상 읽기 전용이라, DWC 의
 * 전용 스위치로 잠깐 쓰기를 허용해야 한다.
 *
 * 순서와 의도:
 *  1. dw_pcie_setup -- IP 공통 초기화(레인 수, 속도 등).
 *  2. dw_pcie_msi_init -- iMSI-RX 레지스터. 재개 때 마스크 상태가 복원된다.
 *  3. BAR0/BAR1 에 0x4/0x0 -- 루트 포트를 64비트 메모리 BAR 로 선언해 둔다
 *     (0x4 = 64비트 표시 비트). 뒤에서 다시 0 으로 지운다.
 *  4. PCI_INTERRUPT_LINE 의 상위 절반을 0x0100 으로 -- Interrupt Pin 을
 *     INTA(1)로 고정한다. 하위 8비트(Interrupt Line)는 건드리지 않는다.
 *  5. PCI_PRIMARY_BUS 를 0x00ff0100 으로 -- primary=0, secondary=1,
 *     subordinate=0xff. 열거가 시작될 수 있는 초기 버스 구간이다.
 *  6. PCI_COMMAND 하위 절반에 IO|MEMORY|MASTER|SERR -- 루트 포트가 트랜잭션을
 *     주고받고 시스템 오류를 보고할 수 있게 연다.
 *  7. dw_pcie_hide_unsupported_l1ss -- 지원하지 않는 L1 하위상태 능력을 가려
 *     상대가 쓸 수 없게 한다.
 *  8. dw_pcie_config_presets -- Gen3 이상 이퀄라이제이션 프리셋.
 *  9. iATU 배분: **child_ops 가 걸린 경우 또는 ECAM 모드일 때만** 부른다.
 *     자체 설정 접근 방식을 쓰는 SoC 는 창 배분도 직접 하기 때문이다.
 * 10. BAR0 을 0 으로 되돌린다 -- 루트 포트는 실제 BAR 를 노출하지 않는다.
 *     3번에서 잠깐 세웠던 것은 IP 내부 상태를 정해진 값으로 만들기 위한 것이다.
 * 11. 클래스 코드를 PCI_CLASS_BRIDGE_PCI 로 -- 그래야 커널이 이 함수를
 *     브리지로 인식해 하위 버스를 열거한다.
 * 12. PORT_LOGIC_SPEED_CHANGE -- 링크가 올라온 뒤 목표 속도로 협상을 다시
 *     시도하게 하는 트리거.
 * 13. 마지막으로, 내장 iMSI-RX 를 쓰면서 keep_rp_msi_en 이 아니면 루트 포트
 *     자신의 MSI/MSI-X 능력을 설정공간에서 **제거한다**. 루트 포트가 스스로
 *     MSI 를 받는 것처럼 보이면 커널이 그쪽으로 벡터를 배정해 iMSI-RX 경로와
 *     충돌하기 때문이다.
 *
 * 실행 컨텍스트: 프로브 및 재개 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_host_init / dw_pcie_resume_noirq → [이 함수]
 *     → dw_pcie_setup → dw_pcie_msi_init → dw_pcie_config_presets
 *       → dw_pcie_iatu_setup → dw_pcie_remove_capability
 */
int dw_pcie_setup_rc(struct dw_pcie_rp *pp)
{
	/* NVMe: RC 모드 레지스터 설정, NVMe SSD를 위한 PCIe 루트 포트 구성 */
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);	/* NVMe: dw_pcie, NVMe 호스트 레지스터 접근 */
	u32 val;					/* NVMe: 레지스터 값, NVMe RC 레지스터 구성 */
	int ret;					/* NVMe: 반환값, NVMe RC 설정 결과 */

	/*
	 * Enable DBI read-only registers for writing/updating configuration.
	 * Write permission gets disabled towards the end of this function.
	 */
	dw_pcie_dbi_ro_wr_en(pci);	/* NVMe: DBI 쓰기 가능, NVMe RC 레지스터 수정 */

	dw_pcie_setup(pci);	/* NVMe: DesignWave 공통 설정, NVMe 호스트 기본 구성 */

	dw_pcie_msi_init(pp);	/* NVMe: MSI 하드웨어 초기화, NVMe MSI TLP 수신 준비 */

	/* Setup RC BARs */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 0x00000004);	/* NVMe: BAR0 32-bit MEM, NVMe 루트 포트 BAR 초기화 */
	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_1, 0x00000000);	/* NVMe: BAR1 0, NVMe 루트 포트 BAR 초기화 */

	/* Setup interrupt pins */
	val = dw_pcie_readl_dbi(pci, PCI_INTERRUPT_LINE);	/* NVMe: INTERRUPT_LINE 읽기, NVMe INTx 라우팅 설정 */
	val &= 0xffff00ff;	/* NVMe: interrupt pin 필드 클리어, NVMe INTx pin 초기화 */
	val |= 0x00000100;	/* NVMe: INTA# 설정, NVMe 루트 포트 INTx pin */
	dw_pcie_writel_dbi(pci, PCI_INTERRUPT_LINE, val);	/* NVMe: interrupt pin 레지스터 쓰기, NVMe INTx 라우팅 */

	/* Setup bus numbers */
	val = dw_pcie_readl_dbi(pci, PCI_PRIMARY_BUS);	/* NVMe: PRIMARY_BUS 읽기, NVMe 버스 번호 설정 */
	val &= 0xff000000;	/* NVMe: 하위 버스 번호 필드 클리어, NVMe 버스 범위 초기화 */
	val |= 0x00ff0100;	/* NVMe: primary=0, secondary=1, subordinate=255, NVMe 버스 트리 */
	dw_pcie_writel_dbi(pci, PCI_PRIMARY_BUS, val);	/* NVMe: 버스 번호 쓰기, NVMe PCIe 버스 열거 범위 */

	/* Setup command register */
	val = dw_pcie_readl_dbi(pci, PCI_COMMAND);	/* NVMe: COMMAND 읽기, NVMe 루트 포트 명령 설정 */
	val &= 0xffff0000;	/* NVMe: 하위 16비트 클리어, NVMe command 초기화 */
	val |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		PCI_COMMAND_MASTER | PCI_COMMAND_SERR;	/* NVMe: IO/MEM/Bus Master/SERR 활성화, NVMe BAR/IO/DMA/에러 허용 */
	dw_pcie_writel_dbi(pci, PCI_COMMAND, val);	/* NVMe: command 레지스터 쓰기, NVMe 루트 포트 트랜잭션 활성화 */

	dw_pcie_hide_unsupported_l1ss(pci);	/* NVMe: 미지원 L1SS 숨김, NVMe ASPM/전원 관리 capability 정리 */

	dw_pcie_config_presets(pp);	/* NVMe: equalization preset 구성, NVMe 링크 품질 최적화 */
	/*
	 * If the platform provides its own child bus config accesses, it means
	 * the platform uses its own address translation component rather than
	 * ATU, so we should not program the ATU here.
	 */
	if (pp->bridge->child_ops == &dw_child_pcie_ops || pp->ecam_enabled) {
		ret = dw_pcie_iatu_setup(pp);	/* NVMe: iATU 설정, NVMe 메모리/IO/DMA 매핑 */
		if (ret)
			return ret;	/* NVMe: iATU 설정 실패, NVMe 메모리/DMA 불가 */
	}

	dw_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 0);	/* NVMe: BAR0 비활성화, NVMe 루트 포트 BAR 클리어 */

	/* Program correct class for RC */
	dw_pcie_writew_dbi(pci, PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_PCI);	/* NVMe: PCI-PCI bridge 클래스, NVMe 루트 포트 클래스 설정 */

	val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);	/* NVMe: 링크 폭/속도 제어 읽기, NVMe 링크 협상 제어 */
	val |= PORT_LOGIC_SPEED_CHANGE;	/* NVMe: 속도 변경 요청, NVMe Gen3/4/5/7 협상 */
	dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);	/* NVMe: 링크 속도 변경 설정, NVMe 최대 속도로 협상 */

	dw_pcie_dbi_ro_wr_dis(pci);	/* NVMe: DBI 쓰기 금지, NVMe RC 레지스터 보호 */

	/*
	 * The iMSI-RX module does not support receiving MSI or MSI-X generated
	 * by the Root Port. If iMSI-RX is used as the MSI controller, remove
	 * the MSI and MSI-X capabilities of the Root Port to allow the drivers
	 * to fall back to INTx instead.
	 */
	if (pp->use_imsi_rx && !pp->keep_rp_msi_en) {
		dw_pcie_remove_capability(pci, PCI_CAP_ID_MSI);	/* NVMe: 루트 포트 MSI capability 제거, NVMe RP INTx 폴백 */
		dw_pcie_remove_capability(pci, PCI_CAP_ID_MSIX);	/* NVMe: 루트 포트 MSI-X capability 제거, NVMe RP INTx 폴백 */
	}

	return 0;	/* NVMe: RC 설정 완료, NVMe PCIe 루트 포트 준비 */
}
EXPORT_SYMBOL_GPL(dw_pcie_setup_rc);

/* [한국어]
 * dw_pcie_pme_turn_off - iATU 의 MSG 창을 통해 PME_Turn_Off 를 브로드캐스트한다
 *
 * @pci: 이 DWC 인스턴스.
 * @return: 0 성공, -ENOSPC 는 MSG 용 창이나 주소가 준비되지 않음,
 *          -ENOMEM 은 ioremap 실패, 그 외는 창 프로그래밍 실패값.
 *
 * 서스펜드 전에 하위 장치들에게 "곧 전원을 내리니 정리하라" 고 알리는
 * PCIe 메시지가 PME_Turn_Off 다. DWC 는 이것을 위한 전용 레지스터를 두지
 * 않고, **주소 창에 대한 쓰기를 MSG TLP 로 바꾸는** iATU 기능으로 보낸다.
 *
 * 그래서 전제 조건이 둘이다: 예약된 창 번호가 실제 창 개수 안에 있어야 하고
 * (msg_atu_index), 쓸 주소 범위가 확보되어 있어야 한다(msg_res). 둘 중
 * 하나라도 없으면 -ENOSPC 이고, 호출자는 이 SoC 가 이 방식을 못 쓴다고 보고
 * 서스펜드를 접는다.
 *
 * 창을 프로그래밍할 때 담기는 것:
 *   - code = PCIE_MSG_CODE_PME_TURN_OFF -- 어떤 메시지인지.
 *   - routing = PCIE_MSG_TYPE_R_BC -- 브로드캐스트. 하위 전체에 뿌린다.
 *   - type = PCIE_ATU_TYPE_MSG -- 이 창의 쓰기를 MSG TLP 로 바꾸라는 지정.
 *
 * 그 다음 ioremap 으로 그 범위를 잠깐 매핑하고 writel(0, mem) 한다. **쓰는
 * 값 자체는 의미가 없다** -- 쓰기 행위가 곧 메시지 발송이고, 페이로드 없는
 * 메시지이기 때문이다. 보내고 나면 바로 iounmap 한다. 서스펜드 경로에서
 * 한 번뿐이므로 매핑을 들고 있을 이유가 없다.
 *
 * 실행 컨텍스트: 서스펜드(noirq 단계)의 프로세스 문맥.
 *
 * 호출 체인:
 *   dw_pcie_suspend_noirq → [이 함수] → dw_pcie_prog_outbound_atu
 *     → ioremap → writel → iounmap
 */
static int dw_pcie_pme_turn_off(struct dw_pcie *pci)
{
	/* NVMe: PME_Turn_Off 메시지 전송, NVMe 장치 절전 상태 진입 유도 */
	struct dw_pcie_ob_atu_cfg atu = { 0 };	/* NVMe: outbound ATU 설정, NVMe PME MSG TLP 경로 */
	void __iomem *mem;			/* NVMe: MSG 영역 가상 주소, NVMe PME TLP 전송 주소 */
	int ret;				/* NVMe: 반환값, NVMe PME 전송 결과 */

	if (pci->num_ob_windows <= pci->pp.msg_atu_index)
		return -ENOSPC;	/* NVMe: MSG TLP ATU 없음, NVMe PME 전송 불가 */

	if (!pci->pp.msg_res)
		return -ENOSPC;	/* NVMe: MSG 리소스 없음, NVMe PME 전송 불가 */

	atu.code = PCIE_MSG_CODE_PME_TURN_OFF;	/* NVMe: PME_Turn_Off 메시지 코드, NVMe 절전 메시지 */
	atu.routing = PCIE_MSG_TYPE_R_BC;	/* NVMe: broadcast 라우팅, NVMe 모든 장치에 PME 전송 */
	atu.type = PCIE_ATU_TYPE_MSG;		/* NVMe: MSG TLP 타입, NVMe PME 메시지 TLP */
	atu.size = resource_size(pci->pp.msg_res);	/* NVMe: MSG 영역 크기, NVMe PME TLP 전송 범위 */
	atu.index = pci->pp.msg_atu_index;		/* NVMe: MSG 전용 ATU 인덱스, NVMe PME 경로 */

	atu.parent_bus_addr = pci->pp.msg_res->start - pci->parent_bus_offset;	/* NVMe: CPU MSG 주소, NVMe PME TLP 전송 주소 */

	ret = dw_pcie_prog_outbound_atu(pci, &atu);	/* NVMe: MSG TLP ATU 프로그램, NVMe PME 메시지 경로 설정 */
	if (ret)
		return ret;	/* NVMe: ATU 설정 실패, NVMe PME 전송 불가 */

	mem = ioremap(pci->pp.msg_res->start, pci->region_align);	/* NVMe: MSG 영역 매핑, NVMe PME TLP 전송용 */
	if (!mem)
		return -ENOMEM;	/* NVMe: 매핑 실패, NVMe PME 전송 불가 */

	/* A dummy write is converted to a Msg TLP */
	writel(0, mem);	/* NVMe: dummy write -> PME_Turn_Off MSG TLP, NVMe 장치 절전 요청 */

	iounmap(mem);	/* NVMe: MSG 영역 언매핑, NVMe PME TLP 전송 완료 */

	return 0;	/* NVMe: PME_Turn_Off 전송 완료, NVMe 절전 진행 */
}

/* [한국어]
 * dw_pcie_suspend_noirq - 링크를 L2 로 내리고 루트 포트를 잠재운다
 *
 * @pci: 이 DWC 인스턴스.
 * @return: 0 이면 서스펜드 진행 가능. 음수는 PME_Turn_Off 실패로, 상위
 *          드라이버가 서스펜드를 취소한다.
 *
 * SoC 글루 드라이버의 noirq 서스펜드 콜백이 그대로 위임하는 함수다. noirq
 * 단계인 이유는 여기서 링크와 설정공간을 건드리므로 인터럽트가 살아 있으면
 * 안 되기 때문이다.
 *
 * 흐름과 각 판단의 근거:
 *
 * (1) 링크가 이미 내려가 있으면 절차를 통째로 건너뛰고 stop_link 로 간다.
 *
 * (2) **L1SS(L1 하위상태)가 켜져 있으면 아무것도 하지 않고 0 을 돌려준다.**
 *     상류 주석이 이유를 명시한다 -- NVMe 같은 장치는 낮은 재개 지연을
 *     기대하는데, L2 까지 내렸다 올리면 재개가 느려진다. L1SS 로도 충분히
 *     절전되므로 링크를 살려 둔다. (이 파일에서 NVMe 가 언급되는 유일한
 *     자리이며, 상류 주석에 그대로 적혀 있는 근거다.)
 *
 * (3) PME_Turn_Off 발송: SoC 가 자체 훅을 두었으면 그것을, 아니면 iATU MSG
 *     방식을 쓴다. 후자가 실패하면 그대로 실패를 올린다 -- 하위 장치가
 *     정리되지 않은 채 전원을 내릴 수 없기 때문이다.
 *
 * (4) L2 진입 확인: 일부 SoC 는 브로드캐스트 후 LTSSM 레지스터를 읽을 수
 *     없다(skip_l23_ready). 그런 경우 PCIe spec r6.0 §5.3.3.2.1 권고대로
 *     10ms 만 기다리고 넘어간다. 나머지는 read_poll_timeout 으로 LTSSM 이
 *     L2_IDLE 이 되거나 DETECT_WAIT 이하로 떨어지기를 기다린다.
 *
 * (5) 타임아웃이어도 **실패로 보지 않는다**. 상류 주석이 spec r7.0
 *     §5.3.3.2.1 을 근거로, 일부 장치가 10ms 안에 PME_TO_Ack 을 주지 않아도
 *     L2/L3 절차를 계속하라고 권고한다고 적어 두었다. 그래서 경고만 남기고
 *     ret 을 0 으로 되돌린다.
 *
 * (6) udelay(1) -- spec r6.0 §5.3.3.2.1 이 L2/L3 Ready 후 refclock 과 주전원을
 *     끄기 전에 최소 100ns 를 두라고 한다. 연결된 장치가 없어도 무해하다.
 *
 * (7) stop_link: 링크를 멈추고 SoC 의 deinit 을 부른 뒤 suspended 를 세운다.
 *     이 표시가 있어야 resume 이 복구 절차를 밟는다.
 *
 * 코드 관찰: offset 이 0 인 경우(PCI Express 능력 구조를 못 찾음)를 검사하지
 * 않고 (2)의 읽기에 그대로 쓴다. 다만 PCIe 장치는 이 능력을 반드시 가지므로
 * 실제로 도달하는 경로는 아니다.
 *
 * 실행 컨텍스트: 시스템 서스펜드의 noirq 단계. 프로세스 문맥이며 mdelay 로
 * 바쁘게 기다리는 구간이 있다.
 *
 * 호출 체인:
 *   SoC 글루 suspend_noirq → [이 함수] → dw_pcie_pme_turn_off
 *     → read_poll_timeout(dw_pcie_get_ltssm) → dw_pcie_stop_link
 *       → pp->ops->deinit
 */
int dw_pcie_suspend_noirq(struct dw_pcie *pci)
{
	/* NVMe: suspend_noirq 콜백, NVMe 시스템 절전 시 PCIe 링크/장치 처리 */
	u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);	/* NVMe: PCIe capability 오프셋, NVMe 링크 제어 접근 */
	int ret = 0;							/* NVMe: 반환값, NVMe suspend 결과 */
	u32 val;							/* NVMe: 레지스터 값, NVMe LTSSM 상태 */

	if (!dw_pcie_link_up(pci))
		goto stop_link;	/* NVMe: 링크 다운이면 바로 정지, NVMe 절전 진행 */

	/*
	 * If L1SS is supported, then do not put the link into L2 as some
	 * devices such as NVMe expect low resume latency.
	 */
	if (dw_pcie_readw_dbi(pci, offset + PCI_EXP_LNKCTL) & PCI_EXP_LNKCTL_ASPM_L1)
		return 0;	/* NVMe: L1 ASPM 활성 시 L2 진입 skip, NVMe 빠른 resume 지원 */

	if (pci->pp.ops->pme_turn_off) {
		pci->pp.ops->pme_turn_off(&pci->pp);	/* NVMe: vendor PME_Turn_Off, NVMe SoC 특화 절전 */
	} else {
		ret = dw_pcie_pme_turn_off(pci);	/* NVMe: generic PME_Turn_Off, NVMe 장치 절전 메시지 */
		if (ret)
			return ret;	/* NVMe: PME 전송 실패, NVMe suspend 실패 */
	}

	/*
	 * Some SoCs do not support reading the LTSSM register after
	 * PME_Turn_Off broadcast. For those SoCs, skip waiting for L2/L3 Ready
	 * state and wait 10ms as recommended in PCIe spec r6.0, sec 5.3.3.2.1.
	 */
	if (pci->pp.skip_l23_ready) {
		mdelay(PCIE_PME_TO_L2_TIMEOUT_US/1000);	/* NVMe: 10ms 대기, NVMe L2 진입 대체 */
		goto stop_link;	/* NVMe: 링크 정지로 이동, NVMe 절전 완료 */
	}

	ret = read_poll_timeout(dw_pcie_get_ltssm, val,
				val == DW_PCIE_LTSSM_L2_IDLE ||
				val <= DW_PCIE_LTSSM_DETECT_WAIT,
				PCIE_PME_TO_L2_TIMEOUT_US/10,
				PCIE_PME_TO_L2_TIMEOUT_US, false, pci);	/* NVMe: L2 idle 또는 detect wait 폴링, NVMe 링크 절전 상태 확인 */
	if (ret) {
		/*
		 * Failure is non-fatal since spec r7.0, sec 5.3.3.2.1,
		 * recommends proceeding with L2/L3 sequence even if one or more
		 * devices do not respond with PME_TO_Ack after 10ms timeout.
		 */
		dev_warn(pci->dev, "Timeout waiting for L2 entry! LTSSM: 0x%x\n", val);	/* NVMe: L2 진입 타임아웃, NVMe 절전 계속 진행 */
		ret = 0;	/* NVMe: non-fatal, NVMe suspend 계속 */
	}

	/*
	 * Per PCIe r6.0, sec 5.3.3.2.1, software should wait at least
	 * 100ns after L2/L3 Ready before turning off refclock and
	 * main power. This is harmless when no endpoint is connected.
	 */
	udelay(1);	/* NVMe: 1us 대기, NVMe refclock/power off 안전 여유 */

stop_link:
	dw_pcie_stop_link(pci);	/* NVMe: 링크 정지, NVMe 물리 연결 해제 */
	if (pci->pp.ops->deinit)
		pci->pp.ops->deinit(&pci->pp);	/* NVMe: vendor deinit, NVMe SoC 절전 설정 */

	pci->suspended = true;	/* NVMe: suspend 상태 표시, NVMe resume 시 재초기화 플래그 */

	return ret;	/* NVMe: suspend 결과 반환, NVMe 절전 완료/실패 */
}
EXPORT_SYMBOL_GPL(dw_pcie_suspend_noirq);

/* [한국어]
 * dw_pcie_resume_noirq - 서스펜드로 내려 둔 루트 포트와 링크를 되살린다
 *
 * @pci: 이 DWC 인스턴스.
 * @return: 0 성공. 음수면 되감기까지 마친 뒤의 실패값.
 *
 * dw_pcie_suspend_noirq 의 짝이다. 맨 앞의 !pci->suspended 검사가 핵심이다 --
 * 서스펜드가 L1SS 때문에 조기 반환했다면 suspended 가 서지 않았고, 그러면
 * 링크가 그대로 살아 있으므로 복구할 것이 없다. 그런 경우 0 을 돌려 즉시
 * 끝낸다.
 *
 * 복구는 프로브의 축소판이다: SoC 의 init(클록/PHY/리셋) → dw_pcie_setup_rc
 * (설정공간과 iATU 를 통째로 다시 프로그래밍) → 링크 기동 → 링크 대기 →
 * post_init 훅. 하드웨어가 전원을 잃었을 수 있으므로 설정공간을 처음처럼
 * 전부 다시 쓰는 것이 전제다.
 *
 * dw_pcie_setup_rc 의 반환값을 검사하지 않는 점에 유의(상류 그대로, 수정하지
 * 않음). 이 경로에서 실패할 수 있는 부분은 iATU 배분인데, 프로브 때 이미
 * 같은 배분에 성공했으므로 재개에서 새로 실패할 이유가 없다는 전제로 보인다.
 *
 * 링크 대기는 서스펜드와 같은 판단을 쓴다 -- **-ETIMEDOUT 만** 실패로 보고,
 * 그 외에는 그대로 진행한다.
 *
 * 에러 되감기: 링크 대기 실패면 stop_link 부터, init 이후의 실패면 deinit
 * 까지 흘러간다. suspended 는 이미 false 로 내려놓았으므로 실패해도 다음
 * 재개가 다시 시도하지는 않는다.
 *
 * 실행 컨텍스트: 시스템 재개의 noirq 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   SoC 글루 resume_noirq → [이 함수] → pp->ops->init → dw_pcie_setup_rc
 *     → dw_pcie_start_link → dw_pcie_wait_for_link → pp->ops->post_init
 */
int dw_pcie_resume_noirq(struct dw_pcie *pci)
{
	/* NVMe: resume_noirq 콜백, NVMe 시스템 resume 시 PCIe 링크/장치 복구 */
	int ret;	/* NVMe: 반환값, NVMe resume 결과 */

	if (!pci->suspended)
		return 0;	/* NVMe: suspend 상태 아니면 skip, NVMe resume 불필요 */

	pci->suspended = false;	/* NVMe: suspend 플래그 클리어, NVMe resume 진행 */

	if (pci->pp.ops->init) {
		ret = pci->pp.ops->init(&pci->pp);	/* NVMe: vendor 초기화, NVMe SoC resume 설정 */
		if (ret) {
			dev_err(pci->dev, "Host init failed: %d\n", ret);	/* NVMe: resume 초기화 실패, NVMe 복구 불가 */
			return ret;	/* NVMe: resume 실패, NVMe 장치 비활성 */
		}
	}

	dw_pcie_setup_rc(&pci->pp);	/* NVMe: RC 재설정, NVMe 루트 포트 복구 */

	ret = dw_pcie_start_link(pci);	/* NVMe: 링크 재시작, NVMe SSD 물리 연결 복구 */
	if (ret)
		goto err_deinit;	/* NVMe: 링크 시작 실패, NVMe resume 실패 처리 */

	ret = dw_pcie_wait_for_link(pci);	/* NVMe: 링크 업 대기, NVMe SSD 연결 복구 확인 */
	if (ret == -ETIMEDOUT)
		goto err_stop_link;	/* NVMe: 링크 업 타임아웃, NVMe resume 실패 처리 */

	if (pci->pp.ops->post_init)
		pci->pp.ops->post_init(&pci->pp);	/* NVMe: vendor 후처리, NVMe SoC resume 마무리 */

	return 0;	/* NVMe: resume 완료, NVMe PCIe 복구 */

err_stop_link:
	dw_pcie_stop_link(pci);	/* NVMe: 링크 정지, NVMe resume 실패 시 연결 해제 */

err_deinit:
	if (pci->pp.ops->deinit)
		pci->pp.ops->deinit(&pci->pp);	/* NVMe: vendor deinit, NVMe resume 실패 시 정리 */

	return ret;	/* NVMe: resume 실패 코드 반환, NVMe 복구 실패 */
}
EXPORT_SYMBOL_GPL(dw_pcie_resume_noirq);
