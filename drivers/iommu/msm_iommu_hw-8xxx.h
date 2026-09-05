/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 */

/*
 * [한국어 설명] Qualcomm MSM(8xxx) IOMMU의 하드웨어 정의 (msm_iommu_hw-8xxx.h)
 *
 * === 파일의 역할 ===
 * msm_iommu.c가 쓰는 **모든 하드웨어 지식**이 이 헤더에 모여 있다.
 * 레지스터 오프셋, 필드의 마스크와 시프트, 그리고 그 둘을 엮어
 * "레지스터 하나 읽기/쓰기"와 "필드 하나 읽기/쓰기"를 표현하는
 * 매크로들이다. 실행 코드는 한 줄도 없고 전부 전처리기 정의다.
 *
 * 1300줄 가까이 되지만 구조는 단순하다. 같은 정보가 **네 겹으로**
 * 반복되기 때문이다.
 *
 *   1) 접근 매크로   SET_TTBR0(b, c, v)     → 쓰기 한 번
 *   2) 필드 매크로   SET_TTBR0_PA(b, c, v)  → 읽고-고치고-쓰기
 *   3) 필드 위치     TTBR0_PA               → (마스크 << 시프트)
 *   4) 마스크와 시프트  TTBR0_PA_MASK / TTBR0_PA_SHIFT
 *
 * 이름 이어 붙이기(## 연산자)가 그 네 겹을 잇는다. 예를 들어
 * SET_GLOBAL_FIELD(b, r, F, v)는 F에서 F_MASK와 F_SHIFT를 만들어
 * 내므로, 필드 이름 하나만 넘기면 나머지가 따라온다. 그래서 필드가
 * 하나 늘 때마다 정확히 네 줄이 함께 늘어난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [msm_iommu.c] 도메인 붙이기, 매핑, 폴트 처리
 *        ↓ 이 헤더의 매크로만을 통해
 *   [MMIO 레지스터] 전역 영역 + 컨텍스트 뱅크 창들
 *
 * 이 IOMMU의 하드웨어 모델은 두 층이다.
 *
 *  - **전역 영역**: IOMMU 하나 전체에 해당하는 설정. 스트림 ID를
 *    컨텍스트 뱅크에 대응시키는 표(M2VCBR_N), 뱅크의 속성(CBACR_N),
 *    전역 제어와 오류 보고가 여기 있다. 오프셋이 0xFF000 이상의
 *    높은 자리에 몰려 있다.
 *  - **컨텍스트 뱅크**: 주소 공간 하나에 해당하는 창. 뱅크마다
 *    4KB 창을 갖고(CTX_SHIFT가 그 지수다), 그 안에 테이블 기준
 *    주소, 폴트 상태, 무효화 명령이 들어 있다. 뱅크 번호를 12비트
 *    밀어 기준 주소에 더하면 그 뱅크의 창이 된다 — GET_CTX_REG와
 *    SET_CTX_REG가 하는 계산이 그것이다.
 *
 * 실행 컨텍스트: 모든 매크로가 readl/writel로 펼쳐지므로, 부르는
 * 쪽이 클럭을 켜고 락을 잡은 상태여야 한다. 필드 매크로는
 * 읽고-고치고-쓰기라 **원자적이지 않다** — 같은 레지스터를 두 곳에서
 * 동시에 고치면 한쪽이 사라진다.
 *
 * === 타 모듈과의 연결 ===
 * - msm_iommu.c: 이 헤더의 유일한 사용자. 컨텍스트 뱅크 설정,
 *   TLB 무효화, 폴트 진단이 모두 여기 매크로로 이뤄진다.
 * - msm_iommu.h: 드라이버 쪽 구조체 정의. 이 파일과 겹치지 않는다.
 * - 페이지 테이블 형식은 ARM 짧은 서술자와 같아, FL_/SL_ 접두사의
 *   상수들이 그 엔트리 비트를 정의한다. 실제 테이블 조작도
 *   msm_iommu.c가 그 상수로 직접 한다.
 * 데이터 흐름: 마스터의 스트림 ID → M2VCBR_N → 컨텍스트 뱅크 번호
 * → 그 뱅크의 TTBR0 → 1단계 테이블 → 2단계 테이블 → 물리 주소.
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체는 없고 매크로만 있으므로, 알아 두면 나머지가 읽히는 것들:
 * - GET_CTX_REG / SET_CTX_REG: 뱅크 번호를 창 오프셋으로 바꾸는
 *   계산이 들어 있다. 이 파일의 모든 컨텍스트 접근이 여기로 모인다.
 * - SET_FIELD: 읽고-고치고-쓰기의 본체. 다른 필드를 보존하려면
 *   반드시 이 절차를 거쳐야 하며, 그 대가로 원자성이 없다.
 * - M2VCBR_N과 CBACR_N: 스트림을 뱅크에 잇고 뱅크에 VMID를 주는
 *   두 표. 이 IOMMU의 격리 모델이 이 둘로 표현된다.
 * - FSR / FSYNR0 / FSYNR1: 폴트가 났을 때 "무엇이, 어떤 접근으로,
 *   어디서" 를 알려 주는 세 레지스터.
 * - PRRR / NMRR: TEX 클래스 여덟 개를 메모리 타입과 캐시 정책으로
 *   옮기는 재매핑 표. ARM의 TEX remap과 같은 개념이다.
 */

#ifndef __ARCH_ARM_MACH_MSM_IOMMU_HW_8XXX_H	/* [한국어] 중복 포함을 막는 헤더 가드의 시작. */
#define __ARCH_ARM_MACH_MSM_IOMMU_HW_8XXX_H	/* [한국어] 중복 포함을 막는 헤더 가드. */

#define CTX_SHIFT 12	/* [한국어] 컨텍스트 뱅크 하나가 차지하는 레지스터 창의 크기(4KB)의 지수. 뱅크 번호를 이만큼 밀어 기준 주소에 더하면 그 뱅크의 창이 된다. */

#define GET_GLOBAL_REG(reg, base) (readl((base) + (reg)))	/* [한국어] 전역 레지스터 하나를 읽는다. 전역 영역은 뱅크 오프셋이 붙지 않는다. */
/* [한국어] 컨텍스트 뱅크 c의 레지스터를 읽는다. 뱅크 번호를 4KB 단위로 밀어 창을 고른다. */
#define GET_CTX_REG(reg, base, ctx) \
				(readl((base) + (reg) + ((ctx) << CTX_SHIFT)))	/* [한국어] 뱅크 번호를 4KB 창 크기만큼 밀어 그 뱅크의 레지스터 주소를 만든다. */

#define SET_GLOBAL_REG(reg, base, val)	writel((val), ((base) + (reg)))	/* [한국어] 전역 레지스터 하나에 쓴다. */

/* [한국어] 컨텍스트 뱅크 c의 레지스터에 쓴다. */
#define SET_CTX_REG(reg, base, ctx, val) \
			writel((val), ((base) + (reg) + ((ctx) << CTX_SHIFT)))	/* [한국어] 읽기와 같은 방식으로 뱅크 창의 주소를 계산해 쓴다. */

/* Wrappers for numbered registers */
#define SET_GLOBAL_REG_N(b, n, r, v) SET_GLOBAL_REG(b, ((r) + (n << 2)), (v))	/* [한국어] 배열형 전역 레지스터의 n번째 항목에 쓴다(항목마다 4바이트). */
#define GET_GLOBAL_REG_N(b, n, r)    GET_GLOBAL_REG(b, ((r) + (n << 2)))	/* [한국어] 배열형 전역 레지스터의 n번째 항목을 읽는다. */

/* Field wrappers */
#define GET_GLOBAL_FIELD(b, r, F)    GET_FIELD(((b) + (r)), F##_MASK, F##_SHIFT)	/* [한국어] 전역 레지스터에서 필드 하나만 뽑아 읽는다. 이름 이어 붙이기로 마스크와 시프트를 함께 만든다. */
/* [한국어] 컨텍스트 뱅크의 레지스터에서 필드 하나만 뽑아 읽는다. */
#define GET_CONTEXT_FIELD(b, c, r, F)	\
	GET_FIELD(((b) + (r) + ((c) << CTX_SHIFT)), F##_MASK, F##_SHIFT)

/* [한국어] 전역 레지스터의 필드 하나만 고쳐 쓴다(읽고-고치고-쓰기). */
#define SET_GLOBAL_FIELD(b, r, F, v) \
	SET_FIELD(((b) + (r)), F##_MASK, F##_SHIFT, (v))
/* [한국어] 컨텍스트 뱅크 레지스터의 필드 하나만 고쳐 쓴다. */
#define SET_CONTEXT_FIELD(b, c, r, F, v)	\
	SET_FIELD(((b) + (r) + ((c) << CTX_SHIFT)), F##_MASK, F##_SHIFT, (v))	/* [한국어] 뱅크 창을 고른 뒤, 필드 이름에서 만든 마스크와 시프트로 그 자리만 고친다. */

#define GET_FIELD(addr, mask, shift)  ((readl(addr) >> (shift)) & (mask))	/* [한국어] 주소에서 값을 읽어 시프트하고 마스크해 필드 값만 남긴다. */

/* [한국어] 읽고-고치고-쓰기로 필드 하나만 바꾼다. 다른 필드를 보존하려면 반드시 이 절차를 거쳐야 한다. */
#define SET_FIELD(addr, mask, shift, v) \
do { \
	int t = readl(addr); \
	writel((t & ~((mask) << (shift))) + (((v) & (mask)) << (shift)), addr);\
} while (0)	/* [한국어] 매크로 본문을 하나의 문장으로 묶는 관용구. */


#define NUM_FL_PTE	4096	/* [한국어] 1단계 테이블의 엔트리 수. 4096 × 1MB로 32비트 주소 공간 전체를 덮는다. */
#define NUM_SL_PTE	256	/* [한국어] 2단계 테이블의 엔트리 수. 1MB를 4KB로 나눈 256개다. */
#define NUM_TEX_CLASS	8	/* [한국어] TEX 클래스의 개수. PRRR과 NMRR이 클래스마다 메모리 타입과 캐시 정책을 정한다. */

/* First-level page table bits */
#define FL_BASE_MASK		0xFFFFFC00	/* [한국어] 1단계 엔트리에서 2단계 테이블의 주소를 뽑는 마스크(1KB 정렬). */
#define FL_TYPE_TABLE		(1 << 0)	/* [한국어] 이 1단계 엔트리가 2단계 테이블을 가리킨다. */
#define FL_TYPE_SECT		(2 << 0)	/* [한국어] 이 1단계 엔트리가 1MB 섹션을 직접 매핑한다. */
#define FL_SUPERSECTION		(1 << 18)	/* [한국어] 이 섹션 엔트리가 16MB 슈퍼섹션의 일부다(같은 값이 16번 반복된다). */
#define FL_AP_WRITE		(1 << 10)	/* [한국어] 섹션에 쓰기를 허용한다. */
#define FL_AP_READ		(1 << 11)	/* [한국어] 섹션에 읽기를 허용한다. */
#define FL_SHARED		(1 << 16)	/* [한국어] 이 섹션을 공유 메모리로 다룬다. */
#define FL_BUFFERABLE		(1 << 2)	/* [한국어] 쓰기 버퍼링을 허용한다. */
#define FL_CACHEABLE		(1 << 3)	/* [한국어] 캐시를 허용한다. */
#define FL_TEX0			(1 << 12)	/* [한국어] TEX 클래스의 0번 비트 — 나머지 두 비트와 합쳐 클래스를 고른다. */
#define FL_OFFSET(va)		(((va) & 0xFFF00000) >> 20)	/* [한국어] 가상 주소에서 1단계 인덱스를 뽑는다(상위 12비트). */
#define FL_NG			(1 << 17)	/* [한국어] 전역이 아닌 매핑 — ASID로 태그되어 컨텍스트마다 구별된다. */

/* Second-level page table bits */
#define SL_BASE_MASK_LARGE	0xFFFF0000	/* [한국어] 2단계 엔트리에서 64KB 큰 페이지의 주소를 뽑는 마스크. */
#define SL_BASE_MASK_SMALL	0xFFFFF000	/* [한국어] 2단계 엔트리에서 4KB 작은 페이지의 주소를 뽑는 마스크. */
#define SL_TYPE_LARGE		(1 << 0)	/* [한국어] 이 2단계 엔트리가 64KB 큰 페이지다(같은 값이 16번 반복된다). */
#define SL_TYPE_SMALL		(2 << 0)	/* [한국어] 이 2단계 엔트리가 4KB 작은 페이지다. */
#define SL_AP0			(1 << 4)	/* [한국어] 접근 권한 비트 0. */
#define SL_AP1			(2 << 4)	/* [한국어] 접근 권한 비트 1 — AP0과 합쳐 읽기/쓰기 조합을 만든다. */
#define SL_SHARED		(1 << 10)	/* [한국어] 이 페이지를 공유 메모리로 다룬다. */
#define SL_BUFFERABLE		(1 << 2)	/* [한국어] 쓰기 버퍼링을 허용한다. */
#define SL_CACHEABLE		(1 << 3)	/* [한국어] 캐시를 허용한다. */
#define SL_TEX0			(1 << 6)	/* [한국어] TEX 클래스의 0번 비트. */
#define SL_OFFSET(va)		(((va) & 0xFF000) >> 12)	/* [한국어] 가상 주소에서 2단계 인덱스를 뽑는다(가운데 8비트). */
#define SL_NG			(1 << 11)	/* [한국어] 전역이 아닌 매핑 — ASID로 태그된다. */

/* Memory type and cache policy attributes */
#define MT_SO			0	/* [한국어] 메모리 타입: 강한 순서(Strongly Ordered) — 재배치도 병합도 없다. */
#define MT_DEV			1	/* [한국어] 메모리 타입: 디바이스 — 부수 효과가 있는 MMIO에 쓴다. */
#define MT_NORMAL		2	/* [한국어] 메모리 타입: 일반 메모리 — 캐시와 병합이 허용된다. */
#define CP_NONCACHED		0	/* [한국어] 캐시 정책: 캐시하지 않는다. */
#define CP_WB_WA		1	/* [한국어] 캐시 정책: 라이트백 + 쓰기 할당. */
#define CP_WT			2	/* [한국어] 캐시 정책: 라이트스루. */
#define CP_WB_NWA		3	/* [한국어] 캐시 정책: 라이트백 + 쓰기 할당 없음. */

/* Global register setters / getters */
/* [한국어] 여기서부터 전역 레지스터의 접근 매크로다.
 * 전역 영역은 IOMMU 하나 전체에 해당하므로 뱅크 번호를 받지 않고,
 * 기준 주소와 값만으로 접근한다.
 * 이름 뒤에 _N이 붙은 둘(M2VCBR_N, CBACR_N)만 배열이라 인덱스를
 * 하나 더 받는다 — 스트림과 뱅크마다 한 워드씩 있기 때문이다. */
#define SET_M2VCBR_N(b, N, v)	 SET_GLOBAL_REG_N(M2VCBR_N, N, (b), (v))	/* [한국어] M2VCBR_N 배열의 n번째 항목에 쓴다 — 마스터 ID를 컨텍스트 뱅크에 대응시키는 표(스트림당 한 워드). */
#define SET_CBACR_N(b, N, v)	 SET_GLOBAL_REG_N(CBACR_N, N, (b), (v))	/* [한국어] CBACR_N 배열의 n번째 항목에 쓴다 — 컨텍스트 뱅크의 속성 — VMID와 인터럽트 번호를 정한다. */
#define SET_TLBRSW(b, v)	 SET_GLOBAL_REG(TLBRSW, (b), (v))	/* [한국어] 전역 레지스터 TLBRSW에 쓴다 — TLB 읽기/쓰기 선택 — 디버그용으로 TLB 항목 하나를 지목한다. */
#define SET_TLBTR0(b, v)	 SET_GLOBAL_REG(TLBTR0, (b), (v))	/* [한국어] 전역 레지스터 TLBTR0에 쓴다 — 지목된 TLB 항목의 속성(권한·공유·메모리 타입). */
#define SET_TLBTR1(b, v)	 SET_GLOBAL_REG(TLBTR1, (b), (v))	/* [한국어] 전역 레지스터 TLBTR1에 쓴다 — 지목된 TLB 항목의 VMID와 물리 주소. */
#define SET_TLBTR2(b, v)	 SET_GLOBAL_REG(TLBTR2, (b), (v))	/* [한국어] 전역 레지스터 TLBTR2에 쓴다 — 지목된 TLB 항목의 ASID·유효 비트·가상 주소. */
#define SET_TESTBUSCR(b, v)	 SET_GLOBAL_REG(TESTBUSCR, (b), (v))	/* [한국어] 전역 레지스터 TESTBUSCR에 쓴다 — 테스트 버스 제어 — 하드웨어 디버그 전용. */
#define SET_GLOBAL_TLBIALL(b, v) SET_GLOBAL_REG(GLOBAL_TLBIALL, (b), (v))	/* [한국어] 전역 레지스터 GLOBAL_TLBIALL에 쓴다 — 모든 컨텍스트의 TLB를 통째로 무효화한다. */
#define SET_TLBIVMID(b, v)	 SET_GLOBAL_REG(TLBIVMID, (b), (v))	/* [한국어] 전역 레지스터 TLBIVMID에 쓴다 — 지정한 VMID의 TLB 항목만 무효화한다. */
#define SET_CR(b, v)		 SET_GLOBAL_REG(CR, (b), (v))	/* [한국어] 전역 레지스터 CR에 쓴다 — IOMMU 전역 제어 — 클라이언트 통과, stall, TLB 잠금 허용 등. */
#define SET_EAR(b, v)		 SET_GLOBAL_REG(EAR, (b), (v))	/* [한국어] 전역 레지스터 EAR에 쓴다 — 전역 오류가 난 주소. */
#define SET_ESR(b, v)		 SET_GLOBAL_REG(ESR, (b), (v))	/* [한국어] 전역 레지스터 ESR에 쓴다 — 전역 오류 상태 — 설정 오류, 통과 중 접근 등. */
#define SET_ESRRESTORE(b, v)	 SET_GLOBAL_REG(ESRRESTORE, (b), (v))	/* [한국어] 전역 레지스터 ESRRESTORE에 쓴다 — 전역 오류 상태를 복원용으로 되쓰는 창. */
#define SET_ESYNR0(b, v)	 SET_GLOBAL_REG(ESYNR0, (b), (v))	/* [한국어] 전역 레지스터 ESYNR0에 쓴다 — 전역 오류를 낸 트랜잭션의 식별자들(마스터·페이지·VMID). */
#define SET_ESYNR1(b, v)	 SET_GLOBAL_REG(ESYNR1, (b), (v))	/* [한국어] 전역 레지스터 ESYNR1에 쓴다 — 전역 오류를 낸 트랜잭션의 속성들(크기·버스트·권한). */
#define SET_RPU_ACR(b, v)	 SET_GLOBAL_REG(RPU_ACR, (b), (v))	/* [한국어] 전역 레지스터 RPU_ACR에 쓴다 — RPU(원격 처리 유닛) 보조 제어. */

#define GET_M2VCBR_N(b, N)	 GET_GLOBAL_REG_N(M2VCBR_N, N, (b))	/* [한국어] M2VCBR_N 배열의 n번째 항목을 읽는다 — 마스터 ID를 컨텍스트 뱅크에 대응시키는 표(스트림당 한 워드). */
#define GET_CBACR_N(b, N)	 GET_GLOBAL_REG_N(CBACR_N, N, (b))	/* [한국어] CBACR_N 배열의 n번째 항목을 읽는다 — 컨텍스트 뱅크의 속성 — VMID와 인터럽트 번호를 정한다. */
#define GET_TLBTR0(b)		 GET_GLOBAL_REG(TLBTR0, (b))	/* [한국어] 전역 레지스터 TLBTR0를 읽는다 — 지목된 TLB 항목의 속성(권한·공유·메모리 타입). */
#define GET_TLBTR1(b)		 GET_GLOBAL_REG(TLBTR1, (b))	/* [한국어] 전역 레지스터 TLBTR1를 읽는다 — 지목된 TLB 항목의 VMID와 물리 주소. */
#define GET_TLBTR2(b)		 GET_GLOBAL_REG(TLBTR2, (b))	/* [한국어] 전역 레지스터 TLBTR2를 읽는다 — 지목된 TLB 항목의 ASID·유효 비트·가상 주소. */
#define GET_TESTBUSCR(b)	 GET_GLOBAL_REG(TESTBUSCR, (b))	/* [한국어] 전역 레지스터 TESTBUSCR를 읽는다 — 테스트 버스 제어 — 하드웨어 디버그 전용. */
#define GET_GLOBAL_TLBIALL(b)	 GET_GLOBAL_REG(GLOBAL_TLBIALL, (b))	/* [한국어] 전역 레지스터 GLOBAL_TLBIALL를 읽는다 — 모든 컨텍스트의 TLB를 통째로 무효화한다. */
#define GET_TLBIVMID(b)		 GET_GLOBAL_REG(TLBIVMID, (b))	/* [한국어] 전역 레지스터 TLBIVMID를 읽는다 — 지정한 VMID의 TLB 항목만 무효화한다. */
#define GET_CR(b)		 GET_GLOBAL_REG(CR, (b))	/* [한국어] 전역 레지스터 CR를 읽는다 — IOMMU 전역 제어 — 클라이언트 통과, stall, TLB 잠금 허용 등. */
#define GET_EAR(b)		 GET_GLOBAL_REG(EAR, (b))	/* [한국어] 전역 레지스터 EAR를 읽는다 — 전역 오류가 난 주소. */
#define GET_ESR(b)		 GET_GLOBAL_REG(ESR, (b))	/* [한국어] 전역 레지스터 ESR를 읽는다 — 전역 오류 상태 — 설정 오류, 통과 중 접근 등. */
#define GET_ESRRESTORE(b)	 GET_GLOBAL_REG(ESRRESTORE, (b))	/* [한국어] 전역 레지스터 ESRRESTORE를 읽는다 — 전역 오류 상태를 복원용으로 되쓰는 창. */
#define GET_ESYNR0(b)		 GET_GLOBAL_REG(ESYNR0, (b))	/* [한국어] 전역 레지스터 ESYNR0를 읽는다 — 전역 오류를 낸 트랜잭션의 식별자들(마스터·페이지·VMID). */
#define GET_ESYNR1(b)		 GET_GLOBAL_REG(ESYNR1, (b))	/* [한국어] 전역 레지스터 ESYNR1를 읽는다 — 전역 오류를 낸 트랜잭션의 속성들(크기·버스트·권한). */
#define GET_REV(b)		 GET_GLOBAL_REG(REV, (b))	/* [한국어] 전역 레지스터 REV를 읽는다 — 하드웨어 리비전. */
#define GET_IDR(b)		 GET_GLOBAL_REG(IDR, (b))	/* [한국어] 전역 레지스터 IDR를 읽는다 — 하드웨어 능력 — 컨텍스트 뱅크 수, TLB 크기, 하드웨어 워크 지원 여부. */
#define GET_RPU_ACR(b)		 GET_GLOBAL_REG(RPU_ACR, (b))	/* [한국어] 전역 레지스터 RPU_ACR를 읽는다 — RPU(원격 처리 유닛) 보조 제어. */


/* Context register setters/getters */
/* [한국어] 여기서부터 컨텍스트 뱅크의 접근 매크로다.
 * 모두 뱅크 번호 c를 받으며, 그것이 4KB 창의 선택으로 이어진다.
 * 같은 이름의 레지스터라도 뱅크마다 값이 다르다 — 그것이 곧
 * 주소 공간의 격리다. */
#define SET_SCTLR(b, c, v)	 SET_CTX_REG(SCTLR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에 쓴다 — 이 컨텍스트의 시스템 제어 — MMU 활성화, TEX 재매핑, 접근 플래그. */
#define SET_ACTLR(b, c, v)	 SET_CTX_REG(ACTLR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에 쓴다 — 이 컨텍스트의 보조 제어 — 폴트 처리 방식과 테이블 워크 속성. */
#define SET_CONTEXTIDR(b, c, v)	 SET_CTX_REG(CONTEXTIDR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 CONTEXTIDR에 쓴다 — 이 컨텍스트의 ASID와 프로세스 ID. */
#define SET_TTBR0(b, c, v)	 SET_CTX_REG(TTBR0, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에 쓴다 — 이 컨텍스트의 1단계 테이블 기준 주소 0번. */
#define SET_TTBR1(b, c, v)	 SET_CTX_REG(TTBR1, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에 쓴다 — 이 컨텍스트의 1단계 테이블 기준 주소 1번. */
#define SET_TTBCR(b, c, v)	 SET_CTX_REG(TTBCR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TTBCR에 쓴다 — 두 TTBR의 경계와 워크 비활성화를 정한다. */
#define SET_PAR(b, c, v)	 SET_CTX_REG(PAR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 PAR에 쓴다 — 주소 변환 결과 — 성공하면 물리 주소, 실패하면 폴트 원인. */
#define SET_FSR(b, c, v)	 SET_CTX_REG(FSR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 FSR에 쓴다 — 이 컨텍스트의 폴트 상태 — 어떤 종류의 폴트가 났는가. */
#define SET_FSRRESTORE(b, c, v)	 SET_CTX_REG(FSRRESTORE, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 FSRRESTORE에 쓴다 — 폴트 상태를 복원용으로 되쓰는 창. */
#define SET_FAR(b, c, v)	 SET_CTX_REG(FAR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 FAR에 쓴다 — 폴트가 난 가상 주소. */
#define SET_FSYNR0(b, c, v)	 SET_CTX_REG(FSYNR0, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에 쓴다 — 폴트를 낸 트랜잭션의 식별자들. */
#define SET_FSYNR1(b, c, v)	 SET_CTX_REG(FSYNR1, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에 쓴다 — 폴트를 낸 트랜잭션의 속성들. */
#define SET_PRRR(b, c, v)	 SET_CTX_REG(PRRR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 PRRR에 쓴다 — 1차 영역 재매핑 — TEX 클래스를 메모리 타입으로 옮긴다. */
#define SET_NMRR(b, c, v)	 SET_CTX_REG(NMRR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 NMRR에 쓴다 — 일반 메모리 재매핑 — TEX 클래스별 캐시 정책. */
#define SET_TLBLKCR(b, c, v)	 SET_CTX_REG(TLBLCKR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TLBLCKR에 쓴다 — TLB 잠금 제어 — 특정 항목을 교체되지 않게 고정한다. */
#define SET_V2PSR(b, c, v)	 SET_CTX_REG(V2PSR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 V2PSR에 쓴다 — 가상→물리 변환 요청의 결과 상태. */
#define SET_TLBFLPTER(b, c, v)	 SET_CTX_REG(TLBFLPTER, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TLBFLPTER에 쓴다 — TLB가 캐시한 1단계 엔트리(디버그용). */
#define SET_TLBSLPTER(b, c, v)	 SET_CTX_REG(TLBSLPTER, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TLBSLPTER에 쓴다 — TLB가 캐시한 2단계 엔트리(디버그용). */
#define SET_BFBCR(b, c, v)	 SET_CTX_REG(BFBCR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에 쓴다 — 분기 예측 버퍼 제어 — 테이블 워크 예측을 조절한다. */
#define SET_CTX_TLBIALL(b, c, v) SET_CTX_REG(CTX_TLBIALL, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 CTX_TLBIALL에 쓴다 — 이 컨텍스트의 TLB를 통째로 무효화한다. */
#define SET_TLBIASID(b, c, v)	 SET_CTX_REG(TLBIASID, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TLBIASID에 쓴다 — 지정한 ASID의 TLB 항목만 무효화한다. */
#define SET_TLBIVA(b, c, v)	 SET_CTX_REG(TLBIVA, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TLBIVA에 쓴다 — 지정한 ASID의 특정 가상 주소만 무효화한다. */
#define SET_TLBIVAA(b, c, v)	 SET_CTX_REG(TLBIVAA, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 TLBIVAA에 쓴다 — ASID를 가리지 않고 특정 가상 주소를 무효화한다. */
#define SET_V2PPR(b, c, v)	 SET_CTX_REG(V2PPR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 V2PPR에 쓴다 — 특권 읽기 관점의 가상→물리 변환을 요청한다. */
#define SET_V2PPW(b, c, v)	 SET_CTX_REG(V2PPW, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 V2PPW에 쓴다 — 특권 쓰기 관점의 가상→물리 변환을 요청한다. */
#define SET_V2PUR(b, c, v)	 SET_CTX_REG(V2PUR, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 V2PUR에 쓴다 — 사용자 읽기 관점의 가상→물리 변환을 요청한다. */
#define SET_V2PUW(b, c, v)	 SET_CTX_REG(V2PUW, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 V2PUW에 쓴다 — 사용자 쓰기 관점의 가상→물리 변환을 요청한다. */
#define SET_RESUME(b, c, v)	 SET_CTX_REG(RESUME, (b), (c), (v))	/* [한국어] 컨텍스트 뱅크 c의 RESUME에 쓴다 — 멈춰 세운 트랜잭션을 재개시키거나 종료시킨다. */

#define GET_SCTLR(b, c)		 GET_CTX_REG(SCTLR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 SCTLR를 읽는다 — 이 컨텍스트의 시스템 제어 — MMU 활성화, TEX 재매핑, 접근 플래그. */
#define GET_ACTLR(b, c)		 GET_CTX_REG(ACTLR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 ACTLR를 읽는다 — 이 컨텍스트의 보조 제어 — 폴트 처리 방식과 테이블 워크 속성. */
#define GET_CONTEXTIDR(b, c)	 GET_CTX_REG(CONTEXTIDR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 CONTEXTIDR를 읽는다 — 이 컨텍스트의 ASID와 프로세스 ID. */
#define GET_TTBR0(b, c)		 GET_CTX_REG(TTBR0, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TTBR0를 읽는다 — 이 컨텍스트의 1단계 테이블 기준 주소 0번. */
#define GET_TTBR1(b, c)		 GET_CTX_REG(TTBR1, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TTBR1를 읽는다 — 이 컨텍스트의 1단계 테이블 기준 주소 1번. */
#define GET_TTBCR(b, c)		 GET_CTX_REG(TTBCR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TTBCR를 읽는다 — 두 TTBR의 경계와 워크 비활성화를 정한다. */
#define GET_PAR(b, c)		 GET_CTX_REG(PAR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 PAR를 읽는다 — 주소 변환 결과 — 성공하면 물리 주소, 실패하면 폴트 원인. */
#define GET_FSR(b, c)		 GET_CTX_REG(FSR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 FSR를 읽는다 — 이 컨텍스트의 폴트 상태 — 어떤 종류의 폴트가 났는가. */
#define GET_FSRRESTORE(b, c)	 GET_CTX_REG(FSRRESTORE, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 FSRRESTORE를 읽는다 — 폴트 상태를 복원용으로 되쓰는 창. */
#define GET_FAR(b, c)		 GET_CTX_REG(FAR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 FAR를 읽는다 — 폴트가 난 가상 주소. */
#define GET_FSYNR0(b, c)	 GET_CTX_REG(FSYNR0, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0를 읽는다 — 폴트를 낸 트랜잭션의 식별자들. */
#define GET_FSYNR1(b, c)	 GET_CTX_REG(FSYNR1, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1를 읽는다 — 폴트를 낸 트랜잭션의 속성들. */
#define GET_PRRR(b, c)		 GET_CTX_REG(PRRR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 PRRR를 읽는다 — 1차 영역 재매핑 — TEX 클래스를 메모리 타입으로 옮긴다. */
#define GET_NMRR(b, c)		 GET_CTX_REG(NMRR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 NMRR를 읽는다 — 일반 메모리 재매핑 — TEX 클래스별 캐시 정책. */
#define GET_TLBLCKR(b, c)	 GET_CTX_REG(TLBLCKR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TLBLCKR를 읽는다 — TLB 잠금 제어 — 특정 항목을 교체되지 않게 고정한다. */
#define GET_V2PSR(b, c)		 GET_CTX_REG(V2PSR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 V2PSR를 읽는다 — 가상→물리 변환 요청의 결과 상태. */
#define GET_TLBFLPTER(b, c)	 GET_CTX_REG(TLBFLPTER, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TLBFLPTER를 읽는다 — TLB가 캐시한 1단계 엔트리(디버그용). */
#define GET_TLBSLPTER(b, c)	 GET_CTX_REG(TLBSLPTER, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TLBSLPTER를 읽는다 — TLB가 캐시한 2단계 엔트리(디버그용). */
#define GET_BFBCR(b, c)		 GET_CTX_REG(BFBCR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 BFBCR를 읽는다 — 분기 예측 버퍼 제어 — 테이블 워크 예측을 조절한다. */
#define GET_CTX_TLBIALL(b, c)	 GET_CTX_REG(CTX_TLBIALL, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 CTX_TLBIALL를 읽는다 — 이 컨텍스트의 TLB를 통째로 무효화한다. */
#define GET_TLBIASID(b, c)	 GET_CTX_REG(TLBIASID, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TLBIASID를 읽는다 — 지정한 ASID의 TLB 항목만 무효화한다. */
#define GET_TLBIVA(b, c)	 GET_CTX_REG(TLBIVA, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TLBIVA를 읽는다 — 지정한 ASID의 특정 가상 주소만 무효화한다. */
#define GET_TLBIVAA(b, c)	 GET_CTX_REG(TLBIVAA, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 TLBIVAA를 읽는다 — ASID를 가리지 않고 특정 가상 주소를 무효화한다. */
#define GET_V2PPR(b, c)		 GET_CTX_REG(V2PPR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 V2PPR를 읽는다 — 특권 읽기 관점의 가상→물리 변환을 요청한다. */
#define GET_V2PPW(b, c)		 GET_CTX_REG(V2PPW, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 V2PPW를 읽는다 — 특권 쓰기 관점의 가상→물리 변환을 요청한다. */
#define GET_V2PUR(b, c)		 GET_CTX_REG(V2PUR, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 V2PUR를 읽는다 — 사용자 읽기 관점의 가상→물리 변환을 요청한다. */
#define GET_V2PUW(b, c)		 GET_CTX_REG(V2PUW, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 V2PUW를 읽는다 — 사용자 쓰기 관점의 가상→물리 변환을 요청한다. */
#define GET_RESUME(b, c)	 GET_CTX_REG(RESUME, (b), (c))	/* [한국어] 컨텍스트 뱅크 c의 RESUME를 읽는다 — 멈춰 세운 트랜잭션을 재개시키거나 종료시킨다. */


/* Global field setters / getters */
/* [한국어] 여기서부터는 레지스터 전체가 아니라 **필드 하나**를
 * 다루는 매크로다. 안쪽에서 읽고-고치고-쓰기를 하므로 다른 필드가
 * 보존되지만, 그 대가로 원자적이지 않다.
 * 아래 정의들은 레지스터별로 묶여 있고, 각 묶음의 앞에 그 레지스터
 * 이름이 주석으로 적혀 있다. */
/* Global Field Setters: */
/* CBACR_N */
#define SET_RWVMID(b, n, v)   SET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), RWVMID, v)	/* [한국어] CBACR_N[n]의 RWVMID 필드를 쓴다 — 라운드로빈 중재에 쓸 VMID. */
#define SET_RWE(b, n, v)      SET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), RWE, v)	/* [한국어] CBACR_N[n]의 RWE 필드를 쓴다 — 라운드로빈 중재 활성화. */
#define SET_RWGE(b, n, v)     SET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), RWGE, v)	/* [한국어] CBACR_N[n]의 RWGE 필드를 쓴다 — 라운드로빈 전역 활성화. */
#define SET_CBVMID(b, n, v)   SET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), CBVMID, v)	/* [한국어] CBACR_N[n]의 CBVMID 필드를 쓴다 — 이 컨텍스트 뱅크가 쓸 VMID. */
#define SET_IRPTNDX(b, n, v)  SET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), IRPTNDX, v)	/* [한국어] CBACR_N[n]의 IRPTNDX 필드를 쓴다 — 이 컨텍스트가 쓸 인터럽트 번호. */


/* M2VCBR_N */
#define SET_VMID(b, n, v)     SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), VMID, v)	/* [한국어] M2VCBR_N[n]의 VMID 필드를 쓴다 — 이 스트림에 부여할 VMID. */
#define SET_CBNDX(b, n, v)    SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), CBNDX, v)	/* [한국어] M2VCBR_N[n]의 CBNDX 필드를 쓴다 — 이 스트림이 쓸 컨텍스트 뱅크 번호. */
#define SET_BYPASSD(b, n, v)  SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BYPASSD, v)	/* [한국어] M2VCBR_N[n]의 BYPASSD 필드를 쓴다 — 통과 모드에서의 디버그 표시. */
#define SET_BPRCOSH(b, n, v)  SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPRCOSH, v)	/* [한국어] M2VCBR_N[n]의 BPRCOSH 필드를 쓴다 — 통과 시 외부 공유 캐시 정책. */
#define SET_BPRCISH(b, n, v)  SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPRCISH, v)	/* [한국어] M2VCBR_N[n]의 BPRCISH 필드를 쓴다 — 통과 시 내부 공유 캐시 정책. */
#define SET_BPRCNSH(b, n, v)  SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPRCNSH, v)	/* [한국어] M2VCBR_N[n]의 BPRCNSH 필드를 쓴다 — 통과 시 비공유 캐시 정책. */
#define SET_BPSHCFG(b, n, v)  SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPSHCFG, v)	/* [한국어] M2VCBR_N[n]의 BPSHCFG 필드를 쓴다 — 통과 시 공유 속성 설정. */
#define SET_NSCFG(b, n, v)    SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), NSCFG, v)	/* [한국어] M2VCBR_N[n]의 NSCFG 필드를 쓴다 — 보안/비보안 속성 설정. */
#define SET_BPMTCFG(b, n, v)  SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPMTCFG, v)	/* [한국어] M2VCBR_N[n]의 BPMTCFG 필드를 쓴다 — 통과 시 메모리 타입을 덮어쓸지 여부. */
/* [한국어] M2VCBR_N[n]의 BPMEMTYPE 필드를 쓴다 — 통과 시 강제할 메모리 타입. */
#define SET_BPMEMTYPE(b, n, v) \
	SET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPMEMTYPE, v)	/* [한국어] 통과 모드에서 강제할 메모리 타입을 M2VCBR_N[n]에 쓴다. */


/* CR */
#define SET_RPUE(b, v)		 SET_GLOBAL_FIELD(b, CR, RPUE, v)	/* [한국어] CR의 RPUE 필드를 쓴다 — RPU 활성화. */
#define SET_RPUERE(b, v)	 SET_GLOBAL_FIELD(b, CR, RPUERE, v)	/* [한국어] CR의 RPUERE 필드를 쓴다 — RPU 오류 응답 활성화. */
#define SET_RPUEIE(b, v)	 SET_GLOBAL_FIELD(b, CR, RPUEIE, v)	/* [한국어] CR의 RPUEIE 필드를 쓴다 — RPU 오류 인터럽트 활성화. */
#define SET_DCDEE(b, v)		 SET_GLOBAL_FIELD(b, CR, DCDEE, v)	/* [한국어] CR의 DCDEE 필드를 쓴다 — 디코드 오류 인터럽트 활성화. */
#define SET_CLIENTPD(b, v)       SET_GLOBAL_FIELD(b, CR, CLIENTPD, v)	/* [한국어] CR의 CLIENTPD 필드를 쓴다 — 클라이언트를 통과 모드로 둘지 여부. */
#define SET_STALLD(b, v)	 SET_GLOBAL_FIELD(b, CR, STALLD, v)	/* [한국어] CR의 STALLD 필드를 쓴다 — 폴트 시 트랜잭션을 멈춰 세울지 여부. */
#define SET_TLBLKCRWE(b, v)      SET_GLOBAL_FIELD(b, CR, TLBLKCRWE, v)	/* [한국어] CR의 TLBLKCRWE 필드를 쓴다 — TLB 잠금 레지스터 쓰기 허용. */
#define SET_CR_TLBIALLCFG(b, v)  SET_GLOBAL_FIELD(b, CR, CR_TLBIALLCFG, v)	/* [한국어] CR의 CR_TLBIALLCFG 필드를 쓴다 — 전역 TLB 무효화의 범위 설정. */
#define SET_TLBIVMIDCFG(b, v)    SET_GLOBAL_FIELD(b, CR, TLBIVMIDCFG, v)	/* [한국어] CR의 TLBIVMIDCFG 필드를 쓴다 — VMID 무효화의 범위 설정. */
#define SET_CR_HUME(b, v)        SET_GLOBAL_FIELD(b, CR, CR_HUME, v)	/* [한국어] CR의 CR_HUME 필드를 쓴다 — 하드웨어 업데이트 확장 활성화. */


/* ESR */
#define SET_CFG(b, v)		 SET_GLOBAL_FIELD(b, ESR, CFG, v)	/* [한국어] ESR의 CFG 필드를 쓴다 — 설정 오류가 났다. */
#define SET_BYPASS(b, v)	 SET_GLOBAL_FIELD(b, ESR, BYPASS, v)	/* [한국어] ESR의 BYPASS 필드를 쓴다 — 통과 모드에서 오류가 났다. */
#define SET_ESR_MULTI(b, v)      SET_GLOBAL_FIELD(b, ESR, ESR_MULTI, v)	/* [한국어] ESR의 ESR_MULTI 필드를 쓴다 — 오류가 여러 번 겹쳤다. */


/* ESYNR0 */
#define SET_ESYNR0_AMID(b, v)    SET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_AMID, v)	/* [한국어] ESYNR0의 ESYNR0_AMID 필드를 쓴다 — 오류를 낸 마스터 ID. */
#define SET_ESYNR0_APID(b, v)    SET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_APID, v)	/* [한국어] ESYNR0의 ESYNR0_APID 필드를 쓴다 — 오류를 낸 페이지 ID. */
#define SET_ESYNR0_ABID(b, v)    SET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_ABID, v)	/* [한국어] ESYNR0의 ESYNR0_ABID 필드를 쓴다 — 오류를 낸 버스 ID. */
#define SET_ESYNR0_AVMID(b, v)   SET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_AVMID, v)	/* [한국어] ESYNR0의 ESYNR0_AVMID 필드를 쓴다 — 오류를 낸 VMID. */
#define SET_ESYNR0_ATID(b, v)    SET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_ATID, v)	/* [한국어] ESYNR0의 ESYNR0_ATID 필드를 쓴다 — 오류를 낸 트랜잭션 ID. */


/* ESYNR1 */
/* [한국어] ESYNR1의 ESYNR1_AMEMTYPE 필드를 쓴다 — 그 트랜잭션의 메모리 타입. */
#define SET_ESYNR1_AMEMTYPE(b, v) \
			SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AMEMTYPE, v)	/* [한국어] 오류를 낸 트랜잭션의 메모리 타입 필드에 쓴다(복원용). */
#define SET_ESYNR1_ASHARED(b, v)  SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ASHARED, v)	/* [한국어] ESYNR1의 ESYNR1_ASHARED 필드를 쓴다 — 공유 접근이었는가. */
/* [한국어] ESYNR1의 ESYNR1_AINNERSHARED 필드를 쓴다 — 내부 공유 접근이었는가. */
#define SET_ESYNR1_AINNERSHARED(b, v) \
			SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AINNERSHARED, v)	/* [한국어] 내부 공유 여부 필드에 쓴다(복원용). */
#define SET_ESYNR1_APRIV(b, v)   SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_APRIV, v)	/* [한국어] ESYNR1의 ESYNR1_APRIV 필드를 쓴다 — 특권 접근이었는가. */
#define SET_ESYNR1_APROTNS(b, v) SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_APROTNS, v)	/* [한국어] ESYNR1의 ESYNR1_APROTNS 필드를 쓴다 — 비보안 접근이었는가. */
#define SET_ESYNR1_AINST(b, v)   SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AINST, v)	/* [한국어] ESYNR1의 ESYNR1_AINST 필드를 쓴다 — 명령 인출이었는가. */
#define SET_ESYNR1_AWRITE(b, v)  SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AWRITE, v)	/* [한국어] ESYNR1의 ESYNR1_AWRITE 필드를 쓴다 — 쓰기였는가. */
#define SET_ESYNR1_ABURST(b, v)  SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ABURST, v)	/* [한국어] ESYNR1의 ESYNR1_ABURST 필드를 쓴다 — 버스트 종류. */
#define SET_ESYNR1_ALEN(b, v)    SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ALEN, v)	/* [한국어] ESYNR1의 ESYNR1_ALEN 필드를 쓴다 — 버스트 길이. */
#define SET_ESYNR1_ASIZE(b, v)   SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ASIZE, v)	/* [한국어] ESYNR1의 ESYNR1_ASIZE 필드를 쓴다 — 전송 크기. */
#define SET_ESYNR1_ALOCK(b, v)   SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ALOCK, v)	/* [한국어] ESYNR1의 ESYNR1_ALOCK 필드를 쓴다 — 잠금 전송이었는가. */
#define SET_ESYNR1_AOOO(b, v)    SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AOOO, v)	/* [한국어] ESYNR1의 ESYNR1_AOOO 필드를 쓴다 — 순서 없는 전송이었는가. */
#define SET_ESYNR1_AFULL(b, v)   SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AFULL, v)	/* [한국어] ESYNR1의 ESYNR1_AFULL 필드를 쓴다 — 버퍼가 가득 찼는가. */
#define SET_ESYNR1_AC(b, v)      SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AC, v)	/* [한국어] ESYNR1의 ESYNR1_AC 필드를 쓴다 — 캐시 가능 접근이었는가. */
#define SET_ESYNR1_DCD(b, v)     SET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_DCD, v)	/* [한국어] ESYNR1의 ESYNR1_DCD 필드를 쓴다 — 디코드 오류였는가. */


/* TESTBUSCR */
#define SET_TBE(b, v)		 SET_GLOBAL_FIELD(b, TESTBUSCR, TBE, v)	/* [한국어] TESTBUSCR의 TBE 필드를 쓴다 — 테스트 버스 활성화. */
#define SET_SPDMBE(b, v)	 SET_GLOBAL_FIELD(b, TESTBUSCR, SPDMBE, v)	/* [한국어] TESTBUSCR의 SPDMBE 필드를 쓴다 — SPDM 버스 활성화. */
#define SET_WGSEL(b, v)		 SET_GLOBAL_FIELD(b, TESTBUSCR, WGSEL, v)	/* [한국어] TESTBUSCR의 WGSEL 필드를 쓴다 — 파형 그룹 선택. */
#define SET_TBLSEL(b, v)	 SET_GLOBAL_FIELD(b, TESTBUSCR, TBLSEL, v)	/* [한국어] TESTBUSCR의 TBLSEL 필드를 쓴다 — 테스트 버스 하위 선택. */
#define SET_TBHSEL(b, v)	 SET_GLOBAL_FIELD(b, TESTBUSCR, TBHSEL, v)	/* [한국어] TESTBUSCR의 TBHSEL 필드를 쓴다 — 테스트 버스 상위 선택. */
#define SET_SPDM0SEL(b, v)       SET_GLOBAL_FIELD(b, TESTBUSCR, SPDM0SEL, v)	/* [한국어] TESTBUSCR의 SPDM0SEL 필드를 쓴다 — SPDM 0번 선택. */
#define SET_SPDM1SEL(b, v)       SET_GLOBAL_FIELD(b, TESTBUSCR, SPDM1SEL, v)	/* [한국어] TESTBUSCR의 SPDM1SEL 필드를 쓴다 — SPDM 1번 선택. */
#define SET_SPDM2SEL(b, v)       SET_GLOBAL_FIELD(b, TESTBUSCR, SPDM2SEL, v)	/* [한국어] TESTBUSCR의 SPDM2SEL 필드를 쓴다 — SPDM 2번 선택. */
#define SET_SPDM3SEL(b, v)       SET_GLOBAL_FIELD(b, TESTBUSCR, SPDM3SEL, v)	/* [한국어] TESTBUSCR의 SPDM3SEL 필드를 쓴다 — SPDM 3번 선택. */


/* TLBIVMID */
#define SET_TLBIVMID_VMID(b, v)  SET_GLOBAL_FIELD(b, TLBIVMID, TLBIVMID_VMID, v)	/* [한국어] TLBIVMID의 TLBIVMID_VMID 필드를 쓴다 — 무효화할 VMID. */


/* TLBRSW */
#define SET_TLBRSW_INDEX(b, v)   SET_GLOBAL_FIELD(b, TLBRSW, TLBRSW_INDEX, v)	/* [한국어] TLBRSW의 TLBRSW_INDEX 필드를 쓴다 — 읽거나 쓸 TLB 항목의 번호. */
#define SET_TLBBFBS(b, v)	 SET_GLOBAL_FIELD(b, TLBRSW, TLBBFBS, v)	/* [한국어] TLBRSW의 TLBBFBS 필드를 쓴다 — 분기 예측 버퍼 쪽을 선택할지 여부. */


/* TLBTR0 */
#define SET_PR(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, PR, v)	/* [한국어] TLBTR0의 PR 필드를 쓴다 — 특권 읽기 허용. */
#define SET_PW(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, PW, v)	/* [한국어] TLBTR0의 PW 필드를 쓴다 — 특권 쓰기 허용. */
#define SET_UR(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, UR, v)	/* [한국어] TLBTR0의 UR 필드를 쓴다 — 사용자 읽기 허용. */
#define SET_UW(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, UW, v)	/* [한국어] TLBTR0의 UW 필드를 쓴다 — 사용자 쓰기 허용. */
#define SET_XN(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, XN, v)	/* [한국어] TLBTR0의 XN 필드를 쓴다 — 실행 금지. */
#define SET_NSDESC(b, v)	 SET_GLOBAL_FIELD(b, TLBTR0, NSDESC, v)	/* [한국어] TLBTR0의 NSDESC 필드를 쓴다 — 비보안 서술자에서 왔다. */
#define SET_ISH(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, ISH, v)	/* [한국어] TLBTR0의 ISH 필드를 쓴다 — 내부 공유. */
#define SET_SH(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, SH, v)	/* [한국어] TLBTR0의 SH 필드를 쓴다 — 공유 속성. */
#define SET_MT(b, v)		 SET_GLOBAL_FIELD(b, TLBTR0, MT, v)	/* [한국어] TLBTR0의 MT 필드를 쓴다 — 메모리 타입. */
#define SET_DPSIZR(b, v)	 SET_GLOBAL_FIELD(b, TLBTR0, DPSIZR, v)	/* [한국어] TLBTR0의 DPSIZR 필드를 쓴다 — 서술자 페이지 크기(행). */
#define SET_DPSIZC(b, v)	 SET_GLOBAL_FIELD(b, TLBTR0, DPSIZC, v)	/* [한국어] TLBTR0의 DPSIZC 필드를 쓴다 — 서술자 페이지 크기(열). */


/* TLBTR1 */
#define SET_TLBTR1_VMID(b, v)    SET_GLOBAL_FIELD(b, TLBTR1, TLBTR1_VMID, v)	/* [한국어] TLBTR1의 TLBTR1_VMID 필드를 쓴다 — 이 TLB 항목의 VMID. */
#define SET_TLBTR1_PA(b, v)      SET_GLOBAL_FIELD(b, TLBTR1, TLBTR1_PA, v)	/* [한국어] TLBTR1의 TLBTR1_PA 필드를 쓴다 — 이 TLB 항목의 물리 주소. */


/* TLBTR2 */
#define SET_TLBTR2_ASID(b, v)    SET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_ASID, v)	/* [한국어] TLBTR2의 TLBTR2_ASID 필드를 쓴다 — 이 TLB 항목의 ASID. */
#define SET_TLBTR2_V(b, v)       SET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_V, v)	/* [한국어] TLBTR2의 TLBTR2_V 필드를 쓴다 — 이 항목이 유효한가. */
#define SET_TLBTR2_NSTID(b, v)   SET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_NSTID, v)	/* [한국어] TLBTR2의 TLBTR2_NSTID 필드를 쓴다 — 비보안 변환 식별자. */
#define SET_TLBTR2_NV(b, v)      SET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_NV, v)	/* [한국어] TLBTR2의 TLBTR2_NV 필드를 쓴다 — 비보안 유효 비트. */
#define SET_TLBTR2_VA(b, v)      SET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_VA, v)	/* [한국어] TLBTR2의 TLBTR2_VA 필드를 쓴다 — 이 TLB 항목의 가상 주소. */


/* Global Field Getters */
/* CBACR_N */
#define GET_RWVMID(b, n)	 GET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), RWVMID)	/* [한국어] CBACR_N[n]의 RWVMID 필드를 읽는다 — 라운드로빈 중재에 쓸 VMID. */
#define GET_RWE(b, n)		 GET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), RWE)	/* [한국어] CBACR_N[n]의 RWE 필드를 읽는다 — 라운드로빈 중재 활성화. */
#define GET_RWGE(b, n)		 GET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), RWGE)	/* [한국어] CBACR_N[n]의 RWGE 필드를 읽는다 — 라운드로빈 전역 활성화. */
#define GET_CBVMID(b, n)	 GET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), CBVMID)	/* [한국어] CBACR_N[n]의 CBVMID 필드를 읽는다 — 이 컨텍스트 뱅크가 쓸 VMID. */
#define GET_IRPTNDX(b, n)	 GET_GLOBAL_FIELD(b, (n<<2)|(CBACR_N), IRPTNDX)	/* [한국어] CBACR_N[n]의 IRPTNDX 필드를 읽는다 — 이 컨텍스트가 쓸 인터럽트 번호. */


/* M2VCBR_N */
#define GET_VMID(b, n)       GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), VMID)	/* [한국어] M2VCBR_N[n]의 VMID 필드를 읽는다 — 이 스트림에 부여할 VMID. */
#define GET_CBNDX(b, n)      GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), CBNDX)	/* [한국어] M2VCBR_N[n]의 CBNDX 필드를 읽는다 — 이 스트림이 쓸 컨텍스트 뱅크 번호. */
#define GET_BYPASSD(b, n)    GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BYPASSD)	/* [한국어] M2VCBR_N[n]의 BYPASSD 필드를 읽는다 — 통과 모드에서의 디버그 표시. */
#define GET_BPRCOSH(b, n)    GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPRCOSH)	/* [한국어] M2VCBR_N[n]의 BPRCOSH 필드를 읽는다 — 통과 시 외부 공유 캐시 정책. */
#define GET_BPRCISH(b, n)    GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPRCISH)	/* [한국어] M2VCBR_N[n]의 BPRCISH 필드를 읽는다 — 통과 시 내부 공유 캐시 정책. */
#define GET_BPRCNSH(b, n)    GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPRCNSH)	/* [한국어] M2VCBR_N[n]의 BPRCNSH 필드를 읽는다 — 통과 시 비공유 캐시 정책. */
#define GET_BPSHCFG(b, n)    GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPSHCFG)	/* [한국어] M2VCBR_N[n]의 BPSHCFG 필드를 읽는다 — 통과 시 공유 속성 설정. */
#define GET_NSCFG(b, n)      GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), NSCFG)	/* [한국어] M2VCBR_N[n]의 NSCFG 필드를 읽는다 — 보안/비보안 속성 설정. */
#define GET_BPMTCFG(b, n)    GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPMTCFG)	/* [한국어] M2VCBR_N[n]의 BPMTCFG 필드를 읽는다 — 통과 시 메모리 타입을 덮어쓸지 여부. */
#define GET_BPMEMTYPE(b, n)  GET_GLOBAL_FIELD(b, (n<<2)|(M2VCBR_N), BPMEMTYPE)	/* [한국어] M2VCBR_N[n]의 BPMEMTYPE 필드를 읽는다 — 통과 시 강제할 메모리 타입. */


/* CR */
#define GET_RPUE(b)		 GET_GLOBAL_FIELD(b, CR, RPUE)	/* [한국어] CR의 RPUE 필드를 읽는다 — RPU 활성화. */
#define GET_RPUERE(b)		 GET_GLOBAL_FIELD(b, CR, RPUERE)	/* [한국어] CR의 RPUERE 필드를 읽는다 — RPU 오류 응답 활성화. */
#define GET_RPUEIE(b)		 GET_GLOBAL_FIELD(b, CR, RPUEIE)	/* [한국어] CR의 RPUEIE 필드를 읽는다 — RPU 오류 인터럽트 활성화. */
#define GET_DCDEE(b)		 GET_GLOBAL_FIELD(b, CR, DCDEE)	/* [한국어] CR의 DCDEE 필드를 읽는다 — 디코드 오류 인터럽트 활성화. */
#define GET_CLIENTPD(b)		 GET_GLOBAL_FIELD(b, CR, CLIENTPD)	/* [한국어] CR의 CLIENTPD 필드를 읽는다 — 클라이언트를 통과 모드로 둘지 여부. */
#define GET_STALLD(b)		 GET_GLOBAL_FIELD(b, CR, STALLD)	/* [한국어] CR의 STALLD 필드를 읽는다 — 폴트 시 트랜잭션을 멈춰 세울지 여부. */
#define GET_TLBLKCRWE(b)	 GET_GLOBAL_FIELD(b, CR, TLBLKCRWE)	/* [한국어] CR의 TLBLKCRWE 필드를 읽는다 — TLB 잠금 레지스터 쓰기 허용. */
#define GET_CR_TLBIALLCFG(b)	 GET_GLOBAL_FIELD(b, CR, CR_TLBIALLCFG)	/* [한국어] CR의 CR_TLBIALLCFG 필드를 읽는다 — 전역 TLB 무효화의 범위 설정. */
#define GET_TLBIVMIDCFG(b)	 GET_GLOBAL_FIELD(b, CR, TLBIVMIDCFG)	/* [한국어] CR의 TLBIVMIDCFG 필드를 읽는다 — VMID 무효화의 범위 설정. */
#define GET_CR_HUME(b)		 GET_GLOBAL_FIELD(b, CR, CR_HUME)	/* [한국어] CR의 CR_HUME 필드를 읽는다 — 하드웨어 업데이트 확장 활성화. */


/* ESR */
#define GET_CFG(b)		 GET_GLOBAL_FIELD(b, ESR, CFG)	/* [한국어] ESR의 CFG 필드를 읽는다 — 설정 오류가 났다. */
#define GET_BYPASS(b)		 GET_GLOBAL_FIELD(b, ESR, BYPASS)	/* [한국어] ESR의 BYPASS 필드를 읽는다 — 통과 모드에서 오류가 났다. */
#define GET_ESR_MULTI(b)	 GET_GLOBAL_FIELD(b, ESR, ESR_MULTI)	/* [한국어] ESR의 ESR_MULTI 필드를 읽는다 — 오류가 여러 번 겹쳤다. */


/* ESYNR0 */
#define GET_ESYNR0_AMID(b)	 GET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_AMID)	/* [한국어] ESYNR0의 ESYNR0_AMID 필드를 읽는다 — 오류를 낸 마스터 ID. */
#define GET_ESYNR0_APID(b)	 GET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_APID)	/* [한국어] ESYNR0의 ESYNR0_APID 필드를 읽는다 — 오류를 낸 페이지 ID. */
#define GET_ESYNR0_ABID(b)	 GET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_ABID)	/* [한국어] ESYNR0의 ESYNR0_ABID 필드를 읽는다 — 오류를 낸 버스 ID. */
#define GET_ESYNR0_AVMID(b)	 GET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_AVMID)	/* [한국어] ESYNR0의 ESYNR0_AVMID 필드를 읽는다 — 오류를 낸 VMID. */
#define GET_ESYNR0_ATID(b)	 GET_GLOBAL_FIELD(b, ESYNR0, ESYNR0_ATID)	/* [한국어] ESYNR0의 ESYNR0_ATID 필드를 읽는다 — 오류를 낸 트랜잭션 ID. */


/* ESYNR1 */
#define GET_ESYNR1_AMEMTYPE(b)   GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AMEMTYPE)	/* [한국어] ESYNR1의 ESYNR1_AMEMTYPE 필드를 읽는다 — 그 트랜잭션의 메모리 타입. */
#define GET_ESYNR1_ASHARED(b)    GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ASHARED)	/* [한국어] ESYNR1의 ESYNR1_ASHARED 필드를 읽는다 — 공유 접근이었는가. */
/* [한국어] ESYNR1의 ESYNR1_AINNERSHARED 필드를 읽는다 — 내부 공유 접근이었는가. */
#define GET_ESYNR1_AINNERSHARED(b) \
			GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AINNERSHARED)	/* [한국어] 내부 공유 여부 필드를 읽는다. */
#define GET_ESYNR1_APRIV(b)      GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_APRIV)	/* [한국어] ESYNR1의 ESYNR1_APRIV 필드를 읽는다 — 특권 접근이었는가. */
#define GET_ESYNR1_APROTNS(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_APROTNS)	/* [한국어] ESYNR1의 ESYNR1_APROTNS 필드를 읽는다 — 비보안 접근이었는가. */
#define GET_ESYNR1_AINST(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AINST)	/* [한국어] ESYNR1의 ESYNR1_AINST 필드를 읽는다 — 명령 인출이었는가. */
#define GET_ESYNR1_AWRITE(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AWRITE)	/* [한국어] ESYNR1의 ESYNR1_AWRITE 필드를 읽는다 — 쓰기였는가. */
#define GET_ESYNR1_ABURST(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ABURST)	/* [한국어] ESYNR1의 ESYNR1_ABURST 필드를 읽는다 — 버스트 종류. */
#define GET_ESYNR1_ALEN(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ALEN)	/* [한국어] ESYNR1의 ESYNR1_ALEN 필드를 읽는다 — 버스트 길이. */
#define GET_ESYNR1_ASIZE(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ASIZE)	/* [한국어] ESYNR1의 ESYNR1_ASIZE 필드를 읽는다 — 전송 크기. */
#define GET_ESYNR1_ALOCK(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_ALOCK)	/* [한국어] ESYNR1의 ESYNR1_ALOCK 필드를 읽는다 — 잠금 전송이었는가. */
#define GET_ESYNR1_AOOO(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AOOO)	/* [한국어] ESYNR1의 ESYNR1_AOOO 필드를 읽는다 — 순서 없는 전송이었는가. */
#define GET_ESYNR1_AFULL(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AFULL)	/* [한국어] ESYNR1의 ESYNR1_AFULL 필드를 읽는다 — 버퍼가 가득 찼는가. */
#define GET_ESYNR1_AC(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_AC)	/* [한국어] ESYNR1의 ESYNR1_AC 필드를 읽는다 — 캐시 가능 접근이었는가. */
#define GET_ESYNR1_DCD(b)	 GET_GLOBAL_FIELD(b, ESYNR1, ESYNR1_DCD)	/* [한국어] ESYNR1의 ESYNR1_DCD 필드를 읽는다 — 디코드 오류였는가. */


/* IDR */
#define GET_NM2VCBMT(b)		 GET_GLOBAL_FIELD(b, IDR, NM2VCBMT)	/* [한국어] IDR의 NM2VCBMT 필드를 읽는다 — 스트림 대응표의 항목 수. */
#define GET_HTW(b)		 GET_GLOBAL_FIELD(b, IDR, HTW)	/* [한국어] IDR의 HTW 필드를 읽는다 — 하드웨어 테이블 워크 지원 여부. */
#define GET_HUM(b)		 GET_GLOBAL_FIELD(b, IDR, HUM)	/* [한국어] IDR의 HUM 필드를 읽는다 — 하드웨어 업데이트 확장 지원 여부. */
#define GET_TLBSIZE(b)		 GET_GLOBAL_FIELD(b, IDR, TLBSIZE)	/* [한국어] IDR의 TLBSIZE 필드를 읽는다 — TLB 항목 수. */
#define GET_NCB(b)		 GET_GLOBAL_FIELD(b, IDR, NCB)	/* [한국어] IDR의 NCB 필드를 읽는다 — 컨텍스트 뱅크의 개수. */
#define GET_NIRPT(b)		 GET_GLOBAL_FIELD(b, IDR, NIRPT)	/* [한국어] IDR의 NIRPT 필드를 읽는다 — 인터럽트 선의 개수. */


/* REV */
#define GET_MAJOR(b)		 GET_GLOBAL_FIELD(b, REV, MAJOR)	/* [한국어] REV의 MAJOR 필드를 읽는다. */
#define GET_MINOR(b)		 GET_GLOBAL_FIELD(b, REV, MINOR)	/* [한국어] REV의 MINOR 필드를 읽는다. */


/* TESTBUSCR */
#define GET_TBE(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, TBE)	/* [한국어] TESTBUSCR의 TBE 필드를 읽는다 — 테스트 버스 활성화. */
#define GET_SPDMBE(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, SPDMBE)	/* [한국어] TESTBUSCR의 SPDMBE 필드를 읽는다 — SPDM 버스 활성화. */
#define GET_WGSEL(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, WGSEL)	/* [한국어] TESTBUSCR의 WGSEL 필드를 읽는다 — 파형 그룹 선택. */
#define GET_TBLSEL(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, TBLSEL)	/* [한국어] TESTBUSCR의 TBLSEL 필드를 읽는다 — 테스트 버스 하위 선택. */
#define GET_TBHSEL(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, TBHSEL)	/* [한국어] TESTBUSCR의 TBHSEL 필드를 읽는다 — 테스트 버스 상위 선택. */
#define GET_SPDM0SEL(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, SPDM0SEL)	/* [한국어] TESTBUSCR의 SPDM0SEL 필드를 읽는다 — SPDM 0번 선택. */
#define GET_SPDM1SEL(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, SPDM1SEL)	/* [한국어] TESTBUSCR의 SPDM1SEL 필드를 읽는다 — SPDM 1번 선택. */
#define GET_SPDM2SEL(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, SPDM2SEL)	/* [한국어] TESTBUSCR의 SPDM2SEL 필드를 읽는다 — SPDM 2번 선택. */
#define GET_SPDM3SEL(b)		 GET_GLOBAL_FIELD(b, TESTBUSCR, SPDM3SEL)	/* [한국어] TESTBUSCR의 SPDM3SEL 필드를 읽는다 — SPDM 3번 선택. */


/* TLBIVMID */
#define GET_TLBIVMID_VMID(b)	 GET_GLOBAL_FIELD(b, TLBIVMID, TLBIVMID_VMID)	/* [한국어] TLBIVMID의 TLBIVMID_VMID 필드를 읽는다 — 무효화할 VMID. */


/* TLBTR0 */
#define GET_PR(b)		 GET_GLOBAL_FIELD(b, TLBTR0, PR)	/* [한국어] TLBTR0의 PR 필드를 읽는다 — 특권 읽기 허용. */
#define GET_PW(b)		 GET_GLOBAL_FIELD(b, TLBTR0, PW)	/* [한국어] TLBTR0의 PW 필드를 읽는다 — 특권 쓰기 허용. */
#define GET_UR(b)		 GET_GLOBAL_FIELD(b, TLBTR0, UR)	/* [한국어] TLBTR0의 UR 필드를 읽는다 — 사용자 읽기 허용. */
#define GET_UW(b)		 GET_GLOBAL_FIELD(b, TLBTR0, UW)	/* [한국어] TLBTR0의 UW 필드를 읽는다 — 사용자 쓰기 허용. */
#define GET_XN(b)		 GET_GLOBAL_FIELD(b, TLBTR0, XN)	/* [한국어] TLBTR0의 XN 필드를 읽는다 — 실행 금지. */
#define GET_NSDESC(b)		 GET_GLOBAL_FIELD(b, TLBTR0, NSDESC)	/* [한국어] TLBTR0의 NSDESC 필드를 읽는다 — 비보안 서술자에서 왔다. */
#define GET_ISH(b)		 GET_GLOBAL_FIELD(b, TLBTR0, ISH)	/* [한국어] TLBTR0의 ISH 필드를 읽는다 — 내부 공유. */
#define GET_SH(b)		 GET_GLOBAL_FIELD(b, TLBTR0, SH)	/* [한국어] TLBTR0의 SH 필드를 읽는다 — 공유 속성. */
#define GET_MT(b)		 GET_GLOBAL_FIELD(b, TLBTR0, MT)	/* [한국어] TLBTR0의 MT 필드를 읽는다 — 메모리 타입. */
#define GET_DPSIZR(b)		 GET_GLOBAL_FIELD(b, TLBTR0, DPSIZR)	/* [한국어] TLBTR0의 DPSIZR 필드를 읽는다 — 서술자 페이지 크기(행). */
#define GET_DPSIZC(b)		 GET_GLOBAL_FIELD(b, TLBTR0, DPSIZC)	/* [한국어] TLBTR0의 DPSIZC 필드를 읽는다 — 서술자 페이지 크기(열). */


/* TLBTR1 */
#define GET_TLBTR1_VMID(b)	 GET_GLOBAL_FIELD(b, TLBTR1, TLBTR1_VMID)	/* [한국어] TLBTR1의 TLBTR1_VMID 필드를 읽는다 — 이 TLB 항목의 VMID. */
#define GET_TLBTR1_PA(b)	 GET_GLOBAL_FIELD(b, TLBTR1, TLBTR1_PA)	/* [한국어] TLBTR1의 TLBTR1_PA 필드를 읽는다 — 이 TLB 항목의 물리 주소. */


/* TLBTR2 */
#define GET_TLBTR2_ASID(b)	 GET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_ASID)	/* [한국어] TLBTR2의 TLBTR2_ASID 필드를 읽는다 — 이 TLB 항목의 ASID. */
#define GET_TLBTR2_V(b)		 GET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_V)	/* [한국어] TLBTR2의 TLBTR2_V 필드를 읽는다 — 이 항목이 유효한가. */
#define GET_TLBTR2_NSTID(b)	 GET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_NSTID)	/* [한국어] TLBTR2의 TLBTR2_NSTID 필드를 읽는다 — 비보안 변환 식별자. */
#define GET_TLBTR2_NV(b)	 GET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_NV)	/* [한국어] TLBTR2의 TLBTR2_NV 필드를 읽는다 — 비보안 유효 비트. */
#define GET_TLBTR2_VA(b)	 GET_GLOBAL_FIELD(b, TLBTR2, TLBTR2_VA)	/* [한국어] TLBTR2의 TLBTR2_VA 필드를 읽는다 — 이 TLB 항목의 가상 주소. */


/* Context Register setters / getters */
/* [한국어] 컨텍스트 뱅크 레지스터의 필드 단위 접근이다.
 * 전역 쪽과 형태가 같고 뱅크 번호가 하나 더 붙을 뿐이다. */
/* Context Register setters */
/* ACTLR */
#define SET_CFERE(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, CFERE, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 CFERE 필드를 쓴다 — 컨텍스트 폴트 시 오류 응답 활성화. */
#define SET_CFEIE(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, CFEIE, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 CFEIE 필드를 쓴다 — 컨텍스트 폴트 인터럽트 활성화. */
#define SET_PTSHCFG(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, PTSHCFG, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PTSHCFG 필드를 쓴다 — 페이지 테이블 접근의 공유 속성. */
#define SET_RCOSH(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, RCOSH, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 RCOSH 필드를 쓴다 — 외부 공유 읽기 정책. */
#define SET_RCISH(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, RCISH, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 RCISH 필드를 쓴다 — 내부 공유 읽기 정책. */
#define SET_RCNSH(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, RCNSH, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 RCNSH 필드를 쓴다 — 비공유 읽기 정책. */
#define SET_PRIVCFG(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, PRIVCFG, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PRIVCFG 필드를 쓴다 — 특권 속성 설정. */
#define SET_DNA(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, DNA, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 DNA 필드를 쓴다 — 접근 플래그 갱신 금지. */
#define SET_DNLV2PA(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, DNLV2PA, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 DNLV2PA 필드를 쓴다 — 2단계 물리 주소 갱신 금지. */
#define SET_TLBMCFG(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, TLBMCFG, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 TLBMCFG 필드를 쓴다 — TLB 다중 적중 처리 설정. */
#define SET_CFCFG(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, CFCFG, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 CFCFG 필드를 쓴다 — 폴트 시 멈춰 세울지 종료할지. */
#define SET_TIPCF(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, TIPCF, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 TIPCF 필드를 쓴다 — 변환 진행 중 폴트 설정. */
#define SET_V2PCFG(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, V2PCFG, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 V2PCFG 필드를 쓴다 — 가상→물리 요청의 동작 설정. */
#define SET_HUME(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, HUME, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 HUME 필드를 쓴다 — 하드웨어 업데이트 확장 활성화. */
#define SET_PTMTCFG(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, PTMTCFG, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PTMTCFG 필드를 쓴다 — 테이블 접근의 메모리 타입 덮어쓰기. */
#define SET_PTMEMTYPE(b, c, v)	 SET_CONTEXT_FIELD(b, c, ACTLR, PTMEMTYPE, v)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PTMEMTYPE 필드를 쓴다 — 테이블 접근에 강제할 메모리 타입. */


/* BFBCR */
#define SET_BFBDFE(b, c, v)	 SET_CONTEXT_FIELD(b, c, BFBCR, BFBDFE, v)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 BFBDFE 필드를 쓴다 — 분기 예측 데이터 인출 활성화. */
#define SET_BFBSFE(b, c, v)	 SET_CONTEXT_FIELD(b, c, BFBCR, BFBSFE, v)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 BFBSFE 필드를 쓴다 — 분기 예측 추측 인출 활성화. */
#define SET_SFVS(b, c, v)	 SET_CONTEXT_FIELD(b, c, BFBCR, SFVS, v)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 SFVS 필드를 쓴다 — 추측 인출의 유효 크기. */
#define SET_FLVIC(b, c, v)	 SET_CONTEXT_FIELD(b, c, BFBCR, FLVIC, v)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 FLVIC 필드를 쓴다 — 1단계 예측 항목 수. */
#define SET_SLVIC(b, c, v)	 SET_CONTEXT_FIELD(b, c, BFBCR, SLVIC, v)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 SLVIC 필드를 쓴다 — 2단계 예측 항목 수. */


/* CONTEXTIDR */
/* [한국어] 컨텍스트 뱅크 c의 CONTEXTIDR에서 CONTEXTIDR_ASID 필드를 쓴다 — 이 컨텍스트의 ASID — TLB 태그가 된다. */
#define SET_CONTEXTIDR_ASID(b, c, v)   \
		SET_CONTEXT_FIELD(b, c, CONTEXTIDR, CONTEXTIDR_ASID, v)
/* [한국어] 컨텍스트 뱅크 c의 CONTEXTIDR에서 PROCID 필드를 쓴다 — 소프트웨어가 쓰는 프로세스 식별자. */
#define SET_CONTEXTIDR_PROCID(b, c, v) \
		SET_CONTEXT_FIELD(b, c, CONTEXTIDR, PROCID, v)	/* [한국어] 소프트웨어가 쓰는 프로세스 식별자를 쓴다. */


/* FSR */
#define SET_TF(b, c, v)		 SET_CONTEXT_FIELD(b, c, FSR, TF, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 TF 필드를 쓴다 — 변환 폴트 — 매핑이 없다. */
#define SET_AFF(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSR, AFF, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 AFF 필드를 쓴다 — 접근 플래그 폴트. */
#define SET_APF(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSR, APF, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 APF 필드를 쓴다 — 권한 폴트. */
#define SET_TLBMF(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSR, TLBMF, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 TLBMF 필드를 쓴다 — TLB 다중 적중. */
#define SET_HTWDEEF(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSR, HTWDEEF, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 HTWDEEF 필드를 쓴다 — 테이블 워크 중 디코드 오류. */
#define SET_HTWSEEF(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSR, HTWSEEF, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 HTWSEEF 필드를 쓴다 — 테이블 워크 중 슬레이브 오류. */
#define SET_MHF(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSR, MHF, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 MHF 필드를 쓴다 — 다중 적중 폴트. */
#define SET_SL(b, c, v)		 SET_CONTEXT_FIELD(b, c, FSR, SL, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 SL 필드를 쓴다 — 두 번째 단계에서 났는가. */
#define SET_SS(b, c, v)		 SET_CONTEXT_FIELD(b, c, FSR, SS, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 SS 필드를 쓴다 — 트랜잭션이 멈춰 세워졌다. */
#define SET_MULTI(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSR, MULTI, v)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 MULTI 필드를 쓴다 — 폴트가 겹쳐서 났다. */


/* FSYNR0 */
#define SET_AMID(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR0, AMID, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 AMID 필드를 쓴다 — 폴트를 낸 마스터 ID. */
#define SET_APID(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR0, APID, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 APID 필드를 쓴다 — 폴트를 낸 페이지 ID. */
#define SET_ABID(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR0, ABID, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 ABID 필드를 쓴다 — 폴트를 낸 버스 ID. */
#define SET_ATID(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR0, ATID, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 ATID 필드를 쓴다 — 폴트를 낸 트랜잭션 ID. */


/* FSYNR1 */
#define SET_AMEMTYPE(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, AMEMTYPE, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AMEMTYPE 필드를 쓴다 — 그 트랜잭션의 메모리 타입. */
#define SET_ASHARED(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, ASHARED, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ASHARED 필드를 쓴다 — 공유 접근이었는가. */
/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AINNERSHARED 필드를 쓴다 — 내부 공유 접근이었는가. */
#define SET_AINNERSHARED(b, c, v)  \
				SET_CONTEXT_FIELD(b, c, FSYNR1, AINNERSHARED, v)	/* [한국어] 폴트를 낸 접근이 내부 공유였는지를 쓴다(복원용). */
#define SET_APRIV(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, APRIV, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 APRIV 필드를 쓴다 — 특권 접근이었는가. */
#define SET_APROTNS(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, APROTNS, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 APROTNS 필드를 쓴다 — 비보안 접근이었는가. */
#define SET_AINST(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, AINST, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AINST 필드를 쓴다 — 명령 인출이었는가. */
#define SET_AWRITE(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, AWRITE, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AWRITE 필드를 쓴다 — 쓰기였는가. */
#define SET_ABURST(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, ABURST, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ABURST 필드를 쓴다 — 버스트 종류. */
#define SET_ALEN(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, ALEN, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ALEN 필드를 쓴다 — 버스트 길이. */
/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 FSYNR1_ASIZE 필드를 쓴다 — 전송 크기. */
#define SET_FSYNR1_ASIZE(b, c, v) \
				SET_CONTEXT_FIELD(b, c, FSYNR1, FSYNR1_ASIZE, v)	/* [한국어] 폴트를 낸 접근의 전송 크기를 쓴다(복원용). */
#define SET_ALOCK(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, ALOCK, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ALOCK 필드를 쓴다 — 잠금 전송이었는가. */
#define SET_AFULL(b, c, v)	 SET_CONTEXT_FIELD(b, c, FSYNR1, AFULL, v)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AFULL 필드를 쓴다 — 버퍼가 가득 찼는가. */


/* NMRR */
#define SET_ICPC0(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC0, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC0 필드를 쓴다 — TEX 클래스 0의 내부 캐시 정책. */
#define SET_ICPC1(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC1, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC1 필드를 쓴다 — TEX 클래스 1의 내부 캐시 정책. */
#define SET_ICPC2(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC2, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC2 필드를 쓴다 — TEX 클래스 2의 내부 캐시 정책. */
#define SET_ICPC3(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC3, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC3 필드를 쓴다 — TEX 클래스 3의 내부 캐시 정책. */
#define SET_ICPC4(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC4, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC4 필드를 쓴다 — TEX 클래스 4의 내부 캐시 정책. */
#define SET_ICPC5(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC5, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC5 필드를 쓴다 — TEX 클래스 5의 내부 캐시 정책. */
#define SET_ICPC6(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC6, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC6 필드를 쓴다 — TEX 클래스 6의 내부 캐시 정책. */
#define SET_ICPC7(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, ICPC7, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC7 필드를 쓴다 — TEX 클래스 7의 내부 캐시 정책. */
#define SET_OCPC0(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC0, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC0 필드를 쓴다 — TEX 클래스 0의 외부 캐시 정책. */
#define SET_OCPC1(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC1, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC1 필드를 쓴다 — TEX 클래스 1의 외부 캐시 정책. */
#define SET_OCPC2(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC2, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC2 필드를 쓴다 — TEX 클래스 2의 외부 캐시 정책. */
#define SET_OCPC3(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC3, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC3 필드를 쓴다 — TEX 클래스 3의 외부 캐시 정책. */
#define SET_OCPC4(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC4, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC4 필드를 쓴다 — TEX 클래스 4의 외부 캐시 정책. */
#define SET_OCPC5(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC5, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC5 필드를 쓴다 — TEX 클래스 5의 외부 캐시 정책. */
#define SET_OCPC6(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC6, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC6 필드를 쓴다 — TEX 클래스 6의 외부 캐시 정책. */
#define SET_OCPC7(b, c, v)	 SET_CONTEXT_FIELD(b, c, NMRR, OCPC7, v)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC7 필드를 쓴다 — TEX 클래스 7의 외부 캐시 정책. */


/* PAR */
#define SET_FAULT(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, FAULT, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT 필드를 쓴다 — 변환이 실패했는가 — 이 비트로 아래 두 해석이 갈린다. */

#define SET_FAULT_TF(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, FAULT_TF, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_TF 필드를 쓴다 — 변환 폴트였다. */
#define SET_FAULT_AFF(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, FAULT_AFF, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_AFF 필드를 쓴다 — 접근 플래그 폴트였다. */
#define SET_FAULT_APF(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, FAULT_APF, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_APF 필드를 쓴다 — 권한 폴트였다. */
#define SET_FAULT_TLBMF(b, c, v) SET_CONTEXT_FIELD(b, c, PAR, FAULT_TLBMF, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_TLBMF 필드를 쓴다 — TLB 다중 적중이었다. */
/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_HTWDEEF 필드를 쓴다 — 워크 중 디코드 오류였다. */
#define SET_FAULT_HTWDEEF(b, c, v) \
				SET_CONTEXT_FIELD(b, c, PAR, FAULT_HTWDEEF, v)
/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_HTWSEEF 필드를 쓴다 — 워크 중 슬레이브 오류였다. */
#define SET_FAULT_HTWSEEF(b, c, v) \
				SET_CONTEXT_FIELD(b, c, PAR, FAULT_HTWSEEF, v)	/* [한국어] PAR에 워크 중 슬레이브 오류 비트를 쓴다. */
#define SET_FAULT_MHF(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, FAULT_MHF, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_MHF 필드를 쓴다 — 다중 적중 폴트였다. */
#define SET_FAULT_SL(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, FAULT_SL, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_SL 필드를 쓴다 — 두 번째 단계에서 났다. */
#define SET_FAULT_SS(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, FAULT_SS, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_SS 필드를 쓴다 — 멈춰 세워졌다. */

#define SET_NOFAULT_SS(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, NOFAULT_SS, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 NOFAULT_SS 필드를 쓴다. */
#define SET_NOFAULT_MT(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, NOFAULT_MT, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 NOFAULT_MT 필드를 쓴다. */
#define SET_NOFAULT_SH(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, NOFAULT_SH, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 NOFAULT_SH 필드를 쓴다. */
#define SET_NOFAULT_NS(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, NOFAULT_NS, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 NOFAULT_NS 필드를 쓴다. */
#define SET_NOFAULT_NOS(b, c, v) SET_CONTEXT_FIELD(b, c, PAR, NOFAULT_NOS, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 NOFAULT_NOS 필드를 쓴다. */
#define SET_NPFAULT_PA(b, c, v)	 SET_CONTEXT_FIELD(b, c, PAR, NPFAULT_PA, v)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 NPFAULT_PA 필드를 쓴다. */


/* PRRR */
#define SET_MTC0(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC0, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC0 필드를 쓴다 — TEX 클래스 0의 메모리 타입. */
#define SET_MTC1(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC1, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC1 필드를 쓴다 — TEX 클래스 1의 메모리 타입. */
#define SET_MTC2(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC2, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC2 필드를 쓴다 — TEX 클래스 2의 메모리 타입. */
#define SET_MTC3(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC3, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC3 필드를 쓴다 — TEX 클래스 3의 메모리 타입. */
#define SET_MTC4(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC4, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC4 필드를 쓴다 — TEX 클래스 4의 메모리 타입. */
#define SET_MTC5(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC5, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC5 필드를 쓴다 — TEX 클래스 5의 메모리 타입. */
#define SET_MTC6(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC6, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC6 필드를 쓴다 — TEX 클래스 6의 메모리 타입. */
#define SET_MTC7(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, MTC7, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC7 필드를 쓴다 — TEX 클래스 7의 메모리 타입. */
#define SET_SHDSH0(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, SHDSH0, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHDSH0 필드를 쓴다 — 장치 메모리 0의 공유 속성. */
#define SET_SHDSH1(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, SHDSH1, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHDSH1 필드를 쓴다 — 장치 메모리 1의 공유 속성. */
#define SET_SHNMSH0(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, SHNMSH0, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHNMSH0 필드를 쓴다 — 일반 메모리 0의 공유 속성. */
#define SET_SHNMSH1(b, c, v)     SET_CONTEXT_FIELD(b, c, PRRR, SHNMSH1, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHNMSH1 필드를 쓴다 — 일반 메모리 1의 공유 속성. */
#define SET_NOS0(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS0, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS0 필드를 쓴다 — TEX 클래스 0가 외부 공유인가. */
#define SET_NOS1(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS1, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS1 필드를 쓴다 — TEX 클래스 1가 외부 공유인가. */
#define SET_NOS2(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS2, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS2 필드를 쓴다 — TEX 클래스 2가 외부 공유인가. */
#define SET_NOS3(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS3, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS3 필드를 쓴다 — TEX 클래스 3가 외부 공유인가. */
#define SET_NOS4(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS4, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS4 필드를 쓴다 — TEX 클래스 4가 외부 공유인가. */
#define SET_NOS5(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS5, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS5 필드를 쓴다 — TEX 클래스 5가 외부 공유인가. */
#define SET_NOS6(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS6, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS6 필드를 쓴다 — TEX 클래스 6가 외부 공유인가. */
#define SET_NOS7(b, c, v)	 SET_CONTEXT_FIELD(b, c, PRRR, NOS7, v)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS7 필드를 쓴다 — TEX 클래스 7가 외부 공유인가. */


/* RESUME */
#define SET_TNR(b, c, v)	 SET_CONTEXT_FIELD(b, c, RESUME, TNR, v)	/* [한국어] 컨텍스트 뱅크 c의 RESUME에서 TNR 필드를 쓴다 — 재개할지 종료할지를 고른다. */


/* SCTLR */
#define SET_M(b, c, v)		 SET_CONTEXT_FIELD(b, c, SCTLR, M, v)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 M 필드를 쓴다 — 이 컨텍스트의 MMU 활성화. */
#define SET_TRE(b, c, v)	 SET_CONTEXT_FIELD(b, c, SCTLR, TRE, v)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 TRE 필드를 쓴다 — TEX 재매핑 활성화. */
#define SET_AFE(b, c, v)	 SET_CONTEXT_FIELD(b, c, SCTLR, AFE, v)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 AFE 필드를 쓴다 — 접근 플래그 활성화. */
#define SET_HAF(b, c, v)	 SET_CONTEXT_FIELD(b, c, SCTLR, HAF, v)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 HAF 필드를 쓴다 — 하드웨어 접근 플래그 갱신. */
#define SET_BE(b, c, v)		 SET_CONTEXT_FIELD(b, c, SCTLR, BE, v)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 BE 필드를 쓴다 — 빅엔디언 테이블. */
#define SET_AFFD(b, c, v)	 SET_CONTEXT_FIELD(b, c, SCTLR, AFFD, v)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 AFFD 필드를 쓴다 — 접근 플래그 폴트 비활성화. */


/* TLBLKCR */
#define SET_LKE(b, c, v)	   SET_CONTEXT_FIELD(b, c, TLBLKCR, LKE, v)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 LKE 필드를 쓴다 — TLB 잠금 활성화. */
/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 TLBLCKR_TLBIALLCFG 필드를 쓴다 — 전체 무효화가 잠긴 항목도 지울지. */
#define SET_TLBLKCR_TLBIALLCFG(b, c, v) \
			SET_CONTEXT_FIELD(b, c, TLBLKCR, TLBLCKR_TLBIALLCFG, v)
/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 TLBIASIDCFG 필드를 쓴다 — ASID 무효화가 잠긴 항목도 지울지. */
#define SET_TLBIASIDCFG(b, c, v) \
			SET_CONTEXT_FIELD(b, c, TLBLKCR, TLBIASIDCFG, v)	/* [한국어] ASID 무효화가 잠긴 항목까지 지울지를 설정한다. */
#define SET_TLBIVAACFG(b, c, v)	SET_CONTEXT_FIELD(b, c, TLBLKCR, TLBIVAACFG, v)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 TLBIVAACFG 필드를 쓴다 — 주소 무효화가 잠긴 항목도 지울지. */
#define SET_FLOOR(b, c, v)	SET_CONTEXT_FIELD(b, c, TLBLKCR, FLOOR, v)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 FLOOR 필드를 쓴다 — 잠긴 영역의 하한. */
#define SET_VICTIM(b, c, v)	SET_CONTEXT_FIELD(b, c, TLBLKCR, VICTIM, v)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 VICTIM 필드를 쓴다 — 다음에 교체할 항목. */


/* TTBCR */
#define SET_N(b, c, v)	         SET_CONTEXT_FIELD(b, c, TTBCR, N, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBCR에서 N 필드를 쓴다 — TTBR0과 TTBR1의 경계를 정하는 비트 수. */
#define SET_PD0(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBCR, PD0, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBCR에서 PD0 필드를 쓴다 — TTBR0 워크 비활성화. */
#define SET_PD1(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBCR, PD1, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBCR에서 PD1 필드를 쓴다 — TTBR1 워크 비활성화. */


/* TTBR0 */
#define SET_TTBR0_IRGNH(b, c, v) SET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_IRGNH, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_IRGNH 필드를 쓴다 — 내부 캐시 정책(상위 비트). */
#define SET_TTBR0_SH(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_SH, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_SH 필드를 쓴다 — 테이블 접근의 공유 속성. */
#define SET_TTBR0_ORGN(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_ORGN, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_ORGN 필드를 쓴다 — 외부 캐시 정책. */
#define SET_TTBR0_NOS(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_NOS, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_NOS 필드를 쓴다 — 외부 공유 여부. */
#define SET_TTBR0_IRGNL(b, c, v) SET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_IRGNL, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_IRGNL 필드를 쓴다 — 내부 캐시 정책(하위 비트). */
#define SET_TTBR0_PA(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_PA, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_PA 필드를 쓴다 — 1단계 테이블의 물리 주소. */


/* TTBR1 */
#define SET_TTBR1_IRGNH(b, c, v) SET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_IRGNH, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_IRGNH 필드를 쓴다 — 내부 캐시 정책(상위 비트). */
#define SET_TTBR1_SH(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_SH, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_SH 필드를 쓴다 — 테이블 접근의 공유 속성. */
#define SET_TTBR1_ORGN(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_ORGN, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_ORGN 필드를 쓴다 — 외부 캐시 정책. */
#define SET_TTBR1_NOS(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_NOS, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_NOS 필드를 쓴다 — 외부 공유 여부. */
#define SET_TTBR1_IRGNL(b, c, v) SET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_IRGNL, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_IRGNL 필드를 쓴다 — 내부 캐시 정책(하위 비트). */
#define SET_TTBR1_PA(b, c, v)	 SET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_PA, v)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_PA 필드를 쓴다 — 1단계 테이블의 물리 주소. */


/* V2PSR */
#define SET_HIT(b, c, v)	 SET_CONTEXT_FIELD(b, c, V2PSR, HIT, v)	/* [한국어] 컨텍스트 뱅크 c의 V2PSR에서 HIT 필드를 쓴다 — 변환에 성공했는가. */
#define SET_INDEX(b, c, v)	 SET_CONTEXT_FIELD(b, c, V2PSR, INDEX, v)	/* [한국어] 컨텍스트 뱅크 c의 V2PSR에서 INDEX 필드를 쓴다 — 적중한 TLB 항목의 번호. */


/* Context Register getters */
/* ACTLR */
#define GET_CFERE(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, CFERE)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 CFERE 필드를 읽는다 — 컨텍스트 폴트 시 오류 응답 활성화. */
#define GET_CFEIE(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, CFEIE)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 CFEIE 필드를 읽는다 — 컨텍스트 폴트 인터럽트 활성화. */
#define GET_PTSHCFG(b, c)       GET_CONTEXT_FIELD(b, c, ACTLR, PTSHCFG)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PTSHCFG 필드를 읽는다 — 페이지 테이블 접근의 공유 속성. */
#define GET_RCOSH(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, RCOSH)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 RCOSH 필드를 읽는다 — 외부 공유 읽기 정책. */
#define GET_RCISH(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, RCISH)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 RCISH 필드를 읽는다 — 내부 공유 읽기 정책. */
#define GET_RCNSH(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, RCNSH)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 RCNSH 필드를 읽는다 — 비공유 읽기 정책. */
#define GET_PRIVCFG(b, c)       GET_CONTEXT_FIELD(b, c, ACTLR, PRIVCFG)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PRIVCFG 필드를 읽는다 — 특권 속성 설정. */
#define GET_DNA(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, DNA)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 DNA 필드를 읽는다 — 접근 플래그 갱신 금지. */
#define GET_DNLV2PA(b, c)       GET_CONTEXT_FIELD(b, c, ACTLR, DNLV2PA)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 DNLV2PA 필드를 읽는다 — 2단계 물리 주소 갱신 금지. */
#define GET_TLBMCFG(b, c)       GET_CONTEXT_FIELD(b, c, ACTLR, TLBMCFG)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 TLBMCFG 필드를 읽는다 — TLB 다중 적중 처리 설정. */
#define GET_CFCFG(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, CFCFG)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 CFCFG 필드를 읽는다 — 폴트 시 멈춰 세울지 종료할지. */
#define GET_TIPCF(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, TIPCF)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 TIPCF 필드를 읽는다 — 변환 진행 중 폴트 설정. */
#define GET_V2PCFG(b, c)        GET_CONTEXT_FIELD(b, c, ACTLR, V2PCFG)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 V2PCFG 필드를 읽는다 — 가상→물리 요청의 동작 설정. */
#define GET_HUME(b, c)	        GET_CONTEXT_FIELD(b, c, ACTLR, HUME)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 HUME 필드를 읽는다 — 하드웨어 업데이트 확장 활성화. */
#define GET_PTMTCFG(b, c)       GET_CONTEXT_FIELD(b, c, ACTLR, PTMTCFG)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PTMTCFG 필드를 읽는다 — 테이블 접근의 메모리 타입 덮어쓰기. */
#define GET_PTMEMTYPE(b, c)     GET_CONTEXT_FIELD(b, c, ACTLR, PTMEMTYPE)	/* [한국어] 컨텍스트 뱅크 c의 ACTLR에서 PTMEMTYPE 필드를 읽는다 — 테이블 접근에 강제할 메모리 타입. */

/* BFBCR */
#define GET_BFBDFE(b, c)	GET_CONTEXT_FIELD(b, c, BFBCR, BFBDFE)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 BFBDFE 필드를 읽는다 — 분기 예측 데이터 인출 활성화. */
#define GET_BFBSFE(b, c)	GET_CONTEXT_FIELD(b, c, BFBCR, BFBSFE)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 BFBSFE 필드를 읽는다 — 분기 예측 추측 인출 활성화. */
#define GET_SFVS(b, c)		GET_CONTEXT_FIELD(b, c, BFBCR, SFVS)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 SFVS 필드를 읽는다 — 추측 인출의 유효 크기. */
#define GET_FLVIC(b, c)		GET_CONTEXT_FIELD(b, c, BFBCR, FLVIC)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 FLVIC 필드를 읽는다 — 1단계 예측 항목 수. */
#define GET_SLVIC(b, c)		GET_CONTEXT_FIELD(b, c, BFBCR, SLVIC)	/* [한국어] 컨텍스트 뱅크 c의 BFBCR에서 SLVIC 필드를 읽는다 — 2단계 예측 항목 수. */


/* CONTEXTIDR */
/* [한국어] 컨텍스트 뱅크 c의 CONTEXTIDR에서 CONTEXTIDR_ASID 필드를 읽는다 — 이 컨텍스트의 ASID — TLB 태그가 된다. */
#define GET_CONTEXTIDR_ASID(b, c) \
			GET_CONTEXT_FIELD(b, c, CONTEXTIDR, CONTEXTIDR_ASID)	/* [한국어] 이 컨텍스트의 ASID를 읽는다 — TLB 태그로 쓰이는 값이다. */
#define GET_CONTEXTIDR_PROCID(b, c) GET_CONTEXT_FIELD(b, c, CONTEXTIDR, PROCID)	/* [한국어] 컨텍스트 뱅크 c의 CONTEXTIDR에서 PROCID 필드를 읽는다 — 소프트웨어가 쓰는 프로세스 식별자. */


/* FSR */
#define GET_TF(b, c)		GET_CONTEXT_FIELD(b, c, FSR, TF)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 TF 필드를 읽는다 — 변환 폴트 — 매핑이 없다. */
#define GET_AFF(b, c)		GET_CONTEXT_FIELD(b, c, FSR, AFF)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 AFF 필드를 읽는다 — 접근 플래그 폴트. */
#define GET_APF(b, c)		GET_CONTEXT_FIELD(b, c, FSR, APF)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 APF 필드를 읽는다 — 권한 폴트. */
#define GET_TLBMF(b, c)		GET_CONTEXT_FIELD(b, c, FSR, TLBMF)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 TLBMF 필드를 읽는다 — TLB 다중 적중. */
#define GET_HTWDEEF(b, c)	GET_CONTEXT_FIELD(b, c, FSR, HTWDEEF)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 HTWDEEF 필드를 읽는다 — 테이블 워크 중 디코드 오류. */
#define GET_HTWSEEF(b, c)	GET_CONTEXT_FIELD(b, c, FSR, HTWSEEF)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 HTWSEEF 필드를 읽는다 — 테이블 워크 중 슬레이브 오류. */
#define GET_MHF(b, c)		GET_CONTEXT_FIELD(b, c, FSR, MHF)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 MHF 필드를 읽는다 — 다중 적중 폴트. */
#define GET_SL(b, c)		GET_CONTEXT_FIELD(b, c, FSR, SL)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 SL 필드를 읽는다 — 두 번째 단계에서 났는가. */
#define GET_SS(b, c)		GET_CONTEXT_FIELD(b, c, FSR, SS)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 SS 필드를 읽는다 — 트랜잭션이 멈춰 세워졌다. */
#define GET_MULTI(b, c)		GET_CONTEXT_FIELD(b, c, FSR, MULTI)	/* [한국어] 컨텍스트 뱅크 c의 FSR에서 MULTI 필드를 읽는다 — 폴트가 겹쳐서 났다. */


/* FSYNR0 */
#define GET_AMID(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR0, AMID)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 AMID 필드를 읽는다 — 폴트를 낸 마스터 ID. */
#define GET_APID(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR0, APID)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 APID 필드를 읽는다 — 폴트를 낸 페이지 ID. */
#define GET_ABID(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR0, ABID)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 ABID 필드를 읽는다 — 폴트를 낸 버스 ID. */
#define GET_ATID(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR0, ATID)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR0에서 ATID 필드를 읽는다 — 폴트를 낸 트랜잭션 ID. */


/* FSYNR1 */
#define GET_AMEMTYPE(b, c)	GET_CONTEXT_FIELD(b, c, FSYNR1, AMEMTYPE)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AMEMTYPE 필드를 읽는다 — 그 트랜잭션의 메모리 타입. */
#define GET_ASHARED(b, c)	GET_CONTEXT_FIELD(b, c, FSYNR1, ASHARED)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ASHARED 필드를 읽는다 — 공유 접근이었는가. */
#define GET_AINNERSHARED(b, c)  GET_CONTEXT_FIELD(b, c, FSYNR1, AINNERSHARED)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AINNERSHARED 필드를 읽는다 — 내부 공유 접근이었는가. */
#define GET_APRIV(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR1, APRIV)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 APRIV 필드를 읽는다 — 특권 접근이었는가. */
#define GET_APROTNS(b, c)	GET_CONTEXT_FIELD(b, c, FSYNR1, APROTNS)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 APROTNS 필드를 읽는다 — 비보안 접근이었는가. */
#define GET_AINST(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR1, AINST)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AINST 필드를 읽는다 — 명령 인출이었는가. */
#define GET_AWRITE(b, c)	GET_CONTEXT_FIELD(b, c, FSYNR1, AWRITE)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AWRITE 필드를 읽는다 — 쓰기였는가. */
#define GET_ABURST(b, c)	GET_CONTEXT_FIELD(b, c, FSYNR1, ABURST)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ABURST 필드를 읽는다 — 버스트 종류. */
#define GET_ALEN(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR1, ALEN)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ALEN 필드를 읽는다 — 버스트 길이. */
#define GET_FSYNR1_ASIZE(b, c)	GET_CONTEXT_FIELD(b, c, FSYNR1, FSYNR1_ASIZE)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 FSYNR1_ASIZE 필드를 읽는다 — 전송 크기. */
#define GET_ALOCK(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR1, ALOCK)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 ALOCK 필드를 읽는다 — 잠금 전송이었는가. */
#define GET_AFULL(b, c)		GET_CONTEXT_FIELD(b, c, FSYNR1, AFULL)	/* [한국어] 컨텍스트 뱅크 c의 FSYNR1에서 AFULL 필드를 읽는다 — 버퍼가 가득 찼는가. */


/* NMRR */
#define GET_ICPC0(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC0)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC0 필드를 읽는다 — TEX 클래스 0의 내부 캐시 정책. */
#define GET_ICPC1(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC1)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC1 필드를 읽는다 — TEX 클래스 1의 내부 캐시 정책. */
#define GET_ICPC2(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC2)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC2 필드를 읽는다 — TEX 클래스 2의 내부 캐시 정책. */
#define GET_ICPC3(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC3)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC3 필드를 읽는다 — TEX 클래스 3의 내부 캐시 정책. */
#define GET_ICPC4(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC4)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC4 필드를 읽는다 — TEX 클래스 4의 내부 캐시 정책. */
#define GET_ICPC5(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC5)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC5 필드를 읽는다 — TEX 클래스 5의 내부 캐시 정책. */
#define GET_ICPC6(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC6)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC6 필드를 읽는다 — TEX 클래스 6의 내부 캐시 정책. */
#define GET_ICPC7(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, ICPC7)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 ICPC7 필드를 읽는다 — TEX 클래스 7의 내부 캐시 정책. */
#define GET_OCPC0(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC0)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC0 필드를 읽는다 — TEX 클래스 0의 외부 캐시 정책. */
#define GET_OCPC1(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC1)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC1 필드를 읽는다 — TEX 클래스 1의 외부 캐시 정책. */
#define GET_OCPC2(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC2)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC2 필드를 읽는다 — TEX 클래스 2의 외부 캐시 정책. */
#define GET_OCPC3(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC3)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC3 필드를 읽는다 — TEX 클래스 3의 외부 캐시 정책. */
#define GET_OCPC4(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC4)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC4 필드를 읽는다 — TEX 클래스 4의 외부 캐시 정책. */
#define GET_OCPC5(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC5)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC5 필드를 읽는다 — TEX 클래스 5의 외부 캐시 정책. */
#define GET_OCPC6(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC6)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC6 필드를 읽는다 — TEX 클래스 6의 외부 캐시 정책. */
#define GET_OCPC7(b, c)		GET_CONTEXT_FIELD(b, c, NMRR, OCPC7)	/* [한국어] 컨텍스트 뱅크 c의 NMRR에서 OCPC7 필드를 읽는다 — TEX 클래스 7의 외부 캐시 정책. */
#define NMRR_ICP(nmrr, n)	(((nmrr) & (3 << ((n) * 2))) >> ((n) * 2))	/* [한국어] NMRR 값에서 n번 TEX 클래스의 내부 캐시 정책 2비트를 뽑는다. */
/* [한국어] NMRR 값에서 n번 TEX 클래스의 외부 캐시 정책 2비트를 뽑는다. 외부는 비트 16부터 시작한다. */
#define NMRR_OCP(nmrr, n)	(((nmrr) & (3 << ((n) * 2 + 16))) >> \
								((n) * 2 + 16))	/* [한국어] 외부 캐시 정책은 비트 16부터 시작하므로 그만큼 더 민다. */

/* PAR */
#define GET_FAULT(b, c)		GET_CONTEXT_FIELD(b, c, PAR, FAULT)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT 필드를 읽는다 — 변환이 실패했는가 — 이 비트로 아래 두 해석이 갈린다. */

#define GET_FAULT_TF(b, c)	GET_CONTEXT_FIELD(b, c, PAR, FAULT_TF)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_TF 필드를 읽는다 — 변환 폴트였다. */
#define GET_FAULT_AFF(b, c)	GET_CONTEXT_FIELD(b, c, PAR, FAULT_AFF)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_AFF 필드를 읽는다 — 접근 플래그 폴트였다. */
#define GET_FAULT_APF(b, c)	GET_CONTEXT_FIELD(b, c, PAR, FAULT_APF)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_APF 필드를 읽는다 — 권한 폴트였다. */
#define GET_FAULT_TLBMF(b, c)   GET_CONTEXT_FIELD(b, c, PAR, FAULT_TLBMF)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_TLBMF 필드를 읽는다 — TLB 다중 적중이었다. */
#define GET_FAULT_HTWDEEF(b, c) GET_CONTEXT_FIELD(b, c, PAR, FAULT_HTWDEEF)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_HTWDEEF 필드를 읽는다 — 워크 중 디코드 오류였다. */
#define GET_FAULT_HTWSEEF(b, c) GET_CONTEXT_FIELD(b, c, PAR, FAULT_HTWSEEF)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_HTWSEEF 필드를 읽는다 — 워크 중 슬레이브 오류였다. */
#define GET_FAULT_MHF(b, c)	GET_CONTEXT_FIELD(b, c, PAR, FAULT_MHF)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_MHF 필드를 읽는다 — 다중 적중 폴트였다. */
#define GET_FAULT_SL(b, c)	GET_CONTEXT_FIELD(b, c, PAR, FAULT_SL)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_SL 필드를 읽는다 — 두 번째 단계에서 났다. */
#define GET_FAULT_SS(b, c)	GET_CONTEXT_FIELD(b, c, PAR, FAULT_SS)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 FAULT_SS 필드를 읽는다 — 멈춰 세워졌다. */

#define GET_NOFAULT_SS(b, c)	GET_CONTEXT_FIELD(b, c, PAR, PAR_NOFAULT_SS)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 PAR_NOFAULT_SS 필드를 읽는다 — 성공 시 — 멈춤 상태. */
#define GET_NOFAULT_MT(b, c)	GET_CONTEXT_FIELD(b, c, PAR, PAR_NOFAULT_MT)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 PAR_NOFAULT_MT 필드를 읽는다 — 성공 시 — 메모리 타입. */
#define GET_NOFAULT_SH(b, c)	GET_CONTEXT_FIELD(b, c, PAR, PAR_NOFAULT_SH)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 PAR_NOFAULT_SH 필드를 읽는다 — 성공 시 — 공유 속성. */
#define GET_NOFAULT_NS(b, c)	GET_CONTEXT_FIELD(b, c, PAR, PAR_NOFAULT_NS)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 PAR_NOFAULT_NS 필드를 읽는다 — 성공 시 — 비보안 여부. */
#define GET_NOFAULT_NOS(b, c)   GET_CONTEXT_FIELD(b, c, PAR, PAR_NOFAULT_NOS)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 PAR_NOFAULT_NOS 필드를 읽는다 — 성공 시 — 외부 공유 여부. */
#define GET_NPFAULT_PA(b, c)	GET_CONTEXT_FIELD(b, c, PAR, PAR_NPFAULT_PA)	/* [한국어] 컨텍스트 뱅크 c의 PAR에서 PAR_NPFAULT_PA 필드를 읽는다 — 성공 시 — 변환된 물리 주소. */


/* PRRR */
#define GET_MTC0(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC0)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC0 필드를 읽는다 — TEX 클래스 0의 메모리 타입. */
#define GET_MTC1(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC1)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC1 필드를 읽는다 — TEX 클래스 1의 메모리 타입. */
#define GET_MTC2(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC2)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC2 필드를 읽는다 — TEX 클래스 2의 메모리 타입. */
#define GET_MTC3(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC3)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC3 필드를 읽는다 — TEX 클래스 3의 메모리 타입. */
#define GET_MTC4(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC4)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC4 필드를 읽는다 — TEX 클래스 4의 메모리 타입. */
#define GET_MTC5(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC5)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC5 필드를 읽는다 — TEX 클래스 5의 메모리 타입. */
#define GET_MTC6(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC6)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC6 필드를 읽는다 — TEX 클래스 6의 메모리 타입. */
#define GET_MTC7(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, MTC7)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 MTC7 필드를 읽는다 — TEX 클래스 7의 메모리 타입. */
#define GET_SHDSH0(b, c)	GET_CONTEXT_FIELD(b, c, PRRR, SHDSH0)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHDSH0 필드를 읽는다 — 장치 메모리 0의 공유 속성. */
#define GET_SHDSH1(b, c)	GET_CONTEXT_FIELD(b, c, PRRR, SHDSH1)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHDSH1 필드를 읽는다 — 장치 메모리 1의 공유 속성. */
#define GET_SHNMSH0(b, c)	GET_CONTEXT_FIELD(b, c, PRRR, SHNMSH0)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHNMSH0 필드를 읽는다 — 일반 메모리 0의 공유 속성. */
#define GET_SHNMSH1(b, c)	GET_CONTEXT_FIELD(b, c, PRRR, SHNMSH1)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 SHNMSH1 필드를 읽는다 — 일반 메모리 1의 공유 속성. */
#define GET_NOS0(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS0)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS0 필드를 읽는다 — TEX 클래스 0가 외부 공유인가. */
#define GET_NOS1(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS1)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS1 필드를 읽는다 — TEX 클래스 1가 외부 공유인가. */
#define GET_NOS2(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS2)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS2 필드를 읽는다 — TEX 클래스 2가 외부 공유인가. */
#define GET_NOS3(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS3)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS3 필드를 읽는다 — TEX 클래스 3가 외부 공유인가. */
#define GET_NOS4(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS4)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS4 필드를 읽는다 — TEX 클래스 4가 외부 공유인가. */
#define GET_NOS5(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS5)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS5 필드를 읽는다 — TEX 클래스 5가 외부 공유인가. */
#define GET_NOS6(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS6)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS6 필드를 읽는다 — TEX 클래스 6가 외부 공유인가. */
#define GET_NOS7(b, c)		GET_CONTEXT_FIELD(b, c, PRRR, NOS7)	/* [한국어] 컨텍스트 뱅크 c의 PRRR에서 NOS7 필드를 읽는다 — TEX 클래스 7가 외부 공유인가. */
#define PRRR_NOS(prrr, n)	 ((prrr) & (1 << ((n) + 24)) ? 1 : 0)	/* [한국어] PRRR 값에서 n번 TEX 클래스가 외부 공유인지 뽑는다(비트 24부터). */
#define PRRR_MT(prrr, n)	 ((((prrr) & (3 << ((n) * 2))) >> ((n) * 2)))	/* [한국어] PRRR 값에서 n번 TEX 클래스의 메모리 타입 2비트를 뽑는다. */


/* RESUME */
#define GET_TNR(b, c)		GET_CONTEXT_FIELD(b, c, RESUME, TNR)	/* [한국어] 컨텍스트 뱅크 c의 RESUME에서 TNR 필드를 읽는다 — 재개할지 종료할지를 고른다. */


/* SCTLR */
#define GET_M(b, c)		GET_CONTEXT_FIELD(b, c, SCTLR, M)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 M 필드를 읽는다 — 이 컨텍스트의 MMU 활성화. */
#define GET_TRE(b, c)		GET_CONTEXT_FIELD(b, c, SCTLR, TRE)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 TRE 필드를 읽는다 — TEX 재매핑 활성화. */
#define GET_AFE(b, c)		GET_CONTEXT_FIELD(b, c, SCTLR, AFE)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 AFE 필드를 읽는다 — 접근 플래그 활성화. */
#define GET_HAF(b, c)		GET_CONTEXT_FIELD(b, c, SCTLR, HAF)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 HAF 필드를 읽는다 — 하드웨어 접근 플래그 갱신. */
#define GET_BE(b, c)		GET_CONTEXT_FIELD(b, c, SCTLR, BE)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 BE 필드를 읽는다 — 빅엔디언 테이블. */
#define GET_AFFD(b, c)		GET_CONTEXT_FIELD(b, c, SCTLR, AFFD)	/* [한국어] 컨텍스트 뱅크 c의 SCTLR에서 AFFD 필드를 읽는다 — 접근 플래그 폴트 비활성화. */


/* TLBLKCR */
#define GET_LKE(b, c)		GET_CONTEXT_FIELD(b, c, TLBLKCR, LKE)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 LKE 필드를 읽는다 — TLB 잠금 활성화. */
/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 TLBLCKR_TLBIALLCFG 필드를 읽는다 — 전체 무효화가 잠긴 항목도 지울지. */
#define GET_TLBLCKR_TLBIALLCFG(b, c) \
			GET_CONTEXT_FIELD(b, c, TLBLKCR, TLBLCKR_TLBIALLCFG)	/* [한국어] 전체 무효화가 잠긴 항목까지 지울지를 읽는다. */
#define GET_TLBIASIDCFG(b, c)   GET_CONTEXT_FIELD(b, c, TLBLKCR, TLBIASIDCFG)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 TLBIASIDCFG 필드를 읽는다 — ASID 무효화가 잠긴 항목도 지울지. */
#define GET_TLBIVAACFG(b, c)	GET_CONTEXT_FIELD(b, c, TLBLKCR, TLBIVAACFG)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 TLBIVAACFG 필드를 읽는다 — 주소 무효화가 잠긴 항목도 지울지. */
#define GET_FLOOR(b, c)		GET_CONTEXT_FIELD(b, c, TLBLKCR, FLOOR)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 FLOOR 필드를 읽는다 — 잠긴 영역의 하한. */
#define GET_VICTIM(b, c)	GET_CONTEXT_FIELD(b, c, TLBLKCR, VICTIM)	/* [한국어] 컨텍스트 뱅크 c의 TLBLKCR에서 VICTIM 필드를 읽는다 — 다음에 교체할 항목. */


/* TTBCR */
#define GET_N(b, c)		GET_CONTEXT_FIELD(b, c, TTBCR, N)	/* [한국어] 컨텍스트 뱅크 c의 TTBCR에서 N 필드를 읽는다 — TTBR0과 TTBR1의 경계를 정하는 비트 수. */
#define GET_PD0(b, c)		GET_CONTEXT_FIELD(b, c, TTBCR, PD0)	/* [한국어] 컨텍스트 뱅크 c의 TTBCR에서 PD0 필드를 읽는다 — TTBR0 워크 비활성화. */
#define GET_PD1(b, c)		GET_CONTEXT_FIELD(b, c, TTBCR, PD1)	/* [한국어] 컨텍스트 뱅크 c의 TTBCR에서 PD1 필드를 읽는다 — TTBR1 워크 비활성화. */


/* TTBR0 */
#define GET_TTBR0_IRGNH(b, c)	GET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_IRGNH)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_IRGNH 필드를 읽는다 — 내부 캐시 정책(상위 비트). */
#define GET_TTBR0_SH(b, c)	GET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_SH)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_SH 필드를 읽는다 — 테이블 접근의 공유 속성. */
#define GET_TTBR0_ORGN(b, c)	GET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_ORGN)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_ORGN 필드를 읽는다 — 외부 캐시 정책. */
#define GET_TTBR0_NOS(b, c)	GET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_NOS)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_NOS 필드를 읽는다 — 외부 공유 여부. */
#define GET_TTBR0_IRGNL(b, c)	GET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_IRGNL)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_IRGNL 필드를 읽는다 — 내부 캐시 정책(하위 비트). */
#define GET_TTBR0_PA(b, c)	GET_CONTEXT_FIELD(b, c, TTBR0, TTBR0_PA)	/* [한국어] 컨텍스트 뱅크 c의 TTBR0에서 TTBR0_PA 필드를 읽는다 — 1단계 테이블의 물리 주소. */


/* TTBR1 */
#define GET_TTBR1_IRGNH(b, c)	GET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_IRGNH)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_IRGNH 필드를 읽는다 — 내부 캐시 정책(상위 비트). */
#define GET_TTBR1_SH(b, c)	GET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_SH)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_SH 필드를 읽는다 — 테이블 접근의 공유 속성. */
#define GET_TTBR1_ORGN(b, c)	GET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_ORGN)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_ORGN 필드를 읽는다 — 외부 캐시 정책. */
#define GET_TTBR1_NOS(b, c)	GET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_NOS)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_NOS 필드를 읽는다 — 외부 공유 여부. */
#define GET_TTBR1_IRGNL(b, c)	GET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_IRGNL)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_IRGNL 필드를 읽는다 — 내부 캐시 정책(하위 비트). */
#define GET_TTBR1_PA(b, c)	GET_CONTEXT_FIELD(b, c, TTBR1, TTBR1_PA)	/* [한국어] 컨텍스트 뱅크 c의 TTBR1에서 TTBR1_PA 필드를 읽는다 — 1단계 테이블의 물리 주소. */


/* V2PSR */
#define GET_HIT(b, c)		GET_CONTEXT_FIELD(b, c, V2PSR, HIT)	/* [한국어] 컨텍스트 뱅크 c의 V2PSR에서 HIT 필드를 읽는다 — 변환에 성공했는가. */
#define GET_INDEX(b, c)		GET_CONTEXT_FIELD(b, c, V2PSR, INDEX)	/* [한국어] 컨텍스트 뱅크 c의 V2PSR에서 INDEX 필드를 읽는다 — 적중한 TLB 항목의 번호. */


/* Global Registers */
/* [한국어] 전역 레지스터의 오프셋 표.
 * 값들이 0xFF000 이상에 몰려 있는 것은, 그 아래 공간을 컨텍스트
 * 뱅크의 창들이 차지하기 때문이다 — 뱅크 하나가 4KB이므로
 * 0xFF000까지면 뱅크 255개 분량이 된다. */
#define M2VCBR_N	(0xFF000)	/* [한국어] 마스터 ID를 컨텍스트 뱅크에 대응시키는 표(스트림당 한 워드) (오프셋 (0xFF000)). */
#define CBACR_N		(0xFF800)	/* [한국어] 컨텍스트 뱅크의 속성 — VMID와 인터럽트 번호를 정한다 (오프셋 (0xFF800)). */
#define TLBRSW		(0xFFE00)	/* [한국어] TLB 읽기/쓰기 선택 — 디버그용으로 TLB 항목 하나를 지목한다 (오프셋 (0xFFE00)). */
#define TLBTR0		(0xFFE80)	/* [한국어] 지목된 TLB 항목의 속성(권한·공유·메모리 타입) (오프셋 (0xFFE80)). */
#define TLBTR1		(0xFFE84)	/* [한국어] 지목된 TLB 항목의 VMID와 물리 주소 (오프셋 (0xFFE84)). */
#define TLBTR2		(0xFFE88)	/* [한국어] 지목된 TLB 항목의 ASID·유효 비트·가상 주소 (오프셋 (0xFFE88)). */
#define TESTBUSCR	(0xFFE8C)	/* [한국어] 테스트 버스 제어 — 하드웨어 디버그 전용 (오프셋 (0xFFE8C)). */
#define GLOBAL_TLBIALL	(0xFFF00)	/* [한국어] 모든 컨텍스트의 TLB를 통째로 무효화한다 (오프셋 (0xFFF00)). */
#define TLBIVMID	(0xFFF04)	/* [한국어] 지정한 VMID의 TLB 항목만 무효화한다 (오프셋 (0xFFF04)). */
#define CR		(0xFFF80)	/* [한국어] IOMMU 전역 제어 — 클라이언트 통과, stall, TLB 잠금 허용 등 (오프셋 (0xFFF80)). */
#define EAR		(0xFFF84)	/* [한국어] 전역 오류가 난 주소 (오프셋 (0xFFF84)). */
#define ESR		(0xFFF88)	/* [한국어] 전역 오류 상태 — 설정 오류, 통과 중 접근 등 (오프셋 (0xFFF88)). */
#define ESRRESTORE	(0xFFF8C)	/* [한국어] 전역 오류 상태를 복원용으로 되쓰는 창 (오프셋 (0xFFF8C)). */
#define ESYNR0		(0xFFF90)	/* [한국어] 전역 오류를 낸 트랜잭션의 식별자들(마스터·페이지·VMID) (오프셋 (0xFFF90)). */
#define ESYNR1		(0xFFF94)	/* [한국어] 전역 오류를 낸 트랜잭션의 속성들(크기·버스트·권한) (오프셋 (0xFFF94)). */
#define REV		(0xFFFF4)	/* [한국어] 하드웨어 리비전 (오프셋 (0xFFFF4)). */
#define IDR		(0xFFFF8)	/* [한국어] 하드웨어 능력 — 컨텍스트 뱅크 수, TLB 크기, 하드웨어 워크 지원 여부 (오프셋 (0xFFFF8)). */
#define RPU_ACR		(0xFFFFC)	/* [한국어] RPU(원격 처리 유닛) 보조 제어 (오프셋 (0xFFFFC)). */


/* Context Bank Registers */
/* [한국어] 컨텍스트 뱅크 창 안에서의 오프셋 표.
 * 0x000~0x04C가 설정과 상태, 0x800부터가 무효화와 주소 변환
 * 요청 명령이다. 이 오프셋들은 뱅크마다 반복되며, 뱅크 번호를
 * 12비트 밀어 더한 자리가 실제 주소가 된다. */
#define SCTLR		(0x000)	/* [한국어] 이 컨텍스트의 시스템 제어 — MMU 활성화, TEX 재매핑, 접근 플래그 (오프셋 (0x000)). */
#define ACTLR		(0x004)	/* [한국어] 이 컨텍스트의 보조 제어 — 폴트 처리 방식과 테이블 워크 속성 (오프셋 (0x004)). */
#define CONTEXTIDR	(0x008)	/* [한국어] 이 컨텍스트의 ASID와 프로세스 ID (오프셋 (0x008)). */
#define TTBR0		(0x010)	/* [한국어] 이 컨텍스트의 1단계 테이블 기준 주소 0번 (오프셋 (0x010)). */
#define TTBR1		(0x014)	/* [한국어] 이 컨텍스트의 1단계 테이블 기준 주소 1번 (오프셋 (0x014)). */
#define TTBCR		(0x018)	/* [한국어] 두 TTBR의 경계와 워크 비활성화를 정한다 (오프셋 (0x018)). */
#define PAR		(0x01C)	/* [한국어] 주소 변환 결과 — 성공하면 물리 주소, 실패하면 폴트 원인 (오프셋 (0x01C)). */
#define FSR		(0x020)	/* [한국어] 이 컨텍스트의 폴트 상태 — 어떤 종류의 폴트가 났는가 (오프셋 (0x020)). */
#define FSRRESTORE	(0x024)	/* [한국어] 폴트 상태를 복원용으로 되쓰는 창 (오프셋 (0x024)). */
#define FAR		(0x028)	/* [한국어] 폴트가 난 가상 주소 (오프셋 (0x028)). */
#define FSYNR0		(0x02C)	/* [한국어] 폴트를 낸 트랜잭션의 식별자들 (오프셋 (0x02C)). */
#define FSYNR1		(0x030)	/* [한국어] 폴트를 낸 트랜잭션의 속성들 (오프셋 (0x030)). */
#define PRRR		(0x034)	/* [한국어] 1차 영역 재매핑 — TEX 클래스를 메모리 타입으로 옮긴다 (오프셋 (0x034)). */
#define NMRR		(0x038)	/* [한국어] 일반 메모리 재매핑 — TEX 클래스별 캐시 정책 (오프셋 (0x038)). */
#define TLBLCKR		(0x03C)	/* [한국어] TLB 잠금 제어 — 특정 항목을 교체되지 않게 고정한다 (오프셋 (0x03C)). */
#define V2PSR		(0x040)	/* [한국어] 가상→물리 변환 요청의 결과 상태 (오프셋 (0x040)). */
#define TLBFLPTER	(0x044)	/* [한국어] TLB가 캐시한 1단계 엔트리(디버그용) (오프셋 (0x044)). */
#define TLBSLPTER	(0x048)	/* [한국어] TLB가 캐시한 2단계 엔트리(디버그용) (오프셋 (0x048)). */
#define BFBCR		(0x04C)	/* [한국어] 분기 예측 버퍼 제어 — 테이블 워크 예측을 조절한다 (오프셋 (0x04C)). */
#define CTX_TLBIALL	(0x800)	/* [한국어] 이 컨텍스트의 TLB를 통째로 무효화한다 (오프셋 (0x800)). */
#define TLBIASID	(0x804)	/* [한국어] 지정한 ASID의 TLB 항목만 무효화한다 (오프셋 (0x804)). */
#define TLBIVA		(0x808)	/* [한국어] 지정한 ASID의 특정 가상 주소만 무효화한다 (오프셋 (0x808)). */
#define TLBIVAA		(0x80C)	/* [한국어] ASID를 가리지 않고 특정 가상 주소를 무효화한다 (오프셋 (0x80C)). */
#define V2PPR		(0x810)	/* [한국어] 특권 읽기 관점의 가상→물리 변환을 요청한다 (오프셋 (0x810)). */
#define V2PPW		(0x814)	/* [한국어] 특권 쓰기 관점의 가상→물리 변환을 요청한다 (오프셋 (0x814)). */
#define V2PUR		(0x818)	/* [한국어] 사용자 읽기 관점의 가상→물리 변환을 요청한다 (오프셋 (0x818)). */
#define V2PUW		(0x81C)	/* [한국어] 사용자 쓰기 관점의 가상→물리 변환을 요청한다 (오프셋 (0x81C)). */
#define RESUME		(0x820)	/* [한국어] 멈춰 세운 트랜잭션을 재개시키거나 종료시킨다 (오프셋 (0x820)). */


/* Global Register Fields */
/* [한국어] 여기서부터 끝까지는 필드의 비트 자리 정의다.
 * 필드마다 세 줄이 짝을 이룬다: 마스크를 제자리로 민 값(이 절),
 * 폭을 나타내는 마스크(_MASK), 시작 비트(_SHIFT).
 * 앞의 필드 접근 매크로들이 이름 이어 붙이기로 뒤의 둘을 찾아
 * 쓰므로, 세 이름이 정확히 대응해야 한다. */
/* CBACRn */
#define RWVMID        (RWVMID_MASK       << RWVMID_SHIFT)	/* [한국어] RWVMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 라운드로빈 중재에 쓸 VMID. */
#define RWE           (RWE_MASK          << RWE_SHIFT)	/* [한국어] RWE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 라운드로빈 중재 활성화. */
#define RWGE          (RWGE_MASK         << RWGE_SHIFT)	/* [한국어] RWGE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 라운드로빈 전역 활성화. */
#define CBVMID        (CBVMID_MASK       << CBVMID_SHIFT)	/* [한국어] CBVMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 컨텍스트 뱅크가 쓸 VMID. */
#define IRPTNDX       (IRPTNDX_MASK      << IRPTNDX_SHIFT)	/* [한국어] IRPTNDX 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 컨텍스트가 쓸 인터럽트 번호. */


/* CR */
#define RPUE          (RPUE_MASK          << RPUE_SHIFT)	/* [한국어] RPUE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — RPU 활성화. */
#define RPUERE        (RPUERE_MASK        << RPUERE_SHIFT)	/* [한국어] RPUERE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — RPU 오류 응답 활성화. */
#define RPUEIE        (RPUEIE_MASK        << RPUEIE_SHIFT)	/* [한국어] RPUEIE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — RPU 오류 인터럽트 활성화. */
#define DCDEE         (DCDEE_MASK         << DCDEE_SHIFT)	/* [한국어] DCDEE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 디코드 오류 인터럽트 활성화. */
#define CLIENTPD      (CLIENTPD_MASK      << CLIENTPD_SHIFT)	/* [한국어] CLIENTPD 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 클라이언트를 통과 모드로 둘지 여부. */
#define STALLD        (STALLD_MASK        << STALLD_SHIFT)	/* [한국어] STALLD 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 폴트 시 트랜잭션을 멈춰 세울지 여부. */
#define TLBLKCRWE     (TLBLKCRWE_MASK     << TLBLKCRWE_SHIFT)	/* [한국어] TLBLKCRWE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TLB 잠금 레지스터 쓰기 허용. */
#define CR_TLBIALLCFG (CR_TLBIALLCFG_MASK << CR_TLBIALLCFG_SHIFT)	/* [한국어] CR_TLBIALLCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 전역 TLB 무효화의 범위 설정. */
#define TLBIVMIDCFG   (TLBIVMIDCFG_MASK   << TLBIVMIDCFG_SHIFT)	/* [한국어] TLBIVMIDCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — VMID 무효화의 범위 설정. */
#define CR_HUME       (CR_HUME_MASK       << CR_HUME_SHIFT)	/* [한국어] CR_HUME 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 하드웨어 업데이트 확장 활성화. */


/* ESR */
#define CFG           (CFG_MASK          << CFG_SHIFT)	/* [한국어] CFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 설정 오류가 났다. */
#define BYPASS        (BYPASS_MASK       << BYPASS_SHIFT)	/* [한국어] BYPASS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 모드에서 오류가 났다. */
#define ESR_MULTI     (ESR_MULTI_MASK    << ESR_MULTI_SHIFT)	/* [한국어] ESR_MULTI 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 오류가 여러 번 겹쳤다. */


/* ESYNR0 */
#define ESYNR0_AMID   (ESYNR0_AMID_MASK  << ESYNR0_AMID_SHIFT)	/* [한국어] ESYNR0_AMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 오류를 낸 마스터 ID. */
#define ESYNR0_APID   (ESYNR0_APID_MASK  << ESYNR0_APID_SHIFT)	/* [한국어] ESYNR0_APID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 오류를 낸 페이지 ID. */
#define ESYNR0_ABID   (ESYNR0_ABID_MASK  << ESYNR0_ABID_SHIFT)	/* [한국어] ESYNR0_ABID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 오류를 낸 버스 ID. */
#define ESYNR0_AVMID  (ESYNR0_AVMID_MASK << ESYNR0_AVMID_SHIFT)	/* [한국어] ESYNR0_AVMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 오류를 낸 VMID. */
#define ESYNR0_ATID   (ESYNR0_ATID_MASK  << ESYNR0_ATID_SHIFT)	/* [한국어] ESYNR0_ATID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 오류를 낸 트랜잭션 ID. */


/* ESYNR1 */
#define ESYNR1_AMEMTYPE      (ESYNR1_AMEMTYPE_MASK    << ESYNR1_AMEMTYPE_SHIFT)	/* [한국어] ESYNR1_AMEMTYPE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 그 트랜잭션의 메모리 타입. */
#define ESYNR1_ASHARED       (ESYNR1_ASHARED_MASK     << ESYNR1_ASHARED_SHIFT)	/* [한국어] ESYNR1_ASHARED 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 공유 접근이었는가. */
/* [한국어] ESYNR1_AINNERSHARED 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 공유 접근이었는가. */
#define ESYNR1_AINNERSHARED  (ESYNR1_AINNERSHARED_MASK<< \
						ESYNR1_AINNERSHARED_SHIFT)	/* [한국어] 긴 이름 때문에 시프트 인자가 다음 줄로 넘어갔다. */
#define ESYNR1_APRIV         (ESYNR1_APRIV_MASK       << ESYNR1_APRIV_SHIFT)	/* [한국어] ESYNR1_APRIV 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 특권 접근이었는가. */
#define ESYNR1_APROTNS       (ESYNR1_APROTNS_MASK     << ESYNR1_APROTNS_SHIFT)	/* [한국어] ESYNR1_APROTNS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 비보안 접근이었는가. */
#define ESYNR1_AINST         (ESYNR1_AINST_MASK       << ESYNR1_AINST_SHIFT)	/* [한국어] ESYNR1_AINST 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 명령 인출이었는가. */
#define ESYNR1_AWRITE        (ESYNR1_AWRITE_MASK      << ESYNR1_AWRITE_SHIFT)	/* [한국어] ESYNR1_AWRITE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 쓰기였는가. */
#define ESYNR1_ABURST        (ESYNR1_ABURST_MASK      << ESYNR1_ABURST_SHIFT)	/* [한국어] ESYNR1_ABURST 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 버스트 종류. */
#define ESYNR1_ALEN          (ESYNR1_ALEN_MASK        << ESYNR1_ALEN_SHIFT)	/* [한국어] ESYNR1_ALEN 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 버스트 길이. */
#define ESYNR1_ASIZE         (ESYNR1_ASIZE_MASK       << ESYNR1_ASIZE_SHIFT)	/* [한국어] ESYNR1_ASIZE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 전송 크기. */
#define ESYNR1_ALOCK         (ESYNR1_ALOCK_MASK       << ESYNR1_ALOCK_SHIFT)	/* [한국어] ESYNR1_ALOCK 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 잠금 전송이었는가. */
#define ESYNR1_AOOO          (ESYNR1_AOOO_MASK        << ESYNR1_AOOO_SHIFT)	/* [한국어] ESYNR1_AOOO 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 순서 없는 전송이었는가. */
#define ESYNR1_AFULL         (ESYNR1_AFULL_MASK       << ESYNR1_AFULL_SHIFT)	/* [한국어] ESYNR1_AFULL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 버퍼가 가득 찼는가. */
#define ESYNR1_AC            (ESYNR1_AC_MASK          << ESYNR1_AC_SHIFT)	/* [한국어] ESYNR1_AC 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 캐시 가능 접근이었는가. */
#define ESYNR1_DCD           (ESYNR1_DCD_MASK         << ESYNR1_DCD_SHIFT)	/* [한국어] ESYNR1_DCD 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 디코드 오류였는가. */


/* IDR */
#define NM2VCBMT      (NM2VCBMT_MASK     << NM2VCBMT_SHIFT)	/* [한국어] NM2VCBMT 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 스트림 대응표의 항목 수. */
#define HTW           (HTW_MASK          << HTW_SHIFT)	/* [한국어] HTW 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 하드웨어 테이블 워크 지원 여부. */
#define HUM           (HUM_MASK          << HUM_SHIFT)	/* [한국어] HUM 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 하드웨어 업데이트 확장 지원 여부. */
#define TLBSIZE       (TLBSIZE_MASK      << TLBSIZE_SHIFT)	/* [한국어] TLBSIZE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TLB 항목 수. */
#define NCB           (NCB_MASK          << NCB_SHIFT)	/* [한국어] NCB 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 컨텍스트 뱅크의 개수. */
#define NIRPT         (NIRPT_MASK        << NIRPT_SHIFT)	/* [한국어] NIRPT 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 인터럽트 선의 개수. */


/* M2VCBRn */
#define VMID          (VMID_MASK         << VMID_SHIFT)	/* [한국어] VMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 스트림에 부여할 VMID. */
#define CBNDX         (CBNDX_MASK        << CBNDX_SHIFT)	/* [한국어] CBNDX 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 스트림이 쓸 컨텍스트 뱅크 번호. */
#define BYPASSD       (BYPASSD_MASK      << BYPASSD_SHIFT)	/* [한국어] BYPASSD 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 모드에서의 디버그 표시. */
#define BPRCOSH       (BPRCOSH_MASK      << BPRCOSH_SHIFT)	/* [한국어] BPRCOSH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 시 외부 공유 캐시 정책. */
#define BPRCISH       (BPRCISH_MASK      << BPRCISH_SHIFT)	/* [한국어] BPRCISH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 시 내부 공유 캐시 정책. */
#define BPRCNSH       (BPRCNSH_MASK      << BPRCNSH_SHIFT)	/* [한국어] BPRCNSH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 시 비공유 캐시 정책. */
#define BPSHCFG       (BPSHCFG_MASK      << BPSHCFG_SHIFT)	/* [한국어] BPSHCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 시 공유 속성 설정. */
#define NSCFG         (NSCFG_MASK        << NSCFG_SHIFT)	/* [한국어] NSCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 보안/비보안 속성 설정. */
#define BPMTCFG       (BPMTCFG_MASK      << BPMTCFG_SHIFT)	/* [한국어] BPMTCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 시 메모리 타입을 덮어쓸지 여부. */
#define BPMEMTYPE     (BPMEMTYPE_MASK    << BPMEMTYPE_SHIFT)	/* [한국어] BPMEMTYPE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 통과 시 강제할 메모리 타입. */


/* REV */
#define IDR_MINOR     (MINOR_MASK        << MINOR_SHIFT)	/* [한국어] 리비전의 부 번호가 차지하는 비트 자리. 마스크 이름이 MINOR로 짧은 점에 유의. */
#define IDR_MAJOR     (MAJOR_MASK        << MAJOR_SHIFT)	/* [한국어] 리비전의 주 번호가 차지하는 비트 자리. 마스크 이름이 MAJOR로 짧은 점에 유의. */


/* TESTBUSCR */
#define TBE           (TBE_MASK          << TBE_SHIFT)	/* [한국어] TBE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테스트 버스 활성화. */
#define SPDMBE        (SPDMBE_MASK       << SPDMBE_SHIFT)	/* [한국어] SPDMBE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — SPDM 버스 활성화. */
#define WGSEL         (WGSEL_MASK        << WGSEL_SHIFT)	/* [한국어] WGSEL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 파형 그룹 선택. */
#define TBLSEL        (TBLSEL_MASK       << TBLSEL_SHIFT)	/* [한국어] TBLSEL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테스트 버스 하위 선택. */
#define TBHSEL        (TBHSEL_MASK       << TBHSEL_SHIFT)	/* [한국어] TBHSEL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테스트 버스 상위 선택. */
#define SPDM0SEL      (SPDM0SEL_MASK     << SPDM0SEL_SHIFT)	/* [한국어] SPDM0SEL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — SPDM 0번 선택. */
#define SPDM1SEL      (SPDM1SEL_MASK     << SPDM1SEL_SHIFT)	/* [한국어] SPDM1SEL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — SPDM 1번 선택. */
#define SPDM2SEL      (SPDM2SEL_MASK     << SPDM2SEL_SHIFT)	/* [한국어] SPDM2SEL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — SPDM 2번 선택. */
#define SPDM3SEL      (SPDM3SEL_MASK     << SPDM3SEL_SHIFT)	/* [한국어] SPDM3SEL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — SPDM 3번 선택. */


/* TLBIVMID */
#define TLBIVMID_VMID (TLBIVMID_VMID_MASK << TLBIVMID_VMID_SHIFT)	/* [한국어] TLBIVMID_VMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 무효화할 VMID. */


/* TLBRSW */
#define TLBRSW_INDEX  (TLBRSW_INDEX_MASK << TLBRSW_INDEX_SHIFT)	/* [한국어] TLBRSW_INDEX 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 읽거나 쓸 TLB 항목의 번호. */
#define TLBBFBS       (TLBBFBS_MASK      << TLBBFBS_SHIFT)	/* [한국어] TLBBFBS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 분기 예측 버퍼 쪽을 선택할지 여부. */


/* TLBTR0 */
#define PR            (PR_MASK           << PR_SHIFT)	/* [한국어] PR 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 특권 읽기 허용. */
#define PW            (PW_MASK           << PW_SHIFT)	/* [한국어] PW 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 특권 쓰기 허용. */
#define UR            (UR_MASK           << UR_SHIFT)	/* [한국어] UR 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 사용자 읽기 허용. */
#define UW            (UW_MASK           << UW_SHIFT)	/* [한국어] UW 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 사용자 쓰기 허용. */
#define XN            (XN_MASK           << XN_SHIFT)	/* [한국어] XN 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 실행 금지. */
#define NSDESC        (NSDESC_MASK       << NSDESC_SHIFT)	/* [한국어] NSDESC 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 비보안 서술자에서 왔다. */
#define ISH           (ISH_MASK          << ISH_SHIFT)	/* [한국어] ISH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 공유. */
#define SH            (SH_MASK           << SH_SHIFT)	/* [한국어] SH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 공유 속성. */
#define MT            (MT_MASK           << MT_SHIFT)	/* [한국어] MT 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 메모리 타입. */
#define DPSIZR        (DPSIZR_MASK       << DPSIZR_SHIFT)	/* [한국어] DPSIZR 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 서술자 페이지 크기(행). */
#define DPSIZC        (DPSIZC_MASK       << DPSIZC_SHIFT)	/* [한국어] DPSIZC 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 서술자 페이지 크기(열). */


/* TLBTR1 */
#define TLBTR1_VMID   (TLBTR1_VMID_MASK  << TLBTR1_VMID_SHIFT)	/* [한국어] TLBTR1_VMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 TLB 항목의 VMID. */
#define TLBTR1_PA     (TLBTR1_PA_MASK    << TLBTR1_PA_SHIFT)	/* [한국어] TLBTR1_PA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 TLB 항목의 물리 주소. */


/* TLBTR2 */
#define TLBTR2_ASID   (TLBTR2_ASID_MASK  << TLBTR2_ASID_SHIFT)	/* [한국어] TLBTR2_ASID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 TLB 항목의 ASID. */
#define TLBTR2_V      (TLBTR2_V_MASK     << TLBTR2_V_SHIFT)	/* [한국어] TLBTR2_V 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 항목이 유효한가. */
#define TLBTR2_NSTID  (TLBTR2_NSTID_MASK << TLBTR2_NSTID_SHIFT)	/* [한국어] TLBTR2_NSTID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 비보안 변환 식별자. */
#define TLBTR2_NV     (TLBTR2_NV_MASK    << TLBTR2_NV_SHIFT)	/* [한국어] TLBTR2_NV 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 비보안 유효 비트. */
#define TLBTR2_VA     (TLBTR2_VA_MASK    << TLBTR2_VA_SHIFT)	/* [한국어] TLBTR2_VA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 TLB 항목의 가상 주소. */


/* Context Register Fields */
/* ACTLR */
#define CFERE              (CFERE_MASK              << CFERE_SHIFT)	/* [한국어] CFERE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 컨텍스트 폴트 시 오류 응답 활성화. */
#define CFEIE              (CFEIE_MASK              << CFEIE_SHIFT)	/* [한국어] CFEIE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 컨텍스트 폴트 인터럽트 활성화. */
#define PTSHCFG            (PTSHCFG_MASK            << PTSHCFG_SHIFT)	/* [한국어] PTSHCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 페이지 테이블 접근의 공유 속성. */
#define RCOSH              (RCOSH_MASK              << RCOSH_SHIFT)	/* [한국어] RCOSH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 외부 공유 읽기 정책. */
#define RCISH              (RCISH_MASK              << RCISH_SHIFT)	/* [한국어] RCISH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 공유 읽기 정책. */
#define RCNSH              (RCNSH_MASK              << RCNSH_SHIFT)	/* [한국어] RCNSH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 비공유 읽기 정책. */
#define PRIVCFG            (PRIVCFG_MASK            << PRIVCFG_SHIFT)	/* [한국어] PRIVCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 특권 속성 설정. */
#define DNA                (DNA_MASK                << DNA_SHIFT)	/* [한국어] DNA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 접근 플래그 갱신 금지. */
#define DNLV2PA            (DNLV2PA_MASK            << DNLV2PA_SHIFT)	/* [한국어] DNLV2PA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 2단계 물리 주소 갱신 금지. */
#define TLBMCFG            (TLBMCFG_MASK            << TLBMCFG_SHIFT)	/* [한국어] TLBMCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TLB 다중 적중 처리 설정. */
#define CFCFG              (CFCFG_MASK              << CFCFG_SHIFT)	/* [한국어] CFCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 폴트 시 멈춰 세울지 종료할지. */
#define TIPCF              (TIPCF_MASK              << TIPCF_SHIFT)	/* [한국어] TIPCF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 변환 진행 중 폴트 설정. */
#define V2PCFG             (V2PCFG_MASK             << V2PCFG_SHIFT)	/* [한국어] V2PCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 가상→물리 요청의 동작 설정. */
#define HUME               (HUME_MASK               << HUME_SHIFT)	/* [한국어] HUME 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 하드웨어 업데이트 확장 활성화. */
#define PTMTCFG            (PTMTCFG_MASK            << PTMTCFG_SHIFT)	/* [한국어] PTMTCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테이블 접근의 메모리 타입 덮어쓰기. */
#define PTMEMTYPE          (PTMEMTYPE_MASK          << PTMEMTYPE_SHIFT)	/* [한국어] PTMEMTYPE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테이블 접근에 강제할 메모리 타입. */


/* BFBCR */
#define BFBDFE             (BFBDFE_MASK             << BFBDFE_SHIFT)	/* [한국어] BFBDFE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 분기 예측 데이터 인출 활성화. */
#define BFBSFE             (BFBSFE_MASK             << BFBSFE_SHIFT)	/* [한국어] BFBSFE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 분기 예측 추측 인출 활성화. */
#define SFVS               (SFVS_MASK               << SFVS_SHIFT)	/* [한국어] SFVS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 추측 인출의 유효 크기. */
#define FLVIC              (FLVIC_MASK              << FLVIC_SHIFT)	/* [한국어] FLVIC 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 1단계 예측 항목 수. */
#define SLVIC              (SLVIC_MASK              << SLVIC_SHIFT)	/* [한국어] SLVIC 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 2단계 예측 항목 수. */


/* CONTEXTIDR */
#define CONTEXTIDR_ASID    (CONTEXTIDR_ASID_MASK    << CONTEXTIDR_ASID_SHIFT)	/* [한국어] CONTEXTIDR_ASID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 컨텍스트의 ASID — TLB 태그가 된다. */
#define PROCID             (PROCID_MASK             << PROCID_SHIFT)	/* [한국어] PROCID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 소프트웨어가 쓰는 프로세스 식별자. */


/* FSR */
#define TF                 (TF_MASK                 << TF_SHIFT)	/* [한국어] TF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 변환 폴트 — 매핑이 없다. */
#define AFF                (AFF_MASK                << AFF_SHIFT)	/* [한국어] AFF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 접근 플래그 폴트. */
#define APF                (APF_MASK                << APF_SHIFT)	/* [한국어] APF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 권한 폴트. */
#define TLBMF              (TLBMF_MASK              << TLBMF_SHIFT)	/* [한국어] TLBMF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TLB 다중 적중. */
#define HTWDEEF            (HTWDEEF_MASK            << HTWDEEF_SHIFT)	/* [한국어] HTWDEEF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테이블 워크 중 디코드 오류. */
#define HTWSEEF            (HTWSEEF_MASK            << HTWSEEF_SHIFT)	/* [한국어] HTWSEEF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테이블 워크 중 슬레이브 오류. */
#define MHF                (MHF_MASK                << MHF_SHIFT)	/* [한국어] MHF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 다중 적중 폴트. */
#define SL                 (SL_MASK                 << SL_SHIFT)	/* [한국어] SL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 두 번째 단계에서 났는가. */
#define SS                 (SS_MASK                 << SS_SHIFT)	/* [한국어] SS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 트랜잭션이 멈춰 세워졌다. */
#define MULTI              (MULTI_MASK              << MULTI_SHIFT)	/* [한국어] MULTI 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 폴트가 겹쳐서 났다. */


/* FSYNR0 */
#define AMID               (AMID_MASK               << AMID_SHIFT)	/* [한국어] AMID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 폴트를 낸 마스터 ID. */
#define APID               (APID_MASK               << APID_SHIFT)	/* [한국어] APID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 폴트를 낸 페이지 ID. */
#define ABID               (ABID_MASK               << ABID_SHIFT)	/* [한국어] ABID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 폴트를 낸 버스 ID. */
#define ATID               (ATID_MASK               << ATID_SHIFT)	/* [한국어] ATID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 폴트를 낸 트랜잭션 ID. */


/* FSYNR1 */
#define AMEMTYPE           (AMEMTYPE_MASK           << AMEMTYPE_SHIFT)	/* [한국어] AMEMTYPE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 그 트랜잭션의 메모리 타입. */
#define ASHARED            (ASHARED_MASK            << ASHARED_SHIFT)	/* [한국어] ASHARED 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 공유 접근이었는가. */
#define AINNERSHARED       (AINNERSHARED_MASK       << AINNERSHARED_SHIFT)	/* [한국어] AINNERSHARED 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 공유 접근이었는가. */
#define APRIV              (APRIV_MASK              << APRIV_SHIFT)	/* [한국어] APRIV 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 특권 접근이었는가. */
#define APROTNS            (APROTNS_MASK            << APROTNS_SHIFT)	/* [한국어] APROTNS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 비보안 접근이었는가. */
#define AINST              (AINST_MASK              << AINST_SHIFT)	/* [한국어] AINST 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 명령 인출이었는가. */
#define AWRITE             (AWRITE_MASK             << AWRITE_SHIFT)	/* [한국어] AWRITE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 쓰기였는가. */
#define ABURST             (ABURST_MASK             << ABURST_SHIFT)	/* [한국어] ABURST 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 버스트 종류. */
#define ALEN               (ALEN_MASK               << ALEN_SHIFT)	/* [한국어] ALEN 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 버스트 길이. */
#define FSYNR1_ASIZE       (FSYNR1_ASIZE_MASK       << FSYNR1_ASIZE_SHIFT)	/* [한국어] FSYNR1_ASIZE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 전송 크기. */
#define ALOCK              (ALOCK_MASK              << ALOCK_SHIFT)	/* [한국어] ALOCK 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 잠금 전송이었는가. */
#define AFULL              (AFULL_MASK              << AFULL_SHIFT)	/* [한국어] AFULL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 버퍼가 가득 찼는가. */


/* NMRR */
#define ICPC0              (ICPC0_MASK              << ICPC0_SHIFT)	/* [한국어] ICPC0 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 0의 내부 캐시 정책. */
#define ICPC1              (ICPC1_MASK              << ICPC1_SHIFT)	/* [한국어] ICPC1 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 1의 내부 캐시 정책. */
#define ICPC2              (ICPC2_MASK              << ICPC2_SHIFT)	/* [한국어] ICPC2 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 2의 내부 캐시 정책. */
#define ICPC3              (ICPC3_MASK              << ICPC3_SHIFT)	/* [한국어] ICPC3 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 3의 내부 캐시 정책. */
#define ICPC4              (ICPC4_MASK              << ICPC4_SHIFT)	/* [한국어] ICPC4 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 4의 내부 캐시 정책. */
#define ICPC5              (ICPC5_MASK              << ICPC5_SHIFT)	/* [한국어] ICPC5 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 5의 내부 캐시 정책. */
#define ICPC6              (ICPC6_MASK              << ICPC6_SHIFT)	/* [한국어] ICPC6 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 6의 내부 캐시 정책. */
#define ICPC7              (ICPC7_MASK              << ICPC7_SHIFT)	/* [한국어] ICPC7 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 7의 내부 캐시 정책. */
#define OCPC0              (OCPC0_MASK              << OCPC0_SHIFT)	/* [한국어] OCPC0 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 0의 외부 캐시 정책. */
#define OCPC1              (OCPC1_MASK              << OCPC1_SHIFT)	/* [한국어] OCPC1 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 1의 외부 캐시 정책. */
#define OCPC2              (OCPC2_MASK              << OCPC2_SHIFT)	/* [한국어] OCPC2 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 2의 외부 캐시 정책. */
#define OCPC3              (OCPC3_MASK              << OCPC3_SHIFT)	/* [한국어] OCPC3 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 3의 외부 캐시 정책. */
#define OCPC4              (OCPC4_MASK              << OCPC4_SHIFT)	/* [한국어] OCPC4 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 4의 외부 캐시 정책. */
#define OCPC5              (OCPC5_MASK              << OCPC5_SHIFT)	/* [한국어] OCPC5 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 5의 외부 캐시 정책. */
#define OCPC6              (OCPC6_MASK              << OCPC6_SHIFT)	/* [한국어] OCPC6 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 6의 외부 캐시 정책. */
#define OCPC7              (OCPC7_MASK              << OCPC7_SHIFT)	/* [한국어] OCPC7 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 7의 외부 캐시 정책. */


/* PAR */
#define FAULT              (FAULT_MASK              << FAULT_SHIFT)	/* [한국어] FAULT 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 변환이 실패했는가 — 이 비트로 아래 두 해석이 갈린다. */
/* If a fault is present, these are the
same as the fault fields in the FAR */
#define FAULT_TF           (FAULT_TF_MASK           << FAULT_TF_SHIFT)	/* [한국어] FAULT_TF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 변환 폴트였다. */
#define FAULT_AFF          (FAULT_AFF_MASK          << FAULT_AFF_SHIFT)	/* [한국어] FAULT_AFF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 접근 플래그 폴트였다. */
#define FAULT_APF          (FAULT_APF_MASK          << FAULT_APF_SHIFT)	/* [한국어] FAULT_APF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 권한 폴트였다. */
#define FAULT_TLBMF        (FAULT_TLBMF_MASK        << FAULT_TLBMF_SHIFT)	/* [한국어] FAULT_TLBMF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TLB 다중 적중이었다. */
#define FAULT_HTWDEEF      (FAULT_HTWDEEF_MASK      << FAULT_HTWDEEF_SHIFT)	/* [한국어] FAULT_HTWDEEF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 워크 중 디코드 오류였다. */
#define FAULT_HTWSEEF      (FAULT_HTWSEEF_MASK      << FAULT_HTWSEEF_SHIFT)	/* [한국어] FAULT_HTWSEEF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 워크 중 슬레이브 오류였다. */
#define FAULT_MHF          (FAULT_MHF_MASK          << FAULT_MHF_SHIFT)	/* [한국어] FAULT_MHF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 다중 적중 폴트였다. */
#define FAULT_SL           (FAULT_SL_MASK           << FAULT_SL_SHIFT)	/* [한국어] FAULT_SL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 두 번째 단계에서 났다. */
#define FAULT_SS           (FAULT_SS_MASK           << FAULT_SS_SHIFT)	/* [한국어] FAULT_SS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 멈춰 세워졌다. */

/* If NO fault is present, the following fields are in effect */
/* (FAULT remains as before) */
#define PAR_NOFAULT_SS     (PAR_NOFAULT_SS_MASK     << PAR_NOFAULT_SS_SHIFT)	/* [한국어] PAR_NOFAULT_SS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 성공 시 — 멈춤 상태. */
#define PAR_NOFAULT_MT     (PAR_NOFAULT_MT_MASK     << PAR_NOFAULT_MT_SHIFT)	/* [한국어] PAR_NOFAULT_MT 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 성공 시 — 메모리 타입. */
#define PAR_NOFAULT_SH     (PAR_NOFAULT_SH_MASK     << PAR_NOFAULT_SH_SHIFT)	/* [한국어] PAR_NOFAULT_SH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 성공 시 — 공유 속성. */
#define PAR_NOFAULT_NS     (PAR_NOFAULT_NS_MASK     << PAR_NOFAULT_NS_SHIFT)	/* [한국어] PAR_NOFAULT_NS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 성공 시 — 비보안 여부. */
#define PAR_NOFAULT_NOS    (PAR_NOFAULT_NOS_MASK    << PAR_NOFAULT_NOS_SHIFT)	/* [한국어] PAR_NOFAULT_NOS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 성공 시 — 외부 공유 여부. */
#define PAR_NPFAULT_PA     (PAR_NPFAULT_PA_MASK     << PAR_NPFAULT_PA_SHIFT)	/* [한국어] PAR_NPFAULT_PA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 성공 시 — 변환된 물리 주소. */


/* PRRR */
#define MTC0               (MTC0_MASK               << MTC0_SHIFT)	/* [한국어] MTC0 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 0의 메모리 타입. */
#define MTC1               (MTC1_MASK               << MTC1_SHIFT)	/* [한국어] MTC1 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 1의 메모리 타입. */
#define MTC2               (MTC2_MASK               << MTC2_SHIFT)	/* [한국어] MTC2 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 2의 메모리 타입. */
#define MTC3               (MTC3_MASK               << MTC3_SHIFT)	/* [한국어] MTC3 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 3의 메모리 타입. */
#define MTC4               (MTC4_MASK               << MTC4_SHIFT)	/* [한국어] MTC4 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 4의 메모리 타입. */
#define MTC5               (MTC5_MASK               << MTC5_SHIFT)	/* [한국어] MTC5 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 5의 메모리 타입. */
#define MTC6               (MTC6_MASK               << MTC6_SHIFT)	/* [한국어] MTC6 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 6의 메모리 타입. */
#define MTC7               (MTC7_MASK               << MTC7_SHIFT)	/* [한국어] MTC7 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 7의 메모리 타입. */
#define SHDSH0             (SHDSH0_MASK             << SHDSH0_SHIFT)	/* [한국어] SHDSH0 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 장치 메모리 0의 공유 속성. */
#define SHDSH1             (SHDSH1_MASK             << SHDSH1_SHIFT)	/* [한국어] SHDSH1 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 장치 메모리 1의 공유 속성. */
#define SHNMSH0            (SHNMSH0_MASK            << SHNMSH0_SHIFT)	/* [한국어] SHNMSH0 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 일반 메모리 0의 공유 속성. */
#define SHNMSH1            (SHNMSH1_MASK            << SHNMSH1_SHIFT)	/* [한국어] SHNMSH1 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 일반 메모리 1의 공유 속성. */
#define NOS0               (NOS0_MASK               << NOS0_SHIFT)	/* [한국어] NOS0 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 0가 외부 공유인가. */
#define NOS1               (NOS1_MASK               << NOS1_SHIFT)	/* [한국어] NOS1 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 1가 외부 공유인가. */
#define NOS2               (NOS2_MASK               << NOS2_SHIFT)	/* [한국어] NOS2 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 2가 외부 공유인가. */
#define NOS3               (NOS3_MASK               << NOS3_SHIFT)	/* [한국어] NOS3 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 3가 외부 공유인가. */
#define NOS4               (NOS4_MASK               << NOS4_SHIFT)	/* [한국어] NOS4 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 4가 외부 공유인가. */
#define NOS5               (NOS5_MASK               << NOS5_SHIFT)	/* [한국어] NOS5 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 5가 외부 공유인가. */
#define NOS6               (NOS6_MASK               << NOS6_SHIFT)	/* [한국어] NOS6 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 6가 외부 공유인가. */
#define NOS7               (NOS7_MASK               << NOS7_SHIFT)	/* [한국어] NOS7 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 클래스 7가 외부 공유인가. */


/* RESUME */
#define TNR                (TNR_MASK                << TNR_SHIFT)	/* [한국어] TNR 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 재개할지 종료할지를 고른다. */


/* SCTLR */
#define M                  (M_MASK                  << M_SHIFT)	/* [한국어] M 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 이 컨텍스트의 MMU 활성화. */
#define TRE                (TRE_MASK                << TRE_SHIFT)	/* [한국어] TRE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TEX 재매핑 활성화. */
#define AFE                (AFE_MASK                << AFE_SHIFT)	/* [한국어] AFE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 접근 플래그 활성화. */
#define HAF                (HAF_MASK                << HAF_SHIFT)	/* [한국어] HAF 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 하드웨어 접근 플래그 갱신. */
#define BE                 (BE_MASK                 << BE_SHIFT)	/* [한국어] BE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 빅엔디언 테이블. */
#define AFFD               (AFFD_MASK               << AFFD_SHIFT)	/* [한국어] AFFD 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 접근 플래그 폴트 비활성화. */


/* TLBIASID */
#define TLBIASID_ASID      (TLBIASID_ASID_MASK      << TLBIASID_ASID_SHIFT)	/* [한국어] TLBIASID_ASID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 무효화할 ASID. */


/* TLBIVA */
#define TLBIVA_ASID        (TLBIVA_ASID_MASK        << TLBIVA_ASID_SHIFT)	/* [한국어] TLBIVA_ASID 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 무효화할 ASID. */
#define TLBIVA_VA          (TLBIVA_VA_MASK          << TLBIVA_VA_SHIFT)	/* [한국어] TLBIVA_VA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 무효화할 가상 주소. */


/* TLBIVAA */
#define TLBIVAA_VA         (TLBIVAA_VA_MASK         << TLBIVAA_VA_SHIFT)	/* [한국어] TLBIVAA_VA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 무효화할 가상 주소. */


/* TLBLCKR */
#define LKE                (LKE_MASK                << LKE_SHIFT)	/* [한국어] LKE 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TLB 잠금 활성화. */
#define TLBLCKR_TLBIALLCFG (TLBLCKR_TLBIALLCFG_MASK<<TLBLCKR_TLBIALLCFG_SHIFT)	/* [한국어] TLBLCKR_TLBIALLCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 전체 무효화가 잠긴 항목도 지울지. */
#define TLBIASIDCFG        (TLBIASIDCFG_MASK        << TLBIASIDCFG_SHIFT)	/* [한국어] TLBIASIDCFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — ASID 무효화가 잠긴 항목도 지울지. */
#define TLBIVAACFG         (TLBIVAACFG_MASK         << TLBIVAACFG_SHIFT)	/* [한국어] TLBIVAACFG 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 주소 무효화가 잠긴 항목도 지울지. */
#define FLOOR              (FLOOR_MASK              << FLOOR_SHIFT)	/* [한국어] FLOOR 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 잠긴 영역의 하한. */
#define VICTIM             (VICTIM_MASK             << VICTIM_SHIFT)	/* [한국어] VICTIM 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 다음에 교체할 항목. */


/* TTBCR */
#define N                  (N_MASK                  << N_SHIFT)	/* [한국어] N 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TTBR0과 TTBR1의 경계를 정하는 비트 수. */
#define PD0                (PD0_MASK                << PD0_SHIFT)	/* [한국어] PD0 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TTBR0 워크 비활성화. */
#define PD1                (PD1_MASK                << PD1_SHIFT)	/* [한국어] PD1 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — TTBR1 워크 비활성화. */


/* TTBR0 */
#define TTBR0_IRGNH        (TTBR0_IRGNH_MASK        << TTBR0_IRGNH_SHIFT)	/* [한국어] TTBR0_IRGNH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 캐시 정책(상위 비트). */
#define TTBR0_SH           (TTBR0_SH_MASK           << TTBR0_SH_SHIFT)	/* [한국어] TTBR0_SH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테이블 접근의 공유 속성. */
#define TTBR0_ORGN         (TTBR0_ORGN_MASK         << TTBR0_ORGN_SHIFT)	/* [한국어] TTBR0_ORGN 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 외부 캐시 정책. */
#define TTBR0_NOS          (TTBR0_NOS_MASK          << TTBR0_NOS_SHIFT)	/* [한국어] TTBR0_NOS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 외부 공유 여부. */
#define TTBR0_IRGNL        (TTBR0_IRGNL_MASK        << TTBR0_IRGNL_SHIFT)	/* [한국어] TTBR0_IRGNL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 캐시 정책(하위 비트). */
#define TTBR0_PA           (TTBR0_PA_MASK           << TTBR0_PA_SHIFT)	/* [한국어] TTBR0_PA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 1단계 테이블의 물리 주소. */


/* TTBR1 */
#define TTBR1_IRGNH        (TTBR1_IRGNH_MASK        << TTBR1_IRGNH_SHIFT)	/* [한국어] TTBR1_IRGNH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 캐시 정책(상위 비트). */
#define TTBR1_SH           (TTBR1_SH_MASK           << TTBR1_SH_SHIFT)	/* [한국어] TTBR1_SH 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 테이블 접근의 공유 속성. */
#define TTBR1_ORGN         (TTBR1_ORGN_MASK         << TTBR1_ORGN_SHIFT)	/* [한국어] TTBR1_ORGN 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 외부 캐시 정책. */
#define TTBR1_NOS          (TTBR1_NOS_MASK          << TTBR1_NOS_SHIFT)	/* [한국어] TTBR1_NOS 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 외부 공유 여부. */
#define TTBR1_IRGNL        (TTBR1_IRGNL_MASK        << TTBR1_IRGNL_SHIFT)	/* [한국어] TTBR1_IRGNL 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 내부 캐시 정책(하위 비트). */
#define TTBR1_PA           (TTBR1_PA_MASK           << TTBR1_PA_SHIFT)	/* [한국어] TTBR1_PA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 1단계 테이블의 물리 주소. */


/* V2PSR */
#define HIT                (HIT_MASK                << HIT_SHIFT)	/* [한국어] HIT 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 변환에 성공했는가. */
#define INDEX              (INDEX_MASK              << INDEX_SHIFT)	/* [한국어] INDEX 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 적중한 TLB 항목의 번호. */


/* V2Pxx */
#define V2Pxx_INDEX        (V2Pxx_INDEX_MASK        << V2Pxx_INDEX_SHIFT)	/* [한국어] V2Pxx_INDEX 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 요청 식별 번호. */
#define V2Pxx_VA           (V2Pxx_VA_MASK           << V2Pxx_VA_SHIFT)	/* [한국어] V2Pxx_VA 필드가 차지하는 비트 자리(마스크를 제자리로 민 값) — 변환을 요청할 가상 주소. */


/* Global Register Masks */
/* CBACRn */
#define RWVMID_MASK               0x1F	/* [한국어] RWVMID 필드의 폭을 나타내는 마스크 — 라운드로빈 중재에 쓸 VMID. */
#define RWE_MASK                  0x01	/* [한국어] RWE 필드의 폭을 나타내는 마스크 — 라운드로빈 중재 활성화. */
#define RWGE_MASK                 0x01	/* [한국어] RWGE 필드의 폭을 나타내는 마스크 — 라운드로빈 전역 활성화. */
#define CBVMID_MASK               0x1F	/* [한국어] CBVMID 필드의 폭을 나타내는 마스크 — 이 컨텍스트 뱅크가 쓸 VMID. */
#define IRPTNDX_MASK              0xFF	/* [한국어] IRPTNDX 필드의 폭을 나타내는 마스크 — 이 컨텍스트가 쓸 인터럽트 번호. */


/* CR */
#define RPUE_MASK                 0x01	/* [한국어] RPUE 필드의 폭을 나타내는 마스크 — RPU 활성화. */
#define RPUERE_MASK               0x01	/* [한국어] RPUERE 필드의 폭을 나타내는 마스크 — RPU 오류 응답 활성화. */
#define RPUEIE_MASK               0x01	/* [한국어] RPUEIE 필드의 폭을 나타내는 마스크 — RPU 오류 인터럽트 활성화. */
#define DCDEE_MASK                0x01	/* [한국어] DCDEE 필드의 폭을 나타내는 마스크 — 디코드 오류 인터럽트 활성화. */
#define CLIENTPD_MASK             0x01	/* [한국어] CLIENTPD 필드의 폭을 나타내는 마스크 — 클라이언트를 통과 모드로 둘지 여부. */
#define STALLD_MASK               0x01	/* [한국어] STALLD 필드의 폭을 나타내는 마스크 — 폴트 시 트랜잭션을 멈춰 세울지 여부. */
#define TLBLKCRWE_MASK            0x01	/* [한국어] TLBLKCRWE 필드의 폭을 나타내는 마스크 — TLB 잠금 레지스터 쓰기 허용. */
#define CR_TLBIALLCFG_MASK        0x01	/* [한국어] CR_TLBIALLCFG 필드의 폭을 나타내는 마스크 — 전역 TLB 무효화의 범위 설정. */
#define TLBIVMIDCFG_MASK          0x01	/* [한국어] TLBIVMIDCFG 필드의 폭을 나타내는 마스크 — VMID 무효화의 범위 설정. */
#define CR_HUME_MASK              0x01	/* [한국어] CR_HUME 필드의 폭을 나타내는 마스크 — 하드웨어 업데이트 확장 활성화. */


/* ESR */
#define CFG_MASK                  0x01	/* [한국어] CFG 필드의 폭을 나타내는 마스크 — 설정 오류가 났다. */
#define BYPASS_MASK               0x01	/* [한국어] BYPASS 필드의 폭을 나타내는 마스크 — 통과 모드에서 오류가 났다. */
#define ESR_MULTI_MASK            0x01	/* [한국어] ESR_MULTI 필드의 폭을 나타내는 마스크 — 오류가 여러 번 겹쳤다. */


/* ESYNR0 */
#define ESYNR0_AMID_MASK          0xFF	/* [한국어] ESYNR0_AMID 필드의 폭을 나타내는 마스크 — 오류를 낸 마스터 ID. */
#define ESYNR0_APID_MASK          0x1F	/* [한국어] ESYNR0_APID 필드의 폭을 나타내는 마스크 — 오류를 낸 페이지 ID. */
#define ESYNR0_ABID_MASK          0x07	/* [한국어] ESYNR0_ABID 필드의 폭을 나타내는 마스크 — 오류를 낸 버스 ID. */
#define ESYNR0_AVMID_MASK         0x1F	/* [한국어] ESYNR0_AVMID 필드의 폭을 나타내는 마스크 — 오류를 낸 VMID. */
#define ESYNR0_ATID_MASK          0xFF	/* [한국어] ESYNR0_ATID 필드의 폭을 나타내는 마스크 — 오류를 낸 트랜잭션 ID. */


/* ESYNR1 */
#define ESYNR1_AMEMTYPE_MASK             0x07	/* [한국어] ESYNR1_AMEMTYPE 필드의 폭을 나타내는 마스크 — 그 트랜잭션의 메모리 타입. */
#define ESYNR1_ASHARED_MASK              0x01	/* [한국어] ESYNR1_ASHARED 필드의 폭을 나타내는 마스크 — 공유 접근이었는가. */
#define ESYNR1_AINNERSHARED_MASK         0x01	/* [한국어] ESYNR1_AINNERSHARED 필드의 폭을 나타내는 마스크 — 내부 공유 접근이었는가. */
#define ESYNR1_APRIV_MASK                0x01	/* [한국어] ESYNR1_APRIV 필드의 폭을 나타내는 마스크 — 특권 접근이었는가. */
#define ESYNR1_APROTNS_MASK              0x01	/* [한국어] ESYNR1_APROTNS 필드의 폭을 나타내는 마스크 — 비보안 접근이었는가. */
#define ESYNR1_AINST_MASK                0x01	/* [한국어] ESYNR1_AINST 필드의 폭을 나타내는 마스크 — 명령 인출이었는가. */
#define ESYNR1_AWRITE_MASK               0x01	/* [한국어] ESYNR1_AWRITE 필드의 폭을 나타내는 마스크 — 쓰기였는가. */
#define ESYNR1_ABURST_MASK               0x01	/* [한국어] ESYNR1_ABURST 필드의 폭을 나타내는 마스크 — 버스트 종류. */
#define ESYNR1_ALEN_MASK                 0x0F	/* [한국어] ESYNR1_ALEN 필드의 폭을 나타내는 마스크 — 버스트 길이. */
#define ESYNR1_ASIZE_MASK                0x01	/* [한국어] ESYNR1_ASIZE 필드의 폭을 나타내는 마스크 — 전송 크기. */
#define ESYNR1_ALOCK_MASK                0x03	/* [한국어] ESYNR1_ALOCK 필드의 폭을 나타내는 마스크 — 잠금 전송이었는가. */
#define ESYNR1_AOOO_MASK                 0x01	/* [한국어] ESYNR1_AOOO 필드의 폭을 나타내는 마스크 — 순서 없는 전송이었는가. */
#define ESYNR1_AFULL_MASK                0x01	/* [한국어] ESYNR1_AFULL 필드의 폭을 나타내는 마스크 — 버퍼가 가득 찼는가. */
#define ESYNR1_AC_MASK                   0x01	/* [한국어] ESYNR1_AC 필드의 폭을 나타내는 마스크 — 캐시 가능 접근이었는가. */
#define ESYNR1_DCD_MASK                  0x01	/* [한국어] ESYNR1_DCD 필드의 폭을 나타내는 마스크 — 디코드 오류였는가. */


/* IDR */
#define NM2VCBMT_MASK             0x1FF	/* [한국어] NM2VCBMT 필드의 폭을 나타내는 마스크 — 스트림 대응표의 항목 수. */
#define HTW_MASK                  0x01	/* [한국어] HTW 필드의 폭을 나타내는 마스크 — 하드웨어 테이블 워크 지원 여부. */
#define HUM_MASK                  0x01	/* [한국어] HUM 필드의 폭을 나타내는 마스크 — 하드웨어 업데이트 확장 지원 여부. */
#define TLBSIZE_MASK              0x0F	/* [한국어] TLBSIZE 필드의 폭을 나타내는 마스크 — TLB 항목 수. */
#define NCB_MASK                  0xFF	/* [한국어] NCB 필드의 폭을 나타내는 마스크 — 컨텍스트 뱅크의 개수. */
#define NIRPT_MASK                0xFF	/* [한국어] NIRPT 필드의 폭을 나타내는 마스크 — 인터럽트 선의 개수. */


/* M2VCBRn */
#define VMID_MASK                 0x1F	/* [한국어] VMID 필드의 폭을 나타내는 마스크 — 이 스트림에 부여할 VMID. */
#define CBNDX_MASK                0xFF	/* [한국어] CBNDX 필드의 폭을 나타내는 마스크 — 이 스트림이 쓸 컨텍스트 뱅크 번호. */
#define BYPASSD_MASK              0x01	/* [한국어] BYPASSD 필드의 폭을 나타내는 마스크 — 통과 모드에서의 디버그 표시. */
#define BPRCOSH_MASK              0x01	/* [한국어] BPRCOSH 필드의 폭을 나타내는 마스크 — 통과 시 외부 공유 캐시 정책. */
#define BPRCISH_MASK              0x01	/* [한국어] BPRCISH 필드의 폭을 나타내는 마스크 — 통과 시 내부 공유 캐시 정책. */
#define BPRCNSH_MASK              0x01	/* [한국어] BPRCNSH 필드의 폭을 나타내는 마스크 — 통과 시 비공유 캐시 정책. */
#define BPSHCFG_MASK              0x03	/* [한국어] BPSHCFG 필드의 폭을 나타내는 마스크 — 통과 시 공유 속성 설정. */
#define NSCFG_MASK                0x03	/* [한국어] NSCFG 필드의 폭을 나타내는 마스크 — 보안/비보안 속성 설정. */
#define BPMTCFG_MASK              0x01	/* [한국어] BPMTCFG 필드의 폭을 나타내는 마스크 — 통과 시 메모리 타입을 덮어쓸지 여부. */
#define BPMEMTYPE_MASK            0x07	/* [한국어] BPMEMTYPE 필드의 폭을 나타내는 마스크 — 통과 시 강제할 메모리 타입. */


/* REV */
#define MINOR_MASK                0x0F	/* [한국어] MINOR 필드의 폭을 나타내는 마스크. */
#define MAJOR_MASK                0x0F	/* [한국어] MAJOR 필드의 폭을 나타내는 마스크. */


/* TESTBUSCR */
#define TBE_MASK                  0x01	/* [한국어] TBE 필드의 폭을 나타내는 마스크 — 테스트 버스 활성화. */
#define SPDMBE_MASK               0x01	/* [한국어] SPDMBE 필드의 폭을 나타내는 마스크 — SPDM 버스 활성화. */
#define WGSEL_MASK                0x03	/* [한국어] WGSEL 필드의 폭을 나타내는 마스크 — 파형 그룹 선택. */
#define TBLSEL_MASK               0x03	/* [한국어] TBLSEL 필드의 폭을 나타내는 마스크 — 테스트 버스 하위 선택. */
#define TBHSEL_MASK               0x03	/* [한국어] TBHSEL 필드의 폭을 나타내는 마스크 — 테스트 버스 상위 선택. */
#define SPDM0SEL_MASK             0x0F	/* [한국어] SPDM0SEL 필드의 폭을 나타내는 마스크 — SPDM 0번 선택. */
#define SPDM1SEL_MASK             0x0F	/* [한국어] SPDM1SEL 필드의 폭을 나타내는 마스크 — SPDM 1번 선택. */
#define SPDM2SEL_MASK             0x0F	/* [한국어] SPDM2SEL 필드의 폭을 나타내는 마스크 — SPDM 2번 선택. */
#define SPDM3SEL_MASK             0x0F	/* [한국어] SPDM3SEL 필드의 폭을 나타내는 마스크 — SPDM 3번 선택. */


/* TLBIMID */
#define TLBIVMID_VMID_MASK        0x1F	/* [한국어] TLBIVMID_VMID 필드의 폭을 나타내는 마스크 — 무효화할 VMID. */


/* TLBRSW */
#define TLBRSW_INDEX_MASK         0xFF	/* [한국어] TLBRSW_INDEX 필드의 폭을 나타내는 마스크 — 읽거나 쓸 TLB 항목의 번호. */
#define TLBBFBS_MASK              0x03	/* [한국어] TLBBFBS 필드의 폭을 나타내는 마스크 — 분기 예측 버퍼 쪽을 선택할지 여부. */


/* TLBTR0 */
#define PR_MASK                   0x01	/* [한국어] PR 필드의 폭을 나타내는 마스크 — 특권 읽기 허용. */
#define PW_MASK                   0x01	/* [한국어] PW 필드의 폭을 나타내는 마스크 — 특권 쓰기 허용. */
#define UR_MASK                   0x01	/* [한국어] UR 필드의 폭을 나타내는 마스크 — 사용자 읽기 허용. */
#define UW_MASK                   0x01	/* [한국어] UW 필드의 폭을 나타내는 마스크 — 사용자 쓰기 허용. */
#define XN_MASK                   0x01	/* [한국어] XN 필드의 폭을 나타내는 마스크 — 실행 금지. */
#define NSDESC_MASK               0x01	/* [한국어] NSDESC 필드의 폭을 나타내는 마스크 — 비보안 서술자에서 왔다. */
#define ISH_MASK                  0x01	/* [한국어] ISH 필드의 폭을 나타내는 마스크 — 내부 공유. */
#define SH_MASK                   0x01	/* [한국어] SH 필드의 폭을 나타내는 마스크 — 공유 속성. */
#define MT_MASK                   0x07	/* [한국어] MT 필드의 폭을 나타내는 마스크 — 메모리 타입. */
#define DPSIZR_MASK               0x07	/* [한국어] DPSIZR 필드의 폭을 나타내는 마스크 — 서술자 페이지 크기(행). */
#define DPSIZC_MASK               0x07	/* [한국어] DPSIZC 필드의 폭을 나타내는 마스크 — 서술자 페이지 크기(열). */


/* TLBTR1 */
#define TLBTR1_VMID_MASK          0x1F	/* [한국어] TLBTR1_VMID 필드의 폭을 나타내는 마스크 — 이 TLB 항목의 VMID. */
#define TLBTR1_PA_MASK            0x000FFFFF	/* [한국어] TLBTR1_PA 필드의 폭을 나타내는 마스크 — 이 TLB 항목의 물리 주소. */


/* TLBTR2 */
#define TLBTR2_ASID_MASK          0xFF	/* [한국어] TLBTR2_ASID 필드의 폭을 나타내는 마스크 — 이 TLB 항목의 ASID. */
#define TLBTR2_V_MASK             0x01	/* [한국어] TLBTR2_V 필드의 폭을 나타내는 마스크 — 이 항목이 유효한가. */
#define TLBTR2_NSTID_MASK         0x01	/* [한국어] TLBTR2_NSTID 필드의 폭을 나타내는 마스크 — 비보안 변환 식별자. */
#define TLBTR2_NV_MASK            0x01	/* [한국어] TLBTR2_NV 필드의 폭을 나타내는 마스크 — 비보안 유효 비트. */
#define TLBTR2_VA_MASK            0x000FFFFF	/* [한국어] TLBTR2_VA 필드의 폭을 나타내는 마스크 — 이 TLB 항목의 가상 주소. */


/* Global Register Shifts */
/* CBACRn */
#define RWVMID_SHIFT             0	/* [한국어] RWVMID 필드가 시작하는 비트 위치 — 라운드로빈 중재에 쓸 VMID. */
#define RWE_SHIFT                8	/* [한국어] RWE 필드가 시작하는 비트 위치 — 라운드로빈 중재 활성화. */
#define RWGE_SHIFT               9	/* [한국어] RWGE 필드가 시작하는 비트 위치 — 라운드로빈 전역 활성화. */
#define CBVMID_SHIFT             16	/* [한국어] CBVMID 필드가 시작하는 비트 위치 — 이 컨텍스트 뱅크가 쓸 VMID. */
#define IRPTNDX_SHIFT            24	/* [한국어] IRPTNDX 필드가 시작하는 비트 위치 — 이 컨텍스트가 쓸 인터럽트 번호. */


/* CR */
#define RPUE_SHIFT               0	/* [한국어] RPUE 필드가 시작하는 비트 위치 — RPU 활성화. */
#define RPUERE_SHIFT             1	/* [한국어] RPUERE 필드가 시작하는 비트 위치 — RPU 오류 응답 활성화. */
#define RPUEIE_SHIFT             2	/* [한국어] RPUEIE 필드가 시작하는 비트 위치 — RPU 오류 인터럽트 활성화. */
#define DCDEE_SHIFT              3	/* [한국어] DCDEE 필드가 시작하는 비트 위치 — 디코드 오류 인터럽트 활성화. */
#define CLIENTPD_SHIFT           4	/* [한국어] CLIENTPD 필드가 시작하는 비트 위치 — 클라이언트를 통과 모드로 둘지 여부. */
#define STALLD_SHIFT             5	/* [한국어] STALLD 필드가 시작하는 비트 위치 — 폴트 시 트랜잭션을 멈춰 세울지 여부. */
#define TLBLKCRWE_SHIFT          6	/* [한국어] TLBLKCRWE 필드가 시작하는 비트 위치 — TLB 잠금 레지스터 쓰기 허용. */
#define CR_TLBIALLCFG_SHIFT      7	/* [한국어] CR_TLBIALLCFG 필드가 시작하는 비트 위치 — 전역 TLB 무효화의 범위 설정. */
#define TLBIVMIDCFG_SHIFT        8	/* [한국어] TLBIVMIDCFG 필드가 시작하는 비트 위치 — VMID 무효화의 범위 설정. */
#define CR_HUME_SHIFT            9	/* [한국어] CR_HUME 필드가 시작하는 비트 위치 — 하드웨어 업데이트 확장 활성화. */


/* ESR */
#define CFG_SHIFT                0	/* [한국어] CFG 필드가 시작하는 비트 위치 — 설정 오류가 났다. */
#define BYPASS_SHIFT             1	/* [한국어] BYPASS 필드가 시작하는 비트 위치 — 통과 모드에서 오류가 났다. */
#define ESR_MULTI_SHIFT          31	/* [한국어] ESR_MULTI 필드가 시작하는 비트 위치 — 오류가 여러 번 겹쳤다. */


/* ESYNR0 */
#define ESYNR0_AMID_SHIFT        0	/* [한국어] ESYNR0_AMID 필드가 시작하는 비트 위치 — 오류를 낸 마스터 ID. */
#define ESYNR0_APID_SHIFT        8	/* [한국어] ESYNR0_APID 필드가 시작하는 비트 위치 — 오류를 낸 페이지 ID. */
#define ESYNR0_ABID_SHIFT        13	/* [한국어] ESYNR0_ABID 필드가 시작하는 비트 위치 — 오류를 낸 버스 ID. */
#define ESYNR0_AVMID_SHIFT       16	/* [한국어] ESYNR0_AVMID 필드가 시작하는 비트 위치 — 오류를 낸 VMID. */
#define ESYNR0_ATID_SHIFT        24	/* [한국어] ESYNR0_ATID 필드가 시작하는 비트 위치 — 오류를 낸 트랜잭션 ID. */


/* ESYNR1 */
#define ESYNR1_AMEMTYPE_SHIFT           0	/* [한국어] ESYNR1_AMEMTYPE 필드가 시작하는 비트 위치 — 그 트랜잭션의 메모리 타입. */
#define ESYNR1_ASHARED_SHIFT            3	/* [한국어] ESYNR1_ASHARED 필드가 시작하는 비트 위치 — 공유 접근이었는가. */
#define ESYNR1_AINNERSHARED_SHIFT       4	/* [한국어] ESYNR1_AINNERSHARED 필드가 시작하는 비트 위치 — 내부 공유 접근이었는가. */
#define ESYNR1_APRIV_SHIFT              5	/* [한국어] ESYNR1_APRIV 필드가 시작하는 비트 위치 — 특권 접근이었는가. */
#define ESYNR1_APROTNS_SHIFT            6	/* [한국어] ESYNR1_APROTNS 필드가 시작하는 비트 위치 — 비보안 접근이었는가. */
#define ESYNR1_AINST_SHIFT              7	/* [한국어] ESYNR1_AINST 필드가 시작하는 비트 위치 — 명령 인출이었는가. */
#define ESYNR1_AWRITE_SHIFT             8	/* [한국어] ESYNR1_AWRITE 필드가 시작하는 비트 위치 — 쓰기였는가. */
#define ESYNR1_ABURST_SHIFT             10	/* [한국어] ESYNR1_ABURST 필드가 시작하는 비트 위치 — 버스트 종류. */
#define ESYNR1_ALEN_SHIFT               12	/* [한국어] ESYNR1_ALEN 필드가 시작하는 비트 위치 — 버스트 길이. */
#define ESYNR1_ASIZE_SHIFT              16	/* [한국어] ESYNR1_ASIZE 필드가 시작하는 비트 위치 — 전송 크기. */
#define ESYNR1_ALOCK_SHIFT              20	/* [한국어] ESYNR1_ALOCK 필드가 시작하는 비트 위치 — 잠금 전송이었는가. */
#define ESYNR1_AOOO_SHIFT               22	/* [한국어] ESYNR1_AOOO 필드가 시작하는 비트 위치 — 순서 없는 전송이었는가. */
#define ESYNR1_AFULL_SHIFT              24	/* [한국어] ESYNR1_AFULL 필드가 시작하는 비트 위치 — 버퍼가 가득 찼는가. */
#define ESYNR1_AC_SHIFT                 30	/* [한국어] ESYNR1_AC 필드가 시작하는 비트 위치 — 캐시 가능 접근이었는가. */
#define ESYNR1_DCD_SHIFT                31	/* [한국어] ESYNR1_DCD 필드가 시작하는 비트 위치 — 디코드 오류였는가. */


/* IDR */
#define NM2VCBMT_SHIFT           0	/* [한국어] NM2VCBMT 필드가 시작하는 비트 위치 — 스트림 대응표의 항목 수. */
#define HTW_SHIFT                9	/* [한국어] HTW 필드가 시작하는 비트 위치 — 하드웨어 테이블 워크 지원 여부. */
#define HUM_SHIFT                10	/* [한국어] HUM 필드가 시작하는 비트 위치 — 하드웨어 업데이트 확장 지원 여부. */
#define TLBSIZE_SHIFT            12	/* [한국어] TLBSIZE 필드가 시작하는 비트 위치 — TLB 항목 수. */
#define NCB_SHIFT                16	/* [한국어] NCB 필드가 시작하는 비트 위치 — 컨텍스트 뱅크의 개수. */
#define NIRPT_SHIFT              24	/* [한국어] NIRPT 필드가 시작하는 비트 위치 — 인터럽트 선의 개수. */


/* M2VCBRn */
#define VMID_SHIFT               0	/* [한국어] VMID 필드가 시작하는 비트 위치 — 이 스트림에 부여할 VMID. */
#define CBNDX_SHIFT              8	/* [한국어] CBNDX 필드가 시작하는 비트 위치 — 이 스트림이 쓸 컨텍스트 뱅크 번호. */
#define BYPASSD_SHIFT            16	/* [한국어] BYPASSD 필드가 시작하는 비트 위치 — 통과 모드에서의 디버그 표시. */
#define BPRCOSH_SHIFT            17	/* [한국어] BPRCOSH 필드가 시작하는 비트 위치 — 통과 시 외부 공유 캐시 정책. */
#define BPRCISH_SHIFT            18	/* [한국어] BPRCISH 필드가 시작하는 비트 위치 — 통과 시 내부 공유 캐시 정책. */
#define BPRCNSH_SHIFT            19	/* [한국어] BPRCNSH 필드가 시작하는 비트 위치 — 통과 시 비공유 캐시 정책. */
#define BPSHCFG_SHIFT            20	/* [한국어] BPSHCFG 필드가 시작하는 비트 위치 — 통과 시 공유 속성 설정. */
#define NSCFG_SHIFT              22	/* [한국어] NSCFG 필드가 시작하는 비트 위치 — 보안/비보안 속성 설정. */
#define BPMTCFG_SHIFT            24	/* [한국어] BPMTCFG 필드가 시작하는 비트 위치 — 통과 시 메모리 타입을 덮어쓸지 여부. */
#define BPMEMTYPE_SHIFT          25	/* [한국어] BPMEMTYPE 필드가 시작하는 비트 위치 — 통과 시 강제할 메모리 타입. */


/* REV */
#define MINOR_SHIFT              0	/* [한국어] MINOR 필드가 시작하는 비트 위치. */
#define MAJOR_SHIFT              4	/* [한국어] MAJOR 필드가 시작하는 비트 위치. */


/* TESTBUSCR */
#define TBE_SHIFT                0	/* [한국어] TBE 필드가 시작하는 비트 위치 — 테스트 버스 활성화. */
#define SPDMBE_SHIFT             1	/* [한국어] SPDMBE 필드가 시작하는 비트 위치 — SPDM 버스 활성화. */
#define WGSEL_SHIFT              8	/* [한국어] WGSEL 필드가 시작하는 비트 위치 — 파형 그룹 선택. */
#define TBLSEL_SHIFT             12	/* [한국어] TBLSEL 필드가 시작하는 비트 위치 — 테스트 버스 하위 선택. */
#define TBHSEL_SHIFT             14	/* [한국어] TBHSEL 필드가 시작하는 비트 위치 — 테스트 버스 상위 선택. */
#define SPDM0SEL_SHIFT           16	/* [한국어] SPDM0SEL 필드가 시작하는 비트 위치 — SPDM 0번 선택. */
#define SPDM1SEL_SHIFT           20	/* [한국어] SPDM1SEL 필드가 시작하는 비트 위치 — SPDM 1번 선택. */
#define SPDM2SEL_SHIFT           24	/* [한국어] SPDM2SEL 필드가 시작하는 비트 위치 — SPDM 2번 선택. */
#define SPDM3SEL_SHIFT           28	/* [한국어] SPDM3SEL 필드가 시작하는 비트 위치 — SPDM 3번 선택. */


/* TLBIMID */
#define TLBIVMID_VMID_SHIFT      0	/* [한국어] TLBIVMID_VMID 필드가 시작하는 비트 위치 — 무효화할 VMID. */


/* TLBRSW */
#define TLBRSW_INDEX_SHIFT       0	/* [한국어] TLBRSW_INDEX 필드가 시작하는 비트 위치 — 읽거나 쓸 TLB 항목의 번호. */
#define TLBBFBS_SHIFT            8	/* [한국어] TLBBFBS 필드가 시작하는 비트 위치 — 분기 예측 버퍼 쪽을 선택할지 여부. */


/* TLBTR0 */
#define PR_SHIFT                 0	/* [한국어] PR 필드가 시작하는 비트 위치 — 특권 읽기 허용. */
#define PW_SHIFT                 1	/* [한국어] PW 필드가 시작하는 비트 위치 — 특권 쓰기 허용. */
#define UR_SHIFT                 2	/* [한국어] UR 필드가 시작하는 비트 위치 — 사용자 읽기 허용. */
#define UW_SHIFT                 3	/* [한국어] UW 필드가 시작하는 비트 위치 — 사용자 쓰기 허용. */
#define XN_SHIFT                 4	/* [한국어] XN 필드가 시작하는 비트 위치 — 실행 금지. */
#define NSDESC_SHIFT             6	/* [한국어] NSDESC 필드가 시작하는 비트 위치 — 비보안 서술자에서 왔다. */
#define ISH_SHIFT                7	/* [한국어] ISH 필드가 시작하는 비트 위치 — 내부 공유. */
#define SH_SHIFT                 8	/* [한국어] SH 필드가 시작하는 비트 위치 — 공유 속성. */
#define MT_SHIFT                 9	/* [한국어] MT 필드가 시작하는 비트 위치 — 메모리 타입. */
#define DPSIZR_SHIFT             16	/* [한국어] DPSIZR 필드가 시작하는 비트 위치 — 서술자 페이지 크기(행). */
#define DPSIZC_SHIFT             20	/* [한국어] DPSIZC 필드가 시작하는 비트 위치 — 서술자 페이지 크기(열). */


/* TLBTR1 */
#define TLBTR1_VMID_SHIFT        0	/* [한국어] TLBTR1_VMID 필드가 시작하는 비트 위치 — 이 TLB 항목의 VMID. */
#define TLBTR1_PA_SHIFT          12	/* [한국어] TLBTR1_PA 필드가 시작하는 비트 위치 — 이 TLB 항목의 물리 주소. */


/* TLBTR2 */
#define TLBTR2_ASID_SHIFT        0	/* [한국어] TLBTR2_ASID 필드가 시작하는 비트 위치 — 이 TLB 항목의 ASID. */
#define TLBTR2_V_SHIFT           8	/* [한국어] TLBTR2_V 필드가 시작하는 비트 위치 — 이 항목이 유효한가. */
#define TLBTR2_NSTID_SHIFT       9	/* [한국어] TLBTR2_NSTID 필드가 시작하는 비트 위치 — 비보안 변환 식별자. */
#define TLBTR2_NV_SHIFT          10	/* [한국어] TLBTR2_NV 필드가 시작하는 비트 위치 — 비보안 유효 비트. */
#define TLBTR2_VA_SHIFT          12	/* [한국어] TLBTR2_VA 필드가 시작하는 비트 위치 — 이 TLB 항목의 가상 주소. */


/* Context Register Masks */
/* ACTLR */
#define CFERE_MASK                       0x01	/* [한국어] CFERE 필드의 폭을 나타내는 마스크 — 컨텍스트 폴트 시 오류 응답 활성화. */
#define CFEIE_MASK                       0x01	/* [한국어] CFEIE 필드의 폭을 나타내는 마스크 — 컨텍스트 폴트 인터럽트 활성화. */
#define PTSHCFG_MASK                     0x03	/* [한국어] PTSHCFG 필드의 폭을 나타내는 마스크 — 페이지 테이블 접근의 공유 속성. */
#define RCOSH_MASK                       0x01	/* [한국어] RCOSH 필드의 폭을 나타내는 마스크 — 외부 공유 읽기 정책. */
#define RCISH_MASK                       0x01	/* [한국어] RCISH 필드의 폭을 나타내는 마스크 — 내부 공유 읽기 정책. */
#define RCNSH_MASK                       0x01	/* [한국어] RCNSH 필드의 폭을 나타내는 마스크 — 비공유 읽기 정책. */
#define PRIVCFG_MASK                     0x03	/* [한국어] PRIVCFG 필드의 폭을 나타내는 마스크 — 특권 속성 설정. */
#define DNA_MASK                         0x01	/* [한국어] DNA 필드의 폭을 나타내는 마스크 — 접근 플래그 갱신 금지. */
#define DNLV2PA_MASK                     0x01	/* [한국어] DNLV2PA 필드의 폭을 나타내는 마스크 — 2단계 물리 주소 갱신 금지. */
#define TLBMCFG_MASK                     0x03	/* [한국어] TLBMCFG 필드의 폭을 나타내는 마스크 — TLB 다중 적중 처리 설정. */
#define CFCFG_MASK                       0x01	/* [한국어] CFCFG 필드의 폭을 나타내는 마스크 — 폴트 시 멈춰 세울지 종료할지. */
#define TIPCF_MASK                       0x01	/* [한국어] TIPCF 필드의 폭을 나타내는 마스크 — 변환 진행 중 폴트 설정. */
#define V2PCFG_MASK                      0x03	/* [한국어] V2PCFG 필드의 폭을 나타내는 마스크 — 가상→물리 요청의 동작 설정. */
#define HUME_MASK                        0x01	/* [한국어] HUME 필드의 폭을 나타내는 마스크 — 하드웨어 업데이트 확장 활성화. */
#define PTMTCFG_MASK                     0x01	/* [한국어] PTMTCFG 필드의 폭을 나타내는 마스크 — 테이블 접근의 메모리 타입 덮어쓰기. */
#define PTMEMTYPE_MASK                   0x07	/* [한국어] PTMEMTYPE 필드의 폭을 나타내는 마스크 — 테이블 접근에 강제할 메모리 타입. */


/* BFBCR */
#define BFBDFE_MASK                      0x01	/* [한국어] BFBDFE 필드의 폭을 나타내는 마스크 — 분기 예측 데이터 인출 활성화. */
#define BFBSFE_MASK                      0x01	/* [한국어] BFBSFE 필드의 폭을 나타내는 마스크 — 분기 예측 추측 인출 활성화. */
#define SFVS_MASK                        0x01	/* [한국어] SFVS 필드의 폭을 나타내는 마스크 — 추측 인출의 유효 크기. */
#define FLVIC_MASK                       0x0F	/* [한국어] FLVIC 필드의 폭을 나타내는 마스크 — 1단계 예측 항목 수. */
#define SLVIC_MASK                       0x0F	/* [한국어] SLVIC 필드의 폭을 나타내는 마스크 — 2단계 예측 항목 수. */


/* CONTEXTIDR */
#define CONTEXTIDR_ASID_MASK             0xFF	/* [한국어] CONTEXTIDR_ASID 필드의 폭을 나타내는 마스크 — 이 컨텍스트의 ASID — TLB 태그가 된다. */
#define PROCID_MASK                      0x00FFFFFF	/* [한국어] PROCID 필드의 폭을 나타내는 마스크 — 소프트웨어가 쓰는 프로세스 식별자. */


/* FSR */
#define TF_MASK                          0x01	/* [한국어] TF 필드의 폭을 나타내는 마스크 — 변환 폴트 — 매핑이 없다. */
#define AFF_MASK                         0x01	/* [한국어] AFF 필드의 폭을 나타내는 마스크 — 접근 플래그 폴트. */
#define APF_MASK                         0x01	/* [한국어] APF 필드의 폭을 나타내는 마스크 — 권한 폴트. */
#define TLBMF_MASK                       0x01	/* [한국어] TLBMF 필드의 폭을 나타내는 마스크 — TLB 다중 적중. */
#define HTWDEEF_MASK                     0x01	/* [한국어] HTWDEEF 필드의 폭을 나타내는 마스크 — 테이블 워크 중 디코드 오류. */
#define HTWSEEF_MASK                     0x01	/* [한국어] HTWSEEF 필드의 폭을 나타내는 마스크 — 테이블 워크 중 슬레이브 오류. */
#define MHF_MASK                         0x01	/* [한국어] MHF 필드의 폭을 나타내는 마스크 — 다중 적중 폴트. */
#define SL_MASK                          0x01	/* [한국어] SL 필드의 폭을 나타내는 마스크 — 두 번째 단계에서 났는가. */
#define SS_MASK                          0x01	/* [한국어] SS 필드의 폭을 나타내는 마스크 — 트랜잭션이 멈춰 세워졌다. */
#define MULTI_MASK                       0x01	/* [한국어] MULTI 필드의 폭을 나타내는 마스크 — 폴트가 겹쳐서 났다. */


/* FSYNR0 */
#define AMID_MASK                        0xFF	/* [한국어] AMID 필드의 폭을 나타내는 마스크 — 폴트를 낸 마스터 ID. */
#define APID_MASK                        0x1F	/* [한국어] APID 필드의 폭을 나타내는 마스크 — 폴트를 낸 페이지 ID. */
#define ABID_MASK                        0x07	/* [한국어] ABID 필드의 폭을 나타내는 마스크 — 폴트를 낸 버스 ID. */
#define ATID_MASK                        0xFF	/* [한국어] ATID 필드의 폭을 나타내는 마스크 — 폴트를 낸 트랜잭션 ID. */


/* FSYNR1 */
#define AMEMTYPE_MASK                    0x07	/* [한국어] AMEMTYPE 필드의 폭을 나타내는 마스크 — 그 트랜잭션의 메모리 타입. */
#define ASHARED_MASK                     0x01	/* [한국어] ASHARED 필드의 폭을 나타내는 마스크 — 공유 접근이었는가. */
#define AINNERSHARED_MASK                0x01	/* [한국어] AINNERSHARED 필드의 폭을 나타내는 마스크 — 내부 공유 접근이었는가. */
#define APRIV_MASK                       0x01	/* [한국어] APRIV 필드의 폭을 나타내는 마스크 — 특권 접근이었는가. */
#define APROTNS_MASK                     0x01	/* [한국어] APROTNS 필드의 폭을 나타내는 마스크 — 비보안 접근이었는가. */
#define AINST_MASK                       0x01	/* [한국어] AINST 필드의 폭을 나타내는 마스크 — 명령 인출이었는가. */
#define AWRITE_MASK                      0x01	/* [한국어] AWRITE 필드의 폭을 나타내는 마스크 — 쓰기였는가. */
#define ABURST_MASK                      0x01	/* [한국어] ABURST 필드의 폭을 나타내는 마스크 — 버스트 종류. */
#define ALEN_MASK                        0x0F	/* [한국어] ALEN 필드의 폭을 나타내는 마스크 — 버스트 길이. */
#define FSYNR1_ASIZE_MASK                0x07	/* [한국어] FSYNR1_ASIZE 필드의 폭을 나타내는 마스크 — 전송 크기. */
#define ALOCK_MASK                       0x03	/* [한국어] ALOCK 필드의 폭을 나타내는 마스크 — 잠금 전송이었는가. */
#define AFULL_MASK                       0x01	/* [한국어] AFULL 필드의 폭을 나타내는 마스크 — 버퍼가 가득 찼는가. */


/* NMRR */
#define ICPC0_MASK                       0x03	/* [한국어] ICPC0 필드의 폭을 나타내는 마스크 — TEX 클래스 0의 내부 캐시 정책. */
#define ICPC1_MASK                       0x03	/* [한국어] ICPC1 필드의 폭을 나타내는 마스크 — TEX 클래스 1의 내부 캐시 정책. */
#define ICPC2_MASK                       0x03	/* [한국어] ICPC2 필드의 폭을 나타내는 마스크 — TEX 클래스 2의 내부 캐시 정책. */
#define ICPC3_MASK                       0x03	/* [한국어] ICPC3 필드의 폭을 나타내는 마스크 — TEX 클래스 3의 내부 캐시 정책. */
#define ICPC4_MASK                       0x03	/* [한국어] ICPC4 필드의 폭을 나타내는 마스크 — TEX 클래스 4의 내부 캐시 정책. */
#define ICPC5_MASK                       0x03	/* [한국어] ICPC5 필드의 폭을 나타내는 마스크 — TEX 클래스 5의 내부 캐시 정책. */
#define ICPC6_MASK                       0x03	/* [한국어] ICPC6 필드의 폭을 나타내는 마스크 — TEX 클래스 6의 내부 캐시 정책. */
#define ICPC7_MASK                       0x03	/* [한국어] ICPC7 필드의 폭을 나타내는 마스크 — TEX 클래스 7의 내부 캐시 정책. */
#define OCPC0_MASK                       0x03	/* [한국어] OCPC0 필드의 폭을 나타내는 마스크 — TEX 클래스 0의 외부 캐시 정책. */
#define OCPC1_MASK                       0x03	/* [한국어] OCPC1 필드의 폭을 나타내는 마스크 — TEX 클래스 1의 외부 캐시 정책. */
#define OCPC2_MASK                       0x03	/* [한국어] OCPC2 필드의 폭을 나타내는 마스크 — TEX 클래스 2의 외부 캐시 정책. */
#define OCPC3_MASK                       0x03	/* [한국어] OCPC3 필드의 폭을 나타내는 마스크 — TEX 클래스 3의 외부 캐시 정책. */
#define OCPC4_MASK                       0x03	/* [한국어] OCPC4 필드의 폭을 나타내는 마스크 — TEX 클래스 4의 외부 캐시 정책. */
#define OCPC5_MASK                       0x03	/* [한국어] OCPC5 필드의 폭을 나타내는 마스크 — TEX 클래스 5의 외부 캐시 정책. */
#define OCPC6_MASK                       0x03	/* [한국어] OCPC6 필드의 폭을 나타내는 마스크 — TEX 클래스 6의 외부 캐시 정책. */
#define OCPC7_MASK                       0x03	/* [한국어] OCPC7 필드의 폭을 나타내는 마스크 — TEX 클래스 7의 외부 캐시 정책. */


/* PAR */
#define FAULT_MASK                       0x01	/* [한국어] FAULT 필드의 폭을 나타내는 마스크 — 변환이 실패했는가 — 이 비트로 아래 두 해석이 갈린다. */
/* If a fault is present, these are the
same as the fault fields in the FAR */
#define FAULT_TF_MASK                    0x01	/* [한국어] FAULT_TF 필드의 폭을 나타내는 마스크 — 변환 폴트였다. */
#define FAULT_AFF_MASK                   0x01	/* [한국어] FAULT_AFF 필드의 폭을 나타내는 마스크 — 접근 플래그 폴트였다. */
#define FAULT_APF_MASK                   0x01	/* [한국어] FAULT_APF 필드의 폭을 나타내는 마스크 — 권한 폴트였다. */
#define FAULT_TLBMF_MASK                 0x01	/* [한국어] FAULT_TLBMF 필드의 폭을 나타내는 마스크 — TLB 다중 적중이었다. */
#define FAULT_HTWDEEF_MASK               0x01	/* [한국어] FAULT_HTWDEEF 필드의 폭을 나타내는 마스크 — 워크 중 디코드 오류였다. */
#define FAULT_HTWSEEF_MASK               0x01	/* [한국어] FAULT_HTWSEEF 필드의 폭을 나타내는 마스크 — 워크 중 슬레이브 오류였다. */
#define FAULT_MHF_MASK                   0x01	/* [한국어] FAULT_MHF 필드의 폭을 나타내는 마스크 — 다중 적중 폴트였다. */
#define FAULT_SL_MASK                    0x01	/* [한국어] FAULT_SL 필드의 폭을 나타내는 마스크 — 두 번째 단계에서 났다. */
#define FAULT_SS_MASK                    0x01	/* [한국어] FAULT_SS 필드의 폭을 나타내는 마스크 — 멈춰 세워졌다. */

/* If NO fault is present, the following
 * fields are in effect
 * (FAULT remains as before) */
#define PAR_NOFAULT_SS_MASK              0x01	/* [한국어] PAR_NOFAULT_SS 필드의 폭을 나타내는 마스크 — 성공 시 — 멈춤 상태. */
#define PAR_NOFAULT_MT_MASK              0x07	/* [한국어] PAR_NOFAULT_MT 필드의 폭을 나타내는 마스크 — 성공 시 — 메모리 타입. */
#define PAR_NOFAULT_SH_MASK              0x01	/* [한국어] PAR_NOFAULT_SH 필드의 폭을 나타내는 마스크 — 성공 시 — 공유 속성. */
#define PAR_NOFAULT_NS_MASK              0x01	/* [한국어] PAR_NOFAULT_NS 필드의 폭을 나타내는 마스크 — 성공 시 — 비보안 여부. */
#define PAR_NOFAULT_NOS_MASK             0x01	/* [한국어] PAR_NOFAULT_NOS 필드의 폭을 나타내는 마스크 — 성공 시 — 외부 공유 여부. */
#define PAR_NPFAULT_PA_MASK              0x000FFFFF	/* [한국어] PAR_NPFAULT_PA 필드의 폭을 나타내는 마스크 — 성공 시 — 변환된 물리 주소. */


/* PRRR */
#define MTC0_MASK                        0x03	/* [한국어] MTC0 필드의 폭을 나타내는 마스크 — TEX 클래스 0의 메모리 타입. */
#define MTC1_MASK                        0x03	/* [한국어] MTC1 필드의 폭을 나타내는 마스크 — TEX 클래스 1의 메모리 타입. */
#define MTC2_MASK                        0x03	/* [한국어] MTC2 필드의 폭을 나타내는 마스크 — TEX 클래스 2의 메모리 타입. */
#define MTC3_MASK                        0x03	/* [한국어] MTC3 필드의 폭을 나타내는 마스크 — TEX 클래스 3의 메모리 타입. */
#define MTC4_MASK                        0x03	/* [한국어] MTC4 필드의 폭을 나타내는 마스크 — TEX 클래스 4의 메모리 타입. */
#define MTC5_MASK                        0x03	/* [한국어] MTC5 필드의 폭을 나타내는 마스크 — TEX 클래스 5의 메모리 타입. */
#define MTC6_MASK                        0x03	/* [한국어] MTC6 필드의 폭을 나타내는 마스크 — TEX 클래스 6의 메모리 타입. */
#define MTC7_MASK                        0x03	/* [한국어] MTC7 필드의 폭을 나타내는 마스크 — TEX 클래스 7의 메모리 타입. */
#define SHDSH0_MASK                      0x01	/* [한국어] SHDSH0 필드의 폭을 나타내는 마스크 — 장치 메모리 0의 공유 속성. */
#define SHDSH1_MASK                      0x01	/* [한국어] SHDSH1 필드의 폭을 나타내는 마스크 — 장치 메모리 1의 공유 속성. */
#define SHNMSH0_MASK                     0x01	/* [한국어] SHNMSH0 필드의 폭을 나타내는 마스크 — 일반 메모리 0의 공유 속성. */
#define SHNMSH1_MASK                     0x01	/* [한국어] SHNMSH1 필드의 폭을 나타내는 마스크 — 일반 메모리 1의 공유 속성. */
#define NOS0_MASK                        0x01	/* [한국어] NOS0 필드의 폭을 나타내는 마스크 — TEX 클래스 0가 외부 공유인가. */
#define NOS1_MASK                        0x01	/* [한국어] NOS1 필드의 폭을 나타내는 마스크 — TEX 클래스 1가 외부 공유인가. */
#define NOS2_MASK                        0x01	/* [한국어] NOS2 필드의 폭을 나타내는 마스크 — TEX 클래스 2가 외부 공유인가. */
#define NOS3_MASK                        0x01	/* [한국어] NOS3 필드의 폭을 나타내는 마스크 — TEX 클래스 3가 외부 공유인가. */
#define NOS4_MASK                        0x01	/* [한국어] NOS4 필드의 폭을 나타내는 마스크 — TEX 클래스 4가 외부 공유인가. */
#define NOS5_MASK                        0x01	/* [한국어] NOS5 필드의 폭을 나타내는 마스크 — TEX 클래스 5가 외부 공유인가. */
#define NOS6_MASK                        0x01	/* [한국어] NOS6 필드의 폭을 나타내는 마스크 — TEX 클래스 6가 외부 공유인가. */
#define NOS7_MASK                        0x01	/* [한국어] NOS7 필드의 폭을 나타내는 마스크 — TEX 클래스 7가 외부 공유인가. */


/* RESUME */
#define TNR_MASK                         0x01	/* [한국어] TNR 필드의 폭을 나타내는 마스크 — 재개할지 종료할지를 고른다. */


/* SCTLR */
#define M_MASK                           0x01	/* [한국어] M 필드의 폭을 나타내는 마스크 — 이 컨텍스트의 MMU 활성화. */
#define TRE_MASK                         0x01	/* [한국어] TRE 필드의 폭을 나타내는 마스크 — TEX 재매핑 활성화. */
#define AFE_MASK                         0x01	/* [한국어] AFE 필드의 폭을 나타내는 마스크 — 접근 플래그 활성화. */
#define HAF_MASK                         0x01	/* [한국어] HAF 필드의 폭을 나타내는 마스크 — 하드웨어 접근 플래그 갱신. */
#define BE_MASK                          0x01	/* [한국어] BE 필드의 폭을 나타내는 마스크 — 빅엔디언 테이블. */
#define AFFD_MASK                        0x01	/* [한국어] AFFD 필드의 폭을 나타내는 마스크 — 접근 플래그 폴트 비활성화. */


/* TLBIASID */
#define TLBIASID_ASID_MASK               0xFF	/* [한국어] TLBIASID_ASID 필드의 폭을 나타내는 마스크 — 무효화할 ASID. */


/* TLBIVA */
#define TLBIVA_ASID_MASK                 0xFF	/* [한국어] TLBIVA_ASID 필드의 폭을 나타내는 마스크 — 무효화할 ASID. */
#define TLBIVA_VA_MASK                   0x000FFFFF	/* [한국어] TLBIVA_VA 필드의 폭을 나타내는 마스크 — 무효화할 가상 주소. */


/* TLBIVAA */
#define TLBIVAA_VA_MASK                  0x000FFFFF	/* [한국어] TLBIVAA_VA 필드의 폭을 나타내는 마스크 — 무효화할 가상 주소. */


/* TLBLCKR */
#define LKE_MASK                         0x01	/* [한국어] LKE 필드의 폭을 나타내는 마스크 — TLB 잠금 활성화. */
#define TLBLCKR_TLBIALLCFG_MASK          0x01	/* [한국어] TLBLCKR_TLBIALLCFG 필드의 폭을 나타내는 마스크 — 전체 무효화가 잠긴 항목도 지울지. */
#define TLBIASIDCFG_MASK                 0x01	/* [한국어] TLBIASIDCFG 필드의 폭을 나타내는 마스크 — ASID 무효화가 잠긴 항목도 지울지. */
#define TLBIVAACFG_MASK                  0x01	/* [한국어] TLBIVAACFG 필드의 폭을 나타내는 마스크 — 주소 무효화가 잠긴 항목도 지울지. */
#define FLOOR_MASK                       0xFF	/* [한국어] FLOOR 필드의 폭을 나타내는 마스크 — 잠긴 영역의 하한. */
#define VICTIM_MASK                      0xFF	/* [한국어] VICTIM 필드의 폭을 나타내는 마스크 — 다음에 교체할 항목. */


/* TTBCR */
#define N_MASK                           0x07	/* [한국어] N 필드의 폭을 나타내는 마스크 — TTBR0과 TTBR1의 경계를 정하는 비트 수. */
#define PD0_MASK                         0x01	/* [한국어] PD0 필드의 폭을 나타내는 마스크 — TTBR0 워크 비활성화. */
#define PD1_MASK                         0x01	/* [한국어] PD1 필드의 폭을 나타내는 마스크 — TTBR1 워크 비활성화. */


/* TTBR0 */
#define TTBR0_IRGNH_MASK                 0x01	/* [한국어] TTBR0_IRGNH 필드의 폭을 나타내는 마스크 — 내부 캐시 정책(상위 비트). */
#define TTBR0_SH_MASK                    0x01	/* [한국어] TTBR0_SH 필드의 폭을 나타내는 마스크 — 테이블 접근의 공유 속성. */
#define TTBR0_ORGN_MASK                  0x03	/* [한국어] TTBR0_ORGN 필드의 폭을 나타내는 마스크 — 외부 캐시 정책. */
#define TTBR0_NOS_MASK                   0x01	/* [한국어] TTBR0_NOS 필드의 폭을 나타내는 마스크 — 외부 공유 여부. */
#define TTBR0_IRGNL_MASK                 0x01	/* [한국어] TTBR0_IRGNL 필드의 폭을 나타내는 마스크 — 내부 캐시 정책(하위 비트). */
#define TTBR0_PA_MASK                    0x0003FFFF	/* [한국어] TTBR0_PA 필드의 폭을 나타내는 마스크 — 1단계 테이블의 물리 주소. */


/* TTBR1 */
#define TTBR1_IRGNH_MASK                 0x01	/* [한국어] TTBR1_IRGNH 필드의 폭을 나타내는 마스크 — 내부 캐시 정책(상위 비트). */
#define TTBR1_SH_MASK                    0x01	/* [한국어] TTBR1_SH 필드의 폭을 나타내는 마스크 — 테이블 접근의 공유 속성. */
#define TTBR1_ORGN_MASK                  0x03	/* [한국어] TTBR1_ORGN 필드의 폭을 나타내는 마스크 — 외부 캐시 정책. */
#define TTBR1_NOS_MASK                   0x01	/* [한국어] TTBR1_NOS 필드의 폭을 나타내는 마스크 — 외부 공유 여부. */
#define TTBR1_IRGNL_MASK                 0x01	/* [한국어] TTBR1_IRGNL 필드의 폭을 나타내는 마스크 — 내부 캐시 정책(하위 비트). */
#define TTBR1_PA_MASK                    0x0003FFFF	/* [한국어] TTBR1_PA 필드의 폭을 나타내는 마스크 — 1단계 테이블의 물리 주소. */


/* V2PSR */
#define HIT_MASK                         0x01	/* [한국어] HIT 필드의 폭을 나타내는 마스크 — 변환에 성공했는가. */
#define INDEX_MASK                       0xFF	/* [한국어] INDEX 필드의 폭을 나타내는 마스크 — 적중한 TLB 항목의 번호. */


/* V2Pxx */
#define V2Pxx_INDEX_MASK                 0xFF	/* [한국어] V2Pxx_INDEX 필드의 폭을 나타내는 마스크 — 요청 식별 번호. */
#define V2Pxx_VA_MASK                    0x000FFFFF	/* [한국어] V2Pxx_VA 필드의 폭을 나타내는 마스크 — 변환을 요청할 가상 주소. */


/* Context Register Shifts */
/* ACTLR */
#define CFERE_SHIFT                    0	/* [한국어] CFERE 필드가 시작하는 비트 위치 — 컨텍스트 폴트 시 오류 응답 활성화. */
#define CFEIE_SHIFT                    1	/* [한국어] CFEIE 필드가 시작하는 비트 위치 — 컨텍스트 폴트 인터럽트 활성화. */
#define PTSHCFG_SHIFT                  2	/* [한국어] PTSHCFG 필드가 시작하는 비트 위치 — 페이지 테이블 접근의 공유 속성. */
#define RCOSH_SHIFT                    4	/* [한국어] RCOSH 필드가 시작하는 비트 위치 — 외부 공유 읽기 정책. */
#define RCISH_SHIFT                    5	/* [한국어] RCISH 필드가 시작하는 비트 위치 — 내부 공유 읽기 정책. */
#define RCNSH_SHIFT                    6	/* [한국어] RCNSH 필드가 시작하는 비트 위치 — 비공유 읽기 정책. */
#define PRIVCFG_SHIFT                  8	/* [한국어] PRIVCFG 필드가 시작하는 비트 위치 — 특권 속성 설정. */
#define DNA_SHIFT                      10	/* [한국어] DNA 필드가 시작하는 비트 위치 — 접근 플래그 갱신 금지. */
#define DNLV2PA_SHIFT                  11	/* [한국어] DNLV2PA 필드가 시작하는 비트 위치 — 2단계 물리 주소 갱신 금지. */
#define TLBMCFG_SHIFT                  12	/* [한국어] TLBMCFG 필드가 시작하는 비트 위치 — TLB 다중 적중 처리 설정. */
#define CFCFG_SHIFT                    14	/* [한국어] CFCFG 필드가 시작하는 비트 위치 — 폴트 시 멈춰 세울지 종료할지. */
#define TIPCF_SHIFT                    15	/* [한국어] TIPCF 필드가 시작하는 비트 위치 — 변환 진행 중 폴트 설정. */
#define V2PCFG_SHIFT                   16	/* [한국어] V2PCFG 필드가 시작하는 비트 위치 — 가상→물리 요청의 동작 설정. */
#define HUME_SHIFT                     18	/* [한국어] HUME 필드가 시작하는 비트 위치 — 하드웨어 업데이트 확장 활성화. */
#define PTMTCFG_SHIFT                  20	/* [한국어] PTMTCFG 필드가 시작하는 비트 위치 — 테이블 접근의 메모리 타입 덮어쓰기. */
#define PTMEMTYPE_SHIFT                21	/* [한국어] PTMEMTYPE 필드가 시작하는 비트 위치 — 테이블 접근에 강제할 메모리 타입. */


/* BFBCR */
#define BFBDFE_SHIFT                   0	/* [한국어] BFBDFE 필드가 시작하는 비트 위치 — 분기 예측 데이터 인출 활성화. */
#define BFBSFE_SHIFT                   1	/* [한국어] BFBSFE 필드가 시작하는 비트 위치 — 분기 예측 추측 인출 활성화. */
#define SFVS_SHIFT                     2	/* [한국어] SFVS 필드가 시작하는 비트 위치 — 추측 인출의 유효 크기. */
#define FLVIC_SHIFT                    4	/* [한국어] FLVIC 필드가 시작하는 비트 위치 — 1단계 예측 항목 수. */
#define SLVIC_SHIFT                    8	/* [한국어] SLVIC 필드가 시작하는 비트 위치 — 2단계 예측 항목 수. */


/* CONTEXTIDR */
#define CONTEXTIDR_ASID_SHIFT          0	/* [한국어] CONTEXTIDR_ASID 필드가 시작하는 비트 위치 — 이 컨텍스트의 ASID — TLB 태그가 된다. */
#define PROCID_SHIFT                   8	/* [한국어] PROCID 필드가 시작하는 비트 위치 — 소프트웨어가 쓰는 프로세스 식별자. */


/* FSR */
#define TF_SHIFT                       1	/* [한국어] TF 필드가 시작하는 비트 위치 — 변환 폴트 — 매핑이 없다. */
#define AFF_SHIFT                      2	/* [한국어] AFF 필드가 시작하는 비트 위치 — 접근 플래그 폴트. */
#define APF_SHIFT                      3	/* [한국어] APF 필드가 시작하는 비트 위치 — 권한 폴트. */
#define TLBMF_SHIFT                    4	/* [한국어] TLBMF 필드가 시작하는 비트 위치 — TLB 다중 적중. */
#define HTWDEEF_SHIFT                  5	/* [한국어] HTWDEEF 필드가 시작하는 비트 위치 — 테이블 워크 중 디코드 오류. */
#define HTWSEEF_SHIFT                  6	/* [한국어] HTWSEEF 필드가 시작하는 비트 위치 — 테이블 워크 중 슬레이브 오류. */
#define MHF_SHIFT                      7	/* [한국어] MHF 필드가 시작하는 비트 위치 — 다중 적중 폴트. */
#define SL_SHIFT                       16	/* [한국어] SL 필드가 시작하는 비트 위치 — 두 번째 단계에서 났는가. */
#define SS_SHIFT                       30	/* [한국어] SS 필드가 시작하는 비트 위치 — 트랜잭션이 멈춰 세워졌다. */
#define MULTI_SHIFT                    31	/* [한국어] MULTI 필드가 시작하는 비트 위치 — 폴트가 겹쳐서 났다. */


/* FSYNR0 */
#define AMID_SHIFT                     0	/* [한국어] AMID 필드가 시작하는 비트 위치 — 폴트를 낸 마스터 ID. */
#define APID_SHIFT                     8	/* [한국어] APID 필드가 시작하는 비트 위치 — 폴트를 낸 페이지 ID. */
#define ABID_SHIFT                     13	/* [한국어] ABID 필드가 시작하는 비트 위치 — 폴트를 낸 버스 ID. */
#define ATID_SHIFT                     24	/* [한국어] ATID 필드가 시작하는 비트 위치 — 폴트를 낸 트랜잭션 ID. */


/* FSYNR1 */
#define AMEMTYPE_SHIFT                 0	/* [한국어] AMEMTYPE 필드가 시작하는 비트 위치 — 그 트랜잭션의 메모리 타입. */
#define ASHARED_SHIFT                  3	/* [한국어] ASHARED 필드가 시작하는 비트 위치 — 공유 접근이었는가. */
#define AINNERSHARED_SHIFT             4	/* [한국어] AINNERSHARED 필드가 시작하는 비트 위치 — 내부 공유 접근이었는가. */
#define APRIV_SHIFT                    5	/* [한국어] APRIV 필드가 시작하는 비트 위치 — 특권 접근이었는가. */
#define APROTNS_SHIFT                  6	/* [한국어] APROTNS 필드가 시작하는 비트 위치 — 비보안 접근이었는가. */
#define AINST_SHIFT                    7	/* [한국어] AINST 필드가 시작하는 비트 위치 — 명령 인출이었는가. */
#define AWRITE_SHIFT                   8	/* [한국어] AWRITE 필드가 시작하는 비트 위치 — 쓰기였는가. */
#define ABURST_SHIFT                   10	/* [한국어] ABURST 필드가 시작하는 비트 위치 — 버스트 종류. */
#define ALEN_SHIFT                     12	/* [한국어] ALEN 필드가 시작하는 비트 위치 — 버스트 길이. */
#define FSYNR1_ASIZE_SHIFT             16	/* [한국어] FSYNR1_ASIZE 필드가 시작하는 비트 위치 — 전송 크기. */
#define ALOCK_SHIFT                    20	/* [한국어] ALOCK 필드가 시작하는 비트 위치 — 잠금 전송이었는가. */
#define AFULL_SHIFT                    24	/* [한국어] AFULL 필드가 시작하는 비트 위치 — 버퍼가 가득 찼는가. */


/* NMRR */
#define ICPC0_SHIFT                    0	/* [한국어] ICPC0 필드가 시작하는 비트 위치 — TEX 클래스 0의 내부 캐시 정책. */
#define ICPC1_SHIFT                    2	/* [한국어] ICPC1 필드가 시작하는 비트 위치 — TEX 클래스 1의 내부 캐시 정책. */
#define ICPC2_SHIFT                    4	/* [한국어] ICPC2 필드가 시작하는 비트 위치 — TEX 클래스 2의 내부 캐시 정책. */
#define ICPC3_SHIFT                    6	/* [한국어] ICPC3 필드가 시작하는 비트 위치 — TEX 클래스 3의 내부 캐시 정책. */
#define ICPC4_SHIFT                    8	/* [한국어] ICPC4 필드가 시작하는 비트 위치 — TEX 클래스 4의 내부 캐시 정책. */
#define ICPC5_SHIFT                    10	/* [한국어] ICPC5 필드가 시작하는 비트 위치 — TEX 클래스 5의 내부 캐시 정책. */
#define ICPC6_SHIFT                    12	/* [한국어] ICPC6 필드가 시작하는 비트 위치 — TEX 클래스 6의 내부 캐시 정책. */
#define ICPC7_SHIFT                    14	/* [한국어] ICPC7 필드가 시작하는 비트 위치 — TEX 클래스 7의 내부 캐시 정책. */
#define OCPC0_SHIFT                    16	/* [한국어] OCPC0 필드가 시작하는 비트 위치 — TEX 클래스 0의 외부 캐시 정책. */
#define OCPC1_SHIFT                    18	/* [한국어] OCPC1 필드가 시작하는 비트 위치 — TEX 클래스 1의 외부 캐시 정책. */
#define OCPC2_SHIFT                    20	/* [한국어] OCPC2 필드가 시작하는 비트 위치 — TEX 클래스 2의 외부 캐시 정책. */
#define OCPC3_SHIFT                    22	/* [한국어] OCPC3 필드가 시작하는 비트 위치 — TEX 클래스 3의 외부 캐시 정책. */
#define OCPC4_SHIFT                    24	/* [한국어] OCPC4 필드가 시작하는 비트 위치 — TEX 클래스 4의 외부 캐시 정책. */
#define OCPC5_SHIFT                    26	/* [한국어] OCPC5 필드가 시작하는 비트 위치 — TEX 클래스 5의 외부 캐시 정책. */
#define OCPC6_SHIFT                    28	/* [한국어] OCPC6 필드가 시작하는 비트 위치 — TEX 클래스 6의 외부 캐시 정책. */
#define OCPC7_SHIFT                    30	/* [한국어] OCPC7 필드가 시작하는 비트 위치 — TEX 클래스 7의 외부 캐시 정책. */


/* PAR */
#define FAULT_SHIFT                    0	/* [한국어] FAULT 필드가 시작하는 비트 위치 — 변환이 실패했는가 — 이 비트로 아래 두 해석이 갈린다. */
/* If a fault is present, these are the
same as the fault fields in the FAR */
#define FAULT_TF_SHIFT                 1	/* [한국어] FAULT_TF 필드가 시작하는 비트 위치 — 변환 폴트였다. */
#define FAULT_AFF_SHIFT                2	/* [한국어] FAULT_AFF 필드가 시작하는 비트 위치 — 접근 플래그 폴트였다. */
#define FAULT_APF_SHIFT                3	/* [한국어] FAULT_APF 필드가 시작하는 비트 위치 — 권한 폴트였다. */
#define FAULT_TLBMF_SHIFT              4	/* [한국어] FAULT_TLBMF 필드가 시작하는 비트 위치 — TLB 다중 적중이었다. */
#define FAULT_HTWDEEF_SHIFT            5	/* [한국어] FAULT_HTWDEEF 필드가 시작하는 비트 위치 — 워크 중 디코드 오류였다. */
#define FAULT_HTWSEEF_SHIFT            6	/* [한국어] FAULT_HTWSEEF 필드가 시작하는 비트 위치 — 워크 중 슬레이브 오류였다. */
#define FAULT_MHF_SHIFT                7	/* [한국어] FAULT_MHF 필드가 시작하는 비트 위치 — 다중 적중 폴트였다. */
#define FAULT_SL_SHIFT                 16	/* [한국어] FAULT_SL 필드가 시작하는 비트 위치 — 두 번째 단계에서 났다. */
#define FAULT_SS_SHIFT                 30	/* [한국어] FAULT_SS 필드가 시작하는 비트 위치 — 멈춰 세워졌다. */

/* If NO fault is present, the following
 * fields are in effect
 * (FAULT remains as before) */
#define PAR_NOFAULT_SS_SHIFT           1	/* [한국어] PAR_NOFAULT_SS 필드가 시작하는 비트 위치 — 성공 시 — 멈춤 상태. */
#define PAR_NOFAULT_MT_SHIFT           4	/* [한국어] PAR_NOFAULT_MT 필드가 시작하는 비트 위치 — 성공 시 — 메모리 타입. */
#define PAR_NOFAULT_SH_SHIFT           7	/* [한국어] PAR_NOFAULT_SH 필드가 시작하는 비트 위치 — 성공 시 — 공유 속성. */
#define PAR_NOFAULT_NS_SHIFT           9	/* [한국어] PAR_NOFAULT_NS 필드가 시작하는 비트 위치 — 성공 시 — 비보안 여부. */
#define PAR_NOFAULT_NOS_SHIFT          10	/* [한국어] PAR_NOFAULT_NOS 필드가 시작하는 비트 위치 — 성공 시 — 외부 공유 여부. */
#define PAR_NPFAULT_PA_SHIFT           12	/* [한국어] PAR_NPFAULT_PA 필드가 시작하는 비트 위치 — 성공 시 — 변환된 물리 주소. */


/* PRRR */
#define MTC0_SHIFT                     0	/* [한국어] MTC0 필드가 시작하는 비트 위치 — TEX 클래스 0의 메모리 타입. */
#define MTC1_SHIFT                     2	/* [한국어] MTC1 필드가 시작하는 비트 위치 — TEX 클래스 1의 메모리 타입. */
#define MTC2_SHIFT                     4	/* [한국어] MTC2 필드가 시작하는 비트 위치 — TEX 클래스 2의 메모리 타입. */
#define MTC3_SHIFT                     6	/* [한국어] MTC3 필드가 시작하는 비트 위치 — TEX 클래스 3의 메모리 타입. */
#define MTC4_SHIFT                     8	/* [한국어] MTC4 필드가 시작하는 비트 위치 — TEX 클래스 4의 메모리 타입. */
#define MTC5_SHIFT                     10	/* [한국어] MTC5 필드가 시작하는 비트 위치 — TEX 클래스 5의 메모리 타입. */
#define MTC6_SHIFT                     12	/* [한국어] MTC6 필드가 시작하는 비트 위치 — TEX 클래스 6의 메모리 타입. */
#define MTC7_SHIFT                     14	/* [한국어] MTC7 필드가 시작하는 비트 위치 — TEX 클래스 7의 메모리 타입. */
#define SHDSH0_SHIFT                   16	/* [한국어] SHDSH0 필드가 시작하는 비트 위치 — 장치 메모리 0의 공유 속성. */
#define SHDSH1_SHIFT                   17	/* [한국어] SHDSH1 필드가 시작하는 비트 위치 — 장치 메모리 1의 공유 속성. */
#define SHNMSH0_SHIFT                  18	/* [한국어] SHNMSH0 필드가 시작하는 비트 위치 — 일반 메모리 0의 공유 속성. */
#define SHNMSH1_SHIFT                  19	/* [한국어] SHNMSH1 필드가 시작하는 비트 위치 — 일반 메모리 1의 공유 속성. */
#define NOS0_SHIFT                     24	/* [한국어] NOS0 필드가 시작하는 비트 위치 — TEX 클래스 0가 외부 공유인가. */
#define NOS1_SHIFT                     25	/* [한국어] NOS1 필드가 시작하는 비트 위치 — TEX 클래스 1가 외부 공유인가. */
#define NOS2_SHIFT                     26	/* [한국어] NOS2 필드가 시작하는 비트 위치 — TEX 클래스 2가 외부 공유인가. */
#define NOS3_SHIFT                     27	/* [한국어] NOS3 필드가 시작하는 비트 위치 — TEX 클래스 3가 외부 공유인가. */
#define NOS4_SHIFT                     28	/* [한국어] NOS4 필드가 시작하는 비트 위치 — TEX 클래스 4가 외부 공유인가. */
#define NOS5_SHIFT                     29	/* [한국어] NOS5 필드가 시작하는 비트 위치 — TEX 클래스 5가 외부 공유인가. */
#define NOS6_SHIFT                     30	/* [한국어] NOS6 필드가 시작하는 비트 위치 — TEX 클래스 6가 외부 공유인가. */
#define NOS7_SHIFT                     31	/* [한국어] NOS7 필드가 시작하는 비트 위치 — TEX 클래스 7가 외부 공유인가. */


/* RESUME */
#define TNR_SHIFT                      0	/* [한국어] TNR 필드가 시작하는 비트 위치 — 재개할지 종료할지를 고른다. */


/* SCTLR */
#define M_SHIFT                        0	/* [한국어] M 필드가 시작하는 비트 위치 — 이 컨텍스트의 MMU 활성화. */
#define TRE_SHIFT                      1	/* [한국어] TRE 필드가 시작하는 비트 위치 — TEX 재매핑 활성화. */
#define AFE_SHIFT                      2	/* [한국어] AFE 필드가 시작하는 비트 위치 — 접근 플래그 활성화. */
#define HAF_SHIFT                      3	/* [한국어] HAF 필드가 시작하는 비트 위치 — 하드웨어 접근 플래그 갱신. */
#define BE_SHIFT                       4	/* [한국어] BE 필드가 시작하는 비트 위치 — 빅엔디언 테이블. */
#define AFFD_SHIFT                     5	/* [한국어] AFFD 필드가 시작하는 비트 위치 — 접근 플래그 폴트 비활성화. */


/* TLBIASID */
#define TLBIASID_ASID_SHIFT            0	/* [한국어] TLBIASID_ASID 필드가 시작하는 비트 위치 — 무효화할 ASID. */


/* TLBIVA */
#define TLBIVA_ASID_SHIFT              0	/* [한국어] TLBIVA_ASID 필드가 시작하는 비트 위치 — 무효화할 ASID. */
#define TLBIVA_VA_SHIFT                12	/* [한국어] TLBIVA_VA 필드가 시작하는 비트 위치 — 무효화할 가상 주소. */


/* TLBIVAA */
#define TLBIVAA_VA_SHIFT               12	/* [한국어] TLBIVAA_VA 필드가 시작하는 비트 위치 — 무효화할 가상 주소. */


/* TLBLCKR */
#define LKE_SHIFT                      0	/* [한국어] LKE 필드가 시작하는 비트 위치 — TLB 잠금 활성화. */
#define TLBLCKR_TLBIALLCFG_SHIFT       1	/* [한국어] TLBLCKR_TLBIALLCFG 필드가 시작하는 비트 위치 — 전체 무효화가 잠긴 항목도 지울지. */
#define TLBIASIDCFG_SHIFT              2	/* [한국어] TLBIASIDCFG 필드가 시작하는 비트 위치 — ASID 무효화가 잠긴 항목도 지울지. */
#define TLBIVAACFG_SHIFT               3	/* [한국어] TLBIVAACFG 필드가 시작하는 비트 위치 — 주소 무효화가 잠긴 항목도 지울지. */
#define FLOOR_SHIFT                    8	/* [한국어] FLOOR 필드가 시작하는 비트 위치 — 잠긴 영역의 하한. */
#define VICTIM_SHIFT                   8	/* [한국어] VICTIM 필드가 시작하는 비트 위치 — 다음에 교체할 항목. */


/* TTBCR */
#define N_SHIFT                        3	/* [한국어] N 필드가 시작하는 비트 위치 — TTBR0과 TTBR1의 경계를 정하는 비트 수. */
#define PD0_SHIFT                      4	/* [한국어] PD0 필드가 시작하는 비트 위치 — TTBR0 워크 비활성화. */
#define PD1_SHIFT                      5	/* [한국어] PD1 필드가 시작하는 비트 위치 — TTBR1 워크 비활성화. */


/* TTBR0 */
#define TTBR0_IRGNH_SHIFT              0	/* [한국어] TTBR0_IRGNH 필드가 시작하는 비트 위치 — 내부 캐시 정책(상위 비트). */
#define TTBR0_SH_SHIFT                 1	/* [한국어] TTBR0_SH 필드가 시작하는 비트 위치 — 테이블 접근의 공유 속성. */
#define TTBR0_ORGN_SHIFT               3	/* [한국어] TTBR0_ORGN 필드가 시작하는 비트 위치 — 외부 캐시 정책. */
#define TTBR0_NOS_SHIFT                5	/* [한국어] TTBR0_NOS 필드가 시작하는 비트 위치 — 외부 공유 여부. */
#define TTBR0_IRGNL_SHIFT              6	/* [한국어] TTBR0_IRGNL 필드가 시작하는 비트 위치 — 내부 캐시 정책(하위 비트). */
#define TTBR0_PA_SHIFT                 14	/* [한국어] TTBR0_PA 필드가 시작하는 비트 위치 — 1단계 테이블의 물리 주소. */


/* TTBR1 */
#define TTBR1_IRGNH_SHIFT              0	/* [한국어] TTBR1_IRGNH 필드가 시작하는 비트 위치 — 내부 캐시 정책(상위 비트). */
#define TTBR1_SH_SHIFT                 1	/* [한국어] TTBR1_SH 필드가 시작하는 비트 위치 — 테이블 접근의 공유 속성. */
#define TTBR1_ORGN_SHIFT               3	/* [한국어] TTBR1_ORGN 필드가 시작하는 비트 위치 — 외부 캐시 정책. */
#define TTBR1_NOS_SHIFT                5	/* [한국어] TTBR1_NOS 필드가 시작하는 비트 위치 — 외부 공유 여부. */
#define TTBR1_IRGNL_SHIFT              6	/* [한국어] TTBR1_IRGNL 필드가 시작하는 비트 위치 — 내부 캐시 정책(하위 비트). */
#define TTBR1_PA_SHIFT                 14	/* [한국어] TTBR1_PA 필드가 시작하는 비트 위치 — 1단계 테이블의 물리 주소. */


/* V2PSR */
#define HIT_SHIFT                      0	/* [한국어] HIT 필드가 시작하는 비트 위치 — 변환에 성공했는가. */
#define INDEX_SHIFT                    8	/* [한국어] INDEX 필드가 시작하는 비트 위치 — 적중한 TLB 항목의 번호. */


/* V2Pxx */
#define V2Pxx_INDEX_SHIFT              0	/* [한국어] V2Pxx_INDEX 필드가 시작하는 비트 위치 — 요청 식별 번호. */
#define V2Pxx_VA_SHIFT                 12	/* [한국어] V2Pxx_VA 필드가 시작하는 비트 위치 — 변환을 요청할 가상 주소. */

#endif
