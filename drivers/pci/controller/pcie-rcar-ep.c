// SPDX-License-Identifier: GPL-2.0
/* PCI/NVMe: GPL v2; NVMe 호스트 드라이버와 동일한 라이선스 정책으로 커널에 내장 */
/*
 * PCIe endpoint driver for Renesas R-Car SoCs
 *  Copyright (c) 2020 Renesas Electronics Europe GmbH
 *
 * Author: Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>
 */

#include <linux/delay.h>	/* PCI/NVMe: 링크 안정화/INTx pulse용 udelay/usleep_range */
#include <linux/of_address.h>	/* PCI/NVMe: DT 메모리 영역 → 물리 주소 변환, BAR 백킹 메모리 파악 */
#include <linux/of_platform.h>	/* PCI/NVMe: platform_driver DT 바인딩, NVMe 열거 전 컨트롤러 등록 */
#include <linux/pci.h>		/* PCI/NVMe: PCIe Capability/INTx/MSI 레지스터, NVMe 장치 식별 */
#include <linux/pci-epc.h>	/* PCI/NVMe: PCI Endpoint Controller 프레임워크, 호스트가 보는 EP 능력 공개 */
#include <linux/platform_device.h>	/* PCI/NVMe: SoC PCIe MAC을 platform device로 노출 */
#include <linux/pm_runtime.h>	/* PCI/NVMe: ASPM/런타임 PM, NVMe ns 전력 상태와 연결 */

#include "pcie-rcar.h"		/* PCI/NVMe: R-Car PCIe MAC 레지스터 오프셋/매크로 정의 */

#define RCAR_EPC_MAX_FUNCTIONS		1
/* PCI/NVMe: EP 컨트롤러가 노출하는 function 수; NVMe SSD 하나의 PCIe function만 EP 모드로 노출 */

/* Structure representing the PCIe interface */
/* PCI/NVMe: R-Car SoC의 PCIe Endpoint Controller 소프트웨어 상태 */
struct rcar_pcie_endpoint {
	struct rcar_pcie	pcie;		/* PCI/NVMe: 공용 R-Car PCIe MAC 상태(레지스터 베이스 등) */
	phys_addr_t		*ob_mapped_addr;	/* PCI/NVMe: 각 outbound ATU 윈도우에 매핑된 CPU측 물리주소; NVMe DMA/메모리 맵핑 추적 */
	struct pci_epc_mem_window *ob_window;	/* PCI/NVMe: 호스트가 접근 가능한 outbound 메모리 윈도우 배열 */
	u8			max_functions;	/* PCI/NVMe: 호스트가 열거할 function 개수; NVMe 보통 1 */
	unsigned int		bar_to_atu[MAX_NR_INBOUND_MAPS];
				/* PCI/NVMe: 각 BAR가 사용하는 inbound ATU 인덱스; NVMe BAR0/2/4 매핑 상태 */
	unsigned long		*ib_window_map;	/* PCI/NVMe: inbound ATU 윈도우 사용 비트맵; BAR 할당/해제 관리 */
	u32			num_ib_windows;	/* PCI/NVMe: 사용 가능한 inbound ATU 윈도우 수 */
	u32			num_ob_windows;	/* PCI/NVMe: 사용 가능한 outbound ATU 윈도우 수; NVMe DMA 범위 확보 */
};

/* PCI/NVMe: PCIe MAC을 Endpoint 모드로 초기화; 호스트가 볼 Capability 구성 */
static void rcar_pcie_ep_hw_init(struct rcar_pcie *pcie)
{
	u32 val;

	/* PCI/NVMe: PCIe 트랜잭션 레이어 일시 정지; 이후 capability 재설정 */
	rcar_pci_write_reg(pcie, 0, PCIETCTLR);

	/* Set endpoint mode */
	/* PCI/NVMe: MAC을 Endpoint로 설정 → 호스트가 루트 컴플렉스 아래에서 이 디바이스를 열거 */
	rcar_pci_write_reg(pcie, 0, PCIEMSR);

	/* Initialize default capabilities. */
	/* PCI/NVMe: PCIe Express Capability 헤더; 호스트가 pci_find_capability()로 찾음 */
	rcar_rmw32(pcie, REXPCAP(0), 0xff, PCI_CAP_ID_EXP);
	/* PCI/NVMe: Device/Port Type을 Endpoint로 설정; 호스트 lspci -vv에서 Endpoint 표시 */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_FLAGS),
		   PCI_EXP_FLAGS_TYPE, PCI_EXP_TYPE_ENDPOINT << 4);
	/* PCI/NVMe: PCI 헤더 타입을 일반 Endpoint(0x00)로; NVMe BAR 레지스터 노출 허용 */
	rcar_rmw32(pcie, RCONF(PCI_HEADER_TYPE), PCI_HEADER_TYPE_MASK,
		   PCI_HEADER_TYPE_NORMAL);

	/* Write out the physical slot number = 0 */
	/* PCI/NVMe: Slot Capability의 Physical Slot Number; 핫플러그/ACPI _SUN에 영향 */
	rcar_rmw32(pcie, REXPCAP(PCI_EXP_SLTCAP), PCI_EXP_SLTCAP_PSN, 0);

	val = rcar_pci_read_reg(pcie, EXPCAP(1));
	/* device supports fixed 128 bytes MPSS */
	/* PCI/NVMe: Max Payload Size Supported=128B; NVMe 호스트가 MPS 협상 시 참고 */
	val &= ~GENMASK(2, 0);
	rcar_pci_write_reg(pcie, val, EXPCAP(1));

	val = rcar_pci_read_reg(pcie, EXPCAP(2));
	/* read requests size 128 bytes */
	/* PCI/NVMe: Max Read Request Size=128B; NVMe DMA 읽기 퍼포먼스/지연 영향 */
	val &= ~GENMASK(14, 12);
	/* payload size 128 bytes */
	/* PCI/NVMe: Initial/Current Max Payload Size=128B; TLP 크기 제한 */
	val &= ~GENMASK(7, 5);
	rcar_pci_write_reg(pcie, val, EXPCAP(2));

	/* Set target link speed to 5.0 GT/s */
	/* PCI/NVMe: Target Link Speed=5GT/s; NVMe Gen3 x1/x2 링크 협상 목표 */
	rcar_rmw32(pcie, EXPCAP(12), PCI_EXP_LNKSTA_CLS,
		   PCI_EXP_LNKSTA_CLS_5_0GB);

	/* Set the completion timer timeout to the maximum 50ms. */
	/* PCI/NVMe: Completion Timeout=50ms; NVMe admin/io 명령 완료 대기 한도 */
	rcar_rmw32(pcie, TLCTLR + 1, 0x3f, 50);

	/* Terminate list of capabilities (Next Capability Offset=0) */
	/* PCI/NVMe: 가상 채널 Capability 리스트 종료; 호스트의 capability 파싱 안전장치 */
	rcar_rmw32(pcie, RVCCAP(0), 0xfff00000, 0);

	/* PCI/NVMe: AER(Advanced Error Reporting) Capability는 본 EP MAC에서 별도 초기화하지 않음;
	 * NVMe 호스트 측 pcieportdrv가 AER 처리 및 nvme_reset_work와 연계
	 */

	/* flush modifications */
	/* PCI/NVMe: capability 수정을 메모리 순서대로 플러시; 호스트 열거 전 레지스터 확정 */
	wmb();
}

/* PCI/NVMe: CPU 물리주소가 속한 outbound ATU 윈도우 인덱스 반환; NVMe DMA 범위 일치 여부 확인 */
static int rcar_pcie_ep_get_window(struct rcar_pcie_endpoint *ep,
				   phys_addr_t addr)
{
	int i;

	/* PCI/NVMe: 등록된 outbound 윈도우를 순회; NVMe 호스트가 볼 pci_addr 대응 윈도우 탐색 */
	for (i = 0; i < ep->num_ob_windows; i++)
		/* PCI/NVMe: 윈도우 물리 기준 주소와 일치하면 해당 ATU 인덱스 반환 */
		if (ep->ob_window[i].phys_base == addr)
			return i;

	/* PCI/NVMe: 일치하는 윈도우 없으면 DMA/메모리 맵핑 실패 반환 */
	return -EINVAL;
}

/* PCI/NVMe: DT의 outbound memory 리소스를 ATU 윈도우로 파싱; NVMe 호스트가 접근할 시스템 메모리 범위 */
static int rcar_pcie_parse_outbound_ranges(struct rcar_pcie_endpoint *ep,
					   struct platform_device *pdev)
{
	struct rcar_pcie *pcie = &ep->pcie;
	char outbound_name[10];	/* PCI/NVMe: "memory0", "memory1" ... 버퍼 */
	struct resource *res;	/* PCI/NVMe: DT에서 읽은 메모리 리소스 */
	unsigned int i = 0;

	/* PCI/NVMe: outbound 윈도우 수 초기화 */
	ep->num_ob_windows = 0;
	/* PCI/NVMe: RCAR_PCI_MAX_RESOURCES만큼 outbound ATU 윈도우 등록 시도 */
	for (i = 0; i < RCAR_PCI_MAX_RESOURCES; i++) {
		/* PCI/NVMe: DT 리소스 이름 "memory%u" 구성 */
		sprintf(outbound_name, "memory%u", i);
		/* PCI/NVMe: DT에서 이름 기반 메모리 리소스 획득; NVMe DMA/매핑 물리 메모리 출처 */
		res = platform_get_resource_byname(pdev,
						   IORESOURCE_MEM,
						   outbound_name);
		if (!res) {
			/* PCI/NVMe: outbound 윈도우 누락 시 NVMe DMA 메모리 범위 부족 */
			dev_err(pcie->dev, "missing outbound window %u\n", i);
			return -EINVAL;
		}
		/* PCI/NVMe: 해당 물리 메모리 영역을 커널 리소스 트리에 등록/예약 */
		if (!devm_request_mem_region(&pdev->dev, res->start,
					     resource_size(res),
					     res->name)) {
			dev_err(pcie->dev, "Cannot request memory region %s.\n",
				outbound_name);
			return -EIO;
		}

		/* PCI/NVMe: ATU 윈도우의 CPU측 물리 기준 주소 기록 */
		ep->ob_window[i].phys_base = res->start;
		/* PCI/NVMe: ATU 윈도우 크기; NVMe 호스트가 볼 수 있는 메모리 영역 크기 */
		ep->ob_window[i].size = resource_size(res);
		/* controller doesn't support multiple allocation
		 * from same window, so set page_size to window size
		 */
		/* PCI/NVMe: 한 윈도우당 하나의 할당만 허용; page_size를 윈도우 크기로 고정 */
		ep->ob_window[i].page_size = resource_size(res);
	}
	/* PCI/NVMe: 실제 등록된 outbound 윈도우 수 저장 */
	ep->num_ob_windows = i;

	return 0;
}

/* PCI/NVMe: platform data 획득; PCIe 컨트롤러 레지스터 맵핑 및 윈도우 설정 */
static int rcar_pcie_ep_get_pdata(struct rcar_pcie_endpoint *ep,
				  struct platform_device *pdev)
{
	struct rcar_pcie *pcie = &ep->pcie;
	struct pci_epc_mem_window *window;
	struct device *dev = pcie->dev;
	struct resource res;
	int err;

	/* PCI/NVMe: DT reg 영역을 resource로 변환; PCIe MAC 레지스터 공간 */
	err = of_address_to_resource(dev->of_node, 0, &res);
	if (err)
		return err;
	/* PCI/NVMe: PCIe MAC 레지스터를 커널 가상 주소에 ioremap; NVMe 호스트 열거 전 HW 제어용 */
	pcie->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	/* PCI/NVMe: outbound 윈도우 구조체 배열 할당; 최대 리소스 수만큼 */
	ep->ob_window = devm_kcalloc(dev, RCAR_PCI_MAX_RESOURCES,
				     sizeof(*window), GFP_KERNEL);
	if (!ep->ob_window)
		return -ENOMEM;

	/* PCI/NVMe: outbound 메모리 윈도우 파싱; 실패 시 NVMe DMA 범위 없음 */
	rcar_pcie_parse_outbound_ranges(ep, pdev);

	/* PCI/NVMe: DT max-functions 속성 읽기; NVMe는 보통 단일 function */
	err = of_property_read_u8(dev->of_node, "max-functions",
				  &ep->max_functions);
	/* PCI/NVMe: 속성 없거나 최대 초과 시 1개 function으로 제한 */
	if (err < 0 || ep->max_functions > RCAR_EPC_MAX_FUNCTIONS)
		ep->max_functions = RCAR_EPC_MAX_FUNCTIONS;

	return 0;
}

/* PCI/NVMe: EP의 PCI Configuration Space 헤더 작성; 호스트 열거 시 VID/DID/Class 등 읽힘 */
static int rcar_pcie_ep_write_header(struct pci_epc *epc, u8 fn, u8 vfn,
				     struct pci_epf_header *hdr)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	u32 val;

	/* PCI/NVMe: function 0일 때만 vendor ID를 쓰고, 아니면 기존 값 보존 */
	if (!fn)
		val = hdr->vendorid;
	else
		val = rcar_pci_read_reg(pcie, IDSETR0);
	/* PCI/NVMe: device ID를 상위 16비트에 기록; NVMe 호스트가 pci_device_id 매칭에 사용 */
	val |= hdr->deviceid << 16;
	rcar_pci_write_reg(pcie, val, IDSETR0);

	/* PCI/NVMe: Rev ID/Prog IF/Subclass/Baseclass 조합; NVMe class=0x010802 판별 */
	val = hdr->revid;
	val |= hdr->progif_code << 8;
	val |= hdr->subclass_code << 16;
	val |= hdr->baseclass_code << 24;
	rcar_pci_write_reg(pcie, val, IDSETR1);

	/* PCI/NVMe: subsystem vendor/subsystem ID 설정; NVMe 모델 식별 추가 정보 */
	if (!fn)
		val = hdr->subsys_vendor_id;
	else
		val = rcar_pci_read_reg(pcie, SUBIDSETR);
	val |= hdr->subsys_id << 16;
	rcar_pci_write_reg(pcie, val, SUBIDSETR);

	/* PCI/NVMe: interrupt pin은 INTA까지만 지원; NVMe legacy 인터럽트 사용 시 INTA */
	if (hdr->interrupt_pin > PCI_INTERRUPT_INTA)
		return -EINVAL;
	/* PCI/NVMe: Configuration Status/Command/INT 등 포함된 PCICONF(15) 읽기 */
	val = rcar_pci_read_reg(pcie, PCICONF(15));
	/* PCI/NVMe: interrupt pin 필드(INTA=1)를 상위 바이트에 기록 */
	val |= (hdr->interrupt_pin << 8);
	rcar_pci_write_reg(pcie, val, PCICONF(15));

	return 0;
}

/* PCI/NVMe: EP의 inbound BAR를 설정; 호스트가 NVMe controller registers를 매핑할 메모리 윈도우 */
static int rcar_pcie_ep_set_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				struct pci_epf_bar *epf_bar)
{
	/* PCI/NVMe: BAR enable + 64비트 BAR 플래그; NVMe BAR0/2/4 64비트 매핑 필수 */
	int flags = epf_bar->flags | LAR_ENABLE | LAM_64BIT;
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	/* PCI/NVMe: 요청 크기를 2의 거듭제곱으로 정렬; NVMe BAR 크기 규칙 준수 */
	u64 size = 1ULL << fls64(epf_bar->size - 1);
	/* PCI/NVMe: CPU측 물리주소; NVMe 호스트가 MMIO로 접근할 시스템 메모리 */
	dma_addr_t cpu_addr = epf_bar->phys_addr;
	/* PCI/NVMe: 설정할 BAR 번호; NVMe는 BAR0(64비트) 또는 BAR2/4 추가 사용 */
	enum pci_barno bar = epf_bar->barno;
	struct rcar_pcie *pcie = &ep->pcie;
	u32 mask;
	int idx;
	int err;

	/* PCI/NVMe: 사용 가능한 inbound ATU 윈도우 검색; NVMe BAR 당 2개(64비트) 소비 */
	idx = find_first_zero_bit(ep->ib_window_map, ep->num_ib_windows);
	if (idx >= ep->num_ib_windows) {
		dev_err(pcie->dev, "no free inbound window\n");
		return -EINVAL;
	}

	/* PCI/NVMe: I/O space BAR이면 IO_SPACE 플래그 추가; NVMe는 MEM space 사용 */
	if ((flags & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO)
		flags |= IO_SPACE;

	/* PCI/NVMe: BAR→ATU 인덱스 매핑 저장; 해제 시 복원에 사용 */
	ep->bar_to_atu[bar] = idx;
	/* use 64-bit BARs */
	/* PCI/NVMe: 64비트 BAR은 상위/하위 DWORD 2개의 ATU 윈도우를 점유 */
	set_bit(idx, ep->ib_window_map);
	set_bit(idx + 1, ep->ib_window_map);

	/* PCI/NVMe: cpu_addr가 0보다 크면 정렬 제약을 size에 반영 */
	if (cpu_addr > 0) {
		unsigned long nr_zeros = __ffs64(cpu_addr);
		u64 alignment = 1ULL << nr_zeros;

		size = min(size, alignment);
	}

	/* PCI/NVMe: 32비트 BAR 크기로 제한; NVMe BAR 주소 공간 제한 */
	size = min(size, 1ULL << 32);

	/* PCI/NVMe: size를 2의 거듭제곱 마스크로 변환; 하위 4비트는 flags용으로 클리어 */
	mask = roundup_pow_of_two(size) - 1;
	mask &= ~0xf;

	/* PCI/NVMe: inbound ATU를 통해 CPU 주소를 BAR 공간에 매핑; 호스트의 NVMe MMIO 기준 */
	rcar_pcie_set_inbound(pcie, cpu_addr,
			      0x0, mask | flags, idx, false);

	/* PCI/NVMe: PHY ready 대기; 링크가 뜨지 않으면 NVMe 열거 불가 */
	err = rcar_pcie_wait_for_phyrdy(pcie);
	if (err) {
		dev_err(pcie->dev, "phy not ready\n");
		return -EINVAL;
	}

	return 0;
}

/* PCI/NVMe: EP의 inbound BAR 해제; 호스트가 더 이상 NVMe BAR에 접근하지 못함 */
static void rcar_pcie_ep_clear_bar(struct pci_epc *epc, u8 fn, u8 vfn,
				   struct pci_epf_bar *epf_bar)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	/* PCI/NVMe: 해제할 BAR 번호 */
	enum pci_barno bar = epf_bar->barno;
	/* PCI/NVMe: 해당 BAR가 사용한 ATU 인덱스 복원 */
	u32 atu_index = ep->bar_to_atu[bar];

	/* PCI/NVMe: inbound ATU를 0으로 클리어; NVMe MMIO 경로 차단 */
	rcar_pcie_set_inbound(&ep->pcie, 0x0, 0x0, 0x0, bar, false);

	/* PCI/NVMe: 64비트 BAR이 차지한 2개 ATU 윈도우 반납 */
	clear_bit(atu_index, ep->ib_window_map);
	clear_bit(atu_index + 1, ep->ib_window_map);
}

/* PCI/NVMe: EP MSI Capability의 MMC(Message Multiple Capable) 설정 */
static int rcar_pcie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 vfn, u8 nr_irqs)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	/* PCI/NVMe: 요청 IRQ 수의 log2; NVMe MSI 벡터 수(cap)로 변환 */
	u8 mmc = order_base_2(nr_irqs);
	u32 flags;

	/* PCI/NVMe: MSI Capability 레지스터 읽기; NVMe 호스트가 pci_enable_msi() 시 참고 */
	flags = rcar_pci_read_reg(pcie, MSICAP(fn));
	/* PCI/NVMe: MMC 필드 기록; 호스트가 요청 가능한 MSI 벡터 수 결정 */
	flags |= mmc << MSICAP0_MMESCAP_OFFSET;
	rcar_pci_write_reg(pcie, flags, MSICAP(fn));

	return 0;
}

/* PCI/NVMe: EP가 현재 노출하는 MSI 벡터 수 반환; NVMe nvme_reset_work에서 MSI 설정 시 사용 */
static int rcar_pcie_ep_get_msi(struct pci_epc *epc, u8 fn, u8 vfn)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	u32 flags;

	/* PCI/NVMe: MSI Capability 읽기; MSI Enable 비트 확인 */
	flags = rcar_pci_read_reg(pcie, MSICAP(fn));
	if (!(flags & MSICAP0_MSIE))
		return -EINVAL;

	/* PCI/NVMe: MME(Multiple Message Enabled) 필드에서 실제 사용 벡터 수 계산 */
	return 1 << ((flags & MSICAP0_MMESE_MASK) >> MSICAP0_MMESE_OFFSET);
}

/* PCI/NVMe: CPU 물리주소 → PCI 주소 outbound ATU 매핑; NVMe 호스트 DMA/메모리 맵핑 */
static int rcar_pcie_ep_map_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				 phys_addr_t addr, u64 pci_addr, size_t size)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct rcar_pcie *pcie = &ep->pcie;
	struct resource_entry win;	/* PCI/NVMe: ATU에 전달할 resource 엔트리 */
	struct resource res;		/* PCI/NVMe: PCI 주소 범위 */
	int window;			/* PCI/NVMe: 선택된 outbound ATU 윈도우 */
	int err;

	/* check if we have a link. */
	/* PCI/NVMe: Data Link Layer가 up인지 확인; NVMe 열거/IO 전송 전 필수 */
	err = rcar_pcie_wait_for_dl(pcie);
	if (err) {
		dev_err(pcie->dev, "link not up\n");
		return err;
	}

	/* PCI/NVMe: addr에 해당하는 outbound 윈도우 검색; DMA 메모리 영역 일치해야 함 */
	window = rcar_pcie_ep_get_window(ep, addr);
	if (window < 0) {
		dev_err(pcie->dev, "failed to get corresponding window\n");
		return -EINVAL;
	}

	/* PCI/NVMe: resource_entry 초기화; ATU 설정용 임시 구조체 */
	memset(&win, 0x0, sizeof(win));
	memset(&res, 0x0, sizeof(res));
	/* PCI/NVMe: 호스트가 보는 PCI 버스 주소 범위; NVMe DMA 주소 = pci_addr */
	res.start = pci_addr;
	res.end = pci_addr + size - 1;
	res.flags = IORESOURCE_MEM;
	win.res = &res;

	/* PCI/NVMe: outbound ATU를 설정하여 PCI 주소를 CPU 물리주소로 변환; IOMMU 없는 환경의 DMA 변환 */
	rcar_pcie_set_outbound(pcie, window, &win);

	/* PCI/NVMe: 매핑된 CPU 물리주소 기록; 해제 시 역참조 */
	ep->ob_mapped_addr[window] = addr;

	return 0;
}

/* PCI/NVMe: outbound ATU 매핑 해제; NVMe DMA 종료 후 주소 변환 제거 */
static void rcar_pcie_ep_unmap_addr(struct pci_epc *epc, u8 fn, u8 vfn,
				    phys_addr_t addr)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);
	struct resource_entry win;
	struct resource res;
	int idx;

	/* PCI/NVMe: addr로 등록된 outbound 윈도우를 선형 검색 */
	for (idx = 0; idx < ep->num_ob_windows; idx++)
		if (ep->ob_mapped_addr[idx] == addr)
			break;

	/* PCI/NVMe: 일치하는 매핑이 없으면 조용히 리턴 */
	if (idx >= ep->num_ob_windows)
		return;

	/* PCI/NVMe: ATU 레지스터를 0으로 클리어; 해당 PCI 주소는 미지정 상태로 변환 */
	memset(&win, 0x0, sizeof(win));
	memset(&res, 0x0, sizeof(res));
	win.res = &res;
	rcar_pcie_set_outbound(&ep->pcie, idx, &win);

	/* PCI/NVMe: 매핑 테이블에서 제거 */
	ep->ob_mapped_addr[idx] = 0;
}

/* PCI/NVMe: EP가 INTx(legacy) 인터럽트를 어서트; NVMe 호스트의 legacy IRQ 핸들러로 전달 */
static int rcar_pcie_ep_assert_intx(struct rcar_pcie_endpoint *ep,
				    u8 fn, u8 intx)
{
	struct rcar_pcie *pcie = &ep->pcie;
	u32 val;

	/* PCI/NVMe: MSI가 활성화되면 INTx와 동시 사용 불가; NVMe는 보통 MSI/MSI-X 선호 */
	val = rcar_pci_read_reg(pcie, PCIEMSITXR);
	if ((val & PCI_MSI_FLAGS_ENABLE)) {
		dev_err(pcie->dev, "MSI is enabled, cannot assert INTx\n");
		return -EINVAL;
	}

	/* PCI/NVMe: INTx 전송 비활성화 상태면 어서트 불가 */
	val = rcar_pci_read_reg(pcie, PCICONF(1));
	if ((val & INTDIS)) {
		dev_err(pcie->dev, "INTx message transmission is disabled\n");
		return -EINVAL;
	}

	/* PCI/NVMe: 이미 INTx가 어서트된 상태면 중복 어서트 방지 */
	val = rcar_pci_read_reg(pcie, PCIEINTXR);
	if ((val & ASTINTX)) {
		dev_err(pcie->dev, "INTx is already asserted\n");
		return -EINVAL;
	}

	/* PCI/NVMe: INTx 어서트 비트 설정; 호스트 루트 컴플렉스에 인터럽트 메시지 전송 */
	val |= ASTINTX;
	rcar_pci_write_reg(pcie, val, PCIEINTXR);
	/* PCI/NVMe: 1ms 정도 pulse 유지; 레거시 PCI INTx 어서트 폭 보장 */
	usleep_range(1000, 1001);
	/* PCI/NVMe: INTx 디어서트; Edge 방식이 아닌 Level 인터럽트의 pulse 구현 */
	val = rcar_pci_read_reg(pcie, PCIEINTXR);
	val &= ~ASTINTX;
	rcar_pci_write_reg(pcie, val, PCIEINTXR);

	return 0;
}

/* PCI/NVMe: EP가 MSI 인터럽트를 어서트; NVMe 호스트의 MSI 벡터로 전달 */
static int rcar_pcie_ep_assert_msi(struct rcar_pcie *pcie,
				   u8 fn, u8 interrupt_num)
{
	u16 msi_count;	/* PCI/NVMe: 사용 가능한 MSI 벡터 수 */
	u32 val;

	/* Check MSI enable bit */
	/* PCI/NVMe: MSI Enable 비트 확인; NVMe 호스트가 pci_enable_msi()로 활성화해야 함 */
	val = rcar_pci_read_reg(pcie, MSICAP(fn));
	if (!(val & MSICAP0_MSIE))
		return -EINVAL;

	/* Get MSI numbers from MME */
	/* PCI/NVMe: MME 필드에서 현재 허용된 MSI 벡터 수 산출 */
	msi_count = ((val & MSICAP0_MMESE_MASK) >> MSICAP0_MMESE_OFFSET);
	msi_count = 1 << msi_count;

	/* PCI/NVMe: 벡터 번호 범위 검증; 잘못된 벡터는 무시 */
	if (!interrupt_num || interrupt_num > msi_count)
		return -EINVAL;

	/* PCI/NVMe: MSI-X가 아닌 MSI 트리거 레지스터에 벡터 번호 기록; 호스트 CPU로 MSI TLP 전송 */
	val = rcar_pci_read_reg(pcie, PCIEMSITXR);
	rcar_pci_write_reg(pcie, val | (interrupt_num - 1), PCIEMSITXR);

	return 0;
}

/* PCI/NVMe: EP가 인터럽트를 발생; NVMe 호스트는 MSI/INTx 중 하나로 CQ 완료 알림 수신 */
static int rcar_pcie_ep_raise_irq(struct pci_epc *epc, u8 fn, u8 vfn,
				  unsigned int type, u16 interrupt_num)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);

	/* PCI/NVMe: 인터럽트 유형별 분기; NVMe reset/config 단계에서 협상된 방식 사용 */
	switch (type) {
	case PCI_IRQ_INTX:
		/* PCI/NVMe: 레거시 INTx 어서트; nvme_probe에서 MSI 실패 시 폭백(fallback) */
		return rcar_pcie_ep_assert_intx(ep, fn, 0);

	case PCI_IRQ_MSI:
		/* PCI/NVMe: MSI 벡터 어서트; NVMe CQ 인터럽트의 표준 경로 */
		return rcar_pcie_ep_assert_msi(&ep->pcie, fn, interrupt_num);

	default:
		/* PCI/NVMe: MSI-X 등 미지원 인터럽트 유형; NVMe 호스트는 MSI-X 폭백(fallback) 시 실패 */
		return -EINVAL;
	}
}

/* PCI/NVMe: EP 컨트롤러 동작 시작; 링크 업 후 NVMe 호스트가 열거 가능 */
static int rcar_pcie_ep_start(struct pci_epc *epc)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);

	/* PCI/NVMe: MAC 제어 레지스터를 초기값으로 설정; LTSSM 시작 준비 */
	rcar_pci_write_reg(&ep->pcie, MACCTLR_INIT_VAL, MACCTLR);
	/* PCI/NVMe: Configuration Done, LTSSM init; 호스트 측에서 link up 및 PCIe 열거 시작 */
	rcar_pci_write_reg(&ep->pcie, CFINIT, PCIETCTLR);

	return 0;
}

/* PCI/NVMe: EP 컨트롤러 동작 중지; NVMe 호스트와의 링크 종료 */
static void rcar_pcie_ep_stop(struct pci_epc *epc)
{
	struct rcar_pcie_endpoint *ep = epc_get_drvdata(epc);

	/* PCI/NVMe: PCIe 트랜잭션 제어 레지스터를 0으로; LTSSM 정지 및 열거 해제 */
	rcar_pci_write_reg(&ep->pcie, 0, PCIETCTLR);
}

/* PCI/NVMe: 이 EP 컨트롤러가 지원하는 기능; NVMe 호스트가 BAR/MSI 협상 시 참고 */
static const struct pci_epc_features rcar_pcie_epc_features = {
	.msi_capable = true,	/* PCI/NVMe: MSI 지원; NVMe 호스트가 pci_enable_msi() 가능 */
	/* use 64-bit BARs so mark BAR[1,3,5] as reserved */
	/* PCI/NVMe: 64비트 BAR만 지원하므로 홀수 BAR는 예약; NVMe BAR0(64bit) 매핑 */
	.bar[BAR_0] = { .type = BAR_FIXED, .fixed_size = 128,
			.only_64bit = true, },
	.bar[BAR_2] = { .type = BAR_FIXED, .fixed_size = 256,
			.only_64bit = true, },
	.bar[BAR_4] = { .type = BAR_FIXED, .fixed_size = 256,
			.only_64bit = true, },
};

/* PCI/NVMe: EPC feature 구조체 반환; NVMe endpoint function 등록 시 사용 */
static const struct pci_epc_features*
rcar_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	return &rcar_pcie_epc_features;
}

/* PCI/NVMe: PCI Endpoint Controller 연산 테이블; NVMe EPF 드라이버가 콜백 호출 */
static const struct pci_epc_ops rcar_pcie_epc_ops = {
	.write_header	= rcar_pcie_ep_write_header,	/* PCI/NVMe: NVMe VID/DID/Class/INTx 설정 */
	.set_bar	= rcar_pcie_ep_set_bar,		/* PCI/NVMe: NVMe BAR(register memory) 매핑 */
	.clear_bar	= rcar_pcie_ep_clear_bar,	/* PCI/NVMe: NVMe BAR 해제 */
	.set_msi	= rcar_pcie_ep_set_msi,		/* PCI/NVMe: NVMe MSI 벡터 수 설정 */
	.get_msi	= rcar_pcie_ep_get_msi,		/* PCI/NVMe: 현재 NVMe MSI 벡터 수 조회 */
	.map_addr	= rcar_pcie_ep_map_addr,	/* PCI/NVMe: NVMe DMA용 outbound ATU 설정 */
	.unmap_addr	= rcar_pcie_ep_unmap_addr,	/* PCI/NVMe: NVMe DMA용 outbound ATU 해제 */
	.raise_irq	= rcar_pcie_ep_raise_irq,	/* PCI/NVMe: NVMe 완료 인터럽트 발생 */
	.start		= rcar_pcie_ep_start,		/* PCI/NVMe: NVMe EP 동작 시작(링크업) */
	.stop		= rcar_pcie_ep_stop,		/* PCI/NVMe: NVMe EP 동작 중지 */
	.get_features	= rcar_pcie_ep_get_features,	/* PCI/NVMe: NVMe EP capability/BAR 특성 */
};

/* PCI/NVMe: Device Tree compatible 목록; NVMe EP가 탑재될 R-Car SoC 매칭 */
static const struct of_device_id rcar_pcie_ep_of_match[] = {
	{ .compatible = "renesas,r8a774c0-pcie-ep", },
	{ .compatible = "renesas,rcar-gen3-pcie-ep" },
	{ },
};

/* PCI/NVMe: platform_driver probe; R-Car PCIe EP 컨트롤러를 초기화하고 NVMe EP 등록 */
static int rcar_pcie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rcar_pcie_endpoint *ep;	/* PCI/NVMe: EP 컨트롤러 소프트웨어 상태 */
	struct rcar_pcie *pcie;		/* PCI/NVMe: 공용 PCIe MAC 상태 */
	struct pci_epc *epc;		/* PCI/NVMe: PCI Endpoint Controller 장치 */
	int err;

	/* PCI/NVMe: EP 상태 구조체 할당; NVMe EPF 바인딩 전 드라이버 사설 데이터 */
	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	/* PCI/NVMe: 공용 PCIe MAC 상태 포인터 연결 */
	pcie = &ep->pcie;
	/* PCI/NVMe: device 포인터 저장; dev_err/pm_runtime 등에 사용 */
	pcie->dev = dev;

	/* PCI/NVMe: Runtime PM 활성화; NVMe ASPM/전력 관리의 기반 */
	pm_runtime_enable(dev);
	/* PCI/NVMe: 디바이스를 active 상태로 전환; 레지스터 접근 전 전원 안정화 */
	err = pm_runtime_resume_and_get(dev);
	if (err < 0) {
		dev_err(dev, "pm_runtime_resume_and_get failed\n");
		goto err_pm_disable;
	}

	/* PCI/NVMe: DT 리소스 파싱 및 PCIe MAC 레지스터 ioremap; NVMe BAR/DMA 메모리 출처 확보 */
	err = rcar_pcie_ep_get_pdata(ep, pdev);
	if (err < 0) {
		dev_err(dev, "failed to request resources: %d\n", err);
		goto err_pm_put;
	}

	/* PCI/NVMe: 최대 inbound ATU 윈도우 수 설정; NVMe BAR 매핑 한도 */
	ep->num_ib_windows = MAX_NR_INBOUND_MAPS;
	/* PCI/NVMe: inbound 윈도우 사용 비트맵 할당; BAR별 ATU 점유 추적 */
	ep->ib_window_map =
			devm_kcalloc(dev, BITS_TO_LONGS(ep->num_ib_windows),
				     sizeof(long), GFP_KERNEL);
	if (!ep->ib_window_map) {
		err = -ENOMEM;
		dev_err(dev, "failed to allocate memory for inbound map\n");
		goto err_pm_put;
	}

	/* PCI/NVMe: outbound 매핑된 주소 테이블 할당; NVMe DMA 범위별 ATU 역참조 */
	ep->ob_mapped_addr = devm_kcalloc(dev, ep->num_ob_windows,
					  sizeof(*ep->ob_mapped_addr),
					  GFP_KERNEL);
	if (!ep->ob_mapped_addr) {
		err = -ENOMEM;
		dev_err(dev, "failed to allocate memory for outbound memory pointers\n");
		goto err_pm_put;
	}

	/* PCI/NVMe: PCI Endpoint Controller 장치 생성; NVMe EPF 드라이버가 이 epc에 바인딩 */
	epc = devm_pci_epc_create(dev, &rcar_pcie_epc_ops);
	if (IS_ERR(epc)) {
		dev_err(dev, "failed to create epc device\n");
		err = PTR_ERR(epc);
		goto err_pm_put;
	}

	/* PCI/NVMe: EPC가 노출할 function 수 설정; NVMe는 단일 function */
	epc->max_functions = ep->max_functions;
	/* PCI/NVMe: EPC private 데이터로 ep 저장; 콜백에서 ep 상태 획득 */
	epc_set_drvdata(epc, ep);

	/* PCI/NVMe: PCIe MAC Endpoint 모드 하드웨어 초기화; NVMe 호스트가 볼 capability 세팅 */
	rcar_pcie_ep_hw_init(pcie);

	/* PCI/NVMe: outbound 메모리 공간 초기화; NVMe DMA/매핑에 사용될 물리 메모리 풀 */
	err = pci_epc_multi_mem_init(epc, ep->ob_window, ep->num_ob_windows);
	if (err < 0) {
		dev_err(dev, "failed to initialize the epc memory space\n");
		goto err_pm_put;
	}

	/* PCI/NVMe: EPC 초기화 완료 알림; NVMe endpoint function 드라이버가 등록 가능해짐 */
	pci_epc_init_notify(epc);

	return 0;

err_pm_put:
	/* PCI/NVMe: 리소스 할당/초기화 실패 시 runtime PM 사용 카운트 반납 */
	pm_runtime_put(dev);

err_pm_disable:
	/* PCI/NVMe: runtime PM 비활성화; NVMe EP 전원 관리 종료 */
	pm_runtime_disable(dev);

	return err;
}

/* PCI/NVMe: R-Car PCIe Endpoint platform_driver 정의; NVMe EP 바인딩의 진입점 */
static struct platform_driver rcar_pcie_ep_driver = {
	.driver = {
		.name = "rcar-pcie-ep",	/* PCI/NVMe: platform bus 드라이버 이름 */
		.of_match_table = rcar_pcie_ep_of_match,	/* PCI/NVMe: DT compatible 매칭 테이블 */
		.suppress_bind_attrs = true,	/* PCI/NVMe: sysfs 수동 바인딩 금지; PCIe EP는 자동 probe */
	},
	.probe = rcar_pcie_ep_probe,	/* PCI/NVMe: NVMe EP 컨트롤러 초기화 콜백 */
};
/* PCI/NVMe: 커널 빌트인 platform_driver 등록; NVMe 호스트 시스템 부팅 시 자동 로드 */
builtin_platform_driver(rcar_pcie_ep_driver);
