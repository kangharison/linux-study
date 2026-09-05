/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
/*
 * [한국어 설명] IOMMU 서브시스템 내부 전용 선언 (drivers/iommu/iommu-priv.h)
 *
 * === 파일의 역할 ===
 * include/linux/iommu.h 에 두면 외부 드라이버가 쓸 수 있게 되는 것들을 모아 둔
 * 헤더다. 여기 있는 API 들은 서브시스템 자신과 iommufd 만 부르는 것을 전제로
 * 하며, 그래서 검사가 느슨하거나(dev_iommu_ops) 사용 규약이 까다롭다
 * (부착 핸들의 수명).
 *
 * 크게 네 묶음이다.
 *  1) 내부 접근자: dev_iommu_ops, iommu_fwspec_ops — 검사 없이 곧바로 꺼낸다.
 *  2) 셀프테스트 진입점: iommu_device_register_bus 등, 가짜 IOMMU 를 만드는 API.
 *  3) 부착 핸들 API: iommufd 가 폴트를 자기 문맥으로 되짚기 위해 쓴다.
 *  4) 디버그 페이지 할당 훅: 매핑/해제를 추적해 짝이 맞는지 검증한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iommu.c / dma-iommu.c / io-pgfault.c / iommufd 가 서로를 부를 때 쓰는 계약이다.
 * 외부 벤더 드라이버는 include/linux/iommu.h 만 본다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: 대부분의 구현이 있다.
 * - iommufd: 부착 핸들과 sw_msi 를 통해 연결된다.
 * - iommu-debug-pagealloc: 매핑 추적 훅의 실제 구현.
 *
 * === 주요 함수/구조체 요약 ===
 * - dev_iommu_ops()          : 장치의 드라이버 콜백 표를 검사 없이 꺼낸다.
 * - iommu_fwspec_ops()       : fwspec 으로부터 드라이버를 찾는다.
 * - iommu_attach_handle_get(): PASID → 부착 핸들 조회 (폴트 전달의 핵심).
 * - iommu_*_group_handle()   : 핸들과 함께 도메인을 붙이고 뗀다.
 * - iommu_debug_map/unmap_*(): 매핑과 해제의 짝을 검증하는 훅.
 */
#ifndef __LINUX_IOMMU_PRIV_H	/* [한국어] 중복 포함 방지 */
#define __LINUX_IOMMU_PRIV_H

#include <linux/iommu.h>	/* [한국어] 공개 정의 위에 내부 확장을 얹는다 */
#include <linux/iommu-debug-pagealloc.h>	/* [한국어] 매핑 추적 훅의 정적 키와 구현 선언 */
#include <linux/msi.h>	/* [한국어] MSI 서술자 (sw_msi 경로) */

/*
 * [한국어] (아래 영어 주석 참고)
 * dev_iommu_ops - 장치를 맡은 드라이버의 콜백 표를 꺼낸다
 *
 * @dev:    대상 장치
 * @return: 그 드라이버의 ops
 *
 * 검사가 없는 것이 이 헤더에 있는 이유다. 프로브에 성공한 장치라면 반드시
 * 채워져 있다는 전제이며, 서브시스템 안에서만 쓰이므로 그 전제를 스스로 지킬
 * 수 있다고 본다. 외부에 노출했다면 성립하지 않을 가정이다.
 *
 * 붙지 않은 장치에 부르면 NULL 역참조가 되므로, 그 가능성이 있는 곳은
 * dev_has_iommu 로 먼저 확인한다.
 */
static inline const struct iommu_ops *dev_iommu_ops(struct device *dev)
{
	/*
	 * Assume that valid ops must be installed if iommu_probe_device()
	 * has succeeded. The device ops are essentially for internal use
	 * within the IOMMU subsystem itself, so we should be able to trust
	 * ourselves not to misuse the helper.
	 */
	return dev->iommu->iommu_dev->ops;	/* [한국어] NULL 검사가 없다. 위 영어 주석대로 iommu_probe_device 가 성공한 장치에만 쓰인다는 전제이며, 서브시스템 내부 전용이라 그 전제를 스스로 지킬 수 있다고 본다. 외부에 노출하면 성립하지 않을 가정이라 이 헤더에 있다 */
}

void dev_iommu_free(struct device *dev);	/* [한국어] 장치별 IOMMU 상태 해제. of_iommu.c 가 되감기에서 부른다 */

const struct iommu_ops *iommu_ops_from_fwnode(const struct fwnode_handle *fwnode);	/* [한국어] 펌웨어 노드로 등록된 드라이버를 찾는다 */

/*
 * [한국어]
 * iommu_fwspec_ops - fwspec 이 가리키는 IOMMU 드라이버를 찾는다
 *
 * @fwspec: 장치의 펌웨어 매칭 정보. NULL 도 허용된다.
 * @return: 그 드라이버의 ops, 없으면 NULL
 *
 * fwspec 이 NULL 일 때 NULL fwnode 로 조회하는 것이 요점이다. 그러면 fwnode 를
 * 등록하지 않은 인스턴스가 매칭되는데, 인텔·AMD·s390 처럼 시스템에 하나뿐인
 * IOMMU 가 그렇다. iommu_init_device 가 "펌웨어 정보가 없어도 드라이버를 찾을
 * 수 있다"고 전제하는 근거가 이 동작이다.
 */
static inline const struct iommu_ops *iommu_fwspec_ops(struct iommu_fwspec *fwspec)
{
	return iommu_ops_from_fwnode(fwspec ? fwspec->iommu_fwnode : NULL);	/* [한국어] fwspec 이 없으면 NULL fwnode 로 찾는다. 그러면 인텔·AMD 처럼 시스템에 하나뿐인 IOMMU 가 매칭되는데, iommu.c 가 그 편법을 쓰는 근거가 이 한 줄이다 */
}

void iommu_fwspec_free(struct device *dev);	/* [한국어] fwspec 만 해제. 장치 상태는 남긴다 */

int iommu_device_register_bus(struct iommu_device *iommu,	/* [한국어] 셀프테스트가 가짜 IOMMU 를 한 버스에 등록한다 */
			      const struct iommu_ops *ops,	/* [한국어] 그 드라이버의 콜백 표 */
			      const struct bus_type *bus,	/* [한국어] 맡을 버스 */
			      struct notifier_block *nb);	/* [한국어] 호출자가 제공하는 알림 블록 메모리 */
void iommu_device_unregister_bus(struct iommu_device *iommu,	/* [한국어] 그 짝 */
				 const struct bus_type *bus,	/* [한국어] 같은 버스 */
				 struct notifier_block *nb);	/* [한국어] 같은 알림 블록 */

int iommu_mock_device_add(struct device *dev, struct iommu_device *iommu);	/* [한국어] 가짜 장치를 가짜 IOMMU 에 묶어 등록한다 */

struct iommu_attach_handle *iommu_attach_handle_get(struct iommu_group *group,	/* [한국어] PASID 로 부착 핸들을 찾는다. 폴트 전달 경로의 출발점이며, xa_lock 만 쓰므로 인터럽트 문맥에서도 부를 수 있다 */
						    ioasid_t pasid,	/* [한국어] 찾을 PASID (0 이면 RID 부착) */
						    unsigned int type);	/* [한국어] 기대하는 도메인 종류. 0 이면 가리지 않는다 */
int iommu_attach_group_handle(struct iommu_domain *domain,	/* [한국어] 핸들과 함께 그룹을 붙인다 */
			      struct iommu_group *group,	/* [한국어] 대상 그룹 */
			      struct iommu_attach_handle *handle);	/* [한국어] 매번 새것이어야 한다 — 락 없이 읽는 경로가 있어 재사용하면 경쟁이 생긴다 */
void iommu_detach_group_handle(struct iommu_domain *domain,	/* [한국어] 그 짝 */
			       struct iommu_group *group);	/* [한국어] 대상 그룹 */
int iommu_replace_group_handle(struct iommu_group *group,	/* [한국어] 차단 도메인을 거치지 않고 도메인을 교체한다 */
			       struct iommu_domain *new_domain,	/* [한국어] 새 도메인 */
			       struct iommu_attach_handle *handle);	/* [한국어] 새 핸들 */

#if IS_ENABLED(CONFIG_IOMMUFD_DRIVER_CORE) && IS_ENABLED(CONFIG_IRQ_MSI_IOMMU)	/* [한국어] iommufd 와 MSI 재매핑이 모두 켜진 빌드 */
int iommufd_sw_msi(struct iommu_domain *domain, struct msi_desc *desc,	/* [한국어] 사용자 공간이 소유한 도메인에서 MSI 도어벨을 매핑한다 */
		   phys_addr_t msi_addr);	/* [한국어] 도어벨의 물리 주소 */
#else /* !CONFIG_IOMMUFD_DRIVER_CORE || !CONFIG_IRQ_MSI_IOMMU */	/* [한국어] 둘 중 하나라도 꺼진 빌드 */
static inline int iommufd_sw_msi(struct iommu_domain *domain,	/* [한국어] 빈 구현 */
				 struct msi_desc *desc, phys_addr_t msi_addr)	/* [한국어] 같은 시그니처 */
{
	return -EOPNOTSUPP;	/* [한국어] iommu.c 의 분기가 이 값을 보고 MSI 설정을 실패시킨다 */
}
#endif /* CONFIG_IOMMUFD_DRIVER_CORE && CONFIG_IRQ_MSI_IOMMU */

int iommu_replace_device_pasid(struct iommu_domain *domain,	/* [한국어] PASID 의 도메인을 원자적으로 교체한다 (iommufd 전용) */
			       struct device *dev, ioasid_t pasid,	/* [한국어] 대상 장치와 PASID */
			       struct iommu_attach_handle *handle);	/* [한국어] 새 핸들 (필수) */

#ifdef CONFIG_IOMMU_DEBUG_PAGEALLOC	/* [한국어] 매핑 추적 진단이 켜진 빌드 */

void __iommu_debug_map(struct iommu_domain *domain, phys_addr_t phys,	/* [한국어] 매핑 사실을 기록한다 */
		       size_t size);	/* [한국어] 매핑 범위 */
void __iommu_debug_unmap_begin(struct iommu_domain *domain,	/* [한국어] 해제 직전 상태를 기록 */
			       unsigned long iova, size_t size);	/* [한국어] 해제 범위 */
void __iommu_debug_unmap_end(struct iommu_domain *domain,	/* [한국어] 해제 후 실제 결과와 대조한다 */
			     unsigned long iova, size_t size, size_t unmapped);	/* [한국어] 요청 범위와 실제 해제량 */

/* [한국어] 진단이 꺼진 빌드의 빈 구현. 호출부가 설정을 신경 쓰지 않아도 되게
 * 한다 — 컴파일러가 통째로 지운다. */
static inline void iommu_debug_map(struct iommu_domain *domain,
				   phys_addr_t phys, size_t size)
{
	if (static_branch_unlikely(&iommu_debug_initialized))	/* [한국어] 정적 키 — 진단이 실제로 켜지지 않았으면 이 분기 자체가 코드에서 사라진다. 매핑 핫패스라 그 차이가 크다 */
		__iommu_debug_map(domain, phys, size);	/* [한국어] 켜져 있을 때만 실제 기록 */
}

/* [한국어] 같은 이유의 빈 구현. */
static inline void iommu_debug_unmap_begin(struct iommu_domain *domain,
					   unsigned long iova, size_t size)
{
	if (static_branch_unlikely(&iommu_debug_initialized))	/* [한국어] 같은 정적 키 */
		__iommu_debug_unmap_begin(domain, iova, size);	/* [한국어] 해제 전 기록 */
}

/* [한국어] 같은 이유의 빈 구현. */
static inline void iommu_debug_unmap_end(struct iommu_domain *domain,
					 unsigned long iova, size_t size,
					 size_t unmapped)
{
	if (static_branch_unlikely(&iommu_debug_initialized))	/* [한국어] 같은 정적 키 */
		__iommu_debug_unmap_end(domain, iova, size, unmapped);	/* [한국어] 해제 후 대조 — 매핑과 해제의 범위가 어긋나면 여기서 드러난다 */
}

void iommu_debug_init(void);	/* [한국어] 진단 자료구조를 세우고 정적 키를 켠다 */

#else	/* [한국어] 진단이 꺼진 빌드 — 아래는 모두 빈 구현이다 */
static inline void iommu_debug_map(struct iommu_domain *domain,	/* [한국어] 아무 일도 하지 않는다 */
				   phys_addr_t phys, size_t size)	/* [한국어] 같은 시그니처 */
{
}

static inline void iommu_debug_unmap_begin(struct iommu_domain *domain,	/* [한국어] 마찬가지 */
					   unsigned long iova, size_t size)	/* [한국어] 같은 시그니처 */
{
}

static inline void iommu_debug_unmap_end(struct iommu_domain *domain,	/* [한국어] 마찬가지 */
					 unsigned long iova, size_t size,	/* [한국어] 같은 시그니처 */
					 size_t unmapped)	/* [한국어] 실제 해제량 */
{
}

static inline void iommu_debug_init(void)	/* [한국어] 초기화할 것이 없다 */
{
}

#endif /* CONFIG_IOMMU_DEBUG_PAGEALLOC */

#endif /* __LINUX_IOMMU_PRIV_H */
