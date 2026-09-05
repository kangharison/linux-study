// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Yong Wu <yong.wu@mediatek.com>
 */
/*
 * [한국어 설명] MediaTek M4U IOMMU 드라이버 (mtk_iommu.c)
 *
 * === 파일의 역할 ===
 * MediaTek SoC의 M4U(Multimedia Memory Management Unit)를 리눅스
 * IOMMU API에 붙인다. 페이지 테이블은 io-pgtable의 ARM_V7S 포맷이
 * 맡으므로(MediaTek 확장을 얹어 35비트 물리 주소까지 쓴다),
 * 이 파일이 다루는 것은 그 위의 배선과 정책이다.
 *
 * 이 드라이버를 읽을 때 붙잡아야 할 것이 다섯이다.
 *
 * (1) **포트를 켜는 스위치가 이 하드웨어에 없다.** 멀티미디어
 *     마스터들은 M4U에 직접 붙지 않고 SMI larb(local arbiter)를
 *     거치는데, "이 포트가 IOMMU를 쓸 것인가"는 그 larb의
 *     레지스터가 정한다. 그래서 mtk_iommu_config()가 하는 일은
 *     larb 드라이버의 구조체에 비트를 세우는 것이고, 실제 쓰기는
 *     SMI 드라이버가 한다. 두 드라이버를 잇기 위해 component
 *     프레임워크를 쓴다 — 이 파일에 bind/unbind가 있는 이유다.
 *
 * (2) **IOVA 영역이 여럿이지만 페이지 테이블은 하나다.**
 *     16GB의 IOVA 공간을 4GB씩 나눠 마스터마다 다른 영역을 주되,
 *     테이블은 공유한다. 영역이 곧 IOMMU 그룹이 되므로 격리는
 *     그룹 단위로 이뤄지고, 어느 마스터가 어느 영역에 속하는지는
 *     larb 번호와 포트 번호로 표에서 찾는다.
 *
 * (3) **뱅크는 진짜로 테이블이 나뉜다.** 하드웨어가 최대 5개의
 *     뱅크를 갖고 각 뱅크가 독립된 테이블 기준 레지스터를 갖는다.
 *     뱅크가 활성화되어 있으면 뱅크가 곧 그룹이 되고, 아니면
 *     IOVA 영역이 그룹이 된다.
 *
 * (4) **두 개의 M4U가 테이블을 공유할 수 있다.** SHARE_PGTABLE
 *     플래그가 붙은 하드웨어는 전역 목록(m4ulist)에 함께 들어가,
 *     첫 번째 것의 테이블을 모두가 쓴다. 그래서 무효화가
 *     for_each_m4u로 목록 전체를 훑는다.
 *
 * (5) **플래그가 세대 차이를 전부 흡수한다.** flags 하나에
 *     22개의 비트가 들어 있고, 그것이 레지스터 배치부터 인터럽트
 *     ID의 비트 폭, IOVA 폭, 전원 관리 방식까지 정한다.
 *     MTK_IOMMU_HAS_FLAG 하나로 모든 분기가 표현된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [멀티미디어 마스터] 예: 카메라, 코덱
 *        ↓ 물리적으로
 *   [SMI larb] 포트별 IOMMU 사용 여부를 여기서 켠다
 *        ↓
 *   [SMI common] → [M4U] 변환 수행
 *
 *   [디바이스 트리] iommus = <&m4u 포트ID>
 *        ↓ of_xlate
 *   [이 파일] fwspec에 (larb, port) 인코딩을 등록
 *        ↓ probe_device
 *   [이 파일] 마스터와 larb를 device_link로 묶는다
 *        ↓ attach_dev
 *   [이 파일] 도메인을 완성하고 → 뱅크에 테이블 주소를 심고
 *             → larb의 포트 비트를 켠다
 *
 * 실행 컨텍스트: map/unmap은 io-pgtable에 위임한다. 무효화는
 * tlb_lock(irqsave) 아래에서 폴링하며, 전원이 꺼진 하드웨어는
 * 건너뛴다. 도메인 완성과 그룹 결정은 뮤텍스를 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * - io-pgtable(ARM_V7S): 실제 페이지 테이블. MediaTek 확장으로
 *   ARM v7 짧은 서술자의 예약 비트를 상위 물리 주소에 쓴다.
 * - soc/mediatek/smi.h와 SMI 드라이버: larb의 포트 활성화.
 *   struct mtk_smi_larb_iommu를 공유해 그쪽이 실제 레지스터를 쓴다.
 * - linux/component.h: M4U와 larb들의 결합 순서를 맞춘다.
 * - linux/arm-smccc.h: 일부 INFRA IOMMU는 포트 설정을 ATF(보안
 *   모니터)에 SMC로 요청한다 — 커널이 직접 쓸 수 없는 레지스터다.
 * - dt-bindings/memory/mtk-memory-port.h: fwspec의 ID를 larb와
 *   port로 쪼개는 매크로.
 * 데이터 흐름: 마스터의 포트 ID → larb/port → IOVA 영역과 뱅크 →
 * 그 도메인의 테이블 → 물리 주소(4GB 모드면 비트 32를 얹는다).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mtk_iommu_plat_data: SoC 하나의 모든 차이를 담은 표.
 *   플래그, IOVA 영역 목록, 뱅크 구성, larb 번호 재매핑이 들어 있다.
 * - struct mtk_iommu_bank_data: 뱅크 하나. 자기 레지스터 창과
 *   인터럽트, 그리고 붙어 있는 도메인.
 * - mtk_iommu_config(): larb의 포트 비트를 켜고 끈다 — 이 드라이버가
 *   마스터를 IOMMU에 잇는 유일한 지점이다.
 * - mtk_iommu_domain_finalise(): 페이지 테이블을 만들거나 공유하고,
 *   IOVA 영역으로 도메인의 범위를 정한다.
 * - mtk_iommu_tlb_flush_range_sync(): 목록의 모든 M4U에서 범위를
 *   무효화한다. 전원이 꺼진 것은 건너뛴다.
 * - larbid_remap: 인터럽트가 알려 준 하드웨어 larb 번호를 소프트웨어
 *   번호로 옮기는 표. SoC마다 배선이 달라 필요하다.
 */
/* [한국어] ATF(보안 모니터)에 SMC 호출을 보내는 인터페이스.
 * 일부 INFRA IOMMU의 포트 설정 레지스터는 커널이 쓸 수 없어
 * 보안 세계에 요청해야 한다. */
#include <linux/arm-smccc.h>
/* [한국어] FIELD_GET — 폴트 주소에서 상위 비트를 뽑을 때 쓴다. */
#include <linux/bitfield.h>
/* [한국어] WARN_ON 등. */
#include <linux/bug.h>
/* [한국어] bclk — 일부 하드웨어가 요구하는 전용 클럭. */
#include <linux/clk.h>
/* [한국어] component 프레임워크 — M4U와 SMI larb들의 결합 순서를
 * 맞춘다. larb가 모두 준비된 뒤에야 bind가 불린다. */
#include <linux/component.h>
/* [한국어] struct device와 디바이스 링크. */
#include <linux/device.h>
/* [한국어] ERR_PTR/IS_ERR. */
#include <linux/err.h>
/* [한국어] 폴트 인터럽트 등록. */
#include <linux/interrupt.h>
/* [한국어] MMIO 접근. */
#include <linux/io.h>
/* [한국어] IOMMU 코어 계약. */
#include <linux/iommu.h>
/* [한국어] readl_poll_timeout_atomic — TLB 무효화 완료 대기. */
#include <linux/iopoll.h>
/* [한국어] 실제 페이지 테이블을 맡기는 계층(ARM_V7S 포맷). */
#include <linux/io-pgtable.h>
/* [한국어] 같은 테이블을 공유하는 M4U들의 목록. */
#include <linux/list.h>
/* [한국어] syscon — infracfg와 pericfg 레지스터 접근. */
#include <linux/mfd/syscon.h>
/* [한국어] 모듈로 빌드된다. */
#include <linux/module.h>
/* [한국어] 디바이스 트리 주소 파싱. */
#include <linux/of_address.h>
/* [한국어] 인터럽트 파싱. */
#include <linux/of_irq.h>
/* [한국어] of_find_device_by_node — larb와 M4U를 찾을 때. */
#include <linux/of_platform.h>
/* [한국어] PCIe를 지원하는 INFRA IOMMU의 특수 처리에 필요하다. */
#include <linux/pci.h>
/* [한국어] 플랫폼 드라이버 모델. */
#include <linux/platform_device.h>
/* [한국어] 런타임 PM — 무효화가 전원 상태를 확인하는 근거다. */
#include <linux/pm_runtime.h>
/* [한국어] regmap — infracfg/pericfg의 공유 레지스터 접근. */
#include <linux/regmap.h>
/* [한국어] kzalloc 등. */
#include <linux/slab.h>
/* [한국어] 뱅크별 무효화 락. */
#include <linux/spinlock.h>
/* [한국어] REG_INFRA_MISC와 4GB 지원 비트 — 4GB 모드 판별에 쓴다. */
#include <linux/soc/mediatek/infracfg.h>
/* [한국어] MTK_SIP_KERNEL_IOMMU_CONTROL — ATF에 보낼 SMC 함수 번호. */
#include <linux/soc/mediatek/mtk_sip_svc.h>
/* [한국어] str_enable_disable/str_write_read — 로그 문자열. */
#include <linux/string_choices.h>
/* [한국어] wmb() — 무효화 명령이 실제로 나갔음을 보장한다. */
#include <asm/barrier.h>
/* [한국어] struct mtk_smi_larb_iommu — SMI 드라이버와 공유하는
 * 구조체. 이 파일이 여기에 포트 비트를 세우면 그쪽이 하드웨어에 쓴다. */
#include <soc/mediatek/smi.h>

/* [한국어] MTK_M4U_TO_LARB / MTK_M4U_TO_PORT — fwspec의 ID 하나를
 * larb 번호와 포트 번호로 쪼갠다. 디바이스 트리와 공유하는 인코딩이다. */
#include <dt-bindings/memory/mtk-memory-port.h>

/* [한국어] 이 뱅크의 1단계 테이블 기준 주소를 쓰는 레지스터. */
#define REG_MMU_PT_BASE_ADDR			0x000

/* [한국어] 무효화 명령을 실행시키는 레지스터. */
#define REG_MMU_INVALIDATE			0x020
/* [한국어] 전체 무효화 명령. */
#define F_ALL_INVLD				0x2
/* [한국어] 범위 무효화 명령 — 아래 두 레지스터로 범위를 먼저 정한다. */
#define F_MMU_INV_RANGE				0x1

/* [한국어] 범위 무효화의 시작 주소. */
#define REG_MMU_INVLD_START_A			0x024
/* [한국어] 범위 무효화의 끝 주소. */
#define REG_MMU_INVLD_END_A			0x028

/* [한국어] 2세대 하드웨어의 무효화 대상 선택 레지스터. */
#define REG_MMU_INV_SEL_GEN2			0x02c
/* [한국어] 1세대의 같은 레지스터. 오프셋이 달라 plat_data가
 * 어느 쪽을 쓸지 지정한다(inv_sel_reg). */
#define REG_MMU_INV_SEL_GEN1			0x038
/* [한국어] 0번 변환 유닛을 무효화 대상에 넣는다. */
#define F_INVLD_EN0				BIT(0)
/* [한국어] 1번 변환 유닛도 함께 넣는다. 이 드라이버는 항상 둘 다 켠다. */
#define F_INVLD_EN1				BIT(1)

/* [한국어] 기타 제어 레지스터 — AXI 동작 방식을 정한다. */
#define REG_MMU_MISC_CTRL			0x048
/* [한국어] 쓰기를 순서대로 내보내게 하는 비트들.
 * 비트가 둘인 이유: 변환 유닛이 둘이라 각각의 자리가 있다. */
#define F_MMU_IN_ORDER_WR_EN_MASK		(BIT(1) | BIT(17))
/* [한국어] 표준 AXI 모드 비트들. 끄면 MediaTek 확장 동작을 쓴다. */
#define F_MMU_STANDARD_AXI_MODE_MASK		(BIT(3) | BIT(19))

/* [한국어] 동적 클럭 관리(DCM) 비활성화 레지스터. */
#define REG_MMU_DCM_DIS				0x050
/* [한국어] DCM을 끄는 비트. 일부 하드웨어에서 DCM이 문제를 일으켜
 * 플래그로 끌 수 있게 해 두었다. */
#define F_MMU_DCM				BIT(8)

/* [한국어] 쓰기 길이 제어 레지스터. */
#define REG_MMU_WR_LEN_CTRL			0x054
/* [한국어] 쓰기 조절(throttling)을 끄는 비트들. 이 드라이버는
 * WR_THROT_EN 플래그가 있으면 이 비트를 내려 조절을 **켠다**. */
#define F_MMU_WR_THROT_DIS_MASK			(BIT(5) | BIT(21))

/* [한국어] 주 제어 레지스터. */
#define REG_MMU_CTRL_REG			0x110
/* [한국어] 변환 폴트가 나면 미리 정해 둔 보호 주소로 접근을
 * 돌린다 — 잘못된 DMA가 시스템 메모리를 덮지 않게 하는 장치다. */
#define F_MMU_TF_PROT_TO_PROGRAM_ADDR		(2 << 4)
/* [한국어] 미리 읽기의 교체 정책을 바꾼다(구형 하드웨어용). */
#define F_MMU_PREFETCH_RT_REPLACE_MOD		BIT(4)
/* [한국어] MT8173 계열은 같은 기능의 비트 위치가 다르다. */
#define F_MMU_TF_PROT_TO_PROGRAM_ADDR_MT8173	(2 << 5)

/* [한국어] 폴트 시 접근을 돌릴 보호 메모리의 물리 주소를 쓴다. */
#define REG_MMU_IVRP_PADDR			0x114

/* [한국어] 유효한 물리 주소 범위를 제한하는 레지스터(4GB 모드용). */
#define REG_MMU_VLD_PA_RNG			0x118
/* [한국어] 끝 주소와 시작 주소를 한 워드에 담는다(각각 비트 32:30 단위). */
#define F_MMU_VLD_PA_RNG(EA, SA)		(((EA) << 8) | (SA))

/* [한국어] 인터럽트 제어 레지스터 0 — 종류별 활성화와 지우기. */
#define REG_MMU_INT_CONTROL0			0x120
/* [한국어] L2 다중 적중 인터럽트 활성화. */
#define F_L2_MULIT_HIT_EN			BIT(0)
/* [한국어] 테이블 워크 실패 인터럽트 활성화. */
#define F_TABLE_WALK_FAULT_INT_EN		BIT(1)
/* [한국어] 미리 읽기 큐 넘침 인터럽트 활성화. */
#define F_PREETCH_FIFO_OVERFLOW_INT_EN		BIT(2)
/* [한국어] 미스 큐 넘침 인터럽트 활성화. */
#define F_MISS_FIFO_OVERFLOW_INT_EN		BIT(3)
/* [한국어] 미리 읽기 큐 오류 인터럽트 활성화. */
#define F_PREFETCH_FIFO_ERR_INT_EN		BIT(5)
/* [한국어] 미스 큐 오류 인터럽트 활성화. */
#define F_MISS_FIFO_ERR_INT_EN			BIT(6)
/* [한국어] 이 비트를 세우면 밀린 인터럽트가 지워진다. */
#define F_INT_CLR_BIT				BIT(12)

/* [한국어] 주 인터럽트 제어 — 폴트 종류별로 인터럽트를 켠다. */
#define REG_MMU_INT_MAIN_CONTROL		0x124
						/* mmu0 | mmu1 */
/* [한국어] 아래 비트들이 모두 쌍인 것은 변환 유닛이 둘(mmu0, mmu1)
 * 이기 때문이다 — 같은 종류의 폴트가 유닛마다 별도 비트를 갖는다. */
/* [한국어] 변환 폴트 — 매핑이 없는 주소에 접근했다. */
#define F_INT_TRANSLATION_FAULT			(BIT(0) | BIT(7))
/* [한국어] 다중 적중 — TLB에 같은 주소가 둘 이상 있다. */
#define F_INT_MAIN_MULTI_HIT_FAULT		(BIT(1) | BIT(8))
/* [한국어] 유효하지 않은 물리 주소로 변환됐다. */
#define F_INT_INVALID_PA_FAULT			(BIT(2) | BIT(9))
/* [한국어] TLB 항목 교체 중에 오류가 났다. */
#define F_INT_ENTRY_REPLACEMENT_FAULT		(BIT(3) | BIT(10))
/* [한국어] TLB 미스 처리가 실패했다. */
#define F_INT_TLB_MISS_FAULT			(BIT(4) | BIT(11))
/* [한국어] 미스 트랜잭션 큐가 넘쳤다. */
#define F_INT_MISS_TRANSACTION_FIFO_FAULT	(BIT(5) | BIT(12))
/* [한국어] 미리 읽기 트랜잭션 큐가 넘쳤다. */
#define F_INT_PRETETCH_TRANSATION_FIFO_FAULT	(BIT(6) | BIT(13))

/* [한국어] 무효화 완료를 알리는 레지스터. 0이 아니게 되면 끝난 것이고,
 * 소프트웨어가 다시 0으로 지워야 한다. */
#define REG_MMU_CPE_DONE			0x12C

/* [한국어] 폴트 상태 — 어느 변환 유닛에서 어떤 폴트가 났는지. */
#define REG_MMU_FAULT_ST1			0x134
/* [한국어] 0번 유닛의 폴트 비트들(7종). */
#define F_REG_MMU0_FAULT_MASK			GENMASK(6, 0)
/* [한국어] 1번 유닛의 폴트 비트들 — 같은 7종이 7비트 위에 놓인다. */
#define F_REG_MMU1_FAULT_MASK			GENMASK(13, 7)

/* [한국어] 0번 유닛에서 폴트가 난 가상 주소. */
#define REG_MMU0_FAULT_VA			0x13c
/* [한국어] 그 주소의 하위 부분(비트 31:12). */
#define F_MMU_INVAL_VA_31_12_MASK		GENMASK(31, 12)
/* [한국어] 34비트 IOVA를 쓰는 하드웨어에서, 주소의 상위 3비트가
 * 하위 비트 자리를 빌려 실려 온다. */
#define F_MMU_INVAL_VA_34_32_MASK		GENMASK(11, 9)
/* [한국어] 마찬가지로 물리 주소의 상위 3비트도 이 자리에 실린다. */
#define F_MMU_INVAL_PA_34_32_MASK		GENMASK(8, 6)
/* [한국어] 폴트를 낸 접근이 쓰기였는가. */
#define F_MMU_FAULT_VA_WRITE_BIT		BIT(1)
/* [한국어] 폴트가 몇 단계에서 났는가(1단계인가 2단계인가). */
#define F_MMU_FAULT_VA_LAYER_BIT		BIT(0)

/* [한국어] 0번 유닛이 만들어 낸(잘못된) 물리 주소. */
#define REG_MMU0_INVLD_PA			0x140
/* [한국어] 1번 유닛의 폴트 주소. */
#define REG_MMU1_FAULT_VA			0x144
/* [한국어] 1번 유닛의 잘못된 물리 주소. */
#define REG_MMU1_INVLD_PA			0x148
/* [한국어] 0번 유닛에서 폴트를 낸 마스터의 식별자. */
#define REG_MMU0_INT_ID				0x150
/* [한국어] 1번 유닛의 같은 레지스터. */
#define REG_MMU1_INT_ID				0x154
/* [한국어] 식별자에서 larb(공통) 번호를 뽑는다 — 2비트 서브커먼 구성. */
#define F_MMU_INT_ID_COMM_ID(a)			(((a) >> 9) & 0x7)
/* [한국어] 그 구성에서의 서브커먼 번호(2비트). */
#define F_MMU_INT_ID_SUB_COMM_ID(a)		(((a) >> 7) & 0x3)
/* [한국어] 3비트 서브커먼 구성에서의 larb 번호 — 자리가 한 칸 밀린다. */
#define F_MMU_INT_ID_COMM_ID_EXT(a)		(((a) >> 10) & 0x7)
/* [한국어] 그 구성에서의 서브커먼 번호(3비트). */
#define F_MMU_INT_ID_SUB_COMM_ID_EXT(a)		(((a) >> 7) & 0x7)
/* Macro for 5 bits length port ID field (default) */
/* [한국어] 서브커먼이 없는 구성의 larb 번호. */
#define F_MMU_INT_ID_LARB_ID(a)			(((a) >> 7) & 0x7)
/* [한국어] 기본 구성의 포트 번호(5비트). */
#define F_MMU_INT_ID_PORT_ID(a)			(((a) >> 2) & 0x1f)
/* Macro for 6 bits length port ID field */
/* [한국어] 포트 번호가 6비트인 하드웨어의 larb 번호 — 한 칸 밀린다. */
#define F_MMU_INT_ID_LARB_ID_WID_6(a)		(((a) >> 8) & 0x7)
/* [한국어] 그 구성의 포트 번호(6비트). */
#define F_MMU_INT_ID_PORT_ID_WID_6(a)		(((a) >> 2) & 0x3f)

/* [한국어] 보호 메모리에 요구되는 정렬. 폴트 시 접근이 이리로
 * 돌려지므로, 실제 메모리가 이 정렬로 잡혀 있어야 한다. */
#define MTK_PROTECT_PA_ALIGN			256
/* [한국어] 뱅크 하나가 차지하는 레지스터 창의 크기(4KB).
 * 뱅크 번호에 이 값을 곱해 기준 주소에 더한다. */
#define MTK_IOMMU_BANK_SZ			0x1000

/* [한국어] PERICFG 안에서 INFRA IOMMU의 마스터 활성화 비트가 있는
 * 레지스터. MM 계열의 larb에 해당하는 것이 INFRA에서는 이것이다. */
#define PERICFG_IOMMU_1				0x714

/* [한국어] 4GB 모드 — 물리 주소를 재배치해 4GB 위를 쓰는 구성. */
#define HAS_4GB_MODE			BIT(0)
/* HW will use the EMI clock if there isn't the "bclk". */
/* [한국어] 전용 bclk 클럭이 필요한 하드웨어. 없으면 EMI 클럭을 쓴다. */
#define HAS_BCLK			BIT(1)
/* [한국어] 유효 물리 주소 범위 레지스터가 있다(4GB 모드에서 쓴다). */
#define HAS_VLD_PA_RNG			BIT(2)
/* [한국어] MISC_CTRL을 0으로 써 AXI를 리셋해야 하는 구형 하드웨어. */
#define RESET_AXI			BIT(3)
/* [한국어] 순서 없는 쓰기를 허용한다(성능 향상). */
#define OUT_ORDER_WR_EN			BIT(4)
/* [한국어] 인터럽트 ID에서 서브커먼이 2비트인 구성. */
#define HAS_SUB_COMM_2BITS		BIT(5)
/* [한국어] 서브커먼이 3비트인 구성. */
#define HAS_SUB_COMM_3BITS		BIT(6)
/* [한국어] 쓰기 조절을 켠다 — 버스를 독점하지 않게 한다. */
#define WR_THROT_EN			BIT(7)
/* [한국어] 보호 주소 레지스터의 형식이 구형인 하드웨어. */
#define HAS_LEGACY_IVRP_PADDR		BIT(8)
/* [한국어] IOVA가 34비트다 — 16GB의 주소 공간을 쓸 수 있다. */
#define IOVA_34_EN			BIT(9)
/* [한국어] 두 M4U가 페이지 테이블을 공유한다. */
#define SHARE_PGTABLE			BIT(10) /* 2 HW share pgtable */
/* [한국어] 동적 클럭 관리를 꺼야 하는 하드웨어. */
#define DCM_DISABLE			BIT(11)
/* [한국어] 표준 AXI 모드를 쓴다(멀티미디어가 아닌 IOMMU용). */
#define STD_AXI_MODE			BIT(12) /* For non MM iommu */
/* 2 bits: iommu type */
/* [한국어] 멀티미디어용 — larb를 거쳐 마스터가 붙는다. */
#define MTK_IOMMU_TYPE_MM		(0x0 << 13)
/* [한국어] 인프라용 — larb 없이 PERICFG나 ATF로 포트를 켠다. */
#define MTK_IOMMU_TYPE_INFRA		(0x1 << 13)
/* [한국어] AI 처리 유닛용. */
#define MTK_IOMMU_TYPE_APU		(0x2 << 13)
/* [한국어] 위 세 종류를 가려내는 마스크. */
#define MTK_IOMMU_TYPE_MASK		(0x3 << 13)
/* PM and clock always on. e.g. infra iommu */
/* [한국어] 전원과 클럭이 항상 켜져 있다. 무효화가 전원 상태를
 * 확인하지 않아도 된다는 뜻이기도 하다. */
#define PM_CLK_AO			BIT(15)
/* [한국어] 이 INFRA IOMMU가 PCIe를 지원한다 — 포트 비트를
 * 하나 더 켜야 하는 특수 처리가 붙는다. */
#define IFA_IOMMU_PCIE_SUPPORT		BIT(16)
/* [한국어] 페이지 테이블 자체가 35비트 물리 주소에 놓일 수 있다. */
#define PGTABLE_PA_35_EN		BIT(17)
/* [한국어] MT8173 계열의 보호 주소 비트 위치를 쓴다. */
#define TF_PORT_TO_ADDR_MT8173		BIT(18)
/* [한국어] 인터럽트 ID의 포트 필드가 6비트인 하드웨어. */
#define INT_ID_PORT_WIDTH_6		BIT(19)
/* [한국어] INFRA 마스터의 설정을 ATF에 SMC로 맡긴다 — 커널이
 * 직접 쓸 수 없는 보안 레지스터이기 때문이다. */
#define CFG_IFA_MASTER_IN_ATF		BIT(20)
/* [한국어] 하나의 마스터가 여러 larb에 걸쳐 있을 수 있다. */
#define DL_WITH_MULTI_LARB		BIT(21)

/* [한국어] 플래그를 마스크로 걸러 특정 값과 같은지 본다.
 * 값이 여러 비트인 종류(IOMMU_TYPE)를 다루기 위해 마스크가 따로 있다. */
#define MTK_IOMMU_HAS_FLAG_MASK(pdata, _x, mask)	\
				((((pdata)->flags) & (mask)) == (_x))

/* [한국어] 단일 비트 플래그가 켜져 있는지 본다 — 마스크가 값과 같다. */
#define MTK_IOMMU_HAS_FLAG(pdata, _x)	MTK_IOMMU_HAS_FLAG_MASK(pdata, _x, _x)
/* [한국어] IOMMU의 종류를 판별한다. 2비트 필드라 전용 마스크를 쓴다. */
#define MTK_IOMMU_IS_TYPE(pdata, _x)	MTK_IOMMU_HAS_FLAG_MASK(pdata, _x,\
							MTK_IOMMU_TYPE_MASK)

/* [한국어] "이 자리에 larb가 없다"를 뜻하는 표식.
 * larbid_remap 표에서 배선되지 않은 자리를 메우는 데 쓴다. */
#define MTK_INVALID_LARBID		MTK_LARB_NR_MAX

/* [한국어] 인터럽트 ID의 공통(common) 번호가 가질 수 있는 최대값. */
#define MTK_LARB_COM_MAX	8
/* [한국어] 서브커먼 번호의 최대값. 이 둘이 larbid_remap 표의 크기를 정한다. */
#define MTK_LARB_SUBCOM_MAX	8

/* [한국어] 한 M4U가 가질 수 있는 IOMMU 그룹의 최대 개수.
 * IOVA 영역 수와 뱅크 수 중 큰 쪽을 덮을 만큼이면 된다. */
#define MTK_IOMMU_GROUP_MAX	8
/* [한국어] 하드웨어가 가질 수 있는 뱅크의 최대 개수. */
#define MTK_IOMMU_BANK_MAX	5

/* [한국어] 지원하는 SoC의 목록.
 * 대부분의 차이는 flags가 흡수하지만, 4GB 모드의 infracfg 호환
 * 문자열처럼 SoC를 직접 알아야 하는 곳이 남아 이 열거형이 있다. */
enum mtk_iommu_plat {
	M4U_MT2712,	/* [한국어] 이 SoC를 가리키는 식별자. */
	M4U_MT6779,
	M4U_MT6795,
	M4U_MT8167,
	M4U_MT8173,
	M4U_MT8183,
	M4U_MT8186,
	M4U_MT8188,
	M4U_MT8189,
	M4U_MT8192,
	M4U_MT8195,
	M4U_MT8365,
};

/* [한국어] IOVA 공간을 나눈 영역 하나. */
struct mtk_iommu_iova_region {
	dma_addr_t		iova_base;
	/* [한국어] 이 영역의 시작 IOVA.
	 * 읽는 자: 도메인의 aperture 시작이 되고, 상위 32비트가
	 *          larb의 "뱅크" 값으로도 쓰인다(mtk_iommu_config).
	 * 값 범위: 보통 4GB의 배수다. */

	unsigned long long	size;
	/* [한국어] 이 영역의 크기.
	 * 값 범위: 4GB에서 8MB를 뺀 값이 흔하다 — 영역 사이에
	 *          간격을 두어 경계에서의 오작동을 피한다. */
};

/* [한국어] 절전 동안 보존할 레지스터 값들. */
struct mtk_iommu_suspend_reg {
	u32			misc_ctrl;
	/* [한국어] AXI 동작 방식 설정. 전역이라 뱅크 0의 것만 저장한다. */

	u32			dcm_dis;
	/* [한국어] 동적 클럭 관리 설정. */

	u32			ctrl_reg;
	/* [한국어] 주 제어 레지스터(폴트 처리 방식 등). */

	u32			vld_pa_rng;
	/* [한국어] 유효 물리 주소 범위(4GB 모드용). */

	u32			wr_len_ctrl;
	/* [한국어] 쓰기 조절 설정.
	 * 특별한 쓰임: 리줌이 이 값이 0인지로 "아직 한 번도 설정되지
	 *              않았다"를 판별한다 — 첫 리줌에서 쓰레기 값을
	 *              되쓰지 않기 위한 장치다. */

	u32			int_control[MTK_IOMMU_BANK_MAX];
	/* [한국어] 뱅크별 인터럽트 제어 0. 뱅크마다 독립이라 배열이다. */

	u32			int_main_control[MTK_IOMMU_BANK_MAX];
	/* [한국어] 뱅크별 주 인터럽트 제어. */

	u32			ivrp_paddr[MTK_IOMMU_BANK_MAX];
	/* [한국어] 뱅크별 보호 메모리 주소. */
};

/* [한국어] SoC 하나의 모든 차이를 담은 표.
 * 이 구조체가 있어서 나머지 코드가 SoC를 거의 의식하지 않는다. */
struct mtk_iommu_plat_data {
	enum mtk_iommu_plat	m4u_plat;
	/* [한국어] 어느 SoC인가.
	 * 읽는 자: 4GB 모드의 구형 infracfg 호환 문자열을 고를 때만
	 *          쓰인다 — 나머지 차이는 모두 flags가 흡수한다. */

	u32			flags;
	/* [한국어] 22개의 능력/동작 비트를 담은 묶음.
	 * 읽는 자: MTK_IOMMU_HAS_FLAG로 곳곳에서 분기한다.
	 * 이 하나가 레지스터 배치, IOVA 폭, 전원 방식, 인터럽트 ID
	 * 해석까지 정한다. */

	u32			inv_sel_reg;
	/* [한국어] 무효화 대상 선택 레지스터의 오프셋.
	 * 값 범위: 1세대와 2세대에서 자리가 달라 둘 중 하나다. */

	char			*pericfg_comp_str;
	/* [한국어] INFRA IOMMU가 포트를 켤 PERICFG 영역의 호환 문자열.
	 * 값 범위: ATF에 맡기는 구성에서는 NULL이다. */

	struct list_head	*hw_list;
	/* [한국어] 이 하드웨어가 속할 전역 목록(m4ulist/infralist/apulist).
	 * 왜 필요한가: 테이블을 공유하는 하드웨어들이 서로를 찾는
	 *              통로다. 공유하지 않으면 자기 목록을 쓴다. */

	/*
	 * The IOMMU HW may support 16GB iova. In order to balance the IOVA ranges,
	 * different masters will be put in different iova ranges, for example vcodec
	 * is in 4G-8G and cam is in 8G-12G. Meanwhile, some masters may have the
	 * special IOVA range requirement, like CCU can only support the address
	 * 0x40000000-0x44000000.
	 * Here list the iova ranges this SoC supports and which larbs/ports are in
	 * which region.
	 *
	 * 16GB iova all use one pgtable, but each a region is a iommu group.
	 */
	/* [한국어] 원본 주석의 마지막 줄이 요점이다: **16GB를 한 테이블로
	 * 덮되, 영역마다 IOMMU 그룹을 나눈다.** 마스터를 서로 다른
	 * 영역에 두면 IOVA 할당이 분산되고, CCU처럼 특정 주소만 쓸 수
	 * 있는 마스터도 수용할 수 있다. */
	struct {
		unsigned int	iova_region_nr;
		/* [한국어] 이 SoC가 나눈 IOVA 영역의 개수.
		 * 값 범위: 1이면 영역 구분이 없어 그룹도 하나다. */

		const struct mtk_iommu_iova_region	*iova_region;
		/* [한국어] 그 영역들의 목록.
		 * 읽는 자: 도메인의 aperture와 larb의 뱅크 값이 여기서 나온다. */

		/*
		 * Indicate the correspondance between larbs, ports and regions.
		 *
		 * The index is the same as iova_region and larb port numbers are
		 * described as bit positions.
		 * For example, storing BIT(0) at index 2,1 means "larb 1, port0 is in region 2".
		 *              [2] = { [1] = BIT(0) }
		 */
		const u32	(*iova_region_larb_msk)[MTK_LARB_NR_MAX];
		/* [한국어] "어느 larb의 어느 포트가 어느 영역에 속하는가"의 표.
		 * 첫 인덱스가 영역, 둘째가 larb, 각 값이 포트 비트맵이다.
		 * 읽는 자: get_iova_region_id가 이 표를 훑어 마스터의
		 *          영역을 찾는다.
		 * 값 범위: NULL이면 영역이 하나뿐인 SoC다. */
	};

	/*
	 * The IOMMU HW may have 5 banks. Each bank has a independent pgtable.
	 * Here list how many banks this SoC supports/enables and which ports are in which bank.
	 */
	struct {	/* [한국어] 뱅크 관련 설정을 익명 구조체로 묶어 둔다. */
		u8		banks_num;
		/* [한국어] 이 하드웨어가 가진 뱅크의 수.
		 * 읽는 자: 레지스터 영역의 크기 검사와 뱅크 순회.
		 * 값 범위: 1~5. */

		bool		banks_enable[MTK_IOMMU_BANK_MAX];
		/* [한국어] 그중 실제로 쓸 뱅크들.
		 * 왜 따로 있는가: 하드웨어에 5개가 있어도 쓰지 않는 것이
		 *                 있다 — MT8195 INFRA는 0번과 4번만 쓴다. */

		unsigned int	banks_portmsk[MTK_IOMMU_BANK_MAX];
		/* [한국어] 뱅크마다 어느 포트들이 속하는지의 비트맵.
		 * 읽는 자: get_bank_id가 마스터의 포트로 뱅크를 찾는다.
		 * 왜 중요한가: 뱅크는 테이블이 진짜로 나뉘므로, 이 표가
		 *              곧 주소 공간의 분할을 뜻한다. */
	};

	unsigned char       larbid_remap[MTK_LARB_COM_MAX][MTK_LARB_SUBCOM_MAX];
	/* [한국어] 인터럽트가 알려 준 하드웨어 larb 번호를 소프트웨어
	 * 번호로 옮기는 표.
	 * 왜 필요한가: SMI의 배선이 SoC마다 달라, 인터럽트가 보고하는
	 *              (공통, 서브커먼) 쌍이 디바이스 트리의 larb 번호와
	 *              일치하지 않는다. 폴트 로그를 사람이 읽을 수 있게
	 *              하려면 이 표를 거쳐야 한다. */
};

/* [한국어] 뱅크 하나의 상태. 뱅크마다 독립된 테이블과 도메인을 갖는다. */
struct mtk_iommu_bank_data {
	void __iomem			*base;
	/* [한국어] 이 뱅크의 4KB 레지스터 창.
	 * 설정자: probe가 기준 주소에 뱅크 번호 × 4KB를 더해 만든다. */

	int				irq;
	/* [한국어] 이 뱅크의 폴트 인터럽트. 뱅크마다 따로 있다. */

	u8				id;
	/* [한국어] 뱅크 번호. 무효화가 다른 하드웨어의 같은 번호 뱅크를
	 * 찾는 데도 쓰인다. */

	struct device			*parent_dev;
	/* [한국어] 이 뱅크를 품은 M4U의 디바이스. 로그와 인터럽트 등록용. */

	struct mtk_iommu_data		*parent_data;
	/* [한국어] 그 M4U의 상태. 무효화가 여기서 목록과 플래그를 얻는다. */

	spinlock_t			tlb_lock; /* lock for tlb range flush */
	/* [한국어] 이 뱅크의 무효화를 직렬화하는 락.
	 * 왜 뱅크마다인가: 무효화가 시작/끝 주소를 두 레지스터에 나눠
	 *                  쓰고 완료를 기다리는 절차라, 두 CPU가 겹치면
	 *                  범위가 뒤섞인다. 뱅크가 다르면 레지스터도
	 *                  달라 겹칠 일이 없다. */

	struct mtk_iommu_domain		*m4u_dom; /* Each bank has a domain */
	/* [한국어] 이 뱅크에 붙어 있는 도메인.
	 * 값 범위: NULL이면 아직 하드웨어가 초기화되지 않았다는 뜻이라,
	 *          attach가 그것을 보고 초기화 여부를 판단한다. */
};

/* [한국어] M4U 하드웨어 하나의 상태. */
struct mtk_iommu_data {
	struct device			*dev;
	/* [한국어] 이 M4U의 플랫폼 디바이스. */

	struct clk			*bclk;
	/* [한국어] 전용 클럭. HAS_BCLK가 없으면 NULL이고, 그때는
	 * 하드웨어가 EMI 클럭을 쓴다. */

	phys_addr_t			protect_base; /* protect memory base */
	/* [한국어] 폴트 시 접근을 돌릴 보호 메모리의 물리 주소.
	 * 왜 필요한가: 잘못된 DMA를 그냥 버리는 대신 이 안전한 자리로
	 *              보내, 시스템 메모리가 훼손되지 않게 한다. */

	struct mtk_iommu_suspend_reg	reg;
	/* [한국어] 절전 동안 보존할 레지스터 값들. */

	struct iommu_group		*m4u_group[MTK_IOMMU_GROUP_MAX];
	/* [한국어] 그룹 번호로 찾는 IOMMU 그룹 배열.
	 * 왜 배열인가: 뱅크나 IOVA 영역이 곧 그룹이므로, 그 번호를
	 *              인덱스로 쓰면 같은 그룹을 재사용할 수 있다.
	 * 동기화: mutex. */

	bool                            enable_4GB;
	/* [한국어] 4GB 모드가 켜졌는가.
	 * 설정자: probe가 infracfg 레지스터를 읽어 정한다.
	 * 읽는 자: map이 물리 주소에 비트 32를 얹을지, iova_to_phys가
	 *          그것을 되돌릴지 정한다. */

	struct iommu_device		iommu;
	/* [한국어] IOMMU 코어에 등록되는 부분(임베드). */

	const struct mtk_iommu_plat_data *plat_data;
	/* [한국어] 이 SoC의 설정 표. 거의 모든 분기가 여기를 본다. */

	struct device			*smicomm_dev;
	/* [한국어] 이 M4U가 붙어 있는 SMI common 디바이스.
	 * 왜 필요한가: M4U가 깨어날 때 SMI common도 켜져 있어야 해서,
	 *              그쪽을 소비자로 하는 전원 링크를 만든다. */

	struct mtk_iommu_bank_data	*bank;
	/* [한국어] 뱅크 배열. banks_num개가 들어 있다. */

	struct mtk_iommu_domain		*share_dom;
	/* [한국어] 페이지 테이블을 공유할 대표 도메인.
	 * 왜 필요한가: IOVA 영역이 여럿이어도 테이블은 하나여야 한다.
	 *              첫 도메인이 테이블을 만들고 이 자리에 등록하면,
	 *              이후 도메인들이 그것을 그대로 가져다 쓴다.
	 * 동기화: mutex. */

	struct regmap			*pericfg;
	/* [한국어] INFRA IOMMU가 포트를 켤 PERICFG 영역.
	 * 값 범위: MM 계열이거나 ATF에 맡기는 구성에서는 NULL이다. */

	struct mutex			mutex; /* Protect m4u_group/m4u_dom above */
	/* [한국어] 그룹 배열과 공유 도메인, 뱅크의 도메인 연결을 보호한다.
	 * 스핀락이 아닌 이유: 그 안에서 테이블 생성과 전원 조작 같은
	 *                     잠들 수 있는 작업을 하기 때문이다. */

	/*
	 * In the sharing pgtable case, list data->list to the global list like m4ulist.
	 * In the non-sharing pgtable case, list data->list to the itself hw_list_head.
	 */
	/* [한국어] 아래 세 필드가 "테이블을 공유하는가"를 표현하는 방식이다.
	 * 공유하면 전역 목록을 가리키고, 아니면 자기 안의 목록을 가리켜
	 * 혼자만 들어 있게 한다. 덕분에 무효화 코드는 공유 여부를
	 * 의식하지 않고 목록을 훑기만 하면 된다. */
	struct list_head		*hw_list;
	/* [한국어] 실제로 순회할 목록의 주소. 위 둘 중 하나를 가리킨다. */

	struct list_head		hw_list_head;
	/* [한국어] 공유하지 않을 때 쓰는 자기만의 목록 머리. */

	struct list_head		list;
	/* [한국어] 그 목록에 자신을 매다는 고리. */

	struct mtk_smi_larb_iommu	larb_imu[MTK_LARB_NR_MAX];
	/* [한국어] larb마다의 IOMMU 설정. **SMI 드라이버와 공유하는
	 * 구조체**로, 이 파일이 여기에 포트 비트를 세우면 SMI 쪽이
	 * 실제 레지스터에 쓴다.
	 * 설정자: component bind가 larb 드라이버로부터 채운다. */
};

/* [한국어] IOMMU 도메인 하나. */
struct mtk_iommu_domain {
	struct io_pgtable_cfg		cfg;
	/* [한국어] 페이지 테이블 설정. 공유하는 경우 대표 도메인의
	 * 것을 통째로 복사해 온다.
	 * 읽는 자: attach가 cfg.arm_v7s_cfg.ttbr를 하드웨어에 심는다. */

	struct io_pgtable_ops		*iop;
	/* [한국어] 페이지 테이블 연산. map/unmap이 그대로 위임한다.
	 * 공유하는 도메인들은 같은 포인터를 갖는다 — 그것이 곧
	 * 테이블 공유의 실체다. */

	struct mtk_iommu_bank_data	*bank;
	/* [한국어] 이 도메인이 붙어 있는 뱅크.
	 * 값 범위: NULL이면 아직 완성되지 않은 도메인이라,
	 *          attach가 그것을 보고 초기화 여부를 판단한다. */

	struct iommu_domain		domain;
	/* [한국어] 코어가 보는 도메인 부분(임베드). */

	struct mutex			mutex; /* Protect "data" in this structure */
	/* [한국어] 이 도메인의 완성 과정을 보호한다.
	 * 왜 필요한가: 여러 디바이스가 같은 도메인에 동시에 붙을 수
	 *              있는데, 테이블은 한 번만 만들어야 한다. */
};

/*
 * [한국어]
 * mtk_iommu_bind - component 프레임워크가 larb들을 결합시킬 때 불린다
 *
 * @dev: M4U 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 이 드라이버가 component 프레임워크를 쓰는 이유가 여기 있다.
 * larb 드라이버들이 모두 준비된 뒤에야 이 콜백이 불리고, 그때
 * larb_imu 배열이 채워진다. 그러기 전에는 포트를 켤 수 없으므로
 * 순서를 보장할 장치가 필요했던 것이다.
 *
 * 실행 컨텍스트: component 코어. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   component 코어 → [mtk_iommu_bind] → component_bind_all()
 */
static int mtk_iommu_bind(struct device *dev)
{
	/* [한국어] 이 M4U의 상태. */
	struct mtk_iommu_data *data = dev_get_drvdata(dev);

	/* [한국어] 모든 larb 드라이버가 larb_imu 배열을 채우게 한다. */
	return component_bind_all(dev, &data->larb_imu);
}

/*
 * [한국어]
 * mtk_iommu_unbind - larb들과의 결합을 푼다
 *
 * @dev: M4U 디바이스.
 * @return: 없음.
 *
 * 실행 컨텍스트: component 코어. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   component 코어 → [mtk_iommu_unbind]
 */
static void mtk_iommu_unbind(struct device *dev)
{
	/* [한국어] 이 M4U의 상태. */
	struct mtk_iommu_data *data = dev_get_drvdata(dev);

	/* [한국어] larb_imu 배열의 연결을 되돌린다. */
	component_unbind_all(dev, &data->larb_imu);
}

/* [한국어] 연산 테이블의 전방 선언. probe가 정의보다 앞에서 참조한다. */
static const struct iommu_ops mtk_iommu_ops;

/* [한국어] 하드웨어 초기화 함수의 전방 선언. attach가 이것을 부르는데
 * 정의는 훨씬 뒤에 있다. */
static int mtk_iommu_hw_init(const struct mtk_iommu_data *data, unsigned int bankid);

/* [한국어] IOVA를 무효화 레지스터가 요구하는 형식으로 바꾼다.
 * 34비트 IOVA를 32비트 레지스터에 담기 위한 인코딩인데,
 * 하위 절반에서는 페이지 오프셋(비트 11:0)이 필요 없으므로
 * 그 자리에 상위 비트를 얹는다. */
#define MTK_IOMMU_TLB_ADDR(iova) ({					\
	dma_addr_t _addr = iova;					\
	((lower_32_bits(_addr) & GENMASK(31, 12)) | upper_32_bits(_addr));\
})	/* [한국어] 매크로 본문의 끝 — 값을 내는 식이라 괄호로 감쌌다. */

/*
 * In M4U 4GB mode, the physical address is remapped as below:
 *
 * CPU Physical address:
 * ====================
 *
 * 0      1G       2G     3G       4G     5G
 * |---A---|---B---|---C---|---D---|---E---|
 * +--I/O--+------------Memory-------------+
 *
 * IOMMU output physical address:
 *  =============================
 *
 *                                 4G      5G     6G      7G      8G
 *                                 |---E---|---B---|---C---|---D---|
 *                                 +------------Memory-------------+
 *
 * The Region 'A'(I/O) can NOT be mapped by M4U; For Region 'B'/'C'/'D', the
 * bit32 of the CPU physical address always is needed to set, and for Region
 * 'E', the CPU physical address keep as is.
 * Additionally, The iommu consumers always use the CPU phyiscal address.
 */
/* [한국어] 위 그림이 4GB 모드의 전부다. CPU가 보는 물리 주소와
 * IOMMU가 내보내는 물리 주소가 **다르다**는 것이 요점이다.
 *
 * 4GB 이상의 메모리를 32비트 버스로 다루기 위해, 하드웨어가 아래
 * 3GB의 메모리를 4GB 위로 옮겨 놓았다. 그래서 커널이 준 CPU 물리
 * 주소에 비트 32를 얹으면 IOMMU가 쓸 주소가 된다.
 *
 * 다만 이미 4GB 위에 있던 영역 E는 그대로 두어야 하므로, 되돌릴
 * 때(iova_to_phys) 이 상수와 비교해 구별한다.
 *
 * 원본 주석의 마지막 줄이 중요하다: **소비자는 언제나 CPU 물리
 * 주소를 쓴다.** 그래서 map이 얹고 iova_to_phys가 벗긴다. */
#define MTK_IOMMU_4GB_MODE_REMAP_BASE	 0x140000000UL

/* [한국어] APU IOMMU들이 함께 들어가는 전역 목록. */
static LIST_HEAD(apulist);	/* List the apu iommu HWs */
/* [한국어] INFRA IOMMU들의 전역 목록. */
static LIST_HEAD(infralist);	/* List the iommu_infra HW */
/* [한국어] 멀티미디어 M4U들의 전역 목록.
 * 이 목록에 함께 들어간다는 것이 곧 페이지 테이블을 공유한다는 뜻이며,
 * 무효화가 목록 전체를 훑는 근거이기도 하다. */
static LIST_HEAD(m4ulist);	/* List all the M4U HWs */

/* [한국어] 같은 목록에 있는 M4U들을 훑는 관용구.
 * 공유하지 않는 하드웨어는 자기만 들어 있는 목록을 가리키므로,
 * 이 매크로 하나로 두 경우를 모두 다룰 수 있다. */
#define for_each_m4u(data, head)  list_for_each_entry(data, head, list)

/* [한국어] 한 영역이 덮는 크기. 4GB에서 8MB를 뺀 것인데,
 * 영역 사이에 간격을 두어 경계에서의 오작동을 피하려는 것이다. */
#define MTK_IOMMU_IOVA_SZ_4G		(SZ_4G - SZ_8M) /* 8M as gap */

/* [한국어] 영역을 나누지 않는 SoC가 쓰는 단일 영역.
 * 0부터 거의 4GB까지 하나로 덮는다. */
static const struct mtk_iommu_iova_region single_domain[] = {
	{.iova_base = 0,		.size = MTK_IOMMU_IOVA_SZ_4G},	/* [한국어] 이 영역의 시작 주소와 크기. */
};

/* [한국어] 다중 영역 구성이 가질 수 있는 최대 영역 수. */
#define MT8192_MULTI_REGION_NR_MAX	6

/* [한국어] 실제로 쓸 영역 수. **32비트 DMA 주소 커널에서는 1로 접힌다** —
 * 4GB 위의 영역을 표현할 수 없기 때문이다. 그래서 아래 배열들이
 * 조건부 컴파일로 뒷부분을 잘라 낸다. */
#define MT8192_MULTI_REGION_NR	(IS_ENABLED(CONFIG_ARCH_DMA_ADDR_T_64BIT) ? \
				 MT8192_MULTI_REGION_NR_MAX : 1)

/* [한국어] MT8189 APU의 영역 구성. 용도별로 크기가 제각각인데,
 * AI 가속기의 보안 영역, 코드, 로컬 메모리, 벡터 유닛이 서로 다른
 * 주소 요구를 갖기 때문이다. */
static const struct mtk_iommu_iova_region mt8189_multi_dom_apu[] = {
	{ .iova_base = 0x200000ULL,	.size = SZ_512M},	/* APU SECURE */	/* [한국어] 이 영역의 시작 주소와 크기. */
#if IS_ENABLED(CONFIG_ARCH_DMA_ADDR_T_64BIT)
	{ .iova_base = SZ_1G,		.size = 0xc0000000},	/* APU CODE */
	{ .iova_base = 0x70000000ULL,	.size = 0x12600000},	/* APU VLM */
	{ .iova_base = SZ_4G,		.size = SZ_4G * 3},	/* APU VPU */
#endif
};

/* [한국어] 멀티미디어 SoC들이 공유하는 영역 구성.
 * 앞의 넷은 4GB씩 균등하게 나눈 것이고, 뒤의 둘은 CCU(카메라 제어
 * 유닛)처럼 좁은 주소만 쓸 수 있는 마스터를 위한 특수 영역이다.
 * 그 특수 영역이 앞 영역과 겹치는 자리에 있다는 점에 유의 —
 * get_resv_regions가 그 겹침을 예약으로 알려 준다. */
static const struct mtk_iommu_iova_region mt8192_multi_dom[MT8192_MULTI_REGION_NR] = {
	{ .iova_base = 0x0,		.size = MTK_IOMMU_IOVA_SZ_4G},	/* 0 ~ 4G,  */	/* [한국어] 이 영역의 시작 주소와 크기. */
	#if IS_ENABLED(CONFIG_ARCH_DMA_ADDR_T_64BIT)
	{ .iova_base = SZ_4G,		.size = MTK_IOMMU_IOVA_SZ_4G},	/* 4G ~ 8G */
	{ .iova_base = SZ_4G * 2,	.size = MTK_IOMMU_IOVA_SZ_4G},	/* 8G ~ 12G */
	{ .iova_base = SZ_4G * 3,	.size = MTK_IOMMU_IOVA_SZ_4G},	/* 12G ~ 16G */

	{ .iova_base = 0x240000000ULL,	.size = 0x4000000},	/* CCU0 */
	{ .iova_base = 0x244000000ULL,	.size = 0x4000000},	/* CCU1 */
	#endif
};

/* If 2 M4U share a domain(use the same hwlist), Put the corresponding info in first data.*/
/*
 * [한국어]
 * mtk_iommu_get_frst_data - 목록의 첫 하드웨어를 얻는다
 *
 * @hwlist: 순회할 목록.
 * @return: 첫 항목.
 *
 * 원본 주석이 밝히는 규약이 이 함수의 존재 이유다: **공유하는
 * 하드웨어들의 공통 정보는 첫 번째 것에 둔다.** 페이지 테이블,
 * 그룹 배열, 공유 도메인이 모두 그렇다.
 *
 * 실행 컨텍스트: attach와 그룹 결정.
 *
 * 호출 체인:
 *   attach_device() / device_group() → [mtk_iommu_get_frst_data]
 */
static struct mtk_iommu_data *mtk_iommu_get_frst_data(struct list_head *hwlist)
{
	/* [한국어] 공유 정보는 언제나 첫 항목에 있다. */
	return list_first_entry(hwlist, struct mtk_iommu_data, list);
}

/*
 * [한국어]
 * to_mtk_domain - iommu_domain에서 바깥 도메인을 복원한다
 *
 * @dom: 코어가 넘겨준 도메인.
 * @return: 드라이버 쪽 도메인.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄.
 *
 * 호출 체인:
 *   각종 iommu_domain_ops 콜백 → [to_mtk_domain]
 */
static struct mtk_iommu_domain *to_mtk_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드 멤버의 주소에서 바깥 구조체를 역산한다. */
	return container_of(dom, struct mtk_iommu_domain, domain);
}

/*
 * [한국어]
 * mtk_iommu_tlb_flush_all - 이 하드웨어의 TLB를 통째로 비운다
 *
 * @data: 대상 M4U.
 * @return: 없음.
 *
 * 전체 무효화는 뱅크와 무관한 전역 동작이라, 원본 주석이 밝히듯
 * 항상 뱅크 0의 레지스터로 낸다.
 *
 * 완료를 기다리지 않고 wmb()만 거는 점에 유의 — 전체 무효화는
 * 범위 무효화와 달리 완료 신호가 없고, 쓰기가 하드웨어에
 * 도달했음만 보장하면 된다.
 *
 * 실행 컨텍스트: 무효화 경로와 리줌. 전원이 켜진 상태여야 한다.
 *
 * 호출 체인:
 *   flush_iotlb_all() / isr() / runtime_resume() / range_sync의 실패
 *   → [mtk_iommu_tlb_flush_all]
 */
static void mtk_iommu_tlb_flush_all(struct mtk_iommu_data *data)
{
	/* Tlb flush all always is in bank0. */
	/* [한국어] 전체 무효화는 전역 동작이라 뱅크 0으로만 낸다. */
	struct mtk_iommu_bank_data *bank = &data->bank[0];
	/* [한국어] 그 뱅크의 레지스터 창. */
	void __iomem *base = bank->base;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 선택 레지스터와 명령 레지스터를 연달아 써야 하므로
	 * 그 사이에 다른 무효화가 끼어들면 안 된다. */
	spin_lock_irqsave(&bank->tlb_lock, flags);
	/* [한국어] 두 변환 유닛을 모두 대상에 넣는다. 선택 레지스터의
	 * 오프셋은 세대마다 달라 plat_data가 알려 준다. */
	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0, base + data->plat_data->inv_sel_reg);
	/* [한국어] 전체 무효화 명령을 낸다. */
	writel_relaxed(F_ALL_INVLD, base + REG_MMU_INVALIDATE);
	/* [한국어] 쓰기가 하드웨어에 도달했음을 보장한다. 전체 무효화는
	 * 완료 신호가 없어 이것이 할 수 있는 전부다. */
	wmb(); /* Make sure the tlb flush all done */
	spin_unlock_irqrestore(&bank->tlb_lock, flags);	/* [한국어] 명령이 나갔으니 락을 놓는다. */
}

/*
 * [한국어]
 * mtk_iommu_tlb_flush_range_sync - 범위를 무효화하고 완료를 기다린다
 *
 * @iova: 무효화할 시작 주소.
 * @size: 크기.
 * @bank: 대상 뱅크(그 번호로 다른 하드웨어의 같은 뱅크도 찾는다).
 * @return: 없음.
 *
 * **목록의 모든 M4U에 대해 반복한다.** 테이블을 공유하는 하드웨어가
 * 여럿이면 각각의 TLB를 모두 비워야 하기 때문이다. 공유하지 않는
 * 경우 목록에 자기 하나뿐이라 같은 코드가 그대로 통한다.
 *
 * 전원 확인이 이 함수의 핵심 판단이다. 원본 주석이 설명하듯,
 * 꺼진 하드웨어를 일부러 깨워 무효화하지 않는다 — 다시 켜질 때
 * 리줌 콜백이 어차피 전체를 비우기 때문이다. 다만 두 가지 예외가
 * 있어 check_pm_status로 갈린다:
 *  - 전원 도메인이 없는 하드웨어(MT8173)도 이 검사를 거쳐야
 *    꺼진 상태의 타임아웃 로그를 피할 수 있다.
 *  - 전원이 항상 켜진 INFRA IOMMU는 검사 자체가 무의미하고,
 *    마스터와 전원 링크도 없어 상태가 늘 "쓰이지 않음"으로 보인다.
 *
 * 무효화가 시간 안에 끝나지 않으면 전체 무효화로 물러선다 —
 * 더 많이 비우는 쪽이 안전한 방향이다.
 *
 * 실행 컨텍스트: 해제/매핑 후 무효화. atomic 폴링을 쓴다.
 *
 * 호출 체인:
 *   iotlb_sync() / sync_map() → [mtk_iommu_tlb_flush_range_sync]
 */
static void mtk_iommu_tlb_flush_range_sync(unsigned long iova, size_t size,
					   struct mtk_iommu_bank_data *bank)
{
	/* [한국어] 테이블을 공유하는 하드웨어들의 목록. */
	struct list_head *head = bank->parent_data->hw_list;
	/* [한국어] 현재 하드웨어에서 같은 번호의 뱅크. */
	struct mtk_iommu_bank_data *curbank;
	/* [한국어] 순회 커서. */
	struct mtk_iommu_data *data;
	/* [한국어] 전원 상태를 확인해야 하는가. */
	bool check_pm_status;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 현재 뱅크의 레지스터 창. */
	void __iomem *base;
	/* [한국어] 완료 대기 결과와 읽은 값. */
	int ret;
	u32 tmp;

	/* [한국어] 이 테이블을 쓰는 모든 하드웨어에서 비운다. */
	for_each_m4u(data, head) {
		/*
		 * To avoid resume the iommu device frequently when the iommu device
		 * is not active, it doesn't always call pm_runtime_get here, then tlb
		 * flush depends on the tlb flush all in the runtime resume.
		 *
		 * There are 2 special cases:
		 *
		 * Case1: The iommu dev doesn't have power domain but has bclk. This case
		 * should also avoid the tlb flush while the dev is not active to mute
		 * the tlb timeout log. like mt8173.
		 *
		 * Case2: The power/clock of infra iommu is always on, and it doesn't
		 * have the device link with the master devices. This case should avoid
		 * the PM status check.
		 */
		/* [한국어] 전원이 항상 켜진 하드웨어는 상태를 확인할 이유가
		 * 없다 — 오히려 마스터와의 전원 링크가 없어 늘 "쓰이지 않음"
		 * 으로 보이므로, 검사하면 무효화를 통째로 건너뛰게 된다. */
		check_pm_status = !MTK_IOMMU_HAS_FLAG(data->plat_data, PM_CLK_AO);

		if (check_pm_status) {
			/* [한국어] 꺼져 있으면 일부러 깨우지 않는다 —
			 * 다시 켜질 때 리줌이 전체를 비워 준다. */
			if (pm_runtime_get_if_in_use(data->dev) <= 0)
				continue;
		}

		/* [한국어] 이 하드웨어에서 같은 번호의 뱅크를 찾는다.
		 * 공유하는 하드웨어들은 뱅크 구성이 같다는 전제다. */
		curbank = &data->bank[bank->id];
		base = curbank->base;

		/* [한국어] 네 번의 레지스터 쓰기가 하나의 절차를 이루므로
		 * 그 사이에 다른 무효화가 끼어들면 안 된다. */
		spin_lock_irqsave(&curbank->tlb_lock, flags);
		/* [한국어] 두 변환 유닛을 모두 대상에 넣는다. */
		writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
			       base + data->plat_data->inv_sel_reg);

		/* [한국어] 범위의 시작을 레지스터 형식으로 바꿔 쓴다. */
		writel_relaxed(MTK_IOMMU_TLB_ADDR(iova), base + REG_MMU_INVLD_START_A);
		/* [한국어] 끝은 마지막 바이트의 주소다(포함 구간). */
		writel_relaxed(MTK_IOMMU_TLB_ADDR(iova + size - 1),
			       base + REG_MMU_INVLD_END_A);
		/* [한국어] 그 범위에 대해 무효화를 실행시킨다. */
		writel_relaxed(F_MMU_INV_RANGE, base + REG_MMU_INVALIDATE);

		/* tlb sync */
		/* [한국어] 완료 레지스터가 0이 아니게 될 때까지 기다린다.
		 * 락을 쥔 채이므로 잠들지 않는 변형을 쓴다. */
		ret = readl_poll_timeout_atomic(base + REG_MMU_CPE_DONE,
						tmp, tmp != 0, 10, 1000);

		/* Clear the CPE status */
		/* [한국어] 완료 표시를 지워 다음 무효화가 그것을 오인하지
		 * 않게 한다. 타임아웃이었더라도 지운다. */
		writel_relaxed(0, base + REG_MMU_CPE_DONE);
		spin_unlock_irqrestore(&curbank->tlb_lock, flags);

		/* [한국어] 시간 안에 끝나지 않았다 — 더 많이 비우는 쪽으로
		 * 물러선다. 안전한 방향이다. */
		if (ret) {
			dev_warn(data->dev,	/* [한국어] 부분 무효화가 시간 안에 끝나지 않았음을 알린다. */
				 "Partial TLB flush timed out, falling back to full flush\n");
			mtk_iommu_tlb_flush_all(data);	/* [한국어] 더 많이 비우는 쪽으로 물러선다. */
		}

		/* [한국어] 올렸던 전원 참조를 놓는다. */
		if (check_pm_status)
			pm_runtime_put(data->dev);
	}
}

/*
 * [한국어]
 * mtk_iommu_isr - 폴트 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev_id: 등록 시 넘긴 뱅크.
 * @return: 항상 IRQ_HANDLED.
 *
 * 이 함수의 대부분은 **폴트를 낸 마스터를 사람이 읽을 수 있는
 * 이름으로 되돌리는 일**이다. 하드웨어는 인터럽트 ID 하나만
 * 주는데, 그 안에서 larb 번호와 포트 번호를 뽑는 방법이
 * SoC마다 네 가지나 되고(서브커먼 2비트/3비트/없음/포트 6비트),
 * 뽑아낸 larb 번호마저 배선 때문에 디바이스 트리의 번호와 달라
 * larbid_remap 표를 한 번 더 거쳐야 한다.
 *
 * 34비트 IOVA를 다루는 부분도 눈여겨볼 만하다. 폴트 주소 레지스터가
 * 32비트라, 페이지 오프셋이 필요 없는 하위 비트 자리에 가상 주소와
 * 물리 주소의 상위 비트를 함께 실어 보낸다.
 *
 * 마지막에 전체 무효화를 하는 이유: 폴트를 낸 잘못된 항목이
 * TLB에 남아 있으면 재시도가 또 실패한다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [mtk_iommu_isr] → report_iommu_fault()
 */
static irqreturn_t mtk_iommu_isr(int irq, void *dev_id)
{
	/* [한국어] 폴트를 낸 뱅크. */
	struct mtk_iommu_bank_data *bank = dev_id;
	/* [한국어] 그 뱅크가 속한 M4U. */
	struct mtk_iommu_data *data = bank->parent_data;
	/* [한국어] 그 뱅크에 붙어 있는 도메인(없을 수 있다). */
	struct mtk_iommu_domain *dom = bank->m4u_dom;
	/* [한국어] 폴트를 낸 마스터의 위치. 찾지 못하면 무효 표식으로 남는다. */
	unsigned int fault_larb = MTK_INVALID_LARBID, fault_port = 0, sub_comm = 0;
	/* [한국어] 폴트 상태, 인터럽트 ID, 그리고 주소의 상위 비트들. */
	u32 int_state, regval, va34_32, pa34_32;
	/* [한국어] 이 SoC의 설정 표. */
	const struct mtk_iommu_plat_data *plat_data = data->plat_data;
	/* [한국어] 이 뱅크의 레지스터 창. */
	void __iomem *base = bank->base;
	/* [한국어] 폴트가 난 가상 주소와, 하드웨어가 만들어 낸 물리 주소. */
	u64 fault_iova, fault_pa;
	/* [한국어] 몇 단계에서 났는가, 그리고 쓰기였는가. */
	bool layer, write;

	/* Read error info from registers */
	/* [한국어] 두 변환 유닛 중 어느 쪽이 폴트를 냈는지 보고,
	 * 그쪽의 레지스터에서 정보를 읽는다. */
	int_state = readl_relaxed(base + REG_MMU_FAULT_ST1);
	if (int_state & F_REG_MMU0_FAULT_MASK) {	/* [한국어] 0번 변환 유닛의 폴트 비트가 서 있는가. */
		regval = readl_relaxed(base + REG_MMU0_INT_ID);	/* [한국어] 그 유닛의 인터럽트 ID를 읽는다. */
		fault_iova = readl_relaxed(base + REG_MMU0_FAULT_VA);	/* [한국어] 그 유닛의 폴트 주소를 읽는다. */
		fault_pa = readl_relaxed(base + REG_MMU0_INVLD_PA);	/* [한국어] 그 유닛이 만들어 낸 물리 주소를 읽는다. */
	} else {
		regval = readl_relaxed(base + REG_MMU1_INT_ID);	/* [한국어] 1번 유닛의 인터럽트 ID를 읽는다. */
		fault_iova = readl_relaxed(base + REG_MMU1_FAULT_VA);	/* [한국어] 1번 유닛의 폴트 주소를 읽는다. */
		fault_pa = readl_relaxed(base + REG_MMU1_INVLD_PA);	/* [한국어] 1번 유닛의 물리 주소를 읽는다. */
	}
	/* [한국어] 주소의 하위 두 비트가 방향과 단계를 알려 준다 —
	 * 페이지 오프셋이 필요 없는 자리를 그렇게 쓴다. */
	layer = fault_iova & F_MMU_FAULT_VA_LAYER_BIT;
	write = fault_iova & F_MMU_FAULT_VA_WRITE_BIT;
	/* [한국어] 34비트 IOVA라면 상위 3비트가 하위 자리에 실려 온다 —
	 * 그것을 뽑아 제자리로 올린다. */
	if (MTK_IOMMU_HAS_FLAG(plat_data, IOVA_34_EN)) {
		va34_32 = FIELD_GET(F_MMU_INVAL_VA_34_32_MASK, fault_iova);	/* [한국어] 하위 자리에 실려 온 가상 주소의 상위 3비트를 뽑는다. */
		fault_iova = fault_iova & F_MMU_INVAL_VA_31_12_MASK;	/* [한국어] 주소 부분만 남긴다. */
		fault_iova |= (u64)va34_32 << 32;	/* [한국어] 뽑아 둔 상위 비트를 제자리로 올린다. */
	}
	/* [한국어] 물리 주소의 상위 비트도 같은 레지스터에 실려 있다.
	 * IOVA_34_EN과 무관하게 항상 뽑는 점에 유의 — 물리 주소는
	 * 35비트까지 갈 수 있기 때문이다. */
	pa34_32 = FIELD_GET(F_MMU_INVAL_PA_34_32_MASK, fault_iova);
	fault_pa |= (u64)pa34_32 << 32;

	/* [한국어] 멀티미디어 IOMMU에서만 larb/포트를 되짚을 수 있다.
	 * INFRA나 APU는 larb 개념이 없다. */
	if (MTK_IOMMU_IS_TYPE(plat_data, MTK_IOMMU_TYPE_MM)) {
		/* [한국어] 인터럽트 ID의 비트 배치가 SoC마다 넷으로 갈린다. */
		if (MTK_IOMMU_HAS_FLAG(plat_data, HAS_SUB_COMM_2BITS)) {
			fault_larb = F_MMU_INT_ID_COMM_ID(regval);	/* [한국어] 식별자에서 larb(공통) 번호를 뽑는다. */
			sub_comm = F_MMU_INT_ID_SUB_COMM_ID(regval);	/* [한국어] 서브커먼 번호(2비트)를 뽑는다. */
			fault_port = F_MMU_INT_ID_PORT_ID(regval);	/* [한국어] 포트 번호를 뽑는다. */
		} else if (MTK_IOMMU_HAS_FLAG(plat_data, HAS_SUB_COMM_3BITS)) {
			/* [한국어] 서브커먼이 3비트면 larb 번호가 한 칸 위로 밀린다. */
			fault_larb = F_MMU_INT_ID_COMM_ID_EXT(regval);
			sub_comm = F_MMU_INT_ID_SUB_COMM_ID_EXT(regval);	/* [한국어] 서브커먼 번호(3비트)를 뽑는다. */
			fault_port = F_MMU_INT_ID_PORT_ID(regval);	/* [한국어] 포트 번호를 뽑는다. */
		} else if (MTK_IOMMU_HAS_FLAG(plat_data, INT_ID_PORT_WIDTH_6)) {
			/* [한국어] 포트가 6비트인 구성 — larb 자리도 함께 밀린다. */
			fault_port = F_MMU_INT_ID_PORT_ID_WID_6(regval);
			fault_larb = F_MMU_INT_ID_LARB_ID_WID_6(regval);	/* [한국어] 포트가 6비트인 구성의 larb 번호. */
		} else {
			/* [한국어] 서브커먼이 없는 가장 단순한 구성. */
			fault_port = F_MMU_INT_ID_PORT_ID(regval);
			fault_larb = F_MMU_INT_ID_LARB_ID(regval);	/* [한국어] 서브커먼이 없는 구성의 larb 번호. */
		}
		/* [한국어] 하드웨어가 준 (공통, 서브커먼) 쌍을 디바이스
		 * 트리의 larb 번호로 옮긴다 — SMI 배선이 SoC마다 달라
		 * 이 표 없이는 사람이 읽을 수 없는 번호가 나온다. */
		fault_larb = data->plat_data->larbid_remap[fault_larb][sub_comm];
	}

	/* [한국어] 도메인이 없거나 상위 핸들러가 처리하지 못하면
	 * 진단 정보를 직접 남긴다. larb와 포트가 함께 찍혀
	 * 어느 마스터가 잘못했는지 알 수 있다. */
	if (!dom || report_iommu_fault(&dom->domain, bank->parent_dev, fault_iova,
			       write ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ)) {
		dev_err_ratelimited(	/* [한국어] 폴트를 쏟아내는 마스터가 로그를 채우지 않도록 제한한다. */
			bank->parent_dev,
			"fault type=0x%x iova=0x%llx pa=0x%llx master=0x%x(larb=%d port=%d) layer=%d %s\n",
			int_state, fault_iova, fault_pa, regval, fault_larb, fault_port,
			layer, str_write_read(write));
	}

	/* Interrupt clear */
	/* [한국어] 지우기 비트를 세워 밀린 인터럽트를 없앤다. 기존
	 * 값을 읽어 얹는 이유는 활성화 비트들을 보존하기 위함이다. */
	regval = readl_relaxed(base + REG_MMU_INT_CONTROL0);
	regval |= F_INT_CLR_BIT;	/* [한국어] 지우기 비트를 얹는다 — 다른 활성화 비트는 보존한다. */
	writel_relaxed(regval, base + REG_MMU_INT_CONTROL0);

	/* [한국어] 폴트를 낸 항목이 TLB에 남아 있으면 재시도가 또
	 * 실패한다 — 전체를 비워 그것을 없앤다. */
	mtk_iommu_tlb_flush_all(data);

	return IRQ_HANDLED;	/* [한국어] 폴트를 처리했음을 커널에 알린다. */
}

/*
 * [한국어]
 * mtk_iommu_get_bank_id - 이 마스터가 쓸 뱅크 번호를 찾는다
 *
 * @dev: 마스터 디바이스.
 * @plat_data: 이 SoC의 설정 표.
 * @return: 뱅크 번호(찾지 못하면 0).
 *
 * 마스터의 모든 포트를 비트맵으로 모은 뒤, 뱅크마다 정의된
 * 포트 비트맵과 겹치는지 본다. 겹치는 첫 뱅크가 답이다.
 *
 * 뱅크가 하나뿐인 SoC에서는 곧바로 0을 돌려준다 — 대부분의
 * SoC가 그렇다.
 *
 * 실행 컨텍스트: attach와 그룹 결정.
 *
 * 호출 체인:
 *   attach_device() / get_group_id() → [mtk_iommu_get_bank_id]
 */
static unsigned int mtk_iommu_get_bank_id(struct device *dev,
					  const struct mtk_iommu_plat_data *plat_data)
{
	/* [한국어] 이 마스터의 포트 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 순회 인덱스, 포트 비트맵, 찾은 뱅크 번호. */
	unsigned int i, portmsk = 0, bankid = 0;

	/* [한국어] 뱅크가 하나면 고를 것이 없다. */
	if (plat_data->banks_num == 1)
		return bankid;

	/* [한국어] 이 마스터가 쓰는 포트들을 비트맵으로 모은다. */
	for (i = 0; i < fwspec->num_ids; i++)
		portmsk |= BIT(MTK_M4U_TO_PORT(fwspec->ids[i]));

	/* [한국어] 활성화된 뱅크 중 이 포트들을 담당하는 것을 찾는다. */
	for (i = 0; i < plat_data->banks_num && i < MTK_IOMMU_BANK_MAX; i++) {
		if (!plat_data->banks_enable[i])	/* [한국어] 쓰지 않는 뱅크는 건너뛴다. */
			continue;

		/* [한국어] 하나라도 겹치면 그 뱅크가 담당이다. */
		if (portmsk & plat_data->banks_portmsk[i]) {
			bankid = i;	/* [한국어] 이 뱅크가 담당이다. */
			break;
		}
	}
	return bankid; /* default is 0 */	/* [한국어] 찾지 못했으면 기본 뱅크(0)를 쓴다. */
}

/*
 * [한국어]
 * mtk_iommu_get_iova_region_id - 이 마스터가 쓸 IOVA 영역을 찾는다
 *
 * @dev: 마스터 디바이스.
 * @plat_data: 이 SoC의 설정 표.
 * @return: 영역 번호, 찾지 못하면 -EINVAL.
 *
 * 뱅크 찾기와 비슷하지만 **판정이 더 엄격하다.** 겹치기만 하면
 * 되는 뱅크와 달리, 이 마스터의 모든 포트가 그 영역에 속해야
 * 한다((표 & 포트) == 포트). 한 마스터의 포트들이 서로 다른
 * 영역에 흩어지면 도메인을 하나로 잡을 수 없기 때문이다.
 *
 * larb 번호는 첫 포트의 것만 쓴다 — 한 마스터의 포트는 모두
 * 같은 larb에 있다는 전제다.
 *
 * 실행 컨텍스트: attach, 그룹 결정, 예약 영역 조회.
 *
 * 호출 체인:
 *   attach_device() / get_group_id() / get_resv_regions()
 *   → [mtk_iommu_get_iova_region_id]
 */
static int mtk_iommu_get_iova_region_id(struct device *dev,
					const struct mtk_iommu_plat_data *plat_data)
{
	/* [한국어] 이 마스터의 포트 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 포트 비트맵과 larb 번호. */
	unsigned int portidmsk = 0, larbid;
	/* [한국어] 지금 검사 중인 영역의 larb별 포트 표. */
	const u32 *rgn_larb_msk;
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 영역이 하나면 고를 것이 없다. */
	if (plat_data->iova_region_nr == 1)
		return 0;

	/* [한국어] 한 마스터의 포트는 모두 같은 larb에 있다는 전제로
	 * 첫 포트의 larb 번호만 쓴다. */
	larbid = MTK_M4U_TO_LARB(fwspec->ids[0]);
	for (i = 0; i < fwspec->num_ids; i++)	/* [한국어] 이 마스터가 쓰는 포트들을 비트맵으로 모은다. */
		portidmsk |= BIT(MTK_M4U_TO_PORT(fwspec->ids[i]));

	/* [한국어] 영역마다 이 마스터를 담을 수 있는지 확인한다. */
	for (i = 0; i < plat_data->iova_region_nr; i++) {
		rgn_larb_msk = plat_data->iova_region_larb_msk[i];
		/* [한국어] 이 영역에 배정된 마스터가 없다. */
		if (!rgn_larb_msk)
			continue;

		/* [한국어] **모든** 포트가 이 영역에 속해야 한다 —
		 * 일부만 겹치면 도메인을 하나로 잡을 수 없다. */
		if ((rgn_larb_msk[larbid] & portidmsk) == portidmsk)
			return i;
	}

	/* [한국어] 어느 영역에도 배정되지 않은 마스터다 — 설정 표가
	 * 이 마스터를 빠뜨렸다는 뜻이다. */
	dev_err(dev, "Can NOT find the region for larb(%d-%x).\n",
		larbid, portidmsk);
	return -EINVAL;	/* [한국어] 어느 영역에도 배정되지 않은 마스터다. */
}

/*
 * [한국어]
 * mtk_iommu_config - 마스터의 포트에서 IOMMU 사용을 켜거나 끈다
 *
 * @data: 대상 M4U.
 * @dev: 마스터 디바이스.
 * @enable: 켤 것인가.
 * @regionid: 이 마스터가 쓸 IOVA 영역.
 * @return: 0 성공, 음수 오류.
 *
 * **이 드라이버가 마스터를 IOMMU에 잇는 유일한 지점**이며,
 * 그 방법이 IOMMU 종류마다 완전히 다르다.
 *
 *  - 멀티미디어: M4U에는 스위치가 없다. SMI larb의 구조체에
 *    포트 비트를 세우면 SMI 드라이버가 실제 레지스터에 쓴다.
 *    함께 설정하는 larb_mmu->bank[port]는 그 포트가 쓸 IOVA
 *    영역의 상위 32비트로, larb가 주소에 얹어 준다 — 그래서
 *    마스터는 4GB 안의 주소만 내보내도 16GB 공간을 쓸 수 있다.
 *  - INFRA: PERICFG 레지스터의 비트를 직접 켜거나, 그것마저
 *    보안 영역이면 ATF에 SMC로 요청한다.
 *  - PCIe: 출력 ID가 하나뿐이라, 쓰기용 비트를 하나 더 켜 준다.
 *
 * 실행 컨텍스트: attach와 detach. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   attach_device() / identity_attach() → [mtk_iommu_config]
 */
static int mtk_iommu_config(struct mtk_iommu_data *data, struct device *dev,
			    bool enable, unsigned int regionid)
{
	/* [한국어] SMI 드라이버와 공유하는 larb 설정. */
	struct mtk_smi_larb_iommu    *larb_mmu;
	/* [한국어] larb 번호와 포트 번호. */
	unsigned int                 larbid, portid;
	/* [한국어] 이 마스터의 포트 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 이 마스터가 쓸 IOVA 영역. */
	const struct mtk_iommu_iova_region *region;
	/* [한국어] 포트들의 비트맵. */
	unsigned long portid_msk = 0;
	/* [한국어] ATF 호출의 결과. */
	struct arm_smccc_res res;
	/* [한국어] 순회 인덱스와 결과 코드. */
	int i, ret = 0;

	/* [한국어] 이 마스터가 쓰는 포트들을 비트맵으로 모은다. */
	for (i = 0; i < fwspec->num_ids; ++i) {
		portid = MTK_M4U_TO_PORT(fwspec->ids[i]);	/* [한국어] 이 ID에서 포트 번호를 뽑는다. */
		portid_msk |= BIT(portid);	/* [한국어] 포트 비트맵에 더한다. */
	}

	/* [한국어] 멀티미디어 — larb를 통해 켠다. */
	if (MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_MM)) {
		/* All ports should be in the same larb. just use 0 here */
		/* [한국어] 포트가 모두 같은 larb에 있다는 전제다. */
		larbid = MTK_M4U_TO_LARB(fwspec->ids[0]);
		larb_mmu = &data->larb_imu[larbid];	/* [한국어] 그 larb의 공유 설정 구조체를 잡는다. */
		region = data->plat_data->iova_region + regionid;

		/* [한국어] 포트마다 "이 영역의 상위 32비트"를 알려 준다.
		 * larb가 마스터의 주소에 이 값을 얹어 주므로, 마스터는
		 * 4GB 안의 주소만 내도 16GB 공간의 자기 영역을 쓴다. */
		for_each_set_bit(portid, &portid_msk, 32)
			larb_mmu->bank[portid] = upper_32_bits(region->iova_base);

		dev_dbg(dev, "%s iommu for larb(%s) port 0x%lx region %d rgn-bank %d.\n",	/* [한국어] 어느 포트를 어느 영역으로 설정했는지 남긴다. */
			str_enable_disable(enable), dev_name(larb_mmu->dev),
			portid_msk, regionid, upper_32_bits(region->iova_base));

		/* [한국어] 공유 구조체의 비트를 세우거나 지운다.
		 * 실제 레지스터 쓰기는 SMI 드라이버가 한다. */
		if (enable)
			larb_mmu->mmu |= portid_msk;
		else
			larb_mmu->mmu &= ~portid_msk;
	/* [한국어] 인프라 — larb가 없어 다른 통로를 쓴다. */
	} else if (MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_INFRA)) {
		/* [한국어] 설정 레지스터가 보안 영역에 있어 커널이 쓸 수
		 * 없는 하드웨어 — 보안 모니터에 대신 요청한다. */
		if (MTK_IOMMU_HAS_FLAG(data->plat_data, CFG_IFA_MASTER_IN_ATF)) {
			arm_smccc_smc(MTK_SIP_KERNEL_IOMMU_CONTROL,	/* [한국어] 보안 모니터에 포트 설정을 요청한다. */
				      IOMMU_ATF_CMD_CONFIG_INFRA_IOMMU,
				      portid_msk, enable, 0, 0, 0, 0, &res);
			ret = res.a0;	/* [한국어] SMC의 첫 반환값이 결과 코드다. */
		} else {
			/* PCI dev has only one output id, enable the next writing bit for PCIe */
			/* [한국어] PCIe는 읽기와 쓰기가 서로 다른 비트를
			 * 쓰는데 출력 ID는 하나뿐이라, 다음 비트를 함께 켠다. */
			if (dev_is_pci(dev)) {
				if (fwspec->num_ids != 1) {	/* [한국어] PCIe는 출력 ID가 하나뿐이어야 한다. */
					dev_err(dev, "PCI dev can only have one port.\n");	/* [한국어] 그렇지 않으면 이 처리를 적용할 수 없다. */
					return -ENODEV;	/* [한국어] 구성이 잘못됐음을 알린다. */
				}
				portid_msk |= BIT(portid + 1);	/* [한국어] 읽기 다음 비트가 쓰기용이라 함께 켠다. */
			}

			/* [한국어] PERICFG의 해당 비트들만 원자적으로 고친다. */
			ret = regmap_update_bits(data->pericfg, PERICFG_IOMMU_1,
						 (u32)portid_msk, enable ? (u32)portid_msk : 0);
		}
		if (ret)	/* [한국어] 포트 설정이 실패한 경우. */
			dev_err(dev, "%s iommu(%s) inframaster 0x%lx fail(%d).\n",
				str_enable_disable(enable), dev_name(data->dev),
				portid_msk, ret);
	}
	return ret;	/* [한국어] 설정 결과를 호출자에게 전한다. */
}

/*
 * [한국어]
 * mtk_iommu_domain_finalise - 도메인을 완성한다(테이블과 IOVA 범위)
 *
 * @dom: 완성할 도메인.
 * @data: 기준이 되는 M4U(공유하는 경우 목록의 첫 번째).
 * @region_id: 이 도메인이 쓸 IOVA 영역.
 * @return: 0 성공, -ENOMEM.
 *
 * **테이블을 공유하는 것이 기본 동작**이다. 이미 만들어진 대표
 * 도메인(share_dom)이 있으면 그 테이블 연산과 설정을 통째로
 * 복사해 오고, 영역 범위만 자기 것으로 정한다. 그래서 IOVA 영역이
 * 여섯 개여도 실제 페이지 테이블은 하나뿐이다.
 *
 * 처음이라면 io-pgtable을 만든다. quirk 셋이 이 하드웨어의 성격을
 * 말해 준다: ARM_NS는 비보안 표시를 엔트리에 넣고, NO_PERMS는
 * 권한 비트를 쓰지 않으며(이 IOMMU에는 권한 개념이 없다),
 * ARM_MTK_EXT는 v7 짧은 서술자의 예약 비트를 상위 물리 주소로
 * 전용한다 — 그 덕분에 32비트 엔트리로 35비트 주소를 가리킨다.
 *
 * oas가 세 갈래인 것도 눈여겨볼 만하다. 4GB 모드에서는 주소가
 * 4GB 위로 재배치되므로 33비트가 필요하고, 그 모드가 없는
 * 최신 하드웨어는 35비트를 그대로 쓴다.
 *
 * 실행 컨텍스트: attach 경로. 도메인과 데이터의 뮤텍스를 잡은 상태.
 *
 * 호출 체인:
 *   mtk_iommu_attach_device() → [mtk_iommu_domain_finalise]
 *   → alloc_io_pgtable_ops()
 */
static int mtk_iommu_domain_finalise(struct mtk_iommu_domain *dom,
				     struct mtk_iommu_data *data,
				     unsigned int region_id)
{
	/* [한국어] 이미 테이블을 만들어 둔 대표 도메인(없으면 NULL). */
	struct mtk_iommu_domain	*share_dom = data->share_dom;
	/* [한국어] 이 도메인이 쓸 IOVA 영역. */
	const struct mtk_iommu_iova_region *region;

	/* Share pgtable when 2 MM IOMMU share the pgtable or one IOMMU use multiple iova ranges */
	/* [한국어] 대표 도메인이 있으면 테이블을 그대로 가져다 쓴다.
	 * 같은 iop 포인터를 갖는 것이 곧 테이블 공유의 실체다. */
	if (share_dom) {
		dom->iop = share_dom->iop;	/* [한국어] 같은 테이블 연산을 가리키게 한다 — 이것이 공유의 실체다. */
		dom->cfg = share_dom->cfg;	/* [한국어] 테이블 설정도 그대로 물려받는다. */
		dom->domain.pgsize_bitmap = share_dom->domain.pgsize_bitmap;	/* [한국어] 지원 페이지 크기도 같아야 한다. */
		goto update_iova_region;	/* [한국어] 테이블은 다 되었으니 영역 설정으로 건너뛴다. */
	}

	/* [한국어] 처음이라면 테이블을 만든다. */
	dom->cfg = (struct io_pgtable_cfg) {
		.quirks = IO_PGTABLE_QUIRK_ARM_NS |	/* [한국어] 이 하드웨어의 성격을 나타내는 세 가지 quirk. */
			IO_PGTABLE_QUIRK_NO_PERMS |
			IO_PGTABLE_QUIRK_ARM_MTK_EXT,
		/* [한국어] 비보안 표시를 넣고(ARM_NS), 권한 비트는 쓰지
		 * 않으며(이 IOMMU에는 권한 개념이 없다), v7 서술자의
		 * 예약 비트를 상위 물리 주소로 전용한다(MTK_EXT). */

		.pgsize_bitmap = dom->domain.pgsize_bitmap,
		/* [한국어] 생성 시 정해 둔 네 가지 크기(4KB~16MB). */

		.ias = MTK_IOMMU_HAS_FLAG(data->plat_data, IOVA_34_EN) ? 34 : 32,
		/* [한국어] IOVA 폭. 34비트면 16GB 공간을 영역으로 나눠 쓴다. */

		.iommu_dev = data->dev,
		/* [한국어] 테이블 페이지 할당의 기준 디바이스. */
	};

	/* [한국어] 테이블 **자체**가 35비트 주소에 놓일 수 있는
	 * 하드웨어라면 TTBR 확장 quirk를 더한다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, PGTABLE_PA_35_EN))
		dom->cfg.quirks |= IO_PGTABLE_QUIRK_ARM_MTK_TTBR_EXT;

	/* [한국어] 4GB 모드에서는 주소가 4GB 위로 재배치되므로
	 * 33비트가 필요하다. 그 모드가 아니면 32비트로 충분하다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_4GB_MODE))
		dom->cfg.oas = data->enable_4GB ? 33 : 32;
	else
		/* [한국어] 4GB 모드가 없는 최신 하드웨어는 35비트를 쓴다. */
		dom->cfg.oas = 35;

	/* [한국어] ARM v7 짧은 서술자 포맷으로 테이블을 만든다. */
	dom->iop = alloc_io_pgtable_ops(ARM_V7S, &dom->cfg, data);
	if (!dom->iop) {	/* [한국어] 페이지 테이블을 만들지 못했다. */
		dev_err(data->dev, "Failed to alloc io pgtable\n");	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
		return -ENOMEM;	/* [한국어] 도메인을 완성할 수 없다. */
	}

	/* [한국어] 이후 도메인들이 이 테이블을 가져다 쓰도록 등록한다. */
	data->share_dom = dom;

/* [한국어] 테이블을 만들었든 가져왔든, 영역 범위는 도메인마다 다르다. */
update_iova_region:
	/* Update the iova region for this domain */
	/* [한국어] 이 도메인이 담당할 IOVA 구간을 코어에 알린다.
	 * 테이블은 공유해도 범위가 달라 서로 겹치지 않는다. */
	region = data->plat_data->iova_region + region_id;
	dom->domain.geometry.aperture_start = region->iova_base;	/* [한국어] 이 도메인이 담당할 IOVA의 시작. */
	dom->domain.geometry.aperture_end = region->iova_base + region->size - 1;	/* [한국어] 그 끝(포함). */
	dom->domain.geometry.force_aperture = true;	/* [한국어] 코어가 이 범위를 벗어난 IOVA를 주지 않게 한다. */
	return 0;	/* [한국어] 도메인 완성이 끝났다. */
}

/*
 * [한국어]
 * mtk_iommu_domain_alloc_paging - 페이징 도메인의 껍데기를 만든다
 *
 * @dev: 이 도메인을 쓸 디바이스(이 드라이버는 쓰지 않는다).
 * @return: 새 도메인, 실패하면 NULL.
 *
 * 페이지 크기만 정해 두고 나머지는 attach로 미룬다. 어느 IOVA
 * 영역을 쓸지, 어느 뱅크에 붙을지가 디바이스를 알아야 정해지기
 * 때문이다.
 *
 * 네 가지 크기는 ARM v7 짧은 서술자가 정의하는 그대로다 —
 * 4KB 작은 페이지, 64KB 큰 페이지, 1MB 섹션, 16MB 슈퍼섹션.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [mtk_iommu_domain_alloc_paging]
 */
static struct iommu_domain *mtk_iommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 만들 도메인. */
	struct mtk_iommu_domain *dom;

	/* [한국어] 0으로 초기화해 받는다 — bank가 NULL이어야
	 * attach가 "아직 완성되지 않았다"고 판단한다. */
	dom = kzalloc_obj(*dom);
	if (!dom)	/* [한국어] 도메인 껍데기를 잡지 못했다. */
		return NULL;
	/* [한국어] 완성 과정을 보호할 뮤텍스. */
	mutex_init(&dom->mutex);
	/* [한국어] ARM v7 짧은 서술자의 네 가지 크기. */
	dom->domain.pgsize_bitmap = SZ_4K | SZ_64K | SZ_1M | SZ_16M;

	return &dom->domain;	/* [한국어] 코어에는 임베드된 부분만 돌려준다. */
}

/*
 * [한국어]
 * mtk_iommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 페이지 테이블을 해제하지 않는 점에 유의. **테이블이 공유되기
 * 때문**이다 — 이 도메인이 사라져도 다른 도메인이 같은 iop를
 * 쓰고 있을 수 있다. 그래서 테이블은 하드웨어 제거까지 살아남는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 free → [mtk_iommu_domain_free]
 */
static void mtk_iommu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 테이블은 공유되므로 건드리지 않고 껍데기만 해제한다. */
	kfree(to_mtk_domain(domain));
}

/*
 * [한국어]
 * mtk_iommu_attach_device - 마스터를 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 마스터 디바이스.
 * @old: 직전 도메인(쓰지 않는다).
 * @return: 0 성공, 음수 오류.
 *
 * 세 단계가 각각 다른 락 아래에서 이뤄진다.
 *
 *  1) **도메인 완성**(도메인 뮤텍스). 아직 뱅크가 없으면 테이블을
 *     만들거나 공유해 오고, 이 도메인이 붙을 뱅크를 정한다.
 *     공유 정보는 목록의 첫 하드웨어에 있으므로 그쪽 뮤텍스도
 *     함께 잡는다.
 *  2) **하드웨어 초기화**(데이터 뮤텍스). 그 뱅크에 처음 붙는
 *     도메인이라면 레지스터를 설정하고 테이블 주소를 심는다.
 *     전원을 켠 채로 해야 하므로 pm_runtime을 감싼다.
 *  3) **포트 켜기**(락 없음). larb나 PERICFG에 비트를 세운다.
 *
 * 34비트 IOVA를 쓰는 영역(region_id > 0)에 붙는 마스터는 DMA
 * 마스크도 넓혀야 한다 — 그러지 않으면 DMA 계층이 4GB 위의
 * IOVA를 주지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [mtk_iommu_attach_device]
 *   → mtk_iommu_domain_finalise() → mtk_iommu_hw_init()
 *   → mtk_iommu_config()
 */
static int mtk_iommu_attach_device(struct iommu_domain *domain,
				   struct device *dev, struct iommu_domain *old)
{
	/* [한국어] 이 마스터의 M4U와, 공유 정보를 가진 첫 하드웨어. */
	struct mtk_iommu_data *data = dev_iommu_priv_get(dev), *frstdata;
	/* [한국어] 붙일 도메인. */
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	/* [한국어] 테이블을 공유하는 하드웨어들의 목록. */
	struct list_head *hw_list = data->hw_list;
	/* [한국어] 전원을 켜고 끌 M4U 디바이스. */
	struct device *m4udev = data->dev;
	/* [한국어] 이 마스터가 붙을 뱅크. */
	struct mtk_iommu_bank_data *bank;
	/* [한국어] 그 뱅크의 번호. */
	unsigned int bankid;
	/* [한국어] 결과 코드와 IOVA 영역 번호. */
	int ret, region_id;

	/* [한국어] 이 마스터가 쓸 IOVA 영역을 먼저 정한다. */
	region_id = mtk_iommu_get_iova_region_id(dev, data->plat_data);
	if (region_id < 0)	/* [한국어] 영역을 찾지 못했다 — 붙일 수 없는 마스터다. */
		return region_id;

	/* [한국어] 붙을 뱅크도 정한다. */
	bankid = mtk_iommu_get_bank_id(dev, data->plat_data);
	mutex_lock(&dom->mutex);
	/* [한국어] 뱅크가 비어 있으면 아직 완성되지 않은 도메인이다. */
	if (!dom->bank) {
		/* Data is in the frstdata in sharing pgtable case. */
		/* [한국어] 공유 테이블과 대표 도메인은 첫 하드웨어에 있다. */
		frstdata = mtk_iommu_get_frst_data(hw_list);

		/* [한국어] 그쪽의 share_dom을 읽고 쓰므로 뮤텍스가 필요하다. */
		mutex_lock(&frstdata->mutex);
		ret = mtk_iommu_domain_finalise(dom, frstdata, region_id);	/* [한국어] 테이블을 만들거나 공유해 오고 영역 범위를 정한다. */
		mutex_unlock(&frstdata->mutex);	/* [한국어] 공유 정보 조작이 끝났으니 뮤텍스를 놓는다. */
		if (ret) {	/* [한국어] 도메인 완성이 실패한 경우. */
			mutex_unlock(&dom->mutex);	/* [한국어] 도메인 뮤텍스도 놓는다. */
			return ret;	/* [한국어] 실패 이유를 코어에 전한다. */
		}
		/* [한국어] 이 값이 채워지면 완성된 도메인이다. */
		dom->bank = &data->bank[bankid];
	}
	mutex_unlock(&dom->mutex);

	/* [한국어] 뱅크의 하드웨어 초기화는 이쪽 뮤텍스가 보호한다. */
	mutex_lock(&data->mutex);
	bank = &data->bank[bankid];	/* [한국어] 이 마스터가 붙을 뱅크. */
	if (!bank->m4u_dom) { /* Initialize the M4U HW for each a BANK */
		/* [한국어] 레지스터를 만지려면 전원을 켜야 한다. */
		ret = pm_runtime_resume_and_get(m4udev);
		if (ret < 0) {	/* [한국어] 전원을 켜지 못한 경우. */
			dev_err(m4udev, "pm get fail(%d) in attach.\n", ret);	/* [한국어] 레지스터를 만질 수 없음을 알린다. */
			goto err_unlock;	/* [한국어] 데이터 뮤텍스를 놓고 나간다. */
		}

		/* [한국어] 이 뱅크의 레지스터를 설정하고 인터럽트를 건다. */
		ret = mtk_iommu_hw_init(data, bankid);
		if (ret) {	/* [한국어] 하드웨어 초기화가 실패한 경우. */
			pm_runtime_put(m4udev);	/* [한국어] 올려 둔 전원 참조를 놓는다. */
			goto err_unlock;	/* [한국어] 데이터 뮤텍스를 놓고 나간다. */
		}
		/* [한국어] 이 값이 채워지면 이 뱅크는 초기화된 것이고,
		 * 리줌도 이 값을 보고 복원 여부를 정한다. */
		bank->m4u_dom = dom;
		/* [한국어] 테이블 주소를 하드웨어에 심는다. relaxed가
		 * 아닌 writel을 쓰는 것은 앞선 설정이 모두 반영된 뒤에
		 * 변환이 시작되게 하려는 것이다. */
		writel(dom->cfg.arm_v7s_cfg.ttbr, bank->base + REG_MMU_PT_BASE_ADDR);

		pm_runtime_put(m4udev);	/* [한국어] 설정이 끝났으니 전원 참조를 놓는다. */
	}
	mutex_unlock(&data->mutex);

	/* [한국어] 0번이 아닌 영역은 4GB 위에 있으므로, 이 마스터의
	 * DMA 마스크를 넓혀야 그 IOVA를 받을 수 있다. */
	if (region_id > 0) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));	/* [한국어] 34비트 IOVA를 받을 수 있도록 마스크를 넓힌다. */
		if (ret) {	/* [한국어] 마스크 설정이 거부된 경우. */
			dev_err(m4udev, "Failed to set dma_mask for %s(%d).\n", dev_name(dev), ret);	/* [한국어] 어느 디바이스에서 막혔는지 남긴다. */
			return ret;	/* [한국어] 이 마스터는 그 영역을 쓸 수 없다. */
		}
	}

	/* [한국어] 마지막으로 larb(또는 PERICFG)에서 이 마스터의
	 * 포트를 켠다 — 이 순간부터 DMA가 IOMMU를 거친다. */
	return mtk_iommu_config(data, dev, true, region_id);

/* [한국어] 하드웨어 초기화 실패 — 데이터 뮤텍스를 놓고 나간다. */
err_unlock:
	mutex_unlock(&data->mutex);	/* [한국어] 실패 경로에서도 뮤텍스를 놓는다. */
	return ret;	/* [한국어] 실패 이유를 코어에 전한다. */
}

/*
 * [한국어]
 * mtk_iommu_identity_attach - 마스터의 포트를 꺼 IOMMU를 우회시킨다
 *
 * @identity_domain: 정적 identity 도메인.
 * @dev: 마스터 디바이스.
 * @old: 직전 도메인.
 * @return: 항상 0.
 *
 * 포트를 끄면 그 마스터의 DMA가 IOMMU를 거치지 않고 나간다 —
 * 그것이 이 하드웨어의 통과 모드다. 하드웨어 쪽에 손댈 것이
 * 없어 larb의 비트 하나면 끝난다.
 *
 * 영역 번호로 0을 넘기는 점에 유의: 끄는 경우에는 larb의 뱅크
 * 값이 쓰이지 않으므로 아무 값이나 무해하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) → [mtk_iommu_identity_attach]
 *   → mtk_iommu_config()
 */
static int mtk_iommu_identity_attach(struct iommu_domain *identity_domain,
				     struct device *dev,
				     struct iommu_domain *old)
{
	/* [한국어] 이 마스터의 M4U. */
	struct mtk_iommu_data *data = dev_iommu_priv_get(dev);

	/* [한국어] 이미 우회 상태거나 붙은 적이 없으면 할 일이 없다. */
	if (old == identity_domain || !old)
		return 0;

	/* [한국어] 포트를 꺼 DMA가 IOMMU를 거치지 않게 한다.
	 * 끌 때는 영역 값이 쓰이지 않아 0으로 넘긴다. */
	mtk_iommu_config(data, dev, false, 0);
	return 0;	/* [한국어] 포트를 껐으니 이 마스터의 DMA는 IOMMU를 지나지 않는다. */
}

/* [한국어] identity 도메인의 연산 테이블. 포트를 끄는 콜백 하나뿐이다. */
static struct iommu_domain_ops mtk_iommu_identity_ops = {
	.attach_dev = mtk_iommu_identity_attach,
	/* [한국어] 이 도메인으로 옮길 때 부를 콜백. */
};

/* [한국어] 시스템 전체가 공유하는 정적 통과 도메인. */
static struct iommu_domain mtk_iommu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 코어에 통과 모드임을 알리는 표시. */

	.ops = &mtk_iommu_identity_ops,
	/* [한국어] 위의 연산 테이블. */
};

/*
 * [한국어]
 * mtk_iommu_map - IOVA에 물리 주소를 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 IOVA.
 * @paddr: 물리 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @prot: 권한(io-pgtable이 NO_PERMS quirk로 무시한다).
 * @gfp: 할당 플래그.
 * @mapped: 매핑된 바이트 수를 돌려줄 곳.
 * @return: 0 성공, 음수 오류.
 *
 * 하는 일은 4GB 모드 보정 하나와 위임뿐이다. 원본 주석이 밝히듯
 * 4GB 모드의 하드웨어는 재배치된 주소만 쓸 수 있으므로, CPU가
 * 준 주소에 비트 32를 얹어야 한다.
 *
 * 실행 컨텍스트: DMA 매핑 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 map_pages → [mtk_iommu_map] → io-pgtable
 */
static int mtk_iommu_map(struct iommu_domain *domain, unsigned long iova,
			 phys_addr_t paddr, size_t pgsize, size_t pgcount,
			 int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 대상 도메인. */
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);

	/* The "4GB mode" M4U physically can not use the lower remap of Dram. */
	/* [한국어] 4GB 모드에서는 메모리가 4GB 위로 재배치되어 있으므로,
	 * CPU 물리 주소에 비트 32를 얹어야 IOMMU가 쓸 주소가 된다. */
	if (dom->bank->parent_data->enable_4GB)
		paddr |= BIT_ULL(32);

	/* Synchronize with the tlb_lock */
	/* [한국어] 실제 테이블 조작은 io-pgtable이 한다. */
	return dom->iop->map_pages(dom->iop, iova, paddr, pgsize, pgcount, prot, gfp, mapped);
}

/*
 * [한국어]
 * mtk_iommu_unmap - IOVA 범위의 매핑을 해제한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 IOVA.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @gather: 무효화 범위를 모을 곳.
 * @return: 해제된 바이트 수.
 *
 * gather에 범위를 누적해 두고 실제 무효화는 iotlb_sync가 한다 —
 * 무효화가 폴링을 동반해 비싸기 때문에 묶는 것이 이득이다.
 *
 * 실행 컨텍스트: DMA 해제 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 unmap_pages → [mtk_iommu_unmap] → io-pgtable
 */
static size_t mtk_iommu_unmap(struct iommu_domain *domain,
			      unsigned long iova, size_t pgsize, size_t pgcount,
			      struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);

	/* [한국어] 무효화할 범위를 모아 둔다. 실제 명령은 나중에 나간다. */
	iommu_iotlb_gather_add_range(gather, iova, pgsize * pgcount);
	return dom->iop->unmap_pages(dom->iop, iova, pgsize, pgcount, gather);	/* [한국어] 실제 해제는 io-pgtable이 한다. */
}

/*
 * [한국어]
 * mtk_iommu_flush_iotlb_all - 이 도메인의 TLB를 통째로 비운다
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * bank가 NULL인지 확인하는 이유: 아직 아무도 붙지 않은 도메인에도
 * 코어가 이 콜백을 부를 수 있는데, 그때는 비울 하드웨어가 없다.
 *
 * 실행 컨텍스트: 전체 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 flush_iotlb_all → [mtk_iommu_flush_iotlb_all]
 */
static void mtk_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	/* [한국어] 대상 도메인. */
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);

	/* [한국어] 아직 완성되지 않은 도메인에는 비울 하드웨어가 없다. */
	if (dom->bank)
		mtk_iommu_tlb_flush_all(dom->bank->parent_data);
}

/*
 * [한국어]
 * mtk_iommu_iotlb_sync - 모아 둔 해제 범위를 무효화한다
 *
 * @domain: 대상 도메인.
 * @gather: 누적된 범위.
 * @return: 없음.
 *
 * gather의 시작과 끝으로 길이를 구해 범위 무효화를 낸다.
 * 양 끝을 포함하는 구간이라 +1이 붙는다.
 *
 * 실행 컨텍스트: 해제 후 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync → [mtk_iommu_iotlb_sync]
 */
static void mtk_iommu_iotlb_sync(struct iommu_domain *domain,
				 struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	/* [한국어] 모인 범위의 길이(양 끝 포함이라 +1). */
	size_t length = gather->end - gather->start + 1;

	/* [한국어] 목록의 모든 하드웨어에서 그 범위를 비운다. */
	mtk_iommu_tlb_flush_range_sync(gather->start, length, dom->bank);
}

/*
 * [한국어]
 * mtk_iommu_sync_map - 새로 만든 매핑을 하드웨어에 반영한다
 *
 * @domain: 대상 도메인.
 * @iova: 새 매핑의 시작.
 * @size: 크기.
 * @return: 항상 0.
 *
 * 매핑을 **추가**한 뒤에도 무효화가 필요한 이유: 이 하드웨어의
 * TLB는 "이 주소에 매핑이 없다"는 부정 결과도 캐시하므로,
 * 지워 주지 않으면 새 매핑이 보이지 않는다.
 *
 * 실행 컨텍스트: 매핑 직후.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync_map → [mtk_iommu_sync_map]
 */
static int mtk_iommu_sync_map(struct iommu_domain *domain, unsigned long iova,
			      size_t size)
{
	/* [한국어] 대상 도메인. */
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);

	/* [한국어] 부정 캐시가 새 매핑을 가리지 않도록 그 범위를 비운다. */
	mtk_iommu_tlb_flush_range_sync(iova, size, dom->bank);
	return 0;	/* [한국어] 무효화를 요청했으니 성공이다. */
}

/*
 * [한국어]
 * mtk_iommu_iova_to_phys - IOVA를 CPU 물리 주소로 변환한다
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: CPU 물리 주소, 매핑이 없으면 0.
 *
 * io-pgtable이 돌려주는 것은 **IOMMU가 보는 주소**이므로,
 * 4GB 모드라면 map이 얹었던 비트 32를 벗겨 CPU 물리 주소로
 * 되돌려야 한다.
 *
 * 재배치 기준과 비교하는 이유: 원래부터 4GB 위에 있던 영역(그림의 E)은
 * 재배치되지 않았으므로 비트를 벗기면 안 된다.
 *
 * 실행 컨텍스트: 조회 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 iova_to_phys → [mtk_iommu_iova_to_phys] → io-pgtable
 */
static phys_addr_t mtk_iommu_iova_to_phys(struct iommu_domain *domain,
					  dma_addr_t iova)
{
	/* [한국어] 대상 도메인. */
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	/* [한국어] io-pgtable이 돌려준 주소(IOMMU 관점). */
	phys_addr_t pa;

	pa = dom->iop->iova_to_phys(dom->iop, iova);
	/* [한국어] 4GB 모드에서 재배치된 영역만 비트 32를 벗긴다.
	 * 원래 4GB 위에 있던 영역은 그대로 두어야 한다. */
	if (IS_ENABLED(CONFIG_PHYS_ADDR_T_64BIT) &&
	    dom->bank->parent_data->enable_4GB &&
	    pa >= MTK_IOMMU_4GB_MODE_REMAP_BASE)
		pa &= ~BIT_ULL(32);

	return pa;	/* [한국어] 보정을 마친 CPU 물리 주소를 돌려준다. */
}

/*
 * [한국어]
 * mtk_iommu_probe_device - 마스터를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 iommu_device, 실패하면 ERR_PTR.
 *
 * 멀티미디어 IOMMU에서만 할 일이 있다: **마스터와 larb를
 * 전원 링크로 묶는 것**이다. 마스터가 DMA를 내려면 그 앞의
 * larb가 켜져 있어야 하기 때문이다.
 *
 * DL_WITH_MULTI_LARB 플래그가 없으면 마스터의 모든 포트가 같은
 * larb에 있어야 한다고 강제한다 — 그 전제가 mtk_iommu_config와
 * get_iova_region_id에서 첫 포트의 larb만 보는 근거다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [mtk_iommu_probe_device]
 */
static struct iommu_device *mtk_iommu_probe_device(struct device *dev)
{
	/* [한국어] 이 마스터의 포트 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] of_xlate가 매달아 둔 M4U 상태. */
	struct mtk_iommu_data *data = dev_iommu_priv_get(dev);
	/* [한국어] 만든 전원 링크. */
	struct device_link *link;
	/* [한국어] 링크를 걸 larb 디바이스. */
	struct device *larbdev;
	/* [한국어] 이 마스터가 걸쳐 있는 larb들의 비트맵. */
	unsigned long larbid_msk = 0;
	/* [한국어] larb 번호들과 순회 인덱스. */
	unsigned int larbid, larbidx, i;

	/* [한국어] larb가 없는 IOMMU(INFRA, APU)는 링크할 대상이 없다. */
	if (!MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_MM))
		return &data->iommu;

	/*
	 * Link the consumer device with the smi-larb device(supplier).
	 * w/DL_WITH_MULTI_LARB: the master may connect with multi larbs,
	 * we should create device link with each larb.
	 * w/o DL_WITH_MULTI_LARB: the master must connect with one larb,
	 * otherwise fail.
	 */
	/* [한국어] 첫 포트의 larb를 기준으로 삼는다. */
	larbid = MTK_M4U_TO_LARB(fwspec->ids[0]);
	if (larbid >= MTK_LARB_NR_MAX)	/* [한국어] 배열 범위를 벗어난 larb 번호다. */
		return ERR_PTR(-EINVAL);

	larbid_msk |= BIT(larbid);

	/* [한국어] 나머지 포트들의 larb를 확인한다. */
	for (i = 1; i < fwspec->num_ids; i++) {
		larbidx = MTK_M4U_TO_LARB(fwspec->ids[i]);
		/* [한국어] 여러 larb를 허용하는 하드웨어면 모두 모은다. */
		if (MTK_IOMMU_HAS_FLAG(data->plat_data, DL_WITH_MULTI_LARB)) {
			larbid_msk |= BIT(larbidx);	/* [한국어] 여러 larb를 허용하는 하드웨어이므로 함께 모은다. */
		} else if (larbid != larbidx) {
			/* [한국어] 그렇지 않으면 하나여야 한다 — 이 전제가
			 * 다른 함수들이 첫 포트의 larb만 보는 근거다. */
			dev_err(dev, "Can only use one larb. Fail@larb%d-%d.\n",
				larbid, larbidx);
			return ERR_PTR(-EINVAL);	/* [한국어] 하나의 larb만 허용하는 구성인데 둘 이상이다. */
		}
	}

	/* [한국어] 모은 larb마다 전원 링크를 만든다. 마스터가 깨어날 때
	 * 그 앞의 larb들이 먼저 깨어나게 하는 것이다. */
	for_each_set_bit(larbid, &larbid_msk, 32) {
		larbdev = data->larb_imu[larbid].dev;
		/* [한국어] component bind가 아직 채우지 않았다. */
		if (!larbdev)
			return ERR_PTR(-EINVAL);

		/* [한국어] STATELESS는 이 드라이버가 링크를 직접 관리하겠다는
		 * 뜻이라, release_device가 명시적으로 지운다. */
		link = device_link_add(dev, larbdev,
				       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
		if (!link) {	/* [한국어] 전원 링크를 만들지 못한 경우. */
			dev_err(dev, "Unable to link %s\n", dev_name(larbdev));	/* [한국어] 어느 larb와의 링크가 실패했는지 남긴다. */
			goto link_remove;	/* [한국어] 이미 만든 링크를 지우러 간다. */
		}
	}

	return &data->iommu;

/* [한국어] 도중에 실패했다 — 이미 만든 링크만 지운다.
 * 상한을 larbid로 두어, 실패한 그 larb 앞까지만 훑는다. */
link_remove:
	for_each_set_bit(i, &larbid_msk, larbid) {	/* [한국어] 실패한 larb 앞까지만 훑는다. */
		larbdev = data->larb_imu[i].dev;	/* [한국어] 지울 링크의 상대. */
		device_link_remove(dev, larbdev);	/* [한국어] 그 링크를 지운다. */
	}

	return ERR_PTR(-ENODEV);	/* [한국어] 등록에 실패했음을 코어에 알린다. */
}

/*
 * [한국어]
 * mtk_iommu_release_device - 마스터의 전원 링크를 걷어낸다
 *
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * probe_device가 만든 링크를 지운다. 여기서는 실제 포트 목록을
 * 다시 훑어 비트맵을 만드는데, probe 때와 달리 검증이 필요 없어
 * 코드가 짧다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 release_device → [mtk_iommu_release_device]
 */
static void mtk_iommu_release_device(struct device *dev)
{
	/* [한국어] 이 마스터의 포트 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 이 마스터의 M4U. */
	struct mtk_iommu_data *data;
	/* [한국어] 링크를 지울 larb 디바이스. */
	struct device *larbdev;
	/* [한국어] larb 번호와 순회 인덱스. */
	unsigned int larbid, i;
	/* [한국어] 이 마스터가 걸쳐 있는 larb들의 비트맵. */
	unsigned long larbid_msk = 0;

	data = dev_iommu_priv_get(dev);
	/* [한국어] larb가 없는 IOMMU는 지울 링크도 없다. */
	if (!MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_MM))
		return;

	/* [한국어] 이 마스터가 쓰는 larb들을 모은다. */
	for (i = 0; i < fwspec->num_ids; i++) {
		larbid = MTK_M4U_TO_LARB(fwspec->ids[i]);	/* [한국어] 이 ID에서 larb 번호를 뽑는다. */
		larbid_msk |= BIT(larbid);	/* [한국어] 비트맵에 더한다. */
	}

	/* [한국어] STATELESS로 만든 링크는 명시적으로 지워야 한다. */
	for_each_set_bit(larbid, &larbid_msk, 32) {
		larbdev = data->larb_imu[larbid].dev;	/* [한국어] 링크를 지울 larb 디바이스. */
		device_link_remove(dev, larbdev);	/* [한국어] 그 링크를 지운다. */
	}
}

/*
 * [한국어]
 * mtk_iommu_get_group_id - 이 마스터가 속할 그룹 번호를 정한다
 *
 * @dev: 마스터 디바이스.
 * @plat_data: 이 SoC의 설정 표.
 * @return: 그룹 번호, 실패하면 음수.
 *
 * 원본 주석이 정책을 밝힌다: **뱅크가 쓰이면 뱅크가 그룹이고,
 * 아니면 IOVA 영역이 그룹이다.** 뱅크는 테이블 자체가 나뉘므로
 * 더 강한 격리이고, 영역은 테이블을 공유하되 주소 범위로
 * 나누는 약한 격리다.
 *
 * 뱅크 번호가 0이면 뱅크 구분이 없다는 뜻이라 영역 쪽으로
 * 넘어간다 — 0번 뱅크는 기본값이기도 하기 때문이다.
 *
 * 실행 컨텍스트: 그룹 결정.
 *
 * 호출 체인:
 *   mtk_iommu_device_group() → [mtk_iommu_get_group_id]
 */
static int mtk_iommu_get_group_id(struct device *dev, const struct mtk_iommu_plat_data *plat_data)
{
	/* [한국어] 이 마스터가 붙을 뱅크. */
	unsigned int bankid;

	/*
	 * If the bank function is enabled, each bank is a iommu group/domain.
	 * Otherwise, each iova region is a iommu group/domain.
	 */
	/* [한국어] 뱅크가 나뉘어 있으면 그것이 곧 격리 단위다. */
	bankid = mtk_iommu_get_bank_id(dev, plat_data);
	if (bankid)	/* [한국어] 0이 아니면 뱅크가 격리 단위다. */
		return bankid;

	/* [한국어] 0번은 기본값이라 뱅크 구분이 없다는 뜻이다 —
	 * 그때는 IOVA 영역이 격리 단위가 된다. */
	return mtk_iommu_get_iova_region_id(dev, plat_data);
}

/*
 * [한국어]
 * mtk_iommu_device_group - 이 마스터가 속할 IOMMU 그룹을 정한다
 *
 * @dev: 마스터 디바이스.
 * @return: 그룹, 실패하면 ERR_PTR.
 *
 * 그룹 번호로 배열을 찾아, 이미 있으면 참조를 올리고 없으면
 * 새로 만든다. **배열이 첫 하드웨어에 있는 것**이 요점인데,
 * 테이블을 공유하는 하드웨어들이 같은 그룹을 써야 하기 때문이다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 device_group → [mtk_iommu_device_group]
 */
static struct iommu_group *mtk_iommu_device_group(struct device *dev)
{
	/* [한국어] 이 마스터의 M4U와, 공유 정보를 가진 첫 하드웨어. */
	struct mtk_iommu_data *c_data = dev_iommu_priv_get(dev), *data;
	/* [한국어] 테이블을 공유하는 하드웨어들의 목록. */
	struct list_head *hw_list = c_data->hw_list;
	/* [한국어] 찾거나 만든 그룹. */
	struct iommu_group *group;
	/* [한국어] 그룹 번호. */
	int groupid;

	/* [한국어] 그룹 배열은 첫 하드웨어에 있다. */
	data = mtk_iommu_get_frst_data(hw_list);
	if (!data)	/* [한국어] 목록이 비어 있다 — 있을 수 없는 상태다. */
		return ERR_PTR(-ENODEV);

	/* [한국어] 뱅크 또는 IOVA 영역이 그룹 번호가 된다. */
	groupid = mtk_iommu_get_group_id(dev, data->plat_data);
	if (groupid < 0)	/* [한국어] 그룹 번호를 정하지 못했다. */
		return ERR_PTR(groupid);

	/* [한국어] 그룹 배열을 만지는 동안 잠근다. */
	mutex_lock(&data->mutex);
	group = data->m4u_group[groupid];	/* [한국어] 그 번호의 그룹이 이미 있는지 본다. */
	if (!group) {
		/* [한국어] 이 번호의 첫 마스터다 — 그룹을 새로 만든다. */
		group = iommu_group_alloc();
		if (!IS_ERR(group))	/* [한국어] 만들기에 성공했으면 배열에 기록해 재사용한다. */
			data->m4u_group[groupid] = group;
	} else {
		/* [한국어] 이미 있으면 참조만 올려 같은 그룹을 쓴다. */
		iommu_group_ref_get(group);
	}
	mutex_unlock(&data->mutex);	/* [한국어] 그룹 배열 조작이 끝났으니 뮤텍스를 놓는다. */
	return group;	/* [한국어] 찾았거나 만든 그룹을 코어에 돌려준다. */
}

/*
 * [한국어]
 * mtk_iommu_of_xlate - 디바이스 트리의 iommus 참조를 처리한다
 *
 * @dev: 마스터 디바이스.
 * @args: "iommus = <&m4u 포트ID>"에서 파싱된 인자.
 * @return: 0 성공, 음수 오류.
 *
 * 두 가지를 한다: 첫 호출에서 M4U의 상태를 마스터에 매달고,
 * 인자를 포트 ID로 등록한다. 인자 하나에 larb 번호와 포트 번호가
 * 함께 인코딩되어 있어(dt-bindings의 매크로가 그것을 정의한다)
 * 값 하나면 충분하다.
 *
 * 실행 컨텍스트: 디바이스 트리 파싱. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 of_xlate → [mtk_iommu_of_xlate]
 */
static int mtk_iommu_of_xlate(struct device *dev,
			      const struct of_phandle_args *args)
{
	/* [한국어] 참조가 가리키는 M4U의 플랫폼 디바이스. */
	struct platform_device *m4updev;

	/* [한국어] 인자는 정확히 하나여야 한다 — larb와 포트가 그
	 * 하나에 인코딩되어 있다. */
	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",	/* [한국어] 인자 개수가 규약과 다르다. */
			args->args_count);
		return -EINVAL;	/* [한국어] 디바이스 트리가 잘못됐다. */
	}

	/* [한국어] 첫 호출에서만 M4U 상태를 매단다. */
	if (!dev_iommu_priv_get(dev)) {
		/* Get the m4u device */
		m4updev = of_find_device_by_node(args->np);
		/* [한국어] 코어가 이 콜백을 부르는 시점에는 M4U가
		 * 이미 있어야 한다 — 없으면 순서가 어긋난 것이다. */
		if (WARN_ON(!m4updev))
			return -EINVAL;

		/* [한국어] 이후 모든 콜백이 이 상태를 꺼내 쓴다. */
		dev_iommu_priv_set(dev, platform_get_drvdata(m4updev));

		/* [한국어] 찾기가 올린 참조를 놓는다. */
		put_device(&m4updev->dev);
	}

	/* [한국어] 포트 ID를 등록한다. 여러 번 불려 누적된다. */
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

/*
 * [한국어]
 * mtk_iommu_get_resv_regions - 이 마스터가 쓸 수 없는 IOVA를 알린다
 *
 * @dev: 마스터 디바이스.
 * @head: 예약 영역을 매달 목록.
 * @return: 없음.
 *
 * **다른 영역이 자기 영역 **안에** 겹쳐 있는 경우**를 예약으로
 * 알린다. CCU처럼 좁은 특수 영역이 넓은 일반 영역의 한가운데
 * 자리 잡고 있어, 그 일반 영역을 쓰는 마스터가 그 자리를
 * 침범하면 안 되기 때문이다.
 *
 * 판정 조건이 "완전히 안쪽"이라는 점에 유의: 시작이 뒤에 있고
 * 끝이 앞에 있어야 한다. 자기 자신은 시작이 같아 걸러진다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 get_resv_regions → [mtk_iommu_get_resv_regions]
 */
static void mtk_iommu_get_resv_regions(struct device *dev,
				       struct list_head *head)
{
	/* [한국어] 이 마스터의 M4U. */
	struct mtk_iommu_data *data = dev_iommu_priv_get(dev);
	/* [한국어] 이 마스터가 쓸 영역의 번호와 순회 인덱스. */
	unsigned int regionid = mtk_iommu_get_iova_region_id(dev, data->plat_data), i;
	/* [한국어] 검사 중인 영역과, 이 마스터의 영역. */
	const struct mtk_iommu_iova_region *resv, *curdom;
	/* [한국어] 만들 예약 영역. */
	struct iommu_resv_region *region;
	/* [한국어] 예약 영역의 권한. */
	int prot = IOMMU_WRITE | IOMMU_READ;

	/* [한국어] 영역을 찾지 못한 마스터에는 알릴 것도 없다. */
	if ((int)regionid < 0)
		return;
	curdom = data->plat_data->iova_region + regionid;
	/* [한국어] 모든 영역을 훑으며 자기 안에 들어 있는 것을 찾는다. */
	for (i = 0; i < data->plat_data->iova_region_nr; i++) {
		resv = data->plat_data->iova_region + i;	/* [한국어] 이번에 검사할 영역. */

		/* Only reserve when the region is inside the current domain */
		/* [한국어] 완전히 안쪽에 들어 있는 것만 예약한다.
		 * 자기 자신은 시작이 같아 이 조건에서 걸러진다. */
		if (resv->iova_base <= curdom->iova_base ||
		    resv->iova_base + resv->size >= curdom->iova_base + curdom->size)
			continue;

		/* [한국어] 그 구간을 예약으로 만들어 IOVA 할당기가
		 * 피해 가게 한다. */
		region = iommu_alloc_resv_region(resv->iova_base, resv->size,
						 prot, IOMMU_RESV_RESERVED,
						 GFP_KERNEL);
		if (!region)	/* [한국어] 예약 영역을 만들지 못했다. */
			return;

		list_add_tail(&region->list, head);	/* [한국어] 코어의 목록에 매단다. */
	}
}

/* [한국어] 이 드라이버가 IOMMU 코어에 제공하는 연산 테이블. */
static const struct iommu_ops mtk_iommu_ops = {
	.identity_domain = &mtk_iommu_identity_domain,
	/* [한국어] 통과 모드 — larb의 포트를 꺼 IOMMU를 우회시킨다. */

	.domain_alloc_paging = mtk_iommu_domain_alloc_paging,
	/* [한국어] 페이징 도메인 생성. 껍데기만 만들고 attach가 완성한다. */

	.probe_device	= mtk_iommu_probe_device,
	/* [한국어] 디바이스 등록. larb와의 전원 링크를 만든다. */

	.release_device	= mtk_iommu_release_device,
	/* [한국어] 그 링크를 끊는다. */

	.device_group	= mtk_iommu_device_group,
	/* [한국어] 뱅크 또는 IOVA 영역을 격리 단위로 삼는다. */

	.of_xlate	= mtk_iommu_of_xlate,
	/* [한국어] 디바이스 트리의 포트 ID를 등록한다. */

	.get_resv_regions = mtk_iommu_get_resv_regions,
	/* [한국어] 자기 영역 안에 겹친 특수 영역을 예약으로 알린다. */

	.owner		= THIS_MODULE,
	/* [한국어] 모듈 참조 계수의 주인. */

	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= mtk_iommu_attach_device,
		/* [한국어] 붙이기. 도메인 완성, 뱅크 초기화, 포트 켜기. */

		.map_pages	= mtk_iommu_map,
		/* [한국어] 매핑. 4GB 모드 보정 후 io-pgtable에 위임한다. */

		.unmap_pages	= mtk_iommu_unmap,
		/* [한국어] 해제. 무효화 범위를 모아 둔다. */

		.flush_iotlb_all = mtk_iommu_flush_iotlb_all,
		/* [한국어] 전체 무효화. */

		.iotlb_sync	= mtk_iommu_iotlb_sync,
		/* [한국어] 모아 둔 범위를 실제로 무효화한다. */

		.iotlb_sync_map	= mtk_iommu_sync_map,
		/* [한국어] 새 매핑을 가리는 부정 캐시를 지운다. */

		.iova_to_phys	= mtk_iommu_iova_to_phys,
		/* [한국어] 조회. 4GB 모드의 보정을 되돌린다. */

		.free		= mtk_iommu_domain_free,
		/* [한국어] 도메인 해제. 테이블은 공유되므로 건드리지 않는다. */
	}
};

/*
 * [한국어]
 * mtk_iommu_hw_init - 뱅크 하나의 하드웨어를 설정한다
 *
 * @data: 대상 M4U.
 * @bankid: 설정할 뱅크 번호.
 * @return: 0 성공, -ENODEV(인터럽트 등록 실패).
 *
 * 설정이 **전역과 뱅크별로 나뉜다**. AXI 동작 방식, 폴트 처리
 * 정책, 클럭 게이팅 같은 것은 하드웨어 전체에 하나뿐이라 뱅크 0의
 * 레지스터로 쓰고, 인터럽트 활성화와 보호 주소는 뱅크마다 쓴다.
 *
 * 전역 설정을 매번 다시 쓰는 이유는 원본 주석이 밝힌다: 뱅크 0에
 * 붙는 마스터가 있으리라는 보장이 없어, 어느 뱅크가 처음
 * 초기화되든 전역 설정이 반영되게 하려는 것이다.
 *
 * 대부분의 분기가 플래그 하나씩을 확인하는 형태이며, 그 목록이
 * 곧 이 하드웨어 계열의 세대 차이 전부다.
 *
 * 실행 컨텍스트: attach 경로. 전원이 켜진 상태여야 한다.
 *
 * 호출 체인:
 *   mtk_iommu_attach_device() → [mtk_iommu_hw_init]
 */
static int mtk_iommu_hw_init(const struct mtk_iommu_data *data, unsigned int bankid)
{
	/* [한국어] 설정할 뱅크. */
	const struct mtk_iommu_bank_data *bankx = &data->bank[bankid];
	/* [한국어] 전역 설정이 놓인 뱅크 0. */
	const struct mtk_iommu_bank_data *bank0 = &data->bank[0];
	/* [한국어] 조립할 레지스터 값. */
	u32 regval;

	/*
	 * Global control settings are in bank0. May re-init these global registers
	 * since no sure if there is bank0 consumers.
	 */
	/* [한국어] 폴트를 보호 주소로 돌리는 설정. 비트 위치가 세대마다
	 * 달라, 구형은 값을 통째로 새로 쓰고 신형은 기존 값에 얹는다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, TF_PORT_TO_ADDR_MT8173)) {
		regval = F_MMU_PREFETCH_RT_REPLACE_MOD |	/* [한국어] 구형은 미리 읽기 설정과 함께 값을 통째로 새로 쓴다. */
			 F_MMU_TF_PROT_TO_PROGRAM_ADDR_MT8173;
	} else {
		regval = readl_relaxed(bank0->base + REG_MMU_CTRL_REG);	/* [한국어] 신형은 기존 값을 읽어 보존한다. */
		regval |= F_MMU_TF_PROT_TO_PROGRAM_ADDR;	/* [한국어] 거기에 폴트 처리 비트를 얹는다. */
	}
	writel_relaxed(regval, bank0->base + REG_MMU_CTRL_REG);

	/* [한국어] 4GB 모드에서는 유효한 물리 주소 범위를 하드웨어에
	 * 알려 줘야 한다. */
	if (data->enable_4GB &&
	    MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_VLD_PA_RNG)) {
		/*
		 * If 4GB mode is enabled, the validate PA range is from
		 * 0x1_0000_0000 to 0x1_ffff_ffff. here record bit[32:30].
		 */
		/* [한국어] 재배치된 메모리가 4GB~8GB에 있으므로, 비트
		 * 32:30 단위로 시작 4(=4GB)와 끝 7(=8GB 직전)을 지정한다. */
		regval = F_MMU_VLD_PA_RNG(7, 4);
		writel_relaxed(regval, bank0->base + REG_MMU_VLD_PA_RNG);	/* [한국어] 유효 주소 범위를 하드웨어에 알린다. */
	}
	/* [한국어] 동적 클럭 관리를 끌지 정한다. 일부 하드웨어에서
	 * DCM이 문제를 일으켜 플래그로 제어한다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, DCM_DISABLE))
		writel_relaxed(F_MMU_DCM, bank0->base + REG_MMU_DCM_DIS);
	else
		writel_relaxed(0, bank0->base + REG_MMU_DCM_DIS);

	/* [한국어] 쓰기 조절을 켠다. 레지스터가 "끄기" 비트라
	 * 그것을 내리는 것이 켜는 동작이다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, WR_THROT_EN)) {
		/* write command throttling mode */
		regval = readl_relaxed(bank0->base + REG_MMU_WR_LEN_CTRL);	/* [한국어] 현재 값을 읽어 다른 비트를 보존한다. */
		regval &= ~F_MMU_WR_THROT_DIS_MASK;	/* [한국어] "끄기" 비트를 내려 조절을 켠다. */
		writel_relaxed(regval, bank0->base + REG_MMU_WR_LEN_CTRL);	/* [한국어] 조절 설정을 반영한다. */
	}

	/* [한국어] AXI 동작 방식을 정한다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, RESET_AXI)) {
		/* The register is called STANDARD_AXI_MODE in this case */
		/* [한국어] 구형 하드웨어에서는 이 레지스터가 다른 이름이며,
		 * 0을 쓰는 것이 곧 리셋이다. */
		regval = 0;
	} else {
		regval = readl_relaxed(bank0->base + REG_MMU_MISC_CTRL);
		/* [한국어] 표준 AXI 모드를 쓰지 않는다면 그 비트를 내려
		 * MediaTek 확장 동작을 쓴다. */
		if (!MTK_IOMMU_HAS_FLAG(data->plat_data, STD_AXI_MODE))
			regval &= ~F_MMU_STANDARD_AXI_MODE_MASK;
		/* [한국어] 순서 없는 쓰기를 허용하려면 "순서대로" 비트를 내린다. */
		if (MTK_IOMMU_HAS_FLAG(data->plat_data, OUT_ORDER_WR_EN))
			regval &= ~F_MMU_IN_ORDER_WR_EN_MASK;
	}
	writel_relaxed(regval, bank0->base + REG_MMU_MISC_CTRL);	/* [한국어] 조립한 AXI 설정을 쓴다. */

	/* Independent settings for each bank */
	/* [한국어] 여기서부터는 뱅크마다 따로 설정한다.
	 * 발생 가능한 모든 오류 인터럽트를 켠다 — 조용히 넘어가는
	 * 것보다 로그에 남는 편이 낫다는 판단이다. */
	regval = F_L2_MULIT_HIT_EN |
		F_TABLE_WALK_FAULT_INT_EN |
		F_PREETCH_FIFO_OVERFLOW_INT_EN |
		F_MISS_FIFO_OVERFLOW_INT_EN |
		F_PREFETCH_FIFO_ERR_INT_EN |
		F_MISS_FIFO_ERR_INT_EN;
	writel_relaxed(regval, bankx->base + REG_MMU_INT_CONTROL0);

	/* [한국어] 주 인터럽트도 모든 폴트 종류를 켠다.
	 * 각 상수가 두 비트를 담고 있어(변환 유닛이 둘) 이 한 번으로
	 * 양쪽이 모두 켜진다. */
	regval = F_INT_TRANSLATION_FAULT |
		F_INT_MAIN_MULTI_HIT_FAULT |
		F_INT_INVALID_PA_FAULT |
		F_INT_ENTRY_REPLACEMENT_FAULT |
		F_INT_TLB_MISS_FAULT |
		F_INT_MISS_TRANSACTION_FIFO_FAULT |
		F_INT_PRETETCH_TRANSATION_FIFO_FAULT;
	writel_relaxed(regval, bankx->base + REG_MMU_INT_MAIN_CONTROL);

	/* [한국어] 폴트 시 접근을 돌릴 보호 주소를 심는다.
	 * 구형은 주소를 오른쪽으로 한 칸 밀고 4GB 모드 비트를
	 * 최상위에 얹는 특수 형식을 쓴다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_LEGACY_IVRP_PADDR))
		regval = (data->protect_base >> 1) | (data->enable_4GB << 31);
	else
		/* [한국어] 신형은 상위와 하위를 OR 해 한 워드에 담는다 —
		 * 주소가 32비트를 넘을 수 있기 때문이다. */
		regval = lower_32_bits(data->protect_base) |
			 upper_32_bits(data->protect_base);
	writel_relaxed(regval, bankx->base + REG_MMU_IVRP_PADDR);

	/* [한국어] 이 뱅크의 폴트 인터럽트를 등록한다. 설정이 모두
	 * 끝난 뒤여야 준비되지 않은 상태에서 인터럽트를 받지 않는다. */
	if (devm_request_irq(bankx->parent_dev, bankx->irq, mtk_iommu_isr, 0,
			     dev_name(bankx->parent_dev), (void *)bankx)) {
		/* [한국어] 핸들러 없이 변환을 켜 두면 폴트를 처리할 수
		 * 없다 — 테이블 주소를 지워 이 뱅크를 무력화한다. */
		writel_relaxed(0, bankx->base + REG_MMU_PT_BASE_ADDR);
		dev_err(bankx->parent_dev, "Failed @ IRQ-%d Request\n", bankx->irq);	/* [한국어] 어느 인터럽트 등록이 실패했는지 남긴다. */
		return -ENODEV;	/* [한국어] 폴트를 처리할 수 없으므로 이 뱅크를 쓸 수 없다. */
	}

	return 0;	/* [한국어] 이 뱅크의 하드웨어 설정이 끝났다. */
}

/* [한국어] component 프레임워크에 넘길 콜백 묶음.
 * larb들이 모두 준비되면 bind가, 해체될 때 unbind가 불린다. */
static const struct component_master_ops mtk_iommu_com_ops = {
	.bind		= mtk_iommu_bind,
	/* [한국어] larb_imu 배열을 채운다. */

	.unbind		= mtk_iommu_unbind,
	/* [한국어] 그 연결을 되돌린다. */
};

/*
 * [한국어]
 * mtk_iommu_mm_dts_parse - 멀티미디어 IOMMU의 디바이스 트리를 파싱한다
 *
 * @dev: M4U 디바이스.
 * @match: component 매치 목록을 만들 곳.
 * @data: 채울 M4U 상태.
 * @return: 0 성공, 음수 오류.
 *
 * 두 가지를 확인하고 두 가지를 만든다.
 *
 * 확인: (1) 각 larb가 실제로 존재하고 드라이버가 붙어 있는가.
 * 아직이면 -EPROBE_DEFER로 물러나 나중에 다시 시도한다.
 * (2) **모든 larb가 같은 SMI common에 붙어 있는가.** 다르면
 * 하나의 M4U가 담당할 수 없는 구성이다.
 *
 * 만듦: (1) component 매치 목록 — larb들이 준비되면 bind가 불린다.
 * (2) SMI common과의 전원 링크 — M4U가 깨어날 때 그쪽도 켜져야 한다.
 * 이 링크는 방향이 반대인 점에 유의: **SMI common이 소비자**이고
 * M4U가 공급자다.
 *
 * SMI 계층이 두 단계일 수 있어(sub-common → common) 한 번 더
 * 따라가는 처리가 들어 있다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   mtk_iommu_probe() → [mtk_iommu_mm_dts_parse]
 */
static int mtk_iommu_mm_dts_parse(struct device *dev, struct component_match **match,
				  struct mtk_iommu_data *data)
{
	/* [한국어] 현재 larb 노드와, 모든 larb가 공유해야 할 SMI common. */
	struct device_node *larbnode, *frst_avail_smicomm_node = NULL;
	/* [한국어] 그 노드들의 플랫폼 디바이스. */
	struct platform_device *plarbdev, *pcommdev;
	/* [한국어] SMI common과 만들 전원 링크. */
	struct device_link *link;
	/* [한국어] 순회 인덱스, larb 개수, 결과 코드. */
	int i, larb_nr, ret;

	/* [한국어] 이 M4U에 붙은 larb의 수를 센다. */
	larb_nr = of_count_phandle_with_args(dev->of_node, "mediatek,larbs", NULL);
	if (larb_nr < 0)	/* [한국어] larb 개수를 세지 못했다 — 속성이 없거나 형식이 다르다. */
		return larb_nr;
	/* [한국어] 하나도 없거나 배열 크기를 넘으면 다룰 수 없다. */
	if (larb_nr == 0 || larb_nr > MTK_LARB_NR_MAX)
		return -EINVAL;

	/* [한국어] larb를 하나씩 따라간다. */
	for (i = 0; i < larb_nr; i++) {
		/* [한국어] 이 larb가 붙은 SMI 노드들. */
		struct device_node *smicomm_node, *smi_subcomm_node;
		/* [한국어] 이 larb의 번호. */
		u32 id;

		larbnode = of_parse_phandle(dev->of_node, "mediatek,larbs", i);	/* [한국어] i번째 larb 노드를 따라간다. */
		if (!larbnode) {	/* [한국어] 참조가 가리키는 노드가 없다. */
			ret = -EINVAL;	/* [한국어] 디바이스 트리가 잘못됐다. */
			goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
		}

		/* [한국어] 비활성화된 larb는 건너뛴다 — 보드마다 쓰지
		 * 않는 larb가 있을 수 있다. */
		if (!of_device_is_available(larbnode)) {
			of_node_put(larbnode);	/* [한국어] 쓰지 않는 larb의 노드 참조를 놓는다. */
			continue;	/* [한국어] 다음 larb로 넘어간다. */
		}

		/* [한국어] larb 번호를 읽는다. */
		ret = of_property_read_u32(larbnode, "mediatek,larb-id", &id);
		if (ret)/* The id is consecutive if there is no this property */
			/* [한국어] 속성이 없으면 순서대로 번호가 매겨진
			 * 구성이라, 인덱스를 그대로 쓴다. */
			id = i;
		if (id >= MTK_LARB_NR_MAX) {	/* [한국어] 배열 범위를 벗어난 번호다. */
			of_node_put(larbnode);	/* [한국어] 노드 참조를 놓는다. */
			ret = -EINVAL;	/* [한국어] 디바이스 트리가 잘못됐다. */
			goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
		}

		/* [한국어] 그 노드의 플랫폼 디바이스를 찾는다. 여기서
		 * 올린 참조는 오류 경로가 한꺼번에 놓는다. */
		plarbdev = of_find_device_by_node(larbnode);
		of_node_put(larbnode);	/* [한국어] 디바이스를 찾았으니 노드 참조는 놓는다. */
		if (!plarbdev) {	/* [한국어] 그 노드의 플랫폼 디바이스가 아직 없다. */
			ret = -ENODEV;	/* [한국어] 나중에 다시 시도해야 한다. */
			goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
		}
		/* [한국어] 같은 번호가 두 번 나왔다 — 디바이스 트리 오류다. */
		if (data->larb_imu[id].dev) {
			platform_device_put(plarbdev);	/* [한국어] 중복이므로 방금 얻은 참조를 놓는다. */
			ret = -EEXIST;	/* [한국어] 같은 번호가 두 번 나왔다. */
			goto err_larbdev_put;	/* [한국어] 앞서 잡은 참조들도 놓으러 간다. */
		}
		data->larb_imu[id].dev = &plarbdev->dev;

		/* [한국어] larb 드라이버가 아직 붙지 않았다 — 나중에
		 * 다시 시도하도록 물러난다. */
		if (!plarbdev->dev.driver) {
			ret = -EPROBE_DEFER;	/* [한국어] larb 드라이버가 아직 붙지 않았다 — 나중에 다시 시도한다. */
			goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
		}

		/* Get smi-(sub)-common dev from the last larb. */
		/* [한국어] 이 larb가 붙은 SMI 노드를 따라간다. */
		smi_subcomm_node = of_parse_phandle(larbnode, "mediatek,smi", 0);
		if (!smi_subcomm_node) {	/* [한국어] 이 larb가 어느 SMI에 붙었는지 알 수 없다. */
			ret = -EINVAL;	/* [한국어] 디바이스 트리가 잘못됐다. */
			goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
		}

		/*
		 * It may have two level smi-common. the node is smi-sub-common if it
		 * has a new mediatek,smi property. otherwise it is smi-commmon.
		 */
		/* [한국어] SMI 계층이 두 단계일 수 있다. 방금 얻은 노드가
		 * 또 mediatek,smi를 갖고 있으면 그것은 sub-common이므로
		 * 한 번 더 따라가야 진짜 common에 닿는다. */
		smicomm_node = of_parse_phandle(smi_subcomm_node, "mediatek,smi", 0);
		if (smicomm_node)	/* [한국어] 한 단계 더 있었다면 sub-common 참조는 놓는다. */
			of_node_put(smi_subcomm_node);
		else
			smicomm_node = smi_subcomm_node;

		/*
		 * All the larbs that connect to one IOMMU must connect with the same
		 * smi-common.
		 */
		/* [한국어] 첫 larb의 SMI common을 기준으로 삼고, 이후
		 * larb들이 같은 곳에 붙어 있는지 확인한다. 다르면
		 * 하나의 M4U가 담당할 수 없는 구성이다. */
		if (!frst_avail_smicomm_node) {
			frst_avail_smicomm_node = smicomm_node;	/* [한국어] 첫 larb의 SMI common을 기준으로 삼는다. */
		} else if (frst_avail_smicomm_node != smicomm_node) {	/* [한국어] 이후 larb가 다른 곳에 붙어 있는 경우. */
			dev_err(dev, "mediatek,smi property is not right @larb%d.", id);	/* [한국어] 어느 larb에서 어긋났는지 남긴다. */
			of_node_put(smicomm_node);	/* [한국어] 이번에 올린 노드 참조를 놓는다. */
			ret = -EINVAL;	/* [한국어] 하나의 M4U가 담당할 수 없는 구성이다. */
			goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
		} else {
			/* [한국어] 같은 노드였으니 이번에 올린 참조를 놓는다. */
			of_node_put(smicomm_node);
		}

		/* [한국어] 이 larb를 component 매치 목록에 넣는다.
		 * 목록의 모든 larb가 준비되면 bind가 불린다. */
		component_match_add(dev, match, component_compare_dev, &plarbdev->dev);
	}

	/* [한국어] 쓸 수 있는 larb가 하나도 없었다. */
	if (!frst_avail_smicomm_node) {
		ret = -EINVAL;	/* [한국어] 쓸 수 있는 larb가 하나도 없었다. */
		goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
	}

	/* [한국어] 그 SMI common의 디바이스를 얻는다. */
	pcommdev = of_find_device_by_node(frst_avail_smicomm_node);
	of_node_put(frst_avail_smicomm_node);	/* [한국어] 디바이스를 찾았으니 노드 참조는 놓는다. */
	if (!pcommdev) {	/* [한국어] SMI common의 디바이스가 아직 없다. */
		ret = -ENODEV;	/* [한국어] 나중에 다시 시도해야 한다. */
		goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
	}
	data->smicomm_dev = &pcommdev->dev;

	/* [한국어] **방향에 유의**: SMI common이 소비자이고 M4U가
	 * 공급자다. SMI common이 깨어날 때 M4U가 먼저 깨어나야
	 * 변환이 준비된 상태가 되기 때문이다. */
	link = device_link_add(data->smicomm_dev, dev,
			       DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);
	/* [한국어] 링크가 참조를 갖고 있으므로 찾기의 참조는 놓는다. */
	platform_device_put(pcommdev);
	if (!link) {	/* [한국어] 전원 링크를 만들지 못한 경우. */
		dev_err(dev, "Unable to link %s.\n", dev_name(data->smicomm_dev));	/* [한국어] 어느 디바이스와의 링크가 실패했는지 남긴다. */
		ret = -EINVAL;	/* [한국어] 이 구성으로는 진행할 수 없다. */
		goto err_larbdev_put;	/* [한국어] 잡아 둔 참조를 놓으러 간다. */
	}
	return 0;

/* [한국어] 어느 단계에서 실패했든 잡아 둔 larb 참조를 모두 놓는다. */
err_larbdev_put:
	/* id mapping may not be linear, loop the whole array */
	/* [한국어] 번호가 연속이 아닐 수 있어 배열 전체를 훑는다.
	 * put_device는 NULL에 무해하므로 빈 자리는 그냥 넘어간다. */
	for (i = 0; i < MTK_LARB_NR_MAX; i++)
		put_device(data->larb_imu[i].dev);

	return ret;	/* [한국어] 실패 이유를 probe에 전한다. */
}

/*
 * [한국어]
 * mtk_iommu_probe - M4U 하드웨어 하나를 초기화한다
 *
 * @pdev: 플랫폼 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 순서에 이유가 있다.
 *
 *  1) 보호 메모리를 먼저 잡는다. 폴트 시 접근이 이리로 돌려지므로,
 *     하드웨어를 켜기 전에 준비되어 있어야 한다. 두 배로 잡아
 *     정렬 후에도 충분한 공간이 남게 한다.
 *  2) 4GB 모드 여부를 infracfg에서 읽는다.
 *  3) 레지스터 영역을 매핑하고 **뱅크 수만큼 충분한지 검사한다** —
 *     뱅크가 4KB씩 이어져 있으므로 크기가 곧 뱅크 수의 제약이다.
 *  4) 뱅크마다 창과 인터럽트를 배정한다.
 *  5) 종류에 따라 larb를 파싱하거나(MM) PERICFG를 찾는다(INFRA).
 *  6) **테이블 공유 여부에 따라 목록을 고른다.** 공유하면 전역
 *     목록에, 아니면 자기 목록에 자신을 넣는다.
 *  7) 코어에 등록하고, MM이면 component 마스터로 등록해 larb들의
 *     결합을 기다린다.
 *
 * 실행 컨텍스트: 플랫폼 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [mtk_iommu_probe] → mtk_iommu_mm_dts_parse()
 *   → iommu_device_register()
 */
static int mtk_iommu_probe(struct platform_device *pdev)
{
	/* [한국어] 만들 M4U 상태. */
	struct mtk_iommu_data   *data;
	/* [한국어] 로그와 devm의 기준 디바이스. */
	struct device           *dev = &pdev->dev;
	/* [한국어] 레지스터 자원과 그 시작 주소(sysfs 이름에 쓴다). */
	struct resource         *res;
	resource_size_t		ioaddr;
	/* [한국어] component 매치 목록. */
	struct component_match  *match = NULL;
	/* [한국어] 4GB 모드 여부를 읽을 레지스터 영역. */
	struct regmap		*infracfg;
	/* [한국어] 보호 메모리. */
	void                    *protect;
	/* [한국어] 결과 코드, 뱅크 수, 순회 인덱스. */
	int                     ret, banks_num, i = 0;
	/* [한국어] infracfg에서 읽은 값. */
	u32			val;
	/* [한국어] 호환 문자열을 담을 임시 포인터. */
	char                    *p;
	/* [한국어] 설정 중인 뱅크. */
	struct mtk_iommu_bank_data *bank;
	/* [한국어] 매핑된 레지스터 영역의 시작. */
	void __iomem		*base;

	/* [한국어] 디바이스 수명에 묶어 상태를 잡는다. */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)	/* [한국어] 상태 구조체를 잡지 못했다. */
		return -ENOMEM;
	data->dev = dev;
	/* [한국어] 이 SoC의 설정 표. 거의 모든 분기가 여기를 본다. */
	data->plat_data = of_device_get_match_data(dev);

	/* Protect memory. HW will access here while translation fault.*/
	/* [한국어] 정렬 때문에 앞부분이 잘려도 충분하도록 두 배로 잡는다.
	 * 잘못된 DMA가 이리로 돌려지므로 실제 메모리여야 한다. */
	protect = devm_kcalloc(dev, 2, MTK_PROTECT_PA_ALIGN, GFP_KERNEL);
	if (!protect)	/* [한국어] 보호 메모리를 잡지 못했다. */
		return -ENOMEM;
	data->protect_base = ALIGN(virt_to_phys(protect), MTK_PROTECT_PA_ALIGN);

	/* [한국어] 4GB 모드는 메모리 구성에 달린 것이라 infracfg에서 읽는다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_4GB_MODE)) {
		infracfg = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");	/* [한국어] 디바이스 트리의 참조로 infracfg를 찾는다. */
		if (IS_ERR(infracfg)) {	/* [한국어] 참조가 없거나 찾지 못한 경우. */
			/*
			 * Legacy devicetrees will not specify a phandle to
			 * mediatek,infracfg: in that case, we use the older
			 * way to retrieve a syscon to infra.
			 *
			 * This is for retrocompatibility purposes only, hence
			 * no more compatibles shall be added to this.
			 */
			/* [한국어] 옛 디바이스 트리에는 참조가 없어, SoC를
			 * 직접 알아 호환 문자열로 찾는다. 원본 주석이
			 * 밝히듯 새 SoC는 이 목록에 추가하지 않는다. */
			switch (data->plat_data->m4u_plat) {
			case M4U_MT2712:	/* [한국어] 옛 디바이스 트리를 쓰는 SoC 중 하나. */
				p = "mediatek,mt2712-infracfg";	/* [한국어] 그 SoC의 infracfg 호환 문자열. */
				break;
			case M4U_MT8173:	/* [한국어] 또 다른 옛 SoC. */
				p = "mediatek,mt8173-infracfg";	/* [한국어] 그 SoC의 호환 문자열. */
				break;
			default:	/* [한국어] 그 밖의 SoC는 이 경로로 오지 않는다. */
				p = NULL;	/* [한국어] 찾을 문자열이 없으면 아래 조회가 실패한다. */
			}

			infracfg = syscon_regmap_lookup_by_compatible(p);	/* [한국어] 호환 문자열로 infracfg를 찾는다. */
			if (IS_ERR(infracfg))	/* [한국어] 어느 방법으로도 찾지 못했다. */
				return PTR_ERR(infracfg);
		}

		/* [한국어] 4GB 지원 비트를 읽어 모드를 확정한다. 이 값이
		 * map/iova_to_phys의 주소 보정을 좌우한다. */
		ret = regmap_read(infracfg, REG_INFRA_MISC, &val);
		if (ret)	/* [한국어] 레지스터를 읽지 못했다. */
			return ret;
		data->enable_4GB = !!(val & F_DDR_4GB_SUPPORT_EN);	/* [한국어] 4GB 지원 비트가 곧 이 모드의 활성화 여부다. */
	}

	/* [한국어] 이 SoC가 가진 뱅크의 수. */
	banks_num = data->plat_data->banks_num;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);	/* [한국어] 첫 메모리 자원이 레지스터 영역이다. */
	if (!res)	/* [한국어] 자원이 없으면 진행할 수 없다. */
		return -EINVAL;
	/* [한국어] 뱅크가 4KB씩 이어져 있으므로, 영역이 그만큼 넓어야
	 * 모든 뱅크에 닿을 수 있다. */
	if (resource_size(res) < banks_num * MTK_IOMMU_BANK_SZ) {
		dev_err(dev, "banknr %d. res %pR is not enough.\n", banks_num, res);	/* [한국어] 뱅크 수에 비해 영역이 좁다. */
		return -EINVAL;	/* [한국어] 모든 뱅크에 닿을 수 없다. */
	}
	base = devm_ioremap_resource(dev, res);	/* [한국어] 레지스터 영역을 매핑한다. */
	if (IS_ERR(base))	/* [한국어] 매핑에 실패했다. */
		return PTR_ERR(base);
	/* [한국어] sysfs 이름에 쓸 물리 주소 — 여러 M4U를 구별하는 근거다. */
	ioaddr = res->start;

	/* [한국어] 뱅크 배열을 잡는다. 아래 루프가 모든 자리를 채운다. */
	data->bank = devm_kmalloc(dev, banks_num * sizeof(*data->bank), GFP_KERNEL);
	if (!data->bank)	/* [한국어] 뱅크 배열을 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 뱅크마다 창과 인터럽트를 배정한다.
	 * do-while인 이유: banks_num이 최소 1임이 보장되기 때문이다. */
	do {
		/* [한국어] 쓰지 않는 뱅크는 건너뛴다 — 하드웨어에 있어도
		 * 이 SoC에서 활성화되지 않은 것이 있다. */
		if (!data->plat_data->banks_enable[i])
			continue;
		bank = &data->bank[i];	/* [한국어] 이번에 설정할 뱅크. */
		bank->id = i;
		/* [한국어] 뱅크 번호에 창 크기를 곱해 자기 레지스터
		 * 영역을 잡는다. */
		bank->base = base + i * MTK_IOMMU_BANK_SZ;
		/* [한국어] 아직 도메인이 붙지 않았음을 표시한다 —
		 * attach가 이 값으로 초기화 여부를 판단한다. */
		bank->m4u_dom = NULL;

		/* [한국어] 뱅크마다 인터럽트가 따로 있다. */
		bank->irq = platform_get_irq(pdev, i);
		if (bank->irq < 0)	/* [한국어] 이 뱅크의 인터럽트를 얻지 못했다. */
			return bank->irq;
		bank->parent_dev = dev;	/* [한국어] 인터럽트 등록과 로그의 기준 디바이스. */
		bank->parent_data = data;
		/* [한국어] 이 뱅크의 무효화를 직렬화할 락. */
		spin_lock_init(&bank->tlb_lock);
	} while (++i < banks_num);

	/* [한국어] 전용 클럭이 필요한 하드웨어면 얻어 둔다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_BCLK)) {
		data->bclk = devm_clk_get(dev, "bclk");	/* [한국어] 전용 클럭을 얻는다. */
		if (IS_ERR(data->bclk))	/* [한국어] 클럭을 얻지 못했다. */
			return PTR_ERR(data->bclk);
	}

	/* [한국어] 페이지 테이블 자체가 35비트 주소에 놓일 수 있는
	 * 하드웨어라면 DMA 계층에 그것을 알린다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, PGTABLE_PA_35_EN)) {
		ret = dma_set_mask(dev, DMA_BIT_MASK(35));	/* [한국어] 테이블이 35비트 주소에 놓일 수 있음을 알린다. */
		if (ret) {	/* [한국어] 마스크 설정이 거부된 경우. */
			dev_err(dev, "Failed to set dma_mask 35.\n");	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
			return ret;	/* [한국어] 테이블을 올바른 자리에 잡을 수 없다. */
		}
	}

	/* [한국어] 런타임 PM을 켠다. 무효화가 전원 상태를 확인하는
	 * 근거가 여기서 생긴다. */
	pm_runtime_enable(dev);

	/* [한국어] 종류에 따라 마스터를 잇는 방법이 다르다. */
	if (MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_MM)) {
		/* [한국어] 멀티미디어 — larb들을 파싱하고 SMI common과
		 * 링크를 만든다. */
		ret = mtk_iommu_mm_dts_parse(dev, &match, data);
		if (ret) {	/* [한국어] larb 파싱이 실패한 경우. */
			dev_err_probe(dev, ret, "mm dts parse fail\n");	/* [한국어] -EPROBE_DEFER도 여기로 오므로 err_probe를 쓴다. */
			goto out_runtime_disable;	/* [한국어] 켜 둔 런타임 PM을 되돌리러 간다. */
		}
	} else if (MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_INFRA) &&	/* [한국어] 인프라이면서 ATF에 맡기지 않는 구성. */
		   !MTK_IOMMU_HAS_FLAG(data->plat_data, CFG_IFA_MASTER_IN_ATF)) {
		/* [한국어] 인프라 — 포트를 켤 PERICFG 영역을 찾아 둔다.
		 * ATF에 맡기는 구성에서는 이것도 필요 없다. */
		p = data->plat_data->pericfg_comp_str;
		data->pericfg = syscon_regmap_lookup_by_compatible(p);	/* [한국어] 포트를 켤 PERICFG 영역을 찾는다. */
		if (IS_ERR(data->pericfg)) {	/* [한국어] 찾지 못한 경우. */
			ret = PTR_ERR(data->pericfg);	/* [한국어] 실패 이유를 보관한다. */
			goto out_runtime_disable;	/* [한국어] 켜 둔 런타임 PM을 되돌리러 간다. */
		}
	}

	/* [한국어] of_xlate가 이 값으로 M4U를 찾으므로 등록 전에 심는다. */
	platform_set_drvdata(pdev, data);
	/* [한국어] 그룹 배열과 공유 도메인을 보호할 뮤텍스. */
	mutex_init(&data->mutex);

	/* [한국어] 테이블 공유 여부가 어느 목록에 들어갈지를 정한다.
	 * 공유하면 전역 목록에 함께 들어가 서로를 찾을 수 있고,
	 * 아니면 자기 안의 목록에 혼자 들어간다 — 어느 쪽이든
	 * 무효화 코드는 같은 순회로 처리된다. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, SHARE_PGTABLE)) {
		list_add_tail(&data->list, data->plat_data->hw_list);	/* [한국어] 전역 목록에 자신을 넣어 다른 하드웨어와 테이블을 공유한다. */
		data->hw_list = data->plat_data->hw_list;	/* [한국어] 순회할 목록으로 그 전역 목록을 가리킨다. */
	} else {
		INIT_LIST_HEAD(&data->hw_list_head);	/* [한국어] 공유하지 않으므로 자기만의 목록을 만든다. */
		list_add_tail(&data->list, &data->hw_list_head);	/* [한국어] 거기에 자신만 넣는다. */
		data->hw_list = &data->hw_list_head;	/* [한국어] 순회할 목록으로 그것을 가리킨다. */
	}

	/* [한국어] sysfs에 노출한다. 이름에 물리 주소를 넣어 여러
	 * M4U를 구별한다. */
	ret = iommu_device_sysfs_add(&data->iommu, dev, NULL,
				     "mtk-iommu.%pa", &ioaddr);
	if (ret)	/* [한국어] sysfs 등록이 실패했다. */
		goto out_list_del;

	/* [한국어] 코어에 등록한다. 이 순간부터 마스터들의
	 * of_xlate와 probe_device가 불리기 시작한다. */
	ret = iommu_device_register(&data->iommu, &mtk_iommu_ops, dev);
	if (ret)	/* [한국어] 코어 등록이 실패했다. */
		goto out_sysfs_remove;

	/* [한국어] 멀티미디어라면 component 마스터로 등록해 larb들의
	 * 결합을 기다린다. 모두 준비되면 bind가 불린다. */
	if (MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_MM)) {
		ret = component_master_add_with_match(dev, &mtk_iommu_com_ops, match);	/* [한국어] larb들이 준비되면 bind가 불리도록 등록한다. */
		if (ret)	/* [한국어] component 마스터 등록이 실패했다. */
			goto out_device_unregister;
	}
	return ret;

/* [한국어] component 등록 실패 — 코어 등록부터 되돌린다. */
out_device_unregister:
	iommu_device_unregister(&data->iommu);
/* [한국어] 코어 등록 실패 — sysfs를 되돌린다. */
out_sysfs_remove:
	iommu_device_sysfs_remove(&data->iommu);
/* [한국어] sysfs 실패 — 목록에서 빼고, MM이면 링크와 larb 참조도 놓는다. */
out_list_del:
	list_del(&data->list);	/* [한국어] 공유 목록에서 자신을 뺀다. */
	if (MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_MM)) {	/* [한국어] 멀티미디어라면 링크와 참조도 정리해야 한다. */
		device_link_remove(data->smicomm_dev, dev);	/* [한국어] SMI common과의 전원 링크를 끊는다. */

		for (i = 0; i < MTK_LARB_NR_MAX; i++)	/* [한국어] 잡아 둔 larb 참조를 모두 놓는다. */
			put_device(data->larb_imu[i].dev);
	}
/* [한국어] 런타임 PM을 켠 뒤의 실패 — 그것을 되돌린다. */
out_runtime_disable:
	pm_runtime_disable(dev);	/* [한국어] 켜 둔 런타임 PM을 되돌린다. */
	return ret;	/* [한국어] 실패 이유를 플랫폼 코어에 전한다. */
}

/*
 * [한국어]
 * mtk_iommu_remove - M4U를 걷어낸다
 *
 * @pdev: 대상 플랫폼 디바이스.
 * @return: 없음.
 *
 * probe의 역순이다. 인터럽트를 마지막에 떼는데, **초기화된
 * 뱅크만** 대상으로 삼는 점에 유의 — hw_init이 불리지 않은
 * 뱅크에는 등록된 핸들러가 없다.
 *
 * 페이지 테이블을 해제하지 않는 점도 눈에 띈다. 도메인 해제가
 * 코어의 몫이고, 그때도 공유 때문에 해제하지 않으므로 결국
 * 이 드라이버는 테이블을 반납하지 않는다.
 *
 * 실행 컨텍스트: 드라이버 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [mtk_iommu_remove]
 */
static void mtk_iommu_remove(struct platform_device *pdev)
{
	/* [한국어] 대상 M4U. */
	struct mtk_iommu_data *data = platform_get_drvdata(pdev);
	/* [한국어] 인터럽트를 뗄 뱅크. */
	struct mtk_iommu_bank_data *bank;
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] sysfs를 지우고 코어에서 뺀다. */
	iommu_device_sysfs_remove(&data->iommu);
	iommu_device_unregister(&data->iommu);

	/* [한국어] 공유 목록에서 자신을 뺀다. */
	list_del(&data->list);

	/* [한국어] 멀티미디어라면 SMI 링크와 component 마스터,
	 * 그리고 larb 참조를 정리한다. */
	if (MTK_IOMMU_IS_TYPE(data->plat_data, MTK_IOMMU_TYPE_MM)) {
		device_link_remove(data->smicomm_dev, &pdev->dev);	/* [한국어] SMI common과의 전원 링크를 끊는다. */
		component_master_del(&pdev->dev, &mtk_iommu_com_ops);	/* [한국어] component 마스터 등록을 되돌린다. */

		for (i = 0; i < MTK_LARB_NR_MAX; i++)	/* [한국어] 잡아 둔 larb 참조를 모두 놓는다. */
			put_device(data->larb_imu[i].dev);
	}
	pm_runtime_disable(&pdev->dev);
	/* [한국어] 초기화된 뱅크의 인터럽트만 뗀다 — 그렇지 않은
	 * 뱅크에는 등록된 핸들러가 없다. */
	for (i = 0; i < data->plat_data->banks_num; i++) {
		bank = &data->bank[i];	/* [한국어] 인터럽트를 뗄 후보 뱅크. */
		if (!bank->m4u_dom)	/* [한국어] 초기화된 적이 없는 뱅크에는 등록된 핸들러가 없다. */
			continue;
		devm_free_irq(&pdev->dev, bank->irq, bank);	/* [한국어] 그 뱅크의 폴트 핸들러를 뗀다. */
	}
}

/*
 * [한국어]
 * mtk_iommu_runtime_suspend - 잠들기 전에 레지스터를 떠 둔다
 *
 * @dev: M4U 디바이스.
 * @return: 항상 0.
 *
 * 전원이 끊기면 레지스터 내용이 사라지므로, 전역 설정과 뱅크별
 * 설정을 모두 저장한다. 전역 설정은 뱅크 0의 것만 뜨는데,
 * hw_init이 그쪽에만 쓰기 때문이다.
 *
 * 페이지 테이블 주소를 저장하지 않는 점에 유의: 리줌이
 * m4u_dom에서 직접 꺼내 쓴다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PM 코어 → [mtk_iommu_runtime_suspend]
 */
static int __maybe_unused mtk_iommu_runtime_suspend(struct device *dev)
{
	/* [한국어] 대상 M4U. */
	struct mtk_iommu_data *data = dev_get_drvdata(dev);
	/* [한국어] 값을 떠 둘 자리. */
	struct mtk_iommu_suspend_reg *reg = &data->reg;
	/* [한국어] 현재 읽고 있는 뱅크의 레지스터 창. */
	void __iomem *base;
	/* [한국어] 뱅크 순회 인덱스. */
	int i = 0;

	/* [한국어] 전역 설정은 뱅크 0에만 쓰이므로 그쪽에서 뜬다. */
	base = data->bank[i].base;
	reg->wr_len_ctrl = readl_relaxed(base + REG_MMU_WR_LEN_CTRL);	/* [한국어] 쓰기 조절 설정을 떠 둔다. 리줌이 이 값으로 첫 호출을 구별한다. */
	reg->misc_ctrl = readl_relaxed(base + REG_MMU_MISC_CTRL);	/* [한국어] AXI 동작 방식 설정. */
	reg->dcm_dis = readl_relaxed(base + REG_MMU_DCM_DIS);	/* [한국어] 동적 클럭 관리 설정. */
	reg->ctrl_reg = readl_relaxed(base + REG_MMU_CTRL_REG);	/* [한국어] 주 제어 레지스터. */
	reg->vld_pa_rng = readl_relaxed(base + REG_MMU_VLD_PA_RNG);
	/* [한국어] 뱅크별 설정은 각 뱅크에서 따로 뜬다. */
	do {
		if (!data->plat_data->banks_enable[i])	/* [한국어] 쓰지 않는 뱅크는 뜰 것이 없다. */
			continue;
		base = data->bank[i].base;	/* [한국어] 이번 뱅크의 레지스터 창. */
		reg->int_control[i] = readl_relaxed(base + REG_MMU_INT_CONTROL0);	/* [한국어] 인터럽트 활성화 설정. */
		reg->int_main_control[i] = readl_relaxed(base + REG_MMU_INT_MAIN_CONTROL);	/* [한국어] 주 인터럽트 설정. */
		reg->ivrp_paddr[i] = readl_relaxed(base + REG_MMU_IVRP_PADDR);	/* [한국어] 보호 메모리 주소. */
	} while (++i < data->plat_data->banks_num);
	/* [한국어] 클럭을 내린다. 전원 도메인이 실제로 꺼지려면 필요하다. */
	clk_disable_unprepare(data->bclk);
	return 0;	/* [한국어] 상태를 떠 두는 일은 실패하지 않는다. */
}

/*
 * [한국어]
 * mtk_iommu_runtime_resume - 깨어나 레지스터를 되돌린다
 *
 * @dev: M4U 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 서스펜드의 대칭이지만 두 가지가 더 있다.
 *
 * 첫째, **첫 리줌을 구별한다.** wr_len_ctrl이 0이면 아직 한 번도
 * 저장된 적이 없다는 뜻이라, 쓰레기 값을 되쓰지 않고 클럭만 켜고
 * 돌아간다. 하드웨어 설정은 나중에 attach의 hw_init이 한다.
 *
 * 둘째, 마지막에 **전체 무효화**를 한다. 원본 주석이 밝히듯,
 * 사용자가 pm_runtime_get 전에 DMA 버퍼를 잡았다면 그동안의
 * 매핑이 무효화 없이 이뤄졌을 수 있다 — 무효화 경로가 꺼진
 * 하드웨어를 건너뛰기 때문이다. 여기서 한 번 비워 그 빚을 갚는다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PM 코어 → [mtk_iommu_runtime_resume] → mtk_iommu_tlb_flush_all()
 */
static int __maybe_unused mtk_iommu_runtime_resume(struct device *dev)
{
	/* [한국어] 대상 M4U. */
	struct mtk_iommu_data *data = dev_get_drvdata(dev);
	/* [한국어] 떠 두었던 값들. */
	struct mtk_iommu_suspend_reg *reg = &data->reg;
	/* [한국어] 뱅크에 붙어 있는 도메인(테이블 주소를 여기서 얻는다). */
	struct mtk_iommu_domain *m4u_dom;
	/* [한국어] 현재 뱅크의 레지스터 창. */
	void __iomem *base;
	/* [한국어] 결과 코드와 뱅크 순회 인덱스. */
	int ret, i = 0;

	/* [한국어] 레지스터를 만지려면 클럭부터 켜야 한다. */
	ret = clk_prepare_enable(data->bclk);
	if (ret) {	/* [한국어] 클럭을 켜지 못한 경우. */
		dev_err(data->dev, "Failed to enable clk(%d) in resume\n", ret);	/* [한국어] 레지스터를 만질 수 없음을 알린다. */
		return ret;	/* [한국어] 재개할 수 없다. */
	}

	/*
	 * Uppon first resume, only enable the clk and return, since the values of the
	 * registers are not yet set.
	 */
	/* [한국어] 아직 저장된 적이 없다 — 쓰레기 값을 되쓰지 않고
	 * 클럭만 켠 채 돌아간다. 실제 설정은 attach가 한다. */
	if (!reg->wr_len_ctrl)
		return 0;

	/* [한국어] 전역 설정을 뱅크 0에 되돌린다. */
	base = data->bank[i].base;
	writel_relaxed(reg->wr_len_ctrl, base + REG_MMU_WR_LEN_CTRL);	/* [한국어] 쓰기 조절 설정을 되돌린다. */
	writel_relaxed(reg->misc_ctrl, base + REG_MMU_MISC_CTRL);	/* [한국어] AXI 동작 방식을 되돌린다. */
	writel_relaxed(reg->dcm_dis, base + REG_MMU_DCM_DIS);	/* [한국어] 동적 클럭 관리 설정을 되돌린다. */
	writel_relaxed(reg->ctrl_reg, base + REG_MMU_CTRL_REG);	/* [한국어] 주 제어 레지스터를 되돌린다. */
	writel_relaxed(reg->vld_pa_rng, base + REG_MMU_VLD_PA_RNG);
	/* [한국어] 뱅크별 설정을 되돌린다. */
	do {
		m4u_dom = data->bank[i].m4u_dom;
		/* [한국어] 쓰지 않는 뱅크나 아직 도메인이 없는 뱅크는
		 * 되돌릴 것이 없다. */
		if (!data->plat_data->banks_enable[i] || !m4u_dom)
			continue;
		base = data->bank[i].base;	/* [한국어] 이번 뱅크의 레지스터 창. */
		writel_relaxed(reg->int_control[i], base + REG_MMU_INT_CONTROL0);	/* [한국어] 인터럽트 활성화를 되돌린다. */
		writel_relaxed(reg->int_main_control[i], base + REG_MMU_INT_MAIN_CONTROL);	/* [한국어] 주 인터럽트 설정을 되돌린다. */
		writel_relaxed(reg->ivrp_paddr[i], base + REG_MMU_IVRP_PADDR);
		/* [한국어] 테이블 주소는 저장하지 않고 도메인에서 직접
		 * 꺼낸다 — 그 사이 도메인이 바뀌었을 수도 있기 때문이다. */
		writel(m4u_dom->cfg.arm_v7s_cfg.ttbr, base + REG_MMU_PT_BASE_ADDR);
	} while (++i < data->plat_data->banks_num);

	/*
	 * Users may allocate dma buffer before they call pm_runtime_get,
	 * in which case it will lack the necessary tlb flush.
	 * Thus, make sure to update the tlb after each PM resume.
	 */
	/* [한국어] 잠든 동안의 매핑은 무효화를 건너뛰었을 수 있다 —
	 * 무효화 경로가 꺼진 하드웨어를 그냥 지나치기 때문이다.
	 * 여기서 한 번 비워 그 빚을 갚는다. */
	mtk_iommu_tlb_flush_all(data);
	return 0;	/* [한국어] 재개가 끝났다. */
}

/* [한국어] 전원 관리 콜백 묶음.
 * 시스템 절전은 **late** 단계에서 런타임 PM을 강제로 오가게 한다 —
 * 마스터들이 먼저 잠든 뒤에 IOMMU가 꺼져야 하기 때문이다. */
static const struct dev_pm_ops mtk_iommu_pm_ops = {
	SET_RUNTIME_PM_OPS(mtk_iommu_runtime_suspend, mtk_iommu_runtime_resume, NULL)	/* [한국어] 런타임 전원 전환에 위 두 함수를 연결한다(유휴 콜백은 없다). */
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
};

/* [한국어] 여기서부터 끝까지는 SoC별 설정 표다.
 * 앞의 코드가 하는 모든 분기가 이 표들의 flags 하나를 본다 —
 * 그래서 새 SoC를 더할 때 코드가 아니라 표만 늘어난다.
 *
 * 표를 읽을 때 눈여겨볼 것은 셋이다.
 *  - flags: 이 하드웨어의 성격 전부. 세대, 종류, 주소 폭, 전원 방식.
 *  - iova_region 계열: IOVA를 몇 개로 나누고 어느 마스터가 어디에
 *    속하는가. 이것이 IOMMU 그룹의 경계가 된다.
 *  - larbid_remap: 인터럽트가 준 번호를 디바이스 트리의 larb
 *    번호로 옮기는 표. 폴트 로그를 읽을 수 있게 하는 유일한 근거다. */

/* [한국어] MT2712 — 두 개의 M4U가 테이블을 공유하는 첫 SoC다
 * (SHARE_PGTABLE + m4ulist). larb 번호가 그대로 이어져 재매핑이
 * 항등이다. */
static const struct mtk_iommu_plat_data mt2712_data = {
	.m4u_plat     = M4U_MT2712,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags        = HAS_4GB_MODE | HAS_BCLK | HAS_VLD_PA_RNG | SHARE_PGTABLE |
			MTK_IOMMU_TYPE_MM,
	.hw_list      = &m4ulist,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.iova_region  = single_domain,
	.banks_num    = 1,
	.banks_enable = {true},
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}},
};

/* [한국어] MT6779 — 서브커먼 2비트 구성이 등장하고, 테이블이
 * 35비트 주소에 놓일 수 있다. larbid_remap이 항등이 아닌 것에서
 * SMI 배선이 복잡해졌음을 알 수 있다. */
static const struct mtk_iommu_plat_data mt6779_data = {
	.m4u_plat      = M4U_MT6779,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags         = HAS_SUB_COMM_2BITS | OUT_ORDER_WR_EN | WR_THROT_EN |
			 MTK_IOMMU_TYPE_MM | PGTABLE_PA_35_EN,
	.inv_sel_reg   = REG_MMU_INV_SEL_GEN2,
	.banks_num    = 1,
	.banks_enable = {true},
	.iova_region   = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap  = {{0}, {1}, {2}, {3}, {5}, {7, 8}, {10}, {9}},
};

/* [한국어] MT6795 — MT8173과 거의 같은 구형 구성이다.
 * 4GB 모드, AXI 리셋, 구형 보호 주소 형식을 모두 쓴다. */
static const struct mtk_iommu_plat_data mt6795_data = {
	.m4u_plat     = M4U_MT6795,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags	      = HAS_4GB_MODE | HAS_BCLK | RESET_AXI |
			HAS_LEGACY_IVRP_PADDR | MTK_IOMMU_TYPE_MM |
			TF_PORT_TO_ADDR_MT8173,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.banks_num    = 1,
	.banks_enable = {true},
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {1}, {2}, {3}, {4}}, /* Linear mapping. */
};

/* [한국어] MT8192 계열의 "어느 larb/포트가 어느 영역에 속하는가" 표.
 * 첫 인덱스가 영역, 둘째가 larb, 값이 포트 비트맵이다.
 * ~0은 그 larb의 모든 포트를 뜻하고, 0은 이 영역에 속하지 않음을
 * 뜻한다. 영역 4와 5가 larb 13/14의 특정 포트만 떼어 가는 것은
 * CCU처럼 좁은 주소만 쓸 수 있는 마스터를 위한 것이며, 영역 2가
 * 그 포트들을 제외(~(BIT(9)|BIT(10)))하는 것과 짝을 이룬다. */
static const unsigned int mt8192_larb_region_msk[MT8192_MULTI_REGION_NR_MAX][MTK_LARB_NR_MAX] = {
	[0] = {~0, ~0},				/* Region0: larb0/1 */	/* [한국어] 이 영역에 속하는 larb와 포트들(값이 포트 비트맵, ~0은 전부). */
	[1] = {0, 0, 0, 0, ~0, ~0, 0, ~0},	/* Region1: larb4/5/7 */
	[2] = {0, 0, ~0, 0, 0, 0, 0, 0,		/* Region2: larb2/9/11/13/14/16/17/18/19/20 */
	       0, ~0, 0, ~0, 0, ~(u32)(BIT(9) | BIT(10)), ~(u32)(BIT(4) | BIT(5)), 0,
	       ~0, ~0, ~0, ~0, ~0},
	[3] = {0},
	[4] = {[13] = BIT(9) | BIT(10)},	/* larb13 port9/10 */
	[5] = {[14] = BIT(4) | BIT(5)},		/* larb14 port4/5 */
};

/* [한국어] MT6893 — MT8192와 같은 하드웨어를 쓰되 별도 표를 둔다.
 * 34비트 IOVA와 다중 영역, 테이블 공유가 모두 켜진 구성이다. */
static const struct mtk_iommu_plat_data mt6893_data = {
	.m4u_plat     = M4U_MT8192,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags        = HAS_BCLK | OUT_ORDER_WR_EN | HAS_SUB_COMM_2BITS |
			WR_THROT_EN | IOVA_34_EN | SHARE_PGTABLE | MTK_IOMMU_TYPE_MM,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN2,
	.banks_num    = 1,
	.banks_enable = {true},
	.iova_region  = mt8192_multi_dom,
	.iova_region_nr = ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8192_larb_region_msk,
	.larbid_remap    = {{0}, {1}, {4, 5}, {7}, {2}, {9, 11, 19, 20},
			    {0, 14, 16}, {0, 13, 18, 17}},
};

/* [한국어] MT8167 — larb가 셋뿐인 작은 구성. 영역도 하나다. */
static const struct mtk_iommu_plat_data mt8167_data = {
	.m4u_plat     = M4U_MT8167,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags        = RESET_AXI | HAS_LEGACY_IVRP_PADDR | MTK_IOMMU_TYPE_MM,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.banks_num    = 1,
	.banks_enable = {true},
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {1}, {2}}, /* Linear mapping. */
};

/* [한국어] MT8173 — 이 드라이버가 처음 지원한 SoC.
 * 4GB 모드와 구형 보호 주소 형식이 여기서 유래한다. */
static const struct mtk_iommu_plat_data mt8173_data = {
	.m4u_plat     = M4U_MT8173,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags	      = HAS_4GB_MODE | HAS_BCLK | RESET_AXI |
			HAS_LEGACY_IVRP_PADDR | MTK_IOMMU_TYPE_MM |
			TF_PORT_TO_ADDR_MT8173,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.banks_num    = 1,
	.banks_enable = {true},
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {1}, {2}, {3}, {4}, {5}}, /* Linear mapping. */
};

/* [한국어] MT8183 — larb 재매핑이 완전히 뒤섞인 예다.
 * 하드웨어가 보고하는 순서와 디바이스 트리의 번호가 다르다. */
static const struct mtk_iommu_plat_data mt8183_data = {
	.m4u_plat     = M4U_MT8183,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags        = RESET_AXI | MTK_IOMMU_TYPE_MM,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.banks_num    = 1,
	.banks_enable = {true},
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {4}, {5}, {6}, {7}, {2}, {3}, {1}},
};

/* [한국어] MT8186의 영역-larb 대응표. MT8192와 같은 구조이며,
 * 어느 larb가 어느 영역에 들어가는지만 다르다. */
static const unsigned int mt8186_larb_region_msk[MT8192_MULTI_REGION_NR_MAX][MTK_LARB_NR_MAX] = {
	[0] = {~0, ~0, ~0},			/* Region0: all ports for larb0/1/2 */	/* [한국어] 이 영역에 속하는 larb와 포트들(값이 포트 비트맵, ~0은 전부). */
	[1] = {0, 0, 0, 0, ~0, 0, 0, ~0},	/* Region1: larb4/7 */
	[2] = {0, 0, 0, 0, 0, 0, 0, 0,		/* Region2: larb8/9/11/13/16/17/19/20 */
	       ~0, ~0, 0, ~0, 0, ~(u32)(BIT(9) | BIT(10)), 0, 0,
						/* larb13: the other ports except port9/10 */
	       ~0, ~0, 0, ~0, ~0},
	[3] = {0},
	[4] = {[13] = BIT(9) | BIT(10)},	/* larb13 port9/10 */
	[5] = {[14] = ~0},			/* larb14 */
};

/* [한국어] MT8186의 멀티미디어 IOMMU.
 * larbid_remap에 MTK_INVALID_LARBID가 섞여 있는 것은 그 자리에
 * 배선된 larb가 없다는 뜻이다 — 표의 빈칸을 명시적으로 채운 것이다. */
static const struct mtk_iommu_plat_data mt8186_data_mm = {
	.m4u_plat       = M4U_MT8186,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags          = HAS_BCLK | HAS_SUB_COMM_2BITS | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN | MTK_IOMMU_TYPE_MM | PGTABLE_PA_35_EN,
	.larbid_remap   = {{0}, {1, MTK_INVALID_LARBID, 8}, {4}, {7}, {2}, {9, 11, 19, 20},
			   {MTK_INVALID_LARBID, 14, 16},
			   {MTK_INVALID_LARBID, 13, MTK_INVALID_LARBID, 17}},
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.banks_num      = 1,
	.banks_enable   = {true},
	.iova_region    = mt8192_multi_dom,
	.iova_region_nr = ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8186_larb_region_msk,
};

/* [한국어] MT8188의 인프라 IOMMU.
 * PM_CLK_AO(전원 항상 켜짐)와 CFG_IFA_MASTER_IN_ATF(포트 설정을
 * 보안 모니터에 위임)가 함께 있어, 무효화가 전원 확인을 건너뛰고
 * 포트 켜기가 SMC 호출로 나간다. */
static const struct mtk_iommu_plat_data mt8188_data_infra = {
	.m4u_plat         = M4U_MT8188,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags            = WR_THROT_EN | DCM_DISABLE | STD_AXI_MODE | PM_CLK_AO |
			    MTK_IOMMU_TYPE_INFRA | IFA_IOMMU_PCIE_SUPPORT |
			    PGTABLE_PA_35_EN | CFG_IFA_MASTER_IN_ATF,
	.inv_sel_reg      = REG_MMU_INV_SEL_GEN2,
	.banks_num        = 1,
	.banks_enable     = {true},
	.iova_region      = single_domain,
	.iova_region_nr   = ARRAY_SIZE(single_domain),
};

/* [한국어] MT8188의 영역-larb 대응표.
 * 주석의 괄호 안 숫자가 흥미로운데(예: larb19(21)), 앞이 하드웨어
 * 번호이고 뒤가 이 표에서의 인덱스다 — 둘이 다른 것이
 * larbid_remap이 필요한 이유이기도 하다. */
static const u32 mt8188_larb_region_msk[MT8192_MULTI_REGION_NR_MAX][MTK_LARB_NR_MAX] = {
	[0] = {~0, ~0, ~0, ~0},               /* Region0: all ports for larb0/1/2/3 */	/* [한국어] 이 영역에 속하는 larb와 포트들(값이 포트 비트맵, ~0은 전부). */
	[1] = {0, 0, 0, 0, 0, 0, 0, 0,
	       0, 0, 0, 0, 0, 0, 0, 0,
	       0, 0, 0, 0, 0, ~0, ~0, ~0},    /* Region1: larb19(21)/21(22)/23 */
	[2] = {0, 0, 0, 0, ~0, ~0, ~0, ~0,    /* Region2: the other larbs. */
	       ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0,
	       ~0, ~0, ~0, ~0, ~0, 0, 0, 0,
	       0, ~0},
	[3] = {0},
	[4] = {[24] = BIT(0) | BIT(1)},       /* Only larb27(24) port0/1 */
	[5] = {[24] = BIT(2) | BIT(3)},       /* Only larb27(24) port2/3 */
};

/* [한국어] MT8188의 비디오 출력(VDO) IOMMU.
 * 아래 VPP와 테이블을 공유한다 — 둘 다 m4ulist에 들어가고
 * SHARE_PGTABLE이 켜져 있다. 담당하는 larb만 서로 다르다. */
static const struct mtk_iommu_plat_data mt8188_data_vdo = {
	.m4u_plat       = M4U_MT8188,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags          = HAS_BCLK | HAS_SUB_COMM_3BITS | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN | SHARE_PGTABLE |
			  PGTABLE_PA_35_EN | MTK_IOMMU_TYPE_MM,
	.hw_list        = &m4ulist,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.banks_num      = 1,
	.banks_enable   = {true},
	.iova_region    = mt8192_multi_dom,
	.iova_region_nr = ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8188_larb_region_msk,
	.larbid_remap   = {{2}, {0}, {21}, {0}, {19}, {9, 10,
			   11 /* 11a */, 25 /* 11c */},
			   {13, 0, 29 /* 16b */, 30 /* 17b */, 0}, {5}},
};

/* [한국어] MT8188의 비디오 처리(VPP) IOMMU.
 * 위 VDO와 짝을 이루며 같은 페이지 테이블을 쓴다. */
static const struct mtk_iommu_plat_data mt8188_data_vpp = {
	.m4u_plat       = M4U_MT8188,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags          = HAS_BCLK | HAS_SUB_COMM_3BITS | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN | SHARE_PGTABLE |
			  PGTABLE_PA_35_EN | MTK_IOMMU_TYPE_MM,
	.hw_list        = &m4ulist,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.banks_num      = 1,
	.banks_enable   = {true},
	.iova_region    = mt8192_multi_dom,
	.iova_region_nr = ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8188_larb_region_msk,
	.larbid_remap   = {{1}, {3}, {23}, {7}, {MTK_INVALID_LARBID},
			   {12, 15, 24 /* 11b */}, {14, MTK_INVALID_LARBID,
			   16 /* 16a */, 17 /* 17a */, MTK_INVALID_LARBID,
			   27, 28 /* ccu0 */, MTK_INVALID_LARBID}, {4, 6}},
};

/* [한국어] MT8189 APU의 영역 대응표.
 * "가짜 larb 0"이라는 주석이 요점이다 — APU에는 SMI larb가 없지만,
 * 영역을 고르는 코드가 larb/포트 형식을 전제하므로 포트 번호만
 * 쓰는 가상의 larb 하나로 표현한다. */
static const unsigned int mt8189_apu_region_msk[][MTK_LARB_NR_MAX] = {
	[0] = {[0] = BIT(2)},	/* Region0: fake larb 0 APU_SECURE */	/* [한국어] 이 영역에 속하는 larb와 포트들(값이 포트 비트맵, ~0은 전부). */
	[1] = {[0] = BIT(1)},	/* Region1: fake larb 0 APU_CODE */
	[2] = {[0] = BIT(3)},	/* Region2: fake larb 0 APU_VLM */
	[3] = {[0] = BIT(0)},	/* Region3: fake larb 0 APU_DATA */
};

/* [한국어] MT8189의 AI 가속기(APU) IOMMU.
 * 용도별로 크기가 다른 네 영역을 쓰고, apulist를 자기 목록으로
 * 삼아 멀티미디어 쪽과 섞이지 않는다. */
static const struct mtk_iommu_plat_data mt8189_data_apu = {
	.m4u_plat       = M4U_MT8189,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags          = IOVA_34_EN | DCM_DISABLE |
			  MTK_IOMMU_TYPE_APU | PGTABLE_PA_35_EN,
	.hw_list        = &apulist,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.banks_num	= 1,
	.banks_enable	= {true},
	.iova_region	= mt8189_multi_dom_apu,
	.iova_region_nr	= ARRAY_SIZE(mt8189_multi_dom_apu),
	.larbid_remap   = {{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}},
	.iova_region_larb_msk = mt8189_apu_region_msk,
};

/* [한국어] MT8189의 인프라 IOMMU. 포트 설정을 ATF에 맡기고
 * 테이블은 infralist 안에서 공유한다. */
static const struct mtk_iommu_plat_data mt8189_data_infra = {
	.m4u_plat	= M4U_MT8189,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags		= WR_THROT_EN | DCM_DISABLE | MTK_IOMMU_TYPE_INFRA |
			  CFG_IFA_MASTER_IN_ATF | SHARE_PGTABLE | PGTABLE_PA_35_EN,
	.hw_list	= &infralist,
	.banks_num	= 1,
	.banks_enable	= {true},
	.inv_sel_reg	= REG_MMU_INV_SEL_GEN2,
	.iova_region	= single_domain,
	.iova_region_nr	= ARRAY_SIZE(single_domain),
};

/* [한국어] MT8189의 영역-larb 대응표.
 * 지정 초기화([3] = ~0 형태)를 쓴 덕분에 빈 자리를 일일이
 * 0으로 채우지 않아 앞의 표들보다 읽기 쉽다. */
static const u32 mt8189_larb_region_msk[MT8192_MULTI_REGION_NR_MAX][MTK_LARB_NR_MAX] = {
	[0] = {~0, ~0, ~0, [22] = BIT(0)},	/* Region0: all ports for larb0/1/2 */	/* [한국어] 이 영역에 속하는 larb와 포트들(값이 포트 비트맵, ~0은 전부). */
	[1] = {[3] = ~0, [4] = ~0},		/* Region1: all ports for larb4(3)/7(4) */
	[2] = {[5] = ~0, [6] = ~0,		/* Region2: all ports for larb9(5)/11(6) */
	       [7] = ~0, [8] = ~0,		/* Region2: all ports for larb13(7)/14(8) */
	       [9] = ~0, [10] = ~0,		/* Region2: all ports for larb16(9)/17(10) */
	       [11] = ~0, [12] = ~0,		/* Region2: all ports for larb19(11)/20(12) */
	       [21] = ~0},			/* Region2: larb21 fake GCE larb */
};

/* [한국어] MT8189의 멀티미디어 IOMMU.
 * banks_num이 5인데 banks_enable은 0번만 참인 점에 유의 —
 * 하드웨어에 뱅크가 다섯 있어도 이 SoC에서는 하나만 쓴다.
 * DL_WITH_MULTI_LARB가 켜져 있어 한 마스터가 여러 larb에
 * 걸칠 수 있다(GCE가 그렇다). */
static const struct mtk_iommu_plat_data mt8189_data_mm = {
	.m4u_plat	= M4U_MT8189,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags		= HAS_BCLK | HAS_SUB_COMM_3BITS | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN | MTK_IOMMU_TYPE_MM |
			  PGTABLE_PA_35_EN | DL_WITH_MULTI_LARB,
	.hw_list	= &m4ulist,
	.inv_sel_reg	= REG_MMU_INV_SEL_GEN2,
	.banks_num	= 5,
	.banks_enable	= {true, false, false, false, false},
	.iova_region	= mt8192_multi_dom,
	.iova_region_nr	= ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8189_larb_region_msk,
	.larbid_remap	= {{0}, {1}, {21/* GCE_D */, 21/* GCE_M */, 2},
			   {19, 20, 9, 11}, {7}, {4},
			   {13, 17}, {14, 16}},
};

/* [한국어] MT8192 — 34비트 IOVA와 다중 영역의 기준이 되는 SoC.
 * 위 MT6893이 같은 구성을 물려받는다. */
static const struct mtk_iommu_plat_data mt8192_data = {
	.m4u_plat       = M4U_MT8192,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags          = HAS_BCLK | HAS_SUB_COMM_2BITS | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN | MTK_IOMMU_TYPE_MM,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.banks_num      = 1,
	.banks_enable   = {true},
	.iova_region    = mt8192_multi_dom,
	.iova_region_nr = ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8192_larb_region_msk,
	.larbid_remap   = {{0}, {1}, {4, 5}, {7}, {2}, {9, 11, 19, 20},
			   {0, 14, 16}, {0, 13, 18, 17}},
};

/* [한국어] MT8195의 인프라 IOMMU.
 * **뱅크를 실제로 쓰는 유일한 예**다: 0번은 PCIe, 4번은 USB가
 * 쓰도록 포트 비트맵으로 나뉘어 있다. 뱅크가 다르면 페이지
 * 테이블도 다르므로, PCIe와 USB가 서로 완전히 격리된다. */
static const struct mtk_iommu_plat_data mt8195_data_infra = {
	.m4u_plat	  = M4U_MT8195,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags            = WR_THROT_EN | DCM_DISABLE | STD_AXI_MODE | PM_CLK_AO |
			    MTK_IOMMU_TYPE_INFRA | IFA_IOMMU_PCIE_SUPPORT,
	.pericfg_comp_str = "mediatek,mt8195-pericfg_ao",
	.inv_sel_reg      = REG_MMU_INV_SEL_GEN2,
	.banks_num	  = 5,
	.banks_enable     = {true, false, false, false, true},
	.banks_portmsk    = {[0] = GENMASK(19, 16),     /* PCIe */
			     [4] = GENMASK(31, 20),     /* USB */
			    },
	.iova_region      = single_domain,
	.iova_region_nr   = ARRAY_SIZE(single_domain),
};

/* [한국어] MT8195의 영역-larb 대응표. larb가 29개까지 있어
 * 표가 가장 길다. */
static const unsigned int mt8195_larb_region_msk[MT8192_MULTI_REGION_NR_MAX][MTK_LARB_NR_MAX] = {
	[0] = {~0, ~0, ~0, ~0},               /* Region0: all ports for larb0/1/2/3 */	/* [한국어] 이 영역에 속하는 larb와 포트들(값이 포트 비트맵, ~0은 전부). */
	[1] = {0, 0, 0, 0, 0, 0, 0, 0,
	       0, 0, 0, 0, 0, 0, 0, 0,
	       0, 0, 0, ~0, ~0, ~0, ~0, ~0,   /* Region1: larb19/20/21/22/23/24 */
	       ~0},
	[2] = {0, 0, 0, 0, ~0, ~0, ~0, ~0,    /* Region2: the other larbs. */
	       ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0,
	       ~0, ~0, 0, 0, 0, 0, 0, 0,
	       0, ~0, ~0, ~0, ~0},
	[3] = {0},
	[4] = {[18] = BIT(0) | BIT(1)},       /* Only larb18 port0/1 */
	[5] = {[18] = BIT(2) | BIT(3)},       /* Only larb18 port2/3 */
};

/* [한국어] MT8195의 비디오 출력 IOMMU. 아래 VPP와 테이블을 공유한다. */
static const struct mtk_iommu_plat_data mt8195_data_vdo = {
	.m4u_plat	= M4U_MT8195,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags          = HAS_BCLK | HAS_SUB_COMM_2BITS | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN | SHARE_PGTABLE | MTK_IOMMU_TYPE_MM,
	.hw_list        = &m4ulist,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.banks_num      = 1,
	.banks_enable   = {true},
	.iova_region	= mt8192_multi_dom,
	.iova_region_nr	= ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8195_larb_region_msk,
	.larbid_remap   = {{2, 0}, {21}, {24}, {7}, {19}, {9, 10, 11},
			   {13, 17, 15/* 17b */, 25}, {5}},
};

/* [한국어] MT8195의 비디오 처리 IOMMU.
 * larbid_remap의 주석이 하드웨어 larb 이름(16a, 16b, CCUtop)을
 * 알려 주는데, 같은 번호의 larb가 용도별로 나뉘어 있어
 * 소프트웨어 번호와 일대일로 대응하지 않는다. */
static const struct mtk_iommu_plat_data mt8195_data_vpp = {
	.m4u_plat	= M4U_MT8195,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags          = HAS_BCLK | HAS_SUB_COMM_3BITS | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN | SHARE_PGTABLE | MTK_IOMMU_TYPE_MM,
	.hw_list        = &m4ulist,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.banks_num      = 1,
	.banks_enable   = {true},
	.iova_region	= mt8192_multi_dom,
	.iova_region_nr	= ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt8195_larb_region_msk,
	.larbid_remap   = {{1}, {3},
			   {22, MTK_INVALID_LARBID, MTK_INVALID_LARBID, MTK_INVALID_LARBID, 23},
			   {8}, {20}, {12},
			   /* 16: 16a; 29: 16b; 30: CCUtop0; 31: CCUtop1 */
			   {14, 16, 29, 26, 30, 31, 18},
			   {4, MTK_INVALID_LARBID, MTK_INVALID_LARBID, MTK_INVALID_LARBID, 6}},
};

/* [한국어] MT8365 — 인터럽트 ID의 포트 필드가 6비트인 유일한 예다
 * (INT_ID_PORT_WIDTH_6). 그래서 폴트 핸들러의 네 갈래 중
 * 세 번째 갈래를 타는 것은 이 SoC뿐이다. */
static const struct mtk_iommu_plat_data mt8365_data = {
	.m4u_plat	= M4U_MT8365,	/* [한국어] 어느 SoC인가 — 4GB 모드의 구형 infracfg 조회에만 쓰인다. */
	.flags		= RESET_AXI | INT_ID_PORT_WIDTH_6,
	.inv_sel_reg	= REG_MMU_INV_SEL_GEN1,
	.banks_num	= 1,
	.banks_enable	= {true},
	.iova_region	= single_domain,
	.iova_region_nr	= ARRAY_SIZE(single_domain),
	.larbid_remap	= {{0}, {1}, {2}, {3}, {4}, {5}}, /* Linear mapping. */
};

/* [한국어] 디바이스 트리 호환 문자열과 설정 표의 대응.
 * 하나의 SoC가 여러 항목을 갖는 것에 유의(예: mt8188의 infra/vdo/vpp) —
 * 한 SoC 안에 성격이 다른 IOMMU가 여럿 있고, 각각이 별도의
 * 디바이스로 등록되기 때문이다. */
static const struct of_device_id mtk_iommu_of_ids[] = {
	{ .compatible = "mediatek,mt2712-m4u", .data = &mt2712_data},	/* [한국어] 이 호환 문자열의 노드에 위 설정 표를 붙인다. */
	{ .compatible = "mediatek,mt6779-m4u", .data = &mt6779_data},
	{ .compatible = "mediatek,mt6795-m4u", .data = &mt6795_data},
	{ .compatible = "mediatek,mt6893-iommu-mm", .data = &mt6893_data},
	{ .compatible = "mediatek,mt8167-m4u", .data = &mt8167_data},
	{ .compatible = "mediatek,mt8173-m4u", .data = &mt8173_data},
	{ .compatible = "mediatek,mt8183-m4u", .data = &mt8183_data},
	{ .compatible = "mediatek,mt8186-iommu-mm",    .data = &mt8186_data_mm}, /* mm: m4u */
	{ .compatible = "mediatek,mt8188-iommu-infra", .data = &mt8188_data_infra},
	{ .compatible = "mediatek,mt8188-iommu-vdo",   .data = &mt8188_data_vdo},
	{ .compatible = "mediatek,mt8188-iommu-vpp",   .data = &mt8188_data_vpp},
	{ .compatible = "mediatek,mt8189-iommu-apu",   .data = &mt8189_data_apu},
	{ .compatible = "mediatek,mt8189-iommu-infra", .data = &mt8189_data_infra},
	{ .compatible = "mediatek,mt8189-iommu-mm",    .data = &mt8189_data_mm},
	{ .compatible = "mediatek,mt8192-m4u", .data = &mt8192_data},
	{ .compatible = "mediatek,mt8195-iommu-infra", .data = &mt8195_data_infra},
	{ .compatible = "mediatek,mt8195-iommu-vdo",   .data = &mt8195_data_vdo},
	{ .compatible = "mediatek,mt8195-iommu-vpp",   .data = &mt8195_data_vpp},
	{ .compatible = "mediatek,mt8365-m4u", .data = &mt8365_data},
	{}
};
MODULE_DEVICE_TABLE(of, mtk_iommu_of_ids);

/* [한국어] 플랫폼 드라이버 서술자. */
static struct platform_driver mtk_iommu_driver = {
	.probe	= mtk_iommu_probe,	/* [한국어] 하드웨어 초기화 진입점. */
	.remove = mtk_iommu_remove,
	.driver	= {
		.name = "mtk-iommu",	/* [한국어] 드라이버 이름. */
		.of_match_table = mtk_iommu_of_ids,
		.pm = &mtk_iommu_pm_ops,
	}
};
module_platform_driver(mtk_iommu_driver);	/* [한국어] 모듈 init/exit 상용구를 생성한다. */

MODULE_DESCRIPTION("IOMMU API for MediaTek M4U implementations");	/* [한국어] modinfo로 보이는 설명. */
MODULE_LICENSE("GPL v2");	/* [한국어] 라이선스. GPL 심볼을 쓰기 위해 필요하다. */
