/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * Helper macros for working with log2 values
 *
 */
/*
 * [한국어 설명] 로그2 값으로 하는 산술 헬퍼 (pt_log2.h)
 *
 * === 파일의 역할 ===
 * 페이지 테이블 코드는 크기를 거의 언제나 2의 거듭제곱으로 다룬다. 그럴
 * 때 크기 자체를 들고 다니는 대신 지수(로그2 값)를 들고 다니면 곱셈·나눗셈이
 * 시프트가 되고, 무엇보다 64비트 경계에서 넘치지 않는다.
 *
 * 예를 들어 "주소 공간 전체 크기"는 2^64 라 어떤 정수형에도 담기지 않지만,
 * 지수 64 는 그냥 정수다. 이 파일의 매크로들은 그 표현을 전제로 나눗셈,
 * 나머지, 비교를 시프트와 마스크로 옮겨 준다.
 *
 * 타입을 매번 인자로 받는 것도 이유가 있다. 같은 연산이 가상 주소(형식마다
 * 32/64비트)와 물리 주소(늘 64비트)에 모두 쓰이는데, C 매크로는 타입을
 * 스스로 알 수 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * generic_pt 의 가장 아래층이다. pt_defs.h, pt_common.h, pt_iter.h 와
 * 형식 구현들이 모두 이 매크로로 단계 크기와 경계를 계산한다.
 *
 * 실행 컨텍스트: 전부 인라인 매크로/함수라 호출 비용이 없다. 컴파일 시
 * 검증(static_assert)이 각 매크로 바로 아래 붙어 있어, 정의를 고치면
 * 빌드가 즉시 깨진다.
 *
 * === 타 모듈과의 연결 ===
 * 위: ../generic_pt 의 모든 헤더가 포함한다.
 * 아래: <linux/bitops.h> 의 fls/__ffs/ffz, <linux/limits.h> 의 U32_MAX.
 *
 * 데이터 흐름은 없다. 순수 계산만 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * log2_to_int_t / log2_to_max_int_t: 지수를 값으로, 또는 값-1(하위 비트 마스크)로.
 * log2_div_t / log2_mod_t: 2의 거듭제곱으로 나눈 몫과 나머지.
 * log2_div_eq_t / log2_mod_eq_max_t: 두 주소가 같은 페이지에 있는가,
 *   주소가 페이지의 마지막 바이트인가 — 순회 종료 조건에 쓰인다.
 * log2_set_mod_t / log2_set_mod_max_t: 하위 비트를 특정 값이나 전부 1 로.
 * fls_t / ffs_t / ffz_t: 타입 폭에 맞는 비트 탐색으로 갈라 주는 껍질.
 */
#ifndef __GENERIC_PT_LOG2_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_LOG2_H	/* [한국어] 같은 이름으로 표시 */
#include <linux/bitops.h>	/* [한국어] fls/__ffs/ffz 비트 탐색 */
#include <linux/limits.h>	/* [한국어] U32_MAX — ffz64 의 상위 절반 판정에 쓴다 */

/* Compute a */
#define log2_to_int_t(type, a_lg2) ((type)(((type)1) << (a_lg2)))	/* [한국어] (원 주석: a 를 구한다) 지수를 실제 값으로 */
static_assert(log2_to_int_t(unsigned int, 0) == 1);	/* [한국어] 2^0 = 1 — 컴파일 시 검증 */

/* Compute a - 1 (aka all low bits set) */
#define log2_to_max_int_t(type, a_lg2) ((type)(log2_to_int_t(type, a_lg2) - 1))	/* [한국어] (원 주석: a-1, 즉 하위 비트가 모두 선 값) 오프셋 마스크로 쓴다 */

/* Compute a / b */
#define log2_div_t(type, a, b_lg2) ((type)(((type)a) >> (b_lg2)))	/* [한국어] (원 주석: a/b) 2의 거듭제곱 나눗셈은 시프트다 */
static_assert(log2_div_t(unsigned int, 4, 2) == 1);	/* [한국어] 4/4 = 1 */

/*
 * Compute:
 *   a / c == b / c
 * aka the high bits are equal
 */
#define log2_div_eq_t(type, a, b, c_lg2) \
	(log2_div_t(type, (a) ^ (b), c_lg2) == 0)	/* [한국어] XOR 의 상위 비트가 0 이면 두 주소가 같은 블록 안에 있다 */
static_assert(log2_div_eq_t(unsigned int, 1, 1, 2));	/* [한국어] 같은 값이면 당연히 참 */

/* Compute a % b */
#define log2_mod_t(type, a, b_lg2) \
	((type)(((type)a) & log2_to_max_int_t(type, b_lg2)))	/* [한국어] 하위 비트만 남기면 나머지가 된다 */
static_assert(log2_mod_t(unsigned int, 1, 2) == 1);	/* [한국어] 1 % 4 = 1 */

/*
 * Compute:
 *   a % b == b - 1
 * aka the low bits are all 1s
 */
#define log2_mod_eq_max_t(type, a, b_lg2) \
	(log2_mod_t(type, a, b_lg2) == log2_to_max_int_t(type, b_lg2))	/* [한국어] 하위 비트가 모두 1 이면 그 블록의 마지막 바이트다 */
static_assert(log2_mod_eq_max_t(unsigned int, 3, 2));	/* [한국어] 3 은 4바이트 블록의 마지막 */

/*
 * Return a value such that:
 *    a / b == ret / b
 *    ret % b == val
 * aka set the low bits to val. val must be < b
 */
#define log2_set_mod_t(type, a, val, b_lg2) \
	((((type)(a)) & (~log2_to_max_int_t(type, b_lg2))) | ((type)(val)))	/* [한국어] 상위는 남기고 하위만 val 로 바꾼다 */
static_assert(log2_set_mod_t(unsigned int, 3, 1, 2) == 1);	/* [한국어] 3 의 하위 2비트를 1 로 → 1 */

/* Return a value such that:
 *    a / b == ret / b
 *    ret % b == b - 1
 * aka set the low bits to all 1s
 */
#define log2_set_mod_max_t(type, a, b_lg2) \
	(((type)(a)) | log2_to_max_int_t(type, b_lg2))	/* [한국어] 하위 비트를 모두 세우면 그 블록의 끝 주소가 된다 */
static_assert(log2_set_mod_max_t(unsigned int, 2, 2) == 3);	/* [한국어] 2 가 든 4바이트 블록의 끝은 3 */

/* Compute a * b */
#define log2_mul_t(type, a, b_lg2) ((type)(((type)a) << (b_lg2)))	/* [한국어] (원 주석: a*b) 곱셈도 시프트 */
static_assert(log2_mul_t(unsigned int, 2, 2) == 8);	/* [한국어] 2*4 = 8 */

#define _dispatch_sz(type, fn, a) \
	(sizeof(type) == 4 ? fn##32((u32)a) : fn##64(a))	/* [한국어] 타입 폭을 보고 32/64비트 구현으로 갈라 준다. sizeof 는 상수라 한쪽만 남는다 */

/*
 * Return the highest value such that:
 *    fls_t(u32, 0) == 0
 *    fls_t(u3, 1) == 1
 *    a >= log2_to_int(ret - 1)
 * aka find last set bit
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * fls32 - 32비트 값에서 마지막으로 선 비트를 찾는다
 *
 * @a: 볼 값.
 * @return: 1 기반 비트 번호, 0 이면 선 비트가 없다.
 *
 * 커널의 fls 를 그대로 부른다. 이 껍질이 있는 이유는 이름뿐이다 —
 * _dispatch_sz 가 fn##32 / fn##64 로 이름을 만들어 고르므로, 32비트 쪽
 * 이름이 존재해야 한다.
 */
static inline unsigned int fls32(u32 a)
{
	return fls(a);	/* [한국어] 커널의 fls 를 그대로 — 이름만 폭에 맞춰 준다 */
}
#define fls_t(type, a) _dispatch_sz(type, fls, a)	/* [한국어] (원 주석: 마지막으로 선 비트를 찾는다) */

/*
 * Return the highest value such that:
 *    ffs_t(u32, 0) == UNDEFINED
 *    ffs_t(u32, 1) == 0
 *    log_mod(a, ret) == 0
 * aka find first set bit
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * __ffs32 - 32비트 값에서 처음으로 선 비트를 찾는다
 *
 * @a: 볼 값(0 이면 결과가 정의되지 않는다).
 * @return: 0 기반 비트 번호.
 *
 * 주소가 몇 비트까지 정렬되어 있는지 재는 데 쓴다. 역시 이름을 맞추기 위한
 * 껍질이다.
 */
static inline unsigned int __ffs32(u32 a)
{
	return __ffs(a);	/* [한국어] 커널의 __ffs 를 그대로 */
}
#define ffs_t(type, a) _dispatch_sz(type, __ffs, a)	/* [한국어] (원 주석: 처음으로 선 비트를 찾는다) 정렬 계산에 쓴다 */

/*
 * Return the highest value such that:
 *    ffz_t(u32, U32_MAX) == UNDEFINED
 *    ffz_t(u32, 0) == 0
 *    ffz_t(u32, 1) == 1
 *    log_mod(a, ret) == log_to_max_int(ret)
 * aka find first zero bit
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * ffz32 - 32비트 값에서 처음으로 0 인 비트를 찾는다
 *
 * @a: 볼 값(전부 1 이면 결과가 정의되지 않는다).
 * @return: 0 기반 비트 번호.
 *
 * 하위에서 연속된 1 이 몇 개인지를 재는 것과 같다. "이 주소부터 블록 끝까지
 * 몇 바이트인가"를 구할 때 쓰인다.
 */
static inline unsigned int ffz32(u32 a)
{
	return ffz(a);	/* [한국어] 커널의 ffz 를 그대로 */
}
/*
 * [한국어]
 * ffz64 - 64비트 값에서 처음으로 0 인 비트를 찾는다
 *
 * @a: 볼 값(전부 1 이면 결과가 정의되지 않는다).
 * @return: 0 기반 비트 번호.
 *
 * 64비트 아키텍처에서는 커널의 ffz 가 그대로 처리한다. 32비트 커널에서는
 * unsigned long 이 32비트라 ffz 가 상위 절반을 보지 못하므로, 하위 절반이
 * 전부 1 인지 먼저 확인하고 상위로 넘어간다.
 */
static inline unsigned int ffz64(u64 a)	/* [한국어] 32비트 커널을 위한 손수 만든 구현 */
{
	if (sizeof(u64) == sizeof(unsigned long))	/* [한국어] 64비트 아키텍처면 */
		return ffz(a);	/* [한국어] 커널 구현이 그대로 64비트를 처리한다 */

	if ((u32)a == U32_MAX)	/* [한국어] 32비트 커널: 하위 절반에 0 이 없으면 */
		return ffz32(a >> 32) + 32;	/* [한국어] 상위 절반에서 찾고 자리수를 더한다 */
	return ffz32(a);	/* [한국어] 하위 절반에 0 이 있다 */
}
#define ffz_t(type, a) _dispatch_sz(type, ffz, a)	/* [한국어] (원 주석: 처음으로 0 인 비트를 찾는다) 연속된 1 의 길이를 잰다 */

#endif	/* [한국어] 포함 방지 끝 */
