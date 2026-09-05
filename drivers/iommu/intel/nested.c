// SPDX-License-Identifier: GPL-2.0
/*
 * nested.c - nested mode translation support
 *
 * Copyright (C) 2023 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *         Jacob Pan <jacob.jun.pan@linux.intel.com>
 *         Yi Liu <yi.l.liu@intel.com>
 */

/*
 * [한국어 설명] 중첩 변환(2단계 번역) 지원 — 게스트 페이지 테이블 위의 호스트 페이지 테이블 (intel/nested.c)
 *
 * === 파일의 역할 ===
 * 가상 머신에 장치를 직접 넘길 때, 게스트도 자기 IOMMU 를 갖고 있다고 믿고
 * 자기 페이지 테이블을 만든다. 그 테이블을 호스트가 신뢰할 수는 없다 —
 * 게스트가 아무 호스트 물리 주소나 적을 수 있기 때문이다.
 * 중첩 변환은 하드웨어가 두 단계를 연달아 밟게 해서 그 문제를 푼다.
 *   게스트 DMA → (게스트의 1단계 테이블) → 게스트 물리 주소
 *              → (호스트의 2단계 테이블) → 호스트 물리 주소
 * 게스트가 1단계에 무엇을 적든 그 결과는 다시 2단계를 거치므로, 호스트가
 * 허용한 범위를 벗어날 수 없다. 그래서 호스트는 게스트 테이블을 파싱하지
 * 않고 주소만 하드웨어에 넘긴다.
 * 이 파일은 그 "1단계 도메인"을 만들고, 부모(2단계) 도메인에 매달고, 게스트가
 * 요청하는 무효화를 대신 수행한다. 실제 하드웨어 항목 설정은 pasid.c 의
 * intel_pasid_setup_nested 가 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 도메인이 부모-자식으로 짝을 이루는 유일한 곳이다.
 *   부모(s2_domain) — 호스트가 소유하는 2단계 페이지 테이블. VFIO/iommufd 가
 *                     게스트 물리 → 호스트 물리 매핑을 여기에 만든다.
 *                     nested_parent 로 표시되어 있어야 한다.
 *   자식(이 파일)   — 게스트가 소유하는 1단계 테이블을 가리키기만 한다.
 *                     페이지 테이블을 만들지 않고 주소만 들고 있다.
 * 위쪽으로는 iommufd 가 유저스페이스(VMM)의 요청을 이 파일로 넘기고,
 * 아래쪽으로는 pasid.c 가 항목을 세우고 cache.c 가 무효화를 보낸다.
 * 실행 컨텍스트: 커널 모듈, 프로세스 컨텍스트. 모든 진입점이 유저스페이스
 * ioctl 에서 시작하므로 입력 검증이 이 파일의 큰 부분을 차지한다.
 *
 * === 타 모듈과의 연결 ===
 * - iommufd(유저스페이스 인터페이스): 도메인 생성 요청과 무효화 요청이
 *   iommu_user_data 형태로 들어온다. 그 안의 값은 신뢰할 수 없다.
 * - pasid.c: intel_pasid_setup_nested() 가 PASID 항목에 1단계와 2단계 주소를
 *   함께 담는다.
 * - cache.c: 게스트가 자기 테이블을 고쳤다고 알리면 그 범위를 무효화한다.
 *   중첩용 태그(CACHE_TAG_NESTING_*)가 이 경로를 위해 존재한다.
 * - iommu.c: dmar_domain 의 s2_domain/s1_cfg/s2_link 필드와, 부모의
 *   s1_domains 목록·s1_lock 이 부모-자식 관계를 표현한다.
 * 데이터 흐름: VMM 이 게스트 테이블 주소로 도메인 생성 요청 → 검증 후 부모에
 * 매단다 → 장치를 붙이면 PASID 항목에 두 주소가 들어간다 → 게스트가 자기
 * 테이블을 고치면 VMM 이 무효화 ioctl → 이 파일이 캐시를 비운다.
 *
 * === 주요 함수/구조체 요약 ===
 * - intel_iommu_domain_alloc_nested(): 1단계 도메인을 만든다. 유저스페이스가
 *   준 값을 검증하고 부모의 s1_domains 목록에 매단다.
 * - intel_nested_attach_dev(): 장치를 이 도메인에 붙인다. 부모가 이 유닛과
 *   호환되는지 먼저 확인하는 것이 특징이다.
 * - intel_nested_set_dev_pasid(): PASID 단위로 붙인다.
 * - intel_nested_cache_invalidate_user(): 게스트가 요청한 무효화를 대신
 *   수행한다. 유저스페이스 배열을 하나씩 검증하며 처리한다.
 * - intel_nested_domain_free(): 부모의 목록에서 빼고 해제한다.
 */
#define pr_fmt(fmt)	"DMAR: " fmt	/* [한국어] 이 파일의 로그 접두사 */

#include <linux/iommu.h>	/* [한국어] 코어의 도메인 타입과 유저 데이터 구조체 */
#include <linux/pci.h>	/* [한국어] 소스 id */
#include <linux/pci-ats.h>	/* [한국어] ATS 능력 */

#include "iommu.h"	/* [한국어] 유닛·도메인·장치 자료구조 */
#include "pasid.h"	/* [한국어] PASID 항목 형식 */

/*
 * [한국어]
 * intel_nested_attach_dev - 장치를 중첩(1단계) 도메인에 붙인다
 *
 * @domain: 게스트의 1단계 도메인. @dev: 장치. @old: 직전 도메인(쓰지 않는다).
 * @return: 0 성공, 음수면 실패(그 경우 장치는 차단 상태로 남는다).
 *
 * 다른 attach 와 구조가 비슷하지만 첫 검사가 다르다. 1단계 도메인은 혼자
 * 동작할 수 없고 반드시 2단계 부모 위에 얹힌다(코드 안 영어 주석). 그래서
 * 자기 자신이 아니라 "부모가 이 유닛과 호환되는가"를 확인한다 — 실제 번역의
 * 하위 단계를 담당하는 것이 부모이기 때문이다.
 *
 * 그 다음은 순서대로 자원을 확보한다.
 *   1) domain_attach_iommu — 이 유닛에서 도메인 id 를 확보.
 *   2) cache_tag_assign_domain — 무효화 대상 등록. 중첩이므로 이 안에서
 *      부모 쪽 NESTING_* 태그까지 함께 등록된다.
 *   3) iopf_for_domain_set — 폴트 처리 연결.
 *   4) intel_pasid_setup_nested — 하드웨어 항목에 두 단계 주소를 담는다.
 * 각 단계가 실패하면 goto 라벨이 정확히 역순으로 되돌린다.
 *
 * 성공한 뒤에야 info->domain 과 목록을 갱신한다. 하드웨어가 먼저 서고
 * 소프트웨어 상태가 따라가는 순서이며, 이 파일 전체가 같은 원칙을 따른다.
 *
 * 실행 컨텍스트: iommufd 의 장치 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_attach_device()/iommufd → [intel_nested_attach_dev]
 *     → paging_domain_compatible(부모) → cache_tag_assign_domain()
 *     → intel_pasid_setup_nested()
 */
static int intel_nested_attach_dev(struct iommu_domain *domain,
				   struct device *dev, struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int ret = 0;	/* [한국어] 각 단계 결과 */

	device_block_translation(dev);	/* [한국어] 먼저 지금 붙어 있는 것을 전부 내린다 */

	/*
	 * Stage-1 domain cannot work alone, it is nested on a s2_domain.
	 * The s2_domain will be used in nested translation, hence needs
	 * to ensure the s2_domain is compatible with this IOMMU.
	 */
	ret = paging_domain_compatible(&dmar_domain->s2_domain->domain, dev);	/* [한국어] 자기가 아니라 부모를 검사한다. 1단계 도메인은 혼자 동작할 수 없고 실제 번역의 하위 단계를 부모가 담당하기 때문이다 (위 영어 주석) */
	if (ret) {	/* [한국어] 부모가 이 유닛에서 동작할 수 없으면 */
		dev_err_ratelimited(dev, "s2 domain is not compatible\n");	/* [한국어] 부모가 이 유닛에서 동작할 수 없다 */
		return ret;	/* [한국어] 붙일 수 없다 */
	}

	ret = domain_attach_iommu(dmar_domain, iommu);	/* [한국어] 이 유닛에서 도메인 id 를 확보한다 */
	if (ret) {	/* [한국어] 도메인 id 확보에 실패하면 */
		dev_err_ratelimited(dev, "Failed to attach domain to iommu\n");	/* [한국어] 도메인 id 고갈 등 */
		return ret;	/* [한국어] 실패 */
	}

	ret = cache_tag_assign_domain(dmar_domain, dev, IOMMU_NO_PASID);	/* [한국어] 무효화 대상 등록. 중첩이므로 이 안에서 부모 쪽 NESTING_* 태그까지 함께 등록된다 */
	if (ret)	/* [한국어] 실패 */
		goto detach_iommu;	/* [한국어] 도메인 id 를 되돌린다 */

	ret = iopf_for_domain_set(domain, dev);	/* [한국어] 폴트 처리를 연결한다 */
	if (ret)	/* [한국어] 실패 */
		goto unassign_tag;	/* [한국어] 태그 등록을 되돌린다 */

	ret = intel_pasid_setup_nested(iommu, dev,	/* [한국어] 하드웨어 항목에 게스트 1단계와 호스트 2단계 주소를 함께 담는다 */
				       IOMMU_NO_PASID, dmar_domain);	/* [한국어] PASID 를 쓰지 않는 기본 트래픽의 항목 */
	if (ret)	/* [한국어] 실패 */
		goto disable_iopf;	/* [한국어] 폴트 연결을 되돌린다 */

	info->domain = dmar_domain;	/* [한국어] 하드웨어가 선 뒤에야 소프트웨어 상태를 갱신한다 */
	info->domain_attached = true;	/* [한국어] 붙은 상태로 표시 */
	spin_lock_irqsave(&dmar_domain->lock, flags);	/* [한국어] 도메인의 장치 목록을 바꾼다 */
	list_add(&info->link, &dmar_domain->devices);	/* [한국어] 목록에 매단다 */
	spin_unlock_irqrestore(&dmar_domain->lock, flags);	/* [한국어] 락 해제 */

	return 0;	/* [한국어] 중첩 변환이 시작된다 */
disable_iopf:	/* [한국어] 하드웨어 설정 실패 경로 */
	iopf_for_domain_remove(domain, dev);	/* [한국어] 폴트 연결을 뗀다 */
unassign_tag:	/* [한국어] 폴트 연결 실패가 합류 */
	cache_tag_unassign_domain(dmar_domain, dev, IOMMU_NO_PASID);	/* [한국어] 무효화 대상 등록을 되돌린다 */
detach_iommu:	/* [한국어] 태그 등록 실패가 합류 */
	domain_detach_iommu(dmar_domain, iommu);	/* [한국어] 도메인 id 참조를 놓는다 */

	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * intel_nested_domain_free - 중첩 도메인을 부모에서 떼어 내고 해제한다
 *
 * @domain: 해제할 1단계 도메인.
 * @return: 없음.
 *
 * 페이지 테이블을 해제하지 않는 것이 특징이다 — 이 도메인은 게스트의 1단계
 * 테이블을 가리키기만 했고, 그 테이블은 게스트(정확히는 VMM)의 것이다.
 * 그래서 부모의 s1_domains 목록에서 자기를 빼고, 무효화 배치 버퍼와 구조체만
 * 반납한다.
 *
 * 부모 목록에서 먼저 빼는 순서가 중요하다. 부모가 더티 추적 설정을 자식들에
 * 전파하거나 해제 전 검사를 할 때 이 목록을 훑는데, 해제된 자식이 남아
 * 있으면 그때 해제된 메모리를 읽는다.
 *
 * 실행 컨텍스트: 도메인 해제. 프로세스 컨텍스트.
 */
static void intel_nested_domain_free(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct dmar_domain *s2_domain = dmar_domain->s2_domain;	/* [한국어] 부모 도메인 */

	spin_lock(&s2_domain->s1_lock);	/* [한국어] 부모의 자식 목록을 보호한다 */
	list_del(&dmar_domain->s2_link);	/* [한국어] 먼저 목록에서 뺀다. 남아 있으면 부모가 설정을 전파할 때 해제된 메모리를 읽는다 */
	spin_unlock(&s2_domain->s1_lock);	/* [한국어] 락 해제 */
	kfree(dmar_domain->qi_batch);	/* [한국어] 무효화 배치 버퍼 */
	kfree(dmar_domain);	/* [한국어] 도메인 구조체. 페이지 테이블은 해제하지 않는다 — 게스트의 것이다 */
}

/*
 * [한국어]
 * intel_nested_cache_invalidate_user - 게스트가 요청한 무효화를 대신 수행한다
 *
 * @domain: 중첩 도메인. @array: 유저스페이스가 넘긴 무효화 요청 배열.
 * @return: 0 성공, 음수면 실패. 처리한 개수는 array->entry_num 으로 돌려준다.
 *
 * 왜 이 함수가 필요한가: 게스트가 자기 1단계 페이지 테이블을 고치면 그
 * 매핑의 캐시를 비워야 한다. 그런데 게스트는 하드웨어에 직접 무효화를 보낼
 * 수 없다 — 그러면 다른 게스트의 캐시까지 비울 수 있기 때문이다. 그래서
 * 게스트가 자기 IOMMU 에 무효화를 요청하면 VMM 이 그것을 가로채 이 ioctl 로
 * 호스트에 전달하고, 호스트가 범위를 검증한 뒤 대신 수행한다.
 *
 * 입력이 전부 신뢰할 수 없는 값이라 검증이 꼼꼼하다.
 *   - 배열의 형식이 VT-d 1단계용인가.
 *   - 각 항목의 flags 에 모르는 비트가 켜져 있지 않은가. 예약 필드가 0 인가.
 *     모르는 비트를 조용히 무시하면 게스트는 그 기능이 동작한다고 믿는다.
 *   - 주소가 페이지 정렬인가.
 *   - npages 가 U64_MAX(전체 무효화)인데 주소가 0 이 아닌 모순은 아닌가.
 *
 * 부분 처리를 허용하는 것이 특징이다. 도중에 실패하면 거기서 멈추고, 그때까지
 * 처리한 개수를 entry_num 으로 돌려준다. 유저스페이스는 그 값을 보고 어디까지
 * 성공했는지 알아 나머지를 다시 요청할 수 있다 — 전부 되돌리는 것보다 이쪽이
 * 자연스럽다. 이미 수행한 무효화는 되돌릴 수 있는 성질의 것이 아니기도 하다.
 *
 * 실행 컨텍스트: iommufd 무효화 ioctl. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommufd IOMMU_HWPT_INVALIDATE ioctl → [intel_nested_cache_invalidate_user]
 *     → cache_tag_flush_range()
 */
static int intel_nested_cache_invalidate_user(struct iommu_domain *domain,
					      struct iommu_user_data_array *array)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct iommu_hwpt_vtd_s1_invalidate inv_entry;	/* [한국어] 요청 하나를 복사해 올 자리 */
	u32 index, processed = 0;	/* [한국어] 순회 인덱스와 처리한 개수 */
	int ret = 0;	/* [한국어] 결과 */

	if (array->type != IOMMU_HWPT_INVALIDATE_DATA_VTD_S1) {	/* [한국어] VT-d 1단계용 형식이 아니면 */
		ret = -EINVAL;	/* [한국어] 해석할 수 없다 */
		goto out;	/* [한국어] 처리 개수 0 으로 반환 */
	}

	for (index = 0; index < array->entry_num; index++) {	/* [한국어] 요청을 하나씩 */
		ret = iommu_copy_struct_from_user_array(&inv_entry, array,	/* [한국어] 유저스페이스에서 안전하게 복사한다 */
							IOMMU_HWPT_INVALIDATE_DATA_VTD_S1,	/* [한국어] 기대하는 형식 */
							index, __reserved);	/* [한국어] 몇 번째인지와 예약 필드의 이름 */
		if (ret)	/* [한국어] 복사 실패(잘못된 포인터 등) */
			break;	/* [한국어] 거기서 멈춘다 */

		if ((inv_entry.flags & ~IOMMU_VTD_INV_FLAGS_LEAF) ||	/* [한국어] 모르는 플래그가 켜져 있거나 */
		    inv_entry.__reserved) {	/* [한국어] 예약 필드가 0 이 아니면 */
			ret = -EOPNOTSUPP;	/* [한국어] 거절한다. 조용히 무시하면 게스트는 그 기능이 동작한다고 믿는다 */
			break;	/* [한국어] 멈춘다 */
		}

		if (!IS_ALIGNED(inv_entry.addr, VTD_PAGE_SIZE) ||	/* [한국어] 주소가 페이지 정렬이 아니거나 */
		    ((inv_entry.npages == U64_MAX) && inv_entry.addr)) {	/* [한국어] 전체 무효화인데 주소가 0 이 아닌 모순이면 */
			ret = -EINVAL;	/* [한국어] 거절 */
			break;	/* [한국어] 멈춘다 */
		}

		cache_tag_flush_range(dmar_domain, inv_entry.addr,	/* [한국어] 검증을 통과한 범위만 무효화한다 */
				      inv_entry.addr + nrpages_to_size(inv_entry.npages) - 1,	/* [한국어] 페이지 수를 바이트 크기로 바꿔 끝 주소를 구한다 */
				      inv_entry.flags & IOMMU_VTD_INV_FLAGS_LEAF);	/* [한국어] 잎 항목만 무효화하라는 힌트를 그대로 전달 */
		processed++;	/* [한국어] 처리한 개수를 센다 */
	}

out:	/* [한국어] 형식 오류가 합류 */
	array->entry_num = processed;	/* [한국어] 어디까지 성공했는지 알려 준다. 이미 수행한 무효화는 되돌릴 수 없으므로 부분 처리를 그대로 보고한다 */
	return ret;	/* [한국어] 실패 이유(또는 0) */
}

/*
 * [한국어]
 * domain_setup_nested - PASID 항목을 중첩 변환으로 세운다(필요하면 옛 항목을 먼저 내린다)
 *
 * @iommu: 유닛. @domain: 중첩 도메인. @dev: 장치. @pasid: 대상 PASID.
 * @old: 이 PASID 가 쓰고 있던 도메인. NULL 이 아니면 교체다.
 * @return: intel_pasid_setup_nested 의 결과.
 *
 * intel_pasid_setup_nested 는 이미 present 인 항목을 덮어쓰지 않는다(-EBUSY).
 * 그래서 교체인 경우 먼저 내려야 하고, 이 얇은 층이 그 판단을 맡는다.
 *
 * 여기에는 짧은 공백이 있다: 옛 항목을 내린 순간부터 새 항목이 설 때까지
 * 그 PASID 의 DMA 는 차단된다. fault_ignore 를 false 로 두는 것은 그 사이의
 * 접근을 폴트로 보고받겠다는 뜻이다 — 조용히 삼키면 무엇이 잘못되었는지
 * 알 수 없다.
 *
 * 실행 컨텍스트: PASID 부착/교체. 프로세스 컨텍스트.
 */
static int domain_setup_nested(struct intel_iommu *iommu,
			       struct dmar_domain *domain,
			       struct device *dev, ioasid_t pasid,
			       struct iommu_domain *old)
{
	if (old)	/* [한국어] 교체라면 */
		intel_pasid_tear_down_entry(iommu, dev, pasid, false);	/* [한국어] 옛 항목을 먼저 내린다. setup_nested 는 present 인 항목을 덮어쓰지 않는다. false 는 그 사이의 접근을 폴트로 보고받겠다는 뜻이다 */

	return intel_pasid_setup_nested(iommu, dev, pasid, domain);	/* [한국어] 새 항목을 세운다 */
}

/*
 * [한국어]
 * intel_nested_set_dev_pasid - 장치의 한 PASID 를 중첩 도메인에 붙인다
 *
 * @domain: 중첩 도메인. @dev: 장치. @pasid: 대상 PASID. @old: 직전 도메인.
 * @return: 0 성공, 음수면 실패(옛 상태가 그대로 유지된다).
 *
 * attach_dev 의 PASID 단위 판이다. 게스트 안에서 여러 프로세스가 각자
 * 주소 공간을 갖고 장치를 쓰는 경우(게스트 SVA)가 이 경로로 온다.
 *
 * 거절 조건이 셋이다.
 *   - 유닛이 PASID 를 못 하거나 부모 항목을 공유하는 서브디바이스면.
 *   - kdump 로 물려받은 컨텍스트가 아직 전환되지 않았으면(-EBUSY).
 *   - 부모(2단계) 도메인이 이 유닛과 호환되지 않으면.
 * 마지막 검사가 attach_dev 와 같은 이유로 부모를 본다 — 실제 번역의 하위
 * 단계를 담당하는 것이 부모이기 때문이다.
 *
 * 교체 순서와 되돌리기는 이 드라이버의 다른 set_dev_pasid 들과 같다:
 * 준비 → 폴트 처리 이동 → 하드웨어 교체 → 성공 후 옛 기록 제거.
 *
 * 실행 컨텍스트: iommufd PASID 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_attach_device_pasid()/iommufd → [intel_nested_set_dev_pasid]
 *     → domain_add_dev_pasid() → domain_setup_nested()
 */
static int intel_nested_set_dev_pasid(struct iommu_domain *domain,
				      struct device *dev, ioasid_t pasid,
				      struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct dev_pasid_info *dev_pasid;	/* [한국어] 만들 (장치, PASID) 기록 */
	int ret;	/* [한국어] 결과 */

	if (!pasid_supported(iommu) || dev_is_real_dma_subdevice(dev))	/* [한국어] 유닛이 PASID 를 못 하거나 부모 항목을 공유하는 서브디바이스면 */
		return -EOPNOTSUPP;	/* [한국어] PASID 단위로 붙일 수 없다 */

	if (context_copied(iommu, info->bus, info->devfn))	/* [한국어] 물려받은 컨텍스트가 아직 전환되지 않았으면 */
		return -EBUSY;	/* [한국어] 그 위에 PASID 를 얹을 수 없다 */

	ret = paging_domain_compatible(&dmar_domain->s2_domain->domain, dev);	/* [한국어] attach_dev 와 같은 이유로 부모를 검사한다 */
	if (ret)	/* [한국어] 부모가 이 유닛에서 동작할 수 없으면 */
		return ret;	/* [한국어] 붙일 수 없다 */

	dev_pasid = domain_add_dev_pasid(domain, dev, pasid);	/* [한국어] 도메인 id 확보와 무효화 대상 등록 */
	if (IS_ERR(dev_pasid))	/* [한국어] 실패 */
		return PTR_ERR(dev_pasid);	/* [한국어] 전달 */

	ret = iopf_for_domain_replace(domain, old, dev);	/* [한국어] 폴트 처리를 새 도메인으로 옮긴다 */
	if (ret)	/* [한국어] 실패 */
		goto out_remove_dev_pasid;	/* [한국어] 기록을 되돌린다 */

	ret = domain_setup_nested(iommu, dmar_domain, dev, pasid, old);	/* [한국어] 옛 항목을 내리고 새 중첩 항목을 세운다 */
	if (ret)	/* [한국어] 실패 */
		goto out_unwind_iopf;	/* [한국어] 폴트 처리를 되돌린다 */

	domain_remove_dev_pasid(old, dev, pasid);	/* [한국어] 성공한 뒤에야 옛 도메인의 기록을 지운다 */

	return 0;	/* [한국어] 이 PASID 가 중첩 변환을 쓴다 */

out_unwind_iopf:	/* [한국어] 하드웨어 설정 실패 경로 */
	iopf_for_domain_replace(old, domain, dev);	/* [한국어] 폴트 처리를 옛 도메인으로 */
out_remove_dev_pasid:	/* [한국어] 폴트 처리 실패가 합류 */
	domain_remove_dev_pasid(domain, dev, pasid);	/* [한국어] 새 도메인의 기록을 지운다 */
	return ret;	/* [한국어] 실패 이유 */
}

static const struct iommu_domain_ops intel_nested_domain_ops = {	/* [한국어] 중첩 도메인의 콜백 표 */
	.attach_dev		= intel_nested_attach_dev,	/* [한국어] 장치 부착 */
	.set_dev_pasid		= intel_nested_set_dev_pasid,	/* [한국어] PASID 부착 */
	.free			= intel_nested_domain_free,	/* [한국어] 해제 */
	.cache_invalidate_user	= intel_nested_cache_invalidate_user,	/* [한국어] 게스트가 요청한 무효화 대행. 다른 도메인 종류에는 없는 콜백으로, 중첩의 성격을 잘 보여 준다 */
};

/*
 * [한국어]
 * intel_iommu_domain_alloc_nested - 게스트의 1단계 도메인을 만들어 부모에 매단다
 *
 * @dev: 이 도메인을 쓸 장치. @parent: 부모가 될 2단계 도메인.
 * @flags: 요청한 성질(PASID 만 허용).
 * @user_data: 유저스페이스가 준 게스트 테이블 설정(IOMMU_HWPT_DATA_VTD_S1).
 * @return: 만들어진 도메인, 실패 시 ERR_PTR.
 *
 * 유저스페이스가 준 값을 받아들이는 자리라 검증이 앞에 몰려 있다.
 *   - 하드웨어가 중첩을 지원하는가(nested_supported).
 *   - 데이터 형식이 VT-d 1단계용인가.
 *   - 부모가 정말 2단계 페이징 도메인이고 nested_parent 로 표시되어 있는가.
 *     아무 도메인이나 부모로 삼으면 두 단계 번역이 성립하지 않는다.
 * 그 다음 iommu_copy_struct_from_user 로 게스트 설정을 복사한다 — 그 안의
 * 페이지 테이블 주소는 여전히 신뢰할 수 없지만, 그것이 가리키는 모든 주소가
 * 부모의 2단계 매핑을 거치므로 안전하다.
 *
 * 만드는 것이 적다: 페이지 테이블도, 그것을 담을 union 도 쓰지 않는다.
 * 목록 셋과 락 둘, xarray 하나, 그리고 부모 링크가 전부다. 그것이 "가리키기만
 * 하는 도메인"이 필요로 하는 상태다.
 *
 * GFP_KERNEL_ACCOUNT 를 쓰는 것을 눈여겨볼 것: 유저스페이스가 요청해 만드는
 * 객체이므로 그 프로세스의 메모리 cgroup 에 청구한다. VMM 이 도메인을 무한히
 * 만들어 커널 메모리를 고갈시키는 것을 막는다.
 *
 * 마지막에 부모의 s1_domains 목록에 매단다. 그래야 부모가 더티 추적 설정을
 * 자식들에 전파하거나, 해제 전에 자식이 남아 있는지 확인할 수 있다.
 *
 * 실행 컨텍스트: iommufd 도메인 생성 ioctl. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommufd IOMMU_HWPT_ALLOC ioctl → iommu_ops.domain_alloc_nested
 *     → [intel_iommu_domain_alloc_nested]
 */
struct iommu_domain *
intel_iommu_domain_alloc_nested(struct device *dev, struct iommu_domain *parent,
				u32 flags,
				const struct iommu_user_data *user_data)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct dmar_domain *s2_domain = to_dmar_domain(parent);	/* [한국어] 부모가 될 2단계 도메인 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct iommu_hwpt_vtd_s1 vtd;	/* [한국어] 유저스페이스에서 복사해 올 게스트 설정 */
	struct dmar_domain *domain;	/* [한국어] 만들 도메인 */
	int ret;	/* [한국어] 결과 */

	if (!nested_supported(iommu) || flags & ~IOMMU_HWPT_ALLOC_PASID)	/* [한국어] 하드웨어가 중첩을 못 하거나 모르는 플래그가 있으면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절 */

	/* Must be nested domain */
	if (user_data->type != IOMMU_HWPT_DATA_VTD_S1)	/* [한국어] VT-d 1단계 형식이 아니면 (위 영어 주석) */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 해석할 수 없다 */
	if (!intel_domain_is_ss_paging(s2_domain) || !s2_domain->nested_parent)	/* [한국어] 부모가 2단계 페이징이 아니거나 중첩 부모로 표시되지 않았으면 */
		return ERR_PTR(-EINVAL);	/* [한국어] 아무 도메인이나 부모로 삼으면 두 단계 번역이 성립하지 않는다 */

	ret = iommu_copy_struct_from_user(&vtd, user_data,	/* [한국어] 게스트 설정을 안전하게 복사한다 */
					  IOMMU_HWPT_DATA_VTD_S1, __reserved);	/* [한국어] 기대 형식과 예약 필드 이름 */
	if (ret)	/* [한국어] 복사 실패 */
		return ERR_PTR(ret);	/* [한국어] 전달 */

	domain = kzalloc_obj(*domain, GFP_KERNEL_ACCOUNT);	/* [한국어] 유저스페이스가 요청해 만드는 객체이므로 그 프로세스의 메모리 cgroup 에 청구한다. VMM 이 도메인을 무한히 만들어 커널 메모리를 고갈시키는 것을 막는다 */
	if (!domain)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 전달 */

	domain->s2_domain = s2_domain;	/* [한국어] 부모를 기억한다 */
	domain->s1_cfg = vtd;	/* [한국어] 게스트 테이블 주소와 설정. 여전히 신뢰할 수 없지만 부모의 2단계를 거치므로 안전하다 */
	domain->domain.ops = &intel_nested_domain_ops;	/* [한국어] 중첩 전용 콜백 표 */
	domain->domain.type = IOMMU_DOMAIN_NESTED;	/* [한국어] 코어가 이 도메인을 중첩으로 인식한다 */
	INIT_LIST_HEAD(&domain->devices);	/* [한국어] 붙은 장치 목록 */
	INIT_LIST_HEAD(&domain->dev_pasids);	/* [한국어] PASID 쌍 목록 */
	INIT_LIST_HEAD(&domain->cache_tags);	/* [한국어] 무효화 대상 목록 */
	spin_lock_init(&domain->lock);	/* [한국어] 장치 목록을 지키는 락 */
	spin_lock_init(&domain->cache_lock);	/* [한국어] 무효화 목록을 지키는 락 */
	xa_init(&domain->iommu_array);	/* [한국어] 유닛별 도메인 id. 페이지 테이블은 만들지 않는다 — 게스트의 것을 가리키기만 한다 */

	spin_lock(&s2_domain->s1_lock);	/* [한국어] 부모의 자식 목록을 보호한다 */
	list_add(&domain->s2_link, &s2_domain->s1_domains);	/* [한국어] 부모에 매단다. 그래야 부모가 더티 추적을 전파하거나 해제 전 검사를 할 수 있다 */
	spin_unlock(&s2_domain->s1_lock);	/* [한국어] 락 해제 */

	return &domain->domain;	/* [한국어] 코어가 다루는 도메인 포인터로 반환 */
}
