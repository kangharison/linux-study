// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

/*
 * [한국어 설명] AMD IOMMU 를 iommufd 에 노출하는 얇은 계층 (iommufd.c)
 *
 * === 파일의 역할 ===
 * iommufd 는 사용자 공간(주로 VMM)이 IOMMU 를 직접 다루게 하는 인터페이스다.
 * 이 파일은 그 인터페이스가 요구하는 두 가지를 AMD 쪽에서 제공한다.
 *
 *  1) 하드웨어 정보 보고(amd_iommufd_hw_info): 사용자에게 이 IOMMU 의 능력
 *     비트(EFR/EFR2)를 그대로 알려 준다. VMM 은 그것을 보고 게스트에게
 *     어떤 기능을 보여 줄지 정한다.
 *  2) vIOMMU 객체의 생성과 파괴: 게스트가 자기 IOMMU 를 직접 다루는
 *     중첩 변환의 뼈대를 만든다.
 *
 * 실제 중첩 변환의 내용은 nested.c 가 맡고, 이 파일은 객체의 수명만
 * 관리한다 — 만들 때 부모 도메인에 매달고, 없앨 때 떼어 낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 코어 iommufd 와 AMD 드라이버 사이의 접합부다. 위로는 iommufd 가
 * amd_iommu_ops 를 통해 여기 함수들을 부르고, 아래로는 protection_domain 과
 * amd_iommu_viommu 를 다룬다.
 *
 * 실행 컨텍스트: 사용자의 ioctl 을 처리하는 프로세스 문맥. 다만 부모
 * 도메인의 lock 은 인터럽트를 끄고 잡아야 한다 — 그 락이 무효화 경로와
 * 공유되기 때문이다.
 *
 * 호출 체인:
 *   사용자 ioctl → iommufd 코어 → amd_iommu_ops.get_viommu_size/viommu_init
 *     → [이 파일] → nested.c 의 도메인 할당
 *
 * === 타 모듈과의 연결 ===
 * linux/iommufd.h 의 iommufd_viommu 와 그 ops, amd_iommu_types.h 의
 * amd_iommu_viommu / protection_domain.
 *
 * 데이터 흐름: 사용자가 만든 vIOMMU 객체 → 부모 도메인의 viommu_list 에
 * 등록 → 이후 그 도메인을 플러시할 때 게스트 도메인 id 까지 찾아낼 수 있게
 * 된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - amd_iommufd_hw_info(): EFR/EFR2 를 사용자에게 보고한다.
 * - amd_iommufd_get_viommu_size(): 코어가 잡아 줄 객체의 크기를 알린다.
 * - amd_iommufd_viommu_init(): vIOMMU 를 초기화하고 부모 도메인에 연결한다.
 * - amd_iommufd_viommu_destroy(): 그 연결을 끊고 게스트 id 표를 없앤다.
 * - amd_viommu_ops: 코어에 등록하는 콜백 표.
 */
#include <linux/iommu.h>	/* [한국어] 코어 IOMMU 타입 */

#include "iommufd.h"	/* [한국어] 이 파일이 제공하는 함수의 선언 */
#include "amd_iommu.h"	/* [한국어] to_pdomain 등 드라이버 내부 도우미 */
#include "amd_iommu_types.h"	/* [한국어] amd_iommu_viommu, protection_domain */

static const struct iommufd_viommu_ops amd_viommu_ops;	/* [한국어] 아래에서 정의되는 콜백 표의 전방 선언 — init 이 그것을 가리켜야 한다 */

/*
 * [한국어]
 * amd_iommufd_hw_info - 이 IOMMU 의 능력을 사용자 공간에 보고한다
 *
 * @dev: 대상 장치(어느 유닛인지는 쓰지 않는다 — 전역 능력을 보고한다).
 * @length: 돌려주는 구조체의 크기를 적어 줄 곳.
 * @type: 들어올 때는 사용자가 요청한 형식, 나갈 때는 실제 형식.
 * @return: 힙에 할당한 정보 구조체. 오류면 ERR_PTR.
 *
 * VMM 은 게스트에게 IOMMU 를 보여 주기 전에 "호스트 하드웨어가 무엇을 할 수
 * 있는가"를 알아야 한다. 게스트에게 없는 기능을 광고하면 게스트가 그것을
 * 쓰려다 실패하기 때문이다. 그 질문에 답하는 것이 이 함수다.
 *
 * type 을 양방향으로 쓰는 것이 이 인터페이스의 관례다. 사용자가 DEFAULT 로
 * 물으면 "네가 아무거나 골라라"라는 뜻이고, 그때 드라이버가 자기 형식을
 * 골라 적어 준다. AMD 형식을 명시적으로 요청한 경우도 받아들이며, 그 밖은
 * 이해할 수 없으므로 -EOPNOTSUPP 다.
 *
 * 전역 amd_iommu_efr 을 보고하는 이유: 드라이버가 모든 유닛의 공통분만
 * 쓰므로, 유닛별 값을 알려 주면 오히려 실제로 못 쓰는 기능을 광고하게 된다.
 *
 * 할당한 메모리의 소유권은 호출자(iommufd 코어)에게 넘어간다.
 *
 * 호출 체인:
 *   IOMMU_GET_HW_INFO ioctl → iommufd 코어 → [이 함수]
 */
void *amd_iommufd_hw_info(struct device *dev, u32 *length, enum iommu_hw_info_type *type)
{
	struct iommu_hw_info_amd *hwinfo;	/* [한국어] 사용자에게 돌려줄 정보 구조체 */

	if (*type != IOMMU_HW_INFO_TYPE_DEFAULT &&	/* [한국어] 사용자가 "아무거나"라고 했거나 */
	    *type != IOMMU_HW_INFO_TYPE_AMD)	/* [한국어] AMD 형식을 명시적으로 요청한 경우만 답할 수 있다 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 그 밖의 형식은 이해할 수 없다 */

	hwinfo = kzalloc_obj(*hwinfo);	/* [한국어] 소유권이 호출자에게 넘어가는 메모리 */
	if (!hwinfo)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 포인터 대신 오류를 실어 돌려준다 */

	*length = sizeof(*hwinfo);	/* [한국어] 사용자 버퍼에 복사할 크기 */
	*type = IOMMU_HW_INFO_TYPE_AMD;	/* [한국어] 실제로 채운 형식을 알려 준다 — DEFAULT 로 물었다면 이 값으로 답이 된다 */

	hwinfo->efr = amd_iommu_efr;	/* [한국어] 모든 유닛의 공통 능력. 유닛별 값을 주면 실제로 못 쓰는 기능을 광고하게 된다 */
	hwinfo->efr2 = amd_iommu_efr2;	/* [한국어] 두 번째 능력 워드도 같은 이유로 공통값 */

	return hwinfo;	/* [한국어] 코어가 사용자 공간으로 복사한 뒤 해제한다 */
}

/*
 * [한국어]
 * amd_iommufd_get_viommu_size - vIOMMU 객체를 얼마나 크게 잡을지 알린다
 *
 * @dev: 대상 장치.
 * @viommu_type: 사용자가 요청한 vIOMMU 종류.
 * @return: 할당할 바이트 수.
 *
 * 코어가 객체를 할당하고 드라이버는 그것을 초기화만 하는 구조다. 그런데
 * 드라이버가 코어 구조체(iommufd_viommu)를 자기 구조체 안에 품고 있으므로,
 * 코어는 얼마나 잡아야 할지 알 수 없다. 그래서 크기를 묻는 콜백이 따로 있다.
 *
 * VIOMMU_STRUCT_SIZE 매크로는 바깥 구조체의 크기를 돌려주면서, core 필드가
 * 실제로 그 안에 있는지도 컴파일 시점에 확인한다.
 *
 * 호출 체인:
 *   IOMMU_VIOMMU_ALLOC ioctl → iommufd 코어 → [이 함수]
 */
size_t amd_iommufd_get_viommu_size(struct device *dev, enum iommu_viommu_type viommu_type)
{
	return VIOMMU_STRUCT_SIZE(struct amd_iommu_viommu, core);	/* [한국어] 바깥 구조체의 크기. 매크로가 core 필드의 존재도 컴파일 시점에 확인한다 */
}

/*
 * [한국어]
 * amd_iommufd_viommu_init - vIOMMU 객체를 초기화하고 부모 도메인에 연결한다
 *
 * @viommu: 코어가 할당해 준 객체(우리 구조체 안에 박혀 있다).
 * @parent: 중첩 변환의 2단계 도메인이 될 부모.
 * @user_data: 사용자가 넘긴 추가 인자(여기서는 쓰지 않는다).
 * @return: 0 성공.
 *
 * 하는 일은 세 가지다.
 *  1) 게스트 도메인 id → 호스트 도메인 id 대응표를 빈 상태로 만든다.
 *     XA_FLAGS_ALLOC1 은 "id 를 1부터 할당하라"는 뜻으로, 0 을 "없음"의
 *     표식으로 쓸 수 있게 한다.
 *  2) 부모 도메인을 기억한다. 게스트가 만들 1단계 테이블이 가리키는 모든
 *     주소가 이 도메인을 한 번 더 거치므로, 이것이 곧 격리의 근거다.
 *  3) 부모의 viommu_list 에 자신을 매단다. 부모 도메인의 매핑이 바뀌면
 *     그 위에 얹힌 게스트 도메인들도 무효화해야 하는데, 그 역방향 탐색에
 *     이 목록이 쓰인다.
 *
 * 부모의 lock 을 irqsave 로 잡는 이유: 같은 락을 무효화 경로가 인터럽트를
 * 끈 채로 잡는다. 여기서 인터럽트를 열어 두면 락 순서가 어긋난다.
 *
 * 호출 체인:
 *   IOMMU_VIOMMU_ALLOC ioctl → iommufd 코어 → [이 함수]
 */
int amd_iommufd_viommu_init(struct iommufd_viommu *viommu, struct iommu_domain *parent,
			    const struct iommu_user_data *user_data)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	struct protection_domain *pdom = to_pdomain(parent);	/* [한국어] 부모 도메인을 AMD 구조체로 */
	struct amd_iommu_viommu *aviommu = container_of(viommu, struct amd_iommu_viommu, core);	/* [한국어] 코어가 잡아 준 객체를 감싸는 우리 구조체로 */

	xa_init_flags(&aviommu->gdomid_array, XA_FLAGS_ALLOC1);	/* [한국어] 게스트→호스트 도메인 id 표. 1부터 할당해 0 을 "없음"으로 쓸 수 있게 한다 */
	aviommu->parent = pdom;	/* [한국어] 중첩 변환의 2단계 도메인. 게스트가 무엇을 적든 이 도메인을 다시 거친다 */

	viommu->ops = &amd_viommu_ops;	/* [한국어] 코어가 이 객체를 다룰 콜백 표 */

	spin_lock_irqsave(&pdom->lock, flags);	/* [한국어] 무효화 경로와 공유하는 락이라 인터럽트를 끄고 잡는다 */
	list_add(&aviommu->pdom_list, &pdom->viommu_list);	/* [한국어] 부모의 매핑이 바뀔 때 이 vIOMMU 까지 찾아갈 수 있게 매단다 */
	spin_unlock_irqrestore(&pdom->lock, flags);	/* [한국어] 연결 완료 */

	return 0;	/* [한국어] 실패할 지점이 없다 */
}

/*
 * [한국어]
 * amd_iommufd_viommu_destroy - vIOMMU 를 부모에서 떼고 게스트 id 표를 없앤다
 *
 * @viommu: 없앨 객체.
 *
 * init 이 한 일을 역순으로 되돌린다. 목록에서 먼저 빼는 것이 중요하다 —
 * 그래야 이 시점 이후의 무효화가 사라질 객체를 훑지 않는다.
 *
 * xa_destroy 는 표에 남은 항목까지 함께 정리한다. 정상적인 경로라면 게스트
 * 도메인들이 먼저 없어져 표가 비어 있어야 하지만, 사용자가 순서를 지키지
 * 않아도 누수가 나지 않도록 한 것이다.
 *
 * 객체 자체는 코어가 놓는다 — 할당한 쪽이 해제한다는 규칙이다.
 *
 * 호출 체인:
 *   vIOMMU fd 를 닫을 때 → iommufd 코어 → [이 함수]
 */
static void amd_iommufd_viommu_destroy(struct iommufd_viommu *viommu)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	struct amd_iommu_viommu *aviommu = container_of(viommu, struct amd_iommu_viommu, core);	/* [한국어] 우리 구조체로 되짚는다 */
	struct protection_domain *pdom = aviommu->parent;	/* [한국어] 매달려 있던 부모 도메인 */

	spin_lock_irqsave(&pdom->lock, flags);	/* [한국어] 목록 조작을 무효화 경로로부터 보호 */
	list_del(&aviommu->pdom_list);	/* [한국어] 먼저 목록에서 뺀다 — 이후의 무효화가 사라질 객체를 훑지 않게 */
	spin_unlock_irqrestore(&pdom->lock, flags);	/* [한국어] 연결 해제 완료 */
	xa_destroy(&aviommu->gdomid_array);	/* [한국어] 남은 게스트 id 대응까지 정리한다. 사용자가 순서를 지키지 않아도 누수가 없게 */
}

/*
 * See include/linux/iommufd.h
 * struct iommufd_viommu_ops - vIOMMU specific operations
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct iommufd_viommu_ops amd_viommu_ops — 코어가 vIOMMU 를 다루는 콜백 표
 *
 * destroy 하나뿐인 것이 이 구현의 현재 범위를 말해 준다. 게스트 명령 큐
 * 처리나 게스트 무효화 전달 같은 확장 콜백은 아직 제공하지 않고, 중첩
 * 도메인의 생성은 별도 경로(amd_iommu_alloc_domain_nested)로 간다.
 */
static const struct iommufd_viommu_ops amd_viommu_ops = {
	.destroy = amd_iommufd_viommu_destroy,
	/* [한국어] vIOMMU 를 없앨 때 부르는 유일한 콜백.
	 * 게스트 명령 큐 처리나 무효화 전달 같은 확장 콜백을 아직 두지 않은 것이
	 *   이 구현의 현재 범위를 보여 준다. */
};
