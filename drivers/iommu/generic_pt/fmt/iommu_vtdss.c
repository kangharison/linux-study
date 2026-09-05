// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] Intel VT-d 2단계(second stage) 페이지 테이블 형식의 IOMMU 모듈 (iommu_vtdss.c)
 *
 * === 파일의 역할 ===
 * 이 파일에는 코드가 한 줄도 없다. 매크로 몇 개를 정의하고 공통 템플릿을
 * 포함하는 것이 전부다. 그 템플릿이 형식 헤더와 구현 헤더를 끌어들여
 * Intel VT-d 2단계(second stage) 형식 전용 함수들을 만들어 낸다.
 *
 * generic_pt 는 페이지 테이블 조작 코드를 형식마다 새로 쓰지 않으려고
 * 만들어졌다. 걷기·매핑·해제·무효화 같은 알고리즘은 형식과 무관하게 같고,
 * 다른 것은 "항목을 어떻게 읽고 쓰는가"뿐이다. 그 차이만 형식 헤더에
 * 두고 나머지는 공유한다.
 *
 * 그 방식이 C 템플릿이다. 매크로로 형식을 고정한 뒤 공통 코드를 그대로
 * 컴파일하면, 인라인된 형식별 접근자가 박힌 전용 코드가 나온다. 함수
 * 포인터를 거치지 않으므로 손으로 쓴 코드와 같은 속도가 난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IOMMU 드라이버 → iommu_pt.h 가 만든 공용 진입점 → 형식 헤더의 접근자.
 * 이 파일은 그 사슬의 맨 아래에서 "어느 형식으로 찍어 낼 것인가"만 정한다.
 *
 * 실행 컨텍스트: 커널 모듈. 빌드 시 커널 이미지나 모듈로 들어간다.
 *
 * === 타 모듈과의 연결 ===
 * 드라이버: drivers/iommu/intel 의 2단계 변환 경로가 이 모듈을 쓴다.
 * 공통 코드: ../iommu_pt.h, ../pt_common.h, vtdss.h.
 *
 * 데이터 흐름: 드라이버가 map/unmap 을 부르면 iommu_pt.h 의 코드가 돌고,
 * 그 안에서 이 형식의 항목 접근자가 인라인되어 실제 메모리를 고친다.
 *
 * === 주요 함수/구조체 요약 ===
 * PT_FMT: 어느 형식 헤더를 끌어올지 정하는 이름.
 * PT_SUPPORTED_FEATURES: 이 형식이 허용하는 기능 비트의 집합. 드라이버가
 *   그 밖의 기능을 요청하면 초기화가 거절된다.
 * iommu_template.h: 실제 코드를 만들어 내는 템플릿.
 */
#define PT_FMT vtdss	/* [한국어] vtdss.h 와 defs_vtdss.h 를 끌어온다 */
/*
 * [한국어] 이 형식이 허용하는 기능 비트.
 * FLUSH_RANGE: 무효화를 범위 단위로 모아 보낼 수 있다.
 * VTDSS_FORCE_COHERENCE / FORCE_WRITEABLE: 항목에 일관성·쓰기 허용을
 *   강제로 세우는 VT-d 고유 비트.
 * DMA_INCOHERENT: 페이지 테이블 자체가 CPU 캐시와 일관되지 않는 플랫폼이라
 *   항목을 고칠 때마다 캐시를 비워야 한다.
 * 드라이버가 여기 없는 기능을 요청하면 초기화가 거절된다.
 */
#define PT_SUPPORTED_FEATURES                                            \
	(BIT(PT_FEAT_FLUSH_RANGE) | BIT(PT_FEAT_VTDSS_FORCE_COHERENCE) | \
	 BIT(PT_FEAT_VTDSS_FORCE_WRITEABLE) | BIT(PT_FEAT_DMA_INCOHERENT))	/* [한국어] 기능 집합의 끝 */

#include "iommu_template.h"	/* [한국어] 이 한 줄이 형식 전용 코드 전체를 만들어 낸다 */
