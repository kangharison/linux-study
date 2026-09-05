/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 *
 */
/*
 * [한국어 설명] Intel VT-d 2단계 형식의 기본 자료형 정의 (defs_vtdss.h)
 *
 * === 파일의 역할 ===
 * 공통 코드가 컴파일되기 전에 알아야 하는 최소한의 것들만 담는다: 가상
 * 주소와 물리 주소를 몇 비트로 다룰지, 항목을 쓸 때 넘길 속성이 무엇인지.
 *
 * 형식 구현 헤더(vtdss.h)와 나뉘어 있는 이유는 포함 순서다. ../pt_defs.h 가
 * 이 자료형들을 전제로 정의를 만들고, 그 결과를 다시 형식 구현이 쓴다.
 * 그래서 자료형만 먼저 알려 줄 파일이 따로 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_<형식>.c → iommu_template.h → [이 파일] → ../pt_defs.h
 *   → vtdss.h → ../pt_common.h → ../iommu_pt.h
 *
 * 실행 컨텍스트: 컴파일 시점에만 존재한다. 실행 코드는 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommu_template.h 가 이름을 만들어 포함한다.
 * 아래: <linux/generic_pt/common.h> 의 공통 정의를 전제한다.
 * 이 파일이 정의한 pt_write_attrs 를 공통 코드가 그대로 들고 다니다가,
 * 항목을 쓸 때 형식 구현에 넘긴다 — 내용은 형식만 안다.
 *
 * === 주요 함수/구조체 요약 ===
 * pt_vaddr_t / pt_oaddr_t: 이 형식이 다루는 입력·출력 주소의 폭.
 * vtdss_pt_write_attrs: 항목 하나를 쓸 때 필요한 값 묶음.
 * pt_write_attrs: 공통 코드가 부르는 이름. 형식마다 다른 구조체를 같은
 *   이름으로 보이게 해 템플릿이 성립한다.
 */
#ifndef __GENERIC_PT_FMT_DEFS_VTDSS_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_FMT_DEFS_VTDSS_H	/* [한국어] 같은 이름으로 표시 */

#include <linux/generic_pt/common.h>	/* [한국어] 형식과 무관한 공통 정의 */
#include <linux/types.h>	/* [한국어] u32/u64 등 고정 폭 정수 */

typedef u64 pt_vaddr_t;	/* [한국어] 입력 주소 64비트 */
typedef u64 pt_oaddr_t;	/* [한국어] 출력 물리 주소 */

struct vtdss_pt_write_attrs {	/* [한국어] 항목 하나를 쓸 때 필요한 값 묶음 — 공통 코드가 내용을 모른 채 들고 다닌다 */
	u64 descriptor_bits;
	/* [한국어] 항목에 얹을 권한·속성 비트(R, W, SNP 등).
	 * 설정자: 형식 구현이 공통 권한을 VT-d 항목 배치로 옮긴다.
	 * 읽는 자: 항목을 쓰는 코드가 물리 주소와 OR 한다.
	 * 값 범위: 주소 필드를 뺀 나머지 비트.
	 * 동기화: 호출 스택 값. */
	gfp_t gfp;
	/* [한국어] 중간 단계 표 할당에 쓸 플래그.
	 * 설정자: 드라이버의 map 경로.
	 * 읽는 자: 없는 단계를 만드는 공통 코드.
	 * 값 범위: GFP_KERNEL 또는 GFP_ATOMIC.
	 * 동기화: 호출 스택 값. */
};
#define pt_write_attrs vtdss_pt_write_attrs	/* [한국어] 공통 코드가 부르는 이름으로 */

#endif	/* [한국어] 포함 방지 끝 */
