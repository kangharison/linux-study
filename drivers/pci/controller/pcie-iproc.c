// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright (C) 2015 Broadcom Corporation
 */

#include <linux/kernel.h>	/* NVMe: 기본 커널 타입/매크로; NVMe PCIe host 공통 인프라 */
#include <linux/pci.h>	/* NVMe: PCI 버스/장치/리소스 정의; NVMe 열거 및 리소스 할당의 핵심 헤더 */
#include <linux/pci-ecam.h>	/* NVMe: ECAM 주소 매핑; NVMe config 공간 접근 시 사용 */
#include <linux/msi.h>	/* NVMe: MSI/MSI-X 인터럽트 정의; NVMe queue 인터럽트 처리의 기반 */
#include <linux/clk.h>	/* NVMe: PCIe controller 클럭 제어; NVMe 링크 안정성과 전력 상태 연관 */
#include <linux/module.h>	/* NVMe: module/platform driver 등록; NVMe PCIe 루트 브리지 로딩 */
#include <linux/mbus.h>	/* NVMe: SoC 내부 버스 매핑; NVMe DMA 주소 변환에 영향 */
#include <linux/slab.h>	/* NVMe: 동적 메모리 할당; NVMe host bridge 구조체 할당 시 사용 */
#include <linux/delay.h>	/* NVMe: PERST/링크 안정화 대기; NVMe 장치 reset 타이밍 제어 */
#include <linux/interrupt.h>	/* NVMe: IRQ 핸들러/인터럽트 관리; NVMe MSI-X 인터럽트 라우팅 */
#include <linux/irqchip/arm-gic-v3.h>	/* NVMe: ARM GICv3 ITS MSI 목적지; NVMe MSI를 ITS로 steering */
#include <linux/platform_device.h>	/* NVMe: DT/ACPI 기반 platform_device; SoC PCIe 루트 컴플렉스 등록 */
#include <linux/of_address.h>	/* NVMe: DT reg/주소 파싱; NVMe 컨트롤러 메모리 영역 획득 */
#include <linux/of_irq.h>	/* NVMe: DT 인터럽트 매핑; NVMe용 INTx/MSI 라우팅 파싱 */
#include <linux/of_pci.h>	/* NVMe: OF PCI 헬퍼; DT에서 PCI 버스 범위/리소스 파싱 */
#include <linux/of_platform.h>	/* NVMe: platform child device 등록; NVMe 컨트롤러 하위 장치 바인딩 */
#include <linux/phy/phy.h>	/* NVMe: PCIe PHY 초기화/파워; NVMe 링크 물리 계층 설정 */

#include "pcie-iproc.h"	/* NVMe: iProc PCIe host 내부 구조체/플래그; NVMe host bridge 상태 정의 */

#define EP_PERST_SOURCE_SELECT_SHIFT	2	/* NVMe: EP_PERST_SOURCE_SELECT_SHIFT 매크로 정의; PERST 소스 선택 비트 위치; NVMe EP reset 경로 설정 */
#define EP_PERST_SOURCE_SELECT		BIT(EP_PERST_SOURCE_SELECT_SHIFT)	/* NVMe: EP_PERST_SOURCE_SELECT 매크로 정의; PERST 소스 선택 마스크; NVMe 장치 reset 제어 */
#define EP_MODE_SURVIVE_PERST_SHIFT	1	/* NVMe: EP_MODE_SURVIVE_PERST_SHIFT 매크로 정의; PERST 후 EP 모드 유지 비트; NVMe 장치 생존 모드 제어 */
#define EP_MODE_SURVIVE_PERST		BIT(EP_MODE_SURVIVE_PERST_SHIFT)	/* NVMe: EP_MODE_SURVIVE_PERST 매크로 정의; PERST 생존 모드 마스크; NVMe EP reset 동작 제어 */
#define RC_PCIE_RST_OUTPUT_SHIFT	0	/* NVMe: RC_PCIE_RST_OUTPUT_SHIFT 매크로 정의; 루트 컴플렉스 PCIe reset 출력 비트; NVMe 링크 reset 제어 */
#define RC_PCIE_RST_OUTPUT		BIT(RC_PCIE_RST_OUTPUT_SHIFT)	/* NVMe: RC_PCIE_RST_OUTPUT 매크로 정의; RC reset 출력 마스크; NVMe 장치에 PERST# 신호 생성 */
#define PAXC_RESET_MASK			0x7f	/* NVMe: PAXC_RESET_MASK 매크로 정의; PAXC reset 마스크; 내부 NVMe EP(PAXC)용 reset 비트 */

#define GIC_V3_CFG_SHIFT		0	/* NVMe: GIC_V3_CFG_SHIFT 매크로 정의; GICv3 ITS MSI 모드 비트 위치; NVMe MSI steering 모드 설정 */
#define GIC_V3_CFG			BIT(GIC_V3_CFG_SHIFT)	/* NVMe: GIC_V3_CFG 매크로 정의; GICv3 ITS MSI 모드 마스크; NVMe MSI를 ITS로 전달 */

#define MSI_ENABLE_CFG_SHIFT		0	/* NVMe: MSI_ENABLE_CFG_SHIFT 매크로 정의; MSI enable 비트 위치; NVMe MSI/MSI-X 인터럽트 활성화 */
#define MSI_ENABLE_CFG			BIT(MSI_ENABLE_CFG_SHIFT)	/* NVMe: MSI_ENABLE_CFG 매크로 정의; MSI enable 마스크; NVMe MSI write 해석 활성화 */

#define CFG_IND_ADDR_MASK		0x00001ffc	/* NVMe: CFG_IND_ADDR_MASK 매크로 정의; RC 내부 config 주소 마스크; NVMe 루트 포트 config 접근 */

#define CFG_ADDR_REG_NUM_MASK		0x00000ffc	/* NVMe: CFG_ADDR_REG_NUM_MASK 매크로 정의; EP config 레지스터 번호 마스크; NVMe EP config offset 인코딩 */
#define CFG_ADDR_CFG_TYPE_1		1	/* NVMe: CFG_ADDR_CFG_TYPE_1 매크로 정의; Type 1 config 접근; NVMe 장치가 다운스트림 버스에 있을 때 사용 */

#define SYS_RC_INTX_MASK		0xf	/* NVMe: SYS_RC_INTX_MASK 매크로 정의; INTx A/B/C/D 마스크; NVMe legacy 인터럽트 enable 비트 */

#define PCIE_PHYLINKUP_SHIFT		3	/* NVMe: PCIE_PHYLINKUP_SHIFT 매크로 정의; PHY link up 상태 비트 위치; NVMe 물리 링크 감지 */
#define PCIE_PHYLINKUP			BIT(PCIE_PHYLINKUP_SHIFT)	/* NVMe: PCIE_PHYLINKUP 매크로 정의; PHY link up 마스크; NVMe SSD 물리 연결 확인 */
#define PCIE_DL_ACTIVE_SHIFT		2	/* NVMe: PCIE_DL_ACTIVE_SHIFT 매크로 정의; 데이터 링크 active 비트 위치; NVMe PCIe 데이터 링크 활성화 */
#define PCIE_DL_ACTIVE			BIT(PCIE_DL_ACTIVE_SHIFT)	/* NVMe: PCIE_DL_ACTIVE 매크로 정의; 데이터 링크 active 마스크; NVMe L0 상태 진입 확인 */

#define APB_ERR_EN_SHIFT		0	/* NVMe: APB_ERR_EN_SHIFT 매크로 정의; APB error enable 비트 위치; NVMe config 접근 중 bus error 제어 */
#define APB_ERR_EN			BIT(APB_ERR_EN_SHIFT)	/* NVMe: APB_ERR_EN 매크로 정의; APB error enable 마스크; multi-function NVMe 열거 시 예외 방지 */

#define CFG_RD_SUCCESS			0	/* NVMe: CFG_RD_SUCCESS 매크로 정의; config read 성공 상태; NVMe ID/STS 레지스터 읽기 성공 */
#define CFG_RD_UR			1	/* NVMe: CFG_RD_UR 매크로 정의; config read Unsupported Request; 존재하지 않는 NVMe function 접근 시 */
#define CFG_RD_RRS			2	/* NVMe: CFG_RD_RRS 매크로 정의; config read Request Retry Status; NVMe 장치가 아직 준비되지 않음 */
#define CFG_RD_CA			3	/* NVMe: CFG_RD_CA 매크로 정의; config read Completer Abort; NVMe EP 내부 오류 발생 */
#define CFG_RETRY_STATUS		0xffff0001	/* NVMe: CFG_RETRY_STATUS 매크로 정의; RRS 반환 시 하드웨어가 되돌린 특수 값; NVMe Vendor ID 읽기 시 주의 */
#define CFG_RETRY_STATUS_TIMEOUT_US	500000 /* 500 milliseconds */	/* NVMe: CFG_RETRY_STATUS_TIMEOUT_US 매크로 정의; RRS 소프트웨어 재시도 타임아웃; NVMe 열거 지연 대비 */

/* derive the enum index of the outbound/inbound mapping registers */
#define MAP_REG(base_reg, index)	((base_reg) + (index) * 2)	/* NVMe: MAP_REG 매크로 정의; OARR/OMAP 또는 IARR/IMAP 레지스터 인덱스 계산; NVMe 메모리 매핑 레지스터 접근 */

/*
 * Maximum number of outbound mapping window sizes that can be supported by any
 * OARR/OMAP mapping pair
 */
#define MAX_NUM_OB_WINDOW_SIZES		4	/* NVMe: MAX_NUM_OB_WINDOW_SIZES 매크로 정의; outbound 윈도우 크기 종류 최대값; NVMe BAR/MMIO 매핑 윈도우 설정 */

#define OARR_VALID_SHIFT		0	/* NVMe: OARR_VALID_SHIFT 매크로 정의; OARR valid 비트 위치; NVMe outbound 매핑 윈도우 활성화 */
#define OARR_VALID			BIT(OARR_VALID_SHIFT)	/* NVMe: OARR_VALID 매크로 정의; OARR valid 마스크; NVMe용 PCI MMIO가 SoC에서 PCI 주소로 매핑됨을 표시 */
#define OARR_SIZE_CFG_SHIFT		1	/* NVMe: OARR_SIZE_CFG_SHIFT 매크로 정의; OARR 크기 설정 비트 위치; NVMe MMIO 윈도우 크기 선택 */

/*
 * Maximum number of inbound mapping region sizes that can be supported by an
 * IARR
 */
#define MAX_NUM_IB_REGION_SIZES		9	/* NVMe: MAX_NUM_IB_REGION_SIZES 매크로 정의; inbound region 크기 종류 최대값; NVMe DMA/IO inbound 매핑 크기 선택 */

#define IMAP_VALID_SHIFT		0	/* NVMe: IMAP_VALID_SHIFT 매크로 정의; IMAP valid 비트 위치; NVMe inbound 매핑 윈도우 활성화 */
#define IMAP_VALID			BIT(IMAP_VALID_SHIFT)	/* NVMe: IMAP_VALID 매크로 정의; IMAP valid 마스크; PCIe에서 SoC 메모리로의 NVMe DMA/IO 경로 활성화 */

#define IPROC_PCI_PM_CAP		0x48	/* NVMe: IPROC_PCI_PM_CAP 매크로 정의; iProc PM capability offset; NVMe ASPM/전력 관리 capability 위치 */
#define IPROC_PCI_PM_CAP_MASK		0xffff	/* NVMe: IPROC_PCI_PM_CAP_MASK 매크로 정의; PM capability 워드 마스크; NVMe 전력 관리 capability 수정용 */
#define IPROC_PCI_EXP_CAP		0xac	/* NVMe: IPROC_PCI_EXP_CAP 매크로 정의; iProc PCIe capability offset; NVMe link/ASPM/AER capability 위치 */

#define IPROC_PCIE_REG_INVALID		0xffff	/* NVMe: IPROC_PCIE_REG_INVALID 매크로 정의; 미사용 레지스터 표시; NVMe controller 버전별 레지스터 존재 여부 구분 */

/**
 * struct iproc_pcie_ob_map - iProc PCIe outbound mapping controller-specific
 * parameters
 * @window_sizes: list of supported outbound mapping window sizes in MB
 * @nr_sizes: number of supported outbound mapping window sizes
 */
struct iproc_pcie_ob_map {	/* NVMe: iproc_pcie_ob_map 멤버; NVMe PCIe host 구조 정의 */
	resource_size_t window_sizes[MAX_NUM_OB_WINDOW_SIZES];	/* NVMe: window_sizes 멤버; outbound 윈도우 크기 배열; NVMe BAR/MMIO를 PCI 주소 공간에 매핑할 때 사용 */
	unsigned int nr_sizes;	/* NVMe: nr_sizes 멤버; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
};	/* NVMe: NVMe PCIe host controller 동작 */

static const struct iproc_pcie_ob_map paxb_ob_map[] = {	/* NVMe: paxb_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* OARR0/OMAP0 */
		.window_sizes = { 128, 256 },	/* NVMe: window_sizes 필드 초기화; outbound 윈도우 크기 배열; NVMe BAR/MMIO를 PCI 주소 공간에 매핑할 때 사용 */
		.nr_sizes = 2,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
	},	/* NVMe: paxb_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* OARR1/OMAP1 */
		.window_sizes = { 128, 256 },	/* NVMe: window_sizes 필드 초기화; outbound 윈도우 크기 배열; NVMe BAR/MMIO를 PCI 주소 공간에 매핑할 때 사용 */
		.nr_sizes = 2,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
	},	/* NVMe: paxb_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
};	/* NVMe: NVMe PCIe host controller 동작 */

static const struct iproc_pcie_ob_map paxb_v2_ob_map[] = {	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* OARR0/OMAP0 */
		.window_sizes = { 128, 256 },	/* NVMe: window_sizes 필드 초기화; outbound 윈도우 크기 배열; NVMe BAR/MMIO를 PCI 주소 공간에 매핑할 때 사용 */
		.nr_sizes = 2,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
	},	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* OARR1/OMAP1 */
		.window_sizes = { 128, 256 },	/* NVMe: window_sizes 필드 초기화; outbound 윈도우 크기 배열; NVMe BAR/MMIO를 PCI 주소 공간에 매핑할 때 사용 */
		.nr_sizes = 2,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
	},	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* OARR2/OMAP2 */
		.window_sizes = { 128, 256, 512, 1024 },	/* NVMe: window_sizes 필드 초기화; outbound 윈도우 크기 배열; NVMe BAR/MMIO를 PCI 주소 공간에 매핑할 때 사용 */
		.nr_sizes = 4,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
	},	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* OARR3/OMAP3 */
		.window_sizes = { 128, 256, 512, 1024 },	/* NVMe: window_sizes 필드 초기화; outbound 윈도우 크기 배열; NVMe BAR/MMIO를 PCI 주소 공간에 매핑할 때 사용 */
		.nr_sizes = 4,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
	},	/* NVMe: paxb_v2_ob_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/**
 * enum iproc_pcie_ib_map_type - iProc PCIe inbound mapping type
 * @IPROC_PCIE_IB_MAP_MEM: DDR memory
 * @IPROC_PCIE_IB_MAP_IO: device I/O memory
 * @IPROC_PCIE_IB_MAP_INVALID: invalid or unused
 */
enum iproc_pcie_ib_map_type {	/* NVMe: enum enumerator; NVMe PCIe host iproc_pcie_ib_map_type 식별자 */
	IPROC_PCIE_IB_MAP_MEM = 0,	/* NVMe: IPROC_PCIE_IB_MAP_MEM enumerator; DDR 메모리 inbound 매핑; NVMe DMA 버퍼가 타겟이 됨 */
	IPROC_PCIE_IB_MAP_IO,	/* NVMe: IPROC_PCIE_IB_MAP_IO enumerator; I/O 공간 inbound 매핑; NVMe MSI write/IO 영역 처리 */
	IPROC_PCIE_IB_MAP_INVALID	/* NVMe: IPROC_PCIE_IB_MAP_INVALID enumerator; 미사용/유효하지 않은 inbound 타입; NVMe 매핑 시 skip */
};	/* NVMe: NVMe PCIe host controller 동작 */

/**
 * struct iproc_pcie_ib_map - iProc PCIe inbound mapping controller-specific
 * parameters
 * @type: inbound mapping region type
 * @size_unit: inbound mapping region size unit, could be SZ_1K, SZ_1M, or
 * SZ_1G
 * @region_sizes: list of supported inbound mapping region sizes in KB, MB, or
 * GB, depending on the size unit
 * @nr_sizes: number of supported inbound mapping region sizes
 * @nr_windows: number of supported inbound mapping windows for the region
 * @imap_addr_offset: register offset between the upper and lower 32-bit
 * IMAP address registers
 * @imap_window_offset: register offset between each IMAP window
 */
struct iproc_pcie_ib_map {	/* NVMe: iproc_pcie_ib_map 멤버; NVMe PCIe host 구조 정의 */
	enum iproc_pcie_ib_map_type type;	/* NVMe: type 멤버; inbound 영역 타입; NVMe DMA(mem) 또는 MSI(io) 매핑 구분 */
	unsigned int size_unit;	/* NVMe: size_unit 멤버; inbound 크기 단위(KB/MB/GB); NVMe DMA/IO 영역 크기 정밀 설정 */
	resource_size_t region_sizes[MAX_NUM_IB_REGION_SIZES];	/* NVMe: region_sizes 멤버; 지원 inbound region 크기 배열; NVMe DMA 범위 선택 */
	unsigned int nr_sizes;	/* NVMe: nr_sizes 멤버; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
	unsigned int nr_windows;	/* NVMe: nr_windows 멤버; region 내 IMAP 윈도우 수; NVMe MSI 또는 DMA segmentation */
	u16 imap_addr_offset;	/* NVMe: imap_addr_offset 멤버; 상위/하위 IMAP 주소 레지스터 간 offset; NVMe 64-bit DMA/MSI 주소 설정 */
	u16 imap_window_offset;	/* NVMe: imap_window_offset 멤버; 연속 IMAP 윈도우 간 offset; NVMe MSI multiple vector 또는 DMA window 분할 */
};	/* NVMe: NVMe PCIe host controller 동작 */

static const struct iproc_pcie_ib_map paxb_v2_ib_map[] = {	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* IARR0/IMAP0 */
		.type = IPROC_PCIE_IB_MAP_IO,	/* NVMe: type 필드 초기화; inbound 영역 타입; NVMe DMA(mem) 또는 MSI(io) 매핑 구분 */
		.size_unit = SZ_1K,	/* NVMe: size_unit 필드 초기화; inbound 크기 단위(KB/MB/GB); NVMe DMA/IO 영역 크기 정밀 설정 */
		.region_sizes = { 32 },	/* NVMe: region_sizes 필드 초기화; 지원 inbound region 크기 배열; NVMe DMA 범위 선택 */
		.nr_sizes = 1,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
		.nr_windows = 8,	/* NVMe: nr_windows 필드 초기화; region 내 IMAP 윈도우 수; NVMe MSI 또는 DMA segmentation */
		.imap_addr_offset = 0x40,	/* NVMe: imap_addr_offset 필드 초기화; 상위/하위 IMAP 주소 레지스터 간 offset; NVMe 64-bit DMA/MSI 주소 설정 */
		.imap_window_offset = 0x4,	/* NVMe: imap_window_offset 필드 초기화; 연속 IMAP 윈도우 간 offset; NVMe MSI multiple vector 또는 DMA window 분할 */
	},	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* IARR1/IMAP1 */
		.type = IPROC_PCIE_IB_MAP_MEM,	/* NVMe: type 필드 초기화; inbound 영역 타입; NVMe DMA(mem) 또는 MSI(io) 매핑 구분 */
		.size_unit = SZ_1M,	/* NVMe: size_unit 필드 초기화; inbound 크기 단위(KB/MB/GB); NVMe DMA/IO 영역 크기 정밀 설정 */
		.region_sizes = { 8 },	/* NVMe: region_sizes 필드 초기화; 지원 inbound region 크기 배열; NVMe DMA 범위 선택 */
		.nr_sizes = 1,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
		.nr_windows = 8,	/* NVMe: nr_windows 필드 초기화; region 내 IMAP 윈도우 수; NVMe MSI 또는 DMA segmentation */
		.imap_addr_offset = 0x4,	/* NVMe: imap_addr_offset 필드 초기화; 상위/하위 IMAP 주소 레지스터 간 offset; NVMe 64-bit DMA/MSI 주소 설정 */
		.imap_window_offset = 0x8,	/* NVMe: imap_window_offset 필드 초기화; 연속 IMAP 윈도우 간 offset; NVMe MSI multiple vector 또는 DMA window 분할 */

	},	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* IARR2/IMAP2 */
		.type = IPROC_PCIE_IB_MAP_MEM,	/* NVMe: type 필드 초기화; inbound 영역 타입; NVMe DMA(mem) 또는 MSI(io) 매핑 구분 */
		.size_unit = SZ_1M,	/* NVMe: size_unit 필드 초기화; inbound 크기 단위(KB/MB/GB); NVMe DMA/IO 영역 크기 정밀 설정 */
		.region_sizes = { 64, 128, 256, 512, 1024, 2048, 4096, 8192,	/* NVMe: region_sizes 필드 초기화; 지원 inbound region 크기 배열; NVMe DMA 범위 선택 */
				  16384 },	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		.nr_sizes = 9,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
		.nr_windows = 1,	/* NVMe: nr_windows 필드 초기화; region 내 IMAP 윈도우 수; NVMe MSI 또는 DMA segmentation */
		.imap_addr_offset = 0x4,	/* NVMe: imap_addr_offset 필드 초기화; 상위/하위 IMAP 주소 레지스터 간 offset; NVMe 64-bit DMA/MSI 주소 설정 */
		.imap_window_offset = 0x8,	/* NVMe: imap_window_offset 필드 초기화; 연속 IMAP 윈도우 간 offset; NVMe MSI multiple vector 또는 DMA window 분할 */
	},	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* IARR3/IMAP3 */
		.type = IPROC_PCIE_IB_MAP_MEM,	/* NVMe: type 필드 초기화; inbound 영역 타입; NVMe DMA(mem) 또는 MSI(io) 매핑 구분 */
		.size_unit = SZ_1G,	/* NVMe: size_unit 필드 초기화; inbound 크기 단위(KB/MB/GB); NVMe DMA/IO 영역 크기 정밀 설정 */
		.region_sizes = { 1, 2, 4, 8, 16, 32 },	/* NVMe: region_sizes 필드 초기화; 지원 inbound region 크기 배열; NVMe DMA 범위 선택 */
		.nr_sizes = 6,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
		.nr_windows = 8,	/* NVMe: nr_windows 필드 초기화; region 내 IMAP 윈도우 수; NVMe MSI 또는 DMA segmentation */
		.imap_addr_offset = 0x4,	/* NVMe: imap_addr_offset 필드 초기화; 상위/하위 IMAP 주소 레지스터 간 offset; NVMe 64-bit DMA/MSI 주소 설정 */
		.imap_window_offset = 0x8,	/* NVMe: imap_window_offset 필드 초기화; 연속 IMAP 윈도우 간 offset; NVMe MSI multiple vector 또는 DMA window 분할 */
	},	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	{	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
		/* IARR4/IMAP4 */
		.type = IPROC_PCIE_IB_MAP_MEM,	/* NVMe: type 필드 초기화; inbound 영역 타입; NVMe DMA(mem) 또는 MSI(io) 매핑 구분 */
		.size_unit = SZ_1G,	/* NVMe: size_unit 필드 초기화; inbound 크기 단위(KB/MB/GB); NVMe DMA/IO 영역 크기 정밀 설정 */
		.region_sizes = { 32, 64, 128, 256, 512 },	/* NVMe: region_sizes 필드 초기화; 지원 inbound region 크기 배열; NVMe DMA 범위 선택 */
		.nr_sizes = 5,	/* NVMe: nr_sizes 필드 초기화; 지원하는 outbound 크기 개수; NVMe MMIO 리소스 할당 시 적합한 윈도우 선택 */
		.nr_windows = 8,	/* NVMe: nr_windows 필드 초기화; region 내 IMAP 윈도우 수; NVMe MSI 또는 DMA segmentation */
		.imap_addr_offset = 0x4,	/* NVMe: imap_addr_offset 필드 초기화; 상위/하위 IMAP 주소 레지스터 간 offset; NVMe 64-bit DMA/MSI 주소 설정 */
		.imap_window_offset = 0x8,	/* NVMe: imap_window_offset 필드 초기화; 연속 IMAP 윈도우 간 offset; NVMe MSI multiple vector 또는 DMA window 분할 */
	},	/* NVMe: paxb_v2_ib_map 초기화 데이터; NVMe PCIe host 제어 파라미터 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/*
 * iProc PCIe host registers
 */
enum iproc_pcie_reg {	/* NVMe: enum enumerator; NVMe PCIe host iproc_pcie_reg 식별자 */
	/* clock/reset signal control */
	IPROC_PCIE_CLK_CTRL = 0,	/* NVMe: IPROC_PCIE_CLK_CTRL enumerator; clock/reset 신호 제어; NVMe host controller 클럭/reset 상태 */

	/*
	 * To allow MSI to be steered to an external MSI controller (e.g., ARM
	 * GICv3 ITS)
	 */
	IPROC_PCIE_MSI_GIC_MODE,	/* NVMe: IPROC_PCIE_MSI_GIC_MODE enumerator; MSI를 외부 GICv3 ITS로 steering 모드; NVMe MSI 목적지 제어 */

	/*
	 * IPROC_PCIE_MSI_BASE_ADDR and IPROC_PCIE_MSI_WINDOW_SIZE define the
	 * window where the MSI posted writes are written, for the writes to be
	 * interpreted as MSI writes.
	 */
	IPROC_PCIE_MSI_BASE_ADDR,	/* NVMe: IPROC_PCIE_MSI_BASE_ADDR enumerator; MSI write 윈도우 기준 주소; NVMe MSI write 해석 범위 */
	IPROC_PCIE_MSI_WINDOW_SIZE,	/* NVMe: IPROC_PCIE_MSI_WINDOW_SIZE enumerator; MSI write 윈도우 크기; NVMe MSI 영역 크기 설정 */

	/*
	 * To hold the address of the register where the MSI writes are
	 * programmed.  When ARM GICv3 ITS is used, this should be programmed
	 * with the address of the GITS_TRANSLATER register.
	 */
	IPROC_PCIE_MSI_ADDR_LO,	/* NVMe: IPROC_PCIE_MSI_ADDR_LO enumerator; MSI 목적지 주소 하위 32비트; NVMe ITS GITS_TRANSLATER 주소 */
	IPROC_PCIE_MSI_ADDR_HI,	/* NVMe: IPROC_PCIE_MSI_ADDR_HI enumerator; MSI 목적지 주소 상위 32비트; NVMe 64-bit MSI 주소 */

	/* enable MSI */
	IPROC_PCIE_MSI_EN_CFG,	/* NVMe: IPROC_PCIE_MSI_EN_CFG enumerator; MSI enable 제어; NVMe MSI/MSI-X 인터럽트 활성화 */

	/* allow access to root complex configuration space */
	IPROC_PCIE_CFG_IND_ADDR,	/* NVMe: IPROC_PCIE_CFG_IND_ADDR enumerator; RC 자신 config 공간 indirect 주소; NVMe 루트 포트 config 접근 */
	IPROC_PCIE_CFG_IND_DATA,	/* NVMe: IPROC_PCIE_CFG_IND_DATA enumerator; RC 자신 config 공간 indirect 데이터; NVMe 루트 포트 config 데이터 */

	/* allow access to device configuration space */
	IPROC_PCIE_CFG_ADDR,	/* NVMe: IPROC_PCIE_CFG_ADDR enumerator; downstream EP config 접근 주소; NVMe EP config TLP 주소 */
	IPROC_PCIE_CFG_DATA,	/* NVMe: IPROC_PCIE_CFG_DATA enumerator; downstream EP config 접근 데이터; NVMe EP config read/write 데이터 */

	/* enable INTx */
	IPROC_PCIE_INTX_EN,	/* NVMe: IPROC_PCIE_INTX_EN enumerator; INTx 인터럽트 enable; NVMe legacy INTx A/B/C/D 활성화 */

	/* outbound address mapping */
	IPROC_PCIE_OARR0,	/* NVMe: IPROC_PCIE_OARR0 enumerator; outbound address register 0 low; NVMe MMIO AXI->PCI 변환 */
	IPROC_PCIE_OMAP0,	/* NVMe: IPROC_PCIE_OMAP0 enumerator; outbound mapping register 0 low; NVMe MMIO PCI 주소 출력 */
	IPROC_PCIE_OARR1,	/* NVMe: IPROC_PCIE_OARR1 enumerator; outbound address register 1 low; NVMe 추가 MMIO 윈도우 */
	IPROC_PCIE_OMAP1,	/* NVMe: IPROC_PCIE_OMAP1 enumerator; outbound mapping register 1 low; NVMe 추가 PCI 주소 출력 */
	IPROC_PCIE_OARR2,	/* NVMe: IPROC_PCIE_OARR2 enumerator; outbound address register 2 low; NVMe PAXB v2 대형 MMIO */
	IPROC_PCIE_OMAP2,	/* NVMe: IPROC_PCIE_OMAP2 enumerator; outbound mapping register 2 low; NVMe PAXB v2 대형 PCI 주소 */
	IPROC_PCIE_OARR3,	/* NVMe: IPROC_PCIE_OARR3 enumerator; outbound address register 3 low; NVMe PAXB v2 대형 MMIO */
	IPROC_PCIE_OMAP3,	/* NVMe: IPROC_PCIE_OMAP3 enumerator; outbound mapping register 3 low; NVMe PAXB v2 대형 PCI 주소 */

	/* inbound address mapping */
	IPROC_PCIE_IARR0,	/* NVMe: IPROC_PCIE_IARR0 enumerator; inbound address register 0 low; NVMe IO(32K) inbound 영역 */
	IPROC_PCIE_IMAP0,	/* NVMe: IPROC_PCIE_IMAP0 enumerator; inbound mapping register 0 low; NVMe IO->AXI 변환 */
	IPROC_PCIE_IARR1,	/* NVMe: IPROC_PCIE_IARR1 enumerator; inbound address register 1 low; NVMe 작은 DMA inbound 영역 */
	IPROC_PCIE_IMAP1,	/* NVMe: IPROC_PCIE_IMAP1 enumerator; inbound mapping register 1 low; NVMe 작은 DMA->AXI 변환 */
	IPROC_PCIE_IARR2,	/* NVMe: IPROC_PCIE_IARR2 enumerator; inbound address register 2 low; NVMe 중형 DMA inbound 영역 */
	IPROC_PCIE_IMAP2,	/* NVMe: IPROC_PCIE_IMAP2 enumerator; inbound mapping register 2 low; NVMe 중형 DMA->AXI 변환 */
	IPROC_PCIE_IARR3,	/* NVMe: IPROC_PCIE_IARR3 enumerator; inbound address register 3 low; NVMe 대형 DMA inbound 영역 */
	IPROC_PCIE_IMAP3,	/* NVMe: IPROC_PCIE_IMAP3 enumerator; inbound mapping register 3 low; NVMe 대형 DMA->AXI 변환 */
	IPROC_PCIE_IARR4,	/* NVMe: IPROC_PCIE_IARR4 enumerator; inbound address register 4 low; NVMe 초대형 DMA inbound 영역 */
	IPROC_PCIE_IMAP4,	/* NVMe: IPROC_PCIE_IMAP4 enumerator; inbound mapping register 4 low; NVMe 초대형 DMA->AXI 변환 */

	/* config read status */
	IPROC_PCIE_CFG_RD_STATUS,	/* NVMe: IPROC_PCIE_CFG_RD_STATUS enumerator; config read 상태; NVMe RRS/UR/CA 결과 확인 */

	/* link status */
	IPROC_PCIE_LINK_STATUS,	/* NVMe: IPROC_PCIE_LINK_STATUS enumerator; PCIe 링크 상태; NVMe PHY/link up 감지 */

	/* enable APB error for unsupported requests */
	IPROC_PCIE_APB_ERR_EN,	/* NVMe: IPROC_PCIE_APB_ERR_EN enumerator; APB error enable; NVMe config 접근 bus error 제어 */

	/* total number of core registers */
	IPROC_PCIE_MAX_NUM_REG,	/* NVMe: IPROC_PCIE_MAX_NUM_REG enumerator; 총 레지스터 수; NVMe controller 버전별 레지스터 테이블 크기 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/* iProc PCIe PAXB BCMA registers */
static const u16 iproc_pcie_reg_paxb_bcma[IPROC_PCIE_MAX_NUM_REG] = {	/* NVMe: iproc_pcie_reg_paxb_bcma 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	[IPROC_PCIE_CLK_CTRL]		= 0x000,	/* NVMe: [IPROC_PCIE_CLK_CTRL] = ... 초기화; clock/reset 신호 제어; NVMe host controller 클럭/reset 상태 */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x120,	/* NVMe: [IPROC_PCIE_CFG_IND_ADDR] = ... 초기화; RC 자신 config 공간 indirect 주소; NVMe 루트 포트 config 접근 */
	[IPROC_PCIE_CFG_IND_DATA]	= 0x124,	/* NVMe: [IPROC_PCIE_CFG_IND_DATA] = ... 초기화; RC 자신 config 공간 indirect 데이터; NVMe 루트 포트 config 데이터 */
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,	/* NVMe: [IPROC_PCIE_CFG_ADDR] = ... 초기화; downstream EP config 접근 주소; NVMe EP config TLP 주소 */
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,	/* NVMe: [IPROC_PCIE_CFG_DATA] = ... 초기화; downstream EP config 접근 데이터; NVMe EP config read/write 데이터 */
	[IPROC_PCIE_INTX_EN]		= 0x330,	/* NVMe: [IPROC_PCIE_INTX_EN] = ... 초기화; INTx 인터럽트 enable; NVMe legacy INTx A/B/C/D 활성화 */
	[IPROC_PCIE_LINK_STATUS]	= 0xf0c,	/* NVMe: [IPROC_PCIE_LINK_STATUS] = ... 초기화; PCIe 링크 상태; NVMe PHY/link up 감지 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/* iProc PCIe PAXB registers */
static const u16 iproc_pcie_reg_paxb[IPROC_PCIE_MAX_NUM_REG] = {	/* NVMe: iproc_pcie_reg_paxb 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	[IPROC_PCIE_CLK_CTRL]		= 0x000,	/* NVMe: [IPROC_PCIE_CLK_CTRL] = ... 초기화; clock/reset 신호 제어; NVMe host controller 클럭/reset 상태 */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x120,	/* NVMe: [IPROC_PCIE_CFG_IND_ADDR] = ... 초기화; RC 자신 config 공간 indirect 주소; NVMe 루트 포트 config 접근 */
	[IPROC_PCIE_CFG_IND_DATA]	= 0x124,	/* NVMe: [IPROC_PCIE_CFG_IND_DATA] = ... 초기화; RC 자신 config 공간 indirect 데이터; NVMe 루트 포트 config 데이터 */
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,	/* NVMe: [IPROC_PCIE_CFG_ADDR] = ... 초기화; downstream EP config 접근 주소; NVMe EP config TLP 주소 */
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,	/* NVMe: [IPROC_PCIE_CFG_DATA] = ... 초기화; downstream EP config 접근 데이터; NVMe EP config read/write 데이터 */
	[IPROC_PCIE_INTX_EN]		= 0x330,	/* NVMe: [IPROC_PCIE_INTX_EN] = ... 초기화; INTx 인터럽트 enable; NVMe legacy INTx A/B/C/D 활성화 */
	[IPROC_PCIE_OARR0]		= 0xd20,	/* NVMe: [IPROC_PCIE_OARR0] = ... 초기화; outbound address register 0 low; NVMe MMIO AXI->PCI 변환 */
	[IPROC_PCIE_OMAP0]		= 0xd40,	/* NVMe: [IPROC_PCIE_OMAP0] = ... 초기화; outbound mapping register 0 low; NVMe MMIO PCI 주소 출력 */
	[IPROC_PCIE_OARR1]		= 0xd28,	/* NVMe: [IPROC_PCIE_OARR1] = ... 초기화; outbound address register 1 low; NVMe 추가 MMIO 윈도우 */
	[IPROC_PCIE_OMAP1]		= 0xd48,	/* NVMe: [IPROC_PCIE_OMAP1] = ... 초기화; outbound mapping register 1 low; NVMe 추가 PCI 주소 출력 */
	[IPROC_PCIE_LINK_STATUS]	= 0xf0c,	/* NVMe: [IPROC_PCIE_LINK_STATUS] = ... 초기화; PCIe 링크 상태; NVMe PHY/link up 감지 */
	[IPROC_PCIE_APB_ERR_EN]		= 0xf40,	/* NVMe: [IPROC_PCIE_APB_ERR_EN] = ... 초기화; APB error enable; NVMe config 접근 bus error 제어 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/* iProc PCIe PAXB v2 registers */
static const u16 iproc_pcie_reg_paxb_v2[IPROC_PCIE_MAX_NUM_REG] = {	/* NVMe: iproc_pcie_reg_paxb_v2 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	[IPROC_PCIE_CLK_CTRL]		= 0x000,	/* NVMe: [IPROC_PCIE_CLK_CTRL] = ... 초기화; clock/reset 신호 제어; NVMe host controller 클럭/reset 상태 */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x120,	/* NVMe: [IPROC_PCIE_CFG_IND_ADDR] = ... 초기화; RC 자신 config 공간 indirect 주소; NVMe 루트 포트 config 접근 */
	[IPROC_PCIE_CFG_IND_DATA]	= 0x124,	/* NVMe: [IPROC_PCIE_CFG_IND_DATA] = ... 초기화; RC 자신 config 공간 indirect 데이터; NVMe 루트 포트 config 데이터 */
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,	/* NVMe: [IPROC_PCIE_CFG_ADDR] = ... 초기화; downstream EP config 접근 주소; NVMe EP config TLP 주소 */
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,	/* NVMe: [IPROC_PCIE_CFG_DATA] = ... 초기화; downstream EP config 접근 데이터; NVMe EP config read/write 데이터 */
	[IPROC_PCIE_INTX_EN]		= 0x330,	/* NVMe: [IPROC_PCIE_INTX_EN] = ... 초기화; INTx 인터럽트 enable; NVMe legacy INTx A/B/C/D 활성화 */
	[IPROC_PCIE_OARR0]		= 0xd20,	/* NVMe: [IPROC_PCIE_OARR0] = ... 초기화; outbound address register 0 low; NVMe MMIO AXI->PCI 변환 */
	[IPROC_PCIE_OMAP0]		= 0xd40,	/* NVMe: [IPROC_PCIE_OMAP0] = ... 초기화; outbound mapping register 0 low; NVMe MMIO PCI 주소 출력 */
	[IPROC_PCIE_OARR1]		= 0xd28,	/* NVMe: [IPROC_PCIE_OARR1] = ... 초기화; outbound address register 1 low; NVMe 추가 MMIO 윈도우 */
	[IPROC_PCIE_OMAP1]		= 0xd48,	/* NVMe: [IPROC_PCIE_OMAP1] = ... 초기화; outbound mapping register 1 low; NVMe 추가 PCI 주소 출력 */
	[IPROC_PCIE_OARR2]		= 0xd60,	/* NVMe: [IPROC_PCIE_OARR2] = ... 초기화; outbound address register 2 low; NVMe PAXB v2 대형 MMIO */
	[IPROC_PCIE_OMAP2]		= 0xd68,	/* NVMe: [IPROC_PCIE_OMAP2] = ... 초기화; outbound mapping register 2 low; NVMe PAXB v2 대형 PCI 주소 */
	[IPROC_PCIE_OARR3]		= 0xdf0,	/* NVMe: [IPROC_PCIE_OARR3] = ... 초기화; outbound address register 3 low; NVMe PAXB v2 대형 MMIO */
	[IPROC_PCIE_OMAP3]		= 0xdf8,	/* NVMe: [IPROC_PCIE_OMAP3] = ... 초기화; outbound mapping register 3 low; NVMe PAXB v2 대형 PCI 주소 */
	[IPROC_PCIE_IARR0]		= 0xd00,	/* NVMe: [IPROC_PCIE_IARR0] = ... 초기화; inbound address register 0 low; NVMe IO(32K) inbound 영역 */
	[IPROC_PCIE_IMAP0]		= 0xc00,	/* NVMe: [IPROC_PCIE_IMAP0] = ... 초기화; inbound mapping register 0 low; NVMe IO->AXI 변환 */
	[IPROC_PCIE_IARR1]		= 0xd08,	/* NVMe: [IPROC_PCIE_IARR1] = ... 초기화; inbound address register 1 low; NVMe 작은 DMA inbound 영역 */
	[IPROC_PCIE_IMAP1]		= 0xd70,	/* NVMe: [IPROC_PCIE_IMAP1] = ... 초기화; inbound mapping register 1 low; NVMe 작은 DMA->AXI 변환 */
	[IPROC_PCIE_IARR2]		= 0xd10,	/* NVMe: [IPROC_PCIE_IARR2] = ... 초기화; inbound address register 2 low; NVMe 중형 DMA inbound 영역 */
	[IPROC_PCIE_IMAP2]		= 0xcc0,	/* NVMe: [IPROC_PCIE_IMAP2] = ... 초기화; inbound mapping register 2 low; NVMe 중형 DMA->AXI 변환 */
	[IPROC_PCIE_IARR3]		= 0xe00,	/* NVMe: [IPROC_PCIE_IARR3] = ... 초기화; inbound address register 3 low; NVMe 대형 DMA inbound 영역 */
	[IPROC_PCIE_IMAP3]		= 0xe08,	/* NVMe: [IPROC_PCIE_IMAP3] = ... 초기화; inbound mapping register 3 low; NVMe 대형 DMA->AXI 변환 */
	[IPROC_PCIE_IARR4]		= 0xe68,	/* NVMe: [IPROC_PCIE_IARR4] = ... 초기화; inbound address register 4 low; NVMe 초대형 DMA inbound 영역 */
	[IPROC_PCIE_IMAP4]		= 0xe70,	/* NVMe: [IPROC_PCIE_IMAP4] = ... 초기화; inbound mapping register 4 low; NVMe 초대형 DMA->AXI 변환 */
	[IPROC_PCIE_CFG_RD_STATUS]	= 0xee0,	/* NVMe: [IPROC_PCIE_CFG_RD_STATUS] = ... 초기화; config read 상태; NVMe RRS/UR/CA 결과 확인 */
	[IPROC_PCIE_LINK_STATUS]	= 0xf0c,	/* NVMe: [IPROC_PCIE_LINK_STATUS] = ... 초기화; PCIe 링크 상태; NVMe PHY/link up 감지 */
	[IPROC_PCIE_APB_ERR_EN]		= 0xf40,	/* NVMe: [IPROC_PCIE_APB_ERR_EN] = ... 초기화; APB error enable; NVMe config 접근 bus error 제어 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/* iProc PCIe PAXC v1 registers */
static const u16 iproc_pcie_reg_paxc[IPROC_PCIE_MAX_NUM_REG] = {	/* NVMe: iproc_pcie_reg_paxc 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	[IPROC_PCIE_CLK_CTRL]		= 0x000,	/* NVMe: [IPROC_PCIE_CLK_CTRL] = ... 초기화; clock/reset 신호 제어; NVMe host controller 클럭/reset 상태 */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x1f0,	/* NVMe: [IPROC_PCIE_CFG_IND_ADDR] = ... 초기화; RC 자신 config 공간 indirect 주소; NVMe 루트 포트 config 접근 */
	[IPROC_PCIE_CFG_IND_DATA]	= 0x1f4,	/* NVMe: [IPROC_PCIE_CFG_IND_DATA] = ... 초기화; RC 자신 config 공간 indirect 데이터; NVMe 루트 포트 config 데이터 */
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,	/* NVMe: [IPROC_PCIE_CFG_ADDR] = ... 초기화; downstream EP config 접근 주소; NVMe EP config TLP 주소 */
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,	/* NVMe: [IPROC_PCIE_CFG_DATA] = ... 초기화; downstream EP config 접근 데이터; NVMe EP config read/write 데이터 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/* iProc PCIe PAXC v2 registers */
static const u16 iproc_pcie_reg_paxc_v2[IPROC_PCIE_MAX_NUM_REG] = {	/* NVMe: iproc_pcie_reg_paxc_v2 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	[IPROC_PCIE_MSI_GIC_MODE]	= 0x050,	/* NVMe: [IPROC_PCIE_MSI_GIC_MODE] = ... 초기화; MSI를 외부 GICv3 ITS로 steering 모드; NVMe MSI 목적지 제어 */
	[IPROC_PCIE_MSI_BASE_ADDR]	= 0x074,	/* NVMe: [IPROC_PCIE_MSI_BASE_ADDR] = ... 초기화; MSI write 윈도우 기준 주소; NVMe MSI write 해석 범위 */
	[IPROC_PCIE_MSI_WINDOW_SIZE]	= 0x078,	/* NVMe: [IPROC_PCIE_MSI_WINDOW_SIZE] = ... 초기화; MSI write 윈도우 크기; NVMe MSI 영역 크기 설정 */
	[IPROC_PCIE_MSI_ADDR_LO]	= 0x07c,	/* NVMe: [IPROC_PCIE_MSI_ADDR_LO] = ... 초기화; MSI 목적지 주소 하위 32비트; NVMe ITS GITS_TRANSLATER 주소 */
	[IPROC_PCIE_MSI_ADDR_HI]	= 0x080,	/* NVMe: [IPROC_PCIE_MSI_ADDR_HI] = ... 초기화; MSI 목적지 주소 상위 32비트; NVMe 64-bit MSI 주소 */
	[IPROC_PCIE_MSI_EN_CFG]		= 0x09c,	/* NVMe: [IPROC_PCIE_MSI_EN_CFG] = ... 초기화; MSI enable 제어; NVMe MSI/MSI-X 인터럽트 활성화 */
	[IPROC_PCIE_CFG_IND_ADDR]	= 0x1f0,	/* NVMe: [IPROC_PCIE_CFG_IND_ADDR] = ... 초기화; RC 자신 config 공간 indirect 주소; NVMe 루트 포트 config 접근 */
	[IPROC_PCIE_CFG_IND_DATA]	= 0x1f4,	/* NVMe: [IPROC_PCIE_CFG_IND_DATA] = ... 초기화; RC 자신 config 공간 indirect 데이터; NVMe 루트 포트 config 데이터 */
	[IPROC_PCIE_CFG_ADDR]		= 0x1f8,	/* NVMe: [IPROC_PCIE_CFG_ADDR] = ... 초기화; downstream EP config 접근 주소; NVMe EP config TLP 주소 */
	[IPROC_PCIE_CFG_DATA]		= 0x1fc,	/* NVMe: [IPROC_PCIE_CFG_DATA] = ... 초기화; downstream EP config 접근 데이터; NVMe EP config read/write 데이터 */
};	/* NVMe: NVMe PCIe host controller 동작 */

/*
 * List of device IDs of controllers that have corrupted capability list that
 * require SW fixup
 */
static const u16 iproc_pcie_corrupt_cap_did[] = {	/* NVMe: iproc_pcie_corrupt_cap_did 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	0x16cd,	/* NVMe: iproc_pcie_corrupt_cap_did 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	0x16f0,	/* NVMe: iproc_pcie_corrupt_cap_did 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	0xd802,	/* NVMe: iproc_pcie_corrupt_cap_did 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	0xd804	/* NVMe: iproc_pcie_corrupt_cap_did 초기화 데이터; NVMe PCIe host 제어 파라미터 */
};	/* NVMe: NVMe PCIe host controller 동작 */

static inline struct iproc_pcie *iproc_data(struct pci_bus *bus)	/* NVMe: pci_bus.sysdata에서 iproc_pcie 포인터 반환; NVMe host bridge private data 접근 */
{	/* NVMe: iproc_data 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie *pcie = bus->sysdata;	/* NVMe: iproc_data 내부 동작; NVMe PCIe host 처리 */
	return pcie;	/* NVMe: return; NVMe iproc_data 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline bool iproc_pcie_reg_is_invalid(u16 reg_offset)	/* NVMe: 레지스터 offset이 미사용 값인지 확인; NVMe controller 버전별 레지스터 보호 */
{	/* NVMe: iproc_pcie_reg_is_invalid 내부 동작; NVMe PCIe host 처리 */
	return !!(reg_offset == IPROC_PCIE_REG_INVALID);	/* NVMe: return; NVMe iproc_pcie_reg_is_invalid 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline u16 iproc_pcie_reg_offset(struct iproc_pcie *pcie,	/* NVMe: enum -> 실제 레지스터 offset 변환; NVMe host register 접근 */
					enum iproc_pcie_reg reg)	/* NVMe: iproc_pcie_reg_offset 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_reg_offset 내부 동작; NVMe PCIe host 처리 */
	return pcie->reg_offsets[reg];	/* NVMe: return; NVMe iproc_pcie_reg_offset 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline u32 iproc_pcie_read_reg(struct iproc_pcie *pcie,	/* NVMe: iProc PCIe 코어 레지스터 읽기; NVMe 링크/MSI/config 상태 확인 */
				      enum iproc_pcie_reg reg)	/* NVMe: iproc_pcie_read_reg 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_read_reg 내부 동작; NVMe PCIe host 처리 */
	u16 offset = iproc_pcie_reg_offset(pcie, reg);	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */

	if (iproc_pcie_reg_is_invalid(offset))	/* NVMe: 조건 분기; NVMe iproc_pcie_read_reg 흐름 제어 */
		return 0;	/* NVMe: return; NVMe iproc_pcie_read_reg 결과 반환/종료 */

	return readl(pcie->base + offset);	/* NVMe: return; NVMe iproc_pcie_read_reg 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline void iproc_pcie_write_reg(struct iproc_pcie *pcie,	/* NVMe: iProc PCIe 코어 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
					enum iproc_pcie_reg reg, u32 val)	/* NVMe: iproc_pcie_write_reg 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_write_reg 내부 동작; NVMe PCIe host 처리 */
	u16 offset = iproc_pcie_reg_offset(pcie, reg);	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */

	if (iproc_pcie_reg_is_invalid(offset))	/* NVMe: 조건 분기; NVMe iproc_pcie_write_reg 흐름 제어 */
		return;	/* NVMe: return; NVMe iproc_pcie_write_reg 결과 반환/종료 */

	writel(val, pcie->base + offset);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */
}	/* NVMe: NVMe PCIe host controller 동작 */

/*
 * APB error forwarding can be disabled during access of configuration
 * registers of the endpoint device, to prevent unsupported requests
 * (typically seen during enumeration with multi-function devices) from
 * triggering a system exception.
 */
static inline void iproc_pcie_apb_err_disable(struct pci_bus *bus,	/* NVMe: APB error forwarding disable/enable; NVMe multi-function 열거 시 bus error 예외 회피 */
					      bool disable)	/* NVMe: iproc_pcie_apb_err_disable 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_apb_err_disable 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie *pcie = iproc_data(bus);	/* NVMe: pci_bus->sysdata에서 iproc_pcie 획득; NVMe host bridge private data 접근 */
	u32 val;	/* NVMe: iproc_pcie_apb_err_disable 내부 동작; NVMe PCIe host 처리 */

	if (bus->number && pcie->has_apb_err_disable) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_APB_ERR_EN);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
		if (disable)	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			val &= ~APB_ERR_EN;	/* NVMe: val 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
		else	/* NVMe: else 분기; NVMe if 대안 동작 수행 */
			val |= APB_ERR_EN;	/* NVMe: val 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_APB_ERR_EN, val);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
	}	/* NVMe: iproc_pcie_apb_err_disable 내부 동작; NVMe PCIe host 처리 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static void __iomem *iproc_pcie_map_ep_cfg_reg(struct iproc_pcie *pcie,	/* NVMe: downstream NVMe EP config 공간 MMIO 주소 변환; Type1 config TLP 대응 */
					       unsigned int busno,	/* NVMe: iproc_pcie_map_ep_cfg_reg 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
					       unsigned int devfn,	/* NVMe: iproc_pcie_map_ep_cfg_reg 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
					       int where)	/* NVMe: iproc_pcie_map_ep_cfg_reg 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_map_ep_cfg_reg 내부 동작; NVMe PCIe host 처리 */
	u16 offset;	/* NVMe: iproc_pcie_map_ep_cfg_reg 내부 동작; NVMe PCIe host 처리 */
	u32 val;	/* NVMe: iproc_pcie_map_ep_cfg_reg 내부 동작; NVMe PCIe host 처리 */

	/* EP device access */
	val = ALIGN_DOWN(PCIE_ECAM_OFFSET(busno, devfn, where), 4) |	/* NVMe: val 변수/필드 갱신; NVMe iproc_pcie_map_ep_cfg_reg 상태/주소/제어값 설정 */
		CFG_ADDR_CFG_TYPE_1;	/* NVMe: iproc_pcie_map_ep_cfg_reg 내부 동작; NVMe PCIe host 처리 */

	iproc_pcie_write_reg(pcie, IPROC_PCIE_CFG_ADDR, val);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
	offset = iproc_pcie_reg_offset(pcie, IPROC_PCIE_CFG_DATA);	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */

	if (iproc_pcie_reg_is_invalid(offset))	/* NVMe: 조건 분기; NVMe iproc_pcie_map_ep_cfg_reg 흐름 제어 */
		return NULL;	/* NVMe: return; NVMe iproc_pcie_map_ep_cfg_reg 결과 반환/종료 */

	return (pcie->base + offset);	/* NVMe: return; NVMe iproc_pcie_map_ep_cfg_reg 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static unsigned int iproc_pcie_cfg_retry(struct iproc_pcie *pcie,	/* NVMe: RRS(Configuration Retry Status) 소프트웨어 재시도; NVMe Vendor ID 외 config read 처리 */
					 void __iomem *cfg_data_p)	/* NVMe: iproc_pcie_cfg_retry 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_cfg_retry 내부 동작; NVMe PCIe host 처리 */
	int timeout = CFG_RETRY_STATUS_TIMEOUT_US;	/* NVMe: iproc_pcie_cfg_retry 내부 동작; NVMe PCIe host 처리 */
	unsigned int data;	/* NVMe: iproc_pcie_cfg_retry 내부 동작; NVMe PCIe host 처리 */
	u32 status;	/* NVMe: iproc_pcie_cfg_retry 내부 동작; NVMe PCIe host 처리 */

	/*
	 * As per PCIe r6.0, sec 2.3.2, Config RRS Software Visibility only
	 * affects config reads of the Vendor ID.  For config writes or any
	 * other config reads, the Root may automatically reissue the
	 * configuration request again as a new request.
	 *
	 * For config reads, this hardware returns CFG_RETRY_STATUS data
	 * when it receives a RRS completion, regardless of the address of
	 * the read or the RRS Software Visibility Enable bit.  As a
	 * partial workaround for this, we retry in software any read that
	 * returns CFG_RETRY_STATUS.
	 *
	 * Note that a non-Vendor ID config register may have a value of
	 * CFG_RETRY_STATUS.  If we read that, we can't distinguish it from
	 * a RRS completion, so we will incorrectly retry the read and
	 * eventually return the wrong data (0xffffffff).
	 */
	data = readl(cfg_data_p);	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
	while (data == CFG_RETRY_STATUS && timeout--) {	/* NVMe: while 반복; NVMe while 재시도/대기 루프 */
		/*
		 * RRS state is set in CFG_RD status register
		 * This will handle the case where CFG_RETRY_STATUS is
		 * valid config data.
		 */
		status = iproc_pcie_read_reg(pcie, IPROC_PCIE_CFG_RD_STATUS);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
		if (status != CFG_RD_RRS)	/* NVMe: 조건 분기; NVMe while 흐름 제어 */
			return data;	/* NVMe: return; NVMe while 결과 반환/종료 */

		udelay(1);	/* NVMe: 마이크로초 지연; NVMe reset/링크 안정화 대기 */
		data = readl(cfg_data_p);	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
	}	/* NVMe: iproc_pcie_cfg_retry 내부 동작; NVMe PCIe host 처리 */

	if (data == CFG_RETRY_STATUS)	/* NVMe: 조건 분기; NVMe iproc_pcie_cfg_retry 흐름 제어 */
		data = 0xffffffff;	/* NVMe: data 변수/필드 갱신; NVMe iproc_pcie_cfg_retry 상태/주소/제어값 설정 */

	return data;	/* NVMe: return; NVMe iproc_pcie_cfg_retry 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static void iproc_pcie_fix_cap(struct iproc_pcie *pcie, int where, u32 *val)	/* NVMe: PAXC capability list corruption 수정; NVMe PCIe/PM capability 올바르게 노출 */
{	/* NVMe: iproc_pcie_fix_cap 내부 동작; NVMe PCIe host 처리 */
	u32 i, dev_id;	/* NVMe: iproc_pcie_fix_cap 내부 동작; NVMe PCIe host 처리 */

	switch (where & ~0x3) {	/* NVMe: switch 분기; NVMe switch 상태/offset별 처리 */
	case PCI_VENDOR_ID:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		dev_id = *val >> 16;	/* NVMe: dev_id 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */

		/*
		 * Activate fixup for those controllers that have corrupted
		 * capability list registers
		 */
		for (i = 0; i < ARRAY_SIZE(iproc_pcie_corrupt_cap_did); i++)	/* NVMe: 반복문; NVMe switch 리소스/윈도우 순회 */
			if (dev_id == iproc_pcie_corrupt_cap_did[i])	/* NVMe: 조건 분기; NVMe switch 흐름 제어 */
				pcie->fix_paxc_cap = true;	/* NVMe: pcie->fix_paxc_cap 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */

	case IPROC_PCI_PM_CAP:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		if (pcie->fix_paxc_cap) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			/* advertise PM, force next capability to PCIe */
			*val &= ~IPROC_PCI_PM_CAP_MASK;
			*val |= IPROC_PCI_EXP_CAP << 8 | PCI_CAP_ID_PM;
		}	/* NVMe: switch 내부 동작; NVMe PCIe host 처리 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */

	case IPROC_PCI_EXP_CAP:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		if (pcie->fix_paxc_cap) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			/* advertise root port, version 2, terminate here */
			*val = (PCI_EXP_TYPE_ROOT_PORT << 4 | 2) << 16 |
				PCI_CAP_ID_EXP;	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
		}	/* NVMe: switch 내부 동작; NVMe PCIe host 처리 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */

	case IPROC_PCI_EXP_CAP + PCI_EXP_RTCTL:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		/* Don't advertise RRS SV support */
		*val &= ~(PCI_EXP_RTCAP_RRS_SV << 16);
		break;	/* NVMe: break; NVMe 처리 블록 종료 */

	default:	/* NVMe: default 처리; NVMe switch 기타 경우 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	}	/* NVMe: iproc_pcie_fix_cap 내부 동작; NVMe PCIe host 처리 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_config_read(struct pci_bus *bus, unsigned int devfn,	/* NVMe: PCI config read (RC 또는 EP); NVMe 장치 열거 시 Vendor/Device ID, BAR, capability 읽기 */
				  int where, int size, u32 *val)	/* NVMe: iproc_pcie_config_read 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie *pcie = iproc_data(bus);	/* NVMe: pci_bus->sysdata에서 iproc_pcie 획득; NVMe host bridge private data 접근 */
	unsigned int busno = bus->number;	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */
	void __iomem *cfg_data_p;	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */
	unsigned int data;	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */

	/* root complex access */
	if (busno == 0) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		ret = pci_generic_config_read32(bus, devfn, where, size, val);	/* NVMe: generic ECAM config read; NVMe EP config 공간 표준 읽기 */
		if (ret == PCIBIOS_SUCCESSFUL)	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			iproc_pcie_fix_cap(pcie, where, val);	/* NVMe: corrupted capability list 수정; NVMe capability 탐색 보정 */

		return ret;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */

	cfg_data_p = iproc_pcie_map_ep_cfg_reg(pcie, busno, devfn, where);	/* NVMe: NVMe EP config 공간 MMIO 주소 매핑; Type1 config 접근 */

	if (!cfg_data_p)	/* NVMe: 조건 분기; NVMe iproc_pcie_config_read 흐름 제어 */
		return PCIBIOS_DEVICE_NOT_FOUND;	/* NVMe: return; NVMe iproc_pcie_config_read 결과 반환/종료 */

	data = iproc_pcie_cfg_retry(pcie, cfg_data_p);	/* NVMe: RRS config read 재시도; NVMe 열거 지연/준비 상태 처리 */

	*val = data;
	if (size <= 2)	/* NVMe: 조건 분기; NVMe iproc_pcie_config_read 흐름 제어 */
		*val = (data >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);

	/*
	 * For PAXC and PAXCv2, the total number of PFs that one can enumerate
	 * depends on the firmware configuration. Unfortunately, due to an ASIC
	 * bug, unconfigured PFs cannot be properly hidden from the root
	 * complex. As a result, write access to these PFs will cause bus lock
	 * up on the embedded processor
	 *
	 * Since all unconfigured PFs are left with an incorrect, staled device
	 * ID of 0x168e (PCI_DEVICE_ID_NX2_57810), we try to catch those access
	 * early here and reject them all
	 */
#define DEVICE_ID_MASK     0xffff0000	/* NVMe: DEVICE_ID_MASK 매크로 정의; Vendor/Device ID 레지스터 내 device id 마스크; NVMe stale device id 필터 */
#define DEVICE_ID_SHIFT    16	/* NVMe: DEVICE_ID_SHIFT 매크로 정의; Device ID 비트 시프트; NVMe device id 추출 */
	if (pcie->rej_unconfig_pf &&	/* NVMe: 조건 분기; NVMe iproc_pcie_config_read 흐름 제어 */
	    (where & CFG_ADDR_REG_NUM_MASK) == PCI_VENDOR_ID)	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */
		if ((*val & DEVICE_ID_MASK) ==	/* NVMe: 조건 분기; NVMe iproc_pcie_config_read 흐름 제어 */
		    (PCI_DEVICE_ID_NX2_57810 << DEVICE_ID_SHIFT))	/* NVMe: iproc_pcie_config_read 내부 동작; NVMe PCIe host 처리 */
			return PCIBIOS_FUNC_NOT_SUPPORTED;	/* NVMe: return; NVMe iproc_pcie_config_read 결과 반환/종료 */

	return PCIBIOS_SUCCESSFUL;	/* NVMe: return; NVMe iproc_pcie_config_read 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

/*
 * Note access to the configuration registers are protected at the higher layer
 * by 'pci_lock' in drivers/pci/access.c
 */
static void __iomem *iproc_pcie_map_cfg_bus(struct iproc_pcie *pcie,	/* NVMe: bus/devfn/where -> MMIO 주소 (RC/EP); NVMe config 공간 접근의 핵심 경로 */
					    int busno, unsigned int devfn,	/* NVMe: iproc_pcie_map_cfg_bus 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
					    int where)	/* NVMe: iproc_pcie_map_cfg_bus 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_map_cfg_bus 내부 동작; NVMe PCIe host 처리 */
	u16 offset;	/* NVMe: iproc_pcie_map_cfg_bus 내부 동작; NVMe PCIe host 처리 */

	/* root complex access */
	if (busno == 0) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		if (PCIE_ECAM_DEVFN(devfn) > 0)	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			return NULL;	/* NVMe: return; NVMe if 결과 반환/종료 */

		iproc_pcie_write_reg(pcie, IPROC_PCIE_CFG_IND_ADDR,	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
				     where & CFG_IND_ADDR_MASK);	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
		offset = iproc_pcie_reg_offset(pcie, IPROC_PCIE_CFG_IND_DATA);	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */
		if (iproc_pcie_reg_is_invalid(offset))	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			return NULL;	/* NVMe: return; NVMe if 결과 반환/종료 */
		else	/* NVMe: else 분기; NVMe if 대안 동작 수행 */
			return (pcie->base + offset);	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_map_cfg_bus 내부 동작; NVMe PCIe host 처리 */

	return iproc_pcie_map_ep_cfg_reg(pcie, busno, devfn, where);	/* NVMe: return; NVMe iproc_pcie_map_cfg_bus 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static void __iomem *iproc_pcie_bus_map_cfg_bus(struct pci_bus *bus,	/* NVMe: pci_ops.map_bus 콜백; NVMe config 트랜잭션 주소 변환 */
						unsigned int devfn,	/* NVMe: iproc_pcie_bus_map_cfg_bus 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
						int where)	/* NVMe: iproc_pcie_bus_map_cfg_bus 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_bus_map_cfg_bus 내부 동작; NVMe PCIe host 처리 */
	return iproc_pcie_map_cfg_bus(iproc_data(bus), bus->number, devfn,	/* NVMe: bus/devfn/where -> MMIO 주소 (RC/EP); NVMe config 공간 접근의 핵심 경로 */
				      where);	/* NVMe: iproc_pcie_bus_map_cfg_bus 내부 동작; NVMe PCIe host 처리 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pci_raw_config_read32(struct iproc_pcie *pcie,	/* NVMe: RC 자신의 config 공간 raw dword 읽기; NVMe 루트 포트 capability/링크 제어 */
				       unsigned int devfn, int where,	/* NVMe: iproc_pci_raw_config_read32 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
				       int size, u32 *val)	/* NVMe: iproc_pci_raw_config_read32 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pci_raw_config_read32 내부 동작; NVMe PCIe host 처리 */
	void __iomem *addr;	/* NVMe: iproc_pci_raw_config_read32 내부 동작; NVMe PCIe host 처리 */

	addr = iproc_pcie_map_cfg_bus(pcie, 0, devfn, where & ~0x3);	/* NVMe: config bus MMIO 매핑; NVMe root/EP config 접근 공통 경로 */
	if (!addr)	/* NVMe: 조건 분기; NVMe iproc_pci_raw_config_read32 흐름 제어 */
		return PCIBIOS_DEVICE_NOT_FOUND;	/* NVMe: return; NVMe iproc_pci_raw_config_read32 결과 반환/종료 */

	*val = readl(addr);

	if (size <= 2)	/* NVMe: 조건 분기; NVMe iproc_pci_raw_config_read32 흐름 제어 */
		*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);

	return PCIBIOS_SUCCESSFUL;	/* NVMe: return; NVMe iproc_pci_raw_config_read32 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pci_raw_config_write32(struct iproc_pcie *pcie,	/* NVMe: RC 자신의 config 공간 raw dword 쓰기; NVMe 루트 포트 class/링크 설정 */
					unsigned int devfn, int where,	/* NVMe: iproc_pci_raw_config_write32 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
					int size, u32 val)	/* NVMe: iproc_pci_raw_config_write32 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pci_raw_config_write32 내부 동작; NVMe PCIe host 처리 */
	void __iomem *addr;	/* NVMe: iproc_pci_raw_config_write32 내부 동작; NVMe PCIe host 처리 */
	u32 mask, tmp;	/* NVMe: iproc_pci_raw_config_write32 내부 동작; NVMe PCIe host 처리 */

	addr = iproc_pcie_map_cfg_bus(pcie, 0, devfn, where & ~0x3);	/* NVMe: config bus MMIO 매핑; NVMe root/EP config 접근 공통 경로 */
	if (!addr)	/* NVMe: 조건 분기; NVMe iproc_pci_raw_config_write32 흐름 제어 */
		return PCIBIOS_DEVICE_NOT_FOUND;	/* NVMe: return; NVMe iproc_pci_raw_config_write32 결과 반환/종료 */

	if (size == 4) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		writel(val, addr);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */
		return PCIBIOS_SUCCESSFUL;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pci_raw_config_write32 내부 동작; NVMe PCIe host 처리 */

	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8));	/* NVMe: mask 변수/필드 갱신; NVMe iproc_pci_raw_config_write32 상태/주소/제어값 설정 */
	tmp = readl(addr) & mask;	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
	tmp |= val << ((where & 0x3) * 8);	/* NVMe: tmp 변수/필드 갱신; NVMe iproc_pci_raw_config_write32 상태/주소/제어값 설정 */
	writel(tmp, addr);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */

	return PCIBIOS_SUCCESSFUL;	/* NVMe: return; NVMe iproc_pci_raw_config_write32 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_config_read32(struct pci_bus *bus, unsigned int devfn,	/* NVMe: pci_ops.read: APB err disable 후 config read; NVMe EP config 공간 안전 접근 */
				    int where, int size, u32 *val)	/* NVMe: iproc_pcie_config_read32 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_config_read32 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_config_read32 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie *pcie = iproc_data(bus);	/* NVMe: pci_bus->sysdata에서 iproc_pcie 획득; NVMe host bridge private data 접근 */

	iproc_pcie_apb_err_disable(bus, true);	/* NVMe: APB bus error forward disable; NVMe multi-function 열거 시 예외 방지 */
	if (pcie->iproc_cfg_read)	/* NVMe: 조건 분기; NVMe iproc_pcie_config_read32 흐름 제어 */
		ret = iproc_pcie_config_read(bus, devfn, where, size, val);	/* NVMe: PCI config read (RC 또는 EP); NVMe 장치 열거 시 Vendor/Device ID, BAR, capability 읽기 */
	else	/* NVMe: else 분기; NVMe iproc_pcie_config_read32 대안 동작 수행 */
		ret = pci_generic_config_read32(bus, devfn, where, size, val);	/* NVMe: generic ECAM config read; NVMe EP config 공간 표준 읽기 */
	iproc_pcie_apb_err_disable(bus, false);	/* NVMe: APB bus error forward disable; NVMe multi-function 열거 시 예외 방지 */

	return ret;	/* NVMe: return; NVMe iproc_pcie_config_read32 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_config_write32(struct pci_bus *bus, unsigned int devfn,	/* NVMe: pci_ops.write: APB err disable 후 config write; NVMe EP config 공간 안전 쓰기 */
				     int where, int size, u32 val)	/* NVMe: iproc_pcie_config_write32 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_config_write32 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_config_write32 내부 동작; NVMe PCIe host 처리 */

	iproc_pcie_apb_err_disable(bus, true);	/* NVMe: APB bus error forward disable; NVMe multi-function 열거 시 예외 방지 */
	ret = pci_generic_config_write32(bus, devfn, where, size, val);	/* NVMe: generic ECAM config write; NVMe EP config 공간 표준 쓰기 */
	iproc_pcie_apb_err_disable(bus, false);	/* NVMe: APB bus error forward disable; NVMe multi-function 열거 시 예외 방지 */

	return ret;	/* NVMe: return; NVMe iproc_pcie_config_write32 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static struct pci_ops iproc_pcie_ops = {	/* NVMe: iproc_pcie_ops 초기화 데이터; NVMe PCIe host 제어 파라미터 */
	.map_bus = iproc_pcie_bus_map_cfg_bus,	/* NVMe: map_bus 필드 초기화; NVMe PCIe host iproc_pcie_ops 제어 파라미터 */
	.read = iproc_pcie_config_read32,	/* NVMe: read 필드 초기화; NVMe PCIe host iproc_pcie_ops 제어 파라미터 */
	.write = iproc_pcie_config_write32,	/* NVMe: write 필드 초기화; NVMe PCIe host iproc_pcie_ops 제어 파라미터 */
};	/* NVMe: NVMe PCIe host controller 동작 */

static void iproc_pcie_perst_ctrl(struct iproc_pcie *pcie, bool assert)	/* NVMe: PERST# 신호 assert/deassert; NVMe 장치 fundamental reset 타이밍 제어 */
{	/* NVMe: iproc_pcie_perst_ctrl 내부 동작; NVMe PCIe host 처리 */
	u32 val;	/* NVMe: iproc_pcie_perst_ctrl 내부 동작; NVMe PCIe host 처리 */

	/*
	 * PAXC and the internal emulated endpoint device downstream should not
	 * be reset.  If firmware has been loaded on the endpoint device at an
	 * earlier boot stage, reset here causes issues.
	 */
	if (pcie->ep_is_internal)	/* NVMe: 조건 분기; NVMe iproc_pcie_perst_ctrl 흐름 제어 */
		return;	/* NVMe: return; NVMe iproc_pcie_perst_ctrl 결과 반환/종료 */

	if (assert) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_CLK_CTRL);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
		val &= ~EP_PERST_SOURCE_SELECT & ~EP_MODE_SURVIVE_PERST &	/* NVMe: val 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
			~RC_PCIE_RST_OUTPUT;	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_CLK_CTRL, val);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
		udelay(250);	/* NVMe: 마이크로초 지연; NVMe reset/링크 안정화 대기 */
	} else {	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_CLK_CTRL);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
		val |= RC_PCIE_RST_OUTPUT;	/* NVMe: val 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_CLK_CTRL, val);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
		msleep(100);	/* NVMe: 밀리초 지연; NVMe PERST 해제 후 링크 트레이닝 대기 */
	}	/* NVMe: iproc_pcie_perst_ctrl 내부 동작; NVMe PCIe host 처리 */
}	/* NVMe: NVMe PCIe host controller 동작 */

int iproc_pcie_shutdown(struct iproc_pcie *pcie)	/* NVMe: PERST assert 후 shutdown; NVMe host bridge 종료 시 reset 유지 */
{	/* NVMe: iproc_pcie_shutdown 내부 동작; NVMe PCIe host 처리 */
	iproc_pcie_perst_ctrl(pcie, true);	/* NVMe: PERST# 신호 제어; NVMe 장치 reset/재열거 타이밍 */
	msleep(500);	/* NVMe: 밀리초 지연; NVMe PERST 해제 후 링크 트레이닝 대기 */

	return 0;	/* NVMe: return; NVMe iproc_pcie_shutdown 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */
EXPORT_SYMBOL_GPL(iproc_pcie_shutdown);	/* NVMe: 심볼 외부 노출; NVMe 관련 SoC driver가 host helper 재사용 */

static int iproc_pcie_check_link(struct iproc_pcie *pcie)	/* NVMe: PHY/data link/header type/link status 점검; NVMe SSD 연결 확인 */
{	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */
	u32 hdr_type, link_ctrl, link_status, class, val;	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */
	bool link_is_active = false;	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */

	/*
	 * PAXC connects to emulated endpoint devices directly and does not
	 * have a Serdes.  Therefore skip the link detection logic here.
	 */
	if (pcie->ep_is_internal)	/* NVMe: 조건 분기; NVMe iproc_pcie_check_link 흐름 제어 */
		return 0;	/* NVMe: return; NVMe iproc_pcie_check_link 결과 반환/종료 */

	val = iproc_pcie_read_reg(pcie, IPROC_PCIE_LINK_STATUS);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
	if (!(val & PCIE_PHYLINKUP) || !(val & PCIE_DL_ACTIVE)) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "PHY or data link is INACTIVE!\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return -ENODEV;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */

	/* make sure we are not in EP mode */
	iproc_pci_raw_config_read32(pcie, 0, PCI_HEADER_TYPE, 1, &hdr_type);	/* NVMe: raw RC config read; NVMe 루트 포트 capability/링크 상태 읽기 */
	if ((hdr_type & PCI_HEADER_TYPE_MASK) != PCI_HEADER_TYPE_BRIDGE) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "in EP mode, hdr=%#02x\n", hdr_type);	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return -EFAULT;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */

	/* force class to PCI_CLASS_BRIDGE_PCI_NORMAL (0x060400) */
#define PCI_BRIDGE_CTRL_REG_OFFSET	0x43c	/* NVMe: PCI_BRIDGE_CTRL_REG_OFFSET 매크로 정의; PAXC bridge 제어 레지스터 offset; NVMe 루트 포트 class 설정 */
#define PCI_BRIDGE_CTRL_REG_CLASS_MASK	0xffffff	/* NVMe: PCI_BRIDGE_CTRL_REG_CLASS_MASK 매크로 정의; bridge class 필드 마스크; NVMe 루트 포트 class overwrite */
	iproc_pci_raw_config_read32(pcie, 0, PCI_BRIDGE_CTRL_REG_OFFSET,	/* NVMe: raw RC config read; NVMe 루트 포트 capability/링크 상태 읽기 */
				    4, &class);	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */
	class &= ~PCI_BRIDGE_CTRL_REG_CLASS_MASK;	/* NVMe: class 변수/필드 갱신; NVMe iproc_pcie_check_link 상태/주소/제어값 설정 */
	class |= PCI_CLASS_BRIDGE_PCI_NORMAL;	/* NVMe: class 변수/필드 갱신; NVMe iproc_pcie_check_link 상태/주소/제어값 설정 */
	iproc_pci_raw_config_write32(pcie, 0, PCI_BRIDGE_CTRL_REG_OFFSET,	/* NVMe: raw RC config write; NVMe 루트 포트 class/링크 제어 쓰기 */
				     4, class);	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */

	/* check link status to see if link is active */
	iproc_pci_raw_config_read32(pcie, 0, IPROC_PCI_EXP_CAP + PCI_EXP_LNKSTA,	/* NVMe: raw RC config read; NVMe 루트 포트 capability/링크 상태 읽기 */
				    2, &link_status);	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */
	if (link_status & PCI_EXP_LNKSTA_NLW)	/* NVMe: 조건 분기; NVMe iproc_pcie_check_link 흐름 제어 */
		link_is_active = true;	/* NVMe: link_is_active 변수/필드 갱신; NVMe iproc_pcie_check_link 상태/주소/제어값 설정 */

	if (!link_is_active) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		/* try GEN 1 link speed */
#define PCI_TARGET_LINK_SPEED_MASK	0xf	/* NVMe: PCI_TARGET_LINK_SPEED_MASK 매크로 정의; target link speed 필드 마스크; NVMe Gen1/Gen2 선택 */
#define PCI_TARGET_LINK_SPEED_GEN2	0x2	/* NVMe: PCI_TARGET_LINK_SPEED_GEN2 매크로 정의; Gen2 target link speed; NVMe Gen2 링크 시도 */
#define PCI_TARGET_LINK_SPEED_GEN1	0x1	/* NVMe: PCI_TARGET_LINK_SPEED_GEN1 매크로 정의; Gen1 target link speed; NVMe Gen1 fallback */
		iproc_pci_raw_config_read32(pcie, 0,	/* NVMe: raw RC config read; NVMe 루트 포트 capability/링크 상태 읽기 */
					    IPROC_PCI_EXP_CAP + PCI_EXP_LNKCTL2,	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
					    4, &link_ctrl);	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
		if ((link_ctrl & PCI_TARGET_LINK_SPEED_MASK) ==	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		    PCI_TARGET_LINK_SPEED_GEN2) {	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
			link_ctrl &= ~PCI_TARGET_LINK_SPEED_MASK;	/* NVMe: link_ctrl 변수/필드 갱신; NVMe block 상태/주소/제어값 설정 */
			link_ctrl |= PCI_TARGET_LINK_SPEED_GEN1;	/* NVMe: link_ctrl 변수/필드 갱신; NVMe block 상태/주소/제어값 설정 */
			iproc_pci_raw_config_write32(pcie, 0,	/* NVMe: raw RC config write; NVMe 루트 포트 class/링크 제어 쓰기 */
					IPROC_PCI_EXP_CAP + PCI_EXP_LNKCTL2,	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
					4, link_ctrl);	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
			msleep(100);	/* NVMe: 밀리초 지연; NVMe PERST 해제 후 링크 트레이닝 대기 */

			iproc_pci_raw_config_read32(pcie, 0,	/* NVMe: raw RC config read; NVMe 루트 포트 capability/링크 상태 읽기 */
					IPROC_PCI_EXP_CAP + PCI_EXP_LNKSTA,	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
					2, &link_status);	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
			if (link_status & PCI_EXP_LNKSTA_NLW)	/* NVMe: 조건 분기; NVMe block 흐름 제어 */
				link_is_active = true;	/* NVMe: link_is_active 변수/필드 갱신; NVMe block 상태/주소/제어값 설정 */
		}	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
	}	/* NVMe: iproc_pcie_check_link 내부 동작; NVMe PCIe host 처리 */

	dev_info(dev, "link: %s\n", link_is_active ? "UP" : "DOWN");	/* NVMe: 정보 메시지 출력; NVMe link 상태/MSI 설정 보고 */

	return link_is_active ? 0 : -ENODEV;	/* NVMe: return; NVMe iproc_pcie_check_link 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static void iproc_pcie_enable(struct iproc_pcie *pcie)	/* NVMe: INTx A/B/C/D enable; NVMe legacy 인터럽트 경로 활성화 */
{	/* NVMe: iproc_pcie_enable 내부 동작; NVMe PCIe host 처리 */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_INTX_EN, SYS_RC_INTX_MASK);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline bool iproc_pcie_ob_is_valid(struct iproc_pcie *pcie,	/* NVMe: OARR valid 비트 검사; NVMe outbound MMIO 윈도우 사용 여부 */
					  int window_idx)	/* NVMe: iproc_pcie_ob_is_valid 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_ob_is_valid 내부 동작; NVMe PCIe host 처리 */
	u32 val;	/* NVMe: iproc_pcie_ob_is_valid 내부 동작; NVMe PCIe host 처리 */

	val = iproc_pcie_read_reg(pcie, MAP_REG(IPROC_PCIE_OARR0, window_idx));	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */

	return !!(val & OARR_VALID);	/* NVMe: return; NVMe iproc_pcie_ob_is_valid 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline int iproc_pcie_ob_write(struct iproc_pcie *pcie, int window_idx,	/* NVMe: OARR/OMAP에 AXI/PCI 주소 기록; NVMe BAR/MMIO outbound 변환 설정 */
				      int size_idx, u64 axi_addr, u64 pci_addr)	/* NVMe: iproc_pcie_ob_write 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_ob_write 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_ob_write 내부 동작; NVMe PCIe host 처리 */
	u16 oarr_offset, omap_offset;	/* NVMe: iproc_pcie_ob_write 내부 동작; NVMe PCIe host 처리 */

	/*
	 * Derive the OARR/OMAP offset from the first pair (OARR0/OMAP0) based
	 * on window index.
	 */
	oarr_offset = iproc_pcie_reg_offset(pcie, MAP_REG(IPROC_PCIE_OARR0,	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */
							  window_idx));	/* NVMe: iproc_pcie_ob_write 내부 동작; NVMe PCIe host 처리 */
	omap_offset = iproc_pcie_reg_offset(pcie, MAP_REG(IPROC_PCIE_OMAP0,	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */
							  window_idx));	/* NVMe: iproc_pcie_ob_write 내부 동작; NVMe PCIe host 처리 */
	if (iproc_pcie_reg_is_invalid(oarr_offset) ||	/* NVMe: 조건 분기; NVMe iproc_pcie_ob_write 흐름 제어 */
	    iproc_pcie_reg_is_invalid(omap_offset))	/* NVMe: 레지스터 유효성 확인; NVMe 미지원 controller에서 접근 방지 */
		return -EINVAL;	/* NVMe: return; NVMe iproc_pcie_ob_write 결과 반환/종료 */

	/*
	 * Program the OARR registers.  The upper 32-bit OARR register is
	 * always right after the lower 32-bit OARR register.
	 */
	writel(lower_32_bits(axi_addr) | (size_idx << OARR_SIZE_CFG_SHIFT) |	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */
	       OARR_VALID, pcie->base + oarr_offset);	/* NVMe: iproc_pcie_ob_write 내부 동작; NVMe PCIe host 처리 */
	writel(upper_32_bits(axi_addr), pcie->base + oarr_offset + 4);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */

	/* now program the OMAP registers */
	writel(lower_32_bits(pci_addr), pcie->base + omap_offset);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */
	writel(upper_32_bits(pci_addr), pcie->base + omap_offset + 4);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */

	dev_dbg(dev, "ob window [%d]: offset 0x%x axi %pap pci %pap\n",	/* NVMe: 디버그 메시지 출력; NVMe 매핑/MSI 디버깅 */
		window_idx, oarr_offset, &axi_addr, &pci_addr);	/* NVMe: iproc_pcie_ob_write 내부 동작; NVMe PCIe host 처리 */
	dev_dbg(dev, "oarr lo 0x%x oarr hi 0x%x\n",	/* NVMe: 디버그 메시지 출력; NVMe 매핑/MSI 디버깅 */
		readl(pcie->base + oarr_offset),	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
		readl(pcie->base + oarr_offset + 4));	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
	dev_dbg(dev, "omap lo 0x%x omap hi 0x%x\n",	/* NVMe: 디버그 메시지 출력; NVMe 매핑/MSI 디버깅 */
		readl(pcie->base + omap_offset),	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
		readl(pcie->base + omap_offset + 4));	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */

	return 0;	/* NVMe: return; NVMe iproc_pcie_ob_write 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

/*
 * Some iProc SoCs require the SW to configure the outbound address mapping
 *
 * Outbound address translation:
 *
 * iproc_pcie_address = axi_address - axi_offset
 * OARR = iproc_pcie_address
 * OMAP = pci_addr
 *
 * axi_addr -> iproc_pcie_address -> OARR -> OMAP -> pci_address
 */
static int iproc_pcie_setup_ob(struct iproc_pcie *pcie, u64 axi_addr,	/* NVMe: resource 기반 outbound 매핑 설정; NVMe BAR가 PCI 주소 공간에 노출되도록 구성 */
			       u64 pci_addr, resource_size_t size)	/* NVMe: iproc_pcie_setup_ob 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie_ob *ob = &pcie->ob;	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */
	int ret = -EINVAL, window_idx, size_idx;	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */

	if (axi_addr < ob->axi_offset) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "axi address %pap less than offset %pap\n",	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
			&axi_addr, &ob->axi_offset);	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
		return -EINVAL;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */

	/*
	 * Translate the AXI address to the internal address used by the iProc
	 * PCIe core before programming the OARR
	 */
	axi_addr -= ob->axi_offset;	/* NVMe: axi_addr 변수/필드 갱신; NVMe iproc_pcie_setup_ob 상태/주소/제어값 설정 */

	/* iterate through all OARR/OMAP mapping windows */
	for (window_idx = ob->nr_windows - 1; window_idx >= 0; window_idx--) {	/* NVMe: 반복문; NVMe for 리소스/윈도우 순회 */
		const struct iproc_pcie_ob_map *ob_map =	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
			&pcie->ob_map[window_idx];	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */

		/*
		 * If current outbound window is already in use, move on to the
		 * next one.
		 */
		if (iproc_pcie_ob_is_valid(pcie, window_idx))	/* NVMe: 조건 분기; NVMe for 흐름 제어 */
			continue;	/* NVMe: continue; NVMe 다음 반복으로 이동 */

		/*
		 * Iterate through all supported window sizes within the
		 * OARR/OMAP pair to find a match.  Go through the window sizes
		 * in a descending order.
		 */
		for (size_idx = ob_map->nr_sizes - 1; size_idx >= 0;	/* NVMe: 반복문; NVMe for 리소스/윈도우 순회 */
		     size_idx--) {	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
			resource_size_t window_size =	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
				ob_map->window_sizes[size_idx] * SZ_1M;	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */

			/*
			 * Keep iterating until we reach the last window and
			 * with the minimal window size at index zero. In this
			 * case, we take a compromise by mapping it using the
			 * minimum window size that can be supported
			 */
			if (size < window_size) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
				if (size_idx > 0 || window_idx > 0)	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
					continue;	/* NVMe: continue; NVMe 다음 반복으로 이동 */

				/*
				 * For the corner case of reaching the minimal
				 * window size that can be supported on the
				 * last window
				 */
				axi_addr = ALIGN_DOWN(axi_addr, window_size);	/* NVMe: axi_addr 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
				pci_addr = ALIGN_DOWN(pci_addr, window_size);	/* NVMe: pci_addr 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
				size = window_size;	/* NVMe: size 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
			}	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */

			if (!IS_ALIGNED(axi_addr, window_size) ||	/* NVMe: 조건 분기; NVMe block 흐름 제어 */
			    !IS_ALIGNED(pci_addr, window_size)) {	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
				dev_err(dev,	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
					"axi %pap or pci %pap not aligned\n",	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
					&axi_addr, &pci_addr);	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
				return -EINVAL;	/* NVMe: return; NVMe block 결과 반환/종료 */
			}	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */

			/*
			 * Match found!  Program both OARR and OMAP and mark
			 * them as a valid entry.
			 */
			ret = iproc_pcie_ob_write(pcie, window_idx, size_idx,	/* NVMe: OARR/OMAP 레지스터 기록; NVMe MMIO outbound 주소 변환 설정 */
						  axi_addr, pci_addr);	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
			if (ret)	/* NVMe: 조건 분기; NVMe block 흐름 제어 */
				goto err_ob;	/* NVMe: goto; NVMe block 오류 처리/정리 경로 이동 */

			size -= window_size;	/* NVMe: size 변수/필드 갱신; NVMe block 상태/주소/제어값 설정 */
			if (size == 0)	/* NVMe: 조건 분기; NVMe block 흐름 제어 */
				return 0;	/* NVMe: return; NVMe block 결과 반환/종료 */

			/*
			 * If we are here, we are done with the current window,
			 * but not yet finished all mappings.  Need to move on
			 * to the next window.
			 */
			axi_addr += window_size;	/* NVMe: axi_addr 변수/필드 갱신; NVMe block 상태/주소/제어값 설정 */
			pci_addr += window_size;	/* NVMe: pci_addr 변수/필드 갱신; NVMe block 상태/주소/제어값 설정 */
			break;	/* NVMe: break; NVMe 처리 블록 종료 */
		}	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
	}	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */

err_ob:	/* NVMe: 레이블; NVMe iproc_pcie_setup_ob 오류 처리 진입점 */
	dev_err(dev, "unable to configure outbound mapping\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
	dev_err(dev,	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		"axi %pap, axi offset %pap, pci %pap, res size %pap\n",	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */
		&axi_addr, &ob->axi_offset, &pci_addr, &size);	/* NVMe: iproc_pcie_setup_ob 내부 동작; NVMe PCIe host 처리 */

	return ret;	/* NVMe: return; NVMe iproc_pcie_setup_ob 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_map_ranges(struct iproc_pcie *pcie,	/* NVMe: host bridge windows 순회하며 outbound 설정; NVMe IORESOURCE_MEM 매핑 */
				 struct list_head *resources)	/* NVMe: iproc_pcie_map_ranges 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_map_ranges 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_map_ranges 내부 동작; NVMe PCIe host 처리 */
	struct resource_entry *window;	/* NVMe: iproc_pcie_map_ranges 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_map_ranges 내부 동작; NVMe PCIe host 처리 */

	resource_list_for_each_entry(window, resources) {	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
		struct resource *res = window->res;	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
		u64 res_type = resource_type(res);	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */

		switch (res_type) {	/* NVMe: switch 분기; NVMe switch 상태/offset별 처리 */
		case IORESOURCE_IO:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		case IORESOURCE_BUS:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
			break;	/* NVMe: break; NVMe 처리 블록 종료 */
		case IORESOURCE_MEM:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
			ret = iproc_pcie_setup_ob(pcie, res->start,	/* NVMe: outbound 매핑 설정; NVMe BAR/MMIO PCI 주소 공간 연결 */
						  res->start - window->offset,	/* NVMe: switch 내부 동작; NVMe PCIe host 처리 */
						  resource_size(res));	/* NVMe: switch 내부 동작; NVMe PCIe host 처리 */
			if (ret)	/* NVMe: 조건 분기; NVMe switch 흐름 제어 */
				return ret;	/* NVMe: return; NVMe switch 결과 반환/종료 */
			break;	/* NVMe: break; NVMe 처리 블록 종료 */
		default:	/* NVMe: default 처리; NVMe switch 기타 경우 */
			dev_err(dev, "invalid resource %pR\n", res);	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
			return -EINVAL;	/* NVMe: return; NVMe switch 결과 반환/종료 */
		}	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
	}	/* NVMe: iproc_pcie_map_ranges 내부 동작; NVMe PCIe host 처리 */

	return 0;	/* NVMe: return; NVMe iproc_pcie_map_ranges 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline bool iproc_pcie_ib_is_in_use(struct iproc_pcie *pcie,	/* NVMe: IARR region 사용 비트 검사; NVMe DMA 영역 중복 할당 방지 */
					   int region_idx)	/* NVMe: iproc_pcie_ib_is_in_use 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_ib_is_in_use 내부 동작; NVMe PCIe host 처리 */
	const struct iproc_pcie_ib_map *ib_map = &pcie->ib_map[region_idx];	/* NVMe: iproc_pcie_ib_is_in_use 내부 동작; NVMe PCIe host 처리 */
	u32 val;	/* NVMe: iproc_pcie_ib_is_in_use 내부 동작; NVMe PCIe host 처리 */

	val = iproc_pcie_read_reg(pcie, MAP_REG(IPROC_PCIE_IARR0, region_idx));	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */

	return !!(val & (BIT(ib_map->nr_sizes) - 1));	/* NVMe: return; NVMe iproc_pcie_ib_is_in_use 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static inline bool iproc_pcie_ib_check_type(const struct iproc_pcie_ib_map *ib_map,	/* NVMe: IARR region 타입(MEM/IO) 확인; NVMe DMA/MSI 매핑 구분 */
					    enum iproc_pcie_ib_map_type type)	/* NVMe: iproc_pcie_ib_check_type 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_ib_check_type 내부 동작; NVMe PCIe host 처리 */
	return !!(ib_map->type == type);	/* NVMe: return; NVMe iproc_pcie_ib_check_type 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_ib_write(struct iproc_pcie *pcie, int region_idx,	/* NVMe: IARR/IMAP에 PCI/AXI 주소 기록; NVMe DMA/MSI inbound 변환 설정 */
			       int size_idx, int nr_windows, u64 axi_addr,	/* NVMe: iproc_pcie_ib_write 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
			       u64 pci_addr, resource_size_t size)	/* NVMe: iproc_pcie_ib_write 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	const struct iproc_pcie_ib_map *ib_map = &pcie->ib_map[region_idx];	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	u16 iarr_offset, imap_offset;	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	u32 val;	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	int window_idx;	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */

	iarr_offset = iproc_pcie_reg_offset(pcie,	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */
				MAP_REG(IPROC_PCIE_IARR0, region_idx));	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	imap_offset = iproc_pcie_reg_offset(pcie,	/* NVMe: 레지스터 offset 획득; NVMe controller 버전별 레지스터 접근 */
				MAP_REG(IPROC_PCIE_IMAP0, region_idx));	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	if (iproc_pcie_reg_is_invalid(iarr_offset) ||	/* NVMe: 조건 분기; NVMe iproc_pcie_ib_write 흐름 제어 */
	    iproc_pcie_reg_is_invalid(imap_offset))	/* NVMe: 레지스터 유효성 확인; NVMe 미지원 controller에서 접근 방지 */
		return -EINVAL;	/* NVMe: return; NVMe iproc_pcie_ib_write 결과 반환/종료 */

	dev_dbg(dev, "ib region [%d]: offset 0x%x axi %pap pci %pap\n",	/* NVMe: 디버그 메시지 출력; NVMe 매핑/MSI 디버깅 */
		region_idx, iarr_offset, &axi_addr, &pci_addr);	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */

	/*
	 * Program the IARR registers.  The upper 32-bit IARR register is
	 * always right after the lower 32-bit IARR register.
	 */
	writel(lower_32_bits(pci_addr) | BIT(size_idx),	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */
	       pcie->base + iarr_offset);	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	writel(upper_32_bits(pci_addr), pcie->base + iarr_offset + 4);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */

	dev_dbg(dev, "iarr lo 0x%x iarr hi 0x%x\n",	/* NVMe: 디버그 메시지 출력; NVMe 매핑/MSI 디버깅 */
		readl(pcie->base + iarr_offset),	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
		readl(pcie->base + iarr_offset + 4));	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */

	/*
	 * Now program the IMAP registers.  Each IARR region may have one or
	 * more IMAP windows.
	 */
	size >>= ilog2(nr_windows);	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */
	for (window_idx = 0; window_idx < nr_windows; window_idx++) {	/* NVMe: 반복문; NVMe for 리소스/윈도우 순회 */
		val = readl(pcie->base + imap_offset);	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
		val |= lower_32_bits(axi_addr) | IMAP_VALID;	/* NVMe: val 변수/필드 갱신; NVMe for 상태/주소/제어값 설정 */
		writel(val, pcie->base + imap_offset);	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */
		writel(upper_32_bits(axi_addr),	/* NVMe: 레지스터 값 쓰기; NVMe PCIe host 동작 설정 */
		       pcie->base + imap_offset + ib_map->imap_addr_offset);	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */

		dev_dbg(dev, "imap window [%d] lo 0x%x hi 0x%x\n",	/* NVMe: 디버그 메시지 출력; NVMe 매핑/MSI 디버깅 */
			window_idx, readl(pcie->base + imap_offset),	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
			readl(pcie->base + imap_offset +	/* NVMe: 레지스터 값 읽기; NVMe controller 상태/링크/매핑 확인 */
			      ib_map->imap_addr_offset));	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */

		imap_offset += ib_map->imap_window_offset;	/* NVMe: imap_offset 변수/필드 갱신; NVMe for 상태/주소/제어값 설정 */
		axi_addr += size;	/* NVMe: axi_addr 변수/필드 갱신; NVMe for 상태/주소/제어값 설정 */
	}	/* NVMe: iproc_pcie_ib_write 내부 동작; NVMe PCIe host 처리 */

	return 0;	/* NVMe: return; NVMe iproc_pcie_ib_write 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_setup_ib(struct iproc_pcie *pcie,	/* NVMe: dma-ranges/inbound region 설정; NVMe DMA 버퍼/MSI 영역 SoC 메모리 연결 */
			       struct resource_entry *entry,	/* NVMe: iproc_pcie_setup_ib 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
			       enum iproc_pcie_ib_map_type type)	/* NVMe: iproc_pcie_setup_ib 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie_ib *ib = &pcie->ib;	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	unsigned int region_idx, size_idx;	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	u64 axi_addr = entry->res->start;	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	u64 pci_addr = entry->res->start - entry->offset;	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	resource_size_t size = resource_size(entry->res);	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */

	/* iterate through all IARR mapping regions */
	for (region_idx = 0; region_idx < ib->nr_regions; region_idx++) {	/* NVMe: 반복문; NVMe for 리소스/윈도우 순회 */
		const struct iproc_pcie_ib_map *ib_map =	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
			&pcie->ib_map[region_idx];	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */

		/*
		 * If current inbound region is already in use or not a
		 * compatible type, move on to the next.
		 */
		if (iproc_pcie_ib_is_in_use(pcie, region_idx) ||	/* NVMe: 조건 분기; NVMe for 흐름 제어 */
		    !iproc_pcie_ib_check_type(ib_map, type))	/* NVMe: inbound region 타입 확인; NVMe DMA/IO 매핑 구분 */
			continue;	/* NVMe: continue; NVMe 다음 반복으로 이동 */

		/* iterate through all supported region sizes to find a match */
		for (size_idx = 0; size_idx < ib_map->nr_sizes; size_idx++) {	/* NVMe: 반복문; NVMe for 리소스/윈도우 순회 */
			resource_size_t region_size =	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
			ib_map->region_sizes[size_idx] * ib_map->size_unit;	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */

			if (size != region_size)	/* NVMe: 조건 분기; NVMe for 흐름 제어 */
				continue;	/* NVMe: continue; NVMe 다음 반복으로 이동 */

			if (!IS_ALIGNED(axi_addr, region_size) ||	/* NVMe: 조건 분기; NVMe for 흐름 제어 */
			    !IS_ALIGNED(pci_addr, region_size)) {	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
				dev_err(dev,	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
					"axi %pap or pci %pap not aligned\n",	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
					&axi_addr, &pci_addr);	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
				return -EINVAL;	/* NVMe: return; NVMe block 결과 반환/종료 */
			}	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */

			/* Match found!  Program IARR and all IMAP windows. */
			ret = iproc_pcie_ib_write(pcie, region_idx, size_idx,	/* NVMe: IARR/IMAP 레지스터 기록; NVMe DMA/MSI inbound 주소 변환 설정 */
						  ib_map->nr_windows, axi_addr,	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
						  pci_addr, size);	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
			if (ret)	/* NVMe: 조건 분기; NVMe for 흐름 제어 */
				goto err_ib;	/* NVMe: goto; NVMe for 오류 처리/정리 경로 이동 */
			else	/* NVMe: else 분기; NVMe for 대안 동작 수행 */
				return 0;	/* NVMe: return; NVMe for 결과 반환/종료 */

		}	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
	}	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */
	ret = -EINVAL;	/* NVMe: ret 변수/필드 갱신; NVMe iproc_pcie_setup_ib 상태/주소/제어값 설정 */

err_ib:	/* NVMe: 레이블; NVMe iproc_pcie_setup_ib 오류 처리 진입점 */
	dev_err(dev, "unable to configure inbound mapping\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
	dev_err(dev, "axi %pap, pci %pap, res size %pap\n",	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		&axi_addr, &pci_addr, &size);	/* NVMe: iproc_pcie_setup_ib 내부 동작; NVMe PCIe host 처리 */

	return ret;	/* NVMe: return; NVMe iproc_pcie_setup_ib 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_map_dma_ranges(struct iproc_pcie *pcie)	/* NVMe: host bridge dma_ranges 순회; NVMe DMA 주소 변환(IOMMU/ inbound) 구성 */
{	/* NVMe: iproc_pcie_map_dma_ranges 내부 동작; NVMe PCIe host 처리 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);	/* NVMe: iproc_pcie에서 host_bridge 획득; NVMe 도메인/버스 리소스 접근 */
	struct resource_entry *entry;	/* NVMe: iproc_pcie_map_dma_ranges 내부 동작; NVMe PCIe host 처리 */
	int ret = 0;	/* NVMe: iproc_pcie_map_dma_ranges 내부 동작; NVMe PCIe host 처리 */

	resource_list_for_each_entry(entry, &host->dma_ranges) {	/* NVMe: block 내부 동작; NVMe PCIe host 처리 */
		/* Each range entry corresponds to an inbound mapping region */
		ret = iproc_pcie_setup_ib(pcie, entry, IPROC_PCIE_IB_MAP_MEM);	/* NVMe: inbound 매핑 설정; NVMe DMA/IO 영역 SoC 메모리 연결 */
		if (ret)	/* NVMe: 조건 분기; NVMe block 흐름 제어 */
			break;	/* NVMe: break; NVMe 처리 블록 종료 */
	}	/* NVMe: iproc_pcie_map_dma_ranges 내부 동작; NVMe PCIe host 처리 */

	return ret;	/* NVMe: return; NVMe iproc_pcie_map_dma_ranges 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static void iproc_pcie_invalidate_mapping(struct iproc_pcie *pcie)	/* NVMe: OARR/IARR 레지스터 클리어; NVMe 재초기화/reset 시 stale 매핑 제거 */
{	/* NVMe: iproc_pcie_invalidate_mapping 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie_ib *ib = &pcie->ib;	/* NVMe: iproc_pcie_invalidate_mapping 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie_ob *ob = &pcie->ob;	/* NVMe: iproc_pcie_invalidate_mapping 내부 동작; NVMe PCIe host 처리 */
	int idx;	/* NVMe: iproc_pcie_invalidate_mapping 내부 동작; NVMe PCIe host 처리 */

	if (pcie->ep_is_internal)	/* NVMe: 조건 분기; NVMe iproc_pcie_invalidate_mapping 흐름 제어 */
		return;	/* NVMe: return; NVMe iproc_pcie_invalidate_mapping 결과 반환/종료 */

	if (pcie->need_ob_cfg) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		/* iterate through all OARR mapping regions */
		for (idx = ob->nr_windows - 1; idx >= 0; idx--) {	/* NVMe: 반복문; NVMe for 리소스/윈도우 순회 */
			iproc_pcie_write_reg(pcie,	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
					     MAP_REG(IPROC_PCIE_OARR0, idx), 0);	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
		}	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
	}	/* NVMe: iproc_pcie_invalidate_mapping 내부 동작; NVMe PCIe host 처리 */

	if (pcie->need_ib_cfg) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		/* iterate through all IARR mapping regions */
		for (idx = 0; idx < ib->nr_regions; idx++) {	/* NVMe: 반복문; NVMe for 리소스/윈도우 순회 */
			iproc_pcie_write_reg(pcie,	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
					     MAP_REG(IPROC_PCIE_IARR0, idx), 0);	/* NVMe: for 내부 동작; NVMe PCIe host 처리 */
		}	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
	}	/* NVMe: iproc_pcie_invalidate_mapping 내부 동작; NVMe PCIe host 처리 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproce_pcie_get_msi(struct iproc_pcie *pcie,	/* NVMe: DT에서 GICv3 ITS node를 찾아 GITS_TRANSLATER 주소 반환; NVMe MSI 목적지 */
			       struct device_node *msi_node,	/* NVMe: iproce_pcie_get_msi 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
			       u64 *msi_addr)	/* NVMe: iproce_pcie_get_msi 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproce_pcie_get_msi 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproce_pcie_get_msi 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproce_pcie_get_msi 내부 동작; NVMe PCIe host 처리 */
	struct resource res;	/* NVMe: iproce_pcie_get_msi 내부 동작; NVMe PCIe host 처리 */

	/*
	 * Check if 'msi-map' points to ARM GICv3 ITS, which is the only
	 * supported external MSI controller that requires steering.
	 */
	if (!of_device_is_compatible(msi_node, "arm,gic-v3-its")) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "unable to find compatible MSI controller\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return -ENODEV;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproce_pcie_get_msi 내부 동작; NVMe PCIe host 처리 */

	/* derive GITS_TRANSLATER address from GICv3 */
	ret = of_address_to_resource(msi_node, 0, &res);	/* NVMe: DT 주소를 resource로 변환; NVMe MSI controller(GITS_TRANSLATER) 주소 획득 */
	if (ret < 0) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "unable to obtain MSI controller resources\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return ret;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproce_pcie_get_msi 내부 동작; NVMe PCIe host 처리 */

	*msi_addr = res.start + GITS_TRANSLATER;
	return 0;	/* NVMe: return; NVMe iproce_pcie_get_msi 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_paxb_v2_msi_steer(struct iproc_pcie *pcie, u64 msi_addr)	/* NVMe: PAXB v2에서 MSI write를 ITS 영역으로 inbound 매핑; NVMe MSI 라우팅 */
{	/* NVMe: iproc_pcie_paxb_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_paxb_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */
	struct resource_entry entry;	/* NVMe: iproc_pcie_paxb_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */

	memset(&entry, 0, sizeof(entry));	/* NVMe: iproc_pcie_paxb_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */
	entry.res = &entry.__res;	/* NVMe: entry.res 변수/필드 갱신; NVMe iproc_pcie_paxb_v2_msi_steer 상태/주소/제어값 설정 */

	msi_addr &= ~(SZ_32K - 1);	/* NVMe: msi_addr 변수/필드 갱신; NVMe iproc_pcie_paxb_v2_msi_steer 상태/주소/제어값 설정 */
	entry.res->start = msi_addr;	/* NVMe: entry.res->start 변수/필드 갱신; NVMe iproc_pcie_paxb_v2_msi_steer 상태/주소/제어값 설정 */
	entry.res->end = msi_addr + SZ_32K - 1;	/* NVMe: entry.res->end 변수/필드 갱신; NVMe iproc_pcie_paxb_v2_msi_steer 상태/주소/제어값 설정 */

	ret = iproc_pcie_setup_ib(pcie, &entry, IPROC_PCIE_IB_MAP_IO);	/* NVMe: inbound 매핑 설정; NVMe DMA/IO 영역 SoC 메모리 연결 */
	return ret;	/* NVMe: return; NVMe iproc_pcie_paxb_v2_msi_steer 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static void iproc_pcie_paxc_v2_msi_steer(struct iproc_pcie *pcie, u64 msi_addr,	/* NVMe: PAXC v2 MSI steering 레지스터 설정; NVMe MSI를 ITS로 전달/차단 */
					 bool enable)	/* NVMe: iproc_pcie_paxc_v2_msi_steer 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_paxc_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */
	u32 val;	/* NVMe: iproc_pcie_paxc_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */

	if (!enable) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		/*
		 * Disable PAXC MSI steering. All write transfers will be
		 * treated as non-MSI transfers
		 */
		val = iproc_pcie_read_reg(pcie, IPROC_PCIE_MSI_EN_CFG);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
		val &= ~MSI_ENABLE_CFG;	/* NVMe: val 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
		iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_EN_CFG, val);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
		return;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_paxc_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */

	/*
	 * Program bits [43:13] of address of GITS_TRANSLATER register into
	 * bits [30:0] of the MSI base address register.  In fact, in all iProc
	 * based SoCs, all I/O register bases are well below the 32-bit
	 * boundary, so we can safely assume bits [43:32] are always zeros.
	 */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_BASE_ADDR,	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
			     (u32)(msi_addr >> 13));	/* NVMe: iproc_pcie_paxc_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */

	/* use a default 8K window size */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_WINDOW_SIZE, 0);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */

	/* steering MSI to GICv3 ITS */
	val = iproc_pcie_read_reg(pcie, IPROC_PCIE_MSI_GIC_MODE);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
	val |= GIC_V3_CFG;	/* NVMe: val 변수/필드 갱신; NVMe iproc_pcie_paxc_v2_msi_steer 상태/주소/제어값 설정 */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_GIC_MODE, val);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */

	/*
	 * Program bits [43:2] of address of GITS_TRANSLATER register into the
	 * iProc MSI address registers.
	 */
	msi_addr >>= 2;	/* NVMe: iproc_pcie_paxc_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_ADDR_HI,	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
			     upper_32_bits(msi_addr));	/* NVMe: iproc_pcie_paxc_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_ADDR_LO,	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
			     lower_32_bits(msi_addr));	/* NVMe: iproc_pcie_paxc_v2_msi_steer 내부 동작; NVMe PCIe host 처리 */

	/* enable MSI */
	val = iproc_pcie_read_reg(pcie, IPROC_PCIE_MSI_EN_CFG);	/* NVMe: iProc PCIe 내부 레지스터 읽기; NVMe 관련 config/링크/MSI 상태 확인 */
	val |= MSI_ENABLE_CFG;	/* NVMe: val 변수/필드 갱신; NVMe iproc_pcie_paxc_v2_msi_steer 상태/주소/제어값 설정 */
	iproc_pcie_write_reg(pcie, IPROC_PCIE_MSI_EN_CFG, val);	/* NVMe: iProc PCIe 내부 레지스터 쓰기; NVMe 링크/MSI/매핑 제어 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_msi_steer(struct iproc_pcie *pcie,	/* NVMe: controller 유형별 MSI steering 분기; NVMe MSI 경로 확립 */
				struct device_node *msi_node)	/* NVMe: iproc_pcie_msi_steer 매개변수 선언; NVMe PCIe host 호출 인터페이스 */
{	/* NVMe: iproc_pcie_msi_steer 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_msi_steer 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_msi_steer 내부 동작; NVMe PCIe host 처리 */
	u64 msi_addr;	/* NVMe: iproc_pcie_msi_steer 내부 동작; NVMe PCIe host 처리 */

	ret = iproce_pcie_get_msi(pcie, msi_node, &msi_addr);	/* NVMe: external MSI controller(GICv3 ITS) 주소 획득; NVMe MSI 목적지 설정 */
	if (ret < 0) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "msi steering failed\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return ret;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_msi_steer 내부 동작; NVMe PCIe host 처리 */

	switch (pcie->type) {	/* NVMe: switch 분기; NVMe switch 상태/offset별 처리 */
	case IPROC_PCIE_PAXB_V2:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		ret = iproc_pcie_paxb_v2_msi_steer(pcie, msi_addr);	/* NVMe: PAXB v2 MSI write를 ITS로 steering; NVMe MSI 라우팅 */
		if (ret)	/* NVMe: 조건 분기; NVMe switch 흐름 제어 */
			return ret;	/* NVMe: return; NVMe switch 결과 반환/종료 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	case IPROC_PCIE_PAXC_V2:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		iproc_pcie_paxc_v2_msi_steer(pcie, msi_addr, true);	/* NVMe: PAXC v2 MSI steering enable/disable; NVMe MSI 해석/전달 제어 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	default:	/* NVMe: default 처리; NVMe switch 기타 경우 */
		return -EINVAL;	/* NVMe: return; NVMe switch 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_msi_steer 내부 동작; NVMe PCIe host 처리 */

	return 0;	/* NVMe: return; NVMe iproc_pcie_msi_steer 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_msi_enable(struct iproc_pcie *pcie)	/* NVMe: MSI controller node 획득 및 iproc_msi_init; NVMe queue 인터럽트 활성화 */
{	/* NVMe: iproc_pcie_msi_enable 내부 동작; NVMe PCIe host 처리 */
	struct device_node *msi_node = NULL;	/* NVMe: iproc_pcie_msi_enable 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_msi_enable 내부 동작; NVMe PCIe host 처리 */

	/*
	 * Either the "msi-parent" or the "msi-map" phandle needs to exist
	 * for us to obtain the MSI node.
	 */
	of_msi_xlate(pcie->dev, &msi_node, 0);	/* NVMe: DT에서 MSI 노드 획득; NVMe MSI parent/ITS 파싱 */
	if (!msi_node)	/* NVMe: 조건 분기; NVMe iproc_pcie_msi_enable 흐름 제어 */
		return -ENODEV;	/* NVMe: return; NVMe iproc_pcie_msi_enable 결과 반환/종료 */

	/*
	 * Certain revisions of the iProc PCIe controller require additional
	 * configurations to steer the MSI writes towards an external MSI
	 * controller.
	 */
	if (pcie->need_msi_steer) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		ret = iproc_pcie_msi_steer(pcie, msi_node);	/* NVMe: controller별 MSI steering 분기; NVMe MSI 경로 확립 */
		if (ret)	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			goto out_put_node;	/* NVMe: goto; NVMe if 오류 처리/정리 경로 이동 */
	}	/* NVMe: iproc_pcie_msi_enable 내부 동작; NVMe PCIe host 처리 */

	/*
	 * If another MSI controller is being used, the call below should fail
	 * but that is okay
	 */
	ret = iproc_msi_init(pcie, msi_node);	/* NVMe: iProc MSI controller 초기화; NVMe MSI/MSI-X vector 할당 */

out_put_node:	/* NVMe: 레이블; NVMe iproc_pcie_msi_enable 오류 처리 진입점 */
	of_node_put(msi_node);	/* NVMe: DT node 참조 카운트 감소; NVMe MSI node 리소스 해제 */
	return ret;	/* NVMe: return; NVMe iproc_pcie_msi_enable 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static void iproc_pcie_msi_disable(struct iproc_pcie *pcie)	/* NVMe: iproc_msi_exit 호출; NVMe MSI 리소스 해제 */
{	/* NVMe: iproc_pcie_msi_disable 내부 동작; NVMe PCIe host 처리 */
	iproc_msi_exit(pcie);	/* NVMe: iProc MSI controller 종료; NVMe MSI vector 해제 */
}	/* NVMe: NVMe PCIe host controller 동작 */

static int iproc_pcie_rev_init(struct iproc_pcie *pcie)	/* NVMe: controller revision별 register table/flag 선택; NVMe 지원 형태 결정 */
{	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */
	struct device *dev = pcie->dev;	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */
	unsigned int reg_idx;	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */
	const u16 *regs;	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */

	switch (pcie->type) {	/* NVMe: switch 분기; NVMe switch 상태/offset별 처리 */
	case IPROC_PCIE_PAXB_BCMA:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		regs = iproc_pcie_reg_paxb_bcma;	/* NVMe: regs 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	case IPROC_PCIE_PAXB:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		regs = iproc_pcie_reg_paxb;	/* NVMe: regs 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->has_apb_err_disable = true;	/* NVMe: pcie->has_apb_err_disable 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		if (pcie->need_ob_cfg) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			pcie->ob_map = paxb_ob_map;	/* NVMe: pcie->ob_map 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
			pcie->ob.nr_windows = ARRAY_SIZE(paxb_ob_map);	/* NVMe: pcie->ob.nr_windows 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
		}	/* NVMe: switch 내부 동작; NVMe PCIe host 처리 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	case IPROC_PCIE_PAXB_V2:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		regs = iproc_pcie_reg_paxb_v2;	/* NVMe: regs 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->iproc_cfg_read = true;	/* NVMe: pcie->iproc_cfg_read 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->has_apb_err_disable = true;	/* NVMe: pcie->has_apb_err_disable 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		if (pcie->need_ob_cfg) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			pcie->ob_map = paxb_v2_ob_map;	/* NVMe: pcie->ob_map 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
			pcie->ob.nr_windows = ARRAY_SIZE(paxb_v2_ob_map);	/* NVMe: pcie->ob.nr_windows 변수/필드 갱신; NVMe if 상태/주소/제어값 설정 */
		}	/* NVMe: switch 내부 동작; NVMe PCIe host 처리 */
		pcie->ib.nr_regions = ARRAY_SIZE(paxb_v2_ib_map);	/* NVMe: pcie->ib.nr_regions 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->ib_map = paxb_v2_ib_map;	/* NVMe: pcie->ib_map 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->need_msi_steer = true;	/* NVMe: pcie->need_msi_steer 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		dev_warn(dev, "reads of config registers that contain %#x return incorrect data\n",	/* NVMe: 경고 메시지 출력; NVMe config read anomaly 보고 */
			 CFG_RETRY_STATUS);	/* NVMe: switch 내부 동작; NVMe PCIe host 처리 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	case IPROC_PCIE_PAXC:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		regs = iproc_pcie_reg_paxc;	/* NVMe: regs 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->ep_is_internal = true;	/* NVMe: pcie->ep_is_internal 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->iproc_cfg_read = true;	/* NVMe: pcie->iproc_cfg_read 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->rej_unconfig_pf = true;	/* NVMe: pcie->rej_unconfig_pf 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	case IPROC_PCIE_PAXC_V2:	/* NVMe: case 처리; NVMe switch 특정 조건 분기 */
		regs = iproc_pcie_reg_paxc_v2;	/* NVMe: regs 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->ep_is_internal = true;	/* NVMe: pcie->ep_is_internal 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->iproc_cfg_read = true;	/* NVMe: pcie->iproc_cfg_read 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->rej_unconfig_pf = true;	/* NVMe: pcie->rej_unconfig_pf 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		pcie->need_msi_steer = true;	/* NVMe: pcie->need_msi_steer 변수/필드 갱신; NVMe switch 상태/주소/제어값 설정 */
		break;	/* NVMe: break; NVMe 처리 블록 종료 */
	default:	/* NVMe: default 처리; NVMe switch 기타 경우 */
		dev_err(dev, "incompatible iProc PCIe interface\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return -EINVAL;	/* NVMe: return; NVMe switch 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */

	pcie->reg_offsets = devm_kcalloc(dev, IPROC_PCIE_MAX_NUM_REG,	/* NVMe: 커널 메모리 할당; NVMe host bridge register offset 테이블 */
					 sizeof(*pcie->reg_offsets),	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */
					 GFP_KERNEL);	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */
	if (!pcie->reg_offsets)	/* NVMe: 조건 분기; NVMe iproc_pcie_rev_init 흐름 제어 */
		return -ENOMEM;	/* NVMe: return; NVMe iproc_pcie_rev_init 결과 반환/종료 */

	/* go through the register table and populate all valid registers */
	pcie->reg_offsets[0] = (pcie->type == IPROC_PCIE_PAXC_V2) ?	/* NVMe: pcie->reg_offsets[0] 변수/필드 갱신; NVMe iproc_pcie_rev_init 상태/주소/제어값 설정 */
		IPROC_PCIE_REG_INVALID : regs[0];	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */
	for (reg_idx = 1; reg_idx < IPROC_PCIE_MAX_NUM_REG; reg_idx++)	/* NVMe: 반복문; NVMe iproc_pcie_rev_init 리소스/윈도우 순회 */
		pcie->reg_offsets[reg_idx] = regs[reg_idx] ?	/* NVMe: pcie->reg_offsets[reg_idx] 변수/필드 갱신; NVMe iproc_pcie_rev_init 상태/주소/제어값 설정 */
			regs[reg_idx] : IPROC_PCIE_REG_INVALID;	/* NVMe: iproc_pcie_rev_init 내부 동작; NVMe PCIe host 처리 */

	return 0;	/* NVMe: return; NVMe iproc_pcie_rev_init 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */

int iproc_pcie_setup(struct iproc_pcie *pcie, struct list_head *res)	/* NVMe: PHY/reset/mapping/link/MSI 초기화 후 pci_host_probe; NVMe PCIe 열거/바인딩 준비 */
{	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */
	struct device *dev;	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */
	int ret;	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */
	struct pci_dev *pdev;	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);	/* NVMe: iproc_pcie에서 host_bridge 획득; NVMe 도메인/버스 리소스 접근 */

	dev = pcie->dev;	/* NVMe: dev 변수/필드 갱신; NVMe iproc_pcie_setup 상태/주소/제어값 설정 */

	ret = iproc_pcie_rev_init(pcie);	/* NVMe: controller revision별 register/flag 초기화; NVMe 지원 여부 결정 */
	if (ret) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "unable to initialize controller parameters\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return ret;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	ret = phy_init(pcie->phy);	/* NVMe: PCIe PHY 초기화; NVMe 물리 링크 준비 */
	if (ret) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "unable to initialize PCIe PHY\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		return ret;	/* NVMe: return; NVMe if 결과 반환/종료 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	ret = phy_power_on(pcie->phy);	/* NVMe: PCIe PHY 전원 공급; NVMe 링크 업 전력/클록 활성화 */
	if (ret) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "unable to power on PCIe PHY\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		goto err_exit_phy;	/* NVMe: goto; NVMe if 오류 처리/정리 경로 이동 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	iproc_pcie_perst_ctrl(pcie, true);	/* NVMe: PERST# 신호 제어; NVMe 장치 reset/재열거 타이밍 */
	iproc_pcie_perst_ctrl(pcie, false);	/* NVMe: PERST# 신호 제어; NVMe 장치 reset/재열거 타이밍 */

	iproc_pcie_invalidate_mapping(pcie);	/* NVMe: 기존 OARR/IARR 매핑 무효화; NVMe 재초기화/reset 시 정리 */

	if (pcie->need_ob_cfg) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		ret = iproc_pcie_map_ranges(pcie, res);	/* NVMe: DT/ACPI resource 기반 outbound 매핑; NVMe BAR 리소스 할당 */
		if (ret) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			dev_err(dev, "map failed\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
			goto err_power_off_phy;	/* NVMe: goto; NVMe if 오류 처리/정리 경로 이동 */
		}	/* NVMe: if 내부 동작; NVMe PCIe host 처리 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	if (pcie->need_ib_cfg) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		ret = iproc_pcie_map_dma_ranges(pcie);	/* NVMe: dma-ranges 기반 inbound 매핑; NVMe DMA 주소 변환(IOMMU 연동) */
		if (ret && ret != -ENOENT)	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
			goto err_power_off_phy;	/* NVMe: goto; NVMe if 오류 처리/정리 경로 이동 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	ret = iproc_pcie_check_link(pcie);	/* NVMe: PCIe 링크 상태 확인; NVMe SSD 물리/데이터 링크 활성화 점검 */
	if (ret) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "no PCIe EP device detected\n");	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		goto err_power_off_phy;	/* NVMe: goto; NVMe if 오류 처리/정리 경로 이동 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	iproc_pcie_enable(pcie);	/* NVMe: INTx 인터럽트 enable; NVMe legacy INTx 경로 활성화 */

	if (IS_ENABLED(CONFIG_PCI_MSI))	/* NVMe: 조건 분기; NVMe iproc_pcie_setup 흐름 제어 */
		if (iproc_pcie_msi_enable(pcie))	/* NVMe: 조건 분기; NVMe iproc_pcie_setup 흐름 제어 */
			dev_info(dev, "not using iProc MSI\n");	/* NVMe: 정보 메시지 출력; NVMe link 상태/MSI 설정 보고 */

	host->ops = &iproc_pcie_ops;	/* NVMe: host->ops 변수/필드 갱신; NVMe iproc_pcie_setup 상태/주소/제어값 설정 */
	host->sysdata = pcie;	/* NVMe: host->sysdata 변수/필드 갱신; NVMe iproc_pcie_setup 상태/주소/제어값 설정 */
	host->map_irq = pcie->map_irq;	/* NVMe: host->map_irq 변수/필드 갱신; NVMe iproc_pcie_setup 상태/주소/제어값 설정 */

	ret = pci_host_probe(host);	/* NVMe: PCI host bridge probing 및 bus scan; NVMe 장치 탐색/바인딩 시작 */
	if (ret < 0) {	/* NVMe: 조건 분기; NVMe if 흐름 제어 */
		dev_err(dev, "failed to scan host: %d\n", ret);	/* NVMe: 에러 메시지 출력; NVMe host 초기화/매핑 실패 보고 */
		goto err_power_off_phy;	/* NVMe: goto; NVMe if 오류 처리/정리 경로 이동 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	for_each_pci_bridge(pdev, host->bus) {	/* NVMe: 루트 버스 하위 bridge 순회; NVMe 루트 포트 link status 출력 */
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT)	/* NVMe: 조건 분기; NVMe block 흐름 제어 */
			pcie_print_link_status(pdev);	/* NVMe: 루트 포트 link status 출력; NVMe 링크 속도/폭 확인 */
	}	/* NVMe: iproc_pcie_setup 내부 동작; NVMe PCIe host 처리 */

	return 0;	/* NVMe: return; NVMe iproc_pcie_setup 결과 반환/종료 */

err_power_off_phy:	/* NVMe: 레이블; NVMe iproc_pcie_setup 오류 처리 진입점 */
	phy_power_off(pcie->phy);	/* NVMe: PCIe PHY 전원 차단; NVMe host bridge 종료 시 전력 정리 */
err_exit_phy:	/* NVMe: 레이블; NVMe iproc_pcie_setup 오류 처리 진입점 */
	phy_exit(pcie->phy);	/* NVMe: PCIe PHY 종료; NVMe controller 물리 계층 리소스 해제 */
	return ret;	/* NVMe: return; NVMe iproc_pcie_setup 결과 반환/종료 */
}	/* NVMe: NVMe PCIe host controller 동작 */
EXPORT_SYMBOL(iproc_pcie_setup);	/* NVMe: NVMe PCIe host controller 동작 */

void iproc_pcie_remove(struct iproc_pcie *pcie)	/* NVMe: bus stop/remove 및 PHY 종료; NVMe 장치 안전 제거 */
{	/* NVMe: iproc_pcie_remove 내부 동작; NVMe PCIe host 처리 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie);	/* NVMe: iproc_pcie에서 host_bridge 획득; NVMe 도메인/버스 리소스 접근 */

	pci_stop_root_bus(host->bus);	/* NVMe: 루트 버스 중지; NVMe 장치 driver unbind 전 bus 정지 */
	pci_remove_root_bus(host->bus);	/* NVMe: 루트 버스 제거; NVMe bus 및 하위 장치 정리 */

	iproc_pcie_msi_disable(pcie);	/* NVMe: MSI 컨트롤러 종료; NVMe 인터럽트 리소스 해제 */

	phy_power_off(pcie->phy);	/* NVMe: PCIe PHY 전원 차단; NVMe host bridge 종료 시 전력 정리 */
	phy_exit(pcie->phy);	/* NVMe: PCIe PHY 종료; NVMe controller 물리 계층 리소스 해제 */
}	/* NVMe: NVMe PCIe host controller 동작 */
EXPORT_SYMBOL(iproc_pcie_remove);	/* NVMe: NVMe PCIe host controller 동작 */

/*
 * The MSI parsing logic in certain revisions of Broadcom PAXC based root
 * complex does not work and needs to be disabled
 */
static void quirk_paxc_disable_msi_parsing(struct pci_dev *pdev)	/* NVMe: PAXC MSI parsing 비활성화 quirk; NVMe MSI parsing 버그 우회 */
{	/* NVMe: quirk_paxc_disable_msi_parsing 내부 동작; NVMe PCIe host 처리 */
	struct iproc_pcie *pcie = iproc_data(pdev->bus);	/* NVMe: pci_bus->sysdata에서 iproc_pcie 획득; NVMe host bridge private data 접근 */

	if (pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE)	/* NVMe: 조건 분기; NVMe quirk_paxc_disable_msi_parsing 흐름 제어 */
		iproc_pcie_paxc_v2_msi_steer(pcie, 0, false);	/* NVMe: PAXC v2 MSI steering enable/disable; NVMe MSI 해석/전달 제어 */
}	/* NVMe: NVMe PCIe host controller 동작 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x16f0,	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */
			quirk_paxc_disable_msi_parsing);	/* NVMe: NVMe PCIe host controller 동작 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd802,	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */
			quirk_paxc_disable_msi_parsing);	/* NVMe: NVMe PCIe host controller 동작 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd804,	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */
			quirk_paxc_disable_msi_parsing);	/* NVMe: NVMe PCIe host controller 동작 */

static void quirk_paxc_bridge(struct pci_dev *pdev)	/* NVMe: PAXC bridge class/MPS quirk; NVMe 루트 포트 올바른 노출 */
{	/* NVMe: quirk_paxc_bridge 내부 동작; NVMe PCIe host 처리 */
	/*
	 * The PCI config space is shared with the PAXC root port and the first
	 * Ethernet device.  So, we need to workaround this by telling the PCI
	 * code that the bridge is not an Ethernet device.
	 */
	if (pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE)	/* NVMe: 조건 분기; NVMe quirk_paxc_bridge 흐름 제어 */
		pdev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;	/* NVMe: pdev->class 변수/필드 갱신; NVMe quirk_paxc_bridge 상태/주소/제어값 설정 */

	/*
	 * MPSS is not being set properly (as it is currently 0).  This is
	 * because that area of the PCI config space is hard coded to zero, and
	 * is not modifiable by firmware.  Set this to 2 (e.g., 512 byte MPS)
	 * so that the MPS can be set to the real max value.
	 */
	pdev->pcie_mpss = 2;	/* NVMe: pdev->pcie_mpss 변수/필드 갱신; NVMe quirk_paxc_bridge 상태/주소/제어값 설정 */
}	/* NVMe: NVMe PCIe host controller 동작 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x16cd, quirk_paxc_bridge);	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0x16f0, quirk_paxc_bridge);	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd750, quirk_paxc_bridge);	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd802, quirk_paxc_bridge);	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_BROADCOM, 0xd804, quirk_paxc_bridge);	/* NVMe: 초기 PCI quirk 등록; NVMe 특정 device id의 버그 우회 */

MODULE_AUTHOR("Ray Jui <rjui@broadcom.com>");	/* NVMe: 모듈 작성자 정보; NVMe PCIe host driver 메타데이터 */
MODULE_DESCRIPTION("Broadcom iPROC PCIe common driver");	/* NVMe: 모듈 설명; NVMe PCIe host controller 드라이버 설명 */
MODULE_LICENSE("GPL v2");	/* NVMe: 모듈 라이선스; NVMe PCIe host와 동일 GPL v2 정책 */
