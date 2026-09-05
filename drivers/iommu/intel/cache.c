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
	if (ret)
		cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_NESTING_IOTLB);

	return ret;
}

static void __cache_tag_unassign_parent_domain(struct dmar_domain *domain, u16 did,
					       struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_NESTING_IOTLB);

	if (info->ats_enabled)
		cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_NESTING_DEVTLB);
}

static u16 domain_get_id_for_dev(struct dmar_domain *domain, struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;

	/*
	 * The driver assigns different domain IDs for all domains except
	 * the SVA type.
	 */
	if (domain->domain.type == IOMMU_DOMAIN_SVA)
		return FLPT_DEFAULT_DID;

	return domain_id_iommu(domain, iommu);
}

/*
 * Assign cache tags to a domain when it's associated with a device's
 * PASID using a specific domain ID.
 *
 * On success (return value of 0), cache tags are created and added to the
 * domain's cache tag list. On failure (negative return value), an error
 * code is returned indicating the reason for the failure.
 */
int cache_tag_assign_domain(struct dmar_domain *domain,
			    struct device *dev, ioasid_t pasid)
{
	u16 did = domain_get_id_for_dev(domain, dev);
	int ret;

	ret = __cache_tag_assign_domain(domain, did, dev, pasid);
	if (ret || domain->domain.type != IOMMU_DOMAIN_NESTED)
		return ret;

	ret = __cache_tag_assign_parent_domain(domain->s2_domain, did, dev, pasid);
	if (ret)
		__cache_tag_unassign_domain(domain, did, dev, pasid);

	return ret;
}

/*
 * Remove the cache tags associated with a device's PASID when the domain is
 * detached from the device.
 *
 * The cache tags must be previously assigned to the domain by calling the
 * assign interface.
 */
void cache_tag_unassign_domain(struct dmar_domain *domain,
			       struct device *dev, ioasid_t pasid)
{
	u16 did = domain_get_id_for_dev(domain, dev);

	__cache_tag_unassign_domain(domain, did, dev, pasid);
	if (domain->domain.type == IOMMU_DOMAIN_NESTED)
		__cache_tag_unassign_parent_domain(domain->s2_domain, did, dev, pasid);
}

static unsigned long calculate_psi_aligned_address(unsigned long start,
						   unsigned long end,
						   unsigned long *_mask)
{
	unsigned long pages = aligned_nrpages(start, end - start + 1);
	unsigned long aligned_pages = __roundup_pow_of_two(pages);
	unsigned long bitmask = aligned_pages - 1;
	unsigned long mask = ilog2(aligned_pages);
	unsigned long pfn = IOVA_PFN(start);

	/*
	 * PSI masks the low order bits of the base address. If the
	 * address isn't aligned to the mask, then compute a mask value
	 * needed to ensure the target range is flushed.
	 */
	if (unlikely(bitmask & pfn)) {
		unsigned long end_pfn = pfn + pages - 1, shared_bits;

		/*
		 * Since end_pfn <= pfn + bitmask, the only way bits
		 * higher than bitmask can differ in pfn and end_pfn is
		 * by carrying. This means after masking out bitmask,
		 * high bits starting with the first set bit in
		 * shared_bits are all equal in both pfn and end_pfn.
		 */
		shared_bits = ~(pfn ^ end_pfn) & ~bitmask;
		mask = shared_bits ? __ffs(shared_bits) : MAX_AGAW_PFN_WIDTH;
	}

	*_mask = mask;

	return ALIGN_DOWN(start, VTD_PAGE_SIZE << mask);
}

static void qi_batch_flush_descs(struct intel_iommu *iommu, struct qi_batch *batch)
{
	if (!iommu || !batch->index)
		return;

	qi_submit_sync(iommu, batch->descs, batch->index, 0);

	/* Reset the index value and clean the whole batch buffer. */
	memset(batch, 0, sizeof(*batch));
}

static void qi_batch_increment_index(struct intel_iommu *iommu, struct qi_batch *batch)
{
	if (++batch->index == QI_MAX_BATCHED_DESC_COUNT)
		qi_batch_flush_descs(iommu, batch);
}

static void qi_batch_add_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
			       unsigned int size_order, u64 type,
			       struct qi_batch *batch)
{
	qi_desc_iotlb(iommu, did, addr, size_order, type, &batch->descs[batch->index]);
	qi_batch_increment_index(iommu, batch);
}

static void qi_batch_add_dev_iotlb(struct intel_iommu *iommu, u16 sid, u16 pfsid,
				   u16 qdep, u64 addr, unsigned int mask,
				   struct qi_batch *batch)
{
	/*
	 * According to VT-d spec, software is recommended to not submit any Device-TLB
	 * invalidation requests while address remapping hardware is disabled.
	 */
	if (!(iommu->gcmd & DMA_GCMD_TE))
		return;

	qi_desc_dev_iotlb(sid, pfsid, qdep, addr, mask, &batch->descs[batch->index]);
	qi_batch_increment_index(iommu, batch);
}

static void qi_batch_add_piotlb_all(struct intel_iommu *iommu, u16 did,
				    u32 pasid, struct qi_batch *batch)
{
	qi_desc_piotlb_all(did, pasid, &batch->descs[batch->index]);
	qi_batch_increment_index(iommu, batch);
}

static void qi_batch_add_piotlb(struct intel_iommu *iommu, u16 did, u32 pasid,
				u64 addr, unsigned int size_order, bool ih,
				struct qi_batch *batch)
{
	qi_desc_piotlb(did, pasid, addr, size_order, ih,
		       &batch->descs[batch->index]);
	qi_batch_increment_index(iommu, batch);
}

static void qi_batch_add_pasid_dev_iotlb(struct intel_iommu *iommu, u16 sid, u16 pfsid,
					 u32 pasid,  u16 qdep, u64 addr,
					 unsigned int size_order, struct qi_batch *batch)
{
	/*
	 * According to VT-d spec, software is recommended to not submit any
	 * Device-TLB invalidation requests while address remapping hardware
	 * is disabled.
	 */
	if (!(iommu->gcmd & DMA_GCMD_TE))
		return;

	qi_desc_dev_iotlb_pasid(sid, pfsid, pasid, qdep, addr, size_order,
				&batch->descs[batch->index]);
	qi_batch_increment_index(iommu, batch);
}

static bool intel_domain_use_piotlb(struct dmar_domain *domain)
{
	return domain->domain.type == IOMMU_DOMAIN_SVA ||
			domain->domain.type == IOMMU_DOMAIN_NESTED ||
			intel_domain_is_fs_paging(domain);
}

static void cache_tag_flush_iotlb(struct dmar_domain *domain, struct cache_tag *tag,
				  unsigned long addr, unsigned long mask, int ih)
{
	struct intel_iommu *iommu = tag->iommu;
	u64 type = DMA_TLB_PSI_FLUSH;

	if (intel_domain_use_piotlb(domain)) {
		if (mask >= MAX_AGAW_PFN_WIDTH)
			qi_batch_add_piotlb_all(iommu, tag->domain_id,
						tag->pasid, domain->qi_batch);
		else
			qi_batch_add_piotlb(iommu, tag->domain_id, tag->pasid,
					    addr, mask, ih, domain->qi_batch);
		return;
	}

	/*
	 * Fallback to domain selective flush if no PSI support or the size
	 * is too big.
	 */
	if (!cap_pgsel_inv(iommu->cap) ||
	    mask > cap_max_amask_val(iommu->cap)) {
		addr = 0;
		mask = 0;
		ih = 0;
		type = DMA_TLB_DSI_FLUSH;
	}

	if (ecap_qis(iommu->ecap))
		qi_batch_add_iotlb(iommu, tag->domain_id, addr | ih, mask, type,
				   domain->qi_batch);
	else
		__iommu_flush_iotlb(iommu, tag->domain_id, addr | ih, mask, type);
}

static void cache_tag_flush_devtlb_psi(struct dmar_domain *domain, struct cache_tag *tag,
				       unsigned long addr, unsigned long mask)
{
	struct intel_iommu *iommu = tag->iommu;
	struct device_domain_info *info;
	u16 sid;

	info = dev_iommu_priv_get(tag->dev);
	sid = PCI_DEVID(info->bus, info->devfn);

	if (tag->pasid == IOMMU_NO_PASID) {
		qi_batch_add_dev_iotlb(iommu, sid, info->pfsid, info->ats_qdep,
				       addr, mask, domain->qi_batch);
		if (info->dtlb_extra_inval)
			qi_batch_add_dev_iotlb(iommu, sid, info->pfsid, info->ats_qdep,
					       addr, mask, domain->qi_batch);
		return;
	}

	qi_batch_add_pasid_dev_iotlb(iommu, sid, info->pfsid, tag->pasid,
				     info->ats_qdep, addr, mask, domain->qi_batch);
	if (info->dtlb_extra_inval)
		qi_batch_add_pasid_dev_iotlb(iommu, sid, info->pfsid, tag->pasid,
					     info->ats_qdep, addr, mask,
					     domain->qi_batch);
}

/*
 * Invalidates a range of IOVA from @start (inclusive) to @end (inclusive)
 * when the memory mappings in the target domain have been modified.
 */
void cache_tag_flush_range(struct dmar_domain *domain, unsigned long start,
			   unsigned long end, int ih)
{
	struct intel_iommu *iommu = NULL;
	unsigned long mask, addr;
	struct cache_tag *tag;
	unsigned long flags;

	if (start == 0 && end == ULONG_MAX) {
		addr = 0;
		mask = MAX_AGAW_PFN_WIDTH;
	} else {
		addr = calculate_psi_aligned_address(start, end, &mask);
	}

	spin_lock_irqsave(&domain->cache_lock, flags);
	list_for_each_entry(tag, &domain->cache_tags, node) {
		if (iommu && iommu != tag->iommu)
			qi_batch_flush_descs(iommu, domain->qi_batch);
		iommu = tag->iommu;

		switch (tag->type) {
		case CACHE_TAG_IOTLB:
		case CACHE_TAG_NESTING_IOTLB:
			cache_tag_flush_iotlb(domain, tag, addr, mask, ih);
			break;
		case CACHE_TAG_NESTING_DEVTLB:
			/*
			 * Address translation cache in device side caches the
			 * result of nested translation. There is no easy way
			 * to identify the exact set of nested translations
			 * affected by a change in S2. So just flush the entire
			 * device cache.
			 */
			addr = 0;
			mask = MAX_AGAW_PFN_WIDTH;
			fallthrough;
		case CACHE_TAG_DEVTLB:
			cache_tag_flush_devtlb_psi(domain, tag, addr, mask);
			break;
		}

		trace_cache_tag_flush_range(tag, start, end, addr, mask);
	}
	qi_batch_flush_descs(iommu, domain->qi_batch);
	spin_unlock_irqrestore(&domain->cache_lock, flags);
}

/*
 * Invalidates all ranges of IOVA when the memory mappings in the target
 * domain have been modified.
 */
void cache_tag_flush_all(struct dmar_domain *domain)
{
	cache_tag_flush_range(domain, 0, ULONG_MAX, 0);
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
void cache_tag_flush_range_np(struct dmar_domain *domain, unsigned long start,
			      unsigned long end)
{
	struct intel_iommu *iommu = NULL;
	unsigned long mask, addr;
	struct cache_tag *tag;
	unsigned long flags;

	addr = calculate_psi_aligned_address(start, end, &mask);

	spin_lock_irqsave(&domain->cache_lock, flags);
	list_for_each_entry(tag, &domain->cache_tags, node) {
		if (iommu && iommu != tag->iommu)
			qi_batch_flush_descs(iommu, domain->qi_batch);
		iommu = tag->iommu;

		if (!cap_caching_mode(iommu->cap) ||
		    intel_domain_is_fs_paging(domain)) {
			iommu_flush_write_buffer(iommu);
			continue;
		}

		if (tag->type == CACHE_TAG_IOTLB ||
		    tag->type == CACHE_TAG_NESTING_IOTLB)
			cache_tag_flush_iotlb(domain, tag, addr, mask, 0);

		trace_cache_tag_flush_range_np(tag, start, end, addr, mask);
	}
	qi_batch_flush_descs(iommu, domain->qi_batch);
	spin_unlock_irqrestore(&domain->cache_lock, flags);
}
