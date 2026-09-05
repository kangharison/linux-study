// SPDX-License-Identifier: GPL-2.0
/*
 * intel-pasid.c - PASID idr, table and entry manipulation
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 */

/*
 * [한국어 설명] scalable 모드 PASID 테이블과 항목의 조작 (intel/pasid.c)
 *
 * === 파일의 역할 ===
 * pasid.h 가 정의한 자료구조를 실제로 만들고, 채우고, 내리는 파일이다.
 * scalable 모드에서 번역은 PASID 항목에서 시작하므로, "이 장치의 이 PASID 가
 * 어떤 주소 공간을 쓰는가"를 정하는 일이 전부 여기서 일어난다.
 * 크게 네 가지를 한다.
 *   [1] 테이블 수명: 장치마다 PASID 디렉터리를 만들고 반납한다
 *       (intel_pasid_alloc_table / free_table). 그 아래 PASID 테이블은
 *       필요할 때 한 페이지씩 늘어난다 — 백만 개를 미리 잡을 수는 없다.
 *   [2] 항목 설정: 변환 종류별로 네 가지 진입점이 있다. 1단계, 2단계, 통과,
 *       중첩. 각각 pasid_pte_config_* 가 비트를 조립하고 그 위의 setup_*
 *       함수가 락·검증·캐시 무효화를 감싼다.
 *   [3] 항목 해제: intel_pasid_tear_down_entry 가 present 를 지우고 관련
 *       캐시를 순서대로 비운다. 그 순서가 이 파일에서 가장 중요한 부분이다.
 *   [4] 컨텍스트 연결: intel_pasid_setup_sm_context 가 컨텍스트 항목이
 *       이 장치의 PASID 디렉터리를 가리키게 만든다 — scalable 모드로
 *       들어가는 관문이며, PCI 별칭까지 모두 설정해야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 번역 사슬에서 이 파일이 다루는 구간은 다음과 같다.
 *   컨텍스트 항목 → [PASID 디렉터리] → [PASID 테이블] → [PASID 항목]
 *   → 페이지 테이블
 * 위쪽으로는 iommu.c 의 도메인 부착(domain_setup_first_level 등)과 장치
 * 프로브가 이 파일의 함수를 부르고, svm.c 의 SVA 설정과 nested.c 의 중첩
 * 도메인도 여기로 모인다. 아래쪽으로는 cache.c 의 무효화와 dmar.c 의
 * qi_flush_* 를 불러 하드웨어 캐시를 비운다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 항목 조작은 iommu->lock(스핀락) 아래에서
 * 하고, 캐시 무효화는 락 밖에서 한다 — 무효화는 하드웨어 완료를 기다리므로
 * 락을 오래 쥐면 다른 장치의 부착을 막는다.
 *
 * === 타 모듈과의 연결 ===
 * - pasid.h: 항목의 비트 배치와 pasid_set_* 헬퍼. 이 파일은 그 헬퍼를 조합해
 *   하나의 완결된 항목을 만든다.
 * - iommu.c: 장치 프로브가 테이블을 만들고, 도메인 부착이 항목을 세운다.
 *   device_domain_info 의 pasid_table 필드가 두 파일을 잇는다.
 * - iommu.h: 컨텍스트 항목의 context_set_sm_* 계열로 디렉터리를 연결하고,
 *   qi_flush_* 로 무효화를 보낸다.
 * - cache.c: 항목을 고친 뒤 그 PASID 의 IOTLB 와 디바이스 TLB 를 비운다.
 * - iommu-pages.h: 테이블 페이지를 잡고 반납하는 공용 할당기.
 * 데이터 흐름: 프로브에서 디렉터리 생성 → 컨텍스트 항목이 그것을 가리키게
 * 설정 → 도메인 부착 시 해당 PASID 항목에 페이지 테이블 주소와 정책 기록 →
 * present 세움 → 캐시 무효화 → 그때부터 그 PASID 의 DMA 가 번역된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - intel_pasid_alloc_table()/free_table(): 장치의 PASID 디렉터리 수명.
 * - intel_pasid_get_entry(): PASID 번호로 항목의 주소를 얻는다. 그 구간의
 *   테이블이 없으면 그 자리에서 한 페이지를 만들어 디렉터리에 매단다.
 * - intel_pasid_setup_first_level()/second_level()/pass_through()/nested():
 *   변환 종류별 항목 설정. 각각 대응하는 pasid_pte_config_* 가 비트를 짠다.
 * - intel_pasid_tear_down_entry(): 항목을 내리고 캐시를 비운다. PASID 캐시 →
 *   IOTLB → 디바이스 TLB 순서와, 중첩이면 2단계까지 비우는 판단이 핵심이다.
 * - pasid_flush_caches(): 항목을 새로 세운 뒤의 무효화. 캐싱 모드 하드웨어는
 *   "없음"까지 캐시하므로 새 항목도 알려야 한다.
 * - intel_pasid_setup_sm_context(): 컨텍스트 항목을 PASID 디렉터리에 연결.
 *   PCI 별칭마다 반복해야 하므로 pci_for_each_dma_alias 를 쓴다.
 * - intel_context_flush_no_pasid(): PASID 를 쓰지 않는 기본 트래픽의 캐시를
 *   비운다. 컨텍스트 항목 자체가 바뀌었을 때 쓴다.
 */
#define pr_fmt(fmt)	"DMAR: " fmt

#include <linux/bitops.h>
#include <linux/cpufeature.h>
#include <linux/dmar.h>
#include <linux/iommu.h>
#include <linux/memory.h>
#include <linux/pci.h>
#include <linux/pci-ats.h>
#include <linux/spinlock.h>

#include "iommu.h"
#include "pasid.h"
#include "../iommu-pages.h"

/*
 * Intel IOMMU system wide PASID name space:
 */
u32 intel_pasid_max_id = PASID_MAX;

/*
 * Per device pasid table management:
 */

/*
 * Allocate a pasid table for @dev. It should be called in a
 * single-thread context.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * intel_pasid_alloc_table - 장치의 PASID 디렉터리를 만든다
 *
 * @dev: 대상 장치(PCI 여야 한다).
 * @return: 0 성공, -ENODEV/-EEXIST/-ENOMEM.
 *
 * scalable 모드에서 이 장치의 번역이 시작될 자리를 마련한다. 컨텍스트 항목이
 * 가리키게 될 디렉터리이며, 그 아래 PASID 테이블들은 필요할 때 하나씩 늘어난다.
 *
 * 크기 계산이 눈여겨볼 부분이다.
 *   size  = max_pasid >> (PASID_PDE_SHIFT - 3)
 * PASID 하나가 디렉터리 항목 하나를 쓰는 것이 아니다 — 하위 6비트는 테이블
 * 안을 색인하므로 디렉터리 항목 하나가 64개 PASID 를 담당하고, 항목 하나가
 * 8바이트다. 그래서 (max_pasid / 64) * 8 = max_pasid >> 3 이 되는데,
 * 코드가 >> (6-3) 으로 쓴 것이 그 계산이다.
 * 반대 방향인 max_pasid = 1 << (order + PAGE_SHIFT + 3) 도 같은 관계다.
 *
 * max_pasid 는 장치가 지원하는 수(pci_max_pasids)와 시스템 상한
 * (intel_pasid_max_id) 중 작은 쪽이다. PASID 를 지원하지 않는 장치면 0 이라
 * order 도 0 이 되어 최소 한 페이지만 잡는다 — PASID 를 안 써도
 * IOMMU_NO_PASID(0) 항목은 필요하기 때문이다.
 *
 * 비코히런트 유닛에서 clflush 하는 이유: 갓 만든 0 으로 채워진 디렉터리가
 * CPU 캐시에만 있으면 하드웨어가 쓰레기를 present 로 오해할 수 있다.
 *
 * 실행 컨텍스트: 장치 프로브. might_sleep() 이 명시하듯 잠들 수 있는 문맥이며,
 * 위 영어 주석대로 단일 스레드에서 불려야 한다(같은 장치를 두 번 프로브하지
 * 않는다는 전제).
 *
 * 호출 체인:
 *   intel_iommu_probe_device() → [intel_pasid_alloc_table]
 *     → iommu_alloc_pages_node_sz()
 */
int intel_pasid_alloc_table(struct device *dev)
{
	struct device_domain_info *info;	/* [한국어] 장치 정보 */
	struct pasid_table *pasid_table;	/* [한국어] 만들 테이블 구조체 */
	struct pasid_dir_entry *dir;	/* [한국어] 디렉터리 페이지 */
	u32 max_pasid = 0;	/* [한국어] 이 장치가 쓸 수 있는 PASID 상한 */
	int order, size;	/* [한국어] 디렉터리 크기(바이트)와 그 페이지 차수 */

	might_sleep();	/* [한국어] 이 함수는 잠들 수 있는 문맥에서만 불려야 한다고 명시한다 */
	info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보를 얻는다 */
	if (WARN_ON(!info || !dev_is_pci(dev)))	/* [한국어] 프로브되지 않았거나 PCI 가 아니면 */
		return -ENODEV;	/* [한국어] 호출자 버그다 */
	if (WARN_ON(info->pasid_table))	/* [한국어] 이미 만들어져 있으면 */
		return -EEXIST;	/* [한국어] 두 번 만들면 앞의 것이 누수된다 */

	pasid_table = kzalloc_obj(*pasid_table);	/* [한국어] 테이블 구조체 */
	if (!pasid_table)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 프로브 실패 */

	if (info->pasid_supported)	/* [한국어] 장치가 PASID 를 지원하면 */
		max_pasid = min_t(u32, pci_max_pasids(to_pci_dev(dev)),	/* [한국어] 장치가 지원하는 수와 */
				  intel_pasid_max_id);	/* [한국어] 시스템 상한 중 작은 쪽을 쓴다 */

	size = max_pasid >> (PASID_PDE_SHIFT - 3);	/* [한국어] 디렉터리의 바이트 크기. 항목 하나가 64개 PASID 를 담당하고(하위 6비트) 8바이트이므로 max_pasid/64*8 이며, 그것이 >> (6-3) 이다 */
	order = size ? get_order(size) : 0;	/* [한국어] 그 크기를 담을 페이지 차수. PASID 를 안 쓰는 장치도 0 번 항목이 필요해 최소 한 페이지를 잡는다 */
	dir = iommu_alloc_pages_node_sz(info->iommu->node, GFP_KERNEL,	/* [한국어] 유닛과 가까운 노드에서 잡는다 — 하드웨어가 매 번역마다 읽는 메모리다 */
					1 << (order + PAGE_SHIFT));	/* [한국어] 그 차수만큼의 크기로 */
	if (!dir) {	/* [한국어] 할당 실패 */
		kfree(pasid_table);	/* [한국어] 구조체를 반납하고 */
		return -ENOMEM;	/* [한국어] 프로브 실패 */
	}

	pasid_table->table = dir;	/* [한국어] 디렉터리를 연결 */
	pasid_table->max_pasid = 1 << (order + PAGE_SHIFT + 3);	/* [한국어] 실제로 잡은 크기에서 상한을 역산한다. 위 size 계산의 역방향이다 */
	info->pasid_table = pasid_table;	/* [한국어] 장치에 매단다. 이 시점부터 intel_pasid_get_table 이 이것을 돌려준다 */

	if (!ecap_coherent(info->iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
		clflush_cache_range(pasid_table->table, (1 << order) * PAGE_SIZE);	/* [한국어] 0 으로 채워진 디렉터리를 메모리로 밀어낸다. 그러지 않으면 하드웨어가 쓰레기를 present 로 오해할 수 있다 */

	return 0;	/* [한국어] 디렉터리 준비 완료 */
}

/*
 * [한국어]
 * intel_pasid_free_table - 디렉터리와 그 아래 모든 PASID 테이블을 반납한다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * 두 단계로 해제한다: 디렉터리 항목을 훑으며 그 아래 매달린 테이블 페이지를
 * 먼저 반납하고, 마지막에 디렉터리 자신을 반납한다. 순서를 바꾸면 이미
 * 해제된 디렉터리를 읽어 테이블 주소를 얻으려 하게 된다.
 *
 * info->pasid_table 을 먼저 NULL 로 만드는 것이 중요하다. 이 뒤로
 * intel_pasid_get_table() 이 NULL 을 돌려주므로, 해제 중인 테이블을 새로
 * 참조하는 경로가 생기지 않는다.
 *
 * 호출 전 조건: 이 장치의 모든 PASID 항목이 이미 내려가 있어야 한다.
 * 하드웨어가 아직 이 테이블을 워크하는 중에 반납하면, 재사용된 메모리를
 * PASID 항목으로 해석한다. intel_iommu_release_device() 가
 * intel_pasid_teardown_sm_context() 를 먼저 부르는 이유가 그것이다.
 *
 * 실행 컨텍스트: 장치 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_release_device() → [intel_pasid_free_table]
 *     → get_pasid_table_from_pde() → iommu_free_pages()
 */
void intel_pasid_free_table(struct device *dev)
{
	struct device_domain_info *info;	/* [한국어] 장치 정보 */
	struct pasid_table *pasid_table;	/* [한국어] 반납할 테이블 구조체 */
	struct pasid_dir_entry *dir;	/* [한국어] 디렉터리 */
	struct pasid_entry *table;	/* [한국어] 그 아래 PASID 테이블 */
	int i, max_pde;	/* [한국어] 디렉터리 순회 */

	info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	if (!info || !dev_is_pci(dev) || !info->pasid_table)	/* [한국어] 만든 적이 없으면 */
		return;	/* [한국어] 반납할 것도 없다 */

	pasid_table = info->pasid_table;	/* [한국어] 지역 변수로 옮기고 */
	info->pasid_table = NULL;	/* [한국어] 장치에서 먼저 떼어 낸다 — 이 뒤로 intel_pasid_get_table 이 NULL 을 돌려주므로 해제 중인 테이블을 새로 참조하는 경로가 생기지 않는다 */

	/* Free scalable mode PASID directory tables: */
	dir = pasid_table->table;	/* [한국어] 디렉터리 (위 영어 주석) */
	max_pde = pasid_table->max_pasid >> PASID_PDE_SHIFT;	/* [한국어] 디렉터리 항목 수 = PASID 상한 / 64 */
	for (i = 0; i < max_pde; i++) {	/* [한국어] 항목마다 */
		table = get_pasid_table_from_pde(&dir[i]);	/* [한국어] 매달린 테이블이 있으면 */
		iommu_free_pages(table);	/* [한국어] 그 페이지를 반납한다. NULL 이면 아무것도 하지 않는다 */
	}

	iommu_free_pages(pasid_table->table);	/* [한국어] 마지막으로 디렉터리 자신. 순서를 바꾸면 해제된 디렉터리에서 테이블 주소를 읽게 된다 */
	kfree(pasid_table);	/* [한국어] 구조체 반납 */
}

/*
 * [한국어]
 * intel_pasid_get_table - 장치에 매달린 PASID 테이블 구조체를 얻는다
 *
 * @dev: 대상 장치.
 * @return: 그 장치의 struct pasid_table, 없으면 NULL.
 *
 * NULL 이 돌아오는 경우가 둘이다: 아직 프로브되지 않았거나(info 가 없다),
 * PASID 테이블이 해제된 뒤다. 두 경우 모두 그 장치의 PASID 항목을 만질 수
 * 없다는 뜻이라 호출자가 반드시 확인해야 한다.
 *
 * 실행 컨텍스트: 어디서든. 순수 조회.
 */
struct pasid_table *intel_pasid_get_table(struct device *dev)
{
	struct device_domain_info *info;	/* [한국어] 장치 정보 */

	info = dev_iommu_priv_get(dev);	/* [한국어] 장치에 매달린 정보 */
	if (!info)	/* [한국어] 아직 프로브되지 않았으면 */
		return NULL;	/* [한국어] 테이블도 없다 */

	return info->pasid_table;	/* [한국어] 해제된 뒤에도 NULL 이다 */
}

/*
 * [한국어]
 * intel_pasid_get_dev_max_id - 이 장치가 쓸 수 있는 PASID 의 상한을 얻는다
 *
 * @dev: 대상 장치.
 * @return: 상한값. 테이블이 없으면 0.
 *
 * 디렉터리를 만들 때 정해진 max_pasid 를 돌려준다. 항목을 색인하기 전의
 * 범위 검사에 쓰이며, 이 값을 넘는 PASID 로 색인하면 디렉터리 밖의 메모리를
 * 읽게 된다 — intel_pasid_get_entry 가 WARN 을 남기고 거절하는 이유다.
 *
 * 실행 컨텍스트: 어디서든. 순수 조회.
 */
static int intel_pasid_get_dev_max_id(struct device *dev)
{
	struct device_domain_info *info;	/* [한국어] 장치 정보 */

	info = dev_iommu_priv_get(dev);	/* [한국어] 장치에 매달린 정보 */
	if (!info || !info->pasid_table)	/* [한국어] 테이블이 없으면 */
		return 0;	/* [한국어] 쓸 수 있는 PASID 가 없다 */

	return info->pasid_table->max_pasid;	/* [한국어] 디렉터리를 만들 때 정해진 상한 */
}

/*
 * [한국어]
 * intel_pasid_get_entry - PASID 번호로 그 항목의 주소를 얻는다(없으면 테이블을 만든다)
 *
 * @dev: 대상 장치. @pasid: 찾을 PASID.
 * @return: 그 PASID 의 항목 주소, 실패 시 NULL.
 *
 * 2단 색인이 여기서 드러난다. PASID 의 상위 비트(>> PASID_PDE_SHIFT)로
 * 디렉터리를, 하위 6비트(& PASID_PTE_MASK)로 테이블 안을 색인한다.
 *
 * 그 구간의 테이블이 아직 없으면 그 자리에서 한 페이지를 만들어 디렉터리에
 * 매단다. 백만 개의 PASID 를 위해 테이블을 미리 다 잡을 수는 없으므로,
 * 실제로 쓰이는 구간만 늘려 가는 구조다.
 *
 * cmpxchg 로 매다는 이유(위 영어 주석): 다른 CPU 가 같은 구간의 테이블을
 * 동시에 만들 수 있다. 그때 둘 다 페이지를 잡아 하나만 성공해야 하는데,
 * try_cmpxchg64 가 "아직 0 이면 내 것을 넣는다"를 원자적으로 해 준다.
 * 실패하면 내가 잡은 페이지를 버리고 남의 것을 쓰러 retry 로 돌아간다.
 * 디렉터리 항목은 한 번 채워지면 장치 해제 전까지 지워지지 않으므로,
 * 해제와의 경쟁은 걱정하지 않아도 된다는 것이 영어 주석의 설명이다.
 *
 * GFP_ATOMIC 인 것은 이 함수가 iommu->lock 을 쥔 채 불릴 수 있기 때문이다.
 *
 * 실행 컨텍스트: 항목 설정·해제 경로. 잠들면 안 된다.
 *
 * 호출 체인:
 *   intel_pasid_setup_*()/tear_down_entry() → [intel_pasid_get_entry]
 *     → get_pasid_table_from_pde() → try_cmpxchg64()
 */
static struct pasid_entry *intel_pasid_get_entry(struct device *dev, u32 pasid)
{
	struct device_domain_info *info;	/* [한국어] 장치 정보 */
	struct pasid_table *pasid_table;	/* [한국어] 이 장치의 디렉터리 */
	struct pasid_dir_entry *dir;	/* [한국어] 디렉터리 배열 */
	struct pasid_entry *entries;	/* [한국어] 그 아래 PASID 테이블 */
	int dir_index, index;	/* [한국어] 2단 색인의 두 인덱스 */

	pasid_table = intel_pasid_get_table(dev);	/* [한국어] 이 장치의 디렉터리 */
	if (WARN_ON(!pasid_table || pasid >= intel_pasid_get_dev_max_id(dev)))	/* [한국어] 테이블이 없거나 PASID 가 상한을 넘으면 */
		return NULL;	/* [한국어] 디렉터리 밖을 읽게 되므로 거절한다 */

	dir = pasid_table->table;	/* [한국어] 디렉터리 배열 */
	info = dev_iommu_priv_get(dev);	/* [한국어] 할당에 쓸 NUMA 노드를 얻으려고 */
	dir_index = pasid >> PASID_PDE_SHIFT;	/* [한국어] 상위 비트로 디렉터리를 색인 */
	index = pasid & PASID_PTE_MASK;	/* [한국어] 하위 6비트로 테이블 안을 색인 */

retry:	/* [한국어] 다른 CPU 가 먼저 테이블을 만든 경우 여기로 돌아온다 */
	entries = get_pasid_table_from_pde(&dir[dir_index]);	/* [한국어] 그 구간의 테이블이 이미 있는지 */
	if (!entries) {	/* [한국어] 없으면 지금 만든다 */
		u64 tmp;	/* [한국어] cmpxchg 의 기대값 */

		entries = iommu_alloc_pages_node_sz(info->iommu->node,	/* [한국어] 유닛과 가까운 노드에서 */
						    GFP_ATOMIC, SZ_4K);	/* [한국어] 한 페이지. 락을 쥔 채 불릴 수 있어 ATOMIC 이다 */
		if (!entries)	/* [한국어] 할당 실패 */
			return NULL;	/* [한국어] 디렉터리 밖을 읽게 되므로 거절한다 */

		if (!ecap_coherent(info->iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
			clflush_cache_range(entries, VTD_PAGE_SIZE);	/* [한국어] 0 으로 채워진 테이블을 메모리로 밀어낸다 */

		/*
		 * The pasid directory table entry won't be freed after
		 * allocation. No worry about the race with free and
		 * clear. However, this entry might be populated by others
		 * while we are preparing it. Use theirs with a retry.
		 */
		tmp = 0ULL;	/* [한국어] "아직 비어 있다"를 기대값으로 */
		if (!try_cmpxchg64(&dir[dir_index].val, &tmp,	/* [한국어] 원자적으로 매단다. 다른 CPU 가 같은 구간의 테이블을 동시에 만들 수 있어서다 (위 영어 주석) */
				   (u64)virt_to_phys(entries) | PASID_PTE_PRESENT)) {	/* [한국어] 물리 주소와 present 비트를 함께 넣는다 */
			iommu_free_pages(entries);	/* [한국어] 졌으면 내가 잡은 페이지를 버리고 */
			goto retry;	/* [한국어] 남이 만든 것을 쓰러 돌아간다 */
		}
		if (!ecap_coherent(info->iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
			clflush_cache_range(&dir[dir_index].val, sizeof(*dir));	/* [한국어] 방금 채운 디렉터리 항목을 메모리로 밀어낸다. 하드웨어가 이 항목을 읽어야 새 테이블에 닿는다 */
	}

	return &entries[index];	/* [한국어] 테이블 안에서 하위 6비트로 색인한 항목의 주소 */
}

/*
 * Interfaces for PASID table entry manipulation:
 */
static void
intel_pasid_clear_entry(struct device *dev, u32 pasid, bool fault_ignore)
{
	struct pasid_entry *pe;	/* [한국어] 비울 항목 */

	pe = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목을 찾는다 */
	if (WARN_ON(!pe))	/* [한국어] 없으면 호출자가 범위를 어긴 것이다 */
		return;	/* [한국어] 항목이 없다 — 호출자가 범위를 어긴 것이다 */

	if (fault_ignore && pasid_pte_is_present(pe))	/* [한국어] 폴트를 무시하라고 했고 아직 유효한 항목이면 */
		pasid_clear_entry_with_fpd(pe);	/* [한국어] 접근은 막되 폴트 보고만 끈 상태로 만든다. 내리는 순간에도 장치가 그 PASID 로 DMA 를 낼 수 있어, 그것을 전부 보고하면 로그가 뒤덮인다 */
	else
		pasid_clear_entry(pe);	/* [한국어] 아니면 통째로 비운다 */
}

static void
pasid_cache_invalidation_with_pasid(struct intel_iommu *iommu,
				    u16 did, u32 pasid)
{
	struct qi_desc desc;	/* [한국어] 보낼 무효화 서술자 */

	desc.qw0 = QI_PC_DID(did) | QI_PC_GRAN(QI_PC_PASID_SEL) |	/* [한국어] 도메인 id 와 "이 PASID 만" 범위를 */
		QI_PC_PASID(pasid) | QI_PC_TYPE;	/* [한국어] 대상 PASID·명령 종류와 함께 담는다 */
	desc.qw1 = 0;	/* [한국어] PASID 캐시 무효화는 주소를 쓰지 않는다 */
	desc.qw2 = 0;	/* [한국어] 예약 워드를 비운다 — 값이 남아 있으면 서술자가 거부된다 */
	desc.qw3 = 0;	/* [한국어] 같음 */

	qi_submit_sync(iommu, &desc, 1, 0);	/* [한국어] 하나만 보내고 완료를 기다린다. 배치를 쓰지 않는 것은 이 무효화가 항목 해제 경로에서 한 번씩만 일어나기 때문이다 */
}

static void
devtlb_invalidation_with_pasid(struct intel_iommu *iommu,
			       struct device *dev, u32 pasid)
{
	struct device_domain_info *info;	/* [한국어] 장치 정보 */
	u16 sid, qdep, pfsid;	/* [한국어] 소스 id, ATS 큐 깊이, PF 소스 id */

	info = dev_iommu_priv_get(dev);	/* [한국어] 장치에 매달린 정보 */
	if (!info || !info->ats_enabled)	/* [한국어] ATS 가 꺼져 있으면 */
		return;	/* [한국어] 장치 안에 캐시가 없으므로 비울 것도 없다 */

	if (!pci_device_is_present(to_pci_dev(dev)))	/* [한국어] 장치가 이미 뽑혔으면 */
		return;	/* [한국어] 응답하지 않을 무효화를 보내면 시간 초과만 난다 */

	sid = PCI_DEVID(info->bus, info->devfn);	/* [한국어] 16비트 소스 id */
	qdep = info->ats_qdep;	/* [한국어] 장치가 한 번에 받을 수 있는 요청 수 */
	pfsid = info->pfsid;	/* [한국어] SR-IOV VF 라면 PF 의 소스 id */

	/*
	 * When PASID 0 is used, it indicates RID2PASID(DMA request w/o PASID),
	 * devTLB flush w/o PASID should be used. For non-zero PASID under
	 * SVA usage, device could do DMA with multiple PASIDs. It is more
	 * efficient to flush devTLB specific to the PASID.
	 */
	if (pasid == IOMMU_NO_PASID)	/* [한국어] PASID 0 은 RID2PASID — PASID 없는 DMA 를 뜻한다 (위 영어 주석) */
		qi_flush_dev_iotlb(iommu, sid, pfsid, qdep, 0, 64 - VTD_PAGE_SHIFT);	/* [한국어] PASID 없는 형식으로 그 장치의 캐시를 통째로 비운다. 마스크가 64-12 라 주소 공간 전체다 */
	else
		qi_flush_dev_iotlb_pasid(iommu, sid, pfsid, pasid, qdep, 0, 64 - VTD_PAGE_SHIFT);	/* [한국어] PASID 를 지정해 그 주소 공간의 캐시만 비운다. SVA 에서는 장치가 여러 PASID 로 DMA 를 내므로, 이쪽이 훨씬 효율적이다 (위 영어 주석) */
}

/*
 * [한국어]
 * intel_pasid_tear_down_entry - PASID 항목을 내리고 관련 캐시를 모두 비운다
 *
 * @iommu: 담당 유닛. @dev: 장치. @pasid: 내릴 PASID.
 * @fault_ignore: true 면 내린 뒤에도 폴트를 보고하지 않는 상태로 둔다.
 * @return: 없음.
 *
 * 이 파일에서 가장 순서가 중요한 함수다. 항목을 지우는 것만으로는 끝이 아니고,
 * 하드웨어가 이미 캐시한 것을 정해진 순서로 비워야 한다.
 *
 * 정상 경로의 순서:
 *   1) present 를 지운다(락 안에서). 이 순간부터 하드웨어는 새 번역을 위해
 *      이 항목을 쓰지 않는다.
 *   2) 락을 놓는다. 아래 무효화는 하드웨어 완료를 기다리므로 락을 쥔 채
 *      하면 다른 장치의 부착이 그동안 막힌다.
 *   3) PASID 캐시 → IOTLB → 디바이스 TLB 순으로 비운다. 이 순서는 스펙의
 *      권고이며, 상위 캐시를 먼저 비워야 하위를 비우는 동안 다시 채워지지
 *      않는다.
 *   4) IOTLB 무효화의 형식이 변환 종류에 따라 갈린다. 통과나 1단계면 PASID
 *      인식 형식(qi_flush_piotlb_all)을, 2단계나 중첩이면 도메인 단위
 *      무효화를 쓴다 — 2단계 캐시 항목은 PASID 로 태그되지 않기 때문이다.
 *   5) 항목을 완전히 비우고 마지막으로 남은 페이지 요청을 배수한다.
 *
 * present 가 이미 0 인 경우가 두 갈래다.
 *   - FPD 도 0 이면 애초에 세운 적이 없는 항목이다. WARN 으로 전체가 0 인지
 *     확인하고 그냥 돌아간다.
 *   - FPD 가 1 이면 SVA 에서 이미 한 번 내린 항목이다(위 영어 주석).
 *     이 상태에서도 남은 페이지 요청이 있을 수 있으므로, 항목을 마저 비우고
 *     PRQ 를 배수한 뒤 돌아간다.
 *
 * fault_ignore 의 의미: 참이면 마지막에 PRQ 배수를 건너뛰고 FPD 를 남긴다.
 * 장치가 사라지는 중이라 응답할 상대가 없는 경우에 쓴다.
 *
 * 실행 컨텍스트: PASID 분리. 프로세스 컨텍스트(무효화 완료를 기다린다).
 *
 * 호출 체인:
 *   device_block_translation()/blocking_domain_set_dev_pasid()/SVA 해제
 *     → [intel_pasid_tear_down_entry]
 *     → pasid_cache_invalidation_with_pasid() → qi_flush_piotlb_all()
 *     → devtlb_invalidation_with_pasid() → intel_iommu_drain_pasid_prq()
 */
void intel_pasid_tear_down_entry(struct intel_iommu *iommu, struct device *dev,
				 u32 pasid, bool fault_ignore)
{
	struct pasid_entry *pte;	/* [한국어] 내릴 항목 */
	u16 did, pgtt;	/* [한국어] 그 항목의 도메인 id 와 변환 종류. 항목을 지우기 전에 읽어 둬야 한다 */

	spin_lock(&iommu->lock);	/* [한국어] 항목 조작 구간 */
	pte = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목 */
	if (WARN_ON(!pte)) {	/* [한국어] 없으면 호출자가 범위를 어긴 것이다 */
		spin_unlock(&iommu->lock);	/* [한국어] 락을 놓고 */
		return;	/* [한국어] 아무것도 하지 않는다 */
	}

	if (!pasid_pte_is_present(pte)) {	/* [한국어] 이미 내려간 항목이면 */
		if (!pasid_pte_is_fault_disabled(pte)) {	/* [한국어] FPD 도 꺼져 있으면 애초에 세운 적이 없다 */
			WARN_ON(READ_ONCE(pte->val[0]) != 0);	/* [한국어] 정말 비어 있는지 확인한다 — 아니면 어딘가에서 절반만 세운 것이다 */
			spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
			return;	/* [한국어] 할 일 없음 */
		}

		/*
		 * When a PASID is used for SVA by a device, it's possible
		 * that the pasid entry is non-present with the Fault
		 * Processing Disabled bit set. Clear the pasid entry and
		 * drain the PRQ for the PASID before return.
		 */
		pasid_clear_entry(pte);	/* [한국어] FPD 만 남아 있던 항목을 마저 비운다 (위 영어 주석) */
		spin_unlock(&iommu->lock);	/* [한국어] 배수는 잠들 수 있으므로 락을 먼저 놓는다 */
		intel_iommu_drain_pasid_prq(dev, pasid);	/* [한국어] 남은 페이지 요청을 배수한다. 응답 없는 요청은 장치를 영원히 멈춰 세운다 */

		return;	/* [한국어] 이 경로는 여기서 끝 */
	}

	did = pasid_get_domain_id(pte);	/* [한국어] 무효화에 쓸 도메인 id 를 지우기 전에 읽는다 */
	pgtt = pasid_pte_get_pgtt(pte);	/* [한국어] 변환 종류도. 아래에서 무효화 형식을 고르는 근거가 된다 */
	pasid_clear_present(pte);	/* [한국어] present 를 지운다 — 이 순간부터 하드웨어는 새 번역에 이 항목을 쓰지 않는다 */
	spin_unlock(&iommu->lock);	/* [한국어] 아래 무효화는 하드웨어 완료를 기다리므로 락을 놓는다. 쥔 채로 하면 다른 장치의 부착이 그동안 막힌다 */

	if (!ecap_coherent(iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
		clflush_cache_range(pte, sizeof(*pte));	/* [한국어] 지운 것을 메모리로 밀어낸다 */

	pasid_cache_invalidation_with_pasid(iommu, did, pasid);	/* [한국어] 먼저 PASID 캐시를 비운다. 상위를 먼저 비워야 하위를 비우는 동안 다시 채워지지 않는다 */

	if (pgtt == PASID_ENTRY_PGTT_PT || pgtt == PASID_ENTRY_PGTT_FL_ONLY)	/* [한국어] 통과나 1단계 변환이었으면 */
		qi_flush_piotlb_all(iommu, did, pasid);	/* [한국어] PASID 인식 형식으로 그 PASID 의 IOTLB 를 비운다 */
	else
		iommu->flush.flush_iotlb(iommu, did, 0, 0, DMA_TLB_DSI_FLUSH);	/* [한국어] 2단계나 중첩이었으면 도메인 단위로 비운다 — 2단계 캐시 항목은 PASID 로 태그되지 않아 PASID 형식으로는 지워지지 않는다 */

	devtlb_invalidation_with_pasid(iommu, dev, pasid);	/* [한국어] 마지막으로 장치 안의 캐시 */
	intel_pasid_clear_entry(dev, pasid, fault_ignore);	/* [한국어] 이제 항목을 완전히 비운다(또는 FPD 만 남긴다) */
	if (!ecap_coherent(iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
		clflush_cache_range(pte, sizeof(*pte));	/* [한국어] 그것도 메모리로 밀어낸다 */

	if (!fault_ignore)	/* [한국어] 폴트를 무시하라는 요청이 아니면 */
		intel_iommu_drain_pasid_prq(dev, pasid);	/* [한국어] 남은 페이지 요청을 배수한다 */
}

/*
 * This function flushes cache for a newly setup pasid table entry.
 * Caller of it should not modify the in-use pasid table entries.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_flush_caches - 새로 세운 PASID 항목이 하드웨어에 보이게 한다
 *
 * @iommu: 대상 유닛. @pte: 방금 세운 항목. @pasid: 그 PASID. @did: 도메인 id.
 * @return: 없음.
 *
 * 새 항목을 만든 뒤에 부른다. 항목을 "지울" 때와 달리 대개 무효화가 필요
 * 없지만, 두 경우가 예외다.
 *
 *   [1] 비코히런트 유닛: 우리가 쓴 항목이 CPU 캐시에만 있어 하드웨어가 보지
 *       못한다. clflush 로 메모리에 밀어낸다.
 *   [2] 캐싱 모드(에뮬레이션된 IOMMU): "이 PASID 는 설정되지 않았다"까지
 *       캐시하므로, 그 캐시를 지워야 새 항목이 보인다. PASID 캐시와 IOTLB
 *       둘 다 비운다.
 * 그 밖의 하드웨어에서는 쓰기 버퍼만 비우면 된다 — rwbf 를 요구하는 유닛을
 * 위한 조치이며, 아닌 유닛에서는 사실상 아무 일도 하지 않는다.
 *
 * 디바이스 TLB 를 비우지 않는 것을 눈여겨볼 것: 장치는 자기가 요청한 적 없는
 * 번역을 캐시하지 않으므로, 새 항목이 생겼다고 장치 캐시를 건드릴 이유가 없다.
 *
 * 위 영어 주석의 전제: 이 함수는 "새로 세운" 항목에만 쓴다. 이미 쓰이고 있던
 * 항목을 고친 뒤라면 intel_pasid_flush_present 를 써야 한다 — 그쪽은 장치
 * 캐시까지 비운다.
 *
 * 실행 컨텍스트: 항목 설정 직후, iommu->lock 을 놓은 뒤.
 */
static void pasid_flush_caches(struct intel_iommu *iommu,
				struct pasid_entry *pte,
			       u32 pasid, u16 did)
{
	if (!ecap_coherent(iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
		clflush_cache_range(pte, sizeof(*pte));	/* [한국어] 방금 쓴 항목을 메모리로 밀어낸다 */

	if (cap_caching_mode(iommu->cap)) {	/* [한국어] 캐싱 모드(에뮬레이션된 IOMMU)면 */
		pasid_cache_invalidation_with_pasid(iommu, did, pasid);	/* [한국어] 캐시된 "이 PASID 는 설정되지 않았다"를 지운다 */
		qi_flush_piotlb_all(iommu, did, pasid);	/* [한국어] 같은 이유로 IOTLB 도 비운다 */
	} else {
		iommu_flush_write_buffer(iommu);	/* [한국어] 보통의 하드웨어에서는 쓰기 버퍼만 비우면 된다. 디바이스 TLB 는 건드리지 않는다 — 장치는 요청한 적 없는 번역을 캐시하지 않는다 */
	}
}

/*
 * This function is supposed to be used after caller updates the fields
 * except for the SSADE and P bit of a pasid table entry. It does the
 * below:
 * - Flush cacheline if needed
 * - Flush the caches per Table 28 ”Guidance to Software for Invalidations“
 *   of VT-d spec 5.0.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * intel_pasid_flush_present - 이미 쓰이고 있던 항목을 고친 뒤 캐시를 비운다
 *
 * @iommu: 유닛. @dev: 장치. @pasid: PASID. @did: 도메인 id. @pte: 고친 항목.
 * @return: 없음.
 *
 * pasid_flush_caches 와의 차이가 핵심이다. 저쪽은 "없던 항목이 생겼다"라
 * 하드웨어가 캐시했을 것이 없지만, 이쪽은 "쓰이던 항목이 바뀌었다"라
 * 하드웨어와 장치가 옛 내용을 캐시하고 있다. 그래서 세 캐시를 모두 비운다.
 *
 * 무효화 순서와 형식은 스펙 5.0 의 Table 28 이 정한 그대로다(위 영어 주석).
 *   1) 도메인 안에서 그 PASID 만 골라 PASID 캐시를 비운다.
 *   2) 그 PASID 의 IOTLB 를 비운다.
 *   3) 디바이스 TLB 를 비운다. RID_PASID(= PASID 없는 트래픽)면 그 장치의
 *      캐시를 통째로, 아니면 PASID 를 지정해 비운다.
 * devtlb_invalidation_with_pasid 가 3번의 두 형식을 알아서 고른다.
 *
 * 호출 전 조건(위 영어 주석): 호출자가 SSADE 와 P 비트를 제외한 필드만
 * 고쳤어야 한다. present 를 건드리는 변경은 항목의 소유권이 오가는 것이라
 * 훨씬 무거운 절차(tear_down → setup)가 필요하다.
 *
 * 실행 컨텍스트: 더티 추적 설정 변경 등. iommu->lock 밖에서.
 */
static void intel_pasid_flush_present(struct intel_iommu *iommu,
				      struct device *dev,
				      u32 pasid, u16 did,
				      struct pasid_entry *pte)
{
	if (!ecap_coherent(iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
		clflush_cache_range(pte, sizeof(*pte));	/* [한국어] 고친 항목을 메모리로 밀어낸다 */

	/*
	 * VT-d spec 5.0 table28 states guides for cache invalidation:
	 *
	 * - PASID-selective-within-Domain PASID-cache invalidation
	 * - PASID-selective PASID-based IOTLB invalidation
	 * - If (pasid is RID_PASID)
	 *    - Global Device-TLB invalidation to affected functions
	 *   Else
	 *    - PASID-based Device-TLB invalidation (with S=1 and
	 *      Addr[63:12]=0x7FFFFFFF_FFFFF) to affected functions
	 */
	pasid_cache_invalidation_with_pasid(iommu, did, pasid);	/* [한국어] 스펙 Table 28 의 1번 — 도메인 안에서 그 PASID 만 골라 비운다 */
	qi_flush_piotlb_all(iommu, did, pasid);	/* [한국어] 2번 — 그 PASID 의 IOTLB */

	devtlb_invalidation_with_pasid(iommu, dev, pasid);	/* [한국어] 3번 — 장치 안의 캐시. RID_PASID 인지에 따라 형식이 갈리며 그 판단은 이 함수가 한다 */
}

/*
 * Set up the scalable mode pasid table entry for first only
 * translation type.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_pte_config_first_level - 1단계 변환용 PASID 항목의 비트를 조립한다
 *
 * @iommu: 대상 유닛. @pte: 채울 항목. @fsptptr: 1단계 페이지 테이블의 물리 주소.
 * @did: 도메인 id. @flags: PASID_FLAG_* 조합.
 * @return: 없음.
 *
 * pasid.h 의 setter 들을 순서대로 불러 하나의 완결된 항목을 만든다. 이
 * 파일의 pasid_pte_config_* 넷이 모두 같은 형태이며, 변환 종류마다 채우는
 * 필드가 다르다.
 *
 * 순서에 두 가지 규칙이 있다.
 *   - pasid_clear_entry 로 먼저 비운다. setter 들이 마스크 없이 |= 만 하는
 *     경우가 있어, 이전 값이 남아 있으면 비트가 섞인다.
 *   - pasid_set_present 를 마지막에 부른다. 그 안의 dma_wmb 가 앞의 모든
 *     쓰기를 하드웨어에 먼저 보이게 만든다.
 *
 * 1단계 항목이 담는 것: 페이지 테이블 주소(flptr)와 레벨 수(flpm), 도메인 id,
 * 주소 폭, 그리고 두 가지 스누프 설정. SVA 에서는 flptr 에 프로세스의 CR3
 * 값이 그대로 들어간다 — 1단계 형식이 x86-64 CPU 페이지 테이블과 같기 때문이다.
 *
 * 실행 컨텍스트: iommu->lock 을 쥔 채(lockdep_assert_held 로 확인).
 */
static void pasid_pte_config_first_level(struct intel_iommu *iommu,
					 struct pasid_entry *pte,
					 phys_addr_t fsptptr, u16 did,
					 int flags)
{
	lockdep_assert_held(&iommu->lock);	/* [한국어] 항목 조작은 이 락 아래에서만 — setter 들이 원자적이지 않다 */

	pasid_clear_entry(pte);	/* [한국어] 먼저 통째로 비운다. setter 중에 마스크 없이 |= 만 하는 것이 있어 이전 값이 남으면 비트가 섞인다 */

	/* Setup the first level page table pointer: */
	pasid_set_flptr(pte, fsptptr);	/* [한국어] 1단계 페이지 테이블 주소 (위 영어 주석). SVA 에서는 프로세스의 CR3 값이 그대로 들어간다 */

	if (flags & PASID_FLAG_FL5LP)	/* [한국어] 5레벨을 쓰라고 했으면 */
		pasid_set_flpm(pte, 1);	/* [한국어] 레벨 수를 5 로. 기본은 4레벨이다 */

	if (flags & PASID_FLAG_PAGE_SNOOP)	/* [한국어] 페이지 워크 스누프를 요청했으면 */
		pasid_set_pgsnp(pte);	/* [한국어] 하드웨어가 테이블을 읽을 때도 CPU 캐시를 보게 한다 */

	pasid_set_domain_id(pte, did);	/* [한국어] IOTLB 태그가 될 도메인 id */
	pasid_set_address_width(pte, iommu->agaw);	/* [한국어] 이 유닛이 쓰는 주소 폭 */
	pasid_set_page_snoop(pte, flags & PASID_FLAG_PWSNP);	/* [한국어] 데이터 DMA 의 캐시 스누프 강제 여부 */

	/* Setup Present and PASID Granular Transfer Type: */
	pasid_set_translation_type(pte, PASID_ENTRY_PGTT_FL_ONLY);	/* [한국어] 1단계만 쓴다고 표시 (위 영어 주석) */
	pasid_set_present(pte);	/* [한국어] 마지막에 present 를 세운다. 그 안의 dma_wmb 가 위의 모든 쓰기를 먼저 보이게 만든다 */
}

/*
 * [한국어]
 * intel_pasid_setup_first_level - PASID 항목을 1단계 변환으로 세운다
 *
 * @iommu: 유닛. @dev: 장치. @fsptptr: 1단계 페이지 테이블의 물리 주소.
 * @pasid: 대상 PASID. @did: 도메인 id. @flags: PASID_FLAG_* 조합.
 * @return: 0 성공, -EINVAL(하드웨어 미지원), -ENODEV(항목 없음), -EBUSY(이미 쓰임).
 *
 * config 함수를 감싸 검증·락·캐시 무효화를 붙인 진입점이다. 이 파일의 네
 * setup_* 함수가 모두 같은 골격이다.
 *
 * 먼저 하드웨어가 할 수 있는 일인지 확인한다. 1단계 변환 지원(ecap_flts)과,
 * 5레벨을 요청했다면 그 지원(cap_fl5lp_support)이다. 여기서 걸러 내지 않으면
 * 하드웨어가 이해하지 못하는 항목을 만들게 되고, 그 결과는 조용한 오동작이다.
 *
 * -EBUSY 의 의미: 이미 present 인 항목을 덮어쓰지 않는다. 그 PASID 를 다른
 * 도메인이 쓰고 있다는 뜻이고, 덮어쓰면 그쪽의 매핑이 소리 없이 사라진다.
 * 교체가 필요하면 호출자가 먼저 tear_down 해야 한다.
 *
 * 락을 놓은 뒤에 캐시를 비우는 순서를 눈여겨볼 것 — 무효화는 하드웨어 완료를
 * 기다리므로 락을 쥔 채 하면 다른 장치의 부착이 그동안 막힌다.
 *
 * 실행 컨텍스트: 도메인 부착, SVA 설정. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   __domain_setup_first_level()/intel_svm_set_dev_pasid()
 *     → [intel_pasid_setup_first_level]
 *     → pasid_pte_config_first_level() → pasid_flush_caches()
 */
int intel_pasid_setup_first_level(struct intel_iommu *iommu, struct device *dev,
				  phys_addr_t fsptptr, u32 pasid, u16 did,
				  int flags)
{
	struct pasid_entry *pte;	/* [한국어] 세울 항목 */

	if (!ecap_flts(iommu->ecap)) {	/* [한국어] 유닛이 1단계 변환을 못 하면 */
		pr_err("No first level translation support on %s\n",	/* [한국어] 이유를 남기고 */
		       iommu->name);	/* [한국어] 어느 유닛인지 */
		return -EINVAL;	/* [한국어] 거절한다. 여기서 걸러 내지 않으면 하드웨어가 이해 못 하는 항목이 만들어진다 */
	}

	if ((flags & PASID_FLAG_FL5LP) && !cap_fl5lp_support(iommu->cap)) {	/* [한국어] 5레벨을 요청했는데 지원하지 않으면 */
		pr_err("No 5-level paging support for first-level on %s\n",	/* [한국어] 이유를 남기고 */
		       iommu->name);	/* [한국어] 어느 유닛인지 */
		return -EINVAL;	/* [한국어] 거절 */
	}

	spin_lock(&iommu->lock);	/* [한국어] 항목 조작 구간 */
	pte = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목(없으면 테이블을 만든다) */
	if (!pte) {	/* [한국어] 만들 수 없으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -ENODEV;	/* [한국어] 설정 실패 */
	}

	if (pasid_pte_is_present(pte)) {	/* [한국어] 이미 쓰이고 있는 항목이면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -EBUSY;	/* [한국어] 덮어쓰지 않는다. 덮어쓰면 그 PASID 를 쓰던 쪽의 매핑이 소리 없이 사라진다 */
	}

	pasid_pte_config_first_level(iommu, pte, fsptptr, did, flags);	/* [한국어] 비트를 조립해 항목을 세운다 */

	spin_unlock(&iommu->lock);	/* [한국어] 무효화는 하드웨어 완료를 기다리므로 락을 먼저 놓는다 */

	pasid_flush_caches(iommu, pte, pasid, did);	/* [한국어] 새 항목이 하드웨어에 보이게 한다 */

	return 0;	/* [한국어] 1단계 변환이 설정되었다 */
}

/*
 * Set up the scalable mode pasid entry for second only translation type.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_pte_config_second_level - 2단계 변환용 PASID 항목의 비트를 조립한다
 *
 * @iommu: 유닛. @pte: 채울 항목. @domain: 2단계 페이지 테이블을 가진 도메인.
 * @did: 도메인 id.
 * @return: 없음.
 *
 * 1단계 판과 골격은 같지만 담는 것이 다르다. 페이지 테이블 주소를 인자로
 * 받지 않고 도메인의 sspt 에서 직접 꺼낸다 — 2단계 테이블은 도메인이 소유하고
 * 있어 별도로 전달할 이유가 없다.
 *
 * pt_iommu_vtdss_hw_info() 가 공용 페이지 테이블 라이브러리에서 하드웨어에
 * 넘길 두 값(테이블의 물리 주소, 주소 폭)을 꺼내 준다. 그 두 값이 실제
 * 테이블의 모양과 어긋나면 하드웨어가 엉뚱한 메모리를 테이블로 읽는다.
 *
 * 1단계에 없는 두 설정이 붙는다.
 *   - pasid_set_fault_enable: 폴트 보고를 켠다.
 *   - dirty_tracking 이면 SSADE: 2단계 페이지 테이블에 접근/더티 비트를
 *     기록하게 한다. 더티 추적은 2단계 전용 기능이라 1단계 판에는 없다.
 *
 * 스누프 설정이 반대 방향인 것을 눈여겨볼 것: 도메인이 DMA_INCOHERENT 로
 * 만들어졌으면 스누프를 켜지 않고, 아니면 켠다. 즉 "비코히런트가 아니면
 * 스누프한다"는 부정의 부정이다.
 *
 * 실행 컨텍스트: iommu->lock 을 쥔 채.
 */
static void pasid_pte_config_second_level(struct intel_iommu *iommu,
					  struct pasid_entry *pte,
					  struct dmar_domain *domain, u16 did)
{
	struct pt_iommu_vtdss_hw_info pt_info;	/* [한국어] 공용 페이지 테이블 라이브러리에서 꺼낼 하드웨어용 정보 */

	lockdep_assert_held(&iommu->lock);	/* [한국어] 항목 조작은 이 락 아래에서만 */

	pt_iommu_vtdss_hw_info(&domain->sspt, &pt_info);	/* [한국어] 테이블의 물리 주소와 주소 폭을 꺼낸다 */
	pasid_clear_entry(pte);	/* [한국어] 먼저 통째로 비운다 */
	pasid_set_domain_id(pte, did);	/* [한국어] IOTLB 태그가 될 도메인 id */
	pasid_set_slptr(pte, pt_info.ssptptr);	/* [한국어] 2단계 페이지 테이블의 물리 주소 */
	pasid_set_address_width(pte, pt_info.aw);	/* [한국어] 그 테이블의 주소 폭. 실제 깊이와 어긋나면 하드웨어가 엉뚱한 메모리를 테이블로 읽는다 */
	pasid_set_translation_type(pte, PASID_ENTRY_PGTT_SL_ONLY);	/* [한국어] 2단계만 쓴다고 표시 */
	pasid_set_fault_enable(pte);	/* [한국어] 폴트 보고를 켠다 */
	pasid_set_page_snoop(pte, !(domain->sspt.vtdss_pt.common.features &	/* [한국어] 도메인이 비코히런트로 만들어지지 않았으면 */
				    BIT(PT_FEAT_DMA_INCOHERENT)));	/* [한국어] 스누프를 켠다 — 부정의 부정이라 읽기 까다로운 자리다 */
	if (domain->dirty_tracking)	/* [한국어] 도메인이 더티 추적 중이면 */
		pasid_set_ssade(pte);	/* [한국어] 이 항목에도 반영한다. 더티 추적은 2단계 전용이라 1단계 판에는 없는 설정이다 */

	pasid_set_present(pte);	/* [한국어] 마지막에 present. 그 안의 dma_wmb 가 위의 모든 쓰기를 먼저 보이게 한다 */
}

/*
 * [한국어]
 * intel_pasid_setup_second_level - PASID 항목을 2단계 변환으로 세운다
 *
 * @iommu: 유닛. @domain: 2단계 도메인. @dev: 장치. @pasid: 대상 PASID.
 * @return: 0 성공, -EINVAL(하드웨어 미지원), -ENODEV, -EBUSY.
 *
 * setup_first_level 과 같은 골격이다: 하드웨어 능력 확인 → 락 → 항목 확보 →
 * 중복 확인 → 조립 → 락 해제 → 캐시 무효화.
 *
 * 다른 점은 도메인 id 를 인자로 받지 않고 domain_id_iommu() 로 직접 구한다는
 * 것이다. 2단계 도메인은 이 유닛에서 자기 id 를 갖고 있기 때문이다(1단계는
 * SVA 처럼 id 를 갖지 않는 경우가 있어 호출자가 정해 넘긴다).
 *
 * 실행 컨텍스트: 도메인 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   domain_setup_second_level() → [intel_pasid_setup_second_level]
 *     → pasid_pte_config_second_level() → pasid_flush_caches()
 */
int intel_pasid_setup_second_level(struct intel_iommu *iommu,
				   struct dmar_domain *domain,
				   struct device *dev, u32 pasid)
{
	struct pasid_entry *pte;	/* [한국어] 세울 항목 */
	u16 did;	/* [한국어] 이 유닛에서의 도메인 id */


	/*
	 * If hardware advertises no support for second level
	 * translation, return directly.
	 */
	if (!ecap_slts(iommu->ecap)) {	/* [한국어] 유닛이 2단계 변환을 못 하면 (위 영어 주석) */
		pr_err("No second level translation support on %s\n",	/* [한국어] 이유를 남기고 */
		       iommu->name);	/* [한국어] 어느 유닛인지 */
		return -EINVAL;	/* [한국어] 거절 */
	}

	did = domain_id_iommu(domain, iommu);	/* [한국어] 2단계 도메인은 이 유닛에서 자기 id 를 갖고 있어 직접 구한다 */

	spin_lock(&iommu->lock);	/* [한국어] 항목 조작 구간 */
	pte = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목 */
	if (!pte) {	/* [한국어] 만들 수 없으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -ENODEV;	/* [한국어] 설정 실패 */
	}

	if (pasid_pte_is_present(pte)) {	/* [한국어] 이미 쓰이고 있으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -EBUSY;	/* [한국어] 덮어쓰지 않는다 */
	}

	pasid_pte_config_second_level(iommu, pte, domain, did);	/* [한국어] 비트를 조립한다 */
	spin_unlock(&iommu->lock);	/* [한국어] 무효화 전에 락을 놓는다 */

	pasid_flush_caches(iommu, pte, pasid, did);	/* [한국어] 새 항목이 하드웨어에 보이게 한다 */

	return 0;	/* [한국어] 2단계 변환이 설정되었다 */
}

/*
 * Set up dirty tracking on a second only or nested translation type.
 */
/*
 * [한국어]
 * intel_pasid_setup_dirty_tracking - 이미 세워진 항목의 더티 추적만 켜고 끈다
 *
 * @iommu: 유닛. @dev: 장치. @pasid: 대상 PASID. @enabled: 켤지 끌지.
 * @return: 0 성공(이미 원하는 상태면 아무것도 안 하고 0),
 *          -ENODEV(항목 없음), -EOPNOTSUPP(그 변환 종류로는 불가능).
 *
 * 앞의 setup_* 들과 성격이 다르다. 항목을 새로 세우는 것이 아니라 이미
 * 쓰이고 있는 항목의 비트 하나(SSADE)만 바꾼다. 그래서 present 를 건드리지
 * 않고, 캐시 무효화도 "고친 항목"에 맞는 무거운 절차를 쓴다.
 *
 * 변환 종류를 확인하는 이유: 더티 추적은 2단계 페이지 테이블에 비트를
 * 기록하는 기능이라, 2단계를 쓰지 않는 항목(1단계 전용, 통과)에서는 켤 수
 * 없다. 그래서 PGTT 가 SL_ONLY 나 NESTED 일 때만 허용한다.
 *
 * 이미 원하는 상태면 곧바로 0 을 돌려주는 것이 중요하다 — 무효화는 값비싼
 * 왕복이고, 도메인 단위 설정이 장치마다 반복해서 불리기 때문이다.
 *
 * 무효화 순서는 스펙 Table 25 가 정한 그대로다(코드 안 영어 주석).
 *   1) 도메인 안에서 그 PASID 만 골라 PASID 캐시.
 *   2) IOTLB — PGTT 가 SL 이나 Nested 이므로 도메인 단위 무효화를 쓴다.
 *      (1단계였다면 PASID 형식이었겠지만 위에서 이미 걸러졌다.)
 *   3) 디바이스 TLB.
 *
 * 실행 컨텍스트: 마이그레이션 준비. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   domain_set_dirty_tracking() → [intel_pasid_setup_dirty_tracking]
 *     → pasid_set_ssade()/pasid_clear_ssade()
 *     → pasid_cache_invalidation_with_pasid() → devtlb_invalidation_with_pasid()
 */
int intel_pasid_setup_dirty_tracking(struct intel_iommu *iommu,
				     struct device *dev, u32 pasid,
				     bool enabled)
{
	struct pasid_entry *pte;	/* [한국어] 고칠 항목 */
	u16 did, pgtt;	/* [한국어] 도메인 id 와 변환 종류 */

	spin_lock(&iommu->lock);	/* [한국어] 항목 조작 구간 */

	pte = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목 */
	if (!pte) {	/* [한국어] 없으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		dev_err_ratelimited(
			dev, "Failed to get pasid entry of PASID %d\n", pasid);	/* [한국어] 어느 PASID 인지 남긴다 */
		return -ENODEV;	/* [한국어] 설정 불가 */
	}

	did = pasid_get_domain_id(pte);	/* [한국어] 무효화에 쓸 도메인 id */
	pgtt = pasid_pte_get_pgtt(pte);	/* [한국어] 변환 종류. 더티 추적이 가능한지의 근거다 */
	if (pgtt != PASID_ENTRY_PGTT_SL_ONLY &&	/* [한국어] 2단계 전용이 아니고 */
	    pgtt != PASID_ENTRY_PGTT_NESTED) {	/* [한국어] 중첩도 아니면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		dev_err_ratelimited(
			dev,
			"Dirty tracking not supported on translation type %d\n",	/* [한국어] 더티 비트는 2단계 페이지 테이블에 기록되므로 그것을 쓰지 않는 항목에서는 켤 수 없다 */
			pgtt);	/* [한국어] 문제의 변환 종류 */
		return -EOPNOTSUPP;	/* [한국어] 거절 */
	}

	if (pasid_get_ssade(pte) == enabled) {	/* [한국어] 이미 원하는 상태면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락을 놓고 */
		return 0;	/* [한국어] 무효화 없이 돌아간다. 도메인 단위 설정이 장치마다 반복 호출되므로 이 조기 반환이 실제로 자주 걸린다 */
	}

	if (enabled)	/* [한국어] 켜라고 했으면 */
		pasid_set_ssade(pte);	/* [한국어] SSADE 를 세우고 */
	else
		pasid_clear_ssade(pte);	/* [한국어] 아니면 지운다 */
	spin_unlock(&iommu->lock);	/* [한국어] 무효화 전에 락을 놓는다 */

	if (!ecap_coherent(iommu->ecap))	/* [한국어] 비코히런트 유닛이면 */
		clflush_cache_range(pte, sizeof(*pte));	/* [한국어] 고친 항목을 메모리로 밀어낸다 */

	/*
	 * From VT-d spec table 25 "Guidance to Software for Invalidations":
	 *
	 * - PASID-selective-within-Domain PASID-cache invalidation
	 *   If (PGTT=SS or Nested)
	 *    - Domain-selective IOTLB invalidation
	 *   Else
	 *    - PASID-selective PASID-based IOTLB invalidation
	 * - If (pasid is RID_PASID)
	 *    - Global Device-TLB invalidation to affected functions
	 *   Else
	 *    - PASID-based Device-TLB invalidation (with S=1 and
	 *      Addr[63:12]=0x7FFFFFFF_FFFFF) to affected functions
	 */
	pasid_cache_invalidation_with_pasid(iommu, did, pasid);	/* [한국어] 스펙 Table 25 의 1번 (위 영어 주석) */

	iommu->flush.flush_iotlb(iommu, did, 0, 0, DMA_TLB_DSI_FLUSH);	/* [한국어] 2번 — PGTT 가 SL 이나 Nested 이므로 도메인 단위 IOTLB 무효화를 쓴다 */

	devtlb_invalidation_with_pasid(iommu, dev, pasid);	/* [한국어] 3번 — 장치 안의 캐시 */

	return 0;	/* [한국어] 추적 설정이 반영되었다 */
}

/*
 * Set up the scalable mode pasid entry for passthrough translation type.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_pte_config_pass_through - 통과 변환용 PASID 항목의 비트를 조립한다
 *
 * @iommu: 유닛. @pte: 채울 항목. @did: 도메인 id.
 * @return: 없음.
 *
 * 통과 모드는 번역을 하지 않으므로 페이지 테이블 주소가 없다. 그래서 이
 * config 함수만 다른 셋과 달리 주소 인자를 받지 않는다.
 *
 * 그래도 채워야 하는 것들:
 *   - 도메인 id: 번역은 안 하지만 캐시 무효화의 대상 지정에는 쓰인다.
 *   - 주소 폭: 하드웨어가 지원하는 값을 넣어야 한다. 통과 모드에서 이 필드가
 *     무시되지 않고 "다룰 수 있는 주소 범위"를 정하기 때문이다.
 *   - fault enable: 통과 모드라도 그 범위를 벗어난 접근은 알아야 한다.
 *   - page snoop: 유닛이 scalable 모드 워크 코히런시를 지원하면 켠다.
 *
 * 이것이 레거시 모드의 CONTEXT_TT_PASS_THROUGH 에 대응하는 scalable 모드
 * 구현이며, 항등 도메인이 이 경로로 세워진다.
 *
 * 실행 컨텍스트: iommu->lock 을 쥔 채.
 */
static void pasid_pte_config_pass_through(struct intel_iommu *iommu,
					  struct pasid_entry *pte, u16 did)
{
	lockdep_assert_held(&iommu->lock);	/* [한국어] 항목 조작은 이 락 아래에서만 */

	pasid_clear_entry(pte);	/* [한국어] 먼저 통째로 비운다 */
	pasid_set_domain_id(pte, did);	/* [한국어] 번역은 안 하지만 캐시 무효화의 대상 지정에 쓰인다 */
	pasid_set_address_width(pte, iommu->agaw);	/* [한국어] 통과 모드에서도 다룰 수 있는 주소 범위를 정한다 */
	pasid_set_translation_type(pte, PASID_ENTRY_PGTT_PT);	/* [한국어] 통과라고 표시 — 이 값 때문에 페이지 테이블 주소 필드가 무시된다 */
	pasid_set_fault_enable(pte);	/* [한국어] 범위를 벗어난 접근은 통과 모드에서도 알아야 한다 */
	pasid_set_page_snoop(pte, !!ecap_smpwc(iommu->ecap));	/* [한국어] 유닛이 워크 코히런시를 지원하면 스누프를 켠다 */
	pasid_set_present(pte);	/* [한국어] 마지막에 present */
}

/*
 * [한국어]
 * intel_pasid_setup_pass_through - PASID 항목을 통과 모드로 세운다
 *
 * @iommu: 유닛. @dev: 장치. @pasid: 대상 PASID.
 * @return: 0 성공, -ENODEV, -EBUSY.
 *
 * 항등 도메인의 scalable 모드 구현이다. 다른 setup_* 과 골격이 같지만 두
 * 가지가 다르다.
 *   - 하드웨어 능력 검사가 없다. 통과 모드는 scalable 모드를 지원하는 모든
 *     유닛이 할 수 있다.
 *   - 도메인 id 를 인자로 받지 않고 FLPT_DEFAULT_DID 를 쓴다. 통과 모드는
 *     주소 공간을 갖지 않아 도메인 id 를 할당받지 않으며, 스펙이 1단계·통과
 *     항목에 2단계와 다른 id 를 요구하기 때문에 그 예약값을 쓴다.
 *
 * 실행 컨텍스트: 항등 도메인 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   identity_domain_attach_dev()/domain_setup_passthrough()
 *     → [intel_pasid_setup_pass_through]
 *     → pasid_pte_config_pass_through() → pasid_flush_caches()
 */
int intel_pasid_setup_pass_through(struct intel_iommu *iommu,
				   struct device *dev, u32 pasid)
{
	u16 did = FLPT_DEFAULT_DID;	/* [한국어] 통과 모드는 주소 공간이 없어 도메인 id 를 할당받지 않는다. 스펙이 1단계·통과에 2단계와 다른 id 를 요구해 예약값을 쓴다 */
	struct pasid_entry *pte;	/* [한국어] 세울 항목 */

	spin_lock(&iommu->lock);	/* [한국어] 항목 조작 구간 */
	pte = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목 */
	if (!pte) {	/* [한국어] 만들 수 없으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -ENODEV;	/* [한국어] 설정 실패 */
	}

	if (pasid_pte_is_present(pte)) {	/* [한국어] 이미 쓰이고 있으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -EBUSY;	/* [한국어] 덮어쓰지 않는다 */
	}

	pasid_pte_config_pass_through(iommu, pte, did);	/* [한국어] 비트를 조립한다 */
	spin_unlock(&iommu->lock);	/* [한국어] 무효화 전에 락을 놓는다 */

	pasid_flush_caches(iommu, pte, pasid, did);	/* [한국어] 새 항목이 하드웨어에 보이게 한다 */

	return 0;	/* [한국어] 이 PASID 는 이제 번역 없이 통과한다 */
}

/*
 * Set the page snoop control for a pasid entry which has been set up.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * intel_pasid_setup_page_snoop_control - 이미 세워진 항목에 캐시 스누프 강제를 켠다
 *
 * @iommu: 유닛. @dev: 장치. @pasid: 대상 PASID.
 * @return: 없음.
 *
 * dirty_tracking 판과 성격이 같다 — 쓰이고 있는 항목의 비트 하나만 바꾼다.
 * 그래서 present 를 건드리지 않고, intel_pasid_flush_present 로 세 캐시를
 * 모두 비운다.
 *
 * 언제 불리는가: VFIO/KVM 이 도메인에 강제 코히런시를 요구할 때
 * (intel_iommu_enforce_cache_coherency_fs). 1단계 페이지 테이블은 PTE 안에
 * 스누프 제어 비트를 둘 자리가 없어, 이미 붙어 있는 장치의 PASID 항목을
 * 하나씩 고쳐야 한다. 2단계는 PTE 마다 SNP 비트가 있어 이 함수가 필요 없다.
 *
 * present 가 아니면 WARN 을 남긴다 — 아직 세우지 않은 항목에 정책만 얹는 것은
 * 호출자의 순서 오류이고, 나중에 그 항목을 세울 때 이 설정이 덮여 사라진다.
 *
 * 실행 컨텍스트: 강제 코히런시 설정. 도메인 락을 쥔 채 불린다.
 *
 * 호출 체인:
 *   intel_iommu_enforce_cache_coherency_fs()
 *     → [intel_pasid_setup_page_snoop_control] → intel_pasid_flush_present()
 */
void intel_pasid_setup_page_snoop_control(struct intel_iommu *iommu,
					  struct device *dev, u32 pasid)
{
	struct pasid_entry *pte;	/* [한국어] 고칠 항목 */
	u16 did;	/* [한국어] 무효화에 쓸 도메인 id */

	spin_lock(&iommu->lock);	/* [한국어] 항목 조작 구간 */
	pte = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목 */
	if (WARN_ON(!pte || !pasid_pte_is_present(pte))) {	/* [한국어] 아직 세우지 않은 항목에 정책만 얹는 것은 호출 순서 오류다 — 나중에 항목을 세울 때 이 설정이 덮여 사라진다 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return;	/* [한국어] 아무것도 하지 않는다 */
	}

	pasid_set_pgsnp(pte);	/* [한국어] 페이지 워크도 CPU 캐시를 스누프하게 한다 */
	did = pasid_get_domain_id(pte);	/* [한국어] 무효화에 쓸 도메인 id 를 락 안에서 읽어 둔다 */
	spin_unlock(&iommu->lock);	/* [한국어] 무효화 전에 락을 놓는다 */

	intel_pasid_flush_present(iommu, dev, pasid, did, pte);	/* [한국어] 쓰이고 있던 항목을 고쳤으므로 세 캐시를 모두 비운다 */
}

/*
 * [한국어]
 * pasid_pte_config_nestd - 중첩 변환용 PASID 항목의 비트를 조립한다
 *
 * @iommu: 유닛. @pte: 채울 항목. @s1_cfg: 게스트가 준 1단계 설정.
 * @s2_domain: 부모(호스트) 2단계 도메인. @did: 도메인 id.
 * @return: 없음.
 *
 * 네 config 함수 중 가장 많은 것을 담는다. 중첩에서는 1단계와 2단계 주소가
 * 모두 유효해야 하기 때문이다.
 *   게스트 DMA → (게스트의 1단계 테이블) → 게스트 물리 주소
 *              → (호스트의 2단계 테이블) → 호스트 물리 주소
 * 하드웨어가 이 두 단계를 스스로 밟으므로, 항목 하나에 두 테이블의 주소가
 * 함께 들어간다.
 *
 * 신뢰 경계가 여기서 드러난다. s1_cfg 는 유저스페이스(VMM)가 준 값이라
 * 신뢰할 수 없다. 그런데 호스트는 그 테이블을 파싱하지 않고 주소만 하드웨어에
 * 넘긴다 — 안전한 이유는 그 테이블이 가리키는 모든 주소가 2단계 매핑을 한 번
 * 더 거치기 때문이다. 게스트가 아무 주소나 적어도 2단계가 허용한 범위를
 * 벗어날 수 없다.
 *
 * 게스트가 요청할 수 있는 것들(s1_cfg->flags):
 *   SRE — 특권 요청 허용. WPE 는 SRE 안에서만 의미가 있어 중첩 검사를 한다.
 *   EAFE — 확장 접근 플래그.
 * 이 플래그들은 setup_nested 가 하드웨어 지원 여부를 먼저 확인한 뒤에만
 * 여기 도달한다.
 *
 * 2단계 쪽 설정(스누프, 더티 추적)은 부모 도메인의 상태를 그대로 따른다 —
 * 게스트가 정할 수 있는 것이 아니다.
 *
 * 실행 컨텍스트: iommu->lock 을 쥔 채.
 */
static void pasid_pte_config_nestd(struct intel_iommu *iommu,
				   struct pasid_entry *pte,
				   struct iommu_hwpt_vtd_s1 *s1_cfg,
				   struct dmar_domain *s2_domain,
				   u16 did)
{
	struct pt_iommu_vtdss_hw_info pt_info;	/* [한국어] 부모 2단계 테이블의 하드웨어용 정보 */

	lockdep_assert_held(&iommu->lock);	/* [한국어] 항목 조작은 이 락 아래에서만 */

	pt_iommu_vtdss_hw_info(&s2_domain->sspt, &pt_info);	/* [한국어] 부모 테이블의 물리 주소와 주소 폭을 꺼낸다 */

	pasid_clear_entry(pte);	/* [한국어] 먼저 통째로 비운다 */

	if (s1_cfg->addr_width == ADDR_WIDTH_5LEVEL)	/* [한국어] 게스트가 5레벨 테이블을 쓴다고 했으면 */
		pasid_set_flpm(pte, 1);	/* [한국어] 1단계 레벨 수를 5 로 */

	pasid_set_flptr(pte, s1_cfg->pgtbl_addr);	/* [한국어] 게스트의 1단계 페이지 테이블 주소. 유저스페이스가 준 값이지만, 그것이 가리키는 모든 주소가 2단계를 한 번 더 거치므로 안전하다 */

	if (s1_cfg->flags & IOMMU_VTD_S1_SRE) {	/* [한국어] 게스트가 특권 요청을 요청했으면 */
		pasid_set_sre(pte);	/* [한국어] 허용한다 */
		if (s1_cfg->flags & IOMMU_VTD_S1_WPE)	/* [한국어] 그 안에서 쓰기 보호도 요청했으면 */
			pasid_set_wpe(pte);	/* [한국어] 켠다. WPE 는 SRE 안에서만 의미가 있어 중첩 검사를 한다 */
	}

	if (s1_cfg->flags & IOMMU_VTD_S1_EAFE)	/* [한국어] 확장 접근 플래그를 요청했으면 */
		pasid_set_eafe(pte);	/* [한국어] 켠다 */

	if (s2_domain->force_snooping)	/* [한국어] 부모가 강제 코히런시 중이면 */
		pasid_set_pgsnp(pte);	/* [한국어] 페이지 워크도 스누프한다. 게스트가 정할 수 있는 것이 아니라 호스트 정책이다 */

	pasid_set_slptr(pte, pt_info.ssptptr);	/* [한국어] 부모의 2단계 페이지 테이블 주소 */
	pasid_set_fault_enable(pte);	/* [한국어] 폴트 보고를 켠다 */
	pasid_set_domain_id(pte, did);	/* [한국어] IOTLB 태그가 될 도메인 id */
	pasid_set_address_width(pte, pt_info.aw);	/* [한국어] 2단계 테이블의 주소 폭 */
	pasid_set_page_snoop(pte, !(s2_domain->sspt.vtdss_pt.common.features &	/* [한국어] 부모가 비코히런트로 만들어지지 않았으면 */
				    BIT(PT_FEAT_DMA_INCOHERENT)));	/* [한국어] 스누프를 켠다 */
	if (s2_domain->dirty_tracking)	/* [한국어] 부모가 더티 추적 중이면 */
		pasid_set_ssade(pte);	/* [한국어] 이 항목에도 반영한다 */
	pasid_set_translation_type(pte, PASID_ENTRY_PGTT_NESTED);	/* [한국어] 중첩이라고 표시 — 이 값 때문에 1단계와 2단계 주소가 모두 유효해진다 */
	pasid_set_present(pte);	/* [한국어] 마지막에 present */
}

/**
 * intel_pasid_setup_nested() - Set up PASID entry for nested translation.
 * @iommu:      IOMMU which the device belong to
 * @dev:        Device to be set up for translation
 * @pasid:      PASID to be programmed in the device PASID table
 * @domain:     User stage-1 domain nested on a stage-2 domain
 *
 * This is used for nested translation. The input domain should be
 * nested type and nested on a parent with 'is_nested_parent' flag
 * set.
 */
int intel_pasid_setup_nested(struct intel_iommu *iommu, struct device *dev,
			     u32 pasid, struct dmar_domain *domain)
{
	struct iommu_hwpt_vtd_s1 *s1_cfg = &domain->s1_cfg;	/* [한국어] 게스트가 준 1단계 설정 */
	struct dmar_domain *s2_domain = domain->s2_domain;	/* [한국어] 이 도메인이 얹혀 있는 부모 */
	u16 did = domain_id_iommu(domain, iommu);	/* [한국어] 자식 도메인의 id 를 쓴다 — 하드웨어가 이 id 로 두 단계를 모두 식별한다 */
	struct pasid_entry *pte;	/* [한국어] 세울 항목 */

	/* Address width should match the address width supported by hardware */
	switch (s1_cfg->addr_width) {	/* [한국어] 게스트가 요청한 주소 폭이 하드웨어가 지원하는 것인지 (위 영어 주석) */
	case ADDR_WIDTH_4LEVEL:	/* [한국어] 4레벨은 */
		break;	/* [한국어] 항상 지원된다 */
	case ADDR_WIDTH_5LEVEL:	/* [한국어] 5레벨은 */
		if (!cap_fl5lp_support(iommu->cap)) {	/* [한국어] 하드웨어가 지원해야 한다 */
			dev_err_ratelimited(dev,
					    "5-level paging not supported\n");	/* [한국어] 지원하지 않으면 이유를 남기고 */
			return -EINVAL;	/* [한국어] 거절 */
		}
		break;	/* [한국어] 지원하면 통과 */
	default:	/* [한국어] 그 밖의 값은 */
		dev_err_ratelimited(dev, "Invalid stage-1 address width %d\n",	/* [한국어] 유저스페이스가 이상한 값을 준 것이다 */
				    s1_cfg->addr_width);	/* [한국어] 문제의 값 */
		return -EINVAL;	/* [한국어] 거절 */
	}

	if ((s1_cfg->flags & IOMMU_VTD_S1_SRE) && !ecap_srs(iommu->ecap)) {	/* [한국어] 특권 요청을 요청했는데 하드웨어가 못 하면 */
		pr_err_ratelimited("No supervisor request support on %s\n",	/* [한국어] 이유를 남기고 */
				   iommu->name);	/* [한국어] 어느 유닛인지 */
		return -EINVAL;	/* [한국어] 거절 */
	}

	if ((s1_cfg->flags & IOMMU_VTD_S1_EAFE) && !ecap_eafs(iommu->ecap)) {	/* [한국어] 확장 접근 플래그도 마찬가지 */
		pr_err_ratelimited("No extended access flag support on %s\n",	/* [한국어] 이유를 남기고 */
				   iommu->name);	/* [한국어] 어느 유닛인지 */
		return -EINVAL;	/* [한국어] 거절. 이 검사들이 없으면 하드웨어가 무시하는 설정을 게스트가 켜졌다고 믿게 된다 */
	}

	spin_lock(&iommu->lock);	/* [한국어] 항목 조작 구간 */
	pte = intel_pasid_get_entry(dev, pasid);	/* [한국어] 그 PASID 의 항목 */
	if (!pte) {	/* [한국어] 만들 수 없으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -ENODEV;	/* [한국어] 설정 실패 */
	}
	if (pasid_pte_is_present(pte)) {	/* [한국어] 이미 쓰이고 있으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -EBUSY;	/* [한국어] 덮어쓰지 않는다 */
	}

	pasid_pte_config_nestd(iommu, pte, s1_cfg, s2_domain, did);	/* [한국어] 두 단계의 주소와 정책을 담아 항목을 세운다 */
	spin_unlock(&iommu->lock);	/* [한국어] 무효화 전에 락을 놓는다 */

	pasid_flush_caches(iommu, pte, pasid, did);	/* [한국어] 새 항목이 하드웨어에 보이게 한다 */

	return 0;	/* [한국어] 중첩 변환이 설정되었다 */
}

/*
 * Interfaces to setup or teardown a pasid table to the scalable-mode
 * context table entry:
 */

static void device_pasid_table_teardown(struct device *dev, u8 bus, u8 devfn)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;
	struct context_entry *context;
	u16 did;

	spin_lock(&iommu->lock);
	context = iommu_context_addr(iommu, bus, devfn, false);
	if (!context) {
		spin_unlock(&iommu->lock);
		return;
	}

	did = context_domain_id(context);
	context_clear_entry(context);
	__iommu_flush_cache(iommu, context, sizeof(*context));
	spin_unlock(&iommu->lock);
	intel_context_flush_no_pasid(info, context, did);
}

static int pci_pasid_table_teardown(struct pci_dev *pdev, u16 alias, void *data)
{
	struct device *dev = data;

	if (dev == &pdev->dev)
		device_pasid_table_teardown(dev, PCI_BUS_NUM(alias), alias & 0xff);

	return 0;
}

void intel_pasid_teardown_sm_context(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	if (!dev_is_pci(dev)) {
		device_pasid_table_teardown(dev, info->bus, info->devfn);
		return;
	}

	pci_for_each_dma_alias(to_pci_dev(dev), pci_pasid_table_teardown, dev);
}

/*
 * Get the PASID directory size for scalable mode context entry.
 * Value of X in the PDTS field of a scalable mode context entry
 * indicates PASID directory with 2^(X + 7) entries.
 */
static unsigned long context_get_sm_pds(struct pasid_table *table)
{
	unsigned long pds, max_pde;

	max_pde = table->max_pasid >> PASID_PDE_SHIFT;
	pds = find_first_bit(&max_pde, MAX_NR_PASID_BITS);
	if (pds < 7)
		return 0;

	return pds - 7;
}

static int context_entry_set_pasid_table(struct context_entry *context,
					 struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 할당에 쓸 NUMA 노드를 얻으려고 */
	struct pasid_table *table = info->pasid_table;
	struct intel_iommu *iommu = info->iommu;
	unsigned long pds;

	context_clear_entry(context);

	pds = context_get_sm_pds(table);
	context->lo = (u64)virt_to_phys(table->table) | context_pdts(pds);
	context_set_sm_rid2pasid(context, IOMMU_NO_PASID);

	if (info->ats_supported)
		context_set_sm_dte(context);
	if (info->pasid_supported)
		context_set_pasid(context);
	if (info->pri_supported)
		context_set_sm_pre(context);

	context_set_fault_enable(context);
	context_set_present(context);
	__iommu_flush_cache(iommu, context, sizeof(*context));

	return 0;
}

static int device_pasid_table_setup(struct device *dev, u8 bus, u8 devfn)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	struct context_entry *context;

	spin_lock(&iommu->lock);
	context = iommu_context_addr(iommu, bus, devfn, true);
	if (!context) {
		spin_unlock(&iommu->lock);
		return -ENOMEM;
	}

	if (context_present(context) && !context_copied(iommu, bus, devfn)) {
		spin_unlock(&iommu->lock);
		return 0;
	}

	if (context_copied(iommu, bus, devfn)) {
		context_clear_present(context);
		__iommu_flush_cache(iommu, context, sizeof(*context));

		/*
		 * For kdump cases, old valid entries may be cached due to
		 * the in-flight DMA and copied pgtable, but there is no
		 * unmapping behaviour for them, thus we need explicit cache
		 * flushes for all affected domain IDs and PASIDs used in
		 * the copied PASID table. Given that we have no idea about
		 * which domain IDs and PASIDs were used in the copied tables,
		 * upgrade them to global PASID and IOTLB cache invalidation.
		 */
		iommu->flush.flush_context(iommu, 0,
					   PCI_DEVID(bus, devfn),
					   DMA_CCMD_MASK_NOBIT,
					   DMA_CCMD_DEVICE_INVL);
		qi_flush_pasid_cache(iommu, 0, QI_PC_GLOBAL, 0);
		iommu->flush.flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);
		devtlb_invalidation_with_pasid(iommu, dev, IOMMU_NO_PASID);

		context_clear_entry(context);
		__iommu_flush_cache(iommu, context, sizeof(*context));

		/*
		 * At this point, the device is supposed to finish reset at
		 * its driver probe stage, so no in-flight DMA will exist,
		 * and we don't need to worry anymore hereafter.
		 */
		clear_context_copied(iommu, bus, devfn);
	}

	context_entry_set_pasid_table(context, dev);
	spin_unlock(&iommu->lock);

	/*
	 * It's a non-present to present mapping. If hardware doesn't cache
	 * non-present entry we don't need to flush the caches. If it does
	 * cache non-present entries, then it does so in the special
	 * domain #0, which we have to flush:
	 */
	if (cap_caching_mode(iommu->cap)) {
		iommu->flush.flush_context(iommu, 0,
					   PCI_DEVID(bus, devfn),
					   DMA_CCMD_MASK_NOBIT,
					   DMA_CCMD_DEVICE_INVL);
		iommu->flush.flush_iotlb(iommu, 0, 0, 0, DMA_TLB_DSI_FLUSH);
	}

	return 0;
}

static int pci_pasid_table_setup(struct pci_dev *pdev, u16 alias, void *data)
{
	struct device *dev = data;

	if (dev != &pdev->dev)
		return 0;

	return device_pasid_table_setup(dev, PCI_BUS_NUM(alias), alias & 0xff);
}

/*
 * Set the device's PASID table to its context table entry.
 *
 * The PASID table is set to the context entries of both device itself
 * and its alias requester ID for DMA.
 */
int intel_pasid_setup_sm_context(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	if (!dev_is_pci(dev))
		return device_pasid_table_setup(dev, info->bus, info->devfn);

	return pci_for_each_dma_alias(to_pci_dev(dev), pci_pasid_table_setup, dev);
}

/*
 * Global Device-TLB invalidation following changes in a context entry which
 * was present.
 */
static void __context_flush_dev_iotlb(struct device_domain_info *info)
{
	if (!info->ats_enabled)
		return;

	/*
	 * Skip dev-IOTLB flush for inaccessible PCIe devices to prevent the
	 * Intel IOMMU from waiting indefinitely for an ATS invalidation that
	 * cannot complete.
	 */
	if (!pci_device_is_present(to_pci_dev(info->dev)))
		return;

	qi_flush_dev_iotlb(info->iommu, PCI_DEVID(info->bus, info->devfn),
			   info->pfsid, info->ats_qdep, 0, MAX_AGAW_PFN_WIDTH);

	/*
	 * There is no guarantee that the device DMA is stopped when it reaches
	 * here. Therefore, always attempt the extra device TLB invalidation
	 * quirk. The impact on performance is acceptable since this is not a
	 * performance-critical path.
	 */
	quirk_extra_dev_tlb_flush(info, 0, MAX_AGAW_PFN_WIDTH, IOMMU_NO_PASID,
				  info->ats_qdep);
}

/*
 * Cache invalidations after change in a context table entry that was present
 * according to the Spec 6.5.3.3 (Guidance to Software for Invalidations).
 * This helper can only be used when IOMMU is working in the legacy mode or
 * IOMMU is in scalable mode but all PASID table entries of the device are
 * non-present.
 */
void intel_context_flush_no_pasid(struct device_domain_info *info,
				  struct context_entry *context, u16 did)
{
	struct intel_iommu *iommu = info->iommu;

	/*
	 * Device-selective context-cache invalidation. The Domain-ID field
	 * of the Context-cache Invalidate Descriptor is ignored by hardware
	 * when operating in scalable mode. Therefore the @did value doesn't
	 * matter in scalable mode.
	 */
	iommu->flush.flush_context(iommu, did, PCI_DEVID(info->bus, info->devfn),
				   DMA_CCMD_MASK_NOBIT, DMA_CCMD_DEVICE_INVL);

	/*
	 * For legacy mode:
	 * - Domain-selective IOTLB invalidation
	 * - Global Device-TLB invalidation to all affected functions
	 */
	if (!sm_supported(iommu)) {
		iommu->flush.flush_iotlb(iommu, did, 0, 0, DMA_TLB_DSI_FLUSH);
		__context_flush_dev_iotlb(info);

		return;
	}

	__context_flush_dev_iotlb(info);
}
