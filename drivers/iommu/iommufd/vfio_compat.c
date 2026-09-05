// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] 옛 VFIO type1 ioctl 을 iommufd 위에서 흉내 내는 층 (vfio_compat.c)
 *
 * === 파일의 역할 ===
 * iommufd 가 나오기 전, 사용자 공간이 DMA 매핑을 다루던 인터페이스는
 * VFIO 의 type1 컨테이너였다. QEMU 같은 기존 프로그램이 고쳐 쓰지 않고도
 * iommufd 위에서 돌게 하려고, 그 옛 ioctl 들을 이 파일이 받아 iommufd 의
 * IOAS 연산으로 옮긴다.
 *
 * 옛 ABI 에는 IOAS 를 고르는 자리가 없다. 그래서 문맥마다 "호환 IOAS"
 * 하나를 정해 두고 모든 옛 ioctl 이 그것을 쓰게 한다. 그 IOAS 를
 * 얻어 오는 것이 get_compat_ioas() 이고, 사용자는 IOMMU_VFIO_IOAS 로
 * 그것이 무엇인지 알거나 바꿀 수 있다.
 *
 * 흉내는 완전하지 않다. 구현할 수 없다고 판단해 버린 기능들이 있고
 * (VFIO_UPDATE_VADDR, 옛 방식 더티 추적), 그런 것은 확장 조회에서 0 을
 * 돌려주어 사용자가 쓰지 않게 한다.
 *
 * TYPE1 과 TYPE1v2 의 차이를 큰 페이지 금지로 흉내 내는 부분이 이 파일의
 * 재미있는 대목이다. 옛 TYPE1 은 매핑 한가운데를 풀 수 있어야 하는데,
 * 큰 IOPTE 하나로 덮인 구간은 드라이버가 쪼개 주지 못한다. 그래서 아예
 * 큰 페이지를 쓰지 않게 만들어 어디서나 풀 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * QEMU 등 옛 사용자 → VFIO 컨테이너 fd → vfio 코어 → 이 파일
 *   → iopt_map_user_pages / iopt_unmap_iova (io_pagetable.c)
 *   → iommu 도메인
 *
 * iommufd 를 직접 쓰는 새 사용자는 이 파일을 거치지 않고 main.c 의
 * ioctl 표로 곧장 간다.
 *
 * 실행 컨텍스트: 모두 프로세스 문맥의 ioctl 경로.
 *
 * === 타 모듈과의 연결 ===
 * 위: drivers/vfio 의 컨테이너 코드가 iommufd_vfio_ioctl() 로 넘긴다.
 *   장치가 붙을 때 iommufd_vfio_compat_ioas_create() 를 불러 호환 IOAS 를
 *   마련한다 — 이 함수들이 IOMMUFD_VFIO 네임스페이스로 내보내진다.
 * 아래: ioas.c 의 IOAS 객체, io_pagetable.c 의 매핑 연산.
 *
 * === 주요 함수/구조체 요약 ===
 * get_compat_ioas: 문맥에 정해진 호환 IOAS 를 참조와 함께 얻는다.
 * iommufd_vfio_ioas: IOMMU_VFIO_IOAS 명령으로 그 IOAS 를 읽거나 바꾼다.
 * iommufd_vfio_map_dma / unmap_dma: 옛 매핑·해제 ioctl.
 * iommufd_vfio_check_extension: 어떤 옛 기능을 지원하는지 답한다.
 * iommufd_vfio_iommu_get_info: 페이지 크기와 쓸 수 있는 IOVA 구간을
 *   가변 길이 capability 사슬로 돌려준다.
 * iommufd_vfio_ioctl: 옛 ioctl 번호를 위 함수들로 나눠 보내는 입구.
 */
#include <linux/file.h>
#include <linux/interval_tree.h>
#include <linux/iommu.h>
#include <linux/iommufd.h>
#include <linux/slab.h>
#include <linux/vfio.h>
#include <uapi/linux/vfio.h>
#include <uapi/linux/iommufd.h>

#include "iommufd_private.h"

/*
 * [한국어]
 * get_compat_ioas - 문맥에 정해진 호환 IOAS 를 참조와 함께 얻는다
 *
 * @ictx: 대상 문맥.
 * @return: 참조를 든 IOAS, 없으면 ERR_PTR(-ENODEV).
 *
 * 옛 ABI 에는 IOAS 를 고르는 자리가 없어, 문맥마다 하나를 정해 두고
 * 모든 옛 ioctl 이 그것을 쓴다.
 *
 * 포인터를 읽는 것과 참조를 드는 것이 한 락 안에서 일어나야 한다 —
 * 그 사이에 IOAS 가 파괴되면 이미 죽은 객체를 붙잡게 된다.
 *
 * 호출자는 다 쓰고 iommufd_put_object() 로 참조를 놓아야 한다.
 */
static struct iommufd_ioas *get_compat_ioas(struct iommufd_ctx *ictx)
{
	struct iommufd_ioas *ioas = ERR_PTR(-ENODEV);	/* [한국어] 기본값은 "그런 장치 없음". 아래에서 찾으면 덮어쓴다. */

	xa_lock(&ictx->objects);	/* [한국어] 객체 xarray 의 락. 문맥의 vfio_ioas 포인터도 이 락이 지킨다. */
	if (!ictx->vfio_ioas || !iommufd_lock_obj(&ictx->vfio_ioas->obj))	/* [한국어] 정해진 IOAS 가 없거나, 있어도 이미 파괴 중이면 실패다. lock_obj 가 참조를 들면서 살아 있는지도 함께 본다. */
		goto out_unlock;
	ioas = ictx->vfio_ioas;	/* [한국어] 참조를 들었으니 이제 락 밖에서도 안전하다. */
out_unlock:	/* [한국어] 두 경로가 합류해 락을 놓는다. */
	xa_unlock(&ictx->objects);	/* [한국어] 락 해제. */
	return ioas;	/* [한국어] 참조를 든 IOAS 또는 오류 포인터. */
}

/**
 * iommufd_vfio_compat_ioas_get_id - Ensure a compat IOAS exists
 * @ictx: Context to operate on
 * @out_ioas_id: The IOAS ID of the compatibility IOAS
 *
 * Return the ID of the current compatibility IOAS. The ID can be passed into
 * other functions that take an ioas_id.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_vfio_compat_ioas_get_id - 호환 IOAS 의 id 를 알려 준다
 *
 * @ictx: 문맥.
 * @out_ioas_id: 여기에 id 를 쓴다.
 * @return: 0 성공, IOAS 가 없으면 -ENODEV.
 *
 * vfio 코어가 부른다. 그 id 를 새 인터페이스의 ioas_id 자리에 그대로
 * 넣을 수 있어, 옛 경로와 새 경로가 같은 주소 공간을 가리키게 된다.
 *
 * IOMMUFD_VFIO 네임스페이스로 내보내 vfio 모듈만 쓸 수 있게 한다.
 */
int iommufd_vfio_compat_ioas_get_id(struct iommufd_ctx *ictx, u32 *out_ioas_id)
{
	struct iommufd_ioas *ioas;	/* [한국어] 참조를 받아 둘 지역 변수. */

	ioas = get_compat_ioas(ictx);	/* [한국어] 문맥에 정해진 IOAS 를 참조와 함께 얻는다. */
	if (IS_ERR(ioas))	/* [한국어] 아직 만들어지지 않았다. */
		return PTR_ERR(ioas);	/* [한국어] -ENODEV 를 그대로 올린다. */
	*out_ioas_id = ioas->obj.id;	/* [한국어] 사용자가 아는 객체 id 를 꺼내 준다. */
	iommufd_put_object(ictx, &ioas->obj);	/* [한국어] id 만 필요했으니 곧바로 참조를 놓는다. */
	return 0;	/* [한국어] 성공. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_vfio_compat_ioas_get_id, "IOMMUFD_VFIO");	/* [한국어] vfio 모듈만 쓸 수 있게 IOMMUFD_VFIO 네임스페이스로 내보낸다. 그쪽에서 MODULE_IMPORT_NS 를 선언해야 링크된다. */

/**
 * iommufd_vfio_compat_set_no_iommu - Called when a no-iommu device is attached
 * @ictx: Context to operate on
 *
 * This allows selecting the VFIO_NOIOMMU_IOMMU and blocks normal types.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_vfio_compat_set_no_iommu - 이 문맥을 no-iommu 모드로 표시한다
 *
 * @ictx: 문맥.
 * @return: 0 성공, 이미 IOAS 가 있으면 -EINVAL.
 *
 * IOMMU 를 거치지 않고 물리 주소를 그대로 쓰는 장치가 붙을 때 부른다.
 * 그런 문맥에는 주소 공간이라는 것이 없다.
 *
 * 이미 IOAS 가 있으면 거절한다 — 한 컨테이너를 iommu 방식과 no-iommu
 * 방식에 함께 쓸 수 없다.
 */
int iommufd_vfio_compat_set_no_iommu(struct iommufd_ctx *ictx)
{
	int ret;	/* [한국어] 결과 코드. */

	xa_lock(&ictx->objects);	/* [한국어] vfio_ioas 포인터와 모드 플래그를 함께 지키는 락. */
	if (!ictx->vfio_ioas) {	/* [한국어] 아직 주소 공간을 만들지 않았을 때만 허용한다. */
		ictx->no_iommu_mode = 1;	/* [한국어] 이후 IOAS 생성이 거절되고, NOIOMMU 확장만 지원된다고 답하게 된다. */
		ret = 0;	/* [한국어] 성공. */
	} else {	/* [한국어] 이미 IOAS 가 있으면 */
		ret = -EINVAL;	/* [한국어] 한 컨테이너를 두 방식에 함께 쓸 수 없다. */
	}
	xa_unlock(&ictx->objects);	/* [한국어] 락 해제. */
	return ret;	/* [한국어] 결과를 올린다. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_vfio_compat_set_no_iommu, "IOMMUFD_VFIO");	/* [한국어] 같은 네임스페이스로 내보낸다 — no-iommu 장치가 붙을 때 vfio 코어가 부른다. */

/**
 * iommufd_vfio_compat_ioas_create - Ensure the compat IOAS is created
 * @ictx: Context to operate on
 *
 * The compatibility IOAS is the IOAS that the vfio compatibility ioctls operate
 * on since they do not have an IOAS ID input in their ABI. Only attaching a
 * group should cause a default creation of the internal ioas, this does nothing
 * if an existing ioas has already been assigned somehow.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_vfio_compat_ioas_create - 호환 IOAS 를 마련한다
 *
 * @ictx: 문맥.
 * @return: 0 성공(이미 있어도 성공), 음수면 실패.
 *
 * 옛 ABI 에는 IOAS 를 만드는 명령이 없으므로, 장치를 붙일 때 커널이
 * 대신 하나 만들어 준다.
 *
 * 먼저 만들어 놓고 락 안에서 자리를 다투는 구조다. 락을 쥔 채 할당할 수
 * 없어서인데, 그 사이 다른 스레드가 먼저 채워 넣었으면 내 것을 되돌린다.
 *
 * 원 주석대로 이렇게 자동으로 생긴 IOAS 도 사용자가 만든 것과 똑같이
 * 다뤄진다 — id 를 알아낼 수 있고, 손수 지우지 않으면 문맥이 닫힐 때
 * 함께 사라진다.
 */
int iommufd_vfio_compat_ioas_create(struct iommufd_ctx *ictx)
{
	struct iommufd_ioas *ioas = NULL;	/* [한국어] 만들어 볼 IOAS. */
	int ret;	/* [한국어] 결과 코드. */

	ioas = iommufd_ioas_alloc(ictx);	/* [한국어] 락을 잡기 전에 먼저 만든다 — 할당은 잠들 수 있어 스핀락 안에서 못 한다. */
	if (IS_ERR(ioas))	/* [한국어] 메모리가 없다. */
		return PTR_ERR(ioas);	/* [한국어] 오류를 올린다. */

	xa_lock(&ictx->objects);	/* [한국어] 여기서부터 자리 다툼을 해결한다. */
	/*
	 * VFIO won't allow attaching a container to both iommu and no iommu
	 * operation
	 */
	if (ictx->no_iommu_mode) {	/* [한국어] no-iommu 로 표시된 문맥에는 주소 공간을 둘 수 없다. */
		ret = -EINVAL;	/* [한국어] 거절. */
		goto out_abort;	/* [한국어] 방금 만든 것을 되돌린다. */
	}

	if (ictx->vfio_ioas && iommufd_lock_obj(&ictx->vfio_ioas->obj)) {	/* [한국어] 그 사이 다른 스레드가 이미 채워 넣었고 그것이 살아 있다면 */
		ret = 0;	/* [한국어] 이 함수의 목적은 "있게 하는 것"이므로 성공이다. */
		iommufd_put_object(ictx, &ictx->vfio_ioas->obj);	/* [한국어] 확인용으로 든 참조를 곧바로 놓는다. */
		goto out_abort;	/* [한국어] 내가 만든 것은 필요 없어졌다. */
	}
	ictx->vfio_ioas = ioas;	/* [한국어] 내 것이 채택됐다. */
	xa_unlock(&ictx->objects);	/* [한국어] 공개 전에 락을 놓는다. */

	/*
	 * An automatically created compat IOAS is treated as a userspace
	 * created object. Userspace can learn the ID via IOMMU_VFIO_IOAS_GET,
	 * and if not manually destroyed it will be destroyed automatically
	 * at iommufd release.
	 */
	iommufd_object_finalize(ictx, &ioas->obj);	/* [한국어] 사용자에게 보이게 만든다 — 원 주석대로 사용자가 만든 객체와 똑같이 다뤄진다. */
	return 0;	/* [한국어] 성공. */

out_abort:	/* [한국어] 실패 또는 경합에 진 경로. */
	xa_unlock(&ictx->objects);	/* [한국어] 락을 놓고 */
	iommufd_object_abort(ictx, &ioas->obj);	/* [한국어] 공개하지 않은 객체를 되돌린다. finalize 전이라 abort 로 충분하다. */
	return ret;	/* [한국어] 0(이미 있음) 또는 오류. */
}
EXPORT_SYMBOL_NS_GPL(iommufd_vfio_compat_ioas_create, "IOMMUFD_VFIO");	/* [한국어] 장치를 붙일 때 vfio 코어가 불러 호환 IOAS 를 마련하게 한다. */

/*
 * [한국어]
 * iommufd_vfio_ioas - IOMMU_VFIO_IOAS 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @return: 0 성공, 음수면 실패.
 *
 * 새 인터페이스 쪽에서 호환 IOAS 를 들여다보거나 바꾸는 창구다. 옛
 * ioctl 과 새 ioctl 을 한 프로그램이 섞어 쓸 때, 둘이 같은 주소 공간을
 * 보게 맞추는 데 쓴다.
 *
 * GET 은 지금 무엇인지 알려 주고, SET 은 그것을 지정한 IOAS 로 바꾸며,
 * CLEAR 는 아무것도 가리키지 않게 한다.
 *
 * SET 이 참조를 들지 않고 놓아 버리는 데 주의. 문맥의 vfio_ioas 는
 * 약한 포인터라서, 쓰는 쪽(get_compat_ioas)이 락 안에서 살아 있는지
 * 확인하고 그때 참조를 든다.
 */
int iommufd_vfio_ioas(struct iommufd_ucmd *ucmd)
{
	struct iommu_vfio_ioas *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */
	struct iommufd_ioas *ioas;	/* [한국어] 다루는 IOAS. */

	if (cmd->__reserved)	/* [한국어] 예약 필드는 0 이어야 나중에 의미를 붙일 수 있다. */
		return -EOPNOTSUPP;
	switch (cmd->op) {	/* [한국어] 읽기·쓰기·지우기 셋 중 하나. */
	case IOMMU_VFIO_IOAS_GET:	/* [한국어] 지금 정해진 IOAS 를 묻는다. */
		ioas = get_compat_ioas(ucmd->ictx);	/* [한국어] 참조와 함께 얻는다. */
		if (IS_ERR(ioas))	/* [한국어] 아직 호환 IOAS 가 없다. */
			return PTR_ERR(ioas);	/* [한국어] 없으면 -ENODEV. */
		cmd->ioas_id = ioas->obj.id;	/* [한국어] 사용자에게 알릴 id. */
		iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 참조를 놓는다. */
		return iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 값을 사용자 버퍼에 되돌려 쓴다. */

	case IOMMU_VFIO_IOAS_SET:	/* [한국어] 다른 IOAS 를 호환 IOAS 로 지정한다. */
		ioas = iommufd_get_ioas(ucmd->ictx, cmd->ioas_id);	/* [한국어] id 가 실제로 IOAS 인지 확인하며 참조를 든다. */
		if (IS_ERR(ioas))	/* [한국어] 그런 id 의 IOAS 가 없다. */
			return PTR_ERR(ioas);	/* [한국어] 없는 id 다. */
		xa_lock(&ucmd->ictx->objects);	/* [한국어] 포인터를 바꾸므로 락. */
		ucmd->ictx->vfio_ioas = ioas;	/* [한국어] 약한 포인터로 기억한다 — 참조를 계속 들지 않는다. */
		xa_unlock(&ucmd->ictx->objects);	/* [한국어] 락 해제. */
		iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 확인용 참조를 놓는다. 이 IOAS 가 파괴되면 포인터는 남지만 get_compat_ioas 가 lock_obj 로 걸러 낸다. */
		return 0;	/* [한국어] 성공. 돌려줄 값이 없어 respond 를 부르지 않는다. */

	case IOMMU_VFIO_IOAS_CLEAR:	/* [한국어] 지정을 지운다. */
		xa_lock(&ucmd->ictx->objects);	/* [한국어] 포인터를 바꾸므로 락. */
		ucmd->ictx->vfio_ioas = NULL;	/* [한국어] 이후 옛 ioctl 은 모두 -ENODEV 가 된다. */
		xa_unlock(&ucmd->ictx->objects);	/* [한국어] 락 해제. */
		return 0;	/* [한국어] 성공. */
	default:	/* [한국어] 모르는 연산. */
		return -EOPNOTSUPP;	/* [한국어] 지원하지 않는다. */
	}
}

/*
 * [한국어]
 * iommufd_vfio_map_dma - VFIO_IOMMU_MAP_DMA 를 흉내 낸다
 *
 * @ictx: 문맥.
 * @cmd: ioctl 번호(쓰이지 않는다 — 서명을 다른 처리기와 맞추려고 받는다).
 * @arg: 사용자 인자 포인터.
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자가 고른 IOVA 에 자기 메모리를 붙인다. 새 인터페이스와 달리
 * 커널이 IOVA 를 골라 주는 방식은 없다.
 *
 * 옛 인터페이스로 만든 매핑은 늘 VFIO 방식의 rlimit 계산을 쓴다.
 * 원 주석대로, 더 빠른 사용자 기준 계산을 원하면 새 인터페이스를 써야
 * 한다 — 옛 프로그램의 한도 감각을 바꾸지 않기 위해서다.
 *
 * IOMMU_CACHE 를 처음부터 넣는 이유: 옛 ABI 에는 캐시 일관성을 고르는
 * 플래그가 없고, type1 은 늘 일관된 매핑을 뜻했다.
 */
static int iommufd_vfio_map_dma(struct iommufd_ctx *ictx, unsigned int cmd,
				void __user *arg)
{
	u32 supported_flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;	/* [한국어] 받아들이는 플래그의 전부. 나머지가 켜져 있으면 거절해야 나중에 의미를 붙일 수 있다. */
	size_t minsz = offsetofend(struct vfio_iommu_type1_dma_map, size);	/* [한국어] 옛 ABI 는 구조체를 뒤로 늘려 왔다. 반드시 있어야 하는 최소 크기를 여기까지로 잡는다. */
	struct vfio_iommu_type1_dma_map map;	/* [한국어] 사용자 인자를 받아 둘 버퍼. */
	int iommu_prot = IOMMU_CACHE;	/* [한국어] 옛 ABI 에는 캐시 일관성 선택이 없다. type1 은 늘 일관된 매핑을 뜻했으므로 처음부터 켜 둔다. */
	struct iommufd_ioas *ioas;	/* [한국어] 매핑을 넣을 주소 공간. */
	unsigned long iova;	/* [한국어] 요청한 IOVA. iopt_map_user_pages 가 in/out 으로 쓰므로 지역 변수에 옮겨 넘긴다. */
	int rc;	/* [한국어] 결과 코드. */

	if (copy_from_user(&map, arg, minsz))	/* [한국어] 최소 크기만 가져온다 — 사용자가 더 짧은 구조체를 쓸 수도 있다. */
		return -EFAULT;	/* [한국어] 사용자 주소가 잘못됐다. */

	if (map.argsz < minsz || map.flags & ~supported_flags)	/* [한국어] 자기가 말한 크기가 최소에 못 미치거나 모르는 플래그가 켜져 있으면 거절. */
		return -EINVAL;	/* [한국어] 잘못된 인자. */

	if (map.flags & VFIO_DMA_MAP_FLAG_READ)	/* [한국어] 읽기 권한 요청. */
		iommu_prot |= IOMMU_READ;	/* [한국어] 장치가 이 구간을 읽을 수 있게 한다. */
	if (map.flags & VFIO_DMA_MAP_FLAG_WRITE)	/* [한국어] 쓰기 권한 요청. */
		iommu_prot |= IOMMU_WRITE;	/* [한국어] 장치가 이 구간에 쓸 수 있게 한다. */

	ioas = get_compat_ioas(ictx);	/* [한국어] 옛 ABI 가 쓰는 그 하나의 주소 공간. */
	if (IS_ERR(ioas))	/* [한국어] 호환 IOAS 가 없으면 매핑할 곳도 없다. */
		return PTR_ERR(ioas);	/* [한국어] 없으면 -ENODEV. */

	/*
	 * Maps created through the legacy interface always use VFIO compatible
	 * rlimit accounting. If the user wishes to use the faster user based
	 * rlimit accounting then they must use the new interface.
	 */
	iova = map.iova;	/* [한국어] 사용자가 고른 IOVA. 새 인터페이스와 달리 커널이 골라 주는 길은 없다. */
	rc = iopt_map_user_pages(ictx, &ioas->iopt, &iova, u64_to_user_ptr(map.vaddr),	/* [한국어] 사용자 메모리를 고정(pin)하고 그 IOVA 에 붙인다. 마지막 0 이 플래그 자리로, VFIO 방식 rlimit 계산을 뜻한다. */
				 map.size, iommu_prot, 0);
	iommufd_put_object(ictx, &ioas->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 매핑 결과. */
}

/*
 * [한국어]
 * iommufd_vfio_unmap_dma - VFIO_IOMMU_UNMAP_DMA 를 흉내 낸다
 *
 * @ictx: 문맥.
 * @cmd: ioctl 번호(쓰이지 않는다).
 * @arg: 사용자 인자 포인터.
 * @return: 0 성공, 음수면 실패. 실제로 푼 크기는 사용자 구조체에 쓴다.
 *
 * ALL 플래그면 그 IOAS 의 모든 매핑을 푼다. 아니면 지정한 구간만 푼다.
 *
 * 큰 페이지가 금지된 IOAS(TYPE1 흉내)에서는 먼저 구간의 양 끝에 "자름"을
 * 넣는다. 요청 구간이 기존 매핑의 한가운데를 가로지를 수 있는데, 영역을
 * 미리 그 경계에서 쪼개 두어야 부분 해제가 가능하다. 시작이 0 이면 그
 * 앞은 자를 것이 없어 하나만 넣는다.
 *
 * 원 주석이 밝히듯 더티 비트맵을 함께 받아 오는 옛 플래그는 지원하지
 * 않는다 — 더티 추적은 새 인터페이스로 방향이 바뀌었다.
 */
static int iommufd_vfio_unmap_dma(struct iommufd_ctx *ictx, unsigned int cmd,
				  void __user *arg)
{
	size_t minsz = offsetofend(struct vfio_iommu_type1_dma_unmap, size);	/* [한국어] 반드시 있어야 하는 최소 구조체 크기. */
	/*
	 * VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP is obsoleted by the new
	 * dirty tracking direction:
	 *  https://lore.kernel.org/kvm/20220731125503.142683-1-yishaih@nvidia.com/
	 *  https://lore.kernel.org/kvm/20220428210933.3583-1-joao.m.martins@oracle.com/
	 */
	u32 supported_flags = VFIO_DMA_UNMAP_FLAG_ALL;	/* [한국어] 받아들이는 플래그는 "전부 해제" 하나뿐이다. 더티 비트맵 플래그는 위 주석대로 폐기됐다. */
	struct vfio_iommu_type1_dma_unmap unmap;	/* [한국어] 사용자 인자 버퍼. 실제로 푼 크기를 돌려주는 데도 쓴다. */
	unsigned long unmapped = 0;	/* [한국어] 실제로 푼 바이트 수를 받는다. */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 주소 공간. */
	int rc;	/* [한국어] 결과 코드. */

	if (copy_from_user(&unmap, arg, minsz))	/* [한국어] 최소 크기만 가져온다. */
		return -EFAULT;	/* [한국어] 사용자 주소 오류. */

	if (unmap.argsz < minsz || unmap.flags & ~supported_flags)	/* [한국어] 크기와 플래그를 검사한다. */
		return -EINVAL;	/* [한국어] 잘못된 인자. */

	ioas = get_compat_ioas(ictx);	/* [한국어] 호환 IOAS 를 참조와 함께 얻는다. */
	if (IS_ERR(ioas))	/* [한국어] 호환 IOAS 가 없으면 풀 것도 없다. */
		return PTR_ERR(ioas);	/* [한국어] 없으면 -ENODEV. */

	if (unmap.flags & VFIO_DMA_UNMAP_FLAG_ALL) {	/* [한국어] 전부 해제 요청이면 */
		if (unmap.iova != 0 || unmap.size != 0) {	/* [한국어] 구간을 함께 지정하는 것은 모순이다. */
			rc = -EINVAL;	/* [한국어] 거절. */
			goto err_put;	/* [한국어] 참조를 놓고 나간다. */
		}
		rc = iopt_unmap_all(&ioas->iopt, &unmapped);	/* [한국어] 이 IOAS 의 모든 영역을 푼다. */
	} else {	/* [한국어] 구간을 지정한 해제라면 */
		if (READ_ONCE(ioas->iopt.disable_large_pages)) {	/* [한국어] TYPE1 흉내로 큰 페이지가 금지된 IOAS 인지 본다. */
			/*
			 * Create cuts at the start and last of the requested
			 * range. If the start IOVA is 0 then it doesn't need to
			 * be cut.
			 */
			unsigned long iovas[] = { unmap.iova + unmap.size - 1,	/* [한국어] 자를 두 지점: 구간의 마지막 주소와, 구간 바로 앞 주소. */
						  unmap.iova - 1 };	/* [한국어] 시작이 0 이면 이 값은 쓰이지 않는다(아래에서 개수를 1 로 준다). */

			rc = iopt_cut_iova(&ioas->iopt, iovas,	/* [한국어] 요청 구간의 경계에서 영역을 쪼갠다. 그래야 매핑 한가운데를 풀 수 있다. */
					   unmap.iova ? 2 : 1);	/* [한국어] 시작이 0 이면 그 앞에는 자를 것이 없어 한 곳만 자른다. */
			if (rc)	/* [한국어] 쪼개기에 실패하면 해제도 할 수 없다. */
				goto err_put;	/* [한국어] 참조를 놓고 나간다. */
		}
		rc = iopt_unmap_iova(&ioas->iopt, unmap.iova, unmap.size,	/* [한국어] 지정한 구간을 푼다. */
				     &unmapped);
	}
	unmap.size = unmapped;	/* [한국어] 실제로 푼 크기를 사용자에게 알린다 — 요청보다 작을 수 있다. */
	if (copy_to_user(arg, &unmap, minsz))	/* [한국어] 되돌려 쓴다. */
		rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */

err_put:	/* [한국어] 성공과 실패가 함께 지나는 정리 지점. */
	iommufd_put_object(ictx, &ioas->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_vfio_cc_iommu - VFIO_DMA_CC_IOMMU 확장을 답한다
 *
 * @ictx: 문맥.
 * @return: 1 이면 모든 도메인이 캐시 일관성을 강제한다, 0 이면 아니다,
 *   음수면 오류.
 *
 * "이 컨테이너의 DMA 는 캐시 일관성이 보장되는가"를 묻는 질문이다.
 * KVM 이 이 답을 보고 게스트에게 캐시 조작 명령을 허용할지 정한다.
 *
 * 하나라도 강제하지 못하는 도메인이 있으면 전체가 아니라고 답해야 한다.
 */
static int iommufd_vfio_cc_iommu(struct iommufd_ctx *ictx)
{
	struct iommufd_hwpt_paging *hwpt_paging;	/* [한국어] IOAS 에 붙어 있는 페이징 도메인 하나. */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 주소 공간. */
	int rc = 1;	/* [한국어] 기본은 "일관성 보장됨". 아니라는 증거를 찾으면 0 으로 내린다. */

	ioas = get_compat_ioas(ictx);	/* [한국어] 호환 IOAS 를 얻는다. */
	if (IS_ERR(ioas))	/* [한국어] 조회할 주소 공간이 없다. */
		return PTR_ERR(ioas);	/* [한국어] 없으면 -ENODEV. */

	mutex_lock(&ioas->mutex);	/* [한국어] 도메인 목록을 지키는 뮤텍스. */
	list_for_each_entry(hwpt_paging, &ioas->hwpt_list, hwpt_item) {	/* [한국어] 이 IOAS 를 쓰는 모든 페이징 도메인. */
		if (!hwpt_paging->enforce_cache_coherency) {	/* [한국어] 하나라도 강제하지 못하면 */
			rc = 0;	/* [한국어] 전체가 보장되지 않는다고 답해야 한다. */
			break;	/* [한국어] 더 볼 필요가 없다. */
		}
	}
	mutex_unlock(&ioas->mutex);	/* [한국어] 뮤텍스 해제. */

	iommufd_put_object(ictx, &ioas->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 1 이면 지원, 0 이면 아님. */
}

/*
 * [한국어]
 * iommufd_vfio_check_extension - VFIO_CHECK_EXTENSION 을 답한다
 *
 * @ictx: 문맥.
 * @type: 묻는 확장 번호.
 * @return: 0 이면 지원하지 않음, 양수면 지원.
 *
 * 사용자가 어떤 옛 기능을 쓸 수 있는지 미리 묻는 자리다. 여기서 0 을
 * 돌려주면 사용자는 그 기능을 쓰지 않는 길로 간다 — 흉내 낼 수 없는
 * 기능을 거절하는 가장 깔끔한 방법이다.
 *
 * VFIO_UPDATE_VADDR 는 원 주석대로 안전하게 구현할 방법이 보이지 않아
 * 거절한다.
 */
static int iommufd_vfio_check_extension(struct iommufd_ctx *ictx,
					unsigned long type)
{
	switch (type) {	/* [한국어] 묻는 확장 번호로 나눈다. */
	case VFIO_TYPE1_IOMMU:	/* [한국어] 옛 type1. */
	case VFIO_TYPE1v2_IOMMU:	/* [한국어] 한가운데 해제를 금지한 type1 개정판. */
	case VFIO_UNMAP_ALL:	/* [한국어] 전부 해제 플래그. */
		return !ictx->no_iommu_mode;	/* [한국어] 이 셋은 IOMMU 를 쓰는 문맥에서만 뜻이 있다. */

	case VFIO_NOIOMMU_IOMMU:	/* [한국어] IOMMU 없이 물리 주소를 그대로 쓰는 모드. */
		return IS_ENABLED(CONFIG_VFIO_NOIOMMU);	/* [한국어] 커널이 그 기능을 넣고 빌드됐을 때만 지원한다. */

	case VFIO_DMA_CC_IOMMU:	/* [한국어] 캐시 일관성 강제 여부. */
		return iommufd_vfio_cc_iommu(ictx);	/* [한국어] 붙어 있는 도메인들을 실제로 살펴 답한다. */

	case __VFIO_RESERVED_TYPE1_NESTING_IOMMU:	/* [한국어] 옛 중첩 IOMMU 규격. 이름에 예약 표시가 붙은 채 폐기됐다. */
		return 0;	/* [한국어] 지원하지 않는다. */

	/*
	 * VFIO_DMA_MAP_FLAG_VADDR
	 * https://lore.kernel.org/kvm/1611939252-7240-1-git-send-email-steven.sistare@oracle.com/
	 * https://lore.kernel.org/all/Yz777bJZjTyLrHEQ@nvidia.com/
	 *
	 * It is hard to see how this could be implemented safely.
	 */
	case VFIO_UPDATE_VADDR:	/* [한국어] 매핑된 사용자 주소를 나중에 바꾸는 기능. */
	default:	/* [한국어] 모르는 확장 번호도 여기로 온다. */
		return 0;	/* [한국어] 지원하지 않는다고 답하면 사용자가 그 길로 가지 않는다. */
	}
}

/*
 * [한국어]
 * iommufd_vfio_set_iommu - VFIO_SET_IOMMU 를 흉내 낸다
 *
 * @ictx: 문맥.
 * @type: 사용자가 고른 IOMMU 종류.
 * @return: 0 성공, 음수면 실패.
 *
 * 옛 사용자는 컨테이너에 어떤 IOMMU 모형을 쓸지 여기서 고른다.
 *
 * TYPE1 과 TYPE1v2 의 차이가 이 함수의 핵심이다. 원 주석대로 TYPE1 은
 * 매핑 한가운데를 풀 수 있어야 하는데, 큰 IOPTE 하나로 덮인 구간은
 * 드라이버가 쪼개 주지 못한다. 오래된 규격이라 쓰는 곳도 거의 없어,
 * 아예 큰 페이지를 금지해 어디서나 풀 수 있게 만드는 것으로 흉내 낸다.
 *
 * NOIOMMU 모드는 IOMMU 없이 물리 주소를 그대로 쓰는 방식이라 IOAS 가
 * 아예 없다. 원 주석대로 흉내가 불완전하지만, 최소한 권한 검사는 한다.
 */
static int iommufd_vfio_set_iommu(struct iommufd_ctx *ictx, unsigned long type)
{
	bool no_iommu_mode = READ_ONCE(ictx->no_iommu_mode);	/* [한국어] 락 없이 읽으므로 컴파일러가 값을 쪼개거나 다시 읽지 않게 한다. */
	struct iommufd_ioas *ioas = NULL;	/* [한국어] 대상 주소 공간. */
	int rc = 0;	/* [한국어] 결과 코드. */

	/*
	 * Emulation for NOIOMMU is imperfect in that VFIO blocks almost all
	 * other ioctls. We let them keep working but they mostly fail since no
	 * IOAS should exist.
	 */
	if (IS_ENABLED(CONFIG_VFIO_NOIOMMU) && type == VFIO_NOIOMMU_IOMMU &&	/* [한국어] no-iommu 로 표시된 문맥에 no-iommu 를 고르는 경우. */
	    no_iommu_mode) {
		if (!capable(CAP_SYS_RAWIO))	/* [한국어] IOMMU 보호 없이 DMA 를 시키는 것이라 강한 권한을 요구한다. */
			return -EPERM;	/* [한국어] 권한이 없다. */
		return 0;	/* [한국어] IOAS 없이 성공으로 끝낸다. */
	}

	if ((type != VFIO_TYPE1_IOMMU && type != VFIO_TYPE1v2_IOMMU) ||	/* [한국어] 흉내 내는 종류는 이 둘뿐이고, */
	    no_iommu_mode)	/* [한국어] no-iommu 문맥에서는 그 둘도 쓸 수 없다. */
		return -EINVAL;	/* [한국어] 거절. */

	/* VFIO fails the set_iommu if there is no group */
	ioas = get_compat_ioas(ictx);	/* [한국어] 원 주석대로 VFIO 는 그룹이 붙지 않은 컨테이너의 set_iommu 를 실패시킨다 — 여기서는 IOAS 가 없는 것이 그 상태다. */
	if (IS_ERR(ioas))	/* [한국어] 원 주석대로 그룹(=IOAS)이 없으면 set_iommu 는 실패한다. */
		return PTR_ERR(ioas);	/* [한국어] -ENODEV. */

	/*
	 * The difference between TYPE1 and TYPE1v2 is the ability to unmap in
	 * the middle of mapped ranges. This is complicated by huge page support
	 * which creates single large IOPTEs that cannot be split by the iommu
	 * driver. TYPE1 is very old at this point and likely nothing uses it,
	 * however it is simple enough to emulate by simply disabling the
	 * problematic large IOPTEs. Then we can safely unmap within any range.
	 */
	if (type == VFIO_TYPE1_IOMMU)	/* [한국어] 옛 TYPE1 이라면 */
		rc = iopt_disable_large_pages(&ioas->iopt);	/* [한국어] 큰 페이지를 금지한다. 쪼갤 수 없는 큰 IOPTE 를 아예 만들지 않아, 어디서나 부분 해제가 가능해진다. */
	iommufd_put_object(ictx, &ioas->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_get_pagesizes - 이 IOAS 가 쓸 수 있는 페이지 크기 비트맵
 *
 * @ioas: 대상 IOAS.
 * @return: 지원 크기의 비트맵. 비트 n 이 서면 2^n 크기를 쓸 수 있다.
 *
 * 붙어 있는 모든 도메인이 함께 지원하는 크기만 쓸 수 있으므로 교집합을
 * 취한다. 하나도 없으면 ULONG_MAX 그대로여서 "무엇이든 된다"가 된다.
 *
 * PAGE_SIZE 보정은 원 주석이 가리키는 vfio_update_pgsize_bitmap() 과 같은
 * 취지다. 호스트 페이지보다 작은 단위는 어차피 사용자가 쓸 수 없으므로
 * 지우고, 대신 호스트 페이지 크기를 넣어 준다.
 */
static unsigned long iommufd_get_pagesizes(struct iommufd_ioas *ioas)
{
	struct io_pagetable *iopt = &ioas->iopt;	/* [한국어] 이 IOAS 의 페이지 테이블 관리 구조. */
	unsigned long pgsize_bitmap = ULONG_MAX;	/* [한국어] 모든 비트를 세워 두고 교집합을 취해 나간다. */
	struct iommu_domain *domain;	/* [한국어] 붙어 있는 도메인 하나. */
	unsigned long index;	/* [한국어] 도메인 xarray 순회용 첨자. */

	down_read(&iopt->domains_rwsem);	/* [한국어] 도메인 목록을 읽는 동안 바뀌지 않게 한다. */
	xa_for_each(&iopt->domains, index, domain)	/* [한국어] 이 IOAS 에 붙은 모든 도메인. */
		pgsize_bitmap &= domain->pgsize_bitmap;	/* [한국어] 모두가 지원하는 크기만 남긴다. */

	/* See vfio_update_pgsize_bitmap() */
	if (pgsize_bitmap & ~PAGE_MASK) {	/* [한국어] 호스트 페이지보다 작은 크기를 지원한다고 나오면 */
		pgsize_bitmap &= PAGE_MASK;	/* [한국어] 그 비트들을 지운다 — 사용자는 어차피 그보다 잘게 매핑할 수 없다. */
		pgsize_bitmap |= PAGE_SIZE;	/* [한국어] 대신 호스트 페이지 크기를 세워 준다. */
	}
	pgsize_bitmap = max(pgsize_bitmap, ioas->iopt.iova_alignment);	/* [한국어] IOVA 정렬 제약이 더 크면 그것이 실질적인 최소 단위다. */
	up_read(&iopt->domains_rwsem);	/* [한국어] 락 해제. */
	return pgsize_bitmap;	/* [한국어] 지원 크기 비트맵. */
}

/*
 * [한국어]
 * iommufd_fill_cap_iova - 쓸 수 있는 IOVA 구간 목록 capability 를 채운다
 *
 * @ioas: 대상 IOAS.
 * @cur: 채워 넣을 사용자 주소, 자리 계산만 할 때는 NULL 이 아니라 헛
 *   주소일 수 있다.
 * @avail: 그 자리에 남은 바이트 수.
 * @return: 이 capability 가 차지하는 바이트 수, 실패하면 음수.
 *
 * 예약된 구간(reserved_itree)의 "구멍"이 곧 사용자가 쓸 수 있는 IOVA 다.
 * 그래서 구멍만 골라 목록으로 만든다.
 *
 * 자리가 모자라도 오류가 아니다. 개수는 끝까지 세어 필요한 크기를
 * 돌려주고, 사용자가 더 큰 버퍼로 다시 묻게 한다.
 */
static int iommufd_fill_cap_iova(struct iommufd_ioas *ioas,
				 struct vfio_info_cap_header __user *cur,
				 size_t avail)
{
	struct vfio_iommu_type1_info_cap_iova_range __user *ucap_iovas =	/* [한국어] 사용자 쪽 capability 의 시작 주소. 넘겨받은 것은 그 안의 header 주소라 container_of 로 되짚는다. */
		container_of(cur,	/* [한국어] __user 표시가 붙은 채로 계산해도 되는 이유는 오프셋 뺄셈뿐이기 때문이다. */
			     struct vfio_iommu_type1_info_cap_iova_range __user,
			     header);
	struct vfio_iommu_type1_info_cap_iova_range cap_iovas = {	/* [한국어] 커널 쪽 머리말 사본. 개수를 세어 마지막에 한 번에 쓴다. */
		.header = {	/* [한국어] capability 사슬의 공통 머리말. 종류 번호와 판, 그리고 다음 것을 가리키는 오프셋이 들어간다. */
			.id = VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE,	/* [한국어] 이 capability 가 IOVA 구간 목록임을 알리는 번호. */
			.version = 1,	/* [한국어] 이 capability 의 판. 사용자가 해석 방법을 고르는 데 쓴다. */
		},
	};
	struct interval_tree_span_iter span;	/* [한국어] 구간 트리의 "채워진 곳과 빈 곳"을 번갈아 훑는 반복자. */

	interval_tree_for_each_span(&span, &ioas->iopt.reserved_itree, 0,	/* [한국어] 예약 트리 전체를 훑는다. 0 부터 ULONG_MAX 까지가 주소 공간 전부다. */
				    ULONG_MAX) {
		struct vfio_iova_range range;	/* [한국어] 사용자에게 넘길 구간 하나. */

		if (!span.is_hole)	/* [한국어] 예약된(쓸 수 없는) 구간은 건너뛴다. */
			continue;
		range.start = span.start_hole;	/* [한국어] 빈 구간의 시작 = 쓸 수 있는 IOVA 의 시작. */
		range.end = span.last_hole;	/* [한국어] 빈 구간의 마지막 주소(포함). */
		if (avail >= struct_size(&cap_iovas, iova_ranges,	/* [한국어] 이 구간까지 담을 자리가 있는지 본다. 없으면 세기만 한다. */
					 cap_iovas.nr_iovas + 1) &&
		    copy_to_user(&ucap_iovas->iova_ranges[cap_iovas.nr_iovas],	/* [한국어] 자리가 있으니 사용자 배열에 채운다. */
				 &range, sizeof(range)))
			return -EFAULT;	/* [한국어] 사용자 주소 오류. */
		cap_iovas.nr_iovas++;	/* [한국어] 자리가 없어 못 썼더라도 개수는 센다 — 필요한 크기를 알려 주어야 한다. */
	}
	if (avail >= struct_size(&cap_iovas, iova_ranges, cap_iovas.nr_iovas) &&	/* [한국어] 머리말까지 담을 자리가 있으면 */
	    copy_to_user(ucap_iovas, &cap_iovas, sizeof(cap_iovas)))	/* [한국어] 개수가 확정된 머리말을 마지막에 쓴다. */
		return -EFAULT;	/* [한국어] 사용자 주소 오류. */
	return struct_size(&cap_iovas, iova_ranges, cap_iovas.nr_iovas);	/* [한국어] 이 capability 가 필요로 하는 전체 크기. */
}

/*
 * [한국어]
 * iommufd_fill_cap_dma_avail - 남은 매핑 가능 개수 capability 를 채운다
 *
 * @ioas: 대상 IOAS(여기서는 쓰이지 않는다 — 표의 서명을 맞추려고 받는다).
 * @cur: 채워 넣을 사용자 주소.
 * @avail: 남은 바이트 수.
 * @return: 이 capability 의 크기, 실패하면 음수.
 *
 * 원 주석이 U32_MAX 를 쓰는 이유를 밝힌다 — iommufd 는 개수가 아니라
 * cgroup 메모리 한도로 제한하므로 사실상 무제한인데, S390 의 qemu 가 이
 * 값을 실제로 보고 판단하기 때문에 U16_MAX 보다 큰 값이 필요했다.
 */
static int iommufd_fill_cap_dma_avail(struct iommufd_ioas *ioas,
				      struct vfio_info_cap_header __user *cur,
				      size_t avail)
{
	struct vfio_iommu_type1_info_dma_avail cap_dma = {	/* [한국어] 커널 쪽에서 통째로 만들어 한 번에 복사한다. */
		.header = {	/* [한국어] 이쪽도 같은 공통 머리말이다. next 는 상위에서 나중에 채운다. */
			.id = VFIO_IOMMU_TYPE1_INFO_DMA_AVAIL,	/* [한국어] 남은 매핑 개수를 알리는 capability 번호. */
			.version = 1,	/* [한국어] 이 capability 의 판. */
		},
		/*
		 * iommufd's limit is based on the cgroup's memory limit.
		 * Normally vfio would return U16_MAX here, and provide a module
		 * parameter to adjust it. Since S390 qemu userspace actually
		 * pays attention and needs a value bigger than U16_MAX return
		 * U32_MAX.
		 */
		.avail = U32_MAX,	/* [한국어] 위 주석대로, 실제 제한이 개수가 아니라 cgroup 메모리라서 사실상 무제한을 뜻하는 값을 넣는다. */
	};

	if (avail >= sizeof(cap_dma) &&	/* [한국어] 자리가 있을 때만 쓴다. */
	    copy_to_user(cur, &cap_dma, sizeof(cap_dma)))	/* [한국어] 사용자 버퍼로 복사. */
		return -EFAULT;	/* [한국어] 사용자 주소 오류. */
	return sizeof(cap_dma);	/* [한국어] 이 capability 의 크기(자리가 없었어도 알려 준다). */
}

/*
 * [한국어]
 * iommufd_vfio_iommu_get_info - VFIO_IOMMU_GET_INFO 를 흉내 낸다
 *
 * @ictx: 문맥.
 * @arg: 사용자 인자 포인터.
 * @return: 0 성공, 음수면 실패.
 *
 * 고정 부분(플래그와 페이지 크기) 뒤에 가변 길이 capability 들이 사슬로
 * 이어지는 옛 ABI 다. 각 capability 는 next 오프셋으로 다음을 가리킨다.
 *
 * 채우기 함수들을 표로 두고 같은 고리를 두 번 돈다 — 자리가 있으면
 * 채우고, 없으면 크기만 센다. 그래서 사용자는 작은 버퍼로 한 번 물어
 * 필요한 크기를 알아낸 뒤 다시 부르는 방식을 쓸 수 있다.
 *
 * next 를 뒤늦게 쓰는 것이 눈에 띈다. 앞 capability 의 next 는 다음
 * capability 의 위치를 알아야 쓸 수 있으므로 한 바퀴 늦게 채운다.
 */
static int iommufd_vfio_iommu_get_info(struct iommufd_ctx *ictx,
				       void __user *arg)
{
	typedef int (*fill_cap_fn)(struct iommufd_ioas *ioas,	/* [한국어] 채우기 함수들의 공통 서명. 표로 묶어 같은 고리에서 부르려고 정의한다. */
				   struct vfio_info_cap_header __user *cur,
				   size_t avail);
	static const fill_cap_fn fill_fns[] = {	/* [한국어] capability 를 만들어 내는 함수들의 표. 여기 적힌 순서대로 사슬이 만들어진다. */
		iommufd_fill_cap_dma_avail,	/* [한국어] 첫 번째 capability: 남은 매핑 개수. */
		iommufd_fill_cap_iova,	/* [한국어] 두 번째: 쓸 수 있는 IOVA 구간 목록. */
	};
	size_t minsz = offsetofend(struct vfio_iommu_type1_info, iova_pgsizes);	/* [한국어] 반드시 있어야 하는 고정 부분의 크기. */
	struct vfio_info_cap_header __user *last_cap = NULL;	/* [한국어] 바로 앞 capability 의 위치. 다음 것의 자리를 알아야 next 를 쓸 수 있어 한 바퀴 늦게 채운다. */
	struct vfio_iommu_type1_info info = {};	/* [한국어] 돌려줄 고정 부분. 남는 필드가 쓰레기가 되지 않게 0 으로. */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 주소 공간. */
	size_t total_cap_size;	/* [한국어] 고정 부분과 지금까지의 capability 를 합한 크기. */
	int rc;	/* [한국어] 결과 코드. */
	int i;	/* [한국어] 표 순회용 첨자. */

	if (copy_from_user(&info, arg, minsz))	/* [한국어] 사용자가 준 argsz 를 알아야 하므로 먼저 읽는다. */
		return -EFAULT;	/* [한국어] 사용자 주소 오류. */

	if (info.argsz < minsz)	/* [한국어] 고정 부분도 담지 못하는 버퍼는 거절. */
		return -EINVAL;	/* [한국어] 잘못된 인자. */
	minsz = min_t(size_t, info.argsz, sizeof(info));	/* [한국어] 되돌려 쓸 크기. 사용자 버퍼와 커널 구조체 중 작은 쪽까지만 쓴다 — 옛 사용자를 넘어 쓰면 안 된다. */

	ioas = get_compat_ioas(ictx);	/* [한국어] 호환 IOAS 를 참조와 함께 얻는다. */
	if (IS_ERR(ioas))	/* [한국어] 조회할 주소 공간이 없다. */
		return PTR_ERR(ioas);	/* [한국어] -ENODEV. */

	info.flags = VFIO_IOMMU_INFO_PGSIZES;	/* [한국어] 페이지 크기 필드가 유효함을 알린다. */
	info.iova_pgsizes = iommufd_get_pagesizes(ioas);	/* [한국어] 붙은 도메인들의 교집합. */
	info.cap_offset = 0;	/* [한국어] 기본은 capability 없음. 자리가 충분할 때만 아래에서 채운다. */

	down_read(&ioas->iopt.iova_rwsem);	/* [한국어] 예약 구간 트리를 읽는 동안 바뀌지 않게 한다. */
	total_cap_size = sizeof(info);	/* [한국어] 첫 capability 는 고정 부분 바로 뒤에 온다. */
	for (i = 0; i != ARRAY_SIZE(fill_fns); i++) {	/* [한국어] 표에 적힌 순서대로 사슬을 만든다. */
		int cap_size;	/* [한국어] 이번 capability 가 차지하는 크기. */

		if (info.argsz > total_cap_size)	/* [한국어] 아직 사용자 버퍼에 자리가 남았으면 */
			cap_size = fill_fns[i](ioas, arg + total_cap_size,	/* [한국어] 실제로 채우게 한다. */
					       info.argsz - total_cap_size);
		else
			cap_size = fill_fns[i](ioas, NULL, 0);	/* [한국어] 자리가 없으면 크기만 세게 한다 — 사용자에게 얼마가 필요한지 알려 주기 위해서다. */
		if (cap_size < 0) {	/* [한국어] 채우다 오류가 났다. */
			rc = cap_size;	/* [한국어] 그 코드를 결과로. */
			goto out_put;	/* [한국어] 정리하고 나간다. */
		}
		cap_size = ALIGN(cap_size, sizeof(u64));	/* [한국어] 다음 capability 를 8바이트 경계에 놓는다 — 정렬되지 않은 접근을 피하려는 ABI 규칙. */

		if (last_cap && info.argsz >= total_cap_size &&	/* [한국어] 앞 capability 가 있고 그 자리가 사용자 버퍼 안이라면 */
		    put_user(total_cap_size, &last_cap->next)) {	/* [한국어] 이제야 그 next 에 이번 것의 오프셋을 써 넣는다. */
			rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */
			goto out_put;	/* [한국어] 정리하고 나간다. */
		}
		last_cap = arg + total_cap_size;	/* [한국어] 다음 바퀴에서 next 를 채울 대상. */
		total_cap_size += cap_size;	/* [한국어] 누적 크기를 늘린다. */
	}

	/*
	 * If the user did not provide enough space then only some caps are
	 * returned and the argsz will be updated to the correct amount to get
	 * all caps.
	 */
	if (info.argsz >= total_cap_size)	/* [한국어] 전부 담을 수 있었다면 */
		info.cap_offset = sizeof(info);	/* [한국어] 첫 capability 의 위치를 알려 사슬을 따라가게 한다. */
	info.argsz = total_cap_size;	/* [한국어] 필요한(또는 실제로 쓴) 전체 크기. 모자랐다면 사용자가 이 값으로 다시 부른다. */
	info.flags |= VFIO_IOMMU_INFO_CAPS;	/* [한국어] capability 사슬이 있음을 알린다. */
	if (copy_to_user(arg, &info, minsz)) {	/* [한국어] 고정 부분을 되돌려 쓴다. */
		rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */
		goto out_put;	/* [한국어] 정리하고 나간다. */
	}
	rc = 0;	/* [한국어] 성공. */

out_put:	/* [한국어] 모든 경로가 지나는 정리 지점. */
	up_read(&ioas->iopt.iova_rwsem);	/* [한국어] 읽기 락 해제. */
	iommufd_put_object(ictx, &ioas->obj);	/* [한국어] IOAS 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_vfio_ioctl - 옛 VFIO ioctl 의 입구
 *
 * @ictx: 문맥.
 * @cmd: ioctl 번호.
 * @arg: 사용자 인자(번호에 따라 값이거나 포인터다).
 * @return: 각 처리기의 결과. 모르는 번호면 -ENOIOCTLCMD.
 *
 * vfio 코어가 컨테이너 fd 로 들어온 ioctl 중 iommu 관련한 것을 이리로
 * 넘긴다.
 *
 * -ENOIOCTLCMD 를 돌려주는 것이 요점이다 — 이것은 "내가 다룰 수 없다"는
 * 뜻이라 상위 계층이 다른 처리를 시도할 수 있다. -EINVAL 로 답하면
 * 사용자에게 그대로 실패로 보인다.
 *
 * VFIO_IOMMU_DIRTY_PAGES 를 default 와 함께 두어 명시적으로 거절한다 —
 * 옛 방식 더티 추적은 흉내 내지 않는다.
 */
int iommufd_vfio_ioctl(struct iommufd_ctx *ictx, unsigned int cmd,
		       unsigned long arg)
{
	void __user *uarg = (void __user *)arg;	/* [한국어] 번호에 따라 arg 가 값이거나 포인터다. 포인터로 쓸 경우를 위해 미리 변환해 둔다. */

	switch (cmd) {	/* [한국어] 옛 ioctl 번호로 나눈다. */
	case VFIO_GET_API_VERSION:	/* [한국어] 사용자가 ABI 판을 묻는다. */
		return VFIO_API_VERSION;	/* [한국어] 옛 VFIO 와 같은 값을 답해야 사용자가 계속 진행한다. */
	case VFIO_SET_IOMMU:	/* [한국어] IOMMU 모형 선택. arg 는 값이다. */
		return iommufd_vfio_set_iommu(ictx, arg);	/* [한국어] TYPE1 이면 큰 페이지를 금지한다. */
	case VFIO_CHECK_EXTENSION:	/* [한국어] 확장 지원 여부. arg 는 값이다. */
		return iommufd_vfio_check_extension(ictx, arg);	/* [한국어] 흉내 낼 수 없는 것은 0 을 답해 쓰지 않게 한다. */
	case VFIO_IOMMU_GET_INFO:	/* [한국어] 페이지 크기와 IOVA 구간 조회. arg 는 포인터다. */
		return iommufd_vfio_iommu_get_info(ictx, uarg);	/* [한국어] 가변 길이 capability 사슬을 채워 돌려준다. */
	case VFIO_IOMMU_MAP_DMA:	/* [한국어] 매핑 생성. */
		return iommufd_vfio_map_dma(ictx, cmd, uarg);	/* [한국어] iopt_map_user_pages 로 옮긴다. */
	case VFIO_IOMMU_UNMAP_DMA:	/* [한국어] 매핑 해제. */
		return iommufd_vfio_unmap_dma(ictx, cmd, uarg);	/* [한국어] 필요하면 경계에서 쪼갠 뒤 푼다. */
	case VFIO_IOMMU_DIRTY_PAGES:	/* [한국어] 옛 방식 더티 추적. 명시적으로 거절한다 — 새 인터페이스로 방향이 바뀌었다. */
	default:	/* [한국어] 모르는 번호. */
		return -ENOIOCTLCMD;	/* [한국어] "내가 다룰 수 없다"는 뜻이라 상위 계층이 다른 처리를 시도할 수 있다. */
	}
	return -ENOIOCTLCMD;	/* [한국어] switch 가 모든 경로에서 돌아가므로 닿지 않는 줄이다. 컴파일러 경고를 막는다. */
}
