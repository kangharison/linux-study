// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] IOAS 관련 ioctl 구현 (ioas.c)
 *
 * === 파일의 역할 ===
 * 사용자 공간이 IOVA 공간을 다루는 명령들의 구현이다. IOAS 를 만들고,
 * 쓸 수 있는 범위를 알려 주고, 메모리를 매핑하고 걷어내고, 두 IOAS
 * 사이에 매핑을 복사하고, 옵션을 읽고 쓴다.
 *
 * 실제 자료구조 조작은 io_pagetable.c 와 pages.c 가 한다. 이 파일은
 * 사용자 인자를 검증하고 객체를 붙잡았다 놓는 껍질에 가깝다 — 다만 그
 * 검증이 보안 경계라 꼼꼼하다.
 *
 * 두 함수가 특히 무겁다.
 *
 * iommufd_ioas_copy 는 한 IOAS 의 매핑을 다른 IOAS 로 옮긴다. 페이지를
 * 다시 고정하지 않고 iopt_pages 를 공유하므로, 같은 메모리를 두 번
 * 고정하거나 두 번 계상하지 않는다.
 *
 * iommufd_ioas_change_process 는 고정된 페이지의 계상을 다른 프로세스로
 * 옮긴다. VMM 이 fork 하거나 프로세스를 갈아탈 때 필요한데, 그러려면 이
 * 문맥의 모든 IOAS 락을 한꺼번에 쥐어야 한다 — 원 주석이 그 방식을
 * 스스로 "매우 추하다"고 인정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * main.c 의 명령 표 → [이 파일] → io_pagetable.c → pages.c → iommu 코어
 *
 * 실행 컨텍스트: 프로세스 문맥, 사용자 시스템 콜.
 *
 * === 타 모듈과의 연결 ===
 * 위: main.c 의 ioctl 분배, vfio_compat.c 가 옛 VFIO 명령을 옮겨 온다.
 * 아래: io_pagetable.h 의 iopt_* API.
 *
 * === 주요 함수/구조체 요약 ===
 * iommufd_ioas_alloc / destroy: IOVA 공간의 생성과 파괴.
 * iommufd_ioas_iova_ranges: 예약되지 않은 구간을 사용자에게 알린다.
 * iommufd_ioas_allow_iovas: 사용자가 쓸 범위를 원자적으로 갈아 끼운다.
 * iommufd_ioas_map / map_file: 사용자 메모리나 파일을 매핑한다.
 * iommufd_ioas_copy: 페이지를 다시 고정하지 않고 매핑만 복사한다.
 * iommufd_ioas_unmap: 매핑을 걷어낸다.
 * iommufd_ioas_change_process: 고정 페이지의 계상 주체를 옮긴다.
 * conv_iommu_prot: 사용자 플래그를 IOMMU 권한으로 옮긴다. IOMMU_CACHE 를
 *   늘 붙이는 이유가 원 주석에 있다.
 */
#include <linux/file.h>	/* [한국어] 파일 출처 매핑 */
#include <linux/interval_tree.h>	/* [한국어] 예약·허용 범위 트리 */
#include <linux/iommu.h>	/* [한국어] IOMMU 권한 상수 */
#include <linux/iommufd.h>	/* [한국어] 드라이버에 공개된 부분 */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 구조체 */

#include "io_pagetable.h"	/* [한국어] IOVA 공간 조작 API */

/*
 * [한국어]
 * iommufd_ioas_destroy - IOAS 를 허문다
 *
 * @obj: 파괴할 객체.
 *
 * 남은 매핑을 모두 걷어낸 뒤 표 자체를 해제한다. -ENOENT 는 이미 비어
 * 있었다는 뜻이라 정상이다.
 */
void iommufd_ioas_destroy(struct iommufd_object *obj)
{
	struct iommufd_ioas *ioas = container_of(obj, struct iommufd_ioas, obj);	/* [한국어] 구체 타입으로 */
	int rc;	/* [한국어] 결과 */

	rc = iopt_unmap_all(&ioas->iopt, NULL);	/* [한국어] 남은 매핑을 모두 걷어내고 */
	WARN_ON(rc && rc != -ENOENT);	/* [한국어] -ENOENT 는 이미 비어 있었다는 뜻이라 정상이다 */
	iopt_destroy_table(&ioas->iopt);	/* [한국어] 표 자체를 해제한다 */
	mutex_destroy(&ioas->mutex);	/* [한국어] 락도 */
}

/*
 * [한국어]
 * iommufd_ioas_alloc - 빈 IOVA 공간을 만든다
 *
 * @ictx: 문맥.
 * @return: 새 IOAS, 실패하면 ERR_PTR.
 *
 * 아직 확정하지 않은 상태로 돌려준다 — 호출자가 finalize 하거나
 * abort 해야 한다.
 */
struct iommufd_ioas *iommufd_ioas_alloc(struct iommufd_ctx *ictx)
{
	struct iommufd_ioas *ioas;	/* [한국어] 만들 IOAS */

	ioas = iommufd_object_alloc(ictx, ioas, IOMMUFD_OBJ_IOAS);	/* [한국어] id 를 예약하되 아직 공개하지 않는다 */
	if (IS_ERR(ioas))	/* [한국어] 실패면 */
		return ioas;	/* [한국어] 그대로 전한다 */

	iopt_init_table(&ioas->iopt);	/* [한국어] 빈 IOVA 공간 */
	INIT_LIST_HEAD(&ioas->hwpt_list);	/* [한국어] 아직 딸린 도메인이 없다 */
	mutex_init(&ioas->mutex);	/* [한국어] 그 목록을 지킬 락 */
	return ioas;	/* [한국어] 호출자가 확정하거나 되돌린다 */
}

/*
 * [한국어]
 * iommufd_ioas_alloc_ioctl - IOMMU_IOAS_ALLOC 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자에게 id 를 돌려준 뒤에야 확정한다. 순서가 반대면 사용자가 id 를
 * 받기 전에 그 객체를 다른 스레드가 볼 수 있게 된다.
 *
 * 확정을 읽기 락 아래에서 하는 이유: change_process 가 쓰기 락으로 모든
 * IOAS 를 붙잡는데, 그 사이에 새 IOAS 가 끼어들면 안 된다.
 */
int iommufd_ioas_alloc_ioctl(struct iommufd_ucmd *ucmd)
{
	struct iommu_ioas_alloc *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_ioas *ioas;	/* [한국어] 만들 IOAS */
	int rc;	/* [한국어] 결과 */

	if (cmd->flags)	/* [한국어] 아직 정의된 플래그가 없다 */
		return -EOPNOTSUPP;	/* [한국어] 새 커널이 쓸 자리라 지금은 거절한다 */

	ioas = iommufd_ioas_alloc(ucmd->ictx);	/* [한국어] 객체를 만들고 */
	if (IS_ERR(ioas))	/* [한국어] 실패면 */
		return PTR_ERR(ioas);	/* [한국어] 그대로 */

	cmd->out_ioas_id = ioas->obj.id;	/* [한국어] 사용자가 이후 이 id 로 부른다 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 먼저 돌려주고 */
	if (rc)	/* [한국어] 복사에 실패하면 */
		goto out_table;	/* [한국어] 만든 것을 되돌린다 */

	down_read(&ucmd->ictx->ioas_creation_lock);	/* [한국어] change_process 가 모든 IOAS 를 붙잡는 사이에 */
	iommufd_object_finalize(ucmd->ictx, &ioas->obj);	/* [한국어] 새 것이 끼어들면 안 된다 */
	up_read(&ucmd->ictx->ioas_creation_lock);	/* [한국어] 공개 완료 */
	return 0;	/* [한국어] 성공 */

out_table:	/* [한국어] 응답에 실패했을 때 — 만든 IOAS 를 되돌린다 */
	iommufd_object_abort_and_destroy(ucmd->ictx, &ioas->obj);	/* [한국어] 표까지 만든 뒤라 타입별 정리가 필요하다 */
	return rc;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * iommufd_ioas_iova_ranges - 쓸 수 있는 IOVA 구간을 알려 준다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, -EMSGSIZE 면 버퍼가 모자랐다.
 *
 * 예약 트리의 "구멍"이 곧 쓸 수 있는 구간이다. 그래서 예약된 곳이 아니라
 * 그 사이를 돌려준다.
 *
 * 버퍼가 모자라도 개수는 끝까지 센다 — 사용자가 그 값을 보고 버퍼를
 * 키워 다시 부를 수 있게 하려는 것이다. 그래서 -EMSGSIZE 를 응답 뒤에
 * 돌려준다.
 *
 * 정렬도 함께 알려 준다. 사용자가 매핑을 요청할 때 그 정렬을 지켜야 한다.
 */
int iommufd_ioas_iova_ranges(struct iommufd_ucmd *ucmd)
{
	struct iommu_iova_range __user *ranges;	/* [한국어] 결과를 담을 사용자 배열 */
	struct iommu_ioas_iova_ranges *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 IOAS */
	struct interval_tree_span_iter span;	/* [한국어] 예약 트리의 구멍을 훑는다 */
	u32 max_iovas;	/* [한국어] 사용자 버퍼가 담을 수 있는 개수 */
	int rc;	/* [한국어] 결과 */

	if (cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	ioas = iommufd_get_ioas(ucmd->ictx, cmd->ioas_id);	/* [한국어] 대상을 붙잡고 */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return PTR_ERR(ioas);	/* [한국어] 그대로 */

	down_read(&ioas->iopt.iova_rwsem);	/* [한국어] 트리를 훑는 동안 바뀌면 안 된다 */
	max_iovas = cmd->num_iovas;	/* [한국어] 사용자가 준 버퍼 크기 */
	ranges = u64_to_user_ptr(cmd->allowed_iovas);	/* [한국어] 그 버퍼 */
	cmd->num_iovas = 0;	/* [한국어] 실제로 센 개수를 여기 담는다 */
	cmd->out_iova_alignment = ioas->iopt.iova_alignment;	/* [한국어] 매핑 요청이 지켜야 할 정렬 */
	interval_tree_for_each_span(&span, &ioas->iopt.reserved_itree, 0,	/* [한국어] 예약 트리를 훑되 */
				    ULONG_MAX) {	/* [한국어] 전 범위에 대해 */
		if (!span.is_hole)	/* [한국어] 예약된 구간은 */
			continue;	/* [한국어] 쓸 수 없으므로 건너뛴다 */
		if (cmd->num_iovas < max_iovas) {	/* [한국어] 버퍼에 자리가 있으면 */
			struct iommu_iova_range elm = {	/* [한국어] 한 항목을 만들어 */
				.start = span.start_hole,	/* [한국어] 구멍의 시작 */
				.last = span.last_hole,	/* [한국어] 구멍의 끝 */
			};

			if (copy_to_user(&ranges[cmd->num_iovas], &elm,	/* [한국어] 사용자 버퍼로 */
					 sizeof(elm))) {	/* [한국어] 복사한다 */
				rc = -EFAULT;	/* [한국어] 버퍼가 잘못됐으면 */
				goto out_put;	/* [한국어] 거절 */
			}
		}
		cmd->num_iovas++;	/* [한국어] 버퍼가 모자라도 개수는 끝까지 센다 */
	}
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 센 개수와 정렬을 돌려준다 */
	if (rc)	/* [한국어] 복사 실패면 */
		goto out_put;	/* [한국어] 거절 */
	if (cmd->num_iovas > max_iovas)	/* [한국어] 버퍼가 모자랐으면 */
		rc = -EMSGSIZE;	/* [한국어] 사용자가 개수를 보고 다시 부를 수 있다 */
out_put:	/* [한국어] 객체를 놓고 나가는 공통 경로 */
	up_read(&ioas->iopt.iova_rwsem);	/* [한국어] 트리 보호 해제 */
	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_ioas_load_iovas - 사용자가 준 허용 범위를 새 트리에 담는다
 *
 * @itree: 채울 트리(비어 있어야 한다).
 * @ranges: 사용자 버퍼.
 * @num: 항목 수.
 * @return: 0 성공, 음수면 실패.
 *
 * 겹치는 범위를 거절하는 것이 중요하다 — 구간 트리는 겹침을 허용하지만,
 * 그러면 "어디가 허용인가"의 답이 흐려진다.
 *
 * 새 트리에 담는 이유는 원자성이다. 아래 호출자가 그 이유를 설명한다.
 */
static int iommufd_ioas_load_iovas(struct rb_root_cached *itree,
				   struct iommu_iova_range __user *ranges,
				   u32 num)
{
	u32 i;	/* [한국어] 항목 인덱스 */

	for (i = 0; i != num; i++) {	/* [한국어] 사용자가 준 범위들을 */
		struct iommu_iova_range range;	/* [한국어] 하나씩 */
		struct iopt_allowed *allowed;	/* [한국어] 트리에 넣을 노드 */

		if (copy_from_user(&range, ranges + i, sizeof(range)))	/* [한국어] 사용자 버퍼에서 읽고 */
			return -EFAULT;	/* [한국어] 잘못된 포인터면 거절 */

		if (range.start >= range.last)	/* [한국어] 뒤집혔거나 빈 범위면 */
			return -EINVAL;	/* [한국어] 뜻이 없다 */

		if (interval_tree_iter_first(itree, range.start, range.last))	/* [한국어] 앞의 것과 겹치면 */
			return -EINVAL;	/* [한국어] "어디가 허용인가"의 답이 흐려진다 */

		allowed = kzalloc_obj(*allowed, GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자가 시킨 할당이라 계상한다 */
		if (!allowed)	/* [한국어] 메모리 부족 */
			return -ENOMEM;	/* [한국어] 호출자가 지금까지 만든 것을 버린다 */
		allowed->node.start = range.start;	/* [한국어] 범위의 시작 */
		allowed->node.last = range.last;	/* [한국어] 끝(포함) */

		interval_tree_insert(&allowed->node, itree);	/* [한국어] 새 트리에 담는다 */
	}
	return 0;	/* [한국어] 모두 담았다 */
}

/*
 * [한국어]
 * iommufd_ioas_allow_iovas - 쓸 IOVA 범위를 갈아 끼운다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 방식을 설명한다 — 갱신이 원자적이어야 하므로, 새 트리를
 * 따로 만들어 두었다가 통째로 바꿔 끼운다. 성공하면 옛 트리를, 실패하면
 * 새 트리를 버린다.
 *
 * 중간에 실패해 반쯤 바뀐 상태가 남으면 사용자가 쓸 수 없는 범위를
 * 쓸 수 있다고 믿게 되므로, 그 원자성이 필요하다.
 */
int iommufd_ioas_allow_iovas(struct iommufd_ucmd *ucmd)
{
	struct iommu_ioas_allow_iovas *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct rb_root_cached allowed_iova = RB_ROOT_CACHED;	/* [한국어] 새로 만들 트리 */
	struct interval_tree_node *node;	/* [한국어] 정리용 순회 */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 IOAS */
	struct io_pagetable *iopt;	/* [한국어] 그 IOVA 공간 */
	int rc = 0;	/* [한국어] 결과 */

	if (cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	ioas = iommufd_get_ioas(ucmd->ictx, cmd->ioas_id);	/* [한국어] 대상을 붙잡고 */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return PTR_ERR(ioas);	/* [한국어] 그대로 */
	iopt = &ioas->iopt;	/* [한국어] 그 IOVA 공간 */

	rc = iommufd_ioas_load_iovas(&allowed_iova,	/* [한국어] 새 트리를 먼저 만들고 */
				     u64_to_user_ptr(cmd->allowed_iovas),	/* [한국어] 사용자 버퍼에서 */
				     cmd->num_iovas);	/* [한국어] 그 개수만큼 */
	if (rc)	/* [한국어] 만들다 실패했으면 */
		goto out_free;	/* [한국어] 만든 것만 버린다 */

	/*
	 * We want the allowed tree update to be atomic, so we have to keep the
	 * original nodes around, and keep track of the new nodes as we allocate
	 * memory for them. The simplest solution is to have a new/old tree and
	 * then swap new for old. On success we free the old tree, on failure we
	 * free the new tree.
	 */
	rc = iopt_set_allow_iova(iopt, &allowed_iova);	/* [한국어] (원 주석: 갱신이 원자적이어야 해 새 트리와 옛 트리를 통째로 바꿔 끼운다) */
out_free:	/* [한국어] 새 트리든 옛 트리든 남은 쪽을 버린다 */
	while ((node = interval_tree_iter_first(&allowed_iova, 0, ULONG_MAX))) {	/* [한국어] 성공했으면 옛 트리가, 실패했으면 새 트리가 여기 남는다 */
		interval_tree_remove(node, &allowed_iova);	/* [한국어] 하나씩 빼서 */
		kfree(container_of(node, struct iopt_allowed, node));	/* [한국어] 버린다 */
	}
	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * conv_iommu_prot - 사용자 플래그를 IOMMU 권한으로 옮긴다
 *
 * @map_flags: 사용자가 준 매핑 플래그.
 * @return: IOMMU_* 권한 조합.
 *
 * IOMMU_CACHE 를 늘 붙이는 근거를 원 주석이 든다 — 사용자 공간에 캐시를
 * 직접 비우는 수단을 주지 않고, 대부분의 아키텍처에서 그 명령은 특권이다.
 * 그러니 하드웨어가 CPU 와 일관되게 동작해야 하고, 그 능력은 장치를 묶을
 * 때 확인한다.
 */
static int conv_iommu_prot(u32 map_flags)
{
	/*
	 * We provide no manual cache coherency ioctls to userspace and most
	 * architectures make the CPU ops for cache flushing privileged.
	 * Therefore we require the underlying IOMMU to support CPU coherent
	 * operation. Support for IOMMU_CACHE is enforced by the
	 * IOMMU_CAP_CACHE_COHERENCY test during bind.
	 */
	int iommu_prot = IOMMU_CACHE;	/* [한국어] (원 주석: 사용자에게 캐시 비우기 수단을 주지 않고 그 명령은 대개 특권이라, 하드웨어가 CPU 와 일관되어야 한다) */

	if (map_flags & IOMMU_IOAS_MAP_WRITEABLE)	/* [한국어] 쓰기 허용을 요청했으면 */
		iommu_prot |= IOMMU_WRITE;	/* [한국어] 그 비트를 */
	if (map_flags & IOMMU_IOAS_MAP_READABLE)	/* [한국어] 읽기 허용을 요청했으면 */
		iommu_prot |= IOMMU_READ;	/* [한국어] 그 비트를 */
	return iommu_prot;	/* [한국어] 매핑에 그대로 쓰인다 */
}

/*
 * [한국어]
 * iommufd_ioas_map_file - 파일의 일부를 IOVA 에 매핑한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * memfd 나 hugetlbfs 처럼 파일로 표현된 메모리를 붙인다. 사용자 VA 를
 * 거치지 않으므로 그 프로세스가 죽어도 매핑이 살아 있을 수 있고,
 * change_process 도 이 종류에만 허용된다.
 *
 * 권한이 하나도 없으면 거절한다 — 읽지도 쓰지도 못하는 매핑은 뜻이 없다.
 */
int iommufd_ioas_map_file(struct iommufd_ucmd *ucmd)
{
	struct iommu_ioas_map_file *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	unsigned long iova = cmd->iova;	/* [한국어] 매핑할 IOVA(또는 커널이 고른 값을 담을 자리) */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 IOAS */
	unsigned int flags = 0;	/* [한국어] IOVA 를 커널이 고를 것인가 */
	int rc;	/* [한국어] 결과 */

	if (cmd->flags &	/* [한국어] 모르는 플래그가 있으면 */
	     ~(IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_WRITEABLE |	/* [한국어] 정의된 셋 말고 */
	       IOMMU_IOAS_MAP_READABLE))	/* [한국어] 다른 것이 서 있으면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 — 새 커널이 쓸 자리다 */

	if (cmd->iova >= ULONG_MAX || cmd->length >= ULONG_MAX)	/* [한국어] 32비트 커널에서 잘릴 값이면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	if (!(cmd->flags &	/* [한국어] 권한이 */
	      (IOMMU_IOAS_MAP_WRITEABLE | IOMMU_IOAS_MAP_READABLE)))	/* [한국어] 하나도 없으면 */
		return -EINVAL;	/* [한국어] 읽지도 쓰지도 못하는 매핑은 뜻이 없다 */

	ioas = iommufd_get_ioas(ucmd->ictx, cmd->ioas_id);	/* [한국어] 대상을 붙잡고 */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return PTR_ERR(ioas);	/* [한국어] 그대로 */

	if (!(cmd->flags & IOMMU_IOAS_MAP_FIXED_IOVA))	/* [한국어] IOVA 를 지정하지 않았으면 */
		flags = IOPT_ALLOC_IOVA;	/* [한국어] 커널이 골라 준다 */

	rc = iopt_map_file_pages(ucmd->ictx, &ioas->iopt, &iova, cmd->fd,	/* [한국어] 파일의 일부를 */
				 cmd->start, cmd->length,	/* [한국어] 그 오프셋부터 그 길이만큼 */
				 conv_iommu_prot(cmd->flags), flags);	/* [한국어] 권한을 옮겨 매핑한다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_put;	/* [한국어] 객체만 놓고 나간다 */

	cmd->iova = iova;	/* [한국어] 커널이 골랐으면 그 값을 알려 준다 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 사용자에게 돌려준다 */
out_put:	/* [한국어] 객체를 놓고 나가는 공통 경로 */
	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_ioas_map - 사용자 VA 를 IOVA 에 매핑한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 가장 흔한 매핑 경로다. FIXED_IOVA 가 없으면 커널이 IOVA 를 골라
 * 사용자에게 돌려준다.
 *
 * ULONG_MAX 검사가 필요한 이유: 사용자는 64비트로 주지만 커널의 IOVA 는
 * unsigned long 이라, 32비트 커널에서 잘릴 수 있다.
 */
int iommufd_ioas_map(struct iommufd_ucmd *ucmd)
{
	struct iommu_ioas_map *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	unsigned long iova = cmd->iova;	/* [한국어] 매핑할 IOVA */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 IOAS */
	unsigned int flags = 0;	/* [한국어] IOVA 를 커널이 고를 것인가 */
	int rc;	/* [한국어] 결과 */

	if ((cmd->flags &	/* [한국어] 모르는 플래그나 */
	     ~(IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_WRITEABLE |	/* [한국어] 정의된 셋 밖의 비트 */
	       IOMMU_IOAS_MAP_READABLE)) ||	/* [한국어] 또는 */
	    cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */
	if (cmd->iova >= ULONG_MAX || cmd->length >= ULONG_MAX)	/* [한국어] 32비트에서 잘릴 값이면 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	if (!(cmd->flags &	/* [한국어] 권한이 */
	      (IOMMU_IOAS_MAP_WRITEABLE | IOMMU_IOAS_MAP_READABLE)))	/* [한국어] 하나도 없으면 */
		return -EINVAL;	/* [한국어] 뜻이 없는 매핑이다 */

	ioas = iommufd_get_ioas(ucmd->ictx, cmd->ioas_id);	/* [한국어] 대상을 붙잡고 */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return PTR_ERR(ioas);	/* [한국어] 그대로 */

	if (!(cmd->flags & IOMMU_IOAS_MAP_FIXED_IOVA))	/* [한국어] IOVA 를 지정하지 않았으면 */
		flags = IOPT_ALLOC_IOVA;	/* [한국어] 커널이 골라 준다 */
	rc = iopt_map_user_pages(ucmd->ictx, &ioas->iopt, &iova,	/* [한국어] 사용자 VA 를 */
				 u64_to_user_ptr(cmd->user_va), cmd->length,	/* [한국어] 그 길이만큼 */
				 conv_iommu_prot(cmd->flags), flags);	/* [한국어] 권한을 옮겨 매핑한다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_put;	/* [한국어] 객체만 놓고 나간다 */

	cmd->iova = iova;	/* [한국어] 커널이 골랐으면 그 값을 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 사용자에게 돌려준다 */
out_put:	/* [한국어] 객체를 놓고 나가는 공통 경로 */
	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_ioas_copy - 한 IOAS 의 매핑을 다른 IOAS 로 복사한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 페이지를 다시 고정하지 않는 것이 이 명령의 존재 이유다. 원본에서
 * iopt_pages 참조들을 꺼내 그대로 대상에 붙이므로, 같은 메모리를 두 번
 * 고정하거나 두 번 계상하지 않는다.
 *
 * VMM 이 장치마다 다른 IOVA 배치를 쓰면서도 같은 게스트 메모리를 공유할
 * 때 이 명령이 쓰인다.
 *
 * 원본 객체를 일찍 놓는 것이 눈에 띈다 — 페이지 참조를 이미 잡았으므로
 * IOAS 자체는 더 붙잡고 있을 필요가 없고, 두 객체를 동시에 잡으면 같은
 * IOAS 를 원본과 대상으로 준 경우 교착이 난다.
 */
int iommufd_ioas_copy(struct iommufd_ucmd *ucmd)
{
	struct iommu_ioas_copy *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_ioas *src_ioas;	/* [한국어] 원본 IOAS */
	struct iommufd_ioas *dst_ioas;	/* [한국어] 대상 IOAS */
	unsigned int flags = 0;	/* [한국어] IOVA 를 커널이 고를 것인가 */
	LIST_HEAD(pages_list);	/* [한국어] 옮길 페이지 묶음들 */
	unsigned long iova;	/* [한국어] 대상에서의 IOVA */
	int rc;	/* [한국어] 결과 */

	iommufd_test_syz_conv_iova_id(ucmd, cmd->src_ioas_id, &cmd->src_iova,	/* [한국어] 퍼저가 낸 값을 유효한 범위로 바꾼다(시험 빌드에서만) */
				      &cmd->flags);	/* [한국어] 플래그도 함께 */

	if ((cmd->flags &	/* [한국어] 모르는 플래그가 있으면 */
	     ~(IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_WRITEABLE |	/* [한국어] 정의된 셋 밖의 */
	       IOMMU_IOAS_MAP_READABLE)))	/* [한국어] 비트가 서 있으면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */
	if (cmd->length >= ULONG_MAX || cmd->src_iova >= ULONG_MAX ||	/* [한국어] 32비트에서 잘릴 값이면 */
	    cmd->dst_iova >= ULONG_MAX)	/* [한국어] 어느 쪽이든 */
		return -EOVERFLOW;	/* [한국어] 거절 */

	if (!(cmd->flags &	/* [한국어] 권한이 */
	      (IOMMU_IOAS_MAP_WRITEABLE | IOMMU_IOAS_MAP_READABLE)))	/* [한국어] 하나도 없으면 */
		return -EINVAL;	/* [한국어] 뜻이 없는 매핑이다 */

	src_ioas = iommufd_get_ioas(ucmd->ictx, cmd->src_ioas_id);	/* [한국어] 원본을 붙잡고 */
	if (IS_ERR(src_ioas))	/* [한국어] 없으면 */
		return PTR_ERR(src_ioas);	/* [한국어] 그대로 */
	rc = iopt_get_pages(&src_ioas->iopt, cmd->src_iova, cmd->length,	/* [한국어] 그 범위의 페이지 묶음 참조를 꺼낸다 */
			    &pages_list);	/* [한국어] 다시 고정하지 않는다 */
	iommufd_put_object(ucmd->ictx, &src_ioas->obj);	/* [한국어] 참조를 잡았으니 IOAS 는 놓는다 — 같은 IOAS 를 양쪽에 준 경우 교착을 막는다 */
	if (rc)	/* [한국어] 꺼내지 못했으면 */
		return rc;	/* [한국어] 거절 */

	dst_ioas = iommufd_get_ioas(ucmd->ictx, cmd->dst_ioas_id);	/* [한국어] 대상을 붙잡고 */
	if (IS_ERR(dst_ioas)) {	/* [한국어] 없으면 */
		rc = PTR_ERR(dst_ioas);	/* [한국어] 오류를 전하되 */
		goto out_pages;	/* [한국어] 꺼내 둔 페이지 참조를 놓아야 한다 */
	}

	if (!(cmd->flags & IOMMU_IOAS_MAP_FIXED_IOVA))	/* [한국어] IOVA 를 지정하지 않았으면 */
		flags = IOPT_ALLOC_IOVA;	/* [한국어] 커널이 골라 준다 */
	iova = cmd->dst_iova;	/* [한국어] 사용자가 지정한 자리 */
	rc = iopt_map_pages(&dst_ioas->iopt, &pages_list, cmd->length, &iova,	/* [한국어] 이미 고정된 페이지를 그대로 붙인다 */
			    conv_iommu_prot(cmd->flags), flags);	/* [한국어] 권한은 새로 정한다 */
	if (rc)	/* [한국어] 실패면 */
		goto out_put_dst;	/* [한국어] 되돌린다 */

	cmd->dst_iova = iova;	/* [한국어] 커널이 골랐으면 그 값을 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 사용자에게 돌려준다 */
out_put_dst:	/* [한국어] 대상 객체를 놓는다 */
	iommufd_put_object(ucmd->ictx, &dst_ioas->obj);	/* [한국어] 대상 객체를 놓고 */
out_pages:	/* [한국어] 꺼내 둔 페이지 참조를 놓는다 */
	iopt_free_pages_list(&pages_list);	/* [한국어] 남은 페이지 참조도 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_ioas_unmap - 매핑을 걷어낸다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, -ENOENT 면 그 범위에 매핑이 없었다.
 *
 * 전 범위를 뜻하는 특별한 인자(iova 0, 길이 U64_MAX)를 따로 다룬다 —
 * 그 경우는 표를 통째로 비우는 빠른 경로가 있다.
 *
 * 걷어낸 길이를 돌려주는 이유: 큰 페이지가 통째로 걷혀 요청보다 클 수
 * 있고, 사용자가 그 값으로 다음 위치를 정한다.
 */
int iommufd_ioas_unmap(struct iommufd_ucmd *ucmd)
{
	struct iommu_ioas_unmap *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 IOAS */
	unsigned long unmapped = 0;	/* [한국어] 실제로 걷어낸 바이트 */
	int rc;	/* [한국어] 결과 */

	ioas = iommufd_get_ioas(ucmd->ictx, cmd->ioas_id);	/* [한국어] 대상을 붙잡고 */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return PTR_ERR(ioas);	/* [한국어] 그대로 */

	if (cmd->iova == 0 && cmd->length == U64_MAX) {	/* [한국어] 전 범위를 뜻하는 특별한 인자면 */
		rc = iopt_unmap_all(&ioas->iopt, &unmapped);	/* [한국어] 표를 통째로 비우는 빠른 경로 */
		if (rc)	/* [한국어] 실패면 */
			goto out_put;	/* [한국어] 객체만 놓고 나간다 */
	} else {
		if (cmd->iova >= ULONG_MAX || cmd->length >= ULONG_MAX) {	/* [한국어] 32비트에서 잘릴 값이면 */
			rc = -EOVERFLOW;	/* [한국어] 거절 */
			goto out_put;	/* [한국어] 나간다 */
		}
		rc = iopt_unmap_iova(&ioas->iopt, cmd->iova, cmd->length,	/* [한국어] 그 범위만 */
				     &unmapped);	/* [한국어] 걷어낸다 */
		if (rc)	/* [한국어] 실패면 */
			goto out_put;	/* [한국어] 나간다 */
		if (!unmapped) {	/* [한국어] 아무것도 걷어내지 못했으면 */
			rc = -ENOENT;	/* [한국어] 그 범위에 매핑이 없었다 */
			goto out_put;	/* [한국어] 나간다 */
		}
	}

	cmd->length = unmapped;	/* [한국어] 큰 페이지 때문에 요청보다 클 수 있다 */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 사용자가 그 값으로 다음 위치를 정한다 */

out_put:	/* [한국어] 객체를 놓고 나가는 공통 경로 */
	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_release_all_iova_rwsem - 붙잡아 둔 모든 IOAS 락을 놓는다
 *
 * @ictx: 문맥.
 * @ioas_list: 붙잡은 IOAS 목록.
 *
 * take_all 의 짝이다. 락과 객체 참조를 함께 놓고 목록을 버린다.
 */
static void iommufd_release_all_iova_rwsem(struct iommufd_ctx *ictx,
					   struct xarray *ioas_list)
{
	struct iommufd_ioas *ioas;	/* [한국어] 순회용 */
	unsigned long index;	/* [한국어] xarray 인덱스 */

	xa_for_each(ioas_list, index, ioas) {	/* [한국어] 붙잡아 둔 것들을 */
		up_write(&ioas->iopt.iova_rwsem);	/* [한국어] 락을 놓고 */
		refcount_dec(&ioas->obj.users);	/* [한국어] 객체 참조도 놓는다 */
	}
	up_write(&ictx->ioas_creation_lock);	/* [한국어] 새 IOAS 가 생길 수 있게 한다 */
	xa_destroy(ioas_list);	/* [한국어] 임시 목록을 버린다 */
}

/*
 * [한국어]
 * iommufd_take_all_iova_rwsem - 이 문맥의 모든 IOAS 락을 쥔다
 *
 * @ictx: 문맥.
 * @ioas_list: 붙잡은 것들을 담을 목록.
 * @return: 0 성공, 음수면 실패(그 경우 모두 놓고 돌아간다).
 *
 * 원 주석이 스스로 "매우 추하다"고 인정하는 방식이다. 그 근거는 이렇다 —
 * pages->source_mm 에 락을 하나 더 두면 mdev 의 성능 경로가 느려지므로,
 * 대신 모든 IOVA 락의 쓰기 쪽을 잡아 그 필드까지 함께 보호한다. 복사
 * 때문에 어느 IOAS 가 그 pages 를 읽을지 알 수 없어 전부 잠근다.
 *
 * 이곳이 이 계층에서 락을 겹쳐 잡는 유일한 자리이고, 그래서 id 순서로
 * 일정하게 잡아 교착을 피한다.
 *
 * ioas_creation_lock 이 두 가지를 막는다 — 그사이 새 IOAS 가 끼어드는
 * 것과, 둘 이상의 스레드가 동시에 이 겹친 잠금을 하는 것이다.
 */
static int iommufd_take_all_iova_rwsem(struct iommufd_ctx *ictx,
				       struct xarray *ioas_list)
{
	struct iommufd_object *obj;	/* [한국어] 순회 중인 객체 */
	unsigned long index;	/* [한국어] xarray 인덱스 — 곧 id 다 */
	int rc;	/* [한국어] 결과 */

	/*
	 * This is very ugly, it is done instead of adding a lock around
	 * pages->source_mm, which is a performance path for mdev, we just
	 * obtain the write side of all the iova_rwsems which also protects the
	 * pages->source_*. Due to copies we can't know which IOAS could read
	 * from the pages, so we just lock everything. This is the only place
	 * locks are nested and they are uniformly taken in ID order.
	 *
	 * ioas_creation_lock prevents new IOAS from being installed in the
	 * xarray while we do this, and also prevents more than one thread from
	 * holding nested locks.
	 */
	down_write(&ictx->ioas_creation_lock);	/* [한국어] (원 주석: 새 IOAS 가 끼어드는 것과 둘 이상이 동시에 겹친 잠금을 하는 것을 막는다) */
	xa_lock(&ictx->objects);	/* [한국어] 목록을 훑는 동안 */
	xa_for_each(&ictx->objects, index, obj) {	/* [한국어] 모든 객체를 id 순으로 — 그 순서가 교착을 막는다 */
		struct iommufd_ioas *ioas;	/* [한국어] IOAS 로 되짚을 자리 */

		if (!obj || obj->type != IOMMUFD_OBJ_IOAS)	/* [한국어] IOAS 가 아니면 */
			continue;	/* [한국어] 건너뛴다 */

		if (!refcount_inc_not_zero(&obj->users))	/* [한국어] 파괴 중이면 */
			continue;	/* [한국어] 건너뛴다 */

		xa_unlock(&ictx->objects);	/* [한국어] 락을 잡는 동안 잠들 수 있어 놓는다 */

		ioas = container_of(obj, struct iommufd_ioas, obj);	/* [한국어] 구체 타입으로 */
		down_write_nest_lock(&ioas->iopt.iova_rwsem,	/* [한국어] 겹쳐 잡는 유일한 자리라 */
				     &ictx->ioas_creation_lock);	/* [한국어] lockdep 에 그 사정을 알린다 */

		rc = xa_err(xa_store(ioas_list, index, ioas, GFP_KERNEL));	/* [한국어] 붙잡은 것을 기록해 두고 */
		if (rc) {	/* [한국어] 실패면 */
			iommufd_release_all_iova_rwsem(ictx, ioas_list);	/* [한국어] 지금까지 잡은 것을 모두 놓는다 */
			return rc;	/* [한국어] 호출자에게 */
		}

		xa_lock(&ictx->objects);	/* [한국어] 다시 잡고 순회를 이어 간다 */
	}
	xa_unlock(&ictx->objects);	/* [한국어] 모두 붙잡았다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * need_charge_update - 이 페이지 묶음의 계상을 옮겨야 하는지 판단한다
 *
 * @pages: 볼 묶음.
 * @return: 옮겨야 하면 참.
 *
 * 계상 방식에 따라 견줄 대상이 다르다. 계상하지 않는 묶음은 옮길 것이
 * 없고, 프로세스 통계 방식은 mm 만 보면 된다.
 *
 * 사용자 한도 방식이 둘 다 보는 이유를 원 주석이 밝힌다 — 그 방식도
 * mm->pinned_vm 에 함께 계상하므로 mm 이 바뀌어도 갱신이 필요하다.
 */
static bool need_charge_update(struct iopt_pages *pages)
{
	switch (pages->account_mode) {	/* [한국어] 계상 방식에 따라 */
	case IOPT_PAGES_ACCOUNT_NONE:	/* [한국어] 계상하지 않는 묶음은 */
		return false;	/* [한국어] 옮길 것이 없다 */
	case IOPT_PAGES_ACCOUNT_MM:	/* [한국어] 프로세스 통계 방식이면 */
		return pages->source_mm != current->mm;	/* [한국어] mm 만 견주면 된다 */
	case IOPT_PAGES_ACCOUNT_USER:	/* [한국어] 사용자 한도 방식이면 */
		/*
		 * Update when mm changes because it also accounts
		 * in mm->pinned_vm.
		 */
		return (pages->source_user != current_user()) ||	/* [한국어] (원 주석: 그 방식도 mm->pinned_vm 에 함께 계상하므로 mm 이 바뀌어도 갱신이 필요하다) */
		       (pages->source_mm != current->mm);	/* [한국어] 둘 중 하나라도 다르면 */
	}
	return true;	/* [한국어] 모르는 방식이면 안전하게 갱신한다 */
}

/*
 * [한국어]
 * charge_current - 현재 프로세스에 그만큼을 새로 계상한다
 *
 * @npinned: 계상 방식별 페이지 수.
 * @return: 0 성공, 음수면 한도를 넘었다.
 *
 * 임시 구조체에 현재 프로세스 정보를 담아 계상 함수를 부른다 — 그
 * 함수가 묶음 단위로 동작하도록 되어 있어, 여기서는 가짜 묶음을 만들어
 * 쓴다.
 *
 * 중간에 실패하면 이미 계상한 것들을 역순으로 되돌린다. 새 프로세스의
 * 한도를 넘으면 옮기지 못하고 원래대로 두어야 하기 때문이다.
 */
static int charge_current(unsigned long *npinned)
{
	struct iopt_pages tmp = {	/* [한국어] 계상 함수가 묶음 단위로 동작해 */
		.source_mm = current->mm,	/* [한국어] 현재 프로세스 정보를 담은 */
		.source_task = current->group_leader,	/* [한국어] 가짜 묶음을 */
		.source_user = current_user(),	/* [한국어] 만들어 쓴다 */
	};
	unsigned int account_mode;	/* [한국어] 계상 방식 */
	int rc;	/* [한국어] 결과 */

	for (account_mode = 0; account_mode != IOPT_PAGES_ACCOUNT_MODE_NUM;	/* [한국어] 방식마다 */
	     account_mode++) {	/* [한국어] 한 번씩 */
		if (!npinned[account_mode])	/* [한국어] 그 방식으로 셀 것이 없으면 */
			continue;	/* [한국어] 건너뛴다 */

		tmp.account_mode = account_mode;	/* [한국어] 이번 방식으로 */
		rc = iopt_pages_update_pinned(&tmp, npinned[account_mode], true,	/* [한국어] 새 프로세스에 계상한다 */
					      NULL);	/* [한국어] 기존 사용자 정보는 없다 */
		if (rc)	/* [한국어] 한도를 넘었으면 */
			goto err_undo;	/* [한국어] 이미 계상한 것을 되돌린다 */
	}
	return 0;	/* [한국어] 모두 계상했다 */

err_undo:	/* [한국어] 실패한 지점 앞까지 되돌린다 */
	while (account_mode != 0) {	/* [한국어] 실패한 지점 앞까지 */
		account_mode--;	/* [한국어] 역순으로 */
		if (!npinned[account_mode])	/* [한국어] 계상하지 않은 방식은 */
			continue;	/* [한국어] 건너뛴다 */
		tmp.account_mode = account_mode;	/* [한국어] 그 방식으로 */
		iopt_pages_update_pinned(&tmp, npinned[account_mode], false,	/* [한국어] 계상을 되돌린다 */
					 NULL);	/* [한국어] 아무것도 바꾸지 않은 상태로 돌아간다 */
	}
	return rc;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * change_mm - 페이지 묶음의 소유 프로세스 정보를 바꾼다
 *
 * @pages: 바꿀 묶음.
 *
 * 세 참조를 모두 갈아 끼운다. 새 것을 먼저 잡고 옛 것을 놓는 순서가
 * 중요하다 — 반대로 하면 둘이 같은 경우 그 사이에 참조가 0 이 된다.
 */
static void change_mm(struct iopt_pages *pages)
{
	struct task_struct *old_task = pages->source_task;	/* [한국어] 옛 태스크 */
	struct user_struct *old_user = pages->source_user;	/* [한국어] 옛 사용자 */
	struct mm_struct *old_mm = pages->source_mm;	/* [한국어] 옛 주소 공간 */

	pages->source_mm = current->mm;	/* [한국어] 새 것을 먼저 잡고 */
	mmgrab(pages->source_mm);	/* [한국어] 참조를 올린 뒤 */
	mmdrop(old_mm);	/* [한국어] 옛 것을 놓는다 — 둘이 같아도 0 이 되지 않는다 */

	pages->source_task = current->group_leader;	/* [한국어] 태스크도 같은 순서로 */
	get_task_struct(pages->source_task);	/* [한국어] 새 것을 잡고 */
	put_task_struct(old_task);	/* [한국어] 옛 것을 놓는다 */

	pages->source_user = get_uid(current_user());	/* [한국어] 사용자도 */
	free_uid(old_user);	/* [한국어] 같은 순서로 */
}

/*
 * [한국어] 붙잡아 둔 모든 IOAS 의 모든 구간을 도는 반복문.
 * change_process 가 세 번 도는데, 그 순회를 매번 쓰지 않으려고 매크로로
 * 묶었다.
 */
#define for_each_ioas_area(_xa, _index, _ioas, _area) \
	xa_for_each((_xa), (_index), (_ioas)) \
		for (_area = iopt_area_iter_first(&_ioas->iopt, 0, ULONG_MAX); \
		     _area; \
		     _area = iopt_area_iter_next(_area, 0, ULONG_MAX))	/* [한국어] 그 IOAS 의 다음 구간으로 */

/*
 * [한국어]
 * iommufd_ioas_change_process - 고정 페이지의 계상 주체를 옮긴다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, -EINVAL 옮길 수 없는 종류, 음수면 한도 초과.
 *
 * VMM 이 프로세스를 갈아탈 때 쓴다. 고정된 페이지는 원래 요청한 프로세스의
 * 한도에 계상되어 있는데, 그 프로세스가 사라지면 계상이 엉킨다.
 *
 * 파일 출처만 허용하는 이유: 사용자 VA 출처의 묶음은 그 프로세스의 주소
 * 공간에 매여 있어 다른 프로세스로 옮길 수 없다.
 *
 * 세 번 도는 구조가 이 함수의 골격이다.
 *  1) 모두 파일 출처인지 확인한다 — 하나라도 아니면 아무것도 바꾸지 않는다.
 *  2) 새로 계상할 양을 센다. 원 주석대로 last_npinned 를 0 으로 만들어
 *     같은 묶음이 여러 IOAS 에 보여도 두 번 세지 않게 한다. 모든 락을
 *     쥐고 있어 npinned 와 last_npinned 가 같으므로 복원이 쉽다.
 *  3) 새 프로세스에 계상해 보고, 성공하면 옛 계상을 되돌리며 소유를 옮긴다.
 *
 * 새 쪽을 먼저 계상하는 이유: 한도를 넘으면 아무것도 바꾸지 않고 물러나야
 * 하는데, 옛 것을 먼저 놓으면 되돌릴 때 다시 한도에 걸릴 수 있다.
 */
int iommufd_ioas_change_process(struct iommufd_ucmd *ucmd)
{
	struct iommu_ioas_change_process *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_ctx *ictx = ucmd->ictx;	/* [한국어] 문맥 */
	unsigned long all_npinned[IOPT_PAGES_ACCOUNT_MODE_NUM] = {};	/* [한국어] 방식별로 옮길 페이지 수 */
	struct iommufd_ioas *ioas;	/* [한국어] 순회용 */
	struct iopt_area *area;	/* [한국어] 순회용 */
	struct iopt_pages *pages;	/* [한국어] 그 구간의 페이지 묶음 */
	struct xarray ioas_list;	/* [한국어] 붙잡은 IOAS 들 */
	unsigned long index;	/* [한국어] xarray 인덱스 */
	int rc;	/* [한국어] 결과 */

	if (cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	xa_init(&ioas_list);	/* [한국어] 빈 목록으로 시작 */
	rc = iommufd_take_all_iova_rwsem(ictx, &ioas_list);	/* [한국어] 모든 IOAS 를 쓰기 락으로 붙잡는다 */
	if (rc)	/* [한국어] 실패면 */
		return rc;	/* [한국어] 아무것도 바꾸지 않는다 */

	for_each_ioas_area(&ioas_list, index, ioas, area)  {	/* [한국어] 첫 순회: 모두 파일 출처인지 */
		if (area->pages->type != IOPT_ADDRESS_FILE) {	/* [한국어] 사용자 VA 출처가 하나라도 있으면 */
			rc = -EINVAL;	/* [한국어] 그 주소 공간에 매여 있어 옮길 수 없다 */
			goto out;	/* [한국어] 아무것도 바꾸지 않고 나간다 */
		}
	}

	/*
	 * Count last_pinned pages, then clear it to avoid double counting
	 * if the same iopt_pages is visited multiple times in this loop.
	 * Since we are under all the locks, npinned == last_npinned, so we
	 * can easily restore last_npinned before we return.
	 */
	for_each_ioas_area(&ioas_list, index, ioas, area)  {	/* [한국어] (원 주석: last_pinned 를 세고 0 으로 만들어 같은 묶음을 두 번 세지 않게 한다) */
		pages = area->pages;	/* [한국어] 그 구간의 묶음 */

		if (need_charge_update(pages)) {	/* [한국어] 옮겨야 하는 묶음이면 */
			all_npinned[pages->account_mode] += pages->last_npinned;	/* [한국어] 그 방식의 몫에 더하고 */
			pages->last_npinned = 0;	/* [한국어] 두 번 세지 않도록 비운다 */
		}
	}

	rc = charge_current(all_npinned);	/* [한국어] 새 프로세스에 먼저 계상해 본다 */

	if (rc) {	/* [한국어] 한도를 넘었으면 */
		/* Charge failed.  Fix last_npinned and bail. */
		for_each_ioas_area(&ioas_list, index, ioas, area)	/* [한국어] (원 주석: 계상 실패 — last_npinned 를 고치고 물러난다) */
			area->pages->last_npinned = area->pages->npinned;	/* [한국어] 모든 락을 쥐고 있어 둘이 같다 */
		goto out;	/* [한국어] 아무것도 바꾸지 않은 셈이 된다 */
	}

	for_each_ioas_area(&ioas_list, index, ioas, area) {	/* [한국어] 셋째 순회: 실제로 옮긴다 */
		pages = area->pages;	/* [한국어] 그 구간의 묶음 */

		/* Uncharge the old one (which also restores last_npinned) */
		if (need_charge_update(pages)) {	/* [한국어] (원 주석: 옛 쪽을 되돌린다 — last_npinned 도 함께 복원된다) */
			int r = iopt_pages_update_pinned(pages, pages->npinned,	/* [한국어] 옛 프로세스의 계상을 */
							 false, NULL);	/* [한국어] 내린다 */

			if (WARN_ON(r))	/* [한국어] 되돌리기는 실패할 수 없다 */
				rc = r;	/* [한국어] 실패하면 알린다 */
		}
		change_mm(pages);	/* [한국어] 소유 프로세스를 바꾼다 */
	}

out:	/* [한국어] 모든 락을 놓고 나가는 공통 경로 */
	iommufd_release_all_iova_rwsem(ictx, &ioas_list);	/* [한국어] 모든 락과 참조를 놓는다 */
	return rc;	/* [한국어] 성패 */
}

/*
 * [한국어]
 * iommufd_option_rlimit_mode - 고정 페이지 계상 방식을 읽고 쓴다
 *
 * @cmd: 옵션 명령.
 * @ictx: 문맥.
 * @return: 0 성공, 음수면 실패.
 *
 * 두 방식이 있다. 사용자 한도(RLIMIT_MEMLOCK)에 계상하는 것과, VFIO 처럼
 * 프로세스 통계에 계상하는 것이다.
 *
 * 후자로 바꾸려면 CAP_SYS_RESOURCE 가 필요하다 — 그쪽은 한도 검사를
 * 우회하는 셈이기 때문이다.
 *
 * 객체가 하나라도 있으면 거절하는 이유: 이미 만들어진 묶음들이 옛 방식으로
 * 계상되어 있어, 방식을 바꾸면 되돌릴 때 짝이 맞지 않는다.
 */
int iommufd_option_rlimit_mode(struct iommu_option *cmd,
			       struct iommufd_ctx *ictx)
{
	if (cmd->object_id)	/* [한국어] 이 옵션은 문맥 단위라 */
		return -EOPNOTSUPP;	/* [한국어] 객체 id 를 받지 않는다 */

	if (cmd->op == IOMMU_OPTION_OP_GET) {	/* [한국어] 읽기 요청이면 */
		cmd->val64 = ictx->account_mode == IOPT_PAGES_ACCOUNT_MM;	/* [한국어] 1 이면 프로세스 통계 방식 */
		return 0;	/* [한국어] 값을 담아 돌려준다 */
	}
	if (cmd->op == IOMMU_OPTION_OP_SET) {	/* [한국어] 쓰기 요청이면 */
		int rc = 0;	/* [한국어] 결과 */

		if (!capable(CAP_SYS_RESOURCE))	/* [한국어] 한도 검사를 우회하는 셈이라 */
			return -EPERM;	/* [한국어] 권한이 필요하다 */

		xa_lock(&ictx->objects);	/* [한국어] 객체 유무를 확인하는 동안 */
		if (!xa_empty(&ictx->objects)) {	/* [한국어] 이미 만들어진 객체가 있으면 */
			rc = -EBUSY;	/* [한국어] 옛 방식으로 계상된 것이 있어 짝이 맞지 않는다 */
		} else {
			if (cmd->val64 == 0)	/* [한국어] 0 이면 */
				ictx->account_mode = IOPT_PAGES_ACCOUNT_USER;	/* [한국어] 사용자 한도 방식 */
			else if (cmd->val64 == 1)	/* [한국어] 1 이면 */
				ictx->account_mode = IOPT_PAGES_ACCOUNT_MM;	/* [한국어] 프로세스 통계 방식 */
			else
				rc = -EINVAL;	/* [한국어] 그 밖의 값은 없다 */
		}
		xa_unlock(&ictx->objects);	/* [한국어] 확인과 변경이 원자적이었다 */

		return rc;	/* [한국어] 성패 */
	}
	return -EOPNOTSUPP;	/* [한국어] 읽기도 쓰기도 아니면 */
}

/*
 * [한국어]
 * iommufd_ioas_option_huge_pages - 큰 페이지 사용 여부를 읽고 쓴다
 *
 * @cmd: 옵션 명령.
 * @ioas: 대상 IOAS.
 * @return: 0 성공, 음수면 실패.
 *
 * 끄면 기존의 큰 페이지도 잘게 쪼갠다. 커널 쪽 소비자가 페이지 단위로
 * 세밀히 다루어야 할 때 필요하다.
 */
static int iommufd_ioas_option_huge_pages(struct iommu_option *cmd,
					  struct iommufd_ioas *ioas)
{
	if (cmd->op == IOMMU_OPTION_OP_GET) {	/* [한국어] 읽기 요청이면 */
		cmd->val64 = !ioas->iopt.disable_large_pages;	/* [한국어] 막지 않았으면 1 */
		return 0;	/* [한국어] 값을 담아 돌려준다 */
	}
	if (cmd->op == IOMMU_OPTION_OP_SET) {	/* [한국어] 쓰기 요청이면 */
		if (cmd->val64 == 0)	/* [한국어] 끄라고 하면 */
			return iopt_disable_large_pages(&ioas->iopt);	/* [한국어] 기존의 큰 페이지도 쪼갠다 */
		if (cmd->val64 == 1) {	/* [한국어] 켜라고 하면 */
			iopt_enable_large_pages(&ioas->iopt);	/* [한국어] 이후 매핑부터 쓸 수 있다 */
			return 0;	/* [한국어] 성공 */
		}
		return -EINVAL;	/* [한국어] 그 밖의 값은 없다 */
	}
	return -EOPNOTSUPP;	/* [한국어] 읽기도 쓰기도 아니면 */
}

/*
 * [한국어]
 * iommufd_ioas_option - IOAS 단위 옵션을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, -EOPNOTSUPP 모르는 옵션.
 *
 * main.c 의 옵션 명령이 IOAS 단위 옵션을 여기로 넘긴다.
 */
int iommufd_ioas_option(struct iommufd_ucmd *ucmd)
{
	struct iommu_option *cmd = ucmd->cmd;	/* [한국어] 사용자가 준 인자 */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 IOAS */
	int rc = 0;	/* [한국어] 결과 */

	if (cmd->__reserved)	/* [한국어] 예약 필드가 0 이 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */

	ioas = iommufd_get_ioas(ucmd->ictx, cmd->object_id);	/* [한국어] 대상을 붙잡고 */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return PTR_ERR(ioas);	/* [한국어] 그대로 */

	switch (cmd->option_id) {	/* [한국어] 옵션 종류에 따라 */
	case IOMMU_OPTION_HUGE_PAGES:	/* [한국어] 큰 페이지 사용 여부 */
		rc = iommufd_ioas_option_huge_pages(cmd, ioas);	/* [한국어] 그 구현으로 */
		break;
	default:	/* [한국어] 모르는 옵션 */
		rc = -EOPNOTSUPP;	/* [한국어] 모르는 옵션 */
	}

	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 객체를 놓는다 */
	return rc;	/* [한국어] 성패 */
}
