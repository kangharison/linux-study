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
 * [한국어 설명] DesignWare PCIe IP 의 공통 코어 (pcie-designware.c)
 *
 * === 파일의 역할 ===
 * Synopsys DesignWare 는 PCIe 컨트롤러 IP 중 가장 널리 쓰이는 것이다.
 * 퀄컴, 삼성 엑시노스, 록칩, TI, 인텔, NXP 등 수많은 SoC 가 이 IP 를
 * 라이선스해 자기 칩에 넣는다. 그래서 dwc/ 디렉터리에만 40개가 넘는
 * 드라이버가 있는데, 그 전부가 공유하는 부분이 이 파일이다.
 *
 * SoC 마다 다른 것은 대개 주변부다 — 클럭, 리셋, 전원, PHY, 그리고
 * 레지스터가 어디에 매핑되는가. 링크를 올리고 config 접근을 하고 주소를
 * 변환하는 핵심 동작은 IP 가 같으므로 동일하다. 이 파일이 그 핵심을 맡고,
 * SoC 별 드라이버는 자기 주변부만 채워 넣는 구조다.
 *
 * 이 파일에서 가장 중요한 개념이 iATU(internal Address Translation Unit)다.
 * PCIe 는 CPU 가 보는 주소와 버스에 나가는 주소가 다를 수 있는데, 그 변환을
 * 하는 하드웨어다. 창(region)을 여러 개 두고 각각에
 *   "CPU 주소 A~B 로 들어온 접근을 PCIe 주소 C 로, 타입 T 로 내보내라"
 * 를 설정한다. MMIO 접근, config 접근, I/O 접근이 전부 이것을 거친다.
 *
 * iATU 창의 개수가 유한하다는 점이 실무에서 자주 문제가 된다. config
 * 접근을 할 때마다 창을 바꿔 써야 하는 구성에서는 그 재설정 비용이 붙고,
 * 그래서 ECAM 처럼 창 하나로 전체를 덮을 수 있으면 훨씬 빠르다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SoC 별 드라이버(pcie-qcom.c, pci-exynos.c, pcie-dw-rockchip.c ...)
 *   -> 자기 클럭·전원·PHY 를 켠 뒤
 *      -> dw_pcie_host_init() [pcie-designware-host.c]
 *         -> [이 파일] dw_pcie_setup(), iATU 설정, 링크 대기
 *            -> PCI 코어의 열거로 이어진다
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트가 대부분. 일부 레지스터
 * 접근 함수는 config 접근 경로에서 불려 잠금 아래에서 동작한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: dwc/ 아래의 모든 SoC 별 드라이버.
 * 옆쪽: pcie-designware-host.c(RC 모드), pcie-designware-ep.c(EP 모드).
 *   같은 IP 를 호스트로 쓰느냐 엔드포인트로 쓰느냐로 갈린다.
 * 아래쪽: PHY 서브시스템, 클럭·리셋 프레임워크, 그리고 PCI 코어.
 * 공유 상태: struct dw_pcie — IP 레지스터 베이스, iATU 정보, 링크 상태.
 *   SoC 드라이버가 이것을 자기 구조체에 품고 container_of 로 오간다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 하지만 임베디드/ARM 서버에서 NVMe 를 쓴다면 그 밑에 있는 것이 대개
 * 이 IP 다. NVMe 성능이 기대에 못 미칠 때 확인할 것이 링크 폭과 속도인데,
 * 그것을 읽고 설정하는 코드가 여기 있다(dw_pcie_link_up, 그리고 SoC
 * 드라이버가 부르는 속도 변경 함수들).
 *
 * MSI 도 관계가 있다. DesignWare 는 자체 MSI 컨트롤러를 갖고 있어서
 * (pcie-designware-host.c 의 dw_pcie_msi_* 계열) 표준 MSI 와 동작이
 * 조금 다르다. NVMe 가 큐마다 인터럽트를 요구할 때 그 벡터 개수 제한이
 * 이 IP 의 구성에 걸리는 경우가 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * dw_pcie_read() / dw_pcie_write() : IP 레지스터 접근의 기본. 정렬되지 않은
 *                         접근을 처리하고 오류를 보고한다.
 * dw_pcie_read_dbi() / dw_pcie_write_dbi() : DBI(Design Block Interface)
 *                         접근. 이 IP 자신의 config space 를 읽고 쓴다.
 * dw_pcie_prog_outbound_atu() : iATU 아웃바운드 창 설정. CPU -> PCIe 방향.
 * dw_pcie_prog_inbound_atu()  : 인바운드. PCIe -> 메모리 방향(EP 모드).
 * dw_pcie_link_up()       : 링크가 올라왔는지 확인.
 * dw_pcie_wait_for_link() : 링크를 기다린다. 타임아웃이 있다.
 * dw_pcie_setup()         : IP 의 기본 설정. 링크 폭 등을 정한다.
 * dw_pcie_find_capability() / dw_pcie_find_ext_capability() : capability 탐색.
 * struct dw_pcie          : 이 IP 인스턴스의 모든 상태.
 */

#include <linux/align.h>		/* PCI/NVMe: DMA descriptor, BAR, page alignment 처리 */
#include <linux/bitops.h>		/* PCI/NVMe: PCIe 링크/MSI/ATU 레지스터 비트 조작 */
#include <linux/clk.h>			/* PCI/NVMe: NVMe 장치 및 PCIe 링크 클럭 전원 관리 */
/* [한국어] msleep, usleep_range 등. 링크 트레이닝을 기다리고 리셋
 * 홀드 타임을 지키는 데 쓴다. */
#include <linux/delay.h>
/* [한국어] eDMA(embedded DMA) 관련 정의. DesignWare IP 는 DMA 엔진을
 * 내장할 수 있고, 이 파일이 그것을 커널 dmaengine 프레임워크에
 * 등록한다(아래 dw_pcie_edma_* 함수들). 실제로 쓰는 심볼은
 * DW_EDMA_CHIP_LOCAL, EDMA_MAX_RD_CH, EDMA_MAX_WR_CH,
 * EDMA_MF_EDMA_LEGACY, EDMA_MF_EDMA_UNROLL 등이다. */
#include <linux/dma/edma.h>
/* [한국어] GPIO 디스크립터 API. 슬롯의 PERST# 신호를 GPIO 로 다루는
 * 보드가 있어 필요하다. */
#include <linux/gpio/consumer.h>
/* [한국어] struct resource 와 resource_size 등. 디바이스 트리에서 얻은
 * 주소 범위를 다룬다. */
#include <linux/ioport.h>
/* [한국어] 디바이스 트리 속성 읽기(of_property_read_u32 등). */
#include <linux/of.h>
/* [한국어] 디바이스 트리의 주소 범위 변환. */
#include <linux/of_address.h>
/* [한국어] DesignWare IP 의 공개 정의. 벤더별 IP 버전 식별에 쓰인다. */
#include <linux/pcie-dwc.h>
/* [한국어] 플랫폼 장치 자원 조회(platform_get_resource_byname 등). */
#include <linux/platform_device.h>
/* [한국어] SZ_1K, SZ_4K, SZ_1G, SZ_4G 상수.
 * (이전 주석은 "PAGE_SIZE 등" 이라고 했으나, sizes.h 는 PAGE_SIZE 를
 *  정의하지 않고 이 파일도 그것을 쓰지 않는다. 실제로 쓰는 것은
 *  위 네 SZ_ 상수이며 주로 iATU 창 크기 계산에 등장한다.) */
#include <linux/sizes.h>
/* [한국어] u32, u64 등 기본 타입. */
#include <linux/types.h>

/* [한국어] PCI 코어 내부 헤더. PCI_FIND_NEXT_CAP 같은 내부 매크로를
 * 쓰기 위해 필요하다 — 이 IP 자신의 config space 를 훑어야 하는데
 * 그것은 아직 struct pci_dev 로 표현되지 않기 때문이다. */
#include "../../pci.h"
/* [한국어] struct dw_pcie 와 DesignWare 레지스터 상수, 그리고 이
 * 파일이 구현하는 함수들의 선언. */
#include "pcie-designware.h"

/* [한국어] 애플리케이션 클럭의 디바이스 트리 이름 표.
 *
 * DesignWare IP 는 클럭을 여러 개 받는데, 디바이스 트리에서는 그것들을
 * clock-names 속성의 문자열로 구분한다. 이 표가 열거값과 그 문자열을
 * 잇는다 — dw_pcie_get_clocks() 가 이 이름들로 클럭을 하나씩 찾는다.
 *
 * "애플리케이션" 과 "코어" 로 나눈 것은 IP 문서의 구분을 따른 것이다.
 * 애플리케이션 클럭은 IP 와 SoC 사이의 인터페이스(DBI, AXI)에,
 * 코어 클럭은 IP 내부와 PHY 에 관계한다. */
static const char * const dw_pcie_app_clks[DW_PCIE_NUM_APP_CLKS] = {
	/* [한국어] DBI(Design Block Interface) 클럭. 이 IP 자신의 config
	 * 레지스터에 접근하려면 이 클럭이 살아 있어야 한다. */
	[DW_PCIE_DBI_CLK] = "dbi",
	/* [한국어] AXI 마스터 클럭. 이 IP 가 AXI 버스의 마스터로서
	 * 시스템 메모리에 접근할 때 쓴다 — 장치의 DMA 가 지나는 길이다. */
	[DW_PCIE_MSTR_CLK] = "mstr",
	/* [한국어] AXI 슬레이브 클럭. CPU 가 이 IP 를 통해 PCIe 로 나갈 때
	 * 쓴다 — MMIO 와 config 접근이 지나는 길이다. */
	[DW_PCIE_SLV_CLK] = "slv",
};

/* [한국어] 코어 클럭의 디바이스 트리 이름 표. 위 애플리케이션 클럭과
 * 같은 방식이며 IP 내부와 PHY 쪽 클럭들이다. */
static const char * const dw_pcie_core_clks[DW_PCIE_NUM_CORE_CLKS] = {
	/* [한국어] PIPE 인터페이스 클럭. PIPE(PHY Interface for PCI Express)는
	 * IP 와 PHY 사이의 표준 인터페이스이며, 그 구간의 클럭이다. */
	[DW_PCIE_PIPE_CLK] = "pipe",
	/* [한국어] IP 내부 로직 클럭. */
	[DW_PCIE_CORE_CLK] = "core",
	/* [한국어] 보조 클럭. 주 클럭이 꺼지는 저전력 상태에서도 링크
	 * 상태를 유지하는 데 쓰인다 — 그래서 ASPM 과 관계가 있다. */
	[DW_PCIE_AUX_CLK] = "aux",
	/* [한국어] 레퍼런스 클럭. PCIe 규격이 요구하는 100MHz 기준 클럭으로,
	 * 링크 양쪽이 같은 기준으로 동작하게 한다. */
	[DW_PCIE_REF_CLK] = "ref",
};

/* [한국어] 애플리케이션 리셋의 디바이스 트리 이름 표.
 * 클럭과 같은 구조이며, dw_pcie_get_resets() 가 이 이름들로 리셋
 * 컨트롤러 핸들을 얻는다. 클럭과 리셋이 같은 이름을 쓰는 것은
 * 같은 기능 블록에 대응하기 때문이다. */
static const char * const dw_pcie_app_rsts[DW_PCIE_NUM_APP_RSTS] = {
	/* [한국어] DBI 블록 리셋. */
	[DW_PCIE_DBI_RST] = "dbi",
	/* [한국어] AXI 마스터 블록 리셋. */
	[DW_PCIE_MSTR_RST] = "mstr",
	/* [한국어] AXI 슬레이브 블록 리셋. */
	[DW_PCIE_SLV_RST] = "slv",
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

/* [한국어]
 * dw_pcie_get_clocks - 디바이스 트리에서 이 IP 의 클럭들을 확보한다
 *
 * @pci: 대상 컨트롤러.
 * @return: 0 이면 성공. 실패 시 음수(-EPROBE_DEFER 포함).
 *
 * 위에 정의한 이름 표를 clk_bulk 구조체에 채운 뒤 한 번에 확보한다.
 * 하나씩 devm_clk_get 하는 대신 bulk API 를 쓰면 코드가 짧아지고
 * 에러 처리가 한 곳으로 모인다.
 *
 * optional 판을 쓰는 것이 요점이다. SoC 마다 노출하는 클럭이 달라서,
 * 없는 클럭은 NULL 로 두고 나중에 clk_bulk_enable 이 조용히 건너뛴다.
 * 즉 "없는 것" 과 "오류" 를 구분해 준다.
 *
 * 실행 컨텍스트: probe — 프로세스 컨텍스트. 클럭 확보가 잠들 수 있고
 *   해당 클럭 드라이버가 아직 없으면 -EPROBE_DEFER 가 나와 나중에
 *   probe 가 재시도된다.
 *
 * 에러 경로: devm 이라 실패해도 앞서 확보한 것은 자동 해제된다.
 *
 * 호출 체인:
 *   dw_pcie_get_resources() → [이 함수] → devm_clk_bulk_get_optional()
 */
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

/* [한국어]
 * dw_pcie_get_resets - 리셋 컨트롤과 PERST# GPIO 를 확보한다
 *
 * @pci: 대상 컨트롤러.
 * @return: 0 이면 성공. 실패 시 음수.
 *
 * 위 클럭 확보와 같은 구조이되 두 가지가 다르다.
 *
 * 첫째, 애플리케이션 리셋은 shared, 코어 리셋은 exclusive 로 얻는다.
 * shared 는 다른 장치와 같은 리셋 선을 공유해도 된다는 뜻이고,
 * exclusive 는 이 드라이버만 그 선을 제어한다는 뜻이다. 코어 리셋을
 * 남과 나눠 쓰면 다른 장치가 이 IP 를 리셋해 버릴 수 있어 배타로 잡는다.
 *
 * 둘째, 마지막에 PERST# GPIO 를 함께 얻는다. PERST# 는 슬롯의 장치에게
 * "리셋 상태를 유지하라" 를 알리는 신호로, 리셋 컨트롤러가 아니라
 * GPIO 로 배선된 보드가 많다. GPIOD_OUT_HIGH 로 얻는 것은 확보하는
 * 순간 리셋을 걸어 둔다는 뜻이며, 나중에 전원과 클럭이 안정된 뒤
 * 떼어 링크를 시작시킨다.
 *
 * 실행 컨텍스트: probe — 프로세스 컨텍스트. -EPROBE_DEFER 가 나올 수 있다.
 *
 * 호출 체인:
 *   dw_pcie_get_resources() → [이 함수]
 *     → devm_reset_control_bulk_get_optional_shared/_exclusive()
 *     → devm_gpiod_get_optional()
 */
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

/* [한국어]
 * dw_pcie_get_resources - 이 IP 가 쓸 모든 자원을 디바이스 트리에서 얻는다
 *
 * @pci: 대상 컨트롤러. SoC 드라이버가 일부를 미리 채워 둘 수 있다.
 * @return: 0 이면 성공. 실패 시 음수.
 *
 * dwc/ 의 모든 SoC 드라이버가 초기화 초입에 부르는 함수다. 다섯 개의
 * MMIO 영역과 클럭·리셋을 확보한다.
 *
 * 각 영역이 무엇인지:
 *   dbi  — Design Block Interface. 이 IP 자신의 config space 다.
 *          레지스터 설정이 거의 다 여기를 거친다. 필수.
 *   dbi2 — 두 번째 DBI 창. 엔드포인트 모드에서 BAR 마스크를 쓸 때
 *          필요하다. 없으면 dbi + 4KB 를 기본값으로 쓴다.
 *   atu  — iATU 레지스터. 별도 창이 없는 구형 IP 는 DBI 안에 있어
 *          기본 오프셋을 더해 쓴다.
 *   dma  — 내장 eDMA 레지스터. 없으면 ATU 창 안의 오프셋을 쓴다.
 *   elbi — SoC 고유의 제어·상태 레지스터. 선택.
 *
 * "이미 채워져 있으면 건너뛴다"(!pci->dbi_base 같은 검사)는 패턴이
 * 반복되는데, SoC 드라이버가 자기 방식으로 미리 매핑해 두는 경우를
 * 위한 것이다.
 *
 * 클럭과 리셋은 REQ_RES capability 가 켜진 드라이버에 대해서만 얻는다.
 * 그렇지 않은 드라이버는 자기가 직접 관리하겠다는 뜻이다.
 *
 * 실행 컨텍스트: probe — 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 단계든 실패하면 곧바로 물러난다. 매핑은 전부 devm
 *   이라 자동 해제된다.
 *
 * 호출 체인:
 *   SoC 드라이버 probe → [이 함수]
 *     → devm_pci_remap_cfg_resource() / devm_ioremap_resource()
 *     → dw_pcie_get_clocks() → dw_pcie_get_resets()
 */
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

	/* [한국어] CDM 은 Configuration Dependent Module 의 약자다
	 * (디바이스 트리 바인딩 문서 snps,dw-pcie-common.yaml 에서 확인).
	 * 표준 PCIe config 레지스터를 포함한 그 레지스터들이 손상되지
	 * 않았는지 하드웨어가 자동으로 검사하게 하는 기능이며,
	 * 이 속성이 있으면 capability 비트를 세워 나중에 켠다.
	 * (이전 주석은 "Correctable/Detectable?" 이라고 물음표를 달아
	 *  추측하고 있었다.) */
	if (of_property_read_bool(np, "snps,enable-cdm-check"))			/* NVMe: CDM 체크 요청 */
		dw_pcie_cap_set(pci, CDM_CHECK);					/* NVMe: 레지스터 무결성 검사 활성화 */

	return 0;									/* NVMe: 리소스 획득 성공, NVMe 열거 진행 가능 */
}

/* [한국어]
 * dw_pcie_version_detect - IP 코어의 버전과 타입을 읽어 기록한다
 *
 * @pci: 대상 컨트롤러.
 * @return: 없음.
 *
 * DesignWare IP 는 세대마다 레지스터 배치와 동작이 조금씩 달라서,
 * 드라이버가 버전을 알아야 그에 맞게 처리할 수 있다. 이 함수가 그
 * 버전을 하드웨어에서 읽어 pci->version 에 기록한다.
 *
 * 상류 주석이 밝히는 사정이 중요하다 — v4.70a 보다 오래된 IP 는 이
 * 레지스터가 0 이다. 그래서 0 이면 "구형" 으로 보고 버전 기반 처리를
 * 하지 않고 물러난다.
 *
 * SoC 드라이버가 미리 버전을 지정해 둔 경우 읽은 값과 비교해 다르면
 * 경고를 남긴다. 디바이스 트리나 드라이버의 정보가 실제 하드웨어와
 * 어긋났다는 뜻이라 진단에 도움이 된다.
 *
 * 실행 컨텍스트: probe — 프로세스 컨텍스트. DBI 클럭이 켜진 뒤여야 한다.
 *
 * 호출 체인:
 *   SoC 드라이버 또는 host/ep 초기화 → [이 함수] → dw_pcie_readl_dbi()
 */
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
/* [한국어]
 * dw_pcie_find_capability - 이 IP 의 config space 에서 capability 를 찾는다
 *
 * @pci: 대상 컨트롤러.
 * @cap: 찾을 capability ID(PCI_CAP_ID_EXP 등).
 * @return: 그 오프셋. 없으면 0.
 *
 * 커널의 pci_find_capability() 를 쓸 수 없다. 그 함수는 struct pci_dev
 * 를 받는데, 여기서 보려는 것은 아직 PCI 장치로 등록되지 않은 이 IP
 * 자신의 config space(DBI)이기 때문이다.
 *
 * 그래서 PCI 코어 내부 매크로에 DBI 전용 읽기 함수를 넘겨 같은 탐색
 * 논리를 재사용한다. capability 가 연결 리스트라는 구조는 같으므로
 * 읽는 방법만 바꿔 끼우면 된다. cadence 의 같은 자리도 동일한 기법을 쓴다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. DBI 접근이라 잠들지 않는다.
 *
 * 호출 체인:
 *   dwc/ 의 여러 초기화 코드 → [이 함수] → PCI_FIND_NEXT_CAP()
 */
u8 dw_pcie_find_capability(struct dw_pcie *pci, u8 cap)
{
	return PCI_FIND_NEXT_CAP(dw_pcie_read_cfg, PCI_CAPABILITY_LIST, cap,	/* NVMe: linked list 순회, NVMe 기능 캡 탐색 */
				 NULL, pci);						/* NVMe: RC 자체 config space 탐색 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_capability);

/* PCI/NVMe: PCIe extended capability 탐색, NVMe AER/ACS/SR-IOV 등 */
/* [한국어]
 * dw_pcie_find_ext_capability - 확장 capability 를 찾는다
 *
 * @pci: 대상 컨트롤러.
 * @cap: 찾을 확장 capability ID(PCI_EXT_CAP_ID_ERR 등).
 * @return: 그 오프셋. 없으면 0.
 *
 * 위 함수의 확장판이다. 확장 capability 는 config space 의 0x100
 * 이후에 별도의 연결 리스트로 놓여 있어 시작점과 항목 형식이 다르다.
 * 시작 오프셋 0 을 넘기면 매크로가 0x100 부터 시작한다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_remove_ext_capability(), __dw_pcie_find_vsec_capability() 등
 *     → [이 함수] → PCI_FIND_NEXT_EXT_CAP()
 */
u16 dw_pcie_find_ext_capability(struct dw_pcie *pci, u8 cap)
{
	return PCI_FIND_NEXT_EXT_CAP(dw_pcie_read_cfg, 0, cap, NULL, pci);	/* NVMe: 256바이트 이후 확장 캡 리스트 순회 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_ext_capability);

/* [한국어]
 * dw_pcie_remove_capability - capability 하나를 목록에서 떼어 낸다
 *
 * @pci: 대상 컨트롤러.
 * @cap: 없앨 capability ID.
 * @return: 없음. 그 capability 가 없으면 조용히 물러난다.
 *
 * 하드웨어가 가진 기능을 소프트웨어가 감추는 함수다. 왜 그런 일이
 * 필요한가 — IP 는 그 기능을 구현했다고 광고하는데 이 SoC 의 배선이나
 * 통합 방식 때문에 실제로는 동작하지 않는 경우가 있다. 그대로 두면
 * 호스트 쪽 소프트웨어가 그것을 믿고 쓰려다 문제가 생긴다.
 *
 * 없애는 방법은 실제로 지우는 것이 아니라 연결 리스트에서 건너뛰게
 * 하는 것이다. 앞 항목의 next 포인터를 이 항목의 next 로 바꿔 쓰면
 * 목록을 훑는 쪽에서는 보이지 않게 된다. 흔한 연결 리스트 삭제다.
 *
 * capability 목록은 원래 읽기 전용이라, 쓰기 전에
 * dw_pcie_dbi_ro_wr_en() 으로 잠금을 풀고 끝나면 다시 잠근다.
 * 그 짝을 맞추지 않으면 이후 코드가 실수로 config 를 덮어쓸 수 있다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. 링크가 올라오기 전,
 *   호스트가 config 를 읽기 전에 끝나야 한다.
 *
 * 호출 체인:
 *   SoC 드라이버 초기화 → [이 함수]
 *     → dw_pcie_dbi_ro_wr_en() → dw_pcie_writeb_dbi() → _ro_wr_dis()
 */
void dw_pcie_remove_capability(struct dw_pcie *pci, u8 cap)			/* PCI/NVMe: RC의 표준 capability 제거 */
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
/* [한국어]
 * dw_pcie_remove_ext_capability - 확장 capability 하나를 목록에서 떼어 낸다
 *
 * @pci: 대상 컨트롤러.
 * @cap: 없앨 확장 capability ID.
 * @return: 없음.
 *
 * 위 함수의 확장판이지만 처리가 한 갈래 더 있다.
 *
 * 확장 capability 목록의 첫 항목은 위치가 0x100 으로 고정이라 그것을
 * 건너뛸 앞 항목이 없다. 그래서 그 경우에는 목록에서 빼는 대신
 * capability ID 만 0 으로 지운다 — 헤더의 상위 절반(next 포인터와
 * 버전)은 남겨 목록의 연결은 유지하고, ID 만 "없는 것" 으로 만든다.
 *
 * 그 밖의 경우는 표준 판과 같다. 앞 항목의 next 필드를 이 항목의
 * next 로 바꿔 건너뛰게 한다. 확장 capability 는 next 가 헤더의
 * 비트 31:20 에 있어 시프트 폭이 20 이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_hide_unsupported_l1ss() 등 → [이 함수]
 */
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
/* [한국어]
 * __dw_pcie_find_vsec_capability - 벤더 고유 확장 capability 를 찾는다
 *
 * @pci: 대상 컨트롤러.
 * @vendor_id: 이 IP 의 벤더 ID 와 일치해야 하는 값.
 * @vsec_id: 찾을 VSEC ID.
 * @return: 그 오프셋. 없으면 0.
 *
 * VSEC(Vendor-Specific Extended Capability)는 규격이 정한 것이 아니라
 * 각 벤더가 자기 용도로 정의하는 확장이다. 그래서 같은 capability ID
 * (PCI_EXT_CAP_ID_VNDR)를 쓰면서 내부의 VSEC ID 로 서로를 구분한다.
 *
 * 그러므로 찾으려면 두 단계가 필요하다.
 *   먼저 이 IP 의 벤더가 맞는지 확인한다. 다른 벤더의 장치에서
 *     같은 VSEC ID 가 전혀 다른 뜻일 수 있기 때문이다.
 *   그다음 VNDR capability 를 하나씩 훑으며 VSEC ID 를 비교한다.
 *
 * 이 IP 에서 이런 확장을 쓰는 예가 RAS DES(디버그·통계)와 PTM 이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_find_vsec_capability() → [이 함수]
 *     → PCI_FIND_NEXT_EXT_CAP() 반복
 */
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

/* [한국어]
 * dw_pcie_find_vsec_capability - 여러 벤더의 VSEC 후보를 차례로 찾아본다
 *
 * @pci: 대상 컨트롤러.
 * @vsec_ids: {벤더 ID, VSEC ID, 리비전} 항목들의 표. 벤더 ID 가 0 인
 *   항목이 끝 표시다.
 * @return: 처음으로 맞는 VSEC 의 오프셋. 없으면 0.
 *
 * 같은 기능이라도 벤더마다 다른 VSEC ID 를 쓴다. 예컨대 PTM 확장을
 * 어느 벤더는 ID 0x02 로, 다른 벤더는 0x04 로 정의한다. 그래서 표를
 * 두고 차례로 시도한다.
 *
 * 리비전까지 확인하는 이유는 같은 ID 라도 버전이 다르면 레지스터
 * 배치가 달라 그대로 쓸 수 없기 때문이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_find_rasdes_capability() / dw_pcie_find_ptm_capability()
 *     → [이 함수] → __dw_pcie_find_vsec_capability()
 */
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
/* [한국어]
 * dw_pcie_find_rasdes_capability - RAS DES 확장의 위치를 찾는다

 * @pci: 대상 컨트롤러.
 * @return: 그 오프셋. 없으면 0.
 *
 * RAS DES(Reliability, Availability, Serviceability — Debug, Error
 * injection, Statistics)는 DesignWare 가 제공하는 벤더 확장이다.
 * 링크 오류 통계, 이벤트 카운터, 오류 주입 같은 진단 기능이 여기 있고,
 * pcie-designware-debugfs.c 가 그것을 debugfs 로 노출한다.
 *
 * 벤더마다 VSEC ID 가 달라 표를 넘겨 찾는다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_setup() 또는 debugfs 초기화 → [이 함수]
 *     → dw_pcie_find_vsec_capability()
 */
u16 dw_pcie_find_rasdes_capability(struct dw_pcie *pci)
{
	return dw_pcie_find_vsec_capability(pci, dwc_pcie_rasdes_vsec_ids);	/* NVMe: RAS-DES VSEC ID 테이블 검색 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_rasdes_capability);

/* PCI/NVMe: PTM(Precision Time Measurement) VSEC 탐색, NVMe 타임스탬프 동기화 */
/* [한국어]
 * dw_pcie_find_ptm_capability - PTM 확장의 위치를 찾는다

 * @pci: 대상 컨트롤러.
 * @return: 그 오프셋. 없으면 0.
 *
 * PTM(Precision Time Measurement)은 장치와 호스트의 시각을 맞추는
 * PCIe 기능이다. 표준 확장 capability 로도 존재하지만, DesignWare 는
 * 그 제어 레지스터를 벤더 확장에 두어 이렇게 따로 찾는다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PTM 관련 초기화 → [이 함수] → dw_pcie_find_vsec_capability()
 */
u16 dw_pcie_find_ptm_capability(struct dw_pcie *pci)
{
	return dw_pcie_find_vsec_capability(pci, dwc_pcie_ptm_vsec_ids);		/* NVMe: PTM VSEC ID 테이블 검색 */
}
EXPORT_SYMBOL_GPL(dw_pcie_find_ptm_capability);

/* [한국어]
 * dw_pcie_read - 정렬을 확인하고 크기에 맞는 MMIO 읽기를 한다

 * @addr: 읽을 주소. 호출자가 이미 베이스를 더해 넘긴다.
 * @size: 1, 2, 4 중 하나.
 * @val: 결과를 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL, 또는 정렬·크기가 잘못되면
 *   PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * MMIO 는 정렬되지 않은 접근을 허용하지 않는다. 2바이트 읽기는 짝수
 * 주소, 4바이트는 4의 배수 주소여야 하며 어기면 버스 오류가 나거나
 * 조용히 엉뚱한 값이 읽힌다. 그래서 하드웨어를 건드리기 전에 검사한다.
 *
 * PCIBIOS_* 를 쓰는 것은 이 함수가 결국 PCI config 접근 경로에 쓰이기
 * 때문이다. 그쪽 규약이 음수 errno 가 아니라 이 상수들이다.
 *
 * 같은 IP 를 쓰는 mobiveil 이나 cadence 의 대응 함수와 달리, 여기서는
 * 32비트 접근만 되는 하드웨어를 위한 읽고-자르기 처리가 없다 —
 * DesignWare 의 DBI 는 임의 폭 접근을 받아들이기 때문이다.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 MMIO 라 잠들지 않는다.
 *
 * 호출 체인:
 *   dw_pcie_read_dbi() 등 → [이 함수] → readl/readw/readb
 */
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

/* [한국어]
 * dw_pcie_write - 정렬을 확인하고 크기에 맞는 MMIO 쓰기를 한다

 * @addr: 쓸 주소.
 * @size: 1, 2, 4 중 하나.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_BAD_REGISTER_NUMBER.
 *
 * 위 read 의 짝이다. 출력 인자가 없어 실패 시 채워 둘 것이 없다는
 * 점만 다르다.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 MMIO.
 *
 * 호출 체인:
 *   dw_pcie_write_dbi() 등 → [이 함수] → writel/writew/writeb
 */
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
/* [한국어]
 * dw_pcie_read_dbi - DBI(이 IP 의 config space)에서 읽는다

 * @pci: 대상 컨트롤러.
 * @reg: DBI 안의 오프셋.
 * @size: 읽을 크기(바이트).
 * @return: 읽은 값. 실패해도 값을 돌려주므로 호출자가 구분할 수 없다.
 *
 * DBI(Design Block Interface)는 이 IP 자신의 config space 다. 링크
 * 설정, capability 조작, BAR 구성이 전부 여기를 거친다.
 *
 * ops 콜백을 먼저 보는 것이 요점이다. SoC 에 따라 DBI 접근에 특별한
 * 처리가 필요한 경우가 있다 — 예컨대 접근 전에 어떤 레지스터를
 * 건드려야 하거나, 주소를 보정해야 하는 통합 방식이 있다. 그런
 * 드라이버는 자기 콜백을 채워 이 기본 경로를 대신한다.
 *
 * 실패를 반환값으로 알리지 않고 메시지만 남기는 점에 주의해야 한다.
 * 호출자는 정상적으로 읽은 0 과 실패를 구분할 수 없다. 다만 실패는
 * 호출자가 정렬을 어긴 경우뿐이라 정상 동작에서는 일어나지 않는다.
 *
 * 실행 컨텍스트: 초기화와 config 접근 경로.
 *
 * 호출 체인:
 *   dw_pcie_readl_dbi/readw_dbi/readb_dbi 매크로 → [이 함수]
 *     → ops->read_dbi 또는 dw_pcie_read()
 */
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
/* [한국어]
 * dw_pcie_write_dbi - DBI 에 쓴다

 * @pci: 대상 컨트롤러.
 * @reg: DBI 안의 오프셋.
 * @size: 쓸 크기(바이트).
 * @val: 쓸 값.
 * @return: 없음. 실패해도 알리지 않는다.
 *
 * 위 read 의 짝이다. SoC 별 콜백을 먼저 보는 구조도 같다.
 *
 * config space 의 읽기 전용 필드에 쓰려면 그 전에
 * dw_pcie_dbi_ro_wr_en() 으로 잠금을 풀어야 한다. 이 함수 자체는
 * 그것을 확인하지 않으므로 호출자의 책임이다.
 *
 * 실행 컨텍스트: 초기화와 config 접근 경로.
 *
 * 호출 체인:
 *   dw_pcie_writel_dbi 계열 매크로 → [이 함수]
 *     → ops->write_dbi 또는 dw_pcie_write()
 */
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
/* [한국어]
 * dw_pcie_write_dbi2 - 두 번째 DBI 창에 쓴다

 * @pci: 대상 컨트롤러.
 * @reg: DBI2 안의 오프셋.
 * @size: 쓸 크기(바이트).
 * @val: 쓸 값.
 * @return: 없음.
 *
 * DBI2 가 왜 따로 있는가. 엔드포인트 모드에서 BAR 를 설정할 때
 * 두 가지를 써야 하기 때문이다 — BAR 주소와 BAR 마스크(크기).
 * 그런데 config space 의 같은 오프셋에 둘 다 놓을 수 없어, IP 가
 * 같은 레지스터를 두 창으로 노출한다. DBI 로 쓰면 주소가, DBI2 로
 * 쓰면 마스크가 설정되는 식이다.
 *
 * 그래서 이 함수에는 읽기 짝이 없다. 마스크는 쓰기만 하면 되기
 * 때문이다.
 *
 * dw_pcie_get_resources() 에서 dbi2 자원이 없으면 dbi + 4KB 를
 * 기본값으로 쓰는데, 많은 IP 가 실제로 그 배치를 따르기 때문이다.
 *
 * 실행 컨텍스트: 엔드포인트 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_writel_dbi2 계열 매크로 → [이 함수]
 *     → ops->write_dbi2 또는 dw_pcie_write()
 */
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
/* [한국어]
 * dw_pcie_select_atu - iATU 창 하나의 레지스터 주소를 얻는다

 * @pci: 대상 컨트롤러.
 * @dir: 방향(아웃바운드 PCIE_ATU_REGION_DIR_OB 또는 인바운드 _IB).
 * @index: 몇 번째 창인지.
 * @return: 그 창의 레지스터가 시작하는 주소.
 *
 * 이 함수가 이 파일에서 가장 덜 자명한 부분이며, DesignWare IP 의
 * 세대 차이를 흡수한다.
 *
 * 구형 IP 는 창 하나만 들여다볼 수 있는 "뷰포트" 방식이다. 어느 창을
 * 볼지 VIEWPORT 레지스터에 먼저 쓰고, 그다음 늘 같은 주소에서 읽고
 * 쓴다. 창을 바꾸려면 뷰포트를 다시 써야 한다.
 *
 * 신형(unrolled)은 창마다 자기 레지스터 묶음이 주소 공간에 펼쳐져
 * 있다. 인덱스로 주소를 계산해 바로 접근하면 된다.
 *
 * 그 차이가 중요한 이유는 동시성이다. 뷰포트 방식은 "뷰포트 쓰기 +
 * 레지스터 접근" 두 단계가 원자적이지 않아, 두 스레드가 서로 다른
 * 창을 건드리면 어긋난다. unrolled 는 그 문제가 없다.
 * 이 파일에는 그것을 막는 잠금이 없으므로, 뷰포트 방식 IP 에서는
 * 상위 계층이 직렬화한다고 전제하는 셈이다 — 그 근거를 이 트리에서
 * 확인하지는 못했다.
 *
 * 실행 컨텍스트: iATU 설정 경로. 부수효과(뷰포트 쓰기)가 있어
 *   순수 계산이 아니다.
 *
 * 호출 체인:
 *   dw_pcie_readl_atu() / dw_pcie_writel_atu() → [이 함수]
 */
static inline void __iomem *dw_pcie_select_atu(struct dw_pcie *pci, u32 dir,
					       u32 index)
{
	if (dw_pcie_cap_is(pci, IATU_UNROLL))						/* NVMe: unrolled iATU를 사용하는 IP 버전 */
		return pci->atu_base + PCIE_ATU_UNROLL_BASE(dir, index);	/* NVMe: 인덱스 기반 직접 ATU 레지스터 주소 반환 */

	dw_pcie_writel_dbi(pci, PCIE_ATU_VIEWPORT, dir | index);			/* NVMe: viewport 레지스터에 방향/인덱스 기록, 구버전 ATU */
	return pci->atu_base;									/* NVMe: viewport 기반 ATU 기본 주소 반환 */
}

/* PCI/NVMe: ATU 레지스터 읽기, NVMe BAR/iATU 설정 검증 */
/* [한국어]
 * dw_pcie_readl_atu - iATU 레지스터를 읽는다

 * @pci: 대상 컨트롤러.
 * @dir: 방향.
 * @index: 창 번호.
 * @reg: 그 창 안의 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 창을 고르고(뷰포트 방식이면 그 쓰기까지) 32비트를 읽는다.
 * iATU 레지스터는 전부 32비트라 크기 인자가 없다.
 *
 * DBI 접근과 마찬가지로 SoC 별 콜백을 먼저 본다. 다만 read_dbi 콜백을
 * 재사용하는 점이 눈에 띈다 — ATU 전용 콜백이 따로 없고, 그 콜백에
 * 베이스 주소를 인자로 넘겨 구분하게 되어 있다.
 *
 * 실행 컨텍스트: iATU 설정 경로.
 *
 * 호출 체인:
 *   dw_pcie_readl_atu_ob() / _ib() → [이 함수] → dw_pcie_select_atu()
 */
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
/* [한국어]
 * dw_pcie_writel_atu - iATU 레지스터에 쓴다

 * @pci: 대상 컨트롤러.
 * @dir: 방향.
 * @index: 창 번호.
 * @reg: 그 창 안의 레지스터 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 위 read 의 짝이다. 역시 write_dbi 콜백을 재사용한다.
 *
 * 실행 컨텍스트: iATU 설정 경로.
 *
 * 호출 체인:
 *   dw_pcie_writel_atu_ob() / _ib() → [이 함수] → dw_pcie_select_atu()
 */
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

/* [한국어]
 * dw_pcie_readl_atu_ob - 아웃바운드 창의 레지스터를 읽는다

 * @pci: 대상 컨트롤러.
 * @index: 창 번호.
 * @reg: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 방향 인자를 고정한 편의 래퍼다. 호출부마다 방향 상수를 적지 않아도
 * 되고, 실수로 반대 방향을 건드리는 일을 막는다.
 *
 * 실행 컨텍스트: iATU 설정 경로.
 *
 * 호출 체인:
 *   dw_pcie_prog_outbound_atu(), dw_pcie_iatu_detect() → [이 함수]
 */
static inline u32 dw_pcie_readl_atu_ob(struct dw_pcie *pci, u32 index, u32 reg)	/* PCI/NVMe: outbound ATU 레지스터 읽기 */
{
	/* NVMe: CPU->PCIe 방향 ATU 읽기, NVMe BAR/CFG/MSG 전송용 */
	return dw_pcie_readl_atu(pci, PCIE_ATU_REGION_DIR_OB, index, reg);
}

/* [한국어]
 * dw_pcie_writel_atu_ob - 아웃바운드 창의 레지스터에 쓴다

 * @pci: 대상 컨트롤러.
 * @index: 창 번호.
 * @reg: 레지스터 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 위 read 의 짝인 편의 래퍼다. 방향 인자를 고정해 호출부가 실수로
 * 인바운드를 건드리는 일을 막는다.
 *
 * 실행 컨텍스트: iATU 설정 경로.
 *
 * 호출 체인:
 *   dw_pcie_prog_outbound_atu(), dw_pcie_disable_atu() → [이 함수]
 */
static inline void dw_pcie_writel_atu_ob(struct dw_pcie *pci, u32 index, u32 reg,	/* PCI/NVMe: outbound ATU 레지스터 쓰기 */
					 u32 val)
{
	/* NVMe: CPU->PCIe 방향 ATU 쓰기, NVMe 메모리/config/message 창 설정 */
	dw_pcie_writel_atu(pci, PCIE_ATU_REGION_DIR_OB, index, reg, val);
}

/* [한국어]
 * dw_pcie_enable_ecrc - 특정 IP 버전의 ECRC 결함을 우회한다

 * @val: 아웃바운드 창의 Control 1 레지스터에 쓸 값.
 * @return: TD 비트가 필요에 따라 켜진 값.
 *
 * ECRC(End-to-end CRC)는 TLP 끝에 검사값(Digest)을 붙여 경로 전체의
 * 무결성을 확인하는 기능이다. TD(TLP Digest) 비트가 그것이 붙어
 * 있음을 나타낸다.
 *
 * 아래 상류 주석이 밝히는 결함이 이 함수의 이유다 — 특정 DWC 버전에서
 * 아웃바운드 창의 TD 비트가 의도와 다르게 동작해, 소프트웨어가
 * 직접 보정해야 한다.
 *
 * 이런 함수가 존재한다는 것 자체가 IP 버전 확인(dw_pcie_version_detect)
 * 이 왜 필요한지를 보여 준다.
 *
 * 실행 컨텍스트: 아웃바운드 창 설정 중. 순수 계산.
 *
 * 호출 체인:
 *   dw_pcie_prog_outbound_atu() → [이 함수]
 */
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
/* [한국어]
 * dw_pcie_prog_outbound_atu - 아웃바운드 주소 변환 창을 설정한다

 * @pci: 대상 컨트롤러.
 * @atu: 설정할 내용. 창 번호, 이쪽 주소, 저쪽 PCIe 주소, 크기,
 *   TLP 종류와 라우팅 방식이 들어 있다.
 * @return: 0 이면 성공. 창 번호가 범위를 넘으면 -ENOSPC,
 *   주소·크기가 제약을 어기면 -EINVAL, 활성화를 기다리다 실패하면
 *   -ETIMEDOUT.
 *
 * 이 파일에서 가장 중요한 함수다. CPU 가 어떤 물리 주소에 접근하면
 * 그것을 PCIe TLP 로 바꿔 저쪽 주소로 내보내도록 하드웨어를 설정한다.
 * MMIO 접근, config 접근, 메시지 전송이 전부 이 창을 거친다.
 *
 * 창 하나에 여섯 레지스터를 쓴다 — 이쪽 주소의 시작과 끝(base, limit),
 * 저쪽 주소(target), 그리고 제어 둘(ctrl1, ctrl2).
 *
 * 세 가지 검증을 먼저 한다.
 *   창 번호가 하드웨어가 가진 개수 안인가.
 *   시작과 끝이 같은 region_limit 블록 안인가 — 창이 하드웨어가
 *     다룰 수 있는 경계를 넘어가면 안 된다.
 *   주소들이 region_align 에 맞는가, 그리고 크기가 0 이 아닌가.
 * 이 값들(region_limit, region_align)은 dw_pcie_iatu_detect() 가
 * 하드웨어를 떠보아 알아낸 것이다.
 *
 * 버전에 따른 분기가 둘 있다.
 *   v4.60a 이상만 상위 32비트 limit 레지스터를 갖는다. 그 미만은
 *     4GB 를 넘는 창을 만들 수 없다.
 *   v4.90a 와 v5.00a 는 ECRC 결함이 있어 위 dw_pcie_enable_ecrc() 로
 *     보정한다.
 *
 * 마지막에 활성화 비트를 되읽으며 기다리는 것이 중요하다. 쓰기가
 * 반영되기까지 시간이 걸리는데, 그 전에 그 주소로 접근하면 변환이
 * 아직 걸리지 않아 엉뚱한 곳으로 나간다.
 *
 * 실행 컨텍스트: 초기화 중, 또는 config 접근 경로에서 창을 바꿔 쓸 때.
 *   mdelay 로 바쁜 대기를 하므로 인터럽트를 끈 채 부르면 그만큼 멈춘다.
 *
 * 호출 체인:
 *   dw_pcie_host_init() 계열, dw_pcie_other_conf_map_bus() 등
 *     → [이 함수] → dw_pcie_writel_atu_ob()
 */
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

/* [한국어]
 * dw_pcie_readl_atu_ib - 인바운드 창의 레지스터를 읽는다

 * @pci: 대상 컨트롤러.
 * @index: 창 번호.
 * @reg: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 아웃바운드 판의 인바운드 짝이다. 인바운드는 저쪽에서 들어온 요청을
 * 이쪽 주소로 옮기는 방향이며, 엔드포인트 모드에서 BAR 뒤의 메모리를
 * 연결하는 데 쓴다.
 *
 * 실행 컨텍스트: iATU 설정 경로.
 *
 * 호출 체인:
 *   dw_pcie_iatu_detect() 등 → [이 함수] → dw_pcie_readl_atu()
 */
static inline u32 dw_pcie_readl_atu_ib(struct dw_pcie *pci, u32 index, u32 reg)	/* PCI/NVMe: inbound ATU 레지스터 읽기 */
{
	return dw_pcie_readl_atu(pci, PCIE_ATU_REGION_DIR_IB, index, reg);					/* NVMe: PCIe->CPU 방향 ATU 읽기, NVMe EP BAR 수신용 */
}

/* [한국어]
 * dw_pcie_writel_atu_ib - 인바운드 창의 레지스터에 쓴다

 * @pci: 대상 컨트롤러.
 * @index: 창 번호.
 * @reg: 레지스터 오프셋.
 * @val: 쓸 값.
 * @return: 없음.
 *
 * 위 read 의 짝이다.
 *
 * 실행 컨텍스트: iATU 설정 경로.
 *
 * 호출 체인:
 *   dw_pcie_prog_inbound_atu(), dw_pcie_prog_ep_inbound_atu(),
 *   dw_pcie_disable_atu() → [이 함수] → dw_pcie_writel_atu()
 */
static inline void dw_pcie_writel_atu_ib(struct dw_pcie *pci, u32 index, u32 reg,		/* PCI/NVMe: inbound ATU 레지스터 쓰기 */
					 u32 val)
{
	/* NVMe: PCIe->CPU 방향 ATU 쓰기, NVMe SSD EP BAR 매핑 */
	dw_pcie_writel_atu(pci, PCIE_ATU_REGION_DIR_IB, index, reg, val);
}

/* PCI/NVMe: inbound iATU 프로그램, NVMe EP 모드에서 host가 NVMe BAR 접근 */
/* [한국어]
 * dw_pcie_prog_inbound_atu - 인바운드 주소 변환 창을 설정한다 (주소 기준)

 * @pci: 대상 컨트롤러.
 * @index: 창 번호.
 * @type: TLP 종류.
 * @parent_bus_addr: 이쪽 주소. 들어온 요청이 닿을 곳이다.
 * @pci_addr: 저쪽 PCIe 주소. 이 범위로 들어오는 요청이 대상이다.
 * @size: 창 크기.
 * @return: 0 이면 성공. -ENOSPC, -EINVAL, -ETIMEDOUT 중 하나로 실패.
 *
 * 아웃바운드의 반대 방향이다. 저쪽에서 들어온 요청을 이쪽 메모리로
 * 옮긴다.
 *
 * 두 함수의 base/target 이 반대로 쓰인다는 점이 헷갈리기 쉽다.
 *   아웃바운드: base = 이쪽 주소(감시할 범위), target = 저쪽 주소
 *   인바운드:   base = 저쪽 주소(감시할 범위), target = 이쪽 주소
 * 어느 쪽이든 base 는 "들어오는 것을 알아볼 범위" 이고 target 은
 * "옮겨 갈 곳" 이다. 방향이 반대라 그 역할이 바뀌는 것이다.
 *
 * 검증과 활성화 대기는 아웃바운드와 같은 구조다. 다만 아웃바운드에
 * 있던 ECRC 보정과 메시지 TLP 처리는 없다 — 들어오는 요청에는
 * 해당하지 않기 때문이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. mdelay 로 바쁜 대기를 한다.
 *
 * 호출 체인:
 *   호스트/EP 초기화 → [이 함수] → dw_pcie_writel_atu_ib()
 */
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
/* [한국어]
 * dw_pcie_prog_ep_inbound_atu - 인바운드 창을 BAR 기준으로 설정한다

 * @pci: 대상 컨트롤러.
 * @func_no: 어느 엔드포인트 함수의 BAR 인지.
 * @index: 창 번호.
 * @type: TLP 종류.
 * @parent_bus_addr: 그 BAR 뒤에 놓일 이쪽 메모리 주소.
 * @bar: 몇 번 BAR 인지.
 * @size: 크기. 정렬 검사에만 쓰인다.
 * @return: 0 이면 성공, -EINVAL 또는 -ETIMEDOUT.
 *
 * 위 함수와 목적은 같지만 대상을 알아보는 방식이 다르다.
 *
 * 주소 기준(위 함수)은 "이 주소 범위로 들어온 요청" 을 잡는다.
 * BAR 기준(이 함수)은 "이 함수의 이 BAR 로 들어온 요청" 을 잡는다.
 * BAR_MODE_ENABLE 비트가 그 전환이며, 그러면 base/limit 레지스터를
 * 쓰지 않는다 — 실제로 이 함수는 target 만 쓰고 base 를 건드리지 않는다.
 *
 * 왜 그래야 하는가. 엔드포인트의 BAR 주소는 호스트가 열거하며 정해
 * 주므로 소프트웨어가 미리 알 수 없다. 주소로 잡으려면 그 값을 알아야
 * 하는데, BAR 번호로 잡으면 하드웨어가 알아서 맞춰 준다.
 *
 * FUNC_NUM_MATCH_EN 을 함께 켜는 것은 다중 함수 장치에서 어느 함수의
 * BAR 인지까지 구분하기 위해서다.
 *
 * 실행 컨텍스트: 엔드포인트 BAR 설정 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_ep_set_bar() [pcie-designware-ep.c] → [이 함수]
 */
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
/* [한국어]
 * dw_pcie_disable_atu - 주소 변환 창 하나를 끈다

 * @pci: 대상 컨트롤러.
 * @dir: 방향(아웃바운드/인바운드).
 * @index: 끌 창 번호.
 * @return: 없음.
 *
 * Control 2 레지스터를 0 으로 만들어 활성화 비트를 지운다. 그러면
 * 그 창은 아무 변환도 하지 않는다.
 *
 * 나머지 레지스터(주소, 타입)는 그대로 두는데, 활성화 비트만 꺼도
 * 동작이 멈추기 때문이다. 창을 재사용할 때는 설정 함수가 전부 다시
 * 쓰므로 남은 값이 문제되지 않는다.
 *
 * 실행 컨텍스트: 정리 경로, 또는 창을 재사용하기 전.
 *
 * 호출 체인:
 *   호스트/EP 정리 경로 → [이 함수]
 *     → dw_pcie_writel_atu_ob() 또는 _ib()
 */
void dw_pcie_disable_atu(struct dw_pcie *pci, u32 dir, int index)
{
	dw_pcie_writel_atu(pci, dir, index, PCIE_ATU_REGION_CTRL2, 0);						/* NVMe: ATU ENABLE 비트 클리어, NVMe 트래픽 중단 */
}

/* [한국어]
 * dw_pcie_ltssm_status_string - LTSSM 상태 값을 사람이 읽을 문자열로 바꾼다

 * @ltssm: LTSSM 상태 값.
 * @return: 그 상태의 이름.
 *
 * 링크가 올라오지 않았을 때 어느 단계에서 멈췄는지가 진단의 핵심이다.
 * 숫자만 찍으면 규격 문서를 찾아봐야 하므로 이름으로 바꿔 준다.
 *
 * LTSSM(Link Training and Status State Machine)의 각 상태가 뜻하는
 * 바를 알면 원인을 좁힐 수 있다 — Detect 에서 멈춰 있으면 상대가
 * 아예 없거나 전기적으로 감지되지 않는 것이고, Polling 까지 갔다면
 * 상대는 있는데 협상이 안 되는 것이다.
 *
 * 실행 컨텍스트: 오류 로그 경로. 순수 변환.
 *
 * 호출 체인:
 *   dw_pcie_wait_for_link() → [이 함수]
 */
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
/* [한국어]
 * dw_pcie_wait_for_link - 링크가 올라오기를 기다리고 결과를 보고한다

 * @pci: 대상 컨트롤러.
 * @return: 0 이면 링크가 올라왔다. 실패는 원인에 따라 셋으로 나뉜다 —
 *   -ENODEV(장치가 없음), -EIO(있는데 활성화 안 됨), -ETIMEDOUT(그 밖).
 *
 * 단순히 기다리기만 하는 것이 아니라, 실패했을 때 LTSSM 상태를 읽어
 * 원인을 구분해 주는 것이 이 함수의 값어치다.
 *   Detect.Quiet / Detect.Active — 상대가 감지되지 않는다. 슬롯이
 *     비어 있는 정상적인 경우이므로 -ENODEV 로 알리고 dev_info 로만 남긴다.
 *   Polling.Active / Polling.Compliance — 상대는 있는데 협상이
 *     끝나지 않았다. 다시 시도하면 될 수도 있어 -EIO 로 구분한다.
 *   그 밖 — 진짜 문제이므로 dev_err 과 함께 상태 이름을 찍는다.
 * 호출자가 이 구분을 보고 재시도할지 포기할지 정할 수 있다.
 *
 * 링크가 올라온 뒤 Gen3 이상이면 100ms 를 더 기다리는 부분도 중요하다.
 * PCIe 규격이 링크가 올라온 뒤 첫 config 요청까지 그만큼 두라고
 * 정하고 있으며, 지키지 않으면 장치가 아직 준비되지 않아 응답하지
 * 못한다. PCIE_RESET_CONFIG_WAIT_MS 가 그 값이다.
 *
 * 마지막에 협상된 속도와 폭을 dev_info 로 남긴다. NVMe 성능이
 * 기대에 못 미칠 때 가장 먼저 보게 되는 줄이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:
 *   호스트/EP 초기화 → [이 함수]
 *     → dw_pcie_link_up() → dw_pcie_get_ltssm() → dw_pcie_readw_dbi()
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

/* [한국어]
 * dw_pcie_link_up - 링크가 올라와 있는지 확인한다

 * @pci: 대상 컨트롤러.  @return: 링크가 살아 있으면 true.
 *
 * SoC 별 콜백이 있으면 그것을 우선하고, 없으면 IP 의 표준 방식 —
 * 링크 상태 레지스터의 UP 비트와 트레이닝 중 비트를 함께 보는 것 — 을 쓴다.
 *
 * 트레이닝 중 비트까지 확인하는 이유가 중요하다. 협상이 진행 중일 때도
 * UP 비트가 잠깐 설 수 있는데, 그 순간을 "올라왔다" 로 보면 아직 속도와
 * 폭이 확정되지 않은 상태에서 config 접근을 시작하게 된다.
 *
 * 실행 컨텍스트: 링크 대기 루프와 핫플러그 처리. 순수 조회.
 *
 * 호출 체인:  dw_pcie_wait_for_link(), 호스트 초기화 → [이 함수]
 *     → ops->link_up 또는 dw_pcie_readl_dbi()
 */
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

/* [한국어]
 * dw_pcie_upconfig_setup - 레인 수를 동적으로 늘릴 수 있게 한다

 * @pci: 대상 컨트롤러.  @return: 없음.
 *
 * Upconfigure 는 링크가 올라온 뒤 레인 수를 늘리는 PCIe 기능이다.
 * 처음에 적은 레인으로 협상됐더라도 나중에 더 넓힐 수 있게 한다.
 *
 * 왜 처음부터 넓게 협상하지 않는가 — 신호 품질이 나쁘거나 상대가
 * 준비되지 않아 일부 레인이 빠진 채 링크가 서는 경우가 있기 때문이다.
 * 이 비트를 켜 두면 조건이 좋아졌을 때 다시 넓힐 여지가 생긴다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:  SoC 드라이버 초기화 → [이 함수] → dw_pcie_writel_dbi()
 */
void dw_pcie_upconfig_setup(struct dw_pcie *pci)								/* PCI/NVMe: lane upconfig 지원 설정, NVMe 대역폭 동적 확장 */
{
	u32 val;																/* NVMe: multi lane control 레지스터 값 */

	val = dw_pcie_readl_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL);						/* NVMe: Multi Lane Control 레지스터 읽기 */
	val |= PORT_MLTI_UPCFG_SUPPORT;												/* NVMe: upconfig 지원 비트 설정, lane 수 런타임 증가 가능 */
	dw_pcie_writel_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, val);						/* NVMe: upconfig 설정 기록, NVMe 성능 동적 조정 */
}
EXPORT_SYMBOL_GPL(dw_pcie_upconfig_setup);

/* [한국어]
 * dw_pcie_link_set_max_speed - 협상할 최대 링크 속도를 제한한다

 * @pci: 대상 컨트롤러. max_link_speed 를 본다.  @return: 없음.
 *
 * 디바이스 트리의 max-link-speed 로 지정한 상한을 하드웨어에 반영한다.
 * 지정되지 않았으면 IP 가 지원하는 최대치를 그대로 쓴다.
 *
 * 왜 일부러 낮추는가. 보드 배선 품질이 나쁘면 높은 속도로 트레이닝을
 * 시도하다 실패해 링크가 오르내리기를 반복할 수 있다. 그럴 때는 아예
 * 낮은 속도로 고정하는 편이 안정적이다.
 *
 * NVMe 성능을 진단할 때 확인할 지점이다 — 드라이브가 Gen4 를 지원해도
 * 디바이스 트리가 Gen2 로 묶어 두었으면 그것이 상한이 된다.
 *
 * 실행 컨텍스트: 초기화 중, 링크 트레이닝 전 프로세스 컨텍스트.
 *
 * 호출 체인:  dw_pcie_setup() → [이 함수] → dw_pcie_writel_dbi()
 */
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

/* [한국어]
 * dw_pcie_link_get_max_link_width - 하드웨어가 지원하는 최대 레인 수를 읽는다

 * @pci: 대상 컨트롤러.  @return: 최대 레인 수.
 *
 * Link Capabilities 레지스터의 Maximum Link Width 필드를 읽는다.
 * 이 IP 가 합성될 때 정해진 값이라 소프트웨어가 바꿀 수 없다.
 *
 * 아래 set_max_link_width 가 이 값을 상한으로 삼는다 — 하드웨어에
 * 없는 레인을 쓰라고 할 수는 없기 때문이다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. 순수 조회.
 *
 * 호출 체인:  SoC 드라이버 → [이 함수] → dw_pcie_readl_dbi()
 */
int dw_pcie_link_get_max_link_width(struct dw_pcie *pci)							/* PCI/NVMe: PCIe 링크 최대 폭 조회, NVMe x4/x8/x16 가능 폭 */
{
	u8 cap = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);							/* NVMe: PCIe Express capability 오프셋 */
	u32 lnkcap = dw_pcie_readl_dbi(pci, cap + PCI_EXP_LNKCAP);						/* NVMe: Link Capability 레지스터 읽기 */

	return FIELD_GET(PCI_EXP_LNKCAP_MLW, lnkcap);									/* NVMe: Maximum Link Width 반환, NVMe lane 수 */
}

/* PCI/NVMe: PCIe 링크 폭 설정, NVMe 사용 가능 lane 수 제한 */
/* [한국어]
 * dw_pcie_link_set_max_link_width - 쓸 레인 수를 설정한다

 * @pci: 대상 컨트롤러.
 * @num_lanes: 쓸 레인 수. 0 이면 아무것도 하지 않는다.
 *
 * 디바이스 트리의 num-lanes 를 하드웨어에 반영한다. 두 곳을 고쳐야 한다 —
 * Link Capabilities 의 광고 값과 Port Link Control 의 실제 설정이다.
 *
 * 보드가 실제로 배선한 레인 수와 맞춰야 한다는 점이 중요하다. IP 는
 * x8 을 지원하는데 보드가 x4 만 배선했다면, x8 로 광고하면 상대가
 * 없는 레인으로 트레이닝을 시도하다 실패하거나 시간을 낭비한다.
 *
 * NVMe 대역폭의 상한이 여기서 정해진다 — Gen4 x4 와 Gen4 x2 는
 * 두 배 차이가 난다.
 *
 * 실행 컨텍스트: 초기화 중, 링크 트레이닝 전 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_setup() → [이 함수] → dw_pcie_writel_dbi()
 */
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

/* [한국어]
 * dw_pcie_iatu_detect - iATU 의 창 개수와 주소 제약을 하드웨어에서 알아낸다

 * @pci: 대상 컨트롤러. 결과를 num_ob_windows, num_ib_windows,
 *   region_align, region_limit 에 기록한다.  @return: 없음.
 *
 * DesignWare IP 는 합성 시점에 창 개수와 주소 정렬 제약이 정해지고,
 * 그것을 알려 주는 레지스터가 따로 없다. 그래서 하드웨어를 직접 떠본다.
 *
 * 방법이 재미있다. 창의 limit 레지스터에 전부 1 을 써 보고 되읽는다.
 * 하드웨어가 구현하지 않은 하위 비트는 0 으로 남으므로, 남은 0 의
 * 개수가 곧 정렬 단위(region_align)를 말해 준다. 상위 쪽도 마찬가지로
 * 표현 가능한 최대 주소(region_limit)를 알려 준다.
 * 호스트가 BAR 크기를 알아내는 방법과 같은 발상이다.
 *
 * 창 개수도 같은 식으로 센다. 있는 창은 쓴 값이 남고 없는 창은
 * 0 이 되므로, 하나씩 시도하며 실패하는 지점을 찾는다.
 *
 * 이 값들이 있어야 dw_pcie_prog_outbound_atu() 의 검증이 성립한다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트. 실제로 레지스터를
 *   썼다 지우므로 창이 아직 쓰이기 전에 불려야 한다.
 *
 * 호출 체인:  dw_pcie_setup() 또는 호스트/EP 초기화 → [이 함수]
 *     → dw_pcie_writel_atu_ob/_ib(), dw_pcie_readl_atu_ob/_ib()
 */
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

/* [한국어]
 * dw_pcie_readl_dma - eDMA 레지스터를 읽는다

 * @pci: 대상 컨트롤러.  @reg: eDMA 영역 안의 오프셋.  @return: 읽은 값.
 *
 * DBI 접근과 같은 구조이되 베이스가 edma.reg_base 다. SoC 별 콜백을
 * 먼저 보는 것도 같으며, 여기서도 read_dbi 콜백을 재사용한다.
 *
 * 실행 컨텍스트: eDMA 탐지·초기화 경로.
 *
 * 호출 체인:  dw_pcie_edma_find_mf(), _find_channels() → [이 함수]
 */
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
/* [한국어]
 * dw_pcie_edma_irq_vector - eDMA 채널 번호를 IRQ 번호로 바꾼다

 * @dev: 이 컨트롤러의 device.
 * @nr: 채널 번호(쓰기 채널 다음에 읽기 채널이 이어진다).
 * @return: 그 채널의 IRQ 번호. 없으면 음수.
 *
 * dmaengine 프레임워크가 채널마다 인터럽트를 요청할 때 부르는 콜백이다.
 * 디바이스 트리에 "dma0", "dma1" 같은 이름으로 등록된 IRQ 를 찾아 준다.
 *
 * 이름을 문자열로 만들어 찾는 방식이라 버퍼 크기가 6바이트로 빠듯하다 —
 * "dma" + 두 자리 숫자 + 종료 문자다.
 *
 * 실행 컨텍스트: eDMA 등록 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_edma_probe() [dmaengine 쪽] → [이 함수]
 *     → platform_get_irq_byname_optional()
 */
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

/* [한국어]
 * dw_pcie_edma_init_data - eDMA 칩 정보의 기본값을 채운다

 * @pci: 대상 컨트롤러.  @return: 없음.
 *
 * dmaengine 에 등록할 struct dw_edma_chip 의 공통 필드를 채운다 —
 * 이름, device 포인터, IRQ 조회 콜백 같은 것들이다.
 * 채널 수와 레지스터 배치는 아래 find_ 함수들이 하드웨어를 보고 채운다.
 *
 * 실행 컨텍스트: eDMA 탐지 중 프로세스 컨텍스트.
 *
 * 호출 체인:  dw_pcie_edma_detect() → [이 함수]
 */
static void dw_pcie_edma_init_data(struct dw_pcie *pci)								/* PCI/NVMe: eDMA 초기 데이터 설정, NVMe DMA 엔진 준비 */
{
	pci->edma.dev = pci->dev;														/* NVMe: eDMA 장치를 PCIe RC 장치로 설정, DMA coherence/IOMMU 연결 */

	if (!pci->edma.ops)																/* NVMe: glue driver가 ops를 미리 설정하지 않은 경우 */
		pci->edma.ops = &dw_pcie_edma_ops;											/* NVMe: 기본 eDMA ops 사용, NVMe DMA IRQ 연결 */

	pci->edma.flags |= DW_EDMA_CHIP_LOCAL;											/* NVMe: 로컬 eDMA 엔진임을 표시, NVMe offload 가능 */
}

/* [한국어]
 * dw_pcie_edma_find_mf - eDMA 레지스터의 매핑 형식을 알아낸다

 * @pci: 대상 컨트롤러.  @return: 0 이면 성공. eDMA 가 없으면 -ENODEV.
 *
 * MF 는 Mapping Format 이다. eDMA 레지스터의 배치가 IP 버전에 따라 다르다.
 *   LEGACY — 뷰포트 방식. 채널을 골라 가며 같은 주소를 쓴다.
 *   UNROLL — 채널마다 자기 레지스터가 펼쳐져 있다.
 * iATU 의 뷰포트/unrolled 구분과 같은 발상이다.
 *
 * 알아내는 방법도 떠보기다. UNROLL 배치에서만 의미 있는 자리를 읽어
 * 기대한 값이 나오는지 본다.
 *
 * 실행 컨텍스트: eDMA 탐지 중 프로세스 컨텍스트.
 *
 * 호출 체인:  dw_pcie_edma_find_chip() → [이 함수] → dw_pcie_readl_dma()
 */
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
/* [한국어]
 * dw_pcie_edma_find_channels - 읽기·쓰기 채널 개수를 알아낸다

 * @pci: 대상 컨트롤러.
 * @return: 0 이면 성공. 채널이 없거나 상한을 넘으면 -EINVAL.
 *
 * eDMA 는 쓰기 채널과 읽기 채널을 따로 갖는다. 그 개수를 컨트롤
 * 레지스터에서 읽는다.
 *
 * 상류 주석이 밝히듯 HDMA 플랫폼은 자동 탐지가 안 된다. 그런 경우는
 * SoC 드라이버가 미리 채워 두어야 하고, 이 함수는 그 값을 존중한다.
 *
 * 실행 컨텍스트: eDMA 탐지 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_edma_find_chip() → [이 함수] → dw_pcie_readl_dma()
 */
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

/* [한국어]
 * dw_pcie_edma_find_chip - eDMA 하드웨어를 찾아 정보를 채운다

 * @pci: 대상 컨트롤러.  @return: 0 이면 있음, 없으면 -ENODEV.
 *
 * 위 두 find_ 함수를 순서대로 부른다. 매핑 형식을 먼저 알아야
 * 채널 개수를 읽을 레지스터 위치를 알 수 있기 때문이다.
 *
 * 실행 컨텍스트: eDMA 탐지 중 프로세스 컨텍스트.
 *
 * 호출 체인:  dw_pcie_edma_detect() → [이 함수]
 *     → dw_pcie_edma_find_mf() → dw_pcie_edma_find_channels()
 */
static int dw_pcie_edma_find_chip(struct dw_pcie *pci)									/* PCI/NVMe: eDMA 엔진 탐색 및 초기화, NVMe DMA offload 준비 */
{
	int ret;																	/* NVMe: 반환값 */

	dw_pcie_edma_init_data(pci);														/* NVMe: eDMA 기본 데이터 초기화 */

	ret = dw_pcie_edma_find_mf(pci);													/* NVMe: eDMA mapping format 탐색 */
	if (ret)																	/* NVMe: mapping format 탐색 실패 */
		return ret;																/* NVMe: eDMA 없음 또는 설정 오류 */

	return dw_pcie_edma_find_channels(pci);												/* NVMe: 채널 수 탐색 결과 반환 */
}

/* [한국어]
 * dw_pcie_edma_irq_verify - eDMA IRQ 구성이 채널 수와 맞는지 확인한다

 * @pci: 대상 컨트롤러.  @return: 0 이면 맞다. 아니면 -EINVAL.
 *
 * eDMA 인터럽트는 세 가지 구성이 가능하다 — 채널마다 하나, 읽기와
 * 쓰기에 각각 하나, 또는 전부 하나로 묶기. 어느 구성인지는 디바이스
 * 트리의 IRQ 이름으로 드러난다.
 *
 * 채널 수보다 IRQ 가 적거나 이름이 맞지 않으면 등록 후에 인터럽트를
 * 받지 못하게 되므로, 등록 전에 확인해 미리 걸러 낸다.
 *
 * 실행 컨텍스트: eDMA 탐지 중 프로세스 컨텍스트.
 *
 * 호출 체인:  dw_pcie_edma_detect() → [이 함수]
 */
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
/* [한국어]
 * dw_pcie_edma_ll_alloc - 채널마다 링크드 리스트 메모리를 잡는다

 * @pci: 대상 컨트롤러.
 * @return: 0 이면 성공. -ENOMEM.
 *
 * eDMA 는 전송 목록을 메모리에 링크드 리스트로 두고 하드웨어가 그것을
 * 따라가며 처리한다. 그 목록이 놓일 메모리를 채널마다 잡는 함수다.
 *
 * dmam_alloc_coherent 를 쓰는 이유가 중요하다. 이 메모리는 하드웨어가
 * 직접 읽으므로 CPU 캐시에 남은 값과 어긋나면 안 된다. dmam 접두어는
 * devres 판이라 드라이버가 떨어질 때 자동 해제된다.
 *
 * 실행 컨텍스트: eDMA 탐지 중 프로세스 컨텍스트. 메모리 할당으로 잠든다.
 *
 * 에러 경로: 중간에 실패해도 앞서 잡은 것은 devres 가 정리한다.
 *
 * 호출 체인:
 *   dw_pcie_edma_detect() → [이 함수] → dmam_alloc_coherent()
 */
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

/* [한국어]
 * dw_pcie_edma_detect - eDMA 를 찾아 dmaengine 에 등록한다

 * @pci: 대상 컨트롤러.  @return: 0 이면 성공(eDMA 가 없어도 성공이다).
 *
 * eDMA 는 DesignWare IP 가 선택적으로 내장하는 DMA 엔진이다. 있으면
 * 커널 dmaengine 프레임워크에 등록해, 다른 드라이버가 일반적인
 * dmaengine API 로 쓸 수 있게 한다.
 *
 * 순서가 정해져 있다 — 기본값 채우기 → 하드웨어 찾기 → IRQ 검증 →
 * 링크드 리스트 메모리 확보 → 등록.
 *
 * eDMA 가 없는 것이 오류가 아니라는 점이 중요하다. 많은 SoC 가 그것을
 * 합성하지 않으며, 그때는 조용히 물러나 나머지 초기화가 계속된다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:  호스트/EP 초기화 → [이 함수]
 *     → dw_pcie_edma_find_chip() → dw_pcie_edma_irq_verify()
 *     → dw_pcie_edma_ll_alloc() → dw_edma_probe()
 */
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

/* [한국어]
 * dw_pcie_edma_remove - eDMA 등록을 해제한다

 * @pci: 대상 컨트롤러.  @return: 없음.
 *
 * detect 의 반대다. 링크드 리스트 메모리는 devres 라 여기서 해제하지 않는다.
 *
 * 실행 컨텍스트: 정리 경로 프로세스 컨텍스트.
 *
 * 호출 체인:  호스트/EP 정리 → [이 함수] → dw_edma_remove()
 */
void dw_pcie_edma_remove(struct dw_pcie *pci)											/* PCI/NVMe: eDMA 드라이버 제거, NVMe DMA offload 해제 */
{
	dw_edma_remove(&pci->edma);															/* NVMe: eDMA 드라이버 등록 해제, NVMe DMA 리소스 정리 */
}

/* PCI/NVMe: 지원하지 않는 L1 PM Substates 숨김, NVMe ASPM 호환성 */
/* [한국어]
 * dw_pcie_hide_unsupported_l1ss - 동작하지 않는 L1 서브스테이트를 감춘다

 * @pci: 대상 컨트롤러.
 * @return: 없음.
 *
 * L1SS(L1 Substates)는 L1 절전 상태를 더 깊게 나눈 것으로, L1.1 과
 * L1.2 가 있다. 이것이 동작하려면 CLKREQ# 신호가 배선되어 있어야
 * 하는데, 보드가 그것을 연결하지 않은 경우가 있다.
 *
 * 그런데 IP 는 config space 에 L1SS capability 를 그대로 노출한다.
 * 그러면 호스트 쪽 소프트웨어가 그것을 믿고 켜려다 링크가 깨진다.
 *
 * 그래서 플랫폼이 명시적으로 지원한다고 하지 않으면 그 capability 를
 * 목록에서 떼어 내 아예 보이지 않게 한다. 앞서 본
 * dw_pcie_remove_ext_capability() 를 쓰는 실제 사례다.
 *
 * 실행 컨텍스트: 초기화 중, 호스트가 config 를 읽기 전.
 *
 * 호출 체인:
 *   dw_pcie_setup() 또는 SoC 초기화 → [이 함수]
 *     → dw_pcie_remove_ext_capability()
 */
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

/* [한국어]
 * dw_pcie_setup - 링크 트레이닝 전의 공통 설정을 한다

 * @pci: 대상 컨트롤러.  @return: 없음.
 *
 * 호스트든 엔드포인트든 공통으로 필요한 설정을 모아 놓은 함수다.
 * 최대 속도와 레인 수를 정하고, CDM 검사를 켜고, 그 밖의 기본 동작을
 * 구성한다.
 *
 * 이 함수가 링크 트레이닝 전에 불려야 하는 이유는 여기서 정하는
 * 값들이 협상에 쓰이기 때문이다. 링크가 올라온 뒤에 바꿔 봐야
 * 이미 협상이 끝난 뒤다.
 *
 * 실행 컨텍스트: 초기화 중, 링크 트레이닝 전 프로세스 컨텍스트.
 *
 * 호출 체인:  dw_pcie_host_init() / dw_pcie_ep_init() → [이 함수]
 *     → dw_pcie_link_set_max_speed() → dw_pcie_link_set_max_link_width()
 */
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
/* [한국어]
 * dw_pcie_parent_bus_offset - CPU 물리 주소와 상위 버스 주소의 차이를 구한다

 * @pci: 대상 컨트롤러.
 * @reg_name: 기준으로 삼을 자원 이름("dbi" 등).
 * @cpu_phys_addr: 그 자원의 CPU 물리 주소.
 * @return: 두 주소의 차이. 같으면 0.
 *
 * SoC 에 따라 CPU 가 보는 물리 주소와 이 IP 가 매달린 버스에서 보는
 * 주소가 다를 수 있다. 중간에 다른 버스 브리지가 있어 주소가 옮겨지는
 * 구성이 그렇다.
 *
 * iATU 창을 설정할 때는 IP 가 보는 주소를 써야 하므로, 그 차이를
 * 미리 구해 두었다가 빼 준다. 디바이스 트리의 ranges 를 따라가
 * 실제 변환을 알아내는 방식이다.
 *
 * cadence 의 cpu_addr_fixup 콜백과 목적이 같지만 접근이 다르다 —
 * 그쪽은 SoC 드라이버가 계산식을 제공하고, 이쪽은 디바이스 트리에서
 * 알아낸다.
 *
 * 실행 컨텍스트: 초기화 중 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   호스트/EP 초기화 → [이 함수] → of_ 계열 주소 변환
 */
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
