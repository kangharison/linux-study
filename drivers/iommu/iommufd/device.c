// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] 장치를 iommufd 에 묶고 페이지 테이블에 붙이는 곳 (device.c)
 *
 * === 파일의 역할 ===
 * "이 장치가 어느 주소 공간으로 DMA 하는가"를 정하는 파일이다. 장치를
 * iommufd 문맥에 묶는 bind, 그 장치를 페이지 테이블에 붙이는 attach,
 * 붙은 것을 다른 것으로 바꾸는 replace, 떼는 detach 가 모두 여기 있다.
 *
 * 이 파일을 어렵게 만드는 것은 IOMMU 그룹이다. 하드웨어가 여러 장치를
 * 하나로 묶어 놓아 서로를 구분하지 못하는 경우가 있는데, 그런 장치들은
 * 반드시 같은 주소 공간을 써야 한다. 그래서 붙이기·떼기는 장치 단위가
 * 아니라 그룹 단위로 일어나고, 그룹 안의 첫 장치와 마지막 장치만 실제
 * 하드웨어를 건드린다. struct iommufd_group 이 그 상태를 들고 있다.
 *
 * 예약 IOVA 도 그룹 단위로 관리한다. 장치마다 쓰면 안 되는 주소 구간이
 * 있고(하드웨어가 그 주소를 다른 뜻으로 해석한다), 그것을 IOAS 에 등록해
 * 그 구간에 매핑이 생기지 않게 막는다. 그룹 안의 여러 장치가 같은 구간을
 * 예약하므로 그룹 하나가 대표로 든다.
 *
 * MSI 창의 처리도 여기서 갈린다. ARM 처럼 인터럽트 doorbell 이 IOVA 공간
 * 안에 있어야 하는 플랫폼에서는, 커널이 그 주소를 정해 도메인에 심어야
 * 한다. sw_msi_* 가 그 일을 한다.
 *
 * 파일 뒷부분은 성격이 다른 물건이다 — 접근자(access). CPU 로 사용자
 * 메모리를 읽고 쓰는 경로로, vfio 의 에뮬레이션 장치가 쓴다. DMA 가 아니라
 * 커널이 직접 IOVA 를 따라 페이지를 찾아온다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * vfio 드라이버 → iommufd_device_bind → iommufd_device_attach
 *   → iommufd_device_auto_get_domain (도메인이 없으면 만든다)
 *   → iommufd_hw_pagetable_attach → iommu_attach_group
 *
 * 사용자 → IOMMU_DEVICE_GET_HW_INFO → iommufd_get_hw_info
 *
 * 에뮬레이션 장치 → iommufd_access_pin_pages / iommufd_access_rw
 *   → io_pagetable.c 의 영역 반복자 → 사용자 페이지
 *
 * 실행 컨텍스트: 모두 프로세스 문맥. 접근자의 unmap 알림만 매핑을 푸는
 * 쪽의 문맥에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위: drivers/vfio 가 IOMMUFD 네임스페이스로 이 파일의 함수를 부른다.
 *   main.c 의 ioctl 표가 IOMMU_DEVICE_GET_HW_INFO 를 여기로 보낸다.
 * 아래: iommu 코어의 attach/detach API, io_pagetable.c 의 예약 IOVA 와
 *   페이지 조회, hw_pagetable.c 의 도메인 할당.
 *
 * === 주요 함수/구조체 요약 ===
 * struct iommufd_group: 한 IOMMU 그룹에 대한 iommufd 쪽 상태. 그룹 안의
 *   장치 목록과 PASID 별 붙임 상태, 예약 MSI 주소를 든다.
 * iommufd_get_group: 그룹 상태를 얻거나 만든다. 락을 놓았다 다시 잡는
 *   경합을 cmpxchg 고리로 푼다.
 * iommufd_device_bind: 장치의 DMA 소유권을 가져와 문맥에 묶는다.
 * iommufd_hw_pagetable_attach / detach: 그룹의 첫/마지막 장치일 때만
 *   실제 하드웨어를 건드린다.
 * iommufd_device_do_replace: 무중단 교체. 옛 것을 떼지 않고 바꿔치기한다.
 * iommufd_device_auto_get_domain: IOAS 만 지정했을 때 쓸 만한 도메인을
 *   찾거나 새로 만든다.
 * iommufd_access_pin_pages / rw: CPU 로 IOVA 를 따라가는 경로.
 */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 도메인 붙이기·떼기 API 와 장치 능력 조회. */
#include <linux/iommufd.h>	/* [한국어] vfio 등 외부 모듈에 내보내는 iommufd API 선언. 이 파일이 그 구현이다. */
#include <linux/pci-ats.h>	/* [한국어] PCI 의 PRI·PASID 상태를 읽는 함수들. 폴트 큐 궁합 판정과 능력 보고에 쓴다. */
#include <linux/slab.h>	/* [한국어] kzalloc_obj / kfree. */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 명령 구조체와 능력 비트 정의. */

#include "../iommu-priv.h"	/* [한국어] iommu 코어의 비공개 헤더. attach 핸들처럼 드라이버에는 감춰진 것을 쓴다. */
#include "io_pagetable.h"	/* [한국어] 영역 반복자와 예약 IOVA 등록 함수. 접근자 경로가 이것으로 페이지를 찾아간다. */
#include "iommufd_private.h"	/* [한국어] 객체 모형과 이 모듈 안에서만 쓰는 선언들. */

/*
 * [한국어] MSI 창을 격리하지 못하는 플랫폼에서도 장치를 묶게 허용하는
 * 모듈 파라미터.
 *
 * 격리되지 않으면 장치가 평범한 DMA 쓰기 하나로 이 문맥 밖의 인터럽트를
 * 일으킬 수 있다 — 게스트가 호스트를 건드리는 길이 열린다.
 *
 * 그래도 남겨 둔 이유는 옛 VFIO 가 같은 파라미터를 가졌기 때문이다.
 * 아래 MODULE_PARM_DESC 가 "보안 약점"이라고 못 박는다.
 */
static bool allow_unsafe_interrupts;
module_param(allow_unsafe_interrupts, bool, S_IRUGO | S_IWUSR);	/* [한국어] 읽기는 누구나, 쓰기는 root 만 할 수 있게 /sys/module 아래에 노출한다. 부팅 뒤에도 바꿀 수 있다. */
MODULE_PARM_DESC(	/* [한국어] modinfo 에 보이는 설명. 이것이 보안 약점임을 못 박아 둔다. */
	allow_unsafe_interrupts,
	"Allow IOMMUFD to bind to devices even if the platform cannot isolate "
	"the MSI interrupt window. Enabling this is a security weakness.");

/*
 * [한국어] 한 그룹의 한 PASID 가 어디에 붙어 있는지를 담는 구조체.
 *
 * 그룹의 pasid_attach xarray 에 PASID 를 열쇠로 들어간다. 그룹 안의 모든
 * 장치가 같은 도메인을 공유하지만, 어느 장치들이 실제로 붙어 있는지는
 * 세어야 하므로 목록을 함께 둔다.
 */
struct iommufd_attach {
	/* [한국어] 이 그룹의 이 PASID 가 붙어 있는 페이지 테이블.
	 *  설정자: 그룹의 첫 장치를 붙일 때, 그리고 교체할 때.
	 *  읽는 자: 두 번째 이후 장치가 같은 것에 붙는지 확인할 때, 그리고
	 *  뗄 때 어디서 떼야 하는지 알아낼 때.
	 *  값 범위: 유효한 포인터. NULL 은 자리만 잡아 둔 중간 상태다.
	 *  동기화: 그룹 뮤텍스(igroup->lock)가 지킨다. */
	struct iommufd_hw_pagetable *hwpt;
	/* [한국어] 이 도메인에 붙어 있는 장치들. 열쇠는 장치의 객체 id.
	 *  설정자: 붙일 때 넣고 뗄 때 지운다.
	 *  읽는 자: 장치 수를 세어 첫/마지막 장치를 판정하는 곳,
	 *  그리고 교체할 때 그룹 전체의 예약 IOVA 를 옮기는 곳.
	 *  비게 되면 그룹이 그 도메인에서 완전히 떨어졌다는 뜻이라,
	 *  그때 실제 하드웨어에서 뗀다.
	 *  동기화: 그룹 뮤텍스가 지킨다. */
	struct xarray device_array;
};

/*
 * [한국어]
 * iommufd_group_release - 그룹 상태의 마지막 참조가 사라졌을 때
 *
 * @kref: 그 참조 카운터.
 *
 * kref_put 이 0 이 되었을 때만 불린다.
 *
 * cmpxchg 로 지우는 것이 요점이다. 참조를 0 으로 만든 뒤 여기 오기까지
 * 사이에 다른 스레드가 같은 그룹 id 로 새 항목을 넣었을 수 있는데,
 * 그때 무턱대고 지우면 남의 것을 지우게 된다.
 */
static void iommufd_group_release(struct kref *kref)
{
	struct iommufd_group *igroup =	/* [한국어] 참조 카운터에서 그룹 상태를 되짚는다. 값은 다음 줄의 container_of 가 만든다. */
		container_of(kref, struct iommufd_group, ref);	/* [한국어] 참조 카운터에서 그룹 상태를 되짚는다. */

	WARN_ON(!xa_empty(&igroup->pasid_attach));	/* [한국어] 아직 어딘가에 붙어 있는데 참조가 0 이 됐다면 어딘가에서 참조를 잘못 놓은 것이다. */

	xa_cmpxchg(&igroup->ictx->groups, iommu_group_id(igroup->group), igroup,	/* [한국어] 내 것일 때만 지운다. 참조가 0 이 된 뒤 여기 오기까지 사이에 다른 스레드가 같은 id 로 새 항목을 넣었을 수 있다. */
		   NULL, GFP_KERNEL);
	iommu_group_put(igroup->group);	/* [한국어] 들고 있던 iommu 코어 그룹 참조를 놓는다. */
	mutex_destroy(&igroup->lock);	/* [한국어] 디버그 설정에서 뮤텍스 파괴를 기록한다. */
	kfree(igroup);	/* [한국어] 그룹 상태를 해제한다. */
}

/*
 * [한국어]
 * iommufd_put_group - 그룹 상태의 참조를 하나 놓는다
 *
 * @group: 놓을 그룹 상태.
 *
 * 마지막이면 release 가 불린다.
 */
static void iommufd_put_group(struct iommufd_group *group)
{
	kref_put(&group->ref, iommufd_group_release);	/* [한국어] 0 이 되면 release 가 불린다. */
}

/*
 * [한국어]
 * iommufd_group_try_get - 이미 있는 그룹 상태를 붙잡아 본다
 *
 * @igroup: xarray 에서 꺼낸 후보(NULL 일 수 있다).
 * @group: 우리가 찾는 진짜 iommu 그룹.
 * @return: 참조를 들었으면 true.
 *
 * 참조가 이미 0 이면 실패한다 — 그런 항목은 곧 지워질 운명이다.
 *
 * 원 주석이 그룹 id 재사용을 못 하는 근거를 밝힌다. 락 아래에서 포인터를
 * 얻을 수 있었다는 것은 그룹이 아직 반납되지 않았다는 뜻이므로, id 가
 * 다른 그룹에 다시 배정됐을 리 없다. 그래도 확인하는 것은 안전망이다.
 */
static bool iommufd_group_try_get(struct iommufd_group *igroup,
				  struct iommu_group *group)
{
	if (!igroup)	/* [한국어] xarray 가 비어 있었다. */
		return false;	/* [한국어] 붙잡을 것이 없다. */
	/*
	 * group ID's cannot be re-used until the group is put back which does
	 * not happen if we could get an igroup pointer under the xa_lock.
	 */
	if (WARN_ON(igroup->group != group))	/* [한국어] 위 영어 주석의 근거대로 일어날 수 없는 일이지만, 어긋나면 큰 문제라 확인한다. */
		return false;	/* [한국어] 다른 그룹의 항목이다. */
	return kref_get_unless_zero(&igroup->ref);	/* [한국어] 이미 0 이면 실패한다 — 그런 항목은 곧 지워질 운명이라 되살리면 안 된다. */
}

/*
 * iommufd needs to store some more data for each iommu_group, we keep a
 * parallel xarray indexed by iommu_group id to hold this instead of putting it
 * in the core structure. To keep things simple the iommufd_group memory is
 * unique within the iommufd_ctx. This makes it easy to check there are no
 * memory leaks.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_get_group - 이 장치가 속한 그룹의 iommufd 상태를 얻는다
 *
 * @ictx: 문맥.
 * @dev: 대상 장치.
 * @return: 참조를 든 그룹 상태, 실패하면 오류 포인터.
 *
 * 원 주석대로 iommu 코어의 그룹 구조체에 필드를 더하는 대신, 그룹 id 를
 * 열쇠로 하는 별도 xarray 를 둔다. 문맥마다 따로 두어 메모리 누수를
 * 확인하기 쉽게 만들었다.
 *
 * 흐름이 복잡한 이유: 할당은 잠들 수 있어 xa_lock 안에서 못 한다. 그래서
 * 락을 놓고 만든 뒤 다시 잡아 넣는데, 그 사이 다른 스레드가 먼저 넣었을
 * 수 있다. 그 경합을 cmpxchg 고리로 푼다.
 *
 * 고리가 필요한 이유는 세 갈래 결과가 있기 때문이다 — 내 것이 들어갔거나,
 * 남의 쓸 만한 것이 있거나, 남의 것이 있지만 죽어 가는 중이거나. 마지막
 * 경우에는 그것을 예상값으로 삼아 다시 시도한다.
 */
static struct iommufd_group *iommufd_get_group(struct iommufd_ctx *ictx,
					       struct device *dev)
{
	struct iommufd_group *new_igroup;	/* [한국어] 미리 만들어 두는 후보. */
	struct iommufd_group *cur_igroup;	/* [한국어] cmpxchg 의 예상값. 처음에는 "비어 있음"으로 시작한다. */
	struct iommufd_group *igroup;	/* [한국어] xarray 에서 실제로 꺼낸 값. */
	struct iommu_group *group;	/* [한국어] iommu 코어 쪽 그룹. */
	unsigned int id;	/* [한국어] 그 그룹의 번호. xarray 의 열쇠다. */

	group = iommu_group_get(dev);	/* [한국어] 장치가 속한 그룹을 참조와 함께 얻는다. */
	if (!group)	/* [한국어] IOMMU 아래에 있지 않은 장치다. */
		return ERR_PTR(-ENODEV);	/* [한국어] 다룰 수 없다. */

	id = iommu_group_id(group);	/* [한국어] 그룹 번호를 열쇠로 쓴다. */

	xa_lock(&ictx->groups);	/* [한국어] 먼저 이미 있는지 빠르게 본다. */
	igroup = xa_load(&ictx->groups, id);	/* [한국어] 있으면 그것을 쓴다. */
	if (iommufd_group_try_get(igroup, group)) {	/* [한국어] 살아 있는 것을 붙잡았다면 */
		xa_unlock(&ictx->groups);	/* [한국어] 락을 놓고 */
		iommu_group_put(group);	/* [한국어] 새로 든 코어 참조는 필요 없다 — 기존 항목이 이미 들고 있다. */
		return igroup;	/* [한국어] 기존 것을 돌려준다. */
	}
	xa_unlock(&ictx->groups);	/* [한국어] 없거나 죽어 가는 것이었다. 락을 놓고 만들러 간다. */

	new_igroup = kzalloc_obj(*new_igroup);	/* [한국어] 할당은 잠들 수 있어 락 밖에서 한다. */
	if (!new_igroup) {	/* [한국어] 메모리가 없다. */
		iommu_group_put(group);	/* [한국어] 든 참조를 놓고 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패를 알린다. */
	}

	kref_init(&new_igroup->ref);	/* [한국어] 참조를 1 로 시작한다. */
	mutex_init(&new_igroup->lock);	/* [한국어] 붙임 상태를 지킬 뮤텍스. */
	xa_init(&new_igroup->pasid_attach);	/* [한국어] PASID 별 붙임 상태를 담을 xarray. */
	new_igroup->sw_msi_start = PHYS_ADDR_MAX;	/* [한국어] "아직 MSI 창을 모른다"는 표시. 예약 구간을 등록하며 채워진다. */
	/* group reference moves into new_igroup */
	new_igroup->group = group;	/* [한국어] 위 영어 주석대로 코어 그룹 참조의 주인이 여기로 넘어온다. */

	/*
	 * The ictx is not additionally refcounted here becase all objects using
	 * an igroup must put it before their destroy completes.
	 */
	new_igroup->ictx = ictx;	/* [한국어] 속한 문맥을 기억한다. 원 주석대로 참조는 들지 않는데, 이것을 쓰는 객체들이 모두 먼저 사라지기 때문이다. */

	/*
	 * We dropped the lock so igroup is invalid. NULL is a safe and likely
	 * value to assume for the xa_cmpxchg algorithm.
	 */
	cur_igroup = NULL;	/* [한국어] 원 주석대로 락을 놓았던 사이의 값은 믿을 수 없다. "비어 있음"이 가장 그럴듯한 예상값이다. */
	xa_lock(&ictx->groups);	/* [한국어] 여기서부터 경합을 해결한다. */
	while (true) {	/* [한국어] 세 갈래 결과 중 하나로 끝날 때까지 돈다. */
		igroup = __xa_cmpxchg(&ictx->groups, id, cur_igroup, new_igroup,	/* [한국어] 예상값과 같을 때만 바꾼다. 실제 값을 돌려주므로 다음 판단에 쓴다. */
				      GFP_KERNEL);
		if (xa_is_err(igroup)) {	/* [한국어] 메모리 부족 등 xarray 자체의 오류. */
			xa_unlock(&ictx->groups);	/* [한국어] 락을 놓고 */
			iommufd_put_group(new_igroup);	/* [한국어] 만들어 둔 것을 버린다. */
			return ERR_PTR(xa_err(igroup));	/* [한국어] 오류를 올린다. */
		}

		/* new_group was successfully installed */
		if (cur_igroup == igroup) {	/* [한국어] 예상과 같았다 = 내 것이 들어갔다. */
			xa_unlock(&ictx->groups);	/* [한국어] 락을 놓고 */
			return new_igroup;	/* [한국어] 내 것을 돌려준다. */
		}

		/* Check again if the current group is any good */
		if (iommufd_group_try_get(igroup, group)) {	/* [한국어] 남의 것이 있고 그것이 살아 있다면 */
			xa_unlock(&ictx->groups);	/* [한국어] 락을 놓고 */
			iommufd_put_group(new_igroup);	/* [한국어] 내 것을 버리고 */
			return igroup;	/* [한국어] 남의 것을 쓴다. */
		}
		cur_igroup = igroup;	/* [한국어] 남의 것이 죽어 가는 중이다. 그것을 예상값으로 삼아 다시 시도한다. */
	}
}

/*
 * [한국어]
 * iommufd_device_remove_vdev - 이 장치에 딸린 가상 장치를 없앤다
 *
 * @idev: 사라지는 장치.
 *
 * 장치가 파괴될 때, 그것을 가리키던 vdev 객체를 먼저 정리해야 한다.
 * 사용자가 만든 객체를 커널이 대신 없애는 드문 경우다.
 *
 * 그래서 그냥 없애지 않고 "묘비(tombstone)"를 남긴다 — id 는 남겨 두되
 * 내용은 비운다. 사용자가 나중에 그 id 를 지우려 할 때 없는 객체라고
 * 하면 혼란스럽고, 그 사이 다른 객체가 같은 id 를 받으면 더 나쁘다.
 *
 * destroying 표시를 먼저 세우는 이유: 이후로 새 vdev 가 이 장치를
 * 가리키지 못하게 막는다.
 *
 * 원 주석이 밝히는 까다로운 경우 — 사용자가 이미 vdev 파괴를 시작해
 * 객체 목록에서는 빠졌지만 아직 끝나지 않은 상태. 그쪽도 같은 뮤텍스를
 * 기다리므로, 여기서는 락을 놓고 나가면 그쪽이 마저 끝낸다.
 */
static void iommufd_device_remove_vdev(struct iommufd_device *idev)
{
	struct iommufd_vdevice *vdev;	/* [한국어] 이 장치를 가리키던 가상 장치. */

	mutex_lock(&idev->igroup->lock);	/* [한국어] vdev 포인터를 지키는 락. 그룹 뮤텍스를 함께 쓴다. */
	/* prevent new references from vdev */
	idev->destroying = true;	/* [한국어] 이후로 새 vdev 가 이 장치를 가리키지 못하게 막는다. */
	/* vdev has been completely destroyed by userspace */
	if (!idev->vdev)	/* [한국어] 이미 사용자가 완전히 없앴다. */
		goto out_unlock;	/* [한국어] 할 일이 없다. */

	vdev = iommufd_get_vdevice(idev->ictx, idev->vdev->obj.id);	/* [한국어] 객체 목록에서 다시 찾아 본다 — 아직 살아 있는지 확인하는 방법이다. */
	/*
	 * An ongoing vdev destroy ioctl has removed the vdev from the object
	 * xarray, but has not finished iommufd_vdevice_destroy() yet as it
	 * needs the same mutex. We exit the locking then wait on wait_cnt
	 * reference for the vdev destruction.
	 */
	if (IS_ERR(vdev))	/* [한국어] 원 주석의 경우: 사용자의 파괴가 이미 목록에서 뺐지만 아직 끝나지 않았다. */
		goto out_unlock;	/* [한국어] 그쪽이 같은 뮤텍스를 기다리므로, 놓고 나가면 마저 끝낸다. */

	/* Should never happen */
	if (WARN_ON(vdev != idev->vdev)) {	/* [한국어] 같은 id 로 다른 객체가 나왔다면 어딘가 크게 어긋난 것이다. */
		iommufd_put_object(idev->ictx, &vdev->obj);	/* [한국어] 찾으며 든 참조를 놓고 */
		goto out_unlock;	/* [한국어] 더 손대지 않는다. */
	}

	/*
	 * vdev is still alive. Hold a users refcount to prevent racing with
	 * userspace destruction, then use iommufd_object_tombstone_user() to
	 * destroy it and leave a tombstone.
	 */
	refcount_inc(&vdev->obj.users);	/* [한국어] 사용자가 지금 지우려 하는 것과 겹치지 않게 붙잡아 둔다. */
	iommufd_put_object(idev->ictx, &vdev->obj);	/* [한국어] 조회용 참조는 놓는다. 위에서 든 users 참조가 남는다. */
	mutex_unlock(&idev->igroup->lock);	/* [한국어] 파괴는 락 밖에서 한다 — 그 안에서 같은 락을 잡는다. */
	iommufd_object_tombstone_user(idev->ictx, &vdev->obj);	/* [한국어] id 는 남기고 내용만 비운다. 사용자가 나중에 그 id 를 지우려 할 때 혼란이 없고, 다른 객체가 그 id 를 받지도 않는다. */
	return;	/* [한국어] 정리를 마쳤다. */

out_unlock:	/* [한국어] 아무것도 하지 않고 나가는 경로들이 합류한다. */
	mutex_unlock(&idev->igroup->lock);	/* [한국어] 락을 놓는다. */
}

/*
 * [한국어]
 * iommufd_device_pre_destroy - 장치 파괴 직전에 불리는 갈고리
 *
 * @obj: 파괴될 장치 객체.
 *
 * 참조가 아직 남아 있을 때 불린다. 이 장치를 붙잡고 있던 vdev 의
 * 참조를 여기서 풀어 주어야 실제 파괴가 진행될 수 있다.
 *
 * destroy 와 나뉜 이유가 그것이다 — destroy 는 참조가 0 이 된 뒤에
 * 불리므로, 참조를 풀어 주는 일은 그 전에 해야 한다.
 */
void iommufd_device_pre_destroy(struct iommufd_object *obj)
{
	struct iommufd_device *idev =	/* [한국어] 객체에서 장치를 되짚는다. */
		container_of(obj, struct iommufd_device, obj);	/* [한국어] 객체에서 장치를 되짚는다. */

	/* Release the wait_cnt reference on this */
	iommufd_device_remove_vdev(idev);	/* [한국어] vdev 가 들고 있던 wait_cnt 참조를 여기서 풀어야 파괴가 진행된다. */
}

/*
 * [한국어]
 * iommufd_device_destroy - 장치 객체를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * DMA 소유권을 돌려주면 그 장치는 다시 평범한 커널 DMA API 아래로
 * 돌아간다.
 *
 * 모의 장치일 때 문맥 참조를 놓지 않는 이유: 애초에 들지 않았다.
 * 셀프테스트의 모의 장치는 문맥 안에 살아 있어, 참조를 들면 문맥이
 * 영원히 닫히지 않는 고리가 된다.
 */
void iommufd_device_destroy(struct iommufd_object *obj)
{
	struct iommufd_device *idev =	/* [한국어] 객체에서 장치를 되짚는다. */
		container_of(obj, struct iommufd_device, obj);	/* [한국어] 객체에서 장치를 되짚는다. */

	iommu_device_release_dma_owner(idev->dev);	/* [한국어] DMA 소유권을 돌려준다. 이후 그 장치는 다시 커널 DMA API 아래로 간다. */
	iommufd_put_group(idev->igroup);	/* [한국어] 그룹 상태 참조를 놓는다. 마지막이면 그룹 상태도 사라진다. */
	if (!iommufd_selftest_is_mock_dev(idev->dev))	/* [한국어] 모의 장치는 애초에 문맥 참조를 들지 않았다. */
		iommufd_ctx_put(idev->ictx);	/* [한국어] 문맥 참조를 놓는다. */
}

/**
 * iommufd_device_bind - Bind a physical device to an iommu fd
 * @ictx: iommufd file descriptor
 * @dev: Pointer to a physical device struct
 * @id: Output ID number to return to userspace for this device
 *
 * A successful bind establishes an ownership over the device and returns
 * struct iommufd_device pointer, otherwise returns error pointer.
 *
 * A driver using this API must set driver_managed_dma and must not touch
 * the device until this routine succeeds and establishes ownership.
 *
 * Binding a PCI device places the entire RID under iommufd control.
 *
 * The caller must undo this with iommufd_device_unbind()
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_device_bind - 장치를 이 iommufd 문맥에 묶는다
 *
 * @ictx: 문맥.
 * @dev: 묶을 물리 장치.
 * @id: 사용자에게 알릴 객체 id 를 여기에 쓴다.
 * @return: 장치 객체, 실패하면 오류 포인터.
 *
 * 이 함수가 성공하면 그 장치의 DMA 는 커널의 DMA API 가 아니라 iommufd 가
 * 다룬다. 원 주석대로 호출한 드라이버는 그전까지 장치를 건드리면 안 된다.
 *
 * 두 가지 안전 검사가 있다. 캐시 일관성을 못 하는 장치는 아예 거절한다 —
 * 원 주석대로 사용자에게 일관성을 되돌릴 방법을 주지 않기 때문이다.
 * MSI 격리가 안 되는 플랫폼은 모듈 파라미터로만 통과시킨다.
 *
 * PCI 장치를 묶으면 원 주석대로 그 RID 전체가 iommufd 아래로 들어온다 —
 * 함수 단위가 아니라 장치 단위라는 뜻이다.
 */
struct iommufd_device *iommufd_device_bind(struct iommufd_ctx *ictx,
					   struct device *dev, u32 *id)
{
	struct iommufd_device *idev;	/* [한국어] 만들 장치 객체. */
	struct iommufd_group *igroup;	/* [한국어] 그 장치가 속한 그룹 상태. */
	int rc;	/* [한국어] 오류 코드. */

	/*
	 * iommufd always sets IOMMU_CACHE because we offer no way for userspace
	 * to restore cache coherency.
	 */
	if (!device_iommu_capable(dev, IOMMU_CAP_CACHE_COHERENCY))	/* [한국어] 원 주석대로 iommufd 는 늘 IOMMU_CACHE 를 세우고, 사용자에게 그것을 되돌릴 길을 주지 않는다. 그러니 못 하는 장치는 아예 받지 않는다. */
		return ERR_PTR(-EINVAL);	/* [한국어] 다룰 수 없는 장치. */

	igroup = iommufd_get_group(ictx, dev);	/* [한국어] 그룹 상태를 얻거나 만든다. */
	if (IS_ERR(igroup))	/* [한국어] 실패하면 */
		return ERR_CAST(igroup);	/* [한국어] 그 오류를 그대로 올린다. */

	/*
	 * For historical compat with VFIO the insecure interrupt path is
	 * allowed if the module parameter is set. Secure/Isolated means that a
	 * MemWr operation from the device (eg a simple DMA) cannot trigger an
	 * interrupt outside this iommufd context.
	 */
	if (!iommufd_selftest_is_mock_dev(dev) &&	/* [한국어] 모의 장치는 이 검사를 건너뛴다 — 진짜 인터럽트가 없다. */
	    !iommu_group_has_isolated_msi(igroup->group)) {	/* [한국어] 원 주석대로 격리되지 않으면 평범한 DMA 쓰기 하나로 이 문맥 밖의 인터럽트를 일으킬 수 있다. */
		if (!allow_unsafe_interrupts) {	/* [한국어] 모듈 파라미터로 허용하지 않았으면 */
			rc = -EPERM;	/* [한국어] 거절한다. */
			goto out_group_put;	/* [한국어] 그룹 참조를 놓고 나간다. */
		}

		dev_warn(	/* [한국어] 허용했더라도 무엇을 감수하는지 로그에 남긴다. */
			dev,
			"MSI interrupts are not secure, they cannot be isolated by the platform. "
			"Check that platform features like interrupt remapping are enabled. "
			"Use the \"allow_unsafe_interrupts\" module parameter to override\n");
	}

	rc = iommu_device_claim_dma_owner(dev, ictx);	/* [한국어] 이 장치의 DMA 소유권을 가져온다. 다른 곳이 이미 쥐고 있으면 실패한다. */
	if (rc)	/* [한국어] 실패하면 */
		goto out_group_put;	/* [한국어] 그룹 참조를 놓고 나간다. */

	idev = iommufd_object_alloc(ictx, idev, IOMMUFD_OBJ_DEVICE);	/* [한국어] 객체를 만든다. 아직 공개하지 않는다. */
	if (IS_ERR(idev)) {	/* [한국어] 메모리가 없다. */
		rc = PTR_ERR(idev);	/* [한국어] 오류를 꺼내 */
		goto out_release_owner;	/* [한국어] 소유권까지 되돌린다. */
	}
	idev->ictx = ictx;	/* [한국어] 속한 문맥. */
	if (!iommufd_selftest_is_mock_dev(dev))	/* [한국어] 모의 장치는 문맥 안에 살아 있어, 참조를 들면 문맥이 영원히 닫히지 않는 고리가 된다. */
		iommufd_ctx_get(ictx);	/* [한국어] 문맥이 이 장치보다 먼저 사라지지 않게 붙잡는다. */
	idev->dev = dev;	/* [한국어] 물리 장치를 기억한다. */
	idev->enforce_cache_coherency =	/* [한국어] 이 장치가 캐시 일관성을 강제할 수 있는지. 도메인을 고를 때 이 값이 쓰인다. */
		device_iommu_capable(dev, IOMMU_CAP_ENFORCE_CACHE_COHERENCY);
	/* The calling driver is a user until iommufd_device_unbind() */
	refcount_inc(&idev->obj.users);	/* [한국어] 원 주석대로 부른 드라이버가 unbind 할 때까지 이 참조를 든다. */
	/* igroup refcount moves into iommufd_device */
	idev->igroup = igroup;	/* [한국어] 원 주석대로 그룹 참조의 주인이 여기로 넘어온다 — 아래 실패 경로를 더는 타지 않는다. */

	/*
	 * If the caller fails after this success it must call
	 * iommufd_unbind_device() which is safe since we hold this refcount.
	 * This also means the device is a leaf in the graph and no other object
	 * can take a reference on it.
	 */
	iommufd_object_finalize(ictx, &idev->obj);	/* [한국어] 사용자에게 보이게 만든다. 원 주석대로 이후의 실패는 호출자가 unbind 로 되돌려야 한다. */
	*id = idev->obj.id;	/* [한국어] 사용자에게 알릴 id. */
	return idev;	/* [한국어] 성공. */

out_release_owner:	/* [한국어] 객체 할당 실패 경로. */
	iommu_device_release_dma_owner(dev);	/* [한국어] 소유권을 돌려준다. */
out_group_put:	/* [한국어] 그 앞 실패들이 합류한다. */
	iommufd_put_group(igroup);	/* [한국어] 그룹 참조를 놓는다. */
	return ERR_PTR(rc);	/* [한국어] 오류 포인터로 알린다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_bind, "IOMMUFD");	/* [한국어] vfio 등 IOMMUFD 네임스페이스를 들여온 모듈만 쓸 수 있게 내보낸다. */

/**
 * iommufd_ctx_has_group - True if any device within the group is bound
 *                         to the ictx
 * @ictx: iommufd file descriptor
 * @group: Pointer to a physical iommu_group struct
 *
 * True if any device within the group has been bound to this ictx, ex. via
 * iommufd_device_bind(), therefore implying ictx ownership of the group.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_ctx_has_group - 이 그룹의 장치가 이 문맥에 묶여 있는가
 *
 * @ictx: 문맥.
 * @group: 물어보는 iommu 그룹.
 * @return: 하나라도 묶여 있으면 true.
 *
 * vfio 가 그룹 단위 권한을 판정할 때 쓴다. 그룹의 한 장치라도 이 문맥이
 * 쥐고 있으면 그룹 전체를 이 문맥이 소유한 것으로 본다 — 그룹은 나눌 수
 * 없기 때문이다.
 *
 * 객체 전체를 훑는 것이 느려 보이지만, 이 질문은 장치를 열 때만 나온다.
 */
bool iommufd_ctx_has_group(struct iommufd_ctx *ictx, struct iommu_group *group)
{
	struct iommufd_object *obj;	/* [한국어] 훑어볼 객체. */
	unsigned long index;	/* [한국어] xarray 순회용 첨자. */

	if (!ictx || !group)	/* [한국어] 둘 중 하나라도 없으면 물음 자체가 성립하지 않는다. */
		return false;	/* [한국어] 아니라고 답한다. */

	xa_lock(&ictx->objects);	/* [한국어] 객체 목록을 훑는 동안 바뀌지 않게 한다. */
	xa_for_each(&ictx->objects, index, obj) {	/* [한국어] 이 문맥의 모든 객체. */
		if (obj->type == IOMMUFD_OBJ_DEVICE &&	/* [한국어] 장치 객체만 본다. */
		    container_of(obj, struct iommufd_device, obj)	/* [한국어] 장치로 되짚어 */
				    ->igroup->group == group) {
			xa_unlock(&ictx->objects);	/* [한국어] 락을 놓고 */
			return true;	/* [한국어] 하나라도 있으면 그룹 전체를 이 문맥이 소유한 것으로 본다. */
		}
	}
	xa_unlock(&ictx->objects);	/* [한국어] 다 훑었다. */
	return false;	/* [한국어] 이 그룹의 장치는 없다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_ctx_has_group, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/**
 * iommufd_device_unbind - Undo iommufd_device_bind()
 * @idev: Device returned by iommufd_device_bind()
 *
 * Release the device from iommufd control. The DMA ownership will return back
 * to unowned with DMA controlled by the DMA API. This invalidates the
 * iommufd_device pointer, other APIs that consume it must not be called
 * concurrently.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_device_unbind - bind 를 되돌린다
 *
 * @idev: 풀 장치.
 *
 * 원 주석대로 이 포인터는 여기서 무효가 되므로, 이것을 쓰는 다른 API 를
 * 동시에 부르면 안 된다.
 *
 * destroy_user 를 쓰는 이유: 사용자가 같은 객체를 지우려 할 수 있어,
 * 그쪽과 겹치지 않게 조율하는 판이 필요하다.
 */
void iommufd_device_unbind(struct iommufd_device *idev)
{
	iommufd_object_destroy_user(idev->ictx, &idev->obj);	/* [한국어] 사용자가 같은 객체를 지우려는 것과 겹치지 않게 조율하며 파괴한다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_unbind, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * [한국어]
 * iommufd_device_to_ictx - 장치가 묶인 문맥을 돌려준다
 *
 * @idev: 대상 장치.
 * @return: 그 문맥.
 *
 * vfio 가 문맥 단위 연산을 할 때 필요하다. 구조체 내부를 드러내지 않으려
 * 접근자 함수로 감쌌다.
 */
struct iommufd_ctx *iommufd_device_to_ictx(struct iommufd_device *idev)
{
	return idev->ictx;	/* [한국어] 문맥 포인터를 그대로 준다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_to_ictx, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * [한국어]
 * iommufd_device_to_id - 장치의 객체 id 를 돌려준다
 *
 * @idev: 대상 장치.
 * @return: 사용자가 아는 id.
 *
 * 같은 이유로 감싼 접근자다.
 */
u32 iommufd_device_to_id(struct iommufd_device *idev)
{
	return idev->obj.id;	/* [한국어] 객체 id 를 그대로 준다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_to_id, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * [한국어]
 * iommufd_group_device_num - 그 PASID 에 붙어 있는 장치 수를 센다
 *
 * @igroup: 대상 그룹.
 * @pasid: 세어 볼 PASID.
 * @return: 붙어 있는 장치 수.
 *
 * 그룹의 첫 장치인지 마지막 장치인지를 판정하는 데 쓴다. 그 두 경우에만
 * 실제 하드웨어를 건드리기 때문이다.
 *
 * 호출자가 그룹 뮤텍스를 쥐고 있어야 한다.
 */
static unsigned int iommufd_group_device_num(struct iommufd_group *igroup,
					     ioasid_t pasid)
{
	struct iommufd_attach *attach;	/* [한국어] 그 PASID 의 붙임 상태. */
	struct iommufd_device *idev;	/* [한국어] 순회용. */
	unsigned int count = 0;	/* [한국어] 세어 나갈 개수. */
	unsigned long index;	/* [한국어] xarray 순회용 첨자. */

	lockdep_assert_held(&igroup->lock);	/* [한국어] 호출자가 그룹 뮤텍스를 쥐고 있어야 한다. */

	attach = xa_load(&igroup->pasid_attach, pasid);	/* [한국어] 그 PASID 로 붙어 있는가. */
	if (attach)	/* [한국어] 붙어 있으면 */
		xa_for_each(&attach->device_array, index, idev)	/* [한국어] 장치 목록을 훑으며 */
			count++;	/* [한국어] 하나씩 센다. */
	return count;	/* [한국어] 0 이면 아직 아무도 붙지 않았다는 뜻. */
}

#ifdef CONFIG_IRQ_MSI_IOMMU	/* [한국어] MSI doorbell 을 IOMMU 안에 매핑해야 하는 플랫폼에서만 아래 구현을 쓴다. ARM 계열이 그렇고 x86 은 인터럽트 재매핑이 따로 있어 필요 없다. */
/*
 * [한국어]
 * iommufd_group_setup_msi - 이 그룹이 쓰던 MSI 창을 새 도메인에 심는다
 *
 * @igroup: 대상 그룹.
 * @hwpt_paging: 새로 붙는 페이징 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * ARM 같은 플랫폼에서는 인터럽트 doorbell 이 IOVA 공간 안에 있어야 한다.
 * 장치가 인터럽트를 쏘려면 그 주소가 도메인에 매핑돼 있어야 하므로,
 * 도메인을 바꿀 때마다 다시 심어 준다.
 *
 * 어느 주소인지는 커널이 정한다 — 사용자가 고르게 두면 게스트가 임의의
 * 인터럽트를 일으킬 수 있다.
 *
 * sw_msi_start 가 PHYS_ADDR_MAX 면 이 플랫폼은 그런 매핑이 필요 없다는
 * 뜻이라 그냥 성공한다.
 */
static int iommufd_group_setup_msi(struct iommufd_group *igroup,
				   struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_ctx *ictx = igroup->ictx;	/* [한국어] MSI 창 목록은 문맥이 들고 있다. */
	struct iommufd_sw_msi_map *cur;	/* [한국어] 훑어볼 MSI 창 하나. */

	if (igroup->sw_msi_start == PHYS_ADDR_MAX)	/* [한국어] 이 플랫폼(또는 이 그룹)은 소프트웨어 MSI 매핑이 필요 없다. */
		return 0;	/* [한국어] 할 일이 없다. */

	/*
	 * Install all the MSI pages the device has been using into the domain
	 */
	guard(mutex)(&ictx->sw_msi_lock);	/* [한국어] 목록을 훑는 동안 바뀌지 않게 한다. guard 라 함수를 나갈 때 저절로 풀린다. */
	list_for_each_entry(cur, &ictx->sw_msi_list, sw_msi_item) {	/* [한국어] 문맥이 아는 모든 MSI 창. */
		int rc;	/* [한국어] 설치 결과. */

		if (cur->sw_msi_start != igroup->sw_msi_start ||	/* [한국어] 이 그룹이 쓰는 창 시작 주소와 다르면 남의 것이다. */
		    !test_bit(cur->id, igroup->required_sw_msi.bitmap))	/* [한국어] 이 그룹이 실제로 쓰는 창인지 비트맵으로 확인한다. */
			continue;	/* [한국어] 아니면 건너뛴다. */

		rc = iommufd_sw_msi_install(ictx, hwpt_paging, cur);	/* [한국어] 그 창을 새 도메인에 매핑한다. 이것이 없으면 장치가 인터럽트를 쏠 수 없다. */
		if (rc)	/* [한국어] 실패하면 */
			return rc;	/* [한국어] 그대로 올린다. 호출자가 예약까지 되돌린다. */
	}
	return 0;	/* [한국어] 모두 심었다. */
}
#else
static inline int
/*
 * [한국어] CONFIG_IRQ_MSI_IOMMU 가 꺼진 커널의 빈 판.
 *
 * x86 처럼 인터럽트 재매핑이 IOMMU 밖에서 이뤄지는 플랫폼에서는 할 일이
 * 없다. 호출부에 #ifdef 를 뿌리는 대신 여기서 갈라, 부르는 쪽 코드가
 * 깨끗해진다.
 */
iommufd_group_setup_msi(struct iommufd_group *igroup,
			struct iommufd_hwpt_paging *hwpt_paging)
{
	return 0;	/* [한국어] 할 일이 없으니 늘 성공이다. */
}
#endif

static bool
/*
 * [한국어]
 * iommufd_group_first_attach - 이 PASID 의 첫 붙임인가
 *
 * @igroup: 대상 그룹.
 * @pasid: 물어보는 PASID.
 * @return: 아직 아무 장치도 붙지 않았으면 true.
 *
 * 첫 붙임일 때만 실제 하드웨어를 건드리고 예약 IOVA 를 등록한다.
 * 두 번째부터는 이미 같은 도메인에 붙어 있으므로 목록에 더하기만 한다.
 */
iommufd_group_first_attach(struct iommufd_group *igroup, ioasid_t pasid)
{
	lockdep_assert_held(&igroup->lock);	/* [한국어] 그룹 뮤텍스 아래에서만 뜻이 있는 판정이다. */
	return !xa_load(&igroup->pasid_attach, pasid);	/* [한국어] 항목이 없으면 아직 아무도 붙지 않았다. */
}

static int	/* [한국어] 이 아래 반환형이 다음 줄의 함수 이름과 나뉘어 있다. */
/*
 * [한국어]
 * iommufd_device_attach_reserved_iova - 이 장치의 예약 구간을 IOAS 에 등록한다
 *
 * @idev: 붙는 장치.
 * @hwpt_paging: 붙을 페이징 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 장치마다 쓰면 안 되는 IOVA 구간이 있다. 하드웨어가 그 주소를 DMA 가
 * 아닌 다른 뜻으로 해석하기 때문인데(대표적으로 MSI doorbell), 그 구간에
 * 매핑이 있으면 DMA 가 엉뚱한 곳으로 간다. 그래서 IOAS 에 "여기는 쓰지
 * 말라"고 등록한다.
 *
 * 이미 그 구간에 매핑이 있으면 실패한다 — 사용자가 먼저 매핑을 지워야
 * 이 장치를 붙일 수 있다.
 *
 * MSI 설정은 그룹의 첫 붙임일 때만 한다. 그룹 안의 장치들이 같은 도메인을
 * 쓰므로 한 번만 심으면 된다.
 */
iommufd_device_attach_reserved_iova(struct iommufd_device *idev,
				    struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_group *igroup = idev->igroup;	/* [한국어] 예약과 MSI 상태는 그룹이 든다. */
	int rc;	/* [한국어] 결과 코드. */

	lockdep_assert_held(&igroup->lock);	/* [한국어] 그룹 뮤텍스 아래에서 불려야 한다. */

	rc = iopt_table_enforce_dev_resv_regions(&hwpt_paging->ioas->iopt,	/* [한국어] 이 장치가 쓰면 안 되는 구간을 IOAS 에 등록한다. 이미 그 자리에 매핑이 있으면 실패한다. */
						 idev->dev,
						 &igroup->sw_msi_start);	/* [한국어] MSI 창의 시작 주소를 여기에 받아 둔다 — 나중에 그 창을 도메인에 심을 때 쓴다. */
	if (rc)	/* [한국어] 예약에 실패하면 */
		return rc;	/* [한국어] 붙일 수 없다. */

	if (iommufd_group_first_attach(igroup, IOMMU_NO_PASID)) {	/* [한국어] 그룹의 첫 장치일 때만 */
		rc = iommufd_group_setup_msi(igroup, hwpt_paging);	/* [한국어] MSI 창을 도메인에 심는다. 그룹이 같은 도메인을 쓰므로 한 번이면 된다. */
		if (rc) {	/* [한국어] 심기에 실패하면 */
			iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt,	/* [한국어] 방금 등록한 예약을 걷어 낸다. */
						  idev->dev);
			return rc;	/* [한국어] 실패를 올린다. */
		}
	}
	return 0;	/* [한국어] 성공. */
}

/* The device attach/detach/replace helpers for attach_handle */

/*
 * [한국어]
 * iommufd_device_is_attached - 이 장치가 그 PASID 로 붙어 있는가
 *
 * @idev: 물어보는 장치.
 * @pasid: 물어보는 PASID.
 * @return: 붙어 있으면 참.
 *
 * 그룹은 붙어 있어도 이 장치는 아직 아닐 수 있다 — 사용자가 그룹 안의
 * 장치를 하나씩 붙이기 때문이다. 교체 요청이 실제로 붙어 있는 장치에
 * 대한 것인지 확인하는 데 쓴다.
 */
static bool iommufd_device_is_attached(struct iommufd_device *idev,
				       ioasid_t pasid)
{
	struct iommufd_attach *attach;	/* [한국어] 그 PASID 의 붙임 상태. */

	attach = xa_load(&idev->igroup->pasid_attach, pasid);	/* [한국어] 그룹이 붙어 있는지 먼저 본다. */
	return xa_load(&attach->device_array, idev->obj.id);	/* [한국어] 그 안에 이 장치가 있는지. 그룹은 붙어 있어도 이 장치는 아직 아닐 수 있다. */
}

/*
 * [한국어]
 * iommufd_hwpt_pasid_compat - PASID 와 도메인의 궁합을 본다
 *
 * @hwpt: 붙일 도메인.
 * @idev: 붙는 장치.
 * @pasid: 붙일 PASID.
 * @return: 0 이면 괜찮다, -EINVAL 이면 안 된다.
 *
 * 어떤 도메인은 PASID 와 함께 쓸 수 없다. 하드웨어가 PASID 별 항목을
 * 다루는 방식이 도메인 형식에 따라 다르기 때문이다.
 *
 * 두 방향으로 검사한다. PASID 없는 붙임이면, 이 도메인이 PASID 를 못
 * 다루는데 이미 다른 PASID 가 붙어 있는지 본다. PASID 붙임이면, 이
 * 도메인 자신과 기본 도메인이 모두 PASID 를 다룰 수 있어야 한다.
 *
 * 기본 도메인까지 보는 이유: 하드웨어가 PASID 표를 기본 도메인 항목에
 * 매달아 두는 구조라, 그쪽이 PASID 를 모르면 표를 걸 자리가 없다.
 */
static int iommufd_hwpt_pasid_compat(struct iommufd_hw_pagetable *hwpt,
				     struct iommufd_device *idev,
				     ioasid_t pasid)
{
	struct iommufd_group *igroup = idev->igroup;	/* [한국어] 붙임 상태는 그룹이 든다. */

	lockdep_assert_held(&igroup->lock);	/* [한국어] 그룹 뮤텍스 아래에서 봐야 값이 흔들리지 않는다. */

	if (pasid == IOMMU_NO_PASID) {	/* [한국어] 장치 전체를 붙이는 경우. */
		unsigned long start = IOMMU_NO_PASID;	/* [한국어] PASID 0 다음부터 찾기 위한 시작점. */

		if (!hwpt->pasid_compat &&	/* [한국어] 이 도메인이 PASID 와 함께 쓸 수 없는 종류인데 */
		    xa_find_after(&igroup->pasid_attach,	/* [한국어] 그보다 큰 PASID 가 이미 붙어 있으면 */
				  &start, UINT_MAX, XA_PRESENT))
			return -EINVAL;	/* [한국어] 기본 도메인이 PASID 를 모르는 상태가 되어 앞뒤가 맞지 않는다. */
	} else {	/* [한국어] 특정 PASID 를 붙이는 경우. */
		struct iommufd_attach *attach;	/* [한국어] 기본 붙임 상태를 볼 지역 변수. */

		if (!hwpt->pasid_compat)	/* [한국어] 이 도메인 자체가 PASID 를 못 다루면 */
			return -EINVAL;	/* [한국어] 그 PASID 에 붙일 수 없다. */

		attach = xa_load(&igroup->pasid_attach, IOMMU_NO_PASID);	/* [한국어] 기본(PASID 없는) 붙임 상태를 본다. */
		if (attach && attach->hwpt && !attach->hwpt->pasid_compat)	/* [한국어] 하드웨어가 PASID 표를 기본 도메인 항목에 매다는 구조라, 그쪽이 PASID 를 모르면 표를 걸 자리가 없다. */
			return -EINVAL;	/* [한국어] 그래서 거절한다. */
	}

	return 0;	/* [한국어] 궁합이 맞는다. */
}

/*
 * [한국어]
 * iommufd_hwpt_compatible_device - 폴트 큐가 달린 도메인에 붙여도 되는 장치인가
 *
 * @hwpt: 붙일 도메인.
 * @idev: 붙는 장치.
 * @return: 괜찮으면 참.
 *
 * 폴트 큐가 없으면 아무 장치나 괜찮다. 문제는 사용자 공간이 폴트에
 * "실패"로 답할 수 있다는 점이다.
 *
 * 원 주석이 그 위험을 밝힌다 — PRI 는 물리 함수(PF)와 가상 함수(VF)가
 * 함께 쓰는 자원이라, VF 에 실패를 전하면 PF 까지 영향을 받는다. 그것을
 * 조율할 방법이 없어, 아예 그런 VF 는 폴트 큐가 달린 도메인에 붙이지
 * 않는다.
 */
static bool iommufd_hwpt_compatible_device(struct iommufd_hw_pagetable *hwpt,
					   struct iommufd_device *idev)
{
	struct pci_dev *pdev;	/* [한국어] PCI 장치로 본 모습. */

	if (!hwpt->fault || !dev_is_pci(idev->dev))	/* [한국어] 폴트 큐가 없거나 PCI 장치가 아니면 이 문제와 무관하다. */
		return true;	/* [한국어] 괜찮다. */

	/*
	 * Once we turn on PCI/PRI support for VF, the response failure code
	 * should not be forwarded to the hardware due to PRI being a shared
	 * resource between PF and VFs. There is no coordination for this
	 * shared capability. This waits for a vPRI reset to recover.
	 */
	pdev = to_pci_dev(idev->dev);	/* [한국어] PCI 쪽으로 내려간다. */

	return (!pdev->is_virtfn || !pci_pri_supported(pdev));	/* [한국어] 원 주석대로 PRI 는 PF 와 VF 가 함께 쓰는 자원이라, VF 에 실패 응답을 전하면 PF 까지 영향을 받는다. 조율할 방법이 없어 그런 VF 는 받지 않는다. */
}

/*
 * [한국어]
 * iommufd_hwpt_attach_device - 실제로 하드웨어에 붙인다
 *
 * @hwpt: 붙일 도메인.
 * @idev: 붙는 장치.
 * @pasid: 붙일 PASID. IOMMU_NO_PASID 면 장치 전체.
 * @return: 0 성공, 음수면 실패.
 *
 * attach 핸들을 만들어 iommu 코어에 함께 넘긴다. 그 핸들이 나중에
 * "이 폴트가 어느 iommufd 장치의 것인가"를 되짚는 열쇠가 된다.
 *
 * PASID 유무에 따라 부르는 코어 함수가 다르다. PASID 없는 붙임은 그룹
 * 단위이고, PASID 붙임은 장치 단위다 — PASID 는 장치마다 따로 있다.
 */
static int iommufd_hwpt_attach_device(struct iommufd_hw_pagetable *hwpt,
				      struct iommufd_device *idev,
				      ioasid_t pasid)
{
	struct iommufd_attach_handle *handle;	/* [한국어] 코어에 함께 넘길 핸들. */
	int rc;	/* [한국어] 결과 코드. */

	if (!iommufd_hwpt_compatible_device(hwpt, idev))	/* [한국어] 폴트 큐와 이 장치의 궁합을 먼저 본다. */
		return -EINVAL;	/* [한국어] 맞지 않는다. */

	rc = iommufd_hwpt_pasid_compat(hwpt, idev, pasid);	/* [한국어] PASID 궁합도 본다. */
	if (rc)	/* [한국어] 맞지 않으면 */
		return rc;	/* [한국어] 그대로 올린다. */

	handle = kzalloc_obj(*handle);	/* [한국어] 핸들을 만든다. 폴트가 왔을 때 어느 장치인지 되짚는 열쇠가 된다. */
	if (!handle)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */

	handle->idev = idev;	/* [한국어] 핸들에서 장치로 가는 길. */
	if (pasid == IOMMU_NO_PASID)	/* [한국어] 장치 전체를 붙이는 경우는 */
		rc = iommu_attach_group_handle(hwpt->domain, idev->igroup->group,	/* [한국어] 그룹 단위로 붙인다 — 그룹은 나눌 수 없다. */
					       &handle->handle);
	else
		rc = iommu_attach_device_pasid(hwpt->domain, idev->dev, pasid,	/* [한국어] PASID 는 장치마다 따로 있어 장치 단위로 붙인다. */
					       &handle->handle);
	if (rc)	/* [한국어] 붙이기에 실패하면 */
		goto out_free_handle;	/* [한국어] 핸들을 버린다. */

	return 0;	/* [한국어] 성공. */

out_free_handle:	/* [한국어] 실패 경로. */
	kfree(handle);	/* [한국어] 코어가 받지 않았으므로 여기서 해제한다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

static struct iommufd_attach_handle *	/* [한국어] 반환형이 길어 다음 줄과 나뉘었다. */
/*
 * [한국어]
 * iommufd_device_get_attach_handle - 붙일 때 넘겼던 핸들을 되찾는다
 *
 * @idev: 대상 장치.
 * @pasid: 대상 PASID.
 * @return: 그 핸들, 없으면 NULL.
 *
 * 뗄 때 그 핸들을 해제해야 하고, 그 전에 그 핸들에 매달린 답 없는
 * 폴트들을 처리해야 한다.
 *
 * 오류를 NULL 로 바꾸는 이유: 호출자에게는 "없다"와 "찾을 수 없다"가
 * 같은 뜻이다.
 */
iommufd_device_get_attach_handle(struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommu_attach_handle *handle;	/* [한국어] 코어가 들고 있던 핸들. */

	lockdep_assert_held(&idev->igroup->lock);	/* [한국어] 붙임 상태가 흔들리지 않는 동안에만 뜻이 있다. */

	handle = iommu_attach_handle_get(idev->igroup->group, pasid, 0);	/* [한국어] 붙일 때 넘겼던 핸들을 코어에서 되찾는다. */
	if (IS_ERR(handle))	/* [한국어] 붙어 있지 않거나 다른 이유로 없다. */
		return NULL;	/* [한국어] 호출자에게는 "없다"와 같은 뜻이다. */
	return to_iommufd_handle(handle);	/* [한국어] 코어 핸들에서 iommufd 핸들로 되짚는다. */
}

/*
 * [한국어]
 * iommufd_hwpt_detach_device - 실제로 하드웨어에서 뗀다
 *
 * @hwpt: 붙어 있던 도메인.
 * @idev: 떼는 장치.
 * @pasid: 떼는 PASID.
 *
 * 순서가 중요하다. 먼저 하드웨어에서 떼어 새 폴트가 생기지 않게 한 뒤,
 * 이미 쌓인 답 없는 폴트에 실패로 응답한다. 응답하지 않으면 장치가
 * 영원히 기다린다.
 */
static void iommufd_hwpt_detach_device(struct iommufd_hw_pagetable *hwpt,
				       struct iommufd_device *idev,
				       ioasid_t pasid)
{
	struct iommufd_attach_handle *handle;	/* [한국어] 풀어야 할 핸들. */

	handle = iommufd_device_get_attach_handle(idev, pasid);	/* [한국어] 떼기 전에 핸들을 잡아 둔다 — 뗀 뒤에는 찾을 수 없다. */
	if (pasid == IOMMU_NO_PASID)	/* [한국어] 장치 전체를 뗄 때는 */
		iommu_detach_group_handle(hwpt->domain, idev->igroup->group);	/* [한국어] 그룹 단위로 뗀다. 이 순간부터 새 폴트가 생기지 않는다. */
	else
		iommu_detach_device_pasid(hwpt->domain, idev->dev, pasid);	/* [한국어] PASID 는 장치 단위로 뗀다. */

	iommufd_auto_response_faults(hwpt, handle);	/* [한국어] 이미 쌓인 답 없는 폴트에 실패로 응답한다. 응답하지 않으면 장치가 영원히 기다린다. */
	kfree(handle);	/* [한국어] 핸들을 해제한다. */
}

/*
 * [한국어]
 * iommufd_hwpt_replace_device - 하드웨어에서 도메인을 바꿔치기한다
 *
 * @idev: 대상 장치.
 * @pasid: 대상 PASID.
 * @hwpt: 새 도메인.
 * @old: 옛 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 떼고 붙이는 것이 아니라 한 번에 바꾼다. 그 사이 DMA 가 막히는 창이
 * 없어야 하기 때문이다 — 게스트가 도메인을 바꾸는 동안에도 장치는
 * 계속 돌고 있다.
 *
 * 옛 핸들의 폴트 응답은 교체가 성공한 뒤에 한다. 실패하면 옛 도메인이
 * 그대로 남아 있으므로 그 폴트들도 그대로 두어야 한다.
 */
static int iommufd_hwpt_replace_device(struct iommufd_device *idev,
				       ioasid_t pasid,
				       struct iommufd_hw_pagetable *hwpt,
				       struct iommufd_hw_pagetable *old)
{
	struct iommufd_attach_handle *handle, *old_handle;	/* [한국어] 새로 만들 핸들과 놓아 줄 옛 핸들. */
	int rc;	/* [한국어] 결과 코드. */

	if (!iommufd_hwpt_compatible_device(hwpt, idev))	/* [한국어] 새 도메인과 이 장치의 궁합. */
		return -EINVAL;	/* [한국어] 맞지 않는다. */

	rc = iommufd_hwpt_pasid_compat(hwpt, idev, pasid);	/* [한국어] PASID 궁합. */
	if (rc)	/* [한국어] 맞지 않으면 */
		return rc;	/* [한국어] 그대로 올린다. */

	old_handle = iommufd_device_get_attach_handle(idev, pasid);	/* [한국어] 바꾸기 전에 옛 핸들을 잡아 둔다. */

	handle = kzalloc_obj(*handle);	/* [한국어] 새 핸들을 만든다. */
	if (!handle)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. 이 시점에는 아직 아무것도 바뀌지 않았다. */

	handle->idev = idev;	/* [한국어] 새 핸들에서 장치로 가는 길. */
	if (pasid == IOMMU_NO_PASID)	/* [한국어] 장치 전체를 바꾸는 경우. */
		rc = iommu_replace_group_handle(idev->igroup->group,	/* [한국어] 떼고 붙이는 것이 아니라 한 번에 바꾼다 — 그 사이 DMA 가 막히지 않는다. */
						hwpt->domain, &handle->handle);
	else
		rc = iommu_replace_device_pasid(hwpt->domain, idev->dev,	/* [한국어] PASID 쪽도 같은 방식으로 한 번에 바꾼다. */
						pasid, &handle->handle);
	if (rc)	/* [한국어] 바꾸기에 실패하면 */
		goto out_free_handle;	/* [한국어] 옛 상태가 그대로 남는다. 새 핸들만 버린다. */

	iommufd_auto_response_faults(hwpt, old_handle);	/* [한국어] 성공한 뒤에야 옛 핸들의 폴트를 정리한다 — 실패했다면 그 폴트들도 그대로 두어야 한다. */
	kfree(old_handle);	/* [한국어] 옛 핸들을 해제한다. */

	return 0;	/* [한국어] 성공. */

out_free_handle:	/* [한국어] 실패 경로. */
	kfree(handle);	/* [한국어] 코어가 받지 않은 새 핸들을 버린다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * iommufd_hw_pagetable_attach - 장치를 페이지 테이블에 붙인다
 *
 * @hwpt: 붙일 페이지 테이블.
 * @idev: 붙는 장치.
 * @pasid: 붙일 PASID.
 * @return: 0 성공, 음수면 실패.
 *
 * 이 파일에서 가장 조심스러운 함수다. 그룹 단위 상태와 장치 단위 상태를
 * 함께 세우면서, 어느 단계에서 실패해도 온전히 되돌려야 한다.
 *
 * 자리 잡기(XA_ZERO_ENTRY)를 쓰는 것이 요점이다. 진짜 값을 넣기 전에
 * 자리만 예약해 두면, 그 뒤의 할당이 실패해도 되돌리기가 쉽고 그 사이
 * 다른 스레드가 같은 자리를 차지하지 못한다.
 *
 * 원 주석대로 그룹의 첫 장치일 때만 실제로 붙인다. 나머지는 이미 붙은
 * 도메인을 따라간다. 그래도 사용자가 장치를 하나씩 붙여야 하는 이유는,
 * 예약 IOVA 가 장치마다 다르고 붙일 때만 등록되기 때문이다.
 *
 * 실패 경로가 계단식으로 이어진다 — 되돌릴 것이 단계마다 늘어나므로
 * 진행한 만큼만 되짚어 푼다.
 */
int iommufd_hw_pagetable_attach(struct iommufd_hw_pagetable *hwpt,
				struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommufd_hwpt_paging *hwpt_paging = find_hwpt_paging(hwpt);	/* [한국어] 중첩 도메인이면 그 부모 페이징 도메인을, 아니면 자기 자신을 얻는다. 예약 IOVA 는 페이징 쪽 IOAS 에 건다. */
	bool attach_resv = hwpt_paging && pasid == IOMMU_NO_PASID;	/* [한국어] 예약 IOVA 를 다룰지 여부. PASID 붙임에는 걸지 않는다 — 장치 단위 예약은 기본 붙임 때 이미 걸렸다. */
	struct iommufd_group *igroup = idev->igroup;	/* [한국어] 붙임 상태를 든 그룹. */
	struct iommufd_hw_pagetable *old_hwpt;	/* [한국어] 이미 붙어 있던 도메인(있다면). */
	struct iommufd_attach *attach;	/* [한국어] 이 PASID 의 붙임 상태. */
	int rc;	/* [한국어] 결과 코드. */

	mutex_lock(&igroup->lock);	/* [한국어] 그룹 상태 전체를 이 뮤텍스가 지킨다. */

	attach = xa_cmpxchg(&igroup->pasid_attach, pasid, NULL,	/* [한국어] 비어 있으면 자리표시자를 넣어 예약한다. 다른 스레드가 같은 PASID 를 동시에 붙이지 못하게 막는 장치다. */
			    XA_ZERO_ENTRY, GFP_KERNEL);
	if (xa_is_err(attach)) {	/* [한국어] xarray 자체의 오류(메모리 부족 등). */
		rc = xa_err(attach);	/* [한국어] 오류를 꺼내 */
		goto err_unlock;	/* [한국어] 락만 놓고 나간다. */
	}

	if (!attach) {	/* [한국어] 비어 있었다 = 이 PASID 의 첫 붙임이다. */
		attach = kzalloc_obj(*attach);	/* [한국어] 붙임 상태를 만든다. */
		if (!attach) {	/* [한국어] 메모리가 없다. */
			rc = -ENOMEM;	/* [한국어] 실패. */
			goto err_release_pasid;	/* [한국어] 예약해 둔 자리를 반납한다. */
		}
		xa_init(&attach->device_array);	/* [한국어] 장치 목록을 비운 상태로 초기화. */
	}

	old_hwpt = attach->hwpt;	/* [한국어] 이미 붙어 있는 도메인. 첫 붙임이면 NULL 이다. */

	rc = xa_insert(&attach->device_array, idev->obj.id, XA_ZERO_ENTRY,	/* [한국어] 장치 자리도 먼저 예약한다. 이미 있으면 -EBUSY 가 되어 중복 붙임을 잡아 낸다. */
		       GFP_KERNEL);
	if (rc) {	/* [한국어] 장치 자리 예약에 실패했다. */
		WARN_ON(rc == -EBUSY && !old_hwpt);	/* [한국어] 아무것도 붙어 있지 않은데 장치가 이미 목록에 있다면 앞뒤가 맞지 않는다. */
		goto err_free_attach;	/* [한국어] 방금 만든 붙임 상태를 되돌린다. */
	}

	if (old_hwpt && old_hwpt != hwpt) {	/* [한국어] 그룹이 이미 다른 도메인에 붙어 있으면 */
		rc = -EINVAL;	/* [한국어] 같은 그룹의 장치들은 반드시 같은 도메인을 써야 한다. */
		goto err_release_devid;	/* [한국어] 예약한 장치 자리를 반납한다. */
	}

	if (attach_resv) {	/* [한국어] 예약 IOVA 를 다뤄야 하는 경우 */
		rc = iommufd_device_attach_reserved_iova(idev, hwpt_paging);	/* [한국어] 이 장치의 예약 구간을 IOAS 에 등록한다. */
		if (rc)	/* [한국어] 예약 IOVA 등록에 실패했다. */
			goto err_release_devid;	/* [한국어] 실패하면 장치 자리를 반납한다. */
	}

	/*
	 * Only attach to the group once for the first device that is in the
	 * group. All the other devices will follow this attachment. The user
	 * should attach every device individually to the hwpt as the per-device
	 * reserved regions are only updated during individual device
	 * attachment.
	 */
	if (iommufd_group_first_attach(igroup, pasid)) {	/* [한국어] 원 주석대로 그룹의 첫 장치일 때만 실제로 붙인다. */
		rc = iommufd_hwpt_attach_device(hwpt, idev, pasid);	/* [한국어] 하드웨어에 붙인다. */
		if (rc)	/* [한국어] 하드웨어 붙이기에 실패했다. */
			goto err_unresv;	/* [한국어] 실패하면 방금 등록한 예약을 걷는다. */
		attach->hwpt = hwpt;	/* [한국어] 붙은 도메인을 기록한다. */
		WARN_ON(xa_is_err(xa_store(&igroup->pasid_attach, pasid, attach,	/* [한국어] 예약해 둔 자리에 진짜 값을 넣는다. 자리가 이미 있으므로 실패할 수 없다. */
					   GFP_KERNEL)));
	}
	refcount_inc(&hwpt->obj.users);	/* [한국어] 이 장치 몫의 도메인 참조. 뗄 때 놓는다. */
	WARN_ON(xa_is_err(xa_store(&attach->device_array, idev->obj.id,	/* [한국어] 예약해 둔 장치 자리에 진짜 값을 넣는다. */
				   idev, GFP_KERNEL)));
	mutex_unlock(&igroup->lock);	/* [한국어] 성공. 락을 놓는다. */
	return 0;	/* [한국어] 붙였다. */
err_unresv:	/* [한국어] 하드웨어 붙이기 실패 경로. */
	if (attach_resv)	/* [한국어] 예약을 걸었다면 */
		iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt, idev->dev);	/* [한국어] 걷어 낸다. */
err_release_devid:	/* [한국어] 장치 자리를 예약한 뒤의 실패들이 합류한다. */
	xa_release(&attach->device_array, idev->obj.id);	/* [한국어] 자리표시자를 반납한다. */
err_free_attach:	/* [한국어] 붙임 상태를 만든 뒤의 실패들이 합류한다. */
	if (iommufd_group_first_attach(igroup, pasid))	/* [한국어] 이번에 만든 것일 때만 */
		kfree(attach);	/* [한국어] 해제한다. 이미 있던 것이면 남의 것이라 건드리면 안 된다. */
err_release_pasid:	/* [한국어] PASID 자리를 예약한 뒤의 실패들이 합류한다. */
	if (iommufd_group_first_attach(igroup, pasid))	/* [한국어] 이번에 자리를 예약한 경우에만 */
		xa_release(&igroup->pasid_attach, pasid);	/* [한국어] 자리표시자를 반납한다. */
err_unlock:	/* [한국어] 모든 실패가 마지막으로 합류한다. */
	mutex_unlock(&igroup->lock);	/* [한국어] 락을 놓고 */
	return rc;	/* [한국어] 실패를 올린다. */
}

struct iommufd_hw_pagetable *	/* [한국어] 반환형이 다음 줄의 함수 이름과 나뉘어 있다. */
/*
 * [한국어]
 * iommufd_hw_pagetable_detach - 장치를 페이지 테이블에서 뗀다
 *
 * @idev: 떼는 장치.
 * @pasid: 떼는 PASID.
 * @return: 떼어 낸 페이지 테이블. 붙어 있지 않았으면 NULL.
 *
 * 그룹의 마지막 장치일 때만 실제로 하드웨어에서 뗀다.
 *
 * 예약 IOVA 는 장치마다 등록했으므로 장치마다 지운다 — 마지막인지와
 * 무관하다.
 *
 * 도메인 포인터를 돌려주는 이유는 원 주석이 밝힌다: 호출자가 그것을
 * 파괴해야 한다. 여기서 파괴하지 않는 것은 호출자가 락을 쥔 상태일 수
 * 있어서다.
 */
iommufd_hw_pagetable_detach(struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommufd_group *igroup = idev->igroup;	/* [한국어] 붙임 상태를 든 그룹. */
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 예약을 걸었던 페이징 도메인. */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 떼어 낼 도메인. */
	struct iommufd_attach *attach;	/* [한국어] 이 PASID 의 붙임 상태. */

	mutex_lock(&igroup->lock);	/* [한국어] 그룹 상태를 지키는 뮤텍스. */
	attach = xa_load(&igroup->pasid_attach, pasid);	/* [한국어] 붙어 있는지 본다. */
	if (!attach) {	/* [한국어] 붙어 있지 않았다. */
		mutex_unlock(&igroup->lock);	/* [한국어] 락을 놓고 */
		return NULL;	/* [한국어] 호출자에게 할 일이 없음을 알린다. */
	}

	hwpt = attach->hwpt;	/* [한국어] 떼어 낼 도메인. */
	hwpt_paging = find_hwpt_paging(hwpt);	/* [한국어] 예약이 걸린 IOAS 를 찾기 위해. */

	xa_erase(&attach->device_array, idev->obj.id);	/* [한국어] 이 장치를 목록에서 뺀다. */
	if (xa_empty(&attach->device_array)) {	/* [한국어] 그룹의 마지막 장치였다면 */
		iommufd_hwpt_detach_device(hwpt, idev, pasid);	/* [한국어] 이제 실제로 하드웨어에서 뗀다. */
		xa_erase(&igroup->pasid_attach, pasid);	/* [한국어] 붙임 상태를 지우고 */
		kfree(attach);	/* [한국어] 해제한다. */
	}
	if (hwpt_paging && pasid == IOMMU_NO_PASID)	/* [한국어] 예약은 장치마다 걸었으므로 */
		iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt, idev->dev);	/* [한국어] 마지막인지와 무관하게 이 장치 것을 지운다. */
	mutex_unlock(&igroup->lock);	/* [한국어] 락을 놓는다. */

	iommufd_hw_pagetable_put(idev->ictx, hwpt);	/* [한국어] 이 장치 몫으로 들었던 참조를 놓는다. */

	/* Caller must destroy hwpt */
	return hwpt;	/* [한국어] 원 주석대로 호출자가 이것을 파괴한다. */
}

static struct iommufd_hw_pagetable *	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iommufd_device_do_attach - 붙이기를 교체와 같은 모양으로 감싼다
 *
 * @idev: 붙는 장치.
 * @pasid: 붙일 PASID.
 * @hwpt: 붙일 페이지 테이블.
 * @return: 늘 NULL(성공), 실패하면 오류 포인터.
 *
 * 아래 attach_fn 표에 들어갈 수 있게 교체 함수와 서명을 맞춘 것이다.
 * 교체는 옛 도메인을 돌려주지만 붙이기는 돌려줄 것이 없어 NULL 이다.
 *
 * 이렇게 감싸 두면 붙이기와 교체를 같은 고리로 다룰 수 있다.
 */
iommufd_device_do_attach(struct iommufd_device *idev, ioasid_t pasid,
			 struct iommufd_hw_pagetable *hwpt)
{
	int rc;	/* [한국어] 결과 코드. */

	rc = iommufd_hw_pagetable_attach(hwpt, idev, pasid);	/* [한국어] 실제 붙이기. */
	if (rc)	/* [한국어] 실패하면 */
		return ERR_PTR(rc);	/* [한국어] 오류 포인터로 알린다 — 교체 함수와 반환 형식을 맞춘 결과다. */
	return NULL;	/* [한국어] 붙이기는 파괴할 옛 도메인이 없다. */
}

static void	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iommufd_group_remove_reserved_iova - 그룹 전체의 예약 구간을 지운다
 *
 * @igroup: 대상 그룹.
 * @hwpt_paging: 예약을 등록해 두었던 도메인.
 *
 * 교체에서 쓴다. 새 IOAS 로 옮겨 갈 때 옛 IOAS 에 남긴 예약을 걷어야
 * 하는데, 그때는 그룹 안의 모든 장치 것을 한꺼번에 지운다.
 *
 * 붙이기·떼기가 장치 단위인 것과 대비된다 — 교체는 그룹 전체가 한 번에
 * 옮겨 가기 때문이다.
 */
iommufd_group_remove_reserved_iova(struct iommufd_group *igroup,
				   struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_attach *attach;	/* [한국어] 기본 붙임 상태. */
	struct iommufd_device *cur;	/* [한국어] 훑어볼 장치. */
	unsigned long index;	/* [한국어] xarray 순회용 첨자. */

	lockdep_assert_held(&igroup->lock);	/* [한국어] 그룹 뮤텍스 아래에서만 목록이 안정하다. */

	attach = xa_load(&igroup->pasid_attach, IOMMU_NO_PASID);	/* [한국어] 예약은 기본 붙임에만 걸린다. */
	xa_for_each(&attach->device_array, index, cur)	/* [한국어] 그룹 안의 모든 장치를 훑으며 */
		iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt, cur->dev);	/* [한국어] 각자의 예약 구간을 지운다. */
}

static int	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iommufd_group_do_replace_reserved_iova - 교체할 IOAS 에 예약을 옮긴다
 *
 * @igroup: 대상 그룹.
 * @hwpt_paging: 새 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * IOAS 가 그대로면 아무것도 하지 않는다 — 예약은 IOAS 에 걸리므로 같은
 * IOAS 로 옮기는 것은 예약과 무관하다.
 *
 * 바뀌면 그룹의 모든 장치 예약을 새 IOAS 에 등록한다. 중간에 실패하면
 * 방금 등록한 것들을 모두 걷어 낸다.
 *
 * sw_msi_start 를 NULL 로 넘기는 데 주의 — 이미 그룹이 그 값을 알고
 * 있어 다시 받을 필요가 없다.
 */
iommufd_group_do_replace_reserved_iova(struct iommufd_group *igroup,
				       struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_hwpt_paging *old_hwpt_paging;	/* [한국어] 지금 붙어 있는 페이징 도메인. */
	struct iommufd_attach *attach;	/* [한국어] 기본 붙임 상태. */
	struct iommufd_device *cur;	/* [한국어] 훑어볼 장치. */
	unsigned long index;	/* [한국어] xarray 순회용 첨자. */
	int rc;	/* [한국어] 결과 코드. */

	lockdep_assert_held(&igroup->lock);	/* [한국어] 그룹 뮤텍스 아래. */

	attach = xa_load(&igroup->pasid_attach, IOMMU_NO_PASID);	/* [한국어] 기본 붙임 상태를 본다. */
	old_hwpt_paging = find_hwpt_paging(attach->hwpt);	/* [한국어] 지금 예약이 걸린 곳. */
	if (!old_hwpt_paging || hwpt_paging->ioas != old_hwpt_paging->ioas) {	/* [한국어] IOAS 가 바뀌는 경우에만 예약을 옮긴다 — 예약은 IOAS 에 걸리므로 같은 IOAS 면 할 일이 없다. */
		xa_for_each(&attach->device_array, index, cur) {	/* [한국어] 그룹 안의 모든 장치에 대해 */
			rc = iopt_table_enforce_dev_resv_regions(	/* [한국어] 새 IOAS 에 예약을 등록한다. */
				&hwpt_paging->ioas->iopt, cur->dev, NULL);	/* [한국어] MSI 시작 주소는 받지 않는다 — 그룹이 이미 알고 있다. */
			if (rc)	/* [한국어] 한 장치의 예약 등록에 실패했다. */
				goto err_unresv;	/* [한국어] 중간에 실패하면 방금 등록한 것들을 모두 걷는다. */
		}
	}

	rc = iommufd_group_setup_msi(igroup, hwpt_paging);	/* [한국어] 새 도메인에도 MSI 창을 심는다. */
	if (rc)	/* [한국어] 실패하면 */
		goto err_unresv;	/* [한국어] 예약을 걷는다. */
	return 0;	/* [한국어] 성공. */

err_unresv:	/* [한국어] 실패 경로. */
	iommufd_group_remove_reserved_iova(igroup, hwpt_paging);	/* [한국어] 새 IOAS 에 걸었던 예약을 모두 걷는다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

static struct iommufd_hw_pagetable *	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iommufd_device_do_replace - 붙어 있는 페이지 테이블을 바꾼다
 *
 * @idev: 대상 장치.
 * @pasid: 대상 PASID.
 * @hwpt: 새 페이지 테이블.
 * @return: 떼어 낸 옛 것(호출자가 파괴한다), 바꿀 필요가 없었으면 NULL,
 *   실패하면 오류 포인터.
 *
 * 게스트가 주소 공간을 바꿀 때 쓰는 경로다. 떼었다 붙이면 그 사이 DMA 가
 * 막혀 장치가 오류를 내므로, 한 번에 바꿔야 한다.
 *
 * 참조 수 옮기기가 이 함수의 까다로운 대목이다. 그룹 안의 장치 수만큼
 * 참조가 옛 도메인에 걸려 있었으니 그만큼을 새 도메인으로 옮긴다.
 * 원 주석대로 이 스레드 몫 하나는 남겨 두는데, 호출자가 그것으로
 * 옛 도메인을 파괴하기 때문이다.
 *
 * 같은 도메인으로 바꾸라는 요청은 아무 일도 하지 않고 NULL 을 돌려준다 —
 * 오류가 아니라 이미 원하는 상태다.
 */
iommufd_device_do_replace(struct iommufd_device *idev, ioasid_t pasid,
			  struct iommufd_hw_pagetable *hwpt)
{
	struct iommufd_hwpt_paging *hwpt_paging = find_hwpt_paging(hwpt);	/* [한국어] 새 도메인 쪽의 페이징 도메인. */
	bool attach_resv = hwpt_paging && pasid == IOMMU_NO_PASID;	/* [한국어] 예약 IOVA 를 옮길지 여부. */
	struct iommufd_hwpt_paging *old_hwpt_paging;	/* [한국어] 옛 도메인 쪽의 페이징 도메인. */
	struct iommufd_group *igroup = idev->igroup;	/* [한국어] 붙임 상태를 든 그룹. */
	struct iommufd_hw_pagetable *old_hwpt;	/* [한국어] 바꾸기 전에 붙어 있던 도메인. */
	struct iommufd_attach *attach;	/* [한국어] 이 PASID 의 붙임 상태. */
	unsigned int num_devices;	/* [한국어] 참조를 옮길 개수. */
	int rc;	/* [한국어] 결과 코드. */

	mutex_lock(&igroup->lock);	/* [한국어] 그룹 상태를 지키는 뮤텍스. */

	attach = xa_load(&igroup->pasid_attach, pasid);	/* [한국어] 붙어 있는지 본다. */
	if (!attach) {	/* [한국어] 붙어 있지 않으면 바꿀 것도 없다. */
		rc = -EINVAL;	/* [한국어] 교체는 붙어 있는 상태에서만 뜻이 있다. */
		goto err_unlock;	/* [한국어] 락을 놓고 나간다. */
	}

	old_hwpt = attach->hwpt;	/* [한국어] 옛 도메인. */

	WARN_ON(!old_hwpt || xa_empty(&attach->device_array));	/* [한국어] 붙임 상태가 있는데 도메인이 없거나 장치가 하나도 없다면 앞뒤가 맞지 않는다. */

	if (!iommufd_device_is_attached(idev, pasid)) {	/* [한국어] 그룹은 붙어 있어도 이 장치는 아직 아닐 수 있다. */
		rc = -EINVAL;	/* [한국어] 붙지 않은 장치를 교체할 수는 없다. */
		goto err_unlock;	/* [한국어] 락을 놓고 나간다. */
	}

	if (hwpt == old_hwpt) {	/* [한국어] 같은 도메인으로 바꾸라는 요청이면 */
		mutex_unlock(&igroup->lock);	/* [한국어] 아무 일도 하지 않고 */
		return NULL;	/* [한국어] 이미 원하는 상태다. 오류가 아니다. */
	}

	if (attach_resv) {	/* [한국어] 예약 IOVA 를 옮겨야 하면 */
		rc = iommufd_group_do_replace_reserved_iova(igroup, hwpt_paging);	/* [한국어] 새 IOAS 에 그룹 전체의 예약을 등록한다. */
		if (rc)	/* [한국어] 예약 옮기기에 실패했다. */
			goto err_unlock;	/* [한국어] 실패하면 아무것도 바뀌지 않은 채 나간다. */
	}

	rc = iommufd_hwpt_replace_device(idev, pasid, hwpt, old_hwpt);	/* [한국어] 하드웨어에서 한 번에 바꿔치기한다. */
	if (rc)	/* [한국어] 하드웨어 교체에 실패했다. */
		goto err_unresv;	/* [한국어] 실패하면 새 IOAS 에 걸었던 예약을 걷는다. */

	old_hwpt_paging = find_hwpt_paging(old_hwpt);	/* [한국어] 옛 쪽의 페이징 도메인. */
	if (old_hwpt_paging && pasid == IOMMU_NO_PASID &&	/* [한국어] 옛 IOAS 에 예약이 걸려 있었고 */
	    (!hwpt_paging || hwpt_paging->ioas != old_hwpt_paging->ioas))	/* [한국어] 새 IOAS 가 다르다면 */
		iommufd_group_remove_reserved_iova(igroup, old_hwpt_paging);	/* [한국어] 옛 IOAS 의 예약을 걷어 낸다. 같은 IOAS 면 그대로 두어야 한다. */

	attach->hwpt = hwpt;	/* [한국어] 새 도메인을 기록한다. */

	num_devices = iommufd_group_device_num(igroup, pasid);	/* [한국어] 장치 수만큼의 참조가 옛 도메인에 걸려 있다. */
	/*
	 * Move the refcounts held by the device_array to the new hwpt. Retain a
	 * refcount for this thread as the caller will free it.
	 */
	refcount_add(num_devices, &hwpt->obj.users);	/* [한국어] 그만큼을 새 도메인에 더한다. */
	if (num_devices > 1)	/* [한국어] 원 주석대로 이 스레드 몫 하나는 남긴다. */
		WARN_ON(refcount_sub_and_test(num_devices - 1,	/* [한국어] 나머지를 옛 도메인에서 뺀다. 여기서 0 이 되면 남긴 하나가 사라진 것이라 버그다. */
					      &old_hwpt->obj.users));
	mutex_unlock(&igroup->lock);	/* [한국어] 락을 놓는다. */

	/* Caller must destroy old_hwpt */
	return old_hwpt;	/* [한국어] 원 주석대로 호출자가 남은 참조로 이것을 파괴한다. */
err_unresv:	/* [한국어] 하드웨어 교체 실패 경로. */
	if (attach_resv)	/* [한국어] 예약을 옮겼다면 */
		iommufd_group_remove_reserved_iova(igroup, hwpt_paging);	/* [한국어] 새 IOAS 것을 걷어 낸다. */
err_unlock:	/* [한국어] 모든 실패가 합류한다. */
	mutex_unlock(&igroup->lock);	/* [한국어] 락을 놓고 */
	return ERR_PTR(rc);	/* [한국어] 오류 포인터로 알린다. */
}

typedef struct iommufd_hw_pagetable *(*attach_fn)(	/* [한국어] 붙이기와 교체가 공유하는 함수 포인터 형. 둘의 서명을 맞춰 두어 같은 몸통을 나눠 쓸 수 있게 한다. */
	struct iommufd_device *idev, ioasid_t pasid,
	struct iommufd_hw_pagetable *hwpt);

/*
 * When automatically managing the domains we search for a compatible domain in
 * the iopt and if one is found use it, otherwise create a new domain.
 * Automatic domain selection will never pick a manually created domain.
 */
static struct iommufd_hw_pagetable *	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_device_auto_get_domain - IOAS 만 지정했을 때 쓸 도메인을 마련한다
 *
 * @idev: 붙는 장치.
 * @pasid: 붙일 PASID.
 * @ioas: 사용자가 지정한 주소 공간.
 * @pt_id: 실제로 쓴 도메인의 id 를 여기에 쓴다.
 * @do_attach: 붙이기 또는 교체 함수.
 * @return: 호출자가 파괴할 옛 도메인, 없으면 NULL, 실패하면 오류 포인터.
 *
 * 사용자가 IOAS 를 주면 커널이 도메인을 골라 준다. 이미 있는 자동 도메인
 * 중에 이 장치가 붙을 수 있는 것이 있으면 그것을 쓰고, 없으면 만든다.
 *
 * 원 주석대로 손으로 만든 도메인은 절대 고르지 않는다 — 사용자가 특정
 * 목적으로 만든 것이라 마음대로 나눠 쓰면 안 된다.
 *
 * -EINVAL 만 "이 도메인과는 안 맞는다"로 읽고 다음 후보로 넘어가는 것이
 * 요점이다. 다른 오류는 진짜 실패이므로 그대로 사용자에게 올린다.
 *
 * immediate_attach 는 도메인을 만드는 중에 붙이기까지 하는 방식이다.
 * 원 주석대로 어떤 드라이버는 붙이지 않고서는 도메인을 만들 수 없어
 * 그 경로가 필요하다. 교체에는 쓸 수 없어 붙이기일 때만 고른다.
 */
iommufd_device_auto_get_domain(struct iommufd_device *idev, ioasid_t pasid,
			       struct iommufd_ioas *ioas, u32 *pt_id,
			       attach_fn do_attach)
{
	/*
	 * iommufd_hw_pagetable_attach() is called by
	 * iommufd_hwpt_paging_alloc() in immediate attachment mode, same as
	 * iommufd_device_do_attach(). So if we are in this mode then we prefer
	 * to use the immediate_attach path as it supports drivers that can't
	 * directly allocate a domain.
	 */
	bool immediate_attach = do_attach == iommufd_device_do_attach;	/* [한국어] 원 주석대로 붙이기일 때만 즉시 붙임 방식을 쓸 수 있다. 교체는 그 방식으로 표현되지 않는다. */
	struct iommufd_hw_pagetable *destroy_hwpt;	/* [한국어] 호출자가 파괴할 도메인(교체일 때의 옛 것). */
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] 후보로 훑어볼 자동 도메인. */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 그 도메인의 공통 부분. */

	/*
	 * There is no differentiation when domains are allocated, so any domain
	 * that is willing to attach to the device is interchangeable with any
	 * other.
	 */
	mutex_lock(&ioas->mutex);	/* [한국어] 도메인 목록을 지키는 뮤텍스. */
	list_for_each_entry(hwpt_paging, &ioas->hwpt_list, hwpt_item) {	/* [한국어] 이 IOAS 에 딸린 도메인들을 훑는다. */
		if (!hwpt_paging->auto_domain)	/* [한국어] 원 주석대로 손으로 만든 도메인은 고르지 않는다 — 사용자가 특정 목적으로 만든 것이다. */
			continue;	/* [한국어] 다음 후보로. */

		hwpt = &hwpt_paging->common;	/* [한국어] 공통 부분으로 올라간다. */
		if (!iommufd_lock_obj(&hwpt->obj))	/* [한국어] 파괴 중인 도메인이면 */
			continue;	/* [한국어] 건너뛴다. */
		destroy_hwpt = (*do_attach)(idev, pasid, hwpt);	/* [한국어] 붙여 본다. 이것이 궁합을 실제로 시험하는 유일한 방법이다. */
		if (IS_ERR(destroy_hwpt)) {	/* [한국어] 붙지 않았다. */
			iommufd_put_object(idev->ictx, &hwpt->obj);	/* [한국어] 참조를 놓고 */
			/*
			 * -EINVAL means the domain is incompatible with the
			 * device. Other error codes should propagate to
			 * userspace as failure. Success means the domain is
			 * attached.
			 */
			if (PTR_ERR(destroy_hwpt) == -EINVAL)	/* [한국어] 원 주석대로 -EINVAL 만 "이 도메인과 안 맞는다"는 뜻이다. */
				continue;	/* [한국어] 다음 후보를 본다. */
			goto out_unlock;	/* [한국어] 다른 오류는 진짜 실패라 그대로 올린다. */
		}
		*pt_id = hwpt->obj.id;	/* [한국어] 사용자에게 실제로 쓴 도메인을 알린다. */
		iommufd_put_object(idev->ictx, &hwpt->obj);	/* [한국어] 조회용 참조를 놓는다. 붙이기가 자기 몫을 따로 들었다. */
		goto out_unlock;	/* [한국어] 성공. */
	}

	hwpt_paging = iommufd_hwpt_paging_alloc(idev->ictx, ioas, idev, pasid,	/* [한국어] 쓸 만한 것이 없어 새로 만든다. */
						0, immediate_attach, NULL);
	if (IS_ERR(hwpt_paging)) {	/* [한국어] 만들지 못했다. */
		destroy_hwpt = ERR_CAST(hwpt_paging);	/* [한국어] 오류를 옮겨 */
		goto out_unlock;	/* [한국어] 올린다. */
	}
	hwpt = &hwpt_paging->common;	/* [한국어] 공통 부분. */

	if (!immediate_attach) {	/* [한국어] 만드는 중에 붙이지 않았다면 */
		destroy_hwpt = (*do_attach)(idev, pasid, hwpt);	/* [한국어] 여기서 붙인다. */
		if (IS_ERR(destroy_hwpt))	/* [한국어] 실패하면 */
			goto out_abort;	/* [한국어] 아직 공개하지 않은 도메인을 되돌린다. */
	} else {	/* [한국어] 즉시 붙임 방식이었다면 */
		destroy_hwpt = NULL;	/* [한국어] 이미 붙었고 파괴할 옛 것도 없다. */
	}

	hwpt_paging->auto_domain = true;	/* [한국어] 다음번에 이 도메인이 후보로 뽑히게 표시한다. */
	*pt_id = hwpt->obj.id;	/* [한국어] 사용자에게 알릴 id. */

	iommufd_object_finalize(idev->ictx, &hwpt->obj);	/* [한국어] 이제야 공개한다. */
	mutex_unlock(&ioas->mutex);	/* [한국어] 락을 놓고 */
	return destroy_hwpt;	/* [한국어] 호출자가 파괴할 것(있다면)을 돌려준다. */

out_abort:	/* [한국어] 새 도메인에 붙이지 못한 경로. */
	iommufd_object_abort_and_destroy(idev->ictx, &hwpt->obj);	/* [한국어] 공개하지 않았으므로 abort 로 되돌린다. */
out_unlock:	/* [한국어] 모든 경로가 합류한다. */
	mutex_unlock(&ioas->mutex);	/* [한국어] 락을 놓고 */
	return destroy_hwpt;	/* [한국어] 결과(또는 오류 포인터)를 돌려준다. */
}

/*
 * [한국어]
 * iommufd_device_change_pt - 붙이기와 교체가 공유하는 몸통
 *
 * @idev: 대상 장치.
 * @pasid: 대상 PASID.
 * @pt_id: 입력은 IOAS 또는 HWPT 의 id, 출력은 실제로 쓴 HWPT 의 id.
 * @do_attach: 붙이기 함수 또는 교체 함수.
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자가 준 id 가 무엇이냐에 따라 갈린다. 페이지 테이블이면 곧장 그것에
 * 붙이고, IOAS 면 커널이 도메인을 골라 준다.
 *
 * 붙이기와 교체가 이 함수 하나를 함수 포인터로 나눠 쓴다 — 앞뒤 처리가
 * 똑같고 가운데 한 걸음만 다르기 때문이다.
 *
 * 원 주석이 파괴 시점을 못 박는다: 모든 락을 놓은 뒤에 파괴해야 한다.
 * 도메인 파괴가 다시 이 락들을 잡을 수 있어 교착이 생긴다.
 */
static int iommufd_device_change_pt(struct iommufd_device *idev,
				    ioasid_t pasid,
				    u32 *pt_id, attach_fn do_attach)
{
	struct iommufd_hw_pagetable *destroy_hwpt;	/* [한국어] 호출자가 파괴할 옛 도메인. */
	struct iommufd_object *pt_obj;	/* [한국어] 사용자가 준 id 의 객체. */

	pt_obj = iommufd_get_object(idev->ictx, *pt_id, IOMMUFD_OBJ_ANY);	/* [한국어] 종류를 가리지 않고 찾는다 — IOAS 일 수도 도메인일 수도 있다. */
	if (IS_ERR(pt_obj))	/* [한국어] 없는 id 다. */
		return PTR_ERR(pt_obj);	/* [한국어] 오류를 올린다. */

	switch (pt_obj->type) {	/* [한국어] 종류에 따라 갈린다. */
	case IOMMUFD_OBJ_HWPT_NESTED:	/* [한국어] 게스트 페이지 테이블을 쓰는 중첩 도메인. */
	case IOMMUFD_OBJ_HWPT_PAGING: {	/* [한국어] 호스트가 관리하는 페이징 도메인. */
		struct iommufd_hw_pagetable *hwpt =	/* [한국어] 객체를 도메인으로 되짚는다. 값은 다음 줄의 container_of 가 만든다. */
			container_of(pt_obj, struct iommufd_hw_pagetable, obj);	/* [한국어] 공통 도메인 구조로 되짚는다. */

		destroy_hwpt = (*do_attach)(idev, pasid, hwpt);	/* [한국어] 사용자가 지목한 그 도메인에 곧장 붙인다. */
		if (IS_ERR(destroy_hwpt))	/* [한국어] 실패하면 */
			goto out_put_pt_obj;	/* [한국어] 조회 참조를 놓고 나간다. */
		break;	/* [한국어] 성공. */
	}
	case IOMMUFD_OBJ_IOAS: {	/* [한국어] 주소 공간만 지정한 경우. */
		struct iommufd_ioas *ioas =	/* [한국어] 객체를 IOAS 로 되짚는다. */
			container_of(pt_obj, struct iommufd_ioas, obj);	/* [한국어] IOAS 로 되짚는다. */

		destroy_hwpt = iommufd_device_auto_get_domain(idev, pasid, ioas,	/* [한국어] 커널이 도메인을 골라 주거나 새로 만든다. */
							      pt_id, do_attach);
		if (IS_ERR(destroy_hwpt))	/* [한국어] 실패하면 */
			goto out_put_pt_obj;	/* [한국어] 조회 참조를 놓고 나간다. */
		break;	/* [한국어] 성공. */
	}
	default:	/* [한국어] 다른 종류의 객체 id 를 준 경우. */
		destroy_hwpt = ERR_PTR(-EINVAL);	/* [한국어] 붙일 수 있는 대상이 아니다. */
		goto out_put_pt_obj;	/* [한국어] 나간다. */
	}
	iommufd_put_object(idev->ictx, pt_obj);	/* [한국어] 조회용 참조를 놓는다. */

	/* This destruction has to be after we unlock everything */
	if (destroy_hwpt)	/* [한국어] 교체였고 옛 도메인이 남았다면 */
		iommufd_hw_pagetable_put(idev->ictx, destroy_hwpt);	/* [한국어] 원 주석대로 모든 락을 놓은 뒤에 파괴한다 — 파괴가 같은 락들을 다시 잡을 수 있다. */
	return 0;	/* [한국어] 성공. */

out_put_pt_obj:	/* [한국어] 실패 경로. */
	iommufd_put_object(idev->ictx, pt_obj);	/* [한국어] 조회 참조를 놓고 */
	return PTR_ERR(destroy_hwpt);	/* [한국어] 오류 코드를 올린다. */
}

/**
 * iommufd_device_attach - Connect a device/pasid to an iommu_domain
 * @idev: device to attach
 * @pasid: pasid to attach
 * @pt_id: Input a IOMMUFD_OBJ_IOAS, or IOMMUFD_OBJ_HWPT_PAGING
 *         Output the IOMMUFD_OBJ_HWPT_PAGING ID
 *
 * This connects the device/pasid to an iommu_domain, either automatically
 * or manually selected. Once this completes the device could do DMA with
 * @pasid. @pasid is IOMMU_NO_PASID if this attach is for no pasid usage.
 *
 * The caller should return the resulting pt_id back to userspace.
 * This function is undone by calling iommufd_device_detach().
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_device_attach - 장치를 도메인에 붙인다 (드라이버용 API)
 *
 * @idev: 붙을 장치.
 * @pasid: 붙일 PASID. 없으면 IOMMU_NO_PASID.
 * @pt_id: 입력은 IOAS 또는 HWPT id, 출력은 실제 HWPT id.
 * @return: 0 성공, 음수면 실패.
 *
 * 이 함수가 성공하면 그 장치는 그 주소 공간으로 DMA 할 수 있다.
 *
 * 참조를 하나 더 드는 이유를 원 주석이 밝힌다 — 붙어 있는 채로 장치를
 * 파괴하려는 드라이버 버그를 잡기 위해서다. 그런 코드는 참조가 남아
 * 파괴가 진행되지 않는다.
 */
int iommufd_device_attach(struct iommufd_device *idev, ioasid_t pasid,
			  u32 *pt_id)
{
	int rc;	/* [한국어] 결과 코드. */

	rc = iommufd_device_change_pt(idev, pasid, pt_id,	/* [한국어] 붙이기 함수를 넘겨 공통 몸통을 돌린다. */
				      &iommufd_device_do_attach);
	if (rc)	/* [한국어] 실패하면 */
		return rc;	/* [한국어] 그대로 올린다. */

	/*
	 * Pairs with iommufd_device_detach() - catches caller bugs attempting
	 * to destroy a device with an attachment.
	 */
	refcount_inc(&idev->obj.users);	/* [한국어] 원 주석대로 detach 와 짝이 된다. 붙어 있는 채로 장치를 파괴하려는 드라이버 버그를 참조로 막는다. */
	return 0;	/* [한국어] 성공. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_attach, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/**
 * iommufd_device_replace - Change the device/pasid's iommu_domain
 * @idev: device to change
 * @pasid: pasid to change
 * @pt_id: Input a IOMMUFD_OBJ_IOAS, or IOMMUFD_OBJ_HWPT_PAGING
 *         Output the IOMMUFD_OBJ_HWPT_PAGING ID
 *
 * This is the same as::
 *
 *   iommufd_device_detach();
 *   iommufd_device_attach();
 *
 * If it fails then no change is made to the attachment. The iommu driver may
 * implement this so there is no disruption in translation. This can only be
 * called if iommufd_device_attach() has already succeeded. @pasid is
 * IOMMU_NO_PASID for no pasid usage.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_device_replace - 붙어 있는 도메인을 바꾼다 (드라이버용 API)
 *
 * @idev: 대상 장치.
 * @pasid: 대상 PASID.
 * @pt_id: 입력은 새 IOAS 또는 HWPT id, 출력은 실제 HWPT id.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 뗐다 붙인 것과 결과가 같되, 실패하면 아무것도 바뀌지
 * 않는다는 점이 다르다. 드라이버가 지원하면 변환이 끊기는 창도 없다.
 *
 * 참조를 더 들지 않는 데 주의 — 이미 붙어 있던 상태이므로 attach 때 든
 * 참조가 그대로 유효하다.
 */
int iommufd_device_replace(struct iommufd_device *idev, ioasid_t pasid,
			   u32 *pt_id)
{
	return iommufd_device_change_pt(idev, pasid, pt_id,	/* [한국어] 교체 함수를 넘겨 같은 몸통을 돌린다. 참조를 더 들지 않는 것은 이미 붙어 있던 상태이기 때문이다. */
					&iommufd_device_do_replace);
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_replace, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/**
 * iommufd_device_detach - Disconnect a device/device to an iommu_domain
 * @idev: device to detach
 * @pasid: pasid to detach
 *
 * Undo iommufd_device_attach(). This disconnects the idev from the previously
 * attached pt_id. The device returns back to a blocked DMA translation.
 * @pasid is IOMMU_NO_PASID for no pasid usage.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_device_detach - 장치를 도메인에서 뗀다 (드라이버용 API)
 *
 * @idev: 뗄 장치.
 * @pasid: 뗄 PASID.
 *
 * 뗀 뒤 그 장치의 DMA 는 막힌다 — 아무 도메인에도 붙어 있지 않으면
 * 변환할 표가 없다.
 *
 * 붙어 있지 않았으면 참조를 놓지 않고 그냥 돌아간다. attach 가 들었던
 * 참조와 짝이 맞아야 하기 때문이다.
 */
void iommufd_device_detach(struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 떼어 낸 도메인. */

	hwpt = iommufd_hw_pagetable_detach(idev, pasid);	/* [한국어] 실제로 뗀다. */
	if (!hwpt)	/* [한국어] 붙어 있지 않았으면 */
		return;	/* [한국어] attach 가 든 참조도 없으므로 놓지 않는다. */
	refcount_dec(&idev->obj.users);	/* [한국어] attach 가 들었던 참조를 놓는다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_detach, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * On success, it will refcount_inc() at a valid new_ioas and refcount_dec() at
 * a valid cur_ioas (access->ioas). A caller passing in a valid new_ioas should
 * call iommufd_put_object() if it does an iommufd_get_object() for a new_ioas.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_change_ioas - 접근자가 볼 주소 공간을 바꾼다
 *
 * @access: 대상 접근자.
 * @new_ioas: 새 주소 공간. NULL 이면 떼는 것이다.
 * @return: 0 성공, 음수면 실패.
 *
 * 붙이기·교체·떼기가 모두 이 함수 하나로 모인다.
 *
 * ioas 와 ioas_unpin 두 포인터를 두는 것이 이 함수의 핵심이다. ioas 를
 * 먼저 NULL 로 만들어 새 고정을 막되, 이미 고정된 페이지를 놓는 쪽은
 * ioas_unpin 을 계속 쓸 수 있게 한다. 그러지 않으면 놓아야 할 페이지를
 * 어느 IOAS 에 돌려줘야 할지 알 수 없게 된다.
 *
 * unmap 콜백을 락 밖에서 부르는 이유: 드라이버가 그 안에서 다시 이
 * 접근자의 함수를 부를 수 있어, 락을 쥔 채 부르면 교착이 생긴다.
 *
 * 원 주석대로 성공하면 새 IOAS 의 참조가 늘고 옛 것의 참조가 준다.
 */
static int iommufd_access_change_ioas(struct iommufd_access *access,
				      struct iommufd_ioas *new_ioas)
{
	u32 iopt_access_list_id = access->iopt_access_list_id;	/* [한국어] 옛 IOAS 의 접근자 목록에서 이 접근자를 찾는 번호. 아래에서 지울 때 쓰므로 미리 꺼내 둔다. */
	struct iommufd_ioas *cur_ioas = access->ioas;	/* [한국어] 지금 붙어 있는 주소 공간. */
	int rc;	/* [한국어] 결과 코드. */

	lockdep_assert_held(&access->ioas_lock);	/* [한국어] 호출자가 접근자 뮤텍스를 쥐고 있어야 한다. */

	/* We are racing with a concurrent detach, bail */
	if (cur_ioas != access->ioas_unpin)	/* [한국어] 두 포인터가 어긋나 있다 = 다른 스레드가 이미 바꾸는 중이다. */
		return -EBUSY;	/* [한국어] 겹치지 않게 물러난다. */

	if (cur_ioas == new_ioas)	/* [한국어] 바꿀 것이 없다. */
		return 0;	/* [한국어] 이미 원하는 상태다. */

	/*
	 * Set ioas to NULL to block any further iommufd_access_pin_pages().
	 * iommufd_access_unpin_pages() can continue using access->ioas_unpin.
	 */
	access->ioas = NULL;	/* [한국어] 원 주석대로 새 고정을 막는다. 이미 고정된 것을 놓는 쪽은 ioas_unpin 을 계속 쓴다. */

	if (new_ioas) {	/* [한국어] 붙이는 경우(떼기가 아니면) */
		rc = iopt_add_access(&new_ioas->iopt, access);	/* [한국어] 새 IOAS 의 접근자 목록에 넣는다. 매핑이 풀릴 때 알림을 받으려면 여기 있어야 한다. */
		if (rc) {	/* [한국어] 실패하면 */
			access->ioas = cur_ioas;	/* [한국어] 원래대로 되돌리고 */
			return rc;	/* [한국어] 실패를 올린다. */
		}
		refcount_inc(&new_ioas->obj.users);	/* [한국어] 새 IOAS 를 붙잡는다. */
	}

	if (cur_ioas) {	/* [한국어] 옛 IOAS 가 있었으면 정리한다. */
		if (!iommufd_access_is_internal(access) && access->ops->unmap) {	/* [한국어] 드라이버 접근자이고 알림 콜백이 있으면 */
			mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 부른다 — 드라이버가 그 안에서 이 접근자의 함수를 다시 부를 수 있다. */
			access->ops->unmap(access->data, 0, ULONG_MAX);	/* [한국어] 주소 공간 전체가 사라진다고 알려 고정을 모두 놓게 한다. */
			mutex_lock(&access->ioas_lock);	/* [한국어] 다시 잡는다. */
		}
		iopt_remove_access(&cur_ioas->iopt, access, iopt_access_list_id);	/* [한국어] 옛 IOAS 의 접근자 목록에서 뺀다. */
		refcount_dec(&cur_ioas->obj.users);	/* [한국어] 옛 IOAS 참조를 놓는다. */
	}

	access->ioas = new_ioas;	/* [한국어] 새 주소 공간을 세운다. */
	access->ioas_unpin = new_ioas;	/* [한국어] 이제 두 포인터가 다시 같아진다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iommufd_access_change_ioas_id - id 로 지정해 주소 공간을 바꾼다
 *
 * @access: 대상 접근자.
 * @id: 새 IOAS 의 객체 id.
 * @return: 0 성공, 음수면 실패.
 *
 * id 를 실제 객체로 바꿔 위 함수에 넘긴다. 조회하며 든 참조는 여기서
 * 놓는다 — 성공했다면 change_ioas 가 자기 몫의 참조를 따로 들었다.
 */
static int iommufd_access_change_ioas_id(struct iommufd_access *access, u32 id)
{
	struct iommufd_ioas *ioas = iommufd_get_ioas(access->ictx, id);	/* [한국어] id 가 실제로 IOAS 인지 확인하며 참조를 든다. */
	int rc;	/* [한국어] 결과 코드. */

	if (IS_ERR(ioas))	/* [한국어] 없는 id 다. */
		return PTR_ERR(ioas);	/* [한국어] 오류를 올린다. */
	rc = iommufd_access_change_ioas(access, ioas);	/* [한국어] 실제 교체. */
	iommufd_put_object(access->ictx, &ioas->obj);	/* [한국어] 조회용 참조를 놓는다. 성공했다면 change_ioas 가 자기 몫을 따로 들었다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_access_destroy_object - 접근자 객체를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * 아직 IOAS 에 붙어 있으면 먼저 뗀다. 그 과정에서 드라이버의 unmap
 * 콜백이 불려 고정해 둔 페이지를 놓게 된다.
 *
 * 내부 접근자는 문맥 참조를 들지 않았으므로 놓지도 않는다.
 */
void iommufd_access_destroy_object(struct iommufd_object *obj)
{
	struct iommufd_access *access =	/* [한국어] 객체를 접근자로 되짚는다. */
		container_of(obj, struct iommufd_access, obj);	/* [한국어] 객체에서 접근자를 되짚는다. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (access->ioas)	/* [한국어] 아직 붙어 있으면 */
		WARN_ON(iommufd_access_change_ioas(access, NULL));	/* [한국어] 뗀다. 이 시점에 실패할 이유가 없어 실패하면 버그다. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓는다. */
	if (!iommufd_access_is_internal(access))	/* [한국어] 내부 접근자는 문맥 참조를 들지 않았다. */
		iommufd_ctx_put(access->ictx);	/* [한국어] 문맥 참조를 놓는다. */
}

/*
 * [한국어]
 * __iommufd_access_create - 접근자의 공통 부분을 만든다
 *
 * @ictx: 문맥.
 * @return: 만들어진 접근자, 실패하면 오류 포인터.
 *
 * 원 주석대로 접근자에는 사용자 ABI 가 없다 — 사용자가 만들 수 있는
 * 물건이 아니라 커널 드라이버가 쓰는 것이다. 그래도 객체 틀을 쓰는 것은
 * 참조 관리와 수명 규칙을 그대로 물려받기 위해서다.
 *
 * 아직 finalize 하지 않는다. 두 갈래 생성 함수가 각자 필요한 설정을
 * 마친 뒤 공개한다.
 */
static struct iommufd_access *__iommufd_access_create(struct iommufd_ctx *ictx)
{
	struct iommufd_access *access;	/* [한국어] 만들 접근자. */

	/*
	 * There is no uAPI for the access object, but to keep things symmetric
	 * use the object infrastructure anyhow.
	 */
	access = iommufd_object_alloc(ictx, access, IOMMUFD_OBJ_ACCESS);	/* [한국어] 원 주석대로 사용자 ABI 가 없는데도 객체 틀을 쓴다 — 참조 관리와 수명 규칙을 그대로 물려받으려는 것이다. */
	if (IS_ERR(access))	/* [한국어] 메모리가 없다. */
		return access;	/* [한국어] 오류 포인터를 그대로 올린다. */

	/* The calling driver is a user until iommufd_access_destroy() */
	refcount_inc(&access->obj.users);	/* [한국어] 원 주석대로 부른 드라이버가 destroy 할 때까지 이 참조를 든다. */
	mutex_init(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지킬 뮤텍스. */
	return access;	/* [한국어] 아직 공개하지 않은 접근자. */
}

/*
 * [한국어]
 * iommufd_access_create_internal - 커널 내부용 접근자를 만든다
 *
 * @ictx: 문맥.
 * @return: 만들어진 접근자, 실패하면 오류 포인터.
 *
 * 드라이버가 아니라 iommufd 자신이 쓰는 접근자다. ops 도 data 도 없어
 * unmap 알림을 받지 않는다.
 *
 * 정렬을 PAGE_SIZE 로 잡는 이유: 내부 사용자는 페이지 단위로만 다루므로
 * 그보다 잘게 허용할 이유가 없다.
 */
struct iommufd_access *iommufd_access_create_internal(struct iommufd_ctx *ictx)
{
	struct iommufd_access *access;	/* [한국어] 만들 접근자. */

	access = __iommufd_access_create(ictx);	/* [한국어] 공통 부분을 만든다. */
	if (IS_ERR(access))	/* [한국어] 실패하면 */
		return access;	/* [한국어] 그대로 올린다. */
	access->iova_alignment = PAGE_SIZE;	/* [한국어] 내부 사용자는 페이지 단위로만 다루므로 그보다 잘게 허용할 이유가 없다. */

	iommufd_object_finalize(ictx, &access->obj);	/* [한국어] 공개한다. ops 도 data 도 없어 더 세울 것이 없다. */
	return access;	/* [한국어] 만들어진 접근자. */
}

/**
 * iommufd_access_create - Create an iommufd_access
 * @ictx: iommufd file descriptor
 * @ops: Driver's ops to associate with the access
 * @data: Opaque data to pass into ops functions
 * @id: Output ID number to return to userspace for this access
 *
 * An iommufd_access allows a driver to read/write to the IOAS without using
 * DMA. The underlying CPU memory can be accessed using the
 * iommufd_access_pin_pages() or iommufd_access_rw() functions.
 *
 * The provided ops are required to use iommufd_access_pin_pages().
 */
struct iommufd_access *	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_create - 드라이버용 접근자를 만든다
 *
 * @ictx: 문맥.
 * @ops: 드라이버의 콜백들.
 * @data: 콜백에 그대로 넘겨 줄 값.
 * @id: 사용자에게 알릴 객체 id.
 * @return: 만들어진 접근자, 실패하면 오류 포인터.
 *
 * 원 주석대로 접근자는 DMA 없이 IOAS 를 읽고 쓰는 길이다. vfio 의
 * 에뮬레이션 장치가 게스트 메모리를 들여다볼 때 쓴다.
 *
 * 정렬이 두 갈래인 것이 요점이다. 페이지를 고정해 쓰는 드라이버는 페이지
 * 단위로 맞춰야 하지만, 그냥 읽고 쓰기만 하는 드라이버는 바이트 단위로도
 * 된다. 정렬이 느슨하면 사용자가 더 자유롭게 매핑을 짤 수 있다.
 */
iommufd_access_create(struct iommufd_ctx *ictx,
		      const struct iommufd_access_ops *ops, void *data, u32 *id)
{
	struct iommufd_access *access;	/* [한국어] 만들 접근자. */

	access = __iommufd_access_create(ictx);	/* [한국어] 공통 부분을 만든다. */
	if (IS_ERR(access))	/* [한국어] 실패하면 */
		return access;	/* [한국어] 그대로 올린다. */

	access->data = data;	/* [한국어] 콜백에 그대로 넘겨 줄 값. */
	access->ops = ops;	/* [한국어] 드라이버의 콜백들. */

	if (ops->needs_pin_pages)	/* [한국어] 페이지를 고정해 쓰는 드라이버면 */
		access->iova_alignment = PAGE_SIZE;	/* [한국어] 페이지 경계에 맞춰야 struct page 를 돌려줄 수 있다. */
	else
		access->iova_alignment = 1;	/* [한국어] 읽고 쓰기만 하는 드라이버는 바이트 단위로도 된다. 느슨한 정렬이 사용자에게 더 자유를 준다. */

	access->ictx = ictx;	/* [한국어] 문맥을 기억한다. */
	iommufd_ctx_get(ictx);	/* [한국어] 접근자가 살아 있는 동안 문맥이 사라지지 않게 한다. */
	iommufd_object_finalize(ictx, &access->obj);	/* [한국어] 공개한다. */
	*id = access->obj.id;	/* [한국어] 사용자에게 알릴 id. */
	return access;	/* [한국어] 만들어진 접근자. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_create, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/**
 * iommufd_access_destroy - Destroy an iommufd_access
 * @access: The access to destroy
 *
 * The caller must stop using the access before destroying it.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_destroy - 접근자를 없앤다
 *
 * @access: 없앨 접근자.
 *
 * 원 주석대로 없애기 전에 그것을 쓰는 일을 모두 멈춰야 한다.
 */
void iommufd_access_destroy(struct iommufd_access *access)
{
	iommufd_object_destroy_user(access->ictx, &access->obj);	/* [한국어] 사용자 쪽 파괴와 겹치지 않게 조율하며 없앤다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_destroy, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * [한국어]
 * iommufd_access_detach - 접근자를 주소 공간에서 뗀다
 *
 * @access: 대상 접근자.
 *
 * 뗀 뒤에는 pin_pages 나 rw 가 -ENOENT 로 실패한다.
 *
 * 붙어 있지 않은데 부르는 것은 드라이버 버그라 WARN 을 남긴다.
 */
void iommufd_access_detach(struct iommufd_access *access)
{
	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (WARN_ON(!access->ioas)) {	/* [한국어] 붙어 있지 않은데 떼라는 것은 드라이버 버그다. */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return;	/* [한국어] 아무것도 하지 않는다. */
	}
	WARN_ON(iommufd_access_change_ioas(access, NULL));	/* [한국어] NULL 로 바꾸는 것이 곧 떼기다. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓는다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_detach, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * [한국어]
 * iommufd_access_attach - 접근자를 주소 공간에 붙인다
 *
 * @access: 대상 접근자.
 * @ioas_id: 붙일 IOAS 의 객체 id.
 * @return: 0 성공, 음수면 실패.
 *
 * 이미 붙어 있는데 또 부르는 것은 드라이버 버그다 — 바꾸려면 replace 를
 * 써야 뗐다 붙이는 사이의 창이 생기지 않는다.
 */
int iommufd_access_attach(struct iommufd_access *access, u32 ioas_id)
{
	int rc;	/* [한국어] 결과 코드. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (WARN_ON(access->ioas)) {	/* [한국어] 이미 붙어 있는데 또 붙이라는 것은 드라이버 버그다. */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return -EINVAL;	/* [한국어] 거절한다. 바꾸려면 replace 를 써야 창이 생기지 않는다. */
	}

	rc = iommufd_access_change_ioas_id(access, ioas_id);	/* [한국어] id 로 지정해 붙인다. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
	return rc;	/* [한국어] 결과를 올린다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_attach, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. attach_internal 은 커널 안에서만 쓰여 내보내지 않는다. */

/*
 * [한국어]
 * iommufd_access_attach_internal - 내부 접근자를 주소 공간에 붙인다
 *
 * @access: 대상 접근자.
 * @ioas: 붙일 주소 공간(id 가 아니라 포인터).
 * @return: 0 성공, 음수면 실패.
 *
 * 커널 안에서 부르므로 이미 객체 포인터를 들고 있다. id 로 다시 찾을
 * 이유가 없어 갈라 두었다.
 */
int iommufd_access_attach_internal(struct iommufd_access *access,
				   struct iommufd_ioas *ioas)
{
	int rc;	/* [한국어] 결과 코드. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (WARN_ON(access->ioas)) {	/* [한국어] 이미 붙어 있으면 버그다. */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return -EINVAL;	/* [한국어] 거절한다. */
	}

	rc = iommufd_access_change_ioas(access, ioas);	/* [한국어] 포인터를 직접 넘긴다 — 커널 안에서 부르므로 id 로 다시 찾을 이유가 없다. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
	return rc;	/* [한국어] 결과를 올린다. */
}

/*
 * [한국어]
 * iommufd_access_replace - 접근자가 볼 주소 공간을 바꾼다
 *
 * @access: 대상 접근자.
 * @ioas_id: 새 IOAS 의 객체 id.
 * @return: 0 성공, 붙어 있지 않았으면 -ENOENT.
 *
 * attach 와 달리 이미 붙어 있어야 한다. 붙어 있지 않은데 바꾸라는 것은
 * 뜻이 통하지 않는다.
 */
int iommufd_access_replace(struct iommufd_access *access, u32 ioas_id)
{
	int rc;	/* [한국어] 결과 코드. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (!access->ioas) {	/* [한국어] 붙어 있지 않으면 */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return -ENOENT;	/* [한국어] 바꿀 것이 없다. attach 와 달리 WARN 을 남기지 않는 것은 경합으로 일어날 수 있어서다. */
	}
	rc = iommufd_access_change_ioas_id(access, ioas_id);	/* [한국어] 새 IOAS 로 바꾼다. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
	return rc;	/* [한국어] 결과를 올린다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_replace, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/**
 * iommufd_access_notify_unmap - Notify users of an iopt to stop using it
 * @iopt: iopt to work on
 * @iova: Starting iova in the iopt
 * @length: Number of bytes
 *
 * After this function returns there should be no users attached to the pages
 * linked to this iopt that intersect with iova,length. Anyone that has attached
 * a user through iopt_access_pages() needs to detach it through
 * iommufd_access_unpin_pages() before this function returns.
 *
 * iommufd_access_destroy() will wait for any outstanding unmap callback to
 * complete. Once iommufd_access_destroy() no unmap ops are running or will
 * run in the future. Due to this a driver must not create locking that prevents
 * unmap to complete while iommufd_access_destroy() is running.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_notify_unmap - 매핑이 풀린다고 접근자들에게 알린다
 *
 * @iopt: 대상 페이지 테이블 관리 구조.
 * @iova: 풀리는 구간의 시작.
 * @length: 그 길이.
 *
 * 매핑을 풀기 전에 그 페이지를 붙잡고 있는 접근자들을 놓게 해야 한다.
 * 원 주석대로 이 함수가 돌아온 뒤에는 그 구간을 붙잡은 사용자가 없어야
 * 한다.
 *
 * 원 주석이 드라이버에 거는 조건이 중요하다 — unmap 이 끝나는 것을
 * 막는 락을 만들면 안 된다. 파괴가 unmap 을 기다리므로, 그 반대 방향
 * 의존이 생기면 교착이다.
 *
 * 락을 놓았다 다시 잡으며 도는 이유: 콜백이 잠들 수 있다. 참조를 든 채
 * 놓으므로 그 접근자가 사라지지는 않지만, 목록 자체는 바뀔 수 있어
 * 다시 처음부터 훑는다.
 *
 * 내부 접근자는 건너뛴다 — ops 가 없어 알릴 상대가 없다.
 */
void iommufd_access_notify_unmap(struct io_pagetable *iopt, unsigned long iova,
				 unsigned long length)
{
	struct iommufd_ioas *ioas =	/* [한국어] 페이지 테이블 관리 구조에서 IOAS 를 되짚는다. */
		container_of(iopt, struct iommufd_ioas, iopt);	/* [한국어] 페이지 테이블 관리 구조에서 IOAS 를 되짚는다. */
	struct iommufd_access *access;	/* [한국어] 알릴 접근자. */
	unsigned long index;	/* [한국어] xarray 순회용 첨자. */

	xa_lock(&ioas->iopt.access_list);	/* [한국어] 접근자 목록을 지키는 락. */
	xa_for_each(&ioas->iopt.access_list, index, access) {	/* [한국어] 이 IOAS 를 보는 모든 접근자. */
		if (!iommufd_lock_obj(&access->obj) ||	/* [한국어] 파괴 중인 접근자는 건너뛴다. */
		    iommufd_access_is_internal(access))	/* [한국어] 내부 접근자는 ops 가 없어 알릴 상대가 없다. */
			continue;	/* [한국어] 다음으로. */
		xa_unlock(&ioas->iopt.access_list);	/* [한국어] 콜백이 잠들 수 있어 락을 놓는다. */

		access->ops->unmap(access->data, iova, length);	/* [한국어] 드라이버에게 그 구간을 놓으라고 알린다. 돌아올 때까지 기다린다. */

		iommufd_put_object(access->ictx, &access->obj);	/* [한국어] 들었던 참조를 놓는다. */
		xa_lock(&ioas->iopt.access_list);	/* [한국어] 다시 잡고 이어서 훑는다. 목록이 바뀌었을 수 있지만 첨자 기준 순회라 이어 갈 수 있다. */
	}
	xa_unlock(&ioas->iopt.access_list);	/* [한국어] 모두 알렸다. */
}

/**
 * iommufd_access_unpin_pages() - Undo iommufd_access_pin_pages
 * @access: IOAS access to act on
 * @iova: Starting IOVA
 * @length: Number of bytes to access
 *
 * Return the struct page's. The caller must stop accessing them before calling
 * this. The iova/length must exactly match the one provided to access_pages.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_unpin_pages - 고정해 둔 페이지를 놓는다
 *
 * @access: 대상 접근자.
 * @iova: 고정할 때 준 시작 IOVA.
 * @length: 그때 준 길이.
 *
 * 원 주석대로 고정할 때와 정확히 같은 구간을 주어야 한다. 참조를 영역
 * 단위로 세어 두어서, 일부만 놓는 것은 지원하지 않는다.
 *
 * ioas 가 아니라 ioas_unpin 을 쓰는 것이 요점이다. 그 사이 접근자가
 * 다른 IOAS 로 옮겨 갔더라도, 고정은 옛 IOAS 에 걸려 있으므로 그쪽에
 * 돌려줘야 한다.
 */
void iommufd_access_unpin_pages(struct iommufd_access *access,
				unsigned long iova, unsigned long length)
{
	bool internal = iommufd_access_is_internal(access);	/* [한국어] 내부 접근자와 드라이버 접근자는 참조를 따로 센다. */
	struct iopt_area_contig_iter iter;	/* [한국어] 이어진 영역들을 훑는 반복자. */
	struct io_pagetable *iopt;	/* [한국어] 대상 페이지 테이블 관리 구조. */
	unsigned long last_iova;	/* [한국어] 구간의 마지막 주소. */
	struct iopt_area *area;	/* [한국어] 지금 보는 영역. */

	if (WARN_ON(!length) ||	/* [한국어] 길이 0 은 뜻이 없다. */
	    WARN_ON(check_add_overflow(iova, length - 1, &last_iova)))	/* [한국어] 주소가 넘치면 계산이 성립하지 않는다. */
		return;	/* [한국어] 드라이버 버그이므로 아무것도 하지 않는다. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	/*
	 * The driver must be doing something wrong if it calls this before an
	 * iommufd_access_attach() or after an iommufd_access_detach().
	 */
	if (WARN_ON(!access->ioas_unpin)) {	/* [한국어] 원 주석대로 attach 전이나 detach 후에 부르는 것은 드라이버 버그다. */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return;	/* [한국어] 아무것도 하지 않는다. */
	}
	iopt = &access->ioas_unpin->iopt;	/* [한국어] ioas 가 아니라 ioas_unpin 을 쓴다 — 그 사이 다른 IOAS 로 옮겨 갔더라도 고정은 옛 쪽에 걸려 있다. */

	down_read(&iopt->iova_rwsem);	/* [한국어] 영역 트리를 읽는 동안 바뀌지 않게 한다. */
	iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova)	/* [한국어] 구간이 걸친 영역들을 차례로 훑는다. */
		iopt_area_remove_access(	/* [한국어] 영역마다 그 부분의 고정을 놓는다. */
			area, iopt_area_iova_to_index(area, iter.cur_iova),	/* [한국어] IOVA 를 그 영역 안의 페이지 번호로 바꾼다. */
			iopt_area_iova_to_index(
				area,
				min(last_iova, iopt_area_last_iova(area))),	/* [한국어] 영역의 끝과 구간의 끝 중 앞선 쪽까지. */
			internal);
	WARN_ON(!iopt_area_contig_done(&iter));	/* [한국어] 구간 전체를 덮지 못했다면 고정할 때와 다른 범위를 준 것이다 — 드라이버 버그. */
	up_read(&iopt->iova_rwsem);	/* [한국어] 읽기 락 해제. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 뮤텍스 해제. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_unpin_pages, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * [한국어]
 * iopt_area_contig_is_aligned - 이 영역 조각이 페이지 경계에 맞는가
 *
 * @iter: 지금 보고 있는 조각.
 * @return: 맞으면 참.
 *
 * struct page 를 돌려주려면 조각의 시작이 페이지 경계여야 한다. IOMMU
 * 페이지가 CPU 페이지보다 작을 수 있어 저절로 맞지는 않는다.
 *
 * 끝도 확인하는데, 마지막 조각은 예외다. 중간 조각의 끝이 페이지
 * 한가운데면 다음 조각이 같은 페이지의 나머지를 가리키게 되어, 페이지
 * 목록으로 표현할 수 없다. 마지막 조각은 뒤가 없으므로 상관없다.
 */
static bool iopt_area_contig_is_aligned(struct iopt_area_contig_iter *iter)
{
	if (iopt_area_start_byte(iter->area, iter->cur_iova) % PAGE_SIZE)	/* [한국어] 이 조각의 시작이 페이지 한가운데면 struct page 로 표현할 수 없다. */
		return false;	/* [한국어] 정렬되지 않았다. */

	if (!iopt_area_contig_done(iter) &&	/* [한국어] 마지막 조각이 아니라면 끝도 봐야 한다. */
	    (iopt_area_start_byte(iter->area, iopt_area_last_iova(iter->area)) %	/* [한국어] 이 영역의 마지막 바이트가 페이지 안 어디인지 계산한다. */
	     PAGE_SIZE) != (PAGE_SIZE - 1))	/* [한국어] 페이지의 마지막 바이트가 아니면, 다음 조각이 같은 페이지의 나머지를 가리키게 되어 목록으로 표현할 수 없다. */
		return false;	/* [한국어] 정렬되지 않았다. */
	return true;	/* [한국어] 페이지 경계에 맞는다. */
}

/*
 * [한국어]
 * check_area_prot - 이 영역이 요청한 접근을 허락하는가
 *
 * @area: 대상 영역.
 * @flags: IOMMUFD_ACCESS_RW_* 조합.
 * @return: 허락하면 참.
 *
 * 쓰기를 요구하면 쓰기 권한이, 아니면 읽기 권한이 있어야 한다.
 *
 * 이 검사가 없으면 커널이 사용자가 읽기 전용으로 매핑한 메모리에 쓰게
 * 되어, 사용자가 세운 보호를 우회하게 된다.
 */
static bool check_area_prot(struct iopt_area *area, unsigned int flags)
{
	if (flags & IOMMUFD_ACCESS_RW_WRITE)	/* [한국어] 쓰기 요청이면 */
		return area->iommu_prot & IOMMU_WRITE;	/* [한국어] 쓰기 권한이 있어야 한다. 없으면 사용자가 세운 보호를 커널이 우회하는 셈이 된다. */
	return area->iommu_prot & IOMMU_READ;	/* [한국어] 읽기 요청이면 읽기 권한을 본다. */
}

/**
 * iommufd_access_pin_pages() - Return a list of pages under the iova
 * @access: IOAS access to act on
 * @iova: Starting IOVA
 * @length: Number of bytes to access
 * @out_pages: Output page list
 * @flags: IOPMMUFD_ACCESS_RW_* flags
 *
 * Reads @length bytes starting at iova and returns the struct page * pointers.
 * These can be kmap'd by the caller for CPU access.
 *
 * The caller must perform iommufd_access_unpin_pages() when done to balance
 * this.
 *
 * This API always requires a page aligned iova. This happens naturally if the
 * ioas alignment is >= PAGE_SIZE and the iova is PAGE_SIZE aligned. However
 * smaller alignments have corner cases where this API can fail on otherwise
 * aligned iova.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_pin_pages - IOVA 구간의 페이지를 고정해 목록으로 준다
 *
 * @access: 대상 접근자.
 * @iova: 시작 IOVA.
 * @length: 길이.
 * @out_pages: 페이지 포인터를 채워 넣을 배열.
 * @flags: IOMMUFD_ACCESS_RW_* 조합.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 돌려받은 페이지는 kmap 해서 CPU 로 읽고 쓸 수 있다.
 * 고정돼 있으므로 그 사이 매핑이 풀려도 페이지가 사라지지 않는다.
 *
 * 원 주석이 밝히는 걸림돌: 이 API 는 늘 페이지 정렬된 IOVA 를 요구한다.
 * IOAS 정렬이 페이지 이상이면 저절로 맞지만, 더 잘면 정렬된 IOVA 인데도
 * 실패하는 구석이 생긴다 — 영역 경계가 페이지 한가운데일 수 있다.
 *
 * 구간이 여러 영역에 걸치면 영역마다 나눠 처리하고, 중간에 실패하면
 * 그때까지 고정한 것을 모두 놓는다.
 *
 * prevent_access 는 지금 풀리는 중인 영역이라는 표시다. 그런 영역은
 * 새로 고정해 주면 안 된다.
 */
int iommufd_access_pin_pages(struct iommufd_access *access, unsigned long iova,
			     unsigned long length, struct page **out_pages,
			     unsigned int flags)
{
	bool internal = iommufd_access_is_internal(access);	/* [한국어] 참조를 어느 쪽으로 셀지 가른다. */
	struct iopt_area_contig_iter iter;	/* [한국어] 이어진 영역 반복자. */
	struct io_pagetable *iopt;	/* [한국어] 대상 페이지 테이블 관리 구조. */
	unsigned long last_iova;	/* [한국어] 구간의 마지막 주소. */
	struct iopt_area *area;	/* [한국어] 지금 보는 영역. */
	int rc;	/* [한국어] 결과 코드. */

	/* Driver's ops don't support pin_pages */
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&	/* [한국어] 테스트 빌드에서만 하는 확인이다 — 늘 하기에는 비용이 아깝고, 어차피 드라이버 버그를 잡는 용도다. */
	    WARN_ON(access->iova_alignment != PAGE_SIZE ||	/* [한국어] 고정을 쓰려면 페이지 정렬로 만들어졌어야 한다. */
		    (!internal && !access->ops->unmap)))	/* [한국어] 드라이버 접근자는 unmap 콜백이 있어야 한다 — 매핑이 풀릴 때 놓을 방법이 없으면 안 된다. */
		return -EINVAL;	/* [한국어] 잘못 쓴 것이다. */

	if (!length)	/* [한국어] 길이 0 은 뜻이 없다. */
		return -EINVAL;	/* [한국어] 거절. */
	if (check_add_overflow(iova, length - 1, &last_iova))	/* [한국어] 주소가 넘친다. */
		return -EOVERFLOW;	/* [한국어] 거절. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (!access->ioas) {	/* [한국어] 붙어 있지 않거나 지금 바뀌는 중이다. */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return -ENOENT;	/* [한국어] 고정할 곳이 없다. */
	}
	iopt = &access->ioas->iopt;	/* [한국어] 대상 페이지 테이블. */

	down_read(&iopt->iova_rwsem);	/* [한국어] 영역 트리를 읽는 동안 바뀌지 않게 한다. */
	iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova) {	/* [한국어] 구간이 걸친 영역들을 차례로. */
		unsigned long last = min(last_iova, iopt_area_last_iova(area));	/* [한국어] 이 영역에서 다룰 마지막 주소. */
		unsigned long last_index = iopt_area_iova_to_index(area, last);	/* [한국어] 그것의 페이지 번호. */
		unsigned long index =	/* [한국어] 이 영역 안에서 시작 IOVA 가 몇 번째 페이지인지. */
			iopt_area_iova_to_index(area, iter.cur_iova);	/* [한국어] 시작의 페이지 번호. */

		if (area->prevent_access ||	/* [한국어] 지금 풀리는 중인 영역이면 새로 고정해 주면 안 된다. */
		    !iopt_area_contig_is_aligned(&iter)) {	/* [한국어] 페이지 경계에 맞지 않으면 목록으로 표현할 수 없다. */
			rc = -EINVAL;	/* [한국어] 거절. */
			goto err_remove;	/* [한국어] 앞서 고정한 것들을 놓는다. */
		}

		if (!check_area_prot(area, flags)) {	/* [한국어] 권한이 모자라면 */
			rc = -EPERM;	/* [한국어] 거절. */
			goto err_remove;	/* [한국어] 앞서 고정한 것들을 놓는다. */
		}

		rc = iopt_area_add_access(area, index, last_index, out_pages,	/* [한국어] 이 영역의 페이지들을 고정하고 배열에 채운다. */
					  flags, internal);
		if (rc)	/* [한국어] 고정에 실패했다. */
			goto err_remove;	/* [한국어] 실패하면 되돌린다. */
		out_pages += last_index - index + 1;	/* [한국어] 채운 만큼 배열 자리를 옮긴다. */
	}
	if (!iopt_area_contig_done(&iter)) {	/* [한국어] 구간 한가운데에 매핑이 없는 구멍이 있었다. */
		rc = -ENOENT;	/* [한국어] 거절. */
		goto err_remove;	/* [한국어] 앞서 고정한 것들을 놓는다. */
	}

	up_read(&iopt->iova_rwsem);	/* [한국어] 읽기 락 해제. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 뮤텍스 해제. */
	return 0;	/* [한국어] 성공. */

err_remove:	/* [한국어] 실패 경로. */
	if (iova < iter.cur_iova) {	/* [한국어] 한 조각이라도 고정했다면 */
		last_iova = iter.cur_iova - 1;	/* [한국어] 성공한 데까지를 되돌릴 범위로 삼는다. */
		iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova)	/* [한국어] 그 범위를 다시 훑으며 */
			iopt_area_remove_access(	/* [한국어] 고정을 모두 놓는다. */
				area,
				iopt_area_iova_to_index(area, iter.cur_iova),
				iopt_area_iova_to_index(
					area, min(last_iova,
						  iopt_area_last_iova(area))),
				internal);
	}
	up_read(&iopt->iova_rwsem);	/* [한국어] 읽기 락 해제. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 뮤텍스 해제. */
	return rc;	/* [한국어] 실패를 올린다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_pin_pages, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/**
 * iommufd_access_rw - Read or write data under the iova
 * @access: IOAS access to act on
 * @iova: Starting IOVA
 * @data: Kernel buffer to copy to/from
 * @length: Number of bytes to access
 * @flags: IOMMUFD_ACCESS_RW_* flags
 *
 * Copy kernel to/from data into the range given by IOVA/length. If flags
 * indicates IOMMUFD_ACCESS_RW_KTHREAD then a large copy can be optimized
 * by changing it into copy_to/from_user().
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_rw - IOVA 구간을 커널 버퍼와 주고받는다
 *
 * @access: 대상 접근자.
 * @iova: 시작 IOVA.
 * @data: 커널 버퍼.
 * @length: 길이.
 * @flags: IOMMUFD_ACCESS_RW_* 조합.
 * @return: 0 성공, 음수면 실패.
 *
 * 고정하지 않고 그때그때 페이지를 찾아 복사한다. 짧은 접근에 알맞다.
 *
 * 원 주석대로 KTHREAD 플래그를 주면 큰 복사를 copy_to/from_user 로
 * 바꿔 처리할 수 있다 — 커널 스레드가 사용자 주소 공간을 빌려 쓰는
 * 방식이라 페이지를 하나씩 매핑하지 않아도 된다.
 *
 * 구조는 pin_pages 와 같지만 되돌릴 것이 없어 실패 처리가 간단하다.
 */
int iommufd_access_rw(struct iommufd_access *access, unsigned long iova,
		      void *data, size_t length, unsigned int flags)
{
	struct iopt_area_contig_iter iter;	/* [한국어] 이어진 영역 반복자. */
	struct io_pagetable *iopt;	/* [한국어] 대상 페이지 테이블 관리 구조. */
	struct iopt_area *area;	/* [한국어] 지금 보는 영역. */
	unsigned long last_iova;	/* [한국어] 구간의 마지막 주소. */
	int rc = -EINVAL;	/* [한국어] 영역을 하나도 만나지 못했을 때의 기본값. */

	if (!length)	/* [한국어] 길이 0 은 뜻이 없다. */
		return -EINVAL;	/* [한국어] 거절. */
	if (check_add_overflow(iova, length - 1, &last_iova))	/* [한국어] 주소가 넘친다. */
		return -EOVERFLOW;	/* [한국어] 거절. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (!access->ioas) {	/* [한국어] 붙어 있지 않다. */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return -ENOENT;	/* [한국어] 읽고 쓸 곳이 없다. */
	}
	iopt = &access->ioas->iopt;	/* [한국어] 대상 페이지 테이블. */

	down_read(&iopt->iova_rwsem);	/* [한국어] 영역 트리를 읽는 동안 바뀌지 않게 한다. */
	iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova) {	/* [한국어] 구간이 걸친 영역들을 차례로. */
		unsigned long last = min(last_iova, iopt_area_last_iova(area));	/* [한국어] 이 영역에서 다룰 마지막 주소. */
		unsigned long bytes = (last - iter.cur_iova) + 1;	/* [한국어] 이번에 옮길 바이트 수. */

		if (area->prevent_access) {	/* [한국어] 풀리는 중인 영역은 건드리지 않는다. */
			rc = -EINVAL;	/* [한국어] 거절. */
			goto err_out;	/* [한국어] 정리하고 나간다. */
		}

		if (!check_area_prot(area, flags)) {	/* [한국어] 권한이 모자라면 */
			rc = -EPERM;	/* [한국어] 거절. */
			goto err_out;	/* [한국어] 정리하고 나간다. */
		}

		rc = iopt_pages_rw_access(	/* [한국어] 이 영역의 해당 부분을 커널 버퍼와 주고받는다. */
			area->pages, iopt_area_start_byte(area, iter.cur_iova),	/* [한국어] IOVA 를 그 영역이 가리키는 사용자 메모리 안의 오프셋으로 바꾼다. */
			data, bytes, flags);
		if (rc)	/* [한국어] 복사에 실패했다. */
			goto err_out;	/* [한국어] 실패하면 나간다. 이미 옮긴 부분은 되돌리지 않는다 — 복사는 되돌릴 수 있는 연산이 아니다. */
		data += bytes;	/* [한국어] 커널 버퍼 쪽 자리를 옮긴다. */
	}
	if (!iopt_area_contig_done(&iter))	/* [한국어] 구간 한가운데에 구멍이 있었다. */
		rc = -ENOENT;	/* [한국어] 일부만 옮겼음을 알린다. */
err_out:	/* [한국어] 성공과 실패가 함께 지나는 정리 지점. */
	up_read(&iopt->iova_rwsem);	/* [한국어] 읽기 락 해제. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 뮤텍스 해제. */
	return rc;	/* [한국어] 결과. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_rw, "IOMMUFD");	/* [한국어] 같은 네임스페이스로 내보낸다. */

/*
 * [한국어]
 * iommufd_get_hw_info - IOMMU_DEVICE_GET_HW_INFO 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자에게 이 장치의 IOMMU 가 어떤 물건인지 알려 준다. 게스트에게
 * 중첩 변환을 시키려면 그 하드웨어의 형식을 알아야 하기 때문이다.
 *
 * 길이 처리가 이 함수의 요점이다. 사용자 버퍼가 작으면 잘라 담고, 크면
 * 남는 부분을 0 으로 채운 뒤, 커널이 아는 전체 길이를 알려 준다.
 * 원 주석대로 그래야 사용자가 커널의 능력을 알 수 있다.
 *
 * 드라이버에 hw_info 가 없으면 형식을 NONE 으로 답한다 — 오류가 아니라
 * "이 하드웨어는 알려 줄 것이 없다"는 뜻이다.
 *
 * PASID 능력을 dev->iommu->max_pasids 로 판정하는 근거를 원 주석이
 * 밝힌다: 지금 모든 드라이버가 probe 때 PASID 를 켜므로, 그 값이 0 이
 * 아니라는 것은 지원과 활성화를 함께 뜻한다.
 */
int iommufd_get_hw_info(struct iommufd_ucmd *ucmd)
{
	const u32 SUPPORTED_FLAGS = IOMMU_HW_INFO_FLAG_INPUT_TYPE;	/* [한국어] 지금 아는 플래그는 하나뿐. 나머지가 켜져 있으면 거절한다. */
	struct iommu_hw_info *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */
	void __user *user_ptr = u64_to_user_ptr(cmd->data_uptr);	/* [한국어] 정보를 채워 넣을 사용자 버퍼. 64비트 정수를 포인터로 바꾼다. */
	const struct iommu_ops *ops;	/* [한국어] 그 장치를 다루는 드라이버의 연산표. */
	struct iommufd_device *idev;	/* [한국어] 물어보는 장치. */
	unsigned int data_len;	/* [한국어] 드라이버가 준 정보의 길이. */
	unsigned int copy_len;	/* [한국어] 실제로 복사할 길이(사용자 버퍼와 견준 값). */
	void *data;	/* [한국어] 드라이버가 만들어 준 정보 버퍼. */
	int rc;	/* [한국어] 결과 코드. */

	if (cmd->flags & ~SUPPORTED_FLAGS)	/* [한국어] 모르는 플래그가 켜져 있으면 */
		return -EOPNOTSUPP;	/* [한국어] 거절한다. 조용히 무시하면 나중에 의미를 붙일 수 없다. */
	if (cmd->__reserved[0] || cmd->__reserved[1] || cmd->__reserved[2])	/* [한국어] 예약 필드는 모두 0 이어야 한다. */
		return -EOPNOTSUPP;	/* [한국어] 거절. */

	/* Clear the type field since drivers don't support a random input */
	if (!(cmd->flags & IOMMU_HW_INFO_FLAG_INPUT_TYPE))	/* [한국어] 형식을 지정하는 플래그를 주지 않았으면 */
		cmd->in_data_type = IOMMU_HW_INFO_TYPE_DEFAULT;	/* [한국어] 원 주석대로 그 필드를 지운다 — 드라이버는 임의의 입력을 다루지 못하므로, 쓰레기 값이 흘러 들어가지 않게 한다. */

	idev = iommufd_get_device(ucmd, cmd->dev_id);	/* [한국어] id 로 장치를 찾고 참조를 든다. */
	if (IS_ERR(idev))	/* [한국어] 없는 id 다. */
		return PTR_ERR(idev);	/* [한국어] 오류를 올린다. */

	ops = dev_iommu_ops(idev->dev);	/* [한국어] 이 장치를 맡은 IOMMU 드라이버. */
	if (ops->hw_info) {	/* [한국어] 정보를 줄 수 있는 드라이버라면 */
		data = ops->hw_info(idev->dev, &data_len, &cmd->out_data_type);	/* [한국어] 드라이버가 자기 형식으로 버퍼를 만들어 준다. 형식 번호도 함께 알려 준다. */
		if (IS_ERR(data)) {	/* [한국어] 만들지 못했다. */
			rc = PTR_ERR(data);	/* [한국어] 오류를 꺼내 */
			goto out_put;	/* [한국어] 장치 참조를 놓고 나간다. */
		}

		/*
		 * drivers that have hw_info callback should have a unique
		 * iommu_hw_info_type.
		 */
		if (WARN_ON_ONCE(cmd->out_data_type ==	/* [한국어] 원 주석대로 hw_info 를 가진 드라이버는 고유한 형식 번호를 가져야 한다. */
				 IOMMU_HW_INFO_TYPE_NONE)) {	/* [한국어] NONE 을 답했다면 드라이버 쪽 버그다. */
			rc = -EOPNOTSUPP;	/* [한국어] 사용자에게는 지원하지 않는 것으로 알린다. */
			goto out_free;	/* [한국어] 버퍼를 해제하고 나간다. */
		}
	} else {	/* [한국어] hw_info 가 없는 드라이버라면 */
		cmd->out_data_type = IOMMU_HW_INFO_TYPE_NONE;	/* [한국어] 알려 줄 것이 없다고 답한다. 오류가 아니다. */
		data_len = 0;	/* [한국어] 줄 정보가 없다. */
		data = NULL;	/* [한국어] 버퍼도 없다. */
	}

	copy_len = min(cmd->data_len, data_len);	/* [한국어] 사용자 버퍼와 실제 정보 중 작은 쪽까지만 복사한다. */
	if (copy_to_user(user_ptr, data, copy_len)) {	/* [한국어] 사용자 버퍼로 옮긴다. */
		rc = -EFAULT;	/* [한국어] 사용자 주소가 잘못됐다. */
		goto out_free;	/* [한국어] 버퍼를 해제하고 나간다. */
	}

	/*
	 * Zero the trailing bytes if the user buffer is bigger than the
	 * data size kernel actually has.
	 */
	if (copy_len < cmd->data_len) {	/* [한국어] 사용자 버퍼가 더 크면 */
		if (clear_user(user_ptr + copy_len, cmd->data_len - copy_len)) {	/* [한국어] 원 주석대로 남는 뒷부분을 0 으로 채운다 — 옛 커널이 채우지 않은 자리를 사용자가 유효한 값으로 오해하면 안 된다. */
			rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */
			goto out_free;	/* [한국어] 나간다. */
		}
	}

	/*
	 * We return the length the kernel supports so userspace may know what
	 * the kernel capability is. It could be larger than the input buffer.
	 */
	cmd->data_len = data_len;	/* [한국어] 원 주석대로 커널이 아는 전체 길이를 알려 준다. 사용자 버퍼보다 클 수 있고, 그래야 사용자가 다시 물을 수 있다. */

	cmd->out_capabilities = 0;	/* [한국어] 능력 비트를 처음부터 쌓는다. */
	if (device_iommu_capable(idev->dev, IOMMU_CAP_DIRTY_TRACKING))	/* [한국어] 더티 추적을 하는 하드웨어면 */
		cmd->out_capabilities |= IOMMU_HW_CAP_DIRTY_TRACKING;	/* [한국어] 그 비트를 세운다. 마이그레이션에 쓸 수 있다는 뜻이다. */

	/* Report when ATS cannot be used for this device */
	if (!device_iommu_capable(idev->dev, IOMMU_CAP_PCI_ATS_SUPPORTED))	/* [한국어] ATS(장치 쪽 변환 캐시)를 쓸 수 없으면 */
		cmd->out_capabilities |= IOMMU_HW_CAP_PCI_ATS_NOT_SUPPORTED;	/* [한국어] 부정형 비트로 알린다 — 옛 커널이 0 을 답했을 때 "지원함"으로 읽히게 하려는 배치다. */

	cmd->out_max_pasid_log2 = 0;	/* [한국어] PASID 를 못 쓰는 것이 기본값. */
	/*
	 * Currently, all iommu drivers enable PASID in the probe_device()
	 * op if iommu and device supports it. So the max_pasids stored in
	 * dev->iommu indicates both PASID support and enable status. A
	 * non-zero dev->iommu->max_pasids means PASID is supported and
	 * enabled. The iommufd only reports PASID capability to userspace
	 * if it's enabled.
	 */
	if (idev->dev->iommu->max_pasids) {	/* [한국어] 원 주석의 근거대로 이 값이 0 이 아니라는 것은 PASID 지원과 활성화를 함께 뜻한다. */
		cmd->out_max_pasid_log2 = ilog2(idev->dev->iommu->max_pasids);	/* [한국어] 개수가 아니라 비트 폭으로 알린다 — PASID 공간은 늘 2의 거듭제곱이다. */

		if (dev_is_pci(idev->dev)) {	/* [한국어] PCI 장치라면 더 자세히 알려 준다. */
			struct pci_dev *pdev = to_pci_dev(idev->dev);	/* [한국어] PCI 쪽으로 내려간다. */
			int ctrl;	/* [한국어] PASID 제어 레지스터 값. */

			ctrl = pci_pasid_status(pdev);	/* [한국어] 장치의 PASID 설정을 읽는다. */

			WARN_ON_ONCE(ctrl < 0 ||	/* [한국어] max_pasids 가 0 이 아닌데 PASID 가 꺼져 있으면 앞뒤가 맞지 않는다. */
				     !(ctrl & PCI_PASID_CTRL_ENABLE));

			if (ctrl & PCI_PASID_CTRL_EXEC)	/* [한국어] 실행 권한 요청을 할 수 있는 장치면 */
				cmd->out_capabilities |=	/* [한국어] 그 능력을 알린다. 게스트가 그 권한을 다룰 수 있어야 하기 때문이다. */
						IOMMU_HW_CAP_PCI_PASID_EXEC;
			if (ctrl & PCI_PASID_CTRL_PRIV)	/* [한국어] 특권 모드 요청을 할 수 있는 장치면 */
				cmd->out_capabilities |=	/* [한국어] 그 능력도 알린다. */
						IOMMU_HW_CAP_PCI_PASID_PRIV;
		}
	}

	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 채운 값을 사용자 버퍼에 되돌려 쓴다. */
out_free:	/* [한국어] 드라이버 버퍼를 해제해야 하는 경로. */
	kfree(data);	/* [한국어] NULL 이어도 안전하다. */
out_put:	/* [한국어] 장치 참조를 놓아야 하는 경로. */
	iommufd_put_object(ucmd->ictx, &idev->obj);	/* [한국어] 조회용 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}
