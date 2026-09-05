// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011-2014 NVIDIA CORPORATION.  All rights reserved.
 */

/*
 * [한국어 설명] NVIDIA Tegra SMMU 드라이버 (tegra-smmu.c)
 *
 * === 파일의 역할 ===
 * Tegra30부터 Tegra124/210 세대까지의 NVIDIA SoC에 들어 있는 SMMU를
 * 리눅스 IOMMU 서브시스템에 붙이는 드라이버다. 이름은 ARM의 SMMU와
 * 같지만 전혀 다른 NVIDIA 자체 설계이며, 메모리 컨트롤러(MC) 블록의
 * 일부로 존재한다 — 그래서 이 드라이버는 독립 플랫폼 드라이버가 아니라
 * drivers/memory/tegra의 MC 드라이버가 호출하는 라이브러리 형태다
 * (tegra_smmu_probe()가 export 없이 호출되는 진입점이다).
 *
 * 구조를 이해하는 데 필요한 개념이 넷이다.
 *
 * (1) **2단계 평면 페이지 테이블**. PD(Page Directory) 1024 엔트리 ×
 *     PT(Page Table) 1024 엔트리 × 4KB 페이지 = 정확히 4GB IOVA 공간을
 *     덮는다. PDE와 PTE 모두 32비트이고, 상위 비트가 권한 플래그다.
 *
 * (2) **ASID와 그 재사용**. 도메인 하나가 ASID 하나를 갖고, TLB 엔트리가
 *     ASID로 태그된다. 그런데 여러 디바이스가 같은 도메인에 붙을 수 있어
 *     use_count로 참조를 세고, 마지막 detach에서야 ASID를 반납한다
 *     (tegra_smmu_as_prepare/unprepare).
 *
 * (3) **swgroup과 클라이언트**. Tegra의 메모리 클라이언트(디스플레이,
 *     비디오 등)는 "swgroup"으로 묶이고, 각 swgroup마다 ASID를 지정하는
 *     레지스터가 하나씩 있다. 디바이스 트리의 iommus 프로퍼티가 그
 *     swgroup 번호를 담으며, attach가 그 레지스터에 ASID를 써 넣는다.
 *     동시에 그 swgroup에 속한 개별 클라이언트들의 활성화 비트도 켠다.
 *
 * (4) **두 겹의 캐시**. TLB(변환 결과)와 PTC(Page Table Cache, 테이블
 *     내용)를 따로 무효화해야 한다. 게다가 테이블 메모리가 캐시
 *     코히런트하지 않아 dma_sync까지 세 단계를 거쳐야 갱신이 반영된다 —
 *     tegra_smmu_set_pde/set_pte가 그 순서를 보여 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [drivers/memory/tegra/mc.c] MC probe
 *        ↓ tegra_smmu_probe(dev, soc, mc)
 *   [이 파일] 레지스터 설정 → IOMMU 코어 등록
 *
 *   [디바이스 드라이버] dma_map_*() / iommu_map()
 *        ↓
 *   [IOMMU 코어] iommu_ops 디스패치
 *        ↓
 *   [이 파일] tegra_smmu_map() → PDE 확인(없으면 PT 할당) → PTE 기록
 *        ↓ 매 엔트리마다: dma_sync → PTC flush → TLB flush → 읽기 배리어
 *   [Tegra SMMU 하드웨어]
 *
 * 주목할 점: 현재 def_domain_type이 항상 IDENTITY를 반환해, 실질적으로
 * 모든 디바이스가 변환 없이 동작한다. 파일 안의 FIXME가 밝히듯 일부
 * 디바이스의 결함 때문에 임시로 전체를 통과 모드로 둔 상태다.
 * 즉 아래의 정교한 페이지 테이블 코드가 기본 경로에서는 쓰이지 않는다.
 *
 * 실행 컨텍스트: attach/detach는 프로세스 컨텍스트(smmu->lock 뮤텍스),
 * map/unmap은 as->lock 스핀락(irqsave)으로 보호되며 atomic일 수 있다.
 * 폴트 인터럽트 처리는 이 파일이 아니라 MC 드라이버가 담당한다.
 *
 * === 타 모듈과의 연결 ===
 * - soc/tegra/mc.h: struct tegra_mc, tegra_smmu_soc, tegra_mc_client 등.
 *   SoC별 swgroup/클라이언트 목록이 그쪽에 정의되어 있고 이 드라이버는
 *   그것을 데이터로 참조한다.
 * - drivers/memory/tegra/mc.c: 이 파일의 tegra_smmu_probe()/remove()를
 *   호출하는 유일한 소비자.
 * - soc/tegra/ahb.h: tegra_ahb_enable_smmu() — Tegra30에서 AHB 버스가
 *   SMMU를 거치도록 별도로 켜야 한다.
 * - iommu-pages.h: PD/PT를 __GFP_DMA로 받는 페이지 테이블 할당자.
 * 데이터 흐름: 디바이스 트리의 `iommus = <&mc swgroup>` → probe_device가
 * 직접 파싱해 of_xlate를 부르고 swgroup을 fwspec에 넣는다 → attach가
 * ASID를 확보하고 그 swgroup 레지스터에 기록한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct tegra_smmu: SMMU 인스턴스. 레지스터, ASID 비트맵, 그룹 목록.
 * - struct tegra_smmu_as: 주소 공간(= 도메인) 하나. PD와 PT 배열,
 *   PDE별 사용 카운트, ASID 번호.
 * - tegra_smmu_as_prepare(): ASID를 확보하고 PD를 하드웨어에 등록한다.
 *   use_count로 여러 디바이스의 공유를 관리한다.
 * - as_get_pde_page(): PT를 할당한다. 스핀락을 잠시 풀어 잠들 수 있는
 *   할당을 시도하는 것이 이 함수의 핵심 기법이다.
 * - tegra_smmu_set_pde()/set_pte(): 엔트리를 쓰고 세 단계 무효화를 수행한다.
 * - tegra_smmu_pte_put_use(): PT의 마지막 엔트리가 사라지면 PT를 반납한다.
 * - tegra_smmu_device_group(): swgroup이나 group_soc가 같은 디바이스를
 *   한 IOMMU 그룹으로 묶는다.
 * - tegra_smmu_probe(): MC 드라이버가 부르는 초기화 진입점.
 */

/* [한국어] BIT_MASK(), fls() 등 비트 조작 헬퍼. pfn_mask와 tlb_mask
 * 계산에 쓴다. */
#include <linux/bitops.h>
/* [한국어] debugfs 인터페이스 — swgroup과 클라이언트의 활성화 상태를
 * 사용자 공간에 노출한다. */
#include <linux/debugfs.h>
/* [한국어] IS_ERR/PTR_ERR/ERR_PTR — probe가 오류 포인터를 반환한다. */
#include <linux/err.h>
/* [한국어] IOMMU 코어 계약 — iommu_ops, iommu_group 관련 API. */
#include <linux/iommu.h>
/* [한국어] WARN_ON_ONCE() 등 기본 매크로. */
#include <linux/kernel.h>
/* [한국어] of_parse_phandle_with_args() 등 디바이스 트리 접근자.
 * 이 드라이버는 probe_device에서 iommus를 직접 파싱한다. */
#include <linux/of.h>
/* [한국어] of_find_device_by_node() — phandle에서 MC 플랫폼 디바이스를
 * 역추적한다. */
#include <linux/of_platform.h>
/* [한국어] dev_is_pci(), pci_device_group() — PCI 디바이스의 그룹 결정. */
#include <linux/pci.h>
/* [한국어] platform_get_drvdata() — MC 디바이스에서 tegra_mc를 꺼낸다. */
#include <linux/platform_device.h>
/* [한국어] kzalloc_obj/kcalloc/kfree/devm_kzalloc. */
#include <linux/slab.h>
/* [한국어] spinlock_t — 페이지 테이블 접근을 직렬화한다. */
#include <linux/spinlock.h>
/* [한국어] dma_map_single()/dma_sync_single_range_for_device() —
 * 테이블 메모리를 하드웨어에 보이게 유지한다. */
#include <linux/dma-mapping.h>

/* [한국어] tegra_ahb_enable_smmu() — Tegra30에서 AHB 버스 트래픽이
 * SMMU를 거치도록 별도로 켜야 한다. */
#include <soc/tegra/ahb.h>
/* [한국어] struct tegra_mc와 SoC별 swgroup/클라이언트 정의.
 * 이 드라이버가 참조하는 하드웨어 지식의 대부분이 여기서 온다. */
#include <soc/tegra/mc.h>

/* [한국어] iommu_alloc_pages_sz()/iommu_free_pages() — PD(4KB)와
 * PT(4KB)를 받는 페이지 테이블 전용 할당자. */
#include "iommu-pages.h"

/* [한국어] IOMMU 그룹 하나를 표현하는 구조체.
 * 왜 별도 구조체가 필요한가: 이 드라이버는 "같은 swgroup" 또는
 * "같은 group_soc에 속한 swgroup들"을 하나의 IOMMU 그룹으로 묶는데,
 * 그 대응 관계를 기억해 두어야 두 번째 디바이스가 같은 그룹에 합류할 수
 * 있기 때문이다.
 * 수명: device_group이 devm_kzalloc으로 만들고, 그룹이 해제될 때
 *       tegra_smmu_group_release가 목록에서 뺀다(메모리는 devm이 관리). */
struct tegra_smmu_group {
	struct list_head list;
	/* [한국어] smmu->groups 목록에 매다는 연결 고리.
	 * 설정자: device_group이 list_add_tail로 추가.
	 * 읽는 자: device_group이 기존 그룹을 찾을 때 순회한다.
	 * 동기화: smmu->lock 뮤텍스로 보호된다. */

	struct tegra_smmu *smmu;
	/* [한국어] 이 그룹이 속한 SMMU 인스턴스.
	 * 설정자: device_group.
	 * 읽는 자: group_release가 목록에서 뺄 때 락을 얻는 통로로 쓴다. */

	const struct tegra_smmu_group_soc *soc;
	/* [한국어] 이 그룹에 대응하는 SoC 정의(여러 swgroup을 묶는 규칙).
	 * 설정자: device_group이 tegra_smmu_find_group() 결과를 저장.
	 * 읽는 자: 다른 swgroup의 디바이스가 같은 group_soc에 속하는지
	 *          판별할 때 비교 대상이 된다.
	 * 값 범위: NULL일 수 있다 — 그 경우 swgroup이 정확히 같은
	 *          디바이스만 이 그룹을 공유한다. */

	struct iommu_group *group;
	/* [한국어] 실제 IOMMU 코어 그룹.
	 * 설정자: device_group이 pci_device_group() 또는
	 *          generic_device_group()으로 만든다.
	 * 읽는 자: 같은 그룹에 합류하는 디바이스가 참조를 얻어 간다. */

	unsigned int swgroup;
	/* [한국어] 이 그룹을 만든 첫 디바이스의 swgroup 번호.
	 * 설정자: device_group.
	 * 읽는 자: 같은 swgroup의 디바이스가 이 그룹을 찾을 때의 열쇠. */
};

/* [한국어] SMMU 인스턴스 하나의 상태.
 * 수명: tegra_smmu_probe()에서 devm_kzalloc으로 만들어져 MC 디바이스와
 *       함께 사라진다. 시스템에 하나뿐이다. */
struct tegra_smmu {
	void __iomem *regs;
	/* [한국어] SMMU 레지스터가 매핑된 주소.
	 * 설정자: probe가 mc->regs를 그대로 받는다 — SMMU가 MC 블록 안에
	 *          있어 레지스터 영역을 공유하기 때문이다.
	 * 읽는 자: smmu_readl()/writel()의 기준 주소. */

	struct device *dev;
	/* [한국어] MC 디바이스(= SMMU의 부모).
	 * 설정자: probe.
	 * 읽는 자: dma_map_single의 DMA 마스터, dev_dbg 로깅, devm 할당. */

	struct tegra_mc *mc;
	/* [한국어] 메모리 컨트롤러 인스턴스.
	 * 설정자: probe.
	 * 읽는 자: mc->soc의 atom_size(PTC 플러시 정렬)와
	 *          num_address_bits(pfn_mask 계산)를 읽는다. */

	const struct tegra_smmu_soc *soc;
	/* [한국어] 이 SoC의 SMMU 정의 — swgroup 목록, 클라이언트 목록,
	 * ASID 개수, TLB 라인 수 등.
	 * 설정자: probe가 인자로 받은 값을 저장.
	 * 읽는 자: 이 파일 전반. 세대별 차이가 전부 이 구조체에 담겨 있다. */

	struct list_head groups;
	/* [한국어] 만들어진 IOMMU 그룹들의 목록.
	 * 설정자: probe가 초기화하고 device_group이 추가한다.
	 * 읽는 자: device_group이 기존 그룹을 찾을 때 순회한다.
	 * 동기화: smmu->lock으로 보호된다. */

	unsigned long pfn_mask;
	/* [한국어] PDE/PTE에 담을 수 있는 물리 페이지 번호의 마스크.
	 * 설정자: probe가 mc->soc->num_address_bits에서 계산한다.
	 * 읽는 자: smmu_dma_addr_valid()가 주소가 담기는지 검사하고,
	 *          smmu_pde_to_dma()와 iova_to_phys()가 PFN을 뽑는다.
	 * 값 범위: 물리 주소 폭에 따라 다르지만 대개 20~22비트 분량. */

	unsigned long tlb_mask;
	/* [한국어] TLB 라인 수를 표현하는 데 필요한 비트 마스크.
	 * 설정자: probe가 fls(num_tlb_lines)로 계산.
	 * 읽는 자: SMMU_TLB_CONFIG_ACTIVE_LINES 매크로.
	 * 왜 필요한가: 활성 TLB 라인 수 필드에 값을 넣을 때 폭을 넘지
	 *              않게 잘라 주는 역할이다. */

	unsigned long *asids;
	/* [한국어] ASID 사용 여부 비트맵.
	 * 설정자: probe의 devm_bitmap_zalloc, alloc_asid가 세우고
	 *          free_asid가 지운다.
	 * 읽는 자: find_first_zero_bit로 빈 ASID를 찾을 때.
	 * 크기: soc->num_asids개(세대에 따라 4 또는 128). */

	struct mutex lock;
	/* [한국어] ASID 할당과 그룹 목록을 보호하는 뮤텍스.
	 * 설정자: probe가 초기화.
	 * 읽는 자: as_prepare/unprepare, device_group, group_release.
	 * 왜 뮤텍스인가: as_prepare가 dma_map_single을 부르며 잠들 수 있어
	 *                스핀락을 쓸 수 없다. */

	struct list_head list;
	/* [한국어] 여러 SMMU를 전역 목록으로 관리하던 시절의 흔적.
	 * 현재 코드에서는 초기화도 사용도 하지 않는다 — SMMU가 하나뿐이라
	 * 목록이 필요 없어졌기 때문이다. */

	struct dentry *debugfs;
	/* [한국어] debugfs 디렉토리 핸들.
	 * 설정자: tegra_smmu_debugfs_init().
	 * 읽는 자: debugfs_remove_recursive로 정리할 때.
	 * 값 범위: CONFIG_DEBUG_FS가 꺼져 있으면 만들어지지 않는다. */

	struct iommu_device iommu;	/* IOMMU Core code handle */
	/* [한국어] IOMMU 코어에 등록되는 핸들(임베드).
	 * 설정자: probe의 iommu_device_sysfs_add()/register().
	 * 읽는 자: probe_device()가 담당 IOMMU로 이 주소를 반환한다. */
};

/* [한국어] PD와 PT 구조체의 전방 선언. 아래 tegra_smmu_as가 이들의
 * 포인터를 갖는데, 실제 정의는 페이지 테이블 상수들 다음에 나온다. */
struct tegra_pd;
struct tegra_pt;

/* [한국어] 주소 공간(Address Space) 하나 — 곧 IOMMU 도메인이다.
 * "as"라는 이름이 Tegra 문서의 용어를 그대로 따른 것이다.
 * 수명: domain_alloc_paging에서 만들어져 domain_free에서 해제된다. */
struct tegra_smmu_as {
	struct iommu_domain domain;
	/* [한국어] IOMMU 코어가 보는 도메인 부분(임베드).
	 * 설정자: domain_alloc_paging이 pgsize_bitmap(4KB)과
	 *          geometry(0~4GB-1)를 채운다. */

	struct tegra_smmu *smmu;
	/* [한국어] 이 주소 공간이 붙어 있는 SMMU.
	 * 설정자: as_prepare가 첫 attach에서 기록하고, as_unprepare가
	 *          마지막 detach에서 NULL로 되돌린다.
	 * 읽는 자: 모든 테이블 조작과 무효화가 이 포인터로 하드웨어에 접근한다.
	 * 값 범위: NULL(attach 전) 또는 유효한 포인터. */

	unsigned int use_count;
	/* [한국어] 이 주소 공간에 붙어 있는 (디바이스, swgroup) 쌍의 수.
	 * 설정자: as_prepare가 증가, as_unprepare가 감소.
	 * 읽는 자: 0에서 1로 갈 때 ASID를 확보하고 PD를 등록하며,
	 *          1에서 0으로 갈 때 그것들을 되돌린다.
	 * 왜 필요한가: 여러 디바이스(또는 한 디바이스의 여러 swgroup)가
	 *              같은 도메인을 공유하므로, 하나가 떠날 때마다
	 *              ASID를 반납하면 나머지가 망가진다.
	 * 동기화: smmu->lock으로 보호된다. */

	spinlock_t lock;
	/* [한국어] 페이지 테이블 접근을 직렬화하는 스핀락.
	 * 설정자: domain_alloc_paging이 초기화.
	 * 읽는 자: map/unmap이 irqsave로 잡는다.
	 * 특이점: as_get_pde_page()가 이 락을 **잠시 풀었다가 다시 잡는다** —
	 *         잠들 수 있는 할당을 시도하기 위해서다. */

	u32 *count;
	/* [한국어] PDE마다 그 PT에서 사용 중인 PTE 개수를 세는 배열.
	 * 설정자: pte_get_use가 증가, pte_put_use가 감소.
	 * 읽는 자: pte_put_use가 0이 되는 순간 그 PT를 반납한다.
	 * 크기: SMMU_NUM_PDE(1024)개.
	 * 왜 필요한가: 빈 PT를 회수해 메모리를 아끼려는 것이다 —
	 *              대부분의 IOMMU 드라이버가 하지 않는 최적화다. */

	struct tegra_pt **pts;
	/* [한국어] PDE 인덱스 → PT의 커널 가상 주소 배열.
	 * 설정자: as_get_pte가 새 PT를 등록하고, pte_put_use가 NULL로 지운다.
	 * 읽는 자: pte_lookup과 as_get_pde_page.
	 * 왜 별도 배열인가: PDE에는 PT의 DMA 주소만 들어 있어 가상 주소를
	 *                   되찾을 수 없다. 그래서 소프트웨어가 따로 기억한다. */

	struct tegra_pd *pd;
	/* [한국어] 1단계 페이지 디렉토리(4KB, 1024 엔트리)의 커널 가상 주소.
	 * 설정자: domain_alloc_paging의 iommu_alloc_pages_sz().
	 * 읽는 자: PDE를 읽고 쓰는 모든 경로.
	 * 왜 __GFP_DMA인가: PD의 물리 주소가 32비트 PTB 레지스터에 담겨야 한다. */

	dma_addr_t pd_dma;
	/* [한국어] 그 PD의 DMA 주소.
	 * 설정자: as_prepare의 dma_map_single().
	 * 읽는 자: PTB_DATA 레지스터 기록, PDE 갱신 시 dma_sync와 PTC 플러시.
	 * 값 범위: pfn_mask에 담기는 범위여야 한다(as_prepare가 검증). */

	unsigned id;
	/* [한국어] 이 주소 공간에 할당된 ASID 번호.
	 * 설정자: as_prepare의 tegra_smmu_alloc_asid().
	 * 읽는 자: swgroup 레지스터에 기록되고, TLB 무효화의 태그로 쓰인다.
	 * 값 범위: 0 ~ soc->num_asids-1. */

	u32 attr;
	/* [한국어] PD 자체의 속성 비트(읽기/쓰기/비보안).
	 * 설정자: domain_alloc_paging이 세 비트를 모두 세운다.
	 * 읽는 자: as_prepare가 PTB_DATA에 PD 주소와 함께 기록한다.
	 * 값 범위: SMMU_PD_READABLE | WRITABLE | NONSECURE로 고정이다 —
	 *          도메인마다 다르게 둘 이유가 없어 상수처럼 쓰인다. */
};

/*
 * [한국어]
 * to_smmu_as - 일반 iommu_domain을 이 드라이버의 주소 공간으로 되돌린다
 *
 * @dom: 코어가 넘긴 일반 도메인 포인터.
 * @return: 그것을 감싸는 struct tegra_smmu_as 포인터.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄. 순수 포인터 산술이다.
 *
 * 호출 체인:
 *   map/unmap/attach/free/iova_to_phys → [to_smmu_as]
 */
static struct tegra_smmu_as *to_smmu_as(struct iommu_domain *dom)
{
	/* [한국어] 임베드된 멤버의 주소에서 오프셋을 빼 바깥 구조체를 얻는다. */
	return container_of(dom, struct tegra_smmu_as, domain);
}

/*
 * [한국어]
 * smmu_writel - SMMU 레지스터에 쓴다
 *
 * @smmu: 대상 인스턴스.
 * @value: 쓸 값.
 * @offset: 레지스터 오프셋.
 * @return: 없음.
 *
 * 인자 순서가 값 → 오프셋인 점에 주의: 흔한 관례(오프셋 → 값)와
 * 반대라 읽을 때 혼동하기 쉽다.
 *
 * 실행 컨텍스트: 설정, 무효화, swgroup 조작 전반.
 *
 * 호출 체인:
 *   무효화 함수들 / tegra_smmu_enable()/disable() / probe
 *   → [smmu_writel] → writel()
 */
static inline void smmu_writel(struct tegra_smmu *smmu, u32 value,
			       unsigned long offset)
{
	/* [한국어] MC 레지스터 영역의 베이스에 오프셋을 더해 쓴다. */
	writel(value, smmu->regs + offset);
}

/*
 * [한국어]
 * smmu_readl - SMMU 레지스터를 읽는다
 *
 * @smmu: 대상 인스턴스.
 * @offset: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 실행 컨텍스트: read-modify-write와 debugfs 덤프, 그리고 smmu_flush()의
 * 배리어 용도.
 *
 * 호출 체인:
 *   tegra_smmu_enable()/disable() / smmu_flush() / debugfs
 *   → [smmu_readl] → readl()
 */
static inline u32 smmu_readl(struct tegra_smmu *smmu, unsigned long offset)
{
	/* [한국어] 베이스에 오프셋을 더해 읽는다. */
	return readl(smmu->regs + offset);
}

/* [한국어] SMMU 전역 설정 레지스터. */
#define SMMU_CONFIG 0x010
/* [한국어] SMMU 활성화 비트. probe가 마지막에 세워 변환을 시작한다. */
#define  SMMU_CONFIG_ENABLE (1 << 0)

/* [한국어] TLB 동작을 제어하는 레지스터. */
#define SMMU_TLB_CONFIG 0x14
/* [한국어] hit-under-miss 활성화 — 미스 처리 중에도 다른 요청의 히트를
 * 처리해 지연을 줄인다. */
#define  SMMU_TLB_CONFIG_HIT_UNDER_MISS (1 << 29)
/* [한국어] 라운드로빈 중재 활성화 — 여러 클라이언트가 경합할 때
 * 공평하게 나눈다. 지원 여부가 SoC마다 달라 soc 플래그로 확인한다. */
#define  SMMU_TLB_CONFIG_ROUND_ROBIN_ARBITRATION (1 << 28)
/* [한국어] 활성화할 TLB 라인 수. soc가 정한 값을 tlb_mask로 잘라
 * 필드 폭을 넘지 않게 한다. */
#define  SMMU_TLB_CONFIG_ACTIVE_LINES(smmu) \
	((smmu)->soc->num_tlb_lines & (smmu)->tlb_mask)

/* [한국어] PTC(Page Table Cache) 동작을 제어하는 레지스터.
 * PTC는 TLB와 별개로 페이지 테이블의 **내용**을 캐시한다. */
#define SMMU_PTC_CONFIG 0x18
/* [한국어] PTC 활성화 비트. */
#define  SMMU_PTC_CONFIG_ENABLE (1 << 29)
/* [한국어] 동시 요청 개수 제한 필드. 지원하는 SoC에서만 8로 설정한다. */
#define  SMMU_PTC_CONFIG_REQ_LIMIT(x) (((x) & 0x0f) << 24)
/* [한국어] PTC 인덱스 매핑 필드. probe가 0x3f(전 비트)를 넣어
 * 모든 인덱스 비트를 쓰게 한다. */
#define  SMMU_PTC_CONFIG_INDEX_MAP(x) ((x) & 0x3f)

/* [한국어] PTB(Page Table Base) 설정 시 대상 ASID를 지정하는 레지스터.
 * 이 레지스터에 ASID를 쓴 뒤 PTB_DATA에 PD 주소를 쓰는 2단계 방식이다. */
#define SMMU_PTB_ASID 0x01c
/* [한국어] ASID 값 필드(7비트). */
#define  SMMU_PTB_ASID_VALUE(x) ((x) & 0x7f)

/* [한국어] 앞서 PTB_ASID로 지정한 ASID의 PD 주소와 속성을 쓰는 레지스터. */
#define SMMU_PTB_DATA 0x020
/* [한국어] PD의 DMA 주소를 페이지 번호로 바꾸고 속성을 얹는 매크로.
 * 12비트 시프트가 곧 4KB 페이지 번호로의 변환이다. */
#define  SMMU_PTB_DATA_VALUE(dma, attr) ((dma) >> 12 | (attr))

/* [한국어] PT의 DMA 주소로 PDE 값을 만드는 매크로. PTB_DATA와 같은
 * 형태이지만 시프트를 상수로 표현한 점만 다르다. */
#define SMMU_MK_PDE(dma, attr) ((dma) >> SMMU_PTE_SHIFT | (attr))

/* [한국어] TLB 무효화 명령 레지스터. 값의 하위 2비트가 매칭 방식을,
 * 상위 비트가 ASID와 주소를 담는다. */
#define SMMU_TLB_FLUSH 0x030
/* [한국어] 주소를 따지지 않고 전부 무효화. */
#define  SMMU_TLB_FLUSH_VA_MATCH_ALL     (0 << 0)
/* [한국어] 섹션(4MB, PDE 하나가 덮는 범위) 단위 무효화. */
#define  SMMU_TLB_FLUSH_VA_MATCH_SECTION (2 << 0)
/* [한국어] 그룹(16KB) 단위 무효화 — PTE 네 개 분량이다. */
#define  SMMU_TLB_FLUSH_VA_MATCH_GROUP   (3 << 0)
/* [한국어] 섹션 무효화용 값 조립. 주소에서 4MB 경계 위 비트만 남기고
 * 12비트 시프트한 뒤 매칭 방식을 얹는다. PDE를 고쳤을 때 쓴다. */
#define  SMMU_TLB_FLUSH_VA_SECTION(addr) ((((addr) & 0xffc00000) >> 12) | \
					  SMMU_TLB_FLUSH_VA_MATCH_SECTION)
/* [한국어] 그룹 무효화용 값 조립. 16KB 경계로 마스킹한다.
 * PTE 하나를 고쳐도 그것이 속한 16KB 그룹 전체를 비우는 셈인데,
 * 하드웨어가 그 단위로만 무효화하기 때문이다. */
#define  SMMU_TLB_FLUSH_VA_GROUP(addr)   ((((addr) & 0xffffc000) >> 12) | \
					  SMMU_TLB_FLUSH_VA_MATCH_GROUP)
/* [한국어] ASID 매칭 활성화 비트. 세우면 지정한 ASID의 엔트리만
 * 무효화되어 다른 주소 공간을 건드리지 않는다. */
#define  SMMU_TLB_FLUSH_ASID_MATCH       (1 << 31)

/* [한국어] PTC 무효화 명령 레지스터. */
#define SMMU_PTC_FLUSH 0x034
/* [한국어] PTC 전체 무효화. probe에서 한 번 쓴다. */
#define  SMMU_PTC_FLUSH_TYPE_ALL (0 << 0)
/* [한국어] 특정 주소의 PTC 라인만 무효화. 엔트리를 고칠 때마다 쓴다. */
#define  SMMU_PTC_FLUSH_TYPE_ADR (1 << 0)

/* [한국어] PTC 무효화 주소의 상위 비트를 담는 레지스터.
 * 물리 주소가 32비트를 넘는 SoC에서만 쓴다. */
#define SMMU_PTC_FLUSH_HI 0x9b8
/* [한국어] 그 상위 비트 필드의 마스크(2비트) — 최대 34비트 물리 주소까지. */
#define  SMMU_PTC_FLUSH_HI_MASK 0x3

/* per-SWGROUP SMMU_*_ASID register */
/* [한국어] swgroup 레지스터의 활성화 비트. 이 비트가 서야 그 swgroup의
 * 트래픽이 SMMU를 거친다. */
#define SMMU_ASID_ENABLE (1 << 31)
/* [한국어] swgroup 레지스터의 ASID 필드 마스크(7비트). */
#define SMMU_ASID_MASK 0x7f
/* [한국어] ASID 값을 그 필드에 맞게 자르는 매크로. */
#define SMMU_ASID_VALUE(x) ((x) & SMMU_ASID_MASK)

/* page table definitions */
/* [한국어] PD의 엔트리 개수. IOVA 비트 22~31(10비트)이 인덱스이므로 1024개다. */
#define SMMU_NUM_PDE 1024
/* [한국어] PT의 엔트리 개수. IOVA 비트 12~21(10비트)이 인덱스이므로 1024개다.
 * 1024 × 1024 × 4KB = 정확히 4GB로 32비트 IOVA 공간을 덮는다. */
#define SMMU_NUM_PTE 1024

/* [한국어] PD의 바이트 크기 = 1024 × 4바이트 = 4KB(페이지 하나). */
#define SMMU_SIZE_PD (SMMU_NUM_PDE * 4)
/* [한국어] PT의 바이트 크기 = 역시 4KB. 두 테이블이 모두 페이지 하나에
 * 딱 맞는 것이 이 설계의 깔끔한 점이다. */
#define SMMU_SIZE_PT (SMMU_NUM_PTE * 4)

/* [한국어] PD 인덱스가 IOVA에서 시작하는 비트 위치(22) —
 * PDE 하나가 4MB를 덮는다는 뜻이다. */
#define SMMU_PDE_SHIFT 22
/* [한국어] PT 인덱스가 시작하는 비트 위치(12) — 4KB 페이지 경계다. */
#define SMMU_PTE_SHIFT 12

/* [한국어] 페이지 경계 마스크. SMMU_SIZE_PT가 4KB이므로 하위 12비트를
 * 지우는 마스크가 된다(테이블 크기와 페이지 크기가 같아 성립하는 표현). */
#define SMMU_PAGE_MASK		(~(SMMU_SIZE_PT-1))
/* [한국어] 주소에서 페이지 내 오프셋만 뽑는다. PTE 포인터의 오프셋
 * 계산에도 쓰인다(테이블 안에서 몇 번째 바이트인지). */
#define SMMU_OFFSET_IN_PAGE(x)	((unsigned long)(x) & ~SMMU_PAGE_MASK)
/* [한국어] 페이지 번호를 물리 주소로 바꾼다. */
#define SMMU_PFN_PHYS(x)	((phys_addr_t)(x) << SMMU_PTE_SHIFT)
/* [한국어] 물리 주소를 페이지 번호로 바꾼다. */
#define SMMU_PHYS_PFN(x)	((unsigned long)((x) >> SMMU_PTE_SHIFT))

/* [한국어] PD 자체가 읽기 가능함을 나타내는 속성(PTB_DATA에 실린다). */
#define SMMU_PD_READABLE	(1 << 31)
/* [한국어] PD가 쓰기 가능함을 나타내는 속성. */
#define SMMU_PD_WRITABLE	(1 << 30)
/* [한국어] PD가 비보안 메모리에 있음을 나타내는 속성. */
#define SMMU_PD_NONSECURE	(1 << 29)

/* [한국어] PDE의 읽기 허용 비트. */
#define SMMU_PDE_READABLE	(1 << 31)
/* [한국어] PDE의 쓰기 허용 비트. */
#define SMMU_PDE_WRITABLE	(1 << 30)
/* [한국어] PDE가 가리키는 PT가 비보안 메모리에 있음을 나타내는 비트. */
#define SMMU_PDE_NONSECURE	(1 << 29)
/* [한국어] NEXT 비트 — 이 PDE가 다음 레벨 테이블을 가리킨다는 표시다.
 * 이 비트가 없으면 4MB 큰 페이지로 해석될 수 있으나, 이 드라이버는
 * 4KB만 쓰므로 항상 세운다. */
#define SMMU_PDE_NEXT		(1 << 28)

/* [한국어] PTE의 읽기 허용 비트. IOMMU_READ가 요청되면 세운다. */
#define SMMU_PTE_READABLE	(1 << 31)
/* [한국어] PTE의 쓰기 허용 비트. IOMMU_WRITE가 요청되면 세운다. */
#define SMMU_PTE_WRITABLE	(1 << 30)
/* [한국어] PTE가 가리키는 페이지가 비보안임을 나타내는 비트.
 * 이 드라이버는 항상 세운다 — 리눅스가 비보안 세계에서 돌기 때문이다. */
#define SMMU_PTE_NONSECURE	(1 << 29)

/* [한국어] PDE에 항상 붙이는 속성 묶음. 읽기/쓰기/비보안을 모두 허용하고,
 * 실제 권한 제어는 PTE에서 한다 — 상위 레벨에서 막을 이유가 없기 때문이다. */
#define SMMU_PDE_ATTR		(SMMU_PDE_READABLE | SMMU_PDE_WRITABLE | \
				 SMMU_PDE_NONSECURE)

/* [한국어] 1단계 페이지 디렉토리(4KB).
 * 구조체로 감싼 이유는 타입 안전성이다 — PD와 PT가 모두 u32 1024개라
 * 포인터를 혼동하기 쉬운데, 서로 다른 타입으로 두면 컴파일러가 잡아 준다. */
struct tegra_pd {
	u32 val[SMMU_NUM_PDE];
	/* [한국어] PDE 배열. 각 엔트리는 PT의 페이지 번호와 속성을 담는다.
	 * 설정자: tegra_smmu_set_pde().
	 * 읽는 자: pte_lookup, as_get_pte, pte_put_use가 PT 주소를 뽑을 때.
	 * 값 범위: 0(PT 없음) 또는 (PT 페이지 번호 | PDE_ATTR | PDE_NEXT). */
};

/* [한국어] 2단계 페이지 테이블(4KB). */
struct tegra_pt {
	u32 val[SMMU_NUM_PTE];
	/* [한국어] PTE 배열. 각 엔트리는 물리 페이지 번호와 권한을 담는다.
	 * 설정자: tegra_smmu_set_pte().
	 * 읽는 자: iova_to_phys와 unmap.
	 * 값 범위: 0(매핑 없음) 또는 (페이지 번호 | 권한 비트들). */
};

/*
 * [한국어]
 * iova_pd_index - IOVA에서 1단계(PD) 인덱스를 뽑는다
 *
 * @iova: 대상 I/O 가상 주소.
 * @return: PD 배열의 인덱스(0~1023).
 *
 * 22비트 시프트 후 하위 10비트만 남긴다. 마스크가 필요한 이유는
 * IOVA가 32비트를 넘는 타입일 수 있어 상위 쓰레기를 걸러야 하기 때문이다.
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   set_pde/pte_lookup/as_get_pte/pte_get_use/pte_put_use → [iova_pd_index]
 */
static unsigned int iova_pd_index(unsigned long iova)
{
	/* [한국어] 4MB 경계로 나눈 몫에서 하위 10비트를 취한다. */
	return (iova >> SMMU_PDE_SHIFT) & (SMMU_NUM_PDE - 1);
}

/*
 * [한국어]
 * iova_pt_index - IOVA에서 2단계(PT) 인덱스를 뽑는다
 *
 * @iova: 대상 IOVA.
 * @return: PT 배열의 인덱스(0~1023).
 *
 * 실행 컨텍스트: 모든 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   tegra_smmu_pte_offset() → [iova_pt_index]
 */
static unsigned int iova_pt_index(unsigned long iova)
{
	/* [한국어] 4KB 경계로 나눈 몫에서 하위 10비트를 취한다. */
	return (iova >> SMMU_PTE_SHIFT) & (SMMU_NUM_PTE - 1);
}

/*
 * [한국어]
 * smmu_dma_addr_valid - DMA 주소가 PDE/PTE에 담기는지 검사한다
 *
 * @smmu: 대상 인스턴스(pfn_mask를 읽는다).
 * @addr: 검사할 DMA 주소.
 * @return: 담기면 true.
 *
 * 왜 필요한가: PDE와 PTE가 32비트이고 그중 일부만 페이지 번호에 쓰인다.
 * 테이블 메모리가 그 범위 밖에 할당되면 주소를 담을 수 없어 조용히
 * 잘못된 곳을 가리키게 되므로, 할당 직후 반드시 검사해야 한다.
 *
 * 검사 방식: 페이지 번호로 바꾼 뒤 마스크를 씌워도 값이 그대로면
 * 그 마스크 안에 들어간다는 뜻이다.
 *
 * 실행 컨텍스트: PD/PT 할당 직후.
 *
 * 호출 체인:
 *   tegra_smmu_as_prepare() / as_get_pte() → [smmu_dma_addr_valid]
 */
static bool smmu_dma_addr_valid(struct tegra_smmu *smmu, dma_addr_t addr)
{
	/* [한국어] 페이지 번호로 바꾼다. 12는 SMMU_PTE_SHIFT와 같은 값인데
	 * 여기서는 상수를 직접 썼다. */
	addr >>= 12;
	/* [한국어] 마스크를 씌워도 값이 보존되면 표현 가능한 범위다. */
	return (addr & smmu->pfn_mask) == addr;
}

/*
 * [한국어]
 * smmu_pde_to_dma - PDE에서 PT의 DMA 주소를 복원한다
 *
 * @smmu: 대상 인스턴스.
 * @pde: 해석할 1단계 엔트리.
 * @return: 그 PDE가 가리키는 PT의 DMA 주소.
 *
 * pfn_mask로 페이지 번호만 뽑은 뒤 12비트 시프트해 주소로 되돌린다.
 * 속성 비트(상위 4비트)는 마스크에 걸려 자연히 제거된다.
 *
 * 실행 컨텍스트: 워크와 해제 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   pte_lookup / as_get_pte / pte_put_use → [smmu_pde_to_dma]
 */
static dma_addr_t smmu_pde_to_dma(struct tegra_smmu *smmu, u32 pde)
{
	/* [한국어] 페이지 번호만 뽑아 주소로 되돌린다. */
	return (dma_addr_t)(pde & smmu->pfn_mask) << 12;
}

/*
 * [한국어]
 * smmu_flush_ptc_all - PTC(페이지 테이블 캐시) 전체를 무효화한다
 *
 * @smmu: 대상 인스턴스.
 * @return: 없음.
 *
 * probe에서 부트로더가 남긴 캐시를 지우는 데만 쓰인다. 평소에는
 * 주소 단위 무효화(smmu_flush_ptc)로 충분하다.
 *
 * 실행 컨텍스트: probe.
 *
 * 호출 체인:
 *   tegra_smmu_probe() → [smmu_flush_ptc_all]
 */
static void smmu_flush_ptc_all(struct tegra_smmu *smmu)
{
	/* [한국어] TYPE_ALL(0)을 써서 PTC 전체를 비운다. */
	smmu_writel(smmu, SMMU_PTC_FLUSH_TYPE_ALL, SMMU_PTC_FLUSH);
}

/*
 * [한국어]
 * smmu_flush_ptc - 특정 주소의 PTC 라인을 무효화한다
 *
 * @smmu: 대상 인스턴스.
 * @dma: 테이블의 DMA 주소.
 * @offset: 그 테이블 안에서 고친 엔트리의 바이트 오프셋.
 * @return: 없음.
 *
 * PTC는 페이지 테이블의 **내용**을 캐시하므로, 엔트리를 고치면 그
 * 캐시 라인을 비워야 하드웨어가 새 값을 읽는다. TLB 무효화와는
 * 별개의 단계다.
 *
 * 오프셋을 atom_size로 내림하는 이유: PTC의 캐시 라인 단위가
 * atom_size(메모리 컨트롤러의 최소 전송 단위)라, 그 경계에 맞춰야
 * 올바른 라인이 지정된다.
 *
 * 상위 비트 처리: 물리 주소가 32비트를 넘는 SoC에서는 별도
 * 레지스터에 상위 비트를 먼저 써야 한다. 32비트 dma_addr_t 커널에서는
 * 상위 비트가 존재하지 않으므로 0을 쓴다.
 *
 * 실행 컨텍스트: PDE/PTE 갱신 직후와 as_prepare.
 *
 * 호출 체인:
 *   tegra_smmu_set_pde() / set_pte() / as_prepare() → [smmu_flush_ptc]
 */
static inline void smmu_flush_ptc(struct tegra_smmu *smmu, dma_addr_t dma,
				  unsigned long offset)
{
	/* [한국어] 레지스터에 쓸 값을 조립하는 임시 변수. */
	u32 value;

	/* [한국어] PTC 캐시 라인 경계로 오프셋을 내림한다. atom_size는
	 * 메모리 컨트롤러의 최소 전송 단위이자 PTC 라인 크기다. */
	offset &= ~(smmu->mc->soc->atom_size - 1);

	/* [한국어] 물리 주소가 32비트를 넘는 SoC라면 상위 비트를 먼저
	 * 별도 레지스터에 써야 한다. */
	if (smmu->mc->soc->num_address_bits > 32) {
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		/* [한국어] 64비트 dma_addr_t 커널에서만 상위 비트가 존재한다. */
		value = (dma >> 32) & SMMU_PTC_FLUSH_HI_MASK;
#else
		/* [한국어] 32비트 dma_addr_t라면 상위 비트가 있을 수 없다.
		 * 시프트 자체가 정의되지 않으므로 0을 쓴다. */
		value = 0;
#endif
		smmu_writel(smmu, value, SMMU_PTC_FLUSH_HI);
	}

	/* [한국어] 무효화할 주소와 "주소 지정 방식" 플래그를 함께 쓴다.
	 * 이 쓰기가 실제 무효화를 유발한다. */
	value = (dma + offset) | SMMU_PTC_FLUSH_TYPE_ADR;
	smmu_writel(smmu, value, SMMU_PTC_FLUSH);	/* [한국어] 조립한 값을 써서 그 주소의 PTC 라인을 비운다. */
}

/*
 * [한국어]
 * smmu_flush_tlb - TLB 전체를 무효화한다
 *
 * @smmu: 대상 인스턴스.
 * @return: 없음.
 *
 * ASID를 따지지 않고 모든 엔트리를 비운다. probe에서 부트로더가 남긴
 * 변환을 지우는 데만 쓰인다.
 *
 * 실행 컨텍스트: probe.
 *
 * 호출 체인:
 *   tegra_smmu_probe() → [smmu_flush_tlb]
 */
static inline void smmu_flush_tlb(struct tegra_smmu *smmu)
{
	/* [한국어] MATCH_ALL(0)만 쓰면 ASID 매칭 없이 전부 비워진다. */
	smmu_writel(smmu, SMMU_TLB_FLUSH_VA_MATCH_ALL, SMMU_TLB_FLUSH);
}

/*
 * [한국어]
 * smmu_flush_tlb_asid - 특정 ASID의 TLB 엔트리를 모두 무효화한다
 *
 * @smmu: 대상 인스턴스.
 * @asid: 대상 ASID.
 * @return: 없음.
 *
 * ASID 필드의 위치가 SoC마다 다르다: ASID가 4개뿐인 구세대(Tegra30)는
 * 비트 29~30에 2비트로, 그 이후는 비트 24~30에 7비트로 담는다.
 * 아래 세 무효화 함수가 모두 같은 분기를 반복하는데, 공통 함수로
 * 빼지 않은 것은 원본 그대로다.
 *
 * 실행 컨텍스트: as_prepare(ASID를 새로 쓰기 시작할 때).
 *
 * 호출 체인:
 *   tegra_smmu_as_prepare() → [smmu_flush_tlb_asid]
 */
static inline void smmu_flush_tlb_asid(struct tegra_smmu *smmu,
				       unsigned long asid)
{
	/* [한국어] 레지스터에 쓸 값. */
	u32 value;

	/* [한국어] ASID 필드의 위치가 세대에 따라 다르다. 4개뿐인
	 * 구세대는 2비트만 필요해 더 위쪽에 놓여 있다. */
	if (smmu->soc->num_asids == 4)
		value = (asid & 0x3) << 29;
	else
		value = (asid & 0x7f) << 24;

	/* [한국어] ASID 매칭을 켜고 주소는 따지지 않게 한다 —
	 * 그 결과 이 ASID의 모든 엔트리가 비워진다. */
	value |= SMMU_TLB_FLUSH_ASID_MATCH | SMMU_TLB_FLUSH_VA_MATCH_ALL;
	smmu_writel(smmu, value, SMMU_TLB_FLUSH);	/* [한국어] 조립한 값을 써서 그 ASID의 TLB 엔트리를 모두 비운다. */
}

/*
 * [한국어]
 * smmu_flush_tlb_section - 특정 ASID의 한 섹션(4MB) TLB를 무효화한다
 *
 * @smmu: 대상 인스턴스.
 * @asid: 대상 ASID.
 * @iova: 그 섹션에 속한 주소.
 * @return: 없음.
 *
 * PDE를 고쳤을 때 쓴다. PDE 하나가 4MB를 덮으므로 그 범위 전체의
 * 변환 결과가 무효가 되기 때문이다.
 *
 * 실행 컨텍스트: PDE 갱신 직후.
 *
 * 호출 체인:
 *   tegra_smmu_set_pde() → [smmu_flush_tlb_section]
 */
static inline void smmu_flush_tlb_section(struct tegra_smmu *smmu,
					  unsigned long asid,
					  unsigned long iova)
{
	/* [한국어] 레지스터에 쓸 값. */
	u32 value;

	/* [한국어] 세대별 ASID 필드 위치(위 함수와 동일한 분기). */
	if (smmu->soc->num_asids == 4)
		value = (asid & 0x3) << 29;
	else
		value = (asid & 0x7f) << 24;

	/* [한국어] ASID 매칭과 섹션 주소를 함께 지정한다. */
	value |= SMMU_TLB_FLUSH_ASID_MATCH | SMMU_TLB_FLUSH_VA_SECTION(iova);
	smmu_writel(smmu, value, SMMU_TLB_FLUSH);	/* [한국어] 조립한 값을 써서 그 섹션의 TLB를 비운다. */
}

/*
 * [한국어]
 * smmu_flush_tlb_group - 특정 ASID의 한 그룹(16KB) TLB를 무효화한다
 *
 * @smmu: 대상 인스턴스.
 * @asid: 대상 ASID.
 * @iova: 그 그룹에 속한 주소.
 * @return: 없음.
 *
 * PTE를 고쳤을 때 쓴다. 하드웨어가 TLB를 16KB(PTE 네 개) 단위로만
 * 무효화하므로, 한 페이지를 고쳐도 이웃 세 페이지의 캐시까지 함께
 * 사라진다 — 정확도를 포기한 대신 무효화 명령이 단순해진 설계다.
 *
 * 실행 컨텍스트: PTE 갱신 직후.
 *
 * 호출 체인:
 *   tegra_smmu_set_pte() → [smmu_flush_tlb_group]
 */
static inline void smmu_flush_tlb_group(struct tegra_smmu *smmu,
					unsigned long asid,
					unsigned long iova)
{
	/* [한국어] 레지스터에 쓸 값. */
	u32 value;

	/* [한국어] 세대별 ASID 필드 위치. */
	if (smmu->soc->num_asids == 4)
		value = (asid & 0x3) << 29;
	else
		value = (asid & 0x7f) << 24;

	/* [한국어] ASID 매칭과 16KB 그룹 주소를 함께 지정한다. */
	value |= SMMU_TLB_FLUSH_ASID_MATCH | SMMU_TLB_FLUSH_VA_GROUP(iova);
	smmu_writel(smmu, value, SMMU_TLB_FLUSH);	/* [한국어] 조립한 값을 써서 그 그룹의 TLB를 비운다. */
}

/*
 * [한국어]
 * smmu_flush - 앞선 레지스터 쓰기가 하드웨어에 도달했음을 보장한다
 *
 * @smmu: 대상 인스턴스.
 * @return: 없음.
 *
 * 값을 쓰지 않고 **읽기만** 한다. MMIO 읽기는 앞선 쓰기들이 모두
 * 하드웨어에 도달한 뒤에야 완료되므로, 이것이 곧 배리어 역할을 한다.
 * 읽은 값은 버린다 — PTB_ASID를 고른 것도 부작용이 없는 레지스터라서다.
 *
 * 왜 필요한가: 무효화 명령을 쓴 직후 호출자가 곧바로 매핑을 사용하면,
 * 명령이 아직 하드웨어에 도달하지 않아 옛 변환이 쓰일 수 있다.
 *
 * 실행 컨텍스트: 모든 엔트리 갱신의 마지막 단계.
 *
 * 호출 체인:
 *   set_pde/set_pte/as_prepare/probe → [smmu_flush] → smmu_readl()
 */
static inline void smmu_flush(struct tegra_smmu *smmu)
{
	/* [한국어] 아무 레지스터나 읽으면 앞선 쓰기가 확정된다.
	 * 반환값을 쓰지 않는 것이 의도적이다. */
	smmu_readl(smmu, SMMU_PTB_ASID);
}

/*
 * [한국어]
 * tegra_smmu_alloc_asid - 빈 ASID를 하나 확보한다
 *
 * @smmu: 대상 인스턴스.
 * @idp: 출력 인자 — 확보한 ASID 번호가 여기 저장된다.
 * @return: 0 성공, -ENOSPC(남은 ASID 없음).
 *
 * 락을 잡지 않는 점에 주목: 호출자(as_prepare)가 smmu->lock을 이미
 * 쥐고 있다는 전제다.
 *
 * 실행 컨텍스트: attach 경로, smmu->lock 보유 상태.
 *
 * 호출 체인:
 *   tegra_smmu_as_prepare() → [tegra_smmu_alloc_asid]
 */
static int tegra_smmu_alloc_asid(struct tegra_smmu *smmu, unsigned int *idp)
{
	/* [한국어] 찾은 ASID 번호. */
	unsigned long id;

	/* [한국어] 비트맵에서 처음으로 0인 자리를 찾는다. */
	id = find_first_zero_bit(smmu->asids, smmu->soc->num_asids);
	/* [한국어] 범위를 넘었다면 모든 ASID가 사용 중이다. */
	if (id >= smmu->soc->num_asids)
		return -ENOSPC;

	/* [한국어] 그 자리를 점유한다. */
	set_bit(id, smmu->asids);
	/* [한국어] 호출자에게 번호를 전달한다. */
	*idp = id;

	return 0;
}

/*
 * [한국어]
 * tegra_smmu_free_asid - ASID를 반납한다
 *
 * @smmu: 대상 인스턴스.
 * @id: 반납할 ASID 번호.
 * @return: 없음.
 *
 * 비트 하나를 지우는 것이 전부다. 하드웨어 쪽 정리(swgroup 레지스터의
 * 활성화 비트 해제)는 tegra_smmu_disable()이 따로 수행한다.
 *
 * 실행 컨텍스트: 마지막 detach, smmu->lock 보유 상태.
 *
 * 호출 체인:
 *   tegra_smmu_as_unprepare() → [tegra_smmu_free_asid]
 */
static void tegra_smmu_free_asid(struct tegra_smmu *smmu, unsigned int id)
{
	/* [한국어] 비트를 지워 다음 도메인이 이 ASID를 쓸 수 있게 한다. */
	clear_bit(id, smmu->asids);
}

/*
 * [한국어]
 * tegra_smmu_domain_alloc_paging - 페이징 도메인(주소 공간)을 만든다
 *
 * @dev: 요청한 디바이스(사용하지 않는다).
 * @return: 새 도메인의 iommu_domain 포인터, 실패하면 NULL.
 *
 * 세 가지 메모리를 할당한다:
 *  1) PD(4KB) — 하드웨어가 직접 읽는 1단계 테이블.
 *  2) count 배열(1024 × 4바이트) — PDE별 사용 카운트.
 *  3) pts 배열(1024 × 포인터) — PT의 가상 주소를 기억하는 소프트웨어 배열.
 * 뒤의 둘은 하드웨어가 보지 않는 순수 소프트웨어 구조다.
 *
 * ASID 확보와 하드웨어 등록은 첫 attach(as_prepare)로 미룬다 — 그때가
 * 되어야 어느 SMMU에 붙을지 알 수 있기 때문이다.
 *
 * __GFP_DMA로 PD를 받는 이유: 그 물리 주소가 32비트 PTB_DATA
 * 레지스터에 담겨야 한다.
 *
 * 오류 경로가 goto 없이 각 단계마다 앞선 할당을 되돌리는 형태라
 * 조금 장황하지만, 단계가 셋뿐이라 읽기에는 무리가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   iommu_domain_alloc() → iommu_ops->domain_alloc_paging
 *   → [tegra_smmu_domain_alloc_paging]
 */
static struct iommu_domain *tegra_smmu_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 새로 만들 주소 공간. */
	struct tegra_smmu_as *as;

	/* [한국어] 0으로 초기화해 할당한다. smmu와 use_count가 0으로
	 * 시작해야 "아직 attach 전"을 나타낸다. */
	as = kzalloc_obj(*as);
	if (!as)	/* [한국어] 도메인 구조체를 할당하지 못했다. */
		return NULL;

	/* [한국어] PD의 속성을 읽기/쓰기/비보안으로 고정한다.
	 * as_prepare가 PTB_DATA에 PD 주소와 함께 기록한다. */
	as->attr = SMMU_PD_READABLE | SMMU_PD_WRITABLE | SMMU_PD_NONSECURE;

	/* [한국어] 1단계 페이지 디렉토리(4KB)를 받는다. __GFP_DMA로
	 * 낮은 주소에서 받아야 32비트 레지스터에 담긴다.
	 * 0으로 초기화되어 모든 PDE가 "PT 없음" 상태다. */
	as->pd = iommu_alloc_pages_sz(GFP_KERNEL | __GFP_DMA, SMMU_SIZE_PD);
	if (!as->pd) {	/* [한국어] 1단계 페이지 디렉토리를 확보하지 못했다. */
		kfree(as);	/* [한국어] 앞서 만든 도메인 구조체를 되돌린다. */
		return NULL;	/* [한국어] 도메인을 만들지 못했음을 코어에 알린다. */
	}

	/* [한국어] PDE마다 사용 중인 PTE 개수를 셀 배열. 이것이 있어야
	 * 빈 PT를 회수할 수 있다. */
	as->count = kcalloc(SMMU_NUM_PDE, sizeof(u32), GFP_KERNEL);
	if (!as->count) {	/* [한국어] PDE별 카운트 배열을 확보하지 못했다. */
		iommu_free_pages(as->pd);	/* [한국어] PD를 되돌린다. */
		kfree(as);	/* [한국어] 도메인 구조체도 되돌린다. */
		return NULL;	/* [한국어] 도메인 생성 실패를 알린다. */
	}

	/* [한국어] PT의 커널 가상 주소를 기억하는 배열. PDE에는 DMA 주소만
	 * 있어 가상 주소를 되찾을 수 없으므로 별도로 둔다. */
	as->pts = kzalloc_objs(*as->pts, SMMU_NUM_PDE);
	if (!as->pts) {	/* [한국어] PT 주소 배열을 확보하지 못했다. */
		kfree(as->count);	/* [한국어] 카운트 배열을 되돌린다. */
		iommu_free_pages(as->pd);	/* [한국어] PD를 되돌린다. */
		kfree(as);	/* [한국어] 도메인 구조체를 되돌린다. */
		return NULL;	/* [한국어] 도메인 생성 실패를 알린다. */
	}

	/* [한국어] 페이지 테이블 접근을 직렬화할 락을 초기화한다. */
	spin_lock_init(&as->lock);

	/* [한국어] 지원 페이지 크기는 4KB 하나뿐이다. */
	as->domain.pgsize_bitmap = SZ_4K;

	/* setup aperture */
	/* [한국어] IOVA 공간의 시작을 0으로 둔다. */
	as->domain.geometry.aperture_start = 0;
	/* [한국어] 끝을 4GB-1로 둔다 — PD 1024 × PT 1024 × 4KB가 정확히
	 * 이 범위를 덮는다. */
	as->domain.geometry.aperture_end = 0xffffffff;
	/* [한국어] 코어가 이 범위를 강제하게 한다. */
	as->domain.geometry.force_aperture = true;

	/* [한국어] 코어에는 임베드된 일반 도메인 포인터를 돌려준다. */
	return &as->domain;
}

/*
 * [한국어]
 * tegra_smmu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 원본의 TODO가 밝히듯 **PD와 PT를 해제하지 않는다**. 소프트웨어
 * 배열(count, pts)과 도메인 구조체만 반납하므로, 실제로는 PD 4KB와
 * 남아 있는 PT들이 그대로 누수된다.
 * 도메인 해제가 드문 경로라 방치된 것으로 보이며, WARN_ON_ONCE로
 * use_count가 0이 아닌(= 아직 디바이스가 붙어 있는) 경우만 잡아 낸다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free → [tegra_smmu_domain_free]
 */
static void tegra_smmu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 이 드라이버의 주소 공간으로 복원한다. */
	struct tegra_smmu_as *as = to_smmu_as(domain);

	/* TODO: free page directory and page tables */

	/* [한국어] 아직 디바이스가 붙어 있는데 해제하려 한다면 상위 계층의
	 * 순서가 어긋난 것이다. ONCE인 이유는 같은 버그가 반복되면 로그가
	 * 넘치기 때문이다. */
	WARN_ON_ONCE(as->use_count);
	/* [한국어] PDE별 사용 카운트 배열을 반납한다. */
	kfree(as->count);
	/* [한국어] PT 주소 배열을 반납한다. 그 안의 PT들 자체는 해제되지
	 * 않는다는 점이 위 TODO가 가리키는 문제다. */
	kfree(as->pts);
	/* [한국어] 도메인 구조체를 반납한다. */
	kfree(as);
}

/*
 * [한국어]
 * tegra_smmu_find_swgroup - swgroup 번호로 SoC 정의를 찾는다
 *
 * @smmu: 대상 인스턴스.
 * @swgroup: 찾을 swgroup 번호.
 * @return: 그 swgroup의 정의(레지스터 오프셋과 이름), 없으면 NULL.
 *
 * SoC 데이터에 정의된 swgroup 배열을 선형 탐색한다. 개수가 수십 개
 * 수준이라 이 방식으로 충분하다.
 *
 * 반환값의 핵심은 group->reg — 그 swgroup의 ASID를 지정하는 레지스터의
 * 오프셋이다. attach/detach가 그 레지스터를 읽고 쓴다.
 *
 * 실행 컨텍스트: attach/detach 경로.
 *
 * 호출 체인:
 *   tegra_smmu_enable()/disable() → [tegra_smmu_find_swgroup]
 */
static const struct tegra_smmu_swgroup *
tegra_smmu_find_swgroup(struct tegra_smmu *smmu, unsigned int swgroup)
{
	/* [한국어] 찾은 정의. 없으면 NULL로 남는다. */
	const struct tegra_smmu_swgroup *group = NULL;
	/* [한국어] 순회 인덱스. */
	unsigned int i;

	/* [한국어] SoC가 정의한 모든 swgroup을 선형 탐색한다. */
	for (i = 0; i < smmu->soc->num_swgroups; i++) {
		if (smmu->soc->swgroups[i].swgroup == swgroup) {	/* [한국어] 찾는 swgroup 번호와 일치하는 정의를 만났다. */
			group = &smmu->soc->swgroups[i];	/* [한국어] 그 정의를 결과로 삼고 순회를 멈춘다. */
			break;
		}
	}

	/* [한국어] 찾은 정의 또는 NULL. */
	return group;
}

/*
 * [한국어]
 * tegra_smmu_enable - swgroup과 그에 속한 클라이언트들을 SMMU에 연결한다
 *
 * @smmu: 대상 인스턴스.
 * @swgroup: 활성화할 swgroup 번호.
 * @asid: 그 swgroup이 쓸 ASID.
 * @return: 없음.
 *
 * 두 단계로 이뤄진다:
 *  1) swgroup 레지스터에 ASID를 쓰고 활성화 비트를 세운다. 이것이
 *     "이 swgroup의 트래픽을 이 주소 공간으로 변환하라"는 지시다.
 *  2) 그 swgroup에 속한 개별 클라이언트들의 활성화 비트를 켠다.
 *     클라이언트마다 담당 레지스터와 비트 위치가 다르므로 전체를
 *     순회하며 swgroup이 일치하는 것만 처리한다.
 *
 * swgroup 정의를 찾지 못하면 경고 후 즉시 반환하는데, 원본 주석대로
 * 그 상태에서 클라이언트만 켜 봐야 의미가 없기 때문이다.
 *
 * read-modify-write를 쓰는 이유: 레지스터에 여러 클라이언트의 비트가
 * 함께 들어 있어, 통째로 쓰면 남의 설정을 지운다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   tegra_smmu_attach_dev() → [tegra_smmu_enable]
 *   → tegra_smmu_find_swgroup()
 */
static void tegra_smmu_enable(struct tegra_smmu *smmu, unsigned int swgroup,
			      unsigned int asid)
{
	/* [한국어] swgroup의 SoC 정의(레지스터 오프셋). */
	const struct tegra_smmu_swgroup *group;
	/* [한국어] 클라이언트 순회 인덱스. */
	unsigned int i;
	/* [한국어] read-modify-write에 쓸 임시 값. */
	u32 value;

	/* [한국어] 이 swgroup의 레지스터 위치를 찾는다. */
	group = tegra_smmu_find_swgroup(smmu, swgroup);
	if (group) {
		/* [한국어] 현재 값을 읽어 ASID 필드만 갈아 끼운다. */
		value = smmu_readl(smmu, group->reg);
		value &= ~SMMU_ASID_MASK;	/* [한국어] 기존 ASID 값을 지운다 — 새 값으로 갈아 끼우기 위함이다. */
		value |= SMMU_ASID_VALUE(asid);
		/* [한국어] 활성화 비트를 세워 이 swgroup의 변환을 켠다. */
		value |= SMMU_ASID_ENABLE;
		smmu_writel(smmu, value, group->reg);	/* [한국어] 조립한 값을 써서 이 swgroup을 활성화한다. */
	} else {
		/* [한국어] SoC 데이터에 없는 swgroup 번호다 — 디바이스 트리와
		 * SoC 정의가 어긋났다는 뜻이다. */
		pr_warn("%s group from swgroup %u not found\n", __func__,
				swgroup);
		/* No point moving ahead if group was not found */
		/* [한국어] 원본 주석대로, swgroup을 켜지 못한 상태에서
		 * 클라이언트만 켜 봐야 소용이 없다. */
		return;
	}

	/* [한국어] 이 swgroup에 속한 클라이언트들의 활성화 비트를 켠다.
	 * 클라이언트 배열 전체를 순회하며 swgroup이 일치하는 것만 처리한다. */
	for (i = 0; i < smmu->soc->num_clients; i++) {
		/* [한국어] i번째 클라이언트 정의(이름, swgroup, 레지스터/비트). */
		const struct tegra_mc_client *client = &smmu->soc->clients[i];

		/* [한국어] 다른 swgroup의 클라이언트는 건너뛴다. */
		if (client->swgroup != swgroup)
			continue;

		/* [한국어] 그 클라이언트의 비트만 세운다. read-modify-write가
		 * 필수인데, 한 레지스터에 여러 클라이언트가 모여 있기 때문이다. */
		value = smmu_readl(smmu, client->regs.smmu.reg);
		value |= BIT(client->regs.smmu.bit);	/* [한국어] 이 클라이언트의 비트만 세운다. */
		smmu_writel(smmu, value, client->regs.smmu.reg);	/* [한국어] 다른 클라이언트의 설정을 보존한 채 기록한다. */
	}
}

/*
 * [한국어]
 * tegra_smmu_disable - swgroup과 클라이언트들을 SMMU에서 분리한다
 *
 * @smmu: 대상 인스턴스.
 * @swgroup: 비활성화할 swgroup 번호.
 * @asid: 그 swgroup이 쓰던 ASID.
 * @return: 없음.
 *
 * enable의 대칭이다. 다만 두 가지가 다르다:
 *  - swgroup을 찾지 못해도 경고하지 않고 클라이언트 정리로 넘어간다.
 *  - ASID 필드를 지우고 다시 쓰는데, 활성화 비트를 내리므로 그 값은
 *    사실상 의미가 없다(하드웨어가 무시한다).
 *
 * 실행 컨텍스트: detach 경로.
 *
 * 호출 체인:
 *   tegra_smmu_identity_attach() / attach_dev의 오류 경로
 *   → [tegra_smmu_disable]
 */
static void tegra_smmu_disable(struct tegra_smmu *smmu, unsigned int swgroup,
			       unsigned int asid)
{
	/* [한국어] swgroup의 SoC 정의. */
	const struct tegra_smmu_swgroup *group;
	/* [한국어] 클라이언트 순회 인덱스. */
	unsigned int i;
	/* [한국어] read-modify-write용 임시 값. */
	u32 value;

	/* [한국어] 이 swgroup의 레지스터 위치를 찾는다. */
	group = tegra_smmu_find_swgroup(smmu, swgroup);
	if (group) {
		/* [한국어] ASID 필드를 갈아 끼우고 활성화 비트를 내린다.
		 * 비트를 내리면 ASID 값은 무의미하지만, 대칭성을 위해
		 * enable과 같은 형태로 써 준다. */
		value = smmu_readl(smmu, group->reg);
		value &= ~SMMU_ASID_MASK;	/* [한국어] 기존 ASID 값을 지운다. */
		value |= SMMU_ASID_VALUE(asid);	/* [한국어] 대칭성을 위해 ASID를 다시 넣지만, 비활성화되므로 의미는 없다. */
		value &= ~SMMU_ASID_ENABLE;	/* [한국어] 활성화 비트를 내려 이 swgroup의 변환을 끈다. */
		smmu_writel(smmu, value, group->reg);	/* [한국어] 조립한 값을 기록한다. */
	}

	/* [한국어] 그 swgroup에 속한 클라이언트들의 비트를 내린다. */
	for (i = 0; i < smmu->soc->num_clients; i++) {
		/* [한국어] i번째 클라이언트 정의. */
		const struct tegra_mc_client *client = &smmu->soc->clients[i];

		/* [한국어] 다른 swgroup은 건너뛴다. */
		if (client->swgroup != swgroup)
			continue;

		/* [한국어] 그 클라이언트의 비트만 지운다. */
		value = smmu_readl(smmu, client->regs.smmu.reg);
		value &= ~BIT(client->regs.smmu.bit);	/* [한국어] 이 클라이언트의 비트만 지운다. */
		smmu_writel(smmu, value, client->regs.smmu.reg);	/* [한국어] 다른 클라이언트의 설정을 보존한 채 기록한다. */
	}
}

/*
 * [한국어]
 * tegra_smmu_as_prepare - 주소 공간을 하드웨어에 등록한다(참조 카운팅)
 *
 * @smmu: 대상 인스턴스.
 * @as: 준비할 주소 공간.
 * @return: 0 성공, -ENOMEM(PD 매핑 실패/주소 범위 초과), -ENOSPC(ASID 고갈).
 *
 * use_count가 이 함수의 핵심이다. 여러 디바이스(또는 한 디바이스의
 * 여러 swgroup)가 같은 도메인을 공유하므로, 실제 하드웨어 등록은
 * 첫 번째 호출에서만 한다. 두 번째부터는 카운트만 올리고 곧바로 나간다.
 *
 * 첫 호출에서 하는 일:
 *  1) PD를 DMA로 매핑해 하드웨어가 읽을 수 있게 한다.
 *  2) 그 주소가 PTB_DATA에 담기는 범위인지 검사한다(주석대로 64비트
 *     DMA 주소는 다룰 수 없다).
 *  3) ASID를 확보한다.
 *  4) PTC와 TLB를 비운다 — 그 ASID를 이전에 쓰던 흔적을 지우기 위함이다.
 *  5) PTB_ASID로 대상 ASID를 고른 뒤 PTB_DATA에 PD 주소를 쓴다.
 *     이 2단계 방식이 이 하드웨어의 PD 등록 절차다.
 *  6) smmu_flush()로 쓰기가 도달했음을 보장한다.
 *
 * 실행 컨텍스트: attach 경로. smmu->lock 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   tegra_smmu_attach_dev() → [tegra_smmu_as_prepare]
 *   → dma_map_single(), tegra_smmu_alloc_asid()
 */
static int tegra_smmu_as_prepare(struct tegra_smmu *smmu,
				 struct tegra_smmu_as *as)
{
	/* [한국어] PTB_DATA에 쓸 값. */
	u32 value;
	/* [한국어] 결과 코드. */
	int err = 0;

	/* [한국어] ASID 할당과 use_count를 보호한다. dma_map_single이
	 * 잠들 수 있어 뮤텍스를 쓴다. */
	mutex_lock(&smmu->lock);

	/* [한국어] 이미 등록된 주소 공간이면 카운트만 올리고 끝낸다.
	 * 두 번째 디바이스가 같은 도메인에 붙는 흔한 경우다. */
	if (as->use_count > 0) {
		as->use_count++;	/* [한국어] 이미 등록된 주소 공간이므로 참조만 하나 늘린다. */
		goto unlock;	/* [한국어] 락을 풀러 간다 — err이 0이라 성공으로 반환된다. */
	}

	/* [한국어] PD를 DMA로 매핑해 하드웨어가 읽을 수 있게 한다.
	 * DMA_TO_DEVICE는 CPU가 쓰고 하드웨어가 읽는다는 뜻이다. */
	as->pd_dma =
		dma_map_single(smmu->dev, as->pd, SMMU_SIZE_PD, DMA_TO_DEVICE);
	if (dma_mapping_error(smmu->dev, as->pd_dma)) {	/* [한국어] PD를 하드웨어에 보이게 만들지 못했다. */
		err = -ENOMEM;	/* [한국어] 매핑 실패를 메모리 부족으로 보고한다. */
		goto unlock;	/* [한국어] 되돌릴 자원이 없으므로 락만 풀러 간다. */
	}

	/* We can't handle 64-bit DMA addresses */
	/* [한국어] PTB_DATA에 담을 수 있는 범위인지 확인한다. 주석대로
	 * 이 레지스터가 32비트라 64비트 DMA 주소는 표현할 수 없다. */
	if (!smmu_dma_addr_valid(smmu, as->pd_dma)) {
		err = -ENOMEM;	/* [한국어] 32비트 PTB_DATA에 담을 수 없는 주소다. */
		goto err_unmap;	/* [한국어] 방금 만든 PD 매핑을 되돌리러 간다. */
	}

	/* [한국어] 이 주소 공간이 쓸 ASID를 확보한다. */
	err = tegra_smmu_alloc_asid(smmu, &as->id);
	if (err < 0)	/* [한국어] 모든 ASID가 사용 중이면 이 주소 공간을 등록할 수 없다. */
		goto err_unmap;

	/* [한국어] PD 내용이 캐시에 남아 있을 수 있으므로 PTC를 비운다. */
	smmu_flush_ptc(smmu, as->pd_dma, 0);
	/* [한국어] 이 ASID를 이전에 쓰던 도메인의 TLB 엔트리를 지운다 —
	 * ASID가 재사용되므로 반드시 필요한 단계다. */
	smmu_flush_tlb_asid(smmu, as->id);

	/* [한국어] PD를 등록할 대상 ASID를 먼저 고른다. */
	smmu_writel(smmu, as->id & 0x7f, SMMU_PTB_ASID);
	/* [한국어] 그다음 PD의 페이지 번호와 속성을 쓴다. 이 2단계가
	 * 이 하드웨어의 PD 등록 방식이다. */
	value = SMMU_PTB_DATA_VALUE(as->pd_dma, as->attr);
	smmu_writel(smmu, value, SMMU_PTB_DATA);
	/* [한국어] 쓰기가 하드웨어에 도달했음을 보장한다. */
	smmu_flush(smmu);

	/* [한국어] 이 주소 공간이 어느 SMMU에 속하는지 기록한다.
	 * 이후 모든 테이블 조작이 이 포인터를 쓴다. */
	as->smmu = smmu;
	/* [한국어] 첫 사용자를 등록한다. */
	as->use_count++;

	mutex_unlock(&smmu->lock);	/* [한국어] 등록이 끝났으니 락을 푼다. */

	return 0;

/* [한국어] ASID 확보나 주소 검증이 실패했을 때의 되감기 — PD 매핑을 푼다. */
err_unmap:
	dma_unmap_single(smmu->dev, as->pd_dma, SMMU_SIZE_PD, DMA_TO_DEVICE);
/* [한국어] 카운트만 올린 성공 경로와 모든 실패가 모이는 지점. */
unlock:
	mutex_unlock(&smmu->lock);

	/* [한국어] 카운트만 올린 경우 err이 0이라 성공으로 반환된다. */
	return err;
}

/*
 * [한국어]
 * tegra_smmu_as_unprepare - 주소 공간의 참조를 하나 줄인다
 *
 * @smmu: 대상 인스턴스.
 * @as: 정리할 주소 공간.
 * @return: 없음.
 *
 * 마지막 사용자가 떠날 때만 실제 정리를 한다: ASID를 반납하고
 * PD의 DMA 매핑을 푼다. PD 메모리 자체는 도메인이 살아 있는 한 유지된다.
 *
 * as->smmu를 NULL로 되돌리는 것이 중요하다 — 다음 attach가
 * "아직 등록 전"으로 올바르게 판단하게 한다.
 *
 * 실행 컨텍스트: detach 경로. smmu->lock 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   tegra_smmu_identity_attach() / attach_dev의 오류 경로
 *   → [tegra_smmu_as_unprepare] → tegra_smmu_free_asid()
 */
static void tegra_smmu_as_unprepare(struct tegra_smmu *smmu,
				    struct tegra_smmu_as *as)
{
	/* [한국어] use_count와 ASID 비트맵을 보호한다. */
	mutex_lock(&smmu->lock);

	/* [한국어] 아직 다른 사용자가 남아 있으면 카운트만 줄이고 끝낸다.
	 * 전위 감소로 줄인 뒤의 값을 검사한다. */
	if (--as->use_count > 0) {
		mutex_unlock(&smmu->lock);	/* [한국어] 아직 다른 사용자가 남아 있으므로 정리하지 않는다. */
		return;
	}

	/* [한국어] 마지막 사용자가 떠났으므로 ASID를 반납한다. */
	tegra_smmu_free_asid(smmu, as->id);

	/* [한국어] PD의 DMA 매핑을 푼다. 메모리 자체는 도메인이 유지한다. */
	dma_unmap_single(smmu->dev, as->pd_dma, SMMU_SIZE_PD, DMA_TO_DEVICE);

	/* [한국어] 어느 SMMU에도 속하지 않는 상태로 되돌린다 —
	 * 다음 attach가 as_prepare의 첫 경로를 타게 된다. */
	as->smmu = NULL;

	mutex_unlock(&smmu->lock);	/* [한국어] ASID 반납과 매핑 해제가 끝났으니 락을 푼다. */
}

/*
 * [한국어]
 * tegra_smmu_attach_dev - 디바이스를 주소 공간에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙일 디바이스.
 * @old: 직전 도메인(사용하지 않는다).
 * @return: 0 성공, -ENOENT(fwspec 없음), -ENODEV(swgroup이 하나도 없음),
 *          as_prepare가 낸 오류.
 *
 * 이 디바이스의 각 swgroup마다 as_prepare를 부르고 하드웨어를 켠다.
 * as_prepare가 참조 카운팅을 하므로, swgroup이 여러 개면 카운트도
 * 그만큼 올라간다 — detach가 같은 횟수만큼 unprepare를 부르므로
 * 균형이 맞는다.
 *
 * `if (index == 0)` 검사: 루프가 한 번도 돌지 않았다는 것은
 * fwspec에 swgroup이 하나도 없다는 뜻이라, 붙일 대상이 없다.
 *
 * 오류 경로가 정교하다: 실패 지점까지 성공했던 swgroup들을 역순으로
 * 되돌린다. `while (index--)`가 그 되감기다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev
 *   → [tegra_smmu_attach_dev] → tegra_smmu_as_prepare(), tegra_smmu_enable()
 */
static int tegra_smmu_attach_dev(struct iommu_domain *domain,
				 struct device *dev, struct iommu_domain *old)
{
	/* [한국어] 이 디바이스의 swgroup 번호들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] of_xlate가 심어 둔 SMMU 인스턴스. */
	struct tegra_smmu *smmu = dev_iommu_priv_get(dev);
	/* [한국어] 붙일 주소 공간. */
	struct tegra_smmu_as *as = to_smmu_as(domain);
	/* [한국어] swgroup 순회 인덱스. 오류 되감기에도 쓰인다. */
	unsigned int index;
	/* [한국어] as_prepare의 결과. */
	int err;

	/* [한국어] fwspec이 없다면 이 디바이스는 IOMMU 설정을 갖지 않는다. */
	if (!fwspec)
		return -ENOENT;

	/* [한국어] 각 swgroup마다 주소 공간을 준비하고 하드웨어를 켠다. */
	for (index = 0; index < fwspec->num_ids; index++) {
		/* [한국어] 참조를 하나 올린다. 첫 호출이면 ASID를 확보하고
		 * PD를 등록한다. */
		err = tegra_smmu_as_prepare(smmu, as);
		if (err)	/* [한국어] 참조를 올리지 못했으면 이미 켠 swgroup들을 되돌려야 한다. */
			goto disable;

		/* [한국어] 그 swgroup의 레지스터에 ASID를 쓰고 클라이언트들을
		 * 켠다. 이 시점부터 그 클라이언트들의 DMA가 변환된다. */
		tegra_smmu_enable(smmu, fwspec->ids[index], as->id);
	}

	/* [한국어] 루프가 한 번도 돌지 않았다면 swgroup이 없다는 뜻이라
	 * 붙일 대상이 없다. */
	if (index == 0)
		return -ENODEV;

	return 0;

/* [한국어] 중간에 실패했을 때 이미 성공한 swgroup들을 되돌리는 지점. */
disable:
	/* [한국어] 후위 감소를 쓰므로 실패한 index는 건너뛰고 그 앞의
	 * 성공한 것들만 역순으로 정리한다. */
	while (index--) {
		tegra_smmu_disable(smmu, fwspec->ids[index], as->id);	/* [한국어] 이 swgroup의 하드웨어 설정을 되돌린다. */
		tegra_smmu_as_unprepare(smmu, as);	/* [한국어] 올렸던 참조도 하나 줄인다. */
	}

	return err;	/* [한국어] 실패를 유발한 오류 코드를 반환한다. */
}

/*
 * [한국어]
 * tegra_smmu_identity_attach - 항등 도메인으로 전환한다(= detach)
 *
 * @identity_domain: 정적 항등 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인. 이 함수의 작업 대상이다.
 * @return: 0 성공, -ENODEV(fwspec 없음).
 *
 * attach의 역순이다: 각 swgroup을 하드웨어에서 끄고 참조를 하나씩 줄인다.
 * 마지막 참조가 사라지면 as_unprepare가 ASID를 반납한다.
 *
 * 두 검사의 순서에 주목: fwspec을 먼저 보고, 그다음 old를 본다.
 * old가 항등 도메인이면 되돌릴 것이 없다.
 *
 * 실행 컨텍스트: detach/도메인 전환 경로.
 *
 * 호출 체인:
 *   iommu_detach_device() → domain_ops->attach_dev
 *   → [tegra_smmu_identity_attach] → tegra_smmu_disable(), as_unprepare()
 */
static int tegra_smmu_identity_attach(struct iommu_domain *identity_domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	/* [한국어] 이 디바이스의 swgroup 번호들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 떼어낼 옛 주소 공간. */
	struct tegra_smmu_as *as;
	/* [한국어] 그 주소 공간이 붙어 있던 SMMU. */
	struct tegra_smmu *smmu;
	/* [한국어] swgroup 순회 인덱스. */
	unsigned int index;

	/* [한국어] IOMMU 설정이 없는 디바이스다. */
	if (!fwspec)
		return -ENODEV;

	/* [한국어] 이미 항등 상태이거나 붙은 적이 없으면 할 일이 없다. */
	if (old == identity_domain || !old)
		return 0;

	/* [한국어] 옛 도메인을 이 드라이버의 형태로 복원한다. */
	as = to_smmu_as(old);
	/* [한국어] 그 주소 공간이 붙어 있던 SMMU를 얻는다. */
	smmu = as->smmu;
	/* [한국어] 각 swgroup을 끄고 참조를 하나씩 줄인다.
	 * attach가 swgroup 개수만큼 prepare를 불렀으므로 대칭이 맞는다. */
	for (index = 0; index < fwspec->num_ids; index++) {
		tegra_smmu_disable(smmu, fwspec->ids[index], as->id);	/* [한국어] 이 swgroup의 하드웨어 설정을 끈다. */
		tegra_smmu_as_unprepare(smmu, as);	/* [한국어] 참조를 하나 줄인다 — 마지막이면 ASID가 반납된다. */
	}
	return 0;	/* [한국어] 모든 swgroup을 정리했다. */
}

/* [한국어] 항등 도메인의 연산 테이블. attach_dev 하나뿐이다. */
static struct iommu_domain_ops tegra_smmu_identity_ops = {
	/* [한국어] swgroup 연결을 끊고 ASID 참조를 줄이는 콜백. */
	.attach_dev = tegra_smmu_identity_attach,
};

/* [한국어] 정적 항등 도메인.
 * 이 드라이버에서는 특히 중요한데, def_domain_type이 항상 IDENTITY를
 * 반환하므로 **모든 디바이스의 기본 도메인**이 바로 이것이다. */
static struct iommu_domain tegra_smmu_identity_domain = {
	/* [한국어] 코어가 항등 도메인임을 알아보는 종류 표시. */
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 위에서 정의한 콜백 하나짜리 테이블. */
	.ops = &tegra_smmu_identity_ops,
};

/*
 * [한국어]
 * tegra_smmu_set_pde - PDE 하나를 쓰고 세 단계 무효화를 수행한다
 *
 * @as: 대상 주소 공간.
 * @iova: 그 PDE가 담당하는 주소.
 * @value: 쓸 PDE 값(0이면 PT 제거).
 * @return: 없음.
 *
 * 이 함수의 네 단계가 이 하드웨어의 갱신 절차를 그대로 보여 준다:
 *  1) 메모리에 값을 쓴다.
 *  2) dma_sync로 CPU 캐시를 메모리로 밀어낸다 — 테이블이 코히런트하지
 *     않아 이 단계가 없으면 하드웨어가 옛 값을 본다.
 *  3) PTC를 비운다 — 하드웨어가 테이블 내용을 따로 캐시하기 때문이다.
 *  4) TLB의 해당 섹션(4MB)을 비운다 — 그 PDE가 덮는 범위의 변환 결과가
 *     무효가 되기 때문이다.
 *  5) smmu_flush로 명령들이 도달했음을 보장한다.
 *
 * 세 겹의 캐시(CPU 캐시, PTC, TLB)를 모두 다뤄야 하는 것이 이
 * 하드웨어를 다루는 데 가장 번거로운 지점이다.
 *
 * 실행 컨텍스트: PT 설치/제거 경로. as->lock 보유 상태.
 *
 * 호출 체인:
 *   as_get_pte() / tegra_smmu_pte_put_use() → [tegra_smmu_set_pde]
 */
static void tegra_smmu_set_pde(struct tegra_smmu_as *as, unsigned long iova,
			       u32 value)
{
	/* [한국어] 이 IOVA가 가리키는 PDE의 인덱스. */
	unsigned int pd_index = iova_pd_index(iova);
	/* [한국어] 무효화 명령을 보낼 SMMU. */
	struct tegra_smmu *smmu = as->smmu;
	/* [한국어] 고칠 PDE의 주소. */
	u32 *pd = &as->pd->val[pd_index];
	/* [한국어] PD 안에서 그 엔트리의 바이트 오프셋 — dma_sync와
	 * PTC 플러시가 요구한다. */
	unsigned long offset = pd_index * sizeof(*pd);

	/* Set the page directory entry first */
	/* [한국어] 1단계: 메모리에 값을 쓴다. */
	*pd = value;

	/* The flush the page directory entry from caches */
	/* [한국어] 2단계: CPU 캐시를 메모리로 밀어낸다. 테이블이 코히런트하지
	 * 않아 이것이 없으면 하드웨어가 옛 값을 읽는다.
	 * 엔트리 하나(4바이트) 범위만 동기화해 비용을 줄인다. */
	dma_sync_single_range_for_device(smmu->dev, as->pd_dma, offset,
					 sizeof(*pd), DMA_TO_DEVICE);

	/* And flush the iommu */
	/* [한국어] 3단계: PTC에서 그 라인을 비운다 — 하드웨어가 테이블
	 * 내용을 따로 캐시하기 때문이다. */
	smmu_flush_ptc(smmu, as->pd_dma, offset);
	/* [한국어] 4단계: 그 PDE가 덮는 4MB 섹션의 TLB를 비운다. */
	smmu_flush_tlb_section(smmu, as->id, iova);
	/* [한국어] 5단계: 명령들이 하드웨어에 도달했음을 보장한다. */
	smmu_flush(smmu);
}

/*
 * [한국어]
 * tegra_smmu_pte_offset - PT 안에서 해당 PTE의 주소를 구한다
 *
 * @pt: 2단계 페이지 테이블.
 * @iova: 대상 IOVA.
 * @return: 그 PTE의 주소.
 *
 * 실행 컨텍스트: 워크 경로. 순수 계산이다.
 *
 * 호출 체인:
 *   tegra_smmu_pte_lookup() / as_get_pte() → [tegra_smmu_pte_offset]
 */
static u32 *tegra_smmu_pte_offset(struct tegra_pt *pt, unsigned long iova)
{
	/* [한국어] IOVA에서 2단계 인덱스를 구해 그 엔트리를 가리킨다. */
	return &pt->val[iova_pt_index(iova)];
}

/*
 * [한국어]
 * tegra_smmu_pte_lookup - IOVA에 해당하는 PTE를 찾는다(만들지는 않는다)
 *
 * @as: 대상 주소 공간.
 * @iova: 찾을 IOVA.
 * @dmap: 출력 인자 — 그 PT의 DMA 주소가 저장된다.
 * @return: PTE의 주소, PT가 없으면 NULL.
 *
 * 읽기 전용 워크다. PT가 없으면 만들지 않고 NULL을 준다 —
 * unmap과 iova_to_phys가 이 함수를 쓴다.
 *
 * dmap을 함께 돌려주는 이유: 호출자가 그 PTE를 고치려면 dma_sync와
 * PTC 플러시에 PT의 DMA 주소가 필요한데, PDE에서 다시 뽑는 수고를
 * 덜어 주는 것이다.
 *
 * 실행 컨텍스트: unmap과 조회 경로. as->lock 보유 상태.
 *
 * 호출 체인:
 *   __tegra_smmu_unmap() / tegra_smmu_iova_to_phys()
 *   → [tegra_smmu_pte_lookup]
 */
static u32 *tegra_smmu_pte_lookup(struct tegra_smmu_as *as, unsigned long iova,
				  dma_addr_t *dmap)
{
	/* [한국어] 1단계 인덱스. */
	unsigned int pd_index = iova_pd_index(iova);
	/* [한국어] PDE에서 DMA 주소를 뽑는 데 필요한 SMMU. */
	struct tegra_smmu *smmu = as->smmu;
	/* [한국어] 2단계 테이블. */
	struct tegra_pt *pt;

	/* [한국어] 소프트웨어 배열에서 PT의 가상 주소를 얻는다.
	 * PDE에는 DMA 주소만 있어 이 배열이 필요하다. */
	pt = as->pts[pd_index];
	/* [한국어] PT가 없으면 이 영역에 매핑이 하나도 없다는 뜻이다. */
	if (!pt)
		return NULL;

	/* [한국어] 호출자가 쓸 수 있도록 PT의 DMA 주소를 함께 돌려준다. */
	*dmap = smmu_pde_to_dma(smmu, as->pd->val[pd_index]);

	/* [한국어] 그 PT 안에서 해당 PTE의 주소를 구해 반환한다. */
	return tegra_smmu_pte_offset(pt, iova);
}

/*
 * [한국어]
 * as_get_pte - IOVA에 해당하는 PTE를 얻는다(PT가 없으면 설치한다)
 *
 * @as: 대상 주소 공간.
 * @iova: 매핑할 IOVA.
 * @dmap: 출력 인자 — PT의 DMA 주소.
 * @pt: 미리 할당해 둔 PT(as_get_pde_page가 준 것).
 * @return: PTE의 주소, 실패하면 NULL.
 *
 * 인자로 PT를 받는 구조가 특이하다: 할당은 as_get_pde_page가 미리
 * (락을 풀고) 해 두고, 이 함수는 그것을 DMA 매핑해 설치하기만 한다.
 * 그렇게 나눈 이유는 할당이 잠들 수 있어 락 밖에서 해야 하기 때문이다.
 *
 * 이미 PT가 있으면(다른 CPU가 먼저 설치했거나 원래 있었으면) 인자로
 * 받은 pt는 쓰지 않는다 — as_get_pde_page가 그 경우 이미 설치된 것을
 * 돌려주므로 여기서는 자연히 같은 포인터가 된다.
 *
 * 실패 시 iommu_free_pages(pt)로 인자를 해제하는 점에 주목: 호출자가
 * 아니라 이 함수가 소유권을 넘겨받은 것으로 취급한다.
 *
 * 실행 컨텍스트: 매핑 경로. as->lock 보유 상태.
 *
 * 호출 체인:
 *   __tegra_smmu_map() → [as_get_pte] → dma_map_single(),
 *   tegra_smmu_set_pde()
 */
static u32 *as_get_pte(struct tegra_smmu_as *as, dma_addr_t iova,
		       dma_addr_t *dmap, struct tegra_pt *pt)
{
	/* [한국어] 1단계 인덱스. */
	unsigned int pde = iova_pd_index(iova);
	/* [한국어] DMA 매핑과 무효화에 필요한 SMMU. */
	struct tegra_smmu *smmu = as->smmu;

	/* [한국어] 아직 이 영역에 PT가 없다면 인자로 받은 것을 설치한다. */
	if (!as->pts[pde]) {
		/* [한국어] PT의 DMA 주소. */
		dma_addr_t dma;

		/* [한국어] PT를 DMA로 매핑해 하드웨어가 읽을 수 있게 한다. */
		dma = dma_map_single(smmu->dev, pt, SMMU_SIZE_PT,
				     DMA_TO_DEVICE);
		if (dma_mapping_error(smmu->dev, dma)) {
			/* [한국어] 매핑 실패 — 인자로 받은 PT의 소유권이
			 * 이 함수에 있으므로 여기서 반납한다. */
			iommu_free_pages(pt);
			return NULL;	/* [한국어] DMA 매핑에 실패해 이 PT를 쓸 수 없다. */
		}

		/* [한국어] PDE에 담을 수 있는 주소인지 확인한다. */
		if (!smmu_dma_addr_valid(smmu, dma)) {
			/* [한국어] 담을 수 없으면 매핑을 풀고 페이지도 반납한다. */
			dma_unmap_single(smmu->dev, dma, SMMU_SIZE_PT,
					 DMA_TO_DEVICE);
			iommu_free_pages(pt);	/* [한국어] PDE에 담을 수 없는 주소라 이 PT를 버린다. */
			return NULL;	/* [한국어] 설치할 수 없음을 NULL로 알린다. */
		}

		/* [한국어] 소프트웨어 배열에 PT의 가상 주소를 기록한다.
		 * 이것이 있어야 나중에 그 PT를 찾을 수 있다. */
		as->pts[pde] = pt;

		/* [한국어] PDE를 써서 하드웨어가 이 PT를 보게 만든다.
		 * NEXT 비트로 "다음 레벨 테이블"임을 표시한다. */
		tegra_smmu_set_pde(as, iova, SMMU_MK_PDE(dma, SMMU_PDE_ATTR |
							      SMMU_PDE_NEXT));

		/* [한국어] 호출자가 PTE를 고칠 때 쓸 DMA 주소를 돌려준다. */
		*dmap = dma;
	} else {
		/* [한국어] 이미 PT가 있으면 PDE에서 DMA 주소만 뽑아 돌려준다. */
		*dmap = smmu_pde_to_dma(smmu, as->pd->val[pde]);
	}

	/* [한국어] 그 PT 안에서 해당 PTE의 주소를 반환한다. */
	return tegra_smmu_pte_offset(as->pts[pde], iova);
}

/*
 * [한국어]
 * tegra_smmu_pte_get_use - PT의 사용 카운트를 하나 늘린다
 *
 * @as: 대상 주소 공간.
 * @iova: 매핑되는 주소.
 * @return: 없음.
 *
 * 새 PTE를 만들 때마다 그 PT의 카운트를 올린다. 이 카운트가 0이
 * 되는 순간 PT를 회수할 수 있게 된다.
 *
 * 실행 컨텍스트: 매핑 경로. as->lock 보유 상태라 원자적 연산이 필요 없다.
 *
 * 호출 체인:
 *   __tegra_smmu_map() → [tegra_smmu_pte_get_use]
 */
static void tegra_smmu_pte_get_use(struct tegra_smmu_as *as, unsigned long iova)
{
	/* [한국어] 이 IOVA가 속한 PT의 인덱스. */
	unsigned int pd_index = iova_pd_index(iova);

	/* [한국어] 그 PT에서 사용 중인 PTE 개수를 하나 늘린다. */
	as->count[pd_index]++;
}

/*
 * [한국어]
 * tegra_smmu_pte_put_use - PT의 사용 카운트를 줄이고, 0이면 PT를 회수한다
 *
 * @as: 대상 주소 공간.
 * @iova: 해제되는 주소.
 * @return: 없음.
 *
 * 이 드라이버의 눈에 띄는 최적화다. 대부분의 IOMMU 드라이버는 빈
 * 페이지 테이블을 그대로 두지만, 여기서는 마지막 PTE가 사라지면
 * 그 PT(4KB)를 시스템에 돌려준다. 4GB 주소 공간에 PT가 1024개까지
 * 생길 수 있어(최대 4MB) 회수할 가치가 있다는 판단이다.
 *
 * 회수 순서: PDE를 먼저 0으로 지워 하드웨어가 그 PT를 보지 않게 만든
 * 뒤에 DMA 매핑을 풀고 메모리를 반납한다. 순서가 뒤바뀌면 하드웨어가
 * 해제된 메모리를 걸을 수 있다.
 *
 * 실행 컨텍스트: 해제 경로. as->lock 보유 상태.
 *
 * 호출 체인:
 *   __tegra_smmu_unmap() → [tegra_smmu_pte_put_use]
 *   → tegra_smmu_set_pde(), iommu_free_pages()
 */
static void tegra_smmu_pte_put_use(struct tegra_smmu_as *as, unsigned long iova)
{
	/* [한국어] 이 IOVA가 속한 PT의 인덱스. */
	unsigned int pde = iova_pd_index(iova);
	/* [한국어] 그 PT의 가상 주소. */
	struct tegra_pt *pt = as->pts[pde];

	/*
	 * When no entries in this page table are used anymore, return the
	 * memory page to the system.
	 */
	/* [한국어] 카운트를 줄이고 0이 되면(= 이 PT의 마지막 매핑이었으면)
	 * 테이블을 통째로 회수한다. */
	if (--as->count[pde] == 0) {
		/* [한국어] 무효화와 DMA 해제에 필요한 SMMU. */
		struct tegra_smmu *smmu = as->smmu;
		/* [한국어] PDE를 지우기 전에 PT의 DMA 주소를 미리 뽑아 둔다. */
		dma_addr_t pte_dma = smmu_pde_to_dma(smmu, as->pd->val[pde]);

		/* [한국어] PDE를 0으로 지워 하드웨어가 이 PT를 보지 않게 한다.
		 * 이 호출 안에서 캐시 동기화와 TLB 무효화까지 이뤄진다. */
		tegra_smmu_set_pde(as, iova, 0);

		/* [한국어] 하드웨어가 더 이상 참조하지 않으므로 DMA 매핑을 푼다. */
		dma_unmap_single(smmu->dev, pte_dma, SMMU_SIZE_PT,
				 DMA_TO_DEVICE);
		/* [한국어] PT 메모리를 시스템에 반납한다. */
		iommu_free_pages(pt);
		/* [한국어] 소프트웨어 배열도 비워 다음 매핑이 새 PT를 만들게 한다. */
		as->pts[pde] = NULL;
	}
}

/*
 * [한국어]
 * tegra_smmu_set_pte - PTE 하나를 쓰고 세 단계 무효화를 수행한다
 *
 * @as: 대상 주소 공간.
 * @iova: 그 PTE가 담당하는 주소.
 * @pte: 고칠 PTE의 주소.
 * @pte_dma: 그 PT의 DMA 주소.
 * @val: 쓸 값(0이면 매핑 제거).
 * @return: 없음.
 *
 * set_pde와 같은 절차를 PTE에 대해 수행한다. 다른 점은 TLB 무효화
 * 단위가 섹션(4MB)이 아니라 그룹(16KB)이라는 것뿐이다.
 *
 * 오프셋 계산이 흥미롭다: SMMU_OFFSET_IN_PAGE(pte)로 **포인터 값**에서
 * 페이지 내 오프셋을 뽑는다. PT가 페이지 하나에 정렬되어 있으므로,
 * 그 포인터의 하위 12비트가 곧 테이블 안에서의 바이트 오프셋이 된다.
 *
 * 실행 컨텍스트: 매핑/해제 경로. as->lock 보유 상태.
 *
 * 호출 체인:
 *   __tegra_smmu_map() / __tegra_smmu_unmap() → [tegra_smmu_set_pte]
 */
static void tegra_smmu_set_pte(struct tegra_smmu_as *as, unsigned long iova,
			       u32 *pte, dma_addr_t pte_dma, u32 val)
{
	/* [한국어] 무효화 명령을 보낼 SMMU. */
	struct tegra_smmu *smmu = as->smmu;
	/* [한국어] PT 안에서 이 엔트리의 바이트 오프셋. PT가 페이지 정렬이라
	 * 포인터의 하위 12비트가 곧 오프셋이다. */
	unsigned long offset = SMMU_OFFSET_IN_PAGE(pte);

	/* [한국어] 1단계: 메모리에 값을 쓴다. */
	*pte = val;

	/* [한국어] 2단계: CPU 캐시를 메모리로 밀어낸다. 4바이트만
	 * 동기화하면 충분하다. */
	dma_sync_single_range_for_device(smmu->dev, pte_dma, offset,
					 4, DMA_TO_DEVICE);
	/* [한국어] 3단계: PTC에서 그 라인을 비운다. */
	smmu_flush_ptc(smmu, pte_dma, offset);
	/* [한국어] 4단계: 그 PTE가 속한 16KB 그룹의 TLB를 비운다. */
	smmu_flush_tlb_group(smmu, as->id, iova);
	/* [한국어] 5단계: 명령들이 도달했음을 보장한다. */
	smmu_flush(smmu);
}

/*
 * [한국어]
 * as_get_pde_page - PT를 확보한다(필요하면 락을 풀고 할당한다)
 *
 * @as: 대상 주소 공간.
 * @iova: 매핑할 IOVA.
 * @gfp: 할당 플래그.
 * @flags: as->lock의 irqsave 플래그 포인터 — 락을 풀고 다시 잡을 때 쓴다.
 * @return: PT의 가상 주소, 할당 실패하면 NULL.
 *
 * 이 함수가 이 드라이버에서 가장 정교한 부분이다. 문제 상황은 이렇다:
 * map은 as->lock 스핀락 안에서 실행되는데, 그 안에서 페이지를 할당하려면
 * GFP_ATOMIC을 써야 한다. 그런데 원자적 메모리 풀은 작아서 자주
 * 고갈된다.
 *
 * 해법이 원본 주석에 있다: gfp가 blocking을 허용하면 **락을 잠시 풀고**
 * 잠들 수 있는 할당을 시도한 뒤 다시 잡는다. 그러면 원자적 풀을
 * 소진하지 않는다.
 *
 * 그 대가로 경쟁이 생긴다: 락이 풀린 사이에 다른 CPU가 같은 PDE에
 * PT를 설치할 수 있다. 그래서 락을 다시 잡은 뒤 as->pts[pde]를
 * 재확인하고, 이미 설치되었으면 자기가 만든 것을 버린다.
 *
 * 두 번째 주석이 짚는 미묘한 점: 그 경우 **할당 실패도 치명적이지
 * 않다** — 이미 다른 CPU가 설치한 것을 쓰면 되기 때문이다. 그래서
 * pt가 NULL이어도 as->pts[pde]가 있으면 그것을 반환한다.
 *
 * 실행 컨텍스트: 매핑 경로. as->lock을 잡은 채 진입해 잡은 채 나간다.
 *
 * 호출 체인:
 *   __tegra_smmu_map() → [as_get_pde_page] → iommu_alloc_pages_sz()
 */
static struct tegra_pt *as_get_pde_page(struct tegra_smmu_as *as,
					unsigned long iova, gfp_t gfp,
					unsigned long *flags)
{
	/* [한국어] 1단계 인덱스. */
	unsigned int pde = iova_pd_index(iova);
	/* [한국어] 이미 있는 PT(있으면 그대로 쓴다). */
	struct tegra_pt *pt = as->pts[pde];

	/* at first check whether allocation needs to be done at all */
	/* [한국어] 이미 PT가 있으면 할당할 이유가 없다 — 가장 흔한 경로이자
	 * 락을 풀지 않는 빠른 경로다. */
	if (pt)
		return pt;

	/*
	 * In order to prevent exhaustion of the atomic memory pool, we
	 * allocate page in a sleeping context if GFP flags permit. Hence
	 * spinlock needs to be unlocked and re-locked after allocation.
	 */
	/* [한국어] 잠들 수 있는 할당이 허용된다면 락을 푼다. 원본 주석대로
	 * 원자적 메모리 풀의 고갈을 막기 위한 조치다. */
	if (gfpflags_allow_blocking(gfp))
		spin_unlock_irqrestore(&as->lock, *flags);

	/* [한국어] PT(4KB)를 할당한다. __GFP_DMA로 낮은 주소에서 받아야
	 * PDE에 주소가 담긴다. 락이 풀린 상태일 수도, 잡힌 상태일 수도 있다. */
	pt = iommu_alloc_pages_sz(gfp | __GFP_DMA, SMMU_SIZE_PT);

	/* [한국어] 락을 풀었다면 다시 잡는다. 호출자가 여전히 락을 쥔
	 * 상태로 진행할 수 있게 해야 하기 때문이다. */
	if (gfpflags_allow_blocking(gfp))
		spin_lock_irqsave(&as->lock, *flags);

	/*
	 * In a case of blocking allocation, a concurrent mapping may win
	 * the PDE allocation. In this case the allocated page isn't needed
	 * if allocation succeeded and the allocation failure isn't fatal.
	 */
	/* [한국어] 락이 풀린 사이에 다른 CPU가 이 PDE에 PT를 설치했을 수 있다. */
	if (as->pts[pde]) {
		/* [한국어] 내가 만든 것이 있으면 버린다 — 경쟁에서 진 것이다. */
		if (pt)
			iommu_free_pages(pt);

		/* [한국어] 이긴 쪽의 PT를 쓴다. 내 할당이 실패했더라도
		 * 여기서 유효한 PT를 얻으므로 치명적이지 않다 —
		 * 원본 주석이 짚는 바로 그 지점이다. */
		pt = as->pts[pde];
	}

	/* [한국어] 확보한 PT(또는 할당 실패 시 NULL). */
	return pt;
}

/*
 * [한국어]
 * __tegra_smmu_map - 락을 쥔 채 실제 매핑을 수행한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 IOVA.
 * @paddr: 물리 주소.
 * @size: 크기(항상 4KB).
 * @prot: 보호 플래그.
 * @gfp: PT 할당 플래그.
 * @flags: as->lock의 irqsave 플래그 — as_get_pde_page가 락을 풀었다
 *         잡을 때 필요해 포인터로 전달한다.
 * @return: 0 성공, -ENOMEM(PT 확보 실패).
 *
 * 동작 순서:
 *  1) PT를 확보한다(없으면 만든다 — 이때 락이 잠시 풀릴 수 있다).
 *  2) 그 PT를 설치하고 PTE의 주소를 얻는다.
 *  3) 새 매핑이면 PT의 사용 카운트를 올린다. 덮어쓰기면 올리지 않는다 —
 *     그래야 카운트가 실제 사용 중인 PTE 개수와 일치한다.
 *  4) 보호 플래그를 PTE 비트로 바꾸고 엔트리를 쓴다.
 *
 * NONSECURE를 항상 세우는 이유: 리눅스가 비보안 세계에서 돌기 때문이다.
 * 읽기/쓰기는 요청에 따라 갈리며, 둘 다 없으면 접근이 모두 막힌
 * 유효 엔트리가 된다.
 *
 * 실행 컨텍스트: 매핑 경로. as->lock을 쥔 채 진입한다.
 *
 * 호출 체인:
 *   tegra_smmu_map() → [__tegra_smmu_map] → as_get_pde_page(),
 *   as_get_pte(), tegra_smmu_set_pte()
 */
static int
__tegra_smmu_map(struct iommu_domain *domain, unsigned long iova,
		 phys_addr_t paddr, size_t size, int prot, gfp_t gfp,
		 unsigned long *flags)
{
	/* [한국어] 이 드라이버의 주소 공간으로 복원한다. */
	struct tegra_smmu_as *as = to_smmu_as(domain);
	/* [한국어] PT의 DMA 주소(set_pte에 넘긴다). */
	dma_addr_t pte_dma;
	/* [한국어] 확보한 2단계 테이블. */
	struct tegra_pt *pt;
	/* [한국어] PTE에 넣을 권한 비트들. */
	u32 pte_attrs;
	/* [한국어] 고칠 PTE의 주소. */
	u32 *pte;

	/* [한국어] PT를 확보한다. 필요하면 락을 잠시 풀고 할당하므로,
	 * 이 호출 뒤에는 상태가 바뀌었을 수 있다. */
	pt = as_get_pde_page(as, iova, gfp, flags);
	if (!pt)	/* [한국어] PT를 확보하지 못했으므로 매핑할 수 없다. */
		return -ENOMEM;

	/* [한국어] 그 PT를 PDE에 설치(필요하면)하고 PTE의 주소를 얻는다.
	 * 실패 시 as_get_pte가 pt를 이미 해제했다. */
	pte = as_get_pte(as, iova, &pte_dma, pt);
	if (!pte)	/* [한국어] PT 설치에 실패했다 — 그 안에서 이미 페이지가 반납되었다. */
		return -ENOMEM;

	/* If we aren't overwriting a pre-existing entry, increment use */
	/* [한국어] 빈 자리에 새로 쓰는 경우에만 카운트를 올린다.
	 * 덮어쓰기에서도 올리면 카운트가 실제보다 커져 PT가 영영
	 * 회수되지 않는다. */
	if (*pte == 0)
		tegra_smmu_pte_get_use(as, iova);

	/* [한국어] 비보안 비트를 기본으로 둔다 — 리눅스는 비보안 세계에서 돈다. */
	pte_attrs = SMMU_PTE_NONSECURE;

	/* [한국어] 읽기가 요청되었으면 읽기 허용 비트를 세운다. */
	if (prot & IOMMU_READ)
		pte_attrs |= SMMU_PTE_READABLE;

	/* [한국어] 쓰기가 요청되었으면 쓰기 허용 비트를 세운다. */
	if (prot & IOMMU_WRITE)
		pte_attrs |= SMMU_PTE_WRITABLE;

	/* [한국어] 물리 페이지 번호와 권한을 합쳐 PTE를 쓴다.
	 * 이 호출 안에서 캐시 동기화와 PTC/TLB 무효화까지 이뤄진다. */
	tegra_smmu_set_pte(as, iova, pte, pte_dma,
			   SMMU_PHYS_PFN(paddr) | pte_attrs);

	return 0;	/* [한국어] PTE 기록과 무효화까지 끝났다. */
}

/*
 * [한국어]
 * __tegra_smmu_unmap - 락을 쥔 채 실제 해제를 수행한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 IOVA.
 * @size: 크기(항상 4KB).
 * @gather: TLB 무효화 수집 구조체 — 이 드라이버는 즉시 무효화하므로
 *          쓰지 않는다.
 * @return: 해제한 바이트 수, 매핑이 없으면 0.
 *
 * PTE를 0으로 지우고 카운트를 줄인다. 카운트가 0이 되면
 * pte_put_use가 그 PT를 통째로 회수한다.
 *
 * 실행 컨텍스트: 해제 경로. as->lock을 쥔 채 진입한다.
 *
 * 호출 체인:
 *   tegra_smmu_unmap() → [__tegra_smmu_unmap] → tegra_smmu_pte_lookup(),
 *   tegra_smmu_set_pte(), tegra_smmu_pte_put_use()
 */
static size_t
__tegra_smmu_unmap(struct iommu_domain *domain, unsigned long iova,
		   size_t size, struct iommu_iotlb_gather *gather)
{
	/* [한국어] 이 드라이버의 주소 공간으로 복원한다. */
	struct tegra_smmu_as *as = to_smmu_as(domain);
	/* [한국어] PT의 DMA 주소. */
	dma_addr_t pte_dma;
	/* [한국어] 지울 PTE의 주소. */
	u32 *pte;

	/* [한국어] 해당 PTE를 찾는다. PT가 없거나 엔트리가 비어 있으면
	 * 해제할 것이 없다. */
	pte = tegra_smmu_pte_lookup(as, iova, &pte_dma);
	if (!pte || !*pte)	/* [한국어] PT가 없거나 엔트리가 비어 있으면 해제할 것이 없다. */
		return 0;

	/* [한국어] 엔트리를 0으로 지운다. 캐시 동기화와 무효화가
	 * 이 호출 안에서 이뤄진다. */
	tegra_smmu_set_pte(as, iova, pte, pte_dma, 0);
	/* [한국어] 카운트를 줄인다. 0이 되면 이 안에서 PT가 회수된다. */
	tegra_smmu_pte_put_use(as, iova);

	/* [한국어] 요청한 크기를 그대로 해제했다. */
	return size;
}

/*
 * [한국어]
 * tegra_smmu_map - 매핑 생성 콜백(락 관리 담당)
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 IOVA.
 * @paddr: 물리 주소.
 * @size: 페이지 크기(4KB).
 * @count: 페이지 개수 — 이 구현은 한 번에 하나만 처리한다.
 * @prot: 보호 플래그.
 * @gfp: 할당 플래그.
 * @mapped: 출력 인자.
 * @return: 0 성공, 음수 errno.
 *
 * 락을 잡고 __tegra_smmu_map에 위임한 뒤 푼다. flags를 포인터로
 * 넘기는 이유는 그 안에서 as_get_pde_page가 락을 잠시 풀 수 있어,
 * 플래그 값이 갱신되어야 하기 때문이다.
 *
 * count를 무시하고 한 페이지만 처리하므로 코어가 나머지를 다시 요청한다.
 *
 * 실행 컨텍스트: 매핑 경로. as->lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   iommu_map() → domain_ops->map_pages → [tegra_smmu_map]
 *   → __tegra_smmu_map()
 */
static int tegra_smmu_map(struct iommu_domain *domain, unsigned long iova,
			  phys_addr_t paddr, size_t size, size_t count,
			  int prot, gfp_t gfp, size_t *mapped)
{
	/* [한국어] 이 드라이버의 주소 공간으로 복원한다. */
	struct tegra_smmu_as *as = to_smmu_as(domain);
	/* [한국어] irqsave용 플래그. 포인터로 넘겨 안쪽에서 갱신될 수 있게 한다. */
	unsigned long flags;
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] 페이지 테이블 접근을 직렬화한다. */
	spin_lock_irqsave(&as->lock, flags);
	/* [한국어] 실제 작업을 위임한다. flags를 포인터로 넘기는 이유는
	 * 그 안에서 락을 잠시 풀 수 있기 때문이다. */
	ret = __tegra_smmu_map(domain, iova, paddr, size, prot, gfp, &flags);
	spin_unlock_irqrestore(&as->lock, flags);

	/* [한국어] 성공했다면 한 페이지를 매핑했음을 코어에 알린다. */
	if (!ret)
		*mapped = size;

	return ret;
}

/*
 * [한국어]
 * tegra_smmu_unmap - 매핑 해제 콜백(락 관리 담당)
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 IOVA.
 * @size: 페이지 크기.
 * @count: 페이지 개수 — 무시된다.
 * @gather: TLB 무효화 수집 구조체 — 쓰지 않는다.
 * @return: 해제한 바이트 수.
 *
 * map과 대칭으로 락만 관리하고 위임한다. unmap 쪽은 락을 푸는
 * 경우가 없어 flags를 포인터로 넘기지 않는다.
 *
 * 실행 컨텍스트: 해제 경로. as->lock을 irqsave로 잡는다.
 *
 * 호출 체인:
 *   iommu_unmap() → domain_ops->unmap_pages → [tegra_smmu_unmap]
 *   → __tegra_smmu_unmap()
 */
static size_t tegra_smmu_unmap(struct iommu_domain *domain, unsigned long iova,
			       size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	/* [한국어] 이 드라이버의 주소 공간으로 복원한다. */
	struct tegra_smmu_as *as = to_smmu_as(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] 페이지 테이블 접근을 직렬화한다. */
	spin_lock_irqsave(&as->lock, flags);
	/* [한국어] 실제 해제를 위임하고 결과를 size에 덮어쓴다. */
	size = __tegra_smmu_unmap(domain, iova, size, gather);
	spin_unlock_irqrestore(&as->lock, flags);

	/* [한국어] 해제한 바이트 수(또는 0). */
	return size;
}

/*
 * [한국어]
 * tegra_smmu_iova_to_phys - 소프트웨어 워크로 IOVA를 물리 주소로 바꾼다
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑이 없으면 0.
 *
 * 락을 잡지 않는 점에 주목: map/unmap과 달리 읽기만 하므로 크래시
 * 위험은 없지만, 동시에 매핑이 바뀌면 결과가 낡을 수 있다.
 *
 * 실행 컨텍스트: 조회 경로.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops->iova_to_phys
 *   → [tegra_smmu_iova_to_phys]
 */
static phys_addr_t tegra_smmu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	/* [한국어] 이 드라이버의 주소 공간으로 복원한다. */
	struct tegra_smmu_as *as = to_smmu_as(domain);
	/* [한국어] PTE에서 뽑은 물리 페이지 번호. */
	unsigned long pfn;
	/* [한국어] PT의 DMA 주소(여기서는 쓰지 않지만 lookup이 요구한다). */
	dma_addr_t pte_dma;
	/* [한국어] 찾은 PTE의 주소. */
	u32 *pte;

	/* [한국어] 해당 PTE를 찾는다. PT가 없거나 엔트리가 비어 있으면
	 * 매핑이 없다는 뜻이다. */
	pte = tegra_smmu_pte_lookup(as, iova, &pte_dma);
	if (!pte || !*pte)	/* [한국어] PT가 없거나 엔트리가 비어 있으면 매핑이 없다는 뜻이다. */
		return 0;

	/* [한국어] 권한 비트를 걸러 내고 페이지 번호만 남긴다. */
	pfn = *pte & as->smmu->pfn_mask;

	/* [한국어] 페이지 번호를 주소로 되돌리고 페이지 내 오프셋을 더한다. */
	return SMMU_PFN_PHYS(pfn) + SMMU_OFFSET_IN_PAGE(iova);
}

/*
 * [한국어]
 * tegra_smmu_find - 디바이스 트리 노드에서 SMMU 인스턴스를 찾는다
 *
 * @np: iommus 프로퍼티가 가리키는 노드(= MC 노드).
 * @return: SMMU 인스턴스, 찾지 못하면 NULL.
 *
 * SMMU가 MC 블록의 일부라, 디바이스 트리에서 iommus가 가리키는 것은
 * MC 노드다. 그 플랫폼 디바이스의 drvdata가 struct tegra_mc이고,
 * 그 안의 smmu 필드가 우리가 찾는 인스턴스다.
 *
 * mc->smmu가 NULL일 수 있는 이유: MC는 probe되었지만 SMMU 초기화가
 * 아직 끝나지 않았거나, 그 SoC에 SMMU가 없는 구성일 수 있다.
 *
 * 실행 컨텍스트: probe_device 경로.
 *
 * 호출 체인:
 *   tegra_smmu_probe_device() → [tegra_smmu_find]
 */
static struct tegra_smmu *tegra_smmu_find(struct device_node *np)
{
	/* [한국어] 그 노드의 플랫폼 디바이스. */
	struct platform_device *pdev;
	/* [한국어] 메모리 컨트롤러 인스턴스. */
	struct tegra_mc *mc;

	/* [한국어] 노드에 대응하는 플랫폼 디바이스를 찾는다(참조 +1). */
	pdev = of_find_device_by_node(np);
	if (!pdev)	/* [한국어] 그 노드에 대응하는 플랫폼 디바이스가 아직 없다. */
		return NULL;

	/* [한국어] MC 드라이버가 심어 둔 인스턴스를 꺼낸다. */
	mc = platform_get_drvdata(pdev);
	/* [한국어] 값을 꺼냈으니 참조를 내린다. mc 포인터는 MC가 살아 있는
	 * 동안 유효하다. */
	put_device(&pdev->dev);
	if (!mc)	/* [한국어] MC가 아직 초기화되지 않았거나 이 SoC에는 SMMU가 없다. */
		return NULL;

	/* [한국어] MC 안의 SMMU 인스턴스를 반환한다. probe가 mc->smmu를
	 * 채워 두었기에 가능한 접근이다. */
	return mc->smmu;
}

/*
 * [한국어]
 * tegra_smmu_configure - 디바이스의 fwspec을 초기화하고 swgroup을 등록한다
 *
 * @smmu: 담당 SMMU.
 * @dev: 클라이언트 디바이스.
 * @args: 파싱된 iommus 항목.
 * @return: 0 성공, 음수 errno.
 *
 * 왜 of_xlate를 직접 부르는가: 이 드라이버는 probe_device에서 iommus를
 * 손수 파싱하는 구조라, 코어가 대신 해 주는 fwspec 초기화와 of_xlate
 * 호출을 여기서 직접 수행한다.
 *
 * fwnode로 smmu->dev(= MC 디바이스)를 넘기는 점에 주목: 이 디바이스가
 * 어느 IOMMU에 속하는지를 코어에 알리는 값이다.
 *
 * 실행 컨텍스트: probe_device 경로.
 *
 * 호출 체인:
 *   tegra_smmu_probe_device() → [tegra_smmu_configure]
 *   → iommu_fwspec_init(), ops->of_xlate()
 */
static int tegra_smmu_configure(struct tegra_smmu *smmu, struct device *dev,
				const struct of_phandle_args *args)
{
	/* [한국어] 이 SMMU의 연산 테이블 — of_xlate를 부르기 위해 꺼낸다. */
	const struct iommu_ops *ops = smmu->iommu.ops;
	/* [한국어] 각 단계의 결과. */
	int err;

	/* [한국어] 이 디바이스의 IOMMU 펌웨어 스펙을 초기화한다.
	 * MC 디바이스의 fwnode를 넘겨 "이 IOMMU 소속"임을 표시한다.
	 * 이미 초기화되어 있으면 그대로 성공한다. */
	err = iommu_fwspec_init(dev, dev_fwnode(smmu->dev));
	if (err < 0) {	/* [한국어] 펌웨어 스펙 초기화에 실패했다. */
		dev_err(dev, "failed to initialize fwspec: %d\n", err);	/* [한국어] 어느 단계에서 실패했는지 남긴다. */
		return err;	/* [한국어] 오류를 그대로 상위에 전달한다. */
	}

	/* [한국어] swgroup 번호를 fwspec에 등록한다. 코어가 부르는 대신
	 * 여기서 직접 호출하는 구조다. */
	err = ops->of_xlate(dev, args);
	if (err < 0) {	/* [한국어] swgroup 번호 등록에 실패했다. */
		dev_err(dev, "failed to parse SW group ID: %d\n", err);	/* [한국어] 파싱 실패를 남긴다. */
		return err;	/* [한국어] 오류를 그대로 전달한다. */
	}

	return 0;	/* [한국어] fwspec 준비가 모두 끝났다. */
}

/*
 * [한국어]
 * tegra_smmu_probe_device - 디바이스의 iommus를 파싱하고 담당 SMMU를 알린다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 SMMU의 핸들, 없으면 ERR_PTR(-ENODEV).
 *
 * 다른 드라이버들과 달리 여기서 iommus 프로퍼티를 **직접 순회**한다.
 * 항목마다 SMMU를 찾아 configure를 부르고, 그 안에서 of_xlate가
 * swgroup을 fwspec에 쌓는다.
 *
 * 왜 코어의 of_xlate 경로를 쓰지 않는가: SMMU가 MC의 일부라 iommus가
 * 가리키는 노드에서 한 단계 더 들어가야 하고(tegra_smmu_find),
 * 그 과정에서 SMMU가 아직 준비되지 않았을 수 있어 건너뛰어야 하기
 * 때문으로 보인다.
 *
 * 마지막에 priv를 다시 읽어 판정하는 점에 주목: 루프 안에서 of_xlate가
 * priv를 채웠는지가 곧 "이 디바이스가 SMMU를 쓰는가"의 답이다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   iommu_probe_device() → iommu_ops->probe_device
 *   → [tegra_smmu_probe_device] → tegra_smmu_find(), tegra_smmu_configure()
 */
static struct iommu_device *tegra_smmu_probe_device(struct device *dev)
{
	/* [한국어] 이 디바이스의 트리 노드. */
	struct device_node *np = dev->of_node;
	/* [한국어] 찾은 SMMU 인스턴스. */
	struct tegra_smmu *smmu = NULL;
	/* [한국어] 파싱된 iommus 항목. */
	struct of_phandle_args args;
	/* [한국어] 항목 순회 인덱스. */
	unsigned int index = 0;
	/* [한국어] configure의 결과. */
	int err;

	/* [한국어] iommus 프로퍼티의 항목을 하나씩 파싱한다. */
	while (of_parse_phandle_with_args(np, "iommus", "#iommu-cells", index,
					  &args) == 0) {
		/* [한국어] 그 노드(MC)에서 SMMU 인스턴스를 찾는다. */
		smmu = tegra_smmu_find(args.np);
		/* [한국어] 찾았을 때만 설정한다 — 아직 SMMU가 준비되지
		 * 않았다면 조용히 건너뛴다. */
		if (smmu) {
			err = tegra_smmu_configure(smmu, dev, &args);	/* [한국어] fwspec을 초기화하고 이 항목의 swgroup을 등록한다. */

			if (err < 0) {
				/* [한국어] 실패해도 노드 참조는 반드시 내린다. */
				of_node_put(args.np);
				return ERR_PTR(err);	/* [한국어] 설정 실패를 오류 포인터로 감싸 반환한다. */
			}
		}

		/* [한국어] 파싱이 올린 노드 참조를 내린다. */
		of_node_put(args.np);
		index++;	/* [한국어] 다음 iommus 항목으로 넘어간다. */
	}

	/* [한국어] of_xlate가 priv를 채웠는지로 최종 판정한다.
	 * 채워지지 않았다면 이 디바이스는 SMMU를 쓰지 않는다. */
	smmu = dev_iommu_priv_get(dev);
	if (!smmu)	/* [한국어] of_xlate가 priv를 채우지 못했다 — 이 디바이스는 SMMU를 쓰지 않는다. */
		return ERR_PTR(-ENODEV);

	/* [한국어] 담당 SMMU의 코어 핸들을 반환한다. */
	return &smmu->iommu;
}

/*
 * [한국어]
 * tegra_smmu_find_group - swgroup이 속한 그룹 정의를 찾는다
 *
 * @smmu: 대상 인스턴스.
 * @swgroup: 찾을 swgroup 번호.
 * @return: 그 swgroup을 포함하는 group_soc, 없으면 NULL.
 *
 * SoC 데이터의 그룹 정의를 이중 루프로 탐색한다. 각 group_soc가
 * 여러 swgroup을 묶고 있어, 그 배열까지 훑어야 한다.
 *
 * 왜 이런 묶음이 필요한가: 하드웨어적으로 서로 격리할 수 없는
 * 클라이언트들(예: 디스플레이 컨트롤러의 여러 윈도)이 있어,
 * 그것들을 한 IOMMU 그룹으로 묶어야 하기 때문이다.
 *
 * 실행 컨텍스트: device_group 경로.
 *
 * 호출 체인:
 *   tegra_smmu_device_group() → [tegra_smmu_find_group]
 */
static const struct tegra_smmu_group_soc *
tegra_smmu_find_group(struct tegra_smmu *smmu, unsigned int swgroup)
{
	/* [한국어] 그룹과 그 안의 swgroup 순회 인덱스. */
	unsigned int i, j;

	/* [한국어] 모든 그룹 정의를 훑는다. */
	for (i = 0; i < smmu->soc->num_groups; i++)
		/* [한국어] 그 그룹이 묶는 swgroup들을 훑는다. */
		for (j = 0; j < smmu->soc->groups[i].num_swgroups; j++)
			/* [한국어] 찾는 swgroup을 포함하는 그룹이면 반환한다. */
			if (smmu->soc->groups[i].swgroups[j] == swgroup)
				return &smmu->soc->groups[i];

	/* [한국어] 어느 그룹에도 속하지 않는 swgroup이다 — 그 경우
	 * 자기만의 그룹을 갖게 된다. */
	return NULL;
}

/*
 * [한국어]
 * tegra_smmu_group_release - IOMMU 그룹이 해제될 때 목록에서 뺀다
 *
 * @iommu_data: iommu_group_set_iommudata로 붙여 둔 tegra_smmu_group.
 * @return: 없음.
 *
 * 코어가 그룹의 마지막 참조를 놓을 때 부르는 콜백이다. 여기서
 * smmu->groups 목록에서 빼야, 나중에 같은 swgroup의 디바이스가
 * 해제된 그룹을 찾아 쓰는 사고를 막을 수 있다.
 *
 * 구조체 메모리 자체는 devm이 관리하므로 여기서 해제하지 않는다.
 *
 * 실행 컨텍스트: 그룹 해제 경로(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   iommu_group_release() → 등록된 release 콜백
 *   → [tegra_smmu_group_release]
 */
static void tegra_smmu_group_release(void *iommu_data)
{
	/* [한국어] 그룹에 붙여 둔 이 드라이버의 구조체. */
	struct tegra_smmu_group *group = iommu_data;
	/* [한국어] 목록 락을 얻기 위한 SMMU 포인터. */
	struct tegra_smmu *smmu = group->smmu;

	/* [한국어] 목록을 보호하며 이 그룹을 뺀다. */
	mutex_lock(&smmu->lock);
	list_del(&group->list);	/* [한국어] 해제되는 그룹을 목록에서 뺀다. */
	mutex_unlock(&smmu->lock);	/* [한국어] 목록 갱신이 끝났으니 락을 푼다. */
}

/*
 * [한국어]
 * tegra_smmu_device_group - 이 디바이스가 속할 IOMMU 그룹을 결정한다
 *
 * @dev: 그룹을 정할 디바이스.
 * @return: 그룹 포인터(참조가 증가된 상태), 실패하면 NULL.
 *
 * 그룹 결정 규칙이 두 겹이다:
 *  1) swgroup이 같은 디바이스는 같은 그룹.
 *  2) group_soc가 같은(= SoC가 "함께 격리해야 한다"고 정의한) swgroup들도
 *     같은 그룹.
 * 기존 그룹 목록을 순회하며 두 조건 중 하나라도 맞으면 그 그룹에 합류한다.
 *
 * 맞는 그룹이 없으면 새로 만든다. PCI면 표준 PCI 그룹 규칙을,
 * 아니면 디바이스마다 개별 그룹을 만드는 헬퍼를 쓴다.
 *
 * group_soc가 있으면 그 이름을 그룹 이름으로 지정하는데,
 * sysfs에서 그룹을 식별하기 쉽게 하려는 배려다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로. smmu->lock 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   iommu_group_get_for_dev() → iommu_ops->device_group
 *   → [tegra_smmu_device_group] → tegra_smmu_find_group()
 */
static struct iommu_group *tegra_smmu_device_group(struct device *dev)
{
	/* [한국어] 이 디바이스의 swgroup 번호들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 담당 SMMU. */
	struct tegra_smmu *smmu = dev_iommu_priv_get(dev);
	/* [한국어] 이 swgroup이 속한 그룹 정의(없을 수 있다). */
	const struct tegra_smmu_group_soc *soc;
	/* [한국어] 첫 swgroup 번호를 그룹 판별의 열쇠로 쓴다. */
	unsigned int swgroup = fwspec->ids[0];
	/* [한국어] 목록 순회 커서이자 새로 만들 그룹. */
	struct tegra_smmu_group *group;
	/* [한국어] 기존 그룹을 찾았을 때 반환할 참조. */
	struct iommu_group *grp;

	/* Find group_soc associating with swgroup */
	/* [한국어] 이 swgroup이 SoC 정의상 어느 그룹에 묶이는지 찾는다. */
	soc = tegra_smmu_find_group(smmu, swgroup);

	/* [한국어] 그룹 목록을 보호한다. */
	mutex_lock(&smmu->lock);

	/* Find existing iommu_group associating with swgroup or group_soc */
	/* [한국어] 이미 만들어진 그룹 중 같은 swgroup이거나 같은 group_soc에
	 * 속하는 것이 있으면 거기 합류한다. */
	list_for_each_entry(group, &smmu->groups, list)
		if ((group->swgroup == swgroup) || (soc && group->soc == soc)) {
			/* [한국어] 참조를 하나 올려 돌려준다 — 호출자가
			 * 나중에 put 할 것이다. */
			grp = iommu_group_ref_get(group->group);
			mutex_unlock(&smmu->lock);	/* [한국어] 참조를 얻었으니 락을 푼다. */
			return grp;	/* [한국어] 기존 그룹에 합류한다. */
		}

	/* [한국어] 맞는 그룹이 없으므로 새로 만든다. devm이라 SMMU가
	 * 사라질 때 함께 해제된다. */
	group = devm_kzalloc(smmu->dev, sizeof(*group), GFP_KERNEL);
	if (!group) {	/* [한국어] 그룹 구조체를 할당하지 못했다. */
		mutex_unlock(&smmu->lock);	/* [한국어] 락을 풀고 실패를 알린다. */
		return NULL;	/* [한국어] 그룹을 만들 수 없음을 NULL로 보고한다. */
	}

	/* [한국어] 목록 연결 고리를 초기화한다. */
	INIT_LIST_HEAD(&group->list);
	/* [한국어] 이 그룹을 만든 swgroup을 기억한다 — 나중에 같은
	 * swgroup의 디바이스가 이것을 찾는다. */
	group->swgroup = swgroup;
	/* [한국어] 소속 SMMU를 기록한다(release 콜백이 락을 얻는 통로). */
	group->smmu = smmu;
	/* [한국어] SoC 그룹 정의를 기록한다 — 다른 swgroup이 같은 정의에
	 * 속하는지 비교하는 기준이 된다. */
	group->soc = soc;

	/* [한국어] 실제 코어 그룹을 만든다. PCI는 ACS 등을 고려한 표준
	 * 규칙을, 그 외에는 디바이스마다 개별 그룹을 쓴다. */
	if (dev_is_pci(dev))
		group->group = pci_device_group(dev);
	else
		group->group = generic_device_group(dev);

	/* [한국어] 그룹 생성 실패 — 방금 할당한 구조체를 되돌린다. */
	if (IS_ERR(group->group)) {
		devm_kfree(smmu->dev, group);	/* [한국어] 코어 그룹 생성이 실패했으니 구조체를 되돌린다. */
		mutex_unlock(&smmu->lock);	/* [한국어] 락을 푼다. */
		return NULL;	/* [한국어] 그룹을 만들 수 없음을 알린다. */
	}

	/* [한국어] 그룹에 이 구조체를 붙이고 해제 콜백을 등록한다.
	 * 그래야 그룹이 사라질 때 목록에서 뺄 수 있다. */
	iommu_group_set_iommudata(group->group, group, tegra_smmu_group_release);
	/* [한국어] SoC 정의가 있으면 그 이름을 그룹 이름으로 쓴다 —
	 * sysfs에서 어떤 그룹인지 알아보기 쉬워진다. */
	if (soc)
		iommu_group_set_name(group->group, soc->name);
	/* [한국어] 목록에 추가해 이후 디바이스들이 찾을 수 있게 한다. */
	list_add_tail(&group->list, &smmu->groups);
	mutex_unlock(&smmu->lock);

	/* [한국어] 새로 만든 그룹을 반환한다. 생성 시 참조가 이미 하나
	 * 잡혀 있어 별도로 올리지 않는다. */
	return group->group;
}

/*
 * [한국어]
 * tegra_smmu_of_xlate - iommus 항목 하나를 해석한다
 *
 * @dev: 클라이언트 디바이스.
 * @args: 파싱된 항목. args->np가 MC 노드, args->args[0]이 swgroup 번호다.
 * @return: iommu_fwspec_add_ids()의 결과.
 *
 * 두 가지를 한다: SMMU 인스턴스를 priv에 심고, swgroup 번호를 fwspec에
 * 추가한다.
 *
 * 참조 카운트에 대한 원본 주석이 흥미롭다: put_device로 참조를 곧바로
 * 내리는데, 이후 tegra_smmu_ops의 여러 함수가 그 디바이스의 private
 * 데이터를 계속 쓴다. 그래도 안전한 이유는 SMMU의 부모 디바이스가
 * 곧 MC 자신이라, MC가 살아 있는 한 그 데이터도 유효하기 때문이다.
 * 즉 참조 카운트가 엄밀히는 필요 없다는 설명이다.
 *
 * 실행 컨텍스트: probe_device 경로(tegra_smmu_configure가 부른다).
 *
 * 호출 체인:
 *   tegra_smmu_configure() → ops->of_xlate → [tegra_smmu_of_xlate]
 */
static int tegra_smmu_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	/* [한국어] iommus가 가리키는 MC 플랫폼 디바이스(참조 +1). */
	struct platform_device *iommu_pdev = of_find_device_by_node(args->np);
	/* [한국어] 그 디바이스의 drvdata = 메모리 컨트롤러 인스턴스. */
	struct tegra_mc *mc = platform_get_drvdata(iommu_pdev);
	/* [한국어] iommus의 첫 인자 = 이 디바이스의 swgroup 번호. */
	u32 id = args->args[0];

	/*
	 * Note: we are here releasing the reference of &iommu_pdev->dev, which
	 * is mc->dev. Although some functions in tegra_smmu_ops may keep using
	 * its private data beyond this point, it's still safe to do so because
	 * the SMMU parent device is the same as the MC, so the reference count
	 * isn't strictly necessary.
	 */
	/* [한국어] 위 주석의 근거로 참조를 곧바로 내린다 — SMMU의 부모가
	 * 곧 MC라, MC가 살아 있는 한 그 데이터도 유효하기 때문이다. */
	put_device(&iommu_pdev->dev);

	/* [한국어] SMMU 인스턴스를 클라이언트의 priv에 심는다.
	 * attach와 device_group이 이 포인터를 쓴다. */
	dev_iommu_priv_set(dev, mc->smmu);

	/* [한국어] swgroup 번호를 fwspec에 추가한다. attach가 그 배열을
	 * 순회해 하드웨어를 켠다. */
	return iommu_fwspec_add_ids(dev, &id, 1);
}

/*
 * [한국어]
 * tegra_smmu_def_domain_type - 이 디바이스의 기본 도메인 종류를 정한다
 *
 * @dev: 대상 디바이스(보지 않는다).
 * @return: 항상 IOMMU_DOMAIN_IDENTITY.
 *
 * 이 한 줄이 이 드라이버의 현재 상태를 결정한다. 항상 IDENTITY를
 * 반환하므로, 모든 디바이스가 변환 없이(물리 주소 그대로) 동작한다.
 * 위쪽의 정교한 페이지 테이블 코드는 사용자가 명시적으로 UNMANAGED
 * 도메인을 만들지 않는 한 쓰이지 않는다.
 *
 * 원본 FIXME가 그 사정을 밝힌다: 일부 디바이스에 문제가 있어 임시로
 * 전체를 통과 모드로 두었고, 더 나은 방법은 문제가 있는 디바이스만
 * 개별적으로 처리하는 것이라고 적혀 있다.
 *
 * 실행 컨텍스트: 디바이스 probe 경로.
 *
 * 호출 체인:
 *   iommu_get_default_domain_type() → iommu_ops->def_domain_type
 *   → [tegra_smmu_def_domain_type]
 */
static int tegra_smmu_def_domain_type(struct device *dev)
{
	/*
	 * FIXME: For now we want to run all translation in IDENTITY mode, due
	 * to some device quirks. Better would be to just quirk the troubled
	 * devices.
	 */
	/* [한국어] 모든 디바이스를 통과 모드로 둔다. 위 FIXME가 밝히듯
	 * 일부 디바이스의 결함 때문에 취한 임시 조치다. */
	return IOMMU_DOMAIN_IDENTITY;
}

/* [한국어] IOMMU 코어에 노출하는 이 드라이버의 연산 테이블. */
static const struct iommu_ops tegra_smmu_ops = {
	/* [한국어] "변환 없음" 정적 도메인. def_domain_type이 항상
	 * IDENTITY를 반환하므로, 실질적으로 모든 디바이스의 기본 도메인이다. */
	.identity_domain = &tegra_smmu_identity_domain,
	/* [한국어] 기본 도메인 종류 결정 — 현재는 항상 IDENTITY다. */
	.def_domain_type = &tegra_smmu_def_domain_type,
	/* [한국어] 페이징 도메인 생성 — PD와 소프트웨어 배열을 만든다. */
	.domain_alloc_paging = tegra_smmu_domain_alloc_paging,
	/* [한국어] iommus를 직접 파싱해 담당 SMMU를 판정한다. */
	.probe_device = tegra_smmu_probe_device,
	/* [한국어] swgroup과 SoC 그룹 정의로 IOMMU 그룹을 결정한다. */
	.device_group = tegra_smmu_device_group,
	/* [한국어] iommus 항목 하나를 해석해 swgroup을 등록한다.
	 * 코어가 아니라 이 드라이버의 probe_device가 직접 호출한다. */
	.of_xlate = tegra_smmu_of_xlate,
	/* [한국어] 페이징 도메인의 연산 테이블(익명 const 구조체). */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		/* [한국어] ASID를 확보하고 swgroup들을 켠다. */
		.attach_dev	= tegra_smmu_attach_dev,
		/* [한국어] 한 번에 한 페이지씩 매핑한다. */
		.map_pages	= tegra_smmu_map,
		/* [한국어] 한 번에 한 페이지씩 해제하고, 빈 PT는 회수한다. */
		.unmap_pages	= tegra_smmu_unmap,
		/* [한국어] 2단계 소프트웨어 워크로 물리 주소를 조회한다. */
		.iova_to_phys	= tegra_smmu_iova_to_phys,
		/* [한국어] 소프트웨어 배열만 반납한다(PD/PT는 누수 — TODO 참조).
		 * iotlb_sync 계열 콜백이 없는 것은 엔트리를 고칠 때마다
		 * 즉시 무효화하기 때문이다. */
		.free		= tegra_smmu_domain_free,
	}
};

/*
 * [한국어]
 * tegra_smmu_ahb_enable - AHB 버스가 SMMU를 거치도록 켠다
 *
 * @return: 없음.
 *
 * Tegra30에는 AHB 버스에 붙은 마스터들이 있는데, 그 트래픽이 SMMU를
 * 거치게 하려면 AHB 컨트롤러 쪽에서 별도로 활성화해야 한다.
 * 디바이스 트리에서 AHB 노드를 찾아 그 함수를 부르는 것이 전부다.
 *
 * 매칭 테이블을 함수 안에 정적으로 두는 점이 특이한데, 이 함수 외에는
 * 쓰이지 않아 파일 전역을 어지럽히지 않으려는 배치로 보인다.
 * AHB 노드가 없는 SoC(Tegra114 이후)에서는 조용히 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: probe.
 *
 * 호출 체인:
 *   tegra_smmu_probe() → [tegra_smmu_ahb_enable] → tegra_ahb_enable_smmu()
 */
static void tegra_smmu_ahb_enable(void)
{
	/* [한국어] AHB 컨트롤러 노드를 찾기 위한 매칭 테이블.
	 * Tegra30에만 있는 노드다. */
	static const struct of_device_id ahb_match[] = {
		{ .compatible = "nvidia,tegra30-ahb", },	/* [한국어] Tegra30의 AHB 컨트롤러 노드. */
		{ }
	};
	/* [한국어] 찾은 AHB 노드. */
	struct device_node *ahb;

	/* [한국어] AHB 노드를 찾는다. 없으면(Tegra114 이후) 할 일이 없다. */
	ahb = of_find_matching_node(NULL, ahb_match);
	if (ahb) {
		/* [한국어] AHB 드라이버에게 SMMU 경유를 켜 달라고 요청한다. */
		tegra_ahb_enable_smmu(ahb);
		/* [한국어] 노드 참조를 내린다. */
		of_node_put(ahb);
	}
}

/*
 * [한국어]
 * tegra_smmu_swgroups_show - debugfs: swgroup별 활성화 상태와 ASID를 덤프한다
 *
 * @s: seq_file 출력 핸들.
 * @data: 사용하지 않는다.
 * @return: 항상 0.
 *
 * 각 swgroup의 레지스터를 읽어 활성화 여부와 ASID를 표로 출력한다.
 * "어느 디바이스가 어느 주소 공간에 붙어 있는가"를 눈으로 확인하는
 * 가장 직접적인 수단이다.
 *
 * 실행 컨텍스트: 사용자가 debugfs 파일을 읽을 때(프로세스 컨텍스트).
 * 락을 잡지 않으므로 읽는 도중 attach가 일어나면 일관되지 않은
 * 스냅숏이 나올 수 있지만, 진단용이라 문제 삼지 않는다.
 *
 * 호출 체인:
 *   사용자 read() → seq_file → [tegra_smmu_swgroups_show]
 */
static int tegra_smmu_swgroups_show(struct seq_file *s, void *data)
{
	/* [한국어] debugfs 파일에 연결해 둔 SMMU 인스턴스. */
	struct tegra_smmu *smmu = s->private;
	/* [한국어] swgroup 순회 인덱스. */
	unsigned int i;
	/* [한국어] 읽은 레지스터 값. */
	u32 value;

	/* [한국어] 표 머리글을 출력한다. */
	seq_printf(s, "swgroup    enabled  ASID\n");
	seq_printf(s, "------------------------\n");

	/* [한국어] SoC가 정의한 모든 swgroup을 훑는다. */
	for (i = 0; i < smmu->soc->num_swgroups; i++) {
		/* [한국어] i번째 swgroup 정의(이름과 레지스터 오프셋). */
		const struct tegra_smmu_swgroup *group = &smmu->soc->swgroups[i];
		/* [한국어] 활성화 여부를 나타낼 문자열. */
		const char *status;
		/* [한국어] 그 swgroup에 설정된 ASID. */
		unsigned int asid;

		/* [한국어] 그 swgroup의 레지스터를 읽는다. */
		value = smmu_readl(smmu, group->reg);

		/* [한국어] 활성화 비트로 상태 문자열을 정한다. */
		if (value & SMMU_ASID_ENABLE)
			status = "yes";
		else
			status = "no";

		/* [한국어] ASID 필드를 뽑는다. 비활성 상태라면 의미가 없는
		 * 값일 수 있다. */
		asid = value & SMMU_ASID_MASK;

		/* [한국어] 이름, 상태, ASID를 정렬해 한 줄로 출력한다. */
		seq_printf(s, "%-9s  %-7s  %#04x\n", group->name, status,
			   asid);
	}

	return 0;	/* [한국어] 모든 swgroup을 출력했다. */
}

/* [한국어] 위 show 함수를 debugfs 파일 연산으로 감싸는 매크로.
 * tegra_smmu_swgroups_fops라는 이름의 file_operations를 만들어 준다 —
 * open/read/llseek/release를 seq_file 표준 구현으로 채운다. */
DEFINE_SHOW_ATTRIBUTE(tegra_smmu_swgroups);

/*
 * [한국어]
 * tegra_smmu_clients_show - debugfs: 클라이언트별 활성화 상태를 덤프한다
 *
 * @s: seq_file 출력 핸들.
 * @data: 사용하지 않는다.
 * @return: 항상 0.
 *
 * swgroup보다 한 단계 아래인 개별 클라이언트(디스플레이 윈도, 카메라
 * 채널 등)의 활성화 비트를 훑어 출력한다. swgroup은 켜져 있는데
 * 특정 클라이언트만 꺼져 있는 상황을 찾아내는 데 쓴다.
 *
 * 실행 컨텍스트: 사용자가 debugfs 파일을 읽을 때.
 *
 * 호출 체인:
 *   사용자 read() → seq_file → [tegra_smmu_clients_show]
 */
static int tegra_smmu_clients_show(struct seq_file *s, void *data)
{
	/* [한국어] debugfs 파일에 연결해 둔 SMMU 인스턴스. */
	struct tegra_smmu *smmu = s->private;
	/* [한국어] 클라이언트 순회 인덱스. */
	unsigned int i;
	/* [한국어] 읽은 레지스터 값. */
	u32 value;

	/* [한국어] 표 머리글을 출력한다. */
	seq_printf(s, "client       enabled\n");
	seq_printf(s, "--------------------\n");

	/* [한국어] SoC가 정의한 모든 클라이언트를 훑는다. */
	for (i = 0; i < smmu->soc->num_clients; i++) {
		/* [한국어] i번째 클라이언트 정의(이름, 레지스터, 비트 위치). */
		const struct tegra_mc_client *client = &smmu->soc->clients[i];
		/* [한국어] 활성화 여부 문자열. */
		const char *status;

		/* [한국어] 그 클라이언트의 비트가 있는 레지스터를 읽는다. */
		value = smmu_readl(smmu, client->regs.smmu.reg);

		/* [한국어] 해당 비트로 상태를 판정한다. */
		if (value & BIT(client->regs.smmu.bit))
			status = "yes";
		else
			status = "no";

		/* [한국어] 이름과 상태를 한 줄로 출력한다. */
		seq_printf(s, "%-12s %s\n", client->name, status);
	}

	return 0;	/* [한국어] 모든 클라이언트를 출력했다. */
}

/* [한국어] 클라이언트 덤프용 file_operations를 만든다.
 * tegra_smmu_clients_fops라는 이름이 생긴다. */
DEFINE_SHOW_ATTRIBUTE(tegra_smmu_clients);

/*
 * [한국어]
 * tegra_smmu_debugfs_init - debugfs 파일들을 만든다
 *
 * @smmu: 대상 인스턴스.
 * @return: 없음.
 *
 * /sys/kernel/debug/smmu/ 아래에 swgroups와 clients 두 파일을 만든다.
 * 디렉토리를 NULL 부모에 만들므로 debugfs 최상위에 놓인다 —
 * SMMU가 하나뿐이라는 전제에서 가능한 단순화다.
 *
 * 반환값을 검사하지 않는 이유: debugfs는 실패해도 기능에 영향이 없어,
 * 커널 관례상 오류를 무시한다.
 *
 * 실행 컨텍스트: probe(CONFIG_DEBUG_FS가 켜져 있을 때만).
 *
 * 호출 체인:
 *   tegra_smmu_probe() → [tegra_smmu_debugfs_init]
 */
static void tegra_smmu_debugfs_init(struct tegra_smmu *smmu)
{
	/* [한국어] debugfs 최상위에 "smmu" 디렉토리를 만든다. */
	smmu->debugfs = debugfs_create_dir("smmu", NULL);

	/* [한국어] swgroup 상태 파일을 만든다. private 데이터로 smmu를
	 * 넘겨 show 함수가 s->private로 받을 수 있게 한다. */
	debugfs_create_file("swgroups", S_IRUGO, smmu->debugfs, smmu,
			    &tegra_smmu_swgroups_fops);
	/* [한국어] 클라이언트 상태 파일을 만든다. */
	debugfs_create_file("clients", S_IRUGO, smmu->debugfs, smmu,
			    &tegra_smmu_clients_fops);
}

/*
 * [한국어]
 * tegra_smmu_debugfs_exit - debugfs 파일들을 제거한다
 *
 * @smmu: 대상 인스턴스.
 * @return: 없음.
 *
 * 디렉토리를 재귀적으로 지우면 그 안의 파일들도 함께 사라진다.
 *
 * 실행 컨텍스트: remove(CONFIG_DEBUG_FS가 켜져 있을 때만).
 *
 * 호출 체인:
 *   tegra_smmu_remove() → [tegra_smmu_debugfs_exit]
 */
static void tegra_smmu_debugfs_exit(struct tegra_smmu *smmu)
{
	/* [한국어] 디렉토리와 그 안의 파일을 모두 제거한다. */
	debugfs_remove_recursive(smmu->debugfs);
}

/*
 * [한국어]
 * tegra_smmu_probe - SMMU를 초기화한다(MC 드라이버가 호출한다)
 *
 * @dev: MC 디바이스.
 * @soc: 이 SoC의 SMMU 정의(swgroup/클라이언트 목록 등).
 * @mc: 메모리 컨트롤러 인스턴스.
 * @return: 새 SMMU 인스턴스, 실패하면 ERR_PTR.
 *
 * 다른 IOMMU 드라이버와 달리 플랫폼 드라이버의 probe가 아니라,
 * MC 드라이버가 직접 부르는 함수다. SMMU가 MC 블록의 일부이기 때문이다.
 *
 * 동작 과정:
 *  1) 인스턴스 할당.
 *  2) mc->smmu를 **먼저** 설정한다. 원본 주석이 그 이유를 설명한다 —
 *     iommu_device_register()가 반환되기 전에 이미 디바이스들을
 *     추가하려 하고, 그 과정에서 probe_device가 mc->smmu를 읽기 때문이다.
 *     전역 변수를 쓰지 않으려는 대안으로 택한 방법이다.
 *  3) ASID 비트맵과 그룹 목록, 락을 준비한다.
 *  4) pfn_mask와 tlb_mask를 SoC 정보에서 계산한다.
 *  5) PTC와 TLB 설정을 쓴다.
 *  6) 두 캐시를 비우고 SMMU를 켠다.
 *  7) AHB 경유를 켠다(Tegra30만).
 *  8) sysfs와 IOMMU 코어에 등록하고 debugfs를 만든다.
 *
 * 레지스터를 mc->regs로 공유하는 점에 주목: SMMU 레지스터가 MC 레지스터
 * 영역 안에 있어 별도 매핑이 필요 없다.
 *
 * 실행 컨텍스트: MC 드라이버의 probe(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   drivers/memory/tegra/mc.c의 probe → [tegra_smmu_probe]
 *   → iommu_device_register()
 */
struct tegra_smmu *tegra_smmu_probe(struct device *dev,
				    const struct tegra_smmu_soc *soc,
				    struct tegra_mc *mc)
{
	/* [한국어] 만들 SMMU 인스턴스. */
	struct tegra_smmu *smmu;
	/* [한국어] 레지스터에 쓸 값을 조립하는 임시 변수. */
	u32 value;
	/* [한국어] 등록 결과. */
	int err;

	/* [한국어] 인스턴스를 0으로 초기화해 할당한다. devm이라 MC 디바이스와
	 * 함께 사라진다. */
	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);
	if (!smmu)	/* [한국어] 인스턴스를 할당하지 못했다. */
		return ERR_PTR(-ENOMEM);

	/*
	 * This is a bit of a hack. Ideally we'd want to simply return this
	 * value. However iommu_device_register() will attempt to add
	 * all devices to the IOMMU before we get that far. In order
	 * not to rely on global variables to track the IOMMU instance, we
	 * set it here so that it can be looked up from the .probe_device()
	 * callback via the IOMMU device's .drvdata field.
	 */
	/* [한국어] 원본 주석이 밝히는 순환 의존을 푸는 지점이다.
	 * iommu_device_register()가 반환되기 전에 디바이스들을 추가하려
	 * 하고, 그때 probe_device가 tegra_smmu_find()로 mc->smmu를 읽는다.
	 * 그래서 반환값을 기다리지 않고 여기서 미리 심어 둔다 —
	 * 전역 변수를 쓰지 않으려는 대안이다. */
	mc->smmu = smmu;

	/* [한국어] ASID 사용 비트맵을 0으로 초기화해 할당한다.
	 * 크기는 SoC가 정한 ASID 개수만큼이다. */
	smmu->asids = devm_bitmap_zalloc(dev, soc->num_asids, GFP_KERNEL);
	if (!smmu->asids)	/* [한국어] ASID 비트맵을 확보하지 못했다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] IOMMU 그룹 목록을 초기화한다. */
	INIT_LIST_HEAD(&smmu->groups);
	/* [한국어] ASID 할당과 그룹 목록을 보호할 뮤텍스를 준비한다. */
	mutex_init(&smmu->lock);

	/* [한국어] SMMU 레지스터는 MC 레지스터 영역 안에 있어 그대로 공유한다. */
	smmu->regs = mc->regs;
	/* [한국어] SoC별 정의(swgroup/클라이언트 목록)를 보관한다. */
	smmu->soc = soc;
	/* [한국어] MC 디바이스를 DMA 마스터이자 로깅 대상으로 쓴다. */
	smmu->dev = dev;
	/* [한국어] MC 인스턴스를 보관한다 — atom_size와 주소 폭을 읽는다. */
	smmu->mc = mc;

	/* [한국어] PDE/PTE에 담을 수 있는 페이지 번호의 마스크를 계산한다.
	 * 물리 주소 폭에서 페이지 시프트를 뺀 만큼의 비트가 필요하고,
	 * BIT_MASK(n) - 1이 하위 n비트를 모두 세운 값을 준다. */
	smmu->pfn_mask =
		BIT_MASK(mc->soc->num_address_bits - SMMU_PTE_SHIFT) - 1;
	/* [한국어] 계산 결과를 디버그 로그로 남겨, 주소 폭 설정이 맞는지
	 * 부팅 로그에서 확인할 수 있게 한다. */
	dev_dbg(dev, "address bits: %u, PFN mask: %#lx\n",
		mc->soc->num_address_bits, smmu->pfn_mask);
	/* [한국어] TLB 라인 수를 표현하는 데 필요한 비트 마스크를 만든다.
	 * fls(n)이 최상위 비트 위치를 주므로, (1 << fls(n)) - 1이 그 값을
	 * 담을 수 있는 마스크가 된다. */
	smmu->tlb_mask = (1 << fls(smmu->soc->num_tlb_lines)) - 1;
	dev_dbg(dev, "TLB lines: %u, mask: %#lx\n", smmu->soc->num_tlb_lines,	/* [한국어] TLB 라인 수와 계산된 마스크를 남겨 설정을 확인할 수 있게 한다. */
		smmu->tlb_mask);

	/* [한국어] PTC를 켜고 인덱스 매핑을 전 비트(0x3f)로 설정한다 —
	 * 캐시 인덱스에 최대한 많은 주소 비트를 쓰게 해 충돌을 줄인다. */
	value = SMMU_PTC_CONFIG_ENABLE | SMMU_PTC_CONFIG_INDEX_MAP(0x3f);

	/* [한국어] 요청 개수 제한을 지원하는 SoC에서만 8로 설정한다.
	 * 동시에 처리할 테이블 페치 수를 제한해 버스 혼잡을 막는다. */
	if (soc->supports_request_limit)
		value |= SMMU_PTC_CONFIG_REQ_LIMIT(8);

	smmu_writel(smmu, value, SMMU_PTC_CONFIG);

	/* [한국어] TLB 설정 — hit-under-miss를 켜 미스 처리 중에도 히트를
	 * 처리하게 하고, 활성 라인 수를 SoC 값으로 지정한다. */
	value = SMMU_TLB_CONFIG_HIT_UNDER_MISS |
		SMMU_TLB_CONFIG_ACTIVE_LINES(smmu);

	/* [한국어] 라운드로빈 중재를 지원하는 SoC에서만 켠다 —
	 * 여러 클라이언트가 경합할 때 공평하게 나눠 준다. */
	if (soc->supports_round_robin_arbitration)
		value |= SMMU_TLB_CONFIG_ROUND_ROBIN_ARBITRATION;

	smmu_writel(smmu, value, SMMU_TLB_CONFIG);

	/* [한국어] 부트로더가 남긴 PTC 내용을 전부 지운다. */
	smmu_flush_ptc_all(smmu);
	/* [한국어] 마찬가지로 TLB도 전부 비운다. */
	smmu_flush_tlb(smmu);
	/* [한국어] SMMU를 켠다. 이 시점부터 활성화된 swgroup의 트래픽이
	 * 변환된다(아직 활성화된 swgroup은 없다). */
	smmu_writel(smmu, SMMU_CONFIG_ENABLE, SMMU_CONFIG);
	/* [한국어] 쓰기가 하드웨어에 도달했음을 보장한다. */
	smmu_flush(smmu);

	/* [한국어] Tegra30에서 AHB 버스 트래픽도 SMMU를 거치게 한다. */
	tegra_smmu_ahb_enable();

	/* [한국어] /sys/class/iommu/ 아래에 이 SMMU를 노출한다. */
	err = iommu_device_sysfs_add(&smmu->iommu, dev, NULL, dev_name(dev));
	if (err)	/* [한국어] sysfs 등록 실패 — 코어 등록으로 진행할 수 없다. */
		return ERR_PTR(err);

	/* [한국어] IOMMU 코어에 연산 테이블을 등록한다. 위 주석대로
	 * 이 호출이 반환되기 전에 probe_device가 불릴 수 있어,
	 * mc->smmu를 미리 설정해 둔 것이다. */
	err = iommu_device_register(&smmu->iommu, &tegra_smmu_ops, dev);
	if (err) {
		/* [한국어] 등록 실패 시 sysfs 노드를 되돌린다. */
		iommu_device_sysfs_remove(&smmu->iommu);
		return ERR_PTR(err);	/* [한국어] 코어 등록 실패를 오류 포인터로 반환한다. */
	}

	/* [한국어] debugfs가 켜진 커널에서만 진단 파일을 만든다. */
	if (IS_ENABLED(CONFIG_DEBUG_FS))
		tegra_smmu_debugfs_init(smmu);

	/* [한국어] 초기화된 인스턴스를 MC 드라이버에 돌려준다. */
	return smmu;
}

/*
 * [한국어]
 * tegra_smmu_remove - SMMU를 정리한다(MC 드라이버가 호출한다)
 *
 * @smmu: 정리할 인스턴스.
 * @return: 없음.
 *
 * 코어 등록을 해제하고 sysfs와 debugfs를 정리한다. 하드웨어를 끄지
 * 않는 점에 주목: SMMU_CONFIG의 ENABLE 비트가 그대로 남아 페이지
 * 테이블이 해제된 뒤에도 변환이 시도될 수 있다. 실무적으로는 시스템
 * 종료 시점에만 실행되는 경로라 문제가 드러나지 않는다.
 *
 * 실행 컨텍스트: MC 드라이버의 remove(프로세스 컨텍스트).
 *
 * 호출 체인:
 *   drivers/memory/tegra/mc.c의 remove → [tegra_smmu_remove]
 */
void tegra_smmu_remove(struct tegra_smmu *smmu)
{
	/* [한국어] 코어 등록을 해제해 더 이상 콜백이 들어오지 않게 한다. */
	iommu_device_unregister(&smmu->iommu);
	/* [한국어] sysfs 노드를 제거한다. */
	iommu_device_sysfs_remove(&smmu->iommu);

	/* [한국어] debugfs 파일들도 정리한다. */
	if (IS_ENABLED(CONFIG_DEBUG_FS))
		tegra_smmu_debugfs_exit(smmu);
}
