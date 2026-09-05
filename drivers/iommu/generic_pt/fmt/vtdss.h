/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 * Intel VT-d Second Stange 5/4 level page table
 *
 * This is described in
 *   Section "3.7 Second-Stage Translation"
 *   Section "9.8 Second-Stage Paging Entries"
 *
 * Of the "Intel Virtualization Technology for Directed I/O Architecture
 * Specification".
 *
 * The named levels in the spec map to the pts->level as:
 *   Table/SS-PTE - 0
 *   Directory/SS-PDE - 1
 *   Directory Ptr/SS-PDPTE - 2
 *   PML4/SS-PML4E - 3
 *   PML5/SS-PML5E - 4
 */
/*
 * [한국어 설명] Intel VT-d 2단계 페이지 테이블 형식 구현 (vtdss.h)
 *
 * === 파일의 역할 ===
 * VT-d 의 2단계(second stage) 변환 표를 generic_pt 의 형식 API 로 감싼다.
 * 출처는 원 주석이 든 VT-d 명세의 3.7 절과 9.8 절이다.
 *
 * 2단계가 무엇인가: 중첩 변환에서 게스트가 만든 1단계 표를 걸은 결과를
 * 다시 한 번 옮기는 표다. 게스트가 보는 물리 주소를 호스트의 진짜 물리
 * 주소로 바꾸는 층이며, 중첩을 쓰지 않는 경우에도 VFIO 처럼 사용자
 * 공간이 IOVA 를 직접 관리할 때 이 표를 쓴다.
 *
 * x86_64 형식과 배치가 닮았지만 결정적인 차이가 있다. 존재 비트가 없어
 * 읽기·쓰기 중 하나라도 서 있어야 유효한 항목이 된다 — 그래서 권한 없는
 * 매핑을 만들 수 없고, 항목이 0 인지로 빈자리를 판별한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_vtdss.c → iommu_template.h → defs_vtdss.h → pt_defs.h →
 *   [이 파일] → pt_common.h → iommu_pt.h → drivers/iommu/intel
 *
 * 단계 번호 대응은 파일 상단의 원 주석에 있다: SS-PTE=0, SS-PDE=1,
 * SS-PDPTE=2, SS-PML4E=3, SS-PML5E=4.
 *
 * 실행 컨텍스트: 전부 인라인.
 *
 * === 타 모듈과의 연결 ===
 * 위: pt_common.h 의 API 선언, iommu_pt.h 의 매핑 경로.
 * 아래: <linux/bitfield.h>. SME 관련 포함이 없는 것이 x86_64 형식과
 *       다른 점이다 — 인텔 플랫폼에는 그 비트가 없다.
 * fmt_hw_info 가 넘기는 ssptptr 과 aw 를 drivers/iommu/intel 이 컨텍스트
 * 항목에 적는다.
 *
 * === 주요 함수/구조체 요약 ===
 * vtdss_pt_load_entry_raw: 항목이 0 인지로 존재를, PS 로 잎 여부를 가른다.
 * vtdss_pt_iommu_set_prot: 권한이 하나도 없으면 거절한다 — 존재 비트가
 *   없는 형식의 필연적 결과다. ERRATA_772415_SPR17 회피도 여기 있다.
 * vtdss_pt_entry_is_write_dirty 계열: 더티 추적. 연속 항목이 없어 한
 *   자리만 보면 된다.
 * vtdss_pt_sw_bit: 명세가 "무시됨"으로 남긴 비트를 소프트웨어가 쓴다.
 * vtdss_pt_iommu_fmt_hw_info: 최상위 표 주소와 주소 폭 코드(aw).
 */
#ifndef __GENERIC_PT_FMT_VTDSS_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_FMT_VTDSS_H	/* [한국어] 같은 이름으로 표시 */

#include "defs_vtdss.h"	/* [한국어] 이 형식의 주소 타입과 쓰기 속성 */
#include "../pt_defs.h"	/* [한국어] 순회 상태와 산술 */

#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP */
#include <linux/container_of.h>	/* [한국어] IOMMU 객체와 공통 상태를 오간다 */
#include <linux/log2.h>	/* [한국어] ilog2 — 표 항목 수 계산 */

/*
 * [한국어] 이 형식의 크기 상수들.
 * 5단계까지 지원하므로 입력 57비트, 출력 52비트다.
 */
enum {
	PT_MAX_OUTPUT_ADDRESS_LG2 = 52,
	/* [한국어] 출력 물리 주소의 비트 폭.
	 * 읽는 자: 매핑하려는 물리 주소가 표현 가능한지 검사하는 코드.
	 * 값이 52 인 이유: 아래 VTDSS_FMT_OA 가 비트 12 부터 51 까지를 쓰고, 페이지 안
	 *   오프셋 12비트를 더하면 52 다.
	 * 실제 하드웨어가 지원하는 폭(HAW, Host Address Width)은 이보다 좁을 수 있어,
	 *   드라이버가 캐퍼빌리티 레지스터를 읽어 다시 한 번 제한한다.
	 * SS 는 second stage — VT-d 의 2단계(호스트가 관리하는 바깥 변환) 형식이다. */
	PT_MAX_VA_ADDRESS_LG2 = 57,
	/* [한국어] 입력(IOVA) 주소의 비트 폭.
	 * 읽는 자: 도메인의 주소 공간 크기를 정할 때, IOVA 가 범위 안인지 볼 때.
	 * 값이 57 인 이유: 5단계까지 지원하기 때문이다. 단계 하나가 9비트를 더하므로
	 *   4단계면 48, 5단계면 57 이다.
	 * 실제로 몇 단계를 쓸지는 하드웨어의 SAGAW 능력과 요청한 주소 공간에 따라
	 *   정해지고, 이 값은 형식이 담을 수 있는 상한이다. */
	PT_ITEM_WORD_SIZE = sizeof(u64),
	/* [한국어] 페이지 테이블 항목 하나의 바이트 폭 — 8바이트.
	 * 읽는 자: 공통 순회 코드가 항목 사이의 보폭과 표당 항목 수를 계산할 때.
	 * 값 범위: 8 로 고정.
	 * 표가 4KB, 항목이 8바이트이므로 표마다 512개, 곧 단계마다 색인이 9비트다. */
	PT_MAX_TOP_LEVEL = 4,
	/* [한국어] 최상위 단계의 번호 — 0 기반이라 최대 5단계.
	 * 읽는 자: 표를 만들 때 몇 단계를 잡을지.
	 * 값 범위: 4. 단계 이름은 VT-d 규격의 SS-PTE, SS-PDE, SS-PDPE, SS-PML4E,
	 *   SS-PML5E 에 차례로 대응한다.
	 * 0 기반이라는 점을 놓치면 단계를 하나 적게 잡는다. */
	PT_GRANULE_LG2SZ = 12,
	/* [한국어] 매핑할 수 있는 최소 페이지 크기의 로그 값 — 4KB.
	 * 읽는 자: 매핑 요청의 정렬 검사와 단계별 페이지 크기 계산.
	 * 값 범위: 12 로 고정.
	 * 아래 PT_TABLEMEM_LG2SZ 와 값이 같지만 뜻이 다르다 — 그쪽은 표 자체의 크기다. */
	PT_TABLEMEM_LG2SZ = 12,
	/* [한국어] 표 하나가 차지하는 메모리 크기의 로그 값 — 4KB.
	 * 읽는 자: 표를 할당할 때, 그리고 표당 항목 수를 계산할 때.
	 * 값 범위: 12 로 고정.
	 * 12 - 3 = 9 가 곧 단계당 색인 비트 수이며, 그로부터 위
	 *   PT_MAX_VA_ADDRESS_LG2 의 57(= 5 * 9 + 12)이 나온다. */

	/* SSPTPTR is 4k aligned and limited by HAW */
	PT_TOP_PHYS_MASK = GENMASK_ULL(63, 12),
	/* [한국어] (원 주석 참고) 최상위 표의 물리 주소로 쓸 수 있는 비트들.
	 * 읽는 자: 최상위 표의 주소를 컨텍스트/PASID 항목의 SSPTPTR 필드에 실을 때.
	 * 값 범위: 비트 12 부터 63 까지. 다른 형식의 마스크보다 넓은데, 원 주석대로
	 *   이 필드 자체에는 상한이 없고 하드웨어의 HAW 가 실제 상한을 정하기
	 *   때문이다. 그 검사는 드라이버가 따로 한다.
	 * 하위 12비트가 빠진 이유: 표는 4KB 정렬이라 그 자리가 언제나 0 이다. */
};

/* Shared descriptor bits */
/*
 * [한국어] (위 영어 주석에 이어)
 * 모든 단계가 공유하는 항목 비트.
 * 존재 비트가 없다는 것이 이 배치의 특징이다 — R 이나 W 중 하나가
 * 서 있는 것이 곧 유효하다는 뜻이다.
 */
enum {
	VTDSS_FMT_R = BIT(0),
	/* [한국어] 읽기 허용 비트 — 이 형식에서는 존재 표시를 겸한다.
	 * 설정자: IOMMU_READ 권한이 있으면 세운다.
	 * 읽는 자: 하드웨어와, 항목이 비었는지 보는 순회 코드.
	 * 이 배치의 특징이 여기 있다. 다른 형식과 달리 별도의 존재(present) 비트가
	 *   없다 — R 이나 아래 W 중 하나라도 서 있으면 유효한 항목이고, 둘 다 0 이면
	 *   빈 항목이다.
	 * 그래서 "존재하지만 아무 권한도 없는" 항목을 만들 수 없고, 매핑을 지울 때는
	 *   두 비트를 함께 내려야 한다. */
	VTDSS_FMT_W = BIT(1),
	/* [한국어] 쓰기 허용 비트.
	 * 설정자: IOMMU_WRITE 권한이 있으면 세운다.
	 * 읽는 자: 하드웨어.
	 * 위 R 과 함께 이 비트도 존재 표시를 겸한다 — 둘 중 하나만 서 있어도 항목은
	 *   유효하다. 따라서 쓰기 전용 매핑(W 만 1)이 이 형식에서는 성립한다.
	 * RISC-V 형식이 "R 없는 W" 를 예약으로 금지하는 것과 대비된다. */
	VTDSS_FMT_A = BIT(8),
	/* [한국어] 접근됨(accessed) 비트 — 하드웨어가 세운다.
	 * 설정자: 하드웨어. 소프트웨어는 접근 추적을 쓸 때 0 으로 지운다.
	 * 읽는 자: 접근 추적 결과를 수집하는 코드.
	 * 다른 형식에서 이 비트를 미리 세워 두는 것과 다르다. VT-d 는 이 비트를
	 *   기능으로 제공하므로(장치가 실제로 쓴 페이지를 알아내는 용도), 추적을
	 *   쓰는 동안에는 0 으로 두어야 의미가 있다.
	 * 비트 8 처럼 높은 자리에 있는 것은, 낮은 자리를 권한 비트가 쓰고 있어서다. */
	VTDSS_FMT_D = BIT(9),
	/* [한국어] 더티 비트 — 이 페이지에 쓰기가 있었다는 표시.
	 * 설정자: 하드웨어. 소프트웨어는 더티 추적을 시작할 때 0 으로 지운다.
	 * 읽는 자: iommufd 의 더티 추적 수집 경로.
	 * 왜 필요한가: 라이브 마이그레이션에서 장치가 고친 페이지만 다시 보내면
	 *   전송량이 크게 준다. 그 목록을 이 비트로 얻는다.
	 * 위 A 비트와 짝을 이루며, 둘 다 하드웨어가 페이지 테이블 메모리에 원자적
	 *   쓰기를 하는 방식으로 갱신한다. */
	VTDSS_FMT_SNP = BIT(11),
	/* [한국어] 스누프 강제 비트 — 장치 접근이 CPU 캐시를 스누프하게 만든다.
	 * 설정자: 도메인이 캐시 일관성을 강제할 때 세운다.
	 * 읽는 자: 하드웨어.
	 * 무엇을 하는가: 장치가 비일관(no-snoop) 접근을 요청해도 하드웨어가 CPU
	 *   캐시를 확인하게 한다. AMD 형식의 FC 비트와 같은 역할이다.
	 * 왜 필요한가: 신뢰할 수 없는 장치에게 캐시 일관성 선택을 맡기면 호스트가
	 *   캐시 상태를 예측할 수 없다. 매핑 단위로 강제해 두면 그 위험이 사라진다.
	 * 하드웨어가 이 기능을 지원하는지는 캐퍼빌리티 레지스터의 SC 비트가 알려 준다. */
	VTDSS_FMT_OA = GENMASK_ULL(51, 12),
	/* [한국어] 출력 주소가 들어가는 자리 — 잎이면 물리 주소, 표면 아래 표의 주소.
	 * 설정자: 매핑을 만들거나 표를 이어 붙일 때.
	 * 읽는 자: 하드웨어, 그리고 항목에서 주소를 되꺼내는 코드.
	 * 값 범위: 비트 12 부터 51 까지 40비트. 12비트를 밀면 52비트 물리 주소가 된다.
	 * 큰 페이지(2MB, 1GB)에서는 이 자리의 하위 비트가 그 크기만큼 0 이어야 한다 —
	 *   정렬되지 않은 주소는 큰 페이지로 표현할 수 없다. */
};

/* PDPTE/PDE */
/*
 * [한국어] (위 영어 주석에 이어)
 * 중간 단계에서만 뜻이 있는 비트.
 * PS 가 서 있으면 아래 표가 아니라 큰 페이지다.
 */
enum {
	VTDSS_FMT_PS = BIT(7),
	/* [한국어] 큰 페이지 표시 — 이 항목이 아래 표가 아니라 잎이라는 뜻이다.
	 * 설정자: 2MB 나 1GB 매핑을 만들 때 세운다.
	 * 읽는 자: 하드웨어와, 순회가 내려갈지 여기서 멈출지 판정하는 코드.
	 * 왜 중간 단계에만 있는가: 단계 0 에는 아래 표가 없어 언제나 잎이므로 구분이
	 *   필요 없다. 그 자리의 비트 7 은 다른 뜻으로 쓰인다 — 같은 비트가 단계에
	 *   따라 달라지는 것이 이 계열 형식의 공통된 함정이다.
	 * 큰 페이지의 이득: 2MB 매핑 하나가 4KB 항목 512개를 대신해 표 메모리와
	 *   TLB 항목이 함께 줄고, 순회 깊이도 한 단계 짧아진다.
	 * 하드웨어가 어느 크기까지 지원하는지는 캐퍼빌리티의 SLLPS 필드가 알려 준다. */
};

/*
 * [한국어] 공통 상태에서 이 형식의 구조체로 되짚는 매크로.
 * container_of_const 라 const 포인터를 넘겨도 const 가 유지된다.
 */
#define common_to_vtdss_pt(common_ptr) \
	container_of_const(common_ptr, struct pt_vtdss, common)	/* [한국어] 공통 부분을 품은 형식별 구조체로 되짚는다 */
#define to_vtdss_pt(pts) common_to_vtdss_pt((pts)->range->common)	/* [한국어] 순회 상태에서 한 번에 되짚는 지름길 */

/*
 * [한국어]
 * vtdss_pt_table_pa - 표 항목이 가리키는 아래 표의 물리 주소
 *
 * @pts: 볼 항목(PT_ENTRY_TABLE).
 * @return: 아래 표의 물리 주소.
 *
 * 암호화 비트를 벗기는 처리가 없는 것이 x86_64 형식과 다르다 — 인텔
 * 플랫폼의 이 표에는 그런 비트가 없다.
 */
static inline pt_oaddr_t vtdss_pt_table_pa(const struct pt_state *pts)
{
	return oalog2_mul(FIELD_GET(VTDSS_FMT_OA, pts->entry),	/* [한국어] 주소는 페이지 번호로 저장되어 있다 */
			  PT_TABLEMEM_LG2SZ);	/* [한국어] 표 크기만큼 곱한다 */
}
#define pt_table_pa vtdss_pt_table_pa	/* [한국어] 공통 API 이름으로 잇는다 */

/*
 * [한국어]
 * vtdss_pt_entry_oa - 잎 항목이 내는 출력 주소
 *
 * @pts: 볼 항목.
 * @return: 출력 주소.
 *
 * 크기 인코딩이 없어 주소 필드를 그대로 꺼내면 된다.
 */
static inline pt_oaddr_t vtdss_pt_entry_oa(const struct pt_state *pts)
{
	return oalog2_mul(FIELD_GET(VTDSS_FMT_OA, pts->entry),	/* [한국어] 크기 인코딩이 없어 그대로 */
			  PT_GRANULE_LG2SZ);	/* [한국어] 페이지 번호를 주소로 */
}
#define pt_entry_oa vtdss_pt_entry_oa	/* [한국어] 이쪽을 구현하면 pt_item_oa 는 기본이 만들어 준다 */

/*
 * [한국어]
 * vtdss_pt_can_have_leaf - 이 단계에 잎을 놓을 수 있는가
 *
 * @pts: 현재 단계.
 * @return: 2단계 이하면 참.
 *
 * 4KB·2MB·1GB 까지가 이 형식의 페이지 크기다.
 */
static inline bool vtdss_pt_can_have_leaf(const struct pt_state *pts)
{
	return pts->level <= 2;	/* [한국어] 4KB·2MB·1GB 까지 */
}
#define pt_can_have_leaf vtdss_pt_can_have_leaf	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_num_items_lg2 - 이 단계 표의 항목 수(지수)
 *
 * @pts: 현재 단계.
 * @return: 늘 9(4KB 표에 8바이트 항목이면 512개).
 */
static inline unsigned int vtdss_pt_num_items_lg2(const struct pt_state *pts)
{
	return PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64));	/* [한국어] 모든 단계가 같다 */
}
#define pt_num_items_lg2 vtdss_pt_num_items_lg2	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_load_entry_raw - 항목을 읽고 종류를 가른다
 *
 * @pts: 읽을 위치.
 * @return: 비었는가, 표인가, 출력 주소인가.
 *
 * 존재 비트가 없어 항목 전체가 0 인지로 빈자리를 판별한다. 그것이 성립하는
 * 이유는 유효한 항목에 반드시 R 이나 W 가 서 있기 때문이다 —
 * vtdss_pt_iommu_set_prot 이 권한 없는 매핑을 거절해 그 불변식을 지킨다.
 */
static inline enum pt_entry_type vtdss_pt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64);	/* [한국어] 이 단계의 표 */
	u64 entry;	/* [한국어] 읽은 값 */

	pts->entry = entry = READ_ONCE(tablep[pts->index]);	/* [한국어] 한 번만 읽어 담는다 */
	if (!entry)	/* [한국어] 존재 비트가 없어 0 인지로 판별한다 */
		return PT_ENTRY_EMPTY;	/* [한국어] 유효한 항목에는 반드시 R 이나 W 가 있다 */
	if (pts->level == 0 ||	/* [한국어] 0단계에는 아래 표가 없고 */
	    (vtdss_pt_can_have_leaf(pts) && (pts->entry & VTDSS_FMT_PS)))	/* [한국어] 큰 페이지를 허용하는 단계에서 PS 가 서 있으면 */
		return PT_ENTRY_OA;	/* [한국어] 잎이다 */
	return PT_ENTRY_TABLE;	/* [한국어] 그 밖에는 아래 표 */
}
#define pt_load_entry_raw vtdss_pt_load_entry_raw	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_install_leaf_entry - 표에 잎 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @oa: 출력 주소.
 * @oasz_lg2: 이 잎이 덮는 크기의 지수.
 * @attrs: 얹을 권한 비트.
 *
 * 존재 비트를 따로 세우지 않는다. attrs 에 R 이나 W 가 반드시 들어 있어,
 * 그것이 존재 표시 노릇을 한다.
 */
static inline void
vtdss_pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			    unsigned int oasz_lg2,
			    const struct pt_write_attrs *attrs)
{
	u64 *tablep = pt_cur_table(pts, u64);	/* [한국어] 이 단계의 표 */
	u64 entry;	/* [한국어] 만들 항목 값 */

	if (!pt_check_install_leaf_args(pts, oa, oasz_lg2))	/* [한국어] 정렬과 크기를 먼저 확인 */
		return;	/* [한국어] 어긋나면 쓰지 않는다 */

	entry = FIELD_PREP(VTDSS_FMT_OA, log2_div(oa, PT_GRANULE_LG2SZ)) |	/* [한국어] 주소를 페이지 번호로 */
		attrs->descriptor_bits;	/* [한국어] R 이나 W 가 반드시 들어 있어 존재 표시 노릇을 한다 */
	if (pts->level != 0)	/* [한국어] 중간 단계면 */
		entry |= VTDSS_FMT_PS;	/* [한국어] 아래 표가 아니라 큰 페이지임을 알린다 */

	WRITE_ONCE(tablep[pts->index], entry);	/* [한국어] 연속 항목이 없어 한 번만 쓴다 */
	pts->entry = entry;	/* [한국어] 순회 상태도 새 값으로 */
}
#define pt_install_leaf_entry vtdss_pt_install_leaf_entry	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_install_table - 아래 표를 가리키는 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @table_pa: 아래 표의 물리 주소.
 * @attrs: 속성(이 형식에서는 쓰지 않는다).
 * @return: 내가 꽂았으면 참.
 *
 * 표 단계에는 늘 R·W 를 세운다. 하드웨어가 경로상의 권한을 AND 하므로
 * 중간에서 좁히면 잎의 설정을 덮어쓴다.
 */
static inline bool vtdss_pt_install_table(struct pt_state *pts,
					  pt_oaddr_t table_pa,
					  const struct pt_write_attrs *attrs)
{
	u64 entry;	/* [한국어] 만들 항목 값 */

	entry = VTDSS_FMT_R | VTDSS_FMT_W |	/* [한국어] 권한을 AND 하므로 표 단계는 늘 열어 둔다 */
		FIELD_PREP(VTDSS_FMT_OA, log2_div(table_pa, PT_GRANULE_LG2SZ));	/* [한국어] 아래 표의 주소를 페이지 번호로 */
	return pt_table_install64(pts, entry);	/* [한국어] 경합에서 지면 거짓 */
}
#define pt_install_table vtdss_pt_install_table	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_attr_from_entry - 항목에서 권한 비트만 다시 꺼낸다
 *
 * @pts: 읽을 항목.
 * @attrs: 채울 결과.
 *
 * 읽기·쓰기와 SNP(스누프 강제)만 옮긴다. 주소와 PS 는 새 항목을 만들 때
 * 다시 정해진다.
 */
static inline void vtdss_pt_attr_from_entry(const struct pt_state *pts,
					    struct pt_write_attrs *attrs)
{
	attrs->descriptor_bits = pts->entry &	/* [한국어] 새 항목에 그대로 옮길 비트만 */
				 (VTDSS_FMT_R | VTDSS_FMT_W | VTDSS_FMT_SNP);	/* [한국어] 권한과 스누프 강제 — 주소와 PS 는 다시 정해진다 */
}
#define pt_attr_from_entry vtdss_pt_attr_from_entry	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_entry_is_write_dirty - 이 항목이 가리키는 곳에 쓰기가 있었는가
 *
 * @pts: 볼 항목.
 * @return: 있었으면 참.
 *
 * 연속 항목이 없는 형식이라 한 자리만 보면 된다 — AMD v1 이 묶음 전체를
 * 훑어야 하는 것과 대조적이다.
 */
static inline bool vtdss_pt_entry_is_write_dirty(const struct pt_state *pts)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 볼 항목 */

	return READ_ONCE(*tablep) & VTDSS_FMT_D;	/* [한국어] 연속 항목이 없어 한 자리만 보면 된다 */
}
#define pt_entry_is_write_dirty vtdss_pt_entry_is_write_dirty	/* [한국어] 더티 추적을 지원한다는 신호이기도 하다 */

/*
 * [한국어]
 * vtdss_pt_entry_make_write_clean - 더티 표시를 지운다
 *
 * @pts: 고칠 항목.
 *
 * 읽고-고치고-쓰기를 원자 연산 없이 한다. 그 직후의 하드웨어 쓰기를
 * 놓치는 것은 pt_common.h 가 명시한 계약 안이다 — TLB 를 비운 뒤부터
 * 세는 것이 규칙이다.
 */
static inline void vtdss_pt_entry_make_write_clean(struct pt_state *pts)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 고칠 항목 */

	WRITE_ONCE(*tablep, READ_ONCE(*tablep) & ~(u64)VTDSS_FMT_D);	/* [한국어] 더티 비트만 내린다 — 직후의 쓰기를 놓치는 것은 계약 안이다 */
}
#define pt_entry_make_write_clean vtdss_pt_entry_make_write_clean	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_entry_make_write_dirty - 소프트웨어가 더티 표시를 남긴다
 *
 * @pts: 고칠 항목.
 * @return: 성공하면 참.
 *
 * cmpxchg 인 이유: 하드웨어가 같은 항목의 접근 비트를 동시에 갱신할 수
 * 있어, 통째로 덮어쓰면 그 갱신을 잃는다.
 */
static inline bool vtdss_pt_entry_make_write_dirty(struct pt_state *pts)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;	/* [한국어] 고칠 항목 */
	u64 new = pts->entry | VTDSS_FMT_D;	/* [한국어] 더티 비트만 얹은 값 */

	return try_cmpxchg64(tablep, &pts->entry, new);	/* [한국어] 하드웨어가 접근 비트를 동시에 갱신할 수 있다 */
}
#define pt_entry_make_write_dirty vtdss_pt_entry_make_write_dirty	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_max_sw_bit - 소프트웨어가 쓸 수 있는 비트의 개수
 *
 * @common: 무시된다.
 * @return: 10(0번부터 10번까지).
 */
static inline unsigned int vtdss_pt_max_sw_bit(struct pt_common *common)
{
	return 10;	/* [한국어] 0번부터 10번까지 */
}
#define pt_max_sw_bit vtdss_pt_max_sw_bit	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_sw_bit - 소프트웨어 비트 번호를 실제 항목 비트로 옮긴다
 *
 * @bitnr: 0 부터 10 까지의 번호.
 * @return: 그 번호에 해당하는 비트 마스크.
 *
 * 명세가 "Ignored"로 남긴 자리가 흩어져 있어 번호를 옮기는 표가 필요하다.
 * 10번 비트가 낱개로 있고, 52번부터 60번까지가 이어지며, 63번이 따로 있다.
 */
static inline u64 vtdss_pt_sw_bit(unsigned int bitnr)
{
	if (__builtin_constant_p(bitnr) && bitnr > 10)	/* [한국어] 상수 인자가 범위를 넘으면 */
		BUILD_BUG();	/* [한국어] 빌드에서 걸리게 한다 */

	/* Bits marked Ignored in the specification */
	switch (bitnr) {	/* [한국어] (원 주석: 명세가 Ignored 로 남긴 비트들) */
	case 0:	/* [한국어] 0번은 */
		return BIT(10);	/* [한국어] 10번 비트 */
	case 1 ... 9:	/* [한국어] 1번부터 9번은 */
		return BIT_ULL((bitnr - 1) + 52);	/* [한국어] 52번부터 이어진 자리 */
	case 10:	/* [한국어] 10번은 */
		return BIT_ULL(63);	/* [한국어] 최상위 비트 */
	/* Some bits in 9-3 are available in some entries */
	default:	/* [한국어] 범위를 넘는 번호 */
		PT_WARN_ON(true);	/* [한국어] (원 주석: 3~9번에도 항목에 따라 쓸 자리가 있다) 여기서는 쓰지 않는다 */
		return 0;	/* [한국어] 잘못된 번호 */
	}
}
#define pt_sw_bit vtdss_pt_sw_bit	/* [한국어] 이 정의가 있으면 기본 구현이 실제 소프트웨어 비트 코드를 만든다 */

/* --- iommu */
#include <linux/generic_pt/iommu.h>	/* [한국어] (원 주석: 여기부터 IOMMU 계층과의 접점) */
#include <linux/iommu.h>	/* [한국어] IOMMU_READ/WRITE 권한 상수 */

#define pt_iommu_table pt_iommu_vtdss	/* [한국어] 공통 코드가 부르는 이름을 이 형식의 구조체로 */

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
			->vtdss_pt.common;	/* [한국어] 그 안의 공통 부분으로 */
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
	return &container_of(common, struct pt_iommu_table, vtdss_pt.common)	/* [한국어] 공통 상태에서 */
			->iommu;	/* [한국어] IOMMU 객체로 되돌아간다 */
}

/*
 * [한국어]
 * vtdss_pt_iommu_set_prot - IOMMU 권한을 항목 비트로 옮긴다
 *
 * @common: 페이지 테이블 인스턴스.
 * @attrs: 채울 쓰기 속성.
 * @iommu_prot: IOMMU_READ/WRITE 조합.
 * @return: 0 성공, -EINVAL 이면 만들 수 없는 조합이다.
 *
 * 권한이 하나도 없으면 거절하는 것이 이 형식의 필연이다. 원 주석이 그
 * 이유를 밝힌다 — 존재 비트가 없어 R/W 로 존재를 판별하므로, 둘 다 없는
 * 항목은 빈자리와 구별되지 않는다.
 *
 * 뒤쪽의 읽기 전용 거부는 하드웨어 결함 회피다. ERRATA_772415_SPR17 은
 * 중첩 구성의 부모 도메인에서 읽기 전용 매핑이 오동작하게 만들어, 그
 * 조합을 아예 막는다.
 */
static inline int vtdss_pt_iommu_set_prot(struct pt_common *common,
					  struct pt_write_attrs *attrs,
					  unsigned int iommu_prot)
{
	u64 pte = 0;	/* [한국어] 만들 속성 비트 */

	/*
	 * VTDSS does not have a present bit, so we tell if any entry is present
	 * by checking for R or W.
	 */
	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))	/* [한국어] (원 주석: 존재 비트가 없어 R/W 로 존재를 판별한다) */
		return -EINVAL;	/* [한국어] 둘 다 없으면 빈자리와 구별되지 않는다 */

	if (iommu_prot & IOMMU_READ)	/* [한국어] 읽기 허용이면 */
		pte |= VTDSS_FMT_R;	/* [한국어] 읽기 비트 */
	if (iommu_prot & IOMMU_WRITE)	/* [한국어] 쓰기 허용이면 */
		pte |= VTDSS_FMT_W;	/* [한국어] 쓰기 비트 */
	if (pt_feature(common, PT_FEAT_VTDSS_FORCE_COHERENCE))	/* [한국어] 일관성 강제 기능이 켜져 있으면 */
		pte |= VTDSS_FMT_SNP;	/* [한국어] 장치 접근이 CPU 캐시를 스누프하게 한다 */

	if (pt_feature(common, PT_FEAT_VTDSS_FORCE_WRITEABLE) &&	/* [한국어] 하드웨어 결함 회피가 켜져 있는데 */
	    !(iommu_prot & IOMMU_WRITE)) {	/* [한국어] 읽기 전용을 요청하면 */
		pr_err_ratelimited(	/* [한국어] 거절하는 이유를 알린다 */
			"Read-only mapping is disallowed on the domain which serves as the parent in a nested configuration, due to HW errata (ERRATA_772415_SPR17)\n");
		return -EINVAL;	/* [한국어] 중첩 부모에서 읽기 전용이 오동작한다 */
	}

	attrs->descriptor_bits = pte;	/* [한국어] 잎을 쓸 때 그대로 얹힌다 */
	return 0;	/* [한국어] 성공 */
}
#define pt_iommu_set_prot vtdss_pt_iommu_set_prot	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_iommu_fmt_init - 형식별 초기화
 *
 * @iommu_table: 초기화할 객체.
 * @cfg: 드라이버가 준 설정.
 * @return: 0 성공, -EOPNOTSUPP 이면 지원하지 않는 단계 수다.
 *
 * 3·4·5단계(top_level 2·3·4)를 받는다. x86_64 형식이 3단계를 거부하는
 * 것과 달리 이쪽은 허용하는데, VT-d 하드웨어가 39비트 주소 공간을
 * 실제로 쓰기 때문이다.
 */
static inline int vtdss_pt_iommu_fmt_init(struct pt_iommu_vtdss *iommu_table,
					  const struct pt_iommu_vtdss_cfg *cfg)
{
	struct pt_vtdss *table = &iommu_table->vtdss_pt;	/* [한국어] 형식별 페이지 테이블 상태 */

	if (cfg->top_level > 4 || cfg->top_level < 2)	/* [한국어] 3·4·5단계만 받는다 */
		return -EOPNOTSUPP;	/* [한국어] 그 밖은 하드웨어가 걷지 못한다 */

	pt_top_set_level(&table->common, cfg->top_level);	/* [한국어] 표는 첫 매핑이 만든다 */
	return 0;	/* [한국어] 성공 */
}
#define pt_iommu_fmt_init vtdss_pt_iommu_fmt_init	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * vtdss_pt_iommu_fmt_hw_info - 하드웨어에 적을 값을 꺼낸다
 *
 * @table: 페이지 테이블 객체.
 * @top_range: 최상위 정보를 담은 범위.
 * @info: 채울 결과.
 *
 * drivers/iommu/intel 이 이 둘을 컨텍스트 항목에 적는다. aw 는 명세가
 * 쓰는 주소 폭 코드로, 단계 수와 1 만큼 어긋나 있다 — 원 주석의 대응표가
 * 그것이다.
 */
static inline void
vtdss_pt_iommu_fmt_hw_info(struct pt_iommu_vtdss *table,
			   const struct pt_range *top_range,
			   struct pt_iommu_vtdss_hw_info *info)
{
	info->ssptptr = virt_to_phys(top_range->top_table);	/* [한국어] 컨텍스트 항목에 적을 최상위 표의 물리 주소 */
	PT_WARN_ON(info->ssptptr & ~PT_TOP_PHYS_MASK);	/* [한국어] 그 필드에 담기지 않는 비트가 있으면 안 된다 */
	/*
	 * top_level = 2 = 3 level table aw=1
	 * top_level = 3 = 4 level table aw=2
	 * top_level = 4 = 5 level table aw=3
	 */
	info->aw = top_range->top_level - 1;	/* [한국어] (원 주석: 단계 수와 aw 코드의 대응표) 1 만큼 어긋나 있다 */
}
#define pt_iommu_fmt_hw_info vtdss_pt_iommu_fmt_hw_info	/* [한국어] 공통 API 이름으로 */

#if defined(GENERIC_PT_KUNIT)	/* [한국어] 시험 모듈을 빌드하는 중이면 */
/*
 * [한국어] kunit 이 시험할 설정 목록.
 * 39·48·57비트 주소 공간, 즉 3·4·5단계 표를 모두 돈다.
 */
static const struct pt_iommu_vtdss_cfg vtdss_kunit_fmt_cfgs[] = {
	[0] = { .common.hw_max_vasz_lg2 = 39, .top_level = 2},	/* [한국어] 39비트 = 3단계 */
	[1] = { .common.hw_max_vasz_lg2 = 48, .top_level = 3},	/* [한국어] 48비트 = 4단계 */
	[2] = { .common.hw_max_vasz_lg2 = 57, .top_level = 4},	/* [한국어] 57비트 = 5단계 */
};
#define kunit_fmt_cfgs vtdss_kunit_fmt_cfgs	/* [한국어] 시험 코드가 부르는 이름으로 */
enum { KUNIT_FMT_FEATURES = BIT(PT_FEAT_VTDSS_FORCE_WRITEABLE) };	/* [한국어] 결함 회피 경로도 시험한다 */
#endif	/* [한국어] 포함 방지 끝 */
#endif
