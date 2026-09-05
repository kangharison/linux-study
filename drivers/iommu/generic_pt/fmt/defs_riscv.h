/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES
 *
 */
/*
 * [한국어 설명] RISC-V Sv 계열 형식의 기본 자료형 정의 (defs_riscv.h)
 *
 * === 파일의 역할 ===
 * 공통 코드가 컴파일되기 전에 알아야 하는 최소한의 것들만 담는다: 가상
 * 주소와 물리 주소를 몇 비트로 다룰지, 항목을 쓸 때 넘길 속성이 무엇인지.
 *
 * 형식 구현 헤더(riscv.h)와 나뉘어 있는 이유는 포함 순서다. ../pt_defs.h 가
 * 이 자료형들을 전제로 정의를 만들고, 그 결과를 다시 형식 구현이 쓴다.
 * 그래서 자료형만 먼저 알려 줄 파일이 따로 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_<형식>.c → iommu_template.h → [이 파일] → ../pt_defs.h
 *   → riscv.h → ../pt_common.h → ../iommu_pt.h
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
 * riscvpt_write_attrs: 항목 하나를 쓸 때 필요한 값 묶음.
 * pt_write_attrs: 공통 코드가 부르는 이름. 형식마다 다른 구조체를 같은
 *   이름으로 보이게 해 템플릿이 성립한다.
 */
#ifndef __GENERIC_PT_FMT_DEFS_RISCV_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_FMT_DEFS_RISCV_H	/* [한국어] 같은 이름으로 표시 */

#include <linux/generic_pt/common.h>	/* [한국어] 형식과 무관한 공통 정의 */
#include <linux/types.h>	/* [한국어] u32/u64 등 고정 폭 정수 */

#ifdef PT_RISCV_32BIT	/* [한국어] 32비트 변종을 찍어 내는 중인가 */
typedef u32 pt_riscv_entry_t;	/* [한국어] Sv32 는 항목이 32비트 */
#define riscvpt_write_attrs riscv32pt_write_attrs	/* [한국어] 32비트 변종의 심볼 이름 */
#else
typedef u64 pt_riscv_entry_t;	/* [한국어] Sv39/48/57 은 항목이 64비트 */
#define riscvpt_write_attrs riscv64pt_write_attrs	/* [한국어] 64비트 변종의 심볼 이름 */
#endif

typedef pt_riscv_entry_t pt_vaddr_t;	/* [한국어] 입력 주소 폭이 항목 폭을 따라간다 */
typedef u64 pt_oaddr_t;	/* [한국어] 출력 물리 주소는 두 변종 모두 64비트 — Sv32 도 34비트 물리 주소를 낸다 */

struct riscvpt_write_attrs {	/* [한국어] 항목 하나를 쓸 때 필요한 값 묶음 — 폭은 변종에 따라 갈린다 */
	pt_riscv_entry_t descriptor_bits;
	/* [한국어] 항목에 얹을 권한·속성 비트(V, R, W, X, U, A, D 등).
	 * 설정자: 형식 구현이 공통 권한을 RISC-V 항목 배치로 옮긴다.
	 * 읽는 자: 항목을 쓰는 코드가 물리 페이지 번호와 합쳐 최종 값을 만든다.
	 * 값 범위: 주소 필드를 뺀 나머지 비트. 폭이 변종에 따라 32 또는 64비트다.
	 * 동기화: 호출 스택 값. */
	gfp_t gfp;
	/* [한국어] 중간 단계 표 할당에 쓸 플래그.
	 * 설정자: 드라이버의 map 경로.
	 * 읽는 자: 없는 단계를 만드는 공통 코드.
	 * 값 범위: GFP_KERNEL 또는 GFP_ATOMIC.
	 * 동기화: 호출 스택 값. */
};
#define pt_write_attrs riscvpt_write_attrs	/* [한국어] 공통 코드가 부르는 이름으로 */

#endif	/* [한국어] 포함 방지 끝 */
