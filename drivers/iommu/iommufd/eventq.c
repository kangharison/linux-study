// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Intel Corporation
 */
/*
 * [한국어 설명] 폴트와 vIOMMU 이벤트를 사용자 공간에 전하는 큐 (eventq.c)
 *
 * === 파일의 역할 ===
 * 하드웨어가 낸 사건을 사용자 공간(대개 VMM)에 전하는 두 큐를 구현한다.
 * 둘 다 별도의 파일 디스크립터로 읽히고, 공통 부분(struct iommufd_eventq)을
 * 공유한다.
 *
 * 1) 폴트 큐. 장치가 매핑되지 않은 주소를 건드리면 IOMMU 가 폴트를 낸다.
 *    그것을 사용자에게 읽히고, 사용자가 응답을 써 넣으면 장치가 멈춰 있던
 *    요청을 다시 시도한다. 그래서 이 큐만 쓰기(write)를 지원한다.
 *
 * 2) vIOMMU 이벤트 큐. 게스트에게 전해야 할 하드웨어 사건을 담는다. 큐가
 *    가득 차면 이벤트를 버리는 대신 "여기서 잃어버렸다"는 표시를 남긴다.
 *
 * 응답 없는 폴트의 처리가 이 파일에서 미묘한 부분이다. 장치가 떠나거나
 * 큐가 사라지면 그 폴트들에 실패로 응답해야 한다 — 그러지 않으면 장치가
 * 영원히 기다린다.
 *
 * 잃어버림 표시의 재배치도 까다롭다. 그것은 늘 큐의 맨 뒤에 있어야 하고,
 * 읽어 갈 때는 복사본을 만들어 넘긴다 — 원본은 되돌려 놓을 수 있어야
 * 하기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 하드웨어 폴트 → 드라이버 → iommu 코어의 iopf → iommufd_fault_iopf_handler
 *   → 이 큐 → 사용자 read() → 사용자 write() → iopf_group_response → 장치
 *
 * 드라이버 인터럽트 스레드 → iommufd_viommu_report_event(driver.c)
 *   → 이 큐 → 사용자 read() → 게스트
 *
 * 실행 컨텍스트: 읽기·쓰기는 프로세스 문맥. 큐에 넣는 쪽은 인터럽트
 * 스레드일 수 있어 스핀락을 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 위: 사용자 공간의 read/write/poll.
 * 아래: iommu 코어의 iopf API, driver.c 의 이벤트 보고 경로.
 *
 * === 주요 함수/구조체 요약 ===
 * iommufd_auto_response_faults: 장치가 떠날 때 그 장치의 답 없는 폴트에
 *   실패로 응답한다.
 * iommufd_fault_fops_read / write: 폴트를 읽히고 응답을 받는다. 응답을
 *   짝짓는 열쇠가 cookie 다.
 * iommufd_veventq_deliver_fetch / restore: 잃어버림 표시를 다루는 짝.
 * iommufd_eventq_init: 익명 inode 로 파일을 만들어 큐를 연다.
 * INIT_EVENTQ_FOPS: 두 큐가 공유하는 파일 연산을 찍어 내는 매크로.
 */
#define pr_fmt(fmt) "iommufd: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/iommufd.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <uapi/linux/iommufd.h>

#include "../iommu-priv.h"
#include "iommufd_private.h"

/* IOMMUFD_OBJ_FAULT Functions */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_auto_response_faults - 그 장치의 답 없는 폴트에 실패로 응답한다
 *
 * @hwpt: 그 폴트들이 속한 도메인.
 * @handle: 떠나는 장치의 attach 핸들.
 *
 * 장치를 뗄 때 부른다. 두 곳을 훑어야 한다 — 아직 사용자가 읽어 가지
 * 않은 큐와, 읽어 갔지만 응답이 오지 않은 목록이다.
 *
 * 응답하지 않고 그냥 버리면 장치가 영원히 기다린다. 그래서 모두
 * "무효"로 답해 그 요청을 접게 한다.
 *
 * 큐에서 먼저 떼어 내 별도 목록으로 옮긴 뒤 스핀락 밖에서 응답하는
 * 이유: 응답 경로가 잠들 수 있다.
 */
void iommufd_auto_response_faults(struct iommufd_hw_pagetable *hwpt,
				  struct iommufd_attach_handle *handle)
{
	struct iommufd_fault *fault = hwpt->fault;	/* [한국어] 이 도메인에 연결된 폴트 큐. 없으면 할 일도 없다. */
	struct iopf_group *group, *next;	/* [한국어] 목록을 돌며 원소를 떼어 내므로 다음 원소를 미리 잡아 두는 safe 순회용. */
	struct list_head free_list;	/* [한국어] 스핀락 안에서 떼어 낸 묶음을 잠시 모아 두는 곳. 응답은 락 밖에서 한다. */
	unsigned long index;	/* [한국어] 응답 대기 xarray 를 훑을 때의 cookie 값. */

	if (!fault || !handle)	/* [한국어] 폴트 큐가 없거나 attach 핸들이 없으면 이 장치가 남긴 폴트도 없다. */
		return;
	INIT_LIST_HEAD(&free_list);	/* [한국어] 임시 목록을 비운 상태로 초기화한다. */

	mutex_lock(&fault->mutex);	/* [한국어] 읽기·쓰기 경로와 배타적으로 돌기 위한 뮤텍스. 응답 xarray 도 이것이 지킨다. */
	spin_lock(&fault->common.lock);	/* [한국어] deliver 목록은 인터럽트 문맥에서도 채워지므로 스핀락으로 지킨다. */
	list_for_each_entry_safe(group, next, &fault->common.deliver, node) {	/* [한국어] 아직 사용자가 읽어 가지 않은 폴트들을 훑는다. */
		if (group->attach_handle != &handle->handle)	/* [한국어] 떠나는 그 장치의 폴트만 고른다. 다른 장치 것은 그대로 둔다. */
			continue;
		list_move(&group->node, &free_list);	/* [한국어] 큐에서 빼서 임시 목록으로 옮긴다. 응답은 락을 놓은 뒤에 한다. */
	}
	spin_unlock(&fault->common.lock);	/* [한국어] 여기서 락을 놓는다 — 아래 응답 경로는 잠들 수 있다. */

	list_for_each_entry_safe(group, next, &free_list, node) {	/* [한국어] 옮겨 둔 묶음들에 차례로 응답한다. */
		list_del(&group->node);	/* [한국어] 임시 목록에서도 떼어 낸다. */
		iopf_group_response(group, IOMMU_PAGE_RESP_INVALID);	/* [한국어] "무효"로 답해 장치가 그 요청을 접게 한다. 답이 없으면 장치는 영원히 기다린다. */
		iopf_free_group(group);	/* [한국어] 응답을 마쳤으니 묶음을 해제한다. */
	}

	xa_for_each(&fault->response, index, group) {	/* [한국어] 사용자가 읽어 갔지만 응답이 오지 않은 폴트들. 이쪽도 대신 답해 준다. */
		if (group->attach_handle != &handle->handle)	/* [한국어] 역시 떠나는 장치의 것만 고른다. */
			continue;
		xa_erase(&fault->response, index);	/* [한국어] 대기 목록에서 지운다. 뒤늦게 사용자가 답해도 찾지 못하고 EINVAL 이 된다. */
		iopf_group_response(group, IOMMU_PAGE_RESP_INVALID);	/* [한국어] 같은 이유로 무효 응답을 보낸다. */
		iopf_free_group(group);	/* [한국어] 묶음 해제. */
	}
	mutex_unlock(&fault->mutex);	/* [한국어] 두 목록을 모두 비웠으니 뮤텍스를 놓는다. */
}

/*
 * [한국어]
 * iommufd_fault_destroy - 폴트 큐를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * 남아 있는 모든 폴트에 실패로 응답하고 정리한다.
 *
 * 원 주석이 락을 잡지 않는 근거를 밝힌다 — 이 시점에 참조 수가 0 이라
 * 다른 스레드가 이 포인터에 접근할 수 없다.
 */
void iommufd_fault_destroy(struct iommufd_object *obj)
{
	struct iommufd_eventq *eventq =	/* [한국어] 폴트 큐를 담는 공통 객체 포인터. 다음 줄의 container_of 가 값을 만든다. */
		container_of(obj, struct iommufd_eventq, obj);	/* [한국어] 공통 객체에서 이벤트 큐를 복원한다. obj 가 구조체 안에 박혀 있어 역산이 된다. */
	struct iommufd_fault *fault = eventq_to_fault(eventq);	/* [한국어] 공통 큐에서 폴트 전용 부분으로 내려간다. */
	struct iopf_group *group, *next;	/* [한국어] 목록을 파괴하며 도는 safe 순회용. */
	unsigned long index;	/* [한국어] 응답 대기 xarray 순회용 cookie. */

	/*
	 * The iommufd object's reference count is zero at this point.
	 * We can be confident that no other threads are currently
	 * accessing this pointer. Therefore, acquiring the mutex here
	 * is unnecessary.
	 */
	list_for_each_entry_safe(group, next, &fault->common.deliver, node) {	/* [한국어] 아직 읽히지 않은 폴트 전부. 장치별로 가리지 않는다 — 큐 자체가 사라지므로. */
		list_del(&group->node);	/* [한국어] 목록에서 뗀다. */
		iopf_group_response(group, IOMMU_PAGE_RESP_INVALID);	/* [한국어] 무효로 답해 장치를 풀어 준다. */
		iopf_free_group(group);	/* [한국어] 해제. */
	}
	xa_for_each(&fault->response, index, group) {	/* [한국어] 응답을 기다리던 것들도 같은 처리를 한다. */
		xa_erase(&fault->response, index);	/* [한국어] 항목을 지운다. */
		iopf_group_response(group, IOMMU_PAGE_RESP_INVALID);	/* [한국어] 무효 응답. */
		iopf_free_group(group);	/* [한국어] 해제. */
	}
	xa_destroy(&fault->response);	/* [한국어] 비어 있는 xarray 의 내부 노드까지 해제한다. */
	mutex_destroy(&fault->mutex);	/* [한국어] 디버그 설정에서 뮤텍스의 파괴를 기록한다. 이후 잠그면 오류로 잡힌다. */
}

/*
 * [한국어]
 * iommufd_compose_fault_message - 커널 폴트를 사용자 형식으로 옮긴다
 *
 * @fault: 커널 쪽 폴트.
 * @hwpt_fault: 채울 사용자 형식.
 * @idev: 폴트를 낸 장치.
 * @cookie: 응답을 짝지을 열쇠.
 *
 * 장치를 사용자가 아는 id 로 바꾸고, 응답에 쓸 cookie 를 실어 준다.
 * 사용자는 그 값을 그대로 되돌려 보내야 커널이 어느 폴트인지 안다.
 */
static void iommufd_compose_fault_message(struct iommu_fault *fault,
					  struct iommu_hwpt_pgfault *hwpt_fault,
					  struct iommufd_device *idev,
					  u32 cookie)
{
	hwpt_fault->flags = fault->prm.flags;	/* [한국어] PRI(Page Request Interface) 플래그. 마지막 요청인지, PASID 가 유효한지 등을 담는다. */
	hwpt_fault->dev_id = idev->obj.id;	/* [한국어] 커널 장치 포인터 대신 사용자가 아는 객체 id 로 바꿔 준다. */
	hwpt_fault->pasid = fault->prm.pasid;	/* [한국어] 어느 주소 공간에서 난 폴트인지. PASID 없는 장치면 의미 없는 값이다. */
	hwpt_fault->grpid = fault->prm.grpid;	/* [한국어] 하드웨어가 붙인 요청 묶음 번호. 응답할 때 하드웨어가 이것으로 짝을 찾는다. */
	hwpt_fault->perm = fault->prm.perm;	/* [한국어] 장치가 요구한 권한(읽기/쓰기/실행). 사용자가 그에 맞는 매핑을 만들어야 한다. */
	hwpt_fault->addr = fault->prm.addr;	/* [한국어] 폴트가 난 가상 주소. */
	hwpt_fault->length = 0;	/* [한국어] 현재 하드웨어는 길이를 알려 주지 않아 늘 0 이다. 나중을 위해 자리만 잡아 둔 필드. */
	hwpt_fault->cookie = cookie;	/* [한국어] 응답을 짝지을 열쇠. 사용자는 이 값을 그대로 되돌려 보내야 한다. */
}

/* Fetch the first node out of the fault->deliver list */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_fault_deliver_fetch - 큐에서 폴트 묶음 하나를 꺼낸다
 *
 * @fault: 대상 큐.
 * @return: 꺼낸 묶음, 비었으면 NULL.
 */
static struct iopf_group *
iommufd_fault_deliver_fetch(struct iommufd_fault *fault)
{
	struct list_head *list = &fault->common.deliver;	/* [한국어] 아직 읽히지 않은 폴트들의 목록. */
	struct iopf_group *group = NULL;	/* [한국어] 비어 있으면 NULL 그대로 돌려준다. */

	spin_lock(&fault->common.lock);	/* [한국어] 넣는 쪽이 인터럽트 문맥일 수 있어 스핀락이다. */
	if (!list_empty(list)) {	/* [한국어] 비어 있지 않을 때만 꺼낸다. */
		group = list_first_entry(list, struct iopf_group, node);	/* [한국어] 맨 앞 = 가장 먼저 들어온 것. 폴트는 도착 순서대로 전한다. */
		list_del(&group->node);	/* [한국어] 목록에서 뗀다. 실패하면 restore 로 되돌려야 한다. */
	}
	spin_unlock(&fault->common.lock);	/* [한국어] 락 해제. */
	return group;	/* [한국어] 꺼낸 묶음, 혹은 NULL. */
}

/* Restore a node back to the head of the fault->deliver list */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_fault_deliver_restore - 꺼냈던 묶음을 큐 앞에 되돌린다
 *
 * @fault: 대상 큐.
 * @group: 되돌릴 묶음.
 *
 * 사용자 버퍼가 모자라거나 복사에 실패했을 때 쓴다. 앞에 넣으므로
 * 다음 읽기가 그것부터 받는다 — 순서가 뒤바뀌지 않는다.
 */
static void iommufd_fault_deliver_restore(struct iommufd_fault *fault,
					  struct iopf_group *group)
{
	spin_lock(&fault->common.lock);	/* [한국어] 목록을 건드리므로 같은 스핀락. */
	list_add(&group->node, &fault->common.deliver);	/* [한국어] 뒤가 아니라 앞에 넣는다 — 이 묶음이 원래 맨 앞이었으므로 순서가 유지된다. */
	spin_unlock(&fault->common.lock);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * iommufd_fault_fops_read - 폴트를 사용자 버퍼로 읽힌다
 *
 * @filep: 폴트 큐 파일.
 * @buf: 사용자 버퍼.
 * @count: 그 크기.
 * @ppos: 오프셋(쓰지 않는다).
 * @return: 읽은 바이트, 하나도 못 읽었으면 오류.
 *
 * 폴트 묶음은 통째로 읽히거나 아예 읽히지 않는다 — 한 묶음의 일부만
 * 넘기면 사용자가 그것을 하나의 요청으로 다룰 수 없다.
 *
 * 읽어 가는 순간 cookie 를 배정해 응답 대기 목록에 넣는다. 그 배정이
 * 실패하면 폴트를 큐에 되돌린다.
 *
 * 오프셋을 허용하지 않는 이유: 이것은 스트림이지 파일이 아니다.
 */
static ssize_t iommufd_fault_fops_read(struct file *filep, char __user *buf,
				       size_t count, loff_t *ppos)
{
	size_t fault_size = sizeof(struct iommu_hwpt_pgfault);	/* [한국어] 폴트 한 건의 크기. 읽기·쓰기 단위가 이 값의 배수여야 한다. */
	struct iommufd_eventq *eventq = filep->private_data;	/* [한국어] 파일에 매달아 둔 큐. open 시점에 anon_inode_getfile 이 넣어 준 값이다. */
	struct iommufd_fault *fault = eventq_to_fault(eventq);	/* [한국어] 폴트 전용 부분으로 내려간다. */
	struct iommu_hwpt_pgfault data = {};	/* [한국어] 사용자에게 넘길 한 건짜리 버퍼. 남는 필드가 쓰레기 값이 되지 않게 0 으로 채운다. */
	struct iommufd_device *idev;	/* [한국어] 폴트를 낸 장치. 사용자에게는 그 객체 id 로 전한다. */
	struct iopf_group *group;	/* [한국어] 한 번에 다루는 폴트 묶음. 하드웨어가 묶어서 올린다. */
	struct iopf_fault *iopf;	/* [한국어] 묶음 안의 개별 폴트. */
	size_t done = 0;	/* [한국어] 지금까지 사용자 버퍼에 채운 바이트 수. */
	int rc = 0;	/* [한국어] 오류 코드. 한 건도 못 채웠을 때만 이것을 돌려준다. */

	if (*ppos || count % fault_size)	/* [한국어] 오프셋을 주거나 크기가 폴트 크기의 배수가 아니면 거절한다 — 이것은 스트림이지 임의 접근 파일이 아니다. */
		return -ESPIPE;

	mutex_lock(&fault->mutex);	/* [한국어] 응답 xarray 와 큐를 한꺼번에 다루므로 쓰기 경로와 배타적으로 돈다. */
	while ((group = iommufd_fault_deliver_fetch(fault))) {	/* [한국어] 큐가 비거나 버퍼가 찰 때까지 계속 꺼낸다. */
		if (done >= count ||	/* [한국어] 남은 자리가 이 묶음 전체를 담지 못하면 중단한다. */
		    group->fault_count * fault_size > count - done) {	/* [한국어] 묶음은 통째로 넘어가야 한다 — 일부만 주면 사용자가 그것을 하나의 요청으로 다룰 수 없다. */
			iommufd_fault_deliver_restore(fault, group);	/* [한국어] 자리가 없으니 꺼낸 것을 큐 앞에 되돌린다. */
			break;
		}

		rc = xa_alloc(&fault->response, &group->cookie, group,	/* [한국어] 응답을 짝지을 cookie 를 배정하며 대기 목록에 넣는다. 32비트로 제한하는 이유는 사용자 구조체의 필드 폭이 그렇기 때문. */
			      xa_limit_32b, GFP_KERNEL);
		if (rc) {	/* [한국어] 배정에 실패하면(메모리 부족) 이 묶음은 아직 전할 수 없다. */
			iommufd_fault_deliver_restore(fault, group);	/* [한국어] 되돌리고 중단한다. 이미 채운 만큼은 그대로 돌려준다. */
			break;
		}

		idev = to_iommufd_handle(group->attach_handle)->idev;	/* [한국어] attach 핸들에서 iommufd 장치 객체를 얻는다 — 사용자에게 알릴 id 의 출처. */
		list_for_each_entry(iopf, &group->faults, list) {	/* [한국어] 묶음 안의 폴트를 하나씩 사용자 형식으로 옮긴다. */
			iommufd_compose_fault_message(&iopf->fault,	/* [한국어] 커널 표현을 사용자 ABI 로 변환한다. 같은 cookie 가 묶음 전체에 붙는다. */
						      &data, idev,
						      group->cookie);
			if (copy_to_user(buf + done, &data, fault_size)) {	/* [한국어] 사용자 버퍼로 복사. 페이지 폴트가 날 수 있어 실패를 다뤄야 한다. */
				xa_erase(&fault->response, group->cookie);	/* [한국어] 전하지 못했으니 방금 배정한 cookie 를 거둬들인다. */
				iommufd_fault_deliver_restore(fault, group);	/* [한국어] 묶음을 큐로 되돌려 다음 읽기에 다시 시도하게 한다. */
				rc = -EFAULT;	/* [한국어] 사용자 주소가 잘못됐다는 표준 오류. */
				break;
			}
			done += fault_size;	/* [한국어] 한 건을 채웠으니 다음 자리로 옮긴다. */
		}
	}
	mutex_unlock(&fault->mutex);	/* [한국어] 뮤텍스 해제. */

	return done == 0 ? rc : done;	/* [한국어] 한 건이라도 넘겼으면 그 바이트 수가 성공이다. 오류는 다음 읽기에서 다시 만난다. */
}

/*
 * [한국어]
 * iommufd_fault_fops_write - 사용자의 폴트 응답을 받는다
 *
 * @filep: 폴트 큐 파일.
 * @buf: 사용자 버퍼.
 * @count: 그 크기.
 * @ppos: 오프셋(쓰지 않는다).
 * @return: 처리한 바이트, 하나도 못 했으면 오류.
 *
 * cookie 로 어느 폴트인지 찾아 하드웨어에 응답을 전한다. 그 순간 장치가
 * 멈춰 있던 요청을 다시 시도한다.
 *
 * static_assert 둘이 사용자 값과 커널 값이 같은지 컴파일 시 확인한다 —
 * 그래야 그대로 넘길 수 있다.
 *
 * 모르는 응답 코드를 거절하는 이유: 사용자가 임의의 값을 하드웨어에
 * 전하게 두면 안 된다.
 */
static ssize_t iommufd_fault_fops_write(struct file *filep, const char __user *buf,
					size_t count, loff_t *ppos)
{
	size_t response_size = sizeof(struct iommu_hwpt_page_response);	/* [한국어] 응답 한 건의 크기. 쓰기 단위도 이 배수여야 한다. */
	struct iommufd_eventq *eventq = filep->private_data;	/* [한국어] 파일에 매달린 큐. */
	struct iommufd_fault *fault = eventq_to_fault(eventq);	/* [한국어] 폴트 전용 부분. */
	struct iommu_hwpt_page_response response;	/* [한국어] 사용자에게서 한 건씩 받아 담을 버퍼. */
	struct iopf_group *group;	/* [한국어] cookie 로 찾아낸, 응답을 기다리던 묶음. */
	size_t done = 0;	/* [한국어] 지금까지 처리한 바이트 수. */
	int rc = 0;	/* [한국어] 오류 코드. */

	if (*ppos || count % response_size)	/* [한국어] 읽기와 같은 이유로 오프셋과 어긋난 크기를 거절한다. */
		return -ESPIPE;

	mutex_lock(&fault->mutex);	/* [한국어] 응답 xarray 를 지키는 뮤텍스. */
	while (count > done) {	/* [한국어] 사용자가 준 만큼 반복한다. */
		if (copy_from_user(&response, buf + done, response_size)) {	/* [한국어] 사용자 버퍼에서 한 건을 가져온다. */
			rc = -EFAULT;	/* [한국어] 주소가 잘못됐다. */
			break;
		}

		static_assert((int)IOMMUFD_PAGE_RESP_SUCCESS ==	/* [한국어] 사용자 ABI 의 값과 커널 내부 값이 같은지 컴파일 시 확인한다 — 같아야 변환 없이 그대로 넘길 수 있다. */
			      (int)IOMMU_PAGE_RESP_SUCCESS);
		static_assert((int)IOMMUFD_PAGE_RESP_INVALID ==	/* [한국어] 실패 코드도 같은 이유로 확인한다. */
			      (int)IOMMU_PAGE_RESP_INVALID);
		if (response.code != IOMMUFD_PAGE_RESP_SUCCESS &&	/* [한국어] 아는 응답 코드 둘 외에는 받지 않는다. */
		    response.code != IOMMUFD_PAGE_RESP_INVALID) {	/* [한국어] 임의의 값을 하드웨어에 그대로 전하게 두면 안 된다. */
			rc = -EINVAL;	/* [한국어] 알 수 없는 코드. */
			break;
		}

		group = xa_erase(&fault->response, response.cookie);	/* [한국어] cookie 로 묶음을 찾아 동시에 대기 목록에서 지운다 — 찾기와 지우기가 한 연산이라 두 번 응답하는 경합이 없다. */
		if (!group) {	/* [한국어] 없는 cookie 다. 이미 응답했거나, 장치가 떠나며 대신 응답됐거나, 사용자가 지어낸 값이다. */
			rc = -EINVAL;	/* [한국어] 짝을 찾지 못했다. */
			break;
		}

		iopf_group_response(group, response.code);	/* [한국어] 하드웨어에 응답을 전한다. 성공이면 장치가 멈춰 있던 요청을 다시 시도한다. */
		iopf_free_group(group);	/* [한국어] 묶음 해제. */
		done += response_size;	/* [한국어] 다음 응답으로. */
	}
	mutex_unlock(&fault->mutex);	/* [한국어] 뮤텍스 해제. */

	return done == 0 ? rc : done;	/* [한국어] 한 건이라도 처리했으면 그 바이트 수를 돌려준다. */
}

/* IOMMUFD_OBJ_VEVENTQ Functions */

/*
 * [한국어]
 * iommufd_veventq_abort - vIOMMU 이벤트 큐를 되돌린다
 *
 * @obj: 되돌릴 객체.
 *
 * 남은 이벤트를 모두 버린다 — 이쪽은 응답할 상대가 없어 그냥 놓으면 된다.
 *
 * 잃어버림 표시는 큐 안에 정적으로 박혀 있으므로 해제하지 않는다.
 *
 * 호출자가 vIOMMU 의 쓰기 락을 쥐고 있어야 한다 — 그 목록에서 자기를
 * 빼기 때문이다.
 */
void iommufd_veventq_abort(struct iommufd_object *obj)
{
	struct iommufd_eventq *eventq =	/* [한국어] 같은 방식으로 공통 객체를 복원한다. */
		container_of(obj, struct iommufd_eventq, obj);	/* [한국어] 공통 객체에서 이벤트 큐를 복원한다. */
	struct iommufd_veventq *veventq = eventq_to_veventq(eventq);	/* [한국어] vIOMMU 전용 부분으로 내려간다. */
	struct iommufd_viommu *viommu = veventq->viommu;	/* [한국어] 이 큐가 매달린 vIOMMU. 목록에서 자기를 빼야 한다. */
	struct iommufd_vevent *cur, *next;	/* [한국어] 목록을 파괴하며 도는 safe 순회용. */

	lockdep_assert_held_write(&viommu->veventqs_rwsem);	/* [한국어] 호출자가 vIOMMU 의 쓰기 락을 쥐고 있어야 한다 — 아래에서 그 목록을 건드린다. */

	list_for_each_entry_safe(cur, next, &eventq->deliver, node) {	/* [한국어] 아직 읽히지 않은 이벤트 전부. 폴트와 달리 응답할 상대가 없어 그냥 버린다. */
		list_del(&cur->node);	/* [한국어] 목록에서 뗀다. */
		if (cur != &veventq->lost_events_header)	/* [한국어] 잃어버림 표시는 큐 구조체 안에 정적으로 박혀 있어 해제하면 안 된다. */
			kfree(cur);	/* [한국어] 보통 이벤트는 할당된 것이므로 해제한다. */
	}

	refcount_dec(&viommu->obj.users);	/* [한국어] 생성 때 올려 둔 vIOMMU 참조를 놓는다. */
	list_del(&veventq->node);	/* [한국어] vIOMMU 의 큐 목록에서 자기를 뺀다. 쓰기 락 아래라 안전하다. */
}

/*
 * [한국어]
 * iommufd_veventq_destroy - vIOMMU 이벤트 큐를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * abort 와 같되 락을 여기서 잡는다.
 */
void iommufd_veventq_destroy(struct iommufd_object *obj)
{
	struct iommufd_veventq *veventq = eventq_to_veventq(	/* [한국어] 공통 객체에서 vIOMMU 이벤트 큐를 복원한다. */
		container_of(obj, struct iommufd_eventq, obj));

	down_write(&veventq->viommu->veventqs_rwsem);	/* [한국어] abort 가 요구하는 쓰기 락을 여기서 잡는다. */
	iommufd_veventq_abort(obj);	/* [한국어] 실제 정리는 abort 와 같다. */
	up_write(&veventq->viommu->veventqs_rwsem);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * iommufd_veventq_deliver_fetch - 큐에서 이벤트 하나를 꺼낸다
 *
 * @veventq: 대상 큐.
 * @return: 꺼낸 이벤트, 비었으면 NULL.
 *
 * 잃어버림 표시를 만나면 복사본을 만들어 돌려준다. 원 주석이 그 이유를
 * 밝힌다 — 사용자 복사에 실패하면 되돌려 놓아야 하는데, 원본은 큐 구조체
 * 안에 정적으로 박혀 있어 목록 조작만으로는 상태를 되살릴 수 없다.
 *
 * 복사에 실패하면 아무것도 꺼내지 않고 돌아간다 — 스핀락 아래라
 * 기다릴 수 없고, 다음 읽기에 다시 시도하면 된다.
 */
static struct iommufd_vevent *
iommufd_veventq_deliver_fetch(struct iommufd_veventq *veventq)
{
	struct iommufd_eventq *eventq = &veventq->common;	/* [한국어] 공통 부분(목록과 락). */
	struct list_head *list = &eventq->deliver;	/* [한국어] 아직 읽히지 않은 이벤트 목록. */
	struct iommufd_vevent *vevent = NULL;	/* [한국어] 돌려줄 이벤트. 복사본을 만들었는지 여부의 표시로도 쓰인다. */

	spin_lock(&eventq->lock);	/* [한국어] 넣는 쪽이 인터럽트 스레드일 수 있어 스핀락. */
	if (!list_empty(list)) {	/* [한국어] 비어 있지 않을 때만 꺼낸다. */
		struct iommufd_vevent *next;	/* [한국어] 맨 앞 항목을 가리킬 지역 변수. */

		next = list_first_entry(list, struct iommufd_vevent, node);	/* [한국어] 도착 순서대로 맨 앞을 본다. */
		/* Make a copy of the lost_events_header for copy_to_user */
		if (next == &veventq->lost_events_header) {	/* [한국어] 그것이 잃어버림 표시라면 원본을 그대로 넘길 수 없다. */
			vevent = kzalloc_obj(*vevent, GFP_ATOMIC);	/* [한국어] 스핀락 아래라 기다릴 수 없는 할당. 실패해도 되도록 설계되어 있다. */
			if (!vevent)	/* [한국어] 할당이 실패했으면(스핀락 아래의 GFP_ATOMIC) 이번에는 아무것도 꺼내지 않는다. */
				goto out_unlock;	/* [한국어] 할당에 실패하면 아무것도 꺼내지 않고 돌아간다 — 다음 읽기에 다시 시도하면 된다. */
		}
		list_del(&next->node);	/* [한국어] 목록에서 뗀다. */
		if (vevent)	/* [한국어] 복사본을 만들었다면 */
			memcpy(vevent, next, sizeof(*vevent));	/* [한국어] 그 안에 내용을 옮긴다. 원본은 큐 구조체에 남아 다시 쓰인다. */
		else
			vevent = next;	/* [한국어] 보통 이벤트는 그대로 넘긴다. */
	}
out_unlock:	/* [한국어] 실패 경로가 합류하는 지점. 락을 놓고 NULL 을 돌려준다. */
	spin_unlock(&eventq->lock);	/* [한국어] 락 해제. */
	return vevent;	/* [한국어] 꺼낸 이벤트, 혹은 NULL. */
}

/*
 * [한국어]
 * iommufd_veventq_deliver_restore - 꺼냈던 이벤트를 큐 앞에 되돌린다
 *
 * @veventq: 대상 큐.
 * @vevent: 되돌릴 이벤트.
 *
 * 잃어버림 표시였으면 복사본을 버리고, 큐가 비었을 때만 원본을 되돌린다.
 * 그 표시는 늘 맨 뒤에 있어야 하는데, 큐에 다른 이벤트가 남아 있으면
 * 앞에 넣는 것이 그 규칙을 깨기 때문이다.
 */
static void iommufd_veventq_deliver_restore(struct iommufd_veventq *veventq,
					    struct iommufd_vevent *vevent)
{
	struct iommufd_eventq *eventq = &veventq->common;	/* [한국어] 공통 부분. */
	struct list_head *list = &eventq->deliver;	/* [한국어] 되돌릴 목록. */

	spin_lock(&eventq->lock);	/* [한국어] 목록을 건드리므로 스핀락. */
	if (vevent_for_lost_events_header(vevent)) {	/* [한국어] 되돌리려는 것이 잃어버림 표시(의 복사본)인지 본다. */
		/* Remove the copy of the lost_events_header */
		kfree(vevent);	/* [한국어] 복사본은 버린다 — 원본이 큐 구조체 안에 그대로 있다. */
		vevent = NULL;	/* [한국어] 아무것도 되돌리지 않는 것이 기본이다. */
		/* An empty list needs the lost_events_header back */
		if (list_empty(list))	/* [한국어] 그 사이 큐가 완전히 비었다면 */
			vevent = &veventq->lost_events_header;	/* [한국어] 원본 표시를 되돌린다. 비어 있으니 앞에 넣어도 "맨 뒤" 규칙이 깨지지 않는다. */
	}
	if (vevent)	/* [한국어] 되돌릴 것이 있으면 */
		list_add(&vevent->node, list);	/* [한국어] 맨 앞에 넣어 다음 읽기가 그것부터 받게 한다. */
	spin_unlock(&eventq->lock);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * iommufd_veventq_fops_read - vIOMMU 이벤트를 사용자 버퍼로 읽힌다
 *
 * @filep: 이벤트 큐 파일.
 * @buf: 사용자 버퍼.
 * @count: 그 크기.
 * @ppos: 오프셋(쓰지 않는다).
 * @return: 읽은 바이트, 하나도 못 읽었으면 오류.
 *
 * 이벤트는 머리말과 내용으로 나뉘어 있고, 둘을 이어 붙여 넘긴다.
 * 잃어버림 표시는 내용이 없어 머리말만 확인하면 된다.
 *
 * 큐에 든 개수를 줄이는 것이 잃어버림 표시에는 해당하지 않는다 —
 * 그것은 애초에 세지 않았기 때문이다.
 */
static ssize_t iommufd_veventq_fops_read(struct file *filep, char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct iommufd_eventq *eventq = filep->private_data;	/* [한국어] 파일에 매달린 큐. */
	struct iommufd_veventq *veventq = eventq_to_veventq(eventq);	/* [한국어] vIOMMU 전용 부분. */
	struct iommufd_vevent_header *hdr;	/* [한국어] 이벤트 머리말. 종류와 플래그, 일련번호가 들어 있다. */
	struct iommufd_vevent *cur;	/* [한국어] 지금 다루는 이벤트. */
	size_t done = 0;	/* [한국어] 채운 바이트 수. */
	int rc = 0;	/* [한국어] 오류 코드. */

	if (*ppos)	/* [한국어] 오프셋 지정을 거절한다. 크기 배수 검사가 없는 것은 이벤트 길이가 종류마다 다르기 때문. */
		return -ESPIPE;

	while ((cur = iommufd_veventq_deliver_fetch(veventq))) {	/* [한국어] 큐가 비거나 버퍼가 찰 때까지 꺼낸다. */
		/* Validate the remaining bytes against the header size */
		if (done >= count || sizeof(*hdr) > count - done) {	/* [한국어] 머리말조차 들어가지 않으면 중단한다. */
			iommufd_veventq_deliver_restore(veventq, cur);	/* [한국어] 자리가 없으니 되돌린다. */
			break;
		}
		hdr = &cur->header;	/* [한국어] 머리말은 이벤트 구조체 안에 박혀 있다. */

		/* If being a normal vEVENT, validate against the full size */
		if (!vevent_for_lost_events_header(cur) &&	/* [한국어] 보통 이벤트라면 내용까지 다 들어가는지 확인한다. */
		    sizeof(hdr) + cur->data_len > count - done) {	/* [한국어] 머리말과 내용을 이어 붙인 크기가 남은 자리를 넘으면 안 된다 — 이벤트는 통째로 전해져야 한다. */
			iommufd_veventq_deliver_restore(veventq, cur);	/* [한국어] 자리가 모자라니 되돌린다. */
			break;
		}

		if (copy_to_user(buf + done, hdr, sizeof(*hdr))) {	/* [한국어] 머리말을 먼저 복사한다. */
			iommufd_veventq_deliver_restore(veventq, cur);	/* [한국어] 실패했으니 되돌려 다음 읽기에 다시 시도하게 한다. */
			rc = -EFAULT;	/* [한국어] 사용자 주소가 잘못됐다. */
			break;
		}
		done += sizeof(*hdr);	/* [한국어] 머리말만큼 전진. */

		if (cur->data_len &&	/* [한국어] 내용이 있는 이벤트만 이어서 복사한다. 잃어버림 표시는 내용이 없다. */
		    copy_to_user(buf + done, cur->event_data, cur->data_len)) {	/* [한국어] 드라이버가 채워 넣은 하드웨어 고유 이벤트 본문. */
			iommufd_veventq_deliver_restore(veventq, cur);	/* [한국어] 머리말은 이미 넘어갔지만 이벤트를 되돌린다 — 사용자는 done 만큼만 읽은 것으로 보고, 중복된 머리말은 다음 읽기에서 다시 온다. */
			rc = -EFAULT;	/* [한국어] 복사 실패. */
			break;
		}
		spin_lock(&eventq->lock);	/* [한국어] 큐에 든 개수를 고치므로 스핀락. */
		if (!vevent_for_lost_events_header(cur))	/* [한국어] 잃어버림 표시는 애초에 세지 않았으므로 줄이지 않는다. */
			veventq->num_events--;	/* [한국어] 한 건을 내보냈으니 깊이 여유가 하나 생긴다. 이 값이 depth 에 닿으면 새 이벤트가 버려진다. */
		spin_unlock(&eventq->lock);	/* [한국어] 락 해제. */
		done += cur->data_len;	/* [한국어] 내용만큼 전진. */
		kfree(cur);	/* [한국어] 넘겼으니 해제한다. 잃어버림 표시라면 이것은 복사본이다. */
	}

	return done == 0 ? rc : done;	/* [한국어] 한 건이라도 넘겼으면 성공으로 본다. */
}

/* Common Event Queue Functions */

/*
 * [한국어]
 * iommufd_eventq_fops_poll - 두 큐가 공유하는 poll 구현
 *
 * @filep: 큐 파일.
 * @wait: poll 대기 표.
 * @return: 준비된 이벤트 비트.
 *
 * 읽을 것이 있으면 EPOLLIN 이다. 폴트 큐만 EPOLLOUT 을 늘 세우는데,
 * 응답 쓰기는 큐 상태와 무관하게 언제나 받을 수 있기 때문이다.
 */
static __poll_t iommufd_eventq_fops_poll(struct file *filep,
					 struct poll_table_struct *wait)
{
	struct iommufd_eventq *eventq = filep->private_data;	/* [한국어] 파일에 매달린 큐. 두 종류가 이 함수를 공유한다. */
	__poll_t pollflags = 0;	/* [한국어] 돌려줄 준비 상태 비트. */

	if (eventq->obj.type == IOMMUFD_OBJ_FAULT)	/* [한국어] 폴트 큐만 쓰기를 받는다. */
		pollflags |= EPOLLOUT;	/* [한국어] 응답 쓰기는 큐 상태와 무관하게 언제나 받을 수 있어 늘 세운다. */

	poll_wait(filep, &eventq->wait_queue, wait);	/* [한국어] 이 파일의 대기열에 등록한다. 새 이벤트가 들어오면 wake_up 이 여기를 깨운다. */
	spin_lock(&eventq->lock);	/* [한국어] 목록을 들여다보므로 스핀락. */
	if (!list_empty(&eventq->deliver))	/* [한국어] 읽을 것이 있으면 */
		pollflags |= EPOLLIN | EPOLLRDNORM;	/* [한국어] 읽기 준비됨. 두 비트를 함께 세우는 것이 관례다. */
	spin_unlock(&eventq->lock);	/* [한국어] 락 해제. */

	return pollflags;	/* [한국어] 준비 상태를 돌려준다. */
}

/*
 * [한국어]
 * iommufd_eventq_fops_release - 큐 파일이 닫힐 때
 *
 * @inode: 닫히는 노드.
 * @filep: 그 파일.
 * @return: 늘 0.
 *
 * 파일이 들고 있던 객체 참조와 문맥 참조를 놓는다. 객체 자체는 사용자가
 * IOMMU_DESTROY 로 지워야 사라진다.
 */
static int iommufd_eventq_fops_release(struct inode *inode, struct file *filep)
{
	struct iommufd_eventq *eventq = filep->private_data;	/* [한국어] 닫히는 파일이 가리키던 큐. */

	refcount_dec(&eventq->obj.users);	/* [한국어] 파일이 들고 있던 객체 참조를 놓는다. 객체 자체는 IOMMU_DESTROY 로 지워야 사라진다. */
	iommufd_ctx_put(eventq->ictx);	/* [한국어] 문맥 참조도 놓는다 — 큐 파일이 살아 있는 동안 문맥이 사라지면 안 됐다. */
	return 0;	/* [한국어] close 는 실패하지 않는다. */
}

/*
 * [한국어] 두 큐의 파일 연산을 찍어 내는 매크로.
 * 읽기와 쓰기만 다르고 나머지는 같아, 그 둘만 인자로 받는다.
 * vIOMMU 이벤트 큐는 쓰기가 없어 NULL 을 준다.
 */
#define INIT_EVENTQ_FOPS(read_op, write_op)                                    \
	((const struct file_operations){                                       \
		.owner = THIS_MODULE,                                          \
		.open = nonseekable_open,                                      \
		.read = read_op,                                               \
		.write = write_op,                                             \
		.poll = iommufd_eventq_fops_poll,                              \
		.release = iommufd_eventq_fops_release,                        \
	})	/* [한국어] 매크로가 만들어 내는 익명 구조체 리터럴의 끝. */

/*
 * [한국어]
 * iommufd_eventq_init - 큐의 공통 부분을 세우고 파일을 연다
 *
 * @eventq: 세울 큐.
 * @name: 파일 이름(디버깅에 보인다).
 * @ictx: 문맥.
 * @fops: 그 큐의 파일 연산.
 * @return: 배정된 파일 디스크립터 번호, 실패하면 음수.
 *
 * 익명 inode 로 파일을 만든다 — 파일 시스템에 이름이 없고 오직 이
 * 디스크립터로만 닿을 수 있다.
 *
 * 번호를 잡되 아직 설치하지 않는 것이 요점이다. 사용자에게 응답을
 * 보내는 데 실패할 수 있어, 성공이 확정된 뒤에야 설치한다 — 그러지
 * 않으면 사용자가 모르는 디스크립터가 열린 채 남는다.
 *
 * 원 주석대로 파일 자체는 실패 시 코어가 놓아 준다.
 */
static int iommufd_eventq_init(struct iommufd_eventq *eventq, char *name,
			       struct iommufd_ctx *ictx,
			       const struct file_operations *fops)
{
	struct file *filep;	/* [한국어] 만들어 낼 익명 파일. */

	spin_lock_init(&eventq->lock);	/* [한국어] 목록을 지킬 스핀락 초기화. */
	INIT_LIST_HEAD(&eventq->deliver);	/* [한국어] 전달 대기 목록을 빈 상태로. */
	init_waitqueue_head(&eventq->wait_queue);	/* [한국어] 읽기를 기다리는 쪽이 잠들 대기열. */

	/* The filep is fput() by the core code during failure */
	filep = anon_inode_getfile(name, fops, eventq, O_RDWR);	/* [한국어] 파일 시스템에 이름 없는 파일을 만든다. private_data 로 큐를 매달아 두어 파일 연산이 그것을 찾는다. */
	if (IS_ERR(filep))	/* [한국어] 만들지 못했으면 그 오류를 그대로 올린다. */
		return PTR_ERR(filep);	/* [한국어] 오류 포인터를 음수 코드로 바꾼다. */

	eventq->ictx = ictx;	/* [한국어] 문맥을 기억한다. */
	iommufd_ctx_get(eventq->ictx);	/* [한국어] 파일이 살아 있는 동안 문맥이 사라지지 않게 참조를 든다. */
	eventq->filep = filep;	/* [한국어] 나중에 fd_install 할 때 쓴다. */
	refcount_inc(&eventq->obj.users);	/* [한국어] 파일이 객체를 붙잡는 참조. */

	return get_unused_fd_flags(O_CLOEXEC);	/* [한국어] 번호만 잡아 두고 아직 설치하지 않는다 — 응답에 실패하면 되돌려야 하기 때문. exec 때 자동으로 닫히게 한다. */
}

static const struct file_operations iommufd_fault_fops =	/* [한국어] 폴트 큐의 파일 연산. 읽기와 쓰기를 모두 갖는다. */
	INIT_EVENTQ_FOPS(iommufd_fault_fops_read, iommufd_fault_fops_write);

/*
 * [한국어]
 * iommufd_fault_alloc - IOMMU_FAULT_QUEUE_ALLOC 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 폴트 큐를 만들고 그 파일 디스크립터를 돌려준다. 사용자는 그 뒤
 * HWPT 를 만들 때 이 큐의 id 를 지정해 연결한다.
 *
 * cookie 배정에 xarray 를 쓰되 1 부터 시작하게 하는 이유: 0 은
 * "없음"과 구별되지 않는다.
 */
int iommufd_fault_alloc(struct iommufd_ucmd *ucmd)
{
	struct iommu_fault_alloc *cmd = ucmd->cmd;	/* [한국어] 사용자에게서 온 명령 버퍼. */
	struct iommufd_fault *fault;	/* [한국어] 만들 폴트 큐. */
	int fdno;	/* [한국어] 배정받은 파일 디스크립터 번호. */
	int rc;	/* [한국어] 오류 코드. */

	if (cmd->flags)	/* [한국어] 아직 정의된 플래그가 없다. 알 수 없는 값을 조용히 무시하면 나중에 의미를 붙일 수 없다. */
		return -EOPNOTSUPP;

	fault = __iommufd_object_alloc_ucmd(ucmd, fault, IOMMUFD_OBJ_FAULT,	/* [한국어] 객체를 만들고 명령에 매단다 — 실패 시 되돌리기와 성공 시 finalize 를 코어가 알아서 한다. */
					    common.obj);
	if (IS_ERR(fault))	/* [한국어] 할당 실패. */
		return PTR_ERR(fault);	/* [한국어] 오류 코드로 바꿔 올린다. */

	xa_init_flags(&fault->response, XA_FLAGS_ALLOC1);	/* [한국어] cookie 배정용 xarray. ALLOC1 은 1 부터 배정한다는 뜻 — 0 은 "없음"과 구별되지 않는다. */
	mutex_init(&fault->mutex);	/* [한국어] 읽기·쓰기와 자동 응답을 갈라 놓는 뮤텍스. */

	fdno = iommufd_eventq_init(&fault->common, "[iommufd-pgfault]",	/* [한국어] 공통 부분을 세우고 파일을 연다. 이름은 /proc/<pid>/fd 에 보인다. */
				   ucmd->ictx, &iommufd_fault_fops);
	if (fdno < 0)	/* [한국어] 파일을 열지 못했으면 그대로 올린다. 객체는 ucmd 정리가 되돌린다. */
		return fdno;	/* [한국어] 음수 오류 코드. */

	cmd->out_fault_id = fault->common.obj.id;	/* [한국어] HWPT 를 만들 때 이 큐를 지목할 id. */
	cmd->out_fault_fd = fdno;	/* [한국어] 폴트를 읽고 응답을 쓸 디스크립터. */

	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 두 출력 값을 사용자 버퍼에 되돌려 쓴다. */
	if (rc)	/* [한국어] 쓰지 못했다면 사용자는 이 번호를 모른다. */
		goto out_put_fdno;	/* [한국어] 잡아 둔 번호를 놓아 준다. */

	fd_install(fdno, fault->common.filep);	/* [한국어] 이제야 번호에 파일을 붙인다. 이 순간부터 사용자가 그 디스크립터를 쓸 수 있다. */

	return 0;	/* [한국어] 성공. */
out_put_fdno:	/* [한국어] 응답 실패 경로. 설치하지 않은 fd 번호를 반납한다. */
	put_unused_fd(fdno);	/* [한국어] 설치하지 않은 번호를 반납한다. */
	return rc;	/* [한국어] 응답 실패 코드. */
}

/*
 * [한국어]
 * iommufd_fault_iopf_handler - 드라이버가 낸 폴트를 큐에 넣는다
 *
 * @group: 폴트 묶음.
 * @return: 늘 0.
 *
 * HWPT 에 폴트 큐를 연결하면 도메인의 폴트 처리기가 이 함수로 바뀐다.
 * 그때부터 그 도메인의 폴트가 사용자 공간으로 간다.
 *
 * 실행 컨텍스트: iommu 코어의 폴트 작업 큐. 스핀락으로 넣고 곧바로
 * 읽는 쪽을 깨운다.
 */
int iommufd_fault_iopf_handler(struct iopf_group *group)
{
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 폴트가 난 도메인. */
	struct iommufd_fault *fault;	/* [한국어] 그 도메인에 연결된 폴트 큐. */

	hwpt = group->attach_handle->domain->iommufd_hwpt;	/* [한국어] iommu 코어의 도메인에서 iommufd 쪽 객체로 거슬러 올라간다. */
	fault = hwpt->fault;	/* [한국어] 큐를 꺼낸다. 이 처리기가 걸려 있다는 것 자체가 큐가 있다는 뜻이다. */

	spin_lock(&fault->common.lock);	/* [한국어] 인터럽트 문맥에서도 불릴 수 있어 스핀락. */
	list_add_tail(&group->node, &fault->common.deliver);	/* [한국어] 맨 뒤에 붙여 도착 순서를 지킨다. */
	spin_unlock(&fault->common.lock);	/* [한국어] 락 해제. */

	wake_up_interruptible(&fault->common.wait_queue);	/* [한국어] 읽기를 기다리며 잠든 쪽과 poll 을 깨운다. */

	return 0;	/* [한국어] 코어에는 늘 성공을 알린다 — 실제 응답은 사용자가 나중에 한다. */
}

static const struct file_operations iommufd_veventq_fops =	/* [한국어] vIOMMU 이벤트 큐의 파일 연산. 쓰기가 NULL 이라 write() 는 EINVAL 이 된다. */
	INIT_EVENTQ_FOPS(iommufd_veventq_fops_read, NULL);

/*
 * [한국어]
 * iommufd_veventq_alloc - IOMMU_VEVENTQ_ALLOC 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * vIOMMU 에 이벤트 큐를 하나 붙인다. 종류마다 하나뿐이라 이미 있으면
 * 거절한다.
 *
 * 잃어버림 표시의 플래그를 미리 세워 두는 것이 눈에 띈다 — 큐가 가득 찼을
 * 때 또 할당할 수는 없으므로, 그 항목을 처음부터 준비해 둔다.
 *
 * 이 객체는 abort 를 가져 _ucmd 할당기를 쓸 수 없다. vIOMMU 의 쓰기 락
 * 안에서 되돌려야 하기 때문이다.
 */
int iommufd_veventq_alloc(struct iommufd_ucmd *ucmd)
{
	struct iommu_veventq_alloc *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */
	struct iommufd_veventq *veventq;	/* [한국어] 만들 이벤트 큐. */
	struct iommufd_viommu *viommu;	/* [한국어] 큐를 매달 vIOMMU. */
	int fdno;	/* [한국어] 배정받은 디스크립터 번호. */
	int rc;	/* [한국어] 오류 코드. */

	if (cmd->flags || cmd->__reserved ||	/* [한국어] 정의되지 않은 플래그와 예약 필드는 0 이어야 한다. */
	    cmd->type == IOMMU_VEVENTQ_TYPE_DEFAULT)	/* [한국어] 기본 종류는 자리표시자라 실제로 만들 수 없다. */
		return -EOPNOTSUPP;
	if (!cmd->veventq_depth)	/* [한국어] 깊이 0 은 아무 이벤트도 담지 못한다. */
		return -EINVAL;	/* [한국어] 잘못된 인자. */

	viommu = iommufd_get_viommu(ucmd, cmd->viommu_id);	/* [한국어] id 로 vIOMMU 를 찾고 참조를 든다. */
	if (IS_ERR(viommu))	/* [한국어] 없는 id 이거나 종류가 다르다. */
		return PTR_ERR(viommu);	/* [한국어] 오류를 올린다. */

	down_write(&viommu->veventqs_rwsem);	/* [한국어] 큐 목록에 넣어야 하므로 쓰기 락. 중복 검사와 삽입이 한 락 안에서 일어나야 경합이 없다. */

	if (iommufd_viommu_find_veventq(viommu, cmd->type)) {	/* [한국어] 같은 종류의 큐가 이미 있으면 */
		rc = -EEXIST;	/* [한국어] 하나만 허용한다 — 같은 사건이 두 큐로 갈라지면 순서를 알 수 없다. */
		goto out_unlock_veventqs;	/* [한국어] 락을 놓고 나간다. */
	}

	veventq = __iommufd_object_alloc(ucmd->ictx, veventq,	/* [한국어] _ucmd 판이 아닌 것에 주목. 이 객체는 abort 를 가지고, 그 abort 는 이 쓰기 락 안에서 돌아야 한다. */
					 IOMMUFD_OBJ_VEVENTQ, common.obj);
	if (IS_ERR(veventq)) {	/* [한국어] 할당 실패. */
		rc = PTR_ERR(veventq);	/* [한국어] 오류 코드를 꺼낸다. */
		goto out_unlock_veventqs;	/* [한국어] 아직 목록에 넣지 않았으므로 락만 놓으면 된다. */
	}

	veventq->type = cmd->type;	/* [한국어] 이 큐가 받을 이벤트 종류. */
	veventq->viommu = viommu;	/* [한국어] 매달린 vIOMMU 를 기억한다. */
	refcount_inc(&viommu->obj.users);	/* [한국어] 큐가 살아 있는 동안 vIOMMU 가 사라지지 않게 참조를 든다. */
	veventq->depth = cmd->veventq_depth;	/* [한국어] 담을 수 있는 최대 이벤트 수. 넘치면 잃어버림 표시로 바뀐다. */
	list_add_tail(&veventq->node, &viommu->veventqs);	/* [한국어] vIOMMU 의 큐 목록에 넣는다. 이제부터 abort 로 되돌려야 한다. */
	veventq->lost_events_header.header.flags =	/* [한국어] 넘침 표시를 미리 준비해 둔다 — 정작 큐가 가득 찼을 때는 할당할 여유가 없을 수 있다. */
		IOMMU_VEVENTQ_FLAG_LOST_EVENTS;

	fdno = iommufd_eventq_init(&veventq->common, "[iommufd-viommu-event]",	/* [한국어] 공통 부분을 세우고 익명 파일을 연다. */
				   ucmd->ictx, &iommufd_veventq_fops);
	if (fdno < 0) {	/* [한국어] 열지 못했다. */
		rc = fdno;	/* [한국어] 음수 코드를 옮긴다. */
		goto out_abort;	/* [한국어] 목록에 이미 넣었으므로 abort 로 되돌린다. */
	}

	cmd->out_veventq_id = veventq->common.obj.id;	/* [한국어] 사용자가 이 큐를 지목할 id. */
	cmd->out_veventq_fd = fdno;	/* [한국어] 이벤트를 읽을 디스크립터. */

	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 출력 값을 사용자에게 돌려 쓴다. */
	if (rc)	/* [한국어] 전하지 못했으면 사용자는 이 큐를 알지 못한다. */
		goto out_put_fdno;	/* [한국어] 번호를 반납하고 객체도 되돌린다. */

	iommufd_object_finalize(ucmd->ictx, &veventq->common.obj);	/* [한국어] 이제야 객체를 사용자에게 보이게 만든다. 이 뒤로는 되돌릴 수 없다. */
	fd_install(fdno, veventq->common.filep);	/* [한국어] 번호에 파일을 붙인다. */
	goto out_unlock_veventqs;	/* [한국어] 성공 경로도 같은 곳에서 락을 놓는다. */

out_put_fdno:	/* [한국어] 응답에 실패했을 때 합류하는 지점. */
	put_unused_fd(fdno);	/* [한국어] 설치하지 않은 번호를 반납한다. */
out_abort:	/* [한국어] 객체를 되돌려야 하는 실패 경로. */
	iommufd_object_abort_and_destroy(ucmd->ictx, &veventq->common.obj);	/* [한국어] 아직 공개하지 않은 객체를 되돌린다 — 이것이 abort 를 불러 목록에서 빼고 vIOMMU 참조를 놓는다. 쓰기 락 아래여야 하는 이유다. */
out_unlock_veventqs:	/* [한국어] 성공과 실패가 모두 지나는 마지막 정리 지점. */
	up_write(&viommu->veventqs_rwsem);	/* [한국어] vIOMMU 큐 목록의 쓰기 락을 놓는다. */
	iommufd_put_object(ucmd->ictx, &viommu->obj);	/* [한국어] 조회하며 들었던 vIOMMU 참조를 놓는다. 성공했다면 큐가 든 참조가 따로 남아 있다. */
	return rc;	/* [한국어] 0 이면 성공. 성공 경로에서는 rc 가 iommufd_ucmd_respond 의 0 이다. */
}
