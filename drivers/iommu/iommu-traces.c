// SPDX-License-Identifier: GPL-2.0
/*
 * iommu trace points
 *
 * Copyright (C) 2013 Shuah Khan <shuah.kh@samsung.com>
 *
 */

/*
 * [한국어 설명] iommu 코어 추적점의 실체를 만드는 파일 (iommu-traces.c)
 *
 * === 파일의 역할 ===
 * 코드가 한 줄도 없다. 추적점(tracepoint)의 실제 구조체와 함수를 커널
 * 이미지에 딱 한 번만 만들어 내기 위한 자리다. CREATE_TRACE_POINTS 를
 * 정의한 뒤 헤더를 포함하면, 그 안의 TRACE_EVENT 매크로들이 선언이 아니라
 * 정의로 전개된다.
 * 왜 이런 구조가 필요한가. 추적점은 여러 .c 파일에서 호출되지만 그 실체는
 * 하나여야 한다. 헤더를 포함하는 파일마다 정의가 생기면 링크가 실패한다.
 * 그래서 "정의를 만드는 파일"을 하나 따로 두고, 나머지는 같은 헤더를
 * 그 매크로 없이 포함해 선언만 얻는다. 커널 전반이 이 관용구를 쓴다.
 * 그다음 EXPORT_TRACEPOINT_SYMBOL_GPL 로 각 추적점을 모듈에게 열어 준다 —
 * IOMMU 드라이버들이 모듈로 빌드될 수 있어, 그쪽에서 이 추적점을 부르려면
 * 심볼이 내보내져 있어야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IOMMU 계층의 관측 창구다. 장치가 그룹에 들어오고 나가는 일, 도메인에
 * 붙는 일, 매핑을 걸고 푸는 일, 그리고 페이지 폴트가 모두 여기 등록된
 * 추적점으로 남는다. 커널을 다시 빌드하지 않고도 /sys/kernel/tracing 을
 * 통해 그 흐름을 들여다볼 수 있다.
 * 이 파일 자체에는 실행 시점의 논리가 없고, 빌드 시점에만 뜻이 있다.
 *
 * === 타 모듈과의 연결 ===
 * - include/trace/events/iommu.h: 실제 추적점 정의가 있는 곳. 이 파일이
 *   그것을 정의로 전개한다.
 * - drivers/iommu/iommu.c: 장치·그룹·도메인 추적점을 호출한다.
 * - 각 벤더 드라이버: 매핑과 폴트 추적점을 호출한다. 모듈로 빌드될 수 있어
 *   아래 EXPORT 가 필요하다.
 * - ftrace/perf 서브시스템: 생성된 추적점이 그쪽에 등록되어 사용자 공간에
 *   노출된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - 이 파일에 함수는 없다. CREATE_TRACE_POINTS 매크로 하나와 그 뒤의
 *   EXPORT 목록이 전부의 의미다.
 * - add_device_to_group / remove_device_from_group: 격리 그룹의 구성 변화.
 * - attach_device_to_domain: 장치가 어느 주소 공간에 묶였는가.
 * - map / unmap: 매핑을 걸고 푸는 일 — 가장 자주 찍히는 추적점이다.
 * - io_page_fault: 장치가 매핑되지 않은 주소를 건드렸을 때.
 */

#include <linux/string.h>	/* [한국어] 추적점 매크로가 전개될 때 쓰는 문자열 함수. */
#include <linux/types.h>	/* [한국어] 기본 정수 타입. */

#define CREATE_TRACE_POINTS	/* [한국어] 이 정의가 있는 상태로 아래 헤더를 포함하면, 그 안의 TRACE_EVENT 들이 선언이 아니라 실제 정의로 전개된다. 커널 전체에서 이 파일 하나만 그렇게 한다. */
#include <trace/events/iommu.h>	/* [한국어] iommu 추적점 정의. 다른 파일들은 같은 헤더를 이 매크로 없이 포함해 선언만 얻는다. */

/* iommu_group_event */
/* [한국어] (위 영어 주석 참고) 격리 그룹의 구성이 바뀔 때 찍히는 추적점들.
 * 어느 장치가 어느 그룹에 묶였는지는 격리 경계를 이해하는 출발점이다. */
EXPORT_TRACEPOINT_SYMBOL_GPL(add_device_to_group);	/* [한국어] 장치가 그룹에 들어왔다 — 모듈 드라이버가 부를 수 있게 내보낸다. */
EXPORT_TRACEPOINT_SYMBOL_GPL(remove_device_from_group);	/* [한국어] 장치가 그룹에서 빠졌다. */

/* iommu_device_event */
/* [한국어] (위 영어 주석 참고) 장치가 주소 공간에 묶이는 사건. */
EXPORT_TRACEPOINT_SYMBOL_GPL(attach_device_to_domain);	/* [한국어] 장치가 도메인에 붙었다 — 이 뒤로 그 장치의 DMA 가 그 페이지 테이블을 거친다. */

/* iommu_map_unmap */
/* [한국어] (위 영어 주석 참고) 매핑을 걸고 푸는 사건. DMA 가 많은 부하에서는
 * 초당 수만 번씩 찍히므로, 켜 두면 그 자체가 부담이 될 수 있다. */
EXPORT_TRACEPOINT_SYMBOL_GPL(map);	/* [한국어] 매핑을 걸었다. */
EXPORT_TRACEPOINT_SYMBOL_GPL(unmap);	/* [한국어] 매핑을 풀었다. */

/* iommu_error */
/* [한국어] (위 영어 주석 참고) 장치가 매핑되지 않은 주소를 건드렸을 때.
 * 드라이버의 DMA 버그를 잡는 가장 직접적인 단서다. */
EXPORT_TRACEPOINT_SYMBOL_GPL(io_page_fault);	/* [한국어] 페이지 폴트가 났다. */
