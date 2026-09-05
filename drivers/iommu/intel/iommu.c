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
/*
 * [한국어] (위 영어 주석에 이어)
 * root_entry_lctp - 루트 항목에서 "하위" 컨텍스트 테이블의 물리 주소를 꺼낸다
 *
 * @re: 루트 테이블의 항목 하나. 버스 번호 하나에 대응한다.
 * @return: present 비트가 켜져 있으면 컨텍스트 테이블의 물리 주소, 아니면 0.
 *
 * VT-d 의 3단 구조: 루트 테이블(버스로 인덱스) → 컨텍스트 테이블(devfn 으로
 * 인덱스) → 페이지 테이블. 루트 항목 하나는 128비트(lo/hi)인데, 컨텍스트
 * 테이블 항목이 16바이트라 한 페이지(4KB)에는 256개가 아니라 128개만 들어간다.
 * 그래서 devfn 0~127 을 담당하는 "하위" 테이블과 128~255 를 담당하는 "상위"
 * 테이블로 나누고, 루트 항목의 lo/hi 가 각각을 가리킨다. 이 함수는 그 lo 쪽이다.
 *
 * 비트 0 은 present 플래그이고 나머지 상위 비트가 페이지 정렬된 물리 주소라,
 * VTD_PAGE_MASK 로 플래그 비트를 털어 내면 그대로 주소가 된다.
 *
 * 실행 컨텍스트: 어디서든(락 없음, 순수 계산). 테이블 순회·덤프 경로에서 쓴다.
 *
 * 호출 체인:
 *   free_context_table()/dmar_fault_dump_ptes()/copy_translation_tables()
 *     → [root_entry_lctp]
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
/*
 * [한국어] (위 영어 주석에 이어)
 * root_entry_uctp - 루트 항목에서 "상위" 컨텍스트 테이블의 물리 주소를 꺼낸다
 *
 * @re: 루트 테이블의 항목 하나.
 * @return: present 면 물리 주소, 아니면 0.
 *
 * root_entry_lctp 의 짝이며 devfn 128~255 를 담당한다. 읽는 필드가 lo 대신
 * hi 라는 점만 다르다. 두 함수가 따로 있는 이유는 위(root_entry_lctp 주석)에
 * 설명한 대로 컨텍스트 테이블이 한 페이지에 다 들어가지 않아 둘로 쪼개지기
 * 때문이다. 호출자는 devfn 의 비트 7 을 보고 어느 쪽을 쓸지 고른다.
 *
 * 실행 컨텍스트: 락 없는 순수 계산.
 *
 * 호출 체인:
 *   free_context_table()/dmar_fault_dump_ptes()/copy_translation_tables()
 *     → [root_entry_uctp]
 */
static phys_addr_t root_entry_uctp(struct root_entry *re)
{
	if (!(re->hi & 1))	/* [한국어] 상위 항목의 present 플래그 */
		return 0;	/* [한국어] 없다 */

	return re->hi & VTD_PAGE_MASK;	/* [한국어] 상위 컨텍스트 테이블. 장치 번호 128~255 를 담당하며, 한 페이지에 256개 항목이 들어가지 않아 둘로 나뉜다 */
}

/*
 * [한국어]
 * device_rid_cmp_key - 소스 id(키)와 트리 노드를 비교하는 rbtree 콜백
 *
 * @key: 찾는 값. 실제로는 u16 소스 id 를 가리키는 포인터다.
 * @node: 비교 대상 rbtree 노드. device_domain_info 안에 박혀 있다.
 * @return: 키가 더 작으면 -1, 크면 1, 같으면 0.
 *
 * 왜 소스 id 로 색인하는가: VT-d 하드웨어가 폴트를 보고할 때 알려 주는 것은
 * struct device 가 아니라 16비트 소스 id(버스 8비트 + devfn 8비트)뿐이다.
 * 그래서 각 유닛은 자기 아래 장치들을 소스 id 로 색인한 rbtree
 * (iommu->device_rbtree)로 들고 있어야 하고, 폴트 처리기가 그 트리에서
 * 장치를 되찾는다. PCI_DEVID(bus, devfn) 이 그 16비트 값을 조립한다.
 *
 * 인터페이스가 두 개인 이유: rb_find 는 "키 vs 노드" 비교자를,
 * rb_find_add 는 "노드 vs 노드" 비교자를 요구한다. 이 함수는 앞쪽이고,
 * device_rid_cmp 가 뒤쪽으로 이 함수를 감싼다.
 *
 * 실행 컨텍스트: device_rbtree_lock 을 쥔 채 호출된다. 순수 비교라 잠들지 않는다.
 *
 * 호출 체인:
 *   device_rbtree_find() → rb_find() → [device_rid_cmp_key]
 *   device_rbtree_insert() → rb_find_add() → device_rid_cmp() → [이 함수]
 */
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

/*
 * [한국어]
 * device_rid_cmp - 노드 대 노드 비교자. 삽입 시 rbtree 가 쓴다
 *
 * @lhs: 새로 넣으려는 노드.
 * @rhs: 트리에 이미 있는 비교 대상 노드.
 * @return: -1 / 0 / 1 (device_rid_cmp_key 와 같은 규약).
 *
 * 새 노드에서 소스 id 를 뽑아 키로 만든 뒤 device_rid_cmp_key 에 넘긴다.
 * 비교 규칙을 한 곳에만 두기 위한 얇은 어댑터이며, 두 비교자가 서로 다른
 * 순서를 쓰게 되는 사고를 막는다.
 *
 * 실행 컨텍스트: device_rbtree_lock 을 쥔 삽입 경로. 순수 계산.
 *
 * 호출 체인:
 *   device_rbtree_insert() → rb_find_add() → [device_rid_cmp]
 *     → device_rid_cmp_key()
 */
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
/*
 * [한국어] (위 영어 주석에 이어)
 * device_rbtree_find - 소스 id 로 이 유닛 아래의 장치를 되찾는다
 *
 * @iommu: 폴트를 보고한 DMAR 유닛.
 * @rid: 하드웨어가 알려 준 16비트 소스 id(버스 8비트 + devfn 8비트).
 * @return: 등록된 장치의 struct device *, 없으면 NULL.
 *
 * 왜 필요한가: VT-d 폴트 레코드와 페이지 요청(PRQ)에는 소스 id 만 들어 있다.
 * 폴트를 드라이버나 io-pgfault 계층으로 넘기려면 그 숫자를 struct device 로
 * 되돌려야 하는데, 그 역방향 조회를 이 트리가 담당한다. 장치는 프로브 때
 * device_rbtree_insert 로 등록되고 release 때 device_rbtree_remove 로 빠진다.
 *
 * 동기화: spin_lock_irqsave 를 쓴다. 이 조회가 폴트 인터럽트 핸들러에서도
 * 불리기 때문에, 같은 락을 인터럽트 밖(프로브/해제)에서 잡을 때 인터럽트를
 * 막지 않으면 자기 자신과 데드락이 난다.
 *
 * 수명 주의(위 영어 주석): 이 함수는 반환한 장치가 곧바로 해제되지 않는다는
 * 보장을 하지 않는다. 락은 트리 구조만 지키지 장치의 참조 계수를 올리지는
 * 않기 때문이다. 그 장치를 오래 붙들 호출자는 스스로 참조를 잡아야 한다.
 *
 * 호출 체인:
 *   intel_iommu_fault_handler()/prq_event_thread() → [device_rbtree_find]
 *     → rb_find() → device_rid_cmp_key()
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

/*
 * [한국어]
 * device_rbtree_insert - 장치를 소스 id 색인 트리에 등록한다
 *
 * @iommu: 이 장치를 맡을 DMAR 유닛.
 * @info: 등록할 장치 정보. info->bus/devfn 이 키가 된다.
 * @return: 0 성공, -EEXIST 면 같은 소스 id 가 이미 등록되어 있다.
 *
 * 장치가 이 유닛에 붙는 순간(intel_iommu_probe_device) 호출된다. 이 등록이
 * 끝나야 하드웨어 폴트/페이지 요청이 struct device 로 되짚어진다
 * (device_rbtree_find 주석 참고).
 *
 * rb_find_add 를 쓰는 이유: "이미 있는지 찾고, 없으면 넣는다"를 트리 순회 한
 * 번으로 끝낸다. rb_find + rb_insert 로 나누면 순회를 두 번 하는 데다 그 사이
 * 락을 놓칠 여지가 생긴다. 반환값이 NULL 이 아니면 충돌한 기존 노드다.
 *
 * -EEXIST 가 WARN 인 이유: 두 장치가 같은 requester id 를 갖는 것은 하드웨어
 * 구성이 잘못되었거나 커널이 같은 장치를 두 번 프로브했다는 뜻이다. 정상
 * 동작에서는 일어날 수 없으므로 조용히 넘기지 않고 스택을 남긴다.
 *
 * 동기화: device_rbtree_lock 을 인터럽트를 끈 채 잡는다. 같은 락을 폴트
 * 인터럽트 문맥의 device_rbtree_find 가 잡기 때문이다.
 * 실행 컨텍스트: 장치 프로브. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_probe_device() → [device_rbtree_insert]
 *     → rb_find_add() → device_rid_cmp()
 */
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

/*
 * [한국어]
 * device_rbtree_remove - 소스 id 색인 트리에서 장치를 뺀다
 *
 * @info: 제거할 장치의 device_domain_info. info->iommu 가 소속 유닛이다.
 * @return: 없음.
 *
 * 장치가 IOMMU 에서 떨어질 때(intel_iommu_release_device) 호출된다. 이 뒤로는
 * 그 소스 id 로 오는 폴트가 장치를 찾지 못하고 "알 수 없는 소스" 로 처리된다.
 * 순서상 중요한 점: 트리에서 빼는 것은 장치를 도메인에서 떼어 낸 뒤여야 한다.
 * 아직 매핑이 살아 있는 동안 트리에서 빼면, 그 사이 발생한 폴트를 어느 장치의
 * 것인지 알 수 없게 된다.
 *
 * 동기화: device_rbtree_find 와 같은 이유로 spin_lock_irqsave 를 쓴다.
 * rb_erase 자체는 트리 구조만 바꾸며, info 의 메모리 해제는 호출자 몫이다.
 *
 * 호출 체인:
 *   intel_iommu_release_device() → [device_rbtree_remove] → rb_erase()
 */
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
	/* [한국어] 이 SATC 항목의 장치들이 ATS 를 "반드시" 켜야 하는지 여부.
	 * 설정자: dmar_parse_one_satc() 가 ACPI 항목의 flags 비트 0 을 그대로 옮긴다.
	 * 읽는 자: dmar_ats_supported() — 이 값이 1 이면 PCIe ATS 능력 구조가 없어도
	 *   SoC 내부 경로로 ATS 가 동작하는 것으로 보고 활성화를 허용한다.
	 * 값 범위: 0 이면 "쓸 수 있다"(선택), 1 이면 "없으면 동작하지 않는다"(필수).
	 *   일반 PCIe 장치의 ATS 는 성능 최적화지만, SoC 통합 장치 중에는 번역
	 *   캐시를 전제로 설계되어 그것 없이는 DMA 자체가 실패하는 것들이 있다.
	 * 동기화: 파싱 시점에 한 번 쓰이고 이후 읽기만 한다. RCU 목록에 실려 있어
	 *   순회 중 갱신되지 않는다. */
};

static LIST_HEAD(dmar_atsr_units);	/* [한국어] ATS 지원을 보고한 루트 포트 목록. 파싱 순서와 무관하게 조회되므로 전역이다 */
static LIST_HEAD(dmar_rmrr_units);	/* [한국어] 펌웨어가 예약한 메모리 구간 목록. 장치 프로브 때 항등 매핑 여부를 정하는 근거가 된다 */
static LIST_HEAD(dmar_satc_units);	/* [한국어] ATS 가 필수인 SoC 통합 장치 목록 */

#define for_each_rmrr_units(rmrr) \	/* [한국어] RMRR 목록 순회 관용구. 여러 곳에서 같은 순회를 하므로 매크로로 뺐다 */
	list_for_each_entry(rmrr, &dmar_rmrr_units, list)	/* [한국어] 위 매크로의 본체 */

static void intel_iommu_domain_free(struct iommu_domain *domain);

int dmar_disabled = !IS_ENABLED(CONFIG_INTEL_IOMMU_DEFAULT_ON);	/* [한국어] VT-d 를 끌지 여부. 기본값이 빌드 설정에서 오고, intel_iommu=on/off 가 덮어쓴다 */
int intel_iommu_sm = IS_ENABLED(CONFIG_INTEL_IOMMU_SCALABLE_MODE_DEFAULT_ON);	/* [한국어] scalable mode(PASID 기반 새 형식) 사용 여부. SVA 와 nested 변환의 전제 조건이다 */

int intel_iommu_enabled = 0;	/* [한국어] VT-d 가 실제로 켜졌는가. 다른 서브시스템(그래픽 드라이버 등)이 참고한다 */
EXPORT_SYMBOL_GPL(intel_iommu_enabled);	/* [한국어] 모듈에서도 볼 수 있게 */

static int intel_iommu_superpage = 1;	/* [한국어] 큰 페이지(2MB/1GB) 매핑을 쓸지. 끄면 PTE 가 늘지만 일부 하드웨어 결함을 피할 수 있다 */
static int iommu_identity_mapping;	/* [한국어] 항등 매핑이 필요한 장치 종류의 비트마스크 */
static int iommu_skip_te_disable;	/* [한국어] 종료 시 번역을 끄지 않는다. kexec 로 넘어갈 때 진행 중인 DMA 를 끊지 않기 위한 것 */
static int disable_igfx_iommu;	/* [한국어] 통합 그래픽을 IOMMU 밖에 둔다. 일부 세대의 GPU 펌웨어가 IOMMU 아래에서 오작동해 생긴 우회다 */

#define IDENTMAP_AZALIA		4	/* [한국어] 특정 HD 오디오 컨트롤러에 항등 매핑을 강제하는 비트 */

const struct iommu_ops intel_iommu_ops;	/* [한국어] 코어에 등록할 콜백 표. 정의는 파일 끝에 있다 */

/*
 * [한국어]
 * translation_pre_enabled - 커널이 시작하기 전에 이미 번역이 켜져 있었는지 본다
 *
 * @iommu: 대상 DMAR 유닛.
 * @return: true 면 우리가 오기 전부터 VT-d 번역이 동작 중이었다.
 *
 * 이 상태가 왜 특별한가: kdump 로 부팅한 커널이나, 펌웨어/이전 커널이 IOMMU 를
 * 켜 둔 채 넘겨준 경우가 여기에 해당한다. 그 순간에도 장치들은 이전 테이블을
 * 통해 DMA 를 계속하고 있으므로, 번역을 덜컥 끄면 진행 중인 전송이 원래 의도와
 * 다른 물리 주소로 향한다(끄면 번역 없이 통과하므로 IOVA 가 그대로 물리 주소로
 * 해석된다). 그래서 이 플래그가 켜져 있으면 copy_translation_tables 로 기존
 * 테이블을 이어받는 경로를 탄다.
 *
 * 플래그는 init_translation_status 가 하드웨어 상태 레지스터를 읽어 세우고,
 * 인계가 끝나면 clear_translation_pre_enabled 가 지운다.
 *
 * 실행 컨텍스트: 초기화 경로. 순수 비트 검사.
 *
 * 호출 체인:
 *   init_dmars()/intel_iommu_add() → [translation_pre_enabled]
 */
static bool translation_pre_enabled(struct intel_iommu *iommu)
{
	return (iommu->flags & VTD_FLAG_TRANS_PRE_ENABLED);	/* [한국어] 커널이 시작하기 전에 이미 번역이 켜져 있었는가. kexec 나 펌웨어가 켜 둔 경우이며, 그 상태를 함부로 끄면 진행 중인 DMA 가 끊긴다 */
}

/*
 * [한국어]
 * clear_translation_pre_enabled - "인계받을 상태" 표시를 지운다
 *
 * @iommu: 대상 DMAR 유닛.
 * @return: 없음.
 *
 * 이전 커널/펌웨어의 테이블을 이어받는 데 실패했거나, 인계를 마치고 우리
 * 테이블로 전환한 시점에 호출한다. 이 플래그가 남아 있으면 이후 코드가 아직
 * 남의 테이블이 살아 있다고 오해하고 하드웨어를 건드리지 않으려 하므로,
 * 전환 사실을 반드시 여기서 기록해야 한다.
 *
 * 실행 컨텍스트: 초기화 경로, 단일 스레드. 별도 락 없음.
 *
 * 호출 체인:
 *   init_dmars() (인계 실패/완료 지점) → [clear_translation_pre_enabled]
 */
static void clear_translation_pre_enabled(struct intel_iommu *iommu)
{
	iommu->flags &= ~VTD_FLAG_TRANS_PRE_ENABLED;	/* [한국어] 우리가 상태를 넘겨받았음을 표시한다 */
}

/*
 * [한국어]
 * init_translation_status - 하드웨어를 읽어 "이미 번역이 켜져 있는지"를 기록한다
 *
 * @iommu: 방금 레지스터를 매핑한 DMAR 유닛.
 * @return: 없음. 결과는 iommu->flags 에 남는다.
 *
 * DMAR_GSTS_REG(Global Status Register)의 TES(Translation Enable Status) 비트를
 * 본다. 이 비트가 켜져 있다는 것은 우리가 아무것도 하기 전에 이미 하드웨어가
 * 어떤 루트 테이블을 워크하고 있다는 뜻이다.
 *
 * 왜 초기화 아주 이른 시점에 읽어야 하는가: 우리가 레지스터를 하나라도 쓰기
 * 시작하면 원래 상태를 알 길이 없어진다. 그래서 유닛의 레지스터를 매핑한 직후,
 * 아무것도 바꾸기 전에 한 번 읽어 VTD_FLAG_TRANS_PRE_ENABLED 로 남겨 둔다.
 * 이후 init_dmars 가 이 값을 보고 기존 테이블 인계(kdump 경로)를 시도한다.
 *
 * 실행 컨텍스트: 유닛 초기화 초반. MMIO readl 하나뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   init_dmars()/intel_iommu_add() → [init_translation_status] → readl()
 */
static void init_translation_status(struct intel_iommu *iommu)
{
	u32 gsts;	/* [한국어] 전역 상태 레지스터 값 */

	gsts = readl(iommu->reg + DMAR_GSTS_REG);	/* [한국어] 하드웨어 상태를 읽는다 */
	if (gsts & DMA_GSTS_TES)	/* [한국어] Translation Enable Status — 번역이 이미 켜져 있다 */
		iommu->flags |= VTD_FLAG_TRANS_PRE_ENABLED;	/* [한국어] 기억해 둔다. 이후 초기화가 기존 테이블을 이어받을지 새로 만들지를 이 값으로 정한다 */
}

/*
 * [한국어]
 * intel_iommu_setup - "intel_iommu=" 커널 부트 인자를 해석한다
 *
 * @str: 등호 뒤의 문자열. 쉼표로 여러 옵션을 이어 쓸 수 있다.
 * @return: 0 성공, -EINVAL 이면 값이 비어 있다.
 *
 * 왜 이렇게 옵션이 많은가: VT-d 는 잘못 켜면 부팅 자체가 막히는 기능이라,
 * 특정 장치나 특정 기능만 예외로 두는 탈출구가 역사적으로 계속 늘었다.
 * 여기서 세우는 전역 변수들이 이후 초기화 전반의 정책을 정한다.
 *
 * 주요 옵션:
 *   on/off        — dmar_disabled. off 는 no_platform_optin 까지 세워 펌웨어의
 *                   권장까지 무시한다.
 *   igfx_off      — 통합 GPU 만 IOMMU 밖에 둔다. 오래된 iGPU 펌웨어가 IOMMU
 *                   아래에서 오동작하는 사례가 많아 남아 있는 옵션이다.
 *   sm_on/sm_off  — scalable mode(PASID/SVA 를 쓰는 새 형식) 강제 on/off.
 *   strict/       — 무효화 정책. 느슨한(lazy) 모드는 언매핑 후 TLB 를 즉시
 *   no_strict       비우지 않아 빠르지만, 그 사이 장치가 해제된 페이지에
 *                   접근할 수 있는 창이 생긴다.
 *   tboot_noforce — TXT 부팅에서도 실패를 치명적으로 다루지 않는다.
 *
 * 파싱 방식: while 루프가 문자열을 훑으며 strncmp 로 접두사를 맞춰 보고,
 * 쉼표를 만나면 다음 옵션으로 넘어간다. 커널 부트 인자 파서의 전형적인 형태다.
 *
 * 실행 컨텍스트: __init. 부팅 극초기, 다른 CPU 가 올라오기 전.
 *
 * 호출 체인:
 *   커널 부트 인자 파서(__setup) → [intel_iommu_setup]
 */
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

	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치는 별도 처리가 필요하다 — 아래에서 실제 DMA 를 내는 장치로 바꿔 잡는다 */
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

/*
 * [한국어]
 * free_context_table - 유닛의 루트 테이블과 그 아래 컨텍스트 테이블을 모두 반납한다
 *
 * @iommu: 대상 DMAR 유닛.
 * @return: 없음.
 *
 * 구조를 따라 아래에서 위로 해제한다: 루트 항목 256개(버스 번호 하나당 하나)를
 * 훑으며 각 항목의 하위(lctp)·상위(uctp) 컨텍스트 테이블 페이지를 반납하고,
 * 마지막에 루트 테이블 자신을 반납한다. 한 루트 항목이 테이블 두 개를 가리키는
 * 이유는 root_entry_lctp 주석에 적힌 대로 컨텍스트 항목 128개가 한 페이지를
 * 채우기 때문이다.
 *
 * 호출 전 조건: 반드시 번역이 꺼진 뒤여야 한다. 하드웨어가 아직 이 테이블을
 * 워크하고 있는 동안 페이지를 반납하면, 해제되어 재사용된 메모리를 IOMMU 가
 * 페이지 테이블로 해석하게 된다.
 *
 * 동기화: iommu->lock 을 잡는다. 컨텍스트 매핑을 세우는 경로와 같은 락이라,
 * 해제 중에 새 매핑이 들어오는 일이 없다.
 *
 * 실행 컨텍스트: 유닛 해제(free_dmar_iommu) 또는 초기화 실패 정리.
 * 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   free_dmar_iommu() → [free_context_table]
 *     → root_entry_lctp()/root_entry_uctp() → iommu_free_pages()
 */
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
/*
 * [한국어]
 * pgtable_walk - 폴트가 난 주소의 페이지 테이블을 레벨별로 덤프한다
 *
 * @iommu: 폴트를 보고한 DMAR 유닛.
 * @pfn: 폴트 주소의 페이지 프레임 번호(IOVA >> 12).
 * @bus, @devfn: 폴트를 낸 장치의 소스 id 를 쪼갠 값. 로그에 함께 찍는다.
 * @parent: 워크를 시작할 페이지 테이블(보통 도메인의 최상위 테이블).
 * @level: @parent 가 몇 단계인지. 여기서부터 1 이 될 때까지 내려간다.
 * @return: 없음. 결과는 dmesg 로만 나간다.
 *
 * 왜 필요한가: DMA 폴트가 났을 때 "어느 주소가 실패했다"만으로는 원인을 알기
 * 어렵다. 매핑이 아예 없는 것인지, 있는데 권한이 모자란 것인지, 큰 페이지
 * 중간을 가리킨 것인지에 따라 의심할 곳이 완전히 다르다. 그래서 각 레벨의
 * 항목 값을 그대로 찍어 준다.
 *
 * 동작: 레벨마다 pfn 의 해당 구간 비트로 오프셋을 뽑아 항목을 읽고, present 가
 * 아니면 거기서 멈춘다(그 지점이 매핑이 끊긴 곳이다). 큰 페이지 항목을
 * 만나도 멈춘다 — 그 아래 레벨이 없기 때문이다.
 *
 * CONFIG_DMAR_DEBUG 안에서만 빌드된다. 폴트 경로에서 dmesg 로 테이블 내용을
 * 쏟아내는 것은 진단용이지 상용 동작이 아니기 때문이다.
 *
 * 실행 컨텍스트: 폴트 인터럽트 처리 경로. 잠들면 안 되고, 락도 잡지 않는다
 * (이미 잘못된 상태를 관찰하는 중이라 정합성보다 정보가 우선이다).
 *
 * 호출 체인:
 *   dmar_fault() → dmar_fault_dump_ptes() → [pgtable_walk]
 */
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

/*
 * [한국어]
 * dmar_fault_dump_ptes - 폴트 하나에 대해 루트/컨텍스트/PASID/페이지 테이블을 훑어 찍는다
 *
 * @iommu: 폴트를 보고한 DMAR 유닛.
 * @source_id: 하드웨어가 알려 준 16비트 소스 id.
 * @addr: 폴트가 난 주소(IOVA).
 * @pasid: scalable mode 라면 폴트를 낸 PASID, 아니면 무효값.
 * @return: 없음. 진단 출력만 한다.
 *
 * VT-d 의 변환 경로 전체를 위에서부터 따라 내려가며 각 단계의 항목을 덤프한다.
 *   1) 루트 테이블에서 버스 번호로 항목을 찾는다. present 가 아니면 이 버스에
 *      아무 설정이 없다는 뜻이라 거기서 멈춘다.
 *   2) 컨텍스트 테이블에서 devfn 으로 항목을 찾는다. 여기서 present 가 아니면
 *      "장치가 도메인에 붙지 않았는데 DMA 를 냈다"는 흔한 실패다.
 *   3) 레거시 모드면 컨텍스트 항목이 곧바로 페이지 테이블 루트를 가리킨다.
 *      scalable 모드면 PASID 디렉터리 → PASID 테이블 → 항목을 한 단계 더
 *      거쳐야 페이지 테이블에 닿는다. 이 분기가 두 모드의 실질적 차이다.
 *   4) 마지막으로 pgtable_walk 가 페이지 테이블을 레벨별로 찍는다.
 *
 * 각 단계에서 멈춘 지점 자체가 진단 정보다 — 어느 표에서 끊겼는지가 곧
 * 무엇을 설정하지 않았는지를 말해 준다.
 *
 * CONFIG_DMAR_DEBUG 전용. 실행 컨텍스트는 폴트 인터럽트 처리 경로다.
 *
 * 호출 체인:
 *   dmar_fault() (인터럽트 핸들러) → [dmar_fault_dump_ptes]
 *     → root_entry_lctp()/root_entry_uctp() → pgtable_walk()
 */
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
/*
 * [한국어] (위 "iommu handling" 영어 주석에 이어)
 * iommu_alloc_root_entry - 유닛의 루트 테이블 한 페이지를 잡는다
 *
 * @iommu: 대상 DMAR 유닛.
 * @return: 0 성공, -ENOMEM 실패(그 유닛은 쓸 수 없다).
 *
 * 루트 테이블은 VT-d 주소 변환의 최상단이다. PCI 버스 번호(0~255)로 색인되는
 * 항목 256개 × 16바이트 = 정확히 4KB, 즉 한 페이지다. 그래서 크기 계산 없이
 * SZ_4K 한 장을 잡는다.
 *
 * iommu->node 에서 잡는 이유: 이 페이지는 하드웨어가 매 번역마다 읽는다.
 * 해당 DMAR 유닛과 같은 NUMA 노드의 메모리를 쓰면 그 접근의 지연이 줄어든다.
 * GFP_ATOMIC 인 것은 이 경로가 인터럽트를 끈 초기화 구간에서도 불릴 수 있기
 * 때문이다.
 *
 * __iommu_flush_cache: 페이지는 0 으로 초기화되어 있지만, 코히런시가 없는
 * 유닛(!ecap_coherent)에서는 CPU 캐시에만 있고 메모리에는 아직 반영되지 않았을
 * 수 있다. 하드웨어는 메모리를 직접 읽으므로 여기서 밀어내야 한다. 그러지
 * 않으면 유닛이 쓰레기 값을 present 항목으로 오해할 수 있다.
 *
 * 이 함수는 테이블을 만들기만 한다. 하드웨어에 주소를 알리는 것은
 * iommu_set_root_entry 의 몫이며, 그 사이에 kdump 인계(copy_translation_tables)
 * 가 끼어들 수 있다.
 *
 * 실행 컨텍스트: 초기화/핫플러그. 인터럽트를 끈 구간일 수 있어 GFP_ATOMIC.
 *
 * 호출 체인:
 *   init_dmars()/intel_iommu_add() → [iommu_alloc_root_entry]
 *     → iommu_alloc_pages_node_sz() → __iommu_flush_cache()
 */
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
	default:	/* [한국어] 펌웨어/호출자가 알 수 없는 무효화 종류를 넘겼다 */
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
	default:	/* [한국어] 마찬가지로 알 수 없는 IOTLB 무효화 종류 */
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

/*
 * [한국어]
 * copy_translation_tables - 앞선 커널의 번역 구조를 통째로 인수인계한다
 *
 * @iommu:  대상 유닛
 * @return: 0 성공, 음수면 인수인계 불가
 *
 * kdump 의 핵심 함수다. 하드웨어 레지스터에서 앞선 커널의 루트 테이블 주소를
 * 직접 읽어, 그 아래의 컨텍스트 테이블을 모두 우리 메모리로 복사한다. 매핑이
 * 유지되므로 진행 중이던 DMA 가 끊기지 않고, 그래서 덤프를 쓸 디스크 컨트롤러가
 * 계속 동작한다.
 *
 * 모드가 다르면 곧바로 포기하는 것이 이 함수의 첫 판단이다. RTT(scalable) 비트는
 * 번역을 끈 상태에서만 바꿀 수 있는데, 끄는 순간 진행 중인 DMA 가 물리 주소로
 * 통과해 메모리를 덮어쓴다 (위 영어 주석). 인수인계를 포기하는 편이 안전하다.
 *
 * 옛 테이블을 그대로 쓰지 않고 복사하는 이유는 소유권이다. 그 메모리는 크래시
 * 커널이 관리하는 영역이 아니라 언제 재사용될지 알 수 없다.
 *
 * 실행 컨텍스트: 유닛 초기화 (kdump). 프로세스 문맥.
 *
 * 호출 체인: init_dmars → [이 함수] → copy_context_table
 */
static int copy_translation_tables(struct intel_iommu *iommu)
{
	struct context_entry **ctxt_tbls;	/* [한국어] 새로 만든 컨텍스트 테이블들을 모을 배열 */
	struct root_entry *old_rt;	/* [한국어] 앞선 커널의 루트 테이블 (임시 매핑) */
	phys_addr_t old_rt_phys;	/* [한국어] 그 물리 주소 */
	int ctxt_table_entries;	/* [한국어] 배열 크기 */
	u64 rtaddr_reg;	/* [한국어] 현재 하드웨어의 루트 주소 레지스터 값 */
	int bus, ret;	/* [한국어] 버스 순회와 결과 */
	bool new_ext, ext;	/* [한국어] 앞선 커널과 우리가 각각 확장 모드였는가 */

	rtaddr_reg = readq(iommu->reg + DMAR_RTADDR_REG);	/* [한국어] 하드웨어에게 직접 묻는다 — 앞선 커널이 무엇을 설정했는지는 이 레지스터만이 안다 */
	ext        = !!(rtaddr_reg & DMA_RTADDR_SMT);	/* [한국어] 앞선 커널이 scalable mode 였는가 */
	new_ext    = !!sm_supported(iommu);	/* [한국어] 우리가 쓸 모드 */

	/*
	 * The RTT bit can only be changed when translation is disabled,
	 * but disabling translation means to open a window for data
	 * corruption. So bail out and don't copy anything if we would
	 * have to change the bit.
	 */
	if (new_ext != ext)	/* [한국어] 모드가 다르다 */
		return -EINVAL;	/* [한국어] RTT 비트는 번역을 끈 상태에서만 바꿀 수 있고, 끄는 순간 진행 중인 DMA 가 물리 주소로 통과해 데이터가 손상된다. 그래서 복사를 포기한다 (위 영어 주석) */

	iommu->copied_tables = bitmap_zalloc(BIT_ULL(16), GFP_KERNEL);	/* [한국어] 65536개 소스 id 각각에 대해 '물려받은 항목인가'를 기록할 비트맵 */
	if (!iommu->copied_tables)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 복사 불가 */

	old_rt_phys = rtaddr_reg & VTD_PAGE_MASK;	/* [한국어] 앞선 커널의 루트 테이블 주소 */
	if (!old_rt_phys)	/* [한국어] 주소가 없다 */
		return -EINVAL;	/* [한국어] 복사할 것이 없다 */

	old_rt = memremap(old_rt_phys, PAGE_SIZE, MEMREMAP_WB);	/* [한국어] 임시로 매핑한다. 그 메모리는 크래시 커널의 선형 매핑 안에 없을 수 있다 */
	if (!old_rt)	/* [한국어] 매핑 실패 */
		return -ENOMEM;	/* [한국어] 복사 불가 */

	/* This is too big for the stack - allocate it from slab */
	ctxt_table_entries = ext ? 512 : 256;	/* [한국어] 확장 모드는 버스마다 테이블이 둘 */
	ret = -ENOMEM;	/* [한국어] 아래 할당 실패 시의 값 */
	ctxt_tbls = kcalloc(ctxt_table_entries, sizeof(void *), GFP_KERNEL);	/* [한국어] 스택에 두기엔 너무 크다 (위 영어 주석) */
	if (!ctxt_tbls)	/* [한국어] 할당 실패 */
		goto out_unmap;	/* [한국어] 임시 매핑을 풀고 나간다 */

	for (bus = 0; bus < 256; bus++) {	/* [한국어] 모든 버스에 대해 */
		ret = copy_context_table(iommu, &old_rt[bus],	/* [한국어] 그 버스의 컨텍스트 테이블을 옮긴다 */
					 ctxt_tbls, bus, ext);	/* [한국어] 결과를 배열에 담는다 */
		if (ret) {	/* [한국어] 한 버스가 실패해도 */
			pr_err("%s: Failed to copy context table for bus %d\n",	/* [한국어] 기록만 남기고 */
				iommu->name, bus);	/* [한국어] 어느 유닛의 어느 버스인지 */
			continue;	/* [한국어] 나머지 버스는 계속 옮긴다 — 일부라도 살리는 편이 낫다 */
		}
	}

	spin_lock(&iommu->lock);	/* [한국어] 루트 테이블 변경 구간 */

	/* Context tables are copied, now write them to the root_entry table */
	for (bus = 0; bus < 256; bus++) {	/* [한국어] 옮긴 테이블들을 루트에 연결한다 */
		int idx = ext ? bus * 2 : bus;	/* [한국어] 확장 모드의 인덱스 보정 */
		u64 val;	/* [한국어] 루트 항목에 쓸 값 */

		if (ctxt_tbls[idx]) {	/* [한국어] 하위 테이블이 있으면 */
			val = virt_to_phys(ctxt_tbls[idx]) | 1;	/* [한국어] 물리 주소 + present 비트 */
			iommu->root_entry[bus].lo = val;	/* [한국어] 우리 루트 테이블에 연결 */
		}

		if (!ext || !ctxt_tbls[idx + 1])	/* [한국어] 확장 모드가 아니거나 상위 테이블이 없으면 */
			continue;	/* [한국어] 다음 버스로 */

		val = virt_to_phys(ctxt_tbls[idx + 1]) | 1;	/* [한국어] 상위 테이블의 주소 */
		iommu->root_entry[bus].hi = val;	/* [한국어] 루트 항목의 상위 절반에 */
	}

	spin_unlock(&iommu->lock);	/* [한국어] 루트 테이블 변경 끝 */

	kfree(ctxt_tbls);	/* [한국어] 임시 배열 해제 (테이블 자체는 루트가 참조한다) */

	__iommu_flush_cache(iommu, iommu->root_entry, PAGE_SIZE);	/* [한국어] 루트 테이블을 메모리로 밀어낸다 */

	ret = 0;	/* [한국어] 복사 완료 */

out_unmap:	/* [한국어] 임시 매핑 해제 경로 */
	memunmap(old_rt);	/* [한국어] 앞선 커널의 루트 테이블 매핑 해제 */

	return ret;	/* [한국어] 0 이면 인수인계 성공 */
}

/*
 * [한국어]
 * init_dmars - 모든 DMAR 유닛을 세우고 번역을 켠다
 *
 * @return: 0 성공, 음수 실패
 *
 * 이 드라이버의 부팅 초기화 본체다. 세 번의 순회로 나뉘어 있고, 그 분할에 이유가
 * 있다.
 *
 *  1) 유닛마다 무효화 큐를 세우고 루트 테이블을 만든다. kdump 면 앞선 커널의
 *     설정을 인수인계한다. PASID 상한도 여기서 모든 유닛의 최솟값으로 정해지는데,
 *     PASID 가 전역 자원이라 어느 유닛에서도 표현 가능해야 하기 때문이다.
 *  2) 모든 유닛의 큐가 준비된 뒤에야 루트 테이블을 설치한다. 일부 X58 칩셋에서
 *     이 순서가 아니면 flush_context 가 영영 끝나지 않아 부팅이 멈춘다
 *     (위 영어 주석).
 *  3) 폴트 인터럽트와 PRI 큐를 걸고 번역을 켠다.
 *
 * 실패하면 이미 설정한 유닛들을 모두 되돌린다. 반쯤 켜진 상태로 두면 일부 장치만
 * 격리되어, 어느 장치가 무엇을 볼 수 있는지 알 수 없게 된다.
 *
 * 실행 컨텍스트: 부팅 초기화. dmar_global_lock 을 든 채 (일부 구간에서 잠깐 놓는다).
 *
 * 호출 체인: intel_iommu_init → [이 함수]
 */
static int __init init_dmars(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회 커서 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */
	int ret;	/* [한국어] 각 단계의 결과 */

	for_each_iommu(iommu, drhd) {	/* [한국어] 1단계 — 유닛마다 큐와 루트 테이블을 준비한다 */
		if (drhd->ignored) {	/* [한국어] 무시하도록 표시된 유닛 */
			iommu_disable_translation(iommu);	/* [한국어] 번역을 꺼 둔다 */
			continue;	/* [한국어] 설정하지 않는다 */
		}

		/*
		 * Find the max pasid size of all IOMMU's in the system.
		 * We need to ensure the system pasid table is no bigger
		 * than the smallest supported.
		 */
		if (pasid_supported(iommu)) {	/* [한국어] PASID 를 지원하는 유닛이면 */
			u32 temp = 2 << ecap_pss(iommu->ecap);	/* [한국어] 이 유닛이 지원하는 PASID 개수 */

			intel_pasid_max_id = min_t(u32, temp,	/* [한국어] 시스템 전체의 상한을 가장 작은 유닛에 맞춘다. PASID 는 전역 자원이라 어느 유닛에서도 표현 가능해야 한다 (위 영어 주석) */
						   intel_pasid_max_id);	/* [한국어] 현재까지의 최솟값 */
		}

		intel_iommu_init_qi(iommu);	/* [한국어] 무효화 큐를 세우고 방식을 정한다 */
		init_translation_status(iommu);	/* [한국어] 번역이 이미 켜져 있었는지 확인한다 */

		if (translation_pre_enabled(iommu) && !is_kdump_kernel()) {	/* [한국어] 켜져 있는데 크래시 커널이 아니다 */
			iommu_disable_translation(iommu);	/* [한국어] 펌웨어가 켜 둔 것이므로 우리가 다시 세운다 */
			clear_translation_pre_enabled(iommu);	/* [한국어] 인수인계 표시 해제 */
			pr_warn("Translation was enabled for %s but we are not in kdump mode\n",	/* [한국어] 펌웨어가 IOMMU 를 켜 둔 채 넘긴 것은 흔치 않은 상황이라 알린다 */
				iommu->name);	/* [한국어] 어느 유닛인지 */
		}

		/*
		 * TBD:
		 * we could share the same root & context tables
		 * among all IOMMU's. Need to Split it later.
		 */
		ret = iommu_alloc_root_entry(iommu);	/* [한국어] 우리 루트 테이블을 만든다 */
		if (ret)	/* [한국어] 할당 실패 */
			goto free_iommu;	/* [한국어] 여기까지 설정한 유닛들을 정리한다 */

		if (translation_pre_enabled(iommu)) {	/* [한국어] 크래시 커널이고 번역이 켜져 있다 */
			pr_info("Translation already enabled - trying to copy translation structures\n");	/* [한국어] 인수인계를 시도한다 */

			ret = copy_translation_tables(iommu);	/* [한국어] 앞선 커널의 설정을 옮긴다 */
			if (ret) {	/* [한국어] 실패 */
				/*
				 * We found the IOMMU with translation
				 * enabled - but failed to copy over the
				 * old root-entry table. Try to proceed
				 * by disabling translation now and
				 * allocating a clean root-entry table.
				 * This might cause DMAR faults, but
				 * probably the dump will still succeed.
				 */
				pr_err("Failed to copy translation tables from previous kernel for %s\n",	/* [한국어] 진행 중이던 DMA 가 끊길 수 있음을 알린다 */
				       iommu->name);	/* [한국어] 어느 유닛인지 */
				iommu_disable_translation(iommu);	/* [한국어] 깨끗한 상태에서 다시 시작한다. 폴트가 날 수 있지만 덤프는 성공할 가능성이 높다 (위 영어 주석) */
				clear_translation_pre_enabled(iommu);	/* [한국어] 인수인계 포기 */
			} else {
				pr_info("Copied translation tables from previous kernel for %s\n",	/* [한국어] 인수인계 성공 */
					iommu->name);	/* [한국어] 어느 유닛인지 */
			}
		}

		intel_svm_check(iommu);	/* [한국어] 이 유닛에서 SVA 를 쓸 수 있는지 확인하고 기록한다 */
	}

	/*
	 * Now that qi is enabled on all iommus, set the root entry and flush
	 * caches. This is required on some Intel X58 chipsets, otherwise the
	 * flush_context function will loop forever and the boot hangs.
	 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 2단계 — 모든 유닛의 큐가 준비된 뒤에 루트 테이블을 설치한다 */
		iommu_flush_write_buffer(iommu);	/* [한국어] 쓰기 버퍼를 먼저 비운다 */
		iommu_set_root_entry(iommu);	/* [한국어] 루트 주소를 알리고 캐시를 비운다. 이 순서가 아니면 일부 X58 칩셋에서 flush_context 가 영영 끝나지 않아 부팅이 멈춘다 (위 영어 주석) */
	}

	check_tylersburg_isoch();	/* [한국어] 특정 칩셋의 아이소크로너스 DMA 결함을 확인한다 */

	/*
	 * for each drhd
	 *   enable fault log
	 *   global invalidate context cache
	 *   global invalidate iotlb
	 *   enable translation
	 */
	for_each_iommu(iommu, drhd) {	/* [한국어] 3단계 — 폴트 로그를 켜고 번역을 활성화한다 (위 영어 주석) */
		if (drhd->ignored) {	/* [한국어] 무시하는 유닛이라도 */
			/*
			 * we always have to disable PMRs or DMA may fail on
			 * this device
			 */
			if (force_on)	/* [한국어] 강제 모드면 */
				iommu_disable_protect_mem_regions(iommu);	/* [한국어] 보호 영역만은 꺼야 한다 — 그러지 않으면 이 장치의 DMA 가 실패한다 (위 영어 주석) */
			continue;	/* [한국어] 나머지 설정은 건너뛴다 */
		}

		iommu_flush_write_buffer(iommu);	/* [한국어] 번역을 켜기 전에 쓰기 버퍼를 비운다 */

		if (ecap_prs(iommu->ecap)) {	/* [한국어] 페이지 요청 큐를 지원하는 유닛이면 */
			/*
			 * Call dmar_alloc_hwirq() with dmar_global_lock held,
			 * could cause possible lock race condition.
			 */
			up_write(&dmar_global_lock);	/* [한국어] 전역 락을 잠깐 놓는다. dmar_alloc_hwirq 가 그 락을 다시 잡으려 해 교착이 되기 때문이다 (위 영어 주석) */
			ret = intel_iommu_enable_prq(iommu);	/* [한국어] PRI 큐를 세운다 — SVA 의 요구 페이징이 이것 위에 선다 */
			down_write(&dmar_global_lock);	/* [한국어] 다시 잡는다 */
			if (ret)	/* [한국어] 큐 생성 실패 */
				goto free_iommu;	/* [한국어] 여기까지 설정한 것을 정리한다 */
		}

		ret = dmar_set_interrupt(iommu);	/* [한국어] 폴트 인터럽트를 건다. 이것이 있어야 번역 실패가 보고된다 */
		if (ret)	/* [한국어] 인터럽트 등록 실패 */
			goto free_iommu;	/* [한국어] 정리 */
	}

	return 0;	/* [한국어] 모든 유닛이 번역을 시작했다 */

free_iommu:	/* [한국어] 실패 경로 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 설정한 유닛들을 */
		disable_dmar_iommu(iommu);	/* [한국어] 정지시키고 */
		free_dmar_iommu(iommu);	/* [한국어] 자료구조를 반납한다 */
	}

	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * init_no_remapping_devices - 켤 필요가 없거나 켜서는 안 되는 유닛을 표시한다
 *
 * 두 가지를 판별한다.
 *
 * 첫째, 담당 장치가 하나도 없는 유닛. ACPI 표에 있지만 그 장치들이 실제로는
 * 존재하지 않는 경우이며, 켜 봐야 아무 일도 하지 않으므로 무시한다.
 *
 * 둘째, 그래픽만 담당하는 유닛. 이 표시(gfx_dedicated)가 두 곳에서 쓰인다 —
 * intel_iommu=igfx_off 면 통째로 무시하고, kexec 로 넘어갈 때는 번역을 끄지 않는다.
 * 후자는 디스플레이가 계속 DMA 를 내고 있어, 번역을 끄면 넘어가는 커널에서
 * 화면이 깨지기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기화.
 *
 * 호출 체인: intel_iommu_init → [이 함수]
 */
static void __init init_no_remapping_devices(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회 커서 */
	struct device *dev;	/* [한국어] 범위 표의 장치 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	for_each_drhd_unit(drhd) {	/* [한국어] 1단계 — 담당 장치가 하나도 없는 유닛을 걸러 낸다 */
		if (!drhd->include_all) {	/* [한국어] '나머지 전부'를 담당하는 유닛이 아니면 */
			for_each_active_dev_scope(drhd->devices,	/* [한국어] 범위 표에 살아 있는 장치가 있는지 */
						  drhd->devices_cnt, i, dev)	/* [한국어] 하나만 찾으면 된다 */
				break;	/* [한국어] 첫 장치에서 멈춘다 */
			/* ignore DMAR unit if no devices exist */
			if (i == drhd->devices_cnt)	/* [한국어] 끝까지 갔다 = 장치가 하나도 없다 */
				drhd->ignored = 1;	/* [한국어] 이 유닛은 무시한다 — 담당할 장치가 없으니 켤 이유가 없다 */
		}
	}

	for_each_active_drhd_unit(drhd) {	/* [한국어] 2단계 — 그래픽 전용 유닛을 식별한다 */
		if (drhd->include_all)	/* [한국어] '나머지 전부'를 담당하면 그래픽 전용일 수 없다 */
			continue;	/* [한국어] 건너뛴다 */

		for_each_active_dev_scope(drhd->devices,	/* [한국어] 이 유닛의 장치들 중 */
					  drhd->devices_cnt, i, dev)	/* [한국어] 하나씩 */
			if (!dev_is_pci(dev) || !IS_GFX_DEVICE(to_pci_dev(dev)))	/* [한국어] 그래픽이 아닌 것이 있으면 */
				break;	/* [한국어] 전용이 아니다 */
		if (i < drhd->devices_cnt)	/* [한국어] 중간에 멈췄다 = 그래픽 아닌 장치가 있다 */
			continue;	/* [한국어] 다음 유닛으로 */

		/* This IOMMU has *only* gfx devices. Either bypass it or
		   set the gfx_mapped flag, as appropriate */
		drhd->gfx_dedicated = 1;	/* [한국어] 이 유닛은 그래픽만 담당한다. kexec 때 번역을 끄지 않는 예외가 이 표시를 본다 — 디스플레이가 계속 DMA 를 내고 있기 때문이다 */
		if (disable_igfx_iommu)	/* [한국어] 통합 그래픽을 IOMMU 밖에 두라는 설정이면 */
			drhd->ignored = 1;	/* [한국어] 이 유닛을 통째로 무시한다 (위 영어 주석) */
	}
}

#ifdef CONFIG_SUSPEND	/* [한국어] 서스펜드 지원이 켜진 빌드에서만 */
/*
 * [한국어]
 * init_iommu_hw - 서스펜드에서 깨어난 뒤 하드웨어를 다시 세운다
 *
 * @return: 0 성공, 음수 실패
 *
 * 서스펜드는 IOMMU 레지스터를 초기화하므로 루트 테이블 주소부터 다시 알려야 한다.
 * 소프트웨어 자료구조(페이지 테이블, 컨텍스트 테이블)는 메모리에 그대로 남아 있어
 * 다시 만들 필요는 없다.
 *
 * 무효화 큐를 먼저 되살리는 순서가 중요하다. 아래의 iommu_set_root_entry 가
 * 캐시를 비우는데, 그 무효화가 큐를 통해 나가기 때문이다.
 *
 * 실행 컨텍스트: 리쥼. 인터럽트가 꺼진 상태일 수 있다.
 *
 * 호출 체인: iommu_resume → [이 함수]
 */
static int init_iommu_hw(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회 커서 */
	struct intel_iommu *iommu = NULL;	/* [한국어] 현재 유닛 */
	int ret;	/* [한국어] 결과 */

	for_each_active_iommu(iommu, drhd) {	/* [한국어] 먼저 모든 유닛의 무효화 큐를 되살린다 */
		if (iommu->qi) {	/* [한국어] 큐를 쓰던 유닛이면 */
			ret = dmar_reenable_qi(iommu);	/* [한국어] 큐를 다시 켠다. 아래에서 캐시를 비우려면 이것이 먼저여야 한다 */
			if (ret)	/* [한국어] 실패 */
				return ret;	/* [한국어] 리쥼 불가 */
		}
	}

	for_each_iommu(iommu, drhd) {	/* [한국어] 그 다음 각 유닛을 복원한다 */
		if (drhd->ignored) {	/* [한국어] 무시하는 유닛이라도 */
			/*
			 * we always have to disable PMRs or DMA may fail on
			 * this device
			 */
			if (force_on)	/* [한국어] 강제 모드면 */
				iommu_disable_protect_mem_regions(iommu);	/* [한국어] 보호 영역은 꺼야 한다 (위 영어 주석) */
			continue;	/* [한국어] 나머지는 건너뛴다 */
		}

		iommu_flush_write_buffer(iommu);	/* [한국어] 쓰기 버퍼 정리 */
		iommu_set_root_entry(iommu);	/* [한국어] 루트 테이블을 다시 알린다 — 서스펜드로 레지스터가 초기화되었다 */
		iommu_enable_translation(iommu);	/* [한국어] 번역을 다시 켠다 */
		iommu_disable_protect_mem_regions(iommu);	/* [한국어] BIOS 가 리쥼 중에 다시 켰을 수 있는 보호 영역을 끈다 */
	}

	return 0;	/* [한국어] 하드웨어 상태 복원 완료 */
}

/*
 * [한국어]
 * iommu_flush_all - 모든 유닛의 모든 캐시를 비운다
 *
 * 서스펜드 직전에 부른다. 깨어난 뒤 하드웨어가 옛 번역을 들고 있으면 안 되고,
 * 특히 하드웨어에 따라 서스펜드 중 캐시 내용이 어떻게 되는지 보장이 없기 때문이다.
 *
 * 실행 컨텍스트: 서스펜드 경로.
 *
 * 호출 체인: iommu_suspend → [이 함수]
 */
static void iommu_flush_all(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회 커서 */
	struct intel_iommu *iommu;	/* [한국어] 현재 유닛 */

	for_each_active_iommu(iommu, drhd) {	/* [한국어] 모든 유닛에 대해 */
		iommu->flush.flush_context(iommu, 0, 0, 0,	/* [한국어] 컨텍스트 캐시 전체 무효화 */
					   DMA_CCMD_GLOBAL_INVL);	/* [한국어] 전역 */
		iommu->flush.flush_iotlb(iommu, 0, 0, 0,	/* [한국어] IOTLB 도 */
					 DMA_TLB_GLOBAL_FLUSH);	/* [한국어] 전역. 서스펜드 전에 캐시를 비워, 깨어난 뒤 옛 번역이 남지 않게 한다 */
	}
}

/*
 * [한국어]
 * iommu_suspend - 서스펜드 전에 IOMMU 를 정지시키고 상태를 저장한다
 *
 * @data:   syscore 콜백 인자 (쓰지 않는다)
 * @return: 항상 0
 *
 * 하는 일은 세 가지다. 캐시를 비우고, 번역을 끄고, 하드웨어가 잃어버릴 레지스터
 * 값을 소프트웨어에 보관한다.
 *
 * 보관하는 것이 폴트 인터럽트 설정뿐인 것에 주목할 것. 루트 테이블 주소는
 * 소프트웨어가 이미 알고 있어 다시 계산할 수 있지만, MSI 메시지 주소와 데이터는
 * 인터럽트 코어가 할당한 값이라 재현할 수 없다.
 *
 * 실행 컨텍스트: syscore 서스펜드. 인터럽트가 꺼진 상태.
 *
 * 호출 체인: syscore_ops.suspend == [이 함수]
 */
static int iommu_suspend(void *data)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회 커서 */
	struct intel_iommu *iommu = NULL;	/* [한국어] 현재 유닛 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 */

	iommu_flush_all();	/* [한국어] 모든 캐시를 먼저 비운다 */

	for_each_active_iommu(iommu, drhd) {	/* [한국어] 각 유닛에 대해 */
		iommu_disable_translation(iommu);	/* [한국어] 번역을 끈다 — 서스펜드 중에는 DMA 가 없어야 한다 */

		raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 직렬화 */

		iommu->iommu_state[SR_DMAR_FECTL_REG] =	/* [한국어] 폴트 인터럽트 제어 레지스터를 */
			readl(iommu->reg + DMAR_FECTL_REG);	/* [한국어] 저장해 둔다. 서스펜드로 하드웨어 상태가 사라지므로 소프트웨어가 기억해야 한다 */
		iommu->iommu_state[SR_DMAR_FEDATA_REG] =	/* [한국어] 인터럽트 데이터 */
			readl(iommu->reg + DMAR_FEDATA_REG);	/* [한국어] MSI 메시지의 데이터 부분 */
		iommu->iommu_state[SR_DMAR_FEADDR_REG] =	/* [한국어] 인터럽트 주소 하위 */
			readl(iommu->reg + DMAR_FEADDR_REG);	/* [한국어] MSI 도어벨 주소 */
		iommu->iommu_state[SR_DMAR_FEUADDR_REG] =	/* [한국어] 인터럽트 주소 상위 */
			readl(iommu->reg + DMAR_FEUADDR_REG);	/* [한국어] 64비트 주소의 나머지 절반 */

		raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 끝 */
	}
	return 0;	/* [한국어] 서스펜드 준비 완료 */
}

/*
 * [한국어]
 * iommu_resume - 깨어난 뒤 IOMMU 를 복원한다
 *
 * @data: syscore 콜백 인자 (쓰지 않는다)
 *
 * 하드웨어를 다시 세우고 보관해 둔 폴트 인터럽트 설정을 되돌린다.
 *
 * 복원에 실패했을 때의 처리가 이 함수의 성격을 말해 준다. force_on(TXT 부팅)이면
 * panic 이다 — 격리가 보장되지 않는 상태로 계속 동작하느니 멈추는 편이 낫다는
 * 판단이며, 그것이 TXT 로 부팅한 이유이기도 하다.
 *
 * 실행 컨텍스트: syscore 리쥼. 인터럽트가 꺼진 상태.
 *
 * 호출 체인: syscore_ops.resume == [이 함수] → init_iommu_hw
 */
static void iommu_resume(void *data)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회 커서 */
	struct intel_iommu *iommu = NULL;	/* [한국어] 현재 유닛 */
	unsigned long flag;	/* [한국어] 인터럽트 상태 */

	if (init_iommu_hw()) {	/* [한국어] 하드웨어 복원 실패 */
		if (force_on)	/* [한국어] TXT 로 부팅한 경우 */
			panic("tboot: IOMMU setup failed, DMAR can not resume!\n");	/* [한국어] 격리 없이 계속 동작하느니 멈춘다 — TXT 로 부팅한 이유가 그 격리이기 때문 */
		else
			WARN(1, "IOMMU setup failed, DMAR can not resume!\n");	/* [한국어] 그 외에는 경고만. 격리를 잃은 채로 시스템이 계속 돈다 */
		return;	/* [한국어] 더 복원할 것이 없다 */
	}

	for_each_active_iommu(iommu, drhd) {	/* [한국어] 각 유닛의 폴트 인터럽트 설정을 되돌린다 */

		raw_spin_lock_irqsave(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 직렬화 */

		writel(iommu->iommu_state[SR_DMAR_FECTL_REG],	/* [한국어] 인터럽트 제어 */
			iommu->reg + DMAR_FECTL_REG);	/* [한국어] 서스펜드 전 값 그대로 */
		writel(iommu->iommu_state[SR_DMAR_FEDATA_REG],	/* [한국어] MSI 데이터 */
			iommu->reg + DMAR_FEDATA_REG);	/* [한국어] 인터럽트 코어가 할당한 값이라 재현할 수 없어 보관해 두었다 */
		writel(iommu->iommu_state[SR_DMAR_FEADDR_REG],	/* [한국어] MSI 주소 하위 */
			iommu->reg + DMAR_FEADDR_REG);	/* [한국어] 도어벨 주소 */
		writel(iommu->iommu_state[SR_DMAR_FEUADDR_REG],	/* [한국어] MSI 주소 상위 */
			iommu->reg + DMAR_FEUADDR_REG);	/* [한국어] 64비트 주소의 나머지 */

		raw_spin_unlock_irqrestore(&iommu->register_lock, flag);	/* [한국어] 레지스터 접근 끝 */
	}
}

static const struct syscore_ops iommu_syscore_ops = {	/* [한국어] 시스템 코어 서스펜드/리쥼 콜백. 일반 장치 PM 보다 늦게 내려가고 먼저 올라온다 — IOMMU 가 꺼지기 전에 모든 장치가 멈춰야 하기 때문 */
	.resume		= iommu_resume,	/* [한국어] 리쥼 콜백 */
	.suspend	= iommu_suspend,	/* [한국어] 서스펜드 콜백 */
};

static struct syscore iommu_syscore = {	/* [한국어] 등록할 syscore 객체 */
	.ops = &iommu_syscore_ops,	/* [한국어] 위 콜백들 */
};

/*
 * [한국어]
 * init_iommu_pm_ops - VT-d 유닛의 서스펜드/리줌 콜백을 syscore 에 등록한다
 *
 * @return: 없음. 등록은 실패하지 않는다.
 *
 * 왜 syscore 인가: 일반 디바이스 PM 콜백은 다른 드라이버들이 아직 살아 있는
 * 시점에 불린다. 그런데 IOMMU 는 그 드라이버들의 DMA 가 통과하는 길목이라,
 * "모든 디바이스가 멈춘 뒤 / 어떤 디바이스도 아직 깨어나기 전"에만 끄고 켤 수
 * 있다. syscore_ops 는 정확히 그 시점(비인터럽트, CPU 하나만 살아 있는 상태)에
 * 호출되는 마지막/첫 번째 훅이므로 IOMMU 처럼 시스템 전역 자원을 다루는 코드가
 * 쓰는 자리다.
 * 실행 컨텍스트: 부팅 중 init_dmars() 성공 후 한 번. 프로세스 컨텍스트.
 * 등록 이후 iommu_suspend()/iommu_resume() 이 각각 서스펜드 마지막 단계와
 * 리줌 첫 단계에서 불린다.
 *
 * 호출 체인:
 *   intel_iommu_init() → [init_iommu_pm_ops] → register_syscore()
 *   (이후) syscore 서스펜드/리줌 → iommu_suspend()/iommu_resume()
 */
static void __init init_iommu_pm_ops(void)
{
	register_syscore(&iommu_syscore);	/* [한국어] PM 콜백을 등록한다 */
}

#else
static inline void init_iommu_pm_ops(void) {}	/* [한국어] 서스펜드를 지원하지 않는 빌드의 빈 구현 */
#endif	/* CONFIG_PM */

/*
 * [한국어]
 * rmrr_sanity_check - 펌웨어가 보고한 RMRR 구간이 쓸 만한 값인지 검사한다
 *
 * @rmrr: DMAR 표에서 읽은 RMRR(Reserved Memory Region Reporting) 항목.
 *        base_address ~ end_address 는 "닫힌 구간"이다(끝 주소도 포함).
 * @return: 0 이면 정상, -EINVAL 이면 펌웨어가 이상한 값을 줬다.
 *
 * RMRR 이 무엇인가: 펌웨어가 "이 장치는 이 물리 메모리 구간에 계속 DMA 를 한다,
 * OS 가 IOMMU 를 켜더라도 이 구간만은 항등(identity) 매핑으로 남겨 둬라"고
 * 선언하는 표다. 레거시 USB 키보드 에뮬레이션, BIOS 가 관리하는 관리 엔진 등이
 * 대표적이다. 즉 RMRR 은 IOMMU 보호에 뚫는 구멍이므로, 값이 조금이라도
 * 수상하면 받아들이지 않는 편이 안전하다.
 *
 * 검사 항목:
 *   1) 시작 주소가 페이지 정렬인가 — 페이지 단위로만 매핑할 수 있으므로.
 *   2) end_address + 1 이 페이지 정렬인가 — 닫힌 구간이라 끝의 다음이 경계다.
 *   3) 구간이 뒤집혀 있지 않은가.
 *   4) 아키텍처별 추가 검사(arch_rmrr_sanity_check) — x86 에서는 이 구간이
 *      커널이 쓰는 정상 메모리(E820 usable)와 겹치면 거부한다. 겹친다면 장치가
 *      커널 메모리를 마음대로 쓸 권한을 얻는 셈이기 때문이다.
 *
 * 실행 컨텍스트: 부팅 중 DMAR 표 파싱, 또는 유닛 핫플러그. 프로세스 컨텍스트.
 * 에러 처리: 호출자(dmar_parse_one_rmrr)는 이 검사가 실패해도 항목을 버리지는
 * 않고, FW_BUG 경고를 찍고 커널을 오염(taint) 표시한 뒤 그대로 등록한다.
 * 부정확한 RMRR 이라도 무시하면 그 장치가 아예 동작하지 않을 수 있어서다.
 *
 * 호출 체인:
 *   dmar_parse_one_rmrr() → [rmrr_sanity_check] → arch_rmrr_sanity_check()
 */
static int __init rmrr_sanity_check(struct acpi_dmar_reserved_memory *rmrr)
{
	if (!IS_ALIGNED(rmrr->base_address, PAGE_SIZE) ||	/* [한국어] 시작 주소가 페이지 정렬이 아니거나 */
	    !IS_ALIGNED(rmrr->end_address + 1, PAGE_SIZE) ||	/* [한국어] 끝이 페이지 경계가 아니거나 (닫힌 구간이라 +1) */
	    rmrr->end_address <= rmrr->base_address ||	/* [한국어] 범위가 거꾸로이거나 */
	    arch_rmrr_sanity_check(rmrr))	/* [한국어] 아키텍처가 추가로 거부하면 (예: 그 범위가 커널 이미지와 겹치는 경우) */
		return -EINVAL;	/* [한국어] 이 RMRR 은 신뢰할 수 없다 */

	return 0;	/* [한국어] 정상적인 항목 */
}

/*
 * [한국어]
 * dmar_parse_one_rmrr - DMAR 표의 RMRR 항목 하나를 커널 자료구조로 등록한다
 *
 * @header: DMAR 표 안의 항목 헤더. 실제로는 struct acpi_dmar_reserved_memory 다.
 *          DMAR 표 자체는 부팅 내내 매핑된 채로 남으므로 포인터를 그대로 보관해도 된다.
 *          (ATSR/SATC 와 다른 점 — 그쪽은 _DSM 이 준 임시 버퍼라 복사해야 한다.)
 * @arg: dmar_table_detect 계열의 콜백 규약 때문에 있는 자리. 여기서는 쓰지 않는다.
 * @return: 0 성공, -ENOMEM 할당/파싱 실패.
 *
 * 하는 일: 항목의 값을 검사하고(rmrr_sanity_check), dmar_rmrr_unit 을 만들어
 * 구간과 대상 장치 목록을 담은 뒤 전역 dmar_rmrr_units 목록에 매단다.
 * 이 목록은 나중에 두 곳에서 쓰인다.
 *   - device_rmrr_is_relaxable() / device_def_domain_type(): 이 장치가 RMRR 을
 *     가지면 기본 도메인을 항등(identity)으로 강제할지 결정한다.
 *   - intel_iommu_get_resv_regions(): 그 구간을 예약 영역으로 보고해, IOVA
 *     할당기가 그 주소를 다른 매핑에 쓰지 않게 한다.
 *
 * 값이 이상할 때: 항목을 버리지 않고 FW_BUG 경고를 출력하고
 * add_taint(TAINT_FIRMWARE_WORKAROUND) 로 커널에 표시를 남긴 뒤 계속 진행한다.
 * BIOS 벤더/버전을 함께 찍는 이유는, 이후 올라오는 버그 리포트에서 펌웨어를
 * 의심할 근거를 남기기 위해서다.
 *
 * 장치 목록: 항목 구조체 바로 뒤에 device scope 배열이 이어 붙어 있다.
 * (void *)(rmrr + 1) 이 그 시작, ((void *)rmrr) + header.length 가 끝이다.
 * 이 포인터 산술이 ACPI 가변 길이 항목을 읽는 표준 관용구다.
 *
 * 실행 컨텍스트: 부팅 중 DMAR 파싱(__init). 프로세스 컨텍스트, 단일 스레드.
 * 에러 처리: 장치 목록 파싱이 실패하면 free_rmrru 로 가서 자료구조를 반납한다.
 *
 * 호출 체인:
 *   dmar_table_init() → dmar_walk_dmar_table() → [dmar_parse_one_rmrr]
 *     → rmrr_sanity_check() / dmar_alloc_dev_scope()
 */
int __init dmar_parse_one_rmrr(struct acpi_dmar_header *header, void *arg)
{
	struct acpi_dmar_reserved_memory *rmrr;	/* [한국어] ACPI 항목 */
	struct dmar_rmrr_unit *rmrru;	/* [한국어] 만들 커널 자료구조 */

	rmrr = (struct acpi_dmar_reserved_memory *)header;	/* [한국어] ACPI 헤더를 RMRR 항목으로 */
	if (rmrr_sanity_check(rmrr)) {	/* [한국어] 값이 이상하면 */
		pr_warn(FW_BUG	/* [한국어] 펌웨어 버그임을 명시한다 */
			   "Your BIOS is broken; bad RMRR [%#018Lx-%#018Lx]\n"
			   "BIOS vendor: %s; Ver: %s; Product Version: %s\n",
			   rmrr->base_address, rmrr->end_address,
			   dmi_get_system_info(DMI_BIOS_VENDOR),
			   dmi_get_system_info(DMI_BIOS_VERSION),
			   dmi_get_system_info(DMI_PRODUCT_VERSION));
		add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);	/* [한국어] 커널에 오염 표시를 남긴다 — 이후 문제 보고에서 펌웨어를 의심할 근거가 된다 */
	}

	rmrru = kzalloc_obj(*rmrru);	/* [한국어] 커널 자료구조 */
	if (!rmrru)	/* [한국어] 할당 실패 */
		goto out;

	rmrru->hdr = header;	/* [한국어] 원본 ACPI 항목을 가리킨다 (DMAR 표는 부팅 내내 유지된다) */

	rmrru->base_address = rmrr->base_address;	/* [한국어] 예약 구간의 시작 */
	rmrru->end_address = rmrr->end_address;	/* [한국어] 끝 */

	rmrru->devices = dmar_alloc_dev_scope((void *)(rmrr + 1),	/* [한국어] 이 예약이 적용되는 장치 목록을 파싱한다. 항목 뒤에 이어 붙어 있다 */
				((void *)rmrr) + rmrr->header.length,
				&rmrru->devices_cnt);
	if (rmrru->devices_cnt && rmrru->devices == NULL)	/* [한국어] 장치가 있다고 했는데 파싱에 실패했다 */
		goto free_rmrru;

	list_add(&rmrru->list, &dmar_rmrr_units);	/* [한국어] 전역 목록에 등록. 나중에 장치를 프로브할 때 이 목록을 훑어 항등 매핑을 만든다 */

	return 0;	/* [한국어] 항목 하나 처리 완료 */
free_rmrru:	/* [한국어] 파싱 실패 경로 */
	kfree(rmrru);	/* [한국어] 자료구조 반납 */
out:	/* [한국어] 할당 실패가 합류 */
	return -ENOMEM;	/* [한국어] 처리 실패 */
}

/*
 * [한국어]
 * dmar_find_atsr - 같은 내용의 ATSR 항목이 이미 등록되어 있는지 찾는다
 *
 * @atsr: 방금 파싱한 ACPI ATSR 항목.
 * @return: 같은 항목이 이미 있으면 그 dmar_atsr_unit, 없으면 NULL.
 *
 * ATSR 이 무엇인가: ATSR(Root Port ATS Capability Reporting)은 "이 PCIe 루트
 * 포트 아래의 장치들은 ATS(Address Translation Services)를 쓸 수 있다"고
 * 선언하는 DMAR 표 항목이다. ATS 는 장치가 자기 안에 번역 캐시(ATC/디바이스
 * TLB)를 두는 기능이라, 커널이 매핑을 풀 때 IOMMU 뿐 아니라 그 장치의 캐시도
 * 무효화해야 한다. 그래서 어떤 포트가 ATS 를 지원하는지 미리 알아야 한다.
 *
 * 왜 중복 검사가 필요한가: 부팅 때 파싱한 표를, PCIe 호스트 브리지 핫플러그
 * 이벤트에서 ACPI _DSM 이 다시 돌려주는 경우가 있다. 그대로 등록하면 같은
 * 항목이 두 번 목록에 들어간다. 그래서 세그먼트 → 길이 → 전체 바이트 비교
 * 순으로(싼 것부터) 같은 항목인지 확인한다.
 *
 * 동기화: dmar_atsr_units 는 RCU 목록이다. 순회 중에 dmar_parse_one_satc/atsr
 * 이 list_add_rcu 로 항목을 추가하거나 dmar_release_one_atsr 가 지울 수 있어서,
 * 읽는 쪽은 rcu 순회를, 지우는 쪽은 synchronize_rcu 를 쓴다. dmar_rcu_check()
 * 는 "rcu_read_lock 안이거나 dmar_global_lock 을 쥐고 있다"는 조건을 lockdep 에
 * 알려 주는 표현식이다.
 * 실행 컨텍스트: 부팅 파싱 또는 핫플러그. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dmar_parse_one_atsr()/dmar_release_one_atsr()/dmar_check_one_atsr()
 *     → [dmar_find_atsr]
 */
static struct dmar_atsr_unit *dmar_find_atsr(struct acpi_dmar_atsr *atsr)
{
	struct dmar_atsr_unit *atsru;	/* [한국어] 순회 커서 */
	struct acpi_dmar_atsr *tmp;	/* [한국어] 비교할 항목 */

	list_for_each_entry_rcu(atsru, &dmar_atsr_units, list,	/* [한국어] RCU 순회 — 유닛 핫플러그로 목록이 바뀔 수 있다 */
				dmar_rcu_check()) {
		tmp = (struct acpi_dmar_atsr *)atsru->hdr;	/* [한국어] 보관된 원본 항목 */
		if (atsr->segment != tmp->segment)	/* [한국어] 세그먼트가 다르면 */
			continue;	/* [한국어] 다른 항목 */
		if (atsr->header.length != tmp->header.length)	/* [한국어] 길이가 다르면 */
			continue;	/* [한국어] 다른 항목 */
		if (memcmp(atsr, tmp, atsr->header.length) == 0)	/* [한국어] 내용이 완전히 같으면 */
			return atsru;	/* [한국어] 같은 항목이다. 핫플러그로 같은 표를 다시 파싱할 때 중복 등록을 막는다 */
	}

	return NULL;	/* [한국어] 등록된 적이 없다 */
}

/*
 * [한국어]
 * dmar_parse_one_atsr - ACPI ATSR 항목 하나를 커널 자료구조로 등록한다
 *
 * @hdr: DMAR 표 또는 _DSM 이 돌려준 버퍼 안의 항목 헤더.
 * @arg: 콜백 규약상의 자리. 쓰지 않는다.
 * @return: 0 성공(중복이라 아무것도 안 한 경우도 0), -ENOMEM 실패.
 *
 * 하는 일: 중복이 아니면 dmar_atsr_unit 을 만들어 ACPI 항목의 사본과 ATS 를
 * 지원하는 포트 목록을 담고, RCU 목록 dmar_atsr_units 에 매단다.
 *
 * 왜 ACPI 항목을 복사하는가: 부팅 시의 DMAR 표는 계속 매핑되어 있지만,
 * 핫플러그 경로에서는 ACPI _DSM 메서드가 슬랩에서 잡은 버퍼를 돌려주고 그
 * 버퍼는 반환 즉시 해제된다. 그래서 구조체 뒤에 hdr->length 만큼을 함께
 * 할당(kzalloc(sizeof + length))하고 내용을 복사한 뒤, atsru->hdr 이 그
 * 사본을 가리키게 한다. 이렇게 하면 해제도 kfree 한 번으로 끝난다.
 *
 * include_all: flags 의 bit0 이 켜져 있으면 "이 세그먼트의 모든 루트 포트가
 * ATS 를 지원한다"는 뜻이라 장치 목록이 없다. 꺼져 있으면 항목 뒤에 이어진
 * device scope 배열이 대상 포트를 하나씩 지정한다. 나중에
 * dmar_find_matched_atsr_unit() 이 이 두 경우를 구분해 판단한다.
 *
 * 부팅 후 핫플러그인데 VT-d 가 꺼져 있으면(system_state >= SYSTEM_RUNNING &&
 * !intel_iommu_enabled) 등록할 이유가 없으므로 곧바로 0 을 돌려준다.
 *
 * 실행 컨텍스트: 부팅 중 DMAR 파싱, 또는 PCIe 호스트 브리지 핫플러그 알림.
 * 프로세스 컨텍스트. 목록 추가는 list_add_rcu 라 동시 순회와 안전하다.
 * 에러 처리: 장치 목록 파싱이 실패하면 방금 만든 자료구조를 해제하고
 * -ENOMEM 을 돌려준다. 호출자는 핫플러그 전체를 실패시킨다.
 *
 * 호출 체인:
 *   dmar_walk_dmar_table()/dmar_hotplug_insert() → [dmar_parse_one_atsr]
 *     → dmar_find_atsr() / dmar_alloc_dev_scope()
 */
int dmar_parse_one_atsr(struct acpi_dmar_header *hdr, void *arg)
{
	struct acpi_dmar_atsr *atsr;	/* [한국어] ACPI 항목 */
	struct dmar_atsr_unit *atsru;	/* [한국어] 커널 자료구조 */

	if (system_state >= SYSTEM_RUNNING && !intel_iommu_enabled)	/* [한국어] 부팅이 끝난 뒤의 핫플러그인데 VT-d 가 꺼져 있으면 */
		return 0;	/* [한국어] 등록할 이유가 없다 */

	atsr = container_of(hdr, struct acpi_dmar_atsr, header);	/* [한국어] 헤더에서 ATSR 항목으로 */
	atsru = dmar_find_atsr(atsr);	/* [한국어] 이미 등록된 항목인지 */
	if (atsru)	/* [한국어] 있으면 */
		return 0;	/* [한국어] 중복 등록하지 않는다 */

	atsru = kzalloc(sizeof(*atsru) + hdr->length, GFP_KERNEL);	/* [한국어] 자료구조와 ACPI 항목 사본을 한 번에 잡는다 */
	if (!atsru)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 등록 불가 */

	/*
	 * If memory is allocated from slab by ACPI _DSM method, we need to
	 * copy the memory content because the memory buffer will be freed
	 * on return.
	 */
	atsru->hdr = (void *)(atsru + 1);	/* [한국어] 사본을 담을 자리는 구조체 바로 뒤 */
	memcpy(atsru->hdr, hdr, hdr->length);	/* [한국어] 내용을 복사한다. ACPI _DSM 이 준 버퍼는 반환 즉시 해제되므로 참조만 들고 있을 수 없다 (위 영어 주석) */
	atsru->include_all = atsr->flags & 0x1;	/* [한국어] 이 유닛 아래 모든 포트가 ATS 를 지원한다는 표시 */
	if (!atsru->include_all) {	/* [한국어] 특정 포트만 지정한 경우 */
		atsru->devices = dmar_alloc_dev_scope((void *)(atsr + 1),	/* [한국어] 그 포트 목록을 파싱한다 */
				(void *)atsr + atsr->header.length,	/* [한국어] 항목의 끝까지 */
				&atsru->devices_cnt);	/* [한국어] 개수를 받는다 */
		if (atsru->devices_cnt && atsru->devices == NULL) {	/* [한국어] 파싱 실패 */
			kfree(atsru);	/* [한국어] 자료구조 반납 */
			return -ENOMEM;	/* [한국어] 등록 실패 */
		}
	}

	list_add_rcu(&atsru->list, &dmar_atsr_units);	/* [한국어] RCU 목록에 등록. 순회 중에도 안전하게 추가된다 */

	return 0;	/* [한국어] 항목 등록 완료 */
}

/*
 * [한국어]
 * intel_iommu_free_atsr - dmar_atsr_unit 하나와 그 장치 목록을 해제한다
 *
 * @atsru: 이미 목록에서 빠져 있고, RCU 유예 기간도 지난 자료구조.
 * @return: 없음.
 *
 * 장치 목록(devices 배열)은 dmar_alloc_dev_scope 가 따로 잡은 메모리라
 * 먼저 dmar_free_dev_scope 로 반납하고, 그 다음 자료구조 본체를 해제한다.
 * ACPI 항목 사본은 atsru 와 같은 할당 안에 있으므로 별도 해제가 없다.
 *
 * 호출 전 조건: 반드시 list_del_rcu + synchronize_rcu 이후여야 한다.
 * 그러지 않으면 RCU 순회 중인 다른 CPU 가 해제된 메모리를 읽는다.
 * 실행 컨텍스트: 프로세스 컨텍스트(synchronize_rcu 뒤라 잠들 수 있는 자리).
 *
 * 호출 체인:
 *   dmar_release_one_atsr()/intel_iommu_free_dmars() → [intel_iommu_free_atsr]
 *     → dmar_free_dev_scope()
 */
static void intel_iommu_free_atsr(struct dmar_atsr_unit *atsru)
{
	dmar_free_dev_scope(&atsru->devices, &atsru->devices_cnt);	/* [한국어] 장치 목록 해제 */
	kfree(atsru);	/* [한국어] 자료구조 반납 (ACPI 사본도 같은 할당 안에 있다) */
}

/*
 * [한국어]
 * dmar_release_one_atsr - 등록된 ATSR 항목을 목록에서 제거하고 해제한다
 *
 * @hdr: 제거할 ATSR 항목의 ACPI 헤더.
 * @arg: 콜백 규약상의 자리. 쓰지 않는다.
 * @return: 항상 0. 등록된 적이 없어도 성공으로 본다(제거할 게 없을 뿐이다).
 *
 * 언제 불리는가: PCIe 호스트 브리지가 뽑히면서 그 브리지가 보고했던 ATSR 이
 * 더 이상 유효하지 않을 때. 실제 제거 전에 dmar_check_one_atsr 가 먼저
 * "지워도 되는지"를 물어보고, 통과한 뒤에 이 함수가 불린다.
 *
 * RCU 제거 3단계: list_del_rcu 로 목록에서 끊고 → synchronize_rcu 로 이미
 * 진행 중인 순회가 모두 끝날 때까지 기다리고 → 그제서야 해제한다. 이 순서를
 * 어기면 dmar_find_atsr 처럼 rcu 순회 중인 코드가 해제된 메모리를 읽는다.
 * synchronize_rcu 는 잠들 수 있으므로 프로세스 컨텍스트에서만 부를 수 있다.
 *
 * 실행 컨텍스트: ACPI 핫플러그 처리 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dmar_hotplug_remove() → [dmar_release_one_atsr]
 *     → dmar_find_atsr() → list_del_rcu() → synchronize_rcu()
 *     → intel_iommu_free_atsr()
 */
int dmar_release_one_atsr(struct acpi_dmar_header *hdr, void *arg)
{
	struct acpi_dmar_atsr *atsr;	/* [한국어] ACPI 항목 */
	struct dmar_atsr_unit *atsru;	/* [한국어] 등록된 자료구조 */

	atsr = container_of(hdr, struct acpi_dmar_atsr, header);	/* [한국어] 헤더에서 항목으로 */
	atsru = dmar_find_atsr(atsr);	/* [한국어] 등록된 것을 찾는다 */
	if (atsru) {	/* [한국어] 있으면 */
		list_del_rcu(&atsru->list);	/* [한국어] 목록에서 뺀다 */
		synchronize_rcu();	/* [한국어] 진행 중인 순회가 끝날 때까지 기다린다. 그러지 않으면 해제된 항목을 읽는 순회가 남는다 */
		intel_iommu_free_atsr(atsru);	/* [한국어] 이제 안전하게 해제 */
	}

	return 0;	/* [한국어] 제거 완료 */
}

/*
 * [한국어]
 * dmar_check_one_atsr - ATSR 항목을 지워도 되는지 미리 확인한다
 *
 * @hdr: 지우려는 ATSR 항목의 ACPI 헤더.
 * @arg: 콜백 규약상의 자리. 쓰지 않는다.
 * @return: 0 이면 제거해도 된다, -EBUSY 면 아직 쓰이는 중이라 거절.
 *
 * 왜 확인 단계가 따로 있는가: ACPI 핫플러그는 "검사 → 실제 수행"의 2단계로
 * 돈다. 여러 항목 중 하나라도 제거가 불가능하면 아무것도 건드리지 않고 통째로
 * 실패시켜야 중간 상태가 남지 않는다. 이 함수가 그 검사 단계다.
 *
 * 판단 기준: include_all 항목이면 특정 장치를 지목한 게 아니므로 언제든 지울
 * 수 있다. 반대로 device scope 로 포트를 지목한 항목이면,
 * for_each_active_dev_scope 로 아직 살아 있는(struct device 가 매달린) 항목이
 * 하나라도 있는지 본다. 하나라도 있으면 그 장치가 ATS 를 쓰는 중일 수 있으므로
 * -EBUSY 로 거절한다 — 루프 몸통에서 곧바로 return 하는 것은 "존재 여부"만
 * 보면 되기 때문이다.
 *
 * 실행 컨텍스트: ACPI 핫플러그 검사 단계. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dmar_hotplug_remove() → [dmar_check_one_atsr] → dmar_find_atsr()
 */
int dmar_check_one_atsr(struct acpi_dmar_header *hdr, void *arg)
{
	int i;	/* [한국어] 장치 순회 인덱스 */
	struct device *dev;	/* [한국어] 순회 커서 */
	struct acpi_dmar_atsr *atsr;	/* [한국어] ACPI 항목 */
	struct dmar_atsr_unit *atsru;	/* [한국어] 등록된 자료구조 */

	atsr = container_of(hdr, struct acpi_dmar_atsr, header);	/* [한국어] 헤더에서 항목으로 */
	atsru = dmar_find_atsr(atsr);	/* [한국어] 등록된 것을 찾는다 */
	if (!atsru)	/* [한국어] 없으면 제거해도 무방하다 */
		return 0;	/* [한국어] 허용 */

	if (!atsru->include_all && atsru->devices && atsru->devices_cnt) {	/* [한국어] 특정 포트를 지정한 항목이고 장치 목록이 있으면 */
		for_each_active_dev_scope(atsru->devices, atsru->devices_cnt,	/* [한국어] 아직 살아 있는 장치가 하나라도 있는지 */
					  i, dev)	/* [한국어] 순회 */
			return -EBUSY;	/* [한국어] 있으면 제거를 거절한다 — 그 장치가 ATS 를 쓰는 중일 수 있다 */
	}

	return 0;	/* [한국어] 제거해도 된다 */
}

/*
 * [한국어]
 * dmar_find_satc - 같은 내용의 SATC 항목이 이미 등록되어 있는지 찾는다
 *
 * @satc: 방금 파싱한 ACPI SATC 항목.
 * @return: 같은 항목이 있으면 그 dmar_satc_unit, 없으면 NULL.
 *
 * SATC 가 무엇인가: SATC(SoC Integrated Address Translation Cache)는 SoC 에
 * 통합된 장치들 중 "번역 캐시(ATC)를 반드시 켜야만 정상 동작하는" 것들을
 * 알려 주는 DMAR 표 항목이다. ATSR 이 "ATS 를 쓸 수 있다"는 능력 보고라면,
 * SATC 의 atc_required 플래그는 "ATS 가 없으면 이 장치는 못 쓴다"는 요구다.
 * 이 값은 나중에 dev_needs_extra_dtlb_flush / ATS 활성화 판단에 쓰인다.
 *
 * 비교 방식은 dmar_find_atsr 과 같다: 세그먼트 → 헤더 길이 → 전체 바이트
 * memcmp 순으로 싼 검사부터 걸러 낸다. 목적도 같다 — 핫플러그에서 같은 표를
 * 다시 파싱했을 때 중복 등록을 막는 것.
 *
 * 동기화: dmar_satc_units 는 RCU 목록이며 dmar_rcu_check() 가 lockdep 에
 * 유효한 보호 조건을 알려 준다.
 * 실행 컨텍스트: 부팅 파싱 또는 핫플러그. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dmar_parse_one_satc() → [dmar_find_satc]
 */
static struct dmar_satc_unit *dmar_find_satc(struct acpi_dmar_satc *satc)
{
	struct dmar_satc_unit *satcu;	/* [한국어] 순회 커서 */
	struct acpi_dmar_satc *tmp;	/* [한국어] 비교할 항목 */

	list_for_each_entry_rcu(satcu, &dmar_satc_units, list,	/* [한국어] RCU 순회 */
				dmar_rcu_check()) {	/* [한국어] RCU 사용 조건 검증 */
		tmp = (struct acpi_dmar_satc *)satcu->hdr;	/* [한국어] 보관된 원본 */
		if (satc->segment != tmp->segment)	/* [한국어] 세그먼트가 다르면 */
			continue;	/* [한국어] 다른 항목 */
		if (satc->header.length != tmp->header.length)	/* [한국어] 길이가 다르면 */
			continue;	/* [한국어] 다른 항목 */
		if (memcmp(satc, tmp, satc->header.length) == 0)	/* [한국어] 내용이 같으면 */
			return satcu;	/* [한국어] 같은 항목 */
	}

	return NULL;	/* [한국어] 등록된 적이 없다 */
}

/*
 * [한국어]
 * dmar_parse_one_satc - ACPI SATC 항목 하나를 커널 자료구조로 등록한다
 *
 * @hdr: DMAR 표 또는 _DSM 버퍼 안의 SATC 항목 헤더.
 * @arg: 콜백 규약상의 자리. 쓰지 않는다.
 * @return: 0 성공(중복이면 아무것도 안 하고 0), -ENOMEM 실패.
 *
 * 하는 일: 중복이 아니면 dmar_satc_unit 을 만들어 ACPI 사본과 대상 장치 목록,
 * 그리고 atc_required 플래그를 담아 RCU 목록에 매단다.
 *
 * atc_required(flags bit0): 이 목록에 있는 장치들은 디바이스 TLB 없이는
 * 동작하지 못한다는 뜻이다. 그래서 나중에 이 장치를 프로브할 때, ATS 능력이
 * 있으면 무조건 켜야 하고 끌 수도 없다. 일반 PCIe 장치의 ATS 가 "성능을 위한
 * 선택"인 것과 달리 SoC 통합 장치에서는 동작 조건이다.
 *
 * ATSR 과 마찬가지로 ACPI 버퍼가 곧 해제될 수 있으므로
 * kzalloc(sizeof(*satcu) + hdr->length) 로 사본 자리를 함께 잡아 복사한다.
 * 장치 목록은 include_all 같은 예외 없이 항상 파싱한다 — SATC 는 대상 장치를
 * 반드시 지목하는 표이기 때문이다.
 *
 * 실행 컨텍스트: 부팅 중 DMAR 파싱, 또는 핫플러그. 프로세스 컨텍스트.
 * 에러 처리: 장치 목록 파싱 실패 시 자료구조를 반납하고 -ENOMEM.
 *
 * 호출 체인:
 *   dmar_walk_dmar_table()/dmar_hotplug_insert() → [dmar_parse_one_satc]
 *     → dmar_find_satc() / dmar_alloc_dev_scope()
 */
int dmar_parse_one_satc(struct acpi_dmar_header *hdr, void *arg)
{
	struct acpi_dmar_satc *satc;	/* [한국어] ACPI 항목 */
	struct dmar_satc_unit *satcu;	/* [한국어] 커널 자료구조 */

	if (system_state >= SYSTEM_RUNNING && !intel_iommu_enabled)	/* [한국어] 부팅 후 핫플러그인데 VT-d 가 꺼져 있으면 */
		return 0;	/* [한국어] 등록하지 않는다 */

	satc = container_of(hdr, struct acpi_dmar_satc, header);	/* [한국어] 헤더에서 항목으로 */
	satcu = dmar_find_satc(satc);	/* [한국어] 이미 등록되었는지 */
	if (satcu)	/* [한국어] 있으면 */
		return 0;	/* [한국어] 중복 등록하지 않는다 */

	satcu = kzalloc(sizeof(*satcu) + hdr->length, GFP_KERNEL);	/* [한국어] 자료구조와 ACPI 사본을 한 번에 */
	if (!satcu)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 등록 불가 */

	satcu->hdr = (void *)(satcu + 1);	/* [한국어] 사본 자리 */
	memcpy(satcu->hdr, hdr, hdr->length);	/* [한국어] ACPI 버퍼가 곧 해제되므로 복사한다 */
	satcu->atc_required = satc->flags & 0x1;	/* [한국어] 이 장치들은 ATS 가 필수라는 표시. SoC 통합 장치 중 번역 캐시 없이는 동작하지 못하는 것들이 있다 */
	satcu->devices = dmar_alloc_dev_scope((void *)(satc + 1),	/* [한국어] 대상 장치 목록을 파싱한다 */
					      (void *)satc + satc->header.length,	/* [한국어] 항목의 끝까지 */
					      &satcu->devices_cnt);	/* [한국어] 개수를 받는다 */
	if (satcu->devices_cnt && !satcu->devices) {	/* [한국어] 파싱 실패 */
		kfree(satcu);	/* [한국어] 자료구조 반납 */
		return -ENOMEM;	/* [한국어] 등록 실패 */
	}
	list_add_rcu(&satcu->list, &dmar_satc_units);	/* [한국어] RCU 목록에 등록 */

	return 0;	/* [한국어] 등록 완료 */
}

/*
 * [한국어]
 * intel_iommu_add - 핫플러그로 새로 나타난 DRHD 유닛 하나를 동작 상태로 세운다
 *
 * @dmaru: 새로 추가된 DRHD(DMA Remapping Hardware unit Definition) 유닛.
 *         dmaru->iommu 는 이미 dmar_alloc_dev_scope 계열이 만들어 둔 상태다.
 * @return: 0 성공, 음수면 실패(그 경우 유닛은 정지되고 자료구조도 반납된다).
 *
 * 왜 필요한가: 부팅 때는 init_dmars() 가 모든 유닛을 한꺼번에 세우지만,
 * PCIe 호스트 브리지가 나중에 꽂히면 그 브리지에 딸린 VT-d 유닛도 그때
 * 초기화해야 한다. 이 함수가 init_dmars 의 유닛 1개짜리 버전이다.
 *
 * 순서가 중요한 이유 — 아래 단계는 하드웨어 요구 순서 그대로다.
 *   1) 펌웨어가 번역을 켜 둔 채 넘겨줬으면 먼저 끈다. 우리가 세운 적 없는
 *      루트 테이블이 살아 있는 상태에서 뭘 바꾸면 진행 중인 DMA 가 엉킨다.
 *   2) iommu_alloc_root_entry — 루트 테이블을 만든다.
 *   3) intel_svm_check — 이 유닛이 SVA(PASID/PRI)를 지원하는지 표시한다.
 *   4) dmaru->ignored 인 유닛(intel_iommu=off 대상 등)은 여기서 끝낸다.
 *      단 PMR(Protected Memory Region)만은 반드시 꺼야 한다. BIOS 가 켜 둔
 *      보호 영역이 남아 있으면 그 아래 장치의 DMA 가 통째로 막히기 때문이다.
 *   5) intel_iommu_init_qi — 큐 기반 무효화(QI)를 세운다. 이후의 모든 캐시
 *      무효화가 이 큐를 쓴다.
 *   6) PRS 지원 유닛이면 페이지 요청 큐(PRQ)를 세운다.
 *   7) dmar_set_interrupt — 폴트 인터럽트를 건다.
 *   8) iommu_set_root_entry → iommu_enable_translation — 루트 테이블 주소를
 *      하드웨어에 알리고 번역을 켠다. 반드시 이 순서다.
 *   9) 마지막으로 PMR 을 끈다.
 *
 * 실행 컨텍스트: ACPI 핫플러그 처리. 프로세스 컨텍스트.
 * 에러 처리: 6~7 단계에서 실패하면 disable_dmar_iommu 로 유닛을 정지시킨 뒤
 * free_dmar_iommu 로 자료구조를 반납한다. 2 단계 실패는 아직 켠 게 없으므로
 * out 으로 바로 가서 반납만 한다.
 *
 * 호출 체인:
 *   dmar_iommu_hotplug() → [intel_iommu_add]
 *     → iommu_alloc_root_entry() → intel_iommu_init_qi()
 *     → intel_iommu_enable_prq() → dmar_set_interrupt()
 *     → iommu_set_root_entry() → iommu_enable_translation()
 */
static int intel_iommu_add(struct dmar_drhd_unit *dmaru)
{
	struct intel_iommu *iommu = dmaru->iommu;	/* [한국어] 추가된 유닛 */
	int ret;	/* [한국어] 각 단계의 결과 */

	/*
	 * Disable translation if already enabled prior to OS handover.
	 */
	if (iommu->gcmd & DMA_GCMD_TE)	/* [한국어] 펌웨어가 켜 둔 상태로 넘어왔으면 (위 영어 주석) */
		iommu_disable_translation(iommu);	/* [한국어] 우리가 다시 세운다 */

	ret = iommu_alloc_root_entry(iommu);	/* [한국어] 루트 테이블을 만든다 */
	if (ret)	/* [한국어] 실패 */
		goto out;	/* [한국어] 정리하고 나간다 */

	intel_svm_check(iommu);	/* [한국어] SVA 지원 여부를 확인한다 */

	if (dmaru->ignored) {	/* [한국어] 무시하는 유닛이면 */
		/*
		 * we always have to disable PMRs or DMA may fail on this device
		 */
		if (force_on)	/* [한국어] 강제 모드에서는 */
			iommu_disable_protect_mem_regions(iommu);	/* [한국어] 보호 영역만 끈다 (위 영어 주석) */
		return 0;	/* [한국어] 나머지 설정은 하지 않는다 */
	}

	intel_iommu_init_qi(iommu);	/* [한국어] 무효화 큐를 세운다 */
	iommu_flush_write_buffer(iommu);	/* [한국어] 쓰기 버퍼 정리 */

	if (ecap_prs(iommu->ecap)) {	/* [한국어] PRI 를 지원하면 */
		ret = intel_iommu_enable_prq(iommu);	/* [한국어] 페이지 요청 큐를 세운다 */
		if (ret)	/* [한국어] 실패 */
			goto disable_iommu;	/* [한국어] 유닛을 정지시키고 정리 */
	}

	ret = dmar_set_interrupt(iommu);	/* [한국어] 폴트 인터럽트를 건다 */
	if (ret)	/* [한국어] 실패 */
		goto disable_iommu;	/* [한국어] 정리 */

	iommu_set_root_entry(iommu);	/* [한국어] 루트 테이블을 하드웨어에 알린다 */
	iommu_enable_translation(iommu);	/* [한국어] 번역을 켠다 */

	iommu_disable_protect_mem_regions(iommu);	/* [한국어] BIOS 보호 영역을 끈다 */
	return 0;	/* [한국어] 유닛이 동작을 시작했다 */

disable_iommu:	/* [한국어] 설정 도중 실패한 경로 */
	disable_dmar_iommu(iommu);	/* [한국어] 유닛을 정지 */
out:	/* [한국어] 루트 테이블 실패가 합류 */
	free_dmar_iommu(iommu);	/* [한국어] 자료구조 반납 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * dmar_iommu_hotplug - VT-d 유닛의 추가/제거 요청을 처리하는 진입점
 *
 * @dmaru: 대상 DRHD 유닛.
 * @insert: true 면 추가(세워서 켜기), false 면 제거(끄고 반납).
 * @return: 0 성공, -EINVAL 이면 유닛 구조체가 없다. 추가 실패 시 그 이유.
 *
 * 왜 필요한가: DMAR 표에 정의된 VT-d 유닛은 PCIe 호스트 브리지 단위로
 * 존재한다. 브리지가 핫플러그되면 유닛도 함께 나타나거나 사라지므로, DMAR
 * 코어(dmar.c)가 그 사실을 이 콜백으로 알려 준다. 즉 이 함수는 dmar.c 의
 * 일반 핫플러그 처리와 intel/iommu.c 의 실제 초기화 사이의 다리다.
 *
 * intel_iommu_enabled 가 꺼져 있으면 아무 일도 하지 않는다. 번역을 아예 쓰지
 * 않는 부팅에서는 유닛을 세울 필요도, 끌 필요도 없기 때문이다.
 *
 * 제거 경로가 두 단계인 이유: disable_dmar_iommu 는 번역을 끄고 이 유닛에
 * 매달린 도메인들을 떼어 내는 "동작 정지"이고, free_dmar_iommu 는 큐/테이블/
 * 비트맵 같은 메모리를 반납하는 "자원 회수"다. 순서를 바꾸면 아직 하드웨어가
 * 참조 중인 메모리를 해제하게 된다.
 *
 * 실행 컨텍스트: ACPI 핫플러그. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dmar_hotplug_insert()/dmar_hotplug_remove() → [dmar_iommu_hotplug]
 *     → intel_iommu_add() 또는 disable_dmar_iommu() + free_dmar_iommu()
 */
int dmar_iommu_hotplug(struct dmar_drhd_unit *dmaru, bool insert)
{
	int ret = 0;	/* [한국어] 결과 */
	struct intel_iommu *iommu = dmaru->iommu;	/* [한국어] 대상 유닛 */

	if (!intel_iommu_enabled)	/* [한국어] VT-d 가 꺼져 있으면 */
		return 0;	/* [한국어] 할 일 없음 */
	if (iommu == NULL)	/* [한국어] 유닛 구조체가 없다 */
		return -EINVAL;	/* [한국어] 잘못된 요청 */

	if (insert) {	/* [한국어] 유닛이 추가된 경우 */
		ret = intel_iommu_add(dmaru);	/* [한국어] 세우고 번역을 켠다 */
	} else {
		disable_dmar_iommu(iommu);	/* [한국어] 제거된 경우 — 정지시키고 */
		free_dmar_iommu(iommu);	/* [한국어] 자료구조를 반납한다 */
	}

	return ret;	/* [한국어] 결과 */
}

/*
 * [한국어]
 * intel_iommu_free_dmars - 파싱해 둔 RMRR/ATSR/SATC 항목을 전부 반납한다
 *
 * @return: 없음.
 *
 * 언제 불리는가: intel_iommu_init() 이 도중에 실패해서 되돌릴 때, 그리고
 * 모듈 정리 경로에서. 부팅이 성공하면 이 목록들은 시스템이 살아 있는 내내
 * 유지되므로 이 함수는 실질적으로 실패 경로 전용이다.
 *
 * 세 목록을 각각 비운다. 모두 list_for_each_entry_safe 를 쓰는데, 순회 중에
 * 현재 항목을 목록에서 빼고 해제하기 때문이다(다음 포인터를 미리 들고 있어야
 * 한다). 여기서는 RCU 목록이라도 list_del_rcu + synchronize_rcu 가 아니라
 * 평범한 list_del 을 쓰는데, 이 시점에는 동시에 순회하는 코드가 없기
 * 때문이다 — 초기화가 실패했거나 모듈이 내려가는 중이라 진입점이 모두 닫혀
 * 있다.
 *
 * 해제 방식이 항목마다 다른 이유: RMRR/SATC 는 device scope 배열을 따로
 * 잡았으므로 dmar_free_dev_scope 로 먼저 반납하고 본체를 kfree 한다.
 * ATSR 은 같은 일을 intel_iommu_free_atsr 헬퍼가 대신한다(제거 경로에서도
 * 같은 헬퍼를 쓰기 때문에 함수로 빼 두었다).
 *
 * 실행 컨텍스트: 부팅 실패 정리 또는 모듈 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_init() 실패 경로 → [intel_iommu_free_dmars]
 *     → dmar_free_dev_scope() / intel_iommu_free_atsr()
 */
static void intel_iommu_free_dmars(void)
{
	struct dmar_rmrr_unit *rmrru, *rmrr_n;	/* [한국어] 해제하며 순회 */
	struct dmar_atsr_unit *atsru, *atsr_n;	/* [한국어] 마찬가지 */
	struct dmar_satc_unit *satcu, *satc_n;	/* [한국어] 마찬가지 */

	list_for_each_entry_safe(rmrru, rmrr_n, &dmar_rmrr_units, list) {	/* [한국어] RMRR 항목들을 */
		list_del(&rmrru->list);	/* [한국어] 목록에서 빼고 */
		dmar_free_dev_scope(&rmrru->devices, &rmrru->devices_cnt);	/* [한국어] 장치 목록 해제 */
		kfree(rmrru);	/* [한국어] 자료구조 해제 */
	}

	list_for_each_entry_safe(atsru, atsr_n, &dmar_atsr_units, list) {	/* [한국어] ATSR 항목들 */
		list_del(&atsru->list);	/* [한국어] 목록에서 제거 */
		intel_iommu_free_atsr(atsru);	/* [한국어] 해제 */
	}
	list_for_each_entry_safe(satcu, satc_n, &dmar_satc_units, list) {	/* [한국어] SATC 항목들 */
		list_del(&satcu->list);	/* [한국어] 목록에서 제거 */
		dmar_free_dev_scope(&satcu->devices, &satcu->devices_cnt);	/* [한국어] 장치 목록 해제 */
		kfree(satcu);	/* [한국어] 자료구조 해제 */
	}
}

/*
 * [한국어]
 * dmar_find_matched_satc_unit - 이 PCI 장치를 담당하는 SATC 항목을 찾는다
 *
 * @dev: 찾을 PCI 장치(이미 물리 함수로 정규화된 상태로 들어온다).
 * @return: 이 장치를 지목한 dmar_satc_unit, 없으면 NULL.
 *
 * SATC 표는 "이 SoC 통합 장치들은 번역 캐시를 쓴다(또는 반드시 써야 한다)"는
 * 신고다. dmar_ats_supported 가 ATS 활성화 여부를 정하기 전에, 먼저 이 표에
 * 있는 장치인지 확인해야 한다. 표에 있으면 PCIe ATS 능력 구조를 뒤질 필요 없이
 * 결론이 나기 때문이다.
 *
 * 두 단계로 좁힌다: 먼저 PCI 세그먼트(도메인) 번호가 같은 항목만 남기고,
 * 그 항목이 지목한 device scope 배열에서 이 장치를 찾는다. 세그먼트를 먼저
 * 보는 것은 memcmp 나 포인터 비교보다 훨씬 싼 정수 비교이기 때문이다.
 *
 * 동기화: dmar_satc_units 는 RCU 목록이라 rcu_read_lock 안에서 순회한다.
 * 반환된 포인터를 RCU 밖에서 쓰는 것은, 이 목록의 제거 경로가
 * synchronize_rcu 를 거치고 그 제거가 장치 프로브와 같은 락 아래에서만
 * 일어나기 때문에 안전하다.
 * 실행 컨텍스트: 장치 프로브. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dmar_ats_supported() → [dmar_find_matched_satc_unit]
 */
static struct dmar_satc_unit *dmar_find_matched_satc_unit(struct pci_dev *dev)
{
	struct dmar_satc_unit *satcu;	/* [한국어] 순회 커서이자 반환값 */
	struct acpi_dmar_satc *satc;	/* [한국어] 그 항목의 원본 ACPI 자료 */
	struct device *tmp;	/* [한국어] device scope 순회 커서 */
	int i;	/* [한국어] device scope 인덱스 */

	rcu_read_lock();	/* [한국어] SATC 목록은 핫플러그로 바뀔 수 있다 — 순회 동안 항목이 해제되지 않도록 보호한다 */

	list_for_each_entry_rcu(satcu, &dmar_satc_units, list) {	/* [한국어] 등록된 SATC 항목을 훑는다 */
		satc = container_of(satcu->hdr, struct acpi_dmar_satc, header);	/* [한국어] 보관해 둔 ACPI 사본으로 */
		if (satc->segment != pci_domain_nr(dev->bus))	/* [한국어] PCI 세그먼트(도메인)가 다르면 애초에 다른 계층의 장치다 */
			continue;	/* [한국어] 다음 항목 */
		for_each_dev_scope(satcu->devices, satcu->devices_cnt, i, tmp)	/* [한국어] 이 항목이 지목한 장치들을 훑으며 */
			if (to_pci_dev(tmp) == dev)	/* [한국어] 찾는 장치가 그 안에 있으면 */
				goto out;	/* [한국어] 이 SATC 항목이 이 장치를 담당한다 */
	}
	satcu = NULL;	/* [한국어] 어느 항목에도 없다 — SATC 대상이 아니다 */
out:	/* [한국어] 찾았거나 못 찾았거나 여기서 합류 */
	rcu_read_unlock();	/* [한국어] 순회 끝 */
	return satcu;	/* [한국어] 담당 항목 또는 NULL. 반환된 포인터는 RCU 밖에서도 쓰이지만, 이 목록은 핫플러그 제거 시 synchronize_rcu 를 거치므로 호출 문맥에서는 살아 있다 */
}

/*
 * [한국어]
 * dmar_ats_supported - 이 장치에 ATS(장치 내부 번역 캐시)를 켜도 되는지 판단한다
 *
 * @dev: 대상 PCI 장치.
 * @iommu: 이 장치를 담당하는 DMAR 유닛.
 * @return: true 면 켜도 된다, false 면 켜지 말아야 한다.
 *
 * ATS 가 왜 조심스러운가: ATS 를 켜면 장치가 번역 결과를 자기 안에 캐시하고,
 * 그 다음부터는 이미 번역된 주소로 DMA 를 낸다. 즉 IOMMU 를 우회한다.
 * 그래서 (a) 하드웨어 경로가 ATS TLP 를 실제로 나를 수 있어야 하고,
 * (b) 커널이 언매핑 때 그 장치 캐시까지 무효화할 책임을 지게 된다.
 * 조건이 하나라도 어긋나면 켜지 않는 편이 안전하다.
 *
 * 판단 순서:
 *   1) VF 는 PF 의 설정을 따르므로 pci_physfn 으로 정규화한다.
 *   2) SATC 표에 있는 SoC 통합 장치인가? 있으면 ATS 는 지원된다. 단
 *      atc_required 이면서 레거시 모드라면 하드웨어가 스스로 ATS 를 켜므로,
 *      OS 까지 켜면 무효화가 중복으로 나간다 — 그래서 false 를 돌린다.
 *   3) 아니면 PCIe 계층을 루트 쪽으로 거슬러 올라간다.
 *      - 상위 브리지가 없으면 호스트에 직결된 통합 장치 → 허용.
 *      - PCIe 가 아니거나 PCIe-to-PCI 브리지를 지나야 하면 → 거부.
 *        ATS 요청/응답은 PCIe 전용 TLP 라 그 구간을 통과하지 못한다.
 *      - 루트 포트를 만나면 거기서 멈춘다.
 *   4) 그 루트 포트가 ATSR 표에 신고되어 있는지 본다. 명시적으로 지목되었거나
 *      include_all 항목이 있으면 허용, 아니면 거부.
 *
 * 동기화: ATSR/SATC 목록 순회는 rcu_read_lock 아래에서 한다.
 * 실행 컨텍스트: 장치 프로브. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_probe_device()/iommu_enable_pci_ats() → [dmar_ats_supported]
 *     → dmar_find_matched_satc_unit()
 */
static bool dmar_ats_supported(struct pci_dev *dev, struct intel_iommu *iommu)
{
	struct pci_dev *bridge = NULL;	/* [한국어] 장치 위로 거슬러 올라가며 찾은 루트 포트 */
	struct dmar_atsr_unit *atsru;	/* [한국어] ATSR 항목 순회 커서 */
	struct dmar_satc_unit *satcu;	/* [한국어] SATC 항목(있다면) */
	struct acpi_dmar_atsr *atsr;	/* [한국어] ATSR 원본 ACPI 자료 */
	bool supported = true;	/* [한국어] 기본값은 지원. 아래 루프에서 근거를 못 찾으면 false 로 뒤집는다 */
	struct pci_bus *bus;	/* [한국어] 버스 계층을 거슬러 올라가는 커서 */
	struct device *tmp;	/* [한국어] device scope 순회 커서 */
	int i;	/* [한국어] device scope 인덱스 */

	dev = pci_physfn(dev);	/* [한국어] VF 는 자기 PF 의 ATS 설정을 따른다 — 물리 함수로 바꿔 판단한다 */
	satcu = dmar_find_matched_satc_unit(dev);	/* [한국어] SoC 통합 장치로 신고된 것인지 먼저 본다 */
	if (satcu)	/* [한국어] SATC 표에 있는 장치라면 아래 영어 주석의 판단을 따른다 */
		/*
		 * This device supports ATS as it is in SATC table.
		 * When IOMMU is in legacy mode, enabling ATS is done
		 * automatically by HW for the device that requires
		 * ATS, hence OS should not enable this device ATS
		 * to avoid duplicated TLB invalidation.
		 */
		return !(satcu->atc_required && !sm_supported(iommu));	/* [한국어] SATC 에 있으면 ATS 는 지원된다. 다만 atc_required 인데 레거시 모드라면 하드웨어가 알아서 ATS 를 켜므로, OS 까지 켜면 무효화가 두 번 나간다 — 그래서 false 를 돌려 OS 활성화를 막는다 (위 영어 주석) */

	for (bus = dev->bus; bus; bus = bus->parent) {	/* [한국어] SATC 대상이 아니면 PCIe 계층을 루트 쪽으로 거슬러 올라간다 */
		bridge = bus->self;	/* [한국어] 이 버스를 만든 상위 브리지 */
		/* If it's an integrated device, allow ATS */
		if (!bridge)	/* [한국어] 브리지가 없다는 것은 호스트 브리지에 직결된 통합 장치라는 뜻 (위 영어 주석) */
			return true;	/* [한국어] ATSR 표를 볼 필요 없이 허용한다 */
		/* Connected via non-PCIe: no ATS */
		if (!pci_is_pcie(bridge) ||	/* [한국어] 중간에 PCIe 가 아닌 브리지가 끼어 있거나 */
		    pci_pcie_type(bridge) == PCI_EXP_TYPE_PCI_BRIDGE)	/* [한국어] PCIe-to-PCI 브리지를 지나야 한다면 */
			return false;	/* [한국어] ATS 요청/응답 TLP 가 그 구간을 통과하지 못한다 — 지원 불가 (위 영어 주석) */
		/* If we found the root port, look it up in the ATSR */
		if (pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT)	/* [한국어] 루트 포트에 닿았다 */
			break;	/* [한국어] 이 포트가 ATSR 표에 있는지 아래에서 확인한다 (위 영어 주석) */
	}

	rcu_read_lock();	/* [한국어] ATSR 목록 순회 보호 */
	list_for_each_entry_rcu(atsru, &dmar_atsr_units, list) {	/* [한국어] 등록된 ATSR 항목들을 훑는다 */
		atsr = container_of(atsru->hdr, struct acpi_dmar_atsr, header);	/* [한국어] 보관된 ACPI 사본으로 */
		if (atsr->segment != pci_domain_nr(dev->bus))	/* [한국어] 세그먼트가 다르면 무관한 항목 */
			continue;	/* [한국어] 다음 항목 */

		for_each_dev_scope(atsru->devices, atsru->devices_cnt, i, tmp)	/* [한국어] 이 항목이 지목한 포트들 중에 */
			if (tmp == &bridge->dev)	/* [한국어] 위에서 찾은 루트 포트가 있으면 */
				goto out;	/* [한국어] 지원 확정 */

		if (atsru->include_all)	/* [한국어] 또는 이 항목이 "모든 포트" 를 뜻하면 */
			goto out;	/* [한국어] 역시 지원 확정 */
	}
	supported = false;	/* [한국어] 어느 ATSR 도 이 루트 포트를 담지 않았다 — 펌웨어가 ATS 를 보고하지 않은 경로다 */
out:	/* [한국어] 두 결론이 합류 */
	rcu_read_unlock();	/* [한국어] 순회 끝 */

	return supported;	/* [한국어] true 면 이 장치에 ATS 를 켜도 된다. 호출자는 이 값으로 pci_enable_ats 여부를 정한다 */
}

/*
 * [한국어]
 * dmar_iommu_notify_scope_dev - PCI 장치 추가/제거를 RMRR·ATSR·SATC 표에 반영한다
 *
 * @info: 어떤 장치가 어떤 이벤트(BUS_NOTIFY_ADD_DEVICE /
 *        BUS_NOTIFY_REMOVED_DEVICE)를 냈는지 담은 알림 정보.
 * @return: 0 성공, 음수면 처리 실패(장치 추가 자체가 실패한다).
 *
 * 왜 필요한가: ACPI DMAR 표는 장치를 "세그먼트 s, 버스 b, 슬롯/함수 경로"
 * 형태로만 지목한다. 부팅 시점에 그 경로의 장치가 아직 없을 수도 있고,
 * 나중에 핫플러그로 나타날 수도 있다. 그래서 각 표 항목의 device scope 는
 * 처음엔 경로만 들고 있다가, 그 경로의 장치가 실제로 등장하면 이 콜백이
 * struct device 포인터를 채워 넣는다. 반대로 장치가 사라지면 포인터를 끊어
 * 해제된 메모리를 가리키지 않게 한다.
 *
 * 세 표를 각각 훑는데 처리가 조금씩 다르다.
 *   - RMRR: 한 장치가 여러 RMRR 에 속할 수 있으므로 끝까지 훑는다.
 *   - ATSR: include_all 항목은 장치 목록 자체가 없으니 건너뛴다. 한 장치는
 *     하나의 루트 포트 아래에만 있으므로 채워 넣는 데 성공하면 break.
 *   - SATC: include_all 개념이 없고, ATSR 과 같은 이유로 성공 시 break.
 *
 * 실행 컨텍스트: PCI 버스 알림 체인(dmar_register_bus_notifier 가 등록).
 * 프로세스 컨텍스트이며 dmar_global_lock 을 쥔 채 불린다 — 그래서 여기서는
 * RCU 순회가 아니라 평범한 list_for_each_entry 로 충분하다.
 * 에러 처리: 삽입이 실패하면(-ENOMEM 등) 그대로 전파해 장치 추가를 실패시킨다.
 * 표를 반쯤만 갱신한 상태로 두면 이후 판단이 틀어지기 때문이다.
 *
 * 호출 체인:
 *   PCI 버스 알림 → dmar_pci_bus_notifier() → [dmar_iommu_notify_scope_dev]
 *     → dmar_insert_dev_scope() / dmar_remove_dev_scope()
 */
int dmar_iommu_notify_scope_dev(struct dmar_pci_notify_info *info)
{
	int ret;	/* [한국어] 각 삽입 시도의 결과 */
	struct dmar_rmrr_unit *rmrru;	/* [한국어] RMRR 항목 순회 커서 */
	struct dmar_atsr_unit *atsru;	/* [한국어] ATSR 항목 순회 커서 */
	struct dmar_satc_unit *satcu;	/* [한국어] SATC 항목 순회 커서 */
	struct acpi_dmar_atsr *atsr;	/* [한국어] 원본 ACPI 자료 (device scope 배열의 위치를 계산하려면 필요하다) */
	struct acpi_dmar_reserved_memory *rmrr;	/* [한국어] 마찬가지 */
	struct acpi_dmar_satc *satc;	/* [한국어] 마찬가지 */

	if (!intel_iommu_enabled && system_state >= SYSTEM_RUNNING)	/* [한국어] 부팅이 끝났는데 VT-d 가 꺼져 있으면 */
		return 0;	/* [한국어] 이 표들을 갱신할 이유가 없다 */

	list_for_each_entry(rmrru, &dmar_rmrr_units, list) {	/* [한국어] 먼저 RMRR 항목들을 훑는다 */
		rmrr = container_of(rmrru->hdr,	/* [한국어] 보관된 ACPI 항목으로 */
				    struct acpi_dmar_reserved_memory, header);
		if (info->event == BUS_NOTIFY_ADD_DEVICE) {	/* [한국어] 장치가 새로 나타난 경우 */
			ret = dmar_insert_dev_scope(info, (void *)(rmrr + 1),	/* [한국어] 펌웨어가 경로로만 적어 둔 항목에 실제 struct device 를 채워 넣는다. ACPI 는 장치를 "버스 x 의 슬롯 y" 처럼 경로로 지목하므로, 그 경로의 장치가 실제로 등장한 지금에서야 포인터를 연결할 수 있다 */
				((void *)rmrr) + rmrr->header.length,
				rmrr->segment, rmrru->devices,
				rmrru->devices_cnt);
			if (ret < 0)	/* [한국어] 오류 */
				return ret;	/* [한국어] 알림 처리 실패 */
		} else if (info->event == BUS_NOTIFY_REMOVED_DEVICE) {	/* [한국어] 장치가 사라진 경우 */
			dmar_remove_dev_scope(info, rmrr->segment,	/* [한국어] 연결해 둔 포인터를 끊는다. 그러지 않으면 해제된 struct device 를 가리키게 된다 */
				rmrru->devices, rmrru->devices_cnt);
		}
	}

	list_for_each_entry(atsru, &dmar_atsr_units, list) {	/* [한국어] 다음은 ATSR 항목들 */
		if (atsru->include_all)	/* [한국어] "모든 포트" 항목은 장치 목록 자체가 없으므로 */
			continue;	/* [한국어] 건너뛴다 */

		atsr = container_of(atsru->hdr, struct acpi_dmar_atsr, header);	/* [한국어] 보관된 ACPI 항목으로 */
		if (info->event == BUS_NOTIFY_ADD_DEVICE) {	/* [한국어] 장치 추가 */
			ret = dmar_insert_dev_scope(info, (void *)(atsr + 1),	/* [한국어] 경로에 맞는 자리에 포인터를 채운다 */
					(void *)atsr + atsr->header.length,
					atsr->segment, atsru->devices,
					atsru->devices_cnt);
			if (ret > 0)	/* [한국어] 채워 넣었다 — 한 장치는 한 항목에만 속하므로 */
				break;	/* [한국어] 더 볼 필요 없다 */
			else if (ret < 0)	/* [한국어] 오류 */
				return ret;	/* [한국어] 실패를 전파 */
		} else if (info->event == BUS_NOTIFY_REMOVED_DEVICE) {	/* [한국어] 장치 제거 */
			if (dmar_remove_dev_scope(info, atsr->segment,	/* [한국어] 끊었으면 */
					atsru->devices, atsru->devices_cnt))
				break;	/* [한국어] 마찬가지로 더 볼 필요 없다 */
		}
	}
	list_for_each_entry(satcu, &dmar_satc_units, list) {	/* [한국어] 마지막으로 SATC 항목들 — 여기는 include_all 개념이 없어 항상 목록이 있다 */
		satc = container_of(satcu->hdr, struct acpi_dmar_satc, header);	/* [한국어] 보관된 ACPI 항목으로 */
		if (info->event == BUS_NOTIFY_ADD_DEVICE) {	/* [한국어] 장치 추가 */
			ret = dmar_insert_dev_scope(info, (void *)(satc + 1),	/* [한국어] 포인터를 채운다 */
					(void *)satc + satc->header.length,
					satc->segment, satcu->devices,
					satcu->devices_cnt);
			if (ret > 0)	/* [한국어] 채웠으면 */
				break;	/* [한국어] 종료 */
			else if (ret < 0)	/* [한국어] 오류 */
				return ret;	/* [한국어] 전파 */
		} else if (info->event == BUS_NOTIFY_REMOVED_DEVICE) {	/* [한국어] 장치 제거 */
			if (dmar_remove_dev_scope(info, satc->segment,	/* [한국어] 끊었으면 */
					satcu->devices, satcu->devices_cnt))
				break;	/* [한국어] 종료 */
		}
	}

	return 0;	/* [한국어] 세 표 모두 갱신 완료 */
}

/*
 * [한국어]
 * intel_disable_iommus - 등록된 모든 VT-d 유닛의 번역을 끈다
 *
 * @return: 없음.
 *
 * VT-d 를 쓰지 않기로 결론이 난 부팅 경로에서 불린다. 특히 kexec 로 들어온
 * 커널이 중요하다 — 이전 커널이 켜 둔 번역이 그대로 살아 있는데 새 커널은
 * 그 테이블을 모른다. 그 상태로 두면 장치 DMA 가 정체 불명의 물리 주소로
 * 향하므로, 쓰지 않을 거라면 확실히 꺼야 한다.
 *
 * 번역을 끄면 그 뒤의 DMA 는 IOVA 를 물리 주소로 그대로 해석해 통과한다.
 * 즉 격리가 사라지는 되돌릴 수 없는 조치이므로, "VT-d 를 안 쓴다"가 확정된
 * 뒤에만 불러야 한다.
 *
 * 실행 컨텍스트: 부팅 초기화. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_init() (no_iommu/dmar_disabled 경로) → [intel_disable_iommus]
 *     → iommu_disable_translation()
 */
static void intel_disable_iommus(void)
{
	struct intel_iommu *iommu = NULL;	/* [한국어] for_each_iommu 가 갱신할 커서 */
	struct dmar_drhd_unit *drhd;	/* [한국어] 그 유닛이 속한 DRHD 항목 */

	for_each_iommu(iommu, drhd)	/* [한국어] 등록된 모든 DMAR 유닛에 대해 */
		iommu_disable_translation(iommu);	/* [한국어] 번역을 끈다. 이 뒤로 DMA 는 번역 없이 통과하므로, 되돌릴 수 없는 시점에서만 불러야 한다 */
}

/*
 * [한국어]
 * intel_iommu_shutdown - 시스템 종료/kexec 직전에 모든 유닛을 정지시킨다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 다음에 실행될 것(kexec 커널, 또는 재부팅 후 펌웨어)은 우리가
 * 세운 페이지 테이블을 모른다. 번역을 켠 채로 넘기면 그쪽 코드가 아직 자기
 * 테이블을 세우기 전에 장치가 우리 테이블을 통해 DMA 를 계속하게 된다.
 * PMR(Protected Memory Region)도 마찬가지로, 켜 둔 채 넘기면 다음 커널의
 * 정상적인 DMA 가 이유 없이 막힌다.
 *
 * 실행 컨텍스트가 특별하다: 이 시점에는 다른 CPU 가 모두 내려갔고 핫플러그
 * 인터럽트도 꺼져 있다. 그래서 위 영어 주석대로 락도 RCU 검사도 없이 목록을
 * 그냥 순회한다 — 목록을 바꿀 주체가 존재하지 않는다.
 *
 * 순서: 먼저 PMR 을 끄고 그 다음 번역을 끈다. 반대로 하면 번역이 꺼진
 * 짧은 구간 동안 PMR 만 살아 있어 DMA 가 막히는 창이 생긴다.
 *
 * 호출 체인:
 *   kernel_shutdown / machine_kexec 경로 → [intel_iommu_shutdown]
 *     → iommu_disable_protect_mem_regions() → iommu_disable_translation()
 */
void intel_iommu_shutdown(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] DRHD 항목 순회 커서 */
	struct intel_iommu *iommu = NULL;	/* [한국어] 그 항목이 가리키는 유닛 */

	if (no_iommu || dmar_disabled)	/* [한국어] 애초에 켠 적이 없으면 */
		return;	/* [한국어] 끌 것도 없다 */

	/*
	 * All other CPUs were brought down, hotplug interrupts were disabled,
	 * no lock and RCU checking needed anymore
	 */
	list_for_each_entry(drhd, &dmar_drhd_units, list) {	/* [한국어] RCU 없이 그냥 순회한다 — 이 시점에는 다른 CPU 가 모두 내려갔고 핫플러그 인터럽트도 꺼져 있어 목록이 바뀌지 않는다 (위 영어 주석) */
		iommu = drhd->iommu;	/* [한국어] 이 항목의 유닛 */

		/* Disable PMRs explicitly here. */
		iommu_disable_protect_mem_regions(iommu);	/* [한국어] BIOS 보호 영역을 명시적으로 끈다. 다음 커널(kexec)이나 펌웨어가 이 영역에 막혀 DMA 를 못 하는 일을 막는다 (위 영어 주석) */

		/* Make sure the IOMMUs are switched off */
		iommu_disable_translation(iommu);	/* [한국어] 번역을 끈다. 종료/kexec 직전이라 새 커널이 깨끗한 상태를 넘겨받는다 (위 영어 주석) */
	}
}

/*
 * [한국어]
 * dev_to_intel_iommu - sysfs 디바이스에서 VT-d 유닛 구조체로 되돌린다
 *
 * @dev: /sys/class/iommu/dmarN 에 대응하는 struct device.
 * @return: 그 노드를 만든 struct intel_iommu.
 *
 * IOMMU 코어는 유닛마다 struct iommu_device 를 sysfs 에 등록한다. 그것이
 * struct intel_iommu 안에 iommu 필드로 박혀 있으므로, container_of 로
 * 감싸는 구조체를 되찾으면 된다. 아래 sysfs 속성 함수들이 유닛 레지스터를
 * 읽으려면 매번 이 변환이 필요해 헬퍼로 뺐다.
 *
 * 실행 컨텍스트: sysfs 읽기(유저스페이스가 파일을 읽을 때). 프로세스
 * 컨텍스트이며, 노드가 살아 있는 동안 유닛도 살아 있음이 보장된다.
 *
 * 호출 체인:
 *   version_show()/address_show()/cap_show()/... → [dev_to_intel_iommu]
 */
static struct intel_iommu *dev_to_intel_iommu(struct device *dev)
{
	struct iommu_device *iommu_dev = dev_to_iommu_device(dev);	/* [한국어] sysfs 로 노출된 iommu 디바이스에서 코어 구조체를 꺼낸다 */

	return container_of(iommu_dev, struct intel_iommu, iommu);	/* [한국어] 그것을 품고 있는 VT-d 유닛으로 되돌린다. sysfs 속성 함수들이 유닛 레지스터를 읽으려면 이 변환이 필요하다 */
}

/*
 * [한국어]
 * version_show - /sys/.../intel-iommu/version 읽기 핸들러
 *
 * @dev: sysfs 디바이스. @attr: 어떤 속성인지(여기서는 안 쓴다).
 * @buf: 출력 버퍼(PAGE_SIZE). @return: 쓴 바이트 수.
 *
 * DMAR_VER_REG 를 그대로 읽어 major:minor 로 보여 준다. 값을 캐시해 두지 않는
 * 이유는, sysfs 읽기가 드물고 하드웨어를 직접 읽는 편이 진단에 더 정확하기
 * 때문이다. 아래 속성들도 같은 방식이다.
 *
 * 실행 컨텍스트: 유저스페이스의 read(2). 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   sysfs read → [version_show] → dev_to_intel_iommu() → readl()
 */
static ssize_t version_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);	/* [한국어] sysfs 노드에서 유닛으로 */
	u32 ver = readl(iommu->reg + DMAR_VER_REG);	/* [한국어] 버전 레지스터를 직접 읽는다. 캐시해 두지 않는 이유는 sysfs 읽기가 드물고, 하드웨어 값을 그대로 보여 주는 편이 진단에 정확하기 때문이다 */
	return sysfs_emit(buf, "%d:%d\n",	/* [한국어] major:minor 형식으로 출력. sysfs_emit 은 PAGE_SIZE 경계를 안전하게 다루는 표준 헬퍼다 */
			  DMAR_VER_MAJOR(ver), DMAR_VER_MINOR(ver));	/* [한국어] 레지스터 값에서 두 필드를 뽑는다 */
}
static DEVICE_ATTR_RO(version);	/* [한국어] 읽기 전용 sysfs 속성으로 등록 */

/*
 * [한국어]
 * address_show - 이 유닛의 레지스터가 놓인 물리 주소를 보여 준다
 *
 * @dev/@attr/@buf/@return: version_show 와 같은 sysfs 규약.
 *
 * ACPI DMAR 표에는 각 DRHD 항목의 레지스터 기저 주소가 적혀 있다. 그 값과
 * 여기 출력을 대조하면 /sys/class/iommu/dmarN 이 표의 몇 번째 항목인지
 * 사람이 확인할 수 있다. 유닛이 여럿인 시스템에서 진단의 출발점이 된다.
 *
 * 호출 체인:
 *   sysfs read → [address_show] → dev_to_intel_iommu()
 */
static ssize_t address_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);	/* [한국어] 유닛으로 */
	return sysfs_emit(buf, "%llx\n", iommu->reg_phys);	/* [한국어] 이 유닛의 레지스터가 놓인 물리 주소. DMAR 표의 어느 항목인지 사람이 대조할 때 쓴다 */
}
static DEVICE_ATTR_RO(address);	/* [한국어] 읽기 전용 속성 */

/*
 * [한국어]
 * cap_show - 능력 레지스터(CAP)의 원본 값을 16진수로 보여 준다
 *
 * @dev/@attr/@buf/@return: sysfs 규약.
 *
 * CAP 레지스터는 여러 비트필드를 한 값에 담고 있다: 지원 주소 폭(sagaw),
 * 동시 도메인 개수(ndoms), 큰 페이지 지원(fl1gp/sllps), caching mode,
 * write-buffer flush 필요 여부 등. 커널이 이 값들을 어떻게 해석했는지
 * 의심스러울 때 원본을 직접 보려고 노출한다.
 *
 * 호출 체인:
 *   sysfs read → [cap_show] → dev_to_intel_iommu()
 */
static ssize_t cap_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);	/* [한국어] 유닛으로 */
	return sysfs_emit(buf, "%llx\n", iommu->cap);	/* [한국어] 능력 레지스터 원본값. 지원 주소 폭, 도메인 개수, 큰 페이지 지원 여부 등이 모두 이 안의 비트필드다 */
}
static DEVICE_ATTR_RO(cap);	/* [한국어] 읽기 전용 속성 */

/*
 * [한국어]
 * ecap_show - 확장 능력 레지스터(ECAP)의 원본 값을 보여 준다
 *
 * @dev/@attr/@buf/@return: sysfs 규약.
 *
 * ECAP 에는 나중에 추가된 기능들이 모여 있다: PASID 지원, 페이지 요청(PRI),
 * scalable mode, 코히런시(coherent), 디바이스 TLB, 중첩 변환 등. SVA 나
 * nested 가 왜 동작하지 않는지 볼 때 가장 먼저 확인하는 값이다.
 *
 * 호출 체인:
 *   sysfs read → [ecap_show] → dev_to_intel_iommu()
 */
static ssize_t ecap_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);	/* [한국어] 유닛으로 */
	return sysfs_emit(buf, "%llx\n", iommu->ecap);	/* [한국어] 확장 능력 레지스터. PASID, PRI, scalable mode, 코히런시 지원 여부가 여기 있다 */
}
static DEVICE_ATTR_RO(ecap);	/* [한국어] 읽기 전용 속성 */

/*
 * [한국어]
 * domains_supported_show - 이 유닛이 동시에 유지할 수 있는 도메인 수를 보여 준다
 *
 * @dev/@attr/@buf/@return: sysfs 규약.
 *
 * VT-d 는 컨텍스트/PASID 항목에 도메인 id 를 적어 두고, 그 id 로 IOTLB 를
 * 구분한다. 그 필드의 비트 폭이 곧 동시 도메인 수의 상한이며
 * cap_ndoms(cap) 이 그것을 계산한다(보통 65536, 오래된 하드웨어는 훨씬 적다).
 * 유닛마다 다를 수 있어 유닛 단위 속성으로 노출한다.
 *
 * 호출 체인:
 *   sysfs read → [domains_supported_show] → cap_ndoms()
 */
static ssize_t domains_supported_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);	/* [한국어] 유닛으로 */
	return sysfs_emit(buf, "%ld\n", cap_ndoms(iommu->cap));	/* [한국어] 이 유닛이 동시에 유지할 수 있는 도메인 개수. 도메인 id 필드의 비트 폭에서 나오며, 이 수를 넘으면 새 도메인을 붙일 수 없다 */
}
static DEVICE_ATTR_RO(domains_supported);	/* [한국어] 읽기 전용 속성 */

/*
 * [한국어]
 * domains_used_show - 현재 이 유닛에서 쓰이고 있는 도메인 수를 센다
 *
 * @dev/@attr/@buf/@return: sysfs 규약.
 *
 * 유닛의 domain_ida 에서 실제로 할당된 id 개수를 세어 보여 준다. ida 는
 * 개수를 따로 들고 있지 않으므로 0..ndoms-1 를 훑으며 ida_exists 로 확인한다.
 * ndoms 가 큰 하드웨어에서는 이 순회가 길 수 있지만, sysfs 읽기는 사람이
 * 가끔 하는 동작이라 문제가 되지 않는다.
 *
 * 왜 보고 싶은가: domains_supported 와 비교하면 도메인 id 고갈이 가까운지
 * 알 수 있다. 컨테이너나 VFIO 를 많이 쓰는 시스템에서 장치를 도메인에 붙일
 * 수 없게 되는 원인이 대개 이 값이다.
 *
 * 호출 체인:
 *   sysfs read → [domains_used_show] → ida_exists()
 */
static ssize_t domains_used_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct intel_iommu *iommu = dev_to_intel_iommu(dev);	/* [한국어] 유닛으로 */
	unsigned int count = 0;	/* [한국어] 사용 중인 도메인 수 */
	int id;	/* [한국어] 도메인 id 순회 */

	for (id = 0; id < cap_ndoms(iommu->cap); id++)	/* [한국어] 가능한 id 를 전부 훑으며 */
		if (ida_exists(&iommu->domain_ida, id))	/* [한국어] ida 에 할당되어 있는지 확인한다 */
			count++;	/* [한국어] 쓰이는 중 */

	return sysfs_emit(buf, "%d\n", count);	/* [한국어] 몇 개가 쓰이는지. domains_supported 와 함께 보면 도메인 id 고갈이 임박했는지 알 수 있다 */
}
static DEVICE_ATTR_RO(domains_used);	/* [한국어] 읽기 전용 속성 */

static struct attribute *intel_iommu_attrs[] = {	/* [한국어] 위 속성들을 한 그룹으로 묶는다 */
	&dev_attr_version.attr,	/* [한국어] 버전 */
	&dev_attr_address.attr,	/* [한국어] 레지스터 물리 주소 */
	&dev_attr_cap.attr,	/* [한국어] 능력 레지스터 */
	&dev_attr_ecap.attr,	/* [한국어] 확장 능력 레지스터 */
	&dev_attr_domains_supported.attr,	/* [한국어] 지원 도메인 수 */
	&dev_attr_domains_used.attr,	/* [한국어] 사용 중 도메인 수 */
	NULL,	/* [한국어] 배열의 끝 표시 */
};

static struct attribute_group intel_iommu_group = {	/* [한국어] sysfs 하위 디렉터리 하나로 묶는다 */
	.name = "intel-iommu",	/* [한국어] /sys/class/iommu/dmarN/intel-iommu/ 아래에 나타난다 */
	.attrs = intel_iommu_attrs,	/* [한국어] 그 안의 파일들 */
};

const struct attribute_group *intel_iommu_groups[] = {	/* [한국어] iommu 코어에 넘길 그룹 목록 */
	&intel_iommu_group,	/* [한국어] 위에서 만든 그룹 하나뿐 */
	NULL,	/* [한국어] 끝 표시 */
};

/*
 * [한국어]
 * has_external_pci - 외부 포트에 연결된 PCI 장치가 하나라도 있는지 본다
 *
 * @return: true 면 외부 장치가 존재한다.
 *
 * external_facing 은 Thunderbolt/USB4 처럼 사용자가 물리적으로 장치를 꽂을 수
 * 있는 포트 뒤에 있다는 표시다. 그런 장치는 신뢰할 수 없고 DMA 공격의 통로가
 * 되므로, 펌웨어가 IOMMU 사용을 권장했다면(platform opt-in) 관리자 설정을
 * 뒤집어서라도 켤 근거가 된다.
 *
 * for_each_pci_dev 는 순회하며 참조를 잡았다 놓았다 한다. 그래서 루프를
 * 중간에 빠져나갈 때는 pci_dev_put 으로 직접 참조를 놓아야 한다 — 이 함수의
 * true 반환 경로가 정확히 그 경우다.
 *
 * 실행 컨텍스트: 부팅 초기화(__init 경로에서만 불린다). 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   platform_optin_force_iommu() → [has_external_pci]
 */
static bool has_external_pci(void)
{
	struct pci_dev *pdev = NULL;	/* [한국어] for_each_pci_dev 가 갱신할 커서. NULL 로 시작해야 처음부터 훑는다 */

	for_each_pci_dev(pdev)	/* [한국어] 시스템의 모든 PCI 장치를 훑는다. 이 매크로는 참조를 잡은 채 넘겨주고 다음 반복에서 놓아 준다 */
		if (pdev->external_facing) {	/* [한국어] Thunderbolt 같은 외부 포트 뒤에 있다고 표시된 장치라면 */
			pci_dev_put(pdev);	/* [한국어] 루프를 중간에 빠져나가므로 잡힌 참조를 직접 놓는다 */
			return true;	/* [한국어] 외부 장치가 하나라도 있다 — DMA 공격 표면이 존재한다 */
		}

	return false;	/* [한국어] 전부 내부 장치뿐이다 */
}

/*
 * [한국어]
 * platform_optin_force_iommu - 펌웨어가 권장하고 외부 장치가 있으면 VT-d 를 강제로 켠다
 *
 * @return: 1 이면 강제로 켰다(호출자는 이를 force_on 으로 삼는다), 0 이면 그대로 둔다.
 *
 * 세 조건이 모두 맞아야 개입한다.
 *   1) dmar_platform_optin() — 펌웨어가 DMAR 표에서 "이 플랫폼은 IOMMU 를
 *      켜는 것을 전제로 설계되었다"고 표시했다.
 *   2) !no_platform_optin — 관리자가 intel_iommu=off 등으로 그 권장을
 *      명시적으로 거부하지 않았다.
 *   3) has_external_pci() — 실제로 외부 포트 뒤의 장치가 있다. 위협이 없으면
 *      관리자 설정을 뒤집을 이유도 없다.
 *
 * iommu_set_default_passthrough(false) 를 부르는 이유: 기본값이 꺼짐이었다면
 * 도메인 정책도 passthrough 쪽에 맞춰져 있다. 켜기로 한 이상 기본 도메인을
 * 번역으로 돌려놔야 격리가 실제로 동작한다(위 영어 주석). 그 위에서 신뢰할 수
 * 없다고 표시된 장치만 엄격히 다루는 정책이 얹힌다.
 *
 * 반환값 1 의 무게: 호출자가 이 값으로 force_on 을 세우면 이후 초기화 실패가
 * 경고가 아니라 panic 이 된다. 켜기로 약속한 격리를 제공하지 못하는 채로
 * 부팅을 진행하지 않겠다는 뜻이다.
 *
 * 실행 컨텍스트: __init. 부팅 초기, 단일 스레드.
 *
 * 호출 체인:
 *   intel_iommu_init() → [platform_optin_force_iommu] → has_external_pci()
 */
static int __init platform_optin_force_iommu(void)
{
	if (!dmar_platform_optin() || no_platform_optin || !has_external_pci())	/* [한국어] 펌웨어가 IOMMU 사용을 권장하지 않았거나, 관리자가 그 권장을 껐거나, 외부 장치가 없으면 */
		return 0;	/* [한국어] 강제로 켤 이유가 없다 */

	if (no_iommu || dmar_disabled)	/* [한국어] 원래는 끄기로 되어 있었는데 뒤집는 상황이면 */
		pr_info("Intel-IOMMU force enabled due to platform opt in\n");	/* [한국어] 왜 켜졌는지 로그에 남긴다 — 관리자가 부트 인자와 다른 동작을 보고 혼란스럽지 않도록 */

	/*
	 * If Intel-IOMMU is disabled by default, we will apply identity
	 * map for all devices except those marked as being untrusted.
	 */
	if (dmar_disabled)	/* [한국어] 기본이 꺼짐이었다면 */
		iommu_set_default_passthrough(false);	/* [한국어] 기본 도메인을 passthrough 가 아니라 번역으로 둔다. 위 영어 주석대로 신뢰할 수 없다고 표시된 장치만 실제 격리 대상이 되지만, 기본값을 번역으로 두어야 그 판단이 의미를 갖는다 */

	dmar_disabled = 0;	/* [한국어] VT-d 를 켠다 */
	no_iommu = 0;	/* [한국어] 유닛 초기화도 진행한다 */

	return 1;	/* [한국어] 강제로 켰음을 호출자에게 알린다 — force_on 이 되어 실패가 panic 으로 이어진다 */
}

/*
 * [한국어]
 * probe_acpi_namespace_devices - ACPI 네임스페이스 장치들을 IOMMU 에 프로브시킨다
 *
 * @return: 0 성공, 음수면 어떤 장치의 프로브가 실패했다.
 *
 * 왜 따로 필요한가: PCI 장치는 버스 알림을 통해 자동으로 프로브된다. 하지만
 * DMAR 표의 device scope 에는 PCI 가 아닌 ACPI 네임스페이스 장치(ANDD 로
 * 신고되는 SoC 내부 장치들)도 들어갈 수 있고, 그쪽은 PCI 버스 알림을 타지
 * 않는다. 그래서 초기화 마지막에 표를 훑으며 직접 프로브를 걸어 준다.
 *
 * 한 ACPI 장치가 여러 물리 장치에 대응할 수 있어서(physical_node_list),
 * 그 목록을 순회하며 각각 iommu_probe_device 를 부른다.
 *
 * 락 다루기가 까다롭다: iommu_probe_device 는 내부에서 다시
 * dmar_global_lock 을 잡을 수 있다. 그래서 프로브를 부르기 직전에
 * up_read 로 놓고, 끝난 뒤 down_read 로 다시 잡는다. 이 놓았다 잡는 구간
 * 동안 목록이 바뀔 수 있지만, 부팅 중이라 실제로 그럴 주체가 없다.
 * adev->physical_node_lock 은 그 ACPI 장치의 물리 노드 목록을 보호한다.
 *
 * 실행 컨텍스트: 부팅 초기화, dmar_global_lock 읽기 락을 쥔 채 진입.
 * 프로세스 컨텍스트(mutex 를 잡으므로 잠들 수 있어야 한다).
 * 에러 처리: 호출자는 실패해도 부팅을 계속하고 경고만 남긴다 — 그 장치들만
 * IOMMU 밖에 남을 뿐 시스템은 동작한다.
 *
 * 호출 체인:
 *   intel_iommu_init() → [probe_acpi_namespace_devices]
 *     → iommu_probe_device() → intel_iommu_probe_device()
 */
static int __init probe_acpi_namespace_devices(void)
{
	struct dmar_drhd_unit *drhd;	/* [한국어] DRHD 순회 커서 */
	/* To avoid a -Wunused-but-set-variable warning. */
	struct intel_iommu *iommu __maybe_unused;	/* [한국어] for_each_active_iommu 매크로가 요구하지만 본문에서 안 쓴다. __maybe_unused 로 경고를 막는다 (위 영어 주석) */
	struct device *dev;	/* [한국어] device scope 순회 커서 */
	int i, ret = 0;	/* [한국어] 인덱스와 결과 */

	for_each_active_iommu(iommu, drhd) {	/* [한국어] 동작 중인 유닛마다 */
		for_each_active_dev_scope(drhd->devices,	/* [한국어] 그 유닛이 담당한다고 신고된 장치들을 훑는다 */
					  drhd->devices_cnt, i, dev) {
			struct acpi_device_physical_node *pn;	/* [한국어] ACPI 장치에 연결된 실제 물리 장치 노드 */
			struct acpi_device *adev;	/* [한국어] ACPI 쪽 장치 객체 */

			if (dev->bus != &acpi_bus_type)	/* [한국어] PCI 장치는 PCI 버스 알림으로 이미 프로브된다 — 여기서는 ACPI 네임스페이스 장치만 다룬다 */
				continue;	/* [한국어] 다음 장치 */

			up_read(&dmar_global_lock);	/* [한국어] 프로브가 다시 이 락을 잡을 수 있으므로 미리 놓는다. 락 순서 역전을 피하기 위한 조치다 */
			adev = to_acpi_device(dev);	/* [한국어] ACPI 장치로 변환 */
			mutex_lock(&adev->physical_node_lock);	/* [한국어] 이 ACPI 장치에 매달린 물리 장치 목록을 보호한다 */
			list_for_each_entry(pn,	/* [한국어] 하나의 ACPI 장치가 여러 물리 장치에 대응할 수 있다 */
					    &adev->physical_node_list, node) {
				ret = iommu_probe_device(pn->dev);	/* [한국어] 그 물리 장치를 IOMMU 코어에 프로브시킨다. 이때 intel_iommu_probe_device 가 불려 장치가 도메인에 붙는다 */
				if (ret)	/* [한국어] 하나라도 실패하면 */
					break;	/* [한국어] 더 진행하지 않는다 */
			}
			mutex_unlock(&adev->physical_node_lock);	/* [한국어] 목록 보호 해제 */
			down_read(&dmar_global_lock);	/* [한국어] 다시 잡는다 — 바깥 순회가 이 락을 전제로 한다 */

			if (ret)	/* [한국어] 프로브 실패였으면 */
				return ret;	/* [한국어] 초기화 전체를 실패시킨다 */
		}
	}

	return 0;	/* [한국어] ACPI 네임스페이스 장치를 모두 프로브했다 */
}

/*
 * [한국어]
 * tboot_force_iommu - TXT(측정 부팅)로 올라왔으면 VT-d 를 강제로 켠다
 *
 * @return: 1 이면 강제로 켰다, 0 이면 TXT 부팅이 아니라 아무것도 하지 않았다.
 *
 * TXT(Trusted Execution Technology)는 부팅 과정을 측정해 무결성을 보장하는
 * 구조다. 그런데 DMA 격리가 없으면 그 측정이 무의미해진다 — 어떤 장치든
 * 측정이 끝난 뒤 메모리를 마음대로 고쳐 쓸 수 있기 때문이다. 그래서 tboot 로
 * 부팅한 시스템에서는 관리자가 intel_iommu=off 를 줬더라도 무시하고 켠다.
 *
 * 이 강제를 다시 끄고 싶으면 intel_iommu_tboot_noforce 부트 인자를 준다.
 * 호출자(intel_iommu_init)가 그 조건을 먼저 확인한 뒤 이 함수를 부른다.
 *
 * 반환값 1 은 force_on 으로 이어져 이후 초기화 실패가 panic 이 된다.
 * 측정 부팅을 했는데 격리를 못 세운 상태로 계속 가는 것은 허용되지 않는다.
 *
 * 실행 컨텍스트: __init. 부팅 초기.
 *
 * 호출 체인:
 *   intel_iommu_init() → [tboot_force_iommu] → tboot_enabled()
 */
static __init int tboot_force_iommu(void)
{
	if (!tboot_enabled())	/* [한국어] TXT(Trusted Execution Technology)로 부팅한 것이 아니면 */
		return 0;	/* [한국어] 강제할 이유가 없다 */

	if (no_iommu || dmar_disabled)	/* [한국어] 끄기로 되어 있었다면 */
		pr_warn("Forcing Intel-IOMMU to enabled\n");	/* [한국어] 경고로 남긴다. TXT 로 측정 부팅을 해 놓고 DMA 격리를 끄면 그 측정이 무의미해지므로, 관리자 설정을 무시하고서라도 켠다 */

	dmar_disabled = 0;	/* [한국어] VT-d 를 켠다 */
	no_iommu = 0;	/* [한국어] 유닛 초기화도 진행 */

	return 1;	/* [한국어] 강제로 켰음을 알린다 */
}

/*
 * [한국어]
 * intel_iommu_init - VT-d 서브시스템 전체의 초기화 진입점
 *
 * @return: 0 이면 VT-d 가 동작을 시작했다. -ENODEV 면 쓰지 않기로 했거나
 *          하드웨어가 없다. force_on 인 경우 실패는 panic 으로 끝난다.
 *
 * 이 파일에서 가장 중요한 함수이며, 부팅 시 VT-d 가 켜지는 전 과정이 여기
 * 순서대로 들어 있다. 큰 흐름은 다음과 같다.
 *
 *   [1] 강제 여부 결정 — TXT 부팅이거나 펌웨어가 권장했으면 force_on.
 *       이후의 모든 실패가 panic 이 될지 조용한 포기가 될지가 여기서 갈린다.
 *   [2] ACPI DMAR 표 파싱(dmar_table_init) — DRHD/RMRR/ATSR/SATC 항목이
 *       전역 목록에 등록된다.
 *   [3] device scope 연결(dmar_dev_scope_init) — 표의 장치 경로를 실제
 *       struct device 로 잇는다.
 *   [4] 버스 알림 등록 — 이후 나타나는 장치가 자동으로 표에 연결된다.
 *   [5] "쓰지 않기로 한 경우"의 탈출구 — 여기서 나가더라도 PMR 은 끄고,
 *       kexec 로 물려받았을 수 있는 번역도 확실히 꺼 둔다.
 *   [6] init_dmars() — 핵심. 유닛마다 루트 테이블, 무효화 큐, 폴트
 *       인터럽트를 세운다. kdump 라면 이전 커널의 테이블을 인계받는다.
 *   [7] PM 콜백 등록, 유닛별 sysfs/perf 등록, IOMMU 코어 등록.
 *       iommu_device_register 시점부터 코어가 장치 프로브를 시작한다.
 *   [8] ACPI 네임스페이스 장치 프로브.
 *   [9] 마지막으로 번역을 켜고 PMR 을 내린다.
 *
 * 락 다루기: dmar_global_lock 을 쓰기(파싱/초기화)와 읽기(순회)로 나눠 잡고,
 * 프로브나 알림 등록처럼 이 락을 다시 잡는 경로 앞에서는 일부러 놓았다가
 * 다시 잡는다. 코드 곳곳의 up/down 쌍이 그 때문이다 — 락 순서 역전으로
 * lockdep 경고나 실제 데드락이 나는 것을 피한다.
 *
 * 가상화 판별: cap_caching_mode 는 사실상 "이 IOMMU 는 에뮬레이션된 것"이라는
 * 신호다. 그런 환경에서는 지연 무효화(flush queue)의 배치 이득보다 가상/물리
 * 테이블 동기화 비용이 커서 iommu_set_dma_strict() 로 즉시 무효화로 돌린다.
 *
 * 실행 컨텍스트: 부팅 초기화(__init), 프로세스 컨텍스트, 단일 스레드.
 * 에러 처리: 모든 실패가 out_free_dmar 로 모여 파싱해 둔 표를 반납하고
 * 락을 놓은 뒤 반환한다. force_on 이면 그 전에 panic 이 난다.
 *
 * 호출 체인:
 *   pci_iommu_init() (x86 초기화) → [intel_iommu_init]
 *     → dmar_table_init() → dmar_dev_scope_init() → init_dmars()
 *     → init_iommu_pm_ops() → iommu_device_register()
 *     → probe_acpi_namespace_devices() → iommu_enable_translation()
 */
int __init intel_iommu_init(void)
{
	int ret = -ENODEV;	/* [한국어] 기본값은 실패. 마지막까지 갔을 때만 0 으로 바뀐다 */
	struct dmar_drhd_unit *drhd;	/* [한국어] DRHD 순회 커서 */
	struct intel_iommu *iommu;	/* [한국어] 유닛 커서 */

	/*
	 * Intel IOMMU is required for a TXT/tboot launch or platform
	 * opt in, so enforce that.
	 */
	force_on = (!intel_iommu_tboot_noforce && tboot_force_iommu()) ||	/* [한국어] TXT 부팅이거나 (위 영어 주석) */
		    platform_optin_force_iommu();	/* [한국어] 펌웨어가 권장했으면, 실패를 panic 으로 다룬다. 두 경우 모두 격리 없이 계속 가는 것이 더 위험하기 때문이다 */

	down_write(&dmar_global_lock);	/* [한국어] DMAR 전역 자료구조를 바꾸는 구간 */
	if (dmar_table_init()) {	/* [한국어] ACPI DMAR 표를 파싱한다. 여기서 DRHD/RMRR/ATSR/SATC 항목이 모두 등록된다 */
		if (force_on)	/* [한국어] 강제 모드에서 실패하면 */
			panic("tboot: Failed to initialize DMAR table\n");	/* [한국어] 부팅을 멈춘다 — 격리를 약속한 상태로 진행할 수 없다 */
		goto out_free_dmar;	/* [한국어] 아니면 조용히 포기하고 정리 */
	}

	if (dmar_dev_scope_init() < 0) {	/* [한국어] 표에 적힌 장치 경로를 실제 struct device 로 연결한다 */
		if (force_on)	/* [한국어] 강제 모드 */
			panic("tboot: Failed to initialize DMAR device scope\n");	/* [한국어] 멈춘다 */
		goto out_free_dmar;	/* [한국어] 정리 후 반환 */
	}

	up_write(&dmar_global_lock);	/* [한국어] 아래 등록이 이 락을 다시 잡으므로 미리 놓는다 */

	/*
	 * The bus notifier takes the dmar_global_lock, so lockdep will
	 * complain later when we register it under the lock.
	 */
	dmar_register_bus_notifier();	/* [한국어] PCI 버스 알림을 건다. 이후 나타나는 장치가 dmar_iommu_notify_scope_dev 로 표에 연결된다. 락을 놓고 부르는 이유는 위 영어 주석대로 lockdep 경고를 피하기 위해서다 */

	down_write(&dmar_global_lock);	/* [한국어] 다시 잡는다 */

	if (!no_iommu)	/* [한국어] 유닛이 하나라도 있으면 */
		intel_iommu_debugfs_init();	/* [한국어] debugfs 진단 노드를 만든다 */

	if (no_iommu || dmar_disabled) {	/* [한국어] VT-d 를 쓰지 않기로 한 경우 */
		/*
		 * We exit the function here to ensure IOMMU's remapping and
		 * mempool aren't setup, which means that the IOMMU's PMRs
		 * won't be disabled via the call to init_dmars(). So disable
		 * it explicitly here. The PMRs were setup by tboot prior to
		 * calling SENTER, but the kernel is expected to reset/tear
		 * down the PMRs.
		 */
		if (intel_iommu_tboot_noforce) {	/* [한국어] TXT 강제를 껐다면 */
			for_each_iommu(iommu, drhd)	/* [한국어] 모든 유닛에 대해 */
				iommu_disable_protect_mem_regions(iommu);	/* [한국어] 보호 영역만은 반드시 끈다. tboot 이 SENTER 전에 세워 둔 PMR 이 남아 있으면 그 아래 DMA 가 막히므로, 커널이 내려 주기로 되어 있다 (위 영어 주석) */
		}

		/*
		 * Make sure the IOMMUs are switched off, even when we
		 * boot into a kexec kernel and the previous kernel left
		 * them enabled
		 */
		intel_disable_iommus();	/* [한국어] kexec 로 들어와 이전 커널이 켜 둔 상태일 수 있으므로 확실히 꺼 둔다 (위 영어 주석) */
		goto out_free_dmar;	/* [한국어] 파싱해 둔 표를 반납하고 나간다 */
	}

	if (list_empty(&dmar_rmrr_units))	/* [한국어] 예약 구간 신고가 없으면 */
		pr_info("No RMRR found\n");	/* [한국어] 기록만 남긴다. 없는 편이 격리에는 오히려 좋다 */

	if (list_empty(&dmar_atsr_units))	/* [한국어] ATS 보고가 없으면 */
		pr_info("No ATSR found\n");	/* [한국어] 기록 */

	if (list_empty(&dmar_satc_units))	/* [한국어] SATC 신고가 없으면 */
		pr_info("No SATC found\n");	/* [한국어] 기록 */

	init_no_remapping_devices();	/* [한국어] 펌웨어 결함 등으로 번역에서 제외할 장치를 표시한다 */

	ret = init_dmars();	/* [한국어] 핵심 단계 — 유닛마다 루트 테이블·무효화 큐·인터럽트를 세운다 */
	if (ret) {	/* [한국어] 실패 */
		if (force_on)	/* [한국어] 강제 모드면 */
			panic("tboot: Failed to initialize DMARs\n");	/* [한국어] 멈춘다 */
		pr_err("Initialization failed\n");	/* [한국어] 아니면 기록만 */
		goto out_free_dmar;	/* [한국어] 정리 */
	}
	up_write(&dmar_global_lock);	/* [한국어] 쓰기 락 해제. 이후는 읽기만 한다 */

	init_iommu_pm_ops();	/* [한국어] 서스펜드/리줌 콜백을 등록한다 */

	down_read(&dmar_global_lock);	/* [한국어] 읽기 락으로 목록을 훑는다 */
	for_each_active_iommu(iommu, drhd) {	/* [한국어] 동작 중인 유닛마다 */
		/*
		 * The flush queue implementation does not perform
		 * page-selective invalidations that are required for efficient
		 * TLB flushes in virtual environments.  The benefit of batching
		 * is likely to be much lower than the overhead of synchronizing
		 * the virtual and physical IOMMU page-tables.
		 */
		if (cap_caching_mode(iommu->cap) &&	/* [한국어] caching mode 는 사실상 "가상화된 IOMMU" 표시다 (위 영어 주석) */
		    !first_level_by_default(iommu)) {	/* [한국어] 그런데 1단계 페이지 테이블을 쓰지 않는다면 */
			pr_info_once("IOMMU batching disallowed due to virtualization\n");	/* [한국어] 한 번만 알린다 */
			iommu_set_dma_strict();	/* [한국어] 지연 무효화(flush queue)를 끈다. 가상 IOMMU 는 페이지 단위 선택 무효화를 못 해 배치의 이득보다 가상/물리 테이블 동기화 비용이 크기 때문이다 (위 영어 주석) */
		}
		iommu_device_sysfs_add(&iommu->iommu, NULL,	/* [한국어] /sys/class/iommu 아래에 이 유닛을 노출한다 */
				       intel_iommu_groups,	/* [한국어] 위에서 정의한 속성 그룹 */
				       "%s", iommu->name);	/* [한국어] dmar0, dmar1 같은 이름 */
		/*
		 * The iommu device probe is protected by the iommu_probe_device_lock.
		 * Release the dmar_global_lock before entering the device probe path
		 * to avoid unnecessary lock order splat.
		 */
		up_read(&dmar_global_lock);	/* [한국어] 장치 프로브가 이 락을 다시 잡으므로 놓는다 (위 영어 주석) */
		iommu_device_register(&iommu->iommu, &intel_iommu_ops, NULL);	/* [한국어] IOMMU 코어에 이 유닛을 등록한다. 이 시점부터 코어가 장치를 이 유닛으로 프로브하기 시작한다 */
		down_read(&dmar_global_lock);	/* [한국어] 다시 잡는다 */

		iommu_pmu_register(iommu);	/* [한국어] 성능 카운터를 perf 서브시스템에 등록한다 */
	}

	if (probe_acpi_namespace_devices())	/* [한국어] ACPI 네임스페이스 장치들을 프로브한다 */
		pr_warn("ACPI name space devices didn't probe correctly\n");	/* [한국어] 실패해도 부팅은 계속한다 — 그 장치들만 IOMMU 밖에 남는다 */

	/* Finally, we enable the DMA remapping hardware. */
	for_each_iommu(iommu, drhd) {	/* [한국어] 마지막으로 모든 유닛에 대해 (위 영어 주석) */
		if (!drhd->ignored && !translation_pre_enabled(iommu))	/* [한국어] 무시 대상이 아니고, 이미 켜져 있던 상태를 이어받은 것도 아니면 */
			iommu_enable_translation(iommu);	/* [한국어] 번역을 켠다. 인계받은 유닛은 init_dmars 안에서 이미 처리되었으므로 여기서 다시 켜지 않는다 */

		iommu_disable_protect_mem_regions(iommu);	/* [한국어] BIOS 보호 영역을 끈다. 이제 커널의 테이블이 접근을 통제한다 */
	}
	up_read(&dmar_global_lock);	/* [한국어] 순회 끝 */

	pr_info("Intel(R) Virtualization Technology for Directed I/O\n");	/* [한국어] 초기화 완료를 알린다 */

	intel_iommu_enabled = 1;	/* [한국어] 다른 서브시스템(그래픽, VFIO 등)이 이 값을 보고 동작을 바꾼다 */

	return 0;	/* [한국어] VT-d 가 동작 중이다 */

out_free_dmar:	/* [한국어] 모든 실패 경로가 합류 */
	intel_iommu_free_dmars();	/* [한국어] 파싱해 둔 RMRR/ATSR/SATC 를 반납 */
	up_write(&dmar_global_lock);	/* [한국어] 쓰기 락 해제 */
	return ret;	/* [한국어] 실패 이유. -ENODEV 면 애초에 쓰지 않기로 한 경우다 */
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
	default:	/* [한국어] 펌웨어/호출자가 알 수 없는 무효화 종류를 넘겼다 */
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
	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치는 별도 처리가 필요하다 — 아래에서 실제 DMA 를 내는 장치로 바꿔 잡는다 */
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
