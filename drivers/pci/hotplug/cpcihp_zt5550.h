/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * cpcihp_zt5550.h
 *
 * Intel/Ziatech ZT5550 CompactPCI Host Controller driver definitions
 *
 * Copyright 2002 SOMA Networks, Inc.
 * Copyright 2001 Intel San Luis Obispo
 * Copyright 2000,2001 MontaVista Software Inc.
 *
 * Send feedback to <scottm@somanetworks.com>
 */

/* [한국어] 이 헤더가 한 번역 단위에 두 번 이상 펼쳐지는 것을 막는 include 가드 시작.
 * cpcihp_zt5550.c 가 유일한 소비자지만, 매크로 재정의 경고를 원천 차단하기 위한 관례다. */
/*
 * [한국어 설명] Intel/Ziatech ZT5550 CompactPCI 호스트 컨트롤러 레지스터 맵 (cpcihp_zt5550.h)
 *
 * === 파일의 역할 ===
 * ZT5550 CompactPCI(cPCI) 호스트 컨트롤러 칩의 레지스터 오프셋과 인터럽트 비트
 * 정의만을 모아 둔 순수 상수 헤더다. 함수 선언도, 구조체도, 인라인 코드도 없고
 * 오직 #define 만 존재한다. 유일한 소비자는 같은 디렉토리의 cpcihp_zt5550.c 이며,
 * 그 드라이버가 하드웨어에 접근할 때 쓰는 모든 매직 넘버가 여기에 이름으로 묶여 있다.
 * 레지스터는 두 부류로 나뉜다. (1) "직접(direct)" 레지스터는 컨트롤러 PCI 함수의
 * BAR1 MMIO 창 선두에서 해당 오프셋만큼 떨어진 곳을 readb()/writeb() 로 바로 접근한다.
 * (2) "인덱스(indexed)" 레지스터는 MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 번호를 쓴 뒤
 * CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다 — 작은 MMIO 창으로 많은 내부
 * 레지스터를 커버하기 위한 전형적 기법이다. 여기에 더해 #ENUM 핫스왑 신호만은
 * MMIO 가 아닌 레거시 x86 I/O 포트(ENUM_PORT)로 읽는다는 점이 이 하드웨어의 특징이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 핫플러그 스택은 세 층으로 나뉜다. 최상단은 사용자 공간에 sysfs 슬롯 인터페이스를
 * 제공하는 공용 코어(drivers/pci/hotplug/pci_hotplug_core.c), 중간은 CompactPCI 공통
 * 상태 기계(cpci_hotplug_core.c / cpci_hotplug_pci.c), 최하단이 보드별 하드웨어
 * 접근 계층이다. 이 헤더는 그 최하단, 그중에서도 ZT5550 보드 전용 부분에 속한다.
 * 호출 방향으로 보면 cpci_hotplug_core.c 의 폴링 스레드가 struct cpci_hp_controller_ops
 * 콜백(query_enum/enable_irq/disable_irq/check_irq)을 부르고, 그 구현체인
 * cpcihp_zt5550.c 의 zt5550_hc_* 함수들이 이 헤더의 상수로 실제 레지스터를 두드린다.
 * 실행 컨텍스트는 커널 모듈이며, query_enum/enable_irq 계열은 프로세스(커널 스레드)
 * 컨텍스트에서, check_irq 는 인터럽트 판정 경로에서 호출된다 — 그래서 이 헤더가
 * 정의하는 접근은 모두 잠들지 않는 단순 MMIO/포트 I/O 로만 이루어진다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더에 의존하는 쪽은 cpcihp_zt5550.c 하나뿐이다(트리 전체에서 다른 포함처가 없다).
 * 그 드라이버는 초기화 시 hc_registers = ioremap(BAR1) 로 MMIO 창을 잡고
 * (cpcihp_zt5550.c:89) 여기에 CSR_HCINDEX/HCDATA/INTSTAT/INTMASK 오프셋을 더해
 * 네 개의 포인터로 캐시해 둔다(:99~102). 이어서 간접 마스크(HC_INT_MASK_REG)에
 * ALL_INDEXED_INTS_MASK 를, 직접 마스크(CSR_INTMASK)에 ALL_DIRECT_INTS_MASK 를 써서
 * 양쪽 인터럽트를 모두 차단한 상태로 출발한다(:108~116).
 * 데이터 흐름: 백플레인의 #ENUM 신호 -> ENUM_PORT(I/O 포트 0xE1)의 bit 6 ->
 * zt5550_hc_query_enum() -> cpci_hotplug_core.c 의 상태 기계 -> PCI 열거/제거 경로
 * (cpci_hotplug_pci.c) -> 최종적으로 pci_hp_add_bridge()/pci_stop_and_remove_bus_device().
 * 공유 상태: 이 헤더 자체는 상태를 갖지 않는다. 실제 공유 상태는 cpcihp_zt5550.c 의
 * 전역 hc_registers 와 네 개의 csr_* 포인터이며, 이 헤더는 그 포인터들이 가리킬
 * 오프셋만 이름으로 제공한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 이 파일에는 함수도 구조체도 없으므로, 대신 상수 그룹 다섯 개를 요약한다.
 * - CSR_* (0x00~0x14): 직접 레지스터 오프셋. 인덱스/데이터 쌍(HCINDEX/HCDATA)과
 *   인터럽트 상태/마스크(INTSTAT/INTMASK), 그리고 타이머 0/1 의 명령·카운터.
 * - *_INT_MASK + ALL_DIRECT_INTS_MASK: 직접 마스크 레지스터(CSR_INTMASK)의 비트 정의.
 *   비트 1 = 차단, 0 = 허용이라는 극성이 핵심이며, 드라이버가 ENUM 비트만 토글한다.
 * - HC_, ARB_, ISOL_, FAULT_, WD_, SERIAL_ 계열 (0x04~0x3C): 인덱스 레지스터 번호.
 *   이 가운데 HC_INT_MASK_REG 만 실제로 쓰이고 나머지는 하드웨어 문서를 옮겨 둔 미사용 정의다.
 * - SERIAL_INT_MASK, FAULT_INT_MASK, HCF_INT_MASK + ALL_INDEXED_INTS_MASK: 간접 마스크의
 *   비트 정의. 직접 마스크와 값은 겹치지만 대상 레지스터가 다르므로 혼동하면 안 된다.
 * - ENUM_PORT 와 ENUM_MASK: 유일하게 MMIO 가 아닌 레거시 I/O 포트 경로로,
 *   CompactPCI 핫스왑의 실제 트리거인 #ENUM 신호를 읽는다.
 */

#ifndef _CPCIHP_ZT5550_H
/* [한국어] 가드 매크로 정의. 파일명을 대문자로 바꾼 관례적 이름을 쓴다. */
#define _CPCIHP_ZT5550_H

/* Direct registers */
/* [한국어] 호스트 컨트롤러의 인덱스 레지스터 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * 여기에 아래 "Indexed registers" 그룹의 번호(HC_INT_MASK_REG 등)를 writeb 로 써서
 * 다음 CSR_HCDATA 접근이 어느 내부 레지스터를 가리킬지 선택한다.
 * 사용처: cpcihp_zt5550.c:99 에서 csr_hc_index 포인터로 캐시된 뒤 :108 에서 쓰인다. */
#define CSR_HCINDEX		0x00
/* [한국어] 인덱스로 선택된 내부 레지스터의 데이터 창(window) 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * CSR_HCINDEX 에 번호를 쓴 직후 이 주소를 읽으면 해당 레지스터 값이, 쓰면 그 값이 반영된다.
 * 인덱스/데이터 쌍 구조를 쓰는 이유는 BAR 로 노출하는 MMIO 창을 작게 유지하면서도
 * 수십 개의 내부 레지스터에 접근하기 위해서다. 사용처: :100, :109. */
#define CSR_HCDATA		0x04
/* [한국어] 인터럽트 상태 레지스터 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * 비트가 하나라도 1 이면 이 컨트롤러가 인터럽트를 올린 상태다. zt5550_hc_check_irq()
 * (:155)가 이 값을 읽어 0 이 아니면 "내 인터럽트"라고 판정하고 1 을 돌려준다.
 * 즉 공유 IRQ 라인에서 소유권을 가리는 용도다. */
#define CSR_INTSTAT		0x08
/* [한국어] 인터럽트 마스크 레지스터 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * 중요: 이 레지스터는 비트가 1 이면 해당 인터럽트가 "마스크됨(차단)", 0 이면 허용이다.
 * zt5550_hc_enable_irq() 는 ENUM 비트를 AND ~ 로 지워 허용하고(:170),
 * zt5550_hc_disable_irq() 는 OR 로 세워 차단한다(:183) — 극성 판단의 근거다.
 * CSR_INTSTAT(0x08) 바로 다음 바이트(0x09)라는 점에서 두 레지스터는 1바이트 폭이다. */
#define CSR_INTMASK		0x09
/* [한국어] 타이머 0 명령 레지스터 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define CSR_CNT0CMD		0x0C
/* [한국어] 타이머 1 명령 레지스터 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * 0x0C 와 0x0E 로 2바이트 간격인 것으로 보아 명령 레지스터는 16비트 폭이다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define CSR_CNT1CMD		0x0E
/* [한국어] 타이머 0 카운터 값 레지스터 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * 0x10 과 0x14 로 4바이트 간격이므로 카운터는 32비트 폭이다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define CSR_CNT0		0x10
/* [한국어] 타이머 1 카운터 값 레지스터 오프셋. 직접(direct) 레지스터 — 컨트롤러 PCI 함수의 BAR1 MMIO 영역 선두에서 이 오프셋만큼
 * 떨어진 곳을 readb()/writeb() 로 바로 접근한다(인덱스 경유가 필요 없다).
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define CSR_CNT1		0x14

/* Masks for interrupt bits in CSR_INTMASK direct register */
/* [한국어] 타이머 0 인터럽트 비트(bit 0). CSR_INTMASK 에서 이 비트를 1 로 두면 타이머 0
 * 인터럽트가 차단된다. 아래 ALL_DIRECT_INTS_MASK 의 구성 요소다. */
#define CNT0_INT_MASK		0x01
/* [한국어] 타이머 1 인터럽트 비트(bit 1). 의미와 극성은 CNT0_INT_MASK 와 동일하다. */
#define CNT1_INT_MASK		0x02
/* [한국어] #ENUM 신호 인터럽트 비트(bit 2). CompactPCI 핫스왑에서 #ENUM 은 보드가
 * 삽입/추출되려 할 때 백플레인이 어서트하는 신호로, 이 드라이버가 실제로 관심을 갖는
 * 유일한 인터럽트원이다. enable_irq/disable_irq 가 이 비트 하나만 토글한다(:170, :183). */
#define ENUM_INT_MASK		0x04
/* [한국어] 위 세 비트를 모두 켠 값(0x01|0x02|0x04 = 0x07). 초기화 시 이 값을 CSR_INTMASK 에
 * 통째로 써서(:116) 타이머 0/1 과 ENUM 인터럽트를 한 번에 전부 차단한다 —
 * 드라이버가 준비되기 전에 인터럽트가 올라오는 것을 막는 관례적 초기화다. */
#define ALL_DIRECT_INTS_MASK	0x07

/* Indexed registers (through CSR_INDEX, CSR_DATA) */
/* [한국어] 인터럽트 마스크 레지스터의 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 초기화 경로에서 CSR_HCINDEX 에 이 값을 쓴 뒤(:108) CSR_HCDATA 에
 * ALL_INDEXED_INTS_MASK 를 써서(:109) 간접 인터럽트들을 모두 차단한다.
 * 직접 레지스터 CSR_INTMASK(0x09)와는 별개의 두 번째 마스크 레지스터라는 점이 핵심이다. */
#define HC_INT_MASK_REG		0x04
/* [한국어] 호스트 컨트롤러 상태 레지스터의 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define HC_STATUS_REG		0x08
/* [한국어] 호스트 컨트롤러 명령 레지스터의 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define HC_CMD_REG		0x0C
/* [한국어] PCI 버스 아비터의 GNT(grant) 설정 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * CompactPCI 백플레인에서 어느 슬롯에 버스 소유권을 줄지 제어하는 계열이다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define ARB_CONFIG_GNT_REG	0x10
/* [한국어] 아비터 일반 설정 레지스터 인덱스 번호(0x12). 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define ARB_CONFIG_CFG_REG	0x12
/* [한국어] 아비터 설정 레지스터 인덱스 번호. 주의: 값이 0x10 으로 바로 위의
 * ARB_CONFIG_GNT_REG 와 동일하다 — 같은 레지스터를 가리키는 별칭(alias)이거나
 * GNT 설정이 아비터 설정 블록의 첫 레지스터임을 뜻한다. 코드가 어느 쪽도
 * 사용하지 않아 헤더만으로는 확정할 수 없다. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다. */
#define ARB_CONFIG_REG		0x10
/* [한국어] 아이솔레이션(isolation) 설정 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 핫스왑 보드를 버스에서 전기적으로 분리/연결하는 제어와 관련된 계열이다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define ISOL_CONFIG_REG		0x18
/* [한국어] 폴트 상태 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define FAULT_STATUS_REG	0x20
/* [한국어] 폴트 설정 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define FAULT_CONFIG_REG	0x24
/* [한국어] 워치독 타이머 설정 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define WD_CONFIG_REG		0x2C
/* [한국어] 호스트 컨트롤러 진단(diagnostic) 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define HC_DIAG_REG		0x30
/* [한국어] 직렬 통신 제어 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * ZT5550 은 백플레인 관리용 직렬 채널을 갖고 있으며 아래 IN/OUT 레지스터와 짝을 이룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define SERIAL_COMM_REG		0x34
/* [한국어] 직렬 송신 데이터 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define SERIAL_OUT_REG		0x38
/* [한국어] 직렬 수신 데이터 레지스터 인덱스 번호. 인덱스(indexed) 레지스터 번호 — MMIO 로 직접 보이지 않고, CSR_HCINDEX 에 이 번호를
 * 먼저 쓴 뒤 CSR_HCDATA 를 읽거나 쓰는 2단계 접근으로만 다룬다.
 * 현재 in-tree 드라이버(cpcihp_zt5550.c)에서는 참조하지 않는다 — 하드웨어 문서에
 * 맞춰 레지스터 맵 전체를 기록해 둔 것이며, 실제 사용 코드는 없다. */
#define SERIAL_IN_REG		0x3C

/* Masks for interrupt bits in HC_INT_MASK_REG indexed register */
/* [한국어] 직렬 통신 인터럽트 비트(bit 0) — HC_INT_MASK_REG(간접 마스크) 안에서의 위치다.
 * 직접 마스크의 CNT0_INT_MASK 와 값이 0x01 로 같지만 서로 다른 레지스터의 비트이므로
 * 혼동하면 안 된다. */
#define SERIAL_INT_MASK		0x01
/* [한국어] 폴트 인터럽트 비트(bit 1) — 간접 마스크 레지스터 기준. */
#define FAULT_INT_MASK		0x02
/* [한국어] HCF(호스트 컨트롤러 기능) 인터럽트 비트(bit 2) — 간접 마스크 레지스터 기준. */
#define HCF_INT_MASK		0x04
/* [한국어] 위 세 비트를 모두 켠 값(0x07). 초기화 시 CSR_HCDATA 를 통해 HC_INT_MASK_REG 에
 * 이 값을 써서(:109) 직렬/폴트/HCF 인터럽트를 한꺼번에 차단한다.
 * ALL_DIRECT_INTS_MASK 와 값은 같지만 적용 대상 레지스터가 다르다. */
#define ALL_INDEXED_INTS_MASK	0x07

/* Digital I/O port storing ENUM# */
/* [한국어] #ENUM 신호를 읽어 오는 레거시 x86 I/O 포트 주소. MMIO 가 아니라 in/out 명령으로
 * 접근하는 포트 공간이며, 그래서 zt5550_hc_query_enum() 이 readb 가 아닌 inb_p() 를
 * 쓰고(:144) 드라이버가 request_region()/release_region() 으로 이 1바이트 포트를
 * 예약·반납한다(:283, :289, :297). 컨트롤러 BAR 와 무관한 별도 자원이라는 점이 핵심이다. */
#define ENUM_PORT	0xE1
/* Mask to get to the ENUM# bit on the bus */
/* [한국어] ENUM_PORT 에서 읽은 바이트 중 #ENUM 상태가 실린 비트(bit 6, 0x40).
 * zt5550_hc_query_enum() 은 (value & ENUM_MASK) == ENUM_MASK 로 비교해(:145)
 * 해당 비트가 1 일 때 "핫스왑 이벤트 발생"으로 판정한다 — 즉 이 레지스터에서는
 * 1 이 어서트를 뜻하며, 이름의 #(액티브 로우)는 이미 하드웨어가 반전해 준 셈이다. */
#define ENUM_MASK	0x40

/* [한국어] include 가드의 끝. 뒤의 주석은 어떤 #ifndef 에 대응하는 #endif 인지 밝히는 관례다. */
#endif				/* _CPCIHP_ZT5550_H */
