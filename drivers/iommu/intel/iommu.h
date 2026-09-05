/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2006-2015, Intel Corporation.
 *
 * Authors: Ashok Raj <ashok.raj@intel.com>
 *          Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>
 *          David Woodhouse <David.Woodhouse@intel.com>
 */

/*
 * [한국어 설명] Intel VT-d 드라이버의 중심 헤더 — 하드웨어 레지스터 정의와 커널 자료 모델 (intel/iommu.h)
 *
 * === 파일의 역할 ===
 * 이 헤더 하나가 Intel VT-d(Virtualization Technology for Directed I/O) 드라이버가
 * 다루는 두 세계를 모두 정의한다. 하나는 하드웨어 쪽 — 유닛의 MMIO 레지스터
 * 오프셋, 그 안의 비트필드를 뽑는 매크로(cap_ 계열, ecap_ 계열), 루트/컨텍스트/PASID
 * 항목과 페이지 테이블 항목의 비트 배치, 무효화 명령 서술자의 형식이다.
 * 다른 하나는 커널 쪽 — struct intel_iommu(유닛 하나), struct dmar_domain
 * (주소 공간 하나), struct device_domain_info(장치 하나)라는 세 자료구조와
 * 그것들을 잇는 목록·트리·xarray 다.
 * 이 파일에는 실행 코드가 거의 없다. 대신 intel/ 아래의 모든 .c 파일이 이
 * 정의를 통해서만 하드웨어를 만지고 서로의 상태를 읽는다. 그래서 VT-d 의
 * 동작을 이해하려면 이 헤더의 자료 모델을 먼저 잡아야 한다.
 * 인라인 함수들도 대부분 "레지스터 값에서 능력 비트를 뽑는" 순수 계산이거나,
 * 자료구조 사이를 오가는 짧은 변환(to_dmar_domain, domain_id_iommu)이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IOMMU 스택은 세 층이다.
 *   [코어] drivers/iommu/iommu.c — 장치·그룹·도메인이라는 벤더 중립 모델과,
 *          드라이버가 채우는 iommu_ops 콜백 규약.
 *   [벤더 드라이버] drivers/iommu/intel/ — 이 헤더가 정의하는 세계.
 *          코어의 iommu_domain 을 dmar_domain 으로, 코어의 struct device 를
 *          device_domain_info 로 감싸 VT-d 하드웨어에 대응시킨다.
 *   [하드웨어] DRHD 유닛들. 각 유닛이 자기 아래 PCI 세그먼트/버스의 DMA 를
 *          루트 테이블 → 컨텍스트 테이블 → (scalable 이면 PASID 디렉터리 →
 *          PASID 테이블) → 페이지 테이블 순으로 번역한다.
 * 이 헤더는 두 번째 층의 어휘 전체를 제공한다. 위로는 <linux/iommu.h> 의
 * iommu_domain/iommu_ops 를, 옆으로는 <linux/dmar.h> 의 DRHD 열거를,
 * 아래로는 <linux/generic_pt/iommu.h> 의 공용 페이지 테이블 구현을 끌어와
 * 그 사이를 잇는다.
 * 실행 컨텍스트: 커널 모듈(내장). 여기 정의된 인라인 함수들은 부팅 초기화,
 * 프로세스 컨텍스트의 매핑 경로, 그리고 폴트 인터럽트 문맥에서 모두 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더를 포함하는 파일들과 각자가 쓰는 부분:
 *   iommu.c        — 세 자료구조 전부. 도메인 생성/부착, 장치 프로브, 초기화.
 *   dmar.c         — struct intel_iommu 와 레지스터 정의. DMAR 표 파싱과
 *                    유닛 등록, 무효화 큐(QI)의 하부 구현.
 *   cache.c        — cache_tag 모델. "어느 유닛의 어느 도메인 id 에 무효화를
 *                    보낼지"를 정규화해 중복 무효화를 없앤다.
 *   pasid.c/.h     — PASID 디렉터리와 항목의 비트 배치. scalable 모드의 핵심.
 *   prq.c, svm.c   — 페이지 요청 큐와 SVA. ecap_prs/pasid_supported 등의
 *                    능력 비트로 지원 여부를 판단한다.
 *   irq_remapping.c— 인터럽트 재매핑 테이블. DMA 번역과 별개의 기능이지만
 *                    같은 유닛의 레지스터를 쓴다.
 *   nested.c       — 중첩 변환(게스트 1단계 + 호스트 2단계).
 *   perfmon.c, debugfs.c, perf.c — 성능 카운터와 진단.
 * 데이터 흐름: 부팅 시 dmar.c 가 ACPI DMAR 표를 읽어 struct intel_iommu 를
 * 만들고 레지스터를 매핑한다 → iommu.c 가 그 유닛들에 루트 테이블을 세우고
 * 코어에 등록한다 → 장치가 프로브되면 device_domain_info 가 만들어져
 * 유닛의 device_rbtree 와 도메인의 devices 목록에 동시에 매달린다 →
 * 매핑/언매핑이 일어나면 cache.c 가 cache_tags 를 훑어 필요한 무효화만
 * 큐에 넣는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct intel_iommu: DRHD 유닛 하나. 매핑된 레지스터(reg), 능력(cap/ecap),
 *   루트 테이블, 무효화 큐(qi), 도메인 id 할당기(domain_ida), 소스 id 로
 *   장치를 되찾는 rbtree(device_rbtree), 코어에 등록한 iommu_device.
 * - struct dmar_domain: 주소 공간 하나. 붙은 장치 목록(devices), PASID 쌍
 *   목록(dev_pasids), 무효화 대상 목록(cache_tags), 유닛별 도메인 id
 *   (iommu_array), 1단계/2단계 페이지 테이블(fspt/sspt), 중첩 관계(s1_domains).
 * - struct device_domain_info: 장치 하나. 소스 id(bus/devfn/segment), 담당
 *   유닛, 소속 도메인, ATS/PASID/PRI 능력과 활성 여부, 폴트 참조 계수.
 * - cap_ 계열과 ecap_ 계열 매크로군: 능력 레지스터의 비트필드를 이름으로 읽는다.
 *   sm_supported/pasid_supported/nested_supported 같은 상위 판별자가 그 위에
 *   얹혀, 코드 곳곳의 "이 하드웨어가 이걸 할 수 있는가" 질문에 답한다.
 * - struct qi_desc 와 QI_*_TYPE: 무효화 명령 서술자. 컨텍스트, IOTLB,
 *   디바이스 TLB, PASID, 페이지 요청 응답이 모두 이 형식으로 큐에 들어간다.
 * - struct root_entry / context_entry: 번역 사슬의 첫 두 단계. 컨텍스트 항목의
 *   해석은 scalable 모드 여부에 따라 통째로 달라진다.
 */
#ifndef _INTEL_IOMMU_H_	/* [한국어] 중복 포함 방지 */
#define _INTEL_IOMMU_H_	/* [한국어] 같음 */

#include <linux/types.h>	/* [한국어] 기본 정수 타입 */
#include <linux/iova.h>	/* [한국어] IOVA 할당기. dma-iommu 가 이 도메인 위에 IOVA 공간을 얹는다 */
#include <linux/io.h>	/* [한국어] readl/writeq 등 MMIO 접근. 유닛 레지스터를 다루는 데 필수다 */
#include <linux/idr.h>	/* [한국어] ida — 유닛별 도메인 id 할당에 쓴다 */
#include <linux/mmu_notifier.h>	/* [한국어] SVA 에서 프로세스 페이지 테이블 변경을 통보받는 훅 */
#include <linux/list.h>	/* [한국어] 도메인·장치·유닛을 잇는 목록 */
#include <linux/iommu.h>	/* [한국어] 코어의 iommu_domain/iommu_ops/iommu_device. 이 드라이버가 구현할 규약이다 */
#include <linux/io-64-nonatomic-lo-hi.h>	/* [한국어] 64비트 MMIO 를 32비트 두 번으로 나눠 읽는 폴백. 32비트 커널이나 64비트 접근을 못 하는 플랫폼용이며, 하위 워드를 먼저 읽는 순서가 중요하다 */
#include <linux/dmar.h>	/* [한국어] DRHD 유닛 열거와 ACPI DMAR 표 파싱 인터페이스 */
#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP — 레지스터 비트필드를 안전하게 다룬다 */
#include <linux/xarray.h>	/* [한국어] 도메인의 유닛별 정보를 유닛 순번으로 색인한다 */
#include <linux/perf_event.h>	/* [한국어] perfmon.c 가 유닛 성능 카운터를 perf 서브시스템에 붙인다 */
#include <linux/pci.h>	/* [한국어] 소스 id, ATS/PASID/PRI 능력 구조, 별칭 순회 */
#include <linux/generic_pt/iommu.h>	/* [한국어] 공용 페이지 테이블 구현. 1단계는 x86-64 형식을, 2단계는 vtdss 형식을 이 라이브러리가 다룬다 */

#include <asm/iommu.h>	/* [한국어] 아키텍처별 정의(arch_rmrr_sanity_check 등) */
#include <uapi/linux/iommufd.h>	/* [한국어] 유저스페이스와 주고받는 구조체. hw_info 나 중첩 도메인 생성에 쓴다 */

/*
 * VT-d hardware uses 4KiB page size regardless of host page size.
 */
#define VTD_PAGE_SHIFT		(12)	/* [한국어] VT-d 는 호스트 페이지 크기와 무관하게 항상 4KB 를 쓴다 (위 영어 주석). 호스트가 64KB 페이지라도 IOMMU 항목은 4KB 단위다 */
#define VTD_PAGE_SIZE		(1UL << VTD_PAGE_SHIFT)	/* [한국어] 4096 */
#define VTD_PAGE_MASK		(((u64)-1) << VTD_PAGE_SHIFT)	/* [한국어] 하위 12비트(플래그 자리)를 털어 내는 마스크. 항목에서 주소만 뽑을 때 쓴다 */
#define VTD_PAGE_ALIGN(addr)	(((addr) + VTD_PAGE_SIZE - 1) & VTD_PAGE_MASK)	/* [한국어] 위로 올림 정렬 */

#define IOVA_PFN(addr)		((addr) >> PAGE_SHIFT)	/* [한국어] 호스트 페이지 프레임 번호. 위와 달리 PAGE_SHIFT 를 쓰는 것은 IOVA 할당기가 호스트 페이지 단위로 동작하기 때문이다 */

#define VTD_STRIDE_SHIFT        (9)	/* [한국어] 페이지 테이블 한 단계가 다루는 인덱스 비트 수. 512개 항목 = 2^9 */
#define VTD_STRIDE_MASK         (((u64)-1) << VTD_STRIDE_SHIFT)	/* [한국어] 그 인덱스를 털어 내는 마스크 */

#define DMA_PTE_READ		BIT_ULL(0)	/* [한국어] 2단계 PTE 의 읽기 허용 비트 */
#define DMA_PTE_WRITE		BIT_ULL(1)	/* [한국어] 쓰기 허용 비트. 읽기/쓰기 둘 다 0 이면 그 항목은 없는 것으로 취급된다 — 1단계의 present 비트와 다른 점이다 */
#define DMA_PTE_LARGE_PAGE	BIT_ULL(7)	/* [한국어] 이 항목이 하위 테이블이 아니라 큰 페이지(2MB/1GB)를 직접 가리킨다는 표시 */
#define DMA_PTE_SNP		BIT_ULL(11)	/* [한국어] 이 페이지의 DMA 는 CPU 캐시를 스누프하라는 강제. force snooping 이 이 비트를 쓴다 */

#define DMA_FL_PTE_PRESENT	BIT_ULL(0)	/* [한국어] 1단계 PTE 의 present 비트. 1단계는 x86-64 CPU 형식이라 present 와 권한이 따로 있다 */
#define DMA_FL_PTE_US		BIT_ULL(2)	/* [한국어] User/Supervisor 비트. 장치의 DMA 는 유저 권한으로 취급되므로 대개 켜 둔다 */
#define DMA_FL_PTE_ACCESS	BIT_ULL(5)	/* [한국어] 하드웨어가 이 페이지를 참조했음을 남기는 비트 */
#define DMA_FL_PTE_DIRTY	BIT_ULL(6)	/* [한국어] 1단계에서 쓰기가 있었음을 남기는 비트 */

#define DMA_SL_PTE_DIRTY_BIT	9	/* [한국어] 2단계의 dirty 비트 위치. 1단계와 자리가 다르다 */
#define DMA_SL_PTE_DIRTY	BIT_ULL(DMA_SL_PTE_DIRTY_BIT)	/* [한국어] 그 비트. 라이브 마이그레이션이 이 값을 읽어 어느 페이지를 다시 보낼지 정한다 */

#define ADDR_WIDTH_5LEVEL	(57)	/* [한국어] 5단계 페이지 테이블이 덮는 주소 폭 */
#define ADDR_WIDTH_4LEVEL	(48)	/* [한국어] 4단계가 덮는 주소 폭 */

#define CONTEXT_TT_MULTI_LEVEL	0	/* [한국어] 컨텍스트 항목의 translation type — 페이지 테이블을 워크한다(보통의 번역) */
#define CONTEXT_TT_DEV_IOTLB	1	/* [한국어] 워크하되 디바이스 TLB(ATS)도 함께 쓴다 */
#define CONTEXT_TT_PASS_THROUGH 2	/* [한국어] 번역하지 않고 통과시킨다. IOVA 가 그대로 물리 주소가 된다 */
#define CONTEXT_PASIDE		BIT_ULL(3)	/* [한국어] 이 컨텍스트 항목에서 PASID 를 쓴다는 표시 */

/*
 * Intel IOMMU register specification per version 1.0 public spec.
 */
#define	DMAR_VER_REG	0x0	/* Arch version supported by this IOMMU */	/* [한국어] 이 유닛이 구현한 VT-d 스펙 버전. sysfs 의 version 속성이 그대로 보여 준다 */
#define	DMAR_CAP_REG	0x8	/* Hardware supported capabilities */	/* [한국어] 능력 레지스터. 지원 주소 폭, 도메인 개수, 큰 페이지, caching mode 등 아래 cap_ 매크로들이 읽는 원본 */
#define	DMAR_ECAP_REG	0x10	/* Extended capabilities supported */	/* [한국어] 확장 능력 레지스터. PASID, PRI, scalable mode, 코히런시가 여기 있다 */
#define	DMAR_GCMD_REG	0x18	/* Global command register */	/* [한국어] 전역 명령 레지스터. 번역 켜기/끄기, 루트 테이블 설정 같은 한 번짜리 명령을 여기 쓴다 */
#define	DMAR_GSTS_REG	0x1c	/* Global status register */	/* [한국어] 전역 상태. 명령을 쓴 뒤 이 레지스터의 해당 비트가 바뀔 때까지 기다려야 한다 */
#define	DMAR_RTADDR_REG	0x20	/* Root entry table */	/* [한국어] 루트 테이블의 물리 주소. 하위 비트의 SMT 플래그가 컨텍스트 항목의 해석을 통째로 바꾼다 */
#define	DMAR_CCMD_REG	0x28	/* Context command reg */	/* [한국어] 컨텍스트 캐시 무효화 명령(레지스터 방식). 무효화 큐를 쓰기 전 초기화 단계에서 쓴다 */
#define	DMAR_FSTS_REG	0x34	/* Fault Status register */	/* [한국어] 폴트 상태. 인터럽트 핸들러가 이 값을 읽어 어떤 폴트가 났는지 판단한다 */
#define	DMAR_FECTL_REG	0x38	/* Fault control register */	/* [한국어] 폴트 인터럽트 제어(마스크 등) */
#define	DMAR_FEDATA_REG	0x3c	/* Fault event interrupt data register */	/* [한국어] 폴트 인터럽트의 MSI 데이터 */
#define	DMAR_FEADDR_REG	0x40	/* Fault event interrupt addr register */	/* [한국어] 폴트 인터럽트의 MSI 주소 */
#define	DMAR_FEUADDR_REG 0x44	/* Upper address register */	/* [한국어] 그 주소의 상위 32비트 */
#define	DMAR_PMEN_REG	0x64	/* Enable Protected Memory Region */	/* [한국어] PMR(보호 메모리 영역) 활성화. BIOS 가 켜 둔 것을 커널이 내려 줘야 한다 */
#define	DMAR_PLMBASE_REG 0x68	/* PMRR Low addr */	/* [한국어] PMR 하위 영역의 시작 주소 */
#define	DMAR_PLMLIMIT_REG 0x6c	/* PMRR low limit */	/* [한국어] 그 끝 */
#define	DMAR_PHMBASE_REG 0x70	/* pmrr high base addr */	/* [한국어] PMR 상위 영역의 시작 주소 */
#define	DMAR_PHMLIMIT_REG 0x78	/* pmrr high limit */	/* [한국어] 그 끝 */
#define DMAR_IQH_REG	0x80	/* Invalidation queue head register */	/* [한국어] 무효화 큐의 head(하드웨어가 소비한 지점) */
#define DMAR_IQT_REG	0x88	/* Invalidation queue tail register */	/* [한국어] 무효화 큐의 tail(커널이 채운 지점). 이 값을 쓰는 것이 곧 명령 제출이다 */
#define DMAR_IQ_SHIFT	4	/* Invalidation queue head/tail shift */	/* [한국어] head/tail 값이 서술자 인덱스가 되도록 미는 비트 수. 서술자 하나가 16바이트라 4다 */
#define DMAR_IQA_REG	0x90	/* Invalidation queue addr register */	/* [한국어] 무효화 큐 버퍼의 물리 주소와 크기 */
#define DMAR_ICS_REG	0x9c	/* Invalidation complete status register */	/* [한국어] 무효화 완료 상태. wait 서술자와 함께 완료를 확인하는 데 쓴다 */
#define DMAR_IQER_REG	0xb0	/* Invalidation queue error record register */	/* [한국어] 큐에 넣은 서술자가 거부되었을 때 그 이유와 관련 소스 id 를 담는다 */
#define DMAR_IRTA_REG	0xb8    /* Interrupt remapping table addr register */	/* [한국어] 인터럽트 재매핑 테이블의 주소. DMA 번역과 별개의 기능이다 */
#define DMAR_PQH_REG	0xc0	/* Page request queue head register */	/* [한국어] 페이지 요청 큐의 head */
#define DMAR_PQT_REG	0xc8	/* Page request queue tail register */	/* [한국어] 그 tail. 하드웨어가 채우고 커널이 소비하므로 무효화 큐와 역할이 반대다 */
#define DMAR_PQA_REG	0xd0	/* Page request queue address register */	/* [한국어] 페이지 요청 큐 버퍼의 주소 */
#define DMAR_PRS_REG	0xdc	/* Page request status register */	/* [한국어] 페이지 요청 상태(대기 중인 요청이 있는지, 오버플로가 났는지) */
#define DMAR_PECTL_REG	0xe0	/* Page request event control register */	/* [한국어] 페이지 요청 인터럽트 제어 */
#define	DMAR_PEDATA_REG	0xe4	/* Page request event interrupt data register */	/* [한국어] 그 인터럽트의 MSI 데이터 */
#define	DMAR_PEADDR_REG	0xe8	/* Page request event interrupt addr register */	/* [한국어] 그 MSI 주소 */
#define	DMAR_PEUADDR_REG 0xec	/* Page request event Upper address register */	/* [한국어] 그 주소의 상위 32비트 */
#define DMAR_MTRRCAP_REG 0x100	/* MTRR capability register */	/* [한국어] 가상 MTRR 능력. 게스트에게 MTRR 을 보여 줄 때 쓴다 */
#define DMAR_MTRRDEF_REG 0x108	/* MTRR default type register */	/* [한국어] 가상 MTRR 기본 타입 */
#define DMAR_MTRR_FIX64K_00000_REG 0x120 /* MTRR Fixed range registers */	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX16K_80000_REG 0x128	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX16K_A0000_REG 0x130	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_C0000_REG 0x138	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_C8000_REG 0x140	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_D0000_REG 0x148	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_D8000_REG 0x150	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_E0000_REG 0x158	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_E8000_REG 0x160	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_F0000_REG 0x168	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_FIX4K_F8000_REG 0x170	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE0_REG 0x180 /* MTRR Variable range registers */	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK0_REG 0x188	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE1_REG 0x190	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK1_REG 0x198	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE2_REG 0x1a0	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK2_REG 0x1a8	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE3_REG 0x1b0	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK3_REG 0x1b8	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE4_REG 0x1c0	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK4_REG 0x1c8	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE5_REG 0x1d0	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK5_REG 0x1d8	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE6_REG 0x1e0	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK6_REG 0x1e8	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE7_REG 0x1f0	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK7_REG 0x1f8	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE8_REG 0x200	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK8_REG 0x208	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSBASE9_REG 0x210	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_MTRR_PHYSMASK9_REG 0x218	/* [한국어] 가상 MTRR 범위 레지스터. 게스트에 MTRR 을 에뮬레이션할 때만 쓰이며 DMA 번역과는 무관하다 */
#define DMAR_PERFCAP_REG	0x300	/* [한국어] 성능 카운터 능력. perfmon.c 가 읽는다 */
#define DMAR_PERFCFGOFF_REG	0x310	/* [한국어] 카운터 설정 레지스터들의 오프셋 */
#define DMAR_PERFOVFOFF_REG	0x318	/* [한국어] 오버플로 상태 레지스터의 오프셋 */
#define DMAR_PERFCNTROFF_REG	0x31c	/* [한국어] 카운터 값 레지스터들의 오프셋 */
#define DMAR_PERFINTRSTS_REG	0x324	/* [한국어] 성능 인터럽트 상태 */
#define DMAR_PERFINTRCTL_REG	0x328	/* [한국어] 성능 인터럽트 제어 */
#define DMAR_PERFEVNTCAP_REG	0x380	/* [한국어] 셀 수 있는 이벤트의 목록 */
#define DMAR_ECMD_REG		0x400	/* [한국어] 확장 명령 레지스터. ecmd_submit_sync 가 여기에 명령을 쓴다 */
#define DMAR_ECEO_REG		0x408	/* [한국어] 확장 명령의 피연산자 B */
#define DMAR_ECRSP_REG		0x410	/* [한국어] 확장 명령의 응답. IP 비트가 진행 중 여부를 알려 준다 */
#define DMAR_ECCAP_REG		0x430	/* [한국어] 확장 명령 능력 — 어떤 확장 명령을 지원하는지 */

#define DMAR_IQER_REG_IQEI(reg)		FIELD_GET(GENMASK_ULL(3, 0), reg)	/* [한국어] 큐 오류 레코드에서 오류 종류(Invalidation Queue Error Info)를 뽑는다 */
#define DMAR_IQER_REG_ITESID(reg)	FIELD_GET(GENMASK_ULL(47, 32), reg)	/* [한국어] 오류를 낸 무효화 서술자의 대상 소스 id */
#define DMAR_IQER_REG_ICESID(reg)	FIELD_GET(GENMASK_ULL(63, 48), reg)	/* [한국어] 완료 오류와 관련된 소스 id. 어느 장치가 응답하지 않았는지 알려 준다 */

#define OFFSET_STRIDE		(9)	/* [한국어] 페이지 테이블 인덱스 폭. VTD_STRIDE_SHIFT 와 같은 값이며 오프셋 계산에 쓰인다 */

#define DMAR_VER_MAJOR(v)		(((v) & 0xf0) >> 4)	/* [한국어] 버전 레지스터의 상위 니블 */
#define DMAR_VER_MINOR(v)		((v) & 0x0f)	/* [한국어] 하위 니블 */

/*
 * Decoding Capability Register
 */
#define cap_esrtps(c)		(((c) >> 63) & 1)	/* [한국어] Enhanced Set Root Table Pointer — 루트 테이블 주소를 바꿀 때 하드웨어가 캐시를 알아서 비운다. 있으면 iommu_set_root_entry 가 수동 무효화를 건너뛴다 */
#define cap_esirtps(c)		(((c) >> 62) & 1)	/* [한국어] 인터럽트 재매핑 테이블에 대한 같은 기능 */
#define cap_ecmds(c)		(((c) >> 61) & 1)	/* [한국어] 확장 명령 인터페이스 지원 여부. ecmd_submit_sync 가 이것을 먼저 확인한다 */
#define cap_fl5lp_support(c)	(((c) >> 60) & 1)	/* [한국어] 1단계 5레벨 페이지 테이블 지원. 57비트 주소를 쓸 수 있는지를 정한다 */
#define cap_pi_support(c)	(((c) >> 59) & 1)	/* [한국어] Posted Interrupt 지원. 인터럽트를 VMM 을 거치지 않고 게스트에 직접 전달한다 */
#define cap_fl1gp_support(c)	(((c) >> 56) & 1)	/* [한국어] 1단계 1GB 큰 페이지 지원. 없으면 도메인의 pgsize_bitmap 에서 1GB 를 뺀다 */
#define cap_read_drain(c)	(((c) >> 55) & 1)	/* [한국어] 무효화 시 진행 중인 읽기를 배수(drain)할 수 있는지 */
#define cap_write_drain(c)	(((c) >> 54) & 1)	/* [한국어] 쓰기에 대한 같은 능력. 이것이 없으면 무효화 완료가 진행 중인 DMA 를 보장하지 못한다 */
#define cap_max_amask_val(c)	(((c) >> 48) & 0x3f)	/* [한국어] 한 번의 무효화로 다룰 수 있는 최대 주소 마스크(= 범위 크기의 로그값) */
#define cap_num_fault_regs(c)	((((c) >> 40) & 0xff) + 1)	/* [한국어] 폴트 기록 레지스터의 개수. +1 인 것은 필드가 0 부터 세기 때문이다 */
#define cap_pgsel_inv(c)	(((c) >> 39) & 1)	/* [한국어] 페이지 단위 선택 무효화 지원. 없으면 도메인 전체를 비워야 해서 훨씬 비싸다 */

#define cap_super_page_val(c)	(((c) >> 34) & 0xf)	/* [한국어] 2단계가 지원하는 큰 페이지 크기 비트맵(비트0=2MB, 비트1=1GB) */

#define cap_fault_reg_offset(c)	((((c) >> 24) & 0x3ff) * 16)	/* [한국어] 폴트 기록 레지스터들이 시작되는 오프셋. 필드 값에 16을 곱한다 */
#define cap_max_fault_reg_offset(c) \	/* [한국어] 그 마지막 레지스터의 끝. 폴트 순회의 종료 조건이다 */
	(cap_fault_reg_offset(c) + cap_num_fault_regs(c) * 16)	/* [한국어] 시작 오프셋 + 개수 × 16바이트. 폴트 기록 순회의 종료 조건이다 */

#define cap_zlr(c)		(((c) >> 22) & 1)	/* [한국어] Zero Length Read 지원. 길이 0 읽기 요청을 정상 처리하는지 */
#define cap_isoch(c)		(((c) >> 23) & 1)	/* [한국어] 등시성(ISOCH) 전용 유닛인지. Tylersburg 우회가 이 개념과 얽혀 있다 */
#define cap_mgaw(c)		((((c) >> 16) & 0x3f) + 1)	/* [한국어] Maximum Guest Address Width — 실제로 다룰 수 있는 주소 폭. +1 인 것은 필드가 0 부터 세기 때문이다 */
#define cap_sagaw(c)		(((c) >> 8) & 0x1f)	/* [한국어] Supported Adjusted Guest Address Width — 지원하는 2단계 테이블 단계 수의 비트마스크 */
#define cap_caching_mode(c)	(((c) >> 7) & 1)	/* [한국어] 캐싱 모드. 사실상 "이 IOMMU 는 에뮬레이션된 것" 이라는 신호이며, 매핑을 만들 때도 무효화가 필요해진다 */
#define cap_phmr(c)		(((c) >> 6) & 1)	/* [한국어] 상위 PMR(보호 메모리 영역) 지원 */
#define cap_plmr(c)		(((c) >> 5) & 1)	/* [한국어] 하위 PMR 지원 */
#define cap_rwbf(c)		(((c) >> 4) & 1)	/* [한국어] Required Write-Buffer Flushing. 옛 하드웨어 중 이 비트를 세우지 않으면서 실제로는 필요한 것이 있어 quirk 로 보정한다 */
#define cap_afl(c)		(((c) >> 3) & 1)	/* [한국어] Advanced Fault Logging 지원 */
#define cap_ndoms(c)		(((unsigned long)1) << (4 + 2 * ((c) & 0x7)))	/* [한국어] 동시에 유지할 수 있는 도메인 수. 도메인 id 필드의 비트 폭에서 계산되며, 이 수를 넘으면 새 도메인을 붙일 수 없다 */
/*
 * Extended Capability Register
 */

#define ecap_pms(e)		(((e) >> 51) & 0x1)	/* [한국어] Page-request Message Support */
#define ecap_rps(e)		(((e) >> 49) & 0x1)	/* [한국어] RID-PASID 지원. 소스 id 에 기본 PASID 를 묶을 수 있다 */
#define ecap_smpwc(e)		(((e) >> 48) & 0x1)	/* [한국어] scalable 모드 페이지 워크가 캐시 코히런트한지. 아니면 테이블을 고칠 때마다 clflush 가 필요하다 */
#define ecap_flts(e)		(((e) >> 47) & 0x1)	/* [한국어] 1단계(First-Stage) 변환 지원. 1단계 도메인을 만들 수 있는지의 조건이다 */
#define ecap_slts(e)		(((e) >> 46) & 0x1)	/* [한국어] 2단계(Second-Stage) 변환 지원 */
#define ecap_slads(e)		(((e) >> 45) & 0x1)	/* [한국어] 2단계 접근/더티 비트 지원. 라이브 마이그레이션의 전제다 */
#define ecap_smts(e)		(((e) >> 43) & 0x1)	/* [한국어] Scalable Mode 지원. PASID·SVA·중첩 변환이 모두 이 위에 얹힌다 */
#define ecap_dit(e)		(((e) >> 41) & 0x1)	/* [한국어] Device-TLB Invalidation Throttling. VF 의 무효화에 PF 소스 id 를 실어 큐 깊이를 가늠하게 한다 */
#define ecap_pds(e)		(((e) >> 42) & 0x1)	/* [한국어] Page-request Drain 지원. PASID 를 내릴 때 남은 페이지 요청을 배수한다 */
#define ecap_pasid(e)		(((e) >> 40) & 0x1)	/* [한국어] PASID 지원 */
#define ecap_pss(e)		(((e) >> 35) & 0x1f)	/* [한국어] PASID 필드의 비트 폭 - 1. 쓸 수 있는 PASID 개수를 정한다 */
#define ecap_eafs(e)		(((e) >> 34) & 0x1)	/* [한국어] Extended Accessed Flag 지원 */
#define ecap_nwfs(e)		(((e) >> 33) & 0x1)	/* [한국어] No Write Flag 지원 */
#define ecap_srs(e)		(((e) >> 31) & 0x1)	/* [한국어] Supervisor Request 지원. 커널 주소 공간을 장치가 쓰게 하는 기능 */
#define ecap_ers(e)		(((e) >> 30) & 0x1)	/* [한국어] Execute Request 지원 */
#define ecap_prs(e)		(((e) >> 29) & 0x1)	/* [한국어] Page Request 지원. PRI 와 io-pgfault 처리의 조건이다 */
#define ecap_broken_pasid(e)	(((e) >> 28) & 0x1)	/* [한국어] 이 하드웨어의 PASID 구현에 결함이 있음을 하드웨어 스스로 알린다 */
#define ecap_dis(e)		(((e) >> 27) & 0x1)	/* [한국어] Deferred Invalidate 지원 */
#define ecap_nest(e)		(((e) >> 26) & 0x1)	/* [한국어] 중첩 변환 지원. 게스트의 1단계 테이블 아래에 호스트 2단계를 깔 수 있다 */
#define ecap_mts(e)		(((e) >> 25) & 0x1)	/* [한국어] Memory Type 지원. 페이지 항목에 메모리 타입을 지정할 수 있다 */
#define ecap_iotlb_offset(e) 	((((e) >> 8) & 0x3ff) * 16)	/* [한국어] IOTLB 무효화 레지스터의 오프셋. 필드 값에 16을 곱한다 */
#define ecap_max_iotlb_offset(e) (ecap_iotlb_offset(e) + 16)	/* [한국어] 그 영역의 끝 */
#define ecap_coherent(e)	((e) & 0x1)	/* [한국어] 유닛의 페이지 테이블 워크가 CPU 캐시를 스누프하는지. 아니면 항목을 고칠 때마다 clflush 해야 한다 */
#define ecap_qis(e)		((e) & 0x2)	/* [한국어] Queued Invalidation 지원. 레지스터 방식 대신 큐로 무효화를 보낼 수 있다 */
#define ecap_pass_through(e)	(((e) >> 6) & 0x1)	/* [한국어] 통과 모드 지원 */
#define ecap_eim_support(e)	(((e) >> 4) & 0x1)	/* [한국어] Extended Interrupt Mode — 32비트 APIC id 를 쓸 수 있다(x2APIC) */
#define ecap_ir_support(e)	(((e) >> 3) & 0x1)	/* [한국어] 인터럽트 재매핑 지원 */
#define ecap_dev_iotlb_support(e)	(((e) >> 2) & 0x1)	/* [한국어] 디바이스 IOTLB(ATS) 지원. 장치의 ATS 를 켜기 전 확인하는 첫 조건이다 */
#define ecap_max_handle_mask(e) (((e) >> 20) & 0xf)	/* [한국어] 인터럽트 재매핑에서 한 항목이 다룰 수 있는 최대 핸들 마스크 */
#define ecap_sc_support(e)	(((e) >> 7) & 0x1) /* Snooping Control */	/* [한국어] Snooping Control — 도메인 단위로 캐시 코히런시를 강제할 수 있다. VFIO/KVM 이 게스트에 WBINVD 를 허용할지 정하는 근거다 (위 영어 주석) */

/*
 * Decoding Perf Capability Register
 */
#define pcap_num_cntr(p)	((p) & 0xffff)	/* [한국어] 성능 카운터 개수 */
#define pcap_cntr_width(p)	(((p) >> 16) & 0x7f)	/* [한국어] 카운터 하나의 비트 폭 */
#define pcap_num_event_group(p)	(((p) >> 24) & 0x1f)	/* [한국어] 이벤트 그룹 수 */
#define pcap_filters_mask(p)	(((p) >> 32) & 0x1f)	/* [한국어] 지원하는 필터 종류의 비트마스크 */
#define pcap_interrupt(p)	(((p) >> 50) & 0x1)	/* [한국어] 오버플로 인터럽트 지원 여부 */
/* The counter stride is calculated as 2 ^ (x+10) bytes */
#define pcap_cntr_stride(p)	(1ULL << ((((p) >> 52) & 0x7) + 10))	/* [한국어] 카운터 레지스터 사이의 간격. 2^(x+10) 바이트로 계산된다 (위 영어 주석) */

/*
 * Decoding Perf Event Capability Register
 */
#define pecap_es(p)		((p) & 0xfffffff)	/* [한국어] 셀 수 있는 이벤트의 비트마스크 */

/* Virtual command interface capability */
#define vccap_pasid(v)		(((v) & DMA_VCS_PAS)) /* PASID allocation */	/* [한국어] 가상 명령 인터페이스로 PASID 를 할당할 수 있는지 (위 영어 주석). 게스트 커널이 호스트에 PASID 를 요청하는 경로다 */

/* IOTLB_REG */
#define DMA_TLB_FLUSH_GRANU_OFFSET  60	/* [한국어] IOTLB 무효화 명령에서 범위 종류를 담는 비트 위치 */
#define DMA_TLB_GLOBAL_FLUSH (((u64)1) << 60)	/* [한국어] 전역 — 이 유닛의 IOTLB 전체 */
#define DMA_TLB_DSI_FLUSH (((u64)2) << 60)	/* [한국어] 도메인 단위(Domain-Selective) */
#define DMA_TLB_PSI_FLUSH (((u64)3) << 60)	/* [한국어] 페이지 단위(Page-Selective). 가장 좁고 싸지만 cap_pgsel_inv 가 있어야 쓸 수 있다 */
#define DMA_TLB_IIRG(type) ((type >> 60) & 3)	/* [한국어] 요청한 무효화 범위 종류를 꺼낸다 */
#define DMA_TLB_IAIG(val) (((val) >> 57) & 3)	/* [한국어] 하드웨어가 실제로 수행한 범위 종류. 요청보다 넓어질 수 있어 결과를 확인해야 한다 */
#define DMA_TLB_READ_DRAIN (((u64)1) << 49)	/* [한국어] 무효화와 함께 진행 중인 읽기를 배수하라는 요청 */
#define DMA_TLB_WRITE_DRAIN (((u64)1) << 48)	/* [한국어] 쓰기에 대한 같은 요청. 이것이 있어야 무효화 완료가 "더 이상 옛 번역으로 가는 쓰기가 없다"를 뜻한다 */
#define DMA_TLB_DID(id)	(((u64)((id) & 0xffff)) << 32)	/* [한국어] 무효화 대상 도메인 id 를 자리에 맞춰 넣는다 */
#define DMA_TLB_IVT (((u64)1) << 63)	/* [한국어] Invalidate 명령을 시작시키는 비트. 이 비트를 쓰면 하드웨어가 작업을 시작하고, 끝나면 스스로 내린다 */
#define DMA_TLB_IH_NONLEAF (((u64)1) << 6)	/* [한국어] Invalidation Hint — 잎이 아닌 중간 단계 항목은 그대로 두라는 힌트. 매핑만 바뀌고 테이블 구조는 그대로일 때 무효화 범위를 줄인다 */
#define DMA_TLB_MAX_SIZE (0x3f)	/* [한국어] 주소 마스크 필드의 최대값 */

/* INVALID_DESC */
#define DMA_CCMD_INVL_GRANU_OFFSET  61	/* [한국어] 무효화 서술자에서 범위 종류를 담는 비트 위치 (위 영어 주석: 큐에 넣는 서술자 형식) */
#define DMA_ID_TLB_GLOBAL_FLUSH	(((u64)1) << 4)	/* [한국어] 서술자 방식의 전역 IOTLB 무효화 */
#define DMA_ID_TLB_DSI_FLUSH	(((u64)2) << 4)	/* [한국어] 도메인 단위 */
#define DMA_ID_TLB_PSI_FLUSH	(((u64)3) << 4)	/* [한국어] 페이지 단위 */
#define DMA_ID_TLB_READ_DRAIN	(((u64)1) << 7)	/* [한국어] 진행 중인 읽기 배수 */
#define DMA_ID_TLB_WRITE_DRAIN	(((u64)1) << 6)	/* [한국어] 진행 중인 쓰기 배수 */
#define DMA_ID_TLB_DID(id)	(((u64)((id & 0xffff) << 16)))	/* [한국어] 대상 도메인 id */
#define DMA_ID_TLB_IH_NONLEAF	(((u64)1) << 6)	/* [한국어] 중간 단계 항목은 그대로 두라는 힌트 */
#define DMA_ID_TLB_ADDR(addr)	(addr)	/* [한국어] 무효화할 주소. 별도 변환 없이 그대로 실린다 */
#define DMA_ID_TLB_ADDR_MASK(mask)	(mask)	/* [한국어] 그 범위 크기(로그값). 주소의 정렬이 곧 가능한 최대 범위를 정한다 */

/* PMEN_REG */
#define DMA_PMEN_EPM (((u32)1)<<31)	/* [한국어] 보호 메모리 영역을 켜는 비트. 커널은 이것을 내리기만 한다 */
#define DMA_PMEN_PRS (((u32)1)<<0)	/* [한국어] 현재 보호가 걸려 있는지를 알려 주는 상태 비트. EPM 을 내린 뒤 이 비트가 0 이 될 때까지 기다린다 */

/* GCMD_REG */
#define DMA_GCMD_TE (((u32)1) << 31)	/* [한국어] Translation Enable — 번역을 켜고 끈다. VT-d 의 가장 중요한 스위치다 */
#define DMA_GCMD_SRTP (((u32)1) << 30)	/* [한국어] Set Root Table Pointer — RTADDR 에 쓴 주소를 실제로 채택시킨다 */
#define DMA_GCMD_SFL (((u32)1) << 29)	/* [한국어] Set Fault Log */
#define DMA_GCMD_EAFL (((u32)1) << 28)	/* [한국어] Enable Advanced Fault Logging */
#define DMA_GCMD_WBF (((u32)1) << 27)	/* [한국어] Write Buffer Flush — rwbf 가 필요한 하드웨어에서 내부 버퍼를 비운다 */
#define DMA_GCMD_QIE (((u32)1) << 26)	/* [한국어] Queued Invalidation Enable — 무효화 큐를 켠다 */
#define DMA_GCMD_SIRTP (((u32)1) << 24)	/* [한국어] Set Interrupt Remapping Table Pointer */
#define DMA_GCMD_IRE (((u32) 1) << 25)	/* [한국어] Interrupt Remapping Enable */
#define DMA_GCMD_CFI (((u32) 1) << 23)	/* [한국어] Compatibility Format Interrupt 허용 여부. 재매핑을 우회하는 옛 형식 인터럽트를 막을지 정한다 */

/* GSTS_REG */
#define DMA_GSTS_TES (((u32)1) << 31)	/* [한국어] 번역이 켜져 있는가. init_translation_status 가 이 비트를 읽어 인계 여부를 판단한다 */
#define DMA_GSTS_RTPS (((u32)1) << 30)	/* [한국어] 루트 테이블 주소가 채택되었는가 */
#define DMA_GSTS_FLS (((u32)1) << 29)	/* [한국어] 폴트 로그 상태 */
#define DMA_GSTS_AFLS (((u32)1) << 28)	/* [한국어] 고급 폴트 로그 상태 */
#define DMA_GSTS_WBFS (((u32)1) << 27)	/* [한국어] 쓰기 버퍼 비우기가 진행 중인가 */
#define DMA_GSTS_QIES (((u32)1) << 26)	/* [한국어] 무효화 큐가 켜져 있는가 */
#define DMA_GSTS_IRTPS (((u32)1) << 24)	/* [한국어] 인터럽트 재매핑 테이블 주소가 채택되었는가 */
#define DMA_GSTS_IRES (((u32)1) << 25)	/* [한국어] 인터럽트 재매핑이 켜져 있는가 */
#define DMA_GSTS_CFIS (((u32)1) << 23)	/* [한국어] 호환 형식 인터럽트가 허용되어 있는가 */

/* DMA_RTADDR_REG */
#define DMA_RTADDR_SMT (((u64)1) << 10)	/* [한국어] 루트 테이블 주소의 이 한 비트가 scalable mode 를 켠다. 켜지면 컨텍스트 항목이 페이지 테이블이 아니라 PASID 디렉터리를 가리키는 것으로 해석된다 — 표 전체의 의미를 바꾸는 비트다 */

/* CCMD_REG */
#define DMA_CCMD_ICC (((u64)1) << 63)	/* [한국어] Invalidate Context Cache — 이 비트를 쓰면 하드웨어가 작업을 시작하고 끝나면 스스로 내린다 */
#define DMA_CCMD_GLOBAL_INVL (((u64)1) << 61)	/* [한국어] 컨텍스트 캐시 전체 */
#define DMA_CCMD_DOMAIN_INVL (((u64)2) << 61)	/* [한국어] 한 도메인의 컨텍스트만 */
#define DMA_CCMD_DEVICE_INVL (((u64)3) << 61)	/* [한국어] 한 장치의 컨텍스트만 — 가장 좁다 */
#define DMA_CCMD_FM(m) (((u64)((m) & 0x3)) << 32)	/* [한국어] 함수 마스크. 여러 함수를 한 번에 무효화할 때 하위 몇 비트를 무시할지 정한다 */
#define DMA_CCMD_MASK_NOBIT 0	/* [한국어] 함수 하나만 */
#define DMA_CCMD_MASK_1BIT 1	/* [한국어] 하위 1비트를 무시 — 2개 함수 */
#define DMA_CCMD_MASK_2BIT 2	/* [한국어] 4개 함수 */
#define DMA_CCMD_MASK_3BIT 3	/* [한국어] 8개 함수(한 장치의 모든 함수) */
#define DMA_CCMD_SID(s) (((u64)((s) & 0xffff)) << 16)	/* [한국어] 대상 소스 id */
#define DMA_CCMD_DID(d) ((u64)((d) & 0xffff))	/* [한국어] 대상 도메인 id */

/* ECMD_REG */
#define DMA_MAX_NUM_ECMD		256	/* [한국어] 확장 명령의 최대 개수 */
#define DMA_MAX_NUM_ECMDCAP		(DMA_MAX_NUM_ECMD / 64)	/* [한국어] 그 지원 여부를 담는 64비트 워드의 개수 */
#define DMA_ECMD_REG_STEP		8	/* [한국어] 능력 레지스터 사이의 간격 */
#define DMA_ECMD_ENABLE			0xf0	/* [한국어] 성능 카운터를 켜는 확장 명령 */
#define DMA_ECMD_DISABLE		0xf1	/* [한국어] 끄는 명령 */
#define DMA_ECMD_FREEZE			0xf4	/* [한국어] 카운터를 멈춰 값을 안정적으로 읽게 하는 명령 */
#define DMA_ECMD_UNFREEZE		0xf5	/* [한국어] 다시 진행시키는 명령 */
#define DMA_ECMD_OA_SHIFT		16	/* [한국어] 피연산자 A 가 명령 레지스터에서 차지하는 위치 */
#define DMA_ECMD_ECRSP_IP		0x1	/* [한국어] 응답 레지스터의 진행 중(In Progress) 비트 */
#define DMA_ECMD_ECCAP3			3	/* [한국어] 확장 명령 능력 레지스터 중 3번 워드 */
#define DMA_ECMD_ECCAP3_ECNTS		BIT_ULL(48)	/* [한국어] Enable Counters 명령 지원 */
#define DMA_ECMD_ECCAP3_DCNTS		BIT_ULL(49)	/* [한국어] Disable Counters 지원 */
#define DMA_ECMD_ECCAP3_FCNTS		BIT_ULL(52)	/* [한국어] Freeze Counters 지원 */
#define DMA_ECMD_ECCAP3_UFCNTS		BIT_ULL(53)	/* [한국어] Unfreeze Counters 지원 */
#define DMA_ECMD_ECCAP3_ESSENTIAL	(DMA_ECMD_ECCAP3_ECNTS |	\	/* [한국어] 성능 카운터를 쓰려면 넷 다 있어야 한다. perfmon 초기화가 이 조합으로 한 번에 확인한다 */
					 DMA_ECMD_ECCAP3_DCNTS |	\	/* [한국어] 끄기도 */
					 DMA_ECMD_ECCAP3_FCNTS |	\	/* [한국어] 멈추기도 */
					 DMA_ECMD_ECCAP3_UFCNTS)	/* [한국어] 다시 진행시키기도 모두 지원해야 성능 카운터를 쓸 수 있다 */

/* FECTL_REG */
#define DMA_FECTL_IM (((u32)1) << 31)	/* [한국어] 폴트 인터럽트 마스크. 인터럽트를 세우는 동안 잠시 막는 데 쓴다 */

/* FSTS_REG */
#define DMA_FSTS_PFO (1 << 0) /* Primary Fault Overflow */	/* [한국어] 폴트 기록이 넘쳐 일부를 잃었다 (위 영어 주석) */
#define DMA_FSTS_PPF (1 << 1) /* Primary Pending Fault */	/* [한국어] 처리되지 않은 폴트 기록이 있다 — 핸들러가 이 비트를 보고 순회를 시작한다 */
#define DMA_FSTS_IQE (1 << 4) /* Invalidation Queue Error */	/* [한국어] 무효화 서술자가 거부되었다. IQER 레지스터에 이유가 있다 */
#define DMA_FSTS_ICE (1 << 5) /* Invalidation Completion Error */	/* [한국어] 무효화 완료 오류 — 장치가 잘못된 응답을 보냈다 */
#define DMA_FSTS_ITE (1 << 6) /* Invalidation Time-out Error */	/* [한국어] 무효화 시간 초과 — 장치가 응답하지 않았다. 대개 그 장치를 더 이상 신뢰할 수 없다 */
#define DMA_FSTS_PRO (1 << 7) /* Page Request Overflow */	/* [한국어] 페이지 요청 큐가 넘쳤다 */
#define dma_fsts_fault_record_index(s) (((s) >> 8) & 0xff)	/* [한국어] 처리를 시작할 폴트 기록의 인덱스. 하드웨어가 링 버퍼처럼 채운다 */

/* FRCD_REG, 32 bits access */
#define DMA_FRCD_F (((u32)1) << 31)	/* [한국어] 이 폴트 기록이 유효한지. 처리 후 커널이 이 비트를 써서 지운다 (위 영어 주석: 32비트 단위 접근) */
#define dma_frcd_type(d) ((d >> 30) & 1)	/* [한국어] 읽기 폴트인지 쓰기 폴트인지 */
#define dma_frcd_fault_reason(c) (c & 0xff)	/* [한국어] 폴트 사유 코드. "컨텍스트 항목 없음", "권한 없음" 등을 구분한다 */
#define dma_frcd_source_id(c) (c & 0xffff)	/* [한국어] 폴트를 낸 장치의 소스 id. device_rbtree_find 가 이 값으로 장치를 되찾는다 */
#define dma_frcd_pasid_value(c) (((c) >> 8) & 0xfffff)	/* [한국어] 폴트를 낸 PASID */
#define dma_frcd_pasid_present(c) (((c) >> 31) & 1)	/* [한국어] PASID 필드가 유효한지 */
/* low 64 bit */
#define dma_frcd_page_addr(d) (d & (((u64)-1) << PAGE_SHIFT))	/* [한국어] 폴트가 난 주소. 하위 페이지 오프셋은 잘라 낸다 (위 영어 주석: 하위 64비트) */

/* PRS_REG */
#define DMA_PRS_PPR	((u32)1)	/* [한국어] 대기 중인 페이지 요청이 있다 */
#define DMA_PRS_PRO	((u32)2)	/* [한국어] 페이지 요청 큐가 넘쳤다. 이 경우 잃어버린 요청 때문에 장치가 멈출 수 있다 */

#define DMA_VCS_PAS	((u64)1)	/* [한국어] 가상 명령 인터페이스의 PASID 할당 능력 비트 */

/* PERFINTRSTS_REG */
#define DMA_PERFINTRSTS_PIS	((u32)1)	/* [한국어] 성능 카운터 인터럽트가 걸렸음을 알리는 비트 */

#define IOMMU_WAIT_OP(iommu, offset, op, cond, sts)			\	/* [한국어] 레지스터가 원하는 상태가 될 때까지 도는 관용구. VT-d 의 명령은 대부분 "비트를 쓰고 상태 비트가 바뀔 때까지 기다린다" 형태라 매크로로 뺐다 */
do {									\	/* [한국어] 매크로를 한 문장처럼 쓰게 하는 관용구 */
	cycles_t start_time = get_cycles();				\	/* [한국어] 시작 시각. 타임아웃을 재려면 필요하다 */
	while (1) {							\	/* [한국어] 조건이 맞을 때까지 반복 */
		sts = op(iommu->reg + offset);				\	/* [한국어] 레지스터를 읽는다. op 는 readl/readq 중 하나로 호출자가 넘긴다 */
		if (cond)						\	/* [한국어] 원하는 조건이 되었으면 */
			break;						\	/* [한국어] 기다림 종료 */
		if (DMAR_OPERATION_TIMEOUT < (get_cycles() - start_time))\	/* [한국어] 정해진 시간을 넘겼으면 */
			panic("DMAR hardware is malfunctioning\n");	\	/* [한국어] 부팅을 멈춘다. IOMMU 가 응답하지 않는데 계속 진행하면 격리 상태를 알 수 없어 더 위험하다 */
		cpu_relax();						\	/* [한국어] 바쁜 대기 중임을 CPU 에 알린다(하이퍼스레드 양보, 전력 절약) */
	}								\
} while (0)	/* [한국어] 관용구의 끝. 세미콜론을 붙여 쓸 수 있게 한다 */

#define QI_LENGTH	256	/* queue length */	/* [한국어] 무효화 큐에 담을 수 있는 서술자 수 (위 영어 주석) */

enum {
	QI_FREE,	/* [한국어] 이 슬롯은 비어 있다 */
	QI_IN_USE,	/* [한국어] 서술자가 들어가 하드웨어 처리를 기다린다 */
	QI_DONE,	/* [한국어] 처리가 끝났다 */
	QI_ABORT	/* [한국어] 오류로 중단되었다. 뒤따르던 서술자들도 함께 무효가 된다 */
};

#define QI_CC_TYPE		0x1	/* [한국어] 컨텍스트 캐시 무효화 서술자 */
#define QI_IOTLB_TYPE		0x2	/* [한국어] IOTLB 무효화 */
#define QI_DIOTLB_TYPE		0x3	/* [한국어] 디바이스 IOTLB(ATS) 무효화 */
#define QI_IEC_TYPE		0x4	/* [한국어] 인터럽트 항목 캐시 무효화 */
#define QI_IWD_TYPE		0x5	/* [한국어] Invalidation Wait — 앞의 서술자들이 끝났는지 확인하는 표식. 완료 대기의 핵심이다 */
#define QI_EIOTLB_TYPE		0x6	/* [한국어] 확장(PASID 인식) IOTLB 무효화 */
#define QI_PC_TYPE		0x7	/* [한국어] PASID 캐시 무효화 */
#define QI_DEIOTLB_TYPE		0x8	/* [한국어] 확장 디바이스 IOTLB 무효화 */
#define QI_PGRP_RESP_TYPE	0x9	/* [한국어] 페이지 요청 그룹에 대한 응답. 이 서술자가 장치의 멈춘 요청을 풀어 준다 */
#define QI_PSTRM_RESP_TYPE	0xa	/* [한국어] 페이지 스트림 응답 */

#define QI_IEC_SELECTIVE	(((u64)1) << 4)	/* [한국어] 인터럽트 항목 일부만 무효화 */
#define QI_IEC_IIDEX(idx)	(((u64)(idx & 0xffff) << 32))	/* [한국어] 무효화할 인터럽트 항목의 인덱스 */
#define QI_IEC_IM(m)		(((u64)(m & 0x1f) << 27))	/* [한국어] 그 범위 크기(로그값) */

#define QI_IWD_STATUS_DATA(d)	(((u64)d) << 32)	/* [한국어] 완료 시 상태 주소에 쓸 값. 커널은 그 값이 나타나는지 폴링해 완료를 안다 */
#define QI_IWD_STATUS_WRITE	(((u64)1) << 5)	/* [한국어] 완료 시 그 값을 실제로 쓰라는 지시 */
#define QI_IWD_FENCE		(((u64)1) << 6)	/* [한국어] 펜스 — 이 서술자 앞의 것이 모두 끝나야 뒤의 것이 시작된다. 순서가 중요한 무효화 사이에 끼운다 */
#define QI_IWD_PRQ_DRAIN	(((u64)1) << 7)	/* [한국어] 대기 중인 페이지 요청까지 배수한다. PASID 를 내릴 때 남은 요청이 없도록 보장한다 */

#define QI_IOTLB_DID(did) 	(((u64)did) << 16)	/* [한국어] 대상 도메인 id */
#define QI_IOTLB_DR(dr) 	(((u64)dr) << 7)	/* [한국어] 읽기 배수 요청 */
#define QI_IOTLB_DW(dw) 	(((u64)dw) << 6)	/* [한국어] 쓰기 배수 요청 */
#define QI_IOTLB_GRAN(gran) 	(((u64)gran) >> (DMA_TLB_FLUSH_GRANU_OFFSET-4))	/* [한국어] 레지스터 방식의 범위 종류 값을 서술자 자리로 옮긴다. 두 형식이 같은 값을 다른 위치에 두어 시프트로 변환한다 */
#define QI_IOTLB_ADDR(addr)	(((u64)addr) & VTD_PAGE_MASK)	/* [한국어] 무효화할 주소(페이지 정렬) */
#define QI_IOTLB_IH(ih)		(((u64)ih) << 6)	/* [한국어] 중간 단계는 그대로 두라는 힌트 */
#define QI_IOTLB_AM(am)		(((u8)am) & 0x3f)	/* [한국어] 범위 크기(로그값) */

#define QI_CC_FM(fm)		(((u64)fm) << 48)	/* [한국어] 함수 마스크 */
#define QI_CC_SID(sid)		(((u64)sid) << 32)	/* [한국어] 대상 소스 id */
#define QI_CC_DID(did)		(((u64)did) << 16)	/* [한국어] 대상 도메인 id */
#define QI_CC_GRAN(gran)	(((u64)gran) >> (DMA_CCMD_INVL_GRANU_OFFSET-4))	/* [한국어] 레지스터 형식의 범위 종류를 서술자 자리로 옮긴다 */

#define QI_DEV_IOTLB_SID(sid)	((u64)((sid) & 0xffff) << 32)	/* [한국어] 디바이스 TLB 를 비울 장치의 소스 id */
#define QI_DEV_IOTLB_QDEP(qdep)	(((qdep) & 0x1f) << 16)	/* [한국어] 그 장치의 ATS 큐 깊이. 이보다 많이 보내면 응답이 유실된다 */
#define QI_DEV_IOTLB_ADDR(addr)	((u64)(addr) & VTD_PAGE_MASK)	/* [한국어] 무효화할 주소 */
#define QI_DEV_IOTLB_PFSID(pfsid) (((u64)(pfsid & 0xf) << 12) | \	/* [한국어] VF 의 무효화에 PF 의 소스 id 를 싣는다. 필드가 두 조각으로 나뉘어 있어 하위 4비트와 상위 12비트를 따로 넣는다 */
				   ((u64)((pfsid >> 4) & 0xfff) << 52))
#define QI_DEV_IOTLB_SIZE	1	/* [한국어] 범위 지정 방식의 플래그 */
#define QI_DEV_IOTLB_MAX_INVS	32	/* [한국어] 한 번에 보낼 수 있는 디바이스 TLB 무효화의 최대 개수 */

#define QI_PC_PASID(pasid)	(((u64)pasid) << 32)	/* [한국어] 무효화할 PASID */
#define QI_PC_DID(did)		(((u64)did) << 16)	/* [한국어] 대상 도메인 id */
#define QI_PC_GRAN(gran)	(((u64)gran) << 4)	/* [한국어] 범위 종류 */

/* PASID cache invalidation granu */
#define QI_PC_ALL_PASIDS	0	/* [한국어] 이 도메인의 모든 PASID (위 영어 주석) */
#define QI_PC_PASID_SEL		1	/* [한국어] 지정한 PASID 하나만 */
#define QI_PC_GLOBAL		3	/* [한국어] 유닛 전체의 PASID 캐시 */

#define QI_EIOTLB_ADDR(addr)	((u64)(addr) & VTD_PAGE_MASK)	/* [한국어] 확장 IOTLB 무효화의 주소 */
#define QI_EIOTLB_IH(ih)	(((u64)ih) << 6)	/* [한국어] 중간 단계 유지 힌트 */
#define QI_EIOTLB_AM(am)	(((u64)am) & 0x3f)	/* [한국어] 범위 크기 */
#define QI_EIOTLB_PASID(pasid) 	(((u64)pasid) << 32)	/* [한국어] 대상 PASID. 이 필드가 있는 것이 확장 형식과 기본 형식의 차이다 */
#define QI_EIOTLB_DID(did)	(((u64)did) << 16)	/* [한국어] 대상 도메인 id */
#define QI_EIOTLB_GRAN(gran) 	(((u64)gran) << 4)	/* [한국어] 범위 종류 */

/* QI Dev-IOTLB inv granu */
#define QI_DEV_IOTLB_GRAN_ALL		1	/* [한국어] 이 장치의 모든 PASID (위 영어 주석) */
#define QI_DEV_IOTLB_GRAN_PASID_SEL	0	/* [한국어] 지정한 PASID 만 */

#define QI_DEV_EIOTLB_ADDR(a)	((u64)(a) & VTD_PAGE_MASK)	/* [한국어] 확장 디바이스 TLB 무효화의 주소 */
#define QI_DEV_EIOTLB_SIZE	(((u64)1) << 11)	/* [한국어] 범위 지정 플래그 */
#define QI_DEV_EIOTLB_PASID(p)	((u64)((p) & 0xfffff) << 32)	/* [한국어] 대상 PASID */
#define QI_DEV_EIOTLB_SID(sid)	((u64)((sid) & 0xffff) << 16)	/* [한국어] 대상 소스 id */
#define QI_DEV_EIOTLB_QDEP(qd)	((u64)((qd) & 0x1f) << 4)	/* [한국어] 장치의 ATS 큐 깊이 */
#define QI_DEV_EIOTLB_PFSID(pfsid) (((u64)(pfsid & 0xf) << 12) | \	/* [한국어] PF 소스 id. 기본 형식과 마찬가지로 두 조각으로 나뉜다 */
				    ((u64)((pfsid >> 4) & 0xfff) << 52))
#define QI_DEV_EIOTLB_MAX_INVS	32	/* [한국어] 한 번에 보낼 수 있는 최대 개수 */

/* Page group response descriptor QW0 */
#define QI_PGRP_PASID_P(p)	(((u64)(p)) << 4)	/* [한국어] 응답 서술자에 PASID 필드가 유효한지 표시 (위 영어 주석: QW0) */
#define QI_PGRP_RESP_CODE(res)	(((u64)(res)) << 12)	/* [한국어] 응답 코드. 이 값이 장치에 "다시 시도하라"인지 "포기하라"인지를 알려 준다 */
#define QI_PGRP_DID(rid)	(((u64)(rid)) << 16)	/* [한국어] 응답을 받을 장치의 소스 id */
#define QI_PGRP_PASID(pasid)	(((u64)(pasid)) << 32)	/* [한국어] 응답 대상 PASID */

/* Page group response descriptor QW1 */
#define QI_PGRP_IDX(idx)	(((u64)(idx)) << 3)	/* [한국어] 페이지 요청 그룹 인덱스. 장치가 보낸 요청과 응답을 짝지어 주는 번호이며, 이것이 틀리면 장치가 영원히 기다린다 (위 영어 주석: QW1) */


#define QI_RESP_SUCCESS		0x0	/* [한국어] 매핑을 채웠으니 다시 시도하라 */
#define QI_RESP_INVALID		0x1	/* [한국어] 유효하지 않은 요청 — 그 주소에 접근할 권한이 없다 */
#define QI_RESP_FAILURE		0xf	/* [한국어] 처리할 수 없다. 장치는 대개 이 응답을 받으면 오류로 처리한다 */

#define QI_GRAN_NONG_PASID		2	/* [한국어] PASID 단위의 비-전역 무효화 범위 */
#define QI_GRAN_PSI_PASID		3	/* [한국어] PASID 단위의 페이지 선택 무효화 */

#define qi_shift(iommu)		(DMAR_IQ_SHIFT + !!ecap_smts((iommu)->ecap))	/* [한국어] 서술자 하나의 크기(로그값). scalable 모드면 서술자가 32바이트로 커져 한 칸 더 민다 — 큐의 head/tail 계산이 모드에 따라 달라지는 이유다 */

struct qi_desc {
	u64 qw0;
	/* [한국어] 무효화 서술자의 첫 워드. 하위 4비트가 명령 종류(QI_CC_TYPE, QI_IOTLB_TYPE 등)이고
	 * 나머지 비트의 의미는 그 종류마다 다르다.
	 * 설정자: qi_flush_context()/qi_flush_iotlb()/qi_flush_dev_iotlb() 등 dmar.c 의
	 *   각 무효화 헬퍼가 QI_* 매크로로 필드를 조립해 채운다.
	 * 읽는 자: 하드웨어. qi_submit_sync() 가 큐에 복사한 뒤 tail 레지스터를 쓰면
	 *   유닛이 메모리에서 직접 읽어 간다.
	 * 값 범위: 종류마다 다르지만 하위 4비트는 반드시 유효한 QI_*_TYPE 이어야 한다.
	 *   잘못된 종류는 IQER 레지스터에 오류로 기록되고 서술자가 거부된다.
	 * 동기화: q_inval->q_lock 아래에서만 채운다. 채운 뒤 tail 을 쓰기 전에
	 *   메모리 배리어가 필요하다 — 하드웨어가 옛 내용을 읽으면 안 되기 때문이다. */
	u64 qw1;
	/* [한국어] 둘째 워드. 대부분의 종류에서 무효화할 주소와 범위 크기가 여기 들어간다.
	 * 설정자/읽는 자: qw0 과 같다.
	 * 값 범위: 주소는 페이지 정렬(VTD_PAGE_MASK)이어야 하고, 범위 크기(AM)는
	 *   cap_max_amask_val 이 정한 상한을 넘을 수 없다. 주소의 정렬 자체가
	 *   한 번에 무효화할 수 있는 최대 범위를 제한한다.
	 * 동기화: qw0 과 같다. */
	u64 qw2;
	/* [한국어] 셋째 워드. scalable 모드에서 서술자가 32바이트로 커질 때만 쓰인다.
	 * 설정자: PASID 를 다루는 확장 무효화(QI_EIOTLB_TYPE 등)와 페이지 요청 응답.
	 * 읽는 자: 하드웨어. 16바이트 서술자를 쓰는 유닛은 이 워드를 읽지 않는다.
	 * 값 범위: 쓰이지 않는 경우 0 이어야 한다 — 예약 필드에 값이 있으면 거부된다.
	 * 동기화: qw0 과 같다. */
	u64 qw3;
	/* [한국어] 넷째 워드. qw2 와 같은 조건에서 쓰인다.
	 * 설정자/읽는 자/동기화: qw2 와 같다.
	 * 값 범위: 서술자 크기는 qi_shift(iommu) 가 정하며, 그 값이 16바이트를
	 *   가리키면 이 워드와 qw2 는 큐에 아예 복사되지 않는다. */
};

struct q_inval {
	raw_spinlock_t  q_lock;
	/* [한국어] 이 무효화 큐 전체(서술자 링, 상태 배열, head/tail/cnt)를 지키는 락.
	 * 설정자/읽는 자: qi_submit_sync() 와 그 하위 경로. 서술자를 넣고 tail 을 쓰고
	 *   완료를 기다리는 동안 계속 쥐고 있는다.
	 * raw 스핀락인 이유: PREEMPT_RT 커널에서 보통의 spinlock 은 잠들 수 있는
	 *   뮤텍스로 바뀐다. 그런데 이 구간은 하드웨어와 head/tail 로 핸드셰이크하는
	 *   중이라 선점되면 안 되고, 인터럽트 문맥에서도 무효화가 일어날 수 있다.
	 *   그래서 RT 에서도 진짜 스핀락으로 남는 raw 판을 쓴다.
	 * 동기화 범위: 유닛 하나당 하나. 유닛이 여럿이면 서로 독립적으로 진행한다. */
	void		*desc;          /* invalidation queue */
	/* [한국어] 서술자 링 버퍼. 하드웨어가 메모리에서 직접 읽어 가므로 물리적으로 연속이고
	 * 페이지 정렬된 메모리여야 한다 (원 주석: invalidation queue).
	 * 설정자: dmar_enable_qi() 가 할당하고 IQA 레지스터에 그 물리 주소를 알린다.
	 * 읽는 자: 커널은 qi_submit_sync() 에서 채우고, 하드웨어는 head~tail 구간을 읽는다.
	 * 값 범위: 서술자 하나의 크기는 qi_shift(iommu) 가 정한다 — 레거시 16바이트,
	 *   scalable 모드 32바이트. 그래서 같은 QI_LENGTH 라도 버퍼 크기가 달라진다.
	 * 동기화: q_lock 아래에서만 쓴다. 쓴 뒤 tail 을 갱신하기 전에 배리어가 필요하다. */
	int             *desc_status;   /* desc status */
	/* [한국어] 각 서술자 슬롯의 상태(QI_FREE / QI_IN_USE / QI_DONE / QI_ABORT).
	 * 하드웨어는 이 배열의 존재를 모른다 — 순수하게 커널이 완료를 추적하려고 둔 것이다
	 * (원 주석: desc status).
	 * 설정자: qi_submit_sync() 가 슬롯을 잡을 때 QI_IN_USE 로, 완료를 확인하면
	 *   QI_DONE 으로, 오류가 나면 QI_ABORT 로 바꾼다.
	 * 읽는 자: 같은 함수의 완료 대기 루프와, 오류 복구 경로(qi_check_fault).
	 * 값 범위: 위 네 값 중 하나. QI_ABORT 는 이 슬롯뿐 아니라 뒤따르던 서술자들도
	 *   무효가 되었음을 뜻해서, 복구 경로가 그것들을 다시 제출한다.
	 * 동기화: q_lock 아래. */
	int             free_head;      /* first free entry */
	/* [한국어] 다음에 채울 빈 슬롯의 인덱스 (원 주석: first free entry).
	 * 설정자: qi_submit_sync() 가 슬롯을 하나 쓸 때마다 앞으로 민다(QI_LENGTH 로 감싼다).
	 * 읽는 자: 같은 함수. 이 값이 하드웨어 tail 레지스터에 쓸 값의 근거가 된다.
	 * 값 범위: 0 ~ QI_LENGTH-1.
	 * 동기화: q_lock 아래. */
	int             free_tail;      /* last free entry */
	/* [한국어] 아직 하드웨어가 소비하지 않은 구간의 끝 (원 주석: last free entry).
	 * free_tail 부터 free_head 직전까지가 "제출했지만 아직 완료되지 않은" 서술자들이다.
	 * 설정자: 완료가 확인된 슬롯을 회수할 때 앞으로 민다.
	 * 읽는 자: 빈 슬롯이 있는지 판단하는 계산.
	 * 값 범위: 0 ~ QI_LENGTH-1.
	 * 동기화: q_lock 아래. */
	int             free_cnt;
	/* [한국어] 남아 있는 빈 슬롯 수.
	 * 설정자: 슬롯을 쓰면 줄이고 회수하면 늘린다.
	 * 읽는 자: qi_submit_sync() 가 제출 전에 확인한다. 0 이면 하드웨어가 소비할
	 *   때까지 기다려야 하므로, 이 값이 자주 0 이 되는 것은 무효화가 밀리고 있다는 뜻이다.
	 * 값 범위: 0 ~ QI_LENGTH. head/tail 로도 계산할 수 있지만, 링이 가득 찬 경우와
	 *   빈 경우를 구분하기 위해 개수를 따로 둔다.
	 * 동기화: q_lock 아래. */
};

/* Page Request Queue depth */
#define PRQ_ORDER	4	/* [한국어] 페이지 요청 큐의 크기 지수 (위 영어 주석) */
#define PRQ_SIZE	(SZ_4K << PRQ_ORDER)	/* [한국어] 64KB */
#define PRQ_RING_MASK	(PRQ_SIZE - 0x20)	/* [한국어] 링 인덱스를 감싸는 마스크. 요청 하나가 32바이트라 그만큼 뺀다 */
#define PRQ_DEPTH	(PRQ_SIZE >> 5)	/* [한국어] 담을 수 있는 요청 개수 = 크기 / 32 */

struct dmar_pci_notify_info;	/* [한국어] 전방 선언 — PCI 핫플러그 알림 정보. 실제 정의는 <linux/dmar.h> 에 있다 */

#ifdef CONFIG_IRQ_REMAP
#define INTR_REMAP_TABLE_REG_SIZE	0xf	/* [한국어] IRTA 레지스터의 크기 필드 최대값 */
#define INTR_REMAP_TABLE_REG_SIZE_MASK  0xf	/* [한국어] 그 필드를 뽑는 마스크 */

#define INTR_REMAP_TABLE_ENTRIES	65536	/* [한국어] 인터럽트 재매핑 테이블의 항목 수. 이 수가 시스템 전체의 재매핑 가능한 인터럽트 상한이다 */

struct irq_domain;	/* [한국어] 전방 선언 — 커널 인터럽트 도메인 */

struct ir_table {
	struct irte *base;
	/* [한국어] 인터럽트 재매핑 테이블의 시작 주소. 항목 하나(struct irte)가 벡터 번호,
	 * 목적지 CPU(APIC id), 전달 방식(고정/최저우선), 그리고 이 인터럽트를 낼 수 있는
	 * 소스 id 를 담는다.
	 * 설정자: intel_setup_irq_remapping() 이 유닛마다 할당하고 IRTA 레지스터에 알린다.
	 * 읽는 자: 하드웨어가 인터럽트 메시지를 받을 때마다 인덱스로 이 표를 찾는다.
	 *   커널 쪽은 인터럽트를 할당·변경할 때 항목을 고친다.
	 * 값 범위: INTR_REMAP_TABLE_ENTRIES(65536) 개. 이 수가 시스템의 재매핑 가능한
	 *   인터럽트 상한이다.
	 * 동기화: irq_2_ir_lock. 항목을 고친 뒤에는 반드시 인터럽트 항목 캐시를
	 *   무효화(QI_IEC_TYPE)해야 하드웨어가 새 값을 본다. */
	unsigned long *bitmap;
	/* [한국어] 어느 항목이 사용 중인지 추적하는 비트맵.
	 * 설정자: 인터럽트를 할당하면 해당 비트를 세우고, 해제하면 지운다.
	 * 읽는 자: alloc_irte() 가 빈 자리를 찾을 때. 연속된 여러 항목이 필요한 경우
	 *   (다중 벡터 MSI)도 있어 비트맵 위에서 연속 구간을 찾는다.
	 * 값 범위: INTR_REMAP_TABLE_ENTRIES 비트.
	 * 동기화: irq_2_ir_lock. */
};

void intel_irq_remap_add_device(struct dmar_pci_notify_info *info);	/* [한국어] 새 PCI 장치를 인터럽트 재매핑에 등록한다 */
#else
static inline void
intel_irq_remap_add_device(struct dmar_pci_notify_info *info) { }	/* [한국어] 재매핑을 끈 빌드의 빈 구현. 호출부에 #ifdef 를 흩지 않으려는 관용구다 */
#endif

struct iommu_flush {
	void (*flush_context)(struct intel_iommu *iommu, u16 did, u16 sid,
			      u8 fm, u64 type);
	void (*flush_iotlb)(struct intel_iommu *iommu, u16 did, u64 addr,
			    unsigned int size_order, u64 type);
};

enum {
	SR_DMAR_FECTL_REG,	/* [한국어] 서스펜드 때 저장할 레지스터 — 폴트 인터럽트 제어 */
	SR_DMAR_FEDATA_REG,	/* [한국어] 폴트 인터럽트 MSI 데이터 */
	SR_DMAR_FEADDR_REG,	/* [한국어] 폴트 인터럽트 MSI 주소 */
	SR_DMAR_FEUADDR_REG,	/* [한국어] 그 상위 32비트. 리줌 때 이 넷을 되살려야 폴트 보고가 계속 동작한다 */
	MAX_SR_DMAR_REGS	/* [한국어] 저장할 레지스터 개수. 배열 크기로 쓰인다 */
};

#define VTD_FLAG_TRANS_PRE_ENABLED	(1 << 0)	/* [한국어] 커널이 시작하기 전에 이미 번역이 켜져 있었다(kdump/펌웨어 인계) */
#define VTD_FLAG_IRQ_REMAP_PRE_ENABLED	(1 << 1)	/* [한국어] 인터럽트 재매핑도 마찬가지로 이미 켜져 있었다 */
#define VTD_FLAG_SVM_CAPABLE		(1 << 2)	/* [한국어] 이 유닛으로 SVA 를 쓸 수 있다. intel_svm_check 가 세운다 */

#define sm_supported(iommu)	(intel_iommu_sm && ecap_smts((iommu)->ecap))	/* [한국어] scalable 모드를 실제로 쓰는가. 하드웨어 지원(ecap_smts)과 부트 옵션(intel_iommu_sm) 둘 다 필요하다 — 아래 판별자들이 모두 이것을 전제로 한다 */
#define pasid_supported(iommu)	(sm_supported(iommu) &&			\	/* [한국어] PASID 를 쓸 수 있는가. scalable 모드 위에서만 성립한다 */
				 ecap_pasid((iommu)->ecap))
#define ssads_supported(iommu) (sm_supported(iommu) &&                 \	/* [한국어] 2단계 더티 추적을 쓸 수 있는가. 추적 비트(slads)뿐 아니라 워크 코히런시(smpwc)까지 요구하는 것은, 하드웨어가 남긴 비트를 CPU 가 캐시를 거치지 않고 읽어야 하기 때문이다 */
				ecap_slads((iommu)->ecap) &&           \
				ecap_smpwc(iommu->ecap))
#define nested_supported(iommu)	(sm_supported(iommu) &&			\	/* [한국어] 중첩 변환을 쓸 수 있는가 */
				 ecap_nest((iommu)->ecap))

struct pasid_entry;	/* [한국어] 전방 선언 — PASID 테이블의 항목. 정의는 pasid.h 에 있다 */
struct pasid_state_entry;	/* [한국어] PASID 상태 항목 */
struct page_req_dsc;	/* [한국어] 페이지 요청 큐에 들어오는 요청 하나. 정의는 prq.c 쪽에 있다 */

/*
 * 0: Present
 * 1-11: Reserved
 * 12-63: Context Ptr (12 - (haw-1))
 * 64-127: Reserved
 */
struct root_entry {
	u64     lo;
	/* [한국어] 루트 항목의 하위 64비트 — devfn 0~127 을 담당하는 "하위" 컨텍스트 테이블을 가리킨다.
	 * 비트 배치는 바로 위 영어 주석 그대로다: 0=present, 1-11 예약, 12-63=컨텍스트
	 * 테이블의 물리 주소(12 ~ haw-1).
	 * 설정자: iommu_context_addr() 이 그 버스의 컨텍스트 테이블을 처음 만들 때.
	 * 읽는 자: root_entry_lctp() 과 하드웨어. 하드웨어는 소스 id 의 버스 번호로
	 *   루트 테이블을 색인해 이 항목에 닿는다.
	 * 값 범위: present 가 0 이면 그 버스에는 아무 설정이 없다는 뜻이고, 그 버스의
	 *   모든 DMA 가 폴트로 끝난다.
	 * 동기화: iommu->lock. 항목을 고친 뒤 비코히런트 유닛에서는 clflush 가 필요하다. */
	u64     hi;
	/* [한국어] 루트 항목의 상위 64비트 — devfn 128~255 를 담당하는 "상위" 컨텍스트 테이블.
	 * 왜 둘로 나뉘는가: 컨텍스트 항목 하나가 16바이트라 4KB 한 페이지에 256개가
	 *   아니라 128개만 들어간다. 그래서 한 버스의 컨텍스트 테이블을 두 페이지로
	 *   쪼개고, 루트 항목의 lo/hi 가 각각을 가리킨다.
	 * 설정자/읽는 자/동기화: lo 와 같다. 읽는 쪽은 root_entry_uctp() 다.
	 * 값 범위: 위 영어 주석이 64-127 을 예약이라고 적은 것은 스펙 초판 기준이며,
	 *   실제로는 lo 와 같은 형식으로 상위 테이블을 가리킨다. */
};

/*
 * low 64 bits:
 * 0: present
 * 1: fault processing disable
 * 2-3: translation type
 * 12-63: address space root
 * high 64 bits:
 * 0-2: address width
 * 3-6: aval
 * 8-23: domain id
 */
struct context_entry {
	u64 lo;
	/* [한국어] 컨텍스트 항목의 하위 64비트. 비트 배치는 위 영어 주석 그대로다:
	 *   0=present, 1=fault processing disable, 2-3=translation type,
	 *   12-63=address space root.
	 * translation type 이 이 항목의 성격을 정한다 — CONTEXT_TT_MULTI_LEVEL 이면
	 *   address space root 가 페이지 테이블의 최상위를 가리키고,
	 *   CONTEXT_TT_PASS_THROUGH 면 그 필드를 하드웨어가 무시하고 번역 없이 통과시킨다.
	 * 결정적으로, 루트 테이블 주소에 DMA_RTADDR_SMT 가 켜져 있으면(scalable 모드)
	 *   같은 비트들이 전혀 다르게 읽힌다 — address space root 가 페이지 테이블이
	 *   아니라 PASID 디렉터리를 가리키는 것으로 해석된다. 한 비트가 이 구조체의
	 *   의미를 통째로 바꾸는 셈이다.
	 * 설정자: domain_context_mapping_one()(레거시), context_setup_pass_through(),
	 *   copy_context_table()(kdump 인계).
	 * 읽는 자: 하드웨어가 소스 id 의 devfn 으로 색인해 읽는다.
	 * 동기화: iommu->lock. present 는 반드시 마지막에 세운다 — 그 순간부터
	 *   하드웨어가 항목을 쓰기 때문이다. */
	u64 hi;
	/* [한국어] 컨텍스트 항목의 상위 64비트: 0-2=address width, 3-6=aval, 8-23=domain id
	 * (위 영어 주석).
	 * address width 는 이 장치가 쓸 페이지 테이블의 단계 수를 정하고, 통과 모드에서는
	 *   하드웨어가 지원하는 최대 AGAW 로 프로그램해야 한다.
	 * domain id 는 IOTLB 항목을 구분하는 태그다. 같은 도메인 id 를 쓰는 장치들의
	 *   번역은 하드웨어 캐시에서 공유되므로, 도메인 단위 무효화 한 번이 그 모두에
	 *   적용된다 — cache.c 의 cache tag 모델이 이 성질 위에 세워져 있다.
	 * 설정자/읽는 자/동기화: lo 와 같다.
	 * 값 범위: domain id 는 16비트이지만 실제 상한은 cap_ndoms(cap) 이다(스펙 9.3). */
};

struct iommu_domain_info {
	struct intel_iommu *iommu;
	/* [한국어] 이 정보가 어느 유닛에 대한 것인지.
	 * 설정자: domain_attach_iommu() 가 도메인에 그 유닛의 장치가 처음 붙을 때 만든다.
	 * 읽는 자: 무효화 경로가 어느 유닛의 큐로 명령을 보낼지 정할 때.
	 * 값 범위: NULL 이 아닌 유효한 유닛. 이 구조체는 도메인의 iommu_array 에
	 *   유닛 순번(seq_id)으로 색인되어 들어간다.
	 * 동기화: 도메인의 lock. 생성과 해제가 그 락 아래에서 일어난다. */
	unsigned int refcnt;		/* Refcount of devices per iommu */
	/* [한국어] 이 도메인에 붙어 있는, 이 유닛 아래의 장치 수 (원 주석: Refcount of devices per iommu).
	 * 설정자: domain_attach_iommu() 가 늘리고 domain_detach_iommu() 가 줄인다.
	 * 읽는 자: 같은 두 함수. 0 이 되는 순간 아래의 did 를 유닛에 반납하고
	 *   이 구조체 자체를 없앤다.
	 * 왜 필요한가: 도메인 id 는 유닛마다 개수가 정해진 희소 자원(cap_ndoms)이다.
	 *   같은 도메인에 같은 유닛의 장치가 여럿 붙어도 id 는 하나면 되고, 마지막
	 *   장치가 떠날 때 반납해야 한다. 그 시점을 이 계수가 알려 준다.
	 * 동기화: 도메인의 lock. */
	u16 did;			/* Domain ids per IOMMU. Use u16 since
					 * domain ids are 16 bit wide according
					 * to VT-d spec, section 9.3 */
	/* [한국어] 이 유닛에서 이 도메인에 할당된 도메인 id (원 주석: 스펙 9.3 에 따라 16비트).
	 * 설정자: domain_attach_iommu() 가 유닛의 domain_ida 에서 할당한다.
	 * 읽는 자: 컨텍스트/PASID 항목을 채울 때, 그리고 모든 도메인 단위 무효화에서.
	 * 값 범위: 0 ~ cap_ndoms(iommu->cap)-1. 16비트 타입이지만 실제 상한은 유닛마다
	 *   다르며, 그 수를 다 쓰면 그 유닛에는 새 도메인을 붙일 수 없다
	 *   (sysfs 의 domains_used/domains_supported 가 이 상태를 보여 준다).
	 * 왜 유닛마다 따로인가: 도메인 id 는 유닛의 IOTLB 를 구분하는 태그일 뿐,
	 *   시스템 전역의 이름이 아니다. 같은 도메인이라도 유닛이 다르면 다른 id 를
	 *   받을 수 있어서, 도메인은 이 정보를 유닛별로 xarray 에 담아 둔다.
	 * 동기화: 도메인의 lock 아래에서 할당·반납한다. */
};

/*
 * We start simply by using a fixed size for the batched descriptors. This
 * size is currently sufficient for our needs. Future improvements could
 * involve dynamically allocating the batch buffer based on actual demand,
 * allowing us to adjust the batch size for optimal performance in different
 * scenarios.
 */
#define QI_MAX_BATCHED_DESC_COUNT 16	/* [한국어] 한 번에 모아 보낼 무효화 서술자의 개수. 지금은 고정값이면 충분하고, 나중에 수요에 따라 동적으로 잡는 개선을 생각해 볼 수 있다 (위 영어 주석) */
struct qi_batch {
	struct qi_desc descs[QI_MAX_BATCHED_DESC_COUNT];
	/* [한국어] 모아 둔 무효화 서술자들.
	 * 왜 모으는가: 무효화 하나를 보낼 때마다 큐 락을 잡고 tail 을 쓰고 완료를
	 *   기다리면, 큰 범위를 언매핑할 때 그 왕복 비용이 전부가 된다. 여러 개를
	 *   채워 한 번에 제출하면 락 획득과 완료 대기가 한 번으로 줄어든다.
	 * 설정자: cache.c 의 qi_batch_add_* 계열이 채운다.
	 * 읽는 자: qi_batch_flush_descs() 가 통째로 큐에 제출한다.
	 * 값 범위: 앞의 index 개만 유효하다. 나머지는 이전 배치의 잔재가 남아 있을 수
	 *   있으므로 index 를 넘어 읽으면 안 된다.
	 * 동기화: 도메인의 cache_lock. 이 버퍼는 도메인 하나에 하나씩 있다. */
	unsigned int index;
	/* [한국어] 지금까지 채운 서술자 수이자, 다음에 채울 자리.
	 * 설정자: 서술자를 하나 넣을 때마다 늘리고, 제출한 뒤 0 으로 되돌린다.
	 * 읽는 자: 배열이 가득 찼는지(QI_MAX_BATCHED_DESC_COUNT 에 닿았는지) 확인해
	 *   중간 제출을 트리거하는 판단.
	 * 값 범위: 0 ~ QI_MAX_BATCHED_DESC_COUNT.
	 * 동기화: 도메인의 cache_lock. */
};

/*
 * [한국어] struct dmar_domain — VT-d 가 다루는 "주소 공간 하나"
 *
 * 코어의 struct iommu_domain 을 감싸는 이 드라이버의 도메인 표현이다. 하나의
 * 도메인에 여러 장치가 붙을 수 있고, 그 장치들은 같은 IOVA→PA 매핑을 공유한다.
 *
 * 첫 union 이 이 구조체의 성격을 정한다. 도메인은 네 가지 얼굴 중 하나로
 * 쓰이며, 어느 얼굴인지는 domain.type 과 domain.ops 로 구분한다.
 *   - domain: 코어가 보는 얼굴. 모든 경우에 유효하다.
 *   - iommu:  공용 페이지 테이블 라이브러리가 보는 얼굴.
 *   - fspt:   1단계(x86-64 형식) 페이지 테이블을 쓸 때.
 *   - sspt:   2단계(VT-d 고유 형식) 페이지 테이블을 쓸 때.
 * 이들이 union 인 것은 세 표현이 같은 메모리를 겹쳐 쓰되 항상 첫 필드가
 * iommu_domain 이도록 배치되어 있기 때문이다 — 아래 PT_IOMMU_CHECK_DOMAIN 이
 * 그 배치를 컴파일 타임에 확인한다.
 *
 * 마지막 union 은 용도별 추가 상태다. DMA 재매핑 도메인은 자기 위에 얹힌
 * 1단계 도메인 목록(s1_domains)을, 중첩 사용자 도메인은 자기가 얹힌 부모
 * (s2_domain)와 게스트가 준 설정(s1_cfg)을, SVA 도메인은 mmu_notifier 를
 * 갖는다. 한 도메인이 이 셋 중 둘일 수 없으므로 union 으로 겹쳤다.
 *
 * 목록이 네 개인 이유를 함께 보면 구조가 보인다.
 *   devices     — PASID 없이 붙은 장치들. 도메인 단위 설정을 적용할 대상.
 *   dev_pasids  — PASID 단위로 붙은 (장치, PASID) 쌍들.
 *   cache_tags  — 위 둘에서 유도되지만 중복이 제거된 "무효화를 보낼 곳" 목록.
 *   s1_domains  — 중첩 자식들.
 * 앞의 둘은 "무엇이 붙어 있는가"를, cache_tags 는 "무효화를 어디로 보낼
 * 것인가"를 답한다. 여러 장치가 같은 유닛의 같은 도메인 id 를 쓰면 무효화는
 * 한 번이면 되므로, 그 정규화를 위해 목록을 따로 둔다.
 *
 * 락이 셋인 이유: lock 은 장치 목록을, cache_lock 은 무효화 대상 목록을,
 * s1_lock 은 중첩 자식 목록을 지킨다. 무효화는 장치 부착보다 훨씬 자주
 * 일어나므로 두 락을 나눠야 매핑 경로가 부착 경로에 막히지 않는다.
 */
struct dmar_domain {
	union {
		struct iommu_domain domain;
		/* [한국어] 코어가 보는 얼굴. iommu_domain_ops, pgsize_bitmap, dirty_ops 가 여기 있다.
		 * 설정자: 도메인 생성 함수들이 ops 와 pgsize_bitmap 을 채운다.
		 * 읽는 자: IOMMU 코어 전체. to_dmar_domain() 이 이 포인터에서 감싸는
		 *   dmar_domain 을 되찾는다.
		 * 값 범위: type 이 이 도메인의 종류(PAGING/IDENTITY/BLOCKED/SVA/NESTED)를 정하며,
		 *   아래 union 중 어느 얼굴이 유효한지도 그 값으로 갈린다.
		 * 동기화: 생성 시점에 채우고 이후 대부분 읽기만 한다. */
		struct pt_iommu iommu;
		/* [한국어] 공용 페이지 테이블 라이브러리(generic_pt)가 보는 얼굴.
		 * 설정자: 도메인 생성 시 iommu_device 와 nid 를 채운 뒤 pt_iommu_*_init() 에 넘긴다.
		 * 읽는 자: 라이브러리의 map/unmap/iova_to_phys 구현.
		 * 값 범위: fspt/sspt 중 무엇을 쓰든 이 얼굴로 접근할 수 있도록 두 구조체 모두
		 *   첫 필드가 pt_iommu 다.
		 * 동기화: 라이브러리 내부 규약을 따른다. */
		/* First stage page table */
		struct pt_iommu_x86_64 fspt;
		/* [한국어] 1단계 페이지 테이블 (원 주석: First stage page table).
		 * 형식이 x86-64 CPU 페이지 테이블과 같아서, 프로세스의 페이지 테이블을 그대로
		 * 가리키는 SVA 로 자연스럽게 이어진다.
		 * 설정자: intel_iommu_domain_alloc_first_stage() 가 pt_iommu_x86_64_init() 으로 만든다.
		 * 읽는 자: 매핑/언매핑 경로와, 붙이기 전 호환성 검사
		 *   (paging_domain_compatible_first_stage 가 max_vasz_lg2 와 features 를 확인한다).
		 * 값 범위: intel_domain_is_fs_paging() 이 참일 때만 유효하다.
		 * 동기화: 라이브러리가 페이지 테이블 갱신의 원자성을 책임진다. */
		/* Second stage page table */
		struct pt_iommu_vtdss sspt;
		/* [한국어] 2단계 페이지 테이블 (원 주석: Second stage page table).
		 * VT-d 고유 형식이며, 레거시 모드에서 쓸 수 있는 유일한 형식이자 가상화에서
		 * 호스트가 소유하는 하위 단계다. PTE 마다 SNP 비트가 있어 강제 코히런시를
		 * 항목 단위로 표현할 수 있다는 점이 1단계와 다르다.
		 * 설정자: intel_iommu_domain_alloc_second_stage() 가 pt_iommu_vtdss_init() 으로 만든다.
		 * 읽는 자: 매핑/언매핑, 호환성 검사, 더티 비트 조회.
		 * 값 범위: intel_domain_is_ss_paging() 이 참일 때만 유효하다.
		 * 동기화: 라이브러리가 책임진다. */
	};

	struct xarray iommu_array;	/* Attached IOMMU array */
	/* [한국어] 이 도메인이 쓰는 유닛별 정보(struct iommu_domain_info)를 유닛 순번(seq_id)으로
	 * 색인한다 (원 주석: Attached IOMMU array).
	 * 왜 배열이 필요한가: 도메인 id 는 시스템 전역의 이름이 아니라 유닛마다 따로
	 *   할당되는 태그다. 한 도메인에 여러 유닛의 장치가 붙으면 유닛마다 다른
	 *   id 를 받게 되므로, "이 도메인이 저 유닛에서는 몇 번인가"를 따로 들고 있어야 한다.
	 * 설정자: domain_attach_iommu() 가 새 항목을 넣고 domain_detach_iommu() 가 뺀다.
	 * 읽는 자: domain_id_iommu() 와 모든 무효화 경로.
	 * 값 범위: 항목이 없는 유닛 순번은 NULL — 그 유닛에는 이 도메인의 장치가 없다.
	 * 동기화: 도메인의 lock. xarray 자체도 내부 락을 갖지만, 참조 계수와 함께
	 *   갱신해야 해서 바깥 락으로 묶는다. */

	u8 force_snooping:1;		/* Create PASID entry with snoop control */
	/* [한국어] 이 도메인의 모든 매핑이 CPU 캐시를 스누프하도록 강제되었는가
	 * (원 주석: Create PASID entry with snoop control).
	 * 설정자: intel_iommu_enforce_cache_coherency_fs()/_ss(). VFIO/KVM 이 장치를
	 *   게스트에 넘기기 전에 요청한다.
	 * 읽는 자: 이후 이 도메인에 장치를 붙이는 경로. 새로 붙는 장치에도 같은 설정을
	 *   해 줘야 하므로 dmar_domain_attach_device 가 이 값을 본다.
	 * 값 범위: 한 번 켜면 끄지 않는다 — 이미 그 보장을 믿고 있는 사용자가 있기 때문이다.
	 * 왜 필요한가: 이 보장이 있으면 게스트가 캐시를 직접 관리하지 않아도 되고,
	 *   호스트가 게스트에 WBINVD 같은 위험한 명령을 허용하지 않아도 된다.
	 * 동기화: 도메인의 lock 아래에서 검사와 설정을 함께 한다. */
	u8 dirty_tracking:1;		/* Dirty tracking is enabled */
	/* [한국어] 더티 추적이 켜져 있는가 (원 주석: Dirty tracking is enabled).
	 * 설정자: intel_iommu_set_dirty_tracking(). 모든 장치·PASID·중첩 자식에 적용이
	 *   성공한 뒤에야 갱신한다 — 실패 시 되돌릴 원래 값이 필요하기 때문이다.
	 * 읽는 자: 같은 함수(중복 설정 회피)와, 새로 붙는 장치에 같은 설정을 적용하는 경로.
	 * 값 범위: 0/1. ssads_supported 인 유닛에서만 1 이 될 수 있다.
	 * 동기화: 도메인의 lock. */
	u8 nested_parent:1;		/* Has other domains nested on it */
	/* [한국어] 이 도메인 위에 다른(1단계) 도메인이 얹혀 있을 수 있는가
	 * (원 주석: Has other domains nested on it).
	 * 설정자: intel_iommu_domain_alloc_second_stage() 가 IOMMU_HWPT_ALLOC_NEST_PARENT
	 *   플래그를 그대로 옮긴다. 생성 시점에 정해지고 이후 바뀌지 않는다.
	 * 읽는 자: intel_iommu_domain_free() 가 자식이 남아 있는지 확인할 때,
	 *   더티 추적을 자식까지 전파할지 정할 때, 그리고 호환성 검사.
	 * 값 범위: 0/1. 1 이면 이 도메인의 페이지 테이블에 읽기 전용 매핑을 만들 수
	 *   없다(ERRATA_772415_SPR17) — 생성 시 FORCE_WRITEABLE 로 강제된다.
	 * 동기화: 생성 시 한 번 쓰고 이후 읽기만 한다. */
	u8 iotlb_sync_map:1;		/* Need to flush IOTLB cache or write
					 * buffer when creating mappings.
					 */
	/* [한국어] 매핑을 "만들 때"도 무효화가 필요한 도메인인가
	 * (원 주석: Need to flush IOTLB cache or write buffer when creating mappings).
	 * 보통의 IOMMU 는 없던 항목이 생기는 것뿐이라 매핑 시 무효화가 필요 없다.
	 * 두 경우가 예외다.
	 *   - rwbf 가 필요한 옛 유닛: 내부 쓰기 버퍼를 비우지 않으면 우리가 쓴 항목이
	 *     하드웨어에 보이지 않는다.
	 *   - caching mode(에뮬레이션된 IOMMU): "여기엔 매핑이 없다"까지 캐시하므로,
	 *     그 캐시를 지워야 새 매핑이 보인다.
	 * 설정자: 도메인 생성 시 유닛의 능력을 보고 정한다.
	 * 읽는 자: intel_iommu_iotlb_sync_map() 이 이 값을 보고 무효화를 보낼지 정한다.
	 *   호환성 검사도 이 값이 유닛의 요구와 맞는지 확인한다.
	 * 값 범위: 0/1. 생성 시 정해지고 바뀌지 않는다.
	 * 동기화: 생성 시 한 번 쓴다. */

	spinlock_t lock;		/* Protect device tracking lists */
	/* [한국어] 아래 devices/dev_pasids 목록과 위 비트필드 상태를 지키는 락
	 * (원 주석: Protect device tracking lists).
	 * 설정자/읽는 자: 장치 부착·분리, force_snooping/dirty_tracking 설정, 그리고
	 *   그 목록을 훑는 모든 경로.
	 * irqsave 로 잡는 이유: 이 목록을 훑는 코드가 인터럽트를 끈 문맥에서도 불릴 수
	 *   있어서다. cache_lock 과 나눈 이유는, 무효화가 장치 부착보다 훨씬 자주
	 *   일어나므로 두 경로가 서로를 막지 않게 하기 위해서다.
	 * 동기화 범위: 이 도메인 하나. */
	struct list_head devices;	/* all devices' list */
	/* [한국어] PASID 없이 이 도메인에 붙은 장치들의 목록 (원 주석: all devices list).
	 * 각 항목은 struct device_domain_info 의 link 필드다.
	 * 설정자: dmar_domain_attach_device() 가 넣고 device_block_translation() 이 뺀다.
	 * 읽는 자: 도메인 단위 설정을 적용할 때(더티 추적, force snooping),
	 *   그리고 강제 코히런시 지원 여부를 판단할 때.
	 * 값 범위: 비어 있을 수 있다. 도메인 해제 시 비어 있지 않으면 코어가 순서를
	 *   어긴 것이라 WARN 을 남기고 해제를 거부한다.
	 * 동기화: 위 lock. */
	struct list_head dev_pasids;	/* all attached pasids */
	/* [한국어] PASID 단위로 이 도메인에 붙은 (장치, PASID) 쌍들의 목록
	 * (원 주석: all attached pasids). 각 항목은 struct dev_pasid_info 다.
	 * 왜 devices 와 나뉘는가: 한 장치가 여러 PASID 로 서로 다른 도메인에 붙을 수
	 *   있고, 반대로 한 도메인에 같은 장치의 여러 PASID 가 붙을 수도 있다.
	 *   장치 목록만으로는 그 다대다 관계를 표현할 수 없다.
	 * 설정자: domain_add_dev_pasid() 가 넣고 domain_remove_dev_pasid() 가 뺀다.
	 * 읽는 자: 도메인 단위 설정을 PASID 항목에도 적용할 때.
	 * 동기화: 위 lock. */

	spinlock_t cache_lock;		/* Protect the cache tag list */
	/* [한국어] cache_tags 목록과 qi_batch 를 지키는 락 (원 주석: Protect the cache tag list).
	 * 설정자/읽는 자: cache.c 의 태그 등록·해제와 모든 무효화 경로.
	 * lock 과 분리한 이유: 무효화는 매 언매핑마다 일어나고 장치 부착은 드물다.
	 *   하나의 락으로 묶으면 잦은 무효화가 부착을 막거나 그 반대가 된다.
	 * 동기화 범위: 이 도메인 하나. 인터럽트 문맥에서도 잡히므로 irqsave 를 쓴다. */
	struct list_head cache_tags;	/* Cache tag list */
	/* [한국어] 무효화를 실제로 보낼 곳들의 목록 (원 주석: Cache tag list).
	 * 각 항목은 (유닛, 도메인 id, 장치, PASID, 태그 종류)를 담으며, 태그 종류는
	 * IOTLB / DEVTLB / NESTING_IOTLB / NESTING_DEVTLB 중 하나다.
	 * 왜 devices 에서 매번 계산하지 않는가: 같은 유닛의 같은 도메인 id 를 쓰는
	 *   장치가 여럿이면 IOTLB 무효화는 한 번이면 된다. 그 중복 제거를 미리 해 두면
	 *   무효화 경로가 이 목록만 훑으면 되고, 매번 장치 목록을 훑으며 중복을
	 *   걸러 낼 필요가 없다. 무효화가 뜨거운 경로라 그 차이가 크다.
	 * 설정자: cache_tag_assign_domain()/cache_tag_unassign_domain().
	 * 읽는 자: cache_tag_flush_range()/flush_all()/flush_range_np().
	 * 동기화: 위 cache_lock. */
	struct qi_batch *qi_batch;	/* Batched QI descriptors */
	/* [한국어] 이 도메인의 무효화 명령을 모아 두는 버퍼 (원 주석: Batched QI descriptors).
	 * 설정자: 처음 필요할 때 할당하고, 도메인 해제 시 kfree 한다.
	 * 읽는 자: cache.c 의 배치 경로. 서술자를 채우다 가득 차거나 한 무효화 묶음이
	 *   끝나면 통째로 큐에 제출한다.
	 * 값 범위: NULL 일 수 있다(아직 무효화를 보낸 적이 없는 도메인).
	 * 동기화: 위 cache_lock. */

	union {
		/* DMA remapping domain */
		struct {
			/* Protect the s1_domains list */
			spinlock_t	s1_lock;
			/* [한국어] 아래 s1_domains 목록을 지키는 락 (원 주석: Protect the s1_domains list).
			 * 설정자/읽는 자: 중첩 도메인의 생성·해제와, 더티 추적을 자식들에 전파하는 경로.
			 * lock/cache_lock 과 별개인 이유: 중첩 관계 변경은 장치 부착이나 무효화와
			 *   전혀 다른 빈도로 일어나고, 자식마다 그 자식의 lock 을 다시 잡아야 해서
			 *   락 순서를 명확히 나눠 두는 편이 안전하다.
			 * 인터럽트를 끄지 않는 평범한 spin_lock 을 쓴다 — 이 목록은 인터럽트 문맥에서
			 *   건드려지지 않는다. */
			/* Track s1_domains nested on this domain */
			struct list_head s1_domains;
			/* [한국어] 이 2단계 도메인을 부모로 삼아 얹힌 1단계(게스트) 도메인들의 목록
			 * (원 주석: Track s1_domains nested on this domain).
			 * 설정자: 중첩 도메인이 만들어질 때 자식이 자기 s2_link 로 여기 매달린다.
			 * 읽는 자: parent_domain_set_dirty_tracking() 이 설정을 자식까지 전파할 때,
			 *   그리고 intel_iommu_domain_free() 가 자식이 남아 있는지 확인할 때.
			 * 값 범위: nested_parent 가 0 인 도메인에서는 항상 비어 있다.
			 * 동기화: 위 s1_lock. */
		};

		/* Nested user domain */
		struct {
			/* parent page table which the user domain is nested on */
			struct dmar_domain *s2_domain;
			/* [한국어] 이 중첩 사용자 도메인이 얹혀 있는 부모(2단계) 도메인
			 * (원 주석: parent page table which the user domain is nested on).
			 * 중첩 변환에서 게스트의 DMA 는 이 도메인의 1단계 테이블로 게스트 물리 주소를
			 * 얻고, 그 주소를 다시 부모의 2단계 테이블로 호스트 물리 주소로 바꾼다.
			 * 설정자: intel_iommu_domain_alloc_nested() 가 사용자가 지정한 부모를 기록한다.
			 * 읽는 자: 무효화 경로 — 자식의 매핑을 무효화할 때 부모의 도메인 id 가 필요하다.
			 * 값 범위: NULL 이 아니어야 하며, 그 부모는 nested_parent 로 표시되어 있어야 한다.
			 * 동기화: 생성 시 한 번 쓰고 이후 읽기만 한다. */
			/* page table attributes */
			struct iommu_hwpt_vtd_s1 s1_cfg;
			/* [한국어] 게스트가 준 1단계 페이지 테이블의 설정 (원 주석: page table attributes).
			 * 게스트 테이블의 물리 주소와 플래그(주소 폭, 쓰기 보호 등)가 들어 있다.
			 * 설정자: iommufd 가 유저스페이스에서 받은 값을 그대로 옮긴다.
			 * 읽는 자: PASID 항목을 세울 때. 호스트는 이 테이블을 파싱하지 않고 주소만
			 *   하드웨어에 넘긴다 — 워크는 하드웨어가 한다.
			 * 값 범위: 유저스페이스가 준 값이므로 신뢰할 수 없다. 그래서 호스트는 이
			 *   테이블이 가리키는 주소를 2단계 매핑으로 한 번 더 걸러 낸다.
			 * 동기화: 생성 시 한 번 쓴다. */
			/* link to parent domain siblings */
			struct list_head s2_link;
			/* [한국어] 부모의 s1_domains 목록에 매달리는 고리 (원 주석: link to parent domain siblings).
			 * 설정자: 중첩 도메인 생성 시 부모의 목록에 넣고, 해제 시 뺀다.
			 * 읽는 자: 부모가 자식들을 훑는 모든 경로.
			 * 동기화: 부모의 s1_lock. */
		};

		/* SVA domain */
		struct {
			struct mmu_notifier notifier;
			/* [한국어] SVA 도메인이 프로세스의 주소 공간 변경을 통보받는 훅.
			 * SVA 도메인은 자기 페이지 테이블을 갖지 않고 프로세스의 것을 그대로 가리킨다.
			 * 그래서 프로세스 쪽에서 매핑이 바뀌면(munmap, 페이지 회수 등) 그 사실을
			 * 알아 IOMMU 캐시를 비워야 하는데, 그 통지를 이 notifier 가 받는다.
			 * 설정자: intel_svm_domain_alloc() 이 등록한다.
			 * 읽는 자: mm 서브시스템이 콜백을 부른다 — invalidate_range 가 오면 해당
			 *   범위의 IOTLB 와 디바이스 TLB 를 비운다.
			 * 실행 컨텍스트: mm 의 잠금 아래에서 불리므로 콜백 안에서 잠들면 안 된다.
			 * 동기화: mmu_notifier 서브시스템의 규약을 따른다. */
		};
	};
};
PT_IOMMU_CHECK_DOMAIN(struct dmar_domain, iommu, domain);	/* [한국어] union 의 세 얼굴이 같은 자리에서 iommu_domain 을 시작하는지 컴파일 타임에 확인한다. 이 배치가 깨지면 to_dmar_domain() 이 엉뚱한 포인터를 돌려주므로 런타임까지 갈 수 없는 오류다 */
PT_IOMMU_CHECK_DOMAIN(struct dmar_domain, sspt.iommu, domain);	/* [한국어] 2단계 얼굴에 대한 같은 확인 */
PT_IOMMU_CHECK_DOMAIN(struct dmar_domain, fspt.iommu, domain);	/* [한국어] 1단계 얼굴에 대한 같은 확인 */

/*
 * In theory, the VT-d 4.0 spec can support up to 2 ^ 16 counters.
 * But in practice, there are only 14 counters for the existing
 * platform. Setting the max number of counters to 64 should be good
 * enough for a long time. Also, supporting more than 64 counters
 * requires more extras, e.g., extra freeze and overflow registers,
 * which is not necessary for now.
 */
#define IOMMU_PMU_IDX_MAX		64	/* [한국어] 성능 카운터의 최대 개수. 스펙상 2^16 까지 가능하지만 실제 플랫폼에는 14개뿐이라 64 면 한동안 충분하고, 그 이상은 추가 freeze/overflow 레지스터가 필요해 지금은 다루지 않는다 (위 영어 주석) */

struct iommu_pmu {
	struct intel_iommu	*iommu;
	/* [한국어] 이 성능 카운터 묶음이 속한 VT-d 유닛. 카운터는 유닛마다 따로 있다.
	 * 설정자: alloc_iommu_pmu() 가 유닛 초기화 때 채운다.
	 * 읽는 자: perf 콜백들이 레지스터를 만질 때 유닛의 reg 를 얻는 경로.
	 * 동기화: 생성 시 한 번 쓰고 이후 읽기만 한다. */
	u32			num_cntr;	/* Number of counters */
	/* [한국어] 이 유닛이 가진 카운터 개수 (원 주석: Number of counters).
	 * 설정자: pcap_num_cntr() 로 능력 레지스터에서 읽는다.
	 * 읽는 자: 카운터 할당 시 범위 검사, 그리고 used_mask 순회의 상한.
	 * 값 범위: 1 ~ IOMMU_PMU_IDX_MAX. 하드웨어가 더 많다고 신고해도 64 로 자른다.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u32			num_eg;		/* Number of event group */
	/* [한국어] 이벤트 그룹 수 (원 주석: Number of event group).
	 * 한 카운터가 셀 수 있는 이벤트는 그룹으로 묶여 있고, 카운터마다 지원하는
	 * 그룹이 다를 수 있다. 그래서 아래 cntr_evcap 이 카운터별 목록을 따로 둔다.
	 * 설정자: pcap_num_event_group() 로 읽는다.
	 * 읽는 자: cntr_evcap 배열의 두 번째 차원 크기.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u32			cntr_width;	/* Counter width */
	/* [한국어] 카운터 하나의 비트 폭 (원 주석: Counter width).
	 * 설정자: pcap_cntr_width() 로 읽는다.
	 * 읽는 자: 카운터 값을 읽어 perf 이벤트에 더할 때. 폭을 알아야 랩어라운드를
	 *   올바로 처리한다 — 이 값보다 상위 비트는 의미가 없다.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u32			cntr_stride;	/* Counter Stride */
	/* [한국어] 카운터 레지스터 사이의 바이트 간격 (원 주석: Counter Stride).
	 * 설정자: pcap_cntr_stride() 가 2^(x+10) 으로 계산해 준다.
	 * 읽는 자: n번 카운터의 주소를 cntr_reg + n * cntr_stride 로 구하는 계산.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u32			filter;		/* Bitmask of filter support */
	/* [한국어] 지원하는 필터 종류의 비트마스크 (원 주석: Bitmask of filter support).
	 * 필터는 "이 요청만 세라"는 조건이다 — 특정 도메인 id, 소스 id, PASID 등.
	 * 설정자: pcap_filters_mask() 로 읽는다.
	 * 읽는 자: 사용자가 요청한 필터를 이 유닛이 지원하는지 확인하는 경로.
	 * 동기화: 초기화 시 한 번 쓴다. */
	void __iomem		*base;		/* the PerfMon base address */
	/* [한국어] 성능 카운터 레지스터 영역의 시작 (원 주석: the PerfMon base address).
	 * 설정자: 유닛의 reg 에 DMAR_PERFCAP_REG 계열이 알려 준 오프셋을 더해 구한다.
	 * 읽는 자: 아래 세 포인터가 모두 이 값에서 파생된다.
	 * 동기화: 초기화 시 한 번 쓴다. */
	void __iomem		*cfg_reg;	/* counter configuration base address */
	/* [한국어] 카운터 설정 레지스터들의 시작 (원 주석: counter configuration base address).
	 * 어떤 이벤트를 셀지, 어떤 필터를 걸지를 여기에 쓴다.
	 * 설정자: base + DMAR_PERFCFGOFF_REG 가 알려 준 오프셋.
	 * 읽는 자: perf 의 add/del/start/stop 콜백.
	 * 동기화: 초기화 시 한 번 쓴다. */
	void __iomem		*cntr_reg;	/* counter 0 address*/
	/* [한국어] 0번 카운터의 주소 (원 주석: counter 0 address). n번은 여기에
	 * n * cntr_stride 를 더해 얻는다.
	 * 설정자: base + DMAR_PERFCNTROFF_REG 가 알려 준 오프셋.
	 * 읽는 자: 카운터 값을 읽는 모든 경로.
	 * 동기화: 초기화 시 한 번 쓴다. */
	void __iomem		*overflow;	/* overflow status register */
	/* [한국어] 오버플로 상태 레지스터 (원 주석: overflow status register).
	 * 카운터가 넘치면 여기 비트가 서고, 인터럽트가 걸린다.
	 * 설정자: base + DMAR_PERFOVFOFF_REG 가 알려 준 오프셋.
	 * 읽는 자: 오버플로 인터럽트 핸들러가 어느 카운터가 넘쳤는지 확인한다.
	 * 동기화: 초기화 시 한 번 쓴다. */

	u64			*evcap;		/* Indicates all supported events */
	/* [한국어] 이 유닛이 셀 수 있는 모든 이벤트의 비트마스크 배열
	 * (원 주석: Indicates all supported events). 그룹마다 한 워드다.
	 * 설정자: 초기화 때 DMAR_PERFEVNTCAP_REG 를 그룹 수만큼 읽어 채운다.
	 * 읽는 자: 사용자가 요청한 이벤트를 이 유닛이 셀 수 있는지 확인.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u32			**cntr_evcap;	/* Supported events of each counter. */
	/* [한국어] 카운터별로 셀 수 있는 이벤트 목록 (원 주석: Supported events of each counter).
	 * 왜 evcap 과 따로인가: 유닛 전체가 셀 수 있는 이벤트와, 특정 카운터가 셀 수
	 *   있는 이벤트가 다를 수 있다. 이벤트를 요청받으면 그것을 셀 수 있는 카운터를
	 *   골라야 하므로 카운터별 목록이 필요하다.
	 * 설정자: 초기화 때 카운터마다 능력 레지스터를 읽어 2차원 배열로 만든다.
	 * 읽는 자: 카운터 할당 로직.
	 * 동기화: 초기화 시 한 번 쓴다. */

	struct pmu		pmu;
	/* [한국어] perf 서브시스템에 등록하는 PMU 객체. 이벤트 추가·삭제·시작·정지 콜백이 여기 달린다.
	 * 설정자: iommu_pmu_register() 가 채워 perf_pmu_register() 에 넘긴다.
	 * 읽는 자: perf 코어. 유저스페이스가 perf stat 으로 이 PMU 의 이벤트를 요청하면
	 *   여기 달린 콜백이 불린다.
	 * 동기화: perf 코어의 규약을 따른다. */
	DECLARE_BITMAP(used_mask, IOMMU_PMU_IDX_MAX);
	/* [한국어] 어느 카운터가 사용 중인지 나타내는 비트맵.
	 * 설정자: 이벤트를 카운터에 배정할 때 세우고, 놓을 때 지운다.
	 * 읽는 자: 빈 카운터를 찾는 할당 로직.
	 * 동기화: perf 코어가 이 PMU 의 콜백을 직렬화해 준다. */
	struct perf_event	*event_list[IOMMU_PMU_IDX_MAX];
	/* [한국어] 카운터 번호 → 그 카운터에 배정된 perf 이벤트.
	 * 설정자: 이벤트 배정/해제 시.
	 * 읽는 자: 오버플로 인터럽트 핸들러가 넘친 카운터 번호로 이벤트를 되찾아
	 *   perf 에 샘플을 보고한다.
	 * 값 범위: 배정되지 않은 자리는 NULL.
	 * 동기화: used_mask 와 함께 갱신된다. */
	unsigned char		irq_name[16];
	/* [한국어] 이 PMU 의 인터럽트 이름. /proc/interrupts 에 나타난다.
	 * 설정자: 초기화 때 유닛 이름을 넣어 만든다("dmar%d-perf" 형식).
	 * 읽는 자: request_irq() 와 유저스페이스의 진단.
	 * 왜 구조체 안에 두는가: request_irq 는 이름 문자열의 수명을 인터럽트가
	 *   살아 있는 동안 요구한다. 스택이나 임시 버퍼를 넘길 수 없어 여기 담아 둔다.
	 * 동기화: 초기화 시 한 번 쓴다. */
};

#define IOMMU_IRQ_ID_OFFSET_PRQ		(DMAR_UNITS_SUPPORTED)	/* [한국어] 페이지 요청 인터럽트의 id 공간을 폴트 인터럽트와 겹치지 않게 미는 오프셋. 유닛 하나가 폴트·PRQ·성능 세 가지 인터럽트를 가질 수 있어 구간을 나눠 쓴다 */
#define IOMMU_IRQ_ID_OFFSET_PERF	(2 * DMAR_UNITS_SUPPORTED)	/* [한국어] 성능 인터럽트의 구간. 세 번째 블록이다 */

/*
 * [한국어] struct intel_iommu — DRHD 유닛 하나, 즉 물리적인 VT-d 하드웨어 하나
 *
 * ACPI DMAR 표의 DRHD(DMA Remapping Hardware unit Definition) 항목 하나에
 * 대응한다. 대형 시스템은 이런 유닛이 여러 개이고, 각 유닛이 자기 아래의 PCI
 * 세그먼트/버스에서 나오는 DMA 를 담당한다. 그래서 "IOMMU 가 켜져 있다"는
 * 말은 실제로는 "이 유닛들이 각자 자기 표를 세우고 번역을 켰다"는 뜻이다.
 *
 * 담고 있는 것을 성격별로 나누면 다섯 덩어리다.
 *   [하드웨어 창]  reg/reg_phys/reg_size 로 MMIO 를 매핑하고, cap/ecap/vccap/
 *                  ecmdcap 으로 이 유닛이 무엇을 할 수 있는지 안다. gcmd 는
 *                  전역 명령 레지스터의 "우리가 켠 비트"를 기억해 둔 사본이다.
 *   [번역 표]      root_entry 가 루트 테이블을, copied_tables 가 이전 커널에서
 *                  물려받은 버스들을 표시한다.
 *   [무효화]       qi 가 큐를, flush 가 레지스터 방식 폴백을 담는다.
 *   [폴트/페이지요청] prq 와 iopf_queue, 그리고 소스 id 로 장치를 되찾는
 *                  device_rbtree.
 *   [코어 연결]    iommu(iommu_device)로 코어에 등록하고, domain_ida 로
 *                  도메인 id 를 나눠 준다.
 *
 * gcmd 를 따로 두는 이유가 이 하드웨어의 특징을 잘 보여 준다. GCMD 레지스터는
 * 읽어도 현재 설정이 나오지 않는 write-only 성격이라, 비트 하나를 바꾸려면
 * 나머지 비트를 우리가 기억하고 있어야 한다. 그래서 켠 상태를 소프트웨어
 * 사본으로 들고 다닌다 (원 주석: Holds TE, EAFL — SRTP/SFL/WBF 는 한 번짜리
 * 명령이라 기억할 필요가 없다).
 *
 * 락이 넷인 것도 각각 지키는 대상이 다르기 때문이다.
 *   register_lock  — MMIO 접근 직렬화. raw 스핀락(인터럽트 문맥에서도 쓴다).
 *   did_lock       — 도메인 id 할당기. 잠들 수 있어 뮤텍스.
 *   lock           — 컨텍스트 테이블과 도메인 id 사용 상태.
 *   device_rbtree_lock — 소스 id 색인. 폴트 인터럽트에서 잡히므로 irqsave.
 *   iopf_lock      — 폴트 보고와 장치 해제 사이의 경쟁을 막는다.
 *
 * 실행 컨텍스트: 이 구조체는 부팅 중 dmar.c 가 만들고 시스템 수명 내내
 * 유지된다(유닛 핫플러그 시에만 생성·해제). 필드에 따라 부팅 초기화,
 * 프로세스 컨텍스트의 매핑, 폴트 인터럽트에서 모두 접근된다.
 */
struct intel_iommu {
	void __iomem	*reg; /* Pointer to hardware regs, virtual addr */
	/* [한국어] 이 유닛의 MMIO 레지스터가 매핑된 가상 주소 (원 주석: Pointer to hardware regs).
	 * 이 파일의 모든 DMAR_*_REG 오프셋이 이 포인터에 더해져 실제 접근이 된다.
	 * 설정자: map_iommu() 가 부팅 중 ioremap 한다.
	 * 읽는 자: readl/readq/writel/writeq 를 쓰는 모든 경로 — 초기화, 무효화,
	 *   폴트 처리, 성능 카운터.
	 * 값 범위: NULL 이 아니어야 하고, __iomem 이므로 일반 포인터처럼 역참조하면 안 된다.
	 * 동기화: 접근 자체는 register_lock 이 직렬화한다. */
	u64 		reg_phys; /* physical address of hw register set */
	/* [한국어] 레지스터 영역의 물리 주소 (원 주석: physical address of hw register set).
	 * 설정자: DMAR 표의 DRHD 항목에서 읽는다.
	 * 읽는 자: ioremap 할 때, 그리고 sysfs 의 address 속성. DMAR 표의 몇 번째
	 *   항목인지 사람이 대조하는 근거가 된다.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u64		reg_size; /* size of hw register set */
	/* [한국어] 레지스터 영역의 크기 (원 주석: size of hw register set).
	 * 설정자: 능력 레지스터를 읽어 필요한 범위를 계산한다 — 폴트 기록과 IOTLB
	 *   레지스터가 어디까지 뻗는지가 cap/ecap 에 들어 있어서, 처음에 작게 매핑해
	 *   그 값을 읽은 뒤 필요하면 다시 매핑한다.
	 * 읽는 자: ioremap/iounmap.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u64		cap;
	/* [한국어] 능력 레지스터(DMAR_CAP_REG)의 사본.
	 * 설정자: 초기화 때 한 번 읽어 둔다.
	 * 읽는 자: 이 파일의 cap_ 계열 매크로 전부. 도메인 개수, 주소 폭, 큰 페이지
	 *   지원, caching mode 판정이 모두 이 값에서 나온다.
	 * 왜 캐시하는가: 능력은 바뀌지 않는 값인데 MMIO 읽기는 비싸다. 매핑 경로에서
	 *   cap_ndoms 같은 것을 물을 때마다 하드웨어를 읽을 수는 없다.
	 * 동기화: 초기화 시 한 번 쓰고 이후 읽기만 한다. */
	u64		ecap;
	/* [한국어] 확장 능력 레지스터(DMAR_ECAP_REG)의 사본.
	 * PASID, PRI, scalable mode, 코히런시 지원이 여기서 나온다.
	 * 설정자/읽는 자/동기화: cap 과 같다. ecap_ 계열 매크로가 이 값을 읽는다. */
	u64		vccap;
	/* [한국어] 가상 명령 인터페이스 능력 레지스터의 사본.
	 * 게스트 커널이 호스트에 PASID 할당을 요청하는 경로를 지원하는지 알려 준다.
	 * 설정자: scalable 모드 유닛에서 초기화 때 읽는다.
	 * 읽는 자: vccap_pasid() 매크로.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u64		ecmdcap[DMA_MAX_NUM_ECMDCAP];
	/* [한국어] 확장 명령 능력 비트맵. 256개 명령의 지원 여부를 64비트 워드 4개에 담는다.
	 * 설정자: cap_ecmds 인 유닛에서 초기화 때 DMAR_ECCAP_REG 를 순회하며 읽는다.
	 * 읽는 자: 성능 카운터 초기화가 DMA_ECMD_ECCAP3_ESSENTIAL 조합을 확인한다.
	 * 동기화: 초기화 시 한 번 쓴다. */
	u32		gcmd; /* Holds TE, EAFL. Don't need SRTP, SFL, WBF */
	/* [한국어] 전역 명령 레지스터에서 우리가 켠 비트들의 소프트웨어 사본
	 * (원 주석: Holds TE, EAFL. Don't need SRTP, SFL, WBF).
	 * 왜 사본이 필요한가: GCMD 레지스터는 읽어도 현재 설정을 돌려주지 않는다.
	 *   그래서 비트 하나를 바꾸려면 나머지를 우리가 기억하고 있다가 통째로
	 *   다시 써야 한다. TE(번역 켜기)와 EAFL 처럼 켠 채로 유지되는 비트만
	 *   기억하면 되고, SRTP/SFL/WBF 는 한 번 수행되고 스스로 내려가는 명령이라
	 *   기억할 필요가 없다.
	 * 설정자: iommu_enable_translation()/disable_translation() 등이 갱신한다.
	 * 읽는 자: 같은 함수들, 그리고 intel_iommu_add() 가 "펌웨어가 켜 둔 채
	 *   넘겨줬는가"를 판단할 때.
	 * 동기화: register_lock. */
	raw_spinlock_t	register_lock; /* protect register handling */
	/* [한국어] MMIO 레지스터 접근을 직렬화하는 락 (원 주석: protect register handling).
	 * 설정자/읽는 자: 레지스터를 읽고 쓰는 거의 모든 경로.
	 * raw 인 이유: PREEMPT_RT 에서도 진짜 스핀락으로 남아야 한다. 레지스터 접근은
	 *   대개 "쓰고 상태 비트가 바뀔 때까지 폴링"이라 선점되면 안 되고,
	 *   폴트 인터럽트 핸들러에서도 잡힌다.
	 * 동기화 범위: 이 유닛 하나. 유닛이 여럿이면 서로 독립적으로 진행한다. */
	int		seq_id;	/* sequence id of the iommu */
	/* [한국어] 이 유닛의 순번 (원 주석: sequence id of the iommu).
	 * 설정자: 유닛을 등록할 때 전역 비트맵에서 하나 받는다.
	 * 읽는 자: 도메인의 iommu_array 를 색인하는 키, 인터럽트 id 계산,
	 *   "dmar0" 같은 이름 생성.
	 * 값 범위: 0 ~ DMAR_UNITS_SUPPORTED-1.
	 * 동기화: 등록 시 한 번 쓴다. */
	int		agaw; /* agaw of this iommu */
	/* [한국어] 이 유닛이 실제로 쓰기로 한 주소 폭 (원 주석: agaw of this iommu).
	 * Adjusted Guest Address Width — cap 의 sagaw 와 mgaw 를 함께 보고 고른 값이다.
	 * 설정자: __iommu_calculate_agaw() 가 초기화 때 정한다.
	 * 읽는 자: 도메인의 페이지 테이블 단계 수를 정할 때.
	 * 동기화: 초기화 시 한 번 쓴다. */
	int		msagaw; /* max sagaw of this iommu */
	/* [한국어] 이 유닛이 지원하는 최대 주소 폭 (원 주석: max sagaw of this iommu).
	 * agaw 와 달리 "쓸 수 있는 최대"다.
	 * 설정자: __iommu_calculate_sagaw() 계열.
	 * 읽는 자: 통과 모드의 컨텍스트 항목에 AW 를 채울 때. 통과 모드에서는
	 *   지원하는 최대값을 프로그램해야 한다는 스펙 요구가 있다.
	 * 동기화: 초기화 시 한 번 쓴다. */
	unsigned int	irq, pr_irq, perf_irq;
	/* [한국어] 이 유닛이 쓰는 세 인터럽트 번호 — 폴트, 페이지 요청, 성능 카운터.
	 * 설정자: dmar_set_interrupt(), intel_iommu_enable_prq(), iommu_pmu_register().
	 * 읽는 자: 해제 경로가 free_irq 할 때.
	 * 값 범위: 0 이면 아직 할당되지 않았다는 뜻이다. 세 인터럽트는 서로 다른
	 *   id 구간을 쓰며(IOMMU_IRQ_ID_OFFSET_PRQ/PERF), 그래서 유닛이 여럿이어도
	 *   겹치지 않는다.
	 * 동기화: 설정·해제 시에만 쓴다. */
	u16		segment;     /* PCI segment# */
	/* [한국어] 이 유닛이 담당하는 PCI 세그먼트(도메인) 번호 (원 주석: PCI segment#).
	 * 대형 시스템은 PCI 버스 번호 공간이 여럿이라, 소스 id 만으로는 장치를
	 *   특정할 수 없고 세그먼트까지 맞아야 한다.
	 * 설정자: DMAR 표의 DRHD 항목에서 읽는다.
	 * 읽는 자: device_lookup_iommu() 가 장치의 세그먼트와 비교할 때,
	 *   그리고 ATSR/SATC 항목을 이 유닛의 것으로 걸러 낼 때.
	 * 동기화: 초기화 시 한 번 쓴다. */
	unsigned char	name[16];    /* Device Name */
	/* [한국어] 이 유닛의 이름 (원 주석: Device Name). "dmar0" 형식이다.
	 * 설정자: 등록 시 seq_id 로 만든다.
	 * 읽는 자: 로그 메시지, sysfs 노드 이름(/sys/class/iommu/dmar0), 인터럽트 이름.
	 * 왜 구조체 안에 두는가: sysfs 와 request_irq 가 문자열의 수명을 요구한다.
	 * 동기화: 등록 시 한 번 쓴다. */

#ifdef CONFIG_INTEL_IOMMU	/* [한국어] DMA 재매핑을 뺀 빌드(인터럽트 재매핑만 쓰는 경우)에서는 아래 필드들이 필요 없다 */
	/* mutex to protect domain_ida */
	struct mutex	did_lock;
	/* [한국어] 도메인 id 할당기(domain_ida)를 지키는 락 (원 주석: mutex to protect domain_ida).
	 * 뮤텍스인 이유: ida 할당은 내부에서 메모리를 잡을 수 있어 잠들 수 있다.
	 *   도메인 부착은 프로세스 컨텍스트에서만 일어나므로 뮤텍스로 충분하다.
	 * 설정자/읽는 자: domain_attach_iommu()/domain_detach_iommu().
	 * 아래 lock(스핀락)과 다른 것을 지킨다 — 이쪽은 id 를 나눠 주는 할당기,
	 *   저쪽은 컨텍스트 테이블과 id 사용 상태다. */
	struct ida	domain_ida; /* domain id allocator */
	/* [한국어] 이 유닛의 도메인 id 할당기 (원 주석: domain id allocator).
	 * 설정자: domain_attach_iommu() 가 새 id 를 받고, 마지막 장치가 떠나면 반납한다.
	 * 읽는 자: sysfs 의 domains_used 가 몇 개가 쓰이는지 세고,
	 *   copy_translation_tables() 가 물려받은 표의 id 를 예약할 때도 쓴다.
	 * 값 범위: 0 ~ cap_ndoms(cap)-1. 이 수를 다 쓰면 그 유닛에는 새 도메인을
	 *   붙일 수 없다 — 컨테이너나 VFIO 를 많이 쓰는 시스템에서 실제로 겪는 한계다.
	 * 동기화: 위 did_lock. */
	unsigned long	*copied_tables; /* bitmap of copied tables */
	/* [한국어] 이전 커널에서 그대로 물려받은 컨텍스트 테이블이 있는 버스들의 비트맵
	 * (원 주석: bitmap of copied tables).
	 * 왜 필요한가: kdump 커널은 이전 커널의 번역 표를 이어받아, 진행 중이던 DMA 가
	 *   끊기지 않게 한다. 그런데 그 표는 우리가 만든 것이 아니므로, 나중에 그
	 *   장치를 실제로 쓰게 될 때 우리 형식으로 다시 세워야 한다. 어느 버스가
	 *   그런 상태인지를 이 비트맵이 기억한다.
	 * 설정자: copy_translation_tables() 가 버스마다 비트를 세운다.
	 * 읽는 자: context_copied() 매크로. 프로브·부착·해제 경로가 "이 항목은
	 *   우리 것인가 물려받은 것인가"를 이 값으로 구분한다.
	 * 값 범위: NULL 이면 인계받은 것이 없다는 뜻이다(보통의 부팅).
	 * 동기화: 아래 lock. */
	spinlock_t	lock; /* protect context, domain ids */
	/* [한국어] 컨텍스트 테이블과 도메인 id 사용 상태를 지키는 락
	 * (원 주석: protect context, domain ids).
	 * 설정자/읽는 자: 컨텍스트 항목을 세우고 지우는 모든 경로
	 *   (domain_context_mapping_one, context_setup_pass_through, free_context_table).
	 * did_lock 과 나눈 이유: 이쪽은 인터럽트를 끈 짧은 구간이고 잠들 수 없다.
	 *   ida 할당처럼 잠들 수 있는 작업과 같은 락으로 묶을 수 없다. */
	struct root_entry *root_entry; /* virtual address */
	/* [한국어] 이 유닛의 루트 테이블 (원 주석: virtual address).
	 * 번역 사슬의 시작점 — 하드웨어는 RTADDR 레지스터에 적힌 물리 주소로 이 표를
	 * 읽고, 소스 id 의 버스 번호로 색인해 컨텍스트 테이블을 찾는다.
	 * 설정자: iommu_alloc_root_entry() 가 한 페이지를 잡아 채운다.
	 * 읽는 자: 컨텍스트 항목을 찾는 iommu_context_addr(), 폴트 덤프, 해제 경로.
	 * 값 범위: 256개 항목 × 16바이트 = 정확히 4KB 한 페이지.
	 * 동기화: 위 lock. 하드웨어에 알리는 것은 iommu_set_root_entry() 의 몫이다. */

	struct iommu_flush flush;
	/* [한국어] 레지스터 방식 무효화 함수 포인터 두 개(컨텍스트, IOTLB).
	 * 왜 함수 포인터인가: 무효화 큐(QI)를 지원하는 유닛과 그렇지 않은 유닛이
	 *   섞여 있고, 초기화 도중에는 큐가 아직 없어 레지스터 방식을 써야 한다.
	 *   호출부가 매번 분기하지 않도록 초기화 때 적절한 구현을 꽂아 둔다.
	 * 설정자: init_dmars()/intel_iommu_add() 가 __iommu_flush_context 와
	 *   __iommu_flush_iotlb 를 꽂는다.
	 * 읽는 자: 초기화 경로와, 큐를 쓸 수 없는 상황의 폴백.
	 * 동기화: 초기화 시 한 번 쓴다. */
#endif
	struct page_req_dsc *prq;
	/* [한국어] 페이지 요청 큐 버퍼. 하드웨어가 채우고 커널이 소비한다 — 무효화 큐와
	 * 방향이 반대다.
	 * 설정자: intel_iommu_enable_prq() 가 PRQ_SIZE 만큼 잡고 PQA 레지스터에 알린다.
	 * 읽는 자: prq_event_thread() 가 head~tail 구간을 읽어 처리한다.
	 * 값 범위: NULL 이면 PRI 를 쓰지 않는 유닛이다.
	 * 동기화: head/tail 레지스터로 하드웨어와 협조한다. */
	unsigned char prq_name[16];    /* Name for PRQ interrupt */
	/* [한국어] 페이지 요청 인터럽트의 이름 (원 주석: Name for PRQ interrupt).
	 * name 과 마찬가지로, request_irq 가 문자열의 수명을 요구해 구조체에 담는다.
	 * 설정자: PRQ 활성화 시 만든다.
	 * 읽는 자: request_irq 와 /proc/interrupts. */
	unsigned long prq_seq_number;
	/* [한국어] 지금까지 처리한 페이지 요청의 누적 번호.
	 * 무엇에 쓰는가: PASID 를 내릴 때 "그 시점 이후의 요청은 없다"를 확인해야
	 *   하는데, 이 번호를 기준점으로 삼아 드레인이 끝났는지 판단한다.
	 * 설정자: prq_event_thread() 가 요청을 처리할 때마다 늘린다.
	 * 읽는 자: 드레인 완료를 기다리는 경로.
	 * 동기화: 폴트 스레드 하나만 갱신하므로 별도 락이 없다. */
	struct completion prq_complete;
	/* [한국어] 페이지 요청 드레인이 끝났음을 기다리는 쪽에 알리는 completion.
	 * 왜 필요한가: PASID 를 내리려면 그 PASID 로 온 요청이 모두 처리되었음을
	 *   보장해야 한다. 남은 요청에 응답하지 않으면 장치가 영원히 멈추기 때문이다.
	 * 설정자: 드레인이 끝나면 폴트 스레드가 complete() 한다.
	 * 읽는 자: intel_pasid_tear_down_entry() 계열이 wait_for_completion() 한다. */
	struct iopf_queue *iopf_queue;
	/* [한국어] 코어의 I/O 페이지 폴트 처리 큐. 하드웨어에서 받은 요청을 여기 넣으면
	 * 코어가 워커 스레드에서 처리하고 응답을 돌려준다.
	 * 설정자: PRQ 활성화 시 iopf_queue_alloc() 으로 만든다.
	 * 읽는 자: intel_iommu_enable_iopf() 가 장치를 등록하고, 폴트 스레드가
	 *   요청을 넣는다.
	 * 동기화: 코어가 큐 내부를 지킨다. 장치 등록·해제는 그룹 뮤텍스가 지킨다. */
	unsigned char iopfq_name[16];
	/* [한국어] 그 폴트 큐의 이름. 워커 스레드 이름과 진단에 쓰인다.
	 * 설정자/읽는 자: prq_name 과 같은 이유로 구조체에 담아 둔다. */
	/* Synchronization between fault report and iommu device release. */
	struct mutex iopf_lock;
	/* [한국어] 폴트 보고와 장치 해제 사이의 경쟁을 막는 락
	 * (원 주석: Synchronization between fault report and iommu device release).
	 * 왜 필요한가: 폴트 처리는 소스 id 로 struct device 를 되찾아 진행하는데,
	 *   그 사이에 장치가 해제되면 이미 사라진 device_domain_info 를 읽게 된다.
	 *   intel_iommu_release_device() 가 device_rbtree_remove() 를 이 락 안에서
	 *   하는 이유가 그것이다 — 이 뒤로는 폴트 처리기가 그 장치를 찾지 못한다는
	 *   순간을 확정한다.
	 * 뮤텍스인 이유: 폴트 처리 경로가 잠들 수 있는 문맥(워커 스레드)이다. */
	struct q_inval  *qi;            /* Queued invalidation info */
	/* [한국어] 무효화 큐 (원 주석: Queued invalidation info).
	 * 설정자: dmar_enable_qi() 가 만들고 IQA 레지스터에 알린다.
	 * 읽는 자: qi_submit_sync() 와 그것을 부르는 모든 무효화 경로.
	 * 값 범위: NULL 이면 큐를 쓰지 않는 유닛이고, 그 경우 위 flush 의
	 *   레지스터 방식으로 무효화한다.
	 * 동기화: 구조체 안의 q_lock. */
	u32 iommu_state[MAX_SR_DMAR_REGS]; /* Store iommu states between suspend and resume.*/
	/* [한국어] 서스펜드 때 저장해 두는 레지스터 값들
	 * (원 주석: Store iommu states between suspend and resume).
	 * 무엇을 저장하는가: 폴트 인터럽트 관련 네 레지스터(FECTL/FEDATA/FEADDR/
	 *   FEUADDR). 나머지 상태(루트 테이블 주소, 큐 주소 등)는 리줌 때 우리가
	 *   들고 있는 자료구조에서 다시 세울 수 있지만, MSI 설정은 커널이 그 시점에
	 *   할당한 값이라 저장해 두어야 한다.
	 * 설정자: iommu_suspend(). 읽는 자: iommu_resume().
	 * 동기화: syscore 콜백이라 CPU 하나만 살아 있는 시점에 실행된다. */

	/* rb tree for all probed devices */
	struct rb_root device_rbtree;
	/* [한국어] 이 유닛 아래의 장치들을 소스 id 로 색인한 트리 (원 주석: rb tree for all probed devices).
	 * 왜 필요한가: 하드웨어가 폴트나 페이지 요청을 보고할 때 알려 주는 것은
	 *   16비트 소스 id 뿐이다. 그것을 struct device 로 되돌리는 역방향 조회가
	 *   이 트리다.
	 * 설정자: device_rbtree_insert()/remove().
	 * 읽는 자: device_rbtree_find() — 폴트 인터럽트 문맥에서 불린다.
	 * 동기화: 아래 device_rbtree_lock. */
	/* protect the device_rbtree */
	spinlock_t device_rbtree_lock;
	/* [한국어] 위 트리를 지키는 락 (원 주석: protect the device_rbtree).
	 * irqsave 로 잡아야 하는 이유: 조회가 폴트 인터럽트 핸들러에서 일어난다.
	 *   인터럽트 밖(프로브/해제)에서 인터럽트를 막지 않고 이 락을 잡으면,
	 *   그 사이 들어온 폴트가 같은 락을 기다리며 자기 자신과 데드락이 난다. */

#ifdef CONFIG_IRQ_REMAP
	struct ir_table *ir_table;	/* Interrupt remapping info */
	/* [한국어] 인터럽트 재매핑 테이블 (원 주석: Interrupt remapping info).
	 * DMA 번역과 별개의 기능이다 — 장치가 보내는 인터럽트 메시지를 가로채
	 * 실제 벡터와 목적지 CPU 로 바꾼다. 이것이 없으면 장치가 임의의 인터럽트를
	 * 임의의 CPU 에 쏠 수 있다.
	 * 설정자: intel_setup_irq_remapping().
	 * 읽는 자: irq_remapping.c 의 항목 할당·수정 경로.
	 * 동기화: irq_2_ir_lock. */
	struct irq_domain *ir_domain;
	/* [한국어] 이 유닛이 관리하는 인터럽트들의 커널 인터럽트 도메인.
	 * 커널의 인터럽트 할당 요청이 이 도메인을 거쳐 재매핑 항목 할당으로 이어진다.
	 * 설정자: intel_setup_irq_remapping().
	 * 읽는 자: 커널 인터럽트 코어. */
#endif
	struct iommu_device iommu;  /* IOMMU core code handle */
	/* [한국어] IOMMU 코어에 등록하는 핸들 (원 주석: IOMMU core code handle).
	 * 이 필드가 코어와 이 드라이버를 잇는 지점이다 — 코어는 이 포인터만 알고,
	 * dev_to_intel_iommu() 처럼 container_of 로 유닛 전체를 되찾는다.
	 * 설정자: iommu_device_sysfs_add() 와 iommu_device_register() 가 채운다.
	 * 읽는 자: 코어의 모든 경로, 그리고 sysfs 속성 함수들.
	 * 값 범위: ops 가 채워지기 전에는 이 유닛이 아직 장치를 받을 수 없다 —
	 *   device_lookup_iommu() 가 !iommu->iommu.ops 를 확인하는 이유다. */
	int		node;
	/* [한국어] 이 유닛이 속한 NUMA 노드.
	 * 설정자: DMAR 표와 ACPI 근접성 정보에서 구한다.
	 * 읽는 자: 루트 테이블·컨텍스트 테이블·페이지 테이블을 할당할 때. 하드웨어가
	 *   매 번역마다 읽는 메모리이므로 같은 노드에 두면 지연이 줄어든다.
	 * 값 범위: NUMA_NO_NODE 일 수 있다(정보가 없는 시스템). */
	u32		flags;      /* Software defined flags */
	/* [한국어] 소프트웨어가 정의한 상태 플래그 (원 주석: Software defined flags).
	 * VTD_FLAG_TRANS_PRE_ENABLED(번역을 인계받았다),
	 * VTD_FLAG_IRQ_REMAP_PRE_ENABLED(재매핑을 인계받았다),
	 * VTD_FLAG_SVM_CAPABLE(SVA 를 쓸 수 있다)의 조합이다.
	 * 설정자: init_translation_status(), intel_svm_check() 등.
	 * 읽는 자: translation_pre_enabled() 같은 판별자.
	 * 동기화: 초기화 경로에서만 바뀌며 그때는 단일 스레드다. */

	struct dmar_drhd_unit *drhd;
	/* [한국어] 이 유닛을 만든 DMAR 표의 DRHD 항목.
	 * 설정자: 유닛 생성 시 서로를 가리키게 연결한다.
	 * 읽는 자: 이 유닛이 담당하는 장치 목록(device scope)을 훑거나,
	 *   ignored 표시를 확인할 때.
	 * 동기화: DMAR 표는 부팅 내내 유지되고, 목록 순회는 RCU 로 보호한다. */
	void *perf_statistic;
	/* [한국어] 진단용 지연 통계 버퍼(perf.c 가 관리한다).
	 * 무엇을 재는가: 무효화 같은 동작이 얼마나 걸렸는지를 구간별 히스토그램으로
	 *   쌓는다. 위의 iommu_pmu 가 하드웨어 카운터라면 이쪽은 소프트웨어 계측이다.
	 * 설정자: dmar_latency_enable() 이 잡고 disable 이 놓는다.
	 * 읽는 자: debugfs 의 지연 통계 노드.
	 * 값 범위: NULL 이면 계측이 꺼져 있다. */

	struct iommu_pmu *pmu;
	/* [한국어] 이 유닛의 하드웨어 성능 카운터(위 struct iommu_pmu).
	 * 설정자: alloc_iommu_pmu()/iommu_pmu_register().
	 * 읽는 자: perf 서브시스템의 콜백들.
	 * 값 범위: NULL 이면 이 유닛에 성능 카운터가 없거나 필요한 확장 명령을
	 *   지원하지 않는다는 뜻이다. */
};

/* PCI domain-device relationship */
/*
 * [한국어] struct device_domain_info — VT-d 가 보는 "장치 하나"
 *
 * IOMMU 코어의 struct device 에 매달리는(dev_iommu_priv_set) 이 드라이버의
 * 장치 표현이다. 코어는 장치를 이름과 그룹으로 알지만, VT-d 는 소스 id 와
 * 능력 비트로 안다 — 그 번역을 이 구조체가 담당한다.
 *
 * 세 가지를 동시에 기억한다.
 *   [정체]  segment/bus/devfn — 하드웨어가 이 장치를 부르는 이름. 별칭 때문에
 *           장치 자신의 PCI 위치와 다를 수 있다.
 *   [능력]  ats/pasid/pri 의 supported 와 enabled 가 쌍으로 있다. supported 는
 *           프로브 때 조사한 "쓸 수 있는가"이고, enabled 는 finalize 때 실제로
 *           켰는가다. 둘을 나누는 이유는 켜는 순서에 스펙 요구가 있어서
 *           (PASID 를 ATS 보다 먼저) 조사와 활성화를 분리해야 하기 때문이다.
 *   [소속]  iommu(담당 유닛)와 domain(현재 붙은 도메인), 그리고 두 자료구조에
 *           동시에 매달리기 위한 고리 두 개 — link(도메인의 devices 목록)와
 *           node(유닛의 device_rbtree).
 *
 * 왜 목록과 트리에 동시에 매다는가: 도메인은 "이 주소 공간을 쓰는 장치들"을
 * 순회해야 하고(설정 전파, 무효화 대상 수집), 유닛은 "이 소스 id 가 누구인가"를
 * 빠르게 답해야 한다(폴트 처리). 두 질문의 접근 방향이 정반대라 자료구조도
 * 두 개다.
 *
 * 수명: intel_iommu_probe_device() 가 만들고 intel_iommu_release_device() 가
 * kfree 한다. 그 사이 이 포인터는 dev_iommu_priv_get(dev) 으로 언제든 얻을 수
 * 있으며, 해제 순서를 어기면 폴트 처리기가 사라진 구조체를 읽게 된다 —
 * release 경로가 iopf_lock 아래에서 트리 제거를 하는 이유다.
 */
struct device_domain_info {
	struct list_head link;	/* link to domain siblings */
	/* [한국어] 도메인의 devices 목록에 매달리는 고리 (원 주석: link to domain siblings).
	 * 설정자: dmar_domain_attach_device() 가 넣고 device_block_translation() 이 뺀다.
	 * 읽는 자: 도메인이 자기 장치들을 훑는 모든 경로.
	 * 동기화: 도메인의 lock(irqsave). 이 구조체의 것이 아니라 도메인의 락이라는
	 *   점이 중요하다 — 목록의 주인이 도메인이기 때문이다. */
	u32 segment;		/* PCI segment number */
	/* [한국어] PCI 세그먼트(도메인) 번호 (원 주석: PCI segment number).
	 * 대형 시스템은 버스 번호 공간이 여럿이라 세그먼트까지 맞아야 장치가 특정된다.
	 * 설정자: intel_iommu_probe_device(). 보통 유닛의 segment 를 쓰지만,
	 *   실제 DMA 서브디바이스는 자기 PCI 세그먼트를 쓴다.
	 * 읽는 자: 진단 출력과 장치 조회. */
	u8 bus;			/* PCI bus number */
	/* [한국어] 이 장치의 DMA 가 하드웨어에 보일 때의 버스 번호 (원 주석: PCI bus number).
	 * 설정자: device_lookup_iommu() 가 별칭까지 따져 정한 값을 프로브가 저장한다.
	 * 읽는 자: 컨텍스트 항목을 찾는 iommu_context_addr(), 소스 id 조립
	 *   (PCI_DEVID(bus, devfn)), 물려받은 컨텍스트인지 확인하는 context_copied().
	 * 값 범위: 장치 자신의 pdev->bus->number 와 다를 수 있다 — 브리지 뒤의 장치는
	 *   브리지의 소스 id 로 DMA 를 내기 때문이다. */
	u8 devfn;		/* PCI devfn number */
	/* [한국어] 같은 이유로 정해진 devfn (원 주석: PCI devfn number).
	 * 설정자/읽는 자/값 범위: bus 와 같다. 비트 7 이 컨텍스트 테이블의 하위/상위
	 *   중 어느 쪽을 쓸지도 정한다. */
	u16 pfsid;		/* SRIOV physical function source ID */
	/* [한국어] SR-IOV 가상 함수의 경우, 그 물리 함수(PF)의 소스 id
	 * (원 주석: SRIOV physical function source ID).
	 * 왜 필요한가: 유닛이 DIT(Device-TLB Invalidation Throttling)를 지원하면,
	 *   VF 로 보내는 무효화 서술자에 PF 의 소스 id 를 실어 준다. 그래야 하드웨어가
	 *   PF 단위로 큐 깊이를 가늠해 무효화를 조절할 수 있다.
	 * 설정자: intel_iommu_probe_device() 가 ecap_dit 인 경우에만 채운다.
	 * 읽는 자: qi_flush_dev_iotlb() 계열.
	 * 값 범위: DIT 를 지원하지 않으면 예약 필드라 0 이어야 한다. */
	u8 pasid_supported:3;
	/* [한국어] 이 장치의 PASID 능력. 비트 0 은 "능력이 있다"는 우리 표시이고, 상위 비트는
	 * PCIe 가 알려 준 features(실행 권한, 특권 모드 지원)다.
	 * 왜 비트 0 을 겸용하는가: pci_pasid_features() 가 0 을 돌려줄 수 있어서,
	 *   그 값만으로는 "능력 없음"과 구분되지 않는다. 그래서 |1 로 존재를 표시한다.
	 * 설정자: intel_iommu_probe_device(). 읽는 자: probe_finalize 가
	 *   pci_enable_pasid() 에 넘길 때 이 비트를 다시 뺀다(& ~1).
	 * 값 범위: 0 이면 PASID 를 쓸 수 없는 장치다. */
	u8 pasid_enabled:1;
	/* [한국어] 실제로 PASID 를 켰는가.
	 * 설정자: intel_iommu_probe_finalize() 가 켜는 데 성공하면 1.
	 * 읽는 자: release 경로가 끌지 말지 판단할 때.
	 * supported 와 나뉘는 이유: PCIe 스펙이 "ATS 를 켠 뒤 PASID 를 켜면 동작이
	 *   정의되지 않는다"고 못 박아, 쓸지 모르더라도 능력이 있으면 먼저 켜 둔다.
	 *   즉 조사와 활성화가 다른 시점에 일어난다. */
	u8 pri_supported:1;
	/* [한국어] 페이지 요청 인터페이스를 쓸 수 있는가.
	 * 설정자: 프로브 때 유닛(ecap_prs/ecap_pds)과 장치(pci_pri_supported) 양쪽,
	 *   그리고 ATS 가 가능한지까지 확인해 정한다. ATS 가 전제인 이유는 PRI 응답이
	 *   ATS 번역 경로로 돌아오기 때문이다.
	 * 읽는 자: intel_iommu_enable_iopf() 가 이 값이 아니라 pri_enabled 를 본다. */
	u8 pri_enabled:1;
	/* [한국어] 실제로 PRI 를 켰는가.
	 * 설정자: probe_finalize 의 iommu_enable_pci_pri(). 켜는 순서상 마지막이다.
	 * 읽는 자: intel_iommu_enable_iopf()/disable_iopf(). 이것이 0 이면 폴트를
	 *   받을 통로가 없으므로 -ENODEV 다.
	 * 동기화: 그룹 뮤텍스가 이 값과 iopf_refcount 를 함께 지킨다. */
	u8 ats_supported:1;
	/* [한국어] ATS(장치 내부 번역 캐시)를 쓸 수 있는가.
	 * 설정자: 프로브 때 유닛의 ecap_dev_iotlb_support, 장치의 ATS 능력 구조,
	 *   그리고 dmar_ats_supported()(경로와 펌웨어 신고) 셋을 모두 확인한다.
	 * 읽는 자: intel_iommu_capable(IOMMU_CAP_PCI_ATS_SUPPORTED), probe_finalize. */
	u8 ats_enabled:1;
	/* [한국어] 실제로 ATS 를 켰는가.
	 * 설정자: iommu_enable_pci_ats()/disable_pci_ats().
	 * 읽는 자: 무효화 경로 — 켜져 있으면 IOTLB 뿐 아니라 이 장치의 디바이스 TLB 도
	 *   비워야 한다. probe_finalize 는 이 값이 1 일 때만 DEVTLB 캐시 태그를 단다.
	 * 주의: 태그 등록에 실패하면 ATS 를 도로 끈다 — 무효화할 수 없는 캐시를
	 *   켜 두는 것이 훨씬 위험하기 때문이다. */
	u8 dtlb_extra_inval:1;	/* Quirk for devices need extra flush */
	/* [한국어] 디바이스 TLB 무효화를 한 번 더 보내야 하는 결함 장치인가
	 * (원 주석: Quirk for devices need extra flush).
	 * 어떤 결함인가: ATS 무효화 완료 응답을, 그 범위의 번역을 이미 써서 발행한
	 *   posted write 보다 먼저 보내는 장치가 있다. 완료를 믿고 페이지를 해제하면
	 *   뒤늦게 도착한 쓰기가 남의 메모리를 덮어쓴다.
	 * 설정자: 프로브 때 dev_needs_extra_dtlb_flush() 가 판단한다.
	 * 읽는 자: quirk_extra_dev_tlb_flush() — 이 값이 0 이면 곧바로 돌아간다. */
	u8 domain_attached:1;	/* Device has domain attached */
	/* [한국어] 이 장치에 도메인이 붙어 하드웨어 번역이 세워져 있는가
	 * (원 주석: Device has domain attached).
	 * 설정자: 부착 성공 시 1, device_block_translation() 이 0.
	 * 읽는 자: device_block_translation() 자신 — 이 값이 0 이면 이미 차단
	 *   상태이므로 아무것도 하지 않는다. 그래서 두 번 불려도 안전하다.
	 * 왜 domain 포인터로 대신할 수 없는가: 항등 도메인처럼 domain 이 NULL 이면서도
	 *   하드웨어 설정은 세워져 있는 경우가 있다. */
	u8 ats_qdep;
	/* [한국어] 이 장치의 ATS 큐 깊이 — 한 번에 받아 처리할 수 있는 무효화 요청 수.
	 * 설정자: 프로브 때 pci_ats_queue_depth() 로 읽는다.
	 * 읽는 자: 디바이스 TLB 무효화 서술자에 실린다.
	 * 값 범위: 이보다 많이 보내면 장치가 요청을 흘려버려 무효화가 유실된다.
	 *   그래서 이 값이 서술자마다 함께 전달된다. */
	unsigned int iopf_refcount;
	/* [한국어] I/O 페이지 폴트 처리를 요구하는 사용자 수.
	 * 왜 참조 계수인가: 한 장치를 SVA 와 iommufd 가 동시에 쓸 수 있고 둘 다 폴트
	 *   처리를 요구한다. 먼저 끝난 쪽이 큐에서 장치를 빼면 남은 쪽의 페이지 요청이
	 *   응답을 받지 못하고, 응답 없는 PRI 요청은 장치를 영원히 멈춰 세운다.
	 * 설정자/읽는 자: intel_iommu_enable_iopf()/disable_iopf().
	 * 동기화: 그룹 뮤텍스(iommu_group_mutex_assert 로 확인만 한다). */
	struct device *dev; /* it's NULL for PCIe-to-PCI bridge */
	/* [한국어] 원본 struct device. 코어와 이 드라이버를 잇는 반대 방향의 포인터다
	 * (원 주석: it's NULL for PCIe-to-PCI bridge).
	 * 설정자: 프로브. 읽는 자: 로그, 폴트 보고, PCI 조회.
	 * 값 범위: 브리지처럼 실체 없는 항목에서는 NULL 일 수 있으므로, 이 포인터를
	 *   쓰기 전에 확인하는 코드가 곳곳에 있다. */
	struct intel_iommu *iommu; /* IOMMU used by this device */
	/* [한국어] 이 장치를 담당하는 유닛 (원 주석: IOMMU used by this device).
	 * 설정자: 프로브 때 device_lookup_iommu() 의 결과.
	 * 읽는 자: 이 장치에 대한 모든 하드웨어 조작 — 컨텍스트/PASID 항목 설정,
	 *   무효화, 능력 판정.
	 * 값 범위: 장치의 수명 동안 바뀌지 않는다. 유닛이 핫플러그로 사라지면
	 *   장치도 함께 해제된다. */
	struct dmar_domain *domain; /* pointer to domain */
	/* [한국어] 현재 붙어 있는 도메인 (원 주석: pointer to domain).
	 * 설정자: 부착 시 채우고 device_block_translation() 이 NULL 로 되돌린다.
	 * 읽는 자: 무효화 대상 결정, 도메인 전환, 폴트 처리.
	 * 값 범위: NULL 이면 어느 도메인에도 속하지 않는다 — 차단 상태이거나,
	 *   항등 도메인처럼 도메인 자료구조를 쓰지 않는 경우다. */
	struct pasid_table *pasid_table; /* pasid table */
	/* [한국어] 이 장치 전용 PASID 테이블 (원 주석: pasid table).
	 * scalable 모드에서 컨텍스트 항목은 페이지 테이블이 아니라 이 테이블(정확히는
	 * PASID 디렉터리)을 가리킨다. 즉 번역이 실제로 시작되는 곳이다.
	 * 설정자: intel_pasid_alloc_table() 이 프로브 때 만든다.
	 * 읽는 자: PASID 항목을 세우고 내리는 pasid.c 의 모든 경로.
	 * 값 범위: 레거시 모드이거나 부모의 항목을 공유하는 서브디바이스면 NULL. */
	/* device tracking node(lookup by PCI RID) */
	struct rb_node node;
	/* [한국어] 유닛의 device_rbtree 에 매달리는 노드. 키는 PCI_DEVID(bus, devfn) 이다.
	 * 설정자: device_rbtree_insert()/remove().
	 * 읽는 자: device_rbtree_find() — 폴트 인터럽트가 소스 id 로 장치를 되찾는다.
	 * 동기화: 유닛의 device_rbtree_lock(irqsave). link 와 마찬가지로 목록의
	 *   주인(여기서는 유닛)의 락이 지킨다. */
#ifdef CONFIG_INTEL_IOMMU_DEBUGFS
	struct dentry *debugfs_dentry; /* pointer to device directory dentry */
#endif
};

struct dev_pasid_info {
	struct list_head link_domain;	/* link to domain siblings */
	/* [한국어] 도메인의 dev_pasids 목록에 매달리는 고리 (원 주석: link to domain siblings).
	 * 설정자: domain_add_dev_pasid() 가 넣고 domain_remove_dev_pasid() 가 뺀다.
	 * 읽는 자: 도메인 단위 설정을 PASID 항목에도 전파하는 경로.
	 * 동기화: 도메인의 lock(irqsave). */
	struct device *dev;
	/* [한국어] 이 (장치, PASID) 쌍의 장치.
	 * 설정자: domain_add_dev_pasid(). 읽는 자: 목록에서 특정 쌍을 찾을 때의 비교,
	 *   그리고 그 장치의 유닛을 얻어 PASID 항목을 만질 때.
	 * 값 범위: NULL 이 아니다. 이 구조체의 수명은 장치보다 짧다 — 장치가 해제되기
	 *   전에 모든 PASID 가 먼저 떨어진다. */
	ioasid_t pasid;
	/* [한국어] 이 쌍의 PASID 번호.
	 * 설정자: domain_add_dev_pasid() 가 호출자가 준 값을 그대로 저장한다.
	 * 읽는 자: 목록 검색의 비교 키, 무효화 서술자의 PASID 필드, PASID 항목 색인.
	 * 값 범위: 0 ~ 2^(ecap_pss+1)-1. IOMMU_NO_PASID(0)는 PASID 를 쓰지 않는 기본
	 *   트래픽을 뜻하는 특별한 값이라 이 목록에는 들어오지 않는다. */
#ifdef CONFIG_INTEL_IOMMU_DEBUGFS
	struct dentry *debugfs_dentry; /* pointer to pasid directory dentry */
	/* [한국어] 이 PASID 의 debugfs 디렉터리 (원 주석: pointer to pasid directory dentry).
	 * 설정자: intel_iommu_debugfs_create_dev_pasid().
	 * 읽는 자: 해제 시 그 디렉터리를 지우는 경로.
	 * 값 범위: CONFIG_INTEL_IOMMU_DEBUGFS 를 끈 빌드에는 이 필드가 없다. */
#endif
};

/*
 * [한국어]
 * __iommu_flush_cache - 방금 고친 자료구조를 하드웨어가 볼 수 있게 메모리로 밀어낸다
 *
 * @iommu: 이 표를 읽을 유닛.
 * @addr: 밀어낼 메모리의 시작. @size: 그 크기.
 * @return: 없음.
 *
 * 왜 필요한가: 커널이 페이지 테이블이나 컨텍스트 항목을 고치면 그 값은 CPU
 * 캐시에만 있고 메모리에는 아직 반영되지 않았을 수 있다. IOMMU 는 메모리를
 * 직접 읽으므로, 캐시를 스누프하지 않는 유닛(!ecap_coherent)에서는 우리가
 * 쓴 값을 보지 못하고 옛 내용이나 쓰레기 값을 읽는다.
 *
 * ecap_coherent 인 유닛에서는 아무것도 하지 않는다 — 하드웨어가 CPU 캐시를
 * 스누프하므로 그냥 두어도 최신 값을 본다. 그래서 이 함수는 조건 검사만 하고
 * 끝나는 경우가 대부분이며, 그 조건 분기를 호출부마다 쓰지 않으려고 인라인
 * 함수로 뺐다.
 *
 * 실행 컨텍스트: 어디서든. clflush 는 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu_alloc_root_entry()/domain_context_mapping_one()/copy_context_table()
 *     → [__iommu_flush_cache] → clflush_cache_range()
 */
static inline void __iommu_flush_cache(
	struct intel_iommu *iommu, void *addr, int size)
{
	if (!ecap_coherent(iommu->ecap))	/* [한국어] 이 유닛의 워크가 CPU 캐시를 스누프하지 않으면 */
		clflush_cache_range(addr, size);	/* [한국어] 캐시 라인을 메모리로 밀어낸다. 코히런트한 유닛에서는 이 줄을 건너뛴다 */
}

/* Convert generic struct iommu_domain to private struct dmar_domain */
/*
 * [한국어] (위 영어 주석에 이어)
 * to_dmar_domain - 코어의 iommu_domain 에서 이 드라이버의 dmar_domain 을 되찾는다
 *
 * @dom: 코어가 다루는 도메인 포인터.
 * @return: 그것을 품고 있는 dmar_domain.
 *
 * 코어는 벤더 중립적인 iommu_domain 만 알고, 드라이버는 그것을 자기 구조체
 * 안에 박아 둔 뒤 container_of 로 되찾는다 — 커널 전반의 관용구다.
 *
 * dmar_domain 의 domain 필드가 union 안에 있는데도 이 계산이 성립하는 이유는,
 * 그 union 의 모든 얼굴이 같은 자리에서 iommu_domain 으로 시작하도록 배치되어
 * 있기 때문이다. 그 배치를 PT_IOMMU_CHECK_DOMAIN 이 컴파일 타임에 확인한다.
 *
 * 실행 컨텍스트: 어디서든. 순수한 포인터 산술이다.
 */
static inline struct dmar_domain *to_dmar_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct dmar_domain, domain);	/* [한국어] 감싸는 구조체로 되돌린다 */
}

/*
 * Domain ID 0 and 1 are reserved:
 *
 * If Caching mode is set, then invalid translations are tagged
 * with domain-id 0, hence we need to pre-allocate it. We also
 * use domain-id 0 as a marker for non-allocated domain-id, so
 * make sure it is not used for a real domain.
 *
 * Vt-d spec rev3.0 (section 6.2.3.1) requires that each pasid
 * entry for first-level or pass-through translation modes should
 * be programmed with a domain id different from those used for
 * second-level or nested translation. We reserve a domain id for
 * this purpose. This domain id is also used for identity domain
 * in legacy mode.
 */
#define FLPT_DEFAULT_DID		1	/* [한국어] 1단계·통과 변환 전용으로 예약한 도메인 id. 스펙 6.2.3.1 이 "1단계나 통과 모드의 PASID 항목은 2단계·중첩과 다른 도메인 id 를 써야 한다"고 요구해서 하나를 떼어 둔 것이며, 레거시 모드의 항등 도메인도 이 값을 쓴다 (위 영어 주석) */
#define IDA_START_DID			2	/* [한국어] 그래서 실제 도메인 할당은 2번부터 시작한다. 0번은 캐싱 모드 하드웨어가 "유효하지 않은 번역"에 붙이는 값이자 커널이 "할당되지 않음"의 표식으로도 쓰므로 진짜 도메인에 줄 수 없다 (위 영어 주석) */

/* Retrieve the domain ID which has allocated to the domain */
/*
 * [한국어] (위 영어 주석에 이어)
 * domain_id_iommu - 이 도메인이 저 유닛에서 받은 도메인 id 를 꺼낸다
 *
 * @domain: 대상 도메인. @iommu: 어느 유닛에서의 id 인지.
 * @return: 그 유닛에서 이 도메인에 할당된 16비트 도메인 id.
 *
 * 도메인 id 가 왜 유닛마다 다른가: id 는 시스템 전역의 이름이 아니라 유닛의
 * IOTLB 항목을 구분하는 태그일 뿐이다. 유닛마다 할당기(domain_ida)가 따로 있고
 * 개수 상한(cap_ndoms)도 다르므로, 같은 도메인이라도 유닛이 다르면 다른 번호를
 * 받을 수 있다. 그래서 도메인은 유닛 순번으로 색인된 xarray 에 그 매핑을
 * 담아 두고, 이 함수가 그것을 꺼낸다.
 *
 * xa_load 가 NULL 을 돌려주면 그 유닛에 이 도메인의 장치가 없다는 뜻인데,
 * 이 함수는 확인 없이 역참조한다 — 호출자가 "이 유닛에 붙어 있는 도메인"에
 * 대해서만 부르는 것을 전제로 한다.
 *
 * 실행 컨텍스트: 무효화 경로와 항목 설정. 인터럽트 문맥에서도 불릴 수 있다.
 *
 * 호출 체인:
 *   컨텍스트/PASID 항목 설정, 모든 도메인 단위 무효화 → [domain_id_iommu]
 */
static inline u16
domain_id_iommu(struct dmar_domain *domain, struct intel_iommu *iommu)
{
	struct iommu_domain_info *info =	/* [한국어] 이 유닛에서의 정보를 찾는다 */
			xa_load(&domain->iommu_array, iommu->seq_id);	/* [한국어] 유닛 순번으로 색인한다 */

	return info->did;	/* [한국어] 그 유닛에서 이 도메인에 할당된 id */
}

/*
 * [한국어]
 * iommu_domain_did - 코어 도메인에서 이 유닛의 도메인 id 를 구한다(특수 도메인 포함)
 *
 * @domain: 코어가 다루는 도메인. @iommu: 대상 유닛.
 * @return: 무효화에 쓸 도메인 id.
 *
 * domain_id_iommu 의 앞단으로, 도메인 id 를 따로 갖지 않는 두 종류를 먼저
 * 걸러 낸다.
 *   - SVA 도메인: 자기 페이지 테이블이 없고 프로세스의 것을 가리킨다.
 *   - 항등 도메인: 번역 자체를 하지 않는다.
 * 둘 다 도메인 id 를 할당받지 않으므로 FLPT_DEFAULT_DID 를 쓴다. 그 값이
 * 1단계·통과 전용으로 예약되어 있고, 이 두 도메인이 정확히 그 범주이기 때문이다.
 *
 * 나머지 페이징 도메인은 domain_id_iommu 로 넘겨 실제 할당된 id 를 얻는다.
 *
 * 실행 컨텍스트: 무효화 경로.
 *
 * 호출 체인:
 *   cache.c 의 무효화 경로 → [iommu_domain_did] → domain_id_iommu()
 */
static inline u16
iommu_domain_did(struct iommu_domain *domain, struct intel_iommu *iommu)
{
	if (domain->type == IOMMU_DOMAIN_SVA ||	/* [한국어] SVA 도메인이거나 */
	    domain->type == IOMMU_DOMAIN_IDENTITY)	/* [한국어] 항등 도메인이면 */
		return FLPT_DEFAULT_DID;	/* [한국어] 도메인 id 를 따로 갖지 않으므로 예약된 값을 쓴다 */
	return domain_id_iommu(to_dmar_domain(domain), iommu);	/* [한국어] 그 밖의 페이징 도메인은 실제 할당된 id 를 꺼낸다 */
}

/*
 * [한국어]
 * dev_is_real_dma_subdevice - 이 장치가 다른 장치의 이름으로 DMA 를 내는지 판별한다
 *
 * @dev: 검사할 장치.
 * @return: true 면 이 장치의 DMA 는 다른(부모) PCI 함수의 소스 id 로 나간다.
 *
 * 어떤 경우인가: SR-IOV 서브펑션이나 일부 통합 장치는 자기 PCI 함수를 갖고
 * 있으면서도 DMA 는 부모 함수의 소스 id 로 낸다. pci_real_dma_dev() 가 그
 * "실제로 DMA 를 내는 장치"를 알려 주므로, 그것이 자기 자신이 아니면 참이다.
 *
 * 왜 중요한가: 그런 장치는 자기 컨텍스트 항목도, 자기 PASID 테이블도 갖지
 * 않는다. 부모의 것을 공유하기 때문이다. 그래서 이 드라이버 곳곳에서
 *   - 컨텍스트/PASID 항목을 세우거나 내리지 않고(부모의 것을 건드리게 된다),
 *   - PASID 단위 부착을 거절하고(-EOPNOTSUPP),
 *   - 항등 도메인 부착에서 아무것도 하지 않는다
 * 는 예외 처리를 이 판별로 건다.
 *
 * 실행 컨텍스트: 어디서든. 순수 조회다.
 *
 * 호출 체인:
 *   intel_iommu_probe_device()/device_block_translation()/
 *   intel_iommu_set_dev_pasid()/identity_domain_attach_dev() → [이 함수]
 */
static inline bool dev_is_real_dma_subdevice(struct device *dev)
{
	return dev && dev_is_pci(dev) &&	/* [한국어] PCI 장치이면서 */
	       pci_real_dma_dev(to_pci_dev(dev)) != to_pci_dev(dev);	/* [한국어] 실제로 DMA 를 내는 장치가 자기 자신이 아니면 — 부모의 컨텍스트 항목을 공유한다는 뜻이다 */
}

/*
 * 0: readable
 * 1: writable
 * 2-6: reserved
 * 7: super page
 * 8-10: available
 * 11: snoop behavior
 * 12-63: Host physical address
 */
struct dma_pte {
	u64 val;
	/* [한국어] 2단계 페이지 테이블 항목 하나의 원본 64비트. 비트 배치는 바로 위 영어
	 * 주석 그대로다: 0=readable, 1=writable, 7=super page, 11=snoop behavior,
	 * 12-63=호스트 물리 주소.
	 * 1단계 PTE 와 다른 점: 1단계는 present 비트가 따로 있지만 2단계는 읽기/쓰기
	 *   권한 두 비트가 곧 존재 여부다 — 둘 다 0 이면 "매핑 없음"이다
	 *   (dma_pte_present 가 & 3 을 보는 이유).
	 * 설정자: 공용 페이지 테이블 라이브러리의 매핑 경로.
	 * 읽는 자: 하드웨어의 페이지 워크, 그리고 커널의 진단/조회 헬퍼들.
	 * 동기화: 항목 갱신은 원자적 64비트 쓰기여야 한다 — 절반만 바뀐 항목을
	 *   하드웨어가 읽으면 엉뚱한 물리 주소로 번역한다. 32비트 커널에서
	 *   dma_pte_addr 이 cmpxchg64 를 쓰는 이유가 그것이다. */
};

/*
 * [한국어]
 * dma_pte_addr - 2단계 PTE 에서 물리 주소만 뽑는다
 *
 * @pte: 읽을 항목.
 * @return: 플래그 비트를 털어 낸 페이지 정렬 물리 주소.
 *
 * 64비트 커널에서는 그냥 읽고 마스크하면 된다 — 64비트 읽기가 원자적이다.
 * 32비트 커널에서는 그렇지 않아서, 절반을 읽는 사이에 다른 CPU 가 항목을
 * 바꾸면 상위 32비트와 하위 32비트가 서로 다른 값에서 온 쓰레기가 된다.
 * 그래서 __cmpxchg64(&val, 0, 0) 으로 "바꾸지 않는 교환"을 걸어 원자적 읽기를
 * 흉내 낸다 (위 영어 주석: Must have a full atomic 64-bit read).
 *
 * 실행 컨텍스트: 어디서든. 매핑 조회와 진단 경로에서 쓴다.
 */
static inline u64 dma_pte_addr(struct dma_pte *pte)
{
#ifdef CONFIG_64BIT	/* [한국어] 64비트 커널에서는 */
	return pte->val & VTD_PAGE_MASK;	/* [한국어] 평범한 읽기로 충분하다 — 64비트 접근이 원자적이다 */
#else
	/* Must have a full atomic 64-bit read */
	return  __cmpxchg64(&pte->val, 0ULL, 0ULL) & VTD_PAGE_MASK;	/* [한국어] 32비트에서는 "바꾸지 않는 교환"으로 원자적 64비트 읽기를 흉내 낸다. 절반씩 읽으면 그 사이 항목이 바뀌어 상하위가 서로 다른 값에서 온 쓰레기가 될 수 있다 (위 영어 주석) */
#endif
}

/*
 * [한국어]
 * dma_pte_present - 이 2단계 PTE 가 유효한 매핑인지 본다
 *
 * @pte: 검사할 항목.
 * @return: true 면 매핑이 있다.
 *
 * & 3 인 것이 핵심이다. 2단계 PTE 에는 1단계와 달리 present 비트가 따로 없고,
 * 읽기(비트 0)와 쓰기(비트 1) 권한 중 하나라도 서 있으면 유효한 매핑으로
 * 본다. 둘 다 0 이면 하드웨어가 그 항목을 없는 것으로 취급한다.
 *
 * 그래서 매핑을 지울 때 권한 비트만 지워도 되고, 실제로 언매핑 경로가
 * 그렇게 한다.
 *
 * 실행 컨텍스트: 어디서든.
 */
static inline bool dma_pte_present(struct dma_pte *pte)
{
	return (pte->val & 3) != 0;	/* [한국어] 읽기 또는 쓰기 권한 중 하나라도 있으면 유효한 매핑이다 */
}

/*
 * [한국어]
 * dma_pte_superpage - 이 항목이 큰 페이지를 직접 가리키는지 본다
 *
 * @pte: 검사할 항목.
 * @return: true 면 하위 테이블이 아니라 2MB/1GB 페이지를 직접 가리킨다.
 *
 * 페이지 테이블을 워크할 때 이 비트를 만나면 거기서 멈춰야 한다 — 그 아래
 * 레벨이 없기 때문이다. 폴트 덤프(pgtable_walk)와 언매핑 경로가 그 판단에
 * 쓴다.
 *
 * 큰 페이지를 쓸 수 있는지는 하드웨어 능력(cap_super_page_val)이 정하고,
 * 도메인의 pgsize_bitmap 에 반영된다.
 *
 * 실행 컨텍스트: 어디서든.
 */
static inline bool dma_pte_superpage(struct dma_pte *pte)
{
	return (pte->val & DMA_PTE_LARGE_PAGE);	/* [한국어] 큰 페이지 표시 비트 */
}

/*
 * [한국어]
 * context_present - 컨텍스트 항목이 유효한지 본다
 *
 * @context: 검사할 항목.
 * @return: true 면 이 소스 id 에 대한 설정이 세워져 있다.
 *
 * 2단계 PTE 와 달리 컨텍스트 항목에는 present 비트(lo 의 비트 0)가 따로 있다.
 * 이 비트가 0 이면 그 장치의 DMA 는 전부 폴트로 끝난다 — 즉 이 한 비트가
 * "이 장치가 IOMMU 아래에서 동작할 수 있는가"를 정한다.
 *
 * 그래서 항목을 세울 때는 나머지 필드를 모두 채운 뒤 마지막에 이 비트를
 * 세우고(context_set_present), 내릴 때는 이 비트를 먼저 지운다.
 *
 * 실행 컨텍스트: iommu->lock 아래에서 주로 쓰인다.
 */
static inline bool context_present(struct context_entry *context)
{
	return (context->lo & 1);	/* [한국어] 컨텍스트 항목의 present 비트 */
}

#define LEVEL_STRIDE		(9)	/* [한국어] 페이지 테이블 한 단계가 다루는 인덱스 비트 수. 512개 항목이라 9다 */
#define LEVEL_MASK		(((u64)1 << LEVEL_STRIDE) - 1)	/* [한국어] 그 인덱스를 뽑는 마스크(0x1ff) */
#define MAX_AGAW_WIDTH		(64)	/* [한국어] 주소 폭의 이론적 상한 */
#define MAX_AGAW_PFN_WIDTH	(MAX_AGAW_WIDTH - VTD_PAGE_SHIFT)	/* [한국어] 그것을 페이지 프레임 번호 폭으로 바꾼 값(52). IOVA 범위 검사에 쓴다 */

/*
 * [한국어]
 * agaw_to_level - 주소 폭(AGAW) 값에서 페이지 테이블 단계 수를 구한다
 *
 * @agaw: Adjusted Guest Address Width 인덱스(0, 1, 2, ...).
 * @return: 그에 해당하는 최상위 테이블 레벨.
 *
 * +2 인 이유: AGAW 0 은 30비트(3단계 워크)에 대응한다. 한 단계가 9비트를
 * 다루고 페이지 오프셋이 12비트이므로 3단계는 12 + 9*3 = 39비트, 2단계는
 * 30비트다. 즉 AGAW 인덱스에 2 를 더하면 곧 레벨이 된다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산.
 */
static inline int agaw_to_level(int agaw)
{
	return agaw + 2;	/* [한국어] AGAW 0 이 30비트(2단계)에 대응하므로 2 를 더하면 레벨이 된다 */
}

/*
 * [한국어]
 * width_to_agaw - 주소 폭(비트 수)에서 AGAW 인덱스를 구한다
 *
 * @width: 다루려는 주소 폭(비트).
 * @return: 그것을 담을 수 있는 가장 작은 AGAW 인덱스.
 *
 * agaw_to_level 의 역방향이다. -30 은 AGAW 0 이 30비트에 대응하기 때문이고,
 * LEVEL_STRIDE(9)로 나누는 것은 한 단계가 9비트를 더하기 때문이다.
 * DIV_ROUND_UP 이라 요청한 폭보다 작아지는 일이 없다 — 모자라면 매핑할 수
 * 없는 주소가 생기므로 반드시 올림이어야 한다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산.
 */
static inline int width_to_agaw(int width)
{
	return DIV_ROUND_UP(width - 30, LEVEL_STRIDE);	/* [한국어] 30비트를 기준으로 몇 단계가 더 필요한지. 올림이라 요청한 폭보다 좁아지지 않는다 */
}

/*
 * [한국어]
 * level_to_offset_bits - 이 레벨의 인덱스가 주소의 몇 번째 비트부터 시작하는지
 *
 * @level: 페이지 테이블 레벨(1 이 가장 아래).
 * @return: 페이지 프레임 번호에서 이 레벨의 인덱스를 뽑을 시프트 값.
 *
 * 레벨 1(가장 아래)은 pfn 의 하위 9비트를 그대로 쓰므로 0, 레벨 2 는 9,
 * 레벨 3 은 18 … 이런 식이다. pfn_level_offset 이 이 값으로 시프트한다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산.
 */
static inline unsigned int level_to_offset_bits(int level)
{
	return (level - 1) * LEVEL_STRIDE;	/* [한국어] 레벨 1 은 0, 레벨 2 는 9, 레벨 3 은 18 … */
}

/*
 * [한국어]
 * pfn_level_offset - 페이지 프레임 번호에서 이 레벨의 테이블 인덱스를 뽑는다
 *
 * @pfn: IOVA >> 12. @level: 어느 레벨의 인덱스인지.
 * @return: 그 레벨 테이블에서의 항목 번호(0~511).
 *
 * 다단계 페이지 테이블 워크의 핵심 계산이다. 주소를 9비트씩 잘라 각 레벨의
 * 인덱스로 쓰는데, 이 함수가 그 "자르기"를 한다: 해당 레벨의 시작 비트까지
 * 시프트한 뒤 LEVEL_MASK(0x1ff)로 9비트만 남긴다.
 *
 * pgtable_walk() 가 레벨을 내려가며 이 함수로 각 단계의 항목을 찾는다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산.
 */
static inline int pfn_level_offset(u64 pfn, int level)
{
	return (pfn >> level_to_offset_bits(level)) & LEVEL_MASK;	/* [한국어] 해당 레벨의 자리까지 시프트한 뒤 9비트만 남긴다 — 다단계 워크의 인덱스 계산이다 */
}


/*
 * [한국어]
 * context_set_present - 컨텍스트 항목의 present 비트를 세운다(= 하드웨어에 넘긴다)
 *
 * @context: 이미 나머지 필드가 모두 채워진 항목.
 * @return: 없음.
 *
 * 이 함수가 컨텍스트 항목의 소유권을 소프트웨어에서 하드웨어로 넘기는 지점이다.
 * 이 비트가 서는 순간부터 유닛이 그 항목을 실제로 워크하기 시작한다.
 *
 * dma_wmb() 가 먼저 오는 것이 핵심이다. 컴파일러나 CPU 가 present 쓰기를
 * 나머지 필드 쓰기보다 앞으로 옮기면, 하드웨어가 절반만 채워진 항목을 보고
 * 엉뚱한 페이지 테이블이나 도메인 id 로 번역한다. 그 재배치를 막는 장벽이다.
 *
 * READ_ONCE/WRITE_ONCE 를 쓰는 이유: 컴파일러가 이 읽기·쓰기를 쪼개거나
 * 합치지 못하게 한다. 하드웨어가 동시에 같은 항목을 읽고 있으므로 반드시
 * 한 번의 온전한 64비트 쓰기여야 한다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_present(struct context_entry *context)
{
	u64 val;	/* [한국어] 읽어서 고칠 값 */

	dma_wmb();	/* [한국어] 나머지 필드 쓰기가 present 쓰기보다 먼저 보이도록 강제한다. 순서가 뒤집히면 하드웨어가 절반만 채워진 항목을 워크한다 */
	val = READ_ONCE(context->lo) | 1;	/* [한국어] present 비트를 더한다 */
	WRITE_ONCE(context->lo, val);	/* [한국어] 한 번의 온전한 64비트 쓰기로 반영한다 */
}

/*
 * Clear the Present (P) bit (bit 0) of a context table entry. This initiates
 * the transition of the entry's ownership from hardware to software. The
 * caller is responsible for fulfilling the invalidation handshake recommended
 * by the VT-d spec, Section 6.5.3.3 (Guidance to Software for Invalidations).
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * context_clear_present - present 비트를 지운다(= 하드웨어에서 되찾아 온다)
 *
 * @context: 내릴 항목.
 * @return: 없음.
 *
 * set 의 반대 방향으로, 항목의 소유권을 하드웨어에서 소프트웨어로 되돌린다.
 * 이 뒤로 그 소스 id 의 DMA 는 전부 폴트로 끝난다.
 *
 * 장벽의 위치가 set 과 반대인 점을 눈여겨볼 것: set 은 dma_wmb 가 앞에,
 * clear 는 뒤에 온다. set 은 "나머지 필드가 먼저 보여야" 하고, clear 는
 * "present 를 지운 것이 이후의 정리 작업보다 먼저 보여야" 하기 때문이다.
 *
 * 중요한 것은 이 함수만으로는 끝이 아니라는 점이다(위 영어 주석). 하드웨어는
 * 이미 이 항목을 캐시하고 있을 수 있으므로, 호출자가 스펙 6.5.3.3 이 권하는
 * 무효화 핸드셰이크(컨텍스트 캐시 → PASID 캐시 → IOTLB 순)를 수행해야
 * 비로소 하드웨어가 이 변경을 본다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_clear_present(struct context_entry *context)
{
	u64 val;	/* [한국어] 읽어서 고칠 값 */

	val = READ_ONCE(context->lo) & GENMASK_ULL(63, 1);	/* [한국어] 비트 0(present)만 지우고 나머지는 남긴다 */
	WRITE_ONCE(context->lo, val);	/* [한국어] 한 번의 쓰기로 반영 */
	dma_wmb();	/* [한국어] present 를 지운 것이 이후의 정리 작업보다 먼저 보이도록 한다. set 과 장벽의 위치가 반대인 이유다 */
}

/*
 * [한국어]
 * context_set_fault_enable - 이 항목에 대한 폴트 보고를 켠다
 *
 * @context: 대상 항목.
 * @return: 없음.
 *
 * lo 의 비트 1 이 FPD(Fault Processing Disable)이며, 이름대로 1 이면 폴트를
 * 보고하지 않는다. 이 함수는 그 비트를 0 으로 만들어 보고를 켜는 것이다 —
 * 함수 이름과 비트의 의미가 반대라 헷갈리기 쉬운 자리다.
 *
 * 마스크 (((u64)-1) << 2) | 1 은 "비트 1 만 지우고 나머지는 보존"이다:
 * 상위 비트 전부와 비트 0(present)은 남기고 비트 1 만 떨어뜨린다.
 *
 * 폴트 보고를 켜 두는 이유는 통과 모드에서도 마찬가지다 — 하드웨어가 다룰 수
 * 있는 범위를 벗어난 접근은 여전히 알아야 하기 때문이다.
 *
 * 실행 컨텍스트: iommu->lock 아래, present 를 세우기 전에.
 */
static inline void context_set_fault_enable(struct context_entry *context)
{
	context->lo &= (((u64)-1) << 2) | 1;	/* [한국어] 비트 1(Fault Processing Disable)만 떨어뜨려 폴트 보고를 켠다. 비트 0 과 상위 비트는 보존한다 */
}

/*
 * [한국어]
 * context_set_translation_type - 이 항목의 번역 방식을 정한다
 *
 * @context: 대상 항목. @value: CONTEXT_TT_* 중 하나.
 * @return: 없음.
 *
 * lo 의 비트 2-3 이 translation type 이고, 이 두 비트가 항목의 성격을 정한다.
 *   CONTEXT_TT_MULTI_LEVEL(0)  — 페이지 테이블을 워크한다(보통의 번역).
 *   CONTEXT_TT_DEV_IOTLB(1)    — 워크하되 디바이스 TLB 도 함께 쓴다.
 *   CONTEXT_TT_PASS_THROUGH(2) — 번역하지 않고 통과시킨다.
 *
 * 두 줄로 나뉜 이유: 먼저 마스크로 그 자리를 비우고(&=), 그 다음 새 값을
 * 넣는다(|=). 한 번에 대입하지 않는 것은 같은 워드의 다른 필드
 * (present, fault enable, address root)를 보존해야 하기 때문이다.
 * 이 파일의 context_set_* 계열이 모두 같은 형태다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_translation_type(struct context_entry *context,
						unsigned long value)
{
	context->lo &= (((u64)-1) << 4) | 3;	/* [한국어] 비트 2-3(translation type) 자리를 비운다 */
	context->lo |= (value & 3) << 2;	/* [한국어] 거기에 새 값을 넣는다 */
}

/*
 * [한국어]
 * context_set_address_root - 이 항목이 가리킬 최상위 테이블의 물리 주소를 넣는다
 *
 * @context: 대상 항목. @value: 페이지 정렬된 물리 주소.
 * @return: 없음.
 *
 * 무엇을 가리키는지는 모드에 따라 다르다. 레거시 모드에서는 페이지 테이블의
 * 최상위를, scalable 모드에서는 PASID 디렉터리를 가리킨다 — 같은 필드가
 * 루트 테이블 주소의 SMT 비트 하나로 다르게 해석되는 것이다.
 * 통과 모드에서는 하드웨어가 이 필드를 아예 무시한다(ASR is ignored).
 *
 * VTD_PAGE_MASK 로 하위 12비트를 비운 뒤 넣으므로, 주소가 페이지 정렬되어
 * 있지 않으면 조용히 잘린다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_address_root(struct context_entry *context,
					    unsigned long value)
{
	context->lo &= ~VTD_PAGE_MASK;	/* [한국어] 주소 자리를 비운다 */
	context->lo |= value & VTD_PAGE_MASK;	/* [한국어] 페이지 정렬된 주소만 넣는다. 정렬되지 않은 값은 조용히 잘린다 */
}

/*
 * [한국어]
 * context_set_address_width - 이 항목이 쓸 주소 폭(= 테이블 단계 수)을 넣는다
 *
 * @context: 대상 항목. @value: AGAW 인덱스.
 * @return: 없음.
 *
 * hi 의 비트 0-2 다. 하드웨어는 이 값으로 페이지 테이블을 몇 단계 워크할지
 * 안다 — 실제 테이블의 깊이와 어긋나면 엉뚱한 메모리를 테이블로 읽는다.
 *
 * |= 만 하고 마스크를 먼저 걸지 않는 이유: 이 함수는 항상 갓 비운 항목
 * (context_clear_entry 직후)에 대해서만 불린다. 값이 남아 있는 항목에 쓰면
 * 비트가 섞이므로, 호출 순서가 이 함수의 전제다.
 *
 * 통과 모드에서는 하드웨어가 지원하는 최대 AGAW(iommu->msagaw)를 넣어야
 * 한다는 스펙 요구가 있다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_address_width(struct context_entry *context,
					     unsigned long value)
{
	context->hi |= value & 7;	/* [한국어] 비트 0-2 에 AGAW 를 넣는다. 갓 비운 항목을 전제로 마스크 없이 |= 만 한다 */
}

/*
 * [한국어]
 * context_set_domain_id - 이 항목에 도메인 id 를 붙인다
 *
 * @context: 대상 항목. @value: 이 유닛에서의 도메인 id.
 * @return: 없음.
 *
 * hi 의 비트 8-23 이다. 이 id 가 하드웨어 IOTLB 항목에 태그로 붙어, 도메인
 * 단위 무효화가 어느 항목을 지울지를 정한다. 같은 id 를 쓰는 장치들의 번역은
 * 캐시에서 공유되므로, 무효화 한 번이 그 모두에 적용된다 — cache.c 의
 * cache tag 모델이 이 성질 위에 세워져 있다.
 *
 * 마스크가 (1 << 16) - 1 인 것은 필드가 16비트이기 때문이지만, 실제 상한은
 * cap_ndoms(cap) 이며 그보다 큰 값을 넣으면 하드웨어가 무시하거나 오동작한다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_domain_id(struct context_entry *context,
					 unsigned long value)
{
	context->hi |= (value & ((1 << 16) - 1)) << 8;	/* [한국어] 비트 8-23 에 도메인 id 를 넣는다 */
}

/*
 * [한국어]
 * context_set_pasid - 이 항목에서 PASID 를 쓴다고 표시한다
 *
 * @context: 대상 항목.
 * @return: 없음.
 *
 * lo 의 비트 3(CONTEXT_PASIDE)을 세운다. scalable 모드에서 이 비트가 켜져야
 * 하드웨어가 address root 를 PASID 디렉터리로 해석하고, 요청에 실린 PASID 로
 * 그 디렉터리를 색인한다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_pasid(struct context_entry *context)
{
	context->lo |= CONTEXT_PASIDE;	/* [한국어] PASID 사용 표시. 이 비트가 켜져야 address root 가 PASID 디렉터리로 해석된다 */
}

/*
 * [한국어]
 * context_domain_id - 컨텍스트 항목에 적힌 도메인 id 를 읽는다
 *
 * @c: 읽을 항목.
 * @return: 그 항목의 도메인 id.
 *
 * context_set_domain_id 의 역방향. 주로 kdump 인계 경로에서 쓰인다 —
 * 이전 커널의 컨텍스트 항목이 쓰던 도메인 id 를 읽어, 우리 할당기에서 그
 * 번호를 미리 예약해 두어야 나중에 다른 도메인에 같은 번호를 주지 않는다.
 * 진단 덤프에서도 쓴다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산.
 */
static inline int context_domain_id(struct context_entry *c)
{
	return((c->hi >> 8) & 0xffff);	/* [한국어] 비트 8-23 을 도로 꺼낸다 */
}

/*
 * [한국어]
 * context_clear_entry - 컨텍스트 항목을 완전히 비운다
 *
 * @context: 비울 항목.
 * @return: 없음.
 *
 * lo/hi 를 모두 0 으로 만든다. present 가 0 이 되므로 그 소스 id 의 DMA 는
 * 전부 폴트로 끝난다.
 *
 * context_clear_present 와 다른 점: 저쪽은 present 만 지워 나머지 설정을
 * 남기고(진단이나 재사용을 위해), 이쪽은 통째로 지운다. 새 항목을 세우기
 * 직전에 불려 이전 값이 섞이지 않게 하는 것이 주 용도다 — 위의
 * context_set_address_width 처럼 마스크 없이 |= 만 하는 함수들이 이 선행
 * 조건에 기대고 있다.
 *
 * 이 함수만으로는 하드웨어에 반영되지 않는다. 캐시 무효화가 뒤따라야 한다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_clear_entry(struct context_entry *context)
{
	context->lo = 0;	/* [한국어] 하위 워드를 비운다 — present 가 0 이 되므로 이 시점부터 무효한 항목이다 */
	context->hi = 0;	/* [한국어] 상위 워드도 비운다 */
}

#ifdef CONFIG_INTEL_IOMMU
/*
 * [한국어]
 * context_copied - 이 소스 id 의 컨텍스트 항목이 이전 커널에서 물려받은 것인지
 *
 * @iommu: 대상 유닛. @bus, @devfn: 확인할 소스 id.
 * @return: true 면 그 항목은 우리가 만든 것이 아니라 인계받은 것이다.
 *
 * kdump 커널은 이전 커널의 번역 표를 그대로 이어받아, 크래시 시점에 진행 중이던
 * DMA(특히 덤프를 쓸 디스크 컨트롤러)가 끊기지 않게 한다. 그런데 그 항목은
 * 이전 커널의 페이지 테이블을 가리키고 있으므로, 그 장치를 실제로 쓰게 될 때
 * 우리 형식으로 다시 세워야 한다. 이 함수가 "아직 인계 상태인가"를 답한다.
 *
 * 비트맵 인덱스가 (bus << 8) | devfn 인 것은 소스 id 그 자체다 — 65536개
 * 비트로 한 유닛의 모든 소스 id 를 표시할 수 있다.
 * copied_tables 가 NULL 이면 보통의 부팅이라 인계받은 것이 없다.
 *
 * 이 값을 보고 갈라지는 곳들:
 *   - 프로브: 인계 상태면 컨텍스트를 세우지 않고 그대로 둔다.
 *   - 부착: paging_domain_compatible() 이 그때 우리 형식으로 전환한다.
 *   - 해제: 인계받은 것은 내리지 않는다(다음 커널이 이어받을 수 있게).
 *
 * 실행 컨텍스트: 어디서든. 순수 조회다.
 */
static inline bool context_copied(struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	if (!iommu->copied_tables)	/* [한국어] 인계받은 것이 없는 보통의 부팅이면 */
		return false;

	return test_bit(((long)bus << 8) | devfn, iommu->copied_tables);	/* [한국어] 소스 id 를 인덱스로 비트를 확인한다 */
}

/*
 * [한국어]
 * set_context_copied - 이 소스 id 를 "인계받은 항목"으로 표시한다
 *
 * @iommu: 대상 유닛. @bus, @devfn: 표시할 소스 id.
 * @return: 없음.
 *
 * copy_translation_tables() 가 이전 커널의 컨텍스트 테이블을 복사하면서,
 * present 인 항목마다 이 표시를 남긴다. 그래야 나중에 그 항목이 우리 것인지
 * 물려받은 것인지 구분할 수 있다.
 *
 * 실행 컨텍스트: 부팅 초기화(kdump 경로). 단일 스레드.
 */
static inline void
set_context_copied(struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	set_bit(((long)bus << 8) | devfn, iommu->copied_tables);	/* [한국어] 그 소스 id 를 인계 상태로 표시한다 */
}

/*
 * [한국어]
 * clear_context_copied - 인계 표시를 지운다(= 이제 우리 항목이다)
 *
 * @iommu: 대상 유닛. @bus, @devfn: 대상 소스 id.
 * @return: 없음.
 *
 * 인계받은 항목을 우리 형식으로 다시 세운 뒤 부른다. 이 뒤로는 보통의 항목과
 * 똑같이 다뤄지며, 해제 시에도 정상적으로 내려간다.
 *
 * 인계에서 우리 것으로 넘어오는 지점이 두 군데다: 레거시 모드에서는
 * copied_context_tear_down() 이, scalable 모드에서는
 * intel_pasid_setup_sm_context() 경로가 이 표시를 지운다.
 *
 * 실행 컨텍스트: 장치 부착. 프로세스 컨텍스트.
 */
static inline void
clear_context_copied(struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	clear_bit(((long)bus << 8) | devfn, iommu->copied_tables);	/* [한국어] 표시를 지운다 — 이제 우리가 세운 항목이다 */
}
#endif /* CONFIG_INTEL_IOMMU */

/*
 * Set the RID_PASID field of a scalable mode context entry. The
 * IOMMU hardware will use the PASID value set in this field for
 * DMA translations of DMA requests without PASID.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * context_set_sm_rid2pasid - scalable 모드 항목에 "PASID 없는 요청이 쓸 PASID"를 넣는다
 *
 * @context: 대상 컨텍스트 항목. @pasid: 기본으로 쓸 PASID 값.
 * @return: 없음.
 *
 * scalable 모드에서는 번역이 항상 PASID 항목에서 시작한다. 그런데 대부분의
 * 장치는 PASID 를 실어 보내지 않는 평범한 DMA 를 낸다. 그런 요청에 어떤
 * PASID 를 붙일지를 이 필드(RID_PASID)가 정한다 (위 영어 주석).
 *
 * 그래서 이 값이 곧 "이 장치의 기본 주소 공간"을 가리키게 되고, 보통
 * IOMMU_NO_PASID(0)가 들어간다. 즉 레거시 모드에서 컨텍스트 항목이 바로
 * 페이지 테이블을 가리키던 자리를, scalable 모드에서는 "0번 PASID 항목"이
 * 대신하는 셈이다.
 *
 * 마스크가 20비트인 것은 PASID 필드의 폭이 20비트이기 때문이다.
 *
 * 실행 컨텍스트: iommu->lock 아래, 항목을 세우는 중.
 */
static inline void
context_set_sm_rid2pasid(struct context_entry *context, unsigned long pasid)
{
	context->hi |= pasid & ((1 << 20) - 1);	/* [한국어] PASID 필드는 20비트다 */
}

/*
 * Set the DTE(Device-TLB Enable) field of a scalable mode context
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * context_set_sm_dte - scalable 모드 항목에서 디바이스 TLB(ATS)를 켠다
 *
 * @context: 대상 항목.
 * @return: 없음.
 *
 * lo 의 비트 2 가 DTE(Device-TLB Enable)다. 이 비트가 서야 하드웨어가 이
 * 장치의 ATS 요청을 받아들이고 번역 결과를 돌려준다.
 *
 * 레거시 모드에서 같은 일을 하던 것이 translation type 의
 * CONTEXT_TT_DEV_IOTLB 였다. scalable 모드는 그것을 별도 비트로 분리해,
 * 번역 방식과 ATS 사용 여부를 독립적으로 정할 수 있게 했다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_sm_dte(struct context_entry *context)
{
	context->lo |= BIT_ULL(2);	/* [한국어] DTE — 디바이스 TLB 사용 허용 */
}

/*
 * Set the PRE(Page Request Enable) field of a scalable mode context
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * context_set_sm_pre - scalable 모드 항목에서 페이지 요청(PRI)을 켠다
 *
 * @context: 대상 항목.
 * @return: 없음.
 *
 * lo 의 비트 4 가 PRE(Page Request Enable)다. 이 비트가 서야 장치가 보낸
 * 페이지 요청을 하드웨어가 받아 PRQ 에 넣는다.
 *
 * DTE 와 짝을 이루는 이유: 페이지 요청의 응답은 ATS 번역 경로로 돌아온다.
 * 그래서 PRI 를 켜려면 ATS 가 먼저 켜져 있어야 하고, 이 파일의 능력 판정도
 * pri_supported 에 ats_supported 를 전제로 두고 있다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_set_sm_pre(struct context_entry *context)
{
	context->lo |= BIT_ULL(4);	/* [한국어] PRE — 페이지 요청 허용 */
}

/*
 * Clear the PRE(Page Request Enable) field of a scalable mode context
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * context_clear_sm_pre - 페이지 요청을 끈다
 *
 * @context: 대상 항목.
 * @return: 없음.
 *
 * 켜기의 반대. 주의할 점은 이 비트를 끈 뒤에도 이미 큐에 들어와 있거나
 * 하드웨어 안에서 진행 중인 요청이 남아 있을 수 있다는 것이다. 응답하지
 * 않은 페이지 요청은 장치를 영원히 멈춰 세우므로, 끄기만 해서는 안 되고
 * 남은 요청을 배수(drain)해야 한다 — ecap_pds 와 QI_IWD_PRQ_DRAIN 이
 * 그 일을 한다.
 *
 * 실행 컨텍스트: iommu->lock 아래.
 */
static inline void context_clear_sm_pre(struct context_entry *context)
{
	context->lo &= ~BIT_ULL(4);	/* [한국어] PRE 를 내린다. 남은 요청의 배수는 별도로 해야 한다 */
}

/* Returns a number of VTD pages, but aligned to MM page size */
/*
 * [한국어] (위 영어 주석에 이어)
 * aligned_nrpages - 이 주소·크기가 걸치는 VT-d 페이지 수를 센다
 *
 * @host_addr: 시작 주소(정렬되지 않아도 된다). @size: 바이트 크기.
 * @return: 덮어야 할 4KB 페이지 개수.
 *
 * 왜 단순히 size >> 12 가 아닌가: 시작 주소가 페이지 중간이면 같은 크기라도
 * 한 페이지를 더 걸친다. 예를 들어 오프셋 4095 에서 2바이트는 두 페이지에
 * 걸친다. 그래서 먼저 페이지 안 오프셋만 남기고(&= ~PAGE_MASK), 그것을
 * 크기에 더한 뒤 올림 정렬한다.
 *
 * PAGE_MASK(호스트 페이지)와 VTD_PAGE_SHIFT(항상 4KB)가 섞여 있는 점을
 * 눈여겨볼 것: 정렬은 호스트 페이지 기준으로 하고 개수는 VT-d 페이지로 센다.
 * 호스트가 64KB 페이지를 쓰더라도 IOMMU 항목은 4KB 단위이기 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산.
 */
static inline unsigned long aligned_nrpages(unsigned long host_addr, size_t size)
{
	host_addr &= ~PAGE_MASK;	/* [한국어] 페이지 안 오프셋만 남긴다 */
	return PAGE_ALIGN(host_addr + size) >> VTD_PAGE_SHIFT;	/* [한국어] 오프셋을 더해 올림 정렬한 뒤 4KB 페이지 수로 바꾼다. 시작이 페이지 중간이면 한 페이지를 더 걸치는 것이 이 계산으로 반영된다 */
}

/* Return a size from number of VTD pages. */
/*
 * [한국어] (위 영어 주석에 이어)
 * nrpages_to_size - VT-d 페이지 수를 바이트 크기로 되돌린다
 *
 * @npages: 페이지 개수.
 * @return: 그만큼의 바이트 수.
 *
 * aligned_nrpages 의 역방향. 무효화 범위를 계산할 때 두 표현을 오간다 —
 * 페이지 수로 세다가 주소 범위로 바꿔야 하는 지점이 있기 때문이다.
 *
 * 실행 컨텍스트: 어디서든. 순수 계산.
 */
static inline unsigned long nrpages_to_size(unsigned long npages)
{
	return npages << VTD_PAGE_SHIFT;	/* [한국어] 페이지 수 × 4KB */
}

/*
 * [한국어]
 * qi_desc_iotlb - IOTLB 무효화 서술자를 조립한다
 *
 * @iommu: 이 서술자를 받을 유닛(능력을 보고 배수 플래그를 정한다).
 * @did: 대상 도메인 id. @addr: 무효화할 주소(하위 비트에 IH 힌트가 실려 온다).
 * @size_order: 범위 크기의 로그값. @type: DMA_TLB_*_FLUSH 중 하나.
 * @desc: 채울 서술자.
 * @return: 없음.
 *
 * 이 파일의 qi_desc_* 계열은 "무효화 명령을 만드는 곳"이고, 실제 제출은
 * dmar.c 의 qi_submit_sync() 가 한다. 조립과 제출을 나눈 이유는 cache.c 가
 * 여러 서술자를 모아 한 번에 보내기 때문이다(qi_batch).
 *
 * 배수(drain) 플래그가 능력에 따라 갈리는 것이 중요하다. cap_read_drain /
 * cap_write_drain 을 지원하는 유닛에서만 DR/DW 를 세우는데, 이 비트가 있어야
 * "무효화 완료"가 "옛 번역으로 가는 진행 중인 전송이 더 이상 없다"를 뜻한다.
 * 지원하지 않는 유닛에서는 완료를 받아도 그 보장이 없어, 상위 코드가
 * 언매핑 후 페이지 재사용을 더 조심해야 한다.
 *
 * ih = addr & 1 은 주소의 최하위 비트를 힌트로 쓰는 관용구다. 주소는 페이지
 * 정렬이라 하위 12비트가 비어 있으므로, 호출자가 거기에 IH(중간 단계 항목은
 * 그대로 두라)를 실어 보낸다.
 *
 * qw2/qw3 를 0 으로 미는 이유: scalable 모드에서 서술자가 32바이트로 커지면
 * 이 두 워드도 하드웨어가 읽는다. 예약 필드에 값이 남아 있으면 서술자가
 * 거부되므로 반드시 비워야 한다.
 *
 * 실행 컨텍스트: 무효화 경로. 락을 쥔 채 불릴 수 있어 잠들면 안 된다.
 *
 * 호출 체인:
 *   cache_tag_flush_range() 등 → [qi_desc_iotlb] → (나중에) qi_submit_sync()
 */
static inline void qi_desc_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
				 unsigned int size_order, u64 type,
				 struct qi_desc *desc)
{
	u8 dw = 0, dr = 0;	/* [한국어] 쓰기/읽기 배수 플래그. 기본은 끔 */
	int ih = addr & 1;	/* [한국어] 주소의 최하위 비트에 실려 온 IH 힌트를 꺼낸다. 주소는 페이지 정렬이라 그 자리가 비어 있어 이렇게 겸용한다 */

	if (cap_write_drain(iommu->cap))	/* [한국어] 이 유닛이 쓰기 배수를 지원하면 */
		dw = 1;	/* [한국어] 켠다 — 이 플래그가 있어야 무효화 완료가 "옛 번역으로 가는 쓰기가 더 이상 없다"를 뜻한다 */

	if (cap_read_drain(iommu->cap))	/* [한국어] 읽기 배수도 지원하면 */
		dr = 1;	/* [한국어] 켠다 */

	desc->qw0 = QI_IOTLB_DID(did) | QI_IOTLB_DR(dr) | QI_IOTLB_DW(dw)	/* [한국어] 도메인 id 와 배수 플래그를 */
		| QI_IOTLB_GRAN(type) | QI_IOTLB_TYPE;	/* [한국어] 범위 종류·명령 종류와 함께 첫 워드에 담는다 */
	desc->qw1 = QI_IOTLB_ADDR(addr) | QI_IOTLB_IH(ih)	/* [한국어] 둘째 워드에는 주소와 힌트를 */
		| QI_IOTLB_AM(size_order);	/* [한국어] 범위 크기와 함께 담는다 */
	desc->qw2 = 0;	/* [한국어] scalable 모드의 32바이트 서술자에서 하드웨어가 읽으므로 반드시 비운다 */
	desc->qw3 = 0;	/* [한국어] 같은 이유 */
}

/*
 * [한국어]
 * qi_desc_dev_iotlb - 디바이스 TLB(ATS) 무효화 서술자를 조립한다
 *
 * @sid: 대상 장치의 소스 id. @pfsid: SR-IOV 라면 PF 의 소스 id.
 * @qdep: 그 장치의 ATS 큐 깊이. @addr: 무효화할 주소. @mask: 범위 크기(로그값).
 * @desc: 채울 서술자.
 * @return: 없음.
 *
 * IOTLB 무효화가 유닛 안의 캐시를 지우는 것이라면, 이쪽은 장치 안의 캐시를
 * 지운다. ATS 를 켠 장치는 번역 결과를 자기 안에 캐시하므로, 매핑을 풀 때
 * 두 곳을 모두 비워야 한다.
 *
 * 범위 표현 방식이 IOTLB 와 다르다. 여기서는 별도의 크기 필드가 아니라
 * "주소의 하위 비트를 1 로 채우고 S 비트를 세우는" 방식으로 범위를 나타낸다:
 *   addr |= (1 << (12 + mask - 1)) - 1
 * 그래서 mask 가 0 이면 한 페이지, 그보다 크면 그만큼 넓은 범위가 된다.
 *
 * qdep 을 상한에서 잘라 0 으로 만드는 것: 큐 깊이가 QI_DEV_IOTLB_MAX_INVS
 * 이상이면 필드에 담을 수 없다. 0 은 "제한 없음"으로 해석되므로, 잘라 내는
 * 대신 0 을 보내 하드웨어가 알아서 조절하게 한다.
 *
 * 실행 컨텍스트: 무효화 경로. 잠들면 안 된다.
 *
 * 호출 체인:
 *   cache_tag_flush_range() (DEVTLB 태그) → [qi_desc_dev_iotlb]
 */
static inline void qi_desc_dev_iotlb(u16 sid, u16 pfsid, u16 qdep, u64 addr,
				     unsigned int mask, struct qi_desc *desc)
{
	if (mask) {	/* [한국어] 한 페이지보다 넓은 범위면 */
		addr |= (1ULL << (VTD_PAGE_SHIFT + mask - 1)) - 1;	/* [한국어] 주소의 하위 비트를 1 로 채워 범위를 표현한다. 디바이스 TLB 는 별도 크기 필드가 없어 주소 자체로 범위를 나타낸다 */
		desc->qw1 = QI_DEV_IOTLB_ADDR(addr) | QI_DEV_IOTLB_SIZE;	/* [한국어] S 비트를 세워 "한 페이지가 아니다"를 알린다 */
	} else {
		desc->qw1 = QI_DEV_IOTLB_ADDR(addr);	/* [한국어] 한 페이지면 주소만 넣는다 */
	}

	if (qdep >= QI_DEV_IOTLB_MAX_INVS)	/* [한국어] 큐 깊이가 필드에 담을 수 없을 만큼 크면 */
		qdep = 0;	/* [한국어] 0 을 보낸다. 0 은 "제한 없음"으로 해석되어 하드웨어가 알아서 조절한다 */

	desc->qw0 = QI_DEV_IOTLB_SID(sid) | QI_DEV_IOTLB_QDEP(qdep) |	/* [한국어] 대상 장치와 큐 깊이를 */
		   QI_DIOTLB_TYPE | QI_DEV_IOTLB_PFSID(pfsid);	/* [한국어] 명령 종류·PF 소스 id 와 함께 담는다 */
	desc->qw2 = 0;	/* [한국어] scalable 모드의 32바이트 서술자에서 하드웨어가 읽으므로 반드시 비운다 */
	desc->qw3 = 0;	/* [한국어] 같은 이유 */
}

/* PASID-selective IOTLB invalidation */
/*
 * [한국어] (위 영어 주석에 이어)
 * qi_desc_piotlb_all - 한 PASID 의 IOTLB 항목을 통째로 무효화하는 서술자
 *
 * @did: 대상 도메인 id. @pasid: 대상 PASID. @desc: 채울 서술자.
 * @return: 없음.
 *
 * 확장(EIOTLB) 형식이라 PASID 필드가 있다. 범위를 지정하지 않고 그 PASID 에
 * 속한 항목 전부를 지운다(QI_GRAN_NONG_PASID).
 *
 * 언제 쓰는가: 그 주소 공간이 통째로 사라질 때 — SVA 에서 프로세스가 죽거나,
 * PASID 를 회수할 때다. 범위를 하나하나 지우는 것보다 한 번에 비우는 편이
 * 훨씬 싸다.
 *
 * qw1 을 0 으로 두는 것은 주소와 범위를 쓰지 않기 때문이다. 이 형식은
 * 16바이트 서술자라 qw2/qw3 는 아예 건드리지 않는다.
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static inline void qi_desc_piotlb_all(u16 did, u32 pasid, struct qi_desc *desc)
{
	desc->qw0 = QI_EIOTLB_PASID(pasid) | QI_EIOTLB_DID(did) |	/* [한국어] PASID 와 도메인 id 를 */
		    QI_EIOTLB_GRAN(QI_GRAN_NONG_PASID) | QI_EIOTLB_TYPE;	/* [한국어] "이 PASID 전체" 범위와 함께 담는다 */
	desc->qw1 = 0;	/* [한국어] 주소를 쓰지 않으므로 비운다 */
}

/* Page-selective-within-PASID IOTLB invalidation */
/*
 * [한국어] (위 영어 주석에 이어)
 * qi_desc_piotlb - 한 PASID 안에서 특정 주소 범위만 무효화하는 서술자
 *
 * @did: 도메인 id. @pasid: 대상 PASID. @addr: 시작 주소.
 * @size_order: 범위 크기의 로그값. @ih: 중간 단계 항목 유지 힌트.
 * @desc: 채울 서술자.
 * @return: 없음.
 *
 * piotlb_all 의 범위 지정 판(QI_GRAN_PSI_PASID). SVA 에서 프로세스의 일부
 * 매핑만 바뀌었을 때(munmap, 페이지 회수) 그 범위만 지운다.
 *
 * 함수 안 영어 주석이 중요한 전제를 말한다: addr 과 size_order 는 반드시
 * calculate_psi_aligned_address() 로 계산한 값이어야 한다. VT-d 의 범위
 * 무효화는 "주소가 그 크기에 정렬되어 있다"를 전제로 하며, 어긋나면
 * 하드웨어가 의도보다 넓거나 좁은 범위를 지운다. 좁게 지우면 옛 번역이
 * 남아 use-after-free 로 이어지므로 조용한 버그가 된다.
 *
 * 실행 컨텍스트: 무효화 경로.
 */
static inline void qi_desc_piotlb(u16 did, u32 pasid, u64 addr,
				  unsigned int size_order, bool ih,
				  struct qi_desc *desc)
{
	/*
	 * calculate_psi_aligned_address() must be used for addr and size_order
	 */
	desc->qw0 = QI_EIOTLB_PASID(pasid) | QI_EIOTLB_DID(did) |	/* [한국어] PASID 와 도메인 id 를 */
		    QI_EIOTLB_GRAN(QI_GRAN_PSI_PASID) | QI_EIOTLB_TYPE;	/* [한국어] "이 PASID 안의 페이지 선택" 범위와 함께 담는다 */
	desc->qw1 = QI_EIOTLB_ADDR(addr) | QI_EIOTLB_IH(ih) |	/* [한국어] 주소와 힌트를 */
		    QI_EIOTLB_AM(size_order);	/* [한국어] 범위 크기와 함께 담는다. 이 둘은 calculate_psi_aligned_address() 가 계산한 값이어야 한다 (위 영어 주석) */
}

/*
 * [한국어]
 * qi_desc_dev_iotlb_pasid - PASID 를 지정한 디바이스 TLB 무효화 서술자를 조립한다
 *
 * @sid: 장치 소스 id. @pfsid: PF 소스 id. @pasid: 대상 PASID.
 * @qdep: ATS 큐 깊이. @addr: 시작 주소. @size_order: 범위 크기의 로그값.
 * @desc: 채울 서술자.
 * @return: 없음.
 *
 * qi_desc_dev_iotlb 의 PASID 인식 판. SVA 처럼 장치가 여러 주소 공간을 동시에
 * 쓰는 경우, 한 PASID 의 캐시만 지워야 나머지가 살아남는다.
 *
 * 범위 표현이 이 파일에서 가장 까다로운 자리다. S 비트가 0 이면 한 페이지,
 * 1 이면 "주소에서 0 인 최하위 비트가 범위 크기를 나타낸다"는 규칙이다
 * (스펙 6.5.2.6, 위 영어 주석). 예를 들어 비트 12 가 0 이면 8KB, 비트 13 이
 * 0 이면 16KB 다.
 * 그래서 원하는 크기를 표현하려면 세 단계가 필요하다.
 *   1) size_order 아래의 비트를 모두 1 로 채운다. 원래 0 이었던 비트가
 *      "최하위 0" 으로 오인되면 의도보다 좁은 범위가 되기 때문이다.
 *   2) size_order 자리의 비트를 0 으로 만들어 그것이 크기를 나타내게 한다.
 *   3) S 비트를 세워 "한 페이지가 아니다" 를 알린다.
 * 이 순서를 지키지 않으면 무효화 범위가 조용히 어긋난다.
 *
 * 정렬 검사: addr 이 요청한 크기에 정렬되어 있지 않으면 위 규칙으로 그 크기를
 * 표현할 수 없다. 그래서 경고를 남긴다 — 무효화가 조용히 좁아지는 것보다
 * 로그에 남는 편이 낫기 때문이다.
 *
 * MIP(Max Invs Pending)를 0 으로 두는 것은 ECAP 에 DIT 가 들어오기 전까지의
 * 임시 조치다 (위 영어 주석).
 *
 * 실행 컨텍스트: 무효화 경로. 잠들면 안 된다.
 */
static inline void qi_desc_dev_iotlb_pasid(u16 sid, u16 pfsid, u32 pasid,
					   u16 qdep, u64 addr,
					   unsigned int size_order,
					   struct qi_desc *desc)
{
	unsigned long mask = 1UL << (VTD_PAGE_SHIFT + size_order - 1);	/* [한국어] 크기를 나타낼 자리의 비트. 아래에서 이 비트를 0 으로 만들어 범위를 표현한다 */

	desc->qw0 = QI_DEV_EIOTLB_PASID(pasid) | QI_DEV_EIOTLB_SID(sid) |	/* [한국어] PASID 와 장치 소스 id 를 */
		QI_DEV_EIOTLB_QDEP(qdep) | QI_DEIOTLB_TYPE |	/* [한국어] 큐 깊이·명령 종류와 */
		QI_DEV_IOTLB_PFSID(pfsid);	/* [한국어] PF 소스 id 까지 담는다 */

	/*
	 * If S bit is 0, we only flush a single page. If S bit is set,
	 * The least significant zero bit indicates the invalidation address
	 * range. VT-d spec 6.5.2.6.
	 * e.g. address bit 12[0] indicates 8KB, 13[0] indicates 16KB.
	 * size order = 0 is PAGE_SIZE 4KB
	 * Max Invs Pending (MIP) is set to 0 for now until we have DIT in
	 * ECAP.
	 */
	if (!IS_ALIGNED(addr, VTD_PAGE_SIZE << size_order))	/* [한국어] 주소가 요청한 크기에 정렬되어 있지 않으면 */
		pr_warn_ratelimited("Invalidate non-aligned address %llx, order %d\n",	/* [한국어] 아래 규칙으로 그 크기를 표현할 수 없다. 무효화가 조용히 좁아지는 것보다 로그를 남기는 편이 낫다 */
				    addr, size_order);	/* [한국어] 문제의 주소와 크기 */

	/* Take page address */
	desc->qw1 = QI_DEV_EIOTLB_ADDR(addr);	/* [한국어] 먼저 주소를 넣는다 (위 영어 주석) */

	if (size_order) {	/* [한국어] 한 페이지보다 넓으면 */
		/*
		 * Existing 0s in address below size_order may be the least
		 * significant bit, we must set them to 1s to avoid having
		 * smaller size than desired.
		 */
		desc->qw1 |= GENMASK_ULL(size_order + VTD_PAGE_SHIFT - 1,	/* [한국어] size_order 아래의 비트를 모두 1 로 채운다. 원래 0 이던 비트가 "최하위 0"으로 오인되면 의도보다 좁은 범위가 되기 때문이다 (위 영어 주석) */
					VTD_PAGE_SHIFT);	/* [한국어] 페이지 오프셋 자리부터 */
		/* Clear size_order bit to indicate size */
		desc->qw1 &= ~mask;	/* [한국어] size_order 자리를 0 으로 만들어 그것이 크기를 나타내게 한다 (위 영어 주석) */
		/* Set the S bit to indicate flushing more than 1 page */
		desc->qw1 |= QI_DEV_EIOTLB_SIZE;	/* [한국어] S 비트를 세워 한 페이지가 아님을 알린다 (위 영어 주석) */
	}
}

/* Convert value to context PASID directory size field coding. */
#define context_pdts(pds)	(((pds) & 0x7) << 9)	/* [한국어] PASID 디렉터리 크기를 컨텍스트 항목의 필드 자리로 옮긴다 (위 영어 주석). 디렉터리가 몇 개의 PASID 테이블을 담는지를 하드웨어에 알린다 */

struct dmar_drhd_unit *dmar_find_matched_drhd_unit(struct pci_dev *dev);

int dmar_enable_qi(struct intel_iommu *iommu);
void dmar_disable_qi(struct intel_iommu *iommu);
int dmar_reenable_qi(struct intel_iommu *iommu);
void qi_global_iec(struct intel_iommu *iommu);

void qi_flush_context(struct intel_iommu *iommu, u16 did,
		      u16 sid, u8 fm, u64 type);
void qi_flush_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
		    unsigned int size_order, u64 type);
void qi_flush_dev_iotlb(struct intel_iommu *iommu, u16 sid, u16 pfsid,
			u16 qdep, u64 addr, unsigned mask);

void qi_flush_piotlb_all(struct intel_iommu *iommu, u16 did, u32 pasid);

void qi_flush_dev_iotlb_pasid(struct intel_iommu *iommu, u16 sid, u16 pfsid,
			      u32 pasid, u16 qdep, u64 addr,
			      unsigned int size_order);
void quirk_extra_dev_tlb_flush(struct device_domain_info *info,
			       unsigned long address, unsigned long pages,
			       u32 pasid, u16 qdep);
void qi_flush_pasid_cache(struct intel_iommu *iommu, u16 did, u64 granu,
			  u32 pasid);

int qi_submit_sync(struct intel_iommu *iommu, struct qi_desc *desc,
		   unsigned int count, unsigned long options);

void __iommu_flush_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
			 unsigned int size_order, u64 type);
/*
 * Options used in qi_submit_sync:
 * QI_OPT_WAIT_DRAIN - Wait for PRQ drain completion, spec 6.5.2.8.
 */
#define QI_OPT_WAIT_DRAIN		BIT(0)

int domain_attach_iommu(struct dmar_domain *domain, struct intel_iommu *iommu);
void domain_detach_iommu(struct dmar_domain *domain, struct intel_iommu *iommu);
void device_block_translation(struct device *dev);
int paging_domain_compatible(struct iommu_domain *domain, struct device *dev);

struct dev_pasid_info *
domain_add_dev_pasid(struct iommu_domain *domain,
		     struct device *dev, ioasid_t pasid);
void domain_remove_dev_pasid(struct iommu_domain *domain,
			     struct device *dev, ioasid_t pasid);

int __domain_setup_first_level(struct intel_iommu *iommu, struct device *dev,
			       ioasid_t pasid, u16 did, phys_addr_t fsptptr,
			       int flags, struct iommu_domain *old);

int dmar_ir_support(void);

void iommu_flush_write_buffer(struct intel_iommu *iommu);
struct iommu_domain *
intel_iommu_domain_alloc_nested(struct device *dev, struct iommu_domain *parent,
				u32 flags,
				const struct iommu_user_data *user_data);
struct device *device_rbtree_find(struct intel_iommu *iommu, u16 rid);

enum cache_tag_type {
	CACHE_TAG_IOTLB,
	CACHE_TAG_DEVTLB,
	CACHE_TAG_NESTING_IOTLB,
	CACHE_TAG_NESTING_DEVTLB,
};

struct cache_tag {
	struct list_head node;
	enum cache_tag_type type;
	struct intel_iommu *iommu;
	/*
	 * The @dev field represents the location of the cache. For IOTLB, it
	 * resides on the IOMMU hardware. @dev stores the device pointer to
	 * the IOMMU hardware. For DevTLB, it locates in the PCIe endpoint.
	 * @dev stores the device pointer to that endpoint.
	 */
	struct device *dev;
	u16 domain_id;
	ioasid_t pasid;
	unsigned int users;
};

int cache_tag_assign(struct dmar_domain *domain, u16 did, struct device *dev,
		     ioasid_t pasid, enum cache_tag_type type);
int cache_tag_assign_domain(struct dmar_domain *domain,
			    struct device *dev, ioasid_t pasid);
void cache_tag_unassign_domain(struct dmar_domain *domain,
			       struct device *dev, ioasid_t pasid);
void cache_tag_flush_range(struct dmar_domain *domain, unsigned long start,
			   unsigned long end, int ih);
void cache_tag_flush_all(struct dmar_domain *domain);
void cache_tag_flush_range_np(struct dmar_domain *domain, unsigned long start,
			      unsigned long end);

void intel_context_flush_no_pasid(struct device_domain_info *info,
				  struct context_entry *context, u16 did);

int intel_iommu_enable_prq(struct intel_iommu *iommu);
int intel_iommu_finish_prq(struct intel_iommu *iommu);
void intel_iommu_page_response(struct device *dev, struct iopf_fault *evt,
			       struct iommu_page_response *msg);
void intel_iommu_drain_pasid_prq(struct device *dev, u32 pasid);

int intel_iommu_enable_iopf(struct device *dev);
void intel_iommu_disable_iopf(struct device *dev);

static inline int iopf_for_domain_set(struct iommu_domain *domain,
				      struct device *dev)
{
	if (!domain || !domain->iopf_handler)
		return 0;

	return intel_iommu_enable_iopf(dev);
}

static inline void iopf_for_domain_remove(struct iommu_domain *domain,
					  struct device *dev)
{
	if (!domain || !domain->iopf_handler)
		return;

	intel_iommu_disable_iopf(dev);
}

static inline int iopf_for_domain_replace(struct iommu_domain *new,
					  struct iommu_domain *old,
					  struct device *dev)
{
	int ret;

	ret = iopf_for_domain_set(new, dev);
	if (ret)
		return ret;

	iopf_for_domain_remove(old, dev);

	return 0;
}

#ifdef CONFIG_INTEL_IOMMU_SVM	/* [한국어] DMA 재매핑을 뺀 빌드에는 인계 표시 자체가 없다 */
void intel_svm_check(struct intel_iommu *iommu);
struct iommu_domain *intel_svm_domain_alloc(struct device *dev,
					    struct mm_struct *mm);
#else
static inline void intel_svm_check(struct intel_iommu *iommu) {}
static inline struct iommu_domain *intel_svm_domain_alloc(struct device *dev,
							  struct mm_struct *mm)
{
	return ERR_PTR(-ENODEV);
}
#endif

#ifdef CONFIG_INTEL_IOMMU_DEBUGFS
void intel_iommu_debugfs_init(void);
void intel_iommu_debugfs_create_dev(struct device_domain_info *info);
void intel_iommu_debugfs_remove_dev(struct device_domain_info *info);
void intel_iommu_debugfs_create_dev_pasid(struct dev_pasid_info *dev_pasid);
void intel_iommu_debugfs_remove_dev_pasid(struct dev_pasid_info *dev_pasid);
#else
static inline void intel_iommu_debugfs_init(void) {}
static inline void intel_iommu_debugfs_create_dev(struct device_domain_info *info) {}
static inline void intel_iommu_debugfs_remove_dev(struct device_domain_info *info) {}
static inline void intel_iommu_debugfs_create_dev_pasid(struct dev_pasid_info *dev_pasid) {}
static inline void intel_iommu_debugfs_remove_dev_pasid(struct dev_pasid_info *dev_pasid) {}
#endif /* CONFIG_INTEL_IOMMU_DEBUGFS */

extern const struct attribute_group *intel_iommu_groups[];
struct context_entry *iommu_context_addr(struct intel_iommu *iommu, u8 bus,
					 u8 devfn, int alloc);

extern const struct iommu_ops intel_iommu_ops;
extern const struct iommu_domain_ops intel_fs_paging_domain_ops;
extern const struct iommu_domain_ops intel_ss_paging_domain_ops;

static inline bool intel_domain_is_fs_paging(struct dmar_domain *domain)
{
	return domain->domain.ops == &intel_fs_paging_domain_ops;
}

static inline bool intel_domain_is_ss_paging(struct dmar_domain *domain)
{
	return domain->domain.ops == &intel_ss_paging_domain_ops;
}

#ifdef CONFIG_INTEL_IOMMU
extern int intel_iommu_sm;
int iommu_calculate_agaw(struct intel_iommu *iommu);
int iommu_calculate_max_sagaw(struct intel_iommu *iommu);
int ecmd_submit_sync(struct intel_iommu *iommu, u8 ecmd, u64 oa, u64 ob);

static inline bool ecmd_has_pmu_essential(struct intel_iommu *iommu)
{
	return (iommu->ecmdcap[DMA_ECMD_ECCAP3] & DMA_ECMD_ECCAP3_ESSENTIAL) ==
		DMA_ECMD_ECCAP3_ESSENTIAL;
}

extern int dmar_disabled;
extern int intel_iommu_enabled;
#else
static inline int iommu_calculate_agaw(struct intel_iommu *iommu)
{
	return 0;
}
static inline int iommu_calculate_max_sagaw(struct intel_iommu *iommu)
{
	return 0;
}
#define dmar_disabled	(1)
#define intel_iommu_enabled (0)
#define intel_iommu_sm (0)
#endif

static inline const char *decode_prq_descriptor(char *str, size_t size,
		u64 dw0, u64 dw1, u64 dw2, u64 dw3)
{
	char *buf = str;
	int bytes;

	bytes = snprintf(buf, size,
			 "rid=0x%llx addr=0x%llx %c%c%c%c%c pasid=0x%llx index=0x%llx",
			 FIELD_GET(GENMASK_ULL(31, 16), dw0),
			 FIELD_GET(GENMASK_ULL(63, 12), dw1),
			 dw1 & BIT_ULL(0) ? 'r' : '-',
			 dw1 & BIT_ULL(1) ? 'w' : '-',
			 dw0 & BIT_ULL(52) ? 'x' : '-',
			 dw0 & BIT_ULL(53) ? 'p' : '-',
			 dw1 & BIT_ULL(2) ? 'l' : '-',
			 FIELD_GET(GENMASK_ULL(51, 32), dw0),
			 FIELD_GET(GENMASK_ULL(11, 3), dw1));

	/* Private Data */
	if (dw0 & BIT_ULL(9)) {
		size -= bytes;
		buf += bytes;
		snprintf(buf, size, " private=0x%llx/0x%llx\n", dw2, dw3);
	}

	return str;
}

#endif
