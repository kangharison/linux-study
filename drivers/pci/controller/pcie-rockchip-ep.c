// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip AXI PCIe endpoint controller driver
 *
 * Copyright (c) 2018 Rockchip, Inc.
 *
 * Author: Shawn Lin <shawn.lin@rock-chips.com>
 *         Simon Xue <xxm@rock-chips.com>
 */

/* [한국어] configfs 관련 정의. EP 함수 설정이 configfs 로 이루어지는 구조라 포함한다. */
/*
 * [한국어 설명] Rockchip AXI PCIe 컨트롤러를 엔드포인트로 모는 드라이버 (pcie-rockchip-ep.c)
 *
 * === 파일의 역할 ===
 * RK3399 계열 SoC 에 내장된 Rockchip AXI PCIe 컨트롤러를 엔드포인트(EP)로
 * 초기화하고, 커널의 PCI 엔드포인트 프레임워크(EPC)에 struct pci_epc_ops 를
 * 제공해 EPF(엔드포인트 함수) 드라이버가 그 위에 올라탈 수 있게 한다.
 * 같은 IP 를 정반대 역할인 루트 컴플렉스(RC)로 모는 짝이 pcie-rockchip-host.c 이며,
 * 두 파일은 레지스터 지도(pcie-rockchip.h)와 공통 초기화 코드(pcie-rockchip.c)를
 * 공유한다. 갈라지는 지점은 struct rockchip_pcie 의 is_rc 필드 하나로,
 * 이 파일이 그것을 false 로 두고 host 판이 true 로 둔다. 공용 코드가 그 값을
 * 보고 세 곳에서 분기한다 — 자원 이름, PERST# GPIO 이름("ep" vs "reset"),
 * 그리고 MODE_EP 와 MODE_RC 설정이다.
 * 이 파일이 맡는 일은 다섯이다. (1) EPF 가 지정한 config 헤더와 BAR 를
 * 하드웨어에 반영해 호스트에게 "이런 장치다" 라고 보이게 한다.
 * (2) 아웃바운드 창을 관리해 EP 가 호스트 메모리에 접근할 통로를 만든다.
 * (3) MSI 와 INTx 를 실제로 발생시킨다 — MSI 는 전용 창에 대한 메모리 쓰기,
 * INTx 는 레거시 제어 레지스터 토글이다. (4) 호스트가 거는 PERST# 를 GPIO
 * 인터럽트로 감시하고, 링크 훈련을 워크큐에서 폴링해 EPF 에 통지한다.
 * (5) 하드웨어가 지원하지 않는 MSI-X capability 를 목록에서 감춘다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계층으로 보면 위에 EPF 드라이버(예: pci-epf-test)가 있고, 그 아래
 * EPC 프레임워크(drivers/pci/endpoint/pci-epc-core.c)가 있으며, 이 파일이
 * 그 프레임워크의 백엔드로 등록된다. 아래로는 공용 코드와 레지스터가 있다.
 * 진입 경로는 셋이다. (1) probe 가 EPC 객체를 만들고 자원을 준비한 뒤
 * pci_epc_init_notify() 로 EPF 바인딩을 유발한다. (2) EPF 가 pci_epc_* API 를
 * 부르면 EPC 코어가 이 파일의 열두 콜백을 되부른다 — write_header(:1130),
 * set_bar(:1054), clear_bar(:967), align_addr(:858), map_addr(:766),
 * unmap_addr(:741), set_msi(:620), get_msi(:578), raise_irq(:484),
 * start(:434), stop(:410), get_features(:381) (괄호 안은 pci-epc-core.c 의 줄).
 * (3) 호스트가 PERST# 를 토글하면 GPIO 인터럽트가 스레드 핸들러를 깨우고,
 * 그것이 링크 훈련 워크를 예약하거나 취소한다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. PERST# 를 스레드 핸들러로 받는
 * 이유와 링크 훈련을 워크큐로 미루는 이유가 같다 — 둘 다 잠들 수 있는
 * 호출(cancel_delayed_work_sync, readl_poll_timeout)을 하기 때문이다.
 * 다만 INTx 전송 경로의 mdelay(1) 만은 바쁜 대기다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: linux/pci-epc.h 의 struct pci_epc / pci_epc_ops / pci_epc_features 와
 * devm_pci_epc_create(), epc_set_drvdata()/epc_get_drvdata(),
 * pci_epc_multi_mem_init(), pci_epc_mem_alloc_addr(), pci_epc_mem_exit(),
 * pci_epc_init_notify(), pci_epc_linkup()/linkdown().
 * linux/pci-epf.h 의 struct pci_epf_header 와 pci_epf_bar 는 EPF 가 채워
 * 넘기는 서술자다.
 * 옆쪽: pcie-rockchip.h 의 struct rockchip_pcie 와 APB 접근자,
 * pcie-rockchip.c 의 parse_dt / get_phys / enable_clocks / init_port /
 * deinit_phys / disable_clocks.
 * 아래쪽: GPIO(PERST# 감시), 워크큐(링크 훈련), IRQ(스레드 핸들러).
 * 데이터 흐름은 양방향이다. 나가는 쪽: EPF 의 헤더·BAR 서술 → 레지스터 →
 * 호스트가 열거 시 읽는 config 공간. 그리고 EPF 의 메모리 접근 요청 →
 * 아웃바운드 창 → 링크 → 호스트 메모리.
 * 들어오는 쪽: 호스트의 BAR 접근 → 인바운드 변환 → 시스템 메모리.
 * 그리고 호스트가 써 둔 MSI 주소·데이터 → EP 가 읽어 그대로 실행 → 인터럽트.
 * 공유 상태: ob_region_map 비트맵과 ob_addr 배열(창 관리), irq_pci_addr 과
 * irq_pci_fn(전용 창의 현재 매핑), perst_asserted 와 link_up(상태 기계).
 * 이 파일에는 락이 하나도 없는데, EPC 코어가 콜백 호출을 직렬화한다는 전제와
 * PERST# 상태 기계가 한 방향으로만 진행한다는 전제 위에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rockchip_pcie_ep: 공용 상태를 맨 앞에 값으로 품고, 그 뒤에 EPC 객체,
 *   창 관리 자료(max_regions / ob_region_map / ob_addr), MSI 전용 창 정보
 *   (irq_phys_addr / irq_cpu_addr / irq_pci_addr / irq_pci_fn), PERST# 와
 *   링크 상태(perst_irq / perst_asserted / link_up), 훈련 워크를 담는다.
 * - rockchip_pcie_ep_probe(): EPC 생성 → 자원 → 창 메모리 → 클럭 → 포트 초기화
 *   → MSI-X 감추기 → EPF 바인딩 통지 → PERST# 인터럽트 등록.
 * - 열두 개의 pci_epc_ops 콜백: 위 "전체 아키텍처" 절에 EPC 코어의 호출 지점을
 *   파일:라인으로 적어 두었다.
 * - rockchip_pcie_ep_link_training(): 이 파일의 상태 기계 중심. 실패를 오류로
 *   보지 않고 5ms 뒤에 자기 자신을 다시 예약하며 계속 기다린다. 유일하게
 *   재시도하지 않는 경우가 폴링 중 PERST# 가 걸린 때다.
 * - rockchip_pcie_ep_send_msi_irq(): 호스트가 써 둔 주소·데이터를 읽어
 *   전용 창에 한 번 쓰는 것으로 MSI 를 만든다. 같은 목적지면 재매핑을 건너뛰는
 *   최적화가 들어 있다.
 * - rockchip_pcie_ep_hide_broken_msix_cap(): capability 연결 리스트에서 MSI-X
 *   항목을 건너뛰게 만들어, 지원하지 않는 기능을 호스트에게서 감춘다.
 *   host 판이 THP capability 로 L1 서브스테이트를 감추는 것과 같은 수법이다.
 * - rockchip_ob_region(): AXI 주소를 1MB 로 나눠 창 번호를 얻는다. 이 단순한
 *   대응 덕분에 창 관리에 별도 자료구조가 필요 없다.
 * - [상류 코드 관찰, 수정하지 않음] remove 콜백이 없다. builtin 이라 모듈
 *   언로드는 없지만 sysfs 언바인드는 가능하며, 그 경우 EPC 등록과 창 매핑이
 *   남는다(host 판은 remove 를 제대로 구현한다). send_intx_irq 의 mdelay(1)은
 *   바쁜 대기이고, set_msi 는 MMC 필드만 지우고 64비트 플래그는 OR 로만 얹는
 *   비대칭이 있다.
 */

#include <linux/configfs.h>
/* [한국어] usleep_range()/msleep() 선언. 링크 훈련 대기에 쓴다. */
#include <linux/delay.h>
/* [한국어] gpiod 계열 — PERST# 신호를 GPIO 로 감시한다. */
#include <linux/gpio/consumer.h>
/* [한국어] readl_poll_timeout() — 링크 상태 폴링. */
#include <linux/iopoll.h>
/* [한국어] 커널 공통 정의. */
#include <linux/kernel.h>
/* [한국어] irq_get_irq_data 등 IRQ 관련 정의. PERST# 를 인터럽트로 받기 위해 필요하다. */
#include <linux/irq.h>
/* [한국어] 디바이스 트리 접근. */
#include <linux/of.h>
/* [한국어] struct pci_epc, pci_epc_ops, pci_epc_features, epc_get_drvdata() —
 * 엔드포인트 컨트롤러(EPC) 프레임워크의 핵심 헤더다. */
#include <linux/pci-epc.h>
/* [한국어] platform_driver 와 자원 조회. */
#include <linux/platform_device.h>
/* [한국어] struct pci_epf_header, pci_epf_bar — EP 함수(EPF)가 쓰는 서술자 타입. */
#include <linux/pci-epf.h>
/* [한국어] SZ_1M 등 크기 상수. */
#include <linux/sizes.h>
/* [한국어] delayed_work — 링크 훈련을 워크큐로 미뤄 실행하는 데 쓴다. */
#include <linux/workqueue.h>

/* [한국어] 이 드라이버의 공용 헤더. host 판과 같은 레지스터 지도와
 * struct rockchip_pcie 를 공유한다 — 같은 IP 를 정반대 역할로 쓰는 짝이다. */
#include "pcie-rockchip.h"

/**
 * struct rockchip_pcie_ep - private data for PCIe endpoint controller driver
 * @rockchip: Rockchip PCIe controller
 * @epc: PCI EPC device
 * @max_regions: maximum number of regions supported by hardware
 * @ob_region_map: bitmask of mapped outbound regions
 * @ob_addr: base addresses in the AXI bus where the outbound regions start
 * @irq_phys_addr: base address on the AXI bus where the MSI/INTX IRQ
 *		   dedicated outbound regions is mapped.
 * @irq_cpu_addr: base address in the CPU space where a write access triggers
 *		  the sending of a memory write (MSI) / normal message (INTX
 *		  IRQ) TLP through the PCIe bus.
 * @irq_pci_addr: used to save the current mapping of the MSI/INTX IRQ
 *		  dedicated outbound region.
 * @irq_pci_fn: the latest PCI function that has updated the mapping of
 *		the MSI/INTX IRQ dedicated outbound region.
 * @irq_pending: bitmask of asserted INTX IRQs.
 * @perst_irq: IRQ used for the PERST# signal.
 * @perst_asserted: True if the PERST# signal was asserted.
 * @link_up: True if the PCI link is up.
 * @link_training: Work item to execute PCI link training.
 */
struct rockchip_pcie_ep {
	/* [한국어] 공용 컨트롤러 상태. 구조체 맨 앞에 값으로 두어, 이 필드의 주소가 곧
	 * struct rockchip_pcie_ep 의 주소가 된다.
	 * 설정자: rockchip_pcie_ep_probe().
	 * 읽는 자: 공용 코드 전체와 이 파일의 모든 레지스터 접근.
	 * 값 범위: 구조체 내장. is_rc 는 대입하지 않아 false 로 남는다 —
	 *   그것이 host 판과 갈라지는 유일한 지점이다.
	 * 동기화: 공용 코드가 관리한다. */
	struct rockchip_pcie	rockchip;
	/* [한국어] EPC 프레임워크에 등록한 컨트롤러 객체(옆의 상류 kernel-doc).
	 * 설정자: probe 가 devm_pci_epc_create() 로 만들고 epc_set_drvdata() 로
	 *   이 구조체를 심어 둔다 — 그래서 모든 콜백이 epc_get_drvdata() 로 되찾는다.
	 * 읽는 자: 링크 상태 통지(pci_epc_linkup/linkdown)와 EPF 알림.
	 * 값 범위: 유효 포인터.
	 * 동기화: EPC 프레임워크가 관리한다. */
	struct pci_epc		*epc;
	/* [한국어] 하드웨어가 지원하는 아웃바운드 창의 개수(옆의 상류 kernel-doc).
	 * 설정자: probe 가 DT 의 rockchip,max-outbound-regions 에서 읽는다.
	 * 읽는 자: ob_addr 배열 할당 크기와 EPC 능력 보고.
	 * 값 범위: 1 이상 ROCKCHIP_PCIE_EP_MAX_REGION_NUM 이하.
	 * 동기화: 설정 후 읽기 전용. */
	u32			max_regions;
	/* [한국어] 사용 중인 아웃바운드 창의 비트맵(옆의 상류 kernel-doc).
	 * 설정자: map_addr 이 set_bit, unmap_addr 이 clear_bit.
	 * 읽는 자: 같은 두 함수의 중복·존재 검사.
	 * 값 범위: 하위 max_regions 비트.
	 * 동기화: 없다. EPC 코어가 호출을 직렬화한다는 전제다. */
	unsigned long		ob_region_map;
	/* [한국어] 각 아웃바운드 창이 대응하는 AXI(CPU) 주소 배열(옆의 상류 kernel-doc).
	 * 설정자: map_addr 이 채우고 unmap_addr 이 0 으로 되돌린다.
	 * 읽는 자: unmap_addr 이 요청된 주소와 대조해 올바른 창인지 확인한다.
	 * 값 범위: 원소 max_regions 개. devm 으로 할당된다.
	 * 동기화: 없다. */
	phys_addr_t		*ob_addr;
	/* [한국어] MSI/INTx 전용 아웃바운드 창의 AXI 물리 주소(옆의 상류 kernel-doc).
	 * 설정자: probe 가 자원에서 얻는다.
	 * 읽는 자: rockchip_ob_region() 으로 창 번호를 계산하는 데 쓴다.
	 * 값 범위: 유효한 물리 주소.
	 * 동기화: 설정 후 읽기 전용. */
	phys_addr_t		irq_phys_addr;
	/* [한국어] 같은 창의 CPU 가상 주소(옆의 상류 kernel-doc). 여기에 쓰기를 하면
	 * 메모리 쓰기(MSI) 또는 일반 메시지(INTx) TLP 가 링크로 나간다.
	 * 설정자: probe 의 ioremap.
	 * 읽는 자: raise_irq 계열이 실제로 쓰기를 수행한다.
	 * 값 범위: 유효한 __iomem 포인터.
	 * 동기화: 없다. */
	void __iomem		*irq_cpu_addr;
	/* [한국어] 그 전용 창이 현재 어느 PCI 주소로 매핑되어 있는지(옆의 상류 kernel-doc).
	 * 설정자: raise_irq 경로가 매핑을 바꿀 때마다 갱신한다.
	 * 읽는 자: 같은 경로가 "이미 그 주소로 매핑되어 있으면 다시 하지 않는다"는
	 *   최적화 판정에 쓴다.
	 * 값 범위: PCI 주소 또는 0(아직 매핑 안 됨).
	 * 동기화: 없다. */
	u64			irq_pci_addr;
	/* [한국어] 그 매핑을 마지막으로 갱신한 PCI 함수 번호(옆의 상류 kernel-doc).
	 * 설정자·읽는 자: irq_pci_addr 과 같다. 창은 하나뿐인데 함수는 여럿일 수 있어,
	 *   주소뿐 아니라 함수도 같아야 재매핑을 건너뛸 수 있다.
	 * 값 범위: 0 이상.
	 * 동기화: 없다. */
	u8			irq_pci_fn;
	/* [한국어] 어서트된 INTx 의 비트맵(옆의 상류 kernel-doc).
	 * 설정자·읽는 자: INTx assert/deassert 경로.
	 * 값 범위: INTx 는 EP 당 하나뿐이라 실질적으로 비트 0 만 쓴다.
	 * 동기화: 없다. */
	u8			irq_pending;
	/* [한국어] PERST# 신호를 감시하는 인터럽트 번호(옆의 상류 kernel-doc).
	 * 설정자: probe 가 GPIO 에서 얻는다.
	 * 읽는 자: 인터럽트 등록·해제와 트리거 방향 변경.
	 * 값 범위: 유효한 IRQ 번호.
	 * 동기화: IRQ 코어가 관리한다. */
	int			perst_irq;
	/* [한국어] PERST# 가 현재 어서트 상태인지(옆의 상류 kernel-doc).
	 * 설정자: PERST# 인터럽트 핸들러.
	 * 읽는 자: 링크 훈련 워크와 중복 처리 방지.
	 * 값 범위: true/false.
	 * 동기화: 없다 — 인터럽트와 워크큐가 같은 값을 보지만 상태 기계가
	 *   한 방향으로만 진행한다는 전제다. */
	bool			perst_asserted;
	/* [한국어] PCI 링크가 올라와 있는지(옆의 상류 kernel-doc).
	 * 설정자: 링크 훈련 워크와 PERST# 핸들러.
	 * 읽는 자: EPC 코어에 linkup/linkdown 을 통지할지 판정.
	 * 값 범위: true/false.
	 * 동기화: 없다. */
	bool			link_up;
	/* [한국어] 링크 훈련을 미뤄 실행하기 위한 지연 워크(옆의 상류 kernel-doc).
	 * 설정자: probe 가 INIT_DELAYED_WORK 로 초기화한다.
	 * 읽는 자: PERST# 해제 시 schedule_delayed_work 로 예약된다.
	 * 값 범위: 워크큐 서술자.
	 * 동기화: 워크큐 프레임워크가 관리한다. 인터럽트 문맥에서는 훈련을 할 수
	 *   없으므로(잠들어야 한다) 워크로 미루는 구조다. */
	struct delayed_work	link_training;
};

/* [한국어]
 * rockchip_pcie_clear_ep_ob_atu - 아웃바운드 창 하나를 완전히 비운다
 *
 * @rockchip: 공용 컨트롤러 상태.
 * @region: 비울 창 번호.
 *
 * 창을 구성하는 네 레지스터(PCI 주소 하위·상위, 서술자 0·1)를 모두 0 으로 지운다.
 * 특히 서술자 0 에는 함수 번호와 트랜잭션 종류가 들어 있으므로, 그것을 남겨 두면
 * 창이 부분적으로 살아 있는 상태가 된다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_unmap_addr() → [이 함수] → rockchip_pcie_write() ×4
 */
static void rockchip_pcie_clear_ep_ob_atu(struct rockchip_pcie *rockchip,
					  u32 region)
{
	/* [한국어] 창의 PCI 주소 레지스터 두 개와 서술자 두 개를 모두 0 으로 지운다.
	 * 네 레지스터를 다 비워야 창이 완전히 비활성화된다. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0(region));
	/* [한국어] PCI 주소 상위. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR1(region));
	/* [한국어] 서술자 0 — 여기에 함수 번호와 트랜잭션 종류가 들어 있으므로 반드시 지워야 한다. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC0(region));
	/* [한국어] 서술자 1. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC1(region));
}

/* [한국어]
 * rockchip_pcie_ep_ob_atu_num_bits - 요청 범위를 덮는 창 크기(통과 비트 수)를 계산한다
 *
 * @rockchip: 공용 컨트롤러 상태. 실제로는 쓰지 않지만 시그니처 일관성을 위해 받는다.
 * @pci_addr: 매핑할 PCI 주소의 시작.
 * @size: 매핑할 크기.
 * @return: 통과시킬 하위 주소 비트 수(하드웨어 최소·최대 사이로 잘린 값).
 *
 * 계산이 한 줄인데 그 한 줄이 이 함수의 전부다. 범위의 시작과 끝을 XOR 하면
 * 두 주소가 갈라지는 최상위 비트만 남는다. fls64 로 그 위치를 얻으면,
 * 그만큼의 하위 비트를 통과시켜야 범위 전체가 한 창에 들어간다는 뜻이 된다.
 * 정렬과 크기를 따로 계산하지 않고 한 번에 구하는 방식이다.
 *
 * clamp 로 하드웨어 한계에 맞춘다. 요청이 너무 작으면 최소 크기 창을,
 * 너무 크면 최대 크기 창을 쓰게 된다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. 어떤 입력에도 유효한 값을 돌려준다.
 *
 * 호출 체인:
 *   rockchip_pcie_prog_ep_ob_atu() / rockchip_pcie_ep_align_addr() → [이 함수]
 */
static int rockchip_pcie_ep_ob_atu_num_bits(struct rockchip_pcie *rockchip,
					    u64 pci_addr, size_t size)
{
	/* [한국어] 요청 범위의 시작과 끝을 XOR 하면 두 주소가 갈라지는 최상위 비트가 남는다.
	 * fls64 로 그 위치를 얻으면, 그만큼의 하위 비트를 통과시켜야 범위 전체가
	 * 한 창에 들어간다는 뜻이 된다. 정렬을 따로 계산하지 않고 이 한 줄로
	 * 필요한 창 크기를 구하는 영리한 방식이다. */
	int num_pass_bits = fls64(pci_addr ^ (pci_addr + size - 1));

	/* [한국어] 하드웨어가 허용하는 최소·최대 통과 비트 수로 잘라 낸다.
	 * 요청이 너무 작으면 최소 크기 창을, 너무 크면 최대 크기 창을 쓴다. */
	return clamp(num_pass_bits,
		     ROCKCHIP_PCIE_AT_MIN_NUM_BITS,
		     ROCKCHIP_PCIE_AT_MAX_NUM_BITS);
}

/* [한국어]
 * rockchip_pcie_prog_ep_ob_atu - EP 아웃바운드 창 하나를 프로그래밍한다
 *
 * @rockchip: 공용 컨트롤러 상태.
 * @fn: 이 창을 소유할 PCI 함수 번호.
 * @r: 창 번호.
 * @cpu_addr: AXI(CPU) 쪽 주소. 실제로는 쓰지 않는다 — 창 번호가 이미 그 주소에서
 *       계산되어 나왔기 때문이다(rockchip_ob_region 참조).
 * @pci_addr: 이 창이 대응할 PCI 버스 주소.
 * @size: 매핑할 크기.
 *
 * host 판의 rockchip_pcie_prog_ob_atu() 와 이름이 비슷하지만 세 가지가 다르다.
 *   1) 통과 비트 수를 인자로 받지 않고 스스로 계산한다.
 *   2) 레지스터에 넣을 때 num_pass_bits - 1 을 쓴다. 이 레지스터가 0 기반으로
 *      인코딩하기 때문이며, host 판이 값을 그대로 넣는 것과 대비된다.
 *   3) 서술자에 함수 번호를 함께 넣는다. EP 는 여러 PCI 함수를 노출할 수 있어
 *      이 창이 어느 함수의 것인지 하드웨어에 알려야 한다.
 *
 * 종류는 언제나 메모리 쓰기다 — EP 가 아웃바운드로 내보내는 것은 호스트 메모리
 * 접근과 MSI 뿐이고, 둘 다 메모리 쓰기 TLP 이기 때문이다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. 유효성 검사도 하지 않는다 — 호출자가 이미 걸렀다는 전제다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_map_addr() / rockchip_pcie_ep_send_msi_irq() → [이 함수]
 *     → rockchip_pcie_ep_ob_atu_num_bits() → rockchip_pcie_write() ×4
 */
static void rockchip_pcie_prog_ep_ob_atu(struct rockchip_pcie *rockchip, u8 fn,
					 u32 r, u64 cpu_addr, u64 pci_addr,
					 size_t size)
{
	/* [한국어] 계산된 통과 비트 수. */
	int num_pass_bits;
	/* [한국어] 레지스터에 쓸 세 값. */
	u32 addr0, addr1, desc0;

	/* [한국어] 요청 범위를 덮는 창 크기를 구한다. */
	num_pass_bits = rockchip_pcie_ep_ob_atu_num_bits(rockchip,
							 pci_addr, size);

	/* [한국어] 통과 비트 수에서 1 을 뺀 값을 필드에 넣는다 — 이 레지스터가 0 기반으로
	 * 인코딩하기 때문이다. host 판의 prog_ob_atu 가 num_pass_bits 를 그대로
	 * 넣는 것과 다르므로 주의해야 한다. */
	addr0 = ((num_pass_bits - 1) & PCIE_CORE_OB_REGION_ADDR0_NUM_BITS) |
		/* [한국어] PCI 주소 하위를 같은 워드의 주소 필드에 OR 로 넣는다. */
		(lower_32_bits(pci_addr) & PCIE_CORE_OB_REGION_ADDR0_LO_ADDR);
	/* [한국어] PCI 주소 상위는 별도 레지스터로. */
	addr1 = upper_32_bits(pci_addr);
	/* [한국어] 서술자에 함수 번호와 메모리 쓰기 종류를 넣는다. EP 는 여러 PCI 함수를
	 * 노출할 수 있어, 이 창이 어느 함수의 것인지 하드웨어에 알려야 한다. */
	desc0 = ROCKCHIP_PCIE_AT_OB_REGION_DESC0_DEVFN(fn) | AXI_WRAPPER_MEM_WRITE;

	/* PCI bus address region */
	/* [한국어] PCI 주소 하위를 쓴다(옆의 상류 주석). */
	rockchip_pcie_write(rockchip, addr0,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR0(r));
	/* [한국어] PCI 주소 상위를 쓴다. */
	rockchip_pcie_write(rockchip, addr1,
			    ROCKCHIP_PCIE_AT_OB_REGION_PCI_ADDR1(r));
	/* [한국어] 서술자를 쓴다. */
	rockchip_pcie_write(rockchip, desc0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC0(r));
	/* [한국어] 서술자 두 번째 워드는 0 으로 비운다. */
	rockchip_pcie_write(rockchip, 0,
			    ROCKCHIP_PCIE_AT_OB_REGION_DESC1(r));
}

/* [한국어]
 * rockchip_pcie_ep_write_header - EPF 가 지정한 PCI config 헤더를 하드웨어에 쓴다
 *
 * @epc: EPC 객체. drvdata 에 이 드라이버의 EP 객체가 들어 있다.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 이 컨트롤러는 SR-IOV 를 지원하지 않아 쓰지 않는다.
 * @hdr: EPF 드라이버가 채운 헤더 값들(벤더/디바이스 ID, 클래스 코드 등).
 * @return: 언제나 0.
 *
 * 호스트가 이 엔드포인트를 열거할 때 읽게 될 config 헤더를 구성한다.
 * EPF 드라이버가 "나는 이런 장치다" 라고 선언한 내용을 그대로 레지스터에 옮기는
 * 일이며, 이 값들이 호스트 쪽 드라이버 매칭을 결정한다.
 *
 * 주의할 점 둘.
 *   1) 위 영어 주석대로 벤더 ID 는 모든 함수가 함수 0 의 것을 공유한다.
 *      그래서 fn == 0 일 때만 쓴다.
 *   2) 디바이스 ID 는 벤더 ID 와 같은 워드에 있으므로 읽기-수정-쓰기로
 *      상위 16비트만 갈아 끼운다.
 * Revision ID 와 클래스 코드 세 바이트는 config 오프셋 0x08 의 배치 그대로
 * 한 워드로 조립해 쓴다. Interrupt Line 은 호스트가 채우는 필드라 건드리지 않고
 * Interrupt Pin 만 상위 바이트에 쓴다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_write_header() (pci-epc-core.c:1130)
 *     → pci_epc_ops.write_header == [이 함수]
 */
static int rockchip_pcie_ep_write_header(struct pci_epc *epc, u8 fn, u8 vfn,
					 struct pci_epf_header *hdr)
{
	/* [한국어] 읽기-수정-쓰기에 쓸 임시 변수. */
	u32 reg;
	/* [한국어] EPC 에 심어 둔 이 드라이버의 EP 객체를 꺼낸다. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 그 안의 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;

	/* All functions share the same vendor ID with function 0 */
	/* [한국어] 위 영어 주석대로 모든 함수가 함수 0 의 벤더 ID 를 공유한다.
	 * 그래서 함수 0 일 때만 쓴다. */
	if (fn == 0) {
		/* [한국어] 벤더 ID 와 서브시스템 벤더 ID 를 한 워드에 담아 쓴다. */
		rockchip_pcie_write(rockchip,
				    hdr->vendorid | hdr->subsys_vendor_id << 16,
				    PCIE_CORE_CONFIG_VENDOR);
	}

	/* [한국어] Device ID / Vendor ID 레지스터를 읽는다. */
	reg = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_DID_VID);
	/* [한국어] 하위 16비트(벤더 ID)는 보존하고 상위 16비트에 디바이스 ID 를 넣는다. */
	reg = (reg & 0xFFFF) | (hdr->deviceid << 16);
	/* [한국어] 되쓴다. */
	rockchip_pcie_write(rockchip, reg, PCIE_EP_CONFIG_DID_VID);

	/* [한국어] Revision ID 와 클래스 코드 세 바이트를 한 워드로 조립해 쓴다.
	 * PCI config 오프셋 0x08 의 배치가 정확히 그 순서다. */
	rockchip_pcie_write(rockchip,
			    hdr->revid |
			    hdr->progif_code << 8 |
			    hdr->subclass_code << 16 |
			    hdr->baseclass_code << 24,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) + PCI_REVISION_ID);
	/* [한국어] 캐시 라인 크기를 쓴다. */
	rockchip_pcie_write(rockchip, hdr->cache_line_size,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    PCI_CACHE_LINE_SIZE);
	/* [한국어] 서브시스템 ID 를 상위 16비트에 쓴다 — 하위 16비트인 서브시스템 벤더 ID 는
	 * 위에서 이미 PCIE_CORE_CONFIG_VENDOR 로 설정했다. */
	rockchip_pcie_write(rockchip, hdr->subsys_id << 16,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    PCI_SUBSYSTEM_VENDOR_ID);
	/* [한국어] 인터럽트 핀 번호를 상위 바이트에 쓴다. 하위 바이트인 Interrupt Line 은
	 * 호스트가 채우는 필드라 건드리지 않는다. */
	rockchip_pcie_write(rockchip, hdr->interrupt_pin << 8,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    PCI_INTERRUPT_LINE);

	/* [한국어] 헤더 설정 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_ep_set_bar - BAR 하나를 호스트에게 노출하고 인바운드 변환을 건다
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 쓰지 않는다.
 * @epf_bar: EPF 가 준비한 BAR 서술(번호, 크기, 물리 주소, 속성 플래그).
 * @return: 0 = 성공. -EINVAL = 64비트 BAR 를 홀수 번호에 요청함.
 *
 * 두 가지를 동시에 한다 — 호스트에게 보일 BAR 의 크기·속성을 설정하고,
 * 그 BAR 로 들어온 접근을 시스템 메모리의 어느 물리 주소로 보낼지 정한다.
 *
 * 크기 처리가 이 함수에서 가장 미묘하다.
 *   1) 하드웨어 최소 단위(MIN_EP_APERTURE)보다 작으면 그것으로 올린다.
 *   2) 2의 거듭제곱으로 올린다. 위 영어 주석대로 roundup_pow_of_two() 는
 *      unsigned long 을 돌려줘 64비트 값에 쓸 수 없어서, fls64 로 직접 계산한다.
 *   3) log2(크기) - 7 로 aperture 코드를 만든다. 옆의 상류 주석이 그 대응표를
 *      보여 준다 — BAR 크기가 2^(aperture+7) 이기 때문이다.
 *
 * 속성은 네 조합(64비트 × 프리페치)에 각각 다른 컨트롤 코드를 매핑한다.
 * 64비트 BAR 를 홀수 번호에 요청하면 다음 BAR 와 겹치므로 거절한다.
 *
 * 레지스터 배치도 주의가 필요하다. BAR 0~3 과 4~5 의 설정이 서로 다른 워드에
 * 들어 있고, 한 워드 안에 여러 BAR 의 필드가 함께 있어 읽기-수정-쓰기로
 * 자기 몫만 갈아 끼워야 한다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 64비트 정렬 위반 하나뿐이고, 레지스터를 쓰기 전에 걸러진다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_set_bar() (pci-epc-core.c:1054)
 *     → pci_epc_ops.set_bar == [이 함수] → rockchip_pcie_write() ×3
 */
static int rockchip_pcie_ep_set_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				    struct pci_epf_bar *epf_bar)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 이 BAR 가 가리킬 시스템 메모리의 물리 주소. */
	dma_addr_t bar_phys = epf_bar->phys_addr;
	/* [한국어] BAR 번호. */
	enum pci_barno bar = epf_bar->barno;
	/* [한국어] BAR 속성 플래그(IO/메모리, 32/64비트, 프리페치 가능 여부). */
	int flags = epf_bar->flags;
	/* [한국어] 레지스터 조작에 쓸 임시 변수들. */
	u32 addr0, addr1, reg, cfg, b, aperture, ctrl;
	/* [한국어] 정규화된 BAR 크기. */
	u64 sz;

	/* BAR size is 2^(aperture + 7) */
	/* [한국어] 요청 크기가 하드웨어 최소 단위보다 작으면 최소값을 쓴다(옆의 상류 주석 —
	 * BAR 크기는 2^(aperture+7)). */
	sz = max_t(size_t, epf_bar->size, MIN_EP_APERTURE);

	/*
	 * roundup_pow_of_two() returns an unsigned long, which is not suited
	 * for 64bit values.
	 */
	/* [한국어] 위 영어 주석대로 roundup_pow_of_two() 는 unsigned long 을 돌려줘
	 * 64비트 값에 쓸 수 없다. 그래서 fls64 로 최상위 비트를 찾아 직접
	 * 2의 거듭제곱으로 올린다. */
	sz = 1ULL << fls64(sz - 1);
	/* [한국어] 크기를 aperture 코드로 바꾼다. 옆의 상류 주석이 대응표를 보여 준다 —
	 * 128B 가 0, 256B 가 1 이므로 log2(크기) - 7 이다. */
	aperture = ilog2(sz) - 7; /* 128B -> 0, 256B -> 1, 512B -> 2, ... */

	/* [한국어] I/O 공간 BAR 인지 확인한다. */
	if ((flags & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
		/* [한국어] I/O 는 32비트 하나뿐이라 다른 선택이 없다. */
		ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_IO_32BITS;
	} else {
		/* [한국어] 메모리 BAR 의 프리페치 가능 여부. */
		bool is_prefetch = !!(flags & PCI_BASE_ADDRESS_MEM_PREFETCH);
		/* [한국어] 64비트 BAR 여부. */
		bool is_64bits = !!(flags & PCI_BASE_ADDRESS_MEM_TYPE_64);

		/* [한국어] 64비트 BAR 는 두 칸을 쓰므로 반드시 짝수 번호에서 시작해야 한다.
		 * 홀수 번호를 요청하면 다음 BAR 와 겹쳐 버린다. */
		if (is_64bits && (bar & 1))
			/* [한국어] -EINVAL 로 거절한다. */
			return -EINVAL;

		/* [한국어] 64비트 + 프리페치 가능, */
		if (is_64bits && is_prefetch)
			ctrl =
			    ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_PREFETCH_MEM_64BITS;
		/* [한국어] 32비트 + 프리페치 가능, */
		else if (is_prefetch)
			ctrl =
			    ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_PREFETCH_MEM_32BITS;
		/* [한국어] 64비트 일반 메모리, */
		else if (is_64bits)
			ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_MEM_64BITS;
		/* [한국어] 32비트 일반 메모리 — 네 조합을 각각 다른 컨트롤 코드로 매핑한다. */
		else
			ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_MEM_32BITS;
	}

	/* [한국어] BAR 0~3 과 4~5 의 설정이 서로 다른 레지스터에 들어 있다. */
	if (bar < BAR_4) {
		/* [한국어] 앞쪽 레지스터. */
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG0(fn);
		/* [한국어] 인덱스는 그대로. */
		b = bar;
	} else {
		/* [한국어] 뒤쪽 레지스터. */
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG1(fn);
		/* [한국어] 인덱스를 BAR_4 만큼 빼 0 기반으로 되돌린다. */
		b = bar - BAR_4;
	}

	/* [한국어] BAR 가 가리킬 물리 주소 하위. */
	addr0 = lower_32_bits(bar_phys);
	/* [한국어] 그 상위. */
	addr1 = upper_32_bits(bar_phys);

	/* [한국어] 현재 설정 워드를 읽는다. */
	cfg = rockchip_pcie_read(rockchip, reg);
	/* [한국어] 이 BAR 의 aperture 와 컨트롤 필드만 지운다 — 같은 워드에 다른 BAR 의
	 * 설정도 들어 있으므로 통째로 덮으면 안 된다. */
	cfg &= ~(ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
		 ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
	/* [한국어] 새 aperture 와 컨트롤 값을 넣는다. */
	cfg |= (ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE(b, aperture) |
		ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL(b, ctrl));

	/* [한국어] 설정 워드를 되쓴다. */
	rockchip_pcie_write(rockchip, cfg, reg);
	/* [한국어] 인바운드 변환 주소 하위를 쓴다 — 호스트가 이 BAR 에 접근하면 이 물리
	 * 주소로 변환된다. */
	rockchip_pcie_write(rockchip, addr0,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar));
	/* [한국어] 그 상위. */
	rockchip_pcie_write(rockchip, addr1,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar));

	/* [한국어] BAR 설정 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_ep_clear_bar - BAR 를 호스트에게서 감추고 인바운드 변환을 지운다
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 쓰지 않는다.
 * @epf_bar: 해제할 BAR 서술. barno 만 쓴다.
 *
 * set_bar 의 역이다. 같은 레지스터 선택 규칙(BAR 0~3 과 4~5 가 다른 워드)을
 * 쓰고, 컨트롤 필드를 DISABLED 로 바꾼 뒤 인바운드 변환 주소를 지운다.
 *
 * 순서가 중요하다 — 설정을 먼저 끄고 주소를 지운다. 반대로 하면 아주 짧게
 * BAR 가 주소 0 으로 열려 있는 순간이 생겨, 그 사이 호스트 접근이 시스템
 * 메모리 0번지로 향할 수 있다.
 *
 * 같은 IP 의 다른 변종인 rcar-ep 의 clear_bar 가 창 번호를 잘못 넘기는 문제가
 * 있는 것과 달리, 이쪽은 BAR 번호를 그대로 쓰므로 그런 어긋남이 없다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_clear_bar() (pci-epc-core.c:967)
 *     → pci_epc_ops.clear_bar == [이 함수] → rockchip_pcie_write() ×3
 */
static void rockchip_pcie_ep_clear_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				       struct pci_epf_bar *epf_bar)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 레지스터 조작용 임시 변수. */
	u32 reg, cfg, b, ctrl;
	/* [한국어] 해제할 BAR 번호. */
	enum pci_barno bar = epf_bar->barno;

	/* [한국어] set_bar 와 같은 레지스터 선택 규칙. */
	if (bar < BAR_4) {
		/* [한국어] 앞쪽 레지스터. */
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG0(fn);
		/* [한국어] 인덱스 그대로. */
		b = bar;
	} else {
		/* [한국어] 뒤쪽 레지스터. */
		reg = ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG1(fn);
		/* [한국어] 0 기반으로 되돌린다. */
		b = bar - BAR_4;
	}

	/* [한국어] 컨트롤 코드를 "비활성"으로 정한다. */
	ctrl = ROCKCHIP_PCIE_CORE_BAR_CFG_CTRL_DISABLED;
	/* [한국어] 현재 설정을 읽는다. */
	cfg = rockchip_pcie_read(rockchip, reg);
	/* [한국어] 이 BAR 의 두 필드를 지운다. */
	cfg &= ~(ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) |
		 ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b));
	/* [한국어] aperture 는 0 으로 둔 채 컨트롤만 비활성으로 넣는다. */
	cfg |= ROCKCHIP_PCIE_CORE_EP_FUNC_BAR_CFG_BAR_CTRL(b, ctrl);

	/* [한국어] 설정을 되쓴다 — 이 시점부터 호스트에게 이 BAR 가 보이지 않는다. */
	rockchip_pcie_write(rockchip, cfg, reg);
	/* [한국어] 인바운드 변환 주소도 지운다. */
	rockchip_pcie_write(rockchip, 0x0,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar));
	/* [한국어] 상위 주소도 지운다. 설정을 먼저 끄고 주소를 지우는 순서가 중요하다 —
	 * 반대면 아주 짧게 BAR 가 주소 0 으로 열려 있는 순간이 생긴다. */
	rockchip_pcie_write(rockchip, 0x0,
			    ROCKCHIP_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar));
}

/* [한국어]
 * rockchip_ob_region - AXI 주소에서 아웃바운드 창 번호를 구한다
 *
 * @addr: AXI(CPU) 물리 주소.
 * @return: 창 번호(0~31).
 *
 * 이 컨트롤러의 아웃바운드 창은 1MB 단위이고 AXI 영역에 창 번호 순서대로
 * 연속 배치되어 있다. 그래서 주소를 1MB 로 나누기만 하면 창 번호가 된다.
 * 0x1f 로 자르는 것은 창이 최대 32개이기 때문이다.
 *
 * 이 단순한 대응 관계 덕분에 map_addr / unmap_addr 이 별도의 자료구조 없이
 * 주소만으로 창을 찾을 수 있고, ob_region_map 비트맵과 ob_addr 배열도
 * 창 번호를 인덱스로 그대로 쓸 수 있다.
 *
 * 실행 컨텍스트: 어디서든 안전한 순수 계산.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_map_addr() / unmap_addr() / send_msi_irq() → [이 함수]
 */
static inline u32 rockchip_ob_region(phys_addr_t addr)
{
	/* [한국어] AXI 주소를 1MB 로 나눠 창 번호를 구한다. 아웃바운드 창 하나가 1MB 이고
	 * AXI 영역이 창 번호 순서대로 연속 배치되어 있다는 전제다.
	 * 0x1f 로 자르는 것은 창이 최대 32개이기 때문이다. */
	return (addr >> ilog2(SZ_1M)) & 0x1f;
}

/* [한국어]
 * rockchip_pcie_ep_align_addr - 요청 주소를 창 경계에 맞추고 조정된 크기·오프셋을 돌려준다
 *
 * @epc: EPC 객체.
 * @pci_addr: EPF 가 매핑하려는 PCI 주소.
 * @pci_size: 입출력. 들어올 때는 요청 크기, 나갈 때는 실제로 매핑할 크기.
 * @addr_offset: 출력. 정렬된 시작에서 요청 주소까지의 거리.
 * @return: 창 경계에 정렬된 PCI 주소.
 *
 * 왜 필요한가: 하드웨어 창은 크기가 2의 거듭제곱이고 그 크기에 정렬되어야 한다.
 * 그런데 EPF 가 요청하는 주소는 임의의 값이다. 이 콜백이 그 간극을 메운다 —
 * 정렬된 시작 주소로 창을 잡되, 요청 주소가 창 안 어디에 있는지를 오프셋으로
 * 알려 주어 호출자가 최종 위치를 계산할 수 있게 한다.
 *
 * 세 값을 함께 돌려주는 구조라 호출자가 창 경계를 신경 쓰지 않아도 된다.
 * 크기가 창 하나(1MB)를 넘으면 창 끝까지로 잘라 내는데, EPF 는 그 축소된
 * 크기를 보고 나머지를 다시 매핑한다.
 *
 * 마지막에 ROCKCHIP_PCIE_AT_SIZE_ALIGN 으로 올림하는 것은 하드웨어가 요구하는
 * 크기 단위를 맞추기 위해서다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF → pci_epc_mem_map() (pci-epc-core.c:858)
 *     → pci_epc_ops.align_addr == [이 함수]
 *   또는 rockchip_pcie_ep_send_msi_irq() 가 직접 호출
 */
static u64 rockchip_pcie_ep_align_addr(struct pci_epc *epc, u64 pci_addr,
				       size_t *pci_size, size_t *addr_offset)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 호출자가 요청한 크기. */
	size_t size = *pci_size;
	/* [한국어] offset: 정렬 경계에서 요청 주소까지의 거리. mask: 창 안 오프셋을 뽑는 마스크. */
	u64 offset, mask;
	/* [한국어] 창에 필요한 통과 비트 수. */
	int num_bits;

	/* [한국어] 요청 범위를 덮는 창 크기를 구한다. */
	num_bits = rockchip_pcie_ep_ob_atu_num_bits(&ep->rockchip,
						    pci_addr, size);
	/* [한국어] 그 크기 안에서의 오프셋을 뽑을 마스크를 만든다. */
	mask = (1ULL << num_bits) - 1;

	/* [한국어] 요청 주소가 창 시작에서 얼마나 떨어져 있는지. */
	offset = pci_addr & mask;
	/* [한국어] 오프셋 + 크기가 창 하나(1MB)를 넘으면, */
	if (size + offset > SZ_1M)
		/* [한국어] 창 끝까지로 잘라 낸다. EPF 는 이 축소된 크기를 보고 나눠서 매핑한다. */
		size = SZ_1M - offset;

	/* [한국어] 실제로 매핑할 크기를 하드웨어 정렬 단위로 올림한다. */
	*pci_size = ALIGN(offset + size, ROCKCHIP_PCIE_AT_SIZE_ALIGN);
	/* [한국어] 호출자가 반환 주소에 더해야 할 오프셋을 알려 준다. */
	*addr_offset = offset;

	/* [한국어] 정렬된 창 시작 주소를 돌려준다. 세 값(정렬 주소, 조정된 크기, 오프셋)을
	 * 함께 돌려주는 구조라 호출자가 창 경계를 신경 쓰지 않아도 된다. */
	return pci_addr & ~mask;
}

/* [한국어]
 * rockchip_pcie_ep_map_addr - AXI 주소 영역 하나를 PCI 주소로 이어 준다
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 쓰지 않는다.
 * @addr: AXI(CPU) 쪽 주소. 이 값에서 창 번호가 결정된다.
 * @pci_addr: 대응시킬 PCI 버스 주소.
 * @size: 매핑 크기.
 * @return: 0 = 성공, -EBUSY = 그 창이 이미 사용 중.
 *
 * EP 가 호스트 메모리에 접근하려면 자기 AXI 주소 공간의 어느 구역을 호스트의
 * PCI 주소로 이어 두어야 한다. 그 연결이 아웃바운드 창이고, 이 함수가 하나를 건다.
 *
 * 창 번호를 별도로 관리하지 않고 AXI 주소에서 직접 계산하는 것이 이 설계의
 * 핵심이다(rockchip_ob_region). 그 덕분에 EPC 메모리 관리자가 배정한 주소가
 * 곧 창 번호를 결정하고, 이 함수는 그 창이 비었는지만 확인하면 된다.
 *
 * 중복 매핑을 -EBUSY 로 거절하는 것이 중요하다. 그러지 않으면 앞의 매핑이
 * 조용히 덮여 EPF 가 엉뚱한 곳에 접근하게 된다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 중복 매핑 하나뿐이고, 레지스터를 건드리기 전에 걸러진다.
 *
 * 호출 체인:
 *   EPF → pci_epc_map_addr() (pci-epc-core.c:766)
 *     → pci_epc_ops.map_addr == [이 함수]
 *     → rockchip_pcie_prog_ep_ob_atu()
 */
static int rockchip_pcie_ep_map_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				     phys_addr_t addr, u64 pci_addr,
				     size_t size)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *pcie = &ep->rockchip;
	/* [한국어] AXI 주소에서 창 번호를 계산한다. */
	u32 r = rockchip_ob_region(addr);

	/* [한국어] 이미 그 창이 쓰이고 있으면, */
	if (test_bit(r, &ep->ob_region_map))
		/* [한국어] -EBUSY 로 거절한다. 같은 창을 두 번 매핑하면 앞의 매핑이 조용히 사라진다. */
		return -EBUSY;

	/* [한국어] 창을 실제로 프로그래밍한다. */
	rockchip_pcie_prog_ep_ob_atu(pcie, fn, r, addr, pci_addr, size);

	/* [한국어] 비트맵에 사용 중으로 표시한다. */
	set_bit(r, &ep->ob_region_map);
	/* [한국어] 어느 AXI 주소에 대응하는지 기록해 둔다 — unmap 이 대조에 쓴다. */
	ep->ob_addr[r] = addr;

	/* [한국어] 매핑 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_ep_unmap_addr - 아웃바운드 창 매핑을 해제한다
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호. 쓰지 않는다.
 * @vfn: 가상 함수 번호. 쓰지 않는다.
 * @addr: 해제할 AXI 주소.
 *
 * map_addr 의 역이다. 주소에서 창 번호를 계산하고, 두 가지를 확인한다 —
 * 기록해 둔 주소와 요청 주소가 같은지, 그리고 그 창이 실제로 사용 중인지.
 * 하나라도 어긋나면 잘못된 해제 요청이므로 조용히 돌아간다. 반환형이 void 라
 * 오류를 알릴 방법이 없어서다.
 *
 * 그 확인이 없으면 EPF 의 실수로 남의 창을 지울 수 있다. ob_addr 배열이
 * 단순한 기록용이 아니라 이 검증의 근거라는 점이 중요하다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 위의 조용한 반환이 전부다.
 *
 * 호출 체인:
 *   EPF → pci_epc_unmap_addr() (pci-epc-core.c:741)
 *     → pci_epc_ops.unmap_addr == [이 함수]
 *     → rockchip_pcie_clear_ep_ob_atu()
 */
static void rockchip_pcie_ep_unmap_addr(struct pci_epc *epc, u8 fn, u8 vfn,
					phys_addr_t addr)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 창 번호. */
	u32 r = rockchip_ob_region(addr);

	/* [한국어] 두 가지를 확인한다 — 기록된 주소와 요청 주소가 같은지, 그리고 그 창이
	 * 실제로 사용 중인지. 하나라도 어긋나면 잘못된 해제 요청이므로, */
	if (addr != ep->ob_addr[r] || !test_bit(r, &ep->ob_region_map))
		/* [한국어] 조용히 돌아간다. 반환형이 void 라 오류를 알릴 방법이 없다. */
		return;

	/* [한국어] 창의 네 레지스터를 모두 지운다. */
	rockchip_pcie_clear_ep_ob_atu(rockchip, r);

	/* [한국어] 기록해 둔 주소를 지운다. */
	ep->ob_addr[r] = 0;
	/* [한국어] 비트맵에서 해제한다. */
	clear_bit(r, &ep->ob_region_map);
}

/* [한국어]
 * rockchip_pcie_ep_set_msi - 이 함수가 요청할 MSI 벡터 수를 광고한다
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 쓰지 않는다.
 * @nr_irqs: EPF 가 원하는 벡터 수.
 * @return: 언제나 0.
 *
 * MSI capability 의 MMC(Multiple Message Capable) 필드에 "나는 이만큼의 벡터를
 * 쓸 수 있다"를 적어 둔다. MSI 는 2의 거듭제곱 개수만 지원하므로 log2 가
 * 그대로 인코딩이 된다.
 *
 * 여기서 쓰는 MMC 와, get_msi 가 읽는 MME(Multiple Message Enable)를 구분해야
 * 한다. MMC 는 EP 의 요청이고 MME 는 호스트가 실제로 허용한 수다. 호스트가
 * 요청보다 적게 줄 수 있으므로, 실제로 몇 개를 쓸 수 있는지는 get_msi 로 물어야 한다.
 *
 * 64비트 주소 지원 플래그도 함께 세우고, MSI capability 마스크 비트를 지워
 * 호스트에게 MSI 를 노출한다.
 *
 * [상류 코드 관찰] MMC 필드는 지운 뒤 새로 넣지만 64비트 플래그는 지우지 않고
 * OR 로만 얹는다. 결과는 같지만 두 필드의 처리가 비대칭이다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF → pci_epc_set_msi() (pci-epc-core.c:620)
 *     → pci_epc_ops.set_msi == [이 함수]
 */
static int rockchip_pcie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 vfn,
				    u8 nr_irqs)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 요청 벡터 수를 MMC(Multiple Message Capable) 인코딩으로 바꾼다.
	 * MSI 는 2의 거듭제곱 개수만 지원하므로 log2 가 그대로 인코딩이 된다. */
	u8 mmc = order_base_2(nr_irqs);
	/* [한국어] 읽기-수정-쓰기용 임시 변수. */
	u32 flags;

	/* [한국어] 이 함수의 MSI 제어 레지스터를 읽는다. */
	flags = rockchip_pcie_read(rockchip,
				   ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				   ROCKCHIP_PCIE_EP_MSI_CTRL_REG);
	/* [한국어] MMC 필드를 지운다. */
	flags &= ~ROCKCHIP_PCIE_EP_MSI_CTRL_MMC_MASK;
	/* [한국어] 새 MMC 값과 64비트 주소 지원 표시를 함께 넣는다.
	 * [상류 코드 관찰] 64비트 플래그는 지우지 않고 OR 로만 얹으므로,
	 * 이 함수를 반복 호출해도 그 비트는 계속 서 있게 된다 — 결과는 같지만
	 * MMC 필드만 지우고 나머지는 누적하는 비대칭이 있다. */
	flags |=
	   (mmc << ROCKCHIP_PCIE_EP_MSI_CTRL_MMC_OFFSET) |
	   (PCI_MSI_FLAGS_64BIT << ROCKCHIP_PCIE_EP_MSI_FLAGS_OFFSET);
	/* [한국어] MSI capability 마스크 비트를 지워 MSI 를 노출한다. */
	flags &= ~ROCKCHIP_PCIE_EP_MSI_CTRL_MASK_MSI_CAP;
	/* [한국어] 완성한 값을 되쓴다. */
	rockchip_pcie_write(rockchip, flags,
			    ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
			    ROCKCHIP_PCIE_EP_MSI_CTRL_REG);
	/* [한국어] MSI 설정 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_ep_get_msi - 호스트가 실제로 허용한 MSI 벡터 수를 돌려준다
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 쓰지 않는다.
 * @return: 허용된 벡터 수(1, 2, 4, ...). -EINVAL = 호스트가 아직 MSI 를 켜지 않음.
 *
 * set_msi 가 MMC 에 요청을 적어 두면, 호스트가 열거 과정에서 MME 에 실제 허용
 * 수를 적어 준다. 이 함수는 그 MME 를 읽어 개수로 바꿔 돌려준다.
 *
 * ME(MSI Enable) 비트를 먼저 확인하는 이유는, 호스트가 MSI 를 아예 켜지 않았을
 * 수 있기 때문이다. 그 경우 MME 값에는 의미가 없고, EPF 는 -EINVAL 을 보고
 * INTx 를 쓰거나 링크가 준비될 때까지 기다린다.
 *
 * 실행 컨텍스트: EPC 코어의 콜백 경로, 프로세스 컨텍스트. APB 레지스터 접근뿐이라 잠들지 않는다.
 *
 * 에러 경로: MSI 미활성화 하나뿐이다.
 *
 * 호출 체인:
 *   EPF → pci_epc_get_msi() (pci-epc-core.c:578)
 *     → pci_epc_ops.get_msi == [이 함수]
 */
static int rockchip_pcie_ep_get_msi(struct pci_epc *epc, u8 fn, u8 vfn)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] MSI 제어 레지스터 값. */
	u32 flags;

	/* [한국어] 이 함수의 MSI 제어 레지스터를 읽는다. */
	flags = rockchip_pcie_read(rockchip,
				   ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				   ROCKCHIP_PCIE_EP_MSI_CTRL_REG);
	/* [한국어] ME(MSI Enable) 비트가 서 있지 않으면 호스트가 아직 MSI 를 켜지 않은 것이다. */
	if (!(flags & ROCKCHIP_PCIE_EP_MSI_CTRL_ME))
		/* [한국어] -EINVAL 로 알린다. EPF 는 이 값을 보고 MSI 대신 INTx 를 쓰거나 기다린다. */
		return -EINVAL;

	/* [한국어] MME(Multiple Message Enable)는 호스트가 실제로 허용한 벡터 수의 log2 다.
	 * set_msi 가 쓴 MMC(요청 수)와 구분해야 한다 — 호스트가 요청보다 적게
	 * 허용할 수 있으므로, 실제로 쓸 수 있는 개수는 이쪽이다. */
	return 1 << ((flags & ROCKCHIP_PCIE_EP_MSI_CTRL_MME_MASK) >>
		     ROCKCHIP_PCIE_EP_MSI_CTRL_MME_OFFSET);
}

/* [한국어]
 * rockchip_pcie_ep_assert_intx - 레거시 INTx 선을 어서트하거나 디어서트한다
 *
 * @ep: EP 객체.
 * @fn: 물리 함수 번호. 실제로는 쓰지 않는다 — 이 컨트롤러의 레거시 인터럽트
 *       제어 레지스터가 함수별로 나뉘어 있지 않기 때문이다.
 * @intx: INTA~INTD 중 어느 것인지(0~3). 2비트로 잘라 쓴다.
 * @do_assert: true 면 어서트, false 면 디어서트.
 *
 * 소프트웨어 상태(irq_pending 비트맵)와 하드웨어 레지스터를 함께 갱신하는 것이
 * 이 함수의 일이다. 두 경로가 완전히 대칭이며, 각각 "어서트 + pending 상태"와
 * "디어서트 + 정상 상태" 두 비트를 함께 쓴다.
 *
 * 실행 컨텍스트: raise_irq 콜백 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_send_intx_irq() → [이 함수] → rockchip_pcie_write()
 */
static void rockchip_pcie_ep_assert_intx(struct rockchip_pcie_ep *ep, u8 fn,
					 u8 intx, bool do_assert)
{
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;

	/* [한국어] INTA~INTD 를 표현하는 2비트로 자른다. 실제로는 이 컨트롤러가 INTx 하나만
	 * 지원해 호출자가 언제나 0 을 넘긴다. */
	intx &= 3;

	/* [한국어] 어서트 요청이면, */
	if (do_assert) {
		/* [한국어] 소프트웨어 비트맵에 표시하고, */
		ep->irq_pending |= BIT(intx);
		/* [한국어] 레거시 인터럽트 제어 레지스터에 어서트와 pending 상태를 함께 쓴다. */
		rockchip_pcie_write(rockchip,
				    PCIE_CLIENT_INT_IN_ASSERT |
				    PCIE_CLIENT_INT_PEND_ST_PEND,
				    PCIE_CLIENT_LEGACY_INT_CTRL);
	/* [한국어] 디어서트 요청이면, */
	} else {
		/* [한국어] 비트맵에서 지우고, */
		ep->irq_pending &= ~BIT(intx);
		/* [한국어] 디어서트와 정상 상태를 쓴다. 두 경로가 대칭이며, 소프트웨어 상태와
		 * 하드웨어 레지스터를 함께 갱신한다. */
		rockchip_pcie_write(rockchip,
				    PCIE_CLIENT_INT_IN_DEASSERT |
				    PCIE_CLIENT_INT_PEND_ST_NORMAL,
				    PCIE_CLIENT_LEGACY_INT_CTRL);
	}
}

/* [한국어]
 * rockchip_pcie_ep_send_intx_irq - INTx 펄스를 한 번 보낸다
 *
 * @ep: EP 객체.
 * @fn: 물리 함수 번호.
 * @intx: INTx 번호. 호출자가 언제나 0 을 넘긴다.
 * @return: 0 = 성공, -EINVAL = 호스트가 INTx 를 비활성화해 둠.
 *
 * 먼저 Command 레지스터의 INTX_DISABLE 비트를 확인한다. 그 비트가 1 이면
 * 호스트가 이 함수의 INTx 를 꺼 둔 것이므로 보내면 안 된다. 값이 1 일 때
 * "비활성"이라는 역극성에 주의해야 한다.
 *
 * 그다음 어서트 → 1ms 대기 → 디어서트로 한 번의 펄스를 만든다. 위 영어 주석이
 * 그 대기의 근거를 밝힌다 — TRM 이 "AHB 버스 클럭 몇 사이클이 필요하다"고
 * 모호하게만 적어 두어, 충분한 값으로 1ms 를 넣었다는 것이다.
 *
 * [상류 코드 관찰] mdelay 는 바쁜 대기라 이 1ms 동안 CPU 를 점유한다.
 * raise_irq 는 EPF 드라이버가 자주 부를 수 있는 경로여서 비용이 작지 않지만,
 * INTx 자체가 성능보다 호환성을 위한 수단이라 상류가 감수한 것으로 보인다.
 *
 * 실행 컨텍스트: raise_irq 콜백 경로, 프로세스 컨텍스트. mdelay 때문에
 * 1ms 동안 CPU 를 놓지 않는다.
 *
 * 에러 경로: INTX_DISABLE 확인 하나뿐이다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_raise_irq() → [이 함수]
 *     → rockchip_pcie_ep_assert_intx() ×2 → mdelay(1)
 */
static int rockchip_pcie_ep_send_intx_irq(struct rockchip_pcie_ep *ep, u8 fn,
					  u8 intx)
{
	/* [한국어] Command 레지스터 값. */
	u16 cmd;

	/* [한국어] 이 함수의 Command/Status 레지스터를 읽는다. */
	cmd = rockchip_pcie_read(&ep->rockchip,
				 ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				 ROCKCHIP_PCIE_EP_CMD_STATUS);

	/* [한국어] 호스트가 INTx 를 비활성화해 두었으면 보내면 안 된다.
	 * PCI_COMMAND_INTX_DISABLE 은 1 이 "비활성"이라는 점에 주의한다. */
	if (cmd & PCI_COMMAND_INTX_DISABLE)
		/* [한국어] -EINVAL 로 거절한다. */
		return -EINVAL;

	/*
	 * Should add some delay between toggling INTx per TRM vaguely saying
	 * it depends on some cycles of the AHB bus clock to function it. So
	 * add sufficient 1ms here.
	 */
	/* [한국어] 어서트한다. */
	rockchip_pcie_ep_assert_intx(ep, fn, intx, true);
	/* [한국어] 위 영어 주석대로 TRM 이 "AHB 버스 클럭 몇 사이클이 필요하다"고 모호하게만
	 * 적어 두어, 충분한 값으로 1ms 를 넣었다.
	 * [상류 코드 관찰] mdelay 는 바쁜 대기라 이 1ms 동안 CPU 를 점유한다.
	 * raise_irq 는 EPF 드라이버가 자주 부를 수 있는 경로여서 비용이 작지 않다. */
	mdelay(1);
	/* [한국어] 디어서트한다. 어서트-대기-디어서트가 한 번의 INTx 펄스를 만든다. */
	rockchip_pcie_ep_assert_intx(ep, fn, intx, false);
	/* [한국어] 전송 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_ep_send_msi_irq - MSI 를 한 번 보낸다
 *
 * @ep: EP 객체.
 * @fn: 물리 함수 번호.
 * @interrupt_num: 보낼 벡터 번호(1 기반).
 * @return: 0 = 성공. -EINVAL = MSI 미활성화 또는 벡터 번호가 범위를 벗어남.
 *
 * MSI 는 "호스트가 지정한 주소에 호스트가 지정한 데이터를 쓰는" 것으로
 * 인터럽트를 표현한다. 그 주소와 데이터는 호스트가 열거 과정에서 EP 의 MSI
 * capability 레지스터에 써 둔 것이므로, EP 는 그것을 읽어 그대로 실행하면 된다.
 *
 * 동작 과정:
 *   1) ME 비트로 MSI 가 켜져 있는지 확인한다.
 *   2) MME 에서 호스트가 허용한 벡터 수를 구하고, 요청 번호가 그 범위인지 본다.
 *   3) 호스트가 써 둔 MSI 데이터를 읽어, 하위 log2(N) 비트만 벡터 번호로
 *      교체한다. 상위 비트는 호스트가 정한 값이라 보존해야 한다 — MSI 규약상
 *      하위 몇 비트만 벡터 구분에 쓰이기 때문이다.
 *   4) 호스트가 써 둔 64비트 목적지 주소를 상·하위 레지스터에서 조립한다.
 *   5) 전용 아웃바운드 창을 그 주소로 매핑한다. 이미 같은 주소·같은 함수로
 *      매핑되어 있으면 건너뛴다 — unlikely 를 붙인 이 최적화가 없으면
 *      MSI 를 보낼 때마다 레지스터 네 개를 다시 써야 한다.
 *   6) 전용 창의 CPU 주소에 데이터를 16비트로 쓴다. 이 한 번의 쓰기가 링크
 *      너머로 메모리 쓰기 TLP 가 되어 나가고, 호스트에게는 인터럽트로 보인다.
 *
 * 전용 창을 따로 두는 이유는, EPF 가 쓰는 일반 창과 섞이면 MSI 를 보낼 때마다
 * 남의 매핑을 밀어내야 하기 때문이다. probe 가 init_ob_mem 에서 1MB 를 미리
 * 확보해 두는 것이 그 준비다.
 *
 * 실행 컨텍스트: raise_irq 콜백 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 검사 모두 레지스터를 건드리기 전에 걸러진다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_raise_irq() → [이 함수]
 *     → rockchip_pcie_ep_align_addr() → rockchip_pcie_prog_ep_ob_atu()
 *     → writew(irq_cpu_addr + ...)
 */
static int rockchip_pcie_ep_send_msi_irq(struct rockchip_pcie_ep *ep, u8 fn,
					 u8 interrupt_num)
{
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] flags: MSI 제어. mme: 허용 벡터 수의 log2. data: MSI 데이터. data_mask: 그 하위 비트 마스크. */
	u32 flags, mme, data, data_mask;
	/* [한국어] 전용 창 매핑 계산에 쓸 크기와 오프셋. */
	size_t irq_pci_size, offset;
	/* [한국어] 정렬된 PCI 주소. */
	u64 irq_pci_addr;
	/* [한국어] 호스트가 허용한 벡터 수. */
	u8 msi_count;
	/* [한국어] 호스트가 알려 준 MSI 목적지 주소. */
	u64 pci_addr;

	/* Check MSI enable bit */
	/* [한국어] MSI 가 켜져 있는지 확인한다(옆의 상류 주석). */
	flags = rockchip_pcie_read(&ep->rockchip,
				   ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				   ROCKCHIP_PCIE_EP_MSI_CTRL_REG);
	/* [한국어] ME 비트가 없으면, */
	if (!(flags & ROCKCHIP_PCIE_EP_MSI_CTRL_ME))
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* Get MSI numbers from MME */
	/* [한국어] MME 에서 허용 벡터 수의 log2 를 뽑는다(옆의 상류 주석). */
	mme = ((flags & ROCKCHIP_PCIE_EP_MSI_CTRL_MME_MASK) >>
			ROCKCHIP_PCIE_EP_MSI_CTRL_MME_OFFSET);
	/* [한국어] 실제 개수로 바꾼다. */
	msi_count = 1 << mme;
	/* [한국어] 벡터 번호가 1 기반이며 허용 범위를 벗어나면 안 된다. */
	if (!interrupt_num || interrupt_num > msi_count)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* Set MSI private data */
	/* [한국어] MSI 데이터의 하위 몇 비트가 벡터 번호를 나타내는지 정하는 마스크(옆의 상류 주석).
	 * 벡터가 4개면 하위 2비트다. */
	data_mask = msi_count - 1;
	/* [한국어] 호스트가 써 둔 MSI 데이터 값을 읽는다. */
	data = rockchip_pcie_read(rockchip,
				  ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				  ROCKCHIP_PCIE_EP_MSI_CTRL_REG +
				  PCI_MSI_DATA_64);
	/* [한국어] 하위 비트만 벡터 번호로 교체한다. 상위 비트는 호스트가 정한 값이므로 보존한다 —
	 * MSI 규약상 데이터의 하위 log2(N) 비트만 벡터 구분에 쓰이기 때문이다. */
	data = (data & ~data_mask) | ((interrupt_num - 1) & data_mask);

	/* Get MSI PCI address */
	/* [한국어] 호스트가 써 둔 MSI 목적지 주소의 상위 32비트를 읽는다(옆의 상류 주석). */
	pci_addr = rockchip_pcie_read(rockchip,
				      ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				      ROCKCHIP_PCIE_EP_MSI_CTRL_REG +
				      PCI_MSI_ADDRESS_HI);
	/* [한국어] 상위로 밀어 올리고, */
	pci_addr <<= 32;
	/* [한국어] 하위 32비트를 OR 로 붙여 64비트 주소를 완성한다. */
	pci_addr |= rockchip_pcie_read(rockchip,
				       ROCKCHIP_PCIE_EP_FUNC_BASE(fn) +
				       ROCKCHIP_PCIE_EP_MSI_CTRL_REG +
				       PCI_MSI_ADDRESS_LO);

	/* Set the outbound region if needed. */
	/* [한국어] 전용 창이 덮어야 할 크기. PCIE_ADDR_MASK 의 반전에 1 을 더한 것이므로
	 * 마스크가 걸러 내는 하위 비트 범위 전체를 뜻한다. */
	irq_pci_size = ~PCIE_ADDR_MASK + 1;
	/* [한국어] 그 주소를 창 경계에 맞춰 정렬하고, 창 안에서의 오프셋을 얻는다(옆의 상류 주석). */
	irq_pci_addr = rockchip_pcie_ep_align_addr(ep->epc,
						   pci_addr & PCIE_ADDR_MASK,
						   &irq_pci_size, &offset);
	/* [한국어] 이미 같은 주소·같은 함수로 매핑되어 있으면 다시 프로그래밍할 필요가 없다.
	 * unlikely 를 붙인 것은 대개 같은 목적지로 연속해서 인터럽트를 보내기 때문이다 —
	 * 이 최적화가 없으면 MSI 를 보낼 때마다 레지스터 네 개를 다시 써야 한다. */
	if (unlikely(ep->irq_pci_addr != irq_pci_addr ||
		     ep->irq_pci_fn != fn)) {
		/* [한국어] 전용 창을 새 주소로 다시 프로그래밍한다. */
		rockchip_pcie_prog_ep_ob_atu(rockchip, fn,
					rockchip_ob_region(ep->irq_phys_addr),
					ep->irq_phys_addr,
					irq_pci_addr, irq_pci_size);
		/* [한국어] 현재 매핑 주소를 기록해 다음 호출의 판정에 쓴다. */
		ep->irq_pci_addr = irq_pci_addr;
		/* [한국어] 어느 함수의 매핑인지도 기록한다 — 창은 하나뿐인데 함수는 여럿일 수 있어
		 * 주소만으로는 부족하다. */
		ep->irq_pci_fn = fn;
	}

	/* [한국어] 전용 창의 CPU 가상 주소에 MSI 데이터를 16비트로 쓴다.
	 * 이 한 번의 쓰기가 링크 너머로 메모리 쓰기 TLP 가 되어 나가고,
	 * 호스트에게는 MSI 인터럽트로 보인다. offset 은 정렬 때문에 생긴 차이이고,
	 * (pci_addr & ~PCIE_ADDR_MASK) 는 창 안에서의 최종 위치다. */
	writew(data, ep->irq_cpu_addr + offset + (pci_addr & ~PCIE_ADDR_MASK));
	return 0;
}

/* [한국어]
 * rockchip_pcie_ep_raise_irq - EPF 의 요청을 받아 INTx 또는 MSI 를 보낸다
 *
 * @epc: EPC 객체.
 * @fn: 물리 함수 번호.
 * @vfn: 가상 함수 번호. 쓰지 않는다.
 * @type: 인터럽트 종류(PCI_IRQ_INTX / PCI_IRQ_MSI).
 * @interrupt_num: MSI 벡터 번호. INTx 에서는 쓰지 않는다.
 * @return: 하위 함수의 결과, 또는 -EINVAL(지원하지 않는 종류).
 *
 * 두 갈래로 나누는 얇은 분배기다. INTx 는 번호를 항상 0 으로 고정해 넘기는데,
 * 이 컨트롤러가 INTx 선을 하나만 지원하기 때문이다.
 *
 * MSI-X 갈래가 없는 것은 get_features 가 msix_capable 을 켜지 않는 것과
 * 일관된다 — 이 컨트롤러는 MSI-X 를 지원하지 않고, 오히려
 * hide_broken_msix_cap() 으로 그 capability 를 호스트에게서 감춘다.
 *
 * 실행 컨텍스트: EPF 드라이버의 인터럽트 요청 경로, 프로세스 컨텍스트.
 * INTx 갈래는 mdelay 로 1ms 를 소비한다.
 *
 * 에러 경로: 지원하지 않는 종류만 자체 오류이고, 나머지는 하위 결과를 전달한다.
 *
 * 호출 체인:
 *   EPF 드라이버 → pci_epc_raise_irq() (pci-epc-core.c:484)
 *     → pci_epc_ops.raise_irq == [이 함수]
 *     → send_intx_irq() 또는 send_msi_irq()
 */
static int rockchip_pcie_ep_raise_irq(struct pci_epc *epc, u8 fn, u8 vfn,
				      unsigned int type, u16 interrupt_num)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);

	/* [한국어] 요청받은 인터럽트 종류에 따라 분기한다. */
	switch (type) {
	/* [한국어] 레거시 INTx 요청. */
	case PCI_IRQ_INTX:
		/* [한국어] INTx 번호는 항상 0 — 이 컨트롤러가 하나만 지원한다. */
		return rockchip_pcie_ep_send_intx_irq(ep, fn, 0);
	/* [한국어] MSI 요청. */
	case PCI_IRQ_MSI:
		/* [한국어] 벡터 번호를 그대로 넘긴다. */
		return rockchip_pcie_ep_send_msi_irq(ep, fn, interrupt_num);
	/* [한국어] MSI-X 등 그 밖의 종류는, */
	default:
		/* [한국어] -EINVAL 로 거절한다. get_features 가 msix_capable 을 켜지 않으므로
		 * 정상 경로에서는 여기 도달하지 않는다. */
		return -EINVAL;
	}
}

/* [한국어]
 * rockchip_pcie_ep_start - 함수 집합을 확정하고 링크 훈련을 시작한다
 *
 * @epc: EPC 객체.
 * @return: 언제나 0.
 *
 * EPF 드라이버들이 모두 바인딩된 뒤 EPC 코어가 부른다. 하는 일은 셋이다.
 *
 *   1) 바인딩된 EPF 들을 순회하며 물리 함수 활성화 비트맵을 만든다. 함수 0 은
 *      항상 켜고, 각 EPF 의 함수 번호 비트를 더한다. 이 비트맵이 호스트에게
 *      보일 PCI 함수 집합을 결정한다 — probe 시점에는 함수 0 만 켜 두었다가
 *      여기서 실제 구성으로 갱신하는 구조다.
 *   2) PERST# GPIO 가 있으면 그 인터럽트를 켠다. probe 가 IRQ_NOAUTOEN 으로
 *      등록해 두었기 때문에 이 시점까지는 받지 않았다.
 *   3) config 접근을 허용하고 링크 훈련을 시작한다. 두 비트를 함께 세우는 것이
 *      중요하다 — 훈련이 끝나기 전에 호스트가 config 를 읽을 수 있어야 열거가
 *      성립하기 때문이다.
 *
 * PERST# GPIO 가 없는 보드에서는 리셋 해제를 감지할 방법이 없으므로 곧바로
 * 훈련 워크를 예약한다. GPIO 가 있으면 PERST# 인터럽트가 그 역할을 대신한다.
 *
 * 실행 컨텍스트: EPC 코어의 start 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자/EPF → pci_epc_start() (pci-epc-core.c:434)
 *     → pci_epc_ops.start == [이 함수]
 *     → enable_irq() / schedule_delayed_work()
 */
static int rockchip_pcie_ep_start(struct pci_epc *epc)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 등록된 EP 함수 순회용. */
	struct pci_epf *epf;
	/* [한국어] 물리 함수 활성화 비트맵. */
	u32 cfg;

	/* [한국어] 함수 0 은 항상 활성이다. */
	cfg = BIT(0);
	/* [한국어] EPC 에 바인딩된 EPF 들을 순회하며, */
	list_for_each_entry(epf, &epc->pci_epf, list)
		/* [한국어] 각각의 함수 번호 비트를 켠다. 이렇게 만든 비트맵이 호스트에게 보일
		 * PCI 함수 집합을 결정한다. */
		cfg |= BIT(epf->func_no);

	/* [한국어] 물리 함수 설정 레지스터에 쓴다. */
	rockchip_pcie_write(rockchip, cfg, PCIE_CORE_PHY_FUNC_CFG);

	/* [한국어] PERST# GPIO 가 있는 보드라면, */
	if (rockchip->perst_gpio)
		/* [한국어] 그 인터럽트를 켠다. 호스트가 리셋을 걸고 푸는 것을 감지해야 하기 때문이다. */
		enable_irq(ep->perst_irq);

	/* Enable configuration and start link training */
	/* [한국어] config 접근을 허용하고 링크 훈련을 시작한다(옆의 상류 주석).
	 * 두 비트를 함께 세우는 순서가 중요하다 — 훈련이 끝나기 전에 호스트가
	 * config 를 읽을 수 있어야 열거가 성립한다. */
	rockchip_pcie_write(rockchip,
			    PCIE_CLIENT_LINK_TRAIN_ENABLE |
			    PCIE_CLIENT_CONF_ENABLE,
			    PCIE_CLIENT_CONFIG);

	/* [한국어] PERST# GPIO 가 없는 보드에서는 리셋 해제를 감지할 방법이 없으므로, */
	if (!rockchip->perst_gpio)
		/* [한국어] 곧바로 링크 훈련 워크를 예약한다. GPIO 가 있으면 PERST# 인터럽트가
		 * 그 역할을 대신한다. */
		schedule_delayed_work(&ep->link_training, 0);

	/* [한국어] 시작 완료. */
	return 0;
}

/* [한국어]
 * rockchip_pcie_ep_stop - 링크 훈련과 config 접근을 멈춘다
 *
 * @epc: EPC 객체.
 *
 * start 의 역이다. 순서가 이 함수의 핵심이다.
 *
 *   1) PERST# GPIO 가 있으면 먼저 perst_asserted 플래그를 세우고 인터럽트를 끈다.
 *      플래그를 먼저 세워야, 이미 실행 중인 훈련 워크가 링크 업을 통지하지 않고
 *      빠져나간다.
 *   2) 훈련 워크를 취소하고 끝날 때까지 기다린다. _sync 판이라 이 호출이
 *      돌아온 뒤에는 워크가 실행 중이 아님이 보장된다.
 *   3) config 접근을 막고 링크 훈련도 멈춘다 — start 와 정확히 반대되는 두 비트다.
 *
 * 실행 컨텍스트: EPC 코어의 stop 경로, 프로세스 컨텍스트.
 * cancel_delayed_work_sync 때문에 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   사용자/EPF → pci_epc_stop() (pci-epc-core.c:410)
 *     → pci_epc_ops.stop == [이 함수]
 *     → disable_irq() → cancel_delayed_work_sync() → rockchip_pcie_write()
 */
static void rockchip_pcie_ep_stop(struct pci_epc *epc)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;

	/* [한국어] PERST# GPIO 가 있으면, */
	if (rockchip->perst_gpio) {
		/* [한국어] 어서트 상태로 표시해 워크가 훈련을 진행하지 않게 하고, */
		ep->perst_asserted = true;
		/* [한국어] 인터럽트를 끈다. 순서가 중요하다 — 플래그를 먼저 세워야 이미 실행 중인
		 * 핸들러가 잘못된 판단을 하지 않는다. */
		disable_irq(ep->perst_irq);
	}

	/* [한국어] 예약된 훈련 워크가 끝날 때까지 기다리며 취소한다. _sync 판이라
	 * 이 호출이 돌아온 뒤에는 워크가 실행 중이 아님이 보장된다. */
	cancel_delayed_work_sync(&ep->link_training);

	/* Stop link training and disable configuration */
	/* [한국어] config 접근을 막고 링크 훈련도 멈춘다(옆의 상류 주석).
	 * start 와 정확히 반대되는 두 비트다. */
	rockchip_pcie_write(rockchip,
			    PCIE_CLIENT_CONF_DISABLE |
			    PCIE_CLIENT_LINK_TRAIN_DISABLE,
			    PCIE_CLIENT_CONFIG);
}

/* [한국어]
 * rockchip_pcie_ep_retrain_link - 링크 재훈련을 요청한다
 *
 * @rockchip: 공용 컨트롤러 상태.
 *
 * EP 자신의 config 공간에 있는 Link Control 에서 Retrain Link 비트를 세운다.
 * 이 쓰기가 곧 재훈련 트리거다.
 *
 * host 판이 PCIE_RC_CONFIG_CR 을 베이스로 쓰는 것과 달리 여기서는
 * PCIE_EP_CONFIG_BASE 를 쓴다 — 같은 IP 라도 RC 와 EP 의 config 공간이
 * 서로 다른 오프셋에 놓이기 때문이다.
 *
 * 두 곳에서 불린다. 링크 훈련 워크가 Gen2 로 올리려 할 때, 그리고 PERST# 가
 * 해제되어 훈련을 다시 시작할 때다.
 *
 * 실행 컨텍스트: 워크큐 또는 인터럽트 스레드 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_link_training() / rockchip_pcie_ep_perst_deassert()
 *     → [이 함수] → rockchip_pcie_read/write(PCIE_EP_CONFIG_BASE + PCI_EXP_LNKCTL)
 */
static void rockchip_pcie_ep_retrain_link(struct rockchip_pcie *rockchip)
{
	/* [한국어] 읽기-수정-쓰기용 임시 변수. */
	u32 status;

	/* [한국어] EP 자신의 config 공간에 있는 Link Control 을 읽는다. host 판이
	 * PCIE_RC_CONFIG_CR 을 쓰는 것과 대응하는 EP 쪽 베이스다. */
	status = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_BASE + PCI_EXP_LNKCTL);
	/* [한국어] Retrain Link 비트를 세운다. */
	status |= PCI_EXP_LNKCTL_RL;
	/* [한국어] 되쓴다 — 이 쓰기가 링크 재훈련을 유발한다. */
	rockchip_pcie_write(rockchip, status, PCIE_EP_CONFIG_BASE + PCI_EXP_LNKCTL);
}

/* [한국어]
 * rockchip_pcie_ep_link_up - 링크가 올라와 있는지 확인한다
 *
 * @rockchip: 공용 컨트롤러 상태.
 * @return: true = 링크 업, false = 아니면.
 *
 * 클라이언트 기본 상태 레지스터의 링크 업 비트 하나를 읽어 bool 로 정규화한다.
 * host 판이 SMLH 와 RDLH 두 비트를 모두 확인하는 것과 달리 여기서는 하나만
 * 보는데, EP 쪽 상태 레지스터가 그 둘을 이미 합쳐 제공하기 때문으로 보인다.
 *
 * 실행 컨텍스트: 워크큐 문맥. 읽기 한 번이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_link_training() → [이 함수]
 */
static bool rockchip_pcie_ep_link_up(struct rockchip_pcie *rockchip)
{
	/* [한국어] 클라이언트 기본 상태 레지스터를 읽는다. */
	u32 val = rockchip_pcie_read(rockchip, PCIE_CLIENT_BASIC_STATUS1);

	/* [한국어] 링크 업 비트를 bool 로 정규화해 돌려준다. */
	return PCIE_LINK_UP(val);
}

/* [한국어]
 * rockchip_pcie_ep_link_training - 링크 훈련을 폴링하고 성립하면 EPF 에 알린다
 *
 * @work: delayed_work 안의 work 필드. container_of 로 EP 객체를 되찾는다.
 *
 * 이 파일에서 가장 중요한 상태 기계다. EP 는 호스트가 언제 링크를 올릴지 알 수
 * 없으므로, 실패를 오류로 보지 않고 5ms 뒤에 자기 자신을 다시 예약하며 계속
 * 기다린다. 그 재시도 구조가 이 함수의 뼈대다.
 *
 * 동작 과정:
 *   1) Gen1 훈련 완료를 폴링한다. 실패하면 재시도.
 *   2) 링크가 실제로 올라왔는지 따로 확인한다. 실패하면 재시도.
 *   3) Gen2 를 원했는데 아직 Gen1 이면 재훈련을 요청하고 폴링한다. 여기서는
 *      실패해도 재시도하지 않는다 — 속도가 낮을 뿐 링크는 살아 있기 때문이다.
 *   4) 재훈련 과정에서 링크가 내려갔을 수 있으므로 한 번 더 확인한다.
 *   5) 위 영어 주석이 짚는 경쟁 조건 처리 — 폴링하는 동안 호스트가 PERST# 를
 *      걸었다면 이 링크는 의미가 없다. 그 경우 재시도도 하지 않고 조용히 끝낸다.
 *      PERST# 해제 시 다시 예약될 것이기 때문이다.
 *   6) 협상된 속도와 폭을 로그로 남기고, pci_epc_linkup() 으로 EPF 에 알린다.
 *      host 판이 같은 정보를 dev_dbg 로 찍는 것과 달리 dev_info 를 쓰는데,
 *      EP 에게는 링크 성립이 곧 서비스 시작이라 관리자가 알아야 할 사건이기 때문이다.
 *
 * 워크큐로 미루는 이유: 폴링에 최대 수십 밀리초가 걸릴 수 있어 인터럽트
 * 문맥에서는 할 수 없다. PERST# 핸들러가 스레드 핸들러인 것도 같은 이유다.
 *
 * 실행 컨텍스트: 워크큐 문맥, 프로세스 컨텍스트. readl_poll_timeout 이 잠든다.
 *
 * 에러 경로: 오류를 돌려주지 않는다. 실패는 곧 재시도이며, 유일하게 재시도하지
 * 않는 경우가 PERST# 어서트다.
 *
 * 호출 체인:
 *   schedule_delayed_work() ← rockchip_pcie_ep_start() / perst_deassert() / 자기 자신
 *     → [이 함수] → readl_poll_timeout() ×2~3
 *     → rockchip_pcie_ep_retrain_link() → pci_epc_linkup()
 */
static void rockchip_pcie_ep_link_training(struct work_struct *work)
{
	/* [한국어] work_struct 에서 이 EP 객체로 되돌아간다. delayed_work 안의 work 필드를
	 * 기준으로 삼아야 하므로 link_training.work 를 멤버로 지정한다. */
	struct rockchip_pcie_ep *ep =
		container_of(work, struct rockchip_pcie_ep, link_training.work);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 폴링용 레지스터 값. */
	u32 val;
	/* [한국어] 폴링 결과. */
	int ret;

	/* Enable Gen1 training and wait for its completion */
	/* [한국어] Gen1 훈련 완료를 50us 간격으로 폴링한다(옆의 상류 주석). */
	ret = readl_poll_timeout(rockchip->apb_base + PCIE_CORE_CTRL,
				 val, PCIE_LINK_TRAINING_DONE(val), 50,
				 LINK_TRAIN_TIMEOUT);
	/* [한국어] 시간 안에 끝나지 않으면, */
	if (ret)
		/* [한국어] 재시도 구간으로 — 이 워크 자체를 5ms 뒤에 다시 예약한다.
		 * EP 는 호스트가 언제 훈련을 시작할지 알 수 없으므로, 실패를 오류로 보지 않고
		 * 계속 기다리는 것이 옳다. */
		goto again;

	/* Make sure that the link is up */
	/* [한국어] 훈련이 끝났어도 링크가 실제로 올라왔는지 따로 확인한다(옆의 상류 주석). */
	ret = readl_poll_timeout(rockchip->apb_base + PCIE_CLIENT_BASIC_STATUS1,
				 val, PCIE_LINK_UP(val), 50,
				 LINK_TRAIN_TIMEOUT);
	/* [한국어] 아직이면, */
	if (ret)
		/* [한국어] 재시도. */
		goto again;

	/*
	 * Check the current speed: if gen2 speed was requested and we are not
	 * at gen2 speed yet, retrain again for gen2.
	 */
	/* [한국어] 현재 링크 속도를 읽는다(옆의 상류 주석). */
	val = rockchip_pcie_read(rockchip, PCIE_CORE_CTRL);
	/* [한국어] Gen2 를 원했는데 아직 Gen1 이면, */
	if (!PCIE_LINK_IS_GEN2(val) && rockchip->link_gen == 2) {
		/* Enable retrain for gen2 */
		/* [한국어] 재훈련을 요청하고, */
		rockchip_pcie_ep_retrain_link(rockchip);
		/* [한국어] Gen2 가 될 때까지 폴링한다. 반환값을 검사하지 않는 것은 host 판과 같은
		 * 판단이다 — 속도가 낮을 뿐 링크는 살아 있으므로 실패로 보지 않는다. */
		readl_poll_timeout(rockchip->apb_base + PCIE_CORE_CTRL,
				   val, PCIE_LINK_IS_GEN2(val), 50,
				   LINK_TRAIN_TIMEOUT);
	}

	/* Check again that the link is up */
	/* [한국어] 재훈련 과정에서 링크가 내려갔을 수 있으므로 한 번 더 확인한다(옆의 상류 주석). */
	if (!rockchip_pcie_ep_link_up(rockchip))
		/* [한국어] 내려갔으면 재시도. */
		goto again;

	/*
	 * If PERST# was asserted while polling the link, do not notify
	 * the function.
	 */
	/* [한국어] 위 영어 주석대로, 폴링하는 동안 호스트가 PERST# 를 걸었다면 링크는
	 * 의미가 없다. 그 경우 EPF 에 링크 업을 알리면 안 된다. */
	if (ep->perst_asserted)
		/* [한국어] 재시도도 하지 않고 조용히 끝낸다 — PERST# 해제 시 다시 예약될 것이기 때문이다. */
		return;

	/* [한국어] 협상된 속도와 폭을 읽는다. */
	val = rockchip_pcie_read(rockchip, PCIE_CLIENT_BASIC_STATUS0);
	/* [한국어] 결과를 정보 로그로 남긴다. host 판이 dev_dbg 를 쓰는 것과 달리 dev_info 라
	 * 기본 빌드에서도 보인다 — EP 는 링크 성립이 곧 서비스 시작이라
	 * 관리자가 알아야 할 사건이기 때문이다. */
	dev_info(dev,
		 "link up (negotiated speed: %sGT/s, width: x%lu)\n",
		 /* [한국어] 속도 비트가 서 있으면 5GT/s(Gen2), 아니면 2.5GT/s(Gen1). */
		 (val & PCIE_CLIENT_NEG_LINK_SPEED) ? "5" : "2.5",
		 /* [한국어] 폭 필드를 뽑아 1비트 왼쪽으로 민다 — 코드 값의 2배가 실제 레인 수라는 뜻이다. */
		 ((val & PCIE_CLIENT_NEG_LINK_WIDTH_MASK) >>
		  PCIE_CLIENT_NEG_LINK_WIDTH_SHIFT) << 1);

	/* Notify the function */
	/* [한국어] EPC 코어를 통해 바인딩된 EPF 드라이버들에게 링크가 올라왔음을 알린다.
	 * EPF 는 이 통지를 받고서야 호스트와의 통신을 시작한다. */
	pci_epc_linkup(ep->epc);
	/* [한국어] 내부 상태도 갱신한다 — PERST# 어서트 시 linkdown 을 보낼지 판정하는 데 쓴다. */
	ep->link_up = true;

	return;

/* [한국어] 세 곳에서 오는 재시도 구간. */
again:
	/* [한국어] 5ms 뒤에 이 워크를 다시 예약한다. 호스트가 준비될 때까지 무한히 반복하는
	 * 구조이며, EP 라는 역할상 그것이 올바른 동작이다. */
	schedule_delayed_work(&ep->link_training, msecs_to_jiffies(5));
}

/* [한국어]
 * rockchip_pcie_ep_perst_assert - 호스트가 PERST# 를 걸었을 때 링크를 정리한다
 *
 * @ep: EP 객체.
 *
 * 호스트가 리셋을 걸면 링크가 끊기므로, 진행 중인 훈련을 멈추고 EPF 에
 * 링크 다운을 알려야 한다.
 *
 * 순서가 중요하다.
 *   1) 이미 어서트 상태면 중복 처리이므로 돌아간다.
 *   2) perst_asserted 플래그를 먼저 세운다. 이 플래그를 훈련 워크가 보고
 *      링크 업 통지를 건너뛰기 때문에, 취소보다 앞서야 한다.
 *   3) 훈련 워크를 취소하고 끝날 때까지 기다린다.
 *   4) 링크가 올라와 있었다면 EPF 에 내려갔음을 알리고 내부 상태를 갱신한다.
 *
 * link_up 플래그를 따로 두는 이유는, 링크가 올라온 적이 없는데 linkdown 을
 * 통지하면 EPF 가 혼란스럽기 때문이다.
 *
 * 실행 컨텍스트: PERST# 인터럽트 스레드 문맥, 프로세스 컨텍스트.
 * cancel_delayed_work_sync 때문에 잠들 수 있다 — 스레드 핸들러여야 하는 이유다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_perst_irq_thread() → [이 함수]
 *     → cancel_delayed_work_sync() → pci_epc_linkdown()
 */
static void rockchip_pcie_ep_perst_assert(struct rockchip_pcie_ep *ep)
{
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;

	/* [한국어] 어서트 사실을 디버그 로그로 남긴다. */
	dev_dbg(rockchip->dev, "PERST# asserted, link down\n");

	/* [한국어] 이미 어서트 상태면 중복 처리다. */
	if (ep->perst_asserted)
		/* [한국어] 조용히 돌아간다. */
		return;

	/* [한국어] 어서트 상태로 표시한다. 이 플래그를 먼저 세워야, 실행 중인 훈련 워크가
	 * 링크 업을 통지하지 않고 빠져나간다. */
	ep->perst_asserted = true;

	/* [한국어] 훈련 워크를 취소하고 끝날 때까지 기다린다. 위에서 플래그를 먼저 세웠기 때문에
	 * 워크가 잘못된 통지를 남길 위험이 없다. */
	cancel_delayed_work_sync(&ep->link_training);

	/* [한국어] 링크가 올라와 있었다면, */
	if (ep->link_up) {
		/* [한국어] EPF 에게 내려갔음을 알리고, */
		pci_epc_linkdown(ep->epc);
		/* [한국어] 내부 상태를 갱신한다. */
		ep->link_up = false;
	}
}

/* [한국어]
 * rockchip_pcie_ep_perst_deassert - 호스트가 PERST# 를 풀었을 때 훈련을 다시 시작한다
 *
 * @ep: EP 객체.
 *
 * assert 와 대칭이다. 중복 처리를 걸러 내고, 플래그를 내리고, 재훈련을 요청한 뒤
 * 훈련 워크를 지연 없이 예약한다.
 *
 * 실제 폴링을 워크로 미루는 것이 핵심이다. 이 함수는 인터럽트 스레드 문맥에서
 * 불리므로, 여기서 직접 수십 밀리초를 폴링하면 다른 인터럽트 처리가 밀린다.
 *
 * 실행 컨텍스트: PERST# 인터럽트 스레드 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_perst_irq_thread() → [이 함수]
 *     → rockchip_pcie_ep_retrain_link() → schedule_delayed_work()
 */
static void rockchip_pcie_ep_perst_deassert(struct rockchip_pcie_ep *ep)
{
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;

	/* [한국어] 해제 사실을 디버그 로그로 남긴다. */
	dev_dbg(rockchip->dev, "PERST# de-asserted, starting link training\n");

	/* [한국어] 어서트 상태가 아니었다면 중복 처리다. */
	if (!ep->perst_asserted)
		/* [한국어] 조용히 돌아간다. */
		return;

	/* [한국어] 해제 상태로 표시한다. */
	ep->perst_asserted = false;

	/* Enable link re-training */
	/* [한국어] 재훈련을 요청한다(옆의 상류 주석). */
	rockchip_pcie_ep_retrain_link(rockchip);

	/* Start link training */
	/* [한국어] 훈련 워크를 지연 없이 예약한다. 실제 폴링은 워크 문맥에서 이루어지므로
	 * 인터럽트 스레드가 오래 붙잡히지 않는다. */
	schedule_delayed_work(&ep->link_training, 0);
}

/* [한국어]
 * rockchip_pcie_ep_perst_irq_thread - PERST# 신호 변화를 처리하는 스레드 핸들러
 *
 * @irq: 인터럽트 번호. 사용하지 않는다.
 * @data: devm_request_threaded_irq 에 넘긴 EPC 객체.
 * @return: 언제나 IRQ_HANDLED.
 *
 * GPIO 의 현재 값을 읽어 어서트인지 해제인지 판단하고 각각의 처리로 넘긴다.
 * PERST# 는 active-low 신호지만 gpiod 계층이 DT 서술을 반영해 주므로,
 * 드라이버는 1 을 그대로 "어서트"로 읽으면 된다.
 *
 * 마지막 줄이 이 핸들러의 요령이다. 이 GPIO 인터럽트는 레벨 트리거라 현재
 * 레벨로 설정해 두면 계속 재발생한다. 그래서 매번 "반대 레벨"을 기다리도록
 * 트리거 방향을 바꿔, 결과적으로 에지 트리거처럼 동작하게 만든다.
 *
 * 하드 핸들러가 아니라 스레드 핸들러인 이유는 아래 두 처리가
 * cancel_delayed_work_sync 같은 잠들 수 있는 호출을 하기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 스레드 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PERST# GPIO 레벨 변화 → 스레드 핸들러 == [이 함수]
 *     → perst_assert() 또는 perst_deassert() → irq_set_irq_type()
 */
static irqreturn_t rockchip_pcie_ep_perst_irq_thread(int irq, void *data)
{
	/* [한국어] 인터럽트 등록 시 넘긴 EPC 객체. */
	struct pci_epc *epc = data;
	/* [한국어] 거기서 이 드라이버의 EP 객체를 꺼낸다. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] PERST# GPIO 의 현재 값을 읽는다. 이 신호는 active-low 지만 gpiod 계층이
	 * DT 서술을 반영해 주므로, 1 이 곧 논리적 "어서트"다. */
	u32 perst = gpiod_get_value(rockchip->perst_gpio);

	/* [한국어] 어서트 상태면, */
	if (perst)
		/* [한국어] 링크를 내리고 훈련을 멈춘다. */
		rockchip_pcie_ep_perst_assert(ep);
	/* [한국어] 해제 상태면, */
	else
		/* [한국어] 훈련을 다시 시작한다. */
		rockchip_pcie_ep_perst_deassert(ep);

	/* [한국어] 트리거 방향을 반대로 바꾼다. 이 GPIO 인터럽트는 레벨 트리거라, 현재 레벨로
	 * 설정해 두면 계속 재발생한다. 그래서 매번 "반대 레벨"을 기다리도록 바꿔
	 * 에지 트리거처럼 동작하게 만드는 관용이다. */
	irq_set_irq_type(ep->perst_irq,
			 (perst ? IRQF_TRIGGER_HIGH : IRQF_TRIGGER_LOW));

	/* [한국어] 처리 완료. */
	return IRQ_HANDLED;
}

/* [한국어]
 * rockchip_pcie_ep_setup_irq - PERST# GPIO 를 인터럽트로 등록한다
 *
 * @epc: EPC 객체. 핸들러의 인자로 넘겨진다.
 * @return: 0 = 성공(GPIO 가 없는 경우 포함), 음수 = IRQ 획득 또는 등록 실패.
 *
 * PERST# GPIO 가 없는 보드에서는 감시할 것이 없으므로 그대로 성공을 돌려준다.
 * 그 경우 start() 가 훈련 워크를 직접 예약해 링크를 올린다.
 *
 * 위 영어 주석이 설명하는 초기 상태 처리가 이 함수의 핵심이다. PERST# 는
 * active-low 라 평상시(비활성)에는 전기적으로 high 이고, 그 상태가 곧
 * IRQF_TRIGGER_HIGH 조건에 걸린다. 그래서 등록 직후 한 번은 반드시 핸들러가
 * 불린다. 그 첫 호출을 무해하게 만들기 위해, 마치 호스트가 이미 PERST# 를
 * 걸어 둔 것처럼 perst_asserted 를 미리 true 로 꾸며 둔다. 그러면 첫 호출이
 * perst_assert() 로 가서 "이미 어서트 상태"라며 조용히 돌아간다.
 *
 * IRQ_NOAUTOEN 을 세워 등록 시점에 자동 활성화되지 않게 하는 것도 중요하다.
 * start() 가 명시적으로 enable_irq 할 때까지는 인터럽트를 받지 않는다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 지점 모두 곧장 return 한다. devm 등록이라 정리할 것이 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_probe() → [이 함수]
 *     → gpiod_to_irq() → irq_set_status_flags() → devm_request_threaded_irq()
 */
static int rockchip_pcie_ep_setup_irq(struct pci_epc *epc)
{
	/* [한국어] EP 객체. */
	struct rockchip_pcie_ep *ep = epc_get_drvdata(epc);
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 등록 결과. */
	int ret;

	/* [한국어] PERST# GPIO 가 없는 보드면 감시할 것이 없다. */
	if (!rockchip->perst_gpio)
		/* [한국어] 성공으로 돌아간다 — 그 경우 start 가 훈련 워크를 직접 예약한다. */
		return 0;

	/* PCIe reset interrupt */
	/* [한국어] GPIO 를 인터럽트 번호로 바꾼다(옆의 상류 주석). */
	ep->perst_irq = gpiod_to_irq(rockchip->perst_gpio);
	/* [한국어] 실패 검사. */
	if (ep->perst_irq < 0) {
		/* [한국어] 실패 로그. */
		dev_err(dev,
			"failed to get IRQ for PERST# GPIO: %d\n",
			ep->perst_irq);

		/* [한국어] 오류 전달. */
		return ep->perst_irq;
	}

	/*
	 * The perst_gpio is active low, so when it is inactive on start, it
	 * is high and will trigger the perst_irq handler. So treat this initial
	 * IRQ as a dummy one by faking the host asserting PERST#.
	 */
	/* [한국어] 위 영어 주석이 설명하는 처리 — PERST# 는 active-low 라 평상시(비활성)에는
	 * 전기적으로 high 이고, 그 상태가 곧 IRQF_TRIGGER_HIGH 조건에 걸린다.
	 * 그래서 등록 직후 한 번은 반드시 핸들러가 불린다. 그 첫 호출을 무해하게
	 * 만들기 위해, 마치 호스트가 이미 PERST# 를 걸어 둔 것처럼 상태를 꾸며 둔다. */
	ep->perst_asserted = true;
	/* [한국어] 자동 활성화를 막는다 — start() 가 명시적으로 enable_irq 할 때까지
	 * 인터럽트를 받지 않겠다는 뜻이다. */
	irq_set_status_flags(ep->perst_irq, IRQ_NOAUTOEN);
	/* [한국어] 스레드 핸들러로 등록한다. 하드 핸들러가 NULL 인 이유는 이 처리에
	 * cancel_delayed_work_sync 같은 잠들 수 있는 호출이 들어가기 때문이다. */
	ret = devm_request_threaded_irq(dev, ep->perst_irq, NULL,
					rockchip_pcie_ep_perst_irq_thread,
					/* [한국어] HIGH 레벨 트리거로 시작하고, ONESHOT 으로 스레드가 끝날 때까지 재진입을 막는다. */
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"pcie-ep-perst", epc);
	/* [한국어] 등록 실패. */
	if (ret) {
		/* [한국어] 실패 로그. */
		dev_err(dev,
			"failed to request IRQ for PERST# GPIO: %d\n",
			ret);

		/* [한국어] 오류 전달. */
		return ret;
	}

	/* [한국어] 인터럽트 준비 완료. */
	return 0;
}

static const struct pci_epc_features rockchip_pcie_epc_features = {
	/* [한국어] 링크 업 통지를 지원한다고 알린다 — 이 파일이 pci_epc_linkup/linkdown 을
	 * 실제로 부르기 때문이다. EPF 는 이 플래그를 보고 통지를 기다릴지 결정한다. */
	.linkup_notifier = true,
	/* [한국어] MSI 지원. */
	.msi_capable = true,
	/* [한국어] INTx 지원. 두 인터럽트 방식만 있고 msix_capable 이 없는 것이
	 * hide_broken_msix_cap() 이 존재하는 이유와 짝을 이룬다. */
	.intx_capable = true,
	/* [한국어] 주소 정렬 요구. EPF 가 창을 나눌 때 이 값을 기준으로 삼는다. */
	.align = ROCKCHIP_PCIE_AT_SIZE_ALIGN,
};

/* [한국어]
 * rockchip_pcie_ep_get_features - 이 EP 컨트롤러의 능력을 알려 준다
 *
 * @epc: EPC 객체. 쓰지 않는다.
 * @func_no: 물리 함수 번호. 쓰지 않는다.
 * @vfunc_no: 가상 함수 번호. 쓰지 않는다.
 * @return: 정적 상수 rockchip_pcie_epc_features 의 주소.
 *
 * 능력이 인스턴스나 함수와 무관하게 고정이므로 같은 구조체를 그대로 돌려준다.
 *
 * 선언하는 네 가지가 각각 이 파일의 다른 부분과 짝을 이룬다.
 *   - linkup_notifier: 이 파일이 실제로 pci_epc_linkup/linkdown 을 부르기 때문이다.
 *     EPF 는 이 플래그를 보고 통지를 기다릴지 결정한다.
 *   - msi_capable / intx_capable: raise_irq 가 그 둘만 처리하는 것과 일치한다.
 *   - msix_capable 이 없는 것이 hide_broken_msix_cap() 의 존재 이유와 짝이다 —
 *     하드웨어가 지원하지 않으면서 capability 만 광고하는 문제를 감춰야 했다.
 *   - align: EPF 가 창을 나눌 때 기준으로 삼는 정렬 단위다.
 *
 * 실행 컨텍스트: EPC 코어의 능력 조회 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   EPF → pci_epc_get_features() (pci-epc-core.c:381)
 *     → pci_epc_ops.get_features == [이 함수] */
static const struct pci_epc_features*
rockchip_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	/* [한국어] 인스턴스와 무관한 정적 능력이므로 같은 구조체를 그대로 돌려준다. */
	return &rockchip_pcie_epc_features;
}

static const struct pci_epc_ops rockchip_pcie_epc_ops = {
	/* [한국어] config 헤더 설정 — EPC 코어의 pci_epc_write_header()(:1130)가 부른다. */
	.write_header	= rockchip_pcie_ep_write_header,
	/* [한국어] BAR 설정 — pci_epc_set_bar()(:1054)가 부른다. */
	.set_bar	= rockchip_pcie_ep_set_bar,
	/* [한국어] BAR 해제 — pci_epc_clear_bar()(:967)가 부른다. */
	.clear_bar	= rockchip_pcie_ep_clear_bar,
	/* [한국어] 주소 정렬 계산 — pci_epc_mem_map()(:858)이 부른다. */
	.align_addr	= rockchip_pcie_ep_align_addr,
	/* [한국어] 아웃바운드 창 매핑 — pci_epc_map_addr()(:766)이 부른다. */
	.map_addr	= rockchip_pcie_ep_map_addr,
	/* [한국어] 아웃바운드 창 해제 — pci_epc_unmap_addr()(:741)이 부른다. */
	.unmap_addr	= rockchip_pcie_ep_unmap_addr,
	/* [한국어] MSI 벡터 수 설정 — pci_epc_set_msi()(:620)가 부른다. */
	.set_msi	= rockchip_pcie_ep_set_msi,
	/* [한국어] MSI 벡터 수 조회 — pci_epc_get_msi()(:578)가 부른다. */
	.get_msi	= rockchip_pcie_ep_get_msi,
	/* [한국어] 인터럽트 발생 — pci_epc_raise_irq()(:484)가 부른다. */
	.raise_irq	= rockchip_pcie_ep_raise_irq,
	/* [한국어] EP 동작 시작 — pci_epc_start()(:434)가 부른다. */
	.start		= rockchip_pcie_ep_start,
	/* [한국어] EP 동작 정지 — pci_epc_stop()(:410)이 부른다. */
	.stop		= rockchip_pcie_ep_stop,
	/* [한국어] 능력 조회 — pci_epc_get_features()(:381)가 부른다.
	 * 이 열두 콜백이 EPC 프레임워크와 이 드라이버의 전체 접점이다. */
	.get_features	= rockchip_pcie_ep_get_features,
};

/* [한국어]
 * rockchip_pcie_ep_get_resources - DT 에서 공용 자원과 EP 전용 설정을 읽는다
 *
 * @rockchip: 공용 컨트롤러 상태.
 * @ep: EP 객체.
 * @return: 0 = 성공, 음수 = 공용 파싱 또는 PHY 획득 실패.
 *
 * 공용 rockchip_pcie_parse_dt() 로 레지스터 창·클럭·리셋·PERST# GPIO 를 얻은 뒤,
 * EP 에만 필요한 두 가지를 더 읽는다.
 *
 * host 판과 대비되는 점이 둘이다.
 *   1) host 는 여기에 레귤레이터 넷을 더하지만 EP 는 더하지 않는다 —
 *      엔드포인트는 호스트로부터 전원을 받는 쪽이라 슬롯 전원을 제어할 일이 없다.
 *   2) EP 는 rockchip_pcie_get_phys() 를 직접 한 번 더 부른다. host 판은
 *      공용 init_port 안에서 대신 불리므로 이 호출이 없다.
 *
 * DT 오류를 실패로 만들지 않는 태도도 특징이다. 아웃바운드 창 개수와 함수
 * 개수는 속성이 없거나 한계를 넘으면 안전한 기본값으로 대체한다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 지점 모두 곧장 return 한다. devm 자원이라 정리할 것이 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_probe() → [이 함수]
 *     → rockchip_pcie_parse_dt() / rockchip_pcie_get_phys() (공용 pcie-rockchip.c)
 *     → of_property_read_u32() / of_property_read_u8()
 */
static int rockchip_pcie_ep_get_resources(struct rockchip_pcie *rockchip,
					  struct rockchip_pcie_ep *ep)
{
	/* [한국어] 로그 대상. */
	struct device *dev = rockchip->dev;
	/* [한국어] 각 단계 결과. */
	int err;

	/* [한국어] 공용 DT 파싱 — 레지스터 창, 클럭, 리셋, PERST# GPIO 를 얻는다.
	 * host 판이 여기에 레귤레이터 넷을 더하는 것과 달리 EP 는 더하지 않는다. */
	err = rockchip_pcie_parse_dt(rockchip);
	/* [한국어] 실패 전파. */
	if (err)
		return err;

	/* [한국어] PHY 를 가져온다. host 판은 이 호출을 하지 않는데, 공용 init_port 안에서
	 * 대신 불리기 때문이다 — EP 는 여기서 한 번 더 부른다. */
	err = rockchip_pcie_get_phys(rockchip);
	/* [한국어] 실패 전파. */
	if (err)
		return err;

	/* [한국어] 아웃바운드 창 개수를 DT 에서 읽는다. */
	err = of_property_read_u32(dev->of_node,
				   "rockchip,max-outbound-regions",
				   &ep->max_regions);
	/* [한국어] 속성이 없거나 하드웨어 한계를 넘으면, */
	if (err < 0 || ep->max_regions > MAX_REGION_LIMIT)
		/* [한국어] 한계값으로 고정한다. DT 오류를 실패로 만들지 않고 안전한 값으로 대체하는 방식이다. */
		ep->max_regions = MAX_REGION_LIMIT;

	/* [한국어] 창 비트맵을 비운 상태로 시작한다. */
	ep->ob_region_map = 0;

	/* [한국어] 노출할 PCI 함수 개수를 DT 에서 읽는다. */
	err = of_property_read_u8(dev->of_node, "max-functions",
				  &ep->epc->max_functions);
	/* [한국어] 속성이 없으면, */
	if (err < 0)
		/* [한국어] 1개(함수 0만)로 둔다. */
		ep->epc->max_functions = 1;

	/* [한국어] 자원 확보 완료. */
	return 0;
}

static const struct of_device_id rockchip_pcie_ep_of_match[] = {
	/* [한국어] RK3399 의 PCIe 엔드포인트 모드. host 판의 compatible 과 -ep 접미사로 구분된다. */
	{ .compatible = "rockchip,rk3399-pcie-ep"},
	/* [한국어] 테이블 끝. [상류 코드 관찰] host 판과 달리 MODULE_DEVICE_TABLE 이 없는데,
	 * 이 드라이버가 builtin 이라 모듈 자동 로딩 정보가 필요 없기 때문이다. */
	{},
};

/* [한국어]
 * rockchip_pcie_ep_init_ob_mem - 아웃바운드 창 메모리 관리자를 세우고 MSI 전용 창을 확보한다
 *
 * @ep: EP 객체.
 * @return: 0 = 성공, -ENOMEM = 할당 또는 창 확보 실패, 그 밖의 음수 = 관리자 초기화 실패.
 *
 * EPF 가 호스트 메모리에 접근하려면 AXI 주소 공간의 어느 구역을 빌려야 한다.
 * 그 대여를 관리하는 것이 EPC 메모리 관리자이고, 이 함수가 그것을 초기화한다.
 *
 * 동작 과정:
 *   1) 창마다의 AXI 주소를 기록할 배열(ob_addr)을 할당한다.
 *   2) 창 서술 배열을 임시로 만들어 채운다 — 창마다 AXI 자원 시작에서 1MB 씩
 *      떨어진 주소이고, 크기와 페이지 크기가 모두 1MB 다. 페이지 크기가 1MB 인
 *      것은 이 컨트롤러의 창이 1MB 단위로만 움직이기 때문이다.
 *   3) pci_epc_multi_mem_init() 으로 관리자를 초기화하고, 임시 배열은 곧바로
 *      해제한다 — 관리자가 내용을 복사해 갔기 때문이다.
 *   4) MSI/INTx 전용 창 하나를 미리 확보한다. 이 창이 raise_irq 경로의 통로다.
 *      따로 확보해 두지 않으면 MSI 를 보낼 때마다 EPF 의 창을 밀어내야 한다.
 *   5) 전용 창의 현재 매핑 주소를 더미 값으로 초기화한다. 실제 주소와 겹치지
 *      않아야 첫 MSI 전송에서 반드시 재매핑이 일어난다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 에러 경로: 두 할당 실패는 곧장 return 하고(devm 이라 정리 불필요),
 * 전용 창 확보 실패만 err_epc_mem_exit 라벨을 거쳐 관리자를 되돌린다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_probe() → [이 함수]
 *     → devm_kcalloc() ×2 → pci_epc_multi_mem_init()
 *     → pci_epc_mem_alloc_addr()
 */
static int rockchip_pcie_ep_init_ob_mem(struct rockchip_pcie_ep *ep)
{
	/* [한국어] 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip = &ep->rockchip;
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = rockchip->dev;
	/* [한국어] EPC 메모리 관리자에 넘길 창 서술 배열. */
	struct pci_epc_mem_window *windows = NULL;
	/* [한국어] err: 결과. i: 순회 인덱스. */
	int err, i;

	/* [한국어] 창마다의 AXI 주소를 담을 배열을 할당한다. */
	ep->ob_addr = devm_kcalloc(dev, ep->max_regions, sizeof(*ep->ob_addr),
				   GFP_KERNEL);

	/* [한국어] 메모리 부족. */
	if (!ep->ob_addr)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 창 서술 배열을 임시로 할당한다. */
	windows = devm_kcalloc(dev, ep->max_regions,
			       sizeof(struct pci_epc_mem_window), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!windows)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 창마다, */
	for (i = 0; i < ep->max_regions; i++) {
		/* [한국어] AXI 자원 시작에서 1MB 씩 떨어진 주소를 기준으로 삼는다. */
		windows[i].phys_base = rockchip->mem_res->start + (SZ_1M * i);
		/* [한국어] 창 크기는 1MB. */
		windows[i].size = SZ_1M;
		/* [한국어] 페이지 크기도 1MB — 이 컨트롤러의 창이 1MB 단위로만 움직이기 때문이다. */
		windows[i].page_size = SZ_1M;
	}
	/* [한국어] EPC 메모리 관리자를 초기화한다. 이제 EPF 가 pci_epc_mem_alloc_addr 로
	 * 창을 빌려 갈 수 있다. */
	err = pci_epc_multi_mem_init(ep->epc, windows, ep->max_regions);
	/* [한국어] 임시 배열은 곧바로 해제한다 — 관리자가 내용을 복사해 갔기 때문이다. */
	devm_kfree(dev, windows);

	/* [한국어] 초기화 실패. */
	if (err < 0) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to initialize the memory space\n");
		/* [한국어] 오류 전달. */
		return err;
	}

	/* [한국어] MSI/INTx 전용 창 하나를 미리 확보한다. 이 창이 raise_irq 경로의 통로가 된다. */
	ep->irq_cpu_addr = pci_epc_mem_alloc_addr(ep->epc, &ep->irq_phys_addr,
						  SZ_1M);
	/* [한국어] 확보 실패. */
	if (!ep->irq_cpu_addr) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to reserve memory space for MSI\n");
		/* [한국어] -ENOMEM 기록. */
		err = -ENOMEM;
		/* [한국어] 메모리 관리자를 되돌리는 구간으로. */
		goto err_epc_mem_exit;
	}

	/* [한국어] 전용 창의 현재 매핑 주소를 더미 값으로 초기화한다. 실제 주소와 겹치지 않는
	 * 값이어야 첫 MSI 전송에서 반드시 재매핑이 일어난다. */
	ep->irq_pci_addr = ROCKCHIP_PCIE_EP_DUMMY_IRQ_ADDR;

	/* [한국어] 초기화 완료. */
	return 0;

/* [한국어] 전용 창 확보 실패 전용 라벨. */
err_epc_mem_exit:
	/* [한국어] 메모리 관리자를 되돌린다. */
	pci_epc_mem_exit(ep->epc);

	/* [한국어] 기록해 둔 오류를 전달한다. */
	return err;
}

/* [한국어]
 * rockchip_pcie_ep_exit_ob_mem - 아웃바운드 창 메모리 관리자를 해제한다
 *
 * @ep: EP 객체.
 *
 * init_ob_mem 의 짝이다. pci_epc_mem_exit() 한 줄이 전부이며, 미리 확보해 둔
 * MSI 전용 창도 관리자 안에 포함되어 함께 정리된다 — 그래서 별도의
 * pci_epc_mem_free_addr() 호출이 없다.
 *
 * ob_addr 배열은 devm 이라 드라이버 코어가 회수한다.
 *
 * 실행 컨텍스트: probe 실패 경로, 프로세스 컨텍스트.
 * [상류 코드 관찰] 이 드라이버에는 remove 콜백이 없으므로, 정상 종료 경로에서는
 * 이 함수가 불리지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_probe()(err_exit_ob_mem 라벨) → [이 함수] → pci_epc_mem_exit()
 */
static void rockchip_pcie_ep_exit_ob_mem(struct rockchip_pcie_ep *ep)
{
	/* [한국어] 메모리 관리자를 해제한다. 전용 창은 그 안에 포함되어 함께 정리된다. */
	pci_epc_mem_exit(ep->epc);
}

/* [한국어]
 * rockchip_pcie_ep_hide_broken_msix_cap - 쓸 수 없는 MSI-X capability 를 목록에서 감춘다
 *
 * @rockchip: 공용 컨트롤러 상태.
 *
 * 위 영어 주석이 배경을 상세히 설명한다 — 이 컨트롤러는 MSI-X 를 지원하지
 * 않는데도 기본값으로 MSI-X capability 를 광고한다. 그대로 두면 호스트가
 * 쓸 수 없는 MSI-X 벡터를 배정하고, 그 인터럽트는 영영 오지 않는다.
 *
 * 해결책은 capability 연결 리스트를 조작하는 것이다. PCI capability 는
 * 각 항목이 다음 항목의 오프셋을 담고 있는 단방향 리스트라, 어떤 항목의
 * next 포인터를 그 다음 것으로 바꾸면 중간 항목이 목록에서 빠진다.
 * 여기서는 MSI-X 의 next 를 읽어 MSI 의 next 에 넣어, MSI → (MSI-X 건너뜀)
 * → 그다음 항목 으로 이어 붙인다. 그 결과 호스트에게는 MSI-X 가 존재하지
 * 않는 것처럼 보인다.
 *
 * host 판이 THP capability 의 next 를 지워 L1 서브스테이트를 감추는 것과
 * 같은 수법이며, 이 IP 계열이 광고하는 능력과 실제 능력이 어긋나는 곳이
 * 여러 군데 있음을 보여 준다.
 *
 * 실행 컨텍스트: probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   rockchip_pcie_ep_probe() → [이 함수]
 *     → rockchip_pcie_read() ×2 → rockchip_pcie_write()
 */
static void rockchip_pcie_ep_hide_broken_msix_cap(struct rockchip_pcie *rockchip)
{
	/* [한국어] cfg_msi: MSI capability 워드. cfg_msix_cp: MSI-X 의 next 포인터. */
	u32 cfg_msi, cfg_msix_cp;

	/*
	 * MSI-X is not supported but the controller still advertises the MSI-X
	 * capability by default, which can lead to the Root Complex side
	 * allocating MSI-X vectors which cannot be used. Avoid this by skipping
	 * the MSI-X capability entry in the PCIe capabilities linked-list: get
	 * the next pointer from the MSI-X entry and set that in the MSI
	 * capability entry (which is the previous entry). This way the MSI-X
	 * entry is skipped (left out of the linked-list) and not advertised.
	 */
	/* [한국어] 위 영어 주석이 배경을 설명한다 — 이 컨트롤러는 MSI-X 를 지원하지 않는데도
	 * 기본값으로 MSI-X capability 를 광고해, 호스트가 쓸 수 없는 벡터를 배정하게 만든다.
	 * 해결책은 capability 연결 리스트에서 MSI-X 항목을 건너뛰는 것이다.
	 * 먼저 MSI capability 워드를 읽는다. */
	cfg_msi = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_BASE +
				     ROCKCHIP_PCIE_EP_MSI_CTRL_REG);

	/* [한국어] MSI 항목의 next 포인터 필드를 지운다. */
	cfg_msi &= ~ROCKCHIP_PCIE_EP_MSI_CP1_MASK;

	/* [한국어] MSI-X 항목의 next 포인터를 읽어 낸다. */
	cfg_msix_cp = rockchip_pcie_read(rockchip, PCIE_EP_CONFIG_BASE +
					 ROCKCHIP_PCIE_EP_MSIX_CAP_REG) &
					 ROCKCHIP_PCIE_EP_MSIX_CAP_CP_MASK;

	/* [한국어] 그 값을 MSI 항목의 next 로 넣는다. 결과적으로 리스트가
	 * MSI → (MSI-X 건너뜀) → 그다음 항목 으로 이어져, 호스트에게 MSI-X 가
	 * 존재하지 않는 것처럼 보인다. host 판이 THP capability 의 next 를 지워
	 * L1 서브스테이트를 감추는 것과 같은 수법이다. */
	cfg_msi |= cfg_msix_cp;

	/* [한국어] 완성한 값을 되쓴다. */
	rockchip_pcie_write(rockchip, cfg_msi,
			    PCIE_EP_CONFIG_BASE + ROCKCHIP_PCIE_EP_MSI_CTRL_REG);
}

/* [한국어]
 * rockchip_pcie_ep_probe - RK3399 PCIe 컨트롤러를 엔드포인트로 올린다
 *
 * @pdev: DT 에서 "rockchip,rk3399-pcie-ep" 로 매칭된 플랫폼 디바이스.
 * @return: 0 = 성공. -ENOMEM = 할당 실패. 그 밖의 음수 = 각 초기화 단계의 실패.
 *
 * host 판과 같은 IP 를 정반대 역할로 올린다. 갈라지는 지점은 is_rc = false
 * 하나이며, 공용 코드가 그 값으로 자원 이름·PERST# GPIO 이름·모드 설정을 바꾼다.
 *
 * 동작 과정:
 *   1) EP 객체를 할당하고 is_rc 를 false 로, dev 를 기록하고, 링크 훈련 워크를
 *      초기화한다. 워크 초기화를 probe 초반에 해 두어야 아래 어느 실패 경로에서
 *      취소를 부르더라도 안전하다.
 *   2) EPC 객체를 만들고 epc_set_drvdata() 로 이 객체를 심는다. 모든 콜백이
 *      epc_get_drvdata() 로 되찾으므로 이 대입이 어떤 콜백보다도 앞서야 한다.
 *   3) DT 파싱과 PHY 획득, 창 개수·함수 개수 결정.
 *   4) 아웃바운드 창 메모리 관리자와 MSI 전용 창 준비.
 *   5) 클럭을 켜고 공용 init_port() 로 리셋·PHY·MODE_EP 설정을 수행한다.
 *   6) 쓸 수 없는 MSI-X capability 를 감춘다.
 *   7) 우선 함수 0 만 활성화한다. 나머지는 start() 가 바인딩된 EPF 를 보고
 *      다시 계산한다.
 *   8) pci_epc_init_notify() 로 EPF 드라이버들의 바인딩을 유발한다.
 *   9) PERST# 감시 인터럽트를 등록한다. EPF 바인딩보다 뒤이지만
 *      IRQ_NOAUTOEN 으로 등록해 start() 전까지는 받지 않으므로 문제가 없다.
 *
 * 실행 컨텍스트: 드라이버 코어의 probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 세 개의 라벨(err_uninit_port → err_disable_clocks →
 * err_exit_ob_mem)이 계단식으로 이어진다.
 * [상류 코드 관찰] remove 콜백이 없다. builtin 이라 모듈 언로드는 없지만
 * sysfs 언바인드는 여전히 가능하며, 그 경우 EPC 등록과 창 매핑이 남는다.
 * host 판이 remove 를 제대로 구현하는 것과 대비된다.
 *
 * 호출 체인:
 *   DT 매칭 → platform_driver.probe == [이 함수]
 *     → devm_pci_epc_create() → get_resources() → init_ob_mem()
 *     → rockchip_pcie_enable_clocks() → rockchip_pcie_init_port()
 *     → hide_broken_msix_cap() → pci_epc_init_notify() → setup_irq()
 */
static int rockchip_pcie_ep_probe(struct platform_device *pdev)
{
	/* [한국어] 로그와 devm 의 기준 디바이스. */
	struct device *dev = &pdev->dev;
	/* [한국어] 이 드라이버의 EP 객체. */
	struct rockchip_pcie_ep *ep;
	/* [한국어] 그 안의 공용 컨트롤러 상태. */
	struct rockchip_pcie *rockchip;
	/* [한국어] EPC 프레임워크 객체. */
	struct pci_epc *epc;
	/* [한국어] 각 단계 결과. */
	int err;

	/* [한국어] EP 객체를 0 초기화 할당한다. */
	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	/* [한국어] 메모리 부족. */
	if (!ep)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 구조체 맨 앞의 공용 상태를 가리킨다. */
	rockchip = &ep->rockchip;
	/* [한국어] 엔드포인트 모드임을 명시한다. host 판이 true 를 넣는 것과 대비되며,
	 * kzalloc 덕분에 사실 이 대입이 없어도 false 이지만 의도를 분명히 하려고 적었다. */
	rockchip->is_rc = false;
	/* [한국어] 로그와 devm 의 기준 디바이스를 기록한다. */
	rockchip->dev = dev;
	/* [한국어] 링크 훈련 워크를 초기화한다. probe 초반에 해 두어야 아래 어느 경로에서
	 * 취소를 부르더라도 안전하다. */
	INIT_DELAYED_WORK(&ep->link_training, rockchip_pcie_ep_link_training);

	/* [한국어] EPC 객체를 만든다. 이 호출로 콜백 테이블이 프레임워크에 등록된다. */
	epc = devm_pci_epc_create(dev, &rockchip_pcie_epc_ops);
	/* [한국어] 생성 실패. */
	if (IS_ERR(epc)) {
		/* [한국어] 실패 로그. */
		dev_err(dev, "failed to create EPC device\n");
		/* [한국어] 오류 전달. */
		return PTR_ERR(epc);
	}

	/* [한국어] 만든 EPC 를 기록한다. */
	ep->epc = epc;
	/* [한국어] 모든 콜백이 epc_get_drvdata() 로 되찾을 수 있도록 이 객체를 심어 둔다.
	 * 이 대입이 아래 어떤 콜백보다도 앞서야 한다. */
	epc_set_drvdata(epc, ep);

	/* [한국어] DT 파싱과 PHY 획득, 창 개수·함수 개수 결정. */
	err = rockchip_pcie_ep_get_resources(rockchip, ep);
	/* [한국어] 실패 전파. */
	if (err)
		return err;

	/* [한국어] 아웃바운드 창 메모리 관리자와 MSI 전용 창을 준비한다. */
	err = rockchip_pcie_ep_init_ob_mem(ep);
	/* [한국어] 실패 전파. */
	if (err)
		return err;

	/* [한국어] 클럭을 켠다. */
	err = rockchip_pcie_enable_clocks(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 창 메모리를 되돌리는 구간으로. */
		goto err_exit_ob_mem;

	/* [한국어] 공용 포트 초기화 — 리셋과 PHY, MODE_EP 설정이 거기서 일어난다. */
	err = rockchip_pcie_init_port(rockchip);
	/* [한국어] 실패하면, */
	if (err)
		/* [한국어] 클럭까지 되돌리는 구간으로. */
		goto err_disable_clocks;

	/* [한국어] 쓸 수 없는 MSI-X capability 를 목록에서 감춘다. */
	rockchip_pcie_ep_hide_broken_msix_cap(rockchip);

	/* Only enable function 0 by default */
	/* [한국어] 우선 함수 0 만 활성화한다(옆의 상류 주석). 나머지는 start() 가
	 * 바인딩된 EPF 를 보고 다시 계산한다. */
	rockchip_pcie_write(rockchip, BIT(0), PCIE_CORE_PHY_FUNC_CFG);

	/* [한국어] EPC 코어에 초기화가 끝났음을 알린다. 이 통지를 받은 EPF 드라이버들이
	 * 바인딩을 시작한다. */
	pci_epc_init_notify(epc);

	/* [한국어] PERST# 감시 인터럽트를 등록한다. EPF 바인딩보다 뒤에 오는 순서인데,
	 * IRQ_NOAUTOEN 으로 등록해 start() 전까지는 실제로 받지 않으므로 문제가 없다. */
	err = rockchip_pcie_ep_setup_irq(epc);
	/* [한국어] 실패하면, */
	if (err < 0)
		/* [한국어] PHY 부터 되돌리는 구간으로. */
		goto err_uninit_port;

	/* [한국어] probe 성공. */
	return 0;
/* [한국어] setup_irq 실패 전용 라벨. */
err_uninit_port:
	/* [한국어] PHY 를 내린다. */
	rockchip_pcie_deinit_phys(rockchip);
/* [한국어] init_port 실패가 도달하는 라벨. */
err_disable_clocks:
	/* [한국어] 클럭을 끈다. */
	rockchip_pcie_disable_clocks(rockchip);
/* [한국어] 클럭 활성화 실패가 도달하는 라벨. */
err_exit_ob_mem:
	/* [한국어] 창 메모리 관리자를 해제한다. */
	rockchip_pcie_ep_exit_ob_mem(ep);
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return err;
}

static struct platform_driver rockchip_pcie_ep_driver = {
	.driver = {
		/* [한국어] 드라이버 이름 — host 판과 구분된다. */
		.name = "rockchip-pcie-ep",
		/* [한국어] 위에서 정의한 DT 매칭 테이블. */
		.of_match_table = rockchip_pcie_ep_of_match,
	},
	/* [한국어] 장치가 나타났을 때 불릴 진입점.
	 * [상류 코드 관찰] remove 콜백이 없다. builtin 이라 모듈 언로드는 없지만,
	 * sysfs 언바인드는 여전히 가능하며 그 경우 EPC 등록과 창 매핑이 남는다. */
	.probe = rockchip_pcie_ep_probe,
};

/* [한국어] module_platform_driver 가 아니라 builtin_ 판이다. 커널에 내장되며
 * MODULE_LICENSE 등 모듈 메타데이터도 이 파일에는 없다 — host 판이
 * 모듈로 빌드될 수 있는 것과 대비된다. */
builtin_platform_driver(rockchip_pcie_ep_driver);
