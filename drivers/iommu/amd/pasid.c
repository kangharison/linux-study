// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 */

/*
 * [한국어 설명] AMD IOMMU 의 SVA(공유 가상 주소) 구현 (pasid.c)
 *
 * === 파일의 역할 ===
 * 장치가 프로세스의 주소 공간을 그대로 쓰게 만든다. 평소의 DMA 는 커널이
 * IOVA 를 잡고 매핑해 주지만, SVA 에서는 장치가 프로세스의 가상 주소를
 * 그대로 내고 IOMMU 가 프로세스의 페이지 테이블로 그것을 변환한다.
 *
 * 그래서 이 파일이 하는 일은 매핑을 만드는 것이 아니라 연결을 만드는 것이다:
 *  - 프로세스의 페이지 테이블 루트(mm->pgd)를 장치의 GCR3 표에 PASID 별로
 *    걸어 준다. 새로 만드는 것은 아무것도 없다.
 *  - 그 대가로 두 가지 의무가 생긴다. 프로세스의 매핑이 바뀌면 IOMMU 캐시를
 *    지워야 하고(mmu_notifier), 프로세스가 죽으면 그 연결을 즉시 끊어야
 *    한다(release). 후자를 놓치면 장치가 해제된 페이지 테이블을 계속 따라간다.
 *
 * 페이지가 없을 때 폴트를 내어 커널이 가져다주는 부분은 ppr.c 가 맡는다.
 * 이 파일은 "누가 어느 주소 공간을 보는가"의 연결만 관리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 코어 IOMMU 의 SVA 계층과 AMD 하드웨어 사이다. 위로는 iommu_sva_bind_device
 * 가 이 파일의 domain_alloc_sva 를 거쳐 도메인을 만들고, 아래로는
 * amd_iommu_set_gcr3 로 하드웨어에 닿는다.
 *
 * 옆으로는 mm 서브시스템과 이어진다 — mmu_notifier 로 프로세스의 페이지
 * 테이블 변경을 통지받는 것이 SVA 의 정확성을 지탱하는 유일한 장치다.
 *
 * 실행 컨텍스트: 붙이고 떼는 경로는 프로세스 문맥이지만, mmu_notifier
 * 콜백은 mm 이 락을 든 채로 부르므로 잠들 수 없다. 그래서 도메인 락을
 * irqsave 스핀락으로 잡는다.
 *
 * 호출 체인:
 *   iommu_sva_bind_device() → amd_iommu_domain_alloc_sva()
 *     → mmu_notifier_register() → iommu_sva_set_dev_pasid()
 *     → amd_iommu_set_gcr3()
 *   mm 의 매핑 변경 → sva_arch_invalidate_secondary_tlbs()
 *     → amd_iommu_dev_flush_pasid_pages()
 *   프로세스 종료 → sva_mn_release() → remove_dev_pasid()
 *
 * === 타 모듈과의 연결 ===
 * linux/mm_types.h 의 mm_struct(특히 pgd), linux/iommu.h 의 SVA 도메인 타입,
 * 그리고 amd_iommu.h 의 GCR3 설정과 무효화 함수.
 *
 * 공유 상태: protection_domain->dev_data_list 가 "이 주소 공간을 보고 있는
 * {장치, PASID} 쌍"의 목록이고, 무효화와 정리가 모두 그 목록을 훑는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - amd_iommu_domain_alloc_sva(): SVA 도메인을 만들고 mmu_notifier 를 건다.
 * - iommu_sva_set_dev_pasid(): 장치의 PASID 하나를 그 주소 공간에 연결한다.
 * - sva_arch_invalidate_secondary_tlbs(): 프로세스의 매핑이 바뀌면 IOMMU
 *   캐시를 지운다.
 * - sva_mn_release(): 프로세스가 죽으면 모든 연결을 끊는다.
 * - remove_dev_pasid(): 연결 하나를 끊는 공통 절차.
 */
#define pr_fmt(fmt)     "AMD-Vi: " fmt	/* [한국어] 이 파일의 로그 접두사 */
#define dev_fmt(fmt)    pr_fmt(fmt)	/* [한국어] dev_err 계열도 같은 접두사를 쓰게 한다 */

#include <linux/iommu.h>	/* [한국어] 코어 IOMMU 의 도메인 타입과 SVA 인터페이스 */
#include <linux/mm_types.h>	/* [한국어] struct mm_struct — 특히 pgd. SVA 의 전부가 이 포인터를 넘기는 것이다 */

#include "amd_iommu.h"	/* [한국어] GCR3 설정과 무효화 함수의 선언 */

/*
 * [한국어]
 * is_pasid_enabled - 이 장치가 지금 PASID 를 쓸 수 있는 상태인가
 *
 * @dev_data: 장치의 IOMMU 쪽 상태.
 * @return: 세 조건이 모두 만족되면 참.
 *
 * 세 가지를 함께 보는 이유가 각각 다르다.
 *  - pasid_enabled: PCI PASID 능력을 실제로 켰는가. 지원만으로는 부족하다.
 *  - max_pasids: 몇 개까지 쓸 수 있는가. 0 이면 능력이 있어도 쓸 수 없다.
 *  - gcr3_tbl: PASID → 페이지 테이블 대응표가 만들어져 있는가. 이것이
 *    없으면 연결할 곳 자체가 없다.
 *
 * 셋 중 하나라도 빠지면 SVA 연결을 만들 수 없다.
 *
 * 호출 체인:
 *   iommu_sva_set_dev_pasid() → [이 함수]
 */
static inline bool is_pasid_enabled(struct iommu_dev_data *dev_data)
{
	if (dev_data->pasid_enabled && dev_data->max_pasids &&	/* [한국어] PCI PASID 능력을 켰고, 쓸 수 있는 개수가 0 이 아니고 */
	    dev_data->gcr3_info.gcr3_tbl != NULL)	/* [한국어] PASID → 페이지 테이블 대응표가 만들어져 있는가 */
		return true;	/* [한국어] 셋이 모두 갖춰져야 연결할 수 있다 */

	return false;	/* [한국어] 하나라도 빠지면 SVA 를 쓸 수 없다 */
}

/*
 * [한국어]
 * is_pasid_valid - 이 PASID 값을 이 장치에 쓸 수 있는가
 *
 * @dev_data: 대상 장치.
 * @pasid: 검사할 PASID.
 * @return: 범위 안이면 참.
 *
 * 0 을 제외하는 것이 핵심이다. PASID 0 은 "PASID 없이 오는 요청"을 뜻하는
 * 예약값이라 SVA 에 쓸 수 없다 — 그것을 프로세스 주소 공간에 연결하면
 * 평범한 DMA 까지 그 주소 공간으로 가게 된다.
 *
 * 상한은 장치가 광고한 max_pasids 다. 그보다 큰 값을 쓰면 장치가 요청에
 * 실을 수 없어 조용히 잘린다.
 *
 * 호출 체인:
 *   iommu_sva_set_dev_pasid()/amd_iommu_remove_dev_pasid() → [이 함수]
 */
static inline bool is_pasid_valid(struct iommu_dev_data *dev_data,
				  ioasid_t pasid)
{
	if (pasid > 0 && pasid < dev_data->max_pasids)	/* [한국어] 0 은 "PASID 없는 요청"의 예약값이라 제외하고, 장치가 광고한 상한 미만이어야 한다 */
		return true;	/* [한국어] 쓸 수 있는 값 */

	return false;	/* [한국어] 범위를 벗어났거나 예약값 */
}

/*
 * [한국어]
 * remove_dev_pasid - {장치, PASID} 연결 하나를 끊는다
 *
 * @pdom_dev_data: 끊을 연결.
 *
 * 순서가 이 함수의 전부다. 먼저 GCR3 표에서 그 PASID 를 지우고(그 안에서
 * IOTLB 무효화까지 한다), 그다음 목록에서 빼고, 마지막에 메모리를 놓는다.
 *
 * 거꾸로 하면 무효화가 끝나기 전에 소프트웨어 상태가 사라져, 하드웨어가
 * 아직 캐시에 든 옛 페이지 테이블을 따라가는 동안 그것을 추적할 방법이
 * 없어진다.
 *
 * 호출자가 도메인 락을 들고 있어야 한다 — 목록을 건드리기 때문이다.
 *
 * 호출 체인:
 *   remove_pdom_dev_pasid()/sva_mn_release() → [이 함수]
 *     → amd_iommu_clear_gcr3()
 */
static void remove_dev_pasid(struct pdom_dev_data *pdom_dev_data)
{
	/* Update GCR3 table and flush IOTLB */
	amd_iommu_clear_gcr3(pdom_dev_data->dev_data, pdom_dev_data->pasid);	/* [한국어] 먼저 하드웨어에서 지우고 캐시까지 무효화한다 (원 주석: GCR3 표 갱신과 IOTLB 플러시) */

	list_del(&pdom_dev_data->list);	/* [한국어] 그다음 목록에서 뺀다 */
	kfree(pdom_dev_data);	/* [한국어] 마지막에 놓는다. 순서를 거꾸로 하면 무효화 도중 상태를 잃는다 */
}

/* Clear PASID from device GCR3 table and remove pdom_dev_data from list */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * remove_pdom_dev_pasid - 목록에서 그 {장치, PASID} 쌍을 찾아 끊는다
 *
 * @pdom: SVA 도메인.
 * @dev: 대상 장치.
 * @pasid: 끊을 PASID.
 *
 * 쌍으로 찾아야 하는 이유: 한 도메인에 여러 장치가 붙어 있고, 한 장치가
 * 여러 PASID 로 붙어 있을 수 있다. 장치만 보거나 PASID 만 보면 엉뚱한
 * 연결을 끊는다.
 *
 * 찾지 못하면 조용히 돌아간다. 이미 끊긴 연결을 다시 끊으라는 요청은
 * 오류가 아니라 중복이며, 여기서 경고를 내면 정상적인 정리 순서에서도
 * 잡음이 난다.
 *
 * lockdep_assert_held: 목록을 훑고 원소를 지우므로 락이 필수인데, 이
 * 함수가 락을 잡지 않고 호출자에게 맡기므로 그 계약을 코드로 못박아 둔다.
 *
 * 호출 체인:
 *   amd_iommu_remove_dev_pasid() → [이 함수] → remove_dev_pasid()
 */
static void remove_pdom_dev_pasid(struct protection_domain *pdom,
				  struct device *dev, ioasid_t pasid)
{
	struct pdom_dev_data *pdom_dev_data;	/* [한국어] 목록을 훑을 커서 */
	struct iommu_dev_data *dev_data = dev_iommu_priv_get(dev);	/* [한국어] 비교 대상이 될 장치의 IOMMU 상태 */

	lockdep_assert_held(&pdom->lock);	/* [한국어] 이 함수는 락을 잡지 않고 호출자에게 맡기므로, 그 계약을 코드로 못박는다 */

	for_each_pdom_dev_data(pdom_dev_data, pdom) {	/* [한국어] 도메인에 붙은 모든 쌍을 훑는다 */
		if (pdom_dev_data->dev_data == dev_data &&	/* [한국어] 장치가 같고 */
		    pdom_dev_data->pasid == pasid) {	/* [한국어] PASID 도 같은 쌍인가 — 둘 다 봐야 엉뚱한 연결을 끊지 않는다 */
			remove_dev_pasid(pdom_dev_data);	/* [한국어] 찾았으면 끊는다 */
			break;	/* [한국어] 쌍은 유일하므로 더 볼 필요가 없다. 못 찾아도 조용히 돌아간다 */
		}
	}
}

/*
 * [한국어]
 * sva_arch_invalidate_secondary_tlbs - 프로세스의 매핑이 바뀌면 IOMMU 캐시를 지운다
 *
 * @mn: 이 도메인에 걸린 mmu_notifier.
 * @mm: 매핑이 바뀐 프로세스.
 * @start: 바뀐 구간의 시작.
 * @end: 그 구간의 끝(배타적).
 *
 * SVA 의 정확성을 지탱하는 함수다. IOMMU 는 프로세스의 페이지 테이블을
 * 그대로 읽지만, 읽은 결과를 자기 IOTLB 에 캐시한다. 프로세스가 munmap 을
 * 하거나 페이지를 회수당하면 CPU 쪽 TLB 는 커널이 지워 주지만 IOMMU 의
 * 캐시는 그대로 남는다 — 그 상태로 두면 장치가 이미 남에게 넘어간 물리
 * 페이지를 계속 읽고 쓴다.
 *
 * "secondary TLB"라는 이름이 그 관계를 말해 준다. CPU 의 TLB 가 primary 이고,
 * 같은 페이지 테이블을 보는 다른 캐시들이 secondary 다.
 *
 * 도메인의 모든 {장치, PASID} 쌍을 도는 이유: 한 주소 공간을 여러 장치가
 * 볼 수 있고, 각자 자기 캐시를 갖는다.
 *
 * 실행 컨텍스트: mm 이 락을 든 채 부르므로 잠들 수 없다. 그래서 도메인
 * 락도 스핀락이어야 하고, 무효화는 완료를 기다리는 동기 동작이다 —
 * 이 함수가 반환하면 커널은 그 페이지를 재사용해도 안전하다고 가정한다.
 *
 * 호출 체인:
 *   munmap/페이지 회수 등 → mmu_notifier → [이 함수]
 *     → amd_iommu_dev_flush_pasid_pages()
 */
static void sva_arch_invalidate_secondary_tlbs(struct mmu_notifier *mn,
				    struct mm_struct *mm,
				    unsigned long start, unsigned long end)
{
	struct pdom_dev_data *pdom_dev_data;	/* [한국어] 목록 커서 */
	struct protection_domain *sva_pdom;	/* [한국어] 이 통지가 속한 SVA 도메인 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */

	sva_pdom = container_of(mn, struct protection_domain, mn);	/* [한국어] notifier 가 박혀 있는 도메인으로 되짚는다 */

	spin_lock_irqsave(&sva_pdom->lock, flags);	/* [한국어] 목록을 훑는 동안 붙이고 떼는 경로와 경쟁하지 않게. mm 락 아래라 잠들 수 없다 */

	for_each_pdom_dev_data(pdom_dev_data, sva_pdom) {	/* [한국어] 한 주소 공간을 여러 장치가 볼 수 있고 각자 캐시를 갖는다 */
		amd_iommu_dev_flush_pasid_pages(pdom_dev_data->dev_data,	/* [한국어] 그 장치의 */
						pdom_dev_data->pasid,	/* [한국어] 그 PASID 에 대해 */
						start, end - start);	/* [한국어] 바뀐 구간만 무효화한다. 반환 시점에 커널은 그 페이지를 재사용해도 된다고 가정한다 */
	}

	spin_unlock_irqrestore(&sva_pdom->lock, flags);	/* [한국어] 모든 장치의 캐시를 지웠다 */
}

/*
 * [한국어]
 * sva_mn_release - 프로세스가 죽었을 때 모든 연결을 끊는다
 *
 * @mn: 이 도메인의 mmu_notifier.
 * @mm: 사라지는 주소 공간.
 *
 * 프로세스가 종료되면 그 페이지 테이블이 해제된다. 그 순간 IOMMU 가 여전히
 * 그것을 가리키고 있으면, 장치가 해제되어 다른 용도로 넘어간 메모리를
 * 페이지 테이블로 읽는다 — 임의의 물리 주소에 DMA 를 하는 것과 같다.
 *
 * 그래서 이 콜백은 mm 이 사라지기 전에 반드시 모든 GCR3 항목을 지워야 한다.
 * "나중에 정리한다"는 선택지가 없다.
 *
 * 목록 전체를 비우는 이유는 원 주석이 밝힌다: 이 도메인의 dev_data_list 에는
 * 같은 PASID 를 쓰는 여러 장치가 들어 있을 수 있고, 어느 것도 남겨서는
 * 안 된다.
 *
 * _safe 순회를 쓰는 이유: remove_dev_pasid 가 원소를 목록에서 빼고 놓는다.
 *
 * 호출 체인:
 *   프로세스 종료 → mmu_notifier release → [이 함수] → remove_dev_pasid()
 */
static void sva_mn_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	struct pdom_dev_data *pdom_dev_data, *next;	/* [한국어] 원소를 지우며 훑으므로 다음 원소를 미리 잡는다 */
	struct protection_domain *sva_pdom;	/* [한국어] 사라지는 주소 공간의 도메인 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */

	sva_pdom = container_of(mn, struct protection_domain, mn);	/* [한국어] notifier 에서 도메인으로 */

	spin_lock_irqsave(&sva_pdom->lock, flags);	/* [한국어] 목록 조작 보호 */

	/* Assume dev_data_list contains same PASID with different devices */
	for_each_pdom_dev_data_safe(pdom_dev_data, next, sva_pdom)	/* [한국어] (원 주석: 같은 PASID 를 쓰는 여러 장치가 목록에 있을 수 있다) */
		remove_dev_pasid(pdom_dev_data);	/* [한국어] 하나도 남기지 않는다 — 남으면 장치가 해제된 페이지 테이블을 따라간다 */

	spin_unlock_irqrestore(&sva_pdom->lock, flags);	/* [한국어] 정리 완료. 이제 mm 이 사라져도 안전하다 */
}

/*
 * [한국어] struct mmu_notifier_ops sva_mn — mm 서브시스템에 등록하는 두 콜백
 *
 * SVA 가 mm 에게서 받아야 하는 신호가 정확히 이 둘이다.
 *  - 매핑이 바뀌었다 → 캐시를 지워라.
 *  - 주소 공간이 사라진다 → 연결을 끊어라.
 *
 * 다른 mmu_notifier 콜백(invalidate_range_start/end 등)을 쓰지 않는 이유:
 * IOMMU 는 페이지를 참조해 붙잡아 두지 않으므로, 변경 전후를 감싸는 무거운
 * 프로토콜이 필요 없다. 바뀐 뒤에 캐시만 지우면 된다.
 */
static const struct mmu_notifier_ops sva_mn = {
	.arch_invalidate_secondary_tlbs = sva_arch_invalidate_secondary_tlbs,
	/* [한국어] 프로세스의 매핑이 바뀌었을 때 IOMMU 캐시를 지우는 콜백.
	 * "secondary TLB" 는 CPU 의 TLB(primary)와 같은 페이지 테이블을 보는
	 *   다른 캐시들을 가리킨다 — IOMMU 의 IOTLB 가 그중 하나다.
	 * 이 콜백이 반환하면 커널은 그 페이지를 재사용해도 된다고 가정한다. */
	.release = sva_mn_release,
	/* [한국어] 주소 공간이 사라질 때 모든 연결을 끊는 콜백.
	 * 미루는 선택지가 없다 — mm 이 해제된 뒤에도 GCR3 가 그 페이지 테이블을
	 *   가리키고 있으면 장치가 남의 메모리를 페이지 테이블로 읽는다. */
};

/*
 * [한국어]
 * iommu_sva_set_dev_pasid - 장치의 PASID 하나를 프로세스 주소 공간에 연결한다
 *
 * @domain: SVA 도메인(프로세스의 mm 을 품고 있다).
 * @dev: 연결할 장치.
 * @pasid: 그 장치가 쓸 PASID.
 * @old: 이전 도메인. 이 구현은 교체를 지원하지 않는다.
 * @return: 0 성공, -EINVAL 이면 조건 미달, -EOPNOTSUPP 이면 교체 요청.
 *
 * SVA 의 핵심이 단 한 줄로 나타나는 곳이다:
 *   amd_iommu_set_gcr3(dev_data, pasid, iommu_virt_to_phys(domain->mm->pgd))
 * 프로세스의 페이지 테이블 루트를 그대로 하드웨어에 알린다. 새 페이지
 * 테이블도, IOVA 할당도, 매핑도 만들지 않는다 — 그것이 SVA 다.
 *
 * 대신 두 가지 비용이 생긴다. 프로세스의 매핑이 바뀔 때마다 IOMMU 캐시를
 * 지워야 하고, 장치가 아직 없는 페이지에 닿으면 페이지 폴트가 나서
 * 처리해야 한다.
 *
 * old != NULL 을 거절하는 이유: 도메인 교체는 "지금 이 PASID 가 보던 주소
 * 공간을 다른 것으로 바꾼다"는 뜻인데, 그 사이 장치가 낸 요청이 어느 쪽으로
 * 가는지 정의하기 어렵다. 지원하지 않는다고 분명히 답하는 편이 낫다.
 *
 * pdom_dev_data 를 락 밖에서 미리 할당하는 이유: 락 안에서는 잠들 수 없다.
 * 실패하면 락 안에서 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 도메인 락은 mmu_notifier 와 공유하므로
 * irqsave 로 잡는다.
 *
 * 호출 체인:
 *   iommu_sva_bind_device() → 코어 → [이 함수] → amd_iommu_set_gcr3()
 */
int iommu_sva_set_dev_pasid(struct iommu_domain *domain,
			    struct device *dev, ioasid_t pasid,
			    struct iommu_domain *old)
{
	struct pdom_dev_data *pdom_dev_data;	/* [한국어] 새로 만들 연결 항목 */
	struct protection_domain *sva_pdom = to_pdomain(domain);	/* [한국어] 코어 도메인을 AMD 구조체로 */
	struct iommu_dev_data *dev_data = dev_iommu_priv_get(dev);	/* [한국어] 장치의 IOMMU 상태 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	int ret = -EINVAL;	/* [한국어] 검증에 걸리면 이 값으로 돌아간다 */

	if (old)	/* [한국어] 도메인 교체 요청인가 */
		return -EOPNOTSUPP;	/* [한국어] 교체 중 장치가 낸 요청이 어느 주소 공간으로 갈지 정의하기 어려워 지원하지 않는다 */

	/* PASID zero is used for requests from the I/O device without PASID */
	if (!is_pasid_valid(dev_data, pasid))	/* [한국어] (원 주석: PASID 0 은 PASID 없는 요청에 쓰인다) */
		return ret;	/* [한국어] 예약값이거나 범위 밖 */

	/* Make sure PASID is enabled */
	if (!is_pasid_enabled(dev_data))	/* [한국어] 장치가 PASID 를 쓸 준비가 됐는가 (원 주석: PASID 가 켜져 있는지 확인) */
		return ret;	/* [한국어] GCR3 표가 없으면 연결할 곳이 없다 */

	/* Add PASID to protection domain pasid list */
	pdom_dev_data = kzalloc_obj(*pdom_dev_data);	/* [한국어] 락 밖에서 미리 잡는다 — 락 안에서는 잠들 수 없다 */
	if (pdom_dev_data == NULL)	/* [한국어] 할당 실패 */
		return ret;	/* [한국어] 아직 아무것도 바꾸지 않았다 */

	pdom_dev_data->pasid = pasid;	/* [한국어] 이 연결의 PASID */
	pdom_dev_data->dev_data = dev_data;	/* [한국어] 그리고 장치 */

	spin_lock_irqsave(&sva_pdom->lock, flags);	/* [한국어] mmu_notifier 와 공유하는 락이라 irqsave */

	/* Setup GCR3 table */
	ret = amd_iommu_set_gcr3(dev_data, pasid,	/* [한국어] SVA 의 핵심 한 줄 (원 주석: GCR3 표 설정) */
				 iommu_virt_to_phys(domain->mm->pgd));	/* [한국어] 프로세스의 페이지 테이블 루트를 그대로 하드웨어에 알린다. 새로 만드는 것은 없다 */
	if (ret) {	/* [한국어] 하드웨어 설정 실패 */
		kfree(pdom_dev_data);	/* [한국어] 미리 잡아 둔 항목을 놓고 */
		goto out_unlock;	/* [한국어] 락을 풀고 나간다 */
	}

	list_add(&pdom_dev_data->list, &sva_pdom->dev_data_list);	/* [한국어] 무효화와 정리가 이 목록을 훑으므로 반드시 등록해야 한다 */

out_unlock:	/* [한국어] 하드웨어 설정에 실패했든 성공했든 여기로 모여 락을 푼다 */
	spin_unlock_irqrestore(&sva_pdom->lock, flags);	/* [한국어] 성공이든 실패든 여기로 모인다 */
	return ret;	/* [한국어] 0 이면 이제 장치가 프로세스의 주소 공간을 본다 */
}

/*
 * [한국어]
 * amd_iommu_remove_dev_pasid - 장치의 PASID 연결을 끊는다 (코어 콜백)
 *
 * @dev: 대상 장치.
 * @pasid: 끊을 PASID.
 * @domain: 그 PASID 가 붙어 있던 도메인.
 *
 * 사용자가 SVA 를 해제하거나 장치를 놓을 때 코어가 부른다. 실제 작업은
 * remove_pdom_dev_pasid 가 하고, 이 함수는 인자 검증과 락만 맡는다.
 *
 * PASID 유효성을 다시 확인하는 이유: 코어가 넘긴 값이 이 장치에 유효하지
 * 않으면 목록을 훑어도 찾지 못할 뿐이지만, 애초에 잘못된 요청임을 여기서
 * 걸러 두면 그 아래 코드가 단순해진다.
 *
 * 호출 체인:
 *   iommu_sva_unbind_device()/장치 해제 → 코어 → [이 함수]
 *     → remove_pdom_dev_pasid()
 */
void amd_iommu_remove_dev_pasid(struct device *dev, ioasid_t pasid,
				struct iommu_domain *domain)
{
	struct protection_domain *sva_pdom;	/* [한국어] 대상 도메인 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */

	if (!is_pasid_valid(dev_iommu_priv_get(dev), pasid))	/* [한국어] 애초에 유효하지 않은 값이면 목록에도 없다 */
		return;	/* [한국어] 잘못된 요청을 여기서 걸러 둔다 */

	sva_pdom = to_pdomain(domain);	/* [한국어] AMD 구조체로 */

	spin_lock_irqsave(&sva_pdom->lock, flags);	/* [한국어] 목록 조작 보호 */

	/* Remove PASID from dev_data_list */
	remove_pdom_dev_pasid(sva_pdom, dev, pasid);	/* [한국어] 쌍을 찾아 끊는다 (원 주석: dev_data_list 에서 PASID 제거) */

	spin_unlock_irqrestore(&sva_pdom->lock, flags);	/* [한국어] 완료 */
}

/*
 * [한국어]
 * iommu_sva_domain_free - SVA 도메인을 없앤다
 *
 * @domain: 없앨 도메인.
 *
 * mmu_notifier 를 먼저 떼는 것이 중요하다. 그것이 남아 있으면 도메인이
 * 해제된 뒤에도 mm 이 콜백을 부를 수 있고, 그 콜백은 사라진 구조체를
 * 통해 목록을 훑는다.
 *
 * mn.ops 를 검사하는 이유: 도메인을 만들다가 mmu_notifier 등록 전에
 * 실패한 경우가 있어, 등록되지 않은 것을 해제하려 하면 안 된다.
 *
 * mmu_notifier_unregister 는 진행 중인 콜백이 끝날 때까지 기다려 준다 —
 * 그래서 이 호출이 반환한 뒤에는 도메인을 안전하게 놓을 수 있다.
 *
 * 호출 체인:
 *   도메인 해제 → 코어 → [이 함수] → mmu_notifier_unregister()
 */
static void iommu_sva_domain_free(struct iommu_domain *domain)
{
	struct protection_domain *sva_pdom = to_pdomain(domain);	/* [한국어] AMD 구조체로 */

	if (sva_pdom->mn.ops)	/* [한국어] 등록에 성공했던 경우에만 */
		mmu_notifier_unregister(&sva_pdom->mn, domain->mm);	/* [한국어] 먼저 뗀다. 진행 중인 콜백이 끝날 때까지 기다려 주므로 이후 해제가 안전하다 */

	amd_iommu_domain_free(domain);	/* [한국어] 그다음 도메인 자체를 놓는다 */
}

/*
 * [한국어] struct iommu_domain_ops amd_sva_domain_ops — SVA 도메인의 콜백 표
 *
 * 평범한 도메인의 ops 와 비교하면 없는 것이 눈에 띈다: map/unmap 이 없다.
 * SVA 도메인은 매핑을 만들지 않고 프로세스의 페이지 테이블을 빌려 쓰므로,
 * 매핑을 요청받을 일 자체가 없다. attach_dev 도 없다 — 연결은 항상 PASID
 * 단위이기 때문이다.
 *
 * 남은 둘이 SVA 도메인이 하는 일의 전부다: PASID 를 붙이고, 도메인을 놓는다.
 */
static const struct iommu_domain_ops amd_sva_domain_ops = {
	.set_dev_pasid = iommu_sva_set_dev_pasid,
	/* [한국어] PASID 하나를 이 주소 공간에 연결한다.
	 * SVA 도메인의 연결은 항상 PASID 단위라, attach_dev 는 아예 없다. */
	.free	       = iommu_sva_domain_free
	/* [한국어] 도메인을 놓는다. mmu_notifier 를 먼저 떼는 것이 이 콜백의 요점이다. */
};

/*
 * [한국어]
 * amd_iommu_domain_alloc_sva - 프로세스의 주소 공간을 나타내는 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 장치(여기서는 쓰지 않는다).
 * @mm: 공유할 프로세스의 주소 공간.
 * @return: 새 도메인, 실패하면 ERR_PTR.
 *
 * 만드는 것은 껍데기뿐이다. 페이지 테이블을 할당하지 않고, 도메인 타입을
 * IOMMU_DOMAIN_SVA 로 표시해 코어가 이것을 매핑 가능한 도메인으로 다루지
 * 않게 한다.
 *
 * 진짜 일은 mmu_notifier_register 다. 이 등록이 성공해야 프로세스의 매핑
 * 변경과 종료를 통지받을 수 있고, 그것 없이는 SVA 를 안전하게 쓸 수 없다.
 * 그래서 실패하면 도메인 자체를 놓고 오류를 돌려준다.
 *
 * pgd 를 여기서 읽지 않는 것에 유의: 실제 연결은 PASID 를 붙일 때
 * (iommu_sva_set_dev_pasid) 일어난다. 도메인은 여러 장치가 공유할 수 있어,
 * 도메인 생성과 연결이 나뉘어 있다.
 *
 * 호출 체인:
 *   iommu_sva_bind_device() → 코어 → [이 함수] → mmu_notifier_register()
 */
struct iommu_domain *amd_iommu_domain_alloc_sva(struct device *dev,
						struct mm_struct *mm)
{
	struct protection_domain *pdom;	/* [한국어] 만들 도메인 */
	int ret;	/* [한국어] 등록 결과 */

	pdom = protection_domain_alloc();	/* [한국어] 껍데기만 만든다 — 페이지 테이블은 할당하지 않는다 */
	if (!pdom)	/* [한국어] 메모리 부족 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 포인터 자리에 오류를 실어 돌려준다 */

	pdom->domain.ops = &amd_sva_domain_ops;	/* [한국어] map/unmap 이 없는 SVA 전용 콜백 표 */
	pdom->mn.ops = &sva_mn;	/* [한국어] mm 변경을 통지받을 콜백 */
	pdom->domain.type = IOMMU_DOMAIN_SVA;	/* [한국어] 코어가 이것을 매핑 가능한 도메인으로 다루지 않게 한다 */

	ret = mmu_notifier_register(&pdom->mn, mm);	/* [한국어] 진짜 일. 이 등록 없이는 SVA 를 안전하게 쓸 수 없다 */
	if (ret) {	/* [한국어] 등록 실패 */
		amd_iommu_domain_free(&pdom->domain);	/* [한국어] 도메인을 놓고 */
		return ERR_PTR(ret);	/* [한국어] 오류를 돌려준다 */
	}

	return &pdom->domain;	/* [한국어] 코어에는 도메인 부분만 보인다. 실제 연결은 PASID 를 붙일 때 일어난다 */
}
