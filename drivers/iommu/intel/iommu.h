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
					 DMA_ECMD_ECCAP3_DCNTS |	\
					 DMA_ECMD_ECCAP3_FCNTS |	\
					 DMA_ECMD_ECCAP3_UFCNTS)

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
do {									\
	cycles_t start_time = get_cycles();				\	/* [한국어] 시작 시각. 타임아웃을 재려면 필요하다 */
	while (1) {							\
		sts = op(iommu->reg + offset);				\	/* [한국어] 레지스터를 읽는다. op 는 readl/readq 중 하나로 호출자가 넘긴다 */
		if (cond)						\	/* [한국어] 원하는 조건이 되었으면 */
			break;						\	/* [한국어] 기다림 종료 */
		if (DMAR_OPERATION_TIMEOUT < (get_cycles() - start_time))\	/* [한국어] 정해진 시간을 넘겼으면 */
			panic("DMAR hardware is malfunctioning\n");	\	/* [한국어] 부팅을 멈춘다. IOMMU 가 응답하지 않는데 계속 진행하면 격리 상태를 알 수 없어 더 위험하다 */
		cpu_relax();						\	/* [한국어] 바쁜 대기 중임을 CPU 에 알린다(하이퍼스레드 양보, 전력 절약) */
	}								\
} while (0)

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
	SR_DMAR_FECTL_REG,
	SR_DMAR_FEDATA_REG,
	SR_DMAR_FEADDR_REG,
	SR_DMAR_FEUADDR_REG,
	MAX_SR_DMAR_REGS
};

#define VTD_FLAG_TRANS_PRE_ENABLED	(1 << 0)
#define VTD_FLAG_IRQ_REMAP_PRE_ENABLED	(1 << 1)
#define VTD_FLAG_SVM_CAPABLE		(1 << 2)

#define sm_supported(iommu)	(intel_iommu_sm && ecap_smts((iommu)->ecap))
#define pasid_supported(iommu)	(sm_supported(iommu) &&			\
				 ecap_pasid((iommu)->ecap))
#define ssads_supported(iommu) (sm_supported(iommu) &&                 \
				ecap_slads((iommu)->ecap) &&           \
				ecap_smpwc(iommu->ecap))
#define nested_supported(iommu)	(sm_supported(iommu) &&			\
				 ecap_nest((iommu)->ecap))

struct pasid_entry;
struct pasid_state_entry;
struct page_req_dsc;

/*
 * 0: Present
 * 1-11: Reserved
 * 12-63: Context Ptr (12 - (haw-1))
 * 64-127: Reserved
 */
struct root_entry {
	u64     lo;
	u64     hi;
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
	u64 hi;
};

struct iommu_domain_info {
	struct intel_iommu *iommu;
	unsigned int refcnt;		/* Refcount of devices per iommu */
	u16 did;			/* Domain ids per IOMMU. Use u16 since
					 * domain ids are 16 bit wide according
					 * to VT-d spec, section 9.3 */
};

/*
 * We start simply by using a fixed size for the batched descriptors. This
 * size is currently sufficient for our needs. Future improvements could
 * involve dynamically allocating the batch buffer based on actual demand,
 * allowing us to adjust the batch size for optimal performance in different
 * scenarios.
 */
#define QI_MAX_BATCHED_DESC_COUNT 16
struct qi_batch {
	struct qi_desc descs[QI_MAX_BATCHED_DESC_COUNT];
	unsigned int index;
};

struct dmar_domain {
	union {
		struct iommu_domain domain;
		struct pt_iommu iommu;
		/* First stage page table */
		struct pt_iommu_x86_64 fspt;
		/* Second stage page table */
		struct pt_iommu_vtdss sspt;
	};

	struct xarray iommu_array;	/* Attached IOMMU array */

	u8 force_snooping:1;		/* Create PASID entry with snoop control */
	u8 dirty_tracking:1;		/* Dirty tracking is enabled */
	u8 nested_parent:1;		/* Has other domains nested on it */
	u8 iotlb_sync_map:1;		/* Need to flush IOTLB cache or write
					 * buffer when creating mappings.
					 */

	spinlock_t lock;		/* Protect device tracking lists */
	struct list_head devices;	/* all devices' list */
	struct list_head dev_pasids;	/* all attached pasids */

	spinlock_t cache_lock;		/* Protect the cache tag list */
	struct list_head cache_tags;	/* Cache tag list */
	struct qi_batch *qi_batch;	/* Batched QI descriptors */

	union {
		/* DMA remapping domain */
		struct {
			/* Protect the s1_domains list */
			spinlock_t	s1_lock;
			/* Track s1_domains nested on this domain */
			struct list_head s1_domains;
		};

		/* Nested user domain */
		struct {
			/* parent page table which the user domain is nested on */
			struct dmar_domain *s2_domain;
			/* page table attributes */
			struct iommu_hwpt_vtd_s1 s1_cfg;
			/* link to parent domain siblings */
			struct list_head s2_link;
		};

		/* SVA domain */
		struct {
			struct mmu_notifier notifier;
		};
	};
};
PT_IOMMU_CHECK_DOMAIN(struct dmar_domain, iommu, domain);
PT_IOMMU_CHECK_DOMAIN(struct dmar_domain, sspt.iommu, domain);
PT_IOMMU_CHECK_DOMAIN(struct dmar_domain, fspt.iommu, domain);

/*
 * In theory, the VT-d 4.0 spec can support up to 2 ^ 16 counters.
 * But in practice, there are only 14 counters for the existing
 * platform. Setting the max number of counters to 64 should be good
 * enough for a long time. Also, supporting more than 64 counters
 * requires more extras, e.g., extra freeze and overflow registers,
 * which is not necessary for now.
 */
#define IOMMU_PMU_IDX_MAX		64

struct iommu_pmu {
	struct intel_iommu	*iommu;
	u32			num_cntr;	/* Number of counters */
	u32			num_eg;		/* Number of event group */
	u32			cntr_width;	/* Counter width */
	u32			cntr_stride;	/* Counter Stride */
	u32			filter;		/* Bitmask of filter support */
	void __iomem		*base;		/* the PerfMon base address */
	void __iomem		*cfg_reg;	/* counter configuration base address */
	void __iomem		*cntr_reg;	/* counter 0 address*/
	void __iomem		*overflow;	/* overflow status register */

	u64			*evcap;		/* Indicates all supported events */
	u32			**cntr_evcap;	/* Supported events of each counter. */

	struct pmu		pmu;
	DECLARE_BITMAP(used_mask, IOMMU_PMU_IDX_MAX);
	struct perf_event	*event_list[IOMMU_PMU_IDX_MAX];
	unsigned char		irq_name[16];
};

#define IOMMU_IRQ_ID_OFFSET_PRQ		(DMAR_UNITS_SUPPORTED)
#define IOMMU_IRQ_ID_OFFSET_PERF	(2 * DMAR_UNITS_SUPPORTED)

struct intel_iommu {
	void __iomem	*reg; /* Pointer to hardware regs, virtual addr */
	u64 		reg_phys; /* physical address of hw register set */
	u64		reg_size; /* size of hw register set */
	u64		cap;
	u64		ecap;
	u64		vccap;
	u64		ecmdcap[DMA_MAX_NUM_ECMDCAP];
	u32		gcmd; /* Holds TE, EAFL. Don't need SRTP, SFL, WBF */
	raw_spinlock_t	register_lock; /* protect register handling */
	int		seq_id;	/* sequence id of the iommu */
	int		agaw; /* agaw of this iommu */
	int		msagaw; /* max sagaw of this iommu */
	unsigned int	irq, pr_irq, perf_irq;
	u16		segment;     /* PCI segment# */
	unsigned char	name[16];    /* Device Name */

#ifdef CONFIG_INTEL_IOMMU
	/* mutex to protect domain_ida */
	struct mutex	did_lock;
	struct ida	domain_ida; /* domain id allocator */
	unsigned long	*copied_tables; /* bitmap of copied tables */
	spinlock_t	lock; /* protect context, domain ids */
	struct root_entry *root_entry; /* virtual address */

	struct iommu_flush flush;
#endif
	struct page_req_dsc *prq;
	unsigned char prq_name[16];    /* Name for PRQ interrupt */
	unsigned long prq_seq_number;
	struct completion prq_complete;
	struct iopf_queue *iopf_queue;
	unsigned char iopfq_name[16];
	/* Synchronization between fault report and iommu device release. */
	struct mutex iopf_lock;
	struct q_inval  *qi;            /* Queued invalidation info */
	u32 iommu_state[MAX_SR_DMAR_REGS]; /* Store iommu states between suspend and resume.*/

	/* rb tree for all probed devices */
	struct rb_root device_rbtree;
	/* protect the device_rbtree */
	spinlock_t device_rbtree_lock;

#ifdef CONFIG_IRQ_REMAP
	struct ir_table *ir_table;	/* Interrupt remapping info */
	struct irq_domain *ir_domain;
#endif
	struct iommu_device iommu;  /* IOMMU core code handle */
	int		node;
	u32		flags;      /* Software defined flags */

	struct dmar_drhd_unit *drhd;
	void *perf_statistic;

	struct iommu_pmu *pmu;
};

/* PCI domain-device relationship */
struct device_domain_info {
	struct list_head link;	/* link to domain siblings */
	u32 segment;		/* PCI segment number */
	u8 bus;			/* PCI bus number */
	u8 devfn;		/* PCI devfn number */
	u16 pfsid;		/* SRIOV physical function source ID */
	u8 pasid_supported:3;
	u8 pasid_enabled:1;
	u8 pri_supported:1;
	u8 pri_enabled:1;
	u8 ats_supported:1;
	u8 ats_enabled:1;
	u8 dtlb_extra_inval:1;	/* Quirk for devices need extra flush */
	u8 domain_attached:1;	/* Device has domain attached */
	u8 ats_qdep;
	unsigned int iopf_refcount;
	struct device *dev; /* it's NULL for PCIe-to-PCI bridge */
	struct intel_iommu *iommu; /* IOMMU used by this device */
	struct dmar_domain *domain; /* pointer to domain */
	struct pasid_table *pasid_table; /* pasid table */
	/* device tracking node(lookup by PCI RID) */
	struct rb_node node;
#ifdef CONFIG_INTEL_IOMMU_DEBUGFS
	struct dentry *debugfs_dentry; /* pointer to device directory dentry */
#endif
};

struct dev_pasid_info {
	struct list_head link_domain;	/* link to domain siblings */
	struct device *dev;
	ioasid_t pasid;
#ifdef CONFIG_INTEL_IOMMU_DEBUGFS
	struct dentry *debugfs_dentry; /* pointer to pasid directory dentry */
#endif
};

static inline void __iommu_flush_cache(
	struct intel_iommu *iommu, void *addr, int size)
{
	if (!ecap_coherent(iommu->ecap))
		clflush_cache_range(addr, size);
}

/* Convert generic struct iommu_domain to private struct dmar_domain */
static inline struct dmar_domain *to_dmar_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct dmar_domain, domain);
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
#define FLPT_DEFAULT_DID		1
#define IDA_START_DID			2

/* Retrieve the domain ID which has allocated to the domain */
static inline u16
domain_id_iommu(struct dmar_domain *domain, struct intel_iommu *iommu)
{
	struct iommu_domain_info *info =
			xa_load(&domain->iommu_array, iommu->seq_id);

	return info->did;
}

static inline u16
iommu_domain_did(struct iommu_domain *domain, struct intel_iommu *iommu)
{
	if (domain->type == IOMMU_DOMAIN_SVA ||
	    domain->type == IOMMU_DOMAIN_IDENTITY)
		return FLPT_DEFAULT_DID;
	return domain_id_iommu(to_dmar_domain(domain), iommu);
}

static inline bool dev_is_real_dma_subdevice(struct device *dev)
{
	return dev && dev_is_pci(dev) &&
	       pci_real_dma_dev(to_pci_dev(dev)) != to_pci_dev(dev);
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
};

static inline u64 dma_pte_addr(struct dma_pte *pte)
{
#ifdef CONFIG_64BIT
	return pte->val & VTD_PAGE_MASK;
#else
	/* Must have a full atomic 64-bit read */
	return  __cmpxchg64(&pte->val, 0ULL, 0ULL) & VTD_PAGE_MASK;
#endif
}

static inline bool dma_pte_present(struct dma_pte *pte)
{
	return (pte->val & 3) != 0;
}

static inline bool dma_pte_superpage(struct dma_pte *pte)
{
	return (pte->val & DMA_PTE_LARGE_PAGE);
}

static inline bool context_present(struct context_entry *context)
{
	return (context->lo & 1);
}

#define LEVEL_STRIDE		(9)
#define LEVEL_MASK		(((u64)1 << LEVEL_STRIDE) - 1)
#define MAX_AGAW_WIDTH		(64)
#define MAX_AGAW_PFN_WIDTH	(MAX_AGAW_WIDTH - VTD_PAGE_SHIFT)

static inline int agaw_to_level(int agaw)
{
	return agaw + 2;
}

static inline int width_to_agaw(int width)
{
	return DIV_ROUND_UP(width - 30, LEVEL_STRIDE);
}

static inline unsigned int level_to_offset_bits(int level)
{
	return (level - 1) * LEVEL_STRIDE;
}

static inline int pfn_level_offset(u64 pfn, int level)
{
	return (pfn >> level_to_offset_bits(level)) & LEVEL_MASK;
}


static inline void context_set_present(struct context_entry *context)
{
	u64 val;

	dma_wmb();
	val = READ_ONCE(context->lo) | 1;
	WRITE_ONCE(context->lo, val);
}

/*
 * Clear the Present (P) bit (bit 0) of a context table entry. This initiates
 * the transition of the entry's ownership from hardware to software. The
 * caller is responsible for fulfilling the invalidation handshake recommended
 * by the VT-d spec, Section 6.5.3.3 (Guidance to Software for Invalidations).
 */
static inline void context_clear_present(struct context_entry *context)
{
	u64 val;

	val = READ_ONCE(context->lo) & GENMASK_ULL(63, 1);
	WRITE_ONCE(context->lo, val);
	dma_wmb();
}

static inline void context_set_fault_enable(struct context_entry *context)
{
	context->lo &= (((u64)-1) << 2) | 1;
}

static inline void context_set_translation_type(struct context_entry *context,
						unsigned long value)
{
	context->lo &= (((u64)-1) << 4) | 3;
	context->lo |= (value & 3) << 2;
}

static inline void context_set_address_root(struct context_entry *context,
					    unsigned long value)
{
	context->lo &= ~VTD_PAGE_MASK;
	context->lo |= value & VTD_PAGE_MASK;
}

static inline void context_set_address_width(struct context_entry *context,
					     unsigned long value)
{
	context->hi |= value & 7;
}

static inline void context_set_domain_id(struct context_entry *context,
					 unsigned long value)
{
	context->hi |= (value & ((1 << 16) - 1)) << 8;
}

static inline void context_set_pasid(struct context_entry *context)
{
	context->lo |= CONTEXT_PASIDE;
}

static inline int context_domain_id(struct context_entry *c)
{
	return((c->hi >> 8) & 0xffff);
}

static inline void context_clear_entry(struct context_entry *context)
{
	context->lo = 0;
	context->hi = 0;
}

#ifdef CONFIG_INTEL_IOMMU
static inline bool context_copied(struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	if (!iommu->copied_tables)
		return false;

	return test_bit(((long)bus << 8) | devfn, iommu->copied_tables);
}

static inline void
set_context_copied(struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	set_bit(((long)bus << 8) | devfn, iommu->copied_tables);
}

static inline void
clear_context_copied(struct intel_iommu *iommu, u8 bus, u8 devfn)
{
	clear_bit(((long)bus << 8) | devfn, iommu->copied_tables);
}
#endif /* CONFIG_INTEL_IOMMU */

/*
 * Set the RID_PASID field of a scalable mode context entry. The
 * IOMMU hardware will use the PASID value set in this field for
 * DMA translations of DMA requests without PASID.
 */
static inline void
context_set_sm_rid2pasid(struct context_entry *context, unsigned long pasid)
{
	context->hi |= pasid & ((1 << 20) - 1);
}

/*
 * Set the DTE(Device-TLB Enable) field of a scalable mode context
 * entry.
 */
static inline void context_set_sm_dte(struct context_entry *context)
{
	context->lo |= BIT_ULL(2);
}

/*
 * Set the PRE(Page Request Enable) field of a scalable mode context
 * entry.
 */
static inline void context_set_sm_pre(struct context_entry *context)
{
	context->lo |= BIT_ULL(4);
}

/*
 * Clear the PRE(Page Request Enable) field of a scalable mode context
 * entry.
 */
static inline void context_clear_sm_pre(struct context_entry *context)
{
	context->lo &= ~BIT_ULL(4);
}

/* Returns a number of VTD pages, but aligned to MM page size */
static inline unsigned long aligned_nrpages(unsigned long host_addr, size_t size)
{
	host_addr &= ~PAGE_MASK;
	return PAGE_ALIGN(host_addr + size) >> VTD_PAGE_SHIFT;
}

/* Return a size from number of VTD pages. */
static inline unsigned long nrpages_to_size(unsigned long npages)
{
	return npages << VTD_PAGE_SHIFT;
}

static inline void qi_desc_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
				 unsigned int size_order, u64 type,
				 struct qi_desc *desc)
{
	u8 dw = 0, dr = 0;
	int ih = addr & 1;

	if (cap_write_drain(iommu->cap))
		dw = 1;

	if (cap_read_drain(iommu->cap))
		dr = 1;

	desc->qw0 = QI_IOTLB_DID(did) | QI_IOTLB_DR(dr) | QI_IOTLB_DW(dw)
		| QI_IOTLB_GRAN(type) | QI_IOTLB_TYPE;
	desc->qw1 = QI_IOTLB_ADDR(addr) | QI_IOTLB_IH(ih)
		| QI_IOTLB_AM(size_order);
	desc->qw2 = 0;
	desc->qw3 = 0;
}

static inline void qi_desc_dev_iotlb(u16 sid, u16 pfsid, u16 qdep, u64 addr,
				     unsigned int mask, struct qi_desc *desc)
{
	if (mask) {
		addr |= (1ULL << (VTD_PAGE_SHIFT + mask - 1)) - 1;
		desc->qw1 = QI_DEV_IOTLB_ADDR(addr) | QI_DEV_IOTLB_SIZE;
	} else {
		desc->qw1 = QI_DEV_IOTLB_ADDR(addr);
	}

	if (qdep >= QI_DEV_IOTLB_MAX_INVS)
		qdep = 0;

	desc->qw0 = QI_DEV_IOTLB_SID(sid) | QI_DEV_IOTLB_QDEP(qdep) |
		   QI_DIOTLB_TYPE | QI_DEV_IOTLB_PFSID(pfsid);
	desc->qw2 = 0;
	desc->qw3 = 0;
}

/* PASID-selective IOTLB invalidation */
static inline void qi_desc_piotlb_all(u16 did, u32 pasid, struct qi_desc *desc)
{
	desc->qw0 = QI_EIOTLB_PASID(pasid) | QI_EIOTLB_DID(did) |
		    QI_EIOTLB_GRAN(QI_GRAN_NONG_PASID) | QI_EIOTLB_TYPE;
	desc->qw1 = 0;
}

/* Page-selective-within-PASID IOTLB invalidation */
static inline void qi_desc_piotlb(u16 did, u32 pasid, u64 addr,
				  unsigned int size_order, bool ih,
				  struct qi_desc *desc)
{
	/*
	 * calculate_psi_aligned_address() must be used for addr and size_order
	 */
	desc->qw0 = QI_EIOTLB_PASID(pasid) | QI_EIOTLB_DID(did) |
		    QI_EIOTLB_GRAN(QI_GRAN_PSI_PASID) | QI_EIOTLB_TYPE;
	desc->qw1 = QI_EIOTLB_ADDR(addr) | QI_EIOTLB_IH(ih) |
		    QI_EIOTLB_AM(size_order);
}

static inline void qi_desc_dev_iotlb_pasid(u16 sid, u16 pfsid, u32 pasid,
					   u16 qdep, u64 addr,
					   unsigned int size_order,
					   struct qi_desc *desc)
{
	unsigned long mask = 1UL << (VTD_PAGE_SHIFT + size_order - 1);

	desc->qw0 = QI_DEV_EIOTLB_PASID(pasid) | QI_DEV_EIOTLB_SID(sid) |
		QI_DEV_EIOTLB_QDEP(qdep) | QI_DEIOTLB_TYPE |
		QI_DEV_IOTLB_PFSID(pfsid);

	/*
	 * If S bit is 0, we only flush a single page. If S bit is set,
	 * The least significant zero bit indicates the invalidation address
	 * range. VT-d spec 6.5.2.6.
	 * e.g. address bit 12[0] indicates 8KB, 13[0] indicates 16KB.
	 * size order = 0 is PAGE_SIZE 4KB
	 * Max Invs Pending (MIP) is set to 0 for now until we have DIT in
	 * ECAP.
	 */
	if (!IS_ALIGNED(addr, VTD_PAGE_SIZE << size_order))
		pr_warn_ratelimited("Invalidate non-aligned address %llx, order %d\n",
				    addr, size_order);

	/* Take page address */
	desc->qw1 = QI_DEV_EIOTLB_ADDR(addr);

	if (size_order) {
		/*
		 * Existing 0s in address below size_order may be the least
		 * significant bit, we must set them to 1s to avoid having
		 * smaller size than desired.
		 */
		desc->qw1 |= GENMASK_ULL(size_order + VTD_PAGE_SHIFT - 1,
					VTD_PAGE_SHIFT);
		/* Clear size_order bit to indicate size */
		desc->qw1 &= ~mask;
		/* Set the S bit to indicate flushing more than 1 page */
		desc->qw1 |= QI_DEV_EIOTLB_SIZE;
	}
}

/* Convert value to context PASID directory size field coding. */
#define context_pdts(pds)	(((pds) & 0x7) << 9)

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

#ifdef CONFIG_INTEL_IOMMU_SVM
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
