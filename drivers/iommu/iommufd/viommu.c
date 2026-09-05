// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] vIOMMU·가상 장치·하드웨어 큐 객체 (viommu.c)
 *
 * === 파일의 역할 ===
 * 게스트에게 IOMMU 자체를 보여 주는 구성이 여기서 만들어진다.
 *
 * 세 객체가 층을 이룬다.
 *  - vIOMMU: 게스트가 보는 IOMMU 하나. 드라이버가 자기 상태를 덧붙일 수
 *    있게 크기를 스스로 정하고, 그 뒤에 core 구조체를 앞에 둔 확장 구조체를
 *    쓴다.
 *  - vdevice: 게스트가 아는 장치 id 와 호스트의 실제 장치를 잇는다. 게스트가
 *    낸 무효화 명령에 적힌 id 를 드라이버가 해석할 때 이 대응이 쓰인다.
 *  - hw_queue: 게스트가 직접 쓰는 하드웨어 큐. 게스트 메모리에 있는 큐를
 *    하드웨어가 물리 주소로 읽으므로, 그 페이지가 연속이어야 하고 고정되어
 *    있어야 한다.
 *
 * 마지막 것이 이 파일에서 가장 미묘하다. 원 주석이 두 가지를 짚는다 —
 * 물리 접근이라 페이지가 연속이어야 하고, 사용자가 그사이 IOAS_UNMAP 을
 * 부를 수 있어 커널이 따로 고정해 두어야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자(VMM) → IOMMU_VIOMMU_ALLOC / VDEVICE_ALLOC / HW_QUEUE_ALLOC
 *   → [이 파일] → 드라이버의 viommu_init / vdevice_init / hw_queue_init_phys
 *   → 하드웨어의 중첩 변환 자원
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * === 타 모듈과의 연결 ===
 * 위: main.c 의 ioctl 분배.
 * 아래: 드라이버의 vIOMMU 연산(drivers/iommu/amd/nested.c,
 *       arm-smmu-v3-iommufd.c 등), device.c 의 커널 접근 API.
 *
 * === 주요 함수/구조체 요약 ===
 * iommufd_viommu_alloc_ioctl: 드라이버가 정한 크기로 vIOMMU 를 만든다.
 * iommufd_vdevice_alloc_ioctl: 게스트 id ↔ 실제 장치 대응을 만든다.
 *   장치와 가상 장치가 서로를 가리키며 수명을 얽는다.
 * iommufd_hw_queue_alloc_phys: 게스트 큐의 페이지를 고정하고 연속인지
 *   확인한다.
 * iommufd_hw_queue_alloc_ioctl: 그 결과를 드라이버에 넘겨 큐를 세운다.
 */
#include "iommufd_private.h"	/* [한국어] 객체 모델과 내부 API */

/*
 * [한국어]
 * iommufd_viommu_destroy - vIOMMU 를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * 드라이버가 자기 상태를 정리할 기회를 먼저 준 뒤, 부모 HWPT 참조를 놓고
 * 가상 장치 배열을 버린다. 그 배열은 이 시점에 비어 있어야 한다 —
 * vdevice 들이 vIOMMU 참조를 들고 있어 그것이 먼저 사라진다.
 */
void iommufd_viommu_destroy(struct iommufd_object *obj)
{
	struct iommufd_viommu *viommu =	/* [한국어] 공통 객체에서 */
		container_of(obj, struct iommufd_viommu, obj);	/* [한국어] 구체 타입으로 */

	if (viommu->ops && viommu->ops->destroy)	/* [한국어] 드라이버가 정리할 것이 있으면 */
		viommu->ops->destroy(viommu);	/* [한국어] 먼저 기회를 준다 */
	refcount_dec(&viommu->hwpt->common.obj.users);	/* [한국어] 부모 HWPT 참조를 놓고 */
	xa_destroy(&viommu->vdevs);	/* [한국어] 가상 장치 배열을 버린다 — 이 시점에 비어 있어야 한다 */
}

/*
 * [한국어]
 * iommufd_viommu_alloc_ioctl - IOMMU_VIOMMU_ALLOC 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 크기를 드라이버가 정하는 것이 이 객체의 특징이다. 드라이버는 core
 * 구조체를 맨 앞에 둔 자기 구조체를 쓰므로, 그 전체 크기를 물어 그만큼
 * 할당한다.
 *
 * 부모가 중첩 부모로 만들어진 페이징 HWPT 여야 한다 — 그것이 게스트
 * 물리를 호스트 물리로 옮기는 2단계를 맡는다.
 *
 * iommu_dev 를 잡아 두는 이유가 원 주석에 있다 — 물리 IOMMU 는 대개 뽑을
 * 수 없다고 보고, 뽑을 수 있는 구현이 있다면 그쪽이 스스로 참조를 관리한다.
 */
int iommufd_viommu_alloc_ioctl(struct iommufd_ucmd *ucmd)
{
	struct iommu_viommu_alloc *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	const struct iommu_user_data user_data = {	/* [한국어] 드라이버에 넘길 추가 정보 */
		.type = cmd->type,	/* [한국어] 그 형식 */
		.uptr = u64_to_user_ptr(cmd->data_uptr),	/* [한국어] 사용자 버퍼 */
		.len = cmd->data_len,	/* [한국어] 그 길이 */
	};
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 2단계를 맡을 부모 */
	struct iommufd_viommu *viommu;	/* [한국어] 만들 객체 */
	struct iommufd_device *idev;	/* [한국어] 어느 하드웨어인지 알기 위한 장치 */
	const struct iommu_ops *ops;	/* [한국어] 그 드라이버 */
	size_t viommu_size;	/* [한국어] 드라이버가 정하는 크기 */
	int rc;	/* [한국어] 결과 */

	if (cmd->flags || cmd->type == IOMMU_VIOMMU_TYPE_DEFAULT)	/* [한국어] 플래그가 있거나 종류를 정하지 않았으면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	idev = iommufd_get_device(ucmd, cmd->dev_id);	/* [한국어] 장치를 붙잡고 */
	if (IS_ERR(idev))	/* [한국어] 없으면 */
		return PTR_ERR(idev);	/* [한국어] 그대로 */

	ops = dev_iommu_ops(idev->dev);	/* [한국어] 그 장치의 드라이버 */
	if (!ops->get_viommu_size || !ops->viommu_init) {	/* [한국어] vIOMMU 를 지원하지 않으면 */
		rc = -EOPNOTSUPP;	/* [한국어] 거절하되 */
		goto out_put_idev;	/* [한국어] 장치를 놓아야 한다 */
	}

	viommu_size = ops->get_viommu_size(idev->dev, cmd->type);	/* [한국어] 드라이버가 자기 상태까지 담을 크기를 말한다 */
	if (!viommu_size) {	/* [한국어] 그 종류를 지원하지 않으면 0 이다 */
		rc = -EOPNOTSUPP;	/* [한국어] 거절하되 */
		goto out_put_idev;	/* [한국어] 장치를 놓아야 한다 */
	}

	/*
	 * It is a driver bug for providing a viommu_size smaller than the core
	 * vIOMMU structure size
	 */
	if (WARN_ON_ONCE(viommu_size < sizeof(*viommu))) {	/* [한국어] (원 주석: core 구조체보다 작은 크기를 말하는 것은 드라이버 버그다) */
		rc = -EOPNOTSUPP;	/* [한국어] 그대로 쓰면 메모리를 넘어 쓴다 */
		goto out_put_idev;	/* [한국어] 거절 */
	}

	hwpt_paging = iommufd_get_hwpt_paging(ucmd, cmd->hwpt_id);	/* [한국어] 부모를 붙잡고 */
	if (IS_ERR(hwpt_paging)) {	/* [한국어] 없으면 */
		rc = PTR_ERR(hwpt_paging);	/* [한국어] 오류를 전하되 */
		goto out_put_idev;	/* [한국어] 장치를 놓아야 한다 */
	}

	if (!hwpt_paging->nest_parent) {	/* [한국어] 중첩 부모로 만들어지지 않았으면 */
		rc = -EINVAL;	/* [한국어] 2단계를 맡을 수 없다 */
		goto out_put_hwpt;	/* [한국어] 두 객체를 놓는다 */
	}

	viommu = (struct iommufd_viommu *)_iommufd_object_alloc_ucmd(	/* [한국어] ioctl 이 끝날 때 코어가 확정하거나 되돌린다 */
		ucmd, viommu_size, IOMMUFD_OBJ_VIOMMU);	/* [한국어] 드라이버가 말한 크기로 */
	if (IS_ERR(viommu)) {	/* [한국어] 실패면 */
		rc = PTR_ERR(viommu);	/* [한국어] 오류를 전하되 */
		goto out_put_hwpt;	/* [한국어] 두 객체를 놓아야 한다 */
	}

	xa_init(&viommu->vdevs);	/* [한국어] 게스트 id → 가상 장치 대응 */
	viommu->type = cmd->type;	/* [한국어] 드라이버가 아는 종류 */
	viommu->ictx = ucmd->ictx;	/* [한국어] 문맥 */
	viommu->hwpt = hwpt_paging;	/* [한국어] 2단계를 맡는 부모 */
	refcount_inc(&viommu->hwpt->common.obj.users);	/* [한국어] 그것이 먼저 사라지면 안 된다 */
	INIT_LIST_HEAD(&viommu->veventqs);	/* [한국어] 아직 구독된 이벤트 큐가 없다 */
	init_rwsem(&viommu->veventqs_rwsem);	/* [한국어] 그 목록을 지킬 락 */
	/*
	 * It is the most likely case that a physical IOMMU is unpluggable. A
	 * pluggable IOMMU instance (if exists) is responsible for refcounting
	 * on its own.
	 */
	viommu->iommu_dev = __iommu_get_iommu_dev(idev->dev);	/* [한국어] (원 주석: 물리 IOMMU 는 대개 뽑을 수 없고, 뽑을 수 있는 구현은 스스로 참조를 관리한다) */

	rc = ops->viommu_init(viommu, hwpt_paging->common.domain,	/* [한국어] 드라이버가 자기 상태를 채우고 */
			      user_data.len ? &user_data : NULL);	/* [한국어] ops 를 붙인다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_put_hwpt;	/* [한국어] 코어가 되돌린다 */

	/* It is a driver bug that viommu->ops isn't filled */
	if (WARN_ON_ONCE(!viommu->ops)) {	/* [한국어] (원 주석: viommu->ops 를 채우지 않는 것은 드라이버 버그다) */
		rc = -EOPNOTSUPP;	/* [한국어] 이후 경로가 모두 그것을 쓴다 */
		goto out_put_hwpt;	/* [한국어] 거절 */
	}

	cmd->out_viommu_id = viommu->obj.id;	/* [한국어] 사용자가 이후 이 id 로 부른다 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 돌려준다 */

out_put_hwpt:	/* [한국어] 부모 HWPT 를 놓는다 */
	iommufd_put_object(ucmd->ictx, &hwpt_paging->common.obj);	/* [한국어] 조회 참조를 놓는다 — 위에서 따로 잡았다 */
out_put_idev:	/* [한국어] 장치를 놓는다 */
	iommufd_put_object(ucmd->ictx, &idev->obj);	/* [한국어] 장치도 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_vdevice_abort - 가상 장치를 되돌린다
 *
 * @obj: 되돌릴 객체.
 *
 * 확정 전 되돌리기와 파괴가 공유하는 본체다. 그룹 락을 호출자가 쥐고
 * 있어야 하므로 별도의 abort 로 둔다.
 *
 * xa_cmpxchg 가 실패해도 되는 이유를 원 주석이 밝힌다 — 배열에 넣기
 * 전에 실패한 경우에도 이 경로를 지나기 때문이다.
 *
 * 마지막에 장치의 역방향 포인터를 끊는다. 그것이 NULL 이 되어야 장치가
 * 해제될 수 있다.
 */
void iommufd_vdevice_abort(struct iommufd_object *obj)
{
	struct iommufd_vdevice *vdev =	/* [한국어] 공통 객체에서 */
		container_of(obj, struct iommufd_vdevice, obj);	/* [한국어] 구체 타입으로 */
	struct iommufd_viommu *viommu = vdev->viommu;	/* [한국어] 속한 vIOMMU */
	struct iommufd_device *idev = vdev->idev;	/* [한국어] 대응하는 실제 장치 */

	lockdep_assert_held(&idev->igroup->lock);	/* [한국어] 장치의 역방향 포인터를 그 락이 지킨다 */

	if (vdev->destroy)	/* [한국어] 드라이버가 정리할 것이 있으면 */
		vdev->destroy(vdev);	/* [한국어] 먼저 기회를 준다 */
	/* xa_cmpxchg is okay to fail if alloc failed xa_cmpxchg previously */
	xa_cmpxchg(&viommu->vdevs, vdev->virt_id, vdev, NULL, GFP_KERNEL);	/* [한국어] (원 주석: 앞서 xa_cmpxchg 가 실패한 경우에도 이 경로를 지나므로 실패해도 된다) */
	refcount_dec(&viommu->obj.users);	/* [한국어] vIOMMU 참조를 놓고 */
	idev->vdev = NULL;	/* [한국어] 장치가 해제될 수 있게 한다 */
}

/*
 * [한국어]
 * iommufd_vdevice_destroy - 가상 장치를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * abort 와 같되 락을 여기서 잡는다. 그리고 만들 때 잡아 둔 장치 참조를
 * 마지막에 놓는다 — 그 참조가 장치의 파괴를 이 시점까지 미뤄 왔다.
 */
void iommufd_vdevice_destroy(struct iommufd_object *obj)
{
	struct iommufd_vdevice *vdev =	/* [한국어] 공통 객체에서 */
		container_of(obj, struct iommufd_vdevice, obj);	/* [한국어] 구체 타입으로 */
	struct iommufd_device *idev = vdev->idev;	/* [한국어] 대응하는 실제 장치 */
	struct iommufd_ctx *ictx = idev->ictx;	/* [한국어] 장치를 놓을 때 필요하다 */

	mutex_lock(&idev->igroup->lock);	/* [한국어] abort 가 요구하는 락 */
	iommufd_vdevice_abort(obj);	/* [한국어] 본체는 같다 */
	mutex_unlock(&idev->igroup->lock);	/* [한국어] 락을 놓고 */
	iommufd_put_object(ictx, &idev->obj);	/* [한국어] 만들 때 잡아 둔 장치 참조를 놓는다 */
}

/*
 * [한국어]
 * iommufd_vdevice_alloc_ioctl - IOMMU_VDEVICE_ALLOC 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 게스트가 아는 장치 id 와 호스트의 실제 장치를 잇는다. 드라이버는 그
 * 대응으로 게스트가 낸 무효화 명령의 대상을 찾는다.
 *
 * 수명이 서로 얽히는 것이 이 함수의 까다로운 부분이다. 원 주석 둘이 그
 * 규약을 밝힌다.
 *  - 가상 장치가 장치 포인터를 드는 동안 장치의 wait_cnt 참조가 유지되고,
 *    iommufd_device_pre_destroy() 가 그것을 끊는다.
 *  - 장치의 파괴는 idev->vdev 가 NULL 이 될 때까지 기다린다.
 * 그래서 어느 쪽을 먼저 지우든 다른 쪽이 사라진 포인터를 보지 않는다.
 *
 * 한 장치에 가상 장치가 둘일 수 없고, 한 vIOMMU 안에서 같은 게스트 id 도
 * 하나뿐이다 — 두 검사가 그것을 막는다.
 *
 * 성공 경로에서 장치 참조를 놓지 않는 점에 유의. 그 참조가 가상 장치의
 * 수명 동안 유지되어야 한다.
 */
int iommufd_vdevice_alloc_ioctl(struct iommufd_ucmd *ucmd)
{
	struct iommu_vdevice_alloc *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_vdevice *vdev, *curr;	/* [한국어] 만들 객체와 경합으로 먼저 들어간 것 */
	size_t vdev_size = sizeof(*vdev);	/* [한국어] 기본 크기 — 드라이버가 늘릴 수 있다 */
	struct iommufd_viommu *viommu;	/* [한국어] 속할 vIOMMU */
	struct iommufd_device *idev;	/* [한국어] 대응할 실제 장치 */
	u64 virt_id = cmd->virt_id;	/* [한국어] 게스트가 쓸 id */
	int rc = 0;	/* [한국어] 결과 */

	/* virt_id indexes an xarray */
	if (virt_id > ULONG_MAX)	/* [한국어] (원 주석: virt_id 는 xarray 의 색인이다) */
		return -EINVAL;	/* [한국어] 그 폭을 넘을 수 없다 */

	viommu = iommufd_get_viommu(ucmd, cmd->viommu_id);	/* [한국어] vIOMMU 를 붙잡고 */
	if (IS_ERR(viommu))	/* [한국어] 없으면 */
		return PTR_ERR(viommu);	/* [한국어] 그대로 */

	idev = iommufd_get_device(ucmd, cmd->dev_id);	/* [한국어] 장치도 붙잡는다 */
	if (IS_ERR(idev)) {	/* [한국어] 없으면 */
		rc = PTR_ERR(idev);	/* [한국어] 오류를 전하되 */
		goto out_put_viommu;	/* [한국어] vIOMMU 를 놓아야 한다 */
	}

	if (viommu->iommu_dev != __iommu_get_iommu_dev(idev->dev)) {	/* [한국어] 다른 하드웨어의 장치면 */
		rc = -EINVAL;	/* [한국어] 그 vIOMMU 가 다룰 수 없다 */
		goto out_put_idev;	/* [한국어] 두 객체를 놓는다 */
	}

	mutex_lock(&idev->igroup->lock);	/* [한국어] 장치의 역방향 포인터를 지킨다 */
	if (idev->destroying) {	/* [한국어] 장치가 사라지는 중이면 */
		rc = -ENOENT;	/* [한국어] 붙일 수 없다 */
		goto out_unlock_igroup;	/* [한국어] 락을 놓는다 */
	}

	if (idev->vdev) {	/* [한국어] 이미 가상 장치가 있으면 */
		rc = -EEXIST;	/* [한국어] 한 장치에 둘일 수 없다 */
		goto out_unlock_igroup;	/* [한국어] 락을 놓는다 */
	}

	if (viommu->ops && viommu->ops->vdevice_size) {	/* [한국어] 드라이버가 크기를 정하면 */
		/*
		 * It is a driver bug for:
		 * - ops->vdevice_size smaller than the core structure size
		 * - not implementing a pairing ops->vdevice_init op
		 */
		if (WARN_ON_ONCE(viommu->ops->vdevice_size < vdev_size ||	/* [한국어] (원 주석: core 구조체보다 작은 크기이거나 짝이 되는 init 을 두지 않는 것은 드라이버 버그다) */
				 !viommu->ops->vdevice_init)) {	/* [한국어] 둘 중 하나라도 어긋나면 */
			rc = -EOPNOTSUPP;	/* [한국어] 거절 */
			goto out_put_idev;	/* [한국어] 두 객체를 놓는다 */
		}
		vdev_size = viommu->ops->vdevice_size;	/* [한국어] 드라이버가 자기 상태까지 담을 크기 */
	}

	vdev = (struct iommufd_vdevice *)_iommufd_object_alloc(	/* [한국어] 이 객체는 abort 를 가져 _ucmd 할당기를 쓸 수 없다 */
		ucmd->ictx, vdev_size, IOMMUFD_OBJ_VDEVICE);	/* [한국어] 드라이버가 말한 크기로 */
	if (IS_ERR(vdev)) {	/* [한국어] 실패면 */
		rc = PTR_ERR(vdev);	/* [한국어] 오류를 전하되 */
		goto out_unlock_igroup;	/* [한국어] 락을 놓아야 한다 */
	}

	vdev->virt_id = virt_id;	/* [한국어] 게스트가 쓸 id */
	vdev->viommu = viommu;	/* [한국어] 속한 vIOMMU */
	refcount_inc(&viommu->obj.users);	/* [한국어] 그것이 먼저 사라지면 안 된다 */
	/*
	 * A wait_cnt reference is held on the idev so long as we have the
	 * pointer. iommufd_device_pre_destroy() will revoke it before the
	 * idev real destruction.
	 */
	vdev->idev = idev;	/* [한국어] (원 주석: 이 포인터를 드는 동안 장치의 wait_cnt 참조가 유지되고, pre_destroy 가 그것을 끊는다) */

	/*
	 * iommufd_device_destroy() delays until idev->vdev is NULL before
	 * freeing the idev, which only happens once the vdev is finished
	 * destruction.
	 */
	idev->vdev = vdev;	/* [한국어] (원 주석: 장치의 해제는 idev->vdev 가 NULL 이 될 때까지 미뤄진다) */

	curr = xa_cmpxchg(&viommu->vdevs, virt_id, NULL, vdev, GFP_KERNEL);	/* [한국어] 비어 있을 때만 넣는다 */
	if (curr) {	/* [한국어] 같은 게스트 id 가 이미 있으면 */
		rc = xa_err(curr) ?: -EEXIST;	/* [한국어] 메모리 부족이거나 중복이다 */
		goto out_abort;	/* [한국어] 되돌린다 */
	}

	if (viommu->ops && viommu->ops->vdevice_init) {	/* [한국어] 드라이버가 초기화할 것이 있으면 */
		rc = viommu->ops->vdevice_init(vdev);	/* [한국어] 기회를 준다 */
		if (rc)	/* [한국어] 실패면 */
			goto out_abort;	/* [한국어] 되돌린다 */
	}

	cmd->out_vdevice_id = vdev->obj.id;	/* [한국어] 사용자가 이후 이 id 로 부른다 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 먼저 돌려주고 */
	if (rc)	/* [한국어] 복사에 실패하면 */
		goto out_abort;	/* [한국어] 되돌린다 */
	iommufd_object_finalize(ucmd->ictx, &vdev->obj);	/* [한국어] 이제 사용자에게 보인다 */
	goto out_unlock_igroup;	/* [한국어] 성공도 같은 정리 경로를 지난다 */

out_abort:	/* [한국어] 만든 가상 장치를 되돌린다 */
	iommufd_object_abort_and_destroy(ucmd->ictx, &vdev->obj);	/* [한국어] 드라이버 정리까지 되돌린다 */
out_unlock_igroup:	/* [한국어] 그룹 락을 놓는다 */
	mutex_unlock(&idev->igroup->lock);	/* [한국어] 역방향 포인터 보호 해제 */
out_put_idev:	/* [한국어] 실패했을 때만 장치를 놓는다 */
	if (rc)	/* [한국어] 실패했을 때만 */
		iommufd_put_object(ucmd->ictx, &idev->obj);	/* [한국어] 장치를 놓는다 — 성공하면 가상 장치가 그 참조를 든다 */
out_put_viommu:	/* [한국어] vIOMMU 를 놓는다 */
	iommufd_put_object(ucmd->ictx, &viommu->obj);	/* [한국어] vIOMMU 조회 참조를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_hw_queue_destroy_access - 큐 메모리 고정을 푼다
 *
 * @ictx: 문맥.
 * @access: 고정에 쓴 커널 접근.
 * @base_iova: 큐의 IOVA.
 * @length: 그 길이.
 *
 * 고정할 때 페이지 경계로 내려 잡았으므로 풀 때도 같은 범위여야 한다 —
 * 그래서 오프셋을 되계산한다.
 */
static void iommufd_hw_queue_destroy_access(struct iommufd_ctx *ictx,
					    struct iommufd_access *access,
					    u64 base_iova, size_t length)
{
	u64 aligned_iova = PAGE_ALIGN_DOWN(base_iova);	/* [한국어] 고정할 때와 같은 페이지 경계로 */
	u64 offset = base_iova - aligned_iova;	/* [한국어] 그만큼 들어간 곳이었다 */

	iommufd_access_unpin_pages(access, aligned_iova,	/* [한국어] 같은 범위로 풀어야 한다 */
				   PAGE_ALIGN(length + offset));	/* [한국어] 오프셋을 더한 뒤 올림한 길이 */
	iommufd_access_detach_internal(access);	/* [한국어] IOAS 에서 떼고 */
	iommufd_access_destroy_internal(ictx, access);	/* [한국어] 접근 객체를 버린다 */
}

/*
 * [한국어]
 * iommufd_hw_queue_destroy - 하드웨어 큐를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * 드라이버가 하드웨어에서 큐를 떼어 낸 뒤에야 고정을 푼다 — 순서가
 * 바뀌면 하드웨어가 이미 놓인 메모리를 읽는다.
 */
void iommufd_hw_queue_destroy(struct iommufd_object *obj)
{
	struct iommufd_hw_queue *hw_queue =	/* [한국어] 공통 객체에서 */
		container_of(obj, struct iommufd_hw_queue, obj);	/* [한국어] 구체 타입으로 */

	if (hw_queue->destroy)	/* [한국어] 드라이버가 하드웨어에서 큐를 떼어야 하면 */
		hw_queue->destroy(hw_queue);	/* [한국어] 먼저 그렇게 한다 */
	if (hw_queue->access)	/* [한국어] 고정해 두었으면 */
		iommufd_hw_queue_destroy_access(hw_queue->viommu->ictx,	/* [한국어] 이제 풀어도 된다 */
						hw_queue->access,	/* [한국어] 그 접근 객체로 */
						hw_queue->base_addr,	/* [한국어] 같은 범위를 */
						hw_queue->length);	/* [한국어] 같은 길이만큼 */
	if (hw_queue->viommu)	/* [한국어] vIOMMU 를 잡았으면 */
		refcount_dec(&hw_queue->viommu->obj.users);	/* [한국어] 그 참조를 놓는다 */
}

/*
 * When the HW accesses the guest queue via physical addresses, the underlying
 * physical pages of the guest queue must be contiguous. Also, for the security
 * concern that IOMMUFD_CMD_IOAS_UNMAP could potentially remove the mappings of
 * the guest queue from the nesting parent iopt while the HW is still accessing
 * the guest queue memory physically, such a HW queue must require an access to
 * pin the underlying pages and prevent that from happening.
 */
static struct iommufd_access *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_hw_queue_alloc_phys - 게스트 큐의 물리 페이지를 고정하고 검사한다
 *
 * @cmd: 사용자가 준 인자.
 * @viommu: 그 큐가 속할 vIOMMU.
 * @base_pa: 찾은 물리 주소를 돌려준다.
 * @return: 고정에 쓴 접근 객체, 실패하면 ERR_PTR.
 *
 * 원 주석이 두 요구를 밝힌다.
 *  - 하드웨어가 물리 주소로 큐를 읽으므로 그 페이지들이 연속이어야 한다.
 *  - 사용자가 그사이 IOAS_UNMAP 으로 매핑을 걷어낼 수 있어, 커널이 따로
 *    고정해 하드웨어가 사라진 메모리를 읽지 않게 해야 한다.
 *
 * 커널 내부용 접근 객체를 만들어 그 고정을 붙잡아 둔다. 그 객체가 살아
 * 있는 동안 unmap 이 그 범위를 건드릴 수 없다.
 *
 * __GFP_NOWARN 을 쓰는 이유도 원 주석에 있다 — 퍼저가 큰 길이를 넣어
 * 커널 로그를 채우는 것을 막는다.
 */
iommufd_hw_queue_alloc_phys(struct iommu_hw_queue_alloc *cmd,
			    struct iommufd_viommu *viommu, phys_addr_t *base_pa)
{
	u64 aligned_iova = PAGE_ALIGN_DOWN(cmd->nesting_parent_iova);	/* [한국어] 고정은 페이지 단위라 내려 잡고 */
	u64 offset = cmd->nesting_parent_iova - aligned_iova;	/* [한국어] 들어간 만큼을 기억한다 */
	struct iommufd_access *access;	/* [한국어] 고정을 붙잡을 접근 객체 */
	struct page **pages;	/* [한국어] 고정된 페이지들 */
	size_t max_npages;	/* [한국어] 그 개수 */
	size_t length;	/* [한국어] 올림한 길이 */
	size_t i;	/* [한국어] 연속 검사용 */
	int rc;	/* [한국어] 결과 */

	/* max_npages = DIV_ROUND_UP(offset + cmd->length, PAGE_SIZE) */
	if (check_add_overflow(offset, cmd->length, &length))	/* [한국어] (원 주석: max_npages = DIV_ROUND_UP(offset + length, PAGE_SIZE)) */
		return ERR_PTR(-ERANGE);	/* [한국어] 넘치면 거절 */
	if (check_add_overflow(length, PAGE_SIZE - 1, &length))	/* [한국어] 올림에서도 */
		return ERR_PTR(-ERANGE);	/* [한국어] 넘치면 거절 */
	max_npages = length / PAGE_SIZE;	/* [한국어] 필요한 페이지 수 */
	/* length needs to be page aligned too */
	length = max_npages * PAGE_SIZE;	/* [한국어] (원 주석: 길이도 페이지 정렬이어야 한다) */

	/*
	 * Use kvcalloc() to avoid memory fragmentation for a large page array.
	 * Set __GFP_NOWARN to avoid syzkaller blowups
	 */
	pages = kvzalloc_objs(*pages, max_npages, GFP_KERNEL | __GFP_NOWARN);	/* [한국어] (원 주석: 큰 배열의 단편화를 피하려 kvcalloc 을, syzkaller 소동을 막으려 NOWARN 을 쓴다) */
	if (!pages)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 거절 */

	access = iommufd_access_create_internal(viommu->ictx);	/* [한국어] 커널 내부용 접근을 만들고 */
	if (IS_ERR(access)) {	/* [한국어] 실패면 */
		rc = PTR_ERR(access);	/* [한국어] 오류를 전하되 */
		goto out_free;	/* [한국어] 배열을 버려야 한다 */
	}

	rc = iommufd_access_attach_internal(access, viommu->hwpt->ioas);	/* [한국어] 부모의 IOAS 에 붙인다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_destroy;	/* [한국어] 접근 객체를 버린다 */

	rc = iommufd_access_pin_pages(access, aligned_iova, length, pages, 0);	/* [한국어] 고정하면 사용자가 unmap 해도 사라지지 않는다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_detach;	/* [한국어] 붙임을 되돌린다 */

	/* Validate if the underlying physical pages are contiguous */
	for (i = 1; i < max_npages; i++) {	/* [한국어] (원 주석: 물리 페이지들이 연속인지 확인한다) */
		if (page_to_pfn(pages[i]) == page_to_pfn(pages[i - 1]) + 1)	/* [한국어] 바로 다음 페이지면 */
			continue;	/* [한국어] 이어진 것이다 */
		rc = -EFAULT;	/* [한국어] 하나라도 끊기면 하드웨어가 물리 주소로 읽을 수 없다 */
		goto out_unpin;	/* [한국어] 되돌린다 */
	}

	*base_pa = (page_to_pfn(pages[0]) << PAGE_SHIFT) + offset;	/* [한국어] 첫 페이지의 주소에 오프셋을 더한 것이 큐의 실제 위치다 */
	kvfree(pages);	/* [한국어] 배열은 더 필요 없다 — 고정은 접근 객체가 붙잡고 있다 */
	return access;	/* [한국어] 그 접근 객체를 돌려준다 */

out_unpin:	/* [한국어] 고정을 푼다 */
	iommufd_access_unpin_pages(access, aligned_iova, length);	/* [한국어] 고정을 풀고 */
out_detach:	/* [한국어] IOAS 에서 뗀다 */
	iommufd_access_detach_internal(access);	/* [한국어] IOAS 에서 떼고 */
out_destroy:	/* [한국어] 접근 객체를 버린다 */
	iommufd_access_destroy_internal(viommu->ictx, access);	/* [한국어] 접근 객체를 버리고 */
out_free:	/* [한국어] 페이지 배열을 버린다 */
	kvfree(pages);	/* [한국어] 배열도 버린다 */
	return ERR_PTR(rc);	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iommufd_hw_queue_alloc_ioctl - IOMMU_HW_QUEUE_ALLOC 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 게스트가 하드웨어 큐를 직접 쓰게 해 주는 명령이다. 게스트가 명령을
 * 큐에 넣으면 하드웨어가 바로 읽어 가므로, VM 진입·퇴출 없이 무효화가
 * 이루어진다.
 *
 * vIOMMU 와 마찬가지로 크기를 드라이버가 정한다.
 *
 * 물리 주소를 얻는 일이 먼저다 — 그것이 실패하면 큐를 세울 수 없다.
 */
int iommufd_hw_queue_alloc_ioctl(struct iommufd_ucmd *ucmd)
{
	struct iommu_hw_queue_alloc *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_hw_queue *hw_queue;	/* [한국어] 만들 객체 */
	struct iommufd_viommu *viommu;	/* [한국어] 속할 vIOMMU */
	struct iommufd_access *access;	/* [한국어] 큐 메모리 고정 */
	size_t hw_queue_size;	/* [한국어] 드라이버가 정하는 크기 */
	phys_addr_t base_pa;	/* [한국어] 큐의 물리 주소 */
	u64 last;	/* [한국어] 범위의 끝 */
	int rc;	/* [한국어] 결과 */

	if (cmd->flags || cmd->type == IOMMU_HW_QUEUE_TYPE_DEFAULT)	/* [한국어] 플래그가 있거나 종류를 정하지 않았으면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */
	if (!cmd->length)	/* [한국어] 길이가 0 이면 */
		return -EINVAL;	/* [한국어] 큐가 될 수 없다 */
	if (check_add_overflow(cmd->nesting_parent_iova, cmd->length - 1,	/* [한국어] 끝이 */
			       &last))	/* [한국어] 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	viommu = iommufd_get_viommu(ucmd, cmd->viommu_id);	/* [한국어] vIOMMU 를 붙잡고 */
	if (IS_ERR(viommu))	/* [한국어] 없으면 */
		return PTR_ERR(viommu);	/* [한국어] 그대로 */

	if (!viommu->ops || !viommu->ops->get_hw_queue_size ||	/* [한국어] 드라이버가 */
	    !viommu->ops->hw_queue_init_phys) {	/* [한국어] 하드웨어 큐를 지원하지 않으면 */
		rc = -EOPNOTSUPP;	/* [한국어] 거절하되 */
		goto out_put_viommu;	/* [한국어] vIOMMU 를 놓아야 한다 */
	}

	hw_queue_size = viommu->ops->get_hw_queue_size(viommu, cmd->type);	/* [한국어] 드라이버가 자기 상태까지 담을 크기 */
	if (!hw_queue_size) {	/* [한국어] 그 종류를 지원하지 않으면 0 이다 */
		rc = -EOPNOTSUPP;	/* [한국어] 거절하되 */
		goto out_put_viommu;	/* [한국어] vIOMMU 를 놓아야 한다 */
	}

	/*
	 * It is a driver bug for providing a hw_queue_size smaller than the
	 * core HW queue structure size
	 */
	if (WARN_ON_ONCE(hw_queue_size < sizeof(*hw_queue))) {	/* [한국어] (원 주석: core 구조체보다 작은 크기를 말하는 것은 드라이버 버그다) */
		rc = -EOPNOTSUPP;	/* [한국어] 그대로 쓰면 메모리를 넘어 쓴다 */
		goto out_put_viommu;	/* [한국어] 거절 */
	}

	hw_queue = (struct iommufd_hw_queue *)_iommufd_object_alloc_ucmd(	/* [한국어] ioctl 이 끝날 때 코어가 확정하거나 되돌린다 */
		ucmd, hw_queue_size, IOMMUFD_OBJ_HW_QUEUE);	/* [한국어] 드라이버가 말한 크기로 */
	if (IS_ERR(hw_queue)) {	/* [한국어] 실패면 */
		rc = PTR_ERR(hw_queue);	/* [한국어] 오류를 전하되 */
		goto out_put_viommu;	/* [한국어] vIOMMU 를 놓아야 한다 */
	}

	access = iommufd_hw_queue_alloc_phys(cmd, viommu, &base_pa);	/* [한국어] 페이지를 고정하고 연속인지 확인한다 */
	if (IS_ERR(access)) {	/* [한국어] 실패면 */
		rc = PTR_ERR(access);	/* [한국어] 큐를 세울 수 없다 */
		goto out_put_viommu;	/* [한국어] 코어가 되돌린다 */
	}

	hw_queue->viommu = viommu;	/* [한국어] 속한 vIOMMU */
	refcount_inc(&viommu->obj.users);	/* [한국어] 그것이 먼저 사라지면 안 된다 */
	hw_queue->access = access;	/* [한국어] 파괴할 때 이 고정을 푼다 */
	hw_queue->type = cmd->type;	/* [한국어] 드라이버가 아는 종류 */
	hw_queue->length = cmd->length;	/* [한국어] 큐의 길이 */
	hw_queue->base_addr = cmd->nesting_parent_iova;	/* [한국어] 고정을 풀 때 쓸 IOVA */

	rc = viommu->ops->hw_queue_init_phys(hw_queue, cmd->index, base_pa);	/* [한국어] 드라이버가 하드웨어에 그 주소를 알린다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_put_viommu;	/* [한국어] 코어가 되돌린다 */

	cmd->out_hw_queue_id = hw_queue->obj.id;	/* [한국어] 사용자가 이후 이 id 로 부른다 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 돌려준다 */

out_put_viommu:	/* [한국어] vIOMMU 를 놓는다 */
	iommufd_put_object(ucmd->ictx, &viommu->obj);	/* [한국어] 조회 참조를 놓는다 */
	return rc;	/* [한국어] 성패 */
}
