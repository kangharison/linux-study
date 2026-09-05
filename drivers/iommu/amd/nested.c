// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

/*
 * [한국어 설명] 게스트가 자기 IOMMU 를 직접 다루는 중첩 변환 (nested.c)
 *
 * === 파일의 역할 ===
 * 게스트가 만든 1단계(stage-1) 변환 위에 호스트의 2단계(stage-2) 변환을
 * 얹는다. 게스트는 자기 DTE(gDTE)를 채워 넘기고, 호스트는 그것을 거의
 * 그대로 하드웨어 DTE 에 옮겨 담되 2단계 부분만 자기 것으로 바꾼다.
 *
 * 왜 게스트의 테이블 내용을 검사하지 않아도 되는가: 게스트가 적은 모든
 * 주소가 결국 호스트의 2단계 변환을 한 번 더 거친다. 게스트가 아무 주소나
 * 적어도 자기에게 할당된 메모리 밖으로는 나갈 수 없다. 이것이 중첩 변환이
 * 성립하는 근거이며, validate_gdte_nested 가 검사하는 것은 "위험한 값"이
 * 아니라 "하드웨어가 이해할 수 없는 값"이다.
 *
 * 이 파일에서 가장 미묘한 것은 도메인 id 다. 게스트는 자기만의 도메인 id
 * 공간을 쓰지만 하드웨어의 TLB 태그는 호스트 id 로 매겨진다. 그 대응을
 * 일관되게 유지하지 못하면 서로 다른 게스트 도메인이 같은 TLB 태그를
 * 공유해 엉뚱한 변환을 재사용한다 — alloc_domain_nested 안의 긴 영어 주석이
 * 설명하는 TLB 앨리어싱 문제다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iommufd.c 가 만든 vIOMMU 객체 아래에 놓인다. 사용자(VMM)가 게스트 DTE 를
 * 담아 hwpt 를 요청하면 이 파일이 중첩 도메인을 만들고, 장치를 붙일 때
 * 게스트 설정과 호스트 2단계 테이블을 합친 DTE 를 하드웨어에 쓴다.
 *
 * 실행 컨텍스트: 사용자 ioctl 의 프로세스 문맥. 게스트 id 대응표는 xarray
 * 의 내부 락으로 보호하며, 그 락 안에서 잠들 수 없어 할당 전략이 조금
 * 특이하다(gdom_info_load_or_alloc_locked).
 *
 * 호출 체인:
 *   IOMMU_HWPT_ALLOC(nested) → iommufd → amd_iommu_alloc_domain_nested()
 *   장치 attach → nested_attach_device() → set_dte_nested()
 *     → amd_iommu_update_dte()
 *
 * === 타 모듈과의 연결 ===
 * uapi/linux/iommufd.h 의 iommu_hwpt_amd_guest(사용자가 넘기는 게스트 DTE),
 * amd_iommu.h 의 DTE 조작 함수, 그리고 공용 페이지 테이블 API
 * (pt_iommu_amdv1_hw_info)로 부모 도메인의 2단계 정보를 얻는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - amd_iommu_alloc_domain_nested(): 게스트 DTE 를 받아 중첩 도메인을 만들고
 *   게스트 도메인 id 에 호스트 id 를 짝지어 준다.
 * - validate_gdte_nested(): 하드웨어가 이해할 수 없는 게스트 설정을 거른다.
 * - gdom_info_load_or_alloc_locked(): xarray 락을 든 채 항목을 얻거나 만든다.
 * - set_dte_nested(): 게스트 설정 + 호스트 2단계 테이블을 합쳐 DTE 를 짓는다.
 * - nested_domain_free(): 마지막 참조가 사라질 때 호스트 id 를 반납한다.
 */
#define dev_fmt(fmt)	"AMD-Vi: " fmt	/* [한국어] 이 파일의 로그 접두사 */

#include <linux/iommu.h>	/* [한국어] 코어 도메인 타입 */
#include <linux/refcount.h>	/* [한국어] 게스트 도메인 id 대응의 참조 계수 */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자가 넘기는 게스트 DTE 구조체 */

#include "amd_iommu.h"	/* [한국어] DTE 조작과 도메인 id 할당기 */

static const struct iommu_domain_ops nested_domain_ops;	/* [한국어] 아래에서 정의되는 콜백 표의 전방 선언 */

/*
 * [한국어]
 * to_ndomain - 코어의 iommu_domain 에서 중첩 도메인으로 되짚는다
 *
 * @dom: 코어가 넘긴 도메인.
 * @return: 그것을 품고 있는 struct nested_domain.
 *
 * 이 파일의 콜백들이 첫 줄에서 하는 변환이다. protection_domain 이 아니라
 * nested_domain 인 것에 유의 — 중첩 도메인은 페이지 테이블을 갖지 않고
 * 게스트 DTE 사본과 id 대응만 들고 있어, 별도의 구조체를 쓴다.
 */
static inline struct nested_domain *to_ndomain(struct iommu_domain *dom)
{
	return container_of(dom, struct nested_domain, domain);	/* [한국어] 중첩 도메인은 페이지 테이블이 없어 protection_domain 이 아닌 별도 구조체를 쓴다 */
}

/*
 * Validate guest DTE to make sure that configuration for host (v1)
 * and guest (v2) page tables are valid when allocating nested domain.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * validate_gdte_nested - 게스트가 넘긴 DTE 가 하드웨어에 쓸 수 있는 값인지 본다
 *
 * @gdte: 사용자가 넘긴 게스트 DTE.
 * @return: 0 이면 쓸 수 있다. -EINVAL 이면 값이 잘못됐고, -EOPNOTSUPP 이면
 *          호스트 하드웨어가 그 설정을 감당하지 못한다.
 *
 * 여기서 검사하는 것은 보안이 아니다. 게스트가 어떤 주소를 적든 호스트의
 * 2단계 변환이 막아 주므로, 위험한 값이라는 개념이 없다. 대신 "하드웨어가
 * 해석할 수 없거나 호스트 설정과 모순되는 값"을 걸러 낸다.
 *
 * 네 가지를 본다.
 *  - Mode 와 Host-TPR 이 0 이어야 한다: 그 자리는 호스트의 2단계 테이블이
 *    차지할 곳이다. 게스트가 값을 적어 두면 set_dte_nested 가 덮어쓸 때
 *    의도가 충돌한다.
 *  - V 와 GV 를 세웠다면 GCR3 주소가 0 이 아니어야 한다: 게스트 변환을
 *    쓰겠다면서 그 테이블의 주소를 주지 않은 모순이다.
 *  - 게스트 페이징 모드는 4단계 또는 5단계뿐이다: 다른 값은 스펙에 없다.
 *  - GLX == 3 은 예약값이다.
 *
 * 마지막 검사만 성격이 다르다. 게스트가 5단계를 쓰겠다는데 호스트 하드웨어가
 * 4단계까지만 지원하면 그것은 게스트의 잘못이 아니라 이 기계에서 안 되는
 * 것이므로 -EOPNOTSUPP 으로 구별해 답한다.
 *
 * 호출 체인:
 *   amd_iommu_alloc_domain_nested() → [이 함수]
 */
static int validate_gdte_nested(struct iommu_hwpt_amd_guest *gdte)
{
	u32 gpt_level = FIELD_GET(DTE_GPT_LEVEL_MASK, gdte->dte[2]);	/* [한국어] 게스트가 쓰겠다는 페이징 단계 수 */

	/* Must be zero: Mode, Host-TPR */
	if (FIELD_GET(DTE_MODE_MASK, gdte->dte[0]) != 0 ||	/* [한국어] (원 주석: Mode 와 Host-TPR 은 0 이어야 한다) */
	    FIELD_GET(DTE_HOST_TRP, gdte->dte[0]) != 0)	/* [한국어] 그 자리는 호스트의 2단계 테이블이 차지할 곳이라 게스트 값이 있으면 충돌한다 */
		return -EINVAL;	/* [한국어] 잘못된 설정 */

	/* GCR3 TRP must be non-zero if V, GV is set */
	if (FIELD_GET(DTE_FLAG_V, gdte->dte[0]) == 1 &&	/* [한국어] (원 주석: V 와 GV 를 세웠다면 GCR3 주소가 0 이 아니어야 한다) */
	    FIELD_GET(DTE_FLAG_GV, gdte->dte[0]) == 1 &&	/* [한국어] 게스트 변환을 쓰겠다고 했는데 */
	    FIELD_GET(DTE_GCR3_14_12, gdte->dte[0]) == 0 &&	/* [한국어] GCR3 주소의 세 조각이 */
	    FIELD_GET(DTE_GCR3_30_15, gdte->dte[1]) == 0 &&	/* [한국어] 모두 0 이면 */
	    FIELD_GET(DTE_GCR3_51_31, gdte->dte[1]) == 0)	/* [한국어] 테이블 주소를 주지 않은 모순이다 */
		return -EINVAL;	/* [한국어] 거절 */

	/* Valid Guest Paging Mode values are 0 and 1 */
	if (gpt_level != GUEST_PGTABLE_4_LEVEL &&	/* [한국어] (원 주석: 유효한 게스트 페이징 모드는 0 과 1 뿐이다) */
	    gpt_level != GUEST_PGTABLE_5_LEVEL)	/* [한국어] 다른 값은 스펙에 없다 */
		return -EINVAL;	/* [한국어] 거절 */

	/* GLX = 3 is reserved */
	if (FIELD_GET(DTE_GLX, gdte->dte[0]) == 3)	/* [한국어] (원 주석: GLX = 3 은 예약값) */
		return -EINVAL;	/* [한국어] 거절 */

	/*
	 * We need to check host capability before setting
	 * the Guest Paging Mode
	 */
	if (gpt_level == GUEST_PGTABLE_5_LEVEL &&	/* [한국어] (원 주석: 게스트 페이징 모드를 설정하기 전에 호스트 능력을 확인해야 한다) */
	    amd_iommu_gpt_level < PAGE_MODE_5_LEVEL)	/* [한국어] 게스트가 5단계를 원하는데 호스트 하드웨어가 못 한다면 */
		return -EOPNOTSUPP;	/* [한국어] 게스트의 잘못이 아니라 이 기계에서 안 되는 것이라 다른 코드로 구별한다 */

	return 0;	/* [한국어] 하드웨어에 쓸 수 있는 설정 */
}

/*
 * [한국어]
 * gdom_info_load_or_alloc_locked - xarray 락을 든 채 항목을 얻거나 새로 만든다
 *
 * @xa: 게스트 도메인 id 대응표. 호출자가 xa_lock 을 들고 들어온다.
 * @index: 게스트 도메인 id.
 * @return: 그 id 의 항목. 오류면 ERR_PTR.
 *
 * 이름의 _locked 가 계약이다: 들어올 때도 나갈 때도 락을 든 상태여야 한다.
 * 그런데 중간에 메모리를 할당해야 하고, 그 할당은 잠들 수 있어 락 안에서
 * 할 수 없다.
 *
 * 그래서 락을 잠깐 놓고 할당한 뒤 다시 잡는다. 그 사이 다른 스레드가 같은
 * id 로 항목을 만들었을 수 있으므로, __xa_cmpxchg 로 "아직 비어 있을 때만
 * 넣는다"를 원자적으로 시도한다. 이미 다른 항목이 들어와 있으면 우리가
 * 만든 것을 버리고 그쪽을 쓴다.
 *
 * 이 패턴이 없으면 두 스레드가 같은 게스트 도메인에 서로 다른 호스트 id 를
 * 부여해, 같은 게스트 도메인의 장치들이 다른 TLB 태그를 쓰게 된다.
 *
 * 새로 만든 항목의 users 를 0 으로 두는 이유: 호출자가
 * refcount_inc_not_zero 로 "기존 항목인가 새 항목인가"를 구별한다.
 *
 * 호출 체인:
 *   amd_iommu_alloc_domain_nested() → [이 함수]
 */
static void *gdom_info_load_or_alloc_locked(struct xarray *xa, unsigned long index)
{
	struct guest_domain_mapping_info *elm, *res;	/* [한국어] 우리가 만들 항목과, 경쟁에서 이긴 항목 */

	elm = xa_load(xa, index);	/* [한국어] 이미 대응이 있는가 */
	if (elm)	/* [한국어] 있으면 */
		return elm;	/* [한국어] 그것을 쓴다 */

	xa_unlock(xa);	/* [한국어] 할당은 잠들 수 있어 락 안에서 할 수 없다 */
	elm = kzalloc_obj(struct guest_domain_mapping_info);	/* [한국어] 락 밖에서 만든다 */
	xa_lock(xa);	/* [한국어] 계약대로 다시 잡는다 */
	if (!elm)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 락을 든 채로 오류를 돌려준다 */

	res = __xa_cmpxchg(xa, index, NULL, elm, GFP_KERNEL);	/* [한국어] 아직 비어 있을 때만 넣는다 — 락을 놓은 사이 다른 스레드가 만들었을 수 있다 */
	if (xa_is_err(res))	/* [한국어] 표 자체의 오류 */
		res = ERR_PTR(xa_err(res));	/* [한국어] 오류 포인터로 바꿔 전달 */

	if (res) {	/* [한국어] 이미 다른 항목이 들어와 있었다 */
		kfree(elm);	/* [한국어] 우리가 만든 것을 버리고 */
		return res;	/* [한국어] 먼저 들어온 쪽을 쓴다. 이렇게 해야 하나의 게스트 도메인이 하나의 호스트 id 를 갖는다 */
	}

	refcount_set(&elm->users, 0);	/* [한국어] 호출자가 inc_not_zero 로 "새 항목인가"를 구별할 수 있게 0 으로 둔다 */
	return elm;	/* [한국어] 새로 만든 빈 항목 */
}

/*
 * This function is assigned to struct iommufd_viommu_ops.alloc_domain_nested()
 * during the call to struct iommu_ops.viommu_init().
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_alloc_domain_nested - 게스트 DTE 를 받아 중첩 도메인을 만든다
 *
 * @viommu: 이 도메인이 속할 vIOMMU.
 * @flags: 사용자가 준 플래그(여기서는 쓰지 않는다).
 * @user_data: 게스트 DTE 를 담은 사용자 데이터.
 * @return: 새 도메인. 실패하면 ERR_PTR.
 *
 * 하는 일은 두 갈래다.
 *
 * 앞쪽은 단순하다: 사용자 데이터를 복사하고, 검증하고, 도메인 구조체를
 * 채운다.
 *
 * 뒤쪽이 이 파일의 핵심이며, 함수 안의 긴 영어 주석이 그 이유를 설명한다.
 * 게스트는 자기 알고리즘으로 gDomID 를 붙이지만 하드웨어의 TLB 태그는
 * (DomID, PASID) 쌍으로 매겨진다. 한 vIOMMU 안의 모든 DTE 가 같은 2단계
 * 테이블을 쓰므로, 게스트가 서로 다른 1단계 테이블에 같은 PASID 를 쓰면
 * 하드웨어는 그것들을 같은 태그로 보게 된다 — 서로 다른 주소 공간의 변환이
 * 섞이는 TLB 앨리어싱이다.
 *
 * 막는 방법은 gDomID 하나에 hDomID 하나를 일관되게 짝지어 주는 것이다.
 * xarray 가 그 대응을 들고 있고, 같은 gDomID 로 도메인을 여러 번 만들면
 * 같은 hDomID 를 재사용한다(참조 계수로 관리).
 *
 * refcount_inc_not_zero 가 갈림길이다. 성공하면 이미 있는 대응을 얻은
 * 것이고, 실패(0)하면 방금 만들어진 빈 항목이라 새 hDomID 를 발급해야 한다.
 *
 * 실행 컨텍스트: 사용자 ioctl. xa_lock 안에서는 잠들 수 없어 GFP_ATOMIC 을
 * 쓴다.
 *
 * 호출 체인:
 *   IOMMU_HWPT_ALLOC(nested) → iommufd → [이 함수]
 *     → validate_gdte_nested() → gdom_info_load_or_alloc_locked()
 *     → amd_iommu_pdom_id_alloc()
 */
struct iommu_domain *
amd_iommu_alloc_domain_nested(struct iommufd_viommu *viommu, u32 flags,
			      const struct iommu_user_data *user_data)
{
	int ret;	/* [한국어] 오류 코드 */
	struct nested_domain *ndom;	/* [한국어] 만들 중첩 도메인 */
	struct guest_domain_mapping_info *gdom_info;	/* [한국어] 게스트 id → 호스트 id 대응 */
	struct amd_iommu_viommu *aviommu = container_of(viommu, struct amd_iommu_viommu, core);	/* [한국어] 우리 vIOMMU 구조체로 */

	if (user_data->type != IOMMU_HWPT_DATA_AMD_GUEST)	/* [한국어] AMD 게스트 DTE 형식이 아니면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 해석할 수 없다 */

	ndom = kzalloc_obj(*ndom);	/* [한국어] 도메인 구조체 */
	if (!ndom)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 되돌릴 것이 없다 */

	ret = iommu_copy_struct_from_user(&ndom->gdte, user_data,	/* [한국어] 사용자 공간의 게스트 DTE 를 복사한다 */
					  IOMMU_HWPT_DATA_AMD_GUEST,	/* [한국어] 형식을 명시해 크기 검증까지 맡긴다 */
					  dte);	/* [한국어] 복사할 필드 */
	if (ret)	/* [한국어] 복사 실패(잘못된 포인터나 크기) */
		goto out_err;	/* [한국어] 도메인을 놓고 나간다 */

	ret = validate_gdte_nested(&ndom->gdte);	/* [한국어] 하드웨어에 쓸 수 있는 값인지 */
	if (ret)	/* [한국어] 아니면 */
		goto out_err;	/* [한국어] 거절 */

	ndom->gdom_id = FIELD_GET(DTE_DOMID_MASK, ndom->gdte.dte[1]);	/* [한국어] 게스트가 부여한 도메인 id */
	ndom->domain.ops = &nested_domain_ops;	/* [한국어] map/unmap 이 없는 중첩 전용 콜백 표 */
	ndom->domain.type = IOMMU_DOMAIN_NESTED;	/* [한국어] 코어가 이것을 매핑 가능한 도메인으로 다루지 않게 */
	ndom->viommu = aviommu;	/* [한국어] 부모 도메인과 id 대응표에 이것을 통해 도달한다 */

	/*
	 * Normally, when a guest has multiple pass-through devices,
	 * the IOMMU driver setup DTEs with the same stage-2 table and
	 * use the same host domain ID (hDomId). In case of nested translation,
	 * if the guest setup different stage-1 tables with same PASID,
	 * IOMMU would use the same TLB tag. This will results in TLB
	 * aliasing issue.
	 *
	 * The guest is assigning gDomIDs based on its own algorithm for managing
	 * cache tags of (DomID, PASID). Within a single viommu, the nest parent domain
	 * (w/ S2 table) is used by all DTEs. But we need to consistently map the gDomID
	 * to a single hDomID. This is done using an xarray in the vIOMMU to
	 * keep track of the gDomID mapping. When the S2 is changed, the INVALIDATE_IOMMU_PAGES
	 * command must be issued for each hDomID in the xarray.
	 */
	xa_lock(&aviommu->gdomid_array);	/* [한국어] 대응표를 조작하는 동안 */

	gdom_info = gdom_info_load_or_alloc_locked(&aviommu->gdomid_array, ndom->gdom_id);	/* [한국어] 기존 대응을 얻거나 빈 항목을 만든다 */
	if (IS_ERR(gdom_info)) {	/* [한국어] 실패 */
		xa_unlock(&aviommu->gdomid_array);	/* [한국어] 락을 풀고 */
		ret = PTR_ERR(gdom_info);	/* [한국어] 오류를 꺼내 */
		goto out_err;	/* [한국어] 도메인을 놓는다 */
	}

	/* Check if gDomID exist */
	if (refcount_inc_not_zero(&gdom_info->users)) {	/* [한국어] (원 주석: gDomID 가 이미 존재하는가) — 0 이 아니면 기존 대응이다 */
		ndom->gdom_info = gdom_info;	/* [한국어] 그 대응을 쓴다 */
		xa_unlock(&aviommu->gdomid_array);	/* [한국어] 락 해제 */

		pr_debug("%s: Found gdom_id=%#x, hdom_id=%#x\n",	/* [한국어] 같은 게스트 도메인의 장치들이 같은 TLB 태그를 공유하게 된다 */
			  __func__, ndom->gdom_id, gdom_info->hdom_id);

		return &ndom->domain;	/* [한국어] 재사용 성공 */
	}

	/* The gDomID does not exist. We allocate new hdom_id */
	gdom_info->hdom_id = amd_iommu_pdom_id_alloc();	/* [한국어] (원 주석: gDomID 가 없으므로 새 hdom_id 를 할당한다) */
	if (gdom_info->hdom_id <= 0) {	/* [한국어] 도메인 id 공간이 고갈됐다 */
		__xa_cmpxchg(&aviommu->gdomid_array,	/* [한국어] 방금 넣은 빈 항목을 */
			     ndom->gdom_id, gdom_info, NULL, GFP_ATOMIC);	/* [한국어] 표에서 도로 빼낸다 — 락 안이라 GFP_ATOMIC */
		xa_unlock(&aviommu->gdomid_array);	/* [한국어] 락 해제 */
		ret = -ENOSPC;	/* [한국어] id 가 없다 */
		goto out_err_gdom_info;	/* [한국어] 항목과 도메인을 모두 놓는다 */
	}

	ndom->gdom_info = gdom_info;	/* [한국어] 새로 만든 대응을 쓴다 */
	refcount_set(&gdom_info->users, 1);	/* [한국어] 첫 참조. 이후 같은 게스트 id 의 도메인들이 이 값을 늘린다 */

	xa_unlock(&aviommu->gdomid_array);	/* [한국어] 대응이 완성됐다. 이제 다른 스레드가 같은 게스트 id 로 이 항목을 재사용한다 */

	pr_debug("%s: Allocate gdom_id=%#x, hdom_id=%#x\n",	/* [한국어] 새 대응이 생겼음을 남긴다 */
		 __func__, ndom->gdom_id, gdom_info->hdom_id);

	return &ndom->domain;	/* [한국어] 생성 성공 */

out_err_gdom_info:	/* [한국어] 도메인 id 를 얻지 못한 경우 — 표에서 뺀 항목까지 놓는다 */
	kfree(gdom_info);	/* [한국어] 표에서 뺀 항목을 놓는다 */
out_err:	/* [한국어] 그보다 앞에서 실패한 경우 — 도메인 구조체만 놓으면 된다 */
	kfree(ndom);	/* [한국어] 도메인 구조체를 놓고 */
	return ERR_PTR(ret);	/* [한국어] 오류를 돌려준다 */
}

/*
 * [한국어]
 * set_dte_nested - 게스트 설정과 호스트 2단계 테이블을 합쳐 DTE 를 짓는다
 *
 * @iommu: 담당 유닛(여기서는 쓰지 않는다).
 * @dom: 중첩 도메인.
 * @dev_data: 붙일 장치.
 * @new: 채울 DTE 사본.
 *
 * 중첩 변환이 하드웨어에서 어떻게 표현되는지가 이 함수에 그대로 드러난다.
 * 하나의 DTE 에 두 단계가 함께 들어간다:
 *  - 2단계(호스트): amd_iommu_set_dte_v1 이 부모 도메인의 페이지 테이블
 *    주소와 레벨을 채운다. 게스트는 이 부분에 손댈 수 없다.
 *  - 1단계(게스트): GCR3 주소, GLX, 페이징 모드, PPR 설정을 게스트 DTE 에서
 *    그대로 OR 해 온다.
 *
 * 게스트 값을 검사 없이 옮기는 것이 안전한 이유는 다시 한번 2단계다 —
 * GCR3 주소조차 2단계 변환을 거쳐 해석되므로, 게스트가 아무 값이나 적어도
 * 자기 메모리 밖을 가리킬 수 없다.
 *
 * 도메인 id 로 gdom_info->hdom_id 를 쓰는 것이 앞서 만든 대응의 결실이다.
 * 같은 게스트 도메인에 속한 장치들은 모두 같은 hDomID 를 받아 TLB 태그를
 * 공유하고, 다른 게스트 도메인과는 갈린다.
 *
 * GV 를 강제로 세우는 이유: 중첩 변환은 정의상 게스트 변환을 쓰므로, 게스트
 * DTE 가 그것을 빠뜨렸더라도 하드웨어에는 반드시 서 있어야 한다.
 *
 * 호출 체인:
 *   nested_attach_device() → [이 함수] → amd_iommu_make_clear_dte()
 *     → pt_iommu_amdv1_hw_info() → amd_iommu_set_dte_v1()
 */
static void set_dte_nested(struct amd_iommu *iommu, struct iommu_domain *dom,
			   struct iommu_dev_data *dev_data, struct dev_table_entry *new)
{
	struct protection_domain *parent;	/* [한국어] 호스트의 2단계 도메인 */
	struct nested_domain *ndom = to_ndomain(dom);	/* [한국어] 중첩 도메인으로 */
	struct iommu_hwpt_amd_guest *gdte = &ndom->gdte;	/* [한국어] 게스트가 채운 DTE 사본 */
	struct pt_iommu_amdv1_hw_info pt_info;	/* [한국어] 부모 도메인의 페이지 테이블 정보를 받을 곳 */

	/*
	 * The nest parent domain is attached during the call to the
	 * struct iommu_ops.viommu_init(), which will be stored as part
	 * of the struct amd_iommu_viommu.parent.
	 */
	if (WARN_ON(!ndom->viommu || !ndom->viommu->parent))	/* [한국어] (원 주석: 부모 도메인은 viommu_init 때 붙여 둔다) */
		return;	/* [한국어] 없다면 배선이 잘못된 것이라 아무것도 하지 않는다 */

	parent = ndom->viommu->parent;	/* [한국어] 2단계 변환을 담당할 도메인 */
	amd_iommu_make_clear_dte(dev_data, new);	/* [한국어] V 만 세우고 IVRS 지정 비트를 되살린 초기 상태에서 시작한다 */

	/* Retrieve the current pagetable info via the IOMMU PT API. */
	pt_iommu_amdv1_hw_info(&parent->amdv1, &pt_info);	/* [한국어] (원 주석: 공용 페이지 테이블 API 로 현재 테이블 정보를 얻는다) */

	/*
	 * Use domain ID from nested domain to program DTE.
	 * See amd_iommu_alloc_domain_nested().
	 */
	amd_iommu_set_dte_v1(dev_data, parent, ndom->gdom_info->hdom_id,	/* [한국어] (원 주석: 중첩 도메인의 도메인 id 로 DTE 를 설정한다) — 앞서 만든 대응의 결실이다 */
			     &pt_info, new);	/* [한국어] 2단계 테이블 주소와 레벨이 여기서 채워진다. 게스트는 이 부분에 손댈 수 없다 */

	/* GV is required for nested page table */
	new->data[0] |= DTE_FLAG_GV;	/* [한국어] (원 주석: 중첩 페이지 테이블에는 GV 가 필수) — 게스트가 빠뜨렸어도 세운다 */

	/* Guest PPR */
	new->data[0] |= gdte->dte[0] & DTE_FLAG_PPR;	/* [한국어] (원 주석: 게스트 PPR) 게스트가 페이지 요청을 쓸지 */

	/* Guest translation stuff */
	new->data[0] |= gdte->dte[0] & (DTE_GLX | DTE_FLAG_GIOV);	/* [한국어] (원 주석: 게스트 변환 설정) GCR3 레벨 수와 I/O 가상 주소 모드 */

	/* GCR3 table */
	new->data[0] |= gdte->dte[0] & DTE_GCR3_14_12;	/* [한국어] (원 주석: GCR3 테이블) 주소의 첫 조각 */
	new->data[1] |= gdte->dte[1] & (DTE_GCR3_30_15 | DTE_GCR3_51_31);	/* [한국어] 나머지 두 조각. 이 주소조차 2단계를 거쳐 해석되므로 검사 없이 옮겨도 안전하다 */

	/* Guest paging mode */
	new->data[2] |= gdte->dte[2] & DTE_GPT_LEVEL_MASK;	/* [한국어] (원 주석: 게스트 페이징 모드) 4단계인지 5단계인지 */
}

/*
 * [한국어]
 * nested_attach_device - 장치를 중첩 도메인에 붙인다
 *
 * @dom: 중첩 도메인.
 * @dev: 붙일 장치.
 * @old: 이전 도메인(여기서는 쓰지 않는다).
 * @return: 0 성공, -EINVAL 이면 PASID 가 켜진 장치다.
 *
 * PASID 가 켜진 장치를 거부하는 이유가 이 경로의 제약이다. 중첩 변환에서는
 * PASID 공간을 게스트가 관리한다 — 게스트의 GCR3 테이블이 그 대응을 담는다.
 * 호스트가 같은 장치에 자기 PASID 를 설정해 두었다면 두 관리자가 같은
 * 자원을 다투게 되므로, 그 조합을 아예 막는다.
 *
 * WARN_ON 인 이유: 상위 계층이 이미 걸러야 하는 조건이라, 여기까지 왔다는
 * 것은 배선이 잘못됐다는 뜻이다.
 *
 * dev_data->mutex 를 잡는 것은 DTE 갱신이 붙이고 떼는 다른 경로와 겹치지
 * 않게 하기 위해서다.
 *
 * 호출 체인:
 *   장치 attach → 코어 → [이 함수] → set_dte_nested() → amd_iommu_update_dte()
 */
static int nested_attach_device(struct iommu_domain *dom, struct device *dev,
				struct iommu_domain *old)
{
	struct dev_table_entry new = {0};	/* [한국어] 조립할 DTE */
	struct iommu_dev_data *dev_data = dev_iommu_priv_get(dev);	/* [한국어] 장치의 IOMMU 상태 */
	struct amd_iommu *iommu = get_amd_iommu_from_dev_data(dev_data);	/* [한국어] 담당 유닛 */
	int ret = 0;	/* [한국어] 반환값 */

	/*
	 * Needs to make sure PASID is not enabled
	 * for this attach path.
	 */
	if (WARN_ON(dev_data->pasid_enabled))	/* [한국어] (원 주석: 이 경로에서는 PASID 가 켜져 있으면 안 된다) */
		return -EINVAL;	/* [한국어] 중첩에서는 PASID 공간을 게스트가 관리하므로 호스트 설정과 다툰다 */

	mutex_lock(&dev_data->mutex);	/* [한국어] DTE 갱신이 다른 붙이기/떼기와 겹치지 않게 */

	set_dte_nested(iommu, dom, dev_data, &new);	/* [한국어] 게스트 설정 + 호스트 2단계를 합쳐 DTE 를 짓는다 */

	amd_iommu_update_dte(iommu, dev_data, &new);	/* [한국어] 실제 테이블에 원자적으로 반영하고 캐시를 무효화한다 */

	mutex_unlock(&dev_data->mutex);	/* [한국어] 완료 */

	return ret;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * nested_domain_free - 중첩 도메인을 없애고 마지막이면 호스트 id 를 반납한다
 *
 * @dom: 없앨 도메인.
 *
 * 참조 계수가 이 함수의 전부다. 같은 게스트 도메인 id 로 만들어진 도메인이
 * 여럿 있을 수 있고, 그들이 하나의 hDomID 를 공유한다. 마지막 하나가
 * 사라질 때에만 그 대응을 표에서 빼고 hDomID 를 반납해야 한다 — 먼저
 * 반납하면 아직 살아 있는 도메인의 장치들이 다른 도메인에 재발급된 id 를
 * 쓰게 되어, 앞서 막으려던 TLB 앨리어싱이 그대로 일어난다.
 *
 * refcount_dec_and_test 가 거짓이면 아직 다른 참조가 있다는 뜻이라 도메인
 * 구조체만 남기고 돌아간다.
 *
 * __xa_cmpxchg 로 "우리가 아는 그 항목일 때만 지운다"를 쓰는 이유: 락을
 * 든 상태이긴 하지만, 값이 예상과 다르면 어딘가에서 상태가 어긋난 것이므로
 * WARN 으로 드러낸다.
 *
 * 호출 체인:
 *   hwpt 를 닫을 때 → 코어 → [이 함수] → amd_iommu_pdom_id_free()
 */
static void nested_domain_free(struct iommu_domain *dom)
{
	struct guest_domain_mapping_info *curr;	/* [한국어] 표에서 실제로 빠진 항목 */
	struct nested_domain *ndom = to_ndomain(dom);	/* [한국어] 중첩 도메인으로 */
	struct amd_iommu_viommu *aviommu = ndom->viommu;	/* [한국어] 대응표를 가진 vIOMMU */

	xa_lock(&aviommu->gdomid_array);	/* [한국어] 참조 계수와 표를 함께 다루므로 락 안에서 */

	if (!refcount_dec_and_test(&ndom->gdom_info->users)) {	/* [한국어] 아직 다른 도메인이 이 대응을 쓰고 있는가 */
		xa_unlock(&aviommu->gdomid_array);	/* [한국어] 그렇다면 */
		return;	/* [한국어] hDomID 를 반납하면 안 된다 — 반납하면 살아 있는 장치들이 재발급된 id 를 쓰게 된다 */
	}

	/*
	 * The refcount for the gdom_id to hdom_id mapping is zero.
	 * It is now safe to remove the mapping.
	 */
	curr = __xa_cmpxchg(&aviommu->gdomid_array, ndom->gdom_id,	/* [한국어] (원 주석: 참조가 0 이므로 이제 대응을 지워도 안전하다) */
			    ndom->gdom_info, NULL, GFP_ATOMIC);	/* [한국어] 우리가 아는 항목일 때만 지운다 */

	xa_unlock(&aviommu->gdomid_array);	/* [한국어] 표 조작 끝 */
	if (WARN_ON(!curr || xa_err(curr)))	/* [한국어] 값이 예상과 다르면 어딘가에서 상태가 어긋난 것 */
		return;	/* [한국어] 드러내고 더 진행하지 않는다 */

	/* success */
	pr_debug("%s: Free gdom_id=%#x, hdom_id=%#x\n",	/* [한국어] (원 주석: 성공) */
		__func__, ndom->gdom_id, curr->hdom_id);

	amd_iommu_pdom_id_free(ndom->gdom_info->hdom_id);	/* [한국어] 이제 호스트 도메인 id 를 재사용할 수 있다 */
	kfree(curr);	/* [한국어] 대응 항목을 놓고 */
	kfree(ndom);	/* [한국어] 도메인 구조체도 놓는다 */
}

/*
 * [한국어] struct iommu_domain_ops nested_domain_ops — 중첩 도메인의 콜백 표
 *
 * 둘뿐인 것이 이 도메인의 성격을 말해 준다. map/unmap 이 없다 — 매핑은
 * 게스트가 자기 1단계 테이블에 직접 만들고, 호스트의 2단계 매핑은 부모
 * 도메인이 관리한다. 이 도메인 자체는 "게스트 설정을 DTE 에 옮겨 담는
 * 창구"일 뿐이라 붙이고 놓는 일만 한다.
 */
static const struct iommu_domain_ops nested_domain_ops = {
	.attach_dev = nested_attach_device,
	/* [한국어] 장치를 이 중첩 도메인에 붙인다 — 게스트 설정을 DTE 에 옮겨 담는다. */
	.free = nested_domain_free,
	/* [한국어] 도메인을 놓고, 마지막 참조였다면 호스트 도메인 id 도 반납한다. */
};
