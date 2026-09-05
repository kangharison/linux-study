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
	struct list_head list;		/* list of rmrr units	*/
	/* [한국어] (원 주석: list of rmrr units) 전역 rmrr units 목록에 이 항목을 매다는 고리.
	 * 설정자: 부팅 중 DMAR 표를 파싱하며 rmrr units 항목을 만날 때마다 매단다.
	 * 읽는 자: 장치를 도메인에 붙일 때 이 목록을 훑어, 그 장치에 해당하는 예약 구간을
	 *   항등 매핑으로 미리 넣는다.
	 * 동기화: 목록은 부팅과 핫플러그 때만 바뀌고, dmar_global_lock 이 지킨다. */
	struct acpi_dmar_header *hdr;	/* ACPI header		*/
	/* [한국어] (원 주석: ACPI header) 이 항목의 원본 ACPI 구조체를 가리키는 포인터.
	 * 설정자: 파싱이 ACPI 표 안의 해당 위치를 그대로 담는다 — 복사하지 않는다.
	 * 읽는 자: 진단과 재파싱 경로. RMRR 항목의 원본을 그대로 보여 줄 때 쓴다.
	 * 왜 원본을 들고 있는가: 파싱 때 뽑아 둔 필드 말고도 나중에 필요한 값이 생길
	 *   수 있고, 진단할 때 펌웨어가 실제로 뭐라고 적었는지 보여 줄 수 있어야 한다.
	 * 수명: ACPI 표는 부팅 뒤에도 매핑된 채 남으므로 이 포인터는 계속 유효하다. */
	u64	base_address;		/* reserved base address*/
	/* [한국어] (원 주석: reserved base address) 예약 구간의 시작 물리 주소.
	 * 설정자: RMRR 항목 파싱에서.
	 * 읽는 자: 이 구간을 항등 매핑으로 넣을 때, 그리고 사용자가 만든 매핑이
	 *   이 구간과 겹치지 않는지 검사할 때.
	 * 왜 항등 매핑이어야 하는가: 이 구간은 펌웨어나 SMM 이 이미 물리 주소로
	 *   접근하고 있는 곳이다. IOVA 와 물리 주소가 같아야 그 접근이 IOMMU 를
	 *   켠 뒤에도 계속 동작한다.
	 * 값 범위: 페이지 정렬이 보장되지 않아, 매핑할 때 페이지 경계로 넓혀야 한다. */
	u64	end_address;		/* reserved end address */
	/* [한국어] (원 주석: reserved end address) 예약 구간의 끝 주소 — 포함 관계다.
	 * 설정자/읽는 자: 위 base_address 와 같다.
	 * 포함이라는 점이 중요하다: 크기는 end - base + 1 이며, 반열린 구간으로
	 *   오해하면 마지막 페이지가 매핑되지 않아 그 장치만 간헐적으로 실패한다.
	 * 이 구간이 IOMMU 격리의 명백한 구멍이라는 점은 위 블록 주석에 적어 두었다 —
	 *   그래도 받아들일 수밖에 없는 이유까지 함께. */
	struct dmar_dev_scope *devices;	/* target devices */
	/* [한국어] (원 주석: target devices) 이 항목이 적용되는 장치들의 목록.
	 * 설정자: dmar_parse_dev_scope() 가 ACPI 항목 안의 장치 범위(scope)를 훑어 만든다.
	 * 읽는 자: 도메인을 만들 때 그 도메인에 속한 장치가 이 목록에 있는지 보고, 있으면
	 *   위 주소 구간을 항등 매핑으로 넣는다.
	 * 어떻게 장치를 지정하는가: 펌웨어는 PCI 주소가 아니라 "이 루트 포트에서
	 *   출발해 이 경로를 따라간 장치" 라는 형태로 적는다. 부팅 시점에는 아직
	 *   열거되지 않은 장치도 가리킬 수 있어야 하기 때문이다.
	 * 동기화: 장치가 핫플러그될 때 갱신되며, dmar_global_lock 이 지킨다. */
	int	devices_cnt;		/* target device count */
	/* [한국어] (원 주석: target device count) 위 devices 배열에 든 항목의 개수.
	 * 설정자: 장치 범위를 파싱할 때 정해지고, 핫플러그로 장치가 나타나거나
	 *   사라지면 그에 맞춰 바뀐다.
	 * 읽는 자: 그 배열을 훑는 모든 자리. 배열에 끝 표식이 없으므로 이 값이
	 *   유일한 경계다.
	 * 값 범위: 0 이상. 0 이면 이 항목이 가리키는 장치가 현재 하나도 없다는 뜻인데,
	 *   아직 열거되지 않았을 뿐 나중에 나타날 수 있어 항목 자체는 남겨 둔다. */
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
	struct list_head list;		/* list of ATSR units */
	/* [한국어] (원 주석: list of ATSR units) 전역 ATSR units 목록에 이 항목을 매다는 고리.
	 * 설정자: 부팅 중 DMAR 표를 파싱하며 ATSR units 항목을 만날 때마다 매단다.
	 * 읽는 자: 장치의 ATS 를 켜도 되는지 판정할 때 이 목록을 훑는다. 그 장치가 어느
	 *   ATSR 유닛의 범위에 드는지가 곧 펌웨어의 승인 여부다.
	 * 동기화: 목록은 부팅과 핫플러그 때만 바뀌고, dmar_global_lock 이 지킨다. */
	struct acpi_dmar_header *hdr;	/* ACPI header */
	/* [한국어] (원 주석: ACPI header) 이 항목의 원본 ACPI 구조체를 가리키는 포인터.
	 * 설정자: 파싱이 ACPI 표 안의 해당 위치를 그대로 담는다 — 복사하지 않는다.
	 * 읽는 자: 진단 경로, 그리고 핫플러그 때 장치 범위를 다시 훑을 때.
	 * 왜 원본을 들고 있는가: 파싱 때 뽑아 둔 필드 말고도 나중에 필요한 값이 생길
	 *   수 있고, 진단할 때 펌웨어가 실제로 뭐라고 적었는지 보여 줄 수 있어야 한다.
	 * 수명: ACPI 표는 부팅 뒤에도 매핑된 채 남으므로 이 포인터는 계속 유효하다. */
	struct dmar_dev_scope *devices;	/* target devices */
	/* [한국어] (원 주석: target devices) 이 항목이 적용되는 장치들의 목록.
	 * 설정자: dmar_parse_dev_scope() 가 ACPI 항목 안의 장치 범위(scope)를 훑어 만든다.
	 * 읽는 자: dmar_ats_supported() 가 그 장치의 상위 루트 포트가 여기 있는지 확인한다.
	 *   아래 include_all 이 서 있으면 이 목록을 보지 않는다.
	 * 어떻게 장치를 지정하는가: 펌웨어는 PCI 주소가 아니라 "이 루트 포트에서
	 *   출발해 이 경로를 따라간 장치" 라는 형태로 적는다. 부팅 시점에는 아직
	 *   열거되지 않은 장치도 가리킬 수 있어야 하기 때문이다.
	 * 동기화: 장치가 핫플러그될 때 갱신되며, dmar_global_lock 이 지킨다. */
	int devices_cnt;		/* target device count */
	/* [한국어] (원 주석: target device count) 위 devices 배열에 든 항목의 개수.
	 * 설정자: 장치 범위를 파싱할 때 정해지고, 핫플러그로 장치가 나타나거나
	 *   사라지면 그에 맞춰 바뀐다.
	 * 읽는 자: 그 배열을 훑는 모든 자리. 배열에 끝 표식이 없으므로 이 값이
	 *   유일한 경계다.
	 * 값 범위: 0 이상. 0 이면 이 항목이 가리키는 장치가 현재 하나도 없다는 뜻인데,
	 *   아직 열거되지 않았을 뿐 나중에 나타날 수 있어 항목 자체는 남겨 둔다. */
	u8 include_all:1;		/* include all ports */
	/* [한국어] (원 주석: include all ports) 이 유닛 아래의 모든 포트가 해당된다는 표시.
	 * 설정자: ATSR 항목 파싱이 ACPI 플래그를 그대로 옮긴다.
	 * 읽는 자: dmar_ats_supported(). 이 비트가 서 있으면 위 devices 목록을
	 *   훑지 않고 곧바로 승인한다.
	 * 왜 필요한가: 루트 포트가 수십 개인 시스템에서 전부 ATS 를 지원한다면
	 *   그것을 하나하나 나열하는 것은 낭비다. 비트 하나로 "전부"를 표현한다.
	 * 비트필드인 이유: 이 구조체가 항목마다 하나씩 만들어지고, 남는 자리에
	 *   다른 플래그가 추가될 여지를 남겨 둔다. */
};

/*
 * [한국어] SATC(SoC Integrated Address Translation Cache) 항목 하나.
 *
 * SoC 에 통합된 장치 중 ATS 를 쓰되 PCIe 표준 경로를 거치지 않는 것들을 신고한다.
 * 통합 그래픽이나 가속기처럼 칩 안에서 직접 연결된 장치가 대상이며, 표준 ATS
 * 능력 비트로는 알 수 없어 펌웨어가 별도로 알려 준다.
 */
struct dmar_satc_unit {
	struct list_head list;		/* list of SATC units */
	/* [한국어] (원 주석: list of SATC units) 전역 SATC units 목록에 이 항목을 매다는 고리.
	 * 설정자: 부팅 중 DMAR 표를 파싱하며 SATC units 항목을 만날 때마다 매단다.
	 * 읽는 자: SoC 통합 장치의 ATS 를 판정할 때 이 목록을 훑는다. 표준 PCIe ATS 능력
	 *   비트로는 알 수 없는 장치들이 여기 있다.
	 * 동기화: 목록은 부팅과 핫플러그 때만 바뀌고, dmar_global_lock 이 지킨다. */
	struct acpi_dmar_header *hdr;	/* ACPI header */
	/* [한국어] (원 주석: ACPI header) 이 항목의 원본 ACPI 구조체를 가리키는 포인터.
	 * 설정자: 파싱이 ACPI 표 안의 해당 위치를 그대로 담는다 — 복사하지 않는다.
	 * 읽는 자: 진단 경로, 그리고 핫플러그 때 장치 범위를 다시 훑을 때.
	 * 왜 원본을 들고 있는가: 파싱 때 뽑아 둔 필드 말고도 나중에 필요한 값이 생길
	 *   수 있고, 진단할 때 펌웨어가 실제로 뭐라고 적었는지 보여 줄 수 있어야 한다.
	 * 수명: ACPI 표는 부팅 뒤에도 매핑된 채 남으므로 이 포인터는 계속 유효하다. */
	struct dmar_dev_scope *devices;	/* target devices */
	/* [한국어] (원 주석: target devices) 이 항목이 적용되는 장치들의 목록.
	 * 설정자: dmar_parse_dev_scope() 가 ACPI 항목 안의 장치 범위(scope)를 훑어 만든다.
	 * 읽는 자: dmar_ats_supported() 가 그 장치가 여기 있는지 확인한다. 있으면 표준 ATS
	 *   능력 구조가 없어도 ATS 를 쓸 수 있다.
	 * 어떻게 장치를 지정하는가: 펌웨어는 PCI 주소가 아니라 "이 루트 포트에서
	 *   출발해 이 경로를 따라간 장치" 라는 형태로 적는다. 부팅 시점에는 아직
	 *   열거되지 않은 장치도 가리킬 수 있어야 하기 때문이다.
	 * 동기화: 장치가 핫플러그될 때 갱신되며, dmar_global_lock 이 지킨다. */
	struct intel_iommu *iommu;	/* the corresponding iommu */
	/* [한국어] (원 주석: the corresponding iommu) 이 SATC 항목을 담당하는 DMAR 유닛.
	 * 설정자: 파싱이 항목의 세그먼트 번호로 유닛을 찾아 담는다.
	 * 읽는 자: 그 장치의 ATS 를 다룰 때 어느 유닛에 명령을 보낼지 정하는 곳.
	 * 왜 RMRR/ATSR 에는 없고 여기만 있는가: SATC 는 SoC 안에서 직접 연결된
	 *   장치를 다루므로, 그 장치가 어느 유닛에 매여 있는지가 표에 명시된다.
	 *   RMRR 과 ATSR 은 PCI 경로로 장치를 지정해 유닛을 나중에 찾을 수 있다.
	 * 값 범위: NULL 일 수 있다 — 해당 유닛이 아직 초기화되지 않았을 때. */
	int devices_cnt;		/* target device count */
	/* [한국어] (원 주석: target device count) 위 devices 배열에 든 항목의 개수.
	 * 설정자: 장치 범위를 파싱할 때 정해지고, 핫플러그로 장치가 나타나거나
	 *   사라지면 그에 맞춰 바뀐다.
	 * 읽는 자: 그 배열을 훑는 모든 자리. 배열에 끝 표식이 없으므로 이 값이
	 *   유일한 경계다.
	 * 값 범위: 0 이상. 0 이면 이 항목이 가리키는 장치가 현재 하나도 없다는 뜻인데,
	 *   아직 열거되지 않았을 뿐 나중에 나타날 수 있어 항목 자체는 남겨 둔다. */
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

/* [한국어] RMRR 목록 순회 관용구. 여러 곳에서 같은 순회를 하므로 매크로로 뺐다 */
#define for_each_rmrr_units(rmrr) \
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

/*
 * [한국어]
 * domain_context_clear_one_cb - DMA 별칭 하나마다 컨텍스트 항목을 지우는 콜백
 *
 * @pdev: 순회 중인 PCI 장치(여기서는 쓰지 않는다).
 * @alias: 이 장치가 낼 수 있는 소스 id 하나. 상위 8비트가 버스, 하위 8비트가 devfn.
 * @opaque: pci_for_each_dma_alias 에 넘긴 device_domain_info.
 * @return: 항상 0 — 0 이 아니면 순회가 중단되므로, 모든 별칭을 지우려면 0 이어야 한다.
 *
 * 왜 별칭마다 지워야 하는가: PCI 장치가 항상 자기 이름으로 DMA 를 내지는
 * 않는다. PCIe-to-PCI 브리지 뒤의 장치는 브리지의 소스 id 를 쓰고, 일부
 * 장치는 펌웨어가 정한 별칭을 쓴다. IOMMU 는 소스 id 로 컨텍스트 항목을
 * 고르므로, 그 장치가 낼 수 있는 모든 소스 id 의 항목을 지워야 번역이
 * 완전히 끊긴다. 하나라도 남으면 그 id 로 오는 DMA 가 옛 도메인으로 계속
 * 번역된다.
 *
 * 실행 컨텍스트: 장치 분리(detach) 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   domain_context_clear() → pci_for_each_dma_alias()
 *     → [domain_context_clear_one_cb] → domain_context_clear_one()
 */
static int domain_context_clear_one_cb(struct pci_dev *pdev, u16 alias, void *opaque)
{
	struct device_domain_info *info = opaque;	/* [한국어] pci_for_each_dma_alias 가 그대로 넘겨준 장치 정보 */

	domain_context_clear_one(info, PCI_BUS_NUM(alias), alias & 0xff);	/* [한국어] 별칭(alias) 하나의 컨텍스트 항목을 지운다. alias 는 16비트 소스 id 이므로 상위 8비트가 버스, 하위 8비트가 devfn 이다 */
	return 0;	/* [한국어] 계속 순회한다 — 별칭이 여럿이면 전부 지워야 한다 */
}

/*
 * NB - intel-iommu lacks any sort of reference counting for the users of
 * dependent devices.  If multiple endpoints have intersecting dependent
 * devices, unbinding the driver from any one of them will possibly leave
 * the others unable to operate.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * domain_context_clear - 장치가 쓰는 모든 소스 id 의 컨텍스트 항목을 지운다
 *
 * @info: 분리할 장치의 VT-d 정보.
 * @return: 없음.
 *
 * PCI 가 아닌 장치는 별칭이 없으므로 자기 (bus, devfn) 항목만 지우면 된다.
 * PCI 장치는 pci_for_each_dma_alias 로 가능한 모든 소스 id 를 훑으며 지우고,
 * 마지막에 ATS 까지 끈다 — 컨텍스트 항목을 지웠는데 장치 내부 번역 캐시가
 * 살아 있으면 장치는 캐시된 번역으로 DMA 를 계속 낸다.
 *
 * 위 영어 주석의 경고: intel-iommu 는 "의존 장치(dependent device)"에 대한
 * 참조 계수를 두지 않는다. 여러 엔드포인트가 같은 별칭(예: 같은 브리지)을
 * 공유하는 구성에서 그중 하나만 드라이버를 떼어도 공유하던 컨텍스트 항목이
 * 지워져 나머지 장치들이 동작하지 못할 수 있다. 이것은 알려진 한계이며,
 * 그런 장치들이 같은 IOMMU 그룹으로 묶이는 이유이기도 하다.
 *
 * 실행 컨텍스트: 장치 분리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   device_block_translation() → [domain_context_clear]
 *     → pci_for_each_dma_alias() → iommu_disable_pci_ats()
 */
static void domain_context_clear(struct device_domain_info *info)
{
	if (!dev_is_pci(info->dev)) {	/* [한국어] PCI 가 아닌 장치는 별칭 개념이 없다 */
		domain_context_clear_one(info, info->bus, info->devfn);	/* [한국어] 자기 자신의 컨텍스트 항목만 지우면 된다 */
		return;	/* [한국어] 끝 */
	}

	pci_for_each_dma_alias(to_pci_dev(info->dev),	/* [한국어] PCI 장치는 자기 이름이 아닌 소스 id 로 DMA 를 낼 수 있다. PCIe-to-PCI 브리지 뒤의 장치나 별칭을 쓰는 장치가 그렇다 */
			       &domain_context_clear_one_cb, info);	/* [한국어] 가능한 모든 소스 id 의 컨텍스트 항목을 지운다. 하나라도 남으면 그 id 로 오는 DMA 가 여전히 옛 도메인으로 번역된다 */
	iommu_disable_pci_ats(info);	/* [한국어] 장치 내부 번역 캐시도 끈다. 컨텍스트를 지웠는데 ATS 가 살아 있으면 장치가 캐시된 번역으로 계속 DMA 를 낸다 */
}

/*
 * Clear the page table pointer in context or pasid table entries so that
 * all DMA requests without PASID from the device are blocked. If the page
 * table has been set, clean up the data structures.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * device_block_translation - 장치의 번역을 끊어 DMA 를 전부 막고 자료구조를 정리한다
 *
 * @dev: 차단할 장치.
 * @return: 없음.
 *
 * 무엇이 "차단"인가: 컨텍스트 항목(또는 scalable 모드의 PASID 항목)에서
 * 페이지 테이블 포인터를 지운다. 그러면 이 장치가 내는 PASID 없는 DMA 는
 * 번역할 곳이 없어 전부 실패한다. 번역을 아예 끄는 것(passthrough)과는
 * 정반대다 — 통과시키는 게 아니라 막는 것이다.
 *
 * 왜 이 상태가 필요한가: 장치를 한 도메인에서 다른 도메인으로 옮기는 사이,
 * 또는 드라이버가 떨어져 아무 도메인에도 속하지 않는 사이에 그 장치가 옛
 * 매핑으로 DMA 를 계속 내면 안 된다. 그 틈을 막는 것이 차단 상태다.
 *
 * 순서가 중요하다.
 *   1) cache_tag_unassign_domain — 무효화 대상 목록에서 먼저 뺀다. 그래야
 *      이후의 도메인 단위 무효화가 이미 떨어져 나갈 장치를 건드리지 않는다.
 *   2) 하드웨어 항목을 내린다. scalable 모드면 PASID 항목을,
 *      레거시면 컨텍스트 항목을 지운다. 두 모드에서 "번역이 시작되는 곳"이
 *      다르기 때문이다. 서브디바이스(dev_is_real_dma_subdevice)는 부모의
 *      항목을 공유하므로 건너뛴다 — 지우면 부모까지 끊긴다.
 *   3) domain_attached = false 로 표시한다. 두 번 불려도 안전하도록
 *      함수 첫머리에서 이 값을 확인한다.
 *   4) 도메인의 장치 목록에서 빼고, 유닛의 도메인 id 참조를 놓는다.
 *
 * 동기화: 도메인의 장치 목록은 domain->lock 으로 보호하며, 이 락은 무효화
 * 경로에서도 잡히므로 인터럽트를 끈 채(spin_lock_irqsave) 잡는다.
 * 실행 컨텍스트: 도메인 전환/장치 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blocking_domain_attach_dev()/intel_iommu_release_device() 등
 *     → [device_block_translation]
 *     → cache_tag_unassign_domain() → intel_pasid_tear_down_entry()
 *        또는 domain_context_clear() → domain_detach_iommu()
 */
void device_block_translation(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 이 장치의 VT-d 쪽 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	/* Device in DMA blocking state. Noting to do. */
	if (!info->domain_attached)	/* [한국어] 이미 차단 상태면 (위 영어 주석) */
		return;	/* [한국어] 할 일이 없다 */

	if (info->domain)	/* [한국어] 붙어 있던 도메인이 있으면 */
		cache_tag_unassign_domain(info->domain, dev, IOMMU_NO_PASID);	/* [한국어] 무효화 대상 목록에서 이 장치를 뺀다. 먼저 빼 두어야 이후의 도메인 단위 무효화가 이미 떨어진 장치를 건드리지 않는다 */

	if (!dev_is_real_dma_subdevice(dev)) {	/* [한국어] 실제로 자기 컨텍스트 항목을 갖는 장치라면 (서브디바이스는 부모의 항목을 공유한다) */
		if (sm_supported(iommu))	/* [한국어] scalable 모드에서는 */
			intel_pasid_tear_down_entry(iommu, dev,	/* [한국어] PASID 항목을 내린다. 이 모드에서는 컨텍스트 항목이 PASID 디렉터리를 가리키므로 실제 번역을 끊는 곳이 PASID 항목이다 */
						    IOMMU_NO_PASID, false);	/* [한국어] PASID 를 쓰지 않는 기본 트래픽의 항목. false 는 폴트를 유발하지 말고 조용히 차단하라는 뜻이다 */
		else
			domain_context_clear(info);	/* [한국어] 레거시 모드에서는 컨텍스트 항목 자체를 지운다 */
	}

	/* Device now in DMA blocking state. */
	info->domain_attached = false;	/* [한국어] 이제 이 장치의 DMA 는 차단 상태다 (위 영어 주석) */

	if (!info->domain)	/* [한국어] 붙어 있던 도메인이 없었으면 */
		return;	/* [한국어] 정리할 자료구조도 없다 */

	spin_lock_irqsave(&info->domain->lock, flags);	/* [한국어] 도메인의 장치 목록을 바꾼다 */
	list_del(&info->link);	/* [한국어] 그 목록에서 이 장치를 뺀다 */
	spin_unlock_irqrestore(&info->domain->lock, flags);	/* [한국어] 락 해제 */

	domain_detach_iommu(info->domain, iommu);	/* [한국어] 이 유닛에서 도메인 id 참조를 놓는다. 마지막 장치였다면 도메인 id 가 반납된다 */
	info->domain = NULL;	/* [한국어] 더 이상 어느 도메인에도 속하지 않는다 */
}

/*
 * [한국어]
 * blocking_domain_attach_dev - 장치를 차단 도메인에 붙인다(= 모든 DMA 를 막는다)
 *
 * @domain: 전역 blocking_domain. 상태가 없어 실제로는 쓰이지 않는다.
 * @dev: 차단할 장치.
 * @old: 직전에 붙어 있던 도메인(코어가 알려 준다). 여기서는 info 로 알 수 있어 쓰지 않는다.
 * @return: 항상 0. 아무것도 세우지 않고 내리기만 하므로 실패할 수 없다.
 *
 * IOMMU 코어의 차단 도메인 규약: 모든 드라이버는 "이 장치의 DMA 를 전부
 * 막아라"는 요청을 받을 수 있어야 하고, 그 요청은 실패해서는 안 된다.
 * 실패하면 코어가 장치를 안전한 상태로 되돌릴 방법이 없기 때문이다. 그래서
 * 이 콜백은 자원을 새로 잡지 않는 경로로만 구성되어 있다.
 *
 * 순서: 먼저 iopf(I/O 페이지 폴트) 처리를 떼어 내고 그 다음 번역을 끊는다.
 * 반대로 하면, 번역이 끊긴 뒤에도 폴트 큐에 남아 있던 요청이 이미 사라진
 * 도메인을 참조하게 된다.
 *
 * 실행 컨텍스트: 도메인 전환, 드라이버 해제, VFIO 반납 등. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_attach_device(blocking_domain) → [blocking_domain_attach_dev]
 *     → iopf_for_domain_remove() → device_block_translation()
 */
static int blocking_domain_attach_dev(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 이 장치의 VT-d 정보 */

	iopf_for_domain_remove(info->domain ? &info->domain->domain : NULL, dev);	/* [한국어] I/O 페이지 폴트 처리를 먼저 떼어 낸다. 번역을 끊은 뒤에도 폴트 큐에 남은 요청이 있으면 이미 사라진 도메인을 참조하게 된다 */
	device_block_translation(dev);	/* [한국어] 컨텍스트/PASID 항목을 내려 이 장치의 DMA 를 전부 막는다 */
	return 0;	/* [한국어] 차단 도메인 붙이기는 실패할 수 없다 — 아무것도 세우지 않고 내리기만 하기 때문이다 */
}

static int blocking_domain_set_dev_pasid(struct iommu_domain *domain,	/* [한국어] PASID 단위 차단의 전방 선언. 정의는 SVA 관련 코드 뒤에 있다 */
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old);

static struct iommu_domain blocking_domain = {	/* [한국어] 모든 장치가 공유하는 단 하나의 차단 도메인. 상태가 없으므로 인스턴스가 하나면 충분하다 */
	.type = IOMMU_DOMAIN_BLOCKED,	/* [한국어] 코어가 이 도메인을 "모든 DMA 를 막는 도메인" 으로 인식한다 */
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev	= blocking_domain_attach_dev,	/* [한국어] 장치를 여기 붙이면 번역이 내려간다 */
		.set_dev_pasid	= blocking_domain_set_dev_pasid,	/* [한국어] 특정 PASID 만 차단할 때 */
	}
};

/*
 * [한국어]
 * paging_domain_alloc - 빈 dmar_domain 을 만들고 내부 자료구조를 초기화한다
 *
 * @return: 초기화된 dmar_domain, 실패 시 ERR_PTR(-ENOMEM).
 *
 * 도메인은 "하나의 주소 공간"이다. 여러 장치가 같은 도메인에 붙으면 같은
 * IOVA→PA 매핑을 공유한다. 이 함수는 그 껍데기만 만든다 — 페이지 테이블의
 * 단계 수와 최상위 테이블은 호출자가 1단계/2단계 중 무엇을 쓸지 정한 뒤
 * 세운다.
 *
 * 초기화하는 것들과 그 이유:
 *   devices      — 이 도메인에 붙은 장치 목록. 도메인 단위 무효화를 어디에
 *                  보낼지 정할 때 훑는다.
 *   dev_pasids   — PASID 단위로 붙은 (장치, PASID) 쌍. SVA/iommufd 가 쓴다.
 *   cache_tags   — 실제 무효화 대상의 정규화된 목록. 여러 장치가 같은 유닛의
 *                  같은 도메인 id 를 쓰면 무효화는 한 번이면 되므로, 장치
 *                  목록과 별도로 이 목록을 유지한다.
 *   iommu_array  — 유닛 순번 → (도메인 id, 참조 수). 도메인 id 는 유닛마다
 *                  따로 할당되므로 유닛별로 들고 있어야 한다.
 *   s1_domains   — 이 도메인을 2단계(부모)로 삼는 1단계 도메인들. 중첩 변환
 *                  (가상 머신의 게스트 페이지 테이블)에서 쓴다.
 *
 * 락을 세 개(lock, cache_lock, s1_lock)로 나눈 이유: 장치 붙이기/떼기,
 * 무효화 대상 갱신, 중첩 도메인 관리가 서로 다른 빈도와 문맥에서 돌기
 * 때문이다. 특히 cache_lock 은 무효화 경로에서 자주 잡히므로 분리해 두면
 * 장치 붙이기가 무효화를 막지 않는다.
 *
 * 실행 컨텍스트: 도메인 생성 요청. 프로세스 컨텍스트(GFP_KERNEL).
 *
 * 호출 체인:
 *   intel_iommu_domain_alloc_first_stage()/..._second_stage()
 *     → [paging_domain_alloc]
 */
static struct dmar_domain *paging_domain_alloc(void)
{
	struct dmar_domain *domain;	/* [한국어] 만들 도메인 */

	domain = kzalloc_obj(*domain);	/* [한국어] 0 으로 초기화된 도메인 구조체 */
	if (!domain)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 에러 포인터로 반환 — 호출자가 IS_ERR 로 구분한다 */

	INIT_LIST_HEAD(&domain->devices);	/* [한국어] 이 도메인에 붙은 장치 목록 */
	INIT_LIST_HEAD(&domain->dev_pasids);	/* [한국어] PASID 단위로 붙은 (장치, PASID) 쌍 목록 */
	INIT_LIST_HEAD(&domain->cache_tags);	/* [한국어] 무효화 대상 목록. 어느 유닛의 어느 도메인 id 로 무효화를 보낼지가 여기 쌓인다 */
	spin_lock_init(&domain->lock);	/* [한국어] devices/dev_pasids 목록을 지키는 락 */
	spin_lock_init(&domain->cache_lock);	/* [한국어] cache_tags 를 지키는 별도 락. 무효화 경로가 장치 붙이기/떼기와 다른 빈도로 돌기 때문에 락을 나눴다 */
	xa_init(&domain->iommu_array);	/* [한국어] 유닛별 정보(도메인 id, 참조 수)를 유닛 순번으로 색인한다 */
	INIT_LIST_HEAD(&domain->s1_domains);	/* [한국어] 이 도메인을 2단계(부모)로 삼는 1단계 중첩 도메인들의 목록 */
	spin_lock_init(&domain->s1_lock);	/* [한국어] 그 목록을 지키는 락 */

	return domain;	/* [한국어] 필드만 초기화된 빈 도메인. 페이지 테이블은 호출자가 세운다 */
}

/*
 * [한국어]
 * compute_vasz_lg2_fs - 1단계(First-Stage) 페이지 테이블의 주소 폭과 레벨 수를 정한다
 *
 * @iommu: 대상 유닛. 능력 레지스터에서 한계를 읽는다.
 * @top_level: 출력. 최상위 페이지 테이블 레벨 번호(3 이면 4단계, 4 면 5단계).
 * @return: 이 도메인이 쓸 수 있는 가상 주소 폭(비트 수).
 *
 * 왜 두 값을 함께 정하는가: 페이지 테이블의 단계 수와 다룰 수 있는 주소 폭은
 * 같은 것의 두 표현이다. 4단계면 48비트, 5단계면 57비트를 덮는다. 그래서 한
 * 함수가 둘을 함께 결정한다.
 *
 * 상한이 둘이고 둘 중 작은 쪽을 따른다.
 *   - MGAW(Maximum Guest Address Width): 이 유닛의 하드웨어가 실제로 다룰 수
 *     있는 주소 폭. 이보다 큰 주소를 매핑하면 번역이 실패한다.
 *   - FSPM 이 함의하는 정규(canonical) 주소 폭: 1단계 테이블은 CPU 페이지
 *     테이블과 같은 형식이라 x86 의 정규 주소 규칙을 따른다. 4레벨이면
 *     47비트, 5레벨이면 56비트가 사용자 주소의 상한이다(스펙 3.6, 위 영어 주석).
 *
 * 5레벨은 MGAW 가 48을 넘고 cap_fl5lp_support 가 있을 때만 쓴다. 4레벨은
 * 1단계를 지원하는 모든 하드웨어가 지원하므로 조건 없이 기본값이 된다.
 *
 * 실행 컨텍스트: 도메인 생성. 순수 계산이라 락이 필요 없다.
 *
 * 호출 체인:
 *   intel_iommu_domain_alloc_first_stage() → [compute_vasz_lg2_fs]
 *     → cap_mgaw() / cap_fl5lp_support()
 */
static unsigned int compute_vasz_lg2_fs(struct intel_iommu *iommu,
					unsigned int *top_level)
{
	unsigned int mgaw = cap_mgaw(iommu->cap);	/* [한국어] MGAW(Maximum Guest Address Width) — 이 유닛이 다룰 수 있는 주소 폭 */

	/*
	 * Spec 3.6 First-Stage Translation:
	 *
	 * Software must limit addresses to less than the minimum of MGAW
	 * and the lower canonical address width implied by FSPM (i.e.,
	 * 47-bit when FSPM is 4-level and 56-bit when FSPM is 5-level).
	 */
	if (mgaw > 48 && cap_fl5lp_support(iommu->cap)) {	/* [한국어] 48비트를 넘고 1단계 5레벨 페이지 테이블을 지원하면 */
		*top_level = 4;	/* [한국어] 최상위 레벨을 4 로 (레벨 0~4, 즉 5단계) */
		return min(57, mgaw);	/* [한국어] 57비트까지. 스펙 3.6 이 요구하는 상한이며, x86 정규(canonical) 주소의 5레벨 한계와 맞다 (위 영어 주석) */
	}

	/* Four level is always supported */
	*top_level = 3;	/* [한국어] 4레벨은 항상 지원된다 (위 영어 주석) */
	return min(48, mgaw);	/* [한국어] 48비트 상한. MGAW 가 더 작으면 그쪽을 따른다 — 하드웨어가 못 다루는 주소를 쓰면 번역이 실패한다 */
}

/*
 * [한국어]
 * intel_iommu_domain_alloc_first_stage - 1단계(First-Stage) 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 대표 장치. 테이블 메모리의 NUMA 노드를 정하는 데 쓴다.
 * @iommu: 그 장치를 맡은 유닛. 어떤 도메인을 만들 수 있는지는 유닛 능력이 정한다.
 * @flags: iommufd/코어가 요청한 성질. 1단계가 이해하는 것은 PASID 뿐이다.
 * @return: 만들어진 iommu_domain, 실패 시 ERR_PTR. -EOPNOTSUPP 은 "이 유닛으로는
 *          1단계를 만들 수 없다"는 뜻이며, 호출자가 2단계로 내려가는 신호가 된다.
 *
 * 1단계와 2단계의 차이: VT-d scalable 모드는 변환을 두 단계로 나눈다. 1단계는
 * CPU 의 페이지 테이블과 같은 x86-64 형식이고, 2단계는 VT-d 고유 형식이다.
 * 가상화에서 1단계는 게스트가, 2단계는 호스트가 소유하는 구조가 되지만,
 * 가상화가 아닌 보통의 DMA 매핑에서도 둘 중 하나를 골라 쓴다. 1단계를 선호하는
 * 이유는 CPU 페이지 테이블과 형식이 같아 SVA(프로세스 주소 공간 공유)로
 * 자연스럽게 이어지고, 하드웨어가 그쪽에 더 최적화되어 있기 때문이다.
 *
 * 설정하는 것들:
 *   - 주소 폭/레벨: compute_vasz_lg2_fs 가 MGAW 와 정규 주소 규칙을 함께 본다.
 *   - SIGN_EXTEND: 1단계 주소는 x86 정규 주소라 상위 비트가 부호 확장된다.
 *   - DMA_INCOHERENT: 유닛의 페이지 워크가 캐시를 스누프하지 않으면
 *     (!ecap_smpwc) 테이블을 고칠 때마다 캐시를 밀어내야 한다.
 *   - pgsize_bitmap 정리: 하드웨어가 1GB 페이지를 못 하면 그 크기를 뺀다.
 *     코어의 매핑 루프는 이 비트맵만 보고 페이지 크기를 고르므로, 여기서
 *     빼 두지 않으면 하드웨어가 이해하지 못하는 항목을 만들게 된다.
 *
 * 실행 컨텍스트: 도메인 생성 요청(코어 또는 iommufd). 프로세스 컨텍스트.
 * 에러 처리: 페이지 테이블 초기화가 실패하면 도메인 껍데기를 반납한다.
 *
 * 호출 체인:
 *   intel_iommu_domain_alloc_paging_flags() → [이 함수]
 *     → paging_domain_alloc() → compute_vasz_lg2_fs() → pt_iommu_x86_64_init()
 */
static struct iommu_domain *
intel_iommu_domain_alloc_first_stage(struct device *dev,
				     struct intel_iommu *iommu, u32 flags)
{
	struct pt_iommu_x86_64_cfg cfg = {};	/* [한국어] 공용 페이지 테이블 라이브러리(io-pgtable 의 후신)에 넘길 설정. 1단계는 x86-64 CPU 페이지 테이블과 형식이 같아 그 구현을 그대로 쓴다 */
	struct dmar_domain *dmar_domain;	/* [한국어] 만들 도메인 */
	int ret;	/* [한국어] 초기화 결과 */

	if (flags & ~IOMMU_HWPT_ALLOC_PASID)	/* [한국어] 1단계 도메인이 이해하는 플래그는 PASID 하나뿐이다 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 모르는 플래그가 있으면 거절 — 조용히 무시하면 호출자가 요청한 성질이 없는 도메인을 받게 된다 */

	/* Only SL is available in legacy mode */
	if (!sm_supported(iommu) || !ecap_flts(iommu->ecap))	/* [한국어] scalable 모드가 아니거나 1단계 변환을 지원하지 않으면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 레거시 모드에는 2단계밖에 없다 (위 영어 주석) */

	dmar_domain = paging_domain_alloc();	/* [한국어] 빈 도메인 껍데기 */
	if (IS_ERR(dmar_domain))	/* [한국어] 할당 실패 */
		return ERR_CAST(dmar_domain);	/* [한국어] 에러를 그대로 전달 */

	cfg.common.hw_max_vasz_lg2 =	/* [한국어] 다룰 수 있는 가상 주소 폭과 */
		compute_vasz_lg2_fs(iommu, &cfg.top_level);	/* [한국어] 최상위 레벨을 함께 정한다 */
	cfg.common.hw_max_oasz_lg2 = 52;	/* [한국어] 출력(물리) 주소 폭은 52비트. VT-d 페이지 테이블 항목의 주소 필드 폭이다 */
	cfg.common.features = BIT(PT_FEAT_SIGN_EXTEND) |	/* [한국어] 상위 비트가 부호 확장되는 주소 형식 (x86 정규 주소와 같다) */
			      BIT(PT_FEAT_FLUSH_RANGE);	/* [한국어] 범위 단위 무효화를 지원한다고 알린다 */
	/* First stage always uses scalable mode */
	if (!ecap_smpwc(iommu->ecap))	/* [한국어] scalable 모드 페이지 워크가 캐시 코히런트하지 않으면 (위 영어 주석) */
		cfg.common.features |= BIT(PT_FEAT_DMA_INCOHERENT);	/* [한국어] 테이블을 고칠 때마다 캐시를 메모리로 밀어내야 한다고 표시한다 */
	dmar_domain->iommu.iommu_device = dev;	/* [한국어] 테이블 메모리 할당의 기준이 될 장치 */
	dmar_domain->iommu.nid = dev_to_node(dev);	/* [한국어] 그 장치와 가까운 NUMA 노드에서 테이블을 잡는다 */
	dmar_domain->domain.ops = &intel_fs_paging_domain_ops;	/* [한국어] 1단계 전용 콜백 표 */
	/*
	 * iotlb sync for map is only needed for legacy implementations that
	 * explicitly require flushing internal write buffers to ensure memory
	 * coherence.
	 */
	if (rwbf_required(iommu))	/* [한국어] 옛 하드웨어 중 내부 쓰기 버퍼를 명시적으로 비워야 하는 것이 있다 (위 영어 주석) */
		dmar_domain->iotlb_sync_map = true;	/* [한국어] 그런 유닛에서는 매핑 후에도 동기화가 필요하다고 표시한다 */

	ret = pt_iommu_x86_64_init(&dmar_domain->fspt, &cfg, GFP_KERNEL);	/* [한국어] 실제 페이지 테이블 구조를 만든다 */
	if (ret) {	/* [한국어] 페이지 테이블 초기화 실패 */
		kfree(dmar_domain);	/* [한국어] 실패하면 껍데기를 반납 */
		return ERR_PTR(ret);	/* [한국어] 실패 이유를 전달 */
	}

	if (!cap_fl1gp_support(iommu->cap))	/* [한국어] 1GB 큰 페이지를 지원하지 않으면 */
		dmar_domain->domain.pgsize_bitmap &= ~(u64)SZ_1G;	/* [한국어] 그 크기를 후보에서 뺀다. 코어의 매핑 루프가 이 비트맵만 보고 페이지 크기를 고른다 */
	if (!intel_iommu_superpage)	/* [한국어] 큰 페이지를 아예 끄는 부트 인자가 주어졌으면 */
		dmar_domain->domain.pgsize_bitmap = SZ_4K;	/* [한국어] 4KB 만 쓴다 */

	return &dmar_domain->domain;	/* [한국어] 코어가 다루는 iommu_domain 포인터로 반환 */
}

/*
 * [한국어]
 * compute_vasz_lg2_ss - 2단계 페이지 테이블의 주소 폭과 레벨 수를 정한다
 *
 * @iommu: 대상 유닛.
 * @top_level: 출력. 최상위 페이지 테이블 레벨.
 * @return: 쓸 수 있는 가상 주소 폭(비트). 0 이면 어떤 조합도 불가능하다.
 *
 * 1단계용(compute_vasz_lg2_fs)과 나뉜 이유: 2단계는 x86 정규 주소 규칙을 따르지
 * 않는 대신, SAGAW(Supported Adjusted Guest Address Width)라는 별도의 능력
 * 비트마스크가 어떤 단계 수를 지원하는지 알려 준다. 그래서 MGAW(실제 다룰 수
 * 있는 주소 폭)와 SAGAW(테이블 단계) 둘을 동시에 만족하는 가장 큰 조합을
 * 찾아야 한다 — 위 영어 주석이 말하는 그대로다.
 *
 * 왜 둘이 어긋날 수 있는가: 하드웨어에 따라 4레벨이나 5레벨 워크를 할 수는
 * 있지만 IOVA 자체는 3레벨 범위로 제한해야 하는 경우가 있다. 그래서 "워크할 수
 * 있는 단계"와 "쓸 수 있는 주소 폭"을 따로 확인한다.
 *
 * ffs(sagaw >> N) 관용구: SAGAW 마스크에서 최하위로 켜진 비트를 찾아, 그
 * 문턱 위로 지원되는 첫 단계를 고른다. 큰 쪽부터 검사하므로 결과적으로
 * 가능한 가장 큰 주소 공간을 고르게 된다.
 *
 * 0 반환의 의미: 세 조합 모두 안 되면 이 유닛으로는 2단계 도메인을 만들 수
 * 없다. 호출자는 그 값으로 만들어진 도메인이 어차피 아무 주소도 매핑하지
 * 못하므로 실질적으로 실패한다.
 *
 * 실행 컨텍스트: 도메인 생성. 순수 계산.
 *
 * 호출 체인:
 *   intel_iommu_domain_alloc_second_stage() → [compute_vasz_lg2_ss]
 */
static unsigned int compute_vasz_lg2_ss(struct intel_iommu *iommu,
					unsigned int *top_level)
{
	unsigned int sagaw = cap_sagaw(iommu->cap);	/* [한국어] SAGAW — 이 유닛이 지원하는 2단계 페이지 테이블 단계 수의 비트마스크 */
	unsigned int mgaw = cap_mgaw(iommu->cap);	/* [한국어] MGAW — 실제로 다룰 수 있는 주소 폭 */

	/*
	 * Find the largest table size that both the mgaw and sagaw support.
	 * This sets the valid range of IOVA and the top starting level.
	 * Some HW may only support a 4 or 5 level walk but must limit IOVA to
	 * 3 levels.
	 */
	if (mgaw > 48 && sagaw >= BIT(3)) {	/* [한국어] 48비트를 넘고 5단계를 지원하면 (위 영어 주석) */
		*top_level = 4;	/* [한국어] 최상위 레벨 4 = 5단계 */
		return min(57, mgaw);	/* [한국어] 57비트 */
	} else if (mgaw > 39 && sagaw >= BIT(2)) {	/* [한국어] 39비트를 넘고 4단계를 지원하면 */
		*top_level = 3 + ffs(sagaw >> 3);	/* [한국어] 지원하는 것 중 가장 큰 단계를 고른다. ffs 로 마스크의 최하위 켜진 비트를 찾는 것이 곧 "48비트 위로 지원되는 첫 단계"다 */
		return min(48, mgaw);	/* [한국어] 48비트 */
	} else if (mgaw > 30 && sagaw >= BIT(1)) {	/* [한국어] 30비트를 넘고 3단계를 지원하면 */
		*top_level = 2 + ffs(sagaw >> 2);	/* [한국어] 같은 방식으로 단계를 고른다 */
		return min(39, mgaw);	/* [한국어] 39비트 */
	}
	return 0;	/* [한국어] 어느 조합도 안 되면 0 — 호출자가 이 유닛으로는 2단계 도메인을 만들 수 없다고 판단한다 */
}

/*
 * [한국어] 2단계 도메인의 dirty(수정됨) 비트 추적 콜백 표.
 *
 * 무엇에 쓰는가: 가상 머신 라이브 마이그레이션에서 "장치가 어느 페이지에
 * 썼는가"를 알아야 한다. CPU 는 페이지 테이블의 dirty 비트로 그것을 알지만,
 * DMA 는 CPU 를 거치지 않으므로 IOMMU 페이지 테이블에도 같은 비트가 필요하다.
 * VT-d 의 SSADS(Second-Stage Access/Dirty Support)가 그 기능이다.
 *
 * IOMMU_PT_DIRTY_OPS(vtdss) 매크로가 비트를 읽고 지우는 구현을 공용 페이지
 * 테이블 라이브러리에서 채워 넣는다. 추적을 켜고 끄는 것만 유닛 레지스터를
 * 건드리는 VT-d 고유 동작이라 set_dirty_tracking 을 따로 지정한다.
 *
 * 이 표는 IOMMU_HWPT_ALLOC_DIRTY_TRACKING 을 요청한 도메인에만 달린다.
 */
static const struct iommu_dirty_ops intel_second_stage_dirty_ops = {
	IOMMU_PT_DIRTY_OPS(vtdss),	/* [한국어] 공용 페이지 테이블 라이브러리가 제공하는 dirty 비트 읽기/지우기 구현 */
	.set_dirty_tracking = intel_iommu_set_dirty_tracking,	/* [한국어] 추적을 켜고 끄는 것만 VT-d 고유 동작이라 따로 채운다 */
};

/*
 * [한국어]
 * intel_iommu_domain_alloc_second_stage - 2단계(Second-Stage) 페이징 도메인을 만든다
 *
 * @dev: 대표 장치. @iommu: 담당 유닛.
 * @flags: 중첩 부모(NEST_PARENT), dirty 추적, PASID 중 조합.
 * @return: 만들어진 도메인, 실패 시 ERR_PTR.
 *
 * 2단계는 레거시 모드에서 유일하게 쓸 수 있는 형식이고, scalable 모드에서는
 * 호스트가 소유하는 하위 단계다. 가상화에서 게스트의 1단계 테이블 아래에
  * 깔리는 것이 이 도메인이며, 그때 nested_parent 로 표시된다.
 *
 * 1단계와 달리 다루는 성질이 셋이다.
 *   - NEST_PARENT: 이 도메인 위에 게스트의 1단계 도메인이 얹힌다. 이때
 *     PT_FEAT_VTDSS_FORCE_WRITEABLE 를 켜 읽기 전용 매핑을 금지하는데,
 *     ERRATA_772415_SPR17 하드웨어 결함 때문이다(부모가 읽기 전용인 페이지에서
 *     중첩 변환이 잘못 동작한다).
 *   - DIRTY_TRACKING: 라이브 마이그레이션용 dirty 비트 추적.
 *   - PASID.
 *
 * iotlb_sync_map 조건이 1단계보다 넓다: rwbf 뿐 아니라 caching mode 도 포함한다.
 * caching mode 하드웨어는 "매핑 없음" 항목까지 캐시하므로, 새로 만든 매핑을
 * 하드웨어에 알리려면 매핑 후에도 무효화를 보내야 한다. 보통의 IOMMU 가
 * 언매핑 때만 무효화가 필요한 것과 다른 점이다.
 *
 * 실행 컨텍스트: 도메인 생성. 프로세스 컨텍스트.
 * 에러 처리: 요청한 성질을 줄 수 없으면 -EOPNOTSUPP, 테이블 초기화 실패면
 * 껍데기를 반납하고 그 오류를 전달한다.
 *
 * 호출 체인:
 *   intel_iommu_domain_alloc_paging_flags() → [이 함수]
 *     → paging_domain_alloc() → compute_vasz_lg2_ss() → pt_iommu_vtdss_init()
 */
static struct iommu_domain *
intel_iommu_domain_alloc_second_stage(struct device *dev,
				      struct intel_iommu *iommu, u32 flags)
{
	struct pt_iommu_vtdss_cfg cfg = {};	/* [한국어] 2단계 페이지 테이블 설정. 1단계와 달리 VT-d 고유 형식(vtdss)이라 전용 구현을 쓴다 */
	struct dmar_domain *dmar_domain;	/* [한국어] 만들 도메인 */
	unsigned int sslps;	/* [한국어] 2단계가 지원하는 큰 페이지 크기 비트맵 */
	int ret;	/* [한국어] 초기화 결과 */

	if (flags &	/* [한국어] 이 도메인이 이해하는 플래그는 셋뿐이다 — 중첩 부모, dirty 추적, PASID */
	    (~(IOMMU_HWPT_ALLOC_NEST_PARENT | IOMMU_HWPT_ALLOC_DIRTY_TRACKING |
	       IOMMU_HWPT_ALLOC_PASID)))
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 그 밖의 플래그는 거절 */

	if (((flags & IOMMU_HWPT_ALLOC_NEST_PARENT) &&	/* [한국어] 중첩 부모를 요청했는데 */
	     !nested_supported(iommu)) ||	/* [한국어] 하드웨어가 중첩 변환을 못 하거나 */
	    ((flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING) &&	/* [한국어] dirty 추적을 요청했는데 */
	     !ssads_supported(iommu)))	/* [한국어] 2단계 접근/더티 비트를 못 하면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 요청한 성질을 줄 수 없으므로 거절 */

	/* Legacy mode always supports second stage */
	if (sm_supported(iommu) && !ecap_slts(iommu->ecap))	/* [한국어] scalable 모드인데 2단계 변환을 지원하지 않으면 (레거시 모드는 항상 2단계다 — 위 영어 주석) */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절 */

	dmar_domain = paging_domain_alloc();	/* [한국어] 빈 도메인 껍데기 */
	if (IS_ERR(dmar_domain))	/* [한국어] 할당 실패 */
		return ERR_CAST(dmar_domain);	/* [한국어] 전달 */

	cfg.common.hw_max_vasz_lg2 = compute_vasz_lg2_ss(iommu, &cfg.top_level);	/* [한국어] 주소 폭과 레벨을 함께 정한다 */
	cfg.common.hw_max_oasz_lg2 = 52;	/* [한국어] 출력 주소 폭 52비트 */
	cfg.common.features = BIT(PT_FEAT_FLUSH_RANGE);	/* [한국어] 범위 무효화 지원. 1단계와 달리 부호 확장은 없다 — 2단계 주소는 게스트 물리 주소라 정규 주소 규칙이 적용되지 않는다 */

	/*
	 * Read-only mapping is disallowed on the domain which serves as the
	 * parent in a nested configuration, due to HW errata
	 * (ERRATA_772415_SPR17)
	 */
	if (flags & IOMMU_HWPT_ALLOC_NEST_PARENT)	/* [한국어] 중첩 부모로 쓸 도메인이면 */
		cfg.common.features |= BIT(PT_FEAT_VTDSS_FORCE_WRITEABLE);	/* [한국어] 읽기 전용 매핑을 금지한다. ERRATA_772415_SPR17 하드웨어 결함 때문이며, 부모가 읽기 전용인 페이지에서 중첩 변환이 잘못 동작한다 (위 영어 주석) */

	if (!iommu_paging_structure_coherency(iommu))	/* [한국어] 페이지 테이블 워크가 캐시 코히런트하지 않으면 */
		cfg.common.features |= BIT(PT_FEAT_DMA_INCOHERENT);	/* [한국어] 테이블 수정 후 캐시를 밀어내야 한다 */
	dmar_domain->iommu.iommu_device = dev;	/* [한국어] 테이블 할당 기준 장치 */
	dmar_domain->iommu.nid = dev_to_node(dev);	/* [한국어] 가까운 NUMA 노드 */
	dmar_domain->domain.ops = &intel_ss_paging_domain_ops;	/* [한국어] 2단계 전용 콜백 표 */
	dmar_domain->nested_parent = flags & IOMMU_HWPT_ALLOC_NEST_PARENT;	/* [한국어] 이 도메인 아래에 1단계 도메인이 붙을 수 있음을 기록. 해제할 때 자식이 남아 있는지 확인하는 근거가 된다 */

	if (flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING)	/* [한국어] dirty 추적을 요청했으면 */
		dmar_domain->domain.dirty_ops = &intel_second_stage_dirty_ops;	/* [한국어] 그 콜백 표를 단다. 라이브 마이그레이션에서 어느 페이지가 바뀌었는지 추적할 때 쓴다 */

	ret = pt_iommu_vtdss_init(&dmar_domain->sspt, &cfg, GFP_KERNEL);	/* [한국어] 실제 페이지 테이블을 만든다 */
	if (ret) {	/* [한국어] 페이지 테이블 초기화 실패 */
		kfree(dmar_domain);	/* [한국어] 실패하면 껍데기 반납 */
		return ERR_PTR(ret);	/* [한국어] 실패 전달 */
	}

	/* Adjust the supported page sizes to HW capability */
	sslps = cap_super_page_val(iommu->cap);	/* [한국어] 하드웨어가 지원하는 큰 페이지 크기 (위 영어 주석) */
	if (!(sslps & BIT(0)))	/* [한국어] 2MB 를 지원하지 않으면 */
		dmar_domain->domain.pgsize_bitmap &= ~(u64)SZ_2M;	/* [한국어] 후보에서 뺀다 */
	if (!(sslps & BIT(1)))	/* [한국어] 1GB 를 지원하지 않으면 */
		dmar_domain->domain.pgsize_bitmap &= ~(u64)SZ_1G;	/* [한국어] 후보에서 뺀다 */
	if (!intel_iommu_superpage)	/* [한국어] 큰 페이지를 끄는 부트 인자가 있으면 */
		dmar_domain->domain.pgsize_bitmap = SZ_4K;	/* [한국어] 4KB 만 */

	/*
	 * Besides the internal write buffer flush, the caching mode used for
	 * legacy nested translation (which utilizes shadowing page tables)
	 * also requires iotlb sync on map.
	 */
	if (rwbf_required(iommu) || cap_caching_mode(iommu->cap))	/* [한국어] 쓰기 버퍼를 비워야 하는 유닛이거나, 캐싱 모드(= 그림자 페이지 테이블을 쓰는 레거시 중첩 변환)면 (위 영어 주석) */
		dmar_domain->iotlb_sync_map = true;	/* [한국어] 매핑을 만든 뒤에도 동기화가 필요하다. 캐싱 모드에서는 하드웨어가 "없음" 항목까지 캐시하므로, 새로 만든 매핑을 알리려면 무효화를 보내야 한다 */

	return &dmar_domain->domain;	/* [한국어] 완성된 도메인 */
}

/*
 * [한국어]
 * intel_iommu_domain_alloc_paging_flags - 도메인 생성 요청의 진입점. 1단계를 먼저 시도한다
 *
 * @dev: 이 도메인을 쓸 장치.
 * @flags: 요청한 성질.
 * @user_data: iommufd 가 넘긴 사용자 정의 데이터. 여기서는 지원하지 않는다.
 * @return: 만들어진 도메인, 실패 시 ERR_PTR.
 *
 * iommu_ops.domain_alloc_paging_flags 콜백이며, 코어와 iommufd 가 새 주소
 * 공간을 요청할 때 불린다.
 *
 * 정책: 가능하면 1단계를 쓴다. 1단계는 CPU 페이지 테이블과 형식이 같아
 * SVA 로 이어지기 쉽고 하드웨어 최적화도 그쪽에 몰려 있다. 1단계가
 * -EOPNOTSUPP 을 돌려줄 때만 2단계로 내려간다. 이 판별을 위해 두 하위 함수는
 * "지원하지 않음"과 "다른 이유의 실패"를 반드시 구분해서 돌려줘야 하고,
 * 그래서 여기서 반환값을 ERR_PTR(-EOPNOTSUPP) 과 정확히 비교한다.
 *
 * user_data 가 있으면 거절하는 이유: 그것은 사용자가 형식을 지정해 만드는
 * 중첩 도메인 요청이며, intel_iommu_domain_alloc_nested 가 따로 처리한다.
 *
 * 실행 컨텍스트: 도메인 생성 요청. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_alloc()/iommufd → [이 함수]
 *     → intel_iommu_domain_alloc_first_stage()
 *     → intel_iommu_domain_alloc_second_stage()
 */
static struct iommu_domain *
intel_iommu_domain_alloc_paging_flags(struct device *dev, u32 flags,
				      const struct iommu_user_data *user_data)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 이 장치의 VT-d 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 — 어떤 도메인을 만들 수 있는지는 유닛 능력이 정한다 */
	struct iommu_domain *domain;	/* [한국어] 1단계 시도 결과 */

	if (user_data)	/* [한국어] iommufd 가 사용자 정의 데이터를 준 경우는 여기서 다루지 않는다 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 중첩 도메인 생성 경로로 가야 한다 */

	/* Prefer first stage if possible by default. */
	domain = intel_iommu_domain_alloc_first_stage(dev, iommu, flags);	/* [한국어] 1단계를 먼저 시도한다 (위 영어 주석) */
	if (domain != ERR_PTR(-EOPNOTSUPP))	/* [한국어] 지원하지 않는다는 답이 아니면 */
		return domain;	/* [한국어] 성공이든 다른 실패든 그대로 반환한다 */
	return intel_iommu_domain_alloc_second_stage(dev, iommu, flags);	/* [한국어] 1단계가 불가능할 때만 2단계로 내려간다 */
}

/*
 * [한국어]
 * intel_iommu_domain_free - 도메인과 그 페이지 테이블을 반납한다
 *
 * @domain: 해제할 코어 도메인.
 * @return: 없음. 실패를 알릴 방법이 없으므로 위험한 상태면 아무것도 하지 않는다.
 *
 * 두 가지를 먼저 확인한다.
 *   1) 중첩 부모인데 자식 1단계 도메인이 남아 있는가 — 해제하면 자식들이
 *      사라진 부모 테이블을 가리키게 된다.
 *   2) 아직 장치가 붙어 있는가 — 그 장치의 컨텍스트 항목이 이 테이블을
 *      가리키고 있어, 해제하면 하드웨어가 재사용된 메모리를 워크한다.
 * 둘 다 코어가 순서를 어겼을 때만 일어나므로 WARN_ON 으로 스택을 남기고
 * 그냥 돌아간다. 메모리를 누수시키는 편이 use-after-free 보다 안전하다.
 *
 * 해제 순서: 페이지 테이블(pt_iommu_deinit) → 무효화 명령 버퍼(qi_batch)
 * → 도메인 구조체. qi_batch 는 이 도메인의 무효화 명령을 모아 두던 버퍼로,
 * 더 이상 보낼 무효화가 없으므로 함께 반납한다.
 *
 * 실행 컨텍스트: 도메인 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_free() → [intel_iommu_domain_free] → pt_iommu_deinit()
 */
static void intel_iommu_domain_free(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] 코어 도메인에서 VT-d 도메인으로 */

	if (WARN_ON(dmar_domain->nested_parent &&	/* [한국어] 중첩 부모인데 */
		    !list_empty(&dmar_domain->s1_domains)))	/* [한국어] 아직 자식 1단계 도메인이 남아 있으면 */
		return;	/* [한국어] 해제하지 않는다. 해제하면 자식들이 사라진 부모 테이블을 가리키게 된다 */

	if (WARN_ON(!list_empty(&dmar_domain->devices)))	/* [한국어] 아직 장치가 붙어 있으면 */
		return;	/* [한국어] 역시 해제하지 않는다. 코어가 순서를 어긴 것이므로 스택을 남긴다 */

	pt_iommu_deinit(&dmar_domain->iommu);	/* [한국어] 페이지 테이블 전체를 반납한다 */

	kfree(dmar_domain->qi_batch);	/* [한국어] 모아 두었던 무효화 명령 버퍼 */
	kfree(dmar_domain);	/* [한국어] 도메인 구조체 */
}

/*
 * [한국어]
 * paging_domain_compatible_first_stage - 이미 만들어진 1단계 도메인을 이 유닛에 붙여도 되는지 검사한다
 *
 * @dmar_domain: 붙이려는 도메인. 이미 페이지 테이블이 세워져 있고 매핑이
 *               들어 있을 수도 있다.
 * @iommu: 붙일 대상 유닛.
 * @return: 0 이면 붙여도 된다, -EINVAL 이면 안 된다.
 *
 * 왜 이 검사가 필요한가: 도메인은 만들 때 특정 장치/유닛의 능력에 맞춰
 * 설정된다. 그런데 하나의 도메인에 여러 장치를 붙일 수 있고, 그 장치들이 서로
 * 다른 유닛에 속할 수 있다. 능력이 서로 다른 유닛에 같은 테이블을 물리면
 * 조용히 잘못 동작하므로(예: 1GB 항목을 못 읽는 유닛), 붙이기 전에 하나하나
 * 대조해야 한다.
 *
 * 검사 항목과 각각이 어긋났을 때 벌어지는 일:
 *   - 1단계 지원(sm/flts): 애초에 워크할 수 없다.
 *   - 코히런시: 테이블 수정이 하드웨어에 보이지 않아 옛 매핑이 살아 있는
 *     것처럼 동작한다.
 *   - 레벨 수/주소 폭: 상위 주소의 매핑을 번역하지 못한다.
 *   - 페이지 크기: 이미 만들어진 1GB 항목을 하드웨어가 이해하지 못한다.
 *   - iotlb_sync_map: 새 매핑이 하드웨어에 반영되지 않는다.
 * dirty_ops/nested_parent 가 있으면 WARN 하는데, 그것들은 2단계 전용이라
 * 1단계 도메인에 붙어 있다는 것 자체가 생성 경로의 버그다.
 *
 * 실행 컨텍스트: 장치 붙이기. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   paging_domain_compatible() → [이 함수]
 */
static int paging_domain_compatible_first_stage(struct dmar_domain *dmar_domain,
						struct intel_iommu *iommu)
{
	if (WARN_ON(dmar_domain->domain.dirty_ops ||	/* [한국어] 1단계 도메인에는 dirty 추적이나 */
		    dmar_domain->nested_parent))	/* [한국어] 중첩 부모 성질이 있을 수 없다 — 둘 다 2단계 전용이다 */
		return -EINVAL;	/* [한국어] 있으면 만들 때부터 잘못된 것이다 */

	/* Only SL is available in legacy mode */
	if (!sm_supported(iommu) || !ecap_flts(iommu->ecap))	/* [한국어] 이 유닛이 scalable 모드나 1단계 변환을 못 하면 */
		return -EINVAL;	/* [한국어] 붙일 수 없다 (위 영어 주석) */

	if (!ecap_smpwc(iommu->ecap) &&	/* [한국어] 이 유닛의 페이지 워크가 코히런트하지 않은데 */
	    !(dmar_domain->fspt.x86_64_pt.common.features &	/* [한국어] 도메인의 테이블이 */
	      BIT(PT_FEAT_DMA_INCOHERENT)))	/* [한국어] 비코히런트 모드로 만들어지지 않았으면 */
		return -EINVAL;	/* [한국어] 이 유닛에 붙이면 테이블 수정이 하드웨어에 보이지 않는다 */

	/* Supports the number of table levels */
	if (!cap_fl5lp_support(iommu->cap) &&	/* [한국어] 5레벨을 지원하지 않는 유닛인데 (위 영어 주석) */
	    dmar_domain->fspt.x86_64_pt.common.max_vasz_lg2 > 48)	/* [한국어] 도메인이 48비트를 넘는 주소를 쓰면 */
		return -EINVAL;	/* [한국어] 워크할 수 없다 */

	/* Same page size support */
	if (!cap_fl1gp_support(iommu->cap) &&	/* [한국어] 1GB 페이지를 지원하지 않는 유닛인데 (위 영어 주석) */
	    (dmar_domain->domain.pgsize_bitmap & SZ_1G))	/* [한국어] 도메인이 그 크기를 쓸 수 있게 되어 있으면 */
		return -EINVAL;	/* [한국어] 이미 1GB 매핑이 있을 수 있어 위험하다 */

	/* iotlb sync on map requirement */
	if ((rwbf_required(iommu)) && !dmar_domain->iotlb_sync_map)	/* [한국어] 이 유닛은 매핑 후 동기화가 필요한데 도메인이 그렇게 만들어지지 않았으면 (위 영어 주석) */
		return -EINVAL;	/* [한국어] 매핑이 하드웨어에 보이지 않을 수 있다 */

	return 0;	/* [한국어] 모든 조건이 맞는다 — 이 도메인을 이 유닛에 붙여도 된다 */
}

/*
 * [한국어]
 * paging_domain_compatible_second_stage - 2단계 도메인을 이 유닛에 붙여도 되는지 검사한다
 *
 * @dmar_domain: 붙이려는 2단계 도메인.
 * @iommu: 붙일 대상 유닛.
 * @return: 0 가능, -EINVAL 불가능.
 *
 * 1단계용과 같은 목적이지만 확인할 것이 더 많다. 2단계 도메인은 dirty 추적,
 * 중첩 부모, 강제 코히런시 같은 성질을 가질 수 있고, 그 성질들은 각각 유닛의
 * 특정 능력 비트를 요구하기 때문이다.
 *
 * SAGAW 검사(cap_sagaw & BIT(pt_info.aw))가 1단계와 다른 점이다. 2단계는
 * 단계 수를 SAGAW 마스크로 지원 여부가 정해지므로, 도메인이 실제로 쓰는
 * 단계 수가 그 마스크에 있는지 직접 확인해야 한다.
 *
 * FORCE_COHERENCE 검사: 도메인이 "이 도메인의 매핑은 항상 캐시 코히런트하게
 * 다뤄진다"고 약속한 상태(force snooping)라면, 유닛이 snoop control 을
 * 지원해야 그 약속을 지킬 수 있다. 지원하지 않는 유닛에 붙이면 이미 그
 * 약속을 믿고 있는 드라이버(예: GPU)가 잘못된 데이터를 보게 된다.
 *
 * 위 코드의 FIXME(영어 주석): 이 마지막 검사는 dmar_domain->lock 아래에서
 * 해야 하는데 그렇지 않다. force snooping 은 동시에 켜질 수 있어서, 검사와
 * 실제 상태 사이에 창이 있다. 알려진 문제이며 여기서는 고치지 않는다.
 *
 * 실행 컨텍스트: 장치 붙이기. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   paging_domain_compatible() → [이 함수] → pt_iommu_vtdss_hw_info()
 */
static int
paging_domain_compatible_second_stage(struct dmar_domain *dmar_domain,
				      struct intel_iommu *iommu)
{
	unsigned int vasz_lg2 = dmar_domain->sspt.vtdss_pt.common.max_vasz_lg2;	/* [한국어] 이 도메인이 쓰는 주소 폭 */
	unsigned int sslps = cap_super_page_val(iommu->cap);	/* [한국어] 유닛이 지원하는 큰 페이지 크기 */
	struct pt_iommu_vtdss_hw_info pt_info;	/* [한국어] 테이블의 하드웨어 관점 정보(단계 수 등) */

	pt_iommu_vtdss_hw_info(&dmar_domain->sspt, &pt_info);	/* [한국어] 현재 테이블에서 그 값을 뽑는다 */

	if (dmar_domain->domain.dirty_ops && !ssads_supported(iommu))	/* [한국어] 도메인이 dirty 추적을 쓰는데 유닛이 못 하면 */
		return -EINVAL;	/* [한국어] 붙일 수 없다 */
	if (dmar_domain->nested_parent && !nested_supported(iommu))	/* [한국어] 중첩 부모인데 유닛이 중첩을 못 하면 */
		return -EINVAL;	/* [한국어] 붙일 수 없다 */

	/* Legacy mode always supports second stage */
	if (sm_supported(iommu) && !ecap_slts(iommu->ecap))	/* [한국어] scalable 모드인데 2단계를 못 하면 (레거시는 항상 가능 — 위 영어 주석) */
		return -EINVAL;	/* [한국어] 붙일 수 없다 */

	if (!iommu_paging_structure_coherency(iommu) &&	/* [한국어] 워크가 비코히런트한 유닛인데 */
	    !(dmar_domain->sspt.vtdss_pt.common.features &	/* [한국어] 도메인 테이블이 */
	      BIT(PT_FEAT_DMA_INCOHERENT)))	/* [한국어] 비코히런트 모드가 아니면 */
		return -EINVAL;	/* [한국어] 테이블 수정이 하드웨어에 보이지 않는다 */

	/* Address width falls within the capability */
	if (cap_mgaw(iommu->cap) < vasz_lg2)	/* [한국어] 유닛이 다룰 수 있는 주소 폭보다 도메인이 넓으면 (위 영어 주석) */
		return -EINVAL;	/* [한국어] 상위 주소의 매핑을 번역할 수 없다 */

	/* Page table level is supported. */
	if (!(cap_sagaw(iommu->cap) & BIT(pt_info.aw)))	/* [한국어] 유닛이 이 테이블의 단계 수를 지원하지 않으면 (위 영어 주석) */
		return -EINVAL;	/* [한국어] 워크 자체가 불가능하다 */

	/* Same page size support */
	if (!(sslps & BIT(0)) && (dmar_domain->domain.pgsize_bitmap & SZ_2M))	/* [한국어] 2MB 를 못 하는 유닛인데 도메인이 쓸 수 있으면 (위 영어 주석) */
		return -EINVAL;	/* [한국어] 이미 2MB 매핑이 있을 수 있다 */
	if (!(sslps & BIT(1)) && (dmar_domain->domain.pgsize_bitmap & SZ_1G))	/* [한국어] 1GB 도 마찬가지 */
		return -EINVAL;	/* [한국어] 붙일 수 없다 */

	/* iotlb sync on map requirement */
	if ((rwbf_required(iommu) || cap_caching_mode(iommu->cap)) &&	/* [한국어] 이 유닛은 매핑 후 동기화가 필요한데 (위 영어 주석) */
	    !dmar_domain->iotlb_sync_map)	/* [한국어] 도메인이 그렇게 만들어지지 않았으면 */
		return -EINVAL;	/* [한국어] 새 매핑이 하드웨어에 보이지 않는다 */

	/*
	 * FIXME this is locked wrong, it needs to be under the
	 * dmar_domain->lock
	 */
	if ((dmar_domain->sspt.vtdss_pt.common.features &	/* [한국어] 도메인이 강제 코히런시(force snooping)를 쓰는데 */
	     BIT(PT_FEAT_VTDSS_FORCE_COHERENCE)) &&	/* [한국어] (위 FIXME 영어 주석대로 이 검사는 dmar_domain->lock 아래에서 해야 하는데 그렇지 않다 — 알려진 문제다) */
	    !ecap_sc_support(iommu->ecap))	/* [한국어] 유닛이 snoop control 을 지원하지 않으면 */
		return -EINVAL;	/* [한국어] 그 도메인이 약속한 코히런시를 이 유닛에서는 지킬 수 없다 */
	return 0;	/* [한국어] 모든 조건이 맞는다 */
}

/*
 * [한국어]
 * paging_domain_compatible - 도메인을 이 장치에 붙일 수 있는지 판단하고, 필요하면 컨텍스트를 다시 세운다
 *
 * @domain: 붙이려는 코어 도메인.
 * @dev: 붙일 장치.
 * @return: 0 가능, 음수면 그 이유.
 *
 * 도메인 종류(1단계/2단계)에 따라 알맞은 검사 함수로 넘긴다. 두 종류 어느
 * 쪽도 아니면 WARN 을 남기고 거절한다 — 다른 종류(SVA, nested, identity)는
 * 이 경로로 오지 않아야 한다.
 *
 * 검사를 통과한 뒤의 추가 작업이 중요하다: kdump 로 부팅해 이전 커널의
 * 컨텍스트 항목을 그대로 물려받은 장치(context_copied)라면, 그 항목은 이전
 * 커널의 PASID 테이블을 가리키고 있다. 그대로 두면 우리가 만드는 매핑이
 * 하드웨어에 전혀 반영되지 않으므로, 여기서 intel_pasid_setup_sm_context 로
 * 우리 형식의 컨텍스트를 다시 세운다. 물려받은 상태를 우리 것으로 전환하는
 * 마지막 지점이다.
 *
 * 이 검사가 언제 불리는가: 장치를 도메인에 붙이기 직전. iommufd 는 도메인을
 * 재사용하려 할 때 미리 물어보기도 한다.
 *
 * 실행 컨텍스트: 장치 붙이기. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_attach_device()/iommufd → [paging_domain_compatible]
 *     → paging_domain_compatible_first_stage()/..._second_stage()
 *     → intel_pasid_setup_sm_context()
 */
int paging_domain_compatible(struct iommu_domain *domain, struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 이 장치를 맡은 유닛 */
	int ret = -EINVAL;	/* [한국어] 기본값은 부적합 */

	if (intel_domain_is_fs_paging(dmar_domain))	/* [한국어] 1단계 도메인이면 */
		ret = paging_domain_compatible_first_stage(dmar_domain, iommu);	/* [한국어] 1단계 조건으로 검사 */
	else if (intel_domain_is_ss_paging(dmar_domain))	/* [한국어] 2단계 도메인이면 */
		ret = paging_domain_compatible_second_stage(dmar_domain, iommu);	/* [한국어] 2단계 조건으로 검사 */
	else if (WARN_ON(true))	/* [한국어] 둘 다 아니면 도메인 종류가 잘못된 것이다 */
		ret = -EINVAL;	/* [한국어] 거절 */
	if (ret)	/* [한국어] 부적합하면 */
		return ret;	/* [한국어] 붙일 수 없는 이유를 전달 */

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev) &&	/* [한국어] scalable 모드이고 자기 컨텍스트 항목을 갖는 장치인데 */
	    context_copied(iommu, info->bus, info->devfn))	/* [한국어] 그 컨텍스트 항목을 이전 커널에서 그대로 물려받은 것이라면 (kdump 경로) */
		return intel_pasid_setup_sm_context(dev);	/* [한국어] 인계받은 항목을 우리 형식으로 다시 세운다. 물려받은 항목은 이전 커널의 PASID 테이블을 가리키고 있어, 그대로 두면 우리가 만든 매핑이 반영되지 않는다 */

	return 0;	/* [한국어] 이 장치에 이 도메인을 붙일 수 있다 */
}

/*
 * [한국어]
 * intel_iommu_attach_device - 장치를 페이징 도메인에 붙인다
 *
 * @domain: 붙일 도메인(1단계 또는 2단계 페이징).
 * @dev: 붙일 장치.
 * @old: 직전 도메인. 코어가 알려 주지만 여기서는 쓰지 않는다.
 * @return: 0 성공, 음수면 실패. 실패해도 장치는 안전한 차단 상태로 남는다.
 *
 * 순서 자체가 이 함수의 내용이다.
 *   1) device_block_translation — 지금 붙어 있는 것을 먼저 전부 내린다.
 *      "옛 매핑과 새 매핑이 동시에 유효한 순간"을 만들지 않기 위해서다.
 *      또한 이 단계 덕분에 이후 어느 지점에서 실패해도 장치는 DMA 가 막힌
 *      상태로 남아, 되돌릴 것이 없다.
 *   2) paging_domain_compatible — 이 유닛의 능력과 도메인 설정이 맞는지.
 *      이 검사는 kdump 로 물려받은 컨텍스트를 우리 형식으로 전환하는 일까지
 *      겸한다.
 *   3) iopf_for_domain_set — 페이지 폴트 처리를 먼저 연결한다. 번역을 켠 뒤에
 *      연결하면 그 사이의 폴트를 처리할 곳이 없다.
 *   4) dmar_domain_attach_device — 실제로 하드웨어 항목을 세운다.
 *      실패하면 3)을 되돌린다.
 *
 * iommu_domain_ops.attach_dev 콜백이며, 코어가 도메인을 바꿀 때마다 부른다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 그룹 뮤텍스를 쥔 상태로 들어온다.
 *
 * 호출 체인:
 *   iommu_attach_device()/iommu_attach_group() → [이 함수]
 *     → device_block_translation() → paging_domain_compatible()
 *     → iopf_for_domain_set() → dmar_domain_attach_device()
 */
static int intel_iommu_attach_device(struct iommu_domain *domain,
				     struct device *dev,
				     struct iommu_domain *old)
{
	int ret;	/* [한국어] 각 단계의 결과 */

	device_block_translation(dev);	/* [한국어] 먼저 지금 붙어 있는 것을 모두 내린다. 옛 매핑과 새 매핑이 겹치는 순간을 만들지 않기 위한 것이며, 실패해도 되돌릴 필요가 없는 상태로 만든다 */

	ret = paging_domain_compatible(domain, dev);	/* [한국어] 이 도메인을 이 장치에 붙일 수 있는지 */
	if (ret)	/* [한국어] 불가능하면 */
		return ret;	/* [한국어] 장치는 차단 상태로 남는다 — 안전한 실패다 */

	ret = iopf_for_domain_set(domain, dev);	/* [한국어] I/O 페이지 폴트 처리를 이 도메인에 연결한다. 번역을 세우기 전에 해야 첫 폴트부터 처리된다 */
	if (ret)	/* [한국어] 실패 */
		return ret;	/* [한국어] 역시 차단 상태로 남는다 */

	ret = dmar_domain_attach_device(to_dmar_domain(domain), dev);	/* [한국어] 실제로 컨텍스트/PASID 항목을 세워 번역을 켠다 */
	if (ret)	/* [한국어] 실패하면 */
		iopf_for_domain_remove(domain, dev);	/* [한국어] 방금 연결한 폴트 처리를 되돌린다 */

	return ret;	/* [한국어] 성공이면 0 */
}

/*
 * [한국어]
 * intel_iommu_tlb_sync - 모아 둔 언매핑 범위를 한 번에 무효화하고 페이지를 반납한다
 *
 * @domain: 대상 도메인.
 * @gather: 코어가 언매핑하며 누적한 정보 — 범위(start~end)와, 해제 대기 중인
 *          페이지 테이블 페이지 목록(freelist).
 * @return: 없음.
 *
 * 왜 모았다가 한 번에 하는가: 무효화 명령은 큐에 넣고 완료를 기다려야 하는
 * 값비싼 동작이다. 4KB 씩 백만 번 언매핑하면서 매번 무효화를 보내면 그 비용이
 * 전부다. 그래서 코어는 언매핑 범위를 gather 에 누적했다가, 한 묶음이 끝나면
 * 이 콜백에서 한 번에 처리한다.
 *
 * freelist 가 함께 오는 이유와 순서: 큰 매핑을 풀면 페이지 테이블 페이지 자체가
 * 통째로 비게 되어 반납 대상이 된다. 그런데 하드웨어가 그 테이블을 아직
 * 캐시하고 있을 수 있으므로, 반드시 무효화가 끝난 뒤에 반납해야 한다.
 * 이 함수가 cache_tag_flush_range 다음에 iommu_put_pages_list 를 부르는 순서가
 * 정확히 그 규칙이다. 반대로 하면 하드웨어가 이미 재사용된 메모리를 페이지
 * 테이블로 워크한다.
 *
 * freelist 가 비어 있는지를 flush 에 넘기는 이유: 비어 있지 않다는 것은 페이지
 * 테이블 구조 자체가 바뀌었다는 뜻이라, 잎(leaf) 항목만 비우는 좁은 무효화로는
 * 부족하고 중간 단계까지 비워야 한다.
 *
 * 실행 컨텍스트: 언매핑 후처리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_iotlb_sync() → [intel_iommu_tlb_sync]
 *     → cache_tag_flush_range() → iommu_put_pages_list()
 */
static void intel_iommu_tlb_sync(struct iommu_domain *domain,
				 struct iommu_iotlb_gather *gather)
{
	cache_tag_flush_range(to_dmar_domain(domain), gather->start,	/* [한국어] 모아 둔 언매핑 범위를 한 번에 무효화한다. 언매핑마다 무효화를 보내는 대신 gather 에 범위를 누적했다가 여기서 한 번에 처리하는 것이 배치의 핵심이다 */
			      gather->end,	/* [한국어] 범위의 끝 */
			      iommu_pages_list_empty(&gather->freelist));	/* [한국어] 해제 대기 페이지 목록이 비어 있는지. 비어 있지 않다면 페이지 테이블 자체가 정리된 것이라 더 넓은 무효화가 필요하다 */
	iommu_put_pages_list(&gather->freelist);	/* [한국어] 무효화가 끝난 뒤에야 페이지 테이블 페이지를 반납한다. 순서를 바꾸면 하드웨어가 아직 캐시하고 있는 테이블 페이지가 재사용된다 */
}

/*
 * [한국어]
 * domain_support_force_snooping - 이 도메인에 붙은 모든 유닛이 강제 코히런시를 지원하는지 본다
 *
 * @domain: 검사할 도메인. 호출자가 domain->lock 을 쥐고 있어야 한다.
 * @return: 모든 유닛이 snoop control 을 지원하면 true.
 *
 * force snooping 이 무엇인가: 보통 DMA 가 CPU 캐시를 스누프할지는 장치가
 * 요청 안에서 정한다. 그런데 KVM 이 게스트에 장치를 넘길 때는 "이 도메인의
 * 모든 DMA 는 무조건 코히런트하다"는 보장이 필요하다. 그래야 게스트가
 * 캐시를 직접 관리하지 않아도 되고, 호스트가 WBINVD 같은 위험한 명령을
 * 게스트에 허용하지 않아도 된다. VT-d 의 snoop control(SC)이 그 강제다.
 *
 * 왜 모든 유닛을 확인하는가: 하나의 도메인에 여러 장치가, 서로 다른 유닛
 * 아래에서 붙을 수 있다. 그중 하나라도 SC 를 지원하지 않으면 그 장치의 DMA 는
 * 코히런시가 보장되지 않으므로, 도메인 전체가 그 약속을 할 수 없다.
 *
 * assert_spin_locked 인 이유: 순회 도중 장치가 붙거나 떨어지면 결론이
 * 틀어진다. 락을 이 함수가 잡지 않고 호출자에게 요구하는 것은, 호출자가
 * 검사와 force_snooping 설정을 같은 임계 구역 안에서 해야 하기 때문이다.
 *
 * 실행 컨텍스트: domain->lock(irqsave)을 쥔 상태. 잠들면 안 된다.
 *
 * 호출 체인:
 *   intel_iommu_enforce_cache_coherency_fs()/_ss()
 *     → [domain_support_force_snooping]
 */
static bool domain_support_force_snooping(struct dmar_domain *domain)
{
	struct device_domain_info *info;	/* [한국어] 장치 순회 커서 */
	bool support = true;	/* [한국어] 기본값은 지원. 하나라도 못 하면 뒤집는다 */

	assert_spin_locked(&domain->lock);	/* [한국어] 호출자가 도메인 락을 쥐고 있어야 한다. 순회 중에 장치가 붙거나 떨어지면 판단이 틀어지기 때문이다 */
	list_for_each_entry(info, &domain->devices, link) {	/* [한국어] 이 도메인에 붙은 모든 장치를 훑으며 */
		if (!ecap_sc_support(info->iommu->ecap)) {	/* [한국어] 그 장치를 맡은 유닛이 snoop control 을 지원하지 않으면 */
			support = false;	/* [한국어] 도메인 전체가 강제 코히런시를 약속할 수 없다 */
			break;	/* [한국어] 하나만 못 해도 결론이 난다 */
		}
	}

	return support;	/* [한국어] 모든 유닛이 지원할 때만 true */
}

/*
 * [한국어]
 * intel_iommu_enforce_cache_coherency_fs - 1단계 도메인에 강제 코히런시를 켠다
 *
 * @domain: 대상 도메인(1단계 페이징).
 * @return: true 면 켜졌거나 이미 켜져 있다, false 면 하드웨어가 지원하지 않는다.
 *
 * VFIO/KVM 이 장치를 게스트에 넘기기 전에 부른다. false 를 받으면 게스트에
 * WBINVD 를 허용하는 등 다른(더 비싼, 더 위험한) 방법을 써야 한다.
 *
 * 1단계에서의 구현이 2단계와 다르다: 1단계 페이지 테이블은 x86-64 CPU 형식
 * 그대로라 PTE 안에 스누프 제어 비트를 둘 자리가 없다. 그래서 PTE 가 아니라
 * PASID 항목에 도메인 단위로 설정한다. 이미 붙어 있는 장치들에 대해
 * intel_pasid_setup_page_snoop_control 을 하나씩 부르는 것이 그 때문이다.
 * (2단계는 PTE 마다 SNP 비트가 있어 설정만 바꿔 두면 된다 — _ss 쪽 참고.)
 *
 * 이후에 붙는 장치는 dmar_domain_attach_device 가 force_snooping 을 보고
 * 같은 설정을 해 준다. 그래서 여기서는 "지금 붙어 있는 것"만 처리하면 된다.
 *
 * 동기화: guard(spinlock_irqsave) 로 도메인 락을 잡아, 검사와 설정과 순회가
 * 한 임계 구역 안에서 일어나게 한다. 그 사이에 장치가 붙으면 새 장치는 위
 * 규칙에 따라 attach 경로에서 처리된다.
 *
 * 호출 체인:
 *   iommu_enforce_cache_coherency() (VFIO/KVM) → [이 함수]
 *     → domain_support_force_snooping()
 *     → intel_pasid_setup_page_snoop_control()
 */
static bool intel_iommu_enforce_cache_coherency_fs(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct device_domain_info *info;	/* [한국어] 장치 순회 커서 */

	guard(spinlock_irqsave)(&dmar_domain->lock);	/* [한국어] 도메인 장치 목록을 보호한다. guard 는 함수를 벗어날 때 자동으로 놓아 준다 */

	if (dmar_domain->force_snooping)	/* [한국어] 이미 켜져 있으면 */
		return true;	/* [한국어] 다시 할 일이 없다 */

	if (!domain_support_force_snooping(dmar_domain))	/* [한국어] 붙어 있는 유닛 중 하나라도 snoop control 을 못 하면 */
		return false;	/* [한국어] 약속할 수 없다 */

	dmar_domain->force_snooping = true;	/* [한국어] 이제부터 이 도메인의 매핑은 캐시 코히런트하게 다뤄진다 */
	list_for_each_entry(info, &dmar_domain->devices, link)	/* [한국어] 이미 붙어 있는 장치들에 대해 */
		intel_pasid_setup_page_snoop_control(info->iommu, info->dev,	/* [한국어] 1단계는 PTE 마다 스누프 비트를 두지 않으므로, PASID 항목에 도메인 단위로 설정한다 */
						     IOMMU_NO_PASID);	/* [한국어] PASID 를 쓰지 않는 기본 트래픽의 항목 */
	return true;	/* [한국어] 강제 코히런시가 켜졌다 */
}

/*
 * [한국어]
 * intel_iommu_enforce_cache_coherency_ss - 2단계 도메인에 강제 코히런시를 켠다
 *
 * @domain: 대상 도메인(2단계 페이징).
 * @return: true 면 켜졌다, false 면 지원하지 않는다.
 *
 * _fs 판과 목적은 같지만 구현이 훨씬 간단하다. 2단계 페이지 테이블은 항목마다
 * SNP(snoop) 비트를 갖고 있으므로, 테이블 설정에 PT_FEAT_VTDSS_FORCE_COHERENCE
 * 를 켜 두기만 하면 이후 iommu_map() 이 만드는 모든 항목에 그 비트가 실린다
 * (위 영어 주석). 이미 붙어 있는 장치를 하나씩 손볼 필요가 없다.
 *
 * 이미 만들어진 매핑은 어떻게 되는가: 이 콜백은 도메인에 매핑이 들어가기
 * 전(VFIO 가 컨테이너를 세우는 시점)에 불리는 것을 전제로 한다. 그래서 _fs 와
 * 달리 force_snooping 이 이미 켜져 있는지 먼저 확인하지도 않는다.
 *
 * 이 설정은 나중에 paging_domain_compatible_second_stage 의 검사 대상이
 * 되기도 한다 — SC 를 지원하지 않는 유닛에 이 도메인을 붙이려 하면 거절된다.
 *
 * 동기화: 도메인 락을 guard 로 잡는다.
 *
 * 호출 체인:
 *   iommu_enforce_cache_coherency() (VFIO/KVM) → [이 함수]
 *     → domain_support_force_snooping()
 */
static bool intel_iommu_enforce_cache_coherency_ss(struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */

	guard(spinlock_irqsave)(&dmar_domain->lock);	/* [한국어] 도메인 락 */
	if (!domain_support_force_snooping(dmar_domain))	/* [한국어] 유닛들이 지원하지 않으면 */
		return false;	/* [한국어] 약속할 수 없다 */

	/*
	 * Second level page table supports per-PTE snoop control. The
	 * iommu_map() interface will handle this by setting SNP bit.
	 */
	dmar_domain->sspt.vtdss_pt.common.features |=	/* [한국어] 2단계는 PTE 마다 SNP 비트를 둘 수 있으므로, 테이블 설정에 표시만 해 두면 iommu_map() 이 매 항목에 비트를 세운다 (위 영어 주석) */
		BIT(PT_FEAT_VTDSS_FORCE_COHERENCE);	/* [한국어] 그 기능 비트 */
	dmar_domain->force_snooping = true;	/* [한국어] 도메인 상태에도 기록 */
	return true;	/* [한국어] 켜졌다. 1단계와 달리 이미 붙은 장치를 다시 손볼 필요가 없다 — 설정이 PTE 에 들어가기 때문이다 */
}

/*
 * [한국어]
 * intel_iommu_capable - 코어가 묻는 능력에 이 장치/유닛이 답한다
 *
 * @dev: 대상 장치.
 * @cap: 코어가 묻는 능력 종류.
 * @return: 지원하면 true.
 *
 * 각 능력이 무엇을 결정하는가:
 *   CACHE_COHERENCY — DMA 가 CPU 캐시와 코히런트한가. x86 은 장치 DMA 가
 *     캐시를 스누프하므로 항상 참이다.
 *   PRE_BOOT_PROTECTION — 커널이 뜨기 전부터 DMA 가 막혀 있었는가. 펌웨어가
 *     platform opt-in 으로 신고했을 때만 참이며, Thunderbolt 보안 정책에서
 *     "부팅 중 DMA 공격이 가능했는가"를 판단하는 근거가 된다.
 *   ENFORCE_CACHE_COHERENCY — 도메인 단위로 코히런시를 강제할 수 있는가.
 *     VFIO/KVM 이 이 값으로 게스트에 WBINVD 를 허용할지 정한다.
 *   DIRTY_TRACKING — 수정된 페이지를 추적할 수 있는가. 라이브 마이그레이션의
 *     전제 조건이다.
 *   PCI_ATS_SUPPORTED — 프로브 때 dmar_ats_supported 로 판단해 저장해 둔 값.
 *
 * 모르는 능력에 false 를 돌려주는 것이 중요하다: 코어에 새 능력이 추가되어도
 * 이 드라이버가 그것을 지원한다고 잘못 답하지 않는다.
 *
 * 실행 컨텍스트: 아무 데서나 불릴 수 있는 순수 조회. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   iommu_capable()/device_iommu_capable() → [intel_iommu_capable]
 */
static bool intel_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 이 장치의 VT-d 정보 */

	switch (cap) {	/* [한국어] 코어가 묻는 능력 종류에 따라 */
	case IOMMU_CAP_CACHE_COHERENCY:	/* [한국어] DMA 가 CPU 캐시와 코히런트한가 */
		return true;	/* [한국어] x86 은 항상 그렇다 — 장치 DMA 가 캐시를 스누프한다 */
	case IOMMU_CAP_PRE_BOOT_PROTECTION:	/* [한국어] 커널이 뜨기 전부터 DMA 가 막혀 있었는가 */
		return dmar_platform_optin();	/* [한국어] 펌웨어가 그렇게 신고했을 때만 참이다. Thunderbolt 보안에서 이 값이 중요하다 */
	case IOMMU_CAP_ENFORCE_CACHE_COHERENCY:	/* [한국어] 도메인 단위로 코히런시를 강제할 수 있는가 */
		return ecap_sc_support(info->iommu->ecap);	/* [한국어] 유닛의 snoop control 지원 여부. VFIO/KVM 이 이 값으로 게스트의 캐시 관리 정책을 정한다 */
	case IOMMU_CAP_DIRTY_TRACKING:	/* [한국어] 수정된 페이지를 추적할 수 있는가 */
		return ssads_supported(info->iommu);	/* [한국어] 2단계 접근/더티 비트 지원 여부. 라이브 마이그레이션의 전제다 */
	case IOMMU_CAP_PCI_ATS_SUPPORTED:	/* [한국어] 이 장치에 ATS 를 쓸 수 있는가 */
		return info->ats_supported;	/* [한국어] 프로브 때 dmar_ats_supported 로 판단해 저장해 둔 값 */
	default:	/* [한국어] 코어가 아직 모르는 능력을 물었다 — 지원하지 않는다고 답한다 */
		return false;	/* [한국어] 모르는 능력은 지원하지 않는다고 답한다 */
	}
}

/*
 * [한국어]
 * intel_iommu_probe_device - 장치를 VT-d 아래로 들이고 능력을 조사한다
 *
 * @dev: 프로브할 장치.
 * @return: 이 장치를 맡을 유닛의 iommu_device, 실패 시 ERR_PTR.
 *          -ENODEV 는 "이 장치는 IOMMU 아래가 아니다"라는 정상적인 답이다.
 *
 * 장치가 처음 IOMMU 코어에 등장할 때 불리며, 이후 이 장치에 대한 모든 판단의
 * 근거가 되는 device_domain_info 를 만든다. 크게 세 단계다.
 *
 *   [1] 담당 유닛 찾기 — device_lookup_iommu 가 DMAR 표를 따라간다. 유닛이
 *       없으면 이 장치는 번역 대상이 아니다.
 *   [2] 소스 id 정하기 — 보통은 유닛 조회가 알려 준 (bus, devfn)을 쓴다.
 *       별칭 때문에 장치 자신의 위치와 다를 수 있기 때문이다. 다만 실제 DMA
 *       서브디바이스는 자기 PCI 위치를 그대로 쓴다.
 *   [3] 능력 조사 — ATS, PASID, PRI 를 각각 "유닛도 지원하고 장치도 지원하고
 *       경로도 허용하는가"로 확인해 기록한다. 여기서 정해진 값이
 *       intel_iommu_probe_finalize 에서 실제로 무엇을 켤지를 결정한다.
 *
 * 눈여겨볼 세부:
 *   - pasid_supported 에 |1 을 하는 이유: PCIe 가 알려 준 features 가 0 일 수
 *     있어서, 그것만으로는 "능력 없음"과 구분되지 않는다. 비트 0 을 존재
 *     표시로 겸용한다.
 *   - PRI 가 ATS 를 전제로 하는 이유: 페이지 요청의 응답이 ATS 번역 경로로
 *     돌아오기 때문이다.
 *   - pfsid: 유닛이 DIT 를 지원할 때만 의미가 있다. VF 의 무효화 요청에 PF 의
 *     소스 id 를 실어 주면 하드웨어가 PF 단위로 큐 깊이를 가늠할 수 있다.
 *     DIT 가 없으면 예약 필드라 0 이어야 한다.
 *   - context_copied 검사: kdump 로 물려받은 컨텍스트는 여기서 손대지 않는다.
 *     도메인을 붙일 때 paging_domain_compatible 이 전환한다.
 *
 * 실행 컨텍스트: 장치 프로브. 프로세스 컨텍스트(GFP_KERNEL 할당).
 * 에러 처리: 세 단계의 정리 라벨(free_table → clear_rbtree → free)이
 * 되돌리기 순서를 그대로 따른다.
 *
 * 호출 체인:
 *   iommu_probe_device() → [intel_iommu_probe_device]
 *     → device_lookup_iommu() → dmar_ats_supported()
 *     → device_rbtree_insert() → intel_pasid_alloc_table()
 *     → intel_pasid_setup_sm_context()
 */
static struct iommu_device *intel_iommu_probe_device(struct device *dev)
{
	struct pci_dev *pdev = dev_is_pci(dev) ? to_pci_dev(dev) : NULL;	/* [한국어] PCI 장치면 그 포인터, 아니면 NULL */
	struct device_domain_info *info;	/* [한국어] 만들 장치 정보 */
	struct intel_iommu *iommu;	/* [한국어] 이 장치를 맡을 유닛 */
	u8 bus, devfn;	/* [한국어] 유닛이 보는 소스 id 의 두 부분 */
	int ret;	/* [한국어] 각 단계의 결과 */

	iommu = device_lookup_iommu(dev, &bus, &devfn);	/* [한국어] DMAR 표를 따라 이 장치를 담당하는 유닛을 찾는다 */
	if (!iommu || !iommu->iommu.ops)	/* [한국어] 유닛이 없거나 아직 코어에 등록되지 않았으면 */
		return ERR_PTR(-ENODEV);	/* [한국어] 이 장치는 IOMMU 아래가 아니다 */

	info = kzalloc_obj(*info);	/* [한국어] 장치 정보 구조체 */
	if (!info)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 프로브 실패 */

	if (dev_is_real_dma_subdevice(dev)) {	/* [한국어] 부모의 컨텍스트 항목을 공유하는 서브디바이스면 */
		info->bus = pdev->bus->number;	/* [한국어] 자기 PCI 위치를 그대로 쓴다 */
		info->devfn = pdev->devfn;	/* [한국어] 자기 devfn */
		info->segment = pci_domain_nr(pdev->bus);	/* [한국어] 자기 세그먼트 */
	} else {
		info->bus = bus;	/* [한국어] 보통은 유닛 조회가 알려 준 값 — 별칭 때문에 자기 위치와 다를 수 있다 */
		info->devfn = devfn;	/* [한국어] 그 devfn */
		info->segment = iommu->segment;	/* [한국어] 유닛의 세그먼트 */
	}

	info->dev = dev;	/* [한국어] 원본 장치 */
	info->iommu = iommu;	/* [한국어] 담당 유닛 */
	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치라면 ATS/PRI/PASID 능력을 추가로 살핀다 */
		if (ecap_dev_iotlb_support(iommu->ecap) &&	/* [한국어] 유닛이 디바이스 IOTLB 를 지원하고 */
		    pci_ats_supported(pdev) &&	/* [한국어] 장치에 ATS 능력 구조가 있고 */
		    dmar_ats_supported(pdev, iommu)) {	/* [한국어] 경로와 펌웨어 신고까지 허용하면 */
			info->ats_supported = 1;	/* [한국어] ATS 를 쓸 수 있는 장치로 표시 */
			info->dtlb_extra_inval = dev_needs_extra_dtlb_flush(pdev);	/* [한국어] 일부 장치는 결함 때문에 디바이스 TLB 무효화를 한 번 더 보내야 한다 */

			/*
			 * For IOMMU that supports device IOTLB throttling
			 * (DIT), we assign PFSID to the invalidation desc
			 * of a VF such that IOMMU HW can gauge queue depth
			 * at PF level. If DIT is not set, PFSID will be
			 * treated as reserved, which should be set to 0.
			 */
			if (ecap_dit(iommu->ecap))	/* [한국어] 유닛이 디바이스 IOTLB 스로틀링(DIT)을 지원하면 (위 영어 주석) */
				info->pfsid = pci_dev_id(pci_physfn(pdev));	/* [한국어] VF 의 무효화 요청에 PF 의 소스 id 를 실어 준다. 그래야 하드웨어가 PF 단위로 큐 깊이를 가늠할 수 있다. DIT 가 없으면 이 필드는 예약이라 0 이어야 한다 */
			info->ats_qdep = pci_ats_queue_depth(pdev);	/* [한국어] 장치가 한 번에 받을 수 있는 무효화 요청 수. 이보다 많이 보내면 응답이 유실된다 */
		}
		if (sm_supported(iommu)) {	/* [한국어] scalable 모드에서만 PASID/PRI 를 쓸 수 있다 */
			if (pasid_supported(iommu)) {	/* [한국어] 유닛이 PASID 를 지원하면 */
				int features = pci_pasid_features(pdev);	/* [한국어] 장치의 PASID 능력(실행 권한, 특권 모드 지원 여부) */

				if (features >= 0)	/* [한국어] 능력 구조가 있으면 */
					info->pasid_supported = features | 1;	/* [한국어] 비트 0 을 "지원함" 표시로 겸용한다. features 가 0 일 수 있어 그것만으로는 지원 여부를 구분하지 못하기 때문이다 */
			}

			if (info->ats_supported && ecap_prs(iommu->ecap) &&	/* [한국어] ATS 가 되고, 유닛이 페이지 요청을 지원하고 */
			    ecap_pds(iommu->ecap) && pci_pri_supported(pdev))	/* [한국어] 페이지 요청 드레인도 되고, 장치에도 PRI 능력이 있으면 */
				info->pri_supported = 1;	/* [한국어] PRI 를 쓸 수 있다. ATS 가 전제인 이유는 PRI 응답이 ATS 번역 경로로 돌아오기 때문이다 */
		}
	}

	dev_iommu_priv_set(dev, info);	/* [한국어] 장치에 이 정보를 매단다. 이후 모든 콜백이 dev_iommu_priv_get 으로 되찾는다 */
	if (pdev && pci_ats_supported(pdev)) {	/* [한국어] ATS 를 쓸 수 있는 PCI 장치면 */
		pci_prepare_ats(pdev, VTD_PAGE_SHIFT);	/* [한국어] 장치의 ATS 페이지 크기를 4KB 로 맞춘다 */
		ret = device_rbtree_insert(iommu, info);	/* [한국어] 소스 id 색인 트리에 넣는다. ATS/PRI 를 쓰는 장치는 폴트가 소스 id 로 돌아오므로 이 등록이 필요하다 */
		if (ret)	/* [한국어] 중복 등록 등 실패 */
			goto free;	/* [한국어] 장치 정보를 반납하고 나간다 */
	}

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev)) {	/* [한국어] scalable 모드이고 자기 항목을 갖는 장치면 */
		ret = intel_pasid_alloc_table(dev);	/* [한국어] 이 장치 전용 PASID 테이블을 만든다 */
		if (ret) {	/* [한국어] PASID 테이블 할당 실패 */
			dev_err(dev, "PASID table allocation failed\n");	/* [한국어] 실패는 치명적이다 — 이 모드에서는 PASID 테이블 없이 번역을 세울 수 없다 */
			goto clear_rbtree;	/* [한국어] 트리 등록을 되돌린다 */
		}

		if (!context_copied(iommu, info->bus, info->devfn)) {	/* [한국어] 이전 커널에서 컨텍스트를 물려받은 것이 아니라면 */
			ret = intel_pasid_setup_sm_context(dev);	/* [한국어] 지금 컨텍스트 항목을 세운다. 물려받은 경우는 아직 손대지 않고, 도메인을 붙일 때 paging_domain_compatible 이 전환한다 */
			if (ret)	/* [한국어] 실패 */
				goto free_table;	/* [한국어] PASID 테이블을 반납한다 */
		}
	}

	intel_iommu_debugfs_create_dev(info);	/* [한국어] 이 장치의 진단 노드를 만든다 */

	return &iommu->iommu;	/* [한국어] 코어에 "이 장치는 이 유닛이 맡는다"고 알린다 */
free_table:	/* [한국어] 컨텍스트 설정 실패 경로 */
	intel_pasid_free_table(dev);	/* [한국어] PASID 테이블 반납 */
clear_rbtree:	/* [한국어] PASID 테이블 실패가 합류 */
	device_rbtree_remove(info);	/* [한국어] 소스 id 트리에서 제거 */
free:	/* [한국어] 트리 등록 실패가 합류 */
	kfree(info);	/* [한국어] 장치 정보 반납 */

	return ERR_PTR(ret);	/* [한국어] 프로브 실패를 코어에 알린다 */
}

/*
 * [한국어]
 * intel_iommu_probe_finalize - 기본 도메인이 붙은 뒤 PASID/ATS/PRI 를 실제로 켠다
 *
 * @dev: 대상 장치.
 * @return: 없음. 실패해도 조용히 그 기능만 꺼진 채로 진행한다.
 *
 * probe_device 가 "무엇을 켤 수 있는지" 조사했다면, 이 함수는 기본 도메인이
 * 붙은 뒤 실제로 켜는 일을 한다. 코어가 두 단계로 나눈 이유는, 도메인이 붙기
 * 전에 ATS 를 켜면 아직 번역 테이블이 없는 상태에서 장치가 번역을 요청하게
 * 되기 때문이다.
 *
 * 순서가 스펙 요구사항이다: PCIe 스펙은 "ATS 를 켠 뒤에 PASID 를 켜면 장치의
 * 동작이 정의되지 않는다"고 못 박고 있다(위 영어 주석). 그래서 아직 PASID 를
 * 쓸지 모르더라도, 능력이 있으면 무조건 먼저 켜 둔다. 그 다음 ATS, 마지막이
 * PRI 다. release_device 는 정확히 이 역순으로 내린다.
 *
 * cache_tag_assign 이 여기 있는 이유: ATS 를 켰다는 것은 장치가 자기 안에
 * 번역을 캐시하기 시작했다는 뜻이다. 언매핑 때 그 캐시까지 비우려면 무효화
 * 대상 목록에 디바이스 TLB 태그를 등록해야 한다. 등록에 실패하면 ATS 를
 * 도로 끈다 — 무효화할 수 없는 캐시를 켜 두는 것이 훨씬 위험하기 때문이다.
 *
 * 실행 컨텍스트: 장치 프로브 마무리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_probe_device() (기본 도메인 부착 후) → [intel_iommu_probe_finalize]
 *     → pci_enable_pasid() → iommu_enable_pci_ats() → cache_tag_assign()
 *     → iommu_enable_pci_pri()
 */
static void intel_iommu_probe_finalize(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */

	/*
	 * The PCIe spec, in its wisdom, declares that the behaviour of the
	 * device is undefined if you enable PASID support after ATS support.
	 * So always enable PASID support on devices which have it, even if
	 * we can't yet know if we're ever going to use it.
	 */
	if (info->pasid_supported &&	/* [한국어] 장치가 PASID 능력을 갖고 있고 (위 영어 주석) */
	    !pci_enable_pasid(to_pci_dev(dev), info->pasid_supported & ~1))	/* [한국어] 켜는 데 성공하면 (비트 0 은 우리가 붙인 표시라 뺀다) */
		info->pasid_enabled = 1;	/* [한국어] 기록한다. PCIe 스펙상 ATS 를 켠 뒤 PASID 를 켜면 동작이 정의되지 않으므로, 아직 쓸지 모르더라도 항상 PASID 를 먼저 켠다 (위 영어 주석) */

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev)) {	/* [한국어] scalable 모드이고 자기 항목을 갖는 장치면 */
		iommu_enable_pci_ats(info);	/* [한국어] 이제 ATS 를 켠다 — PASID 다음이라는 순서가 중요하다 */
		/* Assign a DEVTLB cache tag to the default domain. */
		if (info->ats_enabled && info->domain) {	/* [한국어] ATS 가 켜졌고 이미 기본 도메인에 붙어 있으면 (위 영어 주석) */
			u16 did = domain_id_iommu(info->domain, iommu);	/* [한국어] 이 유닛에서 그 도메인의 id */

			if (cache_tag_assign(info->domain, did, dev,	/* [한국어] 디바이스 TLB 무효화 대상으로 등록한다. 이 등록이 없으면 언매핑 때 장치 캐시가 남는다 */
					     IOMMU_NO_PASID, CACHE_TAG_DEVTLB))	/* [한국어] PASID 없는 기본 트래픽의 디바이스 TLB 태그 */
				iommu_disable_pci_ats(info);	/* [한국어] 등록에 실패하면 ATS 를 다시 끈다. 무효화할 수 없는 캐시를 켜 두는 것이 더 위험하다 */
		}
	}
	iommu_enable_pci_pri(info);	/* [한국어] 마지막으로 페이지 요청 인터페이스를 켠다. ATS 가 켜진 뒤여야 한다 */
}

/*
 * [한국어]
 * intel_iommu_release_device - 장치를 VT-d 에서 떼어 내고 자원을 반납한다
 *
 * @dev: 떨어져 나가는 장치.
 * @return: 없음.
 *
 * probe_device + probe_finalize 의 역순으로 되돌린다. 순서가 중요한 곳이 셋이다.
 *   1) PRI → ATS → PASID 순으로 끈다. probe_finalize 가 PASID → ATS → PRI
 *      순으로 켰으므로 정확히 역순이다. PRI 를 먼저 끄지 않으면 ATS 를 끈 뒤
 *      도착한 페이지 요청의 응답이 갈 길을 잃는다.
 *   2) 소스 id 트리 제거는 iopf_lock 안에서 한다. 그래야 "이 뒤로는 폴트
 *      처리기가 이 장치를 찾지 못한다"는 순간이 확정된다. 락 없이 지우면
 *      진행 중인 폴트 처리가 해제된 info 를 볼 수 있다.
 *   3) 물려받은 컨텍스트(context_copied)는 내리지 않는다. 그것은 우리가 세운
 *      것이 아니고, 다음 커널이 그 상태를 이어받을 수 있어야 하기 때문이다.
 *
 * 마지막에 info 자체를 kfree 한다. 이 시점 이후 dev_iommu_priv_get 은
 * 유효하지 않으므로, 이 함수 뒤에 이 장치를 참조하는 코드가 남아 있으면
 * 안 된다 — 위 세 순서 규칙이 그것을 보장한다.
 *
 * 실행 컨텍스트: 장치 제거. 프로세스 컨텍스트(mutex 를 잡는다).
 *
 * 호출 체인:
 *   iommu_release_device() → [intel_iommu_release_device]
 *     → iommu_disable_pci_pri()/ats() → device_rbtree_remove()
 *     → intel_pasid_teardown_sm_context() → intel_pasid_free_table()
 */
static void intel_iommu_release_device(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */

	iommu_disable_pci_pri(info);	/* [한국어] 프로브의 역순으로 내린다 — PRI 먼저 */
	iommu_disable_pci_ats(info);	/* [한국어] 그 다음 ATS. 순서를 바꾸면 PRI 응답이 갈 길을 잃는다 */

	if (info->pasid_enabled) {	/* [한국어] PASID 를 켰었으면 */
		pci_disable_pasid(to_pci_dev(dev));	/* [한국어] 마지막으로 끈다 */
		info->pasid_enabled = 0;	/* [한국어] 기록 */
	}

	mutex_lock(&iommu->iopf_lock);	/* [한국어] 폴트 처리 경로와 겹치지 않게 잡는다 */
	if (dev_is_pci(dev) && pci_ats_supported(to_pci_dev(dev)))	/* [한국어] 프로브 때 트리에 넣었던 장치라면 */
		device_rbtree_remove(info);	/* [한국어] 소스 id 색인에서 뺀다. 락 안에서 하는 이유는 이 뒤로 폴트 처리기가 이 장치를 찾지 못하게 하는 순간을 확정하기 위해서다 */
	mutex_unlock(&iommu->iopf_lock);	/* [한국어] 락 해제 */

	if (sm_supported(iommu) && !dev_is_real_dma_subdevice(dev) &&	/* [한국어] scalable 모드이고 자기 항목을 갖는 장치이며 */
	    !context_copied(iommu, info->bus, info->devfn))	/* [한국어] 물려받은 컨텍스트가 아니면 */
		intel_pasid_teardown_sm_context(dev);	/* [한국어] 우리가 세운 컨텍스트를 내린다. 물려받은 것은 손대지 않는다 — 다음 커널이 그 상태를 이어받을 수 있다 */

	intel_pasid_free_table(dev);	/* [한국어] PASID 테이블 반납 */
	intel_iommu_debugfs_remove_dev(info);	/* [한국어] 진단 노드 제거 */
	kfree(info);	/* [한국어] 장치 정보 반납 */
}

/*
 * [한국어]
 * intel_iommu_get_resv_regions - 이 장치에 대해 IOVA 로 써서는 안 되는 주소 범위를 보고한다
 *
 * @device: 대상 장치.
 * @head: 만든 iommu_resv_region 들을 매달 목록(호출자가 준비한다).
 * @return: 없음. 만들 수 있는 만큼만 만들고 돌아간다.
 *
 * 예약 영역이 왜 필요한가: IOVA 할당기는 도메인의 주소 공간을 자유롭게 쓴다고
 * 가정한다. 그런데 실제로는 손대면 안 되는 구간이 있다. 그 구간에 매핑을
 * 만들면 (a) 펌웨어의 DMA 가 엉뚱한 데로 가거나 (b) 인터럽트 메시지가 메모리
 * 쓰기로 오인된다. 이 콜백이 그런 구간을 코어에 알려 준다.
 *
 * 세 종류를 보고한다.
 *   [1] RMRR — 펌웨어가 신고한 예약 구간. 이 장치 자신이나 이 장치가 매달린
 *       브리지에 걸린 항목을 찾는다(브리지의 RMRR 은 그 아래 모든 장치에
 *       적용되므로 is_downstream_to_pci_bridge 로 확인한다).
 *       device_rmrr_is_relaxable 이면 DIRECT_RELAXABLE 로 표시해, 항등 매핑을
 *       강제하지 않고 그 주소만 피하게 한다 — USB/그래픽처럼 부팅 뒤 펌웨어가
 *       손을 떼는 장치가 여기 해당한다.
 *   [2] 플로피 우회(CONFIG_INTEL_IOMMU_FLOPPY_WA) — ISA 브리지 아래의 레거시
 *       DMA 는 하위 16MB 만 다룰 수 있어 그 범위를 통째로 예약한다.
 *   [3] IOAPIC 범위 — 이 주소로 가는 것은 DMA 가 아니라 인터럽트 메시지다.
 *       IOMMU 가 그것을 메모리 쓰기로 번역하면 인터럽트가 사라진다.
 *       IOMMU_RESV_MSI 로 표시해 코어가 다르게 다루게 한다.
 *
 * 할당 플래그가 두 가지인 이유: RMRR 순회는 rcu_read_lock 안이라 잠들 수 없어
 * GFP_ATOMIC 을, 그 밖은 GFP_KERNEL 을 쓴다.
 * 실행 컨텍스트: 장치가 그룹에 들어갈 때. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_get_resv_regions() → [intel_iommu_get_resv_regions]
 *     → device_rmrr_is_relaxable() → iommu_alloc_resv_region()
 */
static void intel_iommu_get_resv_regions(struct device *device,
					 struct list_head *head)
{
	int prot = DMA_PTE_READ | DMA_PTE_WRITE;	/* [한국어] RMRR 구간은 읽기/쓰기 모두 허용해야 한다. 펌웨어가 어느 쪽으로 쓰는지 알 수 없기 때문이다 */
	struct iommu_resv_region *reg;	/* [한국어] 만들 예약 영역 */
	struct dmar_rmrr_unit *rmrr;	/* [한국어] RMRR 항목 순회 커서 */
	struct device *i_dev;	/* [한국어] device scope 순회 커서 */
	int i;	/* [한국어] 인덱스 */

	rcu_read_lock();	/* [한국어] RMRR 목록 순회 보호 */
	for_each_rmrr_units(rmrr) {	/* [한국어] 신고된 예약 구간마다 */
		for_each_active_dev_scope(rmrr->devices, rmrr->devices_cnt,	/* [한국어] 그 구간이 적용되는 장치들을 훑으며 */
					  i, i_dev) {	/* [한국어] 순회 */
			struct iommu_resv_region *resv;	/* [한국어] 만들 항목 */
			enum iommu_resv_type type;	/* [한국어] 예약의 성격 */
			size_t length;	/* [한국어] 구간 길이 */

			if (i_dev != device &&	/* [한국어] 우리가 묻는 장치가 아니고 */
			    !is_downstream_to_pci_bridge(device, i_dev))	/* [한국어] 그 장치 아래에 매달린 것도 아니면 */
				continue;	/* [한국어] 무관한 항목이다. 브리지 아래까지 보는 이유는 브리지에 걸린 RMRR 이 그 아래 모든 장치에 적용되기 때문이다 */

			length = rmrr->end_address - rmrr->base_address + 1;	/* [한국어] 닫힌 구간이라 +1 */

			type = device_rmrr_is_relaxable(device) ?	/* [한국어] 이 장치의 RMRR 을 나중에 풀어도 되는가 */
				IOMMU_RESV_DIRECT_RELAXABLE : IOMMU_RESV_DIRECT;	/* [한국어] USB/그래픽처럼 부팅 뒤 펌웨어가 손을 떼는 장치는 RELAXABLE 로 표시해, 항등 매핑을 강제하지 않고 주소만 피하게 한다 */

			resv = iommu_alloc_resv_region(rmrr->base_address,	/* [한국어] 예약 영역 객체를 만든다 */
						       length, prot, type,	/* [한국어] 길이, 권한, 성격 */
						       GFP_ATOMIC);	/* [한국어] RCU 순회 안이라 잠들 수 없다 */
			if (!resv)	/* [한국어] 할당 실패 */
				break;	/* [한국어] 더 만들지 못한다. 여기까지 만든 것은 그대로 두고 나간다 */

			list_add_tail(&resv->list, head);	/* [한국어] 호출자의 목록에 붙인다 */
		}
	}
	rcu_read_unlock();	/* [한국어] 순회 끝 */

#ifdef CONFIG_INTEL_IOMMU_FLOPPY_WA	/* [한국어] 플로피 컨트롤러 우회를 켠 빌드에서만 */
	if (dev_is_pci(device)) {	/* [한국어] 플로피 우회가 켜진 빌드에서 PCI 장치라면 */
		struct pci_dev *pdev = to_pci_dev(device);	/* [한국어] PCI 장치로 */

		if ((pdev->class >> 8) == PCI_CLASS_BRIDGE_ISA) {	/* [한국어] ISA 브리지면 */
			reg = iommu_alloc_resv_region(0, 1UL << 24, prot,	/* [한국어] 하위 16MB 를 통째로 예약한다. 플로피 컨트롤러의 레거시 DMA 가 그 범위만 쓸 수 있어서다 */
					IOMMU_RESV_DIRECT_RELAXABLE,	/* [한국어] 필요하면 풀 수 있는 예약으로 표시 */
					GFP_KERNEL);	/* [한국어] 여기는 RCU 밖이라 잠들 수 있다 */
			if (reg)	/* [한국어] 만들어졌으면 */
				list_add_tail(&reg->list, head);	/* [한국어] 목록에 붙인다 */
		}
	}
#endif /* CONFIG_INTEL_IOMMU_FLOPPY_WA */

	reg = iommu_alloc_resv_region(IOAPIC_RANGE_START,	/* [한국어] IOAPIC 인터럽트 주소 범위를 예약한다 */
				      IOAPIC_RANGE_END - IOAPIC_RANGE_START + 1,	/* [한국어] 그 길이 */
				      0, IOMMU_RESV_MSI, GFP_KERNEL);	/* [한국어] MSI 형 예약이라 권한은 0. 이 범위는 DMA 가 아니라 인터럽트 메시지가 가는 곳이라, IOVA 할당기가 여기를 쓰면 인터럽트가 메모리 쓰기로 오인된다 */
	if (!reg)	/* [한국어] 할당 실패 */
		return;	/* [한국어] 지금까지 만든 것만 남긴다 */
	list_add_tail(&reg->list, head);	/* [한국어] 목록에 붙인다 */
}

/*
 * [한국어]
 * intel_iommu_device_group - 이 장치가 속할 IOMMU 그룹을 정한다
 *
 * @dev: 대상 장치.
 * @return: 이 장치가 들어갈 iommu_group(참조를 잡은 상태로 반환된다).
 *
 * 그룹이란: 하드웨어적으로 서로 분리할 수 없는 장치들의 묶음이다. 예를 들어
 * ACS(Access Control Services)가 없는 스위치 아래의 장치들은 서로의 트래픽을
 * IOMMU 를 거치지 않고 주고받을 수 있으므로, 하나를 격리해도 의미가 없다.
 * 그래서 코어는 그룹 단위로만 도메인을 붙인다.
 *
 * PCI 장치는 pci_device_group 이 PCIe 토폴로지와 ACS 설정, requester id
 * 별칭까지 보고 판단한다. 이 드라이버가 그 판단에 더할 것이 없어 코어 헬퍼를
 * 그대로 쓴다. PCI 가 아닌 장치(ACPI 네임스페이스 장치 등)는 그런 우회 경로가
 * 없으므로 generic_device_group 으로 하나씩 자기 그룹을 갖는다.
 *
 * 실행 컨텍스트: 장치 프로브. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_group_get_for_dev() → [intel_iommu_device_group]
 */
static struct iommu_group *intel_iommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))	/* [한국어] PCI 장치는 */
		return pci_device_group(dev);	/* [한국어] PCIe 토폴로지와 ACS 설정을 보고 그룹을 정한다. 하드웨어적으로 분리할 수 없는 장치들이 한 그룹이 된다 */
	return generic_device_group(dev);	/* [한국어] 그 밖의 장치는 하나씩 자기 그룹을 갖는다 */
}

/*
 * [한국어]
 * intel_iommu_enable_iopf - 이 장치의 I/O 페이지 폴트 처리를 켠다(참조 계수 방식)
 *
 * @dev: 대상 장치.
 * @return: 0 성공, -ENODEV 면 PRI 가 켜져 있지 않아 폴트를 받을 방법이 없다.
 *
 * I/O 페이지 폴트란: SVA 처럼 매핑을 미리 다 만들어 두지 않는 방식에서는,
 * 장치가 아직 매핑되지 않은 주소에 접근하면 IOMMU 가 그것을 폴트로 보고하고
 * 커널이 페이지를 채운 뒤 장치에 "다시 시도하라"고 답한다. PRI(Page Request
 * Interface)가 그 하드웨어 통로이고, iopf 큐가 그 소프트웨어 처리다.
 *
 * 참조 계수가 필요한 이유: 한 장치를 SVA 와 iommufd 가 동시에 쓸 수 있고,
 * 둘 다 폴트 처리를 요구한다. 먼저 끈 쪽 때문에 다른 쪽의 폴트 처리가
 * 사라지면 그쪽 장치는 영원히 멈춘다(응답 없는 페이지 요청은 장치를 정지시킨다).
 * 그래서 마지막 사용자가 놓을 때까지 유지한다.
 *
 * 동기화: pri_enabled 와 iopf_refcount 는 그룹 뮤텍스가 지킨다. 이 함수는 그
 * 락을 잡지 않고 iommu_group_mutex_assert 로 호출자가 쥐고 있음을 확인만
 * 한다 — 켜고 끄는 결정이 그룹 단위로 일어나기 때문이다.
 *
 * 실행 컨텍스트: SVA/iommufd 설정 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_dev_enable_feature(IOMMU_DEV_FEAT_IOPF)/SVA 설정 → [이 함수]
 *     → iopf_queue_add_device()
 */
int intel_iommu_enable_iopf(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	int ret;	/* [한국어] 결과 */

	if (!info->pri_enabled)	/* [한국어] PRI 가 켜져 있지 않으면 */
		return -ENODEV;	/* [한국어] 페이지 폴트를 받을 방법이 없다 */

	/* pri_enabled is protected by the group mutex. */
	iommu_group_mutex_assert(dev);	/* [한국어] pri_enabled 와 참조 계수를 그룹 뮤텍스가 지킨다 (위 영어 주석) */
	if (info->iopf_refcount) {	/* [한국어] 이미 켜져 있으면 */
		info->iopf_refcount++;	/* [한국어] 참조만 하나 늘린다. 같은 장치를 SVA 와 iommufd 가 동시에 쓸 수 있어 참조 계수가 필요하다 */
		return 0;	/* [한국어] 성공 */
	}

	ret = iopf_queue_add_device(iommu->iopf_queue, dev);	/* [한국어] 이 유닛의 폴트 큐에 장치를 등록한다 */
	if (ret)	/* [한국어] 실패 */
		return ret;	/* [한국어] 전달 */

	info->iopf_refcount = 1;	/* [한국어] 첫 사용자 */

	return 0;	/* [한국어] 이제 이 장치의 페이지 폴트가 처리된다 */
}

/*
 * [한국어]
 * intel_iommu_disable_iopf - I/O 페이지 폴트 처리의 참조를 하나 놓는다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * intel_iommu_enable_iopf 의 짝. 참조 계수를 하나 줄이고, 0 이 되면 그제서야
 * 유닛의 폴트 큐에서 장치를 뺀다. 마지막 사용자가 아닐 때 큐에서 빼면 남은
 * 사용자의 페이지 요청이 응답을 받지 못하고, 응답 없는 PRI 요청은 장치를
 * 영원히 멈춰 세운다 — 참조 계수가 있는 이유가 그것이다.
 *
 * 켠 적 없는데 부르면 WARN_ON 으로 스택을 남긴다. 참조 계수의 짝이 맞지 않는
 * 것은 호출자(SVA 또는 iommufd)의 버그이며, 조용히 넘기면 나중에 엉뚱한
 * 장치가 멈춘다.
 *
 * 동기화: 참조 계수는 그룹 뮤텍스가 지킨다. 이 함수는 잡지 않고 호출자가
 * 쥐고 있음을 assert 로 확인만 한다.
 * 실행 컨텍스트: SVA/iommufd 해제 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_dev_disable_feature()/SVA 해제 → [intel_iommu_disable_iopf]
 *     → iopf_queue_remove_device()
 */
void intel_iommu_disable_iopf(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */

	if (WARN_ON(!info->pri_enabled || !info->iopf_refcount))	/* [한국어] 켠 적 없는데 끄려 하면 호출자 버그다 */
		return;	/* [한국어] 아무것도 하지 않는다 */

	iommu_group_mutex_assert(dev);	/* [한국어] 참조 계수는 그룹 뮤텍스가 지킨다 */
	if (--info->iopf_refcount)	/* [한국어] 아직 다른 사용자가 남아 있으면 */
		return;	/* [한국어] 폴트 처리를 유지한다. 여기서 끄면 남은 사용자의 장치가 응답 없는 페이지 요청으로 멈춘다 */

	iopf_queue_remove_device(iommu->iopf_queue, dev);	/* [한국어] 마지막 사용자가 놓았으므로 큐에서 뺀다 */
}

/*
 * [한국어]
 * intel_iommu_is_attach_deferred - 기본 도메인 부착을 미뤄야 하는 장치인지 답한다
 *
 * @dev: 대상 장치.
 * @return: true 면 코어가 지금 기본 도메인을 붙이지 말고 미뤄야 한다.
 *
 * 왜 미루는가: kdump 커널처럼 이전 커널이 켜 둔 번역을 그대로 물려받은
 * 상태에서는, 장치들이 여전히 옛 페이지 테이블을 통해 DMA 를 하고 있다.
 * 여기서 코어가 평소처럼 기본 도메인을 붙이면 그 순간 컨텍스트 항목이 새
 * 테이블을 가리키게 되고, 진행 중이던 DMA 가 매핑되지 않은 주소로 향한다.
 * 크래시 덤프를 쓰는 디스크 컨트롤러가 그렇게 멈추면 덤프 자체를 못 쓴다.
 *
 * 그래서 "물려받은 유닛이고(translation_pre_enabled) 아직 도메인이 붙지
 * 않았다(!info->domain)"면 미룬다. 실제 드라이버가 로드되어 DMA 매핑을
 * 요구하는 시점에 비로소 도메인이 붙고, 그때 물려받은 상태에서 우리 상태로
 * 전환된다. 그 전환을 실제로 수행하는 것은 paging_domain_compatible 이다.
 *
 * 실행 컨텍스트: 코어가 기본 도메인을 붙이려 할 때. 순수 조회.
 *
 * 호출 체인:
 *   iommu_ops.is_attach_deferred → [intel_iommu_is_attach_deferred]
 *     → translation_pre_enabled()
 */
static bool intel_iommu_is_attach_deferred(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */

	return translation_pre_enabled(info->iommu) && !info->domain;	/* [한국어] 이전 커널/펌웨어가 켜 둔 번역을 인계받은 유닛이고 아직 도메인이 붙지 않았으면, 기본 도메인 부착을 미룬다. 지금 붙이면 물려받은 테이블이 끊겨 장치 DMA 가 중단되기 때문이다. 실제 드라이버가 도메인을 요구할 때 비로소 전환한다 */
}

/*
 * Check that the device does not live on an external facing PCI port that is
 * marked as untrusted. Such devices should not be able to apply quirks and
 * thus not be able to bypass the IOMMU restrictions.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * risky_device - 하드웨어 우회(quirk)를 적용해도 되는 장치인지 판단한다
 *
 * @pdev: 검사할 PCI 장치.
 * @return: true 면 위험하므로 우회를 적용하지 않는다.
 *
 * 왜 이 검사가 보안 문제인가: 이 파일 뒤쪽의 quirk 들(quirk_iommu_igfx,
 * quirk_iommu_rwbf 등)은 특정 벤더/디바이스 ID 를 보고 IOMMU 동작을 완화한다.
 * 그런데 PCI ID 는 장치가 스스로 보고하는 값이라 위조할 수 있다. 외부 포트에
 * 꽂은 악성 장치가 우회 대상 ID 를 흉내 내면, 그 완화를 얻어 IOMMU 격리를
 * 벗어날 수 있다.
 *
 * 그래서 untrusted 로 표시된 장치(Thunderbolt/USB4 같은 외부 포트 뒤)에는
 * 어떤 우회도 적용하지 않는다. 정품 장치가 우연히 그런 포트에 꽂혀 동작이
 * 나빠질 수는 있지만, 그쪽이 격리가 뚫리는 것보다 낫다.
 *
 * 로그를 두 줄 남기는 이유: 첫 줄은 어떤 장치에 우회를 건너뛰었는지 알리고,
 * 둘째 줄은 관리자에게 펌웨어 벤더에 문의하라고 안내한다. 외부 포트 장치가
 * 우회 대상 ID 를 갖고 있다는 것 자체가 정상적인 구성이 아니기 때문이다.
 *
 * 실행 컨텍스트: PCI quirk 적용 시점(부팅/장치 등장). 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   quirk_iommu_igfx()/quirk_iommu_rwbf()/... → [risky_device]
 */
static bool risky_device(struct pci_dev *pdev)
{
	if (pdev->untrusted) {	/* [한국어] 외부 포트 뒤의 신뢰할 수 없는 장치면 (위 영어 주석) */
		pci_info(pdev,	/* [한국어] 우회를 적용하지 않았음을 알린다 */
			 "Skipping IOMMU quirk for dev [%04X:%04X] on untrusted PCI link\n",
			 pdev->vendor, pdev->device);	/* [한국어] 어떤 장치였는지 */
		pci_info(pdev, "Please check with your BIOS/Platform vendor about this\n");	/* [한국어] 펌웨어가 외부 포트 장치에 우회 대상 ID 를 붙인 것 자체가 의심스럽다 — 공격자가 ID 를 위조해 IOMMU 제한을 벗어나려는 시도일 수 있다 */
		return true;	/* [한국어] 우회를 적용하지 않는다 */
	}
	return false;	/* [한국어] 내부 장치라면 우회를 적용해도 된다 */
}

/*
 * [한국어]
 * intel_iommu_iotlb_sync_map - 새로 만든 매핑을 하드웨어에 보이게 한다
 *
 * @domain: 매핑이 추가된 도메인.
 * @iova: 새 매핑의 시작 주소. @size: 그 크기.
 * @return: 항상 0.
 *
 * 보통의 IOMMU 는 매핑을 "만들 때"는 무효화가 필요 없다. 없던 항목이 생긴
 * 것뿐이고, 하드웨어는 "없음"을 캐시하지 않기 때문이다. 무효화는 매핑을
 * 지울 때만 필요하다.
 *
 * 그런데 두 종류의 VT-d 하드웨어는 다르다.
 *   - rwbf 가 필요한 옛 유닛: 내부 쓰기 버퍼를 명시적으로 비우지 않으면
 *     우리가 메모리에 쓴 페이지 테이블 항목이 하드웨어에 보이지 않는다.
 *   - caching mode(= 에뮬레이션된 IOMMU): "여기엔 매핑이 없다"는 사실까지
 *     캐시한다. 그래서 새 매핑을 만들어도 캐시된 "없음"이 남아 있으면
 *     장치가 계속 폴트를 받는다.
 * 두 경우 모두 도메인 생성 때 iotlb_sync_map 으로 표시해 두었고, 이 콜백이
 * 그 표시를 보고 필요한 유닛에서만 무효화를 보낸다.
 *
 * _np 접미사: non-present, 즉 "없음" 항목의 캐시를 지우는 것이 목적임을
 * 뜻한다. 이미 있던 매핑을 바꾼 것이 아니므로 더 무거운 무효화는 필요 없다.
 *
 * 실행 컨텍스트: iommu_map() 뒤. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_map()/iommu_map_sg() → [intel_iommu_iotlb_sync_map]
 *     → cache_tag_flush_range_np()
 */
static int intel_iommu_iotlb_sync_map(struct iommu_domain *domain,
				      unsigned long iova, size_t size)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */

	if (dmar_domain->iotlb_sync_map)	/* [한국어] 이 도메인이 "매핑 후에도 동기화가 필요"로 표시되어 있으면 */
		cache_tag_flush_range_np(dmar_domain, iova, iova + size - 1);	/* [한국어] 새로 만든 범위를 무효화한다. np(non-present)는 "없음" 항목의 캐시를 지운다는 뜻으로, 캐싱 모드 하드웨어가 "여기엔 매핑이 없다"를 캐시해 둔 것을 지워야 새 매핑이 보인다 */

	return 0;	/* [한국어] 보통의 하드웨어에서는 아무것도 하지 않고 성공 */
}

/*
 * [한국어]
 * domain_remove_dev_pasid - 도메인에서 (장치, PASID) 기록을 지우고 참조를 놓는다
 *
 * @domain: 떼어 낼 도메인. NULL 이면 아무것도 하지 않는다.
 * @dev: 대상 장치. @pasid: 대상 PASID.
 * @return: 없음.
 *
 * PASID 는 한 장치 안에서 여러 주소 공간을 구분하는 번호다. 그래서 도메인은
 * 붙어 있는 장치 목록(devices)과 별도로, (장치, PASID) 쌍의 목록(dev_pasids)을
 * 유지한다. 이 함수는 그중 하나를 지운다.
 *
 * 항등 도메인을 건너뛰는 이유: 항등 도메인은 IOVA 를 그대로 물리 주소로
 * 쓰므로 도메인 id 도, 무효화 대상 등록도, PASID 별 메타데이터도 없다
 * (위 영어 주석). 지울 것이 애초에 없다.
 *
 * 순서: 목록에서 먼저 빼고(락 안), 락 밖에서 무효화 등록 해제와 도메인 id
 * 참조 반납을 한다. 뒤쪽 두 작업은 잠들 수 있어 스핀락 안에서 할 수 없다.
 *
 * WARN_ON_ONCE(!dev_pasid): 목록에 없었다는 것은 코어와 이 드라이버의 상태가
 * 어긋났다는 뜻이다. 해제만 건너뛰고 나머지는 그대로 진행한다.
 *
 * 실행 컨텍스트: PASID 분리. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   blocking_domain_set_dev_pasid()/intel_iommu_set_dev_pasid()
 *     → [domain_remove_dev_pasid]
 *     → cache_tag_unassign_domain() → domain_detach_iommu()
 */
void domain_remove_dev_pasid(struct iommu_domain *domain,
			     struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct dev_pasid_info *curr, *dev_pasid = NULL;	/* [한국어] 순회 커서와 찾은 항목 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct dmar_domain *dmar_domain;	/* [한국어] VT-d 도메인 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	if (!domain)	/* [한국어] 도메인이 없으면 */
		return;	/* [한국어] 정리할 것이 없다 */

	/* Identity domain has no meta data for pasid. */
	if (domain->type == IOMMU_DOMAIN_IDENTITY)	/* [한국어] 항등 도메인은 PASID 별 메타데이터를 두지 않는다 (위 영어 주석) */
		return;	/* [한국어] 정리할 것이 없다 */

	dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	spin_lock_irqsave(&dmar_domain->lock, flags);	/* [한국어] dev_pasids 목록을 보호한다 */
	list_for_each_entry(curr, &dmar_domain->dev_pasids, link_domain) {	/* [한국어] 이 도메인에 붙은 (장치, PASID) 쌍들을 훑으며 */
		if (curr->dev == dev && curr->pasid == pasid) {	/* [한국어] 찾는 쌍이면 */
			list_del(&curr->link_domain);	/* [한국어] 목록에서 뺀다 */
			dev_pasid = curr;	/* [한국어] 나중에 해제하려고 기억해 둔다 */
			break;	/* [한국어] 하나뿐이므로 종료 */
		}
	}
	spin_unlock_irqrestore(&dmar_domain->lock, flags);	/* [한국어] 락 해제. 아래 작업들은 잠들 수 있어 락 밖에서 한다 */

	cache_tag_unassign_domain(dmar_domain, dev, pasid);	/* [한국어] 이 PASID 의 무효화 대상 등록을 푼다 */
	domain_detach_iommu(dmar_domain, iommu);	/* [한국어] 유닛의 도메인 id 참조를 하나 놓는다 */
	if (!WARN_ON_ONCE(!dev_pasid)) {	/* [한국어] 목록에 없었다면 코어와 상태가 어긋난 것이다 */
		intel_iommu_debugfs_remove_dev_pasid(dev_pasid);	/* [한국어] 진단 노드 제거 */
		kfree(dev_pasid);	/* [한국어] 자료구조 반납 */
	}
}

/*
 * [한국어]
 * blocking_domain_set_dev_pasid - 특정 PASID 의 DMA 만 차단한다
 *
 * @domain: 전역 blocking_domain(상태가 없어 쓰이지 않는다).
 * @dev: 대상 장치. @pasid: 차단할 PASID.
 * @old: 이 PASID 가 쓰고 있던 도메인.
 * @return: 항상 0 — 차단은 실패할 수 없다.
 *
 * 장치 전체가 아니라 그 안의 한 주소 공간만 막는다. SVA 로 붙였던 프로세스가
 * 죽거나, iommufd 가 PASID 를 회수할 때 불린다.
 *
 * 순서는 blocking_domain_attach_dev 와 같은 원칙이다.
 *   1) PASID 항목을 내려 하드웨어 번역을 끊는다(false = 폴트를 유발하지 말고
 *      조용히 차단).
 *   2) 옛 도메인의 폴트 처리를 뗀다.
 *   3) 옛 도메인의 (장치, PASID) 기록을 지운다.
 * 하드웨어를 먼저 끊는 것이 중요하다 — 소프트웨어 기록을 먼저 지우면 그 사이
 * 도착한 폴트가 참조할 곳을 잃는다.
 *
 * 이 함수는 전방 선언(파일 앞쪽 blocking_domain 정의 위)의 실제 구현이다.
 * blocking_domain 이 이 함수를 참조하고 이 함수가 그 위의 헬퍼들을 쓰기
 * 때문에 순환을 끊으려고 선언과 정의를 떼어 놓았다.
 *
 * 실행 컨텍스트: PASID 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_remove_dev_pasid()/SVA 해제 → [blocking_domain_set_dev_pasid]
 *     → intel_pasid_tear_down_entry() → domain_remove_dev_pasid()
 */
static int blocking_domain_set_dev_pasid(struct iommu_domain *domain,
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */

	intel_pasid_tear_down_entry(info->iommu, dev, pasid, false);	/* [한국어] 이 PASID 의 항목을 내려 해당 주소 공간의 DMA 를 막는다. false 는 조용히 차단하라는 뜻이다 */
	iopf_for_domain_remove(old, dev);	/* [한국어] 옛 도메인의 폴트 처리를 뗀다 */
	domain_remove_dev_pasid(old, dev, pasid);	/* [한국어] 옛 도메인의 (장치, PASID) 기록을 지운다 */

	return 0;	/* [한국어] 차단은 실패할 수 없다 */
}

/*
 * [한국어]
 * domain_add_dev_pasid - 도메인에 (장치, PASID) 기록을 만들고 필요한 참조를 잡는다
 *
 * @domain: 붙일 도메인. @dev: 장치. @pasid: PASID.
 * @return: 만들어진 dev_pasid_info, 실패 시 ERR_PTR.
 *
 * 하드웨어 항목을 세우기 전에 필요한 것들을 미리 확보한다. 순서와 되돌리기가
 * 이 함수의 전부다.
 *   1) 기록 구조체 할당.
 *   2) domain_attach_iommu — 이 유닛에서 도메인 id 를 확보한다. 이미 이
 *      도메인이 이 유닛을 쓰고 있으면 참조만 늘어난다. 도메인 id 는 유닛마다
 *      개수가 정해져 있어(cap_ndoms) 여기서 실패할 수 있다.
 *   3) cache_tag_assign_domain — 이 PASID 를 무효화 대상 목록에 넣는다.
 *      이것이 없으면 나중에 매핑을 풀어도 이 PASID 의 캐시가 남는다.
 *   4) 도메인의 dev_pasids 목록에 매단다.
 * 2~3 이 실패하면 out_detach_iommu → out_free 로 역순으로 되돌린다.
 *
 * 왜 하드웨어 설정과 분리했는가: 호출자(intel_iommu_set_dev_pasid)는 하드웨어
 * 설정이 실패했을 때 이 준비 작업까지 깔끔히 되돌려야 한다. 준비와 설정을
 * 나눠 두면 되돌리기 경로가 단순해진다.
 *
 * 실행 컨텍스트: PASID 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_set_dev_pasid()/SVA 설정 → [domain_add_dev_pasid]
 *     → domain_attach_iommu() → cache_tag_assign_domain()
 */
struct dev_pasid_info *
domain_add_dev_pasid(struct iommu_domain *domain,
		     struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct dev_pasid_info *dev_pasid;	/* [한국어] 만들 (장치, PASID) 기록 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int ret;	/* [한국어] 각 단계 결과 */

	dev_pasid = kzalloc_obj(*dev_pasid);	/* [한국어] 기록 구조체 */
	if (!dev_pasid)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 전달 */

	ret = domain_attach_iommu(dmar_domain, iommu);	/* [한국어] 이 유닛에서 도메인 id 를 확보한다(이미 있으면 참조만 늘린다) */
	if (ret)	/* [한국어] 도메인 id 고갈 등 */
		goto out_free;	/* [한국어] 기록을 반납하고 나간다 */

	ret = cache_tag_assign_domain(dmar_domain, dev, pasid);	/* [한국어] 이 PASID 를 무효화 대상으로 등록한다 */
	if (ret)	/* [한국어] 실패 */
		goto out_detach_iommu;	/* [한국어] 도메인 id 참조를 되돌린다 */

	dev_pasid->dev = dev;	/* [한국어] 어느 장치의 */
	dev_pasid->pasid = pasid;	/* [한국어] 어느 PASID 인지 */
	spin_lock_irqsave(&dmar_domain->lock, flags);	/* [한국어] 도메인의 dev_pasids 목록을 바꾼다 */
	list_add(&dev_pasid->link_domain, &dmar_domain->dev_pasids);	/* [한국어] 도메인에 매단다 */
	spin_unlock_irqrestore(&dmar_domain->lock, flags);	/* [한국어] 락 해제 */

	return dev_pasid;	/* [한국어] 호출자가 이 포인터로 나중에 정리한다 */
out_detach_iommu:	/* [한국어] cache tag 실패 경로 */
	domain_detach_iommu(dmar_domain, iommu);	/* [한국어] 도메인 id 참조를 놓는다 */
out_free:	/* [한국어] 도메인 id 실패가 합류 */
	kfree(dev_pasid);	/* [한국어] 기록 반납 */
	return ERR_PTR(ret);	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * intel_iommu_set_dev_pasid - 장치의 특정 PASID 에 페이징 도메인을 붙인다
 *
 * @domain: 붙일 페이징 도메인. @dev: 장치. @pasid: 대상 PASID.
 * @old: 이 PASID 가 쓰고 있던 도메인(교체인 경우). 없으면 NULL.
 * @return: 0 성공, 음수면 실패(그 경우 옛 상태가 그대로 유지된다).
 *
 * PASID 단위 부착이 왜 필요한가: 하나의 장치가 여러 프로세스/컨텍스트를
 * 동시에 다루는 경우(GPU, 가속기, SR-IOV 서브펑션), 각각에 다른 주소 공간을
 * 주어야 한다. PASID 가 그 구분자이고, 이 함수가 PASID 하나에 도메인 하나를
 * 연결한다.
 *
 * 거절하는 경우들:
 *   - 페이징 도메인이 아니면. SVA/항등 도메인은 각자의 경로가 따로 있다.
 *   - 유닛이 PASID 를 못 하거나, 부모의 컨텍스트 항목을 공유하는
 *     서브디바이스면. 후자는 자기 PASID 테이블이 없다.
 *   - 물려받은 컨텍스트가 아직 전환되지 않았으면(-EBUSY). 기본 도메인이 먼저
 *     붙어 전환이 일어나야 그 위에 PASID 를 얹을 수 있다.
 *
 * 교체(replace)의 순서가 이 함수의 핵심이다.
 *   1) 새 도메인의 준비물을 먼저 확보한다(domain_add_dev_pasid).
 *   2) 폴트 처리를 옛 도메인에서 새 도메인으로 옮긴다. 하드웨어를 바꾸기
 *      전에 해야, 전환 직후 도착하는 폴트가 새 도메인으로 간다.
 *   3) 하드웨어 PASID 항목을 원자적으로 교체한다(old 를 함께 넘겨
 *      domain_setup_*_level 이 한 번에 바꾼다).
 *   4) 성공한 뒤에야 옛 도메인의 기록을 지운다.
 * 실패하면 out_unwind_iopf → out_remove_dev_pasid 로 정확히 역순으로
 * 되돌리므로, 어느 지점에서 실패해도 옛 도메인이 그대로 살아 있다.
 *
 * 실행 컨텍스트: PASID 부착/교체. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_attach_device_pasid()/iommufd → [intel_iommu_set_dev_pasid]
 *     → paging_domain_compatible() → domain_add_dev_pasid()
 *     → iopf_for_domain_replace()
 *     → domain_setup_first_level()/domain_setup_second_level()
 */
static int intel_iommu_set_dev_pasid(struct iommu_domain *domain,
				     struct device *dev, ioasid_t pasid,
				     struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct dev_pasid_info *dev_pasid;	/* [한국어] 만들 (장치, PASID) 기록 */
	int ret;	/* [한국어] 각 단계 결과 */

	if (WARN_ON_ONCE(!(domain->type & __IOMMU_DOMAIN_PAGING)))	/* [한국어] 페이징 도메인이 아니면 */
		return -EINVAL;	/* [한국어] PASID 에 붙일 수 없다. SVA/항등 도메인은 각자의 경로가 따로 있다 */

	if (!pasid_supported(iommu) || dev_is_real_dma_subdevice(dev))	/* [한국어] 유닛이 PASID 를 못 하거나, 부모의 항목을 공유하는 서브디바이스면 */
		return -EOPNOTSUPP;	/* [한국어] PASID 단위로 도메인을 붙일 수 없다 */

	if (context_copied(iommu, info->bus, info->devfn))	/* [한국어] 물려받은 컨텍스트가 아직 전환되지 않았으면 */
		return -EBUSY;	/* [한국어] 그 위에 PASID 를 얹을 수 없다. 기본 도메인이 먼저 붙어 전환이 일어나야 한다 */

	ret = paging_domain_compatible(domain, dev);	/* [한국어] 이 유닛에 이 도메인을 붙일 수 있는지 */
	if (ret)	/* [한국어] 불가능하면 */
		return ret;	/* [한국어] 이유를 전달 */

	dev_pasid = domain_add_dev_pasid(domain, dev, pasid);	/* [한국어] 도메인 id 확보 + 무효화 등록 + 기록 생성 */
	if (IS_ERR(dev_pasid))	/* [한국어] 실패 */
		return PTR_ERR(dev_pasid);	/* [한국어] 전달 */

	ret = iopf_for_domain_replace(domain, old, dev);	/* [한국어] 폴트 처리를 옛 도메인에서 새 도메인으로 옮긴다. 하드웨어를 바꾸기 전에 해야 그 사이의 폴트가 새 도메인으로 간다 */
	if (ret)	/* [한국어] 실패 */
		goto out_remove_dev_pasid;	/* [한국어] 방금 만든 기록을 되돌린다 */

	if (intel_domain_is_fs_paging(dmar_domain))	/* [한국어] 1단계 도메인이면 */
		ret = domain_setup_first_level(iommu, dmar_domain,	/* [한국어] PASID 항목에 1단계 테이블을 세운다 */
					       dev, pasid, old);	/* [한국어] 옛 도메인을 함께 넘겨 원자적으로 교체하게 한다 */
	else if (intel_domain_is_ss_paging(dmar_domain))	/* [한국어] 2단계 도메인이면 */
		ret = domain_setup_second_level(iommu, dmar_domain,	/* [한국어] PASID 항목에 2단계 테이블을 세운다 */
						dev, pasid, old);	/* [한국어] 마찬가지로 교체 */
	else if (WARN_ON(true))	/* [한국어] 둘 다 아니면 위 검사를 통과했을 리 없다 */
		ret = -EINVAL;	/* [한국어] 거절 */

	if (ret)	/* [한국어] 하드웨어 설정 실패 */
		goto out_unwind_iopf;	/* [한국어] 폴트 처리를 되돌린다 */

	domain_remove_dev_pasid(old, dev, pasid);	/* [한국어] 새 설정이 자리 잡은 뒤에야 옛 도메인의 기록을 지운다. 순서를 바꾸면 교체 도중 폴트가 갈 곳이 없어진다 */

	intel_iommu_debugfs_create_dev_pasid(dev_pasid);	/* [한국어] 진단 노드 생성 */

	return 0;	/* [한국어] PASID 가 새 도메인을 쓴다 */

out_unwind_iopf:	/* [한국어] 하드웨어 설정 실패 경로 */
	iopf_for_domain_replace(old, domain, dev);	/* [한국어] 폴트 처리를 옛 도메인으로 되돌린다 */
out_remove_dev_pasid:	/* [한국어] 폴트 처리 실패가 합류 */
	domain_remove_dev_pasid(domain, dev, pasid);	/* [한국어] 새 도메인의 기록을 지운다 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * intel_iommu_hw_info - 유저스페이스(iommufd)에 이 유닛의 하드웨어 정보를 넘긴다
 *
 * @dev: 어느 장치를 통해 묻는지. 그 장치를 맡은 유닛의 정보를 답한다.
 * @length: 출력. 채운 구조체의 크기.
 * @type: 입출력. 들어올 때는 사용자가 원하는 형식, 나갈 때는 실제 형식.
 * @return: kmalloc 한 정보 구조체(코어가 복사한 뒤 해제한다), 실패 시 ERR_PTR.
 *
 * 무엇에 쓰는가: iommufd 를 쓰는 VMM(QEMU 등)이 게스트에게 가상 IOMMU 를
 * 보여 줄 때, 그 가상 IOMMU 의 능력을 실제 하드웨어에서 유도해야 한다. 게스트가
 * 자기 페이지 테이블을 어떤 형식으로 만들지, 몇 레벨을 쓸지가 모두 이 값에
 * 달려 있다. 그래서 cap/ecap 레지스터를 가공 없이 그대로 넘긴다.
 *
 * flags 의 ERRATA_772415_SPR17: 하드웨어 결함까지 알려 준다. 이 결함이 있는
 * 하드웨어에서는 중첩 변환의 부모 테이블에 읽기 전용 매핑을 두면 안 되는데,
 * 그 규칙을 지켜야 하는 것은 게스트 테이블을 만드는 유저스페이스이기 때문이다.
 *
 * type 이 입출력인 이유: 사용자는 IOMMU_HW_INFO_TYPE_DEFAULT 로 "이 하드웨어의
 * 기본 형식을 달라"고 물을 수 있고, 그때 우리가 INTEL_VTD 로 답을 채워 어떤
 * 형식으로 해석해야 하는지 알려 준다.
 *
 * 실행 컨텍스트: iommufd ioctl. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommufd IOMMU_GET_HW_INFO ioctl → [intel_iommu_hw_info]
 */
static void *intel_iommu_hw_info(struct device *dev, u32 *length,
				 enum iommu_hw_info_type *type)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct iommu_hw_info_vtd *vtd;	/* [한국어] 유저스페이스로 보낼 정보 구조체 */

	if (*type != IOMMU_HW_INFO_TYPE_DEFAULT &&	/* [한국어] 사용자가 기본값을 요청한 것도 아니고 */
	    *type != IOMMU_HW_INFO_TYPE_INTEL_VTD)	/* [한국어] VT-d 형식을 요청한 것도 아니면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 우리가 답할 수 있는 형식이 아니다 */

	vtd = kzalloc_obj(*vtd);	/* [한국어] 정보 구조체 */
	if (!vtd)	/* [한국어] 할당 실패 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 전달 */

	vtd->flags = IOMMU_HW_INFO_VTD_ERRATA_772415_SPR17;	/* [한국어] 이 하드웨어 결함을 유저스페이스에도 알린다. QEMU 같은 사용자가 게스트 페이지 테이블을 만들 때 읽기 전용 매핑을 피해야 하기 때문이다 */
	vtd->cap_reg = iommu->cap;	/* [한국어] 능력 레지스터 원본을 그대로 넘긴다. 유저스페이스(iommufd 를 쓰는 VMM)가 게스트에게 보여 줄 가상 IOMMU 의 능력을 이 값에서 유도한다 */
	vtd->ecap_reg = iommu->ecap;	/* [한국어] 확장 능력도 마찬가지 */
	*length = sizeof(*vtd);	/* [한국어] 실제로 채운 크기 */
	*type = IOMMU_HW_INFO_TYPE_INTEL_VTD;	/* [한국어] 어떤 형식으로 답했는지 알려 준다 */
	return vtd;	/* [한국어] 코어가 유저스페이스로 복사한 뒤 해제한다 */
}

/* Set dirty tracking for the devices that the domain has been attached. */
/*
 * [한국어] (위 영어 주석에 이어)
 * domain_set_dirty_tracking - 이 도메인에 붙은 모든 장치/PASID 에 더티 추적을 켜거나 끈다
 *
 * @domain: 대상 도메인. 호출자가 domain->lock 을 쥐고 있어야 한다.
 * @enable: true 면 켜기, false 면 끄기.
 * @return: 0 성공, 음수면 도중에 실패(부분 적용 상태로 돌아온다).
 *
 * 더티 추적이란: 하드웨어가 페이지 테이블 항목에 "이 페이지에 DMA 쓰기가
 * 있었다"는 비트를 남기게 하는 기능이다. VM 라이브 마이그레이션에서 장치가
 * 고친 페이지만 다시 보내려면 이 정보가 필요하다. CPU 쪽 dirty 비트와 목적은
 * 같지만, DMA 는 CPU 를 거치지 않으므로 IOMMU 가 따로 기록해야 한다.
 *
 * 설정이 도메인이 아니라 PASID 항목에 있는 이유: 추적을 켜고 끄는 것은
 * 페이지 테이블의 성질이 아니라 "이 항목을 통한 접근을 어떻게 기록할지"의
 * 문제라, 컨텍스트/PASID 항목의 비트로 제어된다. 그래서 도메인에 붙은
 * 장치 하나하나, PASID 하나하나에 적용해야 한다 — 목록이 둘인 이유다.
 *
 * 부분 실패를 그대로 돌려주는 이유: 되돌리기는 호출자
 * (intel_iommu_set_dirty_tracking)가 원래 값으로 전체를 다시 적용하는 방식으로
 * 한다. 이미 원래 값인 항목에 다시 적용해도 무해하므로, 여기서 "어디까지
 * 갔는지"를 기억할 필요가 없다.
 *
 * 동기화: domain->lock 을 호출자가 쥔다(lockdep_assert_held 로 확인). 순회
 * 중에 장치가 붙거나 떨어지면 일부만 설정된 상태가 남기 때문이다.
 *
 * 호출 체인:
 *   intel_iommu_set_dirty_tracking()/parent_domain_set_dirty_tracking()
 *     → [domain_set_dirty_tracking] → intel_pasid_setup_dirty_tracking()
 */
static int domain_set_dirty_tracking(struct dmar_domain *domain, bool enable)
{
	struct device_domain_info *info;	/* [한국어] 장치 순회 커서 */
	struct dev_pasid_info *dev_pasid;	/* [한국어] (장치, PASID) 순회 커서 */
	int ret = 0;	/* [한국어] 결과 */

	lockdep_assert_held(&domain->lock);	/* [한국어] 호출자가 도메인 락을 쥐고 있어야 한다. 순회 중에 장치가 붙거나 떨어지면 일부만 설정된 상태가 남는다 */

	list_for_each_entry(info, &domain->devices, link) {	/* [한국어] 먼저 PASID 없이 붙은 장치들에 대해 */
		ret = intel_pasid_setup_dirty_tracking(info->iommu, info->dev,	/* [한국어] 그 장치의 PASID 항목에 추적 비트를 켜거나 끈다 */
						       IOMMU_NO_PASID, enable);	/* [한국어] 기본 트래픽의 항목 */
		if (ret)	/* [한국어] 실패하면 */
			return ret;	/* [한국어] 즉시 돌아간다. 되돌리기는 호출자가 한다 */
	}

	list_for_each_entry(dev_pasid, &domain->dev_pasids, link_domain) {	/* [한국어] 다음은 PASID 단위로 붙은 것들 */
		info = dev_iommu_priv_get(dev_pasid->dev);	/* [한국어] 그 장치의 정보 */
		ret = intel_pasid_setup_dirty_tracking(info->iommu, info->dev,	/* [한국어] 같은 설정을 */
						       dev_pasid->pasid, enable);	/* [한국어] 그 PASID 의 항목에 */
		if (ret)	/* [한국어] 실패 */
			break;	/* [한국어] 중단 */
	}

	return ret;	/* [한국어] 0 이면 모두 성공 */
}

/*
 * [한국어]
 * parent_domain_set_dirty_tracking - 중첩 부모 도메인 아래의 모든 1단계 도메인에 더티 추적을 적용한다
 *
 * @domain: 2단계(부모) 도메인.
 * @enable: 켤지 끌지.
 * @return: 0 성공, 음수면 실패(이미 바꾼 것들은 원래대로 되돌린 상태).
 *
 * 왜 자식까지 봐야 하는가: 중첩 변환에서 게스트의 DMA 는 게스트의 1단계
 * 테이블을 거쳐 호스트의 2단계 테이블로 내려온다. 그런데 더티 비트를 남기는
 * 설정은 각 장치/PASID 의 항목에 붙어 있고, 게스트 장치들은 1단계 도메인에
 * 매달려 있다. 그래서 부모에만 켜면 게스트 DMA 는 추적되지 않는다 —
 * 마이그레이션이 페이지를 놓치게 된다.
 *
 * 락이 두 겹인 이유: s1_lock 은 자식 목록을 지키고, 각 자식의 lock 은 그
 * 자식의 장치 목록을 지킨다. 자식마다 잡았다 놓는 것은 그 락이 인터럽트
 * 문맥에서도 잡히기 때문(irqsave)이고, s1_lock 은 그렇지 않아 평범한
 * spin_lock 이면 된다.
 *
 * 되돌리기: 실패하면 자식 전체를 부모의 원래 설정(domain->dirty_tracking)으로
 * 다시 적용한다. 아직 바꾸지 않은 자식에 원래 값을 다시 써도 무해하므로,
 * 어디까지 진행했는지 기억할 필요가 없다.
 *
 * 실행 컨텍스트: 마이그레이션 준비. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   intel_iommu_set_dirty_tracking() → [parent_domain_set_dirty_tracking]
 *     → domain_set_dirty_tracking()
 */
static int parent_domain_set_dirty_tracking(struct dmar_domain *domain,
					    bool enable)
{
	struct dmar_domain *s1_domain;	/* [한국어] 자식 1단계 도메인 순회 커서 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int ret;	/* [한국어] 결과 */

	spin_lock(&domain->s1_lock);	/* [한국어] 자식 목록을 보호한다. 이 락은 인터럽트 문맥에서 잡히지 않아 irqsave 가 필요 없다 */
	list_for_each_entry(s1_domain, &domain->s1_domains, s2_link) {	/* [한국어] 이 2단계 도메인 위에 얹힌 1단계 도메인들에 대해 */
		spin_lock_irqsave(&s1_domain->lock, flags);	/* [한국어] 각 자식 도메인의 장치 목록을 보호한다 */
		ret = domain_set_dirty_tracking(s1_domain, enable);	/* [한국어] 그 자식에 붙은 모든 장치/PASID 에 설정을 적용 */
		spin_unlock_irqrestore(&s1_domain->lock, flags);	/* [한국어] 자식 락 해제 */
		if (ret)	/* [한국어] 하나라도 실패하면 */
			goto err_unwind;	/* [한국어] 이미 바꾼 것들을 되돌린다 */
	}
	spin_unlock(&domain->s1_lock);	/* [한국어] 목록 락 해제 */
	return 0;	/* [한국어] 모든 자식에 적용 완료 */

err_unwind:	/* [한국어] 부분 실패 경로 */
	list_for_each_entry(s1_domain, &domain->s1_domains, s2_link) {	/* [한국어] 자식 전체를 다시 훑으며 */
		spin_lock_irqsave(&s1_domain->lock, flags);	/* [한국어] 자식 락 */
		domain_set_dirty_tracking(s1_domain, domain->dirty_tracking);	/* [한국어] 부모의 원래 설정으로 되돌린다. 아직 바꾸지 않은 자식에 다시 적용해도 값이 같아 무해하므로, 어디까지 진행했는지 기억할 필요 없이 전체를 되돌린다 */
		spin_unlock_irqrestore(&s1_domain->lock, flags);	/* [한국어] 자식 락 해제 */
	}
	spin_unlock(&domain->s1_lock);	/* [한국어] 목록 락 해제 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * intel_iommu_set_dirty_tracking - 도메인 단위로 더티 추적을 켜거나 끈다
 *
 * @domain: 대상 도메인(2단계, dirty_ops 가 달린 것).
 * @enable: 켤지 끌지.
 * @return: 0 성공(이미 원하는 상태였던 경우 포함), 음수면 실패.
 *
 * iommu_dirty_ops.set_dirty_tracking 콜백이며, VFIO 가 라이브 마이그레이션을
 * 시작하기 전에 부른다. 이 뒤로 하드웨어가 DMA 쓰기를 페이지 테이블에 기록하고,
 * VMM 이 그 비트를 읽어 어떤 페이지를 다시 보낼지 정한다.
 *
 * 적용 범위가 둘이다.
 *   1) 이 도메인에 직접 붙은 장치/PASID.
 *   2) 이 도메인이 중첩 부모라면, 그 위에 얹힌 1단계 도메인들에 붙은 것까지.
 *      게스트 DMA 를 놓치지 않으려면 자식까지 켜야 한다.
 *
 * 상태 갱신 순서: dmar_domain->dirty_tracking 은 두 적용이 모두 성공한 뒤에야
 * 쓴다. 그래야 err_unwind 가 "원래 값"으로 되돌릴 수 있다 — 먼저 갱신하면
 * 되돌릴 값이 사라진다.
 *
 * 동기화: dmar_domain->lock 을 잡은 채 검사·적용·갱신을 모두 한다. 그 사이에
 * 장치가 붙으면 attach 경로가 도메인의 dirty_tracking 을 보고 같은 설정을
 * 해 주므로 누락이 없다.
 *
 * 실행 컨텍스트: VFIO 마이그레이션 설정. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_domain_set_dirty_tracking() (VFIO) → [이 함수]
 *     → domain_set_dirty_tracking() → parent_domain_set_dirty_tracking()
 */
static int intel_iommu_set_dirty_tracking(struct iommu_domain *domain,
					  bool enable)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);	/* [한국어] VT-d 도메인으로 */
	int ret;	/* [한국어] 결과 */

	spin_lock(&dmar_domain->lock);	/* [한국어] 설정과 상태 갱신을 한 임계 구역에서 */
	if (dmar_domain->dirty_tracking == enable)	/* [한국어] 이미 원하는 상태면 */
		goto out_unlock;	/* [한국어] 할 일이 없다 */

	ret = domain_set_dirty_tracking(dmar_domain, enable);	/* [한국어] 이 도메인에 직접 붙은 장치들에 적용 */
	if (ret)	/* [한국어] 실패 */
		goto err_unwind;	/* [한국어] 되돌린다 */

	if (dmar_domain->nested_parent) {	/* [한국어] 중첩 부모라면 자식 1단계 도메인들에도 적용해야 한다 */
		ret = parent_domain_set_dirty_tracking(dmar_domain, enable);	/* [한국어] 게스트의 1단계 테이블을 쓰는 DMA 도 추적해야 마이그레이션이 정확하기 때문이다 */
		if (ret)	/* [한국어] 실패 */
			goto err_unwind;	/* [한국어] 되돌린다 */
	}

	dmar_domain->dirty_tracking = enable;	/* [한국어] 모두 성공한 뒤에야 상태를 기록한다 */
out_unlock:	/* [한국어] 이미 원하는 상태였던 경우가 합류 */
	spin_unlock(&dmar_domain->lock);	/* [한국어] 락 해제 */

	return 0;	/* [한국어] 성공 */

err_unwind:	/* [한국어] 부분 실패 경로 */
	domain_set_dirty_tracking(dmar_domain, dmar_domain->dirty_tracking);	/* [한국어] 아직 갱신하지 않은 원래 값으로 되돌린다 */
	spin_unlock(&dmar_domain->lock);	/* [한국어] 락 해제 */
	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * context_setup_pass_through - 소스 id 하나의 컨텍스트 항목을 "번역 없이 통과"로 세운다
 *
 * @dev: 대상 장치(로그와 유닛 조회에 쓴다).
 * @bus, @devfn: 세울 컨텍스트 항목의 소스 id. 별칭일 수 있어 장치 자신의
 *               위치와 다를 수 있다.
 * @return: 0 성공, -ENOMEM 이면 컨텍스트 테이블을 만들지 못했다.
 *
 * 통과(pass-through) 모드란: 컨텍스트 항목의 translation type 을
 * CONTEXT_TT_PASS_THROUGH 로 두면, 하드웨어가 페이지 테이블을 워크하지 않고
 * IOVA 를 그대로 물리 주소로 쓴다. 격리는 없지만 번역 비용도 없다. 신뢰할 수
 * 있는 내부 장치나 IOMMU 로 성능이 떨어지는 장치에 쓴다.
 *
 * 세 가지 세부가 통과 모드 특유다.
 *   - 도메인 id 는 FLPT_DEFAULT_DID 로 고정한다. 번역을 하지 않으니 실제
 *     주소 공간 구분이 필요 없고, 캐시 무효화의 대상 지정에만 쓰인다.
 *   - AW(address width)는 하드웨어가 지원하는 최대 AGAW 로 프로그램해야 하고,
 *     ASR(페이지 테이블 주소)은 하드웨어가 무시한다(위 영어 주석).
 *   - fault enable 은 켜 둔다. 통과 모드라도 하드웨어가 다룰 수 있는 주소
 *     범위를 벗어난 접근은 여전히 폴트로 알아야 한다.
 *
 * present 를 마지막에 세우는 이유: 이 비트를 켜는 순간부터 하드웨어가 항목을
 * 사용한다. 나머지 필드가 모두 채워진 뒤여야 절반만 설정된 항목을 워크하는
 * 일이 없다.
 *
 * 이미 세워져 있으면 그냥 돌아간다: 별칭 순회 때문에 같은 항목이 두 번
 * 들어올 수 있다. 다만 물려받은 항목(context_copied)은 예외로 다시 세운다.
 *
 * 동기화: iommu->lock 으로 컨텍스트 테이블 전체를 보호한다.
 * 실행 컨텍스트: 항등 도메인 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   device_setup_pass_through()/context_setup_pass_through_cb()
 *     → [context_setup_pass_through] → iommu_context_addr()
 *     → copied_context_tear_down() → context_present_cache_flush()
 */
static int context_setup_pass_through(struct device *dev, u8 bus, u8 devfn)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	struct context_entry *context;	/* [한국어] 세울 컨텍스트 항목 */

	spin_lock(&iommu->lock);	/* [한국어] 컨텍스트 테이블 변경 구간 */
	context = iommu_context_addr(iommu, bus, devfn, 1);	/* [한국어] 이 소스 id 의 항목을 찾는다. 마지막 1 은 없으면 테이블을 만들라는 뜻 */
	if (!context) {	/* [한국어] 테이블 할당 실패 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return -ENOMEM;	/* [한국어] 설정 불가 */
	}

	if (context_present(context) && !context_copied(iommu, bus, devfn)) {	/* [한국어] 이미 우리가 세운 항목이 있으면 */
		spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */
		return 0;	/* [한국어] 다시 세울 필요가 없다. 별칭 순회 때문에 같은 항목이 두 번 올 수 있어 이 검사가 필요하다 */
	}

	copied_context_tear_down(iommu, context, bus, devfn);	/* [한국어] 물려받은 항목이면 이전 커널의 자원을 정리한다 */
	context_clear_entry(context);	/* [한국어] 항목을 깨끗이 비운다 */
	context_set_domain_id(context, FLPT_DEFAULT_DID);	/* [한국어] 통과 모드 전용의 고정 도메인 id. 실제 번역을 하지 않으므로 도메인 id 는 캐시 구분용으로만 쓰인다 */

	/*
	 * In pass through mode, AW must be programmed to indicate the largest
	 * AGAW value supported by hardware. And ASR is ignored by hardware.
	 */
	context_set_address_width(context, iommu->msagaw);	/* [한국어] 통과 모드에서는 AW 를 하드웨어가 지원하는 최대 AGAW 로 프로그램해야 하고 ASR(테이블 주소)은 무시된다 (위 영어 주석) */
	context_set_translation_type(context, CONTEXT_TT_PASS_THROUGH);	/* [한국어] 번역 없이 통과시키라는 설정. IOVA 가 그대로 물리 주소가 된다 */
	context_set_fault_enable(context);	/* [한국어] 그래도 폴트 보고는 켜 둔다 — 범위를 벗어난 접근은 여전히 알아야 한다 */
	context_set_present(context);	/* [한국어] 마지막에 present 를 세운다. 이 비트를 켜는 순간부터 하드웨어가 이 항목을 쓰므로, 나머지 필드가 모두 채워진 뒤여야 한다 */
	if (!ecap_coherent(iommu->ecap))	/* [한국어] 유닛이 캐시를 스누프하지 않으면 */
		clflush_cache_range(context, sizeof(*context));	/* [한국어] 항목을 메모리로 밀어낸다 */
	context_present_cache_flush(iommu, FLPT_DEFAULT_DID, bus, devfn);	/* [한국어] 캐싱 모드 하드웨어가 캐시해 둔 "항목 없음"을 지운다 */
	spin_unlock(&iommu->lock);	/* [한국어] 락 해제 */

	return 0;	/* [한국어] 통과 설정 완료 */
}

/*
 * [한국어]
 * context_setup_pass_through_cb - DMA 별칭마다 통과 설정을 반복하는 콜백
 *
 * @pdev: 순회 중인 PCI 장치(쓰지 않는다).
 * @alias: 이 장치가 낼 수 있는 소스 id 하나.
 * @data: 원본 struct device.
 * @return: context_setup_pass_through 의 결과. 0 이 아니면 순회가 중단된다.
 *
 * domain_context_clear_one_cb 의 반대 방향이다. 장치가 여러 소스 id 로 DMA 를
 * 낼 수 있으므로, 통과 모드도 그 모든 id 에 대해 세워야 한다. 하나라도
 * 빠뜨리면 그 id 로 나가는 DMA 만 컨텍스트 항목이 없어 막힌다.
 *
 * 여기서는 0 이 아닌 값을 돌려주면 순회가 멈추는데, 통과 설정은 실패하면
 * 그 장치 전체가 동작하지 못하므로 멈추는 것이 맞다(별칭 지우기와 다른 점).
 *
 * 실행 컨텍스트: 항등 도메인 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   device_setup_pass_through() → pci_for_each_dma_alias()
 *     → [context_setup_pass_through_cb] → context_setup_pass_through()
 */
static int context_setup_pass_through_cb(struct pci_dev *pdev, u16 alias, void *data)
{
	struct device *dev = data;	/* [한국어] pci_for_each_dma_alias 가 넘겨준 원본 장치 */

	return context_setup_pass_through(dev, PCI_BUS_NUM(alias), alias & 0xff);	/* [한국어] 이 별칭의 컨텍스트 항목도 통과로 세운다. 별칭을 빠뜨리면 그 소스 id 로 나가는 DMA 만 막힌다 */
}

/*
 * [한국어]
 * device_setup_pass_through - 장치가 쓰는 모든 소스 id 를 통과 모드로 세운다
 *
 * @dev: 대상 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 레거시 모드(비 scalable)에서 항등 도메인을 붙이는 실제 구현이다. PCI 가
 * 아닌 장치는 별칭이 없어 자기 항목 하나면 되고, PCI 장치는
 * pci_for_each_dma_alias 로 모든 소스 id 를 훑는다 — domain_context_clear 와
 * 정확히 대칭인 구조다.
 *
 * scalable 모드에서는 이 함수 대신 intel_pasid_setup_pass_through 가 쓰인다.
 * 그 모드에서는 번역이 PASID 항목에서 시작하기 때문이다.
 *
 * 실행 컨텍스트: 항등 도메인 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   identity_domain_attach_dev() → [device_setup_pass_through]
 *     → pci_for_each_dma_alias() → context_setup_pass_through_cb()
 */
static int device_setup_pass_through(struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */

	if (!dev_is_pci(dev))	/* [한국어] PCI 가 아니면 별칭이 없다 */
		return context_setup_pass_through(dev, info->bus, info->devfn);	/* [한국어] 자기 항목 하나만 세운다 */

	return pci_for_each_dma_alias(to_pci_dev(dev),	/* [한국어] PCI 장치는 낼 수 있는 모든 소스 id 에 대해 */
				      context_setup_pass_through_cb, dev);	/* [한국어] 같은 통과 설정을 반복한다 */
}

/*
 * [한국어]
 * identity_domain_attach_dev - 장치를 항등(통과) 도메인에 붙인다
 *
 * @domain: 전역 identity_domain(상태가 없어 쓰이지 않는다).
 * @dev: 대상 장치. @old: 직전 도메인(쓰지 않는다).
 * @return: 0 성공, 음수면 실패.
 *
 * 항등 도메인이란: IOVA 를 그대로 물리 주소로 쓰는 "번역하지 않는" 도메인이다.
 * 격리가 필요 없거나 IOMMU 를 켜면 동작하지 않는 장치(RMRR 이 걸린 장치,
 * iommu=pt 로 부팅한 시스템)에 쓴다. 페이지 테이블이 없으므로 모든 장치가
 * 하나의 전역 인스턴스를 공유한다.
 *
 * 먼저 device_block_translation 으로 현재 설정을 내리는 것은
 * intel_iommu_attach_device 와 같은 이유다 — 옛 매핑과 새 설정이 겹치는
 * 순간을 만들지 않고, 실패해도 안전한 차단 상태로 남게 한다.
 *
 * 두 모드에서 세우는 곳이 다르다: scalable 모드는 PASID 항목을
 * (intel_pasid_setup_pass_through), 레거시 모드는 컨텍스트 항목을
 * (device_setup_pass_through, 별칭 포함) 통과로 세운다.
 *
 * PRI 를 건드리지 않는 이유(위 영어 주석): 항등 도메인에는 페이지 요청이
 * 의미가 없고(매핑이 없을 수가 없다), 이미 차단 상태를 거쳐 왔으므로
 * 여기서 켜거나 끌 필요가 없다.
 *
 * 실행 컨텍스트: 도메인 부착. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_attach_device(identity_domain) → [identity_domain_attach_dev]
 *     → device_block_translation()
 *     → intel_pasid_setup_pass_through() 또는 device_setup_pass_through()
 */
static int identity_domain_attach_dev(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	int ret;	/* [한국어] 결과 */

	device_block_translation(dev);	/* [한국어] 먼저 지금 붙어 있는 것을 전부 내린다 */

	if (dev_is_real_dma_subdevice(dev))	/* [한국어] 부모의 컨텍스트 항목을 공유하는 서브디바이스면 */
		return 0;	/* [한국어] 부모가 이미 통과 상태이므로 따로 세울 것이 없다 */

	/*
	 * No PRI support with the global identity domain. No need to enable or
	 * disable PRI in this path as the iommu has been put in the blocking
	 * state.
	 */
	if (sm_supported(iommu))	/* [한국어] scalable 모드면 */
		ret = intel_pasid_setup_pass_through(iommu, dev, IOMMU_NO_PASID);	/* [한국어] PASID 항목을 통과로 세운다 */
	else
		ret = device_setup_pass_through(dev);	/* [한국어] 레거시 모드면 컨텍스트 항목을 통과로 세운다(별칭 포함) */

	if (!ret)	/* [한국어] 성공했으면 */
		info->domain_attached = true;	/* [한국어] 붙은 상태로 표시한다. 위 영어 주석대로 이 경로에서는 PRI 를 켜고 끄지 않는다 — 항등 도메인에는 페이지 요청이 의미가 없고, 이미 차단 상태를 거쳐 왔기 때문이다 */

	return ret;	/* [한국어] 결과 */
}

/*
 * [한국어]
 * identity_domain_set_dev_pasid - 특정 PASID 만 항등(통과) 모드로 만든다
 *
 * @domain: 전역 identity_domain. @dev: 장치. @pasid: 대상 PASID.
 * @old: 이 PASID 가 쓰고 있던 도메인.
 * @return: 0 성공, -EOPNOTSUPP 이면 이 장치/유닛으로는 불가능.
 *
 * 장치 안의 한 주소 공간만 번역 없이 통과시킨다. 같은 장치의 다른 PASID 는
 * 여전히 번역될 수 있다 — PASID 단위 격리의 유연함이 여기 드러난다.
 *
 * 교체 순서는 intel_iommu_set_dev_pasid 와 같은 원칙이다: 폴트 처리를 먼저
 * 옮기고(하드웨어를 바꾼 직후의 폴트가 새 도메인으로 가도록), 하드웨어
 * 항목을 교체하고, 성공한 뒤에야 옛 도메인의 기록을 지운다. 하드웨어 교체가
 * 실패하면 폴트 처리를 되돌려 옛 상태를 온전히 유지한다.
 *
 * 항등 도메인이라 domain_add_dev_pasid 를 부르지 않는 점이 다르다. 페이지
 * 테이블도 도메인 id 도 없으므로 확보할 자원이 없고, 그래서
 * domain_remove_dev_pasid 도 항등 도메인이면 곧바로 돌아간다.
 *
 * 실행 컨텍스트: PASID 부착/교체. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   iommu_attach_device_pasid(identity_domain) → [이 함수]
 *     → iopf_for_domain_replace() → domain_setup_passthrough()
 *     → domain_remove_dev_pasid()
 */
static int identity_domain_set_dev_pasid(struct iommu_domain *domain,
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	int ret;	/* [한국어] 결과 */

	if (!pasid_supported(iommu) || dev_is_real_dma_subdevice(dev))	/* [한국어] 유닛이 PASID 를 못 하거나 부모 항목을 공유하는 서브디바이스면 */
		return -EOPNOTSUPP;	/* [한국어] PASID 단위로 항등 매핑을 줄 수 없다 */

	ret = iopf_for_domain_replace(domain, old, dev);	/* [한국어] 폴트 처리를 먼저 옮긴다 */
	if (ret)	/* [한국어] 실패 */
		return ret;	/* [한국어] 옛 상태 그대로 */

	ret = domain_setup_passthrough(iommu, dev, pasid, old);	/* [한국어] PASID 항목을 통과로 교체한다 */
	if (ret) {	/* [한국어] 실패하면 */
		iopf_for_domain_replace(old, domain, dev);	/* [한국어] 폴트 처리를 되돌린다 */
		return ret;	/* [한국어] 실패 이유 */
	}

	domain_remove_dev_pasid(old, dev, pasid);	/* [한국어] 성공한 뒤에야 옛 도메인의 기록을 지운다 */
	return 0;	/* [한국어] 이 PASID 는 이제 번역 없이 통과한다 */
}

static struct iommu_domain identity_domain = {	/* [한국어] 모든 장치가 공유하는 단 하나의 항등 도메인. 페이지 테이블이 없어 상태가 없다 */
	.type = IOMMU_DOMAIN_IDENTITY,	/* [한국어] 코어가 "IOVA = 물리 주소" 도메인으로 인식한다 */
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev	= identity_domain_attach_dev,	/* [한국어] 장치를 통과 모드로 */
		.set_dev_pasid	= identity_domain_set_dev_pasid,	/* [한국어] 특정 PASID 만 통과 모드로 */
	},
};

const struct iommu_domain_ops intel_fs_paging_domain_ops = {	/* [한국어] 1단계 페이징 도메인의 콜백 표 */
	IOMMU_PT_DOMAIN_OPS(x86_64),	/* [한국어] map/unmap/iova_to_phys 는 공용 x86-64 페이지 테이블 구현이 채운다 */
	.attach_dev = intel_iommu_attach_device,	/* [한국어] 장치 부착 */
	.set_dev_pasid = intel_iommu_set_dev_pasid,	/* [한국어] PASID 부착 */
	.iotlb_sync_map = intel_iommu_iotlb_sync_map,	/* [한국어] 매핑 후 동기화(필요한 하드웨어에서만) */
	.flush_iotlb_all = intel_flush_iotlb_all,	/* [한국어] 도메인 전체 무효화 */
	.iotlb_sync = intel_iommu_tlb_sync,	/* [한국어] 모아 둔 언매핑 범위 무효화 */
	.free = intel_iommu_domain_free,	/* [한국어] 도메인 해제 */
	.enforce_cache_coherency = intel_iommu_enforce_cache_coherency_fs,	/* [한국어] 1단계는 PASID 항목에 스누프 제어를 건다 */
};

const struct iommu_domain_ops intel_ss_paging_domain_ops = {	/* [한국어] 2단계 페이징 도메인의 콜백 표 */
	IOMMU_PT_DOMAIN_OPS(vtdss),	/* [한국어] VT-d 고유 형식의 페이지 테이블 구현 */
	.attach_dev = intel_iommu_attach_device,	/* [한국어] 1단계와 같은 부착 경로를 쓴다 */
	.set_dev_pasid = intel_iommu_set_dev_pasid,	/* [한국어] 같음 */
	.iotlb_sync_map = intel_iommu_iotlb_sync_map,	/* [한국어] 같음 */
	.flush_iotlb_all = intel_flush_iotlb_all,	/* [한국어] 같음 */
	.iotlb_sync = intel_iommu_tlb_sync,	/* [한국어] 같음 */
	.free = intel_iommu_domain_free,	/* [한국어] 같음 */
	.enforce_cache_coherency = intel_iommu_enforce_cache_coherency_ss,	/* [한국어] 2단계만 다르다 — PTE 의 SNP 비트를 쓴다 */
};

const struct iommu_ops intel_iommu_ops = {	/* [한국어] IOMMU 코어가 이 드라이버를 부르는 유일한 통로. 파일 앞쪽에서 전방 선언해 두었던 그 표다 */
	.blocked_domain		= &blocking_domain,	/* [한국어] 모든 DMA 를 막는 도메인. 코어가 장치를 안전한 상태로 두어야 할 때 쓴다 */
	.release_domain		= &blocking_domain,	/* [한국어] 장치가 떠날 때 남겨 둘 도메인. 차단과 같은 것을 쓴다 — 드라이버 없는 장치가 DMA 를 내지 못하게 */
	.identity_domain	= &identity_domain,	/* [한국어] 번역 없이 통과시키는 도메인 */
	.capable		= intel_iommu_capable,	/* [한국어] 능력 질의 */
	.hw_info		= intel_iommu_hw_info,	/* [한국어] iommufd 에 하드웨어 정보 제공 */
	.domain_alloc_paging_flags = intel_iommu_domain_alloc_paging_flags,	/* [한국어] 페이징 도메인 생성(1단계 우선) */
	.domain_alloc_sva	= intel_svm_domain_alloc,	/* [한국어] SVA 도메인 생성 — 프로세스의 페이지 테이블을 그대로 쓴다 */
	.domain_alloc_nested	= intel_iommu_domain_alloc_nested,	/* [한국어] 중첩 도메인 생성 — 게스트의 1단계 테이블을 얹는다 */
	.probe_device		= intel_iommu_probe_device,	/* [한국어] 장치 등장 */
	.probe_finalize		= intel_iommu_probe_finalize,	/* [한국어] 기본 도메인 부착 후 마무리(PASID/ATS/PRI 켜기) */
	.release_device		= intel_iommu_release_device,	/* [한국어] 장치 제거 */
	.get_resv_regions	= intel_iommu_get_resv_regions,	/* [한국어] 쓰면 안 되는 주소 범위 보고 */
	.device_group		= intel_iommu_device_group,	/* [한국어] IOMMU 그룹 결정 */
	.is_attach_deferred	= intel_iommu_is_attach_deferred,	/* [한국어] kdump 인계 상태에서 부착을 미룰지 */
	.def_domain_type	= device_def_domain_type,	/* [한국어] 이 장치의 기본 도메인을 번역으로 할지 항등으로 할지 (RMRR 등이 근거) */
	.page_response		= intel_iommu_page_response,	/* [한국어] 페이지 요청에 대한 응답을 하드웨어로 보낸다 */
};

/*
 * [한국어]
 * quirk_iommu_igfx - 통합 그래픽을 IOMMU 대상에서 제외한다
 *
 * @dev: PCI fixup 이 넘겨준 장치. 아래 DECLARE_PCI_FIXUP_HEADER 목록의
 *       디바이스 ID 와 일치하는 장치에 대해서만 불린다.
 * @return: 없음. 전역 disable_igfx_iommu 를 세우는 것이 전부다.
 *
 * 왜 필요한가: G4x/GM45, QM57/QS57, Broadwell 세대의 통합 GPU 는 IOMMU 아래에서
 * 정상 동작하지 않는다(아래 각 그룹의 영어 주석 참고). GPU 는 자체 GTT 를 통해
 * DMA 를 내는데, 그 경로가 VT-d 번역과 맞물리는 방식이 이 세대들에서 어긋난다.
 * 커널이 고칠 수 있는 문제가 아니라서 그냥 그 장치를 번역 대상에서 뺀다.
 *
 * risky_device 검사가 먼저인 이유: 이 우회는 PCI 디바이스 ID 만 보고 격리를
 * 완화한다. ID 는 장치가 스스로 보고하는 값이라 위조할 수 있으므로, 외부
 * 포트에 꽂힌 신뢰할 수 없는 장치에는 적용하지 않는다.
 *
 * 실행 컨텍스트: PCI 열거 중 헤더 fixup. VT-d 초기화보다 앞선다 — 그래서
 * 전역 변수를 세우기만 하고, init_dmars 가 나중에 그 값을 읽는다.
 *
 * 호출 체인:
 *   PCI 열거 → pci_fixup_device(header) → [quirk_iommu_igfx] → risky_device()
 */
static void quirk_iommu_igfx(struct pci_dev *dev)
{
	if (risky_device(dev))	/* [한국어] 외부 포트의 신뢰할 수 없는 장치면 */
		return;	/* [한국어] 우회를 적용하지 않는다 — ID 위조로 격리를 벗어나는 것을 막는다 */

	pci_info(dev, "Disabling IOMMU for graphics on this chipset\n");	/* [한국어] 왜 그래픽만 IOMMU 밖에 두는지 로그로 남긴다 */
	disable_igfx_iommu = 1;	/* [한국어] 통합 그래픽을 번역 대상에서 제외한다. 이 칩셋들의 GPU 는 IOMMU 아래에서 오동작한다 */
}

/* G4x/GM45 integrated gfx dmar support is totally busted. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2a40, quirk_iommu_igfx);	/* [한국어] G4x/GM45 통합 그래픽 — 이 세대의 DMAR 지원은 완전히 망가져 있다 (위 영어 주석) */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e00, quirk_iommu_igfx);	/* [한국어] 같은 계열의 다른 디바이스 ID */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e10, quirk_iommu_igfx);	/* [한국어] 같은 계열 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e20, quirk_iommu_igfx);	/* [한국어] 같은 계열 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e30, quirk_iommu_igfx);	/* [한국어] 같은 계열 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e40, quirk_iommu_igfx);	/* [한국어] 같은 계열 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e90, quirk_iommu_igfx);	/* [한국어] 같은 계열 */

/* QM57/QS57 integrated gfx malfunctions with dmar */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0044, quirk_iommu_igfx);	/* [한국어] QM57/QS57 통합 그래픽 — dmar 아래에서 오동작한다 (위 영어 주석) */

/* Broadwell igfx malfunctions with dmar */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1606, quirk_iommu_igfx);	/* [한국어] Broadwell 통합 그래픽 — 역시 dmar 아래에서 오동작한다 (위 영어 주석). 아래는 그 세대의 모든 GT 변종 ID 다 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160B, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160E, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1602, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160A, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x160D, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1616, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161B, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161E, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1612, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161A, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x161D, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1626, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162B, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162E, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1622, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162A, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x162D, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1636, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163B, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163E, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1632, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163A, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x163D, quirk_iommu_igfx);	/* [한국어] Broadwell 계열의 또 다른 변종 */

/*
 * [한국어]
 * quirk_iommu_rwbf - 능력 레지스터를 무시하고 쓰기 버퍼 비우기를 강제한다
 *
 * @dev: 대상 칩셋의 PCI 장치.
 * @return: 없음. 전역 rwbf_quirk 를 세운다.
 *
 * RWBF(Required Write-Buffer Flushing)란: 일부 옛 유닛은 우리가 메모리에 쓴
 * 페이지 테이블/컨텍스트 항목이 하드웨어에 보이려면, 명시적으로 쓰기 버퍼를
 * 비우라고 요구한다. 그 요구는 원래 CAP 레지스터의 RWBF 비트로 알려져야 한다.
 *
 * 문제는 Mobile 4 Series 칩셋(과 데스크톱 판)이 그 비트를 세우지 않으면서
 * 실제로는 그 동작이 필요하다는 것이다(위 영어 주석). 비우지 않으면 우리가
 * 세운 매핑이 하드웨어에 보이지 않아, 정상적으로 매핑한 주소가 폴트를 낸다.
 * 그래서 이 칩셋들에서는 하드웨어 신고를 무시하고 강제로 켠다.
 *
 * rwbf_required(iommu) 가 이 전역 값과 CAP 비트를 함께 본다. 도메인 생성
 * (iotlb_sync_map 설정)과 부착 호환성 검사가 그 결과를 쓴다.
 *
 * 실행 컨텍스트: PCI 헤더 fixup. VT-d 초기화 이전.
 *
 * 호출 체인:
 *   PCI 열거 → pci_fixup_device(header) → [quirk_iommu_rwbf]
 */
static void quirk_iommu_rwbf(struct pci_dev *dev)
{
	if (risky_device(dev))	/* [한국어] 신뢰할 수 없는 장치면 */
		return;	/* [한국어] 우회 없음 */

	/*
	 * Mobile 4 Series Chipset neglects to set RWBF capability,
	 * but needs it. Same seems to hold for the desktop versions.
	 */
	pci_info(dev, "Forcing write-buffer flush capability\n");	/* [한국어] 능력 레지스터를 무시하고 강제한다는 사실을 남긴다 */
	rwbf_quirk = 1;	/* [한국어] Mobile 4 계열 칩셋은 RWBF 능력 비트를 세우지 않으면서 실제로는 그 동작이 필요하다. 비우지 않으면 우리가 쓴 페이지 테이블이 하드웨어에 보이지 않는다 (위 영어 주석) */
}

DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2a40, quirk_iommu_rwbf);	/* [한국어] Mobile 4 Series — RWBF 능력을 신고하지 않으면서 실제로는 필요한 칩셋 (위 영어 주석) */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e00, quirk_iommu_rwbf);	/* [한국어] 같은 계열의 다른 디바이스 ID */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e10, quirk_iommu_rwbf);	/* [한국어] 같은 계열의 다른 디바이스 ID */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e20, quirk_iommu_rwbf);	/* [한국어] 같은 계열의 다른 디바이스 ID */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e30, quirk_iommu_rwbf);	/* [한국어] 같은 계열의 다른 디바이스 ID */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e40, quirk_iommu_rwbf);	/* [한국어] 같은 계열의 다른 디바이스 ID */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e90, quirk_iommu_rwbf);	/* [한국어] 같은 계열의 다른 디바이스 ID */

#define GGC 0x52	/* [한국어] GMCH Graphics Control 레지스터의 PCI 설정 공간 오프셋. 통합 그래픽이 쓰는 스텔른(stolen) 메모리의 크기와 VT 지원 여부가 여기 있다 */
#define GGC_MEMORY_SIZE_MASK	(0xf << 8)	/* [한국어] 스텔른 메모리 크기 필드를 뽑는 마스크 */
#define GGC_MEMORY_SIZE_NONE	(0x0 << 8)	/* [한국어] 할당되지 않음 */
#define GGC_MEMORY_SIZE_1M	(0x1 << 8)	/* [한국어] 1MB */
#define GGC_MEMORY_SIZE_2M	(0x3 << 8)	/* [한국어] 2MB */
#define GGC_MEMORY_VT_ENABLED	(0x8 << 8)	/* [한국어] VT 용 그림자 GTT 가 할당되었음을 뜻하는 비트. 이것이 꺼져 있으면 GPU 를 IOMMU 아래에 둘 수 없다 */
#define GGC_MEMORY_SIZE_2M_VT	(0x9 << 8)	/* [한국어] VT 활성 + 2MB */
#define GGC_MEMORY_SIZE_3M_VT	(0xa << 8)	/* [한국어] VT 활성 + 3MB */
#define GGC_MEMORY_SIZE_4M_VT	(0xb << 8)	/* [한국어] VT 활성 + 4MB */

/*
 * [한국어]
 * quirk_calpella_no_shadow_gtt - Calpella/Ironlake 의 그래픽 설정을 보고 정책을 정한다
 *
 * @dev: 대상 칩셋의 PCI 장치.
 * @return: 없음. 두 전역 정책 중 하나를 바꾼다.
 *
 * GGC(GMCH Graphics Control) 레지스터를 읽어 두 갈래로 나뉜다.
 *
 *   [1] VT 용 그림자 GTT 가 할당되지 않았으면(GGC_MEMORY_VT_ENABLED 가 꺼짐):
 *       GPU 를 IOMMU 밖에 둔다. 통합 GPU 는 자기 GTT 를 통해 DMA 를 내는데,
 *       VT-d 아래에서 동작하려면 BIOS 가 그 GTT 를 위한 별도 메모리(그림자
 *       GTT)를 잡아 줘야 한다. 없으면 번역할 방법이 아예 없다.
 *
 *   [2] GTT 는 있고 GPU 를 IOMMU 아래에 두기로 했으면: 지연 무효화(flush
 *       queue)를 끈다. Ironlake 는 IOTLB 를 비우기 전에 GPU 가 유휴 상태여야
 *       하는데(위 영어 주석), 지연 무효화는 언제 비울지를 커널이 통제하지
 *       못한다. 그래서 언매핑 즉시 비우는 strict 모드로 돌린다.
 *
 * 다른 우회들과 달리 결과가 두 가지인 것은, 이 칩셋의 문제가 "동작하지
 * 않는다"가 아니라 "BIOS 설정에 따라 달라진다"이기 때문이다.
 *
 * 실행 컨텍스트: PCI 헤더 fixup. VT-d 초기화 이전이라 전역 정책만 바꾼다.
 *
 * 호출 체인:
 *   PCI 열거 → pci_fixup_device(header) → [quirk_calpella_no_shadow_gtt]
 *     → pci_read_config_word() → iommu_set_dma_strict()
 */
static void quirk_calpella_no_shadow_gtt(struct pci_dev *dev)
{
	unsigned short ggc;	/* [한국어] 읽어 올 GGC 레지스터 값 */

	if (risky_device(dev))	/* [한국어] 신뢰할 수 없는 장치면 */
		return;	/* [한국어] 우회 없음 */

	if (pci_read_config_word(dev, GGC, &ggc))	/* [한국어] 설정 공간에서 GGC 를 읽는다 */
		return;	/* [한국어] 읽지 못하면 판단할 근거가 없다 */

	if (!(ggc & GGC_MEMORY_VT_ENABLED)) {	/* [한국어] BIOS 가 VT 용 그림자 GTT 를 할당하지 않았으면 */
		pci_info(dev, "BIOS has allocated no shadow GTT; disabling IOMMU for graphics\n");	/* [한국어] 이유를 남기고 */
		disable_igfx_iommu = 1;	/* [한국어] GPU 를 IOMMU 밖에 둔다. 그림자 GTT 없이는 GPU 의 DMA 를 번역할 수 없다 */
	} else if (!disable_igfx_iommu) {	/* [한국어] GTT 는 있고 GPU 를 IOMMU 아래에 두기로 했다면 */
		/* we have to ensure the gfx device is idle before we flush */
		pci_info(dev, "Disabling batched IOTLB flush on Ironlake\n");	/* [한국어] 배치 무효화를 끈다고 알린다 */
		iommu_set_dma_strict();	/* [한국어] Ironlake 는 무효화 전에 GPU 가 유휴 상태여야 한다 (위 영어 주석). 지연 무효화는 그 시점을 통제할 수 없으므로 즉시 무효화로 돌린다 */
	}
}
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0040, quirk_calpella_no_shadow_gtt);	/* [한국어] Calpella/Ironlake — BIOS 가 그림자 GTT 를 할당하지 않는 경우가 있다 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0062, quirk_calpella_no_shadow_gtt);	/* [한국어] 같은 계열 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x006a, quirk_calpella_no_shadow_gtt);	/* [한국어] 같은 계열 */

/*
 * [한국어]
 * quirk_igfx_skip_te_disable - 종료/kexec 때 번역을 끄지 않도록 표시한다
 *
 * @dev: 검사할 장치. PCI_ANY_ID 로 등록되어 모든 장치에 불리므로, 함수 안에서
 *       그래픽 장치인지와 세대를 직접 판별한다.
 * @return: 없음. 전역 iommu_skip_te_disable 을 세운다.
 *
 * 왜 필요한가: 보통은 종료 직전에 번역을 꺼서 다음 커널/펌웨어가 깨끗한
 * 상태를 넘겨받게 한다(intel_iommu_shutdown). 그런데 Skylake~Alder Lake 계열의
 * 일부 통합 GPU 는 번역이 꺼지는 순간 진행 중이던 DMA 가 엉켜 시스템이 그대로
 * 멈춘다. 그래서 그런 GPU 가 있는 시스템에서는 번역을 켠 채로 넘긴다.
 *
 * 판별 방식이 다른 우회들과 다르다: 대상 디바이스 ID 가 너무 많아 목록으로
 * 나열하는 대신 PCI_ANY_ID 로 등록하고, IS_GFX_DEVICE 로 그래픽 장치를 거른
 * 뒤 디바이스 ID 의 상위 바이트(세대 번호)를 알려진 목록과 대조한다.
 *
 * risky_device 검사는 세대 판별 뒤에 온다. 순서를 그렇게 둔 것은, 대상이
 * 아닌 장치에까지 경고 로그를 남기지 않기 위해서다.
 *
 * 실행 컨텍스트: PCI 헤더 fixup.
 *
 * 호출 체인:
 *   PCI 열거 → pci_fixup_device(header) → [quirk_igfx_skip_te_disable]
 */
static void quirk_igfx_skip_te_disable(struct pci_dev *dev)
{
	unsigned short ver;	/* [한국어] 디바이스 ID 에서 뽑은 세대 번호 */

	if (!IS_GFX_DEVICE(dev))	/* [한국어] 그래픽 장치가 아니면 */
		return;	/* [한국어] 대상이 아니다. 이 우회는 PCI_ANY_ID 로 등록되어 모든 장치에 불리므로 여기서 걸러야 한다 */

	ver = (dev->device >> 8) & 0xff;	/* [한국어] 디바이스 ID 의 상위 바이트가 세대를 나타낸다 */
	if (ver != 0x45 && ver != 0x46 && ver != 0x4c &&	/* [한국어] Skylake~Alder Lake 계열 중 */
	    ver != 0x4e && ver != 0x8a && ver != 0x98 &&	/* [한국어] 이 우회가 필요한 세대들과 */
	    ver != 0x9a && ver != 0xa7 && ver != 0x7d)	/* [한국어] 비교한다 */
		return;	/* [한국어] 해당하지 않으면 그냥 돌아간다 */

	if (risky_device(dev))	/* [한국어] 신뢰할 수 없는 장치면 */
		return;	/* [한국어] 우회 없음 */

	pci_info(dev, "Skip IOMMU disabling for graphics\n");	/* [한국어] 이유를 남긴다 */
	iommu_skip_te_disable = 1;	/* [한국어] 종료/kexec 때 번역을 끄지 않는다. 이 세대의 GPU 는 번역이 꺼지는 순간 진행 중이던 DMA 가 엉켜 시스템이 멈춘다 */
}
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, PCI_ANY_ID, quirk_igfx_skip_te_disable);	/* [한국어] PCI_ANY_ID — 이 우회는 디바이스 ID 가 아니라 함수 안에서 세대를 판별한다 */

/* On Tylersburg chipsets, some BIOSes have been known to enable the
   ISOCH DMAR unit for the Azalia sound device, but not give it any
   TLB entries, which causes it to deadlock. Check for that.  We do
   this in a function called from init_dmars(), instead of in a PCI
   quirk, because we don't want to print the obnoxious "BIOS broken"
   message if VT-d is actually disabled.
*/
/*
 * [한국어] (위 영어 주석에 이어)
 * check_tylersburg_isoch - Tylersburg 칩셋의 ISOCH DMAR 유닛 설정이 온전한지 확인한다
 *
 * @return: 없음. 문제가 있으면 경고를 남기고 필요하면 항등 매핑을 강제한다.
 *
 * 어떤 결함인가: Tylersburg 칩셋의 일부 BIOS 는 Azalia(HD 오디오) 컨트롤러의
 * DMA 를 ISOCH(등시성) 전용 DMAR 유닛으로 보내면서, 그 유닛에 TLB 항목을 하나도
 * 주지 않는다. 번역할 자리가 없는 유닛으로 DMA 가 몰리면 그 유닛이 데드락에
 * 빠지고, 오디오는 물론 그 유닛을 쓰는 모든 것이 멈춘다.
 *
 * 왜 PCI quirk 가 아니라 init_dmars 에서 부르는 함수인가(위 영어 주석):
 * PCI quirk 는 VT-d 를 쓰든 안 쓰든 무조건 실행된다. 그런데 VT-d 가 꺼져 있으면
 * 이 설정은 아무 문제가 되지 않으므로, "BIOS 가 망가졌다"는 요란한 메시지를
 * 띄울 이유가 없다. 그래서 VT-d 를 실제로 켜는 경로에서만 검사한다.
 *
 * 검사 순서: Azalia 가 있는지 → 시스템 관리 레지스터를 읽을 수 있는지 →
 * ISOCH 로 라우팅되는지(비트 0) → TLB 항목 수가 몇 개인지. 항목 수가 0 이면
 * IDENTMAP_AZALIA 를 세워 그 컨트롤러만 항등 매핑으로 돌려 데드락을 피하고,
 * 권장값(16)이 아닌 다른 값이면 경고만 남기고 진행한다.
 *
 * 참조 관리: pci_get_device 는 참조를 잡아 돌려주므로, 어느 경로로 나가든
 * pci_dev_put 으로 놓는다. 존재 확인만 하는 첫 조회도 마찬가지다.
 *
 * 실행 컨텍스트: init_dmars 안(__init). 부팅 중 단일 스레드.
 *
 * 호출 체인:
 *   init_dmars() → [check_tylersburg_isoch] → pci_get_device()
 *     → pci_read_config_dword()
 */
static void __init check_tylersburg_isoch(void)
{
	struct pci_dev *pdev;	/* [한국어] 조회할 장치 */
	uint32_t vtisochctrl;	/* [한국어] ISOCH 제어 레지스터 값 */

	/* If there's no Azalia in the system anyway, forget it. */
	pdev = pci_get_device(PCI_VENDOR_ID_INTEL, 0x3a3e, NULL);	/* [한국어] Azalia(HD 오디오) 컨트롤러를 찾는다 (위 영어 주석) */
	if (!pdev)	/* [한국어] 없으면 */
		return;	/* [한국어] 이 결함과 무관한 시스템이다 */

	if (risky_device(pdev)) {	/* [한국어] 신뢰할 수 없는 장치면 */
		pci_dev_put(pdev);	/* [한국어] 참조를 놓고 */
		return;	/* [한국어] 판단하지 않는다 */
	}

	pci_dev_put(pdev);	/* [한국어] 존재 확인만 했으므로 참조를 놓는다 */

	/* System Management Registers. Might be hidden, in which case
	   we can't do the sanity check. But that's OK, because the
	   known-broken BIOSes _don't_ actually hide it, so far. */
	pdev = pci_get_device(PCI_VENDOR_ID_INTEL, 0x342e, NULL);	/* [한국어] 시스템 관리 레지스터 장치를 찾는다. 숨겨져 있을 수 있지만, 알려진 결함 BIOS 들은 숨기지 않는다 (위 영어 주석) */
	if (!pdev)	/* [한국어] 없으면 */
		return;	/* [한국어] 검사할 수 없다 */

	if (risky_device(pdev)) {	/* [한국어] 신뢰할 수 없으면 */
		pci_dev_put(pdev);	/* [한국어] 참조를 놓고 */
		return;	/* [한국어] 판단하지 않는다 */
	}

	if (pci_read_config_dword(pdev, 0x188, &vtisochctrl)) {	/* [한국어] ISOCH 제어 레지스터를 읽는다 */
		pci_dev_put(pdev);	/* [한국어] 실패하면 참조를 놓고 */
		return;	/* [한국어] 포기 */
	}

	pci_dev_put(pdev);	/* [한국어] 값을 읽었으므로 참조를 놓는다 */

	/* If Azalia DMA is routed to the non-isoch DMAR unit, fine. */
	if (vtisochctrl & 1)	/* [한국어] Azalia DMA 가 ISOCH 가 아닌 DMAR 유닛으로 간다면 (위 영어 주석) */
		return;	/* [한국어] 문제 없다 */

	/* Drop all bits other than the number of TLB entries */
	vtisochctrl &= 0x1c;	/* [한국어] TLB 항목 수 필드만 남긴다 (위 영어 주석) */

	/* If we have the recommended number of TLB entries (16), fine. */
	if (vtisochctrl == 0x10)	/* [한국어] 권장값인 16개면 (위 영어 주석) */
		return;	/* [한국어] 정상이다 */

	/* Zero TLB entries? You get to ride the short bus to school. */
	if (!vtisochctrl) {	/* [한국어] 0개라면 — Azalia 의 DMA 가 ISOCH 유닛으로 가는데 TLB 자리가 전혀 없다 */
		WARN(1, "Your BIOS is broken; DMA routed to ISOCH DMAR unit but no TLB space.\n"	/* [한국어] 이 조합은 그 유닛이 데드락에 빠진다 (파일 위 영어 주석) */
		     "BIOS vendor: %s; Ver: %s; Product Version: %s\n",	/* [한국어] 어느 BIOS 의 문제인지 */
		     dmi_get_system_info(DMI_BIOS_VENDOR),	/* [한국어] 벤더 */
		     dmi_get_system_info(DMI_BIOS_VERSION),	/* [한국어] 버전 */
		     dmi_get_system_info(DMI_PRODUCT_VERSION));	/* [한국어] 제품 버전 */
		iommu_identity_mapping |= IDENTMAP_AZALIA;	/* [한국어] 이 오디오 컨트롤러만 항등 매핑으로 돌려 데드락을 피한다 */
		return;	/* [한국어] 처리 완료 */
	}

	pr_warn("Recommended TLB entries for ISOCH unit is 16; your BIOS set %d\n",	/* [한국어] 0 은 아니지만 권장값도 아닌 경우 — 경고만 남기고 그대로 진행한다 */
	       vtisochctrl);	/* [한국어] 실제 값 */
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
/*
 * [한국어] (위 영어 주석에 이어)
 * quirk_extra_dev_tlb_flush - 결함 있는 장치에 디바이스 TLB 무효화를 한 번 더 보낸다
 *
 * @info: 대상 장치 정보. dtlb_extra_inval 이 이 우회의 대상 표시다.
 * @address: 무효화할 주소. @mask: 그 범위 크기.
 * @pasid: 무효화할 PASID, 또는 IOMMU_NO_PASID.
 * @qdep: 장치의 ATS 큐 깊이.
 * @return: 없음.
 *
 * 어떤 결함인가(위 영어 주석 요약): 일부 장치는 ATS 무효화 완료 응답을,
 * 무효화 대상 범위의 번역을 이미 써서 발행한 posted write 보다 먼저 보낸다.
 * 즉 "무효화가 끝났다"는 응답이 왔는데도 옛 번역으로 향하는 쓰기가 아직 길
 * 위에 남아 있다. 완료 순서 보장이 깨진 것이다.
 *
 * 왜 위험한가: 커널은 무효화 완료를 보고 그 페이지를 해제하거나 다른 용도로
 * 준다. 그런데 뒤늦게 도착한 쓰기가 그 페이지로 향하면 이미 남의 것이 된
 * 메모리를 덮어쓴다. 특히 언매핑 전에 DMA 가 멈췄다고 보장할 수 없는
 * 경우(사용자가 제어하는 SVA, PASID 해제, 프로세스 crash 후 exit_mmap 등)에
 * 이 창이 실제로 열린다. 그래서 신뢰/특권 호스트 드라이버가 통제하지 않는
 * 모든 dTLB 무효화는 이 우회를 써야 한다.
 *
 * 해법: 같은 무효화를 한 번 더 보낸다. 두 번째 무효화의 완료를 기다리는
 * 동안 첫 번째 이후에 발행된 쓰기가 모두 도착하므로, 순서 보장이 회복된다.
 *
 * 위 영어 주석의 마지막 문장 — 중첩 변환을 켜면 6번 조건(게스트가 제어하는
 * 무효화)이 반드시 이 우회를 필요로 한다.
 *
 * likely(!dtlb_extra_inval) 로 시작하는 이유: 대부분의 장치는 이 결함이 없어
 * 곧바로 돌아간다. 이 함수가 무효화 경로 한가운데서 불리므로 그 분기를
 * 컴파일러에 알려 준다.
 *
 * 실행 컨텍스트: 무효화 경로. 스핀락을 쥔 상태일 수 있어 잠들면 안 된다.
 *
 * 호출 체인:
 *   cache_tag_flush_range()/cache_tag_flush_all() → [quirk_extra_dev_tlb_flush]
 *     → qi_flush_dev_iotlb() / qi_flush_dev_iotlb_pasid()
 */
void quirk_extra_dev_tlb_flush(struct device_domain_info *info,
			       unsigned long address, unsigned long mask,
			       u32 pasid, u16 qdep)
{
	u16 sid;	/* [한국어] 이 장치의 소스 id */

	if (likely(!info->dtlb_extra_inval))	/* [한국어] 이 결함이 없는 장치면 */
		return;	/* [한국어] 추가 무효화가 필요 없다. 대부분의 장치가 여기서 돌아간다 */

	sid = PCI_DEVID(info->bus, info->devfn);	/* [한국어] 버스와 devfn 을 16비트 소스 id 로 */
	if (pasid == IOMMU_NO_PASID) {	/* [한국어] PASID 를 쓰지 않는 기본 트래픽이면 */
		qi_flush_dev_iotlb(info->iommu, sid, info->pfsid,	/* [한국어] 같은 범위에 디바이스 TLB 무효화를 한 번 더 보낸다 */
				   qdep, address, mask);	/* [한국어] 같은 인자로 */
	} else {
		qi_flush_dev_iotlb_pasid(info->iommu, sid, info->pfsid,	/* [한국어] PASID 트래픽이면 PASID 를 지정한 무효화를 */
					 pasid, qdep, address, mask);	/* [한국어] 역시 한 번 더 보낸다 */
	}
}

#define ecmd_get_status_code(res)	(((res) & 0xff) >> 1)	/* [한국어] 응답 레지스터의 하위 8비트에서 상태 코드를 뽑는다. 비트 0 은 진행 중(IP) 플래그라 한 칸 밀어낸다 */

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
/*
 * [한국어] (위 영어 주석에 이어)
 * ecmd_submit_sync - 확장 명령(Enhanced Command)을 보내고 완료까지 기다린다
 *
 * @iommu: 대상 유닛.
 * @ecmd: 보낼 명령 코드.
 * @oa: 피연산자 A. 명령 레지스터에 명령 코드와 함께 실린다.
 * @ob: 피연산자 B. 별도 레지스터로 보낸다.
 * @return: 0 성공, 음수면 소프트웨어 오류(-ENODEV/-EBUSY/-ETIMEDOUT),
 *          양수면 하드웨어가 돌려준 실패 코드(스펙 Table 48) — 위 영어 주석.
 *
 * 확장 명령 인터페이스란: 큐 기반 무효화(QI)와 별개로, 유닛에 직접 명령을
 * 하나씩 보내고 응답을 기다리는 동기식 경로다. 성능 카운터 설정이나 특정
 * 진단 동작처럼 드물게 일어나고 순서가 중요한 명령에 쓴다.
 *
 * 프로토콜: 응답 레지스터(ECRSP)의 IP(In Progress) 비트가 상태를 알려 준다.
 *   1) IP 가 이미 켜져 있으면 이전 명령이 진행 중이라 -EBUSY.
 *   2) 피연산자 B 를 쓰고, 그 다음 명령 레지스터를 쓴다. 명령 레지스터 쓰기가
 *      실행을 시작시키므로 반드시 이 순서여야 한다.
 *   3) IP 가 내려갈 때까지 폴링한다. 끝내 내려가지 않으면 -ETIMEDOUT.
 *   4) 응답의 하위 비트에서 상태 코드를 뽑는다.
 *
 * 피연산자 B 를 무조건 쓰는 이유(위 영어 주석): 필요 없는 명령이라도 그
 * 레지스터에 값이 들어 있는 것 자체는 부작용이 없고, 이 경로가 성능이
 * 중요한 곳이 아니라 MMIO 쓰기 한 번을 더 하는 비용이 문제되지 않는다.
 * 조건 분기를 없애 코드가 단순해지는 쪽을 택했다.
 *
 * 동기화: register_lock 을 raw 스핀락으로 인터럽트를 끈 채 잡는다. raw 인
 * 것은 PREEMPT_RT 커널에서도 이 구간이 잠들면 안 되기 때문이다 — MMIO 로
 * 하드웨어와 핸드셰이크하는 중에 선점되면 IP 비트를 보는 다른 CPU 와
 * 경쟁하게 된다.
 *
 * 실행 컨텍스트: 성능 카운터 설정 등. 프로세스 컨텍스트이지만 락 안에서는
 * 잠들 수 없다.
 *
 * 호출 체인:
 *   iommu_pmu_*() 등 → [ecmd_submit_sync] → readq()/writeq()
 */
int ecmd_submit_sync(struct intel_iommu *iommu, u8 ecmd, u64 oa, u64 ob)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	u64 res;	/* [한국어] 응답 레지스터 값 */
	int ret;	/* [한국어] 반환할 결과 */

	if (!cap_ecmds(iommu->cap))	/* [한국어] 유닛이 확장 명령 인터페이스를 지원하지 않으면 */
		return -ENODEV;	/* [한국어] 쓸 수 없다 */

	raw_spin_lock_irqsave(&iommu->register_lock, flags);	/* [한국어] 레지스터 접근을 직렬화한다. raw 스핀락인 것은 PREEMPT_RT 에서도 이 구간이 잠들면 안 되기 때문이다 */

	res = readq(iommu->reg + DMAR_ECRSP_REG);	/* [한국어] 응답 레지스터를 먼저 읽는다 */
	if (res & DMA_ECMD_ECRSP_IP) {	/* [한국어] 이전 명령이 아직 진행 중이면 */
		ret = -EBUSY;	/* [한국어] 새 명령을 넣을 수 없다 */
		goto err;	/* [한국어] 락을 놓고 나간다 */
	}

	/*
	 * Unconditionally write the operand B, because
	 * - There is no side effect if an ecmd doesn't require an
	 *   operand B, but we set the register to some value.
	 * - It's not invoked in any critical path. The extra MMIO
	 *   write doesn't bring any performance concerns.
	 */
	writeq(ob, iommu->reg + DMAR_ECEO_REG);	/* [한국어] 피연산자 B 를 항상 쓴다. 필요 없는 명령이라도 부작용이 없고, 성능이 중요한 경로가 아니어서 조건 분기를 두지 않았다 (위 영어 주석) */
	writeq(ecmd | (oa << DMA_ECMD_OA_SHIFT), iommu->reg + DMAR_ECMD_REG);	/* [한국어] 명령 코드와 피연산자 A 를 한 워드에 담아 쓴다. 이 쓰기가 명령 실행을 시작시킨다 */

	IOMMU_WAIT_OP(iommu, DMAR_ECRSP_REG, readq,	/* [한국어] 응답 레지스터를 폴링하며 */
		      !(res & DMA_ECMD_ECRSP_IP), res);	/* [한국어] 진행 중 비트가 내려갈 때까지 기다린다 */

	if (res & DMA_ECMD_ECRSP_IP) {	/* [한국어] 시간이 다 되도록 내려가지 않았으면 */
		ret = -ETIMEDOUT;	/* [한국어] 하드웨어가 응답하지 않는다 */
		goto err;	/* [한국어] 정리 */
	}

	ret = ecmd_get_status_code(res);	/* [한국어] 하드웨어가 돌려준 상태 코드. 0 이면 성공이고, 양수면 스펙 Table 48 의 실패 코드다 (위 영어 주석) */
err:	/* [한국어] 두 실패 경로가 합류 */
	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);	/* [한국어] 락 해제 */

	return ret;	/* [한국어] 음수면 소프트웨어 오류, 양수면 하드웨어 상태 코드, 0 이면 성공 */
}

MODULE_IMPORT_NS("GENERIC_PT_IOMMU");	/* [한국어] 공용 페이지 테이블 라이브러리의 심볼 네임스페이스를 가져온다. pt_iommu_x86_64_init 등이 그 안에 있다 */
