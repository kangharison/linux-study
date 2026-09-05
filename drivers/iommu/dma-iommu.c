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

/*
 * [한국어]
 * iommu_dma_forcedac_setup - "iommu.forcedac=" 부트 인자를 처리한다
 *
 * @str:    인자 값
 * @return: 0 성공, 음수면 값 해석 실패
 *
 * 기본적으로 IOVA 할당은 32비트 영역부터 시도한다. PCIe 에서는 SAC/DAC 구분이
 * 의미를 잃었지만, 상위 주소 비트를 제대로 배선하지 않았거나 DMA 마스크를 잘못
 * 신고하는 하드웨어가 남아 있어 보수적으로 낮은 주소를 먼저 쓴다.
 *
 * 이 인자를 켜면 그 배려를 끄고 처음부터 전체 주소 공간을 쓴다. 32비트 영역이
 * 좁아 병목이 되는 시스템(수백 개의 큐를 가진 고성능 장치 여럿)에서 의미가 있다.
 *
 * 실행 컨텍스트: early_param — 부팅 초기.
 *
 * 호출 체인: 부트 인자 파서 → [이 함수]
 */
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

/* [한국어] head 부터 tail 직전까지 링을 도는 매크로. mod_mask 로 감싸므로 배열 끝에서 앞으로 되돌아온다
 * (매크로 이어짐 표시 \ 뒤에는 주석을 붙일 수 없어 위로 옮겼다.) */
#define fq_ring_for_each(i, fq) \
	for ((i) = (fq)->head; (i) != (fq)->tail; (i) = ((i) + 1) & (fq)->mod_mask)	/* [한국어] head == tail 이면 비었다는 규약이라 종료 조건이 곧 '빔' 판정이기도 하다 */

static inline bool fq_full(struct iova_fq *fq)
{
	assert_spin_locked(&fq->lock);	/* [한국어] 호출자가 큐 락을 든 상태여야 한다 */
	return (((fq->tail + 1) & fq->mod_mask) == fq->head);	/* [한국어] 한 칸을 비워 둬 '가득 참'과 '빔'을 구별한다. 그 한 칸이 없으면 head==tail 이 두 뜻을 갖는다 */
}

/*
 * [한국어]
 * fq_ring_add - 링 버퍼에 자리 하나를 확보한다
 *
 * @fq:     대상 큐 (가득 차 있지 않아야 한다)
 * @return: 쓸 수 있는 항목의 인덱스
 *
 * tail 을 전진시키고 이전 값을 돌려주는 것이 전부다. 가득 참 검사는 호출자가
 * 이미 마쳤다는 전제이며, 그래서 여기에는 검사가 없다.
 *
 * 실행 컨텍스트: 큐 락을 든 채.
 *
 * 호출 체인: queue_iova → [이 함수]
 */
static inline unsigned int fq_ring_add(struct iova_fq *fq)
{
	unsigned int idx = fq->tail;	/* [한국어] 넣을 자리 */

	assert_spin_locked(&fq->lock);	/* [한국어] 호출자가 락을 든 상태여야 한다 */

	fq->tail = (idx + 1) & fq->mod_mask;	/* [한국어] 커서를 한 칸 전진시키고 감싼다 */

	return idx;	/* [한국어] 방금 확보한 자리의 인덱스 */
}

/*
 * [한국어]
 * fq_ring_free_locked - 무효화가 끝난 항목들을 실제로 반납한다 (락 없음)
 *
 * @cookie: 도메인의 DMA 상태 (완료 카운터를 읽는다)
 * @fq:     훑을 큐
 *
 * 지연 무효화의 정확성이 이 함수 한 줄에 걸려 있다 — 항목의 counter 가 완료
 * 카운터보다 작을 때만 반납한다. 그 조건이 뜻하는 바는 "이 항목이 큐에 들어간
 * 뒤에 시작된 전체 무효화가 이미 끝났다"이고, 따라서 그 IOVA 를 가리키던 IOTLB
 * 항목은 더 이상 존재하지 않는다.
 *
 * 링은 시간순이므로 조건에 걸리는 첫 항목에서 멈추면 뒤는 볼 필요가 없다.
 *
 * 페이지 테이블 페이지도 같은 조건에서 함께 반납한다. 먼저 놓으면 아직 무효화되지
 * 않은 IOTLB 항목이 참조하던 표가 다른 용도로 재사용된다.
 *
 * 실행 컨텍스트: 큐 락을 든 채. 인터럽트 문맥 가능.
 *
 * 호출 체인: queue_iova, fq_ring_free → [이 함수] → free_iova_fast
 */
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

/*
 * [한국어]
 * fq_ring_free - 락을 잡고 반납을 수행한다
 *
 * @cookie: 도메인의 DMA 상태
 * @fq:     훑을 큐
 *
 * fq_ring_free_locked 의 락 포함판. 타이머 콜백처럼 락을 아직 잡지 않은 문맥에서
 * 쓴다. irqsave 인 것은 이 큐를 인터럽트 문맥의 dma_unmap 도 만지기 때문이다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: fq_flush_timeout → [이 함수]
 */
static void fq_ring_free(struct iommu_dma_cookie *cookie, struct iova_fq *fq)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&fq->lock, flags);	/* [한국어] 큐 보호 */
	fq_ring_free_locked(cookie, fq);	/* [한국어] 락을 잡고 반납 수행 */
	spin_unlock_irqrestore(&fq->lock, flags);	/* [한국어] 락 해제 */
}

/*
 * [한국어]
 * fq_flush_iotlb - 도메인 전체 IOTLB 를 한 번에 비운다
 *
 * @cookie: 도메인의 DMA 상태
 *
 * 지연 무효화의 이득이 여기서 실현된다. 구간별 무효화를 수백 번 내리는 대신
 * 전체를 한 번 비우고, 그동안 큐에 쌓인 IOVA 를 모두 풀어 준다.
 *
 * 카운터를 앞뒤로 감싸는 순서가 핵심이다. start 를 먼저 올리므로, 이 시점 이후에
 * 큐에 들어가는 항목은 더 큰 counter 를 갖게 되어 이번 무효화의 보호를 받지
 * 못한다 — 그것이 정확한 판정이다. finish 를 나중에 올리므로, 그 값을 본 CPU 는
 * 무효화가 실제로 끝났음을 알 수 있다.
 *
 * 실행 컨텍스트: 어디서든. 하드웨어 완료를 기다리므로 비싸다.
 *
 * 호출 체인: queue_iova(큐 포화), fq_flush_timeout → [이 함수]
 *            → domain->ops->flush_iotlb_all
 */
static void fq_flush_iotlb(struct iommu_dma_cookie *cookie)
{
	atomic64_inc(&cookie->fq_flush_start_cnt);	/* [한국어] 무효화를 '시작한다'고 먼저 알린다. 이 시점 이후 큐에 들어가는 항목은 더 큰 counter 를 갖게 되어, 이번 무효화의 보호를 받지 못한다 — 그것이 정확하다 */
	cookie->fq_domain->ops->flush_iotlb_all(cookie->fq_domain);	/* [한국어] 도메인 전체 IOTLB 를 한 번에 비운다. 구간별 무효화를 수천 번 하는 대신 전체를 한 번 — 그것이 지연 무효화의 이득이다 */
	atomic64_inc(&cookie->fq_flush_finish_cnt);	/* [한국어] 완료를 알린다. 이 증가를 본 CPU 는 그 이전 counter 의 항목을 안전하게 반납할 수 있다 */
}

/*
 * [한국어]
 * fq_flush_timeout - 주기적으로 큐를 비운다
 *
 * @t: 타이머 구조체 (여기서 소유 쿠키를 되짚는다)
 *
 * 큐가 차기만 기다리면 DMA 가 뜸해진 뒤 해제된 IOVA 가 무한정 묶인 채 남는다.
 * 이 타이머가 회수의 하한을 보장한다.
 *
 * 무효화를 한 번만 내리고 모든 CPU 의 큐를 훑는 순서가 요점이다. 전체 IOTLB 를
 * 비우는 동작이므로 CPU 별로 반복할 이유가 없고, 그 한 번으로 전 CPU 의 대기분이
 * 함께 풀린다.
 *
 * 타이머 플래그를 맨 먼저 내리는 것도 의도적이다. 아래 작업 도중에 새 항목이
 * 들어오면 그쪽이 타이머를 다시 걸 수 있어야 한다.
 *
 * 실행 컨텍스트: 소프트 인터럽트(타이머 콜백).
 *
 * 호출 체인: 타이머 → [이 함수] → fq_flush_iotlb, fq_ring_free
 */
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

/*
 * [한국어]
 * queue_iova - 해제된 IOVA 를 무효화 대기 큐에 넣는다
 *
 * @cookie:   도메인의 DMA 상태
 * @pfn:      해제된 구간의 시작 pfn
 * @pages:    페이지 수
 * @freelist: 이 해제로 비게 된 페이지 테이블 페이지들
 *
 * 지연 무효화 정책(DMA_FQ)에서 dma_unmap 이 도달하는 곳이다. PTE 는 이미 지워졌지만
 * IOTLB 에는 옛 번역이 남아 있으므로, 그 IOVA 를 곧바로 재사용하면 장치가 이미
 * 반납된 페이지에 닿을 수 있다. 그래서 여기 담아 두었다가 전체 무효화가 한 번
 * 끝난 뒤에 iova.c 로 돌려준다.
 *
 * 맨 앞의 smp_mb() 가 두 가지를 보장한다. 드라이버의 PTE 제거가 이 항목의 등록보다
 * 먼저 보이게 하고(다른 CPU 가 곧바로 무효화를 내려도 지워진 PTE 를 본다),
 * iommu_dma_init_fq 가 쓴 큐 상태를 반쯤 본 채로 만지지 않게 한다.
 *
 * 큐가 가득 차면 지연의 이득을 포기하고 여기서 직접 무효화를 내린다. 그 전에 먼저
 * 이미 완료된 항목을 거두어, 그 상황 자체를 드물게 만든다.
 *
 * 실행 컨텍스트: dma_unmap 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: iommu_dma_free_iova → [이 함수] → fq_ring_free_locked, fq_flush_iotlb
 */
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

/*
 * [한국어]
 * iommu_dma_free_fq_single - 전역 큐 하나를 해제한다
 *
 * @fq: 해제할 큐
 *
 * 남아 있는 항목의 페이지 테이블 페이지만 반납한다. IOVA 는 도메인이 통째로
 * 사라지는 중이라 개별 반납이 무의미하고, put_iova_domain 이 트리를 통째로 비운다.
 *
 * 실행 컨텍스트: 도메인 해체. 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_free_fq → [이 함수]
 */
static void iommu_dma_free_fq_single(struct iova_fq *fq)
{
	int idx;	/* [한국어] 링 순회 커서 */

	fq_ring_for_each(idx, fq)	/* [한국어] 아직 반납되지 않은 항목들에 대해 */
		iommu_put_pages_list(&fq->entries[idx].freelist);	/* [한국어] 페이지 테이블 페이지만 돌려준다. IOVA 는 도메인이 통째로 사라지는 중이라 따로 반납할 필요가 없다 */
	vfree(fq);	/* [한국어] 큐 자체 해제 (vmalloc 으로 잡았다) */
}

/*
 * [한국어]
 * iommu_dma_free_fq_percpu - CPU 별 큐 배열을 해제한다
 *
 * @percpu_fq: 해제할 percpu 큐 배열
 *
 * single 판과 같은 이유로 페이지 테이블 페이지만 거둔다 (위 영어 주석). 다만
 * for_each_possible_cpu 로 도는 것에 주의 — 지금 오프라인인 CPU 의 큐에도 항목이
 * 남아 있을 수 있다.
 *
 * 실행 컨텍스트: 도메인 해체. 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_free_fq → [이 함수]
 */
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

/*
 * [한국어]
 * iommu_dma_free_fq - 도메인의 flush queue 전체를 걷어 낸다
 *
 * @cookie: 도메인의 DMA 상태
 *
 * fq_domain 이 NULL 이면 지연 무효화를 쓰지 않는 도메인이라 할 일이 없다.
 *
 * timer_delete_sync 로 타이머가 끝나기를 기다리는 것이 순서상 중요하다. 콜백이
 * 도는 도중에 큐를 해제하면 해제된 메모리를 훑게 된다.
 *
 * 실행 컨텍스트: 도메인 해체. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_put_dma_cookie → [이 함수]
 */
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

/*
 * [한국어]
 * iommu_dma_init_one_fq - 큐 하나의 링 상태를 초기화한다
 *
 * @fq:      초기화할 큐 (메모리는 이미 확보되어 있다)
 * @fq_size: 항목 수. 반드시 2의 거듭제곱이어야 한다.
 *
 * mod_mask 를 fq_size - 1 로 두는 것이 링의 전부다. 크기가 2의 거듭제곱이라
 * 인덱스를 나눗셈 없이 AND 한 번으로 감쌀 수 있고, 그래서 핫패스에서 비용이 없다.
 *
 * freelist 초기화는 처음 한 번만 필요하다 — 항목은 반납 뒤에도 재사용되며
 * fq_ring_free_locked 가 다시 빈 상태로 되돌려 놓는다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_init_fq_single/percpu → [이 함수]
 */
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

/*
 * [한국어]
 * iommu_dma_init_fq_single - 시스템 전체가 공유할 큐 하나를 만든다
 *
 * @cookie: 도메인의 DMA 상태
 * @return: 0 성공, -ENOMEM 이면 지연 무효화를 켤 수 없다
 *
 * 32768 항목이라 수 MB 에 이른다. 물리 연속을 요구할 수 없으므로 vmalloc 을 쓰며,
 * 큐 접근이 이미 락 아래이므로 vmalloc 의 접근 비용은 문제가 되지 않는다.
 *
 * 이 구성은 무효화 한 번이 매우 비싼 환경(하이퍼바이저로 트랩되는 그림자 페이지
 * 테이블 등)에서 선택된다. 큐가 클수록 무효화 한 번에 정리되는 양이 많다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_dma_init_fq → [이 함수]
 */
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

/*
 * [한국어]
 * iommu_dma_init_fq_percpu - CPU 마다 작은 큐를 하나씩 만든다 (기본 구성)
 *
 * @cookie: 도메인의 DMA 상태
 * @return: 0 성공, -ENOMEM 이면 지연 무효화를 켤 수 없다
 *
 * 각 CPU 가 자기 큐에만 넣으므로 락 경쟁이 사실상 없다. 대신 큐가 작아(256) 자주
 * 차고, 그때마다 전체 무효화가 일어난다.
 *
 * for_each_possible_cpu 로 초기화하는 것이 중요하다 — 나중에 핫플러그로 올라오는
 * CPU 도 자기 큐를 이미 가지고 있어야 하며, 그때 아토믹 문맥에서 할당할 수는 없다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_dma_init_fq → [이 함수]
 */
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
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_dma_init_fq - 도메인에 지연 무효화를 켠다
 *
 * @domain: 대상 도메인
 * @return: 0 성공, -ENOMEM 이면 즉시 무효화로 남는다
 *
 * sysfs 로 type 을 DMA-FQ 로 바꿀 때, 그리고 도메인 초기화 때 불린다. 이미 켜져
 * 있으면 아무 일도 하지 않는 멱등 함수다.
 *
 * 마지막 두 줄의 순서가 이 함수의 계약이다. smp_wmb() 뒤에 fq_domain 을 쓰는데,
 * 그 대입이 곧 "지연 무효화 켜짐" 스위치이기 때문이다. 해제 경로가 이 필드를 보고
 * queue_iova 로 들어가므로, 큐가 완성되기 전에 켜면 다른 CPU 가 반쯤 만들어진
 * 링을 만지게 된다.
 *
 * 실행 컨텍스트: 그룹 락 아래(sysfs 경로) 또는 도메인 초기화. 프로세스 문맥.
 *
 * 호출 체인: iommu_group_store_type, iommu_dma_init_domain → [이 함수]
 */
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_get_dma_cookie - 도메인을 커널 DMA API 용으로 표시하고 상태를 붙인다
 *
 * @domain: 준비할 도메인
 * @return: 0 성공, -EEXIST 면 이미 다른 쿠키가 있다, -ENOMEM 이면 할당 실패
 *
 * cookie_type 을 IOMMU_COOKIE_DMA_IOVA 로 세우는 것이 이 함수의 실질이다. 그
 * 값이 iommu.c 곳곳의 분기 기준이 된다 — 도메인 해제 시 무엇을 풀지, MSI 매핑을
 * 누가 담당할지, 폴트 핸들러 자리를 쓸 수 있는지가 모두 여기서 갈린다.
 *
 * IOVA 공간 자체는 아직 세우지 않는다. 그것은 장치의 주소 제약을 알아야 가능해서
 * iommu_dma_init_domain 이 장치와 함께 처리한다. 그 사이 iovad.granule 이 0 인
 * 것이 "아직 세워지지 않음"의 표식이 된다.
 *
 * 실행 컨텍스트: 도메인 생성. 프로세스 문맥.
 *
 * 호출 체인: iommu.c 의 iommu_setup_default_domain → [이 함수]
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_get_msi_cookie - MSI 재매핑만 코어에 맡긴다
 *
 * @domain: 준비할 도메인 (UNMANAGED 여야 한다)
 * @base:   MSI 매핑에 쓸 IOVA 창의 시작 주소
 * @return: 0 성공, -EINVAL 이면 도메인 종류가 맞지 않음, -EEXIST 면 쿠키 중복
 *
 * VFIO 처럼 IOVA 를 직접 관리하는 사용자를 위한 것이다. DMA API 는 쓰지 않지만
 * MSI 도어벨 매핑만은 코어에 맡기고 싶을 때 쓴다 — 그 매핑을 직접 만들려면
 * 인터럽트 컨트롤러의 도어벨 주소를 알아야 하는데, 그것은 아키텍처마다 다르다.
 *
 * 할당자가 없으므로 호출자가 충분히 큰 연속 창을 미리 예약해 base 로 넘겨야 하고,
 * 코어는 그 창에서 순서대로 페이지를 떼어 쓴다 (위 영어 주석).
 *
 * 실행 컨텍스트: 도메인 생성 직후. 프로세스 문맥.
 *
 * 호출 체인: VFIO → [이 함수]
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_put_dma_cookie - DMA 상태를 통째로 해제한다
 *
 * @domain: 사라지는 도메인
 *
 * 해제 순서가 정해져 있다. flush queue 먼저(타이머 정지 포함), 그 다음 IOVA 공간,
 * 마지막에 MSI 기록이다. 큐가 IOVA 를 참조하고 있으므로 뒤집으면 해제된 것을
 * 만진다.
 *
 * iovad.granule 검사가 필요한 이유는 쿠키만 만들어지고 iommu_dma_init_domain 이
 * 불리기 전에 도메인이 해제되는 경로가 있기 때문이다. 그때는 IOVA 공간이 없다.
 *
 * 실행 컨텍스트: 도메인 해제. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu.c 의 iommu_domain_free → [이 함수]
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_put_msi_cookie - 축소판 쿠키를 해제한다
 *
 * @domain: 사라지는 도메인
 *
 * 매핑 기록만 거둔다. 실제 IOVA 매핑은 도메인의 페이지 테이블과 함께 사라지고,
 * IOVA 창은 애초에 호출자 소유였다.
 *
 * 실행 컨텍스트: 도메인 해제. 프로세스 문맥.
 *
 * 호출 체인: iommu.c 의 iommu_domain_free → [이 함수]
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_dma_get_resv_regions - 펌웨어가 기술한 공통 예약 구간을 모은다
 *
 * @dev:  대상 장치
 * @list: 예약 구간을 매달 목록
 *
 * 벤더 드라이버의 get_resv_regions 콜백이 자기 하드웨어 고유 구간(RMRR 등)을
 * 알리기 전에, 아키텍처 공통 부분을 이 함수로 채운다. 현재는 주로 ARM 플랫폼의
 * GICv3 ITS 창이며, 그 주소로 오는 쓰기는 인터럽트로 해석되므로 데이터 DMA 가
 * 절대 그 자리를 쓰면 안 된다 (위 영어 주석).
 *
 * DT 와 ACPI 를 모두 확인하는 것에 주의할 것 — 두 방식이 공존할 수 있다.
 *
 * 실행 컨텍스트: 프로브 경로. 프로세스 문맥.
 *
 * 호출 체인: 벤더 드라이버의 get_resv_regions → [이 함수]
 */
void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)
{

	if (!is_of_node(dev_iommu_fwspec_get(dev)->iommu_fwnode))	/* [한국어] 장치 트리 노드가 아니면 = ACPI 시스템 */
		iort_iommu_get_resv_regions(dev, list);	/* [한국어] IORT 표에서 GICv3 ITS 창 같은 하드웨어 MSI 구간을 읽어 예약 목록에 넣는다 (위 영어 주석) */

	if (dev->of_node)	/* [한국어] 장치 트리 노드가 있으면 */
		of_iommu_get_resv_regions(dev, list);	/* [한국어] DT 에서 같은 정보를 읽는다 */
}
EXPORT_SYMBOL(iommu_dma_get_resv_regions);	/* [한국어] 벤더 드라이버가 자기 get_resv_regions 콜백에서 이 공통 부분을 그대로 부른다 */

/*
 * [한국어]
 * cookie_init_hw_msi_region - 하드웨어가 고정한 MSI 창을 항등 매핑으로 미리 등록한다
 *
 * @cookie: 도메인의 DMA 상태
 * @start:  창의 시작 물리 주소
 * @end:    창의 끝
 * @return: 0 성공, -ENOMEM 이면 기록 생성 실패
 *
 * IOMMU_RESV_MSI 구간은 하드웨어가 그 주소를 그대로 인터럽트로 해석하는 창이다.
 * 즉 번역이 개입할 수 없으므로 IOVA == 물리 주소로 쓸 수밖에 없고, 그래서 창의
 * 각 페이지에 대해 iova == phys 인 기록을 미리 만들어 둔다.
 *
 * 실제 매핑을 만들지 않는다는 점이 중요하다. 그 범위는 이미 reserve_iova 로
 * 할당 대상에서 빠져 있고, 하드웨어가 번역 없이 처리하므로 페이지 테이블에 넣을
 * 것이 없다. 이 목록은 나중에 iommu_dma_get_msi_page 가 조회할 때 쓰인다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥.
 *
 * 호출 체인: iova_reserve_iommu_regions → [이 함수]
 */
static int cookie_init_hw_msi_region(struct iommu_dma_cookie *cookie,
		phys_addr_t start, phys_addr_t end)
{
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] 이 도메인의 IOVA 공간 */
	struct iommu_dma_msi_page *msi_page;	/* [한국어] 만들 매핑 기록 */
	int i, num_pages;	/* [한국어] 페이지 순회 커서와 개수 */

	start -= iova_offset(iovad, start);	/* [한국어] 페이지 경계로 내림 — 도어벨은 페이지 단위로만 매핑할 수 있다 */
	num_pages = iova_align(iovad, end - start) >> iova_shift(iovad);	/* [한국어] 창 전체를 덮는 페이지 수 */

	for (i = 0; i < num_pages; i++) {	/* [한국어] 각 페이지마다 기록을 하나씩 */
		msi_page = kmalloc_obj(*msi_page);	/* [한국어] 매핑 기록 */
		if (!msi_page)	/* [한국어] 할당 실패 */
			return -ENOMEM;	/* [한국어] 이미 만든 것들은 쿠키 목록에 남아 해제 시 함께 정리된다 */

		msi_page->phys = start;	/* [한국어] 도어벨의 물리 주소 */
		msi_page->iova = start;	/* [한국어] IOVA 도 같은 값이다 — 이 창은 예약되어 있어 항등으로 쓰므로 실제 매핑을 만들 필요조차 없다. 하드웨어 MSI 창(IOMMU_RESV_MSI)이 이런 형태다 */
		INIT_LIST_HEAD(&msi_page->list);	/* [한국어] 목록 고리 초기화 */
		list_add(&msi_page->list, &cookie->msi_page_list);	/* [한국어] 도메인 목록에 등록 — 이후 조회가 여기서 곧바로 찾는다 */
		start += iovad->granule;	/* [한국어] 다음 페이지로 */
	}

	return 0;	/* [한국어] 창 전체를 기록했다 */
}

/*
 * [한국어]
 * iommu_dma_ranges_sort - dma_ranges 항목을 시작 주소 오름차순으로 비교한다
 *
 * @priv:   list_sort 가 넘겨 주는 문맥 (여기서는 쓰지 않는다)
 * @a, @b:  비교할 두 항목
 * @return: a 가 뒤에 와야 하면 1
 *
 * iova_reserve_pci_windows 가 "허용된 창들 사이의 틈"을 계산하려면 목록이 정렬되어
 * 있어야 한다. 펌웨어가 순서를 보장하지 않으므로 여기서 한 번 정렬한다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥.
 *
 * 호출 체인: list_sort → [이 함수]
 */
static int iommu_dma_ranges_sort(void *priv, const struct list_head *a,
		const struct list_head *b)
{
	struct resource_entry *res_a = list_entry(a, typeof(*res_a), node);	/* [한국어] 비교할 첫 항목 */
	struct resource_entry *res_b = list_entry(b, typeof(*res_b), node);	/* [한국어] 비교할 둘째 항목 */

	return res_a->res->start > res_b->res->start;	/* [한국어] 시작 주소 오름차순. 아래 예약 루프가 '구간 사이의 틈'을 계산하려면 정렬이 전제된다 */
}

/*
 * [한국어]
 * iova_reserve_pci_windows - PCI 브리지의 주소 제약을 IOVA 공간에 반영한다
 *
 * @dev:   PCI 장치
 * @iovad: 이 장치가 쓸 IOVA 공간
 * @return: 0 성공, -EINVAL 이면 펌웨어가 기술한 창이 서로 겹친다
 *
 * IOVA 는 장치가 버스에 내는 주소이고, 그 버스에는 다른 장치의 MMIO 창도 함께
 * 놓여 있다. 두 가지를 반영해야 한다.
 *
 *  1) 브리지의 메모리 창(bridge->windows): 그 주소로 간 트랜잭션은 메모리가 아니라
 *     다른 장치의 레지스터로 간다. IOVA 로 내주면 DMA 가 P2P 쓰기로 바뀐다.
 *  2) DMA 허용 범위(bridge->dma_ranges)의 여집합: 그 범위 밖 주소는 브리지가
 *     상류로 통과시키지 않는다. 목록을 정렬한 뒤 창들 사이의 '틈'과 마지막 창
 *     뒤의 영역을 예약하는 것이 그 여집합을 구하는 방식이다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥.
 *
 * 호출 체인: iova_reserve_iommu_regions → [이 함수] → reserve_iova
 */
static int iova_reserve_pci_windows(struct pci_dev *dev,
		struct iova_domain *iovad)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(dev->bus);	/* [한국어] 이 장치가 달린 호스트 브리지 — 주소 창 정보를 들고 있다 */
	struct resource_entry *window;	/* [한국어] 창 순회 커서 */
	unsigned long lo, hi;	/* [한국어] 예약할 pfn 범위 */
	phys_addr_t start = 0, end;	/* [한국어] dma_ranges 사이의 '틈'을 계산할 커서 */

	resource_list_for_each_entry(window, &bridge->windows) {	/* [한국어] 브리지가 CPU 쪽으로 노출하는 창들 */
		if (resource_type(window->res) != IORESOURCE_MEM)	/* [한국어] I/O 포트 창은 DMA 주소와 겹치지 않는다 */
			continue;

		lo = iova_pfn(iovad, window->res->start - window->offset);	/* [한국어] 버스 주소로 환산한 시작 */
		hi = iova_pfn(iovad, window->res->end - window->offset);	/* [한국어] 끝 */
		reserve_iova(iovad, lo, hi);	/* [한국어] 이 범위를 IOVA 로 내주면 안 된다. 장치가 그 주소로 DMA 하면 메모리가 아니라 다른 장치의 MMIO 로 가 버리기 때문이다 — P2P 창과 DMA 주소의 충돌 */
	}

	/* Get reserved DMA windows from host bridge */
	list_sort(NULL, &bridge->dma_ranges, iommu_dma_ranges_sort);	/* [한국어] 아래 루프가 '허용된 창들 사이의 틈'을 계산하므로 시작 주소순 정렬이 필요하다 */
	resource_list_for_each_entry(window, &bridge->dma_ranges) {	/* [한국어] 브리지가 DMA 를 허용하는 범위들 */
		end = window->res->start - window->offset;	/* [한국어] 이번 허용 창의 시작 = 직전 틈의 끝 */
resv_iova:	/* [한국어] 마지막 창 뒤의 남은 영역을 처리하려고 되돌아오는 지점 */
		if (end > start) {	/* [한국어] 직전 창 끝과 이번 창 시작 사이에 틈이 있다 */
			lo = iova_pfn(iovad, start);	/* [한국어] 틈의 시작 */
			hi = iova_pfn(iovad, end);	/* [한국어] 틈의 끝 */
			reserve_iova(iovad, lo, hi);	/* [한국어] 그 틈은 브리지가 DMA 를 통과시키지 않는 구간이므로 IOVA 로 내주면 안 된다. dma_ranges 는 '허용 목록'이고 이 루프는 그 여집합을 예약한다 */
		} else if (end < start) {	/* [한국어] 창들이 겹쳤다 */
			/* DMA ranges should be non-overlapping */
			dev_err(&dev->dev,	/* [한국어] 펌웨어가 기술한 dma_ranges 가 잘못된 것이다 */
				"Failed to reserve IOVA [%pa-%pa]\n",	/* [한국어] 겹친 범위를 그대로 남긴다 */
				&start, &end);	/* [한국어] 문제의 두 경계 */
			return -EINVAL;	/* [한국어] 이 상태로는 안전한 주소 공간을 만들 수 없다 */
		}

		start = window->res->end - window->offset + 1;	/* [한국어] 다음 틈의 시작 = 이번 창의 끝 바로 다음 */
		/* If window is last entry */
		if (window->node.next == &bridge->dma_ranges &&	/* [한국어] 마지막 창이고 */
		    end != ~(phys_addr_t)0) {	/* [한국어] 아직 주소 공간 끝까지 처리하지 않았다면 */
			end = ~(phys_addr_t)0;	/* [한국어] 끝을 주소 공간 최상단으로 놓고 */
			goto resv_iova;	/* [한국어] 마지막 창 뒤의 영역도 예약한다 */
		}
	}

	return 0;	/* [한국어] 브리지 제약이 모두 IOVA 공간에 반영되었다 */
}

/*
 * [한국어]
 * iova_reserve_iommu_regions - 이 장치가 쓰면 안 되는 주소를 모두 IOVA 공간에서 뺀다
 *
 * @dev:    대상 장치
 * @domain: 그 장치가 붙을 도메인
 * @return: 0 성공, 음수면 실패
 *
 * 도메인 초기화의 마지막 단계다. 세 출처에서 제약이 온다 — PCI 브리지 토폴로지,
 * IOMMU 드라이버가 아는 하드웨어 예약 구간, 펌웨어가 기술한 직통 매핑이다.
 *
 * SW_MSI 를 건너뛰는 것이 유일한 예외다. 그 창은 이 파일 자신이 관리하며, 예약해
 * 두는 대신 필요할 때 정상적으로 할당해 쓴다 (위 영어 주석). 반대로 하드웨어가
 * 고정한 MSI 창(IOMMU_RESV_MSI)은 예약과 동시에 항등 매핑 기록까지 만든다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_dma_init_domain → [이 함수]
 *            → iova_reserve_pci_windows, iommu_get_resv_regions, reserve_iova
 */
static int iova_reserve_iommu_regions(struct device *dev,
		struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] 이 도메인의 DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	struct iommu_resv_region *region;	/* [한국어] 예약 구간 순회 커서 */
	LIST_HEAD(resv_regions);	/* [한국어] 드라이버가 알려 줄 구간을 받을 목록 */
	int ret = 0;	/* [한국어] 결과 */

	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치면 */
		ret = iova_reserve_pci_windows(to_pci_dev(dev), iovad);	/* [한국어] 브리지의 주소 창 제약을 먼저 반영한다 */
		if (ret)	/* [한국어] 브리지 정보가 모순되면 */
			return ret;	/* [한국어] 도메인을 세울 수 없다 */
	}

	iommu_get_resv_regions(dev, &resv_regions);	/* [한국어] IOMMU 드라이버와 펌웨어가 알리는 예약 구간 (RMRR, MSI 창 등) */
	list_for_each_entry(region, &resv_regions, list) {	/* [한국어] 하나씩 */
		unsigned long lo, hi;	/* [한국어] 예약할 pfn 범위 */

		/* We ARE the software that manages these! */
		if (region->type == IOMMU_RESV_SW_MSI)	/* [한국어] 소프트웨어 MSI 창은 이 파일이 직접 관리한다 (위 영어 주석) */
			continue;	/* [한국어] 예약하지 않는다 — 나중에 alloc_iova 로 정상 할당해 쓸 것이기 때문 */

		lo = iova_pfn(iovad, region->start);	/* [한국어] 구간의 시작 pfn */
		hi = iova_pfn(iovad, region->start + region->length - 1);	/* [한국어] 끝 pfn (닫힌 구간) */
		reserve_iova(iovad, lo, hi);	/* [한국어] 할당 대상에서 뺀다 */

		if (region->type == IOMMU_RESV_MSI)	/* [한국어] 하드웨어가 고정한 MSI 창이면 */
			ret = cookie_init_hw_msi_region(cookie, region->start,	/* [한국어] 그 창의 각 페이지를 항등 매핑 기록으로 미리 등록해 둔다 */
					region->start + region->length);	/* [한국어] 창의 끝 */
		if (ret)	/* [한국어] 기록 생성 실패 */
			break;	/* [한국어] 중단 */
	}
	iommu_put_resv_regions(dev, &resv_regions);	/* [한국어] 드라이버에게 목록을 돌려준다 */

	return ret;	/* [한국어] 0 이면 이 장치가 쓰면 안 되는 모든 주소가 IOVA 공간에서 빠졌다 */
}

/*
 * [한국어]
 * dev_is_untrusted - 물리적으로 탈착 가능한 위치에 꽂힌 장치인가
 *
 * @dev:    대상 장치
 * @return: true 면 신뢰할 수 없다
 *
 * PCI 코어가 Thunderbolt 등 외부에서 꽂을 수 있는 경로 뒤의 장치에 표시해 준다.
 * 이 판정 하나가 이 파일의 여러 정책을 바꾼다 — 부분 페이지 매핑을 바운스 버퍼로
 * 우회시키고, 최대 매핑 크기를 바운스 버퍼 크기로 제한하며, sg 병합을 포기하게
 * 만든다.
 *
 * 이유는 하나다. IOMMU 는 페이지 단위로만 매핑하므로, 페이지보다 작은 버퍼를
 * 그대로 매핑하면 같은 페이지의 다른 커널 데이터까지 그 장치에 열린다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: dev_use_swiotlb, dev_use_sg_swiotlb, iommu_dma_max_mapping_size → [이 함수]
 */
static bool dev_is_untrusted(struct device *dev)
{
	return dev_is_pci(dev) && to_pci_dev(dev)->untrusted;	/* [한국어] 물리적으로 접근 가능한 위치(Thunderbolt 등)에 꽂힌 장치. PCI 코어가 표시해 준다 */
}

/*
 * [한국어]
 * dev_use_swiotlb - 이 매핑을 바운스 버퍼로 우회시켜야 하는가
 *
 * @dev:    대상 장치
 * @size:   매핑 길이
 * @dir:    DMA 방향
 * @return: true 면 바운스 필요
 *
 * 두 가지 이유가 있다. 신뢰할 수 없는 장치는 부분 페이지 노출을 막기 위해서이고,
 * kmalloc 버퍼는 캐시라인 정렬 문제 때문이다 — 비일관 장치에서 캐시 관리를 하면
 * 같은 캐시라인을 공유하는 이웃 데이터까지 무효화되거나 덮어써진다.
 *
 * 이 함수는 "바운스가 필요한 장치인가"만 판정하고, 실제로 우회할지는 호출자가
 * iova_unaligned 와 함께 본다. 정렬이 맞으면 노출될 여지가 없어 바운스가 불필요하다.
 *
 * 실행 컨텍스트: 매핑 경로. 어디서든.
 *
 * 호출 체인: iommu_dma_map_phys, dma_iova_link, sync 계열 → [이 함수]
 */
static bool dev_use_swiotlb(struct device *dev, size_t size,
			    enum dma_data_direction dir)
{
	return IS_ENABLED(CONFIG_SWIOTLB) &&	/* [한국어] 바운스 버퍼가 빌드에 있어야 하고 */
		(dev_is_untrusted(dev) ||	/* [한국어] 신뢰할 수 없는 장치이거나 — 그런 장치에는 IOMMU 페이지 하나를 통째로 내줘야 하는데, 요청이 페이지보다 작으면 같은 페이지의 다른 데이터가 함께 노출된다 */
		 dma_kmalloc_needs_bounce(dev, size, dir));	/* [한국어] 또는 kmalloc 버퍼가 캐시라인 경계에 맞지 않아, 비일관 장치에서 캐시 관리가 이웃 데이터를 건드릴 수 있는 경우 */
}

/*
 * [한국어]
 * dev_use_sg_swiotlb - 이 scatterlist 를 통째로 바운스해야 하는가
 *
 * @dev:    대상 장치
 * @sg:     첫 세그먼트
 * @nents:  세그먼트 수
 * @dir:    DMA 방향
 * @return: true 면 리스트 전체를 바운스 경로로
 *
 * 부분 바운스가 불가능하다는 점이 단일 매핑판과의 차이다. 일부 세그먼트만 바운스
 * 버퍼로 옮기면 그 버퍼들이 원본과 물리적으로 인접하지 않아, 하나의 연속 IOVA
 * 창으로 접는 전제가 깨진다. 그래서 한 세그먼트라도 위험하면 전부 바운스한다.
 *
 * 실행 컨텍스트: 매핑 경로. 어디서든.
 *
 * 호출 체인: iommu_dma_map_sg → [이 함수]
 */
static bool dev_use_sg_swiotlb(struct device *dev, struct scatterlist *sg,
			       int nents, enum dma_data_direction dir)
{
	struct scatterlist *s;	/* [한국어] 세그먼트 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	if (!IS_ENABLED(CONFIG_SWIOTLB))	/* [한국어] 바운스 버퍼가 없는 빌드 */
		return false;	/* [한국어] 우회할 방법이 없다 */

	if (dev_is_untrusted(dev))	/* [한국어] 신뢰할 수 없는 장치는 세그먼트를 볼 것도 없이 */
		return true;	/* [한국어] 전부 바운스 */

	/*
	 * If kmalloc() buffers are not DMA-safe for this device and
	 * direction, check the individual lengths in the sg list. If any
	 * element is deemed unsafe, use the swiotlb for bouncing.
	 */
	if (!dma_kmalloc_safe(dev, dir)) {	/* [한국어] 이 장치·방향에서 kmalloc 버퍼가 DMA 안전하지 않다면 (위 영어 주석) */
		for_each_sg(sg, s, nents, i)	/* [한국어] 각 세그먼트의 길이를 확인해 */
			if (!dma_kmalloc_size_aligned(s->length))	/* [한국어] 캐시라인 정렬이 보장되지 않는 크기가 하나라도 있으면 */
				return true;	/* [한국어] 리스트 전체를 바운스한다 — 부분만 바운스하면 주소 연속성이 깨진다 */
	}

	return false;	/* [한국어] 그대로 매핑해도 안전하다 */
}

/**
 * iommu_dma_init_options - Initialize dma-iommu options
 * @options: The options to be initialized
 * @dev: Device the options are set for
 *
 * This allows tuning dma-iommu specific to device properties
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_dma_init_options - 하드웨어 특성에 맞는 큐 정책을 고른다
 *
 * @options: 채울 옵션 구조체
 * @dev:     정책의 근거가 될 장치
 *
 * 판단 기준은 shadow_on_flush 하나다. 무효화가 하이퍼바이저로 트랩되어 그림자
 * 페이지 테이블을 갱신하는 환경에서는 무효화 한 번의 비용이 수십 배 비싸므로,
 * 큐를 크게(32768) 하나만 두고 주기를 길게(1초) 잡아 무효화 횟수 자체를 줄인다.
 *
 * 그 외에는 CPU 별 작은 큐가 유리하다 — 락 경쟁이 없고, 무효화가 싸므로 자주
 * 하더라도 손해가 크지 않다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_init_domain → [이 함수]
 */
static void iommu_dma_init_options(struct iommu_dma_options *options,
				   struct device *dev)
{
	/* Shadowing IOTLB flushes do better with a single large queue */
	if (dev->iommu->shadow_on_flush) {	/* [한국어] 무효화가 하이퍼바이저로 트랩되어 그림자 페이지 테이블을 갱신하는 환경(중첩 가상화 등). 무효화 한 번의 비용이 매우 크다 */
		options->qt = IOMMU_DMA_OPTS_SINGLE_QUEUE;	/* [한국어] 큐를 하나만 두고 */
		options->fq_timeout = IOVA_SINGLE_FQ_TIMEOUT;	/* [한국어] 주기를 길게(1초) */
		options->fq_size = IOVA_SINGLE_FQ_SIZE;	/* [한국어] 크게(32768) 잡아, 무효화 한 번에 정리되는 양을 최대화한다 (위 영어 주석) */
	} else {
		options->qt = IOMMU_DMA_OPTS_PER_CPU_QUEUE;	/* [한국어] 보통은 CPU 별 큐 — 락 경쟁이 없다 */
		options->fq_size = IOVA_DEFAULT_FQ_SIZE;	/* [한국어] 작게(256) */
		options->fq_timeout = IOVA_DEFAULT_FQ_TIMEOUT;	/* [한국어] 주기도 짧게(10ms) */
	}
}

/*
 * [한국어]
 * iommu_domain_supports_fq - 이 도메인에서 지연 무효화를 쓸 수 있는가
 *
 * @dev:    대상 장치
 * @domain: 확인할 도메인
 * @return: true 면 DMA-FQ 가 가능하다
 *
 * 지연 무효화는 "전체 IOTLB 를 한 번에 비우기"에 의존한다. 그 동작을 제대로
 * 제공하지 못하는 하드웨어에서는 쓸 수 없으므로 드라이버에게 능력을 물어본다.
 *
 * 공용 페이지 테이블 계층(iommupt)으로 만든 도메인은 항상 지원한다 — 그 계층이
 * 무효화 의미를 스스로 보장하기 때문이다.
 *
 * 실행 컨텍스트: 도메인 초기화. 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_init_domain → [이 함수]
 */
static bool iommu_domain_supports_fq(struct device *dev,
				     struct iommu_domain *domain)
{
	/* iommupt always supports DMA-FQ */
	if (iommupt_from_domain(domain))	/* [한국어] 공용 페이지 테이블 계층으로 만든 도메인이면 */
		return true;	/* [한국어] 그쪽은 지연 무효화를 항상 지원한다 */
	return device_iommu_capable(dev, IOMMU_CAP_DEFERRED_FLUSH);	/* [한국어] 레거시 드라이버는 능력을 직접 물어본다. 범위 무효화를 제대로 못 하는 하드웨어는 지연 무효화를 쓸 수 없다 */
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_dma_init_domain - 도메인에 실제 IOVA 주소 공간을 세운다
 *
 * @domain: iommu_get_dma_cookie 를 이미 거친 도메인
 * @dev:    이 도메인을 쓸 장치 (주소 제약의 출처)
 * @return: 0 성공, -EINVAL/-EFAULT 면 이 장치를 이 도메인에 붙일 수 없다
 *
 * 쿠키를 만드는 것과 IOVA 공간을 세우는 것을 나눠 둔 이유가 여기 있다. 주소 공간의
 * 입도와 하한은 IOMMU 의 능력과 장치의 주소 범위가 모두 정해져야 결정할 수 있고,
 * 그것은 장치가 도메인에 붙는 시점에야 알 수 있다.
 *
 * 재초기화가 허용된다는 점이 중요하다. 같은 도메인에 여러 장치가 붙으면 이 함수가
 * 장치마다 불리는데, 두 번째부터는 조건이 같은지만 확인하고 돌아간다. 조건이
 * 다르면 이미 나간 IOVA 들의 의미가 바뀌므로 거절한다 — 그것이 "서로 다른 주소
 * 제약을 가진 장치는 같은 도메인을 공유할 수 없다"의 구현이다.
 *
 * 마지막의 DMA-FQ 후퇴가 이 파일의 성격을 보여 준다. 지연 무효화를 못 켜면 조용히
 * 즉시 무효화로 내려앉는다 — 느려질 뿐 정확성은 그대로이기 때문이다.
 *
 * 실행 컨텍스트: 장치 프로브 경로. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_setup_dma_ops → [이 함수]
 *            → init_iova_domain, iova_domain_init_rcaches, iommu_dma_init_fq,
 *              iova_reserve_iommu_regions
 */
static int iommu_dma_init_domain(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] 이 도메인의 DMA 상태 */
	const struct bus_dma_region *map = dev->dma_range_map;	/* [한국어] 장치가 실제로 낼 수 있는 주소 범위 (DT/ACPI 가 기술) */
	unsigned long order, base_pfn;	/* [한국어] IOVA 입도의 로그값과 할당 하한 */
	struct iova_domain *iovad;	/* [한국어] 세울 IOVA 공간 */
	int ret;	/* [한국어] 결과 */

	if (!cookie || domain->cookie_type != IOMMU_COOKIE_DMA_IOVA)	/* [한국어] DMA API 용 쿠키가 붙은 도메인이 아니다 */
		return -EINVAL;	/* [한국어] 초기화 대상이 아니다 */

	iovad = &cookie->iovad;	/* [한국어] 쿠키 안에 박힌 IOVA 공간 */

	/* Use the smallest supported page size for IOVA granularity */
	order = __ffs(domain->pgsize_bitmap);	/* [한국어] IOMMU 가 지원하는 가장 작은 페이지를 IOVA 입도로 삼는다 — 그보다 잘게 나눠 봐야 매핑할 수 없다 */
	base_pfn = 1;	/* [한국어] 0 을 비워 둔다. DMA 주소 0 을 오류값으로 쓰는 코드가 많아, 유효한 매핑이 그 자리에 오면 안 된다 (위 영어 주석) */

	/* Check the domain allows at least some access to the device... */
	if (map) {	/* [한국어] 장치의 주소 범위 정보가 있으면 */
		if (dma_range_map_min(map) > domain->geometry.aperture_end ||	/* [한국어] 장치가 낼 수 있는 최소 주소가 IOMMU 창보다 위이거나 */
		    dma_range_map_max(map) < domain->geometry.aperture_start) {	/* [한국어] 최대 주소가 창보다 아래면 — 겹치는 구간이 전혀 없다 */
			pr_warn("specified DMA range outside IOMMU capability\n");	/* [한국어] 펌웨어 기술과 하드웨어가 모순된다 */
			return -EFAULT;	/* [한국어] 이 장치는 이 IOMMU 아래에서 DMA 할 수 없다 */
		}
	}
	/* ...then finally give it a kicking to make sure it fits */
	base_pfn = max_t(unsigned long, base_pfn,	/* [한국어] IOMMU 창의 시작보다 아래는 쓸 수 없다 */
			 domain->geometry.aperture_start >> order);	/* [한국어] 창 시작을 pfn 으로 환산해 하한에 반영 */

	/* start_pfn is always nonzero for an already-initialised domain */
	if (iovad->start_pfn) {	/* [한국어] 이미 초기화된 도메인이다 (위 영어 주석 — 0 이 아니면 세워진 것이다) */
		if (1UL << order != iovad->granule ||	/* [한국어] 입도가 다르거나 */
		    base_pfn != iovad->start_pfn) {	/* [한국어] 하한이 다르면 — 이미 나간 IOVA 들의 의미가 바뀐다 */
			pr_warn("Incompatible range for DMA domain\n");	/* [한국어] 기존 매핑을 무효로 만들 수 없다 */
			return -EFAULT;	/* [한국어] 같은 도메인에 조건이 다른 장치를 붙일 수 없다 */
		}

		return 0;	/* [한국어] 같은 조건이면 재초기화는 무해하다 — 그냥 성공으로 돌아간다 */
	}

	init_iova_domain(iovad, 1UL << order, base_pfn);	/* [한국어] IOVA 공간을 세운다 (앵커 심기 포함) */
	ret = iova_domain_init_rcaches(iovad);	/* [한국어] CPU 별 캐시 계층까지 */
	if (ret)	/* [한국어] 캐시 준비 실패 */
		return ret;	/* [한국어] 성능이 아니라 메모리 문제이므로 실패로 처리한다 */

	iommu_dma_init_options(&cookie->options, dev);	/* [한국어] 하드웨어 특성을 보고 큐 정책을 정한다 */

	/* If the FQ fails we can simply fall back to strict mode */
	if (domain->type == IOMMU_DOMAIN_DMA_FQ &&	/* [한국어] 지연 무효화를 쓰기로 되어 있는데 */
	    (!iommu_domain_supports_fq(dev, domain) ||	/* [한국어] 하드웨어가 지원하지 않거나 */
	     iommu_dma_init_fq(domain)))	/* [한국어] 큐를 못 만들었다면 */
		domain->type = IOMMU_DOMAIN_DMA;	/* [한국어] 조용히 즉시 무효화로 내려앉는다. 성능은 떨어져도 정확성은 그대로다 (위 영어 주석) */

	return iova_reserve_iommu_regions(dev, domain);	/* [한국어] 마지막으로 이 장치가 쓰면 안 되는 주소들을 IOVA 공간에서 뺀다 */
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dma_info_to_prot - DMA API 의 방향·속성을 IOMMU 페이지 권한으로 옮긴다
 *
 * @dir:      DMA 방향
 * @coherent: 장치가 캐시 일관성을 갖는가
 * @attrs:    DMA 속성
 * @return:   IOMMU_READ/WRITE/CACHE/PRIV/MMIO 조합
 *
 * 이 작은 변환에 IOMMU 격리의 실질이 들어 있다. DMA_TO_DEVICE 는 읽기 전용 PTE 가
 * 되므로, 장치가 그 버퍼를 덮어쓰려 하면 폴트가 난다 — 방향을 정확히 신고한
 * 드라이버는 버그 있는 장치로부터 자기 데이터를 지킬 수 있다.
 *
 * 캐시 속성도 여기서 갈린다. 일관성 있는 장치면 IOMMU_CACHE 로 캐시 가능하게
 * 매핑하고, 아니면 비캐시로 두어 드라이버가 dma_sync 로 명시적 관리를 하게 한다.
 *
 * 실행 컨텍스트: 매핑 경로. 어디서든.
 *
 * 호출 체인: 모든 매핑 진입점 → [이 함수]
 */
static int dma_info_to_prot(enum dma_data_direction dir, bool coherent,
		     unsigned long attrs)
{
	int prot;	/* [한국어] IOMMU 페이지 권한 비트로 조립할 값 */

	if (attrs & DMA_ATTR_MMIO)	/* [한국어] 메모리가 아니라 장치 레지스터를 매핑하는 경우 (P2PDMA 등) */
		prot = IOMMU_MMIO;	/* [한국어] 캐시 속성을 device 로 — 투기적 읽기나 병합이 일어나면 MMIO 부작용이 어긋난다 */
	else
		prot = coherent ? IOMMU_CACHE : 0;	/* [한국어] 장치가 캐시 일관성을 가지면 캐시 가능으로 매핑한다. 아니면 비캐시로 두고, 드라이버가 dma_sync 로 명시적 캐시 관리를 해야 한다 */

	if (attrs & DMA_ATTR_PRIVILEGED)	/* [한국어] 특권 접근 요청 (일부 ARM 장치가 두 권한 수준으로 DMA 한다) */
		prot |= IOMMU_PRIV;	/* [한국어] PTE 에 특권 비트를 세운다 */

	switch (dir) {	/* [한국어] DMA 방향을 읽기/쓰기 권한으로 옮긴다 */
	case DMA_BIDIRECTIONAL:	/* [한국어] 양방향 */
		return prot | IOMMU_READ | IOMMU_WRITE;	/* [한국어] 둘 다 허용 */
	case DMA_TO_DEVICE:	/* [한국어] 장치가 메모리를 읽어 간다 */
		return prot | IOMMU_READ;	/* [한국어] 읽기만 — 쓰기를 막는 것이 격리의 실질이다. 장치가 이 버퍼를 덮어쓰려 하면 폴트가 난다 */
	case DMA_FROM_DEVICE:	/* [한국어] 장치가 메모리에 써 넣는다 */
		return prot | IOMMU_WRITE;	/* [한국어] 쓰기만 */
	default:	/* [한국어] DMA_NONE 등 권한 없는 방향 */
		return 0;	/* [한국어] DMA_NONE 등 — 권한 없는 매핑은 만들 수 없으므로 호출자가 실패로 다룬다 */
	}
}

/*
 * [한국어]
 * iommu_dma_alloc_iova - 이 도메인에서 IOVA 구간을 확보한다
 *
 * @domain:    대상 도메인
 * @size:      필요한 길이 (이미 페이지 정렬됨)
 * @dma_limit: 장치가 낼 수 있는 최대 주소
 * @dev:       요청하는 장치
 * @return:    확보한 DMA 주소, 실패하면 0
 *
 * 상한을 세 겹으로 좁힌다 — 장치의 DMA 마스크, 버스가 통과시키는 한계, 그리고
 * IOMMU 창의 끝이다. 그 안에서 iova.c 에 실제 배정을 맡긴다.
 *
 * 32비트 우선 시도가 이 함수의 특징적인 부분이다. PCIe 에서는 SAC/DAC 구분이
 * 의미를 잃었지만 상위 주소 비트를 잘못 다루는 하드웨어가 남아 있어, 64비트를
 * 쓸 수 있는 장치라도 일단 4GB 아래에서 찾아본다. 그 영역이 고갈되면 플래그를
 * 내려 이후로는 시도조차 하지 않고, 그 전환을 dev_notice 로 남겨 장치가 그때부터
 * 오작동하면 원인을 짚을 수 있게 한다.
 *
 * MSI 축소판 쿠키는 할당자가 없어 예약된 창에서 순서대로 떼어 쓴다.
 *
 * 실행 컨텍스트: 매핑 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: __iommu_dma_map, iommu_dma_map_sg, dma_iova_try_alloc → [이 함수]
 *            → alloc_iova_fast
 */
static dma_addr_t iommu_dma_alloc_iova(struct iommu_domain *domain,
		size_t size, u64 dma_limit, struct device *dev)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] 이 도메인의 DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	unsigned long shift, iova_len, iova;	/* [한국어] 입도 로그값, 페이지 단위 길이, 확보한 pfn */

	if (domain->cookie_type == IOMMU_COOKIE_DMA_MSI) {	/* [한국어] MSI 전용 축소판 쿠키에는 할당자가 없다 */
		domain->msi_cookie->msi_iova += size;	/* [한국어] 예약된 창에서 순서대로 떼어 쓴다 */
		return domain->msi_cookie->msi_iova - size;	/* [한국어] 방금 뗀 구간의 시작 */
	}

	shift = iova_shift(iovad);	/* [한국어] 바이트 ↔ pfn 변환에 쓸 시프트 */
	iova_len = size >> shift;	/* [한국어] 요청 길이를 페이지 수로 */

	dma_limit = min_not_zero(dma_limit, dev->bus_dma_limit);	/* [한국어] 장치의 DMA 마스크와 버스가 통과시키는 상한 중 작은 쪽. 0 은 '제한 없음'이므로 min_not_zero 를 쓴다 */

	if (domain->geometry.force_aperture)	/* [한국어] IOMMU 창 밖은 절대 쓸 수 없는 하드웨어면 */
		dma_limit = min(dma_limit, (u64)domain->geometry.aperture_end);	/* [한국어] 창 끝으로 한 번 더 조인다 */

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
	if (dma_limit > DMA_BIT_MASK(32) && dev->iommu->pci_32bit_workaround) {	/* [한국어] 64비트를 쓸 수 있는 장치라도 일단 32비트 영역부터 시도한다. PCIe 에서는 SAC/DAC 구분이 의미를 잃었지만, 상위 주소 비트를 제대로 배선하지 않았거나 마스크를 잘못 신고하는 하드웨어·펌웨어가 여전히 남아 있기 때문이다 (위 영어 주석) */
		iova = alloc_iova_fast(iovad, iova_len,	/* [한국어] 32비트 영역에서만 시도 */
				       DMA_BIT_MASK(32) >> shift, false);	/* [한국어] 상한을 4GB 로. 마지막 false 는 '실패해도 캐시를 비우지 말라'는 뜻 — 아래 64비트 시도가 남아 있으므로 여기서 캐시를 헐 이유가 없다 */
		if (iova)	/* [한국어] 32비트 영역에서 확보했다 */
			goto done;	/* [한국어] 가장 안전한 결과 */

		dev->iommu->pci_32bit_workaround = false;	/* [한국어] 32비트 영역이 고갈되었다. 이후로는 이 장치에 대해 시도조차 하지 않는다 — 매번 실패하는 할당을 반복하면 그 자체가 비용이다 */
		dev_notice(dev, "Using %d-bit DMA addresses\n", bits_per(dma_limit));	/* [한국어] 이 시점 이후 장치가 오작동하면 원인이 상위 주소 비트에 있음을 알려 주는 단서다 (위 영어 주석) */
	}

	iova = alloc_iova_fast(iovad, iova_len, dma_limit >> shift, true);	/* [한국어] 전체 영역에서 시도. 마지막 true 는 '실패하면 캐시를 비우고 재시도하라' — 여기가 마지막 기회다 */
done:	/* [한국어] 32비트 성공 경로가 합류 */
	return (dma_addr_t)iova << shift;	/* [한국어] pfn 을 실제 DMA 주소로 되돌린다. 실패했으면 iova 가 0 이라 0 이 나가고, 그것이 곧 DMA_MAPPING_ERROR 다 */
}

/*
 * [한국어]
 * iommu_dma_free_iova - IOVA 구간을 돌려주거나 무효화 대기 큐에 넣는다
 *
 * @domain: 대상 도메인
 * @iova:   반납할 구간의 시작
 * @size:   길이
 * @gather: 이번 해제의 무효화 수집기. NULL 이면 무효화할 것이 없다는 뜻.
 *
 * strict/lazy 정책이 실제로 갈리는 세 갈래다. gather->queued 가 참이면 큐에 넣어
 * 무효화가 끝난 뒤에 반납하고, 아니면 즉시 반납한다 — 즉시 반납이 안전한 이유는
 * 호출자가 이미 iommu_iotlb_sync 로 무효화를 끝냈기 때문이다.
 *
 * gather 가 NULL 인 경우는 매핑 실패 되감기다. 매핑이 없었으니 IOTLB 에 남을 것도
 * 없어 곧바로 돌려줘도 된다.
 *
 * 실행 컨텍스트: 해제 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: __iommu_dma_unmap, 매핑 실패 경로 → [이 함수]
 *            → queue_iova 또는 free_iova_fast
 */
static void iommu_dma_free_iova(struct iommu_domain *domain, dma_addr_t iova,
				size_t size, struct iommu_iotlb_gather *gather)
{
	struct iova_domain *iovad = &domain->iova_cookie->iovad;	/* [한국어] IOVA 공간 */

	/* The MSI case is only ever cleaning up its most recent allocation */
	if (domain->cookie_type == IOMMU_COOKIE_DMA_MSI)	/* [한국어] MSI 축소판 쿠키 */
		domain->msi_cookie->msi_iova -= size;	/* [한국어] 가장 최근 할당만 되돌린다 — 스택처럼 쓰는 단순 구조다 (위 영어 주석) */
	else if (gather && gather->queued)	/* [한국어] 이 도메인이 지연 무효화를 쓰고, 이번 해제가 그 경로로 왔다 */
		queue_iova(domain->iova_cookie, iova_pfn(iovad, iova),	/* [한국어] IOVA 를 곧바로 돌려주지 않고 큐에 넣는다 */
				size >> iova_shift(iovad),	/* [한국어] 페이지 수 */
				&gather->freelist);	/* [한국어] 비워진 페이지 테이블 페이지도 함께 미룬다 */
	else
		free_iova_fast(iovad, iova_pfn(iovad, iova),	/* [한국어] 즉시 무효화(strict) 경로 — 이미 IOTLB 를 비웠으므로 바로 반납해도 안전하다 */
				size >> iova_shift(iovad));	/* [한국어] 페이지 수 */
}

/*
 * [한국어]
 * __iommu_dma_unmap - IOVA 매핑을 지우고 주소를 회수한다 (공통 해제 경로)
 *
 * @dev:      대상 장치
 * @dma_addr: 해제할 DMA 주소
 * @size:     길이
 *
 * 모든 해제 진입점이 결국 이곳으로 모인다. 하는 일은 네 단계다 — 범위를 페이지
 * 경계로 되돌리고, PTE 를 지우고, 정책에 따라 무효화하고, IOVA 를 회수한다.
 *
 * gather.queued 를 fq_domain 유무로 세우는 한 줄이 정책의 전달 통로다. 그 값이
 * 참이면 벤더 드라이버가 iommu_unmap_fast 안에서 즉시 무효화를 생략하고, 이
 * 함수도 iommu_iotlb_sync 를 건너뛴다.
 *
 * unmapped != size 경고는 실전에서 자주 보게 되는 것이다 — 드라이버가 dma_unmap 에
 * 매핑 때와 다른 길이를 넘긴 전형적인 버그를 잡아낸다.
 *
 * 실행 컨텍스트: 해제 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: iommu_dma_unmap_phys, iommu_dma_unmap_sg, iommu_dma_free → [이 함수]
 *            → iommu_unmap_fast, iommu_dma_free_iova
 */
static void __iommu_dma_unmap(struct device *dev, dma_addr_t dma_addr,
		size_t size)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 이 장치의 기본 도메인 (검사 없는 핫패스용 접근자) */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	size_t iova_off = iova_offset(iovad, dma_addr);	/* [한국어] IOVA 가 페이지 경계에서 얼마나 들어가 있는지 */
	struct iommu_iotlb_gather iotlb_gather;	/* [한국어] 무효화 범위 수집기 (스택) */
	size_t unmapped;	/* [한국어] 실제로 해제된 바이트 */

	dma_addr -= iova_off;	/* [한국어] 페이지 경계로 내린다 — 매핑도 같은 방식으로 확장해 만들었다 */
	size = iova_align(iovad, size + iova_off);	/* [한국어] 앞쪽 오프셋을 더한 뒤 페이지 단위로 올림 */
	iommu_iotlb_gather_init(&iotlb_gather);	/* [한국어] 빈 수집기로 시작 */
	iotlb_gather.queued = READ_ONCE(cookie->fq_domain);	/* [한국어] 이 한 줄이 strict/lazy 를 가른다. fq_domain 이 있으면 queued 가 참이 되고, iommu.c 의 드라이버들은 그것을 보고 즉시 무효화를 생략한다 */

	unmapped = iommu_unmap_fast(domain, dma_addr, size, &iotlb_gather);	/* [한국어] PTE 를 지우고 무효화 범위만 모은다 */
	WARN_ON(unmapped != size);	/* [한국어] 요청한 만큼 지우지 못했다 = 매핑과 해제의 크기가 어긋났다는 뜻. 드라이버가 dma_unmap 에 잘못된 길이를 넘긴 전형적인 버그다 */

	if (!iotlb_gather.queued)	/* [한국어] 즉시 무효화 정책이면 */
		iommu_iotlb_sync(domain, &iotlb_gather);	/* [한국어] 여기서 IOTLB 를 비우고 완료를 기다린다. 이 줄이 dma_unmap 지연의 대부분이며, flush queue 가 없애려는 것이 바로 이 대기다 */
	iommu_dma_free_iova(domain, dma_addr, size, &iotlb_gather);	/* [한국어] IOVA 를 반납하거나(strict) 큐에 넣는다(lazy) */
}

/*
 * [한국어]
 * __iommu_dma_map - 물리 연속 구간 하나를 IOVA 에 매핑한다 (공통 매핑 경로)
 *
 * @dev:      대상 장치
 * @phys:     매핑할 물리 시작 주소
 * @size:     길이
 * @prot:     IOMMU 페이지 권한
 * @dma_mask: 이 매핑에 쓸 주소 상한
 * @return:   장치가 쓸 DMA 주소, 실패하면 DMA_MAPPING_ERROR
 *
 * 페이지 정렬 처리가 이 함수의 핵심이다. 요청한 물리 주소가 IOMMU 페이지 중간에서
 * 시작하면 그 앞부분까지 포함해 매핑하고, 반환 주소에 오프셋을 다시 더해 준다.
 * 결과적으로 장치에는 요청보다 넓은 범위가 열리며, 그 페이지에 다른 데이터가 있다면
 * 함께 노출된다 — 신뢰할 수 없는 장치를 바운스 버퍼로 우회시키는 이유가 이것이다.
 *
 * 순서는 항상 IOVA 확보 → 매핑이며, 매핑이 실패하면 확보한 주소를 즉시 되돌린다.
 *
 * 실행 컨텍스트: 매핑 경로. 인터럽트 문맥 가능 (GFP_ATOMIC).
 *
 * 호출 체인: iommu_dma_map_phys, iommu_dma_alloc → [이 함수]
 *            → iommu_dma_alloc_iova, iommu_map
 */
static dma_addr_t __iommu_dma_map(struct device *dev, phys_addr_t phys,
		size_t size, int prot, u64 dma_mask)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 이 장치의 기본 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	size_t iova_off = iova_offset(iovad, phys);	/* [한국어] 물리 주소가 IOMMU 페이지 안에서 얼마나 들어가 있는지. 이 오프셋만큼 앞을 포함해 매핑하고, 반환 주소에는 다시 더해 준다 */
	dma_addr_t iova;	/* [한국어] 확보한 IOVA */

	if (static_branch_unlikely(&iommu_deferred_attach_enabled) &&	/* [한국어] 지연 부착이 필요한 하드웨어가 하나라도 있는 시스템에서만 이 분기가 존재한다 */
	    iommu_deferred_attach(dev, domain))	/* [한국어] 첫 DMA 인 지금 실제로 도메인을 하드웨어에 건다 */
		return DMA_MAPPING_ERROR;	/* [한국어] 부착 실패 — 매핑할 수 없다 */

	/* If anyone ever wants this we'd need support in the IOVA allocator */
	if (dev_WARN_ONCE(dev, dma_get_min_align_mask(dev) > iova_mask(iovad),	/* [한국어] 장치가 IOMMU 페이지보다 큰 정렬을 요구한다 (위 영어 주석 — IOVA 할당자가 그것을 표현하지 못한다) */
	    "Unsupported alignment constraint\n"))	/* [한국어] 드라이버의 요구를 만족시킬 수 없다 */
		return DMA_MAPPING_ERROR;	/* [한국어] 매핑 거절 */

	size = iova_align(iovad, size + iova_off);	/* [한국어] 앞쪽 오프셋을 포함해 페이지 단위로 올림. IOMMU 는 페이지 단위로만 매핑하므로 요청보다 넓은 범위가 장치에 열리며, 그래서 신뢰할 수 없는 장치는 이 경로가 아니라 swiotlb 로 보낸다 */

	iova = iommu_dma_alloc_iova(domain, size, dma_mask, dev);	/* [한국어] 주소를 먼저 확보한다 */
	if (!iova)	/* [한국어] IOVA 고갈 */
		return DMA_MAPPING_ERROR;	/* [한국어] 드라이버는 재시도하거나 요청을 쪼갠다 */

	if (iommu_map(domain, iova, phys - iova_off, size, prot, GFP_ATOMIC)) {	/* [한국어] 확보한 IOVA 에 물리 페이지를 매핑한다. 물리 주소도 페이지 경계로 내려 시작을 맞춘다. GFP_ATOMIC 인 것은 이 경로가 인터럽트 문맥에서도 불리기 때문이다 */
		iommu_dma_free_iova(domain, iova, size, NULL);	/* [한국어] 매핑 실패 — 확보한 주소를 즉시 되돌린다. gather 가 NULL 이라 큐를 거치지 않고 바로 반납하는데, 매핑이 없었으니 무효화할 것도 없기 때문이다 */
		return DMA_MAPPING_ERROR;	/* [한국어] 매핑 실패 */
	}
	return iova + iova_off;	/* [한국어] 페이지 경계로 내렸던 오프셋을 다시 더해 돌려준다 — 장치가 실제로 쓸 주소다 */
}

/*
 * [한국어]
 * __iommu_dma_free_pages - 낱장 페이지 배열을 반납한다
 *
 * @pages: 페이지 포인터 배열
 * @count: 개수
 *
 * 고차 블록으로 잡았더라도 __iommu_dma_alloc_pages 가 split_page 로 쪼개 두었기
 * 때문에 전부 단일 페이지로 반납할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_alloc_remap, __iommu_dma_free 등 → [이 함수]
 */
static void __iommu_dma_free_pages(struct page **pages, int count)
{
	while (count--)	/* [한국어] 뒤에서부터 하나씩 */
		__free_page(pages[count]);	/* [한국어] 페이지 반납. 고차 할당은 split_page 로 이미 낱장으로 쪼개 두었으므로 전부 단일 페이지로 다룰 수 있다 */
	kvfree(pages);	/* [한국어] 포인터 배열 자체 (kvzalloc 으로 잡았다) */
}

/*
 * [한국어]
 * __iommu_dma_alloc_pages - 흩어져도 좋은 페이지들을 모은다
 *
 * @dev:        대상 장치 (NUMA 노드 선택에 쓰인다)
 * @count:      필요한 페이지 수
 * @order_mask: 시도해 볼 할당 차수들의 비트마스크
 * @gfp:        할당 플래그
 * @return:     페이지 포인터 배열, 실패하면 NULL
 *
 * 큰 차수부터 시도해 내려가는 것이 전략이다. 고차 블록을 얻으면 그만큼 IOMMU 가
 * 큰 페이지로 매핑할 수 있어 PTE 와 IOTLB 소모가 줄지만, 실패해도 작은 페이지로
 * 채우면 그만이다. 그래서 물러설 여지가 있는 동안은 __GFP_NORETRY 를 붙여 메모리
 * 압박을 만들지 않는다 (위 영어 주석).
 *
 * 고차 블록을 얻으면 split_page 로 낱장으로 쪼갠다. 배열에 낱장으로 담아야 각
 * 페이지를 독립적으로 참조하고 해제할 수 있기 때문이다.
 *
 * HIGHMEM 을 허용하는 것도 이 경로의 특징이다 — IOMMU 가 어떤 물리 페이지든
 * 매핑할 수 있으므로 저역 메모리를 아낄 이유가 없다.
 *
 * 실행 컨텍스트: coherent 할당 경로. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: __iommu_dma_alloc_noncontiguous → [이 함수]
 */
static struct page **__iommu_dma_alloc_pages(struct device *dev,
		unsigned int count, unsigned long order_mask, gfp_t gfp)
{
	struct page **pages;	/* [한국어] 확보한 페이지들의 포인터 배열 */
	unsigned int i = 0, nid = dev_to_node(dev);	/* [한국어] 채운 개수와, 장치가 붙은 NUMA 노드 — 장치와 가까운 메모리에서 잡아야 DMA 지연이 낮다 */

	order_mask &= GENMASK(MAX_PAGE_ORDER, 0);	/* [한국어] 페이지 할당자가 다룰 수 있는 차수까지만 남긴다. IOMMU 가 1GB 페이지를 지원해도 버디 할당자가 그 크기를 주지는 않는다 */
	if (!order_mask)	/* [한국어] 쓸 수 있는 차수가 하나도 없다 */
		return NULL;	/* [한국어] 할당 불가 */

	pages = kvzalloc_objs(*pages, count);	/* [한국어] 포인터 배열. 개수가 많아질 수 있어 vmalloc 으로 넘어갈 수 있는 kv 판을 쓴다 */
	if (!pages)	/* [한국어] 배열 할당 실패 */
		return NULL;	/* [한국어] 포기 */

	/* IOMMU can map any pages, so himem can also be used here */
	gfp |= __GFP_NOWARN | __GFP_HIGHMEM;	/* [한국어] IOMMU 가 어떤 물리 페이지든 매핑할 수 있으므로 HIGHMEM 도 쓸 수 있다 (위 영어 주석). NOWARN 은 고차 할당 실패가 정상적인 후퇴이기 때문이다 */

	while (count) {	/* [한국어] 필요한 페이지 수를 다 채울 때까지 */
		struct page *page = NULL;	/* [한국어] 이번 회차에 얻은 블록 */
		unsigned int order_size;	/* [한국어] 그 블록의 페이지 수 */

		/*
		 * Higher-order allocations are a convenience rather
		 * than a necessity, hence using __GFP_NORETRY until
		 * falling back to minimum-order allocations.
		 */
		for (order_mask &= GENMASK(__fls(count), 0);	/* [한국어] 남은 개수보다 큰 차수는 후보에서 뺀다 */
		     order_mask; order_mask &= ~order_size) {	/* [한국어] 실패하면 그 차수를 후보에서 지우고 한 단계 낮춰 재시도 */
			unsigned int order = __fls(order_mask);	/* [한국어] 남은 후보 중 가장 큰 차수부터 */
			gfp_t alloc_flags = gfp;	/* [한국어] 이 시도의 플래그 */

			order_size = 1U << order;	/* [한국어] 그 차수의 페이지 수 */
			if (order_mask > order_size)	/* [한국어] 아직 더 낮은 차수로 물러설 여지가 있다면 */
				alloc_flags |= __GFP_NORETRY;	/* [한국어] 회수를 강하게 하지 않는다. 고차 할당은 편의일 뿐 필수가 아니므로, 메모리 압박을 만들어 가며 붙잡을 이유가 없다 (위 영어 주석) */
			page = alloc_pages_node(nid, alloc_flags, order);	/* [한국어] 장치와 같은 NUMA 노드에서 시도 */
			if (!page)	/* [한국어] 실패 */
				continue;	/* [한국어] 한 단계 낮은 차수로 */
			if (order)	/* [한국어] 고차 블록을 얻었다면 */
				split_page(page, order);	/* [한국어] 낱장으로 쪼개 각 페이지가 독립적으로 참조·해제될 수 있게 한다. 배열에 낱장으로 담기 때문이다 */
			break;	/* [한국어] 확보 성공 */
		}
		if (!page) {	/* [한국어] 모든 차수에서 실패 */
			__iommu_dma_free_pages(pages, i);	/* [한국어] 지금까지 모은 것을 전부 반납 */
			return NULL;	/* [한국어] 할당 실패 */
		}
		count -= order_size;	/* [한국어] 남은 개수 갱신 */
		while (order_size--)	/* [한국어] 얻은 블록의 각 페이지를 */
			pages[i++] = page++;	/* [한국어] 배열에 차례로 담는다 */
	}
	return pages;	/* [한국어] 요청한 개수만큼의 페이지 배열 */
}

/*
 * If size is less than PAGE_SIZE, then a full CPU page will be allocated,
 * but an IOMMU which supports smaller pages might not map the whole thing.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * __iommu_dma_alloc_noncontiguous - 흩어진 페이지를 하나의 IOVA 창으로 접는다
 *
 * @dev:    대상 장치
 * @size:   요청 크기
 * @sgt:    결과 scatterlist (호출자 소유)
 * @gfp:    할당 플래그
 * @attrs:  DMA 속성
 * @return: 페이지 배열, 실패하면 NULL
 *
 * IOMMU 가 있어서 가능해지는 일의 전형이다. coherent 할당은 원래 물리적으로 연속인
 * 메모리를 요구하지만, IOMMU 아래에서는 아무 데서나 페이지를 모아 하나의 연속
 * IOVA 창에 접어 넣으면 장치가 보기에 연속이 된다. 큰 버퍼 할당이 단편화 때문에
 * 실패하는 일이 사라진다.
 *
 * 중간의 gfp 플래그 정리가 눈에 잘 띄지 않지만 중요하다. 존 지정과 정책 플래그는
 * 데이터 페이지 할당에만 의미가 있고, 그 뒤의 sg 테이블이나 페이지 테이블 할당에
 * 그대로 적용하면 오히려 해가 된다 (위 영어 주석).
 *
 * 비일관 장치용이면 매핑 전에 arch_dma_prep_coherent 로 캐시를 비운다 — 이 페이지들은
 * 앞으로 비캐시로 접근되므로 캐시에 남은 옛 내용이 나중에 write-back 되면 안 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: iommu_dma_alloc_remap, iommu_dma_alloc_noncontiguous → [이 함수]
 *            → __iommu_dma_alloc_pages, iommu_dma_alloc_iova, iommu_map_sg
 */
static struct page **__iommu_dma_alloc_noncontiguous(struct device *dev,
		size_t size, struct sg_table *sgt, gfp_t gfp, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 이 장치의 기본 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	bool coherent = dev_is_dma_coherent(dev);	/* [한국어] 장치가 캐시 일관성을 갖는가 */
	int ioprot = dma_info_to_prot(DMA_BIDIRECTIONAL, coherent, attrs);	/* [한국어] coherent 할당은 방향을 모르므로 읽기·쓰기 모두 허용한다 */
	unsigned int count, min_size, alloc_sizes = domain->pgsize_bitmap;	/* [한국어] 페이지 수, 최소 단위, 그리고 시도해 볼 할당 크기 후보들 */
	struct page **pages;	/* [한국어] 확보한 페이지 배열 */
	dma_addr_t iova;	/* [한국어] 확보한 IOVA 창 */
	ssize_t ret;	/* [한국어] iommu_map_sg 결과 (매핑된 바이트 수 또는 음수) */

	if (static_branch_unlikely(&iommu_deferred_attach_enabled) &&	/* [한국어] 지연 부착이 필요한 시스템에서만 */
	    iommu_deferred_attach(dev, domain))	/* [한국어] 여기서 실제로 도메인을 건다 */
		return NULL;	/* [한국어] 부착 실패 */

	min_size = alloc_sizes & -alloc_sizes;	/* [한국어] IOMMU 최소 페이지 크기 (최하위 비트만 남기는 관용구) */
	if (min_size < PAGE_SIZE) {	/* [한국어] IOMMU 페이지가 CPU 페이지보다 작으면 */
		min_size = PAGE_SIZE;	/* [한국어] 할당은 어차피 CPU 페이지 단위다 */
		alloc_sizes |= PAGE_SIZE;	/* [한국어] CPU 페이지 크기를 후보에 넣는다 */
	} else {
		size = ALIGN(size, min_size);	/* [한국어] IOMMU 페이지가 더 크면 요청을 그 배수로 올린다 — 그보다 잘게 매핑할 수 없다 */
	}
	if (attrs & DMA_ATTR_ALLOC_SINGLE_PAGES)	/* [한국어] 호출자가 낱장만 원한다고 명시했다 (큰 블록을 붙잡아 단편화를 만들지 말라는 뜻) */
		alloc_sizes = min_size;	/* [한국어] 최소 크기만 후보로 */

	count = PAGE_ALIGN(size) >> PAGE_SHIFT;	/* [한국어] 필요한 CPU 페이지 수 */
	pages = __iommu_dma_alloc_pages(dev, count, alloc_sizes >> PAGE_SHIFT,	/* [한국어] 물리적으로 흩어져도 좋은 페이지들을 모은다 */
					gfp);	/* [한국어] 호출자의 할당 플래그 그대로 */
	if (!pages)	/* [한국어] 페이지 확보 실패 */
		return NULL;	/* [한국어] 할당 실패 */

	size = iova_align(iovad, size);	/* [한국어] IOVA 창은 IOMMU 페이지 단위로 */
	iova = iommu_dma_alloc_iova(domain, size, dev->coherent_dma_mask, dev);	/* [한국어] 흩어진 페이지들을 담을 하나의 연속 IOVA 창 */
	if (!iova)	/* [한국어] IOVA 고갈 */
		goto out_free_pages;	/* [한국어] 페이지부터 되돌린다 */

	/*
	 * Remove the zone/policy flags from the GFP - these are applied to the
	 * __iommu_dma_alloc_pages() but are not used for the supporting
	 * internal allocations that follow.
	 */
	gfp &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM | __GFP_COMP);	/* [한국어] 존 지정과 정책 플래그를 뗀다. 그것들은 위의 데이터 페이지 할당에만 의미가 있고, 아래의 보조 자료구조(sg 테이블, 페이지 테이블)에 적용하면 오히려 해가 된다 (위 영어 주석) */

	if (sg_alloc_table_from_pages(sgt, pages, count, 0, size, gfp))	/* [한국어] 페이지 배열을 scatterlist 로 엮는다 — 물리적으로 인접한 것들은 자동으로 한 세그먼트로 합쳐진다 */
		goto out_free_iova;	/* [한국어] 테이블 생성 실패 */

	if (!(ioprot & IOMMU_CACHE)) {	/* [한국어] 비일관 장치용 매핑이면 */
		struct scatterlist *sg;	/* [한국어] 세그먼트 순회 커서 */
		int i;	/* [한국어] 인덱스 */

		for_each_sg(sgt->sgl, sg, sgt->orig_nents, i)	/* [한국어] 각 세그먼트에 대해 */
			arch_dma_prep_coherent(sg_page(sg), sg->length);	/* [한국어] 캐시를 비워 둔다. 이 페이지들은 앞으로 비캐시로 매핑되므로, CPU 캐시에 남은 옛 내용이 나중에 write-back 되면 장치가 쓴 데이터를 덮어쓴다 */
	}

	ret = iommu_map_sg(domain, iova, sgt->sgl, sgt->orig_nents, ioprot,	/* [한국어] 흩어진 페이지들을 하나의 연속 IOVA 창에 접어 넣는다 — 이것이 IOMMU 가 없으면 불가능한 일이고, coherent 할당이 물리 연속을 요구하지 않아도 되는 이유다 */
			   gfp);	/* [한국어] 페이지 테이블 할당 플래그 */
	if (ret < 0 || ret < size)	/* [한국어] 실패했거나 일부만 매핑되었다 */
		goto out_free_sg;	/* [한국어] 전부 되돌린다 */

	sgt->sgl->dma_address = iova;	/* [한국어] 호출자는 첫 세그먼트에서 DMA 주소를 읽는다 — 창 전체가 연속이므로 하나면 충분하다 */
	sgt->sgl->dma_length = size;	/* [한국어] 그 창의 길이 */
	return pages;	/* [한국어] 페이지 배열은 호출자가 vmap 하거나 나중에 해제할 때 쓴다 */

out_free_sg:	/* [한국어] 매핑에 실패한 지점 */
	sg_free_table(sgt);	/* [한국어] 테이블 해제 */
out_free_iova:	/* [한국어] 테이블 생성에 실패한 지점이 합류 */
	iommu_dma_free_iova(domain, iova, size, NULL);	/* [한국어] IOVA 반납 (매핑이 없으므로 무효화도 불필요) */
out_free_pages:	/* [한국어] IOVA 확보에 실패한 지점이 합류 */
	__iommu_dma_free_pages(pages, count);	/* [한국어] 페이지 반납 */
	return NULL;	/* [한국어] 할당 실패 */
}

/*
 * [한국어]
 * iommu_dma_alloc_remap - 흩어진 페이지를 양쪽 모두에서 연속으로 보이게 만든다
 *
 * @dev:        대상 장치
 * @size:       요청 크기
 * @dma_handle: 장치가 쓸 주소를 여기에 채운다
 * @gfp:        할당 플래그
 * @attrs:      DMA 속성
 * @return:     CPU 가 쓸 가상 주소, 실패하면 NULL
 *
 * 같은 흩어진 페이지 묶음을 두 번 접는다 — 장치 쪽은 IOMMU 페이지 테이블이,
 * CPU 쪽은 커널 페이지 테이블(vmap)이 연속으로 만들어 준다. 그래서 드라이버는
 * 물리 연속을 전혀 의식하지 않고 큰 coherent 버퍼를 쓸 수 있다.
 *
 * 비일관 장치면 dma_pgprot 이 CPU 쪽 매핑을 비캐시로 만든다. 그래야 CPU 와 장치가
 * 같은 데이터를 다르게 보는 일이 없다.
 *
 * 실행 컨텍스트: 잠들 수 있는 문맥에서만 (vmap 때문). 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_alloc → [이 함수] → __iommu_dma_alloc_noncontiguous
 */
static void *iommu_dma_alloc_remap(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
	struct page **pages;	/* [한국어] 확보한 페이지 배열 */
	struct sg_table sgt;	/* [한국어] 임시 scatterlist — vmap 이 끝나면 필요 없다 */
	void *vaddr;	/* [한국어] 커널 가상 주소 */
	pgprot_t prot = dma_pgprot(dev, PAGE_KERNEL, attrs);	/* [한국어] CPU 쪽 매핑 속성. 비일관 장치면 여기서 비캐시(nocache)로 만들어, CPU 와 장치가 같은 데이터를 다르게 보지 않게 한다 */

	pages = __iommu_dma_alloc_noncontiguous(dev, size, &sgt, gfp, attrs);	/* [한국어] 페이지를 모아 하나의 IOVA 창에 접는다 */
	if (!pages)	/* [한국어] 실패 */
		return NULL;	/* [한국어] 할당 실패 */
	*dma_handle = sgt.sgl->dma_address;	/* [한국어] 장치가 쓸 주소 */
	sg_free_table(&sgt);	/* [한국어] scatterlist 는 여기까지만 필요하다 — 페이지 배열이 남아 있으므로 */
	vaddr = dma_common_pages_remap(pages, size, prot,	/* [한국어] 흩어진 페이지들을 CPU 쪽에서도 연속 가상 주소로 만든다. 장치 쪽은 IOMMU 가, CPU 쪽은 페이지 테이블이 같은 일을 하는 셈이다 */
			__builtin_return_address(0));	/* [한국어] vmalloc 영역 진단에 남길 호출자 주소 */
	if (!vaddr)	/* [한국어] 가상 매핑 실패 */
		goto out_unmap;	/* [한국어] 되감기 */
	return vaddr;	/* [한국어] CPU 는 이 주소로, 장치는 dma_handle 로 같은 메모리를 본다 */

out_unmap:	/* [한국어] vmap 실패 경로 */
	__iommu_dma_unmap(dev, *dma_handle, size);	/* [한국어] IOVA 매핑 해제 */
	__iommu_dma_free_pages(pages, PAGE_ALIGN(size) >> PAGE_SHIFT);	/* [한국어] 페이지 반납 */
	return NULL;	/* [한국어] 할당 실패 */
}

/*
 * This is the actual return value from the iommu_dma_alloc_noncontiguous.
 *
 * The users of the DMA API should only care about the sg_table, but to make
 * the DMA-API internal vmaping and freeing easier we stash away the page
 * array as well (except for the fallback case).  This can go away any time,
 * e.g. when a vmap-variant that takes a scatterlist comes along.
 */
/*
 * [한국어] (위 영어 주석에 이어) sgt 와 페이지 배열을 함께 들고 있는 그릇.
 *
 * DMA API 사용자에게는 sg_table 만 보이지만, 내부적으로는 vmap 이나 해제에 페이지
 * 배열이 필요하다. sgt 를 구조체의 첫 필드로 두어 container_of 로 되짚을 수 있게
 * 했다. 위 영어 주석대로 임시방편이며, scatterlist 를 받는 vmap 변형이 생기면
 * 사라질 수 있다.
 */
struct dma_sgt_handle {
	struct sg_table sgt;	/* [한국어] 호출자에게 돌려줄 scatterlist */
	struct page **pages;	/* [한국어] DMA API 내부에서 vmap/해제에 쓸 페이지 배열. 사용자는 이것을 몰라도 된다 (위 영어 주석) */
};
/* [한국어] 돌려준 sgt 포인터에서 감싸는 핸들로 되짚는다
 * (매크로 이어짐 표시 \ 뒤에는 주석을 붙일 수 없어 위로 옮겼다.) */
#define sgt_handle(sgt) \
	container_of((sgt), struct dma_sgt_handle, sgt)	/* [한국어] sgt 가 핸들의 첫 필드이므로 성립한다 */

struct sg_table *iommu_dma_alloc_noncontiguous(struct device *dev, size_t size,
	       enum dma_data_direction dir, gfp_t gfp, unsigned long attrs)
{
	struct dma_sgt_handle *sh;	/* [한국어] 만들 핸들 */

	sh = kmalloc_obj(*sh, gfp);	/* [한국어] sgt 와 페이지 배열을 함께 담을 그릇 */
	if (!sh)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 포기 */

	sh->pages = __iommu_dma_alloc_noncontiguous(dev, size, &sh->sgt, gfp, attrs);	/* [한국어] 페이지 확보와 IOVA 매핑을 한 번에 */
	if (!sh->pages) {	/* [한국어] 실패 */
		kfree(sh);	/* [한국어] 그릇도 반납 */
		return NULL;	/* [한국어] 할당 실패 */
	}
	return &sh->sgt;	/* [한국어] 호출자에게는 sgt 만 보인다 */
}

/*
 * [한국어]
 * iommu_dma_free_noncontiguous - 비연속 할당을 해제한다
 *
 * @dev:  대상 장치
 * @size: 할당 크기
 * @sgt:  alloc 이 돌려준 sg_table
 * @dir:  DMA 방향
 *
 * 순서가 정해져 있다. 장치 쪽 매핑을 먼저 지운 뒤에야 페이지를 반납할 수 있다 —
 * 반대로 하면 반납된 페이지에 장치가 계속 DMA 할 수 있는 창이 열린다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인: DMA API → [이 함수]
 */
void iommu_dma_free_noncontiguous(struct device *dev, size_t size,
		struct sg_table *sgt, enum dma_data_direction dir)
{
	struct dma_sgt_handle *sh = sgt_handle(sgt);	/* [한국어] sgt 에서 핸들로 되짚는다 */

	__iommu_dma_unmap(dev, sgt->sgl->dma_address, size);	/* [한국어] IOVA 매핑 해제 */
	__iommu_dma_free_pages(sh->pages, PAGE_ALIGN(size) >> PAGE_SHIFT);	/* [한국어] 페이지 반납 */
	sg_free_table(&sh->sgt);	/* [한국어] scatterlist 해제 */
	kfree(sh);	/* [한국어] 핸들 해제 */
}

/*
 * [한국어]
 * iommu_dma_vmap_noncontiguous - 비연속 할당을 CPU 가상 주소로 매핑한다
 *
 * @dev:    대상 장치
 * @size:   매핑할 크기
 * @sgt:    alloc 이 돌려준 sg_table
 * @return: 커널 가상 주소, 실패하면 NULL
 *
 * 핸들에 보관해 둔 페이지 배열 덕분에 vmap 을 그대로 부를 수 있다. scatterlist 만
 * 가지고는 이 API 를 쓸 수 없어 배열을 따로 들고 있는 것이다 (위 영어 주석).
 *
 * 실행 컨텍스트: 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: DMA API → [이 함수]
 */
void *iommu_dma_vmap_noncontiguous(struct device *dev, size_t size,
		struct sg_table *sgt)
{
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;	/* [한국어] 페이지 수 */

	return vmap(sgt_handle(sgt)->pages, count, VM_MAP, PAGE_KERNEL);	/* [한국어] 보관해 둔 페이지 배열 덕분에 vmap 을 그대로 부를 수 있다 — scatterlist 만으로는 이 API 가 없다 (위 영어 주석) */
}

/*
 * [한국어]
 * iommu_dma_mmap_noncontiguous - 비연속 할당을 사용자 공간에 매핑한다
 *
 * @dev:    대상 장치
 * @vma:    사용자 매핑 영역
 * @size:   할당 크기
 * @sgt:    alloc 이 돌려준 sg_table
 * @return: 0 성공, -ENXIO 면 요청 범위가 할당 범위를 벗어난다
 *
 * 범위 검사가 보안상 필수다. 없으면 사용자가 오프셋을 크게 주어 할당 범위 밖의
 * 커널 페이지를 매핑할 수 있다.
 *
 * 실행 컨텍스트: mmap 시스템 호출. 프로세스 문맥.
 *
 * 호출 체인: DMA API → [이 함수]
 */
int iommu_dma_mmap_noncontiguous(struct device *dev, struct vm_area_struct *vma,
		size_t size, struct sg_table *sgt)
{
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;	/* [한국어] 페이지 수 */

	if (vma->vm_pgoff >= count || vma_pages(vma) > count - vma->vm_pgoff)	/* [한국어] 요청 범위가 할당 범위를 벗어난다 — 이 검사가 없으면 사용자가 이웃 커널 메모리를 매핑할 수 있다 */
		return -ENXIO;	/* [한국어] 요청 범위가 할당 크기를 넘는다 */
	return vm_map_pages(vma, sgt_handle(sgt)->pages, count);	/* [한국어] 사용자 공간에 이 페이지들을 매핑한다 */
}

/*
 * [한국어]
 * iommu_dma_sync_single_for_cpu - 장치가 쓴 내용을 CPU 가 보도록 맞춘다
 *
 * @dev:        대상 장치
 * @dma_handle: 동기화할 DMA 주소
 * @size:       길이
 * @dir:        DMA 방향
 *
 * 두 가지 일을 순서대로 한다 — 캐시 무효화와 바운스 버퍼 되복사다. 순서가 중요한데,
 * 캐시를 먼저 비워야 복사가 메모리의 최신 내용을 읽는다.
 *
 * 일관성 있는 장치이고 바운스도 쓰지 않으면 할 일이 없어 곧바로 돌아간다. 이 빠른
 * 탈출이 대부분의 시스템에서 이 함수를 사실상 공짜로 만든다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: 드라이버의 dma_sync_single_for_cpu → [이 함수]
 */
void iommu_dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir)
{
	phys_addr_t phys;	/* [한국어] 동기화할 실제 물리 주소 */

	if (dev_is_dma_coherent(dev) && !dev_use_swiotlb(dev, size, dir))	/* [한국어] 일관성 있는 장치이고 바운스도 쓰지 않으면 */
		return;	/* [한국어] 캐시를 만질 것도, 복사할 것도 없다 */

	phys = iommu_iova_to_phys(iommu_get_dma_domain(dev), dma_handle);	/* [한국어] DMA 주소를 물리 주소로 되돌린다 — 캐시 관리와 바운스 복사는 물리 주소를 다룬다 */
	if (!dev_is_dma_coherent(dev)) {	/* [한국어] 비일관 장치면 */
		arch_sync_dma_for_cpu(phys, size, dir);	/* [한국어] 장치가 쓴 내용을 CPU 가 보도록 캐시를 무효화한다 */
		arch_sync_dma_flush();	/* [한국어] 아키텍처가 요구하면 완료까지 기다린다 */
	}

	swiotlb_sync_single_for_cpu(dev, phys, size, dir);	/* [한국어] 바운스 버퍼를 썼다면 그 내용을 원본 버퍼로 되복사한다. 순서가 중요하다 — 캐시를 먼저 무효화해야 복사가 최신 데이터를 읽는다 */
}

/*
 * [한국어]
 * iommu_dma_sync_single_for_device - CPU 가 쓴 내용을 장치가 보도록 맞춘다
 *
 * @dev:        대상 장치
 * @dma_handle: 동기화할 DMA 주소
 * @size:       길이
 * @dir:        DMA 방향
 *
 * for_cpu 의 거울상이며, 내부 순서가 정확히 반대다 — 바운스 버퍼로 먼저 복사하고
 * 그 다음 캐시를 밀어낸다. 복사가 캐시에 남을 수 있으므로 그 순서여야 장치가
 * 최신 내용을 본다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: 드라이버의 dma_sync_single_for_device → [이 함수]
 */
void iommu_dma_sync_single_for_device(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir)
{
	phys_addr_t phys;	/* [한국어] 동기화할 물리 주소 */

	if (dev_is_dma_coherent(dev) && !dev_use_swiotlb(dev, size, dir))	/* [한국어] 할 일이 없는 경우 */
		return;	/* [한국어] 바로 반환 */

	phys = iommu_iova_to_phys(iommu_get_dma_domain(dev), dma_handle);	/* [한국어] 물리 주소 복원 */
	swiotlb_sync_single_for_device(dev, phys, size, dir);	/* [한국어] 원본 버퍼의 내용을 바운스 버퍼로 복사한다. CPU 방향과 반대 순서다 — 복사를 먼저 하고 캐시를 비워야 장치가 최신 내용을 본다 */

	if (!dev_is_dma_coherent(dev)) {	/* [한국어] 비일관 장치면 */
		arch_sync_dma_for_device(phys, size, dir);	/* [한국어] CPU 캐시를 메모리로 밀어낸다 */
		arch_sync_dma_flush();	/* [한국어] 완료 대기 */
	}
}

/*
 * [한국어]
 * iommu_dma_sync_sg_for_cpu - scatterlist 전체를 CPU 쪽으로 동기화한다
 *
 * @dev:    대상 장치
 * @sgl:    첫 세그먼트
 * @nelems: 세그먼트 수
 * @dir:    DMA 방향
 *
 * 매핑 방식에 따라 두 갈래다. 바운스 경로로 매핑되었으면 세그먼트마다 단일 매핑과
 * 같은 처리를 반복하고, 아니면 캐시 무효화만 하면 된다.
 *
 * 후자에서 arch_sync_dma_flush 를 루프 밖에 두는 것에 주목할 것 — 완료 대기는
 * 세그먼트마다 할 이유가 없다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 드라이버의 dma_sync_sg_for_cpu → [이 함수]
 */
void iommu_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;	/* [한국어] 세그먼트 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	if (sg_dma_is_swiotlb(sgl)) {	/* [한국어] 이 리스트가 바운스 버퍼로 매핑되었다면 */
		for_each_sg(sgl, sg, nelems, i)	/* [한국어] 세그먼트마다 */
			iommu_dma_sync_single_for_cpu(dev, sg_dma_address(sg),	/* [한국어] 단일 매핑과 같은 처리를 반복한다 */
						      sg->length, dir);	/* [한국어] 그 세그먼트의 길이 */
	} else if (!dev_is_dma_coherent(dev)) {	/* [한국어] 바운스는 아니지만 비일관 장치면 */
		for_each_sg(sgl, sg, nelems, i)	/* [한국어] 세그먼트마다 */
			arch_sync_dma_for_cpu(sg_phys(sg), sg->length, dir);	/* [한국어] 캐시만 무효화한다 — 복사는 필요 없다 */
		arch_sync_dma_flush();	/* [한국어] 한 번만 완료 대기 — 세그먼트마다 기다릴 이유가 없다 */
	}
}

/*
 * [한국어]
 * iommu_dma_sync_sg_for_device - scatterlist 전체를 장치 쪽으로 동기화한다
 *
 * @dev:    대상 장치
 * @sgl:    첫 세그먼트
 * @nelems: 세그먼트 수
 * @dir:    DMA 방향
 *
 * for_cpu 의 대칭. 매핑 직전에도 불리는데, 그때는 CPU 가 채워 넣은 요청 데이터를
 * 장치가 볼 수 있게 만드는 역할이다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 드라이버의 dma_sync_sg_for_device, iommu_dma_map_sg → [이 함수]
 */
void iommu_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;	/* [한국어] 세그먼트 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	if (sg_dma_is_swiotlb(sgl)) {	/* [한국어] 바운스 버퍼 매핑 */
		for_each_sg(sgl, sg, nelems, i)	/* [한국어] 세그먼트마다 */
			iommu_dma_sync_single_for_device(dev,	/* [한국어] 단일 매핑과 같은 처리 */
							 sg_dma_address(sg),	/* [한국어] 그 세그먼트의 DMA 주소 */
							 sg->length, dir);	/* [한국어] 길이 */
	} else if (!dev_is_dma_coherent(dev)) {	/* [한국어] 비일관 장치 */
		for_each_sg(sgl, sg, nelems, i)	/* [한국어] 세그먼트마다 */
			arch_sync_dma_for_device(sg_phys(sg), sg->length, dir);	/* [한국어] 캐시를 밀어낸다 */
		arch_sync_dma_flush();	/* [한국어] 한 번만 완료 대기 */
	}
}

/*
 * [한국어]
 * iommu_dma_map_swiotlb - 버퍼를 바운스 버퍼로 복사한다
 *
 * @dev:    대상 장치
 * @phys:   원본 물리 주소
 * @size:   길이
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 * @return: 바운스 버퍼의 물리 주소, 실패하면 DMA_MAPPING_ERROR
 *
 * IOMMU 는 페이지 단위로만 매핑하므로, 페이지 중간에서 시작하거나 끝나는 버퍼를
 * 그대로 매핑하면 같은 페이지의 다른 데이터까지 장치에 열린다. 전용 버퍼로 옮겨
 * 그 페이지를 통째로 이 전송의 것으로 만드는 것이 이 함수다.
 *
 * 앞뒤 패딩을 0 으로 지우는 부분이 요점이다. swiotlb 는 요청 범위만 채워 주므로,
 * 같은 페이지의 나머지에는 이전 사용자의 커널 데이터가 남아 있다. 신뢰할 수 없는
 * 장치는 그것까지 읽을 수 있으므로 반드시 지워야 한다 (위 영어 주석).
 *
 * 실행 컨텍스트: 매핑 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: iommu_dma_map_phys, iommu_dma_iova_bounce_and_link → [이 함수]
 */
static phys_addr_t iommu_dma_map_swiotlb(struct device *dev, phys_addr_t phys,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 이 장치의 기본 도메인 */
	struct iova_domain *iovad = &domain->iova_cookie->iovad;	/* [한국어] IOVA 입도를 알아야 패딩을 지울 범위를 정할 수 있다 */

	if (!is_swiotlb_active(dev)) {	/* [한국어] 바운스 버퍼 풀이 없다 (설정으로 껐거나 고갈) */
		dev_warn_once(dev, "DMA bounce buffers are inactive, unable to map unaligned transaction.\n");	/* [한국어] 정렬이 맞지 않는 요청을 안전하게 처리할 방법이 없다 */
		return (phys_addr_t)DMA_MAPPING_ERROR;	/* [한국어] 매핑 거절 — 노출 위험을 감수하느니 실패시킨다 */
	}

	trace_swiotlb_bounced(dev, phys, size);	/* [한국어] 바운스는 복사 비용을 동반하므로 성능 분석의 주요 단서다 */

	phys = swiotlb_tbl_map_single(dev, phys, size, iova_mask(iovad), dir,	/* [한국어] 전용 버퍼를 잡고 원본 내용을 복사해 넣는다. IOVA 입도를 정렬 요구로 넘겨, 잡힌 버퍼가 IOMMU 페이지 경계에 맞게 한다 */
			attrs);	/* [한국어] 호출자의 DMA 속성 */

	/*
	 * Untrusted devices should not see padding areas with random leftover
	 * kernel data, so zero the pre- and post-padding.
	 * swiotlb_tbl_map_single() has initialized the bounce buffer proper to
	 * the contents of the original memory buffer.
	 */
	if (phys != (phys_addr_t)DMA_MAPPING_ERROR && dev_is_untrusted(dev)) {	/* [한국어] 바운스에 성공했고, 상대가 신뢰할 수 없는 장치라면 */
		size_t start, virt = (size_t)phys_to_virt(phys);	/* [한국어] 바운스 버퍼의 커널 가상 주소 */

		/* Pre-padding */
		start = iova_align_down(iovad, virt);	/* [한국어] 버퍼 앞쪽, 같은 IOMMU 페이지에 속하는 부분 */
		memset((void *)start, 0, virt - start);	/* [한국어] 0 으로 지운다. 페이지 단위로 매핑되므로 장치는 이 앞쪽 패딩도 읽을 수 있고, 거기에 이전 사용자의 커널 데이터가 남아 있으면 그대로 새어 나간다 (위 영어 주석) */

		/* Post-padding */
		start = virt + size;	/* [한국어] 버퍼 뒤쪽 패딩의 시작 */
		memset((void *)start, 0, iova_align(iovad, start) - start);	/* [한국어] 페이지 끝까지 0 으로 — 같은 이유다 */
	}

	return phys;	/* [한국어] 바운스 버퍼의 물리 주소. 이제 이것을 매핑한다 */
}

/*
 * Checks if a physical buffer has unaligned boundaries with respect to
 * the IOMMU granule. Returns non-zero if either the start or end
 * address is not aligned to the granule boundary.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iova_unaligned - 버퍼의 시작이나 끝이 IOMMU 페이지 경계에서 벗어나는가
 *
 * @iovad:  IOVA 공간 (입도를 안다)
 * @phys:   버퍼의 물리 시작 주소
 * @size:   길이
 * @return: 0 이 아니면 어긋난다
 *
 * phys | size 를 한 번에 보는 것이 요령이다. 시작이 정렬되어 있고 길이도 페이지의
 * 배수여야 끝도 경계에 맞으므로, 둘을 OR 해 하위 비트가 남는지만 확인하면 된다.
 *
 * 이 판정이 바운스 여부를 가른다. 정렬이 맞으면 페이지 전체가 이 버퍼의 것이라
 * 노출될 여지가 없어, 신뢰할 수 없는 장치라도 바운스 없이 직접 매핑한다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iommu_dma_map_phys, dma_iova_link → [이 함수]
 */
static inline size_t iova_unaligned(struct iova_domain *iovad, phys_addr_t phys,
				    size_t size)
{
	return iova_offset(iovad, phys | size);	/* [한국어] 시작 주소와 길이를 OR 해 한 번에 본다 — 둘 중 하나라도 IOMMU 페이지 경계에 맞지 않으면 0 이 아니다 (위 영어 주석) */
}

/*
 * [한국어]
 * iommu_dma_map_phys - 물리 구간 하나를 매핑한다 (dma_map_page/resource 의 구현)
 *
 * @dev:    대상 장치
 * @phys:   매핑할 물리 주소
 * @size:   길이
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 * @return: 장치가 쓸 DMA 주소, 실패하면 DMA_MAPPING_ERROR
 *
 * 드라이버의 dma_map_page 가 IOMMU 아래에서 도달하는 곳이다. 세 단계를 거친다 —
 * 필요하면 바운스 버퍼로 우회, 비일관 장치면 캐시 밀어내기, 그리고 실제 매핑.
 *
 * 바운스 판정이 두 조건의 AND 인 것이 중요하다. "바운스가 필요한 장치"이면서
 * "실제로 정렬이 어긋난" 경우에만 우회한다. 신뢰할 수 없는 장치라도 페이지 정렬
 * 버퍼는 노출 위험이 없어 복사 비용을 치를 이유가 없다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: DMA API → [이 함수] → iommu_dma_map_swiotlb, __iommu_dma_map
 */
dma_addr_t iommu_dma_map_phys(struct device *dev, phys_addr_t phys, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);	/* [한국어] 캐시 일관성 여부 */
	int prot = dma_info_to_prot(dir, coherent, attrs);	/* [한국어] 방향과 속성을 PTE 권한으로 */
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 기본 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	dma_addr_t iova, dma_mask = dma_get_mask(dev);	/* [한국어] 확보할 주소와 장치의 주소 상한 */

	/*
	 * If both the physical buffer start address and size are page aligned,
	 * we don't need to use a bounce page.
	 */
	if (dev_use_swiotlb(dev, size, dir) &&	/* [한국어] 바운스가 필요한 장치이고 */
	    iova_unaligned(iovad, phys, size)) {	/* [한국어] 실제로 정렬이 어긋났다면. 정렬이 맞으면 페이지 전체가 이 버퍼의 것이라 노출될 여지가 없으므로 바운스가 불필요하다 (위 영어 주석) */
		if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT))	/* [한국어] MMIO 는 복사할 수 없고(부작용이 있다), coherent 를 요구하는 매핑은 바운스 버퍼로 만족시킬 수 없다 */
			return DMA_MAPPING_ERROR;	/* [한국어] 이 조합은 지원하지 않는다 */

		phys = iommu_dma_map_swiotlb(dev, phys, size, dir, attrs);	/* [한국어] 전용 버퍼로 바꿔치기한다 — 이후 매핑은 원본이 아니라 이 버퍼를 가리킨다 */
		if (phys == (phys_addr_t)DMA_MAPPING_ERROR)	/* [한국어] 바운스 실패 */
			return DMA_MAPPING_ERROR;	/* [한국어] 매핑 실패 */
	}

	if (!coherent && !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO))) {	/* [한국어] 비일관 장치이고, 호출자가 캐시 동기화를 생략하라고 하지 않았으며, MMIO 도 아니면 */
		arch_sync_dma_for_device(phys, size, dir);	/* [한국어] CPU 캐시를 메모리로 밀어낸다 — 장치가 곧 이 메모리를 읽는다 */
		arch_sync_dma_flush();	/* [한국어] 완료 대기 */
	}

	iova = __iommu_dma_map(dev, phys, size, prot, dma_mask);	/* [한국어] IOVA 확보 + 페이지 테이블 기입 */
	if (iova == DMA_MAPPING_ERROR &&	/* [한국어] 매핑에 실패했고 */
	    !(attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT)))	/* [한국어] 바운스를 썼을 수 있는 경우라면 */
		swiotlb_tbl_unmap_single(dev, phys, size, dir, attrs);	/* [한국어] 잡아 둔 바운스 버퍼를 되돌린다. 바운스를 쓰지 않았다면 이 호출이 무해하게 지나간다 */
	return iova;	/* [한국어] 성공 시 장치가 쓸 DMA 주소 */
}

/*
 * [한국어]
 * iommu_dma_unmap_phys - 물리 구간 매핑을 해제한다 (dma_unmap_page 의 구현)
 *
 * @dev:        대상 장치
 * @dma_handle: 해제할 DMA 주소
 * @size:       길이
 * @dir:        DMA 방향
 * @attrs:      DMA 속성
 *
 * 순서가 정해져 있다. 물리 주소를 먼저 역변환하고(매핑을 지운 뒤에는 불가능하다),
 * 캐시를 동기화하고, 매핑을 지우고, 마지막에 바운스 버퍼를 되돌린다.
 *
 * WARN_ON(!phys) 는 이중 해제를 잡는다 — 이미 지워진 주소를 다시 해제하려는 경우로,
 * 그대로 진행하면 엉뚱한 IOVA 를 지우게 된다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: DMA API → [이 함수] → __iommu_dma_unmap, swiotlb_tbl_unmap_single
 */
void iommu_dma_unmap_phys(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t phys;	/* [한국어] 캐시 동기화와 바운스 해제에 필요한 실제 물리 주소 */

	if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT)) {	/* [한국어] 이 두 경우는 바운스도 캐시 동기화도 거치지 않았다 */
		__iommu_dma_unmap(dev, dma_handle, size);	/* [한국어] 매핑만 지우면 끝 */
		return;	/* [한국어] 해제 완료 */
	}

	phys = iommu_iova_to_phys(iommu_get_dma_domain(dev), dma_handle);	/* [한국어] 매핑을 지우기 전에 물리 주소를 얻어야 한다 — 지운 뒤에는 역변환이 불가능하다 */
	if (WARN_ON(!phys))	/* [한국어] 이미 해제되었거나 이 도메인의 주소가 아니다 = 이중 해제 */
		return;	/* [한국어] 더 진행하면 엉뚱한 메모리를 만진다 */

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) && !dev_is_dma_coherent(dev)) {	/* [한국어] 비일관 장치이고 호출자가 동기화를 생략하지 않았으면 */
		arch_sync_dma_for_cpu(phys, size, dir);	/* [한국어] 장치가 쓴 내용을 CPU 가 보도록 캐시를 무효화 */
		arch_sync_dma_flush();	/* [한국어] 완료 대기 */
	}

	__iommu_dma_unmap(dev, dma_handle, size);	/* [한국어] IOVA 매핑 해제 (무효화는 정책에 따라 즉시 또는 지연) */

	swiotlb_tbl_unmap_single(dev, phys, size, dir, attrs);	/* [한국어] 바운스를 썼다면 내용을 원본으로 되복사하고 버퍼를 반납한다. 안 썼으면 무해하게 지나간다 */
}

/*
 * Prepare a successfully-mapped scatterlist to give back to the caller.
 *
 * At this point the segments are already laid out by iommu_dma_map_sg() to
 * avoid individually crossing any boundaries, so we merely need to check a
 * segment's start address to avoid concatenating across one.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * __finalise_sg - 매핑된 scatterlist 를 호출자에게 돌려줄 형태로 정리한다
 *
 * @sg:       입력 리스트 (제자리에서 고친다)
 * @nents:    입력 세그먼트 수
 * @dma_addr: 확보한 IOVA 창의 시작
 * @return:   출력 세그먼트 수 (대개 입력보다 훨씬 적다)
 *
 * iommu_dma_map_sg 가 매핑 전에 리스트를 페이지 정렬로 고치면서 원래 offset/length 를
 * 아직 쓰이지 않는 DMA 필드에 숨겨 두었다. 이 함수가 그것을 되돌리면서, 동시에
 * IOVA 상에서 연속인 세그먼트들을 하나의 출력 세그먼트로 합친다.
 *
 * 합칠 수 있는 조건이 네 가지다 — 쌓고 있는 출력이 있고, 이 세그먼트가 IOVA 페이지
 * 경계에서 시작하며(사이에 틈이 없다), 장치의 세그먼트 경계를 넘지 않고, 합친
 * 길이가 최대 세그먼트 크기를 넘지 않는다.
 *
 * 결과적으로 수십 개의 sg 항목이 한두 개로 줄어드는 일이 흔하다. NVMe 가 큰 I/O 를
 * 적은 수의 SGL 항목으로 보낼 수 있는 이유가 이것이다.
 *
 * P2PDMA 세그먼트는 IOVA 를 쓰지 않으므로 병합 대상이 아니며, 자기 버스 주소를
 * 그대로 출력에 옮기고 표식을 다시 단다.
 *
 * 실행 컨텍스트: 매핑 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: iommu_dma_map_sg → [이 함수]
 */
static int __finalise_sg(struct device *dev, struct scatterlist *sg, int nents,
		dma_addr_t dma_addr)
{
	struct scatterlist *s, *cur = sg;	/* [한국어] 입력 순회 커서와, 결과를 쌓아 갈 출력 커서. 같은 리스트를 제자리에서 압축한다 */
	unsigned long seg_mask = dma_get_seg_boundary(dev);	/* [한국어] 장치가 넘지 못하는 주소 경계 (예: 64KB 경계를 넘는 전송을 못 하는 하드웨어) */
	unsigned int cur_len = 0, max_len = dma_get_max_seg_size(dev);	/* [한국어] 쌓고 있는 출력 세그먼트의 길이와, 장치가 받을 수 있는 최대 세그먼트 길이 */
	int i, count = 0;	/* [한국어] 입력 인덱스와 만들어진 출력 세그먼트 수 */

	for_each_sg(sg, s, nents, i) {	/* [한국어] 입력 세그먼트를 하나씩 */
		/* Restore this segment's original unaligned fields first */
		dma_addr_t s_dma_addr = sg_dma_address(s);	/* [한국어] P2PDMA 세그먼트라면 여기에 버스 주소가 들어 있다 */
		unsigned int s_iova_off = sg_dma_address(s);	/* [한국어] 일반 세그먼트라면 매핑 준비 단계가 여기에 '페이지 내 오프셋'을 임시로 넣어 두었다. 같은 필드를 두 뜻으로 쓰는 것이라 아래에서 종류를 먼저 가른다 */
		unsigned int s_length = sg_dma_len(s);	/* [한국어] 원래 길이 (준비 단계가 보관해 둔 값) */
		unsigned int s_iova_len = s->length;	/* [한국어] IOVA 공간에서 이 세그먼트가 차지한 길이 (페이지 정렬로 부풀려진 값) */

		sg_dma_address(s) = DMA_MAPPING_ERROR;	/* [한국어] 입력 필드를 무효로 만들어 둔다 — 아래에서 출력 커서에만 유효한 값을 쓴다 */
		sg_dma_len(s) = 0;	/* [한국어] 마찬가지 */

		if (sg_dma_is_bus_address(s)) {	/* [한국어] P2PDMA 세그먼트 — IOMMU 를 거치지 않고 이미 버스 주소가 정해져 있다 */
			if (i > 0)	/* [한국어] 첫 세그먼트가 아니면 */
				cur = sg_next(cur);	/* [한국어] 출력 커서를 전진 */

			sg_dma_unmark_bus_address(s);	/* [한국어] 입력의 표식을 지우고 */
			sg_dma_address(cur) = s_dma_addr;	/* [한국어] 출력에 버스 주소를 그대로 옮긴다 */
			sg_dma_len(cur) = s_length;	/* [한국어] 길이도 */
			sg_dma_mark_bus_address(cur);	/* [한국어] 출력에 표식을 다시 단다 — 해제 경로가 이것을 보고 IOMMU 해제를 건너뛴다 */
			count++;	/* [한국어] 출력 세그먼트 하나 */
			cur_len = 0;	/* [한국어] P2PDMA 세그먼트에는 다음 것을 이어붙일 수 없다 */
			continue;	/* [한국어] 다음 입력으로 */
		}

		s->offset += s_iova_off;	/* [한국어] 원래의 페이지 내 오프셋을 복원한다 — 호출자는 자기가 넘긴 그대로의 오프셋/길이를 다시 보게 된다 (위 영어 주석) */
		s->length = s_length;	/* [한국어] 원래 길이 복원 */

		/*
		 * Now fill in the real DMA data. If...
		 * - there is a valid output segment to append to
		 * - and this segment starts on an IOVA page boundary
		 * - but doesn't fall at a segment boundary
		 * - and wouldn't make the resulting output segment too long
		 */
		if (cur_len && !s_iova_off && (dma_addr & seg_mask) &&	/* [한국어] 이어붙일 수 있는 조건 세 가지: 쌓고 있는 출력이 있고, 이 세그먼트가 IOVA 페이지 경계에서 시작하며(중간에 틈이 없다), 장치의 세그먼트 경계를 넘지 않는다 */
		    (max_len - cur_len >= s_length)) {	/* [한국어] 그리고 합쳐도 최대 길이를 넘지 않는다 */
			/* ...then concatenate it with the previous one */
			cur_len += s_length;	/* [한국어] 앞 세그먼트에 이어붙인다 — IOVA 가 연속이므로 장치가 보기에 하나의 구간이다. 이것이 IOMMU 로 sg 를 접는 실질적 이득이다 */
		} else {
			/* Otherwise start the next output segment */
			if (i > 0)	/* [한국어] 이어붙일 수 없다 — 새 출력 세그먼트를 시작한다 */
				cur = sg_next(cur);	/* [한국어] 출력 커서 전진 */
			cur_len = s_length;	/* [한국어] 새 세그먼트의 길이 */
			count++;	/* [한국어] 출력 세그먼트 하나 추가 */

			sg_dma_address(cur) = dma_addr + s_iova_off;	/* [한국어] 이 세그먼트의 DMA 주소 = IOVA 창의 현재 위치 + 페이지 내 오프셋 */
		}

		sg_dma_len(cur) = cur_len;	/* [한국어] 출력 세그먼트의 현재 길이 (이어붙이면 계속 늘어난다) */
		dma_addr += s_iova_len;	/* [한국어] IOVA 커서를 이 세그먼트가 차지한 만큼 전진 */

		if (s_length + s_iova_off < s_iova_len)	/* [한국어] 이 세그먼트 뒤에 페이지 정렬 패딩이 남았다면 */
			cur_len = 0;	/* [한국어] 다음 것을 이어붙일 수 없다 — 사이에 쓰이지 않는 구간이 끼기 때문 */
	}
	return count;	/* [한국어] 호출자에게 돌려줄 출력 세그먼트 개수. 입력보다 대개 훨씬 적다 */
}

/*
 * If mapping failed, then just restore the original list,
 * but making sure the DMA fields are invalidated.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * __invalidate_sg - 매핑에 실패했을 때 리스트를 원래대로 되돌린다
 *
 * @sg:    입력 리스트
 * @nents: 세그먼트 수
 *
 * iommu_dma_map_sg 가 제자리에서 고친 offset/length 를 숨겨 둔 값으로 복원하고,
 * DMA 필드는 무효로 만든다. 호출자가 실패한 리스트를 그대로 다시 쓸 수 있어야
 * 하기 때문이다 (예: 요청을 쪼개 재시도).
 *
 * DMA_MAPPING_ERROR 와 0 을 검사하는 것은 아직 손대지 않은 세그먼트를 구별하기
 * 위해서다 — 순회 도중에 실패했으면 뒤쪽은 원본 그대로다.
 *
 * 실행 컨텍스트: 매핑 실패 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: iommu_dma_map_sg → [이 함수]
 */
static void __invalidate_sg(struct scatterlist *sg, int nents)
{
	struct scatterlist *s;	/* [한국어] 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	for_each_sg(sg, s, nents, i) {	/* [한국어] 모든 세그먼트에 대해 */
		if (sg_dma_is_bus_address(s)) {	/* [한국어] P2PDMA 세그먼트 */
			sg_dma_unmark_bus_address(s);	/* [한국어] 표식만 지운다 — 원본 offset/length 는 건드리지 않았다 */
		} else {
			if (sg_dma_address(s) != DMA_MAPPING_ERROR)	/* [한국어] 준비 단계가 오프셋을 저장해 둔 세그먼트라면 */
				s->offset += sg_dma_address(s);	/* [한국어] 원래 오프셋을 복원 */
			if (sg_dma_len(s))	/* [한국어] 길이를 저장해 둔 세그먼트라면 */
				s->length = sg_dma_len(s);	/* [한국어] 원래 길이를 복원 */
		}
		sg_dma_address(s) = DMA_MAPPING_ERROR;	/* [한국어] DMA 필드는 무효로 — 호출자가 실패한 매핑을 쓰지 못하게 (위 영어 주석) */
		sg_dma_len(s) = 0;	/* [한국어] 마찬가지 */
	}
}

/*
 * [한국어]
 * iommu_dma_unmap_sg_swiotlb - 바운스 경로로 매핑된 리스트를 해제한다
 *
 * @dev:   대상 장치
 * @sg:    첫 세그먼트
 * @nents: 세그먼트 수
 * @dir:   DMA 방향
 * @attrs: DMA 속성
 *
 * 바운스 경로는 병합하지 않고 세그먼트마다 독립적으로 매핑했으므로, 해제도 1:1 로
 * 반복한다. 일반 경로가 창 하나를 한 번에 해제하는 것과 대비된다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iommu_dma_unmap_sg, iommu_dma_map_sg_swiotlb 되감기 → [이 함수]
 */
static void iommu_dma_unmap_sg_swiotlb(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *s;	/* [한국어] 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	for_each_sg(sg, s, nents, i)	/* [한국어] 세그먼트마다 */
		iommu_dma_unmap_phys(dev, sg_dma_address(s),	/* [한국어] 단일 매핑처럼 하나씩 해제한다 — 바운스 경로는 병합하지 않았으므로 1:1 대응이다 */
				sg_dma_len(s), dir, attrs);	/* [한국어] 그 세그먼트의 길이 */
}

/*
 * [한국어]
 * iommu_dma_map_sg_swiotlb - 리스트 전체를 바운스 버퍼로 매핑한다
 *
 * @dev:    대상 장치
 * @sg:     첫 세그먼트
 * @nents:  세그먼트 수
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 * @return: 성공 시 nents (병합이 없어 입력과 같다), 실패 시 -EIO
 *
 * 병합을 포기하는 것이 이 경로의 대가다. 바운스 버퍼들은 서로 인접하지 않으므로
 * 하나의 연속 IOVA 창으로 접을 수 없고, 세그먼트마다 따로 매핑해야 한다. 그래서
 * 신뢰할 수 없는 장치는 IOMMU 를 쓰면서도 sg 병합의 이득을 받지 못한다.
 *
 * 리스트에 swiotlb 표식을 다는 것이 첫 줄인데, 이후의 sync 와 해제가 그 표식을
 * 보고 경로를 가르기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. 어디서든.
 *
 * 호출 체인: iommu_dma_map_sg → [이 함수] → iommu_dma_map_phys
 */
static int iommu_dma_map_sg_swiotlb(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *s;	/* [한국어] 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	sg_dma_mark_swiotlb(sg);	/* [한국어] 이 리스트가 바운스 경로로 매핑되었음을 표시한다. 이후 sync 와 해제가 이 표식을 보고 갈라진다 */

	for_each_sg(sg, s, nents, i) {	/* [한국어] 세그먼트마다 */
		sg_dma_address(s) = iommu_dma_map_phys(dev, sg_phys(s),	/* [한국어] 각각 독립적으로 매핑한다 — 병합하지 않는다. 바운스 버퍼가 서로 인접하지 않기 때문이다 */
				s->length, dir, attrs);	/* [한국어] 그 세그먼트의 길이 */
		if (sg_dma_address(s) == DMA_MAPPING_ERROR)	/* [한국어] 한 세그먼트라도 실패하면 */
			goto out_unmap;	/* [한국어] 이미 매핑한 것들을 되돌린다 */
		sg_dma_len(s) = s->length;	/* [한국어] 이 경로에서는 출력 세그먼트 수가 입력과 같다 */
	}

	return nents;	/* [한국어] 병합이 없으므로 입력 개수 그대로 */

out_unmap:	/* [한국어] 부분 성공 되감기 */
	iommu_dma_unmap_sg_swiotlb(dev, sg, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);	/* [한국어] 성공한 i 개만 해제한다. SKIP_CPU_SYNC 를 더하는 것은 아직 장치가 건드린 적이 없어 캐시를 되돌릴 필요가 없기 때문이다 */
	return -EIO;	/* [한국어] 매핑 실패 */
}

/*
 * The DMA API client is passing in a scatterlist which could describe
 * any old buffer layout, but the IOMMU API requires everything to be
 * aligned to IOMMU pages. Hence the need for this complicated bit of
 * impedance-matching, to be able to hand off a suitably-aligned list,
 * but still preserve the original offsets and sizes for the caller.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_dma_map_sg - scatterlist 를 하나의 연속 IOVA 창으로 접는다 (dma_map_sg 의 구현)
 *
 * @dev:    대상 장치
 * @sg:     첫 세그먼트
 * @nents:  세그먼트 수
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 * @return: 출력 세그먼트 수(양수), 실패하면 음수
 *
 * IOMMU 를 켜는 가장 실질적인 이득이 이 함수다. 블록 계층이 넘긴 수십 개의 흩어진
 * 페이지가 장치에게는 한두 개의 연속 구간으로 보이게 된다.
 *
 * 두 단계로 나뉜다. 먼저 리스트 전체를 훑어 필요한 IOVA 길이를 계산하면서 각
 * 세그먼트를 페이지 정렬로 다듬고(원래 값은 DMA 필드에 숨긴다), 그 다음 창 하나를
 * 확보해 iommu_map_sg 로 통째로 채운다. 마지막에 __finalise_sg 가 원래 값을
 * 복원하며 세그먼트를 병합한다.
 *
 * 패딩 처리가 이 함수에서 가장 미묘한 부분이다. 어떤 세그먼트가 장치의 세그먼트
 * 경계를 넘게 되면, 그 세그먼트가 아니라 '앞' 세그먼트를 늘려 경계까지 채운다.
 * IOVA 창 하나를 정렬해 잡기 때문에 실제 주소를 몰라도 길이만으로 이 배치가
 * 성립한다 (위 영어 주석의 세 가정).
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능 (GFP_ATOMIC).
 *
 * 호출 체인: DMA API → [이 함수]
 *            → iommu_dma_alloc_iova, iommu_map_sg, __finalise_sg
 */
int iommu_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 이 장치의 기본 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	struct scatterlist *s, *prev = NULL;	/* [한국어] 순회 커서와 직전 세그먼트 (패딩을 앞 세그먼트에 붙이기 위해 필요) */
	int prot = dma_info_to_prot(dir, dev_is_dma_coherent(dev), attrs);	/* [한국어] 전체 리스트에 같은 권한을 쓴다 */
	struct pci_p2pdma_map_state p2pdma_state = {};	/* [한국어] P2PDMA 판정 상태 — 같은 메모리 영역이 반복되면 재판정을 아낀다 */
	dma_addr_t iova;	/* [한국어] 확보할 IOVA 창 */
	size_t iova_len = 0;	/* [한국어] 리스트 전체가 IOVA 공간에서 차지할 길이 */
	unsigned long mask = dma_get_seg_boundary(dev);	/* [한국어] 장치가 넘지 못하는 주소 경계 */
	ssize_t ret;	/* [한국어] 매핑 결과 */
	int i;	/* [한국어] 세그먼트 인덱스 */

	if (static_branch_unlikely(&iommu_deferred_attach_enabled)) {	/* [한국어] 지연 부착이 필요한 시스템에서만 */
		ret = iommu_deferred_attach(dev, domain);	/* [한국어] 여기서 실제로 도메인을 건다 */
		if (ret)	/* [한국어] 부착 실패 */
			goto out;	/* [한국어] 에러 코드 정규화를 거쳐 반환 */
	}

	if (dev_use_sg_swiotlb(dev, sg, nents, dir))	/* [한국어] 신뢰할 수 없는 장치이거나 정렬이 위험한 세그먼트가 있으면 */
		return iommu_dma_map_sg_swiotlb(dev, sg, nents, dir, attrs);	/* [한국어] 병합 없이 하나씩 바운스 매핑하는 경로로 간다 */

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))	/* [한국어] 호출자가 생략하라고 하지 않았으면 */
		iommu_dma_sync_sg_for_device(dev, sg, nents, dir);	/* [한국어] 매핑 전에 캐시를 밀어낸다 */

	/*
	 * Work out how much IOVA space we need, and align the segments to
	 * IOVA granules for the IOMMU driver to handle. With some clever
	 * trickery we can modify the list in-place, but reversibly, by
	 * stashing the unaligned parts in the as-yet-unused DMA fields.
	 */
	for_each_sg(sg, s, nents, i) {	/* [한국어] 1단계 — 필요한 IOVA 길이를 계산하며 리스트를 페이지 정렬로 다듬는다 */
		size_t s_iova_off = iova_offset(iovad, s->offset);	/* [한국어] 이 세그먼트가 IOMMU 페이지 안에서 시작하는 오프셋 */
		size_t s_length = s->length;	/* [한국어] 원래 길이 */
		size_t pad_len = (mask - iova_len + 1) & mask;	/* [한국어] 다음 세그먼트 경계까지 남은 거리. 여기까지 채워야 세그먼트가 경계를 넘지 않는다 */

		switch (pci_p2pdma_state(&p2pdma_state, dev, sg_page(s))) {	/* [한국어] 이 페이지가 P2PDMA 메모리인지, 그렇다면 어떤 경로인지 판정한다 */
		case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:	/* [한국어] 호스트 브리지를 거쳐 가는 P2P — 결국 메모리처럼 다뤄야 한다 */
			/*
			 * Mapping through host bridge should be mapped with
			 * regular IOVAs, thus we do nothing here and continue
			 * below.
			 */
			break;	/* [한국어] 일반 세그먼트와 똑같이 아래에서 매핑한다 (위 영어 주석) */
		case PCI_P2PDMA_MAP_NONE:	/* [한국어] P2PDMA 가 아닌 평범한 시스템 메모리 */
			break;	/* [한국어] 일반 처리 */
		case PCI_P2PDMA_MAP_BUS_ADDR:	/* [한국어] 스위치 안에서 장치끼리 직접 오가는 경우 — IOMMU 를 거치지 않는다 */
			/*
			 * iommu_map_sg() will skip this segment as it is marked
			 * as a bus address, __finalise_sg() will copy the dma
			 * address into the output segment.
			 */
			s->dma_address = pci_p2pdma_bus_addr_map(	/* [한국어] 물리 주소를 버스 주소로 직접 변환해 넣는다 */
				p2pdma_state.mem, sg_phys(s));	/* [한국어] 그 메모리 영역의 변환 정보 */
			sg_dma_len(s) = sg->length;	/* [한국어] 길이 기록 */
			sg_dma_mark_bus_address(s);	/* [한국어] 표식을 단다. iommu_map_sg 가 이 세그먼트를 건너뛰고, __finalise_sg 가 주소를 그대로 출력에 옮긴다 (위 영어 주석) */
			continue;	/* [한국어] IOVA 길이 계산에서 제외 — 주소 공간을 쓰지 않는다 */
		default:	/* [한국어] 알 수 없는 P2PDMA 상태 */
			ret = -EREMOTEIO;	/* [한국어] 이 장치에서 도달할 수 없는 P2P 메모리 */
			goto out_restore_sg;	/* [한국어] 리스트를 원상 복구하고 실패 */
		}

		sg_dma_address(s) = s_iova_off;	/* [한국어] 아직 쓰이지 않는 DMA 필드에 원래 오프셋을 숨겨 둔다. 제자리에서 리스트를 고치되 되돌릴 수 있게 만드는 요령이다 (위 영어 주석) */
		sg_dma_len(s) = s_length;	/* [한국어] 원래 길이도 함께 숨긴다 */
		s->offset -= s_iova_off;	/* [한국어] 페이지 경계로 내린다 — IOMMU API 는 페이지 정렬된 입력만 받는다 */
		s_length = iova_align(iovad, s_length + s_iova_off);	/* [한국어] 앞 오프셋을 포함해 페이지 단위로 올림 */
		s->length = s_length;	/* [한국어] 정렬된 길이로 바꿔 둔다 */

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
		if (pad_len && pad_len < s_length - 1) {	/* [한국어] 이 세그먼트가 장치의 세그먼트 경계를 넘게 된다면 */
			prev->length += pad_len;	/* [한국어] 앞 세그먼트를 늘려 경계까지 채운다. 그러면 이 세그먼트는 경계에서 시작해 넘지 않는다. 위 영어 주석이 설명하듯, IOVA 창 하나를 정렬해 잡기 때문에 실제 주소를 몰라도 길이만으로 이 배치가 성립한다 */
			iova_len += pad_len;	/* [한국어] 패딩만큼 필요한 IOVA 도 늘어난다 */
		}

		iova_len += s_length;	/* [한국어] 이 세그먼트가 차지할 길이 누적 */
		prev = s;	/* [한국어] 다음 회차에서 패딩을 붙일 대상 */
	}

	if (!iova_len)	/* [한국어] IOVA 를 쓰는 세그먼트가 하나도 없다 (전부 P2PDMA 버스 주소) */
		return __finalise_sg(dev, sg, nents, 0);	/* [한국어] 매핑 없이 출력만 정리한다 */

	iova = iommu_dma_alloc_iova(domain, iova_len, dma_get_mask(dev), dev);	/* [한국어] 리스트 전체를 담을 연속 IOVA 창을 한 번에 확보한다 — 이것이 sg 를 하나의 주소로 접는 근거다 */
	if (!iova) {	/* [한국어] IOVA 고갈 */
		ret = -ENOMEM;	/* [한국어] 이유 기록 */
		goto out_restore_sg;	/* [한국어] 리스트 복구 후 실패 */
	}

	/*
	 * We'll leave any physical concatenation to the IOMMU driver's
	 * implementation - it knows better than we do.
	 */
	ret = iommu_map_sg(domain, iova, sg, nents, prot, GFP_ATOMIC);	/* [한국어] 물리적으로 인접한 세그먼트를 합치는 것은 IOMMU 코어에 맡긴다 — 페이지 크기 선택까지 함께 판단할 수 있는 쪽이 더 잘한다 (위 영어 주석) */
	if (ret < 0 || ret < iova_len)	/* [한국어] 실패했거나 일부만 매핑되었다 */
		goto out_free_iova;	/* [한국어] IOVA 부터 되돌린다 */

	return __finalise_sg(dev, sg, nents, iova);	/* [한국어] 숨겨 둔 오프셋을 복원하고 세그먼트를 병합해 호출자에게 돌려줄 형태로 만든다 */

out_free_iova:	/* [한국어] 매핑 실패 경로 */
	iommu_dma_free_iova(domain, iova, iova_len, NULL);	/* [한국어] 확보한 창 반납 (매핑이 없으니 무효화도 불필요) */
out_restore_sg:	/* [한국어] IOVA 확보 실패와 P2P 오류가 합류 */
	__invalidate_sg(sg, nents);	/* [한국어] 제자리에서 고쳐 둔 리스트를 원래대로 되돌린다 */
out:	/* [한국어] 지연 부착 실패도 합류 */
	if (ret != -ENOMEM && ret != -EREMOTEIO)	/* [한국어] DMA API 는 이 세 값만 의미 있게 구분한다 */
		return -EINVAL;	/* [한국어] 나머지는 모두 잘못된 인자로 뭉뚱그린다 */
	return ret;	/* [한국어] -ENOMEM 또는 -EREMOTEIO */
}

/*
 * [한국어]
 * iommu_dma_unmap_sg - scatterlist 매핑을 해제한다 (dma_unmap_sg 의 구현)
 *
 * @dev:    대상 장치
 * @sg:     첫 세그먼트
 * @nents:  세그먼트 수
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 *
 * 리스트 전체가 하나의 연속 IOVA 창에 들어 있으므로, 시작과 끝만 찾으면 한 번의
 * unmap 으로 끝난다 (위 영어 주석). 세그먼트가 몇 개든 IOMMU 호출은 한 번이라는
 * 점이 병합의 또 다른 이득이다.
 *
 * P2PDMA 세그먼트는 IOVA 를 쓰지 않으므로 범위 계산에서 제외하고 표식만 지운다.
 * 길이 0 세그먼트는 __finalise_sg 가 병합하며 남긴 빈 자리로, 리스트의 끝을 뜻한다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: DMA API → [이 함수] → __iommu_dma_unmap
 */
void iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, unsigned long attrs)
{
	dma_addr_t end = 0, start;	/* [한국어] 해제할 IOVA 창의 양 끝. 매핑이 하나의 연속 창이므로 시작과 끝만 알면 된다 */
	struct scatterlist *tmp;	/* [한국어] 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	if (sg_dma_is_swiotlb(sg)) {	/* [한국어] 바운스 경로로 매핑되었다면 */
		iommu_dma_unmap_sg_swiotlb(dev, sg, nents, dir, attrs);	/* [한국어] 세그먼트마다 개별 해제한다 — 그쪽은 병합하지 않았다 */
		return;	/* [한국어] 해제 완료 */
	}

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))	/* [한국어] 호출자가 생략하라고 하지 않았으면 */
		iommu_dma_sync_sg_for_cpu(dev, sg, nents, dir);	/* [한국어] 매핑을 지우기 전에 장치가 쓴 내용을 CPU 가 볼 수 있게 한다 */

	/*
	 * The scatterlist segments are mapped into a single
	 * contiguous IOVA allocation, the start and end points
	 * just have to be determined.
	 */
	for_each_sg(sg, tmp, nents, i) {	/* [한국어] 1단계 — 창의 시작을 찾는다 (위 영어 주석) */
		if (sg_dma_is_bus_address(tmp)) {	/* [한국어] P2PDMA 세그먼트는 IOVA 를 쓰지 않는다 */
			sg_dma_unmark_bus_address(tmp);	/* [한국어] 표식만 지우고 */
			continue;	/* [한국어] 건너뛴다 */
		}

		if (sg_dma_len(tmp) == 0)	/* [한국어] 길이 0 = 출력 리스트의 끝 (__finalise_sg 가 병합하며 남긴 빈 세그먼트) */
			break;	/* [한국어] 여기까지 */

		start = sg_dma_address(tmp);	/* [한국어] 첫 유효 세그먼트의 주소가 창의 시작이다 */
		break;	/* [한국어] 찾았으므로 종료 */
	}

	nents -= i;	/* [한국어] 이미 지나온 만큼 남은 개수를 줄인다 */
	for_each_sg(tmp, tmp, nents, i) {	/* [한국어] 2단계 — 창의 끝을 찾는다. 시작 세그먼트부터 이어서 훑는다 */
		if (sg_dma_is_bus_address(tmp)) {	/* [한국어] P2PDMA 세그먼트 */
			sg_dma_unmark_bus_address(tmp);	/* [한국어] 표식 제거 */
			continue;	/* [한국어] 건너뛴다 */
		}

		if (sg_dma_len(tmp) == 0)	/* [한국어] 리스트의 끝 */
			break;	/* [한국어] 순회 종료 */

		end = sg_dma_address(tmp) + sg_dma_len(tmp);	/* [한국어] 마지막 유효 세그먼트의 끝이 창의 끝이다 — 매번 갱신하므로 루프가 끝나면 최댓값이 남는다 */
	}

	if (end)	/* [한국어] 해제할 IOVA 가 실제로 있었다면 */
		__iommu_dma_unmap(dev, start, end - start);	/* [한국어] 창 전체를 한 번에 해제한다. 세그먼트 수와 무관하게 unmap 호출이 한 번이라는 점이 병합의 또 다른 이득이다 */
}

/*
 * [한국어]
 * __iommu_dma_free - coherent 할당의 CPU 쪽 자원을 해제한다
 *
 * @dev:      대상 장치
 * @size:     할당 크기
 * @cpu_addr: alloc 이 돌려준 가상 주소
 *
 * coherent 할당에 경로가 여럿이라(아토믹 풀, 재매핑된 흩어진 페이지, 재매핑된
 * 연속 블록, lowmem 연속 블록) 어느 쪽이었는지 여기서 되짚어야 한다. 판별 근거는
 * 주소의 성질뿐이다 — 풀에 속하는지, vmalloc 영역인지, 페이지 배열이 등록되어
 * 있는지.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_free, iommu_dma_alloc 에러 경로 → [이 함수]
 */
static void __iommu_dma_free(struct device *dev, size_t size, void *cpu_addr)
{
	size_t alloc_size = PAGE_ALIGN(size);	/* [한국어] 실제로 잡았던 크기 */
	int count = alloc_size >> PAGE_SHIFT;	/* [한국어] 페이지 수 */
	struct page *page = NULL, **pages = NULL;	/* [한국어] 단일 블록으로 잡았는지, 흩어진 페이지 배열인지 — 할당 경로가 여럿이라 여기서 판별한다 */

	/* Non-coherent atomic allocation? Easy */
	if (IS_ENABLED(CONFIG_DMA_DIRECT_REMAP) &&	/* [한국어] 아토믹 풀에서 잡은 것인지 먼저 확인한다 */
	    dma_free_from_pool(dev, cpu_addr, alloc_size))	/* [한국어] 풀의 것이었으면 여기서 반납이 끝난다 */
		return;	/* [한국어] 가장 단순한 경로 (위 영어 주석) */

	if (is_vmalloc_addr(cpu_addr)) {	/* [한국어] 가상 주소로 재매핑된 할당이면 */
		/*
		 * If it the address is remapped, then it's either non-coherent
		 * or highmem CMA, or an iommu_dma_alloc_remap() construction.
		 */
		pages = dma_common_find_pages(cpu_addr);	/* [한국어] iommu_dma_alloc_remap 이 남긴 페이지 배열을 찾는다 */
		if (!pages)	/* [한국어] 배열이 없다면 흩어진 것이 아니라 */
			page = vmalloc_to_page(cpu_addr);	/* [한국어] 연속 블록을 재매핑한 경우다 (비일관 장치나 highmem CMA) */
		dma_common_free_remap(cpu_addr, alloc_size);	/* [한국어] 가상 매핑 해제 */
	} else {
		/* Lowmem means a coherent atomic or CMA allocation */
		page = virt_to_page(cpu_addr);	/* [한국어] lowmem 주소 = 재매핑 없이 그대로 쓴 연속 블록 (위 영어 주석) */
	}

	if (pages)	/* [한국어] 흩어진 페이지들이었으면 */
		__iommu_dma_free_pages(pages, count);	/* [한국어] 낱장으로 반납 */
	if (page)	/* [한국어] 연속 블록이었으면 */
		dma_free_contiguous(dev, page, alloc_size);	/* [한국어] CMA 또는 버디 할당자로 반납 */
}

/*
 * [한국어]
 * iommu_dma_free - coherent 할당을 해제한다 (dma_free_coherent 의 구현)
 *
 * @dev:      대상 장치
 * @size:     할당 크기
 * @cpu_addr: CPU 가상 주소
 * @handle:   장치가 쓰던 DMA 주소
 * @attrs:    DMA 속성
 *
 * 장치 쪽 매핑을 먼저 지우고 그 다음 메모리를 반납한다. 반대로 하면 반납된
 * 페이지에 장치가 계속 DMA 할 수 있는 창이 열린다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인: DMA API → [이 함수]
 */
void iommu_dma_free(struct device *dev, size_t size, void *cpu_addr,
		dma_addr_t handle, unsigned long attrs)
{
	__iommu_dma_unmap(dev, handle, size);	/* [한국어] 장치 쪽 매핑을 먼저 지운다 — 페이지를 반납하기 전에 장치가 닿지 못하게 해야 한다 */
	__iommu_dma_free(dev, size, cpu_addr);	/* [한국어] 그 다음 CPU 쪽 매핑과 페이지를 정리 */
}

/*
 * [한국어]
 * iommu_dma_alloc_pages - 물리적으로 연속인 블록을 잡고 필요하면 재매핑한다
 *
 * @dev:    대상 장치
 * @size:   요청 크기
 * @pagep:  확보한 첫 페이지를 여기에 돌려준다
 * @gfp:    할당 플래그
 * @attrs:  DMA 속성
 * @return: CPU 가 쓸 주소, 실패하면 NULL
 *
 * 흩어진 페이지를 접는 경로를 쓸 수 없을 때의 대안이다 — 아토믹 문맥이거나
 * DMA_ATTR_FORCE_CONTIGUOUS 요청일 때 여기로 온다. 물리 연속을 요구하므로 큰
 * 크기에서는 실패할 수 있고, 그래서 CMA 를 먼저 시도한다.
 *
 * 비일관 장치이거나 highmem 페이지를 얻었으면 커널 가상 매핑을 새로 만든다.
 * 전자는 비캐시 속성이 필요해서이고, 후자는 선형 매핑이 없어서다.
 *
 * memset 으로 0 을 채우는 것은 선택이 아니다 — 이전 사용자의 커널 데이터가 장치에
 * 노출되면 안 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 아토믹(gfp 가 정한다).
 *
 * 호출 체인: iommu_dma_alloc → [이 함수]
 */
static void *iommu_dma_alloc_pages(struct device *dev, size_t size,
		struct page **pagep, gfp_t gfp, unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);	/* [한국어] 캐시 일관성 여부 */
	size_t alloc_size = PAGE_ALIGN(size);	/* [한국어] 페이지 단위로 올림 */
	int node = dev_to_node(dev);	/* [한국어] 장치와 가까운 NUMA 노드 */
	struct page *page = NULL;	/* [한국어] 확보한 연속 블록 */
	void *cpu_addr;	/* [한국어] CPU 가 쓸 주소 */

	page = dma_alloc_contiguous(dev, alloc_size, gfp);	/* [한국어] 먼저 CMA 에서 시도 — 큰 연속 블록은 CMA 가 유리하다 */
	if (!page)	/* [한국어] CMA 에 없으면 */
		page = alloc_pages_node(node, gfp, get_order(alloc_size));	/* [한국어] 버디 할당자에서 연속 블록으로. 이 경로는 물리 연속을 요구하므로 큰 크기에서 실패하기 쉽다 */
	if (!page)	/* [한국어] 둘 다 실패 */
		return NULL;	/* [한국어] 할당 실패 */

	if (!coherent || PageHighMem(page)) {	/* [한국어] 비일관 장치이거나 highmem 페이지면 커널 가상 매핑을 새로 만들어야 한다 */
		pgprot_t prot = dma_pgprot(dev, PAGE_KERNEL, attrs);	/* [한국어] 비일관이면 여기서 비캐시 속성이 된다 */

		cpu_addr = dma_common_contiguous_remap(page, alloc_size,	/* [한국어] 연속 블록을 그 속성으로 다시 매핑한다 */
				prot, __builtin_return_address(0));	/* [한국어] 진단용 호출자 주소 */
		if (!cpu_addr)	/* [한국어] 가상 매핑 실패 */
			goto out_free_pages;	/* [한국어] 페이지 반납 */

		if (!coherent)	/* [한국어] 비일관 장치면 */
			arch_dma_prep_coherent(page, size);	/* [한국어] 캐시를 비운다 — 이 페이지는 이제 비캐시로 접근되므로, 캐시에 남은 옛 내용이 나중에 write-back 되면 안 된다 */
	} else {
		cpu_addr = page_address(page);	/* [한국어] 일관성 있는 lowmem 페이지는 선형 매핑 주소를 그대로 쓴다 */
	}

	*pagep = page;	/* [한국어] 호출자가 물리 주소를 얻을 수 있도록 */
	memset(cpu_addr, 0, alloc_size);	/* [한국어] 반드시 0 으로 채운다 — 이전 사용자의 커널 데이터가 장치에 노출되면 안 된다 */
	return cpu_addr;	/* [한국어] CPU 가 쓸 주소 */
out_free_pages:	/* [한국어] 가상 매핑 실패 경로 */
	dma_free_contiguous(dev, page, alloc_size);	/* [한국어] 블록 반납 */
	return NULL;	/* [한국어] 할당 실패 */
}

/*
 * [한국어]
 * iommu_dma_alloc - coherent DMA 버퍼를 할당한다 (dma_alloc_coherent 의 구현)
 *
 * @dev:    대상 장치
 * @size:   요청 크기
 * @handle: 장치가 쓸 DMA 주소를 여기에 채운다
 * @gfp:    할당 플래그
 * @attrs:  DMA 속성
 * @return: CPU 가 쓸 주소, 실패하면 NULL
 *
 * 세 경로로 갈린다.
 *  - 잠들 수 있고 물리 연속을 강제하지 않으면: 흩어진 페이지를 IOVA 창 하나로
 *    접는다. IOMMU 를 쓰는 가장 큰 이득이며, 단편화된 시스템에서도 큰 버퍼가
 *    실패하지 않는다.
 *  - 아토믹 문맥의 비일관 장치: 미리 만들어 둔 비캐시 풀에서 꺼낸다. 아토믹
 *    문맥에서는 페이지 재매핑을 할 수 없기 때문이다.
 *  - 그 외: 물리 연속 블록을 잡는다.
 *
 * NVMe 드라이버의 큐 메모리, 네트워크 드라이버의 디스크립터 링이 이 함수로 온다.
 *
 * 실행 컨텍스트: gfp 가 정한다.
 *
 * 호출 체인: DMA API → [이 함수]
 *            → iommu_dma_alloc_remap / dma_alloc_from_pool / iommu_dma_alloc_pages
 */
void *iommu_dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
		gfp_t gfp, unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);	/* [한국어] 캐시 일관성 여부 */
	int ioprot = dma_info_to_prot(DMA_BIDIRECTIONAL, coherent, attrs);	/* [한국어] coherent 버퍼는 양방향으로 쓰인다 */
	struct page *page = NULL;	/* [한국어] 확보한 페이지(들의 첫 장) */
	void *cpu_addr;	/* [한국어] CPU 가 쓸 주소 */

	gfp |= __GFP_ZERO;	/* [한국어] DMA 버퍼는 반드시 0 으로 시작해야 한다 — 커널 데이터 유출 방지 */

	if (gfpflags_allow_blocking(gfp) &&	/* [한국어] 잠들 수 있는 문맥이고 */
	    !(attrs & DMA_ATTR_FORCE_CONTIGUOUS)) {	/* [한국어] 물리 연속을 강제하지 않는다면 */
		return iommu_dma_alloc_remap(dev, size, handle, gfp, attrs);	/* [한국어] 흩어진 페이지를 IOVA 창 하나로 접는 경로 — IOMMU 를 쓰는 가장 큰 이유다. 물리 연속이 필요 없으니 큰 버퍼도 실패하지 않는다 */
	}

	if (IS_ENABLED(CONFIG_DMA_DIRECT_REMAP) &&	/* [한국어] 아토믹 풀이 있는 빌드이고 */
	    !gfpflags_allow_blocking(gfp) && !coherent)	/* [한국어] 잠들 수 없는 문맥의 비일관 장치라면 */
		page = dma_alloc_from_pool(dev, PAGE_ALIGN(size), &cpu_addr,	/* [한국어] 미리 만들어 둔 비캐시 풀에서 꺼낸다 — 아토믹 문맥에서는 재매핑을 할 수 없기 때문이다 */
					       gfp, NULL);	/* [한국어] 할당 플래그 */
	else
		cpu_addr = iommu_dma_alloc_pages(dev, size, &page, gfp, attrs);	/* [한국어] 그 외에는 물리 연속 블록으로 (FORCE_CONTIGUOUS 요청이 여기 온다) */
	if (!cpu_addr)	/* [한국어] 확보 실패 */
		return NULL;	/* [한국어] 할당 실패 */

	*handle = __iommu_dma_map(dev, page_to_phys(page), size, ioprot,	/* [한국어] 연속 블록을 IOVA 에 매핑한다 */
			dev->coherent_dma_mask);	/* [한국어] coherent 전용 마스크 — 스트리밍 마스크와 다를 수 있다 */
	if (*handle == DMA_MAPPING_ERROR) {	/* [한국어] 매핑 실패 */
		__iommu_dma_free(dev, size, cpu_addr);	/* [한국어] 확보한 메모리를 되돌린다 */
		return NULL;	/* [한국어] 할당 실패 */
	}

	return cpu_addr;	/* [한국어] CPU 는 이 주소, 장치는 *handle 로 같은 메모리를 본다 */
}

/*
 * [한국어]
 * iommu_dma_mmap - coherent 버퍼를 사용자 공간에 매핑한다 (dma_mmap_coherent 의 구현)
 *
 * @dev:      대상 장치
 * @vma:      사용자 매핑 영역
 * @cpu_addr: alloc 이 돌려준 주소
 * @dma_addr: 장치가 쓰는 주소 (여기서는 쓰지 않는다)
 * @size:     할당 크기
 * @attrs:    DMA 속성
 * @return:   0 성공, -ENXIO 면 범위 초과
 *
 * 사용자 공간 매핑에도 같은 캐시 속성을 적용하는 것이 중요하다. 비일관 장치의
 * 버퍼를 사용자가 캐시 가능으로 보면 장치가 쓴 내용을 못 보거나 그 반대가 된다.
 *
 * 범위 검사는 보안 경계다. 없으면 사용자가 오프셋을 크게 주어 할당 범위 밖의
 * 커널 메모리를 매핑할 수 있다.
 *
 * 실행 컨텍스트: mmap 시스템 호출. 프로세스 문맥.
 *
 * 호출 체인: DMA API → [이 함수]
 */
int iommu_dma_mmap(struct device *dev, struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	unsigned long nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;	/* [한국어] 전체 페이지 수 */
	unsigned long pfn, off = vma->vm_pgoff;	/* [한국어] 매핑할 첫 페이지 번호와 사용자 요청 오프셋 */
	int ret;	/* [한국어] 결과 */

	vma->vm_page_prot = dma_pgprot(dev, vma->vm_page_prot, attrs);	/* [한국어] 사용자 공간 매핑에도 같은 캐시 속성을 적용한다 — 비일관 장치면 비캐시여야 한다 */

	if (dma_mmap_from_dev_coherent(dev, vma, cpu_addr, size, &ret))	/* [한국어] 장치 전용 coherent 영역에서 온 버퍼면 그쪽이 처리한다 */
		return ret;	/* [한국어] 그 결과를 그대로 */

	if (off >= nr_pages || vma_pages(vma) > nr_pages - off)	/* [한국어] 요청 범위가 할당 범위를 벗어난다 */
		return -ENXIO;	/* [한국어] 거절 — 이 검사가 없으면 사용자가 인접한 커널 메모리를 매핑할 수 있다 */

	if (is_vmalloc_addr(cpu_addr)) {	/* [한국어] 재매핑된 할당이면 */
		struct page **pages = dma_common_find_pages(cpu_addr);	/* [한국어] 흩어진 페이지 배열을 찾는다 */

		if (pages)	/* [한국어] 배열이 있으면 */
			return vm_map_pages(vma, pages, nr_pages);	/* [한국어] 낱장씩 사용자 공간에 매핑 */
		pfn = vmalloc_to_pfn(cpu_addr);	/* [한국어] 연속 블록을 재매핑한 경우 */
	} else {
		pfn = page_to_pfn(virt_to_page(cpu_addr));	/* [한국어] lowmem 연속 블록 */
	}

	return remap_pfn_range(vma, vma->vm_start, pfn + off,	/* [한국어] 연속 물리 범위를 한 번에 매핑 */
			       vma->vm_end - vma->vm_start,	/* [한국어] 사용자가 요청한 길이 */
			       vma->vm_page_prot);	/* [한국어] 위에서 정한 캐시 속성 */
}

/*
 * [한국어]
 * iommu_dma_get_sgtable - coherent 버퍼를 기술하는 scatterlist 를 만든다
 *
 * @dev:      대상 장치
 * @sgt:      채울 sg_table
 * @cpu_addr: alloc 이 돌려준 주소
 * @dma_addr: 장치가 쓰는 주소
 * @size:     할당 크기
 * @attrs:    DMA 속성
 * @return:   0 성공, 음수 실패
 *
 * dma-buf 로 버퍼를 다른 드라이버와 공유할 때 쓴다. 할당 경로에 따라 결과가 다른데,
 * 흩어진 페이지였으면 여러 세그먼트가 되고 연속 블록이었으면 하나가 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: DMA API → [이 함수]
 */
int iommu_dma_get_sgtable(struct device *dev, struct sg_table *sgt,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	struct page *page;	/* [한국어] 단일 블록인 경우의 첫 페이지 */
	int ret;	/* [한국어] 결과 */

	if (is_vmalloc_addr(cpu_addr)) {	/* [한국어] 재매핑된 할당이면 */
		struct page **pages = dma_common_find_pages(cpu_addr);	/* [한국어] 페이지 배열을 찾는다 */

		if (pages) {	/* [한국어] 흩어진 페이지들이면 */
			return sg_alloc_table_from_pages(sgt, pages,	/* [한국어] 각 페이지를 세그먼트로 엮는다 — 인접한 것은 자동 병합된다 */
					PAGE_ALIGN(size) >> PAGE_SHIFT,	/* [한국어] 페이지 수 */
					0, size, GFP_KERNEL);	/* [한국어] 오프셋 0, 전체 길이 */
		}

		page = vmalloc_to_page(cpu_addr);	/* [한국어] 연속 블록을 재매핑한 경우 */
	} else {
		page = virt_to_page(cpu_addr);	/* [한국어] lowmem 연속 블록 */
	}

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);	/* [한국어] 연속이므로 세그먼트 하나면 충분하다 */
	if (!ret)	/* [한국어] 테이블 생성 성공 */
		sg_set_page(sgt->sgl, page, PAGE_ALIGN(size), 0);	/* [한국어] 전체 범위를 한 세그먼트로 */
	return ret;	/* [한국어] 0 이면 sgt 가 이 버퍼를 기술한다 */
}

/*
 * [한국어]
 * iommu_dma_get_merge_boundary - 상위 계층이 세그먼트를 합쳐도 되는 경계를 알린다
 *
 * @dev:    대상 장치
 * @return: IOMMU 최소 페이지 크기 - 1
 *
 * 블록 계층이 bio 를 합칠지 판단할 때 참고한다. 이 경계를 넘지 않는 한 IOMMU 가
 * 여러 페이지를 하나의 연속 IOVA 로 만들어 주므로, 장치가 보기에는 어차피 하나의
 * 구간이다. 그래서 상위에서 미리 나눌 이유가 없다.
 *
 * 이 값이 있어서 NVMe 가 큰 I/O 를 적은 수의 SGL/PRP 항목으로 보낼 수 있다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 블록 계층의 병합 판정 → [이 함수]
 */
unsigned long iommu_dma_get_merge_boundary(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 이 장치의 도메인 */

	return (1UL << __ffs(domain->pgsize_bitmap)) - 1;	/* [한국어] IOMMU 최소 페이지 크기 - 1. 블록 계층이 '이 경계를 넘지 않는 한 세그먼트를 합쳐도 IOMMU 가 하나로 매핑해 준다'고 판단하는 근거다 — NVMe 가 큰 I/O 를 적은 SGL 항목으로 보낼 수 있는 이유 */
}

/*
 * [한국어]
 * iommu_dma_opt_mapping_size - 성능상 유리한 매핑 크기의 상한
 *
 * @return: IOVA 캐시가 담을 수 있는 최대 크기 (기본 설정에서 128KB)
 *
 * 이 크기를 넘는 매핑은 IOVA 캐시를 우회해 매번 트리 락을 잡는다. 블록 계층이
 * 요청을 이 단위로 쪼개면 락 경쟁이 사라져 처리량이 크게 달라지므로, 상위에
 * 그 경계를 알려 준다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 블록 계층의 max_sectors 계산 → [이 함수]
 */
size_t iommu_dma_opt_mapping_size(void)
{
	return iova_rcache_range();	/* [한국어] IOVA 캐시가 담을 수 있는 최대 크기. 이 크기를 넘는 매핑은 매번 IOVA 트리 락을 잡으므로, 블록 계층이 요청을 이 단위로 쪼개면 처리량이 크게 달라진다 */
}

/*
 * [한국어]
 * iommu_dma_max_mapping_size - 한 번에 매핑할 수 있는 최대 크기
 *
 * @dev:    대상 장치
 * @return: 신뢰할 수 없는 장치면 바운스 버퍼 크기, 아니면 무제한
 *
 * opt_mapping_size 와 다르다. 저쪽은 "이 이상이면 느려진다"는 권고이고, 이쪽은
 * "이 이상은 불가능하다"는 한계다. 신뢰할 수 없는 장치는 모든 매핑이 바운스 버퍼를
 * 거치므로 그 버퍼 하나의 크기가 곧 상한이 된다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: DMA API, 블록 계층 → [이 함수]
 */
size_t iommu_dma_max_mapping_size(struct device *dev)
{
	if (dev_is_untrusted(dev))	/* [한국어] 신뢰할 수 없는 장치는 항상 바운스 버퍼를 거친다 */
		return swiotlb_max_mapping_size(dev);	/* [한국어] 그 버퍼 하나의 크기가 곧 상한이 된다 */

	return SIZE_MAX;	/* [한국어] 그 외에는 IOMMU 가 크기를 제한하지 않는다 */
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dma_iova_try_alloc - 여러 조각을 채워 넣을 IOVA 창을 미리 확보한다
 *
 * @dev:    대상 장치
 * @state:  창의 상태를 담을 구조체 (호출자 소유)
 * @phys:   정렬 계산용 물리 주소 (0 도 가능)
 * @size:   창의 크기
 * @return: true 면 IOVA 경로를 쓸 수 있고 창이 확보되었다
 *
 * dma_map_sg 와 다른 사용 모델을 위한 것이다. 저쪽은 리스트 전체를 한 번에 받아
 * 처리하지만, 이 API 는 창을 먼저 잡고 호출자가 조각을 하나씩 link 해 넣은 뒤
 * 마지막에 sync 를 한 번 부른다. 페이지를 모으는 과정과 매핑을 겹쳐 진행해야 하는
 * 블록 계층이나 RDMA 등록 경로에서 쓴다.
 *
 * false 를 돌려주는 것이 오류가 아니라는 점에 주의할 것 — 이 장치가 IOMMU 경로를
 * 쓰지 않는다는 뜻이며, 호출자는 일반 DMA API 로 돌아가면 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 아토믹.
 *
 * 호출 체인: 블록 계층/RDMA → [이 함수] → iommu_dma_alloc_iova
 */
bool dma_iova_try_alloc(struct device *dev, struct dma_iova_state *state,
		phys_addr_t phys, size_t size)
{
	struct iommu_dma_cookie *cookie;	/* [한국어] DMA 상태 */
	struct iommu_domain *domain;	/* [한국어] 이 장치의 도메인 */
	struct iova_domain *iovad;	/* [한국어] IOVA 공간 */
	size_t iova_off;	/* [한국어] 물리 주소의 페이지 내 오프셋 */
	dma_addr_t addr;	/* [한국어] 확보한 IOVA */

	memset(state, 0, sizeof(*state));	/* [한국어] 실패하더라도 호출자가 깨끗한 상태를 보게 한다 */
	if (!use_dma_iommu(dev))	/* [한국어] 이 장치가 IOMMU 경로를 쓰지 않는다 */
		return false;	/* [한국어] 호출자는 일반 DMA API 로 돌아간다 (위 영어 주석) */

	domain = iommu_get_dma_domain(dev);	/* [한국어] 기본 도메인 */
	cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	iova_off = iova_offset(iovad, phys);	/* [한국어] 정렬 계산에만 쓰인다 — 호출자가 항상 페이지 정렬 전송을 한다면 phys 에 0 을 넘겨도 된다 (위 영어 주석) */

	if (static_branch_unlikely(&iommu_deferred_attach_enabled) &&	/* [한국어] 지연 부착이 필요한 시스템에서만 */
	    iommu_deferred_attach(dev, iommu_get_domain_for_dev(dev)))	/* [한국어] 여기서 도메인을 건다 */
		return false;	/* [한국어] 부착 실패 — IOVA 경로를 쓸 수 없다 */

	if (WARN_ON_ONCE(!size))	/* [한국어] 길이 0 요청은 호출자 버그 */
		return false;	/* [한국어] 거절 */

	/*
	 * DMA_IOVA_USE_SWIOTLB is flag which is set by dma-iommu
	 * internals, make sure that caller didn't set it and/or
	 * didn't use this interface to map SIZE_MAX.
	 */
	if (WARN_ON_ONCE((u64)size & DMA_IOVA_USE_SWIOTLB))	/* [한국어] 이 비트는 내부 표식 자리다. 호출자가 세웠거나 SIZE_MAX 를 넘긴 경우이며, 그대로 두면 나중에 '바운스를 썼다'고 오인한다 (위 영어 주석) */
		return false;	/* [한국어] 거절 */

	addr = iommu_dma_alloc_iova(domain,	/* [한국어] 앞으로 여러 번에 걸쳐 채울 IOVA 창을 미리 한 번에 확보한다 */
			iova_align(iovad, size + iova_off),	/* [한국어] 앞 오프셋을 포함해 페이지 단위로 */
			dma_get_mask(dev), dev);	/* [한국어] 장치의 주소 상한 */
	if (!addr)	/* [한국어] IOVA 고갈 */
		return false;	/* [한국어] 호출자는 일반 경로로 돌아간다 */

	state->addr = addr + iova_off;	/* [한국어] 오프셋을 더한 실제 시작 주소 */
	state->__size = size;	/* [한국어] 이후 link/unlink 가 이 크기를 기준으로 동작한다 */
	return true;	/* [한국어] IOVA 경로를 쓸 수 있다 */
}
EXPORT_SYMBOL_GPL(dma_iova_try_alloc);	/* [한국어] 블록 계층·RDMA 처럼 큰 전송을 여러 조각으로 나눠 채우는 사용자가 부른다 */

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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dma_iova_free - 확보했던 IOVA 창을 반납한다
 *
 * @dev:   대상 장치
 * @state: 창의 상태
 *
 * 매핑은 이미 dma_iova_unlink 로 전부 지워져 있어야 한다. 그래서 gather 에 NULL 을
 * 넘기고, 무효화 없이 곧바로 반납한다.
 *
 * 모든 조각을 unlink 한 뒤 free 하는 대신, dma_iova_destroy 를 쓰면 두 동작이
 * 한 번에 처리되어 더 효율적이다 (위 영어 주석).
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 블록 계층/RDMA, dma_iova_destroy → [이 함수]
 */
void dma_iova_free(struct device *dev, struct dma_iova_state *state)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	size_t iova_start_pad = iova_offset(iovad, state->addr);	/* [한국어] 할당 때 더했던 오프셋 */
	size_t size = dma_iova_size(state);	/* [한국어] 요청했던 크기 */

	iommu_dma_free_iova(domain, state->addr - iova_start_pad,	/* [한국어] 할당 때와 정확히 같은 범위로 되돌린다 */
			iova_align(iovad, size + iova_start_pad), NULL);	/* [한국어] gather 가 NULL — 매핑은 이미 dma_iova_unlink 가 지웠으므로 무효화할 것이 없다 (위 영어 주석) */
}
EXPORT_SYMBOL_GPL(dma_iova_free);	/* [한국어] try_alloc 의 짝 */

/*
 * [한국어]
 * __dma_iova_link - 조각 하나를 창 안에 매핑한다 (동기화 없음)
 *
 * @dev:   대상 장치
 * @addr:  창 안의 목적지 IOVA
 * @phys:  매핑할 물리 주소
 * @size:  길이
 * @dir:   DMA 방향
 * @attrs: DMA 속성
 * @return: 0 성공, 음수 실패
 *
 * iommu_map_nosync 를 쓰는 것이 이 API 전체의 설계다 — 조각마다 IOTLB 동기화를
 * 하면 조각 수만큼 비용이 드는데, 마지막에 dma_iova_sync 로 한 번만 하면 된다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능 (GFP_ATOMIC).
 *
 * 호출 체인: dma_iova_link, iommu_dma_iova_link_swiotlb → [이 함수]
 */
static int __dma_iova_link(struct device *dev, dma_addr_t addr,
		phys_addr_t phys, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);	/* [한국어] 캐시 일관성 여부 */
	int prot = dma_info_to_prot(dir, coherent, attrs);	/* [한국어] 이 조각의 권한 */

	if (!coherent && !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO)))	/* [한국어] 비일관 장치이고 동기화를 생략하지 않았으면 */
		arch_sync_dma_for_device(phys, size, dir);	/* [한국어] 캐시를 밀어낸다 */

	return iommu_map_nosync(iommu_get_dma_domain(dev), addr, phys, size,	/* [한국어] nosync 인 것이 이 API 의 요점이다 — 여러 조각을 이어 붙인 뒤 마지막에 dma_iova_sync 를 한 번만 부른다 */
			prot, GFP_ATOMIC);	/* [한국어] 아토믹 문맥에서도 불릴 수 있다 */
}

/*
 * [한국어]
 * iommu_dma_iova_bounce_and_link - 정렬이 어긋난 조각을 바운스해 창에 연결한다
 *
 * @dev:            대상 장치
 * @addr:           창 안의 목적지 IOVA
 * @phys:           원본 물리 주소
 * @bounce_len:     바운스할 길이
 * @dir:            DMA 방향
 * @attrs:          DMA 속성
 * @iova_start_pad: 페이지 경계까지의 앞쪽 여백
 * @return:         0 성공, -ENOMEM 이면 바운스 실패
 *
 * 조각의 머리나 꼬리가 IOMMU 페이지 중간에 걸릴 때 쓴다. 그 페이지에는 이 조각
 * 말고 다른 데이터도 들어 있으므로 통째로 매핑할 수 없고, 전용 버퍼로 복사해
 * 그 페이지를 이 전송의 것으로 만든다.
 *
 * IOVA 와 바운스 버퍼를 같은 오프셋만큼 내려 정렬을 맞추는 것이 요점이다 —
 * swiotlb 가 그 정렬로 버퍼를 잡아 주기 때문에 성립한다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iommu_dma_iova_link_swiotlb → [이 함수]
 *            → iommu_dma_map_swiotlb, __dma_iova_link
 */
static int iommu_dma_iova_bounce_and_link(struct device *dev, dma_addr_t addr,
		phys_addr_t phys, size_t bounce_len,
		enum dma_data_direction dir, unsigned long attrs,
		size_t iova_start_pad)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 도메인 */
	struct iova_domain *iovad = &domain->iova_cookie->iovad;	/* [한국어] IOVA 입도 */
	phys_addr_t bounce_phys;	/* [한국어] 바운스 버퍼의 물리 주소 */
	int error;	/* [한국어] 결과 */

	bounce_phys = iommu_dma_map_swiotlb(dev, phys, bounce_len, dir, attrs);	/* [한국어] 페이지 경계에 걸친 조각을 전용 버퍼로 복사한다 */
	if (bounce_phys == DMA_MAPPING_ERROR)	/* [한국어] 바운스 실패 */
		return -ENOMEM;	/* [한국어] 이 조각을 매핑할 수 없다 */

	error = __dma_iova_link(dev, addr - iova_start_pad,	/* [한국어] IOVA 도 페이지 경계로 내려 맞춘다 */
			bounce_phys - iova_start_pad,	/* [한국어] 바운스 버퍼도 같은 오프셋만큼 내린다 — 버퍼가 그 정렬로 잡혀 있다 */
			iova_align(iovad, bounce_len), dir, attrs);	/* [한국어] 페이지 단위 길이 */
	if (error)	/* [한국어] 매핑 실패 */
		swiotlb_tbl_unmap_single(dev, bounce_phys, bounce_len, dir,	/* [한국어] 잡아 둔 바운스 버퍼를 되돌린다 */
				attrs);	/* [한국어] 속성 그대로 */
	return error;	/* [한국어] 0 이면 이 조각이 연결되었다 */
}

/*
 * [한국어]
 * iommu_dma_iova_link_swiotlb - 머리와 꼬리만 바운스하고 가운데는 직접 매핑한다
 *
 * @dev:    대상 장치
 * @state:  창의 상태
 * @phys:   조각의 물리 주소
 * @offset: 창 안의 위치
 * @size:   조각의 길이
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 * @return: 0 성공, 음수 실패
 *
 * 복사 비용을 최소화하는 것이 이 함수의 목적이다. 조각 전체를 바운스하면 큰 버퍼도
 * 통째로 복사해야 하지만, 실제로 위험한 것은 페이지 경계에 걸친 앞뒤 조각뿐이다.
 * 가운데의 완전한 페이지들은 다른 데이터가 섞이지 않으므로 그대로 매핑해도 된다.
 *
 * 바운스를 한 번이라도 쓰면 state 에 표식을 남긴다. 해제 경로가 그 표식을 보고
 * 페이지 단위 느린 경로로 들어가 바운스 버퍼를 되돌린다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: dma_iova_link → [이 함수]
 *            → iommu_dma_iova_bounce_and_link, __dma_iova_link
 */
static int iommu_dma_iova_link_swiotlb(struct device *dev,
		struct dma_iova_state *state, phys_addr_t phys, size_t offset,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	size_t iova_start_pad = iova_offset(iovad, phys);	/* [한국어] 조각의 앞쪽이 페이지 중간에서 시작하는가 */
	size_t iova_end_pad = iova_offset(iovad, phys + size);	/* [한국어] 조각의 뒤쪽이 페이지 중간에서 끝나는가 */
	dma_addr_t addr = state->addr + offset;	/* [한국어] 이 조각이 들어갈 IOVA 위치 */
	size_t mapped = 0;	/* [한국어] 지금까지 연결한 길이 */
	int error;	/* [한국어] 결과 */

	if (iova_start_pad) {	/* [한국어] 앞쪽이 페이지 경계에 맞지 않는다 — 그 페이지에는 이 조각 말고 다른 데이터도 들어 있다 */
		size_t bounce_len = min(size, iovad->granule - iova_start_pad);	/* [한국어] 페이지 끝까지, 또는 조각 전체 중 짧은 쪽 */

		error = iommu_dma_iova_bounce_and_link(dev, addr, phys,	/* [한국어] 그만큼만 바운스 버퍼로 복사해 연결한다 */
				bounce_len, dir, attrs, iova_start_pad);	/* [한국어] 앞 패딩 길이 */
		if (error)	/* [한국어] 실패 */
			return error;	/* [한국어] 아직 연결한 것이 없으므로 되감을 것도 없다 */
		state->__size |= DMA_IOVA_USE_SWIOTLB;	/* [한국어] 해제 경로가 바운스 버퍼도 돌려줘야 함을 알리는 표식 */

		mapped += bounce_len;	/* [한국어] 진행 길이 갱신 */
		size -= bounce_len;	/* [한국어] 남은 길이 */
		if (!size)	/* [한국어] 조각 전체가 앞 패딩 안에 들어갔다 */
			return 0;	/* [한국어] 연결 완료 */
	}

	size -= iova_end_pad;	/* [한국어] 뒤쪽 패딩을 뺀 '완전한 페이지들'만 먼저 처리한다 */
	error = __dma_iova_link(dev, addr + mapped, phys + mapped, size, dir,	/* [한국어] 가운데의 정렬된 본문은 바운스 없이 직접 매핑한다 — 복사 비용을 최소한으로 줄이는 것이 이 함수의 목적이다 */
			attrs);	/* [한국어] 속성 그대로 */
	if (error)	/* [한국어] 실패 */
		goto out_unmap;	/* [한국어] 앞에서 연결한 것까지 되돌린다 */
	mapped += size;	/* [한국어] 진행 길이 갱신 */

	if (iova_end_pad) {	/* [한국어] 뒤쪽이 페이지 중간에서 끝난다 */
		error = iommu_dma_iova_bounce_and_link(dev, addr + mapped,	/* [한국어] 그 꼬리도 바운스로 */
				phys + mapped, iova_end_pad, dir, attrs, 0);	/* [한국어] 앞 패딩은 없으므로 0 */
		if (error)	/* [한국어] 실패 */
			goto out_unmap;	/* [한국어] 되감기 */
		state->__size |= DMA_IOVA_USE_SWIOTLB;	/* [한국어] 바운스 사용 표식 */
	}

	return 0;	/* [한국어] 조각 전체가 연결되었다 */

out_unmap:	/* [한국어] 부분 성공 되감기 */
	dma_iova_unlink(dev, state, 0, mapped, dir, attrs);	/* [한국어] 연결한 만큼만 정확히 해제한다 */
	return error;	/* [한국어] 실패 이유 */
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dma_iova_link - 창 안의 한 위치에 물리 구간을 연결한다
 *
 * @dev:    대상 장치
 * @state:  try_alloc 이 만든 창의 상태
 * @phys:   연결할 물리 주소
 * @offset: 창 안의 위치
 * @size:   길이
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 * @return: 0 성공, 음수 실패
 *
 * IOTLB 동기화를 하지 않는 것이 이 API 의 요점이다. 여러 번 부른 뒤 마지막에
 * dma_iova_sync 를 한 번만 부르면 되므로, 조각이 많을수록 이득이 크다.
 *
 * 첫 검사가 이 API 의 제약을 드러낸다 — 정렬이 어긋난 조각은 창의 맨 앞에만 올 수
 * 있다. 중간에 오면 앞 조각과 같은 IOMMU 페이지를 공유하게 되어 서로의 매핑을
 * 덮어쓰기 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: 블록 계층/RDMA → [이 함수]
 *            → iommu_dma_iova_link_swiotlb 또는 __dma_iova_link
 */
int dma_iova_link(struct device *dev, struct dma_iova_state *state,
		phys_addr_t phys, size_t offset, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	size_t iova_start_pad = iova_offset(iovad, phys);	/* [한국어] 이 조각의 앞쪽 정렬 어긋남 */

	if (WARN_ON_ONCE(iova_start_pad && offset > 0))	/* [한국어] 창 중간에 정렬이 어긋난 조각을 넣으려 한다. 그러면 앞 조각의 페이지와 겹쳐 서로의 매핑을 덮어쓴다 */
		return -EIO;	/* [한국어] 호출자 사용법 오류 */

	/*
	 * DMA_IOVA_USE_SWIOTLB is set on state after some entry
	 * took SWIOTLB path, which we were supposed to prevent
	 * for DMA_ATTR_REQUIRE_COHERENT attribute.
	 */
	if (WARN_ON_ONCE((state->__size & DMA_IOVA_USE_SWIOTLB) &&	/* [한국어] 이미 어떤 조각이 바운스 경로를 탔는데 */
			 (attrs & DMA_ATTR_REQUIRE_COHERENT)))	/* [한국어] 호출자가 coherent 를 요구한다 — 바운스 버퍼는 그 요구를 만족시킬 수 없다 (위 영어 주석) */
		return -EOPNOTSUPP;	/* [한국어] 모순된 요청 */

	if (!dev_is_dma_coherent(dev) && (attrs & DMA_ATTR_REQUIRE_COHERENT))	/* [한국어] 비일관 장치에 coherent 를 요구했다 */
		return -EOPNOTSUPP;	/* [한국어] 하드웨어가 제공할 수 없다 */

	if (dev_use_swiotlb(dev, size, dir) &&	/* [한국어] 바운스가 필요한 장치이고 */
	    iova_unaligned(iovad, phys, size)) {	/* [한국어] 실제로 정렬이 어긋났다면 */
		if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT))	/* [한국어] MMIO 나 coherent 요구는 바운스로 처리할 수 없다 */
			return -EPERM;	/* [한국어] 거절 */

		return iommu_dma_iova_link_swiotlb(dev, state, phys, offset,	/* [한국어] 앞뒤 꼬리만 바운스하고 가운데는 직접 매핑하는 경로로 */
				size, dir, attrs);	/* [한국어] 조각의 크기와 방향 */
	}

	return __dma_iova_link(dev, state->addr + offset - iova_start_pad,	/* [한국어] 정렬이 맞으면 곧바로 매핑한다 */
			phys - iova_start_pad,	/* [한국어] 물리 주소도 페이지 경계로 */
			iova_align(iovad, size + iova_start_pad), dir, attrs);	/* [한국어] 페이지 단위 길이 */
}
EXPORT_SYMBOL_GPL(dma_iova_link);	/* [한국어] 여러 조각을 하나의 IOVA 창에 이어 붙이는 사용자가 부른다 */

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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dma_iova_sync - 여러 번의 link 를 한 번에 하드웨어에 반영한다
 *
 * @dev:    대상 장치
 * @state:  창의 상태
 * @offset: 동기화할 구간의 시작
 * @size:   길이
 * @return: 0 성공, 음수 실패
 *
 * link 가 미뤄 둔 두 가지를 여기서 처리한다 — 캐시 쓰기 완료 대기와 IOTLB 반영이다.
 * 조각마다 하면 조각 수만큼 비용이 드는 일을 한 번으로 줄이는 것이 이 API 계열의
 * 존재 이유다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 블록 계층/RDMA → [이 함수] → iommu_sync_map
 */
int dma_iova_sync(struct device *dev, struct dma_iova_state *state,
		size_t offset, size_t size)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	dma_addr_t addr = state->addr + offset;	/* [한국어] 동기화할 구간의 시작 */
	size_t iova_start_pad = iova_offset(iovad, addr);	/* [한국어] 페이지 경계로 내릴 오프셋 */

	if (!dev_is_dma_coherent(dev))	/* [한국어] 비일관 장치면 */
		arch_sync_dma_flush();	/* [한국어] 앞서 link 마다 낸 캐시 쓰기를 여기서 한 번에 완료시킨다 — 조각마다 기다리지 않는 것이 이 API 의 이득이다 */
	return iommu_sync_map(domain, addr - iova_start_pad,	/* [한국어] 여러 번의 nosync 매핑을 여기서 한 번에 하드웨어에 반영한다 */
		      iova_align(iovad, size + iova_start_pad));	/* [한국어] 페이지 단위 범위 */
}
EXPORT_SYMBOL_GPL(dma_iova_sync);	/* [한국어] link 를 여러 번 부른 뒤 마지막에 한 번 */

/*
 * [한국어]
 * iommu_dma_iova_unlink_range_slow - 페이지 단위로 훑으며 바운스와 캐시를 정리한다
 *
 * @dev:   대상 장치
 * @addr:  구간의 시작 IOVA
 * @size:  길이
 * @dir:   DMA 방향
 * @attrs: DMA 속성
 *
 * 이름의 slow 가 정확하다 — 페이지마다 IOVA→물리 역변환을 하고 swiotlb 반납을
 * 시도한다. 창 안의 어느 페이지가 바운스 버퍼인지 기록해 두지 않기 때문에 하나씩
 * 확인할 수밖에 없다.
 *
 * swiotlb_tbl_unmap_single 이 바운스 버퍼가 아닌 주소에 대해 무해하게 지나간다는
 * 성질을 이용해, 확인 없이 모든 페이지에 대해 부른다.
 *
 * 그래서 호출자는 바운스를 썼거나 캐시 동기화가 필요한 경우에만 이 경로를 탄다 —
 * 둘 다 아니면 이 비용을 완전히 건너뛴다.
 *
 * 실행 컨텍스트: 해제 경로. 어디서든.
 *
 * 호출 체인: __iommu_dma_iova_unlink → [이 함수]
 */
static void iommu_dma_iova_unlink_range_slow(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	size_t iova_start_pad = iova_offset(iovad, addr);	/* [한국어] 첫 페이지의 오프셋 */
	bool need_sync_dma = !dev_is_dma_coherent(dev) &&	/* [한국어] 비일관 장치이고 */
			!(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO));	/* [한국어] 동기화를 생략하지 않았으면 마지막에 완료 대기가 필요하다 */
	dma_addr_t end = addr + size;	/* [한국어] 구간의 끝 */

	do {	/* [한국어] 페이지 단위로 훑는다 — 어느 페이지가 바운스 버퍼인지 미리 알 수 없어 하나씩 확인해야 한다 */
		phys_addr_t phys;	/* [한국어] 이 페이지의 물리 주소 */
		size_t len;	/* [한국어] 이번 회차에 처리할 길이 */

		phys = iommu_iova_to_phys(domain, addr);	/* [한국어] 매핑을 지우기 전에 역변환한다 */
		if (WARN_ON(!phys))	/* [한국어] 매핑이 없다 — link 와 unlink 의 범위가 어긋났다는 뜻 */
			/* Something very horrible happen here */
			return;	/* [한국어] 더 진행하면 엉뚱한 메모리를 만진다 (위 영어 주석: 매우 나쁜 일이 일어난 것이다) */

		len = min_t(size_t,	/* [한국어] 이 페이지 안에서 처리할 길이 */
			end - addr, iovad->granule - iova_start_pad);	/* [한국어] 구간의 남은 길이와 페이지 끝까지 중 짧은 쪽 */

		if (!dev_is_dma_coherent(dev) &&	/* [한국어] 비일관 장치이고 */
		    !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO)))	/* [한국어] 동기화를 생략하지 않았으면 */
			arch_sync_dma_for_cpu(phys, len, dir);	/* [한국어] 장치가 쓴 내용을 CPU 가 보도록 캐시 무효화 */

		swiotlb_tbl_unmap_single(dev, phys, len, dir, attrs);	/* [한국어] 바운스 버퍼였다면 내용을 되복사하고 반납한다. 아니면 무해하게 지나간다 — 그래서 페이지마다 확인 없이 부를 수 있다 */

		addr += len;	/* [한국어] 다음 페이지로 */
		iova_start_pad = 0;	/* [한국어] 첫 페이지 이후로는 오프셋이 없다 */
	} while (addr < end);	/* [한국어] 구간 끝까지 */

	if (need_sync_dma)	/* [한국어] 캐시를 만졌으면 */
		arch_sync_dma_flush();	/* [한국어] 한 번만 완료 대기 */
}

/*
 * [한국어]
 * __iommu_dma_iova_unlink - 창의 일부 또는 전부를 해제한다
 *
 * @dev:       대상 장치
 * @state:     창의 상태
 * @offset:    해제할 구간의 시작 위치
 * @size:      길이
 * @dir:       DMA 방향
 * @attrs:     DMA 속성
 * @free_iova: true 면 IOVA 창까지 반납한다 (destroy 경로)
 *
 * unlink 와 destroy 를 하나로 합친 구현이다. 차이는 free_iova 하나이며, 그 값이
 * 지연 무효화 사용 여부까지 결정한다 — IOVA 를 반납하지 않는 unlink 는 큐를 쓸 수
 * 없다. 큐는 IOVA 반납과 무효화를 함께 미루는 장치이기 때문이다.
 *
 * 느린 경로 진입 조건이 두 가지인 것에 주목할 것. 바운스 버퍼를 되돌려야 하거나,
 * 비일관 장치의 캐시를 무효화해야 할 때만 페이지 단위로 훑는다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: dma_iova_unlink, dma_iova_destroy → [이 함수]
 *            → iommu_unmap_fast, iommu_dma_free_iova
 */
static void __iommu_dma_iova_unlink(struct device *dev,
		struct dma_iova_state *state, size_t offset, size_t size,
		enum dma_data_direction dir, unsigned long attrs,
		bool free_iova)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);	/* [한국어] 도메인 */
	struct iommu_dma_cookie *cookie = domain->iova_cookie;	/* [한국어] DMA 상태 */
	struct iova_domain *iovad = &cookie->iovad;	/* [한국어] IOVA 공간 */
	dma_addr_t addr = state->addr + offset;	/* [한국어] 해제할 구간의 시작 */
	size_t iova_start_pad = iova_offset(iovad, addr);	/* [한국어] 페이지 경계로 내릴 오프셋 */
	struct iommu_iotlb_gather iotlb_gather;	/* [한국어] 무효화 범위 수집기 */
	size_t unmapped;	/* [한국어] 실제 해제 바이트 */

	if ((state->__size & DMA_IOVA_USE_SWIOTLB) ||	/* [한국어] 바운스 버퍼가 섞여 있거나 */
	    (!dev_is_dma_coherent(dev) &&	/* [한국어] 비일관 장치이고 */
	     !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO))))	/* [한국어] 동기화가 필요하면 */
		iommu_dma_iova_unlink_range_slow(dev, addr, size, dir, attrs);	/* [한국어] 페이지 단위로 훑는 느린 경로를 먼저 거친다. 둘 다 아니면 이 비용을 완전히 건너뛴다 */

	iommu_iotlb_gather_init(&iotlb_gather);	/* [한국어] 빈 수집기 */
	iotlb_gather.queued = free_iova && READ_ONCE(cookie->fq_domain);	/* [한국어] IOVA 를 반납하지 않는 unlink 는 지연 무효화를 쓸 수 없다 — 큐는 IOVA 반납과 무효화를 함께 미루는 장치이기 때문이다 */

	size = iova_align(iovad, size + iova_start_pad);	/* [한국어] 페이지 단위 범위로 */
	addr -= iova_start_pad;	/* [한국어] 시작도 페이지 경계로 */
	unmapped = iommu_unmap_fast(domain, addr, size, &iotlb_gather);	/* [한국어] PTE 제거 */
	WARN_ON(unmapped != size);	/* [한국어] link 와 unlink 의 범위가 어긋났다 */

	if (!iotlb_gather.queued)	/* [한국어] 지연 무효화가 아니면 */
		iommu_iotlb_sync(domain, &iotlb_gather);	/* [한국어] 여기서 IOTLB 를 비운다 */
	if (free_iova)	/* [한국어] destroy 경로면 */
		iommu_dma_free_iova(domain, addr, size, &iotlb_gather);	/* [한국어] IOVA 창까지 반납한다. unlink 경로는 창을 남겨 두어 다시 채울 수 있게 한다 */
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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dma_iova_unlink - 창의 일부 매핑을 지운다 (창 자체는 유지)
 *
 * @dev:    대상 장치
 * @state:  창의 상태
 * @offset: 지울 구간의 시작
 * @size:   길이
 * @dir:    DMA 방향
 * @attrs:  DMA 속성
 *
 * 창을 남겨 두므로 같은 자리에 다른 내용을 다시 link 할 수 있다. RDMA 메모리 영역
 * 재등록처럼 주소는 유지하고 내용만 바꾸는 사용 사례를 위한 것이다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 블록 계층/RDMA → [이 함수] → __iommu_dma_iova_unlink
 */
void dma_iova_unlink(struct device *dev, struct dma_iova_state *state,
		size_t offset, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	 __iommu_dma_iova_unlink(dev, state, offset, size, dir, attrs, false);	/* [한국어] IOVA 창은 그대로 두고 매핑만 지운다 — 같은 창에 다른 내용을 다시 이어 붙일 수 있다 */
}
EXPORT_SYMBOL_GPL(dma_iova_unlink);	/* [한국어] 조각 단위 해제 */

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
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dma_iova_destroy - 매핑을 지우고 창까지 한 번에 반납한다
 *
 * @dev:        대상 장치
 * @state:      창의 상태
 * @mapped_len: 실제로 연결된 길이
 * @dir:        DMA 방향
 * @attrs:      DMA 속성
 *
 * unlink 후 free 를 따로 부르는 것보다 효율적이다. 해제와 IOVA 반납을 한 번의
 * 무효화 안에서 처리하기 때문이며, 특히 지연 무효화를 쓸 때 차이가 크다.
 *
 * mapped_len 이 0 인 경우를 따로 다루는 것은 첫 link 부터 실패한 상황을 위한 것이다.
 * 지울 매핑이 없으므로 창만 반납한다 (위 영어 주석).
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 블록 계층/RDMA → [이 함수] → __iommu_dma_iova_unlink 또는 dma_iova_free
 */
void dma_iova_destroy(struct device *dev, struct dma_iova_state *state,
		size_t mapped_len, enum dma_data_direction dir,
		unsigned long attrs)
{
	if (mapped_len)	/* [한국어] 연결된 것이 있으면 */
		__iommu_dma_iova_unlink(dev, state, 0, mapped_len, dir, attrs,	/* [한국어] 매핑 해제와 */
				true);	/* [한국어] IOVA 반납을 한 번에 — 그래서 unlink + free 두 번보다 효율적이다 (위 영어 주석) */
	else
		/*
		 * We can be here if first call to dma_iova_link() failed and
		 * there is nothing to unlink, so let's be more clear.
		 */
		dma_iova_free(dev, state);	/* [한국어] 첫 link 부터 실패해 지울 매핑이 없는 경우. 창만 반납한다 (위 영어 주석) */
}
EXPORT_SYMBOL_GPL(dma_iova_destroy);	/* [한국어] link 을 여러 번 한 뒤 한 번에 정리하는 종료 경로 */

/*
 * [한국어]
 * iommu_setup_dma_ops - 장치의 DMA 를 이 파일의 구현으로 갈아 끼운다
 *
 * @dev:    대상 장치
 * @domain: 이 장치가 붙은 도메인
 *
 * 이 파일 전체가 언제부터 동작하는지를 정하는 함수다. dev->dma_iommu 를 세우는
 * 한 줄이 지나면 그 장치의 dma_map_page 는 물리 주소가 아니라 IOVA 를 돌려주기
 * 시작한다 — 드라이버 입장에서 "IOMMU 가 켜졌다"의 실제 의미가 그것이다.
 *
 * 실패해도 장치를 못 쓰게 만들지 않는다. dma_iommu 를 다시 내려 플랫폼 기본
 * 경로(직접 매핑 또는 swiotlb)로 되돌리고 경고만 남긴다 — 격리는 잃지만 동작은
 * 한다는 선택이다.
 *
 * 실행 컨텍스트: 장치 프로브 경로. 그룹 락 아래. 프로세스 문맥.
 *
 * 호출 체인: iommu.c 의 __iommu_probe_device, bus_iommu_probe → [이 함수]
 *            → iommu_dma_init_domain
 */
void iommu_setup_dma_ops(struct device *dev, struct iommu_domain *domain)
{
	if (dev_is_pci(dev))	/* [한국어] PCI 장치면 */
		dev->iommu->pci_32bit_workaround = !iommu_dma_forcedac;	/* [한국어] forcedac 를 켜지 않았으면 32비트 우선 할당을 시도한다. 이 플래그가 IOVA 배정 정책의 시작점이다 */

	dev->dma_iommu = iommu_is_dma_domain(domain);	/* [한국어] 번역 도메인이면 이 장치의 DMA 를 IOMMU 경로로 돌린다. 이 한 줄이 dma_map_* 의 목적지를 바꾼다 */
	if (dev->dma_iommu && iommu_dma_init_domain(domain, dev))	/* [한국어] IOVA 공간과 예약 구간을 세운다 */
		goto out_err;	/* [한국어] 세우지 못했다 */

	return;	/* [한국어] 설정 완료 — 이제 이 장치의 DMA 는 IOMMU 를 지난다 */
out_err:	/* [한국어] 초기화 실패 경로 */
	pr_warn("Failed to set up IOMMU for device %s; retaining platform DMA ops\n",	/* [한국어] 조용히 넘어가면 장치가 왜 느린지 알 수 없다 */
		dev_name(dev));	/* [한국어] 문제의 장치 */
	dev->dma_iommu = false;	/* [한국어] 플랫폼 기본 DMA 경로(직접 매핑 또는 swiotlb)로 되돌린다. 격리는 잃지만 장치는 동작한다 */
}

/*
 * [한국어]
 * has_msi_cookie - 이 도메인이 MSI 매핑을 관리하는가
 *
 * @domain: 확인할 도메인
 * @return: true 면 MSI 매핑 목록이 존재한다
 *
 * 두 종류의 쿠키가 모두 MSI 목록을 갖고 있다 — 커널 DMA API 용 전체 쿠키와,
 * VFIO 등이 MSI 재매핑만 맡길 때 쓰는 축소판이다. 아래의 접근자들이 BUG() 로
 * 끝나지 않으려면 호출 전에 이 검사를 거쳐야 한다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iommu_dma_sw_msi → [이 함수]
 */
static bool has_msi_cookie(const struct iommu_domain *domain)
{
	return domain && (domain->cookie_type == IOMMU_COOKIE_DMA_IOVA ||	/* [한국어] 완전한 DMA 쿠키이거나 */
			  domain->cookie_type == IOMMU_COOKIE_DMA_MSI);	/* [한국어] MSI 전용 축소판 쿠키. 둘 중 하나여야 MSI 매핑 목록이 존재한다 */
}

/*
 * [한국어]
 * cookie_msi_granule - MSI 매핑의 단위 크기
 *
 * @domain: 대상 도메인
 * @return: 매핑 단위 (바이트)
 *
 * 전체 쿠키는 IOVA 공간의 입도를 쓰고, 축소판 쿠키는 IOVA 공간이 없어 CPU 페이지
 * 크기를 쓴다. 이 값이 도어벨 주소를 어디까지 내림할지, 매핑을 몇 바이트로 만들지를
 * 결정한다.
 *
 * default 에서 BUG() 인 것은 has_msi_cookie 검사를 건너뛴 호출자를 잡기 위해서다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iommu_dma_get_msi_page, iommu_dma_sw_msi → [이 함수]
 */
static size_t cookie_msi_granule(const struct iommu_domain *domain)
{
	switch (domain->cookie_type) {	/* [한국어] 쿠키 종류에 따라 MSI 매핑의 단위가 다르다 */
	case IOMMU_COOKIE_DMA_IOVA:	/* [한국어] 완전한 DMA 쿠키 */
		return domain->iova_cookie->iovad.granule;	/* [한국어] IOVA 공간의 입도를 그대로 쓴다 */
	case IOMMU_COOKIE_DMA_MSI:	/* [한국어] 축소판 쿠키에는 IOVA 공간이 없다 */
		return PAGE_SIZE;	/* [한국어] CPU 페이지 크기로 대신한다 */
	default:	/* [한국어] MSI 쿠키가 없는 도메인 — 호출자가 has_msi_cookie 검사를 건너뛴 것 */
		BUG();	/* [한국어] MSI 쿠키가 없는 도메인에서 불렸다 = 호출자가 has_msi_cookie 검사를 건너뛴 것 */
	}
}

/*
 * [한국어]
 * cookie_msi_pages - 이 도메인의 MSI 매핑 목록을 얻는다
 *
 * @domain: 대상 도메인
 * @return: 매핑 기록들의 목록 머리
 *
 * 두 쿠키 형태에서 목록의 위치가 달라 여기서 갈라 준다. granule 과 같은 이유로
 * default 는 BUG() 다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iommu_dma_get_msi_page → [이 함수]
 */
static struct list_head *cookie_msi_pages(const struct iommu_domain *domain)
{
	switch (domain->cookie_type) {	/* [한국어] 쿠키 종류에 따라 목록의 위치가 다르다 */
	case IOMMU_COOKIE_DMA_IOVA:	/* [한국어] 완전한 DMA 쿠키 */
		return &domain->iova_cookie->msi_page_list;	/* [한국어] 큰 쿠키 안의 목록 */
	case IOMMU_COOKIE_DMA_MSI:	/* [한국어] 축소판 쿠키 */
		return &domain->msi_cookie->msi_page_list;	/* [한국어] 작은 쿠키 안의 목록 */
	default:	/* [한국어] 같은 이유 */
		BUG();	/* [한국어] MSI 쿠키가 없는 도메인 */
	}
}

/*
 * [한국어]
 * iommu_dma_get_msi_page - MSI 도어벨 페이지의 매핑을 찾거나 만든다
 *
 * @dev:      MSI 를 설정할 장치
 * @msi_addr: 도어벨의 물리 주소
 * @domain:   그 장치의 도메인
 * @return:   매핑 기록, 실패하면 NULL
 *
 * MSI 는 약속된 주소로의 메모리 쓰기이므로, IOMMU 아래의 장치는 그 주소도 번역을
 * 거친다. 도어벨의 물리 주소를 이 도메인의 IOVA 공간에 매핑해 두고, 장치에는 그
 * IOVA 를 프로그래밍해야 인터럽트가 전달된다.
 *
 * 페이지 단위로 캐시하는 것이 요점이다. 한 시스템의 수천 개 MSI 벡터가 대개 같은
 * 도어벨 페이지를 쓰므로, 벡터마다 IOVA 를 새로 떼면 주소 공간이 금세 소모된다.
 *
 * 권한이 쓰기 전용인 것도 의도적이다 — 장치가 도어벨을 읽을 이유가 없고, 읽기를
 * 막아 두면 그 매핑으로 할 수 있는 일이 인터럽트 발생 하나로 제한된다.
 *
 * 실행 컨텍스트: MSI 설정 경로. 그룹 락 아래. 프로세스 문맥.
 *
 * 호출 체인: iommu_dma_sw_msi → [이 함수] → iommu_dma_alloc_iova, iommu_map
 */
static struct iommu_dma_msi_page *iommu_dma_get_msi_page(struct device *dev,
		phys_addr_t msi_addr, struct iommu_domain *domain)
{
	struct list_head *msi_page_list = cookie_msi_pages(domain);	/* [한국어] 이 도메인의 MSI 매핑 목록 */
	struct iommu_dma_msi_page *msi_page;	/* [한국어] 찾거나 만들 매핑 기록 */
	dma_addr_t iova;	/* [한국어] 도어벨을 매핑할 IOVA */
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;	/* [한국어] 쓰기만 허용한다 — MSI 는 도어벨에 값을 쓰는 동작이고 읽을 이유가 없다. NOEXEC 와 MMIO 는 이 매핑이 코드도 메모리도 아님을 하드웨어에 알린다 */
	size_t size = cookie_msi_granule(domain);	/* [한국어] 매핑 단위 (한 페이지) */

	msi_addr &= ~(phys_addr_t)(size - 1);	/* [한국어] 도어벨 주소를 페이지 경계로 내린다 — 같은 페이지의 여러 도어벨이 하나의 매핑을 공유한다 */
	list_for_each_entry(msi_page, msi_page_list, list)	/* [한국어] 이미 매핑해 둔 것이 있는지 */
		if (msi_page->phys == msi_addr)	/* [한국어] 같은 페이지면 */
			return msi_page;	/* [한국어] 재사용한다. 벡터마다 IOVA 를 새로 떼면 주소 공간이 빠르게 소모된다 */

	msi_page = kzalloc_obj(*msi_page);	/* [한국어] 새 매핑 기록 */
	if (!msi_page)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] MSI 를 설정할 수 없다 */

	iova = iommu_dma_alloc_iova(domain, size, dma_get_mask(dev), dev);	/* [한국어] 도어벨용 IOVA 한 페이지 */
	if (!iova)	/* [한국어] IOVA 고갈 */
		goto out_free_page;	/* [한국어] 기록 반납 */

	if (iommu_map(domain, iova, msi_addr, size, prot, GFP_KERNEL))	/* [한국어] 그 IOVA 에 도어벨의 물리 주소를 매핑한다. 이 매핑이 있어야 장치가 낸 쓰기가 인터럽트 컨트롤러에 도달한다 */
		goto out_free_iova;	/* [한국어] 매핑 실패 */

	INIT_LIST_HEAD(&msi_page->list);	/* [한국어] 목록 고리 초기화 */
	msi_page->phys = msi_addr;	/* [한국어] 재사용 판정 키 */
	msi_page->iova = iova;	/* [한국어] 장치에 프로그래밍할 주소 */
	list_add(&msi_page->list, msi_page_list);	/* [한국어] 도메인 목록에 등록 — 다음 벡터가 이것을 찾아 쓴다 */
	return msi_page;	/* [한국어] 매핑 완료 */

out_free_iova:	/* [한국어] 매핑 실패 경로 */
	iommu_dma_free_iova(domain, iova, size, NULL);	/* [한국어] IOVA 반납 */
out_free_page:	/* [한국어] IOVA 확보 실패가 합류 */
	kfree(msi_page);	/* [한국어] 기록 반납 */
	return NULL;	/* [한국어] MSI 설정 실패 */
}

/*
 * [한국어]
 * iommu_dma_sw_msi - MSI 서술자에 번역 가능한 도어벨 주소를 채워 준다
 *
 * @domain:   장치가 붙은 도메인
 * @desc:     MSI 서술자 (결과를 여기에 기록한다)
 * @msi_addr: 인터럽트 컨트롤러가 알려 준 도어벨 물리 주소
 * @return:   0 성공, -ENOMEM 이면 매핑 실패
 *
 * iommu.c 의 iommu_dma_prepare_msi 가 도메인 종류를 보고 이쪽 또는 iommufd 쪽으로
 * 갈라 부른다. 이 함수는 커널이 IOVA 를 관리하는 경우를 맡는다.
 *
 * MSI 매핑을 관리하지 않는 도메인이면 IOVA 0 을 돌려주는데, 그것이 "번역 없이
 * 원래 물리 주소를 쓰라"는 신호다 — 항등 도메인이 그런 경우다.
 *
 * 실행 컨텍스트: MSI 설정 경로. 그룹 락 아래(assert 로 확인). 프로세스 문맥.
 *
 * 호출 체인: iommu.c 의 iommu_dma_prepare_msi → [이 함수]
 *            → iommu_dma_get_msi_page
 */
int iommu_dma_sw_msi(struct iommu_domain *domain, struct msi_desc *desc,
		     phys_addr_t msi_addr)
{
	struct device *dev = msi_desc_to_dev(desc);	/* [한국어] 이 MSI 를 쓰는 장치 */
	const struct iommu_dma_msi_page *msi_page;	/* [한국어] 매핑 기록 */

	if (!has_msi_cookie(domain)) {	/* [한국어] MSI 매핑을 관리하지 않는 도메인 (항등 도메인 등) */
		msi_desc_set_iommu_msi_iova(desc, 0, 0);	/* [한국어] 번역이 없으니 원래 물리 주소를 그대로 쓰라고 알린다 */
		return 0;	/* [한국어] 할 일 없음 */
	}

	iommu_group_mutex_assert(dev);	/* [한국어] 목록 조작은 그룹 락 아래에서만 — iommu_dma_prepare_msi 가 그 락을 들고 부른다 */
	msi_page = iommu_dma_get_msi_page(dev, msi_addr, domain);	/* [한국어] 도어벨 매핑을 찾거나 만든다 */
	if (!msi_page)	/* [한국어] 실패 */
		return -ENOMEM;	/* [한국어] MSI 를 설정할 수 없다 */

	msi_desc_set_iommu_msi_iova(desc, msi_page->iova,	/* [한국어] 인터럽트 코어에 IOVA 를 돌려준다. 이 값이 장치의 MSI 주소 레지스터에 쓰인다 */
				    ilog2(cookie_msi_granule(domain)));	/* [한국어] 페이지 크기의 로그값 — 코어가 페이지 내 오프셋을 더해 최종 주소를 만든다 */
	return 0;	/* [한국어] MSI 준비 완료 */
}

/*
 * [한국어]
 * iommu_dma_init - 이 파일의 부팅 초기화
 *
 * @return: 0 성공, 음수면 IOVA 슬랩 준비 실패
 *
 * 하는 일은 두 가지다. IOVA 슬랩을 준비하고(이후 만들어지는 모든 도메인이 공유),
 * kdump 커널이면 지연 부착을 켠다.
 *
 * kdump 조건이 미묘하다. 크래시 덤프 커널은 앞선 커널이 남긴 하드웨어 상태 위에서
 * 부팅하는데, 그때 도메인을 곧바로 걸면 아직 진행 중인 DMA(예: 덤프를 쓸 디스크
 * 컨트롤러의 동작)가 끊긴다. 첫 DMA 시점까지 부착을 미루면 그 전환이 안전한
 * 순간에 일어난다.
 *
 * arch_initcall 이라 IOMMU 드라이버 초기화보다 먼저 돈다.
 *
 * 실행 컨텍스트: 부팅 초기 initcall. 프로세스 문맥.
 *
 * 호출 체인: arch_initcall → [이 함수] → iova_cache_get
 */
static int iommu_dma_init(void)
{
	if (is_kdump_kernel())	/* [한국어] 크래시 덤프 커널이다 */
		static_branch_enable(&iommu_deferred_attach_enabled);	/* [한국어] 앞선 커널이 남긴 매핑 위에서 부팅하므로, 도메인을 곧바로 걸면 아직 살아 있는 DMA 가 끊긴다. 첫 DMA 시점까지 부착을 미루는 정적 키를 켠다 */

	return iova_cache_get();	/* [한국어] IOVA 슬랩을 준비한다 — 이후 만들어지는 모든 도메인이 이것을 공유한다 */
}
arch_initcall(iommu_dma_init);	/* [한국어] IOMMU 드라이버 초기화보다 먼저 돌아야 한다 */
