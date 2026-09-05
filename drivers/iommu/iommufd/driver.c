// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] iommufd 가 벤더 드라이버에 내어 주는 공유 코드 (driver.c)
 *
 * === 파일의 역할 ===
 * iommufd 본체는 모듈일 수 있지만, IOMMU 벤더 드라이버는 커널에 붙박이로
 * 들어갈 수 있다. 그래서 양쪽이 함께 쓰는 함수는 따로 떼어 언제나 존재하는
 * 자리에 두어야 한다 — 이 파일이 그것이다.
 *
 * 담고 있는 것은 세 부류다.
 *
 * 1) 객체 의존 관계. 드라이버가 만든 객체(vIOMMU 같은)가 다른 iommufd
 *    객체를 붙잡아 두었다가 놓는 참조 계수 도우미다.
 *
 * 2) mmap 창구. 드라이버가 장치 MMIO 영역을 사용자 공간에 직접 노출하고
 *    싶을 때, 그 영역에 오프셋을 배정해 준다. 사용자는 iommufd 파일
 *    디스크립터에 그 오프셋으로 mmap 한다.
 *
 * 3) 소프트웨어 MSI 매핑. 이것이 이 파일에서 가장 미묘한 부분이다.
 *    일부 플랫폼(주로 ARM)은 MSI 쓰기도 IOMMU 를 거치므로, 장치가 인터럽트를
 *    내려면 MSI 도어벨의 물리 주소가 IOVA 공간에 매핑되어 있어야 한다.
 *    사용자 공간은 그 주소를 모르고 알아서도 안 되므로, 커널이 대신
 *    IOVA 를 골라 매핑하고 그 값을 MSI 메시지에 적어 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간 → iommufd 본체 → [이 파일] ← IOMMU 벤더 드라이버
 * 인터럽트 계층 → iommufd_sw_msi() → iommu_map()
 *
 * 실행 컨텍스트: 대부분 프로세스 문맥. iommufd_viommu_report_event 만
 * 드라이버의 인터럽트 스레드에서 불린다(그래서 GFP_ATOMIC 을 쓴다).
 *
 * === 타 모듈과의 연결 ===
 * 위: drivers/iommu/arm/arm-smmu-v3 등 vIOMMU 를 지원하는 드라이버들,
 *     커널의 MSI 계층.
 * 아래: iommufd_private.h 의 객체 모델, maple tree(mmap 오프셋 할당),
 *       iommu_map().
 *
 * === 주요 함수/구조체 요약 ===
 * _iommufd_object_depend / _undepend: 객체 사이의 참조를 잡고 놓는다.
 *   자기 자신이나 다른 종류를 붙잡는 것은 교착이나 형 오류라 막는다.
 * _iommufd_alloc_mmap / _destroy_mmap: MMIO 영역에 mmap 오프셋을 배정한다.
 * iommufd_viommu_find_dev / get_vdev_id: 가상 장치 id 와 실제 장치 사이를
 *   오간다. 드라이버가 게스트가 쓴 id 를 해석할 때 필요하다.
 * iommufd_viommu_report_event: 하드웨어 이벤트를 사용자 공간의 큐에 넣는다.
 *   큐가 가득 차면 "잃어버림" 표시만 남긴다.
 * iommufd_sw_msi: MSI 도어벨을 IOVA 에 매핑하고 그 주소를 메시지에 적는다.
 */
#include "iommufd_private.h"	/* [한국어] iommufd 의 객체 모델과 내부 자료구조 */

/* Driver should use a per-structure helper in include/linux/iommufd.h */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * _iommufd_object_depend - 한 객체가 다른 객체를 붙잡는다
 *
 * @obj_dependent: 붙잡는 쪽.
 * @obj_depended: 붙잡히는 쪽.
 * @return: 0 성공, -EINVAL 이면 허용되지 않는 조합이다.
 *
 * 붙잡힌 객체는 참조가 살아 있는 동안 해제되지 않는다. 드라이버가 만든
 * vIOMMU 가 자기가 딛고 선 hwpt 를 놓치지 않게 하는 식으로 쓴다.
 *
 * 두 가지를 막는다. 자기 자신을 붙잡으면 해제 시점에 교착이 나고, 종류가
 * 다른 객체 사이의 의존은 이 계층이 다루지 않는다.
 *
 * 원 주석대로 드라이버는 이 함수를 직접 부르지 않고 include/linux/iommufd.h
 * 의 타입별 껍질을 쓴다 — 그쪽이 형 검사를 해 준다.
 */
int _iommufd_object_depend(struct iommufd_object *obj_dependent,
			   struct iommufd_object *obj_depended)
{
	/* Reject self dependency that dead locks */
	if (obj_dependent == obj_depended)	/* [한국어] (원 주석: 교착을 일으키는 자기 의존을 거절한다) */
		return -EINVAL;	/* [한국어] 해제 시점에 자기를 기다리게 된다 */
	/* Only support dependency between two objects of the same type */
	if (obj_dependent->type != obj_depended->type)	/* [한국어] (원 주석: 같은 종류끼리만 지원한다) */
		return -EINVAL;	/* [한국어] 이 계층이 다루지 않는 조합 */

	refcount_inc(&obj_depended->users);	/* [한국어] 참조가 살아 있는 동안 해제되지 않는다 */
	return 0;	/* [한국어] 성공 */
}
EXPORT_SYMBOL_NS_GPL(_iommufd_object_depend, "IOMMUFD");	/* [한국어] 드라이버가 타입별 껍질을 통해 쓴다 */

/* Driver should use a per-structure helper in include/linux/iommufd.h */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * _iommufd_object_undepend - 붙잡았던 객체를 놓는다
 *
 * @obj_dependent: 붙잡았던 쪽.
 * @obj_depended: 놓을 쪽.
 *
 * depend 와 같은 조건을 확인하되, 여기서는 거절할 방법이 없어 경고만 하고
 * 참조를 내리지 않는다 — 잘못된 짝이면 남의 참조를 내리는 편이 더 위험하다.
 */
void _iommufd_object_undepend(struct iommufd_object *obj_dependent,
			      struct iommufd_object *obj_depended)
{
	if (WARN_ON_ONCE(obj_dependent == obj_depended ||	/* [한국어] depend 가 거절했을 조합이면 */
			 obj_dependent->type != obj_depended->type))	/* [한국어] 짝이 맞지 않는다 */
		return;	/* [한국어] 남의 참조를 내리는 편이 더 위험하다 */

	refcount_dec(&obj_depended->users);	/* [한국어] 0 이 되면 해제될 수 있다 */
}
EXPORT_SYMBOL_NS_GPL(_iommufd_object_undepend, "IOMMUFD");	/* [한국어] 짝이 되는 해제 */

/*
 * Allocate an @offset to return to user space to use for an mmap() syscall
 *
 * Driver should use a per-structure helper in include/linux/iommufd.h
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * _iommufd_alloc_mmap - MMIO 영역에 mmap 오프셋을 배정한다
 *
 * @ictx: iommufd 문맥.
 * @owner: 이 영역을 소유하는 객체.
 * @mmio_addr: 노출할 물리 주소.
 * @length: 그 길이.
 * @offset: 배정된 오프셋을 돌려준다.
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자 공간이 iommufd 파일 디스크립터에 이 오프셋으로 mmap 하면 그
 * MMIO 영역이 매핑된다. 드라이버가 게스트에게 하드웨어 큐 레지스터 같은
 * 것을 직접 보여 줄 때 쓴다.
 *
 * 첫 페이지를 건너뛰는 이유를 원 주석이 밝힌다 — 오프셋 0 은 "없음"과
 * 구별되지 않아, 호출자가 결과를 알아보기 쉽게 1 페이지부터 나눠 준다.
 *
 * vm_pgoff 를 미리 계산해 두는 것도 원 주석의 설명대로다. mmap 시스템
 * 콜이 오프셋을 페이지 단위로 나누어 vma 에 넣으므로, 나중에 그 값으로
 * 찾을 수 있게 같은 형태로 저장한다.
 */
int _iommufd_alloc_mmap(struct iommufd_ctx *ictx, struct iommufd_object *owner,
			phys_addr_t mmio_addr, size_t length,
			unsigned long *offset)
{
	struct iommufd_mmap *immap;	/* [한국어] 배정 정보 */
	unsigned long startp;	/* [한국어] 배정받은 오프셋 */
	int rc;	/* [한국어] 결과 */

	if (!PAGE_ALIGNED(mmio_addr))	/* [한국어] mmap 은 페이지 단위라 */
		return -EINVAL;	/* [한국어] 정렬되지 않은 주소는 노출할 수 없다 */
	if (!length || !PAGE_ALIGNED(length))	/* [한국어] 길이도 마찬가지 */
		return -EINVAL;	/* [한국어] 거절 */

	immap = kzalloc(sizeof(*immap), GFP_KERNEL);	/* [한국어] 배정 정보를 담을 자리 */
	if (!immap)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 호출자에게 */
	immap->owner = owner;	/* [한국어] 거둘 때 소유자를 확인한다 */
	immap->length = length;	/* [한국어] 노출할 길이 */
	immap->mmio_addr = mmio_addr;	/* [한국어] 노출할 물리 주소 */

	/* Skip the first page to ease caller identifying the returned offset */
	rc = mtree_alloc_range(&ictx->mt_mmap, &startp, immap, immap->length,	/* [한국어] (원 주석: 첫 페이지를 건너뛰어 호출자가 결과를 알아보기 쉽게 한다) */
			       PAGE_SIZE, ULONG_MAX, GFP_KERNEL);	/* [한국어] 오프셋 0 은 "없음"과 구별되지 않는다 */
	if (rc < 0) {	/* [한국어] 배정 실패면 */
		kfree(immap);	/* [한국어] 정보도 버리고 */
		return rc;	/* [한국어] 오류를 전한다 */
	}

	/* mmap() syscall will right-shift the offset in vma->vm_pgoff too */
	immap->vm_pgoff = startp >> PAGE_SHIFT;	/* [한국어] (원 주석: mmap 이 vma->vm_pgoff 에도 같은 시프트를 한다) */
	*offset = startp;	/* [한국어] 사용자 공간이 이 값으로 mmap 한다 */
	return 0;	/* [한국어] 성공 */
}
EXPORT_SYMBOL_NS_GPL(_iommufd_alloc_mmap, "IOMMUFD");	/* [한국어] MMIO 노출용 */

/* Driver should use a per-structure helper in include/linux/iommufd.h */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * _iommufd_destroy_mmap - 배정했던 mmap 오프셋을 거둔다
 *
 * @ictx: iommufd 문맥.
 * @owner: 그 영역을 소유했던 객체.
 * @offset: 거둘 오프셋.
 *
 * 소유자가 다르면 경고한다 — 남의 영역을 거두는 것은 코드 쪽 버그다.
 * 다만 이미 나무에서 뺀 뒤라 되돌리지는 않는다.
 */
void _iommufd_destroy_mmap(struct iommufd_ctx *ictx,
			   struct iommufd_object *owner, unsigned long offset)
{
	struct iommufd_mmap *immap;	/* [한국어] 거둘 배정 정보 */

	immap = mtree_erase(&ictx->mt_mmap, offset);	/* [한국어] 나무에서 빼고 */
	WARN_ON_ONCE(!immap || immap->owner != owner);	/* [한국어] 남의 영역을 거두는 것은 코드 쪽 버그다 */
	kfree(immap);	/* [한국어] 정보를 해제한다 */
}
EXPORT_SYMBOL_NS_GPL(_iommufd_destroy_mmap, "IOMMUFD");	/* [한국어] 그 해제 */

/*
 * [한국어]
 * iommufd_vdevice_to_device - 가상 장치에서 실제 장치를 꺼낸다
 *
 * @vdev: 가상 장치.
 * @return: 그 뒤의 실제 장치.
 *
 * 게스트가 보는 장치와 호스트의 장치를 잇는 한 걸음이다. 드라이버가
 * 게스트의 명령을 실제 하드웨어에 옮길 때 쓴다.
 */
struct device *iommufd_vdevice_to_device(struct iommufd_vdevice *vdev)
{
	return vdev->idev->dev;	/* [한국어] 게스트가 보는 장치 뒤의 실제 장치 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_vdevice_to_device, "IOMMUFD");	/* [한국어] 가상 장치 → 실제 장치 */

/* Caller should xa_lock(&viommu->vdevs) to protect the return value */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_viommu_find_dev - 게스트가 쓴 장치 id 로 실제 장치를 찾는다
 *
 * @viommu: 그 게스트의 vIOMMU.
 * @vdev_id: 게스트가 쓴 id.
 * @return: 그 장치, 없으면 NULL.
 *
 * 게스트가 명령 큐에 넣은 무효화 명령에는 게스트가 아는 장치 id 가 적혀
 * 있다. 드라이버가 그것을 호스트의 장치로 옮겨야 실제 명령을 만들 수 있다.
 *
 * 원 주석이 계약을 밝힌다 — 반환값이 유효하려면 호출자가 xa_lock 을 쥐고
 * 있어야 한다. 놓는 순간 그 장치가 사라질 수 있다.
 */
struct device *iommufd_viommu_find_dev(struct iommufd_viommu *viommu,
				       unsigned long vdev_id)
{
	struct iommufd_vdevice *vdev;	/* [한국어] 찾을 가상 장치 */

	lockdep_assert_held(&viommu->vdevs.xa_lock);	/* [한국어] (원 주석: 반환값이 유효하려면 호출자가 이 락을 쥐고 있어야 한다) */

	vdev = xa_load(&viommu->vdevs, vdev_id);	/* [한국어] 게스트 id 로 찾고 */
	return vdev ? iommufd_vdevice_to_device(vdev) : NULL;	/* [한국어] 있으면 실제 장치를 돌려준다 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_viommu_find_dev, "IOMMUFD");	/* [한국어] 게스트 id → 실제 장치 */

/* Return -ENOENT if device is not associated to the vIOMMU */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_viommu_get_vdev_id - 실제 장치에 대응하는 게스트 id 를 찾는다
 *
 * @viommu: 그 게스트의 vIOMMU.
 * @dev: 실제 장치.
 * @vdev_id: 찾은 id 를 돌려준다.
 * @return: 0 성공, -ENOENT 면 그 vIOMMU 에 속하지 않는다.
 *
 * find_dev 의 역방향이다. 하드웨어가 낸 이벤트를 게스트에게 전할 때,
 * 호스트의 장치를 게스트가 아는 이름으로 바꿔야 한다.
 *
 * 선형 탐색인 이유: 이 방향은 드물게 쓰이고(이벤트 보고 경로), 한
 * vIOMMU 에 붙는 장치 수가 많지 않다.
 */
int iommufd_viommu_get_vdev_id(struct iommufd_viommu *viommu,
			       struct device *dev, unsigned long *vdev_id)
{
	struct iommufd_vdevice *vdev;	/* [한국어] 순회 중인 가상 장치 */
	unsigned long index;	/* [한국어] xarray 인덱스 */
	int rc = -ENOENT;	/* [한국어] 못 찾았을 때의 기본값 */

	if (WARN_ON_ONCE(!vdev_id))	/* [한국어] 돌려줄 자리가 없으면 */
		return -EINVAL;	/* [한국어] 부를 이유가 없다 */

	xa_lock(&viommu->vdevs);	/* [한국어] 순회 중 목록이 바뀌면 안 된다 */
	xa_for_each(&viommu->vdevs, index, vdev) {	/* [한국어] 이 vIOMMU 의 모든 가상 장치를 */
		if (iommufd_vdevice_to_device(vdev) == dev) {	/* [한국어] 찾는 장치와 같으면 */
			*vdev_id = vdev->virt_id;	/* [한국어] 게스트가 아는 id 를 돌려주고 */
			rc = 0;	/* [한국어] 성공으로 */
			break;	/* [한국어] 멈춘다 */
		}
	}
	xa_unlock(&viommu->vdevs);	/* [한국어] 순회 끝 */
	return rc;	/* [한국어] 성패 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_viommu_get_vdev_id, "IOMMUFD");	/* [한국어] 실제 장치 → 게스트 id */

/*
 * Typically called in driver's threaded IRQ handler.
 * The @type and @event_data must be defined in include/uapi/linux/iommufd.h
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_viommu_report_event - 하드웨어 이벤트를 게스트의 큐에 넣는다
 *
 * @viommu: 그 게스트의 vIOMMU.
 * @type: 이벤트 종류(uapi 헤더가 정의한다).
 * @event_data: 이벤트 내용.
 * @data_len: 그 길이.
 * @return: 0 성공, -EOPNOTSUPP 그 종류의 큐가 없음, -ENOMEM.
 *
 * 페이지 폴트나 오류 같은 하드웨어 사건을 사용자 공간(대개 VMM)에 전한다.
 * 원 주석대로 드라이버의 인터럽트 스레드에서 불리므로 GFP_ATOMIC 을 쓴다.
 *
 * 큐가 가득 찼을 때의 처리가 이 함수의 핵심이다. 이벤트를 버리는 대신
 * 미리 준비된 "잃어버림 머리말"을 큐에 얹는다. 사용자 공간은 그것을 보고
 * "여기서 이벤트가 빠졌다"를 알 수 있어, 조용히 사라지는 것보다 낫다.
 *
 * 메모리 할당에 실패해도 같은 처리를 한다 — 인터럽트 문맥에서 기다릴 수
 * 없으므로 잃어버림으로 기록하고 넘어간다.
 */
int iommufd_viommu_report_event(struct iommufd_viommu *viommu,
				enum iommu_veventq_type type, void *event_data,
				size_t data_len)
{
	struct iommufd_veventq *veventq;	/* [한국어] 이벤트를 넣을 큐 */
	struct iommufd_vevent *vevent;	/* [한국어] 만들 이벤트 */
	int rc = 0;	/* [한국어] 결과 */

	if (WARN_ON_ONCE(!data_len || !event_data))	/* [한국어] 내용 없는 이벤트면 */
		return -EINVAL;	/* [한국어] 전할 것이 없다 */

	down_read(&viommu->veventqs_rwsem);	/* [한국어] 큐 목록이 사라지지 않게 */

	veventq = iommufd_viommu_find_veventq(viommu, type);	/* [한국어] 그 종류의 큐를 찾는다 */
	if (!veventq) {	/* [한국어] 사용자 공간이 그 종류를 구독하지 않았으면 */
		rc = -EOPNOTSUPP;	/* [한국어] 전할 곳이 없다 */
		goto out_unlock_veventqs;	/* [한국어] 풀고 나간다 */
	}

	spin_lock(&veventq->common.lock);	/* [한국어] 큐를 배타적으로 */
	if (veventq->num_events == veventq->depth) {	/* [한국어] 가득 찼으면 */
		vevent = &veventq->lost_events_header;	/* [한국어] 미리 준비된 잃어버림 표시를 쓴다 */
		goto out_set_header;	/* [한국어] 조용히 버리는 것보다 낫다 */
	}

	vevent = kzalloc_flex(*vevent, event_data, data_len, GFP_ATOMIC);	/* [한국어] 인터럽트 스레드라 ATOMIC */
	if (!vevent) {	/* [한국어] 할당 실패면 */
		rc = -ENOMEM;	/* [한국어] 호출자에게 알리되 */
		vevent = &veventq->lost_events_header;	/* [한국어] 사용자 공간에는 잃어버림으로 남긴다 */
		goto out_set_header;	/* [한국어] 기다릴 수 없는 문맥이다 */
	}
	vevent->data_len = data_len;	/* [한국어] 이벤트 길이 */
	memcpy(vevent->event_data, event_data, data_len);	/* [한국어] 내용을 복사한다 */
	veventq->num_events++;	/* [한국어] 큐에 하나 늘었다 */

out_set_header:	/* [한국어] 가득 찼거나 할당에 실패한 경우가 여기로 모인다 */
	iommufd_vevent_handler(veventq, vevent);	/* [한국어] 큐에 넣고 기다리는 쪽을 깨운다 */
	spin_unlock(&veventq->common.lock);	/* [한국어] 큐 보호 해제 */
out_unlock_veventqs:	/* [한국어] 큐를 못 찾은 경우가 여기로 */
	up_read(&viommu->veventqs_rwsem);	/* [한국어] 큐 목록 보호 해제 */
	return rc;	/* [한국어] 성패 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_viommu_report_event, "IOMMUFD");	/* [한국어] 드라이버의 인터럽트 스레드가 부른다 */

#ifdef CONFIG_IRQ_MSI_IOMMU	/* [한국어] MSI 주소도 IOMMU 를 거치는 플랫폼에서만 */
/*
 * Get a iommufd_sw_msi_map for the msi physical address requested by the irq
 * layer. The mapping to IOVA is global to the iommufd file descriptor, every
 * domain that is attached to a device using the same MSI parameters will use
 * the same IOVA.
 */
static struct iommufd_sw_msi_map *
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_sw_msi_get_map - MSI 도어벨 주소에 대응하는 IOVA 배정을 찾거나 만든다
 *
 * @ictx: iommufd 문맥.
 * @msi_addr: MSI 도어벨의 물리 주소(페이지 정렬).
 * @sw_msi_start: 그 장치 그룹이 쓸 IOVA 창의 시작.
 * @return: 배정 정보, 실패하면 ERR_PTR.
 *
 * 원 주석이 설계를 밝힌다 — 배정은 iommufd 파일 디스크립터 단위로 전역이라,
 * 같은 MSI 매개변수를 쓰는 모든 도메인이 같은 IOVA 를 쓴다. 그래야 장치가
 * 도메인을 옮겨 다녀도 MSI 메시지를 다시 쓸 필요가 없다.
 *
 * 목록을 훑으며 두 가지를 동시에 한다: 같은 도어벨이 이미 있으면 그것을
 * 쓰고, 없으면 그 창에서 아직 쓰지 않은 페이지 번호를 찾는다.
 *
 * id 에 상한이 있는 이유: 도메인마다 "어느 배정을 이미 매핑했는가"를
 * 비트맵으로 들고 있어, 그 비트맵의 크기가 곧 배정 수의 한계다.
 */
iommufd_sw_msi_get_map(struct iommufd_ctx *ictx, phys_addr_t msi_addr,
		       phys_addr_t sw_msi_start)
{
	struct iommufd_sw_msi_map *cur;	/* [한국어] 순회 중인 배정 */
	unsigned int max_pgoff = 0;	/* [한국어] 그 창에서 아직 쓰지 않은 페이지 번호 */

	lockdep_assert_held(&ictx->sw_msi_lock);	/* [한국어] 목록을 훑고 고치는 동안 */

	list_for_each_entry(cur, &ictx->sw_msi_list, sw_msi_item) {	/* [한국어] 기존 배정들을 훑는다 */
		if (cur->sw_msi_start != sw_msi_start)	/* [한국어] 다른 창의 배정이면 */
			continue;	/* [한국어] 상관없다 */
		max_pgoff = max(max_pgoff, cur->pgoff + 1);	/* [한국어] 이 창에서 쓰인 다음 자리를 센다 */
		if (cur->msi_addr == msi_addr)	/* [한국어] 같은 도어벨이 이미 있으면 */
			return cur;	/* [한국어] 그것을 쓴다 — 같은 IOVA 를 공유한다 */
	}

	if (ictx->sw_msi_id >=	/* [한국어] 도메인마다 비트맵으로 추적하므로 */
	    BITS_PER_BYTE * sizeof_field(struct iommufd_sw_msi_maps, bitmap))	/* [한국어] 그 크기가 배정 수의 한계다 */
		return ERR_PTR(-EOVERFLOW);	/* [한국어] 더 만들 수 없다 */

	cur = kzalloc_obj(*cur);	/* [한국어] 새 배정 */
	if (!cur)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 호출자에게 */

	cur->sw_msi_start = sw_msi_start;	/* [한국어] 이 그룹이 쓸 IOVA 창 */
	cur->msi_addr = msi_addr;	/* [한국어] 도어벨의 물리 주소 */
	cur->pgoff = max_pgoff;	/* [한국어] 그 창에서의 페이지 번호 */
	cur->id = ictx->sw_msi_id++;	/* [한국어] 비트맵에서의 자리 */
	list_add_tail(&cur->sw_msi_item, &ictx->sw_msi_list);	/* [한국어] 파일 디스크립터 전역 목록에 */
	return cur;	/* [한국어] 새 배정 */
}

/*
 * [한국어]
 * iommufd_sw_msi_install - 배정된 IOVA 에 MSI 도어벨을 실제로 매핑한다
 *
 * @ictx: iommufd 문맥.
 * @hwpt_paging: 매핑할 도메인.
 * @msi_map: 배정 정보.
 * @return: 0 성공, 음수면 매핑 실패.
 *
 * 도메인마다 비트맵으로 "이미 매핑했다"를 기록해 두어 같은 도어벨을 두 번
 * 매핑하지 않는다. 장치가 여러 개 붙은 도메인에서 각 장치가 같은 도어벨을
 * 쓰는 경우가 흔하다.
 *
 * IOMMU_MMIO 를 함께 주는 이유: 장치 레지스터라 캐시할 수 없고, 형식에
 * 따라 암호화 비트도 붙이면 안 된다.
 */
int iommufd_sw_msi_install(struct iommufd_ctx *ictx,
			   struct iommufd_hwpt_paging *hwpt_paging,
			   struct iommufd_sw_msi_map *msi_map)
{
	unsigned long iova;	/* [한국어] 매핑할 IOVA */

	lockdep_assert_held(&ictx->sw_msi_lock);	/* [한국어] 배정과 비트맵을 함께 지킨다 */

	iova = msi_map->sw_msi_start + msi_map->pgoff * PAGE_SIZE;	/* [한국어] 창의 시작에서 그 페이지만큼 */
	if (!test_bit(msi_map->id, hwpt_paging->present_sw_msi.bitmap)) {	/* [한국어] 이 도메인에 아직 매핑하지 않았으면 */
		int rc;	/* [한국어] 매핑 결과 */

		rc = iommu_map(hwpt_paging->common.domain, iova,	/* [한국어] 그 IOVA 에 */
			       msi_map->msi_addr, PAGE_SIZE,	/* [한국어] 도어벨 한 페이지를 */
			       IOMMU_WRITE | IOMMU_READ | IOMMU_MMIO,	/* [한국어] 장치 레지스터라 캐시하지 않는다 */
			       GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자 몫으로 계정한다 */
		if (rc)	/* [한국어] 실패면 */
			return rc;	/* [한국어] 인터럽트를 쓸 수 없다 */
		__set_bit(msi_map->id, hwpt_paging->present_sw_msi.bitmap);	/* [한국어] 같은 도어벨을 두 번 매핑하지 않는다 */
	}
	return 0;	/* [한국어] 성공 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_sw_msi_install, "IOMMUFD_INTERNAL");	/* [한국어] iommufd 본체 모듈만 쓴다 */

/*
 * Called by the irq code if the platform translates the MSI address through the
 * IOMMU. msi_addr is the physical address of the MSI page. iommufd will
 * allocate a fd global iova for the physical page that is the same on all
 * domains and devices.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_sw_msi - MSI 도어벨을 IOVA 에 매핑하고 그 주소를 메시지에 적는다
 *
 * @domain: 그 장치가 붙은 도메인.
 * @desc: 설정 중인 MSI 기술자.
 * @msi_addr: 도어벨의 물리 주소.
 * @return: 0 성공, 음수면 실패.
 *
 * 인터럽트 계층이 MSI 메시지를 만들다가 부른다. 원 주석이 상황을 밝힌다 —
 * 플랫폼이 MSI 주소도 IOMMU 로 변환한다면, 장치가 쓸 주소는 물리 주소가
 * 아니라 IOVA 여야 한다.
 *
 * 그 IOVA 를 사용자 공간이 정하게 둘 수 없다. 임의의 IOVA 를 MSI 로
 * 지정할 수 있으면 장치를 시켜 아무 메모리에나 쓰게 만들 수 있기 때문이다.
 * 그래서 커널이 고르고, 그 값을 여기서 기술자에 직접 적는다.
 *
 * 앞의 락 주장을 원 주석이 설명한다 — iommu 코어가 그룹 뮤텍스 아래에서
 * 부르므로, 이 함수가 도는 동안 attach 핸들이 바뀌지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 장치 그룹 뮤텍스 아래.
 *
 * 호출 체인:
 *   MSI 계층 → [이 함수] → iommufd_sw_msi_get_map() → iommufd_sw_msi_install()
 */
int iommufd_sw_msi(struct iommu_domain *domain, struct msi_desc *desc,
		   phys_addr_t msi_addr)
{
	struct device *dev = msi_desc_to_dev(desc);	/* [한국어] 인터럽트를 낼 장치 */
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 매핑할 도메인 */
	struct iommu_attach_handle *raw_handle;	/* [한국어] 코어가 들고 있는 attach 핸들 */
	struct iommufd_attach_handle *handle;	/* [한국어] iommufd 쪽 표현 */
	struct iommufd_sw_msi_map *msi_map;	/* [한국어] IOVA 배정 */
	struct iommufd_ctx *ictx;	/* [한국어] iommufd 문맥 */
	unsigned long iova;	/* [한국어] 장치가 쓸 주소 */
	int rc;	/* [한국어] 결과 */

	/*
	 * It is safe to call iommu_attach_handle_get() here because the iommu
	 * core code invokes this under the group mutex which also prevents any
	 * change of the attach handle for the duration of this function.
	 */
	iommu_group_mutex_assert(dev);	/* [한국어] (원 주석: 코어가 그룹 뮤텍스 아래에서 부르므로 이 함수가 도는 동안 핸들이 바뀌지 않는다) */

	raw_handle =	/* [한국어] 이 장치의 attach 핸들 */
		iommu_attach_handle_get(dev->iommu_group, IOMMU_NO_PASID, 0);	/* [한국어] PASID 없는 기본 붙임 */
	if (IS_ERR(raw_handle))	/* [한국어] iommufd 가 붙인 장치가 아니면 */
		return 0;	/* [한국어] 우리가 관여할 일이 없다 */
	hwpt_paging = find_hwpt_paging(domain->iommufd_hwpt);	/* [한국어] 매핑을 넣을 도메인 */

	handle = to_iommufd_handle(raw_handle);	/* [한국어] iommufd 쪽 표현으로 */
	/* No IOMMU_RESV_SW_MSI means no change to the msi_msg */
	if (handle->idev->igroup->sw_msi_start == PHYS_ADDR_MAX)	/* [한국어] (원 주석: IOMMU_RESV_SW_MSI 가 없으면 msi_msg 를 바꾸지 않는다) */
		return 0;	/* [한국어] MSI 를 변환하지 않는 플랫폼이다 */

	ictx = handle->idev->ictx;	/* [한국어] 배정 목록이 있는 문맥 */
	guard(mutex)(&ictx->sw_msi_lock);	/* [한국어] 목록과 비트맵을 함께 지킨다 */
	/*
	 * The input msi_addr is the exact byte offset of the MSI doorbell, we
	 * assume the caller has checked that it is contained with a MMIO region
	 * that is secure to map at PAGE_SIZE.
	 */
	msi_map = iommufd_sw_msi_get_map(handle->idev->ictx,	/* [한국어] (원 주석: 입력 주소는 도어벨의 정확한 오프셋이고, 호출자가 그것이 PAGE_SIZE 로 매핑해도 안전한 MMIO 영역 안임을 확인했다고 가정한다) */
					 msi_addr & PAGE_MASK,	/* [한국어] 페이지 단위로 깎아 */
					 handle->idev->igroup->sw_msi_start);	/* [한국어] 그 그룹의 IOVA 창에서 배정을 찾는다 */
	if (IS_ERR(msi_map))	/* [한국어] 배정할 수 없으면 */
		return PTR_ERR(msi_map);	/* [한국어] 인터럽트를 쓸 수 없다 */

	rc = iommufd_sw_msi_install(ictx, hwpt_paging, msi_map);	/* [한국어] 그 IOVA 에 실제로 매핑한다 */
	if (rc)	/* [한국어] 실패면 */
		return rc;	/* [한국어] 호출자에게 */
	__set_bit(msi_map->id, handle->idev->igroup->required_sw_msi.bitmap);	/* [한국어] 도메인을 옮길 때 이 매핑이 따라가야 한다 */

	iova = msi_map->sw_msi_start + msi_map->pgoff * PAGE_SIZE;	/* [한국어] 장치가 쓸 주소 */
	msi_desc_set_iommu_msi_iova(desc, iova, PAGE_SHIFT);	/* [한국어] 사용자 공간이 정하게 두면 아무 메모리에나 쓰게 만들 수 있다 */
	return 0;	/* [한국어] 성공 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_sw_msi, "IOMMUFD");	/* [한국어] MSI 계층이 부른다 */
#endif

MODULE_DESCRIPTION("iommufd code shared with builtin modules");	/* [한국어] 붙박이 드라이버와 함께 쓰는 코드 */
MODULE_IMPORT_NS("IOMMUFD_INTERNAL");	/* [한국어] iommufd 본체의 내부 심볼 */
MODULE_LICENSE("GPL");	/* [한국어] 라이선스 선언 */
