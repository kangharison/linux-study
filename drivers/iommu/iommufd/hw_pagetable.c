// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] HWPT(하드웨어 페이지 테이블) 객체의 생성과 관리 (hw_pagetable.c)
 *
 * === 파일의 역할 ===
 * HWPT 는 iommu_domain 을 감싼 사용자 객체다. 사용자 공간이 도메인을
 * 직접 만들고 장치를 붙일 수 있게 해 준다.
 *
 * 세 종류가 있다.
 *  - 페이징 HWPT: IOAS 의 매핑을 받는 보통의 도메인. 사용자가 명시적으로
 *    만들 수도, 장치를 IOAS 에 붙일 때 자동으로 만들어질 수도 있다.
 *  - 중첩 HWPT(부모가 HWPT): 게스트가 만든 1단계 표를 하드웨어에 건다.
 *    부모 페이징 HWPT 가 2단계를 맡는다.
 *  - 중첩 HWPT(부모가 vIOMMU): 같되 vIOMMU 객체를 통해 만든다. 드라이버가
 *    그 vIOMMU 의 문맥으로 도메인을 만들어 준다.
 *
 * 이 파일에서 미묘한 것이 캐시 일관성 처리다. 도메인이 만들어진 직후,
 * 아직 아무 매핑도 넣기 전에 그 모드를 정해야 한다 — 형식에 따라 항목마다
 * 그 비트가 있어 나중에 바꿀 수 없기 때문이다. 그 결과로 일관성을 강제한
 * HWPT 는 그러지 못하는 장치가 재사용할 수 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 IOMMU_HWPT_ALLOC → [이 파일] → iommu 코어의 도메인 할당
 *   → io_pagetable.c(매핑 채우기) → device.c(장치 붙이기)
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * === 타 모듈과의 연결 ===
 * 위: main.c 의 ioctl 분배, device.c 가 자동 도메인을 만들 때.
 * 아래: iommu 코어의 domain_alloc_paging_flags / domain_alloc_nested,
 *       io_pagetable.c 의 도메인 추가·제거.
 *
 * === 주요 함수/구조체 요약 ===
 * iommufd_hwpt_paging_alloc: 페이징 도메인을 만들고 IOAS 의 매핑을 채운다.
 * iommufd_hwpt_nested_alloc / iommufd_viommu_alloc_hwpt_nested: 중첩 도메인의
 *   두 가지 생성 경로.
 * iommufd_hwpt_alloc: 사용자 명령을 받아 부모 종류에 따라 위 셋 중 하나로
 *   갈라 보낸다.
 * iommufd_hwpt_invalidate: 게스트가 낸 무효화 명령을 하드웨어에 전한다.
 * iommufd_hwpt_set_dirty_tracking / get_dirty_bitmap: 더티 추적의 두 진입점.
 */
#include <linux/iommu.h>	/* [한국어] 도메인 할당과 능력 조회 */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 구조체 */

#include "../iommu-priv.h"	/* [한국어] __iommu_get_iommu_dev 등 내부 API */
#include "iommufd_private.h"	/* [한국어] 객체 모델 */

/*
 * [한국어]
 * __iommufd_hwpt_destroy - 두 종류가 공유하는 정리
 *
 * @hwpt: 정리할 객체.
 *
 * 도메인을 해제하고, 폴트 큐를 붙여 두었으면 그 참조를 놓는다.
 */
static void __iommufd_hwpt_destroy(struct iommufd_hw_pagetable *hwpt)
{
	if (hwpt->domain)	/* [한국어] 도메인을 만들었으면 */
		iommu_domain_free(hwpt->domain);	/* [한국어] 해제한다 */

	if (hwpt->fault)	/* [한국어] 폴트 큐를 붙여 두었으면 */
		refcount_dec(&hwpt->fault->common.obj.users);	/* [한국어] 그 참조를 놓는다 */
}

/*
 * [한국어]
 * iommufd_hwpt_paging_destroy - 페이징 HWPT 를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * IOAS 의 목록에서 빼고 그 도메인에서 매핑을 걷어낸다. 목록이 비어 있으면
 * 아직 IOAS 에 연결되기 전에 실패한 것이라 그 단계를 건너뛴다.
 *
 * IOAS 참조를 마지막에 놓는 이유: 그 위의 모든 정리가 IOAS 를 쓴다.
 */
void iommufd_hwpt_paging_destroy(struct iommufd_object *obj)
{
	struct iommufd_hwpt_paging *hwpt_paging =	/* [한국어] 공통 객체에서 */
		container_of(obj, struct iommufd_hwpt_paging, common.obj);	/* [한국어] 구체 타입으로 */

	if (!list_empty(&hwpt_paging->hwpt_item)) {	/* [한국어] IOAS 에 연결되어 있으면 */
		mutex_lock(&hwpt_paging->ioas->mutex);	/* [한국어] 그 목록을 지키고 */
		list_del(&hwpt_paging->hwpt_item);	/* [한국어] 빼낸다 */
		mutex_unlock(&hwpt_paging->ioas->mutex);	/* [한국어] 목록 갱신 끝 */

		iopt_table_remove_domain(&hwpt_paging->ioas->iopt,	/* [한국어] 그 도메인에서 */
					 hwpt_paging->common.domain);	/* [한국어] 매핑을 걷어낸다 */
	}

	__iommufd_hwpt_destroy(&hwpt_paging->common);	/* [한국어] 도메인과 폴트 큐를 놓고 */
	refcount_dec(&hwpt_paging->ioas->obj.users);	/* [한국어] 마지막으로 IOAS 참조 — 위의 정리가 모두 IOAS 를 쓴다 */
}

/*
 * [한국어]
 * iommufd_hwpt_paging_abort - 확정 전 페이징 HWPT 를 되돌린다
 *
 * @obj: 되돌릴 객체.
 *
 * 파괴와 거의 같지만 락을 잡지 않는다. 원 주석이 그 이유를 밝힌다 —
 * 호출자가 finalize 까지 ioas->mutex 를 쥐고 있어야 하므로, 여기서 다시
 * 잡으면 교착이다. 그래서 이 종류는 별도의 abort 를 둔다.
 *
 * list_del_init 을 쓰는 이유: 뒤이어 부르는 destroy 가 목록을 다시 보므로
 * 비어 있는 상태로 만들어 두어야 두 번 걷어내지 않는다.
 */
void iommufd_hwpt_paging_abort(struct iommufd_object *obj)
{
	struct iommufd_hwpt_paging *hwpt_paging =	/* [한국어] 공통 객체에서 */
		container_of(obj, struct iommufd_hwpt_paging, common.obj);	/* [한국어] 구체 타입으로 */

	/* The ioas->mutex must be held until finalize is called. */
	lockdep_assert_held(&hwpt_paging->ioas->mutex);	/* [한국어] (원 주석: finalize 까지 이 뮤텍스를 쥐고 있어야 한다) */

	if (!list_empty(&hwpt_paging->hwpt_item)) {	/* [한국어] IOAS 에 연결되어 있으면 */
		list_del_init(&hwpt_paging->hwpt_item);	/* [한국어] 비운 상태로 만든다 — 아래 destroy 가 두 번 걷어내지 않도록 */
		iopt_table_remove_domain(&hwpt_paging->ioas->iopt,	/* [한국어] 그 도메인에서 */
					 hwpt_paging->common.domain);	/* [한국어] 매핑을 걷어낸다 */
	}
	iommufd_hwpt_paging_destroy(obj);	/* [한국어] 나머지 정리는 같다 */
}

/*
 * [한국어]
 * iommufd_hwpt_nested_destroy - 중첩 HWPT 를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * 부모가 vIOMMU 인지 페이징 HWPT 인지에 따라 놓을 참조가 다르다.
 */
void iommufd_hwpt_nested_destroy(struct iommufd_object *obj)
{
	struct iommufd_hwpt_nested *hwpt_nested =	/* [한국어] 공통 객체에서 */
		container_of(obj, struct iommufd_hwpt_nested, common.obj);	/* [한국어] 구체 타입으로 */

	__iommufd_hwpt_destroy(&hwpt_nested->common);	/* [한국어] 도메인과 폴트 큐를 놓는다 */
	if (hwpt_nested->viommu)	/* [한국어] vIOMMU 를 통해 만들었으면 */
		refcount_dec(&hwpt_nested->viommu->obj.users);	/* [한국어] 그 참조를 */
	else
		refcount_dec(&hwpt_nested->parent->common.obj.users);	/* [한국어] 아니면 부모 HWPT 의 참조를 놓는다 */
}

/*
 * [한국어]
 * iommufd_hwpt_nested_abort - 확정 전 중첩 HWPT 를 되돌린다
 *
 * @obj: 되돌릴 객체.
 *
 * 중첩은 IOAS 목록에 들어가지 않아 파괴와 같다. 그래도 abort 를 두는
 * 이유는 코어가 그 존재로 "_ucmd 할당기를 쓸 수 없는 종류"를 판별하기
 * 때문이다 — 이 객체는 호출자의 락 안에서 되돌려야 한다.
 */
void iommufd_hwpt_nested_abort(struct iommufd_object *obj)
{
	iommufd_hwpt_nested_destroy(obj);	/* [한국어] IOAS 목록에 들어가지 않아 파괴와 같다 */
}

static int	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_hwpt_paging_enforce_cc - 도메인에 캐시 일관성 강제를 요청한다
 *
 * @hwpt_paging: 대상 HWPT.
 * @return: 0 성공, -EINVAL 이면 도메인이 거절했다.
 *
 * 장치가 일관성을 강제할 수 있다고 보고했으면 도메인도 그래야 한다.
 * 아래 호출부의 원 주석이 그 계약을 밝힌다 — 능력을 보고해 놓고 여기서
 * 실패하는 것은 드라이버 버그다.
 */
iommufd_hwpt_paging_enforce_cc(struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommu_domain *paging_domain = hwpt_paging->common.domain;	/* [한국어] 대상 도메인 */

	if (hwpt_paging->enforce_cache_coherency)	/* [한국어] 이미 강제되어 있으면 */
		return 0;	/* [한국어] 할 일이 없다 */

	if (paging_domain->ops->enforce_cache_coherency)	/* [한국어] 드라이버가 그 연산을 주면 */
		hwpt_paging->enforce_cache_coherency =	/* [한국어] 불러서 */
			paging_domain->ops->enforce_cache_coherency(	/* [한국어] 결과를 */
				paging_domain);	/* [한국어] 기록한다 */
	if (!hwpt_paging->enforce_cache_coherency)	/* [한국어] 거절했거나 연산이 없으면 */
		return -EINVAL;	/* [한국어] 능력을 보고해 놓고 실패한 것이라 드라이버 버그다 */
	return 0;	/* [한국어] 강제됐다 */
}

/**
 * iommufd_hwpt_paging_alloc() - Get a PAGING iommu_domain for a device
 * @ictx: iommufd context
 * @ioas: IOAS to associate the domain with
 * @idev: Device to get an iommu_domain for
 * @pasid: PASID to get an iommu_domain for
 * @flags: Flags from userspace
 * @immediate_attach: True if idev should be attached to the hwpt
 * @user_data: The user provided driver specific data describing the domain to
 *             create
 *
 * Allocate a new iommu_domain and return it as a hw_pagetable. The HWPT
 * will be linked to the given ioas and upon return the underlying iommu_domain
 * is fully popoulated.
 *
 * The caller must hold the ioas->mutex until after
 * iommufd_object_abort_and_destroy() or iommufd_object_finalize() is called on
 * the returned hwpt.
 */
struct iommufd_hwpt_paging *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iommufd_hwpt_paging_alloc - 장치에 쓸 페이징 도메인을 만든다
 *
 * @ictx: 문맥.
 * @ioas: 매핑을 받아 올 IOAS.
 * @idev: 이 도메인을 쓸 장치.
 * @pasid: 곧바로 붙일 때 쓸 PASID.
 * @flags: 사용자가 준 속성.
 * @immediate_attach: 만들면서 곧바로 붙일 것인가.
 * @user_data: 드라이버별 추가 정보.
 * @return: 새 HWPT, 실패하면 ERR_PTR.
 *
 * 순서가 이 함수의 핵심이다.
 *  1) 도메인을 만든다.
 *  2) 캐시 일관성 모드를 정한다. 원 주석이 이유를 밝힌다 — 형식에 따라
 *     항목마다 그 비트가 있어, 매핑을 넣기 전에 정해야 한다. 그리고 한 번
 *     정하면 바뀌지 않으므로, 일관성을 강제한 HWPT 는 그러지 못하는
 *     장치가 재사용할 수 없다.
 *  3) 필요하면 장치를 붙인다.
 *  4) IOAS 의 매핑을 모두 채운다.
 *
 * immediate_attach 는 원 주석대로 임시방편이다 — 붙이기 전에는 도메인을
 * 완성하지 못하는 드라이버가 있어 그 순서를 강요한다. 그 드라이버들이
 * 고쳐지면 사라질 인자다.
 *
 * 원 주석이 호출자의 의무도 밝힌다: finalize 나 abort 까지 ioas->mutex 를
 * 쥐고 있어야 한다.
 */
iommufd_hwpt_paging_alloc(struct iommufd_ctx *ictx, struct iommufd_ioas *ioas,
			  struct iommufd_device *idev, ioasid_t pasid,
			  u32 flags, bool immediate_attach,
			  const struct iommu_user_data *user_data)
{
	const u32 valid_flags = IOMMU_HWPT_ALLOC_NEST_PARENT |	/* [한국어] 이 경로가 아는 속성들 */
				IOMMU_HWPT_ALLOC_DIRTY_TRACKING |	/* [한국어] 더티 추적 */
				IOMMU_HWPT_FAULT_ID_VALID |	/* [한국어] 폴트 큐 연결 */
				IOMMU_HWPT_ALLOC_PASID;	/* [한국어] PASID 를 붙일 수 있는 도메인 */
	const struct iommu_ops *ops = dev_iommu_ops(idev->dev);	/* [한국어] 그 장치의 드라이버 */
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 만들 객체 */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 그 공통 부분 */
	int rc;	/* [한국어] 결과 */

	lockdep_assert_held(&ioas->mutex);	/* [한국어] 목록을 고치고 finalize 까지 쥐고 있어야 한다 */

	if ((flags || user_data) && !ops->domain_alloc_paging_flags)	/* [한국어] 속성을 주었는데 드라이버가 그것을 받는 연산이 없으면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 요청을 들어줄 수 없다 */
	if (flags & ~valid_flags)	/* [한국어] 모르는 속성이 있으면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 조용히 무시하지 않고 거절한다 */
	if ((flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING) &&	/* [한국어] 더티 추적을 요청했는데 */
	    !device_iommu_capable(idev->dev, IOMMU_CAP_DIRTY_TRACKING))	/* [한국어] 장치가 지원하지 않으면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절 */
	if ((flags & IOMMU_HWPT_FAULT_ID_VALID) &&	/* [한국어] 폴트 큐와 */
	    (flags & IOMMU_HWPT_ALLOC_NEST_PARENT))	/* [한국어] 중첩 부모는 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 함께 쓸 수 없다 — 부모는 폴트를 내지 않는다 */

	hwpt_paging = __iommufd_object_alloc(	/* [한국어] 객체를 만들고 id 를 예약한다 */
		ictx, hwpt_paging, IOMMUFD_OBJ_HWPT_PAGING, common.obj);	/* [한국어] 공통 부분이 맨 앞인지 컴파일 시 확인된다 */
	if (IS_ERR(hwpt_paging))	/* [한국어] 실패면 */
		return ERR_CAST(hwpt_paging);	/* [한국어] 그대로 */
	hwpt = &hwpt_paging->common;	/* [한국어] 공통 부분 */
	hwpt->pasid_compat = flags & IOMMU_HWPT_ALLOC_PASID;	/* [한국어] PASID 를 붙일 수 있는가 */

	INIT_LIST_HEAD(&hwpt_paging->hwpt_item);	/* [한국어] 아직 IOAS 목록에 들어가지 않았다 */
	/* Pairs with iommufd_hw_pagetable_destroy() */
	refcount_inc(&ioas->obj.users);	/* [한국어] (원 주석: iommufd_hw_pagetable_destroy() 와 짝을 이룬다) */
	hwpt_paging->ioas = ioas;	/* [한국어] 매핑을 받아 올 IOAS */
	hwpt_paging->nest_parent = flags & IOMMU_HWPT_ALLOC_NEST_PARENT;	/* [한국어] 중첩의 부모로 쓸 수 있는가 */

	if (ops->domain_alloc_paging_flags) {	/* [한국어] 속성을 받는 연산이 있으면 */
		hwpt->domain = ops->domain_alloc_paging_flags(idev->dev,	/* [한국어] 그것으로 만든다 */
				flags & ~IOMMU_HWPT_FAULT_ID_VALID, user_data);	/* [한국어] 폴트 큐는 드라이버가 알 바가 아니라 뺀다 */
		if (IS_ERR(hwpt->domain)) {	/* [한국어] 실패면 */
			rc = PTR_ERR(hwpt->domain);	/* [한국어] 오류를 전하되 */
			hwpt->domain = NULL;	/* [한국어] 정리 경로가 두 번 해제하지 않도록 */
			goto out_abort;	/* [한국어] 되돌린다 */
		}
		hwpt->domain->owner = ops;	/* [한국어] 어느 드라이버의 도메인인지 */
	} else {
		hwpt->domain = iommu_paging_domain_alloc(idev->dev);	/* [한국어] 옛 방식의 드라이버는 속성 없이 */
		if (IS_ERR(hwpt->domain)) {	/* [한국어] 실패면 */
			rc = PTR_ERR(hwpt->domain);	/* [한국어] 오류를 전하고 */
			hwpt->domain = NULL;	/* [한국어] 두 번 해제하지 않도록 */
			goto out_abort;	/* [한국어] 되돌린다 */
		}
	}
	hwpt->domain->iommufd_hwpt = hwpt;	/* [한국어] 도메인에서 이 객체를 되짚을 수 있게 */
	hwpt->domain->cookie_type = IOMMU_COOKIE_IOMMUFD;	/* [한국어] 그 포인터의 종류를 알린다 */

	/*
	 * Set the coherency mode before we do iopt_table_add_domain() as some
	 * iommus have a per-PTE bit that controls it and need to decide before
	 * doing any maps. It is an iommu driver bug to report
	 * IOMMU_CAP_ENFORCE_CACHE_COHERENCY but fail enforce_cache_coherency on
	 * a new domain.
	 *
	 * The cache coherency mode must be configured here and unchanged later.
	 * Note that a HWPT (non-CC) created for a device (non-CC) can be later
	 * reused by another device (either non-CC or CC). However, A HWPT (CC)
	 * created for a device (CC) cannot be reused by another device (non-CC)
	 * but only devices (CC). Instead user space in this case would need to
	 * allocate a separate HWPT (non-CC).
	 */
	if (idev->enforce_cache_coherency) {	/* [한국어] (원 주석: 매핑을 넣기 전에 일관성 모드를 정해야 한다 — 형식에 따라 항목마다 비트가 있다) */
		rc = iommufd_hwpt_paging_enforce_cc(hwpt_paging);	/* [한국어] 도메인에 강제를 요청한다 */
		if (WARN_ON(rc))	/* [한국어] 능력을 보고해 놓고 실패하면 */
			goto out_abort;	/* [한국어] 드라이버 버그다 */
	}

	/*
	 * immediate_attach exists only to accommodate iommu drivers that cannot
	 * directly allocate a domain. These drivers do not finish creating the
	 * domain until attach is completed. Thus we must have this call
	 * sequence. Once those drivers are fixed this should be removed.
	 */
	if (immediate_attach) {	/* [한국어] (원 주석: 붙이기 전에는 도메인을 완성하지 못하는 드라이버를 위한 임시방편) */
		rc = iommufd_hw_pagetable_attach(hwpt, idev, pasid);	/* [한국어] 먼저 붙인다 */
		if (rc)	/* [한국어] 실패면 */
			goto out_abort;	/* [한국어] 되돌린다 */
	}

	rc = iopt_table_add_domain(&ioas->iopt, hwpt->domain);	/* [한국어] IOAS 의 매핑을 모두 채운다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_detach;	/* [한국어] 붙인 것부터 되돌린다 */
	list_add_tail(&hwpt_paging->hwpt_item, &ioas->hwpt_list);	/* [한국어] 이제 이 IOAS 의 도메인 중 하나다 */
	return hwpt_paging;	/* [한국어] 호출자가 확정한다 */

out_detach:	/* [한국어] 붙였던 장치를 뗀다 */
	if (immediate_attach)	/* [한국어] 붙였었으면 */
		iommufd_hw_pagetable_detach(idev, pasid);	/* [한국어] 뗀다 */
out_abort:	/* [한국어] 만든 도메인과 참조를 되돌린다 */
	iommufd_object_abort_and_destroy(ictx, &hwpt->obj);	/* [한국어] 도메인과 참조를 모두 되돌린다 */
	return ERR_PTR(rc);	/* [한국어] 실패 이유 */
}

/**
 * iommufd_hwpt_nested_alloc() - Get a NESTED iommu_domain for a device
 * @ictx: iommufd context
 * @parent: Parent PAGING-type hwpt to associate the domain with
 * @idev: Device to get an iommu_domain for
 * @flags: Flags from userspace
 * @user_data: user_data pointer. Must be valid
 *
 * Allocate a new iommu_domain (must be IOMMU_DOMAIN_NESTED) and return it as
 * a NESTED hw_pagetable. The given parent PAGING-type hwpt must be capable of
 * being a parent.
 */
static struct iommufd_hwpt_nested *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iommufd_hwpt_nested_alloc - 부모 HWPT 위에 중첩 도메인을 만든다
 *
 * @ictx: 문맥.
 * @parent: 2단계를 맡을 부모 페이징 HWPT.
 * @idev: 이 도메인을 쓸 장치.
 * @flags: 사용자가 준 속성.
 * @user_data: 게스트 표의 위치 등 드라이버별 정보(반드시 있어야 한다).
 * @return: 새 HWPT, 실패하면 ERR_PTR.
 *
 * 부모 자격을 세 가지로 확인한다 — 자동으로 만들어진 것이 아니고,
 * 중첩 부모로 만들어졌으며, 같은 드라이버의 도메인이어야 한다. 자동
 * 도메인을 부모로 삼으면 그것이 언제 사라질지 사용자가 알 수 없다.
 *
 * user_data 가 반드시 필요한 이유: 게스트 페이지 테이블의 주소 같은
 * 정보는 드라이버만 해석할 수 있고, 그것 없이는 중첩 도메인을 만들 수 없다.
 */
iommufd_hwpt_nested_alloc(struct iommufd_ctx *ictx,
			  struct iommufd_hwpt_paging *parent,
			  struct iommufd_device *idev, u32 flags,
			  const struct iommu_user_data *user_data)
{
	const struct iommu_ops *ops = dev_iommu_ops(idev->dev);	/* [한국어] 그 장치의 드라이버 */
	struct iommufd_hwpt_nested *hwpt_nested;	/* [한국어] 만들 객체 */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 그 공통 부분 */
	int rc;	/* [한국어] 결과 */

	if ((flags & ~(IOMMU_HWPT_FAULT_ID_VALID | IOMMU_HWPT_ALLOC_PASID)) ||	/* [한국어] 모르는 속성이거나 */
	    !user_data->len || !ops->domain_alloc_nested)	/* [한국어] 게스트 표 정보가 없거나 드라이버가 중첩을 못 하면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절 */
	if (parent->auto_domain || !parent->nest_parent ||	/* [한국어] 자동 도메인이거나 부모 자격이 없거나 */
	    parent->common.domain->owner != ops)	/* [한국어] 다른 드라이버의 도메인이면 */
		return ERR_PTR(-EINVAL);	/* [한국어] 부모로 삼을 수 없다 */

	hwpt_nested = __iommufd_object_alloc(	/* [한국어] 객체를 만들고 id 를 예약한다 */
		ictx, hwpt_nested, IOMMUFD_OBJ_HWPT_NESTED, common.obj);	/* [한국어] 공통 부분이 맨 앞이어야 한다 */
	if (IS_ERR(hwpt_nested))	/* [한국어] 실패면 */
		return ERR_CAST(hwpt_nested);	/* [한국어] 그대로 */
	hwpt = &hwpt_nested->common;	/* [한국어] 공통 부분 */
	hwpt->pasid_compat = flags & IOMMU_HWPT_ALLOC_PASID;	/* [한국어] PASID 를 붙일 수 있는가 */

	refcount_inc(&parent->common.obj.users);	/* [한국어] 부모가 먼저 사라지면 안 된다 */
	hwpt_nested->parent = parent;	/* [한국어] 2단계를 맡는 도메인 */

	hwpt->domain = ops->domain_alloc_nested(	/* [한국어] 드라이버가 게스트 표를 걸어 준다 */
		idev->dev, parent->common.domain,	/* [한국어] 부모 도메인과 함께 */
		flags & ~IOMMU_HWPT_FAULT_ID_VALID, user_data);	/* [한국어] 폴트 큐는 드라이버가 알 바가 아니다 */
	if (IS_ERR(hwpt->domain)) {	/* [한국어] 실패면 */
		rc = PTR_ERR(hwpt->domain);	/* [한국어] 오류를 전하고 */
		hwpt->domain = NULL;	/* [한국어] 두 번 해제하지 않도록 */
		goto out_abort;	/* [한국어] 되돌린다 */
	}
	hwpt->domain->owner = ops;	/* [한국어] 어느 드라이버의 도메인인지 */
	hwpt->domain->iommufd_hwpt = hwpt;	/* [한국어] 도메인에서 이 객체를 되짚을 수 있게 */
	hwpt->domain->cookie_type = IOMMU_COOKIE_IOMMUFD;	/* [한국어] 그 포인터의 종류 */

	if (WARN_ON_ONCE(hwpt->domain->type != IOMMU_DOMAIN_NESTED)) {	/* [한국어] 드라이버가 중첩이 아닌 도메인을 주면 */
		rc = -EOPNOTSUPP;	/* [한국어] 계약 위반이다 */
		goto out_abort;	/* [한국어] 되돌린다 */
	}
	return hwpt_nested;	/* [한국어] 호출자가 확정한다 */

out_abort:	/* [한국어] 만든 도메인과 참조를 되돌린다 */
	iommufd_object_abort_and_destroy(ictx, &hwpt->obj);	/* [한국어] 도메인과 참조를 되돌린다 */
	return ERR_PTR(rc);	/* [한국어] 실패 이유 */
}

/**
 * iommufd_viommu_alloc_hwpt_nested() - Get a hwpt_nested for a vIOMMU
 * @viommu: vIOMMU ojbect to associate the hwpt_nested/domain with
 * @flags: Flags from userspace
 * @user_data: user_data pointer. Must be valid
 *
 * Allocate a new IOMMU_DOMAIN_NESTED for a vIOMMU and return it as a NESTED
 * hw_pagetable.
 */
static struct iommufd_hwpt_nested *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iommufd_viommu_alloc_hwpt_nested - vIOMMU 위에 중첩 도메인을 만든다
 *
 * @viommu: 이 도메인이 속할 vIOMMU.
 * @flags: 사용자가 준 속성.
 * @user_data: 드라이버별 정보(반드시 있어야 한다).
 * @return: 새 HWPT, 실패하면 ERR_PTR.
 *
 * 위 함수와 하는 일은 같지만 만드는 주체가 다르다. 드라이버가 vIOMMU 의
 * 문맥으로 도메인을 만들어 주므로, 그 vIOMMU 가 아는 게스트 상태를 함께
 * 반영할 수 있다.
 *
 * parent 를 vIOMMU 의 HWPT 로 채워 두는 이유: find_hwpt_paging() 이
 * IOAS 나 MSI 를 찾을 때 그 경로를 쓴다.
 */
iommufd_viommu_alloc_hwpt_nested(struct iommufd_viommu *viommu, u32 flags,
				 const struct iommu_user_data *user_data)
{
	struct iommufd_hwpt_nested *hwpt_nested;	/* [한국어] 만들 객체 */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 그 공통 부분 */
	int rc;	/* [한국어] 결과 */

	if (flags & ~(IOMMU_HWPT_FAULT_ID_VALID | IOMMU_HWPT_ALLOC_PASID))	/* [한국어] 모르는 속성이면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절 */
	if (!user_data->len)	/* [한국어] 게스트 표 정보가 없으면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 중첩 도메인을 만들 수 없다 */
	if (!viommu->ops || !viommu->ops->alloc_domain_nested)	/* [한국어] 그 vIOMMU 가 중첩을 못 하면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절 */

	hwpt_nested = __iommufd_object_alloc(	/* [한국어] 객체를 만들고 id 를 예약한다 */
		viommu->ictx, hwpt_nested, IOMMUFD_OBJ_HWPT_NESTED, common.obj);	/* [한국어] 공통 부분이 맨 앞이어야 한다 */
	if (IS_ERR(hwpt_nested))	/* [한국어] 실패면 */
		return ERR_CAST(hwpt_nested);	/* [한국어] 그대로 */
	hwpt = &hwpt_nested->common;	/* [한국어] 공통 부분 */
	hwpt->pasid_compat = flags & IOMMU_HWPT_ALLOC_PASID;	/* [한국어] PASID 를 붙일 수 있는가 */

	hwpt_nested->viommu = viommu;	/* [한국어] 이 도메인이 속한 vIOMMU */
	refcount_inc(&viommu->obj.users);	/* [한국어] 그것이 먼저 사라지면 안 된다 */
	hwpt_nested->parent = viommu->hwpt;	/* [한국어] find_hwpt_paging() 이 IOAS 를 찾을 때 쓴다 */

	hwpt->domain = viommu->ops->alloc_domain_nested(	/* [한국어] 드라이버가 그 vIOMMU 의 문맥으로 */
		viommu, flags & ~IOMMU_HWPT_FAULT_ID_VALID, user_data);	/* [한국어] 게스트 표를 걸어 준다 */
	if (IS_ERR(hwpt->domain)) {	/* [한국어] 실패면 */
		rc = PTR_ERR(hwpt->domain);	/* [한국어] 오류를 전하고 */
		hwpt->domain = NULL;	/* [한국어] 두 번 해제하지 않도록 */
		goto out_abort;	/* [한국어] 되돌린다 */
	}
	hwpt->domain->iommufd_hwpt = hwpt;	/* [한국어] 도메인에서 이 객체를 되짚을 수 있게 */
	hwpt->domain->owner = viommu->iommu_dev->ops;	/* [한국어] 그 vIOMMU 를 제공한 드라이버 */
	hwpt->domain->cookie_type = IOMMU_COOKIE_IOMMUFD;	/* [한국어] 그 포인터의 종류 */

	if (WARN_ON_ONCE(hwpt->domain->type != IOMMU_DOMAIN_NESTED)) {	/* [한국어] 중첩이 아닌 도메인을 주면 */
		rc = -EOPNOTSUPP;	/* [한국어] 계약 위반이다 */
		goto out_abort;	/* [한국어] 되돌린다 */
	}
	return hwpt_nested;	/* [한국어] 호출자가 확정한다 */

out_abort:	/* [한국어] 만든 도메인과 참조를 되돌린다 */
	iommufd_object_abort_and_destroy(viommu->ictx, &hwpt->obj);	/* [한국어] 도메인과 참조를 되돌린다 */
	return ERR_PTR(rc);	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iommufd_hwpt_alloc - IOMMU_HWPT_ALLOC 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자가 준 pt_id 의 종류가 만들 도메인의 종류를 정한다.
 *  - IOAS 면 페이징 도메인.
 *  - 페이징 HWPT 면 그 위의 중첩 도메인.
 *  - vIOMMU 면 그 문맥의 중첩 도메인.
 *
 * data_type 과 data_len 의 짝을 먼저 검사한다 — 한쪽만 있으면 사용자가
 * 무엇을 원하는지 알 수 없다.
 *
 * 폴트 큐 연결이 마지막에 온다. 그것을 붙이면 도메인의 폴트 처리기가
 * 바뀌어, 이후 이 도메인에서 나는 폴트가 사용자 공간으로 간다.
 *
 * 락 처리가 갈래마다 다르다 — IOAS 갈래만 뮤텍스를 잡으므로, 정리 경로가
 * ioas 포인터로 그 여부를 판별한다.
 */
int iommufd_hwpt_alloc(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_alloc *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	const struct iommu_user_data user_data = {	/* [한국어] 드라이버에 넘길 추가 정보 */
		.type = cmd->data_type,	/* [한국어] 그 형식 */
		.uptr = u64_to_user_ptr(cmd->data_uptr),	/* [한국어] 사용자 버퍼 */
		.len = cmd->data_len,	/* [한국어] 그 길이 */
	};
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 만든 객체의 공통 부분 */
	struct iommufd_ioas *ioas = NULL;	/* [한국어] IOAS 갈래에서만 잡는다 — 정리 경로가 이 값으로 판별한다 */
	struct iommufd_object *pt_obj;	/* [한국어] 사용자가 준 부모 객체 */
	struct iommufd_device *idev;	/* [한국어] 이 도메인을 쓸 장치 */
	int rc;	/* [한국어] 결과 */

	if (cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */
	if ((cmd->data_type == IOMMU_HWPT_DATA_NONE && cmd->data_len) ||	/* [한국어] 형식은 없다는데 길이가 있거나 */
	    (cmd->data_type != IOMMU_HWPT_DATA_NONE && !cmd->data_len))	/* [한국어] 형식은 있는데 길이가 없으면 */
		return -EINVAL;	/* [한국어] 무엇을 원하는지 알 수 없다 */

	idev = iommufd_get_device(ucmd, cmd->dev_id);	/* [한국어] 장치를 붙잡고 */
	if (IS_ERR(idev))	/* [한국어] 없으면 */
		return PTR_ERR(idev);	/* [한국어] 그대로 */

	pt_obj = iommufd_get_object(ucmd->ictx, cmd->pt_id, IOMMUFD_OBJ_ANY);	/* [한국어] 부모는 세 종류일 수 있어 종류를 가리지 않고 찾는다 */
	if (IS_ERR(pt_obj)) {	/* [한국어] 없으면 */
		rc = -EINVAL;	/* [한국어] 거절하되 */
		goto out_put_idev;	/* [한국어] 장치를 놓아야 한다 */
	}

	if (pt_obj->type == IOMMUFD_OBJ_IOAS) {	/* [한국어] IOAS 면 페이징 도메인 */
		struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 만들 객체 */

		ioas = container_of(pt_obj, struct iommufd_ioas, obj);	/* [한국어] 구체 타입으로 */
		mutex_lock(&ioas->mutex);	/* [한국어] 목록을 고치고 확정까지 쥔다 */
		hwpt_paging = iommufd_hwpt_paging_alloc(	/* [한국어] 페이징 도메인을 만든다 */
			ucmd->ictx, ioas, idev, IOMMU_NO_PASID, cmd->flags,	/* [한국어] 기본 붙임이라 PASID 는 없다 */
			false, user_data.len ? &user_data : NULL);	/* [한국어] 곧바로 붙이지는 않는다 */
		if (IS_ERR(hwpt_paging)) {	/* [한국어] 실패면 */
			rc = PTR_ERR(hwpt_paging);	/* [한국어] 오류를 전하되 */
			goto out_unlock;	/* [한국어] 뮤텍스를 놓아야 한다 */
		}
		hwpt = &hwpt_paging->common;	/* [한국어] 공통 부분 */
	} else if (pt_obj->type == IOMMUFD_OBJ_HWPT_PAGING) {	/* [한국어] 페이징 HWPT 면 그 위의 중첩 */
		struct iommufd_hwpt_nested *hwpt_nested;	/* [한국어] 만들 객체 */

		hwpt_nested = iommufd_hwpt_nested_alloc(	/* [한국어] 중첩 도메인을 만든다 */
			ucmd->ictx,	/* [한국어] 문맥 */
			container_of(pt_obj, struct iommufd_hwpt_paging,	/* [한국어] 부모를 */
				     common.obj),	/* [한국어] 구체 타입으로 */
			idev, cmd->flags, &user_data);	/* [한국어] 게스트 표 정보와 함께 */
		if (IS_ERR(hwpt_nested)) {	/* [한국어] 실패면 */
			rc = PTR_ERR(hwpt_nested);	/* [한국어] 오류를 전하고 */
			goto out_unlock;	/* [한국어] 정리 경로로 */
		}
		hwpt = &hwpt_nested->common;	/* [한국어] 공통 부분 */
	} else if (pt_obj->type == IOMMUFD_OBJ_VIOMMU) {	/* [한국어] vIOMMU 면 그 문맥의 중첩 */
		struct iommufd_hwpt_nested *hwpt_nested;	/* [한국어] 만들 객체 */
		struct iommufd_viommu *viommu;	/* [한국어] 그 vIOMMU */

		viommu = container_of(pt_obj, struct iommufd_viommu, obj);	/* [한국어] 구체 타입으로 */
		if (viommu->iommu_dev != __iommu_get_iommu_dev(idev->dev)) {	/* [한국어] 장치가 그 vIOMMU 의 하드웨어에 속하지 않으면 */
			rc = -EINVAL;	/* [한국어] 붙일 수 없다 */
			goto out_unlock;	/* [한국어] 정리 경로로 */
		}
		hwpt_nested = iommufd_viommu_alloc_hwpt_nested(	/* [한국어] 그 vIOMMU 의 문맥으로 */
			viommu, cmd->flags, &user_data);	/* [한국어] 중첩 도메인을 만든다 */
		if (IS_ERR(hwpt_nested)) {	/* [한국어] 실패면 */
			rc = PTR_ERR(hwpt_nested);	/* [한국어] 오류를 전하고 */
			goto out_unlock;	/* [한국어] 정리 경로로 */
		}
		hwpt = &hwpt_nested->common;	/* [한국어] 공통 부분 */
	} else {
		rc = -EINVAL;	/* [한국어] 그 밖의 종류는 부모가 될 수 없다 */
		goto out_put_pt;	/* [한국어] 뮤텍스를 잡지 않았으므로 그쪽으로 */
	}

	if (cmd->flags & IOMMU_HWPT_FAULT_ID_VALID) {	/* [한국어] 폴트 큐를 연결하라고 했으면 */
		struct iommufd_fault *fault;	/* [한국어] 그 큐 */

		fault = iommufd_get_fault(ucmd, cmd->fault_id);	/* [한국어] 붙잡고 */
		if (IS_ERR(fault)) {	/* [한국어] 없으면 */
			rc = PTR_ERR(fault);	/* [한국어] 오류를 전하되 */
			goto out_hwpt;	/* [한국어] 만든 도메인을 되돌려야 한다 */
		}
		hwpt->fault = fault;	/* [한국어] 이 도메인의 폴트가 갈 곳 */
		hwpt->domain->iopf_handler = iommufd_fault_iopf_handler;	/* [한국어] 폴트를 사용자 공간으로 보내는 처리기 */
		refcount_inc(&fault->common.obj.users);	/* [한국어] 큐가 먼저 사라지면 안 된다 */
		iommufd_put_object(ucmd->ictx, &fault->common.obj);	/* [한국어] 조회 참조는 놓는다 — 위에서 따로 잡았다 */
	}

	cmd->out_hwpt_id = hwpt->obj.id;	/* [한국어] 사용자가 이후 이 id 로 부른다 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 먼저 돌려주고 */
	if (rc)	/* [한국어] 복사에 실패하면 */
		goto out_hwpt;	/* [한국어] 만든 것을 되돌린다 */
	iommufd_object_finalize(ucmd->ictx, &hwpt->obj);	/* [한국어] 이제 사용자에게 보인다 */
	goto out_unlock;	/* [한국어] 정리 경로로 — 성공도 같은 길을 지난다 */

out_hwpt:	/* [한국어] 만든 HWPT 를 되돌린다 */
	iommufd_object_abort_and_destroy(ucmd->ictx, &hwpt->obj);	/* [한국어] 만든 도메인과 참조를 되돌린다 */
out_unlock:	/* [한국어] IOAS 뮤텍스를 놓는다(잡았을 때만) */
	if (ioas)	/* [한국어] IOAS 갈래였으면 */
		mutex_unlock(&ioas->mutex);	/* [한국어] 뮤텍스를 놓는다 */
out_put_pt:	/* [한국어] 부모 객체를 놓는다 */
	iommufd_put_object(ucmd->ictx, pt_obj);	/* [한국어] 부모 객체를 놓고 */
out_put_idev:	/* [한국어] 장치를 놓는다 */
	iommufd_put_object(ucmd->ictx, &idev->obj);	/* [한국어] 장치도 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_hwpt_set_dirty_tracking - 더티 추적을 켜고 끈다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 페이징 HWPT 에만 뜻이 있다 — 중첩 도메인은 자기 매핑을 갖지 않는다.
 */
int iommufd_hwpt_set_dirty_tracking(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_set_dirty_tracking *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 대상 HWPT */
	struct iommufd_ioas *ioas;	/* [한국어] 그 IOAS */
	int rc = -EOPNOTSUPP;	/* [한국어] 기본 실패값 */
	bool enable;	/* [한국어] 켤 것인가 */

	if (cmd->flags & ~IOMMU_HWPT_DIRTY_TRACKING_ENABLE)	/* [한국어] 모르는 플래그가 있으면 */
		return rc;	/* [한국어] 거절 */

	hwpt_paging = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);	/* [한국어] 페이징 HWPT 만 대상이다 */
	if (IS_ERR(hwpt_paging))	/* [한국어] 없거나 중첩이면 */
		return PTR_ERR(hwpt_paging);	/* [한국어] 그대로 */

	ioas = hwpt_paging->ioas;	/* [한국어] 그 IOAS */
	enable = cmd->flags & IOMMU_HWPT_DIRTY_TRACKING_ENABLE;	/* [한국어] 켜라는 요청인가 */

	rc = iopt_set_dirty_tracking(&ioas->iopt, hwpt_paging->common.domain,	/* [한국어] 켤 때는 먼저 지워 깨끗한 스냅숏을 만든다 */
				     enable);	/* [한국어] 그 도메인에 */

	iommufd_put_object(ucmd->ictx, &hwpt_paging->common.obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_hwpt_get_dirty_bitmap - 더티 비트를 사용자 비트맵으로 옮긴다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * NO_CLEAR 를 주면 읽기만 하고 표시를 남겨 둔다 — 여러 번 읽어야 하는
 * 경우를 위한 것이다.
 */
int iommufd_hwpt_get_dirty_bitmap(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_get_dirty_bitmap *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 대상 HWPT */
	struct iommufd_ioas *ioas;	/* [한국어] 그 IOAS */
	int rc = -EOPNOTSUPP;	/* [한국어] 기본 실패값 */

	if ((cmd->flags & ~(IOMMU_HWPT_GET_DIRTY_BITMAP_NO_CLEAR)) ||	/* [한국어] 모르는 플래그거나 */
	    cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	hwpt_paging = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);	/* [한국어] 페이징 HWPT 만 대상이다 */
	if (IS_ERR(hwpt_paging))	/* [한국어] 없거나 중첩이면 */
		return PTR_ERR(hwpt_paging);	/* [한국어] 그대로 */

	ioas = hwpt_paging->ioas;	/* [한국어] 그 IOAS */
	rc = iopt_read_and_clear_dirty_data(	/* [한국어] 더티 비트를 사용자 비트맵으로 옮긴다 */
		&ioas->iopt, hwpt_paging->common.domain, cmd->flags, cmd);	/* [한국어] NO_CLEAR 면 읽기만 한다 */

	iommufd_put_object(ucmd->ictx, &hwpt_paging->common.obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_hwpt_invalidate - 게스트가 낸 무효화 명령을 하드웨어에 전한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 중첩 변환에서 게스트가 자기 페이지 테이블을 고치면 그 캐시를 지워야
 * 하는데, 게스트는 하드웨어를 직접 건드릴 수 없다. 그래서 VMM 이 그
 * 명령을 이 ioctl 로 넘긴다.
 *
 * 두 갈래가 있다. 중첩 HWPT 에 대한 명령은 그 도메인의 드라이버가,
 * vIOMMU 에 대한 명령은 그 vIOMMU 가 처리한다. 후자는 게스트가 장치 id 를
 * 쓰는 명령까지 다룰 수 있다.
 *
 * 처리한 개수를 늘 돌려주는 것이 이 함수의 규약이다 — 배열의 일부만
 * 처리하고 실패할 수 있으므로, 사용자가 어디서 멈췄는지 알아야 다시
 * 시도할 수 있다. 그래서 오류 경로에서도 응답을 보낸다.
 */
int iommufd_hwpt_invalidate(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_invalidate *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommu_user_data_array data_array = {	/* [한국어] 게스트가 낸 명령들의 배열 */
		.type = cmd->data_type,	/* [한국어] 그 형식 */
		.uptr = u64_to_user_ptr(cmd->data_uptr),	/* [한국어] 사용자 버퍼 */
		.entry_len = cmd->entry_len,	/* [한국어] 항목 하나의 크기 */
		.entry_num = cmd->entry_num,	/* [한국어] 항목 수 */
	};
	struct iommufd_object *pt_obj;	/* [한국어] 대상 객체 */
	u32 done_num = 0;	/* [한국어] 실제로 처리한 개수 */
	int rc;	/* [한국어] 결과 */

	if (cmd->__reserved) {	/* [한국어] 예약 필드가 0 이 아니면 */
		rc = -EOPNOTSUPP;	/* [한국어] 거절하되 */
		goto out;	/* [한국어] 개수는 돌려주어야 한다 */
	}

	if (cmd->entry_num && (!cmd->data_uptr || !cmd->entry_len)) {	/* [한국어] 항목이 있다면서 버퍼나 크기가 없으면 */
		rc = -EINVAL;	/* [한국어] 읽을 수 없다 */
		goto out;	/* [한국어] 응답 경로로 */
	}

	pt_obj = iommufd_get_object(ucmd->ictx, cmd->hwpt_id, IOMMUFD_OBJ_ANY);	/* [한국어] 대상은 두 종류일 수 있다 */
	if (IS_ERR(pt_obj)) {	/* [한국어] 없으면 */
		rc = PTR_ERR(pt_obj);	/* [한국어] 오류를 전하되 */
		goto out;	/* [한국어] 응답 경로로 */
	}
	if (pt_obj->type == IOMMUFD_OBJ_HWPT_NESTED) {	/* [한국어] 중첩 HWPT 면 */
		struct iommufd_hw_pagetable *hwpt =	/* [한국어] 구체 타입으로 */
			container_of(pt_obj, struct iommufd_hw_pagetable, obj);	/* [한국어] 되짚는다 */

		if (!hwpt->domain->ops ||	/* [한국어] 드라이버가 */
		    !hwpt->domain->ops->cache_invalidate_user) {	/* [한국어] 그 연산을 주지 않으면 */
			rc = -EOPNOTSUPP;	/* [한국어] 전할 방법이 없다 */
			goto out_put_pt;	/* [한국어] 객체를 놓는다 */
		}
		rc = hwpt->domain->ops->cache_invalidate_user(hwpt->domain,	/* [한국어] 드라이버가 게스트 명령을 해석해 */
							      &data_array);	/* [한국어] 하드웨어에 전한다 */
	} else if (pt_obj->type == IOMMUFD_OBJ_VIOMMU) {	/* [한국어] vIOMMU 면 */
		struct iommufd_viommu *viommu =	/* [한국어] 구체 타입으로 */
			container_of(pt_obj, struct iommufd_viommu, obj);	/* [한국어] 되짚는다 */

		if (!viommu->ops || !viommu->ops->cache_invalidate) {	/* [한국어] 그 연산이 없으면 */
			rc = -EOPNOTSUPP;	/* [한국어] 전할 방법이 없다 */
			goto out_put_pt;	/* [한국어] 객체를 놓는다 */
		}
		rc = viommu->ops->cache_invalidate(viommu, &data_array);	/* [한국어] 게스트 장치 id 를 쓰는 명령까지 다룰 수 있다 */
	} else {
		rc = -EINVAL;	/* [한국어] 그 밖의 종류는 무효화 대상이 아니다 */
		goto out_put_pt;	/* [한국어] 객체를 놓는다 */
	}

	done_num = data_array.entry_num;	/* [한국어] 드라이버가 처리한 개수를 담아 둔다 */

out_put_pt:	/* [한국어] 부모 객체를 놓는다 */
	iommufd_put_object(ucmd->ictx, pt_obj);	/* [한국어] 객체를 놓는다 */
out:	/* [한국어] 처리 개수를 돌려주고 나가는 공통 경로 */
	cmd->entry_num = done_num;	/* [한국어] 일부만 처리하고 실패할 수 있어 늘 알려 준다 */
	if (iommufd_ucmd_respond(ucmd, sizeof(*cmd)))	/* [한국어] 응답에 실패하면 */
		return -EFAULT;	/* [한국어] 원래 오류보다 이쪽이 앞선다 */
	return rc;	/* [한국어] 처리 결과 */
}
