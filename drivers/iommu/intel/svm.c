// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2015 Intel Corporation.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 */

/*
 * [한국어 설명] SVA(Shared Virtual Addressing) — 장치가 프로세스의 주소 공간을 그대로 쓰게 한다 (intel/svm.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 IOMMU 자체 페이지 테이블을 만들지 않는다. 대신 프로세스의 CPU
 * 페이지 테이블(mm->pgd)을 PASID 항목에 그대로 꽂아, 장치가 CPU 와 똑같은
 * 가상 주소로 메모리에 접근하게 한다. 그것이 SVA 다.
 * 이것이 가능한 이유는 VT-d 의 1단계 페이지 테이블 형식이 x86-64 CPU 페이지
 * 테이블과 같기 때문이다. 그래서 매핑을 만들 필요도, IOVA 를 할당할 필요도
 * 없다 — 애플리케이션이 malloc 으로 얻은 포인터를 그대로 장치에 넘기면 된다.
 * 대신 두 가지 책임이 생긴다.
 *   [1] 프로세스가 매핑을 바꾸면(munmap, 페이지 회수, 스왑) IOMMU 와 장치의
 *       캐시를 비워야 한다. mmu_notifier 가 그 통지를 받는다.
 *   [2] 아직 매핑되지 않은 주소에 장치가 접근하면 페이지 폴트를 처리해야
 *       한다. PRI 와 io-pgfault 계층이 그 일을 하며, 이 파일은 그것이
 *       가능한 장치인지 검사만 한다.
 * 실제 코드는 그 둘의 배선과, "이 장치로 SVA 를 쓸 수 있는가"의 판단이 전부다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * SVA 도메인은 다른 도메인들과 근본적으로 다르다.
 *   보통의 도메인 — 자기 페이지 테이블을 소유하고, 드라이버가 dma_map_*() 로
 *                   매핑을 하나씩 만든다.
 *   SVA 도메인   — 페이지 테이블을 소유하지 않고 프로세스의 것을 가리킨다.
 *                   매핑은 프로세스가 mmap/munmap 으로 이미 만들고 있다.
 * 그래서 struct dmar_domain 의 union 에서 SVA 는 페이지 테이블 대신
 * mmu_notifier 를 갖고, 도메인 id 도 할당받지 않는다(FLPT_DEFAULT_DID 를 쓴다).
 * 위쪽으로는 iommu-sva.c 의 코어 SVA 계층이 이 파일의 domain_alloc 을 부르고,
 * 아래쪽으로는 pasid.c 가 1단계 항목을 세우고 cache.c 가 무효화를 보낸다.
 * 실행 컨텍스트: 커널 모듈. 설정 경로는 프로세스 컨텍스트지만, mmu_notifier
 * 콜백은 mm 의 잠금 아래에서 불려 잠들 수 없다 — 그래서 무효화가 스핀락과
 * 미리 잡아 둔 배치 버퍼만 쓰도록 되어 있다.
 *
 * === 타 모듈과의 연결 ===
 * - mm 서브시스템: mmu_notifier 로 주소 공간 변경을 통보받는다. 이것이 이
 *   파일의 가장 중요한 외부 연결이다.
 * - pasid.c: __domain_setup_first_level() 로 PASID 항목에 mm->pgd 를 꽂는다.
 * - cache.c: 통지를 받으면 cache_tag_flush_range/all 로 IOTLB 와 디바이스
 *   TLB 를 비운다.
 * - iommu.c: device_domain_info 의 pasid_enabled/ats_enabled/pri_supported 가
 *   SVA 가능 여부의 근거이며, domain_add_dev_pasid 로 (장치, PASID) 를 잇는다.
 * - io-pgfault.c/prq.c: 페이지 폴트가 오면 그 주소를 프로세스의 VMA 로
 *   확인하고 페이지를 채운 뒤 장치에 재시도를 알린다.
 * 데이터 흐름: 애플리케이션이 SVA 를 요청 → 코어가 도메인을 만들고
 * mmu_notifier 등록 → PASID 항목에 mm->pgd → 장치가 그 PASID 로 DMA →
 * 폴트가 나면 io-pgfault 가 처리 → 프로세스가 매핑을 바꾸면 notifier 가
 * 캐시를 비운다 → 프로세스가 끝나면 release 콜백이 항목을 내린다.
 *
 * === 주요 함수/구조체 요약 ===
 * - intel_svm_check(): 유닛이 SVA 를 지원하는지 부팅 때 판정해
 *   VTD_FLAG_SVM_CAPABLE 을 세운다. CPU 가 쓰는 페이징 기능(1GB 페이지,
 *   5레벨)을 IOMMU 도 할 수 있어야 한다는 것이 판정 기준이다.
 * - intel_iommu_sva_supported(): 이 장치로 SVA 를 쓸 수 있는지. PASID 와
 *   ATS 는 필수이고, PRI 는 있으면 켜져 있어야 한다.
 * - intel_svm_domain_alloc(): SVA 도메인을 만들고 mmu_notifier 를 등록한다.
 * - intel_svm_set_dev_pasid(): PASID 항목에 mm->pgd 를 꽂는다.
 * - intel_arch_invalidate_secondary_tlbs(): 프로세스 매핑이 바뀌었다는 통지.
 *   이 파일에서 가장 자주 불리는 함수다.
 * - intel_mm_release(): 프로세스가 죽을 때 PASID 항목을 급히 내린다.
 * - intel_mm_free_notifier(): 도메인 해제를 RCU 유예 뒤로 미루는 콜백.
 */
#include <linux/mmu_notifier.h>	/* [한국어] 프로세스 주소 공간 변경을 통보받는 훅. 이 파일의 핵심 의존성이다 */
#include <linux/sched.h>	/* [한국어] 태스크 구조체 */
#include <linux/sched/mm.h>	/* [한국어] mm 참조를 잡고 놓는 헬퍼 */
#include <linux/slab.h>	/* [한국어] 도메인 구조체 할당 */
#include <linux/rculist.h>	/* [한국어] RCU 목록 */
#include <linux/pci.h>	/* [한국어] 소스 id 와 PCI 능력 */
#include <linux/pci-ats.h>	/* [한국어] ATS/PASID/PRI 능력 조회 */
#include <linux/dmar.h>	/* [한국어] DRHD 유닛 */
#include <linux/interrupt.h>	/* [한국어] 폴트 인터럽트 관련 타입 */
#include <linux/mm_types.h>	/* [한국어] struct mm_struct — 프로세스의 주소 공간 */
#include <linux/xarray.h>	/* [한국어] 도메인의 유닛별 정보 */
#include <asm/page.h>	/* [한국어] __pa() — mm->pgd 의 물리 주소를 구한다 */
#include <asm/fpu/api.h>	/* [한국어] FPU 상태 관련(이 파일에서는 간접 의존) */

#include "iommu.h"	/* [한국어] 유닛·도메인·장치 자료구조와 능력 판정 */
#include "pasid.h"	/* [한국어] PASID 항목 형식과 플래그 */
#include "perf.h"	/* [한국어] 지연 계측 */
#include "../iommu-pages.h"	/* [한국어] 공용 페이지 할당기 */
#include "trace.h"	/* [한국어] 추적 이벤트 */

/*
 * [한국어]
 * intel_svm_check - 이 유닛으로 SVA 를 쓸 수 있는지 판정한다
 *
 * @iommu: 검사할 DMAR 유닛.
 * @return: 없음. 가능하면 VTD_FLAG_SVM_CAPABLE 을 세운다.
 *
 * 판정 기준이 흥미롭다. SVA 는 프로세스의 CPU 페이지 테이블을 IOMMU 가
 * 그대로 워크하는 것이므로, CPU 가 쓰는 페이징 기능을 IOMMU 도 할 수 있어야
 * 한다. 하나라도 못 하면 같은 테이블을 두 하드웨어가 다르게 해석하게 된다.
 *
 *   - CPU 가 1GB 큰 페이지를 쓰는데 IOMMU 가 못 하면: 커널이나 애플리케이션이
 *     만든 1GB 매핑을 IOMMU 가 워크하지 못한다.
 *   - CPU 가 5레벨 페이징(LA57)을 쓰는데 IOMMU 가 못 하면: 상위 주소 공간의
 *     매핑에 닿지 못한다.
 *
 * 그래서 "CPU 기능이 켜져 있는데 IOMMU 가 그것을 못 하면 SVA 를 끈다"는
 * 형태의 검사가 둘 나온다. 조용히 끄지 않고 pr_err 로 이유를 남기는 것은,
 * SVA 를 기대한 드라이버가 왜 실패하는지 알 수 있게 하기 위해서다.
 *
 * 실행 컨텍스트: 유닛 초기화(init_dmars/intel_iommu_add). 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   init_dmars()/intel_iommu_add() → [intel_svm_check]
 */
void intel_svm_check(struct intel_iommu *iommu)
{
	if (!pasid_supported(iommu))	/* [한국어] PASID 를 쓸 수 없는 유닛이면 */
		return;	/* [한국어] SVA 도 불가능하다. SVA 는 PASID 로 프로세스를 구분한다 */

	if (cpu_feature_enabled(X86_FEATURE_GBPAGES) &&	/* [한국어] CPU 가 1GB 큰 페이지를 쓰는데 */
	    !cap_fl1gp_support(iommu->cap)) {	/* [한국어] IOMMU 가 1단계에서 그것을 못 하면 */
		pr_err("%s SVM disabled, incompatible 1GB page capability\n",	/* [한국어] 같은 페이지 테이블을 두 하드웨어가 다르게 해석하게 된다 */
		       iommu->name);	/* [한국어] 어느 유닛인지 */
		return;	/* [한국어] SVA 를 켜지 않는다 */
	}

	if (cpu_feature_enabled(X86_FEATURE_LA57) &&	/* [한국어] CPU 가 5레벨 페이징을 쓰는데 */
	    !cap_fl5lp_support(iommu->cap)) {	/* [한국어] IOMMU 가 못 하면 */
		pr_err("%s SVM disabled, incompatible paging mode\n",	/* [한국어] 상위 주소 공간의 매핑에 닿지 못한다 */
		       iommu->name);	/* [한국어] 어느 유닛인지 */
		return;	/* [한국어] SVA 를 켜지 않는다 */
	}

	iommu->flags |= VTD_FLAG_SVM_CAPABLE;	/* [한국어] 이 유닛으로 SVA 를 쓸 수 있다고 표시한다 */
}

/* Pages have been freed at this point */
/*
 * [한국어] (위 영어 주석에 이어)
 * intel_arch_invalidate_secondary_tlbs - 프로세스 매핑이 바뀌었으니 IOMMU 캐시를 비운다
 *
 * @mn: 등록해 둔 notifier. container_of 로 SVA 도메인을 되찾는다.
 * @mm: 그 프로세스의 주소 공간(여기서는 쓰지 않는다).
 * @start, @end: 바뀐 범위.
 * @return: 없음.
 *
 * 이 파일에서 가장 자주 불리는 함수다. 프로세스가 munmap 하거나, 커널이
 * 페이지를 회수하거나 스왑아웃할 때마다 mm 서브시스템이 이 콜백을 부른다.
 * SVA 도메인은 프로세스의 페이지 테이블을 그대로 가리키므로, 그 테이블이
 * 바뀌면 IOMMU 와 장치가 캐시한 번역도 무효가 된다.
 *
 * 위 영어 주석 "Pages have been freed at this point" 가 중요하다. 이 콜백이
 * 불릴 때는 페이지가 이미 해제된 뒤다. 즉 여기서 캐시를 비우지 않으면 장치가
 * 남의 메모리에 쓰게 된다 — 조용한 메모리 손상이다.
 *
 * 경계 처리: mm 은 vm_end 를 "끝 주소의 다음 바이트"로 쓰고 IOMMU 코드는
 * "마지막 주소"로 쓴다(코드 안 영어 주석). 그래서 end - 1 을 넘긴다. 이
 * 한 칸 차이를 놓치면 마지막 페이지의 캐시가 남는다.
 *
 * 전체 범위(0~ULONG_MAX)는 flush_all 로 빠진다 — 범위 계산 없이 도메인
 * 전체를 비우는 편이 싸다.
 *
 * 실행 컨텍스트: mm 의 잠금 아래. 잠들 수 없다. 그래서 이 경로가 쓰는
 * cache.c 의 함수들이 스핀락과 미리 잡아 둔 배치 버퍼만 쓰도록 되어 있다.
 *
 * 호출 체인:
 *   mm 서브시스템(zap_page_range 등) → mmu_notifier
 *     → [intel_arch_invalidate_secondary_tlbs]
 *     → cache_tag_flush_all()/cache_tag_flush_range()
 */
static void intel_arch_invalidate_secondary_tlbs(struct mmu_notifier *mn,
					struct mm_struct *mm,
					unsigned long start, unsigned long end)
{
	struct dmar_domain *domain = container_of(mn, struct dmar_domain, notifier);	/* [한국어] notifier 에서 그것을 품은 SVA 도메인으로 */

	if (start == 0 && end == ULONG_MAX) {	/* [한국어] 전체 범위면 */
		cache_tag_flush_all(domain);	/* [한국어] 범위 계산 없이 도메인 전체를 비운다 */
		return;	/* [한국어] 끝 */
	}

	/*
	 * The mm_types defines vm_end as the first byte after the end address,
	 * different from IOMMU subsystem using the last address of an address
	 * range.
	 */
	cache_tag_flush_range(domain, start, end - 1, 0);	/* [한국어] end - 1 인 이유: mm 은 vm_end 를 "끝의 다음 바이트"로, IOMMU 코드는 "마지막 주소"로 쓴다. 이 한 칸을 놓치면 마지막 페이지의 캐시가 남는다 (위 영어 주석) */
}

/*
 * [한국어]
 * intel_mm_release - 프로세스가 죽을 때 이 도메인의 모든 PASID 항목을 급히 내린다
 *
 * @mn: 등록해 둔 notifier. @mm: 죽는 프로세스의 주소 공간.
 * @return: 없음.
 *
 * 왜 급한가(코드 안 영어 주석): 이 콜백은 exit_mmap() 에서, 페이지 테이블이
 * 지워지기 "전에" 불릴 수 있다. 그리고 __mmu_notifier_release() 가 우리를
 * 목록에서 빼 버리므로, 이후 페이지 테이블이 실제로 지워질 때는
 * invalidate_range 콜백이 오지 않는다. 즉 이 순간이 하드웨어가 그 페이지
 * 테이블을 워크하지 못하게 막을 마지막 기회다.
 *
 * 그래서 이 도메인에 붙은 모든 (장치, PASID) 에 대해 PASID 항목을 내린다.
 * fault_ignore 를 true 로 넘기는 것이 핵심이다 — 죽는 프로세스의 주소로
 * 아직 DMA 를 내고 있는 장치가 있을 수 있는데, 그것을 전부 폴트로 보고하면
 * 로그가 뒤덮인다. 접근은 막되 조용히 막는다.
 *
 * 영어 주석의 마지막 문단은 이 방식의 한계를 인정한다: 항목을 지우면
 * 하드웨어가 당황할 수 있으니, 언젠가는 더미 PGD(제로 페이지 같은)를
 * 가리켜 하드웨어가 우아하게 처리할 수 있는 폴트를 내게 하는 편이 나을 수도
 * 있다는 것이다. 지금은 그렇게 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 종료(exit_mmap). 스핀락만 쓰며 잠들 수 없다.
 *
 * 호출 체인:
 *   exit_mmap() → __mmu_notifier_release() → [intel_mm_release]
 *     → intel_pasid_tear_down_entry(fault_ignore=true)
 */
static void intel_mm_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	struct dmar_domain *domain = container_of(mn, struct dmar_domain, notifier);	/* [한국어] notifier 에서 SVA 도메인으로 */
	struct dev_pasid_info *dev_pasid;	/* [한국어] (장치, PASID) 순회 커서 */
	struct device_domain_info *info;	/* [한국어] 그 장치의 정보 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	/* This might end up being called from exit_mmap(), *before* the page
	 * tables are cleared. And __mmu_notifier_release() will delete us from
	 * the list of notifiers so that our invalidate_range() callback doesn't
	 * get called when the page tables are cleared. So we need to protect
	 * against hardware accessing those page tables.
	 *
	 * We do it by clearing the entry in the PASID table and then flushing
	 * the IOTLB and the PASID table caches. This might upset hardware;
	 * perhaps we'll want to point the PASID to a dummy PGD (like the zero
	 * page) so that we end up taking a fault that the hardware really
	 * *has* to handle gracefully without affecting other processes.
	 */
	spin_lock_irqsave(&domain->lock, flags);	/* [한국어] dev_pasids 목록을 보호한다 */
	list_for_each_entry(dev_pasid, &domain->dev_pasids, link_domain) {	/* [한국어] 이 프로세스의 주소 공간을 쓰던 모든 (장치, PASID) 에 대해 */
		info = dev_iommu_priv_get(dev_pasid->dev);	/* [한국어] 그 장치의 유닛을 얻고 */
		intel_pasid_tear_down_entry(info->iommu, dev_pasid->dev,	/* [한국어] 항목을 내려 하드웨어가 곧 지워질 페이지 테이블을 워크하지 못하게 한다 (위 영어 주석) */
					    dev_pasid->pasid, true);	/* [한국어] true 는 fault_ignore — 죽는 프로세스의 주소로 아직 DMA 를 내는 장치가 있어도 조용히 막는다 */
	}
	spin_unlock_irqrestore(&domain->lock, flags);	/* [한국어] 락 해제 */

}

/*
 * [한국어]
 * intel_mm_free_notifier - SVA 도메인의 실제 해제를 담당하는 RCU 콜백
 *
 * @mn: 해제할 notifier. container_of 로 도메인을 되찾는다.
 * @return: 없음.
 *
 * 왜 해제를 여기서 하는가: mmu_notifier 는 RCU 로 보호된다. 등록을 해제해도
 * 다른 CPU 가 아직 콜백 목록을 순회하고 있을 수 있으므로, 유예 기간이
 * 지나기 전에 구조체를 해제하면 해제된 메모리를 읽는다.
 * mmu_notifier_put() 이 참조를 놓고, 마지막 참조가 사라지고 유예 기간이
 * 지나면 이 콜백이 불린다.
 *
 * 그래서 intel_svm_domain_free() 는 mmu_notifier_put() 만 부르고 도메인을
 * 직접 해제하지 않는다 — 그 함수의 주석이 "해제는 free_notifier 콜백으로
 * 미뤄진다"고 말하는 그대로다.
 *
 * qi_batch 를 함께 반납하는 것을 눈여겨볼 것: SVA 도메인도 무효화를 보내므로
 * 배치 버퍼를 가질 수 있고, 그것도 이 시점에 반납해야 한다.
 *
 * 실행 컨텍스트: RCU 콜백. 잠들 수 없다.
 */
static void intel_mm_free_notifier(struct mmu_notifier *mn)
{
	struct dmar_domain *domain = container_of(mn, struct dmar_domain, notifier);	/* [한국어] notifier 에서 도메인으로 */

	kfree(domain->qi_batch);	/* [한국어] 무효화 배치 버퍼 */
	kfree(domain);	/* [한국어] 도메인 자신. RCU 유예 기간이 지난 뒤라 순회 중인 CPU 가 없다 */
}

static const struct mmu_notifier_ops intel_mmuops = {	/* [한국어] mm 서브시스템이 이 도메인에 통지하는 통로 */
	.release = intel_mm_release,	/* [한국어] 프로세스가 죽을 때 */
	.arch_invalidate_secondary_tlbs = intel_arch_invalidate_secondary_tlbs,	/* [한국어] 매핑이 바뀌었을 때. 가장 자주 불린다 */
	.free_notifier = intel_mm_free_notifier,	/* [한국어] RCU 유예가 지나 실제로 해제해도 될 때 */
};

/*
 * [한국어]
 * intel_iommu_sva_supported - 이 장치로 SVA 를 쓸 수 있는지 판단한다
 *
 * @dev: 검사할 장치.
 * @return: 0 이면 가능, -EINVAL/-ENODEV 면 불가능.
 *
 * SVA 는 여러 기능이 동시에 갖춰져야 성립한다.
 *   - PASID: 프로세스를 구분하는 번호. 장치와 유닛 양쪽에서 켜져 있어야 한다.
 *   - ATS: 장치가 번역을 요청할 수 있어야 한다.
 *   - 유닛의 SVM 능력(VTD_FLAG_SVM_CAPABLE): intel_svm_check 가 부팅 때
 *     CPU 페이징 기능과 대조해 정한 값이다.
 *
 * PRI 의 처리가 흥미롭다(위 영어 주석). SVA 는 매핑을 미리 만들지 않으므로
 * 페이지 폴트 처리가 필요한데, 그것을 반드시 PCIe PRI 로 해야 하는 것은
 * 아니다. 장치가 자체적인 폴트 처리 방식을 갖고 있을 수 있고, IOMMU 쪽에서는
 * 그것을 확인할 방법이 없다. 그래서 규약을 이렇게 정했다.
 *   - PRI 를 지원하지 않는 장치 → 허용한다. 드라이버가 알아서 폴트를
 *     처리한다는 뜻으로 본다.
 *   - PRI 를 지원하는 장치 → 반드시 켜져 있어야 한다. 지원하면서 켜지 않은
 *     것은 설정 오류다.
 *
 * 실행 컨텍스트: SVA 설정. 프로세스 컨텍스트. 순수 조회다.
 *
 * 호출 체인:
 *   intel_svm_domain_alloc()/intel_svm_set_dev_pasid()
 *     → [intel_iommu_sva_supported]
 */
static int intel_iommu_sva_supported(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu;	/* [한국어] 담당 유닛 */

	if (!info || dmar_disabled)	/* [한국어] 프로브되지 않았거나 VT-d 가 꺼져 있으면 */
		return -EINVAL;	/* [한국어] SVA 를 쓸 수 없다 */

	iommu = info->iommu;	/* [한국어] 담당 유닛 */
	if (!iommu)	/* [한국어] 없으면 */
		return -EINVAL;	/* [한국어] IOMMU 아래가 아니다 */

	if (!(iommu->flags & VTD_FLAG_SVM_CAPABLE))	/* [한국어] 유닛이 SVA 를 못 하면 (intel_svm_check 가 CPU 페이징 기능과 대조해 정한 값) */
		return -ENODEV;	/* [한국어] 불가능 */

	if (!info->pasid_enabled || !info->ats_enabled)	/* [한국어] PASID 나 ATS 가 꺼져 있으면 */
		return -EINVAL;	/* [한국어] SVA 의 전제가 갖춰지지 않았다 */

	/*
	 * Devices having device-specific I/O fault handling should not
	 * support PCI/PRI. The IOMMU side has no means to check the
	 * capability of device-specific IOPF.  Therefore, IOMMU can only
	 * default that if the device driver enables SVA on a non-PRI
	 * device, it will handle IOPF in its own way.
	 */
	if (!info->pri_supported)	/* [한국어] PRI 를 지원하지 않는 장치면 (위 영어 주석) */
		return 0;	/* [한국어] 허용한다 — 드라이버가 자체적으로 폴트를 처리한다는 뜻으로 본다 */

	/* Devices supporting PRI should have it enabled. */
	if (!info->pri_enabled)	/* [한국어] 지원하는데 켜지 않았으면 (위 영어 주석) */
		return -EINVAL;	/* [한국어] 설정 오류다 */

	return 0;	/* [한국어] 이 장치로 SVA 를 쓸 수 있다 */
}

/*
 * [한국어]
 * intel_svm_set_dev_pasid - PASID 항목에 프로세스의 페이지 테이블을 꽂는다
 *
 * @domain: SVA 도메인. domain->mm 이 그 프로세스의 주소 공간이다.
 * @dev: 장치. @pasid: 이 프로세스에 배정된 PASID.
 * @old: 이 PASID 가 쓰고 있던 도메인.
 * @return: 0 성공, 음수면 실패(옛 상태가 그대로 유지된다).
 *
 * SVA 의 핵심 한 줄은 __domain_setup_first_level 에 넘기는 __pa(mm->pgd) 다.
 * IOMMU 페이지 테이블을 만드는 대신 프로세스의 CPU 페이지 테이블 주소를
 * 그대로 하드웨어에 알려 준다. 1단계 형식이 x86-64 CPU 형식과 같아서
 * 가능한 일이다.
 *
 * 도메인 id 로 FLPT_DEFAULT_DID 를 쓰는 것도 SVA 의 성격을 보여 준다 —
 * SVA 도메인은 자기 주소 공간을 소유하지 않으므로 도메인 id 를 할당받지
 * 않고, 1단계·통과 전용으로 예약된 값을 공유한다.
 *
 * 플래그 두 개:
 *   FL5LP  — CPU 가 5레벨 페이징을 쓰면 IOMMU 도 5레벨로 워크해야 한다.
 *            프로세스 테이블의 깊이와 반드시 일치해야 한다.
 *   PWSNP  — 페이지 워크가 CPU 캐시를 스누프하게 한다. 프로세스가 방금 고친
 *            페이지 테이블을 IOMMU 가 곧바로 보려면 필요하다.
 *
 * 폴트 처리 연결이 조건부다: PRI 를 지원하는 장치만 iopf 큐에 붙인다.
 * 지원하지 않는 장치는 드라이버가 자체적으로 폴트를 처리한다는 전제이므로
 * (sva_supported 주석 참고) 코어의 폴트 경로를 쓰지 않는다.
 *
 * 교체 순서는 다른 set_dev_pasid 들과 같다: 준비 → 폴트 처리 이동 →
 * 하드웨어 설정 → 성공 후 옛 기록 제거. 실패하면 정확히 역순으로 되돌린다.
 *
 * 실행 컨텍스트: SVA 바인딩. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_sva_bind_device() → 코어 → [intel_svm_set_dev_pasid]
 *     → domain_add_dev_pasid() → __domain_setup_first_level()
 */
static int intel_svm_set_dev_pasid(struct iommu_domain *domain,
				   struct device *dev, ioasid_t pasid,
				   struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct mm_struct *mm = domain->mm;	/* [한국어] 공유할 프로세스의 주소 공간 */
	struct dev_pasid_info *dev_pasid;	/* [한국어] 만들 (장치, PASID) 기록 */
	unsigned long sflags;	/* [한국어] PASID 항목 설정 플래그 */
	int ret = 0;	/* [한국어] 각 단계 결과 */

	ret = intel_iommu_sva_supported(dev);	/* [한국어] 이 장치로 SVA 를 쓸 수 있는지 */
	if (ret)	/* [한국어] 불가능하면 */
		return ret;	/* [한국어] 이유를 전달 */

	dev_pasid = domain_add_dev_pasid(domain, dev, pasid);	/* [한국어] 도메인 id 확보와 무효화 대상 등록 */
	if (IS_ERR(dev_pasid))	/* [한국어] 실패 */
		return PTR_ERR(dev_pasid);	/* [한국어] 전달 */

	/* SVA with non-IOMMU/PRI IOPF handling is allowed. */
	if (info->pri_supported) {	/* [한국어] PRI 를 쓰는 장치만 (위 영어 주석) */
		ret = iopf_for_domain_replace(domain, old, dev);	/* [한국어] 코어의 폴트 처리에 연결한다 */
		if (ret)	/* [한국어] 실패 */
			goto out_remove_dev_pasid;	/* [한국어] 방금 만든 기록을 되돌린다 */
	}

	/* Setup the pasid table: */
	sflags = cpu_feature_enabled(X86_FEATURE_LA57) ? PASID_FLAG_FL5LP : 0;	/* [한국어] CPU 가 5레벨 페이징을 쓰면 IOMMU 도 5레벨로 워크해야 한다. 프로세스 테이블의 깊이와 반드시 일치해야 한다 */
	sflags |= PASID_FLAG_PWSNP;	/* [한국어] 페이지 워크가 CPU 캐시를 스누프하게 한다. 프로세스가 방금 고친 테이블을 IOMMU 가 곧바로 보려면 필요하다 */
	ret = __domain_setup_first_level(iommu, dev, pasid,	/* [한국어] SVA 의 핵심 — PASID 항목에 */
					 FLPT_DEFAULT_DID, __pa(mm->pgd),	/* [한국어] 프로세스의 CPU 페이지 테이블 주소를 그대로 꽂는다. 도메인 id 는 예약값을 쓴다 — SVA 도메인은 자기 주소 공간을 소유하지 않는다 */
					 sflags, old);	/* [한국어] 플래그와 옛 도메인(원자적 교체용) */
	if (ret)	/* [한국어] 하드웨어 설정 실패 */
		goto out_unwind_iopf;	/* [한국어] 폴트 처리를 되돌린다 */

	domain_remove_dev_pasid(old, dev, pasid);	/* [한국어] 성공한 뒤에야 옛 도메인의 기록을 지운다 */

	return 0;	/* [한국어] 이 PASID 로 오는 DMA 가 프로세스의 주소 공간을 쓴다 */
out_unwind_iopf:	/* [한국어] 하드웨어 설정 실패 경로 */
	if (info->pri_supported)	/* [한국어] 연결했던 경우에만 */
		iopf_for_domain_replace(old, domain, dev);	/* [한국어] 폴트 처리를 되돌린다 */
out_remove_dev_pasid:	/* [한국어] 폴트 연결 실패가 합류 */
	domain_remove_dev_pasid(domain, dev, pasid);	/* [한국어] 새 도메인의 기록을 지운다 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * intel_svm_domain_free - SVA 도메인 해제를 RCU 유예 뒤로 미룬다
 *
 * @domain: 해제할 SVA 도메인.
 * @return: 없음.
 *
 * 여기서 kfree 하지 않는다(위 영어 주석). mmu_notifier 는 RCU 로 보호되어,
 * 등록을 해제해도 다른 CPU 가 아직 콜백 목록을 순회하고 있을 수 있다.
 * mmu_notifier_put() 이 참조를 놓고, 유예 기간이 지나면
 * intel_mm_free_notifier() 가 불려 그때 실제로 해제된다.
 *
 * 다른 도메인들의 free 콜백이 그 자리에서 페이지 테이블과 구조체를 반납하는
 * 것과 대비된다 — SVA 도메인은 페이지 테이블을 소유하지 않는 대신
 * mm 서브시스템과 얽혀 있어 수명 관리가 그쪽 규약을 따른다.
 *
 * 실행 컨텍스트: 도메인 해제. 프로세스 컨텍스트.
 */
static void intel_svm_domain_free(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */

	/* dmar_domain free is deferred to the mmu free_notifier callback. */
	mmu_notifier_put(&dmar_domain->notifier);	/* [한국어] 참조만 놓는다. 실제 해제는 RCU 유예가 지난 뒤 free_notifier 콜백이 한다 (위 영어 주석) */
}

static const struct iommu_domain_ops intel_svm_domain_ops = {	/* [한국어] SVA 도메인의 콜백 표. 다른 도메인들과 달리 map/unmap 이 없다 — 매핑은 프로세스가 관리한다 */
	.set_dev_pasid		= intel_svm_set_dev_pasid,	/* [한국어] PASID 에 프로세스 페이지 테이블을 꽂는다 */
	.free			= intel_svm_domain_free	/* [한국어] 해제(RCU 유예 뒤로 미룬다) */
};

/*
 * [한국어]
 * intel_svm_domain_alloc - SVA 도메인을 만들고 프로세스에 mmu_notifier 를 건다
 *
 * @dev: 이 도메인을 쓸 장치. @mm: 공유할 프로세스의 주소 공간.
 * @return: 만들어진 도메인, 실패 시 ERR_PTR.
 *
 * 보통의 도메인 생성(paging_domain_alloc)과 비교하면 무엇이 SVA 인지 잘
 * 드러난다. 이 함수는 페이지 테이블을 만들지 않고, iommu_array 도 devices
 * 목록도 초기화하지 않는다. 대신 mmu_notifier 를 등록한다.
 *
 * 초기화하는 것은 넷뿐이다: 콜백 표, PASID 쌍 목록, 무효화 대상 목록, 그리고
 * 두 락. 이것이 SVA 도메인이 실제로 필요로 하는 상태의 전부다 — 매핑은
 * 프로세스가 관리하고, 이 도메인은 "어느 장치의 어느 PASID 가 이 프로세스를
 * 쓰는가"와 "그때 어디에 무효화를 보내야 하는가"만 알면 된다.
 *
 * mmu_notifier_register 가 마지막인 이유: 등록하는 순간부터 콜백이 올 수
 * 있으므로, 그 콜백이 참조할 목록과 락이 먼저 준비되어 있어야 한다.
 * 등록에 실패하면 아직 콜백이 온 적이 없으므로 그냥 kfree 해도 안전하다
 * (성공한 뒤라면 mmu_notifier_put 을 거쳐야 한다).
 *
 * 실행 컨텍스트: SVA 바인딩. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_sva_bind_device() → iommu_ops.domain_alloc_sva
 *     → [intel_svm_domain_alloc] → mmu_notifier_register()
 */
struct iommu_domain *intel_svm_domain_alloc(struct device *dev,
					    struct mm_struct *mm)
{
	struct dmar_domain *domain;	/* [한국어] 만들 도메인 */
	int ret;	/* [한국어] 결과 */

	ret = intel_iommu_sva_supported(dev);	/* [한국어] 이 장치로 SVA 를 쓸 수 있는지 먼저 확인한다 */
	if (ret)	/* [한국어] 불가능하면 */
		return ERR_PTR(ret);	/* [한국어] 이유를 전달 */

	domain = kzalloc_obj(*domain);	/* [한국어] 도메인 구조체 */
	if (!domain)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 전달 */

	domain->domain.ops = &intel_svm_domain_ops;	/* [한국어] SVA 전용 콜백 표 */
	INIT_LIST_HEAD(&domain->dev_pasids);	/* [한국어] 어느 장치의 어느 PASID 가 이 프로세스를 쓰는지 */
	INIT_LIST_HEAD(&domain->cache_tags);	/* [한국어] 무효화를 어디로 보낼지. 페이지 테이블을 만들지 않으므로 이 넷이 필요한 상태의 전부다 */
	spin_lock_init(&domain->cache_lock);	/* [한국어] 무효화 대상 목록을 지키는 락 */
	spin_lock_init(&domain->lock);	/* [한국어] PASID 쌍 목록을 지키는 락 */

	domain->notifier.ops = &intel_mmuops;	/* [한국어] mm 서브시스템이 부를 콜백들 */
	ret = mmu_notifier_register(&domain->notifier, mm);	/* [한국어] 등록하는 순간부터 콜백이 올 수 있으므로, 그것이 참조할 목록과 락이 먼저 준비되어 있어야 한다 */
	if (ret) {	/* [한국어] 등록 실패 */
		kfree(domain);	/* [한국어] 아직 콜백이 온 적이 없으므로 그냥 해제해도 안전하다. 성공한 뒤라면 mmu_notifier_put 을 거쳐야 한다 */
		return ERR_PTR(ret);	/* [한국어] 실패 전달 */
	}

	return &domain->domain;	/* [한국어] 코어가 다루는 도메인 포인터로 반환 */
}
