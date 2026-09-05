// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024-2025, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] 셀프테스트용 모의 페이지 테이블 모듈 (iommu_mock.c)
 *
 * === 파일의 역할 ===
 * iommufd 의 셀프테스트가 쓸 가짜 IOMMU 의 페이지 테이블을 만든다. 실제
 * 하드웨어 없이 매핑·해제·무효화 경로를 시험하기 위한 것이다.
 *
 * 바탕은 AMD v1 형식을 그대로 쓴다. 새 형식을 만드는 대신 기존 형식을
 * 시험 모드로 다시 찍어 내는 방식이라, 시험이 검증하는 것이 실제로 쓰이는
 * 코드와 같은 코드가 된다.
 *
 * 기능 집합이 0 인 것이 요점이다. 어떤 선택 기능도 켜지 않은 가장 단순한
 * 동작만 시험한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iommufd/selftest.c → 모의 IOMMU 드라이버 → 이 모듈이 만든 페이지 테이블.
 * 실행 컨텍스트: 커널 모듈(CONFIG_IOMMUFD_TEST 를 켰을 때만 빌드된다).
 *
 * === 타 모듈과의 연결 ===
 * 위: drivers/iommu/iommufd/selftest.c 가 이 형식으로 도메인을 만든다.
 * 아래: ../fmt/amdv1.h 가 항목 형식을, ../iommu_pt.h 가 알고리즘을 준다.
 * 이름이 겹치지 않도록 PT_FMT_VARIANT 로 심볼에 꼬리를 붙인다.
 *
 * === 주요 함수/구조체 요약 ===
 * AMDV1_IOMMUFD_SELFTEST: amdv1.h 가 시험용으로 동작을 바꾸는 스위치.
 * PT_FMT / PT_FMT_VARIANT: 어느 형식을 어떤 이름으로 찍어 낼 것인가.
 * PT_SUPPORTED_FEATURES: 0 — 선택 기능 없이 기본 동작만 시험한다.
 */
#define AMDV1_IOMMUFD_SELFTEST 1	/* [한국어] amdv1.h 가 시험용 변종으로 동작하게 한다 */
#define PT_FMT amdv1	/* [한국어] 바탕은 AMD v1 형식을 그대로 쓴다 — 시험이 실제 코드를 검증한다 */
#define PT_FMT_VARIANT mock	/* [한국어] 진짜 amdv1 모듈과 심볼이 겹치지 않게 한다 */
#define PT_SUPPORTED_FEATURES 0	/* [한국어] 선택 기능 없이 가장 단순한 동작만 */

#include "iommu_template.h"	/* [한국어] 이 한 줄이 형식 전용 코드 전체를 만들어 낸다 */
