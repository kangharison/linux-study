// SPDX-License-Identifier: GPL-2.0
/*
 * Handle device page faults
 *
 * Copyright (C) 2020 ARM Ltd.
 */

/*
 * [한국어 설명] 장치 페이지 폴트(I/O Page Fault) 전달 계층 (drivers/iommu/io-pgfault.c)
 *
 * === 파일의 역할 ===
 * 장치가 매핑되지 않은 주소에 접근했을 때 하드웨어가 보내는 페이지 요청을 받아,
 * 그 주소 공간을 소유한 도메인의 처리기로 배달하는 계층이다. SVA 에서는 그
 * 처리기가 handle_mm_fault 를 불러 페이지를 채우고, 장치는 멈췄던 지점부터
 * 재개한다 — CPU 의 데이터 폴트와 같은 일이 장치에서 일어나는 것이다.
 *
 * PCIe 는 이 기능을 PRI(Page Request Interface)라 부른다. 핵심 제약이 두 가지 있고,
 * 이 파일의 구조 대부분이 거기서 나온다.
 *
 *  1) 요청은 '그룹' 단위다. 하나의 장치 트랜잭션이 여러 페이지에 걸칠 수 있어
 *     하드웨어가 여러 요청을 같은 그룹 ID 로 묶어 보내고, 마지막 것에 LAST_PAGE
 *     표시를 단다. 응답도 그룹 단위로 한 번만 보내야 한다. 그래서 이 파일은
 *     마지막이 오기 전까지 부분 폴트를 모아 두었다가(partial 목록) 한꺼번에
 *     처리한다.
 *  2) 응답은 반드시 보내야 한다. 응답하지 않으면 장치가 그 트랜잭션에 영원히
 *     멈춰 있게 되므로, 처리기가 없거나 실패한 경우에도 실패 응답을 보낸다.
 *     err_abort/err_bad_iopf 경로가 그 보장이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름: IOMMU 하드웨어 PRI 인터럽트
 *         → 벤더 드라이버의 스레드 IRQ 핸들러
 *           → [이 파일] iommu_report_device_fault
 *             → find_fault_handler : PASID 로 부착 핸들을, 거기서 도메인을 찾는다
 *             → iopf_group_alloc   : 부분 폴트를 모아 그룹을 만든다
 *             → domain->iopf_handler (SVA 면 iommu-sva.c, 사용자면 iommufd)
 *               → 워크큐에서 실제 처리 후 iopf_group_response
 *                 → ops->page_response 로 장치에 재개/실패를 알린다
 *
 * 이 파일 자체는 폴트를 처리하지 않는다. 배달과 응답 보장만 맡고, 무엇을 할지는
 * 도메인이 정한다 — 그 분리 덕분에 SVA 와 iommufd 가 같은 배달 경로를 공유한다.
 *
 * === 타 모듈과의 연결 ===
 * - 벤더 드라이버: PRI 이벤트를 받아 iommu_report_device_fault 로 올리고,
 *   page_response 콜백으로 응답을 하드웨어에 내려보낸다.
 * - iommu.c: iommu_attach_handle_get 으로 PASID → 도메인을 되짚는다. 그 조회가
 *   xa_lock 만 쓰는 이유가 여기 있다 — 폴트 경로는 그룹 락을 기다릴 수 없다.
 * - iommu-sva.c: 가장 흔한 소비자. 도메인의 iopf_handler 로 등록되어 있다.
 * - iommufd: 사용자 공간이 폴트를 처리하는 경로.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct iommu_fault_param : 장치 하나의 폴트 상태 (워크큐, 부분 폴트 목록,
 *                              응답 대기 그룹 목록). RCU 로 보호된다.
 * - struct iopf_group        : 함께 응답해야 하는 폴트 묶음.
 * - iommu_report_device_fault(): 드라이버가 폴트를 올리는 진입점.
 * - iopf_group_response()    : 그룹에 응답한다. 중복 응답을 막는 것이 핵심.
 * - iopf_queue_add/remove_device(): 장치를 폴트 큐에 붙이고 뗀다.
 * - iopf_queue_flush_dev()   : PASID 반납 전에 밀린 폴트를 모두 처리시킨다.
 */
#include <linux/iommu.h>	/* [한국어] 도메인·부착 핸들 API */
#include <linux/list.h>	/* [한국어] 부분 폴트와 대기 그룹 목록 */
#include <linux/sched/mm.h>	/* [한국어] 폴트 처리기가 쓰는 mm 조작 */
#include <linux/slab.h>	/* [한국어] 폴트·그룹 객체 할당 */
#include <linux/workqueue.h>	/* [한국어] 실제 처리를 잠들 수 있는 문맥으로 넘기는 큐 */

#include "iommu-priv.h"	/* [한국어] iopf_group 등 서브시스템 내부 자료구조 */

/*
 * Return the fault parameter of a device if it exists. Otherwise, return NULL.
 * On a successful return, the caller takes a reference of this parameter and
 * should put it after use by calling iopf_put_dev_fault_param().
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iopf_get_dev_fault_param - 장치의 폴트 상태에 참조를 잡는다
 *
 * @dev:    폴트를 낸 장치
 * @return: 참조가 하나 늘어난 폴트 상태, 없으면 NULL
 *
 * RCU 를 쓰는 이유가 이 함수의 전부다. 폴트 보고는 인터럽트 문맥에서 오므로
 * 뮤텍스를 기다릴 수 없는데, 그 사이 다른 CPU 가 iopf_queue_remove_device 로
 * 상태를 없앨 수 있다.
 *
 * refcount_inc_not_zero 가 그 경쟁을 정확히 처리한다. 이미 0 이 되었다면 제거가
 * 시작된 것이므로 없는 것으로 취급하고, 0 이 아니라면 참조를 잡았으므로 RCU
 * 구간을 벗어나도 안전하다.
 *
 * 실행 컨텍스트: 폴트 보고 경로. 인터럽트 문맥 가능.
 *
 * 호출 체인: iommu_report_device_fault → [이 함수]
 */
static struct iommu_fault_param *iopf_get_dev_fault_param(struct device *dev)
{
	struct dev_iommu *param = dev->iommu;	/* [한국어] 장치별 IOMMU 상태 */
	struct iommu_fault_param *fault_param;	/* [한국어] 이 장치의 폴트 상태 */

	rcu_read_lock();	/* [한국어] fault_param 은 RCU 로 보호된다 — 폴트 경로가 뮤텍스를 기다릴 수 없기 때문 */
	fault_param = rcu_dereference(param->fault_param);	/* [한국어] 현재 포인터를 읽는다 */
	if (fault_param && !refcount_inc_not_zero(&fault_param->users))	/* [한국어] 이미 0 이 된(제거 중인) 상태라면 참조를 늘리지 않는다 */
		fault_param = NULL;	/* [한국어] 없는 것으로 취급 */
	rcu_read_unlock();	/* [한국어] 참조를 잡았으므로 이제 RCU 구간을 벗어나도 안전하다 */

	return fault_param;	/* [한국어] 호출자가 iopf_put_dev_fault_param 으로 놓아야 한다 (위 영어 주석) */
}

/* Caller must hold a reference of the fault parameter. */
/*
 * [한국어] (위 영어 주석에 이어)
 * iopf_put_dev_fault_param - 폴트 상태 참조를 놓는다
 *
 * @fault_param: 놓을 상태
 *
 * kfree_rcu 로 유예 해제하는 것이 요점이다. 참조가 0 이 되는 순간에도 다른 CPU 가
 * rcu_dereference 로 이 포인터를 읽고 있을 수 있으므로, RCU 유예 기간이 지난 뒤에야
 * 메모리를 반납한다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: __iopf_free_group, iopf_queue_remove_device 등 → [이 함수]
 */
static void iopf_put_dev_fault_param(struct iommu_fault_param *fault_param)
{
	if (refcount_dec_and_test(&fault_param->users))	/* [한국어] 마지막 참조였다 */
		kfree_rcu(fault_param, rcu);	/* [한국어] RCU 유예 기간 뒤에 해제한다 — 지금 이 순간에도 다른 CPU 가 rcu_dereference 로 읽고 있을 수 있다 */
}

/*
 * [한국어]
 * __iopf_free_group - 그룹의 내용물을 해제한다 (구조체 자체는 제외)
 *
 * @group: 해제할 그룹
 *
 * 마지막 폴트를 건너뛰는 것에 주의할 것. 그것은 그룹 구조체 안의 last_fault 필드로
 * 박혀 있어 따로 할당된 적이 없다 — 부분 폴트만 개별 할당이다.
 *
 * 구조체 해제를 분리한 이유는 abort_group 때문이다. 메모리가 없을 때 쓰는 스택
 * 예비본은 kfree 할 수 없으므로, 내용물 정리와 구조체 해제를 나눠 두었다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: iopf_free_group, iommu_report_device_fault 의 abort 경로 → [이 함수]
 */
static void __iopf_free_group(struct iopf_group *group)
{
	struct iopf_fault *iopf, *next;	/* [한국어] 해제하며 순회하므로 _safe 판 */

	list_for_each_entry_safe(iopf, next, &group->faults, list) {	/* [한국어] 그룹에 모인 폴트들 */
		if (!(iopf->fault.prm.flags & IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE))	/* [한국어] 마지막 폴트는 그룹 구조체 안에 박혀 있어(last_fault) 따로 할당된 것이 아니다 */
			kfree(iopf);	/* [한국어] 부분 폴트만 개별 할당이므로 그것만 해제한다 */
	}

	/* Pair with iommu_report_device_fault(). */
	iopf_put_dev_fault_param(group->fault_param);	/* [한국어] iommu_report_device_fault 가 잡았던 참조를 놓는다 (위 영어 주석) */
}

/*
 * [한국어]
 * iopf_free_group - 그룹을 통째로 해제한다
 *
 * @group: 해제할 그룹
 *
 * 도메인의 폴트 처리기가 응답을 보낸 뒤 부른다. 이 시점에 그룹이 들고 있던
 * 폴트 상태 참조도 함께 풀리며, 그것이 마지막 참조였다면 장치의 폴트 상태 자체가
 * RCU 유예 뒤에 사라진다.
 *
 * 실행 컨텍스트: 워크큐 또는 폴트 경로.
 *
 * 호출 체인: iommu-sva 의 iommu_sva_handle_iopf, iommufd → [이 함수]
 */
void iopf_free_group(struct iopf_group *group)
{
	__iopf_free_group(group);	/* [한국어] 내용물 해제 */
	kfree(group);	/* [한국어] 그룹 구조체 자체. abort_group 은 스택에 있어 이 경로로 오지 않는다 */
}
EXPORT_SYMBOL_GPL(iopf_free_group);	/* [한국어] 폴트 처리기가 처리를 마친 뒤 부른다 */

/* Non-last request of a group. Postpone until the last one. */
/*
 * [한국어] (위 영어 주석에 이어)
 * report_partial_fault - 그룹의 마지막이 아닌 폴트를 보관해 둔다
 *
 * @fault_param: 이 장치의 폴트 상태
 * @fault:       보관할 폴트
 * @return:      0 성공, -ENOMEM 이면 보관 실패
 *
 * PCIe 는 하나의 장치 트랜잭션이 여러 페이지에 걸치면 요청을 여러 개로 나눠 보내고
 * 마지막에만 LAST_PAGE 표시를 단다. 응답은 그룹 단위로 한 번만 해야 하므로,
 * 마지막이 올 때까지 앞의 것들을 여기 모아 둔다.
 *
 * 복사가 필요한 이유는 원본의 수명 때문이다. 드라이버가 넘긴 폴트는 대개 그
 * 스택이나 하드웨어 큐에 있어 이 호출이 끝나면 사라진다.
 *
 * 실행 컨텍스트: 폴트 보고 경로. 뮤텍스를 잡으므로 스레드 IRQ 문맥이어야 한다.
 *
 * 호출 체인: iommu_report_device_fault → [이 함수]
 */
static int report_partial_fault(struct iommu_fault_param *fault_param,
				struct iommu_fault *fault)
{
	struct iopf_fault *iopf;	/* [한국어] 보관할 폴트 사본 */

	iopf = kzalloc_obj(*iopf);	/* [한국어] 부분 폴트는 나중에 그룹으로 합쳐야 하므로 복사해 둔다 — 원본은 드라이버의 스택이나 하드웨어 큐에 있어 곧 사라진다 */
	if (!iopf)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 드라이버가 실패 응답을 보낸다 */

	iopf->fault = *fault;	/* [한국어] 내용 복사 */

	mutex_lock(&fault_param->lock);	/* [한국어] 부분 폴트 목록 보호 */
	list_add(&iopf->list, &fault_param->partial);	/* [한국어] 마지막 폴트가 올 때까지 여기 모아 둔다 */
	mutex_unlock(&fault_param->lock);	/* [한국어] 락 해제 */

	return 0;	/* [한국어] 응답하지 않는다 — 그룹의 마지막이 아닌 요청은 개별 응답을 받지 않는다 (위 영어 주석) */
}

/*
 * [한국어]
 * iopf_group_alloc - 부분 폴트들을 모아 응답 단위 그룹을 만든다
 *
 * @iopf_param:   이 장치의 폴트 상태
 * @evt:          방금 도착한 마지막 폴트
 * @abort_group:  할당 실패 시 쓸 스택 예비본
 * @return:       만들어진 그룹 (실패하면 abort_group 자체)
 *
 * 할당 실패를 NULL 로 돌려주지 않는 것이 이 함수의 특징이다. 그룹이 없으면 장치에
 * 실패 응답조차 보낼 수 없고, 그러면 장치가 그 트랜잭션에 영원히 멈춰 있게 된다.
 * 그래서 호출자가 스택에 예비 그룹을 마련해 두고, 여기서는 어떤 경우에도 쓸 수 있는
 * 그룹을 돌려준다 (위 영어 주석).
 *
 * 부분 폴트를 마지막 폴트 '앞'으로 옮기는 것도 의도적이다. 하드웨어가 보낸 순서가
 * 유지되어야 처리기가 주소를 순서대로 채울 수 있다.
 *
 * pending_node 를 대기 목록에 넣는 것이 응답 보장의 근거다. 장치가 제거되면 그
 * 목록에 남은 그룹들에 실패 응답을 보낸다.
 *
 * 실행 컨텍스트: 폴트 보고 경로. 뮤텍스를 잡는다.
 *
 * 호출 체인: iommu_report_device_fault → [이 함수]
 */
static struct iopf_group *iopf_group_alloc(struct iommu_fault_param *iopf_param,
					   struct iopf_fault *evt,
					   struct iopf_group *abort_group)
{
	struct iopf_fault *iopf, *next;	/* [한국어] 부분 폴트 순회 커서 */
	struct iopf_group *group;	/* [한국어] 만들 그룹 */

	group = kzalloc_obj(*group);	/* [한국어] 보통은 새로 할당한다 */
	if (!group) {	/* [한국어] 메모리가 없다 */
		/*
		 * We always need to construct the group as we need it to abort
		 * the request at the driver if it can't be handled.
		 */
		group = abort_group;	/* [한국어] 호출자가 스택에 준 예비 그룹을 쓴다. 그룹 없이는 장치에 실패 응답조차 보낼 수 없어, 어떤 상황에서도 그룹은 만들어져야 하기 때문이다 (위 영어 주석) */
	}

	group->fault_param = iopf_param;	/* [한국어] 이 장치의 폴트 상태 (참조는 호출자가 이미 잡았다) */
	group->last_fault.fault = evt->fault;	/* [한국어] 마지막 폴트는 그룹 안에 직접 담는다 — 응답에 쓸 PASID 와 그룹 ID 가 여기서 나온다 */
	INIT_LIST_HEAD(&group->faults);	/* [한국어] 이 그룹의 폴트 목록 */
	INIT_LIST_HEAD(&group->pending_node);	/* [한국어] 응답 대기 목록의 고리. 비어 있는지가 곧 '아직 응답하지 않았는가'의 표식이 된다 */
	list_add(&group->last_fault.list, &group->faults);	/* [한국어] 마지막 폴트를 목록에 넣는다 */

	/* See if we have partial faults for this group */
	mutex_lock(&iopf_param->lock);	/* [한국어] 부분 폴트 목록 보호 */
	list_for_each_entry_safe(iopf, next, &iopf_param->partial, list) {	/* [한국어] 모아 둔 부분 폴트 중 */
		if (iopf->fault.prm.grpid == evt->fault.prm.grpid)	/* [한국어] 같은 그룹 ID 를 가진 것들을 */
			/* Insert *before* the last fault */
			list_move(&iopf->list, &group->faults);	/* [한국어] 그룹으로 옮긴다. 마지막 폴트 '앞'에 들어가므로 하드웨어가 보낸 순서가 유지된다 (위 영어 주석) */
	}
	list_add(&group->pending_node, &iopf_param->faults);	/* [한국어] 응답 대기 목록에 등록. 장치가 제거되면 이 목록의 그룹들에 실패 응답을 보낸다 */
	mutex_unlock(&iopf_param->lock);	/* [한국어] 락 해제 */

	group->fault_count = list_count_nodes(&group->faults);	/* [한국어] 그룹에 모인 폴트 수 */

	return group;	/* [한국어] 처리기에 넘길 그룹 */
}

/*
 * [한국어]
 * find_fault_handler - 이 폴트를 처리할 도메인의 부착 핸들을 찾는다
 *
 * @dev:    폴트를 낸 장치
 * @evt:    폴트 정보 (PASID 를 담고 있다)
 * @return: 부착 핸들, 처리할 주체가 없으면 NULL
 *
 * PASID 를 도메인으로 되짚는 조회다. 세 갈래가 있다.
 *  - PASID 가 실린 폴트: 그 PASID 에 붙은 도메인을 찾는다. SVA 의 보통 경로다.
 *  - PASID 가 실렸지만 붙은 것이 없고, 드라이버가 사용자 관리 PASID 표를 쓰는
 *    경우: 커널은 어떤 PASID 가 유효한지 모르므로 RID 에 붙은 중첩 도메인으로
 *    보낸다. 사용자 공간이 자기 PASID 표를 관리하는 중첩 번역 구성이다.
 *  - PASID 가 없는 폴트: 장치의 기본 주소 공간에서 난 것이다.
 *
 * 마지막의 iopf_handler 검사가 중요하다. 도메인이 붙어 있어도 폴트 처리기를
 * 등록하지 않았다면 이 폴트를 다룰 수 없고, 호출자가 실패 응답을 보내야 한다.
 *
 * 실행 컨텍스트: 폴트 보고 경로. 그룹 락을 들지 않는다 — 조회가 xa_lock 만 쓴다.
 *
 * 호출 체인: iommu_report_device_fault → [이 함수] → iommu_attach_handle_get
 */
static struct iommu_attach_handle *find_fault_handler(struct device *dev,
						     struct iopf_fault *evt)
{
	struct iommu_fault *fault = &evt->fault;	/* [한국어] 하드웨어가 준 폴트 정보 */
	struct iommu_attach_handle *attach_handle;	/* [한국어] 찾을 부착 핸들 */

	if (fault->prm.flags & IOMMU_FAULT_PAGE_REQUEST_PASID_VALID) {	/* [한국어] PASID 가 실린 폴트 — SVA 나 사용자 공간 주소 공간의 것 */
		attach_handle = iommu_attach_handle_get(dev->iommu_group,	/* [한국어] 그 PASID 에 붙은 도메인의 핸들을 찾는다 */
				fault->prm.pasid, 0);	/* [한국어] 도메인 종류는 가리지 않는다 */
		if (IS_ERR(attach_handle)) {	/* [한국어] 그 PASID 로 붙은 것이 없다 */
			const struct iommu_ops *ops = dev_iommu_ops(dev);	/* [한국어] 드라이버 확인 */

			if (!ops->user_pasid_table)	/* [한국어] PASID 표를 커널이 관리하는 보통의 경우 */
				return NULL;	/* [한국어] 처리할 주체가 없다 */
			/*
			 * The iommu driver for this device supports user-
			 * managed PASID table. Therefore page faults for
			 * any PASID should go through the NESTING domain
			 * attached to the device RID.
			 */
			attach_handle = iommu_attach_handle_get(	/* [한국어] PASID 표를 사용자 공간이 관리하는 경우 (중첩 번역). 커널은 어떤 PASID 가 유효한지 모르므로, 모든 PASID 폴트를 RID 에 붙은 중첩 도메인으로 보낸다 (위 영어 주석) */
					dev->iommu_group, IOMMU_NO_PASID,	/* [한국어] RID(비 PASID) 자리의 핸들 */
					IOMMU_DOMAIN_NESTED);	/* [한국어] 중첩 도메인이어야 한다 */
			if (IS_ERR(attach_handle))	/* [한국어] 그것마저 없다 */
				return NULL;	/* [한국어] 처리 불가 */
		}
	} else {
		attach_handle = iommu_attach_handle_get(dev->iommu_group,	/* [한국어] PASID 없는 폴트 — 장치의 기본 주소 공간에서 났다 */
				IOMMU_NO_PASID, 0);	/* [한국어] RID 자리의 핸들 */

		if (IS_ERR(attach_handle))	/* [한국어] 붙은 도메인이 없다 */
			return NULL;	/* [한국어] 처리 불가 */
	}

	if (!attach_handle->domain->iopf_handler)	/* [한국어] 도메인은 있으나 폴트 처리기를 등록하지 않았다 */
		return NULL;	/* [한국어] 이 도메인은 폴트를 다룰 수 없다 */

	return attach_handle;	/* [한국어] 이 핸들에서 도메인과 처리기를 얻을 수 있다 */
}

/*
 * [한국어]
 * iopf_error_response - 처리할 수 없는 폴트에 실패 응답을 보낸다
 *
 * @dev: 폴트를 낸 장치
 * @evt: 그 폴트
 *
 * 그룹을 만들 수조차 없었던 경로가 쓴다. 응답을 보내지 않으면 장치가 멈춘 채로
 * 남으므로, 처리기가 없거나 큐 설정이 없는 경우에도 반드시 무언가는 보내야 한다.
 *
 * RESP_INVALID 는 '그 주소는 유효하지 않다'는 뜻이고, 장치는 재시도하지 않고
 * 오류로 처리한다.
 *
 * 실행 컨텍스트: 폴트 보고 경로.
 *
 * 호출 체인: iommu_report_device_fault 의 err_bad_iopf → [이 함수]
 */
static void iopf_error_response(struct device *dev, struct iopf_fault *evt)
{
	const struct iommu_ops *ops = dev_iommu_ops(dev);	/* [한국어] 응답을 내려보낼 드라이버 */
	struct iommu_fault *fault = &evt->fault;	/* [한국어] 응답에 실을 식별자의 출처 */
	struct iommu_page_response resp = {	/* [한국어] 실패 응답 */
		.pasid = fault->prm.pasid,	/* [한국어] 같은 PASID 로 */
		.grpid = fault->prm.grpid,	/* [한국어] 같은 그룹 ID 로 — 장치가 어느 요청의 응답인지 알아야 한다 */
		.code = IOMMU_PAGE_RESP_INVALID	/* [한국어] '그 주소는 유효하지 않다' — 장치는 재시도하지 않고 오류로 처리한다 */
	};

	ops->page_response(dev, evt, &resp);	/* [한국어] 하드웨어에 응답을 내린다. 응답하지 않으면 장치가 영원히 멈춰 있으므로, 처리기가 없는 경우에도 반드시 보내야 한다 */
}

/**
 * iommu_report_device_fault() - Report fault event to device driver
 * @dev: the device
 * @evt: fault event data
 *
 * Called by IOMMU drivers when a fault is detected, typically in a threaded IRQ
 * handler. If this function fails then ops->page_response() was called to
 * complete evt if required.
 *
 * This module doesn't handle PCI PASID Stop Marker; IOMMU drivers must discard
 * them before reporting faults. A PASID Stop Marker (LRW = 0b100) doesn't
 * expect a response. It may be generated when disabling a PASID (issuing a
 * PASID stop request) by some PCI devices.
 *
 * The PASID stop request is issued by the device driver before unbind(). Once
 * it completes, no page request is generated for this PASID anymore and
 * outstanding ones have been pushed to the IOMMU (as per PCIe 4.0r1.0 - 6.20.1
 * and 10.4.1.2 - Managing PASID TLP Prefix Usage). Some PCI devices will wait
 * for all outstanding page requests to come back with a response before
 * completing the PASID stop request. Others do not wait for page responses, and
 * instead issue this Stop Marker that tells us when the PASID can be
 * reallocated.
 *
 * It is safe to discard the Stop Marker because it is an optimization.
 * a. Page requests, which are posted requests, have been flushed to the IOMMU
 *    when the stop request completes.
 * b. The IOMMU driver flushes all fault queues on unbind() before freeing the
 *    PASID.
 *
 * So even though the Stop Marker might be issued by the device *after* the stop
 * request completes, outstanding faults will have been dealt with by the time
 * the PASID is freed.
 *
 * Any valid page fault will be eventually routed to an iommu domain and the
 * page fault handler installed there will get called. The users of this
 * handling framework should guarantee that the iommu domain could only be
 * freed after the device has stopped generating page faults (or the iommu
 * hardware has been set to block the page faults) and the pending page faults
 * have been flushed. In case no page fault handler is attached or no iopf params
 * are setup, then the ops->page_response() is called to complete the evt.
 *
 * Returns 0 on success, or an error in case of a bad/failed iopf setup.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iommu_report_device_fault - 하드웨어가 낸 페이지 요청을 도메인으로 배달한다
 *
 * @dev:    폴트를 낸 장치
 * @evt:    폴트 정보
 * @return: 0 이면 접수(또는 응답 완료), -EINVAL 이면 설정 오류
 *
 * 벤더 드라이버의 PRI 인터럽트 핸들러가 부르는 진입점이다. 하는 일은 배달이지
 * 처리가 아니다 — 무엇을 할지는 도메인의 iopf_handler 가 정한다.
 *
 * 반환값 규약이 미묘하다. 어떤 경로로 빠지든 장치에는 응답이 이미 보내졌거나
 * (실패 경로) 처리기가 보내기로 되어 있다(성공 경로). 그래서 0 을 돌려주는
 * err_abort 경로에서도 호출자가 다시 응답하면 안 된다.
 *
 * 위 영어 주석의 절반이 PASID Stop Marker 설명인데, 요점은 이 계층이 그것을
 * 다루지 않는다는 것이다. 드라이버가 걸러야 하며, 버려도 안전한 이유는 PASID 를
 * 반납하기 전에 iopf_queue_flush_dev 가 밀린 폴트를 모두 처리하기 때문이다.
 *
 * 또 하나의 계약: 도메인은 장치가 폴트 생성을 멈추고 밀린 폴트가 모두 처리된
 * 뒤에만 해제되어야 한다. 그렇지 않으면 이미 사라진 도메인의 처리기를 부르게 된다.
 *
 * 실행 컨텍스트: 스레드 IRQ 핸들러. 뮤텍스를 잡으므로 잠들 수 있어야 한다.
 *
 * 호출 체인: 벤더 드라이버의 PRI 핸들러 → [이 함수]
 *            → find_fault_handler, iopf_group_alloc, domain->iopf_handler
 */
int iommu_report_device_fault(struct device *dev, struct iopf_fault *evt)
{
	struct iommu_attach_handle *attach_handle;	/* [한국어] 이 폴트를 처리할 도메인의 핸들 */
	struct iommu_fault *fault = &evt->fault;	/* [한국어] 하드웨어가 준 폴트 정보 */
	struct iommu_fault_param *iopf_param;	/* [한국어] 이 장치의 폴트 상태 */
	struct iopf_group abort_group = {};	/* [한국어] 메모리가 없어 그룹을 만들지 못할 때 쓸 스택 예비본. 어떤 상황에서도 장치에 응답은 보내야 하기 때문에 미리 자리를 잡아 둔다 */
	struct iopf_group *group;	/* [한국어] 처리기에 넘길 그룹 */

	attach_handle = find_fault_handler(dev, evt);	/* [한국어] PASID 로 도메인과 처리기를 찾는다 */
	if (!attach_handle)	/* [한국어] 처리할 주체가 없다 */
		goto err_bad_iopf;	/* [한국어] 실패 응답을 보내고 물러난다 */

	/*
	 * Something has gone wrong if a fault capable domain is attached but no
	 * iopf_param is setup
	 */
	iopf_param = iopf_get_dev_fault_param(dev);	/* [한국어] 장치의 폴트 상태에 참조를 잡는다. 이 참조는 그룹이 이어받아 응답이 끝날 때까지 유지된다 */
	if (WARN_ON(!iopf_param))	/* [한국어] 폴트를 다룰 수 있는 도메인이 붙어 있는데 큐 설정이 없다 — 드라이버가 iopf_queue_add_device 를 건너뛴 것이다 (위 영어 주석) */
		goto err_bad_iopf;	/* [한국어] 실패 응답 */

	if (!(fault->prm.flags & IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE)) {	/* [한국어] 그룹의 마지막이 아닌 요청 */
		int ret;	/* [한국어] 보관 결과 */

		ret = report_partial_fault(iopf_param, fault);	/* [한국어] 마지막이 올 때까지 모아 둔다 */
		iopf_put_dev_fault_param(iopf_param);	/* [한국어] 이 경로에서는 그룹을 만들지 않으므로 참조를 여기서 놓는다 */
		/* A request that is not the last does not need to be ack'd */

		return ret;	/* [한국어] 응답은 보내지 않는다 — 그룹 단위로 마지막에 한 번만 응답한다 */
	}

	/*
	 * This is the last page fault of a group. Allocate an iopf group and
	 * pass it to domain's page fault handler. The group holds a reference
	 * count of the fault parameter. It will be released after response or
	 * error path of this function. If an error is returned, the caller
	 * will send a response to the hardware. We need to clean up before
	 * leaving, otherwise partial faults will be stuck.
	 */
	group = iopf_group_alloc(iopf_param, evt, &abort_group);	/* [한국어] 마지막 폴트가 왔다. 모아 둔 부분 폴트와 합쳐 그룹을 만든다 */
	if (group == &abort_group)	/* [한국어] 할당에 실패해 예비본이 쓰였다 */
		goto err_abort;	/* [한국어] 실패 응답을 보내고 정리 */

	group->attach_handle = attach_handle;	/* [한국어] 처리기가 어느 도메인의 것인지 알 수 있게 한다 */

	/*
	 * On success iopf_handler must call iopf_group_response() and
	 * iopf_free_group()
	 */
	if (group->attach_handle->domain->iopf_handler(group))	/* [한국어] 도메인의 처리기에 넘긴다. 성공하면 그 처리기가 iopf_group_response 와 iopf_free_group 을 책임진다 — 이 함수는 더 이상 그룹을 만지지 않는다 (위 영어 주석) */
		goto err_abort;	/* [한국어] 처리기가 접수를 거절했다 */

	return 0;	/* [한국어] 처리기가 접수했다. 실제 처리는 대개 워크큐에서 나중에 일어난다 */

err_abort:	/* [한국어] 그룹은 있으나 처리할 수 없는 경로 */
	dev_warn_ratelimited(dev, "iopf with pasid %d aborted\n",	/* [한국어] 폴트가 버려졌음을 남긴다. 반복될 수 있어 rate limit */
			     fault->prm.pasid);	/* [한국어] 어느 주소 공간의 폴트였는지 */
	iopf_group_response(group, IOMMU_PAGE_RESP_FAILURE);	/* [한국어] 장치에 실패를 알려 멈춘 트랜잭션을 풀어 준다 */
	if (group == &abort_group)	/* [한국어] 스택 예비본이면 */
		__iopf_free_group(group);	/* [한국어] 내용물만 해제한다 — 구조체 자체는 스택에 있다 */
	else
		iopf_free_group(group);	/* [한국어] 할당된 그룹이면 통째로 해제 */

	return 0;	/* [한국어] 응답은 보냈으므로 호출자가 다시 응답하면 안 된다 */

err_bad_iopf:	/* [한국어] 그룹조차 만들지 못한 경로 */
	if (fault->type == IOMMU_FAULT_PAGE_REQ)	/* [한국어] 응답을 기대하는 종류의 폴트라면 */
		iopf_error_response(dev, evt);	/* [한국어] 여기서 직접 실패 응답을 보낸다 */

	return -EINVAL;	/* [한국어] 호출자에게도 실패를 알린다 (위 영어 주석: 실패 시 page_response 는 이미 불렸다) */
}
EXPORT_SYMBOL_GPL(iommu_report_device_fault);	/* [한국어] 벤더 드라이버의 PRI 인터럽트 핸들러가 부른다 */

/**
 * iopf_queue_flush_dev - Ensure that all queued faults have been processed
 * @dev: the endpoint whose faults need to be flushed.
 *
 * The IOMMU driver calls this before releasing a PASID, to ensure that all
 * pending faults for this PASID have been handled, and won't hit the address
 * space of the next process that uses this PASID. The driver must make sure
 * that no new fault is added to the queue. In particular it must flush its
 * low-level queue before calling this function.
 *
 * Return: 0 on success and <0 on error.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iopf_queue_flush_dev - 밀린 폴트가 모두 처리될 때까지 기다린다
 *
 * @dev:    대상 장치
 * @return: 0 성공, -ENODEV 면 큐에 등록되지 않은 장치
 *
 * PASID 를 반납하기 직전에 부른다. 이것을 빠뜨리면 워크큐에 남아 있던 옛 폴트가
 * 나중에 처리되면서, 그 PASID 를 물려받은 다음 프로세스의 주소 공간에 페이지를
 * 채워 넣는다 — 서로 무관한 두 프로세스 사이에 페이지가 새는 셈이다 (위 영어 주석).
 *
 * 호출자가 먼저 해야 할 일이 있다는 점도 계약의 일부다. 하드웨어 큐를 비우고 새
 * 폴트가 들어오지 않게 만든 뒤에 불러야, 이 함수가 기다리는 동안 새 항목이 쌓이지
 * 않는다.
 *
 * 실행 컨텍스트: PASID 해제 경로. 프로세스 문맥, 워크큐 완료를 기다리므로 잠든다.
 *
 * 호출 체인: 벤더 드라이버의 PASID 해제 → [이 함수]
 */
int iopf_queue_flush_dev(struct device *dev)
{
	struct iommu_fault_param *iopf_param;	/* [한국어] 이 장치의 폴트 상태 */

	/*
	 * It's a driver bug to be here after iopf_queue_remove_device().
	 * Therefore, it's safe to dereference the fault parameter without
	 * holding the lock.
	 */
	iopf_param = rcu_dereference_check(dev->iommu->fault_param, true);	/* [한국어] 락 없이 읽는다. iopf_queue_remove_device 이후에 이 함수가 불리는 것은 드라이버 버그이므로, 그 전이라면 포인터가 안정적이다 (위 영어 주석) */
	if (WARN_ON(!iopf_param))	/* [한국어] 큐에 등록되지 않은 장치 */
		return -ENODEV;	/* [한국어] 플러시할 것이 없다 */

	flush_workqueue(iopf_param->queue->wq);	/* [한국어] 큐에 들어간 모든 폴트 처리가 끝날 때까지 기다린다. PASID 를 반납하기 전에 이것을 하지 않으면, 그 PASID 를 물려받은 다음 프로세스의 주소 공간에 옛 폴트가 페이지를 채워 넣는다 (위 영어 주석) */

	return 0;	/* [한국어] 모든 밀린 폴트가 처리되었다 */
}
EXPORT_SYMBOL_GPL(iopf_queue_flush_dev);	/* [한국어] 드라이버가 PASID 해제 직전에 부른다 */

/**
 * iopf_group_response - Respond a group of page faults
 * @group: the group of faults with the same group id
 * @status: the response code
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iopf_group_response - 그룹에 응답을 보낸다
 *
 * @group:  응답할 그룹
 * @status: 응답 코드 (SUCCESS 면 장치가 재개, 그 외는 오류 처리)
 *
 * pending_node 가 비어 있는지 확인하는 것이 이 함수의 핵심이다. 그 고리가 비어
 * 있다는 것은 이미 응답이 나갔다는 뜻이며, 장치 제거 경로가 대기 그룹들에 일괄
 * 실패 응답을 보낸 뒤에 처리기가 뒤늦게 이 함수를 부르는 경우가 실제로 있다.
 * 중복 응답은 장치가 이미 풀린 트랜잭션을 다시 건드리게 만든다.
 *
 * 실행 컨텍스트: 워크큐 또는 폴트 경로. 뮤텍스를 잡는다.
 *
 * 호출 체인: iommu-sva, iommufd, iommu_report_device_fault 의 abort → [이 함수]
 *            → ops->page_response
 */
void iopf_group_response(struct iopf_group *group,
			 enum iommu_page_response_code status)
{
	struct iommu_fault_param *fault_param = group->fault_param;	/* [한국어] 이 장치의 폴트 상태 */
	struct iopf_fault *iopf = &group->last_fault;	/* [한국어] 응답 식별자의 출처 */
	struct device *dev = group->fault_param->dev;	/* [한국어] 응답을 받을 장치 */
	const struct iommu_ops *ops = dev_iommu_ops(dev);	/* [한국어] 응답을 내려보낼 드라이버 */
	struct iommu_page_response resp = {	/* [한국어] 보낼 응답 */
		.pasid = iopf->fault.prm.pasid,	/* [한국어] 어느 주소 공간의 것인지 */
		.grpid = iopf->fault.prm.grpid,	/* [한국어] 어느 그룹의 것인지 — 장치가 이 값으로 멈춘 트랜잭션을 찾는다 */
		.code = status,	/* [한국어] SUCCESS 면 재개, INVALID/FAILURE 면 오류 처리 */
	};

	/* Only send response if there is a fault report pending */
	mutex_lock(&fault_param->lock);	/* [한국어] 대기 목록 보호 */
	if (!list_empty(&group->pending_node)) {	/* [한국어] 아직 응답하지 않았다면 (위 영어 주석) */
		ops->page_response(dev, &group->last_fault, &resp);	/* [한국어] 하드웨어에 응답을 내린다 */
		list_del_init(&group->pending_node);	/* [한국어] 목록에서 빼고 고리를 비운다. 이 '비어 있음'이 곧 응답 완료 표식이라, 장치 제거 경로가 같은 그룹에 두 번 응답하는 것을 막는다 */
	}
	mutex_unlock(&fault_param->lock);	/* [한국어] 락 해제 */
}
EXPORT_SYMBOL_GPL(iopf_group_response);	/* [한국어] 도메인의 폴트 처리기가 처리를 마친 뒤 부른다 */

/**
 * iopf_queue_discard_partial - Remove all pending partial fault
 * @queue: the queue whose partial faults need to be discarded
 *
 * When the hardware queue overflows, last page faults in a group may have been
 * lost and the IOMMU driver calls this to discard all partial faults. The
 * driver shouldn't be adding new faults to this queue concurrently.
 *
 * Return: 0 on success and <0 on error.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iopf_queue_discard_partial - 모아 둔 부분 폴트를 모두 버린다
 *
 * @queue:  대상 큐
 * @return: 0 성공, -EINVAL 이면 큐가 NULL
 *
 * 하드웨어 큐가 넘치면 그룹의 마지막 폴트가 유실될 수 있다. 그러면 모아 둔 부분
 * 폴트들은 영영 그룹이 되지 못한 채 메모리만 붙잡으므로, 드라이버가 오버플로를
 * 감지했을 때 이 함수로 정리한다 (위 영어 주석).
 *
 * 응답을 보내지 않는 것에 주의할 것 — 부분 요청은 애초에 개별 응답을 기대하지
 * 않는다. 장치 쪽은 하드웨어 오버플로를 자체적으로 처리한다.
 *
 * 실행 컨텍스트: 드라이버의 오버플로 처리. 뮤텍스를 잡는다.
 *
 * 호출 체인: 벤더 드라이버 → [이 함수]
 */
int iopf_queue_discard_partial(struct iopf_queue *queue)
{
	struct iopf_fault *iopf, *next;	/* [한국어] 해제하며 순회 */
	struct iommu_fault_param *iopf_param;	/* [한국어] 장치별 폴트 상태 순회 커서 */

	if (!queue)	/* [한국어] 큐가 없다 */
		return -EINVAL;	/* [한국어] 잘못된 호출 */

	mutex_lock(&queue->lock);	/* [한국어] 장치 목록 보호 */
	list_for_each_entry(iopf_param, &queue->devices, queue_list) {	/* [한국어] 이 큐에 붙은 모든 장치에 대해 */
		mutex_lock(&iopf_param->lock);	/* [한국어] 그 장치의 부분 폴트 목록 보호 */
		list_for_each_entry_safe(iopf, next, &iopf_param->partial,	/* [한국어] 모아 둔 부분 폴트를 */
					 list) {	/* [한국어] 하나씩 */
			list_del(&iopf->list);	/* [한국어] 목록에서 빼고 */
			kfree(iopf);	/* [한국어] 해제한다. 하드웨어 큐가 넘쳐 마지막 폴트가 유실되었으므로, 이 부분 폴트들은 영영 그룹이 되지 못하고 메모리만 붙잡는다 (위 영어 주석) */
		}
		mutex_unlock(&iopf_param->lock);	/* [한국어] 락 해제 */
	}
	mutex_unlock(&queue->lock);	/* [한국어] 락 해제 */
	return 0;	/* [한국어] 정리 완료 */
}
EXPORT_SYMBOL_GPL(iopf_queue_discard_partial);	/* [한국어] 드라이버가 하드웨어 큐 오버플로를 감지했을 때 부른다 */

/**
 * iopf_queue_add_device - Add producer to the fault queue
 * @queue: IOPF queue
 * @dev: device to add
 *
 * Return: 0 on success and <0 on error.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iopf_queue_add_device - 장치를 폴트 큐에 등록한다
 *
 * @queue:  대상 큐
 * @dev:    등록할 장치
 * @return: 0 성공, -EBUSY 면 이미 등록됨, -ENODEV 면 드라이버가 응답을 못 보낸다
 *
 * 드라이버가 장치의 PRI 를 켜기 전에 부른다. 이 등록이 없으면
 * iommu_report_device_fault 가 WARN 과 함께 실패 응답을 보낸다.
 *
 * page_response 콜백을 먼저 확인하는 것이 중요하다. 응답을 하드웨어에 내려보낼
 * 방법이 없는 드라이버에서 폴트를 받으면, 장치가 멈춘 채로 영영 남는다.
 *
 * rcu_assign_pointer 를 마지막에 두는 것도 같은 종류의 순서 문제다. 이 대입이
 * 보이는 순간부터 폴트 경로가 구조체를 쓰므로 초기화가 모두 끝난 뒤여야 한다.
 *
 * 실행 컨텍스트: 장치 초기화. 프로세스 문맥, 두 뮤텍스를 큐→장치 순서로 잡는다.
 *
 * 호출 체인: 벤더 드라이버의 PRI 활성화 → [이 함수]
 */
int iopf_queue_add_device(struct iopf_queue *queue, struct device *dev)
{
	int ret = 0;	/* [한국어] 등록 결과 */
	struct dev_iommu *param = dev->iommu;	/* [한국어] 장치별 IOMMU 상태 */
	struct iommu_fault_param *fault_param;	/* [한국어] 만들 폴트 상태 */
	const struct iommu_ops *ops = dev_iommu_ops(dev);	/* [한국어] 드라이버 확인 */

	if (!ops->page_response)	/* [한국어] 응답을 하드웨어에 내려보낼 방법이 없는 드라이버 */
		return -ENODEV;	/* [한국어] 폴트 처리를 켤 수 없다 — 응답할 수 없으면 장치가 멈춘 채로 남는다 */

	mutex_lock(&queue->lock);	/* [한국어] 큐의 장치 목록 보호 */
	mutex_lock(&param->lock);	/* [한국어] 장치 상태 보호. 락 순서가 큐 → 장치로 고정되어 있다 */
	if (rcu_dereference_check(param->fault_param,	/* [한국어] 이미 등록된 장치인지 */
				  lockdep_is_held(&param->lock))) {	/* [한국어] 락을 든 상태이므로 RCU 검사를 생략해도 된다고 알린다 */
		ret = -EBUSY;	/* [한국어] 한 장치는 큐 하나에만 붙는다 */
		goto done_unlock;	/* [한국어] 거절 */
	}

	fault_param = kzalloc_obj(*fault_param);	/* [한국어] 이 장치의 폴트 상태 */
	if (!fault_param) {	/* [한국어] 할당 실패 */
		ret = -ENOMEM;	/* [한국어] 이유 기록 */
		goto done_unlock;	/* [한국어] 실패 */
	}

	mutex_init(&fault_param->lock);	/* [한국어] 부분 폴트와 대기 그룹 목록을 지키는 락 */
	INIT_LIST_HEAD(&fault_param->faults);	/* [한국어] 응답 대기 중인 그룹들 */
	INIT_LIST_HEAD(&fault_param->partial);	/* [한국어] 마지막을 기다리는 부분 폴트들 */
	fault_param->dev = dev;	/* [한국어] 응답을 보낼 장치 */
	refcount_set(&fault_param->users, 1);	/* [한국어] 등록이 든 참조 하나. remove 에서 놓는다 */
	list_add(&fault_param->queue_list, &queue->devices);	/* [한국어] 큐의 장치 목록에 등록 */
	fault_param->queue = queue;	/* [한국어] 처리기가 워크큐를 찾는 경로 */

	rcu_assign_pointer(param->fault_param, fault_param);	/* [한국어] 마지막에 공개한다. 이 대입이 보이는 순간부터 폴트 경로가 이 구조체를 쓰므로, 위의 초기화가 모두 끝난 뒤여야 한다 */

done_unlock:	/* [한국어] 성공·실패 공통 출구 */
	mutex_unlock(&param->lock);	/* [한국어] 역순으로 해제 */
	mutex_unlock(&queue->lock);	/* [한국어] 큐 락 해제 */

	return ret;	/* [한국어] 0 이면 이 장치의 폴트가 이 큐로 온다 */
}
EXPORT_SYMBOL_GPL(iopf_queue_add_device);	/* [한국어] 드라이버가 PRI 를 켜기 전에 부른다 */

/**
 * iopf_queue_remove_device - Remove producer from fault queue
 * @queue: IOPF queue
 * @dev: device to remove
 *
 * Removing a device from an iopf_queue. It's recommended to follow these
 * steps when removing a device:
 *
 * - Disable new PRI reception: Turn off PRI generation in the IOMMU hardware
 *   and flush any hardware page request queues. This should be done before
 *   calling into this helper.
 * - Acknowledge all outstanding PRQs to the device: Respond to all outstanding
 *   page requests with IOMMU_PAGE_RESP_INVALID, indicating the device should
 *   not retry. This helper function handles this.
 * - Disable PRI on the device: After calling this helper, the caller could
 *   then disable PRI on the device.
 *
 * Calling iopf_queue_remove_device() essentially disassociates the device.
 * The fault_param might still exist, but iommu_page_response() will do
 * nothing. The device fault parameter reference count has been properly
 * passed from iommu_report_device_fault() to the fault handling work, and
 * will eventually be released after iommu_page_response().
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iopf_queue_remove_device - 장치를 폴트 큐에서 뗀다
 *
 * @queue: 대상 큐
 * @dev:   제거할 장치
 *
 * 위 영어 주석이 제거 순서를 명시한다 — 하드웨어에서 PRI 생성을 먼저 끄고 큐를
 * 비운 뒤에 이 함수를 부르고, 그 다음 장치의 PRI 를 끈다. 이 함수는 그 가운데
 * 단계, 즉 아직 응답하지 않은 요청들을 모두 실패로 마감하는 일을 맡는다.
 *
 * 그 마감이 이 함수의 존재 이유다. 응답을 기다리는 트랜잭션을 남긴 채 PRI 를 끄면
 * 장치가 그 요청에 영원히 멈춰 있게 된다.
 *
 * 참조 계수 처리가 미묘하다. 이 함수가 놓는 것은 등록이 들었던 참조 하나뿐이며,
 * 처리 중인 그룹이 아직 참조를 들고 있으면 구조체는 살아남는다. 다만
 * fault_param 포인터가 NULL 로 끊기므로, 그 그룹이 나중에 응답하려 해도
 * pending_node 가 이미 비어 있어 아무 일도 일어나지 않는다 (위 영어 주석).
 *
 * 실행 컨텍스트: 장치 제거. 프로세스 문맥, 세 개의 뮤텍스를 순서대로 잡는다.
 *
 * 호출 체인: 벤더 드라이버의 PRI 비활성화, iopf_queue_free → [이 함수]
 */
void iopf_queue_remove_device(struct iopf_queue *queue, struct device *dev)
{
	struct iopf_fault *partial_iopf;	/* [한국어] 부분 폴트 순회 커서 */
	struct iopf_fault *next;	/* [한국어] _safe 순회용 */
	struct iopf_group *group, *temp;	/* [한국어] 대기 그룹 순회 커서 */
	struct dev_iommu *param = dev->iommu;	/* [한국어] 장치별 IOMMU 상태 */
	struct iommu_fault_param *fault_param;	/* [한국어] 해제할 폴트 상태 */
	const struct iommu_ops *ops = dev_iommu_ops(dev);	/* [한국어] 응답을 내려보낼 드라이버 */

	mutex_lock(&queue->lock);	/* [한국어] 큐 목록 보호 */
	mutex_lock(&param->lock);	/* [한국어] 장치 상태 보호 (큐 → 장치 순서) */
	fault_param = rcu_dereference_check(param->fault_param,	/* [한국어] 등록된 상태를 꺼낸다 */
					    lockdep_is_held(&param->lock));	/* [한국어] 락을 든 상태 */

	if (WARN_ON(!fault_param || fault_param->queue != queue))	/* [한국어] 등록되지 않았거나 다른 큐에 붙어 있다 */
		goto unlock;	/* [한국어] 잘못된 호출 */

	mutex_lock(&fault_param->lock);	/* [한국어] 목록 보호 */
	list_for_each_entry_safe(partial_iopf, next, &fault_param->partial, list)	/* [한국어] 마지막을 기다리던 부분 폴트들은 */
		kfree(partial_iopf);	/* [한국어] 그냥 버린다 — 응답을 기대하지 않는 요청들이다 */

	list_for_each_entry_safe(group, temp, &fault_param->faults, pending_node) {	/* [한국어] 아직 응답하지 않은 그룹들에 대해 */
		struct iopf_fault *iopf = &group->last_fault;	/* [한국어] 응답 식별자의 출처 */
		struct iommu_page_response resp = {	/* [한국어] 실패 응답 */
			.pasid = iopf->fault.prm.pasid,	/* [한국어] 같은 PASID */
			.grpid = iopf->fault.prm.grpid,	/* [한국어] 같은 그룹 ID */
			.code = IOMMU_PAGE_RESP_INVALID	/* [한국어] 재시도하지 말라는 뜻. 장치가 멈춘 트랜잭션을 오류로 풀게 된다 (위 영어 주석) */
		};

		ops->page_response(dev, iopf, &resp);	/* [한국어] 응답을 내려보낸다. 이것을 하지 않으면 장치가 영원히 그 요청에 멈춰 있다 */
		list_del_init(&group->pending_node);	/* [한국어] 응답 완료 표시 — 처리기가 나중에 iopf_group_response 를 불러도 중복 응답이 나가지 않는다 */
		iopf_free_group(group);	/* [한국어] 그룹 해제 */
	}
	mutex_unlock(&fault_param->lock);	/* [한국어] 목록 락 해제 */

	list_del(&fault_param->queue_list);	/* [한국어] 큐의 장치 목록에서 제거 */

	/* dec the ref owned by iopf_queue_add_device() */
	rcu_assign_pointer(param->fault_param, NULL);	/* [한국어] 먼저 끊는다 — 이후 폴트 경로가 이 장치를 보면 NULL 을 얻어 물러난다 */
	iopf_put_dev_fault_param(fault_param);	/* [한국어] 등록이 들었던 참조를 놓는다. 처리 중인 그룹이 아직 참조를 들고 있으면 그쪽이 마지막에 해제한다 (위 영어 주석) */
unlock:	/* [한국어] 공통 출구 */
	mutex_unlock(&param->lock);	/* [한국어] 역순 해제 */
	mutex_unlock(&queue->lock);	/* [한국어] 큐 락 해제 */
}
EXPORT_SYMBOL_GPL(iopf_queue_remove_device);	/* [한국어] 드라이버가 PRI 를 끄기 전에 부른다 */

/**
 * iopf_queue_alloc - Allocate and initialize a fault queue
 * @name: a unique string identifying the queue (for workqueue)
 *
 * Return: the queue on success and NULL on error.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iopf_queue_alloc - 폴트 처리 큐를 만든다
 *
 * @name:   워크큐 이름에 쓸 식별자
 * @return: 큐, 실패하면 NULL
 *
 * 대개 IOMMU 인스턴스마다 하나씩 만들고 그 아래의 장치들이 공유한다.
 *
 * 워크큐가 순서 없는(unordered) 것이 설계상 중요하다. PRI 요청은 그룹 안에서만
 * 순서가 의미 있고, 하드웨어가 이미 그룹 단위로 묶어 주므로 상위에서 그룹끼리의
 * 순서를 지킬 이유가 없다 (위 영어 주석). 순서를 강제하면 느린 폴트 하나가 뒤의
 * 모든 폴트를 막게 된다.
 *
 * WQ_UNBOUND 인 것은 폴트 처리가 디스크 I/O 까지 유발할 수 있어 오래 걸리기
 * 때문이다 — 특정 CPU 에 묶어 두면 그 CPU 의 다른 작업이 밀린다.
 *
 * 실행 컨텍스트: 드라이버 초기화. 프로세스 문맥.
 *
 * 호출 체인: 벤더 드라이버 초기화 → [이 함수]
 */
struct iopf_queue *iopf_queue_alloc(const char *name)
{
	struct iopf_queue *queue;	/* [한국어] 만들 큐 */

	queue = kzalloc_obj(*queue);	/* [한국어] 큐 구조체 */
	if (!queue)	/* [한국어] 할당 실패 */
		return NULL;	/* [한국어] 폴트 처리를 켤 수 없다 */

	/*
	 * The WQ is unordered because the low-level handler enqueues faults by
	 * group. PRI requests within a group have to be ordered, but once
	 * that's dealt with, the high-level function can handle groups out of
	 * order.
	 */
	queue->wq = alloc_workqueue("iopf_queue/%s", WQ_UNBOUND, 0, name);	/* [한국어] 순서 없는(unordered) 워크큐다. 하드웨어가 이미 그룹 단위로 묶어 주므로 그룹 안의 순서는 보장되어 있고, 그룹끼리는 순서를 지킬 이유가 없다 (위 영어 주석). WQ_UNBOUND 는 폴트 처리가 오래 걸릴 수 있어 특정 CPU 에 묶지 않으려는 것 */
	if (!queue->wq) {	/* [한국어] 워크큐 생성 실패 */
		kfree(queue);	/* [한국어] 큐 구조체 반납 */
		return NULL;	/* [한국어] 실패 */
	}

	INIT_LIST_HEAD(&queue->devices);	/* [한국어] 이 큐에 붙을 장치 목록 */
	mutex_init(&queue->lock);	/* [한국어] 그 목록을 지키는 락 */

	return queue;	/* [한국어] 드라이버가 여러 장치를 여기 붙인다 */
}
EXPORT_SYMBOL_GPL(iopf_queue_alloc);	/* [한국어] 벤더 드라이버 초기화에서 IOMMU 인스턴스당 하나씩 만든다 */

/**
 * iopf_queue_free - Free IOPF queue
 * @queue: queue to free
 *
 * Counterpart to iopf_queue_alloc(). The driver must not be queuing faults or
 * adding/removing devices on this queue anymore.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * iopf_queue_free - 폴트 큐를 해제한다
 *
 * @queue: 해제할 큐
 *
 * 아직 붙어 있는 장치가 있으면 각각 정리한다 — 그 과정에서 밀린 그룹들에 실패
 * 응답이 나가므로, 멈춘 트랜잭션을 남기지 않는다.
 *
 * destroy_workqueue 가 진행 중인 작업이 끝날 때까지 기다린다는 점이 중요하다.
 * 그 대기가 없으면 처리 중인 그룹이 이미 해제된 큐를 참조하게 된다.
 *
 * 실행 컨텍스트: 드라이버 제거. 프로세스 문맥, 잠든다.
 *
 * 호출 체인: 벤더 드라이버 제거 → [이 함수]
 */
void iopf_queue_free(struct iopf_queue *queue)
{
	struct iommu_fault_param *iopf_param, *next;	/* [한국어] 해제하며 순회 */

	if (!queue)	/* [한국어] NULL 도 안전하게 */
		return;	/* [한국어] 할 일 없음 */

	list_for_each_entry_safe(iopf_param, next, &queue->devices, queue_list)	/* [한국어] 아직 붙어 있는 장치가 있으면 */
		iopf_queue_remove_device(queue, iopf_param->dev);	/* [한국어] 각각 정리한다 (밀린 그룹에 실패 응답 포함) */

	destroy_workqueue(queue->wq);	/* [한국어] 워크큐가 끝날 때까지 기다렸다 해제한다 */
	kfree(queue);	/* [한국어] 큐 구조체 반납 */
}
EXPORT_SYMBOL_GPL(iopf_queue_free);	/* [한국어] 드라이버 제거 경로 */
