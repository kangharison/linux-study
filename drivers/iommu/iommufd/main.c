// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2021 Intel Corporation
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 *
 * iommufd provides control over the IOMMU HW objects created by IOMMU kernel
 * drivers. IOMMU HW objects revolve around IO page tables that map incoming DMA
 * addresses (IOVA) to CPU addresses.
 */
/*
 * [한국어 설명] iommufd 의 파일 연산과 객체 수명 관리 (main.c)
 *
 * === 파일의 역할 ===
 * iommufd 의 뼈대다. 문자 장치를 등록하고, 사용자가 낸 ioctl 을 명령 표로
 * 갈라 보내고, 객체의 생성·확정·파괴를 관장한다.
 *
 * 원 주석이 이 인터페이스의 목적을 밝힌다 — IOMMU 드라이버가 만든 하드웨어
 * 객체를 사용자 공간이 제어하게 하고, 그 객체들의 중심에는 들어오는 DMA
 * 주소(IOVA)를 CPU 주소로 옮기는 페이지 테이블이 있다.
 *
 * 이 파일에서 가장 미묘한 것이 객체 수명이다. 세 단계로 나뉜다.
 *  1) alloc — id 를 예약하되 포인터는 아직 공개하지 않는다. 그래서 그
 *     사이에 실패하면 안전하게 되돌릴 수 있다.
 *  2) finalize — 포인터를 공개한다. 이 순간부터 다른 스레드가 볼 수 있어
 *     더는 마음대로 없앨 수 없으므로, 실패할 수 있는 일은 모두 그 전에
 *     끝나 있어야 한다.
 *  3) remove — xarray 말고는 아무도 참조하지 않을 때만 지운다.
 *
 * 참조가 두 겹인 이유도 여기 있다. users 는 보통의 참조이고, wait_cnt 는
 * "지금 이 객체를 쓰는 중인 스레드가 있는가"를 말한다. 외부 드라이버가
 * 잡은 참조는 짧게 끝나거나 pre_destroy 로 끊을 수 있어야 하므로, 지우려는
 * 쪽이 그것을 기다릴 수 있어야 한다.
 *
 * 파일을 닫을 때의 정리도 특이하다. 객체들이 서로를 참조하는 그래프를
 * 이루므로, 참조가 1 인 잎부터 되풀이해 지워 나간다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간 open("/dev/iommu") → 이 파일의 file_operations
 *   → 명령 표 → ioas.c / hw_pagetable.c / device.c / viommu.c
 *   → io_pagetable.c / pages.c → iommu 코어 → 드라이버
 *
 * /dev/vfio/vfio 도 같은 연산으로 등록되어, VFIO 컨테이너를 이 구현이
 * 대신할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 모든 진입점이 사용자 시스템 콜이다.
 *
 * === 타 모듈과의 연결 ===
 * 위: 사용자 공간(VMM, VFIO 라이브러리).
 * 아래: 이 디렉터리의 각 객체 구현, iommu 코어.
 * 옆: drivers/vfio 가 iommufd_ctx_from_fd() 로 이 문맥을 얻어 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * _iommufd_object_alloc / finalize / abort: 객체 생성의 세 단계.
 * iommufd_object_remove: 파괴. REMOVE_WAIT 면 다른 사용자를 기다린다.
 * iommufd_fops_ioctl: 명령 번호로 표를 찾아 실행하고, 그 명령이 만든
 *   객체를 성패에 따라 확정하거나 되돌린다.
 * iommufd_fops_release: 객체 그래프를 잎부터 되풀이해 허문다.
 * iommufd_fops_mmap: 드라이버가 노출한 MMIO 영역을 사용자에게 매핑한다.
 * iommufd_ioctl_ops: 명령 번호 → 구현 함수 표. IOCTL_OP 매크로가
 *   구조체 크기 검사까지 컴파일 시에 건다.
 * iommufd_object_ops: 객체 종류 → 파괴·되돌리기 함수 표.
 */
#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/bug.h>	/* [한국어] BUILD_BUG_ON_ZERO 로 배치를 컴파일 시 검사한다 */
#include <linux/file.h>	/* [한국어] 파일 참조와 즉시 해제 */
#include <linux/fs.h>	/* [한국어] file_operations */
#include <linux/iommufd.h>	/* [한국어] 드라이버에 공개된 부분 */
#include <linux/miscdevice.h>	/* [한국어] /dev/iommu 등록 */
#include <linux/module.h>	/* [한국어] 모듈 진입점 */
#include <linux/mutex.h>	/* [한국어] 문맥의 락들 */
#include <linux/slab.h>	/* [한국어] 객체 할당 */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 구조체와 명령 번호 */

#include "io_pagetable.h"	/* [한국어] IOAS 파괴 시 필요하다 */
#include "iommufd_private.h"	/* [한국어] 객체 모델 */
#include "iommufd_test.h"	/* [한국어] 시험 전용 명령 */

/*
 * [한국어] 객체 종류마다 다른 파괴 절차.
 *
 * abort 가 따로 있는 이유가 미묘하다. 확정 전에 되돌릴 때와 확정 후에
 * 파괴할 때 해야 할 일이 다른 종류가 있는데, 그런 경우 abort 를 둔다.
 * file_offset 은 그 객체가 파일을 들고 있는 경우 그 필드의 위치다.
 */
struct iommufd_object_ops {
	size_t file_offset;
	/* [한국어] 이 객체가 파일을 들고 있다면 그 필드의 오프셋.
	 * 설정자: IOMMUFD_FILE_OFFSET 매크로가 컴파일 시 계산한다.
	 * 읽는 자: 되돌리기 경로가 그 파일을 즉시 해제할 때.
	 * 값 범위: 0 이면 파일이 없다.
	 * 동기화: 표 자체가 상수다. */
	void (*pre_destroy)(struct iommufd_object *obj);
	/* [한국어] 파괴 전에 다른 참조를 끊는 절차.
	 * 설정자: 표에 정의된 종류만.
	 * 읽는 자: iommufd_object_dec_wait 가 기다리기 전에 부른다.
	 * 값 범위: 함수 포인터 또는 NULL.
	 * 동기화: 표 자체가 상수다.
	 * 이것이 없으면 외부 드라이버가 잡은 참조를 끊을 방법이 없어 영원히 기다린다. */
	void (*destroy)(struct iommufd_object *obj);
	/* [한국어] 종류별 정리 절차.
	 * 설정자: 모든 종류가 정의해야 한다.
	 * 읽는 자: 파괴 경로와 파일 닫힘 경로.
	 * 값 범위: 함수 포인터.
	 * 동기화: 표 자체가 상수다. */
	void (*abort)(struct iommufd_object *obj);
	/* [한국어] 확정 전 되돌리기 전용 절차.
	 * 설정자: 그것이 파괴와 달라야 하는 종류만.
	 * 읽는 자: iommufd_object_abort_and_destroy.
	 * 값 범위: 함수 포인터 또는 NULL(없으면 destroy 를 쓴다).
	 * 동기화: 표 자체가 상수다.
	 * 이것을 가진 종류는 _ucmd 할당기를 쓸 수 없다 — 락 위치가 맞지 않는다. */
};
static const struct iommufd_object_ops iommufd_object_ops[];	/* [한국어] 파일 끝에 정의된 표를 미리 쓴다 */
static struct miscdevice vfio_misc_dev;	/* [한국어] 열린 장치를 가려내려고 아래에서 주소를 견준다 */

/*
 * [한국어]
 * _iommufd_object_alloc - 객체를 만들고 id 를 예약한다
 *
 * @ictx: 문맥.
 * @size: 만들 구조체의 크기.
 * @type: 객체 종류.
 * @return: 새 객체, 실패하면 ERR_PTR.
 *
 * id 는 배정하되 포인터는 아직 넣지 않는 것이 요점이다. 원 주석이 이유를
 * 밝힌다 — 포인터가 공개되어 다른 스레드가 볼 수 있게 되면 그 객체를
 * 확실히 없앨 방법이 없으므로, 실패할 수 있는 일은 모두 finalize 전에
 * 끝나 있어야 한다.
 *
 * wait_cnt 를 1 로 시작하는 것도 그 규칙의 일부다. xarray 가 그 참조를
 * 들고 있는 셈이라, 목록에서 빠질 때 내려간다.
 *
 * GFP_KERNEL_ACCOUNT 인 이유: 사용자가 시킨 할당이므로 그 cgroup 의
 * 메모리로 계상해야 한다.
 */
struct iommufd_object *_iommufd_object_alloc(struct iommufd_ctx *ictx,
					     size_t size,
					     enum iommufd_object_type type)
{
	struct iommufd_object *obj;	/* [한국어] 만들 객체 */
	int rc;	/* [한국어] 결과 */

	obj = kzalloc(size, GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자가 시킨 할당이라 그 cgroup 에 계상한다 */
	if (!obj)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 호출자에게 */
	obj->type = type;	/* [한국어] 종류 검사에 쓰인다 */
	/* Starts out bias'd by 1 until it is removed from the xarray */
	refcount_set(&obj->wait_cnt, 1);	/* [한국어] (원 주석: xarray 에서 빠질 때까지 1 만큼 치우쳐 있다) */
	refcount_set(&obj->users, 1);	/* [한국어] 호출자가 들고 있는 참조 */

	/*
	 * Reserve an ID in the xarray but do not publish the pointer yet since
	 * the caller hasn't initialized it yet. Once the pointer is published
	 * in the xarray and visible to other threads we can't reliably destroy
	 * it anymore, so the caller must complete all errorable operations
	 * before calling iommufd_object_finalize().
	 */
	rc = xa_alloc(&ictx->objects, &obj->id, XA_ZERO_ENTRY, xa_limit_31b,	/* [한국어] (원 주석: id 만 예약하고 포인터는 아직 공개하지 않는다 — 공개되면 확실히 없앨 수 없다) */
		      GFP_KERNEL_ACCOUNT);	/* [한국어] 같은 이유로 계상한다 */
	if (rc)	/* [한국어] id 가 고갈됐거나 메모리 부족 */
		goto out_free;	/* [한국어] 되돌린다 */
	return obj;	/* [한국어] 호출자가 채운 뒤 finalize 한다 */
out_free:	/* [한국어] id 를 못 얻었을 때 — 아직 아무도 못 보았으므로 그냥 버린다 */
	kfree(obj);	/* [한국어] 아직 아무도 못 보았으므로 그냥 버린다 */
	return ERR_PTR(rc);	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * _iommufd_object_alloc_ucmd - ioctl 처리 중에 객체를 만든다
 *
 * @ucmd: 처리 중인 명령.
 * @size: 만들 구조체의 크기.
 * @type: 객체 종류.
 * @return: 새 객체, 실패하면 ERR_PTR.
 *
 * 만든 객체를 명령에 기록해 두면, ioctl 이 끝날 때 코어가 성패에 따라
 * 확정하거나 되돌린다. 그래서 각 명령 구현이 그 처리를 되풀이하지 않아도
 * 된다.
 *
 * abort 를 가진 종류를 거절하는 이유를 원 주석이 설명한다 — abort 는
 * 호출자의 락 안에서 불려야 하는데, 이 방식에서는 코어가 락 밖에서
 * 부르게 되어 맞지 않는다.
 */
struct iommufd_object *_iommufd_object_alloc_ucmd(struct iommufd_ucmd *ucmd,
						  size_t size,
						  enum iommufd_object_type type)
{
	struct iommufd_object *new_obj;	/* [한국어] 만들 객체 */

	/* Something is coded wrong if this is hit */
	if (WARN_ON(ucmd->new_obj))	/* [한국어] (원 주석: 여기 걸리면 코드가 잘못 쓰인 것이다) */
		return ERR_PTR(-EBUSY);	/* [한국어] 한 명령이 두 객체를 만들 수는 없다 */

	/*
	 * An abort op means that its caller needs to invoke it within a lock in
	 * the caller. So it doesn't work with _iommufd_object_alloc_ucmd() that
	 * will invoke the abort op in iommufd_object_abort_and_destroy(), which
	 * must be outside the caller's lock.
	 */
	if (WARN_ON(iommufd_object_ops[type].abort))	/* [한국어] (원 주석: abort 는 호출자의 락 안에서 불려야 하는데 이 방식은 락 밖에서 부른다) */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 그런 종류는 이 할당기를 쓸 수 없다 */

	new_obj = _iommufd_object_alloc(ucmd->ictx, size, type);	/* [한국어] 보통의 할당기로 */
	if (IS_ERR(new_obj))	/* [한국어] 실패면 */
		return new_obj;	/* [한국어] 그대로 전한다 */

	ucmd->new_obj = new_obj;	/* [한국어] ioctl 이 끝날 때 코어가 확정하거나 되돌린다 */
	return new_obj;	/* [한국어] 호출자가 채운다 */
}

/*
 * Allow concurrent access to the object.
 *
 * Once another thread can see the object pointer it can prevent object
 * destruction. Expect for special kernel-only objects there is no in-kernel way
 * to reliably destroy a single object. Thus all APIs that are creating objects
 * must use iommufd_object_abort() to handle their errors and only call
 * iommufd_object_finalize() once object creation cannot fail.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_object_finalize - 객체를 사용자에게 보이게 확정한다
 *
 * @ictx: 문맥.
 * @obj: 확정할 객체.
 *
 * 예약해 둔 자리에 실제 포인터를 넣는다. 원 주석이 그 뒤의 계약을
 * 못박는다 — 이 순간부터 다른 스레드가 객체를 볼 수 있고 파괴를 막을 수
 * 있으므로, 객체를 만드는 모든 API 는 실패 처리를 abort 로 하고 더는
 * 실패할 수 없게 된 뒤에야 이 함수를 불러야 한다.
 */
void iommufd_object_finalize(struct iommufd_ctx *ictx,
			     struct iommufd_object *obj)
{
	XA_STATE(xas, &ictx->objects, obj->id);	/* [한국어] 예약해 둔 자리 */
	void *old;	/* [한국어] 거기 있던 값 */

	xa_lock(&ictx->objects);	/* [한국어] 조회와 배타적으로 */
	old = xas_store(&xas, obj);	/* [한국어] 이 순간부터 다른 스레드가 볼 수 있다 */
	xa_unlock(&ictx->objects);	/* [한국어] 공개 완료 */
	/* obj->id was returned from xa_alloc() so the xas_store() cannot fail */
	WARN_ON(old != XA_ZERO_ENTRY);	/* [한국어] (원 주석: id 가 xa_alloc 에서 나왔으므로 저장은 실패할 수 없다) */
}

/* Undo _iommufd_object_alloc() if iommufd_object_finalize() was not called */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_object_abort - 확정하지 않은 객체를 되돌린다
 *
 * @ictx: 문맥.
 * @obj: 되돌릴 객체.
 *
 * 예약해 둔 id 를 비우고 메모리를 돌려준다. 아직 아무도 볼 수 없었으므로
 * 참조가 하나뿐이어야 하고, 아니면 코드 쪽 버그다.
 */
void iommufd_object_abort(struct iommufd_ctx *ictx, struct iommufd_object *obj)
{
	XA_STATE(xas, &ictx->objects, obj->id);	/* [한국어] 예약해 둔 자리 */
	void *old;	/* [한국어] 거기 있던 값 */

	xa_lock(&ictx->objects);	/* [한국어] 조회와 배타적으로 */
	old = xas_store(&xas, NULL);	/* [한국어] 예약을 비운다 */
	xa_unlock(&ictx->objects);	/* [한국어] 비우기 완료 */
	WARN_ON(old != XA_ZERO_ENTRY);	/* [한국어] 확정된 객체에 이 함수를 부르면 안 된다 */

	if (WARN_ON(!refcount_dec_and_test(&obj->users)))	/* [한국어] 아직 아무도 못 보았으므로 참조가 하나여야 한다 */
		return;	/* [한국어] 아니면 코드 쪽 버그라 해제하지 않는다 */

	kfree(obj);	/* [한국어] 메모리를 돌려준다 */
}

/*
 * Abort an object that has been fully initialized and needs destroy, but has
 * not been finalized.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_object_abort_and_destroy - 초기화까지 마친 객체를 되돌린다
 *
 * @ictx: 문맥.
 * @obj: 되돌릴 객체.
 *
 * abort 와 달리 타입별 정리까지 부른다. 객체를 다 만들어 놓고 그 뒤의
 * 단계에서 실패한 경우에 쓴다.
 *
 * 파일 처리가 이 함수의 까다로운 부분이다. 원 주석이 상세히 설명한다 —
 * 보통의 fput 은 작업 큐로 미뤄져 아래의 kfree 와 순서가 뒤바뀔 수 있어,
 * 즉시 해제하는 동기 버전을 쓴다. 그리고 이 시점에 파일 참조가 하나여야
 * 하므로, 객체를 만드는 함수는 성공이 보장되기 전에 다른 스레드가 그
 * 파일의 참조를 잡을 수 있게 해서는 안 된다.
 */
void iommufd_object_abort_and_destroy(struct iommufd_ctx *ictx,
				      struct iommufd_object *obj)
{
	const struct iommufd_object_ops *ops = &iommufd_object_ops[obj->type];	/* [한국어] 종류별 절차 */

	if (ops->file_offset) {	/* [한국어] 파일을 들고 있는 종류면 */
		struct file **filep = ((void *)obj) + ops->file_offset;	/* [한국어] 그 필드의 위치 */

		/*
		 * A file should hold a users refcount while the file is open
		 * and put it back in its release. The file should hold a
		 * pointer to obj in their private data. Normal fput() is
		 * deferred to a workqueue and can get out of order with the
		 * following kfree(obj). Using the sync version ensures the
		 * release happens immediately. During abort we require the file
		 * refcount is one at this point - meaning the object alloc
		 * function cannot do anything to allow another thread to take a
		 * refcount prior to a guaranteed success.
		 */
		if (*filep)	/* [한국어] (원 주석: 보통의 fput 은 작업 큐로 미뤄져 아래 kfree 와 순서가 뒤바뀔 수 있다) */
			__fput_sync(*filep);	/* [한국어] 즉시 해제해 순서를 보장한다 */
	}

	if (ops->abort)	/* [한국어] 되돌리기 전용 절차가 있으면 */
		ops->abort(obj);	/* [한국어] 그것을 쓰고 */
	else
		ops->destroy(obj);	/* [한국어] 없으면 보통의 파괴 절차를 */
	iommufd_object_abort(ictx, obj);	/* [한국어] id 를 비우고 메모리를 돌려준다 */
}

/*
 * [한국어]
 * iommufd_get_object - id 로 객체를 찾아 붙잡는다
 *
 * @ictx: 문맥.
 * @id: 사용자가 준 id.
 * @type: 기대하는 종류(IOMMUFD_OBJ_ANY 면 아무거나).
 * @return: 그 객체, 없거나 종류가 다르면 ERR_PTR(-ENOENT).
 *
 * 모든 ioctl 이 여기서 시작한다. 종류 검사가 함께 이루어지는 이유는
 * 사용자가 아무 id 나 줄 수 있기 때문이다.
 *
 * xa_lock 안에서 붙잡는 것이 중요하다 — 락 밖이면 찾은 직후에 그 객체가
 * 사라질 수 있다.
 */
struct iommufd_object *iommufd_get_object(struct iommufd_ctx *ictx, u32 id,
					  enum iommufd_object_type type)
{
	struct iommufd_object *obj;	/* [한국어] 찾을 객체 */

	if (iommufd_should_fail())	/* [한국어] 시험이 지정한 실패 지점이면 */
		return ERR_PTR(-ENOENT);	/* [한국어] 오류 처리 경로를 태운다 */

	xa_lock(&ictx->objects);	/* [한국어] 찾은 직후 사라지지 않도록 */
	obj = xa_load(&ictx->objects, id);	/* [한국어] id 로 찾고 */
	if (!obj || (type != IOMMUFD_OBJ_ANY && obj->type != type) ||	/* [한국어] 없거나 기대한 종류가 아니거나 */
	    !iommufd_lock_obj(obj))	/* [한국어] 이미 파괴 중이면 */
		obj = ERR_PTR(-ENOENT);	/* [한국어] 사용자에게는 모두 "없음"이다 */
	xa_unlock(&ictx->objects);	/* [한국어] 붙잡기 완료 */
	return obj;	/* [한국어] 붙잡은 객체 또는 오류 */
}

/*
 * [한국어]
 * iommufd_object_dec_wait - 다른 사용자가 놓을 때까지 기다린다
 *
 * @ictx: 문맥.
 * @to_destroy: 파괴하려는 객체.
 * @return: 0 이면 이제 아무도 쓰지 않는다, -EBUSY 면 시간이 다 됐다.
 *
 * 내가 마지막이면 곧바로 끝난다. 아니면 pre_destroy 로 다른 참조를 끊게
 * 하고, 그래도 남으면 잠들어 기다린다.
 *
 * 60초 제한을 두고 그 뒤에는 실패로 처리하는 것이 이 함수의 판단이다.
 * 영원히 기다리면 프로세스가 죽지 않으므로, 차라리 경고를 남기고 객체를
 * 남겨 둔다 — 파일이 닫힐 때 다시 시도된다.
 */
static int iommufd_object_dec_wait(struct iommufd_ctx *ictx,
				   struct iommufd_object *to_destroy)
{
	if (refcount_dec_and_test(&to_destroy->wait_cnt))	/* [한국어] 내가 마지막이면 */
		return 0;	/* [한국어] 기다릴 것이 없다 */

	if (iommufd_object_ops[to_destroy->type].pre_destroy)	/* [한국어] 끊을 수 있는 참조가 있는 종류면 */
		iommufd_object_ops[to_destroy->type].pre_destroy(to_destroy);	/* [한국어] 먼저 끊게 한다 */

	if (wait_event_timeout(ictx->destroy_wait,	/* [한국어] 모두 놓을 때까지 */
			       refcount_read(&to_destroy->wait_cnt) == 0,	/* [한국어] 잠들어 기다린다 */
			       msecs_to_jiffies(60000)))	/* [한국어] 영원히 기다리면 프로세스가 죽지 않는다 */
		return 0;	/* [한국어] 이제 아무도 쓰지 않는다 */

	pr_crit("Time out waiting for iommufd object to become free\n");	/* [한국어] 참조를 놓지 않는 버그가 있다 */
	refcount_inc(&to_destroy->wait_cnt);	/* [한국어] 내려 두었던 것을 되돌리고 */
	return -EBUSY;	/* [한국어] 객체를 남긴다 — 파일이 닫힐 때 다시 시도된다 */
}

/*
 * Remove the given object id from the xarray if the only reference to the
 * object is held by the xarray.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_object_remove - 아무도 쓰지 않는 객체를 지운다
 *
 * @ictx: 문맥.
 * @to_destroy: 지울 객체(id 로만 지울 때는 NULL).
 * @id: 그 객체의 id.
 * @flags: REMOVE_WAIT, REMOVE_OBJ_TOMBSTONE.
 * @return: 0 성공, -EBUSY 다른 사용자가 있음, -ENOENT 없음.
 *
 * 원 주석이 wait_cnt 의 목적을 밝힌다 — 외부 드라이버가 쓰는 객체를 이
 * 함수가 확실하게 파괴할 수 있게 하려는 것이다. 그 참조는 짧게 끝나거나
 * (ioctl 처리 중), pre_destroy 로 끊을 수 있어야 한다(vdevice 가 잡은
 * 장치 참조처럼).
 *
 * refcount_dec_if_one 이 핵심이다. xarray 말고는 아무도 참조하지 않을
 * 때만 지운다 — 그렇지 않으면 -EBUSY 로 물러난다.
 *
 * 실패 경로에서 wait_cnt 를 되돌리는 이유: 그 참조는 xarray 의 몫이라,
 * 객체가 목록에 남는 이상 다시 1 이어야 한다.
 */
int iommufd_object_remove(struct iommufd_ctx *ictx,
			  struct iommufd_object *to_destroy, u32 id,
			  unsigned int flags)
{
	struct iommufd_object *obj;	/* [한국어] 목록에서 꺼낸 객체 */
	XA_STATE(xas, &ictx->objects, id);	/* [한국어] 그 자리 */
	bool zerod_wait_cnt = false;	/* [한국어] 기다림 계수를 이미 0 으로 만들었는가 */
	int ret;	/* [한국어] 결과 */

	/*
	 * The purpose of the wait_cnt is to ensure deterministic destruction
	 * of objects used by external drivers and destroyed by this function.
	 * Incrementing this wait_cnt should either be short lived, such as
	 * during ioctl execution, or be revoked and blocked during
	 * pre_destroy(), such as vdev holding the idev's refcount.
	 */
	if (flags & REMOVE_WAIT) {	/* [한국어] (원 주석: wait_cnt 는 외부 드라이버가 쓰는 객체를 확실히 파괴하기 위한 것이다) */
		ret = iommufd_object_dec_wait(ictx, to_destroy);	/* [한국어] 다른 사용자가 놓을 때까지 */
		if (ret) {	/* [한국어] 시간이 다 됐으면 */
			/*
			 * We have a bug. Put back the callers reference and
			 * defer cleaning this object until close.
			 */
			refcount_dec(&to_destroy->users);	/* [한국어] (원 주석: 버그다. 호출자의 참조를 되돌리고 정리를 close 로 미룬다) */
			return ret;	/* [한국어] 호출자에게 */
		}
		zerod_wait_cnt = true;	/* [한국어] 실패 경로에서 되돌려야 한다 */
	}

	xa_lock(&ictx->objects);	/* [한국어] 조회와 배타적으로 */
	obj = xas_load(&xas);	/* [한국어] 그 자리의 객체 */
	if (to_destroy) {	/* [한국어] 호출자가 객체를 알고 있으면 */
		/*
		 * If the caller is holding a ref on obj we put it here under
		 * the spinlock.
		 */
		refcount_dec(&obj->users);	/* [한국어] (원 주석: 호출자가 참조를 들고 있으면 여기 스핀락 아래에서 놓는다) */

		if (WARN_ON(obj != to_destroy)) {	/* [한국어] id 가 가리키는 것이 다르면 */
			ret = -ENOENT;	/* [한국어] 그사이 바뀐 것이다 */
			goto err_xa;	/* [한국어] 되돌린다 */
		}
	} else if (xa_is_zero(obj) || !obj) {	/* [한국어] id 만으로 지우는 경우: 비석이거나 비었으면 */
		ret = -ENOENT;	/* [한국어] 지울 것이 없다 */
		goto err_xa;	/* [한국어] 되돌린다 */
	}

	if (!refcount_dec_if_one(&obj->users)) {	/* [한국어] xarray 말고 다른 참조가 있으면 */
		ret = -EBUSY;	/* [한국어] 지금은 지울 수 없다 */
		goto err_xa;	/* [한국어] 되돌린다 */
	}

	xas_store(&xas, (flags & REMOVE_OBJ_TOMBSTONE) ? XA_ZERO_ENTRY : NULL);	/* [한국어] 비석으로 남길지 완전히 비울지 */
	if (ictx->vfio_ioas == container_of(obj, struct iommufd_ioas, obj))	/* [한국어] VFIO 호환 경로가 이 IOAS 를 쓰고 있었으면 */
		ictx->vfio_ioas = NULL;	/* [한국어] 그 포인터도 끊는다 */
	xa_unlock(&ictx->objects);	/* [한국어] 목록에서 빠졌다 */

	/*
	 * Since users is zero any positive wait_cnt must be racing
	 * iommufd_put_object(), or we have a bug.
	 */
	if (!zerod_wait_cnt) {	/* [한국어] (원 주석: users 가 0 이므로 양수인 wait_cnt 는 iommufd_put_object 와의 경합이거나 버그다) */
		ret = iommufd_object_dec_wait(ictx, obj);	/* [한국어] 그 경합이 끝나기를 기다린다 */
		if (WARN_ON(ret))	/* [한국어] 여기서 실패하면 버그다 */
			return ret;	/* [한국어] 객체를 남긴다 */
	}

	iommufd_object_ops[obj->type].destroy(obj);	/* [한국어] 종류별 정리 */
	kfree(obj);	/* [한국어] 메모리를 돌려준다 */
	return 0;	/* [한국어] 성공 */

err_xa:	/* [한국어] 지울 수 없을 때 — 목록에 남긴 채 되돌린다 */
	if (zerod_wait_cnt) {	/* [한국어] 기다림 계수를 내려 두었으면 */
		/* Restore the xarray owned reference */
		refcount_set(&obj->wait_cnt, 1);	/* [한국어] (원 주석: xarray 가 소유한 참조를 복원한다) */
	}
	xa_unlock(&ictx->objects);	/* [한국어] 되돌리기 완료 */

	/* The returned object reference count is zero */
	return ret;	/* [한국어] (원 주석: 돌려주는 객체의 참조 수는 0 이다) */
}

/*
 * [한국어]
 * iommufd_destroy - IOMMU_DESTROY 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자가 id 하나를 주고 지우라고 하는 가장 단순한 명령이다. 기다리지
 * 않으므로 다른 곳에서 쓰고 있으면 -EBUSY 가 돌아간다.
 */
static int iommufd_destroy(struct iommufd_ucmd *ucmd)
{
	struct iommu_destroy *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 id */

	return iommufd_object_remove(ucmd->ictx, NULL, cmd->id, 0);	/* [한국어] 기다리지 않는다 — 쓰는 중이면 -EBUSY */
}

/*
 * [한국어]
 * iommufd_fops_open - 파일이 열릴 때 문맥을 만든다
 *
 * @inode: 열린 노드.
 * @filp: 파일.
 * @return: 0 성공, -ENOMEM.
 *
 * 이 문맥 하나가 사용자의 IOMMU 세계 전체다 — 이후 만드는 모든 객체가
 * 여기 매달리고, 파일이 닫히면 함께 사라진다.
 *
 * /dev/vfio/vfio 로 열린 경우를 가르는 것이 눈에 띈다. 원 주석대로 VFIO
 * 와의 호환을 위해 고정 페이지 계상 방식을 VFIO 와 같게 맞춘다 —
 * 사용자 한도가 아니라 프로세스 통계에 넣는다.
 */
static int iommufd_fops_open(struct inode *inode, struct file *filp)
{
	struct iommufd_ctx *ictx;	/* [한국어] 만들 문맥 */

	ictx = kzalloc_obj(*ictx, GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자가 연 것이라 계상한다 */
	if (!ictx)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 열 수 없다 */

	/*
	 * For compatibility with VFIO when /dev/vfio/vfio is opened we default
	 * to the same rlimit accounting as vfio uses.
	 */
	if (IS_ENABLED(CONFIG_IOMMUFD_VFIO_CONTAINER) &&	/* [한국어] (원 주석: /dev/vfio/vfio 로 열리면 VFIO 와 같은 rlimit 계상을 기본으로 한다) */
	    filp->private_data == &vfio_misc_dev) {	/* [한국어] 그 장치로 열렸는가 */
		ictx->account_mode = IOPT_PAGES_ACCOUNT_MM;	/* [한국어] 사용자 한도가 아니라 프로세스 통계에 */
		pr_info_once("IOMMUFD is providing /dev/vfio/vfio, not VFIO.\n");	/* [한국어] 관리자가 혼동하지 않도록 한 번만 알린다 */
	}

	init_rwsem(&ictx->ioas_creation_lock);	/* [한국어] 기본 IOAS 자동 생성을 직렬화한다 */
	xa_init_flags(&ictx->objects, XA_FLAGS_ALLOC1 | XA_FLAGS_ACCOUNT);	/* [한국어] id 는 1 부터 — 0 은 "없음"으로 쓴다 */
	xa_init(&ictx->groups);	/* [한국어] 장치 그룹 상태 */
	ictx->file = filp;	/* [한국어] 문맥의 수명이 곧 파일의 수명이다 */
	mt_init_flags(&ictx->mt_mmap, MT_FLAGS_ALLOC_RANGE);	/* [한국어] mmap 오프셋을 구간으로 배정한다 */
	init_waitqueue_head(&ictx->destroy_wait);	/* [한국어] 파괴를 기다리는 곳 */
	mutex_init(&ictx->sw_msi_lock);	/* [한국어] MSI 배정 보호 */
	INIT_LIST_HEAD(&ictx->sw_msi_list);	/* [한국어] 아직 배정이 없다 */
	filp->private_data = ictx;	/* [한국어] 이후 모든 진입점이 여기서 문맥을 꺼낸다 */
	return 0;	/* [한국어] 열렸다 */
}

/*
 * [한국어]
 * iommufd_fops_release - 파일이 닫힐 때 모든 객체를 허문다
 *
 * @inode: 닫히는 노드.
 * @filp: 파일.
 * @return: 늘 0.
 *
 * 객체들이 서로를 참조하는 그래프를 이루므로 순서가 문제가 된다. 원
 * 주석이 방법을 설명한다 — 참조가 1 인 잎을 지우면 그것이 참조하던
 * 내부 객체의 수가 줄어들고, 그것을 되풀이하면 결국 전부 지워진다.
 *
 * 한 바퀴 돌았는데 아무것도 못 지웠다면 참조 계수에 버그가 있는 것이라
 * 경고하고 멈춘다 — 무한 반복보다 낫다.
 *
 * xa_empty 를 종료 조건으로 쓸 수 없는 이유도 원 주석에 있다. 비석은
 * XA_ZERO_ENTRY 로 남아 있어 배열이 비지 않지만, xa_for_each 는 그것을
 * 건너뛴다. 그래서 "한 바퀴 돌며 아무 항목도 못 봤는가"로 판단한다.
 */
static int iommufd_fops_release(struct inode *inode, struct file *filp)
{
	struct iommufd_ctx *ictx = filp->private_data;	/* [한국어] 허물 문맥 */
	struct iommufd_sw_msi_map *next;	/* [한국어] MSI 배정 순회용 */
	struct iommufd_sw_msi_map *cur;	/* [한국어] 같은 용도 */
	struct iommufd_object *obj;	/* [한국어] 객체 순회용 */

	/*
	 * The objects in the xarray form a graph of "users" counts, and we have
	 * to destroy them in a depth first manner. Leaf objects will reduce the
	 * users count of interior objects when they are destroyed.
	 *
	 * Repeatedly destroying all the "1 users" leaf objects will progress
	 * until the entire list is destroyed. If this can't progress then there
	 * is some bug related to object refcounting.
	 */
	while (!xa_empty(&ictx->objects)) {	/* [한국어] (원 주석: 객체들이 참조 그래프를 이루므로 깊이 우선으로 허물어야 한다) */
		unsigned int destroyed = 0;	/* [한국어] 이번 바퀴에 지운 수 */
		unsigned long index;	/* [한국어] xarray 인덱스 */
		bool empty = true;	/* [한국어] 이번 바퀴에 항목을 하나라도 보았는가 */

		/*
		 * We can't use xa_empty() to end the loop as the tombstones
		 * are stored as XA_ZERO_ENTRY in the xarray. However
		 * xa_for_each() automatically converts them to NULL and skips
		 * them causing xa_empty() to be kept false. Thus once
		 * xa_for_each() finds no further !NULL entries the loop is
		 * done.
		 */
		xa_for_each(&ictx->objects, index, obj) {	/* [한국어] (원 주석: 비석은 XA_ZERO_ENTRY 로 남아 xa_empty 를 거짓으로 만들지만 이 순회는 그것을 건너뛴다) */
			empty = false;	/* [한국어] 실제 항목을 보았다 */
			if (!refcount_dec_if_one(&obj->users))	/* [한국어] 아직 남이 참조하고 있으면 */
				continue;	/* [한국어] 다음 바퀴에 */

			destroyed++;	/* [한국어] 잎 하나를 지웠다 */
			xa_erase(&ictx->objects, index);	/* [한국어] 목록에서 빼고 */
			iommufd_object_ops[obj->type].destroy(obj);	/* [한국어] 종류별 정리 */
			kfree(obj);	/* [한국어] 메모리를 돌려준다 */
		}

		if (empty)	/* [한국어] 실제 항목이 하나도 없었으면 */
			break;	/* [한국어] 다 지웠다 */

		/* Bug related to users refcount */
		if (WARN_ON(!destroyed))	/* [한국어] (원 주석: users 참조 계수에 버그가 있다) */
			break;	/* [한국어] 무한 반복을 피한다 */
	}

	/*
	 * There may be some tombstones left over from
	 * iommufd_object_tombstone_user()
	 */
	xa_destroy(&ictx->objects);	/* [한국어] (원 주석: iommufd_object_tombstone_user() 가 남긴 비석이 있을 수 있다) */

	WARN_ON(!xa_empty(&ictx->groups));	/* [한국어] 장치가 모두 풀렸으면 비어 있어야 한다 */

	mutex_destroy(&ictx->sw_msi_lock);	/* [한국어] MSI 락 해제 */
	list_for_each_entry_safe(cur, next, &ictx->sw_msi_list, sw_msi_item)	/* [한국어] 남은 MSI 배정을 */
		kfree(cur);	/* [한국어] 모두 버린다 */

	kfree(ictx);	/* [한국어] 문맥 자체 */
	return 0;	/* [한국어] 닫혔다 */
}

/*
 * [한국어]
 * iommufd_option - IOMMU_OPTION 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, -EOPNOTSUPP 모르는 옵션, -EFAULT 복사 실패.
 *
 * 문맥 단위 옵션과 IOAS 단위 옵션을 한 명령으로 다룬다. 결과값을 따로
 * 복사하는 이유: 읽기 요청이면 커널이 채운 값을 돌려주어야 한다.
 */
static int iommufd_option(struct iommufd_ucmd *ucmd)
{
	struct iommu_option *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 옵션 */
	int rc;	/* [한국어] 결과 */

	if (cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 새 커널이 쓸 자리라 지금은 거절한다 */

	switch (cmd->option_id) {	/* [한국어] 옵션 종류에 따라 */
	case IOMMU_OPTION_RLIMIT_MODE:	/* [한국어] 고정 페이지 계상 방식 */
		rc = iommufd_option_rlimit_mode(cmd, ucmd->ictx);	/* [한국어] 문맥 단위 옵션 */
		break;
	case IOMMU_OPTION_HUGE_PAGES:	/* [한국어] 큰 페이지 사용 여부 */
		rc = iommufd_ioas_option(ucmd);	/* [한국어] IOAS 단위 옵션 */
		break;
	default:	/* [한국어] 모르는 옵션 */
		return -EOPNOTSUPP;	/* [한국어] 모르는 옵션 */
	}
	if (rc)	/* [한국어] 실패면 */
		return rc;	/* [한국어] 값을 돌려주지 않는다 */
	if (copy_to_user(&((struct iommu_option __user *)ucmd->ubuffer)->val64,	/* [한국어] 읽기 요청이면 커널이 채운 값을 */
			 &cmd->val64, sizeof(cmd->val64)))	/* [한국어] 돌려주어야 한다 */
		return -EFAULT;	/* [한국어] 사용자 버퍼가 잘못됐다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어] 어떤 명령이든 담을 수 있는 스택 버퍼.
 *
 * ioctl 처리가 사용자 구조체를 커널 스택으로 복사해 오는데, 명령마다
 * 크기가 다르다. 가장 큰 것에 맞춘 union 을 하나 두면 명령마다 할당할
 * 필요가 없다.
 *
 * IOCTL_OP 매크로가 각 명령의 크기를 이 union 과 견주는 컴파일 시 검사를
 * 걸어 두어, 새 명령을 추가하며 여기 넣는 것을 잊으면 빌드가 깨진다.
 */
union ucmd_buffer {
	struct iommu_destroy destroy;	/* [한국어] 객체 파괴 */
	struct iommu_fault_alloc fault;	/* [한국어] 폴트 큐 생성 */
	struct iommu_hw_info info;	/* [한국어] 하드웨어 능력 조회 */
	struct iommu_hw_queue_alloc hw_queue;	/* [한국어] 게스트용 하드웨어 큐 생성 */
	struct iommu_hwpt_alloc hwpt;	/* [한국어] HWPT 생성 */
	struct iommu_hwpt_get_dirty_bitmap get_dirty_bitmap;	/* [한국어] 더티 비트맵 조회 */
	struct iommu_hwpt_invalidate cache;	/* [한국어] 게스트 무효화 명령 전달 */
	struct iommu_hwpt_set_dirty_tracking set_dirty_tracking;	/* [한국어] 더티 추적 켜고 끄기 */
	struct iommu_ioas_alloc alloc;	/* [한국어] IOAS 생성 */
	struct iommu_ioas_allow_iovas allow_iovas;	/* [한국어] 쓸 IOVA 범위 지정 */
	struct iommu_ioas_copy ioas_copy;	/* [한국어] IOAS 사이 매핑 복사 */
	struct iommu_ioas_iova_ranges iova_ranges;	/* [한국어] 쓸 수 있는 범위 조회 */
	struct iommu_ioas_map map;	/* [한국어] 매핑 생성 */
	struct iommu_ioas_unmap unmap;	/* [한국어] 매핑 해제 */
	struct iommu_option option;	/* [한국어] 옵션 읽기·쓰기 */
	struct iommu_vdevice_alloc vdev;	/* [한국어] 가상 장치 생성 */
	struct iommu_veventq_alloc veventq;	/* [한국어] vIOMMU 이벤트 큐 생성 */
	struct iommu_vfio_ioas vfio_ioas;	/* [한국어] VFIO 호환 기본 IOAS 지정 */
	struct iommu_viommu_alloc viommu;	/* [한국어] vIOMMU 생성 */
#ifdef CONFIG_IOMMUFD_TEST	/* [한국어] 시험을 켠 커널에서만 */
	struct iommu_test_cmd test;	/* [한국어] 시험 전용 명령 */
#endif
};

/*
 * [한국어] 명령 하나에 대한 표 항목.
 * 번호, 구조체 크기 한계, 구현 함수를 함께 담는다.
 */
struct iommufd_ioctl_op {
	unsigned int size;
	/* [한국어] 커널이 아는 명령 구조체의 크기.
	 * 설정자: IOCTL_OP 매크로.
	 * 읽는 자: 사용자 구조체를 복사해 올 때의 상한.
	 * 값 범위: 그 명령의 sizeof.
	 * 동기화: 표 자체가 상수다. */
	unsigned int min_size;
	/* [한국어] 뜻이 통하려면 반드시 있어야 할 최소 크기.
	 * 설정자: IOCTL_OP 매크로가 지정한 마지막 필수 필드의 끝으로.
	 * 읽는 자: 사용자가 그보다 작게 주면 거절한다.
	 * 값 범위: size 이하.
	 * 동기화: 표 자체가 상수다. */
	unsigned int ioctl_num;
	/* [한국어] 이 자리에 해당하는 ioctl 번호 전체.
	 * 설정자: IOCTL_OP 매크로.
	 * 읽는 자: 순번만으로 찾은 뒤 방향과 크기까지 맞는지 확인한다.
	 * 값 범위: _IOC 로 만든 번호.
	 * 동기화: 표 자체가 상수다. */
	int (*execute)(struct iommufd_ucmd *ucmd);
	/* [한국어] 그 명령의 구현.
	 * 설정자: IOCTL_OP 매크로.
	 * 읽는 자: ioctl 진입점.
	 * 값 범위: 함수 포인터.
	 * 동기화: 표 자체가 상수다. */
};

#define IOCTL_OP(_ioctl, _fn, _struct, _last)                                  \
	[_IOC_NR(_ioctl) - IOMMUFD_CMD_BASE] = {                               \
		.size = sizeof(_struct) +                                      \
			BUILD_BUG_ON_ZERO(sizeof(union ucmd_buffer) <          \
					  sizeof(_struct)),                    \
		.min_size = offsetofend(_struct, _last),                       \
		.ioctl_num = _ioctl,                                           \
		.execute = _fn,                                                \
	}
static const struct iommufd_ioctl_op iommufd_ioctl_ops[] = {	/* [한국어] 명령 번호 → 구현 표. 순번으로 곧바로 찾는다 */
	IOCTL_OP(IOMMU_DESTROY, iommufd_destroy, struct iommu_destroy, id),	/* [한국어] 객체 파괴 — id 까지가 필수다 */
	IOCTL_OP(IOMMU_FAULT_QUEUE_ALLOC, iommufd_fault_alloc,
		 struct iommu_fault_alloc, out_fault_fd),
	IOCTL_OP(IOMMU_GET_HW_INFO, iommufd_get_hw_info, struct iommu_hw_info,
		 __reserved),
	IOCTL_OP(IOMMU_HW_QUEUE_ALLOC, iommufd_hw_queue_alloc_ioctl,
		 struct iommu_hw_queue_alloc, length),
	IOCTL_OP(IOMMU_HWPT_ALLOC, iommufd_hwpt_alloc, struct iommu_hwpt_alloc,
		 __reserved),
	IOCTL_OP(IOMMU_HWPT_GET_DIRTY_BITMAP, iommufd_hwpt_get_dirty_bitmap,
		 struct iommu_hwpt_get_dirty_bitmap, data),
	IOCTL_OP(IOMMU_HWPT_INVALIDATE, iommufd_hwpt_invalidate,
		 struct iommu_hwpt_invalidate, __reserved),
	IOCTL_OP(IOMMU_HWPT_SET_DIRTY_TRACKING, iommufd_hwpt_set_dirty_tracking,
		 struct iommu_hwpt_set_dirty_tracking, __reserved),
	IOCTL_OP(IOMMU_IOAS_ALLOC, iommufd_ioas_alloc_ioctl,
		 struct iommu_ioas_alloc, out_ioas_id),
	IOCTL_OP(IOMMU_IOAS_ALLOW_IOVAS, iommufd_ioas_allow_iovas,
		 struct iommu_ioas_allow_iovas, allowed_iovas),
	IOCTL_OP(IOMMU_IOAS_CHANGE_PROCESS, iommufd_ioas_change_process,
		 struct iommu_ioas_change_process, __reserved),
	IOCTL_OP(IOMMU_IOAS_COPY, iommufd_ioas_copy, struct iommu_ioas_copy,
		 src_iova),
	IOCTL_OP(IOMMU_IOAS_IOVA_RANGES, iommufd_ioas_iova_ranges,
		 struct iommu_ioas_iova_ranges, out_iova_alignment),
	IOCTL_OP(IOMMU_IOAS_MAP, iommufd_ioas_map, struct iommu_ioas_map, iova),
	IOCTL_OP(IOMMU_IOAS_MAP_FILE, iommufd_ioas_map_file,
		 struct iommu_ioas_map_file, iova),
	IOCTL_OP(IOMMU_IOAS_UNMAP, iommufd_ioas_unmap, struct iommu_ioas_unmap,
		 length),
	IOCTL_OP(IOMMU_OPTION, iommufd_option, struct iommu_option, val64),
	IOCTL_OP(IOMMU_VDEVICE_ALLOC, iommufd_vdevice_alloc_ioctl,
		 struct iommu_vdevice_alloc, virt_id),
	IOCTL_OP(IOMMU_VEVENTQ_ALLOC, iommufd_veventq_alloc,
		 struct iommu_veventq_alloc, out_veventq_fd),
	IOCTL_OP(IOMMU_VFIO_IOAS, iommufd_vfio_ioas, struct iommu_vfio_ioas,
		 __reserved),
	IOCTL_OP(IOMMU_VIOMMU_ALLOC, iommufd_viommu_alloc_ioctl,
		 struct iommu_viommu_alloc, out_viommu_id),
#ifdef CONFIG_IOMMUFD_TEST
	IOCTL_OP(IOMMU_TEST_CMD, iommufd_test, struct iommu_test_cmd, last),
#endif
};

/*
 * [한국어]
 * iommufd_fops_ioctl - 사용자 명령을 표에서 찾아 실행한다
 *
 * @filp: 파일.
 * @cmd: ioctl 번호.
 * @arg: 사용자 버퍼 주소.
 * @return: 명령의 결과.
 *
 * 표 범위 밖의 번호는 VFIO 호환 경로로 넘긴다 — /dev/vfio/vfio 로 열린
 * 경우 옛 VFIO ioctl 이 그대로 들어온다.
 *
 * 구조체 크기 처리가 이 함수의 요점이다. 사용자가 자기가 아는 크기를
 * 구조체 첫 필드에 적어 보내고, 커널은 그것과 자기가 아는 크기를 견주어
 * 부족한 뒤쪽을 0 으로 채운다(copy_struct_from_user). 그래서 옛 사용자와
 * 새 커널, 새 사용자와 옛 커널이 모두 동작한다.
 *
 * min_size 검사는 그 확장의 하한이다 — 어느 명령이든 그 필드까지는
 * 반드시 있어야 뜻이 통한다.
 *
 * 마지막의 new_obj 처리가 _ucmd 할당기의 짝이다.
 */
static long iommufd_fops_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct iommufd_ctx *ictx = filp->private_data;	/* [한국어] 이 파일의 문맥 */
	const struct iommufd_ioctl_op *op;	/* [한국어] 실행할 명령 */
	struct iommufd_ucmd ucmd = {};	/* [한국어] 명령 처리 상태 */
	union ucmd_buffer buf;	/* [한국어] 어떤 명령이든 담을 수 있는 스택 버퍼 */
	unsigned int nr;	/* [한국어] 명령 번호 */
	int ret;	/* [한국어] 결과 */

	nr = _IOC_NR(cmd);	/* [한국어] ioctl 번호에서 순번을 */
	if (nr < IOMMUFD_CMD_BASE ||	/* [한국어] 우리 범위 밖이면 */
	    (nr - IOMMUFD_CMD_BASE) >= ARRAY_SIZE(iommufd_ioctl_ops))	/* [한국어] 표에 없으면 */
		return iommufd_vfio_ioctl(ictx, cmd, arg);	/* [한국어] VFIO 호환 경로로 넘긴다 */

	ucmd.ictx = ictx;	/* [한국어] 명령 처리 상태를 채운다 */
	ucmd.ubuffer = (void __user *)arg;	/* [한국어] 사용자 버퍼 */
	ret = get_user(ucmd.user_size, (u32 __user *)ucmd.ubuffer);	/* [한국어] 첫 필드가 사용자가 아는 크기다 */
	if (ret)	/* [한국어] 읽을 수 없으면 */
		return ret;	/* [한국어] 잘못된 포인터 */

	op = &iommufd_ioctl_ops[nr - IOMMUFD_CMD_BASE];	/* [한국어] 표에서 찾고 */
	if (op->ioctl_num != cmd)	/* [한국어] 번호가 정확히 맞는지 — 방향과 크기까지 */
		return -ENOIOCTLCMD;	/* [한국어] 아니면 우리 명령이 아니다 */
	if (ucmd.user_size < op->min_size)	/* [한국어] 필수 필드까지도 없으면 */
		return -EINVAL;	/* [한국어] 뜻이 통하지 않는다 */

	ucmd.cmd = &buf;	/* [한국어] 스택 버퍼에 담는다 */
	ret = copy_struct_from_user(ucmd.cmd, op->size, ucmd.ubuffer,	/* [한국어] 모자란 뒤쪽은 0 으로 채우고 */
				    ucmd.user_size);	/* [한국어] 남는 뒤쪽은 0 인지 확인한다 */
	if (ret)	/* [한국어] 복사 실패거나 모르는 필드가 0 이 아니면 */
		return ret;	/* [한국어] 거절 */
	ret = op->execute(&ucmd);	/* [한국어] 명령을 실행한다 */

	if (ucmd.new_obj) {	/* [한국어] _ucmd 할당기로 객체를 만들었으면 */
		if (ret)	/* [한국어] 실패했으면 */
			iommufd_object_abort_and_destroy(ictx, ucmd.new_obj);	/* [한국어] 되돌리고 */
		else
			iommufd_object_finalize(ictx, ucmd.new_obj);	/* [한국어] 성공했으면 사용자에게 보이게 한다 */
	}
	return ret;	/* [한국어] 명령의 결과 */
}

/*
 * [한국어]
 * iommufd_fops_vma_open - 매핑이 복제될 때 참조를 올린다
 *
 * @vma: 복제된 매핑.
 *
 * fork 로 주소 공간이 복제되면 같은 MMIO 영역을 가리키는 vma 가 하나 더
 * 생긴다. 그 소유 객체가 먼저 사라지면 안 되므로 참조를 올린다.
 */
static void iommufd_fops_vma_open(struct vm_area_struct *vma)
{
	struct iommufd_mmap *immap = vma->vm_private_data;	/* [한국어] 이 매핑이 가리키는 영역 */

	refcount_inc(&immap->owner->users);	/* [한국어] 소유 객체가 먼저 사라지면 안 된다 */
}

/*
 * [한국어]
 * iommufd_fops_vma_close - 매핑이 사라질 때 참조를 내린다
 *
 * @vma: 사라지는 매핑.
 */
static void iommufd_fops_vma_close(struct vm_area_struct *vma)
{
	struct iommufd_mmap *immap = vma->vm_private_data;	/* [한국어] 이 매핑이 가리키던 영역 */

	refcount_dec(&immap->owner->users);	/* [한국어] 마지막 매핑이 사라지면 객체도 해제될 수 있다 */
}

static const struct vm_operations_struct iommufd_vma_ops = {	/* [한국어] MMIO 매핑의 복제와 해제를 가로챈다 */
	.open = iommufd_fops_vma_open,	/* [한국어] fork 로 복제될 때 참조를 올린다 */
	.close = iommufd_fops_vma_close,	/* [한국어] 사라질 때 내린다 */
};

/* The vm_pgoff must be pre-allocated from mt_mmap, and given to user space */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_fops_mmap - 드라이버가 노출한 MMIO 영역을 사용자에게 매핑한다
 *
 * @filp: 파일.
 * @vma: 만들 매핑.
 * @return: 0 성공, 음수면 거절.
 *
 * 원 주석이 계약을 밝힌다 — vm_pgoff 는 미리 배정되어 사용자에게 건네진
 * 값이어야 한다. 임의의 오프셋을 주면 매핑되지 않는다.
 *
 * 정확히 일치를 요구하는 이유도 원 주석에 있다. maple tree 조회는 그
 * 영역 안의 아무 주소로도 항목을 찾아 주므로, 시작과 길이가 모두 맞는
 * 경우만 허용해 영역의 일부만 매핑하는 것을 막는다.
 *
 * 실행 금지를 거절하고 캐시를 끄는 것은 MMIO 매핑의 상식적 조건이다.
 */
static int iommufd_fops_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct iommufd_ctx *ictx = filp->private_data;	/* [한국어] 이 파일의 문맥 */
	size_t length = vma->vm_end - vma->vm_start;	/* [한국어] 요청한 길이 */
	struct iommufd_mmap *immap;	/* [한국어] 노출된 영역 */
	int rc;	/* [한국어] 결과 */

	if (!PAGE_ALIGNED(length))	/* [한국어] 페이지 단위가 아니면 */
		return -EINVAL;	/* [한국어] MMIO 를 매핑할 수 없다 */
	if (!(vma->vm_flags & VM_SHARED))	/* [한국어] 사적 매핑이면 */
		return -EINVAL;	/* [한국어] 쓰기 시 복사가 MMIO 에는 뜻이 없다 */
	if (vma->vm_flags & VM_EXEC)	/* [한국어] 실행 가능으로 요청하면 */
		return -EPERM;	/* [한국어] 장치 레지스터를 실행할 이유가 없다 */

	mtree_lock(&ictx->mt_mmap);	/* [한국어] 조회 중 영역이 사라지지 않도록 */
	/* vma->vm_pgoff carries a page-shifted start position to an immap */
	immap = mtree_load(&ictx->mt_mmap, vma->vm_pgoff << PAGE_SHIFT);	/* [한국어] (원 주석: vm_pgoff 는 immap 을 가리키는 페이지 단위 시작 위치다) */
	if (!immap || !refcount_inc_not_zero(&immap->owner->users)) {	/* [한국어] 없거나 소유 객체가 사라지는 중이면 */
		mtree_unlock(&ictx->mt_mmap);	/* [한국어] 풀고 */
		return -ENXIO;	/* [한국어] 매핑할 수 없다 */
	}
	mtree_unlock(&ictx->mt_mmap);	/* [한국어] 참조를 잡았으니 놓아도 된다 */

	/*
	 * mtree_load() returns the immap for any contained mmio_addr, so only
	 * allow the exact immap thing to be mapped
	 */
	if (vma->vm_pgoff != immap->vm_pgoff || length != immap->length) {	/* [한국어] (원 주석: 조회는 영역 안의 아무 주소로도 찾아 주므로 정확히 일치할 때만 허용한다) */
		rc = -ENXIO;	/* [한국어] 영역의 일부만 매핑하는 것을 막는다 */
		goto err_refcount;	/* [한국어] 참조를 되돌린다 */
	}

	vma->vm_pgoff = 0;	/* [한국어] 아래 매핑은 오프셋 0 에서 시작한다 */
	vma->vm_private_data = immap;	/* [한국어] 열림·닫힘 콜백이 이 값을 본다 */
	vma->vm_ops = &iommufd_vma_ops;	/* [한국어] 복제와 해제 시 참조를 맞춘다 */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);	/* [한국어] 장치 레지스터라 캐시하면 안 된다 */

	rc = io_remap_pfn_range(vma, vma->vm_start,	/* [한국어] 물리 주소를 그대로 */
				immap->mmio_addr >> PAGE_SHIFT, length,	/* [한국어] 페이지 번호로 넘겨 */
				vma->vm_page_prot);	/* [한국어] 사용자 주소 공간에 꽂는다 */
	if (rc)	/* [한국어] 실패면 */
		goto err_refcount;	/* [한국어] 참조를 되돌린다 */
	return 0;	/* [한국어] 매핑됐다 */

err_refcount:	/* [한국어] 잡아 둔 소유 객체 참조를 되돌린다 */
	refcount_dec(&immap->owner->users);	/* [한국어] 잡아 둔 참조를 놓고 */
	return rc;	/* [한국어] 실패 이유 */
}

static const struct file_operations iommufd_fops = {	/* [한국어] /dev/iommu 와 /dev/vfio/vfio 가 공유한다 */
	.owner = THIS_MODULE,	/* [한국어] 파일이 열려 있는 동안 모듈이 내려가지 않는다 */
	.open = iommufd_fops_open,	/* [한국어] 문맥을 만든다 */
	.release = iommufd_fops_release,	/* [한국어] 모든 객체를 허문다 */
	.unlocked_ioctl = iommufd_fops_ioctl,	/* [한국어] 명령 표로 갈라 보낸다 */
	.mmap = iommufd_fops_mmap,	/* [한국어] 노출된 MMIO 영역을 매핑한다 */
};

/**
 * iommufd_ctx_get - Get a context reference
 * @ictx: Context to get
 *
 * The caller must already hold a valid reference to ictx.
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iommufd_ctx_get - 문맥 참조를 하나 올린다
 *
 * @ictx: 문맥.
 *
 * 문맥의 수명이 곧 파일의 수명이라, 파일 참조를 올리는 것이 전부다.
 */
void iommufd_ctx_get(struct iommufd_ctx *ictx)
{
	get_file(ictx->file);	/* [한국어] 문맥의 수명이 곧 파일의 수명이다 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_ctx_get, "IOMMUFD");	/* [한국어] VFIO 등이 문맥을 붙잡을 때 */

/**
 * iommufd_ctx_from_file - Acquires a reference to the iommufd context
 * @file: File to obtain the reference from
 *
 * Returns a pointer to the iommufd_ctx, otherwise ERR_PTR. The struct file
 * remains owned by the caller and the caller must still do fput. On success
 * the caller is responsible to call iommufd_ctx_put().
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iommufd_ctx_from_file - 파일에서 문맥을 얻는다
 *
 * @file: iommufd 파일이어야 한다.
 * @return: 그 문맥, 아니면 ERR_PTR(-EBADFD).
 *
 * 연산 표를 견주어 정말 iommufd 파일인지 확인한다 — 사용자가 아무 fd 나
 * 줄 수 있기 때문이다.
 *
 * 원 주석이 참조 규칙을 밝힌다: 파일은 여전히 호출자 것이라 fput 을
 * 해야 하고, 성공하면 여기서 얻은 문맥은 iommufd_ctx_put 으로 놓아야 한다.
 */
struct iommufd_ctx *iommufd_ctx_from_file(struct file *file)
{
	struct iommufd_ctx *ictx;	/* [한국어] 얻을 문맥 */

	if (file->f_op != &iommufd_fops)	/* [한국어] 정말 iommufd 파일인지 — 사용자가 아무 fd 나 줄 수 있다 */
		return ERR_PTR(-EBADFD);	/* [한국어] 아니면 거절 */
	ictx = file->private_data;	/* [한국어] 열 때 넣어 둔 문맥 */
	iommufd_ctx_get(ictx);	/* [한국어] 호출자가 놓을 참조를 하나 올린다 */
	return ictx;	/* [한국어] 붙잡은 문맥 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_ctx_from_file, "IOMMUFD");	/* [한국어] 파일에서 문맥을 얻을 때 */

/**
 * iommufd_ctx_from_fd - Acquires a reference to the iommufd context
 * @fd: File descriptor to obtain the reference from
 *
 * Returns a pointer to the iommufd_ctx, otherwise ERR_PTR. On success
 * the caller is responsible to call iommufd_ctx_put().
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iommufd_ctx_from_fd - 파일 디스크립터에서 문맥을 얻는다
 *
 * @fd: iommufd 파일의 디스크립터.
 * @return: 그 문맥, 아니면 ERR_PTR.
 *
 * VFIO 가 이 함수로 사용자가 준 iommufd 를 붙잡는다.
 *
 * fget 이 잡은 파일 참조를 그대로 문맥 참조로 삼는다 — 원 주석대로 둘이
 * 같은 것이기 때문이다.
 */
struct iommufd_ctx *iommufd_ctx_from_fd(int fd)
{
	struct file *file;	/* [한국어] 디스크립터가 가리키는 파일 */

	file = fget(fd);	/* [한국어] 참조를 잡으며 연다 */
	if (!file)	/* [한국어] 없는 디스크립터면 */
		return ERR_PTR(-EBADF);	/* [한국어] 거절 */

	if (file->f_op != &iommufd_fops) {	/* [한국어] iommufd 파일이 아니면 */
		fput(file);	/* [한국어] 잡은 참조를 놓고 */
		return ERR_PTR(-EBADFD);	/* [한국어] 거절 */
	}
	/* fget is the same as iommufd_ctx_get() */
	return file->private_data;	/* [한국어] (원 주석: fget 이 곧 iommufd_ctx_get 과 같다) */
}
EXPORT_SYMBOL_NS_GPL(iommufd_ctx_from_fd, "IOMMUFD");	/* [한국어] 디스크립터에서 문맥을 얻을 때 */

/**
 * iommufd_ctx_put - Put back a reference
 * @ictx: Context to put back
 */
/*
 * [한국어]
 * (위 kdoc 과 함께 읽을 것)
 * iommufd_ctx_put - 문맥 참조를 놓는다
 *
 * @ictx: 놓을 문맥.
 *
 * 마지막이면 파일이 닫히고 release 가 모든 객체를 허문다.
 */
void iommufd_ctx_put(struct iommufd_ctx *ictx)
{
	fput(ictx->file);	/* [한국어] 마지막이면 파일이 닫히고 모든 객체가 허물어진다 */
}
EXPORT_SYMBOL_NS_GPL(iommufd_ctx_put, "IOMMUFD");	/* [한국어] 문맥을 놓을 때 */

#define IOMMUFD_FILE_OFFSET(_struct, _filep, _obj)                           \
	.file_offset = (offsetof(_struct, _filep) +                          \
			BUILD_BUG_ON_ZERO(!__same_type(                      \
				struct file *, ((_struct *)NULL)->_filep)) + \
			BUILD_BUG_ON_ZERO(offsetof(_struct, _obj)))	/* [한국어] 공통 객체가 맨 앞인지도 함께 확인한다 */

static const struct iommufd_object_ops iommufd_object_ops[] = {
	[IOMMUFD_OBJ_ACCESS] = {	/* [한국어] 커널 쪽 접근 */
		.destroy = iommufd_access_destroy_object,	/* [한국어] 고정을 풀고 IOAS 에서 뗀다 */
	},
	[IOMMUFD_OBJ_DEVICE] = {
		.pre_destroy = iommufd_device_pre_destroy,	/* [한국어] vdevice 가 잡은 참조를 먼저 끊는다 */
		.destroy = iommufd_device_destroy,	/* [한국어] 장치 묶임을 푼다 */
	},
	[IOMMUFD_OBJ_FAULT] = {
		.destroy = iommufd_fault_destroy,	/* [한국어] 폴트 큐를 비우고 답 없는 폴트에 응답한다 */
		IOMMUFD_FILE_OFFSET(struct iommufd_fault, common.filep, common.obj),	/* [한국어] 이 객체는 파일을 들고 있다 */
	},
	[IOMMUFD_OBJ_HW_QUEUE] = {
		.destroy = iommufd_hw_queue_destroy,	/* [한국어] 게스트용 하드웨어 큐를 거둔다 */
	},
	[IOMMUFD_OBJ_HWPT_PAGING] = {
		.destroy = iommufd_hwpt_paging_destroy,	/* [한국어] 도메인에서 매핑을 걷고 해제한다 */
		.abort = iommufd_hwpt_paging_abort,	/* [한국어] 확정 전 되돌리기는 절차가 다르다 */
	},
	[IOMMUFD_OBJ_HWPT_NESTED] = {
		.destroy = iommufd_hwpt_nested_destroy,	/* [한국어] 중첩 도메인을 해제한다 */
		.abort = iommufd_hwpt_nested_abort,	/* [한국어] 확정 전 되돌리기 */
	},
	[IOMMUFD_OBJ_IOAS] = {
		.destroy = iommufd_ioas_destroy,	/* [한국어] IOVA 공간을 통째로 허문다 */
	},
	[IOMMUFD_OBJ_VDEVICE] = {
		.destroy = iommufd_vdevice_destroy,	/* [한국어] 가상 장치를 거둔다 */
		.abort = iommufd_vdevice_abort,	/* [한국어] 확정 전 되돌리기 */
	},
	[IOMMUFD_OBJ_VEVENTQ] = {
		.destroy = iommufd_veventq_destroy,	/* [한국어] 이벤트 큐를 비운다 */
		.abort = iommufd_veventq_abort,	/* [한국어] 확정 전 되돌리기 */
		IOMMUFD_FILE_OFFSET(struct iommufd_veventq, common.filep, common.obj),	/* [한국어] 이 객체도 파일을 들고 있다 */
	},
	[IOMMUFD_OBJ_VIOMMU] = {
		.destroy = iommufd_viommu_destroy,	/* [한국어] vIOMMU 를 거둔다 */
	},
#ifdef CONFIG_IOMMUFD_TEST
	[IOMMUFD_OBJ_SELFTEST] = {
		.destroy = iommufd_selftest_destroy,	/* [한국어] 시험 객체 정리 */
	},
#endif
};

static struct miscdevice iommu_misc_dev = {	/* [한국어] /dev/iommu — 이 인터페이스의 본래 이름 */
	.minor = MISC_DYNAMIC_MINOR,	/* [한국어] 부번호는 커널이 고른다 */
	.name = "iommu",	/* [한국어] 장치 이름 */
	.fops = &iommufd_fops,	/* [한국어] 위의 연산 집합 */
	.nodename = "iommu",	/* [한국어] /dev 아래의 경로 */
	.mode = 0660,	/* [한국어] 기본은 소유자와 그룹만 */
};

static struct miscdevice vfio_misc_dev = {	/* [한국어] /dev/vfio/vfio — VFIO 컨테이너를 대신한다 */
	.minor = VFIO_MINOR,	/* [한국어] VFIO 가 쓰던 고정 부번호를 그대로 */
	.name = "vfio",	/* [한국어] 같은 이름으로 */
	.fops = &iommufd_fops,	/* [한국어] 같은 연산 집합 — open 이 두 장치를 가른다 */
	.nodename = "vfio/vfio",	/* [한국어] VFIO 가 쓰던 경로 */
	.mode = 0666,	/* [한국어] VFIO 의 관례를 따른다 */
};

/*
 * Used only by DMABUF, returns a valid struct device to use as a dummy struct
 * device for attachment.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_global_device - 대표 장치를 돌려준다
 *
 * @return: 이 모듈이 등록한 문자 장치.
 *
 * dma-buf 에 붙을 때는 어느 장치의 이름으로 붙을지 정해야 하는데, IOAS
 * 는 특정 장치에 매이지 않는다. 그래서 이 모듈 자신의 장치를 대신 쓴다.
 */
struct device *iommufd_global_device(void)
{
	return iommu_misc_dev.this_device;	/* [한국어] IOAS 는 특정 장치에 매이지 않아 모듈 자신의 장치를 쓴다 */
}

/*
 * [한국어]
 * iommufd_init - 모듈을 올리며 문자 장치를 등록한다
 *
 * @return: 0 성공, 음수면 실패.
 *
 * /dev/iommu 를 늘 등록하고, VFIO 컨테이너를 대신하도록 설정했으면
 * /dev/vfio/vfio 도 같은 연산으로 등록한다.
 *
 * 실패 경로가 역순으로 되돌린다.
 */
static int __init iommufd_init(void)
{
	int ret;	/* [한국어] 결과 */

	ret = misc_register(&iommu_misc_dev);	/* [한국어] /dev/iommu 를 등록한다 */
	if (ret)	/* [한국어] 실패면 */
		return ret;	/* [한국어] 모듈을 올릴 수 없다 */

	if (IS_ENABLED(CONFIG_IOMMUFD_VFIO_CONTAINER)) {	/* [한국어] VFIO 컨테이너를 대신하도록 설정했으면 */
		ret = misc_register(&vfio_misc_dev);	/* [한국어] /dev/vfio/vfio 도 같은 연산으로 */
		if (ret)	/* [한국어] 실패면 */
			goto err_misc;	/* [한국어] 앞의 등록을 되돌린다 */
	}
	ret = iommufd_test_init();	/* [한국어] 시험 환경(켜져 있을 때만) */
	if (ret)	/* [한국어] 실패면 */
		goto err_vfio_misc;	/* [한국어] 역순으로 되돌린다 */
	return 0;	/* [한국어] 올라갔다 */

err_vfio_misc:	/* [한국어] 시험 초기화가 실패했을 때 */
	if (IS_ENABLED(CONFIG_IOMMUFD_VFIO_CONTAINER))	/* [한국어] 등록했었으면 */
		misc_deregister(&vfio_misc_dev);	/* [한국어] 거둔다 */
err_misc:	/* [한국어] 두 번째 등록이 실패했을 때 */
	misc_deregister(&iommu_misc_dev);	/* [한국어] /dev/iommu 도 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iommufd_exit - 모듈을 내리며 등록을 거둔다
 *
 * 등록의 역순으로 해제한다.
 */
static void __exit iommufd_exit(void)
{
	iommufd_test_exit();	/* [한국어] 시험 환경 정리 */
	if (IS_ENABLED(CONFIG_IOMMUFD_VFIO_CONTAINER))	/* [한국어] 등록했었으면 */
		misc_deregister(&vfio_misc_dev);	/* [한국어] 거두고 */
	misc_deregister(&iommu_misc_dev);	/* [한국어] /dev/iommu 도 거둔다 */
}

module_init(iommufd_init);	/* [한국어] 모듈 적재 진입점 */
module_exit(iommufd_exit);	/* [한국어] 모듈 해제 진입점 */

#if IS_ENABLED(CONFIG_IOMMUFD_VFIO_CONTAINER)	/* [한국어] VFIO 컨테이너를 대신할 때만 */
MODULE_ALIAS_MISCDEV(VFIO_MINOR);	/* [한국어] 그 부번호로 자동 적재되게 한다 */
MODULE_ALIAS("devname:vfio/vfio");	/* [한국어] 그 경로 이름으로도 */
#endif
MODULE_IMPORT_NS("IOMMUFD_INTERNAL");	/* [한국어] driver.c 의 내부 심볼 */
MODULE_IMPORT_NS("IOMMUFD");	/* [한국어] 공개 심볼 */
MODULE_IMPORT_NS("DMA_BUF");	/* [한국어] dma-buf 출처의 페이지 */
MODULE_DESCRIPTION("I/O Address Space Management for passthrough devices");	/* [한국어] 모듈 설명 */
MODULE_LICENSE("GPL");	/* [한국어] 라이선스 선언 */
