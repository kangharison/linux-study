// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU API for s390 PCI devices
 *
 * Copyright IBM Corp. 2015
 * Author(s): Gerald Schaefer <gerald.schaefer@de.ibm.com>
 */

/*
 * [한국어 설명] IBM Z(s390) PCI 디바이스용 IOMMU 드라이버 (s390-iommu.c)
 *
 * === 파일의 역할 ===
 * IBM Z 메인프레임의 PCI 디바이스(zPCI)에 대한 DMA 주소 변환을 리눅스
 * IOMMU API에 붙이는 드라이버다. 다른 아키텍처와 근본적으로 다른 점이
 * 하나 있다: **레지스터를 직접 만지지 않는다.** s390에서는 IOMMU가
 * 펌웨어(밀리코드)와 하이퍼바이저 뒤에 있어, 모든 하드웨어 조작이
 * zpci_register_ioat() / zpci_refresh_trans() 같은 명령 래퍼를 통해
 * 이뤄진다. 이 파일이 하는 일은 메모리상의 변환 테이블을 만들고
 * 그 위치를 펌웨어에 등록한 뒤, 갱신할 때마다 무효화 명령을 내는 것이다.
 *
 * 구조를 이해하는 데 필요한 개념이 넷이다.
 *
 * (1) **가변 단계 테이블**. 최대 5단계(RF → RS → RT → ST → PT)이지만,
 *     필요한 만큼만 쓴다. 도메인을 만들 때 aperture 크기를 보고
 *     origin_type을 RTX(3단계)/RSX(4단계)/RFX(5단계) 중에서 고른다.
 *     그래서 워크 함수들이 origin_type에 따라 시작점을 달리한다.
 *
 * (2) **RCU로 보호되는 디바이스 목록**. 무효화는 도메인에 붙은 모든
 *     디바이스에 대해 각각 명령을 내야 하는데, 그 순회가 매핑 경로에서
 *     자주 일어난다. 그래서 목록을 RCU로 읽어 락 경합을 없앴다.
 *
 * (3) **차단(blocked) 도메인이 기본**. probe_device가 디바이스를 곧바로
 *     blocking_domain에 넣어 DMA를 막고 시작한다. attach도 항상
 *     "먼저 차단하고 그다음 새 도메인 등록" 순서를 밟는데, 등록이
 *     실패해도 DMA가 열려 있지 않게 하려는 안전 설계다.
 *
 * (4) **shadow_on_flush와 지연 무효화**. 가상화 환경(zdev->tlb_refresh)에서는
 *     하이퍼바이저가 게스트의 테이블을 그림자 복사하므로, 매핑을 추가한
 *     뒤에도 명시적으로 알려야 한다. 반대로 해제는 gather로 모아
 *     한 번에 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [arch/s390/pci] zpci 디바이스 생성 → zpci_init_iommu()
 *        ↓
 *   [이 파일] IOMMU 코어에 등록 → probe_device가 차단 도메인으로 시작
 *
 *   [디바이스 드라이버] dma_map_*()
 *        ↓
 *   [dma-iommu] IOVA 할당
 *        ↓
 *   [이 파일] s390_iommu_map_pages() → 5단계 테이블을 걸어 PTE 기록
 *        ↓ iotlb_sync_map
 *   [이 파일] zpci_refresh_trans() → 펌웨어/하이퍼바이저에 알림
 *        ↓
 *   [밀리코드/하이퍼바이저] 실제 변환 수행
 *
 * 두 벌의 ops가 있는 것도 특징이다: rtr_avail(리셋 후 재등록 지원)
 * 하드웨어에서만 identity 도메인을 제공한다. 그렇지 않은 구형에서는
 * 통과 모드를 안전하게 오갈 수 없기 때문이다.
 *
 * 실행 컨텍스트: map/unmap은 dma-iommu 경로라 atomic일 수 있고,
 * 테이블 설치는 cmpxchg로 락 없이 처리한다. 디바이스 목록 갱신만
 * 스핀락(irqsave)을 쓰고, 읽기는 RCU다.
 *
 * === 타 모듈과의 연결 ===
 * - asm/pci_dma.h: ZPCI_* 상수 전부와 zpci_register_ioat(),
 *   zpci_refresh_trans() 같은 펌웨어 명령 래퍼.
 * - arch/s390/pci/pci.c 등: zpci_init_iommu()/destroy_iommu()와
 *   zpci_iommu_register_ioat()를 호출하는 쪽. 이 파일은 s390 PCI
 *   서브시스템의 일부로 동작한다.
 * - dma-iommu.h: iommu_dma_forcedac 등 DMA 계층과의 접점.
 * - linux/rculist.h: 디바이스 목록의 RCU 순회.
 * 데이터 흐름: zpci_dev의 start_dma/end_dma가 이 디바이스가 쓸 수 있는
 * IOVA 범위를 정한다 → 도메인 생성 시 그 범위에 맞는 테이블 종류를
 * 고른다 → attach가 테이블 주소를 IOTA로 조립해 펌웨어에 등록한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct s390_domain: 도메인 하나. 최상위 테이블, 붙어 있는 디바이스
 *   목록(RCU), 통계 카운터, 그리고 테이블 종류(origin_type).
 * - dma_walk_cpu_trans(): IOVA에 해당하는 PTE를 찾는다(없으면 중간
 *   테이블을 만든다). 각 단계가 cmpxchg로 경쟁을 처리한다.
 * - s390_domain_alloc_paging(): aperture 크기를 보고 몇 단계 테이블을
 *   쓸지 결정한다 — 이 드라이버에서 가장 특징적인 판단이다.
 * - s390_iommu_attach_device(): 차단 → IOTA 등록 → 목록 추가 순서.
 * - s390_iommu_iotlb_sync_map()/iotlb_sync(): 붙어 있는 모든 디바이스에
 *   무효화 명령을 낸다.
 * - reg_ioat_propagate_error(): 등록 실패 중 "실패로 다루지 않아도 되는"
 *   두 경우를 걸러 낸다.
 */

/* [한국어] dev_is_pci(), to_zpci_dev() 등 PCI 계층 접근자.
 * 이 IOMMU는 PCI 디바이스만 다룬다. */
#include <linux/pci.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, iommu_domain, resv region API. */
#include <linux/iommu.h>
/* [한국어] IOMMU 보조 함수들. 현재 직접 쓰이지는 않는다. */
#include <linux/iommu-helper.h>
/* [한국어] SZ_4K — 이 IOMMU가 지원하는 유일한 페이지 크기. */
#include <linux/sizes.h>
/* [한국어] list_for_each_entry_rcu(), list_add_rcu() 등 RCU 리스트 API.
 * 무효화 경로가 디바이스 목록을 락 없이 순회하는 근거다. */
#include <linux/rculist.h>
/* [한국어] rcu_read_lock(), call_rcu() — 도메인 해제를 유예하는 데 쓴다. */
#include <linux/rcupdate.h>
/* [한국어] ZPCI_* 상수와 zpci_register_ioat()/zpci_refresh_trans() 등
 * 펌웨어 명령 래퍼. 이 드라이버의 하드웨어 지식이 전부 여기서 온다. */
#include <asm/pci_dma.h>

/* [한국어] iommu_dma_forcedac 등 DMA 계층과의 접점.
 * s390은 32비트 DMA 주소를 쓰지 않으므로 init에서 forcedac을 켠다. */
#include "dma-iommu.h"

/* [한국어] 두 벌의 연산 테이블에 대한 전방 선언. 파일 맨 끝에 정의되지만
 * zpci_init_iommu()가 그보다 앞에서 참조한다.
 * 두 벌인 이유: rtr(리셋 후 재등록)을 지원하는 하드웨어에서만
 * identity 도메인을 제공하기 때문이다. */
static const struct iommu_ops s390_iommu_ops, s390_iommu_rtr_ops;

/* [한국어] 영역 테이블(RF/RS/RT/ST)용 슬랩 캐시.
 * 설정자: dma_alloc_cpu_table_caches()가 부팅 시 생성.
 * 읽는 자: dma_alloc_cpu_table()/free_cpu_table().
 * 왜 전용 캐시인가: 테이블이 ZPCI_TABLE_ALIGN 정렬을 요구하는데,
 *                   kmalloc으로는 보장할 수 없기 때문이다. */
static struct kmem_cache *dma_region_table_cache;
/* [한국어] 페이지 테이블(PT)용 슬랩 캐시.
 * 영역 테이블과 크기/정렬이 달라 별도 캐시를 둔다.
 * 설정자/읽는 자: 위와 동일한 구조. */
static struct kmem_cache *dma_page_table_cache;

/* [한국어] 이 시스템에서 IOMMU가 덮을 수 있는 최대 주소 범위.
 * 설정자: s390_iommu_init()이 high_memory의 물리 주소로 초기화하고,
 *          아래 factor를 곱한다.
 * 읽는 자: s390_domain_alloc_paging()이 aperture 크기를 정할 때.
 * 왜 high_memory 기준인가: 시스템 메모리 전체를 덮으면 충분하다는
 *                          가정이다. factor로 늘릴 수 있다. */
static u64 s390_iommu_aperture;
/* [한국어] 위 aperture에 곱할 배수. 커널 파라미터
 * s390_iommu_aperture=N으로 지정한다.
 * 설정자: s390_iommu_aperture_setup().
 * 값 범위: 기본 1. 0을 주면 aperture가 ULONG_MAX(무제한)가 된다 —
 *          더 많은 IOVA 공간이 필요한 워크로드를 위한 탈출구다. */
static u32 s390_iommu_aperture_factor = 1;

/* [한국어] IOMMU 도메인 하나의 상태.
 * 수명: domain_alloc_paging에서 만들어져, domain_free가 RCU 유예 뒤
 *       해제한다(테이블 정리가 그때 이뤄진다). */
struct s390_domain {
	struct iommu_domain	domain;
	/* [한국어] IOMMU 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging이 pgsize_bitmap과 geometry를 채운다.
	 * 값 범위: 페이지 크기는 4KB 하나, aperture 끝은 테이블 종류가 정한다. */

	struct list_head	devices;
	/* [한국어] 이 도메인에 붙어 있는 zpci 디바이스들의 목록.
	 * 설정자: attach가 list_add_rcu로 추가, blocking attach가
	 *          list_del_rcu로 제거한다.
	 * 읽는 자: 무효화 함수 세 개가 RCU 읽기 구역에서 순회한다.
	 * 왜 RCU인가: 무효화가 매핑마다 일어나 순회가 매우 잦은데,
	 *             목록 변경(attach/detach)은 드물다 — RCU에 이상적인 패턴이다.
	 * 동기화: 갱신은 list_lock 스핀락, 읽기는 rcu_read_lock. */

	struct zpci_iommu_ctrs	ctrs;
	/* [한국어] 이 도메인의 통계 카운터(매핑/해제 페이지 수, 무효화 횟수).
	 * 설정자: map/unmap과 무효화 함수들이 atomic64로 증가시킨다.
	 * 읽는 자: zpci_get_iommu_ctrs()를 통해 s390 PCI 서브시스템이
	 *          sysfs 등에 노출한다.
	 * 동기화: atomic64 연산이라 별도 락이 없다. */

	unsigned long		*dma_table;
	/* [한국어] 최상위 변환 테이블의 커널 가상 주소.
	 * 설정자: domain_alloc_paging의 dma_alloc_cpu_table().
	 * 읽는 자: 모든 워크 경로의 출발점이자, attach가 IOTA로 조립해
	 *          펌웨어에 등록하는 값이다.
	 * 왜 종류가 안 적혀 있나: 이 테이블이 RF/RS/RT 중 무엇인지는
	 *                         origin_type이 알려 준다. */

	spinlock_t		list_lock;
	/* [한국어] devices 목록의 **갱신**을 보호하는 스핀락.
	 * 설정자: domain_alloc_paging이 초기화.
	 * 읽는 자: attach와 blocking attach가 irqsave로 잡는다.
	 * 읽기 경로는 이 락을 잡지 않는다 — RCU가 대신한다. */

	struct rcu_head		rcu;
	/* [한국어] 도메인 해제를 RCU 유예 뒤로 미루는 데 쓰는 헤드.
	 * 설정자/읽는 자: domain_free가 call_rcu에 넘긴다.
	 * 왜 필요한가: 무효화 함수들이 RCU 읽기 구역에서 이 도메인의
	 *              devices 목록을 순회 중일 수 있어, 곧바로 해제하면
	 *              use-after-free가 된다. */

	u8			origin_type;
	/* [한국어] 최상위 테이블의 종류 — ZPCI_TABLE_TYPE_RFX/RSX/RTX 중 하나.
	 * 설정자: domain_alloc_paging이 aperture 크기와 하드웨어 지원
	 *          여부(zdev->dtsm)를 보고 결정한다.
	 * 읽는 자: 워크 함수들이 어느 단계에서 시작할지, 그리고
	 *          get_iota_region_flag()가 IOTA에 어떤 플래그를 넣을지.
	 * 값의 의미: RTX면 3단계(RT→ST→PT), RSX면 4단계, RFX면 5단계다.
	 *            작은 aperture에는 얕은 테이블을 써 워크를 줄인다. */
};

/* [한국어] 모든 디바이스가 공유하는 정적 차단 도메인의 전방 선언.
 * 정의는 파일 끝에 있고, probe_device와 attach가 그보다 앞에서
 * 참조하므로 선언이 필요하다.
 * 이 도메인에 붙으면 그 디바이스의 DMA가 전면 차단된다. */
static struct iommu_domain blocking_domain;

/*
 * [한국어]
 * calc_rfx - IOVA에서 1단계(RF, region-first) 인덱스를 뽑는다
 *
 * @ptr: 대상 IOVA.
 * @return: RF 테이블의 인덱스.
 *
 * 아래 다섯 개의 calc_* 함수가 IOVA를 단계별 인덱스로 쪼갠다.
 * RF → RS → RT → ST → PT 순으로 시프트가 작아지며,
 * 마지막 PT만 마스크가 다르다(테이블 크기가 다르기 때문).
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   dma_walk_rf_table() / get_rso_from_iova() → [calc_rfx]
 */
static inline unsigned int calc_rfx(dma_addr_t ptr)
{
	/* [한국어] RF 단계의 비트 구간을 뽑는다. 인덱스 마스크는
	 * 영역 테이블 공통(2048 엔트리)이다. */
	return ((unsigned long)ptr >> ZPCI_RF_SHIFT) & ZPCI_INDEX_MASK;
}

/*
 * [한국어]
 * calc_rsx - IOVA에서 2단계(RS, region-second) 인덱스를 뽑는다
 *
 * @ptr: 대상 IOVA.
 * @return: RS 테이블의 인덱스.
 *
 * 실행 컨텍스트: 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   dma_walk_rs_table() / get_rto_from_iova() → [calc_rsx]
 */
static inline unsigned int calc_rsx(dma_addr_t ptr)
{
	/* [한국어] RS 단계의 비트 구간을 뽑는다. */
	return ((unsigned long)ptr >> ZPCI_RS_SHIFT) & ZPCI_INDEX_MASK;
}

/*
 * [한국어]
 * calc_rtx - IOVA에서 3단계(RT, region-third) 인덱스를 뽑는다
 *
 * @ptr: 대상 IOVA.
 * @return: RT 테이블의 인덱스.
 *
 * RTX가 가장 얕은 origin_type이므로, 이 단계는 어떤 구성에서도
 * 반드시 거친다.
 *
 * 실행 컨텍스트: 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   dma_walk_cpu_trans() / iova_to_phys() → [calc_rtx]
 */
static inline unsigned int calc_rtx(dma_addr_t ptr)
{
	/* [한국어] RT 단계의 비트 구간을 뽑는다. */
	return ((unsigned long)ptr >> ZPCI_RT_SHIFT) & ZPCI_INDEX_MASK;
}

/*
 * [한국어]
 * calc_sx - IOVA에서 4단계(ST, segment) 인덱스를 뽑는다
 *
 * @ptr: 대상 IOVA.
 * @return: 세그먼트 테이블의 인덱스.
 *
 * 실행 컨텍스트: 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   dma_walk_cpu_trans() / iova_to_phys() → [calc_sx]
 */
static inline unsigned int calc_sx(dma_addr_t ptr)
{
	/* [한국어] 세그먼트 단계의 비트 구간을 뽑는다. */
	return ((unsigned long)ptr >> ZPCI_ST_SHIFT) & ZPCI_INDEX_MASK;
}

/*
 * [한국어]
 * calc_px - IOVA에서 마지막(PT, page) 인덱스를 뽑는다
 *
 * @ptr: 대상 IOVA.
 * @return: 페이지 테이블의 인덱스.
 *
 * 다른 단계와 달리 ZPCI_PT_MASK를 쓴다 — 페이지 테이블의 엔트리 수가
 * 영역 테이블과 다르기 때문이다.
 *
 * 실행 컨텍스트: 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   dma_walk_cpu_trans() / iova_to_phys() → [calc_px]
 */
static inline unsigned int calc_px(dma_addr_t ptr)
{
	/* [한국어] 페이지 오프셋 위의 비트를 뽑되, 페이지 테이블 전용
	 * 마스크를 쓴다. */
	return ((unsigned long)ptr >> PAGE_SHIFT) & ZPCI_PT_MASK;
}

/*
 * [한국어]
 * set_pt_pfaa - PTE에 물리 주소(PFAA)를 심는다
 *
 * @entry: 고칠 페이지 테이블 엔트리.
 * @pfaa: 매핑할 물리 주소(Page Frame Absolute Address).
 * @return: 없음.
 *
 * 플래그 비트를 보존한 채 주소 필드만 갈아 끼운다. 아래 set_* 계열이
 * 모두 같은 패턴이다 — 먼저 플래그 마스크로 걸러 내고, 새 주소를
 * OR 한 뒤, 필요하면 타입 비트를 얹는다.
 *
 * 실행 컨텍스트: PTE 갱신 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   dma_update_cpu_trans() → [set_pt_pfaa]
 */
static inline void set_pt_pfaa(unsigned long *entry, phys_addr_t pfaa)
{
	/* [한국어] 플래그 비트만 남기고 주소 부분을 지운다. */
	*entry &= ZPCI_PTE_FLAG_MASK;
	/* [한국어] 새 물리 주소를 그 자리에 넣는다. */
	*entry |= (pfaa & ZPCI_PTE_ADDR_MASK);
}

/*
 * [한국어]
 * set_rf_rso - RF 엔트리에 다음 단계(RS) 테이블 주소를 심는다
 *
 * @entry: 고칠 RF 엔트리.
 * @rso: RS 테이블의 물리 주소.
 * @return: 없음.
 *
 * 타입 비트(RFX)를 함께 세우는 점이 PTE와 다르다. 영역 테이블
 * 엔트리는 자신이 어느 단계인지를 타입 비트로 표시하고, get_* 계열이
 * 그것을 확인해 잘못된 단계의 엔트리를 따라가지 않게 한다.
 *
 * 실행 컨텍스트: 테이블 설치 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   dma_walk_rf_table() → [set_rf_rso]
 */
static inline void set_rf_rso(unsigned long *entry, phys_addr_t rso)
{
	/* [한국어] 플래그를 남기고 주소를 지운다. */
	*entry &= ZPCI_RTE_FLAG_MASK;
	/* [한국어] RS 테이블의 물리 주소를 넣는다. */
	*entry |= (rso & ZPCI_RTE_ADDR_MASK);
	/* [한국어] 이 엔트리가 RF 단계임을 타입 비트로 표시한다 —
	 * get_rf_rso()가 이 값을 확인한다. */
	*entry |= ZPCI_TABLE_TYPE_RFX;
}

/*
 * [한국어]
 * set_rs_rto - RS 엔트리에 다음 단계(RT) 테이블 주소를 심는다
 *
 * @entry: 고칠 RS 엔트리.
 * @rto: RT 테이블의 물리 주소.
 * @return: 없음.
 *
 * 실행 컨텍스트: 테이블 설치 경로.
 *
 * 호출 체인:
 *   dma_walk_rs_table() → [set_rs_rto]
 */
static inline void set_rs_rto(unsigned long *entry, phys_addr_t rto)
{
	/* [한국어] 플래그를 남기고 주소를 지운다. */
	*entry &= ZPCI_RTE_FLAG_MASK;
	/* [한국어] RT 테이블의 물리 주소를 넣는다. */
	*entry |= (rto & ZPCI_RTE_ADDR_MASK);
	/* [한국어] 이 엔트리가 RS 단계임을 표시한다. */
	*entry |= ZPCI_TABLE_TYPE_RSX;
}

/*
 * [한국어]
 * set_rt_sto - RT 엔트리에 세그먼트 테이블 주소를 심는다
 *
 * @entry: 고칠 RT 엔트리.
 * @sto: 세그먼트 테이블의 물리 주소.
 * @return: 없음.
 *
 * 실행 컨텍스트: 테이블 설치 경로.
 *
 * 호출 체인:
 *   dma_get_seg_table_origin() → [set_rt_sto]
 */
static inline void set_rt_sto(unsigned long *entry, phys_addr_t sto)
{
	/* [한국어] 플래그를 남기고 주소를 지운다. */
	*entry &= ZPCI_RTE_FLAG_MASK;
	/* [한국어] 세그먼트 테이블의 물리 주소를 넣는다. */
	*entry |= (sto & ZPCI_RTE_ADDR_MASK);
	/* [한국어] 이 엔트리가 RT 단계임을 표시한다. */
	*entry |= ZPCI_TABLE_TYPE_RTX;
}

/*
 * [한국어]
 * set_st_pto - 세그먼트 엔트리에 페이지 테이블 주소를 심는다
 *
 * @entry: 고칠 세그먼트 엔트리.
 * @pto: 페이지 테이블의 물리 주소.
 * @return: 없음.
 *
 * 세그먼트 엔트리는 마스크가 다르다(ZPCI_STE_*) — 페이지 테이블이
 * 영역 테이블보다 작아 정렬 요구가 다르기 때문이다.
 *
 * 실행 컨텍스트: 테이블 설치 경로.
 *
 * 호출 체인:
 *   dma_get_page_table_origin() → [set_st_pto]
 */
static inline void set_st_pto(unsigned long *entry, phys_addr_t pto)
{
	/* [한국어] 세그먼트 엔트리 전용 플래그 마스크를 쓴다. */
	*entry &= ZPCI_STE_FLAG_MASK;
	/* [한국어] 페이지 테이블의 물리 주소를 넣는다. */
	*entry |= (pto & ZPCI_STE_ADDR_MASK);
	/* [한국어] 이 엔트리가 세그먼트 단계임을 표시한다. */
	*entry |= ZPCI_TABLE_TYPE_SX;
}

/*
 * [한국어]
 * validate_rf_entry - RF 엔트리를 유효 상태로 만든다
 *
 * @entry: 고칠 엔트리.
 * @return: 없음.
 *
 * 유효 비트를 세우고, 그 단계의 테이블 길이를 지정한다.
 * 오프셋 필드를 지우는 것도 중요하다 — 그 필드는 "이 테이블의 어느
 * 부분부터 유효한가"를 뜻하는데, 전체를 쓰므로 0이어야 한다.
 *
 * 아래 네 개의 validate_* 함수가 같은 패턴이며, ST만 길이 필드가
 * 없어 더 짧다.
 *
 * 실행 컨텍스트: 테이블 설치 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   dma_walk_rf_table() → [validate_rf_entry]
 */
static inline void validate_rf_entry(unsigned long *entry)
{
	/* [한국어] 유효 비트 필드를 먼저 지운다. */
	*entry &= ~ZPCI_TABLE_VALID_MASK;
	/* [한국어] 오프셋 필드도 지워 테이블 전체를 쓴다고 표시한다. */
	*entry &= ~ZPCI_TABLE_OFFSET_MASK;
	/* [한국어] 유효 비트를 세운다. */
	*entry |= ZPCI_TABLE_VALID;
	/* [한국어] RF 단계 테이블의 길이를 지정한다. */
	*entry |= ZPCI_TABLE_LEN_RFX;
}

/*
 * [한국어]
 * validate_rs_entry - RS 엔트리를 유효 상태로 만든다
 *
 * @entry: 고칠 엔트리.
 * @return: 없음.
 *
 * 실행 컨텍스트: 테이블 설치 경로.
 *
 * 호출 체인:
 *   dma_walk_rs_table() → [validate_rs_entry]
 */
static inline void validate_rs_entry(unsigned long *entry)
{
	/* [한국어] 유효 비트 필드를 지운다. */
	*entry &= ~ZPCI_TABLE_VALID_MASK;
	/* [한국어] 오프셋 필드를 지운다. */
	*entry &= ~ZPCI_TABLE_OFFSET_MASK;
	/* [한국어] 유효 비트를 세운다. */
	*entry |= ZPCI_TABLE_VALID;
	/* [한국어] RS 단계 테이블의 길이를 지정한다. */
	*entry |= ZPCI_TABLE_LEN_RSX;
}

/*
 * [한국어]
 * validate_rt_entry - RT 엔트리를 유효 상태로 만든다
 *
 * @entry: 고칠 엔트리.
 * @return: 없음.
 *
 * 실행 컨텍스트: 테이블 설치 경로.
 *
 * 호출 체인:
 *   dma_get_seg_table_origin() → [validate_rt_entry]
 */
static inline void validate_rt_entry(unsigned long *entry)
{
	/* [한국어] 유효 비트 필드를 지운다. */
	*entry &= ~ZPCI_TABLE_VALID_MASK;
	/* [한국어] 오프셋 필드를 지운다. */
	*entry &= ~ZPCI_TABLE_OFFSET_MASK;
	/* [한국어] 유효 비트를 세운다. */
	*entry |= ZPCI_TABLE_VALID;
	/* [한국어] RT 단계 테이블의 길이를 지정한다. */
	*entry |= ZPCI_TABLE_LEN_RTX;
}

/*
 * [한국어]
 * validate_st_entry - 세그먼트 엔트리를 유효 상태로 만든다
 *
 * @entry: 고칠 엔트리.
 * @return: 없음.
 *
 * 다른 validate_*와 달리 길이와 오프셋 필드를 다루지 않는다 —
 * 세그먼트 엔트리에는 그 필드가 없기 때문이다.
 *
 * 실행 컨텍스트: 테이블 설치 경로.
 *
 * 호출 체인:
 *   dma_get_page_table_origin() → [validate_st_entry]
 */
static inline void validate_st_entry(unsigned long *entry)
{
	/* [한국어] 유효 비트 필드를 지운다. */
	*entry &= ~ZPCI_TABLE_VALID_MASK;
	/* [한국어] 유효 비트를 세운다. */
	*entry |= ZPCI_TABLE_VALID;
}

/*
 * [한국어]
 * invalidate_pt_entry - PTE를 무효 상태로 만든다
 *
 * @entry: 고칠 페이지 테이블 엔트리.
 * @return: 없음.
 *
 * WARN_ON_ONCE가 이미 무효한 엔트리를 다시 무효화하는 경우를 잡는다 —
 * 상위 계층이 같은 IOVA를 두 번 unmap 했다는 뜻이라 버그의 신호다.
 * PTE의 유효 비트가 다른 테이블과 달리 **반대 논리**인 점에 주의:
 * ZPCI_PTE_INVALID를 세워야 무효가 된다.
 *
 * 실행 컨텍스트: 해제 경로.
 *
 * 호출 체인:
 *   dma_update_cpu_trans() → [invalidate_pt_entry]
 */
static inline void invalidate_pt_entry(unsigned long *entry)
{
	/* [한국어] 이미 무효한 엔트리를 또 무효화하려 한다면 상위 계층의
	 * 중복 unmap이다. ONCE로 로그 폭주를 막는다. */
	WARN_ON_ONCE((*entry & ZPCI_PTE_VALID_MASK) == ZPCI_PTE_INVALID);
	/* [한국어] 유효 비트 필드를 지운다. */
	*entry &= ~ZPCI_PTE_VALID_MASK;
	/* [한국어] 무효 표시를 세운다 — PTE는 INVALID를 명시적으로
	 * 세우는 방식이다. */
	*entry |= ZPCI_PTE_INVALID;
}

/*
 * [한국어]
 * validate_pt_entry - PTE를 유효 상태로 만든다
 *
 * @entry: 고칠 엔트리.
 * @return: 없음.
 *
 * 대칭으로, 이미 유효한 엔트리를 다시 유효화하는 경우를 경고한다 —
 * unmap 없이 덮어쓰려 한다는 뜻이다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   dma_update_cpu_trans() → [validate_pt_entry]
 */
static inline void validate_pt_entry(unsigned long *entry)
{
	/* [한국어] 이미 유효한 엔트리를 덮어쓰려 한다면 상위 계층이
	 * unmap을 건너뛴 것이다. */
	WARN_ON_ONCE((*entry & ZPCI_PTE_VALID_MASK) == ZPCI_PTE_VALID);
	/* [한국어] 유효 비트 필드를 지운다. */
	*entry &= ~ZPCI_PTE_VALID_MASK;
	/* [한국어] 유효 표시를 세운다. */
	*entry |= ZPCI_PTE_VALID;
}

/*
 * [한국어]
 * entry_set_protected - 엔트리를 쓰기 금지로 만든다
 *
 * @entry: 고칠 엔트리.
 * @return: 없음.
 *
 * s390의 권한 모델은 단순하다: 읽기는 항상 허용되고, 쓰기만
 * protected 비트로 막는다. IOMMU_WRITE가 요청되지 않으면
 * map이 이 비트를 세운다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   dma_update_cpu_trans() → [entry_set_protected]
 */
static inline void entry_set_protected(unsigned long *entry)
{
	/* [한국어] 보호 필드를 지운다. */
	*entry &= ~ZPCI_TABLE_PROT_MASK;
	/* [한국어] 보호(쓰기 금지) 표시를 세운다. */
	*entry |= ZPCI_TABLE_PROTECTED;
}

/*
 * [한국어]
 * entry_clr_protected - 엔트리의 쓰기 금지를 푼다
 *
 * @entry: 고칠 엔트리.
 * @return: 없음.
 *
 * 중간 테이블 엔트리에도 쓰인다 — 상위 단계에서 쓰기를 막으면
 * 하위의 모든 매핑이 읽기 전용이 되므로, 테이블 설치 시 항상
 * 보호를 풀어 둔다.
 *
 * 실행 컨텍스트: 매핑과 테이블 설치 경로.
 *
 * 호출 체인:
 *   dma_update_cpu_trans() / 테이블 설치 함수들 → [entry_clr_protected]
 */
static inline void entry_clr_protected(unsigned long *entry)
{
	/* [한국어] 보호 필드를 지운다. */
	*entry &= ~ZPCI_TABLE_PROT_MASK;
	/* [한국어] 비보호(쓰기 허용) 표시를 세운다. */
	*entry |= ZPCI_TABLE_UNPROTECTED;
}

/*
 * [한국어]
 * reg_entry_isvalid - 영역/세그먼트 엔트리가 유효한지 판별한다
 *
 * @entry: 검사할 엔트리 값.
 * @return: 유효하면 참.
 *
 * 실행 컨텍스트: 모든 워크와 해제 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   워크 함수들 / 해제 함수들 → [reg_entry_isvalid]
 */
static inline int reg_entry_isvalid(unsigned long entry)
{
	/* [한국어] 유효 비트 필드가 VALID와 일치하는지 본다. */
	return (entry & ZPCI_TABLE_VALID_MASK) == ZPCI_TABLE_VALID;
}

/*
 * [한국어]
 * pt_entry_isvalid - 페이지 테이블 엔트리가 유효한지 판별한다
 *
 * @entry: 검사할 엔트리 값.
 * @return: 유효하면 참.
 *
 * 영역 엔트리와 마스크/값이 달라 별도 함수가 필요하다.
 *
 * 실행 컨텍스트: 조회 경로. 순수 판별이다.
 *
 * 호출 체인:
 *   s390_iommu_iova_to_phys() → [pt_entry_isvalid]
 */
static inline int pt_entry_isvalid(unsigned long entry)
{
	/* [한국어] PTE 전용 유효 비트를 확인한다. */
	return (entry & ZPCI_PTE_VALID_MASK) == ZPCI_PTE_VALID;
}

/*
 * [한국어]
 * get_rf_rso - RF 엔트리에서 다음 단계(RS) 테이블의 가상 주소를 얻는다
 *
 * @entry: RF 엔트리 값.
 * @return: RS 테이블의 커널 가상 주소, 타입이 맞지 않으면 NULL.
 *
 * 타입 비트를 확인하는 것이 이 함수의 핵심이다. 엔트리가 실제로
 * RF 단계의 것인지 검증해, 잘못된 단계의 엔트리를 따라가 엉뚱한
 * 메모리를 테이블로 해석하는 사고를 막는다.
 *
 * 실행 컨텍스트: 워크와 해제 경로.
 *
 * 호출 체인:
 *   dma_walk_rf_table() / dma_free_rs_table() / get_rso_from_iova()
 *   → [get_rf_rso]
 */
static inline unsigned long *get_rf_rso(unsigned long entry)
{
	/* [한국어] 타입 비트가 RF인지 확인한 뒤에만 주소를 해석한다. */
	if ((entry & ZPCI_TABLE_TYPE_MASK) == ZPCI_TABLE_TYPE_RFX)
		return phys_to_virt(entry & ZPCI_RTE_ADDR_MASK);
	else
		/* [한국어] 타입이 다르면 이 엔트리를 RF로 해석하면 안 된다. */
		return NULL;
}

/*
 * [한국어]
 * get_rs_rto - RS 엔트리에서 RT 테이블의 가상 주소를 얻는다
 *
 * @entry: RS 엔트리 값.
 * @return: RT 테이블의 커널 가상 주소, 타입이 맞지 않으면 NULL.
 *
 * 실행 컨텍스트: 워크와 해제 경로.
 *
 * 호출 체인:
 *   dma_walk_rs_table() / dma_free_rt_table() / get_rto_from_iova()
 *   → [get_rs_rto]
 */
static inline unsigned long *get_rs_rto(unsigned long entry)
{
	/* [한국어] 타입 비트가 RS인지 확인한다. */
	if ((entry & ZPCI_TABLE_TYPE_MASK) == ZPCI_TABLE_TYPE_RSX)
		return phys_to_virt(entry & ZPCI_RTE_ADDR_MASK);
	else
		return NULL;
}

/*
 * [한국어]
 * get_rt_sto - RT 엔트리에서 세그먼트 테이블의 가상 주소를 얻는다
 *
 * @entry: RT 엔트리 값.
 * @return: 세그먼트 테이블의 커널 가상 주소, 타입이 맞지 않으면 NULL.
 *
 * 실행 컨텍스트: 워크와 해제 경로.
 *
 * 호출 체인:
 *   dma_get_seg_table_origin() / dma_free_seg_table() / iova_to_phys()
 *   → [get_rt_sto]
 */
static inline unsigned long *get_rt_sto(unsigned long entry)
{
	/* [한국어] 타입 비트가 RT인지 확인한다. */
	if ((entry & ZPCI_TABLE_TYPE_MASK) == ZPCI_TABLE_TYPE_RTX)
		return phys_to_virt(entry & ZPCI_RTE_ADDR_MASK);
	else
		return NULL;
}

/*
 * [한국어]
 * get_st_pto - 세그먼트 엔트리에서 페이지 테이블의 가상 주소를 얻는다
 *
 * @entry: 세그먼트 엔트리 값.
 * @return: 페이지 테이블의 커널 가상 주소, 타입이 맞지 않으면 NULL.
 *
 * 주소 마스크가 ZPCI_STE_ADDR_MASK인 점이 다르다 — 페이지 테이블의
 * 정렬 요구가 영역 테이블과 다르기 때문이다.
 *
 * 실행 컨텍스트: 워크와 해제 경로.
 *
 * 호출 체인:
 *   dma_get_page_table_origin() / dma_free_seg_table() / iova_to_phys()
 *   → [get_st_pto]
 */
static inline unsigned long *get_st_pto(unsigned long entry)
{
	/* [한국어] 타입 비트가 세그먼트인지 확인한다. */
	if ((entry & ZPCI_TABLE_TYPE_MASK) == ZPCI_TABLE_TYPE_SX)
		return phys_to_virt(entry & ZPCI_STE_ADDR_MASK);
	else
		return NULL;
}

/*
 * [한국어]
 * dma_alloc_cpu_table_caches - 테이블용 슬랩 캐시 두 개를 만든다
 *
 * @return: 0 성공, -ENOMEM(캐시 생성 실패).
 *
 * 영역 테이블과 페이지 테이블은 크기와 정렬 요구가 달라 별도 캐시가
 * 필요하다. 정렬을 명시적으로 지정하는 것이 핵심인데, 하드웨어가
 * 테이블 주소의 하위 비트를 플래그로 쓰므로 그만큼 정렬되어야 한다.
 *
 * 실행 컨텍스트: 부팅 초기(subsys_initcall).
 *
 * 호출 체인:
 *   s390_iommu_init() → [dma_alloc_cpu_table_caches]
 */
static int __init dma_alloc_cpu_table_caches(void)
{
	/* [한국어] 영역 테이블(RF/RS/RT/ST)용 캐시. 크기와 정렬을 모두
	 * 하드웨어 요구에 맞춰 지정한다. */
	dma_region_table_cache = kmem_cache_create("PCI_DMA_region_tables",
						   ZPCI_TABLE_SIZE,
						   ZPCI_TABLE_ALIGN,
						   0, NULL);
	if (!dma_region_table_cache)	/* [한국어] 영역 테이블 캐시를 못 만들면 도메인을 하나도 만들 수 없다. */
		return -ENOMEM;

	/* [한국어] 페이지 테이블용 캐시. 영역 테이블보다 작고 정렬 요구도
	 * 달라 별도로 둔다. */
	dma_page_table_cache = kmem_cache_create("PCI_DMA_page_tables",
						 ZPCI_PT_SIZE,
						 ZPCI_PT_ALIGN,
						 0, NULL);
	if (!dma_page_table_cache) {
		/* [한국어] 두 번째가 실패하면 첫 번째를 되돌린다. */
		kmem_cache_destroy(dma_region_table_cache);
		return -ENOMEM;	/* [한국어] 두 캐시가 모두 있어야 의미가 있으므로 실패를 전한다. */
	}
	return 0;	/* [한국어] 두 캐시가 모두 준비됐다. */
}

/*
 * [한국어]
 * dma_alloc_cpu_table - 영역 테이블을 하나 할당하고 무효화한다
 *
 * @gfp: 할당 플래그.
 * @return: 테이블의 커널 가상 주소, 실패하면 NULL.
 *
 * 슬랩에서 받은 뒤 모든 엔트리를 INVALID로 채운다. zalloc을 쓰지
 * 않는 이유: 0이 곧 무효를 뜻하지 않기 때문이다 — ZPCI_TABLE_INVALID는
 * 특정 비트 패턴이라 명시적으로 채워야 한다.
 *
 * 실행 컨텍스트: 도메인 생성과 워크 중 테이블 설치. gfp에 따라
 * atomic일 수 있다.
 *
 * 호출 체인:
 *   s390_domain_alloc_paging() / 워크 함수들 → [dma_alloc_cpu_table]
 */
static unsigned long *dma_alloc_cpu_table(gfp_t gfp)
{
	/* [한국어] 할당한 테이블과 초기화 순회용 커서. */
	unsigned long *table, *entry;

	/* [한국어] 영역 테이블 전용 캐시에서 받는다(정렬이 보장된다). */
	table = kmem_cache_alloc(dma_region_table_cache, gfp);
	if (!table)	/* [한국어] 슬랩이 비었다 — 상위가 매핑을 포기하게 된다. */
		return NULL;

	/* [한국어] 모든 엔트리를 무효 값으로 채운다. 0이 무효를 뜻하지
	 * 않으므로 zalloc으로는 대체할 수 없다. */
	for (entry = table; entry < table + ZPCI_TABLE_ENTRIES; entry++)
		*entry = ZPCI_TABLE_INVALID;
	return table;
}

/*
 * [한국어]
 * dma_free_cpu_table - 영역 테이블을 반납한다
 *
 * @table: 반납할 테이블.
 * @return: 없음.
 *
 * 실행 컨텍스트: 해제 경로와 설치 경쟁 패배 시.
 *
 * 호출 체인:
 *   dma_free_*_table() / 워크 함수들 → [dma_free_cpu_table]
 */
static void dma_free_cpu_table(void *table)
{
	/* [한국어] 영역 테이블 캐시에 반납한다. */
	kmem_cache_free(dma_region_table_cache, table);
}

/*
 * [한국어]
 * dma_free_page_table - 페이지 테이블을 반납한다
 *
 * @table: 반납할 테이블.
 * @return: 없음.
 *
 * 실행 컨텍스트: 해제 경로와 설치 경쟁 패배 시.
 *
 * 호출 체인:
 *   dma_free_seg_table() / dma_get_page_table_origin()
 *   → [dma_free_page_table]
 */
static void dma_free_page_table(void *table)
{
	/* [한국어] 페이지 테이블 캐시에 반납한다. */
	kmem_cache_free(dma_page_table_cache, table);
}

/*
 * [한국어]
 * dma_free_seg_table - 세그먼트 테이블과 그 아래 페이지 테이블들을 반납한다
 *
 * @entry: 그 세그먼트 테이블을 가리키는 RT 엔트리.
 * @return: 없음.
 *
 * 아래 세 개의 dma_free_*_table이 계층을 따라 내려가며 해제하는
 * 재귀 구조를 이룬다. 각 단계가 자기 아래를 먼저 정리한 뒤
 * 자신을 반납하는 후위 순회다.
 *
 * 실행 컨텍스트: 도메인 해제(RCU 콜백).
 *
 * 호출 체인:
 *   dma_free_rt_table() / dma_cleanup_tables() → [dma_free_seg_table]
 */
static void dma_free_seg_table(unsigned long entry)
{
	/* [한국어] 그 RT 엔트리가 가리키는 세그먼트 테이블. */
	unsigned long *sto = get_rt_sto(entry);
	/* [한국어] 세그먼트 엔트리 순회 인덱스. */
	int sx;

	/* [한국어] 유효한 세그먼트 엔트리마다 그 아래 페이지 테이블을 반납한다. */
	for (sx = 0; sx < ZPCI_TABLE_ENTRIES; sx++)
		if (reg_entry_isvalid(sto[sx]))
			dma_free_page_table(get_st_pto(sto[sx]));

	/* [한국어] 하위를 모두 정리했으니 세그먼트 테이블 자신을 반납한다.
	 * 세그먼트 테이블도 영역 테이블 캐시에서 온다. */
	dma_free_cpu_table(sto);
}

/*
 * [한국어]
 * dma_free_rt_table - RT 테이블과 그 아래 전부를 반납한다
 *
 * @entry: 그 RT 테이블을 가리키는 RS 엔트리.
 * @return: 없음.
 *
 * 실행 컨텍스트: 도메인 해제.
 *
 * 호출 체인:
 *   dma_free_rs_table() / dma_cleanup_tables() → [dma_free_rt_table]
 */
static void dma_free_rt_table(unsigned long entry)
{
	/* [한국어] 그 RS 엔트리가 가리키는 RT 테이블. */
	unsigned long *rto = get_rs_rto(entry);
	/* [한국어] RT 엔트리 순회 인덱스. */
	int rtx;

	/* [한국어] 유효한 RT 엔트리마다 그 아래 세그먼트 계층을 정리한다. */
	for (rtx = 0; rtx < ZPCI_TABLE_ENTRIES; rtx++)
		if (reg_entry_isvalid(rto[rtx]))
			dma_free_seg_table(rto[rtx]);

	/* [한국어] RT 테이블 자신을 반납한다. */
	dma_free_cpu_table(rto);
}

/*
 * [한국어]
 * dma_free_rs_table - RS 테이블과 그 아래 전부를 반납한다
 *
 * @entry: 그 RS 테이블을 가리키는 RF 엔트리.
 * @return: 없음.
 *
 * 5단계 구성(RFX)에서만 쓰인다.
 *
 * 실행 컨텍스트: 도메인 해제.
 *
 * 호출 체인:
 *   dma_cleanup_tables() → [dma_free_rs_table]
 */
static void dma_free_rs_table(unsigned long entry)
{
	/* [한국어] 그 RF 엔트리가 가리키는 RS 테이블. */
	unsigned long *rso = get_rf_rso(entry);
	/* [한국어] RS 엔트리 순회 인덱스. */
	int rsx;

	/* [한국어] 유효한 RS 엔트리마다 그 아래 RT 계층을 정리한다. */
	for (rsx = 0; rsx < ZPCI_TABLE_ENTRIES; rsx++)
		if (reg_entry_isvalid(rso[rsx]))
			dma_free_rt_table(rso[rsx]);

	/* [한국어] RS 테이블 자신을 반납한다. */
	dma_free_cpu_table(rso);
}

/*
 * [한국어]
 * dma_cleanup_tables - 도메인의 모든 변환 테이블을 반납한다
 *
 * @domain: 정리할 도메인.
 * @return: 없음.
 *
 * origin_type에 따라 어느 깊이부터 정리를 시작할지 갈린다 —
 * 5단계(RFX)면 RF부터, 3단계(RTX)면 RT부터다. 그 아래는 각
 * free 함수가 재귀적으로 처리한다.
 *
 * 실행 컨텍스트: 도메인 해제의 RCU 콜백. 이 시점에는 아무도
 * 이 도메인을 참조하지 않음이 보장된다.
 *
 * 호출 체인:
 *   s390_iommu_rcu_free_domain() → [dma_cleanup_tables]
 */
static void dma_cleanup_tables(struct s390_domain *domain)
{
	/* [한국어] 각 단계의 순회 인덱스. 하나만 쓰이지만 셋을 선언해 둔다. */
	int rtx, rsx, rfx;

	/* [한국어] 테이블이 없는 도메인이면(생성 실패 등) 할 일이 없다. */
	if (!domain->dma_table)
		return;

	/* [한국어] 최상위 테이블의 종류에 따라 정리 깊이가 달라진다. */
	switch (domain->origin_type) {
	/* [한국어] 5단계 구성 — RF 엔트리마다 그 아래 전부를 정리한다. */
	case ZPCI_TABLE_TYPE_RFX:
		for (rfx = 0; rfx < ZPCI_TABLE_ENTRIES; rfx++)	/* [한국어] RF 엔트리를 처음부터 끝까지 훑는다. */
			if (reg_entry_isvalid(domain->dma_table[rfx]))
				dma_free_rs_table(domain->dma_table[rfx]);
		break;
	/* [한국어] 4단계 구성 — RS 엔트리부터 시작한다. */
	case ZPCI_TABLE_TYPE_RSX:
		for (rsx = 0; rsx < ZPCI_TABLE_ENTRIES; rsx++)	/* [한국어] RS 엔트리를 처음부터 끝까지 훑는다. */
			if (reg_entry_isvalid(domain->dma_table[rsx]))
				dma_free_rt_table(domain->dma_table[rsx]);
		break;
	/* [한국어] 3단계 구성 — RT 엔트리부터 시작한다. */
	case ZPCI_TABLE_TYPE_RTX:
		for (rtx = 0; rtx < ZPCI_TABLE_ENTRIES; rtx++)	/* [한국어] RT 엔트리를 처음부터 끝까지 훑는다. */
			if (reg_entry_isvalid(domain->dma_table[rtx]))
				dma_free_seg_table(domain->dma_table[rtx]);
		break;
	/* [한국어] 알 수 없는 종류라면 구조를 신뢰할 수 없어 정리하지
	 * 않는다 — 잘못 해석해 엉뚱한 메모리를 해제하느니 누수가 낫다. */
	default:
		WARN_ONCE(1, "Invalid IOMMU table (%x)\n", domain->origin_type);	/* [한국어] 알 수 없는 테이블 종류 — 로그를 한 번만 남긴다. */
		return;
	}

	/* [한국어] 하위를 모두 정리했으니 최상위 테이블을 반납한다. */
	dma_free_cpu_table(domain->dma_table);
}

/*
 * [한국어]
 * dma_alloc_page_table - 페이지 테이블을 하나 할당하고 무효화한다
 *
 * @gfp: 할당 플래그. 매핑 경로에서 오면 GFP_ATOMIC일 수 있다.
 * @return: 페이지 테이블의 커널 가상 주소, 실패하면 NULL.
 *
 * dma_alloc_cpu_table()과 같은 구조지만 캐시와 엔트리 수, 무효 값이
 * 모두 다르다. 페이지 테이블은 ZPCI_PTE_INVALID로 채운다.
 *
 * 실행 컨텍스트: 매핑 경로. atomic 컨텍스트일 수 있다.
 *
 * 호출 체인:
 *   dma_get_page_table_origin() → [dma_alloc_page_table]
 */
static unsigned long *dma_alloc_page_table(gfp_t gfp)
{
	/* [한국어] 할당한 테이블과 초기화 커서. */
	unsigned long *table, *entry;

	/* [한국어] 페이지 테이블 전용 캐시에서 받는다. */
	table = kmem_cache_alloc(dma_page_table_cache, gfp);
	if (!table)	/* [한국어] 페이지 테이블 슬랩이 비었다. */
		return NULL;

	/* [한국어] 모든 PTE를 무효 값으로 채운다. PTE의 무효 표시는
	 * 영역 엔트리와 다른 비트 패턴이다. */
	for (entry = table; entry < table + ZPCI_PT_ENTRIES; entry++)
		*entry = ZPCI_PTE_INVALID;
	return table;
}

/*
 * [한국어]
 * dma_walk_rs_table - RS 단계를 지나 RT 테이블에 도달한다(없으면 만든다)
 *
 * @rso: RS 테이블의 시작 주소.
 * @dma_addr: 대상 IOVA.
 * @gfp: 중간 테이블 할당 플래그.
 * @return: RT 테이블의 주소, 할당 실패면 NULL.
 *
 * 이 파일에서 네 번 반복되는 **락 없는 테이블 설치** 패턴의 대표다.
 * 흐름은 이렇다:
 *   1) 엔트리를 READ_ONCE로 읽는다(컴파일러가 재읽기를 넣지 못하게).
 *   2) 이미 유효하면 그 아래 테이블을 그대로 쓴다.
 *   3) 없으면 새 테이블을 만들고 엔트리 값을 조립한 뒤,
 *      cmpxchg로 "무효였을 때만" 설치한다.
 *   4) cmpxchg가 실패했다면 다른 CPU가 먼저 설치한 것이다 —
 *      내가 만든 테이블을 버리고 상대의 것을 쓴다.
 *
 * 왜 락이 아니라 cmpxchg인가: 매핑 경로는 atomic 컨텍스트일 수 있고
 * 병렬성이 높다. 테이블 설치는 드물게 일어나며 충돌 시 낭비되는
 * 비용도 테이블 하나뿐이라, 락으로 직렬화하는 것보다 유리하다.
 *
 * 실행 컨텍스트: 매핑 경로. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   dma_walk_region_tables() / dma_walk_rf_table() → [dma_walk_rs_table]
 *   → dma_alloc_cpu_table() / cmpxchg()
 */
static unsigned long *dma_walk_rs_table(unsigned long *rso,
					dma_addr_t dma_addr, gfp_t gfp)
{
	/* [한국어] 이 IOVA가 가리키는 RS 엔트리의 인덱스. */
	unsigned int rsx = calc_rsx(dma_addr);
	/* [한국어] cmpxchg가 돌려준 옛 값과, 내가 조립한 새 엔트리 값. */
	unsigned long old_rse, rse;
	/* [한국어] 고칠 엔트리의 주소와, 그 아래 RT 테이블. */
	unsigned long *rsep, *rto;

	/* [한국어] 갱신 대상 엔트리의 주소를 잡는다. */
	rsep = &rso[rsx];
	/* [한국어] READ_ONCE로 한 번만 읽는다 — 다른 CPU가 동시에
	 * 설치할 수 있으므로 컴파일러의 재읽기를 막아야 한다. */
	rse = READ_ONCE(*rsep);
	/* [한국어] 이미 누가 만들어 둔 RT 테이블이 있으면 그것을 쓴다. */
	if (reg_entry_isvalid(rse)) {
		rto = get_rs_rto(rse);	/* [한국어] 기존 엔트리에서 RT 테이블의 주소를 꺼낸다. */
	} else {
		/* [한국어] 없으니 새 RT 테이블을 만든다. */
		rto = dma_alloc_cpu_table(gfp);
		if (!rto)	/* [한국어] 새 테이블을 못 만들면 워크를 이어 갈 수 없다. */
			return NULL;

		/* [한국어] 새 테이블의 물리 주소를 엔트리에 넣는다. */
		set_rs_rto(&rse, virt_to_phys(rto));
		/* [한국어] 유효 비트와 길이를 세운다. */
		validate_rs_entry(&rse);
		/* [한국어] 중간 엔트리는 쓰기를 막지 않는다 — 실제 권한은
		 * 말단 PTE에서 결정한다. */
		entry_clr_protected(&rse);

		/* [한국어] "여전히 무효일 때만" 설치한다. 이 원자적 교환이
		 * 두 CPU가 같은 자리에 각자 만든 테이블을 넣는 사고를 막는다. */
		old_rse = cmpxchg(rsep, ZPCI_TABLE_INVALID, rse);
		if (old_rse != ZPCI_TABLE_INVALID) {	/* [한국어] 교환이 실패했다 = 그 사이 다른 CPU가 먼저 설치했다. */
			/* Somone else was faster, use theirs */
			/* [한국어] 경쟁에서 졌다. 내 테이블은 버리고 상대가
			 * 설치한 것을 쓴다 — 어차피 내용은 같다(전부 무효). */
			dma_free_cpu_table(rto);
			rto = get_rs_rto(old_rse);	/* [한국어] 상대가 설치한 테이블로 갈아탄다. */
		}
	}
	return rto;	/* [한국어] 찾았거나 새로 만든 RT 테이블을 돌려준다. */
}

/*
 * [한국어]
 * dma_walk_rf_table - RF 단계를 지나 RT 테이블까지 도달한다
 *
 * @rfo: RF 테이블(최상위)의 시작 주소.
 * @dma_addr: 대상 IOVA.
 * @gfp: 할당 플래그.
 * @return: RT 테이블의 주소, 실패하면 NULL.
 *
 * 5단계 구성(origin_type == RFX)에서만 쓰인다. RF 엔트리를 위와
 * 같은 cmpxchg 패턴으로 처리한 뒤, 이어서 RS 단계를 위임한다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   dma_walk_region_tables() → [dma_walk_rf_table] → dma_walk_rs_table()
 */
static unsigned long *dma_walk_rf_table(unsigned long *rfo,
					dma_addr_t dma_addr, gfp_t gfp)
{
	/* [한국어] 이 IOVA가 가리키는 RF 엔트리의 인덱스. */
	unsigned int rfx = calc_rfx(dma_addr);
	/* [한국어] cmpxchg의 옛 값과 내가 조립한 새 엔트리. */
	unsigned long old_rfe, rfe;
	/* [한국어] 고칠 엔트리의 주소와 그 아래 RS 테이블. */
	unsigned long *rfep, *rso;

	/* [한국어] 갱신 대상 엔트리의 주소. */
	rfep = &rfo[rfx];
	/* [한국어] 동시 설치에 대비해 한 번만 읽는다. */
	rfe = READ_ONCE(*rfep);
	/* [한국어] 이미 RS 테이블이 있으면 그것을 쓴다. */
	if (reg_entry_isvalid(rfe)) {
		rso = get_rf_rso(rfe);	/* [한국어] 기존 엔트리에서 RS 테이블의 주소를 꺼낸다. */
	} else {
		/* [한국어] 새 RS 테이블을 만든다. */
		rso = dma_alloc_cpu_table(gfp);
		if (!rso)	/* [한국어] 새 RS 테이블을 만들지 못했다. */
			return NULL;

		/* [한국어] 물리 주소를 엔트리에 심는다. */
		set_rf_rso(&rfe, virt_to_phys(rso));
		/* [한국어] 유효 비트와 RF 단계 길이를 세운다. */
		validate_rf_entry(&rfe);
		/* [한국어] 중간 단계에서는 쓰기를 막지 않는다. */
		entry_clr_protected(&rfe);

		/* [한국어] 무효였을 때만 설치하는 원자적 교환. */
		old_rfe = cmpxchg(rfep, ZPCI_TABLE_INVALID, rfe);
		if (old_rfe != ZPCI_TABLE_INVALID) {	/* [한국어] 다른 CPU가 먼저 설치한 경우. */
			/* Somone else was faster, use theirs */
			/* [한국어] 다른 CPU가 먼저 설치했다 — 내 것을 버린다. */
			dma_free_cpu_table(rso);
			rso = get_rf_rso(old_rfe);	/* [한국어] 상대가 설치한 RS 테이블로 갈아탄다. */
		}
	}

	/* [한국어] 상대가 설치한 엔트리의 타입이 RF가 아니면 get_rf_rso가
	 * NULL을 돌려준다. 손상된 테이블을 따라가지 않고 멈춘다. */
	if (!rso)
		return NULL;

	/* [한국어] RF를 지났으니 나머지는 RS 단계 워크와 같다. */
	return dma_walk_rs_table(rso, dma_addr, gfp);
}

/*
 * [한국어]
 * dma_get_seg_table_origin - RT 엔트리 아래의 세그먼트 테이블을 얻는다
 *
 * @rtep: 대상 RT 엔트리의 주소.
 * @gfp: 할당 플래그.
 * @return: 세그먼트 테이블의 주소, 실패하면 NULL.
 *
 * 앞의 두 함수와 완전히 같은 cmpxchg 설치 패턴이다. 다른 점은
 * 인덱스를 직접 계산하지 않고 이미 지목된 엔트리를 받는다는 것뿐이다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   dma_walk_cpu_trans() → [dma_get_seg_table_origin]
 */
static unsigned long *dma_get_seg_table_origin(unsigned long *rtep, gfp_t gfp)
{
	/* [한국어] cmpxchg의 옛 값과 새 엔트리 값. */
	unsigned long old_rte, rte;
	/* [한국어] 그 아래 세그먼트 테이블. */
	unsigned long *sto;

	/* [한국어] 현재 엔트리를 한 번만 읽는다. */
	rte = READ_ONCE(*rtep);
	/* [한국어] 이미 세그먼트 테이블이 있으면 그것을 쓴다. */
	if (reg_entry_isvalid(rte)) {
		sto = get_rt_sto(rte);	/* [한국어] 기존 엔트리에서 세그먼트 테이블의 주소를 꺼낸다. */
	} else {
		/* [한국어] 세그먼트 테이블도 영역 테이블 캐시에서 받는다. */
		sto = dma_alloc_cpu_table(gfp);
		if (!sto)	/* [한국어] 세그먼트 테이블을 만들지 못했다. */
			return NULL;

		/* [한국어] 물리 주소를 엔트리에 심는다. */
		set_rt_sto(&rte, virt_to_phys(sto));
		/* [한국어] 유효 비트와 RT 단계 길이를 세운다. */
		validate_rt_entry(&rte);
		/* [한국어] 중간 단계는 쓰기를 막지 않는다. */
		entry_clr_protected(&rte);

		/* [한국어] 무효였을 때만 설치한다. */
		old_rte = cmpxchg(rtep, ZPCI_TABLE_INVALID, rte);
		if (old_rte != ZPCI_TABLE_INVALID) {	/* [한국어] 다른 CPU가 먼저 설치한 경우. */
			/* Somone else was faster, use theirs */
			/* [한국어] 경쟁 패배 — 내 테이블을 버린다. */
			dma_free_cpu_table(sto);
			sto = get_rt_sto(old_rte);	/* [한국어] 상대가 설치한 세그먼트 테이블로 갈아탄다. */
		}
	}
	return sto;	/* [한국어] 확보한 세그먼트 테이블을 돌려준다. */
}

/*
 * [한국어]
 * dma_get_page_table_origin - 세그먼트 엔트리 아래의 페이지 테이블을 얻는다
 *
 * @step: 대상 세그먼트 엔트리의 주소.
 * @gfp: 할당 플래그.
 * @return: 페이지 테이블의 주소, 실패하면 NULL.
 *
 * 계층의 마지막 설치 단계다. 다른 세 함수와 달리 dma_alloc_page_table()을
 * 쓴다 — 페이지 테이블은 별도 캐시에서 오고 엔트리 수도 다르다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   dma_walk_cpu_trans() → [dma_get_page_table_origin]
 */
static unsigned long *dma_get_page_table_origin(unsigned long *step, gfp_t gfp)
{
	/* [한국어] cmpxchg의 옛 값과 새 엔트리 값. */
	unsigned long old_ste, ste;
	/* [한국어] 그 아래 페이지 테이블. */
	unsigned long *pto;

	/* [한국어] 현재 세그먼트 엔트리를 한 번만 읽는다. */
	ste = READ_ONCE(*step);
	/* [한국어] 이미 페이지 테이블이 있으면 그것을 쓴다. */
	if (reg_entry_isvalid(ste)) {
		pto = get_st_pto(ste);	/* [한국어] 기존 엔트리에서 페이지 테이블의 주소를 꺼낸다. */
	} else {
		/* [한국어] 페이지 테이블 전용 캐시에서 받는다. */
		pto = dma_alloc_page_table(gfp);
		if (!pto)	/* [한국어] 페이지 테이블을 만들지 못했다. */
			return NULL;
		/* [한국어] 물리 주소를 세그먼트 엔트리에 심는다. */
		set_st_pto(&ste, virt_to_phys(pto));
		/* [한국어] 유효 비트를 세운다(세그먼트는 길이 필드가 없다). */
		validate_st_entry(&ste);
		/* [한국어] 세그먼트 단계에서 쓰기를 막으면 그 아래 512개
		 * 매핑이 전부 읽기 전용이 된다 — 반드시 풀어 둔다. */
		entry_clr_protected(&ste);

		/* [한국어] 무효였을 때만 설치한다. */
		old_ste = cmpxchg(step, ZPCI_TABLE_INVALID, ste);
		if (old_ste != ZPCI_TABLE_INVALID) {	/* [한국어] 다른 CPU가 먼저 설치한 경우. */
			/* Somone else was faster, use theirs */
			/* [한국어] 경쟁 패배 — 페이지 테이블 캐시로 되돌린다. */
			dma_free_page_table(pto);
			pto = get_st_pto(old_ste);	/* [한국어] 상대가 설치한 페이지 테이블로 갈아탄다. */
		}
	}
	return pto;	/* [한국어] 확보한 페이지 테이블을 돌려준다. */
}

/*
 * [한국어]
 * dma_walk_region_tables - 영역 단계들을 지나 RT 테이블에 도달한다
 *
 * @domain: 대상 도메인.
 * @dma_addr: 대상 IOVA.
 * @gfp: 할당 플래그.
 * @return: RT 테이블의 주소, 실패하면 NULL.
 *
 * 이 드라이버의 가변 단계 구조가 한곳에 모이는 지점이다. 최상위
 * 테이블이 무엇이냐에 따라 RT에 이르는 경로가 달라진다:
 *   RFX → RF와 RS 두 단계를 더 걸어야 한다
 *   RSX → RS 한 단계만 걸으면 된다
 *   RTX → 최상위가 이미 RT다. 걸을 것이 없다
 * 나머지 워크(RT→ST→PT)는 어느 구성에서나 동일하므로,
 * 이 함수가 그 공통 구간의 시작점을 만들어 주는 역할을 한다.
 *
 * 실행 컨텍스트: 매핑 경로.
 *
 * 호출 체인:
 *   dma_walk_cpu_trans() → [dma_walk_region_tables]
 *   → dma_walk_rf_table() / dma_walk_rs_table()
 */
static unsigned long *dma_walk_region_tables(struct s390_domain *domain,
					     dma_addr_t dma_addr, gfp_t gfp)
{
	/* [한국어] 최상위 테이블의 종류가 남은 단계 수를 결정한다. */
	switch (domain->origin_type) {
	/* [한국어] 5단계 구성 — RF부터 시작해 RS를 거쳐 RT까지 간다. */
	case ZPCI_TABLE_TYPE_RFX:
		return dma_walk_rf_table(domain->dma_table, dma_addr, gfp);
	/* [한국어] 4단계 구성 — RS 한 단계만 거치면 RT다. */
	case ZPCI_TABLE_TYPE_RSX:
		return dma_walk_rs_table(domain->dma_table, dma_addr, gfp);
	/* [한국어] 3단계 구성 — 최상위가 곧 RT다. 그대로 돌려준다. */
	case ZPCI_TABLE_TYPE_RTX:
		return domain->dma_table;
	/* [한국어] 알 수 없는 종류. 손상된 도메인이므로 워크를 포기한다. */
	default:
		return NULL;	/* [한국어] 워크를 시작할 지점을 알 수 없다. */
	}
}

/*
 * [한국어]
 * dma_walk_cpu_trans - IOVA에 해당하는 PTE의 주소를 찾는다(필요하면 만든다)
 *
 * @domain: 대상 도메인.
 * @dma_addr: 변환할 IOVA.
 * @gfp: 중간 테이블 할당 플래그.
 * @return: PTE의 주소, 중간 테이블 할당에 실패하면 NULL.
 *
 * 매핑 경로의 심장이다. 영역 단계를 지나 RT에 도달한 뒤,
 * RT → ST → PT 세 단계를 차례로 밟아 말단 엔트리의 주소를 얻는다.
 * 중간에 없는 테이블은 그때그때 만들어 붙인다(lazy allocation).
 *
 * 반환값이 "PTE의 값"이 아니라 "PTE의 주소"인 점이 중요하다 —
 * 호출자가 그 자리를 xchg로 갱신하기 때문이다.
 *
 * 실행 컨텍스트: 매핑/해제 경로. atomic일 수 있다.
 *
 * 호출 체인:
 *   s390_iommu_validate_trans() / invalidate_trans() → [dma_walk_cpu_trans]
 *   → dma_walk_region_tables() → dma_get_seg_table_origin()
 *   → dma_get_page_table_origin()
 */
static unsigned long *dma_walk_cpu_trans(struct s390_domain *domain,
					 dma_addr_t dma_addr, gfp_t gfp)
{
	/* [한국어] 각 단계에서 얻은 테이블의 시작 주소. */
	unsigned long *rto, *sto, *pto;
	/* [한국어] 각 단계의 인덱스. */
	unsigned int rtx, sx, px;

	/* [한국어] 구성에 맞는 영역 단계를 지나 RT 테이블에 도달한다. */
	rto = dma_walk_region_tables(domain, dma_addr, gfp);
	if (!rto)	/* [한국어] 영역 단계에서 이미 실패했다. */
		return NULL;

	/* [한국어] RT 단계 인덱스를 계산해 그 엔트리 아래로 내려간다. */
	rtx = calc_rtx(dma_addr);
	sto = dma_get_seg_table_origin(&rto[rtx], gfp);	/* [한국어] 그 RT 엔트리 아래의 세그먼트 테이블을 얻는다(없으면 만든다). */
	if (!sto)	/* [한국어] 세그먼트 테이블을 확보하지 못했다. */
		return NULL;

	/* [한국어] 세그먼트 단계 인덱스로 페이지 테이블까지 내려간다. */
	sx = calc_sx(dma_addr);
	pto = dma_get_page_table_origin(&sto[sx], gfp);	/* [한국어] 세그먼트 엔트리 아래의 페이지 테이블을 얻는다(없으면 만든다). */
	if (!pto)	/* [한국어] 페이지 테이블을 확보하지 못했다. */
		return NULL;

	/* [한국어] 마지막으로 PTE의 인덱스를 계산해 그 **주소**를 돌려준다.
	 * 호출자가 이 자리를 직접 갱신한다. */
	px = calc_px(dma_addr);
	return &pto[px];	/* [한국어] 말단 PTE의 주소를 돌려준다 — 값이 아니라 자리다. */
}

/*
 * [한국어]
 * dma_update_cpu_trans - PTE 하나를 원하는 상태로 갱신한다
 *
 * @ptep: 갱신할 PTE의 주소.
 * @page_addr: 매핑할 물리 주소(무효화라면 무시된다).
 * @flags: ZPCI_PTE_INVALID면 해제, ZPCI_TABLE_PROTECTED면 읽기 전용.
 * @return: 없음.
 *
 * 매핑과 해제가 같은 함수를 쓴다 — flags가 방향을 정한다.
 * 로컬 변수에 원하는 값을 다 조립한 뒤 마지막에 xchg 한 번으로
 * 밀어 넣는 것이 핵심이다. 그래야 하드웨어(또는 다른 CPU)가
 * 중간의 어중간한 상태를 보지 않는다. 예를 들어 주소는 새것인데
 * 유효 비트가 아직 안 선 상태 같은 것이다.
 *
 * 실행 컨텍스트: 매핑/해제 경로.
 *
 * 호출 체인:
 *   s390_iommu_validate_trans() / invalidate_trans() → [dma_update_cpu_trans]
 */
static void dma_update_cpu_trans(unsigned long *ptep, phys_addr_t page_addr, int flags)
{
	/* [한국어] 조립할 새 PTE 값. 완성될 때까지 메모리에 쓰지 않는다. */
	unsigned long pte;

	/* [한국어] 현재 값을 한 번만 읽어 플래그 비트를 물려받는다. */
	pte = READ_ONCE(*ptep);
	/* [한국어] 해제 요청이면 유효 비트를 내린다. */
	if (flags & ZPCI_PTE_INVALID) {
		invalidate_pt_entry(&pte);	/* [한국어] 유효 비트를 내려 매핑을 지운다. */
	} else {
		/* [한국어] 매핑 요청이면 물리 주소를 심고 유효화한다.
		 * 순서가 중요하다 — 주소를 먼저 넣고 유효 비트를 세운다. */
		set_pt_pfaa(&pte, page_addr);
		validate_pt_entry(&pte);	/* [한국어] 주소를 넣은 뒤에 유효 비트를 세운다. */
	}

	/* [한국어] 쓰기 권한 요청 여부에 따라 보호 비트를 정한다.
	 * s390은 읽기를 막을 수단이 없어 쓰기만 제어한다. */
	if (flags & ZPCI_TABLE_PROTECTED)
		entry_set_protected(&pte);
	else
		entry_clr_protected(&pte);

	/* [한국어] 완성된 값을 한 번의 원자적 교환으로 밀어 넣는다.
	 * 중간 상태가 하드웨어에 보이지 않게 하는 것이 목적이다. */
	xchg(ptep, pte);
}

/*
 * [한국어]
 * to_s390_domain - iommu_domain에서 바깥 s390_domain을 복원한다
 *
 * @dom: IOMMU 코어가 넘겨준 도메인 포인터.
 * @return: 그것을 품고 있는 s390_domain.
 *
 * 코어는 임베드된 iommu_domain만 다루므로, 드라이버 쪽 상태에
 * 접근하려면 매번 이 변환이 필요하다.
 *
 * 실행 컨텍스트: 모든 콜백의 첫 줄.
 *
 * 호출 체인:
 *   각종 iommu_ops 콜백 → [to_s390_domain]
 */
static struct s390_domain *to_s390_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드 멤버의 주소에서 바깥 구조체의 주소를 역산한다. */
	return container_of(dom, struct s390_domain, domain);
}

/*
 * [한국어]
 * s390_iommu_capable - IOMMU 코어의 능력 질의에 답한다
 *
 * @dev: 질의 대상 디바이스.
 * @cap: 묻는 능력.
 * @return: 지원하면 true.
 *
 * 두 가지를 답한다. 캐시 일관성은 s390에서 항상 보장되므로 무조건
 * 참이다. 지연 플러시(unmap을 모아 두었다가 한꺼번에 무효화)는
 * ISM 디바이스에서만 거부하는데, ISM(Internal Shared Memory)은
 * 매핑 해제가 즉시 반영되어야 하는 특수 장치이기 때문이다.
 *
 * 실행 컨텍스트: 디바이스 probe와 도메인 설정 시.
 *
 * 호출 체인:
 *   IOMMU 코어 → [s390_iommu_capable]
 */
static bool s390_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	/* [한국어] s390 PCI 디바이스의 드라이버 쪽 상태. */
	struct zpci_dev *zdev = to_zpci_dev(dev);

	/* [한국어] 질의된 능력에 따라 답이 갈린다. */
	switch (cap) {
	/* [한국어] s390에서는 DMA가 항상 캐시 일관적이다 — 상위 계층이
	 * 명시적 캐시 관리를 하지 않아도 된다는 뜻이다. */
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	/* [한국어] ISM 디바이스는 해제를 미룰 수 없다. 그 외에는
	 * gather로 모아 한 번에 무효화해도 된다. */
	case IOMMU_CAP_DEFERRED_FLUSH:
		return zdev->pft != PCI_FUNC_TYPE_ISM;
	/* [한국어] 나머지 능력은 지원하지 않는다. */
	default:
		return false;	/* [한국어] 묻지 않은 능력은 지원하지 않는다고 답한다. */
	}
}

/*
 * [한국어]
 * max_tbl_size - 이 도메인의 테이블이 덮을 수 있는 최대 IOVA를 돌려준다
 *
 * @domain: 대상 도메인.
 * @return: 마지막으로 유효한 IOVA(포함), 알 수 없으면 0.
 *
 * origin_type이 결정하는 또 하나의 값이다. 3단계(RTX)면 RT 하나가
 * 덮는 범위, 4단계(RSX)면 그보다 훨씬 넓은 범위, 5단계(RFX)면
 * 64비트 전체다. 도메인의 aperture_end와 예약 영역 계산이
 * 이 값을 기준으로 이뤄진다.
 *
 * 실행 컨텍스트: 도메인 생성과 예약 영역 조회.
 *
 * 호출 체인:
 *   s390_domain_alloc_paging() / get_resv_regions() → [max_tbl_size]
 */
static inline u64 max_tbl_size(struct s390_domain *domain)
{
	/* [한국어] 테이블 종류가 곧 덮는 범위를 정한다. */
	switch (domain->origin_type) {
	/* [한국어] 3단계 — RT 하나가 덮는 크기까지(끝 주소이므로 -1). */
	case ZPCI_TABLE_TYPE_RTX:
		return ZPCI_TABLE_SIZE_RT - 1;
	/* [한국어] 4단계 — RS가 덮는 더 넓은 범위. */
	case ZPCI_TABLE_TYPE_RSX:
		return ZPCI_TABLE_SIZE_RS - 1;
	/* [한국어] 5단계 — 64비트 주소 공간 전체를 덮는다. */
	case ZPCI_TABLE_TYPE_RFX:
		return U64_MAX;
	/* [한국어] 알 수 없는 종류라면 안전하게 0(범위 없음)을 답한다. */
	default:
		return 0;	/* [한국어] 범위를 알 수 없으니 0으로 답한다. */
	}
}

/*
 * [한국어]
 * s390_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 처음 쓸 디바이스. 필요한 IOVA 범위의 근거가 된다.
 * @return: 새 도메인의 iommu_domain, 실패하면 NULL.
 *
 * 이 드라이버에서 가장 중요한 판단이 여기 있다: **테이블을 몇 단계로
 * 만들 것인가.** 결정 규칙은 이렇다.
 *
 *   1) 필요한 aperture를 구한다 — 시스템 전역 한계(s390_iommu_aperture)와
 *      이 디바이스가 실제로 쓸 수 있는 범위 중 작은 쪽.
 *   2) 3단계(RT)로 덮을 수 있으면 3단계. 워크가 가장 짧다.
 *   3) 4단계(RS)로 덮을 수 있고 **하드웨어가 RS를 지원하면**(dtsm) 4단계.
 *   4) 5단계(RF)를 지원하면 5단계.
 *   5) 아무것도 안 되면 3단계로 두되 aperture를 그 범위로 **깎는다.**
 *
 * 마지막 갈래가 중요하다 — 요청받은 범위를 다 못 주더라도 실패시키지
 * 않고, 줄 수 있는 만큼만 주고 zdev->end_dma를 낮춰 상위 계층에
 * 알린다. 그래서 이 함수는 dev의 상태를 수정하는 부수 효과가 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL 할당).
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [s390_domain_alloc_paging]
 *   → dma_alloc_cpu_table()
 */
static struct iommu_domain *s390_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 이 디바이스의 DMA 가능 범위(start_dma/end_dma)와
	 * 하드웨어가 지원하는 테이블 종류(dtsm)를 여기서 읽는다. */
	struct zpci_dev *zdev = to_zpci_dev(dev);
	/* [한국어] 만들 도메인. */
	struct s390_domain *s390_domain;
	/* [한국어] 실제로 덮기로 결정한 IOVA 범위의 크기. */
	u64 aperture_size;

	/* [한국어] 도메인 뼈대를 0으로 초기화해 받는다. */
	s390_domain = kzalloc_obj(*s390_domain);
	if (!s390_domain)	/* [한국어] 도메인 구조체 할당에 실패했다. */
		return NULL;

	/* [한국어] 최상위 테이블을 먼저 만든다. 종류는 아직 정하지 않았지만
	 * 크기와 정렬은 모든 영역 테이블이 같아 문제가 없다. */
	s390_domain->dma_table = dma_alloc_cpu_table(GFP_KERNEL);
	if (!s390_domain->dma_table) {
		/* [한국어] 테이블을 못 만들면 도메인도 의미가 없다. */
		kfree(s390_domain);
		return NULL;	/* [한국어] 도메인 생성 실패를 코어에 알린다. */
	}

	/* [한국어] 시스템 전역 한계와 이 디바이스의 실제 필요 범위 중
	 * 작은 쪽을 택한다 — 필요 이상으로 깊은 테이블을 만들지 않기 위함이다. */
	aperture_size = min(s390_iommu_aperture,
			    zdev->end_dma - zdev->start_dma + 1);
	/* [한국어] 3단계로 충분한가? start_dma만큼 이미 소모되므로 빼고 비교한다. */
	if (aperture_size <= (ZPCI_TABLE_SIZE_RT - zdev->start_dma)) {
		s390_domain->origin_type = ZPCI_TABLE_TYPE_RTX;
	/* [한국어] 4단계로 충분하고, 하드웨어가 RS 테이블을 지원하는가?
	 * dtsm은 이 기능이 지원하는 테이블 종류의 비트맵이다. */
	} else if (aperture_size <= (ZPCI_TABLE_SIZE_RS - zdev->start_dma) &&
		  (zdev->dtsm & ZPCI_IOTA_DT_RS)) {
		s390_domain->origin_type = ZPCI_TABLE_TYPE_RSX;
	/* [한국어] 더 넓은 범위가 필요하고 5단계를 지원한다면 RF로 간다. */
	} else if (zdev->dtsm & ZPCI_IOTA_DT_RF) {
		s390_domain->origin_type = ZPCI_TABLE_TYPE_RFX;	/* [한국어] 5단계(RF) 테이블로 결정한다. */
	} else {
		/* Assume RTX available */
		/* [한국어] 깊은 테이블을 못 쓰는 구형 하드웨어. 실패시키지 않고
		 * 3단계로 두되, 실제로 덮을 수 있는 만큼으로 범위를 깎는다. */
		s390_domain->origin_type = ZPCI_TABLE_TYPE_RTX;
		aperture_size = ZPCI_TABLE_SIZE_RT - zdev->start_dma;	/* [한국어] 덮을 수 있는 크기로 요청 범위를 깎는다 — 실패시키지 않는다. */
	}
	/* [한국어] 결정된 범위를 디바이스에 되돌려 알린다. 위에서 깎였다면
	 * 여기서 end_dma가 줄어들어 상위 계층이 그만큼만 쓰게 된다. */
	zdev->end_dma = zdev->start_dma + aperture_size - 1;

	/* [한국어] 이 IOMMU는 4KB 페이지 하나만 지원한다 — 큰 페이지가 없다. */
	s390_domain->domain.pgsize_bitmap = SZ_4K;
	/* [한국어] aperture 밖의 IOVA 할당을 코어가 거부하도록 강제한다. */
	s390_domain->domain.geometry.force_aperture = true;
	/* [한국어] 테이블 자체는 0부터 덮는다. 디바이스별 하한은
	 * 예약 영역(resv region)으로 따로 표현한다. */
	s390_domain->domain.geometry.aperture_start = 0;
	/* [한국어] 상한은 고른 테이블 종류가 덮는 최대 주소다. */
	s390_domain->domain.geometry.aperture_end = max_tbl_size(s390_domain);

	/* [한국어] 디바이스 목록 갱신용 락을 초기화한다. */
	spin_lock_init(&s390_domain->list_lock);
	/* [한국어] RCU로 읽힐 목록이므로 전용 초기화 매크로를 쓴다. */
	INIT_LIST_HEAD_RCU(&s390_domain->devices);

	/* [한국어] 코어에는 임베드된 부분만 돌려준다. */
	return &s390_domain->domain;
}

/*
 * [한국어]
 * s390_iommu_rcu_free_domain - RCU 유예가 끝난 뒤 실제로 도메인을 해제한다
 *
 * @head: 도메인에 임베드된 rcu_head.
 * @return: 없음.
 *
 * 이 시점에는 무효화 함수들이 이 도메인의 devices 목록을 순회 중일
 * 가능성이 사라졌음이 RCU에 의해 보장된다. 그래서 테이블 계층 전체를
 * 안심하고 반납할 수 있다.
 *
 * 실행 컨텍스트: RCU 콜백(softirq 또는 rcuc 커널 스레드).
 *
 * 호출 체인:
 *   RCU 코어 → [s390_iommu_rcu_free_domain] → dma_cleanup_tables()
 */
static void s390_iommu_rcu_free_domain(struct rcu_head *head)
{
	/* [한국어] rcu_head의 주소에서 도메인을 복원한다. */
	struct s390_domain *s390_domain = container_of(head, struct s390_domain, rcu);

	/* [한국어] 5단계까지의 테이블 계층 전체를 재귀적으로 반납한다. */
	dma_cleanup_tables(s390_domain);
	/* [한국어] 마지막으로 도메인 구조체 자신을 해제한다. */
	kfree(s390_domain);
}

/*
 * [한국어]
 * s390_domain_free - 도메인 해제를 RCU 유예 뒤로 미룬다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 곧바로 해제하지 않는 이유가 이 드라이버 설계의 핵심 중 하나다.
 * 무효화 경로(iotlb_sync 등)가 rcu_read_lock 안에서 devices 목록을
 * 순회하는데, 그 순회가 끝나기 전에 도메인을 해제하면
 * use-after-free가 된다. call_rcu로 미루면 모든 읽기 구역이
 * 빠져나간 뒤에 해제가 이뤄진다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 domain_free → [s390_domain_free] → call_rcu()
 */
static void s390_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 드라이버 쪽 도메인을 복원한다. */
	struct s390_domain *s390_domain = to_s390_domain(domain);

	/* [한국어] 목록을 들여다보기 위해 읽기 구역에 들어간다. */
	rcu_read_lock();
	/* [한국어] 해제 시점에 아직 디바이스가 붙어 있다면 상위 계층의
	 * detach 누락이다 — 경고하되 해제는 진행한다. */
	WARN_ON(!list_empty(&s390_domain->devices));
	rcu_read_unlock();

	/* [한국어] 진행 중인 모든 RCU 읽기가 끝난 뒤 실제 해제를 수행한다. */
	call_rcu(&s390_domain->rcu, s390_iommu_rcu_free_domain);
}

/*
 * [한국어]
 * zdev_s390_domain_update - 디바이스가 현재 붙어 있는 도메인을 기록한다
 *
 * @zdev: 대상 디바이스.
 * @domain: 새로 붙은 도메인.
 * @return: 없음.
 *
 * zdev->s390_domain은 이 파일 밖(리셋 경로 등)에서도 읽히므로
 * 갱신을 dom_lock으로 보호한다. irqsave인 이유는 이 락을 잡는
 * 다른 지점이 인터럽트 컨텍스트일 수 있기 때문이다.
 *
 * 실행 컨텍스트: attach/detach 경로.
 *
 * 호출 체인:
 *   attach 계열 함수들 → [zdev_s390_domain_update]
 */
static void zdev_s390_domain_update(struct zpci_dev *zdev,
				    struct iommu_domain *domain)
{
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 도메인 포인터를 보호하는 락. zpci_iommu_register_ioat()도
	 * 같은 락으로 이 값을 읽는다. */
	spin_lock_irqsave(&zdev->dom_lock, flags);
	zdev->s390_domain = domain;	/* [한국어] 현재 붙어 있는 도메인 포인터를 교체한다. */
	spin_unlock_irqrestore(&zdev->dom_lock, flags);	/* [한국어] 갱신이 끝났으니 락과 인터럽트 상태를 되돌린다. */
}

/*
 * [한국어]
 * get_iota_region_flag - 테이블 종류에 맞는 IOTA 플래그를 돌려준다
 *
 * @domain: 대상 도메인.
 * @return: IOTA에 OR 할 플래그, 알 수 없으면 0.
 *
 * IOTA(I/O Translation Anchor)는 펌웨어에 넘기는 값으로,
 * "테이블의 물리 주소 + 그 테이블이 몇 단계짜리인지"를 한 워드에
 * 담는다. 이 함수가 뒷부분을 만든다. origin_type이 매핑 워크뿐
 * 아니라 하드웨어 등록에도 그대로 반영되는 지점이다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   s390_iommu_domain_reg_ioat() → [get_iota_region_flag]
 */
static u64 get_iota_region_flag(struct s390_domain *domain)
{
	/* [한국어] 최상위 테이블의 종류를 펌웨어에 알릴 플래그로 바꾼다. */
	switch (domain->origin_type) {
	/* [한국어] 3단계 — 앵커가 RT를 가리킨다고 표시. */
	case ZPCI_TABLE_TYPE_RTX:
		return ZPCI_IOTA_RTTO_FLAG;
	/* [한국어] 4단계 — 앵커가 RS를 가리킨다고 표시. */
	case ZPCI_TABLE_TYPE_RSX:
		return ZPCI_IOTA_RSTO_FLAG;
	/* [한국어] 5단계 — 앵커가 RF를 가리킨다고 표시. */
	case ZPCI_TABLE_TYPE_RFX:
		return ZPCI_IOTA_RFTO_FLAG;
	/* [한국어] 알 수 없는 종류를 펌웨어에 등록하면 안 된다.
	 * 0을 돌려주면 등록이 거부되어 DMA가 열리지 않는다. */
	default:
		WARN_ONCE(1, "Invalid IOMMU table (%x)\n", domain->origin_type);	/* [한국어] 등록해서는 안 될 도메인이다 — 흔적을 남긴다. */
		return 0;	/* [한국어] 플래그 0은 유효한 앵커가 아니라 등록이 거부된다. */
	}
}

/*
 * [한국어]
 * reg_ioat_propagate_error - IOAT 등록 결과를 "진짜 실패"인지 판별한다
 *
 * @cc: zpci_register_ioat()의 조건 코드.
 * @status: 함께 돌아온 PCI 상태 바이트.
 * @return: 호출자에게 실패로 전파해야 하면 true.
 *
 * 등록이 성공하지 않았다고 해서 항상 오류로 다뤄서는 안 된다.
 * 두 가지 예외가 있다.
 *
 *  - 디바이스가 이미 오류 상태라면, 이후 리셋 루틴이 다시 활성화할 때
 *    새 도메인의 IOAT를 자기가 등록해 준다. 여기서 실패로 처리하면
 *    attach가 헛되이 실패한다.
 *  - 디바이스가 이미 제거되었다면(INVAL_HANDLE) 등록할 대상 자체가
 *    없는 것이다. 성공으로 처리하고, 뒤따르는 오류 이벤트가
 *    정리를 촉발하게 둔다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   s390_iommu_attach_device() / s390_attach_dev_identity()
 *   → [reg_ioat_propagate_error]
 */
static bool reg_ioat_propagate_error(int cc, u8 status)
{
	/*
	 * If the device is in the error state the reset routine
	 * will register the IOAT of the newly set domain on re-enable
	 */
	/* [한국어] 오류 상태의 디바이스 — 리셋 후 재등록이 예정되어 있으므로
	 * 지금 실패로 다루지 않는다. */
	if (cc == ZPCI_CC_ERR && status == ZPCI_PCI_ST_FUNC_NOT_AVAIL)
		return false;
	/*
	 * If the device was removed treat registration as success
	 * and let the subsequent error event trigger tear down.
	 */
	/* [한국어] 이미 사라진 디바이스 — 등록할 대상이 없으니 성공으로 보고,
	 * 정리는 뒤이어 올 오류 이벤트에 맡긴다. */
	if (cc == ZPCI_CC_INVAL_HANDLE)
		return false;
	/* [한국어] 그 밖에는 조건 코드가 OK가 아니면 진짜 실패다. */
	return cc != ZPCI_CC_OK;
}

/*
 * [한국어]
 * s390_iommu_domain_reg_ioat - 도메인 종류에 맞게 IOAT를 펌웨어에 등록한다
 *
 * @zdev: 대상 디바이스.
 * @domain: 등록할 도메인.
 * @status: 펌웨어가 돌려준 상태 바이트를 받을 곳.
 * @return: 펌웨어 조건 코드(0이 성공).
 *
 * 도메인 세 종류가 각각 다른 등록을 낸다.
 *  - identity: IOTA를 0으로 등록한다. 펌웨어에게 "변환 없이 통과"를
 *    뜻하며, start/end_dma로 허용 범위만 지정한다.
 *  - blocked: 아무것도 등록하지 않는다. 호출자가 이미 등록을 해제한
 *    상태이므로 DMA가 막혀 있다.
 *  - paging: 테이블의 물리 주소에 단계 플래그를 얹어 IOTA를 조립한다.
 *
 * 실행 컨텍스트: attach 경로와 리셋 후 재등록 경로.
 *
 * 호출 체인:
 *   attach 함수들 / zpci_iommu_register_ioat()
 *   → [s390_iommu_domain_reg_ioat] → zpci_register_ioat()
 */
static int s390_iommu_domain_reg_ioat(struct zpci_dev *zdev,
				      struct iommu_domain *domain, u8 *status)
{
	/* [한국어] 페이징 도메인일 때만 쓰인다. */
	struct s390_domain *s390_domain;
	/* [한국어] 펌웨어 조건 코드. blocked면 아무것도 안 하므로 0으로 시작. */
	int rc = 0;
	/* [한국어] 펌웨어에 넘길 변환 앵커. */
	u64 iota;

	/* [한국어] 도메인 종류가 등록 내용을 결정한다. */
	switch (domain->type) {
	/* [한국어] 통과 모드 — IOTA를 0으로 주면 변환 없이 지나간다.
	 * 그래도 start/end_dma로 접근 범위는 제한된다. */
	case IOMMU_DOMAIN_IDENTITY:
		rc = zpci_register_ioat(zdev, 0, zdev->start_dma,	/* [한국어] IOTA를 0으로 등록해 변환 없이 통과시킨다. */
					zdev->end_dma, 0, status);
		break;
	/* Nothing to do in this case */
	/* [한국어] 차단 도메인 — 등록을 아예 하지 않는 것이 곧 차단이다.
	 * 호출자가 이미 zpci_unregister_ioat()를 부른 상태다. */
	case IOMMU_DOMAIN_BLOCKED:
		break;
	/* [한국어] 페이징 도메인 — 테이블 주소와 단계 플래그를 합쳐
	 * 앵커를 만든다. 이 한 워드가 하드웨어에게 변환 구조 전체를 알린다. */
	default:
		s390_domain = to_s390_domain(domain);	/* [한국어] 페이징 도메인의 테이블에 접근하기 위해 변환한다. */
		iota = virt_to_phys(s390_domain->dma_table) |	/* [한국어] 테이블의 물리 주소에 */
		       get_iota_region_flag(s390_domain);
		rc = zpci_register_ioat(zdev, 0, zdev->start_dma,	/* [한국어] 조립한 앵커를 펌웨어에 등록한다. */
					zdev->end_dma, iota, status);
	}

	return rc;	/* [한국어] 펌웨어 조건 코드를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * zpci_iommu_register_ioat - 현재 붙어 있는 도메인의 IOAT를 다시 등록한다
 *
 * @zdev: 대상 디바이스.
 * @status: 펌웨어 상태 바이트를 받을 곳.
 * @return: 펌웨어 조건 코드.
 *
 * s390 PCI 서브시스템이 디바이스를 리셋한 뒤 부르는 진입점이다.
 * 리셋은 하드웨어의 등록 상태를 지우므로, 커널이 기억하고 있는
 * 도메인을 다시 등록해 주어야 DMA가 살아난다.
 *
 * dom_lock을 잡는 이유: 이 함수가 도는 동안 attach가 도메인을
 * 바꾸면 엉뚱한 테이블이 등록된다.
 *
 * 실행 컨텍스트: PCI 리셋/복구 경로. 인터럽트 컨텍스트일 수 있어
 * irqsave를 쓴다.
 *
 * 호출 체인:
 *   arch/s390/pci 복구 코드 → [zpci_iommu_register_ioat]
 *   → s390_iommu_domain_reg_ioat()
 */
int zpci_iommu_register_ioat(struct zpci_dev *zdev, u8 *status)
{
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 펌웨어 조건 코드. */
	int rc;

	/* [한국어] 등록 중에 도메인이 바뀌지 않도록 잠근다. */
	spin_lock_irqsave(&zdev->dom_lock, flags);

	/* [한국어] 지금 붙어 있는 도메인을 그대로 다시 등록한다. */
	rc = s390_iommu_domain_reg_ioat(zdev, zdev->s390_domain, status);

	spin_unlock_irqrestore(&zdev->dom_lock, flags);	/* [한국어] 등록이 끝났으니 락과 인터럽트 상태를 되돌린다. */

	return rc;	/* [한국어] 펌웨어 조건 코드를 그대로 넘긴다. */
}

/*
 * [한국어]
 * blocking_domain_attach_device - 디바이스의 DMA를 전면 차단한다
 *
 * @domain: 차단 도메인(항상 전역 blocking_domain).
 * @dev: 대상 디바이스.
 * @old: 직전 도메인. 이 드라이버는 zdev에서 직접 읽으므로 쓰지 않는다.
 * @return: 항상 0.
 *
 * 이 드라이버의 detach 경로이자, 모든 attach의 첫 단계다.
 * 순서가 중요하다:
 *   1) 도메인의 디바이스 목록에서 뺀다 — 이후 무효화가 이 디바이스에
 *      명령을 내지 않게 된다.
 *   2) 펌웨어 등록을 해제한다 — 이 순간 하드웨어 DMA가 막힌다.
 *   3) 커널 쪽 포인터를 정리한다.
 *
 * 목록 제거가 등록 해제보다 먼저인 이유: 순서가 반대라면 이미 막힌
 * 디바이스에 무효화 명령을 낼 수 있어 오류가 난다.
 *
 * 실행 컨텍스트: attach/detach 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 / s390_iommu_attach_device() → [blocking_domain_attach_device]
 *   → zpci_unregister_ioat()
 */
static int blocking_domain_attach_device(struct iommu_domain *domain,
					 struct device *dev,
					 struct iommu_domain *old)
{
	/* [한국어] 대상 디바이스의 드라이버 쪽 상태. */
	struct zpci_dev *zdev = to_zpci_dev(dev);
	/* [한국어] 지금 붙어 있는 도메인(페이징일 때만 의미가 있다). */
	struct s390_domain *s390_domain;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 이미 차단 상태면 할 일이 없다. attach가 항상 이 함수를
	 * 먼저 부르므로 이 빠른 반환이 자주 쓰인다. */
	if (zdev->s390_domain->type == IOMMU_DOMAIN_BLOCKED)
		return 0;

	/* [한국어] 이전 도메인을 복원한다. */
	s390_domain = to_s390_domain(zdev->s390_domain);
	/* [한국어] 테이블이 붙어 있었다면(= 페이징 도메인이었다면)
	 * 그 도메인의 디바이스 목록에서 뺀다. identity 도메인이면
	 * dma_table이 없어 이 단계를 건너뛴다. */
	if (zdev->dma_table) {
		spin_lock_irqsave(&s390_domain->list_lock, flags);
		/* [한국어] RCU 순회자가 안전하게 빠져나갈 수 있는 제거 방식이다.
		 * 실제 노드 재사용은 유예 기간 뒤에나 가능하다. */
		list_del_rcu(&zdev->iommu_list);
		spin_unlock_irqrestore(&s390_domain->list_lock, flags);	/* [한국어] 목록 조작이 끝났으니 락을 놓는다. */
	}

	/* [한국어] 펌웨어 등록을 해제한다 — 이 시점부터 하드웨어 DMA가 막힌다. */
	zpci_unregister_ioat(zdev, 0);
	/* [한국어] 더 이상 유효한 테이블이 없음을 표시한다. */
	zdev->dma_table = NULL;
	/* [한국어] 현재 도메인을 차단 도메인으로 기록한다. */
	zdev_s390_domain_update(zdev, domain);

	return 0;	/* [한국어] 차단은 실패할 수 없는 동작이라 항상 성공이다. */
}

/*
 * [한국어]
 * s390_iommu_attach_device - 디바이스를 페이징 도메인에 붙인다
 *
 * @domain: 붙일 페이징 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인.
 * @return: 0 성공, -ENODEV(zPCI 디바이스 아님), -EINVAL(범위 불일치),
 *          -EIO(펌웨어 등록 실패).
 *
 * 순서가 이 함수의 전부다: **먼저 차단하고, 그다음 새 도메인을 연다.**
 * 그래서 등록이 실패해도 중간에 DMA가 열린 채 남는 창이 없다 —
 * 실패하면 그냥 차단 상태로 머문다. 원본 주석의
 * "If we fail now DMA remains blocked via blocking domain"이
 * 바로 그 의도다.
 *
 * 목록 추가를 등록보다 **뒤에** 하는 것도 대칭적으로 중요하다.
 * 등록 전에 목록에 넣으면, 아직 하드웨어가 모르는 디바이스에
 * 무효화 명령이 갈 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [s390_iommu_attach_device]
 *   → blocking_domain_attach_device() → s390_iommu_domain_reg_ioat()
 */
static int s390_iommu_attach_device(struct iommu_domain *domain,
				    struct device *dev,
				    struct iommu_domain *old)
{
	/* [한국어] 붙일 도메인의 드라이버 쪽 상태. */
	struct s390_domain *s390_domain = to_s390_domain(domain);
	/* [한국어] 대상 디바이스. */
	struct zpci_dev *zdev = to_zpci_dev(dev);
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 펌웨어가 돌려주는 상태 바이트. */
	u8 status;
	/* [한국어] 펌웨어 조건 코드. */
	int cc;

	/* [한국어] zPCI 디바이스가 아니면 이 IOMMU가 다룰 수 없다. */
	if (!zdev)
		return -ENODEV;

	/* [한국어] 도메인이 덮는 범위와 디바이스가 쓸 범위가 전혀 겹치지
	 * 않으면 붙여 봐야 아무 매핑도 성립하지 않는다 — 코어의 버그다. */
	if (WARN_ON(domain->geometry.aperture_start > zdev->end_dma ||
		domain->geometry.aperture_end < zdev->start_dma))
		return -EINVAL;

	/* [한국어] 먼저 차단 도메인으로 옮긴다. 이전 도메인의 목록에서
	 * 빠지고 펌웨어 등록도 해제되어, 다음 단계가 실패해도 안전하다. */
	blocking_domain_attach_device(&blocking_domain, dev, old);

	/* If we fail now DMA remains blocked via blocking domain */
	/* [한국어] 새 도메인의 테이블을 펌웨어에 등록한다. */
	cc = s390_iommu_domain_reg_ioat(zdev, domain, &status);
	/* [한국어] "실패로 다뤄야 하는" 조건 코드만 걸러 낸다.
	 * 오류 상태/제거된 디바이스는 실패가 아니다. */
	if (reg_ioat_propagate_error(cc, status))
		return -EIO;
	/* [한국어] 이 디바이스가 쓰는 테이블을 기록한다. 무효화 경로가
	 * 이 값이 있는지로 유효한 매핑 대상인지 판단한다. */
	zdev->dma_table = s390_domain->dma_table;
	/* [한국어] 현재 도메인을 새 도메인으로 갱신한다. */
	zdev_s390_domain_update(zdev, domain);

	/* [한국어] 마지막으로 도메인의 디바이스 목록에 넣는다.
	 * 이제부터 이 도메인의 무효화가 이 디바이스에도 전달된다. */
	spin_lock_irqsave(&s390_domain->list_lock, flags);
	list_add_rcu(&zdev->iommu_list, &s390_domain->devices);	/* [한국어] RCU 순회자가 도중에 봐도 안전한 방식으로 추가한다. */
	spin_unlock_irqrestore(&s390_domain->list_lock, flags);	/* [한국어] 목록 조작이 끝났으니 락을 놓는다. */

	return 0;	/* [한국어] 등록과 목록 추가가 모두 끝났다. */
}

/*
 * [한국어]
 * s390_iommu_get_resv_regions - 이 디바이스가 쓸 수 없는 IOVA 구간을 알린다
 *
 * @dev: 대상 디바이스.
 * @list: 예약 영역을 매달 목록(코어가 준비).
 * @return: 없음.
 *
 * 도메인의 geometry는 항상 0부터 시작하지만, 실제로 디바이스가
 * 쓸 수 있는 범위는 [start_dma, end_dma]로 좁다. 그 바깥 두 구간을
 * "예약됨"으로 알려 IOVA 할당기가 피해 가게 한다.
 *
 *   0 ─────── start_dma ─────── end_dma ─────── max_tbl_size
 *   [ 예약 (앞) ]  [   사용 가능   ]  [  예약 (뒤)  ]
 *
 * 차단/통과 도메인에서는 뒤쪽 예약을 만들지 않는다. 그 도메인들은
 * 페이징 테이블이 없어 max_tbl_size를 계산할 대상이 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL 할당).
 *
 * 호출 체인:
 *   IOMMU 코어 get_resv_regions → [s390_iommu_get_resv_regions]
 *   → iommu_alloc_resv_region()
 */
static void s390_iommu_get_resv_regions(struct device *dev,
					struct list_head *list)
{
	/* [한국어] 이 디바이스의 실제 DMA 가능 범위를 여기서 읽는다. */
	struct zpci_dev *zdev = to_zpci_dev(dev);
	/* [한국어] 만들어 매달 예약 영역. */
	struct iommu_resv_region *region;
	/* [한국어] 테이블이 덮는 최대 주소와, 뒤쪽 예약 구간의 길이. */
	u64 max_size, end_resv;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* [한국어] 하한이 0이 아니면 그 앞 구간 전체를 예약한다. */
	if (zdev->start_dma) {
		region = iommu_alloc_resv_region(0, zdev->start_dma, 0,	/* [한국어] 0부터 하한 직전까지를 예약 영역으로 만든다. */
						 IOMMU_RESV_RESERVED, GFP_KERNEL);
		if (!region)	/* [한국어] 영역을 못 만들면 나머지 예약도 포기한다. */
			return;
		/* [한국어] 코어가 넘겨준 목록에 매단다. */
		list_add_tail(&region->list, list);
	}

	/* [한국어] 현재 도메인을 안전하게 들여다보기 위해 잠근다. */
	spin_lock_irqsave(&zdev->dom_lock, flags);
	/* [한국어] 차단/통과 도메인에는 페이징 테이블이 없어 상한을
	 * 계산할 수 없다 — 뒤쪽 예약을 만들지 않고 끝낸다. */
	if (zdev->s390_domain->type == IOMMU_DOMAIN_BLOCKED ||
	    zdev->s390_domain->type == IOMMU_DOMAIN_IDENTITY) {
		spin_unlock_irqrestore(&zdev->dom_lock, flags);	/* [한국어] 락을 놓고 뒤쪽 예약 없이 돌아간다. */
		return;
	}

	/* [한국어] 페이징 도메인이라면 테이블 종류가 덮는 최대 주소를 얻는다. */
	max_size = max_tbl_size(to_s390_domain(zdev->s390_domain));
	/* [한국어] 값을 얻었으니 락을 놓는다 — 이어지는 할당은 잠들 수 있다. */
	spin_unlock_irqrestore(&zdev->dom_lock, flags);

	/* [한국어] 테이블이 디바이스의 상한보다 넓게 덮는다면, 그 남는
	 * 뒷부분도 이 디바이스는 쓸 수 없으므로 예약한다. */
	if (zdev->end_dma < max_size) {
		/* [한국어] 예약할 길이 = 테이블 상한 − 디바이스 상한. */
		end_resv = max_size - zdev->end_dma;
		region = iommu_alloc_resv_region(zdev->end_dma + 1, end_resv,	/* [한국어] 상한 다음부터 테이블이 덮는 끝까지를 예약한다. */
						 0, IOMMU_RESV_RESERVED,
						 GFP_KERNEL);
		if (!region)	/* [한국어] 영역 할당에 실패했다. */
			return;
		list_add_tail(&region->list, list);	/* [한국어] 코어가 넘겨준 목록에 매단다. */
	}
}

/*
 * [한국어]
 * s390_iommu_probe_device - 디바이스를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 이 디바이스를 담당하는 iommu_device, 담당하지 않으면 ERR_PTR.
 *
 * 두 가지 결정을 한다.
 *
 * 첫째, shadow_on_flush 설정이다. zdev->tlb_refresh는 "이 디바이스의
 * 변환을 하이퍼바이저가 그림자로 관리한다"는 뜻이다. 그러면 매핑을
 * 추가한 뒤에도 명시적으로 알려야 하므로, 코어에 그 사실을 표시한다.
 *
 * 둘째, **차단 상태로 시작한다.** 도메인이 붙기 전까지 이 디바이스는
 * 어떤 DMA도 할 수 없다. 이것이 이 드라이버의 기본 안전 자세다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [s390_iommu_probe_device]
 */
static struct iommu_device *s390_iommu_probe_device(struct device *dev)
{
	/* [한국어] PCI 디바이스에서 복원할 zPCI 상태. */
	struct zpci_dev *zdev;

	/* [한국어] 이 IOMMU는 PCI 디바이스만 다룬다. 그 밖은 담당하지 않는다. */
	if (!dev_is_pci(dev))
		return ERR_PTR(-ENODEV);

	/* [한국어] PCI 디바이스에서 s390 고유 상태를 얻는다. */
	zdev = to_zpci_dev(dev);

	/* [한국어] 쓸 수 있는 IOVA 범위가 비어 있다면 매핑을 만들 수 없다. */
	if (zdev->start_dma > zdev->end_dma)
		return ERR_PTR(-EINVAL);

	/* [한국어] 하이퍼바이저가 그림자 테이블을 유지하는 구성이면,
	 * 매핑 추가 후에도 sync_map으로 알려야 한다고 코어에 표시한다. */
	if (zdev->tlb_refresh)
		dev->iommu->shadow_on_flush = 1;

	/* Start with DMA blocked */
	/* [한국어] 도메인 포인터를 보호할 락을 먼저 초기화한다. */
	spin_lock_init(&zdev->dom_lock);
	/* [한국어] 차단 도메인에 넣어 시작한다 — 누군가 명시적으로
	 * 도메인을 붙이기 전까지 DMA는 완전히 막혀 있다. */
	zdev_s390_domain_update(zdev, &blocking_domain);

	/* [한국어] 이 디바이스를 담당하는 IOMMU 인스턴스를 알린다. */
	return &zdev->iommu_dev;
}

/*
 * [한국어]
 * zpci_refresh_all - 이 디바이스의 IOVA 범위 전체를 무효화한다
 *
 * @zdev: 대상 디바이스.
 * @return: 펌웨어 반환값(0이 성공).
 *
 * 함수 핸들(fh)을 상위 32비트로 올려 만드는 것이 s390 명령의 관례다.
 * 범위 무효화가 실패했을 때의 최후 수단으로 쓰인다.
 *
 * 실행 컨텍스트: 무효화 경로.
 *
 * 호출 체인:
 *   flush_iotlb_all() / iotlb_sync_map() → [zpci_refresh_all]
 *   → zpci_refresh_trans()
 */
static int zpci_refresh_all(struct zpci_dev *zdev)
{
	/* [한국어] 함수 핸들을 명령 워드의 상위 절반에 놓고, 이 디바이스가
	 * 쓰는 IOVA 범위 전체를 무효화 대상으로 지정한다. */
	return zpci_refresh_trans((u64)zdev->fh << 32, zdev->start_dma,
				  zdev->end_dma - zdev->start_dma + 1);
}

/*
 * [한국어]
 * s390_iommu_flush_iotlb_all - 도메인의 모든 디바이스에서 전체 무효화
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * 이 드라이버의 무효화 세 함수가 공유하는 구조가 여기 처음 나온다:
 * **RCU 읽기 구역에서 디바이스 목록을 순회하며 각각에 명령을 낸다.**
 * 하나의 도메인에 여러 디바이스가 붙어 있을 수 있고, 펌웨어 명령은
 * 디바이스(함수 핸들) 단위라 그렇다.
 *
 * 반환값을 확인하지 않는다 — 전체 무효화는 안전한 방향의 동작이라
 * 실패해도 상위에 전할 마땅한 처리가 없다.
 *
 * 실행 컨텍스트: 무효화 경로. RCU 읽기 구역이라 잠들 수 없다.
 *
 * 호출 체인:
 *   IOMMU 코어 flush_iotlb_all → [s390_iommu_flush_iotlb_all]
 *   → zpci_refresh_all()
 */
static void s390_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	/* [한국어] 순회할 디바이스 목록을 가진 도메인. */
	struct s390_domain *s390_domain = to_s390_domain(domain);
	/* [한국어] 순회 커서. */
	struct zpci_dev *zdev;

	/* [한국어] 목록을 락 없이 읽기 위해 RCU 읽기 구역에 들어간다.
	 * 이 구역 안에서는 도메인이 해제되지 않음이 보장된다. */
	rcu_read_lock();
	list_for_each_entry_rcu(zdev, &s390_domain->devices, iommu_list) {
		/* [한국어] 전체 무효화 횟수를 센다(성능 진단용). */
		atomic64_inc(&s390_domain->ctrs.global_rpcits);
		/* [한국어] 이 디바이스의 변환 캐시 전체를 버리게 한다. */
		zpci_refresh_all(zdev);
	}
	rcu_read_unlock();	/* [한국어] 순회가 끝났으니 읽기 구역을 빠져나온다. */
}

/*
 * [한국어]
 * s390_iommu_iotlb_sync - 모아 둔 해제 범위를 한 번에 무효화한다
 *
 * @domain: 대상 도메인.
 * @gather: 코어가 모아 둔 무효화 범위.
 * @return: 없음.
 *
 * unmap이 즉시 무효화하지 않고 gather에 범위를 누적한 뒤, 이 함수가
 * 한 번에 처리한다. 펌웨어 명령이 비싸기 때문에 묶는 것이 이득이다
 * (그 지연을 허용할지는 capable()의 DEFERRED_FLUSH가 정한다).
 *
 * gather->end가 0이면 아무것도 모이지 않은 것이다 — 원본 주석이
 * 말하는 그 경우로, 빈 명령을 내지 않고 돌아간다.
 *
 * 실행 컨텍스트: 무효화 경로. RCU 읽기 구역.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync → [s390_iommu_iotlb_sync] → zpci_refresh_trans()
 */
static void s390_iommu_iotlb_sync(struct iommu_domain *domain,
				  struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct s390_domain *s390_domain = to_s390_domain(domain);
	/* [한국어] 모인 범위의 길이(양 끝 포함이라 +1). */
	size_t size = gather->end - gather->start + 1;
	/* [한국어] 순회 커서. */
	struct zpci_dev *zdev;

	/* If gather was never added to there is nothing to flush */
	/* [한국어] 아무 범위도 누적되지 않았다면 낼 명령이 없다.
	 * end가 0인 것이 "비어 있음"의 표시다. */
	if (!gather->end)
		return;

	/* [한국어] 목록을 RCU로 순회한다. */
	rcu_read_lock();
	list_for_each_entry_rcu(zdev, &s390_domain->devices, iommu_list) {
		/* [한국어] 지연 무효화 횟수를 센다. */
		atomic64_inc(&s390_domain->ctrs.sync_rpcits);
		/* [한국어] 모인 범위만 정확히 무효화한다 — 전체 무효화보다
		 * 훨씬 싸다. */
		zpci_refresh_trans((u64)zdev->fh << 32, gather->start,
				   size);
	}
	rcu_read_unlock();	/* [한국어] 순회가 끝났으니 읽기 구역을 빠져나온다. */
}

/*
 * [한국어]
 * s390_iommu_iotlb_sync_map - 새로 만든 매핑을 하이퍼바이저에 알린다
 *
 * @domain: 대상 도메인.
 * @iova: 새 매핑의 시작 IOVA.
 * @size: 새 매핑의 길이.
 * @return: 0 성공, 그 밖은 펌웨어 오류.
 *
 * 매핑을 **추가**한 뒤에도 무효화가 필요한 이유가 이 함수의 존재
 * 이유다. tlb_refresh가 켜진 디바이스는 하이퍼바이저가 게스트의
 * 테이블을 그림자로 복사해 두는데, 새 엔트리를 그냥 써 넣기만 하면
 * 하이퍼바이저는 그 사실을 모른다. 그래서 명시적으로 알린다.
 * tlb_refresh가 없는 디바이스(베어메탈)는 하드웨어가 테이블을
 * 직접 읽으므로 이 단계가 필요 없어 건너뛴다.
 *
 * -ENOMEM 처리가 흥미롭다. 하이퍼바이저가 그림자 매핑을 만들 메모리가
 * 부족하다는 뜻인데, 이때 전체 무효화로 물러선다. 그러면
 * 하이퍼바이저가 이미 해제된 엔트리들을 발견해 IOVA를 회수하고
 * 고정(pin)된 페이지를 풀 수 있게 되어, 메모리 압박이 해소된다.
 * 원본 주석이 말하는 바가 그것이다.
 *
 * 실행 컨텍스트: 매핑 경로 직후. RCU 읽기 구역.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync_map → [s390_iommu_iotlb_sync_map]
 *   → zpci_refresh_trans() → (실패 시) zpci_refresh_all()
 */
static int s390_iommu_iotlb_sync_map(struct iommu_domain *domain,
				     unsigned long iova, size_t size)
{
	/* [한국어] 대상 도메인. */
	struct s390_domain *s390_domain = to_s390_domain(domain);
	/* [한국어] 순회 커서. */
	struct zpci_dev *zdev;
	/* [한국어] 마지막 명령의 결과. 하나라도 실패하면 그 값이 전달된다. */
	int ret = 0;

	/* [한국어] 목록을 RCU로 순회한다. */
	rcu_read_lock();
	list_for_each_entry_rcu(zdev, &s390_domain->devices, iommu_list) {
		/* [한국어] 그림자 테이블을 쓰지 않는 디바이스는 알릴 필요가
		 * 없다 — 하드웨어가 메모리의 테이블을 직접 읽는다. */
		if (!zdev->tlb_refresh)
			continue;
		/* [한국어] 매핑 후 알림 횟수를 센다. */
		atomic64_inc(&s390_domain->ctrs.sync_map_rpcits);
		/* [한국어] 새로 만든 범위만 알린다. */
		ret = zpci_refresh_trans((u64)zdev->fh << 32,
					 iova, size);
		/*
		 * let the hypervisor discover invalidated entries
		 * allowing it to free IOVAs and unpin pages
		 */
		/* [한국어] 하이퍼바이저의 메모리가 부족하다는 신호다.
		 * 전체 무효화로 물러서면 하이퍼바이저가 무효 엔트리들을
		 * 발견해 IOVA를 회수하고 고정 페이지를 풀 수 있다 —
		 * 실패를 회피하는 것이 아니라 압박을 푸는 조치다. */
		if (ret == -ENOMEM) {
			ret = zpci_refresh_all(zdev);
			/* [한국어] 전체 무효화마저 실패하면 회복 수단이 없다.
			 * 남은 디바이스를 시도하지 않고 오류를 전한다. */
			if (ret)
				break;
		}
	}
	rcu_read_unlock();	/* [한국어] 순회가 끝났으니 읽기 구역을 빠져나온다. */

	return ret;	/* [한국어] 마지막 명령의 결과를 그대로 전한다. */
}

/*
 * [한국어]
 * s390_iommu_validate_trans - 연속된 페이지들의 PTE를 채운다
 *
 * @s390_domain: 대상 도메인.
 * @pa: 매핑할 첫 물리 주소.
 * @dma_addr: 매핑할 첫 IOVA.
 * @nr_pages: 페이지 수.
 * @flags: PTE에 반영할 유효/보호 플래그.
 * @gfp: 중간 테이블 할당 플래그.
 * @return: 0 성공, -ENOMEM(중간 테이블 할당 실패).
 *
 * 페이지마다 워크를 다시 하는 단순한 반복이다(연속 IOVA라도 캐시를
 * 두지 않는다). 중요한 것은 **부분 실패를 남기지 않는다**는 점이다.
 * 중간에 테이블 할당이 실패하면 undo 라벨로 내려가 이미 채운 PTE를
 * 역순으로 되돌린다. 그래야 상위 계층이 map 실패를 받았을 때
 * "아무것도 매핑되지 않았다"고 믿을 수 있다.
 *
 * 되돌리기에서 entry가 NULL이면 break 하는데, 이미 만든 테이블이므로
 * 정상적으로는 일어나지 않는다 — 방어적 처리다.
 *
 * 실행 컨텍스트: 매핑 경로. gfp에 따라 atomic일 수 있다.
 *
 * 호출 체인:
 *   s390_iommu_map_pages() → [s390_iommu_validate_trans]
 *   → dma_walk_cpu_trans() → dma_update_cpu_trans()
 */
static int s390_iommu_validate_trans(struct s390_domain *s390_domain,
				     phys_addr_t pa, dma_addr_t dma_addr,
				     unsigned long nr_pages, int flags,
				     gfp_t gfp)
{
	/* [한국어] 물리 주소를 페이지 경계로 내린다. 하위 오프셋은
	 * PTE에 들어가지 않는다. */
	phys_addr_t page_addr = pa & PAGE_MASK;
	/* [한국어] 현재 채우는 PTE의 주소. */
	unsigned long *entry;
	/* [한국어] 진행한 페이지 수. 실패 시 되돌릴 개수이기도 하다. */
	unsigned long i;
	/* [한국어] 실패 시 전할 오류 코드. */
	int rc;

	/* [한국어] 요청된 페이지들을 앞에서부터 하나씩 채운다. */
	for (i = 0; i < nr_pages; i++) {
		/* [한국어] 이 IOVA의 PTE 자리를 찾는다(없는 중간 테이블은
		 * 여기서 만들어진다). */
		entry = dma_walk_cpu_trans(s390_domain, dma_addr, gfp);
		/* [한국어] 중간 테이블 할당 실패. 여기까지 채운 것을 되돌린다. */
		if (unlikely(!entry)) {
			rc = -ENOMEM;	/* [한국어] 중간 테이블을 만들 메모리가 없다. */
			goto undo_cpu_trans;	/* [한국어] 이미 채운 PTE들을 되돌리러 간다. */
		}
		/* [한국어] 물리 주소와 플래그를 원자적으로 써 넣는다. */
		dma_update_cpu_trans(entry, page_addr, flags);
		/* [한국어] 다음 페이지로 물리 주소를 진행시킨다. */
		page_addr += PAGE_SIZE;
		/* [한국어] IOVA도 함께 진행시킨다 — 둘 다 연속이다. */
		dma_addr += PAGE_SIZE;
	}

	return 0;

/* [한국어] 부분 매핑을 남기지 않기 위한 되돌리기 경로. */
undo_cpu_trans:
	/* [한국어] 성공한 개수만큼 역순으로 되돌린다. i는 실패한 페이지의
	 * 인덱스이므로, 감소 후 비교로 그 앞의 것들만 처리한다. */
	while (i-- > 0) {
		/* [한국어] IOVA를 한 페이지 되돌린다. */
		dma_addr -= PAGE_SIZE;
		/* [한국어] 방금 채웠던 PTE를 다시 찾는다. 테이블이 이미
		 * 있으므로 새 할당은 일어나지 않는다. */
		entry = dma_walk_cpu_trans(s390_domain, dma_addr, gfp);
		/* [한국어] 정상적으로는 있을 수 없는 경우 — 더 진행하지 않는다. */
		if (!entry)
			break;
		/* [한국어] 무효화해 원래 상태로 되돌린다. */
		dma_update_cpu_trans(entry, 0, ZPCI_PTE_INVALID);
	}

	return rc;	/* [한국어] 되돌리기를 마치고 실패 이유를 전한다. */
}

/*
 * [한국어]
 * s390_iommu_invalidate_trans - 연속된 페이지들의 PTE를 무효화한다
 *
 * @s390_domain: 대상 도메인.
 * @dma_addr: 시작 IOVA.
 * @nr_pages: 페이지 수.
 * @return: 0 성공, -EINVAL(테이블에 해당 자리가 없음).
 *
 * validate의 반대 방향이지만 되돌리기가 없다. 무효화는 이미
 * 안전한 방향의 동작이라 중간에 멈춰도 문제가 되지 않기 때문이다.
 *
 * GFP_ATOMIC을 넘기지만 실제로 할당이 일어나지는 않는다 — 해제할
 * 매핑이 있다면 테이블은 이미 존재한다. 없다면 워크가 NULL을
 * 돌려주고, 그것은 상위 계층이 매핑되지 않은 곳을 해제하려 한
 * 것이므로 -EINVAL이다.
 *
 * 실행 컨텍스트: 해제 경로. atomic일 수 있다.
 *
 * 호출 체인:
 *   s390_iommu_unmap_pages() → [s390_iommu_invalidate_trans]
 */
static int s390_iommu_invalidate_trans(struct s390_domain *s390_domain,
				       dma_addr_t dma_addr, unsigned long nr_pages)
{
	/* [한국어] 무효화할 PTE의 주소. */
	unsigned long *entry;
	/* [한국어] 진행 인덱스. */
	unsigned long i;
	/* [한국어] 결과 코드. */
	int rc = 0;

	/* [한국어] 요청된 페이지들을 차례로 무효화한다. */
	for (i = 0; i < nr_pages; i++) {
		/* [한국어] PTE 자리를 찾는다. 매핑이 있었다면 테이블도 있으므로
		 * ATOMIC 할당이 실제로 일어날 일은 없다. */
		entry = dma_walk_cpu_trans(s390_domain, dma_addr, GFP_ATOMIC);
		/* [한국어] 자리가 없다 = 매핑되지 않은 IOVA를 해제하려 한 것이다. */
		if (unlikely(!entry)) {
			rc = -EINVAL;	/* [한국어] 매핑된 적 없는 IOVA를 해제하려 한 것이다. */
			break;
		}
		/* [한국어] 유효 비트를 내린다. 주소 인자는 무시된다. */
		dma_update_cpu_trans(entry, 0, ZPCI_PTE_INVALID);
		/* [한국어] 다음 페이지로 진행한다. */
		dma_addr += PAGE_SIZE;
	}

	return rc;	/* [한국어] 무효화 결과를 전한다. */
}

/*
 * [한국어]
 * s390_iommu_map_pages - IOVA 범위에 물리 페이지들을 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 IOVA의 시작.
 * @paddr: 매핑할 물리 주소의 시작.
 * @pgsize: 페이지 크기. 반드시 4KB.
 * @pgcount: 페이지 수.
 * @prot: IOMMU_READ/WRITE 등 요청 권한.
 * @gfp: 할당 플래그.
 * @mapped: 실제로 매핑된 바이트 수를 돌려줄 곳.
 * @return: 0 성공, -EINVAL(인자 오류), -ENOMEM(테이블 할당 실패).
 *
 * 코어가 부르는 매핑 진입점이다. 검사 세 가지를 거친 뒤 실제 작업을
 * validate_trans에 위임한다.
 *
 * 권한 처리가 s390답게 단순하다: **IOMMU_WRITE가 없으면 보호 비트를
 * 세운다.** 읽기는 항상 허용되며 끌 수 없다. 즉 이 IOMMU에는
 * "접근 불가" 상태가 없고, 매핑되면 최소한 읽기는 된다.
 *
 * 실행 컨텍스트: DMA 매핑 경로. gfp에 따라 atomic일 수 있다.
 *
 * 호출 체인:
 *   IOMMU 코어 map_pages → [s390_iommu_map_pages]
 *   → s390_iommu_validate_trans()
 */
static int s390_iommu_map_pages(struct iommu_domain *domain,
				unsigned long iova, phys_addr_t paddr,
				size_t pgsize, size_t pgcount,
				int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 대상 도메인. */
	struct s390_domain *s390_domain = to_s390_domain(domain);
	/* [한국어] 전체 매핑 길이. __ffs로 페이지 크기의 시프트를 구해 곱한다. */
	size_t size = pgcount << __ffs(pgsize);
	/* [한국어] PTE에 넣을 플래그. 유효 비트로 시작한다. */
	int flags = ZPCI_PTE_VALID, rc = 0;

	/* [한국어] 이 IOMMU는 4KB 페이지만 지원한다 — 큰 페이지 요청은 거부. */
	if (pgsize != SZ_4K)
		return -EINVAL;

	/* [한국어] 요청 범위가 도메인의 aperture를 벗어나면 테이블에
	 * 담을 자리가 없다. */
	if (iova < s390_domain->domain.geometry.aperture_start ||
	    (iova + size - 1) > s390_domain->domain.geometry.aperture_end)
		return -EINVAL;

	/* [한국어] IOVA와 물리 주소가 모두 페이지 정렬이어야 한다.
	 * OR로 묶어 한 번에 검사한다. */
	if (!IS_ALIGNED(iova | paddr, pgsize))
		return -EINVAL;

	/* [한국어] 쓰기를 요청하지 않았으면 보호 비트를 세운다.
	 * s390에는 읽기를 막는 수단이 없어 이것이 유일한 권한 제어다. */
	if (!(prot & IOMMU_WRITE))
		flags |= ZPCI_TABLE_PROTECTED;

	/* [한국어] 실제 PTE 채우기를 위임한다. 실패하면 이미 채운 것까지
	 * 되돌아온 상태로 반환된다. */
	rc = s390_iommu_validate_trans(s390_domain, paddr, iova,
				     pgcount, flags, gfp);
	if (!rc) {
		/* [한국어] 성공했으면 요청 전량이 매핑된 것이다. */
		*mapped = size;
		/* [한국어] 매핑된 페이지 수를 누적한다(sysfs 통계). */
		atomic64_add(pgcount, &s390_domain->ctrs.mapped_pages);
	}

	return rc;	/* [한국어] 매핑 결과를 코어에 전한다. */
}

/*
 * [한국어]
 * get_rso_from_iova - IOVA에 해당하는 RS 테이블을 찾는다(만들지는 않는다)
 *
 * @domain: 대상 도메인.
 * @iova: 조회할 IOVA.
 * @return: RS 테이블의 주소, 없으면 NULL.
 *
 * 조회(iova_to_phys) 전용 워크의 첫 단계다. 매핑 경로의 워크 함수와
 * 결정적으로 다른 점: **없으면 만들지 않고 NULL을 돌려준다.**
 * 조회는 있는 것을 확인할 뿐이므로 부수 효과가 없어야 한다.
 *
 * 실행 컨텍스트: 조회 경로. 락을 잡지 않고 READ_ONCE로만 읽는다.
 *
 * 호출 체인:
 *   get_rto_from_iova() → [get_rso_from_iova]
 */
static unsigned long *get_rso_from_iova(struct s390_domain *domain,
					dma_addr_t iova)
{
	/* [한국어] RF 테이블(최상위)의 주소. */
	unsigned long *rfo;
	/* [한국어] 읽어 온 RF 엔트리 값. */
	unsigned long rfe;
	/* [한국어] RF 단계 인덱스. */
	unsigned int rfx;

	/* [한국어] 최상위 테이블의 종류에 따라 RS에 이르는 길이 다르다. */
	switch (domain->origin_type) {
	/* [한국어] 5단계 구성 — RF를 한 단계 내려가야 RS가 나온다. */
	case ZPCI_TABLE_TYPE_RFX:
		rfo = domain->dma_table;	/* [한국어] 최상위 RF 테이블에서 시작한다. */
		rfx = calc_rfx(iova);
		/* [한국어] 동시 갱신 중일 수 있어 한 번만 읽는다. */
		rfe = READ_ONCE(rfo[rfx]);
		/* [한국어] 매핑된 적이 없는 구간이다 — 조회 실패. */
		if (!reg_entry_isvalid(rfe))
			return NULL;
		return get_rf_rso(rfe);
	/* [한국어] 4단계 구성 — 최상위가 곧 RS다. */
	case ZPCI_TABLE_TYPE_RSX:
		return domain->dma_table;
	/* [한국어] 3단계 구성에는 RS 단계 자체가 없다. */
	default:
		return NULL;	/* [한국어] 3단계 구성에는 RS 단계가 아예 없다. */
	}
}

/*
 * [한국어]
 * get_rto_from_iova - IOVA에 해당하는 RT 테이블을 찾는다(만들지는 않는다)
 *
 * @domain: 대상 도메인.
 * @iova: 조회할 IOVA.
 * @return: RT 테이블의 주소, 없으면 NULL.
 *
 * 조회 경로에서 dma_walk_region_tables()에 대응하는 함수다.
 * 5단계와 4단계는 get_rso_from_iova()로 RS까지 간 뒤 한 단계 더
 * 내려가고, 3단계는 최상위가 이미 RT다.
 *
 * 실행 컨텍스트: 조회 경로.
 *
 * 호출 체인:
 *   s390_iommu_iova_to_phys() → [get_rto_from_iova] → get_rso_from_iova()
 */
static unsigned long *get_rto_from_iova(struct s390_domain *domain,
					dma_addr_t iova)
{
	/* [한국어] RS 테이블의 주소. */
	unsigned long *rso;
	/* [한국어] 읽어 온 RS 엔트리 값. */
	unsigned long rse;
	/* [한국어] RS 단계 인덱스. */
	unsigned int rsx;

	/* [한국어] 구성에 따라 RT에 이르는 경로가 갈린다. */
	switch (domain->origin_type) {
	/* [한국어] 5단계와 4단계는 모두 RS 단계를 거친다 — 앞 함수가
	 * 그 차이를 흡수해 주므로 여기서는 한 갈래로 묶인다. */
	case ZPCI_TABLE_TYPE_RFX:
	case ZPCI_TABLE_TYPE_RSX:	/* [한국어] 4단계 구성도 아래와 같은 처리로 묶인다. */
		rso = get_rso_from_iova(domain, iova);	/* [한국어] 구성 차이를 흡수해 RS 테이블을 얻는다. */
		rsx = calc_rsx(iova);
		/* [한국어] RS 엔트리를 한 번만 읽는다. */
		rse = READ_ONCE(rso[rsx]);
		/* [한국어] 매핑되지 않은 구간이다. */
		if (!reg_entry_isvalid(rse))
			return NULL;
		return get_rs_rto(rse);
	/* [한국어] 3단계 구성 — 최상위가 곧 RT다. */
	case ZPCI_TABLE_TYPE_RTX:
		return domain->dma_table;
	/* [한국어] 알 수 없는 종류. */
	default:
		return NULL;	/* [한국어] 알 수 없는 구성 — 조회를 포기한다. */
	}
}

/*
 * [한국어]
 * s390_iommu_iova_to_phys - IOVA를 물리 주소로 변환한다(소프트웨어 워크)
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑되어 있지 않으면 0.
 *
 * 하드웨어가 하는 일을 소프트웨어가 그대로 흉내 내어 테이블을 걷는다.
 * VFIO 같은 상위 계층이 "이 IOVA가 무엇에 매핑되어 있는가"를 물을 때
 * 쓰인다.
 *
 * 중첩된 if가 계층을 그대로 반영한다 — RT 엔트리가 유효해야 ST를
 * 볼 수 있고, ST가 유효해야 PT를 볼 수 있다. 어느 단계에서든
 * 끊기면 phys가 초깃값 0인 채로 반환되어 "매핑 없음"을 뜻한다.
 *
 * 주소의 하위 오프셋을 더하지 않는 점에 주의: 이 IOMMU 인터페이스는
 * 페이지 정렬된 IOVA를 전제로 페이지 프레임 주소를 돌려준다.
 *
 * 실행 컨텍스트: 조회 경로. 락 없이 READ_ONCE만 쓴다.
 *
 * 호출 체인:
 *   IOMMU 코어 iova_to_phys → [s390_iommu_iova_to_phys]
 *   → get_rto_from_iova()
 */
static phys_addr_t s390_iommu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	/* [한국어] 대상 도메인. */
	struct s390_domain *s390_domain = to_s390_domain(domain);
	/* [한국어] 단계별 테이블의 주소. */
	unsigned long *rto, *sto, *pto;
	/* [한국어] 단계별로 읽어 온 엔트리 값. */
	unsigned long ste, pte, rte;
	/* [한국어] 단계별 인덱스. */
	unsigned int rtx, sx, px;
	/* [한국어] 결과 물리 주소. 0이 "매핑 없음"을 뜻하므로 그것으로 시작한다. */
	phys_addr_t phys = 0;

	/* [한국어] aperture 밖은 애초에 매핑될 수 없는 주소다. */
	if (iova < domain->geometry.aperture_start ||
	    iova > domain->geometry.aperture_end)
		return 0;

	/* [한국어] 구성에 맞게 영역 단계를 지나 RT 테이블을 찾는다. */
	rto = get_rto_from_iova(s390_domain, iova);
	/* [한국어] 중간 테이블이 없으면 이 IOVA는 매핑된 적이 없다. */
	if (!rto)
		return 0;

	/* [한국어] 남은 세 단계의 인덱스를 미리 계산해 둔다. */
	rtx = calc_rtx(iova);
	sx = calc_sx(iova);	/* [한국어] 세그먼트 단계의 인덱스. */
	px = calc_px(iova);

	/* [한국어] RT 엔트리를 읽는다. 동시 갱신에 대비해 READ_ONCE. */
	rte = READ_ONCE(rto[rtx]);
	/* [한국어] 유효할 때만 아래로 내려간다. 중첩 구조가 계층을 그대로
	 * 반영하며, 어디서든 끊기면 phys는 0으로 남는다. */
	if (reg_entry_isvalid(rte)) {
		/* [한국어] 세그먼트 테이블로 내려간다. */
		sto = get_rt_sto(rte);
		ste = READ_ONCE(sto[sx]);	/* [한국어] 세그먼트 엔트리를 한 번만 읽는다. */
		if (reg_entry_isvalid(ste)) {
			/* [한국어] 페이지 테이블로 내려간다. */
			pto = get_st_pto(ste);
			pte = READ_ONCE(pto[px]);
			/* [한국어] 말단 PTE가 유효하면 주소 필드를 뽑는다.
			 * PTE는 유효 판별식이 영역 엔트리와 달라 전용 함수를 쓴다. */
			if (pt_entry_isvalid(pte))
				phys = pte & ZPCI_PTE_ADDR_MASK;
		}
	}

	return phys;	/* [한국어] 찾았으면 물리 주소, 못 찾았으면 초깃값 0이다. */
}

/*
 * [한국어]
 * s390_iommu_unmap_pages - IOVA 범위의 매핑을 해제한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 시작 IOVA.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @gather: 무효화 범위를 모아 둘 곳.
 * @return: 실제로 해제한 바이트 수, 실패하면 0.
 *
 * PTE를 무효화하되 **하드웨어 무효화는 여기서 하지 않는다.**
 * 범위를 gather에 누적하고, 나중에 iotlb_sync()가 한 번에 처리한다.
 * 펌웨어 명령이 비싸기 때문에 묶어서 내는 것이 훨씬 유리하다.
 *
 * 실패 시 0을 돌려주는 것이 이 인터페이스의 관례다 — 코어는
 * 반환된 바이트 수만큼만 해제되었다고 이해한다.
 *
 * 실행 컨텍스트: DMA 해제 경로. atomic일 수 있다.
 *
 * 호출 체인:
 *   IOMMU 코어 unmap_pages → [s390_iommu_unmap_pages]
 *   → s390_iommu_invalidate_trans() → iommu_iotlb_gather_add_range()
 */
static size_t s390_iommu_unmap_pages(struct iommu_domain *domain,
				     unsigned long iova,
				     size_t pgsize, size_t pgcount,
				     struct iommu_iotlb_gather *gather)
{
	/* [한국어] 대상 도메인. */
	struct s390_domain *s390_domain = to_s390_domain(domain);
	/* [한국어] 해제할 전체 길이. */
	size_t size = pgcount << __ffs(pgsize);
	/* [한국어] 무효화 결과. */
	int rc;

	/* [한국어] aperture 밖을 해제하려는 것은 상위 계층의 버그다.
	 * map과 달리 WARN을 붙인 이유: 매핑된 적 없는 범위를 해제한다는
	 * 뜻이라 어딘가 장부가 어긋났다는 신호이기 때문이다. */
	if (WARN_ON(iova < s390_domain->domain.geometry.aperture_start ||
	    (iova + size - 1) > s390_domain->domain.geometry.aperture_end))
		return 0;

	/* [한국어] PTE들을 무효화한다. 하드웨어 무효화는 아직이다. */
	rc = s390_iommu_invalidate_trans(s390_domain, iova, pgcount);
	/* [한국어] 실패하면 아무것도 해제하지 못한 것으로 보고한다. */
	if (rc)
		return 0;

	/* [한국어] 무효화할 범위를 누적한다. 실제 펌웨어 명령은
	 * iotlb_sync()가 모아서 낸다. */
	iommu_iotlb_gather_add_range(gather, iova, size);
	/* [한국어] 해제된 페이지 수를 누적한다(sysfs 통계). */
	atomic64_add(pgcount, &s390_domain->ctrs.unmapped_pages);

	return size;	/* [한국어] 요청한 전량을 해제했다고 코어에 보고한다. */
}

/*
 * [한국어]
 * zpci_get_iommu_ctrs - 이 디바이스가 붙은 도메인의 통계 카운터를 얻는다
 *
 * @zdev: 대상 디바이스.
 * @return: 카운터 구조체, 페이징 도메인이 아니면 NULL.
 *
 * s390 PCI 서브시스템이 sysfs로 IOMMU 통계를 보일 때 쓴다.
 * 차단/통과 도메인에는 s390_domain이 없으므로(정적 iommu_domain만
 * 있다) NULL을 돌려준다 — to_s390_domain으로 변환하면 엉뚱한
 * 메모리를 가리키게 되기 때문이다.
 *
 * dom_lock을 호출자가 잡고 있어야 한다. 그래야 카운터를 읽는 동안
 * 도메인이 바뀌어 해제되지 않는다.
 *
 * 실행 컨텍스트: sysfs 조회. 호출자가 락을 잡은 상태.
 *
 * 호출 체인:
 *   arch/s390/pci sysfs 코드 → [zpci_get_iommu_ctrs]
 */
struct zpci_iommu_ctrs *zpci_get_iommu_ctrs(struct zpci_dev *zdev)
{
	/* [한국어] 카운터를 품고 있는 드라이버 쪽 도메인. */
	struct s390_domain *s390_domain;

	/* [한국어] 호출자가 dom_lock을 잡았는지 확인한다. 잡지 않았다면
	 * 반환한 포인터가 곧 해제될 수 있다. */
	lockdep_assert_held(&zdev->dom_lock);

	/* [한국어] 차단/통과 도메인은 정적 iommu_domain일 뿐이라
	 * s390_domain으로 변환할 수 없다. */
	if (zdev->s390_domain->type == IOMMU_DOMAIN_BLOCKED ||
	    zdev->s390_domain->type == IOMMU_DOMAIN_IDENTITY)
		return NULL;

	/* [한국어] 페이징 도메인이면 임베드된 카운터의 주소를 돌려준다. */
	s390_domain = to_s390_domain(zdev->s390_domain);
	return &s390_domain->ctrs;	/* [한국어] 임베드된 카운터의 주소를 돌려준다. */
}

/*
 * [한국어]
 * zpci_init_iommu - 이 디바이스의 IOMMU 인스턴스를 코어에 등록한다
 *
 * @zdev: 대상 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * s390에서는 **PCI 기능마다 IOMMU 인스턴스가 하나씩** 있다.
 * 다른 아키텍처처럼 하나의 IOMMU가 여러 디바이스를 관장하는 구조가
 * 아니라, 기능마다 독립된 변환 앵커를 갖는다. 그래서 이 함수가
 * 디바이스 생성 경로에서 호출된다.
 *
 * 어느 ops를 등록할지가 핵심 판단이다. rtr_avail은 "리셋 후에
 * 변환 등록을 되살릴 수 있다"는 하드웨어 능력인데, 이것이 있어야만
 * identity 도메인을 제공한다. 통과 모드를 안전하게 오가려면
 * 리셋 경계에서 등록 상태를 복원할 수 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 디바이스 생성. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   arch/s390/pci zpci 생성 코드 → [zpci_init_iommu]
 *   → iommu_device_register()
 */
int zpci_init_iommu(struct zpci_dev *zdev)
{
	/* [한국어] 단계별 결과 코드. */
	int rc = 0;

	/* [한국어] sysfs에 이 IOMMU 인스턴스를 노출한다. 이름에 기능 ID를
	 * 넣어 기능마다 구분되게 한다. */
	rc = iommu_device_sysfs_add(&zdev->iommu_dev, NULL, NULL,
				    "s390-iommu.%08x", zdev->fid);
	if (rc)	/* [한국어] sysfs 등록이 실패했다. */
		goto out_err;

	/* [한국어] 리셋 후 재등록이 가능한 하드웨어에서만 identity 도메인을
	 * 제공하는 ops를 쓴다 — 그렇지 않으면 통과 모드로 갔다가
	 * 리셋을 만났을 때 상태를 되살릴 수 없다. */
	if (zdev->rtr_avail) {
		rc = iommu_device_register(&zdev->iommu_dev,	/* [한국어] 통과 모드까지 제공하는 ops로 등록한다. */
					   &s390_iommu_rtr_ops, NULL);
	} else {
		/* [한국어] 구형 하드웨어 — identity 없이 페이징과 차단만. */
		rc = iommu_device_register(&zdev->iommu_dev, &s390_iommu_ops,
					   NULL);
	}
	if (rc)	/* [한국어] 코어 등록이 실패했다. */
		goto out_sysfs;

	return 0;

/* [한국어] 등록 실패 — 앞서 만든 sysfs 항목을 되돌린다. */
out_sysfs:
	iommu_device_sysfs_remove(&zdev->iommu_dev);

/* [한국어] 공통 오류 반환 지점. */
out_err:
	return rc;	/* [한국어] 실패 이유를 그대로 전한다. */
}

/*
 * [한국어]
 * zpci_destroy_iommu - 이 디바이스의 IOMMU 인스턴스를 걷어낸다
 *
 * @zdev: 대상 디바이스.
 * @return: 없음.
 *
 * zpci_init_iommu()의 역순이다. 코어에서 먼저 빼고(그래야 새 요청이
 * 들어오지 않는다) sysfs를 정리한다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   arch/s390/pci 제거 코드 → [zpci_destroy_iommu]
 */
void zpci_destroy_iommu(struct zpci_dev *zdev)
{
	/* [한국어] 코어에서 먼저 뺀다 — 이후로는 새 콜백이 오지 않는다. */
	iommu_device_unregister(&zdev->iommu_dev);
	/* [한국어] sysfs 항목을 지운다. */
	iommu_device_sysfs_remove(&zdev->iommu_dev);
}

/*
 * [한국어]
 * s390_iommu_setup - "s390_iommu=" 커널 파라미터를 처리한다
 *
 * @str: 등호 뒤의 문자열.
 * @return: 항상 1(파라미터를 소비했다는 뜻).
 *
 * 이제는 "strict" 하나만 남아 있고, 그마저도 폐기 예정이다.
 * 일반적인 iommu.strict=1로 대체되었으므로 경고를 찍고 같은 효과를
 * 낸다. 오래된 부트 스크립트를 깨뜨리지 않기 위한 호환 장치다.
 *
 * 실행 컨텍스트: 부팅 초기 파라미터 파싱.
 *
 * 호출 체인:
 *   커널 파라미터 파서 → [s390_iommu_setup]
 */
static int __init s390_iommu_setup(char *str)
{
	/* [한국어] 예전에는 지연 무효화를 끄는 유일한 방법이었다. */
	if (!strcmp(str, "strict")) {
		pr_warn("s390_iommu=strict deprecated; use iommu.strict=1 instead\n");
		/* [한국어] 아키텍처 공통 설정으로 같은 효과를 낸다. */
		iommu_set_dma_strict();
	}
	return 1;	/* [한국어] 이 파라미터를 소비했음을 파서에 알린다. */
}

/* [한국어] 위 파서를 커널 파라미터 테이블에 등록한다. */
__setup("s390_iommu=", s390_iommu_setup);

/*
 * [한국어]
 * s390_iommu_aperture_setup - "s390_iommu_aperture=" 파라미터를 처리한다
 *
 * @str: 등호 뒤의 숫자 문자열.
 * @return: 항상 1.
 *
 * aperture 기본값(시스템 메모리 크기)에 곱할 배수를 받는다.
 * 메모리보다 훨씬 넓은 IOVA 공간이 필요한 워크로드를 위한 손잡이다.
 * 파싱에 실패하면 조용히 1(기본)로 돌아가 부팅을 막지 않는다.
 *
 * 실행 컨텍스트: 부팅 초기 파라미터 파싱.
 *
 * 호출 체인:
 *   커널 파라미터 파서 → [s390_iommu_aperture_setup]
 */
static int __init s390_iommu_aperture_setup(char *str)
{
	/* [한국어] 10진수로 파싱한다. 실패하면 기본 배수 1로 되돌린다 —
	 * 오타 하나로 부팅이 이상해지지 않게 하려는 것이다. */
	if (kstrtou32(str, 10, &s390_iommu_aperture_factor))
		s390_iommu_aperture_factor = 1;
	return 1;	/* [한국어] 이 파라미터를 소비했음을 파서에 알린다. */
}

/* [한국어] 위 파서를 커널 파라미터 테이블에 등록한다. */
__setup("s390_iommu_aperture=", s390_iommu_aperture_setup);

/*
 * [한국어]
 * s390_iommu_init - 드라이버 전역 초기화
 *
 * @return: 0 성공, -ENOMEM(캐시 생성 실패).
 *
 * 두 가지를 한다.
 *
 * 첫째, iommu_dma_forcedac을 켠다. DAC(Dual Address Cycle)은 64비트
 * DMA 주소를 뜻하며, 이것을 강제하면 DMA 계층이 32비트 아래로
 * IOVA를 몰아넣으려는 시도를 하지 않는다. s390에는 32비트만 쓰는
 * 레거시 디바이스가 없어 그 배려가 낭비이기 때문이다.
 *
 * 둘째, aperture 기본값을 정한다. high_memory의 물리 주소, 즉
 * 시스템 메모리 전체 크기를 기준으로 삼는다. 배수가 0이면
 * 무제한으로 해석한다.
 *
 * 실행 컨텍스트: subsys_initcall. 부팅 초기, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   커널 initcall → [s390_iommu_init] → dma_alloc_cpu_table_caches()
 */
static int __init s390_iommu_init(void)
{
	/* [한국어] 캐시 생성 결과. */
	int rc;

	/* [한국어] 64비트 DMA 주소를 강제한다 — s390에는 32비트 전용
	 * 디바이스가 없어 낮은 주소를 아껴 둘 이유가 없다. */
	iommu_dma_forcedac = true;
	/* [한국어] 기본 aperture는 시스템 메모리 전체를 덮는 크기로 잡는다. */
	s390_iommu_aperture = (u64)virt_to_phys(high_memory);
	/* [한국어] 배수 0은 "제한하지 말라"는 뜻으로 해석한다. */
	if (!s390_iommu_aperture_factor)
		s390_iommu_aperture = ULONG_MAX;
	else
		/* [한국어] 지정된 배수만큼 넓힌다. */
		s390_iommu_aperture *= s390_iommu_aperture_factor;

	/* [한국어] 테이블용 슬랩 캐시를 만든다. 이것이 없으면 도메인을
	 * 하나도 만들 수 없다. */
	rc = dma_alloc_cpu_table_caches();
	if (rc)	/* [한국어] 캐시를 못 만들면 이 드라이버는 아무 일도 할 수 없다. */
		return rc;

	return rc;	/* [한국어] 여기까지 왔으면 rc는 0이다. */
}
/* [한국어] PCI 디바이스가 등장하기 전에 캐시가 준비되어야 하므로
 * subsys 단계에서 초기화한다. */
subsys_initcall(s390_iommu_init);

/*
 * [한국어]
 * s390_attach_dev_identity - 디바이스를 통과(identity) 모드로 붙인다
 *
 * @domain: identity 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인.
 * @return: 0 성공, -EIO(펌웨어 등록 실패).
 *
 * s390_iommu_attach_device()와 같은 "먼저 차단, 그다음 등록" 순서를
 * 따르지만 두 가지가 없다. 테이블이 없으므로 zdev->dma_table을
 * 설정하지 않고, 무효화 대상도 아니므로 디바이스 목록에 넣지 않는다.
 * 통과 모드에서는 변환 자체가 없어 무효화할 것도 없기 때문이다.
 *
 * rtr_avail 하드웨어에서만 이 경로가 열린다 — 리셋 후 등록을
 * 되살릴 수 없으면 통과 모드에서 빠져나오지 못할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) → [s390_attach_dev_identity]
 *   → blocking_domain_attach_device() → s390_iommu_domain_reg_ioat()
 */
static int s390_attach_dev_identity(struct iommu_domain *domain,
				    struct device *dev,
				    struct iommu_domain *old)
{
	/* [한국어] 대상 디바이스. */
	struct zpci_dev *zdev = to_zpci_dev(dev);
	/* [한국어] 펌웨어 상태 바이트. */
	u8 status;
	/* [한국어] 펌웨어 조건 코드. */
	int cc;

	/* [한국어] 먼저 차단 상태로 만든다 — 이전 도메인의 목록과 등록이
	 * 여기서 정리된다. */
	blocking_domain_attach_device(&blocking_domain, dev, old);

	/* If we fail now DMA remains blocked via blocking domain */
	/* [한국어] IOTA를 0으로 등록해 "변환 없이 통과"를 요청한다. */
	cc = s390_iommu_domain_reg_ioat(zdev, domain, &status);
	/* [한국어] 실패로 다뤄야 하는 조건 코드만 오류로 전한다.
	 * 실패해도 차단 상태이므로 DMA가 새지 않는다. */
	if (reg_ioat_propagate_error(cc, status))
		return -EIO;

	/* [한국어] 현재 도메인을 identity로 기록한다. 테이블도 목록 추가도
	 * 없다 — 통과 모드에는 무효화할 매핑이 없기 때문이다. */
	zdev_s390_domain_update(zdev, domain);

	return 0;	/* [한국어] 통과 모드로의 전환이 끝났다. */
}

/* [한국어] identity 도메인의 연산 테이블. attach 하나뿐인 이유는
 * 통과 모드에 매핑도 무효화도 없기 때문이다. */
static const struct iommu_domain_ops s390_identity_ops = {
	.attach_dev = s390_attach_dev_identity,
	/* [한국어] 이 도메인에 디바이스를 붙일 때 부를 콜백.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: IOMMU 코어의 attach 경로. */
};

/* [한국어] 시스템 전체가 공유하는 정적 identity 도메인.
 * 통과 모드는 상태를 갖지 않으므로 인스턴스가 하나면 충분하다. */
static struct iommu_domain s390_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 코어가 이 도메인을 통과 모드로 인식하게 하는 종류 표시.
	 * 읽는 자: s390_iommu_domain_reg_ioat()의 switch, get_resv_regions,
	 *          zpci_get_iommu_ctrs 등이 이 값으로 분기한다. */

	.ops = &s390_identity_ops,
	/* [한국어] 위에서 정의한 연산 테이블.
	 * 읽는 자: IOMMU 코어. */
};

/* [한국어] 시스템 전체가 공유하는 정적 차단 도메인.
 * 앞쪽에서 전방 선언된 그 변수이며, 모든 디바이스가 probe 직후
 * 이 도메인에 놓인다 — "기본은 차단"이라는 이 드라이버의 자세다. */
static struct iommu_domain blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	/* [한국어] 차단 도메인임을 코어와 이 드라이버 양쪽에 알리는 표시.
	 * 읽는 자: blocking_domain_attach_device()의 빠른 반환 조건과
	 *          reg_ioat의 switch가 이 값을 본다. */

	.ops = &(const struct iommu_domain_ops) {
		.attach_dev	= blocking_domain_attach_device,
		/* [한국어] 차단으로 옮길 때 부를 콜백. 목록에서 빼고
		 * 펌웨어 등록을 해제하는 일을 한다.
		 * 익명 구조체로 인라인 정의한 이유: 이 도메인 말고는
		 * 쓸 곳이 없기 때문이다. */
	}
};

/* [한국어] 두 벌의 iommu_ops가 공유하는 공통 항목들.
 * 매크로로 뽑아낸 이유: rtr_ops는 여기에 identity_domain 하나만
 * 더할 뿐 나머지가 완전히 같아서, 복사해 두면 한쪽만 고치는
 * 실수가 생기기 때문이다. */
#define S390_IOMMU_COMMON_OPS() \
	.blocked_domain		= &blocking_domain, \
	.release_domain		= &blocking_domain, \
	.capable = s390_iommu_capable, \
	.domain_alloc_paging = s390_domain_alloc_paging, \
	.probe_device = s390_iommu_probe_device, \
	.device_group = generic_device_group, \
	.get_resv_regions = s390_iommu_get_resv_regions, \
	.default_domain_ops = &(const struct iommu_domain_ops) { \
		.attach_dev	= s390_iommu_attach_device, \
		.map_pages	= s390_iommu_map_pages, \
		.unmap_pages	= s390_iommu_unmap_pages, \
		.flush_iotlb_all = s390_iommu_flush_iotlb_all, \
		.iotlb_sync      = s390_iommu_iotlb_sync, \
		.iotlb_sync_map  = s390_iommu_iotlb_sync_map, \
		.iova_to_phys	= s390_iommu_iova_to_phys, \
		.free		= s390_domain_free, \
	}
/* [한국어] 위 매크로가 채우는 항목들의 의미:
 *  - blocked_domain: 코어가 "DMA를 막아라"고 할 때 쓸 도메인.
 *  - release_domain: 드라이버가 떨어져 나갈 때 되돌아갈 도메인.
 *    둘 다 같은 정적 차단 도메인을 가리켜, 어느 경로로 끝나든
 *    디바이스가 차단 상태로 남는다.
 *  - capable/get_resv_regions: 코어의 질의에 답하는 조회 콜백들.
 *  - domain_alloc_paging: 페이징 도메인 생성 — 테이블 단계 수를
 *    여기서 정한다.
 *  - probe_device: 디바이스를 차단 상태로 등록한다.
 *  - device_group: s390은 기능마다 독립된 IOMMU라 그룹을 나눌
 *    필요가 없다 — 일반 구현으로 하나씩 떼어 놓는다.
 *  - default_domain_ops: 페이징 도메인의 실제 동작 전부.
 *    map/unmap과 세 가지 무효화, 조회, 해제가 여기 모여 있다. */

/* [한국어] 구형 하드웨어용 연산 테이블 — 페이징과 차단만 제공한다.
 * identity_domain이 없으므로 코어는 이 IOMMU가 통과 모드를
 * 지원하지 않는다고 판단한다. */
static const struct iommu_ops s390_iommu_ops = {
	S390_IOMMU_COMMON_OPS()	/* [한국어] 공통 항목들을 그대로 펼친다 — identity_domain만 빠진다. */
};

/* [한국어] rtr(리셋 후 재등록) 지원 하드웨어용 연산 테이블.
 * 공통 항목에 identity_domain 하나가 더해진 것이 전부다. */
static const struct iommu_ops s390_iommu_rtr_ops = {
	.identity_domain	= &s390_identity_domain,
	/* [한국어] 통과 모드를 요청받았을 때 쓸 정적 도메인.
	 * 이 항목이 있고 없고가 두 ops의 유일한 차이이며,
	 * zpci_init_iommu()가 zdev->rtr_avail을 보고 어느 쪽을
	 * 등록할지 고른다. */

	S390_IOMMU_COMMON_OPS()
};
