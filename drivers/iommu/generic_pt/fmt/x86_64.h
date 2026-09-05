/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * x86 page table. Supports the 4 and 5 level variations.
 *
 * The 4 and 5 level version is described in:
 *   Section "4.4 4-Level Paging and 5-Level Paging" of the Intel Software
 *   Developer's Manual Volume 3
 *
 *   Section "9.7 First-Stage Paging Entries" of the "Intel Virtualization
 *   Technology for Directed I/O Architecture Specification"
 *
 *   Section "2.2.6 I/O Page Tables for Guest Translations" of the "AMD I/O
 *   Virtualization Technology (IOMMU) Specification"
 *
 * It is used by x86 CPUs, AMD and VT-d IOMMU HW.
 *
 * Note the 3 level format is very similar and almost implemented here. The
 * reserved/ignored layout is different and there are functional bit
 * differences.
 *
 * This format uses PT_FEAT_SIGN_EXTEND to have a upper/non-canonical/lower
 * split. PT_FEAT_SIGN_EXTEND is optional as AMD IOMMU sometimes uses non-sign
 * extended addressing with this page table format.
 *
 * The named levels in the spec map to the pts->level as:
 *   Table/PTE - 0
 *   Directory/PDE - 1
 *   Directory Ptr/PDPTE - 2
 *   PML4/PML4E - 3
 *   PML5/PML5E - 4
 */
/*
 * [한국어 설명] x86-64 페이지 테이블 형식 구현 (x86_64.h)
 *
 * === 파일의 역할 ===
 * CPU 가 쓰는 것과 같은 4·5단계 페이지 테이블을 generic_pt 의 형식 API 로
 * 감싼다. 원 주석이 세 출처를 든다 — 인텔 SDM 3권, VT-d 명세의 1단계 항목,
 * AMD 명세의 게스트 변환. 세 곳이 같은 형식을 기술하는 이유는 그것이
 * CPU 형식 그대로이기 때문이다.
 *
 * 그 사실이 이 형식의 존재 이유이기도 하다. 프로세스의 페이지 테이블을
 * 그대로 장치에 붙일 수 있어야 SVA 가 성립하는데, 그러려면 IOMMU 가 CPU 와
 * 똑같은 표를 걸어야 한다.
 *
 * 부호 확장이 선택 사항인 점이 눈에 띈다. CPU 에서는 상위 절반과 하위
 * 절반이 갈리지만, AMD IOMMU 는 PASID 0 에서 그 해석 없이 전 범위를 아래로
 * 쓴다(drivers/iommu/amd 의 amd_iommu_domain_alloc_paging_v2 참고).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_x86_64.c → iommu_template.h → defs_x86_64.h → pt_defs.h →
 *   [이 파일] → pt_common.h → iommu_pt.h → drivers/iommu/amd (v2 도메인)
 *
 * 단계 번호 대응은 파일 상단의 원 주석에 있다: PTE=0, PDE=1, PDPTE=2,
 * PML4E=3, PML5E=4.
 *
 * 실행 컨텍스트: 전부 인라인.
 *
 * === 타 모듈과의 연결 ===
 * 위: pt_common.h 의 API 선언, iommu_pt.h 의 매핑 경로.
 * 아래: <linux/mem_encrypt.h>(SME), <linux/bitfield.h>.
 * 파일 뒤쪽의 fmt_hw_info 가 drivers/iommu/amd 의 init_gcr3_table() 로
 * 값을 넘긴다 — GCR3 항목에 적힐 최상위 표 주소가 그것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * x86_64_pt_load_entry_raw: P 비트로 존재를, PS 비트로 잎 여부를 가른다.
 *   AMD v1 과 달리 크기 인코딩이 없어 훨씬 단순하다.
 * x86_64_pt_can_have_leaf: 2단계(1GB)까지만 큰 페이지를 허용한다.
 * x86_64_pt_sw_bit: 명세가 "무시됨"으로 남긴 비트들을 소프트웨어가 쓴다.
 *   흩어져 있어 번호를 실제 비트 위치로 옮기는 표가 필요하다.
 * x86_64_pt_iommu_set_prot: 읽기 전용이 따로 없어, 쓰기 권한이 RW 와 D 를
 *   함께 세운다.
 * x86_64_pt_iommu_fmt_hw_info: GCR3 에 적을 표 주소와 단계 수.
 */
#ifndef __GENERIC_PT_FMT_X86_64_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_FMT_X86_64_H	/* [한국어] 같은 이름으로 표시 */

#include "defs_x86_64.h"	/* [한국어] 이 형식의 주소 타입과 쓰기 속성 */
#include "../pt_defs.h"	/* [한국어] 순회 상태와 산술 */

#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP */
#include <linux/container_of.h>	/* [한국어] IOMMU 객체와 공통 상태를 오간다 */
#include <linux/log2.h>	/* [한국어] ilog2 — 표 항목 수 계산 */
#include <linux/mem_encrypt.h>	/* [한국어] __sme_set/__sme_clr */

/*
 * [한국어] 이 형식의 크기 상수들.
 * 5단계까지 지원하므로 입력 주소가 57비트, 출력은 52비트다.
 */
enum {
	PT_MAX_OUTPUT_ADDRESS_LG2 = 52,	/* [한국어] 출력 물리 주소 52비트 */
	PT_MAX_VA_ADDRESS_LG2 = 57,	/* [한국어] 5단계까지 지원하므로 입력은 57비트 */
	PT_ITEM_WORD_SIZE = sizeof(u64),	/* [한국어] 항목 하나가 8바이트 */
	PT_MAX_TOP_LEVEL = 4,	/* [한국어] PML5E 까지 — 0 기반이라 4 가 5단계다 */
	PT_GRANULE_LG2SZ = 12,	/* [한국어] 최소 페이지 4KB */
	PT_TABLEMEM_LG2SZ = 12,	/* [한국어] 표 하나가 4KB */

	/*
	 * For AMD the GCR3 Base only has these bits. For VT-d FSPTPTR is 4k
	 * aligned and is limited by the architected HAW
	 */
	PT_TOP_PHYS_MASK = GENMASK_ULL(51, 12),	/* [한국어] (원 주석: AMD 의 GCR3 Base 는 이 비트뿐이고, VT-d 는 4K 정렬에 HAW 제한을 받는다) */
};

/* Shared descriptor bits */
/*
 * [한국어] (위 영어 주석에 이어)
 * 모든 단계가 공유하는 항목 비트.
 * CPU 페이지 테이블과 같은 배치이므로 이름도 그대로 따랐다.
 */
enum {
	X86_64_FMT_P = BIT(0),	/* [한국어] 존재 — 0 이면 빈 항목 */
	X86_64_FMT_RW = BIT(1),	/* [한국어] 쓰기 허용. 없으면 읽기 전용이다 */
	X86_64_FMT_U = BIT(2),	/* [한국어] 사용자 권한 접근 허용 — 장치 접근이 여기 해당한다 */
	X86_64_FMT_A = BIT(5),	/* [한국어] 접근됨. 미리 세워 두면 하드웨어의 표 쓰기가 사라진다 */
	X86_64_FMT_D = BIT(6),	/* [한국어] 더티 — 쓰기가 있었다는 표시 */
	X86_64_FMT_OA = GENMASK_ULL(51, 12),	/* [한국어] 출력 주소(페이지 번호) */
	X86_64_FMT_XD = BIT_ULL(63),	/* [한국어] 실행 금지 */
};

/* PDPTE/PDE */
/*
 * [한국어] (위 영어 주석에 이어)
 * 중간 단계에서만 뜻이 있는 비트.
 * PS(Page Size)가 서 있으면 그 항목은 아래 표가 아니라 큰 페이지다 —
 * 0단계에는 아래 표가 없어 이 비트가 다른 뜻으로 쓰인다.
 */
enum {
	X86_64_FMT_PS = BIT(7),	/* [한국어] 아래 표가 아니라 큰 페이지라는 표시 */
};

/*
 * [한국어]
 * x86_64_pt_table_pa - 표 항목이 가리키는 아래 표의 물리 주소
 *
 * @pts: 볼 항목(PT_ENTRY_TABLE).
 * @return: 아래 표의 물리 주소.
 *
 * SME 비트를 벗겨야 진짜 주소가 된다 — 설치할 때 얹었기 때문이다.
 */
static inline pt_oaddr_t x86_64_pt_table_pa(const struct pt_state *pts)
{
	u64 entry = pts->entry;	/* [한국어] 읽어 둔 항목 값 */

	if (pts_feature(pts, PT_FEAT_X86_64_AMD_ENCRYPT_TABLES))	/* [한국어] SME 환경이면 */
		entry = __sme_clr(entry);	/* [한국어] 암호화 비트를 벗겨야 진짜 주소가 된다 */
	return oalog2_mul(FIELD_GET(X86_64_FMT_OA, entry),	/* [한국어] 주소는 페이지 번호로 저장되어 있다 */
			  PT_TABLEMEM_LG2SZ);	/* [한국어] 표 크기만큼 곱한다 */
}
#define pt_table_pa x86_64_pt_table_pa	/* [한국어] 공통 API 이름으로 잇는다 */

/*
 * [한국어]
 * x86_64_pt_entry_oa - 잎 항목이 내는 출력 주소
 *
 * @pts: 볼 항목.
 * @return: 출력 주소.
 *
 * AMD v1 과 달리 크기 인코딩이 없어 필드를 그대로 꺼내면 된다. 큰 페이지의
 * 크기는 그 항목이 있는 단계가 정한다.
 */
static inline pt_oaddr_t x86_64_pt_entry_oa(const struct pt_state *pts)
{
	u64 entry = pts->entry;	/* [한국어] 읽어 둔 항목 값 */

	if (pts_feature(pts, PT_FEAT_X86_64_AMD_ENCRYPT_TABLES))	/* [한국어] SME 환경이면 */
		entry = __sme_clr(entry);	/* [한국어] 암호화 비트를 벗긴다 */
	return oalog2_mul(FIELD_GET(X86_64_FMT_OA, entry),	/* [한국어] 크기 인코딩이 없어 필드를 그대로 */
			  PT_GRANULE_LG2SZ);	/* [한국어] 페이지 번호를 주소로 */
}
#define pt_entry_oa x86_64_pt_entry_oa	/* [한국어] 이쪽을 구현하면 pt_item_oa 는 기본이 만들어 준다 */

/*
 * [한국어]
 * x86_64_pt_can_have_leaf - 이 단계에 잎을 놓을 수 있는가
 *
 * @pts: 현재 단계.
 * @return: 2단계 이하면 참.
 *
 * 4KB(0단계), 2MB(1단계), 1GB(2단계)까지가 이 형식이 지원하는 페이지
 * 크기다. 그 위로는 하드웨어가 PS 비트를 해석하지 않는다.
 */
static inline bool x86_64_pt_can_have_leaf(const struct pt_state *pts)
{
	return pts->level <= 2;	/* [한국어] 4KB·2MB·1GB — 그 위로는 하드웨어가 PS 를 해석하지 않는다 */
}
#define pt_can_have_leaf x86_64_pt_can_have_leaf	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_num_items_lg2 - 이 단계 표의 항목 수(지수)
 *
 * @pts: 현재 단계.
 * @return: 늘 9(4KB 표에 8바이트 항목이면 512개).
 *
 * 모든 단계가 같아 단계를 보지 않는다 — 그것이 x86 페이지 테이블의 규칙성이다.
 */
static inline unsigned int x86_64_pt_num_items_lg2(const struct pt_state *pts)
{
	return PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64));	/* [한국어] 모든 단계가 같다 — 4KB 표에 8바이트 항목 */
}
#define pt_num_items_lg2 x86_64_pt_num_items_lg2	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_load_entry_raw - 항목을 읽고 종류를 가른다
 *
 * @pts: 읽을 위치.
 * @return: 비었는가, 표인가, 출력 주소인가.
 *
 * P 비트가 존재를, PS 비트가 잎 여부를 가른다. 0단계에는 아래 표가 없어
 * PS 를 보지 않고 곧바로 잎으로 판정한다 — 그 단계에서 같은 비트는 PAT 로
 * 쓰이므로 잘못 읽으면 안 된다.
 */
static inline enum pt_entry_type x86_64_pt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64);	/* [한국어] 이 단계의 표 */
	u64 entry;	/* [한국어] 읽은 값 */

	pts->entry = entry = READ_ONCE(tablep[pts->index]);	/* [한국어] 한 번만 읽어 담는다 */
	if (!(entry & X86_64_FMT_P))	/* [한국어] 존재 비트가 0 이면 */
		return PT_ENTRY_EMPTY;	/* [한국어] 매핑이 없다 */
	if (pts->level == 0 ||	/* [한국어] 0단계에는 아래 표가 없어 PS 를 보지 않는다 — 그 자리는 PAT 다 */
	    (x86_64_pt_can_have_leaf(pts) && (entry & X86_64_FMT_PS)))	/* [한국어] 큰 페이지를 허용하는 단계에서 PS 가 서 있으면 */
		return PT_ENTRY_OA;	/* [한국어] 잎이다 */
	return PT_ENTRY_TABLE;	/* [한국어] 그 밖에는 아래 표를 가리킨다 */
}
#define pt_load_entry_raw x86_64_pt_load_entry_raw	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_install_leaf_entry - 표에 잎 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @oa: 출력 주소.
 * @oasz_lg2: 이 잎이 덮는 크기의 지수.
 * @attrs: 얹을 권한 비트.
 *
 * 연속 항목이 없어 늘 한 번만 쓴다. 0단계가 아니면 PS 를 세워 "이것은 아래
 * 표가 아니라 큰 페이지"임을 알린다.
 */
static inline void
x86_64_pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			     unsigned int oasz_lg2,
			     const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);	/* [한국어] 이 단계의 표 */
	u64 entry;	/* [한국어] 만들 항목 값 */

	if (!pt_check_install_leaf_args(pts, oa, oasz_lg2))	/* [한국어] 정렬과 크기를 먼저 확인 */
		return;	/* [한국어] 어긋나면 쓰지 않는다 */

	entry = X86_64_FMT_P |	/* [한국어] 존재 비트 */
		FIELD_PREP(X86_64_FMT_OA, log2_div(oa, PT_GRANULE_LG2SZ)) |	/* [한국어] 주소를 페이지 번호로 */
		attrs->descriptor_bits;	/* [한국어] 권한과 속성 */
	if (pts->level != 0)	/* [한국어] 중간 단계면 */
		entry |= X86_64_FMT_PS;	/* [한국어] 아래 표가 아니라 큰 페이지임을 알린다 */

	WRITE_ONCE(tablep[pts->index], entry);	/* [한국어] 연속 항목이 없어 한 번만 쓴다 */
	pts->entry = entry;	/* [한국어] 순회 상태도 새 값으로 */
}
#define pt_install_leaf_entry x86_64_pt_install_leaf_entry	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_install_table - 아래 표를 가리키는 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @table_pa: 아래 표의 물리 주소.
 * @attrs: 속성(이 형식에서는 쓰지 않는다).
 * @return: 내가 꽂았으면 참.
 *
 * 표 단계에는 늘 RW·U·A 를 세운다. x86 은 경로상의 권한을 AND 하므로,
 * 중간 단계가 좁히면 잎의 설정을 덮어쓴다 — AMD v1 과 같은 이유다.
 *
 * A(Accessed)를 미리 세우는 이유: 하드웨어가 그 비트를 스스로 세우려면
 * 표에 쓰기를 해야 하는데, 미리 세워 두면 그 쓰기가 생략된다.
 */
static inline bool x86_64_pt_install_table(struct pt_state *pts,
					   pt_oaddr_t table_pa,
					   const struct pt_write_attrs *attrs)
{
	u64 entry;	/* [한국어] 만들 항목 값 */

	entry = X86_64_FMT_P | X86_64_FMT_RW | X86_64_FMT_U | X86_64_FMT_A |	/* [한국어] 권한을 AND 하므로 표 단계는 늘 열어 둔다. A 를 미리 세워 하드웨어의 표 쓰기를 없앤다 */
		FIELD_PREP(X86_64_FMT_OA, log2_div(table_pa, PT_GRANULE_LG2SZ));	/* [한국어] 아래 표의 주소를 페이지 번호로 */
	if (pts_feature(pts, PT_FEAT_X86_64_AMD_ENCRYPT_TABLES))	/* [한국어] SME 환경이면 */
		entry = __sme_set(entry);	/* [한국어] 하드웨어가 암호화된 메모리로 읽게 한다 */
	return pt_table_install64(pts, entry);	/* [한국어] 경합에서 지면 거짓 */
}
#define pt_install_table x86_64_pt_install_table	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_attr_from_entry - 항목에서 권한 비트만 다시 꺼낸다
 *
 * @pts: 읽을 항목.
 * @attrs: 채울 결과.
 *
 * 큰 페이지를 쪼갤 때 권한을 그대로 물려주기 위한 것이다. 주소와 PS 는
 * 새 항목을 만들 때 다시 정해지므로 옮기지 않는다.
 */
static inline void x86_64_pt_attr_from_entry(const struct pt_state *pts,
					     struct pt_write_attrs *attrs)
{
	attrs->descriptor_bits = pts->entry &	/* [한국어] 새 항목에 그대로 옮길 비트만 */
				 (X86_64_FMT_RW | X86_64_FMT_U | X86_64_FMT_A |	/* [한국어] 권한과 접근 표시 */
				  X86_64_FMT_D | X86_64_FMT_XD);	/* [한국어] 더티와 실행 금지 — 주소와 PS 는 다시 정해진다 */
}
#define pt_attr_from_entry x86_64_pt_attr_from_entry	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_max_sw_bit - 소프트웨어가 쓸 수 있는 비트의 개수
 *
 * @common: 무시된다.
 * @return: 12(0번부터 12번까지).
 *
 * 아래 x86_64_pt_sw_bit 이 옮겨 주는 번호의 상한이다.
 */
static inline unsigned int x86_64_pt_max_sw_bit(struct pt_common *common)
{
	return 12;	/* [한국어] 0번부터 12번까지 쓸 수 있다 */
}
#define pt_max_sw_bit x86_64_pt_max_sw_bit	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_sw_bit - 소프트웨어 비트 번호를 실제 항목 비트로 옮긴다
 *
 * @bitnr: 0 부터 12 까지의 번호.
 * @return: 그 번호에 해당하는 비트 마스크.
 *
 * 명세가 "Ignored/AVL"로 남긴 비트들이 항목 안에 흩어져 있어, 연속된
 * 번호를 실제 위치로 옮기는 표가 필요하다. 9번과 11번이 낱개로 있고,
 * 52번부터 62번까지가 이어져 있다.
 *
 * 상수 인자에 대해 BUILD_BUG 를 거는 이유: 잘못된 번호를 런타임에
 * 발견하는 것보다 빌드에서 걸리는 편이 낫다.
 *
 * 원 주석이 3·4·6·8번에도 쓸 수 있는 자리가 있다고 짚지만, 항목 종류마다
 * 다르므로 여기서는 쓰지 않는다.
 */
static inline u64 x86_64_pt_sw_bit(unsigned int bitnr)
{
	if (__builtin_constant_p(bitnr) && bitnr > 12)	/* [한국어] 상수 인자가 범위를 넘으면 */
		BUILD_BUG();	/* [한국어] 런타임에 발견하는 것보다 빌드에서 걸리는 편이 낫다 */

	/* Bits marked Ignored/AVL in the specification */
	switch (bitnr) {	/* [한국어] (원 주석: 명세가 Ignored/AVL 로 남긴 비트들) 흩어져 있어 표가 필요하다 */
	case 0:	/* [한국어] 0번은 */
		return BIT(9);	/* [한국어] 9번 비트 */
	case 1:	/* [한국어] 1번은 */
		return BIT(11);	/* [한국어] 11번 비트 */
	case 2 ... 12:	/* [한국어] 2번부터는 */
		return BIT_ULL((bitnr - 2) + 52);	/* [한국어] 52번부터 이어진 자리 */
	/* Some bits in 8,6,4,3 are available in some entries */
	default:	/* [한국어] 범위를 넘는 번호 */
		PT_WARN_ON(true);	/* [한국어] (원 주석: 8·6·4·3 번에도 항목에 따라 쓸 자리가 있다) 여기서는 쓰지 않는다 */
		return 0;	/* [한국어] 잘못된 번호 */
	}
}
#define pt_sw_bit x86_64_pt_sw_bit	/* [한국어] 이 정의가 있으면 기본 구현이 실제 소프트웨어 비트 코드를 만든다 */

/* --- iommu */
#include <linux/generic_pt/iommu.h>	/* [한국어] (원 주석: 여기부터 IOMMU 계층과의 접점) */
#include <linux/iommu.h>	/* [한국어] IOMMU_READ/WRITE/MMIO 권한 상수 */

#define pt_iommu_table pt_iommu_x86_64	/* [한국어] 공통 코드가 부르는 이름을 이 형식의 구조체로 */

/* The common struct is in the per-format common struct */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * common_from_iommu - IOMMU 객체에서 공통 페이지 테이블 상태로
 *
 * @iommu_table: IOMMU 계층의 객체.
 * @return: 그 안에 박힌 pt_common.
 */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)	/* [한국어] IOMMU 객체에서 형식별 구조체로 */
			->x86_64_pt.common;	/* [한국어] 그 안의 공통 부분으로 */
}

/*
 * [한국어]
 * iommu_from_common - 공통 상태에서 IOMMU 객체로 되돌아간다
 *
 * @common: 공통 페이지 테이블 상태.
 * @return: 그것을 품은 IOMMU 객체.
 */
static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, x86_64_pt.common)	/* [한국어] 공통 상태에서 */
			->iommu;	/* [한국어] IOMMU 객체로 되돌아간다 */
}

/*
 * [한국어]
 * x86_64_pt_iommu_set_prot - IOMMU 권한을 항목 비트로 옮긴다
 *
 * @common: 페이지 테이블 인스턴스.
 * @attrs: 채울 쓰기 속성.
 * @iommu_prot: IOMMU_READ/WRITE/MMIO 조합.
 * @return: 늘 0.
 *
 * x86 에는 읽기 전용 비트가 따로 없다 — 항목이 존재하면 읽을 수 있고,
 * RW 가 쓰기를 더한다. 그래서 IOMMU_READ 는 아무것도 세우지 않는다.
 *
 * A 와 D 를 미리 세우는 이유: 하드웨어가 접근·쓰기 때 그 비트를 세우려고
 * 표에 쓰기를 하는데, 미리 세워 두면 그 쓰기가 사라진다. 더티 추적을
 * 쓰지 않는 경우의 최적화다.
 *
 * U(User)를 늘 세우는 이유: 장치의 접근은 커널 권한이 아니라 사용자
 * 권한으로 해석된다.
 */
static inline int x86_64_pt_iommu_set_prot(struct pt_common *common,
					   struct pt_write_attrs *attrs,
					   unsigned int iommu_prot)
{
	u64 pte;	/* [한국어] 만들 속성 비트 */

	pte = X86_64_FMT_U | X86_64_FMT_A;	/* [한국어] 장치 접근은 사용자 권한으로 해석된다. A 를 미리 세워 하드웨어의 표 쓰기를 없앤다 */
	if (iommu_prot & IOMMU_WRITE)	/* [한국어] 쓰기 허용이면 */
		pte |= X86_64_FMT_RW | X86_64_FMT_D;	/* [한국어] 읽기 전용 비트가 따로 없어 RW 가 쓰기를 더한다. D 도 미리 */

	/*
	 * Ideally we'd have an IOMMU_ENCRYPTED flag set by higher levels to
	 * control this. For now if the tables use sme_set then so do the ptes.
	 */
	if (pt_feature(common, PT_FEAT_X86_64_AMD_ENCRYPT_TABLES) &&	/* [한국어] (원 주석: 원래는 상위가 IOMMU_ENCRYPTED 로 정해 주는 편이 옳다) */
	    !(iommu_prot & IOMMU_MMIO))	/* [한국어] 장치 레지스터는 암호화 대상이 아니다 */
		pte = __sme_set(pte);	/* [한국어] 그 밖에는 표와 같은 규칙 */

	attrs->descriptor_bits = pte;	/* [한국어] 잎을 쓸 때 그대로 얹힌다 */
	return 0;	/* [한국어] 실패할 수 없는 경로 */
}
#define pt_iommu_set_prot x86_64_pt_iommu_set_prot	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_iommu_fmt_init - 형식별 초기화
 *
 * @iommu_table: 초기화할 객체.
 * @cfg: 드라이버가 준 설정.
 * @return: 0 성공, -EOPNOTSUPP 이면 지원하지 않는 단계 수다.
 *
 * 4단계(top_level 3)와 5단계(4)만 받는다. 3단계 형식은 비슷하지만 예약
 * 비트 배치가 달라 이 코드로는 다룰 수 없다(파일 상단의 원 주석).
 *
 * 입력 주소 폭을 여기서 정하지 않는 것이 AMD v1 과 다르다 — 이 형식은
 * 최상위가 자라지 않으므로 폭이 단계 수에서 곧바로 나온다.
 */
static inline int
x86_64_pt_iommu_fmt_init(struct pt_iommu_x86_64 *iommu_table,
			 const struct pt_iommu_x86_64_cfg *cfg)
{
	struct pt_x86_64 *table = &iommu_table->x86_64_pt;	/* [한국어] 형식별 페이지 테이블 상태 */

	if (cfg->top_level < 3 || cfg->top_level > 4)	/* [한국어] 4단계(3)와 5단계(4)만 */
		return -EOPNOTSUPP;	/* [한국어] 3단계는 예약 비트 배치가 달라 이 코드로 다룰 수 없다 */

	pt_top_set_level(&table->common, cfg->top_level);	/* [한국어] 표는 첫 매핑이 만든다 */

	table->common.max_oasz_lg2 =	/* [한국어] 출력 주소 폭은 */
		min(PT_MAX_OUTPUT_ADDRESS_LG2, cfg->common.hw_max_oasz_lg2);	/* [한국어] 형식과 하드웨어 중 작은 쪽 */
	return 0;	/* [한국어] 입력 폭은 단계 수에서 나오므로 정할 것이 없다 */
}
#define pt_iommu_fmt_init x86_64_pt_iommu_fmt_init	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * x86_64_pt_iommu_fmt_hw_info - 하드웨어에 적을 값을 꺼낸다
 *
 * @table: 페이지 테이블 객체.
 * @top_range: 최상위 정보를 담은 범위.
 * @info: 채울 결과.
 *
 * drivers/iommu/amd 의 init_gcr3_table() 이 이 주소를 GCR3 표의 0번
 * 항목에 넣는다 — 그 순간부터 그 장치의 PASID 0 접근이 이 표를 걷는다.
 */
static inline void
x86_64_pt_iommu_fmt_hw_info(struct pt_iommu_x86_64 *table,
			    const struct pt_range *top_range,
			    struct pt_iommu_x86_64_hw_info *info)
{
	info->gcr3_pt = virt_to_phys(top_range->top_table);	/* [한국어] GCR3 항목에 적을 최상위 표의 물리 주소 */
	PT_WARN_ON(info->gcr3_pt & ~PT_TOP_PHYS_MASK);	/* [한국어] 그 필드에 담기지 않는 비트가 있으면 안 된다 */
	info->levels = top_range->top_level + 1;	/* [한국어] DTE 의 단계 수는 1 기반 */
}
#define pt_iommu_fmt_hw_info x86_64_pt_iommu_fmt_hw_info	/* [한국어] 공통 API 이름으로 */

#if defined(GENERIC_PT_KUNIT)	/* [한국어] 시험 모듈을 빌드하는 중이면 */
/*
 * [한국어] kunit 이 시험할 설정 목록.
 * 네 가지 조합을 모두 돈다 — 부호 확장이 있는 4·5단계(CPU 와 같은 배치)와,
 * 원 주석이 짚듯 AMD IOMMU 가 PASID 0 에 쓰는 부호 확장 없는 변종.
 */
static const struct pt_iommu_x86_64_cfg x86_64_kunit_fmt_cfgs[] = {
	[0] = { .common.features = BIT(PT_FEAT_SIGN_EXTEND),	/* [한국어] 부호 확장이 있는 */
		.common.hw_max_vasz_lg2 = 48, .top_level = 3 },	/* [한국어] 4단계 — CPU 의 48비트 배치 */
	[1] = { .common.features = BIT(PT_FEAT_SIGN_EXTEND),	/* [한국어] 부호 확장이 있는 */
		.common.hw_max_vasz_lg2 = 57, .top_level = 4 },	/* [한국어] 5단계 — 57비트 */
	/* AMD IOMMU PASID 0 formats with no SIGN_EXTEND */
	[2] = { .common.hw_max_vasz_lg2 = 47, .top_level = 3 },	/* [한국어] (원 주석: 부호 확장 없는 AMD PASID 0 형식) 최상위 비트를 버린 4단계 */
	[3] = { .common.hw_max_vasz_lg2 = 56, .top_level = 4},	/* [한국어] 같은 이유로 하나 줄인 5단계 */
};
#define kunit_fmt_cfgs x86_64_kunit_fmt_cfgs	/* [한국어] 시험 코드가 부르는 이름으로 */
enum { KUNIT_FMT_FEATURES =  BIT(PT_FEAT_SIGN_EXTEND)};	/* [한국어] 시험에서 켤 수 있는 기능 */
#endif	/* [한국어] 포함 방지 끝 */
#endif
