/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pasid.h - PASID idr, table and entry header
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 */

/*
 * [한국어 설명] scalable 모드의 PASID 디렉터리·테이블·항목 정의 (intel/pasid.h)
 *
 * === 파일의 역할 ===
 * VT-d scalable 모드에서 번역이 실제로 시작되는 자료구조를 정의한다. 레거시
 * 모드에서는 컨텍스트 항목이 곧바로 페이지 테이블을 가리켰지만, scalable
 * 모드에서는 그 사이에 두 단계가 더 끼어든다: 컨텍스트 항목 → PASID 디렉터리
 * → PASID 테이블 → 항목 → 페이지 테이블.
 * 이 헤더는 그 디렉터리 항목(struct pasid_dir_entry, 64비트)과 테이블 항목
 * (struct pasid_entry, 64비트 × 8)의 비트 배치를 정의하고, 각 비트를 읽고
 * 쓰는 인라인 헬퍼(pasid_set_ 계열, pasid_pte_ 계열)를 제공한다.
 * 항목 하나가 512비트나 되는 이유는, 그 안에 페이지 테이블 주소뿐 아니라
 * 도메인 id, 주소 폭, 변환 종류(1단계/2단계/중첩/통과), 폴트 처리 여부,
 * 캐시 스누프 정책, 더티 추적 설정이 모두 들어가기 때문이다. 레거시 모드에서
 * 컨텍스트 항목 하나가 담던 것을 훨씬 세밀하게 쪼갠 셈이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * scalable 모드의 번역 사슬에서 중간 두 단계를 담당한다.
 *   루트 테이블(버스)  → 컨텍스트 테이블(devfn) → [PASID 디렉터리]
 *   → [PASID 테이블] → PASID 항목 → 페이지 테이블
 * 왜 이 두 단계가 생겼는가: 하나의 장치가 여러 주소 공간을 동시에 쓸 수 있게
 * 하기 위해서다. 요청에 실린 20비트 PASID 로 디렉터리와 테이블을 색인하면
 * 그 PASID 전용 항목에 닿고, 거기서 그 주소 공간의 페이지 테이블이 시작된다.
 * SVA(프로세스 주소 공간 공유), 중첩 변환(게스트 1단계 + 호스트 2단계),
 * PASID 단위 도메인 부착이 모두 이 구조 위에 세워져 있다.
 * 20비트를 한 단계로 색인하면 테이블이 너무 커지므로, 상위 14비트로 디렉터리를
 * 하위 6비트로 테이블을 색인하는 2단 구조를 쓴다(PASID_PDE_SHIFT=6).
 * 실행 컨텍스트: 커널 모듈. 항목을 세우는 것은 프로세스 컨텍스트지만,
 * 항목을 읽는 진단 경로는 폴트 인터럽트에서도 불린다.
 *
 * === 타 모듈과의 연결 ===
 * - pasid.c: 이 헤더가 정의한 비트를 실제로 조립해 항목을 세우고 내린다.
 *   intel_pasid_setup_first_level/second_level/pass_through/nested 가 그 진입점.
 * - iommu.c: 장치를 프로브할 때 intel_pasid_alloc_table() 로 이 장치의
 *   PASID 테이블을 만들고, 도메인을 붙일 때 위 setup 함수들을 부른다.
 * - iommu.h: struct pasid_table 을 device_domain_info 가 들고 있고,
 *   컨텍스트 항목의 context_set_sm_* 계열이 디렉터리를 가리키게 만든다.
 * - cache.c: 항목을 고친 뒤 PASID 캐시와 IOTLB 를 비우는 무효화를 보낸다.
 * - svm.c/nested.c: 각각 SVA 와 중첩 변환용 항목 설정을 이 헤더의 헬퍼로 만든다.
 * 데이터 흐름: 장치 프로브 → PASID 테이블 할당 → 컨텍스트 항목이 디렉터리를
 * 가리키게 설정 → 도메인 부착 시 해당 PASID 항목에 페이지 테이블 주소와
 * 정책을 기록 → 캐시 무효화 → 그때부터 그 PASID 의 DMA 가 번역된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct pasid_dir_entry: 디렉터리 항목. present 비트와 PASID 테이블의
 *   물리 주소를 담는다. 64비트 하나로 끝난다.
 * - struct pasid_entry: 테이블 항목. u64 여덟 개(val[0]~val[7])이며 필드가
 *   여러 워드에 흩어져 있다. 그래서 pasid_set_* 헬퍼가 워드 번호와 비트
 *   위치를 감춘다.
 * - PASID_ENTRY_PGTT_*: 이 항목의 변환 종류 — 1단계만, 2단계만, 중첩,
 *   통과. 이 값이 나머지 필드의 해석을 정한다.
 * - pasid_pde_is_present()/pasid_pte_is_present(): 디렉터리·테이블 항목이
 *   유효한지. 모든 조회의 첫 관문이다.
 * - get_pasid_table_from_pde(): 디렉터리 항목에서 테이블의 가상 주소로.
 * - pasid_set_page_snoop()/pasid_set_ssade()/pasid_set_pgsnp(): 캐시 스누프
 *   강제와 더티 추적처럼 도메인 정책을 항목에 반영하는 헬퍼들.
 */
#ifndef __INTEL_PASID_H	/* [한국어] 중복 포함 방지 */
#define __INTEL_PASID_H	/* [한국어] 같음 */

#define PASID_MAX			0x100000	/* [한국어] PASID 의 개수 상한 = 2^20. 필드 폭이 20비트다 */
#define PASID_PTE_MASK			0x3F	/* [한국어] 항목의 하위 플래그 비트들을 뽑는 마스크. 나머지는 주소다 */
#define PASID_PTE_PRESENT		1	/* [한국어] present 비트. 이 비트가 0 이면 그 PASID 는 설정되지 않은 것이다 */
#define PASID_PTE_FPD			2	/* [한국어] Fault Processing Disable. 이 항목에서 난 폴트를 보고하지 않는다 */
#define PDE_PFN_MASK			PAGE_MASK	/* [한국어] 디렉터리 항목에서 PASID 테이블의 물리 주소만 뽑는 마스크 */
#define PASID_PDE_SHIFT			6	/* [한국어] PASID 의 하위 몇 비트로 테이블 안을 색인할지. 6비트 = 테이블당 64개 항목 */
#define MAX_NR_PASID_BITS		20	/* [한국어] PASID 필드의 비트 폭 */
#define PASID_TBL_ENTRIES		BIT(PASID_PDE_SHIFT)	/* [한국어] 한 PASID 테이블의 항목 수 = 64. 항목 하나가 64바이트라 정확히 4KB 한 페이지다 */

#define is_pasid_enabled(entry)		(((entry)->lo >> 3) & 0x1)	/* [한국어] 컨텍스트 항목에서 PASID 사용 여부(비트 3)를 읽는다 */
#define get_pasid_dir_size(entry)	(1 << ((((entry)->lo >> 9) & 0x7) + 7))	/* [한국어] 컨텍스트 항목에 적힌 PASID 디렉터리의 크기. 필드 값 + 7 을 지수로 쓰므로 최소 128개 항목이다 */

#define PASID_FLAG_NESTED		BIT(1)	/* [한국어] 중첩 변환용 항목이라는 설정 플래그 */
#define PASID_FLAG_PAGE_SNOOP		BIT(2)	/* [한국어] 이 항목의 DMA 는 CPU 캐시를 스누프하도록 강제한다(force snooping) */
#define PASID_FLAG_PWSNP		BIT(3)	/* [한국어] 페이지 워크 자체도 스누프하게 한다 — 하드웨어가 페이지 테이블을 읽을 때 CPU 캐시를 본다 */

/*
 * The PASID_FLAG_FL5LP flag Indicates using 5-level paging for first-
 * level translation, otherwise, 4-level paging will be used.
 */
#define PASID_FLAG_FL5LP		BIT(1)	/* [한국어] 1단계 변환에 5레벨 페이지 테이블을 쓴다. 꺼져 있으면 4레벨이다 (위 영어 주석). NESTED 와 비트 위치가 같은 것은 둘이 서로 다른 설정 경로에서만 쓰여 겹칠 일이 없기 때문이다 */

/*
 * [한국어] struct pasid_dir_entry — PASID 디렉터리의 항목 하나
 *
 * PASID 20비트를 한 단계로 색인하면 항목이 백만 개가 되어 테이블만
 * 64MB 가 된다. 그래서 상위 14비트로 이 디렉터리를, 하위 6비트로 그 아래
 * PASID 테이블을 색인하는 2단 구조를 쓴다.
 *
 * 항목 하나는 64비트뿐이며 present 비트와 PASID 테이블의 물리 주소만 담는다.
 * 실제 번역 설정은 그 아래 테이블의 항목(struct pasid_entry)에 있다.
 *
 * 컨텍스트 항목의 address space root 가 이 디렉터리의 시작을 가리킨다 —
 * scalable 모드에서 DMA_RTADDR_SMT 비트가 컨텍스트 항목의 해석을 바꾼다는
 * 것이 바로 이 뜻이다.
 */
struct pasid_dir_entry {
	u64 val;
	/* [한국어] PASID 디렉터리 항목의 64비트. 하위 비트에 present(PASID_PTE_PRESENT)가,
	 * 나머지 상위 비트에 PASID 테이블의 물리 주소가 들어간다.
	 * 설정자: intel_pasid_get_entry() 가 그 PASID 의 테이블이 아직 없으면 한 페이지를
	 *   잡아 여기에 주소와 present 를 기록한다.
	 * 읽는 자: 하드웨어가 PASID 의 상위 14비트로 이 항목을 색인해 읽고, 커널 쪽은
	 *   get_pasid_table_from_pde() 로 테이블의 가상 주소를 얻는다.
	 * 값 범위: present 가 0 이면 그 PASID 구간의 테이블이 아직 없다는 뜻이다.
	 * 동기화: READ_ONCE 로 읽는다 — 하드웨어와 다른 CPU 가 동시에 볼 수 있어
	 *   컴파일러가 읽기를 쪼개거나 합치면 안 된다. */
};

/*
 * [한국어] struct pasid_entry — PASID 테이블의 항목 하나, 번역 설정의 실체
 *
 * u64 여덟 개, 512비트다. 레거시 모드에서 컨텍스트 항목 하나(128비트)가
 * 담던 것을 훨씬 세밀하게 쪼갠 결과이며, 다음이 모두 여기 들어간다.
 *   - 변환 종류(PGTT): 1단계만/2단계만/중첩/통과. 이 값이 나머지 필드의
 *     해석을 정한다.
 *   - 1단계와 2단계 페이지 테이블의 물리 주소(중첩이면 둘 다 유효하다).
 *   - 도메인 id, 주소 폭, 페이지 테이블 레벨.
 *   - 폴트 처리 여부(FPD), 캐시 스누프 정책(PGSNP/PWSNP), 더티 추적(SSADE).
 *
 * 필드가 여러 워드에 흩어져 있어 비트 위치를 손으로 다루면 틀리기 쉽다.
 * 그래서 pasid.c 가 pasid_set_* 헬퍼로 워드 번호와 비트 위치를 감춘다.
 *
 * 항목 하나가 64바이트이고 테이블당 64개(PASID_TBL_ENTRIES)이므로,
 * PASID 테이블 하나가 정확히 4KB 한 페이지다.
 */
struct pasid_entry {
	u64 val[8];
	/* [한국어] PASID 테이블 항목의 512비트. 필드가 여덟 워드에 흩어져 있다.
	 *   val[0] — present, FPD, PGTT(변환 종류), 1단계 페이지 테이블 주소.
	 *   val[1] — 도메인 id, 주소 폭 등.
	 *   val[2] — 2단계 페이지 테이블 주소(중첩이나 2단계 전용일 때).
	 *   나머지 — 스누프 정책, 더티 추적, 예약 필드.
	 * 설정자: pasid.c 의 pasid_set_* 헬퍼들. 워드 번호와 비트 위치를 감춰 준다.
	 * 읽는 자: 하드웨어가 매 번역마다 읽는다. 커널 쪽은 진단(debugfs, 폴트 덤프)과
	 *   항목을 내릴 때 읽는다.
	 * 값 범위: PGTT 값이 나머지 필드의 유효성을 정한다 — 통과 모드면 페이지 테이블
	 *   주소가 무시되고, 1단계 전용이면 2단계 주소 필드가 예약이다.
	 * 동기화: present 는 반드시 마지막에 세운다. 그 순간부터 하드웨어가 이 항목을
	 *   쓰기 때문이다. 항목을 고친 뒤에는 PASID 캐시 무효화가 뒤따라야 한다. */
};

#define PASID_ENTRY_PGTT_FL_ONLY	(1)	/* [한국어] PGTT(Page Table Translation type) — 1단계만 쓴다. SVA 와 일반 1단계 도메인 */
#define PASID_ENTRY_PGTT_SL_ONLY	(2)	/* [한국어] 2단계만 쓴다. 레거시와 같은 형식의 번역 */
#define PASID_ENTRY_PGTT_NESTED		(3)	/* [한국어] 중첩 — 게스트 1단계를 거쳐 호스트 2단계로 내려간다 */
#define PASID_ENTRY_PGTT_PT		(4)	/* [한국어] 통과 — 번역하지 않는다. 이 값이 항목의 나머지 필드 해석을 통째로 정한다 */

/* The representative of a PASID table */
struct pasid_table {
	void			*table;		/* pasid table pointer */
	/* [한국어] 이 장치의 PASID 디렉터리 시작 주소 (원 주석: pasid table pointer).
	 * 이름은 table 이지만 실제로는 디렉터리다 — 그 아래에 PASID 테이블들이 매달린다.
	 * 설정자: intel_pasid_alloc_table() 이 장치 프로브 때 잡는다.
	 * 읽는 자: intel_pasid_get_entry() 가 여기서 시작해 두 단계를 내려가고,
	 *   컨텍스트 항목의 address space root 에 이 주소의 물리 주소가 들어간다.
	 * 동기화: 프로브 때 한 번 쓰고 이후 읽기만 한다. */
	u32			max_pasid;	/* max pasid */
	/* [한국어] 이 테이블이 다룰 수 있는 최대 PASID 값 (원 주석: max pasid).
	 * 설정자: 유닛의 ecap_pss(PASID 필드 폭)와 장치의 능력 중 작은 쪽에서 정한다.
	 * 읽는 자: PASID 를 항목으로 색인하기 전의 범위 검사. 이 값을 넘는 PASID 를
	 *   요청하면 거절한다 — 디렉터리 밖을 읽게 되기 때문이다.
	 * 동기화: 프로브 때 한 번 쓴다. */
};

/* Get PRESENT bit of a PASID directory entry. */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_pde_is_present - 이 PASID 구간의 테이블이 존재하는지 본다
 *
 * @pde: 디렉터리 항목.
 * @return: true 면 그 아래 PASID 테이블이 있다.
 *
 * PASID 디렉터리는 필요할 때만 하위 테이블을 만든다 — 백만 개의 PASID 를
 * 위해 테이블을 미리 다 잡을 수는 없기 때문이다. 그래서 조회의 첫 관문이
 * 이 검사이고, 없으면 그 자리에서 한 페이지를 잡아 채운다.
 *
 * READ_ONCE 를 쓰는 이유: 하드웨어와 다른 CPU 가 동시에 같은 항목을 볼 수
 * 있어, 컴파일러가 읽기를 쪼개거나 캐시하면 안 된다.
 *
 * 실행 컨텍스트: 어디서든. 진단 경로는 폴트 인터럽트에서도 부른다.
 */
static inline bool pasid_pde_is_present(struct pasid_dir_entry *pde)
{
	return READ_ONCE(pde->val) & PASID_PTE_PRESENT;	/* [한국어] present 비트를 본다 */
}

/* Get PASID table from a PASID directory entry. */
/*
 * [한국어] (위 영어 주석에 이어)
 * get_pasid_table_from_pde - 디렉터리 항목에서 PASID 테이블의 가상 주소로
 *
 * @pde: 디렉터리 항목.
 * @return: 그 아래 PASID 테이블의 가상 주소, 없으면 NULL.
 *
 * 항목에 적힌 것은 물리 주소이므로 phys_to_virt 로 커널이 쓸 수 있는 주소로
 * 바꾼다. 하드웨어와 커널이 같은 표를 서로 다른 주소로 본다는 점이 여기서
 * 드러난다.
 *
 * present 를 먼저 확인하는 것이 중요하다 — 없는 항목의 주소 필드는 의미 없는
 * 값이라 그대로 변환하면 엉뚱한 커널 주소가 나온다.
 *
 * 실행 컨텍스트: 어디서든.
 */
static inline struct pasid_entry *
get_pasid_table_from_pde(struct pasid_dir_entry *pde)
{
	if (!pasid_pde_is_present(pde))	/* [한국어] 아직 테이블이 없으면 */
		return NULL;	/* [한국어] 주소 필드가 의미 없는 값이므로 변환하지 않는다 */

	return phys_to_virt(READ_ONCE(pde->val) & PDE_PFN_MASK);	/* [한국어] 물리 주소를 커널 가상 주소로. 하드웨어와 커널이 같은 표를 다른 주소로 본다 */
}

/* Get PRESENT bit of a PASID table entry. */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_pte_is_present - 이 PASID 의 항목이 설정되어 있는지 본다
 *
 * @pte: PASID 테이블 항목.
 * @return: true 면 이 PASID 로 오는 DMA 가 번역된다.
 *
 * scalable 모드에서 "이 PASID 가 쓸 수 있는가"를 정하는 한 비트다. 0 이면
 * 그 PASID 의 DMA 는 전부 폴트로 끝난다.
 *
 * 항목을 세울 때 이 비트를 마지막에 켜고, 내릴 때 가장 먼저 끄는 것이
 * 규칙이다 — 그 순간부터 하드웨어가 항목을 쓰기 때문이다.
 *
 * 실행 컨텍스트: 어디서든.
 */
static inline bool pasid_pte_is_present(struct pasid_entry *pte)
{
	return READ_ONCE(pte->val[0]) & PASID_PTE_PRESENT;	/* [한국어] 항목의 present 비트 */
}

/* Get FPD(Fault Processing Disable) bit of a PASID table entry */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_pte_is_fault_disabled - 이 항목의 폴트 보고가 꺼져 있는지 본다
 *
 * @pte: PASID 테이블 항목.
 * @return: true 면 이 항목에서 난 폴트를 하드웨어가 보고하지 않는다.
 *
 * FPD 를 켜 두는 경우: PASID 를 내리는 중이라 폴트가 쏟아질 것이 뻔한 구간
 * (pasid_clear_entry_with_fpd 가 만드는 상태)이다. 접근은 여전히 막히지만
 * 로그가 폴트로 뒤덮이지 않는다.
 *
 * 실행 컨텍스트: 어디서든. 진단과 항목 해제 경로에서 쓴다.
 */
static inline bool pasid_pte_is_fault_disabled(struct pasid_entry *pte)
{
	return READ_ONCE(pte->val[0]) & PASID_PTE_FPD;	/* [한국어] 폴트 보고를 끈 상태인지 */
}

/* Get PGTT field of a PASID table entry */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_pte_get_pgtt - 이 항목의 변환 종류를 읽는다
 *
 * @pte: PASID 테이블 항목.
 * @return: PASID_ENTRY_PGTT_* 중 하나.
 *
 * PGTT(Page Table Translation type)는 val[0] 의 비트 6~8 에 있으며, 이 값이
 * 항목의 나머지 필드를 어떻게 읽을지를 정한다.
 *   FL_ONLY — 1단계 페이지 테이블 주소만 유효.
 *   SL_ONLY — 2단계 주소만 유효.
 *   NESTED  — 둘 다 유효. 게스트 1단계를 거쳐 호스트 2단계로 내려간다.
 *   PT      — 통과. 주소 필드를 하드웨어가 무시한다.
 *
 * 항목을 내릴 때 이 값을 먼저 읽는 이유: 종류에 따라 비워야 할 캐시가
 * 다르다. 1단계 전용이면 PASID 캐시와 IOTLB 를, 중첩이면 2단계 쪽까지
 * 함께 비워야 한다.
 *
 * 실행 컨텍스트: 어디서든.
 */
static inline u16 pasid_pte_get_pgtt(struct pasid_entry *pte)
{
	return (u16)((READ_ONCE(pte->val[0]) >> 6) & 0x7);	/* [한국어] 비트 6~8 의 변환 종류. 이 값이 나머지 필드의 해석을 정한다 */
}

/*
 * [한국어]
 * pasid_clear_entry - PASID 항목을 완전히 비운다
 *
 * @pe: 비울 항목.
 * @return: 없음.
 *
 * 여덟 워드를 모두 0 으로 만든다. val[0] 이 0 이 되면서 present 도 꺼지므로,
 * 이 뒤로 그 PASID 의 DMA 는 전부 폴트로 끝난다.
 *
 * 워드를 하나씩 WRITE_ONCE 로 쓰는 이유: memset 을 쓰면 컴파일러가 바이트
 * 단위나 벡터 명령으로 쪼갤 수 있는데, 하드웨어가 동시에 이 항목을 읽고
 * 있으므로 각 워드가 한 번의 온전한 64비트 쓰기여야 한다.
 *
 * 이 함수만으로는 하드웨어에 반영되지 않는다. PASID 캐시와 IOTLB 무효화가
 * 뒤따라야 하며, 그것은 호출자(pasid.c 의 teardown 경로)의 몫이다.
 *
 * 실행 컨텍스트: 항목 해제. iommu->lock 아래.
 */
static inline void pasid_clear_entry(struct pasid_entry *pe)
{
	WRITE_ONCE(pe->val[0], 0);	/* [한국어] present 가 여기서 꺼진다 — 이 뒤로 그 PASID 의 DMA 는 막힌다 */
	WRITE_ONCE(pe->val[1], 0);	/* [한국어] 도메인 id 와 주소 폭 */
	WRITE_ONCE(pe->val[2], 0);	/* [한국어] 2단계 페이지 테이블 주소 */
	WRITE_ONCE(pe->val[3], 0);	/* [한국어] 나머지 설정 */
	WRITE_ONCE(pe->val[4], 0);	/* [한국어] 나머지 설정 */
	WRITE_ONCE(pe->val[5], 0);	/* [한국어] 나머지 설정 */
	WRITE_ONCE(pe->val[6], 0);	/* [한국어] 나머지 설정 */
	WRITE_ONCE(pe->val[7], 0);	/* [한국어] 나머지 설정. memset 대신 워드마다 쓰는 것은 각각이 온전한 64비트 쓰기여야 하기 때문이다 */
}

/*
 * [한국어]
 * pasid_clear_entry_with_fpd - 항목을 비우되 폴트 보고만 꺼 둔다
 *
 * @pe: 비울 항목.
 * @return: 없음.
 *
 * pasid_clear_entry 와 거의 같지만 val[0] 에 PASID_PTE_FPD 를 남긴다.
 * present 는 여전히 0 이므로 접근은 막히고, 다만 그 접근이 폴트로 보고되지
 * 않는다.
 *
 * 왜 이런 상태가 필요한가: PASID 를 내리는 순간 그 장치는 아직 그 PASID 로
 * DMA 를 내고 있을 수 있다. 그것을 전부 폴트로 보고하면 로그가 뒤덮이고,
 * 폴트 처리 자체가 시스템을 느리게 만든다. 접근은 막되 조용히 막는 것이
 * 이 상태의 목적이다.
 *
 * intel_pasid_tear_down_entry 의 fault_ignore 인자가 이 함수와
 * pasid_clear_entry 중 무엇을 쓸지를 정한다.
 *
 * 실행 컨텍스트: 항목 해제. iommu->lock 아래.
 */
static inline void pasid_clear_entry_with_fpd(struct pasid_entry *pe)
{
	WRITE_ONCE(pe->val[0], PASID_PTE_FPD);	/* [한국어] present 는 끄되 FPD 만 남긴다 — 접근은 막고 폴트 보고만 끈다 */
	WRITE_ONCE(pe->val[1], 0);	/* [한국어] 나머지는 모두 비운다 */
	WRITE_ONCE(pe->val[2], 0);	/* [한국어] 같음 */
	WRITE_ONCE(pe->val[3], 0);	/* [한국어] 같음 */
	WRITE_ONCE(pe->val[4], 0);	/* [한국어] 같음 */
	WRITE_ONCE(pe->val[5], 0);	/* [한국어] 같음 */
	WRITE_ONCE(pe->val[6], 0);	/* [한국어] 같음 */
	WRITE_ONCE(pe->val[7], 0);	/* [한국어] 같음 */
}

/*
 * [한국어]
 * pasid_set_bits - PASID 항목의 한 워드에서 지정한 비트만 바꾼다
 *
 * @ptr: 대상 워드(pe->val[n]). @mask: 바꿀 비트들. @bits: 넣을 값.
 * @return: 없음.
 *
 * 이 파일의 모든 pasid_set_* 헬퍼가 결국 이 함수를 부른다. 읽고, 마스크로
 * 그 자리를 비우고, 새 값을 넣어 한 번에 쓴다.
 *
 * READ_ONCE/WRITE_ONCE 인 이유: 하드웨어가 동시에 같은 워드를 읽고 있다.
 * 컴파일러가 읽기·쓰기를 쪼개면 하드웨어가 절반만 바뀐 값을 볼 수 있고,
 * 그것이 유효해 보이는 다른 설정으로 해석될 수 있다.
 *
 * 주의: 이 함수는 원자적 갱신이 아니다. 읽기와 쓰기 사이에 다른 CPU 가 같은
 * 워드를 바꾸면 그 변경이 사라진다. 그래서 항목을 세우는 경로는 모두
 * iommu->lock 을 쥐고 있어야 한다.
 *
 * 실행 컨텍스트: 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_bits(u64 *ptr, u64 mask, u64 bits)
{
	u64 old;	/* [한국어] 현재 값 */

	old = READ_ONCE(*ptr);	/* [한국어] 한 번의 온전한 읽기 */
	WRITE_ONCE(*ptr, (old & ~mask) | bits);	/* [한국어] 마스크 자리를 비우고 새 값을 넣어 한 번에 쓴다. 원자적 갱신이 아니므로 호출자가 락을 쥐고 있어야 한다 */
}

/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_get_bits - PASID 항목의 한 워드를 그대로 읽는다
 *
 * @ptr: 읽을 워드(pe->val[n]).
 * @return: 그 워드의 현재 값.
 *
 * pasid_set_bits 의 짝. READ_ONCE 로 한 번에 읽어, 컴파일러가 읽기를 쪼개
 * 하드웨어가 갱신 중인 값을 반쯤 보는 일이 없게 한다.
 * 주로 특정 비트가 켜져 있는지 확인하는 getter 들이 쓴다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline u64 pasid_get_bits(u64 *ptr)
{
	return READ_ONCE(*ptr);	/* [한국어] 워드 하나를 그대로 읽는다 */
}

/*
 * Setup the DID(Domain Identifier) field (Bit 64~79) of scalable mode
 * PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_domain_id - 이 항목에 도메인 id 를 넣는다 (비트 64~79, 위 영어 주석)
 *
 * @pe: 대상 PASID 항목. @value: 넣을 값.
 * @return: 없음.
 *
 * val[1] 의 하위 16비트다. 이 id 가 하드웨어 IOTLB 항목의 태그가 되어,
 * 도메인 단위 무효화가 어느 항목을 지울지를 정한다. 같은 id 를 쓰는 PASID
 * 들의 번역은 캐시에서 공유되므로 무효화 한 번이 그 모두에 적용된다.
 * 값 범위: 16비트이지만 실제 상한은 유닛의 cap_ndoms 다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void
pasid_set_domain_id(struct pasid_entry *pe, u64 value)
{
	pasid_set_bits(&pe->val[1], GENMASK_ULL(15, 0), value);	/* [한국어] val[1] 의 하위 16비트에 도메인 id */
}

/*
 * Get domain ID value of a scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_get_domain_id - 이 항목에 적힌 도메인 id 를 읽는다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 그 항목의 도메인 id.
 *
 * 주로 kdump 인계 경로와 진단에서 쓴다 — 이전 커널이 쓰던 id 를 읽어
 * 우리 할당기에서 미리 예약해 두어야 다른 도메인에 같은 번호를 주지 않는다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline u16
pasid_get_domain_id(struct pasid_entry *pe)
{
	return (u16)(READ_ONCE(pe->val[1]) & GENMASK_ULL(15, 0));	/* [한국어] 그 자리를 도로 읽는다 */
}

/*
 * Setup the SLPTPTR(Second Level Page Table Pointer) field (Bit 12~63)
 * of a scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_slptr - 2단계 페이지 테이블의 물리 주소를 넣는다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목. @value: 넣을 값.
 * @return: 없음.
 *
 * SLPTR(Second Level Page Table pointer)는 val[0] 의 상위 비트에 들어간다.
 * PGTT 가 SL_ONLY 이거나 NESTED 일 때만 의미가 있으며, 1단계 전용이나
 * 통과 모드에서는 하드웨어가 이 필드를 무시한다.
 * VTD_PAGE_MASK 로 마스크하므로 페이지 정렬되지 않은 주소는 조용히 잘린다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void
pasid_set_slptr(struct pasid_entry *pe, u64 value)
{
	pasid_set_bits(&pe->val[0], VTD_PAGE_MASK, value);	/* [한국어] val[0] 의 주소 자리에 2단계 테이블 주소. 페이지 정렬되지 않은 값은 잘린다 */
}

/*
 * Setup the AW(Address Width) field (Bit 2~4) of a scalable mode PASID
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_address_width - 2단계 페이지 테이블의 주소 폭(AW)을 넣는다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목. @value: 넣을 값.
 * @return: 없음.
 *
 * val[0] 의 비트 2~4 다. 하드웨어는 이 값으로 2단계 테이블을 몇 단계
 * 워크할지 안다 — 실제 테이블의 깊이와 어긋나면 엉뚱한 메모리를 테이블로
 * 읽는다. 도메인 생성 때 계산한 AGAW 가 여기 들어간다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void
pasid_set_address_width(struct pasid_entry *pe, u64 value)
{
	pasid_set_bits(&pe->val[0], GENMASK_ULL(4, 2), value << 2);	/* [한국어] 비트 2~4 에 2단계 주소 폭 */
}

/*
 * Setup the PGTT(PASID Granular Translation Type) field (Bit 6~8)
 * of a scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_translation_type - 이 항목의 변환 종류(PGTT)를 정한다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목. @value: PASID_ENTRY_PGTT_* 중 하나.
 * @return: 없음.
 *
 * val[0] 의 비트 6~8 이며, 이 항목에서 가장 중요한 필드다. 이 값이
 * 나머지 필드의 해석을 통째로 정하기 때문이다.
 *   FL_ONLY — flptr(1단계 주소)만 본다.
 *   SL_ONLY — slptr(2단계 주소)만 본다.
 *   NESTED  — 둘 다 본다. 1단계로 얻은 주소를 2단계로 다시 번역한다.
 *   PT      — 주소 필드를 모두 무시하고 번역 없이 통과시킨다.
 * 그래서 항목을 세우는 순서에서 이 값을 정하는 시점이 중요하다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void
pasid_set_translation_type(struct pasid_entry *pe, u64 value)
{
	pasid_set_bits(&pe->val[0], GENMASK_ULL(8, 6), value << 6);	/* [한국어] 비트 6~8 에 변환 종류. 이 값이 나머지 필드의 해석을 정한다 */
}

/*
 * Enable fault processing by clearing the FPD(Fault Processing
 * Disable) field (Bit 1) of a scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_fault_enable - 이 항목의 폴트 보고를 켠다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * val[0] 의 비트 1 이 FPD(Fault Processing Disable)이며, 이름대로 1 이면
 * 보고하지 않는다. 이 함수는 그 비트를 0 으로 만들어 보고를 켠다 —
 * 함수 이름과 비트의 의미가 반대라 헷갈리기 쉬운 자리다.
 * (컨텍스트 항목의 context_set_fault_enable 도 같은 형태다.)
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_fault_enable(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[0], 1 << 1, 0);	/* [한국어] FPD 비트를 0 으로 — 폴트 보고를 켠다 */
}

/*
 * Enable second level A/D bits by setting the SLADE (Second Level
 * Access Dirty Enable) field (Bit 9) of a scalable mode PASID
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_ssade - 2단계 접근/더티 비트 추적을 켠다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * SSADE(Second Stage Access/Dirty Enable). 이 비트가 켜지면 하드웨어가
 * 2단계 페이지 테이블 항목에 "접근했다/썼다"를 기록한다.
 * 라이브 마이그레이션에서 장치가 고친 페이지만 다시 보내려면 이 정보가
 * 필요하다 — DMA 는 CPU 를 거치지 않으므로 IOMMU 가 따로 남겨야 한다.
 * 도메인 단위 설정이 이 헬퍼를 통해 각 PASID 항목에 반영된다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_ssade(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[0], 1 << 9, 1 << 9);	/* [한국어] SSADE 를 켠다 — 하드웨어가 접근/더티 비트를 기록한다 */
}

/*
 * Disable second level A/D bits by clearing the SLADE (Second Level
 * Access Dirty Enable) field (Bit 9) of a scalable mode PASID
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_clear_ssade - 더티 추적을 끈다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * 마이그레이션이 끝나면 추적을 끈다. 켜 둔 채로 두면 하드웨어가 매 쓰기마다
 * 테이블 항목을 갱신해야 해서 성능이 떨어진다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_clear_ssade(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[0], 1 << 9, 0);	/* [한국어] SSADE 를 끈다 */
}

/*
 * Checks if second level A/D bits specifically the SLADE (Second Level
 * Access Dirty Enable) field (Bit 9) of a scalable mode PASID
 * entry is set.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_get_ssade - 더티 추적이 켜져 있는지 읽는다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: true 면 켜져 있다.
 *
 * 항목을 다시 세울 때 기존 설정을 이어받으려고 읽는다. 도메인이 추적 중인데
 * 항목을 새로 만들면서 그 설정을 빠뜨리면 그 PASID 의 쓰기만 기록되지 않아,
 * 마이그레이션이 조용히 페이지를 놓친다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline bool pasid_get_ssade(struct pasid_entry *pe)
{
	return pasid_get_bits(&pe->val[0]) & (1 << 9);	/* [한국어] SSADE 가 켜져 있는지 */
}

/*
 * Setup the SRE(Supervisor Request Enable) field (Bit 128) of a
 * scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_sre - 특권(supervisor) 요청을 허용한다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * SRE(Supervisor Request Enable). 장치가 커널 주소 공간에 접근하는 요청을
 * 낼 수 있게 한다. 보통의 DMA 는 유저 권한으로 취급되지만, 커널이 직접
 * 관리하는 버퍼를 장치가 다루는 경우에 필요하다.
 * 격리 관점에서 위험한 설정이라 필요한 경로에서만 켠다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_sre(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[2], 1 << 0, 1);	/* [한국어] SRE — 특권 요청 허용 */
}

/*
 * Setup the WPE(Write Protect Enable) field (Bit 132) of a
 * scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_wpe - 쓰기 보호를 켠다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * WPE(Write Protect Enable). 켜지면 특권 요청이라도 읽기 전용 페이지에
 * 쓸 수 없다. CPU 의 CR0.WP 와 같은 개념이며, SRE 를 켠 항목에서 커널
 * 페이지 보호를 유지하려면 함께 켜야 한다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_wpe(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[2], 1 << 4, 1 << 4);	/* [한국어] WPE — 특권 요청이라도 읽기 전용 페이지에 쓰지 못하게 한다 */
}

/*
 * Setup the P(Present) field (Bit 0) of a scalable mode PASID
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_present - 항목을 유효하게 만든다(= 하드웨어에 넘긴다) (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * 이 함수가 PASID 항목의 소유권을 소프트웨어에서 하드웨어로 넘기는 지점이다.
 * 이 비트가 서는 순간부터 그 PASID 의 DMA 가 번역되기 시작한다.
 * dma_wmb() 가 먼저 오는 것이 핵심이다 — 나머지 일곱 워드의 쓰기가 present
 * 쓰기보다 먼저 보여야, 하드웨어가 절반만 채워진 항목을 워크하지 않는다.
 * (컨텍스트 항목의 context_set_present 와 같은 구조다.)
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_present(struct pasid_entry *pe)
{
	dma_wmb();	/* [한국어] 나머지 일곱 워드의 쓰기가 present 쓰기보다 먼저 보이도록 강제한다 */
	pasid_set_bits(&pe->val[0], 1 << 0, 1);	/* [한국어] present 를 세운다 — 이 순간부터 하드웨어가 이 항목을 쓴다 */
}

/*
 * Clear the Present (P) bit (bit 0) of a scalable-mode PASID table entry.
 * This initiates the transition of the entry's ownership from hardware
 * to software. The caller is responsible for fulfilling the invalidation
 * handshake recommended by the VT-d spec, Section 6.5.3.3 (Guidance to
 * Software for Invalidations).
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_clear_present - 항목을 무효화한다(= 하드웨어에서 되찾아 온다) (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * set 의 반대. 이 뒤로 그 PASID 의 DMA 는 폴트로 끝난다.
 * 장벽의 위치가 set 과 반대인 점을 눈여겨볼 것: set 은 dma_wmb 가 앞에,
 * clear 는 뒤에 온다. set 은 "나머지 필드가 먼저 보여야" 하고, clear 는
 * "present 를 지운 것이 이후의 정리 작업보다 먼저 보여야" 하기 때문이다.
 * 이것만으로는 끝이 아니다 — 하드웨어가 이미 캐시했을 수 있으므로 PASID
 * 캐시와 IOTLB 무효화가 뒤따라야 한다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_clear_present(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[0], 1 << 0, 0);	/* [한국어] present 를 지운다 — 이 뒤로 그 PASID 의 DMA 는 막힌다 */
	dma_wmb();	/* [한국어] present 를 지운 것이 이후의 정리 작업보다 먼저 보이도록 한다. set 과 장벽의 위치가 반대인 이유다 */
}

/*
 * Setup Page Walk Snoop bit (Bit 87) of a scalable mode PASID
 * entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_page_snoop - 이 항목의 DMA 가 CPU 캐시를 스누프하도록 강제한다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목. @value: true 면 강제.
 * @return: 없음.
 *
 * 1단계 페이지 테이블은 x86-64 CPU 형식 그대로라 PTE 안에 스누프 제어
 * 비트를 둘 자리가 없다. 그래서 PTE 가 아니라 이 PASID 항목에 도메인 단위로
 * 설정한다 — intel_iommu_enforce_cache_coherency_fs 가 이 헬퍼를 쓴다.
 * (2단계는 PTE 마다 SNP 비트가 있어 설정만 바꿔 두면 된다.)
 * VFIO/KVM 이 게스트에 WBINVD 를 허용하지 않아도 되게 하는 보장이다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_page_snoop(struct pasid_entry *pe, bool value)
{
	pasid_set_bits(&pe->val[1], 1 << 23, value << 23);	/* [한국어] val[1] 의 비트 23 — 데이터 DMA 의 캐시 스누프 강제 */
}

/*
 * Setup the Page Snoop (PGSNP) field (Bit 88) of a scalable mode
 * PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_pgsnp - 페이지 워크도 CPU 캐시를 스누프하게 한다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * PGSNP(Page Snoop). 위 page_snoop 이 "데이터 DMA"의 코히런시라면, 이쪽은
 * "하드웨어가 페이지 테이블을 읽을 때"의 코히런시다. 켜 두면 커널이 테이블을
 * 고친 뒤 clflush 하지 않아도 하드웨어가 최신 값을 본다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void
pasid_set_pgsnp(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[1], 1ULL << 24, 1ULL << 24);	/* [한국어] 비트 24 — 페이지 워크의 캐시 스누프 */
}

/*
 * Setup the First Level Page table Pointer field (Bit 140~191)
 * of a scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_flptr - 1단계 페이지 테이블의 물리 주소를 넣는다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목. @value: 넣을 값.
 * @return: 없음.
 *
 * FLPTR(First Level Page Table pointer)는 val[2] 에 들어간다.
 * SVA 에서는 프로세스의 CR3 값(mm 의 페이지 테이블)이 그대로 여기 들어간다 —
 * 1단계 형식이 x86-64 CPU 페이지 테이블과 같기 때문에 가능한 일이다.
 * PGTT 가 FL_ONLY 이거나 NESTED 일 때만 의미가 있다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void
pasid_set_flptr(struct pasid_entry *pe, u64 value)
{
	pasid_set_bits(&pe->val[2], VTD_PAGE_MASK, value);	/* [한국어] val[2] 의 주소 자리에 1단계 테이블 주소. SVA 에서는 프로세스의 CR3 값이 그대로 들어간다 */
}

/*
 * Setup the First Level Paging Mode field (Bit 130~131) of a
 * scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_flpm - 1단계 페이지 테이블의 레벨 수를 정한다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목. @value: 0 이면 4레벨, 1 이면 5레벨.
 * @return: 없음.
 *
 * FLPM(First Level Paging Mode). flptr 이 가리키는 테이블을 몇 단계
 * 워크할지를 하드웨어에 알린다. SVA 에서는 프로세스의 페이지 테이블 깊이와
 * 반드시 일치해야 하며, 어긋나면 하드웨어가 엉뚱한 메모리를 테이블로 읽는다.
 * PASID_FLAG_FL5LP 플래그가 이 값의 근거가 된다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void
pasid_set_flpm(struct pasid_entry *pe, u64 value)
{
	pasid_set_bits(&pe->val[2], GENMASK_ULL(3, 2), value << 2);	/* [한국어] 비트 2~3 에 1단계 레벨 수. 프로세스 테이블의 깊이와 일치해야 한다 */
}

/*
 * Setup the Extended Access Flag Enable (EAFE) field (Bit 135)
 * of a scalable mode PASID entry.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * pasid_set_eafe - 확장 접근 플래그를 켠다 (위 영어 주석)
 *
 * @pe: 대상 PASID 항목.
 * @return: 없음.
 *
 * EAFE(Extended Accessed Flag Enable). 하드웨어가 1단계 페이지 테이블에
 * 확장 접근 비트를 기록하게 한다. SVA 에서 프로세스 페이지 테이블을
 * 공유할 때, CPU 와 장치의 접근을 구분해 기록하려고 쓴다.
 *
 * 실행 컨텍스트: PASID 항목 설정. iommu->lock 아래.
 */
static inline void pasid_set_eafe(struct pasid_entry *pe)
{
	pasid_set_bits(&pe->val[2], 1 << 7, 1 << 7);	/* [한국어] EAFE — 확장 접근 플래그 기록 */
}

extern unsigned int intel_pasid_max_id;	/* [한국어] 시스템 전체에서 쓸 수 있는 PASID 의 상한. 모든 유닛의 ecap_pss 중 가장 작은 값에서 정해진다 */
int intel_pasid_alloc_table(struct device *dev);	/* [한국어] 이 장치 전용 PASID 디렉터리를 만든다. 프로브 때 한 번 */
void intel_pasid_free_table(struct device *dev);	/* [한국어] 그것과 그 아래 테이블들을 반납한다 */
struct pasid_table *intel_pasid_get_table(struct device *dev);	/* [한국어] 장치에 매달린 PASID 테이블 구조체를 얻는다 */
int intel_pasid_setup_first_level(struct intel_iommu *iommu, struct device *dev,	/* [한국어] PASID 항목을 1단계 변환으로 세운다. SVA 와 1단계 도메인이 쓴다 */
				  phys_addr_t fsptptr, u32 pasid, u16 did,
				  int flags);
int intel_pasid_setup_second_level(struct intel_iommu *iommu,	/* [한국어] 2단계 변환으로 세운다 */
				   struct dmar_domain *domain,
				   struct device *dev, u32 pasid);
int intel_pasid_setup_dirty_tracking(struct intel_iommu *iommu,	/* [한국어] 이미 세워진 항목의 더티 추적만 켜고 끈다 */
				     struct device *dev, u32 pasid,
				     bool enabled);
int intel_pasid_setup_pass_through(struct intel_iommu *iommu,	/* [한국어] 번역 없이 통과하도록 세운다. 항등 도메인의 scalable 모드 구현이다 */
				   struct device *dev, u32 pasid);
int intel_pasid_setup_nested(struct intel_iommu *iommu, struct device *dev,	/* [한국어] 중첩 변환으로 세운다. 게스트 1단계와 호스트 2단계 주소를 모두 담는다 */
			     u32 pasid, struct dmar_domain *domain);
void intel_pasid_tear_down_entry(struct intel_iommu *iommu,	/* [한국어] 항목을 내리고 관련 캐시를 비운다 */
				 struct device *dev, u32 pasid,
				 bool fault_ignore);
void intel_pasid_setup_page_snoop_control(struct intel_iommu *iommu,	/* [한국어] 이미 붙어 있는 장치의 항목에 캐시 스누프 강제를 반영한다 */
					  struct device *dev, u32 pasid);
int intel_pasid_setup_sm_context(struct device *dev);	/* [한국어] 컨텍스트 항목이 이 장치의 PASID 디렉터리를 가리키게 세운다. scalable 모드로 들어가는 관문이다 */
void intel_pasid_teardown_sm_context(struct device *dev);	/* [한국어] 그 컨텍스트 항목을 내린다 */
#endif /* __INTEL_PASID_H */
