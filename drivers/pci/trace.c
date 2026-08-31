// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tracepoints for PCI system
 *
 * Copyright (C) 2025 Alibaba Corporation
 */

/*
 * [한국어 설명] PCI 서브시스템의 tracepoint 정의를 실체화하는 파일 (trace.c)
 *
 * === 파일의 역할 ===
 * 파일 전체가 몇 줄뿐이다. 하는 일은 CREATE_TRACE_POINTS 를 정의한 뒤
 * trace 헤더를 include 하는 것뿐이다.
 *
 * 그 한 줄이 왜 필요한가. 커널의 tracepoint 는 헤더 하나(trace.h)에
 * 정의되고 여러 .c 파일이 그 헤더를 include 해 이벤트를 발생시킨다.
 * 그런데 tracepoint 마다 실제 자료구조와 등록 코드가 어딘가 한 곳에는
 * 있어야 한다. CREATE_TRACE_POINTS 를 정의하고 헤더를 include 하면
 * 그 파일에서 실체가 만들어진다.
 *
 * 즉 이 파일은 "tracepoint 의 실체가 놓일 자리" 를 제공하는 것이 전부다.
 * 이런 패턴은 커널 곳곳에서 반복된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * drivers/pci/trace.h  : tracepoint 를 TRACE_EVENT 매크로로 정의
 *   -> [이 파일]        : CREATE_TRACE_POINTS 를 켜고 그 헤더를 include
 *                         -> 컴파일러가 실제 함수와 자료구조를 만든다
 *   -> 다른 .c 파일들   : 그냥 include 해서 trace_*() 를 부른다
 *
 * 실행 컨텍스트: 해당 없음. 실행되는 코드가 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 없다.
 * 아래쪽: drivers/pci/trace.h, include/trace/ 의 tracepoint 인프라.
 * 공유 상태: 만들어진 tracepoint 자체. 다른 파일들이 그것을 참조한다.
 *
 * === NVMe 관점 ===
 * NVMe 와 직접 관련이 없다.
 *
 * 다만 NVMe 문제를 추적할 때 PCI 계층의 tracepoint 가 유용할 수 있다.
 * ftrace 로 그 이벤트를 켜면 config 접근이나 전원 상태 전환이 언제
 * 일어났는지 시간 순서로 볼 수 있다. NVMe 자체의 tracepoint 는
 * drivers/nvme/host/trace.c 에 따로 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * (정의된 함수 없음. CREATE_TRACE_POINTS 와 include 두 줄이 전부다.)
 */

#include <linux/pci.h>

#define CREATE_TRACE_POINTS
#include <trace/events/pci.h>
#include <trace/events/pci_controller.h>
