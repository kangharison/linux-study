// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2006-2014 Intel Corporation.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>,
 *          Ashok Raj <ashok.raj@intel.com>,
 *          Shaohua Li <shaohua.li@intel.com>,
 *          Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>,
 *          Fenghua Yu <fenghua.yu@intel.com>
 *          Joerg Roedel <jroedel@suse.de>
 */

/*
 * [한국어 설명] 인텔 VT-d IOMMU 드라이버 본체 (drivers/iommu/intel/iommu.c)
 *
 * === 파일의 역할 ===
 * 인텔 서버·데스크톱 칩셋의 IOMMU(VT-d)를 커널 IOMMU API 로 감싸는 드라이버다.
 * NVMe 나 NIC 이 x86 서버에서 dma_map_sg 를 부르면, 코어 계층을 지나 결국 이
 * 파일의 map_pages 가 인텔 페이지 테이블에 PTE 를 기입한다.
 *
 * VT-d 의 자료구조는 3층이다. 하드웨어가 DMA 요청의 requester id(버스:장치.함수)를
 * 받으면 —
 *   1) 루트 테이블(root table): 버스 번호로 인덱싱. 256개 항목.
 *   2) 컨텍스트 테이블(context table): 장치.함수로 인덱싱. 이 항목이 그 장치의
 *      주소 공간(도메인)과 페이지 테이블 루트를 가리킨다.
 *   3) 페이지 테이블: 4단계 또는 5단계. x86 CPU 의 것과 형식이 거의 같다.
 * scalable mode 에서는 컨텍스트 항목이 PASID 디렉터리를 가리키고, PASID 항목마다
 * 별도의 페이지 테이블을 둘 수 있다 — 그것이 SVA 의 하드웨어 근거다.
 *
 * 이 드라이버가 코어와 다른 벤더 드라이버에 비해 특별한 점이 몇 가지 있다.
 *  - RMRR(Reserved Memory Region Reporting): 펌웨어가 "이 장치는 이 물리 주소를
 *    계속 쓴다"고 ACPI 로 신고한다. USB 레거시 에뮬레이션과 관리 엔진이 대표적이며,
 *    그 구간은 항등 매핑으로 유지해야 한다.
 *  - 여러 개의 DMAR 유닛. 소켓마다, 또는 PCIe 루트 포트 묶음마다 별도 하드웨어가
 *    있어, 도메인 하나가 여러 유닛에 걸쳐 설치될 수 있다.
 *  - 무효화 큐(queued invalidation): 무효화를 레지스터가 아니라 링 버퍼로 보낸다.
 *    그 배치 처리가 dma-iommu 의 flush queue 와 맞물려 성능을 만든다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름(매핑): 드라이버 dma_map_sg
 *   → dma-iommu.c (IOVA 확보) → iommu.c iommu_map_sg
 *     → [이 파일] intel_iommu_map_pages : 인텔 페이지 테이블에 PTE 기입
 * 흐름(부착): 장치 프로브 → iommu.c iommu_init_device
 *   → [이 파일] intel_iommu_probe_device (DMAR 유닛과 소스 id 결정)
 *   → 도메인 부착 시 컨텍스트 항목 또는 PASID 항목 기입
 * 흐름(무효화): iommu_unmap → [이 파일] → cache.c 의 무효화 큐 → 하드웨어
 *
 * === 타 모듈과의 연결 ===
 * - dmar.c: ACPI DMAR 테이블을 파싱해 DMAR 유닛과 그 관할 장치를 알아낸다.
 *   이 파일이 다루는 iommu 인스턴스는 그쪽이 만든 것이다.
 * - pasid.c: PASID 표 조작. scalable mode 의 핵심.
 * - cache.c: 무효화 큐 관리. 어느 범위를 어느 유닛에 무효화할지 추적한다.
 * - irq_remapping.c: 인터럽트 재매핑. 같은 하드웨어의 다른 기능이다.
 * - prq.c / svm.c: 페이지 요청 큐와 SVA.
 * - nested.c: 중첩 번역 (사용자 공간이 stage-1 을 관리).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct dmar_domain     : 이 드라이버의 도메인. 페이지 테이블 루트와 부착된
 *                            장치·유닛 목록을 담는다.
 * - domain_context_mapping(): 장치의 컨텍스트 항목에 도메인을 기입한다.
 * - __domain_mapping()     : 페이지 테이블에 PTE 를 채운다.
 * - domain_unmap()         : 그 역. 해제된 테이블 페이지를 목록에 모은다.
 * - intel_iommu_probe_device(): 장치를 맡을 DMAR 유닛과 소스 id 를 정한다.
 * - intel_iommu_attach_device(): 도메인을 하드웨어에 설치한다.
 * - iommu_enable_translation(): 유닛의 번역을 켠다.
 */
#define pr_fmt(fmt)     "DMAR: " fmt	/* [한국어] 이 파일의 로그 접두사. DMAR 은 ACPI 표 이름(DMA Remapping)이자 인텔 IOMMU 의 통칭이다 */
#define dev_fmt(fmt)    pr_fmt(fmt)	/* [한국어] dev_err/dev_warn 도 같은 접두사를 쓰게 한다 */

#include <linux/crash_dump.h>	/* [한국어] kdump 커널 판별. 앞선 커널이 남긴 매핑 위에서 부팅하므로 처리가 다르다 */
#include <linux/dma-direct.h>	/* [한국어] IOMMU 를 거치지 않는 직접 매핑 경로 */
#include <linux/dmi.h>	/* [한국어] BIOS/보드 식별로 하드웨어 결함 우회(quirk)를 적용한다 */
#include <linux/memory.h>	/* [한국어] 메모리 핫플러그 통지 — 새 메모리가 붙으면 항등 도메인에 매핑해야 한다 */
#include <linux/pci.h>	/* [한국어] PCI 장치 순회와 requester id 처리 */
#include <linux/pci-ats.h>	/* [한국어] ATS/PRI 능력 제어. 장치가 번역을 캐시하게 하는 기능 */
#include <linux/spinlock.h>	/* [한국어] 도메인·장치 목록 보호 */
#include <linux/syscore_ops.h>	/* [한국어] 서스펜드/리쥼 시 IOMMU 레지스터 보존 */
#include <linux/tboot.h>	/* [한국어] Trusted Boot 연동. TXT 로 부팅하면 VT-d 활성화 실패가 보안 문제가 된다 */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자 공간과 공유하는 중첩 번역 ABI */

#include "iommu.h"	/* [한국어] 이 드라이버의 자료구조와 레지스터 정의 */
#include "../dma-iommu.h"	/* [한국어] 코어의 DMA API 통합 계층 */
#include "../irq_remapping.h"	/* [한국어] 인터럽트 재매핑 — 같은 하드웨어의 다른 기능 */
#include "../iommu-pages.h"	/* [한국어] 페이지 테이블 페이지 할당자 */
#include "pasid.h"	/* [한국어] PASID 표 조작 (scalable mode) */
#include "perfmon.h"	/* [한국어] IOMMU 성능 카운터 */

#define ROOT_SIZE		VTD_PAGE_SIZE	/* [한국어] 루트 테이블은 정확히 한 페이지다 — 256개 항목 × 16바이트 */
#define CONTEXT_SIZE		VTD_PAGE_SIZE	/* [한국어] 컨텍스트 테이블도 한 페이지. 버스 하나당 256개 함수 항목 */

#define IS_GFX_DEVICE(pdev) pci_is_display(pdev)	/* [한국어] 그래픽 장치 판별. 인텔 GPU 는 펌웨어가 남긴 프레임버퍼 매핑 때문에 특별 처리가 필요하다 */
#define IS_USB_DEVICE(pdev) ((pdev->class >> 8) == PCI_CLASS_SERIAL_USB)	/* [한국어] USB 컨트롤러. 레거시 키보드 에뮬레이션이 RMRR 을 요구하는 대표적인 장치다 */
#define IS_ISA_DEVICE(pdev) ((pdev->class >> 8) == PCI_CLASS_BRIDGE_ISA)	/* [한국어] ISA 브리지. 옛 DMA 컨트롤러가 저역 메모리를 직접 쓴다 */
#define IS_AZALIA(pdev) ((pdev)->vendor == 0x8086 && (pdev)->device == 0x3a3e)	/* [한국어] 특정 인텔 HD 오디오 컨트롤러. 아래의 Tylersburg 아이소크로너스 결함 우회 대상이다 */

#define IOAPIC_RANGE_START	(0xfee00000)	/* [한국어] x86 의 인터럽트 메시지 주소 창 시작. 이 범위로 가는 쓰기는 메모리가 아니라 인터럽트다 */
#define IOAPIC_RANGE_END	(0xfeefffff)	/* [한국어] 그 끝. DMA 가 이 주소를 쓰면 임의의 인터럽트를 만들 수 있어 반드시 예약해 둔다 */
#define IOVA_START_ADDR		(0x1000)	/* [한국어] IOVA 할당의 하한. 주소 0 페이지를 비워 둬 NULL DMA 주소를 오류로 잡는다 */

#define DEFAULT_DOMAIN_ADDRESS_WIDTH 57	/* [한국어] 5단계 페이지 테이블의 주소 폭. 하드웨어가 지원하지 않으면 4단계(48비트)로 낮춘다 */

static void __init check_tylersburg_isoch(void);	/* [한국어] 전방 선언 — 특정 칩셋의 아이소크로너스 DMA 결함을 확인한다 */
static int intel_iommu_set_dirty_tracking(struct iommu_domain *domain,	/* [한국어] 전방 선언 — 라이브 마이그레이션용 더티 추적 */
					  bool enable);	/* [한국어] 켜기/끄기 */
static int rwbf_quirk;	/* [한국어] 쓰기 버퍼 플러시가 필요한 하드웨어 결함. DMI 로 특정 보드를 식별해 켠다 */

#define rwbf_required(iommu)	(rwbf_quirk || cap_rwbf((iommu)->cap))	/* [한국어] 결함 우회이거나 하드웨어가 스스로 요구하면 페이지 테이블 기입 뒤 쓰기 버퍼를 비워야 한다 */

/*
 * set to 1 to panic kernel if can't successfully enable VT-d
 * (used when kernel is launched w/ TXT)
 */
static int force_on = 0;	/* [한국어] VT-d 활성화 실패를 치명적으로 다룰지 (위 영어 주석). TXT 로 부팅한 경우 격리 없이 진행하는 것이 더 위험하다 */
static int intel_iommu_tboot_noforce;	/* [한국어] 그 강제를 부트 인자로 끄는 스위치 */
static int no_platform_optin;	/* [한국어] 펌웨어가 IOMMU 사용을 권장하지 않았음을 기록. 그런 시스템에서는 기본값을 보수적으로 잡는다 */

#define ROOT_ENTRY_NR (VTD_PAGE_SIZE/sizeof(struct root_entry))	/* [한국어] 루트 테이블의 항목 수 = 256 (PCI 버스 번호의 범위) */

/*
 * Take a root_entry and return the Lower Context Table Pointer (LCTP)
 * if marked present.
 */
static phys_addr_t root_entry_lctp(struct root_entry *re)
{
	if (!(re->lo & 1))	/* [한국어] 비트 0 이 present 플래그다 */
		return 0;	/* [한국어] 이 버스에는 컨텍스트 테이블이 없다 */

	return re->lo & VTD_PAGE_MASK;	/* [한국어] 하위 컨텍스트 테이블의 물리 주소. 장치 번호 0~127 을 담당한다 */
}

/*
 * Take a root_entry and return the Upper Context Table Pointer (UCTP)
 * if marked present.
 */
static phys_addr_t root_entry_uctp(struct root_entry *re)
{
	if (!(re->hi & 1))	/* [한국어] 상위 항목의 present 플래그 */
		return 0;	/* [한국어] 없다 */

	return re->hi & VTD_PAGE_MASK;	/* [한국어] 상위 컨텍스트 테이블. 장치 번호 128~255 를 담당하며, 한 페이지에 256개 항목이 들어가지 않아 둘로 나뉜다 */
}

static int device_rid_cmp_key(const void *key, const struct rb_node *node)
{
	struct device_domain_info *info =	/* [한국어] 트리 노드에서 장치 정보로 */
		rb_entry(node, struct device_domain_info, node);	/* [한국어] container_of */
	const u16 *rid_lhs = key;	/* [한국어] 찾는 소스 id */

	if (*rid_lhs < PCI_DEVID(info->bus, info->devfn))	/* [한국어] 버스와 devfn 을 합친 16비트 값으로 비교한다 */
		return -1;	/* [한국어] 왼쪽으로 */

	if (*rid_lhs > PCI_DEVID(info->bus, info->devfn))	/* [한국어] 더 크면 */
		return 1;	/* [한국어] 오른쪽으로 */

	return 0;	/* [한국어] 일치 */
}

static int device_rid_cmp(struct rb_node *lhs, const struct rb_node *rhs)
{
	struct device_domain_info *info =	/* [한국어] 삽입할 노드의 장치 정보 */
		rb_entry(lhs, struct device_domain_info, node);	/* [한국어] container_of */
	u16 key = PCI_DEVID(info->bus, info->devfn);	/* [한국어] 그 장치의 소스 id */

	return device_rid_cmp_key(&key, rhs);	/* [한국어] 키 비교 함수를 재사용한다 */
}

/*
 * Looks up an IOMMU-probed device using its source ID.
 *
 * Returns the pointer to the device if there is a match. Otherwise,
 * returns NULL.
 *
 * Note that this helper doesn't guarantee that the device won't be
 * released by the iommu subsystem after being returned. The caller
 * should use its own synchronization mechanism to avoid the device
 * being released during its use if its possibly the case.
 */
struct device *device_rbtree_find(struct intel_iommu *iommu, u16 rid)
{
	struct device_domain_info *info = NULL;	/* [한국어] 찾은 장치 정보 */
	struct rb_node *node;	/* [한국어] 트리 노드 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&iommu->device_rbtree_lock, flags);	/* [한국어] 이 조회는 폴트 인터럽트 문맥에서도 불린다 — 하드웨어가 소스 id 만 알려 주므로 그것으로 장치를 되찾아야 한다 */
	node = rb_find(&rid, &iommu->device_rbtree, device_rid_cmp_key);	/* [한국어] 소스 id 로 이진 탐색 */
	if (node)	/* [한국어] 찾았으면 */
		info = rb_entry(node, struct device_domain_info, node);	/* [한국어] 장치 정보로 변환 */
	spin_unlock_irqrestore(&iommu->device_rbtree_lock, flags);	/* [한국어] 락 해제 */

	return info ? info->dev : NULL;	/* [한국어] 반환된 장치의 수명은 보장되지 않는다 — 호출자가 따로 동기화해야 한다 (위 영어 주석) */
}

static int device_rbtree_insert(struct intel_iommu *iommu,
				struct device_domain_info *info)
{
	struct rb_node *curr;	/* [한국어] 이미 있던 노드 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&iommu->device_rbtree_lock, flags);	/* [한국어] 트리 변경 구간 */
	curr = rb_find_add(&info->node, &iommu->device_rbtree, device_rid_cmp);	/* [한국어] 찾으면서 없으면 넣는다 — 중복 검사와 삽입을 한 번의 순회로 */
	spin_unlock_irqrestore(&iommu->device_rbtree_lock, flags);	/* [한국어] 락 해제 */
	if (WARN_ON(curr))	/* [한국어] 같은 소스 id 가 이미 등록되어 있다 */
		return -EEXIST;	/* [한국어] 두 장치가 같은 requester id 를 쓸 수는 없다 */

	return 0;	/* [한국어] 등록 완료 — 이제 폴트가 이 장치로 되짚어진다 */
}

static void device_rbtree_remove(struct device_domain_info *info)
{
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 이 장치를 맡은 DMAR 유닛 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&iommu->device_rbtree_lock, flags);	/* [한국어] 트리 변경 구간 */
	rb_erase(&info->node, &iommu->device_rbtree);	/* [한국어] 트리에서 제거. 이후 이 소스 id 의 폴트는 장치를 찾지 못한다 */
	spin_unlock_irqrestore(&iommu->device_rbtree_lock, flags);	/* [한국어] 락 해제 */
}

/*
 * [한국어] RMRR(Reserved Memory Region Reporting) 항목 하나.
 *
 * 펌웨어가 ACPI DMAR 표에 "이 장치는 이 물리 주소 범위를 계속 쓰고 있다"고
 * 신고한 것이다. 커널이 그 장치를 IOMMU 아래로 들일 때 그 범위를 항등 매핑으로
 * 유지하지 않으면, 펌웨어가 쓰던 버퍼로 가는 길이 끊긴다.
 *
 * 대표적인 것이 USB 레거시 키보드 에뮬레이션(SMM 이 컨트롤러를 계속 만진다)과
 * 관리 엔진이다. IOMMU 격리의 명백한 구멍이지만, 그것 없이는 그 장치가 동작하지
 * 않으므로 커널이 받아들일 수밖에 없다.
 */
struct dmar_rmrr_unit {
	struct list_head list;		/* list of rmrr units	*/	/* [한국어] 전역 RMRR 목록의 고리 */
	struct acpi_dmar_header *hdr;	/* ACPI header		*/	/* [한국어] 원본 ACPI 항목. 재파싱이나 진단에 쓴다 */
	u64	base_address;		/* reserved base address*/	/* [한국어] 예약 구간의 시작 물리 주소 */
	u64	end_address;		/* reserved end address */	/* [한국어] 끝 주소 (포함) */
	struct dmar_dev_scope *devices;	/* target devices */	/* [한국어] 이 예약이 적용되는 장치 목록. 펌웨어가 버스·장치 경로로 지정한다 */
	int	devices_cnt;		/* target device count */	/* [한국어] 그 개수 */
};

/*
 * [한국어] ATSR(ATS Reporting) 항목 하나.
 *
 * 어느 PCIe 루트 포트 아래의 장치가 ATS 를 쓸 수 있는지를 펌웨어가 신고한 것이다.
 * ATS 는 장치가 번역 결과를 자기 캐시(ATC)에 들고 있게 해 IOMMU 왕복을 줄이지만,
 * 그러려면 그 경로의 모든 스위치와 루트 포트가 프로토콜을 중계해야 한다.
 *
 * include_all 이 서 있으면 그 유닛 아래의 모든 포트가 해당된다.
 */
struct dmar_atsr_unit {
	struct list_head list;		/* list of ATSR units */	/* [한국어] 전역 ATSR 목록의 고리 */
	struct acpi_dmar_header *hdr;	/* ACPI header */	/* [한국어] 원본 ACPI 항목 */
	struct dmar_dev_scope *devices;	/* target devices */	/* [한국어] ATS 를 지원하는 루트 포트들 */
	int devices_cnt;		/* target device count */	/* [한국어] 그 개수 */
	u8 include_all:1;		/* include all ports */	/* [한국어] 이 유닛 아래 모든 포트가 해당된다는 표시. 포트를 일일이 나열하지 않아도 되게 한다 */
};

/*
 * [한국어] SATC(SoC Integrated Address Translation Cache) 항목 하나.
 *
 * SoC 에 통합된 장치 중 ATS 를 쓰되 PCIe 표준 경로를 거치지 않는 것들을 신고한다.
 * 통합 그래픽이나 가속기처럼 칩 안에서 직접 연결된 장치가 대상이며, 표준 ATS
 * 능력 비트로는 알 수 없어 펌웨어가 별도로 알려 준다.
 */
struct dmar_satc_unit {
	struct list_head list;		/* list of SATC units */	/* [한국어] 전역 SATC 목록의 고리 */
	struct acpi_dmar_header *hdr;	/* ACPI header */	/* [한국어] 원본 ACPI 항목 */
	struct dmar_dev_scope *devices;	/* target devices */	/* [한국어] SoC 통합 ATS 장치들 */
	struct intel_iommu *iommu;	/* the corresponding iommu */	/* [한국어] 이 항목을 담당하는 DMAR 유닛 */
	int devices_cnt;		/* target device count */	/* [한국어] 장치 개수 */
	u8 atc_required:1;		/* ATS is required */
};

static LIST_HEAD(dmar_atsr_units);
static LIST_HEAD(dmar_rmrr_units);
static LIST_HEAD(dmar_satc_units);

#define for_each_rmrr_units(rmrr) \
	list_for_each_entry(rmrr, &dmar_rmrr_units, list)

static void intel_iommu_domain_free(struct iommu_domain *domain);

int dmar_disabled = !IS_ENABLED(CONFIG_INTEL_IOMMU_DEFAULT_ON);
int intel_iommu_sm = IS_ENABLED(CONFIG_INTEL_IOMMU_SCALABLE_MODE_DEFAULT_ON);

int intel_iommu_enabled = 0;	/* [한국어] VT-d 가 실제로 켜졌는가. 다른 서브시스템(그래픽 드라이버 등)이 참고한다 */
EXPORT_SYMBOL_GPL(intel_iommu_enabled);	/* [한국어] 모듈에서도 볼 수 있게 */

static int intel_iommu_superpage = 1;	/* [한국어] 큰 페이지(2MB/1GB) 매핑을 쓸지. 끄면 PTE 가 늘지만 일부 하드웨어 결함을 피할 수 있다 */
static int iommu_identity_mapping;	/* [한국어] 항등 매핑이 필요한 장치 종류의 비트마스크 */
static int iommu_skip_te_disable;	/* [한국어] 종료 시 번역을 끄지 않는다. kexec 로 넘어갈 때 진행 중인 DMA 를 끊지 않기 위한 것 */
static int disable_igfx_iommu;	/* [한국어] 통합 그래픽을 IOMMU 밖에 둔다. 일부 세대의 GPU 펌웨어가 IOMMU 아래에서 오작동해 생긴 우회다 */

#define IDENTMAP_AZALIA		4	/* [한국어] 특정 HD 오디오 컨트롤러에 항등 매핑을 강제하는 비트 */

const struct iommu_ops intel_iommu_ops;	/* [한국어] 코어에 등록할 콜백 표. 정의는 파일 끝에 있다 */

static bool translation_pre_enabled(struct intel_iommu *iommu)
{
	return (iommu->flags & VTD_FLAG_TRANS_PRE_ENABLED);	/* [한국어] 커널이 시작하기 전에 이미 번역이 켜져 있었는가. kexec 나 펌웨어가 켜 둔 경우이며, 그 상태를 함부로 끄면 진행 중인 DMA 가 끊긴다 */
}

static void clear_translation_pre_enabled(struct intel_iommu *iommu)
{
	iommu->flags &= ~VTD_FLAG_TRANS_PRE_ENABLED;	/* [한국어] 우리가 상태를 넘겨받았음을 표시한다 */
}

static void init_translation_status(struct intel_iommu *iommu)
{
	u32 gsts;	/* [한국어] 전역 상태 레지스터 값 */

	gsts = readl(iommu->reg + DMAR_GSTS_REG);	/* [한국어] 하드웨어 상태를 읽는다 */
	if (gsts & DMA_GSTS_TES)	/* [한국어] Translation Enable Status — 번역이 이미 켜져 있다 */
		iommu->flags |= VTD_FLAG_TRANS_PRE_ENABLED;	/* [한국어] 기억해 둔다. 이후 초기화가 기존 테이블을 이어받을지 새로 만들지를 이 값으로 정한다 */
}

static int __init intel_iommu_setup(char *str)
{
	if (!str)	/* [한국어] 값 없는 인자 */
		return -EINVAL;	/* [한국어] 해석 실패 */

	while (*str) {	/* [한국어] 쉼표로 구분된 옵션들을 순회 */
		if (!strncmp(str, "on", 2)) {	/* [한국어] intel_iommu=on */
			dmar_disabled = 0;	/* [한국어] VT-d 를 켠다 */
			pr_info("IOMMU enabled\n");	/* [한국어] 관리자 지시를 로그에 남긴다 */
		} else if (!strncmp(str, "off", 3)) {	/* [한국어] intel_iommu=off */
			dmar_disabled = 1;	/* [한국어] VT-d 를 끈다 */
			no_platform_optin = 1;	/* [한국어] 펌웨어 권장도 무시한다 */
			pr_info("IOMMU disabled\n");	/* [한국어] 기록 */
		} else if (!strncmp(str, "igfx_off", 8)) {	/* [한국어] 통합 그래픽만 제외 */
			disable_igfx_iommu = 1;	/* [한국어] GPU 를 IOMMU 밖에 둔다 */
			pr_info("Disable GFX device mapping\n");	/* [한국어] 기록 */
		} else if (!strncmp(str, "forcedac", 8)) {	/* [한국어] 옛 이름의 DAC 강제 옵션 */
			pr_warn("intel_iommu=forcedac deprecated; use iommu.forcedac instead\n");	/* [한국어] 코어 공통 인자로 옮겨졌다 */
			iommu_dma_forcedac = true;	/* [한국어] 그래도 동작은 시켜 준다 */
		} else if (!strncmp(str, "strict", 6)) {	/* [한국어] 옛 이름의 즉시 무효화 옵션 */
			pr_warn("intel_iommu=strict deprecated; use iommu.strict=1 instead\n");	/* [한국어] 마찬가지로 코어로 옮겨졌다 */
			iommu_set_dma_strict();	/* [한국어] 지연 무효화를 끈다 */
		} else if (!strncmp(str, "sp_off", 6)) {	/* [한국어] 큰 페이지 비활성화 */
			pr_info("Disable supported super page\n");	/* [한국어] 기록 */
			intel_iommu_superpage = 0;	/* [한국어] 2MB/1GB 매핑을 쓰지 않는다 */
		} else if (!strncmp(str, "sm_on", 5)) {	/* [한국어] scalable mode 활성화 */
			pr_info("Enable scalable mode if hardware supports\n");	/* [한국어] PASID·중첩 번역이 가능해진다 */
			intel_iommu_sm = 1;	/* [한국어] 하드웨어가 지원하면 켠다 */
		} else if (!strncmp(str, "sm_off", 6)) {	/* [한국어] scalable mode 금지 */
			pr_info("Scalable mode is disallowed\n");	/* [한국어] 레거시 모드만 쓴다 */
			intel_iommu_sm = 0;	/* [한국어] 끈다 */
		} else if (!strncmp(str, "tboot_noforce", 13)) {	/* [한국어] TXT 부팅에서의 강제를 해제 */
			pr_info("Intel-IOMMU: not forcing on after tboot. This could expose security risk for tboot\n");	/* [한국어] 보안 위험을 명시적으로 알린다 — TXT 로 부팅한 이유가 격리인데 그것을 포기하는 것이기 때문 */
			intel_iommu_tboot_noforce = 1;	/* [한국어] 강제하지 않는다 */
		} else {
			pr_notice("Unknown option - '%s'\n", str);	/* [한국어] 알 수 없는 옵션은 무시하고 알린다 */
		}

		str += strcspn(str, ",");	/* [한국어] 다음 쉼표까지 건너뛴다 */
		while (*str == ',')	/* [한국어] 연속된 쉼표도 */
			str++;	/* [한국어] 넘어간다 */
	}

	return 1;	/* [한국어] __setup 규약: 1 이면 처리됨 */
}
__setup("intel_iommu=", intel_iommu_setup);	/* [한국어] 부트 인자 등록 */

/*
 * Calculate the Supported Adjusted Guest Address Widths of an IOMMU.
 * Refer to 11.4.2 of the VT-d spec for the encoding of each bit of
 * the returned SAGAW.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * __iommu_calculate_sagaw - 이 유닛이 지원하는 주소 폭 비트맵을 구한다
 *
 * @iommu:  대상 DMAR 유닛
 * @return: 지원하는 AGAW 들의 비트맵
 *
 * SAGAW(Supported Adjusted Guest Address Width)는 이 하드웨어가 몇 단계 페이지
 * 테이블을 워크할 수 있는지를 비트로 알린 것이다. 비트 위치가 곧 레벨 수를
 * 나타내며, 레벨이 하나 늘 때마다 주소 폭이 9비트씩 넓어진다.
 *
 * 1단계와 2단계 번역이 각각 다른 폭을 지원할 수 있다는 것이 이 함수의 이유다.
 * scalable mode 에서 둘 다 쓸 수 있으면 교집합을 취하는데, 도메인이 어느 단계로
 * 만들어질지 이 시점에 알 수 없어 양쪽 모두 표현 가능한 폭만 안전하기 때문이다.
 *
 * 실행 컨텍스트: 초기화. 프로세스 문맥.
 *
 * 호출 체인: __iommu_calculate_agaw → [이 함수]
 */
static unsigned long __iommu_calculate_sagaw(struct intel_iommu *iommu)
{
	unsigned long fl_sagaw, sl_sagaw;	/* [한국어] 1단계(first-level)와 2단계(second-level) 페이지 테이블이 각각 지원하는 주소 폭 비트맵 */

	fl_sagaw = BIT(2) | (cap_fl5lp_support(iommu->cap) ? BIT(3) : 0);	/* [한국어] 1단계는 4단계 페이지 테이블(48비트)이 기본이고, 하드웨어가 지원하면 5단계(57비트)도 가능하다. 비트 위치가 곧 레벨 수를 나타낸다 */
	sl_sagaw = cap_sagaw(iommu->cap);	/* [한국어] 2단계는 능력 레지스터가 지원 폭을 직접 알려 준다 */

	/* Second level only. */
	if (!sm_supported(iommu) || !ecap_flts(iommu->ecap))	/* [한국어] scalable mode 가 없거나 1단계 번역을 지원하지 않는다 */
		return sl_sagaw;	/* [한국어] 2단계만 쓸 수 있다 — 레거시 VT-d 의 보통 구성이다 */

	/* First level only. */
	if (!ecap_slts(iommu->ecap))	/* [한국어] 2단계를 지원하지 않는다 */
		return fl_sagaw;	/* [한국어] 1단계만 */

	return fl_sagaw & sl_sagaw;	/* [한국어] 둘 다 쓸 수 있으면 교집합. 도메인이 어느 단계로 만들어질지 미리 알 수 없어, 양쪽 모두 표현 가능한 폭이어야 한다 */
}

/*
 * [한국어]
 * __iommu_calculate_agaw - 지원하는 폭 중 가장 큰 것을 고른다
 *
 * @iommu:   대상 유닛
 * @max_gaw: 시도를 시작할 최대 폭 (비트 수)
 * @return:  고른 AGAW, 지원하는 것이 없으면 음수
 *
 * 큰 것부터 내려오며 첫 지원 값을 고른다. 폭이 넓을수록 페이지 테이블 레벨이
 * 늘어 워크가 깊어지지만, 좁으면 그만큼의 IOVA 공간을 잃는다.
 *
 * 실행 컨텍스트: 초기화.
 *
 * 호출 체인: iommu_calculate_agaw, iommu_calculate_max_sagaw → [이 함수]
 */
static int __iommu_calculate_agaw(struct intel_iommu *iommu, int max_gaw)
{
	unsigned long sagaw;	/* [한국어] 지원 폭 비트맵 */
	int agaw;	/* [한국어] 고를 주소 폭 (AGAW: Adjusted Guest Address Width) */

	sagaw = __iommu_calculate_sagaw(iommu);	/* [한국어] 이 유닛이 지원하는 폭들 */
	for (agaw = width_to_agaw(max_gaw); agaw >= 0; agaw--) {	/* [한국어] 요청한 최대 폭에서 시작해 내려간다 */
		if (test_bit(agaw, &sagaw))	/* [한국어] 이 폭을 지원하면 */
			break;	/* [한국어] 그것을 쓴다 — 지원하는 것 중 가장 큰 폭 */
	}

	return agaw;	/* [한국어] 음수면 지원하는 폭이 하나도 없다는 뜻 */
}

/*
 * Calculate max SAGAW for each iommu.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_calculate_max_sagaw - 이 유닛이 낼 수 있는 최대 주소 폭
 *
 * @iommu:  대상 유닛
 * @return: 최대 AGAW
 *
 * 하드웨어 능력의 상한을 보고할 때 쓴다. 실제 도메인이 쓰는 폭은
 * iommu_calculate_agaw 가 정하며, 그쪽은 기본값에서 시작하므로 더 작을 수 있다.
 *
 * 실행 컨텍스트: 초기화.
 *
 * 호출 체인: dmar.c 의 유닛 초기화 → [이 함수]
 */
int iommu_calculate_max_sagaw(struct intel_iommu *iommu)
{
	return __iommu_calculate_agaw(iommu, MAX_AGAW_WIDTH);	/* [한국어] 이 유닛이 낼 수 있는 최대 폭 (위 영어 주석) */
}

/*
 * calculate agaw for each iommu.
 * "SAGAW" may be different across iommus, use a default agaw, and
 * get a supported less agaw for iommus that don't support the default agaw.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_calculate_agaw - 이 유닛이 실제로 쓸 주소 폭을 정한다
 *
 * @iommu:  대상 유닛
 * @return: AGAW
 *
 * 시스템 기본값(57비트, 5단계)에서 시작해 이 유닛이 지원하는 만큼 낮춘다.
 * 여러 DMAR 유닛의 능력이 다를 수 있고 도메인 하나가 여러 유닛에 걸쳐 설치될 수
 * 있으므로, 결국 그중 가장 낮은 폭에 맞춰지게 된다 (위 영어 주석).
 *
 * 실행 컨텍스트: 초기화.
 *
 * 호출 체인: dmar.c 의 유닛 초기화 → [이 함수]
 */
int iommu_calculate_agaw(struct intel_iommu *iommu)
{
	return __iommu_calculate_agaw(iommu, DEFAULT_DOMAIN_ADDRESS_WIDTH);	/* [한국어] 기본값(57비트)에서 시작해 이 유닛이 지원하는 만큼 낮춘다. 유닛마다 능력이 다를 수 있어 도메인은 그중 가장 낮은 폭에 맞춰진다 (위 영어 주석) */
}

/*
 * [한국어]
 * iommu_paging_structure_coherency - 페이지 테이블 워크가 CPU 캐시를 보는가
 *
 * @iommu:  대상 유닛
 * @return: true 면 일관성이 있어 캐시 플러시가 필요 없다
 *
 * 이 한 줄이 이 드라이버 곳곳의 __iommu_flush_cache 호출을 좌우한다. false 면
 * PTE 나 컨텍스트 항목을 쓸 때마다 clflush 로 메모리에 밀어내야 하고, 그 비용이
 * 매핑 경로에 그대로 얹힌다.
 *
 * scalable mode 와 레거시가 다른 능력 비트를 쓰는 것에 주의할 것 — 하드웨어가
 * 두 모드에서 서로 다른 일관성 보장을 할 수 있기 때문이다.
 *
 * 실행 컨텍스트: 어디서든.
 *
 * 호출 체인: 도메인 생성, __iommu_flush_cache → [이 함수]
 */
static bool iommu_paging_structure_coherency(struct intel_iommu *iommu)
{
	return sm_supported(iommu) ?	/* [한국어] scalable mode 여부에 따라 다른 능력 비트를 본다 */
			ecap_smpwc(iommu->ecap) : ecap_coherent(iommu->ecap);	/* [한국어] 페이지 테이블 워크가 CPU 캐시를 보는가. 보지 않으면 PTE 를 쓸 때마다 clflush 로 밀어내야 한다 — __iommu_flush_cache 가 그 판단에 이 값을 쓴다 */
}

/*
 * [한국어]
 * iommu_context_addr - 이 장치의 컨텍스트 항목 주소를 얻는다 (필요하면 테이블 생성)
 *
 * @iommu:  담당 DMAR 유닛
 * @bus:    PCI 버스 번호
 * @devfn:  장치·함수 번호
 * @alloc:  0 이 아니면 컨텍스트 테이블이 없을 때 만든다
 * @return: 컨텍스트 항목 포인터, 없거나 실패하면 NULL
 *
 * VT-d 의 2단계 인덱싱을 구현한다. 하드웨어가 DMA 요청을 받으면 버스 번호로
 * 루트 테이블을, 장치·함수로 컨텍스트 테이블을 인덱싱하는데, 이 함수가 그 경로를
 * 소프트웨어로 따라간다.
 *
 * scalable mode 의 처리가 이 함수의 절반이다. 그 모드에서는 컨텍스트 항목이
 * 32바이트로 커져 한 페이지에 256개가 들어가지 않으므로, 루트 항목이 하위·상위
 * 두 개의 테이블 주소를 담고 장치 번호 128 을 경계로 나뉜다.
 *
 * context_copied 검사는 kexec 상황을 다룬다. 앞선 커널이 만든 항목을 물려받은
 * 상태에서 그것을 읽기 목적으로 돌려주면, 우리가 설정하지 않은 도메인을 우리
 * 것으로 오해하게 된다 (위 영어 주석).
 *
 * 실행 컨텍스트: 장치 부착 경로. 락 아래일 수 있어 GFP_ATOMIC 를 쓴다.
 *
 * 호출 체인: domain_context_mapping, 컨텍스트 조회 경로 → [이 함수]
 */
struct context_entry *iommu_context_addr(struct intel_iommu *iommu, u8 bus,
					 u8 devfn, int alloc)
{
	struct root_entry *root = &iommu->root_entry[bus];	/* [한국어] 버스 번호로 루트 테이블을 인덱싱한다 */
	struct context_entry *context;	/* [한국어] 그 버스의 컨텍스트 테이블 */
	u64 *entry;	/* [한국어] 루트 항목의 하위 또는 상위 절반 */

	/*
	 * Except that the caller requested to allocate a new entry,
	 * returning a copied context entry makes no sense.
	 */
	if (!alloc && context_copied(iommu, bus, devfn))	/* [한국어] 앞선 커널(kexec 전)이 만든 항목을 그대로 물려받은 상태다. 읽기 목적으로 그것을 돌려주면 우리가 만들지 않은 설정을 우리 것으로 오해하게 된다 (위 영어 주석) */
		return NULL;	/* [한국어] 없는 것으로 취급한다 */

	entry = &root->lo;	/* [한국어] 기본은 하위 절반 (장치 0~127) */
	if (sm_supported(iommu)) {	/* [한국어] scalable mode 는 컨텍스트 항목이 두 배로 커진다 */
		if (devfn >= 0x80) {	/* [한국어] 장치 번호가 128 이상이면 */
			devfn -= 0x80;	/* [한국어] 상위 테이블 안의 인덱스로 바꾸고 */
			entry = &root->hi;	/* [한국어] 루트 항목의 상위 절반을 쓴다. 항목이 커져 한 페이지에 256개가 들어가지 않으므로 테이블을 둘로 나눈다 */
		}
		devfn *= 2;	/* [한국어] scalable mode 의 컨텍스트 항목은 32바이트라 인덱스가 두 배가 된다 */
	}
	if (*entry & 1)	/* [한국어] present 비트 — 컨텍스트 테이블이 이미 있다 */
		context = phys_to_virt(*entry & VTD_PAGE_MASK);	/* [한국어] 그 주소를 가상 주소로 */
	else {
		unsigned long phy_addr;	/* [한국어] 새로 만들 테이블의 물리 주소 */
		if (!alloc)	/* [한국어] 조회만 하는 호출이면 */
			return NULL;	/* [한국어] 만들지 않는다 */

		context = iommu_alloc_pages_node_sz(iommu->node, GFP_ATOMIC,	/* [한국어] 이 유닛과 가까운 NUMA 노드에서. ATOMIC 인 것은 이 경로가 락 아래에서 불릴 수 있기 때문이다 */
						    SZ_4K);	/* [한국어] 컨텍스트 테이블은 한 페이지 */
		if (!context)	/* [한국어] 할당 실패 */
			return NULL;	/* [한국어] 장치를 설정할 수 없다 */

		__iommu_flush_cache(iommu, (void *)context, CONTEXT_SIZE);	/* [한국어] 0 으로 채워진 테이블을 메모리로 밀어낸다. 워크가 비일관인 하드웨어에서 이것을 빠뜨리면 하드웨어가 쓰레기를 유효한 항목으로 읽는다 */
		phy_addr = virt_to_phys((void *)context);	/* [한국어] 하드웨어가 볼 주소 */
		*entry = phy_addr | 1;	/* [한국어] 루트 항목에 심고 present 비트를 세운다 */
		__iommu_flush_cache(iommu, entry, sizeof(*entry));	/* [한국어] 그 항목도 밀어낸다. 순서가 중요하다 — 테이블 내용이 먼저 보이고 그것을 가리키는 항목이 나중에 보여야 한다 */
	}
	return &context[devfn];	/* [한국어] 이 장치의 컨텍스트 항목. 여기에 도메인과 페이지 테이블 루트를 기입하면 하드웨어가 그 장치의 DMA 를 번역하기 시작한다 */
}

/**
 * is_downstream_to_pci_bridge - test if a device belongs to the PCI
 *				 sub-hierarchy of a candidate PCI-PCI bridge
 * @dev: candidate PCI device belonging to @bridge PCI sub-hierarchy
 * @bridge: the candidate PCI-PCI bridge
 *
 * Return: true if @dev belongs to @bridge PCI sub-hierarchy, else false.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * is_downstream_to_pci_bridge - 장치가 이 브리지의 하위 계층에 속하는가
 *
 * @dev:    후보 장치
 * @bridge: 후보 브리지
 * @return: 하위에 속하면 true
 *
 * ACPI DMAR 표는 브리지 하나를 적어 그 아래 전체를 한 유닛에 맡길 수 있다.
 * 그 기술을 해석하려면 "이 장치가 저 브리지 아래인가"를 판정해야 하는데, PCI 가
 * 브리지 아래의 버스 번호를 연속 범위로 할당하기 때문에 번호 비교만으로 답이 난다.
 *
 * 실행 컨텍스트: 유닛 조회 경로. RCU 읽기 구간.
 *
 * 호출 체인: device_lookup_iommu → [이 함수]
 */
static bool
is_downstream_to_pci_bridge(struct device *dev, struct device *bridge)
{
	struct pci_dev *pdev, *pbridge;	/* [한국어] PCI 형으로 변환한 두 장치 */

	if (!dev_is_pci(dev) || !dev_is_pci(bridge))	/* [한국어] 둘 중 하나라도 PCI 가 아니면 */
		return false;	/* [한국어] 계층 관계를 논할 수 없다 */

	pdev = to_pci_dev(dev);	/* [한국어] 후보 장치 */
	pbridge = to_pci_dev(bridge);	/* [한국어] 후보 브리지 */

	if (pbridge->subordinate &&	/* [한국어] 브리지가 실제로 하위 버스를 가지고 있고 */
	    pbridge->subordinate->number <= pdev->bus->number &&	/* [한국어] 장치의 버스 번호가 그 하위 버스 범위 안에 들면 */
	    pbridge->subordinate->busn_res.end >= pdev->bus->number)	/* [한국어] PCI 는 브리지 아래의 버스 번호를 연속 범위로 할당하므로, 번호 비교만으로 계층 소속을 알 수 있다 */
		return true;	/* [한국어] 이 브리지 아래에 있다 */

	return false;	/* [한국어] 아니다 */
}

/*
 * [한국어]
 * quirk_ioat_snb_local_iommu - Sandy Bridge QuickData 장치의 BIOS 오신고를 잡는다
 *
 * @pdev:   확인할 QuickData(IOAT) DMA 엔진
 * @return: BIOS 가 잘못 신고했으면 true
 *
 * 이 칩셋에서 QuickData 엔진의 IOMMU 는 반드시 호스트 브리지가 알려 주는
 * vtbar + 0xa000 에 있다. BIOS 가 다른 유닛을 지정했다면 그것은 거짓이고, 그
 * 유닛에 이 장치를 붙이면 번역이 엉뚱한 페이지 테이블로 간다.
 *
 * 해결책이 "IOMMU 없이 쓴다"인 것이 이 우회의 성격을 말해 준다 — 올바른 유닛을
 * 찾을 방법이 없으므로, 잘못된 곳에 붙이느니 번역을 포기한다. 위 영어 주석이
 * "그 IOMMU 가 실제로 꺼져 있기를 바란다"고 적은 그대로다.
 *
 * add_taint 로 커널에 오염 표시를 남기는 것도 의도적이다. 이후 문제 보고를 받는
 * 쪽이 펌웨어 결함이 있었음을 알 수 있게 한다.
 *
 * 실행 컨텍스트: 유닛 조회 경로.
 *
 * 호출 체인: iommu_is_dummy → [이 함수]
 */
static bool quirk_ioat_snb_local_iommu(struct pci_dev *pdev)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 이 장치를 맡는다고 신고된 유닛 */
	u32 vtbar;	/* [한국어] 칩셋이 실제로 알려 주는 VT-d 레지스터 기준 주소 */
	int rc;	/* [한국어] 설정 공간 읽기 결과 */

	/* We know that this device on this chipset has its own IOMMU.
	 * If we find it under a different IOMMU, then the BIOS is lying
	 * to us. Hope that the IOMMU for this device is actually
	 * disabled, and it needs no translation...
	 */
	rc = pci_bus_read_config_dword(pdev->bus, PCI_DEVFN(0, 0), 0xb0, &vtbar);	/* [한국어] 호스트 브리지의 비공개 레지스터에서 VT-d 기준 주소를 읽는다. BIOS 가 아니라 하드웨어에게 직접 묻는 것이 이 우회의 요점이다 */
	if (rc) {	/* [한국어] 읽기 실패 */
		/* "can't" happen */
		dev_info(&pdev->dev, "failed to run vt-d quirk\n");	/* [한국어] 일어날 수 없는 일이지만 방어한다 (위 영어 주석) */
		return false;	/* [한국어] 우회를 적용하지 않는다 */
	}
	vtbar &= 0xffff0000;	/* [한국어] 기준 주소 필드만 남긴다 */

	/* we know that the this iommu should be at offset 0xa000 from vtbar */
	drhd = dmar_find_matched_drhd_unit(pdev);	/* [한국어] BIOS 가 신고한 담당 유닛 */
	if (!drhd || drhd->reg_base_addr - vtbar != 0xa000) {	/* [한국어] 이 칩셋에서 QuickData 장치의 IOMMU 는 반드시 vtbar + 0xa000 에 있다. 다르다면 BIOS 가 거짓말을 한 것이다 (위 영어 주석) */
		pr_warn_once(FW_BUG "BIOS assigned incorrect VT-d unit for Intel(R) QuickData Technology device\n");	/* [한국어] 펌웨어 버그임을 명시한다 */
		add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);	/* [한국어] 커널에 오염 표시를 남긴다 — 이후 버그 보고를 받는 쪽이 펌웨어 문제였음을 알 수 있게 */
		return true;	/* [한국어] 이 장치는 IOMMU 없이 다룬다. 잘못된 유닛에 붙이느니 번역을 포기하는 편이 낫다 */
	}

	return false;	/* [한국어] BIOS 신고가 맞다 */
}

/*
 * [한국어]
 * iommu_is_dummy - 이 유닛을 실제로 쓸 수 없는 경우인가
 *
 * @iommu:  찾은 유닛 (NULL 일 수 있다)
 * @dev:    대상 장치
 * @return: 쓸 수 없으면 true
 *
 * 두 가지를 거른다. DMAR 표가 무시하도록 표시한 유닛(그래픽 전용 유닛을 끈
 * 경우 등)과, BIOS 오신고로 잘못 연결된 장치다.
 *
 * true 를 돌려주면 그 장치는 IOMMU 아래에 들어가지 않고 직접 매핑으로 남는다.
 * 격리를 잃지만 동작은 한다는 선택이다.
 *
 * 실행 컨텍스트: 유닛 조회 경로.
 *
 * 호출 체인: device_lookup_iommu → [이 함수]
 */
static bool iommu_is_dummy(struct intel_iommu *iommu, struct device *dev)
{
	if (!iommu || iommu->drhd->ignored)	/* [한국어] 유닛이 없거나 무시하도록 표시된 유닛이면 */
		return true;	/* [한국어] 이 장치는 IOMMU 아래에 두지 않는다 */

	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치면 */
		struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] PCI 형으로 */

		if (pdev->vendor == PCI_VENDOR_ID_INTEL &&	/* [한국어] 인텔 장치이고 */
		    pdev->device == PCI_DEVICE_ID_INTEL_IOAT_SNB &&	/* [한국어] Sandy Bridge 의 QuickData(IOAT) DMA 엔진이며 */
		    quirk_ioat_snb_local_iommu(pdev))	/* [한국어] BIOS 가 잘못된 유닛을 지정했다면 */
			return true;	/* [한국어] 가짜 유닛으로 취급해 IOMMU 밖에 둔다 */
	}

	return false;	/* [한국어] 정상적인 장치 */
}

/*
 * [한국어]
 * device_lookup_iommu - 이 장치를 담당하는 DMAR 유닛과 소스 id 를 찾는다
 *
 * @dev:    대상 장치
 * @bus:    찾은 버스 번호를 담을 자리 (NULL 가능)
 * @devfn:  찾은 장치·함수를 담을 자리 (NULL 가능)
 * @return: 담당 유닛, 없으면 NULL
 *
 * 이 드라이버의 프로브가 시작되는 지점이다. 인텔 시스템에는 DMAR 유닛이 여럿
 * 있을 수 있고(소켓마다, 또는 그래픽 전용으로), ACPI DMAR 표가 어느 유닛이 어느
 * 장치를 담당하는지 기술한다.
 *
 * 표를 해석하는 데 네 가지 경우가 있다.
 *  - 장치가 범위 표에 직접 적혀 있다 (가장 단순).
 *  - 브리지가 적혀 있고 장치가 그 아래에 있다 (하위 계층 전체를 위임).
 *  - 유닛이 include_all 로 "나머지 전부"를 담당한다고 신고했다.
 *  - 어디에도 없다 → IOMMU 아래가 아니다.
 *
 * 장치 정규화가 앞부분의 절반을 차지한다. VF 는 범위 표에 나열되지 않아 PF 로
 * 찾아야 하고(다만 BDF 는 자기 것을 쓴다), 플랫폼 장치는 ACPI 노드로 기술되며,
 * 일부 장치는 다른 함수의 이름으로 DMA 를 낸다.
 *
 * 실행 컨텍스트: 장치 프로브. RCU 읽기 구간 — 유닛이 핫플러그로 추가될 수 있다.
 *
 * 호출 체인: intel_iommu_probe_device → [이 함수]
 */
static struct intel_iommu *device_lookup_iommu(struct device *dev, u8 *bus, u8 *devfn)
{
	struct dmar_drhd_unit *drhd = NULL;	/* [한국어] 유닛 순회 커서 */
	struct pci_dev *pdev = NULL;	/* [한국어] PCI 장치면 여기에 */
	struct intel_iommu *iommu;	/* [한국어] 찾은 유닛 */
	struct device *tmp;	/* [한국어] 범위 표의 장치 순회 커서 */
	u16 segment = 0;	/* [한국어] PCI 세그먼트(도메인) 번호. 대형 시스템은 세그먼트가 여럿이다 */
	int i;	/* [한국어] 범위 표 인덱스 */

	if (!dev)	/* [한국어] 장치가 없다 */
		return NULL;	/* [한국어] 찾을 수 없다 */

	if (dev_is_pci(dev)) {
		struct pci_dev *pf_pdev;	/* [한국어] 물리 함수(PF) */

		pdev = pci_real_dma_dev(to_pci_dev(dev));	/* [한국어] DMA 를 실제로 내는 장치. 일부 장치는 다른 함수의 이름으로 DMA 를 낸다 */

		/* VFs aren't listed in scope tables; we need to look up
		 * the PF instead to find the IOMMU. */
		pf_pdev = pci_physfn(pdev);	/* [한국어] VF 라면 그 PF 를 찾는다 */
		dev = &pf_pdev->dev;	/* [한국어] ACPI 범위 표에는 VF 가 나열되지 않으므로 PF 로 찾아야 한다 (위 영어 주석) */
		segment = pci_domain_nr(pdev->bus);	/* [한국어] 이 장치가 속한 PCI 세그먼트 */
	} else if (has_acpi_companion(dev))	/* [한국어] 플랫폼 장치인데 ACPI 노드가 있으면 */
		dev = &ACPI_COMPANION(dev)->dev;	/* [한국어] 범위 표는 ACPI 장치로 기술되므로 그것으로 바꿔 비교한다 */

	rcu_read_lock();	/* [한국어] 유닛 목록은 RCU 로 보호된다 — 핫플러그로 유닛이 추가될 수 있다 */
	for_each_iommu(iommu, drhd) {	/* [한국어] 모든 DMAR 유닛에 대해 */
		if (pdev && segment != drhd->segment)	/* [한국어] 다른 PCI 세그먼트의 유닛 */
			continue;	/* [한국어] 건너뛴다 */

		for_each_active_dev_scope(drhd->devices,	/* [한국어] 이 유닛이 담당한다고 신고된 장치들을 */
					  drhd->devices_cnt, i, tmp) {	/* [한국어] 하나씩 */
			if (tmp == dev) {	/* [한국어] 우리 장치를 찾았다 */
				/* For a VF use its original BDF# not that of the PF
				 * which we used for the IOMMU lookup. Strictly speaking
				 * we could do this for all PCI devices; we only need to
				 * get the BDF# from the scope table for ACPI matches. */
				if (pdev && pdev->is_virtfn)	/* [한국어] VF 는 PF 로 찾았으므로 BDF 는 자기 것을 써야 한다 (위 영어 주석) */
					goto got_pdev;	/* [한국어] 아래에서 자기 BDF 를 채운다 */

				if (bus && devfn) {	/* [한국어] 호출자가 BDF 를 원하면 */
					*bus = drhd->devices[i].bus;	/* [한국어] 범위 표가 기술한 버스 번호 */
					*devfn = drhd->devices[i].devfn;	/* [한국어] 그리고 장치·함수. ACPI 장치는 이 값으로만 알 수 있다 */
				}
				goto out;	/* [한국어] 찾았다 */
			}

			if (is_downstream_to_pci_bridge(dev, tmp))	/* [한국어] 범위 표에 브리지가 적혀 있고 우리 장치가 그 아래면 */
				goto got_pdev;	/* [한국어] 그 유닛이 담당한다 — 브리지 아래 전체를 한 항목으로 기술하는 방식이다 */
		}

		if (pdev && drhd->include_all) {	/* [한국어] 이 유닛이 '나머지 전부'를 담당한다고 신고했다 */
got_pdev:	/* [한국어] 브리지 하위와 VF 경로가 합류 */
			if (bus && devfn) {	/* [한국어] 호출자가 BDF 를 원하면 */
				*bus = pdev->bus->number;	/* [한국어] 장치 자신의 버스 번호 */
				*devfn = pdev->devfn;	/* [한국어] 그리고 장치·함수 */
			}
			goto out;	/* [한국어] 찾았다 */
		}
	}
	iommu = NULL;	/* [한국어] 어느 유닛도 이 장치를 담당하지 않는다 */
out:	/* [한국어] 공통 출구 */
	if (iommu_is_dummy(iommu, dev))	/* [한국어] 무시 표시된 유닛이거나 BIOS 결함 우회 대상이면 */
		iommu = NULL;	/* [한국어] 없는 것으로 취급한다 */

	rcu_read_unlock();	/* [한국어] 목록 순회 끝 */

	return iommu;	/* [한국어] NULL 이면 이 장치는 IOMMU 아래가 아니다 */
}

static void free_context_table(struct intel_iommu *iommu)
{
	struct context_entry *context;	/* [한국어] 해제할 컨텍스트 테이블 */
	int i;	/* [한국어] 버스 번호 순회 */

	if (!iommu->root_entry)	/* [한국어] 루트 테이블이 없으면 컨텍스트 테이블도 없다 */
		return;

	for (i = 0; i < ROOT_ENTRY_NR; i++) {	/* [한국어] 256개 버스 각각에 대해 */
		context = iommu_context_addr(iommu, i, 0, 0);	/* [한국어] 하위 컨텍스트 테이블 (장치 0~127) */
		if (context)	/* [한국어] 있으면 */
			iommu_free_pages(context);

		if (!sm_supported(iommu))	/* [한국어] 레거시 모드는 테이블이 하나뿐이다 */
			continue;

		context = iommu_context_addr(iommu, i, 0x80, 0);	/* [한국어] scalable mode 의 상위 테이블 (장치 128~255) */
		if (context)	/* [한국어] 있으면 */
			iommu_free_pages(context);
	}

	iommu_free_pages(iommu->root_entry);	/* [한국어] 마지막으로 루트 테이블 */
	iommu->root_entry = NULL;	/* [한국어] 두 번 해제되지 않도록 */
}

#ifdef CONFIG_DMAR_DEBUG	/* [한국어] 폴트 진단이 켜진 빌드에서만 */
static void pgtable_walk(struct intel_iommu *iommu, unsigned long pfn,
			 u8 bus, u8 devfn, struct dma_pte *parent, int level)
{
	struct dma_pte *pte;	/* [한국어] 현재 레벨의 항목 */
	int offset;	/* [한국어] 그 레벨의 인덱스 */

	while (1) {	/* [한국어] 잎에 닿거나 무효 항목을 만날 때까지 */
		offset = pfn_level_offset(pfn, level);	/* [한국어] 이 레벨에서 주소가 쓰는 인덱스 */
		pte = &parent[offset];	/* [한국어] 해당 항목 */

		pr_info("pte level: %d, pte value: 0x%016llx\n", level, pte->val);	/* [한국어] 각 레벨의 서술자를 그대로 찍는다. 폴트 원인을 짚으려면 어느 레벨에서 끊겼는지, 권한 비트가 무엇이었는지를 봐야 한다 */

		if (!dma_pte_present(pte)) {	/* [한국어] 유효하지 않은 항목 */
			pr_info("page table not present at level %d\n", level - 1);	/* [한국어] 여기서 번역이 끊겼다 — 폴트의 직접 원인이다 */
			break;
		}

		if (level == 1 || dma_pte_superpage(pte))	/* [한국어] 마지막 레벨이거나 큰 페이지면 더 내려갈 곳이 없다 */
			break;

		parent = phys_to_virt(dma_pte_addr(pte));	/* [한국어] 다음 레벨 테이블로 */
		level--;	/* [한국어] 한 단계 내려간다 */
	}
}

void dmar_fault_dump_ptes(struct intel_iommu *iommu, u16 source_id,
			  unsigned long long addr, u32 pasid)
{
	struct pasid_dir_entry *dir, *pde;	/* [한국어] PASID 디렉터리와 그 항목 */
	struct pasid_entry *entries, *pte;	/* [한국어] PASID 테이블과 그 항목 */
	struct context_entry *ctx_entry;	/* [한국어] 컨텍스트 항목 */
	struct root_entry *rt_entry;	/* [한국어] 루트 항목 */
	int i, dir_index, index, level;	/* [한국어] 순회 커서와 인덱스, 페이지 테이블 레벨 */
	u8 devfn = source_id & 0xff;	/* [한국어] 소스 id 의 하위 8비트가 장치·함수 */
	u8 bus = source_id >> 8;	/* [한국어] 상위 8비트가 버스 번호 */
	struct dma_pte *pgtable;	/* [한국어] 최종적으로 워크할 페이지 테이블 */

	pr_info("Dump %s table entries for IOVA 0x%llx\n", iommu->name, addr);	/* [한국어] 어느 유닛의 어느 주소에서 폴트가 났는지 */

	/* root entry dump */
	if (!iommu->root_entry) {	/* [한국어] 루트 테이블조차 없다 */
		pr_info("root table is not present\n");	/* [한국어] 번역이 설정되지 않은 상태에서 DMA 가 왔다 */
		return;
	}
	rt_entry = &iommu->root_entry[bus];	/* [한국어] 이 버스의 루트 항목 */

	if (sm_supported(iommu))	/* [한국어] scalable mode 는 항목이 두 배라 상위·하위를 함께 찍는다 */
		pr_info("scalable mode root entry: hi 0x%016llx, low 0x%016llx\n",
			rt_entry->hi, rt_entry->lo);
	else
		pr_info("root entry: 0x%016llx", rt_entry->lo);

	/* context entry dump */
	ctx_entry = iommu_context_addr(iommu, bus, devfn, 0);	/* [한국어] 이 장치의 컨텍스트 항목 */
	if (!ctx_entry) {	/* [한국어] 컨텍스트 테이블이 없다 */
		pr_info("context table is not present\n");	/* [한국어] 이 버스의 어떤 장치도 설정되지 않았다 */
		return;
	}

	pr_info("context entry: hi 0x%016llx, low 0x%016llx\n",	/* [한국어] 컨텍스트 항목의 원본 값. 도메인 id 와 페이지 테이블 루트가 여기 들어 있다 */
		ctx_entry->hi, ctx_entry->lo);

	/* legacy mode does not require PASID entries */
	if (!sm_supported(iommu)) {	/* [한국어] 레거시 모드 */
		if (!context_present(ctx_entry)) {	/* [한국어] 이 장치의 컨텍스트가 설정되지 않았다 */
			pr_info("legacy mode page table is not present\n");	/* [한국어] 장치가 IOMMU 에 등록되기 전에 DMA 를 냈다는 뜻 */
			return;
		}
		level = agaw_to_level(ctx_entry->hi & 7);	/* [한국어] 컨텍스트 항목이 페이지 테이블 레벨 수를 담고 있다 */
		pgtable = phys_to_virt(ctx_entry->lo & VTD_PAGE_MASK);	/* [한국어] 그리고 그 루트 주소 */
		goto pgtable_walk;	/* [한국어] 페이지 테이블 워크로 */
	}

	if (!context_present(ctx_entry)) {	/* [한국어] scalable mode 에서 컨텍스트가 없다 */
		pr_info("pasid directory table is not present\n");	/* [한국어] PASID 디렉터리로 가는 길이 없다 */
		return;
	}

	/* get the pointer to pasid directory entry */
	dir = phys_to_virt(ctx_entry->lo & VTD_PAGE_MASK);	/* [한국어] scalable mode 의 컨텍스트 항목은 페이지 테이블이 아니라 PASID 디렉터리를 가리킨다 */

	/* For request-without-pasid, get the pasid from context entry */
	if (intel_iommu_sm && pasid == IOMMU_PASID_INVALID)	/* [한국어] PASID 없는 요청이면 */
		pasid = IOMMU_NO_PASID;	/* [한국어] PASID 0 을 쓴다 — scalable mode 는 RID 트래픽도 PASID 표를 거치며, 그 자리가 0 번이다 */

	dir_index = pasid >> PASID_PDE_SHIFT;	/* [한국어] PASID 의 상위 비트가 디렉터리 인덱스 */
	pde = &dir[dir_index];	/* [한국어] 디렉터리 항목 */
	pr_info("pasid dir entry: 0x%016llx\n", pde->val);	/* [한국어] 그 값 */

	/* get the pointer to the pasid table entry */
	entries = get_pasid_table_from_pde(pde);	/* [한국어] 디렉터리 항목이 가리키는 PASID 테이블 */
	if (!entries) {	/* [한국어] 없다 */
		pr_info("pasid table is not present\n");	/* [한국어] 이 PASID 범위가 설정되지 않았다 */
		return;
	}
	index = pasid & PASID_PTE_MASK;	/* [한국어] PASID 의 하위 비트가 테이블 인덱스 */
	pte = &entries[index];	/* [한국어] 이 PASID 의 항목 */
	for (i = 0; i < ARRAY_SIZE(pte->val); i++)	/* [한국어] PASID 항목은 여러 워드로 이루어져 있다 */
		pr_info("pasid table entry[%d]: 0x%016llx\n", i, pte->val[i]);	/* [한국어] 전부 찍는다 — 어느 워드의 어느 비트가 잘못되었는지 봐야 하므로 */

	if (!pasid_pte_is_present(pte)) {	/* [한국어] 이 PASID 가 설정되지 않았다 */
		pr_info("scalable mode page table is not present\n");	/* [한국어] SVA 바인딩 전에 그 PASID 로 DMA 가 왔다는 뜻 */
		return;
	}

	if (pasid_pte_get_pgtt(pte) == PASID_ENTRY_PGTT_FL_ONLY) {	/* [한국어] 1단계 번역만 쓰는 경우 (SVA 나 커널 DMA) */
		level = pte->val[2] & BIT_ULL(2) ? 5 : 4;	/* [한국어] 5단계인지 4단계인지 */
		pgtable = phys_to_virt(pte->val[2] & VTD_PAGE_MASK);	/* [한국어] 1단계 페이지 테이블 루트 */
	} else {
		level = agaw_to_level((pte->val[0] >> 2) & 0x7);	/* [한국어] 2단계 번역 — 레벨이 AGAW 로 인코딩되어 있다 */
		pgtable = phys_to_virt(pte->val[0] & VTD_PAGE_MASK);	/* [한국어] 2단계 페이지 테이블 루트 */
	}

pgtable_walk:	/* [한국어] 레거시와 scalable 경로가 합류 */
	pgtable_walk(iommu, addr >> VTD_PAGE_SHIFT, bus, devfn, pgtable, level);	/* [한국어] 페이지 테이블을 레벨별로 찍는다 */
}
#endif

/* iommu handling */
static int iommu_alloc_root_entry(struct intel_iommu *iommu)
{
	struct root_entry *root;	/* [한국어] 만들 루트 테이블 */

	root = iommu_alloc_pages_node_sz(iommu->node, GFP_ATOMIC, SZ_4K);	/* [한국어] 이 유닛과 가까운 노드에서 한 페이지. 256개 항목 × 16바이트가 정확히 한 페이지다 */
	if (!root) {	/* [한국어] 할당 실패 */
		pr_err("Allocating root entry for %s failed\n",	/* [한국어] 이 유닛을 쓸 수 없다 */
			iommu->name);	/* [한국어] 어느 유닛인지 */
		return -ENOMEM;	/* [한국어] 초기화 실패 */
	}

	__iommu_flush_cache(iommu, root, ROOT_SIZE);	/* [한국어] 0 으로 채워진 테이블을 메모리로 밀어낸다 */
	iommu->root_entry = root;	/* [한국어] 유닛에 매단다 */

	return 0;	/* [한국어] 루트 테이블 준비 완료 */
}

/*
 * [한국어]
 * iommu_set_root_entry - 루트 테이블 주소를 하드웨어에 알리고 캐시를 비운다
 *
 * @iommu: 대상 DMAR 유닛
 *
 * 이 함수가 돌아온 뒤부터 하드웨어가 이 커널의 테이블을 워크한다. 주소를 쓰는
 * 것 자체는 두 줄이고, 나머지가 그 전환을 안전하게 만드는 일이다.
 *
 * 주소 하위 비트에 SMT(Scalable Mode Translation)를 얹는 것이 결정적이다. 그 한
 * 비트가 컨텍스트 항목의 해석을 통째로 바꾼다 — 레거시면 페이지 테이블 루트를,
 * scalable 이면 PASID 디렉터리를 가리키는 것으로 읽는다.
 *
 * 세 캐시(컨텍스트, PASID, IOTLB)를 모두 비우는 이유는 옛 테이블을 통해 캐시된
 * 내용이 남아 있으면 안 되기 때문이다. Enhanced SRTP 를 지원하는 하드웨어는
 * 그것을 스스로 하므로 건너뛴다 (위 영어 주석).
 *
 * 실행 컨텍스트: 유닛 초기화. 레지스터 락을 잡는다.
 *
 * 호출 체인: init_dmars, 리쥼 경로 → [이 함수]
 */
static void iommu_set_root_entry(struct intel_iommu *iommu)
{
	u64 addr;	/* [한국어] 루트 테이블의 물리 주소 + 모드 비트 */
	u32 sts;	/* [한국어] 상태 레지스터 읽기용 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 */

	addr = virt_to_phys(iommu->root_entry);	/* [한국어] 하드웨어가 볼 주소 */
	if (sm_supported(iommu))	/* [한국어] scalable mode 로 동작시킬 것이면 */
		addr |= DMA_RTADDR_SMT;	/* [한국어] 주소 하위 비트에 모드 표시를 얹는다. 이 비트 하나가 컨텍스트 항목의 해석 자체를 바꾼다 — 레거시면 페이지 테이블 루트, scalable 이면 PASID 디렉터리 */

	raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근은 이 락으로 직렬화한다. raw 판인 것은 RT 커널에서도 이 구간이 선점되면 안 되기 때문이다 */
	writeq(addr, iommu->reg + DMAR_RTADDR_REG);	/* [한국어] 루트 테이블 주소를 알린다 */

	writel(iommu->gcmd | DMA_GCMD_SRTP, iommu->reg + DMAR_GCMD_REG);	/* [한국어] Set Root Table Pointer 명령. 전역 명령 레지스터는 한 번에 하나의 명령만 받으므로 기존 gcmd 값을 함께 써야 다른 설정이 꺼지지 않는다 */

	/* Make sure hardware complete it */
	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] 완료를 기다린다 */
		      readl, (sts & DMA_GSTS_RTPS), sts);	/* [한국어] Root Table Pointer Status 가 설 때까지 폴링한다 */

	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 끝 */

	/*
	 * Hardware invalidates all DMA remapping hardware translation
	 * caches as part of SRTP flow.
	 */
	if (cap_esrtps(iommu->cap))	/* [한국어] Enhanced SRTP 를 지원하는 하드웨어는 이 명령만으로 모든 캐시를 비운다 (위 영어 주석) */
		return;	/* [한국어] 추가 무효화가 불필요하다 */

	iommu->flush.flush_context(iommu, 0, 0, 0, DMA_CCMD_GLOBAL_INVL);	/* [한국어] 컨텍스트 캐시 전체 무효화. 옛 루트 테이블을 통해 캐시된 장치-도메인 대응이 남아 있으면 안 된다 */
	if (sm_supported(iommu))	/* [한국어] scalable mode 면 */
		qi_flush_pasid_cache(iommu, 0, QI_PC_GLOBAL, 0);	/* [한국어] PASID 캐시도 비운다 */
	iommu->flush.flush_iotlb(iommu, 0, 0, 0, DMA_TLB_GLOBAL_FLUSH);	/* [한국어] IOTLB 전체 무효화. 세 캐시를 모두 비워야 새 테이블이 온전히 적용된다 */
}

/*
 * [한국어]
 * iommu_flush_write_buffer - 하드웨어 내부 쓰기 버퍼를 비운다
 *
 * @iommu: 대상 유닛
 *
 * 일부 구형 VT-d 하드웨어는 소프트웨어가 쓴 페이지 테이블이 내부 쓰기 버퍼에
 * 머물러, 워커가 그것을 보지 못하는 결함이 있다. 매핑을 만든 뒤 이 명령으로
 * 버퍼를 비워야 새 PTE 가 실제로 반영된다.
 *
 * cap_rwbf 로 하드웨어가 스스로 요구하는 경우와, DMI 로 특정 보드를 식별해
 * 켜는 rwbf_quirk 두 경로가 있다.
 *
 * 실행 컨텍스트: 매핑 경로. 레지스터 락을 잡는다.
 *
 * 호출 체인: 매핑 후 무효화 경로 → [이 함수]
 */
void iommu_flush_write_buffer(struct intel_iommu *iommu)
{
	u32 val;	/* [한국어] 상태 레지스터 읽기용 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 */

	if (!rwbf_quirk && !cap_rwbf(iommu->cap))	/* [한국어] 이 플러시가 필요한 하드웨어가 아니면 */
		return;	/* [한국어] 할 일이 없다 */

	raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 직렬화 */
	writel(iommu->gcmd | DMA_GCMD_WBF, iommu->reg + DMAR_GCMD_REG);	/* [한국어] Write Buffer Flush 명령. 일부 구형 하드웨어는 페이지 테이블 기입이 내부 쓰기 버퍼에 머물 수 있어, 명시적으로 비워야 워커가 그것을 본다 */

	/* Make sure hardware complete it */
	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] 완료 대기 */
		      readl, (!(val & DMA_GSTS_WBFS)), val);	/* [한국어] Write Buffer Flush Status 가 내려갈 때까지 — 다른 완료 대기와 달리 비트가 '지워지기를' 기다린다 */

	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 끝 */
}

/* return value determine if we need a write buffer flush */
/*
 * [한국어] (위 영어 주석에 이어)
 * __iommu_flush_context - 컨텍스트 캐시를 무효화한다 (레지스터 방식)
 *
 * @iommu:         대상 유닛
 * @did:           도메인 id (범위가 도메인 이하일 때)
 * @source_id:     장치의 BDF (장치 단위일 때)
 * @function_mask: 여러 함수를 한 번에 다룰 마스크
 * @type:          무효화 범위
 *
 * 컨텍스트 캐시는 하드웨어가 "이 소스 id 는 이 도메인, 이 페이지 테이블"이라는
 * 대응을 기억해 둔 것이다. 장치를 다른 도메인으로 옮기거나 컨텍스트 항목을
 * 바꾸면 반드시 비워야 하며, 그러지 않으면 하드웨어가 옛 페이지 테이블을 계속
 * 워크한다.
 *
 * 범위가 셋인 이유는 비용 때문이다. 전역 무효화는 시스템의 모든 장치가 다시
 * 컨텍스트를 읽게 만들어 일시적인 성능 저하를 낳으므로, 가능하면 도메인이나
 * 장치 단위로 좁힌다.
 *
 * 완료를 폴링으로 기다리는 것이 이 방식의 비용이다. 무효화 큐(QI)를 지원하는
 * 하드웨어는 이 함수 대신 큐에 명령을 넣어 그 대기를 없앤다.
 *
 * 실행 컨텍스트: 부착/해제 경로. 레지스터 락을 잡는다.
 *
 * 호출 체인: iommu->flush.flush_context == [이 함수] (레지스터 방식일 때)
 */
static void __iommu_flush_context(struct intel_iommu *iommu,
				  u16 did, u16 source_id, u8 function_mask,
				  u64 type)
{
	u64 val = 0;	/* [한국어] 명령 레지스터에 쓸 값 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 */

	switch (type) {	/* [한국어] 무효화 범위에 따라 명령이 다르다 */
	case DMA_CCMD_GLOBAL_INVL:	/* [한국어] 전역 — 모든 장치의 컨텍스트 캐시 */
		val = DMA_CCMD_GLOBAL_INVL;	/* [한국어] 추가 인자가 없다 */
		break;
	case DMA_CCMD_DOMAIN_INVL:	/* [한국어] 도메인 단위 — 그 도메인에 속한 장치들만 */
		val = DMA_CCMD_DOMAIN_INVL|DMA_CCMD_DID(did);	/* [한국어] 도메인 id 를 함께 싣는다 */
		break;
	case DMA_CCMD_DEVICE_INVL:	/* [한국어] 장치 단위 — 가장 좁은 범위 */
		val = DMA_CCMD_DEVICE_INVL|DMA_CCMD_DID(did)	/* [한국어] 도메인 id 와 */
			| DMA_CCMD_SID(source_id) | DMA_CCMD_FM(function_mask);	/* [한국어] 소스 id, 그리고 함수 마스크. 마스크로 여러 함수를 한 번에 무효화할 수 있다 */
		break;
	default:
		pr_warn("%s: Unexpected context-cache invalidation type 0x%llx\n",	/* [한국어] 알 수 없는 종류 — 호출자 버그 */
			iommu->name, type);	/* [한국어] 어느 유닛의 어떤 요청이었는지 */
		return;
	}
	val |= DMA_CCMD_ICC;	/* [한국어] Invalidate Context Cache 비트. 이것을 쓰면 하드웨어가 작업을 시작하고, 끝나면 스스로 지운다 */

	raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 직렬화 */
	writeq(val, iommu->reg + DMAR_CCMD_REG);	/* [한국어] 명령을 낸다 */

	/* Make sure hardware complete it */
	IOMMU_WAIT_OP(iommu, DMAR_CCMD_REG,	/* [한국어] 완료를 기다린다 */
		readq, (!(val & DMA_CCMD_ICC)), val);	/* [한국어] ICC 비트가 하드웨어에 의해 지워질 때까지 폴링. 레지스터 방식 무효화가 느린 이유가 이 대기이며, 무효화 큐(QI)가 그것을 없애려고 도입되었다 */

	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 끝 */
}

/*
 * [한국어]
 * __iommu_flush_iotlb - IOTLB 를 무효화한다 (레지스터 방식)
 *
 * @iommu:      대상 유닛
 * @did:        도메인 id
 * @addr:       무효화할 주소 (페이지 선택 방식일 때)
 * @size_order: 그 범위의 크기 로그값
 * @type:       무효화 범위 (전역/도메인/페이지)
 *
 * IOTLB 는 IOVA→물리 번역의 캐시다. 매핑을 지운 뒤 이것을 비우기 전까지 장치는
 * 여전히 옛 물리 페이지에 닿을 수 있으므로, 그 페이지를 반납하려면 반드시
 * 거쳐야 하는 관문이다.
 *
 * 세 범위 중 도메인 단위(DSI)가 실무에서 가장 중요하다. dma-iommu 의 flush queue
 * 가 수천 건의 해제를 모았다가 이 명령 한 번으로 정리하며, 그것이 고성능 장치에서
 * IOMMU 를 켜도 처리량이 버티는 이유다.
 *
 * 페이지 선택(PSI)은 주소와 크기를 한 레지스터에 담는데, 주소 정렬이 곧 무효화
 * 가능한 최대 범위를 제한한다. 그래서 정렬이 나쁜 해제는 결국 도메인 전체 무효화로
 * 승격되는 경우가 많다.
 *
 * 마지막의 IAIG 검사가 진단상 유용하다. 하드웨어가 요청보다 넓은 범위를 비웠으면
 * 정확성 문제는 없고 성능만 손해이지만, 0 이면 무효화 자체가 수행되지 않은 것이라
 * 옛 번역이 그대로 남는다.
 *
 * 실행 컨텍스트: 해제 경로. 레지스터 락을 잡는다.
 *
 * 호출 체인: iommu->flush.flush_iotlb == [이 함수] (레지스터 방식일 때)
 */
void __iommu_flush_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
			 unsigned int size_order, u64 type)
{
	int tlb_offset = ecap_iotlb_offset(iommu->ecap);	/* [한국어] IOTLB 레지스터의 위치는 하드웨어마다 다르다 — 능력 레지스터가 알려 준다 */
	u64 val = 0, val_iva = 0;	/* [한국어] 명령 값과 주소 값 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 */

	switch (type) {	/* [한국어] 무효화 범위 */
	case DMA_TLB_GLOBAL_FLUSH:	/* [한국어] 전역 — 모든 도메인의 IOTLB */
		/* global flush doesn't need set IVA_REG */
		val = DMA_TLB_GLOBAL_FLUSH|DMA_TLB_IVT;	/* [한국어] 주소를 쓸 필요가 없다 (위 영어 주석) */
		break;
	case DMA_TLB_DSI_FLUSH:	/* [한국어] Domain-Selective — 한 도메인 전체 */
		val = DMA_TLB_DSI_FLUSH|DMA_TLB_IVT|DMA_TLB_DID(did);	/* [한국어] 도메인 id 를 싣는다. dma-iommu 의 flush queue 가 결국 이 명령 하나로 수천 개의 해제를 정리한다 */
		break;
	case DMA_TLB_PSI_FLUSH:	/* [한국어] Page-Selective — 특정 주소 범위만 */
		val = DMA_TLB_PSI_FLUSH|DMA_TLB_IVT|DMA_TLB_DID(did);	/* [한국어] 도메인 id */
		/* IH bit is passed in as part of address */
		val_iva = size_order | addr;	/* [한국어] 주소와 범위 크기를 한 값에 담는다. 하위 비트가 크기의 로그값이고 상위가 주소라, 주소 정렬이 곧 무효화 가능한 최대 범위를 정한다 (위 영어 주석의 IH 비트도 여기 실린다) */
		break;
	default:
		pr_warn("%s: Unexpected iotlb invalidation type 0x%llx\n",	/* [한국어] 알 수 없는 종류 */
			iommu->name, type);	/* [한국어] 어느 유닛의 어떤 요청 */
		return;
	}

	if (cap_write_drain(iommu->cap))	/* [한국어] 이 하드웨어가 쓰기 배수(drain)를 지원하면 */
		val |= DMA_TLB_WRITE_DRAIN;	/* [한국어] 무효화 전에 진행 중인 쓰기를 모두 완료시킨다. 그러지 않으면 이미 파이프라인에 들어간 DMA 쓰기가 옛 번역으로 완료될 수 있다 */

	raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 직렬화 */
	/* Note: Only uses first TLB reg currently */
	if (val_iva)	/* [한국어] 주소가 필요한 종류면 */
		writeq(val_iva, iommu->reg + tlb_offset);	/* [한국어] 주소 레지스터를 먼저 쓴다 */
	writeq(val, iommu->reg + tlb_offset + 8);	/* [한국어] 그 다음 명령 레지스터. 순서가 뒤집히면 하드웨어가 옛 주소로 무효화한다 */

	/* Make sure hardware complete it */
	IOMMU_WAIT_OP(iommu, tlb_offset + 8,	/* [한국어] 완료 대기 */
		readq, (!(val & DMA_TLB_IVT)), val);	/* [한국어] IVT 비트가 지워질 때까지 */

	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 끝 */

	/* check IOTLB invalidation granularity */
	if (DMA_TLB_IAIG(val) == 0)	/* [한국어] Invalidation Actual Granularity 가 0 = 하드웨어가 무효화를 수행하지 않았다 */
		pr_err("Flush IOTLB failed\n");	/* [한국어] 옛 번역이 그대로 남았다는 뜻이라 심각하다 */
	if (DMA_TLB_IAIG(val) != DMA_TLB_IIRG(type))	/* [한국어] 요청한 범위와 실제 수행된 범위가 다르다 */
		pr_debug("TLB flush request %Lx, actual %Lx\n",	/* [한국어] 하드웨어가 더 넓은 범위를 비운 경우로, 정확성 문제는 없고 성능만 손해다 */
			(unsigned long long)DMA_TLB_IIRG(type),	/* [한국어] 요청한 범위 */
			(unsigned long long)DMA_TLB_IAIG(val));	/* [한국어] 실제 범위 */
}

/*
 * [한국어]
 * domain_lookup_dev_info - 이 도메인에 붙어 있는 특정 장치의 정보를 찾는다
 *
 * @domain: 대상 도메인
 * @iommu:  그 장치를 맡은 유닛
 * @bus/@devfn: 장치의 BDF
 * @return: 장치 정보, 없으면 NULL
 *
 * 유닛까지 비교하는 것이 중요하다. 서로 다른 PCI 세그먼트나 서로 다른 유닛 아래에
 * 같은 BDF 가 존재할 수 있어, 버스·함수만으로는 장치를 특정하지 못한다.
 *
 * 실행 컨텍스트: 부착/해제 경로. 도메인 락을 잡는다.
 *
 * 호출 체인: domain_context_mapping 등 → [이 함수]
 */
static struct device_domain_info *
domain_lookup_dev_info(struct dmar_domain *domain,
		       struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	struct device_domain_info *info;	/* [한국어] 순회 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	spin_lock_irqsave(&domain->lock, flags);	/* [한국어] 도메인의 장치 목록 보호 */
	list_for_each_entry(info, &domain->devices, link) {	/* [한국어] 이 도메인에 붙은 장치들 */
		if (info->iommu == iommu && info->bus == bus &&	/* [한국어] 같은 유닛의 */
		    info->devfn == devfn) {	/* [한국어] 같은 BDF 를 찾는다. 서로 다른 유닛 아래에 같은 BDF 가 있을 수 있어 유닛까지 비교해야 한다 */
			spin_unlock_irqrestore(&domain->lock, flags);	/* [한국어] 락 해제 */
			return info;	/* [한국어] 찾은 장치 정보 */
		}
	}
	spin_unlock_irqrestore(&domain->lock, flags);	/* [한국어] 순회 끝 */

	return NULL;	/* [한국어] 이 도메인에 없는 장치 */
}

/*
 * The extra devTLB flush quirk impacts those QAT devices with PCI device
 * IDs ranging from 0x4940 to 0x4943. It is exempted from risky_device()
 * check because it applies only to the built-in QAT devices and it doesn't
 * grant additional privileges.
 */
#define BUGGY_QAT_DEVID_MASK 0x4940	/* [한국어] 특정 QAT 가속기의 장치 ID 대역 */
/*
 * [한국어] (위 영어 주석에 이어)
 * dev_needs_extra_dtlb_flush - 이 장치가 devTLB 무효화를 한 번 더 필요로 하는가
 *
 * @pdev:   확인할 장치
 * @return: 필요하면 true
 *
 * 특정 세대의 내장 QAT 가속기가 첫 devTLB 무효화를 놓치는 결함이 있다. ATS 를
 * 쓰는 장치는 자기 캐시(ATC)에 번역을 들고 있으므로, 그 무효화가 유실되면
 * 해제된 페이지에 계속 접근하게 된다 — 그래서 한 번 더 보낸다.
 *
 * risky_device 검사를 면제한 이유가 위 영어 주석에 있다. 내장 장치에만 적용되고
 * 추가 권한을 주는 것이 아니므로, 외부에서 꽂은 장치가 이 ID 를 위장해도 얻는
 * 것이 없다.
 *
 * 실행 컨텍스트: 프로브 경로.
 *
 * 호출 체인: intel_iommu_probe_device → [이 함수]
 */
static bool dev_needs_extra_dtlb_flush(struct pci_dev *pdev)
{
	if (pdev->vendor != PCI_VENDOR_ID_INTEL)	/* [한국어] 인텔 장치가 아니면 */
		return false;	/* [한국어] 해당 없음 */

	if ((pdev->device & 0xfffc) != BUGGY_QAT_DEVID_MASK)	/* [한국어] 0x4940~0x4943 범위가 아니면 */
		return false;	/* [한국어] 해당 없음 */

	return true;	/* [한국어] 이 장치는 devTLB 무효화를 한 번 더 보내야 한다. risky_device 검사를 면제한 것은 내장 장치이고 추가 권한을 주는 것이 아니기 때문이다 (위 영어 주석) */
}

/*
 * [한국어]
 * iommu_enable_pci_ats - 장치가 번역을 자기 캐시에 들고 있게 한다
 *
 * @info: 장치 정보
 *
 * ATS(Address Translation Services)는 장치가 IOMMU 에 한 번 물어본 번역 결과를
 * 자기 ATC 에 보관하고 이후 그것을 재사용하게 하는 기능이다. IOMMU 왕복이
 * 사라져 지연이 크게 줄지만, 대가로 무효화를 IOMMU 뿐 아니라 장치에도 보내야
 * 한다 — 그 devTLB 무효화가 해제 경로의 추가 비용이다.
 *
 * 페이지 정렬 검사가 안전장치다. 장치의 무효화 입도가 페이지에 맞지 않으면
 * 요청한 범위가 정확히 지워지지 않아 옛 번역이 남을 수 있다.
 *
 * 실행 컨텍스트: 장치 부착 경로. 프로세스 문맥.
 *
 * 호출 체인: intel_iommu_attach_device 계열 → [이 함수]
 */
static void iommu_enable_pci_ats(struct device_domain_info *info)
{
	struct pci_dev *pdev;	/* [한국어] PCI 형으로 변환할 장치 */

	if (!info->ats_supported)	/* [한국어] ATS 를 지원하지 않는 장치 */
		return;	/* [한국어] 켤 수 없다 */

	pdev = to_pci_dev(info->dev);	/* [한국어] PCI 장치 */
	if (!pci_ats_page_aligned(pdev))	/* [한국어] 장치의 ATC 무효화 입도가 페이지에 맞지 않으면 */
		return;	/* [한국어] 켜지 않는다 — 무효화가 정확히 되지 않으면 옛 번역이 남는다 */

	if (!pci_enable_ats(pdev, VTD_PAGE_SHIFT))	/* [한국어] 장치가 번역을 자기 캐시(ATC)에 들고 있게 한다. IOMMU 왕복이 사라져 지연이 크게 줄지만, 이제 무효화를 장치에도 보내야 한다 */
		info->ats_enabled = 1;	/* [한국어] 켜졌음을 기록 — 이후 무효화 경로가 이 값을 보고 devTLB 명령을 함께 낸다 */
}

/*
 * [한국어]
 * iommu_disable_pci_ats - 장치의 번역 캐시를 끈다
 *
 * @info: 장치 정보
 *
 * 이 호출 이후 장치는 매 접근마다 IOMMU 에 번역을 묻는다. 느려지지만, 무효화가
 * IOMMU 한 곳에만 도달하면 되므로 해제 경로는 단순해진다.
 *
 * 실행 컨텍스트: 장치 해제 경로.
 *
 * 호출 체인: 도메인 해제, 장치 제거 → [이 함수]
 */
static void iommu_disable_pci_ats(struct device_domain_info *info)
{
	if (!info->ats_enabled)	/* [한국어] 켜져 있지 않으면 */
		return;	/* [한국어] 할 일 없음 */

	pci_disable_ats(to_pci_dev(info->dev));	/* [한국어] 장치의 ATC 를 끈다 */
	info->ats_enabled = 0;	/* [한국어] 기록 해제 */
}

/*
 * [한국어]
 * iommu_enable_pci_pri - 장치가 페이지 요청을 보낼 수 있게 한다
 *
 * @info: 장치 정보
 *
 * PRI(Page Request Interface)는 장치가 매핑되지 않은 주소에 접근했을 때 폴트로
 * 죽는 대신 "이 페이지를 채워 달라"고 요청하게 한다. SVA 의 요구 페이징이
 * 성립하는 하드웨어 근거이며, 그 요청이 io-pgfault.c 를 거쳐 handle_mm_fault 로
 * 이어진다.
 *
 * ATS 가 먼저 켜져 있어야 하는 것은 PRI 가 그 위에 얹힌 프로토콜이기 때문이다.
 *
 * PASID 검사가 미묘하다. PASID 를 쓰는 구성에서 장치가 응답 메시지에 PASID 를
 * 싣지 않으면, 커널이 그 응답을 어느 주소 공간의 것으로 되짚을 수 없다.
 *
 * 실행 컨텍스트: 장치 부착 경로.
 *
 * 호출 체인: SVA/PASID 활성화 경로 → [이 함수]
 */
static void iommu_enable_pci_pri(struct device_domain_info *info)
{
	struct pci_dev *pdev;	/* [한국어] PCI 장치 */

	if (!info->ats_enabled || !info->pri_supported)	/* [한국어] PRI 는 ATS 위에 얹히는 기능이라 ATS 가 먼저 켜져 있어야 한다 */
		return;	/* [한국어] 켤 수 없다 */

	pdev = to_pci_dev(info->dev);	/* [한국어] PCI 장치 */
	/* PASID is required in PRG Response Message. */
	if (info->pasid_enabled && !pci_prg_resp_pasid_required(pdev))	/* [한국어] PASID 를 쓰는데 장치가 응답에 PASID 를 싣지 않는다면, 어느 주소 공간의 요청인지 되짚을 수 없다 (위 영어 주석) */
		return;	/* [한국어] 켜지 않는다 */

	if (pci_reset_pri(pdev))	/* [한국어] 장치의 PRI 상태를 초기화한다 */
		return;	/* [한국어] 실패하면 켜지 않는다 */

	if (!pci_enable_pri(pdev, PRQ_DEPTH))	/* [한국어] 페이지 요청을 보낼 수 있게 한다. 이것이 켜져야 SVA 의 요구 페이징이 성립한다 */
		info->pri_enabled = 1;	/* [한국어] 켜졌음을 기록 */
}

/*
 * [한국어]
 * iommu_disable_pci_pri - 페이지 요청 기능을 끈다
 *
 * @info: 장치 정보
 *
 * 끄기 전에 폴트 큐에서 장치를 떼는 것이 순서상 중요하다. 그래야 아직 응답하지
 * 않은 요청들에 실패 응답이 나가고, 장치가 멈춘 채로 남지 않는다.
 *
 * iopf_refcount 가 0 이 아닌데도 여기 왔다면 사용자가 정리하지 않은 것이지만,
 * 그래도 큐에서 떼어 내는 편이 장치를 영영 멈춰 두는 것보다 낫다.
 *
 * 실행 컨텍스트: 장치 해제 경로.
 *
 * 호출 체인: SVA/PASID 해제 경로 → [이 함수]
 */
static void iommu_disable_pci_pri(struct device_domain_info *info)
{
	if (!info->pri_enabled)	/* [한국어] 켜져 있지 않으면 */
		return;	/* [한국어] 할 일 없음 */

	if (WARN_ON(info->iopf_refcount))	/* [한국어] 아직 폴트 처리를 요구하는 사용자가 남아 있다 */
		iopf_queue_remove_device(info->iommu->iopf_queue, info->dev);	/* [한국어] 그래도 큐에서 뗀다 — 밀린 요청에 실패 응답이 나가 장치가 멈추지 않게 */

	pci_disable_pri(to_pci_dev(info->dev));	/* [한국어] PRI 를 끈다 */
	info->pri_enabled = 0;	/* [한국어] 기록 해제 */
}

/*
 * [한국어]
 * intel_flush_iotlb_all - 이 도메인의 IOTLB 를 통째로 비운다 (ops 진입점)
 *
 * @domain: 대상 도메인
 *
 * dma-iommu 의 flush queue 가 쌓아 둔 해제를 한 번에 정리할 때 코어가 부른다.
 * 이 한 번의 호출이 수천 건의 개별 무효화를 대신하며, 지연 무효화의 이득이
 * 실현되는 지점이다.
 *
 * cache_tag 계층에 위임하는 이유는 도메인 하나가 여러 DMAR 유닛에 걸쳐 설치될
 * 수 있기 때문이다. 어느 유닛에 어떤 무효화를 보내야 하는지를 그쪽이 추적한다.
 *
 * 실행 컨텍스트: 어디서든. 인터럽트 문맥 가능.
 *
 * 호출 체인: iommu.c 의 flush_iotlb_all → [이 함수] → cache_tag_flush_all
 */
static void intel_flush_iotlb_all(struct iommu_domain *domain)
{
	cache_tag_flush_all(to_dmar_domain(domain));	/* [한국어] 이 도메인이 걸쳐 있는 모든 유닛에 전체 무효화를 낸다. 도메인 하나가 여러 DMAR 유닛에 설치될 수 있어, 어디에 보낼지를 cache.c 의 태그 목록이 추적한다 */
}

/*
 * [한국어]
 * iommu_disable_protect_mem_regions - BIOS 가 설정한 하드웨어 보호 영역을 끈다
 *
 * @iommu: 대상 유닛
 *
 * VT-d 에는 페이지 테이블과 무관하게 특정 물리 범위로의 DMA 를 차단하는 별도
 * 기능이 있고, BIOS 가 부팅 중 그것을 켜 둔다. 커널이 IOMMU 를 넘겨받아 페이지
 * 테이블로 접근을 관리하기 시작하면 그 중복 보호가 오히려 정상 매핑을 막으므로
 * 꺼야 한다.
 *
 * 실행 컨텍스트: 유닛 초기화. 레지스터 락을 잡는다.
 *
 * 호출 체인: init_dmars → [이 함수]
 */
static void iommu_disable_protect_mem_regions(struct intel_iommu *iommu)
{
	u32 pmen;	/* [한국어] 보호 메모리 활성화 레지스터 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	if (!cap_plmr(iommu->cap) && !cap_phmr(iommu->cap))	/* [한국어] 보호 영역 기능이 없는 하드웨어 */
		return;	/* [한국어] 할 일 없음 */

	raw_spin_lock_irqsave(&iommu->register_lock, flags);	/* [한국어] 레지스터 접근 직렬화 */
	pmen = readl(iommu->reg + DMAR_PMEN_REG);	/* [한국어] 현재 값 */
	pmen &= ~DMA_PMEN_EPM;	/* [한국어] Enable Protected Memory 를 끈다. BIOS 가 설정한 하드웨어 보호 영역인데, 커널이 IOMMU 를 관리하기 시작하면 그 영역을 페이지 테이블로 다루므로 중복 보호가 오히려 방해가 된다 */
	writel(pmen, iommu->reg + DMAR_PMEN_REG);	/* [한국어] 기록 */

	/* wait for the protected region status bit to clear */
	IOMMU_WAIT_OP(iommu, DMAR_PMEN_REG,	/* [한국어] 완료 대기 */
		readl, !(pmen & DMA_PMEN_PRS), pmen);	/* [한국어] Protected Region Status 가 내려갈 때까지 */

	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);	/* [한국어] 레지스터 접근 끝 */
}

/*
 * [한국어]
 * iommu_enable_translation - 이 유닛의 DMA 번역을 켠다
 *
 * @iommu: 대상 유닛
 *
 * 이 함수가 돌아온 순간부터 이 유닛 아래의 모든 DMA 가 페이지 테이블을 거친다.
 * 그 전까지는 장치가 물리 주소를 그대로 쓰고 있었으므로, 켜기 전에 필요한 항등
 * 매핑(RMRR 등)이 모두 자리 잡고 있어야 한다.
 *
 * gcmd 사본을 갱신하는 것이 필수다. 전역 명령 레지스터는 읽을 수 없어 소프트웨어가
 * 현재 설정을 기억해야 하고, 다음 명령을 낼 때 그 사본을 함께 써야 이전 설정이
 * 꺼지지 않는다.
 *
 * 실행 컨텍스트: 유닛 초기화, 리쥼. 레지스터 락을 잡는다.
 *
 * 호출 체인: init_dmars, 리쥼 경로 → [이 함수]
 */
static void iommu_enable_translation(struct intel_iommu *iommu)
{
	u32 sts;	/* [한국어] 상태 레지스터 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	raw_spin_lock_irqsave(&iommu->register_lock, flags);	/* [한국어] 레지스터 접근 직렬화 */
	iommu->gcmd |= DMA_GCMD_TE;	/* [한국어] Translation Enable. gcmd 사본을 갱신해 두는 것이 중요하다 — 전역 명령 레지스터는 읽을 수 없어 소프트웨어가 현재 상태를 기억해야 한다 */
	writel(iommu->gcmd, iommu->reg + DMAR_GCMD_REG);	/* [한국어] 이 순간부터 이 유닛 아래의 모든 DMA 가 번역을 거친다 */

	/* Make sure hardware complete it */
	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] 완료 대기 */
		      readl, (sts & DMA_GSTS_TES), sts);	/* [한국어] Translation Enable Status 가 설 때까지 */

	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);	/* [한국어] 레지스터 접근 끝 */
}

/*
 * [한국어]
 * iommu_disable_translation - 이 유닛의 번역을 끈다
 *
 * @iommu: 대상 유닛
 *
 * 끄는 순간 격리가 사라지고 모든 DMA 가 물리 주소로 통과한다. 그래서 정상 종료
 * 경로에서는 장치가 모두 떼어진 뒤에만 부른다.
 *
 * 앞의 조건이 kexec 를 위한 예외다. 그래픽 전용 유닛은 디스플레이가 계속 DMA 를
 * 내고 있어, 번역을 끄면 넘어가는 커널에서 화면이 깨진다. iommu_skip_te_disable
 * 이 그 상황을 위해 존재한다.
 *
 * 실행 컨텍스트: 종료/서스펜드. 레지스터 락을 잡는다.
 *
 * 호출 체인: disable_dmar_iommu, 서스펜드 경로 → [이 함수]
 */
static void iommu_disable_translation(struct intel_iommu *iommu)
{
	u32 sts;	/* [한국어] 상태 레지스터 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 */

	if (iommu_skip_te_disable && iommu->drhd->gfx_dedicated &&	/* [한국어] 종료 시 번역을 끄지 않도록 요청되었고, 그래픽 전용 유닛이며 */
	    (cap_read_drain(iommu->cap) || cap_write_drain(iommu->cap)))	/* [한국어] 배수 기능이 있다면 */
		return;	/* [한국어] 끄지 않는다. kexec 로 넘어갈 때 디스플레이가 계속 DMA 를 내고 있어, 번역을 끄면 화면이 깨진다 */

	raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 직렬화 */
	iommu->gcmd &= ~DMA_GCMD_TE;	/* [한국어] 번역 비트를 내린다 */
	writel(iommu->gcmd, iommu->reg + DMAR_GCMD_REG);	/* [한국어] 이 순간부터 모든 DMA 가 물리 주소로 통과한다 — 격리가 사라진다 */

	/* Make sure hardware complete it */
	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG,	/* [한국어] 완료 대기 */
		      readl, (!(sts & DMA_GSTS_TES)), sts);	/* [한국어] 상태 비트가 내려갈 때까지 */

	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 끝 */
}

/*
 * [한국어]
 * disable_dmar_iommu - 유닛을 정지 상태로 만든다
 *
 * @iommu: 대상 유닛
 *
 * 도메인 id 가 남아 있으면 아직 장치가 붙어 있다는 뜻이고, 그 상태에서 번역을
 * 끄면 그 장치들이 곧바로 물리 주소로 DMA 하게 된다 (위 영어 주석). 그래서
 * 경고만 남기고 끄지 않는다 — 격리를 잃느니 유닛을 켜 둔 채로 두는 편이 낫다.
 *
 * 실행 컨텍스트: 드라이버 제거, 유닛 핫플러그. 프로세스 문맥.
 *
 * 호출 체인: 유닛 제거 경로 → [이 함수]
 */
static void disable_dmar_iommu(struct intel_iommu *iommu)
{
	/*
	 * All iommu domains must have been detached from the devices,
	 * hence there should be no domain IDs in use.
	 */
	if (WARN_ON(!ida_is_empty(&iommu->domain_ida)))	/* [한국어] 아직 쓰이는 도메인 id 가 남아 있다 = 장치가 떼어지지 않았다 (위 영어 주석) */
		return;	/* [한국어] 번역을 끄면 그 장치들이 곧바로 물리 주소로 DMA 하게 되므로 그대로 둔다 */

	if (iommu->gcmd & DMA_GCMD_TE)	/* [한국어] 번역이 켜져 있으면 */
		iommu_disable_translation(iommu);	/* [한국어] 끈다 */
}

/*
 * [한국어]
 * free_dmar_iommu - 유닛의 자료구조를 반납한다
 *
 * @iommu: 사라지는 유닛
 *
 * copied_tables 는 kexec 로 물려받은 컨텍스트 항목을 추적하던 비트맵이다.
 * 앞선 커널이 만든 설정을 우리 것과 구별하기 위한 것이며, 유닛이 사라지면 함께
 * 없어진다.
 *
 * 실행 컨텍스트: 유닛 제거. 프로세스 문맥.
 *
 * 호출 체인: 유닛 제거 경로 → [이 함수]
 */
static void free_dmar_iommu(struct intel_iommu *iommu)
{
	if (iommu->copied_tables) {	/* [한국어] kexec 로 물려받은 테이블 추적 비트맵이 있으면 */
		bitmap_free(iommu->copied_tables);	/* [한국어] 해제 */
		iommu->copied_tables = NULL;	/* [한국어] 두 번 해제되지 않도록 */
	}

	/* free context mapping */
	free_context_table(iommu);	/* [한국어] 루트·컨텍스트 테이블 반납 */

	if (ecap_prs(iommu->ecap))	/* [한국어] 페이지 요청 큐를 지원하는 하드웨어면 */
		intel_iommu_finish_prq(iommu);	/* [한국어] 그 큐도 정리한다 */
}

/*
 * Check and return whether first level is used by default for
 * DMA translation.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * first_level_by_default - DMA 번역에 1단계 페이지 테이블을 기본으로 쓸 것인가
 *
 * @iommu:  대상 유닛
 * @return: 1단계를 쓰면 true
 *
 * VT-d 는 두 종류의 페이지 테이블을 지원한다. 2단계(second-level)는 VT-d 고유
 * 형식이고, 1단계(first-level)는 x86 CPU 의 페이지 테이블과 형식이 같다.
 *
 * 둘 다 가능하면 1단계를 고르는 이유가 두 가지다. CPU 와 형식이 같아 SVA 에서
 * 프로세스의 페이지 테이블을 그대로 가리킬 수 있고, 큰 페이지 지원도 낫다.
 * 2단계는 주로 가상화의 두 번째 번역 층으로 쓰인다.
 *
 * 레거시 모드에는 2단계만 있다 (위 영어 주석) — 1단계는 scalable mode 에서
 * 도입된 것이기 때문이다.
 *
 * 실행 컨텍스트: 도메인 생성.
 *
 * 호출 체인: 도메인 할당 경로 → [이 함수]
 */
static bool first_level_by_default(struct intel_iommu *iommu)
{
	/* Only SL is available in legacy mode */
	if (!sm_supported(iommu))	/* [한국어] scalable mode 가 없으면 */
		return false;	/* [한국어] 레거시는 2단계 번역만 가능하다 (위 영어 주석) */

	/* Only level (either FL or SL) is available, just use it */
	if (ecap_flts(iommu->ecap) ^ ecap_slts(iommu->ecap))	/* [한국어] 둘 중 하나만 지원하면 */
		return ecap_flts(iommu->ecap);	/* [한국어] 그것을 쓴다 */

	return true;	/* [한국어] 둘 다 가능하면 1단계를 기본으로 삼는다. 1단계는 CPU 페이지 테이블과 형식이 같아 SVA 로 확장하기 쉽고, 큰 페이지 지원도 낫다 */
}

/*
 * [한국어]
 * domain_attach_iommu - 이 도메인이 특정 DMAR 유닛에서 쓸 도메인 id 를 확보한다
 *
 * @domain: 대상 도메인
 * @iommu:  설치할 유닛
 * @return: 0 성공, -ENOSPC 면 id 고갈, -ENOMEM 이면 할당 실패
 *
 * VT-d 의 도메인 id 는 유닛마다 독립적인 자원이다. 하나의 커널 도메인이 여러
 * 유닛에 걸쳐 설치될 수 있고(그 도메인의 장치들이 서로 다른 소켓에 있는 경우),
 * 그때 유닛마다 다른 id 를 받는다. iommu_array 가 그 대응을 담는다.
 *
 * 참조 계수를 두는 이유는 같은 유닛의 여러 장치가 한 도메인을 공유하기 때문이다.
 * 첫 장치가 id 를 떼고 마지막 장치가 돌려준다.
 *
 * cap_ndoms 가 상한인 것이 실무적으로 중요하다. 그 값이 이 유닛에서 동시에
 * 존재할 수 있는 주소 공간의 개수이며, 구형 하드웨어는 그것이 작아 VM 을 많이
 * 띄우면 실제로 고갈된다.
 *
 * 실행 컨텍스트: 장치 부착. did_lock 을 잡는다. 잠들 수 있다.
 *
 * 호출 체인: intel_iommu_attach_device 계열 → [이 함수]
 */
int domain_attach_iommu(struct dmar_domain *domain, struct intel_iommu *iommu)
{
	struct iommu_domain_info *info, *curr;	/* [한국어] 이 유닛에서의 도메인 정보와, 이미 있던 것 */
	int num, ret = -ENOSPC;	/* [한국어] 할당받을 도메인 id 와 결과 */

	if (domain->domain.type == IOMMU_DOMAIN_SVA)	/* [한국어] SVA 도메인은 PASID 표를 통해 설치되며 도메인 id 를 쓰지 않는다 */
		return 0;	/* [한국어] 할 일 없음 */

	info = kzalloc_obj(*info);	/* [한국어] 락 밖에서 미리 잡는다 */
	if (!info)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 부착 불가 */

	guard(mutex)(&iommu->did_lock);	/* [한국어] 도메인 id 할당을 직렬화. guard 라 반환 경로마다 해제를 쓰지 않아도 된다 */
	curr = xa_load(&domain->iommu_array, iommu->seq_id);	/* [한국어] 이 도메인이 이 유닛에 이미 설치되어 있는지 */
	if (curr) {	/* [한국어] 같은 유닛의 다른 장치가 먼저 붙였다 */
		curr->refcnt++;	/* [한국어] 참조만 늘린다 — 도메인 id 는 유닛당 하나면 된다 */
		kfree(info);	/* [한국어] 미리 잡은 것은 버린다 */
		return 0;	/* [한국어] 이미 설치되어 있다 */
	}

	num = ida_alloc_range(&iommu->domain_ida, IDA_START_DID,	/* [한국어] 이 유닛에서 쓸 도메인 id 를 뗀다 */
			      cap_ndoms(iommu->cap) - 1, GFP_KERNEL);	/* [한국어] 상한은 하드웨어가 지원하는 도메인 수. 이 값이 곧 한 유닛이 동시에 관리할 수 있는 주소 공간의 개수이며, 흔히 65536 이지만 구형은 훨씬 적다 */
	if (num < 0) {	/* [한국어] id 고갈 */
		pr_err("%s: No free domain ids\n", iommu->name);	/* [한국어] 이 유닛에 더는 도메인을 만들 수 없다 */
		goto err_unlock;	/* [한국어] 되감기 */
	}

	info->refcnt	= 1;	/* [한국어] 첫 사용자 */
	info->did	= num;	/* [한국어] 하드웨어에 쓸 도메인 id. 컨텍스트 항목과 무효화 명령이 이 값을 싣는다 */
	info->iommu	= iommu;	/* [한국어] 어느 유닛의 정보인지 */
	curr = xa_cmpxchg(&domain->iommu_array, iommu->seq_id,	/* [한국어] 원자적으로 등록한다 — 위의 조회와 이 등록 사이에 다른 CPU 가 끼어들 수 있다 */
			  NULL, info, GFP_KERNEL);	/* [한국어] 비어 있을 때만 넣는다 */
	if (curr) {	/* [한국어] 다른 CPU 가 먼저 넣었거나 메모리 부족 */
		ret = xa_err(curr) ? : -EBUSY;	/* [한국어] 에러 포인터면 그 값, 아니면 경쟁 패배 */
		goto err_clear;	/* [한국어] 뗀 id 를 돌려준다 */
	}

	return 0;	/* [한국어] 이 유닛에서 이 도메인이 쓸 id 가 정해졌다 */

err_clear:	/* [한국어] 등록 실패 경로 */
	ida_free(&iommu->domain_ida, info->did);	/* [한국어] 도메인 id 반납 */
err_unlock:	/* [한국어] id 할당 실패가 합류 */
	kfree(info);	/* [한국어] 정보 구조체 반납 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * domain_detach_iommu - 유닛에서의 도메인 id 참조를 놓는다
 *
 * @domain: 대상 도메인
 * @iommu:  떠나는 유닛
 *
 * 마지막 참조에서만 id 를 반납한다. 그 전까지는 같은 유닛의 다른 장치가 아직
 * 이 도메인을 쓰고 있다는 뜻이다.
 *
 * 실행 컨텍스트: 장치 해제. did_lock 을 잡는다.
 *
 * 호출 체인: 도메인 해제 경로 → [이 함수]
 */
void domain_detach_iommu(struct dmar_domain *domain, struct intel_iommu *iommu)
{
	struct iommu_domain_info *info;	/* [한국어] 이 유닛에서의 도메인 정보 */

	if (domain->domain.type == IOMMU_DOMAIN_SVA)	/* [한국어] SVA 는 도메인 id 를 쓰지 않는다 */
		return;	/* [한국어] 할 일 없음 */

	guard(mutex)(&iommu->did_lock);	/* [한국어] id 조작 직렬화 */
	info = xa_load(&domain->iommu_array, iommu->seq_id);	/* [한국어] 이 유닛의 정보 */
	if (--info->refcnt == 0) {	/* [한국어] 이 유닛에서 마지막 장치가 떠났다 */
		ida_free(&iommu->domain_ida, info->did);	/* [한국어] 도메인 id 를 풀에 돌려준다 */
		xa_erase(&domain->iommu_array, iommu->seq_id);	/* [한국어] 이 도메인이 그 유닛에 더는 설치되지 않았음을 기록 */
		kfree(info);	/* [한국어] 정보 구조체 반납 */
	}
}

/*
 * For kdump cases, old valid entries may be cached due to the
 * in-flight DMA and copied pgtable, but there is no unmapping
 * behaviour for them, thus we need an explicit cache flush for
 * the newly-mapped device. For kdump, at this point, the device
 * is supposed to finish reset at its driver probe stage, so no
 * in-flight DMA will exist, and we don't need to worry anymore
 * hereafter.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * copied_context_tear_down - kdump 로 물려받은 컨텍스트 항목을 정리한다
 *
 * @iommu:      대상 유닛
 * @context:    그 장치의 컨텍스트 항목
 * @bus/@devfn: 장치의 BDF
 *
 * 크래시 덤프 커널은 앞선 커널이 만들어 둔 페이지 테이블과 컨텍스트 항목 위에서
 * 시작한다. 그것을 그대로 쓰는 이유는 진행 중이던 DMA 를 끊지 않기 위해서인데,
 * 그 대가로 하드웨어 캐시에 앞선 커널의 번역이 남아 있게 된다.
 *
 * 문제는 그 매핑들에 대응하는 해제 동작이 없다는 점이다 (위 영어 주석). 정상
 * 경로였다면 unmap 이 무효화를 냈겠지만 여기서는 그런 일이 없었으므로, 이 장치를
 * 새로 설정하기 전에 명시적으로 비워야 한다.
 *
 * 이 시점에는 장치가 드라이버 프로브에서 리셋을 마쳤을 것이므로 진행 중인 DMA 는
 * 없다고 전제한다.
 *
 * 실행 컨텍스트: 장치 부착 경로. 유닛 락을 든 채.
 *
 * 호출 체인: domain_context_mapping_one → [이 함수]
 */
static void copied_context_tear_down(struct intel_iommu *iommu,
				     struct context_entry *context,
				     u8 bus, u8 devfn)
{
	u16 did_old;	/* [한국어] 앞선 커널이 이 장치에 쓰던 도메인 id */

	if (!context_copied(iommu, bus, devfn))	/* [한국어] 물려받은 항목이 아니면 */
		return;	/* [한국어] 할 일 없음 */

	assert_spin_locked(&iommu->lock);	/* [한국어] 호출자가 유닛 락을 든 상태여야 한다 */

	did_old = context_domain_id(context);	/* [한국어] 앞선 커널의 도메인 id 를 꺼낸다 */
	context_clear_entry(context);	/* [한국어] 항목을 지운다 */

	if (did_old < cap_ndoms(iommu->cap)) {	/* [한국어] 그 id 가 이 하드웨어의 범위 안이면 (물려받은 값이 신뢰할 수 있는지 확인) */
		iommu->flush.flush_context(iommu, did_old,	/* [한국어] 그 id 로 캐시된 컨텍스트를 비운다 */
					   PCI_DEVID(bus, devfn),	/* [한국어] 이 장치만 */
					   DMA_CCMD_MASK_NOBIT,	/* [한국어] 함수 마스크 없이 정확히 하나 */
					   DMA_CCMD_DEVICE_INVL);	/* [한국어] 장치 단위 무효화 */
		iommu->flush.flush_iotlb(iommu, did_old, 0, 0,	/* [한국어] 그 도메인의 IOTLB 도 */
					 DMA_TLB_DSI_FLUSH);	/* [한국어] 도메인 전체. 앞선 커널이 만든 매핑에는 대응하는 해제 동작이 없으므로 명시적으로 비워야 한다 (위 영어 주석) */
	}

	clear_context_copied(iommu, bus, devfn);	/* [한국어] 이제 이 항목은 우리 것이다 */
}

/*
 * It's a non-present to present mapping. If hardware doesn't cache
 * non-present entry we only need to flush the write-buffer. If the
 * _does_ cache non-present entries, then it does so in the special
 * domain #0, which we have to flush:
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * context_present_cache_flush - 없음→있음 전환 뒤 부정 캐시를 비운다
 *
 * @iommu:      대상 유닛
 * @did:        새로 설정한 도메인 id
 * @bus/@devfn: 장치의 BDF
 *
 * 보통의 하드웨어는 "이 장치는 설정되지 않았다"는 사실을 캐시하지 않으므로, 새
 * 컨텍스트 항목을 쓰면 곧바로 보인다. 그때는 쓰기 버퍼만 비우면 된다.
 *
 * 그러나 caching mode 하드웨어 — 주로 가상화된 IOMMU — 는 없음 항목까지 캐시하고,
 * 그것을 특수 도메인 0 에 넣는다. 그래서 도메인 0 을 명시적으로 비워야 새 설정이
 * 보인다 (위 영어 주석).
 *
 * 실행 컨텍스트: 장치 부착 경로.
 *
 * 호출 체인: domain_context_mapping_one → [이 함수]
 */
static void context_present_cache_flush(struct intel_iommu *iommu, u16 did,
					u8 bus, u8 devfn)
{
	if (cap_caching_mode(iommu->cap)) {	/* [한국어] 하드웨어가 '없음' 항목까지 캐시하는 모드면 (주로 가상화된 IOMMU) */
		iommu->flush.flush_context(iommu, 0,	/* [한국어] 도메인 0 에 캐시된 부정 항목을 비운다. 없음→있음 전환이라 그 캐시가 남아 있으면 새 매핑이 보이지 않는다 (위 영어 주석) */
					   PCI_DEVID(bus, devfn),	/* [한국어] 이 장치의 소스 id */
					   DMA_CCMD_MASK_NOBIT,	/* [한국어] 이 장치 하나만 */
					   DMA_CCMD_DEVICE_INVL);	/* [한국어] 장치 단위 무효화 */
		iommu->flush.flush_iotlb(iommu, did, 0, 0, DMA_TLB_DSI_FLUSH);	/* [한국어] 새 도메인의 IOTLB 도 비운다 — 그 id 가 앞서 다른 도메인에 쓰였을 수 있다 */
	} else {
		iommu_flush_write_buffer(iommu);	/* [한국어] 보통의 하드웨어는 쓰기 버퍼만 비우면 새 항목이 보인다 */
	}
}

/*
 * [한국어]
 * domain_context_mapping_one - 하나의 소스 id 에 대해 컨텍스트 항목을 기입한다
 *
 * @domain:     설치할 도메인
 * @iommu:      담당 유닛
 * @bus/@devfn: 소스 id
 * @return:     0 성공, 음수 실패
 *
 * 이 함수가 VT-d 에서 "장치를 도메인에 붙인다"의 실체다. 컨텍스트 항목에 도메인
 * id 와 페이지 테이블 루트를 쓰는 순간부터, 그 소스 id 로 오는 DMA 가 이 도메인의
 * 주소 공간을 보게 된다.
 *
 * 기입 순서가 정해져 있다. 모든 필드를 채운 뒤 맨 마지막에 present 를 세우고,
 * 비일관 하드웨어면 그것을 캐시에서 밀어낸다. present 를 먼저 세우면 하드웨어가
 * 아직 채워지지 않은 항목을 읽는다.
 *
 * 번역 종류를 ATS 여부로 가르는 것도 중요하다. DEV_IOTLB 로 표시해야 하드웨어가
 * 그 장치에 devTLB 무효화를 보내며, 그러지 않으면 장치의 ATC 에 옛 번역이 남는다.
 *
 * 실행 컨텍스트: 장치 부착. 유닛 락을 잡는다.
 *
 * 호출 체인: domain_context_mapping(_cb) → [이 함수]
 */
static int domain_context_mapping_one(struct dmar_domain *domain,
				      struct intel_iommu *iommu,
				      u8 bus, u8 devfn)
{
	struct device_domain_info *info =	/* [한국어] 이 장치가 이미 이 도메인에 있는지 */
			domain_lookup_dev_info(domain, iommu, bus, devfn);	/* [한국어] ATS 지원 여부를 알기 위해 필요하다 */
	u16 did = domain_id_iommu(domain, iommu);	/* [한국어] 이 유닛에서 이 도메인이 쓰는 id */
	int translation = CONTEXT_TT_MULTI_LEVEL;	/* [한국어] 번역 종류. 기본은 다단계 페이지 테이블 */
	struct pt_iommu_vtdss_hw_info pt_info;	/* [한국어] 2단계 페이지 테이블의 하드웨어 정보 (루트 주소와 주소 폭) */
	struct context_entry *context;	/* [한국어] 기입할 컨텍스트 항목 */
	int ret;	/* [한국어] 결과 */

	if (WARN_ON(!intel_domain_is_ss_paging(domain)))	/* [한국어] 레거시 컨텍스트 매핑은 2단계 페이지 테이블에만 쓰인다 */
		return -EINVAL;	/* [한국어] 다른 종류의 도메인은 PASID 경로로 설치된다 */

	pt_iommu_vtdss_hw_info(&domain->sspt, &pt_info);	/* [한국어] 공용 페이지 테이블 계층에서 루트 주소와 주소 폭을 꺼낸다 */

	pr_debug("Set context mapping for %02x:%02x.%d\n",	/* [한국어] 어느 장치를 설정하는지 */
		bus, PCI_SLOT(devfn), PCI_FUNC(devfn));	/* [한국어] BDF 를 사람이 읽는 형식으로 */

	spin_lock(&iommu->lock);	/* [한국어] 컨텍스트 테이블 변경 구간 */
	ret = -ENOMEM;	/* [한국어] 아래 할당이 실패하면 이 값이 나간다 */
	context = iommu_context_addr(iommu, bus, devfn, 1);	/* [한국어] 항목 주소를 얻는다. 컨텍스트 테이블이 없으면 만든다 */
	if (!context)	/* [한국어] 테이블 생성 실패 */
		goto out_unlock;	/* [한국어] 설정 불가 */

	ret = 0;	/* [한국어] 아래 검사를 통과하면 성공이다 */
	if (context_present(context) && !context_copied(iommu, bus, devfn))	/* [한국어] 이미 우리가 설정한 항목이면 */
		goto out_unlock;	/* [한국어] 다시 쓸 필요가 없다 */

	copied_context_tear_down(iommu, context, bus, devfn);	/* [한국어] kdump 로 물려받은 항목이면 먼저 정리한다 */
	context_clear_entry(context);	/* [한국어] 0 에서 시작 */
	context_set_domain_id(context, did);	/* [한국어] 이 유닛에서의 도메인 id. 무효화 명령이 이 값으로 범위를 지정한다 */

	if (info && info->ats_supported)	/* [한국어] 이 장치가 ATS 를 쓸 수 있으면 */
		translation = CONTEXT_TT_DEV_IOTLB;	/* [한국어] 장치가 자기 번역 캐시를 갖는 종류로 표시한다. 이 값이 있어야 하드웨어가 devTLB 무효화를 받아들인다 */
	else
		translation = CONTEXT_TT_MULTI_LEVEL;	/* [한국어] 아니면 보통의 다단계 번역 */

	context_set_address_root(context, pt_info.ssptptr);	/* [한국어] 2단계 페이지 테이블의 루트 물리 주소. 이 한 줄이 장치를 이 주소 공간에 묶는다 */
	context_set_address_width(context, pt_info.aw);	/* [한국어] 그 테이블의 주소 폭 (레벨 수) */
	context_set_translation_type(context, translation);	/* [한국어] 번역 종류 */
	context_set_fault_enable(context);	/* [한국어] 번역 실패를 폴트로 보고하게 한다. 끄면 실패가 조용히 버려져 디버깅이 불가능해진다 */
	context_set_present(context);	/* [한국어] present 비트는 반드시 마지막에. 그 전에 세우면 하드웨어가 아직 채워지지 않은 항목을 읽는다 */
	if (!ecap_coherent(iommu->ecap))	/* [한국어] 워크가 비일관인 하드웨어면 */
		clflush_cache_range(context, sizeof(*context));	/* [한국어] 기입한 항목을 메모리로 밀어낸다 */
	context_present_cache_flush(iommu, did, bus, devfn);	/* [한국어] 없음→있음 전환에 필요한 캐시 정리 */
	ret = 0;	/* [한국어] 설정 완료 */

out_unlock:	/* [한국어] 공통 출구 */
	spin_unlock(&iommu->lock);	/* [한국어] 컨텍스트 테이블 락 해제 */

	return ret;	/* [한국어] 0 이면 이 장치의 DMA 가 이제 이 도메인의 페이지 테이블을 거친다 */
}

/*
 * [한국어]
 * domain_context_mapping_cb - 별칭 하나에 대해 컨텍스트를 기입한다 (별칭 순회 콜백)
 *
 * @pdev:   순회 중인 장치
 * @alias:  이 장치가 낼 수 있는 requester id 중 하나
 * @opaque: 설치할 도메인
 * @return: 0 성공, 음수면 순회 중단
 *
 * 하드웨어는 DMA 요청에 실린 requester id 로 컨텍스트를 찾으므로, 장치가 여러
 * id 로 DMA 를 낼 수 있다면 그 모든 자리에 같은 도메인을 심어야 한다.
 *
 * 실행 컨텍스트: 장치 부착 경로.
 *
 * 호출 체인: pci_for_each_dma_alias → [이 함수]
 */
static int domain_context_mapping_cb(struct pci_dev *pdev,
				     u16 alias, void *opaque)
{
	struct device_domain_info *info = dev_iommu_priv_get(&pdev->dev);	/* [한국어] 이 장치의 드라이버 문맥 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct dmar_domain *domain = opaque;	/* [한국어] 설정할 도메인 */

	return domain_context_mapping_one(domain, iommu,	/* [한국어] 이 별칭에 대해서도 같은 컨텍스트를 기입한다 */
					  PCI_BUS_NUM(alias), alias & 0xff);	/* [한국어] 별칭의 버스와 devfn */
}

/*
 * [한국어]
 * domain_context_mapping - 장치의 모든 소스 id 에 도메인을 설치한다
 *
 * @domain: 설치할 도메인
 * @dev:    대상 장치
 * @return: 0 성공, 음수 실패
 *
 * 레거시 모드에서 장치를 도메인에 붙이는 진입점이다. PCI 장치는 별칭마다,
 * 그 외에는 한 번만 컨텍스트 항목을 기입한다.
 *
 * ATS 를 마지막에 켜는 순서가 중요하다. 컨텍스트가 모두 준비되기 전에 켜면
 * 장치가 아직 설정되지 않은 상태의 번역(즉 폴트)을 자기 캐시에 담을 수 있다.
 *
 * 실행 컨텍스트: 장치 부착. 프로세스 문맥.
 *
 * 호출 체인: intel_iommu_attach_device 계열 → [이 함수]
 */
static int
domain_context_mapping(struct dmar_domain *domain, struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 이 장치의 드라이버 문맥 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	u8 bus = info->bus, devfn = info->devfn;	/* [한국어] 장치의 BDF */
	int ret;	/* [한국어] 결과 */

	if (!dev_is_pci(dev))	/* [한국어] PCI 가 아니면 별칭이 없다 */
		return domain_context_mapping_one(domain, iommu, bus, devfn);	/* [한국어] 한 번만 설정 */

	ret = pci_for_each_dma_alias(to_pci_dev(dev),	/* [한국어] 이 장치가 낼 수 있는 모든 requester id 에 대해 */
				     domain_context_mapping_cb, domain);	/* [한국어] 각각 컨텍스트 항목을 기입한다. 브리지 뒤의 장치가 브리지 id 로 DMA 를 내면 하드웨어는 그 id 로 컨텍스트를 찾으므로, 그 자리에도 같은 도메인이 있어야 한다 */
	if (ret)	/* [한국어] 한 별칭이라도 실패하면 */
		return ret;	/* [한국어] 설정 실패 */

	iommu_enable_pci_ats(info);	/* [한국어] 모든 컨텍스트가 준비된 뒤에 ATS 를 켠다. 순서가 반대면 장치가 아직 설정되지 않은 상태에서 번역을 캐시할 수 있다 */

	return 0;	/* [한국어] 이 장치의 모든 requester id 가 이 도메인을 가리킨다 */
}

/*
 * [한국어]
 * domain_context_clear_one - 컨텍스트 항목 하나를 지우고 캐시를 비운다
 *
 * @info:       장치 정보 (담당 유닛과 ATS 상태를 안다)
 * @bus/@devfn: 지울 소스 id
 *
 * 순서가 이 함수의 전부다. present 비트만 먼저 내려 하드웨어가 이 장치를 더는
 * 번역하지 않게 만들고, 락을 놓은 뒤 무효화를 내고, 그 다음에야 항목 전체를
 * 지운다.
 *
 * 왜 나누는가 — 무효화는 하드웨어 완료를 기다리므로 오래 걸린다. 그 동안 유닛
 * 락을 붙잡고 있으면 다른 장치의 부착이 모두 멈춘다. present 를 먼저 내려 두면
 * 락을 놓아도 안전하다.
 *
 * 실행 컨텍스트: 장치 해제. 유닛 락을 잡았다 놓는다.
 *
 * 호출 체인: domain_context_clear → [이 함수]
 */
static void domain_context_clear_one(struct device_domain_info *info, u8 bus, u8 devfn)
{
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct context_entry *context;	/* [한국어] 지울 컨텍스트 항목 */
	u16 did;	/* [한국어] 무효화에 쓸 도메인 id */

	spin_lock(&iommu->lock);	/* [한국어] 컨텍스트 테이블 변경 구간 */
	context = iommu_context_addr(iommu, bus, devfn, 0);	/* [한국어] 항목을 찾는다 (만들지는 않는다) */
	if (!context) {	/* [한국어] 없으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 할 일이 없다 */
		return;
	}

	did = context_domain_id(context);	/* [한국어] 무효화에 쓸 도메인 id 를 먼저 꺼낸다 — 항목을 지운 뒤에는 알 수 없다 */
	context_clear_present(context);	/* [한국어] present 비트만 먼저 내린다. 이 순간부터 하드웨어는 이 장치의 DMA 를 폴트로 처리한다 */
	__iommu_flush_cache(iommu, context, sizeof(*context));	/* [한국어] 그 변경을 하드웨어가 보게 만든다 */
	spin_unlock(&iommu->lock);	/* [한국어] 무효화는 락 밖에서 — 완료를 기다리는 동안 다른 장치의 설정을 막지 않는다 */
	intel_context_flush_no_pasid(info, context, did);	/* [한국어] 캐시된 컨텍스트와 IOTLB, 그리고 장치의 ATC 까지 비운다. 이것이 끝나야 이 장치가 옛 주소 공간에 닿지 못한다 */
	context_clear_entry(context);	/* [한국어] 이제 항목 전체를 0 으로. present 를 먼저 내린 덕분에 이 시점에는 하드웨어가 이 항목을 보지 않는다 */
	__iommu_flush_cache(iommu, context, sizeof(*context));	/* [한국어] 그 변경도 밀어낸다 */
}

/*
 * [한국어]
 * __domain_setup_first_level - PASID 항목에 1단계 페이지 테이블을 설치한다
 *
 * @iommu:   담당 유닛
 * @dev:     대상 장치
 * @pasid:   설치할 PASID (0 이면 RID 트래픽)
 * @did:     도메인 id
 * @fsptptr: 1단계 페이지 테이블 루트의 물리 주소
 * @flags:   레벨 수, 스누핑 등의 플래그
 * @old:     기존에 붙어 있던 도메인 (교체면 먼저 뗀다)
 * @return:  0 성공, 음수 실패
 *
 * 1단계 페이지 테이블은 x86 CPU 의 것과 형식이 같다. 그래서 SVA 에서는 프로세스의
 * mm->pgd 를 그대로 이 자리에 넣을 수 있고, 그것이 장치가 프로세스 가상 주소를
 * 쓰는 방식의 실체다.
 *
 * 실행 컨텍스트: 부착 경로. 프로세스 문맥.
 *
 * 호출 체인: domain_setup_first_level, SVA 경로 → [이 함수]
 */
int __domain_setup_first_level(struct intel_iommu *iommu, struct device *dev,
			       ioasid_t pasid, u16 did, phys_addr_t fsptptr,
			       int flags, struct iommu_domain *old)
{
	if (old)	/* [한국어] 이미 다른 도메인이 이 PASID 에 붙어 있으면 */
		intel_pasid_tear_down_entry(iommu, dev, pasid, false);	/* [한국어] 먼저 떼어 낸다 */

	return intel_pasid_setup_first_level(iommu, dev, fsptptr, pasid, did, flags);	/* [한국어] PASID 항목에 1단계 페이지 테이블을 설치한다. x86 CPU 와 형식이 같은 테이블이라 SVA 가 프로세스의 것을 그대로 가리킬 수 있다 */
}

/*
 * [한국어]
 * domain_setup_second_level - PASID 항목에 2단계 페이지 테이블을 설치한다
 *
 * @iommu:  담당 유닛
 * @domain: 설치할 도메인
 * @dev:    대상 장치
 * @pasid:  설치할 PASID
 * @old:    기존 도메인
 * @return: 0 성공, 음수 실패
 *
 * scalable mode 에서도 2단계 테이블을 쓸 수 있다. 가상화에서 게스트의 출력을
 * 다시 번역하거나, 1단계를 지원하지 않는 하드웨어에서 쓰인다.
 *
 * 실행 컨텍스트: 부착 경로.
 *
 * 호출 체인: dmar_domain_attach_device 등 → [이 함수]
 */
static int domain_setup_second_level(struct intel_iommu *iommu,
				     struct dmar_domain *domain,
				     struct device *dev, ioasid_t pasid,
				     struct iommu_domain *old)
{
	if (old)	/* [한국어] 기존 부착이 있으면 */
		intel_pasid_tear_down_entry(iommu, dev, pasid, false);	/* [한국어] 먼저 정리 */

	return intel_pasid_setup_second_level(iommu, domain, dev, pasid);	/* [한국어] 2단계 페이지 테이블을 PASID 항목에 설치한다 */
}

/*
 * [한국어]
 * domain_setup_passthrough - PASID 항목을 통과 모드로 설정한다
 *
 * @iommu:  담당 유닛
 * @dev:    대상 장치
 * @pasid:  설치할 PASID
 * @old:    기존 도메인
 * @return: 0 성공, 음수 실패
 *
 * 항등 도메인의 scalable mode 구현이다. 페이지 테이블 없이 장치가 낸 주소를
 * 그대로 물리 주소로 쓰며, 번역 비용이 사라지는 대신 격리도 사라진다.
 *
 * 실행 컨텍스트: 부착 경로.
 *
 * 호출 체인: 항등 도메인 부착 경로 → [이 함수]
 */
static int domain_setup_passthrough(struct intel_iommu *iommu,
				    struct device *dev, ioasid_t pasid,
				    struct iommu_domain *old)
{
	if (old)	/* [한국어] 기존 부착이 있으면 */
		intel_pasid_tear_down_entry(iommu, dev, pasid, false);	/* [한국어] 먼저 정리 */

	return intel_pasid_setup_pass_through(iommu, dev, pasid);	/* [한국어] 번역 없이 통과시키는 PASID 항목. 항등 도메인의 scalable mode 구현이다 */
}

/*
 * [한국어]
 * domain_setup_first_level - 도메인의 1단계 테이블을 PASID 항목에 설치한다
 *
 * @iommu:  담당 유닛
 * @domain: 설치할 도메인
 * @dev:    대상 장치
 * @pasid:  설치할 PASID
 * @old:    기존 도메인
 * @return: 0 성공, 음수 실패
 *
 * 플래그 조립이 이 함수의 내용이다. 세 가지가 PASID 항목의 동작을 바꾼다.
 *  - FL5LP: 5단계 페이지 테이블(57비트 주소)임을 알린다.
 *  - PAGE_SNOOP: 장치의 데이터 DMA 가 CPU 캐시를 스누핑하게 한다. 비일관 장치도
 *    소프트웨어 캐시 관리 없이 쓸 수 있게 되지만 스누핑 대역폭을 소모한다.
 *  - PWSNP: 페이지 워크도 스누핑하게 한다. 그러면 소프트웨어가 PTE 를 쓴 뒤
 *    clflush 하지 않아도 하드웨어가 최신 값을 본다.
 *
 * 실행 컨텍스트: 부착 경로.
 *
 * 호출 체인: dmar_domain_attach_device 등 → [이 함수]
 */
static int domain_setup_first_level(struct intel_iommu *iommu,
				    struct dmar_domain *domain,
				    struct device *dev,
				    u32 pasid, struct iommu_domain *old)
{
	struct pt_iommu_x86_64_hw_info pt_info;	/* [한국어] 1단계 페이지 테이블의 하드웨어 정보 */
	unsigned int flags = 0;	/* [한국어] PASID 항목에 실을 플래그 */

	pt_iommu_x86_64_hw_info(&domain->fspt, &pt_info);	/* [한국어] 공용 페이지 테이블 계층에서 루트 주소와 레벨 수를 꺼낸다 */
	if (WARN_ON(pt_info.levels != 4 && pt_info.levels != 5))	/* [한국어] 1단계는 4단계 또는 5단계만 가능하다 */
		return -EINVAL;	/* [한국어] 다른 값은 있을 수 없다 */

	if (pt_info.levels == 5)	/* [한국어] 5단계 페이지 테이블이면 */
		flags |= PASID_FLAG_FL5LP;	/* [한국어] 57비트 주소 공간임을 하드웨어에 알린다 */

	if (domain->force_snooping)	/* [한국어] 이 도메인이 캐시 일관성을 강제하면 */
		flags |= PASID_FLAG_PAGE_SNOOP;	/* [한국어] 장치의 DMA 가 CPU 캐시를 스누핑하게 한다. 비일관 장치도 소프트웨어 캐시 관리 없이 쓸 수 있게 되지만, 스누핑 대역폭을 소모한다 */

	if (!(domain->fspt.x86_64_pt.common.features &	/* [한국어] 페이지 테이블 자체가 비일관이 아니면 */
	      BIT(PT_FEAT_DMA_INCOHERENT)))	/* [한국어] 즉 워크가 캐시를 볼 수 있으면 */
		flags |= PASID_FLAG_PWSNP;	/* [한국어] 페이지 워크도 스누핑하게 한다 — 소프트웨어가 PTE 를 쓴 뒤 플러시하지 않아도 하드웨어가 최신 값을 본다 */

	return __domain_setup_first_level(iommu, dev, pasid,	/* [한국어] 실제 설치 */
					  domain_id_iommu(domain, iommu),	/* [한국어] 이 유닛에서의 도메인 id */
					  pt_info.gcr3_pt, flags, old);	/* [한국어] 페이지 테이블 루트와 플래그 */
}

/*
 * [한국어]
 * dmar_domain_attach_device - 장치를 이 도메인에 붙인다 (드라이버 내부 진입점)
 *
 * @domain: 붙일 도메인
 * @dev:    대상 장치
 * @return: 0 성공, 음수 실패
 *
 * 네 단계로 이뤄진다. 유닛에서의 도메인 id 확보 → 도메인 장치 목록 등록 →
 * 하드웨어 설정(모드에 따라 컨텍스트 항목 또는 PASID 항목) → 무효화 태그 등록.
 *
 * 마지막 단계가 눈에 잘 띄지 않지만 중요하다. 도메인 하나가 여러 유닛에 걸쳐
 * 설치될 수 있고 장치마다 ATS 여부가 다르므로, "이 도메인의 무효화를 어디에
 * 어떤 형태로 보내야 하는가"를 태그 목록이 추적한다.
 *
 * 실패하면 차단 상태로 되돌리는 것이 안전 기본값이다. 반쯤 설정된 채로 두면
 * 장치가 무엇을 보게 될지 알 수 없다.
 *
 * 실행 컨텍스트: 부착 경로. 프로세스 문맥.
 *
 * 호출 체인: intel_iommu_attach_device → [이 함수]
 */
static int dmar_domain_attach_device(struct dmar_domain *domain,
				     struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 이 장치의 드라이버 문맥 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int ret;	/* [한국어] 결과 */

	ret = domain_attach_iommu(domain, iommu);	/* [한국어] 이 유닛에서 쓸 도메인 id 를 먼저 확보한다 */
	if (ret)	/* [한국어] id 고갈 */
		return ret;	/* [한국어] 부착 불가 */

	info->domain = domain;	/* [한국어] 장치가 어느 도메인에 속하는지 */
	info->domain_attached = true;	/* [한국어] 부착 완료 표시. 해제 경로가 이 값을 보고 무엇을 되돌릴지 정한다 */
	spin_lock_irqsave(&domain->lock, flags);	/* [한국어] 도메인의 장치 목록 보호 */
	list_add(&info->link, &domain->devices);	/* [한국어] 목록에 등록. 이후 무효화가 이 목록을 훑어 대상 장치를 찾는다 */
	spin_unlock_irqrestore(&domain->lock, flags);	/* [한국어] 락 해제 */

	if (dev_is_real_dma_subdevice(dev))	/* [한국어] 다른 장치의 이름으로 DMA 를 내는 하위 장치면 */
		return 0;	/* [한국어] 하드웨어 설정은 그 본체가 이미 했다 */

	if (!sm_supported(iommu))	/* [한국어] 레거시 모드 */
		ret = domain_context_mapping(domain, dev);	/* [한국어] 컨텍스트 항목에 직접 기입 */
	else if (intel_domain_is_fs_paging(domain))	/* [한국어] scalable mode, 1단계 페이지 테이블 */
		ret = domain_setup_first_level(iommu, domain, dev,	/* [한국어] PASID 항목에 설치 */
					       IOMMU_NO_PASID, NULL);	/* [한국어] PASID 0 = RID 트래픽 */
	else if (intel_domain_is_ss_paging(domain))	/* [한국어] scalable mode, 2단계 페이지 테이블 */
		ret = domain_setup_second_level(iommu, domain, dev,	/* [한국어] PASID 항목에 설치 */
						IOMMU_NO_PASID, NULL);	/* [한국어] PASID 0 */
	else if (WARN_ON(true))	/* [한국어] 어느 쪽도 아닌 도메인 */
		ret = -EINVAL;	/* [한국어] 있을 수 없는 상태 */

	if (ret)	/* [한국어] 하드웨어 설정 실패 */
		goto out_block_translation;	/* [한국어] 차단 상태로 되돌린다 */

	ret = cache_tag_assign_domain(domain, dev, IOMMU_NO_PASID);	/* [한국어] 무효화 태그를 등록한다. 이 도메인의 무효화가 어느 유닛에 어떤 형태로 가야 하는지를 cache.c 가 이 태그로 추적한다 */
	if (ret)	/* [한국어] 태그 등록 실패 */
		goto out_block_translation;	/* [한국어] 되감기 */

	return 0;	/* [한국어] 이 장치의 DMA 가 이제 이 도메인을 거친다 */

out_block_translation:	/* [한국어] 실패 경로 */
	device_block_translation(dev);	/* [한국어] 차단 상태로 — 반쯤 설정된 채로 두면 장치가 무엇을 보게 될지 알 수 없다 */
	return ret;	/* [한국어] 실패 이유 */
}

/**
 * device_rmrr_is_relaxable - Test whether the RMRR of this device
 * is relaxable (ie. is allowed to be not enforced under some conditions)
 * @dev: device handle
 *
 * We assume that PCI USB devices with RMRRs have them largely
 * for historical reasons and that the RMRR space is not actively used post
 * boot.  This exclusion may change if vendors begin to abuse it.
 *
 * The same exception is made for graphics devices, with the requirement that
 * any use of the RMRR regions will be torn down before assigning the device
 * to a guest.
 *
 * Return: true if the RMRR is relaxable, false otherwise
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * device_rmrr_is_relaxable - 이 장치의 RMRR 을 강제하지 않아도 되는가
 *
 * @dev:    대상 장치
 * @return: 완화 가능하면 true
 *
 * RMRR 은 원칙적으로 항등 매핑을 강제하므로, 그 장치는 사용자 공간에 넘길 수
 * 없고 임의의 주소 공간에 둘 수도 없다. 그런데 실제로는 USB 와 그래픽이 RMRR 을
 * 가진 장치의 대부분이고, 그 둘은 부팅 후에 그 영역을 쓰지 않는다는 관찰이
 * 이 완화의 근거다 (위 영어 주석).
 *
 * 위 주석이 "벤더가 남용하기 시작하면 이 예외가 바뀔 수 있다"고 덧붙인 것에서,
 * 이것이 규격이 아니라 실무적 타협임이 드러난다.
 *
 * 실행 컨텍스트: 프로브 경로.
 *
 * 호출 체인: RMRR 강제 판정 경로 → [이 함수]
 */
static bool device_rmrr_is_relaxable(struct device *dev)
{
	struct pci_dev *pdev;	/* [한국어] PCI 형으로 변환 */

	if (!dev_is_pci(dev))	/* [한국어] PCI 가 아니면 */
		return false;	/* [한국어] 완화 대상이 아니다 */

	pdev = to_pci_dev(dev);	/* [한국어] PCI 장치 */
	if (IS_USB_DEVICE(pdev) || IS_GFX_DEVICE(pdev))	/* [한국어] USB 컨트롤러나 그래픽 장치면 */
		return true;	/* [한국어] RMRR 을 강제하지 않아도 된다. USB 는 레거시 에뮬레이션이 부팅 후에는 그 영역을 쓰지 않고, 그래픽은 게스트에 넘기기 전에 정리하는 것을 조건으로 한다 (위 영어 주석) */
	else
		return false;	/* [한국어] 그 외에는 항등 매핑을 유지해야 한다 */
}

/*
 * [한국어]
 * device_def_domain_type - 이 장치가 선호하는 기본 도메인 종류
 *
 * @dev:    대상 장치
 * @return: IOMMU_DOMAIN_DMA / IOMMU_DOMAIN_IDENTITY, 0 이면 선호 없음
 *
 * 코어의 def_domain_type 콜백이다. iommu.c 가 그룹의 모든 장치에서 이 값을 모아
 * 하나로 합치므로, 여기서 반환한 값이 그룹 전체의 정책을 바꿀 수 있다.
 *
 * 두 가지만 강제한다. 하드웨어가 통과 모드를 지원하지 않으면 번역을 요구하고,
 * 특정 오디오 컨트롤러는 IOMMU 아래에서 오작동해 통과를 요구한다. 그 외에는
 * 0 을 돌려주어 시스템 정책에 맡긴다.
 *
 * 실행 컨텍스트: 도메인 종류 결정. 그룹 락 아래.
 *
 * 호출 체인: iommu.c 의 iommu_get_def_domain_type → ops->def_domain_type == [이 함수]
 */
static int device_def_domain_type(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 이 장치의 문맥 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */

	/*
	 * Hardware does not support the passthrough translation mode.
	 * Always use a dynamaic mapping domain.
	 */
	if (!ecap_pass_through(iommu->ecap))	/* [한국어] 하드웨어가 통과 모드를 지원하지 않으면 (위 영어 주석) */
		return IOMMU_DOMAIN_DMA;	/* [한국어] 항상 번역 도메인을 쓴다 */

	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치면 */
		struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] PCI 형으로 */

		if ((iommu_identity_mapping & IDENTMAP_AZALIA) && IS_AZALIA(pdev))	/* [한국어] 특정 HD 오디오 컨트롤러에 항등 매핑을 강제하도록 설정되어 있으면 */
			return IOMMU_DOMAIN_IDENTITY;	/* [한국어] 통과 도메인을 쓴다 — 그 장치가 IOMMU 아래에서 오작동하기 때문 */
	}

	return 0;	/* [한국어] 선호 없음 — 코어의 시스템 기본값을 따른다 */
}

/*
 * [한국어] (위 영어 주석에 이어)
 * intel_iommu_init_qi - 무효화 큐를 세우고 무효화 방식을 정한다
 *
 * @iommu: 대상 유닛
 *
 * VT-d 에는 무효화를 내는 두 가지 방법이 있다. 레지스터에 직접 쓰고 완료를
 * 폴링하는 방식과, 링 버퍼(큐)에 명령을 넣고 나중에 완료를 확인하는 방식이다.
 *
 * 차이가 크다. 레지스터 방식은 명령 하나마다 하드웨어 완료를 기다리므로 해제가
 * 잦은 워크로드에서 병목이 되고, 큐 방식은 여러 명령을 한 번에 제출한 뒤 마지막에
 * 한 번만 기다린다. dma-iommu 의 flush queue 가 성능을 내려면 아래 계층도 배치를
 * 받아 줘야 하고, 그것이 이 큐다.
 *
 * 앞부분이 인수인계 처리다. 펌웨어나 인터럽트 재매핑 초기화가 이미 큐를 켜 두었을
 * 수 있는데, 그 큐의 메모리는 우리 것이 아니므로 끄고 다시 세운다. 남은 폴트를
 * 먼저 걷어 내는 것도 중요한데, 폴트 레지스터가 가득 차 있으면 새 폴트가 기록되지
 * 않기 때문이다.
 *
 * 실행 컨텍스트: 유닛 초기화. 프로세스 문맥.
 *
 * 호출 체인: init_dmars → [이 함수]
 */
static void intel_iommu_init_qi(struct intel_iommu *iommu)
{
	/*
	 * Start from the sane iommu hardware state.
	 * If the queued invalidation is already initialized by us
	 * (for example, while enabling interrupt-remapping) then
	 * we got the things already rolling from a sane state.
	 */
	if (!iommu->qi) {	/* [한국어] 우리가 아직 무효화 큐를 세우지 않았다면 (인터럽트 재매핑을 켜며 이미 세웠을 수 있다 — 위 영어 주석) */
		/*
		 * Clear any previous faults.
		 */
		dmar_fault(-1, iommu);	/* [한국어] 앞선 상태에서 남은 폴트를 먼저 걷어 낸다. 폴트 레지스터가 가득 차 있으면 새 폴트가 기록되지 않는다 */
		/*
		 * Disable queued invalidation if supported and already enabled
		 * before OS handover.
		 */
		dmar_disable_qi(iommu);	/* [한국어] 펌웨어가 켜 둔 큐를 끈다. 그 큐의 메모리는 우리 것이 아니므로 그대로 쓸 수 없다 (위 영어 주석) */
	}

	if (dmar_enable_qi(iommu)) {	/* [한국어] 무효화 큐를 세우려 시도한다 */
		/*
		 * Queued Invalidate not enabled, use Register Based Invalidate
		 */
		iommu->flush.flush_context = __iommu_flush_context;	/* [한국어] 실패 — 레지스터 방식으로 물러선다 */
		iommu->flush.flush_iotlb = __iommu_flush_iotlb;	/* [한국어] 레지스터 방식은 명령마다 완료를 폴링해야 해 느리다 */
		pr_info("%s: Using Register based invalidation\n",	/* [한국어] 어느 방식인지 남긴다 — 성능 차이가 커서 진단에 중요하다 */
			iommu->name);	/* [한국어] 어느 유닛인지 */
	} else {
		iommu->flush.flush_context = qi_flush_context;	/* [한국어] 큐 방식. 명령을 링 버퍼에 넣고 완료는 나중에 확인한다 */
		iommu->flush.flush_iotlb = qi_flush_iotlb;	/* [한국어] 여러 무효화를 한 번에 제출할 수 있어, dma-iommu 의 flush queue 와 맞물려 처리량을 만든다 */
		pr_info("%s: Using Queued invalidation\n", iommu->name);	/* [한국어] 큐 방식임을 남긴다 */
	}
}

/*
 * [한국어]
 * copy_context_table - 앞선 커널의 컨텍스트 테이블을 우리 것으로 옮긴다
 *
 * @iommu:  대상 유닛
 * @old_re: 앞선 커널의 루트 항목
 * @tbl:    새로 만든 테이블들을 담을 배열
 * @bus:    이 버스 번호
 * @ext:    확장(scalable) 모드인가
 * @return: 0 성공, -ENOMEM 이면 할당 실패
 *
 * kdump 전용이다. 크래시 덤프 커널은 앞선 커널이 설정해 둔 IOMMU 상태 위에서
 * 시작하는데, 그것을 통째로 지우면 진행 중이던 DMA(덤프를 쓸 디스크 컨트롤러의
 * 동작 등)가 끊긴다. 그래서 항목을 그대로 옮겨 매핑을 유지한 채 인수인계한다.
 *
 * 옛 테이블을 그대로 쓰지 않고 복사하는 이유는 소유권이다. 그 메모리는 크래시
 * 커널이 관리하지 않는 영역이라 언제 재사용될지 알 수 없다.
 *
 * 도메인 id 예약이 조용하지만 중요하다. 옮겨 온 항목이 쓰는 id 를 우리 풀에서도
 * 예약해 두지 않으면, 새로 만든 도메인이 그 id 를 받아 아직 살아 있는 앞선 커널의
 * 매핑과 같은 무효화 범위를 공유하게 된다.
 *
 * 각 항목에 copied 표시를 남기는 것은 나중을 위한 것이다. 그 장치를 실제로
 * 설정할 때 copied_context_tear_down 이 이 표시를 보고 캐시를 비운다.
 *
 * 실행 컨텍스트: 유닛 초기화 (kdump). 프로세스 문맥.
 *
 * 호출 체인: copy_translation_tables → [이 함수]
 */
static int copy_context_table(struct intel_iommu *iommu,
			      struct root_entry *old_re,
			      struct context_entry **tbl,
			      int bus, bool ext)
{
	int tbl_idx, pos = 0, idx, devfn, ret = 0, did;	/* [한국어] 테이블 인덱스, 상위/하위 위치, 항목 인덱스, 장치 번호, 결과, 도메인 id */
	struct context_entry *new_ce = NULL, ce;	/* [한국어] 새로 만들 테이블과 복사할 항목 */
	struct context_entry *old_ce = NULL;	/* [한국어] 앞선 커널의 테이블 (임시 매핑) */
	struct root_entry re;	/* [한국어] 루트 항목 사본 */
	phys_addr_t old_ce_phys;	/* [한국어] 옛 테이블의 물리 주소 */

	tbl_idx = ext ? bus * 2 : bus;	/* [한국어] 확장(scalable) 모드면 버스마다 테이블이 둘이라 인덱스가 두 배 */
	memcpy(&re, old_re, sizeof(re));	/* [한국어] 루트 항목을 복사해 둔다 — 원본은 앞선 커널의 메모리라 언제든 바뀔 수 있다 */

	for (devfn = 0; devfn < 256; devfn++) {	/* [한국어] 이 버스의 모든 장치·함수에 대해 */
		/* First calculate the correct index */
		idx = (ext ? devfn * 2 : devfn) % 256;	/* [한국어] 확장 모드는 항목이 두 배 크기라 인덱스가 두 배가 되고, 256 을 넘으면 다음 테이블로 넘어간다 */

		if (idx == 0) {	/* [한국어] 테이블 경계 — 새 테이블을 잡을 시점 */
			/* First save what we may have and clean up */
			if (new_ce) {	/* [한국어] 앞 테이블을 다 채웠으면 */
				tbl[tbl_idx] = new_ce;	/* [한국어] 결과 배열에 등록 */
				__iommu_flush_cache(iommu, new_ce,	/* [한국어] 기입한 내용을 메모리로 */
						    VTD_PAGE_SIZE);	/* [한국어] 테이블 한 페이지 */
				pos = 1;	/* [한국어] 다음은 상위 테이블 자리 */
			}

			if (old_ce)	/* [한국어] 앞 테이블의 임시 매핑을 */
				memunmap(old_ce);	/* [한국어] 해제 */

			ret = 0;	/* [한국어] 아래 실패 전까지는 성공 */
			if (devfn < 0x80)	/* [한국어] 장치 번호 128 미만이면 */
				old_ce_phys = root_entry_lctp(&re);	/* [한국어] 하위 컨텍스트 테이블 */
			else
				old_ce_phys = root_entry_uctp(&re);	/* [한국어] 아니면 상위 */

			if (!old_ce_phys) {	/* [한국어] 그 테이블이 없다 */
				if (ext && devfn == 0) {	/* [한국어] 확장 모드에서 하위가 없으면 상위만 있을 수 있다 */
					/* No LCTP, try UCTP */
					devfn = 0x7f;	/* [한국어] 루프를 128 로 건너뛴다 (다음 회차에 ++ 되어 0x80) */
					continue;	/* [한국어] 상위 테이블을 시도한다 */
				} else {
					goto out;	/* [한국어] 더 볼 것이 없다 */
				}
			}

			ret = -ENOMEM;	/* [한국어] 아래 할당이 실패하면 이 값 */
			old_ce = memremap(old_ce_phys, PAGE_SIZE,	/* [한국어] 앞선 커널의 테이블을 임시로 매핑한다. 그 메모리는 커널 선형 매핑 안에 있다는 보장이 없어 memremap 이 필요하다 */
					MEMREMAP_WB);	/* [한국어] 쓰기 저장 캐시로 */
			if (!old_ce)	/* [한국어] 매핑 실패 */
				goto out;	/* [한국어] 복사 불가 */

			new_ce = iommu_alloc_pages_node_sz(iommu->node,	/* [한국어] 우리 테이블을 새로 잡는다 — 앞선 커널의 것을 그대로 쓰지 않는다 */
							   GFP_KERNEL, SZ_4K);	/* [한국어] 한 페이지 */
			if (!new_ce)	/* [한국어] 할당 실패 */
				goto out_unmap;	/* [한국어] 임시 매핑을 풀고 나간다 */

			ret = 0;	/* [한국어] 여기까지 성공 */
		}

		/* Now copy the context entry */
		memcpy(&ce, old_ce + idx, sizeof(ce));	/* [한국어] 옛 항목을 사본으로 */

		if (!context_present(&ce))	/* [한국어] 설정되지 않은 장치면 */
			continue;	/* [한국어] 복사할 것이 없다 */

		did = context_domain_id(&ce);	/* [한국어] 앞선 커널이 쓰던 도메인 id */
		if (did >= 0 && did < cap_ndoms(iommu->cap))	/* [한국어] 그 값이 유효 범위 안이면 */
			ida_alloc_range(&iommu->domain_ida, did, did, GFP_KERNEL);	/* [한국어] 같은 id 를 우리 풀에서도 예약한다. 그러지 않으면 새 도메인이 그 id 를 받아, 아직 살아 있는 앞선 커널의 매핑과 충돌한다 */

		set_context_copied(iommu, bus, devfn);	/* [한국어] 이 항목이 물려받은 것임을 표시. 나중에 이 장치를 설정할 때 copied_context_tear_down 이 이 표시를 보고 캐시를 비운다 */
		new_ce[idx] = ce;	/* [한국어] 항목을 그대로 옮긴다 — 진행 중인 DMA 를 끊지 않기 위해서다 */
	}

	tbl[tbl_idx + pos] = new_ce;	/* [한국어] 마지막 테이블을 등록 */

	__iommu_flush_cache(iommu, new_ce, VTD_PAGE_SIZE);	/* [한국어] 기입 내용을 메모리로 */

out_unmap:	/* [한국어] 임시 매핑을 풀어야 하는 경로 */
	memunmap(old_ce);	/* [한국어] 해제 */

out:	/* [한국어] 공통 출구 */
	return ret;	/* [한국어] 0 이면 이 버스의 컨텍스트가 모두 옮겨졌다 */
}

static int copy_translation_tables(struct intel_iommu *iommu)
{
	struct context_entry **ctxt_tbls;
	struct root_entry *old_rt;
	phys_addr_t old_rt_phys;
	int ctxt_table_entries;
	u64 rtaddr_reg;
	int bus, ret;
	bool new_ext, ext;

	rtaddr_reg = readq(iommu->reg + DMAR_RTADDR_REG);
	ext        = !!(rtaddr_reg & DMA_RTADDR_SMT);
	new_ext    = !!sm_supported(iommu);

	/*
	 * The RTT bit can only be changed when translation is disabled,
	 * but disabling translation means to open a window for data
	 * corruption. So bail out and don't copy anything if we would
	 * have to change the bit.
	 */
	if (new_ext != ext)
		return -EINVAL;

	iommu->copied_tables = bitmap_zalloc(BIT_ULL(16), GFP_KERNEL);
	if (!iommu->copied_tables)
		return -ENOMEM;

	old_rt_phys = rtaddr_reg & VTD_PAGE_MASK;
	if (!old_rt_phys)
		return -EINVAL;

	old_rt = memremap(old_rt_phys, PAGE_SIZE, MEMREMAP_WB);
	if (!old_rt)
		return -ENOMEM;

	/* This is too big for the stack - allocate it from slab */
	ctxt_table_entries = ext ? 512 : 256;
	ret = -ENOMEM;
	ctxt_tbls = kcalloc(ctxt_table_entries, sizeof(void *), GFP_KERNEL);
	if (!ctxt_tbls)
		goto out_unmap;

	for (bus = 0; bus < 256; bus++) {
		ret = copy_context_table(iommu, &old_rt[bus],
					 ctxt_tbls, bus, ext);
		if (ret) {
			pr_err("%s: Failed to copy context table for bus %d\n",
				iommu->name, bus);
			continue;
		}
	}

	spin_lock(&iommu->lock);

	/* Context tables are copied, now write them to the root_entry table */
	for (bus = 0; bus < 256; bus++) {
		int idx = ext ? bus * 2 : bus;
		u64 val;

		if (ctxt_tbls[idx]) {
			val = virt_to_phys(ctxt_tbls[idx]) | 1;
			iommu->root_entry[bus].lo = val;
		}

		if (!ext || !ctxt_tbls[idx + 1])
			continue;

		val = virt_to_phys(ctxt_tbls[idx + 1]) | 1;
		iommu->root_entry[bus].hi = val;
	}

	spin_unlock(&iommu->lock);

	kfree(ctxt_tbls);

	__iommu_flush_cache(iommu, iommu->root_entry, PAGE_SIZE);

	ret = 0;

out_unmap:
	memunmap(old_rt);

	return ret;
}

static int __init init_dmars(void)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;
	int ret;

	for_each_iommu(iommu, drhd) {
		if (drhd->ignored) {
			iommu_disable_translation(iommu);
			continue;
		}

		/*
		 * Find the max pasid size of all IOMMU's in the system.
		 * We need to ensure the system pasid table is no bigger
		 * than the smallest supported.
		 */
		if (pasid_supported(iommu)) {
			u32 temp = 2 << ecap_pss(iommu->ecap);

			intel_pasid_max_id = min_t(u32, temp,
						   intel_pasid_max_id);
		}

		intel_iommu_init_qi(iommu);
		init_translation_status(iommu);

		if (translation_pre_enabled(iommu) && !is_kdump_kernel()) {
			iommu_disable_translation(iommu);
			clear_translation_pre_enabled(iommu);
			pr_warn("Translation was enabled for %s but we are not in kdump mode\n",
				iommu->name);
		}

		/*
		 * TBD:
		 * we could share the same root & context tables
		 * among all IOMMU's. Need to Split it later.
		 */
		ret = iommu_alloc_root_entry(iommu);
		if (ret)
			goto free_iommu;

		if (translation_pre_enabled(iommu)) {
			pr_info("Translation already enabled - trying to copy translation structures\n");

			ret = copy_translation_tables(iommu);
			if (ret) {
				/*
				 * We found the IOMMU with translation
				 * enabled - but failed to copy over the
				 * old root-entry table. Try to proceed
				 * by disabling translation now and
				 * allocating a clean root-entry table.
				 * This might cause DMAR faults, but
				 * probably the dump will still succeed.
				 */
				pr_err("Failed to copy translation tables from previous kernel for %s\n",
				       iommu->name);
				iommu_disable_translation(iommu);
				clear_translation_pre_enabled(iommu);
			} else {
				pr_info("Copied translation tables from previous kernel for %s\n",
					iommu->name);
			}
		}

		intel_svm_check(iommu);
	}

	/*
	 * Now that qi is enabled on all iommus, set the root entry and flush
	 * caches. This is required on some Intel X58 chipsets, otherwise the
	 * flush_context function will loop forever and the boot hangs.
	 */
	for_each_active_iommu(iommu, drhd) {
		iommu_flush_write_buffer(iommu);
		iommu_set_root_entry(iommu);
	}

	check_tylersburg_isoch();

	/*
	 * for each drhd
	 *   enable fault log
	 *   global invalidate context cache
	 *   global invalidate iotlb
	 *   enable translation
	 */
	for_each_iommu(iommu, drhd) {
		if (drhd->ignored) {
			/*
			 * we always have to disable PMRs or DMA may fail on
			 * this device
			 */
			if (force_on)
				iommu_disable_protect_mem_regions(iommu);
			continue;
		}

		iommu_flush_write_buffer(iommu);

		if (ecap_prs(iommu->ecap)) {
			/*
			 * Call dmar_alloc_hwirq() with dmar_global_lock held,
			 * could cause possible lock race condition.
			 */
			up_write(&dmar_global_lock);
			ret = intel_iommu_enable_prq(iommu);
			down_write(&dmar_global_lock);
			if (ret)
				goto free_iommu;
		}

		ret = dmar_set_interrupt(iommu);
		if (ret)
			goto free_iommu;
	}

	return 0;

free_iommu:
	for_each_active_iommu(iommu, drhd) {
		disable_dmar_iommu(iommu);
		free_dmar_iommu(iommu);
	}

	return ret;
}

static void __init init_no_remapping_devices(void)
{
	struct dmar_drhd_unit *drhd;
	struct device *dev;
	int i;

	for_each_drhd_unit(drhd) {
		if (!drhd->include_all) {
			for_each_active_dev_scope(drhd->devices,
						  drhd->devices_cnt, i, dev)
				break;
			/* ignore DMAR unit if no devices exist */
			if (i == drhd->devices_cnt)
				drhd->ignored = 1;
		}
	}

	for_each_active_drhd_unit(drhd) {
		if (drhd->include_all)
			continue;

		for_each_active_dev_scope(drhd->devices,
					  drhd->devices_cnt, i, dev)
			if (!dev_is_pci(dev) || !IS_GFX_DEVICE(to_pci_dev(dev)))
				break;
		if (i < drhd->devices_cnt)
			continue;

		/* This IOMMU has *only* gfx devices. Either bypass it or
		   set the gfx_mapped flag, as appropriate */
		drhd->gfx_dedicated = 1;
		if (disable_igfx_iommu)
			drhd->ignored = 1;
	}
}

#ifdef CONFIG_SUSPEND
static int init_iommu_hw(void)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu = NULL;
	int ret;

	for_each_active_iommu(iommu, drhd) {
		if (iommu->qi) {
			ret = dmar_reenable_qi(iommu);
			if (ret)
				return ret;
		}
	}

	for_each_iommu(iommu, drhd) {
		if (drhd->ignored) {
			/*
			 * we always have to disable PMRs or DMA may fail on
			 * this device
			 */
			if (force_on)
				iommu_disable_protect_mem_regions(iommu);
			continue;
		}

		iommu_flush_write_buffer(iommu);
		iommu_set_root_entry(iommu);
		iommu_enable_translation(iommu);
		iommu_disable_protect_mem_regions(iommu);
	}

	return 0;
}

static void iommu_flush_all(void)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;

	for_each_active_iommu(iommu, drhd) {
		iommu->flush.flush_context(iommu, 0, 0, 0,
					   DMA_CCMD_GLOBAL_INVL);
		iommu->flush.flush_iotlb(iommu, 0, 0, 0,
					 DMA_TLB_GLOBAL_FLUSH);
	}
}

static int iommu_suspend(void *data)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu = NULL;
	unsigned long flag;

	iommu_flush_all();

	for_each_active_iommu(iommu, drhd) {
		iommu_disable_translation(iommu);

		raw_spin_lock_irqsave(&iommu->register_lock, flag);

		iommu->iommu_state[SR_DMAR_FECTL_REG] =
			readl(iommu->reg + DMAR_FECTL_REG);
		iommu->iommu_state[SR_DMAR_FEDATA_REG] =
			readl(iommu->reg + DMAR_FEDATA_REG);
		iommu->iommu_state[SR_DMAR_FEADDR_REG] =
			readl(iommu->reg + DMAR_FEADDR_REG);
		iommu->iommu_state[SR_DMAR_FEUADDR_REG] =
			readl(iommu->reg + DMAR_FEUADDR_REG);

		raw_spin_unlock_irqrestore(&iommu->register_lock, flag);
	}
	return 0;
}

static void iommu_resume(void *data)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu = NULL;
	unsigned long flag;

	if (init_iommu_hw()) {
		if (force_on)
			panic("tboot: IOMMU setup failed, DMAR can not resume!\n");
		else
			WARN(1, "IOMMU setup failed, DMAR can not resume!\n");
		return;
	}

	for_each_active_iommu(iommu, drhd) {

		raw_spin_lock_irqsave(&iommu->register_lock, flag);

		writel(iommu->iommu_state[SR_DMAR_FECTL_REG],
			iommu->reg + DMAR_FECTL_REG);
		writel(iommu->iommu_state[SR_DMAR_FEDATA_REG],
			iommu->reg + DMAR_FEDATA_REG);
		writel(iommu->iommu_state[SR_DMAR_FEADDR_REG],
			iommu->reg + DMAR_FEADDR_REG);
		writel(iommu->iommu_state[SR_DMAR_FEUADDR_REG],
			iommu->reg + DMAR_FEUADDR_REG);

		raw_spin_unlock_irqrestore(&iommu->register_lock, flag);
	}
}

static const struct syscore_ops iommu_syscore_ops = {
	.resume		= iommu_resume,
	.suspend	= iommu_suspend,
};

static struct syscore iommu_syscore = {
	.ops = &iommu_syscore_ops,
};

static void __init init_iommu_pm_ops(void)
{
	register_syscore(&iommu_syscore);
}

#else
static inline void init_iommu_pm_ops(void) {}
#endif	/* CONFIG_PM */

static int __init rmrr_sanity_check(struct acpi_dmar_reserved_memory *rmrr)
{
	if (!IS_ALIGNED(rmrr->base_address, PAGE_SIZE) ||
	    !IS_ALIGNED(rmrr->end_address + 1, PAGE_SIZE) ||
	    rmrr->end_address <= rmrr->base_address ||
	    arch_rmrr_sanity_check(rmrr))
		return -EINVAL;

	return 0;
}

int __init dmar_parse_one_rmrr(struct acpi_dmar_header *header, void *arg)
{
	struct acpi_dmar_reserved_memory *rmrr;
	struct dmar_rmrr_unit *rmrru;

	rmrr = (struct acpi_dmar_reserved_memory *)header;
	if (rmrr_sanity_check(rmrr)) {
		pr_warn(FW_BUG
			   "Your BIOS is broken; bad RMRR [%#018Lx-%#018Lx]\n"
			   "BIOS vendor: %s; Ver: %s; Product Version: %s\n",
			   rmrr->base_address, rmrr->end_address,
			   dmi_get_system_info(DMI_BIOS_VENDOR),
			   dmi_get_system_info(DMI_BIOS_VERSION),
			   dmi_get_system_info(DMI_PRODUCT_VERSION));
		add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);
	}

	rmrru = kzalloc_obj(*rmrru);
	if (!rmrru)
		goto out;

	rmrru->hdr = header;

	rmrru->base_address = rmrr->base_address;
	rmrru->end_address = rmrr->end_address;

	rmrru->devices = dmar_alloc_dev_scope((void *)(rmrr + 1),
				((void *)rmrr) + rmrr->header.length,
				&rmrru->devices_cnt);
	if (rmrru->devices_cnt && rmrru->devices == NULL)
		goto free_rmrru;

	list_add(&rmrru->list, &dmar_rmrr_units);

	return 0;
free_rmrru:
	kfree(rmrru);
out:
	return -ENOMEM;
}

static struct dmar_atsr_unit *dmar_find_atsr(struct acpi_dmar_atsr *atsr)
{
	struct dmar_atsr_unit *atsru;
	struct acpi_dmar_atsr *tmp;

	list_for_each_entry_rcu(atsru, &dmar_atsr_units, list,
				dmar_rcu_check()) {
		tmp = (struct acpi_dmar_atsr *)atsru->hdr;
		if (atsr->segment != tmp->segment)
			continue;
		if (atsr->header.length != tmp->header.length)
			continue;
		if (memcmp(atsr, tmp, atsr->header.length) == 0)
			return atsru;
	}

	return NULL;
}

int dmar_parse_one_atsr(struct acpi_dmar_header *hdr, void *arg)
{
	struct acpi_dmar_atsr *atsr;
	struct dmar_atsr_unit *atsru;

	if (system_state >= SYSTEM_RUNNING && !intel_iommu_enabled)
		return 0;

	atsr = container_of(hdr, struct acpi_dmar_atsr, header);
	atsru = dmar_find_atsr(atsr);
	if (atsru)
		return 0;

	atsru = kzalloc(sizeof(*atsru) + hdr->length, GFP_KERNEL);
	if (!atsru)
		return -ENOMEM;

	/*
	 * If memory is allocated from slab by ACPI _DSM method, we need to
	 * copy the memory content because the memory buffer will be freed
	 * on return.
	 */
	atsru->hdr = (void *)(atsru + 1);
	memcpy(atsru->hdr, hdr, hdr->length);
	atsru->include_all = atsr->flags & 0x1;
	if (!atsru->include_all) {
		atsru->devices = dmar_alloc_dev_scope((void *)(atsr + 1),
				(void *)atsr + atsr->header.length,
				&atsru->devices_cnt);
		if (atsru->devices_cnt && atsru->devices == NULL) {
			kfree(atsru);
			return -ENOMEM;
		}
	}

	list_add_rcu(&atsru->list, &dmar_atsr_units);

	return 0;
}

static void intel_iommu_free_atsr(struct dmar_atsr_unit *atsru)
{
	dmar_free_dev_scope(&atsru->devices, &atsru->devices_cnt);
	kfree(atsru);
}

int dmar_release_one_atsr(struct acpi_dmar_header *hdr, void *arg)
{
	struct acpi_dmar_atsr *atsr;
	struct dmar_atsr_unit *atsru;

	atsr = container_of(hdr, struct acpi_dmar_atsr, header);
	atsru = dmar_find_atsr(atsr);
	if (atsru) {
		list_del_rcu(&atsru->list);
		synchronize_rcu();
		intel_iommu_free_atsr(atsru);
	}

	return 0;
}

int dmar_check_one_atsr(struct acpi_dmar_header *hdr, void *arg)
{
	int i;
	struct device *dev;
	struct acpi_dmar_atsr *atsr;
	struct dmar_atsr_unit *atsru;

	atsr = container_of(hdr, struct acpi_dmar_atsr, header);
	atsru = dmar_find_atsr(atsr);
	if (!atsru)
		return 0;

	if (!atsru->include_all && atsru->devices && atsru->devices_cnt) {
		for_each_active_dev_scope(atsru->devices, atsru->devices_cnt,
					  i, dev)
			return -EBUSY;
	}

	return 0;
}

static struct dmar_satc_unit *dmar_find_satc(struct acpi_dmar_satc *satc)
{
	struct dmar_satc_unit *satcu;
	struct acpi_dmar_satc *tmp;

	list_for_each_entry_rcu(satcu, &dmar_satc_units, list,
				dmar_rcu_check()) {
		tmp = (struct acpi_dmar_satc *)satcu->hdr;
		if (satc->segment != tmp->segment)
			continue;
		if (satc->header.length != tmp->header.length)
			continue;
		if (memcmp(satc, tmp, satc->header.length) == 0)
			return satcu;
	}

	return NULL;
}

int dmar_parse_one_satc(struct acpi_dmar_header *hdr, void *arg)
{
	struct acpi_dmar_satc *satc;
	struct dmar_satc_unit *satcu;

	if (system_state >= SYSTEM_RUNNING && !intel_iommu_enabled)
		return 0;

	satc = container_of(hdr, struct acpi_dmar_satc, header);
	satcu = dmar_find_satc(satc);
	if (satcu)
		return 0;

	satcu = kzalloc(sizeof(*satcu) + hdr->length, GFP_KERNEL);
	if (!satcu)
		return -ENOMEM;

	satcu->hdr = (void *)(satcu + 1);
	memcpy(satcu->hdr, hdr, hdr->length);
	satcu->atc_required = satc->flags & 0x1;
	satcu->devices = dmar_alloc_dev_scope((void *)(satc + 1),
					      (void *)satc + satc->header.length,
					      &satcu->devices_cnt);
	if (satcu->devices_cnt && !satcu->devices) {
		kfree(satcu);
		return -ENOMEM;
	}
	list_add_rcu(&satcu->list, &dmar_satc_units);

	return 0;
}

static int intel_iommu_add(struct dmar_drhd_unit *dmaru)
{
	struct intel_iommu *iommu = dmaru->iommu;
	int ret;

	/*
	 * Disable translation if already enabled prior to OS handover.
	 */
	if (iommu->gcmd & DMA_GCMD_TE)
		iommu_disable_translation(iommu);

	ret = iommu_alloc_root_entry(iommu);
	if (ret)
		goto out;

	intel_svm_check(iommu);

	if (dmaru->ignored) {
		/*
		 * we always have to disable PMRs or DMA may fail on this device
		 */
		if (force_on)
			iommu_disable_protect_mem_regions(iommu);
		return 0;
	}

	intel_iommu_init_qi(iommu);
	iommu_flush_write_buffer(iommu);

	if (ecap_prs(iommu->ecap)) {
		ret = intel_iommu_enable_prq(iommu);
		if (ret)
			goto disable_iommu;
	}

	ret = dmar_set_interrupt(iommu);
	if (ret)
		goto disable_iommu;

	iommu_set_root_entry(iommu);
	iommu_enable_translation(iommu);

	iommu_disable_protect_mem_regions(iommu);
	return 0;

disable_iommu:
	disable_dmar_iommu(iommu);
out:
	free_dmar_iommu(iommu);
	return ret;
}

int dmar_iommu_hotplug(struct dmar_drhd_unit *dmaru, bool insert)
{
	int ret = 0;
	struct intel_iommu *iommu = dmaru->iommu;

	if (!intel_iommu_enabled)
		return 0;
	if (iommu == NULL)
		return -EINVAL;

	if (insert) {
		ret = intel_iommu_add(dmaru);
	} else {
		disable_dmar_iommu(iommu);
		free_dmar_iommu(iommu);
	}

	return ret;
}

static void intel_iommu_free_dmars(void)
{
	struct dmar_rmrr_unit *rmrru, *rmrr_n;
	struct dmar_atsr_unit *atsru, *atsr_n;
	struct dmar_satc_unit *satcu, *satc_n;

	list_for_each_entry_safe(rmrru, rmrr_n, &dmar_rmrr_units, list) {
		list_del(&rmrru->list);
		dmar_free_dev_scope(&rmrru->devices, &rmrru->devices_cnt);
		kfree(rmrru);
	}

	list_for_each_entry_safe(atsru, atsr_n, &dmar_atsr_units, list) {
		list_del(&atsru->list);
		intel_iommu_free_atsr(atsru);
	}
	list_for_each_entry_safe(satcu, satc_n, &dmar_satc_units, list) {
		list_del(&satcu->list);
		dmar_free_dev_scope(&satcu->devices, &satcu->devices_cnt);
		kfree(satcu);
	}
}

static struct dmar_satc_unit *dmar_find_matched_satc_unit(struct pci_dev *dev)
{
	struct dmar_satc_unit *satcu;
	struct acpi_dmar_satc *satc;
	struct device *tmp;
	int i;

	rcu_read_lock();

	list_for_each_entry_rcu(satcu, &dmar_satc_units, list) {
		satc = container_of(satcu->hdr, struct acpi_dmar_satc, header);
		if (satc->segment != pci_domain_nr(dev->bus))
			continue;
		for_each_dev_scope(satcu->devices, satcu->devices_cnt, i, tmp)
			if (to_pci_dev(tmp) == dev)
				goto out;
	}
	satcu = NULL;
out:
	rcu_read_unlock();
	return satcu;
}

static bool dmar_ats_supported(struct pci_dev *dev, struct intel_iommu *iommu)
{
	struct pci_dev *bridge = NULL;
	struct dmar_atsr_unit *atsru;
	struct dmar_satc_unit *satcu;
	struct acpi_dmar_atsr *atsr;
	bool supported = true;
	struct pci_bus *bus;
	struct device *tmp;
	int i;

	dev = pci_physfn(dev);
	satcu = dmar_find_matched_satc_unit(dev);
	if (satcu)
		/*
		 * This device supports ATS as it is in SATC table.
		 * When IOMMU is in legacy mode, enabling ATS is done
		 * automatically by HW for the device that requires
		 * ATS, hence OS should not enable this device ATS
		 * to avoid duplicated TLB invalidation.
		 */
		return !(satcu->atc_required && !sm_supported(iommu));

	for (bus = dev->bus; bus; bus = bus->parent) {
		bridge = bus->self;
		/* If it's an integrated device, allow ATS */
		if (!bridge)
			return true;
		/* Connected via non-PCIe: no ATS */
		if (!pci_is_pcie(bridge) ||
		    pci_pcie_type(bridge) == PCI_EXP_TYPE_PCI_BRIDGE)
			return false;
		/* If we found the root port, look it up in the ATSR */
		if (pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT)
			break;
	}

	rcu_read_lock();
	list_for_each_entry_rcu(atsru, &dmar_atsr_units, list) {
		atsr = container_of(atsru->hdr, struct acpi_dmar_atsr, header);
		if (atsr->segment != pci_domain_nr(dev->bus))
			continue;

		for_each_dev_scope(atsru->devices, atsru->devices_cnt, i, tmp)
			if (tmp == &bridge->dev)
				goto out;

		if (atsru->include_all)
			goto out;
	}
	supported = false;
out:
	rcu_read_unlock();

	return supported;
}

int dmar_iommu_notify_scope_dev(struct dmar_pci_notify_info *info)
{
	int ret;
	struct dmar_rmrr_unit *rmrru;
	struct dmar_atsr_unit *atsru;
	struct dmar_satc_unit *satcu;
	struct acpi_dmar_atsr *atsr;
	struct acpi_dmar_reserved_memory *rmrr;
	struct acpi_dmar_satc *satc;

	if (!intel_iommu_enabled && system_state >= SYSTEM_RUNNING)
		return 0;

	list_for_each_entry(rmrru, &dmar_rmrr_units, list) {
		rmrr = container_of(rmrru->hdr,
				    struct acpi_dmar_reserved_memory, header);
		if (info->event == BUS_NOTIFY_ADD_DEVICE) {
			ret = dmar_insert_dev_scope(info, (void *)(rmrr + 1),
				((void *)rmrr) + rmrr->header.length,
				rmrr->segment, rmrru->devices,
				rmrru->devices_cnt);
			if (ret < 0)
				return ret;
		} else if (info->event == BUS_NOTIFY_REMOVED_DEVICE) {
			dmar_remove_dev_scope(info, rmrr->segment,
				rmrru->devices, rmrru->devices_cnt);
		}
	}

	list_for_each_entry(atsru, &dmar_atsr_units, list) {
		if (atsru->include_all)
			continue;

		atsr = container_of(atsru->hdr, struct acpi_dmar_atsr, header);
		if (info->event == BUS_NOTIFY_ADD_DEVICE) {
			ret = dmar_insert_dev_scope(info, (void *)(atsr + 1),
					(void *)atsr + atsr->header.length,
					atsr->segment, atsru->devices,
					atsru->devices_cnt);
			if (ret > 0)
				break;
			else if (ret < 0)
				return ret;
		} else if (info->event == BUS_NOTIFY_REMOVED_DEVICE) {
			if (dmar_remove_dev_scope(info, atsr->segment,
					atsru->devices, atsru->devices_cnt))
				break;
		}
	}
	list_for_each_entry(satcu, &dmar_satc_units, list) {
		satc = container_of(satcu->hdr, struct acpi_dmar_satc, header);
		if (info->event == BUS_NOTIFY_ADD_DEVICE) {
			ret = dmar_insert_dev_scope(info, (void *)(satc + 1),
					(void *)satc + satc->header.length,
					satc->segment, satcu->devices,
					satcu->devices_cnt);
			if (ret > 0)
				break;
			else if (ret < 0)
				return ret;
		} else if (info->event == BUS_NOTIFY_REMOVED_DEVICE) {
			if (dmar_remove_dev_scope(info, satc->segment,
					satcu->devices, satcu->devices_cnt))
				break;
		}
	}

	return 0;
}

static void intel_disable_iommus(void)
{
	struct intel_iommu *iommu = NULL;
	struct dmar_drhd_unit *drhd;

	for_each_iommu(iommu, drhd)
		iommu_disable_translation(iommu);
}

void intel_iommu_shutdown(void)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu = NULL;

	if (no_iommu || dmar_disabled)
		return;

	/*
	 * All other CPUs were brought down, hotplug interrupts were disabled,
	 * no lock and RCU checking needed anymore
	 */
	list_for_each_entry(drhd, &dmar_drhd_units, list) {
		iommu = drhd->iommu;

		/* Disable PMRs explicitly here. */
		iommu_disable_protect_mem_regions(iommu);

		/* Make sure the IOMMUs are switched off */
		iommu_disable_translation(iommu);
	}
}

static struct intel_iommu *dev_to_intel_iommu(struct device *dev)
{
	struct iommu_device *iommu_dev = dev_to_iommu_device(dev);

	return container_of(iommu_dev, struct intel_iommu, iommu);
}

static ssize_t version_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);
	u32 ver = readl(iommu->reg + DMAR_VER_REG);
	return sysfs_emit(buf, "%d:%d\n",
			  DMAR_VER_MAJOR(ver), DMAR_VER_MINOR(ver));
}
static DEVICE_ATTR_RO(version);

static ssize_t address_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);
	return sysfs_emit(buf, "%llx\n", iommu->reg_phys);
}
static DEVICE_ATTR_RO(address);

static ssize_t cap_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);
	return sysfs_emit(buf, "%llx\n", iommu->cap);
}
static DEVICE_ATTR_RO(cap);

static ssize_t ecap_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);
	return sysfs_emit(buf, "%llx\n", iommu->ecap);
}
static DEVICE_ATTR_RO(ecap);

static ssize_t domains_supported_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);
	return sysfs_emit(buf, "%ld\n", cap_ndoms(iommu->cap));
}
static DEVICE_ATTR_RO(domains_supported);

static ssize_t domains_used_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);
	unsigned int count = 0;
	int id;

	for (id = 0; id < cap_ndoms(iommu->cap); id++)
		if (ida_exists(&iommu->domain_ida, id))
			count++;

	return sysfs_emit(buf, "%d\n", count);
}
static DEVICE_ATTR_RO(domains_used);

static struct attribute *intel_iommu_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_address.attr,
	&dev_attr_cap.attr,
	&dev_attr_ecap.attr,
	&dev_attr_domains_supported.attr,
	&dev_attr_domains_used.attr,
	NULL,
};

static struct attribute_group intel_iommu_group = {
	.name = "intel-iommu",
	.attrs = intel_iommu_attrs,
};

const struct attribute_group *intel_iommu_groups[] = {
	&intel_iommu_group,
	NULL,
};

static bool has_external_pci(void)
{
	struct pci_dev *pdev = NULL;

	for_each_pci_dev(pdev)
		if (pdev->external_facing) {
			pci_dev_put(pdev);
			return true;
		}

	return false;
}

static int __init platform_optin_force_iommu(void)
{
	if (!dmar_platform_optin() || no_platform_optin || !has_external_pci())
		return 0;

	if (no_iommu || dmar_disabled)
		pr_info("Intel-IOMMU force enabled due to platform opt in\n");

	/*
	 * If Intel-IOMMU is disabled by default, we will apply identity
	 * map for all devices except those marked as being untrusted.
	 */
	if (dmar_disabled)
		iommu_set_default_passthrough(false);

	dmar_disabled = 0;
	no_iommu = 0;

	return 1;
}

static int __init probe_acpi_namespace_devices(void)
{
	struct dmar_drhd_unit *drhd;
	/* To avoid a -Wunused-but-set-variable warning. */
	struct intel_iommu *iommu __maybe_unused;
	struct device *dev;
	int i, ret = 0;

	for_each_active_iommu(iommu, drhd) {
		for_each_active_dev_scope(drhd->devices,
					  drhd->devices_cnt, i, dev) {
			struct acpi_device_physical_node *pn;
			struct acpi_device *adev;

			if (dev->bus != &acpi_bus_type)
				continue;

			up_read(&dmar_global_lock);
			adev = to_acpi_device(dev);
			mutex_lock(&adev->physical_node_lock);
			list_for_each_entry(pn,
					    &adev->physical_node_list, node) {
				ret = iommu_probe_device(pn->dev);
				if (ret)
					break;
			}
			mutex_unlock(&adev->physical_node_lock);
			down_read(&dmar_global_lock);

			if (ret)
				return ret;
		}
	}

	return 0;
}

static __init int tboot_force_iommu(void)
{
	if (!tboot_enabled())
		return 0;

	if (no_iommu || dmar_disabled)
		pr_warn("Forcing Intel-IOMMU to enabled\n");

	dmar_disabled = 0;
	no_iommu = 0;

	return 1;
}

int __init intel_iommu_init(void)
{
	int ret = -ENODEV;
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;

	/*
	 * Intel IOMMU is required for a TXT/tboot launch or platform
	 * opt in, so enforce that.
	 */
	force_on = (!intel_iommu_tboot_noforce && tboot_force_iommu()) ||
		    platform_optin_force_iommu();

	down_write(&dmar_global_lock);
	if (dmar_table_init()) {
		if (force_on)
			panic("tboot: Failed to initialize DMAR table\n");
		goto out_free_dmar;
	}

	if (dmar_dev_scope_init() < 0) {
		if (force_on)
			panic("tboot: Failed to initialize DMAR device scope\n");
		goto out_free_dmar;
	}

	up_write(&dmar_global_lock);

	/*
	 * The bus notifier takes the dmar_global_lock, so lockdep will
	 * complain later when we register it under the lock.
	 */
	dmar_register_bus_notifier();

	down_write(&dmar_global_lock);

	if (!no_iommu)
		intel_iommu_debugfs_init();

	if (no_iommu || dmar_disabled) {
		/*
		 * We exit the function here to ensure IOMMU's remapping and
		 * mempool aren't setup, which means that the IOMMU's PMRs
		 * won't be disabled via the call to init_dmars(). So disable
		 * it explicitly here. The PMRs were setup by tboot prior to
		 * calling SENTER, but the kernel is expected to reset/tear
		 * down the PMRs.
		 */
		if (intel_iommu_tboot_noforce) {
			for_each_iommu(iommu, drhd)
				iommu_disable_protect_mem_regions(iommu);
		}

		/*
		 * Make sure the IOMMUs are switched off, even when we
		 * boot into a kexec kernel and the previous kernel left
		 * them enabled
		 */
		intel_disable_iommus();
		goto out_free_dmar;
	}

	if (list_empty(&dmar_rmrr_units))
		pr_info("No RMRR found\n");

	if (list_empty(&dmar_atsr_units))
		pr_info("No ATSR found\n");

	if (list_empty(&dmar_satc_units))
		pr_info("No SATC found\n");

	init_no_remapping_devices();

	ret = init_dmars();
	if (ret) {
		if (force_on)
			panic("tboot: Failed to initialize DMARs\n");
		pr_err("Initialization failed\n");
		goto out_free_dmar;
	}
	up_write(&dmar_global_lock);

	init_iommu_pm_ops();

	down_read(&dmar_global_lock);
	for_each_active_iommu(iommu, drhd) {
		/*
		 * The flush queue implementation does not perform
		 * page-selective invalidations that are required for efficient
		 * TLB flushes in virtual environments.  The benefit of batching
		 * is likely to be much lower than the overhead of synchronizing
		 * the virtual and physical IOMMU page-tables.
		 */
		if (cap_caching_mode(iommu->cap) &&
		    !first_level_by_default(iommu)) {
			pr_info_once("IOMMU batching disallowed due to virtualization\n");
			iommu_set_dma_strict();
		}
		iommu_device_sysfs_add(&iommu->iommu, NULL,
				       intel_iommu_groups,
				       "%s", iommu->name);
		/*
		 * The iommu device probe is protected by the iommu_probe_device_lock.
		 * Release the dmar_global_lock before entering the device probe path
		 * to avoid unnecessary lock order splat.
		 */
		up_read(&dmar_global_lock);
		iommu_device_register(&iommu->iommu, &intel_iommu_ops, NULL);
		down_read(&dmar_global_lock);

		iommu_pmu_register(iommu);
	}

	if (probe_acpi_namespace_devices())
		pr_warn("ACPI name space devices didn't probe correctly\n");

	/* Finally, we enable the DMA remapping hardware. */
	for_each_iommu(iommu, drhd) {
		if (!drhd->ignored && !translation_pre_enabled(iommu))
			iommu_enable_translation(iommu);

		iommu_disable_protect_mem_regions(iommu);
	}
	up_read(&dmar_global_lock);

	pr_info("Intel(R) Virtualization Technology for Directed I/O\n");

	intel_iommu_enabled = 1;

	return 0;

out_free_dmar:
	intel_iommu_free_dmars();
	up_write(&dmar_global_lock);
	return ret;
}

static int domain_context_clear_one_cb(struct pci_dev *pdev, u16 alias, void *opaque)
{
	struct device_domain_info *info = opaque;

	domain_context_clear_one(info, PCI_BUS_NUM(alias), alias & 0xff);
	return 0;
}

/*
 * NB - intel-iommu lacks any sort of reference counting for the users of
 * dependent devices.  If multiple endpoints have intersecting dependent
 * devices, unbinding the driver from any one of them will possibly leave
 * the others unable to operate.
 */
static void domain_context_clear(struct device_domain_info *info)
{
	if (!dev_is_pci(info->dev)) {
		domain_context_clear_one(info, info->bus, info->devfn);
		return;
	}

	pci_for_each_dma_alias(to_pci_dev(info->dev),
			       &domain_context_clear_one_cb, info);
	iommu_disable_pci_ats(info);
}

/*
 * Clear the page table pointer in context or pasid table entries so that
 * all DMA requests without PASID from the device are blocked. If the page
 * table has been set, clean up the data structures.
 */
void device_block_translation(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	unsigned long flags;

	/* Device in DMA blocking state. Noting to do. */
	if (!info->domain_attached)
		return;

	if (info->domain)
		cache_tag_unassign_domain(info->domain, dev, IOMMU_NO_PASID);

	if (!dev_is_real_dma_subdevice(dev)) {
		if (sm_supported(iommu))
			intel_pasid_tear_down_entry(iommu, dev,
						    IOMMU_NO_PASID, false);
		else
			domain_context_clear(info);
	}

	/* Device now in DMA blocking state. */
	info->domain_attached = false;

	if (!info->domain)
		return;

	spin_lock_irqsave(&info->domain->lock, flags);
	list_del(&info->link);
	spin_unlock_irqrestore(&info->domain->lock, flags);

	domain_detach_iommu(info->domain, iommu);
	info->domain = NULL;
}

static int blocking_domain_attach_dev(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	iopf_for_domain_remove(info->domain ? &info->domain->domain : NULL, dev);
	device_block_translation(dev);
	return 0;
}

static int blocking_domain_set_dev_pasid(struct iommu_domain *domain,
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old);

static struct iommu_domain blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev	= blocking_domain_attach_dev,
		.set_dev_pasid	= blocking_domain_set_dev_pasid,
	}
};

static struct dmar_domain *paging_domain_alloc(void)
{
	struct dmar_domain *domain;

	domain = kzalloc_obj(*domain);
	if (!domain)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&domain->devices);
	INIT_LIST_HEAD(&domain->dev_pasids);
	INIT_LIST_HEAD(&domain->cache_tags);
	spin_lock_init(&domain->lock);
	spin_lock_init(&domain->cache_lock);
	xa_init(&domain->iommu_array);
	INIT_LIST_HEAD(&domain->s1_domains);
	spin_lock_init(&domain->s1_lock);

	return domain;
}

static unsigned int compute_vasz_lg2_fs(struct intel_iommu *iommu,
					unsigned int *top_level)
{
	unsigned int mgaw = cap_mgaw(iommu->cap);

	/*
	 * Spec 3.6 First-Stage Translation:
	 *
	 * Software must limit addresses to less than the minimum of MGAW
	 * and the lower canonical address width implied by FSPM (i.e.,
	 * 47-bit when FSPM is 4-level and 56-bit when FSPM is 5-level).
	 */
	if (mgaw > 48 && cap_fl5lp_support(iommu->cap)) {
		*top_level = 4;
		return min(57, mgaw);
	}

	/* Four level is always supported */
	*top_level = 3;
	return min(48, mgaw);
}

static struct iommu_domain *
intel_iommu_domain_alloc_first_stage(struct device *dev,
				     struct intel_iommu *iommu, u32 flags)
{
	struct pt_iommu_x86_64_cfg cfg = {};
	struct dmar_domain *dmar_domain;
	int ret;

	if (flags & ~IOMMU_HWPT_ALLOC_PASID)
		return ERR_PTR(-EOPNOTSUPP);

	/* Only SL is available in legacy mode */
	if (!sm_supported(iommu) || !ecap_flts(iommu->ecap))
		return ERR_PTR(-EOPNOTSUPP);

	dmar_domain = paging_domain_alloc();
	if (IS_ERR(dmar_domain))
		return ERR_CAST(dmar_domain);

	cfg.common.hw_max_vasz_lg2 =
		compute_vasz_lg2_fs(iommu, &cfg.top_level);
	cfg.common.hw_max_oasz_lg2 = 52;
	cfg.common.features = BIT(PT_FEAT_SIGN_EXTEND) |
			      BIT(PT_FEAT_FLUSH_RANGE);
	/* First stage always uses scalable mode */
	if (!ecap_smpwc(iommu->ecap))
		cfg.common.features |= BIT(PT_FEAT_DMA_INCOHERENT);
	dmar_domain->iommu.iommu_device = dev;
	dmar_domain->iommu.nid = dev_to_node(dev);
	dmar_domain->domain.ops = &intel_fs_paging_domain_ops;
	/*
	 * iotlb sync for map is only needed for legacy implementations that
	 * explicitly require flushing internal write buffers to ensure memory
	 * coherence.
	 */
	if (rwbf_required(iommu))
		dmar_domain->iotlb_sync_map = true;

	ret = pt_iommu_x86_64_init(&dmar_domain->fspt, &cfg, GFP_KERNEL);
	if (ret) {
		kfree(dmar_domain);
		return ERR_PTR(ret);
	}

	if (!cap_fl1gp_support(iommu->cap))
		dmar_domain->domain.pgsize_bitmap &= ~(u64)SZ_1G;
	if (!intel_iommu_superpage)
		dmar_domain->domain.pgsize_bitmap = SZ_4K;

	return &dmar_domain->domain;
}

static unsigned int compute_vasz_lg2_ss(struct intel_iommu *iommu,
					unsigned int *top_level)
{
	unsigned int sagaw = cap_sagaw(iommu->cap);
	unsigned int mgaw = cap_mgaw(iommu->cap);

	/*
	 * Find the largest table size that both the mgaw and sagaw support.
	 * This sets the valid range of IOVA and the top starting level.
	 * Some HW may only support a 4 or 5 level walk but must limit IOVA to
	 * 3 levels.
	 */
	if (mgaw > 48 && sagaw >= BIT(3)) {
		*top_level = 4;
		return min(57, mgaw);
	} else if (mgaw > 39 && sagaw >= BIT(2)) {
		*top_level = 3 + ffs(sagaw >> 3);
		return min(48, mgaw);
	} else if (mgaw > 30 && sagaw >= BIT(1)) {
		*top_level = 2 + ffs(sagaw >> 2);
		return min(39, mgaw);
	}
	return 0;
}

static const struct iommu_dirty_ops intel_second_stage_dirty_ops = {
	IOMMU_PT_DIRTY_OPS(vtdss),
	.set_dirty_tracking = intel_iommu_set_dirty_tracking,
};

static struct iommu_domain *
intel_iommu_domain_alloc_second_stage(struct device *dev,
				      struct intel_iommu *iommu, u32 flags)
{
	struct pt_iommu_vtdss_cfg cfg = {};
	struct dmar_domain *dmar_domain;
	unsigned int sslps;
	int ret;

	if (flags &
	    (~(IOMMU_HWPT_ALLOC_NEST_PARENT | IOMMU_HWPT_ALLOC_DIRTY_TRACKING |
	       IOMMU_HWPT_ALLOC_PASID)))
		return ERR_PTR(-EOPNOTSUPP);

	if (((flags & IOMMU_HWPT_ALLOC_NEST_PARENT) &&
	     !nested_supported(iommu)) ||
	    ((flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING) &&
	     !ssads_supported(iommu)))
		return ERR_PTR(-EOPNOTSUPP);

	/* Legacy mode always supports second stage */
	if (sm_supported(iommu) && !ecap_slts(iommu->ecap))
		return ERR_PTR(-EOPNOTSUPP);

	dmar_domain = paging_domain_alloc();
	if (IS_ERR(dmar_domain))
		return ERR_CAST(dmar_domain);

	cfg.common.hw_max_vasz_lg2 = compute_vasz_lg2_ss(iommu, &cfg.top_level);
	cfg.common.hw_max_oasz_lg2 = 52;
	cfg.common.features = BIT(PT_FEAT_FLUSH_RANGE);

	/*
	 * Read-only mapping is disallowed on the domain which serves as the
	 * parent in a nested configuration, due to HW errata
	 * (ERRATA_772415_SPR17)
	 */
	if (flags & IOMMU_HWPT_ALLOC_NEST_PARENT)
		cfg.common.features |= BIT(PT_FEAT_VTDSS_FORCE_WRITEABLE);

	if (!iommu_paging_structure_coherency(iommu))
		cfg.common.features |= BIT(PT_FEAT_DMA_INCOHERENT);
	dmar_domain->iommu.iommu_device = dev;
	dmar_domain->iommu.nid = dev_to_node(dev);
	dmar_domain->domain.ops = &intel_ss_paging_domain_ops;
	dmar_domain->nested_parent = flags & IOMMU_HWPT_ALLOC_NEST_PARENT;

	if (flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING)
		dmar_domain->domain.dirty_ops = &intel_second_stage_dirty_ops;

	ret = pt_iommu_vtdss_init(&dmar_domain->sspt, &cfg, GFP_KERNEL);
	if (ret) {
		kfree(dmar_domain);
		return ERR_PTR(ret);
	}

	/* Adjust the supported page sizes to HW capability */
	sslps = cap_super_page_val(iommu->cap);
	if (!(sslps & BIT(0)))
		dmar_domain->domain.pgsize_bitmap &= ~(u64)SZ_2M;
	if (!(sslps & BIT(1)))
		dmar_domain->domain.pgsize_bitmap &= ~(u64)SZ_1G;
	if (!intel_iommu_superpage)
		dmar_domain->domain.pgsize_bitmap = SZ_4K;

	/*
	 * Besides the internal write buffer flush, the caching mode used for
	 * legacy nested translation (which utilizes shadowing page tables)
	 * also requires iotlb sync on map.
	 */
	if (rwbf_required(iommu) || cap_caching_mode(iommu->cap))
		dmar_domain->iotlb_sync_map = true;

	return &dmar_domain->domain;
}

static struct iommu_domain *
intel_iommu_domain_alloc_paging_flags(struct device *dev, u32 flags,
				      const struct iommu_user_data *user_data)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	struct iommu_domain *domain;

	if (user_data)
		return ERR_PTR(-EOPNOTSUPP);

	/* Prefer first stage if possible by default. */
	domain = intel_iommu_domain_alloc_first_stage(dev, iommu, flags);
	if (domain != ERR_PTR(-EOPNOTSUPP))
		return domain;
	return intel_iommu_domain_alloc_second_stage(dev, iommu, flags);
}

static void intel_iommu_domain_free(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);

	if (WARN_ON(dmar_domain->nested_parent &&
		    !list_empty(&dmar_domain->s1_domains)))
		return;

	if (WARN_ON(!list_empty(&dmar_domain->devices)))
		return;

	pt_iommu_deinit(&dmar_domain->iommu);

	kfree(dmar_domain->qi_batch);
	kfree(dmar_domain);
}

static int paging_domain_compatible_first_stage(struct dmar_domain *dmar_domain,
						struct intel_iommu *iommu)
{
	if (WARN_ON(dmar_domain->domain.dirty_ops ||
		    dmar_domain->nested_parent))
		return -EINVAL;

	/* Only SL is available in legacy mode */
	if (!sm_supported(iommu) || !ecap_flts(iommu->ecap))
		return -EINVAL;

	if (!ecap_smpwc(iommu->ecap) &&
	    !(dmar_domain->fspt.x86_64_pt.common.features &
	      BIT(PT_FEAT_DMA_INCOHERENT)))
		return -EINVAL;

	/* Supports the number of table levels */
	if (!cap_fl5lp_support(iommu->cap) &&
	    dmar_domain->fspt.x86_64_pt.common.max_vasz_lg2 > 48)
		return -EINVAL;

	/* Same page size support */
	if (!cap_fl1gp_support(iommu->cap) &&
	    (dmar_domain->domain.pgsize_bitmap & SZ_1G))
		return -EINVAL;

	/* iotlb sync on map requirement */
	if ((rwbf_required(iommu)) && !dmar_domain->iotlb_sync_map)
		return -EINVAL;

	return 0;
}

static int
paging_domain_compatible_second_stage(struct dmar_domain *dmar_domain,
				      struct intel_iommu *iommu)
{
	unsigned int vasz_lg2 = dmar_domain->sspt.vtdss_pt.common.max_vasz_lg2;
	unsigned int sslps = cap_super_page_val(iommu->cap);
	struct pt_iommu_vtdss_hw_info pt_info;

	pt_iommu_vtdss_hw_info(&dmar_domain->sspt, &pt_info);

	if (dmar_domain->domain.dirty_ops && !ssads_supported(iommu))
		return -EINVAL;
	if (dmar_domain->nested_parent && !nested_supported(iommu))
		return -EINVAL;

	/* Legacy mode always supports second stage */
	if (sm_supported(iommu) && !ecap_slts(iommu->ecap))
		return -EINVAL;

	if (!iommu_paging_structure_coherency(iommu) &&
	    !(dmar_domain->sspt.vtdss_pt.common.features &
	      BIT(PT_FEAT_DMA_INCOHERENT)))
		return -EINVAL;

	/* Address width falls within the capability */
	if (cap_mgaw(iommu->cap) < vasz_lg2)
		return -EINVAL;

	/* Page table level is supported. */
	if (!(cap_sagaw(iommu->cap) & BIT(pt_info.aw)))
		return -EINVAL;

	/* Same page size support */
	if (!(sslps & BIT(0)) && (dmar_domain->domain.pgsize_bitmap & SZ_2M))
		return -EINVAL;
	if (!(sslps & BIT(1)) && (dmar_domain->domain.pgsize_bitmap & SZ_1G))
		return -EINVAL;

	/* iotlb sync on map requirement */
	if ((rwbf_required(iommu) || cap_caching_mode(iommu->cap)) &&
	    !dmar_domain->iotlb_sync_map)
		return -EINVAL;

	/*
	 * FIXME this is locked wrong, it needs to be under the
	 * dmar_domain->lock
	 */
	if ((dmar_domain->sspt.vtdss_pt.common.features &
	     BIT(PT_FEAT_VTDSS_FORCE_COHERENCE)) &&
	    !ecap_sc_support(iommu->ecap))
		return -EINVAL;
	return 0;
}

int paging_domain_compatible(struct iommu_domain *domain, struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	struct intel_iommu *iommu = info->iommu;
	int ret = -EINVAL;

	if (intel_domain_is_fs_paging(dmar_domain))
		ret = paging_domain_compatible_first_stage(dmar_domain, iommu);
	else if (intel_domain_is_ss_paging(dmar_domain))
		ret = paging_domain_compatible_second_stage(dmar_domain, iommu);
	else if (WARN_ON(true))
		ret = -EINVAL;
	if (ret)
		return ret;

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev) &&
	    context_copied(iommu, info->bus, info->devfn))
		return intel_pasid_setup_sm_context(dev);

	return 0;
}

static int intel_iommu_attach_device(struct iommu_domain *domain,
				     struct device *dev,
				     struct iommu_domain *old)
{
	int ret;

	device_block_translation(dev);

	ret = paging_domain_compatible(domain, dev);
	if (ret)
		return ret;

	ret = iopf_for_domain_set(domain, dev);
	if (ret)
		return ret;

	ret = dmar_domain_attach_device(to_dmar_domain(domain), dev);
	if (ret)
		iopf_for_domain_remove(domain, dev);

	return ret;
}

static void intel_iommu_tlb_sync(struct iommu_domain *domain,
				 struct iommu_iotlb_gather *gather)
{
	cache_tag_flush_range(to_dmar_domain(domain), gather->start,
			      gather->end,
			      iommu_pages_list_empty(&gather->freelist));
	iommu_put_pages_list(&gather->freelist);
}

static bool domain_support_force_snooping(struct dmar_domain *domain)
{
	struct device_domain_info *info;
	bool support = true;

	assert_spin_locked(&domain->lock);
	list_for_each_entry(info, &domain->devices, link) {
		if (!ecap_sc_support(info->iommu->ecap)) {
			support = false;
			break;
		}
	}

	return support;
}

static bool intel_iommu_enforce_cache_coherency_fs(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	struct device_domain_info *info;

	guard(spinlock_irqsave)(&dmar_domain->lock);

	if (dmar_domain->force_snooping)
		return true;

	if (!domain_support_force_snooping(dmar_domain))
		return false;

	dmar_domain->force_snooping = true;
	list_for_each_entry(info, &dmar_domain->devices, link)
		intel_pasid_setup_page_snoop_control(info->iommu, info->dev,
						     IOMMU_NO_PASID);
	return true;
}

static bool intel_iommu_enforce_cache_coherency_ss(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);

	guard(spinlock_irqsave)(&dmar_domain->lock);
	if (!domain_support_force_snooping(dmar_domain))
		return false;

	/*
	 * Second level page table supports per-PTE snoop control. The
	 * iommu_map() interface will handle this by setting SNP bit.
	 */
	dmar_domain->sspt.vtdss_pt.common.features |=
		BIT(PT_FEAT_VTDSS_FORCE_COHERENCE);
	dmar_domain->force_snooping = true;
	return true;
}

static bool intel_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	case IOMMU_CAP_PRE_BOOT_PROTECTION:
		return dmar_platform_optin();
	case IOMMU_CAP_ENFORCE_CACHE_COHERENCY:
		return ecap_sc_support(info->iommu->ecap);
	case IOMMU_CAP_DIRTY_TRACKING:
		return ssads_supported(info->iommu);
	case IOMMU_CAP_PCI_ATS_SUPPORTED:
		return info->ats_supported;
	default:
		return false;
	}
}

static struct iommu_device *intel_iommu_probe_device(struct device *dev)
{
	struct pci_dev *pdev = dev_is_pci(dev) ? to_pci_dev(dev) : NULL;
	struct device_domain_info *info;
	struct intel_iommu *iommu;
	u8 bus, devfn;
	int ret;

	iommu = device_lookup_iommu(dev, &bus, &devfn);
	if (!iommu || !iommu->iommu.ops)
		return ERR_PTR(-ENODEV);

	info = kzalloc_obj(*info);
	if (!info)
		return ERR_PTR(-ENOMEM);

	if (dev_is_real_dma_subdevice(dev)) {
		info->bus = pdev->bus->number;
		info->devfn = pdev->devfn;
		info->segment = pci_domain_nr(pdev->bus);
	} else {
		info->bus = bus;
		info->devfn = devfn;
		info->segment = iommu->segment;
	}

	info->dev = dev;
	info->iommu = iommu;
	if (dev_is_pci(dev)) {
		if (ecap_dev_iotlb_support(iommu->ecap) &&
		    pci_ats_supported(pdev) &&
		    dmar_ats_supported(pdev, iommu)) {
			info->ats_supported = 1;
			info->dtlb_extra_inval = dev_needs_extra_dtlb_flush(pdev);

			/*
			 * For IOMMU that supports device IOTLB throttling
			 * (DIT), we assign PFSID to the invalidation desc
			 * of a VF such that IOMMU HW can gauge queue depth
			 * at PF level. If DIT is not set, PFSID will be
			 * treated as reserved, which should be set to 0.
			 */
			if (ecap_dit(iommu->ecap))
				info->pfsid = pci_dev_id(pci_physfn(pdev));
			info->ats_qdep = pci_ats_queue_depth(pdev);
		}
		if (sm_supported(iommu)) {
			if (pasid_supported(iommu)) {
				int features = pci_pasid_features(pdev);

				if (features >= 0)
					info->pasid_supported = features | 1;
			}

			if (info->ats_supported && ecap_prs(iommu->ecap) &&
			    ecap_pds(iommu->ecap) && pci_pri_supported(pdev))
				info->pri_supported = 1;
		}
	}

	dev_iommu_priv_set(dev, info);
	if (pdev && pci_ats_supported(pdev)) {
		pci_prepare_ats(pdev, VTD_PAGE_SHIFT);
		ret = device_rbtree_insert(iommu, info);
		if (ret)
			goto free;
	}

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev)) {
		ret = intel_pasid_alloc_table(dev);
		if (ret) {
			dev_err(dev, "PASID table allocation failed\n");
			goto clear_rbtree;
		}

		if (!context_copied(iommu, info->bus, info->devfn)) {
			ret = intel_pasid_setup_sm_context(dev);
			if (ret)
				goto free_table;
		}
	}

	intel_iommu_debugfs_create_dev(info);

	return &iommu->iommu;
free_table:
	intel_pasid_free_table(dev);
clear_rbtree:
	device_rbtree_remove(info);
free:
	kfree(info);

	return ERR_PTR(ret);
}

static void intel_iommu_probe_finalize(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;

	/*
	 * The PCIe spec, in its wisdom, declares that the behaviour of the
	 * device is undefined if you enable PASID support after ATS support.
	 * So always enable PASID support on devices which have it, even if
	 * we can't yet know if we're ever going to use it.
	 */
	if (info->pasid_supported &&
	    !pci_enable_pasid(to_pci_dev(dev), info->pasid_supported & ~1))
		info->pasid_enabled = 1;

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev)) {
		iommu_enable_pci_ats(info);
		/* Assign a DEVTLB cache tag to the default domain. */
		if (info->ats_enabled && info->domain) {
			u16 did = domain_id_iommu(info->domain, iommu);

			if (cache_tag_assign(info->domain, did, dev,
					     IOMMU_NO_PASID, CACHE_TAG_DEVTLB))
				iommu_disable_pci_ats(info);
		}
	}
	iommu_enable_pci_pri(info);
}

static void intel_iommu_release_device(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;

	iommu_disable_pci_pri(info);
	iommu_disable_pci_ats(info);

	if (info->pasid_enabled) {
		pci_disable_pasid(to_pci_dev(dev));
		info->pasid_enabled = 0;
	}

	mutex_lock(&iommu->iopf_lock);
	if (dev_is_pci(dev) && pci_ats_supported(to_pci_dev(dev)))
		device_rbtree_remove(info);
	mutex_unlock(&iommu->iopf_lock);

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev) &&
	    !context_copied(iommu, info->bus, info->devfn))
		intel_pasid_teardown_sm_context(dev);

	intel_pasid_free_table(dev);
	intel_iommu_debugfs_remove_dev(info);
	kfree(info);
}

static void intel_iommu_get_resv_regions(struct device *device,
					 struct list_head *head)
{
	int prot = DMA_PTE_READ | DMA_PTE_WRITE;
	struct iommu_resv_region *reg;
	struct dmar_rmrr_unit *rmrr;
	struct device *i_dev;
	int i;

	rcu_read_lock();
	for_each_rmrr_units(rmrr) {
		for_each_active_dev_scope(rmrr->devices, rmrr->devices_cnt,
					  i, i_dev) {
			struct iommu_resv_region *resv;
			enum iommu_resv_type type;
			size_t length;

			if (i_dev != device &&
			    !is_downstream_to_pci_bridge(device, i_dev))
				continue;

			length = rmrr->end_address - rmrr->base_address + 1;

			type = device_rmrr_is_relaxable(device) ?
				IOMMU_RESV_DIRECT_RELAXABLE : IOMMU_RESV_DIRECT;

			resv = iommu_alloc_resv_region(rmrr->base_address,
						       length, prot, type,
						       GFP_ATOMIC);
			if (!resv)
				break;

			list_add_tail(&resv->list, head);
		}
	}
	rcu_read_unlock();

#ifdef CONFIG_INTEL_IOMMU_FLOPPY_WA
	if (dev_is_pci(device)) {
		struct pci_dev *pdev = to_pci_dev(device);

		if ((pdev->class >> 8) == PCI_CLASS_BRIDGE_ISA) {
			reg = iommu_alloc_resv_region(0, 1UL << 24, prot,
					IOMMU_RESV_DIRECT_RELAXABLE,
					GFP_KERNEL);
			if (reg)
				list_add_tail(&reg->list, head);
		}
	}
#endif /* CONFIG_INTEL_IOMMU_FLOPPY_WA */

	reg = iommu_alloc_resv_region(IOAPIC_RANGE_START,
				      IOAPIC_RANGE_END - IOAPIC_RANGE_START + 1,
				      0, IOMMU_RESV_MSI, GFP_KERNEL);
	if (!reg)
		return;
	list_add_tail(&reg->list, head);
}

static struct iommu_group *intel_iommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	return generic_device_group(dev);
}

int intel_iommu_enable_iopf(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	int ret;

	if (!info->pri_enabled)
		return -ENODEV;

	/* pri_enabled is protected by the group mutex. */
	iommu_group_mutex_assert(dev);
	if (info->iopf_refcount) {
		info->iopf_refcount++;
		return 0;
	}

	ret = iopf_queue_add_device(iommu->iopf_queue, dev);
	if (ret)
		return ret;

	info->iopf_refcount = 1;

	return 0;
}

void intel_iommu_disable_iopf(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;

	if (WARN_ON(!info->pri_enabled || !info->iopf_refcount))
		return;

	iommu_group_mutex_assert(dev);
	if (--info->iopf_refcount)
		return;

	iopf_queue_remove_device(iommu->iopf_queue, dev);
}

static bool intel_iommu_is_attach_deferred(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	return translation_pre_enabled(info->iommu) && !info->domain;
}

/*
 * Check that the device does not live on an external facing PCI port that is
 * marked as untrusted. Such devices should not be able to apply quirks and
 * thus not be able to bypass the IOMMU restrictions.
 */
static bool risky_device(struct pci_dev *pdev)
{
	if (pdev->untrusted) {
		pci_info(pdev,
			 "Skipping IOMMU quirk for dev [%04X:%04X] on untrusted PCI link\n",
			 pdev->vendor, pdev->device);
		pci_info(pdev, "Please check with your BIOS/Platform vendor about this\n");
		return true;
	}
	return false;
}

static int intel_iommu_iotlb_sync_map(struct iommu_domain *domain,
				      unsigned long iova, size_t size)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);

	if (dmar_domain->iotlb_sync_map)
		cache_tag_flush_range_np(dmar_domain, iova, iova + size - 1);

	return 0;
}

void domain_remove_dev_pasid(struct iommu_domain *domain,
			     struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct dev_pasid_info *curr, *dev_pasid = NULL;
	struct intel_iommu *iommu = info->iommu;
	struct dmar_domain *dmar_domain;
	unsigned long flags;

	if (!domain)
		return;

	/* Identity domain has no meta data for pasid. */
	if (domain->type == IOMMU_DOMAIN_IDENTITY)
		return;

	dmar_domain = to_dmar_domain(domain);
	spin_lock_irqsave(&dmar_domain->lock, flags);
	list_for_each_entry(curr, &dmar_domain->dev_pasids, link_domain) {
		if (curr->dev == dev && curr->pasid == pasid) {
			list_del(&curr->link_domain);
			dev_pasid = curr;
			break;
		}
	}
	spin_unlock_irqrestore(&dmar_domain->lock, flags);

	cache_tag_unassign_domain(dmar_domain, dev, pasid);
	domain_detach_iommu(dmar_domain, iommu);
	if (!WARN_ON_ONCE(!dev_pasid)) {
		intel_iommu_debugfs_remove_dev_pasid(dev_pasid);
		kfree(dev_pasid);
	}
}

static int blocking_domain_set_dev_pasid(struct iommu_domain *domain,
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	intel_pasid_tear_down_entry(info->iommu, dev, pasid, false);
	iopf_for_domain_remove(old, dev);
	domain_remove_dev_pasid(old, dev, pasid);

	return 0;
}

struct dev_pasid_info *
domain_add_dev_pasid(struct iommu_domain *domain,
		     struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	struct intel_iommu *iommu = info->iommu;
	struct dev_pasid_info *dev_pasid;
	unsigned long flags;
	int ret;

	dev_pasid = kzalloc_obj(*dev_pasid);
	if (!dev_pasid)
		return ERR_PTR(-ENOMEM);

	ret = domain_attach_iommu(dmar_domain, iommu);
	if (ret)
		goto out_free;

	ret = cache_tag_assign_domain(dmar_domain, dev, pasid);
	if (ret)
		goto out_detach_iommu;

	dev_pasid->dev = dev;
	dev_pasid->pasid = pasid;
	spin_lock_irqsave(&dmar_domain->lock, flags);
	list_add(&dev_pasid->link_domain, &dmar_domain->dev_pasids);
	spin_unlock_irqrestore(&dmar_domain->lock, flags);

	return dev_pasid;
out_detach_iommu:
	domain_detach_iommu(dmar_domain, iommu);
out_free:
	kfree(dev_pasid);
	return ERR_PTR(ret);
}

static int intel_iommu_set_dev_pasid(struct iommu_domain *domain,
				     struct device *dev, ioasid_t pasid,
				     struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	struct intel_iommu *iommu = info->iommu;
	struct dev_pasid_info *dev_pasid;
	int ret;

	if (WARN_ON_ONCE(!(domain->type & __IOMMU_DOMAIN_PAGING)))
		return -EINVAL;

	if (!pasid_supported(iommu) || dev_is_real_dma_subdevice(dev))
		return -EOPNOTSUPP;

	if (context_copied(iommu, info->bus, info->devfn))
		return -EBUSY;

	ret = paging_domain_compatible(domain, dev);
	if (ret)
		return ret;

	dev_pasid = domain_add_dev_pasid(domain, dev, pasid);
	if (IS_ERR(dev_pasid))
		return PTR_ERR(dev_pasid);

	ret = iopf_for_domain_replace(domain, old, dev);
	if (ret)
		goto out_remove_dev_pasid;

	if (intel_domain_is_fs_paging(dmar_domain))
		ret = domain_setup_first_level(iommu, dmar_domain,
					       dev, pasid, old);
	else if (intel_domain_is_ss_paging(dmar_domain))
		ret = domain_setup_second_level(iommu, dmar_domain,
						dev, pasid, old);
	else if (WARN_ON(true))
		ret = -EINVAL;

	if (ret)
		goto out_unwind_iopf;

	domain_remove_dev_pasid(old, dev, pasid);

	intel_iommu_debugfs_create_dev_pasid(dev_pasid);

	return 0;

out_unwind_iopf:
	iopf_for_domain_replace(old, domain, dev);
out_remove_dev_pasid:
	domain_remove_dev_pasid(domain, dev, pasid);
	return ret;
}

static void *intel_iommu_hw_info(struct device *dev, u32 *length,
				 enum iommu_hw_info_type *type)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	struct iommu_hw_info_vtd *vtd;

	if (*type != IOMMU_HW_INFO_TYPE_DEFAULT &&
	    *type != IOMMU_HW_INFO_TYPE_INTEL_VTD)
		return ERR_PTR(-EOPNOTSUPP);

	vtd = kzalloc_obj(*vtd);
	if (!vtd)
		return ERR_PTR(-ENOMEM);

	vtd->flags = IOMMU_HW_INFO_VTD_ERRATA_772415_SPR17;
	vtd->cap_reg = iommu->cap;
	vtd->ecap_reg = iommu->ecap;
	*length = sizeof(*vtd);
	*type = IOMMU_HW_INFO_TYPE_INTEL_VTD;
	return vtd;
}

/* Set dirty tracking for the devices that the domain has been attached. */
static int domain_set_dirty_tracking(struct dmar_domain *domain, bool enable)
{
	struct device_domain_info *info;
	struct dev_pasid_info *dev_pasid;
	int ret = 0;

	lockdep_assert_held(&domain->lock);

	list_for_each_entry(info, &domain->devices, link) {
		ret = intel_pasid_setup_dirty_tracking(info->iommu, info->dev,
						       IOMMU_NO_PASID, enable);
		if (ret)
			return ret;
	}

	list_for_each_entry(dev_pasid, &domain->dev_pasids, link_domain) {
		info = dev_iommu_priv_get(dev_pasid->dev);
		ret = intel_pasid_setup_dirty_tracking(info->iommu, info->dev,
						       dev_pasid->pasid, enable);
		if (ret)
			break;
	}

	return ret;
}

static int parent_domain_set_dirty_tracking(struct dmar_domain *domain,
					    bool enable)
{
	struct dmar_domain *s1_domain;
	unsigned long flags;
	int ret;

	spin_lock(&domain->s1_lock);
	list_for_each_entry(s1_domain, &domain->s1_domains, s2_link) {
		spin_lock_irqsave(&s1_domain->lock, flags);
		ret = domain_set_dirty_tracking(s1_domain, enable);
		spin_unlock_irqrestore(&s1_domain->lock, flags);
		if (ret)
			goto err_unwind;
	}
	spin_unlock(&domain->s1_lock);
	return 0;

err_unwind:
	list_for_each_entry(s1_domain, &domain->s1_domains, s2_link) {
		spin_lock_irqsave(&s1_domain->lock, flags);
		domain_set_dirty_tracking(s1_domain, domain->dirty_tracking);
		spin_unlock_irqrestore(&s1_domain->lock, flags);
	}
	spin_unlock(&domain->s1_lock);
	return ret;
}

static int intel_iommu_set_dirty_tracking(struct iommu_domain *domain,
					  bool enable)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	int ret;

	spin_lock(&dmar_domain->lock);
	if (dmar_domain->dirty_tracking == enable)
		goto out_unlock;

	ret = domain_set_dirty_tracking(dmar_domain, enable);
	if (ret)
		goto err_unwind;

	if (dmar_domain->nested_parent) {
		ret = parent_domain_set_dirty_tracking(dmar_domain, enable);
		if (ret)
			goto err_unwind;
	}

	dmar_domain->dirty_tracking = enable;
out_unlock:
	spin_unlock(&dmar_domain->lock);

	return 0;

err_unwind:
	domain_set_dirty_tracking(dmar_domain, dmar_domain->dirty_tracking);
	spin_unlock(&dmar_domain->lock);
	return ret;
}

static int context_setup_pass_through(struct device *dev, u8 bus, u8 devfn)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	struct context_entry *context;

	spin_lock(&iommu->lock);
	context = iommu_context_addr(iommu, bus, devfn, 1);
	if (!context) {
		spin_unlock(&iommu->lock);
		return -ENOMEM;
	}

	if (context_present(context) && !context_copied(iommu, bus, devfn)) {
		spin_unlock(&iommu->lock);
		return 0;
	}

	copied_context_tear_down(iommu, context, bus, devfn);
	context_clear_entry(context);
	context_set_domain_id(context, FLPT_DEFAULT_DID);

	/*
	 * In pass through mode, AW must be programmed to indicate the largest
	 * AGAW value supported by hardware. And ASR is ignored by hardware.
	 */
	context_set_address_width(context, iommu->msagaw);
	context_set_translation_type(context, CONTEXT_TT_PASS_THROUGH);
	context_set_fault_enable(context);
	context_set_present(context);
	if (!ecap_coherent(iommu->ecap))
		clflush_cache_range(context, sizeof(*context));
	context_present_cache_flush(iommu, FLPT_DEFAULT_DID, bus, devfn);
	spin_unlock(&iommu->lock);

	return 0;
}

static int context_setup_pass_through_cb(struct pci_dev *pdev, u16 alias, void *data)
{
	struct device *dev = data;

	return context_setup_pass_through(dev, PCI_BUS_NUM(alias), alias & 0xff);
}

static int device_setup_pass_through(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);

	if (!dev_is_pci(dev))
		return context_setup_pass_through(dev, info->bus, info->devfn);

	return pci_for_each_dma_alias(to_pci_dev(dev),
				      context_setup_pass_through_cb, dev);
}

static int identity_domain_attach_dev(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	int ret;

	device_block_translation(dev);

	if (dev_is_real_dma_subdevice(dev))
		return 0;

	/*
	 * No PRI support with the global identity domain. No need to enable or
	 * disable PRI in this path as the iommu has been put in the blocking
	 * state.
	 */
	if (sm_supported(iommu))
		ret = intel_pasid_setup_pass_through(iommu, dev, IOMMU_NO_PASID);
	else
		ret = device_setup_pass_through(dev);

	if (!ret)
		info->domain_attached = true;

	return ret;
}

static int identity_domain_set_dev_pasid(struct iommu_domain *domain,
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	int ret;

	if (!pasid_supported(iommu) || dev_is_real_dma_subdevice(dev))
		return -EOPNOTSUPP;

	ret = iopf_for_domain_replace(domain, old, dev);
	if (ret)
		return ret;

	ret = domain_setup_passthrough(iommu, dev, pasid, old);
	if (ret) {
		iopf_for_domain_replace(old, domain, dev);
		return ret;
	}

	domain_remove_dev_pasid(old, dev, pasid);
	return 0;
}

static struct iommu_domain identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev	= identity_domain_attach_dev,
		.set_dev_pasid	= identity_domain_set_dev_pasid,
	},
};

const struct iommu_domain_ops intel_fs_paging_domain_ops = {
	IOMMU_PT_DOMAIN_OPS(x86_64),
	.attach_dev = intel_iommu_attach_device,
	.set_dev_pasid = intel_iommu_set_dev_pasid,
	.iotlb_sync_map = intel_iommu_iotlb_sync_map,
	.flush_iotlb_all = intel_flush_iotlb_all,
	.iotlb_sync = intel_iommu_tlb_sync,
	.free = intel_iommu_domain_free,
	.enforce_cache_coherency = intel_iommu_enforce_cache_coherency_fs,
};

const struct iommu_domain_ops intel_ss_paging_domain_ops = {
	IOMMU_PT_DOMAIN_OPS(vtdss),
	.attach_dev = intel_iommu_attach_device,
	.set_dev_pasid = intel_iommu_set_dev_pasid,
	.iotlb_sync_map = intel_iommu_iotlb_sync_map,
	.flush_iotlb_all = intel_flush_iotlb_all,
	.iotlb_sync = intel_iommu_tlb_sync,
	.free = intel_iommu_domain_free,
	.enforce_cache_coherency = intel_iommu_enforce_cache_coherency_ss,
};

const struct iommu_ops intel_iommu_ops = {
	.blocked_domain		= &blocking_domain,
	.release_domain		= &blocking_domain,
	.identity_domain	= &identity_domain,
	.capable		= intel_iommu_capable,
	.hw_info		= intel_iommu_hw_info,
	.domain_alloc_paging_flags = intel_iommu_domain_alloc_paging_flags,
	.domain_alloc_sva	= intel_svm_domain_alloc,
	.domain_alloc_nested	= intel_iommu_domain_alloc_nested,
	.probe_device		= intel_iommu_probe_device,
	.probe_finalize		= intel_iommu_probe_finalize,
	.release_device		= intel_iommu_release_device,
	.get_resv_regions	= intel_iommu_get_resv_regions,
	.device_group		= intel_iommu_device_group,
	.is_attach_deferred	= intel_iommu_is_attach_deferred,
	.def_domain_type	= device_def_domain_type,
	.page_response		= intel_iommu_page_response,
};

static void quirk_iommu_igfx(struct pci_dev *dev)
{
	if (risky_device(dev))
		return;

	pci_info(dev, "Disabling IOMMU for graphics on this chipset\n");
	disable_igfx_iommu = 1;
}

/* G4x/GM45 integrated gfx dmar support is totally busted. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2a40, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e00, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e10, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e20, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e30, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e40, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e90, quirk_iommu_igfx);

/* QM57/QS57 integrated gfx malfunctions with dmar */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0044, quirk_iommu_igfx);

/* Broadwell igfx malfunctions with dmar */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1606, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160B, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160E, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1602, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160A, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160D, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1616, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161B, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161E, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1612, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161A, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161D, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1626, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162B, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162E, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1622, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162A, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162D, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1636, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163B, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163E, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1632, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163A, quirk_iommu_igfx);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163D, quirk_iommu_igfx);

static void quirk_iommu_rwbf(struct pci_dev *dev)
{
	if (risky_device(dev))
		return;

	/*
	 * Mobile 4 Series Chipset neglects to set RWBF capability,
	 * but needs it. Same seems to hold for the desktop versions.
	 */
	pci_info(dev, "Forcing write-buffer flush capability\n");
	rwbf_quirk = 1;
}

DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2a40, quirk_iommu_rwbf);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e00, quirk_iommu_rwbf);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e10, quirk_iommu_rwbf);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e20, quirk_iommu_rwbf);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e30, quirk_iommu_rwbf);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e40, quirk_iommu_rwbf);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e90, quirk_iommu_rwbf);

#define GGC 0x52
#define GGC_MEMORY_SIZE_MASK	(0xf << 8)
#define GGC_MEMORY_SIZE_NONE	(0x0 << 8)
#define GGC_MEMORY_SIZE_1M	(0x1 << 8)
#define GGC_MEMORY_SIZE_2M	(0x3 << 8)
#define GGC_MEMORY_VT_ENABLED	(0x8 << 8)
#define GGC_MEMORY_SIZE_2M_VT	(0x9 << 8)
#define GGC_MEMORY_SIZE_3M_VT	(0xa << 8)
#define GGC_MEMORY_SIZE_4M_VT	(0xb << 8)

static void quirk_calpella_no_shadow_gtt(struct pci_dev *dev)
{
	unsigned short ggc;

	if (risky_device(dev))
		return;

	if (pci_read_config_word(dev, GGC, &ggc))
		return;

	if (!(ggc & GGC_MEMORY_VT_ENABLED)) {
		pci_info(dev, "BIOS has allocated no shadow GTT; disabling IOMMU for graphics\n");
		disable_igfx_iommu = 1;
	} else if (!disable_igfx_iommu) {
		/* we have to ensure the gfx device is idle before we flush */
		pci_info(dev, "Disabling batched IOTLB flush on Ironlake\n");
		iommu_set_dma_strict();
	}
}
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0040, quirk_calpella_no_shadow_gtt);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0062, quirk_calpella_no_shadow_gtt);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x006a, quirk_calpella_no_shadow_gtt);

static void quirk_igfx_skip_te_disable(struct pci_dev *dev)
{
	unsigned short ver;

	if (!IS_GFX_DEVICE(dev))
		return;

	ver = (dev->device >> 8) & 0xff;
	if (ver != 0x45 && ver != 0x46 && ver != 0x4c &&
	    ver != 0x4e && ver != 0x8a && ver != 0x98 &&
	    ver != 0x9a && ver != 0xa7 && ver != 0x7d)
		return;

	if (risky_device(dev))
		return;

	pci_info(dev, "Skip IOMMU disabling for graphics\n");
	iommu_skip_te_disable = 1;
}
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, PCI_ANY_ID, quirk_igfx_skip_te_disable);

/* On Tylersburg chipsets, some BIOSes have been known to enable the
   ISOCH DMAR unit for the Azalia sound device, but not give it any
   TLB entries, which causes it to deadlock. Check for that.  We do
   this in a function called from init_dmars(), instead of in a PCI
   quirk, because we don't want to print the obnoxious "BIOS broken"
   message if VT-d is actually disabled.
*/
static void __init check_tylersburg_isoch(void)
{
	struct pci_dev *pdev;
	uint32_t vtisochctrl;

	/* If there's no Azalia in the system anyway, forget it. */
	pdev = pci_get_device(PCI_VENDOR_ID_INTEL, 0x3a3e, NULL);
	if (!pdev)
		return;

	if (risky_device(pdev)) {
		pci_dev_put(pdev);
		return;
	}

	pci_dev_put(pdev);

	/* System Management Registers. Might be hidden, in which case
	   we can't do the sanity check. But that's OK, because the
	   known-broken BIOSes _don't_ actually hide it, so far. */
	pdev = pci_get_device(PCI_VENDOR_ID_INTEL, 0x342e, NULL);
	if (!pdev)
		return;

	if (risky_device(pdev)) {
		pci_dev_put(pdev);
		return;
	}

	if (pci_read_config_dword(pdev, 0x188, &vtisochctrl)) {
		pci_dev_put(pdev);
		return;
	}

	pci_dev_put(pdev);

	/* If Azalia DMA is routed to the non-isoch DMAR unit, fine. */
	if (vtisochctrl & 1)
		return;

	/* Drop all bits other than the number of TLB entries */
	vtisochctrl &= 0x1c;

	/* If we have the recommended number of TLB entries (16), fine. */
	if (vtisochctrl == 0x10)
		return;

	/* Zero TLB entries? You get to ride the short bus to school. */
	if (!vtisochctrl) {
		WARN(1, "Your BIOS is broken; DMA routed to ISOCH DMAR unit but no TLB space.\n"
		     "BIOS vendor: %s; Ver: %s; Product Version: %s\n",
		     dmi_get_system_info(DMI_BIOS_VENDOR),
		     dmi_get_system_info(DMI_BIOS_VERSION),
		     dmi_get_system_info(DMI_PRODUCT_VERSION));
		iommu_identity_mapping |= IDENTMAP_AZALIA;
		return;
	}

	pr_warn("Recommended TLB entries for ISOCH unit is 16; your BIOS set %d\n",
	       vtisochctrl);
}

/*
 * Here we deal with a device TLB defect where device may inadvertently issue ATS
 * invalidation completion before posted writes initiated with translated address
 * that utilized translations matching the invalidation address range, violating
 * the invalidation completion ordering.
 * Therefore, any use cases that cannot guarantee DMA is stopped before unmap is
 * vulnerable to this defect. In other words, any dTLB invalidation initiated not
 * under the control of the trusted/privileged host device driver must use this
 * quirk.
 * Device TLBs are invalidated under the following six conditions:
 * 1. Device driver does DMA API unmap IOVA
 * 2. Device driver unbind a PASID from a process, sva_unbind_device()
 * 3. PASID is torn down, after PASID cache is flushed. e.g. process
 *    exit_mmap() due to crash
 * 4. Under SVA usage, called by mmu_notifier.invalidate_range() where
 *    VM has to free pages that were unmapped
 * 5. Userspace driver unmaps a DMA buffer
 * 6. Cache invalidation in vSVA usage (upcoming)
 *
 * For #1 and #2, device drivers are responsible for stopping DMA traffic
 * before unmap/unbind. For #3, iommu driver gets mmu_notifier to
 * invalidate TLB the same way as normal user unmap which will use this quirk.
 * The dTLB invalidation after PASID cache flush does not need this quirk.
 *
 * As a reminder, #6 will *NEED* this quirk as we enable nested translation.
 */
void quirk_extra_dev_tlb_flush(struct device_domain_info *info,
			       unsigned long address, unsigned long mask,
			       u32 pasid, u16 qdep)
{
	u16 sid;

	if (likely(!info->dtlb_extra_inval))
		return;

	sid = PCI_DEVID(info->bus, info->devfn);
	if (pasid == IOMMU_NO_PASID) {
		qi_flush_dev_iotlb(info->iommu, sid, info->pfsid,
				   qdep, address, mask);
	} else {
		qi_flush_dev_iotlb_pasid(info->iommu, sid, info->pfsid,
					 pasid, qdep, address, mask);
	}
}

#define ecmd_get_status_code(res)	(((res) & 0xff) >> 1)

/*
 * Function to submit a command to the enhanced command interface. The
 * valid enhanced command descriptions are defined in Table 47 of the
 * VT-d spec. The VT-d hardware implementation may support some but not
 * all commands, which can be determined by checking the Enhanced
 * Command Capability Register.
 *
 * Return values:
 *  - 0: Command successful without any error;
 *  - Negative: software error value;
 *  - Nonzero positive: failure status code defined in Table 48.
 */
int ecmd_submit_sync(struct intel_iommu *iommu, u8 ecmd, u64 oa, u64 ob)
{
	unsigned long flags;
	u64 res;
	int ret;

	if (!cap_ecmds(iommu->cap))
		return -ENODEV;

	raw_spin_lock_irqsave(&iommu->register_lock, flags);

	res = readq(iommu->reg + DMAR_ECRSP_REG);
	if (res & DMA_ECMD_ECRSP_IP) {
		ret = -EBUSY;
		goto err;
	}

	/*
	 * Unconditionally write the operand B, because
	 * - There is no side effect if an ecmd doesn't require an
	 *   operand B, but we set the register to some value.
	 * - It's not invoked in any critical path. The extra MMIO
	 *   write doesn't bring any performance concerns.
	 */
	writeq(ob, iommu->reg + DMAR_ECEO_REG);
	writeq(ecmd | (oa << DMA_ECMD_OA_SHIFT), iommu->reg + DMAR_ECMD_REG);

	IOMMU_WAIT_OP(iommu, DMAR_ECRSP_REG, readq,
		      !(res & DMA_ECMD_ECRSP_IP), res);

	if (res & DMA_ECMD_ECRSP_IP) {
		ret = -ETIMEDOUT;
		goto err;
	}

	ret = ecmd_get_status_code(res);
err:
	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);

	return ret;
}

MODULE_IMPORT_NS("GENERIC_PT_IOMMU");
