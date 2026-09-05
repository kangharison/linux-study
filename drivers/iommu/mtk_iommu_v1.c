// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for MTK architected m4u v1 implementations
 *
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Honghui Zhang <honghui.zhang@mediatek.com>
 *
 * Based on driver/iommu/mtk_iommu.c
 */
/*
 * [한국어 설명] MediaTek M4U 1세대(m4u v1) IOMMU 드라이버 (mtk_iommu_v1.c)
 *
 * === 파일의 역할 ===
 * MediaTek MT2701 세대 SoC에 들어 있는 M4U(Multimedia Memory Management Unit)
 * 1세대 하드웨어를 리눅스 IOMMU 서브시스템에 붙이는 드라이버다. 2세대 이후를
 * 다루는 mtk_iommu.c와 형제 관계이며, 이름과 달리 구조가 상당히 다르다.
 * 이 1세대 하드웨어의 결정적인 제약이 드라이버 전체의 모양을 결정한다:
 *  - **도메인이 하나뿐이다.** 하드웨어가 페이지 테이블 베이스 레지스터를
 *    하나만 갖고 있어, 이 M4U에 붙는 모든 클라이언트가 같은 IOVA 공간을
 *    공유한다. 그래서 attach가 "이미 만들어진 유일한 도메인이 맞는지"만
 *    확인하고 넘어가는 특이한 모양이 된다.
 *  - **페이지 테이블이 1단계 평면 배열이다.** 4GB IOVA 공간을 4KB 페이지로
 *    나눈 100만 개 엔트리 × 4바이트 = 정확히 4MB짜리 배열 하나다.
 *    map은 배열에 값을 쓰는 것, unmap은 memset(0)이 전부다.
 *  - **폴트가 읽기/쓰기를 구분하지 못한다.** 인터럽트 핸들러가 모든 폴트를
 *    IOMMU_FAULT_READ로 보고하는 이유다.
 *  - **ARM DMA-IOMMU 레거시 경로를 쓴다.** 최신 dma-iommu 대신
 *    arm_iommu_create_mapping()/attach_device()로 DMA 매핑을 붙인다.
 *
 * 또 하나의 축은 **LARB(Local Arbiter)**다. MediaTek SoC에서 멀티미디어 IP
 * (카메라, 디코더, 디스플레이)는 M4U에 직접 붙지 않고 SMI(Smart Multimedia
 * Interface)의 LARB를 거친다. 어느 포트의 트래픽을 M4U로 보낼지는 M4U가
 * 아니라 LARB 레지스터가 결정하므로, 이 드라이버는 component 프레임워크로
 * LARB 디바이스들을 모아 두었다가 attach 시 해당 포트의 MMU 비트를 켠다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [멀티미디어 드라이버] dma_map_*()
 *        ↓ (ARM 레거시 DMA-IOMMU 경로)
 *   [arm_iommu_* / IOMMU 코어] iommu_ops 콜백
 *        ↓
 *   [이 파일] mtk_iommu_v1_map() → 4MB 평면 배열에 엔트리 기록
 *        ↓                       → mtk_iommu_v1_config()로 LARB 포트 활성화
 *   [SMI LARB] 포트별 MMU 비트 — 이 트래픽을 M4U로 보낼지 결정
 *        ↓
 *   [M4U 하드웨어] PT_BASE_ADDR가 가리키는 평면 배열을 인덱싱해 변환
 *
 * probe 흐름이 특이하다: M4U가 마스터가 되어 component_master_add_with_match()로
 * 디바이스 트리의 mediatek,larbs가 가리키는 LARB들을 기다리고, 모두 준비되면
 * bind 콜백에서 larb_imu[] 배열을 채운다. 이 배열이 있어야 포트 제어가 가능하다.
 *
 * 실행 컨텍스트: probe/attach는 프로세스 컨텍스트, map/unmap은 atomic 가능
 * (irqsave 스핀락), ISR은 인터럽트 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * - soc/mediatek/smi.h: struct mtk_smi_larb_iommu와 MTK_SMI_MMU_EN().
 *   LARB 드라이버와 공유하는 인터페이스로, mmu 비트마스크를 여기 써 두면
 *   LARB 드라이버가 전원 인가 시 하드웨어에 반영한다.
 * - dt-bindings/memory/mt2701-larb-port.h: LARB0_PORT_OFFSET 등 포트 ID
 *   공간의 경계값. 디바이스 트리의 iommus 프로퍼티에 실린 하나의 ID를
 *   (LARB 번호, 포트 번호)로 분해하는 데 쓴다.
 * - asm/dma-iommu.h: ARM 32비트 전용 레거시 DMA-IOMMU API. ARM이 아닌
 *   빌드에서는 파일 상단의 스텁 매크로가 대신한다(컴파일 테스트 목적).
 * - linux/component.h: M4U와 LARB들의 probe 순서 문제를 푸는 프레임워크.
 * 데이터 흐름: 디바이스 트리의 `iommus = <&iommu M4U_PORT_xxx>` →
 * probe_device가 fwspec에 ID를 모으고 LARB device_link를 건다 →
 * attach가 그 ID를 (larb, port)로 분해해 MMU 비트를 켠다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mtk_iommu_v1_data: M4U 인스턴스 하나. 레지스터 베이스, IRQ, 클록,
 *   폴트용 보호 메모리, 유일한 도메인 포인터, LARB 배열, 서스펜드 백업.
 * - struct mtk_iommu_v1_domain: 유일한 도메인. 4MB 평면 테이블과 스핀락.
 * - mtk_iommu_v1_create_mapping(): of_xlate에 해당하는 역할. fwspec을 만들고
 *   ARM DMA 매핑(4GB)을 최초 1회 생성한다.
 * - mtk_iommu_v1_probe_device(): iommus 프로퍼티를 순회하며 fwspec을 채우고,
 *   모든 ID가 같은 LARB에 속하는지 검사한 뒤 device_link를 건다.
 * - mtk_iommu_v1_config(): fwspec의 각 ID에 대해 LARB 포트의 MMU 비트를 켜거나 끈다.
 * - mtk_iommu_v1_isr(): 폴트 레지스터를 읽어 어느 LARB/포트가 문제인지 보고하고,
 *   인터럽트를 클리어한 뒤 TLB를 통째로 비운다.
 * - mtk_iommu_v1_hw_init(): 클록을 켜고 제어/인터럽트/보호 메모리 레지스터를
 *   프로그래밍한 뒤 IRQ 핸들러를 등록한다.
 */
/* [한국어] BUG_ON/WARN_ON 계열 매크로. */
#include <linux/bug.h>
/* [한국어] M4U의 버스 클록(bclk)을 켜고 끄는 데 필요하다. */
#include <linux/clk.h>
/* [한국어] component 프레임워크 — M4U(마스터)가 여러 LARB(컴포넌트)가
 * 준비될 때까지 기다렸다가 한꺼번에 바인딩하게 해 준다. */
#include <linux/component.h>
/* [한국어] struct device와 dev_err/dev_iommu_priv_* 접근자. */
#include <linux/device.h>
/* [한국어] dma_alloc_coherent() — 4MB 평면 페이지 테이블을 잡는 데 쓴다. */
#include <linux/dma-mapping.h>
/* [한국어] IS_ERR/PTR_ERR 등 포인터 오류 인코딩 헬퍼. */
#include <linux/err.h>
/* [한국어] devm_request_irq()와 irqreturn_t — 폴트 인터럽트 처리용. */
#include <linux/interrupt.h>
/* [한국어] readl_relaxed()/writel_relaxed() MMIO 접근자. */
#include <linux/io.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, iommu_domain, report_iommu_fault 등. */
#include <linux/iommu.h>
/* [한국어] readl_poll_timeout_atomic() — 범위 TLB 무효화 완료를 폴링하는 데 쓴다. */
#include <linux/iopoll.h>
/* [한국어] 리스트 매크로. 직접 쓰이지는 않지만 관련 헤더들이 요구한다. */
#include <linux/list.h>
/* [한국어] MODULE_* 매크로와 THIS_MODULE. */
#include <linux/module.h>
/* [한국어] of_parse_phandle_with_args() 등 디바이스 트리 주소/phandle 파싱. */
#include <linux/of_address.h>
/* [한국어] 디바이스 트리에서 인터럽트를 얻는 헬퍼. */
#include <linux/of_irq.h>
/* [한국어] of_find_device_by_node() — phandle에서 플랫폼 디바이스를 역추적한다. */
#include <linux/of_platform.h>
/* [한국어] platform_driver/platform_device 정의. */
#include <linux/platform_device.h>
/* [한국어] kzalloc_obj/devm_kzalloc/kfree. */
#include <linux/slab.h>
/* [한국어] spinlock_t와 spin_lock_irqsave — 평면 테이블 보호용. */
#include <linux/spinlock.h>
/* [한국어] str_enable_disable() — 디버그 로그에서 bool을 "enable"/"disable"
 * 문자열로 바꾸는 헬퍼. */
#include <linux/string_choices.h>
/* [한국어] wmb() — TLB 무효화 명령이 실제로 하드웨어에 도달했음을 보장한다. */
#include <asm/barrier.h>
/* [한국어] MTK_LARB_NR_MAX — 이 SoC 계열이 가질 수 있는 최대 LARB 개수.
 * larb_imu[] 배열의 크기를 결정한다. */
#include <dt-bindings/memory/mtk-memory-port.h>
/* [한국어] LARB0_PORT_OFFSET 등 MT2701의 포트 ID 공간 경계.
 * 디바이스 트리에 실린 단일 ID를 (LARB, 포트)로 분해하는 근거다. */
#include <dt-bindings/memory/mt2701-larb-port.h>
/* [한국어] struct mtk_smi_larb_iommu와 MTK_SMI_MMU_EN() — SMI LARB 드라이버와
 * 공유하는 인터페이스. 포트별 MMU 활성화 비트를 여기에 기록한다. */
#include <soc/mediatek/smi.h>

/* [한국어] 이 드라이버는 ARM 32비트의 레거시 DMA-IOMMU 경로에 의존한다.
 * 그런데 MT2701은 ARM 32비트 전용이라 다른 아키텍처에서는 그 API가 없다.
 * 컴파일 테스트(COMPILE_TEST)를 가능하게 하려고, ARM이 아니면 아무것도 하지
 * 않는 스텁으로 대체한다 — 실행되지는 않지만 빌드는 통과하게 만든다. */
#if defined(CONFIG_ARM)
/* [한국어] ARM 32비트: 진짜 arm_iommu_* API를 가져온다. */
#include <asm/dma-iommu.h>
#else
/* [한국어] 비ARM 스텁: 매핑 생성이 항상 NULL을 반환한다. */
#define arm_iommu_create_mapping(...) NULL
/* [한국어] 비ARM 스텁: attach가 항상 실패한다 — 실제로 동작할 수 없음을
 * 명시적으로 드러낸다. */
#define arm_iommu_attach_device(...)	-ENODEV
/* [한국어] 비ARM 스텁: 이 드라이버가 참조하는 최소 필드만 갖춘 껍데기 구조체.
 * attach_device()가 mapping->domain을 비교하므로 그 필드만 있으면 된다. */
struct dma_iommu_mapping {
	struct iommu_domain *domain;
	/* [한국어] 이 DMA 매핑이 사용하는 IOMMU 도메인.
	 * 실제 ARM 구현에서는 arm_iommu_create_mapping()이 채운다.
	 * 읽는 자: mtk_iommu_v1_attach_device()가 "이 도메인이 내가 만든
	 *          유일한 도메인인가"를 판별하는 데 쓴다.
	 * 값 범위: 유효한 도메인 포인터 또는 NULL. */
};
#endif

/* [한국어] 페이지 테이블 베이스 주소 레지스터. 4MB 평면 배열의 물리 주소를
 * 여기 쓰면 하드웨어가 그 배열을 인덱싱해 변환한다. 이 레지스터가 하나뿐인
 * 것이 곧 "도메인이 하나뿐"인 이유다. */
#define REG_MMU_PT_BASE_ADDR			0x000

/* [한국어] INVALIDATE 레지스터에 쓰는 값: TLB 전체 무효화. */
#define F_ALL_INVLD				0x2
/* [한국어] INVALIDATE 레지스터에 쓰는 값: START_A~END_A로 지정한 범위만 무효화. */
#define F_MMU_INV_RANGE				0x1
/* [한국어] INV_SEL 레지스터의 무효화 대상 선택 비트 0.
 * 아래 EN1과 함께 세워 두 개의 TLB(또는 뱅크)를 모두 대상으로 삼는다. */
#define F_INVLD_EN0				BIT(0)
/* [한국어] INV_SEL의 무효화 대상 선택 비트 1. EN0과 항상 함께 쓰인다. */
#define F_INVLD_EN1				BIT(1)

/* [한국어] 폴트 주소 레지스터에서 유효한 비트(페이지 정렬된 상위 20비트).
 * 하위 12비트는 의미가 없으므로 마스킹해 버린다. 무효화 범위 레지스터에
 * 주소를 쓸 때도 같은 마스크를 적용한다. */
#define F_MMU_FAULT_VA_MSK			0xfffff000
/* [한국어] 폴트 시 하드웨어가 데이터를 쓰는 "보호 메모리"의 정렬 요구(128바이트).
 * probe에서 이 크기의 두 배를 할당한 뒤 정렬해 쓰는 이유가 여기 있다. */
#define MTK_PROTECT_PA_ALIGN			128

/* [한국어] M4U 주 제어 레지스터. */
#define REG_MMU_CTRL_REG			0x210
/* [한국어] 코히런시 활성화 비트. 페이지 테이블 워크가 CPU 캐시와 일관성을
 * 갖게 해, 드라이버가 테이블 갱신 후 캐시를 flush 하지 않아도 되게 한다. */
#define F_MMU_CTRL_COHERENT_EN			BIT(8)
/* [한국어] IVRP(Invalid Physical Address) 레지스터 — 변환 실패 시 하드웨어가
 * 접근을 돌릴 물리 주소. 보호 메모리의 주소를 여기 쓴다. */
#define REG_MMU_IVRP_PADDR			0x214
/* [한국어] 인터럽트 제어 레지스터. 어떤 폴트에 인터럽트를 낼지 선택하고,
 * 인터럽트 클리어도 이 레지스터로 한다. */
#define REG_MMU_INT_CONTROL			0x220
/* [한국어] 변환 폴트 — 매핑되지 않은 IOVA에 접근했다. 가장 흔한 폴트다. */
#define F_INT_TRANSLATION_FAULT			BIT(0)
/* [한국어] 메인 TLB 다중 히트 — 같은 주소에 대해 TLB 엔트리가 여러 개
 * 매칭됐다. 소프트웨어가 무효화 없이 매핑을 덮어썼을 때 생긴다. */
#define F_INT_MAIN_MULTI_HIT_FAULT		BIT(1)
/* [한국어] 잘못된 물리 주소 — 테이블 엔트리가 가리키는 주소가 유효하지 않다. */
#define F_INT_INVALID_PA_FAULT			BIT(2)
/* [한국어] 엔트리 교체 폴트 — TLB 엔트리를 교체하는 도중 문제가 생겼다. */
#define F_INT_ENTRY_REPLACEMENT_FAULT		BIT(3)
/* [한국어] 테이블 워크 폴트 — 페이지 테이블을 읽는 과정 자체가 실패했다
 * (예: PT_BASE_ADDR가 잘못됐거나 메모리 오류). */
#define F_INT_TABLE_WALK_FAULT			BIT(4)
/* [한국어] TLB 미스 폴트 — TLB에 없고 테이블에서도 찾지 못했다. */
#define F_INT_TLB_MISS_FAULT			BIT(5)
/* [한국어] 프리페치 DMA FIFO 오버플로 — 테이블 프리페치 요청이 밀렸다.
 * 성능 문제의 신호이지 정확성 문제는 아니다. */
#define F_INT_PFH_DMA_FIFO_OVERFLOW		BIT(6)
/* [한국어] 미스 처리 DMA FIFO 오버플로 — 미스 처리 요청이 밀렸다. */
#define F_INT_MISS_DMA_FIFO_OVERFLOW		BIT(7)

/* [한국어] 변환 폴트 시 하드웨어의 대응 방식을 고르는 2비트 필드(비트 5~6).
 * hw_init에서 2를 넣는데, 이는 "보호 메모리로 리다이렉트"에 해당한다 —
 * 폴트 접근이 실제 메모리를 오염시키지 않게 하는 설정이다. */
#define F_MMU_TF_PROTECT_SEL(prot)		(((prot) & 0x3) << 5)
/* [한국어] 인터럽트 클리어 비트. ISR이 INT_CONTROL을 읽어 이 비트를 세워
 * 다시 쓰면 대기 중인 인터럽트가 해제된다. */
#define F_INT_CLR_BIT				BIT(12)

/* [한국어] 폴트 상태 레지스터 — 위 F_INT_* 비트들이 어떤 폴트가 났는지 알려 준다. */
#define REG_MMU_FAULT_ST			0x224
/* [한국어] 폴트를 일으킨 가상 주소(IOVA). */
#define REG_MMU_FAULT_VA			0x228
/* [한국어] 폴트를 일으킨 물리 주소 — 변환 결과가 잘못된 경우 그 값이 실린다. */
#define REG_MMU_INVLD_PA			0x22C
/* [한국어] 폴트를 일으킨 마스터의 ID. 여기서 LARB 번호와 포트 번호를
 * 추출해 "어느 IP가 잘못했는지"를 특정한다. */
#define REG_MMU_INT_ID				0x388
/* [한국어] TLB 무효화 명령 레지스터. F_ALL_INVLD 또는 F_MMU_INV_RANGE를 쓴다. */
#define REG_MMU_INVALIDATE			0x5c0
/* [한국어] 범위 무효화의 시작 주소. */
#define REG_MMU_INVLD_START_A			0x5c4
/* [한국어] 범위 무효화의 끝 주소(포함). */
#define REG_MMU_INVLD_END_A			0x5c8

/* [한국어] 무효화 대상 선택 레지스터. EN0|EN1을 써 모든 TLB를 대상으로 삼는다. */
#define REG_MMU_INV_SEL				0x5d8
/* [한국어] 표준 AXI 모드 레지스터. 이 드라이버는 값을 설정하지 않고
 * 서스펜드/리줌 때 보존만 한다. */
#define REG_MMU_STANDARD_AXI_MODE		0x5e8

/* [한국어] DCM(Dynamic Clock Management) 레지스터 — 유휴 시 내부 클록을
 * 자동으로 멈추는 전력 절감 기능을 제어한다. */
#define REG_MMU_DCM				0x5f0
/* [한국어] DCM 활성화 비트. hw_init이 이 비트만 세워 동적 클록 게이팅을 켠다. */
#define F_MMU_DCM_ON				BIT(1)
/* [한국어] CPE(Command Processing Engine) 완료 상태 레지스터.
 * 범위 무효화를 요청한 뒤 이 값이 0이 아니게 되기를 폴링해 완료를 확인한다. */
#define REG_MMU_CPE_DONE			0x60c
/* [한국어] 페이지 테이블 엔트리의 유효 비트(0x2). 이 비트가 없으면 하드웨어가
 * 그 엔트리를 매핑 없음으로 본다. map이 물리 주소와 함께 세운다. */
#define F_DESC_VALID				0x2
/* [한국어] 엔트리의 비보안(non-secure) 비트. TrustZone 환경에서 이 매핑이
 * 비보안 세계 소유임을 표시한다. 리눅스는 항상 비보안 쪽이므로 늘 세운다. */
#define F_DESC_NONSEC				BIT(3)
/* [한국어] 폴트 ID 레지스터 값에서 LARB 번호를 뽑는 매크로.
 * 비트 13~15를 꺼낸 뒤 6에서 빼는데, 이는 하드웨어의 LARB 인코딩이
 * 소프트웨어 번호와 역순이기 때문이다(MT2701 실리콘의 배선 사정). */
#define MT2701_M4U_TF_LARB(TF)			(6 - (((TF) >> 13) & 0x7))
/* [한국어] 폴트 ID에서 포트 번호(비트 8~11)를 뽑는 매크로. */
#define MT2701_M4U_TF_PORT(TF)			(((TF) >> 8) & 0xF)
/* MTK generation one iommu HW only support 4K size mapping */
/* [한국어] 페이지 크기의 로그값. IOVA를 평면 배열 인덱스로 바꾸는 시프트량이다.
 * 1세대 하드웨어는 4KB 외의 페이지 크기를 지원하지 않는다. */
#define MT2701_IOMMU_PAGE_SHIFT			12
/* [한국어] 페이지 크기 4KB. domain->pgsize_bitmap에 이 값 하나만 실린다. */
#define MT2701_IOMMU_PAGE_SIZE			(1UL << MT2701_IOMMU_PAGE_SHIFT)
/* [한국어] MT2701이 가진 LARB 개수의 상한. probe_device가 폴트 ID에서
 * 계산한 LARB 번호가 이 값을 넘으면 잘못된 디바이스 트리로 간주한다.
 * larb_imu[] 배열 크기(MTK_LARB_NR_MAX)와는 별개의 값임에 주의. */
#define MT2701_LARB_NR_MAX			3

/*
 * MTK m4u support 4GB iova address space, and only support 4K page
 * mapping. So the pagetable size should be exactly as 4M.
 */
/* [한국어] 평면 페이지 테이블의 크기 4MB.
 * 계산 근거: 4GB IOVA / 4KB 페이지 = 1,048,576개 엔트리 × 4바이트 = 4MB.
 * 다단계 테이블이 아니라 IOVA 전체를 덮는 배열이므로 크기가 고정된다.
 * 4MB 연속 코히런트 메모리를 부팅 초반에 잡아야 하는 부담이 있지만,
 * 대신 워크가 없어 변환 지연이 매우 짧다. */
#define M2701_IOMMU_PGT_SIZE			SZ_4M

/* [한국어] 시스템 서스펜드 동안 보존해야 할 레지스터 값들.
 * M4U는 서스펜드 시 전원이 끊겨 레지스터가 초기화되므로, 리줌 때
 * 그대로 되돌리기 위해 백업해 둔다.
 * 설정자: mtk_iommu_v1_suspend(). 읽는 자: mtk_iommu_v1_resume(). */
struct mtk_iommu_v1_suspend_reg {
	u32			standard_axi_mode;
	/* [한국어] REG_MMU_STANDARD_AXI_MODE의 백업.
	 * 이 드라이버가 직접 설정하지는 않지만 부트로더가 정한 값이 있을 수
	 * 있어 그대로 보존한다.
	 * 값 범위: 하드웨어가 정의하는 AXI 동작 모드 비트들.
	 * 동기화: 서스펜드/리줌은 직렬화된 경로라 락이 없다. */

	u32			dcm_dis;
	/* [한국어] REG_MMU_DCM의 백업(동적 클록 게이팅 설정).
	 * 이름이 dcm_dis("disable")인 것은 과거 코드의 흔적이며, 실제로는
	 * 레지스터 값 그대로를 담는다.
	 * 값 범위: F_MMU_DCM_ON 비트를 포함한 값.
	 * 동기화: 위와 동일. */

	u32			ctrl_reg;
	/* [한국어] REG_MMU_CTRL_REG의 백업 — 코히런시 활성화와 폴트 대응 방식.
	 * 리줌 시 이 값을 되돌리지 않으면 폴트가 보호 메모리로 가지 않아
	 * 시스템 메모리가 오염될 수 있다.
	 * 동기화: 위와 동일. */

	u32			int_control0;
	/* [한국어] REG_MMU_INT_CONTROL의 백업 — 어떤 폴트에 인터럽트를 낼지.
	 * 되돌리지 않으면 리줌 후 폴트가 조용히 무시된다.
	 * 동기화: 위와 동일. */
};

/* [한국어] M4U 인스턴스 하나를 표현하는 구조체.
 * 수명: probe에서 devm_kzalloc으로 만들어져 디바이스와 함께 사라진다.
 * 소유자: 플랫폼 디바이스의 drvdata이자, 클라이언트들의 dev_iommu_priv. */
struct mtk_iommu_v1_data {
	void __iomem			*base;
	/* [한국어] M4U MMIO 레지스터 블록의 매핑된 주소.
	 * 설정자: probe의 devm_ioremap_resource().
	 * 읽는 자: 모든 readl_relaxed/writel_relaxed 호출의 기준 주소.
	 * 값 범위: ioremap된 __iomem 포인터 — 반드시 MMIO 접근자로만 다뤄야 한다.
	 * 동기화: devm 관리라 디바이스 해제 시 자동 언매핑. */

	int				irq;
	/* [한국어] 폴트 인터럽트 번호.
	 * 설정자: probe의 platform_get_irq().
	 * 읽는 자: hw_init이 devm_request_irq()에 넘기고, remove가
	 *          devm_free_irq()에 넘긴다.
	 * 값 범위: 유효한 리눅스 IRQ 번호(음수면 probe 실패).
	 * 동기화: probe 이후 불변. */

	struct device			*dev;
	/* [한국어] 이 M4U 자신의 struct device.
	 * 설정자: probe.
	 * 읽는 자: dma_alloc_coherent()의 DMA 마스터, dev_err/dev_warn 로깅,
	 *          devm_request_irq()의 소유자.
	 * 동기화: probe 이후 불변. */

	struct clk			*bclk;
	/* [한국어] M4U의 버스 클록. 이 클록이 꺼져 있으면 레지스터 접근이
	 * 멈추므로 hw_init에서 반드시 먼저 켠다.
	 * 설정자: probe의 devm_clk_get(dev, "bclk").
	 * 읽는 자: hw_init(켜기), probe 실패 경로와 remove(끄기).
	 * 값 범위: 유효한 클록 포인터(이 드라이버에서는 필수라 optional이 아니다).
	 * 동기화: probe 이후 불변. */

	phys_addr_t			protect_base; /* protect memory base */
	/* [한국어] 변환 폴트 시 하드웨어가 데이터를 쓰는 "보호 메모리"의
	 * 정렬된 물리 주소.
	 * 설정자: probe가 devm_kcalloc으로 잡은 버퍼를 128바이트 정렬해 저장.
	 * 읽는 자: hw_init과 resume이 REG_MMU_IVRP_PADDR에 기록한다.
	 * 값 범위: MTK_PROTECT_PA_ALIGN(128) 정렬된 물리 주소.
	 * 왜 필요한가: 폴트 접근을 실제 메모리 대신 이 버퍼로 보내, 잘못된
	 *              DMA가 시스템 메모리를 오염시키는 것을 막는다.
	 * 왜 2배를 할당하는가: kcalloc 결과가 128바이트 정렬이라는 보장이 없어,
	 *                      정렬 후에도 충분한 공간이 남도록 여유를 둔다.
	 * 동기화: probe 이후 불변. */

	struct mtk_iommu_v1_domain	*m4u_dom;
	/* [한국어] 이 M4U의 **유일한** 도메인.
	 * 설정자: 첫 attach가 기록하고, 실패하면 NULL로 되돌린다.
	 * 읽는 자: ISR이 report_iommu_fault()에 넘길 도메인으로 쓰고,
	 *          resume이 pgt_pa를 되찾는 데 쓴다.
	 * 값 범위: NULL(아직 도메인 없음) 또는 유효한 도메인 포인터.
	 * 왜 하나뿐인가: PT_BASE_ADDR 레지스터가 하나뿐이라 하드웨어가 동시에
	 *                여러 페이지 테이블을 가질 수 없다.
	 * 동기화: attach는 IOMMU 코어가 직렬화한다. */

	struct iommu_device		iommu;
	/* [한국어] IOMMU 코어에 등록되는 핸들(임베드).
	 * 설정자: probe의 iommu_device_sysfs_add()/register().
	 * 읽는 자: probe_device()가 담당 IOMMU로 이 주소를 반환한다.
	 * 동기화: probe/remove에서만 다룬다. */

	struct dma_iommu_mapping	*mapping;
	/* [한국어] ARM 레거시 DMA-IOMMU 매핑(4GB IOVA 공간).
	 * 설정자: create_mapping()이 최초 1회 arm_iommu_create_mapping()으로 생성.
	 * 읽는 자: probe_finalize()가 클라이언트를 여기 붙이고,
	 *          attach_device()가 "이 도메인이 내 매핑의 도메인인가"를 검사한다.
	 * 값 범위: NULL(아직 생성 전) 또는 유효한 매핑 포인터.
	 * 왜 필요한가: 이 드라이버는 최신 dma-iommu 경로가 아니라 ARM 32비트의
	 *              옛 경로를 쓴다. 클라이언트의 dma_map_*()가 이 매핑을 거친다.
	 * 동기화: 최초 생성 경로는 probe_device 안에서 직렬화된다. */

	struct mtk_smi_larb_iommu	larb_imu[MTK_LARB_NR_MAX];
	/* [한국어] 이 M4U에 연결된 SMI LARB들의 상태 배열.
	 * 설정자: probe가 각 원소의 dev를 채우고, component bind가 나머지를
	 *          LARB 드라이버로부터 받아 채운다. config()가 mmu 비트를 갱신한다.
	 * 읽는 자: SMI LARB 드라이버가 전원 인가 시 mmu 필드를 읽어 하드웨어에
	 *          반영한다 — 즉 이 배열이 두 드라이버의 공유 상태다.
	 * 값 범위: 인덱스는 LARB 번호(0 ~ MTK_LARB_NR_MAX-1).
	 * 왜 필요한가: 어느 포트의 트래픽을 M4U로 보낼지는 M4U가 아니라 LARB가
	 *              결정한다. IOMMU를 켜려면 반드시 LARB 쪽을 건드려야 한다.
	 * 동기화: attach 경로에서만 갱신되며, 코어가 직렬화한다. */

	struct mtk_iommu_v1_suspend_reg	reg;
	/* [한국어] 서스펜드 동안 보존할 레지스터 값들(임베드).
	 * 설정자: suspend(). 읽는 자: resume().
	 * 동기화: PM 코어가 직렬화하는 경로라 락이 없다. */
};

/* [한국어] IOMMU 도메인 하나 — 이 드라이버에서는 시스템에 사실상 하나만 존재한다.
 * 수명: domain_alloc_paging에서 kzalloc되고 domain_free에서 해제된다. */
struct mtk_iommu_v1_domain {
	spinlock_t			pgtlock; /* lock for page table */
	/* [한국어] 4MB 평면 테이블 배열을 보호하는 스핀락.
	 * 설정자: domain_finalise()가 초기화한다(alloc이 아니라 finalise인 점에
	 *          주의 — 도메인이 실제로 쓰이기 시작하는 시점이다).
	 * 읽는 자: map/unmap/iova_to_phys가 irqsave로 잡는다.
	 * 왜 irqsave인가: DMA API가 인터럽트 컨텍스트에서도 호출될 수 있다. */

	struct iommu_domain		domain;
	/* [한국어] IOMMU 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging()이 pgsize_bitmap을 4KB로 설정.
	 * 읽는 자: 코어 전반, 그리고 ISR의 report_iommu_fault().
	 * 특이점: aperture(geometry)를 설정하지 않는다 — ARM DMA 매핑이 4GB
	 *         공간을 관리하므로 코어 쪽 범위 검사에 의존하지 않는다.
	 * 동기화: alloc 이후 불변. */

	u32				*pgt_va;
	/* [한국어] 4MB 평면 페이지 테이블의 커널 가상 주소.
	 * 설정자: domain_finalise()의 dma_alloc_coherent().
	 * 읽는 자: map/unmap/iova_to_phys가 (iova >> 12)를 인덱스로 접근한다.
	 * 엔트리 형식: 물리 주소 | F_DESC_VALID | F_DESC_NONSEC.
	 *              sprd와 달리 PFN이 아니라 주소를 그대로 담는다(하위 비트가
	 *              플래그로 쓰이므로 페이지 정렬 덕분에 충돌하지 않는다).
	 * 동기화: 배열 내용은 pgtlock으로 보호된다. */

	dma_addr_t			pgt_pa;
	/* [한국어] 위 테이블의 DMA(물리) 주소.
	 * 설정자: dma_alloc_coherent()의 출력 인자.
	 * 읽는 자: domain_finalise()와 resume()이 REG_MMU_PT_BASE_ADDR에 기록한다.
	 * 동기화: finalise 이후 불변. */

	struct mtk_iommu_v1_data	*data;
	/* [한국어] 이 도메인이 속한 M4U 인스턴스.
	 * 설정자: domain_finalise()가 기록.
	 * 읽는 자: map/unmap이 TLB 무효화를 요청할 때, domain_free가 DMA
	 *          메모리를 반납할 때 쓴다.
	 * 값 범위: finalise 전에는 NULL이므로, finalise되지 않은 도메인을
	 *          free하면 NULL 역참조가 난다(기존 코드의 취약점).
	 * 동기화: attach 경로에서 한 번 설정된다. */
};

/*
 * [한국어]
 * mtk_iommu_v1_bind - component 마스터의 bind 콜백
 *
 * @dev: M4U 디바이스(component 마스터).
 * @return: component_bind_all()의 결과(0 또는 음수 errno).
 *
 * 왜 필요한가: M4U는 LARB 없이는 아무것도 할 수 없지만, LARB 드라이버가
 * 먼저 probe된다는 보장이 없다. component 프레임워크가 "필요한 컴포넌트가
 * 모두 준비되면 알려 주는" 역할을 하고, 이 콜백이 그 알림이다.
 * component_bind_all()은 각 LARB 드라이버의 bind를 호출하며, 그 과정에서
 * LARB들이 자기 정보를 larb_imu[] 배열에 채워 넣는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(마지막 LARB가 probe될 때 또는
 * component_master_add_with_match() 안에서 즉시).
 *
 * 호출 체인:
 *   component 코어 → master_ops->bind → [mtk_iommu_v1_bind]
 *   → component_bind_all() → 각 LARB의 bind
 */
static int mtk_iommu_v1_bind(struct device *dev)
{
	/* [한국어] probe가 심어 둔 M4U 인스턴스를 꺼낸다. */
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);

	/* [한국어] 모든 LARB 컴포넌트를 바인딩하고, 그들이 larb_imu 배열을
	 * 채우게 한다. 두 번째 인자가 각 LARB에 전달되어 자기 슬롯을 찾는 근거가 된다. */
	return component_bind_all(dev, &data->larb_imu);
}

/*
 * [한국어]
 * mtk_iommu_v1_unbind - component 마스터의 unbind 콜백
 *
 * @dev: M4U 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: bind의 반대. LARB 중 하나라도 사라지면 component 프레임워크가
 * 이 콜백으로 전체 바인딩을 해제한다. larb_imu 배열의 내용이 무효가 되므로,
 * 이후 M4U는 포트를 제어할 수 없는 상태가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버 제거 또는 LARB 제거 시).
 *
 * 호출 체인:
 *   component 코어 → master_ops->unbind → [mtk_iommu_v1_unbind]
 *   → component_unbind_all()
 */
static void mtk_iommu_v1_unbind(struct device *dev)
{
	/* [한국어] M4U 인스턴스를 꺼낸다. */
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);

	/* [한국어] 모든 LARB 컴포넌트의 바인딩을 해제한다. */
	component_unbind_all(dev, &data->larb_imu);
}

/*
 * [한국어]
 * to_mtk_domain - 일반 iommu_domain을 이 드라이버의 도메인으로 되돌린다
 *
 * @dom: 코어가 넘긴 일반 도메인 포인터.
 * @return: 그것을 감싸는 struct mtk_iommu_v1_domain 포인터.
 *
 * 왜 필요한가: 코어는 iommu_domain만 알고 드라이버는 그것을 자기 구조체에
 * 임베드해 두므로, container_of로 바깥 구조체를 복원한다.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄. 순수 포인터 산술이다.
 *
 * 호출 체인:
 *   map/unmap/attach/free/iova_to_phys → [to_mtk_domain]
 */
static struct mtk_iommu_v1_domain *to_mtk_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드된 멤버의 주소에서 오프셋을 빼 바깥 구조체를 얻는다. */
	return container_of(dom, struct mtk_iommu_v1_domain, domain);
}

/* [한국어] 각 LARB의 포트 ID가 시작하는 경계값 배열.
 * MT2701의 디바이스 트리는 "M4U 포트 ID" 하나로 (LARB, 포트)를 표현하는데,
 * 그 ID 공간이 LARB별로 연속 구간으로 나뉘어 있다. 이 배열이 그 구간의
 * 시작점들이라, 어떤 ID가 어느 구간에 드는지 보면 LARB를 알 수 있다.
 * 값 출처: dt-bindings/memory/mt2701-larb-port.h.
 * 읽는 자: mt2701_m4u_to_larb()와 mt2701_m4u_to_port(). */
static const int mt2701_m4u_in_larb[] = {
	LARB0_PORT_OFFSET, LARB1_PORT_OFFSET,	/* [한국어] LARB0~1의 포트 ID 구간 시작값 — 오름차순이라 역순 탐색이 성립한다. */
	LARB2_PORT_OFFSET, LARB3_PORT_OFFSET
};

/*
 * [한국어]
 * mt2701_m4u_to_larb - M4U 포트 ID에서 LARB 번호를 알아낸다
 *
 * @id: 디바이스 트리의 iommus 프로퍼티에 실린 M4U 포트 ID.
 * @return: 그 ID가 속한 LARB 번호(0~3).
 *
 * 왜 뒤에서부터 도는가: 경계값 배열이 오름차순이므로, 뒤에서부터 훑으며
 * "id가 이 경계 이상인가"를 처음 만족하는 인덱스가 곧 답이다. 앞에서부터
 * 돌면 매번 다음 경계와도 비교해야 해서 조건이 복잡해진다.
 *
 * 실행 컨텍스트: probe_device와 config, ISR 보고 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   mtk_iommu_v1_config() / probe_device() / release_device()
 *   → [mt2701_m4u_to_larb]
 */
static inline int mt2701_m4u_to_larb(int id)
{
	/* [한국어] 역순 순회용 인덱스. */
	int i;

	/* [한국어] 마지막 LARB부터 내려오며 id가 그 시작 경계 이상인지 본다.
	 * 처음 참이 되는 지점이 이 id가 속한 LARB다. */
	for (i = ARRAY_SIZE(mt2701_m4u_in_larb) - 1; i >= 0; i--)
		if ((id) >= mt2701_m4u_in_larb[i])
			return i;

	/* [한국어] 모든 경계보다 작은 ID — 정상적인 디바이스 트리에서는
	 * 일어나지 않지만, 방어적으로 LARB 0으로 취급한다. */
	return 0;
}

/*
 * [한국어]
 * mt2701_m4u_to_port - M4U 포트 ID에서 LARB 내 포트 번호를 알아낸다
 *
 * @id: M4U 포트 ID.
 * @return: 해당 LARB 안에서의 포트 번호.
 *
 * 왜 필요한가: LARB 레지스터의 MMU 활성화 비트는 "LARB 내 포트 번호"로
 * 인덱싱된다(MTK_SMI_MMU_EN(portid)). 전역 ID에서 그 LARB의 시작 경계를
 * 빼면 곧 로컬 포트 번호가 된다.
 *
 * 실행 컨텍스트: config()에서 포트 비트를 계산할 때. 순수 계산이다.
 *
 * 호출 체인:
 *   mtk_iommu_v1_config() → [mt2701_m4u_to_port] → mt2701_m4u_to_larb()
 */
static inline int mt2701_m4u_to_port(int id)
{
	/* [한국어] 먼저 어느 LARB인지 알아낸다. */
	int larb = mt2701_m4u_to_larb(id);

	/* [한국어] 전역 ID에서 그 LARB 구간의 시작을 빼면 로컬 포트 번호다. */
	return id - mt2701_m4u_in_larb[larb];
}

/*
 * [한국어]
 * mtk_iommu_v1_tlb_flush_all - TLB 전체를 무효화한다
 *
 * @data: 대상 M4U 인스턴스.
 * @return: 없음.
 *
 * 왜 필요한가: 두 상황에서 쓰인다 — (1) 범위 무효화가 타임아웃됐을 때의
 * 대비책, (2) ISR에서 폴트 처리 후 잘못된 TLB 엔트리를 확실히 걷어내기 위해.
 *
 * 동작: INV_SEL에 EN0|EN1을 써 모든 TLB를 대상으로 지정한 뒤,
 * INVALIDATE에 F_ALL_INVLD를 쓴다. 마지막 wmb()는 이 MMIO 쓰기들이 실제로
 * 하드웨어에 도달했음을 보장한다 — 무효화가 끝나기 전에 호출자가 새 매핑을
 * 쓰기 시작하면 옛 엔트리가 남아 있을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트(ISR)와 프로세스 컨텍스트 양쪽.
 * 폴링이 없어 잠들지 않는다.
 *
 * 호출 체인:
 *   mtk_iommu_v1_isr() / mtk_iommu_v1_tlb_flush_range()의 폴백
 *   → [mtk_iommu_v1_tlb_flush_all]
 */
static void mtk_iommu_v1_tlb_flush_all(struct mtk_iommu_v1_data *data)
{
	/* [한국어] 두 무효화 대상(EN0/EN1)을 모두 선택한다. 하드웨어에 TLB가
	 * 둘로 나뉘어 있어 한쪽만 비우면 옛 매핑이 살아남는다. */
	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
			data->base + REG_MMU_INV_SEL);
	/* [한국어] 전체 무효화 명령을 낸다. */
	writel_relaxed(F_ALL_INVLD, data->base + REG_MMU_INVALIDATE);
	/* [한국어] MMIO 쓰기가 하드웨어에 실제로 도달했음을 보장한다.
	 * relaxed 접근자는 순서를 보장하지 않으므로, 무효화가 반영되기 전에
	 * 후속 코드가 진행하지 않도록 여기서 막는다. */
	wmb(); /* Make sure the tlb flush all done */
}

/*
 * [한국어]
 * mtk_iommu_v1_tlb_flush_range - 지정한 IOVA 범위의 TLB만 무효화한다
 *
 * @data: 대상 M4U 인스턴스.
 * @iova: 무효화 시작 주소.
 * @size: 무효화할 바이트 수.
 * @return: 없음.
 *
 * 왜 범위 무효화인가: 전체 무효화는 다른 클라이언트의 유효한 TLB 엔트리까지
 * 날려 성능을 크게 떨어뜨린다. 하드웨어가 범위 무효화를 지원하므로
 * map/unmap 때는 바뀐 구간만 비운다.
 *
 * 동작 과정:
 *  1) INV_SEL로 모든 TLB를 대상 지정.
 *  2) START_A/END_A에 페이지 정렬된 시작/끝 주소를 기록.
 *  3) INVALIDATE에 F_MMU_INV_RANGE를 써 명령을 낸다.
 *  4) CPE_DONE이 0이 아니게 될 때까지 폴링(10us 간격, 최대 100ms).
 *  5) 타임아웃되면 경고를 남기고 전체 무효화로 대체한다.
 *  6) CPE_DONE을 0으로 되돌려 다음 명령을 받을 수 있게 한다.
 *
 * 왜 atomic 폴링인가: 이 함수가 map/unmap 경로에서 불리고, 그 경로는
 * 인터럽트 컨텍스트일 수 있어 잠들 수 없다. readl_poll_timeout_atomic()은
 * udelay로 바쁘게 기다린다.
 *
 * 실행 컨텍스트: map/unmap 직후. 최대 100ms를 바쁘게 기다릴 수 있다.
 *
 * 호출 체인:
 *   mtk_iommu_v1_map() / mtk_iommu_v1_unmap() → [mtk_iommu_v1_tlb_flush_range]
 *   → readl_poll_timeout_atomic(), (실패 시) mtk_iommu_v1_tlb_flush_all()
 */
static void mtk_iommu_v1_tlb_flush_range(struct mtk_iommu_v1_data *data,
					 unsigned long iova, size_t size)
{
	/* [한국어] 폴링 결과(0 성공, -ETIMEDOUT 실패). */
	int ret;
	/* [한국어] 폴링 중 읽은 CPE_DONE 값을 담는 변수.
	 * readl_poll_timeout_atomic 매크로가 이 변수에 값을 넣고 조건을 평가한다. */
	u32 tmp;

	/* [한국어] 두 TLB 모두를 무효화 대상으로 지정한다. */
	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
		data->base + REG_MMU_INV_SEL);
	/* [한국어] 무효화 시작 주소. 페이지 정렬 마스크를 씌우는 이유는
	 * 하드웨어가 페이지 단위로만 무효화하기 때문이다. */
	writel_relaxed(iova & F_MMU_FAULT_VA_MSK,
		data->base + REG_MMU_INVLD_START_A);
	/* [한국어] 무효화 끝 주소(포함). size-1을 더해 마지막 바이트가 속한
	 * 페이지까지 확실히 포함시킨다. */
	writel_relaxed((iova + size - 1) & F_MMU_FAULT_VA_MSK,
		data->base + REG_MMU_INVLD_END_A);
	/* [한국어] 범위 무효화 명령을 낸다. 이 쓰기 이후 하드웨어가 비동기로
	 * 작업하고, 완료되면 CPE_DONE이 0이 아니게 된다. */
	writel_relaxed(F_MMU_INV_RANGE, data->base + REG_MMU_INVALIDATE);

	/* [한국어] CPE_DONE이 0이 아니게 될 때까지 10us 간격으로 최대 100ms
	 * 바쁘게 기다린다. atomic 변형이라 잠들지 않아 인터럽트 컨텍스트에서도
	 * 안전하다. */
	ret = readl_poll_timeout_atomic(data->base + REG_MMU_CPE_DONE,
				tmp, tmp != 0, 10, 100000);
	/* [한국어] 타임아웃 — 하드웨어가 응답하지 않는다. 무효화가 되지 않은 채
	 * 진행하면 옛 매핑으로 DMA가 나갈 수 있으므로, 더 강한 수단인 전체
	 * 무효화로 대체한다(정확성을 성능보다 우선한다). */
	if (ret) {
		dev_warn(data->dev,	/* [한국어] 범위 무효화가 응답하지 않았음을 경고로 남긴다. */
			 "Partial TLB flush timed out, falling back to full flush\n");
		mtk_iommu_v1_tlb_flush_all(data);	/* [한국어] 정확성을 위해 더 강한 전체 무효화로 대체한다. */
	}
	/* Clear the CPE status */
	/* [한국어] 완료 상태를 0으로 되돌린다. 이렇게 하지 않으면 다음 범위
	 * 무효화의 폴링이 이전 완료 신호를 보고 곧바로 성공으로 착각한다. */
	writel_relaxed(0, data->base + REG_MMU_CPE_DONE);
}

/*
 * [한국어]
 * mtk_iommu_v1_isr - 변환 폴트 인터럽트 핸들러
 *
 * @irq: 발생한 IRQ 번호(사용하지 않는다).
 * @dev_id: request_irq에 넘긴 M4U 인스턴스 포인터.
 * @return: 항상 IRQ_HANDLED — 이 IRQ는 M4U 전용이라 공유하지 않는다.
 *
 * 왜 필요한가: 클라이언트가 매핑되지 않은 IOVA에 DMA를 하면 하드웨어가
 * 인터럽트를 낸다. 이 핸들러가 어느 LARB의 어느 포트가 어떤 주소에
 * 접근했는지 로그로 남겨 디버깅을 가능하게 한다.
 *
 * 동작 과정:
 *  1) 폴트 상태/주소/물리주소/마스터ID 레지스터를 읽는다.
 *  2) 마스터 ID에서 LARB 번호와 포트 번호를 추출한다.
 *  3) report_iommu_fault()로 상위(도메인의 폴트 핸들러)에 먼저 알린다.
 *     상위가 처리하지 못하면(0이 아닌 값 반환) 직접 에러 로그를 남긴다.
 *  4) INT_CONTROL에 클리어 비트를 세워 인터럽트를 해제한다.
 *  5) TLB를 전체 무효화한다 — 폴트를 유발한 잘못된 엔트리가 캐시에 남아
 *     있을 수 있기 때문이다.
 *
 * 읽기/쓰기 구분: 원본 주석이 밝히듯 1세대 하드웨어는 폴트가 읽기인지
 * 쓰기인지 알려 주지 않는다. 그래서 무조건 IOMMU_FAULT_READ로 보고한다 —
 * 상위 핸들러가 방향에 따라 다르게 대응한다면 부정확할 수 있다는 뜻이다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. 잠들 수 없고, 로그는
 * ratelimited로 억제해 폴트 폭주 시 콘솔이 마비되지 않게 한다.
 *
 * 호출 체인:
 *   하드웨어 IRQ → [mtk_iommu_v1_isr] → report_iommu_fault(),
 *   mtk_iommu_v1_tlb_flush_all()
 */
static irqreturn_t mtk_iommu_v1_isr(int irq, void *dev_id)
{
	/* [한국어] devm_request_irq에 넘긴 M4U 인스턴스를 복원한다. */
	struct mtk_iommu_v1_data *data = dev_id;
	/* [한국어] 폴트를 보고할 도메인. 유일한 도메인이라 이 포인터로 충분하다.
	 * 아직 attach된 적이 없으면 NULL일 수 있는데, 그 상태에서는 애초에
	 * 변환이 일어나지 않아 폴트도 나지 않는다는 전제다. */
	struct mtk_iommu_v1_domain *dom = data->m4u_dom;
	/* [한국어] 폴트 상태 비트, 임시 레지스터 값, 폴트 IOVA, 폴트 물리 주소. */
	u32 int_state, regval, fault_iova, fault_pa;
	/* [한국어] 폴트를 일으킨 LARB 번호와 포트 번호. */
	unsigned int fault_larb, fault_port;

	/* Read error information from registers */
	/* [한국어] 어떤 종류의 폴트인지(F_INT_* 비트 조합). */
	int_state = readl_relaxed(data->base + REG_MMU_FAULT_ST);
	/* [한국어] 폴트를 일으킨 IOVA. */
	fault_iova = readl_relaxed(data->base + REG_MMU_FAULT_VA);

	/* [한국어] 하위 12비트는 의미가 없으므로 페이지 정렬 마스크를 씌운다. */
	fault_iova &= F_MMU_FAULT_VA_MSK;
	/* [한국어] 변환 결과로 나온(잘못된) 물리 주소. 엔트리가 손상된 경우
	 * 어떤 값이 들어 있었는지 알려 준다. */
	fault_pa = readl_relaxed(data->base + REG_MMU_INVLD_PA);
	/* [한국어] 폴트를 일으킨 마스터의 하드웨어 ID. */
	regval = readl_relaxed(data->base + REG_MMU_INT_ID);
	/* [한국어] ID에서 LARB 번호를 추출한다(하드웨어 인코딩이 역순이라
	 * 매크로가 6에서 빼는 형태다). */
	fault_larb = MT2701_M4U_TF_LARB(regval);
	/* [한국어] ID에서 포트 번호를 추출한다. 이 둘로 어느 IP가 잘못했는지
	 * 특정할 수 있다. */
	fault_port = MT2701_M4U_TF_PORT(regval);

	/*
	 * MTK v1 iommu HW could not determine whether the fault is read or
	 * write fault, report as read fault.
	 */
	/* [한국어] 먼저 도메인에 등록된 폴트 핸들러에 알린다. 그것이 폴트를
	 * 처리했다면(0 반환) 로그를 남기지 않고, 처리하지 못했다면 여기서
	 * 상세 정보를 출력한다. ratelimited인 이유는 폴트가 초당 수천 번
	 * 발생할 수 있어 콘솔이 마비되는 것을 막기 위해서다.
	 * IOMMU_FAULT_READ로 고정하는 것은 하드웨어 한계 때문이다. */
	if (report_iommu_fault(&dom->domain, data->dev, fault_iova,
			IOMMU_FAULT_READ))
		dev_err_ratelimited(data->dev,
			"fault type=0x%x iova=0x%x pa=0x%x larb=%d port=%d\n",
			int_state, fault_iova, fault_pa,
			fault_larb, fault_port);

	/* Interrupt clear */
	/* [한국어] 인터럽트를 해제한다. read-modify-write인 이유는 이 레지스터에
	 * 어떤 폴트에 인터럽트를 낼지 정하는 활성화 비트들도 함께 들어 있어,
	 * 통째로 덮어쓰면 그 설정이 사라지기 때문이다. */
	regval = readl_relaxed(data->base + REG_MMU_INT_CONTROL);
	regval |= F_INT_CLR_BIT;	/* [한국어] 클리어 비트를 세워 대기 중인 인터럽트를 해제할 준비를 한다. */
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL);

	/* [한국어] 폴트를 유발한 잘못된 TLB 엔트리가 남아 있을 수 있으므로
	 * 전체를 비운다. 범위를 특정할 수도 있겠지만, 폴트 상황에서는
	 * 안전을 택한다. */
	mtk_iommu_v1_tlb_flush_all(data);

	/* [한국어] 이 IRQ는 M4U 전용이므로 항상 "내가 처리했다"고 보고한다. */
	return IRQ_HANDLED;
}

/*
 * [한국어]
 * mtk_iommu_v1_config - 이 디바이스가 쓰는 LARB 포트들의 MMU 비트를 켜거나 끈다
 *
 * @data: M4U 인스턴스(larb_imu 배열을 갖고 있다).
 * @dev: 대상 클라이언트 디바이스(fwspec에 포트 ID들이 들어 있다).
 * @enable: true면 IOMMU를 거치게 하고, false면 물리 주소로 직통시킨다.
 * @return: 없음.
 *
 * 왜 M4U가 아니라 LARB를 건드리는가: MediaTek SoC에서 멀티미디어 IP의
 * 트래픽은 SMI LARB를 거쳐 나간다. "이 포트의 주소를 M4U로 보낼지, 아니면
 * 물리 주소 그대로 메모리로 보낼지"를 결정하는 비트가 LARB 레지스터에
 * 있다. 그래서 IOMMU를 켜고 끄는 실질적인 스위치가 여기다.
 *
 * 왜 하드웨어에 직접 쓰지 않는가: larb_mmu->mmu는 SMI LARB 드라이버와
 * 공유하는 소프트웨어 상태다. LARB는 전원이 꺼져 있을 수 있으므로,
 * 여기서는 비트만 기록해 두고 실제 레지스터 반영은 LARB 드라이버가
 * 전원 인가(runtime PM resume) 시점에 수행한다.
 *
 * 실행 컨텍스트: attach/detach 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   mtk_iommu_v1_attach_device() / identity_attach() → [mtk_iommu_v1_config]
 *   → mt2701_m4u_to_larb(), mt2701_m4u_to_port()
 */
static void mtk_iommu_v1_config(struct mtk_iommu_v1_data *data,
				struct device *dev, bool enable)
{
	/* [한국어] 갱신할 LARB의 공유 상태 구조체. */
	struct mtk_smi_larb_iommu    *larb_mmu;
	/* [한국어] 포트 ID에서 분해한 LARB 번호와 로컬 포트 번호. */
	unsigned int                 larbid, portid;
	/* [한국어] 이 디바이스의 IOMMU 펌웨어 스펙 — probe_device가 채워 둔
	 * 포트 ID 배열이 들어 있다. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 이 디바이스가 쓰는 모든 M4U 포트를 순회한다. 하나의 IP가
	 * 읽기용/쓰기용 등 여러 포트를 가질 수 있다. */
	for (i = 0; i < fwspec->num_ids; ++i) {
		/* [한국어] 전역 포트 ID에서 LARB 번호를 뽑는다. */
		larbid = mt2701_m4u_to_larb(fwspec->ids[i]);
		/* [한국어] 같은 ID에서 LARB 내 로컬 포트 번호를 뽑는다. */
		portid = mt2701_m4u_to_port(fwspec->ids[i]);
		/* [한국어] 해당 LARB의 공유 상태를 가리킨다. */
		larb_mmu = &data->larb_imu[larbid];

		/* [한국어] 어떤 포트를 켜고 끄는지 디버그 로그로 남긴다.
		 * str_enable_disable()이 bool을 문자열로 바꿔 준다. */
		dev_dbg(dev, "%s iommu port: %d\n",
			str_enable_disable(enable), portid);

		/* [한국어] 활성화: 해당 포트의 비트를 세운다. 이후 LARB 드라이버가
		 * 이 비트를 하드웨어에 반영하면 그 포트의 트래픽이 M4U를 거친다. */
		if (enable)
			larb_mmu->mmu |= MTK_SMI_MMU_EN(portid);
		else
			/* [한국어] 비활성화: 비트를 내려 그 포트가 물리 주소로
			 * 직통하게 한다(IDENTITY 도메인의 의미). */
			larb_mmu->mmu &= ~MTK_SMI_MMU_EN(portid);
	}
}

/*
 * [한국어]
 * mtk_iommu_v1_domain_finalise - 유일한 도메인의 페이지 테이블을 실제로 만든다
 *
 * @data: M4U 인스턴스. data->m4u_dom이 이미 설정되어 있어야 한다.
 * @return: 0 성공, -ENOMEM(4MB 테이블 할당 실패).
 *
 * 왜 alloc이 아니라 여기서 만드는가: domain_alloc_paging 시점에는 어느 M4U에
 * 붙을지 몰라 DMA 마스터를 알 수 없다. 첫 attach가 되어야 비로소
 * dma_alloc_coherent를 부를 수 있다.
 *
 * 동작 과정:
 *  1) 페이지 테이블 락을 초기화한다.
 *  2) 4MB 코히런트 DMA 메모리를 할당한다 — IOVA 4GB 전체를 덮는 평면 배열.
 *  3) 그 물리 주소를 PT_BASE_ADDR 레지스터에 기록한다. 이 순간부터
 *     하드웨어가 이 배열을 페이지 테이블로 쓴다.
 *  4) 도메인이 이 M4U에 속함을 기록한다.
 *
 * 4MB 연속 코히런트 메모리 할당은 결코 가볍지 않다 — 부팅 초기가 아니면
 * 실패할 수 있다는 점이 이 설계의 약점이다.
 *
 * 실행 컨텍스트: attach 경로(프로세스 컨텍스트, GFP_KERNEL).
 *
 * 호출 체인:
 *   mtk_iommu_v1_attach_device() → [mtk_iommu_v1_domain_finalise]
 *   → dma_alloc_coherent(), writel()
 */
static int mtk_iommu_v1_domain_finalise(struct mtk_iommu_v1_data *data)
{
	/* [한국어] 완성할 도메인. 호출자가 이미 data->m4u_dom에 넣어 두었다. */
	struct mtk_iommu_v1_domain *dom = data->m4u_dom;

	/* [한국어] 평면 테이블을 보호할 스핀락을 초기화한다. 테이블이 생기는
	 * 시점과 락이 유효해지는 시점을 맞춘다. */
	spin_lock_init(&dom->pgtlock);

	/* [한국어] 4GB IOVA 전체를 덮는 4MB 평면 배열을 코히런트 DMA로 잡는다.
	 * 0으로 초기화되어 모든 엔트리가 "매핑 없음"(유효 비트 0) 상태다.
	 * 코히런트여야 하는 이유는 하드웨어가 이 배열을 직접 읽기 때문이다. */
	dom->pgt_va = dma_alloc_coherent(data->dev, M2701_IOMMU_PGT_SIZE,
					 &dom->pgt_pa, GFP_KERNEL);
	/* [한국어] 4MB 연속 메모리 확보 실패 — 메모리가 단편화된 상태에서는
	 * 충분히 일어날 수 있는 실패다. */
	if (!dom->pgt_va)
		return -ENOMEM;

	/* [한국어] 하드웨어에 테이블 위치를 알린다. relaxed가 아닌 writel을
	 * 쓰는 이유는, 앞선 dma_alloc_coherent의 메모리 초기화가 하드웨어에
	 * 보인 뒤에 이 쓰기가 이뤄져야 하기 때문이다(writel은 배리어를 포함한다). */
	writel(dom->pgt_pa, data->base + REG_MMU_PT_BASE_ADDR);

	/* [한국어] 도메인이 이 M4U에 속함을 기록한다. 이후 map/unmap이 이
	 * 포인터로 TLB 무효화를 요청한다. */
	dom->data = data;

	/* [한국어] 도메인 준비 완료. */
	return 0;
}

/*
 * [한국어]
 * mtk_iommu_v1_domain_alloc_paging - 페이징 도메인 껍데기를 만든다
 *
 * @dev: 요청한 디바이스(사용하지 않는다).
 * @return: 새 도메인의 iommu_domain 포인터, 메모리 부족이면 NULL.
 *
 * 왜 이것만 하는가: 실제 페이지 테이블 할당과 하드웨어 등록은
 * domain_finalise()가 첫 attach 때 수행한다. 여기서는 구조체를 만들고
 * 지원 페이지 크기만 알린다.
 *
 * 주의: geometry(aperture)를 설정하지 않는다. IOVA 범위 관리는 ARM 레거시
 * DMA 매핑(arm_iommu_create_mapping의 4GB)이 담당하므로, 코어 쪽 범위
 * 검사에 기대지 않는 구조다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   arm_iommu_create_mapping() → iommu_domain_alloc()
 *   → iommu_ops->domain_alloc_paging → [mtk_iommu_v1_domain_alloc_paging]
 */
static struct iommu_domain *mtk_iommu_v1_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 새로 만들 도메인 구조체. */
	struct mtk_iommu_v1_domain *dom;

	/* [한국어] 0으로 초기화해 할당한다. pgt_va와 data가 NULL로 시작해야
	 * "아직 finalise되지 않음"을 판별할 수 있다. */
	dom = kzalloc_obj(*dom);
	/* [한국어] 메모리 부족 — 코어에 NULL로 알린다. */
	if (!dom)
		return NULL;

	/* [한국어] 지원 페이지 크기는 4KB 하나뿐이다. 코어가 이 비트맵을 보고
	 * map 요청을 4KB 단위로 쪼갠다. */
	dom->domain.pgsize_bitmap = MT2701_IOMMU_PAGE_SIZE;

	/* [한국어] 코어에는 임베드된 일반 도메인 포인터를 돌려준다. */
	return &dom->domain;
}

/*
 * [한국어]
 * mtk_iommu_v1_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 왜 위험한가: dom->data를 무조건 역참조하므로, finalise되지 않은 도메인
 * (즉 한 번도 attach되지 않은 도메인)을 해제하면 NULL 역참조가 난다.
 * 실제로는 이 드라이버가 만드는 도메인이 ARM DMA 매핑에 붙은 하나뿐이고
 * 그것은 반드시 attach되므로 문제가 드러나지 않는다. 코드는 고치지 않고
 * 사실만 기록해 둔다.
 *
 * 또 하나: 하드웨어의 PT_BASE_ADDR를 0으로 되돌리지 않는다. 해제된 메모리를
 * 하드웨어가 계속 테이블로 참조할 여지가 남지만, 이 역시 실제로는 시스템
 * 종료 시점에만 일어나는 경로다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free → [mtk_iommu_v1_domain_free]
 *   → dma_free_coherent(), kfree()
 */
static void mtk_iommu_v1_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	/* [한국어] DMA 메모리를 반납하려면 할당 때의 마스터 디바이스가 필요하다. */
	struct mtk_iommu_v1_data *data = dom->data;

	/* [한국어] 4MB 평면 테이블을 반납한다. 할당 때와 같은 크기를 넘겨야 한다. */
	dma_free_coherent(data->dev, M2701_IOMMU_PGT_SIZE,
			dom->pgt_va, dom->pgt_pa);
	/* [한국어] 도메인 구조체를 반납한다. 위에서 이미 dom을 구했지만
	 * 원본은 여기서 to_mtk_domain을 다시 호출한다 — 결과는 같다. */
	kfree(to_mtk_domain(domain));
}

/*
 * [한국어]
 * mtk_iommu_v1_attach_device - 디바이스를 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙일 클라이언트 디바이스.
 * @old: 직전 도메인(사용하지 않는다).
 * @return: 0 성공(또는 무시), domain_finalise가 낸 음수 errno.
 *
 * 이 함수의 특이한 첫 검사: `mtk_mapping->domain != domain`이면 그냥 0을
 * 반환하고 아무것도 하지 않는다. 왜냐하면 이 하드웨어는 도메인을 하나만
 * 가질 수 있어, 드라이버가 내부적으로 만든(=ARM DMA 매핑에 붙은) 도메인
 * 외의 도메인은 지원할 수 없기 때문이다. 실패로 보고하지 않고 조용히
 * 넘어가는 것은 상위 계층(예: VFIO)이 다른 도메인을 붙이려 할 때
 * 에러 대신 무시로 동작하게 하려는 선택이다.
 *
 * 동작 과정:
 *  1) 내부 도메인이 맞는지 확인. 아니면 무시하고 성공 반환.
 *  2) 아직 도메인이 없으면 이 도메인을 유일 도메인으로 등록하고
 *     finalise(테이블 할당 + 하드웨어 등록)한다. 실패하면 되돌린다.
 *  3) 이 디바이스가 쓰는 LARB 포트들의 MMU 비트를 켠다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. IOMMU 코어가 직렬화한다.
 *
 * 호출 체인:
 *   arm_iommu_attach_device() → iommu_attach_device() → domain_ops->attach_dev
 *   → [mtk_iommu_v1_attach_device] → mtk_iommu_v1_domain_finalise(),
 *     mtk_iommu_v1_config()
 */
static int mtk_iommu_v1_attach_device(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	/* [한국어] create_mapping이 심어 둔 M4U 인스턴스. */
	struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);
	/* [한국어] 붙일 도메인을 이 드라이버의 형태로 복원한다. */
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	/* [한국어] 이 M4U가 관리하는 ARM DMA 매핑. */
	struct dma_iommu_mapping *mtk_mapping;
	/* [한국어] finalise 결과. */
	int ret;

	/* Only allow the domain created internally. */
	/* [한국어] 이 드라이버가 만든 매핑의 도메인만 받아들인다. */
	mtk_mapping = data->mapping;
	/* [한국어] 외부(VFIO 등)가 만든 도메인이면 지원할 수 없다. 하드웨어가
	 * 도메인을 하나만 가질 수 있기 때문이다. 에러 대신 0을 반환해
	 * "붙은 척"하는데, 이는 상위 계층을 깨뜨리지 않으려는 타협이다. */
	if (mtk_mapping->domain != domain)
		return 0;

	/* [한국어] 아직 이 M4U에 도메인이 없다면 지금 붙이는 것이 첫 번째다. */
	if (!data->m4u_dom) {
		/* [한국어] 유일 도메인으로 등록한다. finalise가 이 필드를 읽으므로
		 * 호출 전에 먼저 설정해야 한다. */
		data->m4u_dom = dom;
		/* [한국어] 4MB 테이블을 할당하고 하드웨어에 등록한다. */
		ret = mtk_iommu_v1_domain_finalise(data);
		/* [한국어] 실패하면 등록을 되돌려 다음 attach가 다시 시도할 수
		 * 있게 한다 — 이 되감기가 없으면 반쯤 초기화된 도메인이 남는다. */
		if (ret) {
			data->m4u_dom = NULL;	/* [한국어] finalise 실패를 되돌려 다음 attach가 다시 시도할 수 있게 한다. */
			return ret;	/* [한국어] 테이블을 만들지 못했으므로 attach를 실패로 끝낸다. */
		}
	}

	/* [한국어] 이 디바이스가 쓰는 LARB 포트들을 M4U 경유로 전환한다.
	 * 이 호출이 실질적으로 "IOMMU를 켜는" 동작이다. */
	mtk_iommu_v1_config(data, dev, true);
	return 0;	/* [한국어] LARB 포트까지 켰으니 attach 성공이다. */
}

/*
 * [한국어]
 * mtk_iommu_v1_identity_attach - 디바이스를 항등(identity) 도메인으로 되돌린다
 *
 * @identity_domain: 정적 항등 도메인(사용하지 않는다).
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(사용하지 않는다).
 * @return: 항상 0.
 *
 * 왜 이것만으로 충분한가: 이 하드웨어에서 "IOMMU를 거치지 않는다"는 곧
 * "LARB 포트의 MMU 비트를 내린다"이다. 비트가 0이면 그 포트의 트래픽은
 * 주소 변환 없이 물리 주소 그대로 메모리로 간다 — 정확히 항등 변환이다.
 * 페이지 테이블은 그대로 두어도 무방하다(하드웨어가 참조하지 않는다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(detach 또는 기본 도메인 전환).
 *
 * 호출 체인:
 *   iommu_detach_device()/도메인 전환 → domain_ops->attach_dev
 *   → [mtk_iommu_v1_identity_attach] → mtk_iommu_v1_config(false)
 */
static int mtk_iommu_v1_identity_attach(struct iommu_domain *identity_domain,
					struct device *dev,
					struct iommu_domain *old)
{
	/* [한국어] 대상 M4U 인스턴스를 꺼낸다. */
	struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);

	/* [한국어] LARB 포트의 MMU 비트를 내려 변환을 우회시킨다. */
	mtk_iommu_v1_config(data, dev, false);
	return 0;	/* [한국어] 변환 우회 설정이 끝났으므로 성공을 반환한다. */
}

/* [한국어] 항등 도메인의 연산 테이블. attach_dev 하나뿐인 이유는 이 도메인이
 * 상태를 갖지 않고 "변환 없음"만 뜻하기 때문이다 — 해제할 것도 매핑할 것도 없다. */
static struct iommu_domain_ops mtk_iommu_v1_identity_ops = {
	/* [한국어] LARB 포트의 MMU 비트를 내리는 콜백. */
	.attach_dev = mtk_iommu_v1_identity_attach,
};

/* [한국어] 정적 항등 도메인. 상태가 없어 인스턴스 하나를 모든 디바이스가
 * 공유해도 문제가 없다. iommu_ops의 .identity_domain으로 등록되어,
 * 코어가 "IOMMU를 우회하라"는 요청을 이 도메인으로 표현한다. */
static struct iommu_domain mtk_iommu_v1_identity_domain = {
	/* [한국어] 도메인 종류 — 코어가 이 값으로 항등 도메인임을 안다. */
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 위에서 정의한 콜백 하나짜리 테이블. */
	.ops = &mtk_iommu_v1_identity_ops,
};

/*
 * [한국어]
 * mtk_iommu_v1_map - IOVA 구간에 물리 페이지들을 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑 시작 IOVA.
 * @paddr: 물리 주소 시작(연속이어야 한다).
 * @pgsize: 페이지 크기(항상 4KB).
 * @pgcount: 매핑할 페이지 개수.
 * @prot: 보호 플래그 — **이 하드웨어는 권한 비트가 없어 무시된다.**
 * @gfp: 할당 플래그 — 새로 할당할 것이 없어 쓰이지 않는다.
 * @mapped: 출력 인자 — 실제 매핑한 바이트 수.
 * @return: 요청 전부를 매핑했으면 0, 도중에 기존 매핑을 만났으면 -EEXIST.
 *
 * 왜 도중에 멈출 수 있는가: 루프가 이미 값이 들어 있는 엔트리를 만나면
 * break 한다. 이는 "먼저 unmap 하라"는 IOMMU API 규약을 지키는 것이다.
 * 다만 그때까지 쓴 엔트리는 되돌리지 않고, *mapped에 실제 처리량을 담아
 * -EEXIST와 함께 반환한다 — 상위 계층이 그 값을 보고 정리한다.
 *
 * 엔트리 형식: 물리 주소 | F_DESC_VALID | F_DESC_NONSEC.
 * 물리 주소가 4KB 정렬이라 하위 12비트가 비어 있고, 그 자리에 플래그를
 * 넣는다. sprd처럼 PFN으로 시프트하지 않는 점이 다르다.
 *
 * 실행 컨텍스트: DMA API 경로(atomic 가능) — irqsave 스핀락.
 * 락 밖에서 TLB 범위 무효화를 수행하는데, 이때 최대 100ms를 바쁘게
 * 기다릴 수 있다는 점에 유의.
 *
 * 호출 체인:
 *   iommu_map() → domain_ops->map_pages → [mtk_iommu_v1_map]
 *   → mtk_iommu_v1_tlb_flush_range()
 */
static int mtk_iommu_v1_map(struct iommu_domain *domain, unsigned long iova,
			    phys_addr_t paddr, size_t pgsize, size_t pgcount,
			    int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 엔트리 기록 루프의 인덱스이자, 실제 매핑한 개수가 된다. */
	unsigned int i;
	/* [한국어] 기록을 시작할 엔트리의 주소. IOVA를 그대로 페이지 시프트해
	 * 인덱스로 쓴다 — aperture 시작이 0이라 뺄 것이 없다.
	 * iova 뒤의 공백 두 칸은 원본 그대로다. */
	u32 *pgt_base_iova = dom->pgt_va + (iova  >> MT2701_IOMMU_PAGE_SHIFT);
	/* [한국어] 물리 주소를 32비트로 자른다. 이 하드웨어는 32비트 물리
	 * 주소 공간만 다룬다. */
	u32 pabase = (u32)paddr;

	/* [한국어] 평면 테이블 접근을 직렬화한다. */
	spin_lock_irqsave(&dom->pgtlock, flags);
	/* [한국어] 요청된 페이지 수만큼 엔트리를 채운다. */
	for (i = 0; i < pgcount; i++) {
		/* [한국어] 이미 값이 있으면 기존 매핑이다. 덮어쓰지 않고 멈춰
		 * "먼저 unmap 하라"는 규약을 지킨다. i가 여기서 멈춘 값이
		 * 곧 실제 매핑한 개수가 된다. */
		if (pgt_base_iova[i])
			break;
		/* [한국어] 물리 주소에 유효 비트와 비보안 비트를 얹어 기록한다.
		 * 주소가 4KB 정렬이라 하위 비트가 비어 있어 OR로 충분하다. */
		pgt_base_iova[i] = pabase | F_DESC_VALID | F_DESC_NONSEC;
		/* [한국어] 다음 물리 페이지로 전진한다. */
		pabase += MT2701_IOMMU_PAGE_SIZE;
	}

	spin_unlock_irqrestore(&dom->pgtlock, flags);

	/* [한국어] 실제로 매핑한 바이트 수를 코어에 알린다. 도중에 멈췄다면
	 * 요청보다 작은 값이 된다. */
	*mapped = i * MT2701_IOMMU_PAGE_SIZE;
	/* [한국어] 새로 쓴 구간의 TLB를 무효화한다. 이 하드웨어는 매핑을
	 * 추가할 때도 무효화가 필요한데, 옛 "매핑 없음" 결과가 TLB에 캐시되어
	 * 있을 수 있기 때문이다. 락 밖에서 하는 이유는 최대 100ms를 기다릴
	 * 수 있어 락 보유 시간을 줄이려는 것이다. */
	mtk_iommu_v1_tlb_flush_range(dom->data, iova, *mapped);

	/* [한국어] 전부 처리했으면 성공, 도중에 기존 매핑을 만나 멈췄으면
	 * -EEXIST로 알린다. */
	return i == pgcount ? 0 : -EEXIST;
}

/*
 * [한국어]
 * mtk_iommu_v1_unmap - IOVA 구간의 매핑을 제거한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제 시작 IOVA.
 * @pgsize: 페이지 크기(항상 4KB).
 * @pgcount: 해제할 페이지 개수.
 * @gather: TLB 무효화 수집 구조체 — 이 드라이버는 즉시 무효화하므로 쓰지 않는다.
 * @return: 해제한 바이트 수(항상 요청 전부).
 *
 * 왜 memset 하나인가: 엔트리가 u32 하나이고 0이 곧 "매핑 없음"이므로,
 * 구간을 0으로 채우는 것이 해제의 전부다. 해제할 중간 테이블도 없다.
 *
 * gather를 쓰지 않는 이유: 여기서 곧바로 범위 무효화를 수행하기 때문이다.
 * 코어가 나중에 iotlb_sync를 부르는 방식과 달리, 이 드라이버는 unmap마다
 * 즉시 하드웨어를 갱신한다(iotlb_sync 콜백 자체를 등록하지 않는다).
 *
 * 실행 컨텍스트: DMA API 경로(atomic 가능) — irqsave 스핀락.
 *
 * 호출 체인:
 *   iommu_unmap() → domain_ops->unmap_pages → [mtk_iommu_v1_unmap]
 *   → mtk_iommu_v1_tlb_flush_range()
 */
static size_t mtk_iommu_v1_unmap(struct iommu_domain *domain, unsigned long iova,
				 size_t pgsize, size_t pgcount,
				 struct iommu_iotlb_gather *gather)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 지우기를 시작할 엔트리 주소(map과 같은 인덱스 계산). */
	u32 *pgt_base_iova = dom->pgt_va + (iova  >> MT2701_IOMMU_PAGE_SHIFT);
	/* [한국어] 해제할 총 바이트 수 — 반환값이기도 하다. */
	size_t size = pgcount * MT2701_IOMMU_PAGE_SIZE;

	/* [한국어] 평면 테이블 접근을 직렬화한다. */
	spin_lock_irqsave(&dom->pgtlock, flags);
	/* [한국어] 해당 엔트리들을 0으로 채운다 — 유효 비트가 사라져
	 * 하드웨어가 "매핑 없음"으로 본다. */
	memset(pgt_base_iova, 0, pgcount * sizeof(u32));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	/* [한국어] 지운 구간의 TLB를 즉시 무효화한다. 이것을 빼먹으면
	 * 해제된 IOVA로 DMA가 계속 성공해 심각한 보안 문제가 된다. */
	mtk_iommu_v1_tlb_flush_range(dom->data, iova, size);

	/* [한국어] 요청 전부를 처리했으므로 요청 크기를 그대로 반환한다. */
	return size;
}

/*
 * [한국어]
 * mtk_iommu_v1_iova_to_phys - IOVA를 물리 주소로 변환한다(소프트웨어 조회)
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 페이지 정렬된 물리 주소. 매핑이 없으면 0.
 *
 * 주의: 페이지 내 오프셋을 더하지 않는다. 즉 반환값은 항상 4KB 정렬이며,
 * 호출자가 오프셋을 직접 더해야 정확한 주소가 된다. sprd 드라이버가
 * 오프셋까지 더해 주는 것과 대비되는 지점이다(IOMMU API 규약상으로는
 * 오프셋을 포함해야 하므로, 엄밀히는 불완전한 구현이다 — 코드는 고치지 않는다).
 *
 * 마스킹의 의미: 엔트리에서 하위 12비트(F_DESC_VALID, F_DESC_NONSEC 등
 * 플래그 자리)를 지워 순수한 물리 주소만 남긴다. 매핑이 없으면 엔트리가
 * 0이므로 결과도 0이 되어 자연스럽게 실패를 표현한다.
 *
 * 실행 컨텍스트: 프로세스 또는 atomic — irqsave 스핀락으로 보호한다.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops->iova_to_phys
 *   → [mtk_iommu_v1_iova_to_phys]
 */
static phys_addr_t mtk_iommu_v1_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 계산 결과인 물리 주소. */
	phys_addr_t pa;

	/* [한국어] 테이블 접근을 직렬화한다. */
	spin_lock_irqsave(&dom->pgtlock, flags);
	/* [한국어] IOVA를 인덱스로 엔트리를 읽는다(주소 + 플래그가 함께 들어 있다). */
	pa = *(dom->pgt_va + (iova >> MT2701_IOMMU_PAGE_SHIFT));
	/* [한국어] 하위 12비트의 플래그를 지워 순수 물리 주소만 남긴다.
	 * 매핑이 없으면 엔트리가 0이라 결과도 0이 된다. */
	pa = pa & (~(MT2701_IOMMU_PAGE_SIZE - 1));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	/* [한국어] 페이지 정렬된 물리 주소를 반환한다(오프셋 미포함). */
	return pa;
}

/* [한국어] iommu_ops의 전방 선언. 아래 create_mapping()보다 뒤에 정의되지만,
 * 실제로는 파일 끝에 몰아 두려는 배치상의 선언이다. */
static const struct iommu_ops mtk_iommu_v1_ops;

/*
 * MTK generation one iommu HW only support one iommu domain, and all the client
 * sharing the same iova address space.
 */
/*
 * [한국어]
 * mtk_iommu_v1_create_mapping - fwspec을 만들고 ARM DMA 매핑을 준비한다
 *
 * @dev: iommus 프로퍼티를 가진 클라이언트 디바이스.
 * @args: 파싱된 phandle 인자(args->np가 M4U 노드, args->args[0]이 포트 ID).
 * @return: 0 성공, -EINVAL(#iommu-cells가 1이 아님/M4U 디바이스 없음),
 *          fwspec 초기화나 ARM 매핑 생성이 낸 음수 errno.
 *
 * 왜 of_xlate가 아닌가: 이 드라이버는 iommu_ops에 .of_xlate를 등록하지 않고,
 * probe_device 안에서 iommus 프로퍼티를 직접 순회하며 이 함수를 부른다.
 * ARM 레거시 DMA 매핑 생성이라는 추가 작업이 필요해, 표준 of_xlate 경로에
 * 맞지 않기 때문이다.
 *
 * 동작 과정:
 *  1) #iommu-cells가 1인지 검증(포트 ID 하나만 받는다).
 *  2) fwspec을 초기화한다(이미 있으면 그대로 쓴다).
 *  3) priv가 비어 있으면 M4U 플랫폼 디바이스를 찾아 drvdata를 심는다.
 *  4) 포트 ID를 fwspec에 추가한다.
 *  5) ARM DMA 매핑이 아직 없으면 4GB 크기로 하나 만든다 — 이 매핑이
 *     내부적으로 유일한 도메인을 만들어 낸다.
 *
 * 원본 주석이 밝히듯 이 하드웨어는 도메인이 하나뿐이고 모든 클라이언트가
 * 같은 IOVA 공간을 공유한다. 그래서 매핑도 M4U당 하나만 만든다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   mtk_iommu_v1_probe_device() → [mtk_iommu_v1_create_mapping]
 *   → iommu_fwspec_init(), of_find_device_by_node(), iommu_fwspec_add_ids(),
 *     arm_iommu_create_mapping()
 */
static int mtk_iommu_v1_create_mapping(struct device *dev,
				       const struct of_phandle_args *args)
{
	/* [한국어] 이 디바이스를 담당할 M4U 인스턴스. */
	struct mtk_iommu_v1_data *data;
	/* [한국어] phandle이 가리키는 M4U 플랫폼 디바이스. */
	struct platform_device *m4updev;
	/* [한국어] 4GB IOVA 공간을 관리할 ARM DMA 매핑. */
	struct dma_iommu_mapping *mtk_mapping;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 이 M4U의 디바이스 트리 바인딩은 셀 하나(포트 ID)만 받는다.
	 * 다른 값이면 디바이스 트리가 잘못된 것이므로 거부한다. */
	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",	/* [한국어] 디바이스 트리 바인딩이 어긋났음을 구체적으로 남긴다. */
			args->args_count);
		return -EINVAL;	/* [한국어] 셀 개수가 맞지 않으면 포트 ID를 해석할 수 없다. */
	}

	/* [한국어] 이 디바이스의 IOMMU 펌웨어 스펙을 초기화한다. 이미 초기화된
	 * 경우(두 번째 iommus 항목)에는 기존 것을 유지하고 성공을 반환한다. */
	ret = iommu_fwspec_init(dev, of_fwnode_handle(args->np));
	if (ret)	/* [한국어] fwspec 초기화 실패를 그대로 상위에 전달한다. */
		return ret;

	/* [한국어] 아직 담당 M4U가 정해지지 않았다면 지금 찾아 심는다.
	 * 두 번째 이후 iommus 항목에서는 이 블록을 건너뛴다. */
	if (!dev_iommu_priv_get(dev)) {
		/* Get the m4u device */
		/* [한국어] phandle 노드에 대응하는 플랫폼 디바이스를 찾는다.
		 * 참조 카운트가 하나 올라간다. */
		m4updev = of_find_device_by_node(args->np);
		/* [한국어] M4U 디바이스가 없다는 것은 디바이스 트리와 실제
		 * 드라이버 바인딩이 어긋난 심각한 상황이라 WARN을 남긴다. */
		if (WARN_ON(!m4updev))
			return -EINVAL;

		/* [한국어] M4U의 drvdata(= mtk_iommu_v1_data)를 클라이언트의
		 * priv에 심는다. 이후 attach/config가 이 포인터로 M4U에 접근한다. */
		dev_iommu_priv_set(dev, platform_get_drvdata(m4updev));

		/* [한국어] 올린 참조를 내린다. 포인터 자체는 M4U가 살아 있는 동안
		 * 유효하므로 참조를 유지할 필요가 없다. */
		put_device(&m4updev->dev);
	}

	/* [한국어] 이 iommus 항목의 포트 ID를 fwspec에 추가한다. 여러 항목이
	 * 있으면 호출이 반복되어 ID가 누적된다 — config()가 그 배열을 순회한다. */
	ret = iommu_fwspec_add_ids(dev, args->args, 1);
	if (ret)	/* [한국어] 포트 ID 추가 실패(메모리 부족 등)를 전달한다. */
		return ret;

	/* [한국어] 방금 심었거나 이미 있던 M4U 인스턴스를 다시 꺼낸다. */
	data = dev_iommu_priv_get(dev);
	/* [한국어] 이 M4U의 ARM DMA 매핑을 확인한다. */
	mtk_mapping = data->mapping;
	/* [한국어] 아직 없으면 지금 만든다. M4U당 한 번만 생성되며, 그 안에서
	 * iommu_domain_alloc이 불려 유일한 도메인이 태어난다. */
	if (!mtk_mapping) {
		/* MTK iommu support 4GB iova address space. */
		/* [한국어] IOVA 0부터 4GB까지를 관리하는 매핑을 만든다.
		 * 1ULL << 32로 쓴 것은 32비트 오버플로를 피하기 위함이다. */
		mtk_mapping = arm_iommu_create_mapping(dev, 0, 1ULL << 32);
		/* [한국어] 생성 실패(메모리 부족 등) — 이 디바이스는 DMA를
		 * 쓸 수 없게 된다. */
		if (IS_ERR(mtk_mapping))
			return PTR_ERR(mtk_mapping);

		/* [한국어] 이후 모든 클라이언트가 이 매핑을 공유하도록 보관한다. */
		data->mapping = mtk_mapping;
	}

	/* [한국어] 이 iommus 항목의 처리가 끝났다. */
	return 0;
}

/*
 * [한국어]
 * mtk_iommu_v1_probe_device - 클라이언트 디바이스를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당하면 &data->iommu, 아니면 ERR_PTR(음수 errno).
 *
 * 왜 여기서 iommus를 직접 파싱하는가: .of_xlate를 등록하지 않았기 때문이다.
 * 코어가 대신 해 주는 일을 이 함수가 직접 한다 — 프로퍼티를 순회하며
 * create_mapping()을 부르고 fwspec을 쌓는다.
 *
 * 동작 과정:
 *  1) iommus 프로퍼티의 모든 항목을 순회하며 create_mapping()을 호출한다.
 *     of_node_put으로 각 항목의 참조를 즉시 내린다.
 *  2) 한 항목도 없었다면(fwspec == NULL) 이 IOMMU 소관이 아니다 → -ENODEV.
 *  3) 모든 포트 ID가 **같은 LARB**에 속하는지 검사한다. 다르면 -EINVAL.
 *  4) 그 LARB 디바이스에 device_link를 걸어 전원/제거 순서를 묶는다.
 *
 * 왜 하나의 LARB만 허용하는가: release_device()가 fwspec->ids[0]으로
 * LARB를 찾아 링크를 제거하기 때문이다. 여러 LARB에 걸치면 나머지 링크를
 * 정리할 방법이 없어, 아예 금지한다.
 *
 * device_link의 역할: DL_FLAG_PM_RUNTIME으로 클라이언트가 활성화될 때
 * LARB도 함께 깨어나게 한다. LARB가 잠들어 있으면 MMU 비트가 하드웨어에
 * 반영되지 않기 때문이다. DL_FLAG_STATELESS는 드라이버가 링크 수명을
 * 직접 관리하겠다는 뜻이다(release_device에서 제거한다).
 *
 * 실행 컨텍스트: 디바이스 probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iommu_probe_device() → iommu_ops->probe_device → [mtk_iommu_v1_probe_device]
 *   → of_parse_phandle_with_args(), mtk_iommu_v1_create_mapping(),
 *     mt2701_m4u_to_larb(), device_link_add()
 */
static struct iommu_device *mtk_iommu_v1_probe_device(struct device *dev)
{
	/* [한국어] 순회 후 확보되는 펌웨어 스펙. 한 항목도 없으면 NULL로 남아
	 * "이 디바이스는 내 소관이 아니다"의 판별 기준이 된다. */
	struct iommu_fwspec *fwspec = NULL;
	/* [한국어] 파싱된 iommus 항목 하나를 담는다. */
	struct of_phandle_args iommu_spec;
	/* [한국어] 담당 M4U 인스턴스. */
	struct mtk_iommu_v1_data *data;
	/* [한국어] err는 오류 코드, idx는 순회 인덱스, larbid/larbidx는
	 * LARB 일치 검사에 쓰는 번호들. */
	int err, idx = 0, larbid, larbidx;
	/* [한국어] 클라이언트와 LARB를 잇는 디바이스 링크. */
	struct device_link *link;
	/* [한국어] 링크의 공급자가 될 LARB 디바이스. */
	struct device *larbdev;

	/* [한국어] iommus 프로퍼티의 항목을 하나씩 파싱한다. 더 이상 없으면
	 * of_parse_phandle_with_args가 0이 아닌 값을 반환해 루프가 끝난다. */
	while (!of_parse_phandle_with_args(dev->of_node, "iommus",
					   "#iommu-cells",
					   idx, &iommu_spec)) {

		/* [한국어] 이 항목으로 fwspec을 확장하고, 필요하면 M4U 연결과
		 * ARM DMA 매핑을 준비한다. */
		err = mtk_iommu_v1_create_mapping(dev, &iommu_spec);
		/* [한국어] 파싱이 올려 둔 노드 참조를 즉시 내린다 — 성공/실패
		 * 여부와 무관하게 반드시 필요하다. */
		of_node_put(iommu_spec.np);
		/* [한국어] 실패하면 그 오류를 ERR_PTR로 감싸 반환한다. */
		if (err)
			return ERR_PTR(err);

		/* dev->iommu_fwspec might have changed */
		/* [한국어] create_mapping이 fwspec을 새로 만들거나 재할당했을 수
		 * 있으므로 매 반복마다 다시 읽는다. 캐싱하면 해제된 포인터를
		 * 들고 있을 위험이 있다. */
		fwspec = dev_iommu_fwspec_get(dev);
		idx++;	/* [한국어] 다음 iommus 항목으로 넘어간다. */
	}

	/* [한국어] iommus 항목이 하나도 없었다 = 이 디바이스는 IOMMU를 쓰지
	 * 않는다. 코어에 -ENODEV로 알려 다른 드라이버를 시도하게 한다. */
	if (!fwspec)
		return ERR_PTR(-ENODEV);

	/* [한국어] create_mapping이 심어 둔 M4U 인스턴스를 꺼낸다. */
	data = dev_iommu_priv_get(dev);

	/* Link the consumer device with the smi-larb device(supplier) */
	/* [한국어] 첫 번째 포트 ID로 이 디바이스가 속한 LARB를 알아낸다. */
	larbid = mt2701_m4u_to_larb(fwspec->ids[0]);
	/* [한국어] MT2701이 실제로 가진 LARB 개수를 넘으면 디바이스 트리가
	 * 잘못된 것이다 — larb_imu 배열 밖 접근을 막는 방어이기도 하다. */
	if (larbid >= MT2701_LARB_NR_MAX)
		return ERR_PTR(-EINVAL);

	/* [한국어] 나머지 포트 ID들도 모두 같은 LARB에 속하는지 확인한다. */
	for (idx = 1; idx < fwspec->num_ids; idx++) {
		/* [한국어] 이 ID의 LARB 번호를 구한다. */
		larbidx = mt2701_m4u_to_larb(fwspec->ids[idx]);
		/* [한국어] 하나라도 다르면 거부한다. release_device()가
		 * ids[0]의 LARB만 정리하므로, 여러 LARB에 걸치면 링크가 샌다. */
		if (larbid != larbidx) {
			dev_err(dev, "Can only use one larb. Fail@larb%d-%d.\n",	/* [한국어] 여러 LARB에 걸친 디바이스는 링크 정리가 불가능하다는 것을 남긴다. */
				larbid, larbidx);
			return ERR_PTR(-EINVAL);	/* [한국어] 하나의 LARB만 허용한다는 제약을 위반했으므로 거부한다. */
		}
	}

	/* [한국어] component bind가 채워 둔 LARB 디바이스 포인터를 꺼낸다. */
	larbdev = data->larb_imu[larbid].dev;
	/* [한국어] 아직 LARB가 바인딩되지 않았다면 포트를 제어할 수 없다. */
	if (!larbdev)
		return ERR_PTR(-EINVAL);

	/* [한국어] 클라이언트(소비자)와 LARB(공급자) 사이에 링크를 건다.
	 * PM_RUNTIME: 클라이언트가 활성화되면 LARB도 함께 깨어난다 —
	 *             LARB가 잠들어 있으면 MMU 비트가 반영되지 않는다.
	 * STATELESS: 링크 수명을 드라이버가 직접 관리한다(release_device에서 제거). */
	link = device_link_add(dev, larbdev,
			       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
	/* [한국어] 링크 생성 실패는 치명적이지 않아 경고만 남기고 계속한다 —
	 * 다만 전원 순서가 보장되지 않아 이후 문제가 생길 수 있다. */
	if (!link)
		dev_err(dev, "Unable to link %s\n", dev_name(larbdev));

	/* [한국어] 이 디바이스를 담당하는 IOMMU 핸들을 돌려준다. */
	return &data->iommu;
}

/*
 * [한국어]
 * mtk_iommu_v1_probe_finalize - 디바이스에 ARM DMA 매핑을 실제로 붙인다
 *
 * @dev: 대상 클라이언트 디바이스.
 * @return: 없음(실패해도 알릴 수단이 없다).
 *
 * 왜 별도 콜백인가: probe_device 시점에는 아직 IOMMU 그룹 설정이 끝나지
 * 않아 attach를 부를 수 없다. 코어가 모든 준비를 마친 뒤 probe_finalize를
 * 호출해 주므로, 여기서 arm_iommu_attach_device()로 DMA 경로를 연결한다.
 * 이 호출이 성공해야 클라이언트의 dma_map_*()가 IOMMU를 거치게 된다.
 *
 * __maybe_unused의 이유: 비ARM 빌드에서는 arm_iommu_attach_device가
 * 인자를 무시하는 매크로라 data가 쓰이지 않아 컴파일 경고가 난다.
 *
 * 실패해도 계속 진행하는 이유: DMA-OPS가 동작하지 않을 뿐 디바이스 자체는
 * 살아 있을 수 있다. 에러 메시지로 알리고 넘어간다.
 *
 * 실행 컨텍스트: 디바이스 probe 마무리 단계(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iommu_probe_device()의 마무리 → iommu_ops->probe_finalize
 *   → [mtk_iommu_v1_probe_finalize] → arm_iommu_attach_device()
 *   → iommu_attach_device() → mtk_iommu_v1_attach_device()
 */
static void mtk_iommu_v1_probe_finalize(struct device *dev)
{
	/* [한국어] 담당 M4U 인스턴스. 비ARM 빌드에서는 쓰이지 않아
	 * __maybe_unused로 경고를 막는다. */
	__maybe_unused struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);
	/* [한국어] attach 결과. */
	int err;

	/* [한국어] 이 M4U의 4GB DMA 매핑에 디바이스를 붙인다. 내부적으로
	 * iommu_attach_device를 거쳐 mtk_iommu_v1_attach_device가 불리고,
	 * 거기서 LARB 포트의 MMU 비트가 켜진다. */
	err = arm_iommu_attach_device(dev, data->mapping);
	/* [한국어] 실패하면 이 디바이스의 DMA는 IOMMU를 거치지 않게 된다.
	 * 되돌릴 방법이 없어 경고만 남긴다. */
	if (err)
		dev_err(dev, "Can't create IOMMU mapping - DMA-OPS will not work\n");
}

/*
 * [한국어]
 * mtk_iommu_v1_release_device - 디바이스가 사라질 때 LARB 링크를 정리한다
 *
 * @dev: 제거되는 클라이언트 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: probe_device에서 STATELESS로 만든 device_link는 드라이버가
 * 직접 제거해야 한다. 그렇지 않으면 LARB가 영원히 이 클라이언트에 묶여
 * 런타임 PM이 제대로 동작하지 않는다.
 *
 * ids[0]만 보는 이유: probe_device가 "모든 ID가 같은 LARB"임을 이미
 * 강제했으므로, 첫 번째만 봐도 충분하다.
 *
 * 실행 컨텍스트: 디바이스 제거 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iommu_release_device() → iommu_ops->release_device
 *   → [mtk_iommu_v1_release_device] → device_link_remove()
 */
static void mtk_iommu_v1_release_device(struct device *dev)
{
	/* [한국어] 포트 ID들이 담긴 펌웨어 스펙. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 담당 M4U 인스턴스. */
	struct mtk_iommu_v1_data *data;
	/* [한국어] 링크를 끊을 LARB 디바이스. */
	struct device *larbdev;
	/* [한국어] 그 LARB의 번호. */
	unsigned int larbid;

	/* [한국어] priv에서 M4U를 꺼낸다. */
	data = dev_iommu_priv_get(dev);
	/* [한국어] 첫 포트 ID로 LARB 번호를 구한다(전부 같은 LARB임이 보장된다). */
	larbid = mt2701_m4u_to_larb(fwspec->ids[0]);
	/* [한국어] 해당 LARB 디바이스를 얻는다. */
	larbdev = data->larb_imu[larbid].dev;
	/* [한국어] probe_device에서 만든 링크를 제거해 PM 의존 관계를 푼다. */
	device_link_remove(dev, larbdev);
}

/*
 * [한국어]
 * mtk_iommu_v1_hw_init - M4U 하드웨어를 초기 상태로 프로그래밍한다
 *
 * @data: 대상 M4U 인스턴스.
 * @return: 0 성공, 클록 활성화 실패 시 그 errno, IRQ 등록 실패 시 -ENODEV.
 *
 * 동작 과정:
 *  1) 버스 클록(bclk)을 켠다 — 이후 모든 레지스터 접근의 전제다.
 *  2) 제어 레지스터: 코히런시를 켜고, 폴트 대응을 "보호 메모리로 리다이렉트"
 *     (TF_PROTECT_SEL(2))로 설정한다.
 *  3) 인터럽트 제어: 여덟 종류의 폴트를 모두 인터럽트 대상으로 켠다.
 *  4) IVRP_PADDR: 폴트 시 접근을 돌릴 보호 메모리 주소를 알린다.
 *  5) DCM: 동적 클록 게이팅을 켜 유휴 시 전력을 아낀다.
 *  6) IRQ 핸들러를 등록한다. 실패하면 페이지 테이블 베이스를 0으로 지우고
 *     클록을 끈 뒤 -ENODEV를 반환한다.
 *
 * 6번의 되감기가 흥미롭다: PT_BASE_ADDR를 0으로 쓰는 것은 "테이블 없음"을
 * 뜻해 하드웨어가 변환을 시도하지 않게 만든다. IRQ 없이 변환이 활성화되면
 * 폴트가 조용히 무시되어 디버깅이 불가능해지기 때문이다.
 *
 * 실행 컨텍스트: probe(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   mtk_iommu_v1_probe() → [mtk_iommu_v1_hw_init]
 *   → clk_prepare_enable(), writel_relaxed(), devm_request_irq()
 */
static int mtk_iommu_v1_hw_init(const struct mtk_iommu_v1_data *data)
{
	/* [한국어] 레지스터에 쓸 값을 조립하는 임시 변수. */
	u32 regval;
	/* [한국어] 클록 활성화 결과. */
	int ret;

	/* [한국어] 버스 클록을 켠다. 이것이 성공해야 아래 MMIO 접근이 의미가 있다. */
	ret = clk_prepare_enable(data->bclk);
	/* [한국어] 클록이 없으면 하드웨어가 응답하지 않으므로 probe를 실패시킨다. */
	if (ret) {
		dev_err(data->dev, "Failed to enable iommu bclk(%d)\n", ret);	/* [한국어] 클록 없이는 레지스터가 응답하지 않으므로 원인을 남긴다. */
		return ret;	/* [한국어] 클록 실패를 probe로 전달한다. */
	}

	/* [한국어] 제어 레지스터를 설정한다.
	 * COHERENT_EN: 테이블 워크가 CPU 캐시와 일관성을 갖게 해, 드라이버가
	 *              테이블 갱신 후 캐시를 flush 하지 않아도 되게 한다.
	 * TF_PROTECT_SEL(2): 변환 폴트 시 접근을 보호 메모리로 돌린다 —
	 *                    잘못된 DMA가 시스템 메모리를 오염시키지 못하게 한다. */
	regval = F_MMU_CTRL_COHERENT_EN | F_MMU_TF_PROTECT_SEL(2);
	writel_relaxed(regval, data->base + REG_MMU_CTRL_REG);

	/* [한국어] 여덟 종류의 폴트를 모두 인터럽트 대상으로 켠다. 성능 관련
	 * FIFO 오버플로까지 포함하는 것은, 드물게 발생하는 사건이라 알림 비용이
	 * 크지 않고 진단 가치가 높기 때문이다. */
	regval = F_INT_TRANSLATION_FAULT |
		F_INT_MAIN_MULTI_HIT_FAULT |
		F_INT_INVALID_PA_FAULT |
		F_INT_ENTRY_REPLACEMENT_FAULT |
		F_INT_TABLE_WALK_FAULT |
		F_INT_TLB_MISS_FAULT |
		F_INT_PFH_DMA_FIFO_OVERFLOW |
		F_INT_MISS_DMA_FIFO_OVERFLOW;
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL);	/* [한국어] 조립한 폴트 인터럽트 마스크를 하드웨어에 기록한다. */

	/* protect memory,hw will write here while translation fault */
	/* [한국어] 폴트 시 하드웨어가 데이터를 쓸 보호 메모리의 물리 주소를
	 * 알린다. 위 TF_PROTECT_SEL(2)와 짝을 이루는 설정이다. */
	writel_relaxed(data->protect_base,
			data->base + REG_MMU_IVRP_PADDR);

	/* [한국어] 동적 클록 관리를 켠다 — M4U가 유휴일 때 내부 클록을 멈춰
	 * 모바일 SoC의 전력 소모를 줄인다. */
	writel_relaxed(F_MMU_DCM_ON, data->base + REG_MMU_DCM);

	/* [한국어] 폴트 인터럽트 핸들러를 등록한다. devm이라 디바이스 해제 시
	 * 자동으로 해제되지만, remove가 명시적으로도 해제한다. */
	if (devm_request_irq(data->dev, data->irq, mtk_iommu_v1_isr, 0,
			     dev_name(data->dev), (void *)data)) {
		/* [한국어] IRQ 등록 실패 — 폴트를 감지할 수단이 없는 채로 변환을
		 * 활성화하면 잘못된 DMA가 조용히 무시되어 디버깅이 불가능해진다.
		 * 그래서 페이지 테이블 베이스를 0으로 지워 변환 자체를 막는다. */
		writel_relaxed(0, data->base + REG_MMU_PT_BASE_ADDR);
		/* [한국어] 켜 두었던 클록도 되돌린다. */
		clk_disable_unprepare(data->bclk);
		dev_err(data->dev, "Failed @ IRQ-%d Request\n", data->irq);	/* [한국어] 어느 IRQ 등록이 실패했는지 남긴다. */
		return -ENODEV;	/* [한국어] 폴트를 감지할 수 없는 상태로는 동작할 수 없다. */
	}

	/* [한국어] 하드웨어 초기화 완료. */
	return 0;
}

/* [한국어] IOMMU 코어에 노출하는 이 드라이버의 연산 테이블. */
static const struct iommu_ops mtk_iommu_v1_ops = {
	/* [한국어] "IOMMU 우회" 상태를 표현하는 정적 항등 도메인.
	 * 코어가 기본 도메인으로도 쓸 수 있게 등록한다. */
	.identity_domain = &mtk_iommu_v1_identity_domain,
	/* [한국어] 페이징 도메인 생성 — 껍데기만 만들고 테이블은 attach 때 만든다. */
	.domain_alloc_paging = mtk_iommu_v1_domain_alloc_paging,
	/* [한국어] iommus 프로퍼티를 직접 순회해 fwspec과 LARB 링크를 준비한다. */
	.probe_device	= mtk_iommu_v1_probe_device,
	/* [한국어] 그룹 설정이 끝난 뒤 ARM DMA 매핑을 실제로 붙이는 마무리 콜백. */
	.probe_finalize = mtk_iommu_v1_probe_finalize,
	/* [한국어] 디바이스 제거 시 LARB device_link를 정리한다. */
	.release_device	= mtk_iommu_v1_release_device,
	/* [한국어] 코어의 범용 그룹 헬퍼 — 디바이스마다 개별 그룹을 만든다.
	 * 도메인은 하나뿐이지만 그룹은 나눠도 무방하다(어차피 같은 도메인에 붙는다). */
	.device_group	= generic_device_group,
	/* [한국어] 모듈 언로드 중 콜백이 실행되지 않도록 참조를 잡게 한다. */
	.owner          = THIS_MODULE,
	/* [한국어] 페이징 도메인의 연산 테이블(익명 const 구조체). */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		/* [한국어] 첫 attach에서 테이블을 만들고 LARB 포트를 켠다. */
		.attach_dev	= mtk_iommu_v1_attach_device,
		/* [한국어] 평면 배열에 (물리주소|유효|비보안) 엔트리를 기록한다. */
		.map_pages	= mtk_iommu_v1_map,
		/* [한국어] 해당 구간을 0으로 지우고 즉시 범위 TLB 무효화. */
		.unmap_pages	= mtk_iommu_v1_unmap,
		/* [한국어] 소프트웨어 인덱싱으로 물리 주소를 조회한다. */
		.iova_to_phys	= mtk_iommu_v1_iova_to_phys,
		/* [한국어] 4MB 테이블을 반납하고 도메인 구조체를 해제한다.
		 * iotlb_sync 계열 콜백이 없는 것은 unmap이 즉시 무효화하기 때문이다. */
		.free		= mtk_iommu_v1_domain_free,
	}
};

/* [한국어] 이 드라이버가 바인딩할 디바이스 트리 compatible 목록.
 * 1세대 M4U를 쓰는 유일한 SoC가 MT2701이라 항목이 하나뿐이다. */
static const struct of_device_id mtk_iommu_v1_of_ids[] = {
	/* [한국어] MediaTek MT2701의 M4U. */
	{ .compatible = "mediatek,mt2701-m4u", },
	/* [한국어] 배열 끝을 알리는 빈 항목 — 없으면 매칭 루프가 배열 밖으로 넘어간다. */
	{}
};
/* [한국어] 모듈 자동 로딩을 위해 매칭 테이블을 모듈 메타데이터에 심는다. */
MODULE_DEVICE_TABLE(of, mtk_iommu_v1_of_ids);

/* [한국어] component 마스터의 연산 테이블. M4U가 마스터가 되어 LARB들이
 * 모두 준비되기를 기다린다. */
static const struct component_master_ops mtk_iommu_v1_com_ops = {
	/* [한국어] 모든 LARB이 준비되면 호출되어 larb_imu 배열을 채우게 한다. */
	.bind		= mtk_iommu_v1_bind,
	/* [한국어] LARB 중 하나라도 사라지면 호출되어 바인딩을 해제한다. */
	.unbind		= mtk_iommu_v1_unbind,
};

/*
 * [한국어]
 * mtk_iommu_v1_probe - M4U 플랫폼 디바이스를 초기화한다
 *
 * @pdev: 디바이스 트리에서 매칭된 M4U 디바이스.
 * @return: 0 성공, 음수 errno(각 단계 실패), -EPROBE_DEFER(LARB이 아직 준비 안 됨).
 *
 * 동작 과정:
 *  1) 인스턴스 할당(devm).
 *  2) 폴트용 보호 메모리 확보 — 128바이트 정렬 요구를 만족시키려고
 *     필요한 크기의 2배를 잡은 뒤 ALIGN으로 올린다. GFP_DMA를 쓰는 것은
 *     하드웨어가 접근 가능한 저주소 영역이어야 하기 때문이다.
 *  3) MMIO 매핑, IRQ 번호, 버스 클록 확보.
 *  4) mediatek,larbs 프로퍼티의 LARB들을 순회하며:
 *     - 비활성 노드는 건너뛴다.
 *     - 대응 플랫폼 디바이스를 찾고, 아직 드라이버가 바인딩되지 않았으면
 *       -EPROBE_DEFER로 물러난다(LARB이 먼저 준비되어야 한다).
 *     - larb_imu[i].dev에 기록하고 component match 목록에 추가한다.
 *  5) drvdata 설정 → 하드웨어 초기화 → sysfs → 코어 등록 →
 *     component 마스터 등록.
 *
 * 되감기 순서: out_dev_unreg → out_sysfs_remove → out_clk_unprepare →
 * out_put_larbs가 fallthrough로 이어져, 실패 지점에 따라 필요한 만큼만
 * 정리된다.
 *
 * out_put_larbs가 MTK_LARB_NR_MAX 전체를 도는 이유: 루프 도중 실패했을 때
 * 어디까지 채워졌는지 따로 세지 않아도 되게, NULL 원소에도 안전한
 * put_device()의 성질을 이용한 것이다.
 *
 * 실행 컨텍스트: 디바이스 probe(프로세스 컨텍스트, 잠들 수 있음).
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → driver->probe → [mtk_iommu_v1_probe]
 *   → devm_ioremap_resource(), platform_get_irq(), devm_clk_get(),
 *     mtk_iommu_v1_hw_init(), iommu_device_register(),
 *     component_master_add_with_match()
 */
static int mtk_iommu_v1_probe(struct platform_device *pdev)
{
	/* [한국어] 편의를 위한 struct device 포인터. */
	struct device			*dev = &pdev->dev;
	/* [한국어] 만들 M4U 인스턴스. */
	struct mtk_iommu_v1_data	*data;
	/* [한국어] MMIO 자원 서술자. */
	struct resource			*res;
	/* [한국어] component 프레임워크에 넘길 LARB 매칭 목록.
	 * NULL로 시작해 component_match_add_release가 채운다. */
	struct component_match		*match = NULL;
	/* [한국어] 폴트용 보호 메모리의 가상 주소(정렬 전). */
	void				*protect;
	/* [한국어] LARB 개수, 결과 코드, 순회 인덱스. */
	int				larb_nr, ret, i;

	/* [한국어] 인스턴스를 0으로 초기화해 할당한다. devm이라 자동 해제된다. */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)	/* [한국어] 인스턴스 할당 실패 — 아직 잡은 자원이 없다. */
		return -ENOMEM;

	/* [한국어] DMA 할당과 로깅에 쓸 자기 device 포인터를 보관한다. */
	data->dev = dev;

	/* Protect memory. HW will access here while translation fault.*/
	/* [한국어] 폴트 시 하드웨어가 쓸 버퍼를 확보한다.
	 * 크기를 2배(2 × 128바이트)로 잡는 이유: kcalloc이 128바이트 정렬을
	 * 보장하지 않으므로, 정렬로 앞쪽을 버려도 128바이트가 남게 하려는 것이다.
	 * GFP_DMA인 이유: 이 하드웨어가 접근할 수 있는 저주소 영역이어야 한다. */
	protect = devm_kcalloc(dev, 2, MTK_PROTECT_PA_ALIGN,
			       GFP_KERNEL | GFP_DMA);
	if (!protect)	/* [한국어] 보호 메모리 없이는 폴트가 시스템 메모리를 오염시킨다. */
		return -ENOMEM;
	/* [한국어] 가상 주소를 물리 주소로 바꾸고 128바이트 경계로 올린다.
	 * 하드웨어가 요구하는 정렬을 만족시키는 마지막 단계다. */
	data->protect_base = ALIGN(virt_to_phys(protect), MTK_PROTECT_PA_ALIGN);

	/* [한국어] 디바이스 트리의 첫 reg 항목을 가져온다. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	/* [한국어] 레지스터 블록을 매핑한다. devm이라 자동 언매핑된다. */
	data->base = devm_ioremap_resource(dev, res);
	/* [한국어] 매핑 실패(자원 없음/중복 점유). */
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);

	/* [한국어] 폴트 인터럽트 번호를 가져온다. */
	data->irq = platform_get_irq(pdev, 0);
	/* [한국어] 음수면 인터럽트가 없다는 뜻이라 그대로 실패로 전달한다
	 * (-EPROBE_DEFER일 수도 있다). */
	if (data->irq < 0)
		return data->irq;

	/* [한국어] "bclk"이라는 이름의 버스 클록을 얻는다. optional이 아니라
	 * 필수 — 이 클록 없이는 레지스터가 응답하지 않는다. */
	data->bclk = devm_clk_get(dev, "bclk");
	if (IS_ERR(data->bclk))	/* [한국어] 버스 클록이 없으면 이 M4U는 쓸 수 없다. */
		return PTR_ERR(data->bclk);

	/* [한국어] 이 M4U가 관리할 LARB이 몇 개인지 센다. */
	larb_nr = of_count_phandle_with_args(dev->of_node,
					     "mediatek,larbs", NULL);
	/* [한국어] 프로퍼티가 없거나 잘못됐으면 그 오류를 전달한다. */
	if (larb_nr < 0)
		return larb_nr;

	/* [한국어] larb_imu 배열 크기를 넘는 개수는 담을 수 없다. */
	if (larb_nr > MTK_LARB_NR_MAX)
		return -EINVAL;

	/* [한국어] 각 LARB 노드를 순회하며 디바이스를 확보하고 component
	 * 매칭 목록에 등록한다. */
	for (i = 0; i < larb_nr; i++) {
		/* [한국어] i번째 LARB의 디바이스 트리 노드. */
		struct device_node *larbnode;
		/* [한국어] 그 노드에 대응하는 플랫폼 디바이스. */
		struct platform_device *plarbdev;

		/* [한국어] mediatek,larbs의 i번째 phandle을 해석한다.
		 * 참조 카운트가 올라가므로 각 경로에서 내려야 한다. */
		larbnode = of_parse_phandle(dev->of_node, "mediatek,larbs", i);
		/* [한국어] 개수는 셌는데 파싱이 실패했다면 트리가 손상된 것이다. */
		if (!larbnode) {
			ret = -EINVAL;	/* [한국어] 개수는 셌는데 파싱이 실패했으므로 트리가 손상된 것이다. */
			goto out_put_larbs;	/* [한국어] 확보한 LARB 참조를 되돌리러 간다. */
		}

		/* [한국어] status = "disabled"인 LARB은 건너뛴다. 노드 참조를
		 * 내리는 것을 잊지 않아야 한다. */
		if (!of_device_is_available(larbnode)) {
			of_node_put(larbnode);	/* [한국어] 건너뛰기 전에 올린 노드 참조를 반드시 내린다. */
			continue;	/* [한국어] 비활성 LARB은 매칭 목록에 넣지 않는다. */
		}

		/* [한국어] 노드에 대응하는 플랫폼 디바이스를 찾는다(참조 +1). */
		plarbdev = of_find_device_by_node(larbnode);
		/* [한국어] 디바이스가 아직 만들어지지 않았다 — 트리에는 있는데
		 * 플랫폼 디바이스가 없는 비정상 상황이다. */
		if (!plarbdev) {
			of_node_put(larbnode);	/* [한국어] 실패 경로에서도 노드 참조를 내린다. */
			ret = -ENODEV;	/* [한국어] 트리에는 있으나 플랫폼 디바이스가 없는 비정상 상황이다. */
			goto out_put_larbs;	/* [한국어] 되감기 경로로 간다. */
		}
		/* [한국어] LARB 드라이버가 아직 바인딩되지 않았다면 M4U가 먼저
		 * probe된 것이다. -EPROBE_DEFER로 물러나 나중에 다시 시도한다 —
		 * LARB이 준비되어야 component bind가 성립하기 때문이다. */
		if (!plarbdev->dev.driver) {
			of_node_put(larbnode);	/* [한국어] defer 전에 노드 참조를 내린다. */
			put_device(&plarbdev->dev);	/* [한국어] 방금 얻은 디바이스 참조도 내린다. */
			ret = -EPROBE_DEFER;	/* [한국어] LARB이 먼저 준비되어야 하므로 나중에 다시 시도한다. */
			goto out_put_larbs;	/* [한국어] 이미 확보한 LARB 참조를 정리하러 간다. */
		}
		/* [한국어] LARB 디바이스 포인터를 보관한다. 여기서 얻은 참조는
		 * 의도적으로 유지되며, out_put_larbs와 remove가 내린다. */
		data->larb_imu[i].dev = &plarbdev->dev;

		/* [한국어] component 매칭 목록에 이 LARB 노드를 추가한다.
		 * component_compare_of가 노드로 매칭하고, component_release_of가
		 * 나중에 노드 참조를 내린다(그래서 여기서는 of_node_put을 하지 않는다). */
		component_match_add_release(dev, &match, component_release_of,
					    component_compare_of, larbnode);
	}

	/* [한국어] create_mapping()이 이 인스턴스를 찾아갈 수 있도록 drvdata에
	 * 심는다. 하드웨어 초기화보다 먼저 해야 하는 이유는, 초기화 중
	 * 등록되는 IRQ 핸들러가 이 데이터를 쓸 수 있기 때문이다. */
	platform_set_drvdata(pdev, data);

	/* [한국어] 클록을 켜고 레지스터를 프로그래밍하고 IRQ를 등록한다. */
	ret = mtk_iommu_v1_hw_init(data);
	if (ret)	/* [한국어] 하드웨어 초기화 실패 — LARB 참조부터 되돌린다. */
		goto out_put_larbs;

	/* [한국어] /sys/class/iommu/ 아래에 이 M4U를 노출한다. */
	ret = iommu_device_sysfs_add(&data->iommu, &pdev->dev, NULL,
				     dev_name(&pdev->dev));
	if (ret)	/* [한국어] sysfs 등록 실패 — 클록부터 되감는다. */
		goto out_clk_unprepare;

	/* [한국어] IOMMU 코어에 연산 테이블을 등록한다. 이 시점부터
	 * probe_device 콜백이 들어올 수 있다. */
	ret = iommu_device_register(&data->iommu, &mtk_iommu_v1_ops, dev);
	if (ret)	/* [한국어] 코어 등록 실패 — sysfs부터 되감는다. */
		goto out_sysfs_remove;

	/* [한국어] component 마스터로 등록한다. 매칭된 LARB이 모두 준비되어
	 * 있으면 여기서 곧바로 bind가 호출되고, 아니면 나중에 호출된다. */
	ret = component_master_add_with_match(dev, &mtk_iommu_v1_com_ops, match);
	if (ret)	/* [한국어] component 마스터 등록 실패 — 코어 등록부터 되감는다. */
		goto out_dev_unreg;
	/* [한국어] 성공. ret이 0이므로 그대로 반환한다. */
	return ret;

/* [한국어] 아래는 역순 되감기 레이블들. fallthrough로 이어져 실패 지점부터
 * 아래 단계까지 순서대로 정리된다. */
out_dev_unreg:
	/* [한국어] IOMMU 코어 등록을 취소한다. */
	iommu_device_unregister(&data->iommu);
out_sysfs_remove:
	/* [한국어] sysfs 노드를 제거한다. */
	iommu_device_sysfs_remove(&data->iommu);
out_clk_unprepare:
	/* [한국어] hw_init이 켠 버스 클록을 끈다. */
	clk_disable_unprepare(data->bclk);
out_put_larbs:
	/* [한국어] 확보해 둔 LARB 디바이스 참조를 모두 내린다.
	 * 배열 전체를 도는 것은, 루프 도중 실패한 경우에도 채워진 곳까지만
	 * 유효하고 나머지는 NULL이기 때문이다 — put_device(NULL)은 안전하다. */
	for (i = 0; i < MTK_LARB_NR_MAX; i++)
		put_device(data->larb_imu[i].dev);

	/* [한국어] 실패를 유발한 오류 코드를 반환한다. */
	return ret;
}

/*
 * [한국어]
 * mtk_iommu_v1_remove - M4U 디바이스를 정리한다
 *
 * @pdev: 제거되는 플랫폼 디바이스.
 * @return: 없음.
 *
 * 정리 순서: sysfs → 코어 등록 해제 → 클록 → IRQ → component 마스터 →
 * LARB 참조. probe의 되감기와 대체로 대칭이다.
 *
 * devm_free_irq를 명시적으로 부르는 이유: devm이 자동 해제해 주지만,
 * 그 시점은 디바이스 해제가 끝난 뒤다. 그 사이에 인터럽트가 들어오면
 * 이미 정리된 데이터를 ISR이 만질 수 있어, 여기서 먼저 끊는다.
 *
 * 실행 컨텍스트: 디바이스 제거 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   플랫폼 버스 → driver->remove → [mtk_iommu_v1_remove]
 *   → iommu_device_unregister(), devm_free_irq(), component_master_del()
 */
static void mtk_iommu_v1_remove(struct platform_device *pdev)
{
	/* [한국어] probe가 심어 둔 인스턴스를 꺼낸다. */
	struct mtk_iommu_v1_data *data = platform_get_drvdata(pdev);
	/* [한국어] LARB 참조 해제 루프의 인덱스. */
	int i;

	/* [한국어] sysfs 노드를 먼저 제거한다. */
	iommu_device_sysfs_remove(&data->iommu);
	/* [한국어] 코어 등록을 해제해 더 이상 콜백이 들어오지 않게 한다. */
	iommu_device_unregister(&data->iommu);

	/* [한국어] hw_init이 켠 버스 클록을 끈다. */
	clk_disable_unprepare(data->bclk);
	/* [한국어] IRQ를 명시적으로 해제한다. devm의 자동 해제를 기다리면
	 * 그 사이 들어온 인터럽트가 정리 중인 데이터를 만질 수 있다. */
	devm_free_irq(&pdev->dev, data->irq, data);
	/* [한국어] component 마스터 등록을 해제한다 — 내부적으로 unbind가
	 * 호출되어 LARB 바인딩이 풀린다. */
	component_master_del(&pdev->dev, &mtk_iommu_v1_com_ops);

	/* [한국어] probe에서 확보한 LARB 디바이스 참조를 모두 내린다. */
	for (i = 0; i < MTK_LARB_NR_MAX; i++)
		put_device(data->larb_imu[i].dev);
}

/*
 * [한국어]
 * mtk_iommu_v1_suspend - 시스템 서스펜드 전 레지스터를 백업한다
 *
 * @dev: M4U 디바이스.
 * @return: 항상 0.
 *
 * 왜 필요한가: 서스펜드 동안 M4U의 전원이 끊겨 모든 레지스터가 초기값으로
 * 돌아간다. 리줌 후 그대로 두면 인터럽트가 꺼지고 폴트 보호도 사라지므로,
 * 복원해야 할 값들을 미리 읽어 둔다.
 *
 * 백업 대상 네 가지: AXI 모드, DCM(클록 게이팅), 제어 레지스터,
 * 인터럽트 제어. 페이지 테이블 베이스는 백업하지 않는데, 도메인의 pgt_pa에
 * 이미 있어 리줌 때 거기서 가져올 수 있기 때문이다.
 *
 * __maybe_unused의 이유: CONFIG_PM_SLEEP이 꺼진 빌드에서는 이 함수가
 * 참조되지 않아 컴파일 경고가 난다.
 *
 * 실행 컨텍스트: PM 코어의 서스펜드 경로(프로세스 컨텍스트, 다른 활동이 멈춘 상태).
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops->suspend → [mtk_iommu_v1_suspend]
 */
static int __maybe_unused mtk_iommu_v1_suspend(struct device *dev)
{
	/* [한국어] drvdata에서 M4U 인스턴스를 꺼낸다. */
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);
	/* [한국어] 백업을 담을 구조체. */
	struct mtk_iommu_v1_suspend_reg *reg = &data->reg;
	/* [한국어] 레지스터 베이스 — 반복 접근을 줄이려 지역 변수에 담는다. */
	void __iomem *base = data->base;

	/* [한국어] AXI 동작 모드를 백업한다(부트로더가 정한 값일 수 있다). */
	reg->standard_axi_mode = readl_relaxed(base +
					       REG_MMU_STANDARD_AXI_MODE);
	/* [한국어] 동적 클록 게이팅 설정을 백업한다. */
	reg->dcm_dis = readl_relaxed(base + REG_MMU_DCM);
	/* [한국어] 코히런시와 폴트 대응 방식이 담긴 제어 레지스터를 백업한다. */
	reg->ctrl_reg = readl_relaxed(base + REG_MMU_CTRL_REG);
	/* [한국어] 어떤 폴트에 인터럽트를 낼지가 담긴 레지스터를 백업한다. */
	reg->int_control0 = readl_relaxed(base + REG_MMU_INT_CONTROL);
	/* [한국어] 실패할 수 있는 동작이 없어 항상 성공을 반환한다. */
	return 0;
}

/*
 * [한국어]
 * mtk_iommu_v1_resume - 리줌 후 레지스터를 복원한다
 *
 * @dev: M4U 디바이스.
 * @return: 항상 0.
 *
 * 복원 순서에 주목: 페이지 테이블 베이스를 **가장 먼저** 쓴다. 하드웨어가
 * 변환을 시작하기 전에 올바른 테이블을 가리키게 해야, 리줌 직후 들어오는
 * DMA가 엉뚱한 메모리를 테이블로 읽는 사고를 막을 수 있다.
 * 그다음 백업해 둔 네 레지스터를 되돌리고, 마지막으로 보호 메모리 주소를
 * 다시 알린다(이 값은 백업 대상이 아니라 data에 항상 남아 있다).
 *
 * 주의: data->m4u_dom을 검사 없이 역참조한다. 도메인이 한 번도 만들어지지
 * 않은 상태에서 서스펜드/리줌이 일어나면 NULL 역참조가 난다. 실제로는
 * 부팅 시 클라이언트가 반드시 attach되므로 드러나지 않는 문제다.
 *
 * 실행 컨텍스트: PM 코어의 리줌 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   PM 코어 → dev_pm_ops->resume → [mtk_iommu_v1_resume]
 */
static int __maybe_unused mtk_iommu_v1_resume(struct device *dev)
{
	/* [한국어] M4U 인스턴스를 꺼낸다. */
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);
	/* [한국어] 서스펜드 때 백업해 둔 값들. */
	struct mtk_iommu_v1_suspend_reg *reg = &data->reg;
	/* [한국어] 레지스터 베이스. */
	void __iomem *base = data->base;

	/* [한국어] 가장 먼저 페이지 테이블 위치를 복원한다. 이것이 늦으면
	 * 하드웨어가 초기값(0 또는 쓰레기)을 테이블로 삼아 워크할 수 있다. */
	writel_relaxed(data->m4u_dom->pgt_pa, base + REG_MMU_PT_BASE_ADDR);
	/* [한국어] AXI 모드를 되돌린다. */
	writel_relaxed(reg->standard_axi_mode,
		       base + REG_MMU_STANDARD_AXI_MODE);
	/* [한국어] 동적 클록 게이팅 설정을 되돌린다. */
	writel_relaxed(reg->dcm_dis, base + REG_MMU_DCM);
	/* [한국어] 코히런시와 폴트 대응 방식을 되돌린다. */
	writel_relaxed(reg->ctrl_reg, base + REG_MMU_CTRL_REG);
	/* [한국어] 인터럽트 활성화 설정을 되돌린다 — 이것을 빼먹으면 리줌 후
	 * 폴트가 조용히 무시되어 디버깅이 불가능해진다. */
	writel_relaxed(reg->int_control0, base + REG_MMU_INT_CONTROL);
	/* [한국어] 보호 메모리 주소를 다시 알린다. 백업 대상이 아닌 이유는
	 * 이 값이 data->protect_base에 항상 보존되어 있기 때문이다. */
	writel_relaxed(data->protect_base, base + REG_MMU_IVRP_PADDR);
	/* [한국어] 복원 완료. */
	return 0;
}

/* [한국어] 전원 관리 콜백 테이블. SET_SYSTEM_SLEEP_PM_OPS 매크로가
 * suspend/resume을 시스템 슬립 관련 여러 콜백(freeze/thaw/poweroff/restore)에
 * 한꺼번에 연결해 준다 — 하이버네이션 경로에서도 같은 복원이 필요하기 때문이다.
 * CONFIG_PM_SLEEP이 꺼져 있으면 이 매크로가 아무것도 만들지 않고,
 * 그래서 위 두 함수에 __maybe_unused가 붙어 있다. */
static const struct dev_pm_ops mtk_iommu_v1_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_iommu_v1_suspend, mtk_iommu_v1_resume)	/* [한국어] suspend/resume을 시스템 슬립과 하이버네이션 콜백들에 한꺼번에 연결한다. */
};

/* [한국어] 플랫폼 버스에 등록할 드라이버 정의. */
static struct platform_driver mtk_iommu_v1_driver = {
	/* [한국어] 초기화 진입점. */
	.probe	= mtk_iommu_v1_probe,
	/* [한국어] 정리 진입점. */
	.remove = mtk_iommu_v1_remove,
	/* [한국어] 드라이버 공통 정보. */
	.driver	= {
		/* [한국어] sysfs와 로그에 나타날 이름. 2세대 드라이버
		 * (mtk-iommu)와 구분하려고 -v1을 붙였다. */
		.name = "mtk-iommu-v1",
		/* [한국어] 디바이스 트리 매칭 테이블. */
		.of_match_table = mtk_iommu_v1_of_ids,
		/* [한국어] 서스펜드/리줌 콜백. */
		.pm = &mtk_iommu_v1_pm_ops,
	}
};
/* [한국어] module_init/module_exit 보일러플레이트를 자동 생성한다. */
module_platform_driver(mtk_iommu_v1_driver);

/* [한국어] modinfo에 표시될 설명. */
MODULE_DESCRIPTION("IOMMU API for MediaTek M4U v1 implementations");
/* [한국어] 라이선스 선언. 파일 상단의 SPDX(GPL-2.0-only)와 짝을 이루며,
 * 이것이 있어야 GPL 전용 커널 심볼을 쓸 수 있다. */
MODULE_LICENSE("GPL v2");
