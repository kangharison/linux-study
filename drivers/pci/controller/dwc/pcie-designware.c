// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare PCIe host controller driver
 *
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 */

#include <linux/align.h>		/* PCI/NVMe: DMA descriptor, BAR, page alignment 처리 */
#include <linux/bitops.h>		/* PCI/NVMe: PCIe 링크/MSI/ATU 레지스터 비트 조작 */
#include <linux/clk.h>			/* PCI/NVMe: NVMe 장치 및 PCIe 링크 클럭 전원 관리 */
#include <linux/delay.h>		/* PCI/NVMe: PCIe 링크 트레이닝 및 NVMe 초기화 지연 */
#include <linux/dma/edma.h>		/* PCI/NVMe: 내장 eDMA, NVMe 데이터 이동 가속 가능 */
#include <linux/gpio/consumer.h>	/* PCI/NVMe: NVMe 장치 PERST# 리셋 GPIO 제어 */
#include <linux/ioport.h>		/* PCI/NVMe: PCIe I/O/MEM 리소스 등록, NVMe BAR 매핑에 사용 */
#include <linux/of.h>			/* PCI/NVMe: DT에서 PCIe RC/NVMe 속성 파싱 */
#include <linux/of_address.h>		/* PCI/NVMe: DT 'ranges', NVMe BAR 주소 공간 변환 */
#include <linux/pcie-dwc.h>		/* PCI/NVMe: DesignWare PCIe 코어 헤더 */
#include <linux/platform_device.h>	/* PCI/NVMe: 플랫폼 드라이버 바인딩 */
#include <linux/sizes.h>		/* PCI/NVMe: PAGE_SIZE 등 NVMe DMA/메모리 크기 상수 */
#include <linux/types.h>		/* PCI/NVMe: 기본 데이터 타입 */

#include "../../pci.h"			/* PCI/NVMe: PCI 코어 열거/bridge, NVMe bus 탐색 */
#include "pcie-designware.h"		/* PCI/NVMe: DesignWare 레지스터/구조체, NVMe 링크 제어용 */

static const char * const dw_pcie_app_clks[DW_PCIE_NUM_APP_CLKS] = {	/* PCI/NVMe: RC 애플리케이션 클럭 이름 테이블 */
	[DW_PCIE_DBI_CLK] = "dbi",						/* NVMe: DBI(config) 접근 클럭 */
	[DW_PCIE_MSTR_CLK] = "mstr",						/* NVMe: AXI master 클럭, NVMe DMA/메모리 트랜잭션 */
	[DW_PCIE_SLV_CLK] = "slv",						/* NVMe: AXI slave 클럭 */
};

static const char * const dw_pcie_core_clks[DW_PCIE_NUM_CORE_CLKS] = {	/* PCI/NVMe: RC 코어 클럭 이름 테이블 */
	[DW_PCIE_PIPE_CLK] = "pipe",						/* NVMe: PIPE 인터페이스 클럭, PHY 링크 동기화 */
	[DW_PCIE_CORE_CLK] = "core",						/* NVMe: PCIe 코어 로직 클럭 */
	[DW_PCIE_AUX_CLK] = "aux",						/* NVMe: 보조 클럭, low-power ASPM 상태 유지 */
	[DW_PCIE_REF_CLK] = "ref",						/* NVMe: 레퍼런스 클럭, link training 타이밍 */
};

static const char * const dw_pcie_app_rsts[DW_PCIE_NUM_APP_RSTS] = {	/* PCI/NVMe: RC 애플리케이션 리셋 이름 테이블 */
	[DW_PCIE_DBI_RST] = "dbi",						/* NVMe: DBI(config) 리셋, 열거 전 초기화 */
	[DW_PCIE_MSTR_RST] = "mstr",						/* NVMe: AXI master 리셋, NVMe DMA 트랜잭션 정지 */
	[DW_PCIE_SLV_RST] = "slv",						/* NVMe: AXI slave 리셋 */
};

static const char * const dw_pcie_core_rsts[DW_PCIE_NUM_CORE_RSTS] = {	/* PCI/NVMe: RC 코어 리셋 이름 테이블 */
	[DW_PCIE_NON_STICKY_RST] = "non-sticky",				/* NVMe: 비영구 레지스터 리셋 */
	[DW_PCIE_STICKY_RST] = "sticky",					/* NVMe: 영구 레지스터 리셋 */
	[DW_PCIE_CORE_RST] = "core",						/* NVMe: PCIe 코어 리셋, NVMe 링크 재설정 */
	[DW_PCIE_PIPE_RST] = "pipe",						/* NVMe: PIPE 리셋, PHY 재초기화 */
	[DW_PCIE_PHY_RST] = "phy",						/* NVMe: PHY 리셋, 물리 계층 재시작 */
	[DW_PCIE_HOT_RST] = "hot",						/* NVMe: 핫 리셋, 다운스트림 NVMe 재열거 */
	[DW_PCIE_PWR_RST] = "pwr",						/* NVMe: 전원 리셋, 완전 초기화 */
};

/* PCI/NVMe: PTM(Precision Time Measurement) VSEC ID 테이블 */
static const struct dwc_pcie_vsec_id dwc_pcie_ptm_vsec_ids[] = {
	{ .vendor_id = PCI_VENDOR_ID_QCOM, /* EP */				/* NVMe: QCOM EP PTM VSEC */
	  .vsec_id = 0x03, .vsec_rev = 0x1 },					/* NVMe: PTM capability ID/revision */
	{ .vendor_id = PCI_VENDOR_ID_QCOM, /* RC */				/* NVMe: QCOM RC PTM VSEC */
	  .vsec_id = 0x04, .vsec_rev = 0x1 },					/* NVMe: RC PTM capability ID/revision */
	{ }									/* NVMe: 테이블 종료 */
};

static int dw_pcie_get_clocks(struct dw_pcie *pci)				/* PCI/NVMe: PCIe RC 클럭 획득, NVMe 링크 활성화 전 필요 */
{
	int i, ret;								/* NVMe: 반복 인덱스 및 반환값 */

	for (i = 0; i < DW_PCIE_NUM_APP_CLKS; i++)				/* NVMe: 애플리케이션 클럭 ID 초기화 */
		pci->app_clks[i].id = dw_pcie_app_clks[i];			/* NVMe: 각 클럭에 이름 할당 */

	for (i = 0; i < DW_PCIE_NUM_CORE_CLKS; i++)				/* NVMe: 코어 클럭 ID 초기화 */
		pci->core_clks[i].id = dw_pcie_core_clks[i];			/* NVMe: 각 코어 클럭에 이름 할당 */

	ret = devm_clk_bulk_get_optional(pci->dev, DW_PCIE_NUM_APP_CLKS,	/* NVMe: 선택적 앱 클럭 일괄 획득 */
					 pci->app_clks);
	if (ret)								/* NVMe: 앱 클럭 획득 실패 시 */
		return ret;							/* NVMe: NVMe 초기화 중단, 에러 전달 */

	return devm_clk_bulk_get_optional(pci->dev, DW_PCIE_NUM_CORE_CLKS,	/* NVMe: 선택적 코어 클럭 일괄 획득 */
					  pci->core_clks);			/* NVMe: 실패 시 음수 반환, NVMe 사용 불가 */
}

static int dw_pcie_get_resets(struct dw_pcie *pci)				/* PCI/NVMe: PCIe RC 리셋 라인 획득, NVMe PERST# 포함 */
{
	int i, ret;								/* NVMe: 반복 인덱스 및 반환값 */

	for (i = 0; i < DW_PCIE_NUM_APP_RSTS; i++)				/* NVMe: 애플리케이션 리셋 ID 초기화 */
		pci->app_rsts[i].id = dw_pcie_app_rsts[i];			/* NVMe: 각 리셋에 이름 할당 */

	for (i = 0; i < DW_PCIE_NUM_CORE_RSTS; i++)				/* NVMe: 코어 리셋 ID 초기화 */
		pci->core_rsts[i].id = dw_pcie_core_rsts[i];			/* NVMe: 각 코어 리셋에 이름 할당 */

	ret = devm_reset_control_bulk_get_optional_shared(pci->dev,		/* NVMe: 공유 앱 리셋 일괄 획득 */
						  DW_PCIE_NUM_APP_RSTS,
						  pci->app_rsts);
	if (ret)								/* NVMe: 앱 리셋 획득 실패 시 */
		return ret;							/* NVMe: NVMe 초기화 중단 */

	ret = devm_reset_control_bulk_get_optional_exclusive(pci->dev,		/* NVMe: 전용 코어 리셋 일괄 획득 */
						     DW_PCIE_NUM_CORE_RSTS,
						     pci->core_rsts);
	if (ret)								/* NVMe: 코어 리셋 획득 실패 시 */
		return ret;							/* NVMe: NVMe 초기화 중단 */

	/* NVMe: NVMe 장치 PERST# GPIO 획득, 기본 High(리셋 상태) */
	pci->pe_rst = devm_gpiod_get_optional(pci->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pci->pe_rst))						/* NVMe: GPIO 획득 실패 확인 */
		return PTR_ERR(pci->pe_rst);					/* NVMe: PERST# 제어 불가, NVMe 바인딩 실패 가능 */

	return 0;								/* NVMe: 클럭/리셋 준비 완료 */
}

int dw_pcie_get_resources(struct dw_pcie *pci)					/* PCI/NVMe: PCIe RC 메모리/클록/리셋 리소스 획득, NVMe 열거의 전제조건 */
{
	struct platform_device *pdev = to_platform_device(pci->dev);		/* NVMe: platform_device에서 reg/irq 탐색 */
	struct device_node *np = dev_of_node(pci->dev);				/* NVMe: DT 노드, num-lanes/max-link-speed 파싱 */
	struct resource *res;							/* NVMe: MMIO 리소스 포인터 */
	int ret;								/* NVMe: 반환값 */

	if (!pci->dbi_base) {							/* NVMe: DBI(config) 영역 아직 매핑 안 됨 */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");	/* NVMe: "dbi" MMIO 리소스 조회 */
		pci->dbi_base = devm_pci_remap_cfg_resource(pci->dev, res);	/* NVMe: DBI 영역 매핑, NVMe config space 접근 기반 */
		if (IS_ERR(pci->dbi_base))					/* NVMe: DBI 매핑 실패 시 */
			return PTR_ERR(pci->dbi_base);				/* NVMe: NVMe 장치 접근 불가, 초기화 실패 */
		pci->dbi_phys_addr = res->start;				/* NVMe: DBI 물리 주소 보관, 디버그/ATU 계산용 */
	}

	/* DBI2 is mainly useful for the endpoint controller */
	if (!pci->dbi_base2) {							/* NVMe: DBI2가 아직 매핑되지 않음 */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi2");	/* NVMe: "dbi2" MMIO 리소스 조회 */
		if (res) {							/* NVMe: dbi2가 DT에 정의된 경우 */
			pci->dbi_base2 = devm_pci_remap_cfg_resource(pci->dev, res);	/* NVMe: DBI2 매핑, EP 모드에서 BAR/MSI 설정용 */
			if (IS_ERR(pci->dbi_base2))				/* NVMe: DBI2 매핑 실패 시 */
				return PTR_ERR(pci->dbi_base2);			/* NVMe: EP 설정 불가 */
		} else {
			pci->dbi_base2 = pci->dbi_base + SZ_4K;			/* NVMe: dbi2 미정의 시 dbi+4K 기본값 사용 */
		}
	}

	/* For non-unrolled iATU/eDMA platforms this range will be ignored */
	if (!pci->atu_base) {							/* NVMe: iATU 영역 아직 매핑 안 됨 */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "atu");	/* NVMe: "atu" MMIO 리소스 조회 */
		if (res) {							/* NVMe: 별도 ATU 공간이 있는 경우 */
			pci->atu_size = resource_size(res);			/* NVMe: ATU 영역 크기 기록, 윈도우 수 계산 */
			pci->atu_base = devm_ioremap_resource(pci->dev, res);	/* NVMe: ATU 레지스터 공간 매핑, BAR/config 변환 제어 */
			if (IS_ERR(pci->atu_base))					/* NVMe: ATU 매핑 실패 시 */
				return PTR_ERR(pci->atu_base);				/* NVMe: NVMe 메모리/config 접근 불가 */
			pci->atu_phys_addr = res->start;				/* NVMe: ATU 물리 주소 보관 */
		} else {
			pci->atu_base = pci->dbi_base + DEFAULT_DBI_ATU_OFFSET;	/* NVMe: ATU를 DBI 기본 오프셋에 매핑 */
		}
	}

	/* Set a default value suitable for at most 8 in and 8 out windows */
	if (!pci->atu_size)								/* NVMe: ATU 크기가 아직 설정되지 않음 */
		pci->atu_size = SZ_4K;						/* NVMe: 8개 in/out 윈도우를 위한 기본 4KB */

	/* eDMA region can be mapped to a custom base address */
	if (!pci->edma.reg_base) {							/* NVMe: eDMA 레지스터 기반이 아직 없음 */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dma");	/* NVMe: "dma" MMIO 리소스 조회 */
		if (res) {							/* NVMe: 별도 eDMA 공간이 있는 경우 */
			pci->edma.reg_base = devm_ioremap_resource(pci->dev, res);	/* NVMe: eDMA 레지스터 매핑, NVMe 데이터 복사 가속 */
			if (IS_ERR(pci->edma.reg_base))				/* NVMe: eDMA 매핑 실패 시 */
				return PTR_ERR(pci->edma.reg_base);			/* NVMe: eDMA offload 불가, 폴크백 */
		} else if (pci->atu_size >= 2 * DEFAULT_DBI_DMA_OFFSET) {		/* NVMe: ATU 공간이 충분히 큰 경우 */
			pci->edma.reg_base = pci->atu_base + DEFAULT_DBI_DMA_OFFSET;	/* NVMe: ATU 내 eDMA 오프셋 사용 */
		}
	}

	/* ELBI is an optional resource */
	if (!pci->elbi_base) {							/* NVMe: ELBI(응용 계층) 영역 아직 매핑 안 됨 */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");	/* NVMe: "elbi" MMIO 리소스 조회 */
		if (res) {							/* NVMe: ELBI가 정의된 경우 */
			pci->elbi_base = devm_ioremap_resource(pci->dev, res);	/* NVMe: ELBI 매핑, SoC 특화 제어/상태 */
			if (IS_ERR(pci->elbi_base))					/* NVMe: ELBI 매핑 실패 시 */
				return PTR_ERR(pci->elbi_base);				/* NVMe: SoC 특화 기능 제어 불가 */
		}
	}

	/* LLDD is supposed to manually switch the clocks and resets state */
	if (dw_pcie_cap_is(pci, REQ_RES)) {					/* NVMe: glue driver가 자원 관리를 요청한 경우 */
		ret = dw_pcie_get_clocks(pci);					/* NVMe: 클럭 획득 수행 */
		if (ret)							/* NVMe: 클럭 획득 실패 시 */
			return ret;							/* NVMe: NVMe 초기화 중단 */

		ret = dw_pcie_get_resets(pci);					/* NVMe: 리셋/PERST# 획득 수행 */
		if (ret)							/* NVMe: 리셋 획득 실패 시 */
			return ret;							/* NVMe: NVMe 초기화 중단 */
	}

	if (pci->max_link_speed < 1)							/* NVMe: 링크 속도가 아직 설정되지 않음 */
		pci->max_link_speed = of_pci_get_max_link_speed(np);		/* NVMe: DT에서 최대 PCIe 속도(Gen) 읽기, NVMe 성능 결정 */

	of_property_read_u32(np, "num-lanes", &pci->num_lanes);			/* NVMe: DT에서 PCIe 레인 수 읽기, NVMe 대역폭 결정 */

	if (of_property_read_bool(np, "snps,enable-cdm-check"))			/* NVMe: CDM(Correctable/Detectable?) 체크 요청 */
		dw_pcie_cap_set(pci, CDM_CHECK);					/* NVMe: 레지스터 무결성 검사 활성화, AER 관련 */

	return 0;									/* NVMe: 리소스 획득 성공, NVMe 열거 진행 가능 */
}

void dw_pcie_version_detect(struct dw_pcie *pci)				/* PCI/NVMe: DWC PCIe IP 버전 탐지, NVMe 호환성/쿼크 결정 */
{
	u32 ver;									/* NVMe: 읽은 버전 레지스터 값 */

	/* The content of the CSR is zero on DWC PCIe older than v4.70a */
	ver = dw_pcie_readl_dbi(pci, PCIE_VERSION_NUMBER);			/* NVMe: IP 코어 버전 번호 읽기 */
	if (!ver)									/* NVMe: 구버전(v4.70a 미만)은 0 */
		return;									/* NVMe: 버전 기반 쿼크 적용 안 함 */

	if (pci->version && pci->version != ver)					/* NVMe: 이전에 설정된 버전과 불일치 */
		dev_warn(pci->dev, "Versions don't match (%08x != %08x)\n",	/* NVMe: 경고, NVMe 안정성에 영향 가능 */
			 pci->version, ver);
	else
		pci->version = ver;							/* NVMe: IP 버전 저장, ATU/ECRC 쿼크 선택 */

	ver = dw_pcie_readl_dbi(pci, PCIE_VERSION_TYPE);			/* NVMe: IP 코어 타입 읽기(RC/EP 등) */

	if (pci->type && pci->type != ver)						/* NVMe: 이전 타입과 불일치 */
		dev_warn(pci->dev, "Types don't match (%08x != %08x)\n",		/* NVMe: 타입 불일치 경고 */
			 pci->type, ver);
	else
		pci->type = ver;							/* NVMe: IP 타입 저장 */
}

/* PCI/NVMe: PCIe 표준 capability 탐색, NVMe MSI/MSI-X/PowerManagement 등 */
u8 dw_pcie_find_capability(struct dw_pcie *pci, u8 cap)
{
	return PCI_FIND_NEXT_CAP(dw_pcie_read_cfg, PCI_CAPABILITY_LIST, cap,	/* NVMe: linked list 순회, NVMe 기능 캡 탐색 */
				 NULL, pci);						/* NVMe: RC 자체 config space 탐색 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_capability);

/* PCI/NVMe: PCIe extended capability 탐색, NVMe AER/ACS/SR-IOV 등 */
u16 dw_pcie_find_ext_capability(struct dw_pcie *pci, u8 cap)
{
	return PCI_FIND_NEXT_EXT_CAP(dw_pcie_read_cfg, 0, cap, NULL, pci);	/* NVMe: 256바이트 이후 확장 캡 리스트 순회 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_ext_capability);

void dw_pcie_remove_capability(struct dw_pcie *pci, u8 cap)			/* PCI/NVMe: RC의 표준 capability 제거, NVMe와 호환되지 않는 기능 숨김 */
{
	u8 cap_pos, pre_pos, next_pos;							/* NVMe: 캡 위치/이전/다음 포인터 */
	u16 reg;									/* NVMe: capability 레지스터 값 */

	cap_pos = PCI_FIND_NEXT_CAP(dw_pcie_read_cfg, PCI_CAPABILITY_LIST, cap,	/* NVMe: 제거할 캡 위치 탐색 */
				  &pre_pos, pci);
	if (!cap_pos)									/* NVMe: 캡이 없으면 */
		return;									/* NVMe: 아무 것도 안 함 */

	reg = dw_pcie_readw_dbi(pci, cap_pos);						/* NVMe: 캡 헤더 읽기 */
	next_pos = (reg & 0xff00) >> 8;							/* NVMe: 다음 캡 포인터 추출 */

	dw_pcie_dbi_ro_wr_en(pci);							/* NVMe: read-only capability 링크 강제 쓰기 허용 */
	if (pre_pos == PCI_CAPABILITY_LIST)						/* NVMe: 첫 번째 캡 제거 시 */
		dw_pcie_writeb_dbi(pci, PCI_CAPABILITY_LIST, next_pos);		/* NVMe: 캡 리스트 헤드 갱신 */
	else
		dw_pcie_writeb_dbi(pci, pre_pos + 1, next_pos);				/* NVMe: 이전 캡의 next 포인터 갱신 */
	dw_pcie_dbi_ro_wr_dis(pci);							/* NVMe: read-only 쓰기 다시 금지 */
}
EXPORT_SYMBOL_GPL(dw_pcie_remove_capability);

/* PCI/NVMe: RC의 extended capability 제거, NVMe에서 지원하지 않는 캡 숨김 */
void dw_pcie_remove_ext_capability(struct dw_pcie *pci, u8 cap)
{
	int cap_pos, next_pos, pre_pos;							/* NVMe: 캡 위치/다음/이전 오프셋 */
	u32 pre_header, header;								/* NVMe: 이전/현재 캡 헤더 */

	cap_pos = PCI_FIND_NEXT_EXT_CAP(dw_pcie_read_cfg, 0, cap, &pre_pos, pci);	/* NVMe: 제거할 확장 캡 위치 탐색 */
	if (!cap_pos)									/* NVMe: 확장 캡이 없으면 */
		return;									/* NVMe: 아무 것도 안 함 */

	header = dw_pcie_readl_dbi(pci, cap_pos);					/* NVMe: 확장 캡 헤더 읽기 */

	/*
	 * If the first cap at offset PCI_CFG_SPACE_SIZE is removed,
	 * only set its capid to zero as it cannot be skipped.
	 */
	if (cap_pos == PCI_CFG_SPACE_SIZE) {						/* NVMe: 첫 확장 캡 위치인 경우 */
		dw_pcie_dbi_ro_wr_en(pci);							/* NVMe: read-only 쓰기 허용 */
		dw_pcie_writel_dbi(pci, cap_pos, header & 0xffff0000);			/* NVMe: cap ID만 0으로 설정, 링크 유지 */
		dw_pcie_dbi_ro_wr_dis(pci);							/* NVMe: read-only 쓰기 금지 */
		return;										/* NVMe: 제거 완료 */
	}

	pre_header = dw_pcie_readl_dbi(pci, pre_pos);					/* NVMe: 이전 확장 캡 헤더 읽기 */
	next_pos = PCI_EXT_CAP_NEXT(header);						/* NVMe: 다음 확장 캡 오프셋 추출 */

	dw_pcie_dbi_ro_wr_en(pci);								/* NVMe: read-only 쓰기 허용 */
	dw_pcie_writel_dbi(pci, pre_pos,						/* NVMe: 이전 캡의 next 필드 갱신 */
			  (pre_header & 0xfffff) | (next_pos << 20));			/* NVMe: 현재 캡을 리스트에서 건림 */
	dw_pcie_dbi_ro_wr_dis(pci);								/* NVMe: read-only 쓰기 금지 */
}
EXPORT_SYMBOL_GPL(dw_pcie_remove_ext_capability);

/* PCI/NVMe: vendor-specific extended capability 탐색 helper */
static u16 __dw_pcie_find_vsec_capability(struct dw_pcie *pci, u16 vendor_id,
					  u16 vsec_id)
{
	u16 vsec = 0;									/* NVMe: 탐색 시작 오프셋 */
	u32 header;									/* NVMe: VSEC 헤더 */

	if (vendor_id != dw_pcie_readw_dbi(pci, PCI_VENDOR_ID))				/* NVMe: RC vendor ID가 일치하지 않으면 */
		return 0;									/* NVMe: 해당 VSEC 없음 */

	while ((vsec = PCI_FIND_NEXT_EXT_CAP(dw_pcie_read_cfg, vsec,			/* NVMe: VSEC 확장 캡 리스트 순회 */
					     PCI_EXT_CAP_ID_VNDR, NULL, pci))) {
		header = dw_pcie_readl_dbi(pci, vsec + PCI_VNDR_HEADER);		/* NVMe: VSEC 헤더 읽기 */
		if (PCI_VNDR_HEADER_ID(header) == vsec_id)				/* NVMe: VSEC ID 일치 시 */
			return vsec;							/* NVMe: VSEC 오프셋 반환 */
	}

	return 0;										/* NVMe: 찾지 못함 */
}

static u16 dw_pcie_find_vsec_capability(struct dw_pcie *pci,				/* PCI/NVMe: VSEC ID 테이블 기반 탐색 */
					const struct dwc_pcie_vsec_id *vsec_ids)
{
	const struct dwc_pcie_vsec_id *vid;						/* NVMe: 테이블 항목 포인터 */
	u16 vsec;										/* NVMe: 찾은 VSEC 오프셋 */
	u32 header;										/* NVMe: VSEC 헤더 */

	for (vid = vsec_ids; vid->vendor_id; vid++) {					/* NVMe: ID 테이블 순회 */
		vsec = __dw_pcie_find_vsec_capability(pci, vid->vendor_id,		/* NVMe: vendor ID/VSEC ID로 탐색 */
						      vid->vsec_id);
		if (vsec) {									/* NVMe: VSEC 발견 시 */
			header = dw_pcie_readl_dbi(pci, vsec + PCI_VNDR_HEADER);	/* NVMe: 헤더 읽기 */
			if (PCI_VNDR_HEADER_REV(header) == vid->vsec_rev)		/* NVMe: revision 일치 확인 */
				return vsec;						/* NVMe: 조건 만족 VSEC 반환 */
		}
	}

	return 0;										/* NVMe: 조건에 맞는 VSEC 없음 */
}

/* PCI/NVMe: RAS-DES(Reliability/Availability) VSEC 탐색, NVMe AER/오류 카운터 연결 */
u16 dw_pcie_find_rasdes_capability(struct dw_pcie *pci)
{
	return dw_pcie_find_vsec_capability(pci, dwc_pcie_rasdes_vsec_ids);	/* NVMe: RAS-DES VSEC ID 테이블 검색 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_rasdes_capability);

/* PCI/NVMe: PTM(Precision Time Measurement) VSEC 탐색, NVMe 타임스탬프 동기화 */
u16 dw_pcie_find_ptm_capability(struct dw_pcie *pci)
{
	return dw_pcie_find_vsec_capability(pci, dwc_pcie_ptm_vsec_ids);		/* NVMe: PTM VSEC ID 테이블 검색 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_ptm_capability);

int dw_pcie_read(void __iomem *addr, int size, u32 *val)				/* PCI/NVMe: 정렬된 MMIO 읽기, NVMe BAR/config/ATU 접근 기반 */
{
	if (!IS_ALIGNED((uintptr_t)addr, size)) {					/* NVMe: 주소 정렬 위반 시 */
		*val = 0;									/* NVMe: 0 반환, 잘못된 값 방지 */
		return PCIBIOS_BAD_REGISTER_NUMBER;					/* NVMe: 잘못된 레지스터 번호 리턴 */
	}

	if (size == 4) {									/* NVMe: 32비트 읽기 */
		*val = readl(addr);								/* NVMe: 4바이트 MMIO 읽기, NVMe BAR 32비트 레지스터 */
	} else if (size == 2) {								/* NVMe: 16비트 읽기 */
		*val = readw(addr);								/* NVMe: 2바이트 MMIO 읽기, PCIe config word */
	} else if (size == 1) {								/* NVMe: 8비트 읽기 */
		*val = readb(addr);								/* NVMe: 1바이트 MMIO 읽기 */
	} else {										/* NVMe: 지원하지 않는 크기 */
		*val = 0;									/* NVMe: 0 반환 */
		return PCIBIOS_BAD_REGISTER_NUMBER;					/* NVMe: 에러 반환 */
	}

	return PCIBIOS_SUCCESSFUL;								/* NVMe: 읽기 성공 */
}
EXPORT_SYMBOL_GPL(dw_pcie_read);

int dw_pcie_write(void __iomem *addr, int size, u32 val)				/* PCI/NVMe: 정렬된 MMIO 쓰기, NVMe BAR/config/ATU 제어 기반 */
{
	if (!IS_ALIGNED((uintptr_t)addr, size))						/* NVMe: 주소 정렬 위반 시 */
		return PCIBIOS_BAD_REGISTER_NUMBER;						/* NVMe: 에러 반환, NVMe 레지스터 손상 방지 */

	if (size == 4)										/* NVMe: 32비트 쓰기 */
		writel(val, addr);									/* NVMe: 4바이트 MMIO 쓰기, NVMe doorbell/ATU 제어 */
	else if (size == 2)									/* NVMe: 16비트 쓰기 */
		writew(val, addr);									/* NVMe: 2바이트 MMIO 쓰기, PCIe config word */
	else if (size == 1)									/* NVMe: 8비트 쓰기 */
		writeb(val, addr);									/* NVMe: 1바이트 MMIO 쓰기 */
	else											/* NVMe: 지원하지 않는 크기 */
		return PCIBIOS_BAD_REGISTER_NUMBER;						/* NVMe: 에러 반환 */

	return PCIBIOS_SUCCESSFUL;									/* NVMe: 쓰기 성공 */
}
EXPORT_SYMBOL_GPL(dw_pcie_write);

/* PCI/NVMe: DBI(config) 공간 읽기, RC 자체 및 다운스트림 NVMe config 접근 */
u32 dw_pcie_read_dbi(struct dw_pcie *pci, u32 reg, size_t size)
{
	int ret;										/* NVMe: dw_pcie_read 반환값 */
	u32 val;										/* NVMe: 읽은 값 */

	if (pci->ops && pci->ops->read_dbi)						/* NVMe: glue driver 커스텀 read 콜백 존재 */
		return pci->ops->read_dbi(pci, pci->dbi_base, reg, size);	/* NVMe: SoC 특화 DBI 읽기, NVMe config 접근 */

	ret = dw_pcie_read(pci->dbi_base + reg, size, &val);				/* NVMe: 기본 DBI MMIO 읽기, NVMe capability 탐색 */
	if (ret)										/* NVMe: 읽기 실패 시 */
		dev_err(pci->dev, "Read DBI address failed\n");				/* NVMe: NVMe 열거/초기화 중 에러 로그 */

	return val;										/* NVMe: 읽은 레지스터 값 반환 */
}
EXPORT_SYMBOL_GPL(dw_pcie_read_dbi);

/* PCI/NVMe: DBI(config) 공간 쓰기, NVMe 링크/MSI/BAR 설정 */
void dw_pcie_write_dbi(struct dw_pcie *pci, u32 reg, size_t size, u32 val)
{
	int ret;										/* NVMe: dw_pcie_write 반환값 */

	if (pci->ops && pci->ops->write_dbi) {						/* NVMe: glue driver 커스텀 write 콜백 존재 */
		pci->ops->write_dbi(pci, pci->dbi_base, reg, size, val);	/* NVMe: SoC 특화 DBI 쓰기 */
		return;										/* NVMe: 커스텀 쓰기 완료 */
	}

	ret = dw_pcie_write(pci->dbi_base + reg, size, val);				/* NVMe: 기본 DBI MMIO 쓰기, NVMe capability/링크 설정 */
	if (ret)										/* NVMe: 쓰기 실패 시 */
		dev_err(pci->dev, "Write DBI address failed\n");				/* NVMe: NVMe 제어 레지스터 쓰기 실패 로그 */
}
EXPORT_SYMBOL_GPL(dw_pcie_write_dbi);

/* PCI/NVMe: DBI2 공간 쓰기, EP 모드에서 NVMe BAR/MSI-X 등 설정 */
void dw_pcie_write_dbi2(struct dw_pcie *pci, u32 reg, size_t size, u32 val)
{
	int ret;										/* NVMe: dw_pcie_write 반환값 */

	if (pci->ops && pci->ops->write_dbi2) {						/* NVMe: glue driver 커스텀 DBI2 쓰기 */
		pci->ops->write_dbi2(pci, pci->dbi_base2, reg, size, val);	/* NVMe: SoC 특화 DBI2 쓰기 */
		return;										/* NVMe: 커스텀 쓰기 완료 */
	}

	ret = dw_pcie_write(pci->dbi_base2 + reg, size, val);				/* NVMe: DBI2 MMIO 쓰기, EP BAR 초기화 */
	if (ret)										/* NVMe: 쓰기 실패 시 */
		dev_err(pci->dev, "write DBI address failed\n");				/* NVMe: EP 설정 실패 로그 */
}
EXPORT_SYMBOL_GPL(dw_pcie_write_dbi2);

/* PCI/NVMe: iATU 영역 선택, NVMe 메모리/config/message TLP 변환 */
static inline void __iomem *dw_pcie_select_atu(struct dw_pcie *pci, u32 dir,
					       u32 index)
{
	if (dw_pcie_cap_is(pci, IATU_UNROLL))						/* NVMe: unrolled iATU를 사용하는 IP 버전 */
		return pci->atu_base + PCIE_ATU_UNROLL_BASE(dir, index);	/* NVMe: 인덱스 기반 직접 ATU 레지스터 주소 반환 */

	dw_pcie_writel_dbi(pci, PCIE_ATU_VIEWPORT, dir | index);			/* NVMe: viewport 레지스터에 방향/인덱스 기록, 구버전 ATU */
	return pci->atu_base;									/* NVMe: viewport 기반 ATU 기본 주소 반환 */
}

/* PCI/NVMe: ATU 레지스터 읽기, NVMe BAR/iATU 설정 검증 */
static u32 dw_pcie_readl_atu(struct dw_pcie *pci, u32 dir, u32 index, u32 reg)
{
	void __iomem *base;									/* NVMe: 선택된 ATU 기본 주소 */
	int ret;											/* NVMe: dw_pcie_read 반환값 */
	u32 val;											/* NVMe: 읽은 값 */

	base = dw_pcie_select_atu(pci, dir, index);						/* NVMe: 해당 ATU 영역 선택 */

	if (pci->ops && pci->ops->read_dbi)							/* NVMe: glue driver가 read_dbi 재정의 */
		return pci->ops->read_dbi(pci, base, reg, 4);					/* NVMe: SoC 특화 ATU 읽기 */

	ret = dw_pcie_read(base + reg, 4, &val);						/* NVMe: ATU 레지스터 4바이트 읽기 */
	if (ret)											/* NVMe: 읽기 실패 시 */
		dev_err(pci->dev, "Read ATU address failed\n");					/* NVMe: NVMe 메모리 창 설정 실패 가능성 로그 */

	return val;											/* NVMe: ATU 레지스터 값 반환 */
}

/* PCI/NVMe: ATU 레지스터 쓰기, NVMe 메모리/config 창 프로그래밍 */
static void dw_pcie_writel_atu(struct dw_pcie *pci, u32 dir, u32 index,
			       u32 reg, u32 val)
{
	void __iomem *base;									/* NVMe: 선택된 ATU 기본 주소 */
	int ret;											/* NVMe: dw_pcie_write 반환값 */

	base = dw_pcie_select_atu(pci, dir, index);						/* NVMe: 해당 ATU 영역 선택 */

	if (pci->ops && pci->ops->write_dbi) {							/* NVMe: glue driver가 write_dbi 재정의 */
		pci->ops->write_dbi(pci, base, reg, 4, val);					/* NVMe: SoC 특화 ATU 쓰기 */
		return;											/* NVMe: 커스텀 쓰기 완료 */
	}

	ret = dw_pcie_write(base + reg, 4, val);						/* NVMe: ATU 레지스터 4바이트 쓰기 */
	if (ret)											/* NVMe: 쓰기 실패 시 */
		dev_err(pci->dev, "Write ATU address failed\n");					/* NVMe: NVMe 창 설정 실패 로그 */
}

static inline u32 dw_pcie_readl_atu_ob(struct dw_pcie *pci, u32 index, u32 reg)	/* PCI/NVMe: outbound ATU 레지스터 읽기 */
{
	/* NVMe: CPU->PCIe 방향 ATU 읽기, NVMe BAR/CFG/MSG 전송용 */
	return dw_pcie_readl_atu(pci, PCIE_ATU_REGION_DIR_OB, index, reg);
}

static inline void dw_pcie_writel_atu_ob(struct dw_pcie *pci, u32 index, u32 reg,	/* PCI/NVMe: outbound ATU 레지스터 쓰기 */
					 u32 val)
{
	/* NVMe: CPU->PCIe 방향 ATU 쓰기, NVMe 메모리/config/message 창 설정 */
	dw_pcie_writel_atu(pci, PCIE_ATU_REGION_DIR_OB, index, reg, val);
}

static inline u32 dw_pcie_enable_ecrc(u32 val)							/* PCI/NVMe: ATU TD 비트를 통해 ECRC(TLP Digest) 강제 활성화, NVMe AER 무결성 */
{
	/*
	 * DWC versions 0x3530302a and 0x3536322a have a design issue where
	 * the 'TD' bit in the Control register-1 of the ATU outbound
	 * region acts like an override for the ECRC setting, i.e., the
	 * presence of TLP Digest (ECRC) in the outgoing TLPs is solely
	 * determined by this bit. This is contrary to the PCIe spec which
	 * says that the enablement of the ECRC is solely determined by the
	 * AER registers.
	 *
	 * Because of this, even when the ECRC is enabled through AER
	 * registers, the transactions going through ATU won't have TLP
	 * Digest as there is no way the PCI core AER code could program
	 * the TD bit which is specific to the DesignWare core.
	 *
	 * The best way to handle this scenario is to program the TD bit
	 * always. It affects only the traffic from root port to downstream
	 * devices.
	 *
	 * At this point,
	 * When ECRC is enabled in AER registers, everything works normally
	 * When ECRC is NOT enabled in AER registers, then,
	 * on Root Port:- TLP Digest (DWord size) gets appended to each packet
	 *                even through it is not required. Since downstream
	 *                TLPs are mostly for configuration accesses and BAR
	 *                accesses, they are not in critical path and won't
	 *                have much negative effect on the performance.
	 * on End Point:- TLP Digest is received for some/all the packets coming
	 *                from the root port. TLP Digest is ignored because,
	 *                as per the PCIe Spec r5.0 v1.0 section 2.2.3
	 *                "TLP Digest Rules", when an endpoint receives TLP
	 *                Digest when its ECRC check functionality is disabled
	 *                in AER registers, received TLP Digest is just ignored.
	 * Since there is no issue or error reported either side, best way to
	 * handle the scenario is to program TD bit by default.
	 */

	return val | PCIE_ATU_TD;									/* NVMe: TD 비트 설정, NVMe TLP에 ECRC digest 추가/보존 */
}

/* PCI/NVMe: outbound iATU 프로그램, NVMe config/MMIO/MSG 전송 경로 설정 */
int dw_pcie_prog_outbound_atu(struct dw_pcie *pci,
			      const struct dw_pcie_ob_atu_cfg *atu)
{
	u64 parent_bus_addr = atu->parent_bus_addr;						/* NVMe: CPU 측(또는 상위 버스) 물리 주소 */
	u32 retries, val;										/* NVMe: 재시도 카운터, 임시 레지스터 값 */
	u64 limit_addr;											/* NVMe: 윈도우 상한 주소 */

	if (atu->index >= pci->num_ob_windows)							/* NVMe: 요청 윈도우가 지원 개수 초과 */
		return -ENOSPC;											/* NVMe: NVMe BAR/config 창 부족 */

	limit_addr = parent_bus_addr + atu->size - 1;						/* NVMe: outbound 윈도우 마지막 주소 계산 */

	if ((limit_addr & ~pci->region_limit) != (parent_bus_addr & ~pci->region_limit) ||	/* NVMe: 윈도우가 region limit 경계 넘어감 */
	    !IS_ALIGNED(parent_bus_addr, pci->region_align) ||							/* NVMe: 시작 주소 정렬 불량 */
	    !IS_ALIGNED(atu->pci_addr, pci->region_align) || !atu->size) {					/* NVMe: PCIe 주소/크기 정렬 오류 또는 크기 0 */
		return -EINVAL;											/* NVMe: 잘못된 NVMe 메모리 매핑 파라미터 */
	}

	dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_LOWER_BASE,						/* NVMe: outbound 윈도우 하위 기본 주소 설정 */
			      lower_32_bits(parent_bus_addr));							/* NVMe: CPU 물리 주소 하위 32비트 */
	dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_UPPER_BASE,						/* NVMe: outbound 윈도우 상위 기본 주소 설정 */
			      upper_32_bits(parent_bus_addr));							/* NVMe: CPU 물리 주소 상위 32비트(64비트 환경) */

	dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_LIMIT,							/* NVMe: outbound 윈도우 하위 한계 주소 설정 */
			      lower_32_bits(limit_addr));								/* NVMe: 한계 주소 하위 32비트 */
	if (dw_pcie_ver_is_ge(pci, 460A))									/* NVMe: v4.60a 이상에서 상위 한계 지원 */
		dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_UPPER_LIMIT,						/* NVMe: outbound 윈도우 상위 한계 주소 설정 */
			      upper_32_bits(limit_addr));								/* NVMe: 한계 주소 상위 32비트 */

	dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_LOWER_TARGET,						/* NVMe: outbound 대상 하위 주소 설정 */
			      lower_32_bits(atu->pci_addr));							/* NVMe: PCIe 버스 주소 하위 32비트, NVMe BAR/config target */
	dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_UPPER_TARGET,						/* NVMe: outbound 대상 상위 주소 설정 */
			      upper_32_bits(atu->pci_addr));							/* NVMe: PCIe 버스 주소 상위 32비트 */

	/* NVMe: ATU CTRL1: TLP 타입, 라우팅, function 번호 조합 */
	val = atu->type | atu->routing | PCIE_ATU_FUNC_NUM(atu->func_no);
	if (upper_32_bits(limit_addr) > upper_32_bits(parent_bus_addr) &&					/* NVMe: 4GB 이상의 큰 윈도우 필요 */
	    dw_pcie_ver_is_ge(pci, 460A))									/* NVMe: v4.60a 이상에서 대용량 윈도우 지원 */
		val |= PCIE_ATU_INCREASE_REGION_SIZE;								/* NVMe: region size 증가 비트 설정, NVMe 대용량 BAR 매핑 */
	if (dw_pcie_ver_is(pci, 490A) || dw_pcie_ver_is(pci, 500A))						/* NVMe: ECRC 버그가 있는 IP 버전 */
		val = dw_pcie_enable_ecrc(val);										/* NVMe: TD 비트 강제 설정, NVMe TLP digest 보존 */
	dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_REGION_CTRL1, val);					/* NVMe: outbound ATU 제어1 레지스터 기록 */

	val = PCIE_ATU_ENABLE | atu->ctrl2;										/* NVMe: ATU 활성화 및 추가 제어2 비트 */
	if (atu->type == PCIE_ATU_TYPE_MSG) {										/* NVMe: Message TLP(예: PME, ATS) 윈도우 */
		/* The data-less messages only for now */
		val |= PCIE_ATU_INHIBIT_PAYLOAD | atu->code;							/* NVMe: payload 금지, message code 설정 */
	}
	dw_pcie_writel_atu_ob(pci, atu->index, PCIE_ATU_REGION_CTRL2, val);					/* NVMe: outbound ATU 활성화 및 제어2 설정 */

	/*
	 * Make sure ATU enable takes effect before any subsequent config
	 * and I/O accesses.
	 */
	/* NVMe: ATU 활성화 폴링, NVMe config/BAR 접근 전 완료 필요 */
	for (retries = 0; retries < LINK_WAIT_MAX_IATU_RETRIES; retries++) {
		val = dw_pcie_readl_atu_ob(pci, atu->index, PCIE_ATU_REGION_CTRL2);				/* NVMe: ATU 활성화 비트 읽기 */
		if (val & PCIE_ATU_ENABLE)										/* NVMe: ATU 활성화 완료 */
			return 0;											/* NVMe: outbound 윈도우 설정 성공 */

		mdelay(LINK_WAIT_IATU);											/* NVMe: ATU 활성화 대기 */
	}

	dev_err(pci->dev, "Outbound iATU is not being enabled\n");							/* NVMe: ATU 활성화 실패, NVMe 메모리/config 접근 불가 */

	return -ETIMEDOUT;												/* NVMe: 타임아웃, NVMe 초기화 실패 */
}

static inline u32 dw_pcie_readl_atu_ib(struct dw_pcie *pci, u32 index, u32 reg)	/* PCI/NVMe: inbound ATU 레지스터 읽기 */
{
	return dw_pcie_readl_atu(pci, PCIE_ATU_REGION_DIR_IB, index, reg);					/* NVMe: PCIe->CPU 방향 ATU 읽기, NVMe EP BAR 수신용 */
}

static inline void dw_pcie_writel_atu_ib(struct dw_pcie *pci, u32 index, u32 reg,		/* PCI/NVMe: inbound ATU 레지스터 쓰기 */
					 u32 val)
{
	/* NVMe: PCIe->CPU 방향 ATU 쓰기, NVMe SSD EP BAR 매핑 */
	dw_pcie_writel_atu(pci, PCIE_ATU_REGION_DIR_IB, index, reg, val);
}

/* PCI/NVMe: inbound iATU 프로그램, NVMe EP 모드에서 host가 NVMe BAR 접근 */
int dw_pcie_prog_inbound_atu(struct dw_pcie *pci, int index, int type,
			     u64 parent_bus_addr, u64 pci_addr, u64 size)
{
	u64 limit_addr = pci_addr + size - 1;									/* NVMe: inbound 윈도우 PCIe 측 상한 주소 */
	u32 retries, val;												/* NVMe: 재시도, 임시 값 */

	if (index >= pci->num_ib_windows)										/* NVMe: inbound 윈도우 수 초과 */
		return -ENOSPC;													/* NVMe: NVMe EP BAR 매핑 실패 */

	if ((limit_addr & ~pci->region_limit) != (pci_addr & ~pci->region_limit) ||			/* NVMe: 윈도우가 region limit 경과 */
	    !IS_ALIGNED(parent_bus_addr, pci->region_align) ||							/* NVMe: CPU 메모리 주소 정렬 불량 */
	    !IS_ALIGNED(pci_addr, pci->region_align) || !size) {							/* NVMe: PCIe 주소/크기 정렬 오류 또는 크기 0 */
		return -EINVAL;													/* NVMe: 잘못된 inbound 매핑 파라미터 */
	}

	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_LOWER_BASE,							/* NVMe: inbound 윈도우 하위 기본 주소 설정 */
			      lower_32_bits(pci_addr));									/* NVMe: PCIe 버스 주소 하위 32비트 */
	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_UPPER_BASE,							/* NVMe: inbound 윈도우 상위 기본 주소 설정 */
			      upper_32_bits(pci_addr));									/* NVMe: PCIe 버스 주소 상위 32비트 */

	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_LIMIT,								/* NVMe: inbound 윈도우 하위 한계 주소 설정 */
			      lower_32_bits(limit_addr));									/* NVMe: 한계 주소 하위 32비트 */
	if (dw_pcie_ver_is_ge(pci, 460A))											/* NVMe: v4.60a 이상 상위 한계 지원 */
		dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_UPPER_LIMIT,							/* NVMe: inbound 윈도우 상위 한계 주소 설정 */
			      upper_32_bits(limit_addr));									/* NVMe: 한계 주소 상위 32비트 */

	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_LOWER_TARGET,							/* NVMe: inbound 대상 하위 주소 설정 */
			      lower_32_bits(parent_bus_addr));								/* NVMe: CPU 물리 주소 하위 32비트, NVMe DMA 버퍼/메모리 */
	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_UPPER_TARGET,							/* NVMe: inbound 대상 상위 주소 설정 */
			      upper_32_bits(parent_bus_addr));								/* NVMe: CPU 물리 주소 상위 32비트 */

	val = type;															/* NVMe: ATU TLP 타입(MEM/IO/CFG) 설정 */
	if (upper_32_bits(limit_addr) > upper_32_bits(pci_addr) &&							/* NVMe: 4GB 이상 inbound 윈도우 */
	    dw_pcie_ver_is_ge(pci, 460A))											/* NVMe: 대용량 윈도우 지원 버전 */
		val |= PCIE_ATU_INCREASE_REGION_SIZE;										/* NVMe: region size 증가, NVMe 대용량 BAR 수용 */
	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_REGION_CTRL1, val);						/* NVMe: inbound ATU 제어1 기록 */
	/* NVMe: inbound ATU 활성화, host의 NVMe BAR 접근 허용 */
	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_REGION_CTRL2, PCIE_ATU_ENABLE);

	/*
	 * Make sure ATU enable takes effect before any subsequent config
	 * and I/O accesses.
	 */
	for (retries = 0; retries < LINK_WAIT_MAX_IATU_RETRIES; retries++) {					/* NVMe: inbound ATU 활성화 폴링 */
		val = dw_pcie_readl_atu_ib(pci, index, PCIE_ATU_REGION_CTRL2);					/* NVMe: 활성화 비트 읽기 */
		if (val & PCIE_ATU_ENABLE)											/* NVMe: inbound ATU 활성화 완료 */
			return 0;													/* NVMe: inbound 매핑 성공 */

		mdelay(LINK_WAIT_IATU);													/* NVMe: 활성화 대기 */
	}

	dev_err(pci->dev, "Inbound iATU is not being enabled\n");							/* NVMe: inbound ATU 활성화 실패, NVMe EP BAR 접근 불가 */

	return -ETIMEDOUT;													/* NVMe: 타임아웃 */
}

/* PCI/NVMe: EP 모드에서 function별 inbound ATU(BAR mode) 설정, NVMe SSD function */
int dw_pcie_prog_ep_inbound_atu(struct dw_pcie *pci, u8 func_no, int index,
				int type, u64 parent_bus_addr, u8 bar, size_t size)
{
	u32 retries, val;													/* NVMe: 재시도, 임시 값 */

	if (!IS_ALIGNED(parent_bus_addr, pci->region_align) ||								/* NVMe: CPU 주소 정렬 불량 */
	    !IS_ALIGNED(parent_bus_addr, size))										/* NVMe: 크기 단위 정렬 불량 */
		return -EINVAL;														/* NVMe: NVMe EP BAR 매핑 파라미터 오류 */

	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_LOWER_TARGET,							/* NVMe: inbound 대상 하위 주소 설정 */
			      lower_32_bits(parent_bus_addr));								/* NVMe: CPU 물리 주소 하위 32비트 */
	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_UPPER_TARGET,							/* NVMe: inbound 대상 상위 주소 설정 */
			      upper_32_bits(parent_bus_addr));								/* NVMe: CPU 물리 주소 상위 32비트 */

	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_REGION_CTRL1, type |					/* NVMe: ATU 타입 설정 */
			      PCIE_ATU_FUNC_NUM(func_no));									/* NVMe: function 번호 매칭, NVMe PF/VF 구분 */
	dw_pcie_writel_atu_ib(pci, index, PCIE_ATU_REGION_CTRL2,							/* NVMe: inbound ATU 제어2 설정 */
			      PCIE_ATU_ENABLE | PCIE_ATU_FUNC_NUM_MATCH_EN |					/* NVMe: ATU 활성화 및 function 매칭 */
			      PCIE_ATU_BAR_MODE_ENABLE | (bar << 8));							/* NVMe: BAR 모드 활성화, NVMe BAR 번호 지정 */

	/*
	 * Make sure ATU enable takes effect before any subsequent config
	 * and I/O accesses.
	 */
	for (retries = 0; retries < LINK_WAIT_MAX_IATU_RETRIES; retries++) {					/* NVMe: BAR mode ATU 활성화 폴링 */
		val = dw_pcie_readl_atu_ib(pci, index, PCIE_ATU_REGION_CTRL2);					/* NVMe: 활성화 비트 읽기 */
		if (val & PCIE_ATU_ENABLE)											/* NVMe: EP inbound ATU 활성화 완료 */
			return 0;													/* NVMe: NVMe BAR 매핑 성공 */

		mdelay(LINK_WAIT_IATU);													/* NVMe: 활성화 대기 */
	}

	dev_err(pci->dev, "Inbound iATU is not being enabled\n");							/* NVMe: EP inbound ATU 실패, NVMe function BAR 접근 불가 */

	return -ETIMEDOUT;													/* NVMe: 타임아웃 */
}

/* PCI/NVMe: iATU 윈도우 비활성화, NVMe BAR/config 접근 해제 */
void dw_pcie_disable_atu(struct dw_pcie *pci, u32 dir, int index)
{
	dw_pcie_writel_atu(pci, dir, index, PCIE_ATU_REGION_CTRL2, 0);						/* NVMe: ATU ENABLE 비트 클리어, NVMe 트래픽 중단 */
}

const char *dw_pcie_ltssm_status_string(enum dw_pcie_ltssm ltssm)				/* PCI/NVMe: LTSSM 상태를 문자열로 변환, NVMe 링크 디버깅 */
{
	const char *str;													/* NVMe: 결과 문자열 포인터 */

	switch (ltssm) {													/* NVMe: LTSSM 상태 분기 */
#define DW_PCIE_LTSSM_NAME(n) case n: str = #n; break							/* NVMe: 매크로로 상태별 문자열 대입 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_DETECT_QUIET);								/* NVMe: Detect.Quiet, NVMe 장치 미검출 가능 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_DETECT_ACT);								/* NVMe: Detect.Active, NVMe 장치 탐색 중 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_POLL_ACTIVE);								/* NVMe: Polling.Active, 장치 있으나 활성화 대기 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_POLL_COMPLIANCE);							/* NVMe: Polling.Compliance, compliance 패턴 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_POLL_CONFIG);								/* NVMe: Polling.Config, link width/speed 협상 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_PRE_DETECT_QUIET);							/* NVMe: Pre-Detect.Quiet */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_DETECT_WAIT);								/* NVMe: Detect.Wait */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_CFG_LINKWD_START);							/* NVMe: Config.Linkwidth.Start, lane 수 협상 시작 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_CFG_LINKWD_ACEPT);							/* NVMe: Config.Linkwidth.Accept */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_CFG_LANENUM_WAI);							/* NVMe: Config.Lanenum.Wait */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_CFG_LANENUM_ACEPT);							/* NVMe: Config.Lanenum.Accept */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_CFG_COMPLETE);								/* NVMe: Config.Complete, NVMe 링크 설정 완료 직전 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_CFG_IDLE);									/* NVMe: Config.Idle, L0 진입 준비 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_LOCK);								/* NVMe: Recovery.Lock, 재학습/복구 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_SPEED);								/* NVMe: Recovery.Speed, 속도 재협상 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_RCVRCFG);							/* NVMe: Recovery.RcvrCfg */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_IDLE);								/* NVMe: Recovery.Idle, 복구 완료 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L0);										/* NVMe: L0, NVMe 정상 동작 상태 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L0S);										/* NVMe: L0s, low-power ASPM 상태 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L123_SEND_EIDLE);							/* NVMe: L1/L2/L3 준비, 전력 상태 전환 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L1_IDLE);									/* NVMe: L1 idle, ASPM/PM 활성화 시 NVMe 대기 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L2_IDLE);									/* NVMe: L2 idle, D3cold/suspend 상태 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L2_WAKE);									/* NVMe: L2 wake, NVMe resume 과정 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_DISABLED_ENTRY);							/* NVMe: Disabled.Entry, NVMe 분리 진행 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_DISABLED_IDLE);							/* NVMe: Disabled.Idle */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_DISABLED);									/* NVMe: Disabled, NVMe 핫플러그 제거 상태 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_LPBK_ENTRY);								/* NVMe: Loopback.Entry, diagnostics */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_LPBK_ACTIVE);								/* NVMe: Loopback.Active */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_LPBK_EXIT);								/* NVMe: Loopback.Exit */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_LPBK_EXIT_TIMEOUT);							/* NVMe: Loopback.Exit.Timeout */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_HOT_RESET_ENTRY);							/* NVMe: Hot Reset.Entry, NVMe 핫 리셋 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_HOT_RESET);								/* NVMe: Hot Reset, NVMe 다시 열거 예정 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_EQ0);								/* NVMe: Recovery.Equalization.Phase0, Gen3+ EQ */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_EQ1);								/* NVMe: Recovery.Equalization.Phase1 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_EQ2);								/* NVMe: Recovery.Equalization.Phase2 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_RCVRY_EQ3);								/* NVMe: Recovery.Equalization.Phase3 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L1_1);										/* NVMe: L1.1 substates, NVMe 저전력 */
	DW_PCIE_LTSSM_NAME(DW_PCIE_LTSSM_L1_2);										/* NVMe: L1.2 substates, NVMe 깊은 저전력 */
	default:
		str = "DW_PCIE_LTSSM_UNKNOWN";										/* NVMe: 알 수 없는 상태 */
		break;															/* NVMe: switch 문 종료 */
	}

	return str + strlen("DW_PCIE_LTSSM_");										/* NVMe: 접두어 제거 후 상태 이름 반환, NVMe 디버깅/핫플러그 로그 */
}

/**
 * dw_pcie_wait_for_link - Wait for the PCIe link to be up
 * @pci: DWC instance
 *
 * Returns: 0 if link is up, -ENODEV if device is not found, -EIO if the device
 * is found but not active and -ETIMEDOUT if the link fails to come up for other
 * reasons.
 */
int dw_pcie_wait_for_link(struct dw_pcie *pci)									/* PCI/NVMe: PCIe 링크 업 대기, NVMe 장치 검출/활성화 확인 */
{
	u32 offset, val, ltssm;												/* NVMe: capability 오프셋, 링크 상태, LTSSM */
	int retries;														/* NVMe: 링크 폴링 반복 횟수 */

	/* Check if the link is up or not */
	for (retries = 0; retries < PCIE_LINK_WAIT_MAX_RETRIES; retries++) {				/* NVMe: 최대 재시도만큼 링크 상태 폴링 */
		if (dw_pcie_link_up(pci))											/* NVMe: NVMe 장치와의 PCIe 링크 업 확인 */
			break;														/* NVMe: 링크 업, 루프 종료 */

		msleep(PCIE_LINK_WAIT_SLEEP_MS);									/* NVMe: 링크 업 대기, NVMe 장치 부팅 시간 확보 */
	}

	if (retries >= PCIE_LINK_WAIT_MAX_RETRIES) {									/* NVMe: 링크 업 실패 */
		/*
		 * If the link is in Detect.Quiet or Detect.Active state, it
		 * indicates that no device is detected.
		 */
		ltssm = dw_pcie_get_ltssm(pci);										/* NVMe: 현재 LTSSM 상태 읽기, NVMe 존재 여부 판단 */
		if (ltssm == DW_PCIE_LTSSM_DETECT_QUIET ||								/* NVMe: Detect.Quiet 상태 */
		    ltssm == DW_PCIE_LTSSM_DETECT_ACT) {								/* NVMe: Detect.Active 상태 */
			dev_info(pci->dev, "Device not found\n");							/* NVMe: NVMe 장치 미검출, 열거 건너뜀 */
			return -ENODEV;												/* NVMe: NVMe 없음 */

		/*
		 * If the link is in POLL.{Active/Compliance} state, then the
		 * device is found to be connected to the bus, but it is not
		 * active i.e., the device firmware might not yet initialized.
		 */
		} else if (ltssm == DW_PCIE_LTSSM_POLL_ACTIVE ||							/* NVMe: Polling.Active, 장치 연결됨 */
			   ltssm == DW_PCIE_LTSSM_POLL_COMPLIANCE) {							/* NVMe: Polling.Compliance, 장치 연결됨 */
			dev_info(pci->dev, "Device found, but not active\n");					/* NVMe: NVMe 장치 있으나 아직 활성화 안 됨, retry 가능 */
			return -EIO;												/* NVMe: 장치 비활성 */
		}

		dev_err(pci->dev, "Link failed to come up. LTSSM: %s\n",					/* NVMe: 링크 업 실패 로그 */
			dw_pcie_ltssm_status_string(ltssm));								/* NVMe: LTSSM 상태 문자열 출력, NVMe 초기화 디버깅 */
		return -ETIMEDOUT;													/* NVMe: 타임아웃, NVMe 사용 불가 */
	}

	/*
	 * As per PCIe r6.0, sec 6.6.1, a Downstream Port that supports Link
	 * speeds greater than 5.0 GT/s, software must wait a minimum of 100 ms
	 * after Link training completes before sending a Configuration Request.
	 */
	if (pci->max_link_speed > 2)												/* NVMe: Gen3 이상 링크 속도 */
		msleep(PCIE_RESET_CONFIG_WAIT_MS);									/* NVMe: 100ms 대기 후 NVMe config cycle 전송, 스펙 준수 */

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);							/* NVMe: PCIe Express capability 오프셋 탐색 */
	val = dw_pcie_readw_dbi(pci, offset + PCI_EXP_LNKSTA);							/* NVMe: PCIe Link Status 레지스터 읽기 */

	dev_info(pci->dev, "PCIe Gen.%u x%u link up\n",								/* NVMe: 링크 속도/폭 로그, NVMe 성능 확인 */
		 FIELD_GET(PCI_EXP_LNKSTA_CLS, val),									/* NVMe: Current Link Speed 추출 */
		 FIELD_GET(PCI_EXP_LNKSTA_NLW, val));									/* NVMe: Negotiated Link Width 추출 */

	return 0;																/* NVMe: 링크 업 성공, NVMe 열거 진행 */
}
EXPORT_SYMBOL_GPL(dw_pcie_wait_for_link);

bool dw_pcie_link_up(struct dw_pcie *pci)										/* PCI/NVMe: PCIe 링크 물리적 업 상태 확인, NVMe 핫플러그/초기화 */
{
	u32 val;																/* NVMe: debug 레지스터 값 */

	if (pci->ops && pci->ops->link_up)											/* NVMe: glue driver 커스텀 링크 상태 콜백 */
		return pci->ops->link_up(pci);											/* NVMe: SoC 특화 링크 확인, NVMe 연결 상태 */

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_DEBUG1);								/* NVMe: Port Debug1 레지스터 읽기 */
	return ((val & PCIE_PORT_DEBUG1_LINK_UP) &&									/* NVMe: LINK_UP 비트 설정 */
		(!(val & PCIE_PORT_DEBUG1_LINK_IN_TRAINING)));								/* NVMe: training 중이 아님, NVMe L0 도입 완료 */
}
EXPORT_SYMBOL_GPL(dw_pcie_link_up);

void dw_pcie_upconfig_setup(struct dw_pcie *pci)								/* PCI/NVMe: lane upconfig 지원 설정, NVMe 대역폭 동적 확장 */
{
	u32 val;																/* NVMe: multi lane control 레지스터 값 */

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL);						/* NVMe: Multi Lane Control 레지스터 읽기 */
	val |= PORT_MLTI_UPCFG_SUPPORT;												/* NVMe: upconfig 지원 비트 설정, lane 수 런타임 증가 가능 */
	dw_pcie_writel_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, val);						/* NVMe: upconfig 설정 기록, NVMe 성능 동적 조정 */
}
EXPORT_SYMBOL_GPL(dw_pcie_upconfig_setup);

static void dw_pcie_link_set_max_speed(struct dw_pcie *pci)							/* PCI/NVMe: PCIe 링크 최대 속도 설정, NVMe Gen3/4/5 성능 상한 */
{
	u32 cap, ctrl2, link_speed;												/* NVMe: Link Capability, Control2, 목표 속도 */
	u8 offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);						/* NVMe: PCIe Express capability 오프셋 */

	cap = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);							/* NVMe: Link Capability 레지스터 읽기 */

	/*
	 * Even if the platform doesn't want to limit the maximum link speed,
	 * just cache the hardware default value so that the vendor drivers can
	 * use it to do any link specific configuration.
	 */
	if (pci->max_link_speed < 1) {												/* NVMe: max_link_speed 미설정 시 */
		pci->max_link_speed = FIELD_GET(PCI_EXP_LNKCAP_SLS, cap);						/* NVMe: 하드웨어 기본 최대 속도 캐시, NVMe 성능 추정 */
		return;																/* NVMe: 별도 제한 없음 */
	}

	ctrl2 = dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCTL2);							/* NVMe: Link Control 2 레지스터 읽기 */
	ctrl2 &= ~PCI_EXP_LNKCTL2_TLS;												/* NVMe: Target Link Speed 필드 클리어 */

	switch (pcie_get_link_speed(pci->max_link_speed)) {								/* NVMe: 원하는 NVMe 링크 속도 변환 */
	case PCIE_SPEED_2_5GT:													/* NVMe: Gen1 2.5 GT/s */
		link_speed = PCI_EXP_LNKCTL2_TLS_2_5GT;									/* NVMe: Gen1 target speed 코드 */
		break;																/* NVMe: switch case 종료 */
	case PCIE_SPEED_5_0GT:													/* NVMe: Gen2 5.0 GT/s */
		link_speed = PCI_EXP_LNKCTL2_TLS_5_0GT;									/* NVMe: Gen2 target speed 코드 */
		break;																/* NVMe: switch case 종료 */
	case PCIE_SPEED_8_0GT:													/* NVMe: Gen3 8.0 GT/s */
		link_speed = PCI_EXP_LNKCTL2_TLS_8_0GT;									/* NVMe: Gen3 target speed 코드 */
		break;																/* NVMe: switch case 종료 */
	case PCIE_SPEED_16_0GT:												/* NVMe: Gen4 16.0 GT/s */
		link_speed = PCI_EXP_LNKCTL2_TLS_16_0GT;								/* NVMe: Gen4 target speed 코드 */
		break;																/* NVMe: switch case 종료 */
	default:
		/* Use hardware capability */
		link_speed = FIELD_GET(PCI_EXP_LNKCAP_SLS, cap);							/* NVMe: 알 수 없으면 하드웨어 능력 사용, NVMe 안정성 우선 */
		ctrl2 &= ~PCI_EXP_LNKCTL2_HASD;											/* NVMe: Hardware Autonomous Speed Disable 클리어 */
		break;																/* NVMe: switch case 종료 */
	}

	/* NVMe: Link Control2에 목표 속도 기록, NVMe 속도 협상 */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCTL2, ctrl2 | link_speed);

	cap &= ~((u32)PCI_EXP_LNKCAP_SLS);											/* NVMe: Link Capability Supported Speeds 클리어 */
	dw_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, cap | link_speed);					/* NVMe: 지원 속도 필드 업데이트, NVMe UEFI/드라이버에 노출 */

}

int dw_pcie_link_get_max_link_width(struct dw_pcie *pci)							/* PCI/NVMe: PCIe 링크 최대 폭 조회, NVMe x4/x8/x16 가능 폭 */
{
	u8 cap = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);							/* NVMe: PCIe Express capability 오프셋 */
	u32 lnkcap = dw_pcie_readl_dbi(pci, cap + PCI_EXP_LNKCAP);						/* NVMe: Link Capability 레지스터 읽기 */

	return FIELD_GET(PCI_EXP_LNKCAP_MLW, lnkcap);									/* NVMe: Maximum Link Width 반환, NVMe lane 수 */
}

/* PCI/NVMe: PCIe 링크 폭 설정, NVMe 사용 가능 lane 수 제한 */
static void dw_pcie_link_set_max_link_width(struct dw_pcie *pci, u32 num_lanes)
{
	u32 lnkcap, lwsc, plc;													/* NVMe: Link Capability, Width/Speed Control, Port Link Control */
	u8 cap;																	/* NVMe: PCIe Express capability 오프셋 */

	if (!num_lanes)															/* NVMe: lane 수가 0이면 */
		return;																/* NVMe: 아무 것도 안 함, 하드웨어 기본값 사용 */

	/* Set the number of lanes */
	plc = dw_pcie_readl_dbi(pci, PCIE_PORT_LINK_CONTROL);								/* NVMe: Port Link Control 레지스터 읽기 */
	plc &= ~PORT_LINK_FAST_LINK_MODE;												/* NVMe: Fast Link Mode 비트 클리어, 정상 training */
	plc &= ~PORT_LINK_MODE_MASK;													/* NVMe: 기존 lane mode 필드 클리어 */

	/* Set link width speed control register */
	lwsc = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);						/* NVMe: Link Width/Speed Control 레지스터 읽기 */
	lwsc &= ~PORT_LOGIC_LINK_WIDTH_MASK;											/* NVMe: 기존 link width 필드 클리어 */
	lwsc |= PORT_LOGIC_LINK_WIDTH_1_LANES;											/* NVMe: 기본 1 lane 설정 후 덮어씀 */
	switch (num_lanes) {															/* NVMe: 요청 lane 수에 따라 */
	case 1:
		plc |= PORT_LINK_MODE_1_LANES;											/* NVMe: x1 모드, 저가 NVMe 또는 M.2 B+M key */
		break;																/* NVMe: case 종료 */
	case 2:
		plc |= PORT_LINK_MODE_2_LANES;											/* NVMe: x2 모드 */
		break;																/* NVMe: case 종료 */
	case 4:
		plc |= PORT_LINK_MODE_4_LANES;											/* NVMe: x4 모드, 일반 M.2 NVMe */
		break;																/* NVMe: case 종료 */
	case 8:
		plc |= PORT_LINK_MODE_8_LANES;											/* NVMe: x8 모드, 고성능 NVMe */
		break;																/* NVMe: case 종료 */
	case 16:
		plc |= PORT_LINK_MODE_16_LANES;											/* NVMe: x16 모드, 데이터센터 NVMe */
		break;																/* NVMe: case 종료 */
	default:
		dev_err(pci->dev, "num-lanes %u: invalid value\n", num_lanes);					/* NVMe: 잘못된 lane 수 로그, NVMe 초기화 실패 가능 */
		return;																/* NVMe: 설정 중단 */
	}
	dw_pcie_writel_dbi(pci, PCIE_PORT_LINK_CONTROL, plc);								/* NVMe: Port Link Control에 lane mode 기록 */
	/* NVMe: Link Width/Speed Control에 lane 설정 기록, NVMe link training */
	dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, lwsc);

	cap = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);								/* NVMe: PCIe Express capability 오프셋 */
	lnkcap = dw_pcie_readl_dbi(pci, cap + PCI_EXP_LNKCAP);							/* NVMe: Link Capability 레지스터 읽기 */
	lnkcap &= ~PCI_EXP_LNKCAP_MLW;													/* NVMe: Maximum Link Width 필드 클리어 */
	lnkcap |= FIELD_PREP(PCI_EXP_LNKCAP_MLW, num_lanes);								/* NVMe: 요청 lane 수로 Maximum Link Width 설정 */
	dw_pcie_writel_dbi(pci, cap + PCI_EXP_LNKCAP, lnkcap);								/* NVMe: Link Capability 업데이트, NVMe에 노출되는 폭 */
}

void dw_pcie_iatu_detect(struct dw_pcie *pci)										/* PCI/NVMe: iATU 윈도우 수/형태 자동 탐지, NVMe 메모리/config 경로 확보 */
{
	int max_region, ob, ib;														/* NVMe: 최대 region 수, outbound/inbound 탐색 카운트 */
	u32 val, min, dir;															/* NVMe: 레지스터 값, 최소 limit, 방향 */
	u64 max;																/* NVMe: 상위 limit 최대값 */

	val = dw_pcie_readl_dbi(pci, PCIE_ATU_VIEWPORT);								/* NVMe: ATU viewport 레지스터 읽기, unroll 여부 판단 */
	if (val == 0xFFFFFFFF) {														/* NVMe: viewport 읽기가 all-1이면 unrolled ATU */
		dw_pcie_cap_set(pci, IATU_UNROLL);											/* NVMe: unroll capability 설정 */

		max_region = min((int)pci->atu_size / 512, 256);								/* NVMe: unroll region 최대 개수 계산, 최대 256 */
	} else {
		pci->atu_base = pci->dbi_base + PCIE_ATU_VIEWPORT_BASE;						/* NVMe: viewport 방식 ATU 기본 주소 설정 */
		pci->atu_size = PCIE_ATU_VIEWPORT_SIZE;										/* NVMe: viewport ATU 크기 설정 */

		dw_pcie_writel_dbi(pci, PCIE_ATU_VIEWPORT, 0xFF);								/* NVMe: viewport에 0xFF 써서 최대 인덱스 프로브 */
		max_region = dw_pcie_readl_dbi(pci, PCIE_ATU_VIEWPORT) + 1;						/* NVMe: 지원 region 수 = 읽은 값 + 1 */
	}

	for (ob = 0; ob < max_region; ob++) {												/* NVMe: outbound 윈도우 개수 프로브 */
		dw_pcie_writel_atu_ob(pci, ob, PCIE_ATU_LOWER_TARGET, 0x11110000);					/* NVMe: 테스트 패턴 쓰기 */
		val = dw_pcie_readl_atu_ob(pci, ob, PCIE_ATU_LOWER_TARGET);						/* NVMe: 값 다시 읽기 */
		if (val != 0x11110000)														/* NVMe: 쓰기가 반영되지 않으면 */
			break;																/* NVMe: 해당 인덱스는 유효하지 않음 */
	}

	for (ib = 0; ib < max_region; ib++) {												/* NVMe: inbound 윈도우 개수 프로브 */
		dw_pcie_writel_atu_ib(pci, ib, PCIE_ATU_LOWER_TARGET, 0x11110000);					/* NVMe: 테스트 패턴 쓰기 */
		val = dw_pcie_readl_atu_ib(pci, ib, PCIE_ATU_LOWER_TARGET);						/* NVMe: 값 다시 읽기 */
		if (val != 0x11110000)														/* NVMe: 쓰기 반영 안 됨 */
			break;																/* NVMe: 유효 inbound 수 확정 */
	}

	if (ob) {																	/* NVMe: outbound 윈도우가 하나 이상 있음 */
		dir = PCIE_ATU_REGION_DIR_OB;												/* NVMe: outbound 방향으로 limit 프로브 */
	} else if (ib) {																/* NVMe: inbound 윈도우가 하나 이상 있음 */
		dir = PCIE_ATU_REGION_DIR_IB;												/* NVMe: inbound 방향으로 limit 프로브 */
	} else {
		dev_err(pci->dev, "No iATU regions found\n");									/* NVMe: ATU region 없음, NVMe 메모리/config 접근 불가 */
		return;																	/* NVMe: 탐색 중단, NVMe 초기화 실패 */
	}

	dw_pcie_writel_atu(pci, dir, 0, PCIE_ATU_LIMIT, 0x0);								/* NVMe: limit=0으로 써서 하드웨어가 허용하는 최소값 읽기 */
	min = dw_pcie_readl_atu(pci, dir, 0, PCIE_ATU_LIMIT);								/* NVMe: ATU 최소 region 크기/정렬 정보 획득 */

	if (dw_pcie_ver_is_ge(pci, 460A)) {												/* NVMe: v4.60a 이상 상위 limit 레지스터 존재 */
		dw_pcie_writel_atu(pci, dir, 0, PCIE_ATU_UPPER_LIMIT, 0xFFFFFFFF);					/* NVMe: 상위 limit 레지스터 프로브 */
		max = dw_pcie_readl_atu(pci, dir, 0, PCIE_ATU_UPPER_LIMIT);						/* NVMe: 상위 limit 지원 범위 읽기 */
	} else {
		max = 0;																/* NVMe: 구버전은 상위 limit 없음, 4GB 제한 */
	}

	pci->num_ob_windows = ob;														/* NVMe: 사용 가능 outbound 윈도우 수 저장, NVMe BAR/CFG/MSG 매핑용 */
	pci->num_ib_windows = ib;														/* NVMe: 사용 가능 inbound 윈도우 수 저장, NVMe EP BAR 매핑용 */
	pci->region_align = 1 << fls(min);												/* NVMe: ATU region 정렬 크기 계산, NVMe DMA/메모리 매핑 정렬 기준 */
	pci->region_limit = (max << 32) | (SZ_4G - 1);										/* NVMe: ATU region 최대 한계 설정, 64비트 NVMe BAR 지원 */

	/* NVMe: iATU 구성 정보 로그, NVMe 메모리 매핑 디버깅 */
	dev_info(pci->dev, "iATU: unroll %s, %u ob, %u ib, align %uK, limit %lluG\n",
		 dw_pcie_cap_is(pci, IATU_UNROLL) ? "T" : "F",									/* NVMe: unroll 여부 */
		 pci->num_ob_windows, pci->num_ib_windows,									/* NVMe: outbound/inbound 윈도우 수 */
		 pci->region_align / SZ_1K, (pci->region_limit + 1) / SZ_1G);					/* NVMe: 정렬/한계 크기 */
}

static u32 dw_pcie_readl_dma(struct dw_pcie *pci, u32 reg)							/* PCI/NVMe: eDMA 레지스터 읽기, NVMe 데이터 이동 offload */
{
	u32 val = 0;																/* NVMe: 읽은 값 초기화 */
	int ret;																	/* NVMe: dw_pcie_read 반환값 */

	if (pci->ops && pci->ops->read_dbi)											/* NVMe: glue driver가 read_dbi를 eDMA용으로 재정의 */
		return pci->ops->read_dbi(pci, pci->edma.reg_base, reg, 4);					/* NVMe: SoC 특화 eDMA 레지스터 읽기 */

	ret = dw_pcie_read(pci->edma.reg_base + reg, 4, &val);							/* NVMe: eDMA 레지스터 4바이트 읽기 */
	if (ret)																	/* NVMe: 읽기 실패 시 */
		dev_err(pci->dev, "Read DMA address failed\n");								/* NVMe: NVMe eDMA 제어 실패 로그 */

	return val;																	/* NVMe: eDMA 레지스터 값 반환 */
}

/* PCI/NVMe: eDMA 채널별 IRQ 번호 획득, NVMe DMA 완료 인터럽트 */
static int dw_pcie_edma_irq_vector(struct device *dev, unsigned int nr)
{
	struct platform_device *pdev = to_platform_device(dev);							/* NVMe: platform_device 변환 */
	char name[6];																/* NVMe: per-channel IRQ 이름 버퍼 */
	int ret;																	/* NVMe: IRQ 번호 */

	if (nr >= EDMA_MAX_WR_CH + EDMA_MAX_RD_CH)										/* NVMe: 채널 번호가 최대치 초과 */
		return -EINVAL;															/* NVMe: 잘못된 eDMA 채널 */

	ret = platform_get_irq_byname_optional(pdev, "dma");								/* NVMe: 공유 "dma" IRQ 이름으로 조회 */
	if (ret > 0)																	/* NVMe: 공유 IRQ가 존재하면 */
		return ret;																/* NVMe: 공유 IRQ 번호 반환, NVMe DMA 완료 통합 처리 */

	snprintf(name, sizeof(name), "dma%u", nr);											/* NVMe: 채널별 IRQ 이름 생성, 예: dma0 */

	return platform_get_irq_byname_optional(pdev, name);								/* NVMe: 채널별 IRQ 번호 반환, NVMe DMA 완료 분산 처리 */
}

static struct dw_edma_plat_ops dw_pcie_edma_ops = {									/* PCI/NVMe: eDMA 플랫폼 연산 구조체, NVMe DMA offload 연결 */
	.irq_vector = dw_pcie_edma_irq_vector,											/* NVMe: 채널 IRQ 획득 콜백 등록 */
};

static void dw_pcie_edma_init_data(struct dw_pcie *pci)								/* PCI/NVMe: eDMA 초기 데이터 설정, NVMe DMA 엔진 준비 */
{
	pci->edma.dev = pci->dev;														/* NVMe: eDMA 장치를 PCIe RC 장치로 설정, DMA coherence/IOMMU 연결 */

	if (!pci->edma.ops)																/* NVMe: glue driver가 ops를 미리 설정하지 않은 경우 */
		pci->edma.ops = &dw_pcie_edma_ops;											/* NVMe: 기본 eDMA ops 사용, NVMe DMA IRQ 연결 */

	pci->edma.flags |= DW_EDMA_CHIP_LOCAL;											/* NVMe: 로컬 eDMA 엔진임을 표시, NVMe offload 가능 */
}

static int dw_pcie_edma_find_mf(struct dw_pcie *pci)									/* PCI/NVMe: eDMA 레지스터 매핑 형식 탐지, NVMe DMA 경로 확인 */
{
	u32 val;																	/* NVMe: DMA control 레지스터 값 */

	/*
	 * Bail out finding the mapping format if it is already set by the glue
	 * driver. Also ensure that the edma.reg_base is pointing to a valid
	 * memory region.
	 */
	if (pci->edma.mf != EDMA_MF_EDMA_LEGACY)											/* NVMe: glue driver가 이미 mapping format을 설정 */
		return pci->edma.reg_base ? 0 : -ENODEV;									/* NVMe: reg_base 유효하면 탐색 완료, 아니면 실패 */

	/*
	 * Indirect eDMA CSRs access has been completely removed since v5.40a
	 * thus no space is now reserved for the eDMA channels viewport and
	 * former DMA CTRL register is no longer fixed to FFs.
	 */
	if (dw_pcie_ver_is_ge(pci, 540A))												/* NVMe: v5.40a 이상은 간접 eDMA 제거 */
		val = 0xFFFFFFFF;														/* NVMe: 강제로 unroll 형식 간주, NVMe DMA 레지스터 직접 접근 */
	else
		val = dw_pcie_readl_dbi(pci, PCIE_DMA_VIEWPORT_BASE + PCIE_DMA_CTRL);			/* NVMe: 레거시 DMA CTRL 레지스터 읽기 */

	if (val == 0xFFFFFFFF && pci->edma.reg_base) {										/* NVMe: unrolled eDMA이고 reg_base가 유효 */
		pci->edma.mf = EDMA_MF_EDMA_UNROLL;											/* NVMe: unroll mapping format 설정, NVMe DMA 직접 제어 */
	} else if (val != 0xFFFFFFFF) {													/* NVMe: 레거시 간접 eDMA 존재 */
		pci->edma.mf = EDMA_MF_EDMA_LEGACY;											/* NVMe: 레거시 mapping format 설정 */

		/* NVMe: DBI 내 eDMA viewport 기본 주소 설정, NVMe DMA 간접 제어 */
		pci->edma.reg_base = pci->dbi_base + PCIE_DMA_VIEWPORT_BASE;
	} else {
		return -ENODEV;																/* NVMe: eDMA 없음, NVMe 데이터 이동은 시스템 DMA/CPU로 폴크백 */
	}

	return 0;																	/* NVMe: eDMA mapping format 탐색 성공 */
}

/* PCI/NVMe: eDMA 읽기/쓰기 채널 수 자동 탐지, NVMe DMA 큐 용량 결정 */
static int dw_pcie_edma_find_channels(struct dw_pcie *pci)
{
	u32 val;																	/* NVMe: DMA CTRL 레지스터 값 */

	/*
	 * Autodetect the read/write channels count only for non-HDMA platforms.
	 * HDMA platforms with native CSR mapping doesn't support autodetect,
	 * so the glue drivers should've passed the valid count already. If not,
	 * the below sanity check will catch it.
	 */
	if (pci->edma.mf != EDMA_MF_HDMA_NATIVE) {											/* NVMe: HDMA native가 아닌 경우에만 자동 탐지 */
		val = dw_pcie_readl_dma(pci, PCIE_DMA_CTRL);									/* NVMe: eDMA control 레지스터 읽기 */

		pci->edma.ll_wr_cnt = FIELD_GET(PCIE_DMA_NUM_WR_CHAN, val);						/* NVMe: 쓰기 채널 수 추출, NVMe 쓰기 DMA 처리량 */
		pci->edma.ll_rd_cnt = FIELD_GET(PCIE_DMA_NUM_RD_CHAN, val);						/* NVMe: 읽기 채널 수 추출, NVMe 읽기 DMA 처리량 */
	}

	/* Sanity check the channels count if the mapping was incorrect */
	if (!pci->edma.ll_wr_cnt || pci->edma.ll_wr_cnt > EDMA_MAX_WR_CH ||				/* NVMe: 쓰기 채널 수 범위 위반 */
	    !pci->edma.ll_rd_cnt || pci->edma.ll_rd_cnt > EDMA_MAX_RD_CH)					/* NVMe: 읽기 채널 수 범위 위반 */
		return -EINVAL;																/* NVMe: 잘못된 eDMA 채널 수, NVMe DMA offload 불가 */

	return 0;																	/* NVMe: eDMA 채널 탐색 성공 */
}

static int dw_pcie_edma_find_chip(struct dw_pcie *pci)									/* PCI/NVMe: eDMA 엔진 탐색 및 초기화, NVMe DMA offload 준비 */
{
	int ret;																	/* NVMe: 반환값 */

	dw_pcie_edma_init_data(pci);														/* NVMe: eDMA 기본 데이터 초기화 */

	ret = dw_pcie_edma_find_mf(pci);													/* NVMe: eDMA mapping format 탐색 */
	if (ret)																	/* NVMe: mapping format 탐색 실패 */
		return ret;																/* NVMe: eDMA 없음 또는 설정 오류 */

	return dw_pcie_edma_find_channels(pci);												/* NVMe: 채널 수 탐색 결과 반환 */
}

static int dw_pcie_edma_irq_verify(struct dw_pcie *pci)								/* PCI/NVMe: eDMA IRQ 구성 검증, NVMe DMA 완료 인터럽트 정합성 */
{
	struct platform_device *pdev = to_platform_device(pci->dev);						/* NVMe: platform_device 변환 */
	u16 ch_cnt = pci->edma.ll_wr_cnt + pci->edma.ll_rd_cnt;							/* NVMe: 전체 eDMA 채널 수, NVMe DMA 큐 수 */
	char name[15];																/* NVMe: IRQ 이름 버퍼 */
	int ret;																	/* NVMe: IRQ 번호/결과 */

	if (pci->edma.nr_irqs > 1)														/* NVMe: 이미 IRQ 수가 설정된 경우 */
		return pci->edma.nr_irqs != ch_cnt ? -EINVAL : 0;							/* NVMe: 채널 수와 IRQ 수가 일치해야 NVMe DMA 정상 처리 */

	ret = platform_get_irq_byname_optional(pdev, "dma");								/* NVMe: 공유 "dma" IRQ 조회 */
	if (ret > 0) {																	/* NVMe: 공유 IRQ 존재 */
		pci->edma.nr_irqs = 1;														/* NVMe: 단일 IRQ로 NVMe DMA 처리 */
		return 0;																/* NVMe: IRQ 검증 성공 */
	}

	for (; pci->edma.nr_irqs < ch_cnt; pci->edma.nr_irqs++) {							/* NVMe: 채널 수만큼 개별 IRQ 등록 시도 */
		snprintf(name, sizeof(name), "dma%d", pci->edma.nr_irqs);						/* NVMe: dma0, dma1, ... 이름 생성 */

		ret = platform_get_irq_byname_optional(pdev, name);							/* NVMe: 개별 IRQ 조회 */
		if (ret <= 0)																/* NVMe: 개별 IRQ 없음 */
			return -EINVAL;														/* NVMe: IRQ 구성 불일치, NVMe DMA offload 제한 */
	}

	return 0;																	/* NVMe: eDMA IRQ 검증 성공 */
}

/* PCI/NVMe: eDMA Linked List 메모리 할당, NVMe DMA descriptor 저장 */
static int dw_pcie_edma_ll_alloc(struct dw_pcie *pci)
{
	struct dw_edma_region *ll;														/* NVMe: linked list region 포인터 */
	dma_addr_t paddr;																/* NVMe: 할당받은 DMA 물리 주소, IOMMU/DMA 매핑 */
	int i;																		/* NVMe: 채널 인덱스 */

	for (i = 0; i < pci->edma.ll_wr_cnt; i++) {										/* NVMe: 각 쓰기 채널에 대해 */
		ll = &pci->edma.ll_region_wr[i];											/* NVMe: 쓰기 채널 linked list region */
		ll->sz = DMA_LLP_MEM_SIZE;													/* NVMe: linked list 메모리 크기 PAGE_SIZE */
		ll->vaddr.mem = dmam_alloc_coherent(pci->dev, ll->sz,							/* NVMe: coherent DMA 메모리 할당, NVMe DMA descriptor 일관성 */
							    &paddr, GFP_KERNEL);								/* NVMe: GFP_KERNEL 컨텍스트, DMA 주소 반환 */
		if (!ll->vaddr.mem)															/* NVMe: 할당 실패 시 */
			return -ENOMEM;														/* NVMe: NVMe eDMA descriptor 메모리 부족 */

		ll->paddr = paddr;															/* NVMe: descriptor DMA 물리 주소 저장, eDMA 엔진에 프로그래밍 */
	}

	for (i = 0; i < pci->edma.ll_rd_cnt; i++) {										/* NVMe: 각 읽기 채널에 대해 */
		ll = &pci->edma.ll_region_rd[i];											/* NVMe: 읽기 채널 linked list region */
		ll->sz = DMA_LLP_MEM_SIZE;													/* NVMe: linked list 메모리 크기 */
		ll->vaddr.mem = dmam_alloc_coherent(pci->dev, ll->sz,							/* NVMe: coherent DMA 메모리 할당, NVMe 읽기 descriptor */
							    &paddr, GFP_KERNEL);								/* NVMe: DMA 주소 반환 */
		if (!ll->vaddr.mem)															/* NVMe: 할당 실패 시 */
			return -ENOMEM;														/* NVMe: NVMe eDMA descriptor 메모리 부족 */

		ll->paddr = paddr;															/* NVMe: 읽기 descriptor DMA 물리 주소 저장 */
	}

	return 0;																	/* NVMe: eDMA linked list 메모리 할당 성공 */
}

int dw_pcie_edma_detect(struct dw_pcie *pci)											/* PCI/NVMe: eDMA 엔진 탐색/등록, NVMe 데이터 경로 offload 선택 */
{
	int ret;																	/* NVMe: 반환값 */

	/* Don't fail if no eDMA was found (for the backward compatibility) */
	ret = dw_pcie_edma_find_chip(pci);													/* NVMe: eDMA 칩 탐색 */
	if (ret)																	/* NVMe: eDMA가 없거나 설정 오류 */
		return 0;																/* NVMe: eDMA 없이도 NVMe 동작 가능(폴크백) */

	/* Don't fail on the IRQs verification (for the backward compatibility) */
	ret = dw_pcie_edma_irq_verify(pci);													/* NVMe: eDMA IRQ 구성 검증 */
	if (ret) {																	/* NVMe: IRQ 검증 실패 */
		dev_err(pci->dev, "Invalid eDMA IRQs found\n");								/* NVMe: eDMA IRQ 불일치 로그, NVMe DMA offload 미사용 */
		return 0;																/* NVMe: eDMA 없이 진행, NVMe 정상 동작 */
	}

	ret = dw_pcie_edma_ll_alloc(pci);													/* NVMe: eDMA linked list 메모리 할당 */
	if (ret) {																	/* NVMe: 메모리 할당 실패 */
		dev_err(pci->dev, "Couldn't allocate LLP memory\n");							/* NVMe: descriptor 메모리 부족 로그 */
		return ret;																/* NVMe: 치명적 오류, NVMe 초기화 실패 */
	}

	/* Don't fail if the DW eDMA driver can't find the device */
	ret = dw_edma_probe(&pci->edma);													/* NVMe: DesignWare eDMA 드라이버에 등록, NVMe DMA offload 활성화 */
	if (ret && ret != -ENODEV) {														/* NVMe: eDMA 등록 실패(-ENODEV 제외) */
		dev_err(pci->dev, "Couldn't register eDMA device\n");							/* NVMe: eDMA 등록 오류 로그 */
		return ret;																/* NVMe: NVMe 초기화 실패 */
	}

	dev_info(pci->dev, "eDMA: unroll %s, %hu wr, %hu rd\n",							/* NVMe: eDMA 구성 정보 로그, NVMe DMA offload 여부 */
		 pci->edma.mf == EDMA_MF_EDMA_UNROLL ? "T" : "F",								/* NVMe: unroll 여부 */
		 pci->edma.ll_wr_cnt, pci->edma.ll_rd_cnt);									/* NVMe: 쓰기/읽기 채널 수, NVMe 처리량 힌트 */

	return 0;																	/* NVMe: eDMA 탐색/등록 완료(성공 또는 폴크백) */
}

void dw_pcie_edma_remove(struct dw_pcie *pci)											/* PCI/NVMe: eDMA 드라이버 제거, NVMe DMA offload 해제 */
{
	dw_edma_remove(&pci->edma);															/* NVMe: eDMA 드라이버 등록 해제, NVMe DMA 리소스 정리 */
}

/* PCI/NVMe: 지원하지 않는 L1 PM Substates 숨김, NVMe ASPM 호환성 */
void dw_pcie_hide_unsupported_l1ss(struct dw_pcie *pci)
{
	u16 l1ss;																	/* NVMe: L1SS extended capability 오프셋 */
	u32 l1ss_cap;																/* NVMe: L1SS Capability 레지스터 값 */

	if (pci->l1ss_support)															/* NVMe: 플랫폼이 L1SS 명시 지원 */
		return;																	/* NVMe: L1SS 노출 유지, NVMe ASPM L1.1/L1.2 가능 */

	l1ss = dw_pcie_find_ext_capability(pci, PCI_EXT_CAP_ID_L1SS);						/* NVMe: L1SS 확장 캡 탐색 */
	if (!l1ss)																	/* NVMe: L1SS 캡이 없으면 */
		return;																	/* NVMe: 할 것 없음, NVMe ASPM L1SS 미사용 */

	/*
	 * Unless the driver claims "l1ss_support", don't advertise L1 PM
	 * Substates because they require CLKREQ# and possibly other
	 * device-specific configuration.
	 */
	l1ss_cap = dw_pcie_readl_dbi(pci, l1ss + PCI_L1SS_CAP);							/* NVMe: L1SS Capability 레지스터 읽기 */
	l1ss_cap &= ~(PCI_L1SS_CAP_PCIPM_L1_1 | PCI_L1SS_CAP_ASPM_L1_1 |					/* NVMe: L1.1 지원 비트 클리어, NVMe ASPM L1.1 미광고 */
		      PCI_L1SS_CAP_PCIPM_L1_2 | PCI_L1SS_CAP_ASPM_L1_2 |					/* NVMe: L1.2 지원 비트 클리어, NVMe ASPM L1.2 미광고 */
		      PCI_L1SS_CAP_L1_PM_SS);												/* NVMe: L1 PM Substates 지원 비트 클리어 */
	dw_pcie_writel_dbi(pci, l1ss + PCI_L1SS_CAP, l1ss_cap);							/* NVMe: 수정된 L1SS Capability 기록, NVMe에 잘못된 ASPM 노출 방지 */
}

void dw_pcie_setup(struct dw_pcie *pci)													/* PCI/NVMe: PCIe RC 기본 설정, NVMe 열거 전 링크/동작 구성 */
{
	u32 val;																	/* NVMe: 임시 레지스터 값 */

	dw_pcie_link_set_max_speed(pci);													/* NVMe: PCIe 최대 링크 속도 설정, NVMe Gen 협상 */

	/* Configure Gen1 N_FTS */
	if (pci->n_fts[0]) {																/* NVMe: Gen1 N_FTS 값이 지정된 경우 */
		val = dw_pcie_readl_dbi(pci, PCIE_PORT_AFR);									/* NVMe: Ack Frequency 레지스터 읽기 */
		val &= ~(PORT_AFR_N_FTS_MASK | PORT_AFR_CC_N_FTS_MASK);							/* NVMe: 기존 N_FTS/CC_N_FTS 필드 클리어 */
		val |= PORT_AFR_N_FTS(pci->n_fts[0]);											/* NVMe: Gen1 N_FTS 설정, NVMe L0s 복구 시간 조정 */
		val |= PORT_AFR_CC_N_FTS(pci->n_fts[0]);										/* NVMe: common clock N_FTS 설정 */
		dw_pcie_writel_dbi(pci, PCIE_PORT_AFR, val);									/* NVMe: Ack Frequency 레지스터 기록, NVMe 링크 안정성 */
	}

	/* Configure Gen2+ N_FTS */
	if (pci->n_fts[1]) {																/* NVMe: Gen2+ N_FTS 값이 지정된 경우 */
		val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);						/* NVMe: Link Width/Speed Control 읽기 */
		val &= ~PORT_LOGIC_N_FTS_MASK;												/* NVMe: 기존 N_FTS 필드 클리어 */
		val |= pci->n_fts[1];														/* NVMe: Gen2+ N_FTS 설정, NVMe ASPM/L0s 복구 최적화 */
		dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);						/* NVMe: Link Width/Speed Control 기록 */
	}

	if (dw_pcie_cap_is(pci, CDM_CHECK)) {												/* NVMe: CDM 레지스터 무결성 검사 활성화 요청 */
		val = dw_pcie_readl_dbi(pci, PCIE_PL_CHK_REG_CONTROL_STATUS);						/* NVMe: PL Check Reg Control/Status 읽기 */
		val |= PCIE_PL_CHK_REG_CHK_REG_CONTINUOUS |									/* NVMe: 연속적 무결성 검사 */
		       PCIE_PL_CHK_REG_CHK_REG_START;										/* NVMe: 무결성 검사 시작, AER/오류 감지 강화 */
		dw_pcie_writel_dbi(pci, PCIE_PL_CHK_REG_CONTROL_STATUS, val);						/* NVMe: CDM check 설정 기록, NVMe config 레지스터 오류 감지 */
	}

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_LINK_CONTROL);								/* NVMe: Port Link Control 레지스터 읽기 */
	val &= ~PORT_LINK_FAST_LINK_MODE;												/* NVMe: Fast Link Mode 비트 클리어, 정상 link training */
	val |= PORT_LINK_DLL_LINK_EN;													/* NVMe: Data Link Layer link enable, NVMe TLP 교환 가능 */
	dw_pcie_writel_dbi(pci, PCIE_PORT_LINK_CONTROL, val);								/* NVMe: Port Link Control 기록, NVMe 링크 데이터 계층 활성화 */

	dw_pcie_link_set_max_link_width(pci, pci->num_lanes);								/* NVMe: PCIe 링크 폭 설정, NVMe lane 수 협상 */
}

/* PCI/NVMe: 상위 버스 주소 오프셋 계산, NVMe BAR/ATU 주소 변환 */
resource_size_t dw_pcie_parent_bus_offset(struct dw_pcie *pci,
					  const char *reg_name,
					  resource_size_t cpu_phys_addr)
{
	struct device *dev = pci->dev;														/* NVMe: PCIe RC 장치 구조체 */
	struct device_node *np = dev->of_node;												/* NVMe: DT 노드 */
	int index;																	/* NVMe: reg-names 배열 인덱스 */
	u64 reg_addr, fixup_addr;													/* NVMe: DT의 reg 주소, fixup 결과 주소 */
	u64 (*fixup)(struct dw_pcie *pcie, u64 cpu_addr);								/* NVMe: SoC 주소 fixup 콜백 */

	/* Look up reg_name address on parent bus */
	index = of_property_match_string(np, "reg-names", reg_name);							/* NVMe: reg-names에서 해당 이름의 인덱스 탐색 */

	if (index < 0) {																/* NVMe: reg-names에 이름이 없으면 */
		dev_err(dev, "No %s in devicetree \"reg\" property\n", reg_name);					/* NVMe: DT 오류 로그, NVMe BAR 주소 변환 실패 */
		return 0;																/* NVMe: 오프셋 0 반환, NVMe 메모리 매핑 오류 가능 */
	}

	of_property_read_reg(np, index, &reg_addr, NULL);									/* NVMe: DT reg의 상위 버스 주소 읽기 */

	fixup = pci->ops ? pci->ops->cpu_addr_fixup : NULL;								/* NVMe: SoC 특화 CPU 주소 fixup 콜백 획득 */
	if (fixup) {																	/* NVMe: fixup 콜백이 있으면 */
		fixup_addr = fixup(pci, cpu_phys_addr);											/* NVMe: CPU 물리 주소를 PCIe 상위 버스 주소로 변환, NVMe BAR target 계산 */
		if (reg_addr == fixup_addr) {												/* NVMe: DT와 fixup 결과 일치 */
			/* NVMe: 일치 정보 로그 */
			dev_info(dev, "%s reg[%d] %#010llx == %#010llx == fixup(cpu %#010llx); %ps is redundant with this devicetree\n",
				 reg_name, index, reg_addr, fixup_addr,								/* NVMe: 주소 값들 */
				 (unsigned long long) cpu_phys_addr, fixup);							/* NVMe: fixup 함수 포인터 */
		} else {
			/* NVMe: DT/fixup 불일치 경고, NVMe 주소 변환 오류 */
			dev_warn(dev, "%s reg[%d] %#010llx != %#010llx == fixup(cpu %#010llx); devicetree is broken\n",
				 reg_name, index, reg_addr, fixup_addr,								/* NVMe: 주소 값들 */
				 (unsigned long long) cpu_phys_addr);								/* NVMe: CPU 주소 */
			reg_addr = fixup_addr;												/* NVMe: fixup 결과를 우선 적용, NVMe 메모리 매핑 보정 */
		}

		return cpu_phys_addr - reg_addr;											/* NVMe: 상위 버스 기준 오프셋 반환, NVMe outbound ATU 설정에 사용 */
	}

	if (pci->use_parent_dt_ranges) {

		/*
		 * This platform once had a fixup, presumably because it
		 * translates between CPU and PCI controller addresses.
		 * Log a note if devicetree didn't describe a translation.
		 */
		if (reg_addr == cpu_phys_addr)												/* NVMe: DT와 CPU 주소가 동일하면 */
			/* NVMe: 변환 불필요 로그 */
			dev_info(dev, "%s reg[%d] %#010llx == cpu %#010llx\n; no fixup was ever needed for this devicetree\n",
				 reg_name, index, reg_addr,										/* NVMe: DT 주소 */
				 (unsigned long long) cpu_phys_addr);								/* NVMe: CPU 주소 */
	} else {
		if (reg_addr != cpu_phys_addr) {												/* NVMe: DT와 CPU 주소가 다륾 */
			/* NVMe: DT ranges 오류 경고, NVMe IOMMU/DMA 주소 문제 가능 */
			dev_warn(dev, "%s reg[%d] %#010llx != cpu %#010llx; no fixup and devicetree \"ranges\" is broken, assuming no translation\n",
				 reg_name, index, reg_addr,										/* NVMe: DT 주소 */
				 (unsigned long long) cpu_phys_addr);								/* NVMe: CPU 주소 */
			return 0;														/* NVMe: 변환 없음 가정, NVMe BAR 매핑 정확도 저하 */
		}
	}

	return cpu_phys_addr - reg_addr;													/* NVMe: 상위 버스 오프셋 반환, NVMe outbound/inbound ATU 계산에 사용 */
}
