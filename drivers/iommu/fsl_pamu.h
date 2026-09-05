/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *
 * Copyright (C) 2013 Freescale Semiconductor, Inc.
 */

/*
 * [한국어 설명] Freescale PAMU 하드웨어 레지스터/테이블 정의 헤더 (fsl_pamu.h)
 *
 * === 파일의 역할 ===
 * PAMU(Peripheral Access Management Unit)는 Freescale/NXP QorIQ 계열 PowerPC
 * SoC에 들어 있는 IOMMU에 해당하는 하드웨어다. 이 헤더는 그 하드웨어의
 * "언어"를 정의한다 — CCSR(Configuration, Control and Status Register) 공간의
 * 레지스터 오프셋, 각 레지스터의 비트 필드 마스크/시프트, 그리고 PAMU가 메모리에
 * 두고 읽어 가는 두 개의 큰 테이블(PAACT, OMT)의 엔트리 레이아웃이다.
 * 이 파일에는 실행 코드가 한 줄도 없고, 매크로/구조체 정의와 fsl_pamu.c가
 * 외부에 노출하는 함수 프로토타입만 들어 있다.
 * PAMU의 동작 모델은 다른 IOMMU와 상당히 다르다. ARM SMMU나 인텔 VT-d가
 * 다단계 페이지 테이블로 임의의 IOVA를 변환하는 것과 달리, PAMU는 LIODN
 * (Logical I/O Device Number)이라는 디바이스 식별자 하나마다 PAACE
 * (Peripheral Access Authorization and Control Entry) 한 개를 두고, 그
 * 엔트리에 적힌 "윈도" 하나로 접근 허용 범위와 변환을 표현한다. 현재 리눅스
 * 드라이버는 서브윈도/실제 주소 변환을 쓰지 않고, 오직 (1) LIODN별 접근
 * 허용/차단과 (2) 스태시(stash) 목적지 지정만 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 흐름은 다음과 같다:
 *
 *   [디바이스가 DMA] → PAMU가 LIODN으로 PAACT 인덱싱 → PAACE 검사
 *      → 허용이면 메모리 접근 / 거부면 access violation 인터럽트
 *
 *   [리눅스] fsl_pamu_domain.c (iommu_ops 구현, 도메인/디바이스 attach)
 *              ↓ 호출
 *            fsl_pamu.c (PAACE 필드를 실제로 쓰고 캐시를 무효화)
 *              ↓ 참조
 *            [이 파일] 레지스터 오프셋/비트필드/테이블 레이아웃
 *              ↓
 *            PAMU 하드웨어 (CCSR MMIO + 메모리상의 PAACT/SPAACT/OMT)
 *
 * 실행 컨텍스트: 순수 헤더이므로 컨텍스트가 없지만, 여기서 정의한 값들은
 * 커널 부팅 시(PAMU 초기화)와 디바이스 attach/detach 시(프로세스 컨텍스트),
 * 그리고 access violation 인터럽트 핸들러(인터럽트 컨텍스트) 모두에서 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * - fsl_pamu.c: 이 헤더가 정의한 모든 것을 실제로 쓰는 주 구현부.
 *   PAACT/SPAACT/OMT 테이블을 부팅 때 할당하고, PAMU_PC 레지스터로 하드웨어를
 *   켜고, pamu_config_ppaace()로 PAACE를 채운다.
 * - fsl_pamu_domain.c / fsl_pamu_domain.h: iommu_domain 추상화를 PAMU의
 *   LIODN 모델에 매핑한다. 여기 선언된 pamu_enable_liodn()/pamu_disable_liodn()/
 *   pamu_config_ppaace()를 호출한다.
 * - asm/fsl_pamu_stash.h: 스태시(stash) 목적지 정의. PAMU의 특이 기능으로,
 *   DMA 데이터를 메모리가 아니라 특정 CPU 코어의 캐시에 직접 밀어 넣을 수 있다.
 *   get_stash_id()가 그 목적지 ID를 계산한다.
 * - linux/iommu.h: IOMMU_READ/IOMMU_WRITE 같은 보호 플래그. pamu_config_ppaace()의
 *   prot 인자가 이 플래그를 받아 PAACE_AP_* 값으로 변환된다.
 * 데이터 흐름: 디바이스 트리(u-boot이 채운 fsl,liodn 프로퍼티) → LIODN 번호
 * → PAACT 인덱스 → 이 헤더의 struct paace 레이아웃대로 메모리 기록 →
 * PAMU 캐시 무효화 → 하드웨어가 다음 DMA부터 새 규칙 적용.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct paace: PAACT/SPAACT 테이블의 한 엔트리(64바이트). LIODN 하나의
 *   접근 권한, 목적지(어느 메모리/버스로 보낼지), 윈도 크기, 스태시 설정을 담는다.
 * - struct ome: OMT(Operation Mapping Table)의 한 엔트리. 128바이트 배열로,
 *   디바이스가 낸 I/O 오퍼레이션 인코딩을 코히런시 패브릭이 이해하는 인코딩으로
 *   변환하는 표다. 스태시 동작을 지정하는 데 쓰인다.
 * - struct pamu_mmap_regs: PAACT/SPAACT/OMT 테이블의 물리 주소와 크기를
 *   하드웨어에 알려 주는 레지스터 묶음.
 * - set_bf()/get_bf(): PAACE 안의 압축된 비트 필드를 읽고 쓰는 매크로 쌍.
 * - pamu_config_ppaace(): LIODN 하나의 PAACE를 "전체 주소공간 허용" 형태로
 *   구성하는 핵심 함수(구현은 fsl_pamu.c).
 */

/* [한국어] 중복 인클루드 방지 가드. 이 헤더는 fsl_pamu.c와
 * fsl_pamu_domain.c 양쪽에서 포함되므로 반드시 필요하다. */
#ifndef __FSL_PAMU_H
#define __FSL_PAMU_H

/* [한국어] IOMMU_READ/IOMMU_WRITE 등 보호 플래그와 struct device 관련 선언.
 * pamu_config_ppaace()의 prot 인자가 이 플래그를 받는다. */
#include <linux/iommu.h>
/* [한국어] PCI 디바이스 순회/식별용. PAMU는 PCIe 컨트롤러에도 LIODN을
 * 부여하므로 PCI 계층 정의가 필요하다. */
#include <linux/pci.h>

/* [한국어] PAMU 고유의 스태시(stash) 목적지 정의 — DMA 데이터를 메모리가
 * 아니라 특정 CPU의 L1/L2 캐시나 플랫폼 캐시로 직접 보내는 기능.
 * get_stash_id()가 여기 정의된 상수로 목적지 ID를 만든다. */
#include <asm/fsl_pamu_stash.h>

/* Bit Field macros
 *	v = bit field variable; m = mask, m##_SHIFT = shift, x = value to load
 */
/* [한국어] PAACE의 32비트 워드 안에 여러 필드가 압축되어 있어, 마스크와
 * 시프트를 짝으로 관리한다. 이름 규칙이 핵심이다: 마스크 이름이 M이면
 * 시프트 이름은 반드시 M##_SHIFT여야 하고, 매크로가 그 규칙으로 시프트를
 * 자동 조합한다. 아래 PAACE_AF_AP / PAACE_AF_AP_SHIFT 같은 쌍이 그 예다.
 *
 * set_bf(v, m, x): v에서 m 자리만 지우고 x를 m 위치로 옮겨 끼워 넣는다.
 * ((x) << shift) & m 으로 한 번 더 마스킹하는 이유는, x가 필드 폭을 넘는
 * 값이어도 옆 필드를 침범하지 못하게 막기 위함이다. */
#define set_bf(v, m, x)		(v = ((v) & ~(m)) | (((x) << m##_SHIFT) & (m)))
/* [한국어] get_bf(v, m): v에서 m 자리만 뽑아 오른쪽 끝으로 내려 원래 값으로
 * 복원한다. set_bf의 역연산이다. */
#define get_bf(v, m)		(((v) & (m)) >> m##_SHIFT)

/* PAMU CCSR space */
/* [한국어] PAMU 제어 레지스터(PAMU_PC)에 쓰는 값 두 가지.
 * PGC(PAMU Gate Closed) 비트를 0으로 두면 게이트가 열려 주변장치 접근이
 * 허용된다. 즉 이 상수는 "게이트 닫기 비트 = 0"인 상태를 뜻한다. */
#define PAMU_PGC 0x00000000     /* Allows all peripheral accesses */
/* [한국어] PE(PAMU Enable) 비트. 이 비트를 세워야 PAMU가 실제로 LIODN 기반
 * 접근 검사를 수행한다. 꺼져 있으면 모든 DMA가 무검사로 통과한다. */
#define PAMU_PE 0x40000000      /* enable PAMU                    */

/* PAMU_OFFSET to the next pamu space in ccsr */
/* [한국어] SoC에는 PAMU 인스턴스가 여러 개(코어/버스별로) 있고, CCSR 공간에
 * 4KB 간격으로 나란히 배치된다. 초기화 루프가 이 값만큼 더해 가며 각
 * 인스턴스의 레지스터 블록을 순회한다. */
#define PAMU_OFFSET 0x1000

/* [한국어] 한 PAMU 인스턴스의 레지스터 블록 안에서 struct pamu_mmap_regs가
 * 위치하는 오프셋. 0이므로 블록의 맨 앞이다. */
#define PAMU_MMAP_REGS_BASE 0

/* [한국어] PAMU에게 "PAACT/SPAACT/OMT 테이블이 물리 메모리 어디에 있고
 * 얼마나 큰지"를 알려 주는 MMIO 레지스터 묶음.
 * 각 주소가 상위(ah)/하위(al) 32비트 쌍으로 나뉜 이유는 36비트 이상의
 * 물리 주소를 32비트 레지스터 두 개로 표현하기 위함이다.
 * 부팅 시 fsl_pamu.c의 setup_pamu()가 한 번 채우고, 그 뒤로는 하드웨어만 읽는다. */
struct pamu_mmap_regs {
	u32 ppbah;
	/* [한국어] Primary PAACT Base Address High — PAACT 테이블 물리 주소의 상위 32비트.
	 * 설정자: fsl_pamu.c의 setup_one_pamu()가 부팅 시 한 번 기록.
	 * 읽는 자: PAMU 하드웨어가 LIODN으로 PAACE를 찾을 때마다 참조.
	 * 값 범위: PAACT를 담은 연속 물리 메모리의 시작 주소 >> 32.
	 * 동기화: 부팅 초기 단일 스레드에서만 쓰고, 이후 변경하지 않는다. */

	u32 ppbal;
	/* [한국어] Primary PAACT Base Address Low — PAACT 물리 주소의 하위 32비트.
	 * 설정자/읽는 자/동기화: ppbah와 동일.
	 * 값 범위: 테이블 정렬 요구(보통 테이블 크기 단위 정렬)를 만족하는 주소. */

	u32 pplah;
	/* [한국어] Primary PAACT Limit Address High — PAACT 영역의 끝(경계) 주소 상위 32비트.
	 * 설정자: setup_one_pamu()가 base + PAACT_SIZE로 계산해 기록.
	 * 읽는 자: 하드웨어가 LIODN 인덱싱 결과가 테이블 범위를 벗어나는지 검사할 때.
	 * 왜 필요한가: 범위를 넘는 LIODN 접근을 하드웨어가 access violation으로
	 *              잡아 주어, 잘못된 LIODN이 엉뚱한 메모리를 PAACE로 해석하는 것을 막는다. */

	u32 pplal;
	/* [한국어] Primary PAACT Limit Address Low — 위 경계 주소의 하위 32비트.
	 * 설정자/읽는 자/동기화: pplah와 동일. */

	u32 spbah;
	/* [한국어] Secondary PAACT Base Address High — SPAACT 테이블 물리 주소 상위 32비트.
	 * SPAACT는 하나의 LIODN을 여러 서브윈도로 쪼갤 때 쓰는 보조 테이블이다.
	 * 설정자: setup_one_pamu(). 읽는 자: 하드웨어(서브윈도 사용 시에만).
	 * 값 범위: 현재 리눅스 드라이버는 서브윈도를 쓰지 않지만, 하드웨어가
	 *          레지스터 설정을 요구하므로 테이블을 할당하고 주소를 채워 둔다. */

	u32 spbal;
	/* [한국어] Secondary PAACT Base Address Low — SPAACT 물리 주소 하위 32비트.
	 * 설정자/읽는 자/동기화: spbah와 동일. */

	u32 splah;
	/* [한국어] Secondary PAACT Limit Address High — SPAACT 영역 경계 상위 32비트.
	 * 역할은 pplah와 같으며 대상 테이블만 SPAACT로 바뀐다. */

	u32 splal;
	/* [한국어] Secondary PAACT Limit Address Low — 위 경계의 하위 32비트. */

	u32 obah;
	/* [한국어] OMT Base Address High — 오퍼레이션 매핑 테이블(OMT) 물리 주소 상위 32비트.
	 * 설정자: setup_one_pamu().
	 * 읽는 자: 하드웨어가 indexed 변환 모드에서 PAACE의 omi 인덱스로 OME를 찾을 때.
	 * 왜 필요한가: 스태시 동작(어떤 오퍼레이션을 어떤 캐시 동작으로 바꿀지)이
	 *              이 테이블에 정의되어 있다. */

	u32 obal;
	/* [한국어] OMT Base Address Low — OMT 물리 주소 하위 32비트. */

	u32 olah;
	/* [한국어] OMT Limit Address High — OMT 영역 경계 상위 32비트.
	 * 잘못된 omi 인덱스가 테이블 밖을 가리키는 것을 하드웨어가 잡아낸다. */

	u32 olal;
	/* [한국어] OMT Limit Address Low — OMT 영역 경계 하위 32비트. */
};

/* PAMU Error Registers */
/* [한국어] POES1/POES2 — PAMU Operation Error Status. 하드웨어가 감지한
 * 오퍼레이션 오류(잘못된 트랜잭션 형식 등)의 상태 비트가 모인 레지스터. */
#define PAMU_POES1 0x0040
/* [한국어] POES2 — 위와 짝을 이루는 두 번째 오류 상태 레지스터. */
#define PAMU_POES2 0x0044
/* [한국어] POEAH/POEAL — 오류를 일으킨 트랜잭션의 주소(상위/하위 32비트).
 * 디버깅 시 어떤 주소 접근이 문제였는지 알려 준다. */
#define PAMU_POEAH 0x0048
/* [한국어] 위 오류 주소의 하위 32비트. */
#define PAMU_POEAL 0x004C
/* [한국어] AVS1 — Access Violation Status 1. 접근 위반이 발생하면 하드웨어가
 * 이 레지스터에 원인 비트와 위반 LIODN을 기록한다. 인터럽트 핸들러
 * (fsl_pamu.c의 pamu_av_isr)가 가장 먼저 읽는 레지스터다. */
#define PAMU_AVS1  0x0050
/* [한국어] AV(Access Violation) 비트 — "위반이 하나라도 있었다"는 요약 비트.
 * 핸들러는 이 비트로 자기 인터럽트인지 판별하고, 처리 후 1을 써서 클리어한다. */
#define PAMU_AVS1_AV    0x1
/* [한국어] OTV(Operation Translation Violation) — 디바이스가 낸 오퍼레이션이
 * OMT의 매핑에서 허용되지 않은 종류였다는 뜻(2비트 필드). */
#define PAMU_AVS1_OTV   0x6
/* [한국어] APV(Access Permission Violation) — PAACE의 AP 필드가 허용하지 않는
 * 접근(예: 읽기 전용 윈도에 쓰기)이 시도됐다는 뜻. */
#define PAMU_AVS1_APV   0x78
/* [한국어] WAV(Window Address Violation) — 접근 주소가 PAACE 윈도 범위를
 * 벗어났다는 뜻. */
#define PAMU_AVS1_WAV   0x380
/* [한국어] LAV(LIODN Address Violation) — LIODN 자체가 문제라는 뜻. 대표적으로
 * PAACT에 등록되지 않은 LIODN이 접근을 시도한 경우다(아래 상수 참조). */
#define PAMU_AVS1_LAV   0x1c00
/* [한국어] GCV(Gate Closed Violation) — PAMU 게이트가 닫힌 상태에서 접근이
 * 들어왔다는 뜻. */
#define PAMU_AVS1_GCV   0x2000
/* [한국어] PDV(PAACT Data Violation) — PAACE 자체를 읽는 도중 오류가 났다는 뜻
 * (예: PAACT를 담은 메모리에 ECC 오류). */
#define PAMU_AVS1_PDV   0x4000
/* [한국어] 위 모든 위반 비트를 한데 묶은 마스크. 인터럽트 핸들러가 AVS1에
 * 이 마스크를 그대로 기록해(w1c) 모든 상태 비트를 한 번에 지우는 데 쓴다.
 * 백슬래시로 줄을 이은 것은 한 줄에 다 담기지 않아서다. */
#define PAMU_AV_MASK    (PAMU_AVS1_AV | PAMU_AVS1_OTV | PAMU_AVS1_APV | PAMU_AVS1_WAV \
			 | PAMU_AVS1_LAV | PAMU_AVS1_GCV | PAMU_AVS1_PDV)
/* [한국어] AVS1의 상위 16비트에 위반을 일으킨 LIODN이 실려 온다. 이 시프트로
 * 꺼내면 어느 디바이스가 문제였는지 알 수 있다. */
#define PAMU_AVS1_LIODN_SHIFT 16
/* [한국어] LAV 필드가 이 값이면 "해당 LIODN이 PPAACT에 없다"는 구체적 원인이다.
 * 핸들러는 이 경우 해당 LIODN을 비활성 상태로 만들어 인터럽트 폭주를 막는다. */
#define PAMU_LAV_LIODN_NOT_IN_PPAACT 0x400

/* [한국어] AVS2 — Access Violation Status 2. AVS1에 담지 못한 추가 상태 비트. */
#define PAMU_AVS2  0x0054
/* [한국어] AVAH/AVAL — 접근 위반을 일으킨 주소(상위/하위 32비트).
 * 어떤 DMA 주소가 거부됐는지 진단하는 데 쓴다. */
#define PAMU_AVAH  0x0058
/* [한국어] 위 위반 주소의 하위 32비트. */
#define PAMU_AVAL  0x005C
/* [한국어] EECTL — ECC Error Control. PAMU 내부 SRAM(캐시)의 ECC 동작을 제어한다. */
#define PAMU_EECTL 0x0060
/* [한국어] EEDIS — ECC Error Disable. 특정 ECC 오류 보고를 끄는 마스크. */
#define PAMU_EEDIS 0x0064
/* [한국어] EEINTEN — ECC Error Interrupt Enable. 어떤 ECC 오류에 대해
 * 인터럽트를 낼지 선택한다. */
#define PAMU_EEINTEN 0x0068
/* [한국어] EEDET — ECC Error Detect. 실제로 감지된 ECC 오류의 상태 비트. */
#define PAMU_EEDET 0x006C
/* [한국어] EEATTR — ECC Error Attributes. 오류가 난 트랜잭션의 속성(어느 캐시,
 * 어떤 접근이었는지). */
#define PAMU_EEATTR 0x0070
/* [한국어] EEAHI/EEALO — ECC 오류가 난 주소(상위/하위). */
#define PAMU_EEAHI 0x0074
/* [한국어] 위 ECC 오류 주소의 하위 32비트. */
#define PAMU_EEALO 0x0078
/* [한국어] EEDHI/EEDLO — ECC 오류가 난 데이터 값(상위/하위). 어떤 비트가
 * 뒤집혔는지 확인하는 데 쓴다. 대문자 X로 쓰인 0X007C는 원본 표기 그대로다. */
#define PAMU_EEDHI 0X007C
/* [한국어] 위 ECC 오류 데이터의 하위 32비트. */
#define PAMU_EEDLO 0x0080
/* [한국어] EECC — ECC Error Capture/Check bits. 계산된 체크비트를 담는다. */
#define PAMU_EECC  0x0084
/* [한국어] UDAD — User Defined Attribute Detect. 구현 정의 속성 관련 레지스터. */
#define PAMU_UDAD  0x0090

/* PAMU Revision Registers */
/* [한국어] PR1 — PAMU 하드웨어 리비전 레지스터. 실리콘 버전에 따른 동작 차이를
 * 판별하는 데 쓴다. */
#define PAMU_PR1 0x0BF8
/* [한국어] PR2 — 두 번째 리비전 레지스터(추가 버전 정보). */
#define PAMU_PR2 0x0BFC

/* PAMU version mask */
/* [한국어] PR1에서 실제 버전 번호에 해당하는 하위 16비트만 남기는 마스크.
 * 상위 비트는 예약 영역이라 무시해야 한다. */
#define PAMU_PR1_MASK 0xffff

/* PAMU Capabilities Registers */
/* [한국어] PC1~PC4 — 이 PAMU 인스턴스가 지원하는 기능/용량을 알려 주는
 * 읽기 전용 레지스터들. 최대 LIODN 개수, 윈도 개수 등이 여기서 나온다. */
#define PAMU_PC1 0x0C00
/* [한국어] PC2 — 상위 16비트에 최대 LIODN 값이 들어 있다(PAMU_PC2_MLIODN 참조). */
#define PAMU_PC2 0x0C04
/* [한국어] PC3 — 최대 윈도 개수 인코딩(MWCE)이 들어 있다(PAMU_PC3_MWCE 참조). */
#define PAMU_PC3 0x0C08
/* [한국어] PC4 — 추가 능력 비트. 현재 드라이버는 사용하지 않는다. */
#define PAMU_PC4 0x0C0C

/* PAMU Control Register */
/* [한국어] PC — PAMU 주 제어 레지스터의 오프셋. 아래 PAMU_CONTROL과 같은 값이다. */
#define PAMU_PC 0x0C10

/* PAMU control defs */
/* [한국어] PAMU_CONTROL — PAMU_PC와 동일한 오프셋의 다른 이름.
 * 코드 가독성을 위해 두 이름이 공존한다. */
#define PAMU_CONTROL 0x0C10
/* [한국어] PGC(PAMU Gate Closed) 비트. 1이면 게이트가 닫혀 모든 주변장치
 * 접근이 차단된다. 초기화 중에 세워 두었다가 테이블 준비가 끝나면 내린다. */
#define PAMU_PC_PGC 0x80000000  /* PAMU gate closed bit */
/* [한국어] PE(PAMU Enable) 비트. 이 비트가 서야 LIODN 기반 검사가 활성화된다. */
#define PAMU_PC_PE   0x40000000 /* PAMU enable bit */
/* [한국어] SPCC — Secondary PAACE Cache enable. SPAACE 조회 결과를 PAMU 내부
 * 캐시에 담아 반복 조회를 빠르게 한다. 캐시가 있으므로 PAACE를 고친 뒤에는
 * 반드시 무효화가 필요하다. */
#define PAMU_PC_SPCC 0x00000010 /* sPAACE cache enable */
/* [한국어] PPCC — Primary PAACE Cache enable. 위와 같은 이유로 PAACE 갱신 후
 * 캐시 무효화가 필수다. */
#define PAMU_PC_PPCC 0x00000001 /* pPAACE cache enable */
/* [한국어] OCE — OMT Cache Enable. 오퍼레이션 매핑 테이블 조회 결과를 캐시한다. */
#define PAMU_PC_OCE  0x00001000 /* OMT cache enable */

/* [한국어] PFA1/PFA2 — PAMU Fetch Address. 하드웨어가 테이블을 페치할 때 쓰는
 * 주소 관련 레지스터. 진단 목적으로 읽는다. */
#define PAMU_PFA1 0x0C14
/* [한국어] PFA2 — 위와 짝이 되는 두 번째 페치 주소 레지스터. */
#define PAMU_PFA2 0x0C18

/* [한국어] PC2 레지스터 값에서 최대 LIODN 번호를 뽑는 매크로.
 * 상위 16비트에 실려 있으므로 16비트 오른쪽 시프트로 꺼낸다.
 * 드라이버는 이 값으로 "이 LIODN이 하드웨어가 지원하는 범위인가"를 검사한다. */
#define PAMU_PC2_MLIODN(X) ((X) >> 16)
/* [한국어] PC3 레지스터에서 MWCE(Maximum Window Count Encoding)를 뽑는 매크로.
 * 비트 21~24의 4비트 필드로, 한 PAACE가 가질 수 있는 최대 서브윈도 개수를
 * 2의 지수 형태로 인코딩한다. */
#define PAMU_PC3_MWCE(X) (((X) >> 21) & 0xf)

/* PAMU Interrupt control and Status Register */
/* [한국어] PICS — 인터럽트 제어/상태 레지스터. 접근 위반 인터럽트를 켜고
 * 상태를 확인하는 창구다. */
#define PAMU_PICS 0x0C1C
/* [한국어] 접근 위반이 발생했음을 나타내는 상태 비트. 인터럽트 핸들러가
 * 처리 후 이 비트를 다시 써서 클리어한다(write-1-to-clear). */
#define PAMU_ACCESS_VIOLATION_STAT   0x8
/* [한국어] 접근 위반 시 인터럽트를 발생시킬지 결정하는 활성화 비트.
 * 초기화 때 세워 두어야 pamu_av_isr()이 호출된다. */
#define PAMU_ACCESS_VIOLATION_ENABLE 0x4

/* PAMU Debug Registers */
/* [한국어] PD1~PD4 — 디버그 레지스터. 하드웨어 내부 상태를 들여다보는 용도로,
 * 정상 동작 경로에서는 쓰이지 않는다. */
#define PAMU_PD1 0x0F00
/* [한국어] PD2 — 두 번째 디버그 레지스터. */
#define PAMU_PD2 0x0F04
/* [한국어] PD3 — 세 번째 디버그 레지스터. */
#define PAMU_PD3 0x0F08
/* [한국어] PD4 — 네 번째 디버그 레지스터. */
#define PAMU_PD4 0x0F0C

/* [한국어] PAACE의 AP(Access Permission) 필드 값 — 이 윈도에 대한 접근을
 * 완전히 거부한다. LIODN을 비활성화할 때 쓴다. */
#define PAACE_AP_PERMS_DENIED  0x0
/* [한국어] AP = Query만 허용 — 읽기(로드)는 되지만 쓰기는 안 된다.
 * IOMMU_READ 프로텍션에 대응한다. */
#define PAACE_AP_PERMS_QUERY   0x1
/* [한국어] AP = Update만 허용 — 쓰기(스토어)는 되지만 읽기는 안 된다.
 * IOMMU_WRITE 프로텍션에 대응한다. */
#define PAACE_AP_PERMS_UPDATE  0x2
/* [한국어] AP = 읽기/쓰기 모두 허용. 리눅스 드라이버가 디바이스를 도메인에
 * 붙일 때 기본으로 쓰는 값이다. */
#define PAACE_AP_PERMS_ALL     0x3

/* [한국어] DD(Destination Domain) 필드 값 — 이 접근을 호스트 메모리(코히런시
 * 도메인)로 보낸다. domain_attr 유니온의 to_host 쪽이 유효해진다. */
#define PAACE_DD_TO_HOST       0x0
/* [한국어] DD = I/O 목적지 — 접근을 다른 I/O 버스로 보낸다(피어 투 피어).
 * 이때는 domain_attr 유니온의 to_io 쪽이 유효하다. */
#define PAACE_DD_TO_IO         0x1
/* [한국어] PT(PAACE Type) 필드 값 — 이 엔트리는 PAACT(1차 테이블)의 엔트리다. */
#define PAACE_PT_PRIMARY       0x0
/* [한국어] PT = 2차 — 이 엔트리는 SPAACT(서브윈도 테이블)의 엔트리다. */
#define PAACE_PT_SECONDARY     0x1
/* [한국어] V(Valid) 필드 = 0 — 이 PAACE는 무효하며 하드웨어가 무시한다
 * (해당 LIODN의 접근은 위반으로 처리된다). */
#define PAACE_V_INVALID        0x0
/* [한국어] V = 1 — 이 PAACE가 유효하다. LIODN을 활성화하는 마지막 단계에서
 * 이 비트를 세운다(나머지 필드를 먼저 채운 뒤에 세워야 안전하다). */
#define PAACE_V_VALID          0x1
/* [한국어] MW(Multiple Windows) = 1 — 이 PAACE가 여러 서브윈도로 쪼개져
 * SPAACT를 참조한다는 뜻. 현재 리눅스 드라이버는 이 모드를 쓰지 않는다. */
#define PAACE_MW_SUBWINDOWS    0x1

/* [한국어] WSE(Window Size Encoding) 값들 — 윈도 크기를 2의 지수로 인코딩한다.
 * 규칙: 실제 크기 = 2^(WSE+1). 예를 들어 0xB = 11이면 2^12 = 4KB다.
 * 아래 값들은 4K부터 4G까지 2배씩 커지는 사다리를 이룬다. */
#define PAACE_WSE_4K           0xB
/* [한국어] 8KB 윈도 (2^13). */
#define PAACE_WSE_8K           0xC
/* [한국어] 16KB 윈도 (2^14). */
#define PAACE_WSE_16K          0xD
/* [한국어] 32KB 윈도 (2^15). */
#define PAACE_WSE_32K          0xE
/* [한국어] 64KB 윈도 (2^16). */
#define PAACE_WSE_64K          0xF
/* [한국어] 128KB 윈도 (2^17). */
#define PAACE_WSE_128K         0x10
/* [한국어] 256KB 윈도 (2^18). */
#define PAACE_WSE_256K         0x11
/* [한국어] 512KB 윈도 (2^19). */
#define PAACE_WSE_512K         0x12
/* [한국어] 1MB 윈도 (2^20). */
#define PAACE_WSE_1M           0x13
/* [한국어] 2MB 윈도 (2^21). */
#define PAACE_WSE_2M           0x14
/* [한국어] 4MB 윈도 (2^22). */
#define PAACE_WSE_4M           0x15
/* [한국어] 8MB 윈도 (2^23). */
#define PAACE_WSE_8M           0x16
/* [한국어] 16MB 윈도 (2^24). */
#define PAACE_WSE_16M          0x17
/* [한국어] 32MB 윈도 (2^25). */
#define PAACE_WSE_32M          0x18
/* [한국어] 64MB 윈도 (2^26). */
#define PAACE_WSE_64M          0x19
/* [한국어] 128MB 윈도 (2^27). */
#define PAACE_WSE_128M         0x1A
/* [한국어] 256MB 윈도 (2^28). */
#define PAACE_WSE_256M         0x1B
/* [한국어] 512MB 윈도 (2^29). */
#define PAACE_WSE_512M         0x1C
/* [한국어] 1GB 윈도 (2^30). */
#define PAACE_WSE_1G           0x1D
/* [한국어] 2GB 윈도 (2^31). */
#define PAACE_WSE_2G           0x1E
/* [한국어] 4GB 윈도 (2^32) — WSE가 표현할 수 있는 최대 크기.
 * 현재 드라이버는 "전체 주소공간 허용"을 표현하려고 이 값을 쓴다. */
#define PAACE_WSE_4G           0x1F

/* [한국어] DID(Destination ID) 값들 — 이 LIODN의 접근이 최종적으로 어느
 * 목적지로 라우팅되는지를 지정한다. 코히런시 패브릭의 포트 번호에 해당한다.
 * 아래는 PCI Express 컨트롤러 1번. */
#define PAACE_DID_PCI_EXPRESS_1 0x00
/* [한국어] PCI Express 컨트롤러 2번. */
#define PAACE_DID_PCI_EXPRESS_2 0x01
/* [한국어] PCI Express 컨트롤러 3번. */
#define PAACE_DID_PCI_EXPRESS_3 0x02
/* [한국어] PCI Express 컨트롤러 4번. */
#define PAACE_DID_PCI_EXPRESS_4 0x03
/* [한국어] 로컬 버스(eLBC) — NOR/NAND 플래시 등이 붙는 옛 방식의 버스. */
#define PAACE_DID_LOCAL_BUS     0x04
/* [한국어] SRIO(Serial RapidIO) 컨트롤러 — 임베디드/통신 장비용 고속 인터커넥트. */
#define PAACE_DID_SRIO          0x0C
/* [한국어] 메모리 컨트롤러 1번. 접근을 특정 DDR 컨트롤러로 고정할 때 쓴다. */
#define PAACE_DID_MEM_1         0x10
/* [한국어] 메모리 컨트롤러 2번. */
#define PAACE_DID_MEM_2         0x11
/* [한국어] 메모리 컨트롤러 3번. */
#define PAACE_DID_MEM_3         0x12
/* [한국어] 메모리 컨트롤러 4번. */
#define PAACE_DID_MEM_4         0x13
/* [한국어] 메모리 컨트롤러 1과 2에 인터리브된 영역. */
#define PAACE_DID_MEM_1_2       0x14
/* [한국어] 메모리 컨트롤러 3과 4에 인터리브된 영역. */
#define PAACE_DID_MEM_3_4       0x15
/* [한국어] 메모리 컨트롤러 1~4 전체에 인터리브된 영역. */
#define PAACE_DID_MEM_1_4       0x16
/* [한국어] BMan(Buffer Manager) 소프트웨어 포털 — DPAA(Data Path Acceleration
 * Architecture)의 버퍼 관리 블록. */
#define PAACE_DID_BM_SW_PORTAL  0x18
/* [한국어] PAMU 자기 자신 — PAMU가 테이블을 페치하는 접근을 가리킨다. */
#define PAACE_DID_PAMU          0x1C
/* [한국어] CAAM(Cryptographic Acceleration and Assurance Module) — 암호 가속기. */
#define PAACE_DID_CAAM          0x21
/* [한국어] QMan(Queue Manager) 소프트웨어 포털 — DPAA의 큐 관리 블록. */
#define PAACE_DID_QM_SW_PORTAL  0x3C
/* [한국어] 코어 0의 명령어(instruction) 캐시. 스태시 목적지로 지정하면
 * DMA 데이터가 그 코어의 I-캐시로 들어간다. */
#define PAACE_DID_CORE0_INST    0x80
/* [한국어] 코어 0의 데이터 캐시. 실제 스태시는 거의 항상 이쪽을 쓴다 —
 * 네트워크 패킷을 처리할 코어의 L1 D-캐시에 미리 넣어 두면 지연이 크게 준다. */
#define PAACE_DID_CORE0_DATA    0x81
/* [한국어] 코어 1의 명령어 캐시. */
#define PAACE_DID_CORE1_INST    0x82
/* [한국어] 코어 1의 데이터 캐시. */
#define PAACE_DID_CORE1_DATA    0x83
/* [한국어] 코어 2의 명령어 캐시. */
#define PAACE_DID_CORE2_INST    0x84
/* [한국어] 코어 2의 데이터 캐시. */
#define PAACE_DID_CORE2_DATA    0x85
/* [한국어] 코어 3의 명령어 캐시. */
#define PAACE_DID_CORE3_INST    0x86
/* [한국어] 코어 3의 데이터 캐시. */
#define PAACE_DID_CORE3_DATA    0x87
/* [한국어] 코어 4의 명령어 캐시. */
#define PAACE_DID_CORE4_INST    0x88
/* [한국어] 코어 4의 데이터 캐시. */
#define PAACE_DID_CORE4_DATA    0x89
/* [한국어] 코어 5의 명령어 캐시. */
#define PAACE_DID_CORE5_INST    0x8A
/* [한국어] 코어 5의 데이터 캐시. */
#define PAACE_DID_CORE5_DATA    0x8B
/* [한국어] 코어 6의 명령어 캐시. */
#define PAACE_DID_CORE6_INST    0x8C
/* [한국어] 코어 6의 데이터 캐시. */
#define PAACE_DID_CORE6_DATA    0x8D
/* [한국어] 코어 7의 명령어 캐시. */
#define PAACE_DID_CORE7_INST    0x8E
/* [한국어] 코어 7의 데이터 캐시. */
#define PAACE_DID_CORE7_DATA    0x8F
/* [한국어] 브로드캐스트 — 모든 목적지로 보낸다. 특정 코어를 지정하지 않고
 * 플랫폼 캐시 전체에 스태시할 때 쓰인다. */
#define PAACE_DID_BROADCAST     0xFF

/* [한국어] ATM(Address Translation Mode) = 변환 없음.
 * 디바이스가 낸 주소를 그대로 물리 주소로 쓴다. 리눅스 드라이버가
 * "PAMU를 접근 제어용으로만 쓰고 주소 변환은 하지 않는" 기본 모드다. */
#define PAACE_ATM_NO_XLATE      0x00
/* [한국어] ATM = 윈도 변환. 윈도 베이스를 기준으로 오프셋을 더해 변환한다. */
#define PAACE_ATM_WINDOW_XLATE  0x01
/* [한국어] ATM = 페이지 변환. 페이지 단위 재배치를 수행한다. */
#define PAACE_ATM_PAGE_XLATE    0x02
/* [한국어] ATM = 윈도 + 페이지 변환을 모두 적용. 두 비트를 OR 한 값이다. */
#define PAACE_ATM_WIN_PG_XLATE  (PAACE_ATM_WINDOW_XLATE | PAACE_ATM_PAGE_XLATE)
/* [한국어] OTM(Operation Translation Mode) = 변환 없음.
 * 디바이스가 낸 오퍼레이션 인코딩을 그대로 패브릭에 전달한다. */
#define PAACE_OTM_NO_XLATE      0x00
/* [한국어] OTM = immediate 모드. 변환할 인코딩을 PAACE 안(op_encode.immed_ot)에
 * 직접 적어 둔다. 최대 4개까지만 표현할 수 있다. */
#define PAACE_OTM_IMMEDIATE     0x01
/* [한국어] OTM = indexed 모드. PAACE에는 OMT 인덱스(omi)만 두고, 실제 128개
 * 매핑은 OMT의 OME에서 찾는다. 스태시 설정은 이 모드를 쓴다. */
#define PAACE_OTM_INDEXED       0x02
/* [한국어] OTM 예약값 — 하드웨어가 정의하지 않은 조합이라 써서는 안 된다. */
#define PAACE_OTM_RESERVED      0x03

/* [한국어] domain_attr.to_host.coherency_required 필드에 넣는 값.
 * 1이면 이 디바이스의 접근이 캐시 코히런시 프로토콜에 참여해야 한다는 뜻으로,
 * CPU 캐시와의 일관성이 하드웨어로 보장된다(소프트웨어 캐시 플러시 불필요). */
#define PAACE_M_COHERENCE_REQ   0x01

/* [한국어] PID(Partition ID) 값 0~7 — 하이퍼바이저 파티션 식별자.
 * 여러 파티션이 같은 하드웨어를 나눠 쓸 때 어느 파티션 소유인지 표시한다.
 * 리눅스가 단독으로 돌 때는 0을 쓴다. */
#define PAACE_PID_0             0x0
/* [한국어] 파티션 1. */
#define PAACE_PID_1             0x1
/* [한국어] 파티션 2. */
#define PAACE_PID_2             0x2
/* [한국어] 파티션 3. */
#define PAACE_PID_3             0x3
/* [한국어] 파티션 4. */
#define PAACE_PID_4             0x4
/* [한국어] 파티션 5. */
#define PAACE_PID_5             0x5
/* [한국어] 파티션 6. */
#define PAACE_PID_6             0x6
/* [한국어] 파티션 7. */
#define PAACE_PID_7             0x7

/* [한국어] TCEF(Translation Control Entry Format) = 포맷 0, 8바이트 엔트리.
 * 페이지 변환을 쓸 때 변환 테이블 엔트리의 형식을 지정한다. */
#define PAACE_TCEF_FORMAT0_8B   0x00
/* [한국어] TCEF 포맷 1은 예약되어 있다 — 사용 금지. */
#define PAACE_TCEF_FORMAT1_RSVD 0x01
/*
 * Hard coded value for the PAACT size to accommodate
 * maximum LIODN value generated by u-boot.
 */
/* [한국어] PAACT 테이블의 엔트리 개수(0x500 = 1280).
 * 왜 하드코딩인가: LIODN 번호를 u-boot이 디바이스 트리에 채워 주는데,
 * 리눅스는 부팅 시점에 최대 LIODN이 얼마인지 미리 알 방법이 없다. 그래서
 * u-boot이 생성하는 최대값을 넉넉히 덮는 크기로 테이블을 고정 할당한다.
 * 이 값이 곧 지원 가능한 LIODN의 상한이 된다. */
#define PAACE_NUMBER_ENTRIES    0x500
/* Hard coded value for the SPAACT size */
/* [한국어] SPAACT(서브윈도 테이블)의 엔트리 개수(0x800 = 2048).
 * 현재 드라이버는 서브윈도를 쓰지 않지만, 하드웨어가 베이스/리밋 레지스터
 * 설정을 요구하므로 테이블을 할당해 둔다. */
#define SPAACE_NUMBER_ENTRIES	0x800

/* [한국어] OMT의 엔트리(OME) 개수. PAACE의 omi 필드가 이 범위의 인덱스를 담는다.
 * 16개면 충분한 이유는, 실제로 필요한 오퍼레이션 매핑 조합이 몇 가지
 * (스태시 없음 / 읽기 스태시 / 쓰기 스태시 등)로 한정되기 때문이다. */
#define	OME_NUMBER_ENTRIES      16

/* PAACE Bit Field Defines */
/* [한국어] 아래 마스크/시프트 쌍들은 앞서 정의한 set_bf()/get_bf()와 함께 쓰인다.
 * 이름 규칙(마스크 X, 시프트 X_SHIFT)을 반드시 지켜야 매크로가 동작한다.
 *
 * PPAACE_AF_WBAL — Primary PAACE의 addr_bitfields 안에 있는 Window Base
 * Address Low 필드. 윈도 시작 주소의 하위 부분(4KB 단위)이다. */
#define PPAACE_AF_WBAL			0xfffff000
/* [한국어] WBAL 필드의 시프트량 12 — 주소가 4KB 정렬이라 하위 12비트가
 * 항상 0이므로, 그 자리를 다른 필드에 내주고 12비트 시프트해 저장한다. */
#define PPAACE_AF_WBAL_SHIFT		12
/* [한국어] PPAACE_AF_WSE — 같은 워드 안의 Window Size Encoding 필드 마스크
 * (비트 6~11). 위 PAACE_WSE_* 값 중 하나가 여기 들어간다. */
#define PPAACE_AF_WSE			0x00000fc0
/* [한국어] WSE 필드의 시프트량 6. */
#define PPAACE_AF_WSE_SHIFT		6
/* [한국어] PPAACE_AF_MW — Multiple Windows 비트(비트 5). 1이면 이 PAACE가
 * 서브윈도로 쪼개져 SPAACT를 참조한다. */
#define PPAACE_AF_MW			0x00000020
/* [한국어] MW 비트의 시프트량 5. */
#define PPAACE_AF_MW_SHIFT		5

/* [한국어] SPAACE_AF_LIODN — Secondary PAACE의 addr_bitfields 상위 16비트에
 * 들어가는 LIODN 필드. 서브윈도 엔트리가 어느 LIODN 소속인지 표시한다. */
#define SPAACE_AF_LIODN			0xffff0000
/* [한국어] 위 LIODN 필드의 시프트량 16. */
#define SPAACE_AF_LIODN_SHIFT		16

/* [한국어] PAACE_AF_AP — Primary/Secondary 공통으로 addr_bitfields의 비트 3~4에
 * 있는 Access Permission 필드. 값은 PAACE_AP_PERMS_* 중 하나다.
 * 드라이버가 LIODN을 켜고 끌 때 실제로 바꾸는 필드가 바로 이것이다. */
#define PAACE_AF_AP			0x00000018
/* [한국어] AP 필드의 시프트량 3. */
#define PAACE_AF_AP_SHIFT		3
/* [한국어] PAACE_AF_DD — Destination Domain 비트(비트 2). PAACE_DD_TO_HOST 또는
 * PAACE_DD_TO_IO. 이 값에 따라 domain_attr 유니온의 어느 쪽이 유효한지 정해진다. */
#define PAACE_AF_DD			0x00000004
/* [한국어] DD 비트의 시프트량 2. */
#define PAACE_AF_DD_SHIFT		2
/* [한국어] PAACE_AF_PT — PAACE Type 비트(비트 1). primary인지 secondary인지 표시. */
#define PAACE_AF_PT			0x00000002
/* [한국어] PT 비트의 시프트량 1. */
#define PAACE_AF_PT_SHIFT		1
/* [한국어] PAACE_AF_V — Valid 비트(비트 0). 이 엔트리를 하드웨어가 인정할지 결정한다.
 * 나머지 필드를 모두 채운 뒤 마지막에 세워야 반쯤 채워진 엔트리가 쓰이는
 * 사고를 막을 수 있다. */
#define PAACE_AF_V			0x00000001
/* [한국어] V 비트의 시프트량 0 — 최하위 비트이므로 시프트가 없다.
 * 그래도 set_bf/get_bf의 이름 규칙을 맞추려면 반드시 정의해야 한다. */
#define PAACE_AF_V_SHIFT		0

/* [한국어] PAACE_DA_HOST_CR — domain_attr.to_host.coherency_required 바이트의
 * 최상위 비트(0x80). "이 접근은 코히런시가 필요하다"를 나타낸다. */
#define PAACE_DA_HOST_CR		0x80
/* [한국어] 위 코히런시 비트의 시프트량 7 — 1바이트 필드의 최상위 비트다. */
#define PAACE_DA_HOST_CR_SHIFT		7

/* [한국어] PAACE_IA_CID — impl_attr의 비트 16~23에 있는 Cache ID(스태시 목적지).
 * get_stash_id()가 계산한 값이 여기 들어가, DMA 데이터가 어느 캐시로 갈지 정한다. */
#define PAACE_IA_CID			0x00FF0000
/* [한국어] CID 필드의 시프트량 16. */
#define PAACE_IA_CID_SHIFT		16
/* [한국어] PAACE_IA_WCE — Window Count Encoding(비트 4~7). 서브윈도 개수를
 * 2의 지수로 인코딩한다. 서브윈도를 안 쓰면 0이다. */
#define PAACE_IA_WCE			0x000000F0
/* [한국어] WCE 필드의 시프트량 4. */
#define PAACE_IA_WCE_SHIFT		4
/* [한국어] PAACE_IA_ATM — Address Translation Mode(비트 2~3).
 * PAACE_ATM_* 값이 들어가며, 리눅스는 보통 NO_XLATE를 쓴다. */
#define PAACE_IA_ATM			0x0000000C
/* [한국어] ATM 필드의 시프트량 2. */
#define PAACE_IA_ATM_SHIFT		2
/* [한국어] PAACE_IA_OTM — Operation Translation Mode(비트 0~1).
 * PAACE_OTM_* 값이 들어간다. 스태시를 쓰려면 INDEXED로 설정해야 한다. */
#define PAACE_IA_OTM			0x00000003
/* [한국어] OTM 필드의 시프트량 0 — 최하위 필드지만 이름 규칙상 정의가 필요하다. */
#define PAACE_IA_OTM_SHIFT		0

/* [한국어] PAACE_WIN_TWBAL — win_bitfields 안의 Translated Window Base Address
 * Low(비트 12~31). 주소 변환을 켰을 때 윈도가 매핑될 물리 주소의 하위 부분. */
#define PAACE_WIN_TWBAL			0xfffff000
/* [한국어] TWBAL 필드의 시프트량 12 — 역시 4KB 정렬 덕분에 하위 12비트를 생략한다. */
#define PAACE_WIN_TWBAL_SHIFT		12
/* [한국어] PAACE_WIN_SWSE — Sub-Window Size Encoding(비트 6~11).
 * 서브윈도 하나의 크기를 2의 지수로 인코딩한다. */
#define PAACE_WIN_SWSE			0x00000fc0
/* [한국어] SWSE 필드의 시프트량 6. */
#define PAACE_WIN_SWSE_SHIFT		6

/* PAMU Data Structures */
/* primary / secondary paact structure */
/* [한국어] PAACT(1차)와 SPAACT(2차) 두 테이블이 공유하는 엔트리 레이아웃.
 * 크기는 정확히 64바이트(0x40)이며, 하드웨어가 이 레이아웃 그대로 메모리에서
 * 읽어 간다. 따라서 필드 순서와 크기를 절대 바꿀 수 없다.
 * 인덱싱: PAACT 베이스 + LIODN * sizeof(struct paace).
 * 설정자: fsl_pamu.c의 pamu_config_ppaace()/pamu_update_paace_stash() 등.
 * 읽는 자: PAMU 하드웨어(DMA가 들어올 때마다). 소프트웨어가 쓴 뒤에는
 *          반드시 PAMU 캐시를 무효화해야 하드웨어가 새 값을 본다.
 * 동기화: fsl_pamu.c가 도메인/디바이스 락 아래에서 갱신하고, 하드웨어와의
 *         가시성은 캐시 무효화 시퀀스로 보장한다. */
struct paace {
	/* PAACE Offset 0x00 */
	u32 wbah;				/* only valid for Primary PAACE */
	/* [한국어] Window Base Address High — 윈도 시작 물리 주소의 상위 비트.
	 * 설정자: pamu_config_ppaace()가 윈도를 정의할 때 기록(현재는 0).
	 * 읽는 자: 하드웨어가 접근 주소가 윈도 안인지 판정할 때.
	 * 값 범위: 36비트 이상 물리 주소의 상위 부분. Primary PAACE에서만 유효하며
	 *          Secondary에서는 의미가 없다(원본 주석의 지적 그대로).
	 * 동기화: 엔트리 전체가 V 비트를 세우기 전에 채워지므로, 부분 갱신 문제가 없다. */

	u32 addr_bitfields;		/* See P/S PAACE_AF_* */
	/* [한국어] 여러 필드가 압축된 32비트 워드. set_bf()/get_bf()로만 다뤄야 한다.
	 * 담고 있는 것: WBAL(윈도 베이스 하위, 비트 12~31), WSE(윈도 크기, 6~11),
	 * MW(서브윈도 사용, 5), AP(접근 권한, 3~4), DD(목적지 도메인, 2),
	 * PT(엔트리 종류, 1), V(유효, 0). Secondary PAACE에서는 상위 16비트가
	 * WBAL 대신 LIODN(SPAACE_AF_LIODN)으로 해석된다.
	 * 설정자: pamu_config_ppaace()가 대부분을, pamu_enable/disable_liodn()이
	 *          AP와 V 필드만 골라서 갱신한다.
	 * 읽는 자: 하드웨어가 매 DMA마다.
	 * 동기화: V 비트를 마지막에 세우는 순서 규칙이 사실상의 동기화 장치다. */

	/* PAACE Offset 0x08 */
	/* Interpretation of first 32 bits dependent on DD above */
	/* [한국어] 목적지 속성. 위 addr_bitfields의 DD 비트 값에 따라 아래 두 해석 중
	 * 하나만 유효하다 — DD가 TO_HOST면 to_host, TO_IO면 to_io.
	 * 유니온인 이유는 하드웨어가 같은 4바이트를 두 가지로 해석하기 때문이다. */
	union {
		/* [한국어] DD == PAACE_DD_TO_HOST일 때의 해석 — 접근이 호스트
		 * 메모리(코히런시 도메인)로 갈 때 필요한 속성들. */
		struct {
			/* Destination ID, see PAACE_DID_* defines */
			u8 did;
			/* [한국어] 목적지 ID — PAACE_DID_* 중 하나.
			 * 설정자: pamu_config_ppaace()가 보통 PAACE_DID_PCI_EXPRESS_1
			 *          같은 고정값이나 스태시 대상 코어의 캐시 ID로 설정.
			 * 읽는 자: 하드웨어가 트랜잭션을 라우팅할 때.
			 * 값 범위: 위 PAACE_DID_* 표의 값들(0x00~0xFF).
			 * 동기화: 엔트리 갱신 규칙(V 비트 마지막)을 따른다. */

			/* Partition ID */
			u8 pid;
			/* [한국어] 파티션 ID — 하이퍼바이저가 여러 게스트에게 하드웨어를
			 * 나눠 줄 때 이 엔트리의 소유 파티션을 표시한다.
			 * 설정자: 리눅스 단독 부팅에서는 0(PAACE_PID_0)으로 남는다.
			 * 읽는 자: 하드웨어 및 하이퍼바이저.
			 * 값 범위: PAACE_PID_0 ~ PAACE_PID_7.
			 * 동기화: 위와 동일. */

			/* Snoop ID */
			u8 snpid;
			/* [한국어] 스눕 ID — 이 접근이 어느 스눕 도메인에 참여할지 지정.
			 * 설정자: pamu_config_ppaace()가 기본값으로 둔다.
			 * 읽는 자: 코히런시 패브릭.
			 * 값 범위: 플랫폼이 정의하는 스눕 그룹 번호.
			 * 동기화: 위와 동일. */

			/* coherency_required : 1 reserved : 7 */
			u8 coherency_required; /* See PAACE_DA_* */
			/* [한국어] 최상위 1비트만 의미가 있고(PAACE_DA_HOST_CR = 0x80)
			 * 나머지 7비트는 예약이다. 1이면 이 디바이스의 접근이 CPU 캐시와
			 * 하드웨어 코히런시를 유지해야 한다는 뜻으로, 드라이버가 명시적
			 * 캐시 플러시를 하지 않아도 된다.
			 * 설정자: pamu_config_ppaace()가 PAACE_DA_HOST_CR을 세운다.
			 * 읽는 자: 코히런시 패브릭.
			 * 값 범위: 0(비코히런트) 또는 0x80(코히런트).
			 * 동기화: 위와 동일. */
		} to_host;
		/* [한국어] DD == PAACE_DD_TO_IO일 때의 해석 — 접근이 다른 I/O 버스로
		 * 나갈 때(피어 투 피어). 코히런시 개념이 없어 필드가 훨씬 단순하다. */
		struct {
			/* Destination ID, see PAACE_DID_* defines */
			u8  did;
			/* [한국어] 목적지 I/O 버스의 ID — PAACE_DID_PCI_EXPRESS_* 등.
			 * 설정자: I/O 목적지를 쓰는 구성에서만 채워진다(리눅스는 미사용).
			 * 읽는 자: 하드웨어 라우팅 로직.
			 * 값 범위: PAACE_DID_* 표의 I/O 계열 값.
			 * 동기화: 엔트리 갱신 규칙을 따른다. */

			u8  reserved1;
			/* [한국어] 예약 바이트 — 하드웨어가 정의하지 않은 자리.
			 * 설정자/읽는 자: 없음. 0으로 두어야 한다.
			 * 값 범위: 0.
			 * 왜 존재하는가: to_host 쪽 구조체와 크기(4바이트)를 맞춰
			 *                유니온이 하드웨어 레이아웃과 일치하게 하기 위함이다. */

			u16 reserved2;
			/* [한국어] 두 번째 예약 필드(2바이트). 위와 같은 이유로 존재하며
			 * 0으로 유지해야 한다. reserved1과 합쳐 to_host의 snpid +
			 * coherency_required 자리를 덮는다. */
		} to_io;
	} domain_attr;	/* [한국어] 유니온의 끝 — DD 비트가 어느 해석을 고를지 결정한다. */

	/* Implementation attributes + window count + address & operation translation modes */
	u32 impl_attr;			/* See PAACE_IA_* */
	/* [한국어] 구현 속성 워드. 역시 여러 필드가 압축되어 있어 set_bf/get_bf로 다룬다.
	 * 담고 있는 것: CID(스태시 캐시 ID, 비트 16~23), WCE(서브윈도 개수, 4~7),
	 * ATM(주소 변환 모드, 2~3), OTM(오퍼레이션 변환 모드, 0~1).
	 * 설정자: pamu_config_ppaace()가 ATM/OTM을, pamu_update_paace_stash()가
	 *          CID를 갱신한다 — 스태시 목적지를 바꾸는 것이 곧 이 필드를 바꾸는 것이다.
	 * 읽는 자: 하드웨어가 매 트랜잭션마다.
	 * 값 범위: 각 하위 필드의 정의를 따른다.
	 * 동기화: 스태시 변경은 런타임에도 일어나므로, 갱신 후 PAMU 캐시 무효화가 필수다. */

	/* PAACE Offset 0x10 */
	/* Translated window base address */
	u32 twbah;
	/* [한국어] Translated Window Base Address High — 주소 변환을 켰을 때
	 * 윈도가 매핑될 물리 주소의 상위 비트.
	 * 설정자: 변환 모드를 쓰는 구성에서만 채운다(리눅스는 NO_XLATE라 0).
	 * 읽는 자: 하드웨어의 주소 변환 로직.
	 * 값 범위: 물리 주소 상위 부분.
	 * 동기화: 엔트리 갱신 규칙을 따른다. */

	u32 win_bitfields;			/* See PAACE_WIN_* */
	/* [한국어] 윈도 관련 압축 필드 워드.
	 * 담고 있는 것: TWBAL(변환된 윈도 베이스 하위, 비트 12~31),
	 * SWSE(서브윈도 크기 인코딩, 6~11).
	 * 설정자: 변환/서브윈도를 쓰는 구성에서만.
	 * 읽는 자: 하드웨어.
	 * 값 범위: PAACE_WIN_* 마스크/시프트 정의를 따른다.
	 * 동기화: 엔트리 갱신 규칙을 따른다. */

	/* PAACE Offset 0x18 */
	/* first secondary paace entry */
	u32 fspi;				/* only valid for Primary PAACE */
	/* [한국어] First Secondary PAACE Index — 이 PAACE가 서브윈도를 쓸 때,
	 * SPAACT 안에서 첫 번째 서브윈도 엔트리가 있는 인덱스.
	 * 설정자: 서브윈도 구성 시에만(현재 리눅스 드라이버는 사용하지 않음).
	 * 읽는 자: 하드웨어가 서브윈도를 조회할 때 SPAACT 베이스 + fspi로 접근.
	 * 값 범위: 0 ~ SPAACE_NUMBER_ENTRIES-1. Primary PAACE에서만 유효하다.
	 * 동기화: 엔트리 갱신 규칙을 따른다. */

	/* [한국어] 오퍼레이션 인코딩 설정. impl_attr의 OTM 값에 따라 아래 두 해석 중
	 * 하나만 유효하다 — IMMEDIATE면 immed_ot, INDEXED면 index_ot. */
	union {
		/* [한국어] OTM == PAACE_OTM_IMMEDIATE일 때 — 변환 결과를 PAACE 안에
		 * 직접 적어 둔다. 4바이트뿐이라 매핑을 4개까지만 표현할 수 있다. */
		struct {
			u8 ioea;
			/* [한국어] Incoming Operation Encoding A — 디바이스가 낸
			 * 오퍼레이션 인코딩 중 첫 번째로 매칭할 값.
			 * 설정자: immediate 모드를 쓰는 구성.
			 * 읽는 자: 하드웨어의 오퍼레이션 변환 로직.
			 * 값 범위: IOE_* 상수들.
			 * 동기화: 엔트리 갱신 규칙을 따른다. */

			u8 moea;
			/* [한국어] Mapped Operation Encoding A — ioea에 매칭됐을 때
			 * 대신 내보낼 인코딩. EOE_* 상수 중 하나가 들어간다.
			 * 설정자/읽는 자/동기화: ioea와 동일. */

			u8 ioeb;
			/* [한국어] Incoming Operation Encoding B — 두 번째 매칭 대상.
			 * 설정자/읽는 자/동기화: ioea와 동일. */

			u8 moeb;
			/* [한국어] Mapped Operation Encoding B — ioeb에 대응하는 출력 인코딩.
			 * 설정자/읽는 자/동기화: ioea와 동일. */
		} immed_ot;
		/* [한국어] OTM == PAACE_OTM_INDEXED일 때 — 실제 매핑 128개는 OMT의
		 * OME에 두고, 여기에는 그 인덱스만 적는다. 스태시 설정이 이 경로다. */
		struct {
			u16 reserved;
			/* [한국어] 예약 필드(2바이트). immed_ot의 ioea/moea 자리를 덮으며
			 * 0으로 유지해야 한다.
			 * 왜 존재하는가: omi를 유니온의 뒤쪽 2바이트에 정렬시키기 위한
			 *                자리 채우기다(하드웨어 레이아웃 고정). */

			u16 omi;
			/* [한국어] Operation Mapping Index — OMT 테이블에서 이 LIODN이
			 * 사용할 OME의 인덱스.
			 * 설정자: fsl_pamu_domain.c가 get_ome_index()로 얻은 값을
			 *          pamu_config_ppaace()에 넘겨 여기 기록한다.
			 * 읽는 자: 하드웨어가 OMT 베이스 + omi * sizeof(struct ome)로 조회.
			 * 값 범위: 0 ~ OME_NUMBER_ENTRIES-1 (0~15).
			 * 동기화: 엔트리 갱신 규칙을 따른다. */
		} index_ot;
	} op_encode;	/* [한국어] 유니온의 끝 — OTM 값이 immediate/indexed 중 어느 해석을 쓸지 결정한다. */

	/* PAACE Offsets 0x20-0x38 */
	u32 reserved[8];			/* not currently implemented */
	/* [한국어] 예약 영역 32바이트. 이 배열이 있어야 struct paace가 정확히
	 * 64바이트가 되어 하드웨어가 기대하는 엔트리 간격과 일치한다.
	 * 설정자/읽는 자: 없음(0으로 유지).
	 * 값 범위: 0.
	 * 왜 필요한가: 크기가 어긋나면 LIODN 인덱싱이 통째로 틀어져
	 *              엉뚱한 엔트리를 읽게 된다 — 절대 줄이면 안 되는 패딩이다. */
};

/* OME : Operation mapping entry
 * MOE : Mapped Operation Encodings
 * The operation mapping table is table containing operation mapping entries (OME).
 * The index of a particular OME is programmed in the PAACE entry for translation
 * in bound I/O operations corresponding to an LIODN. The OMT is used for translation
 * specifically in case of the indexed translation mode. Each OME contains a 128
 * byte mapped operation encoding (MOE), where each byte represents an MOE.
 */
/* [한국어] 한 OME가 담는 MOE(Mapped Operation Encoding)의 개수 = 128.
 * 왜 128인가: 디바이스가 낼 수 있는 인바운드 오퍼레이션 인코딩이 7비트
 * (0~127) 공간이라, 인덱스 하나당 바이트 하나로 전부 표현할 수 있다.
 * 즉 OME는 "인바운드 인코딩 → 아웃바운드 인코딩" 128칸짜리 룩업 테이블이다. */
#define NUM_MOE 128
/* [한국어] OMT(Operation Mapping Table)의 한 엔트리.
 * 디바이스가 낸 오퍼레이션 인코딩(IOE_* 계열)을 인덱스로 써서 이 배열을 읽으면,
 * 코히런시 패브릭에 실제로 내보낼 인코딩(EOE_* 계열)이 나온다.
 * 이것이 스태시의 실현 방식이다 — 평범한 "읽기"를 "스태시 할당을 동반한 읽기"
 * (EOE_RSA)로 바꿔치기하는 것이 곧 DMA 데이터를 캐시로 끌어오는 동작이다.
 * 설정자: fsl_pamu.c의 setup_omt()가 부팅 시 채운다.
 * 읽는 자: PAMU 하드웨어(indexed OTM 모드일 때).
 * __packed인 이유: 컴파일러가 패딩을 넣으면 128바이트 정렬이 깨져
 *                  하드웨어가 기대하는 레이아웃과 어긋나기 때문이다. */
struct ome {
	u8 moe[NUM_MOE];
	/* [한국어] 인바운드 오퍼레이션 인코딩을 인덱스로 하는 128칸 변환 표.
	 * 설정자: setup_omt()가 moe[IOE_READ] = EOE_VALID | EOE_RSA 같은 식으로
	 *          필요한 칸만 채운다. EOE_VALID(0x80) 비트가 없으면 그 칸은
	 *          "매핑 없음"으로 취급된다.
	 * 읽는 자: 하드웨어가 DMA 트랜잭션마다 조회한다(OMT 캐시가 있으면 캐시에서).
	 * 값 범위: EOE_VALID | EOE_* 조합, 또는 0(무효).
	 * 동기화: 부팅 시 한 번 채우고 이후 바꾸지 않으므로 런타임 락이 없다. */
} __packed;

/* [한국어] PAACT 테이블 전체 크기 = 엔트리 크기(64B) × 엔트리 개수(0x500).
 * 부팅 시 이만큼의 연속 물리 메모리를 할당하고 ppbah/ppbal에 등록한다. */
#define PAACT_SIZE              (sizeof(struct paace) * PAACE_NUMBER_ENTRIES)
/* [한국어] SPAACT 테이블 전체 크기. 서브윈도용이며 엔트리 레이아웃은 PAACT와 같다.
 * 들여쓰기가 위 줄과 한 칸 다른 것은 원본 그대로다. */
#define SPAACT_SIZE              (sizeof(struct paace) * SPAACE_NUMBER_ENTRIES)
/* [한국어] OMT 테이블 전체 크기 = 128바이트 × 16엔트리 = 2KB. */
#define OMT_SIZE                (sizeof(struct ome) * OME_NUMBER_ENTRIES)

/* [한국어] PAMU가 다루는 페이지 크기의 지수(2^12 = 4KB).
 * 윈도 베이스 주소를 12비트 시프트해 저장하는 근거가 바로 이 정렬이다. */
#define PAMU_PAGE_SHIFT 12
/* [한국어] PAMU 페이지 크기 4KB. ULL로 쓴 이유는 36비트 이상 물리 주소 계산에서
 * 32비트 오버플로가 나지 않게 하기 위함이다. */
#define PAMU_PAGE_SIZE  4096ULL

/* [한국어] 아래 IOE_*는 디바이스가 낸 "인바운드 오퍼레이션 인코딩" 값들이다.
 * 같은 동작에 대해 두 가지 상수가 정의된 것에 주의: 이름에 _IDX가 없는 쪽은
 * 하드웨어가 버스에 싣는 실제 인코딩(최상위 valid 비트 0x80 포함)이고,
 * _IDX가 붙은 쪽은 그 값에서 valid 비트를 뺀, OME 배열의 인덱스로 쓸 값이다.
 * 아래는 일반 읽기(인코딩 0x00). */
#define IOE_READ        0x00
/* [한국어] 일반 읽기의 OME 인덱스 — 0x00이라 위와 값이 같다. */
#define IOE_READ_IDX    0x00
/* [한국어] 일반 쓰기의 버스 인코딩(0x81 = valid 비트 0x80 | 인덱스 0x01). */
#define IOE_WRITE       0x81
/* [한국어] 일반 쓰기의 OME 인덱스(0x01). */
#define IOE_WRITE_IDX   0x01
/* [한국어] 확장 읽기 타입 0의 버스 인코딩. */
#define IOE_EREAD0      0x82    /* Enhanced read type 0 */
/* [한국어] 확장 읽기 타입 0의 OME 인덱스. */
#define IOE_EREAD0_IDX  0x02    /* Enhanced read type 0 */
/* [한국어] 확장 쓰기 타입 0의 버스 인코딩. */
#define IOE_EWRITE0     0x83    /* Enhanced write type 0 */
/* [한국어] 확장 쓰기 타입 0의 OME 인덱스. */
#define IOE_EWRITE0_IDX 0x03    /* Enhanced write type 0 */
/* [한국어] 디렉티브(제어 명령) 타입 0의 버스 인코딩. */
#define IOE_DIRECT0     0x84    /* Directive type 0 */
/* [한국어] 디렉티브 타입 0의 OME 인덱스. */
#define IOE_DIRECT0_IDX 0x04    /* Directive type 0 */
/* [한국어] 확장 읽기 타입 1의 버스 인코딩. */
#define IOE_EREAD1      0x85    /* Enhanced read type 1 */
/* [한국어] 확장 읽기 타입 1의 OME 인덱스. */
#define IOE_EREAD1_IDX  0x05    /* Enhanced read type 1 */
/* [한국어] 확장 쓰기 타입 1의 버스 인코딩. */
#define IOE_EWRITE1     0x86    /* Enhanced write type 1 */
/* [한국어] 확장 쓰기 타입 1의 OME 인덱스. */
#define IOE_EWRITE1_IDX 0x06    /* Enhanced write type 1 */
/* [한국어] 디렉티브 타입 1의 버스 인코딩. */
#define IOE_DIRECT1     0x87    /* Directive type 1 */
/* [한국어] 디렉티브 타입 1의 OME 인덱스. */
#define IOE_DIRECT1_IDX 0x07    /* Directive type 1 */
/* [한국어] RAC(Read with Atomic Clear) — 읽으면서 원자적으로 0을 쓰는 연산.
 * 세마포어/락 구현에 쓰인다. */
#define IOE_RAC         0x8c    /* Read with Atomic clear */
/* [한국어] RAC의 OME 인덱스. */
#define IOE_RAC_IDX     0x0c    /* Read with Atomic clear */
/* [한국어] RAS(Read with Atomic Set) — 읽으면서 원자적으로 1을 세우는 연산
 * (test-and-set에 해당). */
#define IOE_RAS         0x8d    /* Read with Atomic set */
/* [한국어] RAS의 OME 인덱스. */
#define IOE_RAS_IDX     0x0d    /* Read with Atomic set */
/* [한국어] RAD(Read with Atomic Decrement) — 읽으면서 원자적으로 1 감소. */
#define IOE_RAD         0x8e    /* Read with Atomic decrement */
/* [한국어] RAD의 OME 인덱스. */
#define IOE_RAD_IDX     0x0e    /* Read with Atomic decrement */
/* [한국어] RAI(Read with Atomic Increment) — 읽으면서 원자적으로 1 증가.
 * 참조 카운트를 디바이스가 직접 올릴 때 쓴다. */
#define IOE_RAI         0x8f    /* Read with Atomic increment */
/* [한국어] RAI의 OME 인덱스. */
#define IOE_RAI_IDX     0x0f    /* Read with Atomic increment */

/* [한국어] 아래 EOE_*는 OME 배열 칸에 넣는 "출력 인코딩"이다. 즉 PAMU가
 * 코히런시 패브릭 쪽으로 실제로 내보낼 오퍼레이션이다. 스태시 기능의 핵심은
 * 평범한 IOE_READ를 EOE_RSA(스태시 할당을 동반한 읽기)로 바꾸는 데 있다.
 * 아래는 변환 없는 일반 읽기. */
#define EOE_READ        0x00
/* [한국어] 일반 쓰기 — 그냥 메모리에 쓴다. */
#define EOE_WRITE       0x01
/* [한국어] 원자적 클리어를 동반한 읽기(입력 IOE_RAC에 대응). */
#define EOE_RAC         0x0c    /* Read with Atomic clear */
/* [한국어] 원자적 세트를 동반한 읽기. */
#define EOE_RAS         0x0d    /* Read with Atomic set */
/* [한국어] 원자적 감소를 동반한 읽기. */
#define EOE_RAD         0x0e    /* Read with Atomic decrement */
/* [한국어] 원자적 증가를 동반한 읽기. */
#define EOE_RAI         0x0f    /* Read with Atomic increment */
/* [한국어] LDEC — 외부(플랫폼) 캐시로 데이터를 미리 적재한다.
 * 여기서부터가 스태시 계열 오퍼레이션이다. */
#define EOE_LDEC        0x10    /* Load external cache */
/* [한국어] LDECL — 적재하면서 그 캐시 라인을 잠근다(축출 방지).
 * 지연에 민감한 데이터를 캐시에 고정할 때 쓴다. */
#define EOE_LDECL       0x11    /* Load external cache with stash lock */
/* [한국어] LDECPE — 적재하되 "선호 배타(preferred exclusive)" 상태로 가져온다.
 * 곧 쓰기가 예상될 때 코히런시 트래픽을 줄인다. */
#define EOE_LDECPE      0x12    /* Load external cache with preferred exclusive */
/* [한국어] LDECPEL — 선호 배타 + 라인 잠금을 함께 적용. */
#define EOE_LDECPEL     0x13    /* Load external cache with preferred exclusive and lock */
/* [한국어] LDECFE — "강제 배타(forced exclusive)"로 적재. 다른 캐시의 사본을
 * 무효화하고 독점적으로 가져온다. */
#define EOE_LDECFE      0x14    /* Load external cache with forced exclusive */
/* [한국어] LDECFEL — 강제 배타 + 라인 잠금. */
#define EOE_LDECFEL     0x15    /* Load external cache with forced exclusive and lock */
/* [한국어] RSA(Read with Stash Allocate) — 메모리를 읽으면서 그 데이터를
 * PAACE의 CID가 지정한 캐시에 할당한다. 네트워크 수신 경로에서 패킷을
 * 처리할 코어의 L1에 미리 넣어 두는 대표적인 스태시 동작이다. */
#define EOE_RSA         0x16    /* Read with stash allocate */
/* [한국어] RSAU — 스태시 할당 후 잠금을 해제한다(앞서 잠긴 라인을 풀 때). */
#define EOE_RSAU        0x17    /* Read with stash allocate and unlock */
/* [한국어] READI(Read with Invalidate) — 읽으면서 다른 캐시의 사본을 무효화한다. */
#define EOE_READI       0x18    /* Read with invalidate */
/* [한국어] RWNITC(Read With No Intention To Cache) — 한 번만 읽고 버릴 데이터라
 * 캐시를 오염시키지 말라는 힌트. 스트리밍 DMA에 적합하다. */
#define EOE_RWNITC      0x19    /* Read with no intention to cache */
/* [한국어] WCI(Write Cache Inhibited) — 캐시를 우회해 메모리에 직접 쓴다. */
#define EOE_WCI         0x1a    /* Write cache inhibited */
/* [한국어] WWSA(Write With Stash Allocate) — 쓰면서 해당 라인을 캐시에 할당.
 * 곧 CPU가 읽을 데이터를 디바이스가 미리 캐시에 넣어 주는 동작이다. */
#define EOE_WWSA        0x1b    /* Write with stash allocate */
/* [한국어] WWSAL — 스태시 할당 + 라인 잠금. */
#define EOE_WWSAL       0x1c    /* Write with stash allocate and lock */
/* [한국어] WWSAO — 스태시 할당만 하고 메모리에는 쓰지 않는다(allocate only). */
#define EOE_WWSAO       0x1d    /* Write with stash allocate only */
/* [한국어] WWSAOL — 스태시 할당만 + 라인 잠금. */
#define EOE_WWSAOL      0x1e    /* Write with stash allocate only and lock */
/* [한국어] EOE_VALID(0x80) — OME 칸의 최상위 비트. 이 비트를 함께 세워야
 * 그 칸이 "유효한 매핑"으로 인정된다. setup_omt()는 항상
 * EOE_VALID | EOE_xxx 형태로 값을 채운다. */
#define EOE_VALID       0x80

/* Function prototypes */
/* [한국어] pamu_domain_init() — PAMU 서브시스템 전체 초기화.
 * PAACT/SPAACT/OMT 테이블을 할당하고 각 PAMU 인스턴스의 레지스터를 설정한 뒤
 * 하드웨어를 켠다. 호출자: fsl_pamu.c의 플랫폼 드라이버 probe 경로.
 * 반환: 0 성공, 음수 errno 실패(메모리 부족/디바이스 트리 오류 등). */
int pamu_domain_init(void);
/* [한국어] pamu_enable_liodn() — 주어진 LIODN의 PAACE를 유효화한다.
 * 구체적으로는 AP 필드를 PAACE_AP_PERMS_ALL로, V 필드를 PAACE_V_VALID로 세우고
 * PAMU 캐시를 무효화한다. 이 호출 이후부터 그 디바이스의 DMA가 통과한다.
 * 호출자: fsl_pamu_domain.c의 attach 경로. 반환: 0 또는 음수 errno. */
int pamu_enable_liodn(int liodn);
/* [한국어] pamu_disable_liodn() — 주어진 LIODN의 PAACE에서 V 비트를 내려
 * 그 디바이스의 모든 DMA를 차단한다. detach/제거 경로에서 호출된다.
 * 반환: 0 또는 음수 errno(LIODN 범위 초과 등). */
int pamu_disable_liodn(int liodn);
/* [한국어] pamu_config_ppaace() — LIODN 하나의 Primary PAACE를 구성한다.
 * @liodn: 대상 디바이스의 LIODN 번호(PAACT 인덱스).
 * @omi: 사용할 OME 인덱스(스태시 동작 선택). ~0이면 OMT를 쓰지 않는다.
 * @stashid: 스태시 목적지 캐시 ID(impl_attr의 CID로 들어간다).
 * @prot: IOMMU_READ/IOMMU_WRITE 조합 — PAACE_AP_PERMS_* 값으로 변환된다.
 * 윈도를 4GB 전체(PAACE_WSE_4G)로, 변환은 NO_XLATE로 두어 "접근 허용만 하고
 * 주소는 그대로"인 구성을 만드는 것이 리눅스 드라이버의 표준 사용법이다.
 * 반환: 0 성공, 음수 errno. */
int pamu_config_ppaace(int liodn, u32 omi, uint32_t stashid, int prot);

/* [한국어] get_stash_id() — 스태시 목적지 힌트와 vCPU 번호로부터 실제
 * 하드웨어 캐시 ID를 계산한다.
 * @stash_dest_hint: PAMU_ATTR_CACHE_L1/L2/L3 같은 캐시 레벨 힌트.
 * @vcpu: 대상 CPU 번호(어느 코어의 캐시에 넣을지).
 * 디바이스 트리에서 해당 코어의 캐시 노드를 찾아 cache-stash-id 프로퍼티를
 * 읽는 방식으로 구현된다. 반환: 캐시 ID, 실패 시 ~(u32)0. */
u32 get_stash_id(u32 stash_dest_hint, u32 vcpu);
/* [한국어] get_ome_index() — 이 디바이스에 맞는 OME 인덱스를 골라 준다.
 * @omi_index: 출력 인자 — 선택된 인덱스가 여기에 저장된다.
 * @dev: 대상 디바이스. PCI 디바이스인지 여부에 따라 다른 OME를 고른다
 *       (PCI는 읽기/쓰기 스태시 동작이 다르기 때문).
 * 반환: 없음. 결과는 omi_index로 전달된다. */
void get_ome_index(u32 *omi_index, struct device *dev);
/* [한국어] pamu_update_paace_stash() — 이미 구성된 PAACE의 스태시 목적지
 * (impl_attr의 CID 필드)만 런타임에 바꾼다.
 * @liodn: 대상 LIODN. @value: 새 캐시 ID.
 * 왜 별도 함수인가: 스태시 목적지는 IRQ affinity처럼 실행 중에 바뀔 수 있어,
 * 전체 PAACE를 다시 쓰지 않고 한 필드만 갱신하고 캐시를 무효화하는 경로가
 * 필요하기 때문이다. 반환: 0 또는 음수 errno.
 * 선언 뒤 함수 이름 앞의 공백 두 칸은 원본 정렬 그대로다. */
int  pamu_update_paace_stash(int liodn, u32 value);

/* [한국어] __FSL_PAMU_H 인클루드 가드의 끝. */
#endif  /* __FSL_PAMU_H */
