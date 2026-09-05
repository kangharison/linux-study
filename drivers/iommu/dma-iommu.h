/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014-2015 ARM Ltd.
 */
/*
 * [한국어 설명] dma-iommu.c 와 iommu.c 사이의 내부 인터페이스 (drivers/iommu/dma-iommu.h)
 *
 * === 파일의 역할 ===
 * IOMMU 코어(iommu.c)가 DMA API 통합 계층(dma-iommu.c)을 부르기 위한 선언들이다.
 * 서브시스템 내부 전용이라 include/linux 가 아니라 여기 있다.
 *
 * 이 헤더의 구조 자체가 설계를 말해 준다 — CONFIG_IOMMU_DMA 가 꺼진 빌드를 위해
 * 모든 함수의 빈 구현을 제공한다. 그래서 iommu.c 는 그 설정을 신경 쓰지 않고
 * 무조건 부를 수 있고, DMA API 통합이 없는 시스템(순수 VFIO 용도 등)에서는
 * 호출이 그대로 사라진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iommu.c (도메인 생성/해제, 장치 프로브) → [이 선언들] → dma-iommu.c
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: 도메인에 DMA 쿠키를 붙이고 떼며, 장치의 dma_ops 를 갈아 끼운다.
 * - dma-iommu.c: 여기 선언된 함수들의 실제 구현.
 *
 * === 주요 함수/구조체 요약 ===
 * - iommu_setup_dma_ops()   : 장치의 DMA API 를 IOMMU 경로로 전환한다.
 * - iommu_get/put_dma_cookie(): 도메인의 DMA 상태를 붙이고 뗀다.
 * - iommu_dma_init_fq()     : 지연 무효화를 켠다 (sysfs 경로에서도 쓴다).
 * - iommu_dma_sw_msi()      : MSI 도어벨을 매핑한다.
 * - iommu_dma_forcedac      : 32비트 우선 IOVA 할당을 끄는 부트 인자.
 */
#ifndef __DMA_IOMMU_H	/* [한국어] 중복 포함 방지 */
#define __DMA_IOMMU_H

#include <linux/iommu.h>	/* [한국어] struct iommu_domain 과 device 정의 */

#ifdef CONFIG_IOMMU_DMA	/* [한국어] DMA API 통합이 켜진 빌드의 실제 선언들 */

void iommu_setup_dma_ops(struct device *dev, struct iommu_domain *domain);	/* [한국어] 장치의 dma_map_* 을 dma-iommu 구현으로 전환한다. 이 호출이 지나야 그 장치의 DMA 가 IOMMU 를 거친다 */

int iommu_get_dma_cookie(struct iommu_domain *domain);	/* [한국어] 도메인에 DMA 상태(IOVA 공간, flush queue)를 붙인다 */
void iommu_put_dma_cookie(struct iommu_domain *domain);	/* [한국어] 그 짝 */
void iommu_put_msi_cookie(struct iommu_domain *domain);	/* [한국어] MSI 전용 축소판 쿠키의 해제. get 쪽은 VFIO 가 직접 부르므로 공개 헤더에 있다 */

int iommu_dma_init_fq(struct iommu_domain *domain);	/* [한국어] 지연 무효화를 켠다. sysfs 로 DMA-FQ 를 요청할 때 iommu.c 가 부른다 */

void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list);	/* [한국어] 아키텍처 공통 예약 구간(ARM 의 GICv3 ITS 창 등)을 모은다 */

int iommu_dma_sw_msi(struct iommu_domain *domain, struct msi_desc *desc,	/* [한국어] MSI 도어벨을 이 도메인의 IOVA 공간에 매핑한다 */
		     phys_addr_t msi_addr);	/* [한국어] 도어벨의 물리 주소 */

extern bool iommu_dma_forcedac;	/* [한국어] iommu.forcedac 부트 인자. iommu.c 가 장치 프로브에서 읽어 32비트 우선 할당 여부를 정한다 */

#else /* CONFIG_IOMMU_DMA */	/* [한국어] 통합이 꺼진 빌드 — 아래는 모두 빈 구현이다 */

/*
 * [한국어]
 * iommu_setup_dma_ops - (CONFIG_IOMMU_DMA 미설정) DMA ops 전환을 생략한다
 *
 * @dev:    프로브 중인 장치. 여기서는 손대지 않는다.
 * @domain: 그 장치가 속한 IOMMU 도메인. 여기서는 쓰이지 않는다.
 *
 * 켠 빌드에서는 이 호출이 장치의 dma_map_*() 을 dma-iommu 구현으로 갈아끼워,
 * 그 시점부터 그 장치의 DMA 가 IOMMU 를 거치게 만든다. 끈 빌드에서는 갈아끼울
 * 구현 자체가 없으므로 아무 일도 하지 않고, 장치는 플랫폼 기본 DMA 경로
 * (dma-direct 또는 아키텍처 고유 ops)를 그대로 쓴다.
 *
 * 빈 함수로 두는 이유: 호출부인 iommu.c 의 프로브 경로에 #ifdef 를 심지 않기
 * 위해서다. 인라인 빈 함수는 컴파일 후 사라지므로 비용이 0 이다.
 *
 * 실행 컨텍스트: 장치 프로브(프로세스 문맥).
 *
 * 호출 체인:
 *   iommu_probe_device() (drivers/iommu/iommu.c) → [이 빈 구현] → (없음)
 */
static inline void iommu_setup_dma_ops(struct device *dev,	/* [한국어] 아무 일도 하지 않는다 */
				       struct iommu_domain *domain)	/* [한국어] 장치는 플랫폼 기본 DMA 경로를 계속 쓴다 */
{
}

/*
 * [한국어]
 * iommu_dma_init_fq - (CONFIG_IOMMU_DMA 미설정) 지연 무효화를 켤 수 없다고 답한다
 *
 * @domain: DMA-FQ 를 켜 달라고 요청받은 도메인. 여기서는 쓰이지 않는다.
 * @return: 항상 -EINVAL.
 *
 * 켠 빌드에서는 도메인에 flush queue 를 붙여, unmap 마다 TLB 를 비우는 대신
 * 무효화를 모아 두었다가 한꺼번에 처리하게 만든다(성능과 안전성의 맞바꿈).
 * 끈 빌드에는 쿠키도 큐도 없으므로 요청을 받아 줄 수 없다.
 *
 * 에러 경로: -EINVAL 은 sysfs 의 iommu_group_store_type() 까지 그대로 올라가,
 * 사용자가 DMA-FQ 로 바꾸려는 write 가 실패로 끝난다. 도메인 상태는 바뀌지 않는다.
 *
 * 실행 컨텍스트: sysfs write(프로세스 문맥).
 *
 * 호출 체인:
 *   iommu_change_dev_def_domain() (drivers/iommu/iommu.c) → [이 빈 구현]
 */
static inline int iommu_dma_init_fq(struct iommu_domain *domain)	/* [한국어] 지연 무효화를 켤 수 없다 */
{
	return -EINVAL;	/* [한국어] sysfs 의 DMA-FQ 요청이 거절된다 */
}

/*
 * [한국어]
 * iommu_get_dma_cookie - (CONFIG_IOMMU_DMA 미설정) DMA 쿠키를 만들 수 없다고 답한다
 *
 * @domain: 쿠키를 붙일 대상 도메인. 여기서는 쓰이지 않는다.
 * @return: 항상 -ENODEV.
 *
 * 쿠키(struct iommu_dma_cookie)는 도메인마다의 IOVA 할당자와 flush queue 를
 * 담는 그릇이다. 끈 빌드에는 그 구현이 링크되지 않으므로 만들 수 없다.
 *
 * 에러 경로: -ENODEV 를 본 호출자는 IOMMU_DOMAIN_DMA 타입 도메인 생성을
 * 포기하고, 대신 아이덴티티 도메인이나 플랫폼 기본 경로로 돌아간다. 즉 이
 * 반환값은 "DMA 도메인은 이 빌드에 없다"는 뜻으로 읽힌다.
 *
 * 실행 컨텍스트: 도메인 생성 경로(프로세스 문맥).
 *
 * 호출 체인:
 *   iommu_setup_default_domain() (drivers/iommu/iommu.c) → [이 빈 구현]
 */
static inline int iommu_get_dma_cookie(struct iommu_domain *domain)	/* [한국어] DMA 쿠키를 만들 수 없다 */
{
	return -ENODEV;	/* [한국어] iommu.c 가 이 실패를 보고 DMA 도메인 생성을 포기한다 */
}

/*
 * [한국어]
 * iommu_put_dma_cookie - (CONFIG_IOMMU_DMA 미설정) 해제할 쿠키가 없다
 *
 * @domain: 해제 대상 도메인. 여기서는 쓰이지 않는다.
 *
 * 짝이 되는 iommu_get_dma_cookie() 가 이 빌드에서는 항상 실패하므로 붙어 있는
 * 쿠키도 없다. 그래도 함수가 존재해야 하는 이유는 도메인 해제 경로가 get/put 을
 * 짝지어 부르는 형태를 #ifdef 없이 유지할 수 있게 하기 위해서다.
 *
 * 실행 컨텍스트: 도메인 해제(프로세스 문맥).
 *
 * 호출 체인:
 *   iommu_domain_free() (drivers/iommu/iommu.c) → [이 빈 구현]
 */
static inline void iommu_put_dma_cookie(struct iommu_domain *domain)	/* [한국어] 만든 적이 없으므로 */
{
}

/*
 * [한국어]
 * iommu_put_msi_cookie - (CONFIG_IOMMU_DMA 미설정) 해제할 MSI 쿠키가 없다
 *
 * @domain: 해제 대상 도메인. 여기서는 쓰이지 않는다.
 *
 * MSI 쿠키는 IOVA 할당자 없이 도어벨 매핑 목록만 들고 있는 축소판 쿠키로,
 * VFIO 처럼 IOVA 공간을 유저스페이스가 직접 관리하는 경우에 쓴다. 끈 빌드에는
 * 만들어진 적이 없으므로 해제할 것도 없다. get 쪽은 VFIO 가 직접 부르기 때문에
 * 공개 헤더(linux/iommu.h)에 있고, put 만 이 내부 헤더에 있다.
 *
 * 실행 컨텍스트: 도메인 해제(프로세스 문맥).
 *
 * 호출 체인:
 *   iommu_domain_free() (drivers/iommu/iommu.c) → [이 빈 구현]
 */
static inline void iommu_put_msi_cookie(struct iommu_domain *domain)	/* [한국어] 마찬가지 */
{
}

/*
 * [한국어]
 * iommu_dma_get_resv_regions - (CONFIG_IOMMU_DMA 미설정) 예약 구간을 더하지 않는다
 *
 * @dev:  예약 구간을 물어보는 장치. 여기서는 쓰이지 않는다.
 * @list: 구간을 이어 붙일 목록. 여기서는 건드리지 않는다 — 호출자가 이미
 *        초기화해 둔 빈 목록이 그대로 남는다.
 *
 * 켠 빌드에서는 아키텍처 공통의 예약 구간, 대표적으로 ARM GICv3 의 ITS 도어벨
 * 창을 이 목록에 실어 준다. 그 구간을 일반 DMA 할당에 내주면 MSI 쓰기와 IOVA 가
 * 충돌하기 때문이다. 끈 빌드에는 IOVA 할당자 자체가 없어 충돌할 일도 없다.
 *
 * 실행 컨텍스트: 장치 프로브 또는 sysfs 의 reserved_regions 읽기.
 *
 * 호출 체인:
 *   iommu_get_resv_regions() (drivers/iommu/iommu.c) → [이 빈 구현]
 */
static inline void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)	/* [한국어] 예약 구간을 더하지 않는다 */
{
}

/*
 * [한국어]
 * iommu_dma_sw_msi - (CONFIG_IOMMU_DMA 미설정) MSI 도어벨을 매핑하지 않는다
 *
 * @domain:   도어벨을 매핑해 달라는 도메인. 여기서는 쓰이지 않는다.
 * @desc:     대상 MSI 디스크립터. 여기서는 쓰이지 않는다.
 * @msi_addr: 도어벨의 물리 주소. 여기서는 쓰이지 않는다.
 * @return:   항상 -ENODEV.
 *
 * 장치가 IOMMU 뒤에 있으면 MSI 쓰기도 변환을 거치므로, 인터럽트 컨트롤러의
 * 도어벨 물리 주소를 그 장치의 도메인 안에 미리 매핑해 두어야 한다. 그 일을
 * 켠 빌드에서는 쿠키의 msi_page 목록이 맡는다. 끈 빌드에는 그 목록이 없다.
 *
 * 에러 경로: -ENODEV 를 받은 MSI 설정 경로는 소프트웨어 MSI 매핑을 건너뛴다.
 * 이 빌드에서 IOMMU 뒤 장치의 MSI 는 애초에 성립하지 않으므로 문제가 되지 않는다.
 *
 * 실행 컨텍스트: MSI 할당 경로(프로세스 문맥).
 *
 * 호출 체인:
 *   iommu_dma_prepare_msi() → [이 빈 구현]
 */
static inline int iommu_dma_sw_msi(struct iommu_domain *domain,	/* [한국어] MSI 매핑을 관리하지 않는다 */
				   struct msi_desc *desc, phys_addr_t msi_addr)	/* [한국어] 같은 시그니처 */
{
	return -ENODEV;	/* [한국어] 호출자가 다른 경로를 찾거나 실패로 처리한다 */
}

#endif	/* CONFIG_IOMMU_DMA */
#endif	/* __DMA_IOMMU_H */
