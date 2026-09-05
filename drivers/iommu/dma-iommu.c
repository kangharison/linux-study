// SPDX-License-Identifier: GPL-2.0-only
/*
 * A fairly generic DMA-API to IOMMU-API glue layer.
 *
 * Copyright (C) 2014-2015 ARM Ltd.
 *
 * based in part on arch/arm/mm/dma-mapping.c:
 * Copyright (C) 2000-2004 Russell King
 */

/*
 * [한국어 설명] DMA API 와 IOMMU API 를 잇는 접착 계층 (drivers/iommu/dma-iommu.c)
 *
 * === 파일의 역할 ===
 * 장치 드라이버가 부르는 dma_map_page / dma_map_sg / dma_alloc_coherent 를 IOMMU
 * 동작으로 옮기는 계층이다. NVMe 드라이버가 "이 페이지를 장치가 읽게 해 달라"고
 * 말하면, 이 파일이 iova.c 에서 IOVA 를 하나 떼고 iommu.c 로 그 IOVA 에 물리
 * 페이지를 매핑한 뒤 장치에 줄 주소를 돌려준다. IOMMU 를 켠 시스템에서 DMA 주소는
 * 물리 주소가 아니라 여기서 만들어진 IOVA 다.
 *
 * 이 파일이 실제로 결정하는 것은 세 가지다.
 *  1) 주소 배정: IOVA 를 어디서 떼고 어떻게 정렬할지, 32비트 마스크 장치를 어떻게
 *     다룰지, PCIe DAC(64비트 주소)를 쓸지.
 *  2) 무효화 정책: 해제 즉시 IOTLB 를 비울지(strict), 아니면 flush queue 에 모았다가
 *     한꺼번에 비울지(lazy/DMA_FQ). 후자가 이 파일 코드의 상당 부분이며, 고성능
 *     장치에서 IOMMU 를 켜도 처리량이 버티는 이유다.
 *  3) 우회 판단: 신뢰할 수 없는 장치나 정렬이 맞지 않는 버퍼는 swiotlb 바운스
 *     버퍼를 거치게 하고, P2PDMA 세그먼트는 IOMMU 를 아예 건너뛰게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름(매핑):
 *   NVMe 드라이버 dma_map_sg
 *     → DMA API(kernel/dma/mapping.c) → dev->dma_ops == iommu_dma_ops
 *       → [이 파일] iommu_dma_map_sg
 *         → iova.c   alloc_iova_fast    : 연속 IOVA 창 확보
 *         → iommu.c  iommu_map_sg       : 그 창에 흩어진 물리 페이지를 접어 넣음
 *       ← 장치가 볼 연속 DMA 주소 하나를 돌려준다
 * 흐름(해제):
 *   dma_unmap_sg → iommu_dma_unmap_sg → iommu.c iommu_unmap_fast
 *     → 무효화를 gather 에 모아 두고, IOVA 는 flush queue 에 넣는다
 *     → 타이머나 큐 포화 시 flush_iotlb_all 한 번으로 정리하고 IOVA 를 반납
 *
 * 실행 컨텍스트는 커널 모듈이며, 매핑/해제 경로는 인터럽트 문맥에서도 불린다.
 * 그래서 이 파일의 할당은 대부분 GFP_ATOMIC 이고 락은 irqsave 다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: 도메인·그룹을 관리하고 실제 PTE 를 기입한다. 이 파일은 도메인에
 *   iommu_dma_cookie 를 매달아(cookie_type == IOMMU_COOKIE_DMA_IOVA) 자기 상태를 둔다.
 *   iommu_setup_dma_ops 가 장치의 dma_ops 를 이쪽으로 갈아 끼우는 순간부터 그
 *   장치의 DMA 가 IOMMU 를 지난다.
 * - iova.c: IOVA 주소 배정. 이 파일은 IOVA 를 "언제 반납할지"만 정하고, "어디를
 *   줄지"는 전적으로 그쪽이 정한다.
 * - swiotlb: 바운스 버퍼. 신뢰할 수 없는 장치(untrusted)나 IOMMU 페이지 경계에
 *   걸친 부분 페이지 매핑은 다른 데이터가 같은 페이지에 노출되므로, 전용 버퍼로
 *   복사해 넘긴다.
 * - MSI: 인터럽트 도어벨 주소도 이 주소 공간 안에 매핑되어야 한다. msi_page_list 가
 *   그 매핑을 도메인 단위로 캐시한다.
 * - PCI p2pdma: 장치끼리 직접 오가는 세그먼트는 IOMMU 를 거치지 않으므로 매핑
 *   대상에서 제외한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct iommu_dma_cookie : 도메인 하나의 DMA 상태. IOVA 도메인, flush queue,
 *                             MSI 페이지 목록, 정책 옵션이 모두 여기 모인다.
 * - struct iova_fq          : 해제된 IOVA 를 무효화 전까지 담아 두는 링 버퍼.
 * - iommu_dma_init_domain() : 도메인에 IOVA 공간과 예약 구간을 세운다.
 * - iommu_dma_map_page/sg() : 매핑 진입점. IOVA 확보 → iommu_map → 주소 반환.
 * - iommu_dma_unmap_page/sg(): 해제 진입점. iommu_unmap_fast → queue_iova.
 * - queue_iova() / fq_ring_free() : 지연 무효화의 넣기/거두기.
 * - iommu_dma_alloc()       : coherent 할당. 페이지를 모아 하나의 IOVA 창에 접는다.
 * - iommu_dma_prepare_msi() : MSI 도어벨을 이 주소 공간에 매핑한다.
 */
#include <linux/acpi_iort.h>	/* [한국어] ACPI IORT 표 파싱 — MSI 창 같은 예약 구간 정보를 얻는다 */
#include <linux/atomic.h>	/* [한국어] flush queue 의 시작/완료 카운터가 원자 변수다 */
#include <linux/crash_dump.h>	/* [한국어] kdump 커널 판별. 앞선 커널이 남긴 매핑 때문에 정책을 달리해야 한다 */
#include <linux/device.h>	/* [한국어] struct device — DMA 요청의 주체 */
#include <linux/dma-direct.h>	/* [한국어] IOMMU 를 거치지 않는 직접 매핑 경로. 항등 도메인이나 우회 판단에서 쓴다 */
#include <linux/dma-map-ops.h>	/* [한국어] struct dma_map_ops — 이 파일이 채워 장치에 꽂는 vtable */
#include <linux/generic_pt/iommu.h>	/* [한국어] 공용 페이지 테이블 계층 연동 */
#include <linux/gfp.h>	/* [한국어] 할당 플래그. 이 파일은 문맥에 따라 ATOMIC 과 KERNEL 을 오간다 */
#include <linux/huge_mm.h>	/* [한국어] 대형 페이지 처리 — 하나의 큰 페이지를 한 번에 매핑할 수 있는지 판단 */
#include <linux/iommu.h>	/* [한국어] iommu_map/unmap 등 이 파일이 호출하는 코어 API */
#include <linux/iommu-dma.h>	/* [한국어] 이 파일이 외부에 노출하는 선언들 */
#include <linux/iova.h>	/* [한국어] IOVA 할당자 — 주소를 실제로 떼어 주는 계층 */
#include <linux/irq.h>	/* [한국어] MSI 처리에 필요한 인터럽트 자료구조 */
#include <linux/list_sort.h>	/* [한국어] scatterlist 정렬 — 매핑 전 세그먼트를 정돈할 때 */
#include <linux/memremap.h>	/* [한국어] ZONE_DEVICE 페이지 판별. P2PDMA 메모리가 여기 속한다 */
#include <linux/mm.h>	/* [한국어] 페이지 할당과 vmalloc 영역 조작 */
#include <linux/mutex.h>	/* [한국어] flush queue 초기화 등 잠들 수 있는 구간 보호 */
#include <linux/msi.h>	/* [한국어] MSI 서술자 — 도어벨 IOVA 를 여기에 적어 준다 */
#include <linux/of_iommu.h>	/* [한국어] 장치 트리 기반 IOMMU 설정 */
#include <linux/pci.h>	/* [한국어] PCI 장치 판별과 DMA 마스크 처리 */
#include <linux/pci-p2pdma.h>	/* [한국어] P2PDMA 세그먼트 판별 — IOMMU 를 건너뛰어야 하는 구간 */
#include <linux/scatterlist.h>	/* [한국어] sg 리스트 순회와 병합 */
#include <linux/spinlock.h>	/* [한국어] flush queue 와 MSI 목록 보호 */
#include <linux/swiotlb.h>	/* [한국어] 바운스 버퍼. 신뢰할 수 없는 장치나 부분 페이지 매핑의 우회로 */
#include <linux/vmalloc.h>	/* [한국어] coherent 할당이 만든 페이지들을 커널 가상 주소로 잇는다 */
#include <trace/events/swiotlb.h>	/* [한국어] 바운스 발생을 ftrace 로 남긴다 — 성능 저하의 흔한 원인이라 추적이 중요하다 */

#include "dma-iommu.h"	/* [한국어] 이 파일과 iommu.c 사이의 내부 인터페이스 (쿠키 생성/해제 등) */
#include "iommu-pages.h"	/* [한국어] 페이지 테이블용 페이지 할당자. 해제 목록(freelist)을 이 형식으로 주고받는다 */

/*
 * [한국어] MSI 도어벨 한 페이지의 매핑 기록.
 *
 * MSI 는 인터럽트선이 아니라 약속된 주소로의 메모리 쓰기다. IOMMU 아래의 장치가
 * 내는 주소는 전부 IOVA 이므로, 도어벨의 물리 주소를 이 주소 공간에 매핑하고
 * 장치에는 그 IOVA 를 프로그래밍해야 인터럽트가 전달된다.
 *
 * 도메인당 목록으로 캐시한다. 같은 도메인의 여러 장치·여러 벡터가 같은 도어벨을
 * 쓰는 경우가 대부분이라, 매번 새 IOVA 를 떼면 주소 공간이 낭비된다.
 */
struct iommu_dma_msi_page {
	/* [한국어] 쿠키의 msi_page_list 에 매다는 고리.
	 * 설정자: iommu_dma_get_msi_page 가 새 매핑을 만들 때 추가.
	 * 읽는 자: 같은 함수가 기존 매핑을 재사용할 수 있는지 훑을 때.
	 * 동기화: 도메인 단위 락(iommu_dma_prepare_msi 가 그룹 락 아래에서 부른다). */
	struct list_head	list;
	/* [한국어] 이 도어벨이 매핑된 IOVA. 장치에 실제로 프로그래밍되는 주소다.
	 * 설정자: 매핑 생성 시 alloc_iova 결과 또는 예약된 MSI 창의 주소.
	 * 읽는 자: msi_desc_set_iommu_msi_iova 를 통해 인터럽트 코어로 전달된다.
	 * 값 범위: 이 도메인의 IOVA 공간 안. 도메인마다 다를 수 있다. */
	dma_addr_t		iova;
	/* [한국어] 도어벨의 실제 물리 주소. 목록에서 재사용 여부를 판정하는 키다.
	 * 설정자: 매핑 생성 시 요청받은 msi_addr.
	 * 읽는 자: 기존 항목을 훑으며 같은 도어벨인지 비교할 때.
	 * 페이지 단위로 정렬된 값이며, 한 페이지 안의 여러 도어벨은 한 항목을 공유한다. */
	phys_addr_t		phys;
};

/*
 * [한국어] flush queue 를 어떤 모양으로 둘지.
 *
 * 무효화를 모아서 하려면 해제된 IOVA 를 어딘가 담아 둬야 하는데, 그 큐를 CPU 별로
 * 둘지 하나만 둘지의 선택이다. 시스템 규모와 IOMMU 무효화 비용에 따라 유리한
 * 쪽이 달라진다.
 */
enum iommu_dma_queue_type {
	/* [한국어] CPU 마다 작은 큐 하나 (기본값).
	 * 장점: 큐 락 경쟁이 없다. 각 CPU 가 자기 큐에만 넣는다.
	 * 단점: 큐가 작아(256) 자주 차고, 그때마다 무효화가 일어난다. CPU 수가 아주
	 *   많으면 무효화 빈도가 오히려 올라간다. */
	IOMMU_DMA_OPTS_PER_CPU_QUEUE,
	/* [한국어] 시스템 전체에 큐 하나 (32768 항목).
	 * 장점: 큐가 커서 무효화 한 번에 정리되는 양이 많다. 무효화 자체가 매우 비싼
	 *   하드웨어(일부 ARM SMMU 구성)에서 유리하다.
	 * 단점: 모든 CPU 가 한 락을 다툰다.
	 * 선택 근거: 드라이버가 IOMMU_CAP_DEFERRED_FLUSH 관련 특성을 알릴 때 이쪽을 쓴다. */
	IOMMU_DMA_OPTS_SINGLE_QUEUE,
};

/*
 * [한국어] 이 도메인의 지연 무효화 정책 묶음.
 * 도메인을 세울 때 한 번 정해지고 이후 바뀌지 않는다.
 */
struct iommu_dma_options {
	/* [한국어] CPU 별 큐인지 전역 큐 하나인지.
	 * 설정자: iommu_dma_init_options 가 하드웨어 특성을 보고 결정.
	 * 읽는 자: queue_iova, fq_flush_timeout 등이 어느 큐를 만질지 고를 때. */
	enum iommu_dma_queue_type qt;
	/* [한국어] 큐 하나의 항목 수. 반드시 2의 거듭제곱이어야 한다 —
	 * 링 버퍼 인덱스를 mod_mask 로 감싸기 때문이다.
	 * 값: 기본 256, 전역 큐면 32768. */
	size_t		fq_size;
	/* [한국어] 큐를 강제로 비우는 주기(ms).
	 * 왜 필요한가: 큐가 차기만 기다리면, DMA 가 뜸해진 뒤 해제된 IOVA 가 무한정
	 *   묶인 채 남는다. 타이머가 그 하한을 보장한다.
	 * 값: CPU 별 큐 10ms, 전역 큐 1000ms — 큐 크기에 반비례한다. */
	unsigned int	fq_timeout;
};

/*
 * [한국어] 도메인 하나의 DMA 상태 전부.
 *
 * iommu_domain 의 cookie 자리에 매달리며, cookie_type == IOMMU_COOKIE_DMA_IOVA 가
 * "이 도메인은 커널 DMA API 용이고 그 상태가 여기 있다"는 표시다. iommu.c 는 이
 * 구조체의 내용을 전혀 모르고, 도메인이 해제될 때 iommu_put_dma_cookie 를 부를 뿐이다.
 */
struct iommu_dma_cookie {
	/* [한국어] 이 도메인의 IOVA 주소 공간 (iova.c 가 관리).
	 * 포인터가 아니라 구조체로 박아 두어, 쿠키가 있으면 IOVA 공간도 반드시 있다.
	 * 설정자: iommu_dma_init_domain 이 init_iova_domain 으로 세운다.
	 * 읽는 자: 모든 매핑/해제 경로가 alloc_iova_fast/free_iova_fast 로 접근.
	 * 동기화: iova.c 내부의 rbtree 락과 CPU 별 캐시 락이 담당한다. */
	struct iova_domain iovad;
	/* [한국어] 이 도메인에 매핑해 둔 MSI 도어벨 페이지 목록.
	 * 설정자/읽는 자: iommu_dma_get_msi_page.
	 * 왜 목록인가: 같은 도메인의 여러 장치가 서로 다른 도어벨을 쓸 수 있고(멀티
	 *   ITS 구성), 개수가 작아 선형 탐색으로 충분하다.
	 * 동기화: 호출자가 그룹 락을 든 상태에서만 접근한다. */
	struct list_head msi_page_list;
	/* Flush queue */
	/* [한국어] 큐 모양에 따라 둘 중 하나만 쓴다 (options.qt 가 어느 쪽인지 말해 준다).
	 * 공용체로 둔 이유: 두 구성이 동시에 존재할 일이 없고, 쿠키는 도메인마다
	 *   하나씩이라 크기를 아끼는 의미가 있다. */
	union {
		/* [한국어] 전역 큐 하나 (SINGLE_QUEUE 구성).
		 * 모든 CPU 가 이 큐의 lock 을 다툰다.
		 * 설정자: iommu_dma_init_fq_single. */
		struct iova_fq *single_fq;
		/* [한국어] CPU 별 큐 배열 (PER_CPU_QUEUE 구성, 기본).
		 * 각 CPU 가 자기 것만 만지므로 락 경쟁이 사실상 없다.
		 * 설정자: iommu_dma_init_fq_percpu. */
		struct iova_fq __percpu *percpu_fq;
	};
	/* Number of TLB flushes that have been started */
	/* [한국어] 지금까지 시작된 IOTLB 전체 무효화 횟수 (위 영어 주석).
	 * 큐에 항목을 넣을 때 이 값을 함께 적어 둔다. "이 IOVA 는 N번째 무효화 이후에
	 * 해제되었다"는 표식이다.
	 * 설정자: fq_flush_iotlb 가 무효화 직전에 증가.
	 * 동기화: 원자 변수. 여러 CPU 가 동시에 무효화를 시작할 수 있다. */
	atomic64_t fq_flush_start_cnt;
	/* Number of TLB flushes that have been finished */
	/* [한국어] 완료된 무효화 횟수 (위 영어 주석).
	 * 두 카운터를 나눠 둔 것이 이 설계의 핵심 안전장치다. 항목의 counter 가
	 * finish_cnt 보다 작아야만 그 IOVA 를 재사용해도 안전하다 — 그 항목이 큐에
	 * 들어간 뒤 시작된 무효화가 '끝났음'이 보장되기 때문이다. 하나로 합치면
	 * 무효화가 진행 중인 사이에 IOVA 가 풀려 나갈 수 있다.
	 * 설정자: fq_flush_iotlb 가 무효화 완료 직후에 증가. */
	atomic64_t fq_flush_finish_cnt;
	/* Timer to regularily empty the flush queues */
	/* [한국어] 큐를 주기적으로 비우는 타이머 (위 영어 주석).
	 * 큐가 차기만 기다리면 DMA 가 잦아든 뒤 IOVA 가 무한정 묶이므로, 이 타이머가
	 * 회수의 하한을 보장한다.
	 * 설정자: queue_iova 가 항목을 넣으며 필요하면 예약.
	 * 콜백: fq_flush_timeout — 무효화 한 번 내리고 모든 큐를 훑는다. */
	struct timer_list fq_timer;
	/* 1 when timer is active, 0 when not */
	/* [한국어] 타이머가 이미 걸려 있는지 (위 영어 주석).
	 * 왜 원자 변수인가: 여러 CPU 가 동시에 queue_iova 에 들어와 각자 타이머를
	 *   걸려 하면 mod_timer 가 중복 호출된다. cmpxchg 로 한 CPU 만 걸게 만든다.
	 * 값: 0 = 꺼짐, 1 = 걸림. */
	atomic_t fq_timer_on;
	/* Domain for flush queue callback; NULL if flush queue not in use */
	/* [한국어] 무효화를 내릴 도메인. NULL 이면 지연 무효화를 쓰지 않는다 (위 영어 주석).
	 * 이 필드의 NULL 여부가 곧 strict / lazy 정책의 판정 기준이다.
	 * 설정자: iommu_dma_init_fq 가 큐 준비를 마친 마지막에 채운다 — 큐가 완성되기
	 *   전에 채우면 다른 CPU 가 반쯤 만들어진 큐를 쓴다.
	 * 읽는 자: 해제 경로가 queue_iova 로 갈지 즉시 무효화할지 가른다. */
	struct iommu_domain *fq_domain;
	/* Options for dma-iommu use */
	/* [한국어] 이 도메인의 큐 정책 (위 영어 주석).
	 * 설정자: iommu_dma_init_options, 도메인 초기화 시 한 번.
	 * 이후 불변이므로 락 없이 읽는다. */
	struct iommu_dma_options options;
};

/*
 * [한국어] MSI 매핑만 필요한 도메인의 축소판 쿠키.
 *
 * IOVA 할당자도 flush queue 도 없다. VFIO/iommufd 가 소유한 도메인처럼 커널이
 * DMA 를 관리하지 않는 경우에도 MSI 도어벨만은 매핑해야 하기 때문에 존재한다.
 * cookie_type == IOMMU_COOKIE_DMA_MSI 가 이 형태임을 알린다.
 */
struct iommu_dma_msi_cookie {
	/* [한국어] MSI 매핑에 쓸 IOVA 창의 기준 주소.
	 * 할당자가 없으므로 소유자가 미리 정해 준 고정 창을 순서대로 쓴다.
	 * 설정자: iommu_get_msi_cookie 를 부르는 소유자(VFIO 등). */
	dma_addr_t msi_iova;
	/* [한국어] 매핑해 둔 도어벨 페이지 목록. 큰 쿠키의 같은 이름 필드와 같은 역할이며,
	 * 두 쿠키 형태 모두에서 이 목록의 위치를 알아야 하므로 코드가 cookie_type 을
	 * 보고 갈라 접근한다. */
	struct list_head msi_page_list;
};

static DEFINE_STATIC_KEY_FALSE(iommu_deferred_attach_enabled);	/* [한국어] 지연 부착이 필요한 하드웨어가 있을 때만 켜지는 정적 키. 매 매핑마다 검사가 들어가는 자리라, 대부분의 시스템에서 그 분기 자체를 지워 버리기 위해 static key 를 쓴다 */
bool iommu_dma_forcedac __read_mostly;	/* [한국어] PCIe 장치에 64비트 주소(DAC)를 강제할지. 켜면 32비트 영역을 아끼지만 오래된 장치에서 문제가 생길 수 있다 */

static int __init iommu_dma_forcedac_setup(char *str)
{
	int ret = kstrtobool(str, &iommu_dma_forcedac);	/* [한국어] 부트 인자 값 해석 */

	if (!ret && iommu_dma_forcedac)	/* [한국어] 해석에 성공했고 켜졌다면 */
		pr_info("Forcing DAC for PCI devices\n");	/* [한국어] 주소 배정 정책이 평소와 달라졌음을 남긴다 */
	return ret;	/* [한국어] 해석 결과 */
}
early_param("iommu.forcedac", iommu_dma_forcedac_setup);	/* [한국어] 부트 인자 등록 */

/* Number of entries per flush queue */
#define IOVA_DEFAULT_FQ_SIZE	256	/* [한국어] CPU 별 큐의 항목 수. 작게 잡는 이유는 CPU 마다 하나씩 있어 합계가 커지기 때문이다 */
#define IOVA_SINGLE_FQ_SIZE	32768	/* [한국어] 전역 큐 하나만 쓰는 구성에서의 항목 수. 모든 CPU 가 공유하므로 훨씬 크게 잡는다 */

/* Timeout (in ms) after which entries are flushed from the queue */
#define IOVA_DEFAULT_FQ_TIMEOUT	10	/* [한국어] CPU 별 큐를 비우는 주기(ms). 짧을수록 IOVA 회수가 빠르고 무효화 횟수가 는다 */
#define IOVA_SINGLE_FQ_TIMEOUT	1000	/* [한국어] 전역 큐는 훨씬 크므로 주기도 길게 — 무효화 한 번에 정리되는 양이 많다 */

/* Flush queue entry for deferred flushing */
/*
 * [한국어] (위 영어 주석에 이어) 해제 대기 중인 IOVA 구간 하나.
 *
 * dma_unmap 이 불리면 PTE 는 곧바로 지우지만, IOTLB 에는 옛 번역이 남아 있어
 * 그 IOVA 를 바로 재사용하면 장치가 이미 반납된 페이지에 닿을 수 있다. 그래서
 * IOVA 를 여기 담아 두고, 전체 무효화가 한 번 끝난 뒤에야 iova.c 로 돌려준다.
 */
struct iova_fq_entry {
	/* [한국어] 반납 대기 중인 구간의 시작 pfn.
	 * 설정자: queue_iova.
	 * 읽는 자: fq_ring_free_locked 가 free_iova_fast 로 돌려줄 때. */
	unsigned long iova_pfn;
	/* [한국어] 그 구간의 페이지 수. free_iova_fast 가 캐시 등급을 정할 때 쓴다.
	 * 넣을 때와 꺼낼 때의 크기가 같아야 IOVA 캐시의 부기가 맞는다. */
	unsigned long pages;
	/* [한국어] 이 해제로 비게 된 페이지 테이블 페이지들.
	 * 왜 함께 미루는가: 페이지 테이블 페이지를 곧바로 반납하면, 아직 무효화되지
	 *   않은 IOTLB 항목이 참조하던 표를 다른 용도로 재사용하게 된다. IOVA 와
	 *   똑같이 무효화가 끝난 뒤에 놓아야 안전하다.
	 * 설정자: queue_iova 가 호출자의 목록을 여기로 옮겨 붙인다(splice).
	 * 읽는 자: fq_ring_free_locked 가 iommu_put_pages_list 로 반납. */
	struct iommu_pages_list freelist;
	/* [한국어] 이 항목이 큐에 들어갈 때의 fq_flush_start_cnt (위 영어 주석).
	 * 판정 규칙: counter < fq_flush_finish_cnt 이면 이 항목이 들어간 뒤 시작된
	 *   무효화가 이미 끝났다는 뜻이므로 반납해도 안전하다. 이 한 비교가 지연
	 *   무효화 전체의 정확성을 떠받친다. */
	u64 counter; /* Flush counter when this entry was added */
};

/* Per-CPU flush queue structure */
/*
 * [한국어] (위 영어 주석에 이어) 해제 대기 IOVA 를 담는 링 버퍼.
 *
 * head 는 반납할 차례, tail 은 넣을 자리다. 항목 수가 2의 거듭제곱이라 인덱스를
 * mod_mask 로 감싸며, 한 칸을 비워 둬 가득 참과 빔을 구별한다.
 */
struct iova_fq {
	/* [한국어] 이 큐를 지키는 락.
	 * CPU 별 큐라도 필요하다 — dma_unmap 이 인터럽트 문맥에서 오므로 같은 CPU
	 *   안에서 경쟁하고, 타이머 콜백과 다른 CPU 의 회수도 이 큐를 만진다.
	 * 항상 spin_lock_irqsave 로 쓴다. */
	spinlock_t lock;
	/* [한국어] 링 버퍼의 읽기/쓰기 커서.
	 * head: 다음에 반납을 시도할 항목. fq_ring_free_locked 가 전진시킨다.
	 * tail: 다음에 넣을 자리. fq_ring_add 가 전진시킨다.
	 * head == tail 이면 비었고, (tail+1) == head 면 가득 찬 것으로 본다 —
	 *   한 칸을 희생해 두 상태를 구별한다.
	 * 동기화: 위의 lock. */
	unsigned int head, tail;
	/* [한국어] 인덱스를 링 크기로 감싸는 마스크 (= fq_size - 1).
	 * fq_size 가 2의 거듭제곱이어야 하는 이유가 이 마스크다. 나눗셈 대신
	 * AND 한 번으로 감싸므로 핫패스에서 비용이 없다. */
	unsigned int mod_mask;
	/* [한국어] 가변 길이 항목 배열. 크기는 options.fq_size.
	 * 구조체 뒤에 붙여 한 번의 할당으로 큐 전체를 잡는다 — CPU 별 큐라면
	 * percpu 영역에, 전역 큐라면 일반 커널 메모리에. */
	struct iova_fq_entry entries[];
};

#define fq_ring_for_each(i, fq) \	/* [한국어] head 부터 tail 직전까지 링을 도는 매크로. mod_mask 로 감싸므로 배열 끝에서 앞으로 되돌아온다 */
	for ((i) = (fq)->head; (i) != (fq)->tail; (i) = ((i) + 1) & (fq)->mod_mask)	/* [한국어] head == tail 이면 비었다는 규약이라 종료 조건이 곧 '빔' 판정이기도 하다 */

static inline bool fq_full(struct iova_fq *fq)
{
	assert_spin_locked(&fq->lock);	/* [한국어] 호출자가 큐 락을 든 상태여야 한다 */
	return (((fq->tail + 1) & fq->mod_mask) == fq->head);	/* [한국어] 한 칸을 비워 둬 '가득 참'과 '빔'을 구별한다. 그 한 칸이 없으면 head==tail 이 두 뜻을 갖는다 */
}

static inline unsigned int fq_ring_add(struct iova_fq *fq)
{
	unsigned int idx = fq->tail;	/* [한국어] 넣을 자리 */

	assert_spin_locked(&fq->lock);	/* [한국어] 호출자가 락을 든 상태여야 한다 */

	fq->tail = (idx + 1) & fq->mod_mask;	/* [한국어] 커서를 한 칸 전진시키고 감싼다 */

	return idx;	/* [한국어] 방금 확보한 자리의 인덱스 */
}

static void fq_ring_free_locked(struct iommu_dma_cookie *cookie, struct iova_fq *fq)
{
	u64 counter = atomic64_read(&cookie->fq_flush_finish_cnt);	/* [한국어] '완료된' 무효화 횟수. 이 값보다 작은 counter 를 가진 항목만 반납해도 안전하다 */
	unsigned int idx;	/* [한국어] 링 순회 커서 */

	assert_spin_locked(&fq->lock);	/* [한국어] 호출자가 락을 든 상태여야 한다 */

	fq_ring_for_each(idx, fq) {	/* [한국어] 가장 오래된 항목부터 */

		if (fq->entries[idx].counter >= counter)	/* [한국어] 이 항목이 들어간 뒤 시작된 무효화가 아직 끝나지 않았다 */
			break;	/* [한국어] 링은 시간순이므로 여기서 멈추면 뒤는 볼 필요가 없다 */

		iommu_put_pages_list(&fq->entries[idx].freelist);	/* [한국어] 비었던 페이지 테이블 페이지를 이제야 반납한다 — 무효화가 끝났으므로 아무도 그 표를 참조하지 않는다 */
		free_iova_fast(&cookie->iovad,	/* [한국어] IOVA 도 이제 재사용 가능하다 */
			       fq->entries[idx].iova_pfn,	/* [한국어] 구간의 시작 */
			       fq->entries[idx].pages);	/* [한국어] 페이지 수 (캐시 등급 판정에 쓰인다) */

		fq->entries[idx].freelist =	/* [한국어] 항목을 재사용할 수 있도록 */
			IOMMU_PAGES_LIST_INIT(fq->entries[idx].freelist);	/* [한국어] 목록을 빈 상태로 되돌린다 */
		fq->head = (fq->head + 1) & fq->mod_mask;	/* [한국어] 반납한 만큼 head 를 전진 */
	}
}

static void fq_ring_free(struct iommu_dma_cookie *cookie, struct iova_fq *fq)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&fq->lock, flags);	/* [한국어] 큐 보호 */
	fq_ring_free_locked(cookie, fq);	/* [한국어] 락을 잡고 반납 수행 */
	spin_unlock_irqrestore(&fq->lock, flags);	/* [한국어] 락 해제 */
}

static void fq_flush_iotlb(struct iommu_dma_cookie *cookie)
{
	atomic64_inc(&cookie->fq_flush_start_cnt);	/* [한국어] 무효화를 '시작한다'고 먼저 알린다. 이 시점 이후 큐에 들어가는 항목은 더 큰 counter 를 갖게 되어, 이번 무효화의 보호를 받지 못한다 — 그것이 정확하다 */
	cookie->fq_domain->ops->flush_iotlb_all(cookie->fq_domain);	/* [한국어] 도메인 전체 IOTLB 를 한 번에 비운다. 구간별 무효화를 수천 번 하는 대신 전체를 한 번 — 그것이 지연 무효화의 이득이다 */
	atomic64_inc(&cookie->fq_flush_finish_cnt);	/* [한국어] 완료를 알린다. 이 증가를 본 CPU 는 그 이전 counter 의 항목을 안전하게 반납할 수 있다 */
}

static void fq_flush_timeout(struct timer_list *t)
{
	struct iommu_dma_cookie *cookie = timer_container_of(cookie, t,	/* [한국어] 타이머 구조체에서 소유 쿠키로 되짚는다 */
							     fq_timer);	/* [한국어] 이 쿠키의 타이머 필드 */
	int cpu;	/* [한국어] CPU 순회 커서 */

	atomic_set(&cookie->fq_timer_on, 0);	/* [한국어] 먼저 내린다 — 아래 작업 중에 새 항목이 들어오면 타이머를 다시 걸 수 있어야 한다 */
	fq_flush_iotlb(cookie);	/* [한국어] 전체 무효화 한 번 */

	if (cookie->options.qt == IOMMU_DMA_OPTS_SINGLE_QUEUE) {	/* [한국어] 전역 큐 구성이면 */
		fq_ring_free(cookie, cookie->single_fq);	/* [한국어] 그 큐 하나만 훑는다 */
	} else {
		for_each_possible_cpu(cpu)	/* [한국어] CPU 별 구성이면 모든 CPU 의 큐를 */
			fq_ring_free(cookie, per_cpu_ptr(cookie->percpu_fq, cpu));	/* [한국어] 각각 훑어 반납한다. 무효화는 한 번뿐이므로 전 CPU 의 대기분이 이 한 번으로 풀린다 */
	}
}

static void queue_iova(struct iommu_dma_cookie *cookie,
		unsigned long pfn, unsigned long pages,
		struct iommu_pages_list *freelist)
{
	struct iova_fq *fq;	/* [한국어] 넣을 큐 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	unsigned int idx;	/* [한국어] 확보한 자리 */

	/*
	 * Order against the IOMMU driver's pagetable update from unmapping
	 * @pte, to guarantee that fq_flush_iotlb() observes that if called
	 * from a different CPU before we release the lock below. Full barrier
	 * so it also pairs with iommu_dma_init_fq() to avoid seeing partially
	 * written fq state here.
	 */
	smp_mb();	/* [한국어] 드라이버가 PTE 를 지운 것과 이 항목이 큐에 들어가는 것 사이의 순서를 못박는다. 다른 CPU 가 곧바로 무효화를 내리더라도 지워진 PTE 를 반드시 보게 된다. 전체 배리어인 것은 iommu_dma_init_fq 가 쓴 큐 상태를 반쯤 본 채로 만지지 않기 위해서이기도 하다 (위 영어 주석) */

	if (cookie->options.qt == IOMMU_DMA_OPTS_SINGLE_QUEUE)	/* [한국어] 전역 큐 구성 */
		fq = cookie->single_fq;	/* [한국어] 모두가 공유하는 큐 */
	else
		fq = raw_cpu_ptr(cookie->percpu_fq);	/* [한국어] 현재 CPU 의 큐 — 아래 락이 정확성을 책임지므로 선점 비활성이 필요 없다 */

	spin_lock_irqsave(&fq->lock, flags);	/* [한국어] 큐 보호 */

	/*
	 * First remove all entries from the flush queue that have already been
	 * flushed out on another CPU. This makes the fq_full() check below less
	 * likely to be true.
	 */
	fq_ring_free_locked(cookie, fq);	/* [한국어] 먼저 다른 CPU 가 이미 무효화를 끝내 준 항목들을 거둔다. 그러면 아래 가득 참 검사에 걸릴 확률이 낮아진다 (위 영어 주석) */

	if (fq_full(fq)) {	/* [한국어] 그래도 자리가 없다 */
		fq_flush_iotlb(cookie);	/* [한국어] 여기서 직접 무효화를 내린다 — 큐가 넘치면 지연의 이득을 포기하고 즉시 처리한다 */
		fq_ring_free_locked(cookie, fq);	/* [한국어] 이제 모든 항목을 반납할 수 있다 */
	}

	idx = fq_ring_add(fq);	/* [한국어] 자리 확보 */

	fq->entries[idx].iova_pfn = pfn;	/* [한국어] 반납 대기 구간의 시작 */
	fq->entries[idx].pages    = pages;	/* [한국어] 페이지 수 */
	fq->entries[idx].counter  = atomic64_read(&cookie->fq_flush_start_cnt);	/* [한국어] 현재 '시작' 카운터를 찍어 둔다. 나중에 finish 카운터가 이 값을 넘어서면 반납해도 안전해진다 */
	iommu_pages_list_splice(freelist, &fq->entries[idx].freelist);	/* [한국어] 비워진 페이지 테이블 페이지도 함께 미룬다 — IOVA 와 같은 조건에서 풀려야 한다 */

	spin_unlock_irqrestore(&fq->lock, flags);	/* [한국어] 큐 조작 끝 */

	/* Avoid false sharing as much as possible. */
	if (!atomic_read(&cookie->fq_timer_on) &&	/* [한국어] 먼저 읽어 본다. 대부분 이미 켜져 있어 아래 원자 교환까지 갈 필요가 없고, 그 읽기는 캐시라인을 공유 상태로 두므로 false sharing 이 적다 (위 영어 주석) */
	    !atomic_xchg(&cookie->fq_timer_on, 1))	/* [한국어] 실제로 켜는 것은 한 CPU 뿐이다 — 교환 결과가 0 이었던 쪽만 통과한다 */
		mod_timer(&cookie->fq_timer,	/* [한국어] 타이머를 건다 */
			  jiffies + msecs_to_jiffies(cookie->options.fq_timeout));	/* [한국어] 큐가 차지 않아도 이 시간 안에는 반드시 회수된다 */
}

static void iommu_dma_free_fq_single(struct iova_fq *fq)
{
	int idx;	/* [한국어] 링 순회 커서 */

	fq_ring_for_each(idx, fq)	/* [한국어] 아직 반납되지 않은 항목들에 대해 */
		iommu_put_pages_list(&fq->entries[idx].freelist);	/* [한국어] 페이지 테이블 페이지만 돌려준다. IOVA 는 도메인이 통째로 사라지는 중이라 따로 반납할 필요가 없다 */
	vfree(fq);	/* [한국어] 큐 자체 해제 (vmalloc 으로 잡았다) */
}

static void iommu_dma_free_fq_percpu(struct iova_fq __percpu *percpu_fq)
{
	int cpu, idx;	/* [한국어] CPU 와 링 순회 커서 */

	/* The IOVAs will be torn down separately, so just free our queued pages */
	for_each_possible_cpu(cpu) {	/* [한국어] 모든 CPU 의 큐에 대해 */
		struct iova_fq *fq = per_cpu_ptr(percpu_fq, cpu);	/* [한국어] 그 CPU 의 큐 */

		fq_ring_for_each(idx, fq)	/* [한국어] 남은 항목들에 대해 */
			iommu_put_pages_list(&fq->entries[idx].freelist);	/* [한국어] 페이지 테이블 페이지만 반납 (위 영어 주석 — IOVA 는 별도로 정리된다) */
	}

	free_percpu(percpu_fq);	/* [한국어] percpu 배열 해제 */
}

static void iommu_dma_free_fq(struct iommu_dma_cookie *cookie)
{
	if (!cookie->fq_domain)	/* [한국어] 지연 무효화를 쓰지 않는 도메인 */
		return;	/* [한국어] 큐 자체가 없다 */

	timer_delete_sync(&cookie->fq_timer);	/* [한국어] 타이머가 돌고 있으면 끝날 때까지 기다린다 — 큐를 해제한 뒤 콜백이 돌면 해제된 메모리를 만진다 */
	if (cookie->options.qt == IOMMU_DMA_OPTS_SINGLE_QUEUE)	/* [한국어] 구성에 맞는 해제 함수로 */
		iommu_dma_free_fq_single(cookie->single_fq);	/* [한국어] 전역 큐 */
	else
		iommu_dma_free_fq_percpu(cookie->percpu_fq);	/* [한국어] CPU 별 큐 */
}

static void iommu_dma_init_one_fq(struct iova_fq *fq, size_t fq_size)
{
	int i;	/* [한국어] 항목 초기화 커서 */

	fq->head = 0;	/* [한국어] 빈 링 */
	fq->tail = 0;	/* [한국어] head == tail 이 곧 '비었음'이다 */
	fq->mod_mask = fq_size - 1;	/* [한국어] 2의 거듭제곱 크기이므로 이 마스크로 인덱스를 감싼다 */

	spin_lock_init(&fq->lock);	/* [한국어] 이 큐 전용 락 */

	for (i = 0; i < fq_size; i++)	/* [한국어] 모든 항목에 대해 */
		fq->entries[i].freelist =	/* [한국어] 페이지 목록을 */
			IOMMU_PAGES_LIST_INIT(fq->entries[i].freelist);	/* [한국어] 빈 상태로 초기화. 항목은 재사용되므로 처음 한 번만 하면 된다 */
}

static int iommu_dma_init_fq_single(struct iommu_dma_cookie *cookie)
{
	size_t fq_size = cookie->options.fq_size;	/* [한국어] 전역 큐는 32768 항목으로 크다 */
	struct iova_fq *queue;	/* [한국어] 만들 큐 */

	queue = vmalloc(struct_size(queue, entries, fq_size));	/* [한국어] 크기가 수 MB 에 이르러 물리 연속을 요구할 수 없다 — vmalloc 으로 가상 연속만 확보한다 */
	if (!queue)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 지연 무효화를 켤 수 없다 */
	iommu_dma_init_one_fq(queue, fq_size);	/* [한국어] 링 상태 초기화 */
	cookie->single_fq = queue;	/* [한국어] 쿠키에 매단다 */

	return 0;	/* [한국어] 전역 큐 준비 완료 */
}

static int iommu_dma_init_fq_percpu(struct iommu_dma_cookie *cookie)
{
	size_t fq_size = cookie->options.fq_size;	/* [한국어] CPU 별 큐는 256 항목으로 작다 */
	struct iova_fq __percpu *queue;	/* [한국어] 만들 percpu 큐 배열 */
	int cpu;	/* [한국어] CPU 순회 커서 */

	queue = __alloc_percpu(struct_size(queue, entries, fq_size),	/* [한국어] CPU 마다 큐 하나. 가변 배열까지 포함한 크기를 오버플로 안전하게 계산한다 */
			       __alignof__(*queue));	/* [한국어] 구조체의 자연 정렬 */
	if (!queue)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 지연 무효화를 켤 수 없다 */

	for_each_possible_cpu(cpu)	/* [한국어] online 이 아니라 possible — 나중에 올라올 CPU 도 자기 큐를 이미 갖고 있어야 한다 */
		iommu_dma_init_one_fq(per_cpu_ptr(queue, cpu), fq_size);	/* [한국어] 각각 초기화 */
	cookie->percpu_fq = queue;	/* [한국어] 쿠키에 매단다 */
	return 0;	/* [한국어] CPU 별 큐 준비 완료 */
}

/* sysfs updates are serialised by the mutex of the group owning @domain */
int iommu_dma_init_fq(struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] 이 도메인의 DMA 상태 */
	int rc;	/* [한국어] 큐 생성 결과 */

	if (cookie->fq_domain)	/* [한국어] 이미 켜져 있으면 (sysfs 로 DMA-FQ 를 두 번 요청한 경우) */
		return 0;	/* [한국어] 할 일이 없다 — 멱등이다 */

	atomic64_set(&cookie->fq_flush_start_cnt,  0);	/* [한국어] 두 카운터를 0 에서 시작 */
	atomic64_set(&cookie->fq_flush_finish_cnt, 0);	/* [한국어] 항목의 counter 는 0 이상이므로, finish 가 0 인 동안에는 아무 것도 반납되지 않는다 */

	if (cookie->options.qt == IOMMU_DMA_OPTS_SINGLE_QUEUE)	/* [한국어] 정책에 맞는 큐를 만든다 */
		rc = iommu_dma_init_fq_single(cookie);	/* [한국어] 전역 큐 */
	else
		rc = iommu_dma_init_fq_percpu(cookie);	/* [한국어] CPU 별 큐 */

	if (rc) {	/* [한국어] 큐 생성 실패 */
		pr_warn("iova flush queue initialization failed\n");	/* [한국어] 치명적이지는 않다 — 도메인은 즉시 무효화(strict)로 계속 동작한다 */
		return -ENOMEM;	/* [한국어] 호출자가 DMA-FQ 전환을 포기한다 */
	}

	timer_setup(&cookie->fq_timer, fq_flush_timeout, 0);	/* [한국어] 주기적 회수 타이머 준비 */
	atomic_set(&cookie->fq_timer_on, 0);	/* [한국어] 아직 걸리지 않은 상태 */
	/*
	 * Prevent incomplete fq state being observable. Pairs with path from
	 * __iommu_dma_unmap() through iommu_dma_free_iova() to queue_iova()
	 */
	smp_wmb();	/* [한국어] 위의 모든 초기화가 아래 대입보다 먼저 보이게 한다 */
	WRITE_ONCE(cookie->fq_domain, domain);	/* [한국어] 이 대입이 곧 '지연 무효화 켜짐' 스위치다. 반드시 마지막에 해야 한다 — 해제 경로가 이 필드를 보고 queue_iova 로 들어가므로, 큐가 완성되기 전에 켜면 반쯤 만들어진 링을 만진다 (위 영어 주석) */
	return 0;	/* [한국어] 이제 이 도메인의 해제는 큐를 거친다 */
}

/**
 * iommu_get_dma_cookie - Acquire DMA-API resources for a domain
 * @domain: IOMMU domain to prepare for DMA-API usage
 */
int iommu_get_dma_cookie(struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie;	/* [한국어] 만들 쿠키 */

	if (domain->cookie_type != IOMMU_COOKIE_NONE)	/* [한국어] cookie 자리를 이미 다른 용도가 쓰고 있다 */
		return -EEXIST;	/* [한국어] 한 도메인에 쿠키는 하나뿐이다 */

	cookie = kzalloc_obj(*cookie);	/* [한국어] 0 으로 채운 쿠키 — iovad.granule 이 0 이라 '아직 IOVA 공간 없음'을 뜻한다 */
	if (!cookie)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 도메인을 DMA API 용으로 쓸 수 없다 */

	INIT_LIST_HEAD(&cookie->msi_page_list);	/* [한국어] MSI 매핑 목록은 빈 상태로 */
	domain->cookie_type = IOMMU_COOKIE_DMA_IOVA;	/* [한국어] cookie 자리의 용도를 선언 — iommu.c 의 여러 분기가 이 값을 본다 */
	domain->iova_cookie = cookie;	/* [한국어] 도메인에 매단다 */
	return 0;	/* [한국어] IOVA 공간 자체는 iommu_dma_init_domain 이 나중에 세운다 */
}

/**
 * iommu_get_msi_cookie - Acquire just MSI remapping resources
 * @domain: IOMMU domain to prepare
 * @base: Start address of IOVA region for MSI mappings
 *
 * Users who manage their own IOVA allocation and do not want DMA API support,
 * but would still like to take advantage of automatic MSI remapping, can use
 * this to initialise their own domain appropriately. Users should reserve a
 * contiguous IOVA region, starting at @base, large enough to accommodate the
 * number of PAGE_SIZE mappings necessary to cover every MSI doorbell address
 * used by the devices attached to @domain.
 */
int iommu_get_msi_cookie(struct iommu_domain *domain, dma_addr_t base)
{
	struct iommu_dma_msi_cookie *cookie;	/* [한국어] 만들 축소판 쿠키 */

	if (domain->type != IOMMU_DOMAIN_UNMANAGED)	/* [한국어] 커널이 관리하는 도메인은 큰 쿠키를 쓴다 */
		return -EINVAL;	/* [한국어] 소유자가 직접 IOVA 를 관리하는 도메인 전용이다 (위 영어 주석) */

	if (domain->cookie_type != IOMMU_COOKIE_NONE)	/* [한국어] 이미 쿠키가 있다 */
		return -EEXIST;	/* [한국어] 중복 설정 */

	cookie = kzalloc_obj(*cookie);	/* [한국어] 0 으로 채운 축소판 쿠키 */
	if (!cookie)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] MSI 재매핑을 쓸 수 없다 */

	cookie->msi_iova = base;	/* [한국어] 소유자가 예약해 둔 창의 시작 주소. 할당자가 없으므로 여기서부터 순서대로 쓴다 (위 영어 주석) */
	INIT_LIST_HEAD(&cookie->msi_page_list);	/* [한국어] 매핑 목록 초기화 */
	domain->cookie_type = IOMMU_COOKIE_DMA_MSI;	/* [한국어] MSI 전용 쿠키임을 선언 */
	domain->msi_cookie = cookie;	/* [한국어] 도메인에 매단다 */
	return 0;	/* [한국어] 이제 이 도메인에서도 MSI 도어벨이 매핑된다 */
}
EXPORT_SYMBOL(iommu_get_msi_cookie);	/* [한국어] VFIO 등 도메인을 직접 소유하는 사용자가 부른다 */

/**
 * iommu_put_dma_cookie - Release a domain's DMA mapping resources
 * @domain: IOMMU domain previously prepared by iommu_get_dma_cookie()
 */
void iommu_put_dma_cookie(struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] 해제할 쿠키 */
	struct iommu_dma_msi_page *msi, *tmp;	/* [한국어] MSI 목록 순회 (해제하며 돌므로 _safe) */

	if (cookie->iovad.granule) {	/* [한국어] granule 이 0 이 아니면 IOVA 공간이 실제로 세워졌다는 뜻 — 쿠키만 만들고 도메인 초기화 전에 해제되는 경로가 있다 */
		iommu_dma_free_fq(cookie);	/* [한국어] 큐부터 (타이머 정지 포함) */
		put_iova_domain(&cookie->iovad);	/* [한국어] IOVA 트리와 캐시 해제 */
	}
	list_for_each_entry_safe(msi, tmp, &cookie->msi_page_list, list)	/* [한국어] MSI 매핑 기록들을 */
		kfree(msi);	/* [한국어] 해제한다. 실제 IOVA 매핑은 도메인이 사라지며 함께 없어진다 */
	kfree(cookie);	/* [한국어] 쿠키 본체 */
}

/**
 * iommu_put_msi_cookie - Release a domain's MSI mapping resources
 * @domain: IOMMU domain previously prepared by iommu_get_msi_cookie()
 */
void iommu_put_msi_cookie(struct iommu_domain *domain)
{
	struct iommu_dma_msi_cookie *cookie = domain->msi_cookie;	/* [한국어] 축소판 쿠키 */
	struct iommu_dma_msi_page *msi, *tmp;	/* [한국어] MSI 목록 순회 */

	list_for_each_entry_safe(msi, tmp, &cookie->msi_page_list, list)	/* [한국어] 매핑 기록들을 */
		kfree(msi);	/* [한국어] 해제 */
	kfree(cookie);	/* [한국어] 쿠키 본체 */
}

/**
 * iommu_dma_get_resv_regions - Reserved region driver helper
 * @dev: Device from iommu_get_resv_regions()
 * @list: Reserved region list from iommu_get_resv_regions()
 *
 * IOMMU drivers can use this to implement their .get_resv_regions callback
 * for general non-IOMMU-specific reservations. Currently, this covers GICv3
 * ITS region reservation on ACPI based ARM platforms that may require HW MSI
 * reservation.
 */
void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)
{

	if (!is_of_node(dev_iommu_fwspec_get(dev)->iommu_fwnode))	/* [한국어] 장치 트리 노드가 아니면 = ACPI 시스템 */
		iort_iommu_get_resv_regions(dev, list);	/* [한국어] IORT 표에서 GICv3 ITS 창 같은 하드웨어 MSI 구간을 읽어 예약 목록에 넣는다 (위 영어 주석) */

	if (dev->of_node)	/* [한국어] 장치 트리 노드가 있으면 */
		of_iommu_get_resv_regions(dev, list);	/* [한국어] DT 에서 같은 정보를 읽는다 */
}
EXPORT_SYMBOL(iommu_dma_get_resv_regions);	/* [한국어] 벤더 드라이버가 자기 get_resv_regions 콜백에서 이 공통 부분을 그대로 부른다 */

static int cookie_init_hw_msi_region(struct iommu_dma_cookie *cookie,
		phys_addr_t start, phys_addr_t end)
{
	struct iova_domain *iovad = &cookie->iovad;
	struct iommu_dma_msi_page *msi_page;
	int i, num_pages;

	start -= iova_offset(iovad, start);
	num_pages = iova_align(iovad, end - start) >> iova_shift(iovad);

	for (i = 0; i < num_pages; i++) {
		msi_page = kmalloc_obj(*msi_page);
		if (!msi_page)
			return -ENOMEM;

		msi_page->phys = start;
		msi_page->iova = start;
		INIT_LIST_HEAD(&msi_page->list);
		list_add(&msi_page->list, &cookie->msi_page_list);
		start += iovad->granule;
	}

	return 0;
}

static int iommu_dma_ranges_sort(void *priv, const struct list_head *a,
		const struct list_head *b)
{
	struct resource_entry *res_a = list_entry(a, typeof(*res_a), node);
	struct resource_entry *res_b = list_entry(b, typeof(*res_b), node);

	return res_a->res->start > res_b->res->start;
}

static int iova_reserve_pci_windows(struct pci_dev *dev,
		struct iova_domain *iovad)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(dev->bus);
	struct resource_entry *window;
	unsigned long lo, hi;
	phys_addr_t start = 0, end;

	resource_list_for_each_entry(window, &bridge->windows) {
		if (resource_type(window->res) != IORESOURCE_MEM)
			continue;

		lo = iova_pfn(iovad, window->res->start - window->offset);
		hi = iova_pfn(iovad, window->res->end - window->offset);
		reserve_iova(iovad, lo, hi);
	}

	/* Get reserved DMA windows from host bridge */
	list_sort(NULL, &bridge->dma_ranges, iommu_dma_ranges_sort);
	resource_list_for_each_entry(window, &bridge->dma_ranges) {
		end = window->res->start - window->offset;
resv_iova:
		if (end > start) {
			lo = iova_pfn(iovad, start);
			hi = iova_pfn(iovad, end);
			reserve_iova(iovad, lo, hi);
		} else if (end < start) {
			/* DMA ranges should be non-overlapping */
			dev_err(&dev->dev,
				"Failed to reserve IOVA [%pa-%pa]\n",
				&start, &end);
			return -EINVAL;
		}

		start = window->res->end - window->offset + 1;
		/* If window is last entry */
		if (window->node.next == &bridge->dma_ranges &&
		    end != ~(phys_addr_t)0) {
			end = ~(phys_addr_t)0;
			goto resv_iova;
		}
	}

	return 0;
}

static int iova_reserve_iommu_regions(struct device *dev,
		struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	struct iommu_resv_region *region;
	LIST_HEAD(resv_regions);
	int ret = 0;

	if (dev_is_pci(dev)) {
		ret = iova_reserve_pci_windows(to_pci_dev(dev), iovad);
		if (ret)
			return ret;
	}

	iommu_get_resv_regions(dev, &resv_regions);
	list_for_each_entry(region, &resv_regions, list) {
		unsigned long lo, hi;

		/* We ARE the software that manages these! */
		if (region->type == IOMMU_RESV_SW_MSI)
			continue;

		lo = iova_pfn(iovad, region->start);
		hi = iova_pfn(iovad, region->start + region->length - 1);
		reserve_iova(iovad, lo, hi);

		if (region->type == IOMMU_RESV_MSI)
			ret = cookie_init_hw_msi_region(cookie, region->start,
					region->start + region->length);
		if (ret)
			break;
	}
	iommu_put_resv_regions(dev, &resv_regions);

	return ret;
}

static bool dev_is_untrusted(struct device *dev)
{
	return dev_is_pci(dev) && to_pci_dev(dev)->untrusted;
}

static bool dev_use_swiotlb(struct device *dev, size_t size,
			    enum dma_data_direction dir)
{
	return IS_ENABLED(CONFIG_SWIOTLB) &&
		(dev_is_untrusted(dev) ||
		 dma_kmalloc_needs_bounce(dev, size, dir));
}

static bool dev_use_sg_swiotlb(struct device *dev, struct scatterlist *sg,
			       int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;

	if (!IS_ENABLED(CONFIG_SWIOTLB))
		return false;

	if (dev_is_untrusted(dev))
		return true;

	/*
	 * If kmalloc() buffers are not DMA-safe for this device and
	 * direction, check the individual lengths in the sg list. If any
	 * element is deemed unsafe, use the swiotlb for bouncing.
	 */
	if (!dma_kmalloc_safe(dev, dir)) {
		for_each_sg(sg, s, nents, i)
			if (!dma_kmalloc_size_aligned(s->length))
				return true;
	}

	return false;
}

/**
 * iommu_dma_init_options - Initialize dma-iommu options
 * @options: The options to be initialized
 * @dev: Device the options are set for
 *
 * This allows tuning dma-iommu specific to device properties
 */
static void iommu_dma_init_options(struct iommu_dma_options *options,
				   struct device *dev)
{
	/* Shadowing IOTLB flushes do better with a single large queue */
	if (dev->iommu->shadow_on_flush) {
		options->qt = IOMMU_DMA_OPTS_SINGLE_QUEUE;
		options->fq_timeout = IOVA_SINGLE_FQ_TIMEOUT;
		options->fq_size = IOVA_SINGLE_FQ_SIZE;
	} else {
		options->qt = IOMMU_DMA_OPTS_PER_CPU_QUEUE;
		options->fq_size = IOVA_DEFAULT_FQ_SIZE;
		options->fq_timeout = IOVA_DEFAULT_FQ_TIMEOUT;
	}
}

static bool iommu_domain_supports_fq(struct device *dev,
				     struct iommu_domain *domain)
{
	/* iommupt always supports DMA-FQ */
	if (iommupt_from_domain(domain))
		return true;
	return device_iommu_capable(dev, IOMMU_CAP_DEFERRED_FLUSH);
}

/**
 * iommu_dma_init_domain - Initialise a DMA mapping domain
 * @domain: IOMMU domain previously prepared by iommu_get_dma_cookie()
 * @dev: Device the domain is being initialised for
 *
 * If the geometry and dma_range_map include address 0, we reserve that page
 * to ensure it is an invalid IOVA. It is safe to reinitialise a domain, but
 * any change which could make prior IOVAs invalid will fail.
 */
static int iommu_dma_init_domain(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	const struct bus_dma_region *map = dev->dma_range_map;
	unsigned long order, base_pfn;
	struct iova_domain *iovad;
	int ret;

	if (!cookie || domain->cookie_type != IOMMU_COOKIE_DMA_IOVA)
		return -EINVAL;

	iovad = &cookie->iovad;

	/* Use the smallest supported page size for IOVA granularity */
	order = __ffs(domain->pgsize_bitmap);
	base_pfn = 1;

	/* Check the domain allows at least some access to the device... */
	if (map) {
		if (dma_range_map_min(map) > domain->geometry.aperture_end ||
		    dma_range_map_max(map) < domain->geometry.aperture_start) {
			pr_warn("specified DMA range outside IOMMU capability\n");
			return -EFAULT;
		}
	}
	/* ...then finally give it a kicking to make sure it fits */
	base_pfn = max_t(unsigned long, base_pfn,
			 domain->geometry.aperture_start >> order);

	/* start_pfn is always nonzero for an already-initialised domain */
	if (iovad->start_pfn) {
		if (1UL << order != iovad->granule ||
		    base_pfn != iovad->start_pfn) {
			pr_warn("Incompatible range for DMA domain\n");
			return -EFAULT;
		}

		return 0;
	}

	init_iova_domain(iovad, 1UL << order, base_pfn);
	ret = iova_domain_init_rcaches(iovad);
	if (ret)
		return ret;

	iommu_dma_init_options(&cookie->options, dev);

	/* If the FQ fails we can simply fall back to strict mode */
	if (domain->type == IOMMU_DOMAIN_DMA_FQ &&
	    (!iommu_domain_supports_fq(dev, domain) ||
	     iommu_dma_init_fq(domain)))
		domain->type = IOMMU_DOMAIN_DMA;

	return iova_reserve_iommu_regions(dev, domain);
}

/**
 * dma_info_to_prot - Translate DMA API directions and attributes to IOMMU API
 *                    page flags.
 * @dir: Direction of DMA transfer
 * @coherent: Is the DMA master cache-coherent?
 * @attrs: DMA attributes for the mapping
 *
 * Return: corresponding IOMMU API page protection flags
 */
static int dma_info_to_prot(enum dma_data_direction dir, bool coherent,
		     unsigned long attrs)
{
	int prot;

	if (attrs & DMA_ATTR_MMIO)
		prot = IOMMU_MMIO;
	else
		prot = coherent ? IOMMU_CACHE : 0;

	if (attrs & DMA_ATTR_PRIVILEGED)
		prot |= IOMMU_PRIV;

	switch (dir) {
	case DMA_BIDIRECTIONAL:
		return prot | IOMMU_READ | IOMMU_WRITE;
	case DMA_TO_DEVICE:
		return prot | IOMMU_READ;
	case DMA_FROM_DEVICE:
		return prot | IOMMU_WRITE;
	default:
		return 0;
	}
}

static dma_addr_t iommu_dma_alloc_iova(struct iommu_domain *domain,
		size_t size, u64 dma_limit, struct device *dev)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	unsigned long shift, iova_len, iova;

	if (domain->cookie_type == IOMMU_COOKIE_DMA_MSI) {
		domain->msi_cookie->msi_iova += size;
		return domain->msi_cookie->msi_iova - size;
	}

	shift = iova_shift(iovad);
	iova_len = size >> shift;

	dma_limit = min_not_zero(dma_limit, dev->bus_dma_limit);

	if (domain->geometry.force_aperture)
		dma_limit = min(dma_limit, (u64)domain->geometry.aperture_end);

	/*
	 * Try to use all the 32-bit PCI addresses first. The original SAC vs.
	 * DAC reasoning loses relevance with PCIe, but enough hardware and
	 * firmware bugs are still lurking out there that it's safest not to
	 * venture into the 64-bit space until necessary.
	 *
	 * If your device goes wrong after seeing the notice then likely either
	 * its driver is not setting DMA masks accurately, the hardware has
	 * some inherent bug in handling >32-bit addresses, or not all the
	 * expected address bits are wired up between the device and the IOMMU.
	 */
	if (dma_limit > DMA_BIT_MASK(32) && dev->iommu->pci_32bit_workaround) {
		iova = alloc_iova_fast(iovad, iova_len,
				       DMA_BIT_MASK(32) >> shift, false);
		if (iova)
			goto done;

		dev->iommu->pci_32bit_workaround = false;
		dev_notice(dev, "Using %d-bit DMA addresses\n", bits_per(dma_limit));
	}

	iova = alloc_iova_fast(iovad, iova_len, dma_limit >> shift, true);
done:
	return (dma_addr_t)iova << shift;
}

static void iommu_dma_free_iova(struct iommu_domain *domain, dma_addr_t iova,
				size_t size, struct iommu_iotlb_gather *gather)
{
	struct iova_domain *iovad = &domain->iova_cookie->iovad;

	/* The MSI case is only ever cleaning up its most recent allocation */
	if (domain->cookie_type == IOMMU_COOKIE_DMA_MSI)
		domain->msi_cookie->msi_iova -= size;
	else if (gather && gather->queued)
		queue_iova(domain->iova_cookie, iova_pfn(iovad, iova),
				size >> iova_shift(iovad),
				&gather->freelist);
	else
		free_iova_fast(iovad, iova_pfn(iovad, iova),
				size >> iova_shift(iovad));
}

static void __iommu_dma_unmap(struct device *dev, dma_addr_t dma_addr,
		size_t size)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t iova_off = iova_offset(iovad, dma_addr);
	struct iommu_iotlb_gather iotlb_gather;
	size_t unmapped;

	dma_addr -= iova_off;
	size = iova_align(iovad, size + iova_off);
	iommu_iotlb_gather_init(&iotlb_gather);
	iotlb_gather.queued = READ_ONCE(cookie->fq_domain);

	unmapped = iommu_unmap_fast(domain, dma_addr, size, &iotlb_gather);
	WARN_ON(unmapped != size);

	if (!iotlb_gather.queued)
		iommu_iotlb_sync(domain, &iotlb_gather);
	iommu_dma_free_iova(domain, dma_addr, size, &iotlb_gather);
}

static dma_addr_t __iommu_dma_map(struct device *dev, phys_addr_t phys,
		size_t size, int prot, u64 dma_mask)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t iova_off = iova_offset(iovad, phys);
	dma_addr_t iova;

	if (static_branch_unlikely(&iommu_deferred_attach_enabled) &&
	    iommu_deferred_attach(dev, domain))
		return DMA_MAPPING_ERROR;

	/* If anyone ever wants this we'd need support in the IOVA allocator */
	if (dev_WARN_ONCE(dev, dma_get_min_align_mask(dev) > iova_mask(iovad),
	    "Unsupported alignment constraint\n"))
		return DMA_MAPPING_ERROR;

	size = iova_align(iovad, size + iova_off);

	iova = iommu_dma_alloc_iova(domain, size, dma_mask, dev);
	if (!iova)
		return DMA_MAPPING_ERROR;

	if (iommu_map(domain, iova, phys - iova_off, size, prot, GFP_ATOMIC)) {
		iommu_dma_free_iova(domain, iova, size, NULL);
		return DMA_MAPPING_ERROR;
	}
	return iova + iova_off;
}

static void __iommu_dma_free_pages(struct page **pages, int count)
{
	while (count--)
		__free_page(pages[count]);
	kvfree(pages);
}

static struct page **__iommu_dma_alloc_pages(struct device *dev,
		unsigned int count, unsigned long order_mask, gfp_t gfp)
{
	struct page **pages;
	unsigned int i = 0, nid = dev_to_node(dev);

	order_mask &= GENMASK(MAX_PAGE_ORDER, 0);
	if (!order_mask)
		return NULL;

	pages = kvzalloc_objs(*pages, count);
	if (!pages)
		return NULL;

	/* IOMMU can map any pages, so himem can also be used here */
	gfp |= __GFP_NOWARN | __GFP_HIGHMEM;

	while (count) {
		struct page *page = NULL;
		unsigned int order_size;

		/*
		 * Higher-order allocations are a convenience rather
		 * than a necessity, hence using __GFP_NORETRY until
		 * falling back to minimum-order allocations.
		 */
		for (order_mask &= GENMASK(__fls(count), 0);
		     order_mask; order_mask &= ~order_size) {
			unsigned int order = __fls(order_mask);
			gfp_t alloc_flags = gfp;

			order_size = 1U << order;
			if (order_mask > order_size)
				alloc_flags |= __GFP_NORETRY;
			page = alloc_pages_node(nid, alloc_flags, order);
			if (!page)
				continue;
			if (order)
				split_page(page, order);
			break;
		}
		if (!page) {
			__iommu_dma_free_pages(pages, i);
			return NULL;
		}
		count -= order_size;
		while (order_size--)
			pages[i++] = page++;
	}
	return pages;
}

/*
 * If size is less than PAGE_SIZE, then a full CPU page will be allocated,
 * but an IOMMU which supports smaller pages might not map the whole thing.
 */
static struct page **__iommu_dma_alloc_noncontiguous(struct device *dev,
		size_t size, struct sg_table *sgt, gfp_t gfp, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	bool coherent = dev_is_dma_coherent(dev);
	int ioprot = dma_info_to_prot(DMA_BIDIRECTIONAL, coherent, attrs);
	unsigned int count, min_size, alloc_sizes = domain->pgsize_bitmap;
	struct page **pages;
	dma_addr_t iova;
	ssize_t ret;

	if (static_branch_unlikely(&iommu_deferred_attach_enabled) &&
	    iommu_deferred_attach(dev, domain))
		return NULL;

	min_size = alloc_sizes & -alloc_sizes;
	if (min_size < PAGE_SIZE) {
		min_size = PAGE_SIZE;
		alloc_sizes |= PAGE_SIZE;
	} else {
		size = ALIGN(size, min_size);
	}
	if (attrs & DMA_ATTR_ALLOC_SINGLE_PAGES)
		alloc_sizes = min_size;

	count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	pages = __iommu_dma_alloc_pages(dev, count, alloc_sizes >> PAGE_SHIFT,
					gfp);
	if (!pages)
		return NULL;

	size = iova_align(iovad, size);
	iova = iommu_dma_alloc_iova(domain, size, dev->coherent_dma_mask, dev);
	if (!iova)
		goto out_free_pages;

	/*
	 * Remove the zone/policy flags from the GFP - these are applied to the
	 * __iommu_dma_alloc_pages() but are not used for the supporting
	 * internal allocations that follow.
	 */
	gfp &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM | __GFP_COMP);

	if (sg_alloc_table_from_pages(sgt, pages, count, 0, size, gfp))
		goto out_free_iova;

	if (!(ioprot & IOMMU_CACHE)) {
		struct scatterlist *sg;
		int i;

		for_each_sg(sgt->sgl, sg, sgt->orig_nents, i)
			arch_dma_prep_coherent(sg_page(sg), sg->length);
	}

	ret = iommu_map_sg(domain, iova, sgt->sgl, sgt->orig_nents, ioprot,
			   gfp);
	if (ret < 0 || ret < size)
		goto out_free_sg;

	sgt->sgl->dma_address = iova;
	sgt->sgl->dma_length = size;
	return pages;

out_free_sg:
	sg_free_table(sgt);
out_free_iova:
	iommu_dma_free_iova(domain, iova, size, NULL);
out_free_pages:
	__iommu_dma_free_pages(pages, count);
	return NULL;
}

static void *iommu_dma_alloc_remap(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
	struct page **pages;
	struct sg_table sgt;
	void *vaddr;
	pgprot_t prot = dma_pgprot(dev, PAGE_KERNEL, attrs);

	pages = __iommu_dma_alloc_noncontiguous(dev, size, &sgt, gfp, attrs);
	if (!pages)
		return NULL;
	*dma_handle = sgt.sgl->dma_address;
	sg_free_table(&sgt);
	vaddr = dma_common_pages_remap(pages, size, prot,
			__builtin_return_address(0));
	if (!vaddr)
		goto out_unmap;
	return vaddr;

out_unmap:
	__iommu_dma_unmap(dev, *dma_handle, size);
	__iommu_dma_free_pages(pages, PAGE_ALIGN(size) >> PAGE_SHIFT);
	return NULL;
}

/*
 * This is the actual return value from the iommu_dma_alloc_noncontiguous.
 *
 * The users of the DMA API should only care about the sg_table, but to make
 * the DMA-API internal vmaping and freeing easier we stash away the page
 * array as well (except for the fallback case).  This can go away any time,
 * e.g. when a vmap-variant that takes a scatterlist comes along.
 */
struct dma_sgt_handle {
	struct sg_table sgt;
	struct page **pages;
};
#define sgt_handle(sgt) \
	container_of((sgt), struct dma_sgt_handle, sgt)

struct sg_table *iommu_dma_alloc_noncontiguous(struct device *dev, size_t size,
	       enum dma_data_direction dir, gfp_t gfp, unsigned long attrs)
{
	struct dma_sgt_handle *sh;

	sh = kmalloc_obj(*sh, gfp);
	if (!sh)
		return NULL;

	sh->pages = __iommu_dma_alloc_noncontiguous(dev, size, &sh->sgt, gfp, attrs);
	if (!sh->pages) {
		kfree(sh);
		return NULL;
	}
	return &sh->sgt;
}

void iommu_dma_free_noncontiguous(struct device *dev, size_t size,
		struct sg_table *sgt, enum dma_data_direction dir)
{
	struct dma_sgt_handle *sh = sgt_handle(sgt);

	__iommu_dma_unmap(dev, sgt->sgl->dma_address, size);
	__iommu_dma_free_pages(sh->pages, PAGE_ALIGN(size) >> PAGE_SHIFT);
	sg_free_table(&sh->sgt);
	kfree(sh);
}

void *iommu_dma_vmap_noncontiguous(struct device *dev, size_t size,
		struct sg_table *sgt)
{
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;

	return vmap(sgt_handle(sgt)->pages, count, VM_MAP, PAGE_KERNEL);
}

int iommu_dma_mmap_noncontiguous(struct device *dev, struct vm_area_struct *vma,
		size_t size, struct sg_table *sgt)
{
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;

	if (vma->vm_pgoff >= count || vma_pages(vma) > count - vma->vm_pgoff)
		return -ENXIO;
	return vm_map_pages(vma, sgt_handle(sgt)->pages, count);
}

void iommu_dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir)
{
	phys_addr_t phys;

	if (dev_is_dma_coherent(dev) && !dev_use_swiotlb(dev, size, dir))
		return;

	phys = iommu_iova_to_phys(iommu_get_dma_domain(dev), dma_handle);
	if (!dev_is_dma_coherent(dev)) {
		arch_sync_dma_for_cpu(phys, size, dir);
		arch_sync_dma_flush();
	}

	swiotlb_sync_single_for_cpu(dev, phys, size, dir);
}

void iommu_dma_sync_single_for_device(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir)
{
	phys_addr_t phys;

	if (dev_is_dma_coherent(dev) && !dev_use_swiotlb(dev, size, dir))
		return;

	phys = iommu_iova_to_phys(iommu_get_dma_domain(dev), dma_handle);
	swiotlb_sync_single_for_device(dev, phys, size, dir);

	if (!dev_is_dma_coherent(dev)) {
		arch_sync_dma_for_device(phys, size, dir);
		arch_sync_dma_flush();
	}
}

void iommu_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	if (sg_dma_is_swiotlb(sgl)) {
		for_each_sg(sgl, sg, nelems, i)
			iommu_dma_sync_single_for_cpu(dev, sg_dma_address(sg),
						      sg->length, dir);
	} else if (!dev_is_dma_coherent(dev)) {
		for_each_sg(sgl, sg, nelems, i)
			arch_sync_dma_for_cpu(sg_phys(sg), sg->length, dir);
		arch_sync_dma_flush();
	}
}

void iommu_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	if (sg_dma_is_swiotlb(sgl)) {
		for_each_sg(sgl, sg, nelems, i)
			iommu_dma_sync_single_for_device(dev,
							 sg_dma_address(sg),
							 sg->length, dir);
	} else if (!dev_is_dma_coherent(dev)) {
		for_each_sg(sgl, sg, nelems, i)
			arch_sync_dma_for_device(sg_phys(sg), sg->length, dir);
		arch_sync_dma_flush();
	}
}

static phys_addr_t iommu_dma_map_swiotlb(struct device *dev, phys_addr_t phys,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iova_domain *iovad = &domain->iova_cookie->iovad;

	if (!is_swiotlb_active(dev)) {
		dev_warn_once(dev, "DMA bounce buffers are inactive, unable to map unaligned transaction.\n");
		return (phys_addr_t)DMA_MAPPING_ERROR;
	}

	trace_swiotlb_bounced(dev, phys, size);

	phys = swiotlb_tbl_map_single(dev, phys, size, iova_mask(iovad), dir,
			attrs);

	/*
	 * Untrusted devices should not see padding areas with random leftover
	 * kernel data, so zero the pre- and post-padding.
	 * swiotlb_tbl_map_single() has initialized the bounce buffer proper to
	 * the contents of the original memory buffer.
	 */
	if (phys != (phys_addr_t)DMA_MAPPING_ERROR && dev_is_untrusted(dev)) {
		size_t start, virt = (size_t)phys_to_virt(phys);

		/* Pre-padding */
		start = iova_align_down(iovad, virt);
		memset((void *)start, 0, virt - start);

		/* Post-padding */
		start = virt + size;
		memset((void *)start, 0, iova_align(iovad, start) - start);
	}

	return phys;
}

/*
 * Checks if a physical buffer has unaligned boundaries with respect to
 * the IOMMU granule. Returns non-zero if either the start or end
 * address is not aligned to the granule boundary.
 */
static inline size_t iova_unaligned(struct iova_domain *iovad, phys_addr_t phys,
				    size_t size)
{
	return iova_offset(iovad, phys | size);
}

dma_addr_t iommu_dma_map_phys(struct device *dev, phys_addr_t phys, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);
	int prot = dma_info_to_prot(dir, coherent, attrs);
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	dma_addr_t iova, dma_mask = dma_get_mask(dev);

	/*
	 * If both the physical buffer start address and size are page aligned,
	 * we don't need to use a bounce page.
	 */
	if (dev_use_swiotlb(dev, size, dir) &&
	    iova_unaligned(iovad, phys, size)) {
		if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT))
			return DMA_MAPPING_ERROR;

		phys = iommu_dma_map_swiotlb(dev, phys, size, dir, attrs);
		if (phys == (phys_addr_t)DMA_MAPPING_ERROR)
			return DMA_MAPPING_ERROR;
	}

	if (!coherent && !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO))) {
		arch_sync_dma_for_device(phys, size, dir);
		arch_sync_dma_flush();
	}

	iova = __iommu_dma_map(dev, phys, size, prot, dma_mask);
	if (iova == DMA_MAPPING_ERROR &&
	    !(attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT)))
		swiotlb_tbl_unmap_single(dev, phys, size, dir, attrs);
	return iova;
}

void iommu_dma_unmap_phys(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t phys;

	if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT)) {
		__iommu_dma_unmap(dev, dma_handle, size);
		return;
	}

	phys = iommu_iova_to_phys(iommu_get_dma_domain(dev), dma_handle);
	if (WARN_ON(!phys))
		return;

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) && !dev_is_dma_coherent(dev)) {
		arch_sync_dma_for_cpu(phys, size, dir);
		arch_sync_dma_flush();
	}

	__iommu_dma_unmap(dev, dma_handle, size);

	swiotlb_tbl_unmap_single(dev, phys, size, dir, attrs);
}

/*
 * Prepare a successfully-mapped scatterlist to give back to the caller.
 *
 * At this point the segments are already laid out by iommu_dma_map_sg() to
 * avoid individually crossing any boundaries, so we merely need to check a
 * segment's start address to avoid concatenating across one.
 */
static int __finalise_sg(struct device *dev, struct scatterlist *sg, int nents,
		dma_addr_t dma_addr)
{
	struct scatterlist *s, *cur = sg;
	unsigned long seg_mask = dma_get_seg_boundary(dev);
	unsigned int cur_len = 0, max_len = dma_get_max_seg_size(dev);
	int i, count = 0;

	for_each_sg(sg, s, nents, i) {
		/* Restore this segment's original unaligned fields first */
		dma_addr_t s_dma_addr = sg_dma_address(s);
		unsigned int s_iova_off = sg_dma_address(s);
		unsigned int s_length = sg_dma_len(s);
		unsigned int s_iova_len = s->length;

		sg_dma_address(s) = DMA_MAPPING_ERROR;
		sg_dma_len(s) = 0;

		if (sg_dma_is_bus_address(s)) {
			if (i > 0)
				cur = sg_next(cur);

			sg_dma_unmark_bus_address(s);
			sg_dma_address(cur) = s_dma_addr;
			sg_dma_len(cur) = s_length;
			sg_dma_mark_bus_address(cur);
			count++;
			cur_len = 0;
			continue;
		}

		s->offset += s_iova_off;
		s->length = s_length;

		/*
		 * Now fill in the real DMA data. If...
		 * - there is a valid output segment to append to
		 * - and this segment starts on an IOVA page boundary
		 * - but doesn't fall at a segment boundary
		 * - and wouldn't make the resulting output segment too long
		 */
		if (cur_len && !s_iova_off && (dma_addr & seg_mask) &&
		    (max_len - cur_len >= s_length)) {
			/* ...then concatenate it with the previous one */
			cur_len += s_length;
		} else {
			/* Otherwise start the next output segment */
			if (i > 0)
				cur = sg_next(cur);
			cur_len = s_length;
			count++;

			sg_dma_address(cur) = dma_addr + s_iova_off;
		}

		sg_dma_len(cur) = cur_len;
		dma_addr += s_iova_len;

		if (s_length + s_iova_off < s_iova_len)
			cur_len = 0;
	}
	return count;
}

/*
 * If mapping failed, then just restore the original list,
 * but making sure the DMA fields are invalidated.
 */
static void __invalidate_sg(struct scatterlist *sg, int nents)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		if (sg_dma_is_bus_address(s)) {
			sg_dma_unmark_bus_address(s);
		} else {
			if (sg_dma_address(s) != DMA_MAPPING_ERROR)
				s->offset += sg_dma_address(s);
			if (sg_dma_len(s))
				s->length = sg_dma_len(s);
		}
		sg_dma_address(s) = DMA_MAPPING_ERROR;
		sg_dma_len(s) = 0;
	}
}

static void iommu_dma_unmap_sg_swiotlb(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		iommu_dma_unmap_phys(dev, sg_dma_address(s),
				sg_dma_len(s), dir, attrs);
}

static int iommu_dma_map_sg_swiotlb(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	sg_dma_mark_swiotlb(sg);

	for_each_sg(sg, s, nents, i) {
		sg_dma_address(s) = iommu_dma_map_phys(dev, sg_phys(s),
				s->length, dir, attrs);
		if (sg_dma_address(s) == DMA_MAPPING_ERROR)
			goto out_unmap;
		sg_dma_len(s) = s->length;
	}

	return nents;

out_unmap:
	iommu_dma_unmap_sg_swiotlb(dev, sg, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	return -EIO;
}

/*
 * The DMA API client is passing in a scatterlist which could describe
 * any old buffer layout, but the IOMMU API requires everything to be
 * aligned to IOMMU pages. Hence the need for this complicated bit of
 * impedance-matching, to be able to hand off a suitably-aligned list,
 * but still preserve the original offsets and sizes for the caller.
 */
int iommu_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	struct scatterlist *s, *prev = NULL;
	int prot = dma_info_to_prot(dir, dev_is_dma_coherent(dev), attrs);
	struct pci_p2pdma_map_state p2pdma_state = {};
	dma_addr_t iova;
	size_t iova_len = 0;
	unsigned long mask = dma_get_seg_boundary(dev);
	ssize_t ret;
	int i;

	if (static_branch_unlikely(&iommu_deferred_attach_enabled)) {
		ret = iommu_deferred_attach(dev, domain);
		if (ret)
			goto out;
	}

	if (dev_use_sg_swiotlb(dev, sg, nents, dir))
		return iommu_dma_map_sg_swiotlb(dev, sg, nents, dir, attrs);

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		iommu_dma_sync_sg_for_device(dev, sg, nents, dir);

	/*
	 * Work out how much IOVA space we need, and align the segments to
	 * IOVA granules for the IOMMU driver to handle. With some clever
	 * trickery we can modify the list in-place, but reversibly, by
	 * stashing the unaligned parts in the as-yet-unused DMA fields.
	 */
	for_each_sg(sg, s, nents, i) {
		size_t s_iova_off = iova_offset(iovad, s->offset);
		size_t s_length = s->length;
		size_t pad_len = (mask - iova_len + 1) & mask;

		switch (pci_p2pdma_state(&p2pdma_state, dev, sg_page(s))) {
		case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
			/*
			 * Mapping through host bridge should be mapped with
			 * regular IOVAs, thus we do nothing here and continue
			 * below.
			 */
			break;
		case PCI_P2PDMA_MAP_NONE:
			break;
		case PCI_P2PDMA_MAP_BUS_ADDR:
			/*
			 * iommu_map_sg() will skip this segment as it is marked
			 * as a bus address, __finalise_sg() will copy the dma
			 * address into the output segment.
			 */
			s->dma_address = pci_p2pdma_bus_addr_map(
				p2pdma_state.mem, sg_phys(s));
			sg_dma_len(s) = sg->length;
			sg_dma_mark_bus_address(s);
			continue;
		default:
			ret = -EREMOTEIO;
			goto out_restore_sg;
		}

		sg_dma_address(s) = s_iova_off;
		sg_dma_len(s) = s_length;
		s->offset -= s_iova_off;
		s_length = iova_align(iovad, s_length + s_iova_off);
		s->length = s_length;

		/*
		 * Due to the alignment of our single IOVA allocation, we can
		 * depend on these assumptions about the segment boundary mask:
		 * - If mask size >= IOVA size, then the IOVA range cannot
		 *   possibly fall across a boundary, so we don't care.
		 * - If mask size < IOVA size, then the IOVA range must start
		 *   exactly on a boundary, therefore we can lay things out
		 *   based purely on segment lengths without needing to know
		 *   the actual addresses beforehand.
		 * - The mask must be a power of 2, so pad_len == 0 if
		 *   iova_len == 0, thus we cannot dereference prev the first
		 *   time through here (i.e. before it has a meaningful value).
		 */
		if (pad_len && pad_len < s_length - 1) {
			prev->length += pad_len;
			iova_len += pad_len;
		}

		iova_len += s_length;
		prev = s;
	}

	if (!iova_len)
		return __finalise_sg(dev, sg, nents, 0);

	iova = iommu_dma_alloc_iova(domain, iova_len, dma_get_mask(dev), dev);
	if (!iova) {
		ret = -ENOMEM;
		goto out_restore_sg;
	}

	/*
	 * We'll leave any physical concatenation to the IOMMU driver's
	 * implementation - it knows better than we do.
	 */
	ret = iommu_map_sg(domain, iova, sg, nents, prot, GFP_ATOMIC);
	if (ret < 0 || ret < iova_len)
		goto out_free_iova;

	return __finalise_sg(dev, sg, nents, iova);

out_free_iova:
	iommu_dma_free_iova(domain, iova, iova_len, NULL);
out_restore_sg:
	__invalidate_sg(sg, nents);
out:
	if (ret != -ENOMEM && ret != -EREMOTEIO)
		return -EINVAL;
	return ret;
}

void iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, unsigned long attrs)
{
	dma_addr_t end = 0, start;
	struct scatterlist *tmp;
	int i;

	if (sg_dma_is_swiotlb(sg)) {
		iommu_dma_unmap_sg_swiotlb(dev, sg, nents, dir, attrs);
		return;
	}

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		iommu_dma_sync_sg_for_cpu(dev, sg, nents, dir);

	/*
	 * The scatterlist segments are mapped into a single
	 * contiguous IOVA allocation, the start and end points
	 * just have to be determined.
	 */
	for_each_sg(sg, tmp, nents, i) {
		if (sg_dma_is_bus_address(tmp)) {
			sg_dma_unmark_bus_address(tmp);
			continue;
		}

		if (sg_dma_len(tmp) == 0)
			break;

		start = sg_dma_address(tmp);
		break;
	}

	nents -= i;
	for_each_sg(tmp, tmp, nents, i) {
		if (sg_dma_is_bus_address(tmp)) {
			sg_dma_unmark_bus_address(tmp);
			continue;
		}

		if (sg_dma_len(tmp) == 0)
			break;

		end = sg_dma_address(tmp) + sg_dma_len(tmp);
	}

	if (end)
		__iommu_dma_unmap(dev, start, end - start);
}

static void __iommu_dma_free(struct device *dev, size_t size, void *cpu_addr)
{
	size_t alloc_size = PAGE_ALIGN(size);
	int count = alloc_size >> PAGE_SHIFT;
	struct page *page = NULL, **pages = NULL;

	/* Non-coherent atomic allocation? Easy */
	if (IS_ENABLED(CONFIG_DMA_DIRECT_REMAP) &&
	    dma_free_from_pool(dev, cpu_addr, alloc_size))
		return;

	if (is_vmalloc_addr(cpu_addr)) {
		/*
		 * If it the address is remapped, then it's either non-coherent
		 * or highmem CMA, or an iommu_dma_alloc_remap() construction.
		 */
		pages = dma_common_find_pages(cpu_addr);
		if (!pages)
			page = vmalloc_to_page(cpu_addr);
		dma_common_free_remap(cpu_addr, alloc_size);
	} else {
		/* Lowmem means a coherent atomic or CMA allocation */
		page = virt_to_page(cpu_addr);
	}

	if (pages)
		__iommu_dma_free_pages(pages, count);
	if (page)
		dma_free_contiguous(dev, page, alloc_size);
}

void iommu_dma_free(struct device *dev, size_t size, void *cpu_addr,
		dma_addr_t handle, unsigned long attrs)
{
	__iommu_dma_unmap(dev, handle, size);
	__iommu_dma_free(dev, size, cpu_addr);
}

static void *iommu_dma_alloc_pages(struct device *dev, size_t size,
		struct page **pagep, gfp_t gfp, unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);
	size_t alloc_size = PAGE_ALIGN(size);
	int node = dev_to_node(dev);
	struct page *page = NULL;
	void *cpu_addr;

	page = dma_alloc_contiguous(dev, alloc_size, gfp);
	if (!page)
		page = alloc_pages_node(node, gfp, get_order(alloc_size));
	if (!page)
		return NULL;

	if (!coherent || PageHighMem(page)) {
		pgprot_t prot = dma_pgprot(dev, PAGE_KERNEL, attrs);

		cpu_addr = dma_common_contiguous_remap(page, alloc_size,
				prot, __builtin_return_address(0));
		if (!cpu_addr)
			goto out_free_pages;

		if (!coherent)
			arch_dma_prep_coherent(page, size);
	} else {
		cpu_addr = page_address(page);
	}

	*pagep = page;
	memset(cpu_addr, 0, alloc_size);
	return cpu_addr;
out_free_pages:
	dma_free_contiguous(dev, page, alloc_size);
	return NULL;
}

void *iommu_dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
		gfp_t gfp, unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);
	int ioprot = dma_info_to_prot(DMA_BIDIRECTIONAL, coherent, attrs);
	struct page *page = NULL;
	void *cpu_addr;

	gfp |= __GFP_ZERO;

	if (gfpflags_allow_blocking(gfp) &&
	    !(attrs & DMA_ATTR_FORCE_CONTIGUOUS)) {
		return iommu_dma_alloc_remap(dev, size, handle, gfp, attrs);
	}

	if (IS_ENABLED(CONFIG_DMA_DIRECT_REMAP) &&
	    !gfpflags_allow_blocking(gfp) && !coherent)
		page = dma_alloc_from_pool(dev, PAGE_ALIGN(size), &cpu_addr,
					       gfp, NULL);
	else
		cpu_addr = iommu_dma_alloc_pages(dev, size, &page, gfp, attrs);
	if (!cpu_addr)
		return NULL;

	*handle = __iommu_dma_map(dev, page_to_phys(page), size, ioprot,
			dev->coherent_dma_mask);
	if (*handle == DMA_MAPPING_ERROR) {
		__iommu_dma_free(dev, size, cpu_addr);
		return NULL;
	}

	return cpu_addr;
}

int iommu_dma_mmap(struct device *dev, struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	unsigned long nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long pfn, off = vma->vm_pgoff;
	int ret;

	vma->vm_page_prot = dma_pgprot(dev, vma->vm_page_prot, attrs);

	if (dma_mmap_from_dev_coherent(dev, vma, cpu_addr, size, &ret))
		return ret;

	if (off >= nr_pages || vma_pages(vma) > nr_pages - off)
		return -ENXIO;

	if (is_vmalloc_addr(cpu_addr)) {
		struct page **pages = dma_common_find_pages(cpu_addr);

		if (pages)
			return vm_map_pages(vma, pages, nr_pages);
		pfn = vmalloc_to_pfn(cpu_addr);
	} else {
		pfn = page_to_pfn(virt_to_page(cpu_addr));
	}

	return remap_pfn_range(vma, vma->vm_start, pfn + off,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
}

int iommu_dma_get_sgtable(struct device *dev, struct sg_table *sgt,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	struct page *page;
	int ret;

	if (is_vmalloc_addr(cpu_addr)) {
		struct page **pages = dma_common_find_pages(cpu_addr);

		if (pages) {
			return sg_alloc_table_from_pages(sgt, pages,
					PAGE_ALIGN(size) >> PAGE_SHIFT,
					0, size, GFP_KERNEL);
		}

		page = vmalloc_to_page(cpu_addr);
	} else {
		page = virt_to_page(cpu_addr);
	}

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (!ret)
		sg_set_page(sgt->sgl, page, PAGE_ALIGN(size), 0);
	return ret;
}

unsigned long iommu_dma_get_merge_boundary(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);

	return (1UL << __ffs(domain->pgsize_bitmap)) - 1;
}

size_t iommu_dma_opt_mapping_size(void)
{
	return iova_rcache_range();
}

size_t iommu_dma_max_mapping_size(struct device *dev)
{
	if (dev_is_untrusted(dev))
		return swiotlb_max_mapping_size(dev);

	return SIZE_MAX;
}

/**
 * dma_iova_try_alloc - Try to allocate an IOVA space
 * @dev: Device to allocate the IOVA space for
 * @state: IOVA state
 * @phys: physical address
 * @size: IOVA size
 *
 * Check if @dev supports the IOVA-based DMA API, and if yes allocate IOVA space
 * for the given base address and size.
 *
 * Note: @phys is only used to calculate the IOVA alignment. Callers that always
 * do PAGE_SIZE aligned transfers can safely pass 0 here.
 *
 * Returns %true if the IOVA-based DMA API can be used and IOVA space has been
 * allocated, or %false if the regular DMA API should be used.
 */
bool dma_iova_try_alloc(struct device *dev, struct dma_iova_state *state,
		phys_addr_t phys, size_t size)
{
	struct iommu_dma_cookie *cookie;
	struct iommu_domain *domain;
	struct iova_domain *iovad;
	size_t iova_off;
	dma_addr_t addr;

	memset(state, 0, sizeof(*state));
	if (!use_dma_iommu(dev))
		return false;

	domain = iommu_get_dma_domain(dev);
	cookie = domain->iova_cookie;
	iovad = &cookie->iovad;
	iova_off = iova_offset(iovad, phys);

	if (static_branch_unlikely(&iommu_deferred_attach_enabled) &&
	    iommu_deferred_attach(dev, iommu_get_domain_for_dev(dev)))
		return false;

	if (WARN_ON_ONCE(!size))
		return false;

	/*
	 * DMA_IOVA_USE_SWIOTLB is flag which is set by dma-iommu
	 * internals, make sure that caller didn't set it and/or
	 * didn't use this interface to map SIZE_MAX.
	 */
	if (WARN_ON_ONCE((u64)size & DMA_IOVA_USE_SWIOTLB))
		return false;

	addr = iommu_dma_alloc_iova(domain,
			iova_align(iovad, size + iova_off),
			dma_get_mask(dev), dev);
	if (!addr)
		return false;

	state->addr = addr + iova_off;
	state->__size = size;
	return true;
}
EXPORT_SYMBOL_GPL(dma_iova_try_alloc);

/**
 * dma_iova_free - Free an IOVA space
 * @dev: Device to free the IOVA space for
 * @state: IOVA state
 *
 * Undoes a successful dma_try_iova_alloc().
 *
 * Note that all dma_iova_link() calls need to be undone first.  For callers
 * that never call dma_iova_unlink(), dma_iova_destroy() can be used instead
 * which unlinks all ranges and frees the IOVA space in a single efficient
 * operation.
 */
void dma_iova_free(struct device *dev, struct dma_iova_state *state)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t iova_start_pad = iova_offset(iovad, state->addr);
	size_t size = dma_iova_size(state);

	iommu_dma_free_iova(domain, state->addr - iova_start_pad,
			iova_align(iovad, size + iova_start_pad), NULL);
}
EXPORT_SYMBOL_GPL(dma_iova_free);

static int __dma_iova_link(struct device *dev, dma_addr_t addr,
		phys_addr_t phys, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);
	int prot = dma_info_to_prot(dir, coherent, attrs);

	if (!coherent && !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO)))
		arch_sync_dma_for_device(phys, size, dir);

	return iommu_map_nosync(iommu_get_dma_domain(dev), addr, phys, size,
			prot, GFP_ATOMIC);
}

static int iommu_dma_iova_bounce_and_link(struct device *dev, dma_addr_t addr,
		phys_addr_t phys, size_t bounce_len,
		enum dma_data_direction dir, unsigned long attrs,
		size_t iova_start_pad)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iova_domain *iovad = &domain->iova_cookie->iovad;
	phys_addr_t bounce_phys;
	int error;

	bounce_phys = iommu_dma_map_swiotlb(dev, phys, bounce_len, dir, attrs);
	if (bounce_phys == DMA_MAPPING_ERROR)
		return -ENOMEM;

	error = __dma_iova_link(dev, addr - iova_start_pad,
			bounce_phys - iova_start_pad,
			iova_align(iovad, bounce_len), dir, attrs);
	if (error)
		swiotlb_tbl_unmap_single(dev, bounce_phys, bounce_len, dir,
				attrs);
	return error;
}

static int iommu_dma_iova_link_swiotlb(struct device *dev,
		struct dma_iova_state *state, phys_addr_t phys, size_t offset,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t iova_start_pad = iova_offset(iovad, phys);
	size_t iova_end_pad = iova_offset(iovad, phys + size);
	dma_addr_t addr = state->addr + offset;
	size_t mapped = 0;
	int error;

	if (iova_start_pad) {
		size_t bounce_len = min(size, iovad->granule - iova_start_pad);

		error = iommu_dma_iova_bounce_and_link(dev, addr, phys,
				bounce_len, dir, attrs, iova_start_pad);
		if (error)
			return error;
		state->__size |= DMA_IOVA_USE_SWIOTLB;

		mapped += bounce_len;
		size -= bounce_len;
		if (!size)
			return 0;
	}

	size -= iova_end_pad;
	error = __dma_iova_link(dev, addr + mapped, phys + mapped, size, dir,
			attrs);
	if (error)
		goto out_unmap;
	mapped += size;

	if (iova_end_pad) {
		error = iommu_dma_iova_bounce_and_link(dev, addr + mapped,
				phys + mapped, iova_end_pad, dir, attrs, 0);
		if (error)
			goto out_unmap;
		state->__size |= DMA_IOVA_USE_SWIOTLB;
	}

	return 0;

out_unmap:
	dma_iova_unlink(dev, state, 0, mapped, dir, attrs);
	return error;
}

/**
 * dma_iova_link - Link a range of IOVA space
 * @dev: DMA device
 * @state: IOVA state
 * @phys: physical address to link
 * @offset: offset into the IOVA state to map into
 * @size: size of the buffer
 * @dir: DMA direction
 * @attrs: attributes of mapping properties
 *
 * Link a range of IOVA space for the given IOVA state without IOTLB sync.
 * This function is used to link multiple physical addresses in contiguous
 * IOVA space without performing costly IOTLB sync.
 *
 * The caller is responsible to call to dma_iova_sync() to sync IOTLB at
 * the end of linkage.
 */
int dma_iova_link(struct device *dev, struct dma_iova_state *state,
		phys_addr_t phys, size_t offset, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t iova_start_pad = iova_offset(iovad, phys);

	if (WARN_ON_ONCE(iova_start_pad && offset > 0))
		return -EIO;

	/*
	 * DMA_IOVA_USE_SWIOTLB is set on state after some entry
	 * took SWIOTLB path, which we were supposed to prevent
	 * for DMA_ATTR_REQUIRE_COHERENT attribute.
	 */
	if (WARN_ON_ONCE((state->__size & DMA_IOVA_USE_SWIOTLB) &&
			 (attrs & DMA_ATTR_REQUIRE_COHERENT)))
		return -EOPNOTSUPP;

	if (!dev_is_dma_coherent(dev) && (attrs & DMA_ATTR_REQUIRE_COHERENT))
		return -EOPNOTSUPP;

	if (dev_use_swiotlb(dev, size, dir) &&
	    iova_unaligned(iovad, phys, size)) {
		if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT))
			return -EPERM;

		return iommu_dma_iova_link_swiotlb(dev, state, phys, offset,
				size, dir, attrs);
	}

	return __dma_iova_link(dev, state->addr + offset - iova_start_pad,
			phys - iova_start_pad,
			iova_align(iovad, size + iova_start_pad), dir, attrs);
}
EXPORT_SYMBOL_GPL(dma_iova_link);

/**
 * dma_iova_sync - Sync IOTLB
 * @dev: DMA device
 * @state: IOVA state
 * @offset: offset into the IOVA state to sync
 * @size: size of the buffer
 *
 * Sync IOTLB for the given IOVA state. This function should be called on
 * the IOVA-contiguous range created by one ore more dma_iova_link() calls
 * to sync the IOTLB.
 */
int dma_iova_sync(struct device *dev, struct dma_iova_state *state,
		size_t offset, size_t size)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	dma_addr_t addr = state->addr + offset;
	size_t iova_start_pad = iova_offset(iovad, addr);

	if (!dev_is_dma_coherent(dev))
		arch_sync_dma_flush();
	return iommu_sync_map(domain, addr - iova_start_pad,
		      iova_align(iovad, size + iova_start_pad));
}
EXPORT_SYMBOL_GPL(dma_iova_sync);

static void iommu_dma_iova_unlink_range_slow(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t iova_start_pad = iova_offset(iovad, addr);
	bool need_sync_dma = !dev_is_dma_coherent(dev) &&
			!(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO));
	dma_addr_t end = addr + size;

	do {
		phys_addr_t phys;
		size_t len;

		phys = iommu_iova_to_phys(domain, addr);
		if (WARN_ON(!phys))
			/* Something very horrible happen here */
			return;

		len = min_t(size_t,
			end - addr, iovad->granule - iova_start_pad);

		if (!dev_is_dma_coherent(dev) &&
		    !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO)))
			arch_sync_dma_for_cpu(phys, len, dir);

		swiotlb_tbl_unmap_single(dev, phys, len, dir, attrs);

		addr += len;
		iova_start_pad = 0;
	} while (addr < end);

	if (need_sync_dma)
		arch_sync_dma_flush();
}

static void __iommu_dma_iova_unlink(struct device *dev,
		struct dma_iova_state *state, size_t offset, size_t size,
		enum dma_data_direction dir, unsigned long attrs,
		bool free_iova)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	dma_addr_t addr = state->addr + offset;
	size_t iova_start_pad = iova_offset(iovad, addr);
	struct iommu_iotlb_gather iotlb_gather;
	size_t unmapped;

	if ((state->__size & DMA_IOVA_USE_SWIOTLB) ||
	    (!dev_is_dma_coherent(dev) &&
	     !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO))))
		iommu_dma_iova_unlink_range_slow(dev, addr, size, dir, attrs);

	iommu_iotlb_gather_init(&iotlb_gather);
	iotlb_gather.queued = free_iova && READ_ONCE(cookie->fq_domain);

	size = iova_align(iovad, size + iova_start_pad);
	addr -= iova_start_pad;
	unmapped = iommu_unmap_fast(domain, addr, size, &iotlb_gather);
	WARN_ON(unmapped != size);

	if (!iotlb_gather.queued)
		iommu_iotlb_sync(domain, &iotlb_gather);
	if (free_iova)
		iommu_dma_free_iova(domain, addr, size, &iotlb_gather);
}

/**
 * dma_iova_unlink - Unlink a range of IOVA space
 * @dev: DMA device
 * @state: IOVA state
 * @offset: offset into the IOVA state to unlink
 * @size: size of the buffer
 * @dir: DMA direction
 * @attrs: attributes of mapping properties
 *
 * Unlink a range of IOVA space for the given IOVA state.
 */
void dma_iova_unlink(struct device *dev, struct dma_iova_state *state,
		size_t offset, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	 __iommu_dma_iova_unlink(dev, state, offset, size, dir, attrs, false);
}
EXPORT_SYMBOL_GPL(dma_iova_unlink);

/**
 * dma_iova_destroy - Finish a DMA mapping transaction
 * @dev: DMA device
 * @state: IOVA state
 * @mapped_len: number of bytes to unmap
 * @dir: DMA direction
 * @attrs: attributes of mapping properties
 *
 * Unlink the IOVA range up to @mapped_len and free the entire IOVA space. The
 * range of IOVA from dma_addr to @mapped_len must all be linked, and be the
 * only linked IOVA in state.
 */
void dma_iova_destroy(struct device *dev, struct dma_iova_state *state,
		size_t mapped_len, enum dma_data_direction dir,
		unsigned long attrs)
{
	if (mapped_len)
		__iommu_dma_iova_unlink(dev, state, 0, mapped_len, dir, attrs,
				true);
	else
		/*
		 * We can be here if first call to dma_iova_link() failed and
		 * there is nothing to unlink, so let's be more clear.
		 */
		dma_iova_free(dev, state);
}
EXPORT_SYMBOL_GPL(dma_iova_destroy);

void iommu_setup_dma_ops(struct device *dev, struct iommu_domain *domain)
{
	if (dev_is_pci(dev))
		dev->iommu->pci_32bit_workaround = !iommu_dma_forcedac;

	dev->dma_iommu = iommu_is_dma_domain(domain);
	if (dev->dma_iommu && iommu_dma_init_domain(domain, dev))
		goto out_err;

	return;
out_err:
	pr_warn("Failed to set up IOMMU for device %s; retaining platform DMA ops\n",
		dev_name(dev));
	dev->dma_iommu = false;
}

static bool has_msi_cookie(const struct iommu_domain *domain)
{
	return domain && (domain->cookie_type == IOMMU_COOKIE_DMA_IOVA ||
			  domain->cookie_type == IOMMU_COOKIE_DMA_MSI);
}

static size_t cookie_msi_granule(const struct iommu_domain *domain)
{
	switch (domain->cookie_type) {
	case IOMMU_COOKIE_DMA_IOVA:
		return domain->iova_cookie->iovad.granule;
	case IOMMU_COOKIE_DMA_MSI:
		return PAGE_SIZE;
	default:
		BUG();
	}
}

static struct list_head *cookie_msi_pages(const struct iommu_domain *domain)
{
	switch (domain->cookie_type) {
	case IOMMU_COOKIE_DMA_IOVA:
		return &domain->iova_cookie->msi_page_list;
	case IOMMU_COOKIE_DMA_MSI:
		return &domain->msi_cookie->msi_page_list;
	default:
		BUG();
	}
}

static struct iommu_dma_msi_page *iommu_dma_get_msi_page(struct device *dev,
		phys_addr_t msi_addr, struct iommu_domain *domain)
{
	struct list_head *msi_page_list = cookie_msi_pages(domain);
	struct iommu_dma_msi_page *msi_page;
	dma_addr_t iova;
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;
	size_t size = cookie_msi_granule(domain);

	msi_addr &= ~(phys_addr_t)(size - 1);
	list_for_each_entry(msi_page, msi_page_list, list)
		if (msi_page->phys == msi_addr)
			return msi_page;

	msi_page = kzalloc_obj(*msi_page);
	if (!msi_page)
		return NULL;

	iova = iommu_dma_alloc_iova(domain, size, dma_get_mask(dev), dev);
	if (!iova)
		goto out_free_page;

	if (iommu_map(domain, iova, msi_addr, size, prot, GFP_KERNEL))
		goto out_free_iova;

	INIT_LIST_HEAD(&msi_page->list);
	msi_page->phys = msi_addr;
	msi_page->iova = iova;
	list_add(&msi_page->list, msi_page_list);
	return msi_page;

out_free_iova:
	iommu_dma_free_iova(domain, iova, size, NULL);
out_free_page:
	kfree(msi_page);
	return NULL;
}

int iommu_dma_sw_msi(struct iommu_domain *domain, struct msi_desc *desc,
		     phys_addr_t msi_addr)
{
	struct device *dev = msi_desc_to_dev(desc);
	const struct iommu_dma_msi_page *msi_page;

	if (!has_msi_cookie(domain)) {
		msi_desc_set_iommu_msi_iova(desc, 0, 0);
		return 0;
	}

	iommu_group_mutex_assert(dev);
	msi_page = iommu_dma_get_msi_page(dev, msi_addr, domain);
	if (!msi_page)
		return -ENOMEM;

	msi_desc_set_iommu_msi_iova(desc, msi_page->iova,
				    ilog2(cookie_msi_granule(domain)));
	return 0;
}

static int iommu_dma_init(void)
{
	if (is_kdump_kernel())
		static_branch_enable(&iommu_deferred_attach_enabled);

	return iova_cache_get();
}
arch_initcall(iommu_dma_init);
