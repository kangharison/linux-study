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

static inline void iommu_setup_dma_ops(struct device *dev,	/* [한국어] 아무 일도 하지 않는다 */
				       struct iommu_domain *domain)	/* [한국어] 장치는 플랫폼 기본 DMA 경로를 계속 쓴다 */
{
}

static inline int iommu_dma_init_fq(struct iommu_domain *domain)	/* [한국어] 지연 무효화를 켤 수 없다 */
{
	return -EINVAL;	/* [한국어] sysfs 의 DMA-FQ 요청이 거절된다 */
}

static inline int iommu_get_dma_cookie(struct iommu_domain *domain)	/* [한국어] DMA 쿠키를 만들 수 없다 */
{
	return -ENODEV;	/* [한국어] iommu.c 가 이 실패를 보고 DMA 도메인 생성을 포기한다 */
}

static inline void iommu_put_dma_cookie(struct iommu_domain *domain)	/* [한국어] 만든 적이 없으므로 */
{
}

static inline void iommu_put_msi_cookie(struct iommu_domain *domain)	/* [한국어] 마찬가지 */
{
}

static inline void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)	/* [한국어] 예약 구간을 더하지 않는다 */
{
}

static inline int iommu_dma_sw_msi(struct iommu_domain *domain,	/* [한국어] MSI 매핑을 관리하지 않는다 */
				   struct msi_desc *desc, phys_addr_t msi_addr)	/* [한국어] 같은 시그니처 */
{
	return -ENODEV;	/* [한국어] 호출자가 다른 경로를 찾거나 실패로 처리한다 */
}

#endif	/* CONFIG_IOMMU_DMA */
#endif	/* __DMA_IOMMU_H */
