// SPDX-License-Identifier: GPL-2.0
/*
 * Intel IOMMU trace support
 *
 * Copyright (C) 2019 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 */

/*
 * [한국어 설명] VT-d 추적점(tracepoint)의 실체를 만드는 파일 (intel/trace.c)
 *
 * === 파일의 역할 ===
 * 코드가 거의 없다. trace.h 에 정의된 추적점들의 실제 구조체와 함수를 이
 * 파일 하나에서만 생성하기 위한 자리다. CREATE_TRACE_POINTS 를 정의한 뒤
 * trace.h 를 포함하면, 그 헤더의 TRACE_EVENT 매크로들이 선언이 아니라
 * 정의로 전개된다.
 * 왜 이런 구조인가: 추적점은 여러 .c 파일에서 호출되지만(cache.c, prq.c 등)
 * 그 실체는 커널 이미지에 하나만 있어야 한다. 헤더를 포함하는 파일마다
 * 정의가 생기면 링크가 실패한다. 그래서 "정의를 만드는 파일" 하나를 따로
 * 두고, 나머지는 같은 헤더를 CREATE_TRACE_POINTS 없이 포함해 선언만 얻는다.
 * 커널 전반의 추적점이 모두 이 관용구를 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * VT-d 드라이버의 관측 계층이다. 무효화가 언제 어디로 몇 번 나갔는지,
 * 페이지 요청이 어떤 내용이었는지가 ftrace 이벤트로 남아, 커널을 다시
 * 빌드하지 않고도 동작을 들여다볼 수 있게 한다.
 * 이 파일 자체는 아무 로직도 갖지 않고 빌드 시점에만 의미가 있다.
 *
 * === 타 모듈과의 연결 ===
 * - trace.h: 실제 추적점 정의가 있는 곳. 이 파일은 그것을 정의로 전개한다.
 * - cache.c: trace_cache_tag_assign/unassign/flush_* 를 호출한다.
 * - prq.c: trace_prq_report 로 페이지 요청 하나하나를 남긴다.
 * - ftrace/perf 서브시스템: 생성된 추적점이 그쪽에 등록되어
 *   /sys/kernel/tracing 으로 노출된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - 이 파일에 함수는 없다. CREATE_TRACE_POINTS 매크로 하나가 전부의 의미다.
 * - 포함하는 <linux/string.h> 와 <linux/types.h> 는 trace.h 의 매크로가
 *   전개될 때 필요한 기본 타입과 문자열 함수를 위한 것이다.
 */
#include <linux/string.h>	/* [한국어] trace.h 의 매크로가 전개될 때 쓰는 문자열 함수 */
#include <linux/types.h>	/* [한국어] 기본 정수 타입 */

#define CREATE_TRACE_POINTS	/* [한국어] 이 정의가 있는 상태로 trace.h 를 포함하면, 그 안의 TRACE_EVENT 들이 선언이 아니라 실제 구조체와 함수 정의로 전개된다. 커널 전체에서 이 파일 하나만 그렇게 한다 */
#include "trace.h"	/* [한국어] 추적점 정의. 다른 .c 파일들은 같은 헤더를 이 매크로 없이 포함해 선언만 얻는다 */
