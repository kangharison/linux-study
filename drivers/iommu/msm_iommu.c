// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * Author: Stepan Moskovchenko <stepanm@codeaurora.org>
 */

/*
 * [한국어 설명] Qualcomm MSM(APQ8064 세대) IOMMU 드라이버 (msm_iommu.c)
 *
 * === 파일의 역할 ===
 * Qualcomm의 옛 Snapdragon(APQ8064 등)에 들어 있는 "MSM IOMMU"를 리눅스
 * IOMMU 서브시스템에 붙이는 드라이버다. 이 하드웨어는 ARM의 SMMU가 표준화되기
 * 전에 만들어진 Qualcomm 자체 설계이지만, 내부적으로는 ARMv7 short-descriptor
 * 페이지 테이블(ARM_V7S)을 그대로 쓴다. 그래서 페이지 테이블 조작은
 * io-pgtable 프레임워크에 전부 위임하고, 이 파일은 하드웨어 레지스터
 * 프로그래밍과 TLB 무효화, 그리고 디바이스↔컨텍스트 뱅크 연결만 담당한다.
 *
 * 이 하드웨어의 구조를 이해하는 핵심 개념이 셋이다.
 *
 * (1) **컨텍스트 뱅크(context bank)**. IOMMU 하나에 여러 개의 독립적인 변환
 *     문맥이 있고, 각각 자기 TTBR과 TLB를 갖는다. 도메인에 디바이스를 붙일 때
 *     빈 뱅크를 하나 할당(msm_iommu_alloc_ctx)해 그 뱅크에 페이지 테이블을
 *     프로그래밍한다. 뱅크 개수는 디바이스 트리의 "qcom,ncb"가 알려 준다.
 *
 * (2) **MID(Master ID)와 그 매핑**. 버스에 나오는 트랜잭션에는 마스터를
 *     식별하는 MID가 실린다. M2VCBR 테이블이 "MID → 컨텍스트 뱅크"를
 *     매핑하며, config_mids()가 그 테이블을 채운다. 즉 디바이스 트리의
 *     iommus 프로퍼티에 적힌 값이 MID이고, 그것이 뱅크로 이어진다.
 *
 * (3) **클록 게이팅**. 레지스터에 접근하려면 그때마다 pclk(과 선택적으로
 *     iommu_clk)를 켜야 한다. 그래서 이 파일의 거의 모든 하드웨어 접근이
 *     __enable_clocks() / __disable_clocks() 쌍으로 감싸여 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [디바이스 드라이버] dma_map_*() / iommu_map()
 *        ↓
 *   [IOMMU 코어] iommu_ops 디스패치
 *        ↓
 *   [이 파일] msm_iommu_map() → io-pgtable(ARM_V7S)에 위임
 *        ↓                    → 완료 후 msm_iommu_sync_map()으로 TLB 무효화
 *   [io-pgtable-arm-v7s.c] 실제 페이지 테이블 조작
 *        ↓ tlb 콜백(msm_iommu_flush_ops)
 *   [이 파일] __flush_iotlb_range() → 각 컨텍스트 뱅크에 TLBIVA 기록
 *        ↓
 *   [MSM IOMMU 하드웨어]
 *
 * attach 흐름: msm_iommu_attach_dev()가 도메인용 io-pgtable을 만들고,
 * 이 디바이스와 of_node가 일치하는 IOMMU를 찾아 빈 컨텍스트 뱅크를 할당한 뒤
 * config_mids()로 MID를 그 뱅크에 연결하고 __program_context()로 TTBR과
 * 제어 비트를 써 넣는다.
 *
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트이며 전역 스핀락 msm_iommu_lock으로
 * 직렬화된다. map/unmap은 도메인별 pgtlock을 쓰고 atomic일 수 있어
 * io-pgtable에 GFP_ATOMIC을 넘긴다. 폴트 핸들러는 스레드 IRQ다.
 *
 * === 타 모듈과의 연결 ===
 * - msm_iommu_hw-8xxx.h: 이 파일이 쓰는 SET_ 계열과 GET_ 계열 레지스터 매크로
 *   전부가 거기 정의되어 있다. 이름이 곧 레지스터 이름이다.
 * - msm_iommu.h: struct msm_iommu_dev(IOMMU 인스턴스)와
 *   struct msm_iommu_ctx_dev(마스터=디바이스 하나의 MID 목록) 정의.
 * - linux/io-pgtable.h: alloc_io_pgtable_ops(ARM_V7S, ...)로 페이지 테이블
 *   백엔드를 얻는다. 이 드라이버가 테이블을 직접 만지지 않는 이유다.
 * - drivers/iommu/io-pgtable-arm-v7s.c: 실제 테이블 구현. 그쪽이 이 파일의
 *   msm_iommu_flush_ops 콜백을 통해 TLB 무효화를 요청한다.
 * 데이터 흐름: 디바이스 트리의 `iommus = <&iommu MID>` → of_xlate가
 * insert_iommu_master()로 MID를 마스터에 쌓음 → attach가 그 MID들을
 * 컨텍스트 뱅크에 연결 → 이후 그 MID로 오는 트랜잭션이 변환된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct msm_priv: 도메인 하나의 상태. io-pgtable 설정/핸들, 붙어 있는
 *   IOMMU들의 리스트, 페이지 테이블 락.
 * - msm_iommu_attach_dev(): 컨텍스트 뱅크를 할당하고 MID를 연결한 뒤
 *   페이지 테이블을 하드웨어에 프로그래밍한다.
 * - __program_context(): 뱅크 하나를 처음부터 끝까지 설정하는 함수.
 *   TEX remap, HTW(하드웨어 테이블 워크), TTBR, 폴트 처리 방식, MMU 활성화.
 * - __flush_iotlb_range(): io-pgtable이 부르는 TLB 무효화 콜백.
 *   ASID를 주소에 실어 TLBIVA에 쓰는 방식이 특징이다.
 * - msm_iommu_iova_to_phys(): 소프트웨어 워크가 아니라 하드웨어의
 *   V2P(가상→물리) 변환 기능을 써서 주소를 얻는다.
 * - msm_iommu_fault_handler(): 모든 뱅크의 FSR을 훑어 폴트를 보고하고 지운다.
 */

/* [한국어] 이 파일의 pr_* 출력 앞에 모듈 이름을 접두사로 붙인다.
 * 이 드라이버는 dev_err보다 pr_err를 많이 쓰므로(폴트 핸들러 등)
 * 접두사가 있어야 어느 서브시스템의 메시지인지 알 수 있다. */
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt
/* [한국어] 기본 커널 매크로와 타입. */
#include <linux/kernel.h>
/* [한국어] __init 등 초기화 섹션 매크로. */
#include <linux/init.h>
/* [한국어] platform_driver/platform_device — 이 IOMMU는 플랫폼 디바이스다. */
#include <linux/platform_device.h>
/* [한국어] -ENODEV, -ENOSPC 등 오류 코드. */
#include <linux/errno.h>
/* [한국어] devm_ioremap_resource()와 MMIO 접근 기반. */
#include <linux/io.h>
/* [한국어] io-pgtable 프레임워크 — alloc_io_pgtable_ops(ARM_V7S, ...)로
 * 페이지 테이블 백엔드를 빌려 쓴다. 이 드라이버의 핵심 의존성이다. */
#include <linux/io-pgtable.h>
/* [한국어] devm_request_threaded_irq()와 irqreturn_t. */
#include <linux/interrupt.h>
/* [한국어] 리스트 매크로 — IOMMU 목록, 컨텍스트 목록, 도메인별 IOMMU 목록에
 * 모두 쓰인다. 이 드라이버는 리스트 위주로 짜여 있다. */
#include <linux/list.h>
/* [한국어] spinlock_t — 전역 락과 도메인별 페이지 테이블 락. */
#include <linux/spinlock.h>
/* [한국어] kzalloc_obj()/kfree()/devm_kzalloc(). */
#include <linux/slab.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, iommu_domain 등. */
#include <linux/iommu.h>
/* [한국어] clk_enable()/clk_disable() — 레지스터 접근 전후로 반드시 호출한다. */
#include <linux/clk.h>
/* [한국어] IS_ERR/PTR_ERR/IS_ERR_VALUE 등 오류 포인터 헬퍼. */
#include <linux/err.h>

/* [한국어] 캐시 관리 함수들. 현재 코드에서 직접 호출하지는 않지만,
 * 페이지 테이블을 직접 다루던 시절의 흔적으로 남아 있다. */
#include <asm/cacheflush.h>
/* [한국어] SZ_4K, SZ_64K, SZ_1M, SZ_16M — 지원 페이지 크기 상수. */
#include <linux/sizes.h>

/* [한국어] 이 하드웨어의 레지스터 접근 매크로 모음. SET_TTBR0(),
 * GET_FSR() 같은 이름이 전부 거기서 온다. 오프셋 계산과 컨텍스트 뱅크
 * 인덱싱을 매크로가 숨겨 준다. */
#include "msm_iommu_hw-8xxx.h"
/* [한국어] 이 드라이버의 자료구조 정의 — struct msm_iommu_dev와
 * struct msm_iommu_ctx_dev. */
#include "msm_iommu.h"

/* [한국어] ARM 코프로세서 레지스터를 읽는 인라인 어셈블리 매크로(MRC 명령).
 * 현재 코드에서는 쓰이지 않지만, 과거 CPU의 캐시/MMU 설정을 읽어
 * IOMMU 설정에 반영하던 코드의 잔재다. 매크로 정의만 남아 있다. */
#define MRC(reg, processor, op1, crn, crm, op2)				\
__asm__ __volatile__ (							\
"   mrc   "   #processor "," #op1 ", %0,"  #crn "," #crm "," #op2 "\n"  \
: "=r" (reg))	/* [한국어] MRC 매크로의 마지막 줄 — 읽은 값을 reg 변수로 내보내는 출력 제약이다. */

/* bitmap of the page sizes currently supported */
/* [한국어] 이 드라이버가 지원하는 페이지 크기들의 비트맵.
 * ARMv7 short-descriptor 형식이 제공하는 네 가지 크기 그대로다 —
 * 4KB(작은 페이지), 64KB(큰 페이지), 1MB(섹션), 16MB(슈퍼섹션).
 * IOMMU 코어가 이 비트맵을 보고 매핑 요청을 가장 큰 크기부터 쪼갠다. */
#define MSM_IOMMU_PGSIZES	(SZ_4K | SZ_64K | SZ_1M | SZ_16M)

/* [한국어] 이 드라이버 전체를 직렬화하는 전역 스핀락.
 * 보호 대상: qcom_iommu_devices 리스트, 각 IOMMU의 ctx_list,
 *            컨텍스트 뱅크 할당 비트맵, 그리고 하드웨어 레지스터 접근.
 * 왜 전역인가: 이 드라이버는 IOMMU 인스턴스가 몇 개 되지 않는 옛 SoC용이라
 *              세밀한 락을 둘 이유가 없었고, attach 경로가 여러 IOMMU를
 *              한꺼번에 순회하므로 전역 락이 오히려 단순하다.
 * 획득 방식: 폴트 핸들러(스레드 IRQ)도 잡으므로 대부분 irqsave를 쓴다. */
static DEFINE_SPINLOCK(msm_iommu_lock);
/* [한국어] 시스템에 존재하는 모든 MSM IOMMU 인스턴스의 리스트.
 * 설정자: msm_iommu_probe()가 각 인스턴스를 여기 추가한다.
 * 읽는 자: find_iommu_for_dev(), qcom_iommu_of_xlate(), attach_dev()가
 *          "이 디바이스를 담당하는 IOMMU가 어느 것인가"를 찾을 때 순회한다.
 * 동기화: msm_iommu_lock 아래에서만 다룬다. */
static LIST_HEAD(qcom_iommu_devices);
/* [한국어] iommu_ops의 전방 선언. 정의는 파일 아래쪽에 있고, probe에서
 * iommu_device_register()에 넘기기 위해 미리 선언해 둔다. */
static struct iommu_ops msm_iommu_ops;

/* [한국어] IOMMU 도메인 하나의 상태.
 * 수명: domain_alloc_paging에서 kzalloc되고 domain_free에서 해제된다.
 * 이 드라이버는 페이지 테이블을 직접 관리하지 않고 io-pgtable에 맡기므로,
 * 여기 담기는 것은 그 백엔드 핸들과 어느 IOMMU들에 붙어 있는지의 목록이다. */
struct msm_priv {
	struct list_head list_attached;
	/* [한국어] 이 도메인이 붙어 있는 msm_iommu_dev들의 리스트 헤드.
	 * 설정자: attach_dev()가 IOMMU를 찾을 때마다 dom_node로 매단다.
	 * 읽는 자: TLB 무효화 콜백들이 "어느 IOMMU들의 TLB를 비울지" 알기 위해
	 *          순회하고, iova_to_phys()가 첫 IOMMU를 꺼내 쓴다.
	 * 값 범위: 비어 있을 수도(아직 attach 안 됨), 여러 개일 수도 있다.
	 * 동기화: msm_iommu_lock으로 보호된다.
	 * 주의: 연결 고리로 msm_iommu_dev의 dom_node를 쓰므로, 한 IOMMU가
	 *       동시에 두 도메인에 속할 수는 없다. */

	struct iommu_domain domain;
	/* [한국어] IOMMU 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging()이 pgsize_bitmap과 geometry를 채운다.
	 * 읽는 자: 코어 전반, 그리고 domain_config()가 pgsize_bitmap을 그대로
	 *          io-pgtable 설정으로 넘긴다.
	 * 값 범위: aperture는 0~4GB-1, pgsize_bitmap은 MSM_IOMMU_PGSIZES. */

	struct io_pgtable_cfg	cfg;
	/* [한국어] io-pgtable에 넘기는 설정이자, 그것이 채워 돌려주는 결과.
	 * 특히 cfg.arm_v7s_cfg의 ttbr/tcr/prrr/nmrr 값이 중요한데,
	 * __program_context()가 그 값들을 그대로 하드웨어 레지스터에 쓴다.
	 * 즉 이 구조체가 io-pgtable과 하드웨어 사이의 다리다.
	 * 설정자: msm_iommu_domain_config()가 입력을 채우고,
	 *         alloc_io_pgtable_ops()가 출력(ttbr 등)을 채운다.
	 * 읽는 자: __program_context(). */

	struct io_pgtable_ops	*iop;
	/* [한국어] io-pgtable 백엔드의 연산 테이블(map_pages/unmap_pages 등).
	 * 설정자: msm_iommu_domain_config()의 alloc_io_pgtable_ops().
	 * 읽는 자: msm_iommu_map()/unmap()이 실제 작업을 여기로 위임한다.
	 * 값 범위: NULL(아직 config 전) 또는 유효한 ops 포인터.
	 * 해제: identity_attach()가 free_io_pgtable_ops()로 반납한다 —
	 *       domain_free()가 아니라는 점에 주의. */

	struct device		*dev;
	/* [한국어] 이 도메인에 마지막으로 attach된 디바이스.
	 * 설정자: attach_dev()가 매 attach마다 덮어쓴다.
	 * 읽는 자: domain_config()가 io-pgtable의 iommu_dev와 dev_err 대상으로 쓴다.
	 * 왜 하나만 두는가: io-pgtable에 넘길 대표 디바이스가 하나 필요할 뿐이고,
	 *                   여러 디바이스가 붙어도 페이지 테이블은 하나이기 때문이다. */

	spinlock_t		pgtlock; /* pagetable lock */
	/* [한국어] io-pgtable 호출을 직렬화하는 도메인별 락.
	 * 설정자: msm_iommu_domain_config()가 초기화한다.
	 * 읽는 자: msm_iommu_map()/unmap()이 irqsave로 잡는다.
	 * 왜 전역 락과 별개인가: map/unmap은 빈번하고 하드웨어를 건드리지 않으므로
	 *                        전역 락으로 묶으면 불필요한 경합이 생긴다.
	 * 주의: 이 락 안에서 호출되는 io-pgtable이 TLB 콜백을 통해
	 *       msm_iommu_lock을 잡을 수 있어, 락 순서는 pgtlock → msm_iommu_lock이다. */
};

/*
 * [한국어]
 * to_msm_priv - 일반 iommu_domain을 이 드라이버의 도메인으로 되돌린다
 *
 * @dom: 코어가 넘긴 일반 도메인 포인터.
 * @return: 그것을 감싸는 struct msm_priv 포인터.
 *
 * 왜 필요한가: 코어는 iommu_domain만 알고, 드라이버는 그것을 자기 구조체에
 * 임베드해 둔다. container_of로 바깥 구조체를 복원하는 표준 관용구다.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄. 순수 포인터 산술이다.
 *
 * 호출 체인:
 *   msm_iommu_map()/unmap()/attach_dev()/domain_free() → [to_msm_priv]
 */
static struct msm_priv *to_msm_priv(struct iommu_domain *dom)
{
	/* [한국어] 임베드된 멤버 주소에서 오프셋을 빼 바깥 구조체를 얻는다. */
	return container_of(dom, struct msm_priv, domain);
}

/*
 * [한국어]
 * __enable_clocks - IOMMU 레지스터 접근에 필요한 클록을 켠다
 *
 * @iommu: 대상 IOMMU 인스턴스.
 * @return: 0 성공, 클록 활성화 실패 시 음수 errno.
 *
 * 왜 매번 켜야 하는가: 이 SoC는 전력 절감을 위해 IOMMU 클록을 평소에 꺼 둔다.
 * 클록이 꺼진 상태에서 레지스터를 읽으면 값이 오지 않거나 버스가 멈춘다.
 * 그래서 이 파일의 모든 하드웨어 접근이 enable/disable 쌍으로 감싸여 있다.
 *
 * 두 클록의 관계: pclk는 필수(레지스터 인터페이스용), clk는 선택적
 * (IOMMU 코어 로직용)이다. clk 활성화가 실패하면 이미 켠 pclk를 되돌려
 * 짝을 맞춘다 — 이 되감기가 없으면 클록 참조 카운트가 새어 나간다.
 *
 * 실행 컨텍스트: 대부분 msm_iommu_lock을 쥔 상태에서 호출된다.
 * clk_enable()은 prepare가 이미 되어 있어 잠들지 않는다(probe에서
 * devm_clk_get_prepared로 미리 prepare 해 두었다).
 *
 * 호출 체인:
 *   attach_dev/flush/iova_to_phys/fault_handler → [__enable_clocks]
 */
static int __enable_clocks(struct msm_iommu_dev *iommu)
{
	/* [한국어] 클록 활성화 결과. */
	int ret;

	/* [한국어] 레지스터 인터페이스 클록을 먼저 켠다. 이것이 없으면
	 * 아무 레지스터도 읽을 수 없다. */
	ret = clk_enable(iommu->pclk);
	/* [한국어] 실패하면 아무것도 켜지 않은 상태이므로 그대로 반환한다. */
	if (ret)
		goto fail;

	/* [한국어] 선택적 코어 클록이 있는 인스턴스라면 그것도 켠다. */
	if (iommu->clk) {
		ret = clk_enable(iommu->clk);
		/* [한국어] 두 번째가 실패하면 첫 번째를 되돌려 짝을 맞춘다.
		 * ret은 그대로 남아 호출자에게 실패가 전달된다. */
		if (ret)
			clk_disable(iommu->pclk);
	}
/* [한국어] 성공/실패가 함께 모이는 종료 지점. 정리할 것이 없어 반환만 한다. */
fail:
	return ret;	/* [한국어] 클록 활성화 결과를 그대로 호출자에게 전달한다. */
}

/*
 * [한국어]
 * __disable_clocks - 켜 두었던 IOMMU 클록을 끈다
 *
 * @iommu: 대상 IOMMU 인스턴스.
 * @return: 없음.
 *
 * 순서에 주목: 켤 때와 반대로 clk를 먼저 끄고 pclk를 나중에 끈다.
 * pclk가 레지스터 인터페이스의 토대이므로 가장 마지막까지 살려 두는 것이
 * 안전하다는 일반적인 클록 관리 원칙을 따른 것이다.
 *
 * 실행 컨텍스트: 하드웨어 접근이 끝난 직후. 잠들지 않는다.
 *
 * 호출 체인:
 *   attach_dev/flush/iova_to_phys/fault_handler → [__disable_clocks]
 */
static void __disable_clocks(struct msm_iommu_dev *iommu)
{
	/* [한국어] 선택적 코어 클록이 있으면 먼저 끈다. */
	if (iommu->clk)
		clk_disable(iommu->clk);
	/* [한국어] 레지스터 인터페이스 클록을 마지막에 끈다. */
	clk_disable(iommu->pclk);
}

/*
 * [한국어]
 * msm_iommu_reset - IOMMU 전체와 모든 컨텍스트 뱅크를 초기 상태로 되돌린다
 *
 * @base: IOMMU MMIO 레지스터 베이스.
 * @ncb: 이 IOMMU가 가진 컨텍스트 뱅크의 개수(디바이스 트리의 qcom,ncb).
 * @return: 없음.
 *
 * 왜 필요한가: 부트로더가 IOMMU를 어떤 상태로 남겨 두었는지 알 수 없다.
 * 리눅스가 자기 페이지 테이블을 설치하기 전에, 남아 있을 수 있는 옛 매핑과
 * 설정을 전부 지워야 한다 — 그렇지 않으면 부트로더가 만든 매핑으로 DMA가
 * 통과하거나, 예상치 못한 인터럽트가 쏟아질 수 있다.
 *
 * 두 부분으로 나뉜다: 앞쪽은 IOMMU 전역 레지스터(RPU 관련, 테스트 버스,
 * 전역 TLB 무효화 등)를 0으로 만들고, 뒤쪽 루프는 각 컨텍스트 뱅크의
 * 모든 레지스터를 0으로 만든다.
 *
 * TLBLKCRWE에만 1을 쓰는 이유: 이 비트는 "TLB 잠금 레지스터에 쓸 수 있게
 * 허용한다"는 뜻이라, 0으로 두면 이후 설정이 막힌다. 나머지가 전부 0인
 * 가운데 유일한 예외다.
 *
 * 실행 컨텍스트: probe 중 한 번. 이 시점에는 클록이 아직 켜지지 않았는데,
 * probe가 곧바로 레지스터를 만지는 것으로 보아 부팅 초기에는 클록이
 * 기본으로 켜져 있다는 전제로 보인다.
 *
 * 호출 체인:
 *   msm_iommu_probe() → [msm_iommu_reset] → SET_* 매크로들
 */
static void msm_iommu_reset(void __iomem *base, int ncb)
{
	/* [한국어] 컨텍스트 뱅크 순회 인덱스. */
	int ctx;

	/* [한국어] RPU(Remote Processor Unit) 활성화를 끈다. */
	SET_RPUE(base, 0);
	/* [한국어] RPU 관련 인터럽트를 끈다. */
	SET_RPUEIE(base, 0);
	/* [한국어] ESR(오류 상태) 복원 기능을 끈다. */
	SET_ESRRESTORE(base, 0);
	/* [한국어] 테스트 버스 활성화를 끈다(디버그 기능). */
	SET_TBE(base, 0);
	/* [한국어] 전역 제어 레지스터를 0으로 — IOMMU 전체 동작을 기본값으로. */
	SET_CR(base, 0);
	/* [한국어] SPDM(Secure Peripheral Debug Mode) 버스 활성화를 끈다. */
	SET_SPDMBE(base, 0);
	/* [한국어] 테스트 버스 제어 레지스터를 0으로. */
	SET_TESTBUSCR(base, 0);
	/* [한국어] TLB 읽기/쓰기 스위치를 0으로(디버그용 TLB 직접 접근을 끈다). */
	SET_TLBRSW(base, 0);
	/* [한국어] 전역 TLB를 통째로 무효화한다 — 부트로더가 남긴 변환을 지운다. */
	SET_GLOBAL_TLBIALL(base, 0);
	/* [한국어] RPU 보조 제어 레지스터를 0으로. */
	SET_RPU_ACR(base, 0);
	/* [한국어] TLB 잠금 레지스터에 쓰기를 허용한다. 위 모든 설정과 달리
	 * 1을 쓰는 유일한 항목으로, 이후 뱅크별 TLBLKCR 설정이 통하게 하려는 것이다. */
	SET_TLBLKCRWE(base, 1);

	/* [한국어] 모든 컨텍스트 뱅크를 순회하며 초기화한다. */
	for (ctx = 0; ctx < ncb; ctx++) {
		/* [한국어] 아우터 공유(outer shareable) 영역의 버스 프리페치 제어를 끈다. */
		SET_BPRCOSH(base, ctx, 0);
		/* [한국어] 이너 공유(inner shareable) 영역의 버스 프리페치 제어를 끈다. */
		SET_BPRCISH(base, ctx, 0);
		/* [한국어] 비공유(non-shareable) 영역의 버스 프리페치 제어를 끈다. */
		SET_BPRCNSH(base, ctx, 0);
		/* [한국어] 버스 프리페치의 공유성 설정을 기본값으로. */
		SET_BPSHCFG(base, ctx, 0);
		/* [한국어] 버스 프리페치의 메모리 타입 설정을 기본값으로. */
		SET_BPMTCFG(base, ctx, 0);
		/* [한국어] 뱅크의 보조 제어 레지스터를 0으로. */
		SET_ACTLR(base, ctx, 0);
		/* [한국어] 뱅크의 시스템 제어 레지스터를 0으로 — MMU 활성화 비트를
		 * 포함하므로 이 한 줄이 그 뱅크의 변환을 꺼 버린다. */
		SET_SCTLR(base, ctx, 0);
		/* [한국어] 폴트 상태 복원 기능을 끈다. */
		SET_FSRRESTORE(base, ctx, 0);
		/* [한국어] 페이지 테이블 베이스 0을 지운다 — 옛 테이블을 가리키지 못하게. */
		SET_TTBR0(base, ctx, 0);
		/* [한국어] 페이지 테이블 베이스 1도 지운다. */
		SET_TTBR1(base, ctx, 0);
		/* [한국어] TTBR 제어 레지스터(주소 분할 설정)를 0으로. */
		SET_TTBCR(base, ctx, 0);
		/* [한국어] BFB(Branch/Buffer Fetch) 제어를 0으로. */
		SET_BFBCR(base, ctx, 0);
		/* [한국어] PAR(물리 주소 레지스터)를 지운다 — V2P 변환 결과 자리다. */
		SET_PAR(base, ctx, 0);
		/* [한국어] FAR(폴트 주소 레지스터)를 지운다. */
		SET_FAR(base, ctx, 0);
		/* [한국어] 이 뱅크의 TLB를 무효화한다. */
		SET_CTX_TLBIALL(base, ctx, 0);
		/* [한국어] 1단계 페이지 테이블 엔트리용 TLB를 지운다. */
		SET_TLBFLPTER(base, ctx, 0);
		/* [한국어] 2단계 페이지 테이블 엔트리용 TLB를 지운다. */
		SET_TLBSLPTER(base, ctx, 0);
		/* [한국어] TLB 잠금 제어를 0으로 — 고정된 TLB 엔트리를 푼다. */
		SET_TLBLKCR(base, ctx, 0);
		/* [한국어] 컨텍스트 ID(ASID를 담는 레지스터)를 0으로. */
		SET_CONTEXTIDR(base, ctx, 0);
	}
}

/*
 * [한국어]
 * __flush_iotlb - 이 도메인에 속한 모든 컨텍스트 뱅크의 TLB를 통째로 비운다
 *
 * @cookie: io-pgtable에 등록해 둔 쿠키 — 여기서는 struct msm_priv 포인터다.
 * @return: 없음.
 *
 * 왜 필요한가: io-pgtable이 "전체 무효화가 필요하다"고 판단했을 때 부르는
 * 콜백이다. 도메인은 여러 IOMMU에, 각 IOMMU는 여러 컨텍스트 뱅크에 걸쳐
 * 있으므로 이중 루프로 전부 순회해야 한다.
 *
 * 클록 처리에 주목: IOMMU마다 켜고 끄기를 반복한다. 여러 IOMMU를 한꺼번에
 * 켜 두지 않는 이유는, 각 IOMMU의 클록이 독립적이고 켜 둔 시간을 최소화하는
 * 편이 전력에 유리하기 때문이다.
 *
 * 에러 처리의 한계: 클록 활성화가 실패하면 goto fail로 빠져나가는데,
 * fail 레이블이 곧 함수 끝이라 남은 IOMMU들은 무효화되지 않는다.
 * 반환값도 없어 호출자가 실패를 알 방법이 없다 — 기존 코드의 약점이며
 * 여기서는 사실만 기록한다.
 *
 * 실행 컨텍스트: io-pgtable의 map/unmap 경로에서 불린다. 호출자가
 * priv->pgtlock을 쥐고 있을 수 있다.
 *
 * 호출 체인:
 *   io-pgtable → msm_iommu_flush_ops.tlb_flush_all → [__flush_iotlb]
 */
static void __flush_iotlb(void *cookie)
{
	/* [한국어] io-pgtable 생성 시 넘긴 쿠키가 곧 도메인 상태다. */
	struct msm_priv *priv = cookie;
	/* [한국어] 순회할 IOMMU 인스턴스. NULL 초기화는 형식적이다. */
	struct msm_iommu_dev *iommu = NULL;
	/* [한국어] 각 IOMMU에 붙은 마스터(=컨텍스트 뱅크 소유자). */
	struct msm_iommu_ctx_dev *master;
	/* [한국어] 클록 활성화 결과. */
	int ret = 0;

	/* [한국어] 이 도메인이 붙어 있는 모든 IOMMU를 순회한다. */
	list_for_each_entry(iommu, &priv->list_attached, dom_node) {
		/* [한국어] 레지스터를 만지려면 클록이 필요하다. */
		ret = __enable_clocks(iommu);
		/* [한국어] 실패하면 이 IOMMU는 건드릴 수 없다 — 함수를 벗어난다. */
		if (ret)
			goto fail;

		/* [한국어] 이 IOMMU에 붙은 각 마스터의 컨텍스트 뱅크 TLB를 비운다.
		 * master->num이 그 마스터에 할당된 뱅크 번호다. */
		list_for_each_entry(master, &iommu->ctx_list, list)
			SET_CTX_TLBIALL(iommu->base, master->num, 0);

		/* [한국어] 이 IOMMU의 작업이 끝났으니 클록을 되돌린다. */
		__disable_clocks(iommu);
	}
/* [한국어] 정상 종료와 클록 실패가 함께 도달하는 지점. 정리할 것이 없다. */
fail:
	return;
}

/*
 * [한국어]
 * __flush_iotlb_range - 지정한 IOVA 범위의 TLB 엔트리를 무효화한다
 *
 * @iova: 무효화 시작 주소.
 * @size: 무효화할 총 바이트 수.
 * @granule: 한 번에 무효화되는 단위(보통 페이지 크기).
 * @leaf: 리프 엔트리만 무효화하면 되는지 여부 — 이 하드웨어는 구분하지
 *        못하므로 무시된다.
 * @cookie: struct msm_priv 포인터.
 * @return: 없음.
 *
 * 이 함수의 핵심은 **ASID를 주소에 실어 보내는 것**이다. TLBIVA 레지스터는
 * 상위 비트에 가상 주소, 하위 비트에 ASID를 함께 받는다. 그래서
 * iova를 TLBIVA_VA로 마스킹해 주소 부분만 남긴 뒤, 그 뱅크의 CONTEXTIDR에서
 * 읽어 온 ASID를 OR 해서 쓴다. 이렇게 하면 같은 주소라도 다른 ASID의
 * 엔트리는 살아남아, 무관한 문맥의 성능을 해치지 않는다.
 *
 * 루프 구조의 함정: 안쪽 do-while이 iova를 granule씩 증가시키는데,
 * 그 iova가 바깥 루프(마스터 순회)와 공유된다. 즉 두 번째 마스터부터는
 * 이미 증가한 iova에서 시작하게 된다 — 여러 마스터가 한 도메인에 있을 때
 * 잘못 동작할 수 있는 기존 코드의 결함이다. 실제로는 마스터가 하나뿐인
 * 구성이 대부분이라 드러나지 않는다.
 *
 * `while (temp_size -= granule)` 조건: size가 granule의 배수라는 전제로
 * 0이 될 때까지 반복한다. 배수가 아니면 음수로 내려가 오래 도는데,
 * io-pgtable이 항상 배수로 부르므로 성립한다.
 *
 * 실행 컨텍스트: io-pgtable의 무효화 경로와 msm_iommu_sync_map().
 *
 * 호출 체인:
 *   io-pgtable / msm_iommu_sync_map() → [__flush_iotlb_range]
 *   → __enable_clocks(), SET_TLBIVA()
 */
static void __flush_iotlb_range(unsigned long iova, size_t size,
				size_t granule, bool leaf, void *cookie)
{
	/* [한국어] 도메인 상태(쿠키). */
	struct msm_priv *priv = cookie;
	/* [한국어] 순회할 IOMMU. */
	struct msm_iommu_dev *iommu = NULL;
	/* [한국어] 각 IOMMU의 마스터(컨텍스트 뱅크 소유자). */
	struct msm_iommu_ctx_dev *master;
	/* [한국어] 클록 활성화 결과. */
	int ret = 0;
	/* [한국어] 남은 크기를 세는 변수. 원본 size를 망가뜨리지 않으려고
	 * 마스터마다 다시 초기화한다. */
	int temp_size;

	/* [한국어] 이 도메인이 붙어 있는 모든 IOMMU를 순회한다. */
	list_for_each_entry(iommu, &priv->list_attached, dom_node) {
		/* [한국어] 레지스터 접근을 위해 클록을 켠다. */
		ret = __enable_clocks(iommu);
		if (ret)	/* [한국어] 클록을 켜지 못했으면 이 IOMMU는 건드릴 수 없다. */
			goto fail;

		/* [한국어] 각 마스터의 컨텍스트 뱅크에 대해 범위를 무효화한다. */
		list_for_each_entry(master, &iommu->ctx_list, list) {
			/* [한국어] 남은 크기를 이 마스터 기준으로 다시 세팅한다. */
			temp_size = size;
			do {
				/* [한국어] 주소에서 TLBIVA가 받는 가상 주소 부분만 남긴다.
				 * 하위 비트는 ASID 자리이므로 지워야 한다. */
				iova &= TLBIVA_VA;
				/* [한국어] 이 뱅크에 설정된 ASID를 읽어 주소에 실어 넣는다.
				 * 이렇게 해야 이 문맥의 엔트리만 정확히 무효화된다. */
				iova |= GET_CONTEXTIDR_ASID(iommu->base,
							    master->num);
				/* [한국어] 주소+ASID를 TLBIVA에 써서 그 엔트리를 무효화한다. */
				SET_TLBIVA(iommu->base, master->num, iova);
				/* [한국어] 다음 granule 경계로 전진한다. */
				iova += granule;
			} while (temp_size -= granule);
		}

		/* [한국어] 이 IOMMU의 작업이 끝났으니 클록을 끈다. */
		__disable_clocks(iommu);
	}

/* [한국어] 정상 종료와 클록 실패가 모이는 지점. */
fail:
	return;
}

/*
 * [한국어]
 * __flush_iotlb_walk - 페이지 테이블 워크 캐시까지 포함한 범위 무효화 콜백
 *
 * @iova: 무효화 시작 주소.
 * @size: 무효화할 바이트 수.
 * @granule: 무효화 단위.
 * @cookie: struct msm_priv 포인터.
 * @return: 없음.
 *
 * 왜 필요한가: io-pgtable은 "리프 엔트리만 바뀌었는가, 중간 테이블도
 * 바뀌었는가"를 구분해 다른 콜백을 부른다. 중간 테이블이 바뀌면 워크 캐시도
 * 비워야 하는 하드웨어가 있기 때문이다. 이 IOMMU는 그 구분이 없어
 * leaf=false만 전달하고 같은 함수를 쓴다.
 *
 * 실행 컨텍스트: io-pgtable의 테이블 변경 경로.
 *
 * 호출 체인:
 *   io-pgtable → msm_iommu_flush_ops.tlb_flush_walk → [__flush_iotlb_walk]
 *   → __flush_iotlb_range()
 */
static void __flush_iotlb_walk(unsigned long iova, size_t size,
			       size_t granule, void *cookie)
{
	/* [한국어] leaf를 false로 넘기지만, 대상 함수가 그 인자를 쓰지 않으므로
	 * 실질적으로는 범위 무효화 그 자체다. */
	__flush_iotlb_range(iova, size, granule, false, cookie);
}

/*
 * [한국어]
 * __flush_iotlb_page - 페이지 하나를 무효화 대상에 추가하는 콜백
 *
 * @gather: 무효화 범위를 모으는 구조체 — 이 드라이버는 즉시 무효화하므로
 *          사용하지 않는다.
 * @iova: 무효화할 페이지의 주소.
 * @granule: 페이지 크기.
 * @cookie: struct msm_priv 포인터.
 * @return: 없음.
 *
 * 왜 gather를 쓰지 않는가: 원래 이 콜백은 "나중에 한꺼번에 비울 범위를
 * 모아 두라"는 뜻이다. 그런데 이 드라이버는 iotlb_sync 콜백을 NULL로
 * 등록해 두었으므로(파일 아래 ops 정의 참조), 모아 봐야 비울 시점이 없다.
 * 그래서 여기서 즉시 무효화한다 — size와 granule을 같게 넘겨 한 페이지만
 * 처리하게 하는 것이 그 방법이다.
 *
 * 실행 컨텍스트: io-pgtable의 unmap 경로.
 *
 * 호출 체인:
 *   io-pgtable → msm_iommu_flush_ops.tlb_add_page → [__flush_iotlb_page]
 *   → __flush_iotlb_range()
 */
static void __flush_iotlb_page(struct iommu_iotlb_gather *gather,
			       unsigned long iova, size_t granule, void *cookie)
{
	/* [한국어] size와 granule을 같게 넘겨 딱 한 페이지만 무효화한다.
	 * leaf=true는 대상 함수가 무시한다. */
	__flush_iotlb_range(iova, granule, granule, true, cookie);
}

/* [한국어] io-pgtable에 등록하는 TLB 무효화 콜백 묶음.
 * 페이지 테이블을 io-pgtable이 관리하는 대신, 그 변경을 하드웨어에 반영하는
 * 일은 이 드라이버가 맡는다는 계약이다. */
static const struct iommu_flush_ops msm_iommu_flush_ops = {
	/* [한국어] 전체 무효화 — 모든 뱅크의 TLB를 통째로 비운다. */
	.tlb_flush_all = __flush_iotlb,
	/* [한국어] 중간 테이블 변경 시의 범위 무효화. */
	.tlb_flush_walk = __flush_iotlb_walk,
	/* [한국어] 페이지 하나 무효화 — gather를 쓰지 않고 즉시 처리한다. */
	.tlb_add_page = __flush_iotlb_page,
};

/*
 * [한국어]
 * msm_iommu_alloc_ctx - 비어 있는 컨텍스트 뱅크를 하나 할당한다
 *
 * @map: 뱅크 사용 여부를 나타내는 비트맵(IOMMU마다 하나씩).
 * @start: 탐색을 시작할 뱅크 번호.
 * @end: 뱅크 번호의 상한(= ncb).
 * @return: 할당된 뱅크 번호, 남은 뱅크가 없으면 -ENOSPC.
 *
 * 왜 test_and_set_bit 루프인가: find_next_zero_bit로 후보를 찾은 뒤
 * 그 비트를 원자적으로 세우는데, 그 사이에 다른 CPU가 먼저 가져갔을 수 있다.
 * test_and_set_bit이 "이미 세워져 있었다"고 알려 주면 다음 후보를 찾아
 * 다시 시도한다 — 락 없이 경쟁을 해결하는 표준 패턴이다.
 * (실제로는 호출자가 msm_iommu_lock을 쥐고 있어 경쟁이 없지만, 함수 자체는
 * 락에 의존하지 않도록 짜여 있다.)
 *
 * 실행 컨텍스트: attach 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   msm_iommu_attach_dev() → [msm_iommu_alloc_ctx]
 */
static int msm_iommu_alloc_ctx(unsigned long *map, int start, int end)
{
	/* [한국어] 후보 뱅크 번호. */
	int idx;

	do {
		/* [한국어] start부터 end 사이에서 처음으로 0인 비트(=빈 뱅크)를 찾는다. */
		idx = find_next_zero_bit(map, end, start);
		/* [한국어] end가 반환되면 빈 자리가 없다는 뜻이다. */
		if (idx == end)
			return -ENOSPC;
		/* [한국어] 그 비트를 원자적으로 세운다. 이미 세워져 있었다면
		 * 누군가 먼저 가져간 것이므로 루프를 다시 돈다. */
	} while (test_and_set_bit(idx, map));

	/* [한국어] 성공적으로 확보한 뱅크 번호를 반환한다. */
	return idx;
}

/*
 * [한국어]
 * msm_iommu_free_ctx - 컨텍스트 뱅크를 반납한다
 *
 * @map: 뱅크 사용 비트맵.
 * @idx: 반납할 뱅크 번호.
 * @return: 없음.
 *
 * 왜 이것만으로 충분한가: 비트를 지우면 다음 alloc이 이 뱅크를 찾을 수 있다.
 * 하드웨어 쪽 정리(__reset_context)는 호출자가 따로 수행하므로, 이 함수는
 * 소프트웨어 장부만 갱신한다.
 *
 * 실행 컨텍스트: detach(identity attach) 경로.
 *
 * 호출 체인:
 *   msm_iommu_identity_attach() → [msm_iommu_free_ctx]
 */
static void msm_iommu_free_ctx(unsigned long *map, int idx)
{
	/* [한국어] 해당 비트를 원자적으로 지워 뱅크를 다시 쓸 수 있게 한다. */
	clear_bit(idx, map);
}

/*
 * [한국어]
 * config_mids - 이 마스터의 MID들을 지정된 컨텍스트 뱅크에 연결한다
 *
 * @iommu: 대상 IOMMU 인스턴스.
 * @master: 연결할 마스터(디바이스). mids[] 배열과 뱅크 번호 num을 갖고 있다.
 * @return: 없음.
 *
 * 왜 필요한가: 버스에 나오는 트랜잭션에는 마스터를 식별하는 MID가 실린다.
 * IOMMU는 M2VCBR(MID-to-VMID/Context-Bank Register) 테이블로 "이 MID는
 * 어느 컨텍스트 뱅크가 처리한다"를 결정한다. 이 함수가 그 테이블을 채워
 * 디바이스와 뱅크를 실제로 이어 준다.
 *
 * 각 MID마다 설정하는 것들:
 *  - VMID = 0: 가상화를 쓰지 않으므로 모두 같은 VM에 속한다.
 *  - CBNDX = ctx: 이 MID의 트랜잭션을 처리할 컨텍스트 뱅크 번호. 핵심 설정이다.
 *  - CBVMID = 0: 그 뱅크가 속한 VMID.
 *  - CONTEXTIDR_ASID = ctx: 뱅크 번호를 그대로 ASID로 쓴다. TLB 태깅에
 *    쓰이며, __flush_iotlb_range()가 이 값을 읽어 주소에 실어 보낸다.
 *  - NSCFG = 3: 보안 비트를 비보안(non-secure)으로 강제한다. 리눅스는
 *    비보안 세계에서 도므로 트랜잭션을 그렇게 표시해야 한다.
 *
 * M2VCBR_N과 CBACR_N을 먼저 0으로 지우는 이유: 이어지는 개별 설정이
 * read-modify-write일 수 있어, 남아 있던 옛 값이 섞이지 않게 하려는 것이다.
 *
 * 실행 컨텍스트: attach 경로. msm_iommu_lock을 쥔 상태이며 클록이 켜져 있어야 한다.
 *
 * 호출 체인:
 *   msm_iommu_attach_dev() → [config_mids] → SET_* 매크로들
 */
static void config_mids(struct msm_iommu_dev *iommu,
			struct msm_iommu_ctx_dev *master)
{
	/* [한국어] 현재 처리 중인 MID, 대상 뱅크 번호, 순회 인덱스. */
	int mid, ctx, i;

	/* [한국어] 이 마스터가 가진 모든 MID를 순회한다. 하나의 디바이스가
	 * 여러 MID를 낼 수 있다(예: 읽기 채널과 쓰기 채널). */
	for (i = 0; i < master->num_mids; i++) {
		/* [한국어] of_xlate가 쌓아 둔 i번째 MID. */
		mid = master->mids[i];
		/* [한국어] 이 마스터에 할당된 컨텍스트 뱅크 번호. */
		ctx = master->num;

		/* [한국어] 이 MID의 매핑 레지스터를 먼저 0으로 지운다. */
		SET_M2VCBR_N(iommu->base, mid, 0);
		/* [한국어] 이 뱅크의 속성 레지스터도 0으로 지운다. */
		SET_CBACR_N(iommu->base, ctx, 0);

		/* Set VMID = 0 */
		/* [한국어] 이 MID의 VMID를 0으로 — 가상화를 쓰지 않으므로
		 * 모든 마스터가 같은 가상 머신에 속한다. */
		SET_VMID(iommu->base, mid, 0);

		/* Set the context number for that MID to this context */
		/* [한국어] 이 함수의 핵심 한 줄. 이 MID로 오는 트랜잭션을
		 * ctx번 컨텍스트 뱅크가 처리하도록 지정한다. */
		SET_CBNDX(iommu->base, mid, ctx);

		/* Set MID associated with this context bank to 0*/
		/* [한국어] 이 뱅크가 속한 VMID를 0으로 — 위 SET_VMID와 짝을 이룬다. */
		SET_CBVMID(iommu->base, ctx, 0);

		/* Set the ASID for TLB tagging for this context */
		/* [한국어] 뱅크 번호를 그대로 ASID로 쓴다. TLB 엔트리에 이 값이
		 * 태그로 붙고, 무효화 시 __flush_iotlb_range()가 이 레지스터를
		 * 읽어 주소에 실어 보낸다. */
		SET_CONTEXTIDR_ASID(iommu->base, ctx, ctx);

		/* Set security bit override to be Non-secure */
		/* [한국어] 이 MID의 트랜잭션을 비보안으로 강제한다. 리눅스는
		 * 비보안 세계에서 돌기 때문이며, 값 3이 "강제 비보안"을 뜻한다. */
		SET_NSCFG(iommu->base, mid, 3);
	}
}

/*
 * [한국어]
 * __reset_context - 컨텍스트 뱅크 하나를 초기 상태로 되돌린다
 *
 * @base: IOMMU MMIO 베이스.
 * @ctx: 초기화할 뱅크 번호.
 * @return: 없음.
 *
 * 왜 필요한가: 두 곳에서 쓰인다 — (1) __program_context()가 새 설정을
 * 쓰기 전에 깨끗한 상태를 만들 때, (2) detach 시 뱅크를 반납하기 전에
 * 남은 매핑을 지울 때. 특히 (2)가 중요한데, 여기서 SCTLR을 0으로 만들어
 * MMU를 꺼야 이후 그 MID의 DMA가 통과하지 않는다.
 *
 * msm_iommu_reset()의 뱅크 루프와 거의 같지만 CONTEXTIDR만 빠져 있다.
 * detach 시 ASID를 남겨 두는 것이 의도인지 누락인지는 코드만으로 알 수 없다.
 *
 * 실행 컨텍스트: attach/detach 경로. 클록이 켜져 있어야 한다.
 *
 * 호출 체인:
 *   __program_context() / msm_iommu_identity_attach() → [__reset_context]
 */
static void __reset_context(void __iomem *base, int ctx)
{
	/* [한국어] 아우터 공유 영역 버스 프리페치 제어를 끈다. */
	SET_BPRCOSH(base, ctx, 0);
	/* [한국어] 이너 공유 영역 버스 프리페치 제어를 끈다. */
	SET_BPRCISH(base, ctx, 0);
	/* [한국어] 비공유 영역 버스 프리페치 제어를 끈다. */
	SET_BPRCNSH(base, ctx, 0);
	/* [한국어] 버스 프리페치 공유성 설정을 기본값으로. */
	SET_BPSHCFG(base, ctx, 0);
	/* [한국어] 버스 프리페치 메모리 타입 설정을 기본값으로. */
	SET_BPMTCFG(base, ctx, 0);
	/* [한국어] 뱅크 보조 제어 레지스터를 0으로. */
	SET_ACTLR(base, ctx, 0);
	/* [한국어] 시스템 제어 레지스터를 0으로 — MMU 활성화 비트가 여기 있어
	 * 이 한 줄이 그 뱅크의 변환을 끈다. detach에서 가장 중요한 줄이다. */
	SET_SCTLR(base, ctx, 0);
	/* [한국어] 폴트 상태 복원 기능을 끈다. */
	SET_FSRRESTORE(base, ctx, 0);
	/* [한국어] 페이지 테이블 베이스를 지워 옛 테이블을 가리키지 못하게 한다. */
	SET_TTBR0(base, ctx, 0);
	/* [한국어] 두 번째 페이지 테이블 베이스도 지운다. */
	SET_TTBR1(base, ctx, 0);
	/* [한국어] TTBR 제어(주소 분할) 설정을 0으로. */
	SET_TTBCR(base, ctx, 0);
	/* [한국어] BFB 제어를 0으로. */
	SET_BFBCR(base, ctx, 0);
	/* [한국어] 물리 주소 레지스터(V2P 결과 자리)를 지운다. */
	SET_PAR(base, ctx, 0);
	/* [한국어] 폴트 주소 레지스터를 지운다. */
	SET_FAR(base, ctx, 0);
	/* [한국어] 이 뱅크의 TLB를 무효화한다 — 옛 변환이 남지 않게. */
	SET_CTX_TLBIALL(base, ctx, 0);
	/* [한국어] 1단계 테이블 엔트리 TLB를 지운다. */
	SET_TLBFLPTER(base, ctx, 0);
	/* [한국어] 2단계 테이블 엔트리 TLB를 지운다. */
	SET_TLBSLPTER(base, ctx, 0);
	/* [한국어] TLB 잠금 제어를 0으로 — 고정 엔트리를 푼다. */
	SET_TLBLKCR(base, ctx, 0);
}

/*
 * [한국어]
 * __program_context - 컨텍스트 뱅크에 페이지 테이블과 동작 설정을 프로그래밍한다
 *
 * @base: IOMMU MMIO 베이스.
 * @ctx: 설정할 뱅크 번호.
 * @priv: 도메인 상태. cfg.arm_v7s_cfg에서 TTBR/TCR/PRRR/NMRR을 가져온다.
 * @return: 없음.
 *
 * 왜 이 함수가 중요한가: io-pgtable이 만든 페이지 테이블을 하드웨어가
 * 실제로 걷게 만드는 지점이다. alloc_io_pgtable_ops()가 채워 준
 * arm_v7s_cfg의 값들을 그대로 레지스터에 옮겨 쓰는 것이 핵심이며,
 * 그래서 이 IOMMU가 ARMv7 short-descriptor 형식을 "빌려 쓸" 수 있다.
 *
 * 설정 순서와 의미:
 *  1) __reset_context()로 깨끗이 지운다.
 *  2) TRE/AFE: TEX remap과 Access Flag를 켠다 — ARMv7S 형식이 요구하는
 *     메모리 속성 인코딩 방식이며, PRRR/NMRR과 짝을 이룬다.
 *  3) TLBMCFG/V2PCFG = 3: TLB 미스와 V2P 요청 모두를 하드웨어 테이블 워크
 *     (HTW)로 처리하게 한다. 소프트웨어 개입 없이 하드웨어가 테이블을 걷는다.
 *  4) TTBCR/TTBR0/TTBR1: io-pgtable이 만든 테이블의 위치와 분할 방식.
 *     TTBR1을 0으로 두는 것은 주소 공간을 나누지 않고 TTBR0만 쓴다는 뜻이다.
 *  5) PRRR/NMRR: TEX remap 방식에서 메모리 타입을 정의하는 레지스터 쌍.
 *  6) TLB 무효화: 새 테이블을 걸었으니 옛 엔트리를 지운다.
 *  7) CFEIE/CFCFG: 폴트 시 인터럽트를 내고, 접근을 멈춰(stall) 핸들러가
 *     처리하게 한다. 멈추지 않으면 폴트 정보가 덮어써질 수 있다.
 *  8) RCISH/RCOSH/RCNSH: 캐시 가능한 요청을 L2 슬레이브 포트로 보낸다.
 *  9) BFBDFE: 분기 예측 버퍼 프리페치를 켠다(성능).
 * 10) M = 1: 마지막으로 MMU를 켠다. 이 순간부터 변환이 시작된다.
 *
 * 실행 컨텍스트: attach 경로. msm_iommu_lock 보유, 클록 켜진 상태.
 *
 * 호출 체인:
 *   msm_iommu_attach_dev() → [__program_context] → __reset_context(), SET_*
 */
static void __program_context(void __iomem *base, int ctx,
			      struct msm_priv *priv)
{
	/* [한국어] 남아 있을 수 있는 옛 설정을 전부 지우고 시작한다. */
	__reset_context(base, ctx);

	/* Turn on TEX Remap */
	/* [한국어] TEX remap을 켠다 — 페이지 테이블의 TEX/C/B 비트를 그대로
	 * 쓰지 않고 PRRR/NMRR 레지스터를 통해 재해석하게 한다. ARMv7S
	 * io-pgtable이 이 방식을 전제로 엔트리를 만든다. */
	SET_TRE(base, ctx, 1);
	/* [한국어] Access Flag를 켠다 — 엔트리의 AF 비트를 하드웨어가
	 * 확인하게 한다. 역시 ARMv7S 형식의 요구사항이다. */
	SET_AFE(base, ctx, 1);

	/* Set up HTW mode */
	/* TLB miss configuration: perform HTW on miss */
	/* [한국어] TLB 미스가 나면 하드웨어가 직접 페이지 테이블을 걷게 한다
	 * (Hardware Table Walk). 값 3이 그 설정이며, 소프트웨어가 미스를
	 * 처리하던 옛 방식과 대비된다. */
	SET_TLBMCFG(base, ctx, 0x3);

	/* V2P configuration: HTW for access */
	/* [한국어] V2P(가상→물리 변환 질의)도 하드웨어 워크로 처리하게 한다.
	 * msm_iommu_iova_to_phys()가 이 기능을 쓴다. */
	SET_V2PCFG(base, ctx, 0x3);

	/* [한국어] io-pgtable이 계산해 준 TTBCR(주소 분할 설정)을 그대로 쓴다. */
	SET_TTBCR(base, ctx, priv->cfg.arm_v7s_cfg.tcr);
	/* [한국어] io-pgtable이 만든 페이지 테이블의 물리 주소를 TTBR0에 쓴다.
	 * 이 한 줄이 소프트웨어의 테이블과 하드웨어를 잇는 지점이다. */
	SET_TTBR0(base, ctx, priv->cfg.arm_v7s_cfg.ttbr);
	/* [한국어] TTBR1은 쓰지 않는다 — 주소 공간을 둘로 나누지 않고
	 * 전체를 TTBR0 하나로 덮는다. */
	SET_TTBR1(base, ctx, 0);

	/* Set prrr and nmrr */
	/* [한국어] PRRR(Primary Region Remap Register) — TEX remap에서
	 * 메모리 영역의 타입과 공유성을 정의한다. io-pgtable이 계산해 준
	 * 값을 그대로 쓴다. */
	SET_PRRR(base, ctx, priv->cfg.arm_v7s_cfg.prrr);
	/* [한국어] NMRR(Normal Memory Remap Register) — 일반 메모리의
	 * 캐시 정책(write-back/through 등)을 정의한다. */
	SET_NMRR(base, ctx, priv->cfg.arm_v7s_cfg.nmrr);

	/* Invalidate the TLB for this context */
	/* [한국어] 새 테이블을 걸었으니 이 뱅크의 TLB를 비운다. 이것을
	 * 빼먹으면 옛 매핑이 살아남아 잘못된 주소로 DMA가 나간다. */
	SET_CTX_TLBIALL(base, ctx, 0);

	/* Set interrupt number to "secure" interrupt */
	/* [한국어] 폴트 인터럽트를 0번(보안 인터럽트) 경로로 보낸다.
	 * probe에서 등록한 IRQ 이름이 "msm_iommu_secure_irpt_handler"인 것이
	 * 이 설정과 짝을 이룬다. */
	SET_IRPTNDX(base, ctx, 0);

	/* Enable context fault interrupt */
	/* [한국어] 이 뱅크에서 폴트가 나면 인터럽트를 내게 한다.
	 * 끄면 폴트가 조용히 무시되어 디버깅이 불가능해진다. */
	SET_CFEIE(base, ctx, 1);

	/* Stall access on a context fault and let the handler deal with it */
	/* [한국어] 폴트가 나면 해당 접근을 멈춘다(stall). 멈추지 않으면
	 * 후속 트랜잭션이 FAR/FSR을 덮어써 어느 주소가 문제였는지 알 수 없게 된다.
	 * 대신 핸들러가 FSR을 지워야 비로소 재개된다. */
	SET_CFCFG(base, ctx, 1);

	/* Redirect all cacheable requests to L2 slave port. */
	/* [한국어] 이너 공유 영역의 캐시 가능 요청을 L2 슬레이브 포트로 보낸다. */
	SET_RCISH(base, ctx, 1);
	/* [한국어] 아우터 공유 영역의 캐시 가능 요청도 마찬가지로. */
	SET_RCOSH(base, ctx, 1);
	/* [한국어] 비공유 영역의 캐시 가능 요청도 마찬가지로.
	 * 세 줄을 합쳐 "모든 캐시 가능 요청"을 L2로 보낸다는 뜻이 된다. */
	SET_RCNSH(base, ctx, 1);

	/* Turn on BFB prefetch */
	/* [한국어] BFB(Buffer Fetch Block) 프리페치를 켠다 — 페이지 테이블
	 * 엔트리를 미리 읽어 워크 지연을 줄이는 성능 최적화다. */
	SET_BFBDFE(base, ctx, 1);

	/* Enable the MMU */
	/* [한국어] 마지막으로 MMU를 켠다. 모든 설정이 끝난 뒤에 켜야
	 * 반쯤 구성된 상태로 변환이 일어나지 않는다. */
	SET_M(base, ctx, 1);
}

/*
 * [한국어]
 * msm_iommu_domain_alloc_paging - 새 페이징 도메인을 만든다
 *
 * @dev: 요청한 디바이스(사용하지 않는다).
 * @return: 새 도메인의 iommu_domain 포인터, 메모리 부족이면 NULL.
 *
 * 왜 io-pgtable을 여기서 만들지 않는가: io-pgtable 생성에는 DMA 마스터
 * 디바이스가 필요한데, 이 시점에는 어느 디바이스가 붙을지 모른다.
 * 그래서 실제 페이지 테이블 생성은 msm_iommu_domain_config()가
 * attach 시점에 수행한다.
 *
 * aperture를 0~4GB-1로 두는 이유: ARMv7 short-descriptor 형식이
 * 32비트 입력 주소만 다루기 때문이다(domain_config의 ias = 32와 짝).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   iommu_domain_alloc() → iommu_ops->domain_alloc_paging
 *   → [msm_iommu_domain_alloc_paging]
 */
static struct iommu_domain *msm_iommu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 새로 만들 도메인 상태. */
	struct msm_priv *priv;

	/* [한국어] 0으로 초기화해 할당한다. iop와 dev가 NULL로 시작해야
	 * "아직 config 전"임을 판별할 수 있다. */
	priv = kzalloc_obj(*priv);
	/* [한국어] 메모리 부족 — 아래 정리 레이블로 간다. */
	if (!priv)
		goto fail_nomem;

	/* [한국어] 이 도메인이 붙을 IOMMU들의 리스트 헤드를 초기화한다. */
	INIT_LIST_HEAD(&priv->list_attached);

	/* [한국어] 지원 페이지 크기를 코어에 알린다. 네 가지 크기가 있어
	 * 코어가 큰 매핑을 섹션/슈퍼섹션으로 효율적으로 처리할 수 있다. */
	priv->domain.pgsize_bitmap = MSM_IOMMU_PGSIZES;

	/* [한국어] IOVA 공간의 시작을 0으로 둔다. */
	priv->domain.geometry.aperture_start = 0;
	/* [한국어] 끝을 4GB-1로 둔다 — ARMv7S 형식의 32비트 입력 주소 한계다. */
	priv->domain.geometry.aperture_end   = (1ULL << 32) - 1;
	/* [한국어] 코어가 이 범위를 강제하게 해, 범위 밖 IOVA 할당 요청이
	 * 아예 들어오지 않게 한다. */
	priv->domain.geometry.force_aperture = true;

	/* [한국어] 코어에는 임베드된 일반 도메인 포인터를 돌려준다. */
	return &priv->domain;

/* [한국어] 할당 실패 경로. priv가 NULL이지만 kfree(NULL)은 안전하므로
 * 이 형태가 성립한다 — 다소 우회적이지만 원본 그대로 둔다. */
fail_nomem:
	kfree(priv);	/* [한국어] priv가 NULL이어도 kfree는 안전하므로 이 형태가 성립한다. */
	return NULL;	/* [한국어] 도메인을 만들지 못했음을 코어에 알린다. */
}

/*
 * [한국어]
 * msm_iommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 왜 io-pgtable을 해제하지 않는가: 이 드라이버에서 free_io_pgtable_ops()는
 * msm_iommu_identity_attach()가 호출한다. 즉 "기본 도메인으로 돌아갈 때"
 * 페이지 테이블이 해제되고, 이 함수는 껍데기만 반납한다.
 * 도메인이 attach된 적 없이 해제되면 io-pgtable도 없으므로 문제가 없지만,
 * attach된 채로 곧장 free되면 페이지 테이블이 누수될 여지가 있다 —
 * 코어가 항상 detach를 먼저 수행한다는 전제에 기대고 있다.
 *
 * 전역 락을 잡는 이유: 이 도메인이 여전히 어느 IOMMU의 list_attached에
 * 매달려 있을 수 있어, 순회 중인 다른 경로와 경쟁하지 않게 하려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free → [msm_iommu_domain_free]
 */
static void msm_iommu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 해제할 도메인 상태. */
	struct msm_priv *priv;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] 다른 경로가 이 도메인을 순회하는 중이 아님을 보장한다. */
	spin_lock_irqsave(&msm_iommu_lock, flags);
	/* [한국어] 일반 포인터를 이 드라이버의 상태로 복원한다. */
	priv = to_msm_priv(domain);
	/* [한국어] 도메인 상태를 반납한다. io-pgtable은 detach가 이미
	 * 해제했다는 전제다. */
	kfree(priv);
	spin_unlock_irqrestore(&msm_iommu_lock, flags);	/* [한국어] 도메인 반납이 끝났으니 락을 푼다. */
}

/*
 * [한국어]
 * msm_iommu_domain_config - 도메인용 io-pgtable을 생성한다
 *
 * @priv: 대상 도메인 상태. priv->dev가 미리 설정되어 있어야 한다.
 * @return: 0 성공, -EINVAL(io-pgtable 생성 실패).
 *
 * 왜 attach 시점인가: alloc_io_pgtable_ops()는 DMA 마스터 디바이스
 * (cfg.iommu_dev)를 요구한다. 도메인 생성 시점에는 그것을 알 수 없으므로
 * 첫 attach까지 미룬다.
 *
 * 설정값의 의미:
 *  - pgsize_bitmap: 도메인이 광고한 네 가지 페이지 크기를 그대로 전달.
 *  - ias = oas = 32: 입력/출력 주소 모두 32비트. ARMv7S 형식의 한계다.
 *  - tlb: 이 파일의 무효화 콜백들. io-pgtable이 테이블을 고칠 때마다
 *    여기로 되돌아와 하드웨어 TLB를 비운다.
 *  - iommu_dev: DMA 일관성 처리에 쓸 디바이스.
 * 쿠키로 priv를 넘기므로, 콜백들이 cookie를 msm_priv로 되돌려 쓸 수 있다.
 *
 * 주의: 이 함수가 attach마다 호출되어 매번 새 io-pgtable을 만든다.
 * 같은 도메인에 두 번째 디바이스를 붙이면 앞서 만든 테이블이 누수되고
 * priv->iop이 새 것으로 덮인다 — 기존 코드의 결함이며 사실만 기록한다.
 *
 * 실행 컨텍스트: attach 경로. 아직 msm_iommu_lock을 잡기 전이라
 * 잠들 수 있는 할당이 허용된다.
 *
 * 호출 체인:
 *   msm_iommu_attach_dev() → [msm_iommu_domain_config]
 *   → alloc_io_pgtable_ops(ARM_V7S, ...)
 */
static int msm_iommu_domain_config(struct msm_priv *priv)
{
	/* [한국어] 페이지 테이블 접근을 직렬화할 도메인별 락을 초기화한다. */
	spin_lock_init(&priv->pgtlock);

	/* [한국어] io-pgtable에 넘길 설정을 구조체 통째 대입으로 채운다.
	 * 명시하지 않은 필드는 0이 되며, 그 뒤 alloc이 ttbr/tcr/prrr/nmrr 같은
	 * 출력 필드를 채워 돌려준다. */
	priv->cfg = (struct io_pgtable_cfg) {
		/* [한국어] 도메인이 광고한 페이지 크기 집합을 그대로 전달한다. */
		.pgsize_bitmap = priv->domain.pgsize_bitmap,
		/* [한국어] 입력 주소 폭 32비트 — IOVA 공간이 4GB라는 뜻이다. */
		.ias = 32,
		/* [한국어] 출력 주소 폭 32비트 — 물리 주소도 4GB 이내여야 한다. */
		.oas = 32,
		/* [한국어] 테이블이 바뀔 때 하드웨어 TLB를 비울 콜백들.
		 * 이것이 io-pgtable과 이 드라이버를 잇는 되돌아오는 경로다. */
		.tlb = &msm_iommu_flush_ops,
		/* [한국어] 테이블 메모리의 DMA 일관성 처리에 쓸 디바이스. */
		.iommu_dev = priv->dev,
	};

	/* [한국어] ARMv7 short-descriptor 형식의 페이지 테이블을 만든다.
	 * 마지막 인자 priv가 쿠키로 저장되어, 나중에 TLB 콜백의 cookie로 되돌아온다. */
	priv->iop = alloc_io_pgtable_ops(ARM_V7S, &priv->cfg, priv);
	/* [한국어] 생성 실패 — 설정이 형식과 맞지 않거나 메모리가 부족한 경우다. */
	if (!priv->iop) {
		dev_err(priv->dev, "Failed to allocate pgtable\n");	/* [한국어] 설정이 형식과 맞지 않거나 메모리가 부족한 경우다. */
		return -EINVAL;	/* [한국어] 페이지 테이블 없이는 도메인을 쓸 수 없다. */
	}

	/* [한국어] 이제 priv->cfg에 ttbr 등이 채워졌으므로 __program_context()가
	 * 그 값을 하드웨어에 쓸 수 있다. */
	return 0;
}

/* Must be called under msm_iommu_lock */
/*
 * [한국어]
 * find_iommu_for_dev - 이 디바이스를 담당하는 IOMMU 인스턴스를 찾는다
 *
 * @dev: 찾을 대상 디바이스.
 * @return: 담당 IOMMU 포인터, 없으면 NULL.
 *
 * 어떻게 찾는가: 각 IOMMU의 ctx_list에서 **첫 번째** 마스터만 꺼내
 * 그 of_node가 이 디바이스의 of_node와 같은지 본다. 즉 "IOMMU 하나에
 * 마스터 하나"라는 전제가 깔려 있다 — insert_iommu_master()가 리스트가
 * 비어 있을 때만 새 마스터를 만드는 것과 짝을 이루는 설계다.
 *
 * 원본 주석대로 반드시 msm_iommu_lock을 쥐고 불러야 한다. 리스트를
 * 순회하는 동안 probe나 of_xlate가 항목을 추가할 수 있기 때문이다.
 *
 * 실행 컨텍스트: probe_device 경로. 락 보유 상태.
 *
 * 호출 체인:
 *   msm_iommu_probe_device() → [find_iommu_for_dev]
 */
static struct msm_iommu_dev *find_iommu_for_dev(struct device *dev)
{
	/* [한국어] 순회 커서와 결과. 찾지 못하면 NULL이 그대로 반환된다. */
	struct msm_iommu_dev *iommu, *ret = NULL;
	/* [한국어] 각 IOMMU의 첫 번째 마스터. */
	struct msm_iommu_ctx_dev *master;

	/* [한국어] 시스템의 모든 MSM IOMMU를 순회한다. */
	list_for_each_entry(iommu, &qcom_iommu_devices, dev_node) {
		/* [한국어] 이 IOMMU의 첫 마스터를 꺼낸다. 리스트가 비어 있으면
		 * 잘못된 포인터가 나오지만, of_xlate가 항상 마스터를 하나는
		 * 만들어 두므로 성립한다. */
		master = list_first_entry(&iommu->ctx_list,
					  struct msm_iommu_ctx_dev,
					  list);
		/* [한국어] 그 마스터가 이 디바이스의 것인지 of_node로 비교한다. */
		if (master->of_node == dev->of_node) {
			ret = iommu;	/* [한국어] of_node가 일치하는 IOMMU를 찾았다. */
			break;
		}
	}

	/* [한국어] 찾은 IOMMU 또는 NULL. */
	return ret;
}

/*
 * [한국어]
 * msm_iommu_probe_device - 이 디바이스를 담당할 IOMMU를 코어에 알린다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 IOMMU의 iommu_device 핸들, 담당자가 없으면 ERR_PTR(-ENODEV).
 *
 * 순서에 주목: of_xlate가 먼저 실행되어 마스터를 만들어 두었어야 이
 * 함수가 IOMMU를 찾을 수 있다. iommus 프로퍼티가 없는 디바이스는
 * 마스터가 만들어지지 않아 여기서 -ENODEV가 된다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iommu_probe_device() → iommu_ops->probe_device
 *   → [msm_iommu_probe_device] → find_iommu_for_dev()
 */
static struct iommu_device *msm_iommu_probe_device(struct device *dev)
{
	/* [한국어] 찾아낸 담당 IOMMU. */
	struct msm_iommu_dev *iommu;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] 리스트 순회를 보호한다. 검색만 락 안에서 하고, 결과 판정은
	 * 밖에서 하는 것이 락 보유 시간을 줄이는 방법이다. */
	spin_lock_irqsave(&msm_iommu_lock, flags);
	iommu = find_iommu_for_dev(dev);	/* [한국어] 락 안에서 검색만 수행해 락 보유 시간을 줄인다. */
	spin_unlock_irqrestore(&msm_iommu_lock, flags);

	/* [한국어] 담당자가 없다면 이 디바이스는 IOMMU를 쓰지 않는다. */
	if (!iommu)
		return ERR_PTR(-ENODEV);

	/* [한국어] 담당 IOMMU의 코어 핸들을 돌려주면, 코어가 이 디바이스를
	 * 그 IOMMU 소속으로 등록한다. */
	return &iommu->iommu;
}

/*
 * [한국어]
 * msm_iommu_attach_dev - 디바이스를 도메인에 붙이고 컨텍스트 뱅크를 프로그래밍한다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙일 디바이스.
 * @old: 직전 도메인(사용하지 않는다).
 * @return: 0 성공, -EEXIST(이미 뱅크가 할당됨), -ENODEV(빈 뱅크 없음),
 *          클록 활성화 실패 시 그 errno.
 *
 * 동작 과정:
 *  1) priv->dev를 갱신하고 io-pgtable을 만든다(domain_config).
 *  2) 전역 락을 잡고 모든 IOMMU를 순회하며 이 디바이스의 of_node와
 *     일치하는 것을 찾는다.
 *  3) 찾으면 클록을 켜고, 그 IOMMU의 각 마스터에 대해:
 *     - 이미 뱅크가 할당되어 있으면(master->num != 0) -EEXIST로 거부한다.
 *     - 빈 뱅크를 할당하고, config_mids()로 MID를 그 뱅크에 연결하고,
 *       __program_context()로 페이지 테이블과 동작 설정을 써 넣는다.
 *  4) 클록을 끄고 이 IOMMU를 도메인의 list_attached에 매단다.
 *
 * -EEXIST 검사의 허점: 뱅크 0번은 유효한 번호인데도 "할당 안 됨"으로
 * 취급된다(master->num이 0이면 통과한다). 즉 0번 뱅크를 쓰는 마스터는
 * 중복 attach를 걸러 내지 못한다 — 기존 코드의 결함이다.
 *
 * 에러 경로의 한계: fail 레이블이 락 해제만 하고, 이미 할당한 뱅크나
 * 켜 둔 클록을 되돌리지 않는다. 실패 시 클록 참조가 새어 나간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. domain_config()는 락 밖에서
 * (잠들 수 있게) 호출하고, 하드웨어 조작만 락 안에서 한다.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev → [msm_iommu_attach_dev]
 *   → msm_iommu_domain_config(), msm_iommu_alloc_ctx(), config_mids(),
 *     __program_context()
 */
static int msm_iommu_attach_dev(struct iommu_domain *domain, struct device *dev,
				struct iommu_domain *old)
{
	/* [한국어] 결과 코드. 아무 IOMMU도 매칭되지 않으면 0인 채로 반환된다
	 * — 즉 "붙일 것이 없어도 성공"으로 취급한다. */
	int ret = 0;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 순회할 IOMMU 인스턴스. */
	struct msm_iommu_dev *iommu;
	/* [한국어] 붙일 도메인의 상태. */
	struct msm_priv *priv = to_msm_priv(domain);
	/* [한국어] 마스터(디바이스의 MID 목록과 뱅크 번호를 가진 구조체). */
	struct msm_iommu_ctx_dev *master;

	/* [한국어] io-pgtable 생성에 쓸 대표 디바이스를 기록한다. */
	priv->dev = dev;
	/* [한국어] 이 도메인의 페이지 테이블을 만든다. 반환값을 검사하지 않는
	 * 것은 기존 코드의 허점으로, 실패해도 아래에서 그대로 진행한다. */
	msm_iommu_domain_config(priv);

	/* [한국어] 리스트 순회와 하드웨어 조작을 직렬화한다. */
	spin_lock_irqsave(&msm_iommu_lock, flags);
	/* [한국어] 시스템의 모든 IOMMU를 순회하며 이 디바이스의 담당자를 찾는다. */
	list_for_each_entry(iommu, &qcom_iommu_devices, dev_node) {
		/* [한국어] 각 IOMMU의 첫 마스터를 꺼내 of_node를 비교한다
		 * (find_iommu_for_dev와 같은 방식). */
		master = list_first_entry(&iommu->ctx_list,
					  struct msm_iommu_ctx_dev,
					  list);
		/* [한국어] 이 IOMMU가 그 디바이스를 담당한다면 설정을 시작한다. */
		if (master->of_node == dev->of_node) {
			/* [한국어] 레지스터를 만지려면 클록이 필요하다. */
			ret = __enable_clocks(iommu);
			if (ret)	/* [한국어] 클록 실패 시 이 IOMMU의 설정을 진행할 수 없다. */
				goto fail;

			/* [한국어] 이 IOMMU에 속한 모든 마스터를 순회한다.
			 * 위에서 꺼낸 master 변수를 루프 커서로 재사용하는 점에 주의. */
			list_for_each_entry(master, &iommu->ctx_list, list) {
				/* [한국어] 이미 뱅크가 할당된 마스터라면 중복 attach다.
				 * 다만 num이 0인 경우를 구분하지 못하는 허점이 있다. */
				if (master->num) {
					dev_err(dev, "domain already attached");	/* [한국어] 뱅크가 이미 할당된 마스터라 중복 attach로 판단한다. */
					ret = -EEXIST;	/* [한국어] 같은 마스터를 두 번 붙일 수는 없다. */
					goto fail;	/* [한국어] 락을 풀러 간다. */
				}
				/* [한국어] 빈 컨텍스트 뱅크를 하나 확보한다.
				 * 0번부터 ncb-1까지가 탐색 범위다. */
				master->num =
					msm_iommu_alloc_ctx(iommu->context_map,
							    0, iommu->ncb);
				/* [한국어] 음수 오류값이 반환됐는지 확인한다.
				 * IS_ERR_VALUE를 쓰는 이유는 num이 부호 없는 타입일 수
				 * 있어 단순 음수 비교가 통하지 않기 때문이다. */
				if (IS_ERR_VALUE(master->num)) {
					ret = -ENODEV;	/* [한국어] 빈 컨텍스트 뱅크가 남지 않았다는 뜻이다. */
					goto fail;	/* [한국어] 뱅크 고갈이므로 attach를 포기한다. */
				}
				/* [한국어] 이 마스터의 MID들을 방금 확보한 뱅크에 연결한다. */
				config_mids(iommu, master);
				/* [한국어] 그 뱅크에 페이지 테이블과 동작 설정을 써 넣고
				 * MMU를 켠다. 이 호출이 끝나면 변환이 시작된다. */
				__program_context(iommu->base, master->num,
						  priv);
			}
			/* [한국어] 하드웨어 설정이 끝났으니 클록을 끈다. */
			__disable_clocks(iommu);
			/* [한국어] 이 IOMMU를 도메인의 목록에 매단다 — 이후
			 * TLB 무효화 콜백이 이 목록을 순회한다. */
			list_add(&iommu->dom_node, &priv->list_attached);
		}
	}

/* [한국어] 정상 종료와 모든 실패가 모이는 지점. 락만 풀고, 이미 확보한
 * 뱅크나 켜 둔 클록은 되돌리지 않는다(기존 코드의 한계). */
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);

	/* [한국어] 성공이면 0, 아니면 중단을 유발한 오류를 반환한다. */
	return ret;
}

/*
 * [한국어]
 * msm_iommu_identity_attach - 디바이스를 항등 도메인으로 되돌린다(= detach)
 *
 * @identity_domain: 정적 항등 도메인.
 * @dev: 대상 디바이스(사용하지 않는다 — 도메인 단위로 처리한다).
 * @old: 직전 도메인. 이 함수의 실제 작업 대상이다.
 * @return: 0 성공, 클록 활성화 실패 시 그 errno.
 *
 * 왜 이것이 detach인가: 이 드라이버에는 별도의 detach 콜백이 없다.
 * "기본(항등) 도메인으로 돌아간다"는 요청이 곧 detach이며, 그 처리가
 * 여기 모여 있다. 하는 일은 세 가지다 —
 *  1) 옛 도메인의 io-pgtable을 해제한다(페이지 테이블 메모리 반납).
 *  2) 각 컨텍스트 뱅크를 반납하고(비트맵에서 지우고),
 *  3) __reset_context()로 그 뱅크의 MMU를 꺼 변환을 멈춘다.
 *
 * 첫 검사의 의미: old가 없거나 이미 항등 도메인이면 되돌릴 것이 없다.
 * 이 검사가 없으면 to_msm_priv(identity_domain)로 엉뚱한 메모리를 해석하게 된다.
 *
 * 주의할 점: master->num을 0으로 되돌리지 않는다. attach의 중복 검사가
 * master->num을 보므로, 한 번 뱅크를 받았던 마스터는 detach 후에도
 * "이미 attach됨"으로 보일 수 있다 — 기존 코드의 결함이다.
 * 또 list_attached에서 IOMMU를 빼지도 않아, 해제된 도메인이 여전히
 * 목록에 남는다.
 *
 * free_io_pgtable_ops()를 락 밖에서 부르는 이유: 그 안에서 페이지를
 * 해제하며 잠들 수 있기 때문이다. 하드웨어 조작만 락 안에서 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_detach_device()/도메인 전환 → domain_ops->attach_dev
 *   → [msm_iommu_identity_attach] → free_io_pgtable_ops(),
 *     msm_iommu_free_ctx(), __reset_context()
 */
static int msm_iommu_identity_attach(struct iommu_domain *identity_domain,
				     struct device *dev,
				     struct iommu_domain *old)
{
	/* [한국어] 정리할 옛 도메인의 상태. */
	struct msm_priv *priv;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 순회할 IOMMU. */
	struct msm_iommu_dev *iommu;
	/* [한국어] 각 IOMMU의 마스터. */
	struct msm_iommu_ctx_dev *master;
	/* [한국어] 클록 활성화 결과. */
	int ret = 0;

	/* [한국어] 이미 항등 상태이거나 붙은 적이 없으면 할 일이 없다.
	 * 이 검사가 없으면 아래 to_msm_priv()가 항등 도메인을 msm_priv로
	 * 잘못 해석해 메모리를 망가뜨린다. */
	if (old == identity_domain || !old)
		return 0;

	/* [한국어] 옛 도메인의 상태를 복원한다. */
	priv = to_msm_priv(old);
	/* [한국어] 페이지 테이블을 통째로 반납한다. 락을 잡기 전에 하는 이유는
	 * 이 호출이 잠들 수 있기 때문이다. */
	free_io_pgtable_ops(priv->iop);

	/* [한국어] 하드웨어 조작과 리스트 순회를 직렬화한다. */
	spin_lock_irqsave(&msm_iommu_lock, flags);
	/* [한국어] 이 도메인이 붙어 있던 모든 IOMMU를 순회한다. */
	list_for_each_entry(iommu, &priv->list_attached, dom_node) {
		/* [한국어] 레지스터를 만지려면 클록이 필요하다. */
		ret = __enable_clocks(iommu);
		if (ret)	/* [한국어] 클록을 켜지 못하면 뱅크를 초기화할 수 없다. */
			goto fail;

		/* [한국어] 이 IOMMU에 속한 각 마스터의 뱅크를 반납하고 초기화한다. */
		list_for_each_entry(master, &iommu->ctx_list, list) {
			/* [한국어] 소프트웨어 장부에서 뱅크를 비운다 — 다음 attach가
			 * 이 뱅크를 다시 쓸 수 있게 된다. */
			msm_iommu_free_ctx(iommu->context_map, master->num);
			/* [한국어] 하드웨어에서도 그 뱅크를 초기화한다. 특히
			 * SCTLR을 0으로 만들어 MMU를 꺼야 그 MID의 DMA가 멈춘다. */
			__reset_context(iommu->base, master->num);
		}
		/* [한국어] 이 IOMMU의 작업이 끝났으니 클록을 끈다. */
		__disable_clocks(iommu);
	}
/* [한국어] 정상 종료와 클록 실패가 모이는 지점. */
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);
	/* [한국어] 성공이면 0, 클록 실패면 그 오류. */
	return ret;
}

/* [한국어] 항등 도메인의 연산 테이블. attach_dev 하나뿐인 이유는 이 도메인이
 * 상태 없이 "변환 없음"만 뜻하기 때문이다. */
static struct iommu_domain_ops msm_iommu_identity_ops = {
	/* [한국어] 옛 도메인을 정리하고 뱅크의 MMU를 끄는 콜백. */
	.attach_dev = msm_iommu_identity_attach,
};

/* [한국어] 정적 항등 도메인. 상태가 없어 인스턴스 하나를 모두가 공유한다.
 * iommu_ops의 .identity_domain으로 등록되어, 코어의 "IOMMU 우회" 요청을
 * 이 도메인으로 표현한다. */
static struct iommu_domain msm_iommu_identity_domain = {
	/* [한국어] 코어가 항등 도메인임을 알아보는 종류 표시. */
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 위에서 정의한 콜백 하나짜리 테이블. */
	.ops = &msm_iommu_identity_ops,
};

/*
 * [한국어]
 * msm_iommu_map - IOVA 구간에 물리 페이지들을 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑 시작 IOVA.
 * @pa: 물리 주소 시작.
 * @pgsize: 페이지 크기(MSM_IOMMU_PGSIZES 중 하나).
 * @pgcount: 매핑할 페이지 개수.
 * @prot: 보호 플래그 — io-pgtable이 해석한다.
 * @gfp: 코어가 준 할당 플래그 — **여기서는 무시하고 GFP_ATOMIC을 쓴다.**
 * @mapped: 출력 인자 — 매핑한 바이트 수.
 * @return: io-pgtable이 낸 결과(0 또는 음수 errno).
 *
 * 왜 gfp를 무시하고 GFP_ATOMIC을 쓰는가: 이 함수가 priv->pgtlock을
 * 스핀락으로 잡은 상태에서 io-pgtable을 부르기 때문이다. 스핀락 안에서는
 * 잠들 수 없으므로, 코어가 GFP_KERNEL을 주었더라도 GFP_ATOMIC으로
 * 바꿔 넘길 수밖에 없다. 락을 쓰는 설계가 할당 방식을 강제하는 예다.
 *
 * TLB 무효화가 없는 이유: 매핑 추가는 코어가 나중에 iotlb_sync_map을
 * 불러 처리한다(msm_iommu_sync_map 참조).
 *
 * 실행 컨텍스트: DMA API 경로. irqsave 스핀락으로 보호한다.
 *
 * 호출 체인:
 *   iommu_map() → domain_ops->map_pages → [msm_iommu_map]
 *   → io-pgtable의 map_pages
 */
static int msm_iommu_map(struct iommu_domain *domain, unsigned long iova,
			 phys_addr_t pa, size_t pgsize, size_t pgcount,
			 int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 도메인 상태를 복원한다. */
	struct msm_priv *priv = to_msm_priv(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] io-pgtable의 결과. */
	int ret;

	/* [한국어] 페이지 테이블 조작을 직렬화한다. io-pgtable 자체는 락을
	 * 갖지 않으므로 호출자가 보호해야 한다. */
	spin_lock_irqsave(&priv->pgtlock, flags);
	/* [한국어] 실제 테이블 조작은 전부 io-pgtable에 위임한다.
	 * gfp 인자 대신 GFP_ATOMIC을 넘기는 이유는 위 §2 설명 참조. */
	ret = priv->iop->map_pages(priv->iop, iova, pa, pgsize, pgcount, prot,
				   GFP_ATOMIC, mapped);
	spin_unlock_irqrestore(&priv->pgtlock, flags);

	/* [한국어] io-pgtable의 결과를 그대로 코어에 전달한다. */
	return ret;
}

/*
 * [한국어]
 * msm_iommu_sync_map - 매핑을 추가한 뒤 TLB를 무효화한다
 *
 * @domain: 대상 도메인.
 * @iova: 새로 매핑된 구간의 시작.
 * @size: 그 구간의 크기.
 * @return: 항상 0.
 *
 * 왜 매핑 추가에도 무효화가 필요한가: 하드웨어가 "이 주소는 매핑 없음"이라는
 * 결과를 TLB에 캐시해 두었을 수 있다. 새 매핑을 걸어도 그 부정적 캐시가
 * 남아 있으면 여전히 폴트가 난다.
 *
 * granule로 SZ_4K를 고정하는 점에 주목: 실제 매핑이 1MB 섹션이더라도
 * 4KB 단위로 쪼개 무효화한다. 즉 1MB 매핑이면 256번의 TLBIVA가 나간다 —
 * 정확하지만 비싼 방식이다. 하드웨어가 큰 페이지 단위 무효화를 지원하는지
 * 확신할 수 없어 가장 안전한 쪽을 택한 것으로 보인다.
 *
 * 실행 컨텍스트: 매핑 완료 후. __flush_iotlb_range가 내부에서
 * msm_iommu_lock을 잡으므로, 이 시점에는 pgtlock을 놓은 상태여야 한다
 * (코어가 map_pages와 별개의 콜백으로 부르므로 자연히 그렇게 된다).
 *
 * 호출 체인:
 *   iommu_map()의 마무리 → domain_ops->iotlb_sync_map
 *   → [msm_iommu_sync_map] → __flush_iotlb_range()
 */
static int msm_iommu_sync_map(struct iommu_domain *domain, unsigned long iova,
			      size_t size)
{
	/* [한국어] 도메인 상태를 복원한다 — 무효화 콜백의 쿠키로 쓴다. */
	struct msm_priv *priv = to_msm_priv(domain);

	/* [한국어] 4KB 단위로 쪼개 그 구간의 TLB를 무효화한다.
	 * leaf 인자는 이 하드웨어에서 무의미하다. */
	__flush_iotlb_range(iova, size, SZ_4K, false, priv);
	/* [한국어] 실패할 수 있는 동작이 없어 항상 성공을 반환한다. */
	return 0;
}

/*
 * [한국어]
 * msm_iommu_unmap - IOVA 구간의 매핑을 제거한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제 시작 IOVA.
 * @pgsize: 페이지 크기.
 * @pgcount: 해제할 페이지 개수.
 * @gather: TLB 무효화 수집 구조체 — io-pgtable에 그대로 전달한다.
 * @return: 실제로 해제한 바이트 수.
 *
 * 왜 여기서 TLB를 비우지 않는가: io-pgtable이 엔트리를 지우면서
 * tlb_add_page 콜백(__flush_iotlb_page)을 부르고, 그 안에서 즉시
 * 무효화가 일어난다. 즉 무효화가 이 함수 안에서 간접적으로 수행된다.
 * 그래서 map과 달리 별도의 sync 호출이 필요 없고, ops의 iotlb_sync가
 * NULL로 등록되어 있다.
 *
 * 실행 컨텍스트: DMA API 경로. irqsave 스핀락(pgtlock) 보유 상태에서
 * io-pgtable이 무효화 콜백을 부르고, 그것이 다시 msm_iommu_lock을 잡는다 —
 * 락 순서 pgtlock → msm_iommu_lock이 여기서 성립한다.
 *
 * 호출 체인:
 *   iommu_unmap() → domain_ops->unmap_pages → [msm_iommu_unmap]
 *   → io-pgtable의 unmap_pages → __flush_iotlb_page()
 */
static size_t msm_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			      size_t pgsize, size_t pgcount,
			      struct iommu_iotlb_gather *gather)
{
	/* [한국어] 도메인 상태를 복원한다. */
	struct msm_priv *priv = to_msm_priv(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 해제한 바이트 수. */
	size_t ret;

	/* [한국어] 페이지 테이블 조작을 직렬화한다. */
	spin_lock_irqsave(&priv->pgtlock, flags);
	/* [한국어] 실제 해제와 그에 따른 TLB 무효화를 io-pgtable에 맡긴다. */
	ret = priv->iop->unmap_pages(priv->iop, iova, pgsize, pgcount, gather);
	spin_unlock_irqrestore(&priv->pgtlock, flags);

	/* [한국어] 해제한 크기를 코어에 알린다. */
	return ret;
}

/*
 * [한국어]
 * msm_iommu_iova_to_phys - 하드웨어의 V2P 기능으로 IOVA를 물리 주소로 바꾼다
 *
 * @domain: 대상 도메인.
 * @va: 변환할 IOVA.
 * @return: 물리 주소, 실패하면 0.
 *
 * 왜 소프트웨어 워크가 아닌가: 다른 드라이버들은 io-pgtable의
 * iova_to_phys를 부르거나 테이블을 직접 걷는다. 이 드라이버는 대신
 * 하드웨어의 V2P(Virtual to Physical) 변환 레지스터를 쓴다 —
 * V2PPR에 주소를 쓰면 하드웨어가 실제 워크를 수행하고 PAR에 결과를 남긴다.
 * 소프트웨어 워크보다 느리지만(레지스터 왕복 + 클록 켜기), 하드웨어가
 * 보는 것과 정확히 같은 결과를 얻는다는 장점이 있다.
 *
 * 동작 과정:
 *  1) 도메인의 첫 IOMMU와 첫 마스터를 꺼낸다(여러 개면 첫 것만 본다).
 *  2) 클록을 켜고, 그 뱅크의 TLB를 비운다 — 옛 결과가 섞이지 않게.
 *  3) V2PPR에 주소를 써 변환을 요청한다.
 *  4) PAR에서 결과를 읽는다.
 *  5) 슈퍼섹션(16MB)이면 상위 8비트만 PAR에서, 나머지 24비트는 VA에서 가져온다.
 *     일반 페이지면 상위 20비트가 PAR, 하위 12비트가 VA다.
 *  6) 폴트가 났으면 결과를 0으로 만든다.
 *
 * 왜 TLB를 먼저 비우는가: V2P 결과가 TLB에 남아 있던 옛 매핑을 반영하면
 * 안 되기 때문이다. 다만 이 때문에 조회 한 번이 그 뱅크의 TLB를 통째로
 * 날려 성능에 부담이 된다 — 디버깅용 경로라 감수한 설계다.
 *
 * ret 변수의 이중 용도: phys_addr_t인데 __enable_clocks()의 int 결과를
 * 받는 데도 쓰인다. 클록 실패 시 그 오류값이 물리 주소인 척 반환되는
 * 셈이지만, 곧바로 fail로 빠져 0이 아닌 값이 나갈 수 있는 허점이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msm_iommu_lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops->iova_to_phys
 *   → [msm_iommu_iova_to_phys] → SET_V2PPR(), GET_PAR()
 */
static phys_addr_t msm_iommu_iova_to_phys(struct iommu_domain *domain,
					  dma_addr_t va)
{
	/* [한국어] 도메인 상태. */
	struct msm_priv *priv;
	/* [한국어] 질의를 보낼 IOMMU(도메인의 첫 번째). */
	struct msm_iommu_dev *iommu;
	/* [한국어] 그 IOMMU의 첫 마스터 — 뱅크 번호를 얻기 위해 필요하다. */
	struct msm_iommu_ctx_dev *master;
	/* [한국어] PAR 레지스터에서 읽은 원시 변환 결과. */
	unsigned int par;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 최종 물리 주소. 0으로 시작해 실패 시 그대로 반환된다. */
	phys_addr_t ret = 0;

	/* [한국어] 하드웨어 접근과 리스트 순회를 직렬화한다. */
	spin_lock_irqsave(&msm_iommu_lock, flags);

	/* [한국어] 도메인 상태를 복원한다. */
	priv = to_msm_priv(domain);
	/* [한국어] 이 도메인이 붙어 있는 첫 IOMMU를 꺼낸다. 여러 개여도
	 * 같은 페이지 테이블을 쓰므로 어느 것에 물어도 결과가 같다. */
	iommu = list_first_entry(&priv->list_attached,
				 struct msm_iommu_dev, dom_node);

	/* [한국어] 마스터가 없으면 뱅크 번호를 알 수 없어 질의할 수 없다. */
	if (list_empty(&iommu->ctx_list))
		goto fail;

	/* [한국어] 첫 마스터를 꺼내 그 뱅크 번호를 쓴다. */
	master = list_first_entry(&iommu->ctx_list,
				  struct msm_iommu_ctx_dev, list);
	/* [한국어] 방어적 NULL 검사 — list_empty를 이미 확인했으므로
	 * 실제로는 도달하지 않는다. */
	if (!master)
		goto fail;

	/* [한국어] 레지스터 접근을 위해 클록을 켠다. 반환형이 phys_addr_t인
	 * ret에 int 결과를 담는 것은 타입상 어색하지만 원본 그대로다. */
	ret = __enable_clocks(iommu);
	if (ret)	/* [한국어] 클록 실패 — 레지스터를 읽을 수 없으니 조회를 포기한다. */
		goto fail;

	/* Invalidate context TLB */
	/* [한국어] 이 뱅크의 TLB를 비운다 — V2P 결과가 옛 캐시를 반영하지
	 * 않게 하려는 것이다. 조회 비용이 커지는 대신 정확성을 얻는다. */
	SET_CTX_TLBIALL(iommu->base, master->num, 0);
	/* [한국어] V2PPR(권한 있는 읽기 변환) 레지스터에 주소를 써 변환을
	 * 요청한다. V2Pxx_VA 마스크로 주소 부분만 남긴다. */
	SET_V2PPR(iommu->base, master->num, va & V2Pxx_VA);

	/* [한국어] 하드웨어가 남긴 변환 결과를 읽는다. */
	par = GET_PAR(iommu->base, master->num);

	/* We are dealing with a supersection */
	/* [한국어] 결과가 슈퍼섹션(16MB)인지 확인한다. 그렇다면 페이지 내
	 * 오프셋이 24비트이므로, PAR에서는 상위 8비트만 쓰고 나머지는
	 * 원래 주소에서 가져와야 한다. */
	if (GET_NOFAULT_SS(iommu->base, master->num))
		ret = (par & 0xFF000000) | (va & 0x00FFFFFF);
	else	/* Upper 20 bits from PAR, lower 12 from VA */
		/* [한국어] 일반 페이지(4KB 단위)라면 상위 20비트가 물리 주소,
		 * 하위 12비트가 페이지 내 오프셋이다. */
		ret = (par & 0xFFFFF000) | (va & 0x00000FFF);

	/* [한국어] 변환 도중 폴트가 났다면 위에서 계산한 값은 무의미하므로
	 * 0(=매핑 없음)으로 되돌린다. */
	if (GET_FAULT(iommu->base, master->num))
		ret = 0;

	/* [한국어] 질의가 끝났으니 클록을 끈다. */
	__disable_clocks(iommu);
/* [한국어] 모든 실패 경로가 모이는 지점. ret이 0이거나 클록 오류값이다. */
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);
	/* [한국어] 계산된 물리 주소 또는 0. */
	return ret;
}

/*
 * [한국어]
 * print_ctx_regs - 폴트가 난 컨텍스트 뱅크의 주요 레지스터를 덤프한다
 *
 * @base: IOMMU MMIO 베이스.
 * @ctx: 덤프할 뱅크 번호.
 * @return: 없음.
 *
 * 왜 필요한가: 이 IOMMU는 폴트 정보를 큐가 아니라 레지스터에 남긴다.
 * 폴트가 나면 그 뱅크의 FSR/FAR/PAR 등을 읽어야 무엇이 잘못됐는지 알 수
 * 있고, 이 함수가 그것을 사람이 읽을 수 있는 형태로 출력한다.
 *
 * FSR 비트 해독이 핵심이다. 각 비트가 폴트의 원인을 나타내며, 여러 개가
 * 동시에 설 수 있어 문자열을 이어 붙이는 방식으로 출력한다:
 *  TF(0x02)      — Translation Fault: 매핑이 없다. 가장 흔하다.
 *  AFF(0x04)     — Access Flag Fault: AF 비트가 서 있지 않다.
 *  APF(0x08)     — Access Permission Fault: 권한이 없다(읽기 전용에 쓰기 등).
 *  TLBMF(0x10)   — TLB Match Fault: TLB 엔트리가 중복 매칭됐다.
 *  HTWDEEF(0x20) — 하드웨어 워크 중 데이터 외부 오류.
 *  HTWSEEF(0x40) — 하드웨어 워크 중 동기 외부 오류.
 *  MHF(0x80)     — Multi-Hit Fault.
 *  SL(0x10000)   — Second Level: 2단계 테이블에서 난 폴트다.
 *  SS(0x40000000)— Stall Started: CFCFG 설정에 따라 접근이 멈춰 있다.
 *  MULTI(0x8000_0000) — 여러 폴트가 겹쳤다(앞선 폴트를 처리하기 전에 또 났다).
 *
 * 실행 컨텍스트: 폴트 핸들러(스레드 IRQ). pr_err를 쓰므로 콘솔이 느리면
 * 그만큼 지연되지만, 폴트는 드물어야 정상이라 감수한다.
 *
 * 호출 체인:
 *   msm_iommu_fault_handler() → [print_ctx_regs] → GET_* 매크로들
 */
static void print_ctx_regs(void __iomem *base, int ctx)
{
	/* [한국어] 폴트 상태 레지스터를 한 번만 읽어 아래에서 비트별로 해석한다.
	 * 여러 번 읽으면 그 사이에 값이 바뀔 수 있다. */
	unsigned int fsr = GET_FSR(base, ctx);
	/* [한국어] FAR(폴트를 일으킨 가상 주소)와 PAR(변환 결과)를 함께 출력한다. */
	pr_err("FAR    = %08x    PAR    = %08x\n",
	       GET_FAR(base, ctx), GET_PAR(base, ctx));
	/* [한국어] FSR을 원시 값과 함께, 선 비트들의 이름을 나열해 출력한다.
	 * 각 삼항 연산자가 해당 비트가 서 있으면 이름을, 아니면 빈 문자열을
	 * 내놓아 자연스러운 목록이 만들어진다. */
	pr_err("FSR    = %08x [%s%s%s%s%s%s%s%s%s%s]\n", fsr,
			/* [한국어] TF — 변환 폴트(매핑 없음). */
			(fsr & 0x02) ? "TF " : "",
			/* [한국어] AFF — Access Flag 폴트. */
			(fsr & 0x04) ? "AFF " : "",
			/* [한국어] APF — 접근 권한 폴트. */
			(fsr & 0x08) ? "APF " : "",
			/* [한국어] TLBMF — TLB 매치 폴트. */
			(fsr & 0x10) ? "TLBMF " : "",
			/* [한국어] HTWDEEF — 하드웨어 워크 중 데이터 외부 오류. */
			(fsr & 0x20) ? "HTWDEEF " : "",
			/* [한국어] HTWSEEF — 하드웨어 워크 중 동기 외부 오류. */
			(fsr & 0x40) ? "HTWSEEF " : "",
			/* [한국어] MHF — 다중 히트 폴트. */
			(fsr & 0x80) ? "MHF " : "",
			/* [한국어] SL — 2단계 테이블에서 난 폴트임을 표시. */
			(fsr & 0x10000) ? "SL " : "",
			/* [한국어] SS — 접근이 멈춰(stall) 있다. CFCFG=1 설정의 결과다. */
			(fsr & 0x40000000) ? "SS " : "",
			/* [한국어] MULTI — 처리 전에 폴트가 또 발생해 정보가 겹쳤다. */
			(fsr & 0x80000000) ? "MULTI " : "");

	/* [한국어] FSYNR0/1 — 폴트 부가 정보(어느 마스터, 어떤 종류의 접근이었는지). */
	pr_err("FSYNR0 = %08x    FSYNR1 = %08x\n",
	       GET_FSYNR0(base, ctx), GET_FSYNR1(base, ctx));
	/* [한국어] TTBR0/1 — 그 시점에 설정되어 있던 페이지 테이블 주소.
	 * 0이면 뱅크가 제대로 프로그래밍되지 않았다는 뜻이다. */
	pr_err("TTBR0  = %08x    TTBR1  = %08x\n",
	       GET_TTBR0(base, ctx), GET_TTBR1(base, ctx));
	/* [한국어] SCTLR/ACTLR — 뱅크의 제어 설정. MMU가 켜져 있었는지 확인할 수 있다. */
	pr_err("SCTLR  = %08x    ACTLR  = %08x\n",
	       GET_SCTLR(base, ctx), GET_ACTLR(base, ctx));
}

/*
 * [한국어]
 * insert_iommu_master - 디바이스의 마스터 항목을 만들고 MID를 쌓는다
 *
 * @dev: 대상 디바이스.
 * @iommu: 담당 IOMMU(포인터의 포인터로 받지만 값을 바꾸지는 않는다).
 * @spec: 디바이스 트리에서 파싱된 iommus 항목. spec->args[0]이 MID다.
 * @return: 0 성공, -ENOMEM(마스터 할당 실패).
 *
 * 왜 리스트가 비어 있을 때만 마스터를 만드는가: 이 드라이버는 "IOMMU 하나에
 * 마스터 하나"를 전제한다(find_iommu_for_dev가 첫 항목만 보는 것과 짝을 이룬다).
 * 두 번째 이후의 iommus 항목은 새 마스터를 만들지 않고 기존 마스터의
 * mids[] 배열에 MID만 덧붙인다.
 *
 * 위험한 지점: master를 dev_iommu_priv_get(dev)로 초기화하는데, 리스트가
 * 비어 있지 않으면 그 값을 그대로 쓴다. 서로 다른 디바이스가 같은 IOMMU를
 * 공유하면 두 번째 디바이스의 priv가 NULL이라 아래에서 NULL 역참조가 난다 —
 * 기존 코드의 결함이며 여기서는 사실만 기록한다.
 *
 * 중복 MID 검사: 같은 MID가 이미 있으면 경고만 남기고 무시한다.
 * 다만 경고 메시지가 MID 값이 아니라 배열 인덱스(sid)를 출력하는
 * 사소한 버그가 있다.
 *
 * 실행 컨텍스트: of_xlate 경로. msm_iommu_lock을 쥔 상태라 GFP_ATOMIC으로
 * 할당한다.
 *
 * 호출 체인:
 *   qcom_iommu_of_xlate() → [insert_iommu_master]
 */
static int insert_iommu_master(struct device *dev,
				struct msm_iommu_dev **iommu,
				const struct of_phandle_args *spec)
{
	/* [한국어] 이 디바이스의 마스터. 이미 만들어져 있으면 priv에서 온다. */
	struct msm_iommu_ctx_dev *master = dev_iommu_priv_get(dev);
	/* [한국어] 중복 MID 검사 루프의 인덱스. */
	int sid;

	/* [한국어] 이 IOMMU에 아직 마스터가 없다면 지금 만든다.
	 * 있으면 위에서 priv로 가져온 것을 그대로 쓴다. */
	if (list_empty(&(*iommu)->ctx_list)) {
		/* [한국어] 스핀락을 쥔 상태라 GFP_ATOMIC으로 할당한다. */
		master = kzalloc_obj(*master, GFP_ATOMIC);
		if (!master) {	/* [한국어] GFP_ATOMIC 할당이라 메모리 압박 시 실패할 수 있다. */
			dev_err(dev, "Failed to allocate iommu_master\n");	/* [한국어] 마스터를 만들지 못했음을 남긴다. */
			return -ENOMEM;	/* [한국어] 이 디바이스는 IOMMU에 등록될 수 없다. */
		}
		/* [한국어] 이 마스터가 어느 디바이스의 것인지 of_node로 기록한다.
		 * find_iommu_for_dev()가 이 값으로 매칭한다. */
		master->of_node = dev->of_node;
		/* [한국어] IOMMU의 마스터 목록에 매단다. */
		list_add(&master->list, &(*iommu)->ctx_list);
		/* [한국어] 디바이스의 priv에도 저장해 두 번째 iommus 항목에서
		 * 다시 찾을 수 있게 한다. */
		dev_iommu_priv_set(dev, master);
	}

	/* [한국어] 이미 등록된 MID인지 확인한다. 디바이스 트리에 같은 값이
	 * 두 번 적히는 실수를 걸러 내기 위함이다. */
	for (sid = 0; sid < master->num_mids; sid++)
		if (master->mids[sid] == spec->args[0]) {
			/* [한국어] 경고 메시지가 MID 값 대신 인덱스를 출력하는
			 * 사소한 버그가 있지만 원본 그대로 둔다. */
			dev_warn(dev, "Stream ID 0x%x repeated; ignoring\n",
				 sid);
			return 0;	/* [한국어] 중복 MID는 무시하고 성공으로 처리한다. */
		}

	/* [한국어] 새 MID를 배열에 추가하고 개수를 늘린다. 배열 크기 검사가
	 * 없어 mids[]를 넘칠 수 있다는 점도 기존 코드의 한계다. */
	master->mids[master->num_mids++] = spec->args[0];
	return 0;	/* [한국어] MID 등록 완료. */
}

/*
 * [한국어]
 * qcom_iommu_of_xlate - 디바이스 트리의 iommus 프로퍼티를 해석한다
 *
 * @dev: iommus 프로퍼티를 가진 디바이스.
 * @spec: 파싱된 항목. spec->np가 IOMMU 노드, spec->args[0]이 MID다.
 * @return: 0 성공, -ENODEV(그 노드에 해당하는 IOMMU를 찾지 못함).
 *
 * 왜 필요한가: 코어가 `iommus = <&iommu MID>` 같은 프로퍼티를 만나면
 * 이 콜백을 부른다. 여기서 phandle이 가리키는 IOMMU를 찾고,
 * insert_iommu_master()로 그 MID를 마스터에 등록한다.
 * 프로퍼티에 항목이 여럿이면 이 콜백이 여러 번 불려 MID가 쌓인다.
 *
 * probe와의 순서: 이 콜백이 성공하려면 그 IOMMU가 이미 probe되어
 * qcom_iommu_devices 리스트에 있어야 한다. 아직이면 -ENODEV가 나고,
 * 코어가 나중에 다시 시도한다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로. 전역 락을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   of_iommu_configure() → iommu_ops->of_xlate → [qcom_iommu_of_xlate]
 *   → insert_iommu_master()
 */
static int qcom_iommu_of_xlate(struct device *dev,
			       const struct of_phandle_args *spec)
{
	/* [한국어] 찾아낸 IOMMU와 순회 커서. */
	struct msm_iommu_dev *iommu = NULL, *iter;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 결과 코드. */
	int ret = 0;

	/* [한국어] 리스트 순회와 마스터 등록을 직렬화한다. */
	spin_lock_irqsave(&msm_iommu_lock, flags);
	/* [한국어] phandle이 가리키는 노드와 of_node가 일치하는 IOMMU를 찾는다. */
	list_for_each_entry(iter, &qcom_iommu_devices, dev_node) {
		if (iter->dev->of_node == spec->np) {	/* [한국어] phandle이 가리키는 노드와 일치하는 IOMMU인지 본다. */
			iommu = iter;	/* [한국어] 담당 IOMMU를 찾았으니 순회를 멈춘다. */
			break;
		}
	}

	/* [한국어] 아직 그 IOMMU가 probe되지 않았다면 등록할 수 없다.
	 * 코어가 나중에 다시 시도하게 된다. */
	if (!iommu) {
		ret = -ENODEV;	/* [한국어] 그 IOMMU가 아직 probe되지 않았다는 뜻이다. */
		goto fail;	/* [한국어] 락을 풀러 간다 — 코어가 나중에 다시 시도한다. */
	}

	/* [한국어] 찾은 IOMMU에 이 디바이스의 MID를 등록한다. */
	ret = insert_iommu_master(dev, &iommu, spec);
/* [한국어] 성공과 실패가 모이는 지점 — 락만 풀면 된다. */
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);

	/* [한국어] 등록 결과를 코어에 전달한다. */
	return ret;
}

/*
 * [한국어]
 * msm_iommu_fault_handler - 컨텍스트 폴트 인터럽트 핸들러
 *
 * @irq: 발생한 IRQ 번호(사용하지 않는다).
 * @dev_id: request_irq에 넘긴 msm_iommu_dev 포인터.
 * @return: 0을 반환한다 — 원래 irqreturn_t는 IRQ_HANDLED(1)나
 *          IRQ_NONE(0)이어야 하므로, 이 값은 IRQ_NONE에 해당한다.
 *          공유 IRQ(IRQF_SHARED)로 등록되어 있어 다른 핸들러도 시도되는데,
 *          폴트를 실제로 처리했음에도 NONE을 반환하는 것은 기존 코드의
 *          결함으로 보인다(처리되지 않은 인터럽트로 집계된다).
 *
 * 동작 과정:
 *  1) 전역 락을 잡는다(irqsave가 아닌 spin_lock인 점에 주의 —
 *     스레드 IRQ라 인터럽트가 이미 활성화된 컨텍스트이기 때문이다).
 *  2) 클록을 켠다.
 *  3) 모든 컨텍스트 뱅크의 FSR을 훑어, 0이 아닌 뱅크마다 레지스터를
 *     덤프하고 FSR에 0x4000000F를 써서 폴트를 지운다.
 *  4) 클록을 끄고 락을 푼다.
 *
 * FSR에 0x4000000F를 쓰는 의미: FSR은 write-1-to-clear 방식이라
 * 세워진 비트에 1을 쓰면 지워진다. 0xF는 하위 폴트 비트들을,
 * 0x40000000은 SS(stall) 비트를 지운다. SS를 지워야 CFCFG=1로 멈춰 있던
 * 접근이 재개된다 — 이것을 빼먹으면 그 마스터가 영원히 멈춘다.
 *
 * report_iommu_fault()를 부르지 않는 점에 주목: 상위 계층에 폴트를
 * 알리지 않고 로그만 남긴다. 그래서 드라이버가 폴트 핸들러를 등록해도
 * 호출되지 않는다.
 *
 * static이 아닌 이유: msm_iommu.h가 이 심볼을 선언하고 있어 외부에서
 * 참조될 수 있게 열어 둔 것으로 보인다(현재는 이 파일 안에서만 쓰인다).
 *
 * 실행 컨텍스트: 스레드 IRQ(IRQF_ONESHOT). 잠들 수 있지만 스핀락을
 * 잡고 있어 그 구간에서는 잠들면 안 된다.
 *
 * 호출 체인:
 *   하드웨어 IRQ → 스레드 핸들러 → [msm_iommu_fault_handler]
 *   → print_ctx_regs(), SET_FSR()
 */
irqreturn_t msm_iommu_fault_handler(int irq, void *dev_id)
{
	/* [한국어] request_irq에 넘긴 IOMMU 인스턴스. */
	struct msm_iommu_dev *iommu = dev_id;
	/* [한국어] 각 뱅크의 폴트 상태. */
	unsigned int fsr;
	/* [한국어] 뱅크 순회 인덱스와 클록 결과. */
	int i, ret;

	/* [한국어] 하드웨어 접근을 직렬화한다. 스레드 컨텍스트라 irqsave가
	 * 필요 없다고 판단한 것으로 보인다. */
	spin_lock(&msm_iommu_lock);

	/* [한국어] dev_id가 NULL이면 등록이 잘못된 것이다 — 방어적 검사. */
	if (!iommu) {
		pr_err("Invalid device ID in context interrupt handler\n");	/* [한국어] 등록이 잘못되어 dev_id가 비어 있는 비정상 상황이다. */
		goto fail;	/* [한국어] 더 진행할 수 없으니 락을 풀러 간다. */
	}

	/* [한국어] 폴트가 났다는 사실 자체를 먼저 알린다. "Unexpected"인 이유는
	 * 이 드라이버가 페이지 폴트를 정상 동작으로 다루지 않기 때문이다. */
	pr_err("Unexpected IOMMU page fault!\n");
	/* [한국어] 어느 IOMMU인지 베이스 주소로 식별할 수 있게 출력한다.
	 * __iomem 포인터를 unsigned int로 캐스팅하는 것은 32비트 전용
	 * 코드라 가능한 방식이다. */
	pr_err("base = %08x\n", (unsigned int)iommu->base);

	/* [한국어] 레지스터를 읽으려면 클록이 필요하다. */
	ret = __enable_clocks(iommu);
	if (ret)	/* [한국어] 클록을 켜지 못하면 폴트 레지스터를 읽을 수 없다. */
		goto fail;

	/* [한국어] 모든 컨텍스트 뱅크를 훑어 폴트가 난 것을 찾는다.
	 * 여러 뱅크가 동시에 폴트를 낼 수 있으므로 전부 확인한다. */
	for (i = 0; i < iommu->ncb; i++) {
		/* [한국어] 이 뱅크의 폴트 상태를 읽는다. */
		fsr = GET_FSR(iommu->base, i);
		/* [한국어] 0이 아니면 이 뱅크에서 폴트가 났다는 뜻이다. */
		if (fsr) {
			pr_err("Fault occurred in context %d.\n", i);	/* [한국어] 어느 뱅크에서 폴트가 났는지 먼저 알린다. */
			pr_err("Interesting registers:\n");
			/* [한국어] FSR 비트 해독과 주요 레지스터를 덤프한다. */
			print_ctx_regs(iommu->base, i);
			/* [한국어] 폴트 비트들과 stall 비트(0x40000000)를 지운다.
			 * stall을 지워야 CFCFG=1로 멈춰 있던 마스터가 재개된다. */
			SET_FSR(iommu->base, i, 0x4000000F);
		}
	}
	/* [한국어] 모든 뱅크를 확인했으니 클록을 끈다. */
	__disable_clocks(iommu);
/* [한국어] 정상 종료와 모든 실패가 모이는 지점. */
fail:
	spin_unlock(&msm_iommu_lock);
	/* [한국어] 0(=IRQ_NONE)을 반환한다. 폴트를 처리했음에도 NONE인 것은
	 * 기존 코드의 문제로, 공유 IRQ 통계에서 미처리로 잡힌다. */
	return 0;
}

/* [한국어] IOMMU 코어에 노출하는 이 드라이버의 연산 테이블. */
static struct iommu_ops msm_iommu_ops = {
	/* [한국어] "변환 없음" 상태를 표현하는 정적 항등 도메인. detach가
	 * 이 도메인으로의 전환으로 구현되어 있다. */
	.identity_domain = &msm_iommu_identity_domain,
	/* [한국어] 페이징 도메인 생성 — 껍데기만 만들고 io-pgtable은
	 * attach 시점에 만든다. */
	.domain_alloc_paging = msm_iommu_domain_alloc_paging,
	/* [한국어] 디바이스 담당 판정 — of_node로 IOMMU를 찾는다. */
	.probe_device = msm_iommu_probe_device,
	/* [한국어] 코어의 범용 그룹 헬퍼 — 디바이스마다 개별 그룹을 만든다. */
	.device_group = generic_device_group,
	/* [한국어] iommus 프로퍼티 해석 — MID를 마스터에 쌓는다. */
	.of_xlate = qcom_iommu_of_xlate,
	/* [한국어] 페이징 도메인의 연산 테이블(익명 const 구조체). */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		/* [한국어] 컨텍스트 뱅크를 할당하고 하드웨어를 프로그래밍한다. */
		.attach_dev	= msm_iommu_attach_dev,
		/* [한국어] io-pgtable에 매핑을 위임한다. */
		.map_pages	= msm_iommu_map,
		/* [한국어] io-pgtable에 해제를 위임한다(무효화는 콜백으로 즉시 처리). */
		.unmap_pages	= msm_iommu_unmap,
		/*
		 * Nothing is needed here, the barrier to guarantee
		 * completion of the tlb sync operation is implicitly
		 * taken care when the iommu client does a writel before
		 * kick starting the other master.
		 */
		/* [한국어] unmap 후 무효화 완료를 기다리는 콜백을 등록하지 않는다.
		 * 원본 주석의 근거: 클라이언트 드라이버가 다른 마스터를 기동하기
		 * 전에 반드시 writel을 수행하는데, 그 MMIO 쓰기가 배리어 역할을
		 * 해 앞선 TLB 무효화가 완료되도록 보장한다는 것이다.
		 * 즉 동기화를 클라이언트의 관행에 의존하는 설계다. */
		.iotlb_sync	= NULL,
		/* [한국어] 매핑 추가 후에는 명시적 무효화가 필요하다 —
		 * 부정적 캐시(매핑 없음)를 지워야 하기 때문이다. */
		.iotlb_sync_map	= msm_iommu_sync_map,
		/* [한국어] 하드웨어 V2P 기능으로 주소를 조회한다. */
		.iova_to_phys	= msm_iommu_iova_to_phys,
		/* [한국어] 도메인 구조체만 반납한다(io-pgtable은 detach가 해제). */
		.free		= msm_iommu_domain_free,
	}
};

/*
 * [한국어]
 * msm_iommu_probe - IOMMU 플랫폼 디바이스를 초기화한다
 *
 * @pdev: 디바이스 트리에서 매칭된 IOMMU 디바이스.
 * @return: 0 성공, 음수 errno(각 단계 실패).
 *
 * 동작 과정:
 *  1) 인스턴스 할당과 마스터 리스트 초기화.
 *  2) 두 클록(smmu_pclk, iommu_clk)을 확보한다. devm_clk_get_prepared를
 *     쓰는 이유는 prepare까지 미리 해 두어 이후 clk_enable()이 잠들지
 *     않게 하기 위함이다 — 스핀락 안에서 클록을 켜야 하므로 필수적이다.
 *  3) MMIO 매핑과 IRQ 번호 확보.
 *  4) 디바이스 트리의 "qcom,ncb"로 컨텍스트 뱅크 개수를 읽는다.
 *  5) 하드웨어를 리셋한 뒤 **동작 확인 테스트**를 수행한다(아래 참조).
 *  6) 스레드 IRQ 핸들러를 등록한다.
 *  7) 전역 리스트에 등록하고, sysfs와 IOMMU 코어에 등록한다.
 *
 * 5번의 동작 확인이 흥미롭다: 0번 뱅크의 MMU를 잠깐 켜고 V2P 변환을
 * 시도한 뒤 PAR를 읽는다. 페이지 테이블조차 없는 상태이므로 변환은
 * 실패하겠지만, PAR에 무언가 값이 들어와야 하드웨어가 살아 있는 것이다.
 * PAR가 0이면 IOMMU가 응답하지 않는다는 뜻이라 probe를 실패시킨다.
 * 테스트가 끝나면 V2PCFG와 M을 되돌려 원래 상태로 만든다.
 *
 * 에러 경로의 특징: goto 되감기가 없고 각 실패가 곧바로 반환한다.
 * devm_* 계열로 확보한 자원(메모리, ioremap, 클록, IRQ)은 자동 해제되므로
 * 대부분 문제가 없지만, 전역 리스트에 추가한 뒤 실패하면 그 항목이
 * 남는다는 허점이 있다.
 *
 * 실행 컨텍스트: 디바이스 probe(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   플랫폼 버스 매칭 → driver->probe → [msm_iommu_probe]
 *   → devm_clk_get_prepared(), devm_ioremap_resource(), msm_iommu_reset(),
 *     devm_request_threaded_irq(), iommu_device_register()
 */
static int msm_iommu_probe(struct platform_device *pdev)
{
	/* [한국어] MMIO 자원 서술자. */
	struct resource *r;
	/* [한국어] sysfs 이름에 쓸 물리 베이스 주소. */
	resource_size_t ioaddr;
	/* [한국어] 만들 IOMMU 인스턴스. */
	struct msm_iommu_dev *iommu;
	/* [한국어] ret은 결과, par는 동작 확인용 PAR 값, val은 ncb 임시 저장. */
	int ret, par, val;

	/* [한국어] 인스턴스를 0으로 초기화해 할당한다. devm이라 자동 해제된다. */
	iommu = devm_kzalloc(&pdev->dev, sizeof(*iommu), GFP_KERNEL);
	/* [한국어] 메모리 부족 — -ENOMEM이 아니라 -ENODEV를 반환하는 것은
	 * 원본 그대로다. */
	if (!iommu)
		return -ENODEV;

	/* [한국어] 로깅과 devm 자원의 소유자로 쓸 device 포인터를 보관한다. */
	iommu->dev = &pdev->dev;
	/* [한국어] 이 IOMMU에 붙을 마스터들의 리스트 헤드를 초기화한다. */
	INIT_LIST_HEAD(&iommu->ctx_list);

	/* [한국어] 레지스터 인터페이스 클록을 얻고 prepare까지 해 둔다.
	 * prepare를 미리 하는 것이 중요한데, 이후 clk_enable()이 스핀락 안에서
	 * 불리므로 잠들면 안 되기 때문이다. */
	iommu->pclk = devm_clk_get_prepared(iommu->dev, "smmu_pclk");
	/* [한국어] 실패 시 dev_err_probe로 로그와 반환값을 한 번에 처리한다
	 * (-EPROBE_DEFER면 로그를 억제해 준다). */
	if (IS_ERR(iommu->pclk))
		return dev_err_probe(iommu->dev, PTR_ERR(iommu->pclk),
				     "could not get smmu_pclk\n");

	/* [한국어] IOMMU 코어 로직 클록도 마찬가지로 얻어 prepare 한다.
	 * 이름과 달리 optional이 아니라 필수로 요구한다. */
	iommu->clk = devm_clk_get_prepared(iommu->dev, "iommu_clk");
	if (IS_ERR(iommu->clk))	/* [한국어] 코어 클록을 얻지 못하면 이 IOMMU를 쓸 수 없다. */
		return dev_err_probe(iommu->dev, PTR_ERR(iommu->clk),
				     "could not get iommu_clk\n");

	/* [한국어] 디바이스 트리의 첫 reg 항목을 가져온다. */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	/* [한국어] 레지스터 블록을 매핑한다. devm이라 자동 언매핑된다. */
	iommu->base = devm_ioremap_resource(iommu->dev, r);
	/* [한국어] 매핑 실패 — 자원이 없거나 이미 점유되었다. */
	if (IS_ERR(iommu->base)) {
		ret = dev_err_probe(iommu->dev, PTR_ERR(iommu->base), "could not get iommu base\n");	/* [한국어] ioremap 실패 원인을 로그와 반환값으로 함께 처리한다. */
		return ret;	/* [한국어] 매핑 없이는 아무 레지스터도 만질 수 없다. */
	}
	/* [한국어] sysfs 이름에 쓸 물리 주소를 따로 보관한다. */
	ioaddr = r->start;

	/* [한국어] 폴트 인터럽트 번호를 가져온다. */
	iommu->irq = platform_get_irq(pdev, 0);
	/* [한국어] 음수면 인터럽트가 없다는 뜻이다. 원래 오류값 대신
	 * -ENODEV로 바꿔 반환하므로 -EPROBE_DEFER가 소실될 수 있다. */
	if (iommu->irq < 0)
		return -ENODEV;

	/* [한국어] 이 IOMMU가 가진 컨텍스트 뱅크의 개수를 디바이스 트리에서 읽는다.
	 * 하드웨어에서 알아낼 방법이 없어 트리에 적어 두는 방식이다. */
	ret = of_property_read_u32(iommu->dev->of_node, "qcom,ncb", &val);
	if (ret) {	/* [한국어] qcom,ncb 프로퍼티가 없거나 형식이 잘못됐다. */
		dev_err(iommu->dev, "could not get ncb\n");	/* [한국어] 뱅크 개수를 모르면 초기화 범위를 정할 수 없다. */
		return ret;	/* [한국어] 디바이스 트리 오류를 그대로 전달한다. */
	}
	/* [한국어] 뱅크 개수를 보관한다. 뱅크 할당과 폴트 순회의 상한이 된다. */
	iommu->ncb = val;

	/* [한국어] 부트로더가 남긴 설정과 매핑을 전부 지운다. */
	msm_iommu_reset(iommu->base, iommu->ncb);
	/* [한국어] 아래 다섯 줄이 하드웨어 동작 확인 테스트다.
	 * 먼저 0번 뱅크의 MMU를 켠다. */
	SET_M(iommu->base, 0, 1);
	/* [한국어] PAR를 0으로 지워 테스트 결과와 구분할 수 있게 한다. */
	SET_PAR(iommu->base, 0, 0);
	/* [한국어] V2P를 활성화한다(값 1). */
	SET_V2PCFG(iommu->base, 0, 1);
	/* [한국어] 주소 0에 대한 V2P 변환을 요청한다. 페이지 테이블이 없으므로
	 * 변환은 실패하지만, 하드웨어가 살아 있다면 PAR에 무언가를 남긴다. */
	SET_V2PPR(iommu->base, 0, 0);
	/* [한국어] 결과를 읽는다. 이 값이 0이면 하드웨어가 응답하지 않은 것이다. */
	par = GET_PAR(iommu->base, 0);
	/* [한국어] 테스트가 끝났으니 V2P를 되돌린다. */
	SET_V2PCFG(iommu->base, 0, 0);
	/* [한국어] MMU도 다시 끈다 — 실제 attach 전까지는 꺼져 있어야 한다. */
	SET_M(iommu->base, 0, 0);

	/* [한국어] PAR가 0이면 IOMMU가 동작하지 않는다고 판단해 probe를 포기한다.
	 * 존재하지 않거나 전원이 없는 하드웨어를 걸러 내는 실용적인 검사다. */
	if (!par) {
		pr_err("Invalid PAR value detected\n");	/* [한국어] 하드웨어가 응답하지 않는다 — 전원이 없거나 존재하지 않는 IOMMU다. */
		return -ENODEV;	/* [한국어] 동작하지 않는 하드웨어이므로 probe를 포기한다. */
	}

	/* [한국어] 폴트 핸들러를 스레드 IRQ로 등록한다.
	 * 상위(하드 IRQ) 핸들러를 NULL로 두면 커널이 항상 스레드를 깨우고,
	 * IRQF_ONESHOT이 그 동안 IRQ를 마스크해 준다. 핸들러가 pr_err로
	 * 여러 줄을 출력하므로 하드 IRQ에서 처리하기에는 무겁기 때문이다.
	 * IRQF_SHARED는 이 IRQ 라인을 다른 IOMMU 인스턴스와 공유할 수 있어서다. */
	ret = devm_request_threaded_irq(iommu->dev, iommu->irq, NULL,
					msm_iommu_fault_handler,
					IRQF_ONESHOT | IRQF_SHARED,
					"msm_iommu_secure_irpt_handler",
					iommu);
	if (ret) {	/* [한국어] IRQ 등록 실패 — 폴트를 감지할 수 없게 된다. */
		pr_err("Request IRQ %d failed with ret=%d\n", iommu->irq, ret);	/* [한국어] 어느 IRQ가 왜 실패했는지 남긴다. */
		return ret;	/* [한국어] 폴트 처리 없이 운용할 수 없으므로 실패로 끝낸다. */
	}

	/* [한국어] 전역 리스트에 등록한다 — 이 시점부터 of_xlate와
	 * probe_device가 이 IOMMU를 찾을 수 있다.
	 * 락 없이 추가하는 점에 주의: probe는 직렬화된다는 전제다. */
	list_add(&iommu->dev_node, &qcom_iommu_devices);

	/* [한국어] sysfs에 이 IOMMU를 노출한다. 이름에 물리 주소를 넣어
	 * 여러 인스턴스를 구분한다(%pa가 phys_addr_t를 출력한다). */
	ret = iommu_device_sysfs_add(&iommu->iommu, iommu->dev, NULL,
				     "msm-smmu.%pa", &ioaddr);
	if (ret) {	/* [한국어] sysfs 등록 실패. */
		pr_err("Could not add msm-smmu at %pa to sysfs\n", &ioaddr);	/* [한국어] 어느 주소의 IOMMU가 실패했는지 남긴다. */
		return ret;	/* [한국어] 등록 실패를 상위에 전달한다. */
	}

	/* [한국어] IOMMU 코어에 연산 테이블을 등록한다. 이 시점부터
	 * probe_device 콜백이 들어올 수 있다. */
	ret = iommu_device_register(&iommu->iommu, &msm_iommu_ops, &pdev->dev);
	if (ret) {	/* [한국어] 코어 등록 실패. */
		pr_err("Could not register msm-smmu at %pa\n", &ioaddr);	/* [한국어] 어느 주소의 IOMMU가 실패했는지 남긴다. */
		return ret;	/* [한국어] 코어에 등록되지 못했으므로 실패로 끝낸다. */
	}

	/* [한국어] 초기화 성공을 알리는 정보 로그. 뱅크 개수까지 남겨
	 * 부팅 로그만으로 구성을 확인할 수 있게 한다. */
	pr_info("device mapped at %p, irq %d with %d ctx banks\n",
		iommu->base, iommu->irq, iommu->ncb);

	/* [한국어] 여기까지 왔으면 ret은 0이다. */
	return ret;
}

/* [한국어] 이 드라이버가 바인딩할 디바이스 트리 compatible 목록.
 * APQ8064 하나만 지원하는데, 이 세대의 IOMMU를 쓰는 SoC가 사실상
 * 그것뿐이기 때문이다. */
static const struct of_device_id msm_iommu_dt_match[] = {
	/* [한국어] Qualcomm APQ8064의 IOMMU. */
	{ .compatible = "qcom,apq8064-iommu" },
	/* [한국어] 배열 끝을 알리는 빈 항목. */
	{}
};

/* [한국어] 플랫폼 버스에 등록할 드라이버 정의.
 * remove 콜백이 없는 점에 주목 — 아래 builtin_platform_driver로
 * 빌트인 전용이라 언로드가 없고, suppress_bind_attrs도 없지만
 * 실질적으로 제거 경로가 쓰이지 않는다는 전제다. */
static struct platform_driver msm_iommu_driver = {
	/* [한국어] 드라이버 공통 정보. */
	.driver = {
		/* [한국어] sysfs와 로그에 나타날 이름. */
		.name	= "msm_iommu",
		/* [한국어] 디바이스 트리 매칭 테이블. */
		.of_match_table = msm_iommu_dt_match,
	},
	/* [한국어] 초기화 진입점. */
	.probe		= msm_iommu_probe,
};
/* [한국어] 모듈이 아니라 커널에 내장되는 드라이버로 등록한다.
 * module_platform_driver와 달리 exit 경로를 만들지 않으므로 언로드가
 * 불가능하다 — IOMMU를 임의로 제거하면 클라이언트의 DMA 매핑이 통째로
 * 무효화되어 시스템이 손상되기 때문에 합리적인 선택이다. */
builtin_platform_driver(msm_iommu_driver);
