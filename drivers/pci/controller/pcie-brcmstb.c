// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2009 - 2019 Broadcom */

#include <linux/bitfield.h> /* PCI/NVMe: 비트 필드 조작용; NVMe 링크/대역폭 관련 레지스터 해석에 사용 */
#include <linux/bitops.h> /* PCI/NVMe: ilog2, GENMASK 등; NVMe BAR/MSI 영역 크기 계산에 필요 */
#include <linux/clk.h> /* PCI/NVMe: PCIe 컨트롤러 클럭; NVMe 열거 전 클럭 활성화에 사용 */
#include <linux/compiler.h> /* PCI/NVMe: __iomem, likely/unlikely; NVMe DMA 경로 최적화 힌트 */
#include <linux/delay.h> /* PCI/NVMe: usleep_range, msleep; NVMe 리셋/링크 업 대기에 사용 */
#include <linux/init.h> /* PCI/NVMe: module_init/export; NVMe 호스트와 동일한 모듈 생태계 */
#include <linux/interrupt.h> /* PCI/NVMe: irq_desc, handle_edge_irq; NVMe MSI-X/MSI 인터럽트 처리 연결 */
#include <linux/io.h> /* PCI/NVMe: readl/writel; NVMe PCIe 레지스터(ECAM, CAP) 접근의 기반 */
#include <linux/iopoll.h> /* PCI/NVMe: readl_poll_timeout_atomic; NVMe MDIO/링크 안정화 폴링 */
#include <linux/ioport.h> /* PCI/NVMe: resource_type 등; NVMe 메모리 BAR 자원 관리 연결 */
#include <linux/irqchip/chained_irq.h> /* PCI/NVMe: chained_irq_enter/exit; NVMe MSI 하드웨어 IRQ 체인 처리 */
#include <linux/irqchip/irq-msi-lib.h> /* PCI/NVMe: MSI 라이브러리; NVMe MSI/MSI-X 할당/해제 공용 인프라 */
#include <linux/irqdomain.h> /* PCI/NVMe: irq_domain; NVMe 가상 IRQ -> 하드웨어 MSI 번호 매핑 */
#include <linux/kdebug.h> /* PCI/NVMe: die notifier; NVMe 장치 AER/치명 오류 시 디버깅 */
#include <linux/kernel.h> /* PCI/NVMe: 기본 커널 매크로; NVMe 드라이버와 공유 */
#include <linux/list.h> /* PCI/NVMe: resource_entry 리스트; NVMe dma-ranges/windows 탐색 */
#include <linux/log2.h> /* PCI/NVMe: order_base_2; NVMe MSI IRQ 개수(log2) 할당에 사용 */
#include <linux/module.h> /* PCI/NVMe: module_platform_driver; NVMe와 함께 로드되는 PCIe RC 드라이버 */
#include <linux/msi.h> /* PCI/NVMe: MSI_FLAG_*; NVMe MSI/MSI-X 기능 협상의 플래그 정의 */
#include <linux/notifier.h> /* PCI/NVMe: notifier_block; NVMe panic/die 시 PCIe 오류 덤프 */
#include <linux/of_address.h> /* PCI/NVMe: OF 주소 파싱; NVMe 디바이스 트리 리소스 매핑 */
#include <linux/of_irq.h> /* PCI/NVMe: irq_of_parse_and_map; NVMe MSI 하드웨어 IRQ 번호 획득 */
#include <linux/of_pci.h> /* PCI/NVMe: of_pci_get_max_link_speed; NVMe PCIe 속도 제한 파싱 */
#include <linux/of_platform.h> /* PCI/NVMe: platform_driver 등록; NVMe 열거 이전 PCIe RC 초기화 */
#include <linux/panic_notifier.h> /* PCI/NVMe: panic_notifier_list; NVMe 시스템 패닉 시 오류 기록 */
#include <linux/pci.h> /* PCI/NVMe: 핵심 PCI API; NVMe 호스트 드라이버가 직접 사용하는 구조체/함수 */
#include <linux/pci-ecam.h> /* PCI/NVMe: PCI Enhanced Config Access; NVMe config space 접근 */
#include <linux/printk.h> /* PCI/NVMe: dev_err/dev_info; NVMe 초기화/에러 메시지 출력 */
#include <linux/regulator/consumer.h> /* PCI/NVMe: 레귤레이터; NVMe 장치 전원 공급 제어 */
#include <linux/reset.h> /* PCI/NVMe: reset_control; NVMe 컨트롤러 PERST# 및 bridge 리셋 */
#include <linux/sizes.h> /* PCI/NVMe: SZ_1M, SZ_4G 등; NVMe BAR/DMA 범위 크기 상수 */
#include <linux/slab.h> /* PCI/NVMe: devm_kzalloc; NVMe MSI/남부 데이터 할당 */
#include <linux/spinlock.h> /* PCI/NVMe: spinlock; NVMe 인터럽트/bridge lock 보호 */
#include <linux/string.h> /* PCI/NVMe: strcmp 등; NVMe DT 문자열 속성 파싱 */
#include <linux/string_choices.h> /* PCI/NVMe: str_read_write; NVMe PCIe 오류 방향 출력 */
#include <linux/types.h> /* PCI/NVMe: u32/u64 등; NVMe 레지스터 타입 */

#include "../pci.h" /* PCI/NVMe: PCI 코어 남부 헤더; NVMe 호스트가 의존하는 남부 정의 */

/* BRCM_PCIE_CAP_REGS - Offset for the mandatory capability config regs */
#define BRCM_PCIE_CAP_REGS				0x00ac /* PCI/NVMe: PCIe capability 레지스터 기준 오프셋; NVMe LINKCTL2/LINKSTA 접근 기준 */

/* Broadcom STB PCIe Register Offsets */
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1				0x0188 /* PCI/NVMe: Vendor Specific Reg1; NVMe inbound BAR endian 설정 */
#define  PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK	0xc /* PCI/NVMe: BAR2 endian 모드 마스크; NVMe DMA 메모리 일관성에 영향 */
#define  PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN			0x0 /* PCI/NVMe: little endian; NVMe CPU 메모리와 일치하는 바이트 오더 */

#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043c /* PCI/NVMe: RC class code 설정 레지스터; NVMe가 인식하는 bridge 타입 결정 */
#define  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK	0xffffff /* PCI/NVMe: class code 마스크; NVMe 열거 시 0x060400 bridge로 보임 */

#define PCIE_RC_CFG_PRIV1_LINK_CAPABILITY			0x04dc /* PCI/NVMe: 링크 능력 레지스터; NVMe x4/x1 및 PCIe 세대/ASPM 협상 */
#define  PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK	0x1f0 /* PCI/NVMe: 최대 링크 폭 마스크; NVMe 대역폭 직접 결정 */

#define PCIE_RC_CFG_PRIV1_ROOT_CAP			0x4f8 /* PCI/NVMe: Root Capability; NVMe ASPM L1SS 서브스테이트 광고 */
#define  PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK	0xf8 /* PCI/NVMe: L1SS 모드 마스크; NVMe 저전력 상태 지원 범위 제어 */

#define PCIE_RC_DL_MDIO_ADDR				0x1100 /* PCI/NVMe: MDIO 주소 레지스터; NVMe PHY/SSC 설정 포트 */
#define PCIE_RC_DL_MDIO_WR_DATA				0x1104 /* PCI/NVMe: MDIO 쓰기 데이터; NVMe PHY 튜닝 값 기록 */
#define PCIE_RC_DL_MDIO_RD_DATA				0x1108 /* PCI/NVMe: MDIO 읽기 데이터; NVMe PHY 상태 확인 */

#define PCIE_RC_PL_REG_PHY_CTL_1			0x1804 /* PCI/NVMe: PHY 제어 레지스터 1; NVMe 링크 전력/속도 관련 */
#define  PCIE_RC_PL_REG_PHY_CTL_1_REG_P2_POWERDOWN_ENA_NOSYNC_MASK	0x8 /* PCI/NVMe: P2 powerdown 설정; NVMe 링크 폭 변경 시 전력 영향 */

#define PCIE_RC_PL_PHY_CTL_15				0x184c /* PCI/NVMe: PHY 제어 레지스터 15; NVMe L1SS 타이머/클록 주기 */
#define  PCIE_RC_PL_PHY_CTL_15_DIS_PLL_PD_MASK		0x400000 /* PCI/NVMe: PLL powerdown 비활성; NVMe 저전력 링크 복구 안정성 */
#define  PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK	0xff /* PCI/NVMe: PM 클록 주기 마스크; NVMe L1SS 타이밍 계산 */

#define PCIE_MISC_MISC_CTRL				0x4008 /* PCI/NVMe: PCIe RC 핵심 제어; NVMe MPS/64bit/burst 설정 */
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE_MASK	0x80 /* PCI/NVMe: RC 64bit 모드; NVMe 64비트 메모리 접근 지원 */
#define  PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE_MASK	0x400 /* PCI/NVMe: RC MPS 모드; NVMe Max Payload Size 협상 기준 */
#define  PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK		0x1000 /* PCI/NVMe: SCB 접근 활성화; NVMe DMA inbound 경로 개방 */
#define  PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK	0x2000 /* PCI/NVMe: CFG Read UR 모드; NVMe 미연결 포트 조회 시 버스 어보트 방지 */
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK	0x300000 /* PCI/NVMe: 최대 burst 크기; NVMe DMA 처리량 조절 */

#define  PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK		0xf8000000 /* PCI/NVMe: SCB0 크기; NVMe memc0 DMA 뷰포트 크기 */
#define  PCIE_MISC_MISC_CTRL_SCB1_SIZE_MASK		0x07c00000 /* PCI/NVMe: SCB1 크기; NVMe memc1 DMA 뷰포트 크기 */
#define  PCIE_MISC_MISC_CTRL_SCB2_SIZE_MASK		0x0000001f /* PCI/NVMe: SCB2 크기; NVMe memc2 DMA 뷰포트 크기 */
#define  SCB_SIZE_MASK(x) PCIE_MISC_MISC_CTRL_SCB ## x ## _SIZE_MASK /* PCI/NVMe: SCB 크기 마스크 매크로; NVMe memc별 DMA 창 설정 */

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400c /* PCI/NVMe: outbound window0 하위 주소; NVMe 메모리 BAR 접근 목적지 */
#define PCIE_MEM_WIN0_LO(win)	\
			PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + ((win) * 8) /* PCI/NVMe: win번 outbound 하위 주소; NVMe DMA/메모리 매핑 시 CPU->PCIe 변환 */

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010 /* PCI/NVMe: outbound window0 상위 주소; 64비트 NVMe 주소 확장 */
#define PCIE_MEM_WIN0_HI(win)	\
			PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI + ((win) * 8) /* PCI/NVMe: win번 outbound 상위 주소; 64비트 NVMe BAR 매핑에 사용 */

/*
 * NOTE: You may see the term "BAR" in a number of register names used by
 *   this driver.  The term is an artifact of when the HW core was an
 *   endpoint device (EP).  Now it is a root complex (RC) and anywhere a
 *   register has the term "BAR" it is related to an inbound window.
 */

#define PCIE_BRCM_MAX_INBOUND_WINS			16 /* PCI/NVMe: 최대 inbound window 수; NVMe DMA 주소 변환 테이블 크기 */
#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402c /* PCI/NVMe: RC inbound BAR1 low; NVMe 시스템 메모리 뷰포트 설정 */
#define  PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK		0x1f /* PCI/NVMe: BAR1 크기 인코딩 마스크; NVMe DMA 영역 크기 */

#define PCIE_MISC_RC_BAR4_CONFIG_LO			0x40d4 /* PCI/NVMe: RC inbound BAR4 low; NVMe 추가 DMA 뷰포트 */


#define PCIE_MISC_MSI_BAR_CONFIG_LO			0x4044 /* PCI/NVMe: MSI BAR 하위 설정; NVMe MSI write가 도달할 주소 */
#define PCIE_MISC_MSI_BAR_CONFIG_HI			0x4048 /* PCI/NVMe: MSI BAR 상위 설정; NVMe 64비트 MSI 목적지 */

#define PCIE_MISC_MSI_DATA_CONFIG			0x404c /* PCI/NVMe: MSI 데이터 패턴; NVMe MSI 벡터 번호 생성 공식 */
#define  PCIE_MISC_MSI_DATA_CONFIG_VAL_32		0xffe06540 /* PCI/NVMe: 32비트 MSI 데이터 기본값; NVMe MSI 벡터 패턴 */
#define  PCIE_MISC_MSI_DATA_CONFIG_VAL_8		0xfff86540 /* PCI/NVMe: legacy 8비트 MSI 데이터; 레거시 NVMe MSI 패턴 */

#define PCIE_MISC_PCIE_CTRL				0x4064 /* PCI/NVMe: PCIe 제어; NVMe L23 요청/PERST# 제어 */
#define  PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK	0x1 /* PCI/NVMe: L23 저전력 요청; NVMe suspend 시 링크 저전력 진입 */
#define PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK		0x4 /* PCI/NVMe: PERST# 비트; NVMe 컨트롤러 fundamental 리셋 */

#define PCIE_MISC_PCIE_STATUS				0x4068 /* PCI/NVMe: PCIe 상태; NVMe 링크 업/다운 및 L23 상태 확인 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK		0x80 /* PCI/NVMe: RC/EP 모드 표시; NVMe 반드시 RC 모드여야 함 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK	0x20 /* PCI/NVMe: Data Link Layer active; NVMe config/Tlp 통신 가능 조건 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK	0x10 /* PCI/NVMe: PHY link up; NVMe 물리적 연결 상태 */
#define  PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK	0x40 /* PCI/NVMe: L23 상태 진입; NVMe 저전력 상태 확인 */

#define PCIE_MISC_REVISION				0x406c /* PCI/NVMe: 하드웨어 리비전; NVMe MSI 레거시 모드 판별 기준 */
#define  BRCM_PCIE_HW_REV_33				0x0303 /* PCI/NVMe: rev 3.3; NVMe 전용 MSI 레지스터 사용 기준 */
#define  BRCM_PCIE_HW_REV_3_20				0x0320 /* PCI/NVMe: rev 3.20; NVMe PERST# 설정 불가 리비전 */

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT		0x4070 /* PCI/NVMe: outbound base/limit; NVMe 메모리 창 범위 */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK	0xfff00000 /* PCI/NVMe: limit 마스크; NVMe outbound 상한(M 단위) */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK	0xfff0 /* PCI/NVMe: base 마스크; NVMe outbound 하한(M 단위) */
#define PCIE_MEM_WIN0_BASE_LIMIT(win)	\
			PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + ((win) * 4) /* PCI/NVMe: win번 base/limit; NVMe BAR 매핑 범위 */

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI			0x4080 /* PCI/NVMe: outbound base 상위; NVMe 64비트 주소 */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK	0xff /* PCI/NVMe: base 상위 마스크; NVMe 64비트 메모리 공간 */
#define PCIE_MEM_WIN0_BASE_HI(win)	\
			PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + ((win) * 8) /* PCI/NVMe: win번 base 상위; NVMe 64비트 BAR */

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI			0x4084 /* PCI/NVMe: outbound limit 상위; NVMe 64비트 상한 */
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK	0xff /* PCI/NVMe: limit 상위 마스크; NVMe 64비트 메모리 공간 */
#define PCIE_MEM_WIN0_LIMIT_HI(win)	\
			PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI + ((win) * 8) /* PCI/NVMe: win번 limit 상위; NVMe 64비트 BAR */

#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK	0x2 /* PCI/NVMe: CLKREQ debug; NVMe ASPM L0s/L1 설정 연관 */
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK		0x200000 /* PCI/NVMe: L1SS 활성화; NVMe 저전력 서브스테이트 허용 */
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK		0x08000000 /* PCI/NVMe: SerDes IDDQ; NVMe PHY 전력 차단 */
#define  PCIE_BMIPS_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK		0x00800000 /* PCI/NVMe: bmips SerDes IDDQ; NVMe PHY 전력 차단(레거시) */
#define  PCIE_CLKREQ_MASK \
		  (PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK | \
		   PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK) /* PCI/NVMe: CLKREQ 관련 마스크; NVMe ASPM L1SS/L0s 제어 */

#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP			0x40ac /* PCI/NVMe: UBUS BAR1 remap; NVMe DMA 주소->시스템 메모리 remap(BCM7712) */
#define  PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_EN_MASK	BIT(0) /* PCI/NVMe: BAR1 접근 활성화; NVMe inbound DMA 허용 */
#define PCIE_MISC_UBUS_BAR4_CONFIG_REMAP			0x410c /* PCI/NVMe: UBUS BAR4 remap; NVMe 추가 DMA remap */

#define PCIE_MSI_INTR2_BASE		0x4500 /* PCI/NVMe: 전용 MSI 인터럽트 레지스터 베이스; NVMe MSI 상태/마스크/클리어 접근 */

/* Offsets from INTR2_CPU and MSI_INTR2 BASE offsets */
#define  MSI_INT_STATUS			0x0 /* PCI/NVMe: MSI pending 상태; NVMe MSI 도착 여부 확인 */
#define  MSI_INT_CLR			0x8 /* PCI/NVMe: MSI 클리어; NVMe MSI ack 처리 */
#define  MSI_INT_MASK_SET		0x10 /* PCI/NVMe: MSI 마스크 설정; NVMe 특정 MSI 벡터 비활성화 */
#define  MSI_INT_MASK_CLR		0x14 /* PCI/NVMe: MSI 마스크 해제; NVMe MSI 벡터 활성화 */

/* Error report registers */
#define PCIE_OUTB_ERR_TREAT				0x6000 /* PCI/NVMe: outbound 오류 처리 방식; NVMe config/mem 오류 대응 */
#define  PCIE_OUTB_ERR_TREAT_CONFIG			0x1 /* PCI/NVMe: config 오류 처리; NVMe PCIe config 접근 오류 보고 */
#define  PCIE_OUTB_ERR_TREAT_MEM			0x2 /* PCI/NVMe: mem 오류 처리; NVMe DMA/메모리 접근 오류 보고 */
#define PCIE_OUTB_ERR_VALID				0x6004 /* PCI/NVMe: outbound 오류 유효; NVMe AER/오류 이벤트 발생 표시 */
#define PCIE_OUTB_ERR_CLEAR				0x6008 /* PCI/NVMe: outbound 오류 클리어; NVMe 오류 레지스터 리셋 */
#define PCIE_OUTB_ERR_ACC_INFO				0x600c /* PCI/NVMe: 오류 접근 정보; NVMe 오류 유형/방향/바이트레인 */
#define  PCIE_OUTB_ERR_ACC_INFO_CFG_ERR			BIT(0) /* PCI/NVMe: config 오류 플래그; NVMe PCIe 설정 읽기/쓰기 실패 */
#define  PCIE_OUTB_ERR_ACC_INFO_MEM_ERR			BIT(1) /* PCI/NVMe: mem 오류 플래그; NVMe DMA 메모리 사이클 실패 */
#define  PCIE_OUTB_ERR_ACC_INFO_TYPE_64			BIT(2) /* PCI/NVMe: 64비트 접근; NVMe 64비트 TLP 오류 식별 */
#define  PCIE_OUTB_ERR_ACC_INFO_DIR_WRITE		BIT(4) /* PCI/NVMe: 쓰기 방향; NVMe DMA write/read 구분 */
#define  PCIE_OUTB_ERR_ACC_INFO_BYTE_LANES		0xff00 /* PCI/NVMe: 바이트 레인; NVMe TLP BE 표시 */
#define PCIE_OUTB_ERR_ACC_ADDR				0x6010 /* PCI/NVMe: 오류 config 주소; NVMe BDF/레지스터 추적 */
#define PCIE_OUTB_ERR_ACC_ADDR_BUS			0xff00000 /* PCI/NVMe: 오류 버스 번호; NVMe 장치 버스 식별 */
#define PCIE_OUTB_ERR_ACC_ADDR_DEV			0xf8000 /* PCI/NVMe: 오류 디바이스 번호; NVMe 장치 번호 식별 */
#define PCIE_OUTB_ERR_ACC_ADDR_FUNC			0x7000 /* PCI/NVMe: 오류 기능 번호; NVMe 기능 번호 식별 */
#define PCIE_OUTB_ERR_ACC_ADDR_REG			0xfff /* PCI/NVMe: 오류 레지스터 오프셋; NVMe PCIe capability offset */
#define PCIE_OUTB_ERR_CFG_CAUSE				0x6014 /* PCI/NVMe: config 오류 원인; NVMe 접근 실패 상세 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_TIMEOUT		BIT(6) /* PCI/NVMe: config timeout; NVMe 미응답 config 접근 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ABORT			BIT(5) /* PCI/NVMe: config abort; NVMe PCIe completer abort */
#define  PCIE_OUTB_ERR_CFG_CAUSE_UNSUPP_REQ		BIT(4) /* PCI/NVMe: unsupported request; NVMe 잘못된 config 요청 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ACC_TIMEOUT		BIT(2) /* PCI/NVMe: 접근 timeout; NVMe config 사이클 timeout */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ACC_DISABLED		BIT(1) /* PCI/NVMe: 접근 비활성; NVMe 비활성 포트 접근 */
#define  PCIE_OUTB_ERR_CFG_CAUSE_ACC_64BIT		BIT(0) /* PCI/NVMe: 64비트 config 접근; NVMe 64비트 config 오류 */
#define PCIE_OUTB_ERR_MEM_ADDR_LO			0x6018 /* PCI/NVMe: mem 오류 주소 low; NVMe DMA 실패 주소 */
#define PCIE_OUTB_ERR_MEM_ADDR_HI			0x601c /* PCI/NVMe: mem 오류 주소 high; NVMe 64비트 DMA 실패 주소 */
#define PCIE_OUTB_ERR_MEM_CAUSE				0x6020 /* PCI/NVMe: mem 오류 원인; NVMe DMA 오류 상세 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_TIMEOUT		BIT(6) /* PCI/NVMe: mem timeout; NVMe DMA completion timeout */
#define  PCIE_OUTB_ERR_MEM_CAUSE_ABORT			BIT(5) /* PCI/NVMe: mem abort; NVMe completer abort */
#define  PCIE_OUTB_ERR_MEM_CAUSE_UNSUPP_REQ		BIT(4) /* PCI/NVMe: mem unsupported request; NVMe 잘못된 메모리 요청 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_ACC_DISABLED		BIT(1) /* PCI/NVMe: mem 접근 비활성; NVMe 비활성 메모리 영역 */
#define  PCIE_OUTB_ERR_MEM_CAUSE_BAD_ADDR		BIT(0) /* PCI/NVMe: mem bad address; NVMe DMA 정렬/범위 오류 */

#define  PCIE_RGR1_SW_INIT_1_PERST_MASK			0x1 /* PCI/NVMe: PERST# 소프트웨어 초기화 마스크; NVMe fundamental 리셋 비트 */

#define RGR1_SW_INIT_1_INIT_GENERIC_MASK		0x2 /* PCI/NVMe: generic bridge init 마스크; NVMe PCIe bridge 리셋 */
#define RGR1_SW_INIT_1_INIT_GENERIC_SHIFT		0x1 /* PCI/NVMe: generic bridge init 시프트; NVMe bridge 리셋 위치 */
#define RGR1_SW_INIT_1_INIT_7278_MASK			0x1 /* PCI/NVMe: 7278 bridge init 마스크; NVMe bridge 리셋(7278) */
#define RGR1_SW_INIT_1_INIT_7278_SHIFT			0x0 /* PCI/NVMe: 7278 bridge init 시프트; NVMe bridge 리셋 위치(7278) */

/* PCIe parameters */
#define BRCM_NUM_PCIE_OUT_WINS		0x4 /* PCI/NVMe: 최대 outbound window 수; NVMe 메모리 BAR 매핑 제한 */
#define BRCM_INT_PCI_MSI_NR		32 /* PCI/NVMe: 최대 MSI 벡터 수; NVMe MSI-X보다 적음, MSI 한도 */
#define BRCM_INT_PCI_MSI_LEGACY_NR	8 /* PCI/NVMe: 레거시 MSI 벡터 수; 구형 NVMe 컨트롤러 MSI 한도 */
#define BRCM_INT_PCI_MSI_SHIFT		0 /* PCI/NVMe: MSI 벡터 시프트; NVMe MSI 인덱스 기준 */
#define BRCM_INT_PCI_MSI_MASK		GENMASK(BRCM_INT_PCI_MSI_NR - 1, 0) /* PCI/NVMe: 32개 MSI 마스크; NVMe MSI pending/enable 비트 */
#define BRCM_INT_PCI_MSI_LEGACY_MASK	GENMASK(31, \
						32 - BRCM_INT_PCI_MSI_LEGACY_NR) /* PCI/NVMe: 레거시 8개 MSI 마스크; 상위 8비트 사용 */

/* MSI target addresses */
#define BRCM_MSI_TARGET_ADDR_LT_4GB	0x0fffffffcULL /* PCI/NVMe: 4GB 미만 MSI 목적지; NVMe 32비트 MSI 요구 컨트롤러용 */
#define BRCM_MSI_TARGET_ADDR_GT_4GB	0xffffffffcULL /* PCI/NVMe: 4GB 초과 MSI 목적지; 64비트 MSI 지원 NVMe용 */

/* MDIO registers */
#define MDIO_PORT0			0x0 /* PCI/NVMe: MDIO 포트 0; NVMe PHY/SSC 레지스터 접근 포트 */
#define MDIO_DATA_MASK			0x7fffffff /* PCI/NVMe: MDIO 데이터 마스크; NVMe PHY 값 추출 */
#define MDIO_PORT_MASK			0xf0000 /* PCI/NVMe: MDIO 포트 마스크; NVMe PHY 포트 번호 */
#define MDIO_PORT_EXT_MASK		0x200000 /* PCI/NVMe: MDIO 확장 포트; NVMe PHY 포트 확장 비트 */
#define MDIO_REGAD_MASK			0xffff /* PCI/NVMe: MDIO 레지스터 주소 마스크; NVMe PHY 레지스터 선택 */
#define MDIO_CMD_MASK			0x00100000 /* PCI/NVMe: MDIO 명령 마스크; NVMe PHY 읽기/쓰기 선택 */
#define MDIO_CMD_READ			0x1 /* PCI/NVMe: MDIO 읽기 명령; NVMe PHY 상태 읽기 */
#define MDIO_CMD_WRITE			0x0 /* PCI/NVMe: MDIO 쓰기 명령; NVMe PHY 값 쓰기 */
#define MDIO_DATA_DONE_MASK		0x80000000 /* PCI/NVMe: MDIO 완료 비트; NVMe PHY 트랜잭션 종료 확인 */
#define MDIO_RD_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 1 : 0) /* PCI/NVMe: MDIO 읽기 완료 검사; NVMe PHY 폴링 조건 */
#define MDIO_WT_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 0 : 1) /* PCI/NVMe: MDIO 쓰기 완료 검사; NVMe PHY 폴링 조건 */
#define SSC_REGS_ADDR			0x1100 /* PCI/NVMe: SSC 레지스터 기본 주소; NVMe spread spectrum clock 설정 */
#define SET_ADDR_OFFSET			0x1f /* PCI/NVMe: SSC 주소 설정 오프셋; NVMe SSC 레지스터 뱅크 선택 */
#define SSC_CNTL_OFFSET			0x2 /* PCI/NVMe: SSC 제어 오프셋; NVMe SSC 활성화 */
#define SSC_CNTL_OVRD_EN_MASK		0x8000 /* PCI/NVMe: SSC override enable; NVMe SSC 수동 제어 */
#define SSC_CNTL_OVRD_VAL_MASK		0x4000 /* PCI/NVMe: SSC override value; NVMe SSC on/off 값 */
#define SSC_STATUS_OFFSET		0x1 /* PCI/NVMe: SSC 상태 오프셋; NVMe SSC 잠금 확인 */
#define SSC_STATUS_SSC_MASK		0x400 /* PCI/NVMe: SSC 동작 상태; NVMe SSC 활성 여부 */
#define SSC_STATUS_PLL_LOCK_MASK	0x800 /* PCI/NVMe: PLL lock 상태; NVMe 링크 클록 안정화 */
#define PCIE_BRCM_MAX_MEMC		3 /* PCI/NVMe: 최대 메모리 컨트롤러 수; NVMe DMA 뷰포트 memc 한도 */

#define IDX_ADDR(pcie)			((pcie)->cfg->offsets[EXT_CFG_INDEX]) /* PCI/NVMe: 외부 config 인덱스 레지스터 오프셋; NVMe PCIe BDF 선택 */
#define DATA_ADDR(pcie)			((pcie)->cfg->offsets[EXT_CFG_DATA]) /* PCI/NVMe: 외부 config 데이터 레지스터 오프셋; NVMe PCIe config read/write */
#define PCIE_RGR1_SW_INIT_1(pcie)	((pcie)->cfg->offsets[RGR1_SW_INIT_1]) /* PCI/NVMe: bridge/PERST 제어 오프셋; NVMe fundamental 리셋 */
#define HARD_DEBUG(pcie)		((pcie)->cfg->offsets[PCIE_HARD_DEBUG]) /* PCI/NVMe: hard debug 오프셋; NVMe SerDes/CLKREQ/L1SS 제어 */
#define INTR2_CPU_BASE(pcie)		((pcie)->cfg->offsets[PCIE_INTR2_CPU_BASE]) /* PCI/NVMe: CPU INTR2 베이스; NVMe 레거시 MSI 공유 레지스터 */

/* Rescal registers */
#define PCIE_DVT_PMU_PCIE_PHY_CTRL				0xc700 /* PCI/NVMe: RESCAL PHY 제어; NVMe PHY 전원/리셋 시퀀스 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS			0x3 /* PCI/NVMe: DAST 필드 수; NVMe PHY 제어 단계 개수 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_MASK		0x4 /* PCI/NVMe: 디지털 리셋 마스크; NVMe PHY 디지털부 초기화 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_SHIFT	0x2 /* PCI/NVMe: 디지털 리셋 시프트; NVMe PHY 디지털 리셋 위치 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_MASK		0x2 /* PCI/NVMe: 아날로그 리셋 마스크; NVMe PHY 아날로그부 초기화 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_SHIFT		0x1 /* PCI/NVMe: 아날로그 리셋 시프트; NVMe PHY 아날로그 리셋 위치 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_MASK		0x1 /* PCI/NVMe: PHY powerdown 마스크; NVMe PHY 전원 차단 */
#define  PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_SHIFT		0x0 /* PCI/NVMe: PHY powerdown 시프트; NVMe PHY 전원 제어 위치 */

/* Forward declarations */
struct brcm_pcie; /* PCI/NVMe: Broadcom PCIe RC 드라이버 사설 데이터; NVMe 열거의 host bridge sysdata */

enum {
	RGR1_SW_INIT_1, /* PCI/NVMe: bridge/PERST 제어 오프셋 인덱스; NVMe 리셋 제어 */
	EXT_CFG_INDEX, /* PCI/NVMe: 외부 config 인덱스 오프셋 인덱스; NVMe BDF 선택 */
	EXT_CFG_DATA, /* PCI/NVMe: 외부 config 데이터 오프셋 인덱스; NVMe config 접근 */
	PCIE_HARD_DEBUG, /* PCI/NVMe: hard debug 오프셋 인덱스; NVMe SerDes/CLKREQ */
	PCIE_INTR2_CPU_BASE, /* PCI/NVMe: CPU INTR2 베이스 인덱스; NVMe 레거시 MSI 공유 */
};

enum pcie_soc_base {
	GENERIC, /* PCI/NVMe: generic SoC; NVMe 기본 동작 */
	BCM2711, /* PCI/NVMe: Raspberry Pi 4 SoC; NVMe 링크 폭/3GB 제약 */
	BCM4908, /* PCI/NVMe: BCM4908; NVMe PERST# 리셋 컨트롤러 사용 */
	BCM7278, /* PCI/NVMe: BCM7278; NVMe bridge init 시퀀스 상이 */
	BCM7425, /* PCI/NVMe: BCM7425; NVMe 32비트 config 접근 전용 */
	BCM7435, /* PCI/NVMe: BCM7435; NVMe bmips 특수 동작 */
	BCM7712, /* PCI/NVMe: BCM7712; NVMe 다수 inbound window/UBUS remap */
};

struct inbound_win {
	u64 size; /* PCI/NVMe: inbound window 크기; NVMe DMA 변환 영역 크기 */
	u64 pci_offset; /* PCI/NVMe: PCIe 측 시작 오프셋; NVMe가 보는 bus 주소 */
	u64 cpu_addr; /* PCI/NVMe: CPU 측 시작 주소; NVMe DMA 물리 주소 매핑 */
};

/*
 * The RESCAL block is tied to PCIe controller #1, regardless of the number of
 * controllers, and turning off PCIe controller #1 prevents access to the RESCAL
 * register blocks, therefore no other controller can access this register
 * space, and depending upon the bus fabric we may get a timeout (UBUS/GISB),
 * or a hang (AXI).
 */
#define CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN		BIT(0) /* PCI/NVMe: bridge shutdown 회피 퀴크; NVMe 멀티 컨트롤러 시 RESCAL 충돌 방지 */

struct pcie_cfg_data {
	const int *offsets; /* PCI/NVMe: 레지스터 오프셋 테이블; NVMe SoC별 레지스터 위치 */
	const enum pcie_soc_base soc_base; /* PCI/NVMe: SoC 기반; NVMe SoC별 특수 처리 분기 */
	const bool has_phy; /* PCI/NVMe: PHY 제어 필요 여부; NVMe PHY 전원 시퀀스 수행 */
	const u32 quirks; /* PCI/NVMe: SoC별 퀴크; NVMe 초기화/종료 분기 */
	u8 num_inbound_wins; /* PCI/NVMe: inbound window 개수; NVMe DMA 뷰포트 수 */
	int (*perst_set)(struct brcm_pcie *pcie, u32 val); /* PCI/NVMe: PERST# 설정 콜백; NVMe fundamental 리셋 제어 */
	int (*bridge_sw_init_set)(struct brcm_pcie *pcie, u32 val); /* PCI/NVMe: bridge sw init 콜백; NVMe PCIe bridge 리셋 제어 */
	int (*post_setup)(struct brcm_pcie *pcie); /* PCI/NVMe: 후처리 콜백; NVMe SoC별 링크 튜닝 */
	bool has_err_report; /* PCI/NVMe: outbound 오류 보고 지원; NVMe AER/오류 덤프 */
};

struct subdev_regulators {
	unsigned int num_supplies; /* PCI/NVMe: 전원 공급 개수; NVMe 슬롯/하위 장치 전원 */
	struct regulator_bulk_data supplies[]; /* PCI/NVMe: 전원 배열; NVMe 장치 활성/비활성 */
};

struct brcm_msi {
	struct device		*dev; /* PCI/NVMe: PCI device; NVMe MSI 할당 시 로그/에러용 */
	void __iomem		*base; /* PCI/NVMe: PCIe RC 레지스터 베이스; NVMe MSI 설정 레지스터 접근 */
	struct device_node	*np; /* PCI/NVMe: device-node; NVMe MSI 도메인 fwnode */
	struct irq_domain	*inner_domain; /* PCI/NVMe: 남부 MSI irq domain; NVMe MSI virq<->hwirq 매핑 */
	struct mutex		lock; /* guards the alloc/free operations */ /* PCI/NVMe: MSI 할당/해제 보호; NVMe 멀티 큐 MSI 동시성 */
	u64			target_addr; /* PCI/NVMe: MSI 목적지 주소; NVMe 컨트롤러가 쓸 address */
	int			irq; /* PCI/NVMe: MSI 하드웨어 IRQ; NVMe MSI cascade parent IRQ */
	DECLARE_BITMAP(used, BRCM_INT_PCI_MSI_NR); /* PCI/NVMe: MSI 사용 비트맵; NVMe MSI 벡터 할당 현황 */
	bool			legacy; /* PCI/NVMe: 레거시 MSI 모드; 구형 NVMe MSI 레지스터 공유 여부 */
	/* Some chips have MSIs in bits [31..24] of a shared register. */
	int			legacy_shift; /* PCI/NVMe: 레거시 MSI 비트 시프트; NVMe MSI 벡터 위치 보정 */
	int			nr; /* No. of MSI available, depends on chip */ /* PCI/NVMe: 사용 가능 MSI 수; NVMe 요청 큐 개수 제한 */
	/* This is the base pointer for interrupt status/set/clr regs */
	void __iomem		*intr_base; /* PCI/NVMe: MSI 인터럽트 레지스터 베이스; NVMe pending/mask/clear 접근 */
};

/* Internal PCIe Host Controller Information.*/
struct brcm_pcie {
	struct device		*dev; /* PCI/NVMe: platform device; NVMe probe/remove 컨텍스트 */
	void __iomem		*base; /* PCI/NVMe: PCIe RC MMIO 베이스; NVMe 모든 레지스터 접근 기준 */
	struct clk		*clk; /* PCI/NVMe: PCIe 컨트롤러 클럭; NVMe 동작 전 활성화 필요 */
	struct device_node	*np; /* PCI/NVMe: device-node; NVMe DT 속성 파싱 */
	bool			ssc; /* PCI/NVMe: Spread Spectrum Clocking; NVMe jitter 감소/호환성 */
	int			gen; /* PCI/NVMe: 강제 PCIe 세대; NVMe 링크 속도 제한 */
	u64			msi_target_addr; /* PCI/NVMe: MSI 목적지 주소; NVMe MSI 메시지 address 필드 */
	struct brcm_msi		*msi; /* PCI/NVMe: MSI 컨트롤러; NVMe MSI/MSI-X 할당자 */
	struct reset_control	*rescal; /* PCI/NVMe: RESCAL 리셋; NVMe PHY calibration 시퀀스 */
	struct reset_control	*perst_reset; /* PCI/NVMe: PERST# reset; NVMe 컨트롤러 fundamental 리셋 */
	struct reset_control	*bridge_reset; /* PCI/NVMe: bridge reset; NVMe PCIe bridge 리셋 */
	struct reset_control	*swinit_reset; /* PCI/NVMe: swinit reset; NVMe PCIe bridge sync 리셋 */
	int			num_memc; /* PCI/NVMe: 메모리 컨트롤러 수; NVMe DMA 뷰포트 memc 설정 */
	u64			memc_size[PCIE_BRCM_MAX_MEMC]; /* PCI/NVMe: memc 크기 배열; NVMe DMA 영역 구성 */
	u32			hw_rev; /* PCI/NVMe: 하드웨어 리비전; NVMe MSI 레거시 모드 결정 */
	struct subdev_regulators *sr; /* PCI/NVMe: 하위 장치 레귤레이터; NVMe 슬롯 전원 */
	bool			ep_wakeup_capable; /* PCI/NVMe: EP 웨이크업 지원; NVMe suspend 시 전원 유지 결정 */
	const struct pcie_cfg_data	*cfg; /* PCI/NVMe: SoC별 설정; NVMe SoC별 동작 분기 */
	bool			bridge_in_reset; /* PCI/NVMe: bridge 리셋 상태; NVMe register access 보호 */
	struct notifier_block	die_notifier; /* PCI/NVMe: die notifier; NVMe 치명 오류 시 PCIe 덤프 */
	struct notifier_block	panic_notifier; /* PCI/NVMe: panic notifier; NVMe 시스템 패닉 시 PCIe 덤프 */
	spinlock_t		bridge_lock; /* PCI/NVMe: bridge 보호 spinlock; NVMe 인터럽트 내 register 접근 동기화 */
};

static inline bool is_bmips(const struct brcm_pcie *pcie) /* PCI/NVMe: bmips SoC 판별; NVMe 32비트/128MB 창 특수 처리 */
{
	return pcie->cfg->soc_base == BCM7435 || pcie->cfg->soc_base == BCM7425; /* PCI/NVMe: BCM7435/7425 확인; NVMe bmipes 경로 선택 */
}

static int brcm_pcie_bridge_sw_init_set(struct brcm_pcie *pcie, u32 val) /* PCI/NVMe: PCIe bridge sw init 설정; NVMe bridge 리셋/활성화 */
{
	unsigned long flags; /* PCI/NVMe: irqsave 플래그; NVMe 인터럽트 중 bridge_lock 획득 */
	int ret; /* PCI/NVMe: 반환값; NVMe bridge 제어 성공/실패 */

	if (pcie->cfg->has_err_report) /* PCI/NVMe: 오류 보고 기능 시 lock; NVMe die/panic notifier와 동시 접근 방지 */
		spin_lock_irqsave(&pcie->bridge_lock, flags); /* PCI/NVMe: bridge lock 획득; NVMe register 접근 보호 */

	ret = pcie->cfg->bridge_sw_init_set(pcie, val); /* PCI/NVMe: SoC별 bridge sw init 수행; NVMe PCIe bridge on/off */
	/* If we fail, assume the bridge is in reset (off) */
	pcie->bridge_in_reset = ret ? true : val; /* PCI/NVMe: 실패 시 리셋 상태로 가정; NVMe 이후 register 접근 차단 */

	if (pcie->cfg->has_err_report) /* PCI/NVMe: lock 보유 시 해제; NVMe bridge register 접근 종료 */
		spin_unlock_irqrestore(&pcie->bridge_lock, flags); /* PCI/NVMe: bridge lock 해제; NVMe 인터럽트 복원 */

	return ret; /* PCI/NVMe: bridge 제어 결과; NVMe 초기화 진행/중단 판단 */
}

/*
 * This is to convert the size of the inbound "BAR" region to the
 * non-linear values of PCIE_X_MISC_RC_BAR[123]_CONFIG_LO.SIZE
 */
static int brcm_pcie_encode_ibar_size(u64 size) /* PCI/NVMe: inbound BAR 크기 인코딩; NVMe DMA 영역 크기 하드웨어 값 변환 */
{
	int log2_in = ilog2(size); /* PCI/NVMe: 크기 log2; NVMe 4KB~64GB 영역 인코딩 기반 */

	if (log2_in >= 12 && log2_in <= 15) /* PCI/NVMe: 4KB~32KB 범위; NVMe 소형 DMA 영역 */
		/* Covers 4KB to 32KB (inclusive) */
		return (log2_in - 12) + 0x1c; /* PCI/NVMe: 4KB~32KB 인코딩; NVMe 작은 BAR 설정 */
	else if (log2_in >= 16 && log2_in <= 36) /* PCI/NVMe: 64KB~64GB 범위; NVMe 일반 DMA 영역 */
		/* Covers 64KB to 64GB, (inclusive) */
		return log2_in - 15; /* PCI/NVMe: 64KB~64GB 인코딩; NVMe 시스템 메모리 뷰포트 */
	/* Something is awry so disable */
	return 0; /* PCI/NVMe: 유효하지 않은 크기; NVMe inbound window 비활성화 */
}

static u32 brcm_pcie_mdio_form_pkt(int port, int regad, int cmd) /* PCI/NVMe: MDIO 패킷 조립; NVMe PHY 레지스터 트랜잭션 형성 */
{
	u32 pkt = 0; /* PCI/NVMe: MDIO 패킷 초기화; NVMe PHY 명령 워드 */

	pkt |= FIELD_PREP(MDIO_PORT_EXT_MASK, port >> 4); /* PCI/NVMe: 확장 포트 필드; NVMe PHY 포트 상위비트 */
	pkt |= FIELD_PREP(MDIO_PORT_MASK, port); /* PCI/NVMe: 포트 필드; NVMe PHY 포트 번호 */
	pkt |= FIELD_PREP(MDIO_REGAD_MASK, regad); /* PCI/NVMe: 레지스터 주소; NVMe PHY 대상 레지스터 */
	pkt |= FIELD_PREP(MDIO_CMD_MASK, cmd); /* PCI/NVMe: 읽기/쓰기 명령; NVMe PHY 접근 방향 */

	return pkt; /* PCI/NVMe: MDIO 패킷 반환; NVMe PHY 트랜잭션 전송 */
}

/* negative return value indicates error */
static int brcm_pcie_mdio_read(void __iomem *base, u8 port, u8 regad, u32 *val) /* PCI/NVMe: MDIO 읽기; NVMe PHY/SSC 상태 확인 */
{
	u32 data; /* PCI/NVMe: MDIO 데이터 버퍼; NVMe PHY 읽기 결과 */
	int err; /* PCI/NVMe: 폴링 오류; NVMe PHY 응답 timeout */

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_READ), /* PCI/NVMe: 읽기 명령 전송; NVMe PHY 레지스터 요청 */
		   base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR); /* PCI/NVMe: 쓰기 완료 보장; NVMe PHY 트랜잭션 시작 */
	err = readl_poll_timeout_atomic(base + PCIE_RC_DL_MDIO_RD_DATA, data, /* PCI/NVMe: 폴링 완료; NVMe PHY 응답 대기 */
					MDIO_RD_DONE(data), 10, 100);
	*val = FIELD_GET(MDIO_DATA_MASK, data); /* PCI/NVMe: 데이터 추출; NVMe PHY 레지스터 값 */

	return err; /* PCI/NVMe: 읽기 결과; NVMe PHY 상태 획득 성공/실패 */
}

/* negative return value indicates error */
static int brcm_pcie_mdio_write(void __iomem *base, u8 port, /* PCI/NVMe: MDIO 쓰기; NVMe PHY/SSC 값 설정 */
				u8 regad, u16 wrdata)
{
	u32 data; /* PCI/NVMe: MDIO 데이터 버퍼; NVMe PHY 쓰기 완료 확인 */
	int err; /* PCI/NVMe: 폴링 오류; NVMe PHY 쓰기 timeout */

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_WRITE), /* PCI/NVMe: 쓰기 명령 전송; NVMe PHY 레지스터 대상 */
		   base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR); /* PCI/NVMe: 쓰기 동기화; NVMe PHY 트랜잭션 시작 */
	writel(MDIO_DATA_DONE_MASK | wrdata, base + PCIE_RC_DL_MDIO_WR_DATA); /* PCI/NVMe: 데이터+done 비트 기록; NVMe PHY 값 쓰기 */

	err = readl_poll_timeout_atomic(base + PCIE_RC_DL_MDIO_WR_DATA, data, /* PCI/NVMe: 쓰기 완료 폴링; NVMe PHY 처리 완료 대기 */
					MDIO_WT_DONE(data), 10, 100);
	return err; /* PCI/NVMe: 쓰기 결과; NVMe PHY 설정 성공/실패 */
}

/*
 * Configures device for Spread Spectrum Clocking (SSC) mode; a negative
 * return value indicates error.
 */
static int brcm_pcie_set_ssc(struct brcm_pcie *pcie) /* PCI/NVMe: SSC 설정; NVMe 링크 클록 jitter 감소 */
{
	int pll, ssc; /* PCI/NVMe: PLL lock/SSC 상태; NVMe 링크 안정성 확인 */
	int ret; /* PCI/NVMe: 반환값; NVMe SSC 적용 성공/실패 */
	u32 tmp; /* PCI/NVMe: 레지스터 임시값; NVMe SSC 제어/상태 */

	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, SET_ADDR_OFFSET, /* PCI/NVMe: SSC 레지스터 뱅크 선택; NVMe SSC 주소 설정 */
				   SSC_REGS_ADDR);
	if (ret < 0) /* PCI/NVMe: 쓰기 실패 검사; NVMe SSC 설정 중단 */
		return ret; /* PCI/NVMe: 오류 반환; NVMe SSC 미적용 */

	ret = brcm_pcie_mdio_read(pcie->base, MDIO_PORT0, /* PCI/NVMe: SSC 제어 읽기; NVMe SSC 현재 값 */
				  SSC_CNTL_OFFSET, &tmp);
	if (ret < 0) /* PCI/NVMe: 읽기 실패 검사; NVMe SSC 설정 중단 */
		return ret; /* PCI/NVMe: 오류 반환; NVMe SSC 미적용 */

	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_EN_MASK); /* PCI/NVMe: SSC override enable; NVMe SSC 수동 제어 활성 */
	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_VAL_MASK); /* PCI/NVMe: SSC override value=1; NVMe SSC on */
	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, /* PCI/NVMe: SSC 제어 쓰기; NVMe SSC 활성화 */
				   SSC_CNTL_OFFSET, tmp);
	if (ret < 0) /* PCI/NVMe: 쓰기 실패 검사; NVMe SSC 설정 중단 */
		return ret; /* PCI/NVMe: 오류 반환; NVMe SSC 미적용 */

	usleep_range(1000, 2000); /* PCI/NVMe: SSC 안정화 대기; NVMe PLL/SSC settling */
	ret = brcm_pcie_mdio_read(pcie->base, MDIO_PORT0, /* PCI/NVMe: SSC 상태 읽기; NVMe SSC 동작 확인 */
				  SSC_STATUS_OFFSET, &tmp);
	if (ret < 0) /* PCI/NVMe: 읽기 실패 검사; NVMe SSC 상태 미확인 */
		return ret; /* PCI/NVMe: 오류 반환; NVMe SSC 미확인 */

	ssc = FIELD_GET(SSC_STATUS_SSC_MASK, tmp); /* PCI/NVMe: SSC 동작 추출; NVMe SSC on 상태 */
	pll = FIELD_GET(SSC_STATUS_PLL_LOCK_MASK, tmp); /* PCI/NVMe: PLL lock 추출; NVMe 클록 잠금 상태 */

	return ssc && pll ? 0 : -EIO; /* PCI/NVMe: SSC+PLL lock 확인; NVMe 링크 클록 안정 시 성공 */
}

/* Limits operation to a specific generation (1, 2, or 3) */
static void brcm_pcie_set_gen(struct brcm_pcie *pcie, int gen) /* PCI/NVMe: PCIe 세한 제한; NVMe 링크 속도 강제 */
{
	u16 lnkctl2 = readw(pcie->base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2); /* PCI/NVMe: LINKCTL2 읽기; NVMe target link speed 설정 */
	u32 lnkcap = readl(pcie->base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* PCI/NVMe: link capability 읽기; NVMe max speed 광고 */

	u32p_replace_bits(&lnkcap, gen, PCI_EXP_LNKCAP_SLS); /* PCI/NVMe: advertised link speed 변경; NVMe 세대 협상 제한 */
	writel(lnkcap, pcie->base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* PCI/NVMe: capability 기록; NVMe link training speed 제한 */

	u16p_replace_bits(&lnkctl2, gen, PCI_EXP_LNKCTL2_TLS); /* PCI/NVMe: target link speed 변경; NVMe link training 목표 속도 */
	writew(lnkctl2, pcie->base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2); /* PCI/NVMe: LINKCTL2 기록; NVMe link training 적용 */
}

static void brcm_pcie_set_outbound_win(struct brcm_pcie *pcie, /* PCI/NVMe: outbound window 설정; NVMe 메모리 BAR CPU->PCIe 매핑 */
				       u8 win, u64 cpu_addr,
				       u64 pcie_addr, u64 size)
{
	u32 cpu_addr_mb_high, limit_addr_mb_high; /* PCI/NVMe: 상위 주소 변수; NVMe 64비트 주소 확장 */
	phys_addr_t cpu_addr_mb, limit_addr_mb; /* PCI/NVMe: MB 단위 주소; NVMe outbound 범위 */
	int high_addr_shift; /* PCI/NVMe: 상위 주소 시프트; NVMe base/limit 상위 레지스터 정렬 */
	u32 tmp; /* PCI/NVMe: 레지스터 임시값; NVMe window 설정 */

	/* Set the base of the pcie_addr window */
	writel(lower_32_bits(pcie_addr), pcie->base + PCIE_MEM_WIN0_LO(win)); /* PCI/NVMe: PCIe 하위 주소; NVMe BAR 접근 목적지 low */
	writel(upper_32_bits(pcie_addr), pcie->base + PCIE_MEM_WIN0_HI(win)); /* PCI/NVMe: PCIe 상위 주소; NVMe 64비트 BAR 목적지 high */

	/* Write the addr base & limit lower bits (in MBs) */
	cpu_addr_mb = cpu_addr / SZ_1M; /* PCI/NVMe: CPU base MB; NVMe outbound 하한 */
	limit_addr_mb = (cpu_addr + size - 1) / SZ_1M; /* PCI/NVMe: CPU limit MB; NVMe outbound 상한 */

	tmp = readl(pcie->base + PCIE_MEM_WIN0_BASE_LIMIT(win)); /* PCI/NVMe: 기존 base/limit 읽기; NVMe 보존 필드 유지 */
	u32p_replace_bits(&tmp, cpu_addr_mb, /* PCI/NVMe: base 필드 갱신; NVMe outbound 시작 MB */
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK);
	u32p_replace_bits(&tmp, limit_addr_mb, /* PCI/NVMe: limit 필드 갱신; NVMe outbound 끝 MB */
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK);
	writel(tmp, pcie->base + PCIE_MEM_WIN0_BASE_LIMIT(win)); /* PCI/NVMe: base/limit 기록; NVMe outbound 범위 설정 */

	if (is_bmips(pcie)) /* PCI/NVMe: bmips는 상위 주소 불필요; NVMe 32비트 메모리 공간 */
		return; /* PCI/NVMe: bmips early return; NVMe 32비트 outbound 완료 */

	/* Write the cpu & limit addr upper bits */
	high_addr_shift = /* PCI/NVMe: 상위 시프트 계산; NVMe 64비트 base/limit 정렬 */
		HWEIGHT32(PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK);

	cpu_addr_mb_high = cpu_addr_mb >> high_addr_shift; /* PCI/NVMe: CPU base 상위; NVMe 64비트 상위 주소 */
	tmp = readl(pcie->base + PCIE_MEM_WIN0_BASE_HI(win)); /* PCI/NVMe: 기존 base high 읽기; NVMe 상위 보존 필드 */
	u32p_replace_bits(&tmp, cpu_addr_mb_high, /* PCI/NVMe: base high 필드 갱신; NVMe 64비트 outbound 시작 */
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK);
	writel(tmp, pcie->base + PCIE_MEM_WIN0_BASE_HI(win)); /* PCI/NVMe: base high 기록; NVMe 64비트 하한 */

	limit_addr_mb_high = limit_addr_mb >> high_addr_shift; /* PCI/NVMe: CPU limit 상위; NVMe 64비트 상위 상한 */
	tmp = readl(pcie->base + PCIE_MEM_WIN0_LIMIT_HI(win)); /* PCI/NVMe: 기존 limit high 읽기; NVMe 상위 보존 필드 */
	u32p_replace_bits(&tmp, limit_addr_mb_high, /* PCI/NVMe: limit high 필드 갱신; NVMe 64비트 outbound 끝 */
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK);
	writel(tmp, pcie->base + PCIE_MEM_WIN0_LIMIT_HI(win)); /* PCI/NVMe: limit high 기록; NVMe 64비트 상한 */
}

#define BRCM_MSI_FLAGS_REQUIRED (MSI_FLAG_USE_DEF_DOM_OPS	| \
					 MSI_FLAG_USE_DEF_CHIP_OPS	| \
					 MSI_FLAG_NO_AFFINITY) /* PCI/NVMe: MSI 부모 필수 플래그; NVMe MSI 도메인 기본 동작, affinity 없음 */

#define BRCM_MSI_FLAGS_SUPPORTED (MSI_GENERIC_FLAGS_MASK	| \
					  MSI_FLAG_MULTI_PCI_MSI) /* PCI/NVMe: MSI 지원 플래그; NVMe multi-MSI 할당 허용 */

static const struct msi_parent_ops brcm_msi_parent_ops = {
	.required_flags		= BRCM_MSI_FLAGS_REQUIRED, /* PCI/NVMe: 필수 MSI 플래그; NVMe MSI 도메인 요구사항 */
	.supported_flags	= BRCM_MSI_FLAGS_SUPPORTED, /* PCI/NVMe: 지원 MSI 플래그; NVMe MSI 기능 범위 */
	.bus_select_token	= DOMAIN_BUS_PCI_MSI, /* PCI/NVMe: PCI MSI 버스 선택; NVMe MSI 도메인 연결 */
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK, /* PCI/NVMe: MSI chip ack 필요; NVMe MSI 처리 후 ack */
	.prefix			= "BRCM-", /* PCI/NVMe: MSI 도메인 이름 접두사; NVMe IRQ 이름 식별 */
	.init_dev_msi_info	= msi_lib_init_dev_msi_info, /* PCI/NVMe: MSI 장치 정보 초기화; NVMe MSI capability 파싱 연결 */
};

static void brcm_pcie_msi_isr(struct irq_desc *desc) /* PCI/NVMe: MSI cascade ISR; NVMe MSI 하드웨어 IRQ 수신 후 dispatch */
{
	struct irq_chip *chip = irq_desc_get_chip(desc); /* PCI/NVMe: 상위 irq chip; NVMe chained IRQ 처리 */
	unsigned long status; /* PCI/NVMe: MSI pending 비트맵; NVMe 도착 MSI 벡터들 */
	struct brcm_msi *msi; /* PCI/NVMe: MSI 컨트롤러; NVMe MSI 설정/상태 */
	struct device *dev; /* PCI/NVMe: device; NVMe 로그용 */
	u32 bit; /* PCI/NVMe: pending 비트 인덱스; NVMe MSI 벡터 번호 */

	chained_irq_enter(chip, desc); /* PCI/NVMe: chained IRQ 진입; NVMe parent IRQ mask/ack */
	msi = irq_desc_get_handler_data(desc); /* PCI/NVMe: handler_data로 MSI 구조체 획득; NVMe MSI 컨트롤러 참조 */
	dev = msi->dev; /* PCI/NVMe: device 참조; NVMe 로그 */

	status = readl(msi->intr_base + MSI_INT_STATUS); /* PCI/NVMe: MSI pending 읽기; NVMe 도착 MSI 확인 */
	status >>= msi->legacy_shift; /* PCI/NVMe: 레거시 시프트 적용; NVMe 공유 레지스터 내 MSI 위치 보정 */

	for_each_set_bit(bit, &status, msi->nr) { /* PCI/NVMe: 각 pending MSI 처리; NVMe 큐별 인터럽트 dispatch */
		int ret; /* PCI/NVMe: handle 결과; NVMe MSI 처리 성공/실패 */
		ret = generic_handle_domain_irq(msi->inner_domain, bit); /* PCI/NVMe: virq 핸들러 호출; NVMe 큐 인터럽트 -> nvme_interrupt() */
		if (ret) /* PCI/NVMe: 미등록 MSI 처리; NVMe spurious MSI 디버그 */
			dev_dbg(dev, "unexpected MSI\n"); /* PCI/NVMe: 예상외 MSI 로그; NVMe MSI 할당 불일치 */
	}

	chained_irq_exit(chip, desc); /* PCI/NVMe: chained IRQ 종료; NVMe parent IRQ unmask/복원 */
}

static void brcm_msi_compose_msi_msg(struct irq_data *data, struct msi_msg *msg) /* PCI/NVMe: MSI message 조합; NVMe MSI address/data 구성 */
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(data); /* PCI/NVMe: MSI chip data; NVMe 목적지 주소 획득 */

	msg->address_lo = lower_32_bits(msi->target_addr); /* PCI/NVMe: MSI address low; NVMe 컨트롤러가 쓸 32비트 주소 */
	msg->address_hi = upper_32_bits(msi->target_addr); /* PCI/NVMe: MSI address high; NVMe 64비트 MSI 확장 */
	msg->data = (0xffff & PCIE_MISC_MSI_DATA_CONFIG_VAL_32) | data->hwirq; /* PCI/NVMe: MSI data; NVMe 벡터 번호 포함 */
}

static void brcm_msi_ack_irq(struct irq_data *data) /* PCI/NVMe: MSI ack; NVMe MSI pending 비트 클리어 */
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(data); /* PCI/NVMe: MSI chip data; NVMe interrupt base 획득 */
	const int shift_amt = data->hwirq + msi->legacy_shift; /* PCI/NVMe: 클리어 비트 위치; NVMe 레거시 보정 포함 */

	writel(1 << shift_amt, msi->intr_base + MSI_INT_CLR); /* PCI/NVMe: 해당 MSI 비트 클리어; NVMe edge-triggered ack */
}


static struct irq_chip brcm_msi_bottom_irq_chip = {
	.name			= "BRCM STB MSI", /* PCI/NVMe: IRQ chip 이름; NVMe MSI 하드웨어 식별 */
	.irq_compose_msi_msg	= brcm_msi_compose_msi_msg, /* PCI/NVMe: MSI message 조합; NVMe MSI address/data 생성 */
	.irq_ack                = brcm_msi_ack_irq, /* PCI/NVMe: MSI ack; NVMe pending 클리어 */
};

static int brcm_msi_alloc(struct brcm_msi *msi, unsigned int nr_irqs) /* PCI/NVMe: MSI 벡터 할당; NVMe 큐 개수만큼 연속 벡터 */
{
	int hwirq; /* PCI/NVMe: 할당된 hwirq; NVMe MSI hardware vector base */

	mutex_lock(&msi->lock); /* PCI/NVMe: MSI lock 획득; NVMe 동시 할당 방지 */
	hwirq = bitmap_find_free_region(msi->used, msi->nr, /* PCI/NVMe: 연속 빈 영역 탐색; NVMe multi-MSI power-of-2 */
					order_base_2(nr_irqs));
	mutex_unlock(&msi->lock); /* PCI/NVMe: MSI lock 해제; NVMe 할당 완료 */

	return hwirq; /* PCI/NVMe: 할당 결과; NVMe MSI 벡터 base 또는 -EBUSY */
}

static void brcm_msi_free(struct brcm_msi *msi, unsigned long hwirq, /* PCI/NVMe: MSI 벡터 해제; NVMe 큐 제거/드라이버 unload */
			  unsigned int nr_irqs)
{
	mutex_lock(&msi->lock); /* PCI/NVMe: MSI lock 획득; NVMe 동시 해제 방지 */
	bitmap_release_region(msi->used, hwirq, order_base_2(nr_irqs)); /* PCI/NVMe: 사용 영역 반납; NVMe MSI 벡터 재사용 */
	mutex_unlock(&msi->lock); /* PCI/NVMe: MSI lock 해제; NVMe 해제 완료 */
}

static int brcm_irq_domain_alloc(struct irq_domain *domain, unsigned int virq, /* PCI/NVMe: IRQ domain 할당; NVMe virq->hwirq 매핑 생성 */
				 unsigned int nr_irqs, void *args)
{
	struct brcm_msi *msi = domain->host_data; /* PCI/NVMe: host_data; NVMe MSI 컨트롤러 */
	int hwirq, i; /* PCI/NVMe: hwirq/루프 인덱스; NVMe 벡터 매핑 */

	hwirq = brcm_msi_alloc(msi, nr_irqs); /* PCI/NVMe: MSI 하드웨어 벡터 할당; NVMe nr_irqs만큼 연속 */

	if (hwirq < 0) /* PCI/NVMe: 할당 실패; NVMe 요청 큐 수 초과 */
		return hwirq; /* PCI/NVMe: -EBUSY 반환; NVMe MSI 자원 부족 */

	for (i = 0; i < nr_irqs; i++) /* PCI/NVMe: 각 virq-hwirq 매핑; NVMe 큐별 IRQ 등록 */
		irq_domain_set_info(domain, virq + i, (irq_hw_number_t)hwirq + i, /* PCI/NVMe: domain info 설정; NVMe 큐 i에 hwirq+i 할당 */
				    &brcm_msi_bottom_irq_chip, domain->host_data,
				    handle_edge_irq, NULL, NULL);
	return 0; /* PCI/NVMe: 할당 성공; NVMe MSI 도메인 준비 완료 */
}

static void brcm_irq_domain_free(struct irq_domain *domain, /* PCI/NVMe: IRQ domain 해제; NVMe virq->hwirq 매핑 제거 */
				 unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq); /* PCI/NVMe: 첫 virq irq_data; NVMe 시작 hwirq 획득 */
	struct brcm_msi *msi = irq_data_get_irq_chip_data(d); /* PCI/NVMe: MSI 컨트롤러; NVMe 벡터 반납 */

	brcm_msi_free(msi, d->hwirq, nr_irqs); /* PCI/NVMe: hwirq 범위 해제; NVMe MSI 자원 회수 */
}

static const struct irq_domain_ops msi_domain_ops = {
	.alloc	= brcm_irq_domain_alloc, /* PCI/NVMe: domain 할당; NVMe MSI virq 생성 */
	.free	= brcm_irq_domain_free, /* PCI/NVMe: domain 해제; NVMe MSI virq 제거 */
};

static int brcm_allocate_domains(struct brcm_msi *msi) /* PCI/NVMe: MSI irq domain 생성; NVMe MSI 할당 인프라 구축 */
{
	struct device *dev = msi->dev; /* PCI/NVMe: device; NVMe 에러 로그용 */

	struct irq_domain_info info = { /* PCI/NVMe: domain 생성 정보; NVMe MSI domain 설정 */
		.fwnode		= of_fwnode_handle(msi->np), /* PCI/NVMe: DT fwnode; NVMe MSI domain 식별자 */
		.ops		= &msi_domain_ops, /* PCI/NVMe: domain ops; NVMe alloc/free 콜백 */
		.host_data	= msi, /* PCI/NVMe: host_data; NVMe MSI 컨트롤러 전달 */
		.size		= msi->nr, /* PCI/NVMe: domain 크기; NVMe 사용 가능 MSI 수 */
	};

	msi->inner_domain = msi_create_parent_irq_domain(&info, &brcm_msi_parent_ops); /* PCI/NVMe: MSI 부모 domain 생성; NVMe MSI 라이브러리 연결 */
	if (!msi->inner_domain) { /* PCI/NVMe: 생성 실패; NVMe MSI 초기화 불가 */
		dev_err(dev, "failed to create MSI domain\n"); /* PCI/NVMe: 에러 로그; NVMe MSI 사용 불가 */
		return -ENOMEM; /* PCI/NVMe: 메모리 부족; NVMe MSI 할당 인프라 실패 */
	}

	return 0; /* PCI/NVMe: domain 생성 성공; NVMe MSI 할당 준비 */
}

static void brcm_free_domains(struct brcm_msi *msi) /* PCI/NVMe: MSI irq domain 제거; NVMe MSI 인프라 해체 */
{
	irq_domain_remove(msi->inner_domain); /* PCI/NVMe: domain 제거; NVMe MSI virq/hwirq 매핑 정리 */
}

static void brcm_msi_remove(struct brcm_pcie *pcie) /* PCI/NVMe: MSI 제거; NVMe 장치 제거/드라이버 unload 시 */
{
	struct brcm_msi *msi = pcie->msi; /* PCI/NVMe: MSI 컨트롤러; NVMe MSI 자원 */

	if (!msi) /* PCI/NVMe: MSI 미사용; NVMe MSI 없음 */
		return; /* PCI/NVMe: early return; NVMe 정리 불필요 */
	irq_set_chained_handler_and_data(msi->irq, NULL, NULL); /* PCI/NVMe: chained handler 해제; NVMe MSI IRQ 분리 */
	brcm_free_domains(msi); /* PCI/NVMe: domain 제거; NVMe MSI 매핑 해제 */
}

static void brcm_msi_set_regs(struct brcm_msi *msi) /* PCI/NVMe: MSI 레지스터 설정; NVMe MSI 목적지/마스크/데이터 초기화 */
{
	u32 val = msi->legacy ? BRCM_INT_PCI_MSI_LEGACY_MASK : /* PCI/NVMe: 레거시/일반 마스크 선택; NVMe MSI 범위 */
				BRCM_INT_PCI_MSI_MASK;

	writel(val, msi->intr_base + MSI_INT_MASK_CLR); /* PCI/NVMe: MSI 마스크 클리어; NVMe 모든 MSI 벡터 활성화 */
	writel(val, msi->intr_base + MSI_INT_CLR); /* PCI/NVMe: MSI pending 클리어; NVMe 남아있는 MSI 제거 */

	/*
	 * The 0 bit of PCIE_MISC_MSI_BAR_CONFIG_LO is repurposed to MSI
	 * enable, which we set to 1.
	 */
	writel(lower_32_bits(msi->target_addr) | 0x1, /* PCI/NVMe: MSI target low + enable; NVMe MSI address 설정 및 MSI 활성화 */
	       msi->base + PCIE_MISC_MSI_BAR_CONFIG_LO);
	writel(upper_32_bits(msi->target_addr), /* PCI/NVMe: MSI target high; NVMe 64비트 MSI address */
	       msi->base + PCIE_MISC_MSI_BAR_CONFIG_HI);

	val = msi->legacy ? PCIE_MISC_MSI_DATA_CONFIG_VAL_8 : PCIE_MISC_MSI_DATA_CONFIG_VAL_32; /* PCI/NVMe: 레거시/일반 데이터 패턴; NVMe MSI data base */
	writel(val, msi->base + PCIE_MISC_MSI_DATA_CONFIG); /* PCI/NVMe: MSI data 패턴 기록; NVMe MSI data 공식 설정 */
}

static int brcm_pcie_enable_msi(struct brcm_pcie *pcie) /* PCI/NVMe: MSI 컨트롤러 활성화; NVMe MSI/MSI-X 지원 가능하게 함 */
{
	struct brcm_msi *msi; /* PCI/NVMe: MSI 컨트롤러; NVMe MSI 할당자 */
	int irq, ret; /* PCI/NVMe: IRQ/반환값; NVMe MSI 하드웨어 IRQ 및 결과 */
	struct device *dev = pcie->dev; /* PCI/NVMe: device; NVMe 로그용 */

	irq = irq_of_parse_and_map(dev->of_node, 1); /* PCI/NVMe: DT에서 MSI IRQ 획득; NVMe MSI parent IRQ */
	if (irq <= 0) { /* PCI/NVMe: IRQ 획득 실패; NVMe MSI 인터럽트 없음 */
		dev_err(dev, "cannot map MSI interrupt\n"); /* PCI/NVMe: 에러; NVMe MSI 불가 */
		return -ENODEV; /* PCI/NVMe: 장치 없음; NVMe MSI 초기화 실패 */
	}

	msi = devm_kzalloc(dev, sizeof(struct brcm_msi), GFP_KERNEL); /* PCI/NVMe: MSI 구조체 할당; NVMe MSI 컨트롤러 메모리 */
	if (!msi) /* PCI/NVMe: 메모리 부족; NVMe MSI 할당 실패 */
		return -ENOMEM; /* PCI/NVMe: 메모리 부족 반환; NVMe MSI 사용 불가 */

	mutex_init(&msi->lock); /* PCI/NVMe: MSI lock 초기화; NVMe alloc/free 동시성 보호 */
	msi->dev = dev; /* PCI/NVMe: device 설정; NVMe 로그용 */
	msi->base = pcie->base; /* PCI/NVMe: RC 레지스터 베이스; NVMe MSI 레지스터 접근 */
	msi->np = pcie->np; /* PCI/NVMe: DT node; NVMe MSI domain fwnode */
	msi->target_addr = pcie->msi_target_addr; /* PCI/NVMe: MSI 목적지 주소; NVMe MSI 메시지 도착 주소 */
	msi->irq = irq; /* PCI/NVMe: MSI IRQ 번호; NVMe MSI cascade parent */
	msi->legacy = pcie->hw_rev < BRCM_PCIE_HW_REV_33; /* PCI/NVMe: rev 3.3 미만 레거시; NVMe 공유 INTR2 MSI 사용 */

	/*
	 * Sanity check to make sure that the 'used' bitmap in struct brcm_msi
	 * is large enough.
	 */
	BUILD_BUG_ON(BRCM_INT_PCI_MSI_LEGACY_NR > BRCM_INT_PCI_MSI_NR); /* PCI/NVMe: 비트맵 크기 검증; NVMe 레거시 MSI가 일반 범위 내 */

	if (msi->legacy) { /* PCI/NVMe: 레거시 MSI; NVMe INTR2_CPU 공유 레지스터 사용 */
		msi->intr_base = msi->base + INTR2_CPU_BASE(pcie); /* PCI/NVMe: 공용 INTR2 베이스; NVMe 레거시 MSI pending/mask */
		msi->nr = BRCM_INT_PCI_MSI_LEGACY_NR; /* PCI/NVMe: 8개 MSI; NVMe 레거시 MSI 벡터 수 */
		msi->legacy_shift = 24; /* PCI/NVMe: 비트 24 시작; NVMe MSI가 상위 8비트에 위치 */
	} else { /* PCI/NVMe: 현대식 MSI; NVMe 전용 MSI 레지스터 사용 */
		msi->intr_base = msi->base + PCIE_MSI_INTR2_BASE; /* PCI/NVMe: 전용 MSI 베이스; NVMe 32개 MSI pending/mask */
		msi->nr = BRCM_INT_PCI_MSI_NR; /* PCI/NVMe: 32개 MSI; NVMe 최대 MSI 벡터 수 */
		msi->legacy_shift = 0; /* PCI/NVMe: 시프트 없음; NVMe MSI가 하위 비트 */
	}

	ret = brcm_allocate_domains(msi); /* PCI/NVMe: MSI domain 생성; NVMe MSI 할당 인프라 */
	if (ret) /* PCI/NVMe: domain 생성 실패; NVMe MSI 초기화 중단 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe MSI 사용 불가 */

	irq_set_chained_handler_and_data(msi->irq, brcm_pcie_msi_isr, msi); /* PCI/NVMe: MSI IRQ chain 등록; NVMe MSI 도착 시 dispatch */

	brcm_msi_set_regs(msi); /* PCI/NVMe: MSI 레지스터 설정; NVMe MSI 목적지/마스크 초기화 */
	pcie->msi = msi; /* PCI/NVMe: pcie에 MSI 연결; NVMe MSI 컨트롤러 활성화 */

	return 0; /* PCI/NVMe: MSI 활성화 성공; NVMe MSI/MSI-X 사용 가능 */
}

/* The controller is capable of serving in both RC and EP roles */
static bool brcm_pcie_rc_mode(struct brcm_pcie *pcie) /* PCI/NVMe: RC 모드 확인; NVMe 반드시 RC 모드여야 열거 가능 */
{
	void __iomem *base = pcie->base; /* PCI/NVMe: RC 레지스터 베이스; NVMe PORT 상태 읽기 */
	u32 val = readl(base + PCIE_MISC_PCIE_STATUS); /* PCI/NVMe: PCIe 상태 읽기; NVMe RC/EP 모드 비트 */

	return !!FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK, val); /* PCI/NVMe: RC 모드 비트 반환; NVMe RC면 true */
}

static bool brcm_pcie_link_up(struct brcm_pcie *pcie) /* PCI/NVMe: PCIe 링크 업 확인; NVMe 열거/접근 전 조건 */
{
	u32 val = readl(pcie->base + PCIE_MISC_PCIE_STATUS); /* PCI/NVMe: PCIe 상태 읽기; NVMe DL active/PHY linkup 확인 */
	u32 dla = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK, val); /* PCI/NVMe: Data Link active; NVMe TLP 전송 가능 */
	u32 plu = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK, val); /* PCI/NVMe: PHY link up; NVMe 물리 연결 완료 */

	return dla && plu; /* PCI/NVMe: 둘 다 1이면 link up; NVMe 장치 통신 가능 */
}

static void __iomem *brcm_pcie_map_bus(struct pci_bus *bus, /* PCI/NVMe: config space 버스 매핑; NVMe PCIe capability/BAR 접근 */
				       unsigned int devfn, int where)
{
	struct brcm_pcie *pcie = bus->sysdata; /* PCI/NVMe: host bridge sysdata; NVMe RC 사설 데이터 */
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe config 접근 기준 */
	int idx; /* PCI/NVMe: ECAM 인덱스; NVMe BDF 선택 */

	/* Accesses to the RC go right to the RC registers if !devfn */
	if (pci_is_root_bus(bus)) /* PCI/NVMe: root bus 접근; NVMe RC 자체 config */
		return devfn ? NULL : base + PCIE_ECAM_REG(where); /* PCI/NVMe: RC는 devfn 0만; NVMe RC capability 접근 */

	/* An access to our HW w/o link-up will cause a CPU Abort */
	if (!brcm_pcie_link_up(pcie)) /* PCI/NVMe: 링크 다운 시 NULL; NVMe 미연결 장치 접근 방지(어보트 회피) */
		return NULL; /* PCI/NVMe: NULL 반환; NVMe config read UR 처리 */

	/* For devices, write to the config space index register */
	idx = PCIE_ECAM_OFFSET(bus->number, devfn, 0); /* PCI/NVMe: BDF 오프셋; NVMe 대상 장치 선택 */
	writel(idx, base + IDX_ADDR(pcie)); /* PCI/NVMe: 인덱스 레지스터 기록; NVMe config window BDF 설정 */
	return base + DATA_ADDR(pcie) + PCIE_ECAM_REG(where); /* PCI/NVMe: config data 주소; NVMe where 레지스터 접근 */
}

static void __iomem *brcm7425_pcie_map_bus(struct pci_bus *bus, /* PCI/NVMe: BCM7425 config 매핑; NVMe 32비트 config 접근 */
					   unsigned int devfn, int where)
{
	struct brcm_pcie *pcie = bus->sysdata; /* PCI/NVMe: host bridge sysdata; NVMe RC 사설 데이터 */
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe config 접근 기준 */
	int idx; /* PCI/NVMe: ECAM 인덱스; NVMe BDF+offset 선택 */

	/* Accesses to the RC go right to the RC registers if !devfn */
	if (pci_is_root_bus(bus)) /* PCI/NVMe: root bus 접근; NVMe RC 자체 config */
		return devfn ? NULL : base + PCIE_ECAM_REG(where); /* PCI/NVMe: RC는 devfn 0만; NVMe RC capability 접근 */

	/* An access to our HW w/o link-up will cause a CPU Abort */
	if (!brcm_pcie_link_up(pcie)) /* PCI/NVMe: 링크 다운 시 NULL; NVMe 미연결 장치 접근 방지 */
		return NULL; /* PCI/NVMe: NULL 반환; NVMe config read UR 처리 */

	/* For devices, write to the config space index register */
	idx = PCIE_ECAM_OFFSET(bus->number, devfn, where); /* PCI/NVMe: BDF+where 포함; NVMe 7425는 offset도 인덱스에 포함 */
	writel(idx, base + IDX_ADDR(pcie)); /* PCI/NVMe: 인덱스 레지스터 기록; NVMe config window 선택 */
	return base + DATA_ADDR(pcie); /* PCI/NVMe: 고정 data 주소; NVMe 7425는 data 레지스터 고정 */
}

static int brcm_pcie_bridge_sw_init_set_generic(struct brcm_pcie *pcie, u32 val) /* PCI/NVMe: generic bridge sw init; NVMe bridge 리셋 제어 */
{
	u32 tmp, mask = RGR1_SW_INIT_1_INIT_GENERIC_MASK; /* PCI/NVMe: 마스크/임시값; NVMe bridge init 필드 */
	u32 shift = RGR1_SW_INIT_1_INIT_GENERIC_SHIFT; /* PCI/NVMe: 필드 시프트; NVMe bridge init 위치 */
	int ret = 0; /* PCI/NVMe: 반환값; NVMe bridge 제어 성공 가정 */

	if (pcie->bridge_reset) { /* PCI/NVMe: reset_control 사용; NVPe bridge reset 컨트롤러 있음 */
		if (val) /* PCI/NVMe: assert 요청; NVMe bridge reset assert */
			ret = reset_control_assert(pcie->bridge_reset); /* PCI/NVMe: bridge reset assert; NVMe PCIe bridge off */
		else /* PCI/NVMe: deassert 요청; NVMe bridge reset deassert */
			ret = reset_control_deassert(pcie->bridge_reset); /* PCI/NVMe: bridge reset deassert; NVMe PCIe bridge on */

		if (ret) /* PCI/NVMe: 제어 실패; NVMe bridge 상태 불명 */
			dev_err(pcie->dev, "failed to %s 'bridge' reset, err=%d\n", /* PCI/NVMe: 에러 로그; NVMe 초기화 실패 원인 */
				val ? "assert" : "deassert", ret);

		return ret; /* PCI/NVMe: reset_control 결과; NVMe bridge 제어 완료 */
	}

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* PCI/NVMe: 현재 RGR1 읽기; NVMe bridge init 필드 보존 */
	tmp = (tmp & ~mask) | ((val << shift) & mask); /* PCI/NVMe: bridge init 필드 갱신; NVMe bridge on/off */
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* PCI/NVMe: RGR1 기록; NVMe bridge sw init 적용 */

	return ret; /* PCI/NVMe: 결과 반환; NVMe bridge 제어 완료 */
}

static int brcm_pcie_bridge_sw_init_set_7278(struct brcm_pcie *pcie, u32 val) /* PCI/NVMe: 7278 bridge sw init; NVMe bridge 리셋(7278 전용) */
{
	u32 tmp, mask =  RGR1_SW_INIT_1_INIT_7278_MASK; /* PCI/NVMe: 7278 마스크; NVMe bridge init 필드 */
	u32 shift = RGR1_SW_INIT_1_INIT_7278_SHIFT; /* PCI/NVMe: 7278 시프트; NVMe bridge init 위치 */

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* PCI/NVMe: RGR1 읽기; NVMe bridge init 필드 보존 */
	tmp = (tmp & ~mask) | ((val << shift) & mask); /* PCI/NVMe: bridge init 필드 갱신; NVMe bridge on/off */
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* PCI/NVMe: RGR1 기록; NVMe bridge sw init 적용 */

	return 0; /* PCI/NVMe: 성공; NVMe bridge 제어 완료 */
}

static int brcm_pcie_perst_set_4908(struct brcm_pcie *pcie, u32 val) /* PCI/NVMe: BCM4908 PERST#; NVMe fundamental reset 제어 */
{
	int ret; /* PCI/NVMe: 반환값; NVMe PERST# 제어 결과 */

	if (WARN_ONCE(!pcie->perst_reset, "missing PERST# reset controller\n")) /* PCI/NVMe: reset control 누락 경고; NVMe PERST# 없으면 문제 */
		return -EINVAL; /* PCI/NVMe: 잘못된 인자; NVMe PERST# 제어 불가 */

	if (val) /* PCI/NVMe: assert; NVMe 컨트롤러 리셋 assert */
		ret = reset_control_assert(pcie->perst_reset); /* PCI/NVMe: PERST# assert; NVMe 장치 리셋 */
	else /* PCI/NVMe: deassert; NVMe 컨트롤러 리셋 해제 */
		ret = reset_control_deassert(pcie->perst_reset); /* PCI/NVMe: PERST# deassert; NVMe 장치 부팅 */

	if (ret) /* PCI/NVMe: 제어 실패; NVMe 리셋 상태 불명 */
		dev_err(pcie->dev, "failed to %s 'perst' reset, err=%d\n", /* PCI/NVMe: 에러 로그; NVMe 초기화 실패 원인 */
			val ? "assert" : "deassert", ret);
	return ret; /* PCI/NVMe: PERST# 결과; NVMe 리셋 제어 완료 */
}

static int brcm_pcie_perst_set_7278(struct brcm_pcie *pcie, u32 val) /* PCI/NVMe: 7278 PERST#; NVMe fundamental reset(7278 전용) */
{
	u32 tmp; /* PCI/NVMe: 레지스터 임시값; NVMe PERST# 비트 제어 */

	/* Perst bit has moved and assert value is 0 */
	tmp = readl(pcie->base + PCIE_MISC_PCIE_CTRL); /* PCI/NVMe: PCIE_CTRL 읽기; NVMe PERST# 필드 보존 */
	u32p_replace_bits(&tmp, !val, PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK); /* PCI/NVMe: PERSTB 비트 반전; NVMe val=1이면 assert(비트0) */
	writel(tmp, pcie->base +  PCIE_MISC_PCIE_CTRL); /* PCI/NVMe: PCIE_CTRL 기록; NVMe PERST# 적용 */

	return 0; /* PCI/NVMe: 성공; NVMe PERST# 제어 완료 */
}

static int brcm_pcie_perst_set_generic(struct brcm_pcie *pcie, u32 val) /* PCI/NVMe: generic PERST#; NVMe fundamental reset 제어 */
{
	u32 tmp; /* PCI/NVMe: 레지스터 임시값; NVMe PERST# 비트 제어 */

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* PCI/NVMe: RGR1 읽기; NVMe PERST# 필드 보존 */
	u32p_replace_bits(&tmp, val, PCIE_RGR1_SW_INIT_1_PERST_MASK); /* PCI/NVMe: PERST# 필드 설정; NVMe val=1이면 assert */
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1(pcie)); /* PCI/NVMe: RGR1 기록; NVMe PERST# 적용 */

	return 0; /* PCI/NVMe: 성공; NVMe PERST# 제어 완료 */
}

static int brcm_pcie_post_setup_bcm2712(struct brcm_pcie *pcie) /* PCI/NVMe: BCM2712 후처리; NVMe PHY/참조클록/L1SS 튜닝 */
{
	static const u16 data[] = { 0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030, /* PCI/NVMe: PHY 튜닝 데이터; NVMe 링크 안정성 */
				    0x5030, 0x0007 };
	static const u8 regs[] = { 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e }; /* PCI/NVMe: PHY 레지스터 번호; NVMe PHY 설정 대상 */
	int ret, i; /* PCI/NVMe: 반환값/루프; NVMe PHY 튜닝 */
	u32 tmp; /* PCI/NVMe: 레지스터 임시값; NVMe L1SS 타이머 */

	/* Allow a 54MHz (xosc) refclk source */
	ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, SET_ADDR_OFFSET, 0x1600); /* PCI/NVMe: refclk 설정 뱅크; NVMe 54MHz refclk 선택 */
	if (ret < 0) /* PCI/NVMe: 쓰기 실패; NVMe PHY 설정 중단 */
		return ret; /* PCI/NVMe: 오류 반환; NVMe PHY 튜닝 실패 */

	for (i = 0; i < ARRAY_SIZE(regs); i++) { /* PCI/NVMe: PHY 레지스터 루프; NVMe PHY 튜닝 값 적용 */
		ret = brcm_pcie_mdio_write(pcie->base, MDIO_PORT0, regs[i], data[i]); /* PCI/NVMe: PHY 레지스터 쓰기; NVMe PHY 값 설정 */
		if (ret < 0) /* PCI/NVMe: 쓰기 실패; NVMe PHY 튜닝 중단 */
			return ret; /* PCI/NVMe: 오류 반환; NVMe PHY 튜닝 실패 */
	}

	usleep_range(100, 200); /* PCI/NVMe: PHY settling; NVMe PHY 값 안정화 대기 */

	/*
	 * Set L1SS sub-state timers to avoid lengthy state transitions,
	 * PM clock period is 18.52ns (1/54MHz, round down).
	 */
	tmp = readl(pcie->base + PCIE_RC_PL_PHY_CTL_15); /* PCI/NVMe: PHY CTL15 읽기; NVMe PM clock period 필드 */
	tmp &= ~PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK; /* PCI/NVMe: PM clock period 클리어; NVMe L1SS 타이머 초기화 */
	tmp |= 0x12; /* PCI/NVMe: 18(0x12) 주기 설정; NVMe L1SS 빠른 전환(54MHz 기준) */
	writel(tmp, pcie->base + PCIE_RC_PL_PHY_CTL_15); /* PCI/NVMe: PHY CTL15 기록; NVMe L1SS 타이밍 적용 */

	return 0; /* PCI/NVMe: 후처리 성공; NVMe PHY/L1SS 튜닝 완료 */
}

static void add_inbound_win(struct inbound_win *b, u8 *count, u64 size, /* PCI/NVMe: inbound window 추가; NVMe DMA 뷰포트 구성 */
			    u64 cpu_addr, u64 pci_offset)
{
	b->size = size; /* PCI/NVMe: window 크기; NVMe DMA 변환 영역 */
	b->cpu_addr = cpu_addr; /* PCI/NVMe: CPU 주소; NVMe DMA 물리 주소 시작 */
	b->pci_offset = pci_offset; /* PCI/NVMe: PCIe 오프셋; NVMe 버스 주소 시작 */
	(*count)++; /* PCI/NVMe: window 개수 증가; NVMe 사용된 inbound window 수 */
}

static int brcm_pcie_get_inbound_wins(struct brcm_pcie *pcie, /* PCI/NVMe: inbound window 파싱; NVMe dma-ranges 기반 DMA 설정 */
				      struct inbound_win inbound_wins[])
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* PCI/NVMe: host bridge; NVMe dma-ranges/windows 접근 */
	u64 pci_offset, cpu_addr, size = 0, tot_size = 0; /* PCI/NVMe: 주소/크기 변수; NVMe DMA 범위 누적 */
	struct resource_entry *entry; /* PCI/NVMe: 리소스 엔트리; NVMe dma-ranges 탐색 */
	struct device *dev = pcie->dev; /* PCI/NVMe: device; NVMe 에러 로그 */
	u64 lowest_pcie_addr = ~(u64)0; /* PCI/NVMe: 최소 PCIe 주소; NVMe DMA 뷰포트 정렬 기준 */
	int ret, i = 0; /* PCI/NVMe: 반환값/인덱스; NVMe memc 루프 */
	u8 n = 0; /* PCI/NVMe: inbound window 개수; NVMe 창 카운트 */

	/*
	 * The HW registers (and PCIe) use order-1 numbering for BARs.  As such,
	 * we have inbound_wins[0] unused and BAR1 starts at inbound_wins[1].
	 */
	struct inbound_win *b_begin = &inbound_wins[1]; /* PCI/NVMe: BAR1부터 시작; NVMe inbound_wins[0] 미사용 */
	struct inbound_win *b = b_begin; /* PCI/NVMe: 현재 inbound window 포인터; NVMe BAR 순회 */

	/*
	 * STB chips beside 7712 disable the first inbound window default.
	 * Rather being mapped to system memory it is mapped to the
	 * internal registers of the SoC.  This feature is deprecated, has
	 * security considerations, and is not implemented in our modern
	 * SoCs.
	 */
	if (pcie->cfg->soc_base != BCM7712) /* PCI/NVMe: BCM7712 제외; NVMe BAR1은 SoC 내부 레지스터로 disable */
		add_inbound_win(b++, &n, 0, 0, 0); /* PCI/NVMe: BAR1 disable; NVMe 시스템 메모리로 매핑되지 않음 */

	resource_list_for_each_entry(entry, &bridge->dma_ranges) { /* PCI/NVMe: dma-ranges 순회; NVMe DT DMA 범위 파싱 */
		u64 pcie_start = entry->res->start - entry->offset; /* PCI/NVMe: PCIe 시작 주소; NVMe bus 주소 */
		u64 cpu_start = entry->res->start; /* PCI/NVMe: CPU 시작 주소; NVMe 시스템 메모리 주소 */

		size = resource_size(entry->res); /* PCI/NVMe: 리소스 크기; NVMe DMA 영역 크기 */
		tot_size += size; /* PCI/NVMe: 총 크기 누적; NVMe 시스템 메모리 뷰포트 합산 */
		if (pcie_start < lowest_pcie_addr) /* PCI/NVMe: 최소 PCIe 주소 갱신; NVMe 뷰포트 정렬 기준 */
			lowest_pcie_addr = pcie_start; /* PCI/NVMe: 최소 주소 저장; NVMe 뷰포트 시작점 */
		/*
		 * 7712 and newer chips may have many BARs, with each
		 * offering a non-overlapping viewport to system memory.
		 * That being said, each BARs size must still be a power of
		 * two.
		 */
		if (pcie->cfg->soc_base == BCM7712) /* PCI/NVMe: BCM7712; NVMe 여러 inbound window 사용 */
			add_inbound_win(b++, &n, size, cpu_start, pcie_start); /* PCI/NVMe: per-range window; NVMe DMA 뷰포트 추가 */

		if (n > pcie->cfg->num_inbound_wins) /* PCI/NVMe: 최대 window 초과; NVMe inbound 한도 초과 */
			break; /* PCI/NVMe: 루프 종료; NVMe 더 이상 window 추가 불가 */
	}

	if (lowest_pcie_addr == ~(u64)0) { /* PCI/NVMe: dma-ranges 없음; NVMe DMA 설정 불가 */
		dev_err(dev, "DT node has no dma-ranges\n"); /* PCI/NVMe: 에러; NVMe DMA 주소 변환 불가 */
		return -EINVAL; /* PCI/NVMe: 설정 오류; NVMe probe 실패 */
	}

	/*
	 * 7712 and newer chips do not have an internal memory mapping system
	 * that enables multiple memory controllers.  As such, it can return
	 * now w/o doing special configuration.
	 */
	if (pcie->cfg->soc_base == BCM7712) /* PCI/NVMe: BCM7712; NVMe 추가 memc 설정 불필요 */
		return n; /* PCI/NVMe: window 개수 반환; NVMe inbound 구성 완료 */

	ret = of_property_read_variable_u64_array(pcie->np, "brcm,scb-sizes", pcie->memc_size, 1, /* PCI/NVMe: memc 크기 읽기; NVMe 메모리 컨트롤러 크기 */
						  PCIE_BRCM_MAX_MEMC);
	if (ret <= 0) { /* PCI/NVMe: 읽기 실패; NVMe memc 크기 추정 */
		/* Make an educated guess */
		pcie->num_memc = 1; /* PCI/NVMe: memc 1개 가정; NVMe 단일 메모리 컨트롤러 */
		pcie->memc_size[0] = 1ULL << fls64(tot_size - 1); /* PCI/NVMe: tot_size보다 큰 2의 거듭제곱; NVMe DMA 뷰포트 크기 추정 */
	} else { /* PCI/NVMe: memc 크기 획득; NVMe 멀티 memc 설정 */
		pcie->num_memc = ret; /* PCI/NVMe: memc 개수 저장; NVMe DMA 뷰포트 memc 수 */
	}

	/* Each memc is viewed through a "port" that is a power of 2 */
	for (i = 0, size = 0; i < pcie->num_memc; i++) /* PCI/NVMe: memc 크기 합산; NVMe 전체 DMA 뷰포트 크기 */
		size += pcie->memc_size[i]; /* PCI/NVMe: memc 크기 누적; NVMe 시스템 메모리 합 */

	/* Our HW mandates that the window size must be a power of 2 */
	size = 1ULL << fls64(size - 1); /* PCI/NVMe: 2의 거듭제곱으로 반올림; NVMe inbound window 크기 규격 */

	/*
	 * For STB chips, the BAR2 cpu_addr is hardwired to the start
	 * of system memory, so we set it to 0.
	 */
	cpu_addr = 0; /* PCI/NVMe: BAR2 CPU 주소 0; NVMe 시스템 메모리 시작 */
	pci_offset = lowest_pcie_addr; /* PCI/NVMe: BAR2 PCIe 오프셋; NVMe 최소 PCIe 주소 */

	/*
	 * We validate the inbound memory view even though we should trust
	 * whatever the device-tree provides. This is because of an HW issue on
	 * early Raspberry Pi 4's revisions (bcm2711). It turns out its
	 * firmware has to dynamically edit dma-ranges due to a bug on the
	 * PCIe controller integration, which prohibits any access above the
	 * lower 3GB of memory. Given this, we decided to keep the dma-ranges
	 * in check, avoiding hard to debug device-tree related issues in the
	 * future:
	 *
	 * The PCIe host controller by design must set the inbound viewport to
	 * be a contiguous arrangement of all of the system's memory.  In
	 * addition, its size must be a power of two.  To further complicate
	 * matters, the viewport must start on a pcie-address that is aligned
	 * on a multiple of its size.  If a portion of the viewport does not
	 * represent system memory -- e.g. 3GB of memory requires a 4GB
	 * viewport -- we can map the outbound memory in or after 3GB and even
	 * though the viewport will overlap the outbound memory the controller
	 * will know to send outbound memory downstream and everything else
	 * upstream.
	 *
	 * For example:
	 *
	 * - The best-case scenario, memory up to 3GB, is to place the inbound
	 *   region in the first 4GB of pcie-space, as some legacy devices can
	 *   only address 32bits. We would also like to put the MSI under 4GB
	 *   as well, since some devices require a 32bit MSI target address.
	 *
	 * - If the system memory is 4GB or larger we cannot start the inbound
	 *   region at location 0 (since we have to allow some space for
	 *   outbound memory @ 3GB). So instead it will  start at the 1x
	 *   multiple of its size
	 */
	if (!size || (pci_offset & (size - 1)) || /* PCI/NVMe: 크기/정렬 검증; NVMe DMA 뷰포트 규격 확인 */
	    (pci_offset < SZ_4G && pci_offset > SZ_2G)) { /* PCI/NVMe: 2G~4G 시작 금지; NVMe outbound 충돌 방지 */
		dev_err(dev, "Invalid inbound_win2_offset/size: size 0x%llx, off 0x%llx\n", /* PCI/NVMe: 에러; NVMe DMA 설정 오류 */
			size, pci_offset);
		return -EINVAL; /* PCI/NVMe: 오류 반환; NVMe probe 실패 */
	}

	/* Enable inbound window 2, the main inbound window for STB chips */
	add_inbound_win(b++, &n, size, cpu_addr, pci_offset); /* PCI/NVMe: BAR2 inbound window; NVMe 시스템 메모리 DMA 뷰포트 */

	/*
	 * Disable inbound window 3.  On some chips presents the same
	 * window as #2 but the data appears in a settable endianness.
	 */
	add_inbound_win(b++, &n, 0, 0, 0); /* PCI/NVMe: BAR3 disable; NVMe 중복/endian window 사용 안함 */

	return n; /* PCI/NVMe: 총 inbound window 수; NVMe DMA 뷰포트 설정 완료 */
}

static u32 brcm_bar_reg_offset(int bar) /* PCI/NVMe: BAR config 레지스터 오프셋; NVMe inbound window 설정 */
{
	if (bar <= 3) /* PCI/NVMe: BAR1~3; NVMe 메인 inbound window */
		return PCIE_MISC_RC_BAR1_CONFIG_LO + 8 * (bar - 1); /* PCI/NVMe: BAR1 기준; NVMe BARn config 주소 */
	else /* PCI/NVMe: BAR4~; NVMe 추가 inbound window */
		return PCIE_MISC_RC_BAR4_CONFIG_LO + 8 * (bar - 4); /* PCI/NVMe: BAR4 기준; NVMe BARn config 주소 */
}

static u32 brcm_ubus_reg_offset(int bar) /* PCI/NVMe: UBUS remap 레지스터 오프셋; NVMe BCM7712 DMA remap */
{
	if (bar <= 3) /* PCI/NVMe: BAR1~3; NVMe UBUS remap */
		return PCIE_MISC_UBUS_BAR1_CONFIG_REMAP + 8 * (bar - 1); /* PCI/NVMe: BAR1 기준; NVMe UBUS remap 주소 */
	else /* PCI/NVMe: BAR4~; NVMe 추가 UBUS remap */
		return PCIE_MISC_UBUS_BAR4_CONFIG_REMAP + 8 * (bar - 4); /* PCI/NVMe: BAR4 기준; NVMe UBUS remap 주소 */
}

static void set_inbound_win_registers(struct brcm_pcie *pcie, /* PCI/NVMe: inbound window 레지스터 기록; NVMe DMA 주소 변환 활성화 */
				      const struct inbound_win *inbound_wins,
				      u8 num_inbound_wins)
{
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe inbound 레지스터 접근 */
	int i; /* PCI/NVMe: BAR 인덱스; NVMe inbound window 루프 */

	for (i = 1; i <= num_inbound_wins; i++) { /* PCI/NVMe: BAR1~num_inbound_wins; NVMe inbound window 설정 */
		u64 pci_offset = inbound_wins[i].pci_offset; /* PCI/NVMe: PCIe 오프셋; NVMe bus 주소 */
		u64 cpu_addr = inbound_wins[i].cpu_addr; /* PCI/NVMe: CPU 주소; NVMe 물리 주소(BCM7712 remap) */
		u64 size = inbound_wins[i].size; /* PCI/NVMe: window 크기; NVMe DMA 영역 크기 */
		u32 reg_offset = brcm_bar_reg_offset(i); /* PCI/NVMe: BAR config 오프셋; NVMe inbound window 설정 레지스터 */
		u32 tmp = lower_32_bits(pci_offset); /* PCI/NVMe: PCIe 하위 주소; NVMe BAR low 값 */

		u32p_replace_bits(&tmp, brcm_pcie_encode_ibar_size(size), /* PCI/NVMe: 크기 인코딩; NVMe BAR size 필드 */
				  PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK);

		/* Write low */
		writel_relaxed(tmp, base + reg_offset); /* PCI/NVMe: BAR low 기록; NVMe inbound window base/size */
		/* Write high */
		writel_relaxed(upper_32_bits(pci_offset), base + reg_offset + 4); /* PCI/NVMe: BAR high 기록; NVMe 64비트 inbound window */

		/*
		 * Most STB chips:
		 *     Do nothing.
		 * 7712:
		 *     All of their BARs need to be set.
		 */
		if (pcie->cfg->soc_base == BCM7712) { /* PCI/NVMe: BCM7712; NVMe UBUS remap 추가 설정 */
			/* BUS remap register settings */
			reg_offset = brcm_ubus_reg_offset(i); /* PCI/NVMe: UBUS remap 오프셋; NVMe CPU 메모리 remap */
			tmp = lower_32_bits(cpu_addr) & ~0xfff; /* PCI/NVMe: CPU 주소 4K 정렬; NVMe UBUS remap low */
			tmp |= PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_EN_MASK; /* PCI/NVMe: 접근 활성화; NVMe inbound DMA 허용 */
			writel_relaxed(tmp, base + reg_offset); /* PCI/NVMe: UBUS remap low; NVMe CPU->PCIe 매핑 */
			tmp = upper_32_bits(cpu_addr); /* PCI/NVMe: CPU 주소 상위; NVMe 64비트 UBUS remap */
			writel_relaxed(tmp, base + reg_offset + 4); /* PCI/NVMe: UBUS remap high; NVMe 64비트 CPU 메모리 매핑 */
		}
	}
}

static int brcm_pcie_setup(struct brcm_pcie *pcie) /* PCI/NVMe: PCIe RC 초기화; NVMe 열거 전 링크/창/MSI 기반 설정 */
{
	struct inbound_win inbound_wins[PCIE_BRCM_MAX_INBOUND_WINS]; /* PCI/NVMe: inbound window 배열; NVMe DMA 뷰포트 임시 구성 */
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe 레지스터 접근 */
	struct pci_host_bridge *bridge; /* PCI/NVMe: host bridge; NVMe windows/dma_ranges 접근 */
	struct resource_entry *entry; /* PCI/NVMe: resource entry; NVMe outbound 메모리 탐색 */
	u32 tmp, burst, num_lanes, num_lanes_cap; /* PCI/NVMe: 레지스터 임시값/버스트/레인 수; NVMe 성능 설정 */
	u8 num_out_wins = 0; /* PCI/NVMe: outbound window 개수; NVMe 메모리 창 카운트 */
	int num_inbound_wins = 0; /* PCI/NVMe: inbound window 개수; NVMe DMA 창 카운트 */
	int memc, ret; /* PCI/NVMe: memc 인덱스/반환값; NVMe DMA/초기화 결과 */

	/* Reset the bridge */
	ret = brcm_pcie_bridge_sw_init_set(pcie, 1); /* PCI/NVMe: bridge 리셋 assert; NVMe PCIe bridge off */
	if (ret) /* PCI/NVMe: 리셋 실패; NVMe 초기화 중단 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe setup 불가 */

	/* Ensure that PERST# is asserted; some bootloaders may deassert it. */
	if (pcie->cfg->soc_base == BCM2711) { /* PCI/NVMe: BCM2711; NVMe bootloader PERST# 상태 보정 */
		ret = pcie->cfg->perst_set(pcie, 1); /* PCI/NVMe: PERST# assert; NVMe 장치 리셋 상태 확보 */
		if (ret) { /* PCI/NVMe: PERST# 실패; NVMe 초기화 중단 */
			pcie->cfg->bridge_sw_init_set(pcie, 0); /* PCI/NVMe: bridge 해제 시도; NVMe cleanup */
			return ret; /* PCI/NVMe: 실패 반환; NVMe setup 불가 */
		}
	}

	usleep_range(100, 200); /* PCI/NVMe: 리셋 안정화 대기; NVMe bridge/PERST settling */

	/* Take the bridge out of reset */
	ret = brcm_pcie_bridge_sw_init_set(pcie, 0); /* PCI/NVMe: bridge 리셋 해제; NVMe PCIe bridge on */
	if (ret) /* PCI/NVMe: bridge 활성화 실패; NVMe 초기화 중단 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe setup 불가 */

	tmp = readl(base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 읽기; NVMe SerDes IDDQ 상태 확인 */
	if (is_bmips(pcie)) /* PCI/NVMe: bmips; NVMe SerDes IDDQ 비트 위치 다름 */
		tmp &= ~PCIE_BMIPS_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK; /* PCI/NVMe: bmips SerDes IDDQ 클리어; NVMe PHY 전원 on */
	else /* PCI/NVMe: non-bmips; NVMe SerDes IDDQ 클리어 */
		tmp &= ~PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK; /* PCI/NVMe: SerDes IDDQ 클리어; NVMe PHY 전원 on */
	writel(tmp, base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 기록; NVMe PHY 전원 활성화 */
	/* Wait for SerDes to be stable */
	usleep_range(100, 200); /* PCI/NVMe: SerDes 안정화; NVMe PHY 클록 안정 */

	/*
	 * SCB_MAX_BURST_SIZE is a two bit field.  For GENERIC chips it
	 * is encoded as 0=128, 1=256, 2=512, 3=Rsvd, for BCM7278 it
	 * is encoded as 0=Rsvd, 1=128, 2=256, 3=512.
	 */
	if (is_bmips(pcie)) /* PCI/NVMe: bmips; NVMe burst 256B */
		burst = 0x1; /* 256 bytes */
	else if (pcie->cfg->soc_base == BCM2711) /* PCI/NVMe: BCM2711; NVMe burst 128B */
		burst = 0x0; /* 128 bytes */
	else if (pcie->cfg->soc_base == BCM7278) /* PCI/NVMe: BCM7278; NVMe burst 512B */
		burst = 0x3; /* 512 bytes */
	else /* PCI/NVMe: 기타; NVMe burst 512B */
		burst = 0x2; /* 512 bytes */

	/*
	 * Set SCB_MAX_BURST_SIZE, CFG_READ_UR_MODE, SCB_ACCESS_EN,
	 * RCB_MPS_MODE, RCB_64B_MODE
	 */
	tmp = readl(base + PCIE_MISC_MISC_CTRL); /* PCI/NVMe: MISC_CTRL 읽기; NVMe 제어 필드 보존 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK); /* PCI/NVMe: SCB 접근 활성화; NVMe inbound DMA 허용 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK); /* PCI/NVMe: CFG Read UR 모드; NVMe 미연결 포트 UR 반환 */
	u32p_replace_bits(&tmp, burst, PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK); /* PCI/NVMe: burst 크기; NVMe DMA 처리량 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_PCIE_RCB_MPS_MODE_MASK); /* PCI/NVMe: RCB MPS 모드; NVMe Max Payload Size 설정 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_PCIE_RCB_64B_MODE_MASK); /* PCI/NVMe: RCB 64B 모드; NVMe 64비트 메모리 접근 */
	writel(tmp, base + PCIE_MISC_MISC_CTRL); /* PCI/NVMe: MISC_CTRL 기록; NVMe RC 핵심 설정 적용 */

	num_inbound_wins = brcm_pcie_get_inbound_wins(pcie, inbound_wins); /* PCI/NVMe: inbound window 구성; NVMe DMA 뷰포트 파싱 */
	if (num_inbound_wins < 0) /* PCI/NVMe: 구성 실패; NVMe DMA 설정 오류 */
		return num_inbound_wins; /* PCI/NVMe: 실패 반환; NVMe setup 불가 */

	set_inbound_win_registers(pcie, inbound_wins, num_inbound_wins); /* PCI/NVMe: inbound 레지스터 기록; NVMe DMA 주소 변환 활성화 */

	if (!brcm_pcie_rc_mode(pcie)) { /* PCI/NVMe: RC 모드 확인; NVMe EP 모드면 열거 불가 */
		dev_err(pcie->dev, "PCIe RC controller misconfigured as Endpoint\n"); /* PCI/NVMe: 에러; NVMe RC 모드 아님 */
		return -EINVAL; /* PCI/NVMe: 설정 오류; NVMe probe 실패 */
	}

	tmp = readl(base + PCIE_MISC_MISC_CTRL); /* PCI/NVMe: MISC_CTRL 재읽기; NVMe memc size 필드 설정 */
	for (memc = 0; memc < pcie->num_memc; memc++) { /* PCI/NVMe: memc 순회; NVMe DMA 뷰포트 memc 설정 */
		u32 scb_size_val = ilog2(pcie->memc_size[memc]) - 15; /* PCI/NVMe: memc 크기 인코딩; NVMe SCB size 필드 */

		if (memc == 0) /* PCI/NVMe: memc0; NVMe SCB0 size */
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(0));
		else if (memc == 1) /* PCI/NVMe: memc1; NVMe SCB1 size */
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(1));
		else if (memc == 2) /* PCI/NVMe: memc2; NVMe SCB2 size */
			u32p_replace_bits(&tmp, scb_size_val, SCB_SIZE_MASK(2));
	}
	writel(tmp, base + PCIE_MISC_MISC_CTRL); /* PCI/NVMe: MISC_CTRL 기록; NVMe memc DMA 뷰포트 적용 */

	/*
	 * We ideally want the MSI target address to be located in the 32bit
	 * addressable memory area. Some devices might depend on it. This is
	 * possible either when the inbound window is located above the lower
	 * 4GB or when the inbound area is smaller than 4GB (taking into
	 * account the rounding-up we're forced to perform).
	 */
	if (inbound_wins[2].pci_offset >= SZ_4G || /* PCI/NVMe: inbound 4GB 이상; NVMe 32비트 MSI 주소 사용 가능 */
	    (inbound_wins[2].size + inbound_wins[2].pci_offset) < SZ_4G) /* PCI/NVMe: inbound 4GB 미만; NVMe 32비트 MSI 주소 사용 가능 */
		pcie->msi_target_addr = BRCM_MSI_TARGET_ADDR_LT_4GB; /* PCI/NVMe: 4GB 미만 MSI 목적지; NVMe 32비트 MSI */
	else /* PCI/NVMe: 4GB 경계에 걸침; NVMe 64비트 MSI 목적지 필요 */
		pcie->msi_target_addr = BRCM_MSI_TARGET_ADDR_GT_4GB; /* PCI/NVMe: 4GB 초과 MSI 목적지; NVMe 64비트 MSI */


	/* Don't advertise L0s capability if 'aspm-no-l0s' */
	tmp = readl(base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* PCI/NVMe: link capability 읽기; NVMe ASPM L0s 광고 비트 */
	if (of_property_read_bool(pcie->np, "aspm-no-l0s")) /* PCI/NVMe: aspm-no-l0s 속성; NVMe L0s 비활성화 요청 */
		tmp &= ~PCI_EXP_LNKCAP_ASPM_L0S; /* PCI/NVMe: L0s 광고 클리어; NVMe ASPM L0s 비활성 */
	writel(tmp, base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* PCI/NVMe: capability 기록; NVMe ASPM L0s 설정 적용 */

	/* 'tmp' still holds the contents of PRIV1_LINK_CAPABILITY */
	num_lanes_cap = u32_get_bits(tmp, PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK); /* PCI/NVMe: 최대 링크 폭; NVMe 대역폭 */
	num_lanes = 0; /* PCI/NVMe: DT num-lanes 초기화; NVMe 기본 하드웨어 값 사용 */

	/*
	 * Use hardware negotiated Max Link Width value by default.  If the
	 * "num-lanes" DT property is present, assume that the chip's default
	 * link width capability information is incorrect/undesired and use the
	 * specified value instead.
	 */
	if (!of_property_read_u32(pcie->np, "num-lanes", &num_lanes) && /* PCI/NVMe: DT num-lanes 읽기; NVMe 링크 폭 제한 */
	    num_lanes && num_lanes <= 4 && num_lanes_cap != num_lanes) { /* PCI/NVMe: 유효하고 변경 필요; NVMe 링크 폭 강제 */
		u32p_replace_bits(&tmp, num_lanes, /* PCI/NVMe: 링크 폭 필드 갱신; NVMe x1/x4 등으로 제한 */
				PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_MAX_LINK_WIDTH_MASK);
		writel(tmp, base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY); /* PCI/NVMe: capability 기록; NVMe 링크 폭 광고 변경 */
		tmp = readl(base + PCIE_RC_PL_REG_PHY_CTL_1); /* PCI/NVMe: PHY CTL1 읽기; NVMe P2 powerdown 설정 */
		u32p_replace_bits(&tmp, 1, /* PCI/NVMe: P2 powerdown enable; NVMe 링크 폭 변경 시 전력 */
				PCIE_RC_PL_REG_PHY_CTL_1_REG_P2_POWERDOWN_ENA_NOSYNC_MASK);
		writel(tmp, base + PCIE_RC_PL_REG_PHY_CTL_1); /* PCI/NVMe: PHY CTL1 기록; NVMe PHY 전력 설정 */
	}

	/*
	 * For config space accesses on the RC, show the right class for
	 * a PCIe-PCIe bridge (the default setting is to be EP mode).
	 */
	tmp = readl(base + PCIE_RC_CFG_PRIV1_ID_VAL3); /* PCI/NVMe: ID_VAL3 읽기; NVMe class code 설정 */
	u32p_replace_bits(&tmp, 0x060400, /* PCI/NVMe: PCI bridge class code; NVMe RC가 PCIe-PCI bridge로 인식 */
			  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK);
	writel(tmp, base + PCIE_RC_CFG_PRIV1_ID_VAL3); /* PCI/NVMe: class code 기록; NVMe PCIe 계층 구조 표시 */

	bridge = pci_host_bridge_from_priv(pcie); /* PCI/NVMe: host bridge 획득; NVMe outbound windows 탐색 */
	resource_list_for_each_entry(entry, &bridge->windows) { /* PCI/NVMe: windows 순회; NVMe outbound 메모리 영역 탐색 */
		struct resource *res = entry->res; /* PCI/NVMe: resource; NVMe outbound 메모리 범위 */

		if (resource_type(res) != IORESOURCE_MEM) /* PCI/NVMe: MEM 타입만; NVMe 메모리 BAR 매핑 */
			continue; /* PCI/NVMe: IO 등 무시; NVMe MEM만 outbound 설정 */

		if (num_out_wins >= BRCM_NUM_PCIE_OUT_WINS) { /* PCI/NVMe: outbound window 초과; NVMe 메모리 영역 한도 */
			dev_err(pcie->dev, "too many outbound wins\n"); /* PCI/NVMe: 에러; NVMe outbound 자원 부족 */
			return -EINVAL; /* PCI/NVMe: 오류; NVMe setup 실패 */
		}

		if (is_bmips(pcie)) { /* PCI/NVMe: bmips; NVMe 128MB 단위로 분할 */
			u64 start = res->start; /* PCI/NVMe: 시작 주소; NVMe 128MB chunk */
			unsigned int j, nwins = resource_size(res) / SZ_128M; /* PCI/NVMe: 128MB 개수; NVMe bmips outbound 분할 수 */

			/* bmips PCIe outbound windows have a 128MB max size */
			if (nwins > BRCM_NUM_PCIE_OUT_WINS) /* PCI/NVMe: 최대 분할 수 제한; NVMe outbound window 한도 */
				nwins = BRCM_NUM_PCIE_OUT_WINS; /* PCI/NVMe: 한도 적용; NVMe 최대 4개 chunk */
			for (j = 0; j < nwins; j++, start += SZ_128M) /* PCI/NVMe: chunk 루프; NVMe 128MB씩 매핑 */
				brcm_pcie_set_outbound_win(pcie, j, start, /* PCI/NVMe: chunk별 outbound; NVMe 메모리 조각 매핑 */
							   start - entry->offset,
							   SZ_128M);
			break; /* PCI/NVMe: bmips는 첫 MEM 리소스만; NVMe outbound 완료 */
		}
		brcm_pcie_set_outbound_win(pcie, num_out_wins, res->start, /* PCI/NVMe: outbound window 설정; NVMe 메모리 BAR CPU->PCIe */
					   res->start - entry->offset,
					   resource_size(res));
		num_out_wins++; /* PCI/NVMe: outbound window 증가; NVMe 다음 window */
	}

	/* PCIe->SCB endian mode for inbound window */
	tmp = readl(base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1); /* PCI/NVMe: vendor specific reg1 읽기; NVMe BAR2 endian */
	u32p_replace_bits(&tmp, PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN, /* PCI/NVMe: little endian; NVMe CPU 메모리 일관성 */
			PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK);
	writel(tmp, base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1); /* PCI/NVMe: endian 설정; NVMe DMA 바이트 오더 일치 */

	if (pcie->cfg->post_setup) { /* PCI/NVMe: SoC별 후처리; NVMe PHY/L1SS 튜닝 */
		ret = pcie->cfg->post_setup(pcie); /* PCI/NVMe: 후처리 호출; NVMe SoC별 추가 설정 */
		if (ret < 0) /* PCI/NVMe: 후처리 실패; NVMe 초기화 중단 */
			return ret; /* PCI/NVMe: 실패 반환; NVMe setup 불가 */
	}

	return 0; /* PCI/NVMe: setup 성공; NVMe RC 기본 설정 완료 */
}

/*
 * This extends the timeout period for an access to an internal bus.  This
 * access timeout may occur during L1SS sleep periods, even without the
 * presence of a PCIe access.
 */
static void brcm_extend_rbus_timeout(struct brcm_pcie *pcie) /* PCI/NVMe: RBUS timeout 연장; NVMe L1SS 대기 상태 접근 안정화 */
{
	/* TIMEOUT register is two registers before RGR1_SW_INIT_1 */
	const unsigned int REG_OFFSET = PCIE_RGR1_SW_INIT_1(pcie) - 8; /* PCI/NVMe: timeout 레지스터 오프셋; NVMe RGR1 이전 */
	u32 timeout_us = 4000000; /* 4 seconds, our setting for L1SS */ /* PCI/NVMe: 4초 timeout; NVMe L1SS resume 지연 허용 */

	/* 7712 does not have this (RGR1) timer */
	if (pcie->cfg->soc_base == BCM7712) /* PCI/NVMe: BCM7712 제외; NVMe RBUS timeout 없음 */
		return; /* PCI/NVMe: early return; NVMe 설정 불필요 */

	/* Each unit in timeout register is 1/216,000,000 seconds */
	writel(216 * timeout_us, pcie->base + REG_OFFSET); /* PCI/NVMe: timeout 값 기록; NVMe L1SS 중 bus hang 방지 */
}

static void brcm_config_clkreq(struct brcm_pcie *pcie) /* PCI/NVMe: CLKREQ/ASPM 설정; NVMe 저전력/성능 튜닝 */
{
	static const char err_msg[] = "invalid 'brcm,clkreq-mode' DT string\n"; /* PCI/NVMe: 에러 메시지; NVMe DT 파싱 오류 */
	const char *mode = "default"; /* PCI/NVMe: 기본 모드; NVMe L1SS 활성화 */
	u32 clkreq_cntl; /* PCI/NVMe: CLKREQ 제어 임시값; NVMe ASPM 설정 */
	int ret, tmp; /* PCI/NVMe: 반환값/임시값; NVMe DT 읽기 및 L1SS 설정 */

	ret = of_property_read_string(pcie->np, "brcm,clkreq-mode", &mode); /* PCI/NVMe: clkreq-mode 읽기; NVMe ASPM 정책 */
	if (ret && ret != -EINVAL) { /* PCI/NVMe: 잘못된 문자열; NVMe 안전 모드로 폴백 */
		dev_err(pcie->dev, err_msg); /* PCI/NVMe: 에러 로그; NVMe ASPM 설정 오류 */
		mode = "safe"; /* PCI/NVMe: safe 모드; NVMe 절전 비활성 */
	}

	/* Start out assuming safe mode (both mode bits cleared) */
	clkreq_cntl = readl(pcie->base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 읽기; NVMe CLKREQ/L1SS 필드 보존 */
	clkreq_cntl &= ~PCIE_CLKREQ_MASK; /* PCI/NVMe: CLKREQ/L1SS 클리어; NVMe 기본 safe 상태 */

	if (strcmp(mode, "no-l1ss") == 0) { /* PCI/NVMe: no-l1ss 모드; NVMe L0s/L1만 허용 */
		/*
		 * "no-l1ss" -- Provides Clock Power Management, L0s, and
		 * L1, but cannot provide L1 substate (L1SS) power
		 * savings. If the downstream device connected to the RC is
		 * L1SS capable AND the OS enables L1SS, all PCIe traffic
		 * may abruptly halt, potentially hanging the system.
		 */
		clkreq_cntl |= PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK; /* PCI/NVMe: CLKREQ debug enable; NVMe CPM/L0s/L1 허용 */
		/*
		 * We want to un-advertise L1 substates because if the OS
		 * tries to configure the controller into using L1 substate
		 * power savings it may fail or hang when the RC HW is in
		 * "no-l1ss" mode.
		 */
		tmp = readl(pcie->base + PCIE_RC_CFG_PRIV1_ROOT_CAP); /* PCI/NVMe: ROOT_CAP 읽기; NVMe L1SS 광고 필드 */
		u32p_replace_bits(&tmp, 2, PCIE_RC_CFG_PRIV1_ROOT_CAP_L1SS_MODE_MASK); /* PCI/NVMe: L1SS 모드 2; NVMe L1SS 비광고 */
		writel(tmp, pcie->base + PCIE_RC_CFG_PRIV1_ROOT_CAP); /* PCI/NVMe: ROOT_CAP 기록; NVMe L1SS 비활성 */

	} else if (strcmp(mode, "default") == 0) { /* PCI/NVMe: default 모드; NVMe L0s/L1/L1SS 활성 */
		/*
		 * "default" -- Provides L0s, L1, and L1SS, but not
		 * compliant to provide Clock Power Management;
		 * specifically, may not be able to meet the Tclron max
		 * timing of 400ns as specified in "Dynamic Clock Control",
		 * section 3.2.5.2.2 of the PCIe spec.  This situation is
		 * atypical and should happen only with older devices.
		 */
		clkreq_cntl |= PCIE_MISC_HARD_PCIE_HARD_DEBUG_L1SS_ENABLE_MASK; /* PCI/NVMe: L1SS enable; NVMe L1SS 서브스테이트 활성 */
		brcm_extend_rbus_timeout(pcie); /* PCI/NVMe: RBUS timeout 연장; NVMe L1SS sleep 중 bus hang 방지 */

	} else { /* PCI/NVMe: safe 또는 기타; NVMe 절전 비활성 */
		/*
		 * "safe" -- No power savings; refclk is driven by RC
		 * unconditionally.
		 */
		if (strcmp(mode, "safe") != 0) /* PCI/NVMe: 알 수 없는 모드; NVMe 경고 */
			dev_err(pcie->dev, err_msg); /* PCI/NVMe: 에러 로그; NVMe ASPM 설정 오류 */
		mode = "safe"; /* PCI/NVMe: safe 모드 강제; NVMe 절전 끔 */
	}
	writel(clkreq_cntl, pcie->base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 기록; NVMe CLKREQ/L1SS 설정 적용 */

	dev_info(pcie->dev, "clkreq-mode set to %s\n", mode); /* PCI/NVMe: 모드 정보; NVMe ASPM 정책 로그 */
}

static int brcm_pcie_start_link(struct brcm_pcie *pcie) /* PCI/NVMe: PCIe 링크 시작; NVMe 컨트롤러 부팅 및 training */
{
	struct device *dev = pcie->dev; /* PCI/NVMe: device; NVMe 로그용 */
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe link status 레지스터 */
	u16 nlw, cls, lnksta; /* PCI/NVMe: negotiated link width/current link speed; NVMe 최종 링크 파라미터 */
	bool ssc_good = false; /* PCI/NVMe: SSC 성공 플래그; NVMe spread spectrum 상태 */
	int ret, i; /* PCI/NVMe: 반환값/루프; NVMe PERST/training */

	/* Limit the generation if specified */
	if (pcie->gen) /* PCI/NVMe: gen 제한 있음; NVMe 링크 속도 강제 */
		brcm_pcie_set_gen(pcie, pcie->gen); /* PCI/NVMe: link speed 제한; NVMe 세대 협상 상한 */

	/* Unassert the fundamental reset */
	ret = pcie->cfg->perst_set(pcie, 0); /* PCI/NVMe: PERST# deassert; NVMe 컨트롤러 리셋 해제 */
	if (ret) /* PCI/NVMe: PERST# 해제 실패; NVMe 장치 부팅 불가 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe 링크 업 불가 */

	msleep(PCIE_RESET_CONFIG_WAIT_MS); /* PCI/NVMe: 리셋 후 대기; NVMe 컨트롤러 구성 시간 */

	/*
	 * Give the RC/EP even more time to wake up, before trying to
	 * configure RC.  Intermittently check status for link-up, up to a
	 * total of 100ms.
	 */
	for (i = 0; i < 100 && !brcm_pcie_link_up(pcie); i += 5) /* PCI/NVMe: link up 폴링; NVMe 최대 100ms */
		msleep(5); /* PCI/NVMe: 5ms 대기; NVMe link training 진행 */

	if (!brcm_pcie_link_up(pcie)) { /* PCI/NVMe: 링크 다운; NVMe 장치 미응답 */
		dev_err(dev, "link down\n"); /* PCI/NVMe: 에러; NVMe 열거 실패 */
		return -ENODEV; /* PCI/NVMe: 장치 없음; NVMe probe 실패 */
	}

	brcm_config_clkreq(pcie); /* PCI/NVMe: CLKREQ/ASPM 설정; NVMe 저전력 정책 적용 */

	if (pcie->ssc) { /* PCI/NVMe: SSC 요청; NVMe spread spectrum 적용 */
		ret = brcm_pcie_set_ssc(pcie); /* PCI/NVMe: SSC 활성화; NVMe 클록 jitter 감소 */
		if (ret == 0) /* PCI/NVMe: SSC 성공; NVMe ssc_good 표시 */
			ssc_good = true; /* PCI/NVMe: SSC 활성; NVMe 링크 클록 안정 */
		else /* PCI/NVMe: SSC 실패; NVMe 로그만 기록 */
			dev_err(dev, "failed attempt to enter ssc mode\n"); /* PCI/NVMe: 에러; NVMe SSC 미적용 */
	}

	lnksta = readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA); /* PCI/NVMe: LINKSTA 읽기; NVMe 최종 링크 상태 */
	cls = FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta); /* PCI/NVMe: current link speed; NVMe PCIe 세대(2.5/5/8GT/s) */
	nlw = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta); /* PCI/NVMe: negotiated link width; NVMe x1/x4 등 */
	dev_info(dev, "link up, %s x%u %s\n", /* PCI/NVMe: 링크 정보 로그; NVMe 대역폭 확인 */
		 pci_speed_string(pcie_get_link_speed(cls)), nlw,
		 ssc_good ? "(SSC)" : "(!SSC)"); /* PCI/NVMe: SSC 상태 포함; NVMe 링크 품질 */

	return 0; /* PCI/NVMe: 링크 업 성공; NVMe 열거 진행 가능 */
}

static const char * const supplies[] = {
	"vpcie3v3", /* PCI/NVMe: 3.3V 레일; NVMe 슬롯 전원 */
	"vpcie3v3aux", /* PCI/NVMe: 3.3V aux; NVMe 보조 전원/웨이크업 */
	"vpcie12v", /* PCI/NVMe: 12V 레일; NVMe 슬롯 전원 */
};

static void *alloc_subdev_regulators(struct device *dev) /* PCI/NVMe: 하위 장치 레귤레이터 할당; NVMe 슬롯 전원 구조체 */
{
	const size_t size = sizeof(struct subdev_regulators) + /* PCI/NVMe: 기본 구조체 크기; NVMe */
		sizeof(struct regulator_bulk_data) * ARRAY_SIZE(supplies);
	struct subdev_regulators *sr; /* PCI/NVMe: 레귤레이터 구조체; NVMe 전원 공급 리스트 */
	int i; /* PCI/NVMe: 루프 인덱스; NVMe supply 이름 초기화 */

	sr = devm_kzalloc(dev, size, GFP_KERNEL); /* PCI/NVMe: 메모리 할당; NVMe 전원 구조체 */
	if (sr) { /* PCI/NVMe: 할당 성공; NVMe supply 초기화 */
		sr->num_supplies = ARRAY_SIZE(supplies); /* PCI/NVMe: 공급 개수; NVMe 3개 */
		for (i = 0; i < ARRAY_SIZE(supplies); i++) /* PCI/NVMe: supply 이름 설정; NVMe 각 레일 이름 */
			sr->supplies[i].supply = supplies[i]; /* PCI/NVMe: supply 문자열 할당; NVMe DT 매칭용 */
	}

	return sr; /* PCI/NVMe: 할당된 구조체; NVMe 전원 구조체 또는 NULL */
}

static int brcm_pcie_add_bus(struct pci_bus *bus) /* PCI/NVMe: 하위 bus 추가; NVMe 슬롯 전원 및 링크 시작 */
{
	struct brcm_pcie *pcie = bus->sysdata; /* PCI/NVMe: host bridge sysdata; NVMe RC 사설 데이터 */
	struct device *dev = &bus->dev; /* PCI/NVMe: bus device; NVMe regulator/로그용 */
	struct subdev_regulators *sr; /* PCI/NVMe: 레귤레이터 구조체; NVMe 전원 */
	int ret; /* PCI/NVMe: 반환값; NVMe regulator 결과 */

	if (!bus->parent || !pci_is_root_bus(bus->parent)) /* PCI/NVMe: root bus의 직계 자식만; NVMe 슬롯 단위 */
		return 0; /* PCI/NVMe: 하위 switch 버스 무시; NVMe 슬롯 전원은 root 직계에서만 */

	if (dev->of_node) { /* PCI/NVMe: DT node 있음; NVMe 슬롯별 전원 설정 */
		sr = alloc_subdev_regulators(dev); /* PCI/NVMe: regulator 구조체 할당; NVMe 슬롯 전원 */
		if (!sr) { /* PCI/NVMe: 할당 실패; NVMe 전원 없이 진행 */
			dev_info(dev, "Can't allocate regulators for downstream device\n"); /* PCI/NVMe: 정보; NVMe regulator 없음 */
			goto no_regulators; /* PCI/NVMe: regulator 없이 진행; NVMe 링크만 시작 */
		}

		pcie->sr = sr; /* PCI/NVMe: pcie에 연결; NVMe 전원 상태 추적 */

		ret = regulator_bulk_get(dev, sr->num_supplies, sr->supplies); /* PCI/NVMe: regulator 획득; NVMe 전원 리소스 */
		if (ret) { /* PCI/NVMe: 획득 실패; NVMe 전원 없음 */
			dev_info(dev, "Did not get regulators, err=%d\n", ret); /* PCI/NVMe: 정보; NVMe regulator 없음 */
			pcie->sr = NULL; /* PCI/NVMe: 연결 해제; NVMe 전원 미사용 */
			goto no_regulators; /* PCI/NVMe: regulator 없이 진행; NVMe 링크 시작 */
		}

		ret = regulator_bulk_enable(sr->num_supplies, sr->supplies); /* PCI/NVMe: regulator 활성화; NVMe 슬롯 전원 on */
		if (ret) { /* PCI/NVMe: 활성화 실패; NVMe 전원 on 불가 */
			dev_err(dev, "Can't enable regulators for downstream device\n"); /* PCI/NVMe: 에러; NVMe 슬롯 전원 실패 */
			regulator_bulk_free(sr->num_supplies, sr->supplies); /* PCI/NVMe: regulator 해제; NVMe 리소스 정리 */
			pcie->sr = NULL; /* PCI/NVMe: 연결 해제; NVMe 전원 미사용 */
		}
	}

no_regulators:
	brcm_pcie_start_link(pcie); /* PCI/NVMe: 링크 시작; NVMe 장치 열enum 전 PERST 해제/training */
	return 0; /* PCI/NVMe: 성공; NVMe bus 추가 완료 */
}

static void brcm_pcie_remove_bus(struct pci_bus *bus) /* PCI/NVMe: 하위 bus 제거; NVMe 슬롯 전원 off */
{
	struct brcm_pcie *pcie = bus->sysdata; /* PCI/NVMe: host bridge sysdata; NVMe RC 사설 데이터 */
	struct subdev_regulators *sr = pcie->sr; /* PCI/NVMe: regulator 구조체; NVMe 슬롯 전원 */
	struct device *dev = &bus->dev; /* PCI/NVMe: bus device; NVMe 로그용 */

	if (!sr || !bus->parent || !pci_is_root_bus(bus->parent)) /* PCI/NVMe: root 직계 슬롯만; NVMe regulator 없으면 skip */
		return; /* PCI/NVMe: 정리 불필요; NVMe early return */

	if (regulator_bulk_disable(sr->num_supplies, sr->supplies)) /* PCI/NVMe: regulator 비활성화; NVMe 슬롯 전원 off */
		dev_err(dev, "Failed to disable regulators for downstream device\n"); /* PCI/NVMe: 에러; NVMe 전원 off 실패 */
	regulator_bulk_free(sr->num_supplies, sr->supplies); /* PCI/NVMe: regulator 해제; NVMe 전원 리소스 반납 */
	pcie->sr = NULL; /* PCI/NVMe: 연결 해제; NVMe 전원 상태 초기화 */
}

/* L23 is a low-power PCIe link state */
static void brcm_pcie_enter_l23(struct brcm_pcie *pcie) /* PCI/NVMe: L23 저전력 링크 상태 진입; NVMe suspend/전원 off */
{
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe L23 제어 레지스터 */
	int l23, i; /* PCI/NVMe: L23 상태/루프; NVMe 저전력 진입 확인 */
	u32 tmp; /* PCI/NVMe: 레지스터 임시값; NVMe L23 요청 */

	/* Assert request for L23 */
	tmp = readl(base + PCIE_MISC_PCIE_CTRL); /* PCI/NVMe: PCIE_CTRL 읽기; NVMe L23 request 필드 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK); /* PCI/NVMe: L23 요청; NVMe 링크 저전력 진입 */
	writel(tmp, base + PCIE_MISC_PCIE_CTRL); /* PCI/NVMe: PCIE_CTRL 기록; NVMe L23 요청 전송 */

	/* Wait up to 36 msec for L23 */
	tmp = readl(base + PCIE_MISC_PCIE_STATUS); /* PCI/NVMe: STATUS 읽기; NVMe L23 상태 확인 */
	l23 = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK, tmp); /* PCI/NVMe: L23 비트 추출; NVMe 저전력 상태 */
	for (i = 0; i < 15 && !l23; i++) { /* PCI/NVMe: 최대 15회(36ms) 폴링; NVMe L23 진입 대기 */
		usleep_range(2000, 2400); /* PCI/NVMe: 2~2.4ms 대기; NVMe L23 전환 시간 */
		tmp = readl(base + PCIE_MISC_PCIE_STATUS); /* PCI/NVMe: STATUS 재읽기; NVMe L23 상태 */
		l23 = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK, /* PCI/NVMe: L23 비트 재추출; NVMe 저전력 상태 */
				tmp);
	}

	if (!l23) /* PCI/NVMe: L23 진입 실패; NVMe 저전력 전환 문제 */
		dev_err(pcie->dev, "failed to enter low-power link state\n"); /* PCI/NVMe: 에러; NVMe L23 실패 */
}

static int brcm_phy_cntl(struct brcm_pcie *pcie, const int start) /* PCI/NVMe: PHY 전원/리셋 제어; NVMe PHY on/off 시퀀스 */
{
	static const u32 shifts[PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS] = { /* PCI/NVMe: DAST 필드 시프트; NVMe PWRDN/RESET/DIG_RESET */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_SHIFT,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_SHIFT,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_SHIFT,};
	static const u32 masks[PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS] = { /* PCI/NVMe: DAST 필드 마스크; NVMe PWRDN/RESET/DIG_RESET */
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_PWRDN_MASK,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_RESET_MASK,
		PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_DIG_RESET_MASK,};
	const int beg = start ? 0 : PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS - 1; /* PCI/NVMe: 시작/끝 인덱스; NVMe on 순방향, off 역방향 */
	const int end = start ? PCIE_DVT_PMU_PCIE_PHY_CTRL_DAST_NFLDS : -1; /* PCI/NVMe: 종료 조건; NVMe 모든 필드 처리 */
	u32 tmp, combined_mask = 0; /* PCI/NVMe: 임시값/결합 마스크; NVMe PHY 상태 확인 */
	u32 val; /* PCI/NVMe: 필드 값; NVMe on=1, off=0 */
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe PHY 제어 레지스터 */
	int i, ret; /* PCI/NVMe: 루프/반환값; NVMe PHY 제어 */

	for (i = beg; i != end; start ? i++ : i--) { /* PCI/NVMe: DAST 필드 순차 처리; NVMe PHY 안정적 on/off */
		val = start ? BIT_MASK(shifts[i]) : 0; /* PCI/NVMe: on이면 비트 설정; NVMe off면 클리어 */
		tmp = readl(base + PCIE_DVT_PMU_PCIE_PHY_CTRL); /* PCI/NVMe: PHY_CTRL 읽기; NVMe 필드 보존 */
		tmp = (tmp & ~masks[i]) | (val & masks[i]); /* PCI/NVMe: 해당 필드만 갱신; NVMe PHY 단계별 제어 */
		writel(tmp, base + PCIE_DVT_PMU_PCIE_PHY_CTRL); /* PCI/NVMe: PHY_CTRL 기록; NVMe PHY 상태 변경 */
		usleep_range(50, 200); /* PCI/NVMe: 단계별 settling; NVMe PHY 전압/리셋 안정화 */
		combined_mask |= masks[i]; /* PCI/NVMe: 마스크 누적; NVMe 최종 상태 확인용 */
	}

	tmp = readl(base + PCIE_DVT_PMU_PCIE_PHY_CTRL); /* PCI/NVMe: PHY_CTRL 재읽기; NVMe 최종 상태 */
	val = start ? combined_mask : 0; /* PCI/NVMe: 기대값; NVMe on이면 모든 비트, off면 0 */

	ret = (tmp & combined_mask) == val ? 0 : -EIO; /* PCI/NVMe: 상태 검증; NVMe PHY 상태 불일치 시 오류 */
	if (ret) /* PCI/NVMe: 검증 실패; NVMe PHY on/off 오류 */
		dev_err(pcie->dev, "failed to %s phy\n", (start ? "start" : "stop")); /* PCI/NVMe: 에러; NVMe PHY 제어 실패 */

	return ret; /* PCI/NVMe: PHY 제어 결과; NVMe PHY on/off 성공/실패 */
}

static inline int brcm_phy_start(struct brcm_pcie *pcie) /* PCI/NVMe: PHY 시작 래퍼; NVMe PHY on */
{
	return pcie->cfg->has_phy ? brcm_phy_cntl(pcie, 1) : 0; /* PCI/NVMe: PHY 필요 시 on; NVMe PHY 준비 */
}

static inline int brcm_phy_stop(struct brcm_pcie *pcie) /* PCI/NVMe: PHY 정지 래퍼; NVMe PHY off */
{
	return pcie->cfg->has_phy ? brcm_phy_cntl(pcie, 0) : 0; /* PCI/NVMe: PHY 필요 시 off; NVMe PHY 전원 차단 */
}

static int brcm_pcie_turn_off(struct brcm_pcie *pcie) /* PCI/NVMe: PCIe off 시퀀스; NVMe 장치 종료/전원 차단 */
{
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe 전원/리셋 레지스터 */
	int tmp, ret; /* PCI/NVMe: 임시값/반환값; NVMe off 시퀀스 */

	if (brcm_pcie_link_up(pcie)) /* PCI/NVMe: 링크 업이면; NVMe 정상 종료 절차 */
		brcm_pcie_enter_l23(pcie); /* PCI/NVMe: L23 진입; NVMe 링크 저전력 상태 */
	/* Assert fundamental reset */
	ret = pcie->cfg->perst_set(pcie, 1); /* PCI/NVMe: PERST# assert; NVMe 장치 리셋 */
	if (ret) /* PCI/NVMe: PERST# 실패; NVMe off 중단 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe 종료 불완전 */

	/* Deassert request for L23 in case it was asserted */
	tmp = readl(base + PCIE_MISC_PCIE_CTRL); /* PCI/NVMe: PCIE_CTRL 읽기; NVMe L23 request 클리어 */
	u32p_replace_bits(&tmp, 0, PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK); /* PCI/NVMe: L23 요청 해제; NVMe 링크 완전 off 준비 */
	writel(tmp, base + PCIE_MISC_PCIE_CTRL); /* PCI/NVMe: PCIE_CTRL 기록; NVMe L23 request 클리어 */

	/* Turn off SerDes */
	tmp = readl(base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 읽기; NVMe SerDes IDDQ 제어 */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK); /* PCI/NVMe: SerDes IDDQ; NVMe PHY 전원 차단 */
	writel(tmp, base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 기록; NVMe PHY off */

	if (!(pcie->cfg->quirks & CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN)) /* PCI/NVMe: bridge shutdown 회피 퀴크 없음; NVMe bridge off 허용 */
		/* Shutdown PCIe bridge */
		ret = brcm_pcie_bridge_sw_init_set(pcie, 1); /* PCI/NVMe: bridge 리셋 assert; NVMe PCIe bridge off */

	return ret; /* PCI/NVMe: off 결과; NVMe PCIe 종료 완료 */
}

static int pci_dev_may_wakeup(struct pci_dev *dev, void *data) /* PCI/NVMe: wake-up 가능한 PCI 장치 확인; NVMe D3cold/suspend 대비 */
{
	bool *ret = data; /* PCI/NVMe: 결과 포인터; NVMe 웨이크업 장치 존재 여부 */

	if (device_may_wakeup(&dev->dev)) { /* PCI/NVMe: wake-up 설정된 장치; NVMe resume 이벤트 가능 */
		*ret = true; /* PCI/NVMe: wake-up 가능 표시; NVMe 전원 유지 */
		dev_info(&dev->dev, "Possible wake-up device; regulators will not be disabled\n"); /* PCI/NVMe: 정보; NVMe 전원 유지 */
	}
	return (int) *ret; /* PCI/NVMe: 이미 wake-up 발견 시 탐색 중단; NVMe 효율 */
}

static int brcm_pcie_suspend_noirq(struct device *dev) /* PCI/NVMe: suspend noirq; NVMe 시스템 절전 전 PCIe off */
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev); /* PCI/NVMe: driver data; NVMe RC 사설 데이터 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* PCI/NVMe: host bridge; NVMe 하위 버스 walk */
	int ret, rret; /* PCI/NVMe: 반환값/복구 반환값; NVMe suspend/복구 */

	ret = brcm_pcie_turn_off(pcie); /* PCI/NVMe: PCIe off; NVMe 링크/PHY/리셋 차단 */
	if (ret) /* PCI/NVMe: off 실패; NVMe suspend 중단 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe 절전 불가 */

	/*
	 * If brcm_phy_stop() returns an error, just dev_err(). If we
	 * return the error it will cause the suspend to fail and this is a
	 * forgivable offense that will probably be erased on resume.
	 */
	if (brcm_phy_stop(pcie)) /* PCI/NVMe: PHY 정지; NVMe PHY 전원 차단 */
		dev_err(dev, "Could not stop phy for suspend\n"); /* PCI/NVMe: 경고; NVMe suspend는 계속 */

	ret = reset_control_rearm(pcie->rescal); /* PCI/NVMe: rescal 리셋 재무장; NVMe resume 준비 */
	if (ret) { /* PCI/NVMe: rearm 실패; NVMe resume 문제 가능 */
		dev_err(dev, "Could not rearm rescal reset\n"); /* PCI/NVMe: 에러; NVMe suspend 실패 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe 절전 불가 */
	}

	if (pcie->sr) { /* PCI/NVMe: 슬롯 레귤레이터 있음; NVMe 전원 off 고려 */
		/*
		 * Now turn off the regulators, but if at least one
		 * downstream device is enabled as a wake-up source, do not
		 * turn off regulators.
		 */
		pcie->ep_wakeup_capable = false; /* PCI/NVMe: 웨이크업 가능 초기화; NVMe 탐색 전 */
		pci_walk_bus(bridge->bus, pci_dev_may_wakeup, /* PCI/NVMe: 버스 순회; NVMe wake-up 장치 탐색 */
			     &pcie->ep_wakeup_capable);
		if (!pcie->ep_wakeup_capable) { /* PCI/NVMe: wake-up 장치 없음; NVMe 전원 off 가능 */
			ret = regulator_bulk_disable(pcie->sr->num_supplies, /* PCI/NVMe: regulator 비활성화; NVMe 슬롯 전원 off */
						     pcie->sr->supplies);
			if (ret) { /* PCI/NVMe: 전원 off 실패; NVMe suspend 복구 */
				dev_err(dev, "Could not turn off regulators\n"); /* PCI/NVMe: 에러; NVMe 전원 off 실패 */
				rret = reset_control_reset(pcie->rescal); /* PCI/NVMe: rescal 리셋; NVMe 상태 복구 시도 */
				if (rret) /* PCI/NVMe: 복구 실패; NVMe 추가 오류 */
					dev_err(dev, "failed to reset 'rascal' controller ret=%d\n", /* PCI/NVMe: 에러; NVMe 복구 실패 */
						rret);
				return ret; /* PCI/NVMe: suspend 실패; NVMe 절전 불가 */
			}
		}
	}
	clk_disable_unprepare(pcie->clk); /* PCI/NVMe: 클럭 비활성화; NVMe PCIe 컨트롤러 클럭 off */

	return 0; /* PCI/NVMe: suspend 성공; NVMe 절전 완료 */
}

static int brcm_pcie_resume_noirq(struct device *dev) /* PCI/NVMe: resume noirq; NVMe 시스템 복귀 시 PCIe 재초기화 */
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev); /* PCI/NVMe: driver data; NVMe RC 사설 데이터 */
	void __iomem *base; /* PCI/NVMe: RC MMIO 베이스; NVMe 레지스터 접근 */
	u32 tmp; /* PCI/NVMe: 레지스터 임시값; NVMe SerDes/리셋 제어 */
	int ret, rret; /* PCI/NVMe: 반환값/복구 반환값; NVMe resume/rollback */

	base = pcie->base; /* PCI/NVMe: base 설정; NVMe 레지스터 접근 */
	ret = clk_prepare_enable(pcie->clk); /* PCI/NVMe: 클럭 활성화; NVMe PCIe 컨트롤러 클럭 on */
	if (ret) /* PCI/NVMe: 클럭 실패; NVMe resume 중단 */
		return ret; /* PCI/NVMe: 실패 반환; NVMe 복귀 불가 */

	ret = reset_control_reset(pcie->rescal); /* PCI/NVMe: rescal 리셋; NVMe PHY calibration 재시작 */
	if (ret) /* PCI/NVMe: rescal 실패; NVMe 복귀 실패 */
		goto err_disable_clk; /* PCI/NVMe: 클럭 off로 롤백; NVMe cleanup */

	ret = brcm_phy_start(pcie); /* PCI/NVMe: PHY 시작; NVMe PHY 전원/리셋 시퀀스 */
	if (ret) /* PCI/NVMe: PHY 시작 실패; NVMe 복귀 실패 */
		goto err_reset; /* PCI/NVMe: rescal rearm로 롤백; NVMe cleanup */

	/* Take bridge out of reset so we can access the SERDES reg */
	ret = brcm_pcie_bridge_sw_init_set(pcie, 0); /* PCI/NVMe: bridge 리셋 해제; NVMe 레지스터 접근 가능 */
	if (ret) /* PCI/NVMe: bridge 활성화 실패; NVMe 복귀 실패 */
		goto err_reset; /* PCI/NVMe: rescal rearm로 롤백; NVMe cleanup */

	/* SERDES_IDDQ = 0 */
	tmp = readl(base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 읽기; NVMe SerDes IDDQ 상태 */
	u32p_replace_bits(&tmp, 0, PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK); /* PCI/NVMe: SerDes IDDQ 클리어; NVMe PHY 전원 on */
	writel(tmp, base + HARD_DEBUG(pcie)); /* PCI/NVMe: HARD_DEBUG 기록; NVMe PHY 활성화 */

	/* wait for serdes to be stable */
	udelay(100); /* PCI/NVMe: SerDes 안정화; NVMe PHY 클록 settling */

	ret = brcm_pcie_setup(pcie); /* PCI/NVMe: PCIe RC 재설정; NVMe 링크/창/ASPM 복구 */
	if (ret) /* PCI/NVMe: setup 실패; NVMe 복귀 실패 */
		goto err_reset; /* PCI/NVMe: rescal rearm로 롤백; NVMe cleanup */

	if (pcie->sr) { /* PCI/NVMe: 슬롯 레귤레이터 있음; NVMe 전원 on */
		if (pcie->ep_wakeup_capable) { /* PCI/NVMe: wake-up 장치였음; NVMe 전원이 이미 on */
			/*
			 * We are resuming from a suspend.  In the suspend we
			 * did not disable the power supplies, so there is
			 * no need to enable them (and falsely increase their
			 * usage count).
			 */
			pcie->ep_wakeup_capable = false; /* PCI/NVMe: 플래그 클리어; NVMe 다음 suspend 준비 */
		} else { /* PCI/NVMe: 전원 off 되었음; NVMe 전원 on 필요 */
			ret = regulator_bulk_enable(pcie->sr->num_supplies, /* PCI/NVMe: regulator 활성화; NVMe 슬롯 전원 on */
						    pcie->sr->supplies);
			if (ret) { /* PCI/NVMe: 전원 on 실패; NVMe 복귀 실패 */
				dev_err(dev, "Could not turn on regulators\n"); /* PCI/NVMe: 에러; NVMe 전원 on 실패 */
				goto err_reset; /* PCI/NVMe: rescal rearm로 롤백; NVMe cleanup */
			}
		}
	}

	ret = brcm_pcie_start_link(pcie); /* PCI/NVMe: 링크 재시작; NVMe PERST 해제/training */
	if (ret) /* PCI/NVMe: 링크 실패; NVMe 복귀 실패 */
		goto err_regulator; /* PCI/NVMe: regulator off로 롤백; NVMe cleanup */

	if (pcie->msi) /* PCI/NVMe: MSI 있음; NVMe MSI 레지스터 복구 */
		brcm_msi_set_regs(pcie->msi); /* PCI/NVMe: MSI 레지스터 재설정; NVMe MSI 복귀 후 동작 */

	return 0; /* PCI/NVMe: resume 성공; NVMe 복귀 완료 */

err_regulator:
	if (pcie->sr) /* PCI/NVMe: 레귤레이터 있음; NVMe 전원 off cleanup */
		regulator_bulk_disable(pcie->sr->num_supplies, pcie->sr->supplies); /* PCI/NVMe: regulator 비활성화; NVMe 전원 off */
err_reset:
	rret = reset_control_rearm(pcie->rescal); /* PCI/NVMe: rescal rearm; NVMe 리셋 복구 */
	if (rret) /* PCI/NVMe: rearm 실패; NVMe 추가 오류 */
		dev_err(pcie->dev, "failed to rearm 'rescal' reset, err=%d\n", rret); /* PCI/NVMe: 에러; NVMe 복구 실패 */
err_disable_clk:
	clk_disable_unprepare(pcie->clk); /* PCI/NVMe: 클럭 off; NVMe PCIe 컨트롤러 클럭 비활성 */
	return ret; /* PCI/NVMe: resume 실패; NVMe 복귀 불가 */
}

/* Dump out PCIe errors on die or panic */
static int brcm_pcie_dump_err(struct brcm_pcie *pcie,
			       const char *type) /* PCI/NVMe: PCIe 오류 덤프; NVMe AER/치명 오류 시 디버깅 */
{
	void __iomem *base = pcie->base; /* PCI/NVMe: RC MMIO 베이스; NVMe 오류 레지스터 접근 */
	int i, is_cfg_err, is_mem_err, lanes; /* PCI/NVMe: 루프/오류 플래그/바이트레인; NVMe 오류 분석 */
	const char *width_str, *direction_str; /* PCI/NVMe: width/방향 문자열; NVMe 오류 로그 */
	u32 info, cfg_addr, cfg_cause, mem_cause, lo, hi; /* PCI/NVMe: 오류 레지스터; NVMe config/mem 오류 상세 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* PCI/NVMe: host bridge; NVMe domain/bus 정보 */
	unsigned long flags; /* PCI/NVMe: irqsave 플래그; NVMe bridge_lock 보호 */
	char lanes_str[9]; /* PCI/NVMe: 바이트 레인 문자열; NVMe 오류 TLP BE */

	spin_lock_irqsave(&pcie->bridge_lock, flags); /* PCI/NVMe: bridge lock 획득; NVMe 오류 레지스터 보호 */
	/* Don't access registers when the bridge is off */
	if (pcie->bridge_in_reset || readl(base + PCIE_OUTB_ERR_VALID) == 0) { /* PCI/NVMe: bridge off 또는 오류 없음; NVMe 덤프 불필요 */
		spin_unlock_irqrestore(&pcie->bridge_lock, flags); /* PCI/NVMe: lock 해제; NVMe */
		return NOTIFY_DONE; /* PCI/NVMe: 처리 완료; NVMe 덤프 skip */
	}

	/* Read all necessary registers so we can release the spinlock ASAP */
	info = readl(base + PCIE_OUTB_ERR_ACC_INFO); /* PCI/NVMe: 오류 접근 정보; NVMe 오류 유형/방향 */
	is_cfg_err = !!(info & PCIE_OUTB_ERR_ACC_INFO_CFG_ERR); /* PCI/NVMe: config 오류 여부; NVMe PCIe config 접근 실패 */
	is_mem_err = !!(info & PCIE_OUTB_ERR_ACC_INFO_MEM_ERR); /* PCI/NVMe: mem 오류 여부; NVMe DMA/메모리 접근 실패 */
	if (is_cfg_err) { /* PCI/NVMe: config 오류; NVMe BDF/레지스터 추적 */
		cfg_addr = readl(base + PCIE_OUTB_ERR_ACC_ADDR); /* PCI/NVMe: 오류 config 주소; NVMe BDF+offset */
		cfg_cause = readl(base + PCIE_OUTB_ERR_CFG_CAUSE); /* PCI/NVMe: config 오류 원인; NVMe timeout/abort 등 */
	}
	if (is_mem_err) { /* PCI/NVMe: mem 오류; NVMe DMA 주소/원인 추적 */
		mem_cause = readl(base + PCIE_OUTB_ERR_MEM_CAUSE); /* PCI/NVMe: mem 오류 원인; NVMe timeout/abort 등 */
		lo = readl(base + PCIE_OUTB_ERR_MEM_ADDR_LO); /* PCI/NVMe: mem 오류 주소 low; NVMe DMA 실패 주소 */
		hi = readl(base + PCIE_OUTB_ERR_MEM_ADDR_HI); /* PCI/NVMe: mem 오류 주소 high; NVMe 64비트 DMA 실패 주소 */
	}
	/* We've got all of the info, clear the error */
	writel(1, base + PCIE_OUTB_ERR_CLEAR); /* PCI/NVMe: 오류 클리어; NVMe 오류 레지스터 리셋 */
	spin_unlock_irqrestore(&pcie->bridge_lock, flags); /* PCI/NVMe: lock 해제; NVMe */

	dev_err(pcie->dev, "reporting PCIe info which may be related to %s error\n", /* PCI/NVMe: 오류 로그 헤더; NVMe die/panic 유형 */
		type);
	width_str = (info & PCIE_OUTB_ERR_ACC_INFO_TYPE_64) ? "64bit" : "32bit"; /* PCI/NVMe: 접근 width; NVMe 32/64비트 TLP */
	direction_str = str_read_write(!(info & PCIE_OUTB_ERR_ACC_INFO_DIR_WRITE)); /* PCI/NVMe: 접근 방향; NVMe read/write */
	lanes = FIELD_GET(PCIE_OUTB_ERR_ACC_INFO_BYTE_LANES, info); /* PCI/NVMe: 바이트 레인; NVMe TLP byte enable */
	for (i = 0, lanes_str[8] = 0; i < 8; i++) /* PCI/NVMe: 8비트 레인 문자열; NVMe BE 로그 */
		lanes_str[i] = (lanes & (1 << i)) ? '1' : '0'; /* PCI/NVMe: 각 레인 상태; NVMe BE 비트 표시 */

	if (is_cfg_err) { /* PCI/NVMe: config 오류 상세; NVMe PCIe 설정 접근 실패 */
		int bus = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_BUS, cfg_addr); /* PCI/NVMe: 버스 번호; NVMe 오류 장치 버스 */
		int dev = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_DEV, cfg_addr); /* PCI/NVMe: 디바이스 번호; NVMe 오류 장치 번호 */
		int func = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_FUNC, cfg_addr); /* PCI/NVMe: 기능 번호; NVMe 오류 기능 번호 */
		int reg = FIELD_GET(PCIE_OUTB_ERR_ACC_ADDR_REG, cfg_addr); /* PCI/NVMe: 레지스터 오프셋; NVMe PCIe capability offset */

		dev_err(pcie->dev, "Error: CFG Acc, %s, %s (%04x:%02x:%02x.%d) reg=0x%x, lanes=%s\n", /* PCI/NVMe: config 오류 로그; NVMe BDF/레지스터 */
			width_str, direction_str, bridge->domain_nr, bus, dev,
			func, reg, lanes_str);
		dev_err(pcie->dev, " Type: TO=%d Abt=%d UnsupReq=%d AccTO=%d AccDsbld=%d Acc64bit=%d\n", /* PCI/NVMe: config 오류 원인; NVMe timeout/abort/UR 등 */
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_TIMEOUT),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ABORT),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_UNSUPP_REQ),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ACC_TIMEOUT),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ACC_DISABLED),
			!!(cfg_cause & PCIE_OUTB_ERR_CFG_CAUSE_ACC_64BIT));
	}

	if (is_mem_err) { /* PCI/NVMe: mem 오류 상세; NVMe DMA 실패 */
		u64 addr = ((u64)hi << 32) | (u64)lo; /* PCI/NVMe: 64비트 주소 조합; NVMe DMA 실패 주소 */

		dev_err(pcie->dev, "Error: Mem Acc, %s, %s, @0x%llx, lanes=%s\n", /* PCI/NVMe: mem 오류 로그; NVMe DMA 주소/방향 */
			width_str, direction_str, addr, lanes_str);
		dev_err(pcie->dev, " Type: TO=%d Abt=%d UnsupReq=%d AccDsble=%d BadAddr=%d\n", /* PCI/NVMe: mem 오류 원인; NVMe timeout/abort/UR 등 */
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_TIMEOUT),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_ABORT),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_UNSUPP_REQ),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_ACC_DISABLED),
			!!(mem_cause & PCIE_OUTB_ERR_MEM_CAUSE_BAD_ADDR));
	}

	return NOTIFY_DONE; /* PCI/NVMe: notifier 완료; NVMe 오류 덤프 종료 */
}

static int brcm_pcie_die_notify_cb(struct notifier_block *self,
				   unsigned long v, void *p) /* PCI/NVMe: die notifier 콜백; NVMe 시스템 die 시 PCIe 오류 덤프 */
{
	struct brcm_pcie *pcie = /* PCI/NVMe: notifier에서 pcie 획득; NVMe RC 사설 데이터 */
		container_of(self, struct brcm_pcie, die_notifier);

	return brcm_pcie_dump_err(pcie, "Die"); /* PCI/NVMe: die 유형으로 덤프; NVMe 치명 오류 기록 */
}

static int brcm_pcie_panic_notify_cb(struct notifier_block *self,
				     unsigned long v, void *p) /* PCI/NVMe: panic notifier 콜백; NVMe 시스템 panic 시 PCIe 오류 덤프 */
{
	struct brcm_pcie *pcie = /* PCI/NVMe: notifier에서 pcie 획득; NVMe RC 사설 데이터 */
		container_of(self, struct brcm_pcie, panic_notifier);

	return brcm_pcie_dump_err(pcie, "Panic"); /* PCI/NVMe: panic 유형으로 덤프; NVMe 치명 오류 기록 */
}

static void brcm_register_die_notifiers(struct brcm_pcie *pcie) /* PCI/NVMe: die/panic notifier 등록; NVMe 치명 오류 시 PCIe 덤프 */
{
	pcie->panic_notifier.notifier_call = brcm_pcie_panic_notify_cb; /* PCI/NVMe: panic 콜백 설정; NVMe panic 시 덤프 */
	atomic_notifier_chain_register(&panic_notifier_list, /* PCI/NVMe: panic notifier chain 등록; NVMe 시스템 패닉 감시 */
				       &pcie->panic_notifier);

	pcie->die_notifier.notifier_call = brcm_pcie_die_notify_cb; /* PCI/NVMe: die 콜백 설정; NVMe die 시 덤프 */
	register_die_notifier(&pcie->die_notifier); /* PCI/NVMe: die notifier 등록; NVMe 시스템 die 감시 */
}

static void brcm_unregister_die_notifiers(struct brcm_pcie *pcie) /* PCI/NVMe: die/panic notifier 해제; NVMe 드라이버 제거 시 */
{
	unregister_die_notifier(&pcie->die_notifier); /* PCI/NVMe: die notifier 해제; NVMe die 감시 중단 */
	atomic_notifier_chain_unregister(&panic_notifier_list, /* PCI/NVMe: panic notifier chain 해제; NVMe panic 감시 중단 */
					 &pcie->panic_notifier);
}

static void __brcm_pcie_remove(struct brcm_pcie *pcie) /* PCI/NVMe: 내부 제거; NVMe 드라이버 제거/초기화 실패 시 */
{
	brcm_msi_remove(pcie); /* PCI/NVMe: MSI 제거; NVMe MSI 인프라 해체 */
	brcm_pcie_turn_off(pcie); /* PCI/NVMe: PCIe off; NVMe 링크/PHY/리셋 차단 */
	if (brcm_phy_stop(pcie)) /* PCI/NVMe: PHY 정지; NVMe PHY off */
		dev_err(pcie->dev, "Could not stop phy\n"); /* PCI/NVMe: 에러; NVMe PHY off 실패 */
	if (reset_control_rearm(pcie->rescal)) /* PCI/NVMe: rescal rearm; NVMe 리셋 복구 */
		dev_err(pcie->dev, "Could not rearm rescal reset\n"); /* PCI/NVMe: 에러; NVMe rescal 복구 실패 */
	clk_disable_unprepare(pcie->clk); /* PCI/NVMe: 클럭 off; NVMe PCIe 컨트롤러 클럭 비활성 */
}

static void brcm_pcie_remove(struct platform_device *pdev) /* PCI/NVMe: platform remove; NVMe 드라이버 unload 시 */
{
	struct brcm_pcie *pcie = platform_get_drvdata(pdev); /* PCI/NVMe: driver data; NVMe RC 사설 데이터 */
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(pcie); /* PCI/NVMe: host bridge; NVMe root bus 접근 */

	pci_stop_root_bus(bridge->bus); /* PCI/NVMe: root bus 정지; NVMe 하위 장치 접근 중단 */
	pci_remove_root_bus(bridge->bus); /* PCI/NVMe: root bus 제거; NVMe PCIe 장치 unbind */
	if (pcie->cfg->has_err_report) /* PCI/NVMe: 오류 보고 기능; NVMe notifier 해제 */
		brcm_unregister_die_notifiers(pcie); /* PCI/NVMe: die/panic notifier 해제; NVMe 시스템 감시 중단 */

	__brcm_pcie_remove(pcie); /* PCI/NVMe: 내부 제거; NVMe PCIe/PHY/클럭 off */
}

static const int pcie_offsets[] = {
	[RGR1_SW_INIT_1]	= 0x9210, /* PCI/NVMe: generic RGR1 offset; NVMe bridge/PERST 제어 */
	[EXT_CFG_INDEX]		= 0x9000, /* PCI/NVMe: generic config index offset; NVMe BDF 선택 */
	[EXT_CFG_DATA]		= 0x8000, /* PCI/NVMe: generic config data offset; NVMe config read/write */
	[PCIE_HARD_DEBUG]	= 0x4204, /* PCI/NVMe: generic hard debug offset; NVMe SerDes/CLKREQ/L1SS */
	[PCIE_INTR2_CPU_BASE]	= 0x4300, /* PCI/NVMe: generic CPU INTR2 offset; NVMe 레거시 MSI 공유 */
};

static const int pcie_offsets_bcm7278[] = {
	[RGR1_SW_INIT_1]	= 0xc010, /* PCI/NVMe: 7278 RGR1 offset; NVMe bridge/PERST 제어(7278) */
	[EXT_CFG_INDEX]		= 0x9000, /* PCI/NVMe: 7278 config index offset; NVMe BDF 선택 */
	[EXT_CFG_DATA]		= 0x8000, /* PCI/NVMe: 7278 config data offset; NVMe config read/write */
	[PCIE_HARD_DEBUG]	= 0x4204, /* PCI/NVMe: 7278 hard debug offset; NVMe SerDes/CLKREQ/L1SS */
	[PCIE_INTR2_CPU_BASE]	= 0x4300, /* PCI/NVMe: 7278 CPU INTR2 offset; NVMe 레거시 MSI 공유 */
};

static const int pcie_offsets_bcm7425[] = {
	[RGR1_SW_INIT_1]	= 0x8010, /* PCI/NVMe: 7425 RGR1 offset; NVMe bridge/PERST 제어 */
	[EXT_CFG_INDEX]		= 0x8300, /* PCI/NVMe: 7425 config index offset; NVMe BDF+offset 선택 */
	[EXT_CFG_DATA]		= 0x8304, /* PCI/NVMe: 7425 config data offset; NVMe 32비트 config read/write */
	[PCIE_HARD_DEBUG]	= 0x4204, /* PCI/NVMe: 7425 hard debug offset; NVMe SerDes/CLKREQ/L1SS */
	[PCIE_INTR2_CPU_BASE]	= 0x4300, /* PCI/NVMe: 7425 CPU INTR2 offset; NVMe 레거시 MSI 공유 */
};

static const int pcie_offsets_bcm7712[] = {
	[RGR1_SW_INIT_1]	= 0x9210, /* PCI/NVMe: 7712 RGR1 offset; NVMe bridge/PERST 제어 */
	[EXT_CFG_INDEX]		= 0x9000, /* PCI/NVMe: 7712 config index offset; NVMe BDF 선택 */
	[EXT_CFG_DATA]		= 0x8000, /* PCI/NVMe: 7712 config data offset; NVMe config read/write */
	[PCIE_HARD_DEBUG]	= 0x4304, /* PCI/NVMe: 7712 hard debug offset; NVMe SerDes/CLKREQ/L1SS */
	[PCIE_INTR2_CPU_BASE]	= 0x4400, /* PCI/NVMe: 7712 CPU INTR2 offset; NVMe 레거시 MSI 공유 */
};

static const struct pcie_cfg_data generic_cfg = {
	.offsets	= pcie_offsets, /* PCI/NVMe: generic offset 테이블; NVMe 기본 SoC */
	.soc_base	= GENERIC, /* PCI/NVMe: GENERIC; NVMe 기본 동작 */
	.perst_set	= brcm_pcie_perst_set_generic, /* PCI/NVMe: generic PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* PCI/NVMe: generic bridge init; NVMe bridge reset */
	.num_inbound_wins = 3, /* PCI/NVMe: inbound window 3개; NVMe 기본 DMA 뷰포트 */
};

static const struct pcie_cfg_data bcm2711_cfg = {
	.offsets	= pcie_offsets, /* PCI/NVMe: BCM2711 offset 테이블; NVMe Raspberry Pi 4 */
	.soc_base	= BCM2711, /* PCI/NVMe: BCM2711; NVMe 3GB 메모리 제약 */
	.perst_set	= brcm_pcie_perst_set_generic, /* PCI/NVMe: generic PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* PCI/NVMe: generic bridge init; NVMe bridge reset */
	.num_inbound_wins = 3, /* PCI/NVMe: inbound window 3개; NVMe 기본 DMA 뷰포트 */
};

static const struct pcie_cfg_data bcm2712_cfg = {
	.offsets	= pcie_offsets_bcm7712, /* PCI/NVMe: BCM2712 offset 테이블; NVMe Raspberry Pi 5 */
	.soc_base	= BCM7712, /* PCI/NVMe: BCM7712; NVMe 다중 inbound window */
	.perst_set	= brcm_pcie_perst_set_7278, /* PCI/NVMe: 7278-style PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* PCI/NVMe: generic bridge init; NVMe bridge reset */
	.post_setup	= brcm_pcie_post_setup_bcm2712, /* PCI/NVMe: BCM2712 후처리; NVMe PHY/L1SS 튜닝 */
	.quirks		= CFG_QUIRK_AVOID_BRIDGE_SHUTDOWN, /* PCI/NVMe: bridge shutdown 회피; NVMe RESCAL 충돌 방지 */
	.num_inbound_wins = 10, /* PCI/NVMe: inbound window 10개; NVMe 다중 DMA 뷰포트 */
};

static const struct pcie_cfg_data bcm4908_cfg = {
	.offsets	= pcie_offsets, /* PCI/NVMe: BCM4908 offset 테이블; NVMe PERST# reset control 사용 */
	.soc_base	= BCM4908, /* PCI/NVMe: BCM4908; NVMe PERST# reset controller */
	.perst_set	= brcm_pcie_perst_set_4908, /* PCI/NVMe: BCM4908 PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* PCI/NVMe: generic bridge init; NVMe bridge reset */
	.num_inbound_wins = 3, /* PCI/NVMe: inbound window 3개; NVMe 기본 DMA 뷰포트 */
};

static const struct pcie_cfg_data bcm7278_cfg = {
	.offsets	= pcie_offsets_bcm7278, /* PCI/NVMe: BCM7278 offset 테이블; NVMe bridge init 상이 */
	.soc_base	= BCM7278, /* PCI/NVMe: BCM7278; NVMe 512B burst/bridge init */
	.perst_set	= brcm_pcie_perst_set_7278, /* PCI/NVMe: 7278 PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_7278, /* PCI/NVMe: 7278 bridge init; NVMe bridge reset */
	.num_inbound_wins = 3, /* PCI/NVMe: inbound window 3개; NVMe 기본 DMA 뷰포트 */
};

static const struct pcie_cfg_data bcm7425_cfg = {
	.offsets	= pcie_offsets_bcm7425, /* PCI/NVMe: BCM7425 offset 테이블; NVMe 32비트 config 접근 */
	.soc_base	= BCM7425, /* PCI/NVMe: BCM7425; NVMe bmips/32비트 config */
	.perst_set	= brcm_pcie_perst_set_generic, /* PCI/NVMe: generic PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* PCI/NVMe: generic bridge init; NVMe bridge reset */
	.num_inbound_wins = 3, /* PCI/NVMe: inbound window 3개; NVMe 기본 DMA 뷰포트 */
};

static const struct pcie_cfg_data bcm7435_cfg = {
	.offsets	= pcie_offsets, /* PCI/NVMe: BCM7435 offset 테이블; NVMe bmips 동작 */
	.soc_base	= BCM7435, /* PCI/NVMe: BCM7435; NVMe bmips/128MB outbound */
	.perst_set	= brcm_pcie_perst_set_generic, /* PCI/NVMe: generic PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* PCI/NVMe: generic bridge init; NVMe bridge reset */
	.num_inbound_wins = 3, /* PCI/NVMe: inbound window 3개; NVMe 기본 DMA 뷰포트 */
};

static const struct pcie_cfg_data bcm7216_cfg = {
	.offsets	= pcie_offsets_bcm7278, /* PCI/NVMe: BCM7216 offset 테이블; NVMe 7278 기반 */
	.soc_base	= BCM7278, /* PCI/NVMe: BCM7278; NVMe 7278 동작 */
	.perst_set	= brcm_pcie_perst_set_7278, /* PCI/NVMe: 7278 PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_7278, /* PCI/NVMe: 7278 bridge init; NVMe bridge reset */
	.has_phy	= true, /* PCI/NVMe: PHY 제어 있음; NVMe PHY 전원/리셋 시퀀스 */
	.num_inbound_wins = 3, /* PCI/NVMe: inbound window 3개; NVMe 기본 DMA 뷰포트 */
	.has_err_report = true, /* PCI/NVMe: outbound 오류 보고; NVMe AER/오류 덤프 */
};

static const struct pcie_cfg_data bcm7712_cfg = {
	.offsets	= pcie_offsets_bcm7712, /* PCI/NVMe: BCM7712 offset 테이블; NVMe 다중 inbound window */
	.perst_set	= brcm_pcie_perst_set_7278, /* PCI/NVMe: 7278-style PERST#; NVMe fundamental reset */
	.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic, /* PCI/NVMe: generic bridge init; NVMe bridge reset */
	.soc_base	= BCM7712, /* PCI/NVMe: BCM7712; NVMe 다중 DMA 뷰포트/UBUS remap */
	.num_inbound_wins = 10, /* PCI/NVMe: inbound window 10개; NVMe 다중 DMA 뷰포트 */
};

static const struct of_device_id brcm_pcie_match[] = {
	{ .compatible = "brcm,bcm2711-pcie", .data = &bcm2711_cfg }, /* PCI/NVMe: Raspberry Pi 4; NVMe BCM2711 매칭 */
	{ .compatible = "brcm,bcm2712-pcie", .data = &bcm2712_cfg }, /* PCI/NVMe: Raspberry Pi 5; NVMe BCM2712 매칭 */
	{ .compatible = "brcm,bcm4908-pcie", .data = &bcm4908_cfg }, /* PCI/NVMe: BCM4908; NVMe 매칭 */
	{ .compatible = "brcm,bcm7211-pcie", .data = &generic_cfg }, /* PCI/NVMe: BCM7211; NVMe generic 매칭 */
	{ .compatible = "brcm,bcm7216-pcie", .data = &bcm7216_cfg }, /* PCI/NVMe: BCM7216; NVMe 7278+PHY 매칭 */
	{ .compatible = "brcm,bcm7278-pcie", .data = &bcm7278_cfg }, /* PCI/NVMe: BCM7278; NVMe 매칭 */
	{ .compatible = "brcm,bcm7425-pcie", .data = &bcm7425_cfg }, /* PCI/NVMe: BCM7425; NVMe 32비트 config 매칭 */
	{ .compatible = "brcm,bcm7435-pcie", .data = &bcm7435_cfg }, /* PCI/NVMe: BCM7435; NVMe bmips 매칭 */
	{ .compatible = "brcm,bcm7445-pcie", .data = &generic_cfg }, /* PCI/NVMe: BCM7445; NVMe generic 매칭 */
	{ .compatible = "brcm,bcm7712-pcie", .data = &bcm7712_cfg }, /* PCI/NVMe: BCM7712; NVMe 다중 inbound 매칭 */
	{}, /* PCI/NVMe: 테이블 종료; NVMe */
};

static struct pci_ops brcm_pcie_ops = {
	.map_bus = brcm_pcie_map_bus, /* PCI/NVMe: config space 버스 매핑; NVMe ECAM/config 접근 */
	.read = pci_generic_config_read, /* PCI/NVMe: generic config read; NVMe PCIe capability/BAR 읽기 */
	.write = pci_generic_config_write, /* PCI/NVMe: generic config write; NVMe PCIe capability/BAR 쓰기 */
	.add_bus = brcm_pcie_add_bus, /* PCI/NVMe: bus 추가; NVMe 슬롯 전원/링크 시작 */
	.remove_bus = brcm_pcie_remove_bus, /* PCI/NVMe: bus 제거; NVMe 슬롯 전원 off */
};

static struct pci_ops brcm7425_pcie_ops = {
	.map_bus = brcm7425_pcie_map_bus, /* PCI/NVMe: 7425 config 버스 매핑; NVMe 32비트 config 접근 */
	.read = pci_generic_config_read32, /* PCI/NVMe: 32비트 config read; NVMe 7425 PCIe 레지스터 */
	.write = pci_generic_config_write32, /* PCI/NVMe: 32비트 config write; NVMe 7425 PCIe 레지스터 */
	.add_bus = brcm_pcie_add_bus, /* PCI/NVMe: bus 추가; NVMe 슬롯 전원/링크 시작 */
	.remove_bus = brcm_pcie_remove_bus, /* PCI/NVMe: bus 제거; NVMe 슬롯 전원 off */
};

static int brcm_pcie_probe(struct platform_device *pdev) /* PCI/NVMe: platform probe; NVMe PCIe RC 초기화 및 host bridge 등록 */
{
	struct device_node *np = pdev->dev.of_node; /* PCI/NVMe: DT node; NVMe 리소스/속성 파싱 */
	struct pci_host_bridge *bridge; /* PCI/NVMe: host bridge; NVMe PCIe 계층 등록 */
	const struct pcie_cfg_data *data; /* PCI/NVMe: SoC별 설정; NVMe 매칭 데이터 */
	struct brcm_pcie *pcie; /* PCI/NVMe: RC 사설 데이터; NVMe sysdata */
	int ret; /* PCI/NVMe: 반환값; NVMe probe 결과 */

	bridge = devm_pci_alloc_host_bridge(&pdev->dev, sizeof(*pcie)); /* PCI/NVMe: host bridge 할당; NVMe RC private 공간 포함 */
	if (!bridge) /* PCI/NVMe: 할당 실패; NVMe probe 메모리 부족 */
		return -ENOMEM; /* PCI/NVMe: 메모리 부족; NVMe 초기화 불가 */

	data = of_device_get_match_data(&pdev->dev); /* PCI/NVMe: compatible 매칭 데이터; NVMe SoC별 cfg 획득 */
	if (!data) { /* PCI/NVMe: 매칭 실패; NVMe unsupported compatible */
		pr_err("failed to look up compatible string\n"); /* PCI/NVMe: 에러; NVMe DT 매칭 실패 */
		return -EINVAL; /* PCI/NVMe: 잘못된 인자; NVMe probe 실패 */
	}

	pcie = pci_host_bridge_priv(bridge); /* PCI/NVMe: host bridge private; NVMe brcm_pcie 위치 */
	pcie->dev = &pdev->dev; /* PCI/NVMe: device 설정; NVMe 로그/리소스용 */
	pcie->np = np; /* PCI/NVMe: DT node 설정; NVMe DT 속성 파싱 */
	pcie->cfg = data; /* PCI/NVMe: SoC cfg 설정; NVMe SoC별 동작 분기 */

	pcie->base = devm_platform_ioremap_resource(pdev, 0); /* PCI/NVMe: PCIe RC MMIO 매핑; NVMe 레지스터 접근 */
	if (IS_ERR(pcie->base)) /* PCI/NVMe: 매핑 실패; NVMe 레지스터 접근 불가 */
		return PTR_ERR(pcie->base); /* PCI/NVMe: 오류 반환; NVMe probe 실패 */

	pcie->clk = devm_clk_get_optional(&pdev->dev, "sw_pcie"); /* PCI/NVMe: PCIe 클럭 획득; NVMe 컨트롤러 클럭 */
	if (IS_ERR(pcie->clk)) /* PCI/NVMe: 클럭 획득 실패; NVMe 동작 불가 */
		return PTR_ERR(pcie->clk); /* PCI/NVMe: 오류 반환; NVMe probe 실패 */

	ret = of_pci_get_max_link_speed(np); /* PCI/NVMe: DT max-link-speed; NVMe PCIe 세대 제한 */
	if (pcie_get_link_speed(ret) == PCI_SPEED_UNKNOWN) /* PCI/NVMe: 알 수 없는 속도; NVMe 제한 없음 */
		pcie->gen = 0; /* PCI/NVMe: gen 0; NVMe 하드웨어 기본 속도 사용 */

	pcie->ssc = of_property_read_bool(np, "brcm,enable-ssc"); /* PCI/NVMe: SSC 속성; NVMe spread spectrum 활성 */

	pcie->rescal = devm_reset_control_get_optional_shared(&pdev->dev, "rescal"); /* PCI/NVMe: rescal reset; NVMe PHY calibration */
	if (IS_ERR(pcie->rescal)) /* PCI/NVMe: rescal 획득 실패; NVMe PHY 초기화 불가 */
		return PTR_ERR(pcie->rescal); /* PCI/NVMe: 오류 반환; NVMe probe 실패 */

	pcie->perst_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "perst"); /* PCI/NVMe: PERST# reset; NVMe fundamental reset */
	if (IS_ERR(pcie->perst_reset)) /* PCI/NVMe: PERST# 획득 실패; NVMe 장치 리셋 불가 */
		return PTR_ERR(pcie->perst_reset); /* PCI/NVMe: 오류 반환; NVMe probe 실패 */

	pcie->bridge_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "bridge"); /* PCI/NVMe: bridge reset; NVMe bridge reset */
	if (IS_ERR(pcie->bridge_reset)) /* PCI/NVMe: bridge reset 획득 실패; NVMe bridge 제어 불가 */
		return PTR_ERR(pcie->bridge_reset); /* PCI/NVMe: 오류 반환; NVMe probe 실패 */

	pcie->swinit_reset = devm_reset_control_get_optional_exclusive(&pdev->dev, "swinit"); /* PCI/NVMe: swinit reset; NVMe bridge sync reset */
	if (IS_ERR(pcie->swinit_reset)) /* PCI/NVMe: swinit reset 획득 실패; NVMe sync reset 불가 */
		return PTR_ERR(pcie->swinit_reset); /* PCI/NVMe: 오류 반환; NVMe probe 실패 */

	ret = clk_prepare_enable(pcie->clk); /* PCI/NVMe: PCIe 클럭 활성화; NVMe 컨트롤러 동작 준비 */
	if (ret) /* PCI/NVMe: 클럭 활성화 실패; NVMe 동작 불가 */
		return dev_err_probe(&pdev->dev, ret, "could not enable clock\n"); /* PCI/NVMe: 에러; NVMe probe 실패 */

	ret = brcm_pcie_bridge_sw_init_set(pcie, 0); /* PCI/NVMe: bridge 리셋 해제; NVMe 레지스터 접근 가능 */
	if (ret) /* PCI/NVMe: bridge 활성화 실패; NVMe 초기화 불가 */
		return dev_err_probe(&pdev->dev, ret, /* PCI/NVMe: 에러; NVMe probe 실패 */
				     "could not de-assert bridge reset\n");

	if (pcie->swinit_reset) { /* PCI/NVMe: swinit reset 있음; NVMe bridge sync reset */
		ret = reset_control_assert(pcie->swinit_reset); /* PCI/NVMe: swinit assert; NVMe bridge sync reset */
		if (ret) { /* PCI/NVMe: assert 실패; NVMe 초기화 실패 */
			clk_disable_unprepare(pcie->clk); /* PCI/NVMe: 클럭 off; NVMe cleanup */
			return dev_err_probe(&pdev->dev, ret, /* PCI/NVMe: 에러; NVMe probe 실패 */
					     "could not assert reset 'swinit'\n");
		}

		/* HW team recommends 1us for proper sync and propagation of reset */
		udelay(1); /* PCI/NVMe: 1us reset propagation; NVMe bridge sync 안정화 */

		ret = reset_control_deassert(pcie->swinit_reset); /* PCI/NVMe: swinit deassert; NVMe bridge sync reset 해제 */
		if (ret) { /* PCI/NVMe: deassert 실패; NVMe 초기화 실패 */
			clk_disable_unprepare(pcie->clk); /* PCI/NVMe: 클럭 off; NVMe cleanup */
			return dev_err_probe(&pdev->dev, ret, /* PCI/NVMe: 에러; NVMe probe 실패 */
					     "could not de-assert reset 'swinit'\n");
		}
	}

	ret = reset_control_reset(pcie->rescal); /* PCI/NVMe: rescal reset; NVMe PHY calibration 시작 */
	if (ret) { /* PCI/NVMe: rescal reset 실패; NVMe PHY 초기화 불가 */
		clk_disable_unprepare(pcie->clk); /* PCI/NVMe: 클럭 off; NVMe cleanup */
		return dev_err_probe(&pdev->dev, ret, "failed to deassert 'rescal'\n"); /* PCI/NVMe: 에러; NVMe probe 실패 */
	}

	ret = brcm_phy_start(pcie); /* PCI/NVMe: PHY 시작; NVMe PHY 전원/리셋 시퀀스 */
	if (ret) { /* PCI/NVMe: PHY 시작 실패; NVMe 링크 불가 */
		reset_control_rearm(pcie->rescal); /* PCI/NVMe: rescal rearm; NVMe cleanup */
		clk_disable_unprepare(pcie->clk); /* PCI/NVMe: 클럭 off; NVMe cleanup */
		return ret; /* PCI/NVMe: 실패 반환; NVMe probe 실패 */
	}

	ret = brcm_pcie_setup(pcie); /* PCI/NVMe: PCIe RC 설정; NVMe 링크/창/ASPM 기반 */
	if (ret) /* PCI/NVMe: setup 실패; NVMe 초기화 불가 */
		goto fail; /* PCI/NVMe: cleanup; NVMe probe 실패 */

	pcie->hw_rev = readl(pcie->base + PCIE_MISC_REVISION); /* PCI/NVMe: 하드웨어 리비전 읽기; NVMe MSI 레거시 모드 판별 */
	if (pcie->cfg->soc_base == BCM4908 && /* PCI/NVMe: BCM4908 특정; NVMe PERST# 문제 리비전 */
	    pcie->hw_rev >= BRCM_PCIE_HW_REV_3_20) {
		dev_err(pcie->dev, "hardware revision with unsupported PERST# setup\n"); /* PCI/NVMe: 에러; NVMe unsupported HW */
		ret = -ENODEV; /* PCI/NVMe: 장치 없음; NVMe probe 실패 */
		goto fail; /* PCI/NVMe: cleanup; NVMe probe 실패 */
	}

	if (pci_msi_enabled()) { /* PCI/NVMe: MSI 지원 시; NVMe MSI/MSI-X 초기화 */
		struct device_node *msi_np = of_parse_phandle(pcie->np, "msi-parent", 0); /* PCI/NVMe: msi-parent phandle; NVMe MSI 컨트롤러 */

		if (msi_np == pcie->np) /* PCI/NVMe: 내부 MSI 사용; NVMe brcm MSI 컨트롤러 */
			ret = brcm_pcie_enable_msi(pcie); /* PCI/NVMe: MSI 활성화; NVMe MSI/MSI-X 할당 인프라 */

		of_node_put(msi_np); /* PCI/NVMe: node 참조 해제; NVMe DT node refcount */

		if (ret) { /* PCI/NVMe: MSI 활성화 실패; NVMe MSI 없이 진행 불가(에러) */
			dev_err(pcie->dev, "probe of internal MSI failed"); /* PCI/NVMe: 에러; NVMe MSI 초기화 실패 */
			goto fail; /* PCI/NVMe: cleanup; NVMe probe 실패 */
		}
	}

	bridge->ops = pcie->cfg->soc_base == BCM7425 ? /* PCI/NVMe: BCM7425는 32비트 ops; NVMe config 접근 */
				&brcm7425_pcie_ops : &brcm_pcie_ops;
	bridge->sysdata = pcie; /* PCI/NVMe: sysdata 설정; NVMe pci_bus->sysdata로 brcm_pcie 전달 */

	platform_set_drvdata(pdev, pcie); /* PCI/NVMe: driver data 설정; NVMe remove/resume 사용 */

	ret = pci_host_probe(bridge); /* PCI/NVMe: PCI host 등록; NVMe PCIe 버스 열거 시작 */
	if (!ret && !brcm_pcie_link_up(pcie)) /* PCI/NVMe: probe 성공 후 link down; NVMe 장치 미연결 */
		ret = -ENODEV; /* PCI/NVMe: 장치 없음; NVMe probe 실패 */

	if (ret) { /* PCI/NVMe: host probe/link 실패; NVMe 초기화 실패 */
		brcm_pcie_remove(pdev); /* PCI/NVMe: 등록된 리소스 제거; NVMe cleanup */
		return ret; /* PCI/NVMe: 실패 반환; NVMe probe 실패 */
	}

	if (pcie->cfg->has_err_report) { /* PCI/NVMe: 오류 보고 기능; NVMe die/panic notifier 필요 */
		spin_lock_init(&pcie->bridge_lock); /* PCI/NVMe: bridge lock 초기화; NVMe 오류 레지스터 보호 */
		brcm_register_die_notifiers(pcie); /* PCI/NVMe: die/panic notifier 등록; NVMe 치명 오류 덤프 */
	}

	return 0; /* PCI/NVMe: probe 성공; NVMe PCIe RC 준비 완료, NVMe 열수 가능 */

fail:
	__brcm_pcie_remove(pcie); /* PCI/NVMe: 내부 제거; NVMe PCIe/PHY/클럭 off */

	return ret; /* PCI/NVMe: probe 실패; NVMe 초기화 불가 */
}

MODULE_DEVICE_TABLE(of, brcm_pcie_match); /* PCI/NVMe: OF 매칭 테이블; NVMe module auto-load용 */

static const struct dev_pm_ops brcm_pcie_pm_ops = {
	.suspend_noirq = brcm_pcie_suspend_noirq, /* PCI/NVMe: suspend noirq; NVMe 시스템 절전 */
	.resume_noirq = brcm_pcie_resume_noirq, /* PCI/NVMe: resume noirq; NVMe 시스템 복귀 */
};

static struct platform_driver brcm_pcie_driver = {
	.probe = brcm_pcie_probe, /* PCI/NVMe: probe 콜백; NVMe PCIe RC 초기화 */
	.remove = brcm_pcie_remove, /* PCI/NVMe: remove 콜백; NVMe PCIe RC 제거 */
	.driver = {
		.name = "brcm-pcie", /* PCI/NVMe: 드라이버 이름; NVMe platform driver 식별 */
		.of_match_table = brcm_pcie_match, /* PCI/NVMe: OF 매칭; NVMe DT compatible 매칭 */
		.pm = &brcm_pcie_pm_ops, /* PCI/NVMe: 전원 관리 ops; NVMe suspend/resume */
	},
};
module_platform_driver(brcm_pcie_driver); /* PCI/NVMe: platform driver 등록; NVMe 부팅 시 PCIe RC 초기화 */

MODULE_LICENSE("GPL"); /* PCI/NVMe: GPL 라이선스; NVMe 드라이버와 호환 */
MODULE_DESCRIPTION("Broadcom STB PCIe RC driver"); /* PCI/NVMe: 드라이버 설명; NVMe Broadcom PCIe RC */
MODULE_AUTHOR("Broadcom"); /* PCI/NVMe: 저작자; NVMe */
MODULE_SOFTDEP("pre: irq_bcm2712_mip"); /* PCI/NVMe: 선행 모듈; NVMe BCM2712 MSI parent 의존 */
