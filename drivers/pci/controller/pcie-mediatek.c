// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek PCIe host controller driver.
 *
 * Copyright (c) 2017 MediaTek Inc.
 * Author: Ryder Lee <ryder.lee@mediatek.com>
 *	   Honghui Zhang <honghui.zhang@mediatek.com>
 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */

/*
 * [한국어 설명] 미디어텍 SoC 의 PCIe 호스트 (pcie-mediatek.c)
 *
 * === 파일의 역할 ===
 * 미디어텍 SoC(MT7622, MT2712 등)의 PCIe 컨트롤러를 다룬다. DesignWare 가
 * 아니라 미디어텍 자체 IP 라서 dwc/ 가 아닌 이 자리에 있고, 링크 관리부터
 * config 접근, 인터럽트까지 전부 이 파일이 직접 구현한다. 그래서 파일이
 * 1300줄 가까이 된다.
 *
 * 이 컨트롤러의 구조에서 눈여겨볼 점은 포트마다 독립적이라는 것이다.
 * 하나의 컨트롤러가 여러 루트 포트를 갖는데, 각 포트가 자기 레지스터
 * 묶음과 자기 PHY, 자기 클럭, 자기 리셋을 갖는다. 그래서 struct mtk_pcie 가
 * struct mtk_pcie_port 의 목록을 들고 있고, 초기화가 포트 단위로 반복된다.
 *
 * 포트 하나가 실패해도 나머지는 살려야 하기 때문에, 초기화 실패 시 그
 * 포트만 목록에서 빼고 계속 진행한다. 하드웨어가 여럿인 드라이버에서
 * 흔히 보는 방어적 구조다.
 *
 * 세대가 둘 있다는 점도 알아야 한다. 구형(MT2701 등)과 신형(MT7622 등)이
 * 레지스터 배치와 링크 관리 방식이 달라서, soc_data 로 콜백을 갈아 끼운다.
 * 같은 파일 안에 두 하드웨어 세대가 공존하는 것이다.
 *
 * MSI 도 자체 구현이다. DesignWare 처럼 컨트롤러 안에 MSI 수신기가 있고,
 * 그것을 IRQ 도메인으로 감싸 하위 장치에 벡터를 나눠 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리에 mediatek,mt7622-pcie 등이 있으면
 *   -> [이 파일] mtk_pcie_probe()
 *      -> 포트마다: 클럭/PHY/리셋 준비, 레지스터 설정, 링크 대기
 *      -> MSI 도메인 구성
 *      -> pci_host_probe() -> PCI 코어 열거
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트. config 접근은 잠금 아래.
 * 인터럽트 분배는 하드 IRQ.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 버스.
 * 아래쪽: PHY·클럭·리셋 프레임워크, 커널 IRQ 도메인, PCI 코어.
 * 공유 상태: struct mtk_pcie 와 그 아래 struct mtk_pcie_port 목록.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 미디어텍 SoC 를 쓴 라우터나 NAS 에 NVMe 를 붙이는 구성이 있는데,
 * 그때 성능 상한을 정하는 것이 이 컨트롤러다. 포트마다 레인 수가
 * 고정되어 있고(보통 x1), 세대도 Gen2 인 경우가 많아 드라이브의 성능이
 * 다 나오지 않는다. 그것이 드라이브 문제가 아니라 호스트 쪽 제약임을
 * 알아 두면 진단이 빨라진다.
 *
 * === 주요 함수/구조체 요약 ===
 * mtk_pcie_probe()        : 전체 초기화. 포트 목록을 만들고 각각 준비한다.
 * mtk_pcie_parse_port() / mtk_pcie_setup() : 디바이스 트리에서 포트 정보를
 *                           읽고 자원을 확보한다.
 * mtk_pcie_startup_port() / mtk_pcie_startup_port_v2() : 링크를 올린다.
 *                           구형과 신형의 절차가 달라 둘로 나뉘어 있다.
 * mtk_pcie_config_read() / mtk_pcie_config_write() : config 접근.
 * mtk_pcie_setup_irq() / mtk_pcie_intr_handler() : 인터럽트 설정과 분배.
 * mtk_pcie_allocate_msi_domains() 계열 : MSI 도메인 구성.
 * struct mtk_pcie         : 컨트롤러 전체.
 * struct mtk_pcie_port    : 루트 포트 하나. 자기 PHY·클럭·리셋을 갖는다.
 * struct mtk_pcie_soc     : 세대별 차이를 담은 콜백 표.
 */

#include <linux/clk.h> /* 리눅스 clk.h 헤더 포함; 클록 API; NVMe ASPM/전력 상태 전환 시 클럭 게이팅에 사용 */
#include <linux/delay.h> /* 리눅스 delay.h 헤더 포함; msleep/udelay; NVMe 링크 트레이닝 및 reset 대기에 사용 */
#include <linux/iopoll.h> /* 리눅스 iopoll.h 헤더 포함; readl_poll_timeout; NVMe 링크업/설정 완료 폴링에 사용 */
#include <linux/irq.h> /* 리눅스 irq.h 헤더 포함; IRQ 코어 API; NVMe MSI/INTx 인터럽트 디스패치에 사용 */
#include <linux/irqchip/chained_irq.h> /* 리눅스 irqchip/chained_irq.h 헤더 포함; chained irq 핸들러; PCIe INTx를 GIC로 연결 */
#include <linux/irqchip/irq-msi-lib.h> /* 리눅스 irqchip/irq-msi-lib.h 헤더 포함; MSI 라이브러리; NVMe MSI 할당 시 공유 helper */
#include <linux/irqdomain.h> /* 리눅스 irqdomain.h 헤더 포함; IRQ 도메인; NVMe MSI/INTx 가상 IRQ 매핑 */
#include <linux/kernel.h> /* 리눅스 kernel.h 헤더 포함; 커널 공통 매크로; PCIe/NVMe 코드 전반에 사용 */
#include <linux/mfd/syscon.h> /* 리눅스 mfd/syscon.h 헤더 포함; syscon regmap; 공유 PCIe CSR 접근 */
#include <linux/msi.h> /* 리눅스 msi.h 헤더 포함; PCI MSI/MSI-X 정의; NVMe 인터럽트 설정 */
#include <linux/module.h> /* 리눅스 module.h 헤더 포함; 모듈 매크로; PCIe 호스트 드라이버 적재 */
#include <linux/of_address.h> /* 리눅스 of_address.h 헤더 포함; DT 주소 파싱; NVMe BAR 메모리 윈도우 설정 */
#include <linux/of_pci.h> /* 리눅스 of_pci.h 헤더 포함; OF PCI helper; NVMe 열거용 domain/devfn 획득 */
#include <linux/of_platform.h> /* 리눅스 of_platform.h 헤더 포함; 플랫폼 드라이버 OF 바인딩; PCIe RC 초기화 */
#include <linux/pci.h> /* 리눅스 pci.h 헤더 포함; 핵심 PCI 정의; nvme/host/pci.c와 공유 */
#include <linux/phy/phy.h> /* 리눅스 phy/phy.h 헤더 포함; PHY API; NVMe SerDes 물리 링크 초기화 */
#include <linux/platform_device.h> /* 리눅스 platform_device.h 헤더 포함; 플랫폼 장치/드라이버 프레임워크 */
#include <linux/pm_runtime.h> /* 리눅스 pm_runtime.h 헤더 포함; 런타임 PM; NVMe 엔드포인트 전원 상태 연계 */
#include <linux/regmap.h> /* 리눅스 regmap.h 헤더 포함; regmap API; syscon 기반 공유 레지스터 접근 */
#include <linux/reset.h> /* 리눅스 reset.h 헤더 포함; 리셋 컨트롤러; NVMe PERST# 제어 */

#include "../pci.h" /* 내부 헤더 ../pci.h 포함; PCI host bridge 비공개 정의 */

/* PCIe shared registers */
#define PCIE_SYS_CFG		0x00 /* PCIE_SYS_CFG 매크로; 공유 시스템 설정 레지스터 오프셋; PERST/LTSSM 제어 */
#define PCIE_INT_ENABLE		0x0c /* PCIE_INT_ENABLE 매크로; 전역 인터럽트 활성화 레지스터; NVMe INTx 전달 게이트 */
#define PCIE_CFG_ADDR		0x20 /* PCIE_CFG_ADDR 매크로; 설정 접근 주소 레지스터; ECAM 스타일 NVMe 설정 사이클 */
#define PCIE_CFG_DATA		0x24 /* PCIE_CFG_DATA 매크로; 설정 접근 데이터 레지스터; NVMe BAR/CSR 읽기쓰기 경로 */

/* PCIe per port registers */
#define PCIE_BAR0_SETUP		0x10 /* PCIE_BAR0_SETUP 매크로; BAR0 설정 레지스터; NVMe EP에 보이는 메모리 윈도우 */
#define PCIE_CLASS		0x34 /* PCIE_CLASS 매크로; 클스/리비전 레지스터; 루트 포트 식별로 NVMe 열거 연결 */
#define PCIE_LINK_STATUS	0x50 /* PCIE_LINK_STATUS 매크로; 링크 상태 레지스터; bit0이 NVMe 링크업 여부 */

#define PCIE_PORT_INT_EN(x)	BIT(20 + (x)) /* PCIE_PORT_INT_EN 매크로; 포트별 인터럽트 활성화 비트; NVMe INTx 이벤트 신호 */
#define PCIE_PORT_PERST(x)	BIT(1 + (x)) /* PCIE_PORT_PERST 매크로; 포트별 PERST# 비트; NVMe 카드 리셋 제어 */
#define PCIE_PORT_LINKUP	BIT(0) /* PCIE_PORT_LINKUP 매크로; 링크업 상태 비트; NVMe 장치 접근 전에 확인 */
#define PCIE_BAR_MAP_MAX	GENMASK(31, 16) /* PCIE_BAR_MAP_MAX 매크로; BAR 어퍼처 최대 매핑 마스크 */

#define PCIE_BAR_ENABLE		BIT(0) /* PCIE_BAR_ENABLE 매크로; BAR 변환 활성화 비트; NVMe MMIO가 DRAM 도달 허용 */
#define PCIE_REVISION_ID	BIT(0) /* PCIE_REVISION_ID 매크로; 루트 포트 리비전 ID 값 */
#define PCIE_CLASS_CODE		(0x60400 << 8) /* PCIE_CLASS_CODE 매크로; 루트 포트 클래스 코드(PCI bridge); NVMe 버스에 노출 */
#define PCIE_CONF_REG(regn)	(((regn) & GENMASK(7, 2)) | /* PCIE_CONF_REG 매크로; 설정 TLP 레지스터 번호 필드 조합; NVMe 설정 공간 대상 */ \
				((((regn) >> 8) & GENMASK(3, 0)) << 24))
#define PCIE_CONF_FUN(fun)	(((fun) << 8) & GENMASK(10, 8)) /* PCIE_CONF_FUN 매크로; 설정 TLP function 번호 인코딩; NVMe는 보통 function 0 */
#define PCIE_CONF_DEV(dev)	(((dev) << 11) & GENMASK(15, 11)) /* PCIE_CONF_DEV 매크로; 설정 TLP device 번호 인코딩; NVMe EP BDF와 매칭 */
#define PCIE_CONF_BUS(bus)	(((bus) << 16) & GENMASK(23, 16)) /* PCIE_CONF_BUS 매크로; 설정 TLP bus 번호 인코딩; NVMe 다운스트림 버스 라우팅 */
#define PCIE_CONF_ADDR(regn, fun, dev, bus) /* PCIE_CONF_ADDR 매크로; 전체 설정 주소 조합; NVMe PCI 헤더 읽기 전에 사용 */ \
	(PCIE_CONF_REG(regn) | PCIE_CONF_FUN(fun) | \
	 PCIE_CONF_DEV(dev) | PCIE_CONF_BUS(bus)) /* PCIE_CONF_DEV() 함수 호출; NVMe PCIe host 흐름에서 사용 */

/* MediaTek specific configuration registers */
#define PCIE_FTS_NUM		0x70c /* PCIE_FTS_NUM 매크로; FTS 개수 레지스터; NVMe ASPM L0s 빠진 후 재동기화 */
#define PCIE_FTS_NUM_MASK	GENMASK(15, 8) /* PCIE_FTS_NUM_MASK 매크로; FTS 필드 마스크 */
#define PCIE_FTS_NUM_L0(x)	((x) & 0xff << 8) /* PCIE_FTS_NUM_L0 매크로; L0s 종료 시 FTS 개수 값 */

#define PCIE_FC_CREDIT		0x73c /* PCIE_FC_CREDIT 매크로; 플로우 제어 크레딧 레지스터; NVMe TLP 처리율 영향 */
#define PCIE_FC_CREDIT_MASK	(GENMASK(31, 31) | GENMASK(28, 16)) /* PCIE_FC_CREDIT_MASK 매크로; FC 크레딧 마스크 */
#define PCIE_FC_CREDIT_VAL(x)	((x) << 16) /* PCIE_FC_CREDIT_VAL 매크로; FC 크레딧 값; NVMe DMA 대역폭 유지용 */

/* PCIe V2 share registers */
#define PCIE_SYS_CFG_V2		0x0 /* PCIE_SYS_CFG_V2 매크로; V2 공유 시스템 설정 레지스터 오프셋 */
#define PCIE_CSR_LTSSM_EN(x)	BIT(0 + (x) * 8) /* PCIE_CSR_LTSSM_EN 매크로; 포트 x LTSSM 활성화 비트; NVMe 링크 트레이닝 시작 */
#define PCIE_CSR_ASPM_L1_EN(x)	BIT(1 + (x) * 8) /* PCIE_CSR_ASPM_L1_EN 매크로; 포트 x ASPM L1 활성화 비트; NVMe 유휴 시 절전 */

/* PCIe V2 per-port registers */
#define PCIE_MSI_VECTOR		0x0c0 /* PCIE_MSI_VECTOR 매크로; MSI 벡터 레지스터 오프셋; NVMe MSI 쓰기가 도달 */

#define PCIE_CONF_VEND_ID	0x100 /* PCIE_CONF_VEND_ID 매크로; 루트 포트 vendor ID 필드 오프셋 */
#define PCIE_CONF_DEVICE_ID	0x102 /* PCIE_CONF_DEVICE_ID 매크로; 루트 포트 device ID 필드 오프셋 */
#define PCIE_CONF_CLASS_ID	0x106 /* PCIE_CONF_CLASS_ID 매크로; 루트 포트 class ID 필드 오프셋 */

#define PCIE_INT_MASK		0x420 /* PCIE_INT_MASK 매크로; 인터럽트 마스크 레지스터; NVMe INTx/MSI 마스킹 */
#define INTX_MASK		GENMASK(19, 16) /* INTX_MASK 매크로; 레거시 INTx 라인 마스크 */
#define INTX_SHIFT		16 /* INTX_SHIFT 매크로; INTx 상태 비트 시프트 값 */
#define PCIE_INT_STATUS		0x424 /* PCIE_INT_STATUS 매크로; 인터럽트 상태 레지스터; NVMe INTx/MSI 이벤트 반영 */
#define MSI_STATUS		BIT(23) /* MSI_STATUS 매크로; MSI 인터럽트 pending 비트 */
#define PCIE_IMSI_STATUS	0x42c /* PCIE_IMSI_STATUS 매크로; MSI 벡터별 상태; NVMe MSI 처리 후 ack */
#define PCIE_IMSI_ADDR		0x430 /* PCIE_IMSI_ADDR 매크로; MSI 목적지 주소 레지스터; NVMe MSI 쓰기 주소 */
#define MSI_MASK		BIT(23) /* MSI_MASK 매크로; MSI 인터럽트 활성화/마스킹 비트 */
#define MTK_MSI_IRQS_NUM	32 /* MTK_MSI_IRQS_NUM 매크로; 지원 MSI 벡터 개수; NVMe 할당 가능 MSI 상한 */

#define PCIE_AHB_TRANS_BASE0_L	0x438 /* PCIE_AHB_TRANS_BASE0_L 매크로; AHB->PCIe 변환 기저 하위 32비트; NVMe DMA 접근 */
#define PCIE_AHB_TRANS_BASE0_H	0x43c /* PCIE_AHB_TRANS_BASE0_H 매크로; AHB->PCIe 변환 기저 상위 32비트 */
#define AHB2PCIE_SIZE(x)	((x) & GENMASK(4, 0)) /* AHB2PCIE_SIZE 매크로; AHB2PCIe 윈도우 크기 인코딩; NVMe BAR 매핑 범위 */
#define PCIE_AXI_WINDOW0	0x448 /* PCIE_AXI_WINDOW0 매크로; PCIe->AXI 윈도우 레지스터; NVMe DMA 인바운드 매핑 */
#define WIN_ENABLE		BIT(7) /* WIN_ENABLE 매크로; PCIe 인바운드 윈도우 활성화 비트; NVMe DMA */
/*
 * Define PCIe to AHB window size as 2^33 to support max 8GB address space
 * translate, support least 4GB DRAM size access from EP DMA(physical DRAM
 * start from 0x40000000).
 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
#define PCIE2AHB_SIZE	0x21 /* PCIE2AHB_SIZE 매크로; 인바운드 윈도우 크기(2^33); 최대 8GB NVMe DMA */

/* PCIe V2 configuration transaction header */
#define PCIE_CFG_HEADER0	0x460 /* PCIE_CFG_HEADER0 매크로; 설정 TLP 헤더 DW0; NVMe 설정 사이클 */
#define PCIE_CFG_HEADER1	0x464 /* PCIE_CFG_HEADER1 매크로; 설정 TLP 헤더 DW1 */
#define PCIE_CFG_HEADER2	0x468 /* PCIE_CFG_HEADER2 매크로; 설정 TLP 헤더 DW2 */
#define PCIE_CFG_WDATA		0x470 /* PCIE_CFG_WDATA 매크로; 설정 쓰기 데이터 레지스터 */
#define PCIE_APP_TLP_REQ	0x488 /* PCIE_APP_TLP_REQ 매크로; 애플리케이션 TLP 요청 레지스터; NVMe 설정 TLP 트리거 */
#define PCIE_CFG_RDATA		0x48c /* PCIE_CFG_RDATA 매크로; 설정 읽기 데이터 레지스터 */
#define APP_CFG_REQ		BIT(0) /* APP_CFG_REQ 매크로; 설정 TLP 요청 시작 비트; NVMe BDF 대상 */
#define APP_CPL_STATUS		GENMASK(7, 5) /* APP_CPL_STATUS 매크로; Completion 상태 마스크; 비0이면 NVMe 설정 접근 실패 */

#define CFG_WRRD_TYPE_0		4 /* CFG_WRRD_TYPE_0 매크로; Type0 설정 TLP 타입; NVMe EP 직접 접근 */
#define CFG_WR_FMT		2 /* CFG_WR_FMT 매크로; 설정 쓰기 TLP 형식 */
#define CFG_RD_FMT		0 /* CFG_RD_FMT 매크로; 설정 읽기 TLP 형식 */

#define CFG_DW0_LENGTH(length)	((length) & GENMASK(9, 0)) /* CFG_DW0_LENGTH 매크로; TLP DW0 길이 필드 */
#define CFG_DW0_TYPE(type)	(((type) << 24) & GENMASK(28, 24)) /* CFG_DW0_TYPE 매크로; TLP DW0 타입 필드 */
#define CFG_DW0_FMT(fmt)	(((fmt) << 29) & GENMASK(31, 29)) /* CFG_DW0_FMT 매크로; TLP DW0 형식 필드 */
#define CFG_DW2_REGN(regn)	((regn) & GENMASK(11, 2)) /* CFG_DW2_REGN 매크로; TLP DW2 레지스터 번호 필드 */
#define CFG_DW2_FUN(fun)	(((fun) << 16) & GENMASK(18, 16)) /* CFG_DW2_FUN 매크로; TLP DW2 function 번호 필드 */
#define CFG_DW2_DEV(dev)	(((dev) << 19) & GENMASK(23, 19)) /* CFG_DW2_DEV 매크로; TLP DW2 device 번호 필드 */
#define CFG_DW2_BUS(bus)	(((bus) << 24) & GENMASK(31, 24)) /* CFG_DW2_BUS 매크로; TLP DW2 bus 번호 필드 */
#define CFG_HEADER_DW0(type, fmt) /* CFG_HEADER_DW0 매크로; 설정 TLP 헤더 DW0 조합; NVMe 설정 사이클 생성 */ \
	(CFG_DW0_LENGTH(1) | CFG_DW0_TYPE(type) | CFG_DW0_FMT(fmt))
#define CFG_HEADER_DW1(where, size) /* CFG_HEADER_DW1 매크로; 설정 TLP 헤더 DW1 조합; 바이트 마스크 */ \
	(GENMASK(((size) - 1), 0) << ((where) & 0x3))
#define CFG_HEADER_DW2(regn, fun, dev, bus) /* CFG_HEADER_DW2 매크로; 설정 TLP 헤더 DW2 조합; BDF+레지스터 */ \
	(CFG_DW2_REGN(regn) | CFG_DW2_FUN(fun) | \
	CFG_DW2_DEV(dev) | CFG_DW2_BUS(bus)) /* CFG_DW2_DEV() 함수 호출; NVMe PCIe host 흐름에서 사용 */

#define PCIE_RST_CTRL		0x510 /* PCIE_RST_CTRL 매크로; 리셋 제어 레지스터; PHY/MAC/PERST 비트 포함 */
#define PCIE_PHY_RSTB		BIT(0) /* PCIE_PHY_RSTB 매크로; PHY 리셋 해제 비트 */
#define PCIE_PIPE_SRSTB		BIT(1) /* PCIE_PIPE_SRSTB 매크로; PIPE 리셋 해제 비트 */
#define PCIE_MAC_SRSTB		BIT(2) /* PCIE_MAC_SRSTB 매크로; MAC 리셋 해제 비트 */
#define PCIE_CRSTB		BIT(3) /* PCIE_CRSTB 매크로; 설정 공간 리셋 해제 비트 */
#define PCIE_PERSTB		BIT(8) /* PCIE_PERSTB 매크로; PERST# 리셋 해제 비트; NVMe EP 나감 리셋 */
#define PCIE_LINKDOWN_RST_EN	GENMASK(15, 13) /* PCIE_LINKDOWN_RST_EN 매크로; 링크 다운 자동 리셋 마스크; 핫플러그/AER 연계 */
#define PCIE_LINK_STATUS_V2	0x804 /* PCIE_LINK_STATUS_V2 매크로; V2 링크 상태 레지스터 오프셋 */
#define PCIE_PORT_LINKUP_V2	BIT(10) /* PCIE_PORT_LINKUP_V2 매크로; V2 링크업 비트 */

struct mtk_pcie_port; /* 구조체 전방 선언; NVMe PCIe host 관련 incomplete 타입 */

/**
 * enum mtk_pcie_quirks - MTK PCIe quirks
 * @MTK_PCIE_FIX_CLASS_ID: host's class ID needed to be fixed
 * @MTK_PCIE_FIX_DEVICE_ID: host's device ID needed to be fixed
 * @MTK_PCIE_NO_MSI: Bridge has no MSI support, and relies on an external block
 * @MTK_PCIE_SKIP_RSTB: Skip calling RSTB bits on PCIe probe
 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
enum mtk_pcie_quirks {
	MTK_PCIE_FIX_CLASS_ID = BIT(0),
	MTK_PCIE_FIX_DEVICE_ID = BIT(1),
	MTK_PCIE_NO_MSI = BIT(2),
	MTK_PCIE_SKIP_RSTB = BIT(3),
};

/**
 * struct mtk_pcie_soc - differentiate between host generations
 * @device_id: device ID which this host need to be fixed
 * @ops: pointer to configuration access functions
 * @startup: pointer to controller setting functions
 * @setup_irq: pointer to initialize IRQ functions
 * @quirks: PCIe device quirks.
 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
struct mtk_pcie_soc {
	unsigned int device_id; /* 필드 device_id; 고정할 루트 포트 device ID; NVMe BDF 식별에 영향 */
	struct pci_ops *ops; /* 필드 ops; pci_ops 포인터; NVMe 설정 읽기/쓰기 콜백 */
	int (*startup)(struct mtk_pcie_port *port); /* 필드 startup; 포트 초기화 콜백; NVMe 접근 전 링크 트레이닝 */
	int (*setup_irq)(struct mtk_pcie_port *port, struct device_node *node); /* 필드 setup_irq; IRQ 도메인 초기화 콜백; NVMe MSI/INTx */
	enum mtk_pcie_quirks quirks; /* 필드 quirks; SoC별 quirk 플래그; NVMe MSI/클래스/리셋 제어 */
};

/**
 * struct mtk_pcie_port - PCIe port information
 * @base: IO mapped register base
 * @list: port list
 * @pcie: pointer to PCIe host info
 * @reset: pointer to port reset control
 * @sys_ck: pointer to transaction/data link layer clock
 * @ahb_ck: pointer to AHB slave interface operating clock for CSR access
 *          and RC initiated MMIO access
 * @axi_ck: pointer to application layer MMIO channel operating clock
 * @aux_ck: pointer to pe2_mac_bridge and pe2_mac_core operating clock
 *          when pcie_mac_ck/pcie_pipe_ck is turned off
 * @obff_ck: pointer to OBFF functional block operating clock
 * @pipe_ck: pointer to LTSSM and PHY/MAC layer operating clock
 * @phy: pointer to PHY control block
 * @slot: port slot
 * @irq: GIC irq
 * @irq_domain: legacy INTx IRQ domain
 * @inner_domain: inner IRQ domain
 * @lock: protect the msi_irq_in_use bitmap
 * @msi_irq_in_use: bit map for assigned MSI IRQ
 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
struct mtk_pcie_port {
	void __iomem *base; /* 필드 base; 포트 레지스터 베이스; NVMe 설정/MMIO/MSI 접근 */
	struct list_head list; /* 필드 list; 포트 리스트 연결; NVMe 열거 대상 포트 관리 */
	struct mtk_pcie *pcie; /* 필드 pcie; NVMe 호스트 구조체 멤버 */
	struct reset_control *reset; /* 필드 reset; 리셋 컨트롤; NVMe EP PERST# */
	struct clk *sys_ck; /* 필드 sys_ck; 트랜잭션/데이터링크 계층 클럭; NVMe TLP */
	struct clk *ahb_ck; /* 필드 ahb_ck; AHB 슬레이브 인터페이스 클럭; NVMe CSR/MMIO 접근 */
	struct clk *axi_ck; /* 필드 axi_ck; 애플리케이션 계층 MMIO 채널 클럭; NVMe */
	struct clk *aux_ck; /* 필드 aux_ck; MAC 브리지/코어 보조 클럭; NVMe 절전 유지 */
	struct clk *obff_ck; /* 필드 obff_ck; OBFF 기능 블록 클럭; NVMe 전력 관리 */
	struct clk *pipe_ck; /* 필드 pipe_ck; LTSSM 및 PHY/MAC 계층 클럭; NVMe 링크 */
	struct phy *phy; /* 필드 phy; PHY 제어 블록; NVMe SerDes 물리 링크 */
	u32 slot; /* 필드 slot; 포트 슬롯 번호; NVMe EP 물리 슬롯 */
	int irq; /* 필드 irq; GIC IRQ 번호; NVMe MSI/INTx 상위 인터럽트 */
	struct irq_domain *irq_domain; /* 필드 irq_domain; 레거시 INTx IRQ 도메인; NVMe 폴백 인터럽트 */
	struct irq_domain *inner_domain; /* 필드 inner_domain; 내부 MSI IRQ 도메인; NVMe MSI 벡터 */
	struct mutex lock; /* 필드 lock; msi_irq_in_use 비트맵 보호 뮤텍스 */
	DECLARE_BITMAP(msi_irq_in_use, MTK_MSI_IRQS_NUM); /* MSI 벡터 사용 비트맵 선언; NVMe */
};

/**
 * struct mtk_pcie - PCIe host information
 * @dev: pointer to PCIe device
 * @base: IO mapped register base
 * @cfg: IO mapped register map for PCIe config
 * @free_ck: free-run reference clock
 * @ports: pointer to PCIe port information
 * @soc: pointer to SoC-dependent operations
 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
struct mtk_pcie {
	struct device *dev; /* 필드 dev; NVMe 호스트 구조체 멤버 */
	void __iomem *base; /* 필드 base; 포트 레지스터 베이스; NVMe 설정/MMIO/MSI 접근 */
	struct regmap *cfg; /* 필드 cfg; NVMe 호스트 구조체 멤버 */
	struct clk *free_ck; /* 필드 free_ck; free-running 레퍼런스 클럭 */

	struct list_head ports; /* 필드 ports; 활성 PCIe 포트 리스트 */
	const struct mtk_pcie_soc *soc; /* 필드 soc; NVMe 호스트 구조체 멤버 */
};

static void mtk_pcie_subsys_powerdown(struct mtk_pcie *pcie) /* mtk_pcie_subsys_powerdown() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	clk_disable_unprepare(pcie->free_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */

	pm_runtime_put_sync(dev); /* 런타임 PM 참조 해제; NVMe 호스트 절전 */
	pm_runtime_disable(dev); /* 런타임 PM 비활성화 */
} /* 코드 블록 종료 */

static void mtk_pcie_port_free(struct mtk_pcie_port *port) /* mtk_pcie_port_free() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = port->pcie; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	devm_iounmap(dev, port->base); /* 포트 레지스터 영역 언매핑; NVMe 포트 제거 시 */
	list_del(&port->list); /* 포트를 호스트 리스트에서 제거 */
	devm_kfree(dev, port); /* 포트 구조체 메모리 해제; NVMe 장치 미연결 시 */
} /* 코드 블록 종료 */

static void mtk_pcie_put_resources(struct mtk_pcie *pcie) /* mtk_pcie_put_resources() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port, *tmp; /* 지역 변수 port, tmp 선언; NVMe 호스트 동작 상태 저장 */

	list_for_each_entry_safe(port, tmp, &pcie->ports, list) { /* PCIe 포트 리스트를 순회; NVMe 엔드포인트 처리 */
		phy_power_off(port->phy); /* phy_power_off() 함수 호출; NVMe PCIe host 흐름에서 사용 */
		phy_exit(port->phy); /* phy_exit() 함수 호출; NVMe PCIe host 흐름에서 사용 */
		clk_disable_unprepare(port->pipe_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->obff_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->axi_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->aux_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->ahb_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->sys_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		mtk_pcie_port_free(port); /* mtk_pcie_port_free() 함수 호출; NVMe PCIe host 흐름에서 사용 */
	} /* 코드 블록 종료 */

	mtk_pcie_subsys_powerdown(pcie); /* mtk_pcie_subsys_powerdown() 함수 호출; NVMe PCIe host 흐름에서 사용 */
} /* 코드 블록 종료 */

static int mtk_pcie_check_cfg_cpld(struct mtk_pcie_port *port) /* mtk_pcie_check_cfg_cpld() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	u32 val; /* 지역 변수 val 선언; NVMe 호스트 동작 상태 저장 */
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	err = readl_poll_timeout_atomic(port->base + PCIE_APP_TLP_REQ, val,
					!(val & APP_CFG_REQ), 10,
					100 * USEC_PER_MSEC);
	if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PCIBIOS_SET_FAILED; /* PCI 설정 접근 결과 반환; NVMe 구성 읽기/쓰기 상태 */

	if (readl(port->base + PCIE_APP_TLP_REQ) & APP_CPL_STATUS) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PCIBIOS_SET_FAILED; /* PCI 설정 접근 결과 반환; NVMe 구성 읽기/쓰기 상태 */

	return PCIBIOS_SUCCESSFUL; /* PCI 설정 접근 결과 반환; NVMe 구성 읽기/쓰기 상태 */
} /* 코드 블록 종료 */

static int mtk_pcie_hw_rd_cfg(struct mtk_pcie_port *port, u32 bus, u32 devfn, /* mtk_pcie_hw_rd_cfg() 함수 정의; NVMe PCIe host 동작 중 호출 */
			      int where, int size, u32 *val)
{ /* 코드 블록 시작 */
	u32 tmp; /* 지역 변수 tmp 선언; NVMe 호스트 동작 상태 저장 */

	/* Write PCIe configuration transaction header for Cfgrd */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_RD_FMT), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	writel(CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       port->base + PCIE_CFG_HEADER2);

	/* Trigger h/w to transmit Cfgrd TLP */
	tmp = readl(port->base + PCIE_APP_TLP_REQ); /* 임시 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	tmp |= APP_CFG_REQ; /* 임시 값를 갱신; NVMe 호스트 동작에 필요한 레지스터/상태 */
	writel(tmp, port->base + PCIE_APP_TLP_REQ); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* Check completion status */
	if (mtk_pcie_check_cfg_cpld(port)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PCIBIOS_SET_FAILED; /* PCI 설정 접근 결과 반환; NVMe 구성 읽기/쓰기 상태 */

	/* Read cpld payload of Cfgrd */
	*val = readl(port->base + PCIE_CFG_RDATA); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */

	if (size == 1) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		*val = (*val >> (8 * (where & 3))) & 0xff; /* 임시 레지스터 값를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	else if (size == 2)
		*val = (*val >> (8 * (where & 3))) & 0xffff; /* 임시 레지스터 값를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	return PCIBIOS_SUCCESSFUL; /* PCI 설정 접근 결과 반환; NVMe 구성 읽기/쓰기 상태 */
} /* 코드 블록 종료 */

static int mtk_pcie_hw_wr_cfg(struct mtk_pcie_port *port, u32 bus, u32 devfn, /* mtk_pcie_hw_wr_cfg() 함수 정의; NVMe PCIe host 동작 중 호출 */
			      int where, int size, u32 val)
{ /* 코드 블록 시작 */
	/* Write PCIe configuration transaction header for Cfgwr */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_WR_FMT), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	writel(CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       port->base + PCIE_CFG_HEADER2);

	/* Write Cfgwr data */
	val = val << 8 * (where & 3); /* 임시 레지스터 값를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	writel(val, port->base + PCIE_CFG_WDATA); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* Trigger h/w to transmit Cfgwr TLP */
	val = readl(port->base + PCIE_APP_TLP_REQ); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val |= APP_CFG_REQ; /* 임시 레지스터 값를 갱신; NVMe 호스트 동작에 필요한 레지스터/상태 */
	writel(val, port->base + PCIE_APP_TLP_REQ); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* Check completion status */
	return mtk_pcie_check_cfg_cpld(port); /* mtk_pcie_check_cfg_cpld(port) 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */

static struct mtk_pcie_port *mtk_pcie_find_port(struct pci_bus *bus,
						unsigned int devfn)
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = bus->sysdata; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct mtk_pcie_port *port; /* 지역 변수 port 선언; NVMe 호스트 동작 상태 저장 */
	struct pci_dev *dev = NULL; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	/*
	 * Walk the bus hierarchy to get the devfn value
	 * of the port in the root bus.
	 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
	while (bus && bus->number) { /* 조건이 참인 동안 NVMe 상태 변경을 대기/반복 */
		dev = bus->self; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
		bus = dev->bus; /* PCI 버스 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
		devfn = dev->devfn; /* device/function 인코딩를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	} /* 코드 블록 종료 */

	list_for_each_entry(port, &pcie->ports, list) /* PCIe 포트 리스트를 순회; NVMe 엔드포인트 처리 */
		if (port->slot == PCI_SLOT(devfn)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			return port; /* port 값 반환; NVMe 호스트 흐름 제어 */

	return NULL; /* NULL 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */

static int mtk_pcie_config_read(struct pci_bus *bus, unsigned int devfn, /* mtk_pcie_config_read() 함수 정의; NVMe PCIe host 동작 중 호출 */
				int where, int size, u32 *val)
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port; /* 지역 변수 port 선언; NVMe 호스트 동작 상태 저장 */
	u32 bn = bus->number; /* PCI 버스 번호를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	port = mtk_pcie_find_port(bus, devfn); /* MediaTek PCIe 포트 데이터에 NVMe 장치 PCI 버스를 서비스하는 루트 포트 탐색 결과를 대입; NVMe 호스트 상태 갱신 */
	if (!port) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PCIBIOS_DEVICE_NOT_FOUND; /* PCI 설정 접근 결과 반환; NVMe 구성 읽기/쓰기 상태 */

	return mtk_pcie_hw_rd_cfg(port, bn, devfn, where, size, val); /* mtk_pcie_hw_rd_cfg(port, bn, devfn, where, size, val) 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */

static int mtk_pcie_config_write(struct pci_bus *bus, unsigned int devfn, /* mtk_pcie_config_write() 함수 정의; NVMe PCIe host 동작 중 호출 */
				 int where, int size, u32 val)
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port; /* 지역 변수 port 선언; NVMe 호스트 동작 상태 저장 */
	u32 bn = bus->number; /* PCI 버스 번호를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	port = mtk_pcie_find_port(bus, devfn); /* MediaTek PCIe 포트 데이터에 NVMe 장치 PCI 버스를 서비스하는 루트 포트 탐색 결과를 대입; NVMe 호스트 상태 갱신 */
	if (!port) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PCIBIOS_DEVICE_NOT_FOUND; /* PCI 설정 접근 결과 반환; NVMe 구성 읽기/쓰기 상태 */

	return mtk_pcie_hw_wr_cfg(port, bn, devfn, where, size, val); /* mtk_pcie_hw_wr_cfg(port, bn, devfn, where, size, val) 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */

static struct pci_ops mtk_pcie_ops_v2 = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.read  = mtk_pcie_config_read, /* 멤버 .read 초기화; PCI 설정 읽기 콜백; NVMe; NVMe PCIe host */
	.write = mtk_pcie_config_write, /* 멤버 .write 초기화; PCI 설정 쓰기 콜백; NVMe; NVMe PCIe host */
};

static void mtk_compose_msi_msg(struct irq_data *data, struct msi_msg *msg) /* mtk_compose_msi_msg() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port = irq_data_get_irq_chip_data(data); /* MediaTek PCIe 포트 데이터에 irq_data에서 포트 포인터 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	phys_addr_t addr; /* 지역 변수 addr 선언; NVMe 호스트 동작 상태 저장 */

	/* MT2712/MT7622 only support 32-bit MSI addresses */
	addr = virt_to_phys(port->base + PCIE_MSI_VECTOR); /* 물리 주소에 가상 주소를 물리 주소로 변환; NVMe MSI 목적지 결과를 대입; NVMe 호스트 상태 갱신 */
	msg->address_hi = 0; /* msg->address_hi를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	msg->address_lo = lower_32_bits(addr); /* msg->address_lo에 물리주소 하위 32비트; NVMe DMA/MSI 주소 결과를 대입; NVMe 호스트 상태 갱신 */

	msg->data = data->hwirq; /* msg->data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	dev_dbg(port->pcie->dev, "msi#%d address_hi %#x address_lo %#x\n", /* 디버그 로그; NVMe MSI 할당 추적 */
		(int)data->hwirq, msg->address_hi, msg->address_lo);
} /* 코드 블록 종료 */

static void mtk_msi_ack_irq(struct irq_data *data) /* mtk_msi_ack_irq() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port = irq_data_get_irq_chip_data(data); /* MediaTek PCIe 포트 데이터에 irq_data에서 포트 포인터 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	u32 hwirq = data->hwirq; /* 하드웨어 IRQ 번호를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	writel(1 << hwirq, port->base + PCIE_IMSI_STATUS); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
} /* 코드 블록 종료 */

static struct irq_chip mtk_msi_bottom_irq_chip = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.name			= "MTK MSI", /* 멤버 .name 초기화; 드라이버/칩 이름; NVMe PCIe host */
	.irq_compose_msi_msg	= mtk_compose_msi_msg, /* 멤버 .irq_compose_msi_msg 초기화; MSI 메시지 구성 콜백; NVMe; NVMe PCIe host */
	.irq_ack		= mtk_msi_ack_irq, /* 멤버 .irq_ack 초기화; MSI ack 콜백; NVMe; NVMe PCIe host */
};

static int mtk_pcie_irq_domain_alloc(struct irq_domain *domain, unsigned int virq, /* mtk_pcie_irq_domain_alloc() 함수 정의; NVMe PCIe host 동작 중 호출 */
				     unsigned int nr_irqs, void *args)
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port = domain->host_data; /* MediaTek PCIe 포트 데이터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	unsigned long bit;

	WARN_ON(nr_irqs != 1); /* 조건 위박 시 경고 */
	mutex_lock(&port->lock); /* MSI 할당 뮤텍스 획득; NVMe 드라이버 간 경쟁 방지 */

	bit = find_first_zero_bit(port->msi_irq_in_use, MTK_MSI_IRQS_NUM); /* 비트 인덱스에 사용되지 않은 MSI 벡터 검색; NVMe 결과를 대입; NVMe 호스트 상태 갱신 */
	if (bit >= MTK_MSI_IRQS_NUM) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		mutex_unlock(&port->lock); /* MSI 할당 뮤텍스 해제 */
		return -ENOSPC; /* -ENOSPC 오류 반환; NVMe 장치 열거 중단 */
	} /* 코드 블록 종료 */

	__set_bit(bit, port->msi_irq_in_use); /* MSI 벡터 사용 중 표시; NVMe */

	mutex_unlock(&port->lock); /* MSI 할당 뮤텍스 해제 */

	irq_domain_set_info(domain, virq, bit, &mtk_msi_bottom_irq_chip, /* Linux virq를 MSI 칩/도메인에 연결; NVMe */
			    domain->host_data, handle_edge_irq,
			    NULL, NULL);

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static void mtk_pcie_irq_domain_free(struct irq_domain *domain, /* mtk_pcie_irq_domain_free() 함수 정의; NVMe PCIe host 동작 중 호출 */
				     unsigned int virq, unsigned int nr_irqs)
{ /* 코드 블록 시작 */
	struct irq_data *d = irq_domain_get_irq_data(domain, virq); /* irq_data 포인터에 virq에 해당하는 irq_data 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	struct mtk_pcie_port *port = irq_data_get_irq_chip_data(d); /* MediaTek PCIe 포트 데이터에 irq_data에서 포트 포인터 획득 결과를 대입; NVMe 호스트 상태 갱신 */

	mutex_lock(&port->lock); /* MSI 할당 뮤텍스 획득; NVMe 드라이버 간 경쟁 방지 */

	if (!test_bit(d->hwirq, port->msi_irq_in_use)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(port->pcie->dev, "trying to free unused MSI#%lu\n", /* 에러 로그; NVMe 열거 실패 원인 기록 */
			d->hwirq);
	else
		__clear_bit(d->hwirq, port->msi_irq_in_use); /* MSI 벡터 사용 해제; NVMe */

	mutex_unlock(&port->lock); /* MSI 할당 뮤텍스 해제 */

	irq_domain_free_irqs_parent(domain, virq, nr_irqs); /* irq_domain_free_irqs_parent() 함수 호출; NVMe PCIe host 흐름에서 사용 */
} /* 코드 블록 종료 */

static const struct irq_domain_ops msi_domain_ops = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.alloc	= mtk_pcie_irq_domain_alloc, /* 멤버 .alloc 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
	.free	= mtk_pcie_irq_domain_free, /* 멤버 .free 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
};

#define MTK_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| /* MTK_MSI_FLAGS_REQUIRED 매크로 정의; NVMe PCIe 호스트 제어에 사용 */ \
				MSI_FLAG_USE_DEF_CHIP_OPS	| \
				MSI_FLAG_NO_AFFINITY)

#define MTK_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK		| /* MTK_MSI_FLAGS_SUPPORTED 매크로 정의; NVMe PCIe 호스트 제어에 사용 */ \
				 MSI_FLAG_PCI_MSIX)

static const struct msi_parent_ops mtk_msi_parent_ops = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.required_flags		= MTK_MSI_FLAGS_REQUIRED, /* 멤버 .required_flags 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
	.supported_flags	= MTK_MSI_FLAGS_SUPPORTED, /* 멤버 .supported_flags 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI, /* 멤버 .bus_select_token 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK, /* 멤버 .chip_flags 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
	.prefix			= "MTK-", /* 멤버 .prefix 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info, /* 멤버 .init_dev_msi_info 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
};

static int mtk_pcie_allocate_msi_domains(struct mtk_pcie_port *port) /* mtk_pcie_allocate_msi_domains() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	mutex_init(&port->lock); /* MSI 비트맵 보호 뮤텍스 초기화 */

	struct irq_domain_info info = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
		.fwnode		= dev_fwnode(port->pcie->dev), /* 멤버 .fwnode 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
		.ops		= &msi_domain_ops, /* 멤버 .ops 초기화; pci_ops; NVMe 설정 읽기/쓰기 콜백; NVMe PCIe host */
		.host_data	= port, /* 멤버 .host_data 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
		.size		= MTK_MSI_IRQS_NUM, /* 멤버 .size 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
	};

	port->inner_domain = msi_create_parent_irq_domain(&info, &mtk_msi_parent_ops); /* port->inner_domain에 MSI 부모 도메인 생성; NVMe 결과를 대입; NVMe 호스트 상태 갱신 */
	if (!port->inner_domain) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(port->pcie->dev, "failed to create IRQ domain\n"); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		return -ENOMEM; /* -ENOMEM 오류 반환; NVMe 장치 열거 중단 */
	} /* 코드 블록 종료 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static void mtk_pcie_enable_msi(struct mtk_pcie_port *port) /* mtk_pcie_enable_msi() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	u32 val; /* 지역 변수 val 선언; NVMe 호스트 동작 상태 저장 */
	phys_addr_t msg_addr; /* 지역 변수 msg_addr 선언; NVMe 호스트 동작 상태 저장 */

	msg_addr = virt_to_phys(port->base + PCIE_MSI_VECTOR); /* MSI 메시지 물리 주소에 가상 주소를 물리 주소로 변환; NVMe MSI 목적지 결과를 대입; NVMe 호스트 상태 갱신 */
	val = lower_32_bits(msg_addr); /* 임시 레지스터 값에 물리주소 하위 32비트; NVMe DMA/MSI 주소 결과를 대입; NVMe 호스트 상태 갱신 */
	writel(val, port->base + PCIE_IMSI_ADDR); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	val = readl(port->base + PCIE_INT_MASK); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val &= ~MSI_MASK; /* 임시 레지스터 값를 갱신; NVMe 호스트 동작에 필요한 레지스터/상태 */
	writel(val, port->base + PCIE_INT_MASK); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
} /* 코드 블록 종료 */

static void mtk_pcie_irq_teardown(struct mtk_pcie *pcie) /* mtk_pcie_irq_teardown() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port, *tmp; /* 지역 변수 port, tmp 선언; NVMe 호스트 동작 상태 저장 */

	list_for_each_entry_safe(port, tmp, &pcie->ports, list) { /* PCIe 포트 리스트를 순회; NVMe 엔드포인트 처리 */
		irq_set_chained_handler_and_data(port->irq, NULL, NULL); /* GIC IRQ를 PCIe demux에 연결; NVMe */

		if (port->irq_domain) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			irq_domain_remove(port->irq_domain); /* IRQ 도메인 제거 */

		if (IS_ENABLED(CONFIG_PCI_MSI)) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			if (port->inner_domain) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
				irq_domain_remove(port->inner_domain); /* IRQ 도메인 제거 */
		} /* 코드 블록 종료 */

		irq_dispose_mapping(port->irq); /* irq_dispose_mapping() 함수 호출; NVMe PCIe host 흐름에서 사용 */
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

static int mtk_pcie_intx_map(struct irq_domain *domain, unsigned int irq, /* mtk_pcie_intx_map() 함수 정의; NVMe PCIe host 동작 중 호출 */
			     irq_hw_number_t hwirq)
{ /* 코드 블록 시작 */
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq); /* INTx 칩/핸들러 설정 */
	irq_set_chip_data(irq, domain->host_data); /* irq_set_chip_data() 함수 호출; NVMe PCIe host 흐름에서 사용 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static const struct irq_domain_ops intx_domain_ops = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.map = mtk_pcie_intx_map, /* 멤버 .map 초기화; 구조체/콜백 멤버 초기화; NVMe PCIe host */
};

static int mtk_pcie_init_irq_domain(struct mtk_pcie_port *port, /* mtk_pcie_init_irq_domain() 함수 정의; NVMe PCIe host 동작 중 호출 */
				    struct device_node *node)
{ /* 코드 블록 시작 */
	struct device *dev = port->pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct device_node *pcie_intc_node; /* 지역 변수 pcie_intc_node 선언; NVMe 호스트 동작 상태 저장 */
	int ret; /* 지역 변수 ret 선언; NVMe 호스트 동작 상태 저장 */

	/* Setup INTx */
	pcie_intc_node = of_get_next_child(node, NULL); /* pcie_intc_node에 INTx 인터럽트 컨트롤러 DT 노드 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (!pcie_intc_node) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "no PCIe Intc node found\n"); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		return -ENODEV; /* -ENODEV 오류 반환; NVMe 장치 열거 중단 */
	} /* 코드 블록 종료 */

	port->irq_domain = irq_domain_create_linear(of_fwnode_handle(pcie_intc_node), PCI_NUM_INTX,
						    &intx_domain_ops, port);
	of_node_put(pcie_intc_node); /* of_node_put() 함수 호출; NVMe PCIe host 흐름에서 사용 */
	if (!port->irq_domain) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to get INTx IRQ domain\n"); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		return -ENODEV; /* -ENODEV 오류 반환; NVMe 장치 열거 중단 */
	} /* 코드 블록 종료 */

	if (IS_ENABLED(CONFIG_PCI_MSI)) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		ret = mtk_pcie_allocate_msi_domains(port); /* 반환값에 NVMe에서 사용할 MSI 부모 도메인 생성 결과를 대입; NVMe 호스트 상태 갱신 */
		if (ret) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			irq_domain_remove(port->irq_domain); /* IRQ 도메인 제거 */
			return ret; /* ret 값 반환; NVMe 호스트 흐름 제어 */
		} /* 코드 블록 종료 */
	} /* 코드 블록 종료 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static void mtk_pcie_intr_handler(struct irq_desc *desc) /* mtk_pcie_intr_handler() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port = irq_desc_get_handler_data(desc); /* MediaTek PCIe 포트 데이터에 irq_desc에서 포트 데이터 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	struct irq_chip *irqchip = irq_desc_get_chip(desc); /* IRQ chip 기술자에 irq_desc에서 IRQ chip 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	unsigned long status;
	u32 bit = INTX_SHIFT; /* 비트 인덱스를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	chained_irq_enter(irqchip, desc); /* 체인 irq 프레임워크 진입; NVMe 인터럽트 처리 시작 */

	status = readl(port->base + PCIE_INT_STATUS); /* 인터럽트/상태 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	if (status & INTX_MASK) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		for_each_set_bit_from(bit, &status, PCI_NUM_INTX + INTX_SHIFT) { /* NVMe 관련 포트/장치/비트를 순회하는 루프 */
			/* Clear the INTx */
			writel(1 << bit, port->base + PCIE_INT_STATUS); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
			generic_handle_domain_irq(port->irq_domain, /* irq를 NVMe INTx/MSI 핸들러로 전달 */
						  bit - INTX_SHIFT);
		} /* 코드 블록 종료 */
	} /* 코드 블록 종료 */

	if (IS_ENABLED(CONFIG_PCI_MSI)) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		if (status & MSI_STATUS){ /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			unsigned long imsi_status;

			/*
			 * The interrupt status can be cleared even if the
			 * MSI status remains pending. As such, given the
			 * edge-triggered interrupt type, its status should
			 * be cleared before being dispatched to the
			 * handler of the underlying device.
			 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
			writel(MSI_STATUS, port->base + PCIE_INT_STATUS); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
			while ((imsi_status = readl(port->base + PCIE_IMSI_STATUS))) { /* 조건이 참인 동안 NVMe 상태 변경을 대기/반복 */
				for_each_set_bit(bit, &imsi_status, MTK_MSI_IRQS_NUM) /* NVMe 관련 포트/장치/비트를 순회하는 루프 */
					generic_handle_domain_irq(port->inner_domain, bit); /* irq를 NVMe INTx/MSI 핸들러로 전달 */
			} /* 코드 블록 종료 */
		} /* 코드 블록 종료 */
	} /* 코드 블록 종료 */

	chained_irq_exit(irqchip, desc); /* 체인 irq 프레임워크 종료; NVMe 인터럽트 처리 마침 */
} /* 코드 블록 종료 */

static int mtk_pcie_setup_irq(struct mtk_pcie_port *port, /* mtk_pcie_setup_irq() 함수 정의; NVMe PCIe host 동작 중 호출 */
			      struct device_node *node)
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = port->pcie; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct platform_device *pdev = to_platform_device(dev); /* 플랫폼 장치에 device를 platform_device로 변환 결과를 대입; NVMe 호스트 상태 갱신 */
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	err = mtk_pcie_init_irq_domain(port, node); /* 에러 상태(실패 시 NVMe 열거 중단)에 NVMe 인터럽트용 INTx 및 MSI 도메인 초기화 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to init PCIe IRQ domain\n"); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		return err; /* err 값 반환; NVMe 호스트 흐름 제어 */
	} /* 코드 블록 종료 */

	if (of_property_present(dev->of_node, "interrupt-names")) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		port->irq = platform_get_irq_byname(pdev, "pcie_irq"); /* port->irq에 이름으로 GIC IRQ 획득; NVMe 인터럽트 결과를 대입; NVMe 호스트 상태 갱신 */
	else
		port->irq = platform_get_irq(pdev, port->slot); /* port->irq에 인덱스로 GIC IRQ 획득 결과를 대입; NVMe 호스트 상태 갱신 */

	if (port->irq < 0) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return port->irq; /* port->irq 값 반환; NVMe 호스트 흐름 제어 */

	irq_set_chained_handler_and_data(port->irq, /* GIC IRQ를 PCIe demux에 연결; NVMe */
					 mtk_pcie_intr_handler, port);

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static int mtk_pcie_startup_port_v2(struct mtk_pcie_port *port) /* mtk_pcie_startup_port_v2() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = port->pcie; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie); /* PCI host bridge에 private data에서 host bridge 획득; NVMe 리소스 결과를 대입; NVMe 호스트 상태 갱신 */
	struct resource *mem = NULL; /* NVMe BAR용 MEM 리소스 윈도우를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct resource_entry *entry; /* 지역 변수 entry 선언; NVMe 호스트 동작 상태 저장 */
	const struct mtk_pcie_soc *soc = port->pcie->soc; /* SoC별 동작/퀴크 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	u32 val; /* 지역 변수 val 선언; NVMe 호스트 동작 상태 저장 */
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	entry = resource_list_first_type(&host->windows, IORESOURCE_MEM); /* 리소스 리스트 항목에 첫 MEM 리소스 윈도우 획득; NVMe BAR 매핑 결과를 대입; NVMe 호스트 상태 갱신 */
	if (entry) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		mem = entry->res; /* NVMe BAR용 MEM 리소스 윈도우를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	if (!mem) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return -EINVAL; /* -EINVAL 오류 반환; NVMe 장치 열거 중단 */

	/* MT7622 platforms need to enable LTSSM and ASPM from PCIe subsys */
	if (pcie->base) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		val = readl(pcie->base + PCIE_SYS_CFG_V2); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
		val |= PCIE_CSR_LTSSM_EN(port->slot) |
		       PCIE_CSR_ASPM_L1_EN(port->slot); /* PCIE_CSR_ASPM_L1_EN() 함수 호출; NVMe PCIe host 흐름에서 사용 */
		writel(val, pcie->base + PCIE_SYS_CFG_V2); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	} else if (pcie->cfg) { /* 추가 조건 분기 처리 */
		val = PCIE_CSR_LTSSM_EN(port->slot) |
		      PCIE_CSR_ASPM_L1_EN(port->slot); /* PCIE_CSR_ASPM_L1_EN() 함수 호출; NVMe PCIe host 흐름에서 사용 */
		regmap_update_bits(pcie->cfg, PCIE_SYS_CFG_V2, val, val); /* syscon 레지스터 비트 갱신 */
	} /* 코드 블록 종료 */

	if (!(soc->quirks & MTK_PCIE_SKIP_RSTB)) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		/* Assert all reset signals */
		writel(0, port->base + PCIE_RST_CTRL); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

		/*
		 * Enable PCIe link down reset, if link status changed from
		 * link up to link down, this will reset MAC control registers
		 * and configuration space.
		 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
		writel(PCIE_LINKDOWN_RST_EN, port->base + PCIE_RST_CTRL); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

		msleep(PCIE_T_PVPERL_MS); /* 리셋 후 NVMe 트레이닝 전 대기 */

		/* De-assert PHY, PE, PIPE, MAC and configuration reset	*/
		val = readl(port->base + PCIE_RST_CTRL); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
		val |= PCIE_PHY_RSTB | PCIE_PERSTB | PCIE_PIPE_SRSTB |
		       PCIE_MAC_SRSTB | PCIE_CRSTB;
		writel(val, port->base + PCIE_RST_CTRL); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	} /* 코드 블록 종료 */

	/* Set up vendor ID and class code */
	if (soc->quirks & MTK_PCIE_FIX_CLASS_ID) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		val = PCI_VENDOR_ID_MEDIATEK; /* 임시 레지스터 값를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
		writew(val, port->base + PCIE_CONF_VEND_ID); /* 16비트 루트 포트 PCI 헤더 필드 쓰기; NVMe가 유효한 헤더를 보도록 */

		val = PCI_CLASS_BRIDGE_PCI; /* 임시 레지스터 값를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
		writew(val, port->base + PCIE_CONF_CLASS_ID); /* 16비트 루트 포트 PCI 헤더 필드 쓰기; NVMe가 유효한 헤더를 보도록 */
	} /* 코드 블록 종료 */

	if (soc->quirks & MTK_PCIE_FIX_DEVICE_ID) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		writew(soc->device_id, port->base + PCIE_CONF_DEVICE_ID); /* 16비트 루트 포트 PCI 헤더 필드 쓰기; NVMe가 유효한 헤더를 보도록 */

	/* 100ms timeout value should be enough for Gen1/2 training */
	err = readl_poll_timeout(port->base + PCIE_LINK_STATUS_V2, val,
				 !!(val & PCIE_PORT_LINKUP_V2), 20,
				 100 * USEC_PER_MSEC);
	if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return -ETIMEDOUT; /* -ETIMEDOUT 오류 반환; NVMe 장치 열거 중단 */

	/* Set INTx mask */
	val = readl(port->base + PCIE_INT_MASK); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val &= ~INTX_MASK; /* 임시 레지스터 값를 갱신; NVMe 호스트 동작에 필요한 레지스터/상태 */
	writel(val, port->base + PCIE_INT_MASK); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	if (IS_ENABLED(CONFIG_PCI_MSI)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		mtk_pcie_enable_msi(port); /* MSI 목적지 주소 설정 및 MSI 언마스크; NVMe MSI 활성화 */

	/* Set AHB to PCIe translation windows */
	val = lower_32_bits(mem->start) |
	      AHB2PCIE_SIZE(fls(resource_size(mem))); /* AHB2PCIE_SIZE() 함수 호출; NVMe PCIe host 흐름에서 사용 */
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_L); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	val = upper_32_bits(mem->start); /* 임시 레지스터 값에 물리주소 상위 32비트 결과를 대입; NVMe 호스트 상태 갱신 */
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_H); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* Set PCIe to AXI translation memory space.*/
	val = PCIE2AHB_SIZE | WIN_ENABLE; /* 임시 레지스터 값를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	writel(val, port->base + PCIE_AXI_WINDOW0); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static void __iomem *mtk_pcie_map_bus(struct pci_bus *bus,
				      unsigned int devfn, int where)
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = bus->sysdata; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	writel(PCIE_CONF_ADDR(where, PCI_FUNC(devfn), PCI_SLOT(devfn), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
			      bus->number), pcie->base + PCIE_CFG_ADDR);

	return pcie->base + PCIE_CFG_DATA + (where & 3); /* pcie->base + PCIE_CFG_DATA + (where & 3) 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */

static struct pci_ops mtk_pcie_ops = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.map_bus = mtk_pcie_map_bus, /* 멤버 .map_bus 초기화; ECAM 스타일 config 버스 매핑; NVMe; NVMe PCIe host */
	.read  = pci_generic_config_read, /* 멤버 .read 초기화; PCI 설정 읽기 콜백; NVMe; NVMe PCIe host */
	.write = pci_generic_config_write, /* 멤버 .write 초기화; PCI 설정 쓰기 콜백; NVMe; NVMe PCIe host */
};

static int mtk_pcie_startup_port(struct mtk_pcie_port *port) /* mtk_pcie_startup_port() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = port->pcie; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	u32 func = PCI_FUNC(port->slot); /* PCI function 번호(NVMe는 주로 0)에 PCI_FUNC() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	u32 slot = PCI_SLOT(port->slot << 3); /* PCI 슬롯 번호에 PCI_SLOT() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	u32 val; /* 지역 변수 val 선언; NVMe 호스트 동작 상태 저장 */
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	/* assert port PERST_N */
	val = readl(pcie->base + PCIE_SYS_CFG); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val |= PCIE_PORT_PERST(port->slot); /* 임시 레지스터 값에 PCIE_PORT_PERST() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	writel(val, pcie->base + PCIE_SYS_CFG); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* de-assert port PERST_N */
	val = readl(pcie->base + PCIE_SYS_CFG); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val &= ~PCIE_PORT_PERST(port->slot); /* 임시 레지스터 값를 갱신; NVMe 호스트 동작에 필요한 레지스터/상태 */
	writel(val, pcie->base + PCIE_SYS_CFG); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* 100ms timeout value should be enough for Gen1/2 training */
	err = readl_poll_timeout(port->base + PCIE_LINK_STATUS, val,
				 !!(val & PCIE_PORT_LINKUP), 20,
				 100 * USEC_PER_MSEC);
	if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return -ETIMEDOUT; /* -ETIMEDOUT 오류 반환; NVMe 장치 열거 중단 */

	/* enable interrupt */
	val = readl(pcie->base + PCIE_INT_ENABLE); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val |= PCIE_PORT_INT_EN(port->slot); /* 임시 레지스터 값에 PCIE_PORT_INT_EN() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	writel(val, pcie->base + PCIE_INT_ENABLE); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* map to all DDR region. We need to set it before cfg operation. */
	writel(PCIE_BAR_MAP_MAX | PCIE_BAR_ENABLE, /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       port->base + PCIE_BAR0_SETUP);

	/* configure class code and revision ID */
	writel(PCIE_CLASS_CODE | PCIE_REVISION_ID, port->base + PCIE_CLASS); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* configure FC credit */
	writel(PCIE_CONF_ADDR(PCIE_FC_CREDIT, func, slot, 0), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       pcie->base + PCIE_CFG_ADDR);
	val = readl(pcie->base + PCIE_CFG_DATA); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val &= ~PCIE_FC_CREDIT_MASK; /* 임시 레지스터 값를 갱신; NVMe 호스트 동작에 필요한 레지스터/상태 */
	val |= PCIE_FC_CREDIT_VAL(0x806c); /* 임시 레지스터 값에 PCIE_FC_CREDIT_VAL() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	writel(PCIE_CONF_ADDR(PCIE_FC_CREDIT, func, slot, 0), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       pcie->base + PCIE_CFG_ADDR);
	writel(val, pcie->base + PCIE_CFG_DATA); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	/* configure RC FTS number to 250 when it leaves L0s */
	writel(PCIE_CONF_ADDR(PCIE_FTS_NUM, func, slot, 0), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       pcie->base + PCIE_CFG_ADDR);
	val = readl(pcie->base + PCIE_CFG_DATA); /* 임시 레지스터 값에 32비트 호스트 레지스터 읽기; NVMe 링크/인터럽트/설정 상태 관찰 결과를 대입; NVMe 호스트 상태 갱신 */
	val &= ~PCIE_FTS_NUM_MASK; /* 임시 레지스터 값를 갱신; NVMe 호스트 동작에 필요한 레지스터/상태 */
	val |= PCIE_FTS_NUM_L0(0x50); /* 임시 레지스터 값에 PCIE_FTS_NUM_L0() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	writel(PCIE_CONF_ADDR(PCIE_FTS_NUM, func, slot, 0), /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */
	       pcie->base + PCIE_CFG_ADDR);
	writel(val, pcie->base + PCIE_CFG_DATA); /* 32비트 호스트 레지스터 쓰기; 루트 포트를 NVMe 접근에 맞게 구성 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static int mtk_pcie_startup_port_an7583(struct mtk_pcie_port *port) /* mtk_pcie_startup_port_an7583() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = port->pcie; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct pci_host_bridge *host; /* 지역 변수 host 선언; NVMe 호스트 동작 상태 저장 */
	struct resource_entry *entry; /* 지역 변수 entry 선언; NVMe 호스트 동작 상태 저장 */
	struct regmap *pbus_regmap; /* 지역 변수 pbus_regmap 선언; NVMe 호스트 동작 상태 저장 */
	resource_size_t addr; /* 지역 변수 addr 선언; NVMe 호스트 동작 상태 저장 */
	u32 args[2], size; /* 지역 변수 args, size 선언; NVMe 호스트 동작 상태 저장 */

	/*
	 * Configure PBus base address and base address mask to allow
	 * the hw to detect if a given address is accessible on PCIe
	 * controller.
	 */ /* kernel-doc/설명 블록 끝; 이 객체는 NVMe PCIe host 열거에 참여 */
	pbus_regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
							   "mediatek,pbus-csr",
							   ARRAY_SIZE(args), /* ARRAY_SIZE() 함수 호출; NVMe PCIe host 흐름에서 사용 */
							   args);
	if (IS_ERR(pbus_regmap)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(pbus_regmap); /* PTR_ERR(pbus_regmap) 값 반환; NVMe 호스트 흐름 제어 */

	host = pci_host_bridge_from_priv(pcie); /* PCI host bridge에 private data에서 host bridge 획득; NVMe 리소스 결과를 대입; NVMe 호스트 상태 갱신 */
	entry = resource_list_first_type(&host->windows, IORESOURCE_MEM); /* 리소스 리스트 항목에 첫 MEM 리소스 윈도우 획득; NVMe BAR 매핑 결과를 대입; NVMe 호스트 상태 갱신 */
	if (!entry) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return -ENODEV; /* -ENODEV 오류 반환; NVMe 장치 열거 중단 */

	addr = entry->res->start - entry->offset; /* 물리 주소를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	regmap_write(pbus_regmap, args[0], lower_32_bits(addr)); /* syscon 레지스터 쓰기; NVMe 호스트 설정 */
	size = lower_32_bits(resource_size(entry->res)); /* 리소스/윈도우 크기에 물리주소 하위 32비트; NVMe DMA/MSI 주소 결과를 대입; NVMe 호스트 상태 갱신 */
	regmap_write(pbus_regmap, args[1], GENMASK(31, __fls(size))); /* syscon 레지스터 쓰기; NVMe 호스트 설정 */

	return mtk_pcie_startup_port_v2(port); /* mtk_pcie_startup_port_v2(port) 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */

static void mtk_pcie_enable_port(struct mtk_pcie_port *port) /* mtk_pcie_enable_port() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = port->pcie; /* MediaTek PCIe host private data를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	err = clk_prepare_enable(port->sys_ck); /* 에러 상태(실패 시 NVMe 열거 중단)에 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to enable sys_ck%d clock\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_sys_clk; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	err = clk_prepare_enable(port->ahb_ck); /* 에러 상태(실패 시 NVMe 열거 중단)에 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to enable ahb_ck%d\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_ahb_clk; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	err = clk_prepare_enable(port->aux_ck); /* 에러 상태(실패 시 NVMe 열거 중단)에 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to enable aux_ck%d\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_aux_clk; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	err = clk_prepare_enable(port->axi_ck); /* 에러 상태(실패 시 NVMe 열거 중단)에 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to enable axi_ck%d\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_axi_clk; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	err = clk_prepare_enable(port->obff_ck); /* 에러 상태(실패 시 NVMe 열거 중단)에 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to enable obff_ck%d\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_obff_clk; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	err = clk_prepare_enable(port->pipe_ck); /* 에러 상태(실패 시 NVMe 열거 중단)에 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to enable pipe_ck%d\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_pipe_clk; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	reset_control_assert(port->reset); /* reset_control_assert() 함수 호출; NVMe PCIe host 흐름에서 사용 */
	reset_control_deassert(port->reset); /* reset_control_deassert() 함수 호출; NVMe PCIe host 흐름에서 사용 */

	err = phy_init(port->phy); /* 에러 상태(실패 시 NVMe 열거 중단)에 phy_init() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to initialize port%d phy\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_phy_init; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	err = phy_power_on(port->phy); /* 에러 상태(실패 시 NVMe 열거 중단)에 phy_power_on() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to power on port%d phy\n", port->slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_phy_on; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	if (!pcie->soc->startup(port)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return; /* 함수 실행 종료 */

	dev_info(dev, "Port%d link down\n", port->slot); /* 정보 로그; NVMe 포트 링크 다운 등 */

	phy_power_off(port->phy); /* phy_power_off() 함수 호출; NVMe PCIe host 흐름에서 사용 */
err_phy_on:
	phy_exit(port->phy); /* phy_exit() 함수 호출; NVMe PCIe host 흐름에서 사용 */
err_phy_init:
	clk_disable_unprepare(port->pipe_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
err_pipe_clk:
	clk_disable_unprepare(port->obff_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
err_obff_clk:
	clk_disable_unprepare(port->axi_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
err_axi_clk:
	clk_disable_unprepare(port->aux_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
err_aux_clk:
	clk_disable_unprepare(port->ahb_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
err_ahb_clk:
	clk_disable_unprepare(port->sys_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
err_sys_clk:
	mtk_pcie_port_free(port); /* mtk_pcie_port_free() 함수 호출; NVMe PCIe host 흐름에서 사용 */
} /* 코드 블록 종료 */

static int mtk_pcie_parse_port(struct mtk_pcie *pcie, /* mtk_pcie_parse_port() 함수 정의; NVMe PCIe host 동작 중 호출 */
			       struct device_node *node, /* 함수 매개변수 node; NVMe PCIe 동작에 필요한 입력 */
			       int slot)
{ /* 코드 블록 시작 */
	struct mtk_pcie_port *port; /* 지역 변수 port 선언; NVMe 호스트 동작 상태 저장 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct platform_device *pdev = to_platform_device(dev); /* 플랫폼 장치에 device를 platform_device로 변환 결과를 대입; NVMe 호스트 상태 갱신 */
	char name[20];
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL); /* MediaTek PCIe 포트 데이터에 devm_kzalloc() 호출 결과를 대입; NVMe 호스트 상태 갱신 */
	if (!port) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return -ENOMEM; /* -ENOMEM 오류 반환; NVMe 장치 열거 중단 */

	snprintf(name, sizeof(name), "port%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->base = devm_platform_ioremap_resource_byname(pdev, name); /* port->base에 포트 레지스터 베이스 매핑; NVMe 설정/MMIO 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->base)) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to map port%d base\n", slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		return PTR_ERR(port->base); /* PTR_ERR(port->base) 값 반환; NVMe 호스트 흐름 제어 */
	} /* 코드 블록 종료 */

	snprintf(name, sizeof(name), "sys_ck%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->sys_ck = devm_clk_get(dev, name); /* port->sys_ck에 최상위 free 클럭 조회 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->sys_ck)) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to get sys_ck%d clock\n", slot); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		return PTR_ERR(port->sys_ck); /* PTR_ERR(port->sys_ck) 값 반환; NVMe 호스트 흐름 제어 */
	} /* 코드 블록 종료 */

	/* sys_ck might be divided into the following parts in some chips */
	snprintf(name, sizeof(name), "ahb_ck%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->ahb_ck = devm_clk_get_optional(dev, name); /* port->ahb_ck에 선택적 클럭 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->ahb_ck)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(port->ahb_ck); /* PTR_ERR(port->ahb_ck) 값 반환; NVMe 호스트 흐름 제어 */

	snprintf(name, sizeof(name), "axi_ck%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->axi_ck = devm_clk_get_optional(dev, name); /* port->axi_ck에 선택적 클럭 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->axi_ck)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(port->axi_ck); /* PTR_ERR(port->axi_ck) 값 반환; NVMe 호스트 흐름 제어 */

	snprintf(name, sizeof(name), "aux_ck%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->aux_ck = devm_clk_get_optional(dev, name); /* port->aux_ck에 선택적 클럭 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->aux_ck)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(port->aux_ck); /* PTR_ERR(port->aux_ck) 값 반환; NVMe 호스트 흐름 제어 */

	snprintf(name, sizeof(name), "obff_ck%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->obff_ck = devm_clk_get_optional(dev, name); /* port->obff_ck에 선택적 클럭 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->obff_ck)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(port->obff_ck); /* PTR_ERR(port->obff_ck) 값 반환; NVMe 호스트 흐름 제어 */

	snprintf(name, sizeof(name), "pipe_ck%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->pipe_ck = devm_clk_get_optional(dev, name); /* port->pipe_ck에 선택적 클럭 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->pipe_ck)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(port->pipe_ck); /* PTR_ERR(port->pipe_ck) 값 반환; NVMe 호스트 흐름 제어 */

	snprintf(name, sizeof(name), "pcie-rst%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->reset = devm_reset_control_get_optional_exclusive(dev, name); /* port->reset에 리셋 라인 획득; NVMe PERST 결과를 대입; NVMe 호스트 상태 갱신 */
	if (PTR_ERR(port->reset) == -EPROBE_DEFER) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(port->reset); /* PTR_ERR(port->reset) 값 반환; NVMe 호스트 흐름 제어 */

	/* some platforms may use default PHY setting */
	snprintf(name, sizeof(name), "pcie-phy%d", slot); /* 클럭/리셋/PHY 이름 조합 */
	port->phy = devm_phy_optional_get(dev, name); /* port->phy에 선택적 PHY 획득; NVMe SerDes 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(port->phy)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return PTR_ERR(port->phy); /* PTR_ERR(port->phy) 값 반환; NVMe 호스트 흐름 제어 */

	port->slot = slot; /* port->slot를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	port->pcie = pcie; /* port->pcie를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	if (pcie->soc->setup_irq) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		err = pcie->soc->setup_irq(port, node); /* 에러 상태(실패 시 NVMe 열거 중단)를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
		if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			return err; /* err 값 반환; NVMe 호스트 흐름 제어 */
	} /* 코드 블록 종료 */

	INIT_LIST_HEAD(&port->list); /* INIT_LIST_HEAD() 함수 호출; NVMe PCIe host 흐름에서 사용 */
	list_add_tail(&port->list, &pcie->ports); /* list_add_tail() 함수 호출; NVMe PCIe host 흐름에서 사용 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static int mtk_pcie_subsys_powerup(struct mtk_pcie *pcie) /* mtk_pcie_subsys_powerup() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct platform_device *pdev = to_platform_device(dev); /* 플랫폼 장치에 device를 platform_device로 변환 결과를 대입; NVMe 호스트 상태 갱신 */
	struct resource *regs; /* 지역 변수 regs 선언; NVMe 호스트 동작 상태 저장 */
	struct device_node *cfg_node; /* 지역 변수 cfg_node 선언; NVMe 호스트 동작 상태 저장 */
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	/* get shared registers, which are optional */
	regs = platform_get_resource_byname(pdev, IORESOURCE_MEM, "subsys"); /* IO 메모리 리소스에 공유 레지스터 리소스 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (regs) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		pcie->base = devm_ioremap_resource(dev, regs); /* pcie->base에 공유 레지스터 매핑 결과를 대입; NVMe 호스트 상태 갱신 */
		if (IS_ERR(pcie->base)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			return PTR_ERR(pcie->base); /* PTR_ERR(pcie->base) 값 반환; NVMe 호스트 흐름 제어 */
	} /* 코드 블록 종료 */

	cfg_node = of_find_compatible_node(NULL, NULL,
					   "mediatek,generic-pciecfg");
	if (cfg_node) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		pcie->cfg = syscon_node_to_regmap(cfg_node); /* pcie->cfg에 syscon용 regmap 획득 결과를 대입; NVMe 호스트 상태 갱신 */
		of_node_put(cfg_node); /* of_node_put() 함수 호출; NVMe PCIe host 흐름에서 사용 */
		if (IS_ERR(pcie->cfg)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			return PTR_ERR(pcie->cfg); /* PTR_ERR(pcie->cfg) 값 반환; NVMe 호스트 흐름 제어 */
	} /* 코드 블록 종료 */

	pcie->free_ck = devm_clk_get(dev, "free_ck"); /* pcie->free_ck에 최상위 free 클럭 조회 결과를 대입; NVMe 호스트 상태 갱신 */
	if (IS_ERR(pcie->free_ck)) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		if (PTR_ERR(pcie->free_ck) == -EPROBE_DEFER) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			return -EPROBE_DEFER; /* -EPROBE_DEFER 오류 반환; NVMe 장치 열거 중단 */

		pcie->free_ck = NULL; /* pcie->free_ck를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	} /* 코드 블록 종료 */

	pm_runtime_enable(dev); /* 런타임 PM 활성화; NVMe 엔드포인트 전원 연계 */
	pm_runtime_get_sync(dev); /* 런타임 PM 참조 획득; NVMe 동작 전 파워업 */

	/* enable top level clock */
	err = clk_prepare_enable(pcie->free_ck); /* 에러 상태(실패 시 NVMe 열거 중단)에 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		dev_err(dev, "failed to enable free_ck\n"); /* 에러 로그; NVMe 열거 실패 원인 기록 */
		goto err_free_ck; /* 오류 처리 레이블로 이동 */
	} /* 코드 블록 종료 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */

err_free_ck:
	pm_runtime_put_sync(dev); /* 런타임 PM 참조 해제; NVMe 호스트 절전 */
	pm_runtime_disable(dev); /* 런타임 PM 비활성화 */

	return err; /* err 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */

static int mtk_pcie_setup(struct mtk_pcie *pcie) /* mtk_pcie_setup() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct device *dev = pcie->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct device_node *node = dev->of_node; /* 디바이스 트리 노드를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct mtk_pcie_port *port, *tmp; /* 지역 변수 port, tmp 선언; NVMe 호스트 동작 상태 저장 */
	int err, slot; /* 지역 변수 err, slot 선언; NVMe 호스트 동작 상태 저장 */

	slot = of_get_pci_domain_nr(dev->of_node); /* PCI 슬롯 번호에 NVMe 루트 버스용 PCI 도메인 번호 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	if (slot < 0) { /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		for_each_available_child_of_node_scoped(node, child) { /* NVMe 관련 포트/장치/비트를 순회하는 루프 */
			err = of_pci_get_devfn(child); /* 에러 상태(실패 시 NVMe 열거 중단)에 자식 노드의 devfn 파싱; NVMe 엔드포인트 결과를 대입; NVMe 호스트 상태 갱신 */
			if (err < 0) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
				return dev_err_probe(dev, err, "failed to get devfn\n"); /* dev_err_probe(dev, err, "failed to get devfn\n") 값 반환; NVMe 호스트 흐름 제어 */

			slot = PCI_SLOT(err); /* PCI 슬롯 번호에 PCI_SLOT() 호출 결과를 대입; NVMe 호스트 상태 갱신 */

			err = mtk_pcie_parse_port(pcie, child, slot); /* 에러 상태(실패 시 NVMe 열거 중단)에 하나의 루트 포트 DT 리소스 파싱; NVMe 연결 예정 결과를 대입; NVMe 호스트 상태 갱신 */
			if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
				return err; /* err 값 반환; NVMe 호스트 흐름 제어 */
		} /* 코드 블록 종료 */
	} else {
		err = mtk_pcie_parse_port(pcie, node, slot); /* 에러 상태(실패 시 NVMe 열거 중단)에 하나의 루트 포트 DT 리소스 파싱; NVMe 연결 예정 결과를 대입; NVMe 호스트 상태 갱신 */
		if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
			return err; /* err 값 반환; NVMe 호스트 흐름 제어 */
	} /* 코드 블록 종료 */

	err = mtk_pcie_subsys_powerup(pcie); /* 에러 상태(실패 시 NVMe 열거 중단)에 공유 PCIe 서브시스템 전원업; NVMe 호스트 준비 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return err; /* err 값 반환; NVMe 호스트 흐름 제어 */

	/* enable each port, and then check link status */
	list_for_each_entry_safe(port, tmp, &pcie->ports, list) /* PCIe 포트 리스트를 순회; NVMe 엔드포인트 처리 */
		mtk_pcie_enable_port(port); /* 전원/클럭/PHY 활성화 후 NVMe 링크 트레이닝 시작 */

	/* power down PCIe subsys if slots are all empty (link down) */
	if (list_empty(&pcie->ports)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		mtk_pcie_subsys_powerdown(pcie); /* mtk_pcie_subsys_powerdown() 함수 호출; NVMe PCIe host 흐름에서 사용 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static int mtk_pcie_probe(struct platform_device *pdev) /* mtk_pcie_probe() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct device *dev = &pdev->dev; /* PCIe 플랫폼 장치 포인터를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	struct mtk_pcie *pcie; /* 지역 변수 pcie 선언; NVMe 호스트 동작 상태 저장 */
	struct pci_host_bridge *host; /* 지역 변수 host 선언; NVMe 호스트 동작 상태 저장 */
	int err; /* 지역 변수 err 선언; NVMe 호스트 동작 상태 저장 */

	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie)); /* PCI host bridge에 NVMe 버스용 PCI host bridge 구조체 할당 결과를 대입; NVMe 호스트 상태 갱신 */
	if (!host) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return -ENOMEM; /* -ENOMEM 오류 반환; NVMe 장치 열거 중단 */

	pcie = pci_host_bridge_priv(host); /* MediaTek PCIe host private data에 host bridge에서 MediaTek private data 획득 결과를 대입; NVMe 호스트 상태 갱신 */

	pcie->dev = dev; /* pcie->dev를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	pcie->soc = of_device_get_match_data(dev); /* pcie->soc에 SoC variant(V1/V2) 선택; NVMe 호스트 동작 결정 결과를 대입; NVMe 호스트 상태 갱신 */
	platform_set_drvdata(pdev, pcie); /* 드라이버 데이터 저장; 서스펜드/재개 시 NVMe 상태 */
	INIT_LIST_HEAD(&pcie->ports); /* INIT_LIST_HEAD() 함수 호출; NVMe PCIe host 흐름에서 사용 */

	err = mtk_pcie_setup(pcie); /* 에러 상태(실패 시 NVMe 열거 중단)에 루트 포트 열거 및 NVMe 링크 트레이닝 시작 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return err; /* err 값 반환; NVMe 호스트 흐름 제어 */

	host->ops = pcie->soc->ops; /* host->ops를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	host->sysdata = pcie; /* host->sysdata를 설정; NVMe 호스트 동작에 필요한 상태/주소 */
	host->msi_domain = !!(pcie->soc->quirks & MTK_PCIE_NO_MSI); /* host->msi_domain를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	err = pci_host_probe(host); /* 에러 상태(실패 시 NVMe 열거 중단)에 PCI 버스 열거 시작; NVMe 엔드포인트 발견 결과를 대입; NVMe 호스트 상태 갱신 */
	if (err) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		goto put_resources; /* 오류 처리 레이블로 이동 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */

put_resources:
	if (!list_empty(&pcie->ports)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		mtk_pcie_put_resources(pcie); /* mtk_pcie_put_resources() 함수 호출; NVMe PCIe host 흐름에서 사용 */

	return err; /* err 값 반환; NVMe 호스트 흐름 제어 */
} /* 코드 블록 종료 */


static void mtk_pcie_free_resources(struct mtk_pcie *pcie) /* mtk_pcie_free_resources() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie); /* PCI host bridge에 private data에서 host bridge 획득; NVMe 리소스 결과를 대입; NVMe 호스트 상태 갱신 */
	struct list_head *windows = &host->windows; /* struct list_head *windows를 설정; NVMe 호스트 동작에 필요한 상태/주소 */

	pci_free_resource_list(windows); /* NVMe 열거로 생성된 버스 리소스 리스트 해제 */
} /* 코드 블록 종료 */

static void mtk_pcie_remove(struct platform_device *pdev) /* mtk_pcie_remove() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = platform_get_drvdata(pdev); /* MediaTek PCIe host private data에 드라이버 데이터 복원 결과를 대입; NVMe 호스트 상태 갱신 */
	struct pci_host_bridge *host = pci_host_bridge_from_priv(pcie); /* PCI host bridge에 private data에서 host bridge 획득; NVMe 리소스 결과를 대입; NVMe 호스트 상태 갱신 */

	pci_stop_root_bus(host->bus); /* 루트 버스 정지; NVMe 장치 제거 전 */
	pci_remove_root_bus(host->bus); /* 루트 버스 제거; NVMe 장치 detach */
	mtk_pcie_free_resources(pcie); /* NVMe 제거 후 host bridge 리소스 리스트 해제 */

	mtk_pcie_irq_teardown(pcie); /* NVMe가 사용하던 IRQ 도메인/체인 핸들러 제거 */

	mtk_pcie_put_resources(pcie); /* mtk_pcie_put_resources() 함수 호출; NVMe PCIe host 흐름에서 사용 */
} /* 코드 블록 종료 */

static int mtk_pcie_suspend_noirq(struct device *dev) /* mtk_pcie_suspend_noirq() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = dev_get_drvdata(dev); /* MediaTek PCIe host private data에 PM 콜백에서 드라이버 데이터 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	struct mtk_pcie_port *port; /* 지역 변수 port 선언; NVMe 호스트 동작 상태 저장 */

	if (list_empty(&pcie->ports)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */

	list_for_each_entry(port, &pcie->ports, list) { /* PCIe 포트 리스트를 순회; NVMe 엔드포인트 처리 */
		clk_disable_unprepare(port->pipe_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->obff_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->axi_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->aux_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->ahb_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		clk_disable_unprepare(port->sys_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */
		phy_power_off(port->phy); /* phy_power_off() 함수 호출; NVMe PCIe host 흐름에서 사용 */
		phy_exit(port->phy); /* phy_exit() 함수 호출; NVMe PCIe host 흐름에서 사용 */
	} /* 코드 블록 종료 */

	clk_disable_unprepare(pcie->free_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static int mtk_pcie_resume_noirq(struct device *dev) /* mtk_pcie_resume_noirq() 함수 정의; NVMe PCIe host 동작 중 호출 */
{ /* 코드 블록 시작 */
	struct mtk_pcie *pcie = dev_get_drvdata(dev); /* MediaTek PCIe host private data에 PM 콜백에서 드라이버 데이터 획득 결과를 대입; NVMe 호스트 상태 갱신 */
	struct mtk_pcie_port *port, *tmp; /* 지역 변수 port, tmp 선언; NVMe 호스트 동작 상태 저장 */

	if (list_empty(&pcie->ports)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */

	clk_prepare_enable(pcie->free_ck); /* 클럭 도메인 활성화; NVMe PCIe 링크/MMIO에 필요 */

	list_for_each_entry_safe(port, tmp, &pcie->ports, list) /* PCIe 포트 리스트를 순회; NVMe 엔드포인트 처리 */
		mtk_pcie_enable_port(port); /* 전원/클럭/PHY 활성화 후 NVMe 링크 트레이닝 시작 */

	/* In case of EP was removed while system suspend. */
	if (list_empty(&pcie->ports)) /* 조건 검사; NVMe 호스트 동작 분기(에러/링크/IRQ 등) */
		clk_disable_unprepare(pcie->free_ck); /* 클럭 도메인 비활성화; NVMe 유휴 시 전력 절약 */

	return 0; /* 성공 반환; NVMe 열거/동작을 계속 진행 */
} /* 코드 블록 종료 */

static const struct dev_pm_ops mtk_pcie_pm_ops = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	NOIRQ_SYSTEM_SLEEP_PM_OPS(mtk_pcie_suspend_noirq, /* NOIRQ 시스템 수면 PM ops 등록 */
				  mtk_pcie_resume_noirq)
};

static const struct mtk_pcie_soc mtk_pcie_soc_v1 = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.ops = &mtk_pcie_ops, /* 멤버 .ops 초기화; pci_ops; NVMe 설정 읽기/쓰기 콜백; NVMe PCIe host */
	.startup = mtk_pcie_startup_port, /* 멤버 .startup 초기화; 포트 초기화 콜백; NVMe 링크 트레이닝; NVMe PCIe host */
	.quirks = MTK_PCIE_NO_MSI, /* 멤버 .quirks 초기화; SoC별 quirk 플래그; NVMe MSI/클리스/리셋; NVMe PCIe host */
};

static const struct mtk_pcie_soc mtk_pcie_soc_mt2712 = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.ops = &mtk_pcie_ops_v2, /* 멤버 .ops 초기화; pci_ops; NVMe 설정 읽기/쓰기 콜백; NVMe PCIe host */
	.startup = mtk_pcie_startup_port_v2, /* 멤버 .startup 초기화; 포트 초기화 콜백; NVMe 링크 트레이닝; NVMe PCIe host */
	.setup_irq = mtk_pcie_setup_irq, /* 멤버 .setup_irq 초기화; IRQ 도메인 초기화 콜백; NVMe MSI/INTx; NVMe PCIe host */
};

static const struct mtk_pcie_soc mtk_pcie_soc_mt7622 = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.ops = &mtk_pcie_ops_v2, /* 멤버 .ops 초기화; pci_ops; NVMe 설정 읽기/쓰기 콜백; NVMe PCIe host */
	.startup = mtk_pcie_startup_port_v2, /* 멤버 .startup 초기화; 포트 초기화 콜백; NVMe 링크 트레이닝; NVMe PCIe host */
	.setup_irq = mtk_pcie_setup_irq, /* 멤버 .setup_irq 초기화; IRQ 도메인 초기화 콜백; NVMe MSI/INTx; NVMe PCIe host */
	.quirks = MTK_PCIE_FIX_CLASS_ID, /* 멤버 .quirks 초기화; SoC별 quirk 플래그; NVMe MSI/클리스/리셋; NVMe PCIe host */
};

static const struct mtk_pcie_soc mtk_pcie_soc_an7583 = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.ops = &mtk_pcie_ops_v2, /* 멤버 .ops 초기화; pci_ops; NVMe 설정 읽기/쓰기 콜백; NVMe PCIe host */
	.startup = mtk_pcie_startup_port_an7583, /* 멤버 .startup 초기화; 포트 초기화 콜백; NVMe 링크 트레이닝; NVMe PCIe host */
	.setup_irq = mtk_pcie_setup_irq, /* 멤버 .setup_irq 초기화; IRQ 도메인 초기화 콜백; NVMe MSI/INTx; NVMe PCIe host */
	.quirks = MTK_PCIE_FIX_CLASS_ID | MTK_PCIE_SKIP_RSTB, /* 멤버 .quirks 초기화; SoC별 quirk 플래그; NVMe MSI/클리스/리셋; NVMe PCIe host */
};

static const struct mtk_pcie_soc mtk_pcie_soc_mt7629 = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.device_id = PCI_DEVICE_ID_MEDIATEK_7629, /* 멤버 .device_id 초기화; 고정할 루트 포트 device ID; NVMe PCIe host */
	.ops = &mtk_pcie_ops_v2, /* 멤버 .ops 초기화; pci_ops; NVMe 설정 읽기/쓰기 콜백; NVMe PCIe host */
	.startup = mtk_pcie_startup_port_v2, /* 멤버 .startup 초기화; 포트 초기화 콜백; NVMe 링크 트레이닝; NVMe PCIe host */
	.setup_irq = mtk_pcie_setup_irq, /* 멤버 .setup_irq 초기화; IRQ 도메인 초기화 콜백; NVMe MSI/INTx; NVMe PCIe host */
	.quirks = MTK_PCIE_FIX_CLASS_ID | MTK_PCIE_FIX_DEVICE_ID, /* 멤버 .quirks 초기화; SoC별 quirk 플래그; NVMe MSI/클리스/리셋; NVMe PCIe host */
};

static const struct of_device_id mtk_pcie_ids[] = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	{ .compatible = "airoha,an7583-pcie", .data = &mtk_pcie_soc_an7583 }, /* 멤버 .compatible 초기화; 호환 문자열; OF 매치로 NVMe 호스트 바인딩; NVMe PCIe host */
	{ .compatible = "mediatek,mt2701-pcie", .data = &mtk_pcie_soc_v1 }, /* 멤버 .compatible 초기화; 호환 문자열; OF 매치로 NVMe 호스트 바인딩; NVMe PCIe host */
	{ .compatible = "mediatek,mt7623-pcie", .data = &mtk_pcie_soc_v1 }, /* 멤버 .compatible 초기화; 호환 문자열; OF 매치로 NVMe 호스트 바인딩; NVMe PCIe host */
	{ .compatible = "mediatek,mt2712-pcie", .data = &mtk_pcie_soc_mt2712 }, /* 멤버 .compatible 초기화; 호환 문자열; OF 매치로 NVMe 호스트 바인딩; NVMe PCIe host */
	{ .compatible = "mediatek,mt7622-pcie", .data = &mtk_pcie_soc_mt7622 }, /* 멤버 .compatible 초기화; 호환 문자열; OF 매치로 NVMe 호스트 바인딩; NVMe PCIe host */
	{ .compatible = "mediatek,mt7629-pcie", .data = &mtk_pcie_soc_mt7629 }, /* 멤버 .compatible 초기화; 호환 문자열; OF 매치로 NVMe 호스트 바인딩; NVMe PCIe host */
	{},
};
MODULE_DEVICE_TABLE(of, mtk_pcie_ids); /* OF match table 등록; NVMe 호스트 바인딩 */

static struct platform_driver mtk_pcie_driver = { /* 전역/정적 변수 또는 구조체 정의; NVMe PCIe host 데이터/ops 초기화 */
	.probe = mtk_pcie_probe, /* 멤버 .probe 초기화; 플랫폼 드라이버 probe 콜백; NVMe 호스트 바인딩; NVMe PCIe host */
	.remove = mtk_pcie_remove, /* 멤버 .remove 초기화; 플랫폼 드라이버 remove 콜백; NVMe 장치 detach; NVMe PCIe host */
	.driver = { /* 멤버 .driver 초기화; 플랫폼 드라이버 하위 구조체; NVMe PCIe host */
		.name = "mtk-pcie", /* 멤버 .name 초기화; 드라이버/칩 이름; NVMe PCIe host */
		.of_match_table = mtk_pcie_ids, /* 멤버 .of_match_table 초기화; OF 매치 테이블; NVMe PCIe host */
		.suppress_bind_attrs = true, /* 멤버 .suppress_bind_attrs 초기화; sysfs bind 속성 억제; NVMe PCIe host */
		.pm = &mtk_pcie_pm_ops, /* 멤버 .pm 초기화; PM ops; NVMe PCIe host */
	},
};
module_platform_driver(mtk_pcie_driver); /* 플랫폼 드라이버 등록; NVMe pci 드라이버보다 먼저 로드 */
MODULE_DESCRIPTION("MediaTek PCIe host controller driver"); /* 모듈 설명 */
MODULE_LICENSE("GPL v2"); /* 모듈 라이선스 */
