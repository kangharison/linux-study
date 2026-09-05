// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
// Copyright (C) 2016-2018, Allwinner Technology CO., LTD.
// Copyright (C) 2019-2020, Cerno

/*
 * [한국어 설명] Allwinner H6/H616 IOMMU 드라이버 (sun50i-iommu.c)
 *
 * === 파일의 역할 ===
 * Allwinner의 sun50i 계열 SoC(H6, H616)에 들어 있는 IOMMU를 리눅스
 * IOMMU 서브시스템에 붙이는 드라이버다. 이 하드웨어는 디스플레이 엔진,
 * 비디오 디코더 같은 멀티미디어 마스터 여섯 개의 DMA를 관리한다.
 *
 * 구조를 이해하는 데 필요한 개념이 셋이다.
 *
 * (1) **2단계 페이지 테이블**. 1단계 DT(Directory Table)는 4096개의 DTE로
 *     이루어져 IOVA의 비트 20~31을 소비하고, 각 DTE가 256개 PTE짜리
 *     PT(Page Table)를 가리켜 비트 12~19를 소비한다. 나머지 12비트가
 *     페이지 내 오프셋이다. 합쳐서 32비트 IOVA 공간을 정확히 덮는다.
 *     이 하드웨어는 DT를 하나만 가질 수 있어(IOMMU_TTB_REG가 하나뿐),
 *     **도메인도 사실상 하나뿐**이다. 여러 디바이스가 붙으면 같은 도메인을
 *     공유하며, refcount로 마지막 detach를 감지한다.
 *
 * (2) **ACI(Authority Control Index)를 통한 권한**. PTE에 권한 비트가
 *     직접 들어 있지 않다. 대신 4비트 ACI 필드가 하드웨어의 16개 "권한
 *     도메인" 중 하나를 가리키고, 그 도메인의 마스터별 읽기/쓰기 허용
 *     여부가 IOMMU_DM_AUT_CTRL_REG에 설정되어 있다. 이 드라이버는
 *     ACI 1~4를 NONE/RD/WR/RD_WR로 미리 구성해 두고, 매핑을 만들 때
 *     요청된 권한에 맞는 ACI를 PTE에 넣는 방식으로 권한을 표현한다.
 *     하드웨어의 간접 방식을 IOMMU API의 직접 방식에 끼워 맞춘 셈이다.
 *
 * (3) **무효화의 특이한 범위**. TLB와 PTW(Page Table Walk) 캐시를 각각
 *     무효화해야 하고, 하드웨어의 프리페치 때문에 요청 범위보다 한 칸씩
 *     더 넓게 비워야 한다(sun50i_iommu_zap_range 참조).
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [멀티미디어 드라이버] dma_map_*() / iommu_map()
 *        ↓
 *   [IOMMU 코어] iommu_ops 디스패치
 *        ↓
 *   [이 파일] sun50i_iommu_map() → DTE 조회(없으면 PT 할당) → PTE 기록
 *        ↓ iotlb_sync_map
 *   [이 파일] sun50i_iommu_zap_range() → TLB/PTW 캐시 무효화
 *        ↓
 *   [sun50i IOMMU 하드웨어] TTB_REG가 가리키는 DT부터 워크
 *
 * 도메인 수명이 독특하다: domain_alloc은 DT만 만들고, 첫 attach가
 * sun50i_iommu_attach_domain()으로 그것을 하드웨어에 등록하며 IOMMU를
 * 켠다. 마지막 detach(= identity 도메인으로 전환)가 refcount를 0으로
 * 만들면 모든 PT를 해제하고 IOMMU를 끈다.
 *
 * 실행 컨텍스트: map/unmap은 프로세스 컨텍스트(테이블 메모리만 만진다),
 * 무효화와 IOMMU 활성화는 iommu_lock을 irqsave로 잡는다. 폴트 핸들러는
 * 하드 인터럽트 컨텍스트다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu-pages.h: iommu_alloc_pages_sz()/iommu_free_pages(). 1단계 DT를
 *   16KB 크기로 받는 데 쓴다.
 * - linux/dma-mapping.h: 테이블을 하드웨어에 보이게 하는 dma_map_single과
 *   갱신을 반영하는 dma_sync_single_for_device.
 * - linux/reset.h, linux/clk.h: IOMMU를 켜려면 리셋을 풀고 클록을 켜야 한다.
 * - linux/iopoll.h: 무효화 완료를 폴링하는 readl_poll_timeout_atomic.
 * 데이터 흐름: 디바이스 트리의 `iommus = <&iommu master_id>` → of_xlate가
 * IOMMU를 priv에 심고 마스터 ID를 fwspec에 넣는다 → attach가 DT를
 * 하드웨어에 등록하고 IOMMU를 켠다 → 이후 map/unmap이 테이블을 갱신.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct sun50i_iommu: IOMMU 인스턴스. 레지스터 베이스, 클록/리셋,
 *   현재 도메인, 그리고 2단계 PT용 슬랩 캐시.
 * - struct sun50i_iommu_domain: 유일한 페이징 도메인. DT와 그 DMA 주소,
 *   그리고 붙어 있는 디바이스 수를 세는 refcount.
 * - sun50i_iommu_enable(): 리셋 해제 → 클록 → TTB 등록 → 권한 도메인
 *   구성 → TLB 플러시 → 활성화. 이 순서가 곧 하드웨어 기동 절차다.
 * - sun50i_dte_get_page_table(): DTE가 비어 있으면 PT를 할당하고
 *   cmpxchg로 설치한다(경쟁에서 지면 자기 것을 버린다).
 * - sun50i_iommu_zap_range(): 프리페치를 고려해 요청보다 넓게 무효화한다.
 * - sun50i_iommu_irq(): L1/L2 페이지 폴트와 권한 폴트를 구분해 보고하고,
 *   문제를 일으킨 마스터를 리셋한다.
 * - sun50i_mk_pte(): 요청된 권한을 ACI 값으로 변환해 PTE에 심는다.
 */

/* [한국어] FIELD_GET()/FIELD_PREP() — IOVA에서 인덱스를 뽑고 PTE에
 * ACI를 심는 데 쓴다. 이 파일의 비트 조작이 대부분 이 매크로를 거친다. */
#include <linux/bitfield.h>
/* [한국어] WARN_ON() 등 기본 진단 매크로. */
#include <linux/bug.h>
/* [한국어] clk_prepare_enable() — IOMMU를 켜려면 클록이 필요하다. */
#include <linux/clk.h>
/* [한국어] struct device와 dev_err/dev_iommu_priv_* 접근자. */
#include <linux/device.h>
/* [한국어] DMA_TO_DEVICE 등 전송 방향 상수. 페이지 테이블은 CPU가 쓰고
 * 하드웨어가 읽으므로 항상 이 방향이다. */
#include <linux/dma-direction.h>
/* [한국어] dma_map_single()/dma_sync_single_for_device() — 테이블 메모리를
 * 하드웨어에 보이게 유지한다. */
#include <linux/dma-mapping.h>
/* [한국어] IS_ERR/PTR_ERR/ERR_PTR — PT 할당이 오류 포인터를 반환한다. */
#include <linux/err.h>
/* [한국어] -ENOMEM, -EBUSY 등 오류 코드. */
#include <linux/errno.h>
/* [한국어] devm_request_irq()와 irqreturn_t — 폴트 인터럽트 처리. */
#include <linux/interrupt.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, report_iommu_fault 등. */
#include <linux/iommu.h>
/* [한국어] readl_poll_timeout_atomic() — 무효화 완료를 바쁘게 기다린다. */
#include <linux/iopoll.h>
/* [한국어] IORESOURCE_MEM 등 자원 정의. */
#include <linux/ioport.h>
/* [한국어] ilog2() — 폴트 핸들러가 마스터 비트맵에서 마스터 번호를 뽑는 데 쓴다. */
#include <linux/log2.h>
/* [한국어] MODULE_* 매크로. */
#include <linux/module.h>
/* [한국어] of_find_device_by_node() — of_xlate가 phandle에서 IOMMU
 * 플랫폼 디바이스를 역추적한다. */
#include <linux/of_platform.h>
/* [한국어] platform_driver/platform_device 정의. */
#include <linux/platform_device.h>
/* [한국어] 전원 관리 기본 정의. */
#include <linux/pm.h>
/* [한국어] 런타임 PM API. 현재 코드에서 직접 쓰이지는 않는다. */
#include <linux/pm_runtime.h>
/* [한국어] reset_control_deassert()/assert() — IOMMU는 리셋 상태에서
 * 시작하므로 쓰기 전에 풀어야 한다. */
#include <linux/reset.h>
/* [한국어] SZ_4K, SZ_1M — 페이지 크기와 PTW 캐시 무효화 간격. */
#include <linux/sizes.h>
/* [한국어] kmem_cache_create() 등 — 2단계 PT 전용 캐시를 만든다. */
#include <linux/slab.h>
/* [한국어] spinlock_t — 레지스터 접근을 직렬화한다. */
#include <linux/spinlock.h>
/* [한국어] u32, phys_addr_t 등 기본 타입. */
#include <linux/types.h>

/* [한국어] iommu_alloc_pages_sz()/iommu_free_pages() — 1단계 DT(16KB)를
 * 크기 지정해 받는 페이지 테이블 전용 할당자. */
#include "iommu-pages.h"

/* [한국어] 마스터 리셋 레지스터. 폴트를 일으킨 마스터를 리셋해
 * 멈춘 상태에서 빠져나오게 한다. */
#define IOMMU_RESET_REG			0x010
/* [한국어] 모든 마스터의 리셋을 해제하는 값(전 비트 1).
 * 리셋은 "0을 쓰면 리셋, 1을 쓰면 해제"인 역논리다. */
#define IOMMU_RESET_RELEASE_ALL			0xffffffff
/* [한국어] IOMMU 활성화 레지스터. */
#define IOMMU_ENABLE_REG		0x020
/* [한국어] 활성화 비트. 이 비트가 서야 주소 변환이 일어난다. */
#define IOMMU_ENABLE_ENABLE			BIT(0)

/* [한국어] 바이패스 레지스터. 비트가 선 마스터는 변환 없이 통과한다.
 * enable에서 0을 써 모든 마스터가 변환을 거치게 한다. */
#define IOMMU_BYPASS_REG		0x030
/* [한국어] 자동 클록 게이팅 레지스터. 유휴 시 내부 클록을 멈춰 전력을 아낀다. */
#define IOMMU_AUTO_GATING_REG		0x040
/* [한국어] 자동 게이팅 활성화 비트. 무효화 중에는 이것을 꺼야 하는데,
 * 게이팅된 상태에서는 무효화 명령이 처리되지 않기 때문이다. */
#define IOMMU_AUTO_GATING_ENABLE		BIT(0)

/* [한국어] 쓰기 버퍼 제어 레지스터. 이 드라이버는 설정하지 않고
 * 하드웨어 기본값에 맡긴다. */
#define IOMMU_WBUF_CTRL_REG		0x044
/* [한국어] 순서 없는(out-of-order) 처리 제어. 역시 기본값을 쓴다. */
#define IOMMU_OOO_CTRL_REG		0x048
/* [한국어] 4KB 경계 보호 제어. 역시 기본값을 쓴다. */
#define IOMMU_4KB_BDY_PRT_CTRL_REG	0x04c
/* [한국어] TTB(Translation Table Base) 레지스터 — 1단계 DT의 물리 주소.
 * 이 레지스터가 하나뿐이라 도메인도 하나뿐이라는 제약이 생긴다. */
#define IOMMU_TTB_REG			0x050
/* [한국어] TLB 활성화 레지스터. 이 드라이버는 건드리지 않는다. */
#define IOMMU_TLB_ENABLE_REG		0x060
/* [한국어] TLB 프리페치 제어 레지스터. */
#define IOMMU_TLB_PREFETCH_REG		0x070
/* [한국어] 마스터 m의 프리페치를 켜는 비트. enable에서 여섯 마스터를
 * 모두 켜는데, 이 프리페치 때문에 무효화 범위를 넓혀야 한다. */
#define IOMMU_TLB_PREFETCH_MASTER_ENABLE(m)	BIT(m)

/* [한국어] TLB 전체 플러시 레지스터. 비트를 세워 명령을 내고,
 * 하드웨어가 완료하면 스스로 0으로 되돌린다 — 그래서 폴링 대상이다. */
#define IOMMU_TLB_FLUSH_REG		0x080
/* [한국어] PTW(Page Table Walk) 캐시를 비우는 비트. TLB와 별개로
 * 중간 단계(DTE) 조회 결과를 캐시하는 구조가 있어 따로 비워야 한다. */
#define IOMMU_TLB_FLUSH_PTW_CACHE		BIT(17)
/* [한국어] 매크로 TLB(공용 상위 TLB)를 비우는 비트. */
#define IOMMU_TLB_FLUSH_MACRO_TLB		BIT(16)
/* [한국어] 마스터 i 전용 마이크로 TLB를 비우는 비트.
 * GENMASK(5,0)로 한 번 더 마스킹하는 것은 i가 6 이상이어도 엉뚱한
 * 비트를 건드리지 않게 하려는 방어다. */
#define IOMMU_TLB_FLUSH_MICRO_TLB(i)		(BIT(i) & GENMASK(5, 0))

/* [한국어] 범위 TLB 무효화의 대상 주소 레지스터. */
#define IOMMU_TLB_IVLD_ADDR_REG		0x090
/* [한국어] 그 주소에 적용할 마스크. GENMASK(31,12)를 써서 페이지 단위로
 * 정확히 하나만 무효화한다. */
#define IOMMU_TLB_IVLD_ADDR_MASK_REG	0x094
/* [한국어] 범위 무효화 실행 레지스터. */
#define IOMMU_TLB_IVLD_ENABLE_REG	0x098
/* [한국어] 실행 비트. 1을 쓰면 무효화가 시작되고, 완료되면 하드웨어가
 * 0으로 되돌린다 — 이 값이 0이 될 때까지 폴링한다. */
#define IOMMU_TLB_IVLD_ENABLE_ENABLE		BIT(0)

/* [한국어] PTW 캐시 무효화의 대상 주소 레지스터. */
#define IOMMU_PC_IVLD_ADDR_REG		0x0a0
/* [한국어] PTW 캐시 무효화 실행 레지스터. */
#define IOMMU_PC_IVLD_ENABLE_REG	0x0a8
/* [한국어] 실행 비트. TLB 쪽과 같은 방식으로 폴링한다. */
#define IOMMU_PC_IVLD_ENABLE_ENABLE		BIT(0)

/* [한국어] 권한 도메인 d의 제어 레지스터 오프셋.
 * 레지스터 하나가 도메인 두 개를 담당하므로 d/2로 인덱싱한다. */
#define IOMMU_DM_AUT_CTRL_REG(d)	(0x0b0 + ((d) / 2) * 4)
/* [한국어] 도메인 d에서 마스터 m의 읽기를 금지하는 비트.
 * 홀수 도메인은 상위 16비트를 쓰므로 (d&1)*16이 붙고,
 * 마스터마다 읽기/쓰기 두 비트를 차지하므로 m*2가 붙는다.
 * "금지" 의미라 비트를 세우지 않으면 허용이다. */
#define IOMMU_DM_AUT_CTRL_RD_UNAVAIL(d, m)	(1 << (((d & 1) * 16) + ((m) * 2)))
/* [한국어] 도메인 d에서 마스터 m의 쓰기를 금지하는 비트.
 * 읽기 비트 바로 옆(+1)에 있다. */
#define IOMMU_DM_AUT_CTRL_WR_UNAVAIL(d, m)	(1 << (((d & 1) * 16) + ((m) * 2) + 1))

/* [한국어] 권한 도메인 덮어쓰기 레지스터. 이 드라이버는 쓰지 않는다. */
#define IOMMU_DM_AUT_OVWT_REG		0x0d0
/* [한국어] 인터럽트 활성화 레지스터 — 어떤 폴트에 인터럽트를 낼지. */
#define IOMMU_INT_ENABLE_REG		0x100
/* [한국어] 인터럽트 클리어 레지스터. 처리한 상태 비트를 여기 써서 지운다. */
#define IOMMU_INT_CLR_REG		0x104
/* [한국어] 인터럽트 상태 레지스터. 어떤 폴트가 났고 어느 마스터가
 * 원인인지 알려 준다. */
#define IOMMU_INT_STA_REG		0x108
/* [한국어] 마스터 i가 일으킨 폴트의 주소 레지스터. 마스터마다 하나씩 있다. */
#define IOMMU_INT_ERR_ADDR_REG(i)	(0x110 + (i) * 4)
/* [한국어] L1(DTE) 폴트의 주소 레지스터 — 마스터와 무관하게 하나다. */
#define IOMMU_INT_ERR_ADDR_L1_REG	0x130
/* [한국어] L2(PTE) 폴트의 주소 레지스터. */
#define IOMMU_INT_ERR_ADDR_L2_REG	0x134
/* [한국어] 마스터 i가 일으킨 폴트의 데이터(= 그 PTE 값) 레지스터.
 * 권한 폴트에서 이 값의 ACI 필드를 보면 어느 방향 접근이 거부됐는지 알 수 있다. */
#define IOMMU_INT_ERR_DATA_REG(i)	(0x150 + (i) * 4)
/* [한국어] L1 페이지 폴트의 마스터 비트맵 레지스터. */
#define IOMMU_L1PG_INT_REG		0x0180
/* [한국어] L2 페이지 폴트의 마스터 비트맵 레지스터. */
#define IOMMU_L2PG_INT_REG		0x0184

/* [한국어] 인터럽트 상태의 "무효한 L2 페이지(PTE)" 비트 — 매핑되지 않은
 * 주소에 접근했다는 뜻이다. */
#define IOMMU_INT_INVALID_L2PG			BIT(17)
/* [한국어] "무효한 L1 페이지(DTE)" 비트 — 그 1MB 영역에 PT조차 없다는 뜻이다. */
#define IOMMU_INT_INVALID_L1PG			BIT(16)
/* [한국어] 마스터 m이 권한 위반을 일으켰음을 나타내는 비트.
 * 하위 여섯 비트가 여섯 마스터에 대응한다. */
#define IOMMU_INT_MASTER_PERMISSION(m)		BIT(m)
/* [한국어] 여섯 마스터의 권한 위반 비트를 한데 묶은 마스크.
 * 폴트 핸들러가 ilog2로 마스터 번호를 뽑을 때 이 마스크로 먼저 거른다. */
#define IOMMU_INT_MASTER_MASK			(IOMMU_INT_MASTER_PERMISSION(0) | \
						 IOMMU_INT_MASTER_PERMISSION(1) | \
						 IOMMU_INT_MASTER_PERMISSION(2) | \
						 IOMMU_INT_MASTER_PERMISSION(3) | \
						 IOMMU_INT_MASTER_PERMISSION(4) | \
						 IOMMU_INT_MASTER_PERMISSION(5))
/* [한국어] 이 드라이버가 관심을 갖는 모든 인터럽트 원인의 마스크.
 * 활성화 레지스터에 그대로 쓰고, 핸들러가 "내 인터럽트인가"를 판별하는
 * 기준으로도 쓴다. */
#define IOMMU_INT_MASK				(IOMMU_INT_INVALID_L1PG | \
						 IOMMU_INT_INVALID_L2PG | \
						 IOMMU_INT_MASTER_MASK)

/* [한국어] 테이블 엔트리 하나의 크기(4바이트). DTE와 PTE가 같은 크기다. */
#define PT_ENTRY_SIZE			sizeof(u32)

/* [한국어] 1단계 DT의 엔트리 개수. IOVA 비트 20~31(12비트)이 인덱스이므로
 * 2^12 = 4096개다. */
#define NUM_DT_ENTRIES			4096
/* [한국어] DT 전체 크기 = 4096 × 4바이트 = 16KB. */
#define DT_SIZE				(NUM_DT_ENTRIES * PT_ENTRY_SIZE)

/* [한국어] 2단계 PT의 엔트리 개수. IOVA 비트 12~19(8비트)가 인덱스이므로
 * 2^8 = 256개다. */
#define NUM_PT_ENTRIES			256
/* [한국어] PT 전체 크기 = 256 × 4바이트 = 1KB. 페이지보다 작아
 * 전용 슬랩 캐시를 쓰는 이유다. */
#define PT_SIZE				(NUM_PT_ENTRIES * PT_ENTRY_SIZE)

/* [한국어] 이 IOMMU가 다루는 페이지 크기 4KB. 유일한 크기다. */
#define SPAGE_SIZE			4096

/* [한국어] IOMMU 인스턴스 하나의 상태.
 * 수명: probe에서 devm_kzalloc으로 만들어져 디바이스와 함께 사라진다. */
struct sun50i_iommu {
	struct iommu_device iommu;
	/* [한국어] IOMMU 코어에 등록되는 핸들(임베드).
	 * 설정자: probe의 iommu_device_sysfs_add()/register().
	 * 읽는 자: probe_device()가 담당 IOMMU로 이 주소를 반환한다. */

	/* Lock to modify the IOMMU registers */
	spinlock_t iommu_lock;
	/* [한국어] 레지스터 접근을 직렬화하는 스핀락.
	 * 설정자: probe가 초기화.
	 * 읽는 자: 무효화 함수들, enable/disable, 인터럽트 핸들러.
	 *          여러 함수가 assert_spin_locked()로 보유를 강제한다.
	 * 왜 irqsave인가: 폴트 인터럽트 핸들러도 이 락을 잡으므로,
	 *                 프로세스 컨텍스트 쪽은 인터럽트를 막아야 한다.
	 * 주의: 페이지 테이블 자체는 이 락으로 보호되지 않는다 —
	 *       map/unmap은 락 없이 테이블만 만지고, 하드웨어 무효화만
	 *       별도 콜백에서 락을 잡는다. */

	struct device *dev;
	/* [한국어] 이 IOMMU 자신의 device.
	 * 설정자: probe.
	 * 읽는 자: dma_map_single의 DMA 마스터, dev_err/dev_warn 로깅.
	 * 동기화: probe 이후 불변. */

	void __iomem *base;
	/* [한국어] MMIO 레지스터 블록의 매핑 주소.
	 * 설정자: probe의 devm_platform_ioremap_resource().
	 * 읽는 자: iommu_read()/iommu_write()의 기준 주소.
	 * 동기화: devm 관리라 디바이스 해제 시 자동 언매핑. */

	struct reset_control *reset;
	/* [한국어] IOMMU의 리셋 라인.
	 * 설정자: probe의 devm_reset_control_get().
	 * 읽는 자: enable이 deassert(리셋 해제), disable이 assert(리셋).
	 * 왜 필요한가: 이 IOMMU는 리셋 상태로 시작하므로, 레지스터를 쓰기
	 *              전에 반드시 풀어야 한다. */

	struct clk *clk;
	/* [한국어] IOMMU의 클록.
	 * 설정자: probe의 devm_clk_get().
	 * 읽는 자: enable/disable이 켜고 끈다.
	 * 동기화: enable/disable이 직렬화된 경로라 별도 락이 없다. */

	struct iommu_domain *domain;
	/* [한국어] 현재 이 IOMMU에 붙어 있는 도메인.
	 * 설정자: probe가 항등 도메인으로 초기화하고, attach_domain이
	 *          페이징 도메인으로 바꾸며, detach_domain이 NULL로 되돌린다.
	 * 읽는 자: enable()이 DT 주소를 얻는 데, 폴트 핸들러가
	 *          report_iommu_fault의 대상으로 쓴다.
	 * 값 범위: 항등 도메인, 페이징 도메인, 또는 NULL(detach 직후).
	 * 왜 하나뿐인가: IOMMU_TTB_REG가 하나뿐이라 하드웨어가 동시에
	 *                두 개의 DT를 가질 수 없다.
	 * 주의: detach가 NULL로 만드는데 그 뒤 identity_attach가
	 *       to_sun50i_domain(NULL)을 할 여지가 있다 — 실제로는
	 *       refcount가 그것을 막는다. */

	struct kmem_cache *pt_pool;
	/* [한국어] 2단계 PT(1KB) 전용 슬랩 캐시.
	 * 설정자: probe의 kmem_cache_create().
	 * 읽는 자: alloc/free_page_table().
	 * 왜 전용 캐시인가: PT가 1KB로 페이지보다 작고 1KB 정렬이 필요하다.
	 *                   게다가 SLAB_CACHE_DMA32로 4GB 이하에서 받아야
	 *                   32비트 DTE에 주소가 담긴다. */
};

/* [한국어] 페이징 도메인 하나의 상태.
 * 수명: domain_alloc_paging에서 만들어져 domain_free에서 해제된다.
 * 하드웨어가 DT를 하나만 가질 수 있어, 실질적으로 시스템에 하나만 존재한다. */
struct sun50i_iommu_domain {
	struct iommu_domain domain;
	/* [한국어] IOMMU 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging이 pgsize_bitmap(4KB)과
	 *          geometry(0~4GB-1)를 채운다.
	 * 읽는 자: 코어 전반. */

	/* Number of devices attached to the domain */
	refcount_t refcnt;
	/* [한국어] 이 도메인에 붙어 있는 디바이스의 수.
	 * 설정자: alloc이 1로 초기화하고, attach_device가 증가,
	 *          identity_attach가 감소시킨다.
	 * 읽는 자: identity_attach가 0이 되는 순간(마지막 detach)을 감지해
	 *          하드웨어를 끄고 모든 PT를 해제한다.
	 * 왜 필요한가: 여러 디바이스가 같은 도메인을 공유하므로, 하나가
	 *              떨어져 나갈 때마다 IOMMU를 끄면 나머지가 망가진다.
	 * 초기값이 1인 점에 주목: 도메인 생성 자체가 한 참조를 잡는 셈이라,
	 * 첫 attach 후에는 2가 된다. */

	/* L1 Page Table */
	u32 *dt;
	/* [한국어] 1단계 DT(16KB, 4096 엔트리)의 커널 가상 주소.
	 * 설정자: domain_alloc_paging의 iommu_alloc_pages_sz().
	 * 읽는 자: map/unmap/iova_to_phys가 DTE를 인덱싱하고,
	 *          detach_domain이 모든 PT를 해제할 때 순회한다.
	 * 왜 GFP_DMA32인가: DT의 물리 주소가 32비트 TTB 레지스터에 담겨야 한다. */

	dma_addr_t dt_dma;
	/* [한국어] 그 DT의 DMA 주소.
	 * 설정자: attach_domain의 dma_map_single().
	 * 읽는 자: enable()이 IOMMU_TTB_REG에 기록한다.
	 * 값 범위: 32비트 안에 들어가야 한다(DMA32 할당이 그것을 보장). */

	struct sun50i_iommu *iommu;
	/* [한국어] 이 도메인이 붙어 있는 IOMMU.
	 * 설정자: attach_domain.
	 * 읽는 자: table_flush()가 DMA 동기화에 쓰고, map()이 로깅과
	 *          PT 할당에 쓴다.
	 * 값 범위: attach 전에는 NULL이다 — flush_iotlb_all()이 부팅 초기에
	 *          그 상태로 불릴 수 있어 명시적으로 검사한다. */
};

/*
 * [한국어]
 * to_sun50i_domain - 일반 iommu_domain을 이 드라이버의 도메인으로 되돌린다
 *
 * @domain: 코어가 넘긴 일반 도메인 포인터.
 * @return: 그것을 감싸는 struct sun50i_iommu_domain 포인터.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄. 순수 포인터 산술이다.
 *
 * 호출 체인:
 *   map/unmap/attach/free/iova_to_phys → [to_sun50i_domain]
 */
static struct sun50i_iommu_domain *to_sun50i_domain(struct iommu_domain *domain)
{
	/* [한국어] 임베드된 멤버의 주소에서 오프셋을 빼 바깥 구조체를 얻는다. */
	return container_of(domain, struct sun50i_iommu_domain, domain);
}

/*
 * [한국어]
 * sun50i_iommu_from_dev - 클라이언트 디바이스에서 담당 IOMMU를 얻는다
 *
 * @dev: 클라이언트 디바이스.
 * @return: of_xlate가 심어 둔 IOMMU 인스턴스, 없으면 NULL.
 *
 * 왜 래퍼를 두는가: dev_iommu_priv_get()을 그대로 써도 되지만,
 * 이름을 붙이면 "이 priv에는 IOMMU가 들어 있다"는 의도가 드러난다.
 *
 * 실행 컨텍스트: probe_device와 attach 경로.
 *
 * 호출 체인:
 *   sun50i_iommu_probe_device() / attach_device() → [sun50i_iommu_from_dev]
 */
static struct sun50i_iommu *sun50i_iommu_from_dev(struct device *dev)
{
	/* [한국어] of_xlate가 저장해 둔 IOMMU 포인터를 꺼낸다. */
	return dev_iommu_priv_get(dev);
}

/*
 * [한국어]
 * iommu_read - IOMMU 레지스터를 읽는다
 *
 * @iommu: 대상 인스턴스.
 * @offset: 레지스터 오프셋.
 * @return: 읽은 32비트 값.
 *
 * relaxed가 아닌 readl을 쓰는 점에 주목: 이 드라이버는 배리어를 포함한
 * 접근자를 써서 순서를 명시적으로 고민하지 않아도 되게 했다. 레지스터
 * 접근 빈도가 낮아 성능 손해가 크지 않다는 판단이다.
 *
 * 실행 컨텍스트: 무효화 폴링과 인터럽트 핸들러. 잠들지 않는다.
 *
 * 호출 체인:
 *   폴트 핸들러들 → [iommu_read] → readl()
 */
static u32 iommu_read(struct sun50i_iommu *iommu, u32 offset)
{
	/* [한국어] 매핑된 베이스에 오프셋을 더해 읽는다. */
	return readl(iommu->base + offset);
}

/*
 * [한국어]
 * iommu_write - IOMMU 레지스터에 쓴다
 *
 * @iommu: 대상 인스턴스.
 * @offset: 레지스터 오프셋.
 * @value: 쓸 값.
 * @return: 없음.
 *
 * 실행 컨텍스트: 무효화, enable/disable, 인터럽트 핸들러.
 *
 * 호출 체인:
 *   zap 계열 / enable / disable / irq → [iommu_write] → writel()
 */
static void iommu_write(struct sun50i_iommu *iommu, u32 offset, u32 value)
{
	/* [한국어] 매핑된 베이스에 오프셋을 더해 쓴다. */
	writel(value, iommu->base + offset);
}

/*
 * The Allwinner H6 IOMMU uses a 2-level page table.
 *
 * The first level is the usual Directory Table (DT), that consists of
 * 4096 4-bytes Directory Table Entries (DTE), each pointing to a Page
 * Table (PT).
 *
 * Each PT consits of 256 4-bytes Page Table Entries (PTE), each
 * pointing to a 4kB page of physical memory.
 *
 * The IOMMU supports a single DT, pointed by the IOMMU_TTB_REG
 * register that contains its physical address.
 */

/* [한국어] IOVA에서 1단계(DT) 인덱스가 차지하는 비트 20~31.
 * 12비트이므로 4096개 엔트리에 대응한다. */
#define SUN50I_IOVA_DTE_MASK	GENMASK(31, 20)
/* [한국어] IOVA에서 2단계(PT) 인덱스가 차지하는 비트 12~19.
 * 8비트이므로 256개 엔트리에 대응한다. */
#define SUN50I_IOVA_PTE_MASK	GENMASK(19, 12)
/* [한국어] IOVA에서 페이지 내 오프셋이 차지하는 하위 12비트(4KB). */
#define SUN50I_IOVA_PAGE_MASK	GENMASK(11, 0)

/*
 * [한국어]
 * sun50i_iova_get_dte_index - IOVA에서 1단계 테이블 인덱스를 뽑는다
 *
 * @iova: 대상 I/O 가상 주소.
 * @return: DT 배열의 인덱스(0~4095).
 *
 * FIELD_GET이 마스크를 보고 시프트까지 알아서 해 주므로, 시프트 상수를
 * 따로 정의할 필요가 없다 — 마스크 하나가 위치와 폭을 모두 담는다.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   map/unmap/iova_to_phys/dte_get_page_table → [sun50i_iova_get_dte_index]
 */
static u32 sun50i_iova_get_dte_index(dma_addr_t iova)
{
	/* [한국어] 비트 20~31을 뽑아 오른쪽 끝으로 내린다. */
	return FIELD_GET(SUN50I_IOVA_DTE_MASK, iova);
}

/*
 * [한국어]
 * sun50i_iova_get_pte_index - IOVA에서 2단계 테이블 인덱스를 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: PT 배열의 인덱스(0~255).
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   map/unmap/iova_to_phys → [sun50i_iova_get_pte_index]
 */
static u32 sun50i_iova_get_pte_index(dma_addr_t iova)
{
	/* [한국어] 비트 12~19를 뽑아 오른쪽 끝으로 내린다. */
	return FIELD_GET(SUN50I_IOVA_PTE_MASK, iova);
}

/*
 * [한국어]
 * sun50i_iova_get_page_offset - IOVA에서 페이지 내 오프셋을 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: 4KB 페이지 안에서의 바이트 오프셋(0~4095).
 *
 * iova_to_phys가 PTE의 페이지 주소에 이 값을 더해 최종 물리 주소를 만든다.
 *
 * 실행 컨텍스트: 조회 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   sun50i_iommu_iova_to_phys() → [sun50i_iova_get_page_offset]
 */
static u32 sun50i_iova_get_page_offset(dma_addr_t iova)
{
	/* [한국어] 하위 12비트를 그대로 뽑는다. */
	return FIELD_GET(SUN50I_IOVA_PAGE_MASK, iova);
}

/*
 * Each Directory Table Entry has a Page Table address and a valid
 * bit:

 * +---------------------+-----------+-+
 * | PT address          | Reserved  |V|
 * +---------------------+-----------+-+
 *  31:10 - Page Table address
 *   9:2  - Reserved
 *   1:0  - 1 if the entry is valid
 */

/* [한국어] DTE에서 2단계 테이블 주소가 차지하는 비트 10~31.
 * PT가 1KB 정렬이라 하위 10비트를 생략할 수 있다. */
#define SUN50I_DTE_PT_ADDRESS_MASK	GENMASK(31, 10)
/* [한국어] DTE의 속성 필드(하위 2비트). 값이 1이면 유효하다. */
#define SUN50I_DTE_PT_ATTRS		GENMASK(1, 0)
/* [한국어] 유효한 DTE의 속성 값. 2비트 필드지만 정확히 1이어야 유효하다 —
 * 0이나 2, 3은 무효로 취급된다. */
#define SUN50I_DTE_PT_VALID		1

/*
 * [한국어]
 * sun50i_dte_get_pt_address - DTE에서 2단계 테이블의 물리 주소를 뽑는다
 *
 * @dte: 해석할 1단계 엔트리.
 * @return: 그 엔트리가 가리키는 PT의 물리 주소.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   map/unmap/iova_to_phys/detach_domain → [sun50i_dte_get_pt_address]
 */
static phys_addr_t sun50i_dte_get_pt_address(u32 dte)
{
	/* [한국어] 주소 비트만 남긴다. 하위 10비트는 속성/예약이라 0이 되고,
	 * 그것이 곧 1KB 정렬된 PT 주소다. */
	return (phys_addr_t)dte & SUN50I_DTE_PT_ADDRESS_MASK;
}

/*
 * [한국어]
 * sun50i_dte_is_pt_valid - DTE가 유효한 테이블을 가리키는지 판별한다
 *
 * @dte: 검사할 엔트리.
 * @return: 유효하면 true.
 *
 * 단순히 0이 아닌지 보는 것이 아니라 속성 필드가 **정확히 1**인지 본다.
 * 다른 값(2, 3)은 하드웨어가 정의하지 않은 조합이라 무효로 취급한다.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   map/unmap/iova_to_phys/detach_domain → [sun50i_dte_is_pt_valid]
 */
static bool sun50i_dte_is_pt_valid(u32 dte)
{
	/* [한국어] 속성 2비트가 정확히 VALID(1)인지 확인한다. */
	return (dte & SUN50I_DTE_PT_ATTRS) == SUN50I_DTE_PT_VALID;
}

/*
 * [한국어]
 * sun50i_mk_dte - PT의 DMA 주소로 유효한 DTE 값을 만든다
 *
 * @pt_dma: 2단계 테이블의 DMA(=물리) 주소.
 * @return: 하드웨어가 이해하는 DTE 값.
 *
 * 실행 컨텍스트: PT 설치 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   sun50i_dte_get_page_table() → [sun50i_mk_dte]
 */
static u32 sun50i_mk_dte(dma_addr_t pt_dma)
{
	/* [한국어] 주소의 상위 22비트만 남기고 유효 표시를 얹는다.
	 * PT가 1KB 정렬이라 하위 10비트가 0이므로 OR로 충분하다. */
	return (pt_dma & SUN50I_DTE_PT_ADDRESS_MASK) | SUN50I_DTE_PT_VALID;
}

/*
 * Each PTE has a Page address, an authority index and a valid bit:
 *
 * +----------------+-----+-----+-----+---+-----+
 * | Page address   | Rsv | ACI | Rsv | V | Rsv |
 * +----------------+-----+-----+-----+---+-----+
 *  31:12 - Page address
 *  11:8  - Reserved
 *   7:4  - Authority Control Index
 *   3:2  - Reserved
 *     1  - 1 if the entry is valid
 *     0  - Reserved
 *
 * The way permissions work is that the IOMMU has 16 "domains" that
 * can be configured to give each masters either read or write
 * permissions through the IOMMU_DM_AUT_CTRL_REG registers. The domain
 * 0 seems like the default domain, and its permissions in the
 * IOMMU_DM_AUT_CTRL_REG are only read-only, so it's not really
 * useful to enforce any particular permission.
 *
 * Each page entry will then have a reference to the domain they are
 * affected to, so that we can actually enforce them on a per-page
 * basis.
 *
 * In order to make it work with the IOMMU framework, we will be using
 * 4 different domains, starting at 1: RD_WR, RD, WR and NONE
 * depending on the permission we want to enforce. Each domain will
 * have each master setup in the same way, since the IOMMU framework
 * doesn't seem to restrict page access on a per-device basis. And
 * then we will use the relevant domain index when generating the page
 * table entry depending on the permissions we want to be enforced.
 */

/* [한국어] 이 드라이버가 쓰는 권한 도메인(ACI) 번호들.
 * 원본 주석이 설명하는 설계가 여기 담겨 있다 — 하드웨어의 16개 권한
 * 도메인 중 1~4번을 골라, 각각을 "권한 없음/읽기만/쓰기만/읽고 쓰기"로
 * 미리 구성해 둔다. 그러면 PTE의 ACI 필드에 번호만 넣어 페이지별 권한을
 * 표현할 수 있다.
 * 0번을 쓰지 않는 이유도 원본 주석에 있다: 0번은 기본 도메인처럼 보이고
 * 그 권한 레지스터가 읽기 전용이라 설정을 강제할 수 없기 때문이다. */
enum sun50i_iommu_aci {
	SUN50I_IOMMU_ACI_DO_NOT_USE = 0,
	/* [한국어] 0번 — 쓰지 않는다. 하드웨어의 기본 도메인이며 권한
	 * 레지스터가 읽기 전용이라 이 드라이버가 구성할 수 없다.
	 * 이름 자체가 그 사실을 못 박아 둔 것이다. */

	SUN50I_IOMMU_ACI_NONE,
	/* [한국어] 1번 — 모든 마스터의 읽기와 쓰기를 금지한다.
	 * 설정자: enable()이 여섯 마스터 각각에 RD/WR UNAVAIL 비트를 세운다.
	 * 쓰이는 곳: 읽기도 쓰기도 요청되지 않은 매핑(사실상 없다). */

	SUN50I_IOMMU_ACI_RD,
	/* [한국어] 2번 — 읽기만 허용(쓰기 금지).
	 * 설정자: enable()이 WR UNAVAIL만 세운다.
	 * 쓰이는 곳: IOMMU_READ만 요청된 매핑. */

	SUN50I_IOMMU_ACI_WR,
	/* [한국어] 3번 — 쓰기만 허용(읽기 금지).
	 * 설정자: enable()이 RD UNAVAIL만 세운다.
	 * 쓰이는 곳: IOMMU_WRITE만 요청된 매핑. */

	SUN50I_IOMMU_ACI_RD_WR,
	/* [한국어] 4번 — 읽기와 쓰기를 모두 허용.
	 * 설정자: enable()이 이 도메인의 레지스터를 아예 건드리지 않는다 —
	 *          UNAVAIL 비트가 하나도 서지 않은 상태가 곧 전면 허용이다.
	 * 쓰이는 곳: 가장 흔한, 읽기와 쓰기가 모두 요청된 매핑. */
};

/* [한국어] PTE에서 물리 페이지 주소가 차지하는 비트 12~31(4KB 정렬). */
#define SUN50I_PTE_PAGE_ADDRESS_MASK	GENMASK(31, 12)
/* [한국어] PTE의 ACI 필드(비트 4~7). 위 enum 값이 여기 들어가
 * 그 페이지의 권한을 결정한다. */
#define SUN50I_PTE_ACI_MASK		GENMASK(7, 4)
/* [한국어] PTE의 유효 비트(비트 1). 비트 0이 아니라 1인 점에 주의 —
 * 비트 0은 예약이다. */
#define SUN50I_PTE_PAGE_VALID		BIT(1)

/*
 * [한국어]
 * sun50i_pte_get_page_address - PTE에서 물리 페이지 주소를 뽑는다
 *
 * @pte: 해석할 2단계 엔트리.
 * @return: 4KB 정렬된 물리 주소.
 *
 * 실행 컨텍스트: 조회와 폴트 보고 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   sun50i_iommu_iova_to_phys() / sun50i_iommu_map()(중복 검사)
 *   → [sun50i_pte_get_page_address]
 */
static phys_addr_t sun50i_pte_get_page_address(u32 pte)
{
	/* [한국어] 주소 비트만 남긴다. 하위 12비트의 ACI와 유효 비트는 사라진다. */
	return (phys_addr_t)pte & SUN50I_PTE_PAGE_ADDRESS_MASK;
}

/*
 * [한국어]
 * sun50i_get_pte_aci - PTE에서 권한 도메인 번호를 뽑는다
 *
 * @pte: 해석할 엔트리.
 * @return: ACI 값(enum sun50i_iommu_aci).
 *
 * 왜 필요한가: 권한 폴트가 났을 때, 그 페이지의 ACI를 보면 어느 방향
 * 접근이 금지되어 있었는지 알 수 있다 — 즉 폴트가 읽기였는지 쓰기였는지를
 * 역산할 수 있다. handle_perm_irq()가 정확히 그 추론을 한다.
 *
 * 실행 컨텍스트: 권한 폴트 핸들러. 순수 계산이다.
 *
 * 호출 체인:
 *   sun50i_iommu_handle_perm_irq() → [sun50i_get_pte_aci]
 */
static enum sun50i_iommu_aci sun50i_get_pte_aci(u32 pte)
{
	/* [한국어] 비트 4~7을 뽑아 오른쪽 끝으로 내린다. */
	return FIELD_GET(SUN50I_PTE_ACI_MASK, pte);
}

/*
 * [한국어]
 * sun50i_pte_is_page_valid - PTE가 유효한 매핑인지 판별한다
 *
 * @pte: 검사할 엔트리.
 * @return: 유효하면 true.
 *
 * DTE와 달리 단일 비트만 본다 — PTE의 유효 표시는 비트 1 하나뿐이다.
 *
 * 실행 컨텍스트: 매핑/해제/조회 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   map/unmap/iova_to_phys → [sun50i_pte_is_page_valid]
 */
static bool sun50i_pte_is_page_valid(u32 pte)
{
	/* [한국어] 유효 비트가 서 있는지 확인한다. */
	return pte & SUN50I_PTE_PAGE_VALID;
}

/*
 * [한국어]
 * sun50i_mk_pte - 물리 주소와 보호 플래그로 PTE 값을 만든다
 *
 * @page: 매핑할 물리 페이지 주소.
 * @prot: IOMMU_READ/IOMMU_WRITE 조합.
 * @return: 하드웨어가 이해하는 PTE 값.
 *
 * 이 함수가 IOMMU API의 권한 모델을 하드웨어의 ACI 모델로 번역하는 지점이다.
 * 요청된 권한 조합을 네 가지 ACI 중 하나로 사상하고, 그 번호를 PTE에 심는다.
 * 실제 허용/금지 판정은 enable()이 미리 구성해 둔 권한 도메인 레지스터가 한다.
 *
 * 순서에 주목: 읽기와 쓰기가 모두 있으면 RD_WR, 아니면 읽기만/쓰기만,
 * 둘 다 없으면 NONE이다. else-if 사슬이라 첫 조건이 가장 넓은 권한을
 * 잡아 낸다.
 *
 * 실행 컨텍스트: 매핑 생성 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   sun50i_iommu_map() → [sun50i_mk_pte]
 */
static u32 sun50i_mk_pte(phys_addr_t page, int prot)
{
	/* [한국어] 선택된 권한 도메인 번호. */
	enum sun50i_iommu_aci aci;
	/* [한국어] PTE에 얹을 플래그 비트들. */
	u32 flags = 0;

	/* [한국어] 읽기와 쓰기가 모두 요청되었다면 전면 허용 도메인을 쓴다.
	 * 가장 흔한 경우라 먼저 검사한다. */
	if ((prot & (IOMMU_READ | IOMMU_WRITE)) == (IOMMU_READ | IOMMU_WRITE))
		aci = SUN50I_IOMMU_ACI_RD_WR;
	/* [한국어] 읽기만 요청되었다면 쓰기가 금지된 도메인. */
	else if (prot & IOMMU_READ)
		aci = SUN50I_IOMMU_ACI_RD;
	/* [한국어] 쓰기만 요청되었다면 읽기가 금지된 도메인. */
	else if (prot & IOMMU_WRITE)
		aci = SUN50I_IOMMU_ACI_WR;
	else
		/* [한국어] 둘 다 없다면 모든 접근이 금지된 도메인.
		 * 실무에서는 거의 없지만 API가 허용하는 조합이다. */
		aci = SUN50I_IOMMU_ACI_NONE;

	/* [한국어] 선택한 ACI를 PTE의 비트 4~7 자리에 넣는다. */
	flags |= FIELD_PREP(SUN50I_PTE_ACI_MASK, aci);
	/* [한국어] 물리 주소에서 4KB 정렬된 부분만 남긴다. */
	page &= SUN50I_PTE_PAGE_ADDRESS_MASK;
	/* [한국어] 주소와 권한, 유효 비트를 합쳐 완성된 PTE를 만든다. */
	return page | flags | SUN50I_PTE_PAGE_VALID;
}

/*
 * [한국어]
 * sun50i_table_flush - 갱신한 테이블 엔트리를 하드웨어에 보이게 만든다
 *
 * @sun50i_domain: 대상 도메인(IOMMU 디바이스를 얻는다).
 * @vaddr: 갱신한 첫 엔트리의 커널 가상 주소.
 * @count: 갱신한 엔트리 개수.
 * @return: 없음.
 *
 * 왜 필요한가: 이 IOMMU의 테이블 워크는 캐시 코히런트하지 않다.
 * CPU가 쓴 엔트리가 캐시에만 있으면 하드웨어는 옛 값을 본다.
 * dma_sync_single_for_device가 그 캐시 라인을 메모리로 밀어낸다.
 *
 * virt_to_phys를 DMA 주소로 그대로 쓰는 점에 주목: 이 드라이버는
 * 두 값이 같다고 전제하며, alloc_page_table()의 WARN_ON이 그것을 검증한다.
 *
 * 실행 컨텍스트: 매핑/해제 경로. 캐시 조작뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   map/unmap/dte_get_page_table/detach_domain → [sun50i_table_flush]
 *   → dma_sync_single_for_device()
 */
static void sun50i_table_flush(struct sun50i_iommu_domain *sun50i_domain,
			       void *vaddr, unsigned int count)
{
	/* [한국어] DMA 동기화에 필요한 IOMMU 디바이스를 얻는다. */
	struct sun50i_iommu *iommu = sun50i_domain->iommu;
	/* [한국어] 물리 주소를 DMA 주소로 그대로 쓴다 — 두 값이 같다는
	 * 전제가 PT 할당 시 검증되어 있다. */
	dma_addr_t dma = virt_to_phys(vaddr);
	/* [한국어] 동기화할 바이트 수. */
	size_t size = count * PT_ENTRY_SIZE;

	/* [한국어] 그 구간의 캐시를 메모리로 밀어낸다. DMA_TO_DEVICE는
	 * "CPU가 썼으니 디바이스가 볼 수 있게 하라"는 뜻이다. */
	dma_sync_single_for_device(iommu->dev, dma, size, DMA_TO_DEVICE);
}

/*
 * [한국어]
 * sun50i_iommu_zap_iova - 하나의 IOVA에 대한 TLB 엔트리를 무효화한다
 *
 * @iommu: 대상 인스턴스.
 * @iova: 무효화할 주소.
 * @return: 없음.
 *
 * 동작: 주소와 마스크를 쓴 뒤 실행 비트를 세우고, 하드웨어가 그 비트를
 * 스스로 0으로 되돌릴 때까지 폴링한다. 마스크로 GENMASK(31,12)를 주므로
 * 정확히 한 페이지만 대상이 된다.
 *
 * 타임아웃(2ms) 시 경고만 남기고 계속 진행하는데, 무효화가 되지 않은 채
 * 진행하면 옛 매핑이 살아남을 수 있다 — 하드웨어가 응답하지 않는
 * 비정상 상황이라 되돌릴 방법이 마땅치 않기 때문이다.
 *
 * 실행 컨텍스트: iommu_lock을 쥔 상태(호출자가 보장). atomic 폴링이라
 * 잠들지 않는다.
 *
 * 호출 체인:
 *   sun50i_iommu_zap_range() → [sun50i_iommu_zap_iova]
 *   → readl_poll_timeout_atomic()
 */
static void sun50i_iommu_zap_iova(struct sun50i_iommu *iommu,
				  unsigned long iova)
{
	/* [한국어] 폴링 중 읽은 실행 레지스터 값을 담는 변수. */
	u32 reg;
	/* [한국어] 폴링 결과(0 성공, -ETIMEDOUT 실패). */
	int ret;

	/* [한국어] 무효화할 주소를 지정한다. */
	iommu_write(iommu, IOMMU_TLB_IVLD_ADDR_REG, iova);
	/* [한국어] 페이지 단위 마스크를 지정해 정확히 한 페이지만 대상으로 삼는다.
	 * 마스크를 넓히면 더 많은 엔트리를 한 번에 비울 수도 있지만,
	 * 이 드라이버는 페이지 단위로만 쓴다. */
	iommu_write(iommu, IOMMU_TLB_IVLD_ADDR_MASK_REG, GENMASK(31, 12));
	/* [한국어] 실행 비트를 세워 무효화를 시작한다. */
	iommu_write(iommu, IOMMU_TLB_IVLD_ENABLE_REG,
		    IOMMU_TLB_IVLD_ENABLE_ENABLE);

	/* [한국어] 하드웨어가 실행 비트를 0으로 되돌릴 때까지 1us 간격으로
	 * 최대 2ms 기다린다. atomic 변형이라 스핀락을 쥔 채로도 안전하다. */
	ret = readl_poll_timeout_atomic(iommu->base + IOMMU_TLB_IVLD_ENABLE_REG,
					reg, !reg, 1, 2000);
	/* [한국어] 타임아웃이면 경고만 남긴다 — 되돌릴 방법이 없고,
	 * 이 상황에서는 하드웨어 자체가 이상하다는 뜻이다. */
	if (ret)
		dev_warn(iommu->dev, "TLB invalidation timed out!\n");
}

/*
 * [한국어]
 * sun50i_iommu_zap_ptw_cache - PTW(테이블 워크) 캐시를 무효화한다
 *
 * @iommu: 대상 인스턴스.
 * @iova: 무효화할 주소.
 * @return: 없음.
 *
 * TLB와 별개인 이유: 이 하드웨어는 최종 변환 결과(TLB)와 중간 단계
 * 조회 결과(DTE → PT 주소)를 따로 캐시한다. DTE를 고쳤다면 TLB만
 * 비워서는 부족하고 PTW 캐시도 비워야 한다.
 *
 * 마스크 레지스터가 없는 점에 주목: PTW 캐시는 1MB 단위(DTE 하나가
 * 덮는 범위)로 동작하므로 주소만 주면 된다. 그래서 zap_range가
 * SZ_1M 간격으로 호출한다.
 *
 * 실행 컨텍스트: iommu_lock 보유 상태. atomic 폴링.
 *
 * 호출 체인:
 *   sun50i_iommu_zap_range() → [sun50i_iommu_zap_ptw_cache]
 */
static void sun50i_iommu_zap_ptw_cache(struct sun50i_iommu *iommu,
				       unsigned long iova)
{
	/* [한국어] 폴링용 임시 변수. */
	u32 reg;
	/* [한국어] 폴링 결과. */
	int ret;

	/* [한국어] 무효화할 주소를 지정한다. */
	iommu_write(iommu, IOMMU_PC_IVLD_ADDR_REG, iova);
	/* [한국어] 실행 비트를 세워 무효화를 시작한다. */
	iommu_write(iommu, IOMMU_PC_IVLD_ENABLE_REG,
		    IOMMU_PC_IVLD_ENABLE_ENABLE);

	/* [한국어] 완료를 폴링한다. TLB 쪽과 같은 방식이다. */
	ret = readl_poll_timeout_atomic(iommu->base + IOMMU_PC_IVLD_ENABLE_REG,
					reg, !reg, 1, 2000);
	/* [한국어] 타임아웃이면 경고만 남긴다. */
	if (ret)
		dev_warn(iommu->dev, "PTW cache invalidation timed out!\n");
}

/*
 * [한국어]
 * sun50i_iommu_zap_range - 지정한 범위의 TLB와 PTW 캐시를 무효화한다
 *
 * @iommu: 대상 인스턴스.
 * @iova: 무효화 시작 주소.
 * @size: 무효화할 크기.
 * @return: 없음.
 *
 * 이 함수의 무효화 범위가 이 드라이버에서 가장 미묘한 부분이다.
 * 요청 범위보다 **한 칸씩 더 넓게** 비운다:
 *  - TLB: iova와 iova+4KB, 그리고 크기가 한 페이지를 넘으면
 *    iova+size와 iova+size+4KB까지.
 *  - PTW 캐시: iova와 iova+1MB, 크기가 1MB를 넘으면 끝 쪽도 마찬가지.
 * 왜 그런가: enable()이 여섯 마스터의 TLB 프리페치를 모두 켜 두었기
 * 때문이다. 하드웨어가 요청받지 않은 다음 엔트리까지 미리 읽어 캐시에
 * 담으므로, 경계 바로 바깥의 엔트리도 옛 값을 들고 있을 수 있다.
 * 간격이 TLB는 페이지(4KB), PTW는 DTE가 덮는 범위(1MB)인 것도
 * 각 캐시의 단위가 그렇기 때문이다.
 *
 * 자동 게이팅을 끄고 켜는 이유: 클록이 게이팅된 상태에서는 무효화 명령이
 * 처리되지 않아 폴링이 타임아웃된다. 그래서 무효화 동안만 게이팅을 끈다.
 *
 * 실행 컨텍스트: 호출자가 iommu_lock을 잡은 상태여야 한다 —
 * assert_spin_locked()가 그것을 강제한다. 최대 여덟 번의 폴링이
 * 이어질 수 있어 락 보유 시간이 길어질 수 있다.
 *
 * 호출 체인:
 *   sun50i_iommu_iotlb_sync_map() / sun50i_iommu_report_fault()
 *   → [sun50i_iommu_zap_range] → zap_iova(), zap_ptw_cache()
 */
static void sun50i_iommu_zap_range(struct sun50i_iommu *iommu,
				   unsigned long iova, size_t size)
{
	/* [한국어] 호출자가 락을 쥐고 있음을 확인한다. 레지스터를 여러 개
	 * 순서대로 만지므로 중간에 끼어들면 무효화가 뒤엉킨다. */
	assert_spin_locked(&iommu->iommu_lock);

	/* [한국어] 자동 클록 게이팅을 끈다. 게이팅된 상태에서는 무효화
	 * 명령이 처리되지 않아 아래 폴링이 전부 타임아웃된다. */
	iommu_write(iommu, IOMMU_AUTO_GATING_REG, 0);

	/* [한국어] 시작 페이지의 TLB를 비운다. */
	sun50i_iommu_zap_iova(iommu, iova);
	/* [한국어] 그 다음 페이지도 비운다 — 프리페치가 미리 읽어 두었을 수 있다. */
	sun50i_iommu_zap_iova(iommu, iova + SPAGE_SIZE);
	/* [한국어] 범위가 한 페이지를 넘으면 끝 쪽도 같은 이유로 두 칸을 비운다. */
	if (size > SPAGE_SIZE) {
		sun50i_iommu_zap_iova(iommu, iova + size);	/* [한국어] 끝 쪽 페이지도 비운다 — 프리페치가 경계 너머를 읽어 두었을 수 있다. */
		sun50i_iommu_zap_iova(iommu, iova + size + SPAGE_SIZE);	/* [한국어] 그 다음 페이지까지 비워 프리페치 여유를 덮는다. */
	}
	/* [한국어] PTW 캐시도 시작 지점을 비운다. */
	sun50i_iommu_zap_ptw_cache(iommu, iova);
	/* [한국어] PTW 캐시의 단위가 1MB(DTE 하나가 덮는 범위)이므로
	 * 그만큼 떨어진 다음 칸도 비운다. */
	sun50i_iommu_zap_ptw_cache(iommu, iova + SZ_1M);
	/* [한국어] 범위가 1MB를 넘으면 끝 쪽의 두 칸도 비운다. */
	if (size > SZ_1M) {
		sun50i_iommu_zap_ptw_cache(iommu, iova + size);	/* [한국어] 끝 쪽의 PTW 캐시도 비운다. */
		sun50i_iommu_zap_ptw_cache(iommu, iova + size + SZ_1M);	/* [한국어] 1MB 떨어진 다음 칸까지 비워 프리페치 여유를 덮는다. */
	}

	/* [한국어] 무효화가 끝났으니 자동 게이팅을 다시 켜 전력을 아낀다. */
	iommu_write(iommu, IOMMU_AUTO_GATING_REG, IOMMU_AUTO_GATING_ENABLE);
}

/*
 * [한국어]
 * sun50i_iommu_flush_all_tlb - 모든 TLB와 PTW 캐시를 통째로 비운다
 *
 * @iommu: 대상 인스턴스.
 * @return: 0 성공, -ETIMEDOUT(플러시가 완료되지 않음).
 *
 * 한 번의 레지스터 쓰기로 여덟 가지를 모두 비운다 — PTW 캐시,
 * 매크로 TLB(공용), 그리고 여섯 마스터의 마이크로 TLB. 이 하드웨어에는
 * 마스터마다 전용 TLB가 있어 개별적으로 지정해야 한다.
 *
 * zap_range와 달리 자동 게이팅을 끄지 않는 점에 주목: 이 함수가 불리는
 * 시점(enable 중, flush_iotlb_all)에는 게이팅이 아직 켜지지 않았거나,
 * 전체 플러시라 프리페치 경계 문제가 없기 때문이다.
 *
 * 반환값을 쓰는 곳: enable()이 플러시 실패를 초기화 실패로 취급한다 —
 * 옛 매핑이 남은 채로 IOMMU를 켜면 잘못된 변환이 일어나기 때문이다.
 *
 * 실행 컨텍스트: iommu_lock 보유 상태(assert로 강제). atomic 폴링.
 *
 * 호출 체인:
 *   sun50i_iommu_enable() / sun50i_iommu_flush_iotlb_all()
 *   → [sun50i_iommu_flush_all_tlb]
 */
static int sun50i_iommu_flush_all_tlb(struct sun50i_iommu *iommu)
{
	/* [한국어] 폴링용 임시 변수. */
	u32 reg;
	/* [한국어] 폴링 결과. */
	int ret;

	/* [한국어] 호출자의 락 보유를 강제한다. */
	assert_spin_locked(&iommu->iommu_lock);

	/* [한국어] 비울 대상을 모두 지정해 한 번에 명령을 낸다.
	 * PTW 캐시와 매크로 TLB, 그리고 여섯 마스터의 마이크로 TLB를
	 * 하나하나 열거해야 한다 — 하드웨어가 "전부"를 뜻하는 비트를
	 * 따로 제공하지 않기 때문이다. */
	iommu_write(iommu,
		    IOMMU_TLB_FLUSH_REG,
		    IOMMU_TLB_FLUSH_PTW_CACHE |
		    IOMMU_TLB_FLUSH_MACRO_TLB |
		    IOMMU_TLB_FLUSH_MICRO_TLB(5) |
		    IOMMU_TLB_FLUSH_MICRO_TLB(4) |
		    IOMMU_TLB_FLUSH_MICRO_TLB(3) |
		    IOMMU_TLB_FLUSH_MICRO_TLB(2) |
		    IOMMU_TLB_FLUSH_MICRO_TLB(1) |
		    IOMMU_TLB_FLUSH_MICRO_TLB(0));

	/* [한국어] 하드웨어가 플러시 레지스터를 0으로 되돌릴 때까지 기다린다. */
	ret = readl_poll_timeout_atomic(iommu->base + IOMMU_TLB_FLUSH_REG,
					reg, !reg,
					1, 2000);
	/* [한국어] 타임아웃이면 경고를 남긴다. */
	if (ret)
		dev_warn(iommu->dev, "TLB Flush timed out!\n");

	/* [한국어] 결과를 반환한다 — enable()이 이 값으로 초기화 성공 여부를 판단한다. */
	return ret;
}

/*
 * [한국어]
 * sun50i_iommu_flush_iotlb_all - 도메인 전체의 TLB를 비우는 콜백
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * 원본 주석이 설명하는 상황이 이 함수의 핵심이다: 부팅 시 .probe_device
 * 직후 이 콜백이 한 번 불리는데, 그 시점에는 아직 .attach_device가
 * 실행되지 않아 sun50i_domain->iommu가 NULL이다. 그대로 진행하면
 * NULL 역참조가 나므로 조용히 돌아간다. 어차피 디바이스 전원이 켜질 때
 * 모든 캐시를 비우므로 문제가 없다는 것이 원본의 근거다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. iommu_lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   iommu_flush_iotlb_all() → domain_ops->flush_iotlb_all
 *   → [sun50i_iommu_flush_iotlb_all] → sun50i_iommu_flush_all_tlb()
 */
static void sun50i_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sun50i_iommu_domain *sun50i_domain = to_sun50i_domain(domain);
	/* [한국어] 아직 attach 전이면 NULL일 수 있다. */
	struct sun50i_iommu *iommu = sun50i_domain->iommu;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/*
	 * At boot, we'll have a first call into .flush_iotlb_all right after
	 * .probe_device, and since we link our (single) domain to our iommu in
	 * the .attach_device callback, we don't have that pointer set.
	 *
	 * It shouldn't really be any trouble to ignore it though since we flush
	 * all caches as part of the device powerup.
	 */
	/* [한국어] attach 전이라 IOMMU를 모르는 상태다. 원본 주석의 근거대로
	 * 조용히 무시한다 — 전원 인가 시 어차피 전체를 비운다. */
	if (!iommu)
		return;

	/* [한국어] 레지스터 접근을 직렬화한다. 인터럽트 핸들러도 이 락을
	 * 잡으므로 irqsave가 필요하다. */
	spin_lock_irqsave(&iommu->iommu_lock, flags);
	sun50i_iommu_flush_all_tlb(iommu);	/* [한국어] 모든 TLB와 PTW 캐시를 통째로 비운다. */
	spin_unlock_irqrestore(&iommu->iommu_lock, flags);	/* [한국어] 플러시가 끝났으니 락을 푼다. */
}

/*
 * [한국어]
 * sun50i_iommu_iotlb_sync_map - 매핑을 추가한 뒤 캐시를 무효화한다
 *
 * @domain: 대상 도메인.
 * @iova: 새로 매핑된 구간의 시작.
 * @size: 그 구간의 크기.
 * @return: 항상 0.
 *
 * 왜 매핑 추가에도 무효화가 필요한가: 하드웨어가 "이 주소는 매핑 없음"이라는
 * 결과나 옛 DTE를 캐시해 두었을 수 있다. 특히 프리페치가 켜져 있어
 * 요청하지 않은 엔트리까지 미리 읽어 두므로, 새 매핑이 보이려면
 * 그 캐시를 비워야 한다.
 *
 * flush_iotlb_all과 달리 NULL 검사가 없는 점에 주목: 매핑이 일어난다는
 * 것은 이미 attach가 끝났다는 뜻이므로 iommu가 유효하다.
 *
 * 실행 컨텍스트: 매핑 완료 후. iommu_lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   iommu_map()의 마무리 → domain_ops->iotlb_sync_map
 *   → [sun50i_iommu_iotlb_sync_map] → sun50i_iommu_zap_range()
 */
static int sun50i_iommu_iotlb_sync_map(struct iommu_domain *domain,
				       unsigned long iova, size_t size)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sun50i_iommu_domain *sun50i_domain = to_sun50i_domain(domain);
	/* [한국어] attach가 끝났으므로 유효한 IOMMU 포인터다. */
	struct sun50i_iommu *iommu = sun50i_domain->iommu;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] 무효화가 여러 레지스터를 순서대로 만지므로 직렬화가 필요하다. */
	spin_lock_irqsave(&iommu->iommu_lock, flags);
	sun50i_iommu_zap_range(iommu, iova, size);	/* [한국어] 새 매핑 구간과 그 주변의 캐시를 비운다. */
	spin_unlock_irqrestore(&iommu->iommu_lock, flags);

	/* [한국어] 실패를 표현할 수단이 없어 항상 성공을 반환한다. */
	return 0;
}

/*
 * [한국어]
 * sun50i_iommu_iotlb_sync - unmap 이후의 캐시 무효화 콜백
 *
 * @domain: 대상 도메인.
 * @gather: 무효화 범위를 모아 둔 구조체 — 이 드라이버는 쓰지 않는다.
 * @return: 없음.
 *
 * 왜 gather를 무시하고 전체를 비우는가: unmap은 매핑을 없애는 동작이라
 * 옛 매핑이 하나라도 캐시에 남아 있으면 보안 문제가 된다. 범위를 정확히
 * 계산하는 것보다 통째로 비우는 편이 안전하고, unmap 빈도가 높지 않아
 * 성능 손해도 감수할 만하다는 판단이다.
 *
 * 실행 컨텍스트: unmap 마무리 단계.
 *
 * 호출 체인:
 *   iommu_unmap()의 마무리 → domain_ops->iotlb_sync
 *   → [sun50i_iommu_iotlb_sync] → sun50i_iommu_flush_iotlb_all()
 */
static void sun50i_iommu_iotlb_sync(struct iommu_domain *domain,
				    struct iommu_iotlb_gather *gather)
{
	/* [한국어] 범위를 따지지 않고 전체를 비운다. */
	sun50i_iommu_flush_iotlb_all(domain);
}

/*
 * [한국어]
 * sun50i_iommu_enable - IOMMU 하드웨어를 켠다
 *
 * @iommu: 대상 인스턴스. iommu->domain에 페이징 도메인이 이미 연결되어
 *         있어야 한다.
 * @return: 0 성공, 리셋/클록/플러시 실패 시 그 errno.
 *
 * 이 함수가 이 드라이버의 하드웨어 기동 절차 전체다. 순서가 중요하다:
 *  1) 리셋 해제 → 클록 활성화. 이 둘 없이는 레지스터가 응답하지 않는다.
 *  2) TTB 레지스터에 DT의 DMA 주소를 기록한다.
 *  3) 여섯 마스터의 TLB 프리페치를 켠다(성능; 대신 무효화 범위가 넓어진다).
 *  4) 바이패스를 0으로 — 모든 마스터가 변환을 거치게 한다.
 *  5) 폴트 인터럽트를 활성화한다.
 *  6) 권한 도메인 세 개를 구성한다. 이것이 ACI 모델의 실체다:
 *     - ACI_NONE: 여섯 마스터 모두 읽기와 쓰기를 금지.
 *     - ACI_RD: 쓰기만 금지(=읽기 허용).
 *     - ACI_WR: 읽기만 금지(=쓰기 허용).
 *     - ACI_RD_WR은 아예 설정하지 않는다 — 금지 비트가 없는 것이 곧 전면 허용.
 *  7) 전체 TLB 플러시. 실패하면 옛 매핑이 남을 수 있으므로 초기화를 중단한다.
 *  8) 자동 게이팅과 IOMMU 활성화를 켠다. 마지막 줄이 변환의 시작이다.
 *
 * 초반 `if (!iommu->domain) return 0`에 주목: 도메인이 없으면 켤 대상이
 * 없으므로 조용히 성공한다. probe 직후 항등 도메인 상태에서 불릴 수 있다.
 *
 * 실행 컨텍스트: attach 경로(프로세스 컨텍스트). 클록/리셋 조작은
 * 잠들 수 있어 락 밖에서 하고, 레지스터 설정만 락 안에서 한다.
 *
 * 호출 체인:
 *   sun50i_iommu_attach_domain() → [sun50i_iommu_enable]
 *   → reset_control_deassert(), clk_prepare_enable(), flush_all_tlb()
 */
static int sun50i_iommu_enable(struct sun50i_iommu *iommu)
{
	/* [한국어] TTB에 기록할 DT 주소를 얻기 위한 도메인 포인터. */
	struct sun50i_iommu_domain *sun50i_domain;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 각 단계의 결과. */
	int ret;

	/* [한국어] 붙어 있는 도메인이 없다면 켤 이유가 없다. 항등 도메인
	 * 상태에서 불릴 수 있는 경로다. */
	if (!iommu->domain)
		return 0;

	/* [한국어] 그 도메인을 이 드라이버의 형태로 복원한다. */
	sun50i_domain = to_sun50i_domain(iommu->domain);

	/* [한국어] 리셋을 푼다. 이것이 없으면 레지스터가 응답하지 않는다. */
	ret = reset_control_deassert(iommu->reset);
	if (ret)	/* [한국어] 리셋을 풀지 못하면 레지스터가 응답하지 않는다. */
		return ret;

	/* [한국어] 클록을 켠다. 실패하면 리셋을 되돌려야 한다. */
	ret = clk_prepare_enable(iommu->clk);
	if (ret)	/* [한국어] 클록 활성화 실패 — 리셋을 되돌리러 간다. */
		goto err_reset_assert;

	/* [한국어] 이제부터 레지스터를 만지므로 직렬화한다. */
	spin_lock_irqsave(&iommu->iommu_lock, flags);

	/* [한국어] 1단계 DT의 물리 주소를 하드웨어에 알린다. 이 한 줄이
	 * 소프트웨어의 테이블과 하드웨어를 잇는 지점이다. */
	iommu_write(iommu, IOMMU_TTB_REG, sun50i_domain->dt_dma);
	/* [한국어] 여섯 마스터의 TLB 프리페치를 모두 켠다. 변환 지연을 줄이는
	 * 대신, 무효화 시 경계 바깥까지 비워야 하는 부담이 생긴다
	 * (sun50i_iommu_zap_range 참조). */
	iommu_write(iommu, IOMMU_TLB_PREFETCH_REG,
		    IOMMU_TLB_PREFETCH_MASTER_ENABLE(0) |
		    IOMMU_TLB_PREFETCH_MASTER_ENABLE(1) |
		    IOMMU_TLB_PREFETCH_MASTER_ENABLE(2) |
		    IOMMU_TLB_PREFETCH_MASTER_ENABLE(3) |
		    IOMMU_TLB_PREFETCH_MASTER_ENABLE(4) |
		    IOMMU_TLB_PREFETCH_MASTER_ENABLE(5));
	/* [한국어] 바이패스를 모두 해제해 어떤 마스터도 변환을 우회하지
	 * 못하게 한다. */
	iommu_write(iommu, IOMMU_BYPASS_REG, 0);
	/* [한국어] 관심 있는 모든 폴트에 인터럽트를 내게 한다. */
	iommu_write(iommu, IOMMU_INT_ENABLE_REG, IOMMU_INT_MASK);
	/* [한국어] ACI_NONE 도메인을 구성한다 — 여섯 마스터 각각의 읽기와
	 * 쓰기를 모두 금지한다. PTE의 ACI가 이 번호이면 그 페이지는
	 * 어떤 접근도 허용되지 않는다. */
	iommu_write(iommu, IOMMU_DM_AUT_CTRL_REG(SUN50I_IOMMU_ACI_NONE),
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 0) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 0) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 1) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 1) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 2) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 2) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 3) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 3) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 4) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 4) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 5) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_NONE, 5));

	/* [한국어] ACI_RD 도메인 — 쓰기만 금지한다. 읽기 금지 비트를 세우지
	 * 않았으므로 읽기는 허용된다. 즉 "읽기 전용" 페이지가 된다. */
	iommu_write(iommu, IOMMU_DM_AUT_CTRL_REG(SUN50I_IOMMU_ACI_RD),
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_RD, 0) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_RD, 1) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_RD, 2) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_RD, 3) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_RD, 4) |
		    IOMMU_DM_AUT_CTRL_WR_UNAVAIL(SUN50I_IOMMU_ACI_RD, 5));

	/* [한국어] ACI_WR 도메인 — 읽기만 금지해 "쓰기 전용" 페이지를 만든다.
	 * ACI_RD_WR(4번)은 여기서 설정하지 않는데, 금지 비트가 하나도 없는
	 * 상태가 곧 전면 허용이기 때문이다 — 레지스터를 건드리지 않는 것이
	 * 곧 그 설정이다. */
	iommu_write(iommu, IOMMU_DM_AUT_CTRL_REG(SUN50I_IOMMU_ACI_WR),
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_WR, 0) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_WR, 1) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_WR, 2) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_WR, 3) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_WR, 4) |
		    IOMMU_DM_AUT_CTRL_RD_UNAVAIL(SUN50I_IOMMU_ACI_WR, 5));

	/* [한국어] 부트로더가 남긴 옛 매핑이 캐시에 있을 수 있으므로
	 * 전체를 비운다. */
	ret = sun50i_iommu_flush_all_tlb(iommu);
	/* [한국어] 플러시가 실패했다면 옛 변환이 남은 채로 IOMMU를 켜게 되어
	 * 위험하다. 락을 풀고 클록/리셋을 되돌린다. */
	if (ret) {
		spin_unlock_irqrestore(&iommu->iommu_lock, flags);	/* [한국어] 실패 경로에서도 락을 반드시 푼다. */
		goto err_clk_disable;	/* [한국어] 클록과 리셋을 역순으로 되돌리러 간다. */
	}

	/* [한국어] 자동 클록 게이팅을 켜 유휴 시 전력을 아낀다. */
	iommu_write(iommu, IOMMU_AUTO_GATING_REG, IOMMU_AUTO_GATING_ENABLE);
	/* [한국어] 마지막으로 IOMMU를 활성화한다. 이 순간부터 여섯 마스터의
	 * DMA가 DT를 거쳐 변환된다. */
	iommu_write(iommu, IOMMU_ENABLE_REG, IOMMU_ENABLE_ENABLE);

	spin_unlock_irqrestore(&iommu->iommu_lock, flags);

	/* [한국어] 기동 완료. */
	return 0;

/* [한국어] 플러시 실패 시의 되감기 — 클록부터 끈다. */
err_clk_disable:
	clk_disable_unprepare(iommu->clk);

/* [한국어] 클록 실패 시의 되감기 — 리셋을 다시 건다. */
err_reset_assert:
	reset_control_assert(iommu->reset);

	/* [한국어] 실패 원인을 반환한다. */
	return ret;
}

/*
 * [한국어]
 * sun50i_iommu_disable - IOMMU 하드웨어를 끈다
 *
 * @iommu: 대상 인스턴스.
 * @return: 없음.
 *
 * 순서: 변환 비활성화 → TTB 지우기 → 클록 끄기 → 리셋 걸기.
 * TTB를 0으로 지우는 것이 중요한데, 곧 해제될 DT 메모리를 하드웨어가
 * 계속 가리키고 있으면 안 되기 때문이다.
 *
 * 클록과 리셋 조작을 락 밖에서 하는 이유: 그 함수들이 잠들 수 있어
 * 스핀락 안에서 부를 수 없다.
 *
 * 실행 컨텍스트: detach 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   sun50i_iommu_detach_domain() → [sun50i_iommu_disable]
 */
static void sun50i_iommu_disable(struct sun50i_iommu *iommu)
{
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] 레지스터 접근을 직렬화한다. */
	spin_lock_irqsave(&iommu->iommu_lock, flags);

	/* [한국어] 변환을 끈다. 이 시점부터 마스터의 DMA는 차단되거나
	 * 바이패스된다(하드웨어 정의에 따른다). */
	iommu_write(iommu, IOMMU_ENABLE_REG, 0);
	/* [한국어] 테이블 베이스를 지운다 — 곧 해제될 DT를 하드웨어가
	 * 가리키지 않게 하는 안전장치다. */
	iommu_write(iommu, IOMMU_TTB_REG, 0);

	spin_unlock_irqrestore(&iommu->iommu_lock, flags);

	/* [한국어] 클록을 끈다. 잠들 수 있어 락 밖에서 한다. */
	clk_disable_unprepare(iommu->clk);
	/* [한국어] 리셋을 걸어 하드웨어를 초기 상태로 되돌린다. */
	reset_control_assert(iommu->reset);
}

/*
 * [한국어]
 * sun50i_iommu_alloc_page_table - 2단계 페이지 테이블을 하나 할당한다
 *
 * @iommu: 대상 인스턴스(슬랩 캐시와 DMA 디바이스를 얻는다).
 * @gfp: 할당 플래그.
 * @return: 테이블의 커널 가상 주소, 실패하면 ERR_PTR(-ENOMEM).
 *
 * 슬랩 캐시를 쓰는 이유: PT가 1KB라 페이지보다 작고, 1KB 정렬이 필요하며
 * (DTE가 하위 10비트를 생략하므로), 4GB 이하여야 한다(DTE가 32비트).
 * probe에서 만든 캐시가 SLAB_HWCACHE_ALIGN | SLAB_CACHE_DMA32로 그
 * 세 조건을 모두 만족시킨다.
 *
 * WARN_ON의 의미: 이 드라이버는 DMA 주소와 물리 주소가 같다고 전제한다
 * (table_flush가 virt_to_phys를 DMA 주소로 그대로 쓴다). 그 전제가
 * 깨지면 캐시 동기화가 엉뚱한 곳을 향하므로 경고로 드러낸다.
 *
 * 실행 컨텍스트: 매핑 경로. gfp에 따라 atomic일 수 있다.
 *
 * 호출 체인:
 *   sun50i_dte_get_page_table() → [sun50i_iommu_alloc_page_table]
 */
static void *sun50i_iommu_alloc_page_table(struct sun50i_iommu *iommu,
					   gfp_t gfp)
{
	/* [한국어] 매핑 결과 DMA 주소. */
	dma_addr_t pt_dma;
	/* [한국어] 할당한 테이블의 커널 가상 주소. */
	u32 *page_table;

	/* [한국어] 전용 슬랩 캐시에서 0 초기화된 1KB를 받는다.
	 * 0으로 시작해야 모든 PTE가 무효 상태가 된다. */
	page_table = kmem_cache_zalloc(iommu->pt_pool, gfp);
	/* [한국어] 메모리 부족 — 오류 포인터로 알린다. */
	if (!page_table)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 테이블을 DMA로 매핑해 하드웨어가 볼 수 있게 한다.
	 * DMA_TO_DEVICE는 CPU가 쓰고 하드웨어가 읽는다는 뜻이다. */
	pt_dma = dma_map_single(iommu->dev, page_table, PT_SIZE, DMA_TO_DEVICE);
	/* [한국어] 매핑 실패 시 슬랩 객체를 반납하고 오류를 알린다. */
	if (dma_mapping_error(iommu->dev, pt_dma)) {
		dev_err(iommu->dev, "Couldn't map L2 Page Table\n");	/* [한국어] 2단계 테이블을 하드웨어에 보이게 만들지 못했다. */
		kmem_cache_free(iommu->pt_pool, page_table);	/* [한국어] 방금 받은 슬랩 객체를 반납한다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 메모리 부족을 오류 포인터로 알린다. */
	}

	/* We rely on the physical address and DMA address being the same */
	/* [한국어] 이 드라이버의 핵심 전제를 검증한다. table_flush()가
	 * virt_to_phys를 DMA 주소로 그대로 쓰므로, 두 값이 다르면
	 * 캐시 동기화가 엉뚱한 주소를 향하게 된다. */
	WARN_ON(pt_dma != virt_to_phys(page_table));

	/* [한국어] 준비된 테이블을 돌려준다. */
	return page_table;
}

/*
 * [한국어]
 * sun50i_iommu_free_page_table - 2단계 페이지 테이블을 반납한다
 *
 * @iommu: 대상 인스턴스.
 * @page_table: 반납할 테이블의 커널 가상 주소.
 * @return: 없음.
 *
 * 순서: DMA 언매핑을 먼저 하고 슬랩에 반납한다. 반대로 하면 해제된
 * 메모리에 대한 DMA 매핑을 만지게 된다.
 *
 * DMA 주소를 보관하지 않고 다시 계산하는 점에 주목: 두 값이 같다는
 * 전제(alloc의 WARN_ON이 검증) 덕분에 가능한 단순화다.
 *
 * 실행 컨텍스트: 설치 경쟁 패배 시와 detach 경로.
 *
 * 호출 체인:
 *   sun50i_dte_get_page_table() / sun50i_iommu_detach_domain()
 *   → [sun50i_iommu_free_page_table]
 */
static void sun50i_iommu_free_page_table(struct sun50i_iommu *iommu,
					 u32 *page_table)
{
	/* [한국어] DMA 언매핑에 쓸 물리 주소를 다시 계산한다. */
	phys_addr_t pt_phys = virt_to_phys(page_table);

	/* [한국어] 먼저 DMA 매핑을 푼다. */
	dma_unmap_single(iommu->dev, pt_phys, PT_SIZE, DMA_TO_DEVICE);
	/* [한국어] 그다음 슬랩 캐시에 반납한다. */
	kmem_cache_free(iommu->pt_pool, page_table);
}

/*
 * [한국어]
 * sun50i_dte_get_page_table - IOVA에 해당하는 2단계 테이블을 얻는다(없으면 만든다)
 *
 * @sun50i_domain: 대상 도메인.
 * @iova: 매핑할 IOVA.
 * @gfp: 새 테이블 할당에 쓸 플래그.
 * @return: 2단계 테이블의 커널 가상 주소, 실패하면 ERR_PTR.
 *
 * 경쟁 처리가 이 함수의 핵심이다. 두 CPU가 같은 1MB 영역 안의 서로 다른
 * 4KB를 동시에 매핑하면, 둘 다 그 영역의 PT가 없다고 보고 각자 만들어
 * 설치하려 한다. cmpxchg가 하나만 이기게 하고, 진 쪽은 자기 테이블을
 * 버린 뒤 이긴 쪽의 것을 쓴다.
 *
 * 두 번의 table_flush에 주목: 새 테이블 전체(256 엔트리)와 그것을 가리키는
 * DTE 하나를 각각 동기화한다. 순서가 이 방향이어야 하드웨어가 유효한
 * DTE를 보았을 때 그 테이블 내용도 이미 메모리에 있다. 다만 경쟁에서
 * 진 경우에도 이긴 쪽의 테이블을 다시 flush 하는데, 중복이지만 무해하다.
 *
 * 실행 컨텍스트: 매핑 경로. 락이 없어 cmpxchg로 자체 방어한다.
 *
 * 호출 체인:
 *   sun50i_iommu_map() → [sun50i_dte_get_page_table]
 *   → sun50i_iommu_alloc_page_table(), cmpxchg()
 */
static u32 *sun50i_dte_get_page_table(struct sun50i_iommu_domain *sun50i_domain,
				      dma_addr_t iova, gfp_t gfp)
{
	/* [한국어] 테이블 할당과 해제에 필요한 IOMMU 인스턴스. */
	struct sun50i_iommu *iommu = sun50i_domain->iommu;
	/* [한국어] 반환할 2단계 테이블. */
	u32 *page_table;
	/* [한국어] 해당 DTE의 주소. */
	u32 *dte_addr;
	/* [한국어] cmpxchg 이전의 DTE 값(경쟁 판별용). */
	u32 old_dte;
	/* [한국어] 읽거나 만든 DTE 값. */
	u32 dte;

	/* [한국어] IOVA에서 1단계 인덱스를 구해 그 DTE의 주소를 얻는다. */
	dte_addr = &sun50i_domain->dt[sun50i_iova_get_dte_index(iova)];
	/* [한국어] 현재 값을 읽는다. */
	dte = *dte_addr;
	/* [한국어] 이미 유효한 테이블이 걸려 있다면 그것을 그대로 쓴다 —
	 * 가장 흔한 경로다. */
	if (sun50i_dte_is_pt_valid(dte)) {
		/* [한국어] DTE에서 테이블의 물리 주소를 뽑는다. */
		phys_addr_t pt_phys = sun50i_dte_get_pt_address(dte);
		/* [한국어] 커널 가상 주소로 바꿔 반환한다. 테이블이 lowmem에
		 * 있으므로 phys_to_virt가 통한다. */
		return (u32 *)phys_to_virt(pt_phys);
	}

	/* [한국어] 없으면 새 테이블을 만든다. */
	page_table = sun50i_iommu_alloc_page_table(iommu, gfp);
	/* [한국어] 할당 실패는 오류 포인터로 그대로 전달한다. */
	if (IS_ERR(page_table))
		return page_table;

	/* [한국어] 새 테이블을 가리키는 DTE 값을 만든다. */
	dte = sun50i_mk_dte(virt_to_phys(page_table));
	/* [한국어] DTE가 여전히 0일 때만 설치된다. 0이 아닌 값이 돌아오면
	 * 다른 CPU가 먼저 설치했다는 뜻이다. */
	old_dte = cmpxchg(dte_addr, 0, dte);
	/* [한국어] 경쟁에서 진 경우의 처리. */
	if (old_dte) {
		/* [한국어] 이긴 쪽이 설치한 테이블의 물리 주소. */
		phys_addr_t installed_pt_phys =
			sun50i_dte_get_pt_address(old_dte);
		/* [한국어] 그 테이블의 커널 가상 주소. */
		u32 *installed_pt = phys_to_virt(installed_pt_phys);
		/* [한국어] 내가 만들었지만 버릴 테이블. */
		u32 *drop_pt = page_table;

		/* [한국어] 앞으로 쓸 것은 이긴 쪽의 테이블이다. */
		page_table = installed_pt;
		/* [한국어] DTE 값도 이긴 쪽의 것으로 바꾼다. */
		dte = old_dte;
		/* [한국어] 내가 만든 테이블을 반납한다. */
		sun50i_iommu_free_page_table(iommu, drop_pt);
	}

	/* [한국어] 테이블 전체(256 엔트리)를 하드웨어에 보이게 만든다.
	 * 경쟁에서 진 경우에는 이미 이긴 쪽이 flush 했겠지만, 중복은 무해하다. */
	sun50i_table_flush(sun50i_domain, page_table, NUM_PT_ENTRIES);
	/* [한국어] 그것을 가리키는 DTE도 동기화한다. 테이블 내용을 먼저
	 * 밀어낸 뒤 DTE를 밀어내는 순서라, 하드웨어가 유효한 DTE를 볼 때는
	 * 테이블도 이미 메모리에 있다. */
	sun50i_table_flush(sun50i_domain, dte_addr, 1);

	/* [한국어] 쓸 준비가 된 2단계 테이블을 돌려준다. */
	return page_table;
}

/*
 * [한국어]
 * sun50i_iommu_map - IOVA 하나에 물리 페이지를 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 IOVA.
 * @paddr: 매핑할 물리 주소.
 * @size: 페이지 크기(항상 4KB).
 * @count: 페이지 개수 — 이 구현은 한 번에 하나만 처리한다.
 * @prot: 보호 플래그(ACI로 변환된다).
 * @gfp: 2단계 테이블 할당 플래그.
 * @mapped: 출력 인자 — 매핑한 바이트 수.
 * @return: 0 성공, -EINVAL(4GB 초과), -EBUSY(이미 매핑됨), PT 할당 오류.
 *
 * 이 하드웨어는 입력과 출력 모두 32비트 주소만 다룬다. 그래서 4GB를 넘는
 * 물리 주소는 매핑할 수 없고, 그 검사가 함수의 첫 단계다.
 * dev_warn_once를 쓰는 이유: 이런 요청은 시스템 구성 문제라 반복해서
 * 나기 쉬운데, 매번 로그를 남기면 콘솔이 가득 찬다.
 *
 * count 인자를 무시하고 항상 하나만 처리하는 점에 주목: *mapped에 size만
 * 기록하므로 코어가 나머지를 다시 요청한다. 페이지 크기가 4KB 하나뿐이라
 * 묶음 처리의 이득이 작다는 판단이다.
 *
 * 실행 컨텍스트: 매핑 경로. 락 없이 테이블만 만진다 — 코어의 도메인 락이
 * 직렬화를 맡고, PT 설치만 cmpxchg로 방어한다.
 *
 * 호출 체인:
 *   iommu_map() → domain_ops->map_pages → [sun50i_iommu_map]
 *   → sun50i_dte_get_page_table(), sun50i_mk_pte(), sun50i_table_flush()
 */
static int sun50i_iommu_map(struct iommu_domain *domain, unsigned long iova,
			    phys_addr_t paddr, size_t size, size_t count,
			    int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sun50i_iommu_domain *sun50i_domain = to_sun50i_domain(domain);
	/* [한국어] 로깅과 PT 할당에 필요한 IOMMU 인스턴스. */
	struct sun50i_iommu *iommu = sun50i_domain->iommu;
	/* [한국어] 2단계 테이블 안에서의 인덱스. */
	u32 pte_index;
	/* [한국어] 2단계 테이블과 그 안의 대상 엔트리 주소. */
	u32 *page_table, *pte_addr;
	/* [한국어] 결과 코드. */
	int ret = 0;

	/* the IOMMU can only handle 32-bit addresses, both input and output */
	/* [한국어] 물리 주소가 4GB를 넘으면 PTE에 담을 수 없다.
	 * dev_warn_once인 이유는 시스템 구성 문제라 반복되기 쉽기 때문이다. */
	if ((uint64_t)paddr >> 32) {
		ret = -EINVAL;	/* [한국어] 4GB를 넘는 물리 주소는 32비트 PTE에 담을 수 없다. */
		dev_warn_once(iommu->dev,	/* [한국어] 시스템 구성 문제라 반복되기 쉬우므로 한 번만 경고한다. */
			      "attempt to map address beyond 4GB\n");
		goto out;	/* [한국어] 정리 없이 곧바로 반환 지점으로 간다. */
	}

	/* [한국어] 이 IOVA에 해당하는 2단계 테이블을 얻는다. 없으면 만든다. */
	page_table = sun50i_dte_get_page_table(sun50i_domain, iova, gfp);
	/* [한국어] 테이블을 만들지 못했다면 오류를 그대로 전달한다. */
	if (IS_ERR(page_table)) {
		ret = PTR_ERR(page_table);	/* [한국어] 2단계 테이블을 얻지 못한 오류를 그대로 전달한다. */
		goto out;	/* [한국어] 반환 지점으로 간다. */
	}

	/* [한국어] 테이블 안에서의 인덱스를 구한다. */
	pte_index = sun50i_iova_get_pte_index(iova);
	/* [한국어] 그 엔트리의 주소. */
	pte_addr = &page_table[pte_index];
	/* [한국어] 이미 매핑된 자리라면 덮어쓰지 않는다 — IOMMU API 규약상
	 * 먼저 unmap 해야 한다. unlikely로 표시한 것은 정상 흐름에서
	 * 거의 일어나지 않기 때문이다. */
	if (unlikely(sun50i_pte_is_page_valid(*pte_addr))) {
		/* [한국어] 진단을 돕기 위해 기존 매핑의 물리 주소를 뽑는다. */
		phys_addr_t page_phys = sun50i_pte_get_page_address(*pte_addr);
		dev_err(iommu->dev,	/* [한국어] 기존 매핑과 새 요청을 함께 남겨 어떤 충돌인지 알린다. */
			"iova %pad already mapped to %pa cannot remap to %pa prot: %#x\n",
			&iova, &page_phys, &paddr, prot);
		ret = -EBUSY;	/* [한국어] 덮어쓰기는 규약 위반이므로 사용 중으로 보고한다. */
		goto out;	/* [한국어] 반환 지점으로 간다. */
	}

	/* [한국어] 물리 주소와 권한(ACI)을 합친 PTE를 기록한다. */
	*pte_addr = sun50i_mk_pte(paddr, prot);
	/* [한국어] 그 엔트리를 하드웨어에 보이게 만든다. */
	sun50i_table_flush(sun50i_domain, pte_addr, 1);
	/* [한국어] 한 페이지만 처리했음을 코어에 알린다. 코어가 나머지를
	 * 다시 요청하게 된다. */
	*mapped = size;

/* [한국어] 성공과 모든 실패가 모이는 지점. 정리할 자원이 없어 반환만 한다. */
out:
	return ret;	/* [한국어] 성공이면 0, 아니면 위에서 정한 오류 코드다. */
}

/*
 * [한국어]
 * sun50i_iommu_unmap - IOVA 하나의 매핑을 제거한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 IOVA.
 * @size: 페이지 크기(항상 4KB).
 * @count: 페이지 개수 — 역시 하나만 처리한다.
 * @gather: TLB 무효화 수집 구조체 — 쓰지 않는다(iotlb_sync가 전체를 비운다).
 * @return: 해제한 바이트 수(SZ_4K), 매핑이 없으면 0.
 *
 * 2단계 테이블은 비어도 회수하지 않는 점에 주목: 같은 1MB 영역이 다시
 * 매핑될 가능성이 높고, 비었는지 확인하려면 256개 엔트리를 모두 훑어야
 * 하기 때문이다. 모든 PT는 detach 시점에 한꺼번에 회수된다.
 *
 * 실행 컨텍스트: 해제 경로. 락 없이 테이블만 만진다.
 *
 * 호출 체인:
 *   iommu_unmap() → domain_ops->unmap_pages → [sun50i_iommu_unmap]
 */
static size_t sun50i_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
				 size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sun50i_iommu_domain *sun50i_domain = to_sun50i_domain(domain);
	/* [한국어] 2단계 테이블의 물리 주소. */
	phys_addr_t pt_phys;
	/* [한국어] 지울 엔트리의 주소. */
	u32 *pte_addr;
	/* [한국어] 1단계 엔트리 값. */
	u32 dte;

	/* [한국어] IOVA에 해당하는 DTE를 읽는다. */
	dte = sun50i_domain->dt[sun50i_iova_get_dte_index(iova)];
	/* [한국어] 그 영역에 2단계 테이블 자체가 없다면 매핑도 없다. */
	if (!sun50i_dte_is_pt_valid(dte))
		return 0;

	/* [한국어] 테이블의 물리 주소를 뽑는다. */
	pt_phys = sun50i_dte_get_pt_address(dte);
	/* [한국어] 가상 주소로 바꾼 뒤 인덱스를 더해 대상 엔트리를 가리킨다. */
	pte_addr = (u32 *)phys_to_virt(pt_phys) + sun50i_iova_get_pte_index(iova);

	/* [한국어] 그 자리가 이미 비어 있다면 해제할 것이 없다. */
	if (!sun50i_pte_is_page_valid(*pte_addr))
		return 0;

	/* [한국어] 엔트리를 0으로 지운다 — 유효 비트가 사라져 매핑이 없어진다. */
	memset(pte_addr, 0, sizeof(*pte_addr));
	/* [한국어] 지운 결과를 하드웨어에 보이게 만든다. TLB 무효화는
	 * 코어가 나중에 iotlb_sync로 요청한다. */
	sun50i_table_flush(sun50i_domain, pte_addr, 1);

	/* [한국어] 한 페이지를 해제했음을 알린다. */
	return SZ_4K;
}

/*
 * [한국어]
 * sun50i_iommu_iova_to_phys - 소프트웨어 워크로 IOVA를 물리 주소로 바꾼다
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * 2단계 워크를 그대로 따라간다: DTE 확인 → 2단계 테이블 → PTE 확인 →
 * 페이지 주소 + 오프셋. 매 단계에서 유효성을 확인해 중간에 끊기면 0을 준다.
 *
 * 실행 컨텍스트: 조회 경로. 읽기만 하므로 부작용이 없다.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops->iova_to_phys
 *   → [sun50i_iommu_iova_to_phys]
 */
static phys_addr_t sun50i_iommu_iova_to_phys(struct iommu_domain *domain,
					     dma_addr_t iova)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sun50i_iommu_domain *sun50i_domain = to_sun50i_domain(domain);
	/* [한국어] 2단계 테이블의 물리 주소. */
	phys_addr_t pt_phys;
	/* [한국어] 그 테이블의 커널 가상 주소. */
	u32 *page_table;
	/* [한국어] 읽은 1단계/2단계 엔트리. */
	u32 dte, pte;

	/* [한국어] 1단계 엔트리를 읽는다. */
	dte = sun50i_domain->dt[sun50i_iova_get_dte_index(iova)];
	/* [한국어] 그 영역에 테이블이 없으면 매핑도 없다. */
	if (!sun50i_dte_is_pt_valid(dte))
		return 0;

	/* [한국어] 2단계 테이블의 물리 주소를 뽑는다. */
	pt_phys = sun50i_dte_get_pt_address(dte);
	/* [한국어] 커널 가상 주소로 바꾼다. */
	page_table = (u32 *)phys_to_virt(pt_phys);
	/* [한국어] 2단계 인덱스로 엔트리를 읽는다. */
	pte = page_table[sun50i_iova_get_pte_index(iova)];
	/* [한국어] 그 엔트리가 무효하면 이 IOVA는 매핑되지 않았다. */
	if (!sun50i_pte_is_page_valid(pte))
		return 0;

	/* [한국어] 페이지 정렬 물리 주소에 페이지 내 오프셋을 더해
	 * 최종 주소를 만든다. */
	return sun50i_pte_get_page_address(pte) +
		sun50i_iova_get_page_offset(iova);
}

/*
 * [한국어]
 * sun50i_iommu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 요청한 디바이스(사용하지 않는다).
 * @return: 새 도메인의 iommu_domain 포인터, 실패하면 NULL.
 *
 * 여기서는 1단계 DT만 만든다. 하드웨어 등록(TTB 기록과 IOMMU 활성화)은
 * 첫 attach가 sun50i_iommu_attach_domain()에서 수행한다 — 그때가
 * 되어야 어느 IOMMU에 붙을지 알 수 있기 때문이다.
 *
 * GFP_DMA32가 중요하다: DT의 물리 주소가 32비트 TTB 레지스터에 담겨야
 * 하므로 4GB 아래에서 받아야 한다.
 *
 * refcount를 1로 시작하는 이유: 도메인 생성 자체가 한 참조를 잡는 셈이라,
 * 첫 attach 후에는 2가 된다. identity_attach가 하나씩 줄이다 0이 되면
 * 마지막 detach로 판단한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   iommu_domain_alloc() → iommu_ops->domain_alloc_paging
 *   → [sun50i_iommu_domain_alloc_paging] → iommu_alloc_pages_sz()
 */
static struct iommu_domain *
sun50i_iommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 새로 만들 도메인. */
	struct sun50i_iommu_domain *sun50i_domain;

	/* [한국어] 0으로 초기화해 할당한다. iommu와 dt_dma가 0으로 시작해
	 * "아직 attach 전"임을 나타낸다. */
	sun50i_domain = kzalloc_obj(*sun50i_domain);
	if (!sun50i_domain)	/* [한국어] 도메인 구조체를 할당하지 못했다. */
		return NULL;

	/* [한국어] 1단계 DT(16KB)를 할당한다. GFP_DMA32로 4GB 아래에서 받아야
	 * 32비트 TTB 레지스터에 주소가 담긴다. 0으로 초기화되어 모든 DTE가
	 * 무효 상태로 시작한다. */
	sun50i_domain->dt =
		iommu_alloc_pages_sz(GFP_KERNEL | GFP_DMA32, DT_SIZE);
	if (!sun50i_domain->dt)	/* [한국어] 1단계 DT를 확보하지 못했으니 도메인을 되돌리러 간다. */
		goto err_free_domain;

	/* [한국어] 참조 카운트를 1로 시작한다. 도메인 존재 자체가 한 참조다. */
	refcount_set(&sun50i_domain->refcnt, 1);

	/* [한국어] 지원 페이지 크기는 4KB 하나뿐이다. */
	sun50i_domain->domain.pgsize_bitmap = SZ_4K;

	/* [한국어] IOVA 공간의 시작. */
	sun50i_domain->domain.geometry.aperture_start = 0;
	/* [한국어] 끝을 4GB-1로 둔다 — 이 하드웨어의 32비트 주소 한계다. */
	sun50i_domain->domain.geometry.aperture_end = DMA_BIT_MASK(32);
	/* [한국어] 코어가 이 범위를 강제하게 해, 범위 밖 IOVA 요청이 아예
	 * 들어오지 않게 한다. */
	sun50i_domain->domain.geometry.force_aperture = true;

	/* [한국어] 코어에는 임베드된 일반 도메인 포인터를 돌려준다. */
	return &sun50i_domain->domain;

/* [한국어] DT 할당 실패 시의 되감기. */
err_free_domain:
	kfree(sun50i_domain);	/* [한국어] DT 없이 만들어진 도메인 구조체를 반납한다. */

	return NULL;	/* [한국어] 도메인을 만들지 못했음을 코어에 알린다. */
}

/*
 * [한국어]
 * sun50i_iommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 2단계 테이블들은 여기서 해제하지 않는다 — detach 시점에
 * sun50i_iommu_detach_domain()이 이미 전부 회수했기 때문이다.
 * 코어가 항상 detach를 먼저 수행한다는 전제에 기대고 있다.
 *
 * dt를 NULL로 지우는 것은 방어적 습관이다 — 곧 kfree될 구조체라
 * 실질적인 효과는 없지만, use-after-free를 조금 더 잡기 쉽게 만든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free → [sun50i_iommu_domain_free]
 */
static void sun50i_iommu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 이 드라이버의 도메인으로 복원한다. */
	struct sun50i_iommu_domain *sun50i_domain = to_sun50i_domain(domain);

	/* [한국어] 1단계 DT를 반납한다. */
	iommu_free_pages(sun50i_domain->dt);
	/* [한국어] 포인터를 지워 해제된 메모리를 가리키지 않게 한다. */
	sun50i_domain->dt = NULL;

	/* [한국어] 도메인 구조체를 반납한다. */
	kfree(sun50i_domain);
}

/*
 * [한국어]
 * sun50i_iommu_attach_domain - 도메인을 하드웨어에 연결하고 IOMMU를 켠다
 *
 * @iommu: 대상 인스턴스.
 * @sun50i_domain: 붙일 도메인.
 * @return: 0 성공, -ENOMEM(DT의 DMA 매핑 실패), enable이 낸 errno.
 *
 * 세 단계로 이뤄진다:
 *  1) 양방향 연결을 만든다 — IOMMU는 도메인을, 도메인은 IOMMU를 가리킨다.
 *     table_flush()가 도메인에서 IOMMU를 찾으므로 이 연결이 먼저 필요하다.
 *  2) DT를 DMA로 매핑해 하드웨어가 볼 수 있게 하고 그 주소를 기억한다.
 *  3) sun50i_iommu_enable()로 하드웨어를 기동한다.
 *
 * DMA 매핑 실패 시 1단계의 연결을 되돌리지 않는 점에 주목: iommu->domain이
 * 유효하지 않은 도메인을 가리킨 채 남는다. 실무에서는 이 매핑이
 * 실패하지 않는다는 전제로 보인다.
 *
 * 실행 컨텍스트: attach 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   sun50i_iommu_attach_device() → [sun50i_iommu_attach_domain]
 *   → dma_map_single(), sun50i_iommu_enable()
 */
static int sun50i_iommu_attach_domain(struct sun50i_iommu *iommu,
				      struct sun50i_iommu_domain *sun50i_domain)
{
	/* [한국어] IOMMU가 이 도메인을 가리키게 한다. enable()과 폴트
	 * 핸들러가 이 포인터를 쓴다. */
	iommu->domain = &sun50i_domain->domain;
	/* [한국어] 도메인도 IOMMU를 가리키게 한다. table_flush()와 map()이
	 * 이 포인터로 DMA 디바이스와 슬랩 캐시를 찾는다. */
	sun50i_domain->iommu = iommu;

	/* [한국어] 1단계 DT를 DMA로 매핑해 하드웨어가 읽을 수 있게 한다.
	 * 반환된 주소를 TTB 레지스터에 쓰게 된다. */
	sun50i_domain->dt_dma = dma_map_single(iommu->dev, sun50i_domain->dt,
					       DT_SIZE, DMA_TO_DEVICE);
	/* [한국어] 매핑 실패 — 하드웨어가 테이블을 볼 수 없으므로 진행할 수 없다. */
	if (dma_mapping_error(iommu->dev, sun50i_domain->dt_dma)) {
		dev_err(iommu->dev, "Couldn't map L1 Page Table\n");	/* [한국어] 하드웨어가 1단계 테이블을 볼 수 없으면 변환이 불가능하다. */
		return -ENOMEM;	/* [한국어] 매핑 실패를 메모리 부족으로 보고한다. */
	}

	/* [한국어] 리셋 해제부터 활성화까지 하드웨어 기동 절차를 수행한다. */
	return sun50i_iommu_enable(iommu);
}

/*
 * [한국어]
 * sun50i_iommu_detach_domain - 도메인을 하드웨어에서 떼어내고 모든 PT를 해제한다
 *
 * @iommu: 대상 인스턴스.
 * @sun50i_domain: 떼어낼 도메인.
 * @return: 없음.
 *
 * 4096개 DTE를 전부 훑으며 유효한 것마다 그 2단계 테이블을 해제한다.
 * unmap이 빈 테이블을 회수하지 않으므로, 여기서 한꺼번에 정리하는 것이다.
 *
 * 순서에 주목: 테이블을 모두 해제한 **뒤에** IOMMU를 끈다. 반대로 하면
 * 아직 살아 있는 하드웨어가 해제된 테이블을 걸을 수 있다 — 다만 그
 * 사이에도 DTE는 이미 0으로 지워진 상태라 위험이 크지는 않다.
 *
 * 마지막에 DT의 DMA 매핑을 풀고 iommu->domain을 NULL로 만든다.
 * DT 메모리 자체는 domain_free가 해제한다.
 *
 * 실행 컨텍스트: 마지막 detach 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   sun50i_iommu_identity_attach()(refcount가 0이 될 때)
 *   → [sun50i_iommu_detach_domain] → sun50i_iommu_free_page_table(),
 *     sun50i_iommu_disable()
 */
static void sun50i_iommu_detach_domain(struct sun50i_iommu *iommu,
				       struct sun50i_iommu_domain *sun50i_domain)
{
	/* [한국어] DTE 순회 인덱스. */
	unsigned int i;

	/* [한국어] 4096개 DTE를 모두 훑는다. 큰 루프이지만 detach는
	 * 드물게 일어나므로 비용이 문제되지 않는다. */
	for (i = 0; i < NUM_DT_ENTRIES; i++) {
		/* [한국어] 이 DTE가 가리키는 테이블의 물리 주소. */
		phys_addr_t pt_phys;
		/* [한국어] 그 테이블의 커널 가상 주소. */
		u32 *page_table;
		/* [한국어] 현재 DTE의 주소. */
		u32 *dte_addr;
		/* [한국어] 그 값. */
		u32 dte;

		/* [한국어] i번째 DTE를 가리킨다. */
		dte_addr = &sun50i_domain->dt[i];
		/* [한국어] 값을 읽는다. */
		dte = *dte_addr;
		/* [한국어] 테이블이 없는 자리는 건너뛴다. */
		if (!sun50i_dte_is_pt_valid(dte))
			continue;

		/* [한국어] DTE를 먼저 지운다 — 하드웨어가 곧 해제될 테이블을
		 * 가리키지 않게 하는 순서다. */
		memset(dte_addr, 0, sizeof(*dte_addr));
		/* [한국어] 지운 결과를 하드웨어에 보이게 만든다. */
		sun50i_table_flush(sun50i_domain, dte_addr, 1);

		/* [한국어] 미리 읽어 둔 값에서 테이블 주소를 복원한다. */
		pt_phys = sun50i_dte_get_pt_address(dte);
		/* [한국어] 커널 가상 주소로 바꾼다. */
		page_table = phys_to_virt(pt_phys);
		/* [한국어] 그 테이블을 반납한다(DMA 언매핑 포함). */
		sun50i_iommu_free_page_table(iommu, page_table);
	}


	/* [한국어] 모든 테이블을 정리했으니 하드웨어를 끈다.
	 * 변환 비활성화 → TTB 지우기 → 클록 → 리셋 순서다. */
	sun50i_iommu_disable(iommu);

	/* [한국어] 1단계 DT의 DMA 매핑을 푼다. 메모리 자체는 domain_free가
	 * 해제하므로 여기서는 매핑만 해제한다. */
	dma_unmap_single(iommu->dev, virt_to_phys(sun50i_domain->dt),
			 DT_SIZE, DMA_TO_DEVICE);

	/* [한국어] IOMMU가 더 이상 어떤 도메인도 가리키지 않게 한다. */
	iommu->domain = NULL;
}

/*
 * [한국어]
 * sun50i_iommu_identity_attach - 항등 도메인으로 전환한다(= detach)
 *
 * @identity_domain: 정적 항등 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(사용하지 않는다 — iommu->domain을 본다).
 * @return: 항상 0.
 *
 * 이 드라이버에는 별도의 detach 콜백이 없다. "항등 도메인으로 돌아간다"는
 * 요청이 곧 detach이며, 그 처리가 여기 모여 있다.
 *
 * refcount가 이 함수의 핵심이다. 여러 디바이스가 같은 도메인을 공유하므로,
 * 하나가 떨어져 나갈 때마다 하드웨어를 끄면 나머지가 망가진다.
 * refcount_dec_and_test가 0이 되는 순간(= 마지막 사용자)만 실제 detach를 한다.
 *
 * 첫 검사의 의미: 이미 항등 상태라면 되돌릴 것이 없다. 이 검사가 없으면
 * to_sun50i_domain(항등 도메인)으로 엉뚱한 메모리를 해석하게 된다.
 *
 * 실행 컨텍스트: detach/도메인 전환 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iommu_detach_device() / sun50i_iommu_attach_device()
 *   → [sun50i_iommu_identity_attach] → sun50i_iommu_detach_domain()
 */
static int sun50i_iommu_identity_attach(struct iommu_domain *identity_domain,
					struct device *dev,
					struct iommu_domain *old)
{
	/* [한국어] of_xlate가 심어 둔 IOMMU 인스턴스. */
	struct sun50i_iommu *iommu = dev_iommu_priv_get(dev);
	/* [한국어] 현재 붙어 있는 페이징 도메인. */
	struct sun50i_iommu_domain *sun50i_domain;

	dev_dbg(dev, "Detaching from IOMMU domain\n");

	/* [한국어] 이미 항등 상태라면 할 일이 없다. 이 검사가 아래
	 * to_sun50i_domain()이 항등 도메인을 잘못 해석하는 것을 막는다. */
	if (iommu->domain == identity_domain)
		return 0;

	/* [한국어] 현재 페이징 도메인을 이 드라이버의 형태로 복원한다. */
	sun50i_domain = to_sun50i_domain(iommu->domain);
	/* [한국어] 참조를 하나 줄이고, 0이 되면(= 마지막 사용자였다면)
	 * 실제로 하드웨어를 끄고 모든 테이블을 회수한다. */
	if (refcount_dec_and_test(&sun50i_domain->refcnt))
		sun50i_iommu_detach_domain(iommu, sun50i_domain);
	return 0;	/* [한국어] 참조만 올리고 끝냈으므로 성공이다. */
}

/* [한국어] 항등 도메인의 연산 테이블. attach_dev 하나뿐인 이유는
 * 이 도메인이 "변환 없음" 상태를 표현할 뿐 자체 상태가 없기 때문이다. */
static struct iommu_domain_ops sun50i_iommu_identity_ops = {
	/* [한국어] 페이징 도메인에서 빠져나오는(= detach) 처리를 담당한다. */
	.attach_dev = sun50i_iommu_identity_attach,
};

/* [한국어] 정적 항등 도메인. probe가 iommu->domain의 초기값으로 쓰고,
 * 코어의 "IOMMU 우회" 요청도 이 도메인으로 표현된다. */
static struct iommu_domain sun50i_iommu_identity_domain = {
	/* [한국어] 코어가 항등 도메인임을 알아보는 종류 표시. */
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 위에서 정의한 콜백 하나짜리 테이블. */
	.ops = &sun50i_iommu_identity_ops,
};

/*
 * [한국어]
 * sun50i_iommu_attach_device - 디바이스를 페이징 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙일 디바이스.
 * @old: 직전 도메인(identity_attach에 그대로 전달한다).
 * @return: 0 성공, -ENODEV(담당 IOMMU를 찾지 못함).
 *
 * 순서가 미묘하다:
 *  1) refcount를 먼저 올린다. 아래에서 identity_attach를 부를 때
 *     그것이 refcount를 내리는데, 미리 올려 두지 않으면 같은 도메인에
 *     다시 붙는 경우 0이 되어 하드웨어가 꺼져 버린다.
 *  2) 이미 이 도메인이 붙어 있으면(두 번째 디바이스가 같은 도메인에
 *     붙는 경우) 참조만 올리고 끝낸다.
 *  3) 다른 도메인이 붙어 있었다면 identity_attach로 그것을 정리한 뒤
 *     새 도메인을 연결한다.
 *
 * 이 하드웨어는 도메인을 하나만 가질 수 있으므로, 3번의 전환이
 * "옛 도메인의 모든 매핑이 사라진다"는 뜻임에 유의해야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 코어가 그룹 뮤텍스로 직렬화한다.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev
 *   → [sun50i_iommu_attach_device] → sun50i_iommu_identity_attach(),
 *     sun50i_iommu_attach_domain()
 */
static int sun50i_iommu_attach_device(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	/* [한국어] 붙일 도메인을 이 드라이버의 형태로 복원한다. */
	struct sun50i_iommu_domain *sun50i_domain = to_sun50i_domain(domain);
	/* [한국어] 담당 IOMMU 인스턴스. */
	struct sun50i_iommu *iommu;

	/* [한국어] of_xlate가 심어 둔 IOMMU를 꺼낸다. */
	iommu = sun50i_iommu_from_dev(dev);
	/* [한국어] 이 디바이스는 IOMMU를 쓰지 않는다는 뜻이다. */
	if (!iommu)
		return -ENODEV;

	dev_dbg(dev, "Attaching to IOMMU domain\n");

	/* [한국어] 참조를 먼저 올린다. 아래 identity_attach가 참조를 내리므로,
	 * 같은 도메인에 다시 붙는 경우 0이 되어 하드웨어가 꺼지는 것을 막는다.
	 * 순서가 뒤바뀌면 미묘한 버그가 된다. */
	refcount_inc(&sun50i_domain->refcnt);

	/* [한국어] 이미 이 도메인이 붙어 있다면(두 번째 디바이스가 같은
	 * 도메인을 공유하는 경우) 참조만 올리고 끝낸다. */
	if (iommu->domain == domain)
		return 0;

	/* [한국어] 다른 도메인(또는 항등 도메인)이 붙어 있었으므로 정리한다.
	 * 마지막 사용자였다면 하드웨어가 꺼지고 모든 테이블이 회수된다. */
	sun50i_iommu_identity_attach(&sun50i_iommu_identity_domain, dev, old);

	/* [한국어] 새 도메인을 하드웨어에 연결하고 IOMMU를 켠다.
	 * 반환값을 검사하지 않는 점은 기존 코드의 한계다 — 기동이 실패해도
	 * attach는 성공으로 보고된다. */
	sun50i_iommu_attach_domain(iommu, sun50i_domain);

	return 0;	/* [한국어] 마스터 ID 등록까지 끝났으므로 성공이다. */
}

/*
 * [한국어]
 * sun50i_iommu_probe_device - 이 디바이스를 담당할 IOMMU를 코어에 알린다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 IOMMU의 핸들, 없으면 ERR_PTR(-ENODEV).
 *
 * of_xlate가 먼저 실행되어 priv에 IOMMU를 심어 두었어야 한다.
 * iommus 프로퍼티가 없는 디바이스는 priv가 비어 있어 여기서 걸러진다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   iommu_probe_device() → iommu_ops->probe_device
 *   → [sun50i_iommu_probe_device]
 */
static struct iommu_device *sun50i_iommu_probe_device(struct device *dev)
{
	/* [한국어] 담당 IOMMU 인스턴스. */
	struct sun50i_iommu *iommu;

	/* [한국어] of_xlate가 심어 둔 포인터를 꺼낸다. */
	iommu = sun50i_iommu_from_dev(dev);
	/* [한국어] 비어 있다면 이 디바이스는 IOMMU를 쓰지 않는다. */
	if (!iommu)
		return ERR_PTR(-ENODEV);

	/* [한국어] 코어 핸들을 돌려주면 이 디바이스가 해당 IOMMU 소속으로
	 * 등록된다. */
	return &iommu->iommu;
}

/*
 * [한국어]
 * sun50i_iommu_of_xlate - 디바이스 트리의 iommus 프로퍼티를 해석한다
 *
 * @dev: iommus 프로퍼티를 가진 클라이언트 디바이스.
 * @args: 파싱된 항목. args->np가 IOMMU 노드, args->args[0]이 마스터 ID다.
 * @return: iommu_fwspec_add_ids()의 결과(0 또는 음수 errno).
 *
 * 두 가지를 한다: IOMMU 인스턴스를 클라이언트의 priv에 심고,
 * 마스터 ID를 fwspec에 등록한다. 마스터 ID는 이 드라이버가 직접 쓰지는
 * 않지만(권한 도메인을 여섯 마스터 모두에 동일하게 구성하므로),
 * 코어가 요구하는 절차라 채워 둔다.
 *
 * 참조 카운트: of_find_device_by_node()가 참조를 올리므로
 * put_device()로 내려야 한다. drvdata 포인터는 IOMMU가 살아 있는 한
 * 유효하므로 참조를 유지할 필요가 없다.
 *
 * 반환값 검사가 없는 점에 주목: of_find_device_by_node가 NULL을 반환하면
 * platform_get_drvdata(NULL)에서 문제가 생긴다 — 디바이스 트리가
 * 올바르다는 전제다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   of_iommu_configure() → iommu_ops->of_xlate → [sun50i_iommu_of_xlate]
 */
static int sun50i_iommu_of_xlate(struct device *dev,
				 const struct of_phandle_args *args)
{
	/* [한국어] phandle이 가리키는 IOMMU 플랫폼 디바이스. 참조가 하나 올라간다. */
	struct platform_device *iommu_pdev = of_find_device_by_node(args->np);
	/* [한국어] iommus 프로퍼티의 첫 인자 = 이 디바이스의 마스터 ID(0~5). */
	unsigned id = args->args[0];

	/* [한국어] IOMMU 인스턴스를 클라이언트의 priv에 심는다. 이후
	 * probe_device와 attach가 이 포인터로 IOMMU를 찾는다. */
	dev_iommu_priv_set(dev, platform_get_drvdata(iommu_pdev));

	/* [한국어] 올린 참조를 내린다. drvdata 포인터는 IOMMU가 살아 있는
	 * 동안 유효하므로 참조를 유지할 필요가 없다. */
	put_device(&iommu_pdev->dev);

	/* [한국어] 마스터 ID를 fwspec에 등록한다. 이 드라이버는 그 값을
	 * 직접 쓰지 않지만 코어의 절차상 필요하다. */
	return iommu_fwspec_add_ids(dev, &id, 1);
}

/* [한국어] IOMMU 코어에 노출하는 이 드라이버의 연산 테이블. */
static const struct iommu_ops sun50i_iommu_ops = {
	/* [한국어] "변환 없음" 상태를 표현하는 정적 항등 도메인.
	 * detach가 이 도메인으로의 전환으로 구현되어 있다. */
	.identity_domain = &sun50i_iommu_identity_domain,
	/* [한국어] 코어의 헬퍼로 디바이스마다 단독 그룹을 만든다.
	 * 도메인은 하나뿐이지만 그룹은 나눠도 무방하다. */
	.device_group	= generic_single_device_group,
	/* [한국어] 페이징 도메인 생성 — DT만 만들고 하드웨어 등록은 attach로 미룬다. */
	.domain_alloc_paging = sun50i_iommu_domain_alloc_paging,
	/* [한국어] iommus 프로퍼티 해석 — IOMMU를 priv에 심고 마스터 ID를 등록한다. */
	.of_xlate	= sun50i_iommu_of_xlate,
	/* [한국어] 디바이스 담당 판정. */
	.probe_device	= sun50i_iommu_probe_device,
	/* [한국어] 페이징 도메인의 연산 테이블(익명 const 구조체). */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		/* [한국어] refcount를 올리고, 처음이라면 하드웨어를 기동한다. */
		.attach_dev	= sun50i_iommu_attach_device,
		/* [한국어] 전체 TLB/PTW 캐시 플러시. */
		.flush_iotlb_all = sun50i_iommu_flush_iotlb_all,
		/* [한국어] 매핑 추가 후 그 범위(+프리페치 여유)를 무효화한다. */
		.iotlb_sync_map = sun50i_iommu_iotlb_sync_map,
		/* [한국어] 해제 후에는 범위를 따지지 않고 전체를 비운다. */
		.iotlb_sync	= sun50i_iommu_iotlb_sync,
		/* [한국어] 2단계 소프트웨어 워크로 물리 주소를 조회한다. */
		.iova_to_phys	= sun50i_iommu_iova_to_phys,
		/* [한국어] 한 번에 한 페이지씩 매핑한다. */
		.map_pages	= sun50i_iommu_map,
		/* [한국어] 한 번에 한 페이지씩 해제한다. */
		.unmap_pages	= sun50i_iommu_unmap,
		/* [한국어] DT를 반납하고 도메인 구조체를 해제한다. */
		.free		= sun50i_iommu_domain_free,
	}
};

/*
 * [한국어]
 * sun50i_iommu_report_fault - 폴트를 로그와 상위 계층에 보고하고 캐시를 비운다
 *
 * @iommu: 대상 인스턴스.
 * @master: 폴트를 일으킨 마스터 번호(0~5).
 * @iova: 폴트가 난 주소.
 * @prot: IOMMU_FAULT_READ 또는 IOMMU_FAULT_WRITE.
 * @return: 없음.
 *
 * 세 가지를 한다: 에러 로그를 남기고, 도메인에 등록된 폴트 핸들러에
 * 알리고, 그 주소 주변의 캐시를 비운다.
 *
 * 마지막 zap_range가 중요하다: 폴트를 일으킨 매핑 없음 결과가 캐시에
 * 남아 있으면, 나중에 그 주소를 제대로 매핑해도 여전히 폴트가 난다.
 * 그래서 폴트 처리의 일부로 그 주변을 비운다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러 안. iommu_lock을 이미 쥔 상태여야
 * zap_range의 assert가 통과한다.
 *
 * 호출 체인:
 *   handle_pt_irq() / handle_perm_irq() → [sun50i_iommu_report_fault]
 *   → report_iommu_fault(), sun50i_iommu_zap_range()
 */
static void sun50i_iommu_report_fault(struct sun50i_iommu *iommu,
				      unsigned master, phys_addr_t iova,
				      unsigned prot)
{
	/* [한국어] 어느 마스터가 어느 주소에 어떤 방향으로 접근하다 실패했는지
	 * 남긴다. 진단에 필요한 정보가 이 한 줄에 모여 있다. */
	dev_err(iommu->dev, "Page fault for %pad (master %d, dir %s)\n",
		&iova, master, (prot == IOMMU_FAULT_WRITE) ? "wr" : "rd");

	/* [한국어] 도메인에 등록된 폴트 핸들러가 있으면 그쪽에도 알린다.
	 * 일부 드라이버는 이 알림으로 페이지를 채워 넣기도 한다. */
	if (iommu->domain)
		report_iommu_fault(iommu->domain, iommu->dev, iova, prot);
	else
		/* [한국어] 도메인 없이 폴트가 났다는 것은 설정이 어긋났다는
		 * 뜻이다 — 물음표가 붙은 메시지가 그 당혹감을 드러낸다. */
		dev_err(iommu->dev, "Page fault while iommu not attached to any domain?\n");

	/* [한국어] 폴트를 유발한 "매핑 없음" 결과가 캐시에 남지 않게 비운다.
	 * 이것을 빼먹으면 나중에 제대로 매핑해도 폴트가 반복된다. */
	sun50i_iommu_zap_range(iommu, iova, SPAGE_SIZE);
}

/*
 * [한국어]
 * sun50i_iommu_handle_pt_irq - 페이지 테이블 폴트(L1/L2)를 처리한다
 *
 * @iommu: 대상 인스턴스.
 * @addr_reg: 폴트 주소가 담긴 레지스터의 오프셋(L1용 또는 L2용).
 * @blame_reg: 마스터 비트맵이 담긴 레지스터의 오프셋.
 * @return: 폴트가 난 주소.
 *
 * L1(DTE 없음)과 L2(PTE 없음) 폴트를 같은 함수로 처리한다 — 레지스터
 * 오프셋만 다르고 처리 절차는 같기 때문이다. 호출자가 어느 쪽인지에 따라
 * 다른 오프셋을 넘긴다.
 *
 * ilog2로 마스터 번호를 뽑는 방식에 주목: blame 레지스터가 비트맵이고,
 * 여기서는 가장 낮은... 정확히는 최상위 1비트의 위치를 얻는다. 여러
 * 마스터가 동시에 폴트를 냈다면 하나만 보고되는 셈인데, 폴트 상황에서는
 * 정확한 집계보다 빠른 대응이 우선이라는 판단이다.
 *
 * 방향을 항상 READ로 보고하는 이유는 원본 주석에 있다: 주소가 페이지
 * 테이블에 아예 없으면 어떤 연산이 폴트를 일으켰는지 알 수 없다.
 * 권한 폴트(handle_perm_irq)와 달리 참고할 PTE가 없기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러 안. iommu_lock 보유(assert로 강제).
 *
 * 호출 체인:
 *   sun50i_iommu_irq() → [sun50i_iommu_handle_pt_irq]
 *   → sun50i_iommu_report_fault()
 */
static phys_addr_t sun50i_iommu_handle_pt_irq(struct sun50i_iommu *iommu,
					      unsigned addr_reg,
					      unsigned blame_reg)
{
	/* [한국어] 폴트가 난 주소. */
	phys_addr_t iova;
	/* [한국어] 폴트를 일으킨 마스터 번호. */
	unsigned master;
	/* [한국어] 마스터 비트맵. */
	u32 blame;

	/* [한국어] 호출자가 락을 쥐고 있음을 확인한다 — 아래
	 * report_fault가 zap_range를 부르는데 그것이 락을 요구한다. */
	assert_spin_locked(&iommu->iommu_lock);

	/* [한국어] 폴트 주소를 읽는다. L1이냐 L2냐에 따라 다른 레지스터다. */
	iova = iommu_read(iommu, addr_reg);
	/* [한국어] 어느 마스터가 원인인지 비트맵으로 읽는다. */
	blame = iommu_read(iommu, blame_reg);
	/* [한국어] 비트맵에서 마스터 번호를 뽑는다. 여러 비트가 서 있으면
	 * 최상위 것만 얻게 되지만, 폴트 대응에는 충분하다. */
	master = ilog2(blame & IOMMU_INT_MASTER_MASK);

	/*
	 * If the address is not in the page table, we can't get what
	 * operation triggered the fault. Assume it's a read
	 * operation.
	 */
	/* [한국어] 원본 주석대로 방향을 알 수 없어 읽기로 가정해 보고한다.
	 * 매핑 자체가 없으므로 참고할 PTE의 권한 정보도 없기 때문이다. */
	sun50i_iommu_report_fault(iommu, master, iova, IOMMU_FAULT_READ);

	/* [한국어] 폴트 주소를 반환한다. 호출자는 현재 이 값을 쓰지 않는다. */
	return iova;
}

/*
 * [한국어]
 * sun50i_iommu_handle_perm_irq - 권한 위반 폴트를 처리한다
 *
 * @iommu: 대상 인스턴스.
 * @return: 폴트가 난 주소.
 *
 * 페이지 테이블 폴트와 달리 여기서는 **방향을 추론할 수 있다**.
 * 매핑은 존재하지만 권한이 없어 거부된 경우이므로, 그 PTE의 ACI를 보면
 * 어느 방향이 금지되어 있었는지 알 수 있고, 그것이 곧 시도한 방향이다:
 *  - ACI가 RD(읽기 전용)인데 폴트가 났다 → 쓰기를 시도한 것이다.
 *  - ACI가 WR(쓰기 전용)이었다면 읽기를 시도한 것이지만, 코드는 이 경우를
 *    아래 NONE/RD_WR과 묶어 읽기로 처리한다(결과적으로 맞다).
 *  - ACI가 NONE이면 어느 쪽인지 알 수 없어 읽기로 가정한다.
 *  - ACI가 RD_WR인데 폴트가 났다면 있을 수 없는 일이다 — 원본 주석의
 *    "WTF?"가 그 당혹감을 드러낸다. 역시 읽기로 처리한다.
 * switch의 case들이 fallthrough로 이어져 마지막 default와 함께 처리되는
 * 구조라, 실질적으로는 "RD면 쓰기, 나머지는 읽기"가 된다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러 안. iommu_lock 보유.
 *
 * 호출 체인:
 *   sun50i_iommu_irq() → [sun50i_iommu_handle_perm_irq]
 *   → sun50i_get_pte_aci(), sun50i_iommu_report_fault()
 */
static phys_addr_t sun50i_iommu_handle_perm_irq(struct sun50i_iommu *iommu)
{
	/* [한국어] 폴트가 난 페이지의 권한 도메인 번호. */
	enum sun50i_iommu_aci aci;
	/* [한국어] 폴트가 난 주소. */
	phys_addr_t iova;
	/* [한국어] 폴트를 일으킨 마스터 번호. */
	unsigned master;
	/* [한국어] 추론한 접근 방향. */
	unsigned dir;
	/* [한국어] 마스터 비트맵. */
	u32 blame;

	/* [한국어] 호출자의 락 보유를 확인한다. */
	assert_spin_locked(&iommu->iommu_lock);

	/* [한국어] 인터럽트 상태에서 마스터 비트맵을 읽는다. */
	blame = iommu_read(iommu, IOMMU_INT_STA_REG);
	/* [한국어] 마스터 번호를 뽑는다. */
	master = ilog2(blame & IOMMU_INT_MASTER_MASK);
	/* [한국어] 그 마스터 전용 레지스터에서 폴트 주소를 읽는다.
	 * 권한 폴트는 마스터마다 별도 레지스터에 기록된다. */
	iova = iommu_read(iommu, IOMMU_INT_ERR_ADDR_REG(master));
	/* [한국어] 같은 마스터의 데이터 레지스터에서 그 PTE 값을 읽고,
	 * 거기서 ACI를 뽑는다. 이것이 방향 추론의 근거다. */
	aci = sun50i_get_pte_aci(iommu_read(iommu,
					    IOMMU_INT_ERR_DATA_REG(master)));

	/* [한국어] ACI 값으로 시도한 접근 방향을 역산한다. */
	switch (aci) {
		/*
		 * If we are in the read-only domain, then it means we
		 * tried to write.
		 */
	/* [한국어] 읽기 전용 페이지에서 폴트가 났다면 쓰기를 시도한 것이다.
	 * 유일하게 방향을 확실히 알 수 있는 경우다. */
	case SUN50I_IOMMU_ACI_RD:
		dir = IOMMU_FAULT_WRITE;	/* [한국어] 읽기 전용 페이지에서 폴트가 났으니 쓰기를 시도한 것이다. */
		break;

		/*
		 * If we are in the write-only domain, then it means
		 * we tried to read.
		 */
	/* [한국어] 쓰기 전용 페이지라면 읽기를 시도한 것이다.
	 * break가 없어 아래로 흘러 들어간다 — 결과가 같기 때문이다. */
	case SUN50I_IOMMU_ACI_WR:

		/*
		 * If we are in the domain without any permission, we
		 * can't really tell. Let's default to a read
		 * operation.
		 */
	/* [한국어] 모든 접근이 금지된 페이지라면 어느 쪽인지 알 수 없다.
	 * 역시 아래로 흘러 읽기로 처리된다. */
	case SUN50I_IOMMU_ACI_NONE:

		/* WTF? */
	/* [한국어] 전면 허용 페이지에서 권한 폴트가 났다면 하드웨어나
	 * 소프트웨어에 문제가 있다는 뜻이다. 원본의 "WTF?"가 그 상황을
	 * 표현하며, 달리 할 수 있는 것이 없어 읽기로 처리한다. */
	case SUN50I_IOMMU_ACI_RD_WR:
	default:	/* [한국어] 정의되지 않은 ACI 값도 같은 방식으로 처리한다. */
		dir = IOMMU_FAULT_READ;	/* [한국어] 방향을 확정할 수 없는 나머지 경우는 읽기로 가정한다. */
		break;
	}

	/*
	 * If the address is not in the page table, we can't get what
	 * operation triggered the fault. Assume it's a read
	 * operation.
	 */
	/* [한국어] 추론한 방향으로 폴트를 보고한다. 위 주석은
	 * handle_pt_irq에서 복사된 것으로 보이며, 이 함수의 상황과는
	 * 정확히 맞지 않는다(여기서는 방향을 추론할 수 있다). */
	sun50i_iommu_report_fault(iommu, master, iova, dir);

	/* [한국어] 폴트 주소를 반환한다. */
	return iova;
}

/*
 * [한국어]
 * sun50i_iommu_irq - IOMMU 인터럽트 핸들러
 *
 * @irq: 발생한 IRQ 번호(사용하지 않는다).
 * @dev_id: request_irq에 넘긴 IOMMU 인스턴스.
 * @return: IRQ_HANDLED(처리함) 또는 IRQ_NONE(내 인터럽트가 아님).
 *
 * 동작 과정:
 *  1) 상태 레지스터를 읽어 관심 있는 폴트인지 확인한다. 아니면 IRQ_NONE.
 *  2) L1/L2 폴트 비트맵을 읽어 둔다(마지막 리셋 계산에 쓴다).
 *  3) 폴트 종류에 따라 세 갈래로 처리한다 — L2 무효, L1 무효, 그 밖(권한).
 *     L2를 먼저 검사하는 이유: 둘 다 서 있다면 더 구체적인 정보를 가진
 *     L2 쪽이 유용하기 때문이다.
 *  4) 처리한 상태 비트를 지운다.
 *  5) 폴트를 일으킨 마스터들을 리셋한다. 이것이 이 핸들러의 핵심 복구
 *     동작인데, 폴트가 난 마스터는 멈춘 상태로 남아 리셋해야 재개된다.
 *     리셋 레지스터는 역논리(0을 쓰면 리셋)라 ~resets를 쓰고,
 *     곧바로 전 비트 1을 써서 리셋을 해제한다.
 *
 * 실행 컨텍스트: 하드 인터럽트 컨텍스트. spin_lock(irqsave가 아님)을
 * 쓰는데, 이미 인터럽트가 막힌 컨텍스트이기 때문이다.
 *
 * 호출 체인:
 *   하드웨어 IRQ → [sun50i_iommu_irq] → handle_pt_irq()/handle_perm_irq()
 */
static irqreturn_t sun50i_iommu_irq(int irq, void *dev_id)
{
	/* [한국어] 인터럽트 상태와 L1/L2 폴트 비트맵, 그리고 리셋할 마스터 마스크. */
	u32 status, l1_status, l2_status, resets;
	/* [한국어] request_irq에 넘긴 IOMMU 인스턴스. */
	struct sun50i_iommu *iommu = dev_id;

	/* [한국어] 레지스터 접근을 직렬화한다. 인터럽트 컨텍스트라
	 * irqsave가 필요 없다. */
	spin_lock(&iommu->iommu_lock);

	/* [한국어] 인터럽트 상태를 읽는다. */
	status = iommu_read(iommu, IOMMU_INT_STA_REG);
	/* [한국어] 관심 있는 비트가 하나도 없다면 내 인터럽트가 아니다.
	 * 공유 IRQ에서 다른 핸들러에 기회를 주기 위해 IRQ_NONE을 반환한다. */
	if (!(status & IOMMU_INT_MASK)) {
		spin_unlock(&iommu->iommu_lock);	/* [한국어] 내 인터럽트가 아니므로 락을 풀고 물러난다. */
		return IRQ_NONE;	/* [한국어] 공유 IRQ의 다른 핸들러에 기회를 준다. */
	}

	/* [한국어] L1 폴트의 마스터 비트맵을 읽어 둔다. */
	l1_status = iommu_read(iommu, IOMMU_L1PG_INT_REG);
	/* [한국어] L2 폴트의 마스터 비트맵도 읽어 둔다. 아래 리셋 계산에서
	 * 상태 레지스터와 합쳐 쓴다. */
	l2_status = iommu_read(iommu, IOMMU_L2PG_INT_REG);

	/* [한국어] L2(PTE) 무효 폴트 — 그 1MB 영역의 테이블은 있지만
	 * 해당 페이지가 매핑되지 않았다. 더 구체적인 정보라 먼저 검사한다. */
	if (status & IOMMU_INT_INVALID_L2PG)
		sun50i_iommu_handle_pt_irq(iommu,
					    IOMMU_INT_ERR_ADDR_L2_REG,
					    IOMMU_L2PG_INT_REG);
	/* [한국어] L1(DTE) 무효 폴트 — 그 1MB 영역에 테이블조차 없다. */
	else if (status & IOMMU_INT_INVALID_L1PG)
		sun50i_iommu_handle_pt_irq(iommu,
					   IOMMU_INT_ERR_ADDR_L1_REG,
					   IOMMU_L1PG_INT_REG);
	else
		/* [한국어] 페이지 테이블 폴트가 아니라면 권한 위반이다.
		 * 이 경우에만 접근 방향을 추론할 수 있다. */
		sun50i_iommu_handle_perm_irq(iommu);

	/* [한국어] 처리한 인터럽트 상태 비트를 지운다. */
	iommu_write(iommu, IOMMU_INT_CLR_REG, status);

	/* [한국어] 폴트를 일으킨 마스터들을 모은다. 세 레지스터의 마스터
	 * 비트를 OR 하는 이유는 폴트 종류마다 다른 곳에 기록되기 때문이다. */
	resets = (status | l1_status | l2_status) & IOMMU_INT_MASTER_MASK;
	/* [한국어] 그 마스터들을 리셋한다. 이 레지스터는 역논리라
	 * 0을 쓴 비트가 리셋되므로 ~resets를 쓴다. */
	iommu_write(iommu, IOMMU_RESET_REG, ~resets);
	/* [한국어] 곧바로 전 비트를 1로 써 리셋을 해제한다. 이 짧은
	 * 리셋 펄스가 멈춰 있던 마스터를 재개시킨다. */
	iommu_write(iommu, IOMMU_RESET_REG, IOMMU_RESET_RELEASE_ALL);

	spin_unlock(&iommu->iommu_lock);

	/* [한국어] 내 인터럽트를 처리했음을 알린다. */
	return IRQ_HANDLED;
}

/*
 * [한국어]
 * sun50i_iommu_probe - IOMMU 플랫폼 디바이스를 초기화한다
 *
 * @pdev: 디바이스 트리에서 매칭된 IOMMU 디바이스.
 * @return: 0 성공, 음수 errno(각 단계 실패).
 *
 * 동작 과정:
 *  1) 인스턴스 할당과 락 초기화. iommu->domain을 항등 도메인으로 두어
 *     "아직 페이징 도메인이 없다"는 상태를 표현한다.
 *  2) 2단계 PT용 슬랩 캐시를 만든다. 크기와 정렬을 모두 PT_SIZE(1KB)로
 *     주고 SLAB_CACHE_DMA32를 붙여, DTE에 담길 수 있는 주소를 보장한다.
 *  3) MMIO 매핑, IRQ, 클록, 리셋 라인을 차례로 확보한다.
 *  4) sysfs와 IOMMU 코어에 등록한다.
 *  5) 마지막으로 인터럽트 핸들러를 등록한다 — 코어 등록 후에 하는 것은
 *     핸들러가 도메인을 참조할 수 있기 때문이다.
 *
 * probe 함수가 driver 구조체가 아니라 builtin_platform_driver_probe의
 * 인자로 전달되는 점에 주목: 그 매크로는 probe를 __init 섹션에 두어
 * 부팅 후 메모리를 반납할 수 있게 한다(다만 이 코드는 __init 표시를
 * 붙이지 않았다).
 *
 * 실행 컨텍스트: 디바이스 probe(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → builtin_platform_driver_probe → [sun50i_iommu_probe]
 */
static int sun50i_iommu_probe(struct platform_device *pdev)
{
	/* [한국어] 만들 IOMMU 인스턴스. */
	struct sun50i_iommu *iommu;
	/* [한국어] 결과 코드와 IRQ 번호. */
	int ret, irq;

	/* [한국어] 인스턴스를 0으로 초기화해 할당한다. devm이라 자동 해제된다. */
	iommu = devm_kzalloc(&pdev->dev, sizeof(*iommu), GFP_KERNEL);
	if (!iommu)	/* [한국어] 인스턴스를 할당하지 못했다. */
		return -ENOMEM;
	/* [한국어] 레지스터 접근을 직렬화할 락을 초기화한다. */
	spin_lock_init(&iommu->iommu_lock);
	/* [한국어] 항등 도메인을 초기 상태로 둔다 — 아직 페이징 도메인이
	 * 붙지 않았음을 뜻하며, identity_attach의 첫 검사가 이것을 본다. */
	iommu->domain = &sun50i_iommu_identity_domain;
	/* [한국어] of_xlate가 이 인스턴스를 찾을 수 있게 drvdata에 심는다. */
	platform_set_drvdata(pdev, iommu);
	/* [한국어] DMA와 로깅에 쓸 device 포인터를 보관한다. */
	iommu->dev = &pdev->dev;

	/* [한국어] 2단계 PT 전용 슬랩 캐시를 만든다.
	 * 크기와 정렬을 모두 1KB로 주는 것이 핵심 — DTE가 주소의 하위
	 * 10비트를 생략하므로 1KB 정렬이 필수다.
	 * SLAB_CACHE_DMA32는 4GB 아래에서 받게 해 32비트 DTE에 담기게 한다.
	 * 캐시 이름으로 디바이스 이름을 쓰므로 /proc/slabinfo에서 구분된다. */
	iommu->pt_pool = kmem_cache_create(dev_name(&pdev->dev),
					   PT_SIZE, PT_SIZE,
					   SLAB_HWCACHE_ALIGN | SLAB_CACHE_DMA32,
					   NULL);
	if (!iommu->pt_pool)	/* [한국어] 2단계 테이블용 캐시 없이는 매핑을 만들 수 없다. */
		return -ENOMEM;

	/* [한국어] MMIO 레지스터 블록을 매핑한다. */
	iommu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(iommu->base)) {	/* [한국어] MMIO 매핑에 실패했다. */
		ret = PTR_ERR(iommu->base);	/* [한국어] 실패 원인을 꺼낸다. */
		goto err_free_cache;	/* [한국어] 슬랩 캐시를 되돌리러 간다. */
	}

	/* [한국어] 폴트 인터럽트 번호를 얻는다. */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {	/* [한국어] 인터럽트가 없으면 폴트를 감지할 수 없다. */
		ret = irq;	/* [한국어] 오류 코드를 그대로 전달한다. */
		goto err_free_cache;	/* [한국어] 슬랩 캐시를 되돌리러 간다. */
	}

	/* [한국어] IOMMU 클록을 얻는다. 이름 없는 첫 번째 클록이다. */
	iommu->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(iommu->clk)) {	/* [한국어] 클록 없이는 IOMMU를 켤 수 없다. */
		dev_err(&pdev->dev, "Couldn't get our clock.\n");	/* [한국어] 어느 자원이 없는지 남긴다. */
		ret = PTR_ERR(iommu->clk);	/* [한국어] 실패 원인을 꺼낸다. */
		goto err_free_cache;	/* [한국어] 슬랩 캐시를 되돌리러 간다. */
	}

	/* [한국어] 리셋 라인을 얻는다. 이 IOMMU는 리셋 상태로 시작하므로
	 * enable에서 풀어야 한다. */
	iommu->reset = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(iommu->reset)) {	/* [한국어] 리셋 라인 없이는 하드웨어를 기동할 수 없다. */
		dev_err(&pdev->dev, "Couldn't get our reset line.\n");	/* [한국어] 어느 자원이 없는지 남긴다. */
		ret = PTR_ERR(iommu->reset);	/* [한국어] 실패 원인을 꺼낸다. */
		goto err_free_cache;	/* [한국어] 슬랩 캐시를 되돌리러 간다. */
	}

	/* [한국어] /sys/class/iommu/ 아래에 이 IOMMU를 노출한다. */
	ret = iommu_device_sysfs_add(&iommu->iommu, &pdev->dev,
				     NULL, dev_name(&pdev->dev));
	if (ret)	/* [한국어] sysfs 등록 실패 — 슬랩 캐시를 되돌리러 간다. */
		goto err_free_cache;

	/* [한국어] IOMMU 코어에 연산 테이블을 등록한다. 이 시점부터
	 * of_xlate와 probe_device 콜백이 들어올 수 있다. */
	ret = iommu_device_register(&iommu->iommu, &sun50i_iommu_ops, &pdev->dev);
	if (ret)	/* [한국어] 코어 등록 실패 — sysfs부터 되감는다. */
		goto err_remove_sysfs;

	/* [한국어] 마지막으로 폴트 핸들러를 등록한다. 코어 등록 뒤에 하는
	 * 이유는 핸들러가 iommu->domain을 참조하기 때문이다. */
	ret = devm_request_irq(&pdev->dev, irq, sun50i_iommu_irq, 0,
			       dev_name(&pdev->dev), iommu);
	if (ret < 0)	/* [한국어] IRQ 등록 실패 — 코어 등록부터 되감는다. */
		goto err_unregister;

	/* [한국어] 초기화 완료. */
	return 0;

/* [한국어] IRQ 등록 실패 시의 되감기. */
err_unregister:
	iommu_device_unregister(&iommu->iommu);

/* [한국어] 코어 등록 실패 시의 되감기. */
err_remove_sysfs:
	iommu_device_sysfs_remove(&iommu->iommu);

/* [한국어] 자원 확보 실패가 모이는 지점 — 슬랩 캐시를 파괴한다.
 * MMIO/클록/리셋은 devm이 자동으로 정리하므로 여기 없다. */
err_free_cache:
	kmem_cache_destroy(iommu->pt_pool);	/* [한국어] devm이 관리하지 않는 유일한 자원이라 명시적으로 파괴한다. */

	return ret;	/* [한국어] 실패를 유발한 오류 코드를 반환한다. */
}

/* [한국어] 이 드라이버가 바인딩할 디바이스 트리 compatible 목록.
 * H6와 H616이 같은 IOMMU IP를 쓴다. */
static const struct of_device_id sun50i_iommu_dt[] = {
	/* [한국어] Allwinner H6의 IOMMU. */
	{ .compatible = "allwinner,sun50i-h6-iommu", },
	/* [한국어] Allwinner H616의 IOMMU — H6와 동일하게 다룬다. */
	{ .compatible = "allwinner,sun50i-h616-iommu", },
	/* [한국어] 배열 끝을 알리는 빈 항목. */
	{ /* sentinel */ },
};
/* [한국어] 모듈 자동 로딩을 위해 매칭 테이블을 메타데이터에 심는다.
 * 아래 builtin으로 등록되므로 실제 자동 로딩은 일어나지 않지만,
 * 관례상 남겨 둔다. */
MODULE_DEVICE_TABLE(of, sun50i_iommu_dt);

/* [한국어] 플랫폼 드라이버 정의.
 * probe 필드가 없는 점에 주목 — 아래 builtin_platform_driver_probe가
 * probe 함수를 별도 인자로 받아 등록하기 때문이다. */
static struct platform_driver sun50i_iommu_driver = {
	/* [한국어] 드라이버 공통 정보. */
	.driver		= {
		/* [한국어] sysfs와 로그에 나타날 이름. */
		.name			= "sun50i-iommu",
		/* [한국어] 디바이스 트리 매칭 테이블. */
		.of_match_table 	= sun50i_iommu_dt,
		/* [한국어] sysfs를 통한 수동 bind/unbind를 막는다.
		 * IOMMU를 임의로 언바인드하면 클라이언트의 DMA 매핑이 통째로
		 * 무효화되어 시스템이 손상되기 때문이다. */
		.suppress_bind_attrs	= true,
	}
};
/* [한국어] 빌트인 드라이버로 등록하되 probe 함수를 별도로 넘긴다.
 * 이 형태는 probe가 부팅 시 한 번만 불린다는 전제(디바이스 핫플러그 없음)를
 * 깔고 있으며, remove 경로도 만들지 않는다 — IOMMU는 제거할 수 없다. */
builtin_platform_driver_probe(sun50i_iommu_driver, sun50i_iommu_probe);

/* [한국어] modinfo에 표시될 설명. */
MODULE_DESCRIPTION("Allwinner H6 IOMMU driver");
/* [한국어] 이 드라이버를 커널에 올린 개발자. */
MODULE_AUTHOR("Maxime Ripard <maxime@cerno.tech>");
/* [한국어] 원 코드를 제공한 Allwinner 쪽 개발자. */
MODULE_AUTHOR("zhuxianbin <zhuxianbin@allwinnertech.com>");
