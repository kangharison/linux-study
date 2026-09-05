/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 *
 * Template to build the iommu module and kunit from the format and
 * implementation headers.
 *
 * The format should have:
 *  #define PT_FMT <name>
 *  #define PT_SUPPORTED_FEATURES (BIT(PT_FEAT_xx) | BIT(PT_FEAT_yy))
 * And optionally:
 *  #define PT_FORCE_ENABLED_FEATURES ..
 *  #define PT_FMT_VARIANT <suffix>
 */
/*
 * [한국어 설명] 형식별 IOMMU 모듈을 찍어 내는 템플릿 (iommu_template.h)
 *
 * === 파일의 역할 ===
 * C 에는 템플릿이 없으므로 전처리기로 흉내 낸다. 이 헤더는 앞서 정의된
 * PT_FMT 를 보고 그 형식의 정의 헤더와 구현 헤더를 골라 포함하고, 이어서
 * 공통 알고리즘을 포함한다. 결과는 그 형식 전용으로 인라인된 코드다.
 *
 * 이름 충돌을 피하는 방법도 여기 있다. PTPFX 라는 접두어를 만들어 두면
 * 공통 코드가 만들어 내는 모든 외부 심볼에 그것이 붙는다 — 그래서 같은
 * 알고리즘을 여러 형식으로 찍어 내도 링크가 충돌하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_<형식>.c 가 매크로를 정의하고 이 파일을 포함한다. 이 파일은
 * 형식 헤더와 공통 구현을 끌어와 실제 코드가 생기게 한다.
 *
 * 같은 .c 파일이 두 번 컴파일되는 점이 특이하다. 한 번은 보통 모듈로,
 * 한 번은 GENERIC_PT_KUNIT 을 켜고 시험 모듈로 — 그래서 시험이 실제로
 * 쓰이는 코드를 그대로 검증한다.
 *
 * === 타 모듈과의 연결 ===
 * 위: fmt/iommu_amdv1.c, iommu_x86_64.c, iommu_vtdss.c, iommu_riscv64.c,
 *     iommu_mock.c 가 모두 이 파일을 포함한다.
 * 아래: <linux/generic_pt/common.h>, defs_<형식>.h, ../pt_defs.h,
 *       <형식>.h, ../pt_common.h, ../iommu_pt.h(또는 kunit 헤더들).
 *
 * === 주요 함수/구조체 요약 ===
 * PTPFX_RAW / PTPFX: 이 형식이 만들어 낼 심볼의 접두어.
 * PT_FMT_H / PT_DEFS_H: 포함할 형식 헤더의 이름을 문자열로 만든 것.
 * GENERIC_PT_KUNIT: 시험 모듈을 빌드하는 중인지 가르는 스위치.
 */
#include <linux/args.h>	/* [한국어] CONCATENATE 매크로 — 형식 이름을 심볼에 이어 붙인다 */
#include <linux/stringify.h>	/* [한국어] __stringify — 헤더 이름을 #include 가 받을 문자열로 */

#ifdef PT_FMT_VARIANT	/* [한국어] 같은 형식에서 여러 변종을 찍어 내는 경우 */
#define PTPFX_RAW \
	CONCATENATE(CONCATENATE(PT_FMT, _), PT_FMT_VARIANT)	/* [한국어] 형식 이름에 변종 이름을 이어 붙인다 */
#else
#define PTPFX_RAW PT_FMT	/* [한국어] 변종이 없으면 형식 이름 그대로 */
#endif

#define PTPFX CONCATENATE(PTPFX_RAW, _)	/* [한국어] 모든 외부 심볼에 붙을 접두어 — 링크 충돌을 막는다 */

#define _PT_FMT_H PT_FMT.h	/* [한국어] 형식 구현 헤더의 이름 */
#define PT_FMT_H __stringify(_PT_FMT_H)	/* [한국어] #include 가 받을 수 있게 문자열로 */

#define _PT_DEFS_H CONCATENATE(defs_, _PT_FMT_H)	/* [한국어] 형식 정의 헤더의 이름 */
#define PT_DEFS_H __stringify(_PT_DEFS_H)	/* [한국어] 역시 문자열로 */

#include <linux/generic_pt/common.h>	/* [한국어] 형식과 무관한 공통 자료형과 기능 비트 */
#include PT_DEFS_H	/* [한국어] 이 형식의 주소 타입과 쓰기 속성 구조체 */
#include "../pt_defs.h"	/* [한국어] 공통 내부 정의 — 위의 형식 정의를 전제로 한다 */
#include PT_FMT_H	/* [한국어] 항목을 읽고 쓰는 형식별 접근자들 */
#include "../pt_common.h"	/* [한국어] 접근자를 전제로 한 공통 헬퍼 */

#ifndef GENERIC_PT_KUNIT	/* [한국어] 보통 모듈을 빌드하는 중이면 */
#include "../iommu_pt.h"	/* [한국어] iommu 코어에 붙는 진입점들을 만든다 */
#else
/*
 * The makefile will compile the .c file twice, once with GENERIC_PT_KUNIT set
 * which means we are building the kunit modle.
 */
#include "../kunit_generic_pt.h"	/* [한국어] (원 주석: 같은 .c 를 KUNIT 을 켜고 한 번 더 컴파일한다) 표 자체의 시험 */
#include "../kunit_iommu_pt.h"	/* [한국어] iommu 진입점 쪽 시험 */
#endif
