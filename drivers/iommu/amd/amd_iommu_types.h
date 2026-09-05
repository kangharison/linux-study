/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2007-2010 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 *         Leo Duran <leo.duran@amd.com>
 */

#ifndef _ASM_X86_AMD_IOMMU_TYPES_H	/* [한국어] 헤더 중복 포함 방지. 아래 정의가 한 번만 보이게 한다 */
#define _ASM_X86_AMD_IOMMU_TYPES_H

/*
 * [한국어 설명] AMD-Vi IOMMU 의 하드웨어 정의와 드라이버 자료구조 전부 (amd_iommu_types.h)
 *
 * === 파일의 역할 ===
 * AMD IOMMU 드라이버가 다루는 모든 것의 사전이다. MMIO 레지스터 오프셋,
 * 확장 기능 비트, 명령·이벤트·PPR·GA 로그의 서술자 형식, 장치 테이블 항목
 * (DTE)의 비트 배치, 페이지 테이블 항목의 인코딩, 그리고 드라이버가 들고
 * 다니는 struct amd_iommu / protection_domain / iommu_dev_data 가 여기 모여
 * 있다.
 *
 * 이 파일 하나에 하드웨어 정의와 소프트웨어 자료구조가 함께 있는 것이
 * Intel 쪽(iommu.h 와 pasid.h 로 나뉜 것)과 다른 점이다. AMD 는 장치 테이블
 * 항목 하나가 변환 정보와 인터럽트 재매핑 정보를 모두 담기 때문에, 그 둘을
 * 나눌 자연스러운 경계가 없다.
 *
 * 값이 대부분 비트 위치와 마스크인 이유는 AMD IOMMU 스펙(AMD I/O
 * Virtualization Technology Specification)이 자료구조를 비트 단위로 정의하기
 * 때문이다. 구조체로 표현하면 컴파일러의 비트필드 배치에 의존하게 되므로,
 * u64 배열 + 시프트/마스크로 다룬다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * AMD IOMMU 드라이버의 최하층 정의다. init.c 가 이 정의로 하드웨어를 찾아
 * 초기화하고, iommu.c 가 이 구조체들로 도메인과 장치를 관리하며, ppr.c 가
 * 페이지 요청 로그 형식을, 인터럽트 재매핑 경로가 IRTE 형식을 여기서
 * 가져다 쓴다.
 *
 * 실행 컨텍스트는 없다 — 정의만 있는 헤더다. 다만 여기 정의된 구조체의
 * 락(protection_domain.lock, amd_iommu.lock 등)이 드라이버 전체의 동시성
 * 규칙을 정한다.
 *
 * === 타 모듈과의 연결 ===
 * 위로는 linux/iommu.h 의 struct iommu_domain / iommu_device 를 품어 코어
 * IOMMU 계층에 연결되고, linux/iommufd.h 로 사용자 공간 IOMMU 인터페이스에,
 * linux/generic_pt/iommu.h 로 공용 페이지 테이블 구현에 이어진다.
 *
 * 데이터 흐름: ACPI IVRS 표 → init.c 가 여기 정의된 형식으로 파싱 →
 * struct amd_iommu 배열 → 장치 테이블(DTE) → 페이지 테이블. 반대 방향으로는
 * 하드웨어가 쓴 이벤트/PPR/GA 로그가 여기 정의된 형식으로 읽힌다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct amd_iommu: IOMMU 하드웨어 유닛 하나. MMIO 기준 주소, 명령 버퍼,
 *   이벤트 로그, 장치 테이블 범위, 능력 비트를 모두 들고 있다.
 * - struct protection_domain: 하나의 주소 공간. 페이지 테이블과 그것을
 *   쓰는 장치 목록을 잇는다.
 * - struct iommu_dev_data: 장치 하나의 IOMMU 쪽 상태. 어느 도메인에
 *   속하는지, PASID/PRI/ATS 를 쓰는지.
 * - struct dev_table_entry: 장치 테이블 항목. 하드웨어가 요청자 id 로
 *   찾아오는 최초의 자료구조다.
 * - MMIO_*_OFFSET / FEATURE_*: 레지스터 위치와 능력 비트.
 * - DTE_* / IOMMU_PTE_*: 장치 테이블 항목과 페이지 테이블 항목의 비트 배치.
 */
#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP — 이 파일의 정의가 대부분 비트 필드다 */
#include <linux/iommu.h>	/* [한국어] 코어 IOMMU 계층의 struct iommu_domain/iommu_device */
#include <linux/types.h>	/* [한국어] u8/u16/u64 등 고정 폭 타입 */
#include <linux/mmu_notifier.h>	/* [한국어] SVA 에서 프로세스 페이지 테이블 변경을 통지받는다 */
#include <linux/mutex.h>	/* [한국어] 도메인·장치 목록을 지키는 뮤텍스 */
#include <linux/msi.h>	/* [한국어] 인터럽트 재매핑이 MSI 메시지를 다룬다 */
#include <linux/list.h>	/* [한국어] 도메인의 장치 목록 등 연결 리스트 */
#include <linux/spinlock.h>	/* [한국어] 핫패스에서 쓰는 스핀락 */
#include <linux/pci.h>	/* [한국어] 요청자 id, ATS/PRI 능력 */
#include <linux/iommufd.h>	/* [한국어] 사용자 공간에 IOMMU 를 넘기는 인터페이스 */
#include <linux/irqreturn.h>	/* [한국어] 인터럽트 핸들러의 반환 타입 */
#include <linux/generic_pt/iommu.h>	/* [한국어] 공용 페이지 테이블 구현 — AMD v1 형식이 그 위에 얹힌다 */

#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 구조체 정의 */

/*
 * Maximum number of IOMMUs supported
 */
#define MAX_IOMMUS	32	/* [한국어] 한 시스템에서 다룰 IOMMU 유닛의 상한. 배열 크기를 고정하기 위한 값이다 */

/*
 * some size calculation constants
 */
#define DEV_TABLE_ENTRY_SIZE		32	/* [한국어] 장치 테이블 항목 하나가 32바이트. 테이블 크기를 장치 수로부터 계산할 때 쓴다 */

/* Capability offsets used by the driver */
#define MMIO_CAP_HDR_OFFSET	0x00	/* [한국어] PCI 능력 구조의 헤더. IOMMU 를 PCI 장치로 발견할 때 읽는다 */
#define MMIO_RANGE_OFFSET	0x0c	/* [한국어] 이 유닛이 담당하는 장치 범위(버스/devfn)를 담은 능력 필드 */
#define MMIO_MISC_OFFSET	0x10	/* [한국어] MSI 개수 등 기타 정보 */

/* Masks, shifts and macros to parse the device range capability */
#define MMIO_RANGE_LD_MASK	0xff000000	/* [한국어] 담당 범위의 마지막 장치(last device) */
#define MMIO_RANGE_FD_MASK	0x00ff0000	/* [한국어] 첫 장치(first device) */
#define MMIO_RANGE_BUS_MASK	0x0000ff00	/* [한국어] 그 장치들이 있는 버스 번호 */
#define MMIO_RANGE_LD_SHIFT	24	/* [한국어] 위 마스크에 대응하는 시프트 */
#define MMIO_RANGE_FD_SHIFT	16	/* [한국어] 같은 목적 */
#define MMIO_RANGE_BUS_SHIFT	8	/* [한국어] 같은 목적 */
#define MMIO_GET_LD(x)  (((x) & MMIO_RANGE_LD_MASK) >> MMIO_RANGE_LD_SHIFT)	/* [한국어] 능력 값에서 마지막 장치 번호를 꺼낸다 */
#define MMIO_GET_FD(x)  (((x) & MMIO_RANGE_FD_MASK) >> MMIO_RANGE_FD_SHIFT)	/* [한국어] 첫 장치 번호를 꺼낸다 */
#define MMIO_GET_BUS(x) (((x) & MMIO_RANGE_BUS_MASK) >> MMIO_RANGE_BUS_SHIFT)	/* [한국어] 버스 번호를 꺼낸다 */
#define MMIO_MSI_NUM(x)	((x) & 0x1f)	/* [한국어] 이 유닛이 쓸 MSI 벡터 번호 */

/* Flag masks for the AMD IOMMU exclusion range */
#define MMIO_EXCL_ENABLE_MASK 0x01ULL	/* [한국어] 제외 범위(exclusion range) 활성화 비트. 그 범위의 DMA 는 변환을 거치지 않는다 */
#define MMIO_EXCL_ALLOW_MASK  0x02ULL	/* [한국어] 제외 범위를 모든 장치에 허용할지. 펌웨어가 예약한 영역을 다룰 때 쓴다 */

/* Used offsets into the MMIO space */
#define MMIO_DEV_TABLE_OFFSET   0x0000	/* [한국어] 장치 테이블의 물리 주소를 쓰는 레지스터. 변환 사슬의 출발점이다 */
#define MMIO_CMD_BUF_OFFSET     0x0008	/* [한국어] 명령 버퍼의 주소. 무효화 명령을 여기 넣는다 */
#define MMIO_EVT_BUF_OFFSET     0x0010	/* [한국어] 이벤트 로그의 주소. 하드웨어가 오류를 여기 쓴다 */
#define MMIO_CONTROL_OFFSET     0x0018	/* [한국어] 제어 레지스터. IOMMU 를 켜고 기능을 활성화한다 */
#define MMIO_EXCL_BASE_OFFSET   0x0020	/* [한국어] 제외 범위의 시작 주소 */
#define MMIO_EXCL_LIMIT_OFFSET  0x0028	/* [한국어] 제외 범위의 끝 */
#define MMIO_EXT_FEATURES	0x0030	/* [한국어] 확장 기능 비트맵. 이 유닛이 무엇을 지원하는지 전부 여기 있다 */
#define MMIO_PPR_LOG_OFFSET	0x0038	/* [한국어] PPR(Peripheral Page Request) 로그의 주소. 장치의 페이지 폴트가 여기 쌓인다 */
#define MMIO_GA_LOG_BASE_OFFSET	0x00e0	/* [한국어] GA(Guest Avic) 로그의 주소. 게스트 인터럽트 전달 실패가 기록된다 */
#define MMIO_GA_LOG_TAIL_OFFSET	0x00e8	/* [한국어] 그 로그의 꼬리 포인터 */
#define MMIO_MSI_ADDR_LO_OFFSET	0x015C	/* [한국어] IOMMU 자신의 인터럽트를 위한 MSI 주소 하위 */
#define MMIO_MSI_ADDR_HI_OFFSET	0x0160	/* [한국어] 같은 주소의 상위 */
#define MMIO_MSI_DATA_OFFSET	0x0164	/* [한국어] 그 MSI 의 데이터(벡터) */
#define MMIO_INTCAPXT_EVT_OFFSET	0x0170	/* [한국어] x2APIC 모드에서 이벤트 로그 인터럽트를 설정하는 레지스터 */
#define MMIO_INTCAPXT_PPR_OFFSET	0x0178	/* [한국어] 같은 방식의 PPR 로그 인터럽트 */
#define MMIO_INTCAPXT_GALOG_OFFSET	0x0180	/* [한국어] 같은 방식의 GA 로그 인터럽트 */
#define MMIO_EXT_FEATURES2	0x01A0	/* [한국어] 두 번째 확장 기능 워드. 비트가 모자라 늘어난 것이다 */
#define MMIO_CMD_HEAD_OFFSET	0x2000	/* [한국어] 명령 버퍼의 머리 — 하드웨어가 처리한 지점 */
#define MMIO_CMD_TAIL_OFFSET	0x2008	/* [한국어] 꼬리 — 드라이버가 넣은 지점. 둘의 차이가 밀린 명령 수다 */
#define MMIO_EVT_HEAD_OFFSET	0x2010	/* [한국어] 이벤트 로그의 머리 — 드라이버가 읽은 지점 */
#define MMIO_EVT_TAIL_OFFSET	0x2018	/* [한국어] 꼬리 — 하드웨어가 쓴 지점 */
#define MMIO_STATUS_OFFSET	0x2020	/* [한국어] 상태 레지스터. 로그 오버플로 등 사건 비트가 선다 */
#define MMIO_PPR_HEAD_OFFSET	0x2030	/* [한국어] PPR 로그의 머리 */
#define MMIO_PPR_TAIL_OFFSET	0x2038	/* [한국어] PPR 로그의 꼬리 */
#define MMIO_GA_HEAD_OFFSET	0x2040	/* [한국어] GA 로그의 머리 */
#define MMIO_GA_TAIL_OFFSET	0x2048	/* [한국어] GA 로그의 꼬리 */
#define MMIO_CNTR_CONF_OFFSET	0x4000	/* [한국어] 성능 카운터 설정 영역의 시작 */
#define MMIO_CNTR_REG_OFFSET	0x40000	/* [한국어] 성능 카운터 값 영역의 시작 */
#define MMIO_REG_END_OFFSET	0x80000	/* [한국어] MMIO 영역 전체의 끝. 매핑할 크기를 정하는 데 쓴다 */



/* Extended Feature Bits */
#define FEATURE_PREFETCH	BIT_ULL(0)	/* [한국어] 페이지 테이블 프리페치 지원 */
#define FEATURE_PPR		BIT_ULL(1)	/* [한국어] PPR — 장치가 페이지 폴트를 보고할 수 있다. SVA 의 전제 조건이다 */
#define FEATURE_X2APIC		BIT_ULL(2)	/* [한국어] x2APIC 모드의 인터럽트 재매핑 지원 */
#define FEATURE_NX		BIT_ULL(3)	/* [한국어] 페이지 테이블의 실행 금지 비트 지원 */
#define FEATURE_GT		BIT_ULL(4)	/* [한국어] 게스트 변환(Guest Translation) — PASID 별 2단계 변환 */
#define FEATURE_IA		BIT_ULL(6)	/* [한국어] IOTLB 전체 무효화 명령 지원 */
#define FEATURE_GA		BIT_ULL(7)	/* [한국어] 게스트 APIC — 인터럽트를 게스트에 직접 전달 */
#define FEATURE_HE		BIT_ULL(8)	/* [한국어] 하드웨어 오류 보고 레지스터 지원 */
#define FEATURE_PC		BIT_ULL(9)	/* [한국어] 성능 카운터 지원 */
#define FEATURE_HATS		GENMASK_ULL(11, 10)	/* [한국어] 호스트 주소 변환 크기(Host Address Translation Size). 주소 폭을 결정한다 */
#define FEATURE_GATS		GENMASK_ULL(13, 12)	/* [한국어] 게스트 주소 변환 크기 */
#define FEATURE_GLX		GENMASK_ULL(15, 14)	/* [한국어] 게스트 CR3 테이블의 레벨 수. PASID 개수의 상한과 연결된다 */
#define FEATURE_GAM_VAPIC	BIT_ULL(21)	/* [한국어] 게스트 가상 APIC 모드 — AVIC 의 전제 */
#define FEATURE_PASMAX		GENMASK_ULL(36, 32)	/* [한국어] 지원하는 PASID 의 최대 비트 수 */
#define FEATURE_GIOSUP		BIT_ULL(48)	/* [한국어] 게스트 I/O 프로텍션 지원 */
#define FEATURE_HASUP		BIT_ULL(49)	/* [한국어] 호스트 접근(Access) 비트 지원 */
#define FEATURE_EPHSUP		BIT_ULL(50)	/* [한국어] 향상된 PPR 처리 지원 */
#define FEATURE_HDSUP		BIT_ULL(52)	/* [한국어] 호스트 더티(Dirty) 비트 지원 — 마이그레이션의 더티 추적에 쓴다 */
#define FEATURE_SNP		BIT_ULL(63)	/* [한국어] SEV-SNP 보안 중첩 페이징 지원 */


/* Extended Feature 2 Bits */
#define FEATURE_SEVSNPIO_SUP	BIT_ULL(1)	/* [한국어] SEV-SNP 환경에서의 I/O 지원 */
#define FEATURE_GCR3TRPMODE	BIT_ULL(3)	/* [한국어] 게스트 CR3 테이블 트랩 모드 */
#define FEATURE_SNPAVICSUP	GENMASK_ULL(7, 5)	/* [한국어] SNP 환경에서의 AVIC 지원 방식 */
/* [한국어] SNPAVICSUP 필드가 "게스트 APIC 모드 가능"(값 1)인지 판별한다.
 * 필드가 3비트라 여러 값을 가질 수 있고, 그중 하나만 우리가 쓸 수 있다. */
#define FEATURE_SNPAVICSUP_GAM(x) \
	(FIELD_GET(FEATURE_SNPAVICSUP, x) == 0x1)	/* [한국어] 필드 값이 1 이어야 게스트 APIC 모드를 쓸 수 있다 */
#define FEATURE_HT_RANGE_IGNORE		BIT_ULL(11)	/* [한국어] HyperTransport 주소 범위를 변환에서 제외하지 않는다 */

#define FEATURE_NUM_INT_REMAP_SUP	GENMASK_ULL(9, 8)	/* [한국어] 장치당 인터럽트 재매핑 항목 수의 인코딩 */
/* [한국어] 인터럽트 재매핑 항목을 장치당 2048개까지 쓸 수 있는지 판별한다.
 * 기본은 512개이고, 이 값이면 표를 네 배로 잡아 더 많은 인터럽트를 담는다. */
#define FEATURE_NUM_INT_REMAP_SUP_2K(x) \
	(FIELD_GET(FEATURE_NUM_INT_REMAP_SUP, x) == 0x1)	/* [한국어] 값이 1 이면 장치당 2048개 항목 */

/* Note:
 * The current driver only support 16-bit PASID.
 * Currently, hardware only implement upto 16-bit PASID
 * even though the spec says it could have upto 20 bits.
 */
#define PASID_MASK		0x0000ffff	/* [한국어] 드라이버가 다루는 PASID 의 폭. 위 영어 주석대로 스펙은 20비트를 허용하지만 실제 하드웨어와 드라이버 모두 16비트만 쓴다 */

/* MMIO status bits */
#define MMIO_STATUS_EVT_OVERFLOW_MASK		BIT(0)	/* [한국어] 이벤트 로그가 넘쳤다 — 그동안의 오류 기록이 유실됐다는 뜻 */
#define MMIO_STATUS_EVT_INT_MASK		BIT(1)	/* [한국어] 이벤트 로그 인터럽트가 걸렸다 */
#define MMIO_STATUS_COM_WAIT_INT_MASK		BIT(2)	/* [한국어] 완료 대기(COMPLETION_WAIT) 명령이 끝났다. 무효화 완료를 기다리는 근거다 */
#define MMIO_STATUS_EVT_RUN_MASK		BIT(3)	/* [한국어] 이벤트 로그가 동작 중 */
#define MMIO_STATUS_PPR_OVERFLOW_MASK		BIT(5)	/* [한국어] PPR 로그가 넘쳤다. 응답받지 못한 장치가 멈출 수 있어 심각하다 */
#define MMIO_STATUS_PPR_INT_MASK		BIT(6)	/* [한국어] PPR 로그 인터럽트 */
#define MMIO_STATUS_PPR_RUN_MASK		BIT(7)	/* [한국어] PPR 로그가 동작 중 */
#define MMIO_STATUS_GALOG_RUN_MASK		BIT(8)	/* [한국어] GA 로그가 동작 중 */
#define MMIO_STATUS_GALOG_OVERFLOW_MASK		BIT(9)	/* [한국어] GA 로그가 넘쳤다 */
#define MMIO_STATUS_GALOG_INT_MASK		BIT(10)	/* [한국어] GA 로그 인터럽트 */

/* event logging constants */
#define EVENT_ENTRY_SIZE	0x10	/* [한국어] 이벤트 로그 항목 하나가 16바이트 */
#define EVENT_TYPE_SHIFT	28	/* [한국어] 항목에서 사건 종류가 놓인 자리 */
#define EVENT_TYPE_MASK		0xf	/* [한국어] 그 종류 필드의 폭(4비트) */
#define EVENT_TYPE_ILL_DEV	0x1	/* [한국어] 장치 테이블 항목이 유효하지 않은 장치가 요청을 냈다 */
#define EVENT_TYPE_IO_FAULT	0x2	/* [한국어] 변환 실패 — 매핑되지 않은 주소나 권한 위반 */
#define EVENT_TYPE_DEV_TAB_ERR	0x3	/* [한국어] 장치 테이블을 읽다가 하드웨어 오류 */
#define EVENT_TYPE_PAGE_TAB_ERR	0x4	/* [한국어] 페이지 테이블을 읽다가 하드웨어 오류 */
#define EVENT_TYPE_ILL_CMD	0x5	/* [한국어] 명령 버퍼에 잘못된 명령이 들어왔다 — 드라이버 버그를 뜻한다 */
#define EVENT_TYPE_CMD_HARD_ERR	0x6	/* [한국어] 명령을 처리하다 하드웨어 오류 */
#define EVENT_TYPE_IOTLB_INV_TO	0x7	/* [한국어] 장치 IOTLB 무효화에 장치가 응답하지 않았다 */
#define EVENT_TYPE_INV_DEV_REQ	0x8	/* [한국어] 장치가 허용되지 않는 종류의 요청을 냈다 */
#define EVENT_TYPE_INV_PPR_REQ	0x9	/* [한국어] 잘못된 페이지 요청 */
#define EVENT_TYPE_RMP_FAULT	0xd	/* [한국어] SEV-SNP 의 역매핑 테이블 검사에 걸렸다 */
#define EVENT_TYPE_RMP_HW_ERR	0xe	/* [한국어] 그 검사 중 하드웨어 오류 */
#define EVENT_DEVID_MASK	0xffff	/* [한국어] 사건을 낸 장치의 요청자 id */
#define EVENT_DEVID_SHIFT	0	/* [한국어] 그 필드의 위치 */
#define EVENT_DOMID_MASK_LO	0xffff	/* [한국어] 도메인 id 의 하위 16비트 */
#define EVENT_DOMID_MASK_HI	0xf0000	/* [한국어] 상위 4비트 — 도메인 id 가 20비트로 늘면서 자리가 나뉘었다 */
#define EVENT_FLAGS_MASK	0xfff	/* [한국어] 사건 플래그 필드 */
#define EVENT_FLAGS_SHIFT	0x10	/* [한국어] 그 위치 */
#define EVENT_FLAG_RW		0x020	/* [한국어] 읽기/쓰기 구분 비트 */
#define EVENT_FLAG_I		0x008	/* [한국어] 인터럽트 요청에서 난 사건인지 */

/* feature control bits */
#define CONTROL_IOMMU_EN	0	/* [한국어] IOMMU 전체를 켠다. 이 비트가 서야 변환이 시작된다 */
#define CONTROL_HT_TUN_EN	1	/* [한국어] HyperTransport 터널 변환 활성화 */
#define CONTROL_EVT_LOG_EN	2	/* [한국어] 이벤트 로그 활성화 */
#define CONTROL_EVT_INT_EN	3	/* [한국어] 이벤트가 쌓이면 인터럽트를 낸다 */
#define CONTROL_COMWAIT_EN	4	/* [한국어] 완료 대기 인터럽트 활성화 */
#define CONTROL_INV_TIMEOUT	5	/* [한국어] 무효화 타임아웃 필드의 시작 비트 */
#define CONTROL_PASSPW_EN	8	/* [한국어] posted write 를 그대로 통과시킨다 */
#define CONTROL_RESPASSPW_EN	9	/* [한국어] 응답도 같은 방식으로 통과 */
#define CONTROL_COHERENT_EN	10	/* [한국어] IOMMU 의 테이블 접근을 캐시 코히런트하게 — 켜면 드라이버가 캐시를 직접 밀어낼 필요가 없다 */
#define CONTROL_ISOC_EN		11	/* [한국어] 등시성(isochronous) 트래픽 처리 활성화 */
#define CONTROL_CMDBUF_EN	12	/* [한국어] 명령 버퍼 활성화. 이것 없이는 무효화를 보낼 수 없다 */
#define CONTROL_PPRLOG_EN	13	/* [한국어] PPR 로그 활성화 */
#define CONTROL_PPRINT_EN	14	/* [한국어] PPR 인터럽트 활성화 */
#define CONTROL_PPR_EN		15	/* [한국어] PPR 기능 자체를 켠다 */
#define CONTROL_GT_EN		16	/* [한국어] 게스트 변환(PASID 별 변환) 활성화 */
#define CONTROL_GA_EN		17	/* [한국어] 게스트 APIC 활성화 */
#define CONTROL_GAM_EN		25	/* [한국어] 게스트 가상 APIC 모드 — 인터럽트를 게스트에 직접 전달 */
#define CONTROL_GALOG_EN	28	/* [한국어] GA 로그 활성화 */
#define CONTROL_GAINT_EN	29	/* [한국어] GA 로그 인터럽트 활성화 */
#define CONTROL_NUM_INT_REMAP_MODE	43	/* [한국어] 인터럽트 재매핑 표 크기 모드 필드의 시작 비트 */
#define CONTROL_NUM_INT_REMAP_MODE_MASK	0x03	/* [한국어] 그 필드의 폭(2비트) */
#define CONTROL_NUM_INT_REMAP_MODE_2K	0x01	/* [한국어] 장치당 2048개 항목을 쓰겠다는 값 */
#define CONTROL_EPH_EN		45	/* [한국어] 향상된 PPR 처리 활성화 */
#define CONTROL_XT_EN		50	/* [한국어] x2APIC 모드 활성화 */
#define CONTROL_INTCAPXT_EN	51	/* [한국어] x2APIC 방식의 IOMMU 자체 인터럽트 설정 활성화 */
#define CONTROL_GCR3TRPMODE	58	/* [한국어] 게스트 CR3 트랩 모드 */
#define CONTROL_IRTCACHEDIS	59	/* [한국어] 인터럽트 재매핑 캐시 비활성화 — 디버깅이나 특정 errata 우회용 */
#define CONTROL_SNPAVIC_EN	61	/* [한국어] SNP 환경의 AVIC 활성화 */

#define CTRL_INV_TO_MASK	7	/* [한국어] 무효화 타임아웃 필드의 마스크 */
#define CTRL_INV_TO_NONE	0	/* [한국어] 타임아웃 없음 — 응답 없는 장치가 있으면 영원히 기다린다 */
#define CTRL_INV_TO_1MS		1	/* [한국어] 1밀리초 */
#define CTRL_INV_TO_10MS	2	/* [한국어] 10밀리초 */
#define CTRL_INV_TO_100MS	3	/* [한국어] 100밀리초 */
#define CTRL_INV_TO_1S		4	/* [한국어] 1초 */
#define CTRL_INV_TO_10S		5	/* [한국어] 10초 */
#define CTRL_INV_TO_100S	6	/* [한국어] 100초. 값이 클수록 고장난 장치가 시스템을 오래 붙잡는다 */

/* command specific defines */
#define CMD_COMPL_WAIT          0x01	/* [한국어] 완료 대기. 앞선 명령들이 끝났음을 확인하는 유일한 수단이다 */
#define CMD_INV_DEV_ENTRY       0x02	/* [한국어] 장치 테이블 항목 캐시 무효화 */
#define CMD_INV_IOMMU_PAGES	0x03	/* [한국어] IOMMU 의 IOTLB 무효화 */
#define CMD_INV_IOTLB_PAGES	0x04	/* [한국어] 장치 쪽 IOTLB(ATS) 무효화 */
#define CMD_INV_IRT		0x05	/* [한국어] 인터럽트 재매핑 표 캐시 무효화 */
#define CMD_COMPLETE_PPR	0x07	/* [한국어] 페이지 요청에 대한 응답을 장치에 보낸다 */
#define CMD_INV_ALL		0x08	/* [한국어] 모든 캐시를 한 번에 무효화 */

#define CMD_COMPL_WAIT_STORE_MASK	0x01	/* [한국어] 완료 시 지정한 주소에 값을 쓰라는 플래그. 드라이버는 그 값을 폴링해 완료를 안다 */
#define CMD_COMPL_WAIT_INT_MASK		0x02	/* [한국어] 완료 시 인터럽트를 내라는 플래그 */
#define CMD_INV_IOMMU_PAGES_SIZE_MASK	0x01	/* [한국어] 무효화 범위를 주소 하나가 아니라 크기로 지정한다는 플래그 */
#define CMD_INV_IOMMU_PAGES_PDE_MASK	0x02	/* [한국어] 상위 페이지 디렉터리 항목까지 무효화한다. 매핑을 새로 만들 때 필요하다 */
#define CMD_INV_IOMMU_PAGES_GN_MASK	0x04	/* [한국어] 게스트 주소 공간(PASID 별)의 무효화 */

#define PPR_STATUS_MASK			0xf	/* [한국어] PPR 응답의 상태 코드 필드 */
#define PPR_STATUS_SHIFT		12	/* [한국어] 그 위치 */

#define CMD_INV_IOMMU_ALL_PAGES_ADDRESS	0x7fffffffffffffffULL	/* [한국어] "주소 전체"를 뜻하는 특수 주소. 하위 비트가 모두 1이면 크기 무제한으로 해석된다 */

/* macros and definitions for device table entries */
#define DEV_ENTRY_VALID         0x00	/* [한국어] DTE 의 유효 비트 위치. 이 비트가 없으면 그 장치의 DMA 는 전부 차단된다 */
#define DEV_ENTRY_TRANSLATION   0x01	/* [한국어] 변환 활성화 비트. 유효하지만 변환하지 않는(패스스루) 상태와 구별한다 */
#define DEV_ENTRY_HAD           0x07	/* [한국어] 호스트 접근/더티 비트 추적 활성화 */
#define DEV_ENTRY_PPR           0x34	/* [한국어] 이 장치의 페이지 요청을 받아들일지 */
#define DEV_ENTRY_IR            0x3d	/* [한국어] 인터럽트 읽기 허용 */
#define DEV_ENTRY_IW            0x3e	/* [한국어] 인터럽트 쓰기 허용 */
#define DEV_ENTRY_NO_PAGE_FAULT	0x62	/* [한국어] 페이지 폴트를 내지 않고 조용히 실패시킨다 */
#define DEV_ENTRY_EX            0x67	/* [한국어] 제외 범위를 이 장치에 적용할지 */
#define DEV_ENTRY_SYSMGT1       0x68	/* [한국어] 시스템 관리 메시지 처리 방식 비트 1 */
#define DEV_ENTRY_SYSMGT2       0x69	/* [한국어] 같은 필드의 비트 2 */
#define DTE_DATA1_SYSMGT_MASK	GENMASK_ULL(41, 40)	/* [한국어] 두 비트를 한 필드로 다룰 때의 마스크 */

#define DEV_ENTRY_IRQ_TBL_EN	0x80	/* [한국어] 인터럽트 재매핑 표를 쓸지. 서지 않으면 인터럽트가 재매핑을 거치지 않는다 */
#define DEV_ENTRY_INIT_PASS     0xb8	/* [한국어] INIT 인터럽트를 재매핑 없이 통과시킨다 */
#define DEV_ENTRY_EINT_PASS     0xb9	/* [한국어] 외부 인터럽트를 통과시킨다 */
#define DEV_ENTRY_NMI_PASS      0xba	/* [한국어] NMI 를 통과시킨다 — 재매핑하면 NMI 를 놓칠 수 있어서 */
#define DEV_ENTRY_LINT0_PASS    0xbe	/* [한국어] 로컬 인터럽트 0 을 통과 */
#define DEV_ENTRY_LINT1_PASS    0xbf	/* [한국어] 로컬 인터럽트 1 을 통과 */
#define DEV_ENTRY_MODE_MASK	0x07	/* [한국어] 페이지 테이블 레벨 수 필드의 마스크 */
#define DEV_ENTRY_MODE_SHIFT	0x09	/* [한국어] 그 필드의 위치. 0 이면 변환 없음, 1~6 이 레벨 수다 */

#define MAX_DEV_TABLE_ENTRIES	0xffff	/* [한국어] 장치 테이블 항목 수의 상한 = 16비트 요청자 id 전체 */

/* constants to configure the command buffer */
#define CMD_BUFFER_SIZE    8192	/* [한국어] 명령 버퍼 크기. 8KB */
#define CMD_BUFFER_UNINITIALIZED 1	/* [한국어] 아직 하드웨어에 걸지 않았음을 나타내는 표식 */
#define CMD_BUFFER_ENTRIES 512	/* [한국어] 8KB / 16바이트 = 512개 명령 */
#define MMIO_CMD_SIZE_SHIFT 56	/* [한국어] 버퍼 크기를 인코딩해 넣는 자리 */
#define MMIO_CMD_SIZE_512 (0x9ULL << MMIO_CMD_SIZE_SHIFT)	/* [한국어] 512개 항목을 뜻하는 인코딩(0x9) */
#define MMIO_CMD_HEAD_MASK	GENMASK_ULL(18, 4)	/* Command buffer head ptr field [18:4] */	/* [한국어] 머리 포인터가 놓인 비트 구간 (원 주석: [18:4]) */
#define MMIO_CMD_BUFFER_HEAD(x) FIELD_GET(MMIO_CMD_HEAD_MASK, (x))	/* [한국어] 레지스터 값에서 머리 위치를 꺼낸다 */
#define MMIO_CMD_TAIL_MASK	GENMASK_ULL(18, 4)	/* Command buffer tail ptr field [18:4] */	/* [한국어] 꼬리 포인터의 비트 구간 */
#define MMIO_CMD_BUFFER_TAIL(x) FIELD_GET(MMIO_CMD_TAIL_MASK, (x))	/* [한국어] 꼬리 위치를 꺼낸다 */

/* constants for event buffer handling */
#define EVT_BUFFER_SIZE		8192 /* 512 entries */	/* [한국어] 이벤트 버퍼 크기 8KB (원 주석: 512개 항목) */
#define EVT_LEN_MASK		(0x9ULL << 56)	/* [한국어] 그 크기를 레지스터에 인코딩한 값 */

/* Constants for PPR Log handling */
#define PPR_LOG_ENTRIES		512	/* [한국어] PPR 로그 항목 수 */
#define PPR_LOG_SIZE_SHIFT	56	/* [한국어] 크기 인코딩의 자리 */
#define PPR_LOG_SIZE_512	(0x9ULL << PPR_LOG_SIZE_SHIFT)	/* [한국어] 512개 항목을 뜻하는 인코딩 */
#define PPR_ENTRY_SIZE		16	/* [한국어] PPR 항목 하나가 16바이트 */
#define PPR_LOG_SIZE		(PPR_ENTRY_SIZE * PPR_LOG_ENTRIES)	/* [한국어] 로그 전체 크기 = 항목 크기 × 개수 */

/* PAGE_SERVICE_REQUEST PPR Log Buffer Entry flags */
#define PPR_FLAG_EXEC		0x002	/* Execute permission requested */	/* [한국어] 실행 권한을 요청한다 (원 주석: Execute permission requested) */
#define PPR_FLAG_READ		0x004	/* Read permission requested */	/* [한국어] 읽기 권한 요청 */
#define PPR_FLAG_WRITE		0x020	/* Write permission requested */	/* [한국어] 쓰기 권한 요청 */
#define PPR_FLAG_US		0x040	/* 1: User, 0: Supervisor */	/* [한국어] 1이면 사용자, 0이면 커널 권한의 접근 */
#define PPR_FLAG_RVSD		0x080	/* Reserved bit not zero */	/* [한국어] 예약 비트가 0이 아니었다 — 잘못된 요청이라는 표시 */
#define PPR_FLAG_GN		0x100	/* GVA and PASID is valid */	/* [한국어] GVA 와 PASID 가 유효하다. SVA 요청임을 뜻한다 */

#define PPR_REQ_TYPE(x)		(((x) >> 60) & 0xfULL)	/* [한국어] PPR 항목의 요청 종류. 지금은 페이지 폴트 하나뿐이다 */
#define PPR_FLAGS(x)		(((x) >> 48) & 0xfffULL)	/* [한국어] 요청한 권한과 상태 플래그 묶음 */
#define PPR_DEVID(x)		((x) & 0xffffULL)	/* [한국어] 요청을 낸 장치의 id */
#define PPR_TAG(x)		(((x) >> 32) & 0x3ffULL)	/* [한국어] 요청 태그. 응답을 이 값으로 짝지어 보낸다 — 장치는 그것으로 어느 요청의 답인지 안다 */
#define PPR_PASID1(x)		(((x) >> 16) & 0xffffULL)	/* [한국어] PASID 의 하위 16비트 */
#define PPR_PASID2(x)		(((x) >> 42) & 0xfULL)	/* [한국어] 상위 4비트 — 필드가 두 조각으로 나뉘어 있다 */
#define PPR_PASID(x)		((PPR_PASID2(x) << 16) | PPR_PASID1(x))	/* [한국어] 두 조각을 합쳐 원래 PASID 를 복원한다 */

#define PPR_REQ_FAULT		0x01	/* [한국어] 페이지 폴트 요청을 뜻하는 종류 값 */

/* Constants for GA Log handling */
#define GA_LOG_ENTRIES		512	/* [한국어] GA 로그 항목 수 */
#define GA_LOG_SIZE_SHIFT	56	/* [한국어] 크기 인코딩 자리 */
#define GA_LOG_SIZE_512		(0x8ULL << GA_LOG_SIZE_SHIFT)	/* [한국어] 512개 항목 인코딩. 항목이 8바이트라 PPR 과 값이 다르다 */
#define GA_ENTRY_SIZE		8	/* [한국어] GA 항목 하나가 8바이트 */
#define GA_LOG_SIZE		(GA_ENTRY_SIZE * GA_LOG_ENTRIES)	/* [한국어] 로그 전체 크기 */

#define GA_TAG(x)		(u32)(x & 0xffffffffULL)	/* [한국어] 게스트 인터럽트의 태그. KVM 이 이 값으로 어느 vCPU/벡터인지 안다 */
#define GA_DEVID(x)		(u16)(((x) >> 32) & 0xffffULL)	/* [한국어] 그 인터럽트를 낸 장치 */
#define GA_REQ_TYPE(x)		(((x) >> 60) & 0xfULL)	/* [한국어] 기록된 사건의 종류 */

#define GA_GUEST_NR		0x1	/* [한국어] 게스트에 전달하지 못한 인터럽트를 뜻하는 값. KVM 이 대신 주입해야 한다 */

#define IOMMU_IN_ADDR_BIT_SIZE  52	/* [한국어] 입력(가상) 주소의 비트 수 */
#define IOMMU_OUT_ADDR_BIT_SIZE 52	/* [한국어] 출력(물리) 주소의 비트 수 */

/*
 * This bitmap is used to advertise the page sizes our hardware support
 * to the IOMMU core, which will then use this information to split
 * physically contiguous memory regions it is mapping into page sizes
 * that we support.
 *
 * 512GB Pages are not supported due to a hardware bug
 * Page sizes >= the 52 bit max physical address of the CPU are not supported.
 */
#define AMD_IOMMU_PGSIZES	(GENMASK_ULL(51, 12) ^ SZ_512G)	/* [한국어] 지원하는 페이지 크기 비트맵. 512GB 를 XOR 로 빼는 이유는 위 영어 주석대로 하드웨어 버그 때문이다 */

/* Special mode where page-sizes are limited to 4 KiB */
#define AMD_IOMMU_PGSIZES_4K	(PAGE_SIZE)	/* [한국어] 4KB 만 쓰는 제한 모드 (원 주석: 특수 모드) */

/* 4K, 2MB, 1G page sizes are supported */
#define AMD_IOMMU_PGSIZES_V2	(PAGE_SIZE | (1ULL << 21) | (1ULL << 30))	/* [한국어] v2 페이지 테이블이 지원하는 세 가지 크기 — 4K, 2MB, 1GB */

/* Bit value definition for dte irq remapping fields*/
#define DTE_IRQ_PHYS_ADDR_MASK		GENMASK_ULL(51, 6)	/* [한국어] DTE 안에서 인터럽트 재매핑 표의 물리 주소가 놓인 구간 */
#define DTE_IRQ_REMAP_INTCTL_MASK	(0x3ULL << 60)	/* [한국어] 인터럽트 제어 필드의 마스크 */
#define DTE_IRQ_REMAP_INTCTL    (2ULL << 60)	/* [한국어] "재매핑 표를 거쳐 전달"을 뜻하는 값. 다른 값은 차단하거나 그대로 통과시킨다 */
#define DTE_IRQ_REMAP_ENABLE    1ULL	/* [한국어] 인터럽트 재매핑 활성화 비트 */

#define DTE_INTTAB_ALIGNMENT    128	/* [한국어] 재매핑 표의 정렬 요구. 하드웨어가 하위 비트를 다른 용도로 쓰기 때문이다 */
#define DTE_INTTABLEN_MASK      (0xfULL << 1)	/* [한국어] 표 크기 필드의 마스크 */
#define DTE_INTTABLEN_VALUE_512 9ULL	/* [한국어] 512개 항목을 뜻하는 값(2^9) */
#define DTE_INTTABLEN_512       (DTE_INTTABLEN_VALUE_512 << 1)	/* [한국어] 그 값을 필드 자리로 민 것 */
#define MAX_IRQS_PER_TABLE_512  BIT(DTE_INTTABLEN_VALUE_512)	/* [한국어] 장치당 인터럽트 수의 상한 512 */
#define DTE_INTTABLEN_VALUE_2K	11ULL	/* [한국어] 2048개를 뜻하는 값(2^11) */
#define DTE_INTTABLEN_2K	(DTE_INTTABLEN_VALUE_2K << 1)	/* [한국어] 필드 자리로 민 것 */
#define MAX_IRQS_PER_TABLE_2K	BIT(DTE_INTTABLEN_VALUE_2K)	/* [한국어] 상한 2048. 하드웨어가 지원하면 이쪽을 쓴다 */

#define PAGE_MODE_NONE    0x00	/* [한국어] 변환하지 않는다 — 장치가 낸 주소를 그대로 물리 주소로 쓴다 */
#define PAGE_MODE_1_LEVEL 0x01	/* [한국어] 페이지 테이블 1단계 */
#define PAGE_MODE_2_LEVEL 0x02	/* [한국어] 2단계 */
#define PAGE_MODE_3_LEVEL 0x03	/* [한국어] 3단계 */
#define PAGE_MODE_4_LEVEL 0x04	/* [한국어] 4단계. 48비트 주소 공간에 해당한다 */
#define PAGE_MODE_5_LEVEL 0x05	/* [한국어] 5단계 */
#define PAGE_MODE_6_LEVEL 0x06	/* [한국어] 6단계. 최대 주소 공간 */
#define PAGE_MODE_7_LEVEL 0x07	/* [한국어] 7 은 단계 수가 아니라 "이 항목이 곧 큰 페이지"를 뜻하는 특수 값이다 */

#define GUEST_PGTABLE_4_LEVEL	0x00	/* [한국어] 게스트 페이지 테이블이 4단계(48비트) */
#define GUEST_PGTABLE_5_LEVEL	0x01	/* [한국어] 5단계(57비트, LA57) */

#define PM_ADDR_MASK		0x000ffffffffff000ULL	/* [한국어] 페이지 테이블 항목에서 순수 주소만 남기는 마스크. 하위 12비트(플래그)와 상위 예약 비트를 뗀다 */

/*
 * Bit value definition for DTE fields
 */
#define DTE_FLAG_V	BIT_ULL(0)	/* [한국어] 유효(Valid). 이 비트가 없으면 그 장치의 요청은 모두 차단된다 */
#define DTE_FLAG_TV	BIT_ULL(1)	/* [한국어] 변환 유효(Translation Valid). V 는 있는데 TV 가 없으면 변환 없이 통과시킨다 — 패스스루가 그 상태다 */
#define DTE_FLAG_HAD	(3ULL << 7)	/* [한국어] 호스트 접근/더티 비트 추적 활성화 */
#define DTE_MODE_MASK	GENMASK_ULL(11, 9)	/* [한국어] 페이지 테이블 레벨 수 필드. 0 이면 변환 없음 */
#define DTE_HOST_TRP	GENMASK_ULL(51, 12)	/* [한국어] 호스트 페이지 테이블의 물리 주소가 놓인 구간 */
#define DTE_FLAG_PPR	BIT_ULL(52)	/* [한국어] 이 장치의 페이지 요청을 받아들인다 */
#define DTE_FLAG_GIOV	BIT_ULL(54)	/* [한국어] 게스트 I/O 가상 주소 모드 */
#define DTE_FLAG_GV	BIT_ULL(55)	/* [한국어] 게스트 변환 유효 — PASID 별 페이지 테이블을 쓴다는 뜻 */
#define DTE_GLX		GENMASK_ULL(57, 56)	/* [한국어] 게스트 CR3 테이블의 레벨 수. PASID 개수의 상한을 결정한다 */
#define DTE_FLAG_IR	BIT_ULL(61)	/* [한국어] 장치의 읽기 요청 허용 */
#define DTE_FLAG_IW	BIT_ULL(62)	/* [한국어] 장치의 쓰기 요청 허용. 읽기 전용 장치는 이 비트를 빼서 강제할 수 있다 */

#define DTE_FLAG_IOTLB	BIT_ULL(32)	/* [한국어] 장치 IOTLB(ATS) 사용 허용. 장치가 변환을 캐시할 수 있게 된다 */
#define DTE_FLAG_MASK	(0x3ffULL << 32)	/* [한국어] DTE 두 번째 워드의 플래그 구간 전체 */
#define DTE_DOMID_MASK	GENMASK_ULL(15, 0)	/* [한국어] 도메인 id. 무효화 명령이 이 값으로 대상을 좁힌다 */

#define DTE_GCR3_14_12	GENMASK_ULL(60, 58)	/* [한국어] 게스트 CR3 주소의 12~14비트. 세 조각으로 흩어져 있다 */
#define DTE_GCR3_30_15	GENMASK_ULL(31, 16)	/* [한국어] 15~30비트 */
#define DTE_GCR3_51_31	GENMASK_ULL(63, 43)	/* [한국어] 31~51비트. 스펙이 자리를 짜맞추다 보니 이렇게 나뉘었다 */

#define DTE_GPT_LEVEL_SHIFT	54	/* [한국어] 게스트 페이지 테이블 레벨 필드의 위치 */
#define DTE_GPT_LEVEL_MASK	GENMASK_ULL(55, 54)	/* [한국어] 그 필드의 마스크. 4단계인지 5단계인지를 담는다 */

#define GCR3_VALID		0x01ULL	/* [한국어] 게스트 CR3 항목의 유효 비트 */

/* DTE[128:179] | DTE[184:191] */
#define DTE_DATA2_INTR_MASK	~GENMASK_ULL(55, 52)	/* [한국어] DTE 세 번째 워드에서 인터럽트 관련 비트만 남기는 마스크 (원 주석: DTE[128:179] | DTE[184:191]) */

#define IOMMU_PROT_MASK 0x03	/* [한국어] 권한 비트 두 개의 마스크 */
#define IOMMU_PROT_IR 0x01	/* [한국어] 읽기 권한 */
#define IOMMU_PROT_IW 0x02	/* [한국어] 쓰기 권한 */

#define IOMMU_UNITY_MAP_FLAG_EXCL_RANGE	(1 << 2)	/* [한국어] IVRS 표의 단위 매핑이 "제외 범위"임을 나타내는 플래그 */

/* IOMMU capabilities */
#define IOMMU_CAP_IOTLB   24	/* [한국어] PCI 능력 구조에서 장치 IOTLB 지원을 알리는 비트 */
#define IOMMU_CAP_NPCACHE 26	/* [한국어] 존재하지 않는 항목도 캐시한다는 비트. 매핑을 새로 만들 때도 무효화가 필요해진다 */
#define IOMMU_CAP_EFR     27	/* [한국어] 확장 기능 레지스터가 있는지 */

/* IOMMU IVINFO */
#define IOMMU_IVINFO_OFFSET     36	/* [한국어] IVRS 표에서 IVinfo 필드의 오프셋 */
#define IOMMU_IVINFO_EFRSUP     BIT(0)	/* [한국어] 펌웨어가 확장 기능 레지스터 지원을 알리는 비트 */
#define IOMMU_IVINFO_DMA_REMAP  BIT(1)	/* [한국어] 펌웨어가 DMA 재매핑이 필요하다고 알리는 비트 */

/* IOMMU Feature Reporting Field (for IVHD type 10h */
#define IOMMU_FEAT_GASUP_SHIFT	6	/* [한국어] IVHD type 10h 의 기능 필드에서 GA 지원 비트 위치 */

/* IOMMU HATDIS for IVHD type 11h and 40h */
#define IOMMU_IVHD_ATTR_HATDIS_SHIFT	0	/* [한국어] IVHD type 11h/40h 에서 호스트 주소 변환 비활성 비트 */

/* IOMMU Extended Feature Register (EFR) */
#define IOMMU_EFR_XTSUP_SHIFT	2	/* [한국어] EFR 에서 x2APIC 지원 비트 */
#define IOMMU_EFR_GASUP_SHIFT	7	/* [한국어] EFR 에서 GA 지원 비트 */
#define IOMMU_EFR_MSICAPMMIOSUP_SHIFT	46	/* [한국어] MSI 능력을 MMIO 로 접근할 수 있는지 */

#define MAX_DOMAIN_ID 65536	/* [한국어] 도메인 id 공간의 크기 = 16비트 전체 */

/* Timeout stuff */
#define LOOP_TIMEOUT		100000	/* [한국어] 바쁜 대기 루프의 반복 상한. 하드웨어가 응답하지 않을 때 영원히 도는 것을 막는다 */
#define MMIO_STATUS_TIMEOUT	2000000	/* [한국어] 상태 비트를 기다릴 때의 더 긴 상한 */

extern bool amd_iommu_dump;	/* [한국어] 부팅 옵션으로 켜는 상세 로그 플래그 */
/* [한국어] amd_iommu_dump 가 켜져 있을 때만 로그를 낸다.
 * 초기화 경로가 IVRS 표의 항목을 하나하나 찍는 데 쓰는데, 평소에는
 * 부팅 로그를 뒤덮으므로 옵션으로 켜야만 보이게 했다.
 * do-while(0) 로 감싸 한 문장처럼 쓰이게 한다. */
#define DUMP_printk(format, arg...)				\
	do {							\
		if (amd_iommu_dump)				\
			pr_info(format, ## arg);	\
	} while(0);

/* global flag if IOMMUs cache non-present entries */
extern bool amd_iommu_np_cache;	/* [한국어] 하드웨어가 존재하지 않는 항목도 캐시하는가 (원 주석). 그렇다면 매핑 생성 시에도 무효화해야 한다 */
/* Only true if all IOMMUs support device IOTLBs */
extern bool amd_iommu_iotlb_sup;	/* [한국어] 모든 유닛이 장치 IOTLB 를 지원하는가 (원 주석). 하나라도 못 하면 전체를 쓰지 않는다 */

/*
 * [한국어] struct irq_remap_table — 장치 하나(또는 별칭 그룹)의 인터럽트 재매핑 표
 *
 * AMD 는 Intel 과 달리 재매핑 표가 IOMMU 유닛당 하나가 아니라 장치당
 * 하나다. DTE 가 그 표의 주소를 직접 담고 있어, 장치마다 독립적인 표를
 * 가질 수 있다 — 그래서 한 장치의 인터럽트가 다른 장치의 항목을 건드릴
 * 여지가 구조적으로 없다.
 *
 * 대신 DMA 별칭을 공유하는 장치들은 하드웨어가 같은 요청자 id 로 보므로
 * 표도 함께 써야 한다. 그 공유가 min_index 의 존재 이유다.
 */
struct irq_remap_table {
	raw_spinlock_t lock;
	/* [한국어] 표의 항목 배열과 할당 상태를 지킨다.
	 * 설정자/읽는 자: 인터럽트를 할당·수정·해제하는 모든 경로.
	 * raw 인 이유: 인터럽트 이동 경로에서 잡히므로 PREEMPT_RT 에서도 잠들면 안 된다. */
	unsigned min_index;
	/* [한국어] 이 표에서 실제로 쓸 수 있는 첫 항목의 인덱스.
	 * 왜 0 이 아닌가: DMA 별칭을 공유하는 장치들은 표를 함께 쓴다. 그때 앞쪽
	 *   구간을 특정 장치용으로 예약해 두고, 나머지를 공유 영역으로 쓴다.
	 * 설정자: 표를 만들 때. 읽는 자: 빈 항목을 찾는 할당 경로. */
	u32 *table;
	/* [한국어] 항목 배열 자체. 하드웨어가 DTE 에 적힌 주소로 직접 읽는다.
	 * 설정자: 표 생성 시 할당하고, 인터럽트마다 항목을 채운다.
	 * 읽는 자: 하드웨어와, 항목을 고치는 드라이버 경로.
	 * 동기화: 위 lock 으로 보호하고, 고친 뒤에는 반드시 IRT 캐시를 무효화한다 —
	 *   하지 않으면 하드웨어가 한동안 옛 목적지로 인터럽트를 보낸다. */
};

/* Interrupt remapping feature used? */
extern bool amd_iommu_irq_remap;	/* [한국어] 인터럽트 재매핑을 실제로 쓰고 있는가 (원 주석). 초기화가 성공해야 참이 된다 */

extern const struct iommu_ops amd_iommu_ops;	/* [한국어] 코어 IOMMU 계층에 등록하는 콜백 표. iommu.c 가 정의한다 */

/* IVRS indicates that pre-boot remapping was enabled */
extern bool amdr_ivrs_remap_support;	/* [한국어] IVRS 표가 "부팅 전부터 재매핑이 켜져 있었다"고 알리는가 (원 주석). kdump 경로의 판단 근거다 */

#define PCI_SBDF_TO_SEGID(sbdf)		(((sbdf) >> 16) & 0xffff)	/* [한국어] 세그먼트+BDF 를 합친 32비트 값에서 세그먼트만 꺼낸다 */
#define PCI_SBDF_TO_DEVID(sbdf)		((sbdf) & 0xffff)	/* [한국어] 같은 값에서 BDF(요청자 id)만 꺼낸다 */
/* [한국어] 세그먼트와 BDF 를 하나의 32비트 키로 합친다.
 * 세그먼트가 여럿인 시스템에서는 BDF 만으로 장치를 특정할 수 없어,
 * 드라이버 내부의 조회 키는 항상 이 합친 값을 쓴다. */
#define PCI_SEG_DEVID_TO_SBDF(seg, devid)	((((u32)(seg) & 0xffff) << 16) | \
						 ((devid) & 0xffff))	/* [한국어] 하위 16비트에 BDF 를 담는다 */

/* Make iterating over all pci segment easier */
/* [한국어] 전역 세그먼트 목록을 훑는 관용구.
 * 세그먼트마다 장치 테이블과 별칭 표가 따로 있어, 초기화와 정리가
 * 대부분 이 순회로 시작한다. */
#define for_each_pci_segment(pci_seg) \
	list_for_each_entry((pci_seg), &amd_iommu_pci_seg_list, list)
/* [한국어] 순회 중 원소를 목록에서 뺄 수 있는 변형. 해제 경로가 쓴다. */
#define for_each_pci_segment_safe(pci_seg, next) \
	list_for_each_entry_safe((pci_seg), (next), &amd_iommu_pci_seg_list, list)	/* [한국어] 다음 원소를 미리 잡아 두어 현재 원소를 지워도 안전하다 */
/*
 * Make iterating over all IOMMUs easier
 */
/* [한국어] (위 영어 주석에 이어) 전역 IOMMU 유닛 목록을 훑는다.
 * 무효화를 모든 유닛에 보내야 하는 경우가 많아 자주 쓰인다. */
#define for_each_iommu(iommu) \
	list_for_each_entry((iommu), &amd_iommu_list, list)
/* [한국어] 순회 중 유닛을 뺄 수 있는 변형. */
#define for_each_iommu_safe(iommu, next) \
	list_for_each_entry_safe((iommu), (next), &amd_iommu_list, list)	/* [한국어] 같은 방식의 안전한 순회 */
/* Making iterating over protection_domain->dev_data_list easier */
/* [한국어] (위 영어 주석에 이어) 도메인에 붙은 {장치, PASID} 쌍을 훑는다.
 * 도메인의 매핑이 바뀌면 이 목록 전체의 캐시를 지워야 한다. */
#define for_each_pdom_dev_data(pdom_dev_data, pdom) \
	list_for_each_entry(pdom_dev_data, &pdom->dev_data_list, list)
/* [한국어] 순회 중 쌍을 뺄 수 있는 변형. 장치를 뗄 때 쓴다. */
#define for_each_pdom_dev_data_safe(pdom_dev_data, next, pdom) \
	list_for_each_entry_safe((pdom_dev_data), (next), &pdom->dev_data_list, list)

/* [한국어] IVRS 표가 특정 장치들의 DTE 플래그를 재정의한 목록을 훑는다.
 * 펌웨어가 "이 장치는 이런 플래그로 설정하라"고 지시한 예외들이다. */
#define for_each_ivhd_dte_flags(entry) \
	list_for_each_entry((entry), &amd_ivhd_dev_flags_list, list)	/* [한국어] DTE 플래그 재정의 목록을 훑는 본체 */

struct amd_iommu;	/* [한국어] 전방 선언 — 아래 구조체들이 서로를 가리킨다 */
struct iommu_domain;	/* [한국어] 코어 계층의 도메인. 정의는 linux/iommu.h 에 */
struct irq_domain;	/* [한국어] 인터럽트 도메인 */
struct amd_irte_ops;	/* [한국어] IRTE 형식별 조작 함수 표. 형식이 두 가지(기본/GA)라 함수로 갈라 둔다 */

#define AMD_IOMMU_FLAG_TRANS_PRE_ENABLED      (1 << 0)	/* [한국어] 커널 진입 전부터 변환이 켜져 있었음을 나타내는 플래그. kdump 에서 물려받은 상태를 다룰 때 쓴다 */

/*
 * [한국어] struct gcr3_tbl_info — 장치의 게스트 CR3 테이블 상태
 *
 * PASID 별로 다른 페이지 테이블을 쓰려면 "PASID → 페이지 테이블 루트"의
 * 대응이 필요하다. AMD 는 그것을 GCR3 테이블이라 부르며, x86 의 CR3 가
 * 프로세스마다 다른 것과 같은 발상이다.
 *
 * 이 구조체는 그 테이블 자체가 아니라 테이블에 대한 메타데이터다 — 어디
 * 있는지, 몇 단계인지, 지금 몇 개의 PASID 가 붙어 있는지.
 */
struct gcr3_tbl_info {
	u64	*gcr3_tbl;	/* Guest CR3 table */
	/* [한국어] PASID → 페이지 테이블 루트 대응표의 주소 (원 주석: Guest CR3 table).
	 * 설정자: 이 장치에 첫 PASID 가 붙을 때 할당된다.
	 * 읽는 자: DTE 를 채울 때 이 주소를 하드웨어에 알린다.
	 * 왜 장치마다 있는가: DTE 가 장치 단위라, PASID 별 테이블도 장치를 통해
	 *   도달해야 한다. 같은 도메인의 두 장치는 서로 다른 GCR3 표를 가질 수 있다. */
	int	glx;		/* Number of levels for GCR3 table */
	/* [한국어] 그 표의 레벨 수 (원 주석: Number of levels for GCR3 table).
	 * 값 범위: 0~2. 레벨이 많을수록 담을 수 있는 PASID 가 많아진다.
	 * 설정자: 하드웨어의 GLX 능력과 필요한 PASID 수를 보고 정한다.
	 * 읽는 자: DTE 의 GLX 필드와 표 순회 코드. */
	u32	pasid_cnt;	/* Track attached PASIDs */
	/* [한국어] 지금 이 표를 통해 붙어 있는 PASID 수 (원 주석: Track attached PASIDs).
	 * 설정자: PASID 를 붙이고 뗄 때 증감한다.
	 * 읽는 자: 0 이 되면 표를 해제하고 DTE 의 게스트 변환 비트를 내린다.
	 * 왜 세는가: 표를 언제 놓아도 되는지 판단하는 유일한 근거다. */
	u16	domid;		/* Per device domain ID */
	/* [한국어] 이 장치에 부여된 도메인 id (원 주석: Per device domain ID).
	 * 왜 장치마다 따로 두는가: v2 페이지 테이블을 쓰는 장치는 도메인을 공유해도
	 *   무효화를 장치 단위로 해야 하는 경우가 있어, 별도의 id 를 발급한다.
	 * 설정자/읽는 자: GCR3 표를 만들고 없앨 때, 그리고 무효화 명령을 만들 때. */
};

/*
 * [한국어] enum protection_domain_mode — 이 도메인이 어떤 페이지 테이블을 쓰는가
 *
 * AMD IOMMU 는 두 가지 페이지 테이블 형식을 지원한다. v1 은 IOMMU 고유
 * 형식이고, v2 는 CPU 의 x86-64 페이지 테이블과 같은 형식이다. v2 를 쓰면
 * 프로세스의 페이지 테이블을 그대로 IOMMU 에 걸 수 있어 SVA 가 가능해진다 —
 * 이 선택이 곧 "이 도메인으로 SVA 를 할 수 있는가"를 결정한다.
 */
enum protection_domain_mode {
	PD_MODE_NONE,
	/* [한국어] 페이지 테이블이 없다 — 이 도메인은 변환을 하지 않는다.
	 * 패스스루(identity) 도메인이나 아직 형식이 정해지지 않은 상태가 여기 해당한다. */
	PD_MODE_V1,
	/* [한국어] AMD IOMMU 고유의 v1 페이지 테이블.
	 * 레벨 수를 1~6 으로 자유롭게 고를 수 있고 큰 페이지 표현이 유연하지만,
	 *   CPU 의 페이지 테이블과 형식이 달라 프로세스 주소 공간을 그대로 쓸 수 없다. */
	PD_MODE_V2,
	/* [한국어] CPU 의 x86-64 페이지 테이블과 같은 형식.
	 * 형식이 같기 때문에 프로세스의 페이지 테이블을 그대로 IOMMU 에 걸 수 있고,
	 *   그것이 곧 SVA(공유 가상 주소)의 성립 조건이다.
	 * 대신 페이지 크기가 4K/2M/1G 로 제한된다. */
};

/* Track dev_data/PASID list for the protection domain */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct pdom_dev_data — 도메인에 붙은 {장치, PASID} 한 쌍
 *
 * 한 장치가 여러 PASID 로 같은 도메인에 붙을 수 있고, 한 도메인에 여러
 * 장치가 붙을 수도 있다. 그래서 "누가 이 도메인을 쓰는가"를 장치 목록만으로
 * 표현할 수 없고, 쌍을 원소로 하는 목록이 필요하다.
 *
 * 무효화 대상을 정할 때 이 목록을 훑는다 — 도메인의 매핑이 바뀌면 그것을
 * 쓰는 모든 {장치, PASID}의 캐시를 지워야 하기 때문이다.
 */
struct pdom_dev_data {
	/* Points to attached device data */
	struct iommu_dev_data *dev_data;
	/* [한국어] 이 쌍의 장치 쪽 정보 (원 주석: Points to attached device data).
	 * 읽는 자: 무효화 대상을 정할 때 어느 장치에 명령을 보낼지.
	 * 동기화: 도메인의 lock 아래에서만 목록을 걷는다. */
	/* PASID attached to the protection domain */
	ioasid_t pasid;
	/* [한국어] 그 장치가 이 도메인에 붙을 때 쓴 PASID (원 주석).
	 * 값 범위: IOMMU_NO_PASID 이면 PASID 없는 일반 DMA 다.
	 * 읽는 자: 무효화 명령이 이 값으로 대상을 좁힌다 — PASID 별 매핑을
	 *   지울 때 다른 PASID 의 캐시까지 날리지 않기 위해서다. */
	/* For protection_domain->dev_data_list */
	struct list_head list;
	/* [한국어] 도메인의 dev_data_list 에 매다는 고리 (원 주석).
	 * 동기화: protection_domain->lock 으로 보호한다. */
};

/* Keeps track of the IOMMUs attached to protection domain */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct pdom_iommu_info — 이 도메인을 쓰는 IOMMU 유닛과 그 참조 수
 *
 * 무효화 명령은 유닛마다 따로 보내야 한다. 도메인에 붙은 장치가 여러
 * 유닛에 흩어져 있으면 그 유닛들 모두에 보내야 하고, 같은 유닛에 열 장치가
 * 붙어 있어도 한 번만 보내면 된다.
 *
 * refcnt 가 그 중복 제거를 담당한다. 장치나 PASID 가 붙을 때마다 늘고
 * 떨어질 때마다 줄어, 0 이 되면 그 유닛을 목록에서 뺀다.
 */
struct pdom_iommu_info {
	struct amd_iommu *iommu; /* IOMMUs attach to protection domain */
	/* [한국어] 이 도메인의 장치가 붙어 있는 IOMMU 유닛 (원 주석).
	 * 읽는 자: 무효화 명령을 보낼 유닛 목록.
	 * 왜 필요한가: 명령 버퍼는 유닛마다 따로라, 도메인의 매핑을 바꾸면
	 *   관련된 모든 유닛에 각각 명령을 보내야 한다. */
	u32 refcnt;	/* Count of attached dev/pasid per domain/IOMMU */
	/* [한국어] 이 유닛을 통해 붙어 있는 {장치, PASID} 수 (원 주석).
	 * 설정자: 붙일 때 증가, 뗄 때 감소.
	 * 0 이 되면 이 항목을 목록에서 뺀다 — 그 유닛에는 더 이상 이 도메인을
	 *   쓰는 장치가 없으므로 무효화를 보낼 이유가 없다.
	 * 이 계수가 곧 중복 제거다: 한 유닛에 열 장치가 붙어도 명령은 한 번만 간다. */
};

/*
 * [한국어] struct amd_iommu_viommu — 게스트에게 보여 주는 가상 IOMMU
 *
 * 게스트가 자기 IOMMU 를 직접 다루게 하는 기능이다. 게스트는 자기만의
 * 도메인 id 공간을 쓰지만 하드웨어는 호스트의 도메인 id 만 알아듣기 때문에,
 * 그 사이를 번역하는 대응표(gdomid_array)가 필요하다.
 *
 * parent 가 중첩 변환의 2단계 도메인이다. 게스트가 만든 1단계 테이블이
 * 가리키는 모든 주소는 결국 이 도메인을 한 번 더 거치므로, 호스트는 게스트의
 * 테이블 내용을 검사하지 않아도 안전하다.
 */
struct amd_iommu_viommu {
	struct iommufd_viommu core;
	/* [한국어] iommufd 코어가 다루는 부분. 이 구조체가 그것을 감싸 확장한다.
	 * 설정자: viommu 를 만들 때. 읽는 자: 코어가 콜백을 부를 때 container_of 로 되짚는다. */
	struct protection_domain *parent; /* nest parent domain for this viommu */
	/* [한국어] 중첩 변환의 2단계(호스트) 도메인 (원 주석: nest parent domain).
	 * 왜 안전의 근거인가: 게스트가 만든 1단계 테이블이 가리키는 모든 주소가
	 *   이 도메인을 한 번 더 거친다. 그래서 호스트는 게스트 테이블의 내용을
	 *   검사하지 않아도 게스트가 자기 메모리 밖으로 나갈 수 없다.
	 * 설정자: viommu 생성 시 사용자가 지정한 부모 hwpt. */
	struct list_head pdom_list;	  /* For protection_domain->viommu_list */
	/* [한국어] 부모 도메인의 viommu_list 에 매다는 고리 (원 주석).
	 * 왜 필요한가: 부모 도메인의 매핑이 바뀌면 그것을 쓰는 모든 vIOMMU 의
	 *   게스트 도메인까지 무효화해야 하는데, 그 역방향 탐색에 이 목록을 쓴다. */

	/*
	 * Per-vIOMMU guest domain ID to host domain ID mapping.
	 * Indexed by guest domain ID.
	 */
	struct xarray gdomid_array;
	/* [한국어] 게스트 도메인 id → 호스트 도메인 id 대응표 (원 주석).
	 * 왜 번역이 필요한가: 게스트는 자기만의 도메인 id 공간을 쓰지만 하드웨어는
	 *   호스트가 발급한 id 만 알아듣는다. 게스트가 무효화 명령을 내면 그 안의
	 *   게스트 id 를 이 표로 호스트 id 로 바꿔 하드웨어에 전달한다.
	 * 인덱스가 곧 게스트 id 이므로 별도의 키가 없다. */
};

/*
 * Contains guest domain ID mapping info,
 * which is stored in the struct xarray gdomid_array.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct guest_domain_mapping_info — 게스트 도메인 id 하나의 호스트 대응
 *
 * xarray 에 게스트 id 를 키로 담긴다. 게스트의 여러 장치가 같은 게스트
 * 도메인을 쓰면 이 항목을 공유하므로, 언제 지워도 되는지 알려면 참조
 * 계수가 필요하다.
 */
struct guest_domain_mapping_info {
	refcount_t users;
	/* [한국어] 이 대응을 쓰는 게스트 장치 수.
	 * 게스트의 여러 장치가 같은 게스트 도메인을 쓰면 이 항목을 공유하므로,
	 *   마지막 하나가 떨어질 때까지 호스트 도메인 id 를 놓아서는 안 된다. */
	u32 hdom_id;		/* Host domain ID */
	/* [한국어] 대응되는 호스트 도메인 id (원 주석: Host domain ID).
	 * 읽는 자: 게스트가 낸 무효화 명령을 하드웨어 명령으로 바꿀 때. */
};

/*
 * Nested domain is specifically used for nested translation
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct nested_domain — 게스트가 만든 1단계 변환을 감싸는 도메인
 *
 * 중첩 변환에서 게스트의 DTE(gDTE)는 게스트가 채운다. 호스트는 그 내용을
 * 해석하지 않고 그대로 하드웨어에 전달하는데, 그래도 안전한 이유는 그것이
 * 가리키는 모든 주소가 viommu->parent 의 2단계 변환을 다시 거치기 때문이다.
 * 게스트가 아무 주소나 적어도 자기 메모리 밖으로는 나갈 수 없다.
 */
struct nested_domain {
	struct iommu_domain domain; /* generic domain handle used by iommu core code */
	/* [한국어] 코어 계층이 보는 도메인. union 인 이유는 바로 아래에 있다. */
	/* [한국어] 코어 IOMMU 계층이 다루는 부분 (원 주석: generic domain handle).
	 * 반드시 첫 필드여야 하는 것은 아니지만, container_of 로 되짚기 위해 필요하다. */
	u16 gdom_id;                /* domain ID from gDTE */
	/* [한국어] 게스트 DTE 에 적힌 도메인 id (원 주석: domain ID from gDTE).
	 * 게스트의 id 공간에 속하므로, 하드웨어에 쓰기 전에 gdomid_array 로 번역한다. */
	struct guest_domain_mapping_info *gdom_info;
	/* [한국어] 그 번역 결과를 담은 항목.
	 * 설정자: 도메인을 만들 때 gdomid_array 에서 얻거나 새로 만든다.
	 * 읽는 자: 무효화 명령을 만들 때 호스트 id 를 여기서 꺼낸다.
	 * 참조를 들고 있으므로, 이 도메인이 살아 있는 동안 대응이 사라지지 않는다. */
	struct iommu_hwpt_amd_guest gdte; /* Guest vIOMMU DTE */
	/* [한국어] 게스트가 채운 DTE 사본 (원 주석: Guest vIOMMU DTE).
	 * 호스트는 이 내용을 해석하지 않고 그대로 하드웨어에 전달한다.
	 * 설정자: 사용자가 hwpt 를 만들 때 넘긴 값. */
	struct amd_iommu_viommu *viommu;  /* AMD hw-viommu this nested domain belong to */
	/* [한국어] 이 중첩 도메인이 속한 vIOMMU (원 주석).
	 * 부모 도메인과 게스트 id 대응표에 이것을 통해 도달한다. */
};

/*
 * This structure contains generic data for  IOMMU protection domains
 * independent of their use.
 */
struct protection_domain {	/* [한국어] 아래 union 이 이 구조체의 시작이자 코어 계층과의 접점이다 */
	union {	/* [한국어] 한 도메인은 형식 하나만 쓰므로 넷을 겹쳐 둔다 */
		struct iommu_domain domain;	/* [한국어] 코어 IOMMU 계층이 보는 부분 */
		struct pt_iommu iommu;
		/* [한국어] 공용 페이지 테이블 계층이 보는 부분. */
		struct pt_iommu_amdv1 amdv1;
		/* [한국어] v1 형식일 때의 페이지 테이블 상태. */
		struct pt_iommu_x86_64 amdv2;
		/* [한국어] v2(x86-64 형식)일 때의 상태.
		 * 넷을 union 으로 겹치는 이유: 한 도메인은 형식 하나만 쓰고, 각 구조체가
		 *   자기 앞부분에 공통 부분을 품고 있어 어느 쪽으로 봐도 앞은 같다.
		 *   아래 PT_IOMMU_CHECK_DOMAIN 이 그 전제를 컴파일 시점에 검사한다. */
	};
	struct list_head dev_list; /* List of all devices in this domain */
	/* [한국어] 이 도메인에 붙은 모든 장치 (원 주석: List of all devices in this domain).
	 * 읽는 자: 도메인 설정이 바뀌면 모든 장치의 DTE 를 갱신해야 한다.
	 * 동기화: 아래 lock. */
	spinlock_t lock;	/* mostly used to lock the page table*/
	/* [한국어] 도메인의 상태를 지킨다 (원 주석: mostly used to lock the page table).
	 * 지키는 것: 페이지 테이블, dev_list, dev_data_list, iommu_array.
	 * 스핀락인 이유: 매핑 경로가 잠들 수 없는 문맥에서도 불린다. */
	u16 id;			/* the domain id written to the device table */
	/* [한국어] 장치 테이블에 써 넣는 도메인 id (원 주석).
	 * 하드웨어는 이 값으로 캐시를 구분하므로, 무효화 명령의 대상 지정에도 쓰인다.
	 * 값 범위: 0 ~ MAX_DOMAIN_ID-1. 전역 비트맵에서 할당한다. */
	enum protection_domain_mode pd_mode; /* Track page table type */
	/* [한국어] 이 도메인이 v1 인지 v2 인지 (원 주석: Track page table type).
	 * 읽는 자: DTE 를 채우는 코드와, SVA 가 가능한지 판단하는 곳. */
	bool dirty_tracking;	/* dirty tracking is enabled in the domain */
	/* [한국어] 더티 비트 추적이 켜져 있는가 (원 주석).
	 * 라이브 마이그레이션에서 "게스트가 어느 페이지를 고쳤는가"를 알기 위해 쓴다.
	 * 설정자: 사용자가 iommufd 로 켠다. 하드웨어의 HDSUP 지원이 전제다. */
	struct xarray iommu_array;	/* per-IOMMU reference count */
	/* [한국어] 유닛별 참조 수 (원 주석: per-IOMMU reference count).
	 * 담기는 값이 struct pdom_iommu_info 이고, 키는 유닛 인덱스다.
	 * 무효화를 보낼 유닛 목록이 곧 이 배열이다. */

	struct mmu_notifier mn;	/* mmu notifier for the SVA domain */
	/* [한국어] SVA 도메인에서 프로세스의 페이지 테이블 변경을 통지받는 고리 (원 주석).
	 * CPU 쪽에서 매핑이 사라지면 IOMMU 의 캐시도 지워야 하는데, 그 신호가
	 *   이 통지로 온다. SVA 가 아닌 도메인에서는 쓰이지 않는다. */
	struct list_head dev_data_list; /* List of pdom_dev_data */
	/* [한국어] {장치, PASID} 쌍의 목록 (원 주석: List of pdom_dev_data).
	 * dev_list 와 따로 있는 이유: 한 장치가 여러 PASID 로 붙을 수 있어
	 *   장치 목록만으로는 무효화 대상을 정확히 표현할 수 없다. */

	/*
	 * Store reference to list of vIOMMUs, which use this protection domain.
	 * This will be used to look up host domain ID when flushing this domain.
	 */
	struct list_head viommu_list;
	/* [한국어] (위 영어 주석에 이어) 이 도메인을 부모로 쓰는 vIOMMU 목록.
	 * 이 도메인을 플러시할 때 게스트 도메인 id 를 찾아내는 역방향 경로다.
	 * 중첩 변환에서 2단계 매핑이 바뀌면 그 위에 얹힌 게스트 도메인의 캐시도
	 *   함께 지워야 하기 때문에 필요하다. */
};
PT_IOMMU_CHECK_DOMAIN(struct protection_domain, iommu, domain);	/* [한국어] union 의 전제를 컴파일 시점에 검사한다 — pt_iommu 로 봐도 domain 이 같은 자리에 있어야 한다 */
PT_IOMMU_CHECK_DOMAIN(struct protection_domain, amdv1.iommu, domain);	/* [한국어] v1 형식에 대해서도 같은 검사 */
PT_IOMMU_CHECK_DOMAIN(struct protection_domain, amdv2.iommu, domain);	/* [한국어] v2 형식에 대해서도. 셋 중 하나라도 어긋나면 빌드가 멈춘다 */

/*
 * This structure contains information about one PCI segment in the system.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct amd_iommu_pci_seg — PCI 세그먼트 하나에 딸린 모든 조회 표
 *
 * 큰 시스템은 버스 번호 공간(세그먼트)을 여러 개 가질 수 있고, 세그먼트가
 * 다르면 같은 BDF 라도 다른 장치다. 그래서 BDF 를 인덱스로 쓰는 표들은
 * 전역이 될 수 없고 세그먼트마다 하나씩 있어야 한다.
 *
 * 여기 모인 네 개의 표가 AMD IOMMU 드라이버의 조회 경로 전부다:
 *  - dev_table: 하드웨어가 보는 표. 요청자 id → 도메인·페이지 테이블.
 *  - rlookup_table: 소프트웨어용. 장치 id → 담당 IOMMU 유닛.
 *  - irq_lookup_table: 장치 id → 인터럽트 재매핑 표.
 *  - alias_table: 장치 id → 실제 요청자 id. 브리지 뒤 장치가 자기 이름이
 *    아닌 다른 이름으로 요청을 내는 경우를 흡수한다.
 *
 * 넷 다 BDF 를 그대로 인덱스로 쓰는 평평한 배열이다. 해시나 트리를 쓰지
 * 않는 이유는 조회가 변환 경로와 인터럽트 경로의 핫패스에 있고, 인덱스
 * 공간이 16비트로 작아 배열이 가장 빠르기 때문이다.
 */
struct amd_iommu_pci_seg {
	/* List with all PCI segments in the system */
	struct list_head list;
	/* [한국어] 전역 세그먼트 목록에 매다는 고리 (원 주석).
	 * for_each_pci_segment 가 이 목록을 훑는다. */

	/* List of all available dev_data structures */
	struct llist_head dev_data_list;
	/* [한국어] 이 세그먼트에서 만들어진 모든 iommu_dev_data (원 주석).
	 * llist(무잠금 목록)인 이유: 장치 추가는 부팅과 핫플러그에서 일어나고
	 *   락 없이 밀어 넣을 수 있으면 초기화 경로가 단순해진다. 순회는 드물다. */

	/* PCI segment number */
	u16 id;
	/* [한국어] PCI 세그먼트 번호 (원 주석).
	 * 장치를 특정하는 키의 상위 절반이 이 값이다. */

	/* Largest PCI device id we expect translation requests for */
	u16 last_bdf;
	/* [한국어] 이 세그먼트에서 나타날 수 있는 가장 큰 장치 id (원 주석).
	 * 설정자: IVRS 표를 훑어 실제로 존재하는 최대값을 찾는다.
	 * 읽는 자: 아래 표들의 크기를 정한다 — 16비트 전체를 잡으면 메모리가
	 *   낭비되므로, 실제 필요한 만큼만 할당한다. */

	/* Size of the device table */
	u32 dev_table_size;
	/* [한국어] 장치 테이블의 크기 (원 주석). last_bdf 로부터 계산된다.
	 * 하드웨어에도 이 크기를 알려야 하므로 따로 보관한다. */

	/*
	 * device table virtual address
	 *
	 * Pointer to the per PCI segment device table.
	 * It is indexed by the PCI device id or the HT unit id and contains
	 * information about the domain the device belongs to as well as the
	 * page table root pointer.
	 */
	struct dev_table_entry *dev_table;
	/* [한국어] (위 영어 주석에 이어) 하드웨어가 직접 읽는 장치 테이블.
	 * 변환 사슬의 출발점이다. 하드웨어는 요청자 id 를 인덱스로 이 표를 읽고,
	 *   거기서 도메인 id 와 페이지 테이블 루트를 얻는다.
	 * 설정자: 장치를 도메인에 붙이고 뗄 때. 읽는 자: 하드웨어와 드라이버 양쪽.
	 * 동기화: 항목 갱신은 원자적으로 하고, 고친 뒤 반드시 DTE 캐시를 무효화한다. */

	/*
	 * The rlookup iommu table is used to find the IOMMU which is
	 * responsible for a specific device. It is indexed by the PCI
	 * device id.
	 */
	struct amd_iommu **rlookup_table;
	/* [한국어] (위 영어 주석에 이어) 장치 id → 담당 IOMMU 유닛.
	 * 하드웨어는 이 표를 쓰지 않는다 — 순수하게 드라이버가 "이 장치의 명령을
	 *   어느 유닛에 보낼까"를 O(1) 로 답하기 위한 것이다.
	 * 설정자: IVRS 표를 파싱하며 각 유닛이 담당하는 범위를 채운다. */

	/*
	 * This table is used to find the irq remapping table for a given
	 * device id quickly.
	 */
	struct irq_remap_table **irq_lookup_table;
	/* [한국어] (위 영어 주석에 이어) 장치 id → 그 장치의 인터럽트 재매핑 표.
	 * 별칭을 공유하는 장치들은 같은 표를 가리키므로, 이 배열의 여러 칸이
	 *   하나의 표를 가리킬 수 있다. */

	/*
	 * Pointer to a device table which the content of old device table
	 * will be copied to. It's only be used in kdump kernel.
	 */
	struct dev_table_entry *old_dev_tbl_cpy;
	/* [한국어] (위 영어 주석에 이어) kdump 에서 이전 커널의 장치 테이블을 복사해 둘 곳.
	 * 왜 필요한가: 크래시한 커널이 켜 둔 변환을 그대로 물려받아야 살아 있는
	 *   장치의 DMA 가 끊기지 않는다. 새 표를 쓰기 전에 옛 내용을 여기 옮겨 두고,
	 *   장치별로 하나씩 넘겨받는다.
	 * kdump 가 아니면 NULL 로 남는다. */

	/*
	 * The alias table is a driver specific data structure which contains the
	 * mappings of the PCI device ids to the actual requestor ids on the IOMMU.
	 * More than one device can share the same requestor id.
	 */
	u16 *alias_table;
	/* [한국어] (위 영어 주석에 이어) 장치 id → 실제 요청자 id.
	 * 왜 필요한가: PCIe-to-PCI 브리지 뒤의 장치는 브리지 이름으로 요청을 낸다.
	 *   하드웨어는 그 이름으로만 DTE 를 찾으므로, 드라이버도 설정할 때 같은
	 *   이름을 써야 한다. 원 주석대로 여러 장치가 한 요청자 id 를 공유할 수 있다.
	 * 설정자: IVRS 표 파싱과 PCI 별칭 순회. */

	/*
	 * A list of required unity mappings we find in ACPI. It is not locked
	 * because as runtime it is only read. It is created at ACPI table
	 * parsing time.
	 */
	struct list_head unity_map;
	/* [한국어] (위 영어 주석에 이어) 펌웨어가 요구한 항등 매핑 목록.
	 * 펌웨어나 특정 장치가 특정 물리 주소를 그대로 쓰기를 요구하는 구간이며,
	 *   도메인을 만들 때마다 이 매핑들을 먼저 넣어 준다.
	 * 동기화: 원 주석대로 ACPI 파싱 때 한 번 만들고 이후 읽기만 해서 락이 없다. */
};

/*
 * Structure where we save information about one hardware AMD IOMMU in the
 * system.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct amd_iommu — IOMMU 하드웨어 유닛 하나의 모든 것
 *
 * 이 드라이버의 중심 구조체다. 한 유닛이 갖는 세 종류의 자원이 여기 모여
 * 있다.
 *
 *  1) 하드웨어 접근 수단: MMIO 기준 주소, 능력 비트(cap/features), 담당
 *     세그먼트. 레지스터를 건드리는 모든 코드가 여기서 시작한다.
 *  2) 네 개의 링 버퍼: 명령 버퍼(드라이버 → 하드웨어), 이벤트 로그·PPR
 *     로그·GA 로그(하드웨어 → 드라이버). 방향이 반대라 머리/꼬리의 주인도
 *     반대다.
 *  3) 서스펜드/레주메를 위한 레지스터 사본. 원 주석이 밝히듯 BIOS 가
 *     복원해 주지 않아 드라이버가 직접 들고 있어야 한다.
 *
 * cmd_sem 이 이 구조체에서 가장 미묘한 부분이다. AMD 는 "명령이 끝났다"를
 * 알리는 방법이 하나뿐인데, 완료 대기 명령이 드라이버가 지정한 메모리에
 * 값을 쓰게 하는 것이다. 그래서 드라이버는 자기가 넘긴 주소를 폴링해
 * 완료를 안다 — Intel 의 QI 와 같은 구조다.
 */
struct amd_iommu {
	struct list_head list;
	/* [한국어] 전역 IOMMU 목록에 매다는 고리. for_each_iommu 가 이 목록을 훑는다. */

	/* Index within the IOMMU array */
	int index;
	/* [한국어] IOMMU 배열에서의 위치 (원 주석: Index within the IOMMU array).
	 * 도메인의 iommu_array 에서 키로 쓰이고, 로그 메시지의 유닛 번호이기도 하다. */

	/* locks the accesses to the hardware */
	raw_spinlock_t lock;
	/* [한국어] 하드웨어 접근을 직렬화한다 (원 주석: locks the accesses to the hardware).
	 * 지키는 것: 명령 버퍼의 꼬리 포인터와 레지스터 쓰기.
	 * raw 인 이유: 무효화가 잠들 수 없는 문맥에서도 불린다. */

	/* Pointer to PCI device of this IOMMU */
	struct pci_dev *dev;
	/* [한국어] 이 IOMMU 자신의 PCI 장치 (원 주석). IOMMU 도 PCI 장치로 열거된다.
	 * 읽는 자: 설정 공간 접근과 MSI 설정. */

	/* Cache pdev to root device for resume quirks */
	struct pci_dev *root_pdev;
	/* [한국어] 루트 콤플렉스 장치의 캐시 (원 주석: for resume quirks).
	 * 레주메 때 일부 레지스터를 루트 장치를 통해 되살려야 하는 기종이 있어,
	 *   그때마다 찾지 않고 미리 잡아 둔다. */

	/* physical address of MMIO space */
	u64 mmio_phys;
	/* [한국어] MMIO 영역의 물리 시작 주소 (원 주석). */

	/* physical end address of MMIO space */
	u64 mmio_phys_end;
	/* [한국어] 그 영역의 끝 (원 주석). 매핑 크기를 정하는 데 쓴다. */

	/* virtual address of MMIO space */
	u8 __iomem *mmio_base;
	/* [한국어] 매핑된 MMIO 의 가상 주소 (원 주석).
	 * 이 파일의 MMIO_*_OFFSET 상수들이 모두 이 주소를 기준으로 한다. */

	/* capabilities of that IOMMU read from ACPI */
	u32 cap;
	/* [한국어] ACPI 에서 읽은 능력 값 (원 주석).
	 * 담당 장치 범위와 MSI 번호가 여기 들어 있다. */

	/* flags read from acpi table */
	u8 acpi_flags;
	/* [한국어] ACPI 표가 지정한 플래그 (원 주석).
	 * 패스스루 요구나 특정 인터럽트 통과 설정 같은 펌웨어의 지시다. */

	/* Extended features */
	u64 features;
	/* [한국어] 확장 기능 비트맵 (원 주석: Extended features).
	 * FEATURE_* 상수로 검사한다. 이 유닛이 무엇을 할 수 있는지의 전부다. */

	/* Extended features 2 */
	u64 features2;
	/* [한국어] 두 번째 확장 기능 워드 (원 주석). 비트가 모자라 늘어난 것이다. */

	/* PCI device id of the IOMMU device */
	u16 devid;
	/* [한국어] 이 IOMMU 자신의 요청자 id (원 주석).
	 * IOMMU 도 DMA 를 내므로(테이블 읽기) 자기 id 를 갖는다. */

	/*
	 * Capability pointer. There could be more than one IOMMU per PCI
	 * device function if there are more than one AMD IOMMU capability
	 * pointers.
	 */
	u16 cap_ptr;
	/* [한국어] (위 영어 주석에 이어) PCI 설정 공간에서 이 IOMMU 능력 구조의 위치.
	 * 원 주석대로 한 PCI 함수에 IOMMU 능력이 여럿 있을 수 있어, 유닛마다
	 *   자기 능력 구조의 위치를 따로 들고 있어야 한다. */

	/* pci domain of this IOMMU */
	struct amd_iommu_pci_seg *pci_seg;
	/* [한국어] 이 유닛이 속한 PCI 세그먼트 (원 주석).
	 * 장치 테이블과 조회 표들이 거기 있으므로, 사실상 유닛의 작업 대상 전체다. */

	/* start of exclusion range of that IOMMU */
	u64 exclusion_start;
	/* [한국어] 제외 범위의 시작 (원 주석).
	 * 이 구간의 DMA 는 변환을 거치지 않고 그대로 통과한다. 펌웨어가 예약한
	 *   메모리를 장치가 직접 써야 할 때 쓴다. */
	/* length of exclusion range of that IOMMU */
	u64 exclusion_length;
	/* [한국어] 그 범위의 길이 (원 주석). */

	/* command buffer virtual address */
	u8 *cmd_buf;
	/* [한국어] 명령 버퍼의 가상 주소 (원 주석).
	 * 드라이버 → 하드웨어 방향의 링이다. 무효화와 완료 대기가 여기 실린다. */
	u32 cmd_buf_head;
	/* [한국어] 드라이버가 기억하는 머리 위치.
	 * 하드웨어의 머리 레지스터를 매번 읽는 대신 사본을 두어 MMIO 읽기를 줄인다. */
	u32 cmd_buf_tail;
	/* [한국어] 드라이버가 다음에 쓸 자리.
	 * 이 값을 하드웨어의 꼬리 레지스터에 써야 명령이 실제로 시작된다. */

	/* event buffer virtual address */
	u8 *evt_buf;
	/* [한국어] 이벤트 로그의 가상 주소 (원 주석).
	 * 하드웨어 → 드라이버 방향이다. 변환 실패와 오류가 여기 쌓인다. */

	/* Name for event log interrupt */
	unsigned char evt_irq_name[16];
	/* [한국어] 이벤트 로그 인터럽트의 이름 (원 주석).
	 * /proc/interrupts 에 표시되므로 유닛을 구분할 수 있게 번호를 넣는다.
	 * 문자열 자체를 들고 있어야 하는 이유: request_irq 는 이름 포인터를
	 *   보관만 하고 복사하지 않는다. */

	/* Base of the PPR log, if present */
	u8 *ppr_log;
	/* [한국어] PPR 로그의 시작 (원 주석: if present).
	 * 장치가 낸 페이지 폴트가 여기 쌓인다. 하드웨어가 PPR 을 지원할 때만 있다. */

	/* Name for PPR log interrupt */
	unsigned char ppr_irq_name[16];
	/* [한국어] PPR 로그 인터럽트의 이름 (원 주석). */

	/* Base of the GA log, if present */
	u8 *ga_log;
	/* [한국어] GA 로그의 시작 (원 주석: if present).
	 * 게스트에 직접 전달하지 못한 인터럽트가 기록되고, KVM 이 그것을 보고
	 *   대신 주입한다. */

	/* Name for GA log interrupt */
	unsigned char ga_irq_name[16];
	/* [한국어] GA 로그 인터럽트의 이름 (원 주석). */

	/* Tail of the GA log, if present */
	u8 *ga_log_tail;
	/* [한국어] GA 로그의 꼬리 포인터를 담을 메모리 (원 주석).
	 * 다른 로그와 달리 하드웨어가 꼬리 값을 레지스터가 아니라 메모리에 쓴다. */

	/* true if interrupts for this IOMMU are already enabled */
	bool int_enabled;
	/* [한국어] 이 유닛의 인터럽트를 이미 켰는가 (원 주석).
	 * 초기화가 여러 경로에서 불릴 수 있어 중복 등록을 막는다. */

	/* if one, we need to send a completion wait command */
	bool need_sync;
	/* [한국어] (원 주석: if one, we need to send a completion wait command)
	 * 마지막 명령 이후 완료 대기를 보내지 않았다는 표시.
	 * 왜 매번 보내지 않는가: 완료 대기는 하드웨어를 기다리게 하므로 비싸다.
	 *   명령을 여러 개 넣은 뒤 마지막에 한 번만 보내는 것이 훨씬 빠르다. */

	/* true if disable irte caching */
	bool irtcachedis_enabled;
	/* [한국어] 인터럽트 재매핑 캐시를 껐는가 (원 주석).
	 * 특정 하드웨어의 errata 를 피하기 위한 설정이다. */

	/* Handle for IOMMU core code */
	struct iommu_device iommu;
	/* [한국어] 코어 IOMMU 계층이 보는 부분 (원 주석: Handle for IOMMU core code).
	 * dev_to_amd_iommu 가 이것으로부터 바깥 구조체를 되짚는다. */

	/*
	 * We can't rely on the BIOS to restore all values on reinit, so we
	 * need to stash them
	 */

	/* The iommu BAR */
	u32 stored_addr_lo;
	/* [한국어] (위 영어 주석에 이어) IOMMU BAR 의 하위 32비트 사본.
	 * 원 주석대로 BIOS 가 레주메 때 복원해 준다고 믿을 수 없어 직접 보관한다. */
	u32 stored_addr_hi;
	/* [한국어] 같은 BAR 의 상위 32비트. */

	/*
	 * Each iommu has 6 l1s, each of which is documented as having 0x12
	 * registers
	 */
	u32 stored_l1[6][0x12];
	/* [한국어] (위 영어 주석에 이어) L1 간접 레지스터의 사본.
	 * IOMMU 마다 L1 이 여섯 벌 있고 각각 0x12 개의 레지스터를 갖는다.
	 * 서스펜드 전에 전부 읽어 두었다가 레주메 때 되쓴다. */

	/* The l2 indirect registers */
	u32 stored_l2[0x83];
	/* [한국어] L2 간접 레지스터의 사본 (원 주석). */

	/* The maximum PC banks and counters/bank (PCSup=1) */
	u8 max_banks;
	/* [한국어] 성능 카운터 뱅크 수 (원 주석: PCSup=1 일 때). */
	u8 max_counters;
	/* [한국어] 뱅크당 카운터 수 (원 주석). */
#ifdef CONFIG_IRQ_REMAP
	struct irq_domain *ir_domain;
	/* [한국어] 이 유닛의 인터럽트 재매핑 도메인.
	 * 장치의 MSI 가 이 도메인을 부모로 삼아 반드시 재매핑을 거치게 된다. */

	struct amd_irte_ops *irte_ops;
	/* [한국어] IRTE 형식별 조작 함수 표.
	 * 기본 형식과 GA(게스트 전달) 형식이 필드 배치가 달라, 같은 코드로 다룰 수
	 *   없다. 형식마다 함수 묶음을 두고 여기서 골라 쓴다. */
#endif

	u32 flags;
	/* [한국어] 드라이버 내부 상태 플래그. AMD_IOMMU_FLAG_TRANS_PRE_ENABLED 등.
	 * kdump 로 물려받은 상태인지를 여기서 기억한다. */
	volatile u64 *cmd_sem;
	/* [한국어] 완료 대기 명령이 값을 써 넣을 메모리.
	 * 드라이버가 이 값을 폴링해 "앞선 명령이 모두 끝났다"를 안다 — AMD 에서
	 *   완료를 아는 유일한 방법이다.
	 * volatile 인 이유: 값을 바꾸는 것이 CPU 가 아니라 하드웨어라, 컴파일러가
	 *   루프 밖으로 읽기를 끌어내면 영원히 끝나지 않는다. */
	u64 cmd_sem_val;
	/* [한국어] 다음 완료 대기에 쓸 값. 매번 증가시킨다.
	 * 왜 고정값이 아닌가: 이전 완료의 흔적과 구별해야 한다. 같은 값을 계속
	 *   쓰면 옛 완료를 새 완료로 착각할 수 있다. */
	/*
	 * Track physical address to directly use it in build_completion_wait()
	 * and avoid adding any special checks and handling for kdump.
	 */
	u64 cmd_sem_paddr;
	/* [한국어] (위 영어 주석에 이어) 그 메모리의 물리 주소.
	 * 명령에는 물리 주소를 실어야 하는데, kdump 커널에서는 virt_to_phys 가
	 *   기대대로 동작하지 않는 경우가 있어 미리 구해 둔 값을 그대로 쓴다. */

#ifdef CONFIG_AMD_IOMMU_DEBUGFS
	/* DebugFS Info */
	struct dentry *debugfs;
	/* [한국어] 이 유닛의 debugfs 디렉터리 (원 주석: DebugFS Info). */
	int dbg_mmio_offset;
	/* [한국어] debugfs 로 읽을 MMIO 오프셋. 사용자가 쓰면 그 위치를 다음에 읽는다. */
	int dbg_cap_offset;
	/* [한국어] 같은 방식의 PCI 능력 공간 오프셋. */
#endif

	/* IOPF support */
	struct iopf_queue *iopf_queue;
	/* [한국어] 페이지 폴트를 처리하는 작업 큐 (원 주석: IOPF support).
	 * PPR 로그에서 꺼낸 요청을 여기 넣으면 잠들 수 있는 문맥에서 처리된다 —
	 *   페이지를 실제로 가져오는 일은 인터럽트 문맥에서 할 수 없기 때문이다. */
	unsigned char iopfq_name[32];
	/* [한국어] 그 큐의 이름. 워커 스레드 이름으로 보인다. */
};

/*
 * [한국어]
 * dev_to_amd_iommu - sysfs 의 device 로부터 IOMMU 유닛 구조체를 되찾는다
 *
 * @dev: 코어가 IOMMU 마다 만들어 준 device.
 * @return: 그 device 에 대응하는 struct amd_iommu.
 *
 * 코어 IOMMU 계층은 유닛을 struct iommu_device 로만 알고, sysfs 콜백에는
 * struct device 만 넘어온다. 두 단계 container_of 로 벤더 구조체까지 되짚는
 * 것이 유일한 경로다.
 *
 * 호출 체인:
 *   sysfs 속성 콜백 → [이 함수] → dev_to_iommu_device()
 */
static inline struct amd_iommu *dev_to_amd_iommu(struct device *dev)
{
	struct iommu_device *iommu = dev_to_iommu_device(dev);	/* [한국어] device 에서 코어가 아는 IOMMU 객체를 얻고 */

	return container_of(iommu, struct amd_iommu, iommu);	/* [한국어] 거기서 벤더 구조체로 되짚는다 */
}

#define ACPIHID_UID_LEN 256	/* [한국어] ACPI UID 문자열의 최대 길이 */
#define ACPIHID_HID_LEN 9	/* [한국어] ACPI HID 문자열의 최대 길이(8자 + 종료 문자) */

/*
 * [한국어] struct acpihid_map_entry — ACPI HID 장치와 IOMMU 요청자 id 의 대응
 *
 * PCI 장치가 아닌 플랫폼 장치(ACPI 로만 열거되는 것들)도 DMA 를 낸다. 그런
 * 장치에는 BDF 가 없으므로, 펌웨어가 "이 HID/UID 의 장치는 이 요청자 id 로
 * 나타난다"고 알려 주어야 IOMMU 가 그것을 식별할 수 있다.
 *
 * 이 목록이 그 대응이며, IVRS 표 파싱이나 커널 명령줄(cmd_line)에서 채워진다.
 */
struct acpihid_map_entry {
	struct list_head list;
	/* [한국어] acpihid_map 에 매다는 고리. */
	u8 uid[ACPIHID_UID_LEN];
	/* [한국어] ACPI UID 문자열. 같은 HID 의 여러 장치를 구별한다.
	 * 설정자: IVRS 파싱 또는 커널 명령줄.
	 * 읽는 자: 장치를 IOMMU 에 붙일 때 이 값으로 대조한다. */
	u8 hid[ACPIHID_HID_LEN];
	/* [한국어] ACPI HID 문자열. 장치의 종류를 나타낸다.
	 * uid 와 함께 플랫폼 장치를 유일하게 지목하는 이름이 된다. */
	u32 devid;
	/* [한국어] 그 장치가 IOMMU 에 나타나는 요청자 id.
	 * PCI 장치가 아니어서 BDF 가 없으므로, 펌웨어가 알려 준 이 값이 유일한 단서다.
	 * 읽는 자: 이 장치의 DTE 를 찾을 때. */
	u32 root_devid;
	/* [한국어] 그 장치가 속한 루트 장치의 id.
	 * 일부 플랫폼 장치는 상위 장치를 통해 DMA 를 내므로, 하드웨어에는 그
	 *   상위 장치의 이름으로 나타난다. 설정도 그 이름으로 해야 한다. */
	bool cmd_line;
	/* [한국어] 이 항목이 커널 명령줄에서 온 것인지.
	 * 펌웨어 표가 잘못된 기계에서 사용자가 직접 대응을 지정할 수 있고,
	 *   그 경우 표의 값보다 우선한다. 이 표시가 그 우선순위의 근거다. */
	struct iommu_group *group;
	/* [한국어] 이 장치가 속한 IOMMU 그룹.
	 * 격리 단위가 같은 장치들을 묶는다 — 함께 붙거나 함께 떨어져야 한다.
	 * 설정자: 장치를 처음 볼 때 그룹을 만들거나 기존 그룹에 넣는다. */
};

/*
 * [한국어] struct devid_map — IOAPIC/HPET 의 id 와 요청자 id 의 대응
 *
 * acpihid_map_entry 와 같은 문제를 더 단순한 형태로 푼다. IOAPIC 과 HPET 은
 * 인터럽트를 내지만 PCI 장치가 아니어서, 그 인터럽트가 하드웨어에 어떤
 * 요청자 id 로 보이는지를 펌웨어가 알려 줘야 한다.
 *
 * 이 대응이 없으면 그 인터럽트의 재매핑 항목을 만들 수 없다.
 */
struct devid_map {
	struct list_head list;
	/* [한국어] ioapic_map 또는 hpet_map 에 매다는 고리. */
	u8 id;
	/* [한국어] IOAPIC 번호 또는 HPET 블록 번호. 조회의 키다. */
	u32 devid;
	/* [한국어] 그 장치가 IOMMU 에 나타나는 요청자 id.
	 * PCI 장치가 아니어서 BDF 가 없으므로, 펌웨어가 알려 준 이 값이 유일한 단서다. */
	bool cmd_line;
	/* [한국어] 이 항목이 커널 명령줄에서 온 것인지.
	 * 펌웨어 표가 잘못된 기계에서 사용자가 직접 대응을 지정할 수 있고,
	 *   그 경우 표의 값보다 우선한다. 이 표시가 그 우선순위의 근거다. */
};

#define AMD_IOMMU_DEVICE_FLAG_ATS_SUP     0x1    /* ATS feature supported */	/* [한국어] 장치가 ATS 를 지원한다 (원 주석). 변환 결과를 장치가 캐시할 수 있다 */
#define AMD_IOMMU_DEVICE_FLAG_PRI_SUP     0x2    /* PRI feature supported */	/* [한국어] 장치가 PRI 를 지원한다 (원 주석). 페이지 폴트를 보고할 수 있다 — SVA 의 전제 */
#define AMD_IOMMU_DEVICE_FLAG_PASID_SUP   0x4    /* PASID context supported */	/* [한국어] 장치가 PASID 를 요청에 실을 수 있다 (원 주석) */
/* Device may request execution on memory pages */
#define AMD_IOMMU_DEVICE_FLAG_EXEC_SUP    0x8	/* [한국어] 장치가 실행 권한을 요청할 수 있다 (원 주석) */
/* Device may request super-user privileges */
#define AMD_IOMMU_DEVICE_FLAG_PRIV_SUP   0x10	/* [한국어] 장치가 커널 권한의 접근을 요청할 수 있다 (원 주석) */

/*
 * This struct contains device specific data for the IOMMU
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct iommu_dev_data — 장치 하나의 IOMMU 쪽 상태 전부
 *
 * struct device 의 iommu 프라이빗 포인터에 매달려, "이 장치가 어느 도메인에
 * 속하고 어떤 기능을 켰는가"를 담는다. DTE 를 채우는 코드가 참조하는 값이
 * 대부분 여기서 온다.
 *
 * 세 부류의 정보가 있다:
 *  - 소속: domain, list(도메인의 장치 목록에 매다는 고리), devid.
 *  - 능력과 상태: ats/pri/pasid 를 지원하는가(flags), 실제로 켰는가
 *    (*_enabled 비트필드). 지원과 활성화를 나눠 두는 이유는 지원해도
 *    켜지 않는 경우가 흔하기 때문이다.
 *  - PASID 자원: gcr3_info. PASID 별 페이지 테이블이 장치 단위로 달리므로
 *    여기 놓인다.
 *
 * 락이 둘인 것이 눈에 띈다. mutex 는 붙이고 떼는 긴 경로를, dte_lock 은
 * 256비트 DTE 를 여러 조각으로 나눠 쓰는 짧은 구간을 지킨다 — 후자는
 * 인터럽트 문맥에서도 잡힐 수 있어 스핀락이어야 한다.
 */
struct iommu_dev_data {
	/*Protect against attach/detach races */
	struct mutex mutex;
	/* [한국어] 장치를 도메인에 붙이고 떼는 경쟁을 막는다 (원 주석).
	 * 뮤텍스인 이유: 붙이는 과정에 페이지 테이블 할당 등 잠들 수 있는 일이 있다. */
	spinlock_t dte_lock;              /* DTE lock for 256-bit access */
	/* [한국어] DTE 를 갱신하는 짧은 구간을 지킨다 (원 주석: for 256-bit access).
	 * 왜 별도의 락인가: DTE 는 256비트라 한 번에 쓸 수 없고 조각으로 나눠
	 *   써야 한다. 그 사이 다른 CPU 가 끼어들면 하드웨어가 반쯤 갱신된
	 *   항목을 읽는다. 이 경로는 인터럽트 문맥에서도 불릴 수 있어 스핀락이다. */

	struct list_head list;		  /* For domain->dev_list */
	/* [한국어] ioapic_map 또는 hpet_map 에 매다는 고리. */
	/* [한국어] 도메인의 dev_list 에 매다는 고리 (원 주석: For domain->dev_list). */
	struct llist_node dev_data_list;  /* For global dev_data_list */
	/* [한국어] 세그먼트의 전역 목록에 매다는 고리 (원 주석).
	 * llist 라 락 없이 밀어 넣을 수 있다. */
	struct protection_domain *domain; /* Domain the device is bound to */
	/* [한국어] 이 장치가 붙어 있는 도메인 (원 주석: Domain the device is bound to).
	 * NULL 이면 아직 어느 도메인에도 붙지 않았다 — 그 상태에서는 DMA 가 차단된다. */
	struct gcr3_tbl_info gcr3_info;   /* Per-device GCR3 table */
	/* [한국어] PASID 별 페이지 테이블 대응표 (원 주석: Per-device GCR3 table).
	 * 장치 단위인 이유: DTE 가 장치마다 하나이고 GCR3 주소를 거기 적기 때문이다. */
	struct device *dev;
	/* [한국어] 커널의 장치 객체. 로그와 DMA 속성 조회에 쓴다. */
	u16 devid;			  /* PCI Device ID */
	/* [한국어] 이 장치의 요청자 id (원 주석: PCI Device ID).
	 * 별칭이 있는 장치는 별칭 id 가 들어간다 — 하드웨어가 그 이름으로 보기 때문이다. */

	unsigned int max_irqs;		  /* Maximum IRQs supported by device */
	/* [한국어] 이 장치가 쓸 수 있는 인터럽트 수 (원 주석).
	 * 재매핑 표의 크기를 정하는 근거다. 512 또는 2048. */
	u32 max_pasids;			  /* Max supported PASIDs */
	/* [한국어] 이 장치가 지원하는 최대 PASID 수 (원 주석).
	 * PCI PASID 능력에서 읽는다. GCR3 표의 레벨 수를 정하는 데 쓴다. */
	u32 flags;			  /* Holds AMD_IOMMU_DEVICE_FLAG_<*> */
	/* [한국어] 장치가 무엇을 지원하는가 (원 주석: AMD_IOMMU_DEVICE_FLAG_<*>).
	 * 아래 *_enabled 와 짝을 이룬다 — 이쪽은 능력, 저쪽은 실제 활성화다. */
	int ats_qdep;
	/* [한국어] ATS 무효화 큐의 깊이.
	 * 무효화 명령에 실어 보내야 하는 값으로, 장치가 한 번에 몇 개의 무효화를
	 *   받아들일 수 있는지를 나타낸다. PCI ATS 능력에서 읽는다. */
	u8 ats_enabled  :1;		  /* ATS state */
	/* [한국어] ATS 를 실제로 켰는가 (원 주석: ATS state).
	 * 켜져 있으면 unmap 마다 장치 쪽 캐시도 지워야 한다. */
	u8 pri_enabled  :1;		  /* PRI state */
	/* [한국어] PRI 를 실제로 켰는가 (원 주석: PRI state). */
	u8 pasid_enabled:1;		  /* PASID state */
	/* [한국어] PASID 를 실제로 켰는가 (원 주석: PASID state). */
	u8 pri_tlp      :1;		  /* PASID TLB required for
					     PPR completions */
	/* [한국어] (원 주석: PASID TLB required for PPR completions)
	 * PPR 응답을 보낼 때 PASID 를 함께 실어야 하는 장치인지.
	 * 장치마다 요구가 달라 응답 명령의 형식이 갈린다. */
	u8 ppr          :1;		  /* Enable device PPR support */
	/* [한국어] 이 장치의 PPR 지원을 켰는가 (원 주석). */
	bool use_vapic;			  /* Enable device to use vapic mode */
	/* [한국어] 이 장치의 인터럽트를 게스트에 직접 전달하는가 (원 주석: vapic mode).
	 * KVM 이 장치를 게스트에 넘길 때 켜며, IRTE 형식이 GA 형식으로 바뀐다. */
	bool defer_attach;
	/* [한국어] 도메인 붙이기를 뒤로 미뤄야 하는가.
	 * 장치가 아직 준비되지 않았거나 그룹의 다른 장치를 기다려야 할 때 선다.
	 * 읽는 자: 실제 DMA 가 필요해지는 시점에 이 표시를 보고 그때 붙인다. */

	struct ratelimit_state rs;        /* Ratelimit IOPF messages */
	/* [한국어] 페이지 폴트 로그의 속도 제한 (원 주석: Ratelimit IOPF messages).
	 * 왜 필요한가: 잘못 동작하는 장치는 초당 수만 건의 폴트를 낼 수 있고,
	 *   그것을 모두 찍으면 로그가 시스템을 멈춘다. */
};

/* Map HPET and IOAPIC ids to the devid used by the IOMMU */
extern struct list_head ioapic_map;	/* [한국어] IOAPIC id → 요청자 id 대응 목록 (원 주석) */
extern struct list_head hpet_map;	/* [한국어] HPET 에 대한 같은 목록 */
extern struct list_head acpihid_map;	/* [한국어] ACPI HID 장치에 대한 같은 목록 */

/*
 * List with all PCI segments in the system. This list is not locked because
 * it is only written at driver initialization time
 */
extern struct list_head amd_iommu_pci_seg_list;	/* [한국어] 모든 PCI 세그먼트 목록. 원 주석대로 초기화 때만 쓰므로 락이 없다 */

/*
 * List with all IOMMUs in the system. This list is not locked because it is
 * only written and read at driver initialization or suspend time
 */
extern struct list_head amd_iommu_list;	/* [한국어] 모든 IOMMU 유닛 목록. 같은 이유로 락이 없다 */

/*
 * Structure defining one entry in the device table
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct dev_table_entry — 장치 테이블 항목 하나 (256비트)
 *
 * 하드웨어가 요청자 id 로 찾아오는 최초의 자료구조다. 여기에 유효 비트,
 * 변환 활성화, 페이지 테이블 루트, 도메인 id, 인터럽트 재매핑 표 주소,
 * 게스트 CR3 주소가 모두 들어 있다 — DMA 와 인터럽트 두 경로가 이 하나의
 * 항목을 공유하는 것이 AMD 설계의 특징이다.
 *
 * 구조체가 아니라 u64 배열인 이유: 필드가 비트 단위로 흩어져 있고 일부는
 * 여러 조각에 나뉘어 있어(GCR3 주소가 세 조각인 것처럼) 비트필드로 표현할
 * 수 없다. DTE_* 마스크로 다룬다.
 *
 * data128 이 함께 있는 이유: 128비트 단위로 원자적으로 갱신해야 하는 경우가
 * 있다. 항목이 256비트라 한 번에 쓸 수 없으므로, 하드웨어가 중간 상태를
 * 보지 않도록 순서와 원자성을 신경 써야 한다 — dte_lock 이 그 때문에 있다.
 */
struct dev_table_entry {
	union {	/* [한국어] 같은 256비트를 64비트 넷 또는 128비트 둘로 본다 */
		u64 data[4];
		/* [한국어] 256비트 항목을 64비트 넷으로 본다. DTE_* 마스크가 이 배열을 다룬다.
		 * 설정자/읽는 자: 장치를 붙이고 뗄 때의 드라이버, 그리고 하드웨어. */
		u128 data128[2];
		/* [한국어] 같은 항목을 128비트 둘로 본다.
		 * 왜 필요한가: 일부 갱신은 128비트를 원자적으로 바꿔야 한다. 하드웨어가
		 *   반쯤 갱신된 항목을 읽으면 존재하지 않는 페이지 테이블을 따라간다. */
	};
};

/*
 * Structure defining one entry in the command buffer
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct iommu_cmd — 명령 버퍼 항목 하나 (128비트)
 *
 * 무효화와 완료 대기가 모두 이 형식이다. 첫 워드의 상위 비트가 명령 종류
 * (CMD_*)이고 나머지는 종류마다 뜻이 다르다.
 *
 * 크기가 고정이라 링 버퍼의 인덱스 계산이 단순하다 — Intel 의 서술자가
 * 모드에 따라 16/32바이트로 달라지는 것과 대조적이다.
 */
struct iommu_cmd {
	u32 data[4];
	/* [한국어] 명령 하나가 128비트. 첫 워드의 상위가 명령 종류이고 나머지는 종류마다 뜻이 다르다.
	 * 설정자: build_* 계열 함수가 채운다. 읽는 자: 하드웨어가 명령 버퍼에서 읽는다. */
};

/*
 * Structure to sture persistent DTE flags from IVHD
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct ivhd_dte_flags — 펌웨어가 지시한 DTE 플래그 재정의
 *
 * IVRS 표는 특정 장치 범위에 대해 "이 장치들의 DTE 는 이런 플래그로
 * 설정하라"고 지시할 수 있다. 예를 들어 NMI 를 재매핑 없이 통과시켜야 하는
 * 장치나, 시스템 관리 메시지를 특별히 다뤄야 하는 장치가 그렇다.
 *
 * 그 지시를 장치를 붙일 때마다 IVRS 표를 다시 읽어 확인할 수는 없으므로,
 * 파싱 때 이 목록으로 옮겨 두고 DTE 를 만들 때마다 참조한다.
 *
 * 범위(devid_first ~ devid_last)로 표현되는 이유: 표가 개별 장치가 아니라
 * 범위 단위로 지시할 수 있기 때문이다.
 */
struct ivhd_dte_flags {
	struct list_head list;
	/* [한국어] amd_ivhd_dev_flags_list 에 매다는 고리. */
	u16 segid;
	/* [한국어] 이 지시가 적용되는 PCI 세그먼트. */
	u16 devid_first;
	/* [한국어] 적용 범위의 첫 장치 id. */
	u16 devid_last;
	/* [한국어] 적용 범위의 마지막 장치 id.
	 * 범위인 이유: IVRS 표가 개별 장치가 아니라 범위 단위로 지시할 수 있다. */
	struct dev_table_entry dte;
	/* [한국어] 그 범위에 적용할 DTE 플래그.
	 * 항목 전체가 아니라 플래그 비트만 의미가 있으며, DTE 를 만들 때 OR 로 얹는다. */
};

/*
 * One entry for unity mappings parsed out of the ACPI table.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct unity_map_entry — 펌웨어가 요구한 항등 매핑 구간
 *
 * 어떤 장치는 특정 물리 주소를 그대로 써야 한다. 펌웨어가 그 장치와 통신하는
 * 버퍼가 고정 주소에 있거나, 부팅 전부터 진행 중인 DMA 가 있는 경우다.
 * IOMMU 를 켜면서 그 주소가 갑자기 변환 대상이 되면 그 통신이 끊긴다.
 *
 * 그래서 IVRS 표는 "이 장치 범위는 이 주소 구간을 IOVA == PA 로 매핑해
 * 두라"고 요구하고, 도메인을 만들 때마다 그 매핑을 먼저 넣어 준다.
 *
 * 이것이 IOMMU 격리의 예외 구멍이기도 하다 — 그 구간만큼은 장치가 물리
 * 메모리에 직접 닿는다.
 */
struct unity_map_entry {
	struct list_head list;
	/* [한국어] 세그먼트의 unity_map 목록에 매다는 고리.
	 * 도메인을 만들 때마다 이 목록을 훑어 필요한 항등 매핑을 넣는다. */

	/* starting device id this entry is used for (including) */
	u16 devid_start;
	/* [한국어] 이 매핑이 적용되는 첫 장치 id (원 주석: including). */
	/* end device id this entry is used for (including) */
	u16 devid_end;
	/* [한국어] 마지막 장치 id (원 주석: including). */

	/* start address to unity map (including) */
	u64 address_start;
	/* [한국어] 항등 매핑할 주소 구간의 시작 (원 주석: including).
	 * IOVA 와 물리 주소가 같아야 하는 구간이다. */
	/* end address to unity map (including) */
	u64 address_end;
	/* [한국어] 그 구간의 끝 (원 주석: including). */

	/* required protection */
	int prot;
	/* [한국어] 그 매핑에 줄 권한 (원 주석: required protection).
	 * IOMMU_PROT_IR/IW 조합. 읽기만 필요한 구간에 쓰기를 주지 않는다. */
};

/*
 * Data structures for device handling
 */

extern bool amd_iommu_force_isolation;	/* [한국어] 장치를 그룹으로 묶지 않고 하나씩 격리할지. 보안을 위해 성능을 내주는 옵션이다 */

/* Max levels of glxval supported */
extern int amd_iommu_max_glx_val;	/* [한국어] GCR3 표의 최대 레벨 수 (원 주석). 모든 유닛이 지원하는 값 중 가장 작은 것 */

/* IDA to track protection domain IDs */
extern struct ida pdom_ids;	/* [한국어] 도메인 id 할당기 (원 주석). 16비트 공간을 재사용 가능하게 관리한다 */

/* Global EFR and EFR2 registers */
extern u64 amd_iommu_efr;	/* [한국어] 모든 유닛에 공통인 확장 기능 (원 주석). 유닛마다 다르면 공통분만 남긴다 */
extern u64 amd_iommu_efr2;	/* [한국어] 두 번째 기능 워드의 공통분 */

/*
 * [한국어]
 * get_ioapic_devid - IOAPIC id 로 그 인터럽트의 요청자 id 를 찾는다
 *
 * @id: ACPI MADT 가 부여한 IOAPIC 번호.
 * @return: 그 IOAPIC 의 요청자 id, 모르면 -EINVAL.
 *
 * IOAPIC 은 PCI 장치가 아니어서 스스로 요청자 id 를 갖지 않는다. 대신
 * 펌웨어가 IVRS 표에 "이 IOAPIC 의 인터럽트는 이 id 로 나타난다"고 적어
 * 두고, 부팅 때 그것을 ioapic_map 에 담아 둔다.
 *
 * 목록을 선형 탐색하는 이유: IOAPIC 은 많아야 몇 개이고, 이 조회는 인터럽트
 * 설정 시점에만 일어난다.
 *
 * -EINVAL 을 돌려주면 호출자는 그 인터럽트의 재매핑 항목을 만들 수 없다.
 *
 * 호출 체인:
 *   인터럽트 재매핑 초기화 → [이 함수]
 */
static inline int get_ioapic_devid(int id)
{
	struct devid_map *entry;	/* [한국어] 목록을 훑을 커서 */

	list_for_each_entry(entry, &ioapic_map, list) {	/* [한국어] 펌웨어가 알려 준 IOAPIC 대응 목록. 개수가 적어 선형 탐색으로 충분하다 */
		if (entry->id == id)	/* [한국어] 찾는 IOAPIC 인가 */
			return entry->devid;
	}

	return -EINVAL;	/* [한국어] 대응이 없으면 이 IOAPIC 의 인터럽트는 재매핑할 수 없다 */
}

/*
 * [한국어]
 * get_hpet_devid - HPET id 로 그 인터럽트의 요청자 id 를 찾는다
 *
 * @id: HPET 블록 번호.
 * @return: 요청자 id, 모르면 -EINVAL.
 *
 * get_ioapic_devid 와 완전히 같은 구조다. HPET 도 PCI 장치가 아니면서 MSI 를
 * 내므로, 펌웨어가 알려 준 대응이 없으면 그 인터럽트를 재매핑할 수 없다.
 *
 * 호출 체인:
 *   인터럽트 재매핑 초기화 → [이 함수]
 */
static inline int get_hpet_devid(int id)
{
	struct devid_map *entry;	/* [한국어] 목록 커서 */

	list_for_each_entry(entry, &hpet_map, list) {	/* [한국어] HPET 대응 목록 */
		if (entry->id == id)	/* [한국어] 찾는 HPET 블록인가 */
			return entry->devid;
	}

	return -EINVAL;	/* [한국어] 대응 없음 */
}

/*
 * [한국어] enum amd_iommu_intr_mode_type — 인터럽트 재매핑의 동작 모드
 *
 * 세 모드의 차이는 "IRTE 를 어느 형식으로 쓰고, 게스트에 직접 전달할 수
 * 있는가"이다.
 *  - LEGACY: 32비트 IRTE. 게스트 직접 전달 없음.
 *  - LEGACY_GA: 128비트 IRTE 를 쓰지만 게스트 직접 전달은 하지 않는다.
 *    원 주석대로 사용자에게 보이지 않는 내부 상태로, vAPIC 을 완전히 켤 수
 *    없을 때의 후퇴 지점이다.
 *  - VAPIC: 128비트 IRTE + 게스트 vAPIC 직접 전달. VM exit 없이 게스트에
 *    인터럽트가 도달한다.
 *
 * 중간 단계가 따로 있는 이유: 128비트 IRTE 자체는 쓸 수 있는데 게스트 전달의
 * 전제(GA 로그 등)가 갖춰지지 않는 경우가 있다. 그때 전부 32비트로 되돌리면
 * 나중에 다시 올라갈 수 없어, 형식만 유지하고 기능만 끄는 상태를 둔다.
 */
enum amd_iommu_intr_mode_type {
	AMD_IOMMU_GUEST_IR_LEGACY,
	/* [한국어] 32비트 IRTE 만 쓴다. 게스트 직접 전달은 하지 않는다.
	 * 가장 보수적인 모드로, 하드웨어가 GA 를 지원하지 않거나 사용자가 껐을 때. */

	/* This mode is not visible to users. It is used when
	 * we cannot fully enable vAPIC and fallback to only support
	 * legacy interrupt remapping via 128-bit IRTE.
	 */
	AMD_IOMMU_GUEST_IR_LEGACY_GA,
	/* [한국어] (위 영어 주석에 이어) 128비트 IRTE 를 쓰되 게스트 직접 전달은 끈다.
	 * 왜 중간 단계가 필요한가: 128비트 형식 자체는 쓸 수 있는데 vAPIC 의
	 *   전제(GA 로그 등)가 갖춰지지 않는 경우가 있다. 그때 형식까지 32비트로
	 *   되돌리면 나중에 다시 올라갈 수 없으므로, 형식은 유지하고 기능만 끈다.
	 * 원 주석대로 사용자에게는 보이지 않는 내부 상태다. */
	AMD_IOMMU_GUEST_IR_VAPIC,
	/* [한국어] 128비트 IRTE + 게스트 vAPIC 직접 전달.
	 * VM exit 없이 인터럽트가 게스트에 도달해 가상화 성능이 크게 달라진다. */
};

#define AMD_IOMMU_GUEST_IR_GA(x)	(x == AMD_IOMMU_GUEST_IR_VAPIC || \
					 x == AMD_IOMMU_GUEST_IR_LEGACY_GA)	/* [한국어] 후퇴 모드도 128비트 형식을 쓰므로 함께 참이다 */

#define AMD_IOMMU_GUEST_IR_VAPIC(x)	(x == AMD_IOMMU_GUEST_IR_VAPIC)	/* [한국어] 게스트 vAPIC 직접 전달까지 켠 모드인지 */

/*
 * [한국어] union irte — 32비트 인터럽트 재매핑 항목 (레거시 형식)
 *
 * 인터럽트 하나가 어느 CPU 의 몇 번 벡터로 갈지를 담는다. 장치는 이 항목의
 * 인덱스만 알고, 실제 목적지는 하드웨어가 여기서 읽는다.
 *
 * union 인 이유: 항목을 통째로 읽고 쓸 때는 u32 로, 필드를 다룰 때는
 * 비트필드로 접근한다. 하드웨어가 32비트를 원자적으로 읽으므로 통째 쓰기가
 * 곧 원자적 갱신이 된다 — 128비트 형식이 cmpxchg16b 를 요구하는 것과
 * 대조적이다.
 */
union irte {
	u32 val;
	/* [한국어] 항목 전체를 한 워드로 보는 창.
	 * 하드웨어가 32비트를 원자적으로 읽으므로, 이 값을 통째로 쓰는 것이 곧
	 *   원자적 갱신이 된다 — 128비트 형식이 cmpxchg16b 를 요구하는 것과 다르다. */
	struct {
		u32 valid	: 1,
		/* [한국어] 이 항목이 유효한가. 0 이면 이 인덱스로 오는 인터럽트는 폴트가 된다. */
		    no_fault	: 1,
		    /* [한국어] 유효하지 않은 항목에 대해 폴트를 내지 않고 조용히 버릴지. */
		    int_type	: 3,
		    /* [한국어] 전달 방식 — 고정, 하위 우선순위, NMI, INIT 등. */
		    rq_eoi	: 1,
		    /* [한국어] 레벨 트리거 인터럽트에서 EOI 를 요구하는지. */
		    dm		: 1,
		    /* [한국어] 목적지 모드 — 논리(logical)인지 물리(physical)인지. */
		    rsvd_1	: 1,
		    /* [한국어] 예약 비트. 0 이어야 한다. */
		    destination	: 8,
		    /* [한국어] 목적지 APIC id. 8비트라 이 형식으로는 CPU 255개를 넘을 수 없다 —
		     *   그것이 128비트 형식이 필요해진 이유 중 하나다. */
		    vector	: 8,
		    /* [한국어] CPU 를 깨울 벡터 번호. */
		    rsvd_2	: 8;
		    /* [한국어] 남은 예약 비트. */
	} fields;
};

#define APICID_TO_IRTE_DEST_LO(x)    (x & 0xffffff)	/* [한국어] 32비트 APIC id 의 하위 24비트. IRTE 의 lo 워드에 들어간다 */
#define APICID_TO_IRTE_DEST_HI(x)    ((x >> 24) & 0xff)	/* [한국어] 상위 8비트. hi 워드에 따로 들어간다 — 자리가 이어져 있지 않다 */

/*
 * [한국어] union irte_ga_lo — 128비트 IRTE 의 하위 64비트
 *
 * 같은 64비트가 guest_mode 비트에 따라 전혀 다르게 해석된다. 그래서 두 개의
 * 비트필드 구조체를 union 으로 겹쳐 두고, 모드에 맞는 쪽으로 접근한다.
 *
 *  - fields_remap: 호스트로 전달하는 평범한 재매핑. int_type/dm/destination
 *    으로 어느 CPU 에 어떻게 보낼지를 정한다.
 *  - fields_vapic: 게스트 vCPU 로 직접 전달. 목적지가 CPU 가 아니라 게스트의
 *    vAPIC 백킹 페이지이고, is_run 이 "그 vCPU 가 지금 돌고 있는가"를 나타낸다.
 *    돌고 있지 않으면 하드웨어가 GA 로그에 기록하고 KVM 이 대신 처리한다.
 *
 * ga_tag 는 두 모드에 공통이다 — KVM 이 이 값으로 어느 vCPU/벡터인지 안다.
 */
union irte_ga_lo {
	u64 val;
	/* [한국어] 하위 64비트를 통째로 보는 창. */

	/* For int remapping */
	struct {
		u64 valid	: 1,
		/* [한국어] (재매핑 형식) 항목이 유효한가. */
		    no_fault	: 1,
		    /* [한국어] 유효하지 않을 때 조용히 버릴지. */
		    /* ------ */
		    int_type	: 3,
		    /* [한국어] 전달 방식. */
		    rq_eoi	: 1,
		    /* [한국어] EOI 요구 여부. */
		    dm		: 1,
		    /* [한국어] 목적지 모드(논리/물리). */
		    /* ------ */
		    guest_mode	: 1,
		    /* [한국어] 이 항목을 게스트 형식으로 해석할지.
		     * 이 한 비트가 위아래 두 비트필드 중 어느 것이 유효한지를 결정한다 —
		     *   같은 64비트가 전혀 다른 뜻이 된다. */
		    destination	: 24,
		    /* [한국어] 목적지 APIC id 의 하위 24비트. 상위 8비트는 hi 워드에 있다. */
		    ga_tag	: 32;
		    /* [한국어] KVM 이 붙인 태그. 어느 vCPU 의 몇 번 인터럽트인지를 나타내며,
		     *   GA 로그에 기록될 때도 이 값이 실려 KVM 이 짝을 찾는다. */
	} fields_remap;

	/* For guest vAPIC */
	/*
	 * [한국어] 게스트 vAPIC 형식으로 본 같은 64비트.
	 *
	 * 위 fields_remap 과 자리를 공유하며, guest_mode 비트가 어느 쪽으로
	 * 읽을지를 정한다. 앞쪽 필드들의 뜻이 완전히 달라지는 것이 핵심이다:
	 * 재매핑 형식의 int_type/rq_eoi/dm 자리에 여기서는 ga_log_intr 와
	 * is_run 이 놓인다.
	 *
	 * 각 필드:
	 *  - valid       : 항목이 유효한가.
	 *  - no_fault    : 유효하지 않을 때 조용히 버릴지.
	 *  - ga_log_intr : 게스트에 전달하지 못했을 때 GA 로그에 기록하고
	 *                  인터럽트를 낼지. 이것이 켜져 있어야 KVM 이 놓친
	 *                  인터럽트를 대신 주입할 수 있다.
	 *  - rsvd1       : 예약.
	 *  - is_run      : 목적지 vCPU 가 지금 실행 중인가. 하드웨어가 이 비트를
	 *                  보고 직접 전달할지, GA 로그로 넘길지를 정한다.
	 *                  KVM 이 vCPU 를 스케줄링할 때마다 갱신한다.
	 *  - guest_mode  : 1 이면 이 해석이 유효하다.
	 *  - destination : 게스트 vAPIC 백킹 페이지를 가리키는 목적지.
	 *  - ga_tag      : KVM 이 붙인 태그. GA 로그에도 이 값이 실려 짝을 찾는다.
	 */
	struct {
		u64 valid	: 1,	/* [한국어] 게스트 형식으로 본 첫 비트. 각 필드의 뜻은 위 블록에 정리했다 */
		    no_fault	: 1,
		    /* ------ */
		    ga_log_intr	: 1,
		    rsvd1	: 3,
		    is_run	: 1,
		    /* ------ */
		    guest_mode	: 1,
		    destination	: 24,
		    ga_tag	: 32;
	} fields_vapic;	/* [한국어] 게스트 vAPIC 해석의 끝 */
};

/*
 * [한국어] union irte_ga_hi — 128비트 IRTE 의 상위 64비트
 *
 * 하위와 달리 모드에 따라 해석이 갈리지 않는다. 벡터 번호, 게스트 vAPIC
 * 백킹 페이지의 물리 주소(ga_root_ptr), 그리고 목적지 APIC id 의 상위
 * 바이트가 들어 있다.
 *
 * destination 이 상·하위로 나뉘어 있는 것이 눈에 띈다 — x2APIC 의 32비트
 * id 를 담으려다 보니 하위 24비트는 lo 에, 상위 8비트는 여기에 놓였다.
 * APICID_TO_IRTE_DEST_LO/HI 매크로가 그 분해를 맡는다.
 */
union irte_ga_hi {
	u64 val;	/* [한국어] 상위 64비트를 통째로 보는 창 */
	struct {	/* [한국어] 필드 단위로 보는 창 */
		u64 vector	: 8,
		/* [한국어] 게스트 안에서 쓰일 벡터 번호. */
		    rsvd_1	: 4,
		    /* [한국어] 예약. */
		    ga_root_ptr	: 40,
		    /* [한국어] 게스트 vAPIC 백킹 페이지의 물리 주소.
		     * 게스트 모드에서 하드웨어는 인터럽트를 CPU 로 보내지 않고 이 페이지의
		     *   비트를 세운다. 페이지 정렬이라 하위 12비트는 저장하지 않는다. */
		    rsvd_2	: 4,
		    /* [한국어] 예약. */
		    destination : 8;
		    /* [한국어] 목적지 APIC id 의 상위 8비트.
		     * x2APIC 의 32비트 id 를 담으려다 보니 하위 24비트와 자리가 떨어졌다. */
	} fields;
};

/*
 * [한국어] struct irte_ga — 128비트 IRTE 전체
 *
 * lo/hi 두 워드로 보는 방법과 u128 하나로 보는 방법을 union 으로 겹쳐 둔다.
 * 후자가 필요한 이유는 항목 갱신이 원자적이어야 하기 때문이다 — 하드웨어와
 * KVM 이 동시에 이 항목을 볼 수 있어, 반쯤 갱신된 상태를 노출하면 존재하지
 * 않는 목적지로 인터럽트가 간다. cmpxchg16b 로 128비트를 한 번에 바꾼다.
 */
struct irte_ga {
	union {	/* [한국어] lo/hi 두 워드로 보는 방법과 u128 하나로 보는 방법 */
		struct {	/* [한국어] 두 워드를 나란히 놓은 형태 */
			union irte_ga_lo lo;
			/* [한국어] 하위 64비트. 재매핑/게스트 두 해석이 겹쳐 있다. */
			union irte_ga_hi hi;
			/* [한국어] 상위 64비트. 해석이 하나뿐이다. */
		};
		u128 irte;
		/* [한국어] 둘을 합쳐 128비트로 보는 창.
		 * 왜 필요한가: 하드웨어와 KVM 이 동시에 이 항목을 볼 수 있어, 반쯤 갱신된
		 *   상태를 노출하면 존재하지 않는 목적지로 인터럽트가 간다.
		 *   cmpxchg16b 로 한 번에 바꾸기 위한 창이다. */
	};
};

/*
 * [한국어] struct irq_2_irte — 커널 인터럽트와 IRTE 항목의 연결
 *
 * "이 인터럽트는 어느 장치의 표 몇 번인가"를 기억한다. Intel 이 유닛 단위로
 * 표를 갖는 것과 달리 AMD 는 장치마다 표가 있어, 유닛이 아니라 devid 가
 * 열쇠가 된다.
 */
struct irq_2_irte {
	u16 devid; /* Device ID for IRTE table */
	/* [한국어] 이 인터럽트를 내는 장치의 요청자 id (원 주석: Device ID for IRTE table).
	 * AMD 는 재매핑 표가 장치마다 있으므로, 유닛이 아니라 이 값이 표를 찾는 열쇠다.
	 * 별칭을 공유하는 장치들은 같은 표를 쓰므로 이 값도 같아진다. */
	u16 index; /* Index into IRTE table*/
	/* [한국어] 그 표 안에서 이 인터럽트가 쓰는 항목의 번호 (원 주석: Index into IRTE table).
	 * 설정자: 인터럽트 할당이 빈 자리를 찾아 정한다.
	 * 읽는 자: 항목을 고치거나 지울 때, 그리고 MSI 메시지를 만들 때 —
	 *   이 값이 그대로 장치에 설정되어 하드웨어가 표를 찾는 근거가 된다. */
};

/*
 * [한국어] struct amd_ir_data — 인터럽트 하나가 들고 다니는 재매핑 상태
 *
 * 커널 irq_data 의 chip_data 로 매달린다. 담고 있는 것은 세 가지다:
 *  - 어디에 있는가: iommu, irq_2_irte, entry(항목의 실제 주소).
 *  - 장치에 무엇을 써 넣을 것인가: msi_entry.
 *  - 게스트 전달을 켜고 끌 때 되돌릴 값들: cfg, ga_vector, ga_root_ptr, ga_tag.
 *
 * 마지막 묶음이 있는 이유가 원 주석이 말하는 바다. 게스트 전달을 켜면 항목의
 * 내용이 통째로 바뀌므로, 끌 때 호스트 설정으로 되돌리려면 그 값들을 어딘가
 * 기억해 두어야 한다.
 *
 * entry 가 void * 인 이유: 형식이 32비트(union irte)와 128비트(struct
 * irte_ga) 두 가지라, 타입을 하나로 정할 수 없다. 어느 쪽인지는 irte_ops 가
 * 안다.
 */
struct amd_ir_data {
	struct amd_iommu *iommu;
	/* [한국어] 이 인터럽트의 표를 가진 유닛.
	 * 무효화 명령을 보낼 곳이기도 하다. */
	struct irq_2_irte irq_2_irte;
	/* [한국어] 어느 장치의 표 몇 번인지.
	 * 항목을 고치거나 지울 때 찾아가는 유일한 단서다. */
	struct msi_msg msi_entry;
	/* [한국어] 장치에 써 넣을 MSI 주소·데이터.
	 * 재매핑이 켜지면 이 값은 목적지가 아니라 표 인덱스를 담은 핸들이 된다.
	 * 할당 시점에 정해져 CPU 를 옮겨도 바뀌지 않는다. */
	void *entry;    /* Pointer to union irte or struct irte_ga */
	/* [한국어] (원 주석: Pointer to union irte or struct irte_ga)
	 * 표 안의 실제 항목 주소. 형식이 두 가지라 타입을 하나로 정할 수 없어
	 *   void * 로 두고, 어느 쪽인지는 iommu->irte_ops 가 안다. */

	/**
	 * Store information for activate/de-activate
	 * Guest virtual APIC mode during runtime.
	 */
	struct irq_cfg *cfg;
	/* [한국어] (위 영어 주석에 이어) 호스트의 벡터·목적지 설정.
	 * 게스트 전달을 끌 때 여기로 되돌린다. */
	int ga_vector;
	/* [한국어] 게스트 전달에 쓰던 벡터. 다시 켤 때 복원한다. */
	u64 ga_root_ptr;
	/* [한국어] 게스트 vAPIC 백킹 페이지 주소. 같은 이유로 보관한다. */
	u32 ga_tag;
	/* [한국어] KVM 이 붙인 태그. 게스트 전달을 다시 켤 때 그대로 쓴다. */
};

/*
 * [한국어] struct amd_irte_ops — IRTE 형식별 조작 함수 표
 *
 * 32비트 형식과 128비트 형식은 필드 배치가 완전히 달라 같은 코드로 다룰 수
 * 없다. 그렇다고 호출부마다 if 로 갈라 쓰면 인터럽트 경로가 지저분해지므로,
 * 형식마다 함수 묶음을 만들어 유닛에 하나씩 달아 둔다(iommu->irte_ops).
 *
 * 할당 상태를 다루는 함수(set_allocated/is_allocated/clear_allocated)까지
 * 여기 있는 이유: 두 형식은 항목 크기가 달라 "표에서 몇 번째 항목"의 주소
 * 계산도 다르고, 빈 항목을 표시하는 방법도 다르다.
 */
struct amd_irte_ops {
	void (*prepare)(void *, u32, bool, u8, u32, int);
	/* [한국어] 항목의 내용을 조립한다.
	 * 형식마다 필드 배치가 달라 이 단계부터 갈라진다. */
	void (*activate)(struct amd_iommu *iommu, void *, u16, u16);
	/* [한국어] 조립된 내용을 표에 써 넣고 유효 비트를 세운다.
	 * 캐시 무효화까지 여기서 한다. */
	void (*deactivate)(struct amd_iommu *iommu, void *, u16, u16);
	/* [한국어] 유효 비트를 내려 인터럽트를 끊는다. 자리는 그대로 둔다. */
	void (*set_affinity)(struct amd_iommu *iommu, void *, u16, u16, u8, u32);
	/* [한국어] 목적지와 벡터를 바꾼다.
	 * 128비트 형식은 원자적 교체가 필요해 구현이 크게 다르다. */
	void *(*get)(struct irq_remap_table *, int);
	/* [한국어] 표에서 n번째 항목의 주소를 구한다.
	 * 항목 크기가 형식마다 달라 이 계산도 갈라져야 한다. */
	void (*set_allocated)(struct irq_remap_table *, int);
	/* [한국어] 그 항목을 사용 중으로 표시한다. */
	bool (*is_allocated)(struct irq_remap_table *, int);
	/* [한국어] 사용 중인지 묻는다.
	 * AMD 는 별도의 비트맵 없이 항목 자체의 값으로 판별하므로, 형식마다
	 *   "비어 있음"의 표현이 다르다. */
	void (*clear_allocated)(struct irq_remap_table *, int);
	/* [한국어] 그 자리를 비운다. */
};

#ifdef CONFIG_IRQ_REMAP	/* [한국어] 인터럽트 재매핑을 켠 커널에서만 형식별 함수 표가 존재한다 */
extern struct amd_irte_ops irte_32_ops;	/* [한국어] 32비트 IRTE 형식의 조작 함수 표 */
extern struct amd_irte_ops irte_128_ops;	/* [한국어] 128비트 형식의 조작 함수 표 */
#endif

#endif /* _ASM_X86_AMD_IOMMU_TYPES_H */
