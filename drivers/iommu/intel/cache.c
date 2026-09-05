// SPDX-License-Identifier: GPL-2.0
/*
 * cache.c - Intel VT-d cache invalidation
 *
 * Copyright (C) 2024 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 */

/*
 * [한국어 설명] VT-d 캐시 무효화의 정규화 계층 (intel/cache.c)
 *
 * === 파일의 역할 ===
 * "매핑이 바뀌었다"를 "어느 캐시에 어떤 무효화 명령을 몇 번 보낼 것인가"로
 * 바꾸는 파일이다. VT-d 에서 번역 결과가 캐시되는 곳은 한 군데가 아니다 —
 * 유닛 안의 IOTLB, ATS 를 켠 장치 안의 디바이스 TLB, 그리고 중첩 변환에서는
 * 부모·자식 각각의 캐시까지 있다. 도메인에 장치가 열 개 붙어 있어도 그 열
 * 개가 같은 유닛의 같은 도메인 id 를 쓴다면 IOTLB 무효화는 한 번이면 되고,
 * 반대로 ATS 를 켠 장치는 각자의 캐시를 따로 비워야 한다.
 * 이 파일은 그 "누구에게 몇 번" 을 도메인마다 cache_tag 목록으로 미리
 * 정규화해 두고(cache_tag_assign_domain), 무효화가 필요할 때 그 목록만 훑어
 * 명령을 만든다(cache_tag_flush_*). 장치 목록을 매번 훑으며 중복을 걸러 내지
 * 않아도 되게 하는 것이 핵심이며, 무효화는 언매핑마다 일어나는 뜨거운
 * 경로라 그 차이가 크다.
 * 명령을 하나씩 보내지 않고 qi_batch 에 모아 한 번에 제출하는 것도 이
 * 파일의 몫이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 매핑 변경 → [이 파일] → 무효화 큐 → 하드웨어 의 중간 단계다.
 *   위쪽: iommu.c 의 도메인 부착·분리가 cache_tag_assign_domain/
 *         unassign_domain 으로 대상 목록을 갱신하고, 언매핑 경로
 *         (intel_iommu_tlb_sync, iotlb_sync_map)가 cache_tag_flush_* 를 부른다.
 *         SVA 의 mmu_notifier 와 nested.c 의 사용자 무효화도 여기로 모인다.
 *   아래쪽: intel/iommu.h 의 qi_desc_* 매크로로 서술자를 조립하고,
 *         dmar.c 의 qi_submit_sync() 로 유닛의 무효화 큐에 제출한다.
 *         큐를 쓸 수 없는 유닛에서는 iommu->flush 의 레지스터 방식으로 간다.
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트지만, SVA 의 mmu_notifier 경로는
 * 잠들 수 없는 문맥에서 들어온다. 그래서 이 파일의 함수들은 스핀락만 쓰고
 * 할당을 하지 않는다(qi_batch 는 미리 잡아 둔다).
 *
 * === 타 모듈과의 연결 ===
 * - iommu.c: dmar_domain 의 cache_tags 목록과 cache_lock 이 이 파일의 자료다.
 *   도메인이 만들어질 때 빈 목록으로 시작해, 장치가 붙을 때마다 태그가 늘고
 *   떨어질 때마다 준다.
 * - iommu.h: struct cache_tag 와 enum cache_tag_type 의 정의, qi_desc_* 조립
 *   매크로, 그리고 domain_id_iommu() 같은 조회 헬퍼.
 * - dmar.c: qi_submit_sync() 가 실제 제출을 맡는다.
 * - pasid.h: PASID 항목을 다루는 무효화에서 그 형식을 참조한다.
 * - trace.h: 무효화 하나하나를 ftrace 이벤트로 남겨, 어떤 명령이 몇 번
 *   나갔는지 사후에 볼 수 있게 한다.
 * 데이터 흐름: (장치, PASID) 부착 → 필요한 태그들이 목록에 추가 →
 * 매핑 변경 → 목록을 훑어 서술자 조립 → 배치에 모음 → 큐에 제출 →
 * 완료 대기 → 그제서야 페이지 반납이 안전해진다.
 *
 * === 주요 함수/구조체 요약 ===
 * - cache_tag_assign()/cache_tag_unassign(): 태그 하나를 등록·해제한다.
 *   같은 태그가 이미 있으면 참조 계수만 늘려 중복 무효화를 막는다.
 * - cache_tag_assign_domain()/unassign_domain(): 한 (장치, PASID) 에 필요한
 *   태그들을 한꺼번에 다룬다. 도메인 종류(1단계/2단계/중첩)와 ATS 활성
 *   여부에 따라 IOTLB 만일 수도, DEVTLB 까지일 수도 있다.
 * - cache_tag_flush_range(): 주어진 IOVA 범위를 모든 태그에 대해 무효화한다.
 *   범위가 넓으면 페이지 단위 대신 도메인 전체 무효화로 승격한다.
 * - cache_tag_flush_all(): 범위 없이 도메인 전체.
 * - cache_tag_flush_range_np(): "매핑 없음" 캐시를 지운다. 캐싱 모드
 *   하드웨어에서 새로 만든 매핑을 보이게 하는 용도다.
 * - qi_batch_* : 서술자를 모았다가 한 번에 제출하는 배치 계층.
 * - calculate_psi_aligned_address(): 페이지 선택 무효화가 요구하는
 *   "주소가 그 크기에 정렬" 조건을 맞춰 주는 계산. 어긋나면 무효화 범위가
 *   조용히 틀어지므로 이 파일에서 가장 조심스러운 자리다.
 */
#define pr_fmt(fmt)	"DMAR: " fmt	/* [한국어] 이 파일의 모든 로그에 붙는 접두사 */

#include <linux/dmar.h>	/* [한국어] DRHD 유닛 열거 */
#include <linux/iommu.h>	/* [한국어] 코어의 도메인/장치 타입 */
#include <linux/memory.h>	/* [한국어] 메모리 관련 상수 */
#include <linux/pci.h>	/* [한국어] 소스 id 조립과 ATS 큐 깊이 조회 */
#include <linux/spinlock.h>	/* [한국어] cache_lock 을 다루는 데 필요하다 */

#include "iommu.h"	/* [한국어] struct cache_tag, qi_desc_* 조립 매크로, 능력 판정 */
#include "pasid.h"	/* [한국어] PASID 항목 형식 — PASID 단위 무효화에서 참조한다 */
#include "trace.h"	/* [한국어] 무효화 하나하나를 ftrace 이벤트로 남긴다 */

/* Check if an existing cache tag can be reused for a new association. */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tage_match - 이미 있는 태그를 새 등록에 재사용할 수 있는지 본다
 *
 * @tag: 기존 태그. @domain_id: 이 유닛에서의 도메인 id. @iommu: 유닛.
 * @dev: 장치. @pasid: PASID. @type: 태그 종류.
 * @return: true 면 같은 캐시를 가리키므로 참조만 늘리면 된다.
 *
 * 이 함수가 이 파일의 중복 제거를 정의한다. 무엇을 "같다"고 볼 것인가에 따라
 * 무효화가 몇 번 나갈지가 정해지기 때문이다.
 *
 * 비교 규칙이 종류마다 다른 것이 핵심이다.
 *   IOTLB 계열   — 캐시가 유닛 안에 있으므로 (유닛, 도메인 id, PASID)가 같으면
 *                  같은 캐시다. 장치가 열 개든 무효화는 한 번이면 된다.
 *   DEVTLB 계열  — 캐시가 각 장치 안에 있으므로 장치가 다르면 다른 캐시다.
 *                  도메인 id 가 같아도 각각 비워야 한다.
 * 앞의 도메인 id·PASID 검사는 두 계열 모두에 적용되는 공통 조건이다.
 *
 * 중첩용(NESTING_*)을 일반과 구분하는 이유: 같은 유닛·같은 장치라도 게스트
 * 1단계가 바뀐 것과 호스트 2단계가 바뀐 것은 보낼 명령이 다르다.
 *
 * 실행 컨텍스트: cache_lock 을 쥔 채. 순수 비교라 잠들지 않는다.
 *
 * 호출 체인:
 *   cache_tag_assign()/cache_tag_unassign() → [cache_tage_match]
 */
static bool cache_tage_match(struct cache_tag *tag, u16 domain_id,
			     struct intel_iommu *iommu, struct device *dev,
			     ioasid_t pasid, enum cache_tag_type type)
{
	if (tag->type != type)	/* [한국어] 종류가 다르면 */
		return false;	/* [한국어] 다른 캐시다 */

	if (tag->domain_id != domain_id || tag->pasid != pasid)	/* [한국어] 도메인 id 나 PASID 가 다르면 */
		return false;	/* [한국어] 역시 다른 캐시다 */

	if (type == CACHE_TAG_IOTLB || type == CACHE_TAG_NESTING_IOTLB)	/* [한국어] 유닛 안의 캐시라면 */
		return tag->iommu == iommu;	/* [한국어] 같은 유닛이기만 하면 같은 캐시다 — 장치가 달라도 무효화는 한 번이면 된다 */

	if (type == CACHE_TAG_DEVTLB || type == CACHE_TAG_NESTING_DEVTLB)	/* [한국어] 장치 안의 캐시라면 */
		return tag->dev == dev;	/* [한국어] 장치가 같아야 같은 캐시다. 각 장치의 TLB 를 따로 비워야 한다 */

	return false;	/* [한국어] 알 수 없는 종류 */
}

/* Assign a cache tag with specified type to domain. */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tag_assign - 무효화 대상 태그 하나를 도메인에 등록한다
 *
 * @domain: 대상 도메인. @did: 이 유닛에서의 도메인 id. @dev: 장치.
 * @pasid: PASID. @type: 태그 종류.
 * @return: 0 성공(중복이라 참조만 늘린 경우 포함), -ENOMEM 할당 실패.
 *
 * 이미 같은 캐시를 가리키는 태그가 있으면 새로 만들지 않고 users 만 늘린다.
 * 그 판단은 cache_tage_match 가 한다. 이 중복 제거가 이 파일의 존재 이유이며,
 * 도메인에 장치가 많아도 IOTLB 무효화가 유닛당 한 번으로 유지되게 한다.
 *
 * 락을 잡기 전에 미리 할당하는 이유: cache_lock 은 인터럽트를 끄고 잡는
 * 스핀락이라 그 안에서 GFP_KERNEL 할당을 할 수 없다. 그래서 먼저 잡아 두고,
 * 중복이었다면 락을 놓은 뒤 그냥 버린다(kfree). 대부분의 경우 헛수고지만
 * 락 안에서 잠들 수 없다는 제약이 더 강하다.
 *
 * tag->dev 에 무엇을 넣는지가 종류마다 다르다. DEVTLB 면 캐시가 그 장치 안에
 * 있으므로 장치 포인터를, IOTLB 면 캐시가 유닛에 있으므로 유닛의 장치
 * 포인터를 넣는다 — struct cache_tag 의 dev 필드 주석이 말하는 그대로다.
 *
 * 목록에 넣는 위치가 중요하다(위 영어 주석): 같은 유닛의 태그들이 목록에서
 * 이웃하도록 prev 를 유지하며 삽입한다. 그래야 무효화 경로가 목록을 훑으며
 * 같은 유닛의 명령을 연속으로 만나 배치로 묶을 수 있다. 흩어져 있으면
 * 유닛이 바뀔 때마다 배치를 제출해야 해서 묶음이 잘게 쪼개진다.
 *
 * 실행 컨텍스트: 장치/PASID 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cache_tag_assign_domain() → [cache_tag_assign] → cache_tage_match()
 */
int cache_tag_assign(struct dmar_domain *domain, u16 did, struct device *dev,
		     ioasid_t pasid, enum cache_tag_type type)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct cache_tag *tag, *temp;	/* [한국어] 만들 태그와 순회 커서 */
	struct list_head *prev;	/* [한국어] 같은 유닛의 태그 뒤에 넣기 위한 삽입 위치 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	tag = kzalloc_obj(*tag);	/* [한국어] 락 밖에서 미리 잡는다 — cache_lock 안에서는 잠들 수 없다 */
	if (!tag)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 등록 불가 */

	tag->type = type;	/* [한국어] 어느 캐시를 가리키는지 */
	tag->iommu = iommu;	/* [한국어] 어느 유닛의 큐로 보낼지 */
	tag->domain_id = did;	/* [한국어] 무효화 명령의 DID 필드에 실릴 값 */
	tag->pasid = pasid;	/* [한국어] 대상 PASID */
	tag->users = 1;	/* [한국어] 첫 사용자 */

	if (type == CACHE_TAG_DEVTLB || type == CACHE_TAG_NESTING_DEVTLB)	/* [한국어] 장치 안의 캐시면 */
		tag->dev = dev;	/* [한국어] 그 장치를 기록한다 — 무효화 서술자의 소스 id 와 큐 깊이가 여기서 나온다 */
	else
		tag->dev = iommu->iommu.dev;	/* [한국어] 유닛 안의 캐시면 유닛의 장치를 기록한다. 실제 무효화 대상이 아니라 로그와 진단용이다 */

	spin_lock_irqsave(&domain->cache_lock, flags);	/* [한국어] 목록 변경 구간 */
	prev = &domain->cache_tags;	/* [한국어] 기본 삽입 위치는 목록의 앞 */
	list_for_each_entry(temp, &domain->cache_tags, node) {	/* [한국어] 기존 태그들을 훑으며 */
		if (cache_tage_match(temp, did, iommu, dev, pasid, type)) {	/* [한국어] 같은 캐시를 가리키는 것이 있으면 */
			temp->users++;	/* [한국어] 참조만 늘린다 */
			spin_unlock_irqrestore(&domain->cache_lock, flags);	/* [한국어] 락 해제 */
			kfree(tag);	/* [한국어] 미리 잡아 둔 것은 버린다 */
			trace_cache_tag_assign(temp);	/* [한국어] 추적 이벤트 */
			return 0;	/* [한국어] 등록 완료(중복이었다) */
		}
		if (temp->iommu == iommu)	/* [한국어] 같은 유닛의 태그를 만나면 */
			prev = &temp->node;	/* [한국어] 그 뒤를 삽입 위치로 삼는다 */
	}
	/*
	 * Link cache tags of same iommu unit together, so corresponding
	 * flush ops can be batched for iommu unit.
	 */
	list_add(&tag->node, prev);	/* [한국어] 같은 유닛의 태그들이 이웃하게 넣는다. 무효화 경로가 유닛별로 명령을 배치로 묶을 수 있게 하기 위해서다 (위 영어 주석) */

	spin_unlock_irqrestore(&domain->cache_lock, flags);	/* [한국어] 락 해제 */
	trace_cache_tag_assign(tag);	/* [한국어] 추적 이벤트 */

	return 0;	/* [한국어] 새 태그 등록 완료 */
}

/* Unassign a cache tag with specified type from domain. */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tag_unassign - 태그 하나의 참조를 놓고, 마지막이면 없앤다
 *
 * @domain: 대상 도메인. @did: 도메인 id. @dev: 장치. @pasid: PASID.
 * @type: 태그 종류.
 * @return: 없음.
 *
 * assign 의 짝. cache_tage_match 로 같은 캐시를 가리키는 태그를 찾아 users 를
 * 하나 줄이고, 0 이 되면 목록에서 빼고 해제한다.
 *
 * 참조 계수가 0 이 되기 전에 태그를 없애면 안 되는 이유: 아직 그 캐시를 쓰는
 * 다른 (장치, PASID) 조합이 남아 있다는 뜻이고, 태그가 사라지면 그쪽의
 * 매핑을 풀어도 무효화가 나가지 않는다. 옛 번역이 캐시에 남은 채 페이지가
 * 재사용되면 조용한 메모리 손상이 된다.
 *
 * 찾지 못해도 조용히 지나간다 — 이미 없는 태그를 놓으려는 것은 정상적인
 * 해제 경로에서도 일어날 수 있기 때문이다(ATS 가 꺼진 장치의 DEVTLB 태그 등).
 *
 * 실행 컨텍스트: 장치/PASID 분리. cache_lock 을 잡는다.
 *
 * 호출 체인:
 *   cache_tag_unassign_domain()/__cache_tag_assign_domain() 실패 경로
 *     → [cache_tag_unassign] → cache_tage_match()
 */
static void cache_tag_unassign(struct dmar_domain *domain, u16 did,
			       struct device *dev, ioasid_t pasid,
			       enum cache_tag_type type)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct cache_tag *tag;	/* [한국어] 순회 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&domain->cache_lock, flags);	/* [한국어] 목록 변경 구간 */
	list_for_each_entry(tag, &domain->cache_tags, node) {	/* [한국어] 태그들을 훑으며 */
		if (cache_tage_match(tag, did, iommu, dev, pasid, type)) {	/* [한국어] 같은 캐시를 가리키는 것을 찾으면 */
			trace_cache_tag_unassign(tag);	/* [한국어] 추적 이벤트 */
			if (--tag->users == 0) {	/* [한국어] 마지막 사용자였으면 */
				list_del(&tag->node);	/* [한국어] 목록에서 빼고 */
				kfree(tag);	/* [한국어] 해제한다. 아직 사용자가 남아 있는데 없애면 그쪽 무효화가 나가지 않는다 */
			}
			break;	/* [한국어] 하나뿐이므로 종료 */
		}
	}
	spin_unlock_irqrestore(&domain->cache_lock, flags);	/* [한국어] 락 해제 */
}

/* domain->qi_batch will be freed in iommu_free_domain() path. */
/*
 * [한국어] (위 영어 주석에 이어)
 * domain_qi_batch_alloc - 이 도메인의 무효화 배치 버퍼를 준비한다
 *
 * @domain: 대상 도메인.
 * @return: 0 성공(이미 있으면 아무것도 안 하고 0), -ENOMEM 실패.
 *
 * 왜 미리 잡아 두는가: 무효화 경로는 잠들 수 없는 문맥(SVA 의 mmu_notifier,
 * 인터럽트를 끈 구간)에서도 들어온다. 그때 버퍼가 없어 할당해야 한다면
 * 무효화 자체를 할 수 없다. 그래서 장치를 붙이는 시점 — 잠들 수 있는
 * 프로세스 컨텍스트 — 에 미리 잡아 둔다.
 *
 * GFP_ATOMIC 인 것은 cache_lock 을 쥔 채 할당하기 때문이다. 락 밖에서 잡고
 * 안에서 확인하는 방식도 있지만, 이 경로는 장치 부착 때 한 번뿐이라
 * 단순함을 택했다.
 *
 * 이미 있으면 그대로 둔다 — 배치 버퍼는 도메인당 하나이고, 여러 장치가
 * 붙어도 공유한다. 해제는 이 함수가 하지 않고 도메인 해제 경로가 맡는다
 * (위 영어 주석).
 *
 * 실행 컨텍스트: 장치/PASID 부착. cache_lock 을 잡는다.
 *
 * 호출 체인:
 *   __cache_tag_assign_domain()/__cache_tag_assign_parent_domain()
 *     → [domain_qi_batch_alloc]
 */
static int domain_qi_batch_alloc(struct dmar_domain *domain)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int ret = 0;	/* [한국어] 결과 */

	spin_lock_irqsave(&domain->cache_lock, flags);	/* [한국어] 도메인 상태 변경 구간 */
	if (domain->qi_batch)	/* [한국어] 이미 있으면 */
		goto out_unlock;	/* [한국어] 다시 잡지 않는다 — 배치 버퍼는 도메인당 하나를 공유한다 */

	domain->qi_batch = kzalloc_obj(*domain->qi_batch, GFP_ATOMIC);	/* [한국어] 락을 쥔 채 잡으므로 ATOMIC */
	if (!domain->qi_batch)	/* [한국어] 할당 실패 */
		ret = -ENOMEM;	/* [한국어] 부착을 실패시킨다 — 무효화를 보낼 수 없는 상태로 매핑을 만들 수는 없다 */
out_unlock:	/* [한국어] 이미 있던 경우가 합류 */
	spin_unlock_irqrestore(&domain->cache_lock, flags);	/* [한국어] 락 해제 */

	return ret;	/* [한국어] 결과 */
}

/*
 * [한국어]
 * __cache_tag_assign_domain - 한 (장치, PASID)에 필요한 태그들을 등록한다
 *
 * @domain: 대상 도메인. @did: 이 유닛에서의 도메인 id. @dev: 장치. @pasid: PASID.
 * @return: 0 성공, 음수면 실패(그 경우 아무 태그도 남지 않는다).
 *
 * 필요한 태그가 하나일 수도 둘일 수도 있다.
 *   - IOTLB 는 항상 필요하다. 유닛 안의 번역 캐시는 어느 장치든 쓴다.
 *   - DEVTLB 는 ATS 를 켠 장치에만 필요하다. ATS 가 꺼져 있으면 장치 안에
 *     캐시가 없으므로 비울 것도 없다.
 *
 * 되돌리기가 중요하다: DEVTLB 등록이 실패하면 방금 등록한 IOTLB 태그를 도로
 * 뺀다. 절반만 등록된 상태로 두면 이 (장치, PASID)에 대한 무효화가 불완전해져,
 * 매핑을 풀어도 장치 캐시에 옛 번역이 남는다.
 *
 * 배치 버퍼를 먼저 확보하는 것도 순서상 의미가 있다 — 태그를 등록해 놓고
 * 버퍼가 없어 무효화를 못 보내는 상태를 만들지 않기 위해서다.
 *
 * 실행 컨텍스트: 장치/PASID 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cache_tag_assign_domain() → [__cache_tag_assign_domain]
 *     → domain_qi_batch_alloc() → cache_tag_assign()
 */
static int __cache_tag_assign_domain(struct dmar_domain *domain, u16 did,
				     struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	int ret;	/* [한국어] 각 단계 결과 */

	ret = domain_qi_batch_alloc(domain);	/* [한국어] 먼저 배치 버퍼를 확보한다. 태그를 등록해 놓고 무효화를 못 보내는 상태를 만들지 않기 위해서다 */
	if (ret)	/* [한국어] 실패 */
		return ret;	/* [한국어] 부착 실패 */

	ret = cache_tag_assign(domain, did, dev, pasid, CACHE_TAG_IOTLB);	/* [한국어] 유닛 안의 캐시는 항상 대상이다 */
	if (ret || !info->ats_enabled)	/* [한국어] 실패했거나 ATS 가 꺼져 있으면 */
		return ret;	/* [한국어] 여기서 끝 — 장치 안에 캐시가 없으므로 비울 것도 없다 */

	ret = cache_tag_assign(domain, did, dev, pasid, CACHE_TAG_DEVTLB);	/* [한국어] ATS 를 켠 장치는 자기 캐시도 대상이다 */
	if (ret)	/* [한국어] 실패하면 */
		cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_IOTLB);	/* [한국어] 방금 등록한 IOTLB 태그를 되돌린다. 절반만 등록된 상태는 무효화가 불완전해져 조용한 손상으로 이어진다 */

	return ret;	/* [한국어] 결과 */
}

/*
 * [한국어]
 * __cache_tag_unassign_domain - 그 태그들을 놓는다
 *
 * @domain: 대상 도메인. @did: 도메인 id. @dev: 장치. @pasid: PASID.
 * @return: 없음.
 *
 * assign 의 짝이며, 같은 조건(ats_enabled)으로 갈린다. 켤 때 등록했던 것만
 * 정확히 놓아야 참조 계수의 짝이 맞는다.
 *
 * 주의할 점: ats_enabled 는 이 함수가 불리는 시점의 값이다. 만약 등록 이후
 * ATS 가 꺼졌다면 DEVTLB 태그가 남아 계수가 새게 된다. 그래서 ATS 를 끄는
 * 경로(iommu_disable_pci_ats)는 태그를 먼저 정리하거나, 도메인에서 떨어질
 * 때 함께 처리되도록 순서가 맞춰져 있다.
 *
 * 실행 컨텍스트: 장치/PASID 분리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cache_tag_unassign_domain() → [__cache_tag_unassign_domain]
 *     → cache_tag_unassign()
 */
static void __cache_tag_unassign_domain(struct dmar_domain *domain, u16 did,
					struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */

	cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_IOTLB);	/* [한국어] IOTLB 태그를 놓는다 */

	if (info->ats_enabled)	/* [한국어] ATS 를 켤 때 등록했던 것이 있으면 */
		cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_DEVTLB);	/* [한국어] 그것도 놓는다. 같은 조건으로 갈려야 참조 계수의 짝이 맞는다 */
}

/*
 * [한국어]
 * __cache_tag_assign_parent_domain - 중첩 부모(2단계) 쪽 태그들을 등록한다
 *
 * @domain: 부모 도메인. @did: 도메인 id. @dev: 장치. @pasid: PASID.
 * @return: 0 성공, 음수면 실패.
 *
 * __cache_tag_assign_domain 과 구조가 같고 태그 종류만 NESTING_* 이다.
 *
 * 왜 별도의 종류가 필요한가: 중첩 변환에서 게스트가 1단계 테이블을 고치면
 * 자식 도메인의 태그로 무효화가 나가고, 호스트가 2단계를 고치면 부모의
 * 태그로 나간다. 두 경우에 보내야 할 명령의 형식과 범위가 다르다 — 2단계가
 * 바뀌면 그 위에 얹힌 모든 게스트 매핑이 영향을 받으므로 더 넓은 무효화가
 * 필요하다. 종류를 나눠 두면 무효화 경로가 그 차이를 태그만 보고 안다.
 *
 * 실행 컨텍스트: 중첩 도메인 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cache_tag_assign_domain() (중첩 도메인인 경우)
 *     → [__cache_tag_assign_parent_domain] → cache_tag_assign()
 */
static int __cache_tag_assign_parent_domain(struct dmar_domain *domain, u16 did,
					    struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	int ret;	/* [한국어] 각 단계 결과 */

	ret = domain_qi_batch_alloc(domain);	/* [한국어] 배치 버퍼 확보 */
	if (ret)	/* [한국어] 실패 */
		return ret;	/* [한국어] 부착 실패 */

	ret = cache_tag_assign(domain, did, dev, pasid, CACHE_TAG_NESTING_IOTLB);	/* [한국어] 중첩 부모 쪽 유닛 캐시 */
	if (ret || !info->ats_enabled)	/* [한국어] 실패했거나 ATS 가 꺼져 있으면 */
		return ret;	/* [한국어] 여기서 끝 */

	ret = cache_tag_assign(domain, did, dev, pasid, CACHE_TAG_NESTING_DEVTLB);	/* [한국어] 중첩용 디바이스 TLB */
	if (ret)	/* [한국어] 실패하면 되돌린다 */
		cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_NESTING_IOTLB);	/* [한국어] 실패하면 방금 등록한 것을 되돌린다 */

	return ret;	/* [한국어] 결과 */
}

/*
 * [한국어]
 * __cache_tag_unassign_parent_domain - 중첩 부모 쪽 태그들을 놓는다
 *
 * @domain: 부모 도메인. @did: 도메인 id. @dev: 장치. @pasid: PASID.
 * @return: 없음.
 *
 * assign 의 짝이며 __cache_tag_unassign_domain 과 구조가 같다. 종류만
 * NESTING_* 이다.
 *
 * 실행 컨텍스트: 중첩 도메인 분리. 프로세스 컨텍스트.
 */
static void __cache_tag_unassign_parent_domain(struct dmar_domain *domain, u16 did,
					       struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */

	cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_NESTING_IOTLB);	/* [한국어] 중첩 부모 쪽 유닛 캐시 태그를 놓는다 */

	if (info->ats_enabled)	/* [한국어] ATS 를 켤 때 등록했던 것이 있으면 */
		cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_NESTING_DEVTLB);	/* [한국어] 그것도 놓는다 */
}

/*
 * [한국어]
 * domain_get_id_for_dev - 이 도메인이 이 장치의 유닛에서 쓸 도메인 id 를 구한다
 *
 * @domain: 대상 도메인. @dev: 그 도메인에 붙는 장치.
 * @return: 무효화 명령에 실릴 도메인 id.
 *
 * SVA 도메인만 예외다(위 영어 주석). SVA 는 자기 페이지 테이블을 갖지 않고
 * 프로세스의 것을 그대로 가리키므로 도메인 id 를 할당받지 않는다. 대신
 * 1단계·통과 전용으로 예약된 FLPT_DEFAULT_DID 를 쓴다.
 *
 * 나머지 도메인은 유닛마다 따로 할당된 id 를 domain_id_iommu() 로 얻는다 —
 * 같은 도메인이라도 유닛이 다르면 번호가 다르다는 점이 여기서 드러난다.
 *
 * 실행 컨텍스트: 태그 등록·해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cache_tag_assign_domain()/unassign_domain() → [domain_get_id_for_dev]
 */
static u16 domain_get_id_for_dev(struct dmar_domain *domain, struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */

	/*
	 * The driver assigns different domain IDs for all domains except
	 * the SVA type.
	 */
	if (domain->domain.type == IOMMU_DOMAIN_SVA)	/* [한국어] SVA 도메인만 예외다 (위 영어 주석) */
		return FLPT_DEFAULT_DID;	/* [한국어] 도메인 id 를 할당받지 않으므로 예약된 값을 쓴다 */

	return domain_id_iommu(domain, iommu);	/* [한국어] 나머지는 이 유닛에서 할당된 id */
}

/*
 * Assign cache tags to a domain when it's associated with a device's
 * PASID using a specific domain ID.
 *
 * On success (return value of 0), cache tags are created and added to the
 * domain's cache tag list. On failure (negative return value), an error
 * code is returned indicating the reason for the failure.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tag_assign_domain - 장치의 한 PASID 에 필요한 무효화 대상을 모두 등록한다
 *
 * @domain: 붙는 도메인. @dev: 장치. @pasid: PASID.
 * @return: 0 성공, 음수면 실패(그 경우 아무 태그도 남지 않는다).
 *
 * 이 파일의 대외 진입점 중 하나로, 장치가 도메인에 붙을 때 iommu.c 가 부른다.
 *
 * 중첩 도메인이면 두 벌을 등록한다. 게스트가 1단계 테이블을 고치면 자식
 * 도메인의 태그로, 호스트가 2단계를 고치면 부모의 NESTING_* 태그로 무효화가
 * 나가야 하기 때문이다. 부모 쪽 등록이 실패하면 자식 쪽을 되돌려, 절반만
 * 등록된 상태를 남기지 않는다.
 *
 * 도메인 id 는 자식 것을 쓴다는 점을 눈여겨볼 것: 부모 태그에도 같은 did 를
 * 넘긴다. 중첩에서 하드웨어는 자식 도메인의 id 로 두 단계를 모두 식별하기
 * 때문이다.
 *
 * 실행 컨텍스트: 장치/PASID 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dmar_domain_attach_device()/domain_add_dev_pasid()
 *     → [cache_tag_assign_domain] → __cache_tag_assign_domain()
 *     → __cache_tag_assign_parent_domain()
 */
int cache_tag_assign_domain(struct dmar_domain *domain,
			    struct device *dev, ioasid_t pasid)
{
	u16 did = domain_get_id_for_dev(domain, dev);	/* [한국어] 이 유닛에서 쓸 도메인 id */
	int ret;	/* [한국어] 결과 */

	ret = __cache_tag_assign_domain(domain, did, dev, pasid);	/* [한국어] 먼저 이 도메인 쪽 태그들 */
	if (ret || domain->domain.type != IOMMU_DOMAIN_NESTED)	/* [한국어] 실패했거나 중첩이 아니면 */
		return ret;	/* [한국어] 여기서 끝 */

	ret = __cache_tag_assign_parent_domain(domain->s2_domain, did, dev, pasid);	/* [한국어] 중첩이면 부모(2단계) 쪽 태그도 등록한다. 자식의 did 를 그대로 쓰는 것은 하드웨어가 자식 도메인 id 로 두 단계를 모두 식별하기 때문이다 */
	if (ret)	/* [한국어] 실패하면 */
		__cache_tag_unassign_domain(domain, did, dev, pasid);	/* [한국어] 자식 쪽을 되돌린다 */

	return ret;	/* [한국어] 결과 */
}

/*
 * Remove the cache tags associated with a device's PASID when the domain is
 * detached from the device.
 *
 * The cache tags must be previously assigned to the domain by calling the
 * assign interface.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tag_unassign_domain - 그 대상들을 모두 놓는다
 *
 * @domain: 떨어지는 도메인. @dev: 장치. @pasid: PASID.
 * @return: 없음.
 *
 * assign 의 짝. 같은 조건으로 갈리므로 등록했던 것만 정확히 놓는다.
 *
 * 순서상 중요한 점: 이 함수는 하드웨어 항목을 내리기 "전에" 불려야 한다.
 * device_block_translation() 이 cache_tag_unassign_domain 을 먼저 부르고
 * 그 다음 PASID/컨텍스트 항목을 내리는 이유가 그것이다 — 먼저 대상 목록에서
 * 빼야 그 사이의 도메인 단위 무효화가 이미 떨어져 나갈 장치를 건드리지 않는다.
 *
 * 실행 컨텍스트: 장치/PASID 분리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   device_block_translation()/domain_remove_dev_pasid()
 *     → [cache_tag_unassign_domain]
 */
void cache_tag_unassign_domain(struct dmar_domain *domain,
			       struct device *dev, ioasid_t pasid)
{
	u16 did = domain_get_id_for_dev(domain, dev);	/* [한국어] 이 유닛에서의 도메인 id */

	__cache_tag_unassign_domain(domain, did, dev, pasid);	/* [한국어] 이 도메인 쪽 태그들을 놓고 */
	if (domain->domain.type == IOMMU_DOMAIN_NESTED)	/* [한국어] 중첩이면 */
		__cache_tag_unassign_parent_domain(domain->s2_domain, did, dev, pasid);	/* [한국어] 부모 쪽도 놓는다 */
}

/*
 * [한국어]
 * calculate_psi_aligned_address - 페이지 선택 무효화(PSI)가 요구하는 정렬을 맞춘다
 *
 * @start: 무효화할 범위의 시작 주소. @end: 끝 주소(포함).
 * @_mask: 출력 — 쓸 범위 크기의 로그값.
 * @return: 정렬을 맞춘 시작 주소.
 *
 * 이 파일에서 가장 조심스러운 계산이다. PSI(Page-Selective Invalidation)는
 * "주소의 하위 mask 비트를 무시하고, 그 크기만큼의 범위를 지운다"는 방식이라,
 * 주소가 그 크기에 정렬되어 있지 않으면 의도한 범위를 덮지 못한다.
 * 좁게 지우면 옛 번역이 남고, 그 페이지가 재사용되면 조용한 메모리 손상이 된다.
 *
 * 기본 계산: 필요한 페이지 수를 2의 거듭제곱으로 올림하고, 그 로그값을 mask 로
 * 삼는다. 시작 주소가 그 크기에 정렬되어 있다면 이것으로 끝이다.
 *
 * 정렬되지 않았을 때가 어렵다. 예를 들어 pfn=3, 2페이지를 지우려면 mask=1
 * (2페이지)로는 [2,3] 만 덮여 4번 페이지를 놓친다. 그래서 범위 전체를 덮을
 * 만큼 mask 를 키워야 하는데, 얼마나 키울지를 다음 관찰로 구한다(위 영어 주석):
 *   end_pfn <= pfn + bitmask 이므로, bitmask 위쪽 비트에서 pfn 과 end_pfn 이
 *   다를 수 있는 유일한 이유는 자리올림이다. 따라서 bitmask 를 뺀 뒤 두 값이
 *   일치하는 상위 비트들 중 가장 낮은 자리(__ffs(shared_bits))가, 그 범위를
 *   한 번에 덮을 수 있는 최소 크기다.
 * shared_bits 가 0 이면 두 값이 어느 상위 비트에서도 일치하지 않는다는 뜻이라
 * 주소 공간 전체(MAX_AGAW_PFN_WIDTH)를 덮는 수밖에 없다.
 *
 * 결과적으로 무효화 범위는 요청보다 넓어질 수 있지만 결코 좁아지지 않는다 —
 * 넓은 것은 성능 손해일 뿐이고, 좁은 것은 정확성 버그이기 때문이다.
 *
 * 실행 컨텍스트: 무효화 경로. 순수 계산이라 잠들지 않는다.
 *
 * 호출 체인:
 *   cache_tag_flush_range() → [calculate_psi_aligned_address]
 */
static unsigned long calculate_psi_aligned_address(unsigned long start,
						   unsigned long end,
						   unsigned long *_mask)
{
	unsigned long pages = aligned_nrpages(start, end - start + 1);	/* [한국어] 덮어야 할 페이지 수. 시작이 페이지 중간이면 한 페이지를 더 센다 */
	unsigned long aligned_pages = __roundup_pow_of_two(pages);	/* [한국어] PSI 는 2의 거듭제곱 크기만 다룰 수 있어 올림한다 */
	unsigned long bitmask = aligned_pages - 1;	/* [한국어] 그 크기가 무시하는 하위 비트들 */
	unsigned long mask = ilog2(aligned_pages);	/* [한국어] 크기의 로그값 — 서술자에 실릴 값이다 */
	unsigned long pfn = IOVA_PFN(start);	/* [한국어] 시작 페이지 프레임 번호 */

	/*
	 * PSI masks the low order bits of the base address. If the
	 * address isn't aligned to the mask, then compute a mask value
	 * needed to ensure the target range is flushed.
	 */
	if (unlikely(bitmask & pfn)) {	/* [한국어] 시작 주소가 그 크기에 정렬되어 있지 않으면 (위 영어 주석) */
		unsigned long end_pfn = pfn + pages - 1, shared_bits;	/* [한국어] 끝 페이지 번호와, 두 값이 공유하는 상위 비트 */

		/*
		 * Since end_pfn <= pfn + bitmask, the only way bits
		 * higher than bitmask can differ in pfn and end_pfn is
		 * by carrying. This means after masking out bitmask,
		 * high bits starting with the first set bit in
		 * shared_bits are all equal in both pfn and end_pfn.
		 */
		shared_bits = ~(pfn ^ end_pfn) & ~bitmask;	/* [한국어] bitmask 위쪽에서 시작과 끝이 일치하는 비트들. 위 영어 주석의 관찰 — 그 위에서 다를 수 있는 유일한 이유가 자리올림이므로, 일치하는 가장 낮은 비트가 곧 범위를 덮는 최소 크기다 */
		mask = shared_bits ? __ffs(shared_bits) : MAX_AGAW_PFN_WIDTH;	/* [한국어] 그 비트 위치를 새 mask 로 삼는다. 일치하는 비트가 없으면 주소 공간 전체를 덮는 수밖에 없다 */
	}

	*_mask = mask;	/* [한국어] 호출자에게 크기를 돌려준다 */

	return ALIGN_DOWN(start, VTD_PAGE_SIZE << mask);	/* [한국어] 그 크기에 맞춰 내림 정렬한 시작 주소. 범위가 넓어질 수는 있어도 좁아지지는 않는다 */
}

/*
 * [한국어]
 * qi_batch_flush_descs - 모아 둔 서술자들을 유닛의 무효화 큐에 한 번에 제출한다
 *
 * @iommu: 대상 유닛. NULL 이면 아무것도 하지 않는다.
 * @batch: 제출할 배치 버퍼.
 * @return: 없음.
 *
 * 배치의 존재 이유가 이 함수에 있다. qi_submit_sync 는 큐 락을 잡고 tail 을
 * 쓰고 완료를 기다리는 값비싼 왕복인데, 서술자 하나마다 그것을 하면 큰 범위를
 * 언매핑할 때 그 왕복 비용이 전부가 된다. 최대 16개를 모아 한 번에 보내면
 * 락 획득과 완료 대기가 한 번으로 줄어든다.
 *
 * iommu 가 NULL 이거나 담긴 것이 없으면 곧바로 돌아간다. 호출부가 "혹시 남은
 * 것이 있으면 보내라"는 식으로 무조건 부를 수 있게 하기 위한 방어다.
 *
 * 제출 뒤 버퍼 전체를 memset 하는 이유(위 영어 주석): 인덱스만 0 으로
 * 되돌리면 이전 배치의 서술자 내용이 남는다. 다음 배치가 그 자리를 다 채우지
 * 않은 채 제출되면 하드웨어가 남은 쓰레기를 읽을 수 있으므로 통째로 지운다.
 *
 * 실행 컨텍스트: 무효화 경로. qi_submit_sync 안에서 완료를 기다리므로
 * 시간이 걸릴 수 있지만 잠들지는 않는다.
 *
 * 호출 체인:
 *   cache_tag_flush_range() 등 → [qi_batch_flush_descs] → qi_submit_sync()
 */
static void qi_batch_flush_descs(struct intel_iommu *iommu, struct qi_batch *batch)
{
	if (!iommu || !batch->index)	/* [한국어] 유닛이 없거나 담긴 것이 없으면 */
		return;	/* [한국어] 보낼 것이 없다. 호출부가 무조건 부를 수 있게 하는 방어다 */

	qi_submit_sync(iommu, batch->descs, batch->index, 0);	/* [한국어] 담긴 만큼을 한 번에 제출하고 완료까지 기다린다 */

	/* Reset the index value and clean the whole batch buffer. */
	memset(batch, 0, sizeof(*batch));	/* [한국어] 인덱스뿐 아니라 버퍼 전체를 지운다. 이전 내용이 남아 있으면 다음 배치에서 하드웨어가 쓰레기를 읽을 수 있다 (위 영어 주석) */
}

/*
 * [한국어]
 * qi_batch_increment_index - 배치에 하나를 채웠음을 기록하고, 가득 차면 제출한다
 *
 * @iommu: 대상 유닛. @batch: 배치 버퍼.
 * @return: 없음.
 *
 * 모든 qi_batch_add_* 가 서술자를 채운 뒤 이 함수를 부른다. 인덱스를 하나
 * 늘리고, 버퍼가 가득 찼으면(QI_MAX_BATCHED_DESC_COUNT) 그 자리에서 제출해
 * 자리를 비운다.
 *
 * 이 자동 제출 덕분에 호출부는 "몇 개나 담겼는지"를 신경 쓰지 않고 계속
 * 추가하기만 하면 된다. 마지막에 남은 것만 명시적으로 flush 하면 된다.
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static void qi_batch_increment_index(struct intel_iommu *iommu, struct qi_batch *batch)
{
	if (++batch->index == QI_MAX_BATCHED_DESC_COUNT)	/* [한국어] 하나 늘렸는데 가득 찼으면 */
		qi_batch_flush_descs(iommu, batch);	/* [한국어] 그 자리에서 제출해 자리를 비운다 */
}

/*
 * [한국어]
 * qi_batch_add_iotlb - IOTLB 무효화 서술자를 배치에 담는다
 *
 * @iommu: 대상 유닛. @did: 도메인 id. @addr: 주소. @size_order: 범위 크기.
 * @type: 무효화 범위 종류(DMA_TLB_*_FLUSH). @batch: 담을 배치.
 * @return: 없음.
 *
 * iommu.h 의 qi_desc_iotlb 로 서술자를 조립해 배치의 현재 자리에 채우고
 * 인덱스를 민다. "조립"과 "담기"를 나눈 구조라, 조립 규칙(비트 배치)은
 * 헤더에, 배치 관리는 이 파일에 있다.
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static void qi_batch_add_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
			       unsigned int size_order, u64 type,
			       struct qi_batch *batch)
{
	qi_desc_iotlb(iommu, did, addr, size_order, type, &batch->descs[batch->index]);	/* [한국어] 현재 자리에 서술자를 조립해 넣는다 */
	qi_batch_increment_index(iommu, batch);	/* [한국어] 인덱스를 밀고, 가득 찼으면 제출한다 */
}

/*
 * [한국어]
 * qi_batch_add_dev_iotlb - 디바이스 TLB 무효화 서술자를 배치에 담는다
 *
 * @iommu: 대상 유닛. @sid: 장치 소스 id. @pfsid: PF 소스 id.
 * @qdep: ATS 큐 깊이. @addr: 주소. @mask: 범위 크기. @batch: 담을 배치.
 * @return: 없음.
 *
 * 번역이 꺼져 있으면 아무것도 담지 않는다(위 영어 주석). 스펙이 "주소 재매핑
 * 하드웨어가 꺼진 동안에는 디바이스 TLB 무효화를 보내지 말라"고 권하기
 * 때문이다. 번역이 꺼진 상태에서는 장치 캐시에 있을 번역 자체가 의미가 없고,
 * 그 요청이 어떻게 처리될지도 정의되어 있지 않다.
 *
 * gcmd 의 TE 비트로 확인하는 것을 눈여겨볼 것 — GCMD 레지스터는 읽어도
 * 현재 설정을 돌려주지 않으므로, 유닛이 들고 있는 소프트웨어 사본을 본다.
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static void qi_batch_add_dev_iotlb(struct intel_iommu *iommu, u16 sid, u16 pfsid,
				   u16 qdep, u64 addr, unsigned int mask,
				   struct qi_batch *batch)
{
	/*
	 * According to VT-d spec, software is recommended to not submit any Device-TLB
	 * invalidation requests while address remapping hardware is disabled.
	 */
	if (!(iommu->gcmd & DMA_GCMD_TE))	/* [한국어] 번역이 꺼져 있으면 (위 영어 주석) */
		return;	/* [한국어] 디바이스 TLB 무효화를 보내지 않는다. 꺼진 동안의 동작이 정의되어 있지 않다 */

	qi_desc_dev_iotlb(sid, pfsid, qdep, addr, mask, &batch->descs[batch->index]);	/* [한국어] 서술자를 조립해 넣는다 */
	qi_batch_increment_index(iommu, batch);	/* [한국어] 인덱스를 민다 */
}

/*
 * [한국어]
 * qi_batch_add_piotlb_all - 한 PASID 의 IOTLB 전체를 비우는 서술자를 담는다
 *
 * @iommu: 대상 유닛. @did: 도메인 id. @pasid: 대상 PASID. @batch: 담을 배치.
 * @return: 없음.
 *
 * 범위를 지정하지 않고 그 PASID 에 속한 항목을 통째로 지운다. 그 주소 공간이
 * 통째로 사라질 때(SVA 에서 프로세스 종료, PASID 회수) 쓰며, 범위를 하나하나
 * 지우는 것보다 훨씬 싸다.
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static void qi_batch_add_piotlb_all(struct intel_iommu *iommu, u16 did,
				    u32 pasid, struct qi_batch *batch)
{
	qi_desc_piotlb_all(did, pasid, &batch->descs[batch->index]);	/* [한국어] PASID 전체 무효화 서술자 */
	qi_batch_increment_index(iommu, batch);	/* [한국어] 인덱스를 민다 */
}

/*
 * [한국어]
 * qi_batch_add_piotlb - 한 PASID 안의 특정 범위를 비우는 서술자를 담는다
 *
 * @iommu: 대상 유닛. @did: 도메인 id. @pasid: PASID. @addr: 주소.
 * @size_order: 범위 크기. @ih: 중간 단계 유지 힌트. @batch: 담을 배치.
 * @return: 없음.
 *
 * addr 과 size_order 는 calculate_psi_aligned_address 가 계산한 값이어야
 * 한다 — 정렬이 어긋나면 무효화 범위가 조용히 틀어진다.
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static void qi_batch_add_piotlb(struct intel_iommu *iommu, u16 did, u32 pasid,
				u64 addr, unsigned int size_order, bool ih,
				struct qi_batch *batch)
{
	qi_desc_piotlb(did, pasid, addr, size_order, ih,	/* [한국어] PASID 안의 범위 무효화 서술자 */
		       &batch->descs[batch->index]);	/* [한국어] 현재 자리에 */
	qi_batch_increment_index(iommu, batch);	/* [한국어] 인덱스를 민다 */
}

/*
 * [한국어]
 * qi_batch_add_pasid_dev_iotlb - PASID 를 지정한 디바이스 TLB 무효화를 배치에 담는다
 *
 * @iommu: 대상 유닛. @sid: 장치 소스 id. @pfsid: PF 소스 id. @pasid: 대상 PASID.
 * @qdep: ATS 큐 깊이. @addr: 주소. @size_order: 범위 크기. @batch: 담을 배치.
 * @return: 없음.
 *
 * qi_batch_add_dev_iotlb 의 PASID 인식 판. SVA 처럼 장치가 여러 주소 공간을
 * 동시에 쓰는 경우, 한 PASID 의 캐시만 지워야 나머지가 살아남는다.
 *
 * 번역이 꺼져 있으면 보내지 않는 규칙도 같다(위 영어 주석).
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static void qi_batch_add_pasid_dev_iotlb(struct intel_iommu *iommu, u16 sid, u16 pfsid,
					 u32 pasid,  u16 qdep, u64 addr,
					 unsigned int size_order, struct qi_batch *batch)
{
	/*
	 * According to VT-d spec, software is recommended to not submit any
	 * Device-TLB invalidation requests while address remapping hardware
	 * is disabled.
	 */
	if (!(iommu->gcmd & DMA_GCMD_TE))	/* [한국어] 번역이 꺼져 있으면 (위 영어 주석) */
		return;	/* [한국어] 디바이스 TLB 무효화를 보내지 않는다 */

	qi_desc_dev_iotlb_pasid(sid, pfsid, pasid, qdep, addr, size_order,	/* [한국어] PASID 를 지정한 서술자를 조립해 */
				&batch->descs[batch->index]);	/* [한국어] 현재 자리에 넣는다 */
	qi_batch_increment_index(iommu, batch);	/* [한국어] 인덱스를 민다 */
}

/*
 * [한국어]
 * intel_domain_use_piotlb - 이 도메인이 PASID 인식 IOTLB 무효화를 써야 하는지
 *
 * @domain: 검사할 도메인.
 * @return: true 면 확장(PASID 인식) 형식으로 무효화해야 한다.
 *
 * 세 종류가 해당한다.
 *   SVA        — 프로세스 주소 공간을 PASID 로 구분한다.
 *   NESTED     — 게스트의 1단계 테이블이 PASID 항목에 매달려 있다.
 *   1단계 페이징 — scalable 모드에서만 존재하며, 번역이 PASID 항목에서 시작한다.
 * 공통점은 번역의 뿌리가 PASID 항목이라는 것이다. 그런 도메인의 캐시 항목은
 * PASID 로 태그되어 있으므로, PASID 를 명시하지 않는 옛 형식 무효화로는
 * 지워지지 않는다.
 *
 * 2단계 페이징 도메인은 레거시 모드에서도 쓰이고 번역이 컨텍스트 항목에서
 * 시작하므로 옛 형식으로 지운다.
 *
 * 실행 컨텍스트: 무효화 경로. 순수 조회.
 *
 * 호출 체인:
 *   cache_tag_flush_iotlb() → [intel_domain_use_piotlb]
 */
static bool intel_domain_use_piotlb(struct dmar_domain *domain)
{
	return domain->domain.type == IOMMU_DOMAIN_SVA ||	/* [한국어] SVA 이거나 */
			domain->domain.type == IOMMU_DOMAIN_NESTED ||	/* [한국어] 중첩이거나 */
			intel_domain_is_fs_paging(domain);	/* [한국어] 1단계 페이징이면 참. 셋 다 번역이 PASID 항목에서 시작하므로 캐시 항목이 PASID 로 태그된다 */
}

/*
 * [한국어]
 * cache_tag_flush_iotlb - 유닛 안의 번역 캐시를 비우는 명령을 만든다
 *
 * @domain: 대상 도메인. @tag: 어느 유닛의 어느 도메인 id 인지.
 * @addr: 정렬된 시작 주소. @mask: 범위 크기의 로그값. @ih: 중간 단계 유지 힌트.
 * @return: 없음(배치에 담기만 한다).
 *
 * 세 갈래로 나뉘며, 각 갈래가 하드웨어의 한계를 하나씩 다룬다.
 *
 *   [1] PASID 인식 형식이 필요한 도메인(SVA/중첩/1단계)이면 확장 형식으로
 *       보낸다. mask 가 주소 공간 전체를 뜻할 만큼 크면 범위 지정을 포기하고
 *       그 PASID 전체를 비운다 — 어차피 같은 결과인데 명령 하나로 끝난다.
 *
 *   [2] 페이지 선택 무효화를 지원하지 않거나(!cap_pgsel_inv), 요청한 범위가
 *       하드웨어가 한 번에 다룰 수 있는 크기를 넘으면(mask > cap_max_amask_val),
 *       도메인 전체 무효화로 승격한다(위 영어 주석). 주소·크기·힌트를 모두
 *       0 으로 되돌리는 것이 그 승격이다. 넓게 지우는 것은 성능 손해일 뿐이고,
 *       요청한 범위를 못 덮는 것은 정확성 버그이기 때문이다.
 *
 *   [3] 무효화 큐를 쓸 수 있으면 배치에 담고, 아니면 레지스터 방식으로
 *       그 자리에서 보낸다. 옛 하드웨어와 초기화 단계를 위한 폴백이다.
 *
 * addr | ih 로 힌트를 주소에 실어 보내는 것은 하위 비트가 비어 있음을 이용한
 * 관용구다(qi_desc_iotlb 가 다시 꺼낸다).
 *
 * 실행 컨텍스트: 무효화 경로. 잠들면 안 된다.
 *
 * 호출 체인:
 *   cache_tag_flush_range()/flush_all() → [cache_tag_flush_iotlb]
 *     → qi_batch_add_piotlb()/qi_batch_add_iotlb()/__iommu_flush_iotlb()
 */
static void cache_tag_flush_iotlb(struct dmar_domain *domain, struct cache_tag *tag,
				  unsigned long addr, unsigned long mask, int ih)
{
	struct intel_iommu *iommu = tag->iommu;	/* [한국어] 이 태그가 가리키는 유닛 */
	u64 type = DMA_TLB_PSI_FLUSH;	/* [한국어] 기본은 페이지 선택 무효화 */

	if (intel_domain_use_piotlb(domain)) {	/* [한국어] PASID 인식 형식이 필요한 도메인이면 */
		if (mask >= MAX_AGAW_PFN_WIDTH)	/* [한국어] 범위가 주소 공간 전체만큼 크면 */
			qi_batch_add_piotlb_all(iommu, tag->domain_id,	/* [한국어] 범위 지정을 포기하고 */
						tag->pasid, domain->qi_batch);	/* [한국어] 그 PASID 전체를 비운다 — 같은 결과를 명령 하나로 끝낸다 */
		else
			qi_batch_add_piotlb(iommu, tag->domain_id, tag->pasid,	/* [한국어] 아니면 범위를 지정해 */
					    addr, mask, ih, domain->qi_batch);	/* [한국어] 확장 형식으로 담는다 */
		return;	/* [한국어] 확장 형식 경로는 여기서 끝 */
	}

	/*
	 * Fallback to domain selective flush if no PSI support or the size
	 * is too big.
	 */
	if (!cap_pgsel_inv(iommu->cap) ||	/* [한국어] 페이지 선택 무효화를 지원하지 않거나 (위 영어 주석) */
	    mask > cap_max_amask_val(iommu->cap)) {	/* [한국어] 요청 범위가 하드웨어 한계를 넘으면 */
		addr = 0;	/* [한국어] 주소를 지우고 */
		mask = 0;	/* [한국어] 범위도 지우고 */
		ih = 0;	/* [한국어] 힌트도 지운 뒤 */
		type = DMA_TLB_DSI_FLUSH;	/* [한국어] 도메인 전체 무효화로 승격한다. 넓게 지우는 것은 성능 손해지만, 요청 범위를 못 덮는 것은 정확성 버그다 */
	}

	if (ecap_qis(iommu->ecap))	/* [한국어] 무효화 큐를 쓸 수 있으면 */
		qi_batch_add_iotlb(iommu, tag->domain_id, addr | ih, mask, type,	/* [한국어] 배치에 담고 (힌트는 주소의 빈 하위 비트에 실어 보낸다) */
				   domain->qi_batch);	/* [한국어] 그 도메인의 배치 버퍼에 */
	else
		__iommu_flush_iotlb(iommu, tag->domain_id, addr | ih, mask, type);	/* [한국어] 큐가 없으면 레지스터 방식으로 그 자리에서 보낸다 */
}

/*
 * [한국어]
 * cache_tag_flush_devtlb_psi - 장치 안의 번역 캐시에서 특정 범위를 비운다
 *
 * @domain: 대상 도메인. @tag: 어느 장치인지(DEVTLB 태그).
 * @addr: 정렬된 시작 주소. @mask: 범위 크기의 로그값.
 * @return: 없음(배치에 담기만 한다).
 *
 * ATS 를 켠 장치는 번역 결과를 자기 안에 캐시하므로, 유닛의 IOTLB 를 비우는
 * 것만으로는 부족하다. 이 함수가 그 장치 쪽 캐시를 비우는 명령을 만든다.
 *
 * PASID 유무로 형식이 갈린다. IOMMU_NO_PASID 면 기본 형식, 아니면 PASID 를
 * 명시한 확장 형식이다 — 장치 안의 캐시 항목도 PASID 로 태그되어 있어서다.
 *
 * dtlb_extra_inval 인 장치에는 같은 명령을 한 번 더 담는다. 그 장치들은 ATS
 * 무효화 완료 응답을, 그 범위의 번역을 이미 써서 발행한 posted write 보다
 * 먼저 보내는 결함이 있다. 두 번째 무효화의 완료를 기다리는 동안 첫 번째
 * 이후에 발행된 쓰기가 모두 도착하므로 순서 보장이 회복된다 — 그러지 않으면
 * 커널이 완료를 믿고 해제한 페이지에 뒤늦은 쓰기가 도착한다.
 *
 * 실행 컨텍스트: 무효화 경로. 잠들면 안 된다.
 *
 * 호출 체인:
 *   cache_tag_flush_range() → [cache_tag_flush_devtlb_psi]
 *     → qi_batch_add_dev_iotlb()/qi_batch_add_pasid_dev_iotlb()
 */
static void cache_tag_flush_devtlb_psi(struct dmar_domain *domain, struct cache_tag *tag,
				       unsigned long addr, unsigned long mask)
{
	struct intel_iommu *iommu = tag->iommu;	/* [한국어] 이 태그가 가리키는 유닛 */
	struct device_domain_info *info;	/* [한국어] 장치 정보 */
	u16 sid;	/* [한국어] 그 장치의 소스 id */

	info = dev_iommu_priv_get(tag->dev);	/* [한국어] DEVTLB 태그의 dev 는 실제 장치를 가리킨다 */
	sid = PCI_DEVID(info->bus, info->devfn);	/* [한국어] 버스와 devfn 을 16비트 소스 id 로 */

	if (tag->pasid == IOMMU_NO_PASID) {	/* [한국어] PASID 를 쓰지 않는 기본 트래픽이면 */
		qi_batch_add_dev_iotlb(iommu, sid, info->pfsid, info->ats_qdep,	/* [한국어] 기본 형식으로 담고 */
				       addr, mask, domain->qi_batch);	/* [한국어] 범위를 지정한다 */
		if (info->dtlb_extra_inval)	/* [한국어] 완료 순서 결함이 있는 장치면 */
			qi_batch_add_dev_iotlb(iommu, sid, info->pfsid, info->ats_qdep,	/* [한국어] 같은 명령을 한 번 더 담는다. 두 번째의 완료를 기다리는 동안 첫 번째 이후의 쓰기가 모두 도착해 순서 보장이 회복된다 */
					       addr, mask, domain->qi_batch);	/* [한국어] 같은 범위로 */
		return;	/* [한국어] 기본 형식 경로는 여기서 끝 */
	}

	qi_batch_add_pasid_dev_iotlb(iommu, sid, info->pfsid, tag->pasid,	/* [한국어] PASID 트래픽이면 확장 형식으로 */
				     info->ats_qdep, addr, mask, domain->qi_batch);	/* [한국어] 같은 범위를 담는다 */
	if (info->dtlb_extra_inval)	/* [한국어] 결함 장치면 */
		qi_batch_add_pasid_dev_iotlb(iommu, sid, info->pfsid, tag->pasid,	/* [한국어] 여기서도 한 번 더 담는다 */
					     info->ats_qdep, addr, mask,	/* [한국어] 같은 인자로 */
					     domain->qi_batch);	/* [한국어] 같은 배치에 */
}

/*
 * Invalidates a range of IOVA from @start (inclusive) to @end (inclusive)
 * when the memory mappings in the target domain have been modified.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tag_flush_range - 이 도메인의 모든 캐시에서 주어진 IOVA 범위를 비운다
 *
 * @domain: 대상 도메인. @start: 시작 주소(포함). @end: 끝 주소(포함).
 * @ih: 중간 단계 항목은 그대로 두라는 힌트. 매핑만 바뀌고 페이지 테이블
 *      구조는 그대로일 때 1 이다.
 * @return: 없음. 명령이 큐에 제출되고 완료를 기다린 뒤 돌아온다.
 *
 * 이 파일의 핵심 함수다. 매핑이 바뀌었다는 사실 하나로부터, 비워야 할 모든
 * 캐시에 알맞은 명령을 만들어 보낸다.
 *
 * 전체 범위(0~ULONG_MAX)는 계산을 건너뛴다. 그 경우 정렬을 맞출 것도 없이
 * 주소 공간 전체를 뜻하는 mask 를 바로 쓴다 — cache_tag_flush_all 이 이
 * 경로로 들어온다.
 *
 * 태그를 훑으며 유닛이 바뀌는 순간 배치를 제출하는 것이 중요하다. 배치는
 * 하나의 큐에 보낼 서술자만 담을 수 있으므로, 다음 유닛의 명령을 담기 전에
 * 지금까지의 것을 비워야 한다. cache_tag_assign 이 같은 유닛의 태그를
 * 목록에서 이웃하게 넣어 두는 이유가 여기 있다 — 흩어져 있으면 유닛이 바뀔
 * 때마다 제출해야 해서 묶음이 잘게 쪼개진다.
 *
 * NESTING_DEVTLB 의 특별 처리(코드 안 영어 주석): 장치 안의 캐시는 중첩
 * 변환의 "결과"를 담고 있어서, 2단계가 바뀌었을 때 그 영향을 받는 항목이
 * 정확히 무엇인지 알 방법이 없다. 그래서 범위를 포기하고 그 장치의 캐시를
 * 통째로 비운다. fallthrough 로 DEVTLB 경로를 그대로 타는 것이 그 구현이다.
 *
 * 동기화: cache_lock 을 쥔 채 순회부터 제출까지 한다. 그래야 순회 도중
 * 태그가 사라져 해제된 메모리를 읽는 일이 없다.
 * 실행 컨텍스트: 언매핑 후처리, SVA 의 mmu_notifier. 잠들면 안 된다.
 *
 * 호출 체인:
 *   intel_iommu_tlb_sync()/SVA notifier → [cache_tag_flush_range]
 *     → calculate_psi_aligned_address() → cache_tag_flush_iotlb()
 *     → cache_tag_flush_devtlb_psi() → qi_batch_flush_descs()
 */
void cache_tag_flush_range(struct dmar_domain *domain, unsigned long start,
			   unsigned long end, int ih)
{
	struct intel_iommu *iommu = NULL;	/* [한국어] 직전에 처리한 유닛. 유닛이 바뀌는 순간을 잡아 배치를 제출한다 */
	unsigned long mask, addr;	/* [한국어] 정렬을 맞춘 범위 */
	struct cache_tag *tag;	/* [한국어] 순회 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	if (start == 0 && end == ULONG_MAX) {	/* [한국어] 전체 범위 요청이면 */
		addr = 0;	/* [한국어] 정렬 계산 없이 */
		mask = MAX_AGAW_PFN_WIDTH;	/* [한국어] 주소 공간 전체를 뜻하는 크기를 바로 쓴다 */
	} else {
		addr = calculate_psi_aligned_address(start, end, &mask);	/* [한국어] 아니면 PSI 가 요구하는 정렬을 맞춘다 */
	}

	spin_lock_irqsave(&domain->cache_lock, flags);	/* [한국어] 태그 목록 순회 구간 */
	list_for_each_entry(tag, &domain->cache_tags, node) {	/* [한국어] 비워야 할 캐시들을 훑으며 */
		if (iommu && iommu != tag->iommu)	/* [한국어] 유닛이 바뀌었으면 */
			qi_batch_flush_descs(iommu, domain->qi_batch);	/* [한국어] 지금까지의 배치를 먼저 제출한다. 배치는 한 유닛의 큐에만 보낼 수 있기 때문이다 */
		iommu = tag->iommu;	/* [한국어] 현재 유닛을 기억한다 */

		switch (tag->type) {	/* [한국어] 태그 종류에 따라 */
		case CACHE_TAG_IOTLB:	/* [한국어] 유닛 안의 캐시 */
		case CACHE_TAG_NESTING_IOTLB:	/* [한국어] 중첩 부모 쪽 유닛 캐시 */
			cache_tag_flush_iotlb(domain, tag, addr, mask, ih);	/* [한국어] 같은 방식으로 처리한다 */
			break;	/* [한국어] 다음 태그 */
		case CACHE_TAG_NESTING_DEVTLB:	/* [한국어] 중첩용 디바이스 TLB 는 특별하다 */
			/*
			 * Address translation cache in device side caches the
			 * result of nested translation. There is no easy way
			 * to identify the exact set of nested translations
			 * affected by a change in S2. So just flush the entire
			 * device cache.
			 */
			addr = 0;	/* [한국어] 범위 지정을 포기하고 */
			mask = MAX_AGAW_PFN_WIDTH;	/* [한국어] 장치 캐시를 통째로 비운다. 장치가 캐시한 것은 중첩 변환의 "결과"라, 2단계가 바뀌었을 때 영향받는 항목을 정확히 짚을 방법이 없다 (위 영어 주석) */
			fallthrough;	/* [한국어] 아래 DEVTLB 경로를 그대로 탄다 */
		case CACHE_TAG_DEVTLB:	/* [한국어] 장치 안의 캐시 */
			cache_tag_flush_devtlb_psi(domain, tag, addr, mask);	/* [한국어] 그 장치에 무효화를 보낸다 */
			break;	/* [한국어] 다음 태그 */
		}

		trace_cache_tag_flush_range(tag, start, end, addr, mask);	/* [한국어] 추적 이벤트 — 요청 범위와 실제 무효화 범위를 함께 남겨, 정렬 때문에 넓어진 정도를 사후에 볼 수 있다 */
	}
	qi_batch_flush_descs(iommu, domain->qi_batch);	/* [한국어] 마지막 유닛의 남은 배치를 제출한다 */
	spin_unlock_irqrestore(&domain->cache_lock, flags);	/* [한국어] 락 해제 */
}

/*
 * Invalidates all ranges of IOVA when the memory mappings in the target
 * domain have been modified.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tag_flush_all - 이 도메인의 모든 캐시를 통째로 비운다
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * flush_range 에 전체 범위를 넘기는 얇은 껍데기다. 그 함수가 0~ULONG_MAX 를
 * 특별 취급해 정렬 계산 없이 주소 공간 전체를 뜻하는 mask 를 쓴다.
 *
 * 언제 쓰는가: 범위가 너무 넓어 범위 무효화가 오히려 비쌀 때, 그리고
 * 도메인 전체가 무효가 되는 순간(도메인 해제, 페이지 테이블 교체)이다.
 *
 * 실행 컨텍스트: flush_range 와 같다.
 */
void cache_tag_flush_all(struct dmar_domain *domain)
{
	cache_tag_flush_range(domain, 0, ULONG_MAX, 0);	/* [한국어] 전체 범위로 넘긴다. 그쪽이 이 조합을 특별 취급해 정렬 계산을 건너뛴다 */
}

/*
 * Invalidate a range of IOVA when new mappings are created in the target
 * domain.
 *
 * - VT-d spec, Section 6.1 Caching Mode: When the CM field is reported as
 *   Set, any software updates to remapping structures other than first-
 *   stage mapping requires explicit invalidation of the caches.
 * - VT-d spec, Section 6.8 Write Buffer Flushing: For hardware that requires
 *   write buffer flushing, software must explicitly perform write-buffer
 *   flushing, if cache invalidation is not required.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * cache_tag_flush_range_np - 새로 만든 매핑이 하드웨어에 보이게 한다
 *
 * @domain: 대상 도메인. @start: 시작 주소. @end: 끝 주소.
 * @return: 없음.
 *
 * 보통의 IOMMU 는 매핑을 "만들 때" 무효화가 필요 없다. 없던 항목이 생긴
 * 것뿐이고 하드웨어는 "없음"을 캐시하지 않기 때문이다. 그런데 두 경우가
 * 예외이고, 위 영어 주석이 그 근거를 스펙 조항으로 적어 두었다.
 *
 *   [1] 캐싱 모드(스펙 6.1): CM 이 켜진 하드웨어는 1단계가 아닌 모든 재매핑
 *       구조의 변경에 명시적 무효화를 요구한다. 즉 "여기엔 매핑이 없다"까지
 *       캐시하므로, 그 캐시를 지워야 새 매핑이 보인다. np(non-present)라는
 *       이름이 여기서 왔다.
 *   [2] 쓰기 버퍼 플러시(스펙 6.8): 무효화가 필요 없는 하드웨어라도, rwbf 가
 *       필요한 유닛에서는 내부 쓰기 버퍼를 명시적으로 비워야 우리가 메모리에
 *       쓴 항목이 하드웨어에 보인다.
 *
 * 그래서 유닛마다 둘 중 하나를 한다: 캐싱 모드가 아니거나 1단계 도메인이면
 * 쓰기 버퍼만 비우고, 캐싱 모드면 IOTLB 무효화를 보낸다. 1단계를 제외하는
 * 것은 스펙 6.1 이 "1단계 매핑 외의" 구조라고 못 박았기 때문이다.
 *
 * DEVTLB 태그를 건너뛰는 것도 눈여겨볼 것: 장치는 자기가 요청한 적 없는
 * 번역을 캐시하지 않으므로, 새 매핑이 생겼다고 장치 캐시를 비울 이유가 없다.
 *
 * 실행 컨텍스트: iommu_map() 뒤. 잠들면 안 된다.
 *
 * 호출 체인:
 *   intel_iommu_iotlb_sync_map() → [cache_tag_flush_range_np]
 *     → iommu_flush_write_buffer() / cache_tag_flush_iotlb()
 */
void cache_tag_flush_range_np(struct dmar_domain *domain, unsigned long start,
			      unsigned long end)
{
	struct intel_iommu *iommu = NULL;	/* [한국어] 직전에 처리한 유닛. 유닛이 바뀌는 순간을 잡아 배치를 제출한다 */
	unsigned long mask, addr;	/* [한국어] 정렬을 맞춘 범위 */
	struct cache_tag *tag;	/* [한국어] 순회 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	addr = calculate_psi_aligned_address(start, end, &mask);	/* [한국어] PSI 가 요구하는 정렬을 맞춘다 */

	spin_lock_irqsave(&domain->cache_lock, flags);	/* [한국어] 태그 목록 순회 구간 */
	list_for_each_entry(tag, &domain->cache_tags, node) {	/* [한국어] 비워야 할 캐시들을 훑으며 */
		if (iommu && iommu != tag->iommu)	/* [한국어] 유닛이 바뀌었으면 */
			qi_batch_flush_descs(iommu, domain->qi_batch);	/* [한국어] 지금까지의 배치를 먼저 제출한다. 배치는 한 유닛의 큐에만 보낼 수 있기 때문이다 */
		iommu = tag->iommu;	/* [한국어] 현재 유닛을 기억한다 */

		if (!cap_caching_mode(iommu->cap) ||	/* [한국어] 캐싱 모드가 아니거나 (위 영어 주석: 스펙 6.8) */
		    intel_domain_is_fs_paging(domain)) {	/* [한국어] 1단계 도메인이면 (스펙 6.1 이 1단계를 제외한다) */
			iommu_flush_write_buffer(iommu);	/* [한국어] 무효화 대신 쓰기 버퍼만 비운다. 그것이 rwbf 하드웨어가 요구하는 전부다 */
			continue;	/* [한국어] 다음 태그 */
		}

		if (tag->type == CACHE_TAG_IOTLB ||	/* [한국어] 유닛 안의 캐시이거나 */
		    tag->type == CACHE_TAG_NESTING_IOTLB)	/* [한국어] 중첩 부모 쪽 유닛 캐시면 */
			cache_tag_flush_iotlb(domain, tag, addr, mask, 0);	/* [한국어] 캐시된 "매핑 없음"을 지운다. DEVTLB 는 건너뛴다 — 장치는 요청한 적 없는 번역을 캐시하지 않는다 */

		trace_cache_tag_flush_range_np(tag, start, end, addr, mask);	/* [한국어] 추적 이벤트 */
	}
	qi_batch_flush_descs(iommu, domain->qi_batch);	/* [한국어] 마지막 유닛의 남은 배치를 제출한다 */
	spin_unlock_irqrestore(&domain->cache_lock, flags);	/* [한국어] 락 해제 */
}
