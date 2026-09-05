/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * AMD IOMMU v1 page table
 *
 * This is described in Section "2.2.3 I/O Page Tables for Host Translations"
 * of the "AMD I/O Virtualization Technology (IOMMU) Specification"
 *
 * Note the level numbering here matches the core code, so level 0 is the same
 * as mode 1.
 *
 */
/*
 * [한국어 설명] AMD IOMMU v1 페이지 테이블 형식 구현 (amdv1.h)
 *
 * === 파일의 역할 ===
 * generic_pt 가 요구하는 형식 API 를 AMD v1 항목 배치에 맞춰 채운다.
 * 항목을 읽고(load_entry_raw), 잎과 표를 쓰고(install_*), 출력 주소와
 * 크기를 꺼내는(entry_oa, entry_num_contig_lg2) 함수들이 전부다.
 *
 * 이 형식의 특징이 두 가지다.
 *
 * 첫째, 다음 단계 번호를 항목 안에 적는다. 보통의 페이지 테이블은 "표인가
 * 잎인가"를 비트 하나로 가르지만, AMD 는 그 자리에 3비트 코드를 두어
 * 0 이면 잎, 7 이면 "크기가 인코딩된 잎", 그 밖이면 그 번호의 표를 가리킨다.
 * 그래서 한 단계에서 여러 단계를 건너뛰는 표도 만들 수 있다.
 *
 * 둘째, 큰 페이지의 크기를 주소 필드 안에 적는다. 출력 주소의 하위 비트에
 * 1 을 연달아 세워 그 길이로 크기를 나타내는데, 그래서 2의 거듭제곱이면
 * 어떤 크기든 표현할 수 있다 — 다른 형식들이 단계 경계의 크기만 쓸 수
 * 있는 것과 대조적이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_amdv1.c → iommu_template.h → defs_amdv1.h → pt_defs.h →
 *   [이 파일] → pt_common.h → iommu_pt.h → drivers/iommu/amd
 *
 * 실행 컨텍스트: 전부 인라인. 순회 코드 안에 박혀 컴파일된다.
 *
 * === 타 모듈과의 연결 ===
 * 위: pt_common.h 가 선언한 API 를 이 파일이 #define 으로 잇는다.
 * 아래: <asm/page.h>, <linux/mem_encrypt.h>(SME 비트), <linux/bitfield.h>.
 * 파일 뒤쪽의 "--- iommu" 아래는 IOMMU 계층과의 접점이다 — 권한 변환,
 * 초기화, 하드웨어에 넘길 정보 추출.
 *
 * 데이터 흐름: 드라이버의 IOMMU_READ/WRITE → amdv1pt_iommu_set_prot →
 * pt_write_attrs → amdv1pt_install_leaf_entry → 표 항목.
 *
 * === 주요 함수/구조체 요약 ===
 * amdv1pt_load_entry_raw: 항목을 읽고 다음 단계 코드로 종류를 가른다.
 * amdv1pt_entry_num_contig_lg2: 주소 필드의 1 의 길이에서 페이지 크기를
 *   되짚는다. 주석의 수식 전개가 그 역산이다.
 * amdv1pt_install_leaf_entry: 단일 항목이면 하나, 연속이면 같은 값을
 *   여러 항목에 채운다.
 * amdv1pt_possible_sizes: 4KB 부터 단계 크기까지 모든 2의 거듭제곱.
 *   512GB 만 하드웨어 결함으로 빠진다.
 * amdv1pt_iommu_set_prot: IOMMU 권한을 항목 비트로 옮긴다.
 * amdv1pt_iommu_fmt_hw_info: 최상위 표 주소와 단계 수를 꺼낸다 —
 *   drivers/iommu/amd 의 amd_iommu_set_dte_v1() 이 이 값을 DTE 에 적는다.
 */
#ifndef __GENERIC_PT_FMT_AMDV1_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_FMT_AMDV1_H	/* [한국어] 같은 이름으로 표시 */

#include "defs_amdv1.h"	/* [한국어] 이 형식의 주소 타입과 쓰기 속성 */
#include "../pt_defs.h"	/* [한국어] 순회 상태와 산술 */

#include <asm/page.h>	/* [한국어] PAGE_SHIFT — 커널 페이지 크기와 비교 */
#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP 로 항목 필드를 다룬다 */
#include <linux/container_of.h>	/* [한국어] IOMMU 객체와 공통 상태를 오간다 */
#include <linux/mem_encrypt.h>	/* [한국어] __sme_set/__sme_clr — SME 암호화 비트 */
#include <linux/minmax.h>	/* [한국어] min/max */
#include <linux/sizes.h>	/* [한국어] SZ_512G — 결함으로 막는 페이지 크기 */
#include <linux/string.h>	/* [한국어] memset64 — 큰 연속 묶음 채우기 */

/*
 * [한국어] 이 형식의 크기 상수들.
 * generic_pt 의 공통 코드가 단계별 항목 크기와 색인을 이 값들로 계산한다.
 */
enum {
	PT_ITEM_WORD_SIZE = sizeof(u64),
	/* [한국어] 페이지 테이블 항목 하나의 바이트 폭 — 8바이트.
	 * 읽는 자: generic_pt 의 공통 순회 코드가 항목 사이의 보폭과 표당 항목 수를
	 *   계산할 때.
	 * 값 범위: 8 로 고정. AMD v1 형식의 항목은 언제나 64비트다.
	 * 표가 4KB 이고 항목이 8바이트이므로 표마다 512개, 곧 단계마다 색인이
	 *   9비트다 — 이것이 이 형식의 기본 골격이다. */
	/*
	 * The IOMMUFD selftest uses the AMDv1 format with some alterations It
	 * uses a 2k page size to test cases where the CPU page size is not the
	 * same.
	 */
#ifdef AMDV1_IOMMUFD_SELFTEST
	PT_MAX_VA_ADDRESS_LG2 = 56,	/* [한국어] (원 주석: 셀프테스트는 CPU 페이지 크기와 다른 경우를 시험하려고 2KB 페이지를 쓴다) */
	PT_MAX_OUTPUT_ADDRESS_LG2 = 51,
	/* [한국어] (셀프테스트) 출력 물리 주소의 비트 폭.
	 * 읽는 자: 시험이 만드는 표에서 물리 주소 범위를 검사할 때.
	 * 값이 실제 하드웨어(52)보다 1 작은 이유: 페이지 크기가 2KB 로 줄어들면서
	 *   주소 자리 배치가 한 비트 밀리기 때문이다. 위 PT_GRANULE_LG2SZ 가 11 인
	 *   것과 짝이 된다.
	 * 이 #ifdef 갈래 전체가 iommufd 셀프테스트 전용이다 — 실제 하드웨어에서는
	 *   아래 #else 쪽 값들이 쓰인다. */
	PT_MAX_TOP_LEVEL = 4,
	/* [한국어] (셀프테스트) 최상위 단계의 번호 — 0 기반이라 5단계.
	 * 읽는 자: 시험이 표를 만들 때.
	 * 실제 하드웨어의 5(=6단계)보다 하나 적다. 시험은 깊이 자체를 검증하려는
	 *   것이 아니라 페이지 크기가 어긋나는 경우를 보려는 것이라, 표를 얕게
	 *   잡아 시험을 빠르게 한다. */
	PT_GRANULE_LG2SZ = 11,
	/* [한국어] (셀프테스트) 최소 페이지 크기의 로그 값 — 2KB.
	 * 읽는 자: 매핑 요청의 정렬 검사와 단계별 페이지 크기 계산.
	 * 왜 2KB 인가: 위 영어 주석이 말하듯, CPU 페이지 크기(4KB)와 일부러 어긋나게
	 *   한 것이다. 두 크기가 같으면 "페이지 하나가 곧 항목 하나"라는 암묵적
	 *   가정이 코드에 숨어 있어도 시험을 통과해 버린다. 어긋나게 두면 그런
	 *   가정이 곧바로 드러난다.
	 * 실제 AMD 하드웨어는 4KB(로그 12)를 쓴다. */
#else
	PT_MAX_VA_ADDRESS_LG2 = 64,	/* [한국어] 실제 하드웨어: 입력 주소 전 범위 */
	PT_MAX_OUTPUT_ADDRESS_LG2 = 52,
	/* [한국어] (실제 하드웨어) 출력 물리 주소의 비트 폭.
	 * 읽는 자: 매핑하려는 물리 주소가 표현 가능한지 검사하는 코드.
	 * 값이 52 인 이유: 아래 AMDV1PT_FMT_OA 가 비트 12 부터 51 까지를 쓰고,
	 *   여기에 페이지 안 오프셋 12비트를 더하면 52 다.
	 * 위 셀프테스트 갈래의 51 과 대비된다 — 그쪽은 페이지가 2KB 라 배치가 다르다. */
	PT_MAX_TOP_LEVEL = 5,
	/* [한국어] (실제 하드웨어) 최상위 단계의 번호 — 0 기반이라 최대 6단계.
	 * 읽는 자: 표를 만들 때 몇 단계를 잡을지.
	 * 값이 5 인 것이 이 형식의 특징이다. 대부분의 형식이 4~5단계인데 AMD v1 은
	 *   여섯 단계까지 갈 수 있어, 입력 주소 공간이 64비트 전체(위
	 *   PT_MAX_VA_ADDRESS_LG2 = 64)에 이른다.
	 * 실제로 몇 단계를 쓸지는 요청한 주소 공간에 따라 정해지고 이 값은 상한이다. */
	PT_GRANULE_LG2SZ = 12,
	/* [한국어] (실제 하드웨어) 최소 페이지 크기의 로그 값 — 4KB.
	 * 읽는 자: 정렬 검사와 단계별 페이지 크기 계산.
	 * 값 범위: 12. 위 셀프테스트의 11(2KB)과 대비된다.
	 * 이 형식은 단계마다 9비트를 소비하지만, 아래 NEXT_LEVEL 필드의 크기
	 *   인코딩 덕분에 2의 거듭제곱이면 어떤 크기든 잎으로 만들 수 있다 —
	 *   다른 형식이 정해진 몇 가지 큰 페이지만 지원하는 것과 다른 점이다. */
#endif
	PT_TABLEMEM_LG2SZ = 12,	/* [한국어] 표 하나가 4KB */

	/* The DTE only has these bits for the top phyiscal address */
	PT_TOP_PHYS_MASK = GENMASK_ULL(51, 12),
	/* [한국어] (원 주석 참고) 최상위 표의 물리 주소로 쓸 수 있는 비트들.
	 * 읽는 자: 최상위 표의 주소를 장치 표 항목(DTE)에 실을 때, 그리고 검사할 때.
	 * 값 범위: 비트 12 부터 51 까지.
	 * 원 주석이 말하는 제약: DTE 의 최상위 물리 주소 필드가 이 비트들만 갖는다.
	 *   표를 그보다 높은 물리 주소에 잡으면 하드웨어에 알릴 방법이 없으므로,
	 *   할당 단계에서 이 마스크로 걸러야 한다.
	 * 하위 12비트가 빠진 이유: 표는 4KB 정렬이고, 그 자리는 DTE 의 다른 필드다. */
};

/* PTE bits */
/*
 * [한국어] (위 영어 주석에 이어)
 * 페이지 테이블 항목(PTE)의 비트 배치.
 * AMD I/O Virtualization Technology (IOMMU) Specification 의
 * "2.2.3 I/O Page Tables for Host Translations" 절이 출처다.
 */
enum {
	AMDV1PT_FMT_PR = BIT(0),
	/* [한국어] 존재(present) 비트 — 0 이면 이 항목은 비어 있다.
	 * 설정자: 매핑을 만들 때 세우고, 지울 때 내린다.
	 * 읽는 자: 하드웨어와 순회 코드.
	 * 값 범위: 0 또는 1. 0 이면 나머지 비트는 무의미하다.
	 * 0번 비트인 이유는 판정을 가장 싸게 하기 위해서다.
	 * 이 형식에서 "빈 항목"은 곧 0 이라, 표를 0 으로 채우면 그대로 빈 표가 된다. */
	AMDV1PT_FMT_D = BIT(6),
	/* [한국어] 더티 비트 — 하드웨어가 이 페이지에 쓰기를 했다는 표시.
	 * 설정자: 하드웨어. 소프트웨어는 더티 추적을 켤 때 0 으로 지워 두고,
	 *   나중에 세워졌는지 읽는다.
	 * 읽는 자: 더티 추적 결과를 수집하는 코드(iommufd 의 dirty tracking).
	 * 왜 필요한가: 라이브 마이그레이션에서 장치가 고친 페이지만 다시 보내려면,
	 *   어느 페이지가 고쳐졌는지 하드웨어가 알려 줘야 한다.
	 * 다른 형식의 D 비트를 미리 세워 두는 것과 달리, AMD 에서는 이 비트가
	 *   기능으로 쓰이므로 추적 중에는 0 으로 두어야 한다. */
	AMDV1PT_FMT_NEXT_LEVEL = GENMASK_ULL(11, 9),
	/* [한국어] 다음 단계 코드 — 이 항목이 표인지 잎인지, 잎이면 크기가 얼마인지를 정한다.
	 * 설정자: 표를 이어 붙일 때는 아래 단계 번호를, 잎을 만들 때는 0 또는 7 을 넣는다.
	 * 읽는 자: 하드웨어와 순회 코드.
	 * 값 범위: 0 이면 기본 크기의 잎, 7 이면 크기가 인코딩된 잎, 그 밖의 값은
	 *   그 번호의 아래 표를 가리킨다.
	 * 7 의 뜻이 이 형식의 가장 독특한 부분이다. 다른 형식은 단계마다 정해진
	 *   큰 페이지 크기(2MB, 1GB)만 지원하지만, AMD 는 7 을 넣고 아래 OA 필드의
	 *   하위 비트에 크기를 인코딩해 2의 거듭제곱이면 어떤 크기든 표현한다.
	 *   그래서 이 형식은 임의 크기의 연속 매핑을 항목 하나로 담을 수 있다. */
	AMDV1PT_FMT_OA = GENMASK_ULL(51, 12),
	/* [한국어] 출력 주소가 들어가는 자리 — 잎이면 물리 주소, 표면 아래 표의 주소.
	 * 설정자: 매핑을 만들거나 표를 이어 붙일 때.
	 * 읽는 자: 하드웨어, 그리고 항목에서 주소를 되꺼내는 코드.
	 * 값 범위: 비트 12 부터 51 까지 40비트. 12비트를 밀면 52비트 물리 주소다.
	 * 크기 인코딩된 잎(NEXT_LEVEL = 7)에서는 이 자리의 하위 비트가 주소가 아니라
	 *   크기를 나타낸다. 1 이 처음 나타나는 자리가 매핑 크기를 말해 주는 방식이라,
	 *   주소를 꺼낼 때 그 부분을 걸러 내야 한다.
	 * 그 이중 용도 때문에 이 필드를 다루는 코드가 이 형식에서 가장 까다롭다. */
	AMDV1PT_FMT_FC = BIT_ULL(60),
	/* [한국어] 강제 일관(force coherent) 비트.
	 * 설정자: 도메인이 캐시 일관성을 강제하도록 설정되어 있으면 세운다.
	 * 읽는 자: 하드웨어.
	 * 무엇을 하는가: 장치가 비일관(no-snoop) 접근을 요청해도 하드웨어가 그것을
	 *   무시하고 캐시 일관 접근으로 처리하게 만든다.
	 * 왜 필요한가: 장치가 비일관으로 쓴 데이터는 CPU 캐시와 어긋날 수 있다.
	 *   신뢰할 수 없는 장치(유저스페이스 드라이버, 패스스루된 장치)에게 그 선택을
	 *   맡기면 호스트가 캐시 관리를 예측할 수 없어, 매핑 단위로 막아 둔다.
	 * enforce_cache_coherency 라는 도메인 속성이 이 비트로 구현된다. */
	AMDV1PT_FMT_IR = BIT_ULL(61),
	/* [한국어] 읽기 허용(IOMMU read) 비트.
	 * 설정자: IOMMU_READ 권한이 있으면 세운다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0 이면 이 매핑으로 읽기를 할 수 없다.
	 * x86_64 형식과 달리 읽기 허용이 독립된 비트다. 덕분에 "쓰기만 가능"한
	 *   매핑을 표현할 수 있다 — 장치가 결과만 쓰고 되읽지 않는 버퍼에 쓰면
	 *   실수로 인한 정보 유출을 막는다.
	 * 비트 61 처럼 높은 자리에 있는 것은, 낮은 자리를 CPU 형식과 호환되게
	 *   비워 두려던 설계의 흔적이다. */
	AMDV1PT_FMT_IW = BIT_ULL(62),
	/* [한국어] 쓰기 허용(IOMMU write) 비트.
	 * 설정자: IOMMU_WRITE 권한이 있으면 세운다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0 이면 읽기 전용 매핑이다.
	 * 위 IR 과 짝을 이루며, 둘이 독립이라 네 가지 조합이 모두 표현된다.
	 *   둘 다 0 인 항목은 존재하지만 아무 접근도 허용하지 않는 매핑이 되는데,
	 *   그런 항목을 만들 이유는 보통 없다. */
};

/*
 * gcc 13 has a bug where it thinks the output of FIELD_GET() is an enum, make
 * these defines to avoid it.
 */
#define AMDV1PT_FMT_NL_DEFAULT 0	/* [한국어] (원 주석: gcc 13 이 FIELD_GET 결과를 enum 으로 보는 버그가 있어 define 으로 둔다) */
#define AMDV1PT_FMT_NL_SIZE 7	/* [한국어] 크기가 주소에 인코딩된 잎 */

/*
 * [한국어]
 * amdv1pt_table_pa - 표 항목이 가리키는 아래 표의 물리 주소
 *
 * @pts: 볼 항목(PT_ENTRY_TABLE).
 * @return: 아래 표의 물리 주소.
 *
 * 주소는 페이지 번호로 저장되어 있어 다시 곱해 준다.
 *
 * SME 비트를 벗기는 것이 요점이다. 표를 설치할 때 암호화 비트를 얹었으므로
 * 읽을 때 되돌리지 않으면 물리 주소가 엉뚱해진다 — pt_common.h 가 요구하는
 * "설치한 값과 같은 값을 돌려줄 것"이 이 처리다.
 */
static inline pt_oaddr_t amdv1pt_table_pa(const struct pt_state *pts)
{
	u64 entry = pts->entry;	/* [한국어] 읽어 둔 항목 값 */

	if (pts_feature(pts, PT_FEAT_AMDV1_ENCRYPT_TABLES))	/* [한국어] 설치할 때 암호화 비트를 얹었으면 */
		entry = __sme_clr(entry);	/* [한국어] 읽을 때 되돌려야 물리 주소가 맞는다 */
	return oalog2_mul(FIELD_GET(AMDV1PT_FMT_OA, entry), PT_GRANULE_LG2SZ);	/* [한국어] 주소는 페이지 번호로 저장되어 있다 */
}
#define pt_table_pa amdv1pt_table_pa	/* [한국어] 공통 API 이름으로 잇는다 */

/* Returns the oa for the start of the contiguous entry */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amdv1pt_entry_oa - 잎 entry 가 내는 출력 주소의 시작
 *
 * @pts: 볼 항목.
 * @return: entry 시작의 출력 주소.
 *
 * 다음 단계 코드가 7 이면 크기가 주소 안에 인코딩된 큰 페이지다. 그 경우
 * 하위의 연속된 1 들이 크기를 나타내므로, 그 자리를 0 으로 깎아야 진짜
 * 시작 주소가 나온다.
 *
 * 코드가 0 이면 평범한 단일 항목이라 주소를 그대로 쓴다. 그 밖의 값은
 * 표 항목이므로 여기 올 수 없다.
 */
static inline pt_oaddr_t amdv1pt_entry_oa(const struct pt_state *pts)
{
	u64 entry = pts->entry;	/* [한국어] 읽어 둔 항목 값 */
	pt_oaddr_t oa;	/* [한국어] 꺼낼 출력 주소 */

	if (pts_feature(pts, PT_FEAT_AMDV1_ENCRYPT_TABLES))	/* [한국어] 암호화 비트를 얹은 형식이면 */
		entry = __sme_clr(entry);	/* [한국어] 벗겨 낸다 */
	oa = FIELD_GET(AMDV1PT_FMT_OA, entry);	/* [한국어] 주소 필드(페이지 번호) */

	if (FIELD_GET(AMDV1PT_FMT_NEXT_LEVEL, entry) == AMDV1PT_FMT_NL_SIZE) {	/* [한국어] 크기가 주소 안에 인코딩된 큰 페이지면 */
		unsigned int sz_bits = oaffz(oa);	/* [한국어] 하위의 연속된 1 이 크기를 나타낸다 */

		oa = oalog2_set_mod(oa, 0, sz_bits);	/* [한국어] 그 자리를 0 으로 깎아야 진짜 시작 주소다 */
	} else if (PT_WARN_ON(FIELD_GET(AMDV1PT_FMT_NEXT_LEVEL, entry) !=	/* [한국어] 0 도 7 도 아니면 */
			      AMDV1PT_FMT_NL_DEFAULT))	/* [한국어] 표 항목이라 여기 올 수 없다 */
		return 0;	/* [한국어] 호출 경로가 잘못됐다 */
	return oalog2_mul(oa, PT_GRANULE_LG2SZ);	/* [한국어] 페이지 번호를 주소로 */
}
#define pt_entry_oa amdv1pt_entry_oa	/* [한국어] 이쪽을 구현하면 pt_item_oa 는 기본이 만들어 준다 */

/*
 * [한국어]
 * amdv1pt_can_have_leaf - 이 단계에 잎을 놓을 수 있는가
 *
 * @pts: 현재 단계.
 * @return: 최상위가 아니면 참.
 *
 * 원 주석이 근거를 든다 — 명세의 "Table 15: Page Table Level Parameters"가
 * 최상위 단계에 변환 항목을 두지 못하게 한다.
 */
static inline bool amdv1pt_can_have_leaf(const struct pt_state *pts)
{
	/*
	 * Table 15: Page Table Level Parameters
	 * The top most level cannot have translation entries
	 */
	return pts->level < PT_MAX_TOP_LEVEL;	/* [한국어] (원 주석: 명세의 Table 15 — 최상위 단계에는 변환 항목을 둘 수 없다) */
}
#define pt_can_have_leaf amdv1pt_can_have_leaf	/* [한국어] 공통 API 이름으로 */

/* Body in pt_fmt_defaults.h */
/*
 * [한국어] (원 주석: 몸통은 pt_fmt_defaults.h 에 있다)
 * 아래 함수들이 먼저 쓰므로 여기서 선언만 해 둔다.
 */
static inline unsigned int pt_table_item_lg2sz(const struct pt_state *pts);

/*
 * [한국어]
 * amdv1pt_entry_num_contig_lg2 - 이 잎이 몇 개의 항목으로 이루어졌는가
 *
 * @pts: 볼 항목.
 * @return: 항목 수의 지수.
 *
 * 이 형식의 가장 독특한 부분이다. 큰 페이지의 크기가 별도 필드가 아니라
 * 출력 주소의 하위 비트에 "1 이 몇 개 연달아 서 있는가"로 적혀 있다.
 *
 * 원 주석의 수식이 그 역산을 보인다. 설치할 때 크기에서 코드를 만들었으니,
 * 읽을 때는 처음 나오는 0 비트의 위치를 찾아 크기를 되짚는다.
 *
 * 마지막의 시프트는 그 ffz 를 한 번에 끝내려는 최적화다 — 마스크로 필드를
 * 꺼낸 뒤 빼는 대신, 뺄 값만큼 미리 시프트해 두면 ffz 결과가 바로 답이 된다.
 */
static inline unsigned int
amdv1pt_entry_num_contig_lg2(const struct pt_state *pts)
{
	u32 code;	/* [한국어] 크기가 인코딩된 비트열 */

	if (FIELD_GET(AMDV1PT_FMT_NEXT_LEVEL, pts->entry) ==	/* [한국어] 다음 단계 코드가 */
	    AMDV1PT_FMT_NL_DEFAULT)	/* [한국어] 0 이면 단일 항목이다 */
		return ilog2(1);	/* [한국어] 묶음이 아니다 */

	PT_WARN_ON(FIELD_GET(AMDV1PT_FMT_NEXT_LEVEL, pts->entry) !=	/* [한국어] 그 밖에는 7 이어야 한다 */
		   AMDV1PT_FMT_NL_SIZE);	/* [한국어] 표 항목이면 여기 올 수 없다 */

	/*
	 * The contiguous size is encoded in the length of a string of 1's in
	 * the low bits of the OA. Reverse the equation:
	 *  code = log2_to_int(num_contig_lg2 + item_lg2sz -
	 *              PT_GRANULE_LG2SZ - 1) - 1
	 * Which can be expressed as:
	 *  num_contig_lg2 = oalog2_ffz(code) + 1 -
	 *              item_lg2sz - PT_GRANULE_LG2SZ
	 *
	 * Assume the bit layout is correct and remove the masking. Reorganize
	 * the equation to move all the arithmetic before the ffz.
	 */
	code = pts->entry >> (__bf_shf(AMDV1PT_FMT_OA) - 1 +	/* [한국어] (원 주석: 마스킹을 생략하고 산술을 ffz 앞으로 몰아넣는다) */
			      pt_table_item_lg2sz(pts) - PT_GRANULE_LG2SZ);	/* [한국어] 뺄 값만큼 미리 시프트해 두면 */
	return ffz_t(u32, code);	/* [한국어] 처음 나오는 0 비트의 위치가 바로 답이 된다 */
}
#define pt_entry_num_contig_lg2 amdv1pt_entry_num_contig_lg2	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_num_items_lg2 - 이 단계 표의 항목 수(지수)
 *
 * @pts: 현재 단계.
 * @return: 늘 9(4KB 표에 8바이트 항목이면 512개).
 *
 * 원 주석이 5단계의 예외를 짚는다 — 그 단계는 주소의 [63:57] 만 해석하므로
 * 항목이 128개뿐이지만, 그 사정은 max_vasz_lg2 가 따로 처리하므로 여기서는
 * 불리지 않는다.
 */
static inline unsigned int amdv1pt_num_items_lg2(const struct pt_state *pts)
{
	/*
	 * Top entry covers bits [63:57] only, this is handled through
	 * max_vasz_lg2.
	 */
	if (PT_WARN_ON(pts->level == 5))	/* [한국어] (원 주석: 최상위는 [63:57] 만 덮으며 그 사정은 max_vasz_lg2 가 처리한다) */
		return 7;	/* [한국어] 여기서는 불리지 않는다 */
	return PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64));	/* [한국어] 4KB 표에 8바이트 항목 → 512개 */
}
#define pt_num_items_lg2 amdv1pt_num_items_lg2	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_possible_sizes - 이 단계가 만들 수 있는 페이지 크기들
 *
 * @pts: 현재 단계.
 * @return: 크기 비트맵.
 *
 * 크기를 주소 안에 인코딩하는 방식 덕분에, 이 단계의 항목 크기부터 표
 * 전체 크기까지 모든 2의 거듭제곱을 쓸 수 있다 — 다른 형식들이 단계
 * 경계의 몇 가지 크기만 쓰는 것과 대조적이다.
 *
 * 두 가지 제한이 붙는다. 인코딩이 51비트까지만 표현되고, 512GB 페이지는
 * 하드웨어 결함으로 쓸 수 없다.
 */
static inline pt_vaddr_t amdv1pt_possible_sizes(const struct pt_state *pts)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 이 단계의 항목 크기 */

	if (!amdv1pt_can_have_leaf(pts))	/* [한국어] 잎을 놓을 수 없는 단계면 */
		return 0;	/* [한국어] 만들 수 있는 크기가 없다 */

	/*
	 * Table 14: Example Page Size Encodings
	 * Address bits 51:32 can be used to encode page sizes greater than 4
	 * Gbytes. Address bits 63:52 are zero-extended.
	 *
	 * 512GB Pages are not supported due to a hardware bug.
	 * Otherwise every power of two size is supported.
	 */
	return GENMASK_ULL(min(51, isz_lg2 + amdv1pt_num_items_lg2(pts) - 1),	/* [한국어] (원 주석: Table 14 — 51:32 로 4GB 초과 크기를 인코딩하고 63:52 는 0 확장) */
			   isz_lg2) & ~SZ_512G;	/* [한국어] (원 주석: 512GB 페이지는 하드웨어 결함으로 쓸 수 없다) */
}
#define pt_possible_sizes amdv1pt_possible_sizes	/* [한국어] 여러 연속 크기를 지원하므로 기본 구현을 쓰지 않는다 */

/*
 * [한국어]
 * amdv1pt_load_entry_raw - 항목을 읽고 종류를 가른다
 *
 * @pts: 읽을 위치.
 * @return: 비었는가, 표인가, 출력 주소인가.
 *
 * 존재 비트가 0 이면 빈 항목이다. 그렇지 않으면 다음 단계 코드로 가른다:
 * 0 은 단일 항목 잎, 7 은 크기가 인코딩된 잎, 그 밖은 그 번호의 표다.
 *
 * 0단계를 따로 보는 이유: 그 단계에는 아래 표가 없으므로 코드가 무엇이든
 * 잎으로 해석해야 한다.
 */
static inline enum pt_entry_type amdv1pt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 읽을 항목의 주소 */
	unsigned int next_level;	/* [한국어] 다음 단계 코드 */
	u64 entry;	/* [한국어] 읽은 값 */

	pts->entry = entry = READ_ONCE(*tablep);	/* [한국어] 한 번만 읽어 담는다 — 여러 번 읽으면 값이 섞인다 */
	if (!(entry & AMDV1PT_FMT_PR))	/* [한국어] 존재 비트가 0 이면 */
		return PT_ENTRY_EMPTY;	/* [한국어] 매핑이 없다 */

	next_level = FIELD_GET(AMDV1PT_FMT_NEXT_LEVEL, pts->entry);	/* [한국어] 0 은 단일 잎, 7 은 크기 인코딩 잎, 그 밖은 표 */
	if (pts->level == 0 || next_level == AMDV1PT_FMT_NL_DEFAULT ||	/* [한국어] 0단계에는 아래 표가 없어 코드와 무관하게 잎이고 */
	    next_level == AMDV1PT_FMT_NL_SIZE)	/* [한국어] 두 잎 코드도 마찬가지 */
		return PT_ENTRY_OA;	/* [한국어] 출력 주소를 낸다 */
	return PT_ENTRY_TABLE;	/* [한국어] 그 번호의 아래 표를 가리킨다 */
}
#define pt_load_entry_raw amdv1pt_load_entry_raw	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_install_leaf_entry - 표에 잎 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @oa: 출력 주소.
 * @oasz_lg2: 이 잎이 덮는 크기의 지수.
 * @attrs: 얹을 권한 비트.
 *
 * 단일 항목이면 한 번 쓰면 끝이다. 연속 항목이면 같은 값을 여러 자리에
 * 채워야 하는데, 그 값에 크기 인코딩이 들어간다 — 하위 비트에 1 을
 * 연달아 세운 형태다.
 *
 * 32개를 경계로 채우는 방법을 바꾸는 것이 눈에 띈다. 아래
 * amdv1pt_clear_entries 의 원 주석이 이유를 밝힌다 — rep 계열 명령은
 * 시작 비용이 있어 작은 경우에는 단순 반복이 빠르다.
 */
static __always_inline void
amdv1pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			   unsigned int oasz_lg2,
			   const struct pt_write_attrs *attrs)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);	/* [한국어] 이 단계의 항목 크기 */
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 쓸 자리 */
	u64 entry;	/* [한국어] 만들 항목 값 */

	if (!pt_check_install_leaf_args(pts, oa, oasz_lg2))	/* [한국어] 정렬과 크기를 먼저 확인 */
		return;	/* [한국어] 어긋나면 쓰지 않는다 */

	entry = AMDV1PT_FMT_PR |	/* [한국어] 존재 비트 */
		FIELD_PREP(AMDV1PT_FMT_OA, log2_div(oa, PT_GRANULE_LG2SZ)) |	/* [한국어] 주소를 페이지 번호로 */
		attrs->descriptor_bits;	/* [한국어] 권한과 속성 */

	if (oasz_lg2 == isz_lg2) {	/* [한국어] 단일 항목이면 */
		entry |= FIELD_PREP(AMDV1PT_FMT_NEXT_LEVEL,	/* [한국어] 다음 단계 코드를 */
				    AMDV1PT_FMT_NL_DEFAULT);	/* [한국어] 0 으로 — 평범한 잎 */
		WRITE_ONCE(*tablep, entry);	/* [한국어] 한 번 쓰면 끝 */
	} else {
		unsigned int num_contig_lg2 = oasz_lg2 - isz_lg2;	/* [한국어] 묶을 항목 수의 지수 */
		u64 *end = tablep + log2_to_int(num_contig_lg2);	/* [한국어] 채울 마지막 다음 자리 */

		entry |= FIELD_PREP(AMDV1PT_FMT_NEXT_LEVEL,	/* [한국어] 다음 단계 코드를 */
				    AMDV1PT_FMT_NL_SIZE) |	/* [한국어] 7 로 — 크기가 주소에 인코딩된 잎 */
			 FIELD_PREP(AMDV1PT_FMT_OA,	/* [한국어] 주소 필드의 하위에 */
				    oalog2_to_int(oasz_lg2 - PT_GRANULE_LG2SZ -	/* [한국어] 크기만큼의 1 을 세운다 */
						  1) -	/* [한국어] entry_num_contig_lg2 가 이 식을 역산한다 */
					    1);	/* [한국어] 연속된 1 의 길이가 곧 크기다 */

		/* See amdv1pt_clear_entries() */
		if (num_contig_lg2 <= ilog2(32)) {	/* [한국어] (원 주석: amdv1pt_clear_entries() 참고) */
			for (; tablep != end; tablep++)	/* [한국어] 작은 묶음은 */
				WRITE_ONCE(*tablep, entry);	/* [한국어] 단순 반복이 빠르다 */
		} else {
			memset64(tablep, entry, log2_to_int(num_contig_lg2));	/* [한국어] 큰 묶음은 rep 계열이 빠르다 */
		}
	}
	pts->entry = entry;	/* [한국어] 순회 상태도 새 값으로 */
}
#define pt_install_leaf_entry amdv1pt_install_leaf_entry	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_install_table - 아래 표를 가리키는 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @table_pa: 아래 표의 물리 주소.
 * @attrs: 속성(이 형식에서는 쓰지 않는다).
 * @return: 내가 꽂았으면 참.
 *
 * 표 항목에 늘 읽기·쓰기를 허용하는 것이 이 형식의 규칙이다. 원 주석이
 * 이유를 밝힌다 — 하드웨어가 경로상의 모든 단계와 잎의 권한을 AND 하므로,
 * 중간 단계가 권한을 좁히면 잎의 설정을 덮어써 버린다. 권한은 잎에서만
 * 정한다.
 *
 * cmpxchg 로 쓰는 이유는 pt_defs.h 의 pt_table_install64 참고 — 여러
 * 스레드가 같은 자리에 각자 만든 표를 꽂으려 경쟁할 수 있다.
 */
static inline bool amdv1pt_install_table(struct pt_state *pts,
					 pt_oaddr_t table_pa,
					 const struct pt_write_attrs *attrs)
{
	u64 entry;	/* [한국어] 만들 항목 값 */

	/*
	 * IR and IW are ANDed from the table levels along with the PTE. We
	 * always control permissions from the PTE, so always set IR and IW for
	 * tables.
	 */
	entry = AMDV1PT_FMT_PR |	/* [한국어] (원 주석: IR/IW 는 단계마다 AND 되므로 권한은 잎에서만 정한다) */
		FIELD_PREP(AMDV1PT_FMT_NEXT_LEVEL, pts->level) |	/* [한국어] 아래 표의 단계 번호 — 하드웨어가 몇 단계를 더 걸을지 여기서 안다 */
		FIELD_PREP(AMDV1PT_FMT_OA,	/* [한국어] 아래 표의 주소를 */
			   log2_div(table_pa, PT_GRANULE_LG2SZ)) |	/* [한국어] 페이지 번호로 */
		AMDV1PT_FMT_IR | AMDV1PT_FMT_IW;	/* [한국어] 표 단계는 늘 열어 둔다 — 좁히면 잎의 권한을 덮어쓴다 */
	if (pts_feature(pts, PT_FEAT_AMDV1_ENCRYPT_TABLES))	/* [한국어] SME 환경이면 */
		entry = __sme_set(entry);	/* [한국어] 하드웨어가 암호화된 메모리로 읽게 한다 */
	return pt_table_install64(pts, entry);	/* [한국어] 경합에서 지면 거짓 — 진 쪽이 자기 표를 버린다 */
}
#define pt_install_table amdv1pt_install_table	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_attr_from_entry - 항목에서 권한 비트만 다시 꺼낸다
 *
 * @pts: 읽을 항목.
 * @attrs: 채울 결과.
 *
 * 강제 일관(FC)과 읽기·쓰기만 남긴다. 주소와 크기 인코딩은 새 항목을
 * 만들 때 다시 계산되므로 옮기지 않는다.
 */
static inline void amdv1pt_attr_from_entry(const struct pt_state *pts,
					   struct pt_write_attrs *attrs)
{
	attrs->descriptor_bits =	/* [한국어] 새 항목에 그대로 옮길 비트만 */
		pts->entry & (AMDV1PT_FMT_FC | AMDV1PT_FMT_IR | AMDV1PT_FMT_IW);	/* [한국어] 강제 일관과 읽기·쓰기 — 주소와 크기는 다시 계산된다 */
}
#define pt_attr_from_entry amdv1pt_attr_from_entry	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_clear_entries - 연속된 항목들을 0 으로 덮는다
 *
 * @pts: 시작 위치.
 * @num_contig_lg2: 지울 개수의 지수.
 *
 * 원 주석이 32개 경계의 근거를 든다: gcc 가 만드는 rep stos 는 시작
 * 비용이 있어 작은 경우에 느리고, 큰 연속 페이지에서는 반대로 빠르다.
 * 마이크로벤치마크에서 그 차이가 드러난 결과다.
 */
static inline void amdv1pt_clear_entries(struct pt_state *pts,
					 unsigned int num_contig_lg2)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 지울 첫 자리 */
	u64 *end = tablep + log2_to_int(num_contig_lg2);	/* [한국어] 그 다음 자리 */

	/*
	 * gcc generates rep stos for the io-pgtable code, and this difference
	 * can show in microbenchmarks with larger contiguous page sizes.
	 * rep is slower for small cases.
	 */
	if (num_contig_lg2 <= ilog2(32)) {	/* [한국어] (원 주석: rep stos 는 시작 비용이 있어 작은 경우에 느리다) */
		for (; tablep != end; tablep++)	/* [한국어] 작은 묶음은 */
			WRITE_ONCE(*tablep, 0);	/* [한국어] 단순 반복으로 */
	} else {
		memset64(tablep, 0, log2_to_int(num_contig_lg2));	/* [한국어] 큰 묶음은 rep 계열로 */
	}
}
#define pt_clear_entries amdv1pt_clear_entries	/* [한국어] 기본 구현 대신 memset64 최적화를 쓴다 */

/*
 * [한국어]
 * amdv1pt_entry_is_write_dirty - 이 entry 에 쓰기가 있었는가
 *
 * @pts: 볼 항목.
 * @return: 묶음 중 하나라도 표시가 있으면 참.
 *
 * 하드웨어는 실제로 접근한 item 에만 표시를 남기므로, 묶음 전체를 훑어야
 * 한다. 색인을 묶음 시작으로 깎고 시작하는 이유가 그것이다 — 순회가
 * 묶음 한가운데를 가리키고 있을 수 있다.
 */
static inline bool amdv1pt_entry_is_write_dirty(const struct pt_state *pts)
{
	unsigned int num_contig_lg2 = amdv1pt_entry_num_contig_lg2(pts);	/* [한국어] 묶음 크기 */
	u64 *tablep = pt_cur_table(pts, u64) +	/* [한국어] 묶음의 시작으로 색인을 깎는다 */
		      log2_set_mod(pts->index, 0, num_contig_lg2);	/* [한국어] 순회가 한가운데를 가리킬 수 있다 */
	u64 *end = tablep + log2_to_int(num_contig_lg2);	/* [한국어] 묶음의 끝 */

	for (; tablep != end; tablep++)	/* [한국어] 하드웨어는 실제로 접근한 item 에만 표시를 남긴다 */
		if (READ_ONCE(*tablep) & AMDV1PT_FMT_D)	/* [한국어] 하나라도 있으면 */
			return true;	/* [한국어] 이 entry 는 더티다 */
	return false;	/* [한국어] 아무도 쓰지 않았다 */
}
#define pt_entry_is_write_dirty amdv1pt_entry_is_write_dirty	/* [한국어] 더티 추적을 지원한다는 신호이기도 하다 */

/*
 * [한국어]
 * amdv1pt_entry_make_write_clean - 묶음의 더티 표시를 모두 지운다
 *
 * @pts: 고칠 항목.
 *
 * 읽고-고치고-쓰기를 원자 연산 없이 한다. 이 비트만 내리는 것이고, 그
 * 직후의 하드웨어 쓰기를 놓치는 것은 pt_common.h 가 명시한 계약 안에
 * 있다 — TLB 를 비운 뒤부터 세는 것이 규칙이다.
 */
static inline void amdv1pt_entry_make_write_clean(struct pt_state *pts)
{
	unsigned int num_contig_lg2 = amdv1pt_entry_num_contig_lg2(pts);	/* [한국어] 묶음 크기 */
	u64 *tablep = pt_cur_table(pts, u64) +	/* [한국어] 묶음의 시작으로 */
		      log2_set_mod(pts->index, 0, num_contig_lg2);	/* [한국어] 색인을 깎는다 */
	u64 *end = tablep + log2_to_int(num_contig_lg2);	/* [한국어] 묶음의 끝 */

	for (; tablep != end; tablep++)	/* [한국어] 묶음의 모든 item 에서 */
		WRITE_ONCE(*tablep, READ_ONCE(*tablep) & ~(u64)AMDV1PT_FMT_D);	/* [한국어] 더티 비트만 내린다 — 직후의 쓰기를 놓치는 것은 계약 안이다 */
}
#define pt_entry_make_write_clean amdv1pt_entry_make_write_clean	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_entry_make_write_dirty - 소프트웨어가 더티 표시를 남긴다
 *
 * @pts: 고칠 항목.
 * @return: 성공하면 참.
 *
 * cmpxchg 인 이유: 하드웨어가 같은 항목을 동시에 갱신할 수 있어, 통째로
 * 덮어쓰면 그 갱신을 잃는다. 실패하면 그사이 무언가 바뀐 것이므로
 * 호출자가 다시 판단한다.
 */
static inline bool amdv1pt_entry_make_write_dirty(struct pt_state *pts)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 고칠 항목 */
	u64 new = pts->entry | AMDV1PT_FMT_D;	/* [한국어] 더티 비트만 얹은 값 */

	return try_cmpxchg64(tablep, &pts->entry, new);	/* [한국어] 하드웨어가 동시에 갱신할 수 있어 통째로 덮어쓰면 안 된다 */
}
#define pt_entry_make_write_dirty amdv1pt_entry_make_write_dirty	/* [한국어] 공통 API 이름으로 */

/* --- iommu */
#include <linux/generic_pt/iommu.h>	/* [한국어] (원 주석: 여기부터 IOMMU 계층과의 접점) 공통 IOMMU 객체 */
#include <linux/iommu.h>	/* [한국어] IOMMU_READ/WRITE/MMIO 권한 상수 */

#define pt_iommu_table pt_iommu_amdv1	/* [한국어] 공통 코드가 부르는 이름을 이 형식의 구조체로 */

/* The common struct is in the per-format common struct */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * common_from_iommu - IOMMU 객체에서 공통 페이지 테이블 상태로
 *
 * @iommu_table: IOMMU 계층의 객체.
 * @return: 그 안에 박힌 pt_common.
 *
 * 두 겹을 거친다 — 형식별 구조체를 지나 그 안의 공통 부분으로. 그래서
 * 공통 코드가 형식을 몰라도 상태에 닿을 수 있다.
 */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_amdv1, iommu)	/* [한국어] IOMMU 객체에서 형식별 구조체로 */
			->amdpt.common;	/* [한국어] 그 안의 공통 부분으로 */
}

/*
 * [한국어]
 * iommu_from_common - 공통 상태에서 IOMMU 객체로 되돌아간다
 *
 * @common: 공통 페이지 테이블 상태.
 * @return: 그것을 품은 IOMMU 객체.
 *
 * 위 함수의 역이다. 공통 코드가 드라이버 콜백을 불러야 할 때 쓴다.
 */
static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_amdv1, amdpt.common)->iommu;	/* [한국어] 공통 상태에서 IOMMU 객체로 되돌아간다 */
}

/*
 * [한국어]
 * amdv1pt_iommu_set_prot - IOMMU 권한을 항목 비트로 옮긴다
 *
 * @common: 페이지 테이블 인스턴스.
 * @attrs: 채울 쓰기 속성.
 * @iommu_prot: IOMMU_READ/WRITE/MMIO 조합.
 * @return: 늘 0.
 *
 * 공통 권한과 형식 비트 사이의 번역기다. 이 결과가 잎 항목에 그대로
 * 얹힌다.
 *
 * SME 처리에 대해 원 주석이 아쉬움을 남긴다 — 원래는 상위 계층이
 * IOMMU_ENCRYPTED 같은 플래그로 정해 주는 편이 옳지만, 지금은 표에
 * 암호화를 쓰면 항목에도 쓴다는 규칙으로 대신한다. MMIO 매핑만 예외인데,
 * 장치 레지스터는 암호화 대상이 아니기 때문이다.
 */
static inline int amdv1pt_iommu_set_prot(struct pt_common *common,
					 struct pt_write_attrs *attrs,
					 unsigned int iommu_prot)
{
	u64 pte = 0;	/* [한국어] 만들 속성 비트 */

	if (pt_feature(common, PT_FEAT_AMDV1_FORCE_COHERENCE))	/* [한국어] 일관성 강제 기능이 켜져 있으면 */
		pte |= AMDV1PT_FMT_FC;	/* [한국어] 장치의 비일관 접근을 하드웨어가 무시한다 */
	if (iommu_prot & IOMMU_READ)	/* [한국어] 읽기 허용이면 */
		pte |= AMDV1PT_FMT_IR;	/* [한국어] 항목의 읽기 비트 */
	if (iommu_prot & IOMMU_WRITE)	/* [한국어] 쓰기 허용이면 */
		pte |= AMDV1PT_FMT_IW;	/* [한국어] 항목의 쓰기 비트 */

	/*
	 * Ideally we'd have an IOMMU_ENCRYPTED flag set by higher levels to
	 * control this. For now if the tables use sme_set then so do the ptes.
	 */
	if (pt_feature(common, PT_FEAT_AMDV1_ENCRYPT_TABLES) &&	/* [한국어] (원 주석: 원래는 상위가 IOMMU_ENCRYPTED 로 정해 주는 편이 옳다) */
	    !(iommu_prot & IOMMU_MMIO))	/* [한국어] 장치 레지스터는 암호화 대상이 아니다 */
		pte = __sme_set(pte);	/* [한국어] 그 밖에는 표와 같은 규칙을 따른다 */

	attrs->descriptor_bits = pte;	/* [한국어] 잎을 쓸 때 이 값이 그대로 얹힌다 */
	return 0;	/* [한국어] 실패할 수 없는 경로 */
}
#define pt_iommu_set_prot amdv1pt_iommu_set_prot	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * amdv1pt_iommu_fmt_init - 형식별 초기화
 *
 * @iommu_table: 초기화할 객체.
 * @cfg: 드라이버가 준 설정.
 * @return: 0 성공, -EINVAL 이면 시작 단계가 범위 밖이다.
 *
 * 주소 공간 폭을 정하는 것이 주된 일이다. 최상위가 자랄 수 있는 형식이면
 * 형식의 최대치를 쓰지만, 자라지 않는데 시작 단계가 낮으면 그 단계가
 * 실제로 덮는 만큼으로 좁혀야 한다 — 그러지 않으면 표 밖의 주소를
 * 유효하다고 받아들인다.
 *
 * 출력 주소 폭은 형식 한계와 하드웨어 한계 중 작은 쪽을 쓴다.
 *
 * 호출 체인:
 *   pt_iommu_amdv1_init() → [이 함수] → pt_top_set_level()
 */
static inline int amdv1pt_iommu_fmt_init(struct pt_iommu_amdv1 *iommu_table,
					 const struct pt_iommu_amdv1_cfg *cfg)
{
	struct pt_amdv1 *table = &iommu_table->amdpt;	/* [한국어] 형식별 페이지 테이블 상태 */
	unsigned int max_vasz_lg2 = PT_MAX_VA_ADDRESS_LG2;	/* [한국어] 일단 형식의 최대 폭 */

	if (cfg->starting_level == 0 || cfg->starting_level > PT_MAX_TOP_LEVEL)	/* [한국어] 0단계만으로는 표가 성립하지 않고 */
		return -EINVAL;	/* [한국어] 최대 단계를 넘을 수도 없다 */

	if (!pt_feature(&table->common, PT_FEAT_DYNAMIC_TOP) &&	/* [한국어] 최상위가 자라지 않는데 */
	    cfg->starting_level != PT_MAX_TOP_LEVEL)	/* [한국어] 시작 단계가 낮으면 */
		max_vasz_lg2 = PT_GRANULE_LG2SZ +	/* [한국어] 그 단계가 실제로 덮는 만큼으로 좁힌다 */
			       (PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64))) *	/* [한국어] 단계당 비트 수에 */
				       (cfg->starting_level + 1);	/* [한국어] 단계 수를 곱한다 — 넓게 두면 표 밖 주소를 받아들인다 */

	table->common.max_vasz_lg2 =	/* [한국어] 입력 주소 폭은 */
		min(max_vasz_lg2, cfg->common.hw_max_vasz_lg2);	/* [한국어] 형식과 하드웨어 중 작은 쪽 */
	table->common.max_oasz_lg2 =	/* [한국어] 출력 주소 폭도 */
		min(PT_MAX_OUTPUT_ADDRESS_LG2, cfg->common.hw_max_oasz_lg2);	/* [한국어] 마찬가지 */
	pt_top_set_level(&table->common, cfg->starting_level);	/* [한국어] 표는 첫 매핑이 만든다 */
	return 0;	/* [한국어] 성공 */
}
#define pt_iommu_fmt_init amdv1pt_iommu_fmt_init	/* [한국어] 공통 API 이름으로 */

#ifndef PT_FMT_VARIANT	/* [한국어] 시험용 변종이 아닐 때만 */
/*
 * [한국어]
 * amdv1pt_iommu_fmt_hw_info - 하드웨어에 적을 값을 꺼낸다
 *
 * @table: 페이지 테이블 객체.
 * @top_range: 최상위 정보를 담은 범위.
 * @info: 채울 결과.
 *
 * 최상위 표의 물리 주소와 단계 수를 꺼낸다. drivers/iommu/amd 의
 * amd_iommu_set_dte_v1() 이 이 둘을 DTE 에 적어 하드웨어가 표를 걷기
 * 시작한다 — generic_pt 와 벤더 드라이버가 만나는 지점이다.
 *
 * 단계 수에 1 을 더하는 이유: DTE 의 모드 필드는 1 기반이고 이 계층의
 * 단계 번호는 0 기반이다(파일 상단의 원 주석이 그 대응을 밝힌다).
 *
 * 시험용 변종에서는 빌드하지 않는다 — 그쪽은 진짜 DTE 에 값을 적지 않는다.
 */
static inline void
amdv1pt_iommu_fmt_hw_info(struct pt_iommu_amdv1 *table,
			  const struct pt_range *top_range,
			  struct pt_iommu_amdv1_hw_info *info)
{
	info->host_pt_root = virt_to_phys(top_range->top_table);	/* [한국어] DTE 에 적을 최상위 표의 물리 주소 */
	PT_WARN_ON(info->host_pt_root & ~PT_TOP_PHYS_MASK);	/* [한국어] DTE 의 주소 필드에 담기지 않는 비트가 있으면 안 된다 */
	info->mode = top_range->top_level + 1;	/* [한국어] DTE 의 모드는 1 기반, 이 계층의 단계는 0 기반 */
}
#define pt_iommu_fmt_hw_info amdv1pt_iommu_fmt_hw_info	/* [한국어] 공통 API 이름으로 */
#endif

#if defined(GENERIC_PT_KUNIT)	/* [한국어] 시험 모듈을 빌드하는 중이면 */
/*
 * [한국어] kunit 이 시험할 설정 목록.
 * 지금은 하나뿐이며, 원 주석대로 기존 io_pgtable 구현이 쓰던 시작 단계를
 * 그대로 골라 두 구현의 동작을 견줄 수 있게 했다.
 */
static const struct pt_iommu_amdv1_cfg amdv1_kunit_fmt_cfgs[] = {
	/* Matches what io_pgtable does */
	[0] = { .starting_level = 2 },	/* [한국어] (원 주석: io_pgtable 구현이 쓰던 값과 같게 맞춘다) */
};
#define kunit_fmt_cfgs amdv1_kunit_fmt_cfgs	/* [한국어] 시험 코드가 부르는 이름으로 */
enum { KUNIT_FMT_FEATURES = 0 };	/* [한국어] 시험에서 추가로 켤 기능이 없다 */
#endif	/* [한국어] 포함 방지 끝 */

#endif
