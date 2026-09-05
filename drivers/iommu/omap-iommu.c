// SPDX-License-Identifier: GPL-2.0-only
/*
 * omap iommu: tlb and pagetable primitives
 *
 * Copyright (C) 2008-2010 Nokia Corporation
 * Copyright (C) 2013-2017 Texas Instruments Incorporated - https://www.ti.com/
 *
 * Written by Hiroshi DOYU <Hiroshi.DOYU@nokia.com>,
 *		Paul Mundt and Toshihiro Kobayashi
 */

/*
 * [한국어 설명] TI OMAP/DRA7 IOMMU 드라이버 (omap-iommu.c)
 *
 * === 파일의 역할 ===
 * OMAP 계열 SoC의 MMU 블록(카메라 ISP, DSP, IPU 등에 딸린 것)을
 * 리눅스 IOMMU API에 붙인다. 페이지 테이블은 ARM 짧은 서술자와
 * 닮은 2단계 구조로, 네 가지 크기를 지원한다: 16MB 슈퍼섹션,
 * 1MB 섹션, 64KB 큰 페이지, 4KB 작은 페이지.
 *
 * 이 드라이버를 읽을 때 붙잡아야 할 것이 다섯이다.
 *
 * (1) **거울 프로그래밍(mirror programming).** 하나의 클라이언트
 *     디바이스가 여러 MMU 인스턴스 뒤에 있을 수 있다(DRA7의 DSP는
 *     MMU가 둘이다). 그 경우 **각 MMU가 자기만의 페이지 테이블을
 *     갖되 내용이 완전히 같다.** map은 모든 인스턴스에 같은 엔트리를
 *     쓰고, unmap도 모두에서 지운다. 조회는 첫 인스턴스만 보면 된다 —
 *     어차피 같기 때문이다.
 *
 * (2) **TLB가 소프트웨어에 노출되어 있다.** 이 하드웨어의 TLB는
 *     CAM/RAM 레지스터 쌍으로 직접 읽고 쓸 수 있고, MMU_LOCK의
 *     base와 victim 필드가 "몇 개까지 고정(preserve)되어 있고
 *     다음에 어느 자리를 덮어쓸 것인가"를 관리한다. 그래서
 *     절전 시 고정된 항목을 저장했다 복원하는 코드가 존재한다.
 *
 * (3) **한 도메인에 클라이언트는 하나뿐이다.** attach가 그것을
 *     명시적으로 거부한다. 대신 그 하나의 클라이언트가 여러
 *     MMU를 거느릴 수 있다는 것이 (1)의 이야기다.
 *
 * (4) **런타임 PM이 하드웨어 활성화 그 자체다.** iommu_enable()이
 *     하는 일은 pm_runtime_get_sync()뿐이고, 실제 레지스터 설정은
 *     런타임 리줌 콜백이 한다. 그래서 전원과 MMU 상태가 자연히
 *     묶인다.
 *
 * (5) **테이블은 DMA로 읽힌다.** 캐시 일관성이 없어 엔트리를
 *     고칠 때마다 flush_iopte_range()로 밀어내야 하고, DMA 주소와
 *     물리 주소가 같다는 전제를 WARN_ON으로 확인한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [디바이스 트리] iommus = <&mmu_a &mmu_b>
 *        ↓ probe_device
 *   [이 파일] arch_data 배열에 MMU들을 모아 디바이스에 매단다
 *        ↓ attach_dev
 *   [이 파일] MMU마다 테이블을 만들고 전원을 켠다
 *
 *   [클라이언트 드라이버] dma_map_*() 또는 직접 iommu_map()
 *        ↓
 *   [이 파일] omap_iommu_map() → 모든 MMU에 같은 엔트리를 쓴다
 *        ↓
 *   [하드웨어] MMU가 DMA로 테이블을 읽어 변환
 *        ↑ 폴트
 *   [이 파일] iommu_fault_handler() → 상위 보고, 실패 시 진단 출력
 *
 * 실행 컨텍스트: map/unmap은 page_table_lock 아래에서 돌지만
 * iopte_alloc이 그 락을 잠시 놓고 GFP_KERNEL 할당을 한다.
 * 인터럽트 핸들러는 인터럽트 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * - omap-iopgtable.h: 엔트리 형식과 인덱싱 매크로 전부
 *   (iopgd_offset, iopte_offset, IOPGD_SECTION 등).
 * - omap-iommu.h: struct omap_iommu와 레지스터 접근 래퍼,
 *   그리고 for_each_iotlb_cr 같은 TLB 순회 매크로.
 * - platform_data/iommu-omap.h: 리셋 라인과 전원 도메인 제약을
 *   다루는 플랫폼 콜백들(hwmod 시대의 잔재다).
 * - linux/omap-iommu.h: 클라이언트에게 노출되는 API
 *   (save_ctx/restore_ctx, domain_activate/deactivate).
 * 데이터 흐름: IOVA(여기서는 da) → 1단계 인덱스 → 2단계 인덱스
 * → 물리 주소. 각 단계의 엔트리 형식은 크기에 따라 다르다.
 *
 * === 주요 함수/구조체 요약 ===
 * - omap_iopgtable_store_entry(): 엔트리 하나를 심는다. 크기에
 *   따라 네 함수 중 하나로 갈린다.
 * - iopgtable_clear_entry_core(): 엔트리를 지우고, 2단계 테이블이
 *   완전히 비었으면 그 테이블까지 반납한다.
 * - omap_iommu_attach_dev(): 클라이언트의 모든 MMU에 각각 테이블을
 *   만들어 붙인다. 실패하면 역순으로 되돌린다.
 * - omap_iommu_save_tlb_entries()/restore(): 고정된 TLB 항목을
 *   절전 너머로 나른다.
 * - iotlb_lock: base(고정 항목 수)와 vict(다음 교체 대상)로
 *   TLB의 절반을 소프트웨어가 관리하게 하는 장치.
 */

/* [한국어] 페이지 테이블을 DMA 매핑해 캐시 일관성을 관리한다. */
#include <linux/dma-mapping.h>
/* [한국어] ERR_PTR/IS_ERR. */
#include <linux/err.h>
/* [한국어] kmem_cache — 2단계 테이블 전용 캐시. */
#include <linux/slab.h>
/* [한국어] 폴트 인터럽트 등록. */
#include <linux/interrupt.h>
/* [한국어] 레지스터 자원 정의. */
#include <linux/ioport.h>
/* [한국어] 플랫폼 드라이버 모델. */
#include <linux/platform_device.h>
/* [한국어] IOMMU 코어 계약. */
#include <linux/iommu.h>
/* [한국어] 클라이언트에게 노출되는 OMAP 전용 API의 선언
 * (save_ctx, domain_activate 등). */
#include <linux/omap-iommu.h>
/* [한국어] 뮤텍스. 현재 직접 쓰이지는 않는다. */
#include <linux/mutex.h>
/* [한국어] 두 개의 스핀락(테이블용, 인스턴스용). */
#include <linux/spinlock.h>
/* [한국어] MMIO 접근. */
#include <linux/io.h>
/* [한국어] 런타임 PM — 이 드라이버에서 하드웨어 활성화 그 자체다. */
#include <linux/pm_runtime.h>
/* [한국어] 디바이스 트리 파싱. */
#include <linux/of.h>
/* [한국어] 인터럽트 파싱. */
#include <linux/of_irq.h>
/* [한국어] of_find_device_by_node — probe_device가 MMU를 찾을 때. */
#include <linux/of_platform.h>
/* [한국어] regmap — DRA7 DSP의 시스템 설정 레지스터 접근. */
#include <linux/regmap.h>
/* [한국어] syscon — 그 설정 레지스터를 공유 영역에서 찾는다. */
#include <linux/mfd/syscon.h>

/* [한국어] 리셋 라인과 전원 도메인 제약을 다루는 플랫폼 콜백들.
 * OMAP의 hwmod 시대에서 이어진 층이다. */
#include <linux/platform_data/iommu-omap.h>

/* [한국어] 페이지 테이블 엔트리의 형식과 인덱싱 매크로 전부. */
#include "omap-iopgtable.h"
/* [한국어] struct omap_iommu, 레지스터 오프셋, TLB 순회 매크로. */
#include "omap-iommu.h"

/* [한국어] 연산 테이블의 전방 선언. probe가 정의보다 앞에서 참조한다. */
static const struct iommu_ops omap_iommu_ops;

/* [한국어] 플랫폼 디바이스에서 MMU 인스턴스 구조체를 꺼낸다.
 * probe가 platform_set_drvdata로 심어 둔 값이다. */
#define to_iommu(dev)	((struct omap_iommu *)dev_get_drvdata(dev))

/* bitmap of the page sizes currently supported */
/* [한국어] 지원하는 네 가지 매핑 크기.
 * 4KB 작은 페이지, 64KB 큰 페이지, 1MB 섹션, 16MB 슈퍼섹션이다.
 * 뒤의 둘은 1단계 엔트리로, 앞의 둘은 2단계 엔트리로 표현된다. */
#define OMAP_IOMMU_PGSIZES	(SZ_4K | SZ_64K | SZ_1M | SZ_16M)

/* [한국어] MMU_LOCK 레지스터에서 base 필드가 시작하는 비트.
 * base는 "앞에서부터 몇 개의 TLB 항목이 고정되어 있는가"를 뜻한다. */
#define MMU_LOCK_BASE_SHIFT	10
/* [한국어] 그 필드의 마스크(5비트 = 최대 32개). */
#define MMU_LOCK_BASE_MASK	(0x1f << MMU_LOCK_BASE_SHIFT)
/* [한국어] 레지스터 값에서 base를 뽑는다. */
#define MMU_LOCK_BASE(x)	\
	((x & MMU_LOCK_BASE_MASK) >> MMU_LOCK_BASE_SHIFT)

/* [한국어] victim 필드가 시작하는 비트.
 * victim은 "다음에 덮어쓸 TLB 자리"를 가리킨다. */
#define MMU_LOCK_VICT_SHIFT	4
/* [한국어] 그 필드의 마스크. */
#define MMU_LOCK_VICT_MASK	(0x1f << MMU_LOCK_VICT_SHIFT)
/* [한국어] 레지스터 값에서 victim을 뽑는다. */
#define MMU_LOCK_VICT(x)	\
	((x & MMU_LOCK_VICT_MASK) >> MMU_LOCK_VICT_SHIFT)

/* [한국어] 이 파일 끝에 정의되는 플랫폼 드라이버의 전방 선언. */
static struct platform_driver omap_iommu_driver;
/* [한국어] 2단계 페이지 테이블 전용 슬랩 캐시.
 * 설정자: omap_iommu_init()이 1KB 정렬로 만든다.
 * 왜 전용 캐시인가: 하드웨어가 2단계 테이블에 1KB 정렬을
 *                   요구하는데 kmalloc으로는 보장할 수 없다. */
static struct kmem_cache *iopte_cachep;

/**
 * to_omap_domain - Get struct omap_iommu_domain from generic iommu_domain
 * @dom:	generic iommu domain handle
 **/
/*
 * [한국어]
 * to_omap_domain - iommu_domain에서 바깥 도메인을 복원한다
 *
 * @dom: 코어가 넘겨준 도메인.
 * @return: 드라이버 쪽 도메인.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄.
 *
 * 호출 체인:
 *   각종 iommu_domain_ops 콜백 → [to_omap_domain]
 */
static struct omap_iommu_domain *to_omap_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드 멤버의 주소에서 바깥 구조체를 역산한다. */
	return container_of(dom, struct omap_iommu_domain, domain);
}

/**
 * omap_iommu_save_ctx - Save registers for pm off-mode support
 * @dev:	client device
 *
 * This should be treated as an deprecated API. It is preserved only
 * to maintain existing functionality for OMAP3 ISP driver.
 **/
/*
 * [한국어]
 * omap_iommu_save_ctx - MMU 레지스터를 통째로 떠 둔다(폐기 예정 API)
 *
 * @dev: 클라이언트 디바이스.
 * @return: 없음.
 *
 * OMAP3 ISP 드라이버 하나를 위해 남아 있는 옛 API다. 지금은
 * 런타임 PM 콜백이 같은 일을 더 정교하게 하지만, ISP는 자기
 * 전원 관리를 직접 하던 시절의 방식을 그대로 쓴다.
 *
 * 클라이언트의 모든 MMU를 훑는 while 루프에 주목 — arch_data가
 * NULL로 끝나는 배열이라, iommu_dev가 NULL인 항목이 끝을 뜻한다.
 * 이 관용구가 이 파일 곳곳에서 반복된다.
 *
 * 실행 컨텍스트: 클라이언트 드라이버의 절전 경로.
 *
 * 호출 체인:
 *   OMAP3 ISP 드라이버 → [omap_iommu_save_ctx]
 */
void omap_iommu_save_ctx(struct device *dev)
{
	/* [한국어] NULL로 끝나는 MMU 배열. */
	struct omap_iommu_arch_data *arch_data = dev_iommu_priv_get(dev);
	/* [한국어] 현재 다루는 MMU 인스턴스. */
	struct omap_iommu *obj;
	/* [한국어] 저장 버퍼(구조체 뒤에 딸려 있다). */
	u32 *p;
	/* [한국어] 레지스터 순회 인덱스. */
	int i;

	/* [한국어] 이 디바이스에 MMU가 없다. */
	if (!arch_data)
		return;

	/* [한국어] NULL 항목을 만날 때까지가 이 디바이스의 MMU들이다. */
	while (arch_data->iommu_dev) {
		obj = arch_data->iommu_dev;	/* [한국어] 이번에 다룰 MMU 인스턴스. */
		p = obj->ctx;
		/* [한국어] 레지스터 영역 전체를 워드 단위로 떠 둔다.
		 * 어느 레지스터가 중요한지 가리지 않는 거친 방식이다. */
		for (i = 0; i < (MMU_REG_SIZE / sizeof(u32)); i++) {
			p[i] = iommu_read_reg(obj, i * sizeof(u32));	/* [한국어] 레지스터 하나를 워드 단위로 떠 둔다. */
			dev_dbg(obj->dev, "%s\t[%02d] %08x\n", __func__, i,	/* [한국어] 떠 둔 값을 디버그 로그로 남긴다. */
				p[i]);
		}
		/* [한국어] 다음 MMU로 넘어간다. */
		arch_data++;
	}
}
/* [한국어] OMAP3 ISP 드라이버가 모듈일 수 있어 내보낸다. */
EXPORT_SYMBOL_GPL(omap_iommu_save_ctx);

/**
 * omap_iommu_restore_ctx - Restore registers for pm off-mode support
 * @dev:	client device
 *
 * This should be treated as an deprecated API. It is preserved only
 * to maintain existing functionality for OMAP3 ISP driver.
 **/
/*
 * [한국어]
 * omap_iommu_restore_ctx - 떠 둔 MMU 레지스터를 되돌린다(폐기 예정 API)
 *
 * @dev: 클라이언트 디바이스.
 * @return: 없음.
 *
 * save_ctx의 정확한 대칭이다. 읽기를 쓰기로 바꾼 것 말고는
 * 구조가 같다.
 *
 * 실행 컨텍스트: 클라이언트 드라이버의 재개 경로.
 *
 * 호출 체인:
 *   OMAP3 ISP 드라이버 → [omap_iommu_restore_ctx]
 */
void omap_iommu_restore_ctx(struct device *dev)
{
	/* [한국어] NULL로 끝나는 MMU 배열. */
	struct omap_iommu_arch_data *arch_data = dev_iommu_priv_get(dev);
	/* [한국어] 현재 다루는 MMU 인스턴스. */
	struct omap_iommu *obj;
	/* [한국어] 떠 둔 값들. */
	u32 *p;
	/* [한국어] 레지스터 순회 인덱스. */
	int i;

	/* [한국어] MMU가 없으면 되돌릴 것도 없다. */
	if (!arch_data)
		return;

	/* [한국어] 이 디바이스의 모든 MMU를 훑는다. */
	while (arch_data->iommu_dev) {
		obj = arch_data->iommu_dev;	/* [한국어] 이번에 다룰 MMU 인스턴스. */
		p = obj->ctx;
		/* [한국어] 떠 두었던 값을 순서대로 되쓴다. */
		for (i = 0; i < (MMU_REG_SIZE / sizeof(u32)); i++) {
			iommu_write_reg(obj, p[i], i * sizeof(u32));	/* [한국어] 떠 두었던 값을 되쓴다. */
			dev_dbg(obj->dev, "%s\t[%02d] %08x\n", __func__, i,	/* [한국어] 되쓴 값을 디버그 로그로 남긴다. */
				p[i]);
		}
		arch_data++;	/* [한국어] 다음 MMU로 넘어간다. */
	}
}
/* [한국어] save_ctx와 짝을 이뤄 내보낸다. */
EXPORT_SYMBOL_GPL(omap_iommu_restore_ctx);

/*
 * [한국어]
 * dra7_cfg_dspsys_mmu - DRA7 DSP의 시스템 설정에서 MMU를 켜거나 끈다
 *
 * @obj: 대상 MMU 인스턴스.
 * @enable: 켤 것인가.
 * @return: 없음.
 *
 * DRA7의 DSP MMU는 자기 레지스터만으로는 동작하지 않고, 별도의
 * 시스템 설정 레지스터에서도 활성화해야 한다. 그 레지스터가
 * 다른 블록과 공유되는 영역에 있어 regmap(syscon)으로 접근한다.
 *
 * syscfg가 없는 인스턴스(일반 OMAP MMU)에서는 아무 일도 하지
 * 않으므로, 호출부가 세대를 가릴 필요가 없다.
 *
 * 실행 컨텍스트: MMU 활성화/비활성화 경로.
 *
 * 호출 체인:
 *   omap2_iommu_enable() / disable() → [dra7_cfg_dspsys_mmu]
 */
static void dra7_cfg_dspsys_mmu(struct omap_iommu *obj, bool enable)
{
	/* [한국어] 쓸 값과, 고칠 비트의 마스크. */
	u32 val, mask;

	/* [한국어] 이 설정이 필요 없는 인스턴스는 그냥 돌아간다. */
	if (!obj->syscfg)
		return;

	/* [한국어] 인스턴스 번호가 곧 비트 위치를 정한다 —
	 * DSP마다 두 개의 MMU가 각자의 비트를 갖는다. */
	mask = (1 << (obj->id * DSP_SYS_MMU_CONFIG_EN_SHIFT));
	val = enable ? mask : 0;
	/* [한국어] 공유 레지스터라 해당 비트만 원자적으로 고친다. */
	regmap_update_bits(obj->syscfg, DSP_SYS_MMU_CONFIG, mask, val);
}

/*
 * [한국어]
 * __iommu_set_twl - 테이블 워크(TWL)를 켜거나 끈다
 *
 * @obj: 대상 MMU.
 * @on: 켤 것인가.
 * @return: 없음.
 *
 * TWL(Table Walking Logic)은 하드웨어가 TLB 미스 시 스스로 페이지
 * 테이블을 읽어 오는 기능이다. 이것을 끄면 TLB 미스마다 인터럽트가
 * 나서 소프트웨어가 항목을 채워 줘야 한다 — 옛 OMAP의 사용 방식이며,
 * 그래서 인터럽트 마스크도 함께 바뀐다.
 *
 * 이 드라이버는 항상 켠 상태로 쓴다. 끄는 경로는 남아 있지만
 * 현재 호출되지 않는다.
 *
 * 실행 컨텍스트: MMU 활성화 경로.
 *
 * 호출 체인:
 *   omap2_iommu_enable() → [__iommu_set_twl]
 */
static void __iommu_set_twl(struct omap_iommu *obj, bool on)
{
	/* [한국어] 현재 제어 레지스터 값. */
	u32 l = iommu_read_reg(obj, MMU_CNTL);

	/* [한국어] TWL을 쓰면 워크 관련 오류만 인터럽트로 받으면 되고,
	 * 끄면 TLB 미스 자체를 받아 소프트웨어가 채워야 한다. */
	if (on)
		iommu_write_reg(obj, MMU_IRQ_TWL_MASK, MMU_IRQENABLE);
	else
		iommu_write_reg(obj, MMU_IRQ_TLB_MISS_MASK, MMU_IRQENABLE);

	/* [한국어] 제어 필드를 지우고 새로 조립한다. */
	l &= ~MMU_CNTL_MASK;
	if (on)	/* [한국어] TWL을 함께 켤지에 따라 제어 값이 갈린다. */
		l |= (MMU_CNTL_MMU_EN | MMU_CNTL_TWL_EN);
	else
		/* [한국어] TWL 없이 MMU만 켠다. */
		l |= (MMU_CNTL_MMU_EN);

	iommu_write_reg(obj, l, MMU_CNTL);	/* [한국어] 조립한 제어 값을 하드웨어에 쓴다. */
}

/*
 * [한국어]
 * omap2_iommu_enable - MMU 하드웨어를 실제로 켠다
 *
 * @obj: 대상 MMU(obj->iopgd가 설정되어 있어야 한다).
 * @return: 0 성공, -EINVAL(테이블 정렬 위반).
 *
 * 런타임 리줌에서 불려, 레지스터를 실제로 설정하는 곳이다.
 *
 * 정렬 검사가 두 번인 것이 눈에 띈다. 1단계 테이블은 16KB
 * 경계에 있어야 하는데, 가상 주소와 물리 주소 양쪽을 확인한다.
 * TTB 레지스터에 하위 비트를 담을 자리가 없기 때문이다.
 *
 * 실행 컨텍스트: 런타임 리줌. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_runtime_resume() → [omap2_iommu_enable]
 */
static int omap2_iommu_enable(struct omap_iommu *obj)
{
	/* [한국어] 버전 레지스터 값과, TTB에 쓸 물리 주소. */
	u32 l, pa;

	/* [한국어] 테이블이 없거나 16KB 정렬이 아니면 켤 수 없다. */
	if (!obj->iopgd || !IS_ALIGNED((unsigned long)obj->iopgd,  SZ_16K))
		return -EINVAL;

	/* [한국어] 물리 주소도 같은 정렬이어야 한다 — TTB 레지스터에
	 * 하위 비트를 담을 자리가 없다. */
	pa = virt_to_phys(obj->iopgd);
	if (!IS_ALIGNED(pa, SZ_16K))	/* [한국어] 물리 주소도 16KB 정렬이어야 TTB에 담긴다. */
		return -EINVAL;

	/* [한국어] 하드웨어 버전을 읽어 한 번 알린다. */
	l = iommu_read_reg(obj, MMU_REVISION);
	dev_info(obj->dev, "%s: version %d.%d\n", obj->name,	/* [한국어] 하드웨어 버전을 한 번 알린다. */
		 (l >> 4) & 0xf, l & 0xf);

	/* [한국어] 1단계 테이블의 물리 주소를 심는다. */
	iommu_write_reg(obj, pa, MMU_TTB);

	/* [한국어] DRA7 DSP라면 시스템 설정에서도 켜야 한다. */
	dra7_cfg_dspsys_mmu(obj, true);

	/* [한국어] 버스 오류를 클라이언트에게 되돌려 주는 기능.
	 * 디바이스 트리가 요청한 경우에만 켠다 — 이것이 없으면
	 * 잘못된 접근이 조용히 무시된다. */
	if (obj->has_bus_err_back)
		iommu_write_reg(obj, MMU_GP_REG_BUS_ERR_BACK_EN, MMU_GP_REG);

	/* [한국어] 마지막으로 MMU와 테이블 워크를 켠다. */
	__iommu_set_twl(obj, true);

	return 0;	/* [한국어] MMU와 테이블 워크가 켜졌다. */
}

/*
 * [한국어]
 * omap2_iommu_disable - MMU 하드웨어를 끈다
 *
 * @obj: 대상 MMU.
 * @return: 없음.
 *
 * 제어 필드를 통째로 0으로 만들면 MMU도 TWL도 꺼진다.
 * TTB는 지우지 않는데, 다시 켤 때 enable이 어차피 새로 쓰기 때문이다.
 *
 * 실행 컨텍스트: 런타임 서스펜드. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_runtime_suspend() → [omap2_iommu_disable]
 */
static void omap2_iommu_disable(struct omap_iommu *obj)
{
	/* [한국어] 현재 제어 레지스터 값. */
	u32 l = iommu_read_reg(obj, MMU_CNTL);

	/* [한국어] 제어 비트를 모두 내려 MMU와 TWL을 끈다. */
	l &= ~MMU_CNTL_MASK;
	iommu_write_reg(obj, l, MMU_CNTL);
	/* [한국어] DRA7 DSP라면 시스템 설정에서도 끈다. */
	dra7_cfg_dspsys_mmu(obj, false);

	dev_dbg(obj->dev, "%s is shutting down\n", obj->name);	/* [한국어] 끄는 중임을 디버그 로그로 남긴다. */
}

/*
 * [한국어]
 * iommu_enable - MMU를 활성화한다(= 전원을 켠다)
 *
 * @obj: 대상 MMU.
 * @return: 0 성공, 음수 오류.
 *
 * 이 드라이버의 특징적인 설계가 여기 있다: **활성화가 곧 전원
 * 켜기다.** 레지스터 설정은 런타임 리줌 콜백이 하므로, 이 함수는
 * 그것을 촉발하기만 하면 된다.
 *
 * 실패 시 put_noidle을 부르는 이유: get_sync는 실패해도 참조를
 * 올려 둔 상태로 돌아올 수 있어, 그것을 되돌려야 한다.
 *
 * 실행 컨텍스트: attach 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_attach() → [iommu_enable]
 */
static int iommu_enable(struct omap_iommu *obj)
{
	/* [한국어] 전원 켜기 결과. */
	int ret;

	/* [한국어] 전원을 켜면 리줌 콜백이 레지스터를 설정한다. */
	ret = pm_runtime_get_sync(obj->dev);
	/* [한국어] 실패해도 참조는 올라가 있을 수 있어 되돌린다. */
	if (ret < 0)
		pm_runtime_put_noidle(obj->dev);

	/* [한국어] 양수(이미 활성)도 성공으로 본다. */
	return ret < 0 ? ret : 0;
}

/*
 * [한국어]
 * iommu_disable - MMU를 비활성화한다(= 전원 참조를 놓는다)
 *
 * @obj: 대상 MMU.
 * @return: 없음.
 *
 * 참조가 0이 되면 서스펜드 콜백이 레지스터를 정리한다.
 *
 * 실행 컨텍스트: detach 경로.
 *
 * 호출 체인:
 *   omap_iommu_detach() → [iommu_disable]
 */
static void iommu_disable(struct omap_iommu *obj)
{
	/* [한국어] 참조가 0이 되면 서스펜드 콜백이 하드웨어를 끈다. */
	pm_runtime_put_sync(obj->dev);
}

/*
 *	TLB operations
 */
/*
 * [한국어]
 * iotlb_cr_to_virt - TLB 항목에서 그것이 덮는 가상 주소를 복원한다
 *
 * @cr: 읽어 온 CAM/RAM 쌍.
 * @return: 이 항목이 덮는 구간의 시작 가상 주소.
 *
 * CAM에는 주소 태그와 페이지 크기가 함께 들어 있다. 크기에 따라
 * 유효한 주소 비트 수가 달라지므로, 크기로 마스크를 구해 적용한다.
 *
 * 실행 컨텍스트: TLB 순회. 순수 계산이다.
 *
 * 호출 체인:
 *   flush_iotlb_page() → [iotlb_cr_to_virt]
 */
static u32 iotlb_cr_to_virt(struct cr_regs *cr)
{
	/* [한국어] 이 항목의 페이지 크기 필드. */
	u32 page_size = cr->cam & MMU_CAM_PGSZ_MASK;
	/* [한국어] 그 크기에 해당하는 주소 마스크 — 큰 페이지일수록
	 * 유효 비트가 적다. */
	u32 mask = get_cam_va_mask(cr->cam & page_size);

	/* [한국어] 태그에서 주소 부분만 남긴다. */
	return cr->cam & mask;
}

/*
 * [한국어]
 * get_iopte_attr - TLB 항목 정보에서 페이지 테이블 속성 비트를 만든다
 *
 * @e: 매핑 정보.
 * @return: 엔트리에 얹을 속성 비트들.
 *
 * 엔디안, 요소 크기, 혼합 모드를 엔트리 형식으로 옮긴다.
 * 마지막 시프트가 핵심인데, **1단계 엔트리와 2단계 엔트리에서
 * 속성 비트의 위치가 다르기 때문**이다. 작은/큰 페이지(2단계)는
 * 그대로 두고, 섹션/슈퍼섹션(1단계)은 6비트 왼쪽으로 민다.
 *
 * 실행 컨텍스트: 매핑 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   iopgtable_store_entry_core() → [get_iopte_attr]
 */
static u32 get_iopte_attr(struct iotlb_entry *e)
{
	/* [한국어] 조립할 속성 비트. */
	u32 attr;

	/* [한국어] 혼합 크기 모드 비트. */
	attr = e->mixed << 5;
	/* [한국어] 엔디안 비트. */
	attr |= e->endian;
	/* [한국어] 요소 크기(8/16/32비트)를 비트 위치로 옮긴다. */
	attr |= e->elsz >> 3;
	/* [한국어] 2단계 엔트리는 제자리, 1단계 엔트리는 6비트 위에
	 * 속성 필드가 있다 — 그 차이를 여기서 흡수한다. */
	attr <<= (((e->pgsz == MMU_CAM_PGSZ_4K) ||
			(e->pgsz == MMU_CAM_PGSZ_64K)) ? 0 : 6);
	return attr;	/* [한국어] 조립한 속성 비트를 돌려준다. */
}

/*
 * [한국어]
 * iommu_report_fault - 폴트 상태와 주소를 읽고 인터럽트를 지운다
 *
 * @obj: 대상 MMU.
 * @da: 폴트 주소를 돌려줄 곳.
 * @return: 폴트 상태 비트들. 0이면 폴트가 없다.
 *
 * 읽은 상태를 되써 지우는 것(write-1-to-clear)까지 여기서 한다.
 * 그래서 호출자는 이 함수를 부른 뒤 다시 지울 필요가 없다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   iommu_fault_handler() → [iommu_report_fault]
 */
static u32 iommu_report_fault(struct omap_iommu *obj, u32 *da)
{
	/* [한국어] 폴트 상태와 주소. */
	u32 status, fault_addr;

	/* [한국어] 우리가 관심 있는 폴트 비트만 남긴다. */
	status = iommu_read_reg(obj, MMU_IRQSTATUS);
	status &= MMU_IRQ_MASK;
	/* [한국어] 우리 폴트가 아니다 — 주소도 의미가 없다. */
	if (!status) {
		*da = 0;
		return 0;	/* [한국어] 우리 폴트가 아니다. */
	}

	/* [한국어] 폴트가 난 주소를 읽어 돌려준다. */
	fault_addr = iommu_read_reg(obj, MMU_FAULT_AD);
	*da = fault_addr;

	/* [한국어] 읽은 비트를 되써 지운다 — 그러지 않으면 같은
	 * 인터럽트가 반복된다. */
	iommu_write_reg(obj, status, MMU_IRQSTATUS);

	return status;	/* [한국어] 선 폴트 비트를 호출자에게 알린다. */
}

/*
 * [한국어]
 * iotlb_lock_get - TLB의 고정 개수와 다음 교체 자리를 읽는다
 *
 * @obj: 대상 MMU.
 * @l: 결과를 담을 구조체.
 * @return: 없음.
 *
 * base는 "앞에서부터 몇 개가 고정(preserve)되어 있는가", vict는
 * "다음에 덮어쓸 자리"다. 이 둘이 이 하드웨어의 TLB 관리 모델
 * 전부다 — 앞쪽은 소프트웨어가 잠가 두고, 뒤쪽은 순환하며 쓴다.
 *
 * 실행 컨텍스트: TLB 조작 경로.
 *
 * 호출 체인:
 *   TLB 적재/순회/저장 함수들 → [iotlb_lock_get]
 */
void iotlb_lock_get(struct omap_iommu *obj, struct iotlb_lock *l)
{
	/* [한국어] 읽은 레지스터 값. */
	u32 val;

	val = iommu_read_reg(obj, MMU_LOCK);

	/* [한국어] 고정된 항목의 개수. */
	l->base = MMU_LOCK_BASE(val);
	/* [한국어] 다음에 덮어쓸 자리. */
	l->vict = MMU_LOCK_VICT(val);
}

/*
 * [한국어]
 * iotlb_lock_set - TLB의 고정 개수와 다음 교체 자리를 쓴다
 *
 * @obj: 대상 MMU.
 * @l: 쓸 값.
 * @return: 없음.
 *
 * vict를 쓰는 것이 곧 "다음에 읽거나 쓸 TLB 자리를 고르는" 일이라,
 * TLB 순회에도 이 함수가 쓰인다.
 *
 * 실행 컨텍스트: TLB 조작 경로.
 *
 * 호출 체인:
 *   TLB 적재/순회/복원 함수들 → [iotlb_lock_set]
 */
void iotlb_lock_set(struct omap_iommu *obj, struct iotlb_lock *l)
{
	/* [한국어] 두 필드를 제자리에 놓아 조립한다. */
	u32 val;

	val = (l->base << MMU_LOCK_BASE_SHIFT);	/* [한국어] 고정 개수를 제자리에 놓는다. */
	val |= (l->vict << MMU_LOCK_VICT_SHIFT);	/* [한국어] 교체 자리도 제자리에 놓는다. */

	iommu_write_reg(obj, val, MMU_LOCK);	/* [한국어] 조립한 값을 lock 레지스터에 쓴다. */
}

/*
 * [한국어]
 * iotlb_read_cr - 현재 선택된 TLB 자리의 내용을 읽는다
 *
 * @obj: 대상 MMU.
 * @cr: 읽은 CAM/RAM을 담을 곳.
 * @return: 없음.
 *
 * 어느 자리를 읽을지는 미리 iotlb_lock_set으로 vict에 지정해 둔다.
 *
 * 실행 컨텍스트: TLB 순회.
 *
 * 호출 체인:
 *   __iotlb_read_cr() → [iotlb_read_cr]
 */
static void iotlb_read_cr(struct omap_iommu *obj, struct cr_regs *cr)
{
	/* [한국어] 주소 태그와 크기가 들어 있는 쪽. */
	cr->cam = iommu_read_reg(obj, MMU_READ_CAM);
	/* [한국어] 물리 주소와 속성이 들어 있는 쪽. */
	cr->ram = iommu_read_reg(obj, MMU_READ_RAM);
}

/*
 * [한국어]
 * iotlb_load_cr - 현재 선택된 TLB 자리에 항목을 써 넣는다
 *
 * @obj: 대상 MMU.
 * @cr: 써 넣을 CAM/RAM.
 * @return: 없음.
 *
 * 네 번의 레지스터 쓰기가 하나의 절차를 이룬다. CAM에 유효 비트를
 * 얹어 쓰고, RAM을 쓰고, **기존 항목을 지운 뒤**(FLUSH_ENTRY)
 * 적재를 실행한다(LD_TLB). 플러시가 적재 앞에 오는 이유는 같은
 * 주소를 덮는 옛 항목이 남아 중복 적중하는 것을 막기 위함이다.
 *
 * 실행 컨텍스트: TLB 적재와 무효화 경로.
 *
 * 호출 체인:
 *   load_iotlb_entry() / flush_iotlb_page() / restore_tlb_entries()
 *   → [iotlb_load_cr]
 */
static void iotlb_load_cr(struct omap_iommu *obj, struct cr_regs *cr)
{
	/* [한국어] 주소 태그에 유효 비트를 얹어 쓴다. */
	iommu_write_reg(obj, cr->cam | MMU_CAM_V, MMU_CAM);
	/* [한국어] 물리 주소와 속성을 쓴다. */
	iommu_write_reg(obj, cr->ram, MMU_RAM);

	/* [한국어] 같은 주소를 덮던 옛 항목을 먼저 지운다 —
	 * 중복 적중을 막기 위한 순서다. */
	iommu_write_reg(obj, 1, MMU_FLUSH_ENTRY);
	/* [한국어] 그다음 새 항목을 적재한다. */
	iommu_write_reg(obj, 1, MMU_LD_TLB);
}

/* only used in iotlb iteration for-loop */
/*
 * [한국어]
 * __iotlb_read_cr - n번째 TLB 항목을 읽는다
 *
 * @obj: 대상 MMU.
 * @n: 읽을 자리.
 * @return: 그 자리의 CAM/RAM.
 *
 * for_each_iotlb_cr 매크로의 본체다. 구조체를 **값으로** 돌려주는
 * 것이 특이한데, 매크로가 루프 변수에 대입하는 형태로 쓰기 때문이다.
 *
 * vict를 바꿔 자리를 고르고 읽는 방식이라 부수 효과가 있다 —
 * 이 함수를 부르면 하드웨어의 교체 포인터가 옮겨 간다. 그래서
 * 순회를 끝낸 뒤에는 lock을 되돌려야 하는 경우가 있다.
 *
 * 실행 컨텍스트: TLB 순회.
 *
 * 호출 체인:
 *   for_each_iotlb_cr 매크로 → [__iotlb_read_cr]
 */
struct cr_regs __iotlb_read_cr(struct omap_iommu *obj, int n)
{
	/* [한국어] 읽어 돌려줄 항목. */
	struct cr_regs cr;
	/* [한국어] 자리를 고르기 위해 조작할 lock 값. */
	struct iotlb_lock l;

	/* [한국어] 현재 값을 읽어 base는 보존하고 vict만 바꾼다. */
	iotlb_lock_get(obj, &l);
	l.vict = n;	/* [한국어] 읽을 자리를 지정한다. */
	iotlb_lock_set(obj, &l);
	/* [한국어] 그 자리의 내용을 읽는다. */
	iotlb_read_cr(obj, &cr);

	return cr;	/* [한국어] 읽은 항목을 값으로 돌려준다. */
}

/* [한국어] TLB 미리 채우기(prefetch)를 쓰는 빌드에서만 아래 코드가
 * 살아 있다. 매핑을 만든 직후 그 항목을 TLB에 미리 넣어 두면
 * 첫 접근의 미스를 줄일 수 있지만, 고정 항목 관리가 복잡해져
 * 기본으로는 꺼져 있다. */
#ifdef PREFETCH_IOTLB
/*
 * [한국어]
 * iotlb_alloc_cr - 매핑 정보에서 TLB 항목(CAM/RAM 쌍)을 만든다
 *
 * @obj: 대상 MMU(로그용).
 * @e: 매핑 정보.
 * @return: 만든 항목, 실패하면 ERR_PTR/NULL.
 *
 * 정렬 검사가 먼저다. 페이지 크기가 요구하는 정렬을 어긴 주소는
 * CAM의 태그 필드에 담을 수 없어, 담아도 엉뚱한 주소를 덮게 된다.
 *
 * 실행 컨텍스트: TLB 적재 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   load_iotlb_entry() → [iotlb_alloc_cr]
 */
static struct cr_regs *iotlb_alloc_cr(struct omap_iommu *obj,
				      struct iotlb_entry *e)
{
	/* [한국어] 만들 항목. */
	struct cr_regs *cr;

	/* [한국어] 정보가 없으면 만들 것도 없다. */
	if (!e)
		return NULL;

	/* [한국어] 이 페이지 크기가 허용하는 태그 비트를 벗어난
	 * 주소다 — CAM에 담으면 엉뚱한 구간을 덮게 된다. */
	if (e->da & ~(get_cam_va_mask(e->pgsz))) {
		dev_err(obj->dev, "%s:\twrong alignment: %08x\n", __func__,	/* [한국어] 이 크기의 태그에 담을 수 없는 주소다. */
			e->da);
		return ERR_PTR(-EINVAL);	/* [한국어] 담아도 엉뚱한 구간을 덮게 되므로 거부한다. */
	}

	cr = kmalloc_obj(*cr);	/* [한국어] 항목 구조체를 잡는다. */
	if (!cr)	/* [한국어] 항목을 잡지 못했다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] CAM에는 주소 태그, 고정 여부, 크기, 유효 비트가 들어간다. */
	cr->cam = (e->da & MMU_CAM_VATAG_MASK) | e->prsvd | e->pgsz | e->valid;
	/* [한국어] RAM에는 물리 주소와 접근 속성이 들어간다. */
	cr->ram = e->pa | e->endian | e->elsz | e->mixed;

	return cr;	/* [한국어] 조립한 CAM/RAM 쌍을 돌려준다. */
}

/**
 * load_iotlb_entry - Set an iommu tlb entry
 * @obj:	target iommu
 * @e:		an iommu tlb entry info
 **/
/*
 * [한국어]
 * load_iotlb_entry - TLB에 항목 하나를 적재한다
 *
 * @obj: 대상 MMU.
 * @e: 적재할 매핑 정보.
 * @return: 0 성공, -EINVAL/-EBUSY/-ENOMEM.
 *
 * 이 하드웨어의 TLB 관리 모델이 온전히 드러나는 함수다.
 * TLB는 두 영역으로 나뉜다:
 *
 *   [0 .. base)      고정 항목. 소프트웨어가 잠가 둔 자리로,
 *                    하드웨어가 절대 덮어쓰지 않는다.
 *   [base .. 끝)     순환 영역. vict가 가리키는 자리를 덮어쓴다.
 *
 * 고정 항목을 요청하면(prsvd) base 자리에 넣고 base를 하나 늘린다 —
 * 고정 영역이 커지는 것이다. 일반 항목이면 빈 자리를 찾아 넣는다.
 *
 * base가 전체 개수에 이르면 TLB가 전부 고정되어 더 넣을 수 없다.
 *
 * 실행 컨텍스트: 매핑 직후. 프로세스 컨텍스트(전원을 켠다).
 *
 * 호출 체인:
 *   prefetch_iotlb_entry() → [load_iotlb_entry] → iotlb_load_cr()
 */
static int load_iotlb_entry(struct omap_iommu *obj, struct iotlb_entry *e)
{
	/* [한국어] 결과 코드. */
	int err = 0;
	/* [한국어] TLB의 고정 개수와 교체 자리. */
	struct iotlb_lock l;
	/* [한국어] 적재할 항목. */
	struct cr_regs *cr;

	/* [한국어] 인자가 비었거나 TLB 크기를 모르면 진행할 수 없다. */
	if (!obj || !obj->nr_tlb_entries || !e)
		return -EINVAL;

	/* [한국어] 레지스터를 만지려면 전원이 필요하다. */
	pm_runtime_get_sync(obj->dev);

	iotlb_lock_get(obj, &l);
	/* [한국어] 고정 영역이 TLB 전체를 차지했다 — 넣을 자리가 없다. */
	if (l.base == obj->nr_tlb_entries) {
		dev_warn(obj->dev, "%s: preserve entries full\n", __func__);	/* [한국어] TLB 전체가 고정되어 넣을 자리가 없다. */
		err = -EBUSY;	/* [한국어] 지금은 넣을 수 없다고 알린다. */
		goto out;	/* [한국어] 전원 참조를 놓고 나간다. */
	}
	/* [한국어] 일반 항목이라면 빈 자리를 찾는다. */
	if (!e->prsvd) {
		/* [한국어] 순회 인덱스와 읽어 볼 항목. */
		int i;
		struct cr_regs tmp;

		/* [한국어] 유효하지 않은(비어 있는) 첫 자리를 찾는다.
		 * 이 순회 자체가 vict를 옮기는 부수 효과를 낸다. */
		for_each_iotlb_cr(obj, obj->nr_tlb_entries, i, tmp)
			if (!iotlb_cr_valid(&tmp))
				break;

		/* [한국어] 빈 자리가 하나도 없다. */
		if (i == obj->nr_tlb_entries) {
			dev_dbg(obj->dev, "%s: full: no entry\n", __func__);	/* [한국어] 유효하지 않은 자리가 하나도 없다. */
			err = -EBUSY;	/* [한국어] TLB가 가득 찼다. */
			goto out;	/* [한국어] 전원 참조를 놓고 나간다. */
		}

		/* [한국어] 순회가 vict를 옮겨 놓았으므로 다시 읽는다 —
		 * 그 값이 곧 찾은 빈 자리다. */
		iotlb_lock_get(obj, &l);
	} else {
		/* [한국어] 고정 항목은 고정 영역의 바로 다음 자리,
		 * 즉 base에 넣는다. */
		l.vict = l.base;
		iotlb_lock_set(obj, &l);	/* [한국어] 고정 영역의 다음 자리를 선택한다. */
	}

	/* [한국어] 매핑 정보를 하드웨어 형식으로 옮긴다. */
	cr = iotlb_alloc_cr(obj, e);
	if (IS_ERR(cr)) {
		/* [한국어] out으로 가지 않고 직접 정리하는 이유: err에
		 * 담을 값이 포인터에 실려 있어 그대로 돌려줘야 한다. */
		pm_runtime_put_sync(obj->dev);
		return PTR_ERR(cr);	/* [한국어] 항목 조립 실패 이유를 그대로 전한다. */
	}

	/* [한국어] 선택된 자리에 적재한다. */
	iotlb_load_cr(obj, cr);
	kfree(cr);

	/* [한국어] 고정 항목을 넣었으면 고정 영역이 하나 커진다. */
	if (e->prsvd)
		l.base++;
	/* increment victim for next tlb load */
	/* [한국어] 교체 자리를 다음으로 옮긴다. 끝에 닿으면 고정
	 * 영역 바로 뒤로 되돌아가 순환한다. */
	if (++l.vict == obj->nr_tlb_entries)
		l.vict = l.base;
	iotlb_lock_set(obj, &l);
/* [한국어] 전원 참조를 놓고 나가는 공통 자리. */
out:
	pm_runtime_put_sync(obj->dev);	/* [한국어] 전원 참조를 놓는다. */
	return err;	/* [한국어] 적재 결과를 전한다. */
}

#else /* !PREFETCH_IOTLB */

/*
 * [한국어]
 * load_iotlb_entry - TLB 미리 채우기를 쓰지 않을 때의 빈 구현
 *
 * @obj: 대상 MMU(쓰지 않는다).
 * @e: 매핑 정보(쓰지 않는다).
 * @return: 항상 0.
 *
 * TLB를 미리 채우지 않아도 하드웨어가 테이블 워크로 알아서
 * 채우므로, 아무 일도 하지 않는 것이 옳은 구현이다.
 *
 * 실행 컨텍스트: 매핑 직후.
 *
 * 호출 체인:
 *   prefetch_iotlb_entry() → [load_iotlb_entry]
 */
static int load_iotlb_entry(struct omap_iommu *obj, struct iotlb_entry *e)
{
	/* [한국어] 하드웨어가 워크로 채우므로 할 일이 없다. */
	return 0;
}

#endif /* !PREFETCH_IOTLB */

/*
 * [한국어]
 * prefetch_iotlb_entry - 새 매핑을 TLB에 미리 넣어 둔다
 *
 * @obj: 대상 MMU.
 * @e: 매핑 정보.
 * @return: 0 또는 오류.
 *
 * 이름을 통해 의도를 드러내는 얇은 껍데기다. 빌드 옵션에 따라
 * 실제 적재가 되거나 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 매핑 직후.
 *
 * 호출 체인:
 *   omap_iopgtable_store_entry() → [prefetch_iotlb_entry]
 */
static int prefetch_iotlb_entry(struct omap_iommu *obj, struct iotlb_entry *e)
{
	/* [한국어] 빌드 옵션에 따라 실제 적재이거나 빈 함수다. */
	return load_iotlb_entry(obj, e);
}

/**
 * flush_iotlb_page - Clear an iommu tlb entry
 * @obj:	target iommu
 * @da:		iommu device virtual address
 *
 * Clear an iommu tlb entry which includes 'da' address.
 **/
/*
 * [한국어]
 * flush_iotlb_page - 주어진 주소를 덮는 TLB 항목을 지운다
 *
 * @obj: 대상 MMU.
 * @da: 무효화할 가상 주소.
 * @return: 없음.
 *
 * 주소로 TLB 항목을 지우는 명령이 없어, **소프트웨어가 직접
 * 찾아낸다.** 모든 항목을 훑으며 각 항목이 덮는 구간을 계산해
 * da를 포함하는 것을 찾고, 그 자리를 다시 선택해 플러시한다.
 *
 * iotlb_load_cr을 다시 부르는 것이 묘한데, 그 안의 FLUSH_ENTRY가
 * 목적이다. 순회가 이미 vict를 그 자리로 옮겨 놓았으므로,
 * 같은 내용을 다시 적재하며 플러시를 촉발하는 것이다.
 *
 * TLB 크기가 8이나 32뿐이라 이 선형 탐색이 감당할 만하다.
 *
 * 실행 컨텍스트: 매핑/해제 경로. 프로세스 컨텍스트(전원을 켠다).
 *
 * 호출 체인:
 *   omap_iopgtable_store_entry() / iopgtable_clear_entry()
 *   → [flush_iotlb_page]
 */
static void flush_iotlb_page(struct omap_iommu *obj, u32 da)
{
	/* [한국어] 순회 인덱스. */
	int i;
	/* [한국어] 읽어 볼 항목. */
	struct cr_regs cr;

	/* [한국어] 레지스터를 만지려면 전원이 필요하다. */
	pm_runtime_get_sync(obj->dev);

	/* [한국어] 모든 TLB 자리를 훑는다 — 주소로 찾는 명령이 없다. */
	for_each_iotlb_cr(obj, obj->nr_tlb_entries, i, cr) {
		/* [한국어] 이 항목이 덮는 구간의 시작과 크기. */
		u32 start;
		size_t bytes;

		/* [한국어] 비어 있는 자리는 건너뛴다. */
		if (!iotlb_cr_valid(&cr))
			continue;

		/* [한국어] 태그에서 시작 주소를 복원한다. */
		start = iotlb_cr_to_virt(&cr);
		/* [한국어] 크기 필드에서 덮는 바이트 수를 구한다. */
		bytes = iopgsz_to_bytes(cr.cam & 3);

		/* [한국어] 이 항목이 da를 덮는가. */
		if ((start <= da) && (da < start + bytes)) {
			dev_dbg(obj->dev, "%s: %08x<=%08x(%zx)\n",	/* [한국어] 어느 항목이 이 주소를 덮고 있었는지 남긴다. */
				__func__, start, da, bytes);
			/* [한국어] 순회가 vict를 이 자리로 옮겨 놓았으므로,
			 * 다시 적재하며 그 안의 FLUSH_ENTRY로 지운다. */
			iotlb_load_cr(obj, &cr);
			iommu_write_reg(obj, 1, MMU_FLUSH_ENTRY);	/* [한국어] 그 자리를 무효화한다. */
			break;
		}
	}
	pm_runtime_put_sync(obj->dev);

	/* [한국어] 끝까지 못 찾았다 — 이미 캐시되지 않은 주소였다는
	 * 뜻이라 오류는 아니다. */
	if (i == obj->nr_tlb_entries)
		dev_dbg(obj->dev, "%s: no page for %08x\n", __func__, da);
}

/**
 * flush_iotlb_all - Clear all iommu tlb entries
 * @obj:	target iommu
 **/
/*
 * [한국어]
 * flush_iotlb_all - TLB 전체를 비운다
 *
 * @obj: 대상 MMU.
 * @return: 없음.
 *
 * 전역 플러시 명령 하나면 되지만, **그 전에 lock을 0으로
 * 되돌리는 것**이 중요하다. 전체를 비우면 고정 항목도 함께
 * 사라지므로, "고정된 것이 없고 처음부터 채운다"는 상태로
 * 장부를 맞춰야 한다.
 *
 * 실행 컨텍스트: attach와 테이블 전체 정리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_attach() / iopgtable_clear_entry_all()
 *   → [flush_iotlb_all]
 */
static void flush_iotlb_all(struct omap_iommu *obj)
{
	/* [한국어] 되돌릴 lock 값. */
	struct iotlb_lock l;

	/* [한국어] 레지스터를 만지려면 전원이 필요하다. */
	pm_runtime_get_sync(obj->dev);

	/* [한국어] 고정 항목이 없고 처음부터 채운다는 상태로 되돌린다 —
	 * 아래의 전역 플러시가 고정 항목까지 지우기 때문이다. */
	l.base = 0;
	l.vict = 0;	/* [한국어] 교체 자리도 처음으로 되돌린다. */
	iotlb_lock_set(obj, &l);

	/* [한국어] 전역 플러시 명령. */
	iommu_write_reg(obj, 1, MMU_GFLUSH);

	pm_runtime_put_sync(obj->dev);	/* [한국어] 전원 참조를 놓는다. */
}

/*
 *	H/W pagetable operations
 */
/*
 * [한국어]
 * flush_iopte_range - 고친 테이블 엔트리를 하드웨어가 볼 수 있게 밀어낸다
 *
 * @dev: DMA 매핑의 기준 디바이스.
 * @dma: 테이블의 DMA 주소.
 * @offset: 그 안에서 고친 위치의 바이트 오프셋.
 * @num_entries: 고친 엔트리 개수.
 * @return: 없음.
 *
 * 이 하드웨어는 테이블을 캐시 일관성 없이 DMA로 읽는다. 그래서
 * 엔트리를 고칠 때마다 반드시 이 함수를 불러야 한다. 빠뜨리면
 * MMU가 옛 값을 읽어 원인을 찾기 어려운 폴트가 난다.
 *
 * 범위 지정 변형을 쓰는 덕분에, 큰 페이지처럼 16개를 한꺼번에
 * 고친 경우도 한 번의 호출로 끝난다.
 *
 * 실행 컨텍스트: 테이블 갱신 경로.
 *
 * 호출 체인:
 *   iopgd_alloc_* / iopte_alloc_* / clear_entry_core → [flush_iopte_range]
 */
static void flush_iopte_range(struct device *dev, dma_addr_t dma,
			      unsigned long offset, int num_entries)
{
	/* [한국어] 밀어낼 바이트 수. 엔트리는 모두 u32다. */
	size_t size = num_entries * sizeof(u32);

	/* [한국어] CPU 캐시를 메모리로 밀어내 MMU가 새 값을 보게 한다. */
	dma_sync_single_range_for_device(dev, dma, offset, size, DMA_TO_DEVICE);
}

/*
 * [한국어]
 * iopte_free - 2단계 테이블을 반납한다
 *
 * @obj: 대상 MMU.
 * @iopte: 반납할 테이블.
 * @dma_valid: DMA 매핑이 걸려 있는가.
 * @return: 없음.
 *
 * dma_valid 인자가 필요한 이유: 이 함수는 매핑을 걸기 전의
 * 실패 경로에서도 불린다. 그때 매핑을 풀려 하면 안 된다.
 *
 * 원본 주석이 밝히는 규약이 중요하다 — **반납되는 테이블은
 * 깨끗해야 한다.** 슬랩 캐시가 재사용할 때 0으로 다시 채우지
 * 않으므로, 지우는 쪽이 미리 memset 해 두어야 한다.
 *
 * 실행 컨텍스트: 해제 경로와 할당 실패 경로.
 *
 * 호출 체인:
 *   iopte_alloc()의 실패 경로 / clear_entry_core() → [iopte_free]
 */
static void iopte_free(struct omap_iommu *obj, u32 *iopte, bool dma_valid)
{
	/* [한국어] 매핑을 풀 때 쓸 DMA 주소. */
	dma_addr_t pt_dma;

	/* Note: freed iopte's must be clean ready for re-use */
	if (iopte) {
		/* [한국어] 매핑이 걸린 테이블만 푼다. 매핑 전에 실패해
		 * 되돌리는 경우에는 이 단계를 건너뛴다. */
		if (dma_valid) {
			pt_dma = virt_to_phys(iopte);	/* [한국어] 매핑을 풀 때 쓸 주소를 구한다. */
			dma_unmap_single(obj->dev, pt_dma, IOPTE_TABLE_SIZE,	/* [한국어] 하드웨어가 더 이상 이 테이블을 읽지 않게 한다. */
					 DMA_TO_DEVICE);
		}

		/* [한국어] 슬랩 캐시에 반납한다. 내용이 0이어야 한다는
		 * 규약은 지우는 쪽이 지킨다. */
		kmem_cache_free(iopte_cachep, iopte);
	}
}

/*
 * [한국어]
 * iopte_alloc - 2단계 테이블을 확보하고 해당 엔트리의 주소를 얻는다
 *
 * @obj: 대상 MMU.
 * @iopgd: 1단계 엔트리의 주소.
 * @pt_dma: 그 테이블의 DMA 주소를 돌려줄 곳.
 * @da: 대상 가상 주소.
 * @return: 2단계 엔트리의 주소, 실패하면 ERR_PTR.
 *
 * **락을 잠시 놓고 할당한다**는 것이 이 함수의 핵심이다.
 * page_table_lock을 쥔 채로는 GFP_KERNEL 할당을 할 수 없는데,
 * 그렇다고 ATOMIC으로 잡으면 실패 확률이 높아진다. 그래서
 * 락을 놓고 잡았다가 다시 잡는다.
 *
 * 그 대가로 **경쟁이 생긴다.** 락을 놓은 사이 다른 CPU가 같은
 * 자리에 테이블을 달았을 수 있어, 다시 잡은 뒤 엔트리를 재확인해야
 * 한다. 이미 있으면 내가 잡은 것을 버린다.
 *
 * DMA 주소와 물리 주소가 같아야 한다는 전제를 WARN_ON으로 확인하는
 * 점도 중요하다 — 1단계 엔트리에 물리 주소를 심는데, 하드웨어는
 * 그것을 DMA 주소로 읽기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. page_table_lock을 잡은 채로 들어와
 * 잡은 채로 나간다(중간에 잠시 놓는다).
 *
 * 호출 체인:
 *   iopte_alloc_page() / iopte_alloc_large() → [iopte_alloc]
 */
static u32 *iopte_alloc(struct omap_iommu *obj, u32 *iopgd,
			dma_addr_t *pt_dma, u32 da)
{
	/* [한국어] 확보한 2단계 테이블. */
	u32 *iopte;
	/* [한국어] 1단계 엔트리의 위치(플러시 대상). */
	unsigned long offset = iopgd_index(da) * sizeof(da);

	/* a table has already existed */
	/* [한국어] 이미 테이블이 달려 있으면 할당을 건너뛴다. */
	if (*iopgd)
		goto pte_ready;

	/*
	 * do the allocation outside the page table lock
	 */
	/* [한국어] GFP_KERNEL 할당은 잠들 수 있어 락을 쥔 채로 할 수
	 * 없다. 잠시 놓고 잡았다가 다시 잡는다. */
	spin_unlock(&obj->page_table_lock);
	iopte = kmem_cache_zalloc(iopte_cachep, GFP_KERNEL);	/* [한국어] 0으로 초기화된 2단계 테이블을 잡는다. */
	spin_lock(&obj->page_table_lock);

	/* [한국어] 락을 놓은 사이 다른 CPU가 달지 않았는지 다시 본다. */
	if (!*iopgd) {
		/* [한국어] 아무도 달지 않았는데 내 할당도 실패했다. */
		if (!iopte)
			return ERR_PTR(-ENOMEM);

		/* [한국어] 하드웨어가 이 테이블을 읽을 수 있게 매핑한다. */
		*pt_dma = dma_map_single(obj->dev, iopte, IOPTE_TABLE_SIZE,
					 DMA_TO_DEVICE);
		if (dma_mapping_error(obj->dev, *pt_dma)) {	/* [한국어] 하드웨어가 이 테이블을 읽을 수 없다. */
			dev_err(obj->dev, "DMA map error for L2 table\n");
			/* [한국어] 매핑 전 실패라 dma_valid는 거짓이다. */
			iopte_free(obj, iopte, false);
			return ERR_PTR(-ENOMEM);	/* [한국어] 테이블을 확보하지 못했음을 알린다. */
		}

		/*
		 * we rely on dma address and the physical address to be
		 * the same for mapping the L2 table
		 */
		/* [한국어] 1단계 엔트리에는 물리 주소를 심는데 하드웨어는
		 * 그것을 DMA 주소로 읽는다 — 둘이 달라지면 엉뚱한 테이블을
		 * 가리키게 되므로 여기서 전제를 확인한다. */
		if (WARN_ON(*pt_dma != virt_to_phys(iopte))) {
			dev_err(obj->dev, "DMA translation error for L2 table\n");	/* [한국어] 주소 전제가 깨졌음을 알린다. */
			dma_unmap_single(obj->dev, *pt_dma, IOPTE_TABLE_SIZE,	/* [한국어] 걸어 둔 매핑을 되돌린다. */
					 DMA_TO_DEVICE);
			iopte_free(obj, iopte, false);	/* [한국어] 테이블을 반납한다. */
			return ERR_PTR(-ENOMEM);	/* [한국어] 이 전제가 깨지면 진행할 수 없다. */
		}

		/* [한국어] 1단계 엔트리에 테이블 주소와 타입을 심는다. */
		*iopgd = virt_to_phys(iopte) | IOPGD_TABLE;

		/* [한국어] 고친 1단계 엔트리 하나를 하드웨어에 밀어낸다. */
		flush_iopte_range(obj->dev, obj->pd_dma, offset, 1);
		dev_vdbg(obj->dev, "%s: a new pte:%p\n", __func__, iopte);	/* [한국어] 새 2단계 테이블을 만들었음을 남긴다. */
	} else {
		/* We raced, free the reduniovant table */
		/* [한국어] 락을 놓은 사이 다른 CPU가 먼저 달았다 —
		 * 내가 잡은 것은 쓸모가 없다. 아직 매핑 전이다. */
		iopte_free(obj, iopte, false);
	}

/* [한국어] 기존 테이블을 찾았든 새로 만들었든 여기로 모인다. */
pte_ready:
	/* [한국어] 그 테이블 안에서 이 주소의 엔트리 위치를 구한다. */
	iopte = iopte_offset(iopgd, da);
	/* [한국어] 호출자가 플러시할 때 쓸 테이블의 DMA 주소.
	 * 위의 전제 덕분에 물리 주소를 그대로 쓸 수 있다. */
	*pt_dma = iopgd_page_paddr(iopgd);
	dev_vdbg(obj->dev,
		 "%s: da:%08x pgd:%p *pgd:%08x pte:%p *pte:%08x\n",
		 __func__, da, iopgd, *iopgd, iopte, *iopte);

	return iopte;	/* [한국어] 이 주소에 해당하는 2단계 엔트리의 위치를 돌려준다. */
}

/*
 * [한국어]
 * iopgd_alloc_section - 1MB 섹션 매핑을 만든다
 *
 * @obj: 대상 MMU.
 * @da: 가상 주소.
 * @pa: 물리 주소.
 * @prot: 속성 비트.
 * @return: 0 성공, -EINVAL(정렬 위반).
 *
 * 1단계 엔트리에 직접 쓰므로 2단계 테이블이 필요 없다.
 * 가상 주소와 물리 주소를 OR로 묶어 한 번에 정렬을 검사하는
 * 관용구가 이 파일의 네 할당 함수에 반복된다.
 *
 * 실행 컨텍스트: 매핑 경로. page_table_lock을 잡은 상태.
 *
 * 호출 체인:
 *   iopgtable_store_entry_core() → [iopgd_alloc_section]
 */
static int iopgd_alloc_section(struct omap_iommu *obj, u32 da, u32 pa, u32 prot)
{
	/* [한국어] 고칠 1단계 엔트리. */
	u32 *iopgd = iopgd_offset(obj, da);
	/* [한국어] 플러시할 위치. */
	unsigned long offset = iopgd_index(da) * sizeof(da);

	/* [한국어] 두 주소를 OR로 묶어 한 번에 검사한다 — 어느 쪽이든
	 * 1MB 정렬을 어기면 엔트리에 담을 수 없다. */
	if ((da | pa) & ~IOSECTION_MASK) {
		dev_err(obj->dev, "%s: %08x:%08x should aligned on %08lx\n",	/* [한국어] 1MB 정렬을 어긴 주소다. */
			__func__, da, pa, IOSECTION_SIZE);
		return -EINVAL;	/* [한국어] 섹션 엔트리에 담을 수 없다. */
	}

	/* [한국어] 주소, 속성, 섹션 타입을 합쳐 쓴다. */
	*iopgd = (pa & IOSECTION_MASK) | prot | IOPGD_SECTION;
	/* [한국어] 고친 엔트리를 하드웨어에 밀어낸다. */
	flush_iopte_range(obj->dev, obj->pd_dma, offset, 1);
	return 0;	/* [한국어] 섹션 매핑이 완료됐다. */
}

/*
 * [한국어]
 * iopgd_alloc_super - 16MB 슈퍼섹션 매핑을 만든다
 *
 * @obj: 대상 MMU.
 * @da: 가상 주소.
 * @pa: 물리 주소.
 * @prot: 속성 비트.
 * @return: 0 성공, -EINVAL(정렬 위반).
 *
 * 슈퍼섹션은 **같은 엔트리를 16번 반복**해 표현한다. 하드웨어가
 * 1단계 인덱스로 어느 자리를 읽든 같은 답이 나와야 하기 때문이다.
 * 큰 페이지(64KB)도 2단계에서 같은 방식을 쓴다.
 *
 * 실행 컨텍스트: 매핑 경로. page_table_lock을 잡은 상태.
 *
 * 호출 체인:
 *   iopgtable_store_entry_core() → [iopgd_alloc_super]
 */
static int iopgd_alloc_super(struct omap_iommu *obj, u32 da, u32 pa, u32 prot)
{
	/* [한국어] 첫 1단계 엔트리. */
	u32 *iopgd = iopgd_offset(obj, da);
	/* [한국어] 플러시할 시작 위치. */
	unsigned long offset = iopgd_index(da) * sizeof(da);
	/* [한국어] 반복 인덱스. */
	int i;

	/* [한국어] 16MB 정렬을 두 주소 모두에 대해 검사한다. */
	if ((da | pa) & ~IOSUPER_MASK) {
		dev_err(obj->dev, "%s: %08x:%08x should aligned on %08lx\n",	/* [한국어] 16MB 정렬을 어긴 주소다. */
			__func__, da, pa, IOSUPER_SIZE);
		return -EINVAL;	/* [한국어] 슈퍼섹션 엔트리에 담을 수 없다. */
	}

	/* [한국어] 같은 값을 16개 엔트리에 반복해 채운다 — 하드웨어가
	 * 어느 자리를 읽든 같은 답이 나오게 하기 위함이다. */
	for (i = 0; i < 16; i++)
		*(iopgd + i) = (pa & IOSUPER_MASK) | prot | IOPGD_SUPER;
	/* [한국어] 16개를 한 번에 밀어낸다. */
	flush_iopte_range(obj->dev, obj->pd_dma, offset, 16);
	return 0;	/* [한국어] 슈퍼섹션 매핑이 완료됐다. */
}

/*
 * [한국어]
 * iopte_alloc_page - 4KB 작은 페이지 매핑을 만든다
 *
 * @obj: 대상 MMU.
 * @da: 가상 주소.
 * @pa: 물리 주소.
 * @prot: 속성 비트.
 * @return: 0 성공, 음수 오류.
 *
 * 정렬 검사가 없는 유일한 할당 함수다. 4KB가 최소 단위라
 * 상위 계층이 이미 그 정렬을 보장하기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. page_table_lock을 잡은 상태
 * (iopte_alloc이 잠시 놓았다 다시 잡는다).
 *
 * 호출 체인:
 *   iopgtable_store_entry_core() → [iopte_alloc_page] → iopte_alloc()
 */
static int iopte_alloc_page(struct omap_iommu *obj, u32 da, u32 pa, u32 prot)
{
	/* [한국어] 1단계 엔트리. */
	u32 *iopgd = iopgd_offset(obj, da);
	/* [한국어] 2단계 테이블의 DMA 주소(플러시용). */
	dma_addr_t pt_dma;
	/* [한국어] 2단계 테이블을 확보하고 엔트리 위치를 얻는다. */
	u32 *iopte = iopte_alloc(obj, iopgd, &pt_dma, da);
	/* [한국어] 그 테이블 안에서 고칠 위치. */
	unsigned long offset = iopte_index(da) * sizeof(da);

	/* [한국어] 테이블을 확보하지 못했다. */
	if (IS_ERR(iopte))
		return PTR_ERR(iopte);

	/* [한국어] 주소, 속성, 작은 페이지 타입을 합쳐 쓴다. */
	*iopte = (pa & IOPAGE_MASK) | prot | IOPTE_SMALL;
	/* [한국어] 2단계 테이블의 고친 엔트리를 밀어낸다. */
	flush_iopte_range(obj->dev, pt_dma, offset, 1);

	dev_vdbg(obj->dev, "%s: da:%08x pa:%08x pte:%p *pte:%08x\n",	/* [한국어] 심은 엔트리를 디버그 로그로 남긴다. */
		 __func__, da, pa, iopte, *iopte);

	return 0;	/* [한국어] 작은 페이지 매핑이 완료됐다. */
}

/*
 * [한국어]
 * iopte_alloc_large - 64KB 큰 페이지 매핑을 만든다
 *
 * @obj: 대상 MMU.
 * @da: 가상 주소.
 * @pa: 물리 주소.
 * @prot: 속성 비트.
 * @return: 0 성공, 음수 오류.
 *
 * 슈퍼섹션과 같은 발상으로 2단계 엔트리 16개를 같은 값으로 채운다.
 *
 * 검사 순서에 눈여겨볼 점이 있다. **정렬 검사가 테이블 확보
 * 실패 검사보다 앞에 온다.** 그래서 정렬이 틀리면 이미 확보한
 * 테이블을 그대로 두고 -EINVAL로 돌아가는데, 빈 테이블이 남을 뿐
 * 동작상 문제는 없다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   iopgtable_store_entry_core() → [iopte_alloc_large] → iopte_alloc()
 */
static int iopte_alloc_large(struct omap_iommu *obj, u32 da, u32 pa, u32 prot)
{
	/* [한국어] 1단계 엔트리. */
	u32 *iopgd = iopgd_offset(obj, da);
	/* [한국어] 2단계 테이블의 DMA 주소. */
	dma_addr_t pt_dma;
	/* [한국어] 2단계 테이블을 확보하고 첫 엔트리 위치를 얻는다. */
	u32 *iopte = iopte_alloc(obj, iopgd, &pt_dma, da);
	/* [한국어] 그 테이블 안에서 고칠 시작 위치. */
	unsigned long offset = iopte_index(da) * sizeof(da);
	/* [한국어] 반복 인덱스. */
	int i;

	/* [한국어] 64KB 정렬을 두 주소 모두에 대해 검사한다. */
	if ((da | pa) & ~IOLARGE_MASK) {
		dev_err(obj->dev, "%s: %08x:%08x should aligned on %08lx\n",	/* [한국어] 64KB 정렬을 어긴 주소다. */
			__func__, da, pa, IOLARGE_SIZE);
		return -EINVAL;	/* [한국어] 큰 페이지 엔트리에 담을 수 없다. */
	}

	/* [한국어] 테이블 확보 실패 검사가 정렬 검사보다 뒤에 있다 —
	 * 정렬이 틀리면 확보한 테이블이 빈 채로 남지만 무해하다. */
	if (IS_ERR(iopte))
		return PTR_ERR(iopte);

	/* [한국어] 같은 값을 16개 엔트리에 반복해 채운다. */
	for (i = 0; i < 16; i++)
		*(iopte + i) = (pa & IOLARGE_MASK) | prot | IOPTE_LARGE;
	/* [한국어] 16개를 한 번에 밀어낸다. */
	flush_iopte_range(obj->dev, pt_dma, offset, 16);
	return 0;	/* [한국어] 큰 페이지 매핑이 완료됐다. */
}

/*
 * [한국어]
 * iopgtable_store_entry_core - 크기에 맞는 할당 함수를 골라 엔트리를 심는다
 *
 * @obj: 대상 MMU.
 * @e: 매핑 정보.
 * @return: 0 성공, -EINVAL, 할당 함수의 오류.
 *
 * 네 가지 크기를 함수 포인터로 갈라 처리한다. 크기별 코드가
 * 서로 꽤 다르기 때문에(1단계냐 2단계냐, 반복이냐 아니냐)
 * 표로 묶기보다 각각의 함수로 두는 편이 읽기 쉽다.
 *
 * 락을 함수 호출 주위에만 감싸는 점에 유의 — 함수 선택과 속성
 * 계산은 락 없이 해도 된다.
 *
 * 실행 컨텍스트: 매핑 경로. 여기서 page_table_lock을 잡는다.
 *
 * 호출 체인:
 *   omap_iopgtable_store_entry() → [iopgtable_store_entry_core]
 */
static int
iopgtable_store_entry_core(struct omap_iommu *obj, struct iotlb_entry *e)
{
	/* [한국어] 크기에 따라 고를 할당 함수. */
	int (*fn)(struct omap_iommu *, u32, u32, u32);
	/* [한국어] 엔트리에 얹을 속성 비트. */
	u32 prot;
	/* [한국어] 할당 결과. */
	int err;

	if (!obj || !e)	/* [한국어] 인자가 비었으면 진행할 수 없다. */
		return -EINVAL;

	/* [한국어] 크기가 어느 단계의 어떤 형식인지를 결정한다. */
	switch (e->pgsz) {
	/* [한국어] 16MB — 1단계 엔트리 16개. */
	case MMU_CAM_PGSZ_16M:
		fn = iopgd_alloc_super;	/* [한국어] 1단계 엔트리 16개로 표현한다. */
		break;
	/* [한국어] 1MB — 1단계 엔트리 하나. */
	case MMU_CAM_PGSZ_1M:
		fn = iopgd_alloc_section;	/* [한국어] 1단계 엔트리 하나로 표현한다. */
		break;
	/* [한국어] 64KB — 2단계 엔트리 16개. */
	case MMU_CAM_PGSZ_64K:
		fn = iopte_alloc_large;	/* [한국어] 2단계 엔트리 16개로 표현한다. */
		break;
	/* [한국어] 4KB — 2단계 엔트리 하나. */
	case MMU_CAM_PGSZ_4K:
		fn = iopte_alloc_page;	/* [한국어] 2단계 엔트리 하나로 표현한다. */
		break;
	/* [한국어] 지원하지 않는 크기. */
	default:
		fn = NULL;	/* [한국어] 고를 함수가 없다 — 아래에서 걸러진다. */
		break;
	}

	/* [한국어] 코어가 pgsize_bitmap을 지키므로 여기 오면 버그다. */
	if (WARN_ON(!fn))
		return -EINVAL;

	/* [한국어] 단계에 맞는 위치로 속성 비트를 옮긴다. */
	prot = get_iopte_attr(e);

	/* [한국어] 테이블 갱신 구간만 잠근다. */
	spin_lock(&obj->page_table_lock);
	err = fn(obj, e->da, e->pa, prot);	/* [한국어] 크기에 맞는 할당 함수를 부른다. */
	spin_unlock(&obj->page_table_lock);	/* [한국어] 테이블 갱신이 끝났으니 락을 놓는다. */

	return err;	/* [한국어] 심기 결과를 전한다. */
}

/**
 * omap_iopgtable_store_entry - Make an iommu pte entry
 * @obj:	target iommu
 * @e:		an iommu tlb entry info
 **/
/*
 * [한국어]
 * omap_iopgtable_store_entry - 엔트리를 심고 TLB를 정리한다
 *
 * @obj: 대상 MMU.
 * @e: 매핑 정보.
 * @return: 0 성공, 음수 오류.
 *
 * 앞뒤로 TLB를 다루는 것이 이 껍데기의 존재 이유다.
 * **먼저** 그 주소의 옛 TLB 항목을 지우고(없으면 무해하다),
 * 엔트리를 심은 **뒤** 새 항목을 미리 채운다.
 *
 * 앞의 플러시가 중요하다 — 옛 매핑의 TLB 항목이 남아 있으면
 * 새 엔트리를 써도 하드웨어가 옛 주소로 접근한다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   omap_iommu_map() → [omap_iopgtable_store_entry]
 *   → flush_iotlb_page() → iopgtable_store_entry_core()
 */
static int
omap_iopgtable_store_entry(struct omap_iommu *obj, struct iotlb_entry *e)
{
	/* [한국어] 심기 결과. */
	int err;

	/* [한국어] 옛 매핑의 TLB 항목을 먼저 지운다 — 남아 있으면
	 * 새 엔트리가 무시된다. */
	flush_iotlb_page(obj, e->da);
	err = iopgtable_store_entry_core(obj, e);
	/* [한국어] 성공했으면 새 항목을 TLB에 미리 넣어 둔다
	 * (빌드 옵션에 따라 아무 일도 하지 않을 수 있다). */
	if (!err)
		prefetch_iotlb_entry(obj, e);
	return err;	/* [한국어] 심기와 TLB 정리의 결과를 전한다. */
}

/**
 * iopgtable_lookup_entry - Lookup an iommu pte entry
 * @obj:	target iommu
 * @da:		iommu device virtual address
 * @ppgd:	iommu pgd entry pointer to be returned
 * @ppte:	iommu pte entry pointer to be returned
 **/
/*
 * [한국어]
 * iopgtable_lookup_entry - 주소에 해당하는 1단계와 2단계 엔트리를 찾는다
 *
 * @obj: 대상 MMU.
 * @da: 찾을 가상 주소.
 * @ppgd: 1단계 엔트리의 주소를 돌려줄 곳.
 * @ppte: 2단계 엔트리의 주소를 돌려줄 곳(없으면 NULL).
 * @return: 없음.
 *
 * 두 포인터를 함께 돌려주는 형태가 요점이다. 호출자는 pte가
 * NULL인지로 "이 매핑이 1단계에서 끝나는가(섹션/슈퍼섹션)"를
 * 판단한다.
 *
 * 1단계 엔트리 주소는 매핑이 없어도 항상 돌려준다 — 호출자가
 * 그 값을 보고 진단할 수 있게 하기 위함이다.
 *
 * 실행 컨텍스트: 조회 경로. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   omap_iommu_iova_to_phys() → [iopgtable_lookup_entry]
 */
static void
iopgtable_lookup_entry(struct omap_iommu *obj, u32 da, u32 **ppgd, u32 **ppte)
{
	/* [한국어] 찾은 엔트리들. pte는 없을 수 있다. */
	u32 *iopgd, *iopte = NULL;

	/* [한국어] 1단계 엔트리를 찾는다. */
	iopgd = iopgd_offset(obj, da);
	/* [한국어] 비어 있으면 더 내려갈 것이 없다. */
	if (!*iopgd)
		goto out;

	/* [한국어] 2단계 테이블을 가리키는 경우에만 내려간다.
	 * 섹션이나 슈퍼섹션이면 pte가 NULL로 남는다. */
	if (iopgd_is_table(*iopgd))
		iopte = iopte_offset(iopgd, da);
/* [한국어] 어느 경우든 두 포인터를 채워 돌려준다. */
out:
	*ppgd = iopgd;
	*ppte = iopte;
}

/*
 * [한국어]
 * iopgtable_clear_entry_core - 엔트리를 지우고 필요하면 테이블까지 반납한다
 *
 * @obj: 대상 MMU.
 * @da: 지울 가상 주소.
 * @return: 실제로 해제한 바이트 수, 매핑이 없으면 0.
 *
 * 실제 매핑 크기를 **엔트리를 읽어 알아낸다.** 그래서 반환값이
 * 요청과 다를 수 있고, 호출자가 그것을 그대로 코어에 전한다.
 *
 * 반복 매핑(16개짜리)을 지울 때 **첫 엔트리로 되감는** 처리가
 * 핵심이다. 요청 주소가 그 구간의 중간을 가리켜도 16개 전부를
 * 지워야 하기 때문이다.
 *
 * 마지막으로, 2단계 테이블이 완전히 비었으면 그 테이블 자체를
 * 반납하고 1단계 엔트리도 지운다. 테이블 전체를 훑어야 알 수
 * 있어 비싸지만, 그러지 않으면 빈 테이블이 계속 쌓인다.
 * memset이 반납 전에 이뤄져 "깨끗한 상태로 반납" 규약을 지킨다.
 *
 * 실행 컨텍스트: 해제 경로. page_table_lock을 잡은 상태.
 *
 * 호출 체인:
 *   iopgtable_clear_entry() → [iopgtable_clear_entry_core]
 */
static size_t iopgtable_clear_entry_core(struct omap_iommu *obj, u32 da)
{
	/* [한국어] 실제로 해제한 바이트 수. */
	size_t bytes;
	/* [한국어] 1단계 엔트리. */
	u32 *iopgd = iopgd_offset(obj, da);
	/* [한국어] 지울 엔트리의 개수. 반복 매핑이면 16이 된다. */
	int nent = 1;
	/* [한국어] 2단계 테이블의 DMA 주소. */
	dma_addr_t pt_dma;
	/* [한국어] 1단계와 2단계에서 각각 고칠 위치. */
	unsigned long pd_offset = iopgd_index(da) * sizeof(da);
	unsigned long pt_offset = iopte_index(da) * sizeof(da);

	/* [한국어] 매핑이 없다 — 해제한 것도 없다. */
	if (!*iopgd)
		return 0;

	/* [한국어] 2단계 테이블이 달린 경우(4KB 또는 64KB 매핑). */
	if (iopgd_is_table(*iopgd)) {
		/* [한국어] 빈 테이블 판정용 순회 인덱스. */
		int i;
		/* [한국어] 지울 2단계 엔트리. */
		u32 *iopte = iopte_offset(iopgd, da);

		bytes = IOPTE_SIZE;
		/* [한국어] 64KB 매핑이면 16개가 한 덩어리다. */
		if (*iopte & IOPTE_LARGE) {
			nent *= 16;	/* [한국어] 64KB 매핑은 16개 엔트리가 한 덩어리다. */
			/* rewind to the 1st entry */
			/* [한국어] 요청 주소가 구간 중간이어도 첫 엔트리부터
			 * 지워야 하므로 되감는다. */
			iopte = iopte_offset(iopgd, (da & IOLARGE_MASK));
		}
		bytes *= nent;
		/* [한국어] 해당 엔트리들을 0으로 지운다. */
		memset(iopte, 0, nent * sizeof(*iopte));
		pt_dma = iopgd_page_paddr(iopgd);
		/* [한국어] 지운 결과를 하드웨어에 밀어낸다. */
		flush_iopte_range(obj->dev, pt_dma, pt_offset, nent);

		/*
		 * do table walk to check if this table is necessary or not
		 */
		/* [한국어] 이 2단계 테이블에 남은 매핑이 있는지 전부 훑는다. */
		iopte = iopte_offset(iopgd, 0);
		for (i = 0; i < PTRS_PER_IOPTE; i++)
			/* [한국어] 하나라도 남아 있으면 테이블을 유지한다 —
			 * 1단계 엔트리도 건드리지 않고 나간다. */
			if (iopte[i])
				goto out;

		/* [한국어] 완전히 비었으니 테이블을 반납한다. 위의 memset
		 * 덕분에 "깨끗한 상태로 반납" 규약이 지켜진다. */
		iopte_free(obj, iopte, true);
		/* [한국어] 이제 1단계 엔트리 하나를 지울 차례다. */
		nent = 1; /* for the next L1 entry */
	} else {
		/* [한국어] 1단계에서 끝나는 매핑(1MB 또는 16MB). */
		bytes = IOPGD_SIZE;
		/* [한국어] 16MB 슈퍼섹션이면 1단계 엔트리 16개가 한 덩어리다. */
		if ((*iopgd & IOPGD_SUPER) == IOPGD_SUPER) {
			nent *= 16;	/* [한국어] 16MB 매핑은 1단계 엔트리 16개가 한 덩어리다. */
			/* rewind to the 1st entry */
			/* [한국어] 구간의 첫 엔트리로 되감는다. */
			iopgd = iopgd_offset(obj, (da & IOSUPER_MASK));
		}
		bytes *= nent;	/* [한국어] 실제로 해제하는 바이트 수를 구한다. */
	}
	/* [한국어] 1단계 엔트리를 지운다. 위 두 갈래 모두 여기로 온다 —
	 * 테이블을 반납한 경우와 섹션을 지우는 경우다. */
	memset(iopgd, 0, nent * sizeof(*iopgd));
	flush_iopte_range(obj->dev, obj->pd_dma, pd_offset, nent);
/* [한국어] 2단계 테이블이 아직 쓰이고 있어 1단계를 건드리지
 * 않는 경로가 여기로 뛰어든다. */
out:
	return bytes;	/* [한국어] 해제한 바이트 수를 호출자에게 알린다. */
}

/**
 * iopgtable_clear_entry - Remove an iommu pte entry
 * @obj:	target iommu
 * @da:		iommu device virtual address
 **/
/*
 * [한국어]
 * iopgtable_clear_entry - 엔트리를 지우고 TLB도 비운다
 *
 * @obj: 대상 MMU.
 * @da: 지울 가상 주소.
 * @return: 해제한 바이트 수.
 *
 * 테이블 정리와 TLB 무효화를 **같은 락 구간에** 묶는다.
 * 그 사이에 다른 CPU가 같은 주소를 매핑하면, 지운 TLB 항목이
 * 다시 채워지기 전에 무효화가 끝나 버릴 수 있기 때문이다.
 *
 * 실행 컨텍스트: 해제 경로. 여기서 page_table_lock을 잡는다.
 *
 * 호출 체인:
 *   omap_iommu_unmap() / omap_iommu_map()의 되돌리기
 *   → [iopgtable_clear_entry]
 */
static size_t iopgtable_clear_entry(struct omap_iommu *obj, u32 da)
{
	/* [한국어] 해제한 바이트 수. */
	size_t bytes;

	/* [한국어] 테이블 정리와 TLB 무효화를 한 구간으로 묶는다. */
	spin_lock(&obj->page_table_lock);

	bytes = iopgtable_clear_entry_core(obj, da);
	/* [한국어] 지운 매핑의 TLB 항목도 함께 버린다. */
	flush_iotlb_page(obj, da);

	spin_unlock(&obj->page_table_lock);	/* [한국어] 정리 구간이 끝났으니 락을 놓는다. */

	return bytes;	/* [한국어] 해제한 바이트 수를 전한다. */
}

/*
 * [한국어]
 * iopgtable_clear_entry_all - 테이블 전체를 비운다
 *
 * @obj: 대상 MMU.
 * @return: 없음.
 *
 * detach 시점에 이 MMU의 모든 매핑을 정리한다. 1단계 4096개를
 * 훑으며 2단계 테이블을 반납하고 엔트리를 지운다.
 *
 * 엔트리를 하나씩 플러시하는 점이 눈에 띈다 — 범위로 묶으면
 * 빠르겠지만, 어차피 detach 경로라 성능이 문제가 되지 않는다.
 *
 * 마지막의 전역 TLB 플러시가 필수다. 테이블을 다 지워도 TLB에
 * 남은 항목은 여전히 유효하게 동작한다.
 *
 * 실행 컨텍스트: detach 경로. 여기서 page_table_lock을 잡는다.
 *
 * 호출 체인:
 *   _omap_iommu_detach_dev() → [iopgtable_clear_entry_all]
 */
static void iopgtable_clear_entry_all(struct omap_iommu *obj)
{
	/* [한국어] 플러시할 위치. */
	unsigned long offset;
	/* [한국어] 1단계 순회 인덱스. */
	int i;

	/* [한국어] 테이블 전체를 만지는 동안 잠근다. */
	spin_lock(&obj->page_table_lock);

	for (i = 0; i < PTRS_PER_IOPGD; i++) {
		/* [한국어] 이 인덱스에 해당하는 가상 주소. */
		u32 da;
		/* [한국어] 그 1단계 엔트리. */
		u32 *iopgd;

		/* [한국어] 인덱스를 주소로 되돌려 오프셋 계산에 쓴다. */
		da = i << IOPGD_SHIFT;
		iopgd = iopgd_offset(obj, da);	/* [한국어] 그 인덱스의 1단계 엔트리를 찾는다. */
		offset = iopgd_index(da) * sizeof(da);

		/* [한국어] 비어 있으면 건너뛴다. */
		if (!*iopgd)
			continue;

		/* [한국어] 2단계 테이블이 달려 있으면 그것부터 반납한다. */
		if (iopgd_is_table(*iopgd))
			iopte_free(obj, iopte_offset(iopgd, 0), true);

		/* [한국어] 1단계 엔트리를 지우고 밀어낸다. */
		*iopgd = 0;
		flush_iopte_range(obj->dev, obj->pd_dma, offset, 1);
	}

	/* [한국어] 테이블을 다 지워도 TLB에 남은 항목은 여전히
	 * 동작하므로 반드시 비워야 한다. */
	flush_iotlb_all(obj);

	spin_unlock(&obj->page_table_lock);	/* [한국어] 테이블 정리가 끝났으니 락을 놓는다. */
}

/*
 *	Device IOMMU generic operations
 */
/*
 * [한국어]
 * iommu_fault_handler - MMU 폴트 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @data: 등록 시 넘긴 MMU 인스턴스.
 * @return: 처리했으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * 상위 핸들러에게 먼저 기회를 주는 구조다. report_iommu_fault가
 * 0을 돌려주면 그쪽이 처리한 것이라 여기서 끝낸다 — 원본 주석의
 * "TLB/PTE 동적 적재"가 그 쓰임새로, 클라이언트가 폴트를 받아
 * 그때그때 매핑을 채우는 방식이다.
 *
 * 아무도 처리하지 못하면 **인터럽트를 꺼 버린다.** 같은 폴트가
 * 무한히 반복되어 시스템을 마비시키는 것을 막기 위함이며,
 * 대신 그 MMU는 이후 폴트를 보고하지 못한다.
 *
 * 그 뒤에 테이블을 되짚어 진단을 남기고 IRQ_NONE을 돌려주는데,
 * 처리하지 못했음을 커널에 알려 인터럽트 폭주 감지가 작동하게
 * 하려는 것이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [iommu_fault_handler] → report_iommu_fault()
 */
static irqreturn_t iommu_fault_handler(int irq, void *data)
{
	/* [한국어] 폴트 주소와 상태 비트. */
	u32 da, errs;
	/* [한국어] 진단을 위해 되짚을 엔트리들. */
	u32 *iopgd, *iopte;
	/* [한국어] 폴트를 낸 MMU. */
	struct omap_iommu *obj = data;
	/* [한국어] 그 MMU가 붙어 있는 도메인. */
	struct iommu_domain *domain = obj->domain;
	/* [한국어] 드라이버 쪽 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);

	/* [한국어] 클라이언트가 붙어 있지 않으면 우리 폴트가 아니다 —
	 * 인터럽트를 공유하는 다른 MMU의 것일 수 있다. */
	if (!omap_domain->dev)
		return IRQ_NONE;

	/* [한국어] 상태와 주소를 읽고 인터럽트를 지운다. */
	errs = iommu_report_fault(obj, &da);
	/* [한국어] 우리 폴트 비트가 없다 — 이미 지워진 것이거나
	 * 남의 것이다. */
	if (errs == 0)
		return IRQ_HANDLED;

	/* Fault callback or TLB/PTE Dynamic loading */
	/* [한국어] 상위 핸들러가 처리했다면 끝이다. 클라이언트가
	 * 폴트를 받아 그때그때 매핑을 채우는 방식도 여기에 해당한다. */
	if (!report_iommu_fault(domain, obj->dev, da, 0))
		return IRQ_HANDLED;

	/* [한국어] 아무도 처리하지 못했다 — 같은 폴트가 무한히
	 * 반복되지 않도록 이 MMU의 인터럽트를 꺼 버린다. */
	iommu_write_reg(obj, 0, MMU_IRQENABLE);

	/* [한국어] 진단을 위해 1단계 엔트리를 읽는다. */
	iopgd = iopgd_offset(obj, da);

	/* [한국어] 2단계 테이블이 없으면 여기까지가 보여 줄 수 있는
	 * 전부다 — 1단계 엔트리 값만 남기고 끝낸다. */
	if (!iopgd_is_table(*iopgd)) {
		dev_err(obj->dev, "%s: errs:0x%08x da:0x%08x pgd:0x%p *pgd:px%08x\n",	/* [한국어] 1단계 엔트리 값만으로 진단 정보를 남긴다. */
			obj->name, errs, da, iopgd, *iopgd);
		return IRQ_NONE;	/* [한국어] 처리하지 못했음을 커널에 알린다. */
	}

	/* [한국어] 2단계까지 내려가 그 엔트리도 함께 보여 준다. */
	iopte = iopte_offset(iopgd, da);

	dev_err(obj->dev, "%s: errs:0x%08x da:0x%08x pgd:0x%p *pgd:0x%08x pte:0x%p *pte:0x%08x\n",	/* [한국어] 두 단계 엔트리를 모두 보여 진단을 돕는다. */
		obj->name, errs, da, iopgd, *iopgd, iopte, *iopte);

	/* [한국어] 처리하지 못했음을 알려 커널의 인터럽트 폭주 감지가
	 * 작동하게 한다. */
	return IRQ_NONE;
}

/**
 * omap_iommu_attach() - attach iommu device to an iommu domain
 * @obj:	target omap iommu device
 * @iopgd:	page table
 **/
/*
 * [한국어]
 * omap_iommu_attach - MMU 인스턴스에 테이블을 걸고 켠다
 *
 * @obj: 대상 MMU.
 * @iopgd: 이 MMU가 쓸 1단계 테이블.
 * @return: 0 성공, -ENOMEM 또는 전원 오류.
 *
 * 테이블을 DMA 매핑하고, 그 주소를 기억한 뒤 전원을 켠다.
 * 실제 레지스터 설정은 런타임 리줌 콜백이 하는데, 그것이
 * obj->iopgd를 읽으므로 **전원을 켜기 전에 설정해 두어야 한다.**
 * 순서가 이 함수의 요점이다.
 *
 * 실행 컨텍스트: attach 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_attach_dev() → [omap_iommu_attach] → iommu_enable()
 */
static int omap_iommu_attach(struct omap_iommu *obj, u32 *iopgd)
{
	/* [한국어] 전원 켜기 결과. */
	int err;

	/* [한국어] 이 인스턴스의 상태를 바꾸는 구간을 잠근다. */
	spin_lock(&obj->iommu_lock);

	/* [한국어] 하드웨어가 1단계 테이블을 읽을 수 있게 매핑한다. */
	obj->pd_dma = dma_map_single(obj->dev, iopgd, IOPGD_TABLE_SIZE,
				     DMA_TO_DEVICE);
	if (dma_mapping_error(obj->dev, obj->pd_dma)) {	/* [한국어] 하드웨어가 1단계 테이블을 읽을 수 없다. */
		dev_err(obj->dev, "DMA map error for L1 table\n");	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
		err = -ENOMEM;	/* [한국어] 테이블을 걸 수 없다. */
		goto out_err;	/* [한국어] 락을 풀고 나간다. */
	}

	/* [한국어] 리줌 콜백이 이 값을 읽어 TTB에 심으므로,
	 * 전원을 켜기 **전에** 설정해야 한다. */
	obj->iopgd = iopgd;
	err = iommu_enable(obj);	/* [한국어] 전원을 켜면 리줌 콜백이 레지스터를 설정한다. */
	if (err)	/* [한국어] 전원 켜기에 실패했다. */
		goto out_err;
	/* [한국어] 새 테이블을 걸었으니 옛 TLB 항목을 모두 버린다. */
	flush_iotlb_all(obj);

	spin_unlock(&obj->iommu_lock);	/* [한국어] 상태 변경이 끝났으니 락을 놓는다. */

	dev_dbg(obj->dev, "%s: %s\n", __func__, obj->name);	/* [한국어] 붙이기가 끝났음을 남긴다. */

	return 0;

/* [한국어] DMA 매핑이나 전원 켜기에 실패했다. */
out_err:
	spin_unlock(&obj->iommu_lock);	/* [한국어] 실패 경로에서도 락을 놓는다. */

	return err;	/* [한국어] 실패 이유를 전한다. */
}

/**
 * omap_iommu_detach - release iommu device
 * @obj:	target iommu
 **/
/*
 * [한국어]
 * omap_iommu_detach - MMU 인스턴스에서 테이블을 떼고 끈다
 *
 * @obj: 대상 MMU.
 * @return: 없음.
 *
 * attach의 역순이다. iopgd를 NULL로 만드는 것이 중요한데,
 * 서스펜드 콜백이 그 값을 보고 "저장할 TLB가 있는가"를 판단하기
 * 때문이다.
 *
 * 실행 컨텍스트: detach 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_attach_dev()의 되돌리기 / _omap_iommu_detach_dev()
 *   → [omap_iommu_detach]
 */
static void omap_iommu_detach(struct omap_iommu *obj)
{
	/* [한국어] 붙은 적이 없거나 오류 포인터면 할 일이 없다. */
	if (!obj || IS_ERR(obj))
		return;

	/* [한국어] 상태 변경 구간을 잠근다. */
	spin_lock(&obj->iommu_lock);

	/* [한국어] 1단계 테이블의 DMA 매핑을 푼다. */
	dma_unmap_single(obj->dev, obj->pd_dma, IOPGD_TABLE_SIZE,
			 DMA_TO_DEVICE);
	obj->pd_dma = 0;
	/* [한국어] 서스펜드 콜백이 이 값으로 "저장할 것이 있는가"를
	 * 판단하므로 반드시 지운다. */
	obj->iopgd = NULL;
	/* [한국어] 전원 참조를 놓아 서스펜드 콜백이 하드웨어를 끄게 한다. */
	iommu_disable(obj);

	spin_unlock(&obj->iommu_lock);	/* [한국어] 상태 변경이 끝났으니 락을 놓는다. */

	dev_dbg(obj->dev, "%s: %s\n", __func__, obj->name);	/* [한국어] 떼어 냈음을 남긴다. */
}

/*
 * [한국어]
 * omap_iommu_save_tlb_entries - 고정된 TLB 항목을 떠 둔다
 *
 * @obj: 대상 MMU.
 * @return: 없음.
 *
 * 절전으로 전원이 끊기면 TLB 내용이 사라진다. 일반 항목은
 * 테이블 워크로 다시 채워지므로 잃어도 되지만, **고정 항목은
 * 소프트웨어가 넣은 것이라 스스로 되살아나지 않는다.**
 * 그래서 base(고정 개수)만큼만 떠 둔다.
 *
 * 실행 컨텍스트: 런타임 서스펜드. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_runtime_suspend() → [omap_iommu_save_tlb_entries]
 */
static void omap_iommu_save_tlb_entries(struct omap_iommu *obj)
{
	/* [한국어] 고정 개수를 읽을 lock 값. */
	struct iotlb_lock lock;
	/* [한국어] 순회하며 읽는 항목. */
	struct cr_regs cr;
	/* [한국어] 저장 버퍼의 커서. */
	struct cr_regs *tmp;
	/* [한국어] 순회 인덱스. */
	int i;

	/* check if there are any locked tlbs to save */
	/* [한국어] base가 곧 고정 항목의 개수다. */
	iotlb_lock_get(obj, &lock);
	obj->num_cr_ctx = lock.base;
	/* [한국어] 고정된 것이 없으면 잃어도 되는 항목뿐이다. */
	if (!obj->num_cr_ctx)
		return;

	/* [한국어] 고정 영역만 순서대로 떠 둔다. */
	tmp = obj->cr_ctx;
	for_each_iotlb_cr(obj, obj->num_cr_ctx, i, cr)	/* [한국어] 고정 영역만 순서대로 읽어 버퍼에 담는다. */
		* tmp++ = cr;
}

/*
 * [한국어]
 * omap_iommu_restore_tlb_entries - 떠 둔 고정 TLB 항목을 되돌린다
 *
 * @obj: 대상 MMU.
 * @return: 없음.
 *
 * 저장의 대칭이지만 한 단계가 더 있다. 항목을 되돌린 뒤
 * **lock을 원래대로 맞춰야 한다** — base를 고정 개수로,
 * vict를 그다음 자리로 놓아야 이후의 일반 항목 적재가 고정
 * 영역을 덮어쓰지 않는다.
 *
 * 실행 컨텍스트: 런타임 리줌. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_runtime_resume() → [omap_iommu_restore_tlb_entries]
 */
static void omap_iommu_restore_tlb_entries(struct omap_iommu *obj)
{
	/* [한국어] 자리를 고르고 마지막에 되돌릴 lock 값. */
	struct iotlb_lock l;
	/* [한국어] 저장 버퍼의 커서. */
	struct cr_regs *tmp;
	/* [한국어] 순회 인덱스. 루프 뒤에도 쓰인다. */
	int i;

	/* no locked tlbs to restore */
	/* [한국어] 떠 둔 것이 없으면 할 일이 없다. */
	if (!obj->num_cr_ctx)
		return;

	/* [한국어] 복원하는 동안에는 고정 영역이 없는 것으로 두어야
	 * 0번 자리부터 쓸 수 있다. */
	l.base = 0;
	tmp = obj->cr_ctx;	/* [한국어] 저장 버퍼의 처음부터 되돌린다. */
	for (i = 0; i < obj->num_cr_ctx; i++, tmp++) {
		/* [한국어] 자리를 고르고 그 자리에 적재한다. */
		l.vict = i;
		iotlb_lock_set(obj, &l);	/* [한국어] 복원할 자리를 고른다. */
		iotlb_load_cr(obj, tmp);	/* [한국어] 그 자리에 항목을 적재한다. */
	}
	/* [한국어] 복원이 끝났으니 고정 영역의 크기를 되살리고,
	 * 다음 교체 자리를 그 바로 뒤로 놓는다. 그러지 않으면
	 * 이후의 일반 항목이 고정 영역을 덮어쓴다. */
	l.base = obj->num_cr_ctx;
	l.vict = i;	/* [한국어] 다음 교체 자리는 고정 영역 바로 뒤다. */
	iotlb_lock_set(obj, &l);	/* [한국어] 되살린 lock 값을 하드웨어에 쓴다. */
}

/**
 * omap_iommu_domain_deactivate - deactivate attached iommu devices
 * @domain: iommu domain attached to the target iommu device
 *
 * This API allows the client devices of IOMMU devices to suspend
 * the IOMMUs they control at runtime, after they are idled and
 * suspended all activity. System Suspend will leverage the PM
 * driver late callbacks.
 **/
/*
 * [한국어]
 * omap_iommu_domain_deactivate - 도메인의 모든 MMU를 잠재운다
 *
 * @domain: 대상 도메인.
 * @return: 항상 0.
 *
 * 클라이언트 드라이버가 자기 일이 끝났을 때 부르는 API다.
 * 전원 참조를 놓으면 서스펜드 콜백이 하드웨어를 정리한다.
 *
 * **역순으로 순회하는 것**에 유의 — 인스턴스 사이에 하드웨어
 * 의존이 있을 수 있어, 켠 순서의 반대로 끈다.
 *
 * 실행 컨텍스트: 클라이언트의 유휴 처리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   클라이언트 드라이버(예: remoteproc) → [omap_iommu_domain_deactivate]
 */
int omap_iommu_domain_deactivate(struct iommu_domain *domain)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);
	/* [한국어] 순회 커서. */
	struct omap_iommu_device *iommu;
	/* [한국어] 현재 인스턴스. */
	struct omap_iommu *oiommu;
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 붙어 있는 클라이언트가 없으면 켠 것도 없다. */
	if (!omap_domain->dev)
		return 0;

	/* [한국어] 마지막 인스턴스부터 거꾸로 훑는다 — 켠 순서의
	 * 반대로 꺼야 하드웨어 의존을 지킬 수 있다. */
	iommu = omap_domain->iommus;
	iommu += (omap_domain->num_iommus - 1);	/* [한국어] 마지막 인스턴스로 커서를 옮긴다. */
	for (i = 0; i < omap_domain->num_iommus; i++, iommu--) {	/* [한국어] 켠 순서의 반대로 훑는다. */
		oiommu = iommu->iommu_dev;
		/* [한국어] 참조를 놓으면 서스펜드 콜백이 정리한다. */
		pm_runtime_put_sync(oiommu->dev);
	}

	return 0;	/* [한국어] 모든 인스턴스의 참조를 놓았다. */
}
/* [한국어] 클라이언트 드라이버가 모듈이라 내보낸다. */
EXPORT_SYMBOL_GPL(omap_iommu_domain_deactivate);

/**
 * omap_iommu_domain_activate - activate attached iommu devices
 * @domain: iommu domain attached to the target iommu device
 *
 * This API allows the client devices of IOMMU devices to resume the
 * IOMMUs they control at runtime, before they can resume operations.
 * System Resume will leverage the PM driver late callbacks.
 **/
/*
 * [한국어]
 * omap_iommu_domain_activate - 도메인의 모든 MMU를 깨운다
 *
 * @domain: 대상 도메인.
 * @return: 항상 0.
 *
 * deactivate의 대칭이며, **정순으로** 순회한다. 리줌 콜백이
 * 레지스터를 설정하고 고정 TLB를 되돌린다.
 *
 * 실행 컨텍스트: 클라이언트의 재개 처리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   클라이언트 드라이버 → [omap_iommu_domain_activate]
 */
int omap_iommu_domain_activate(struct iommu_domain *domain)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);
	/* [한국어] 순회 커서. */
	struct omap_iommu_device *iommu;
	/* [한국어] 현재 인스턴스. */
	struct omap_iommu *oiommu;
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 붙어 있는 클라이언트가 없으면 깨울 것도 없다. */
	if (!omap_domain->dev)
		return 0;

	/* [한국어] 첫 인스턴스부터 정순으로 깨운다. */
	iommu = omap_domain->iommus;
	for (i = 0; i < omap_domain->num_iommus; i++, iommu++) {	/* [한국어] 첫 인스턴스부터 정순으로 훑는다. */
		oiommu = iommu->iommu_dev;
		/* [한국어] 참조를 올리면 리줌 콜백이 하드웨어를 설정한다. */
		pm_runtime_get_sync(oiommu->dev);
	}

	return 0;	/* [한국어] 모든 인스턴스를 깨웠다. */
}
/* [한국어] deactivate와 짝을 이뤄 내보낸다. */
EXPORT_SYMBOL_GPL(omap_iommu_domain_activate);

/**
 * omap_iommu_runtime_suspend - disable an iommu device
 * @dev:	iommu device
 *
 * This function performs all that is necessary to disable an
 * IOMMU device, either during final detachment from a client
 * device, or during system/runtime suspend of the device. This
 * includes programming all the appropriate IOMMU registers, and
 * managing the associated omap_hwmod's state and the device's
 * reset line. This function also saves the context of any
 * locked TLBs if suspending.
 **/
/*
 * [한국어]
 * omap_iommu_runtime_suspend - MMU 하드웨어를 정리한다
 *
 * @dev: MMU 디바이스.
 * @return: 항상 0.
 *
 * 이 드라이버에서 하드웨어를 실제로 끄는 유일한 곳이다.
 * detach와 절전이 모두 이 경로로 수렴한다.
 *
 * 첫 조건이 미묘하다. **domain과 iopgd가 모두 있을 때만** TLB를
 * 저장하는데, 이것이 "절전"과 "완전한 detach"를 구별한다.
 * detach 경로에서는 이미 iopgd가 NULL이라 저장을 건너뛴다 —
 * 되돌릴 도메인이 없으니 저장해 봐야 소용없기 때문이다.
 *
 * 나머지는 플랫폼 콜백들이다. OMAP의 hwmod 시대에서 이어진 층으로,
 * 디바이스 유휴화, 리셋 라인 걸기, 전원 도메인 제약 해제 순서로
 * 진행한다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PM 코어 → [omap_iommu_runtime_suspend] → omap2_iommu_disable()
 */
static __maybe_unused int omap_iommu_runtime_suspend(struct device *dev)
{
	/* [한국어] 플랫폼 콜백에 넘길 디바이스. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 리셋과 전원 제약을 다루는 플랫폼 콜백 묶음. */
	struct iommu_platform_data *pdata = dev_get_platdata(dev);
	/* [한국어] 대상 MMU 인스턴스. */
	struct omap_iommu *obj = to_iommu(dev);
	/* [한국어] 전원 제약 해제 결과. */
	int ret;

	/* save the TLBs only during suspend, and not for power down */
	/* [한국어] 두 조건이 함께 참일 때만 절전이다. detach 경로에서는
	 * 이미 iopgd가 NULL이라 저장을 건너뛴다 — 되돌릴 도메인이
	 * 없으니 의미가 없다. */
	if (obj->domain && obj->iopgd)
		omap_iommu_save_tlb_entries(obj);

	/* [한국어] MMU를 끈다. */
	omap2_iommu_disable(obj);

	/* [한국어] 플랫폼이 정의한 유휴화 절차. */
	if (pdata && pdata->device_idle)
		pdata->device_idle(pdev);

	/* [한국어] 리셋 라인을 걸어 블록을 확실히 멈춘다. */
	if (pdata && pdata->assert_reset)
		pdata->assert_reset(pdev, pdata->reset_name);

	/* [한국어] 걸어 둔 전원 도메인 제약을 푼다 — 이것이 있어야
	 * 전원 도메인이 실제로 내려갈 수 있다. */
	if (pdata && pdata->set_pwrdm_constraint) {
		ret = pdata->set_pwrdm_constraint(pdev, false, &obj->pwrst);	/* [한국어] 전원 도메인 제약을 푼다. */
		if (ret) {	/* [한국어] 제약 해제가 실패한 경우. */
			dev_warn(obj->dev, "pwrdm_constraint failed to be reset, status = %d\n",	/* [한국어] 전원 도메인이 내려가지 못할 수 있음을 알린다. */
				 ret);
		}
	}

	return 0;	/* [한국어] 서스펜드는 실패로 처리하지 않는다. */
}

/**
 * omap_iommu_runtime_resume - enable an iommu device
 * @dev:	iommu device
 *
 * This function performs all that is necessary to enable an
 * IOMMU device, either during initial attachment to a client
 * device, or during system/runtime resume of the device. This
 * includes programming all the appropriate IOMMU registers, and
 * managing the associated omap_hwmod's state and the device's
 * reset line. The function also restores any locked TLBs if
 * resuming after a suspend.
 **/
/*
 * [한국어]
 * omap_iommu_runtime_resume - MMU 하드웨어를 설정하고 켠다
 *
 * @dev: MMU 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 서스펜드의 정확한 역순이다: 전원 제약을 걸고 → 리셋을 풀고
 * → 디바이스를 켜고 → 고정 TLB를 되돌리고 → MMU를 켠다.
 *
 * TLB 복원이 MMU 활성화보다 **앞**인 것이 중요하다. 켜자마자
 * 클라이언트의 접근이 들어올 수 있어, 그 전에 고정 항목이
 * 제자리에 있어야 한다.
 *
 * 여기서는 domain만 검사하고 iopgd는 보지 않는데, save 쪽에서
 * num_cr_ctx를 0으로 남겨 두므로 복원 함수가 스스로 걸러 낸다.
 *
 * 실행 컨텍스트: 런타임 PM 콜백. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PM 코어 → [omap_iommu_runtime_resume] → omap2_iommu_enable()
 */
static __maybe_unused int omap_iommu_runtime_resume(struct device *dev)
{
	/* [한국어] 플랫폼 콜백에 넘길 디바이스. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 플랫폼 콜백 묶음. */
	struct iommu_platform_data *pdata = dev_get_platdata(dev);
	/* [한국어] 대상 MMU 인스턴스. */
	struct omap_iommu *obj = to_iommu(dev);
	/* [한국어] 단계별 결과. */
	int ret = 0;

	/* [한국어] 전원 도메인이 내려가지 않도록 제약을 건다 —
	 * 이후 단계들이 전원을 전제하기 때문이다. */
	if (pdata && pdata->set_pwrdm_constraint) {
		ret = pdata->set_pwrdm_constraint(pdev, true, &obj->pwrst);	/* [한국어] 전원 도메인이 내려가지 않도록 제약을 건다. */
		if (ret) {	/* [한국어] 제약 설정이 실패한 경우. */
			dev_warn(obj->dev, "pwrdm_constraint failed to be set, status = %d\n",	/* [한국어] 경고만 남기고 계속 진행한다. */
				 ret);
		}
	}

	/* [한국어] 리셋을 푼다. 이것이 실패하면 레지스터에 접근할 수
	 * 없으므로 여기서 멈춘다. */
	if (pdata && pdata->deassert_reset) {
		ret = pdata->deassert_reset(pdev, pdata->reset_name);	/* [한국어] 리셋 라인을 푼다. */
		if (ret) {	/* [한국어] 리셋 해제가 실패한 경우. */
			dev_err(dev, "deassert_reset failed: %d\n", ret);	/* [한국어] 레지스터에 접근할 수 없음을 알린다. */
			return ret;	/* [한국어] 더 진행할 수 없다. */
		}
	}

	/* [한국어] 플랫폼이 정의한 활성화 절차. */
	if (pdata && pdata->device_enable)
		pdata->device_enable(pdev);

	/* restore the TLBs only during resume, and not for power up */
	/* [한국어] MMU를 켜기 **전에** 고정 항목을 되돌린다 —
	 * 켜자마자 접근이 들어올 수 있기 때문이다.
	 * 저장된 것이 없으면 복원 함수가 스스로 걸러 낸다. */
	if (obj->domain)
		omap_iommu_restore_tlb_entries(obj);

	/* [한국어] 마지막으로 레지스터를 설정하고 MMU를 켠다. */
	ret = omap2_iommu_enable(obj);

	return ret;	/* [한국어] MMU 활성화 결과를 PM 코어에 전한다. */
}

/**
 * omap_iommu_prepare - prepare() dev_pm_ops implementation
 * @dev:	iommu device
 *
 * This function performs the necessary checks to determine if the IOMMU
 * device needs suspending or not. The function checks if the runtime_pm
 * status of the device is suspended, and returns 1 in that case. This
 * results in the PM core to skip invoking any of the Sleep PM callbacks
 * (suspend, suspend_late, resume, resume_early etc).
 */
/*
 * [한국어]
 * omap_iommu_prepare - 시스템 절전에서 이 디바이스를 건너뛸지 정한다
 *
 * @dev: MMU 디바이스.
 * @return: 1이면 절전 콜백 전부를 건너뛴다, 0이면 정상 진행.
 *
 * 이미 런타임 서스펜드된 디바이스는 시스템 절전에서 다시 다룰
 * 이유가 없다. 1을 돌려주면 PM 코어가 이 디바이스의 절전 콜백을
 * 통째로 건너뛴다 — 불필요하게 깨웠다 재우는 낭비를 없애는
 * 표준적인 최적화다.
 *
 * 실행 컨텍스트: 시스템 절전 준비 단계.
 *
 * 호출 체인:
 *   PM 코어 → [omap_iommu_prepare]
 */
static int omap_iommu_prepare(struct device *dev)
{
	/* [한국어] 이미 잠들어 있으면 절전 콜백을 전부 건너뛰게 한다. */
	if (pm_runtime_status_suspended(dev))
		return 1;
	return 0;	/* [한국어] 제한할 이유가 없으므로 등록을 허용한다. */
}

/*
 * [한국어]
 * omap_iommu_can_register - 이 인스턴스를 IOMMU 코어에 등록할지 정한다
 *
 * @pdev: 대상 플랫폼 디바이스.
 * @return: 등록해도 되면 참.
 *
 * DRA7 DSP에는 MMU가 여럿 있는데, 그중 **프로세서 포트의 MDMA
 * MMU만** 리눅스 IOMMU로 노출한다. 나머지는 DSP 내부 전용이라
 * 커널이 관리할 대상이 아니다.
 *
 * 그 판별을 디바이스 이름(= 레지스터 주소)으로 하는 것이 거칠지만,
 * 디바이스 트리에 그 구분을 나타낼 속성이 없어 택한 방법이다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_probe() → [omap_iommu_can_register]
 */
static bool omap_iommu_can_register(struct platform_device *pdev)
{
	/* [한국어] 이 디바이스의 트리 노드. */
	struct device_node *np = pdev->dev.of_node;

	/* [한국어] DSP MMU가 아니면 제한할 이유가 없다. */
	if (!of_device_is_compatible(np, "ti,dra7-dsp-iommu"))
		return true;

	/*
	 * restrict IOMMU core registration only for processor-port MDMA MMUs
	 * on DRA7 DSPs
	 */
	/* [한국어] 두 DSP의 MDMA MMU만 이름(= 레지스터 주소)으로
	 * 골라 낸다. 트리에 이 구분을 나타낼 속성이 없어 택한 방법이다. */
	if ((!strcmp(dev_name(&pdev->dev), "40d01000.mmu")) ||
	    (!strcmp(dev_name(&pdev->dev), "41501000.mmu")))
		return true;

	/* [한국어] 나머지 DSP 내부 MMU는 커널이 관리하지 않는다. */
	return false;
}

/*
 * [한국어]
 * omap_iommu_dra7_get_dsp_system_cfg - DRA7 DSP의 시스템 설정 접근을 준비한다
 *
 * @pdev: 대상 플랫폼 디바이스.
 * @obj: 채울 MMU 인스턴스.
 * @return: 0 성공, 음수 오류.
 *
 * DRA7 DSP MMU는 자기 레지스터 외에 공유 시스템 설정 레지스터에서도
 * 활성화해야 한다. 그 레지스터의 regmap과, 그 안에서 이 인스턴스가
 * 쓸 비트 번호(id)를 디바이스 트리에서 얻는다.
 *
 * 다른 MMU에서는 아무것도 하지 않고 성공을 돌려주므로, 호출부가
 * 세대를 가릴 필요가 없다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   omap_iommu_probe() → [omap_iommu_dra7_get_dsp_system_cfg]
 */
static int omap_iommu_dra7_get_dsp_system_cfg(struct platform_device *pdev,
					      struct omap_iommu *obj)
{
	/* [한국어] 이 디바이스의 트리 노드. */
	struct device_node *np = pdev->dev.of_node;

	/* [한국어] DSP MMU가 아니면 이 설정이 필요 없다. */
	if (!of_device_is_compatible(np, "ti,dra7-dsp-iommu"))
		return 0;

	/* [한국어] 공유 설정 레지스터의 regmap과, 그 안에서 이
	 * 인스턴스의 번호를 함께 얻는다. */
	obj->syscfg = syscon_regmap_lookup_by_phandle_args(np, "ti,syscon-mmuconfig",
							   1, &obj->id);
	if (IS_ERR(obj->syscfg))	/* [한국어] 공유 설정 레지스터를 찾지 못했다. */
		return dev_err_probe(&pdev->dev, PTR_ERR(obj->syscfg),
				     "ti,syscon-mmuconfig property is missing\n");

	/* [한국어] 번호가 비트 위치를 정하므로 0이나 1이어야 한다 —
	 * DSP당 MMU가 둘이기 때문이다. */
	if (obj->id != 0 && obj->id != 1) {
		dev_err(&pdev->dev, "invalid IOMMU instance id\n");	/* [한국어] 비트 위치를 정할 수 없는 번호다. */
		return -EINVAL;	/* [한국어] 이 인스턴스를 다룰 수 없다. */
	}

	return 0;	/* [한국어] 시스템 설정 접근 준비가 끝났다. */
}

/*
 *	OMAP Device MMU(IOMMU) detection
 */
/*
 * [한국어]
 * omap_iommu_probe - MMU 인스턴스 하나를 초기화한다
 *
 * @pdev: 플랫폼 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 눈여겨볼 곳이 넷이다.
 *
 * (1) **pm_domain을 일부러 지운다.** OMAP의 hwmod 전원 도메인이
 *     디바이스를 자동으로 켜고 끄면, 이 드라이버의 리셋 조작과
 *     순서가 어긋난다. 그래서 전원 관리를 직접 하겠다고 선언하는
 *     것이다 — 원본 주석의 "self-manage"가 그 뜻이다.
 *
 * (2) **구조체 뒤에 레지스터 저장 버퍼를 붙여 잡는다.**
 *     obj->ctx가 그 자리를 가리킨다. 폐기 예정 API인
 *     save_ctx/restore_ctx가 쓰는 공간이다.
 *
 * (3) TLB 크기는 트리가 알려 주며, 8이나 32만 허용한다 —
 *     lock 필드가 5비트라 32를 넘을 수 없다.
 *
 * (4) **sysfs 등록은 조건부지만 코어 등록은 항상 한다.**
 *     DSP 내부 MMU도 코어에는 등록해야 of_xlate 등이 동작하지만,
 *     sysfs에 노출할 이유는 없다는 판단이다.
 *
 * 실행 컨텍스트: 플랫폼 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [omap_iommu_probe] → iommu_device_register()
 */
static int omap_iommu_probe(struct platform_device *pdev)
{
	/* [한국어] 단계별 결과. */
	int err = -ENODEV;
	/* [한국어] 폴트 인터럽트 번호. */
	int irq;
	/* [한국어] 만들 MMU 인스턴스. */
	struct omap_iommu *obj;
	/* [한국어] 레지스터 자원. */
	struct resource *res;
	/* [한국어] 이 디바이스의 트리 노드. */
	struct device_node *of = pdev->dev.of_node;

	/* [한국어] 이 드라이버는 디바이스 트리 기반만 지원한다. */
	if (!of) {
		pr_err("%s: only DT-based devices are supported\n", __func__);	/* [한국어] 플랫폼 데이터 방식은 더 이상 지원하지 않는다. */
		return -ENODEV;	/* [한국어] 디바이스 트리 없이는 진행할 수 없다. */
	}

	/* [한국어] 구조체와 레지스터 저장 버퍼를 한 덩어리로 잡는다 —
	 * 아래에서 obj->ctx가 그 뒷부분을 가리킨다. */
	obj = devm_kzalloc(&pdev->dev, sizeof(*obj) + MMU_REG_SIZE, GFP_KERNEL);
	if (!obj)	/* [한국어] 상태 구조체를 잡지 못했다. */
		return -ENOMEM;

	/*
	 * self-manage the ordering dependencies between omap_device_enable/idle
	 * and omap_device_assert/deassert_hardreset API
	 */
	/* [한국어] hwmod 전원 도메인이 디바이스를 자동으로 켜고 끄면
	 * 이 드라이버의 리셋 조작과 순서가 어긋난다 — 전원 관리를
	 * 직접 하겠다고 선언하는 것이다. */
	if (pdev->dev.pm_domain) {
		dev_dbg(&pdev->dev, "device pm_domain is being reset\n");	/* [한국어] 전원 관리를 직접 하겠다고 알린다. */
		pdev->dev.pm_domain = NULL;	/* [한국어] 자동 전원 관리를 떼어 낸다. */
	}

	/* [한국어] 로그에 쓸 이름. 디바이스 이름을 그대로 쓴다. */
	obj->name = dev_name(&pdev->dev);
	/* [한국어] 기본 TLB 크기. 트리가 다른 값을 줄 수 있다. */
	obj->nr_tlb_entries = 32;
	err = of_property_read_u32(of, "ti,#tlb-entries", &obj->nr_tlb_entries);
	/* [한국어] 속성이 없는 것(-EINVAL)은 오류가 아니다 — 기본값을 쓴다. */
	if (err && err != -EINVAL)
		return err;
	/* [한국어] lock 필드가 5비트라 32가 상한이고, 하드웨어가
	 * 실제로 갖는 크기는 둘 중 하나뿐이다. */
	if (obj->nr_tlb_entries != 32 && obj->nr_tlb_entries != 8)
		return -EINVAL;
	/* [한국어] 버스 오류를 클라이언트에게 되돌려 줄지 여부.
	 * 켜면 잘못된 접근이 조용히 무시되지 않는다. */
	if (of_property_read_bool(of, "ti,iommu-bus-err-back"))
		obj->has_bus_err_back = MMU_GP_REG_BUS_ERR_BACK_EN;

	/* [한국어] 로그와 DMA 매핑의 기준 디바이스. */
	obj->dev = &pdev->dev;
	/* [한국어] 위에서 함께 잡은 뒷부분이 레지스터 저장 버퍼다. */
	obj->ctx = (void *)obj + sizeof(*obj);
	/* [한국어] 고정 TLB 항목을 떠 둘 버퍼. TLB 크기만큼 잡는다. */
	obj->cr_ctx = devm_kzalloc(&pdev->dev,
				   sizeof(*obj->cr_ctx) * obj->nr_tlb_entries,
				   GFP_KERNEL);
	if (!obj->cr_ctx)	/* [한국어] 고정 TLB 저장 버퍼를 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 인스턴스 상태를 보호할 락. */
	spin_lock_init(&obj->iommu_lock);
	/* [한국어] 페이지 테이블을 보호할 락. */
	spin_lock_init(&obj->page_table_lock);

	/* [한국어] 레지스터 영역을 매핑한다. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	obj->regbase = devm_ioremap_resource(obj->dev, res);	/* [한국어] 레지스터 영역을 매핑한다. */
	if (IS_ERR(obj->regbase))	/* [한국어] 매핑에 실패했다. */
		return PTR_ERR(obj->regbase);

	/* [한국어] DRA7 DSP라면 공유 설정 레지스터 접근도 준비한다.
	 * 다른 MMU에서는 아무 일도 하지 않는다. */
	err = omap_iommu_dra7_get_dsp_system_cfg(pdev, obj);
	if (err)	/* [한국어] 시스템 설정 준비가 실패했다. */
		return err;

	/* [한국어] 폴트 인터럽트를 얻는다. */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)	/* [한국어] 폴트 인터럽트를 얻지 못했다. */
		return -ENODEV;

	/* [한국어] 공유 인터럽트로 등록한다 — 여러 MMU가 하나의
	 * 인터럽트를 나눠 쓸 수 있어, 핸들러가 자기 것인지 가린다. */
	err = devm_request_irq(obj->dev, irq, iommu_fault_handler, IRQF_SHARED,
			       dev_name(obj->dev), obj);
	if (err < 0)	/* [한국어] 핸들러 등록에 실패했다. */
		return err;
	/* [한국어] probe_device가 이 값으로 인스턴스를 찾으므로
	 * 코어에 등록하기 전에 심어 둔다. */
	platform_set_drvdata(pdev, obj);

	/* [한국어] sysfs 노출은 조건부다 — DSP 내부 MMU는 커널이
	 * 관리하되 사용자에게 보일 이유가 없다. */
	if (omap_iommu_can_register(pdev)) {
		err = iommu_device_sysfs_add(&obj->iommu, obj->dev, NULL,	/* [한국어] sysfs에 이 인스턴스를 노출한다. */
					     obj->name);
		if (err)	/* [한국어] sysfs 등록이 실패했다. */
			return err;

		/* [한국어] remove가 sysfs를 지울지 판단할 근거로 남긴다. */
		obj->has_iommu_driver = true;
	}

	/* [한국어] 코어 등록은 조건 없이 한다 — of_xlate와 도메인
	 * 연결이 동작하려면 필요하기 때문이다. */
	err = iommu_device_register(&obj->iommu, &omap_iommu_ops, &pdev->dev);
	if (err)	/* [한국어] 코어 등록이 실패했다. */
		goto out_sysfs;

	/* [한국어] 런타임 PM을 켠다. 이 드라이버에서는 이것이 곧
	 * 하드웨어 활성화의 통로다. */
	pm_runtime_enable(obj->dev);

	/* [한국어] 디버그용 파일들을 만든다. */
	omap_iommu_debugfs_add(obj);

	dev_info(&pdev->dev, "%s registered\n", obj->name);	/* [한국어] 등록이 끝났음을 알린다. */

	return 0;

/* [한국어] 코어 등록 실패 — sysfs를 만들었다면 되돌린다. */
out_sysfs:
	if (obj->has_iommu_driver)	/* [한국어] 만들었던 sysfs 항목만 되돌린다. */
		iommu_device_sysfs_remove(&obj->iommu);
	return err;	/* [한국어] 실패 이유를 플랫폼 코어에 전한다. */
}

/*
 * [한국어]
 * omap_iommu_remove - MMU 인스턴스를 걷어낸다
 *
 * @pdev: 대상 플랫폼 디바이스.
 * @return: 없음.
 *
 * probe의 역순이다. 인터럽트와 레지스터 매핑은 devm이 정리하므로
 * 여기서는 명시적으로 만든 것들만 되돌린다.
 *
 * 실행 컨텍스트: 드라이버 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [omap_iommu_remove]
 */
static void omap_iommu_remove(struct platform_device *pdev)
{
	/* [한국어] 대상 인스턴스. */
	struct omap_iommu *obj = platform_get_drvdata(pdev);

	/* [한국어] sysfs를 만들었던 인스턴스만 지운다. */
	if (obj->has_iommu_driver)
		iommu_device_sysfs_remove(&obj->iommu);

	/* [한국어] 코어에서 뺀다. */
	iommu_device_unregister(&obj->iommu);

	/* [한국어] 디버그 파일들을 지운다. */
	omap_iommu_debugfs_remove(obj);

	/* [한국어] 런타임 PM을 끈다. */
	pm_runtime_disable(obj->dev);

	dev_info(&pdev->dev, "%s removed\n", obj->name);	/* [한국어] 제거가 끝났음을 알린다. */
}

/* [한국어] 전원 관리 콜백 묶음.
 * prepare로 이미 잠든 디바이스를 걸러 내고, 시스템 절전은
 * **late** 단계에서 런타임 PM을 강제로 오가게 한다.
 * late인 이유: 클라이언트 디바이스들이 먼저 잠든 뒤에 MMU가
 * 꺼져야 하기 때문이다. */
static const struct dev_pm_ops omap_iommu_pm_ops = {
	.prepare = omap_iommu_prepare,
	/* [한국어] 이미 잠든 디바이스의 절전 콜백을 건너뛰게 한다. */

	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(omap_iommu_runtime_suspend,
			   omap_iommu_runtime_resume, NULL)
};

/* [한국어] 이 드라이버가 붙을 디바이스 트리 노드들.
 * 세대별로 문자열이 나뉘어 있지만 코드 경로는 dra7-dsp-iommu만
 * 특별 취급한다(시스템 설정 레지스터와 등록 제한). */
static const struct of_device_id omap_iommu_of_match[] = {
	{ .compatible = "ti,omap2-iommu" },	/* [한국어] OMAP2 세대의 MMU. */
	{ .compatible = "ti,omap4-iommu" },
	{ .compatible = "ti,dra7-iommu"	},
	{ .compatible = "ti,dra7-dsp-iommu" },
	{},
};

/* [한국어] MMU 인스턴스의 플랫폼 드라이버. */
static struct platform_driver omap_iommu_driver = {
	.probe	= omap_iommu_probe,
	/* [한국어] 인스턴스 초기화 진입점. */

	.remove = omap_iommu_remove,
	/* [한국어] 인스턴스 정리 진입점. */

	.driver	= {
		.name	= "omap-iommu",
		/* [한국어] 드라이버 이름. */

		.pm	= &omap_iommu_pm_ops,
		/* [한국어] 런타임/시스템 전원 관리 콜백. */

		.of_match_table = of_match_ptr(omap_iommu_of_match),
		/* [한국어] 붙을 노드의 호환 문자열. of_match_ptr는
		 * CONFIG_OF가 꺼진 빌드에서 NULL로 접힌다. */
	},
};

/*
 * [한국어]
 * iotlb_init_entry - 매핑 정보 구조체를 채운다
 *
 * @e: 채울 구조체.
 * @da: 가상 주소.
 * @pa: 물리 주소.
 * @pgsz: 페이지 크기 코드.
 * @return: 그 크기의 바이트 수.
 *
 * 접근 속성은 고정값을 쓴다: 리틀엔디언, 8비트 요소, 혼합 모드
 * 없음. 이 드라이버는 그 세 가지를 조절할 수단을 상위에 노출하지
 * 않으며, 대부분의 클라이언트에게 이 조합이면 충분하다.
 *
 * 실행 컨텍스트: 매핑 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   omap_iommu_map() → [iotlb_init_entry]
 */
static u32 iotlb_init_entry(struct iotlb_entry *e, u32 da, u32 pa, int pgsz)
{
	/* [한국어] 지정하지 않는 필드(prsvd 등)를 0으로 만든다. */
	memset(e, 0, sizeof(*e));

	/* [한국어] 가상 주소. */
	e->da		= da;
	/* [한국어] 물리 주소. */
	e->pa		= pa;
	/* [한국어] 이 항목이 유효함을 표시한다. */
	e->valid	= MMU_CAM_V;
	/* [한국어] 네 크기 중 하나. */
	e->pgsz		= pgsz;
	/* [한국어] 접근 속성은 고정이다 — 상위에 조절 수단을 두지 않는다. */
	e->endian	= MMU_RAM_ENDIAN_LITTLE;
	e->elsz		= MMU_RAM_ELSZ_8;	/* [한국어] 요소 크기는 8비트로 고정한다. */
	e->mixed	= 0;

	/* [한국어] 크기 코드를 바이트 수로 바꿔 돌려준다. */
	return iopgsz_to_bytes(e->pgsz);
}

/*
 * [한국어]
 * omap_iommu_map - 모든 MMU 인스턴스에 같은 매핑을 심는다
 *
 * @domain: 대상 도메인.
 * @da: 가상 주소.
 * @pa: 물리 주소.
 * @bytes: 매핑 크기.
 * @count: 개수(이 드라이버는 쓰지 않는다).
 * @prot: 권한(이 드라이버는 쓰지 않는다).
 * @gfp: 할당 플래그.
 * @mapped: 매핑된 바이트 수를 돌려줄 곳.
 * @return: 0 성공, 음수 오류.
 *
 * **거울 프로그래밍**이 드러나는 첫 함수다. 클라이언트가 여러
 * MMU 뒤에 있으면 각 MMU에 **같은 엔트리**를 심는다. DMA가 어느
 * MMU를 지날지 알 수 없기 때문이다.
 *
 * 중간에 실패하면 이미 심은 것들을 역순으로 지운다 — 일부 MMU만
 * 매핑된 상태를 남기지 않기 위함이다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 map_pages → [omap_iommu_map]
 *   → omap_iopgtable_store_entry()
 */
static int omap_iommu_map(struct iommu_domain *domain, unsigned long da,
			  phys_addr_t pa, size_t bytes, size_t count,
			  int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 대상 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);
	/* [한국어] 로그용 클라이언트 디바이스. */
	struct device *dev = omap_domain->dev;
	/* [한국어] 순회 커서. */
	struct omap_iommu_device *iommu;
	/* [한국어] 현재 인스턴스. */
	struct omap_iommu *oiommu;
	/* [한국어] 모든 인스턴스에 공통으로 쓸 매핑 정보. */
	struct iotlb_entry e;
	/* [한국어] 결과 코드. */
	int ret = -EINVAL;
	/* [한국어] 크기를 하드웨어 코드로 옮긴 값. */
	int omap_pgsz;
	/* [한국어] 순회 인덱스. 되돌리기에도 쓰인다. */
	int i;

	/* [한국어] 지원하는 네 크기 중 하나여야 한다. */
	omap_pgsz = bytes_to_iopgsz(bytes);
	if (omap_pgsz < 0) {	/* [한국어] 지원하지 않는 크기다. */
		dev_err(dev, "invalid size to map: %zu\n", bytes);	/* [한국어] 어떤 크기가 거부됐는지 남긴다. */
		return -EINVAL;	/* [한국어] 네 크기 중 하나여야 한다. */
	}

	dev_dbg(dev, "mapping da 0x%lx to pa %pa size 0x%zx\n", da, &pa, bytes);

	/* [한국어] 매핑 정보를 한 번만 만들어 모든 인스턴스에 재사용한다 —
	 * 내용이 같아야 거울 프로그래밍이 성립한다. */
	iotlb_init_entry(&e, da, pa, omap_pgsz);

	/* [한국어] 이 도메인의 모든 MMU에 같은 엔트리를 심는다. */
	iommu = omap_domain->iommus;
	for (i = 0; i < omap_domain->num_iommus; i++, iommu++) {	/* [한국어] 이 도메인의 모든 MMU를 훑는다. */
		oiommu = iommu->iommu_dev;	/* [한국어] 이번에 심을 인스턴스. */
		ret = omap_iopgtable_store_entry(oiommu, &e);	/* [한국어] 같은 엔트리를 이 인스턴스에도 심는다. */
		if (ret) {	/* [한국어] 심기에 실패한 경우. */
			dev_err(dev, "omap_iopgtable_store_entry failed: %d\n",	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
				ret);
			break;
		}
	}

	/* [한국어] 중간에 실패했다 — 이미 심은 것들을 역순으로 지워
	 * 일부만 매핑된 상태를 남기지 않는다. */
	if (ret) {
		while (i--) {	/* [한국어] 이미 심은 인스턴스들을 역순으로 되돌린다. */
			iommu--;	/* [한국어] 커서를 하나 뒤로 옮긴다. */
			oiommu = iommu->iommu_dev;	/* [한국어] 되돌릴 인스턴스. */
			iopgtable_clear_entry(oiommu, da);	/* [한국어] 그 인스턴스에서 엔트리를 지운다. */
		}
	} else {
		/* [한국어] 모든 인스턴스에 성공했으니 요청 전량이 매핑됐다. */
		*mapped = bytes;
	}

	return ret;	/* [한국어] 매핑 결과를 코어에 전한다. */
}

/*
 * [한국어]
 * omap_iommu_unmap - 모든 MMU 인스턴스에서 매핑을 지운다
 *
 * @domain: 대상 도메인.
 * @da: 지울 가상 주소.
 * @size: 요청 크기(실제 크기는 엔트리가 정한다).
 * @count: 개수(쓰지 않는다).
 * @gather: 무효화 모으기(이 드라이버는 즉시 무효화한다).
 * @return: 해제한 바이트 수, 하나라도 실패하면 0.
 *
 * map과 대칭으로 모든 인스턴스에서 지운다. 반환값은 마지막
 * 인스턴스의 것을 쓰는데, 원본 주석이 밝히듯 거울 프로그래밍
 * 덕분에 모두 같은 값이 나온다고 전제하기 때문이다.
 *
 * 하나라도 실패하면 전체를 실패로 보고하지만, **되돌리지는
 * 않는다.** 이미 지운 것을 되살릴 방법이 없고, 지워진 상태는
 * 안전한 방향이기 때문이다.
 *
 * 실행 컨텍스트: 해제 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 unmap_pages → [omap_iommu_unmap]
 *   → iopgtable_clear_entry()
 */
static size_t omap_iommu_unmap(struct iommu_domain *domain, unsigned long da,
			       size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);
	/* [한국어] 로그용 클라이언트 디바이스. */
	struct device *dev = omap_domain->dev;
	/* [한국어] 순회 커서. */
	struct omap_iommu_device *iommu;
	/* [한국어] 현재 인스턴스. */
	struct omap_iommu *oiommu;
	/* [한국어] 하나라도 실패했는가. */
	bool error = false;
	/* [한국어] 마지막 인스턴스가 보고한 해제 크기. */
	size_t bytes = 0;
	/* [한국어] 순회 인덱스. */
	int i;

	dev_dbg(dev, "unmapping da 0x%lx size %zu\n", da, size);

	/* [한국어] 모든 인스턴스에서 지운다. 실패해도 끝까지 진행한다 —
	 * 일부만 지워진 상태를 남기지 않기 위함이다. */
	iommu = omap_domain->iommus;
	for (i = 0; i < omap_domain->num_iommus; i++, iommu++) {	/* [한국어] 이 도메인의 모든 MMU를 훑는다. */
		oiommu = iommu->iommu_dev;	/* [한국어] 이번에 지울 인스턴스. */
		bytes = iopgtable_clear_entry(oiommu, da);
		/* [한국어] 0이면 그 인스턴스에는 매핑이 없었다는 뜻이다. */
		if (!bytes)
			error = true;
	}

	/*
	 * simplify return - we are only checking if any of the iommus
	 * reported an error, but not if all of them are unmapping the
	 * same number of entries. This should not occur due to the
	 * mirror programming.
	 */
	/* [한국어] 거울 프로그래밍 덕분에 모든 인스턴스가 같은 값을
	 * 보고한다고 전제하고, 마지막 값을 대표로 쓴다. */
	return error ? 0 : bytes;
}

/*
 * [한국어]
 * omap_iommu_count - 이 디바이스에 딸린 MMU의 개수를 센다
 *
 * @dev: 클라이언트 디바이스.
 * @return: MMU 개수.
 *
 * arch_data가 NULL로 끝나는 배열이라, 끝을 만날 때까지 센다.
 * 이 파일 곳곳에 반복되는 관용구다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   omap_iommu_attach_init() → [omap_iommu_count]
 */
static int omap_iommu_count(struct device *dev)
{
	/* [한국어] NULL로 끝나는 MMU 배열. */
	struct omap_iommu_arch_data *arch_data = dev_iommu_priv_get(dev);
	/* [한국어] 센 개수. */
	int count = 0;

	/* [한국어] NULL 항목이 배열의 끝이다. */
	while (arch_data->iommu_dev) {
		count++;	/* [한국어] MMU를 하나 세었다. */
		arch_data++;	/* [한국어] 다음 항목으로 넘어간다. */
	}

	return count;	/* [한국어] 센 개수를 돌려준다. */
}

/* caller should call cleanup if this function fails */
/*
 * [한국어]
 * omap_iommu_attach_init - 도메인에 MMU마다의 테이블을 마련한다
 *
 * @dev: 클라이언트 디바이스.
 * @odomain: 채울 도메인.
 * @return: 0 성공, -ENODEV/-ENOMEM/-EINVAL.
 *
 * **인스턴스마다 별도의 1단계 테이블을 잡는다.** 내용은 같겠지만
 * 물리적으로 다른 테이블인데, 각 MMU가 자기 TTB 레지스터에
 * 서로 다른 주소를 담아야 하기 때문이다.
 *
 * kzalloc으로 16KB를 잡으면 자연히 16KB 정렬이 되지만, 하드웨어가
 * 그것을 요구하므로 WARN_ON으로 확인해 둔다 — 원본 주석이
 * "절대 실패하지 않겠지만 남겨 둔다"고 밝히는 부분이다.
 *
 * 실패 시 정리는 호출자가 detach_fini로 한다 — 주석이 그 계약을
 * 명시한다.
 *
 * 실행 컨텍스트: attach 경로. 도메인 락을 잡은 상태라 GFP_ATOMIC.
 *
 * 호출 체인:
 *   omap_iommu_attach_dev() → [omap_iommu_attach_init]
 */
static int omap_iommu_attach_init(struct device *dev,
				  struct omap_iommu_domain *odomain)
{
	/* [한국어] 순회 커서. */
	struct omap_iommu_device *iommu;
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 이 디바이스에 딸린 MMU의 수를 센다. */
	odomain->num_iommus = omap_iommu_count(dev);
	if (!odomain->num_iommus)	/* [한국어] MMU가 하나도 없는 디바이스다. */
		return -ENODEV;

	/* [한국어] 인스턴스별 상태 배열. 도메인 락을 쥐고 있어 ATOMIC이다. */
	odomain->iommus = kzalloc_objs(*iommu, odomain->num_iommus, GFP_ATOMIC);
	if (!odomain->iommus)	/* [한국어] 인스턴스 상태 배열을 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 인스턴스마다 자기 1단계 테이블을 잡는다 — 내용은
	 * 같아지겠지만 각 MMU가 자기 TTB에 담을 주소가 필요하다. */
	iommu = odomain->iommus;
	for (i = 0; i < odomain->num_iommus; i++, iommu++) {	/* [한국어] 인스턴스마다 자기 테이블을 잡는다. */
		iommu->pgtable = kzalloc(IOPGD_TABLE_SIZE, GFP_ATOMIC);	/* [한국어] 16KB 1단계 테이블. 도메인 락 아래라 ATOMIC이다. */
		if (!iommu->pgtable)	/* [한국어] 테이블을 잡지 못했다. */
			return -ENOMEM;

		/*
		 * should never fail, but please keep this around to ensure
		 * we keep the hardware happy
		 */
		/* [한국어] 16KB 할당은 자연히 16KB 정렬이지만, 하드웨어가
		 * 그것을 요구하므로 확인해 둔다. */
		if (WARN_ON(!IS_ALIGNED((long)iommu->pgtable,
					IOPGD_TABLE_SIZE)))
			return -EINVAL;
	}

	return 0;	/* [한국어] 모든 인스턴스의 테이블이 준비됐다. */
}

/*
 * [한국어]
 * omap_iommu_detach_fini - attach_init이 잡은 것을 모두 반납한다
 *
 * @odomain: 대상 도메인.
 * @return: 없음.
 *
 * attach_init이 중간에 실패한 경우에도 불리므로, 부분적으로만
 * 채워진 배열을 안전하게 다뤄야 한다. kfree(NULL)이 무해하다는
 * 점과 iommus가 NULL일 수 있다는 점이 그 안전을 만든다.
 *
 * 실행 컨텍스트: detach 경로와 attach 실패 경로.
 *
 * 호출 체인:
 *   omap_iommu_attach_dev()의 오류 경로 / _omap_iommu_detach_dev()
 *   → [omap_iommu_detach_fini]
 */
static void omap_iommu_detach_fini(struct omap_iommu_domain *odomain)
{
	/* [한국어] 순회 인덱스. */
	int i;
	/* [한국어] 순회 커서. NULL일 수 있다. */
	struct omap_iommu_device *iommu = odomain->iommus;

	/* [한국어] 배열이 있을 때만 훑는다. 아직 안 잡힌 테이블은
	 * NULL이라 kfree가 무해하게 넘어간다. */
	for (i = 0; iommu && i < odomain->num_iommus; i++, iommu++)
		kfree(iommu->pgtable);

	/* [한국어] 배열 자체를 반납하고 장부를 비운다. */
	kfree(odomain->iommus);
	odomain->num_iommus = 0;	/* [한국어] 개수 장부를 비운다. */
	odomain->iommus = NULL;	/* [한국어] 배열 포인터도 비워 두 번 해제되지 않게 한다. */
}

/*
 * [한국어]
 * omap_iommu_attach_dev - 클라이언트를 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 클라이언트 디바이스.
 * @old: 직전 도메인(이 드라이버는 쓰지 않는다).
 * @return: 0 성공, 음수 오류.
 *
 * **한 도메인에 클라이언트는 하나뿐**임을 명시적으로 강제한다.
 * 대신 그 하나의 클라이언트가 여러 MMU를 거느릴 수 있고,
 * 그 MMU들에 각각 테이블을 만들어 붙이는 것이 이 함수의 일이다.
 *
 * 두 개의 커서(iommu와 arch_data)가 나란히 전진하는 것에 유의 —
 * 앞의 것은 도메인 쪽 상태, 뒤의 것은 디바이스 쪽 MMU 목록이라
 * 서로 대응한다.
 *
 * 오류 경로가 두 단계다. 하드웨어를 붙이다 실패하면 이미 붙인
 * 것들을 역순으로 떼고(attach_fail), 이어서 잡아 둔 테이블까지
 * 반납한다(init_fail). 역순인 이유는 인스턴스 사이의 하드웨어
 * 의존을 지키기 위함이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트지만 도메인 락(스핀락)을
 * 잡은 채로 진행하므로 그 안의 할당이 ATOMIC이다.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [omap_iommu_attach_dev]
 *   → omap_iommu_attach_init() → omap_iommu_attach()
 */
static int omap_iommu_attach_dev(struct iommu_domain *domain,
				 struct device *dev, struct iommu_domain *old)
{
	/* [한국어] 이 디바이스에 딸린 MMU 목록. */
	struct omap_iommu_arch_data *arch_data = dev_iommu_priv_get(dev);
	/* [한국어] 붙일 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);
	/* [한국어] 도메인 쪽 인스턴스 상태 커서. */
	struct omap_iommu_device *iommu;
	/* [한국어] 현재 MMU 인스턴스. */
	struct omap_iommu *oiommu;
	/* [한국어] 결과 코드. */
	int ret = 0;
	/* [한국어] 순회 인덱스. 되돌리기에도 쓰인다. */
	int i;

	/* [한국어] MMU가 없는 디바이스는 붙일 수 없다. */
	if (!arch_data || !arch_data->iommu_dev) {
		dev_err(dev, "device doesn't have an associated iommu\n");	/* [한국어] MMU가 없는 디바이스는 붙일 수 없다. */
		return -ENODEV;	/* [한국어] 담당할 하드웨어가 없다. */
	}

	/* [한국어] 도메인 상태를 바꾸는 전 구간을 잠근다. */
	spin_lock(&omap_domain->lock);

	/* only a single client device can be attached to a domain */
	/* [한국어] 이 드라이버는 도메인당 클라이언트 하나만 지원한다.
	 * 여러 클라이언트가 같은 테이블을 공유하는 구조를 만들지 않았다. */
	if (omap_domain->dev) {
		dev_err(dev, "iommu domain is already attached\n");	/* [한국어] 이미 다른 클라이언트가 붙어 있다. */
		ret = -EINVAL;	/* [한국어] 도메인당 하나뿐이라는 제약을 알린다. */
		goto out;	/* [한국어] 락을 풀고 나간다. */
	}

	/* [한국어] MMU마다의 테이블과 상태 배열을 마련한다. */
	ret = omap_iommu_attach_init(dev, omap_domain);
	if (ret) {	/* [한국어] 테이블 마련이 실패한 경우. */
		dev_err(dev, "failed to allocate required iommu data %d\n",	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
			ret);
		goto init_fail;	/* [한국어] 잡아 둔 것을 반납하러 간다. */
	}

	/* [한국어] 두 커서가 나란히 전진한다 — 도메인 쪽 상태와
	 * 디바이스 쪽 MMU 목록이 서로 대응한다. */
	iommu = omap_domain->iommus;
	for (i = 0; i < omap_domain->num_iommus; i++, iommu++, arch_data++) {	/* [한국어] 도메인 쪽 상태와 디바이스 쪽 목록을 나란히 훑는다. */
		/* configure and enable the omap iommu */
		oiommu = arch_data->iommu_dev;
		/* [한국어] 이 인스턴스에 자기 테이블을 걸고 전원을 켠다. */
		ret = omap_iommu_attach(oiommu, iommu->pgtable);
		if (ret) {	/* [한국어] 하드웨어 붙이기가 실패한 경우. */
			dev_err(dev, "can't get omap iommu: %d\n", ret);	/* [한국어] 어느 인스턴스에서 막혔는지 남긴다. */
			goto attach_fail;	/* [한국어] 이미 붙인 것을 되돌리러 간다. */
		}

		/* [한국어] 인터럽트 핸들러가 이 값으로 도메인을 찾는다. */
		oiommu->domain = domain;
		/* [한국어] 도메인 쪽에서도 인스턴스를 기억한다 —
		 * map/unmap이 이 배열을 훑는다. */
		iommu->iommu_dev = oiommu;
	}

	/* [한국어] 이 값이 설정되어야 폴트 핸들러와 활성화 API가
	 * 도메인을 유효하다고 본다. */
	omap_domain->dev = dev;

	goto out;

/* [한국어] 하드웨어를 붙이다 실패했다 — 이미 붙인 것을 역순으로 뗀다. */
attach_fail:
	while (i--) {	/* [한국어] 이미 붙인 인스턴스들을 역순으로 훑는다. */
		iommu--;	/* [한국어] 도메인 쪽 커서를 하나 뒤로 옮긴다. */
		arch_data--;	/* [한국어] 디바이스 쪽 커서도 함께 옮긴다. */
		oiommu = iommu->iommu_dev;
		/* [한국어] 역순인 이유: 인스턴스 사이에 하드웨어 의존이
		 * 있을 수 있어 켠 순서의 반대로 꺼야 한다. */
		omap_iommu_detach(oiommu);
		iommu->iommu_dev = NULL;	/* [한국어] 도메인 쪽 연결을 끊는다. */
		oiommu->domain = NULL;	/* [한국어] 인스턴스 쪽 연결도 끊는다. */
	}
/* [한국어] 테이블 마련에 실패했거나 위에서 내려온 경우 — 잡아 둔
 * 테이블과 배열을 반납한다. */
init_fail:
	omap_iommu_detach_fini(omap_domain);
/* [한국어] 성공/실패 공통 정리 지점. */
out:
	spin_unlock(&omap_domain->lock);	/* [한국어] 성공이든 실패든 락을 놓는다. */
	return ret;	/* [한국어] 붙이기 결과를 코어에 전한다. */
}

/*
 * [한국어]
 * _omap_iommu_detach_dev - 클라이언트를 도메인에서 뗀다(락은 호출자가 잡는다)
 *
 * @omap_domain: 대상 도메인.
 * @dev: 뗄 디바이스.
 * @return: 없음.
 *
 * attach의 역순이며, **역순 순회**가 다시 등장한다. 원본 주석이
 * 밝히듯 인스턴스 사이의 하드웨어 의존을 지키기 위함이다.
 *
 * 각 인스턴스에서 테이블 전체를 비운 뒤 떼어 내는 순서에 유의.
 * 반대로 하면 이미 꺼진 MMU의 TLB를 비우려 하게 된다.
 *
 * 실행 컨텍스트: 도메인 락을 잡은 상태.
 *
 * 호출 체인:
 *   omap_iommu_identity_attach() / omap_iommu_domain_free()
 *   → [_omap_iommu_detach_dev] → iopgtable_clear_entry_all()
 */
static void _omap_iommu_detach_dev(struct omap_iommu_domain *omap_domain,
				   struct device *dev)
{
	/* [한국어] 이 디바이스에 딸린 MMU 목록. */
	struct omap_iommu_arch_data *arch_data = dev_iommu_priv_get(dev);
	/* [한국어] 도메인 쪽 인스턴스 상태 커서. */
	struct omap_iommu_device *iommu = omap_domain->iommus;
	/* [한국어] 현재 MMU 인스턴스. */
	struct omap_iommu *oiommu;
	/* [한국어] 순회 인덱스. */
	int i;

	/* [한국어] 붙어 있는 클라이언트가 없다. */
	if (!omap_domain->dev) {
		dev_err(dev, "domain has no attached device\n");	/* [한국어] 붙어 있는 클라이언트가 없다. */
		return;
	}

	/* only a single device is supported per domain for now */
	/* [한국어] 도메인에 붙어 있는 것과 다른 디바이스를 떼려 한다 —
	 * 도메인당 하나뿐이므로 있을 수 없는 요청이다. */
	if (omap_domain->dev != dev) {
		dev_err(dev, "invalid attached device\n");	/* [한국어] 도메인당 하나뿐이므로 있을 수 없는 요청이다. */
		return;
	}

	/*
	 * cleanup in the reverse order of attachment - this addresses
	 * any h/w dependencies between multiple instances, if any
	 */
	/* [한국어] 두 커서를 모두 마지막 항목으로 옮겨 역순으로 훑는다. */
	iommu += (omap_domain->num_iommus - 1);
	arch_data += (omap_domain->num_iommus - 1);	/* [한국어] 디바이스 쪽 커서도 마지막으로 옮긴다. */
	for (i = 0; i < omap_domain->num_iommus; i++, iommu--, arch_data--) {	/* [한국어] 켠 순서의 반대로 훑는다. */
		oiommu = iommu->iommu_dev;
		/* [한국어] 테이블과 TLB를 먼저 비운다 — 떼어 낸 뒤에는
		 * 꺼진 MMU를 만지게 된다. */
		iopgtable_clear_entry_all(oiommu);

		/* [한국어] 테이블을 떼고 전원을 놓는다. */
		omap_iommu_detach(oiommu);
		iommu->iommu_dev = NULL;	/* [한국어] 도메인 쪽 연결을 끊는다. */
		oiommu->domain = NULL;	/* [한국어] 인스턴스 쪽 연결도 끊는다. */
	}

	/* [한국어] 잡아 둔 테이블과 배열을 반납한다. */
	omap_iommu_detach_fini(omap_domain);

	/* [한국어] 이 값을 지워야 폴트 핸들러가 이 도메인을 무시한다. */
	omap_domain->dev = NULL;
}

/*
 * [한국어]
 * omap_iommu_identity_attach - 클라이언트를 도메인에서 떼어 낸다
 *
 * @identity_domain: 정적 identity 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인.
 * @return: 항상 0.
 *
 * 이 하드웨어에 통과 모드가 없으므로, identity에 붙는다는 것은
 * **MMU를 꺼 버리는 일**이다. 그래서 이 함수가 곧 detach 구현이다.
 *
 * 다른 드라이버들과 달리 old 인자를 실제로 쓴다 — 그것이
 * 떼어 낼 도메인이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) → [omap_iommu_identity_attach]
 *   → _omap_iommu_detach_dev()
 */
static int omap_iommu_identity_attach(struct iommu_domain *identity_domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	/* [한국어] 떼어 낼 도메인. */
	struct omap_iommu_domain *omap_domain;

	/* [한국어] 이미 떼어진 상태거나 붙은 적이 없으면 할 일이 없다. */
	if (old == identity_domain || !old)
		return 0;

	/* [한국어] old가 곧 떼어 낼 도메인이다. */
	omap_domain = to_omap_domain(old);
	spin_lock(&omap_domain->lock);	/* [한국어] 도메인 상태를 바꾸는 구간을 잠근다. */
	_omap_iommu_detach_dev(omap_domain, dev);	/* [한국어] 실제 떼어 내기를 위임한다. */
	spin_unlock(&omap_domain->lock);	/* [한국어] 락을 놓는다. */
	return 0;	/* [한국어] 떼어 내기는 실패하지 않는다. */
}

/* [한국어] identity 도메인의 연산 테이블. 붙이기 하나뿐이며,
 * 그 붙이기가 사실은 "MMU 끄기"다. */
static struct iommu_domain_ops omap_iommu_identity_ops = {
	.attach_dev = omap_iommu_identity_attach,
	/* [한국어] 이 도메인으로 옮길 때 부를 콜백. */
};

/* [한국어] "어느 도메인에도 붙어 있지 않음"을 나타내는 전역 도메인. */
static struct iommu_domain omap_iommu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 코어에 통과 모드로 알리는 종류 표시. */

	.ops = &omap_iommu_identity_ops,
	/* [한국어] 위의 연산 테이블. */
};

/*
 * [한국어]
 * omap_iommu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 디바이스(이 드라이버는 쓰지 않는다).
 * @return: 새 도메인, 실패하면 NULL.
 *
 * 껍데기만 만든다는 것이 요점이다. 페이지 테이블은 attach 시점에
 * MMU 개수를 알게 된 뒤에야 만들 수 있다 — 인스턴스마다 하나씩
 * 필요하기 때문이다.
 *
 * aperture가 32비트 전체인 것은 1단계 테이블 4096개 × 1MB가
 * 정확히 4GB이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [omap_iommu_domain_alloc_paging]
 */
static struct iommu_domain *omap_iommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 만들 도메인. */
	struct omap_iommu_domain *omap_domain;

	/* [한국어] 0으로 초기화해 받는다 — dev가 NULL이어야
	 * attach가 "아직 비어 있다"고 판단한다. */
	omap_domain = kzalloc_obj(*omap_domain);
	if (!omap_domain)	/* [한국어] 도메인 구조체를 잡지 못했다. */
		return NULL;

	/* [한국어] 도메인 상태를 보호할 락. */
	spin_lock_init(&omap_domain->lock);

	/* [한국어] 네 가지 매핑 크기를 모두 지원한다고 알린다. */
	omap_domain->domain.pgsize_bitmap = OMAP_IOMMU_PGSIZES;

	/* [한국어] IOVA 공간은 0부터 시작한다. */
	omap_domain->domain.geometry.aperture_start = 0;
	/* [한국어] 1단계 4096개 × 1MB = 4GB로 32비트 전체를 덮는다. */
	omap_domain->domain.geometry.aperture_end   = (1ULL << 32) - 1;
	/* [한국어] 코어가 그 범위를 벗어난 IOVA를 주지 않게 강제한다. */
	omap_domain->domain.geometry.force_aperture = true;

	/* [한국어] 페이지 테이블은 attach에서 만든다 — 그때야
	 * MMU 개수를 알기 때문이다. */
	return &omap_domain->domain;
}

/*
 * [한국어]
 * omap_iommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 아직 클라이언트가 붙어 있으면 강제로 떼어 낸다. 정상 경로에서는
 * 코어가 먼저 detach하지만, 그것이 보장되지 않는 경로가 있어
 * 방어해 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 free → [omap_iommu_domain_free]
 */
static void omap_iommu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 해제할 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);

	/*
	 * An iommu device is still attached
	 * (currently, only one device can be attached) ?
	 */
	/* [한국어] 아직 붙어 있으면 강제로 뗀다 — 해제된 테이블을
	 * 가리키는 하드웨어를 남기지 않기 위함이다.
	 * 락 없이 부르는데, 해제 시점에는 경쟁할 상대가 없다는 전제다. */
	if (omap_domain->dev)
		_omap_iommu_detach_dev(omap_domain, omap_domain->dev);

	kfree(omap_domain);	/* [한국어] 도메인 구조체를 해제한다. */
}

/*
 * [한국어]
 * omap_iommu_iova_to_phys - IOVA를 물리 주소로 변환한다
 *
 * @domain: 대상 도메인.
 * @da: 변환할 가상 주소.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * **첫 인스턴스만 본다**는 것이 요점이다. 거울 프로그래밍 덕분에
 * 모든 인스턴스의 테이블이 같으므로, 하나만 확인하면 충분하다 —
 * 원본 주석이 그것을 명시한다.
 *
 * 네 가지 매핑 크기를 모두 다뤄야 해서 분기가 넷이다. pte가
 * 있으면 2단계 매핑(4KB/64KB), 없으면 1단계 매핑(1MB/16MB)이다.
 *
 * 실행 컨텍스트: 조회 경로. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   IOMMU 코어 iova_to_phys → [omap_iommu_iova_to_phys]
 *   → iopgtable_lookup_entry()
 */
static phys_addr_t omap_iommu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t da)
{
	/* [한국어] 대상 도메인. */
	struct omap_iommu_domain *omap_domain = to_omap_domain(domain);
	/* [한국어] 첫 인스턴스의 상태. */
	struct omap_iommu_device *iommu = omap_domain->iommus;
	/* [한국어] 그 MMU 인스턴스. */
	struct omap_iommu *oiommu = iommu->iommu_dev;
	/* [한국어] 로그용 디바이스. */
	struct device *dev = oiommu->dev;
	/* [한국어] 찾은 엔트리들. */
	u32 *pgd, *pte;
	/* [한국어] 결과. 0이 "매핑 없음"을 뜻한다. */
	phys_addr_t ret = 0;

	/*
	 * all the iommus within the domain will have identical programming,
	 * so perform the lookup using just the first iommu
	 */
	/* [한국어] 거울 프로그래밍 덕분에 첫 인스턴스만 보면 된다. */
	iopgtable_lookup_entry(oiommu, da, &pgd, &pte);

	/* [한국어] 2단계 엔트리가 있으면 4KB 또는 64KB 매핑이다. */
	if (pte) {
		if (iopte_is_small(*pte))	/* [한국어] 4KB 작은 페이지 매핑. */
			ret = omap_iommu_translate(*pte, da, IOPTE_MASK);
		else if (iopte_is_large(*pte))	/* [한국어] 64KB 큰 페이지 매핑. */
			ret = omap_iommu_translate(*pte, da, IOLARGE_MASK);
		else
			/* [한국어] 2단계 테이블은 있는데 엔트리 타입을
			 * 알 수 없다 — 손상된 테이블이다. */
			dev_err(dev, "bogus pte 0x%x, da 0x%llx", *pte,
				(unsigned long long)da);
	} else {
		/* [한국어] 1단계에서 끝나는 매핑 — 1MB 또는 16MB다. */
		if (iopgd_is_section(*pgd))
			ret = omap_iommu_translate(*pgd, da, IOSECTION_MASK);
		else if (iopgd_is_super(*pgd))	/* [한국어] 16MB 슈퍼섹션 매핑. */
			ret = omap_iommu_translate(*pgd, da, IOSUPER_MASK);
		else
			/* [한국어] 매핑이 없거나 타입을 알 수 없다.
			 * 조회 함수가 빈 엔트리도 여기로 보내므로,
			 * 매핑 없음도 이 로그를 남긴다. */
			dev_err(dev, "bogus pgd 0x%x, da 0x%llx", *pgd,
				(unsigned long long)da);
	}

	return ret;	/* [한국어] 찾았으면 물리 주소, 못 찾았으면 0이다. */
}

/*
 * [한국어]
 * omap_iommu_probe_device - 디바이스에 딸린 MMU들을 모아 매단다
 *
 * @dev: 검사할 디바이스.
 * @return: 대표 iommu_device, 담당하지 않으면 ERR_PTR.
 *
 * of_xlate가 아무 일도 하지 않으므로, 디바이스 트리 파싱을
 * **여기서 직접 한다.** iommus 속성의 phandle들을 모두 따라가
 * 각 MMU의 드라이버 상태를 모은 배열을 만든다.
 *
 * OMAP은 #iommu-cells가 0이라 항목 하나가 phandle 하나뿐이며,
 * 그래서 phandle 크기로 개수를 셀 수 있다.
 *
 * 배열을 num_iommus + 1개로 잡는 것이 핵심이다. 마지막 항목이
 * 0으로 남아 NULL 종결자 노릇을 하고, 이 파일 곳곳의
 * "while (arch_data->iommu_dev)" 관용구가 그것에 기댄다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [omap_iommu_probe_device]
 */
static struct iommu_device *omap_iommu_probe_device(struct device *dev)
{
	/* [한국어] 만들 MMU 배열과 그것을 채우는 커서. */
	struct omap_iommu_arch_data *arch_data, *tmp;
	/* [한국어] phandle이 가리키는 MMU의 플랫폼 디바이스. */
	struct platform_device *pdev;
	/* [한국어] 그 MMU의 드라이버 상태. */
	struct omap_iommu *oiommu;
	/* [한국어] phandle이 가리키는 트리 노드. */
	struct device_node *np;
	/* [한국어] MMU 개수와 순회 인덱스. */
	int num_iommus, i;

	/*
	 * Allocate the per-device iommu structure for DT-based devices.
	 *
	 * TODO: Simplify this when removing non-DT support completely from the
	 * IOMMU users.
	 */
	/* [한국어] 디바이스 트리 기반만 지원한다. */
	if (!dev->of_node)
		return ERR_PTR(-ENODEV);

	/*
	 * retrieve the count of IOMMU nodes using phandle size as element size
	 * since #iommu-cells = 0 for OMAP
	 */
	/* [한국어] OMAP은 인자가 없는 참조라 항목 하나가 phandle
	 * 하나뿐이다 — 그래서 크기로 개수를 셀 수 있다. */
	num_iommus = of_property_count_elems_of_size(dev->of_node, "iommus",
						     sizeof(phandle));
	if (num_iommus < 0)	/* [한국어] iommus 속성이 없거나 형식이 맞지 않는다. */
		return ERR_PTR(-ENODEV);

	/* [한국어] 하나를 더 잡아 마지막 항목이 0으로 남게 한다 —
	 * 그것이 이 파일 곳곳의 순회가 기대는 NULL 종결자다. */
	arch_data = kzalloc_objs(*arch_data, num_iommus + 1);
	if (!arch_data)	/* [한국어] MMU 배열을 잡지 못했다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] 참조를 하나씩 따라가며 MMU 상태를 모은다. */
	for (i = 0, tmp = arch_data; i < num_iommus; i++, tmp++) {
		/* [한국어] i번째 phandle이 가리키는 노드. */
		np = of_parse_phandle(dev->of_node, "iommus", i);
		if (!np) {	/* [한국어] 참조가 가리키는 노드를 찾지 못했다. */
			kfree(arch_data);	/* [한국어] 지금까지 채운 배열을 반납한다. */
			return ERR_PTR(-EINVAL);	/* [한국어] 디바이스 트리가 잘못됐다. */
		}

		/* [한국어] 그 노드의 플랫폼 디바이스를 찾는다. */
		pdev = of_find_device_by_node(np);
		of_node_put(np);	/* [한국어] 참조를 따라간 뒤 노드 참조를 놓는다. */
		if (!pdev) {	/* [한국어] 그 노드의 플랫폼 디바이스가 아직 없다. */
			kfree(arch_data);	/* [한국어] 배열을 반납한다. */
			return ERR_PTR(-ENODEV);	/* [한국어] 코어가 나중에 다시 시도한다. */
		}

		/* [한국어] probe가 심어 둔 드라이버 상태를 꺼낸다.
		 * 아직 probe되지 않았으면 NULL이라 실패한다 —
		 * 코어가 나중에 다시 시도한다. */
		oiommu = platform_get_drvdata(pdev);
		put_device(&pdev->dev);	/* [한국어] 찾기가 올린 참조를 놓는다. */
		if (!oiommu) {	/* [한국어] 아직 probe되지 않아 상태가 비어 있다. */
			kfree(arch_data);	/* [한국어] 배열을 반납한다. */
			return ERR_PTR(-EINVAL);	/* [한국어] 코어가 나중에 다시 시도한다. */
		}

		tmp->iommu_dev = oiommu;	/* [한국어] 이 MMU를 배열에 담는다. */
	}

	/* [한국어] 이후 모든 콜백이 이 배열을 꺼내 쓴다. */
	dev_iommu_priv_set(dev, arch_data);

	/*
	 * use the first IOMMU alone for the sysfs device linking.
	 * TODO: Evaluate if a single iommu_group needs to be
	 * maintained for both IOMMUs
	 */
	/* [한국어] 코어에는 대표로 첫 MMU만 알린다. 나머지는 이
	 * 드라이버가 arch_data를 통해 직접 다룬다. */
	oiommu = arch_data->iommu_dev;

	return &oiommu->iommu;	/* [한국어] 대표 인스턴스를 코어에 알린다. */
}

/*
 * [한국어]
 * omap_iommu_release_device - 디바이스의 MMU 배열을 해제한다
 *
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * probe_device가 만든 배열을 반납한다. 하드웨어는 이미 코어가
 * detach로 정리해 두었다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 release_device → [omap_iommu_release_device]
 */
static void omap_iommu_release_device(struct device *dev)
{
	/* [한국어] probe_device가 만든 배열. */
	struct omap_iommu_arch_data *arch_data = dev_iommu_priv_get(dev);

	/* [한국어] 담당하지 않았던 디바이스면 해제할 것도 없다. */
	if (!dev->of_node || !arch_data)
		return;

	kfree(arch_data);	/* [한국어] MMU 배열을 반납한다. */

}

/*
 * [한국어]
 * omap_iommu_of_xlate - 디바이스 트리 참조를 처리한다(현재는 빈 구현)
 *
 * @dev: 대상 디바이스.
 * @args: 파싱된 참조.
 * @return: 항상 0.
 *
 * 이 콜백이 있어야 코어가 이 디바이스를 IOMMU 사용자로 인정하지만,
 * 실제 파싱은 probe_device가 다시 한다. 원본의 TODO가 밝히듯
 * 여기서 노드를 모아 두면 그 중복을 없앨 수 있다.
 *
 * 실행 컨텍스트: 디바이스 트리 파싱.
 *
 * 호출 체인:
 *   IOMMU 코어 of_xlate → [omap_iommu_of_xlate]
 */
static int omap_iommu_of_xlate(struct device *dev, const struct of_phandle_args *args)
{
	/* TODO: collect args->np to save re-parsing in probe above */
	/* [한국어] 존재 자체가 목적인 콜백이다 — 실제 파싱은
	 * probe_device가 다시 한다. */
	return 0;
}

/* [한국어] 이 드라이버가 IOMMU 코어에 제공하는 연산 테이블.
 * 무효화 콜백이 없는 것은 map/unmap 안에서 즉시 TLB를 비우기
 * 때문이다. */
static const struct iommu_ops omap_iommu_ops = {
	.identity_domain = &omap_iommu_identity_domain,
	/* [한국어] 통과 모드로 쓸 정적 도메인. 실제로는 MMU를
	 * 꺼 버리는 것이며, 이 드라이버의 detach 구현이기도 하다. */

	.domain_alloc_paging = omap_iommu_domain_alloc_paging,
	/* [한국어] 페이징 도메인 생성. 껍데기만 만들고 테이블은
	 * attach에서 만든다. */

	.probe_device	= omap_iommu_probe_device,
	/* [한국어] 디바이스 트리를 직접 파싱해 MMU 배열을 만든다. */

	.release_device	= omap_iommu_release_device,
	/* [한국어] 그 배열을 반납한다. */

	.device_group	= generic_single_device_group,
	/* [한국어] 격리 단위. 도메인당 클라이언트가 하나뿐이라
	 * 디바이스마다 독립 그룹이면 충분하다. */

	.of_xlate	= omap_iommu_of_xlate,
	/* [한국어] 존재가 목적인 빈 콜백. */

	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= omap_iommu_attach_dev,
		/* [한국어] 붙이기. MMU마다 테이블을 만들어 건다. */

		.map_pages	= omap_iommu_map,
		/* [한국어] 매핑. 모든 MMU에 같은 엔트리를 심는다. */

		.unmap_pages	= omap_iommu_unmap,
		/* [한국어] 해제. 모든 MMU에서 지운다. */

		.iova_to_phys	= omap_iommu_iova_to_phys,
		/* [한국어] 조회. 첫 MMU만 보면 충분하다. */

		.free		= omap_iommu_domain_free,
		/* [한국어] 도메인 해제. 붙어 있으면 강제로 뗀다. */
	}
};

/*
 * [한국어]
 * omap_iommu_init - 드라이버 전역 초기화
 *
 * @return: 0 성공, -ENOMEM.
 *
 * 2단계 테이블 캐시를 만든 뒤 플랫폼 드라이버를 등록한다.
 * 순서가 중요한데, 드라이버를 먼저 등록하면 probe가 즉시 불려
 * 아직 없는 캐시를 쓰려 할 수 있다.
 *
 * 정렬을 1KB로 지정하는 것이 핵심이다 — 1단계 엔트리에 2단계
 * 테이블 주소를 담을 때 하위 10비트가 타입과 속성 자리라,
 * 그만큼 정렬되어야 주소가 온전히 담긴다.
 *
 * subsys_initcall인 이유는 파일 맨 끝 주석이 밝힌다: OMAP3 ISP가
 * probe되기 전에 이 드라이버가 준비되어 있어야 한다.
 *
 * 실행 컨텍스트: subsys_initcall. 부팅 초기.
 *
 * 호출 체인:
 *   커널 initcall → [omap_iommu_init] → platform_driver_register()
 */
static int __init omap_iommu_init(void)
{
	/* [한국어] 만든 슬랩 캐시. */
	struct kmem_cache *p;
	/* [한국어] 캐시 라인 정렬을 요청하는 플래그. */
	const slab_flags_t flags = SLAB_HWCACHE_ALIGN;
	/* [한국어] 2단계 테이블에 요구되는 1KB 정렬 — 1단계 엔트리의
	 * 하위 10비트가 타입과 속성 자리이기 때문이다. */
	size_t align = 1 << 10; /* L2 pagetable alignement */
	/* [한국어] MMU 노드 존재 확인용. */
	struct device_node *np;
	/* [한국어] 드라이버 등록 결과. */
	int ret;

	/* [한국어] 이 시스템에 OMAP MMU가 있는지 본다. */
	np = of_find_matching_node(NULL, omap_iommu_of_match);
	/* [한국어] 없으면 이 SoC가 아니다 — 조용히 성공으로 돌아간다. */
	if (!np)
		return 0;

	/* [한국어] 찾기가 올린 참조를 놓는다. 존재 확인만이 목적이었다. */
	of_node_put(np);

	/* [한국어] 2단계 테이블 전용 캐시. 크기와 정렬이 핵심이다. */
	p = kmem_cache_create("iopte_cache", IOPTE_TABLE_SIZE, align, flags,
			      NULL);
	if (!p)	/* [한국어] 슬랩 캐시를 만들지 못했다. */
		return -ENOMEM;
	iopte_cachep = p;

	/* [한국어] 디버그 파일 시스템의 뿌리를 만든다. */
	omap_iommu_debugfs_init();

	/* [한국어] 준비가 끝났으니 드라이버를 등록한다 — 이 순간부터
	 * probe가 불리기 시작한다. */
	ret = platform_driver_register(&omap_iommu_driver);
	if (ret) {	/* [한국어] 드라이버 등록이 실패한 경우. */
		pr_err("%s: failed to register driver\n", __func__);	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
		goto fail_driver;	/* [한국어] 만들어 둔 캐시를 없애러 간다. */
	}

	return 0;

/* [한국어] 드라이버 등록 실패 — 만들어 둔 캐시를 없앤다. */
fail_driver:
	kmem_cache_destroy(iopte_cachep);	/* [한국어] 2단계 테이블 캐시를 없앤다. */
	return ret;	/* [한국어] 실패 이유를 커널에 전한다. */
}
/* [한국어] 아래 주석이 밝히듯 OMAP3 ISP보다 먼저 준비되어야 한다. */
subsys_initcall(omap_iommu_init);
/* must be ready before omap3isp is probed */
