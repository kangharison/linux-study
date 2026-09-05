// SPDX-License-Identifier: GPL-2.0
/*
 * Helpers for IOMMU drivers implementing SVA
 */
/*
 * [한국어 설명] SVA(Shared Virtual Addressing) 공통 헬퍼 (drivers/iommu/iommu-sva.c)
 *
 * === 파일의 역할 ===
 * 장치가 프로세스의 가상 주소를 그대로 쓰게 만드는 계층이다. 보통의 DMA 는 드라이버가
 * dma_map_* 으로 버퍼를 등록하고 장치에 별도의 DMA 주소를 알려 주지만, SVA 에서는
 * 장치가 CPU 와 똑같은 포인터를 받아 쓴다 — GPU 나 가속기에 malloc 으로 얻은 주소를
 * 그대로 넘길 수 있게 되는 것이다.
 *
 * 그것이 가능하려면 두 가지가 필요하다. 첫째, IOMMU 가 프로세스의 페이지 테이블을
 * 그대로 워크해야 한다. 그래서 SVA 도메인은 자기 페이지 테이블을 만들지 않고 mm 의
 * 것을 가리킨다. 둘째, 장치가 미매핑 주소에 접근했을 때 폴트를 처리해 페이지를
 * 채워 넣어야 한다 — CPU 의 데이터 폴트와 같은 일을 장치를 대신해 해 주는 것이며,
 * 이 파일의 절반이 그 처리기다.
 *
 * PASID 가 그 둘을 잇는다. 하나의 물리 장치가 여러 프로세스의 주소 공간을 동시에
 * 쓸 수 있어야 하므로, mm 마다 전역 PASID 를 하나 배정하고 그 값으로 어느 주소
 * 공간인지 구별한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 바인딩:  가속기 드라이버 iommu_sva_bind_device
 *            → [이 파일] mm 에 PASID 배정 → SVA 도메인 생성/재사용
 *              → iommu.c  iommu_attach_device_pasid → 벤더 드라이버 set_dev_pasid
 *            ← struct iommu_sva 핸들, iommu_sva_get_pasid 로 PASID 를 얻어 장치에 기록
 * 폴트:    IOMMU 하드웨어 PRI 이벤트 → io-pgfault → group->domain->iopf_handler
 *            → [이 파일] iommu_sva_iopf_handler → 워크큐 → handle_mm_fault
 *            → iopf_group_response 로 장치에 재개/실패를 알린다
 *
 * 폴트 처리가 워크큐로 넘어가는 것이 중요하다. handle_mm_fault 는 잠들 수 있고
 * 디스크 I/O 까지 유발할 수 있어 인터럽트 문맥에서 할 수 없기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: PASID 부착/해제(iommu_attach_device_pasid)와 전역 PASID 할당자를 쓴다.
 *   부착 핸들(iommu_attach_handle)을 통해 폴트가 어느 바인딩의 것인지 되짚는다.
 * - mm: struct mm_struct 에 iommu_mm 포인터를 매달아 PASID 와 도메인 목록을 보관한다.
 *   mm 이 사라질 때 mm_pasid_drop 이 불려 PASID 를 반납한다.
 * - mmu_notifier: CPU 쪽 페이지 테이블이 바뀌면 장치의 TLB 도 무효화해야 한다.
 *   벤더 드라이버가 notifier 를 등록하고, 커널 주소 범위 변경은 이 파일의
 *   iommu_sva_invalidate_kva_range 가 전파한다.
 * - 벤더 드라이버: domain_alloc_sva 로 mm 의 페이지 테이블을 가리키는 도메인을 만든다.
 *
 * === 주요 함수/구조체 요약 ===
 * - iommu_alloc_mm_data()   : mm 에 PASID 를 배정하고 SVA 상태를 매단다.
 * - iommu_sva_bind_device() : 장치와 주소 공간을 묶는다. 진입점.
 * - iommu_sva_unbind_device(): 그 짝. 참조가 0 이 되면 도메인까지 해제한다.
 * - iommu_sva_get_pasid()   : 장치에 프로그래밍할 PASID 값을 얻는다.
 * - iommu_sva_handle_mm()   : 장치 폴트를 CPU 폴트 처리기로 넘긴다.
 * - iommu_sva_iopf_handler(): 폴트를 워크큐로 넘기는 진입점.
 * - iommu_sva_domain_alloc(): mm 을 따르는 도메인을 만들고 폴트 처리기를 건다.
 */
#include <linux/mmu_context.h>	/* [한국어] mm_get_enqcmd_pasid 등 프로세스 문맥 조작 */
#include <linux/mmu_notifier.h>	/* [한국어] CPU 페이지 테이블 변경을 장치 TLB 로 전파하는 통지 기구 */
#include <linux/mutex.h>	/* [한국어] 바인딩 상태 전체를 지키는 전역 뮤텍스 */
#include <linux/sched/mm.h>	/* [한국어] mmget_not_zero/mmput — 폴트 처리 중 mm 이 사라지지 않게 붙잡는다 */
#include <linux/iommu.h>	/* [한국어] 도메인·PASID 부착 API */

#include "iommu-priv.h"	/* [한국어] iopf_group 등 서브시스템 내부 자료구조 */

static DEFINE_MUTEX(iommu_sva_lock);	/* [한국어] 바인딩·해제·도메인 목록을 모두 이 하나로 직렬화한다. 경로가 드물게 불리므로 세분화할 이유가 없다 */
static bool iommu_sva_present;	/* [한국어] 시스템에 SVA 바인딩이 하나라도 있는가. 커널 주소 무효화 경로가 목록을 훑기 전에 이 값만 보고 빠르게 빠져나가기 위한 것 */
static LIST_HEAD(iommu_sva_mms);	/* [한국어] SVA 로 묶인 모든 mm 의 목록. 커널 주소 범위 무효화를 전파할 대상이다 */
static struct iommu_domain *iommu_sva_domain_alloc(struct device *dev,	/* [한국어] 아래에서 정의되는 도메인 생성 함수의 전방 선언 */
						   struct mm_struct *mm);	/* [한국어] mm 을 따르는 도메인을 만든다 */

/* Allocate a PASID for the mm within range (inclusive) */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_alloc_mm_data - mm 에 PASID 를 배정하고 SVA 상태를 매단다
 *
 * @mm:     바인딩할 프로세스 주소 공간
 * @dev:    바인딩하려는 장치 (PASID 상한의 근거)
 * @return: SVA 상태 구조체, 실패하면 에러 포인터
 *
 * PASID 는 mm 단위이지 (mm, 장치) 단위가 아니다. 같은 프로세스가 여러 장치에
 * 바인딩되면 모두 같은 PASID 를 쓰고, 그래야 커널이 그 바인딩들을 하나로 다룰 수
 * 있다. 그래서 이미 배정된 mm 이면 그대로 재사용한다.
 *
 * 그 결과 -EOVERFLOW 라는 이상한 실패가 생긴다. 먼저 바인딩한 장치가 큰 PASID 를
 * 받았는데 나중 장치의 표현 범위가 좁으면, 값을 바꿀 수 없으므로 그 장치는 이
 * 프로세스에 붙을 수 없다.
 *
 * 마지막 smp_store_release 가 이 함수의 계약이다. mm->iommu_mm 을 락 없이 읽는
 * 경로가 있어, 공개가 초기화보다 먼저 보이면 반쯤 만들어진 구조체를 쓰게 된다.
 *
 * 실행 컨텍스트: 바인딩 경로. 전역 SVA 락 아래. 프로세스 문맥.
 *
 * 호출 체인: iommu_sva_bind_device → [이 함수] → iommu_alloc_global_pasid
 */
static struct iommu_mm_data *iommu_alloc_mm_data(struct mm_struct *mm, struct device *dev)
{
	struct iommu_mm_data *iommu_mm;	/* [한국어] mm 에 매달린 SVA 상태 (PASID 와 도메인 목록) */
	ioasid_t pasid;	/* [한국어] 새로 배정할 PASID */

	lockdep_assert_held(&iommu_sva_lock);	/* [한국어] 전역 락 아래에서만 — 두 CPU 가 같은 mm 에 동시에 PASID 를 배정하면 하나가 새 나간다 */

	if (!arch_pgtable_dma_compat(mm))	/* [한국어] 이 mm 의 페이지 테이블 형식을 IOMMU 가 워크할 수 없다 (아키텍처가 판정한다 — 예: 5단계 페이지 테이블을 IOMMU 가 못 따라가는 경우) */
		return ERR_PTR(-EBUSY);	/* [한국어] SVA 를 쓸 수 없는 프로세스 */

	iommu_mm = mm->iommu_mm;	/* [한국어] 이미 SVA 상태가 있는지 */
	/* Is a PASID already associated with this mm? */
	if (iommu_mm) {	/* [한국어] 다른 장치가 이미 이 mm 을 바인딩했다 */
		if (iommu_mm->pasid >= dev->iommu->max_pasids)	/* [한국어] 이미 배정된 PASID 가 이 장치의 표현 범위를 넘는다 */
			return ERR_PTR(-EOVERFLOW);	/* [한국어] 이 장치는 그 mm 에 붙을 수 없다 — PASID 는 전역이라 장치마다 다시 배정할 수 없다 */
		return iommu_mm;	/* [한국어] 기존 상태를 그대로 공유한다 */
	}

	iommu_mm = kzalloc_obj(struct iommu_mm_data);	/* [한국어] 새 SVA 상태 */
	if (!iommu_mm)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 바인딩 불가 */

	pasid = iommu_alloc_global_pasid(dev);	/* [한국어] 전역 풀에서 PASID 하나. 전역인 이유는 같은 mm 이 여러 장치에 붙을 때 모두 같은 값이어야 하기 때문이다 */
	if (pasid == IOMMU_PASID_INVALID) {	/* [한국어] PASID 고갈 또는 장치 미지원 */
		kfree(iommu_mm);	/* [한국어] 방금 만든 상태를 거둔다 */
		return ERR_PTR(-ENOSPC);	/* [한국어] 바인딩 불가 */
	}
	iommu_mm->pasid = pasid;	/* [한국어] 이 mm 의 PASID */
	iommu_mm->mm = mm;	/* [한국어] 역참조 — 무효화 전파가 목록에서 mm 을 꺼낼 때 쓴다 */
	INIT_LIST_HEAD(&iommu_mm->sva_domains);	/* [한국어] 이 mm 을 따르는 도메인들의 목록. 서로 다른 IOMMU 아래의 장치들이 각각 자기 도메인을 만들기 때문에 여럿이 될 수 있다 */
	/*
	 * Make sure the write to mm->iommu_mm is not reordered in front of
	 * initialization to iommu_mm fields. If it does, readers may see a
	 * valid iommu_mm with uninitialized values.
	 */
	smp_store_release(&mm->iommu_mm, iommu_mm);	/* [한국어] 공개는 마지막에, 배리어와 함께. 락 없이 mm->iommu_mm 을 읽는 경로가 있어, 이 대입이 위의 초기화보다 먼저 보이면 반쯤 만들어진 구조체를 쓰게 된다 (위 영어 주석) */
	return iommu_mm;	/* [한국어] PASID 가 배정된 SVA 상태 */
}

/**
 * iommu_sva_bind_device() - Bind a process address space to a device
 * @dev: the device
 * @mm: the mm to bind, caller must hold a reference to mm_users
 *
 * Create a bond between device and address space, allowing the device to
 * access the mm using the PASID returned by iommu_sva_get_pasid(). If a
 * bond already exists between @device and @mm, an additional internal
 * reference is taken. Caller must call iommu_sva_unbind_device()
 * to release each reference.
 *
 * On error, returns an ERR_PTR value.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_sva_bind_device - 장치와 프로세스 주소 공간을 묶는다
 *
 * @dev:    바인딩할 장치
 * @mm:     바인딩할 주소 공간 (호출자가 mm_users 참조를 들고 있어야 한다)
 * @return: 바인딩 핸들, 실패하면 에러 포인터
 *
 * 이 호출이 성공하면 장치는 iommu_sva_get_pasid 로 얻은 PASID 를 작업에 실어
 * 프로세스의 가상 주소를 그대로 쓸 수 있다. GPU 나 가속기에 malloc 포인터를
 * 넘기는 프로그래밍 모델이 여기서 성립한다.
 *
 * 재사용이 세 단계로 겹쳐 있다.
 *  1) PASID: mm 마다 하나. 이미 있으면 그대로 쓴다.
 *  2) 바인딩: 같은 장치·같은 PASID 에 이미 붙어 있으면 참조만 늘린다.
 *  3) 도메인: 같은 mm 을 따르는 도메인이 이미 있으면 부착만 시도한다. 같은 IOMMU
 *     아래의 다른 장치가 만든 것을 공유하게 되며, 실패하면 새로 만든다.
 *
 * 도메인이 여럿일 수 있는 이유는 시스템에 IOMMU 가 여럿일 수 있기 때문이다. 도메인의
 * 페이지 테이블 포맷은 만든 드라이버의 것이라 다른 IOMMU 에는 붙일 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 전역 SVA 락을 잡는다.
 *
 * 호출 체인: 가속기/GPU 드라이버 → [이 함수]
 *            → iommu_alloc_mm_data, iommu_sva_domain_alloc,
 *              iommu_attach_device_pasid
 */
struct iommu_sva *iommu_sva_bind_device(struct device *dev, struct mm_struct *mm)
{
	struct iommu_group *group = dev->iommu_group;	/* [한국어] 이 장치의 그룹 — 부착 핸들 조회에 필요하다 */
	struct iommu_attach_handle *attach_handle;	/* [한국어] 기존 바인딩이 있으면 그 핸들 */
	struct iommu_mm_data *iommu_mm;	/* [한국어] mm 의 SVA 상태 */
	struct iommu_domain *domain;	/* [한국어] 이 바인딩에 쓸 도메인 */
	struct iommu_sva *handle;	/* [한국어] 호출자에게 돌려줄 바인딩 핸들 */
	int ret;	/* [한국어] 각 단계의 결과 */

	if (!group)	/* [한국어] IOMMU 아래에 없는 장치 */
		return ERR_PTR(-ENODEV);	/* [한국어] SVA 를 쓸 수 없다 */

	mutex_lock(&iommu_sva_lock);	/* [한국어] 바인딩 상태 변경 구간 */

	/* Allocate mm->pasid if necessary. */
	iommu_mm = iommu_alloc_mm_data(mm, dev);	/* [한국어] mm 에 PASID 를 배정하거나 기존 것을 얻는다 */
	if (IS_ERR(iommu_mm)) {	/* [한국어] PASID 를 얻지 못했다 */
		ret = PTR_ERR(iommu_mm);	/* [한국어] 이유 추출 */
		goto out_unlock;	/* [한국어] 실패 */
	}

	/* A bond already exists, just take a reference`. */
	attach_handle = iommu_attach_handle_get(group, iommu_mm->pasid, IOMMU_DOMAIN_SVA);	/* [한국어] 이 장치·이 PASID 에 이미 SVA 도메인이 붙어 있는지 */
	if (!IS_ERR(attach_handle)) {	/* [한국어] 이미 바인딩되어 있다 */
		handle = container_of(attach_handle, struct iommu_sva, handle);	/* [한국어] 부착 핸들을 품고 있는 SVA 핸들로 되짚는다 */
		if (attach_handle->domain->mm != mm) {	/* [한국어] 같은 PASID 인데 다른 mm 이 붙어 있다 */
			ret = -EBUSY;	/* [한국어] 있을 수 없는 상태 — PASID 는 mm 마다 하나이므로 */
			goto out_unlock;	/* [한국어] 거절 */
		}
		refcount_inc(&handle->users);	/* [한국어] 같은 바인딩에 참조만 하나 더 (위 영어 주석) */
		mutex_unlock(&iommu_sva_lock);	/* [한국어] 락 해제 */
		return handle;	/* [한국어] 기존 핸들 재사용 */
	}

	if (PTR_ERR(attach_handle) != -ENOENT) {	/* [한국어] -ENOENT 는 '아직 없다'는 정상 결과다. 그 외는 진짜 오류 */
		ret = PTR_ERR(attach_handle);	/* [한국어] 이유 추출 */
		goto out_unlock;	/* [한국어] 실패 */
	}

	handle = kzalloc_obj(*handle);	/* [한국어] 새 바인딩 핸들 */
	if (!handle) {	/* [한국어] 할당 실패 */
		ret = -ENOMEM;	/* [한국어] 이유 기록 */
		goto out_unlock;	/* [한국어] 실패 */
	}

	/* Search for an existing domain. */
	list_for_each_entry(domain, &mm->iommu_mm->sva_domains, next) {	/* [한국어] 이 mm 을 따르는 도메인이 이미 있으면 재사용을 시도한다 */
		ret = iommu_attach_device_pasid(domain, dev, iommu_mm->pasid,	/* [한국어] 그 도메인에 이 장치를 붙여 본다 */
						&handle->handle);	/* [한국어] 부착 핸들 — 폴트가 이 바인딩으로 되돌아오는 통로다 */
		if (!ret) {	/* [한국어] 붙었다 — 같은 IOMMU 아래의 다른 장치가 만든 도메인을 공유하게 된다 */
			domain->users++;	/* [한국어] 이 도메인을 쓰는 바인딩 수 */
			goto out;	/* [한국어] 성공 */
		}
	}

	/* Allocate a new domain and set it on device pasid. */
	domain = iommu_sva_domain_alloc(dev, mm);	/* [한국어] 기존 도메인이 없거나 모두 호환되지 않았다 — 새로 만든다 */
	if (IS_ERR(domain)) {	/* [한국어] 드라이버가 SVA 도메인을 만들지 못했다 */
		ret = PTR_ERR(domain);	/* [한국어] 이유 추출 */
		goto out_free_handle;	/* [한국어] 핸들을 거둔다 */
	}

	ret = iommu_attach_device_pasid(domain, dev, iommu_mm->pasid,	/* [한국어] 새 도메인에 부착 */
					&handle->handle);	/* [한국어] 부착 핸들 */
	if (ret)	/* [한국어] 부착 실패 */
		goto out_free_domain;	/* [한국어] 도메인부터 되돌린다 */
	domain->users = 1;	/* [한국어] 첫 사용자 */

	if (list_empty(&iommu_mm->sva_domains)) {	/* [한국어] 이 mm 의 첫 SVA 도메인이면 */
		if (list_empty(&iommu_sva_mms))	/* [한국어] 시스템 전체의 첫 SVA 바인딩이면 */
			iommu_sva_present = true;	/* [한국어] 커널 주소 무효화 전파를 켠다 */
		list_add(&iommu_mm->mm_list_elm, &iommu_sva_mms);	/* [한국어] 전역 mm 목록에 등록 */
	}
	list_add(&domain->next, &iommu_mm->sva_domains);	/* [한국어] 이 mm 의 도메인 목록에 등록 */
out:	/* [한국어] 도메인 재사용과 신규 생성이 합류 */
	refcount_set(&handle->users, 1);	/* [한국어] 첫 참조 */
	mutex_unlock(&iommu_sva_lock);	/* [한국어] 락 해제 */
	handle->dev = dev;	/* [한국어] 해제 경로가 쓸 장치 포인터 */
	return handle;	/* [한국어] 이제 이 장치는 iommu_sva_get_pasid 로 얻은 PASID 로 프로세스 주소를 쓸 수 있다 */

out_free_domain:	/* [한국어] 부착 실패 경로 */
	iommu_domain_free(domain);	/* [한국어] 방금 만든 도메인 해제 */
out_free_handle:	/* [한국어] 도메인 생성 실패가 합류 */
	kfree(handle);	/* [한국어] 핸들 해제 */
out_unlock:	/* [한국어] 앞선 실패들이 모두 합류 */
	mutex_unlock(&iommu_sva_lock);	/* [한국어] 락 해제 */
	return ERR_PTR(ret);	/* [한국어] 실패 이유 */
}
EXPORT_SYMBOL_GPL(iommu_sva_bind_device);	/* [한국어] 가속기·GPU 드라이버가 부른다 */

/**
 * iommu_sva_unbind_device() - Remove a bond created with iommu_sva_bind_device
 * @handle: the handle returned by iommu_sva_bind_device()
 *
 * Put reference to a bond between device and address space. The device should
 * not be issuing any more transaction for this PASID. All outstanding page
 * requests for this PASID must have been flushed to the IOMMU.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_sva_unbind_device - 바인딩 참조 하나를 놓는다
 *
 * @handle: bind 가 돌려준 핸들
 *
 * 호출자의 계약이 엄격하다 (위 영어 주석). 이 시점에 장치는 그 PASID 로 새
 * 트랜잭션을 내고 있지 않아야 하고, 밀린 페이지 요청도 모두 IOMMU 로 흘러
 * 들어간 상태여야 한다. 그렇지 않으면 이미 해제된 도메인으로 폴트가 도착한다.
 *
 * 정리는 세 겹으로 벗겨진다 — 핸들 참조가 0 이 되면 하드웨어에서 PASID 를 거두고,
 * 도메인 사용자가 0 이 되면 도메인을 해제하고, mm 의 마지막 도메인이었으면 전역
 * 목록에서도 뺀다. PASID 자체는 mm 이 사라질 때 mm_pasid_drop 이 반납한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 전역 SVA 락을 잡는다.
 *
 * 호출 체인: 가속기/GPU 드라이버 → [이 함수] → iommu_detach_device_pasid
 */
void iommu_sva_unbind_device(struct iommu_sva *handle)
{
	struct iommu_domain *domain = handle->handle.domain;	/* [한국어] 이 바인딩의 SVA 도메인 */
	struct iommu_mm_data *iommu_mm = domain->mm->iommu_mm;	/* [한국어] 그 mm 의 SVA 상태 */
	struct device *dev = handle->dev;	/* [한국어] 바인딩된 장치 */

	mutex_lock(&iommu_sva_lock);	/* [한국어] 해제 구간 */
	if (!refcount_dec_and_test(&handle->users)) {	/* [한국어] 아직 다른 참조가 남아 있다 */
		mutex_unlock(&iommu_sva_lock);	/* [한국어] 락만 놓고 */
		return;	/* [한국어] 실제 해제는 마지막 참조에서 */
	}

	iommu_detach_device_pasid(domain, dev, iommu_mm->pasid);	/* [한국어] 하드웨어에서 이 PASID 를 거둔다. 위 영어 주석대로 이 시점에는 장치가 더 이상 이 PASID 로 트랜잭션을 내지 않고, 밀린 페이지 요청도 모두 처리된 상태여야 한다 */
	if (--domain->users == 0) {	/* [한국어] 이 도메인의 마지막 사용자였다 */
		list_del(&domain->next);	/* [한국어] mm 의 도메인 목록에서 제거 */
		if (list_empty(&iommu_mm->sva_domains)) {	/* [한국어] 이 mm 의 마지막 도메인이었다 */
			list_del(&iommu_mm->mm_list_elm);	/* [한국어] 전역 mm 목록에서도 제거 */
			if (list_empty(&iommu_sva_mms))	/* [한국어] 시스템에 SVA 바인딩이 하나도 남지 않았다 */
				iommu_sva_present = false;	/* [한국어] 커널 주소 무효화 전파를 끈다 — 그 경로의 비용을 완전히 없앤다 */
		}

		iommu_domain_free(domain);	/* [한국어] 도메인 해제. mm 참조(mmgrab)도 여기서 놓인다 */
	}

	mutex_unlock(&iommu_sva_lock);	/* [한국어] 락 해제 */
	kfree(handle);	/* [한국어] 바인딩 핸들 해제 */
}
EXPORT_SYMBOL_GPL(iommu_sva_unbind_device);	/* [한국어] bind 의 짝 */

/*
 * [한국어]
 * iommu_sva_get_pasid - 장치에 프로그래밍할 PASID 를 얻는다
 *
 * @handle: bind 가 돌려준 핸들
 * @return: 이 주소 공간의 PASID
 *
 * 드라이버는 이 값을 장치의 작업 서술자나 컨텍스트 레지스터에 넣는다. 그러면
 * 그 작업이 내는 DMA 에 PASID 가 실리고, IOMMU 가 그 값으로 어느 페이지 테이블을
 * 쓸지 고른다.
 *
 * 인텔의 ENQCMD 명령을 쓰는 가속기에서는 CPU 가 자동으로 현재 프로세스의 PASID 를
 * 실어 보내므로 드라이버가 따로 기록하지 않아도 된다 — 함수 이름의 enqcmd 가
 * 그것을 가리킨다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 가속기/GPU 드라이버 → [이 함수]
 */
u32 iommu_sva_get_pasid(struct iommu_sva *handle)
{
	struct iommu_domain *domain = handle->handle.domain;	/* [한국어] 이 바인딩의 도메인 */

	return mm_get_enqcmd_pasid(domain->mm);	/* [한국어] mm 에 배정된 PASID. 드라이버는 이 값을 장치의 작업 서술자에 넣어, 그 작업이 어느 주소 공간을 쓰는지 알린다. ENQCMD 명령을 쓰는 인텔 가속기에서는 CPU 가 자동으로 이 값을 실어 보낸다 */
}
EXPORT_SYMBOL_GPL(iommu_sva_get_pasid);	/* [한국어] 바인딩 뒤 장치를 프로그래밍할 때 부른다 */

/*
 * [한국어]
 * mm_pasid_drop - 프로세스가 사라질 때 PASID 를 반납한다
 *
 * @mm: 정리 중인 주소 공간
 *
 * mm 해제 경로가 부른다. 이 시점에는 모든 바인딩이 이미 풀려 있어야 하며,
 * 그렇지 않으면 재사용된 PASID 로 다른 프로세스의 주소 공간에 접근하는 최악의
 * 격리 붕괴가 일어난다.
 *
 * SVA 를 쓴 적이 없는 프로세스가 대부분이므로 NULL 검사에서 곧바로 돌아간다.
 *
 * 실행 컨텍스트: mm 해제 경로. 프로세스 문맥.
 *
 * 호출 체인: __mmdrop → [이 함수] → iommu_free_global_pasid
 */
void mm_pasid_drop(struct mm_struct *mm)
{
	struct iommu_mm_data *iommu_mm = mm->iommu_mm;	/* [한국어] 이 mm 의 SVA 상태 */

	if (!iommu_mm)	/* [한국어] SVA 를 쓴 적이 없는 프로세스 */
		return;	/* [한국어] 할 일 없음 */

	iommu_free_global_pasid(iommu_mm->pasid);	/* [한국어] PASID 를 전역 풀에 돌려준다. 이 시점에는 모든 바인딩이 이미 해제되어 있어야 한다 — 남아 있으면 재사용된 PASID 로 다른 프로세스의 주소 공간에 접근하게 된다 */
	kfree(iommu_mm);	/* [한국어] SVA 상태 해제 */
}

/*
 * I/O page fault handler for SVA
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_sva_handle_mm - 장치가 낸 페이지 폴트를 CPU 폴트 처리기로 넘긴다
 *
 * @fault:  하드웨어가 보고한 폴트
 * @mm:     그 PASID 에 대응하는 프로세스 주소 공간
 * @return: 장치에 돌려줄 응답 코드
 *
 * SVA 의 나머지 절반이다. 장치가 아직 매핑되지 않은 주소에 접근하면 IOMMU 가 그
 * 트랜잭션을 세우고 페이지 요청(PRI)을 보내는데, 이 함수가 그것을 받아 CPU 가
 * 데이터 폴트를 처리하듯 페이지를 채워 넣는다. 요구 페이징, COW 복사, 스왑 인이
 * 모두 일어날 수 있어 이 경로는 잠들 수 있다.
 *
 * 권한 검사가 실질적인 보안 경계다. 하드웨어가 알린 접근 종류(읽기/쓰기/실행)를
 * VMA 권한과 대조해, 허용되지 않으면 페이지를 채우지 않고 실패로 응답한다 —
 * CPU 였다면 SIGSEGV 가 났을 상황이다. 장치가 프로세스 주소 공간을 쓴다고 해서
 * 그 프로세스보다 큰 권한을 갖지는 않는다.
 *
 * FAULT_FLAG_REMOTE 가 붙는 것에 주의할 것. 워크큐 스레드가 남의 mm 을 대신
 * 폴트시키는 것이므로 현재 CPU 문맥과 무관하다는 표시가 필요하다.
 *
 * 실행 컨텍스트: 워크큐. 프로세스 문맥, 반드시 잠들 수 있어야 한다.
 *
 * 호출 체인: iommu_sva_handle_iopf → [이 함수] → handle_mm_fault
 */
static enum iommu_page_response_code
iommu_sva_handle_mm(struct iommu_fault *fault, struct mm_struct *mm)
{
	vm_fault_t ret;	/* [한국어] handle_mm_fault 의 결과 */
	struct vm_area_struct *vma;	/* [한국어] 폴트 주소가 속한 VMA */
	unsigned int access_flags = 0;	/* [한국어] 장치가 요구한 접근 권한 (VM_READ/WRITE/EXEC) */
	unsigned int fault_flags = FAULT_FLAG_REMOTE;	/* [한국어] REMOTE 는 '현재 CPU 문맥이 아닌 남의 mm 에 대한 폴트'라는 뜻이다. 워크큐 스레드가 남의 주소 공간을 대신 폴트시키는 것이므로 반드시 필요하다 */
	struct iommu_fault_page_request *prm = &fault->prm;	/* [한국어] 하드웨어가 준 페이지 요청 정보 */
	enum iommu_page_response_code status = IOMMU_PAGE_RESP_INVALID;	/* [한국어] 기본은 실패. 아래 검사를 모두 통과해야 성공으로 바뀐다 */

	if (!(prm->flags & IOMMU_FAULT_PAGE_REQUEST_PASID_VALID))	/* [한국어] PASID 가 없는 폴트는 SVA 가 다룰 수 없다 */
		return status;	/* [한국어] 장치에 실패를 알린다 */

	if (!mmget_not_zero(mm))	/* [한국어] 프로세스가 이미 종료 중이면 mm 참조를 얻지 못한다 */
		return status;	/* [한국어] 죽어 가는 주소 공간에 페이지를 채울 이유가 없다 */

	mmap_read_lock(mm);	/* [한국어] VMA 를 읽는 동안 주소 공간 배치가 바뀌지 않게 */

	vma = vma_lookup(mm, prm->addr);	/* [한국어] 폴트 주소가 속한 매핑을 찾는다 */
	if (!vma)	/* [한국어] 매핑되지 않은 주소 — 장치가 잘못된 포인터를 썼다 */
		/* Unmapped area */
		goto out_put_mm;	/* [한국어] 실패로 응답 */

	if (prm->perm & IOMMU_FAULT_PERM_READ)	/* [한국어] 장치가 읽으려 했다 */
		access_flags |= VM_READ;	/* [한국어] VMA 권한과 비교할 요구 권한에 추가 */

	if (prm->perm & IOMMU_FAULT_PERM_WRITE) {	/* [한국어] 장치가 쓰려 했다 */
		access_flags |= VM_WRITE;	/* [한국어] 쓰기 권한 요구 */
		fault_flags |= FAULT_FLAG_WRITE;	/* [한국어] 폴트 처리기에도 쓰기임을 알린다 — COW 페이지를 실제로 복사하게 만든다 */
	}

	if (prm->perm & IOMMU_FAULT_PERM_EXEC) {	/* [한국어] 장치가 명령어를 가져오려 했다 (GPU 셰이더 등) */
		access_flags |= VM_EXEC;	/* [한국어] 실행 권한 요구 */
		fault_flags |= FAULT_FLAG_INSTRUCTION;	/* [한국어] 명령어 폴트임을 알린다 */
	}

	if (!(prm->perm & IOMMU_FAULT_PERM_PRIV))	/* [한국어] 특권 접근이 아니면 */
		fault_flags |= FAULT_FLAG_USER;	/* [한국어] 사용자 접근으로 처리한다 — 커널 전용 매핑에 닿으면 거절된다 */

	if (access_flags & ~vma->vm_flags)	/* [한국어] 요구 권한 중 VMA 가 허용하지 않는 것이 있다 */
		/* Access fault */
		goto out_put_mm;	/* [한국어] 접근 위반 — 페이지를 채워 주면 안 된다. CPU 라면 SIGSEGV 가 날 상황이다 */

	ret = handle_mm_fault(vma, prm->addr, fault_flags, NULL);	/* [한국어] CPU 의 페이지 폴트 처리기를 그대로 부른다. 요구 페이징, COW 복사, 스왑 인이 모두 여기서 일어나며, 그래서 이 경로가 잠들 수 있고 워크큐가 필요한 것이다 */
	status = ret & VM_FAULT_ERROR ? IOMMU_PAGE_RESP_INVALID :	/* [한국어] 폴트 처리가 실패했으면 */
		IOMMU_PAGE_RESP_SUCCESS;	/* [한국어] 성공했으면 장치에 재개를 허락한다 */

out_put_mm:	/* [한국어] 성공·실패 공통 출구 */
	mmap_read_unlock(mm);	/* [한국어] 주소 공간 락 해제 */
	mmput(mm);	/* [한국어] mm 참조 반납 — 이 호출로 프로세스가 실제로 정리될 수도 있다 */

	return status;	/* [한국어] 장치에 돌려줄 응답 코드 */
}

/*
 * [한국어]
 * iommu_sva_handle_iopf - 폴트 그룹을 순서대로 처리하고 응답한다 (워크큐 본체)
 *
 * @work: 폴트 그룹에 박힌 워크 구조체
 *
 * 하드웨어는 폴트를 하나씩이 아니라 그룹으로 보낸다. 한 트랜잭션이 여러 페이지에
 * 걸칠 수 있기 때문이며, 그래서 응답도 그룹 단위다.
 *
 * 오류가 끈적한 것이 의도된 동작이다 (위 영어 주석). 그룹 안에서 하나라도 실패하면
 * 뒤는 처리하지 않는데, 어차피 장치가 그 트랜잭션을 이어 갈 수 없으므로 나머지
 * 페이지를 채워 봐야 낭비이기 때문이다.
 *
 * 실행 컨텍스트: 워크큐. 프로세스 문맥, 잠들 수 있다.
 *
 * 호출 체인: 워크큐 → [이 함수] → iommu_sva_handle_mm, iopf_group_response
 */
static void iommu_sva_handle_iopf(struct work_struct *work)
{
	struct iopf_fault *iopf;	/* [한국어] 그룹 안의 개별 폴트 */
	struct iopf_group *group;	/* [한국어] 한 번에 처리할 폴트 묶음 */
	enum iommu_page_response_code status = IOMMU_PAGE_RESP_SUCCESS;	/* [한국어] 낙관적으로 시작 */

	group = container_of(work, struct iopf_group, work);	/* [한국어] 워크 구조체에서 폴트 그룹으로 되짚는다 */
	list_for_each_entry(iopf, &group->faults, list) {	/* [한국어] 하드웨어가 묶어 보낸 폴트들을 순서대로 */
		/*
		 * For the moment, errors are sticky: don't handle subsequent
		 * faults in the group if there is an error.
		 */
		if (status != IOMMU_PAGE_RESP_SUCCESS)	/* [한국어] 앞에서 하나라도 실패했으면 */
			break;	/* [한국어] 뒤는 처리하지 않는다. 오류가 끈적하게 남는 것이 의도된 동작이다 — 그룹은 하나의 트랜잭션이라 일부만 성공시켜도 장치가 이어서 진행할 수 없다 (위 영어 주석) */

		status = iommu_sva_handle_mm(&iopf->fault,	/* [한국어] 이 폴트를 CPU 폴트 처리기로 넘긴다 */
					     group->attach_handle->domain->mm);	/* [한국어] 부착 핸들에서 어느 프로세스의 주소 공간인지 되짚는다 — PASID 를 mm 으로 되돌리는 연결이 여기 있다 */
	}

	iopf_group_response(group, status);	/* [한국어] 장치에 응답을 보낸다. SUCCESS 면 장치가 멈췄던 지점부터 재개하고, INVALID 면 오류로 처리한다 */
	iopf_free_group(group);	/* [한국어] 그룹 해제 */
}

/*
 * [한국어]
 * iommu_sva_iopf_handler - SVA 도메인의 폴트 진입점 (워크큐로 넘긴다)
 *
 * @group:  처리할 폴트 그룹
 * @return: 0 이면 접수됨, -EBUSY 면 큐잉 실패
 *
 * domain->iopf_handler 로 등록되어 있어, 이 도메인에서 나는 모든 페이지 요청이
 * 여기로 온다. 하는 일은 워크큐로 넘기는 것뿐인데, 그것이 요점이다 — 실제 처리는
 * handle_mm_fault 를 부르므로 잠들 수 있고, 이 함수는 폴트 보고 경로(대개 인터럽트
 * 문맥)에서 불리기 때문이다.
 *
 * 큐를 장치별 fault_param 이 들고 있다는 점도 중요하다. 한 장치의 폴트들이 같은
 * 워크큐에서 순서대로 처리되어, 같은 주소에 대한 중복 폴트가 겹쳐 돌지 않는다.
 *
 * 실행 컨텍스트: 폴트 보고 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: io-pgfault 계층 → domain->iopf_handler == [이 함수]
 */
static int iommu_sva_iopf_handler(struct iopf_group *group)
{
	struct iommu_fault_param *fault_param = group->fault_param;	/* [한국어] 이 장치의 폴트 처리 문맥 (워크큐를 들고 있다) */

	INIT_WORK(&group->work, iommu_sva_handle_iopf);	/* [한국어] 실제 처리를 워크큐로 넘긴다. handle_mm_fault 는 잠들 수 있고 디스크 I/O 까지 유발하므로 폴트 인터럽트 문맥에서 할 수 없다 */
	if (!queue_work(fault_param->queue->wq, &group->work))	/* [한국어] 이미 큐에 들어 있으면 실패한다 (있을 수 없는 상태) */
		return -EBUSY;	/* [한국어] 상위가 장치에 실패를 알린다 */

	return 0;	/* [한국어] 처리는 나중에 워크큐에서 */
}

/*
 * [한국어]
 * iommu_sva_domain_alloc - 프로세스 페이지 테이블을 가리키는 도메인을 만든다
 *
 * @dev:    이 도메인을 쓸 장치
 * @mm:     따라갈 주소 공간
 * @return: SVA 도메인, 실패하면 에러 포인터
 *
 * 보통의 도메인은 자기 페이지 테이블을 만들어 iommu_map 으로 채우지만, SVA 도메인은
 * mm 의 페이지 테이블을 그대로 가리킨다. 그래서 CPU 가 mmap/munmap 을 하면 장치도
 * 즉시 같은 것을 보게 되고, 별도의 매핑 동기화가 필요 없다. 벤더 드라이버의
 * domain_alloc_sva 가 그 연결(예: 인텔의 PASID 항목에 mm->pgd 를 기록)을 만든다.
 *
 * mmget 이 아니라 mmgrab 을 쓰는 것에 주의할 것. 페이지 테이블만 살아 있으면 되고
 * 사용자 매핑까지 유지할 필요는 없다 — 프로세스가 종료를 시작해도 도메인은
 * 유효하게 남아 있다가 unbind 에서 정리된다.
 *
 * iopf_handler 를 여기서 거는 것이 폴트 경로의 시작점이다.
 *
 * 실행 컨텍스트: 바인딩 경로. 전역 SVA 락 아래. 프로세스 문맥.
 *
 * 호출 체인: iommu_sva_bind_device → [이 함수] → ops->domain_alloc_sva
 */
static struct iommu_domain *iommu_sva_domain_alloc(struct device *dev,
						   struct mm_struct *mm)
{
	const struct iommu_ops *ops = dev_iommu_ops(dev);	/* [한국어] 이 장치의 IOMMU 드라이버 */
	struct iommu_domain *domain;	/* [한국어] 만들 SVA 도메인 */

	if (!ops->domain_alloc_sva)	/* [한국어] 드라이버가 SVA 를 지원하지 않는다 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 이 장치로는 SVA 를 쓸 수 없다 */

	domain = ops->domain_alloc_sva(dev, mm);	/* [한국어] 드라이버가 mm 의 페이지 테이블을 가리키는 도메인을 만든다. 자기 페이지 테이블을 만들지 않는 것이 SVA 도메인의 본질이며, 그래서 CPU 가 매핑을 바꾸면 장치도 즉시 같은 것을 본다 */
	if (IS_ERR(domain))	/* [한국어] 생성 실패 */
		return domain;	/* [한국어] 에러 포인터를 그대로 */

	domain->type = IOMMU_DOMAIN_SVA;	/* [한국어] 코어가 이 도메인을 SVA 로 인식하게 한다 */
	domain->cookie_type = IOMMU_COOKIE_SVA;	/* [한국어] cookie 자리에 mm 이 들어 있음을 표시 — iommu_domain_free 가 이 값을 보고 mmdrop 을 부른다 */
	mmgrab(mm);	/* [한국어] mm 구조체를 붙잡는다. mmget 이 아니라 mmgrab 인 것에 주의 — 페이지 테이블만 살아 있으면 되고 사용자 매핑까지 유지할 필요는 없다 */
	domain->mm = mm;	/* [한국어] 이 도메인이 따르는 주소 공간 */
	domain->owner = ops;	/* [한국어] 호환성 검사(domain_iommu_ops_compatible)의 근거 */
	domain->iopf_handler = iommu_sva_iopf_handler;	/* [한국어] 이 도메인에서 나는 폴트가 이 파일로 오게 한다 */

	return domain;	/* [한국어] SVA 도메인 완성 */
}

/*
 * [한국어]
 * iommu_sva_invalidate_kva_range - 커널 주소 범위 변경을 모든 SVA 장치에 전파한다
 *
 * @start: 무효화할 범위의 시작 (커널 가상 주소)
 * @end:   끝
 *
 * 사용자 주소는 mmu_notifier 가 mm 단위로 알아서 전파하지만, 커널 주소는 모든 mm 이
 * 공유하므로 그 경로를 탈 수 없다. vmalloc 영역이 해제되고 재사용되는 것 같은
 * 변경은 SVA 로 묶인 모든 주소 공간에 전파해야 한다 — 장치가 커널 주소를 쓰는
 * 구성(일부 가속기)에서 옛 번역이 남으면 해제된 메모리에 닿는다.
 *
 * iommu_sva_present 를 먼저 보는 것이 실질적으로 중요하다. SVA 를 쓰지 않는 대부분의
 * 시스템에서 이 함수는 전역 하나를 읽고 곧바로 돌아가며, 그래야 vmalloc 경로에
 * 부담을 주지 않는다.
 *
 * 실행 컨텍스트: vmalloc 해제 경로. 프로세스 문맥.
 *
 * 호출 체인: vmalloc 무효화 → [이 함수]
 *            → mmu_notifier_arch_invalidate_secondary_tlbs
 */
void iommu_sva_invalidate_kva_range(unsigned long start, unsigned long end)
{
	struct iommu_mm_data *iommu_mm;	/* [한국어] 목록 순회 커서 */

	guard(mutex)(&iommu_sva_lock);	/* [한국어] 범위를 벗어나면 자동 해제되는 락 */
	if (!iommu_sva_present)	/* [한국어] SVA 바인딩이 하나도 없으면 */
		return;	/* [한국어] 목록을 훑을 것도 없다. 이 빠른 탈출이 없으면 커널 주소를 바꿀 때마다 락을 잡게 된다 */

	list_for_each_entry(iommu_mm, &iommu_sva_mms, mm_list_elm)	/* [한국어] SVA 로 묶인 모든 mm 에 대해 */
		mmu_notifier_arch_invalidate_secondary_tlbs(iommu_mm->mm, start, end);	/* [한국어] 장치 TLB 를 무효화한다. 커널 주소 범위는 모든 mm 이 공유하므로, vmalloc 영역이 바뀌면 SVA 로 묶인 모든 주소 공간에 전파해야 한다 */
}
