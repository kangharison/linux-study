// SPDX-License-Identifier: GPL-2.0
/* [한국어] 위 SPDX 줄은 커널의 라이선스 표기 규약이다. 반드시 파일의
 * 첫 줄에 정확한 형식으로만 있어야 scripts/spdxcheck.py 가 인식하므로,
 * 그 앞이나 뒤에 아무것도 덧붙이지 않는다. */
/*
 * Synopsys DesignWare PCIe Endpoint controller driver
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

#include <linux/align.h>
/* NVMe: 메모리/바운드 정렬 매크로; BAR/버퍼 alignment에 사용 */
#include <linux/bitfield.h>
/* NVMe: FIELD_GET/PREP 등 비트필드 조작; PCIe capability 레지스터 파싱 */
#include <linux/of.h>
/* NVMe: DT 파싱; NVMe SSD가 연결된 RC/EP의 OF 노드 접근 */
#include <linux/platform_device.h>
/* NVMe: platform_driver 등록; PCIe 컨트롤러 프로브 */

#include "pcie-designware.h"
/* NVMe: DesignWare PCIe 공통 레지스터/구조체; 호스트-EP 공용 헤더 */
#include <linux/pci-epc.h>
/* NVMe: PCI Endpoint Controller 프레임워크; NVMe 장치의 EP 시뮬레이션/바인딩 */
#include <linux/pci-epf.h>
/* NVMe: PCI Endpoint Function 프레임워크; NVMe EP function 등록 */

/**
 * dw_pcie_ep_get_func_from_ep - Get the struct dw_pcie_ep_func corresponding to
 *				 the endpoint function
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint device
 *
 * Return: struct dw_pcie_ep_func if success, NULL otherwise.
 */
struct dw_pcie_ep_func *
dw_pcie_ep_get_func_from_ep(struct dw_pcie_ep *ep, u8 func_no)
{
	struct dw_pcie_ep_func *ep_func;

	/* NVMe: 등록된 EP function 리스트를 순회; NVMe 호스트가 여러 function을 볼 때 각 function 객체 탐색 */
	list_for_each_entry(ep_func, &ep->func_list, list) {
	/* NVMe: 요청한 function 번호와 일치하는지 확인; SR-IOV 등 다중 function 지원 */
		if (ep_func->func_no == func_no)
			return ep_func;
	}

	return NULL;
}

static void __dw_pcie_ep_reset_bar(struct dw_pcie *pci, u8 func_no,
				   enum pci_barno bar, int flags)
{
	struct dw_pcie_ep *ep = &pci->ep;
	u32 reg;

	/* NVMe: BAR0~5 중 해당 BAR의 PCI config space 오프셋 계산; NVMe BAR0/1(64-bit) 접근 시 사용 */
	reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	/* NVMe: DBI read-only 쓰기 잠금 해제; NVMe 호스트가 쓴 BAR 주소를 EP가 초기화/덮어쓸 수 있도록 허용 */
	dw_pcie_dbi_ro_wr_en(pci);
	/* NVMe: BAR mask 레지스터(dbi2) 0으로 초기화; 호스트가 할당한 주소를 클리어하여 재할당 준비 */
	dw_pcie_ep_writel_dbi2(ep, func_no, reg, 0x0);
	/* NVMe: BAR 실제 레지스터 0으로 초기화; NVMe 장치의 MMIO 베이스 주소 해제 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, 0x0);
	if (flags & PCI_BASE_ADDRESS_MEM_TYPE_64) {
		/* NVMe: 64-bit BAR 상위 32-bit도 동일하게 클리어; NVMe BAR0/1 쌍 초기화 */
		dw_pcie_ep_writel_dbi2(ep, func_no, reg + 4, 0x0);
		dw_pcie_ep_writel_dbi(ep, func_no, reg + 4, 0x0);
	}
	/* NVMe: DBI read-only 쓰기 다시 잠금; 호스트의 임의 BAR 덮어쓰기 방지 */
	dw_pcie_dbi_ro_wr_dis(pci);
}

/**
 * dw_pcie_ep_reset_bar - Reset endpoint BAR
 * @pci: DWC PCI device
 * @bar: BAR number of the endpoint
 */
void dw_pcie_ep_reset_bar(struct dw_pcie *pci, enum pci_barno bar)
{
	u8 func_no, funcs;

	/* NVMe: 이 EPC가 지원하는 최대 function 수 획득; 다중 function NVMe EP 모두 초기화 */
	funcs = pci->ep.epc->max_functions;

	/* NVMe: 모든 function의 지정 BAR을 리셋; 호스트 열거 직전 EP측 BAR 상태 정리 */
	for (func_no = 0; func_no < funcs; func_no++)
		__dw_pcie_ep_reset_bar(pci, func_no, bar, 0);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_reset_bar);

static u8 dw_pcie_ep_find_capability(struct dw_pcie_ep *ep, u8 func_no, u8 cap)
{
	/* NVMe: PCI capability linked list 순회; NVMe 호스트가 읽는 MSI/MSI-X/PM capability 위치 탐색 */
	return PCI_FIND_NEXT_CAP(dw_pcie_ep_read_cfg, PCI_CAPABILITY_LIST,
				 cap, NULL, ep, func_no);
}

static u16 dw_pcie_ep_find_ext_capability(struct dw_pcie_ep *ep,
					  u8 func_no, u8 cap)
{
	/* NVMe: PCIe extended capability 순회; AER/ACS/PTM/Resizable BAR 등 NVMe 관련 확장 capability 탐색 */
	return PCI_FIND_NEXT_EXT_CAP(dw_pcie_ep_read_cfg, 0,
				     cap, NULL, ep, func_no);
}

static int dw_pcie_ep_write_header(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				   struct pci_epf_header *hdr)
{
	/* NVMe: config header의 Vendor/Device ID 등을 DBI로 기록; NVMe 호스트가 pci_device_id 매칭 시 사용 */
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* NVMe: Vendor ID; lspci -nn에서 보이는 값, NVMe SSD厂商 ID 설정 */
	dw_pcie_dbi_ro_wr_en(pci);
	/* NVMe: Device ID; NVMe 호스트 드라이버가 pci_device_id 테이블으로 매칭하는 값 */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_VENDOR_ID, hdr->vendorid);
	/* NVMe: Revision ID; NVMe 컨트롤러 버전/리비전 노출 */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_DEVICE_ID, hdr->deviceid);
	/* NVMe: Prog IF; NVMe 장치의 programming interface 필드 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_REVISION_ID, hdr->revid);
	/* NVMe: Class Code (base+subclass); NVMe는 0x010802 등 Mass Storage/NVM 사용 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_CLASS_PROG, hdr->progif_code);
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_CLASS_DEVICE,
			      hdr->subclass_code | hdr->baseclass_code << 8);
	/* NVMe: Cache Line Size; 호스트 MPS/MRRS와 cache line 일치 권장 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_CACHE_LINE_SIZE,
			      hdr->cache_line_size);
	/* NVMe: Subsystem Vendor ID; NVMe SSD 모듈/시스템 integrator ID */
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_SUBSYSTEM_VENDOR_ID,
	/* NVMe: Subsystem ID; NVMe SSD별 식별자 */
			      hdr->subsys_vendor_id);
	dw_pcie_ep_writew_dbi(ep, func_no, PCI_SUBSYSTEM_ID, hdr->subsys_id);
	/* NVMe: Interrupt Pin; legacy INT# 핀(보통 0); NVMe는 MSI/MSI-X 권장 */
	dw_pcie_ep_writeb_dbi(ep, func_no, PCI_INTERRUPT_PIN,
			      hdr->interrupt_pin);
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

/* BAR Match Mode inbound iATU mapping */
static int dw_pcie_ep_ib_atu_bar(struct dw_pcie_ep *ep, u8 func_no, int type,
				 dma_addr_t parent_bus_addr, enum pci_barno bar,
				 size_t size)
{
	int ret;
	u32 free_win;
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);

	/* NVMe: function 객체 없으면 매핑 불가; 호스트는 이 BAR 접근 시 MMIO 실패 */
	if (!ep_func)
		return -EINVAL;

	/* NVMe: 이미 매핑된 BAR이면 기존 iATU 윈도우 재사용, 아니면 첫 번째 여유 윈도우 탐색 */
	if (!ep_func->bar_to_atu[bar])
		free_win = find_first_zero_bit(ep->ib_window_map, pci->num_ib_windows);
	else
		free_win = ep_func->bar_to_atu[bar] - 1;

	/* NVMe: 여유 inbound ATU 윈도우 없음; NVMe 호스트의 BAR 접근이 실패/버스 오류 유발 */
	if (free_win >= pci->num_ib_windows) {
		dev_err(pci->dev, "No free inbound window\n");
		return -EINVAL;
	}

	/* NVMe: inbound ATU 프로그래밍; 호스트가 BAR에 쓴 PCIe 주소를 EP 낸부 메모리로 변환 */
	ret = dw_pcie_prog_ep_inbound_atu(pci, func_no, free_win, type,
					  parent_bus_addr, bar, size);
	if (ret < 0) {
		dev_err(pci->dev, "Failed to program IB window\n");
		return ret;
	}

	/* NVMe: 0은 미할당 표시이므로 +1 저장; NVMe 호스트의 BAR MMIO 매핑 추적 */
	/*
	 * Always increment free_win before assignment, since value 0 is used to identify
	 * unallocated mapping.
	 */
	ep_func->bar_to_atu[bar] = free_win + 1;
	/* NVMe: iATU 윈도우 사용 중 표시; 다른 BAR/Submap과 충돌 방지 */
	set_bit(free_win, ep->ib_window_map);

	return 0;
}

static void dw_pcie_ep_clear_ib_maps(struct dw_pcie_ep *ep, u8 func_no, enum pci_barno bar)
{
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct device *dev = pci->dev;
	unsigned int i, num;
	u32 atu_index;
	u32 *indexes;

	/* NVMe: function 미등록 시 정리할 것 없음 */
	if (!ep_func)
		return;

	/* Tear down the BAR Match Mode mapping, if any. */
	/* NVMe: BAR Match Mode 매핑 해제; NVMe 호스트가 이 BAR로의 MMIO 접근을 중단하면 EP 낸부 라우팅 제거 */
	if (ep_func->bar_to_atu[bar]) {
		atu_index = ep_func->bar_to_atu[bar] - 1;
	/* NVMe: 해당 inbound ATU 윈도우 비활성화; 호스트 MMIO 트랜잭션 거부/무시 */
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_IB, atu_index);
	/* NVMe: 윈도우 맵에서 해제 표시; 다른 BAR/Submap이 재사용 가능 */
		clear_bit(atu_index, ep->ib_window_map);
	/* NVMe: 미할당 상태로 되돌림 */
		ep_func->bar_to_atu[bar] = 0;
		return;
	}

	/* Tear down all Address Match Mode mappings, if any. */
	/* NVMe: Address Match Mode submap 리스트 획득; NVMe BAR 낸부 세그먼트별 매핑 정리 */
	indexes = ep_func->ib_atu_indexes[bar];
	num = ep_func->num_ib_atu_indexes[bar];
	ep_func->ib_atu_indexes[bar] = NULL;
	ep_func->num_ib_atu_indexes[bar] = 0;
	if (!indexes)
		return;
	/* NVMe: 각 submap별 iATU 윈도우 비활성화; 호스트가 NVMe BAR의 특정 오프셋에 접근 불가 */
	for (i = 0; i < num; i++) {
		dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_IB, indexes[i]);
		clear_bit(indexes[i], ep->ib_window_map);
	}
	/* NVMe: submap 인덱스 배열 메모리 해제; 리소스 누수 방지 */
	devm_kfree(dev, indexes);
}

static u64 dw_pcie_ep_read_bar_assigned(struct dw_pcie_ep *ep, u8 func_no,
					enum pci_barno bar, int flags)
{
	u32 reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	u32 lo, hi;
	u64 addr;

	/* NVMe: 호스트가 BAR에 할당한(쓴) 하위 32-bit 주소 읽기; NVMe BAR0 베이스 획득 */
	lo = dw_pcie_ep_readl_dbi(ep, func_no, reg);

	/* NVMe: I/O space BAR이면 I/O 마스크 적용; NVMe는 메모리 BAR 위주 */
	if (flags & PCI_BASE_ADDRESS_SPACE)
		return lo & PCI_BASE_ADDRESS_IO_MASK;

	/* NVMe: 메모리 BAR 하위 주소; NVMe 호스트가 pci_write_config로 할당한 주소 */
	addr = lo & PCI_BASE_ADDRESS_MEM_MASK;
	/* NVMe: 32-bit BAR이면 여기서 반환; NVMe 소형 BAR의 경우 */
	if (!(flags & PCI_BASE_ADDRESS_MEM_TYPE_64))
		return addr;

	/* NVMe: 64-bit BAR 상위 32-bit 읽기; NVMe BAR0/1 쌍의 전체 64-bit 베이스 구성 */
	hi = dw_pcie_ep_readl_dbi(ep, func_no, reg + 4);
	return addr | ((u64)hi << 32);
}

static int dw_pcie_ep_validate_submap(struct dw_pcie_ep *ep,
				      const struct pci_epf_bar_submap *submap,
				      unsigned int num_submap, size_t bar_size)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	u32 align = pci->region_align;
	size_t off = 0;
	unsigned int i;
	size_t size;

	/* NVMe: region alignment가 없거나 BAR 크기가 정렬되지 않으면 submap 불가; NVMe BAR 정렬 위배 시 실패 */
	if (!align || !IS_ALIGNED(bar_size, align))
		return -EINVAL;

	/*
	 * The submap array order defines the BAR layout (submap[0] starts
	 * at offset 0 and each entry immediately follows the previous
	 * one). Here, validate that it forms a strict, gapless
	 * decomposition of the BAR:
	 *  - each entry has a non-zero size
	 *  - sizes, implicit offsets and phys_addr are aligned to
	 *    pci->region_align
	 *  - each entry lies within the BAR range
	 *  - the entries exactly cover the whole BAR
	 *
	 * Note: dw_pcie_prog_inbound_atu() also checks alignment for the
	 * PCI address and the target phys_addr, but validating up-front
	 * avoids partially programming iATU windows in vain.
	 */
	/* NVMe: submap 배열을 순회하며 BAR 내 gapless 분할 검증; NVMe BAR 낸부 여러 영역이 물리 메모리에 정확히 대응하는지 확인 */
	for (i = 0; i < num_submap; i++) {
		size = submap[i].size;

	/* NVMe: 0 크기 submap은 허용되지 않음; NVMe BAR 세그먼트가 비어 있으면 안 됨 */
		if (!size)
			return -EINVAL;

	/* NVMe: size/offset이 region_align에 정렬되어야 iATU가 동작; NVMe DMA/Mem alignment 요건 충족 */
		if (!IS_ALIGNED(size, align) || !IS_ALIGNED(off, align))
			return -EINVAL;

	/* NVMe: 대상 물리 주소도 정렬 필요; IOMMU/DMA 주소 불일치 방지 */
		if (!IS_ALIGNED(submap[i].phys_addr, align))
			return -EINVAL;

	/* NVMe: submap이 BAR 범위를 벗어나면 안 됨; NVMe 호스트의 MMIO 오프셋 보호 */
		if (off > bar_size || size > bar_size - off)
			return -EINVAL;

		off += size;
	}
	/* NVMe: submap 전체가 BAR 크기와 정확히 일치해야 함; NVMe BAR 낸부 미매핑 영역 방지 */
	if (off != bar_size)
		return -EINVAL;

	return 0;
}

/* Address Match Mode inbound iATU mapping */
static int dw_pcie_ep_ib_atu_addr(struct dw_pcie_ep *ep, u8 func_no, int type,
				  const struct pci_epf_bar *epf_bar)
{
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	const struct pci_epf_bar_submap *submap = epf_bar->submap;
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum pci_barno bar = epf_bar->barno;
	struct device *dev = pci->dev;
	u64 pci_addr, parent_bus_addr;
	u64 size, base, off = 0;
	int free_win, ret;
	unsigned int i;
	u32 *indexes;

	/* NVMe: function, submap, size 유효성 검사; NVMe BAR 매핑 실패 시 조기 리턴 */
	if (!ep_func || !epf_bar->num_submap || !submap || !epf_bar->size)
		return -EINVAL;

	/* NVMe: submap layout 검증; NVMe BAR 낸부 여러 영역이 물리 메모리에 정확히 대응하는지 확인 */
	ret = dw_pcie_ep_validate_submap(ep, submap, epf_bar->num_submap,
					 epf_bar->size);
	if (ret)
		return ret;

	/* NVMe: 호스트가 할당한 BAR 베이스 주소 읽기; NVMe BAR0의 시스템 버스 주소 획득 */
	base = dw_pcie_ep_read_bar_assigned(ep, func_no, bar, epf_bar->flags);
	if (!base) {
		dev_err(dev,
			"BAR%u not assigned, cannot set up sub-range mappings\n",
			bar);
		return -EINVAL;
	}

	/* NVMe: submap 개수만큼 인덱스 배열 할당; 각 iATU 윈도우 번호 저장 */
	indexes = devm_kcalloc(dev, epf_bar->num_submap, sizeof(*indexes),
			       GFP_KERNEL);
	if (!indexes)
		return -ENOMEM;

	/* NVMe: function에 submap 인덱스 배열 연결; 후속 clear_ib_maps에서 해제 */
	ep_func->ib_atu_indexes[bar] = indexes;
	ep_func->num_ib_atu_indexes[bar] = 0;

	/* NVMe: 각 submap마다 별도 inbound iATU 윈도우 프로그래밍; NVMe BAR 오프셋별 라우팅 */
	for (i = 0; i < epf_bar->num_submap; i++) {
		size = submap[i].size;
	/* NVMe: 이 submap이 라우팅할 EP 측 물리 주소; NVMe 컨트롤러 레지스터/버퍼 메모리 */
		parent_bus_addr = submap[i].phys_addr;

	/* NVMe: base+off 연산에서 오버플로우 방지; 64-bit BAR 주소 안전성 */
		if (off > (~0ULL) - base) {
			ret = -EINVAL;
			goto err;
		}

	/* NVMe: PCIe 버스 주소 = BAR 베이스 + submap 오프셋; NVMe 호스트가 보는 MMIO 주소 */
		pci_addr = base + off;
		off += size;

	/* NVMe: outbound가 아닌 inbound에서 여유 ATU 윈도우 탐색 */
		free_win = find_first_zero_bit(ep->ib_window_map,
					       pci->num_ib_windows);
	/* NVMe: inbound 윈도우 부족; NVMe BAR의 일부 영역 매핑 실패 */
		if (free_win >= pci->num_ib_windows) {
			ret = -ENOSPC;
			goto err;
		}

	/* NVMe: parent_bus_addr(EP 물리) <-> pci_addr(호스트 MMIO) inbound ATU 설정 */
		ret = dw_pcie_prog_inbound_atu(pci, free_win, type,
					       parent_bus_addr, pci_addr, size);
		if (ret)
			goto err;

	/* NVMe: 사용된 inbound 윈도우 표시 및 인덱스 기록 */
		set_bit(free_win, ep->ib_window_map);
		indexes[i] = free_win;
		ep_func->num_ib_atu_indexes[bar] = i + 1;
	}
	return 0;
	/* NVMe: 실패 시 이미 설정된 submap iATU 모두 해제; NVMe BAR 매핑 일관성 유지 */
err:
	dw_pcie_ep_clear_ib_maps(ep, func_no, bar);
	return ret;
}

static int dw_pcie_ep_outbound_atu(struct dw_pcie_ep *ep,
				   struct dw_pcie_ob_atu_cfg *atu)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	u32 free_win;
	int ret;

	/* NVMe: EP가 호스트 메모리/DMA 주소로 트랜잭션을 날릴 outbound ATU 윈도우 탐색 */
	free_win = find_first_zero_bit(ep->ob_window_map, pci->num_ob_windows);
	/* NVMe: outbound 윈도우 없음; NVMe EP가 DMA read/write를 발행할 수 없음 */
	if (free_win >= pci->num_ob_windows) {
		dev_err(pci->dev, "No free outbound window\n");
		return -EINVAL;
	}

	/* NVMe: 선택된 윈도우 번호를 ATU 설정 구조체에 기록 */
	atu->index = free_win;
	/* NVMe: EP 낸부 주소를 PCI 주소로 변환하는 outbound ATU 프로그래밍; NVMe DMA 매핑의 핵심 */
	ret = dw_pcie_prog_outbound_atu(pci, atu);
	if (ret)
		return ret;

	/* NVMe: outbound 윈도우 사용 중 표시; 다른 DMA/MSI 매핑과 충돌 방지 */
	set_bit(free_win, ep->ob_window_map);
	/* NVMe: 이 윈도우의 parent bus 주소 저장; 추후 unmap 시 검색 키로 사용 */
	ep->outbound_addr[free_win] = atu->parent_bus_addr;

	return 0;
}

static void dw_pcie_ep_clear_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				 struct pci_epf_bar *epf_bar)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum pci_barno bar = epf_bar->barno;
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);

	/* NVMe: function이나 BAR이 설정된 적 없으면 아무것도 안 함 */
	if (!ep_func || !ep_func->epf_bar[bar])
		return;

	/* NVMe: BAR 레지스터를 0으로 리셋; 호스트가 할당한 MMIO 베이스 제거 */
	__dw_pcie_ep_reset_bar(pci, func_no, bar, epf_bar->flags);

	/* NVMe: BAR에 연결된 inbound iATU 모두 해제; NVMe 호스트의 MMIO 접근 중단 */
	dw_pcie_ep_clear_ib_maps(ep, func_no, bar);

	/* NVMe: function의 BAR 상태 포인터 클리어; 재설정 가능 상태로 전환 */
	ep_func->epf_bar[bar] = NULL;
}

static unsigned int dw_pcie_ep_get_rebar_offset(struct dw_pcie_ep *ep, u8 func_no,
						enum pci_barno bar)
{
	u32 reg, bar_index;
	unsigned int offset, nbars;
	int i;

	/* NVMe: Resizable BAR extended capability 탐색; NVMe 호스트가 BAR 크기를 재조정할 때 사용 */
	offset = dw_pcie_ep_find_ext_capability(ep, func_no, PCI_EXT_CAP_ID_REBAR);
	if (!offset)
		return offset;

	/* NVMe: REBAR control에서 BAR 개수 획득 */
	reg = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
	nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, reg);

	/* NVMe: REBAR capability 구조체를 순회하며 대상 BAR 인덱스 검색 */
	for (i = 0; i < nbars; i++, offset += PCI_REBAR_CTRL) {
		reg = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
		bar_index = FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, reg);
		if (bar_index == bar)
			return offset;
	}

	return 0;
}

static int dw_pcie_ep_set_bar_resizable(struct dw_pcie_ep *ep, u8 func_no,
					struct pci_epf_bar *epf_bar)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum pci_barno bar = epf_bar->barno;
	size_t size = epf_bar->size;
	int flags = epf_bar->flags;
	u32 reg = PCI_BASE_ADDRESS_0 + (4 * bar);
	unsigned int rebar_offset;
	u32 rebar_cap, rebar_ctrl;
	int ret;

	/* NVMe: 해당 BAR의 Resizable BAR capability 오프셋 획득; NVMe 호스트가 BAR 크기 변경 가능 */
	rebar_offset = dw_pcie_ep_get_rebar_offset(ep, func_no, bar);
	if (!rebar_offset)
		return -EINVAL;

	/* NVMe: 요청 size를 REBAR capability encoding으로 변환; NVMe BAR 크기 협상 */
	ret = pci_epc_bar_size_to_rebar_cap(size, &rebar_cap);
	if (ret)
		return ret;

	/* NVMe: BAR mask/enable 레지스터 쓰기 위해 RO 잠금 해제 */
	dw_pcie_dbi_ro_wr_en(pci);

	/*
	 * A BAR mask should not be written for a resizable BAR. The BAR mask
	 * is automatically derived by the controller every time the "selected
	 * size" bits are updated, see "Figure 3-26 Resizable BAR Example for
	 * 32-bit Memory BAR0" in DWC EP databook 5.96a. We simply need to write
	 * BIT(0) to set the BAR enable bit.
	 */
	/* NVMe: Resizable BAR의 BAR mask 레지스터에는 enable 비트만 설정; NVMe 호스트가 size를 결정 */
	dw_pcie_ep_writel_dbi2(ep, func_no, reg, BIT(0));
	/* NVMe: BAR 레지스터에 flags(64-bit 등) 기록; NVMe 호스트가 BAR 특성 인식 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, flags);

	/* NVMe: 64-bit Resizable BAR 상위 워드 초기화 */
	if (flags & PCI_BASE_ADDRESS_MEM_TYPE_64) {
		dw_pcie_ep_writel_dbi2(ep, func_no, reg + 4, 0);
		dw_pcie_ep_writel_dbi(ep, func_no, reg + 4, 0);
	}

	/*
	 * Bits 31:0 in PCI_REBAR_CAP define "supported sizes" bits for sizes
	 * 1 MB to 128 TB. Bits 31:16 in PCI_REBAR_CTRL define "supported sizes"
	 * bits for sizes 256 TB to 8 EB. Disallow sizes 256 TB to 8 EB.
	 */
	/* NVMe: REBAR control에서 256TB~8EB 지원 비트 클리어; NVMe 호스트가 비현실적 size 요청 차단 */
	rebar_ctrl = dw_pcie_ep_readl_dbi(ep, func_no, rebar_offset + PCI_REBAR_CTRL);
	rebar_ctrl &= ~GENMASK(31, 16);
	dw_pcie_ep_writel_dbi(ep, func_no, rebar_offset + PCI_REBAR_CTRL, rebar_ctrl);

	/*
	 * The "selected size" (bits 13:8) in PCI_REBAR_CTRL are automatically
	 * updated when writing PCI_REBAR_CAP, see "Figure 3-26 Resizable BAR
	 * Example for 32-bit Memory BAR0" in DWC EP databook 5.96a.
	 */
	/* NVMe: REBAR CAP 레지스터에 지원 size 기록; NVMe 호스트가 Set Resizable BAR Capabilities 수행 시 참조 */
	dw_pcie_ep_writel_dbi(ep, func_no, rebar_offset + PCI_REBAR_CAP, rebar_cap);

	/* NVMe: RO 레지스터 쓰기 다시 잠금 */
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static int dw_pcie_ep_set_bar_programmable(struct dw_pcie_ep *ep, u8 func_no,
					   struct pci_epf_bar *epf_bar)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum pci_barno bar = epf_bar->barno;
	size_t size = epf_bar->size;
	int flags = epf_bar->flags;
	u32 reg = PCI_BASE_ADDRESS_0 + (4 * bar);

	/* NVMe: programmable BAR 설정을 위해 RO 쓰기 잠금 해제 */
	dw_pcie_dbi_ro_wr_en(pci);

	/* NVMe: BAR mask 레지스터에 (size-1) 하위 비트 기록; NVMe 호스트가 BAR 크기 탐색(sizing) 시 사용 */
	dw_pcie_ep_writel_dbi2(ep, func_no, reg, lower_32_bits(size - 1));
	/* NVMe: BAR 레지스터에 flags 기록; NVMe 호스트가 메모리/64-bit 등 특성 인식 */
	dw_pcie_ep_writel_dbi(ep, func_no, reg, flags);

	/* NVMe: 64-bit BAR 상위 mask/레지스터 설정; NVMe 64-bit BAR(예: BAR0/1) 처리 */
	if (flags & PCI_BASE_ADDRESS_MEM_TYPE_64) {
		dw_pcie_ep_writel_dbi2(ep, func_no, reg + 4, upper_32_bits(size - 1));
		dw_pcie_ep_writel_dbi(ep, func_no, reg + 4, 0);
	}

	/* NVMe: RO 레지스터 쓰기 다시 잠금 */
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static enum pci_epc_bar_type dw_pcie_ep_get_bar_type(struct dw_pcie_ep *ep,
						     enum pci_barno bar)
{
	const struct pci_epc_features *epc_features;

	/* NVMe: 플랫폼별 get_features 콜백이 없으면 기본 programmable 반환; NVMe 호스트가 일반 BAR 탐색 */
	if (!ep->ops->get_features)
		return BAR_PROGRAMMABLE;

	/* NVMe: 플랫폼별 EPC feature 획득; NVMe BAR별 동작(고정/프로그래머블/리사이즈/예약) 결정 */
	epc_features = ep->ops->get_features(ep);

	/* NVMe: 지정 BAR의 타입 반환; NVMe BAR 초기화 방식 선택 */
	return epc_features->bar[bar].type;
}

static int dw_pcie_ep_set_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			      struct pci_epf_bar *epf_bar)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	enum pci_barno bar = epf_bar->barno;
	size_t size = epf_bar->size;
	enum pci_epc_bar_type bar_type;
	int flags = epf_bar->flags;
	int ret, type;

	/* NVMe: function 객체 미존재 시 BAR 설정 불가 */
	if (!ep_func)
		return -EINVAL;

	/*
	 * DWC does not allow BAR pairs to overlap, e.g. you cannot combine BARs
	 * 1 and 2 to form a 64-bit BAR.
	 */
	/* NVMe: 64-bit BAR은 짝수 번호에서 시작; NVMe BAR0/2/4만 64-bit 가능, BAR1/3/5는 불가 */
	if ((flags & PCI_BASE_ADDRESS_MEM_TYPE_64) && (bar & 1))
		return -EINVAL;

	/*
	 * Certain EPF drivers dynamically change the physical address of a BAR
	 * (i.e. they call set_bar() twice, without ever calling clear_bar(), as
	 * calling clear_bar() would clear the BAR's PCI address assigned by the
	 * host).
	 */
	/* NVMe: 이 BAR이 이미 설정된 경우 동적 재설정 경로; NVMe 호스트가 할당한 BAR 주소 보존 */
	if (ep_func->epf_bar[bar]) {
		/*
		 * We can only dynamically change a BAR if the new BAR size and
		 * BAR flags do not differ from the existing configuration.
		 *
		 * Note: this safety check only works when the caller uses
		 * a new struct pci_epf_bar in the second set_bar() call.
		 * If the same instance is updated in place and passed in,
		 * we cannot reliably detect invalid barno/size/flags
		 * changes here.
		 */
	/* NVMe: 동적 변경은 size/flags/barno가 동일할 때만 허용; NVMe BAR 특성 변경 시 재열거 필요 */
		if (ep_func->epf_bar[bar]->barno != bar ||
		    ep_func->epf_bar[bar]->size != size ||
		    ep_func->epf_bar[bar]->flags != flags)
			return -EINVAL;

		/*
		 * When dynamically changing a BAR, tear down any existing
		 * mappings before re-programming. This is redundant when
		 * both the old and new mappings are BAR Match Mode, but
		 * required to handle in-place updates and match-mode
		 * changes reliably.
		 */
	/* NVMe: 기존 inbound 매핑 해제 후 재구성; NVMe BAR 물리 주소 변경 시 stale mapping 제거 */
		dw_pcie_ep_clear_ib_maps(ep, func_no, bar);

		/*
		 * When dynamically changing a BAR, skip writing the BAR reg, as
		 * that would clear the BAR's PCI address assigned by the host.
		 */
	/* NVMe: BAR 레지스터는 다시 쓰지 않고 ATU만 재설정; NVMe 호스트가 할당한 MMIO 주소 유지 */
		goto config_atu;
	} else {
		/*
		 * Subrange mapping is an update-only operation.  The BAR
		 * must have been configured once without submaps so that
		 * subsequent set_bar() calls can update inbound mappings
		 * without touching the BAR register (and clobbering the
		 * host-assigned address).
		 */
	/* NVMe: submap은 기존 BAR 설정 후 update-only; NVMe BAR 미초기화 상태에서 submap 요청 거부 */
		if (epf_bar->num_submap)
			return -EINVAL;
	}

	/* NVMe: BAR 타입 결정; NVMe BAR별 초기화 로직 분기 */
	bar_type = dw_pcie_ep_get_bar_type(ep, bar);
	switch (bar_type) {
	case BAR_FIXED:
		/*
		 * There is no need to write a BAR mask for a fixed BAR (except
		 * to write 1 to the LSB of the BAR mask register, to enable the
		 * BAR). Write the BAR mask regardless. (The fixed bits in the
		 * BAR mask register will be read-only anyway.)
		 */
	/* NVMe: fixed BAR도 mask 레지스터 쓰기(실제로는 enable 비트만 유효); NVMe 호스트 BAR 탐색 */
		fallthrough;
	case BAR_PROGRAMMABLE:
	/* NVMe: programmable BAR 설정; NVMe 호스트가 BAR 크기/주소 할당 가능 */
		ret = dw_pcie_ep_set_bar_programmable(ep, func_no, epf_bar);
		break;
	case BAR_RESIZABLE:
	/* NVMe: Resizable BAR 설정; NVMe 호스트가 BAR 크기 조정(Resizable BAR capability) */
		ret = dw_pcie_ep_set_bar_resizable(ep, func_no, epf_bar);
		break;
	default:
	/* NVMe: 알 수 없는 BAR 타입; NVMe 열거 실패 가능 */
		ret = -EINVAL;
		dev_err(pci->dev, "Invalid BAR type\n");
		break;
	}

	/* NVMe: BAR 레지스터 기록 실패 시 리턴; NVMe 호스트가 BAR를 인식하지 못함 */
	if (ret)
		return ret;

config_atu:
	/* NVMe: BAR flags에 따라 MEM/IO ATU 타입 선택; NVMe는 메모리 BAR 사용 */
	if (!(flags & PCI_BASE_ADDRESS_SPACE))
		type = PCIE_ATU_TYPE_MEM;
	else
		type = PCIE_ATU_TYPE_IO;

	/* NVMe: submap 여부에 따라 Address Match Mode 또는 BAR Match Mode iATU 설정 */
	if (epf_bar->num_submap)
		ret = dw_pcie_ep_ib_atu_addr(ep, func_no, type, epf_bar);
	else
		ret = dw_pcie_ep_ib_atu_bar(ep, func_no, type,
					    epf_bar->phys_addr, bar, size);

	/* NVMe: iATU 설정 실패 시 리턴; NVMe 호스트의 MMIO 접근이 EP 메모리에 도달하지 못함 */
	if (ret)
		return ret;

	/* NVMe: BAR 설정 완료 표시; 이후 동적 변경/해제 시 참조 */
	ep_func->epf_bar[bar] = epf_bar;

	return 0;
}

static int dw_pcie_find_index(struct dw_pcie_ep *ep, phys_addr_t addr,
			      u32 *atu_index)
{
	u32 index;
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* NVMe: 사용 중인 outbound ATU 윈도우를 순회하며 parent_bus_addr가 일치하는지 검색 */
	for_each_set_bit(index, ep->ob_window_map, pci->num_ob_windows) {
	/* NVMe: 주소 불일치면 스킵; NVMe DMA/MSI outbound 매핑 해제 시 정확한 윈도우 탐색 */
		if (ep->outbound_addr[index] != addr)
			continue;
	/* NVMe: 일치하는 ATU 인덱스 반환; 이후 dw_pcie_disable_atu로 해제 */
		*atu_index = index;
		return 0;
	}

	return -EINVAL;
}

static u64 dw_pcie_ep_align_addr(struct pci_epc *epc, u64 pci_addr,
				 size_t *pci_size, size_t *offset)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	/* NVMe: pci_addr를 page/region 크기로 정렬; NVMe MSI/MSI-X 메모리 매핑 정렬 요구 */
	u64 mask = pci->region_align - 1;
	size_t ofst = pci_addr & mask;

	*pci_size = ALIGN(ofst + *pci_size, epc->mem->window.page_size);
	/* NVMe: 원래 주소 내 오프셋 반환; ATU 베이스 + offset으로 실제 주소 복원 */
	*offset = ofst;

	/* NVMe: 정렬된 베이스 주소 반환; IOMMU/DMA page boundary 충족 */
	return pci_addr & ~mask;
}

static void dw_pcie_ep_unmap_addr(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				  phys_addr_t addr)
{
	int ret;
	u32 atu_index;
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* NVMe: parent_bus_offset을 보정하여 ATU 윈도우 인덱스 검색; NVMe DMA 매핑 해제 */
	ret = dw_pcie_find_index(ep, addr - pci->parent_bus_offset,
				 &atu_index);
	/* NVMe: 해당 주소로 매핑된 outbound 윈도우가 없으면 조용히 리턴 */
	if (ret < 0)
		return;

	/* NVMe: outbound 주소 배열에서 제거; 재사용 시 중복 매칭 방지 */
	ep->outbound_addr[atu_index] = 0;
	/* NVMe: outbound ATU 윈도우 비활성화; NVMe EP의 DMA 트랜잭션 차단 */
	dw_pcie_disable_atu(pci, PCIE_ATU_REGION_DIR_OB, atu_index);
	/* NVMe: outbound 윈도우 맵에서 해제; 다른 DMA/MSI 매핑에 재할당 */
	clear_bit(atu_index, ep->ob_window_map);
}

static int dw_pcie_ep_map_addr(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			       phys_addr_t addr, u64 pci_addr, size_t size)
{
	int ret;
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dw_pcie_ob_atu_cfg atu = { 0 };

	/* NVMe: function 번호 설정; 다중 function NVMe EP의 DMA/MSI 구분 */
	atu.func_no = func_no;
	/* NVMe: 메모리 타입 outbound ATU; NVMe DMA/MSI 모두 메모리 트랜잭션 */
	atu.type = PCIE_ATU_TYPE_MEM;
	/* NVMe: EP 측 물리 주소에서 parent bus offset 보정; 호스트 PCI 주소 공간으로 변환 */
	atu.parent_bus_addr = addr - pci->parent_bus_offset;
	/* NVMe: 목표 PCI 버스 주소; NVMe 호스트 메모리 주소(또는 MSI target) */
	atu.pci_addr = pci_addr;
	/* NVMe: 매핑 크기; NVMe DMA 버퍼/MSI payload 크기 */
	atu.size = size;
	/* NVMe: outbound ATU 프로그래밍; EP에서 pci_addr로의 메모리 트랜잭션 가능케 함 */
	ret = dw_pcie_ep_outbound_atu(ep, &atu);
	if (ret) {
		dev_err(pci->dev, "Failed to enable address\n");
		return ret;
	}

	return 0;
}

static int dw_pcie_ep_get_msi(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie_ep_func *ep_func;
	u32 val, reg;

	/* NVMe: function 객체 및 MSI capability 존재 확인; NVMe 호스트가 MSI를 사용하는지 확인 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	if (!ep_func || !ep_func->msi_cap)
		return -EINVAL;

	/* NVMe: MSI capability의 Message Control 레지스터 오프셋 */
	reg = ep_func->msi_cap + PCI_MSI_FLAGS;
	/* NVMe: MSI enable 및 queue size 읽기; NVMe 호스트가 pci_enable_msi()로 설정한 값 */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* NVMe: MSI가 enable되지 않았으면 사용 불가; NVMe는 MSI/MSI-X enable 후에만 인터럽트 처리 */
	if (!(val & PCI_MSI_FLAGS_ENABLE))
		return -EINVAL;

	/* NVMe: Queue Size 필드 추출; NVMe 호스트가 요청한 MSI vector 개수의 로그 */
	val = FIELD_GET(PCI_MSI_FLAGS_QSIZE, val);

	/* NVMe: 실제 vector 개수 반환 (2^QSIZE); NVMe admin/io queue별 vector 할당에 사용 */
	return 1 << val;
}

static int dw_pcie_ep_set_msi(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			      u8 nr_irqs)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dw_pcie_ep_func *ep_func;
	u8 mmc = order_base_2(nr_irqs);
	u32 val, reg;

	/* NVMe: function 및 MSI capability 확인; NVMe 호스트가 pci_enable_msi() 호출 전 설정 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	if (!ep_func || !ep_func->msi_cap)
		return -EINVAL;

	/* NVMe: MSI Message Control 레지스터 */
	reg = ep_func->msi_cap + PCI_MSI_FLAGS;
	/* NVMe: 현재 control 값 읽기; NVMe 호스트가 설정한 다른 비트 보존 */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* NVMe: Multiple Message Capable 마스크 클리어 */
	val &= ~PCI_MSI_FLAGS_QMASK;
	/* NVMe: 요청된 vector 수의 로그를 MMC 필드에 기록; NVMe는 보통 1~N개 vector 요청 */
	val |= FIELD_PREP(PCI_MSI_FLAGS_QMASK, mmc);
	dw_pcie_dbi_ro_wr_en(pci);
	/* NVMe: RO 레지스터 쓰기 잠금 해제 후 control 갱신 */
	dw_pcie_ep_writew_dbi(ep, func_no, reg, val);
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static int dw_pcie_ep_get_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie_ep_func *ep_func;
	u32 val, reg;

	/* NVMe: function 및 MSI-X capability 확인; NVMe 호스트가 MSI-X를 선호하는 경우 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	/* NVMe: MSI-X Message Control 레지스터 */
	reg = ep_func->msix_cap + PCI_MSIX_FLAGS;
	/* NVMe: MSI-X enable 및 table size 읽기; NVMe nvme_reset_work에서 MSI-X 설정 */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* NVMe: MSI-X가 enable되지 않았으면 사용 불가; NVMe 드라이버는 pci_enable_msix_range() 호출 */
	if (!(val & PCI_MSIX_FLAGS_ENABLE))
		return -EINVAL;

	/* NVMe: Table Size 마스크 적용 (N-1 encoding); NVMe queue 수만큼 vector 필요 */
	val &= PCI_MSIX_FLAGS_QSIZE;

	/* NVMe: 실제 MSI-X vector 개수 반환 (N); NVMe IO queue당 독립 vector 매핑에 사용 */
	return val + 1;
}

static int dw_pcie_ep_set_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			       u16 nr_irqs, enum pci_barno bir, u32 offset)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dw_pcie_ep_func *ep_func;
	u32 val, reg;

	/* NVMe: function 및 MSI-X capability 확인; NVMe 호스트가 MSI-X table 위치 설정 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	/* NVMe: MSI-X capability 레지스터 쓰기 위해 RO 잠금 해제 */
	dw_pcie_dbi_ro_wr_en(pci);

	reg = ep_func->msix_cap + PCI_MSIX_FLAGS;
	/* NVMe: MSI-X Message Control: Table Size를 N-1로 기록; NVMe 호스트가 요청한 vector 수 반영 */
	val = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	val &= ~PCI_MSIX_FLAGS_QSIZE;
	val |= nr_irqs - 1; /* encoded as N-1 */
	dw_pcie_ep_writew_dbi(ep, func_no, reg, val);

	reg = ep_func->msix_cap + PCI_MSIX_TABLE;
	/* NVMe: MSI-X Table offset/BIR 설정; NVMe 호스트가 BAR 낸부 table 위치 인식 */
	val = offset | bir;
	dw_pcie_ep_writel_dbi(ep, func_no, reg, val);

	reg = ep_func->msix_cap + PCI_MSIX_PBA;
	/* NVMe: MSI-X PBA offset/BIR 설정; NVMe 호스트가 pending bit array 위치 인식 */
	val = (offset + (nr_irqs * PCI_MSIX_ENTRY_SIZE)) | bir;
	dw_pcie_ep_writel_dbi(ep, func_no, reg, val);

	/* NVMe: RO 레지스터 쓰기 다시 잠금 */
	dw_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static int dw_pcie_ep_raise_irq(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				unsigned int type, u16 interrupt_num)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);

	/* NVMe: 플랫폼별 raise_irq 콜백이 없으면 인터럽트 발생 불가; NVMe 호스트는 인터럽트 수신 못 함 */
	if (!ep->ops->raise_irq)
		return -EINVAL;

	/* NVMe: 플랫폼별 인터럽트 발생 함수 호출; NVMe 완료 큐 인터럽트를 호스트로 전달 */
	return ep->ops->raise_irq(ep, func_no, type, interrupt_num);
}

static void dw_pcie_ep_stop(struct pci_epc *epc)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/*
	 * Tear down the dedicated outbound window used for MSI
	 * generation. This avoids leaking an iATU window across
	 * endpoint stop/start cycles.
	 */
	/* NVMe: MSI 생성용 전용 outbound iATU 매핑이 남아 있으면 해제; NVMe 호스트로의 MSI 누수 방지 */
	if (ep->msi_iatu_mapped) {
		dw_pcie_ep_unmap_addr(epc, 0, 0, ep->msi_mem_phys);
	/* NVMe: msi_mem_phys로 매핑된 outbound ATU 해제; NVMe MSI 전송 중단 */
		ep->msi_iatu_mapped = false;
	/* NVMe: 매핑 상태 플래그 클리어; 다음 start 시 재설정 가능 */
	}

	/* NVMe: PCIe link down; NVMe 호스트는 link loss 감지 후 장치 제거/재열거 */
	dw_pcie_stop_link(pci);
}

static int dw_pcie_ep_start(struct pci_epc *epc)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* NVMe: PCIe link up; NVMe 호스트가 장치를 다시 열거하고 nvme_reset_work 수행 */
	return dw_pcie_start_link(pci);
}

static const struct pci_epc_features*
dw_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	struct dw_pcie_ep *ep = epc_get_drvdata(epc);

	/* NVMe: 플랫폼별 get_features 콜백이 없으면 NULL; NVMe EPF는 기본 BAR/MSI 특성 사용 */
	if (!ep->ops->get_features)
		return NULL;

	/* NVMe: 플랫폼별 EPC feature 반환; NVMe EP function이 지원하는 BAR/MSI/MSI-X 특성 */
	return ep->ops->get_features(ep);
}

static const struct pci_epc_ops epc_ops = {
	/* NVMe: EPF header(Vendor/Device/Class ID 등) 쓰기; NVMe 호스트가 pci_device_id로 매칭 */
	.write_header		= dw_pcie_ep_write_header,
	/* NVMe: BAR 설정; NVMe 호스트가 MMIO BAR를 할당/매핑할 수 있도록 iATU 구성 */
	.set_bar		= dw_pcie_ep_set_bar,
	/* NVMe: BAR 클리어; NVMe 장치 제거 시 BAR/iATU 해제 */
	.clear_bar		= dw_pcie_ep_clear_bar,
	/* NVMe: 주소 정렬; NVMe DMA/MSI 메모리 매핑의 page alignment 계산 */
	.align_addr		= dw_pcie_ep_align_addr,
	/* NVMe: EP->호스트 메모리 outbound ATU 매핑; NVMe DMA/MSI 트랜잭션 경로 설정 */
	.map_addr		= dw_pcie_ep_map_addr,
	/* NVMe: outbound ATU 매핑 해제; NVMe DMA/MSI 종료 시 리소스 정리 */
	.unmap_addr		= dw_pcie_ep_unmap_addr,
	/* NVMe: MSI vector 수 설정; NVMe 호스트의 pci_enable_msi()에 대응 */
	.set_msi		= dw_pcie_ep_set_msi,
	/* NVMe: 설정된 MSI vector 수 조회; NVMe 인터럽트 처리기 등록 시 참조 */
	.get_msi		= dw_pcie_ep_get_msi,
	/* NVMe: MSI-X vector 수/테이블 설정; NVMe 호스트의 pci_enable_msix_range()에 대응 */
	.set_msix		= dw_pcie_ep_set_msix,
	/* NVMe: 설정된 MSI-X vector 수 조회; NVMe queue당 vector 할당 시 사용 */
	.get_msix		= dw_pcie_ep_get_msix,
	/* NVMe: 호스트로 인터럽트 발생; NVMe 완료 큐 인터럽트를 호스트 CPU로 전송 */
	.raise_irq		= dw_pcie_ep_raise_irq,
	/* NVMe: PCIe link 시작; NVMe 호스트가 장치를 다시 발견/열거 */
	.start			= dw_pcie_ep_start,
	/* NVMe: PCIe link 정지 및 MSI ATU 해제; NVMe 호스트에서 장치 제거/사용 중지 */
	.stop			= dw_pcie_ep_stop,
	/* NVMe: EPC feature 조회; NVMe EP가 지원하는 BAR/MSI/MSI-X capability 반환 */
	.get_features		= dw_pcie_ep_get_features,
};

/**
 * dw_pcie_ep_raise_intx_irq - Raise INTx IRQ to the host
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint
 *
 * Return: 0 if success, errno otherwise.
 */
int dw_pcie_ep_raise_intx_irq(struct dw_pcie_ep *ep, u8 func_no)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct device *dev = pci->dev;

	/* NVMe: DesignWare EP는 legacy INTx 인터럽트 발생을 지원하지 않음; NVMe는 MSI/MSI-X 사용 */
	dev_err(dev, "EP cannot raise INTX IRQs\n");

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_raise_intx_irq);

/**
 * dw_pcie_ep_raise_msi_irq - Raise MSI IRQ to the host
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint
 * @interrupt_num: Interrupt number to be raised
 *
 * Return: 0 if success, errno otherwise.
 */
int dw_pcie_ep_raise_msi_irq(struct dw_pcie_ep *ep, u8 func_no,
			     u8 interrupt_num)
{
	u32 msg_addr_lower, msg_addr_upper, reg;
	struct dw_pcie_ep_func *ep_func;
	struct pci_epc *epc = ep->epc;
	size_t map_size = sizeof(u32);
	size_t offset;
	u16 msg_ctrl, msg_data;
	bool has_upper;
	u64 msg_addr;
	int ret;

	/* NVMe: function 및 MSI capability 확인; NVMe 호스트가 MSI capability를 노출했는지 검증 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	if (!ep_func || !ep_func->msi_cap)
		return -EINVAL;

	/* Raise MSI per the PCI Local Bus Specification Revision 3.0, 6.8.1. */
	/* NVMe: MSI Message Control 읽기; NVMe 호스트가 pci_enable_msi()로 설정한 값 */
	reg = ep_func->msi_cap + PCI_MSI_FLAGS;
	msg_ctrl = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	/* NVMe: 64-bit MSI 지원 여부; NVMe 호스트가 64-bit address를 사용하는지 확인 */
	has_upper = !!(msg_ctrl & PCI_MSI_FLAGS_64BIT);
	reg = ep_func->msi_cap + PCI_MSI_ADDRESS_LO;
	/* NVMe: MSI target address 하위 32-bit 읽기; NVMe 호스트가 할당한 메시지 주소 */
	msg_addr_lower = dw_pcie_ep_readl_dbi(ep, func_no, reg);
	if (has_upper) {
	/* NVMe: 64-bit MSI 상위 주소 읽기; 4GB 이상 NVMe 호스트 메모리 영역 */
		reg = ep_func->msi_cap + PCI_MSI_ADDRESS_HI;
		msg_addr_upper = dw_pcie_ep_readl_dbi(ep, func_no, reg);
	/* NVMe: 64-bit MSI data register 읽기; NVMe vector별 data 값 */
		reg = ep_func->msi_cap + PCI_MSI_DATA_64;
		msg_data = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	} else {
	/* NVMe: 32-bit MSI는 상위 주소 0; NVMe 호스트 메모리 하위 4GB */
		msg_addr_upper = 0;
		reg = ep_func->msi_cap + PCI_MSI_DATA_32;
	/* NVMe: 32-bit MSI data register 읽기 */
		msg_data = dw_pcie_ep_readw_dbi(ep, func_no, reg);
	}
	/* NVMe: 하위/상위 주소를 64-bit MSI target address로 조합; NVMe 호스트 CPU의 APIC/IRQ 칩 영역 */
	msg_addr = ((u64)msg_addr_upper) << 32 | msg_addr_lower;

	/* NVMe: MSI target 주소를 region/page 정렬; IOMMU/DMA alignment 및 offset 계산 */
	msg_addr = dw_pcie_ep_align_addr(epc, msg_addr, &map_size, &offset);

	/*
	 * Program the outbound iATU once and keep it enabled.
	 *
	 * The spec warns that updating iATU registers while there are
	 * operations in flight on the AXI bridge interface is not
	 * supported, so we avoid reprogramming the region on every MSI,
	 * specifically unmapping immediately after writel().
	 */
	/* NVMe: MSI target 주소나 크기가 바뀌면 기존 outbound iATU 해제; NVMe 호스트가 MSI 주소를 재할당한 경우 */
	if (ep->msi_iatu_mapped && (ep->msi_msg_addr != msg_addr ||
				    ep->msi_map_size != map_size)) {
		/*
		 * The host changed the MSI target address or the required
		 * mapping size changed. Reprogramming the iATU when there are
		 * operations in flight is unsafe on this controller. However,
		 * there is no unified way to check if we have operations in
		 * flight, thus we don't know if we should WARN() or not.
		 */
	/* NVMe: 기존 MSI outbound ATU 해제; NVMe 호스트의 새 MSI target으로 재매핑 준비 */
		dw_pcie_ep_unmap_addr(epc, func_no, 0, ep->msi_mem_phys);
	/* NVMe: 매핑 플래그 클리어; 새 주소로 재설정 유도 */
		ep->msi_iatu_mapped = false;
	}

	/* NVMe: 아직 매핑되지 않았으면 새로 설정; NVMe 호스트가 MSI를 활성화한 후 첫 인터럽트 */
	if (!ep->msi_iatu_mapped) {
	/* NVMe: msi_mem_phys를 통해 EP 낸부 메모리를 MSI target 주소로 outbound ATU 매핑 */
		ret = dw_pcie_ep_map_addr(epc, func_no, 0,
					  ep->msi_mem_phys, msg_addr,
					  map_size);
		if (ret)
			return ret;

	/* NVMe: 매핑 완료 및 현재 MSI 주소/크기 저장; 이후 변경 시 비교 기준 */
		ep->msi_iatu_mapped = true;
		ep->msi_msg_addr = msg_addr;
		ep->msi_map_size = map_size;
	}

	/* NVMe: MSI data에 vector 번호를 OR하여 msi_mem에 쓰기; NVMe 호스트 CPU가 MSI 수신 */
	writel(msg_data | (interrupt_num - 1), ep->msi_mem + offset);

	return 0;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_raise_msi_irq);

/**
 * dw_pcie_ep_raise_msix_irq_doorbell - Raise MSI-X to the host using Doorbell
 *					method
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint device
 * @interrupt_num: Interrupt number to be raised
 *
 * Return: 0 if success, errno otherwise.
 */
int dw_pcie_ep_raise_msix_irq_doorbell(struct dw_pcie_ep *ep, u8 func_no,
				       u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dw_pcie_ep_func *ep_func;
	u32 msg_data;

	/* NVMe: function 및 MSI-X capability 확인; NVMe 호스트가 MSI-X capability를 노출했는지 검증 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	/* NVMe: doorbell 데이터 구성: PF 번호 + vector 인덱스; NVMe IO queue completion 인터럽트 대응 */
	msg_data = (func_no << PCIE_MSIX_DOORBELL_PF_SHIFT) |
		   (interrupt_num - 1);

	/* NVMe: DesignWare 전용 MSI-X Doorbell 레지스터에 쓰기; NVMe 호스트로 MSI-X 메시지 전송 */
	dw_pcie_writel_dbi(pci, PCIE_MSIX_DOORBELL, msg_data);

	return 0;
}

/**
 * dw_pcie_ep_raise_msix_irq - Raise MSI-X to the host
 * @ep: DWC EP device
 * @func_no: Function number of the endpoint device
 * @interrupt_num: Interrupt number to be raised
 *
 * Return: 0 if success, errno otherwise.
 */
int dw_pcie_ep_raise_msix_irq(struct dw_pcie_ep *ep, u8 func_no,
			      u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct pci_epf_msix_tbl *msix_tbl;
	struct dw_pcie_ep_func *ep_func;
	struct pci_epc *epc = ep->epc;
	size_t map_size = sizeof(u32);
	size_t offset;
	u32 reg, msg_data, vec_ctrl;
	u32 tbl_offset;
	u64 msg_addr;
	int ret;
	u8 bir;

	/* NVMe: function 및 MSI-X capability 확인; NVMe 호스트가 MSI-X capability를 노출했는지 검증 */
	ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	if (!ep_func || !ep_func->msix_cap)
		return -EINVAL;

	/* NVMe: MSI-X Table 레지스터에서 BIR과 offset 추출; NVMe BAR 낸부 MSI-X table 위치 */
	reg = ep_func->msix_cap + PCI_MSIX_TABLE;
	tbl_offset = dw_pcie_ep_readl_dbi(ep, func_no, reg);
	/* NVMe: BAR Indicator Register 추출; NVMe BAR0/1 등 MSI-X table이 있는 BAR 식별 */
	bir = FIELD_GET(PCI_MSIX_TABLE_BIR, tbl_offset);
	/* NVMe: MSI-X table의 BAR 내 offset; NVMe 호스트가 pci_iomap()으로 매핑한 영역 */
	tbl_offset &= PCI_MSIX_TABLE_OFFSET;

	/* NVMe: 해당 BAR의 가상 주소 + offset으로 MSI-X table 포인터 획득; NVMe vector table 접근 */
	msix_tbl = ep_func->epf_bar[bir]->addr + tbl_offset;
	/* NVMe: 지정 vector의 message address/data/control 읽기; NVMe 호스트가 설정한 MSI-X entry */
	msg_addr = msix_tbl[(interrupt_num - 1)].msg_addr;
	msg_data = msix_tbl[(interrupt_num - 1)].msg_data;
	vec_ctrl = msix_tbl[(interrupt_num - 1)].vector_ctrl;

	/* NVMe: vector control의 mask bit가 설정되어 있으면 인터럽트 억제; NVMe 호스트가 vector를 mask한 상태 */
	if (vec_ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT) {
		dev_dbg(pci->dev, "MSI-X entry ctrl set\n");
		return -EPERM;
	}

	/* NVMe: MSI-X target 주소 정렬; IOMMU/DMA page alignment 및 offset 계산 */
	msg_addr = dw_pcie_ep_align_addr(epc, msg_addr, &map_size, &offset);
	/* NVMe: msi_mem_phys를 MSI-X target 주소로 outbound ATU 일시 매핑; per-interrupt 매핑 방식 */
	ret = dw_pcie_ep_map_addr(epc, func_no, 0, ep->msi_mem_phys, msg_addr,
				  map_size);
	if (ret)
		return ret;

	/* NVMe: MSI-X data를 쓰어 호스트로 인터럽트 전송; NVMe IO queue completion 알림 */
	writel(msg_data, ep->msi_mem + offset);

	/* NVMe: posted write가 PCIe로 실제 발행되었는지 확인; NVMe 인터럽트 손실 방지 */
	/* flush posted write before unmap */
	readl(ep->msi_mem + offset);

	/* NVMe: per-interrupt 방식이므로 전송 후 즉시 outbound ATU 해제; 다른 vector/주소 재사용 가능 */
	dw_pcie_ep_unmap_addr(epc, func_no, 0, ep->msi_mem_phys);

	return 0;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_raise_msix_irq);

/**
 * dw_pcie_ep_cleanup - Cleanup DWC EP resources after fundamental reset
 * @ep: DWC EP device
 *
 * Cleans up the DWC EP specific resources like eDMA etc... after fundamental
 * reset like PERST#. Note that this API is only applicable for drivers
 * supporting PERST# or any other methods of fundamental reset.
 */
void dw_pcie_ep_cleanup(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	/* NVMe: debugfs 정리; NVMe EP 디버깅 인터페이스 제거, 호스트/드라이버 개발자 도구 */
	dwc_pcie_debugfs_deinit(pci);
	/* NVMe: eDMA 제거; NVMe EP 낸부 DMA 엔진 정리, NVMe 호스트의 DMA 요청과 무관 */
	dw_pcie_edma_remove(pci);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_cleanup);

/**
 * dw_pcie_ep_deinit - Deinitialize the endpoint device
 * @ep: DWC EP device
 *
 * Deinitialize the endpoint device. EPC device is not destroyed since that will
 * be taken care by Devres.
 */
void dw_pcie_ep_deinit(struct dw_pcie_ep *ep)
{
	struct pci_epc *epc = ep->epc;

	/* NVMe: eDMA/debugfs 등 EP 리소스 정리; NVMe 호스트에서 장치 제거 시 호출 */
	dw_pcie_ep_cleanup(ep);

	/* NVMe: MSI/MSI-X 전용 메모리 영역 해제; NVMe 호스트로의 인터럽트 전송 불가 */
	pci_epc_mem_free_addr(epc, ep->msi_mem_phys, ep->msi_mem,
			      epc->mem->window.page_size);

	/* NVMe: EPC 메모리 관리 종료; NVMe BAR/DMA 물리 주소 공간 해제 */
	pci_epc_mem_exit(epc);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_deinit);

static void dw_pcie_ep_init_rebar_registers(struct dw_pcie_ep *ep, u8 func_no)
{
	/* NVMe: function 객체 미존재 시 REBAR 초기화 불가 */
	struct dw_pcie_ep_func *ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
	unsigned int offset, nbars;
	enum pci_barno bar;
	u32 reg, i, val;

	if (!ep_func)
		return;

	/* NVMe: Resizable BAR extended capability 위치 탐색; NVMe 호스트가 BAR 크기 협상 시 사용 */
	offset = dw_pcie_ep_find_ext_capability(ep, func_no, PCI_EXT_CAP_ID_REBAR);

	if (offset) {
		reg = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
	/* NVMe: REBAR control에서 지원하는 BAR 개수 획득 */
		nbars = FIELD_GET(PCI_REBAR_CTRL_NBAR_MASK, reg);

		/*
		 * PCIe r6.0, sec 7.8.6.2 require us to support at least one
		 * size in the range from 1 MB to 512 GB. Advertise support
		 * for 1 MB BAR size only.
		 *
		 * For a BAR that has been configured via dw_pcie_ep_set_bar(),
		 * advertise support for only that size instead.
		 */
	/* NVMe: 각 REBAR capability에 지원 size 광고; NVMe 호스트가 Set Resizable BAR 수행 */
		for (i = 0; i < nbars; i++, offset += PCI_REBAR_CTRL) {
			/*
			 * While the RESBAR_CAP_REG_* fields are sticky, the
			 * RESBAR_CTRL_REG_BAR_SIZE field is non-sticky (it is
			 * sticky in certain versions of DWC PCIe, but not all).
			 *
			 * RESBAR_CTRL_REG_BAR_SIZE is updated automatically by
			 * the controller when RESBAR_CAP_REG is written, which
			 * is why RESBAR_CAP_REG is written here.
			 */
	/* NVMe: REBAR control에서 BAR 인덱스 추출; NVMe BAR별 size capability 설정 */
			val = dw_pcie_ep_readl_dbi(ep, func_no, offset + PCI_REBAR_CTRL);
			bar = FIELD_GET(PCI_REBAR_CTRL_BAR_IDX, val);
	/* NVMe: 이미 설정된 BAR이면 해당 size만 광고, 아니면 1MB 기본 광고 */
			if (ep_func->epf_bar[bar])
				pci_epc_bar_size_to_rebar_cap(ep_func->epf_bar[bar]->size, &val);
			else
				val = BIT(4);

	/* NVMe: REBAR CAP 레지스터에 지원 size 기록; NVMe 호스트가 BAR 크기 선택 시 참조 */
			dw_pcie_ep_writel_dbi(ep, func_no, offset + PCI_REBAR_CAP, val);
		}
	}
}

static void dw_pcie_ep_init_non_sticky_registers(struct dw_pcie *pci)
{
	struct dw_pcie_ep *ep = &pci->ep;
	u8 funcs = ep->epc->max_functions;
	u32 func0_lnkcap, lnkcap;
	u8 func_no, offset;

	/* NVMe: non-sticky 레지스터 쓰기 위해 RO 잠금 해제;fundamental reset 후 재초기화 */
	dw_pcie_dbi_ro_wr_en(pci);

	for (func_no = 0; func_no < funcs; func_no++)
	/* NVMe: 각 function의 Resizable BAR 레지스터 초기화; NVMe 호스트의 BAR size 협상 준비 */
		dw_pcie_ep_init_rebar_registers(ep, func_no);

	/* NVMe: DWC 컨트롤러 공통 셋업; link width/speed, AXI 등 NVMe 물리 계층 준비 */
	dw_pcie_setup(pci);

	/*
	 * PCIe r7.0, section 7.5.3.6 states that for multi-function
	 * endpoints, max link width and speed fields must report same
	 * values for all functions. However, dw_pcie_setup() programs
	 * these fields only for function 0. Hence, mirror these fields
	 * to all other functions as well.
	 */
	/* NVMe: 다중 function EP인 경우 function 0의 link capability를 다른 function에 복사; NVMe SR-IOV 등 */
	if (funcs > 1) {
	/* NVMe: function 0의 PCI Express capability 오프셋 탐색 */
		offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	/* NVMe: function 0의 LINKCAP 레지스터 읽기; max link width/speed 필드 추출 */
		func0_lnkcap = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);
		func0_lnkcap = FIELD_GET(PCI_EXP_LNKCAP_MLW |
					 PCI_EXP_LNKCAP_SLS, func0_lnkcap);

	/* NVMe: function 1..N의 LINKCAP을 function 0과 동일하게 설정; NVMe 호스트 lspci 일관성 */
		for (func_no = 1; func_no < funcs; func_no++) {
			offset = dw_pcie_ep_find_capability(ep, func_no,
							    PCI_CAP_ID_EXP);
			lnkcap = dw_pcie_ep_readl_dbi(ep, func_no,
						      offset + PCI_EXP_LNKCAP);
			FIELD_MODIFY(PCI_EXP_LNKCAP_MLW | PCI_EXP_LNKCAP_SLS,
				     &lnkcap, func0_lnkcap);
			dw_pcie_ep_writel_dbi(ep, func_no,
					      offset + PCI_EXP_LNKCAP, lnkcap);
		}
	}

	/* NVMe: non-sticky 레지스터 쓰기 다시 잠금; 호스트의 config write로 인한 변경 방지 */
	dw_pcie_dbi_ro_wr_dis(pci);
}

static void dw_pcie_ep_disable_bars(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	enum pci_epc_bar_type bar_type;
	enum pci_barno bar;

	/* NVMe: 모든 표준 BAR에 대해 초기 상태를 disabled로 설정; NVMe 호스트 열거 전 BAR 클리어 */
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		bar_type = dw_pcie_ep_get_bar_type(ep, bar);

		/*
		 * Reserved BARs should not get disabled by default. All other
		 * BAR types are disabled by default.
		 *
		 * This is in line with the current EPC core design, where all
		 * BARs are disabled by default, and then the EPF driver enables
		 * the BARs it wishes to use.
		 */
	/* NVMe: RESERVED가 아닌 BAR만 리셋; NVMe EPF가 필요한 BAR만 이후 enable */
		if (bar_type != BAR_RESERVED)
			dw_pcie_ep_reset_bar(pci, bar);
	}
}

/**
 * dw_pcie_ep_init_registers - Initialize DWC EP specific registers
 * @ep: DWC EP device
 *
 * Initialize the registers (CSRs) specific to DWC EP. This API should be called
 * only when the endpoint receives an active refclk (either from host or
 * generated locally).
 */
int dw_pcie_ep_init_registers(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct dw_pcie_ep_func *ep_func;
	struct device *dev = pci->dev;
	struct pci_epc *epc = ep->epc;
	u32 ptm_cap_base, reg;
	u8 hdr_type;
	u8 func_no;
	void *addr;
	int ret;

	/* NVMe: header type 읽어 EP 모드인지 확인; NVMe 호스트가 볼 장치는 반드시 EP(0x00) */
	hdr_type = dw_pcie_readb_dbi(pci, PCI_HEADER_TYPE) &
		   PCI_HEADER_TYPE_MASK;
	if (hdr_type != PCI_HEADER_TYPE_NORMAL) {
		dev_err(pci->dev,
			"PCIe controller is not set to EP mode (hdr_type:0x%x)!\n",
			hdr_type);
		return -EIO;
	}

	/* NVMe: DWC IP 버전 감지; NVMe 호스트와의 PCIe 프로토콜 호환성 및 workaround 결정 */
	dw_pcie_version_detect(pci);

	/* NVMe: iATU 레지스터 위치/개수 감지; NVMe BAR/DMA/MSI 매핑에 필요한 윈도우 수 파악 */
	dw_pcie_iatu_detect(pci);

	/* NVMe: eDMA 엔진 감지; NVMe EP 낸부 DMA 기능 초기화 */
	ret = dw_pcie_edma_detect(pci);
	if (ret)
		return ret;

	ret = -ENOMEM;
	/* NVMe: inbound ATU 윈도우 사용 비트맵 할당; NVMe BAR/MMIO 매핑 관리 */
	if (!ep->ib_window_map) {
		ep->ib_window_map = devm_bitmap_zalloc(dev, pci->num_ib_windows,
						       GFP_KERNEL);
		if (!ep->ib_window_map)
			goto err_remove_edma;
	}

	/* NVMe: outbound ATU 윈도우 사용 비트맵 할당; NVMe DMA/MSI 트랜잭션 관리 */
	if (!ep->ob_window_map) {
		ep->ob_window_map = devm_bitmap_zalloc(dev, pci->num_ob_windows,
						       GFP_KERNEL);
		if (!ep->ob_window_map)
			goto err_remove_edma;
	}

	/* NVMe: outbound 윈도우당 parent bus 주소 배열 할당; DMA/MSI unmap 시 검색용 */
	if (!ep->outbound_addr) {
		addr = devm_kcalloc(dev, pci->num_ob_windows, sizeof(phys_addr_t),
				    GFP_KERNEL);
		if (!addr)
			goto err_remove_edma;
		ep->outbound_addr = addr;
	}

	/* NVMe: 최대 function 수만큼 dw_pcie_ep_func 객체 생성; NVMe 다중 function 지원 */
	for (func_no = 0; func_no < epc->max_functions; func_no++) {

	/* NVMe: 이미 생성된 function은 스킵; hotplug/linkdown 후 재초기화 시 중복 방지 */
		ep_func = dw_pcie_ep_get_func_from_ep(ep, func_no);
		if (ep_func)
			continue;

	/* NVMe: per-function 객체 할당; NVMe 각 function의 BAR/MSI/MSI-X 상태 저장 */
		ep_func = devm_kzalloc(dev, sizeof(*ep_func), GFP_KERNEL);
		if (!ep_func)
			goto err_remove_edma;

	/* NVMe: function 번호 및 MSI/MSI-X capability 위치 저장; NVMe 호스트의 capability 탐색 대응 */
		ep_func->func_no = func_no;
		ep_func->msi_cap = dw_pcie_ep_find_capability(ep, func_no,
							      PCI_CAP_ID_MSI);
		ep_func->msix_cap = dw_pcie_ep_find_capability(ep, func_no,
							       PCI_CAP_ID_MSIX);

	/* NVMe: function 리스트에 추가; 이후 dw_pcie_ep_get_func_from_ep()에서 검색 */
		list_add_tail(&ep_func->list, &ep->func_list);
	}

	/* NVMe: 플랫폼별 추가 초기화 콜백; NVMe EP 특화 register 설정 기회 */
	if (ep->ops->init)
		ep->ops->init(ep);

	/* NVMe: 모든 BAR 기본 disabled; NVMe EPF가 필요한 BAR만 이후 set_bar()로 활성화 */
	dw_pcie_ep_disable_bars(ep);

	/*
	 * PCIe r6.0, section 7.9.15 states that for endpoints that support
	 * PTM, this capability structure is required in exactly one
	 * function, which controls the PTM behavior of all PTM capable
	 * functions. This indicates the PTM capability structure
	 * represents controller-level registers rather than per-function
	 * registers.
	 *
	 * Therefore, PTM capability registers are configured using the
	 * standard DBI accessors, instead of func_no indexed per-function
	 * accessors.
	 */
	/* NVMe: PTM( Precision Time Measurement) extended capability 탐색; NVMe 호스트 시간 동기화 */
	ptm_cap_base = dw_pcie_find_ext_capability(pci, PCI_EXT_CAP_ID_PTM);

	/*
	 * PTM responder capability can be disabled only after disabling
	 * PTM root capability.
	 */
	/* NVMe: PTM capability가 있으면 root/responder 기능 비활성화; NVMe EP는 보통 PTM responder 불필요 */
	if (ptm_cap_base) {
	/* NVMe: PTM capability 쓰기 위해 RO 잠금 해제 */
		dw_pcie_dbi_ro_wr_en(pci);
		reg = dw_pcie_readl_dbi(pci, ptm_cap_base + PCI_PTM_CAP);
	/* NVMe: PTM Root capability 비활성화; NVMe 호스트가 PTM root 역할 수행 */
		reg &= ~PCI_PTM_CAP_ROOT;
		dw_pcie_writel_dbi(pci, ptm_cap_base + PCI_PTM_CAP, reg);

	/* NVMe: PTM Responder capability 및 granularity 클리어; NVMe EP는 PTM responder로 동작하지 않음 */
		reg = dw_pcie_readl_dbi(pci, ptm_cap_base + PCI_PTM_CAP);
		reg &= ~(PCI_PTM_CAP_RES | PCI_PTM_GRANULARITY_MASK);
		dw_pcie_writel_dbi(pci, ptm_cap_base + PCI_PTM_CAP, reg);
	/* NVMe: RO 레지스터 쓰기 다시 잠금 */
		dw_pcie_dbi_ro_wr_dis(pci);
	}

	/* NVMe: non-sticky 레지스터 초기화; fundamental reset/linkdown 후 NVMe 호스트 열거 준비 */
	dw_pcie_ep_init_non_sticky_registers(pci);

	/* NVMe: debugfs 초기화; NVMe EP 동작 모니터링/디버깅 인터페이스 생성 */
	dwc_pcie_debugfs_init(pci, DW_PCIE_EP_TYPE);

	return 0;

	/* NVMe: 초기화 실패 시 eDMA 제거; NVMe EP 리소스 누수 방지 */
err_remove_edma:
	dw_pcie_edma_remove(pci);

	return ret;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_init_registers);

/**
 * dw_pcie_ep_linkup - Notify EPF drivers about Link Up event
 * @ep: DWC EP device
 */
void dw_pcie_ep_linkup(struct dw_pcie_ep *ep)
{
	struct pci_epc *epc = ep->epc;

	/* NVMe: EPF 드라이버들에게 link up 알림; NVMe 호스트가 장치를 열거하기 시작 */
	pci_epc_linkup(epc);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_linkup);

/**
 * dw_pcie_ep_linkdown - Notify EPF drivers about Link Down event
 * @ep: DWC EP device
 *
 * Non-sticky registers are also initialized before sending the notification to
 * the EPF drivers. This is needed since the registers need to be initialized
 * before the link comes back again.
 */
void dw_pcie_ep_linkdown(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct pci_epc *epc = ep->epc;

	/*
	 * Initialize the non-sticky DWC registers as they would've reset post
	 * Link Down. This is specifically needed for drivers not supporting
	 * PERST# as they have no way to reinitialize the registers before the
	 * link comes back again.
	 */
	/* NVMe: link down으로 리셋된 non-sticky 레지스터 재초기화; NVMe 호스트 재열거 시 정상 동작 보장 */
	dw_pcie_ep_init_non_sticky_registers(pci);

	/* NVMe: EPF 드라이버들에게 link down 알림; NVMe 호스트와의 연결 끊김 처리 */
	pci_epc_linkdown(epc);
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_linkdown);

static int dw_pcie_ep_get_resources(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct device *dev = pci->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *np = dev->of_node;
	struct pci_epc *epc = ep->epc;
	struct resource *res;
	int ret;

	/* NVMe: DesignWare PCIe 공통 리소스(DBI/apb/atu) 획득; NVMe EP register 접근 기반 */
	ret = dw_pcie_get_resources(pci);
	if (ret)
		return ret;

	/* NVMe: "addr_space" 메모리 리소스 획득; NVMe EP가 사용할 물리 주소 공간(호스트가 매핑할 BAR 영역) */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "addr_space");
	if (!res)
		return -EINVAL;

	/* NVMe: EP 물리 주소 공간의 시작과 크기 저장; NVMe BAR/DMA 물리 메모리 베이스 */
	ep->phys_base = res->start;
	ep->addr_size = resource_size(res);

	/*
	 * artpec6_pcie_cpu_addr_fixup() uses ep->phys_base, so call
	 * dw_pcie_parent_bus_offset() after setting ep->phys_base.
	 */
	/* NVMe: CPU 주소와 PCI 버스 주소 간 offset 계산; NVMe 호스트가 보는 주소와 EP 물리 주소 변환 */
	pci->parent_bus_offset = dw_pcie_parent_bus_offset(pci, "addr_space",
							   ep->phys_base);

	/* NVMe: DT에서 최대 function 수 읽기; NVMe 다중 function EP 지원 수준 결정 */
	ret = of_property_read_u8(np, "max-functions", &epc->max_functions);
	if (ret < 0)
	/* NVMe: DT에 없으면 기본 1 function; 일반 NVMe SSD 단일 function */
		epc->max_functions = 1;

	return 0;
}

/**
 * dw_pcie_ep_init - Initialize the endpoint device
 * @ep: DWC EP device
 *
 * Initialize the endpoint device. Allocate resources and create the EPC
 * device with the endpoint framework.
 *
 * Return: 0 if success, errno otherwise.
 */
int dw_pcie_ep_init(struct dw_pcie_ep *ep)
{
	int ret;
	struct pci_epc *epc;
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct device *dev = pci->dev;

	/* NVMe: function 리스트 초기화; NVMe 다중 function 관리 구조 준비 */
	INIT_LIST_HEAD(&ep->func_list);
	/* NVMe: MSI outbound ATU 매핑 상태 초기화; NVMe 호스트로의 첫 MSI 전송 전 상태 */
	ep->msi_iatu_mapped = false;
	/* NVMe: MSI target 주소/크기 초기화; 이후 변경 감지용 기준값 */
	ep->msi_msg_addr = 0;
	ep->msi_map_size = 0;

	/* NVMe: PCI Endpoint Controller 장치 생성; NVMe EPF 드라이버가 등록할 수 있는 EPC 객체 */
	epc = devm_pci_epc_create(dev, &epc_ops);
	if (IS_ERR(epc)) {
		dev_err(dev, "Failed to create epc device\n");
		return PTR_ERR(epc);
	}

	/* NVMe: EPC와 dw_pcie_ep 객체 연결; NVMe EPF 콜백에서 dw_pcie_ep 접근 */
	ep->epc = epc;
	epc_set_drvdata(epc, ep);

	/* NVMe: platform/DT 리소스 획득; NVMe EP의 메모리/function 정보 초기화 */
	ret = dw_pcie_ep_get_resources(ep);
	if (ret)
		return ret;

	/* NVMe: 플랫폼별 사전 초기화 콜백; NVMe EP 특화 setup 기회 */
	if (ep->ops->pre_init)
		ep->ops->pre_init(ep);

	/* NVMe: EPC 메모리 관리 초기화; NVMe BAR/DMA를 위한 물리 주소 윈도우 설정 */
	ret = pci_epc_mem_init(epc, ep->phys_base, ep->addr_size,
			       ep->page_size);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize address space\n");
		return ret;
	}

	/* NVMe: MSI/MSI-X 전송용 메모리 페이지 할당; NVMe 호스트로의 인터럽트 발생에 사용 */
	ep->msi_mem = pci_epc_mem_alloc_addr(epc, &ep->msi_mem_phys,
					     epc->mem->window.page_size);
	/* NVMe: MSI 메모리 할당 실패 시 EPC 메모리 관리 종료; NVMe 인터럽트 기능 사용 불가 */
	if (!ep->msi_mem) {
		ret = -ENOMEM;
		dev_err(dev, "Failed to reserve memory for MSI/MSI-X\n");
		goto err_exit_epc_mem;
	}

	return 0;

err_exit_epc_mem:
	/* NVMe: EPC 메모리 관리 종료; NVMe BAR/DMA 물리 주소 공간 해제 */
	pci_epc_mem_exit(epc);

	return ret;
}
EXPORT_SYMBOL_GPL(dw_pcie_ep_init);
