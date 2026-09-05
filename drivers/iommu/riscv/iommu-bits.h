/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright © 2022-2024 Rivos Inc.
 * Copyright © 2023 FORTH-ICS/CARV
 * Copyright © 2023 RISC-V IOMMU Task Group
 *
 * RISC-V IOMMU - Register Layout and Data Structures.
 *
 * Based on the 'RISC-V IOMMU Architecture Specification', Version 1.0
 * Published at  https://github.com/riscv-non-isa/riscv-iommu
 *
 */

/*
 * [한국어 설명] RISC-V IOMMU 스펙의 레지스터 맵과 자료구조 정의 (riscv/iommu-bits.h)
 *
 * === 파일의 역할 ===
 * RISC-V IOMMU Architecture Specification 1.0을 그대로 C 정의로 옮긴 헤더다.
 * 실행 코드는 파일 끝의 커맨드 조립 인라인 함수 몇 개뿐이고, 나머지는 전부
 * 레지스터 오프셋, 비트 필드 마스크, 메모리상 자료구조의 레이아웃이다.
 * 각 정의 위의 "5.3", "3.1.1" 같은 주석이 스펙의 절 번호이므로, 이 파일은
 * 사실상 스펙과 1:1로 대응하는 번역본이라고 보면 된다.
 *
 * RISC-V IOMMU의 구조를 이해하는 데 필요한 개념이 세 가지 있다.
 *
 * (1) **2단계 디렉토리 → 2단계 주소 변환**. 디바이스가 DMA를 내면 IOMMU는
 *     먼저 device_id로 DDT(Device Directory Table)를 걸어 DC(Device Context)를
 *     찾는다. DC 안에 fsc(1단계 컨텍스트)와 iohgatp(2단계 컨텍스트)가 있어,
 *     각각 게스트 가상→게스트 물리, 게스트 물리→호스트 물리 변환을 담당한다.
 *     PASID를 쓰는 디바이스라면 fsc가 페이지 테이블이 아니라 PDT(Process
 *     Directory Table)를 가리키고, process_id로 한 단계 더 걸어 PC를 찾는다.
 *     즉 device_id → DC → (process_id → PC) → 페이지 테이블의 사슬이다.
 *
 * (2) **세 개의 인메모리 큐**. CQ(Command Queue)로 소프트웨어가 무효화 명령을
 *     보내고, FQ(Fault Queue)로 하드웨어가 폴트를 보고하며, PQ(Page Request
 *     Queue)로 PCIe ATS 페이지 요청이 올라온다. 각 큐마다 베이스(CQB/FQB/PQB),
 *     헤드, 테일, 제어 상태(CSR) 레지스터가 한 벌씩 있고, 그 비트 배치가
 *     동일해 공통 매크로(RISCV_IOMMU_QUEUE_*)를 공유한다.
 *
 * (3) **MSI 재라우팅**. 가상화 환경에서 디바이스가 보낸 MSI를 게스트의
 *     인터럽트 파일로 돌려보내야 한다. DC의 msiptp가 가리키는 MSI 페이지
 *     테이블이 그 변환을 맡는데, 다단계가 아니라 평면 배열이고 모든 엔트리가
 *     리프다(struct riscv_iommu_msipte).
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [riscv/iommu.c] 드라이버 본체 — 큐를 만들고 DDT를 걷고 명령을 보낸다
 *        ↓ 이 헤더의 매크로/구조체/인라인 함수를 사용
 *   [이 파일] 스펙의 레지스터 맵과 자료구조 정의
 *        ↓ 이 정의대로 MMIO를 읽고 쓰며 메모리 구조체를 채운다
 *   [RISC-V IOMMU 하드웨어]
 *
 * 파일 끝의 riscv_iommu_cmd_* 인라인 함수들은 "커맨드 빌더"다. 드라이버가
 * 무효화 명령을 만들 때 비트 조작을 직접 하지 않고 이 함수들을 조합해
 * 쓰도록 해, 스펙 준수를 헤더 한곳에 모아 둔 설계다.
 *
 * 실행 컨텍스트: 순수 헤더라 컨텍스트가 없다. 다만 커맨드 빌더들은
 * 무효화 경로에서 불리므로 atomic 컨텍스트에서도 실행될 수 있고,
 * 전부 메모리 쓰기뿐이라 잠들지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - riscv/iommu.c: 이 헤더의 유일한 실질적 소비자. 큐 관리, DDT 워크,
 *   커맨드 전송, 폴트 처리 전부가 여기 정의된 값에 의존한다.
 * - riscv/iommu.h: 드라이버의 내부 자료구조 정의. 이 헤더를 포함한다.
 * - linux/bitfield.h: FIELD_PREP()/FIELD_GET(). 이 파일이 GENMASK로 정의한
 *   필드에 값을 넣고 빼는 표준 수단이다.
 * - asm/page.h: PHYS_PFN() — 물리 주소를 페이지 번호로 바꾼다. 커맨드
 *   빌더가 무효화 주소를 인코딩할 때 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct riscv_iommu_dc: Device Context. 한 디바이스의 변환 설정 전부
 *   (1단계/2단계 컨텍스트, MSI 테이블, 제어 비트)를 담은 64바이트 엔트리.
 * - struct riscv_iommu_pc: Process Context. PASID 하나의 1단계 컨텍스트.
 * - struct riscv_iommu_command: 커맨드 큐에 넣는 16바이트 명령. dword0에
 *   opcode/func, dword1에 명령별 데이터.
 * - struct riscv_iommu_fq_record: 폴트 큐의 32바이트 기록.
 * - struct riscv_iommu_pq_record: 페이지 요청 큐의 16바이트 기록.
 * - struct riscv_iommu_msipte: MSI 페이지 테이블의 엔트리(평면 배열).
 * - riscv_iommu_cmd_*(): 커맨드 조립 헬퍼들. 기본 형태를 만드는 함수와
 *   필드를 덧붙이는 set_* 함수로 나뉘어, 조합해서 쓰도록 설계되어 있다.
 */

/* [한국어] 중복 인클루드 방지 가드. */
#ifndef _RISCV_IOMMU_BITS_H_
#define _RISCV_IOMMU_BITS_H_

/* [한국어] u64 등 고정폭 정수 타입 — 모든 구조체 필드가 u64다. */
#include <linux/types.h>
/* [한국어] FIELD_PREP()/FIELD_GET() — GENMASK로 정의한 필드에 값을 안전하게
 * 넣고 빼는 매크로. 커맨드 빌더들이 전적으로 이것에 의존한다. */
#include <linux/bitfield.h>
/* [한국어] BIT(), BIT_ULL(), GENMASK(), GENMASK_ULL() 비트 매크로. */
#include <linux/bits.h>
/* [한국어] PHYS_PFN() — 물리 주소를 페이지 프레임 번호로 변환한다.
 * 무효화 커맨드에 주소를 실을 때 쓴다. */
#include <asm/page.h>

/*
 * Chapter 5: Memory Mapped register interface
 */

/* Common field positions */
/* [한국어] 여러 레지스터가 공유하는 PPN(Physical Page Number) 필드(비트 10~53).
 * 물리 주소를 12비트 시프트한 값이 여기 들어간다. 큐 베이스, DDT 포인터,
 * 페이지 테이블 엔트리 등이 모두 같은 배치를 쓴다. */
#define RISCV_IOMMU_PPN_FIELD		GENMASK_ULL(53, 10)
/* [한국어] 큐 크기를 2의 지수로 표현하는 필드(비트 0~4).
 * 값이 N이면 엔트리 개수는 2^(N+1)이다. CQB/FQB/PQB가 공유한다. */
#define RISCV_IOMMU_QUEUE_LOG2SZ_FIELD	GENMASK_ULL(4, 0)
/* [한국어] 큐 헤드/테일 인덱스 필드(하위 32비트 전부).
 * 실제 유효 비트 수는 큐 크기에 따라 달라지고, 나머지는 무시된다. */
#define RISCV_IOMMU_QUEUE_INDEX_FIELD	GENMASK_ULL(31, 0)
/* [한국어] 큐 CSR의 활성화 비트. 소프트웨어가 세우면 하드웨어가 큐를 켠다. */
#define RISCV_IOMMU_QUEUE_ENABLE	BIT(0)
/* [한국어] 큐 CSR의 인터럽트 활성화 비트. 큐에 새 항목이 생기거나 오류가
 * 났을 때 인터럽트를 낼지 결정한다. */
#define RISCV_IOMMU_QUEUE_INTR_ENABLE	BIT(1)
/* [한국어] 큐 메모리 접근 폴트 상태 비트. 하드웨어가 큐 메모리를 읽거나
 * 쓰다가 실패했다는 뜻이며, 소프트웨어가 1을 써서 지운다. */
#define RISCV_IOMMU_QUEUE_MEM_FAULT	BIT(8)
/* [한국어] 큐 오버플로 상태 비트. 소프트웨어가 소비하지 못해 기록이
 * 유실됐다는 뜻이다(FQ/PQ에만 의미가 있다). */
#define RISCV_IOMMU_QUEUE_OVERFLOW	BIT(9)
/* [한국어] 큐가 실제로 동작 중임을 나타내는 상태 비트. ENABLE을 쓴 뒤
 * 이 비트가 설 때까지 기다려야 큐가 준비된 것이다. */
#define RISCV_IOMMU_QUEUE_ACTIVE	BIT(16)
/* [한국어] 큐 설정 변경이 진행 중임을 나타내는 비트. 이 비트가 서 있는 동안
 * 새 설정을 쓰면 안 되므로, 드라이버는 이 비트가 내려갈 때까지 폴링한다. */
#define RISCV_IOMMU_QUEUE_BUSY		BIT(17)

/* [한국어] ATP(Address Translation and Protection) 계열 필드의 PPN 부분
 * (비트 0~43). satp/hgatp 같은 RISC-V CSR과 같은 배치를 따른다 —
 * 위의 RISCV_IOMMU_PPN_FIELD와 시작 비트가 다르다는 점에 주의. */
#define RISCV_IOMMU_ATP_PPN_FIELD	GENMASK_ULL(43, 0)
/* [한국어] ATP 필드의 모드 부분(비트 60~63). Sv39/Sv48/Sv57 같은 변환
 * 방식이나 BARE(변환 없음)를 지정한다. */
#define RISCV_IOMMU_ATP_MODE_FIELD	GENMASK_ULL(63, 60)

/* 5.3 IOMMU Capabilities (64bits) */
/* [한국어] 능력 레지스터의 오프셋(0x0). 드라이버가 probe에서 가장 먼저
 * 읽어 이 하드웨어가 무엇을 지원하는지 파악한다. */
#define RISCV_IOMMU_REG_CAPABILITIES		0x0000
/* [한국어] 구현한 스펙 버전(비트 0~7). 상위 니블이 major, 하위가 minor다. */
#define RISCV_IOMMU_CAPABILITIES_VERSION	GENMASK_ULL(7, 0)
/* [한국어] 1단계 변환에서 Sv32(32비트 가상주소, 2단계 테이블)를 지원한다. */
#define RISCV_IOMMU_CAPABILITIES_SV32		BIT_ULL(8)
/* [한국어] Sv39(39비트 가상주소, 3단계 테이블) 지원. 64비트 리눅스의
 * 기본 구성이라 실무에서 가장 중요한 비트다. */
#define RISCV_IOMMU_CAPABILITIES_SV39		BIT_ULL(9)
/* [한국어] Sv48(48비트 가상주소, 4단계 테이블) 지원. */
#define RISCV_IOMMU_CAPABILITIES_SV48		BIT_ULL(10)
/* [한국어] Sv57(57비트 가상주소, 5단계 테이블) 지원. */
#define RISCV_IOMMU_CAPABILITIES_SV57		BIT_ULL(11)
/* [한국어] Svpbmt 확장 지원 — 페이지 테이블 엔트리로 메모리 속성
 * (캐시 가능/불가 등)을 지정할 수 있다. */
#define RISCV_IOMMU_CAPABILITIES_SVPBMT		BIT_ULL(15)
/* [한국어] 2단계(G-stage) 변환에서 Sv32x4 지원. x4가 붙는 이유는 게스트
 * 물리 주소가 2비트 더 넓어 최상위 테이블이 4배 커지기 때문이다. */
#define RISCV_IOMMU_CAPABILITIES_SV32X4		BIT_ULL(16)
/* [한국어] 2단계 변환에서 Sv39x4 지원. */
#define RISCV_IOMMU_CAPABILITIES_SV39X4		BIT_ULL(17)
/* [한국어] 2단계 변환에서 Sv48x4 지원. */
#define RISCV_IOMMU_CAPABILITIES_SV48X4		BIT_ULL(18)
/* [한국어] 2단계 변환에서 Sv57x4 지원. */
#define RISCV_IOMMU_CAPABILITIES_SV57X4		BIT_ULL(19)
/* [한국어] MRIF(Memory-Resident Interrupt File)에 대한 원자적 갱신 지원.
 * MSI를 메모리에 상주하는 인터럽트 파일로 돌릴 때 필요하다. */
#define RISCV_IOMMU_CAPABILITIES_AMO_MRIF	BIT_ULL(21)
/* [한국어] MSI 평면 페이지 테이블 지원. 이 비트가 서야 struct
 * riscv_iommu_dc의 뒤쪽 4개 필드(msiptp 이하)가 존재한다 —
 * 없으면 DC가 32바이트로 줄어들어 riscv_iommu_get_dc()가 포인터 산술로
 * 건너뛴다. 구조체 크기가 런타임에 달라지는 드문 경우다. */
#define RISCV_IOMMU_CAPABILITIES_MSI_FLAT	BIT_ULL(22)
/* [한국어] MSI를 MRIF로 라우팅하는 기능 지원. */
#define RISCV_IOMMU_CAPABILITIES_MSI_MRIF	BIT_ULL(23)
/* [한국어] 하드웨어가 페이지 테이블의 A(Accessed)/D(Dirty) 비트를 원자적으로
 * 갱신할 수 있다. 없으면 소프트웨어가 미리 세워 둬야 한다. */
#define RISCV_IOMMU_CAPABILITIES_AMO_HWAD	BIT_ULL(24)
/* [한국어] PCIe ATS(Address Translation Services) 지원 — 디바이스가 변환
 * 결과를 자기 캐시(ATC)에 두고 쓸 수 있게 한다. PRI/페이지 요청 큐의 전제다. */
#define RISCV_IOMMU_CAPABILITIES_ATS		BIT_ULL(25)
/* [한국어] T2GPA 지원 — ATS 변환 요청에 대해 최종 물리 주소가 아니라
 * 게스트 물리 주소를 돌려줄 수 있다. 가상화에서 디바이스에 호스트 주소를
 * 노출하지 않으려 할 때 쓴다. */
#define RISCV_IOMMU_CAPABILITIES_T2GPA		BIT_ULL(26)
/* [한국어] 빅엔디언 동작 지원. FCTL의 BE 비트로 실제 전환한다. */
#define RISCV_IOMMU_CAPABILITIES_END		BIT_ULL(27)
/* [한국어] IGS(Interrupt Generation Support) 필드(비트 28~29) —
 * 이 IOMMU가 MSI로 인터럽트를 낼 수 있는지, 배선 인터럽트만 되는지,
 * 둘 다 되는지를 나타낸다. 아래 enum riscv_iommu_igs_settings 참조. */
#define RISCV_IOMMU_CAPABILITIES_IGS		GENMASK_ULL(29, 28)
/* [한국어] HPM(Hardware Performance Monitor) 지원 — 성능 카운터가 있다. */
#define RISCV_IOMMU_CAPABILITIES_HPM		BIT_ULL(30)
/* [한국어] 디버그 기능 지원 — TR_REQ_* 레지스터로 소프트웨어가 임의의
 * 변환을 하드웨어에 물어볼 수 있다. */
#define RISCV_IOMMU_CAPABILITIES_DBG		BIT_ULL(31)
/* [한국어] PAS(Physical Address Size) 필드(비트 32~37) — 지원하는 물리
 * 주소 비트 수. 드라이버가 도메인의 출력 주소 폭을 정하는 근거다. */
#define RISCV_IOMMU_CAPABILITIES_PAS		GENMASK_ULL(37, 32)
/* [한국어] PD8 지원 — 1단계 PDT로 8비트 process_id를 다룰 수 있다. */
#define RISCV_IOMMU_CAPABILITIES_PD8		BIT_ULL(38)
/* [한국어] PD17 지원 — 2단계 PDT로 17비트 process_id를 다룰 수 있다. */
#define RISCV_IOMMU_CAPABILITIES_PD17		BIT_ULL(39)
/* [한국어] PD20 지원 — 3단계 PDT로 20비트 process_id(PCIe PASID 전체 폭)를
 * 다룰 수 있다. SVA를 제대로 쓰려면 이 비트가 필요하다. */
#define RISCV_IOMMU_CAPABILITIES_PD20		BIT_ULL(40)

/**
 * enum riscv_iommu_igs_settings - Interrupt Generation Support Settings
 * @RISCV_IOMMU_CAPABILITIES_IGS_MSI: IOMMU supports only MSI generation
 * @RISCV_IOMMU_CAPABILITIES_IGS_WSI: IOMMU supports only Wired-Signaled interrupt
 * @RISCV_IOMMU_CAPABILITIES_IGS_BOTH: IOMMU supports both MSI and WSI generation
 * @RISCV_IOMMU_CAPABILITIES_IGS_RSRV: Reserved for standard use
 */
/* [한국어] CAPABILITIES의 IGS 필드가 가질 수 있는 값들.
 * 드라이버는 이 값을 보고 MSI를 등록할지, 배선 인터럽트를 요청할지,
 * FCTL의 WSI 비트를 세울지 결정한다. */
enum riscv_iommu_igs_settings {
	RISCV_IOMMU_CAPABILITIES_IGS_MSI = 0,
	/* [한국어] MSI만 지원 — 드라이버는 반드시 MSI를 설정해야 하고,
	 * MSI_CFG_TBL 레지스터에 목적지 주소와 데이터를 채워야 한다. */

	RISCV_IOMMU_CAPABILITIES_IGS_WSI = 1,
	/* [한국어] 배선 인터럽트(Wired-Signaled Interrupt)만 지원 —
	 * FCTL의 WSI 비트를 세우고 플랫폼 인터럽트를 요청해야 한다. */

	RISCV_IOMMU_CAPABILITIES_IGS_BOTH = 2,
	/* [한국어] 둘 다 지원 — 드라이버가 상황에 맞게 고를 수 있다.
	 * 보통 MSI를 선호하고, 실패하면 배선 인터럽트로 물러난다. */

	RISCV_IOMMU_CAPABILITIES_IGS_RSRV = 3
	/* [한국어] 예약값 — 표준이 아직 정의하지 않았다. 이 값이 읽히면
	 * 드라이버는 인터럽트 방식을 결정할 수 없다. */
};

/* 5.4 Features control register (32bits) */
/* [한국어] 기능 제어 레지스터(0x8). 능력(CAPABILITIES)이 "할 수 있는 것"이라면
 * 이 레지스터는 "지금 켜 둔 것"이다. */
#define RISCV_IOMMU_REG_FCTL		0x0008
/* [한국어] 빅엔디언 모드 활성화 비트. 메모리상 자료구조를 빅엔디언으로
 * 해석하게 한다. 리눅스 RISC-V는 리틀엔디언이라 항상 0으로 둔다. */
#define RISCV_IOMMU_FCTL_BE		BIT(0)
/* [한국어] 배선 인터럽트 모드 활성화. 세우면 MSI 대신 배선 인터럽트로
 * 알린다. IGS 값에 따라 드라이버가 설정한다. */
#define RISCV_IOMMU_FCTL_WSI		BIT(1)
/* [한국어] 게스트 주소 길이 제한(Guest eXtended Length) 비트.
 * 세우면 2단계 변환이 32비트 모드(Sv32x4)로 동작한다. */
#define RISCV_IOMMU_FCTL_GXL		BIT(2)

/* 5.5 Device-directory-table pointer (64bits) */
/* [한국어] DDT 포인터 레지스터(0x10). IOMMU가 device_id로 DC를 찾아가는
 * 출발점이자, IOMMU 전체의 동작 모드를 정하는 레지스터이기도 하다. */
#define RISCV_IOMMU_REG_DDTP		0x0010
/* [한국어] IOMMU 동작 모드 필드(비트 0~3). OFF/BARE/1LVL/2LVL/3LVL 중 하나로,
 * 아래 enum riscv_iommu_ddtp_modes 참조. */
#define RISCV_IOMMU_DDTP_IOMMU_MODE	GENMASK_ULL(3, 0)
/* [한국어] 모드 전환이 진행 중임을 나타내는 비트. 드라이버는 DDTP를 쓴 뒤
 * 이 비트가 내려갈 때까지 폴링해야 전환 완료를 확인할 수 있다. */
#define RISCV_IOMMU_DDTP_BUSY		BIT_ULL(4)
/* [한국어] DDT 최상위 테이블의 물리 페이지 번호. 공통 PPN 필드를 그대로 쓴다. */
#define RISCV_IOMMU_DDTP_PPN		RISCV_IOMMU_PPN_FIELD

/**
 * enum riscv_iommu_ddtp_modes - IOMMU translation modes
 * @RISCV_IOMMU_DDTP_IOMMU_MODE_OFF: No inbound transactions allowed
 * @RISCV_IOMMU_DDTP_IOMMU_MODE_BARE: Pass-through mode
 * @RISCV_IOMMU_DDTP_IOMMU_MODE_1LVL: One-level DDT
 * @RISCV_IOMMU_DDTP_IOMMU_MODE_2LVL: Two-level DDT
 * @RISCV_IOMMU_DDTP_IOMMU_MODE_3LVL: Three-level DDT
 * @RISCV_IOMMU_DDTP_IOMMU_MODE_MAX: Max value allowed by specification
 */
/* [한국어] IOMMU 전체의 동작 모드. DDT 레벨 수가 곧 지원 가능한 device_id
 * 폭을 결정한다 — 레벨이 많을수록 넓은 ID 공간을 덮지만 워크가 길어진다.
 * 드라이버는 시스템의 최대 device_id를 보고 필요한 최소 레벨을 고른다. */
enum riscv_iommu_ddtp_modes {
	RISCV_IOMMU_DDTP_IOMMU_MODE_OFF = 0,
	/* [한국어] 완전 차단 — 들어오는 모든 트랜잭션이 거부된다.
	 * 부팅 초기나 오류 상황에서 DMA를 원천 봉쇄할 때 쓴다. */

	RISCV_IOMMU_DDTP_IOMMU_MODE_BARE = 1,
	/* [한국어] 통과(pass-through) 모드 — 변환 없이 물리 주소 그대로 나간다.
	 * IOMMU_DOMAIN_IDENTITY에 대응하며, DDT 자체가 필요 없다. */

	RISCV_IOMMU_DDTP_IOMMU_MODE_1LVL = 2,
	/* [한국어] 1단계 DDT — 최상위 테이블이 곧 DC 배열이다. device_id 공간이
	 * 좁은 시스템에서 워크를 한 번으로 줄인다. */

	RISCV_IOMMU_DDTP_IOMMU_MODE_2LVL = 3,

	RISCV_IOMMU_DDTP_IOMMU_MODE_3LVL = 4,
	/* [한국어] 3단계 DDT — device_id 24비트 전체를 덮는 최대 구성이다.
	 * 설정자: 드라이버가 DDTP 레지스터의 MODE 필드에 이 값을 쓴다.
	 * 읽는 자: 하드웨어. 장치 디렉토리를 몇 단계로 워크할지 이 값으로 정한다.
	 * 왜 단계를 고르게 하는가: device_id 공간이 좁은 시스템에서 3단계를 쓰면
	 *   워크가 세 번 일어나 변환 지연이 늘고, 중간 테이블 메모리도 낭비된다.
	 *   실제로 쓰는 device_id 범위에 맞춰 가장 얕은 구성을 고르는 것이 이득이다.
	 * 값 범위: 4. 아래 MODE_MAX 와 같은 값이라, 하드웨어가 보고한 값이 이보다
	 *   크면 드라이버가 모르는 미래 구성이라는 뜻이 된다. */

	RISCV_IOMMU_DDTP_IOMMU_MODE_MAX = 4
	/* [한국어] 스펙이 허용하는 최대값. 3LVL과 값이 같아, 하드웨어가 보고한
	 * 모드가 유효 범위인지 검사하는 상한으로 쓴다. */
};

/* 5.6 Command Queue Base (64bits) */
/* [한국어] 커맨드 큐 베이스 레지스터(0x18). 소프트웨어가 무효화 명령을
 * 써 넣을 링 버퍼의 위치와 크기를 하드웨어에 알린다. */
#define RISCV_IOMMU_REG_CQB		0x0018
/* [한국어] 큐 크기(2의 지수). 공통 필드를 그대로 쓴다. */
#define RISCV_IOMMU_CQB_ENTRIES		RISCV_IOMMU_QUEUE_LOG2SZ_FIELD
/* [한국어] 큐 버퍼의 물리 페이지 번호. */
#define RISCV_IOMMU_CQB_PPN		RISCV_IOMMU_PPN_FIELD

/* 5.7 Command Queue head (32bits) */
/* [한국어] 커맨드 큐 헤드(0x20) — 하드웨어가 어디까지 소비했는지 알려 준다.
 * 소프트웨어는 읽기만 한다. */
#define RISCV_IOMMU_REG_CQH		0x0020
/* [한국어] 헤드 인덱스 필드. */
#define RISCV_IOMMU_CQH_INDEX		RISCV_IOMMU_QUEUE_INDEX_FIELD

/* 5.8 Command Queue tail (32bits) */
/* [한국어] 커맨드 큐 테일(0x24) — 소프트웨어가 명령을 넣은 뒤 여기를 올려
 * 하드웨어에 새 명령이 있음을 알린다. */
#define RISCV_IOMMU_REG_CQT		0x0024
/* [한국어] 테일 인덱스 필드. */
#define RISCV_IOMMU_CQT_INDEX		RISCV_IOMMU_QUEUE_INDEX_FIELD

/* 5.9 Fault Queue Base (64bits) */
/* [한국어] 폴트 큐 베이스(0x28). 하드웨어가 폴트 기록을 써 넣을 링 버퍼다.
 * 방향이 커맨드 큐와 반대라는 점이 핵심이다. */
#define RISCV_IOMMU_REG_FQB		0x0028
/* [한국어] 폴트 큐 크기(2의 지수). */
#define RISCV_IOMMU_FQB_ENTRIES		RISCV_IOMMU_QUEUE_LOG2SZ_FIELD
/* [한국어] 폴트 큐 버퍼의 물리 페이지 번호. */
#define RISCV_IOMMU_FQB_PPN		RISCV_IOMMU_PPN_FIELD

/* 5.10 Fault Queue Head (32bits) */
/* [한국어] 폴트 큐 헤드(0x30) — 소프트웨어가 어디까지 읽었는지 하드웨어에
 * 알린다. 커맨드 큐와 달리 여기서는 소프트웨어가 헤드를 쓴다. */
#define RISCV_IOMMU_REG_FQH		0x0030
/* [한국어] 헤드 인덱스 필드. */
#define RISCV_IOMMU_FQH_INDEX		RISCV_IOMMU_QUEUE_INDEX_FIELD

/* 5.11 Fault Queue tail (32bits) */
/* [한국어] 폴트 큐 테일(0x34) — 하드웨어가 폴트를 넣을 때마다 올린다.
 * 소프트웨어는 읽기만 하며, 헤드와 다르면 처리할 폴트가 있다는 뜻이다. */
#define RISCV_IOMMU_REG_FQT		0x0034
/* [한국어] 테일 인덱스 필드. */
#define RISCV_IOMMU_FQT_INDEX		RISCV_IOMMU_QUEUE_INDEX_FIELD

/* 5.12 Page Request Queue base (64bits) */
/* [한국어] 페이지 요청 큐 베이스(0x38). PCIe ATS/PRI를 쓰는 디바이스가
 * "이 주소를 매핑해 달라"고 보내는 요청이 여기 쌓인다. */
#define RISCV_IOMMU_REG_PQB		0x0038
/* [한국어] 페이지 요청 큐 크기(2의 지수). */
#define RISCV_IOMMU_PQB_ENTRIES		RISCV_IOMMU_QUEUE_LOG2SZ_FIELD
/* [한국어] 페이지 요청 큐 버퍼의 물리 페이지 번호. */
#define RISCV_IOMMU_PQB_PPN		RISCV_IOMMU_PPN_FIELD

/* 5.13 Page Request Queue head (32bits) */
/* [한국어] 페이지 요청 큐 헤드(0x40) — 소프트웨어가 소비 위치를 알린다. */
#define RISCV_IOMMU_REG_PQH		0x0040
/* [한국어] 헤드 인덱스 필드. */
#define RISCV_IOMMU_PQH_INDEX		RISCV_IOMMU_QUEUE_INDEX_FIELD

/* 5.14 Page Request Queue tail (32bits) */
/* [한국어] 페이지 요청 큐 테일(0x44) — 하드웨어가 요청을 넣을 때 올린다. */
#define RISCV_IOMMU_REG_PQT		0x0044
/* [한국어] 테일 인덱스 필드. 이름에만 _MASK가 붙어 있는데, 다른 큐의
 * 대응 매크로와 이름 규칙이 어긋난 것으로 원본 그대로다. */
#define RISCV_IOMMU_PQT_INDEX_MASK	RISCV_IOMMU_QUEUE_INDEX_FIELD

/* 5.15 Command Queue CSR (32bits) */
/* [한국어] 커맨드 큐 제어/상태 레지스터(0x48). 큐를 켜고 끄며 오류 상태를
 * 확인하는 창구다. */
#define RISCV_IOMMU_REG_CQCSR		0x0048
/* [한국어] 커맨드 큐 활성화(CQEN). 공통 ENABLE 비트를 쓴다. */
#define RISCV_IOMMU_CQCSR_CQEN		RISCV_IOMMU_QUEUE_ENABLE
/* [한국어] 커맨드 큐 인터럽트 활성화(CIE). IOFENCE 완료 등을 알린다. */
#define RISCV_IOMMU_CQCSR_CIE		RISCV_IOMMU_QUEUE_INTR_ENABLE
/* [한국어] 커맨드 큐 메모리 폴트(CQMF) — 큐 버퍼를 읽다가 실패했다. */
#define RISCV_IOMMU_CQCSR_CQMF		RISCV_IOMMU_QUEUE_MEM_FAULT
/* [한국어] 명령 타임아웃(CMD_TO) — 명령 처리가 제한 시간을 넘겼다.
 * 보통 ATS 무효화가 디바이스로부터 응답을 받지 못했을 때 발생한다. */
#define RISCV_IOMMU_CQCSR_CMD_TO	BIT(9)
/* [한국어] 잘못된 명령(CMD_ILL) — opcode나 필드 조합이 유효하지 않다.
 * 드라이버 버그의 신호이므로 커맨드 빌더를 통해서만 명령을 만드는 이유다. */
#define RISCV_IOMMU_CQCSR_CMD_ILL	BIT(10)
/* [한국어] IOFENCE 완료 인터럽트 대기(FENCE_W_IP) — 펜스가 완료되어
 * 인터럽트가 걸려 있다는 뜻이다. */
#define RISCV_IOMMU_CQCSR_FENCE_W_IP	BIT(11)
/* [한국어] 커맨드 큐 동작 중(CQON). ENABLE을 쓴 뒤 이 비트를 기다린다. */
#define RISCV_IOMMU_CQCSR_CQON		RISCV_IOMMU_QUEUE_ACTIVE
/* [한국어] 커맨드 큐 설정 변경 중(BUSY). 이 비트가 내려가야 설정이 반영된다. */
#define RISCV_IOMMU_CQCSR_BUSY		RISCV_IOMMU_QUEUE_BUSY

/* 5.16 Fault Queue CSR (32bits) */
/* [한국어] 폴트 큐 제어/상태 레지스터(0x4C). 구조는 커맨드 큐와 같고,
 * 명령 관련 비트 대신 오버플로 비트가 있다. */
#define RISCV_IOMMU_REG_FQCSR		0x004C
/* [한국어] 폴트 큐 활성화(FQEN). */
#define RISCV_IOMMU_FQCSR_FQEN		RISCV_IOMMU_QUEUE_ENABLE
/* [한국어] 폴트 큐 인터럽트 활성화(FIE) — 새 폴트가 쌓이면 알린다. */
#define RISCV_IOMMU_FQCSR_FIE		RISCV_IOMMU_QUEUE_INTR_ENABLE
/* [한국어] 폴트 큐 메모리 폴트(FQMF) — 큐 버퍼에 쓰다가 실패했다. */
#define RISCV_IOMMU_FQCSR_FQMF		RISCV_IOMMU_QUEUE_MEM_FAULT
/* [한국어] 폴트 큐 오버플로(FQOF) — 소프트웨어가 늦어 폴트 기록이 유실됐다.
 * 커맨드 큐에는 없는 비트인데, 방향이 반대라 넘칠 수 있기 때문이다. */
#define RISCV_IOMMU_FQCSR_FQOF		RISCV_IOMMU_QUEUE_OVERFLOW
/* [한국어] 폴트 큐 동작 중(FQON). */
#define RISCV_IOMMU_FQCSR_FQON		RISCV_IOMMU_QUEUE_ACTIVE
/* [한국어] 폴트 큐 설정 변경 중(BUSY). */
#define RISCV_IOMMU_FQCSR_BUSY		RISCV_IOMMU_QUEUE_BUSY

/* 5.17 Page Request Queue CSR (32bits) */
/* [한국어] 페이지 요청 큐 제어/상태 레지스터(0x50). 폴트 큐와 동일한 구성이다. */
#define RISCV_IOMMU_REG_PQCSR		0x0050
/* [한국어] 페이지 요청 큐 활성화(PQEN). */
#define RISCV_IOMMU_PQCSR_PQEN		RISCV_IOMMU_QUEUE_ENABLE
/* [한국어] 페이지 요청 큐 인터럽트 활성화(PIE). */
#define RISCV_IOMMU_PQCSR_PIE		RISCV_IOMMU_QUEUE_INTR_ENABLE
/* [한국어] 페이지 요청 큐 메모리 폴트(PQMF). */
#define RISCV_IOMMU_PQCSR_PQMF		RISCV_IOMMU_QUEUE_MEM_FAULT
/* [한국어] 페이지 요청 큐 오버플로(PQOF) — 요청이 유실됐다는 뜻이며,
 * 디바이스가 응답을 영원히 기다릴 수 있어 복구가 까다롭다. */
#define RISCV_IOMMU_PQCSR_PQOF		RISCV_IOMMU_QUEUE_OVERFLOW
/* [한국어] 페이지 요청 큐 동작 중(PQON). */
#define RISCV_IOMMU_PQCSR_PQON		RISCV_IOMMU_QUEUE_ACTIVE
/* [한국어] 페이지 요청 큐 설정 변경 중(BUSY). */
#define RISCV_IOMMU_PQCSR_BUSY		RISCV_IOMMU_QUEUE_BUSY

/* 5.18 Interrupt Pending Status (32bits) */
/* [한국어] 인터럽트 대기 상태 레지스터(0x54). 어느 원인으로 인터럽트가
 * 걸렸는지 알려 주며, 처리 후 해당 비트에 1을 써서 지운다. */
#define RISCV_IOMMU_REG_IPSR		0x0054

/* [한국어] 인터럽트 원인 번호 — 커맨드 큐. IPSR의 비트 위치이자
 * ICVEC 레지스터에서 벡터를 고를 때의 인덱스이기도 하다. */
#define RISCV_IOMMU_INTR_CQ		0
/* [한국어] 인터럽트 원인 번호 — 폴트 큐. */
#define RISCV_IOMMU_INTR_FQ		1
/* [한국어] 인터럽트 원인 번호 — 성능 모니터(카운터 오버플로). */
#define RISCV_IOMMU_INTR_PM		2
/* [한국어] 인터럽트 원인 번호 — 페이지 요청 큐. */
#define RISCV_IOMMU_INTR_PQ		3
/* [한국어] 인터럽트 원인의 총 개수. 드라이버가 MSI 벡터를 몇 개 요청할지,
 * 핸들러 배열을 얼마나 크게 잡을지의 기준이다. */
#define RISCV_IOMMU_INTR_COUNT		4

/* [한국어] IPSR의 커맨드 큐 인터럽트 대기 비트(CIP). 원인 번호를 그대로
 * 비트 위치로 쓰는 설계라 BIT()로 유도한다. */
#define RISCV_IOMMU_IPSR_CIP		BIT(RISCV_IOMMU_INTR_CQ)
/* [한국어] IPSR의 폴트 큐 인터럽트 대기 비트(FIP). */
#define RISCV_IOMMU_IPSR_FIP		BIT(RISCV_IOMMU_INTR_FQ)
/* [한국어] IPSR의 성능 모니터 인터럽트 대기 비트(PMIP). */
#define RISCV_IOMMU_IPSR_PMIP		BIT(RISCV_IOMMU_INTR_PM)
/* [한국어] IPSR의 페이지 요청 큐 인터럽트 대기 비트(PIP). */
#define RISCV_IOMMU_IPSR_PIP		BIT(RISCV_IOMMU_INTR_PQ)

/* 5.19 Performance monitoring counter overflow status (32bits) */
/* [한국어] 성능 카운터 오버플로 상태 레지스터(0x58). 어느 카운터가 넘쳤는지
 * 알려 준다. */
#define RISCV_IOMMU_REG_IOCOUNTOVF	0x0058
/* [한국어] 사이클 카운터가 넘쳤음을 나타내는 비트. */
#define RISCV_IOMMU_IOCOUNTOVF_CY	BIT(0)
/* [한국어] 이벤트 카운터 1~31의 오버플로 비트들(비트 1~31). */
#define RISCV_IOMMU_IOCOUNTOVF_HPM	GENMASK_ULL(31, 1)

/* 5.20 Performance monitoring counter inhibits (32bits) */
/* [한국어] 성능 카운터 정지 레지스터(0x5C). 해당 비트를 세우면 그 카운터가
 * 세는 것을 멈춘다 — 측정 구간을 정확히 잘라 내는 데 쓴다. */
#define RISCV_IOMMU_REG_IOCOUNTINH	0x005C
/* [한국어] 사이클 카운터 정지 비트. */
#define RISCV_IOMMU_IOCOUNTINH_CY	BIT(0)
/* [한국어] 이벤트 카운터 1~31의 정지 비트들. 위 OVF와 달리 GENMASK(32비트)를
 * 쓰는데, 이 레지스터가 32비트이기 때문이다. */
#define RISCV_IOMMU_IOCOUNTINH_HPM	GENMASK(31, 1)

/* 5.21 Performance monitoring cycles counter (64bits) */
/* [한국어] 사이클 카운터 레지스터(0x60). IOMMU 내부 클록 사이클을 센다. */
#define RISCV_IOMMU_REG_IOHPMCYCLES     0x0060
/* [한국어] 실제 카운트 값 필드(비트 0~62). */
#define RISCV_IOMMU_IOHPMCYCLES_COUNTER	GENMASK_ULL(62, 0)
/* [한국어] 오버플로 표시 비트(최상위). 카운터가 한 바퀴 돌았음을 알린다. */
#define RISCV_IOMMU_IOHPMCYCLES_OF	BIT_ULL(63)

/* 5.22 Performance monitoring event counters (31 * 64bits) */
/* [한국어] 이벤트 카운터 배열의 시작 오프셋(0x68). 31개가 8바이트 간격으로 늘어선다. */
#define RISCV_IOMMU_REG_IOHPMCTR_BASE	0x0068
/* [한국어] n번째 이벤트 카운터의 오프셋을 계산하는 매크로.
 * n은 0부터 30까지이며, 대응하는 이벤트 선택기는 아래 IOHPMEVT(n)이다. */
#define RISCV_IOMMU_REG_IOHPMCTR(_n)	(RISCV_IOMMU_REG_IOHPMCTR_BASE + ((_n) * 0x8))

/* 5.23 Performance monitoring event selectors (31 * 64bits) */
/* [한국어] 이벤트 선택기 배열의 시작 오프셋(0x160). 각 카운터가 무엇을 셀지
 * 정하는 레지스터들이다. */
#define RISCV_IOMMU_REG_IOHPMEVT_BASE	0x0160
/* [한국어] n번째 이벤트 선택기의 오프셋. IOHPMCTR(n)과 짝을 이룬다. */
#define RISCV_IOMMU_REG_IOHPMEVT(_n)	(RISCV_IOMMU_REG_IOHPMEVT_BASE + ((_n) * 0x8))
/* [한국어] 셀 이벤트의 종류(비트 0~14). enum riscv_iommu_hpmevent_id 값이 들어간다. */
#define RISCV_IOMMU_IOHPMEVT_EVENTID	GENMASK_ULL(14, 0)
/* [한국어] DID/PID 필터에 마스크를 적용할지 여부(DMASK). 세우면 아래
 * 필터 값의 하위 비트를 무시해 범위 매칭이 된다. */
#define RISCV_IOMMU_IOHPMEVT_DMASK	BIT_ULL(15)
/* [한국어] 필터할 process_id 또는 PSCID(비트 16~35). 특정 프로세스의
 * 이벤트만 세고 싶을 때 지정한다. */
#define RISCV_IOMMU_IOHPMEVT_PID_PSCID	GENMASK_ULL(35, 16)
/* [한국어] 필터할 device_id 또는 GSCID(비트 36~59). 특정 디바이스나
 * 게스트의 이벤트만 세고 싶을 때 지정한다. */
#define RISCV_IOMMU_IOHPMEVT_DID_GSCID	GENMASK_ULL(59, 36)
/* [한국어] 위 PID/PSCID 필터를 실제로 적용할지 여부(PV_PSCV). */
#define RISCV_IOMMU_IOHPMEVT_PV_PSCV	BIT_ULL(60)
/* [한국어] 위 DID/GSCID 필터를 실제로 적용할지 여부(DV_GSCV). */
#define RISCV_IOMMU_IOHPMEVT_DV_GSCV	BIT_ULL(61)
/* [한국어] ID 타입 지정(IDT) — 필터 값을 device/process id로 볼지,
 * GSCID/PSCID(가상화 문맥 ID)로 볼지 결정한다. */
#define RISCV_IOMMU_IOHPMEVT_IDT	BIT_ULL(62)
/* [한국어] 이 카운터의 오버플로 표시 비트. IOCOUNTOVF와 중복되는 정보지만,
 * 카운터별로도 확인할 수 있게 두었다. */
#define RISCV_IOMMU_IOHPMEVT_OF		BIT_ULL(63)

/* Number of defined performance-monitoring event selectors */
/* [한국어] 이벤트 선택기(및 카운터)의 개수 31. 드라이버가 배열을 순회할 때의
 * 상한이자, 사이클 카운터를 제외한 이벤트 카운터의 수다. */
#define RISCV_IOMMU_IOHPMEVT_CNT	31

/**
 * enum riscv_iommu_hpmevent_id - Performance-monitoring event identifier
 *
 * @RISCV_IOMMU_HPMEVENT_INVALID: Invalid event, do not count
 * @RISCV_IOMMU_HPMEVENT_URQ: Untranslated requests
 * @RISCV_IOMMU_HPMEVENT_TRQ: Translated requests
 * @RISCV_IOMMU_HPMEVENT_ATS_RQ: ATS translation requests
 * @RISCV_IOMMU_HPMEVENT_TLB_MISS: TLB misses
 * @RISCV_IOMMU_HPMEVENT_DD_WALK: Device directory walks
 * @RISCV_IOMMU_HPMEVENT_PD_WALK: Process directory walks
 * @RISCV_IOMMU_HPMEVENT_S_VS_WALKS: First-stage page table walks
 * @RISCV_IOMMU_HPMEVENT_G_WALKS: Second-stage page table walks
 * @RISCV_IOMMU_HPMEVENT_MAX: Value to denote maximum Event IDs
 */
/* [한국어] 성능 카운터가 셀 수 있는 이벤트의 종류. IOHPMEVT의 EVENTID
 * 필드에 이 값을 넣으면 해당 카운터가 그 이벤트를 센다.
 * 목록을 보면 IOMMU의 내부 동작 단계가 그대로 드러난다 —
 * 요청 수 → TLB 미스 → 디렉토리 워크 → 페이지 테이블 워크. */
enum riscv_iommu_hpmevent_id {
	RISCV_IOMMU_HPMEVENT_INVALID    = 0,
	/* [한국어] 무효 이벤트 — 이 카운터를 쓰지 않겠다는 뜻이다.
	 * 설정자: 카운터를 놓을 때 IOHPMEVT 의 EVENTID 필드에 이 값을 쓴다.
	 * 읽는 자: 하드웨어. 이 값이면 그 카운터는 아무것도 세지 않는다.
	 * 왜 0 인가: 레지스터를 0 으로 초기화하면 자동으로 "쓰지 않음" 상태가 되도록
	 *   스펙이 맞춰 두었다. 리셋 직후 카운터가 엉뚱한 이벤트를 세지 않는다.
	 * 이 enum 의 나머지 값들은 IOMMU 의 내부 동작 단계를 그대로 따라간다 —
	 *   요청 수, TLB 미스, 디렉토리 워크, 페이지 테이블 워크 순이다. */

	RISCV_IOMMU_HPMEVENT_URQ        = 1,
	/* [한국어] 미변환(untranslated) 요청 수 — 디바이스가 IOVA로 보낸
	 * 일반적인 DMA 요청의 개수다. */

	RISCV_IOMMU_HPMEVENT_TRQ        = 2,
	/* [한국어] 변환된(translated) 요청 수 — ATS로 이미 변환을 받아 둔
	 * 디바이스가 물리 주소로 직접 보낸 요청의 개수다. */

	RISCV_IOMMU_HPMEVENT_ATS_RQ     = 3,
	/* [한국어] ATS 변환 요청 수 — 디바이스가 "이 주소 변환해 줘"라고
	 * 물어본 횟수다. */

	RISCV_IOMMU_HPMEVENT_TLB_MISS   = 4,
	/* [한국어] TLB 미스 수 — IOMMU 내부 변환 캐시에서 찾지 못해
	 * 실제 워크가 필요했던 횟수다. 성능 분석의 핵심 지표다. */

	RISCV_IOMMU_HPMEVENT_DD_WALK    = 5,
	/* [한국어] 디바이스 디렉토리 워크 수 — DDT를 걸어 DC를 찾은 횟수. */

	RISCV_IOMMU_HPMEVENT_PD_WALK    = 6,
	/* [한국어] 프로세스 디렉토리 워크 수 — PDT를 걸어 PC를 찾은 횟수.
	 * PASID를 쓰는 워크로드에서만 올라간다. */

	RISCV_IOMMU_HPMEVENT_S_VS_WALKS = 7,
	/* [한국어] 1단계(S/VS-stage) 페이지 테이블 워크 수 — 가상 주소를
	 * 게스트 물리 주소로 바꾸는 워크의 횟수다. */

	RISCV_IOMMU_HPMEVENT_G_WALKS    = 8,
	/* [한국어] 2단계(G-stage) 페이지 테이블 워크 수 — 게스트 물리를
	 * 호스트 물리로 바꾸는 워크의 횟수다. */

	RISCV_IOMMU_HPMEVENT_MAX        = 9
	/* [한국어] 정의된 이벤트 ID의 개수(상한). 유효성 검사용이다. */
};

/* 5.24 Translation request IOVA (64bits) */
/* [한국어] 디버그용 변환 요청 IOVA 레지스터(0x258). 소프트웨어가 임의의
 * 주소 변환을 하드웨어에 직접 물어볼 수 있게 하는 기능으로,
 * CAPABILITIES의 DBG 비트가 서 있을 때만 존재한다. */
#define RISCV_IOMMU_REG_TR_REQ_IOVA     0x0258
/* [한국어] 물어볼 가상 페이지 번호(비트 12~63). 페이지 내 오프셋은 필요 없다. */
#define RISCV_IOMMU_TR_REQ_IOVA_VPN	GENMASK_ULL(63, 12)

/* 5.25 Translation request control (64bits) */
/* [한국어] 디버그 변환 요청 제어 레지스터(0x260). 어떤 문맥으로 변환할지
 * 지정하고, GO_BUSY 비트를 세워 요청을 발사한다. */
#define RISCV_IOMMU_REG_TR_REQ_CTL	0x0260
/* [한국어] 요청 시작/진행 중 비트. 소프트웨어가 1을 쓰면 변환이 시작되고,
 * 하드웨어가 끝나면 스스로 0으로 내린다 — 즉 폴링 대상이다. */
#define RISCV_IOMMU_TR_REQ_CTL_GO_BUSY	BIT_ULL(0)
/* [한국어] 특권(supervisor) 접근으로 간주할지 여부. 페이지 테이블의 U 비트
 * 검사 결과가 달라진다. */
#define RISCV_IOMMU_TR_REQ_CTL_PRIV	BIT_ULL(1)
/* [한국어] 실행(instruction fetch) 접근으로 간주할지 여부. X 권한을 본다. */
#define RISCV_IOMMU_TR_REQ_CTL_EXE	BIT_ULL(2)
/* [한국어] No-Write — 읽기 전용 변환으로 요청한다. 세우지 않으면 쓰기
 * 권한까지 확인하며, D 비트 갱신이 일어날 수 있다. */
#define RISCV_IOMMU_TR_REQ_CTL_NW	BIT_ULL(3)
/* [한국어] 변환에 사용할 process_id(비트 12~31). */
#define RISCV_IOMMU_TR_REQ_CTL_PID	GENMASK_ULL(31, 12)
/* [한국어] 위 PID 필드가 유효한지 표시하는 비트. 없으면 PASID 없는
 * 변환으로 처리된다. */
#define RISCV_IOMMU_TR_REQ_CTL_PV	BIT_ULL(32)
/* [한국어] 변환에 사용할 device_id(비트 40~63). 어느 디바이스인 척하고
 * 물어볼지 정한다. */
#define RISCV_IOMMU_TR_REQ_CTL_DID	GENMASK_ULL(63, 40)

/* 5.26 Translation request response (64bits) */
/* [한국어] 디버그 변환 요청 응답 레지스터(0x268). GO_BUSY가 내려간 뒤
 * 여기서 결과를 읽는다. */
#define RISCV_IOMMU_REG_TR_RESPONSE	0x0268
/* [한국어] 변환이 실패했음을 나타내는 비트. 세워져 있으면 나머지 필드는
 * 의미가 없다. */
#define RISCV_IOMMU_TR_RESPONSE_FAULT	BIT_ULL(0)
/* [한국어] 결과 페이지의 메모리 속성(PBMT, 비트 7~8) — 캐시 가능 여부 등.
 * Svpbmt 확장이 있을 때만 의미가 있다. */
#define RISCV_IOMMU_TR_RESPONSE_PBMT	GENMASK_ULL(8, 7)
/* [한국어] 결과가 슈퍼페이지인지 표시하는 비트(SZ). 세워져 있으면
 * 반환된 PPN이 큰 페이지의 시작을 가리킨다. */
#define RISCV_IOMMU_TR_RESPONSE_SZ	BIT_ULL(9)
/* [한국어] 변환 결과 물리 페이지 번호. 공통 PPN 필드를 쓴다. */
#define RISCV_IOMMU_TR_RESPONSE_PPN	RISCV_IOMMU_PPN_FIELD

/* 5.27 Interrupt cause to vector (64bits) */
/* [한국어] 인터럽트 원인→벡터 매핑 레지스터(0x2F8). 네 가지 원인 각각을
 * 어느 인터럽트 벡터로 보낼지 지정한다. 여러 원인을 한 벡터에 묶을 수도 있어,
 * 드라이버가 MSI 벡터를 몇 개나 확보했느냐에 따라 유연하게 설정한다. */
#define RISCV_IOMMU_REG_ICVEC		0x02F8
/* [한국어] 커맨드 큐 인터럽트의 벡터 번호(비트 0~3). */
#define RISCV_IOMMU_ICVEC_CIV		GENMASK_ULL(3, 0)
/* [한국어] 폴트 큐 인터럽트의 벡터 번호(비트 4~7). */
#define RISCV_IOMMU_ICVEC_FIV		GENMASK_ULL(7, 4)
/* [한국어] 성능 모니터 인터럽트의 벡터 번호(비트 8~11). */
#define RISCV_IOMMU_ICVEC_PMIV		GENMASK_ULL(11, 8)
/* [한국어] 페이지 요청 큐 인터럽트의 벡터 번호(비트 12~15). */
#define RISCV_IOMMU_ICVEC_PIV		GENMASK_ULL(15, 12)

/* 5.28 MSI Configuration table (32 * 64bits) */
/* [한국어] IOMMU 자신이 인터럽트를 보낼 때 쓸 MSI 설정 테이블의 시작(0x300).
 * 벡터마다 주소/데이터/제어 세 항목이 16바이트를 차지하며 32개가 늘어선다.
 * 주의: 이것은 IOMMU가 소프트웨어에 알림을 보내는 용도이지,
 * 디바이스의 MSI를 재라우팅하는 MSI 페이지 테이블과는 별개다. */
#define RISCV_IOMMU_REG_MSI_CFG_TBL	0x0300
/* [한국어] n번째 벡터의 MSI 목적지 주소 레지스터 오프셋(16바이트 간격). */
#define RISCV_IOMMU_REG_MSI_CFG_TBL_ADDR(_n) \
					(RISCV_IOMMU_REG_MSI_CFG_TBL + ((_n) * 0x10))
/* [한국어] 그 주소 레지스터에서 유효한 비트(2~55). 하위 2비트는 4바이트
 * 정렬 때문에 항상 0이라 필드에서 빠져 있다. */
#define RISCV_IOMMU_MSI_CFG_TBL_ADDR	GENMASK_ULL(55, 2)
/* [한국어] n번째 벡터의 MSI 데이터 레지스터 오프셋(주소 + 8). */
#define RISCV_IOMMU_REG_MSI_CFG_TBL_DATA(_n) \
					(RISCV_IOMMU_REG_MSI_CFG_TBL + ((_n) * 0x10) + 0x08)
/* [한국어] MSI 데이터 필드(하위 32비트) — 인터럽트를 식별하는 페이로드다. */
#define RISCV_IOMMU_MSI_CFG_TBL_DATA	GENMASK_ULL(31, 0)
/* [한국어] n번째 벡터의 제어 레지스터 오프셋(주소 + 12).
 * 데이터 레지스터의 상위 절반과 같은 8바이트 안에 놓인다. */
#define RISCV_IOMMU_REG_MSI_CFG_TBL_CTRL(_n) \
					(RISCV_IOMMU_REG_MSI_CFG_TBL + ((_n) * 0x10) + 0x0C)
/* [한국어] 그 벡터의 마스크 비트. 세우면 해당 MSI가 억제된다. */
#define RISCV_IOMMU_MSI_CFG_TBL_CTRL_M	BIT_ULL(0)

/* [한국어] IOMMU MMIO 영역 전체의 크기 4KB. 드라이버가 ioremap 할 범위이자,
 * 디바이스 트리의 reg 크기가 이보다 작으면 거부하는 기준이다. */
#define RISCV_IOMMU_REG_SIZE	0x1000

/*
 * Chapter 2: Data structures
 */

/*
 * Device Directory Table macros for non-leaf nodes
 */
/* [한국어] DDT 비리프(non-leaf) 엔트리의 유효 비트. 이 비트가 0이면
 * 그 하위 트리가 존재하지 않으므로 워크가 중단되고 폴트가 난다. */
#define RISCV_IOMMU_DDTE_V	BIT_ULL(0)
/* [한국어] DDT 비리프 엔트리가 가리키는 다음 레벨 테이블의 물리 페이지 번호.
 * 리프 엔트리는 이 형식이 아니라 struct riscv_iommu_dc 전체다. */
#define RISCV_IOMMU_DDTE_PPN	RISCV_IOMMU_PPN_FIELD

/**
 * struct riscv_iommu_dc - Device Context
 * @tc: Translation Control
 * @iohgatp: I/O Hypervisor guest address translation and protection
 *	     (Second stage context)
 * @ta: Translation Attributes
 * @fsc: First stage context
 * @msiptp: MSI page table pointer
 * @msi_addr_mask: MSI address mask
 * @msi_addr_pattern: MSI address pattern
 * @_reserved: Reserved for future use, padding
 *
 * This structure is used for leaf nodes on the Device Directory Table,
 * in case RISCV_IOMMU_CAPABILITIES_MSI_FLAT is not set, the bottom 4 fields
 * are not present and are skipped with pointer arithmetic to avoid
 * casting, check out riscv_iommu_get_dc().
 * See section 2.1 for more details
 */
/* [한국어] Device Context — 디바이스 하나의 변환 설정 전부를 담은 DDT 리프 엔트리.
 * device_id로 DDT를 걷다 마지막 레벨에서 이 구조체를 만난다.
 * 크기가 하드웨어에 따라 달라지는 드문 구조체다: MSI_FLAT 능력이 없으면
 * 뒤쪽 4개 필드(msiptp 이하)가 아예 존재하지 않아 32바이트가 되고,
 * riscv_iommu_get_dc()가 캐스팅 대신 포인터 산술로 그 차이를 흡수한다.
 * 설정자: 드라이버의 attach 경로가 필드를 채운 뒤 IODIR.INVAL_DDT 명령으로
 *         하드웨어 캐시를 무효화한다.
 * 읽는 자: IOMMU 하드웨어가 DMA마다 참조한다. */
struct riscv_iommu_dc {
	u64 tc;
	/* [한국어] Translation Control — 이 디바이스의 변환 동작을 켜고 끄는
	 * 비트들의 묶음(RISCV_IOMMU_DC_TC_* 참조).
	 * 가장 중요한 것이 최하위 V 비트로, 이것이 0이면 엔트리 전체가 무효다.
	 * 설정자: attach가 마지막에 V를 세워 엔트리를 활성화한다(나머지 필드를
	 *         먼저 채운 뒤에 세워야 반쯤 구성된 엔트리가 쓰이지 않는다).
	 * 읽는 자: 하드웨어가 매 트랜잭션마다.
	 * 값 범위: V, EN_ATS, EN_PRI, PDTV, DPE 등의 비트 조합. */

	u64 iohgatp;
	/* [한국어] 2단계(G-stage) 주소 변환 컨텍스트 — 게스트 물리를 호스트
	 * 물리로 바꾸는 페이지 테이블의 위치와 모드, 그리고 GSCID.
	 * 필드 구성: PPN(0~43) | GSCID(44~59) | MODE(60~63).
	 * 설정자: 가상화 환경에서 VMM이 지정한 2단계 테이블을 attach가 기록한다.
	 *         베어메탈에서는 MODE를 BARE로 두어 2단계를 끈다.
	 * 읽는 자: 하드웨어. GSCID는 무효화 명령의 범위 지정에도 쓰인다.
	 * 이름의 유래: RISC-V CPU의 hgatp CSR과 같은 배치를 따른다. */

	u64 ta;
	/* [한국어] Translation Attributes — 현재 스펙에서는 PSCID(비트 12~31)
	 * 하나만 의미가 있다. PSCID는 1단계 변환의 주소공간 식별자로,
	 * TLB 엔트리를 주소공간별로 구분해 프로세스가 바뀔 때 전체를 비우지
	 * 않아도 되게 한다(CPU의 ASID에 해당).
	 * 설정자: 도메인 생성 시 할당된 PSCID를 attach가 기록한다.
	 * 읽는 자: 하드웨어, 그리고 IOTINVAL.VMA 명령의 PSCID 필드. */

	u64 fsc;
	/* [한국어] First Stage Context — 이 필드의 해석이 tc의 PDTV 비트에
	 * 따라 완전히 달라진다는 점이 핵심이다.
	 * PDTV == 0: iosatp — 1단계 페이지 테이블을 직접 가리킨다(PASID 미사용).
	 * PDTV == 1: pdtp — Process Directory Table을 가리킨다. process_id로
	 *            한 번 더 걸어 struct riscv_iommu_pc를 찾고, 그 안의 fsc가
	 *            비로소 페이지 테이블을 가리킨다.
	 * 필드 구성: PPN(0~43) | MODE(60~63).
	 * 설정자: attach가 도메인 종류에 따라 둘 중 하나로 채운다.
	 * 읽는 자: 하드웨어의 1단계 워크. */

	u64 msiptp;
	/* [한국어] MSI 페이지 테이블 포인터 — 이 디바이스가 보낸 MSI를
	 * 재라우팅할 평면 테이블의 위치와 모드.
	 * 필드 구성: PPN(0~43) | MODE(60~63). MODE는 OFF(0) 또는 FLAT(1).
	 * 존재 조건: MSI_FLAT 능력이 있을 때만 이 필드가 실재한다.
	 * 설정자: 가상화 환경에서 게스트의 인터럽트 파일로 MSI를 돌릴 때 채운다.
	 * 읽는 자: 하드웨어가 MSI 쓰기를 감지했을 때. */

	u64 msi_addr_mask;
	/* [한국어] 어떤 쓰기를 MSI로 인식할지 판별하는 마스크(비트 0~51).
	 * 하드웨어는 (쓰기 주소 >> 12) & ~mask 가 아래 pattern과 같은지 보고
	 * MSI 여부를 판정한다. 즉 mask는 "무시할 비트"를 표시한다.
	 * 설정자: 플랫폼의 인터럽트 컨트롤러 주소 배치에 맞춰 드라이버가 채운다.
	 * 읽는 자: 하드웨어의 MSI 감지 로직.
	 * 존재 조건: MSI_FLAT 능력이 있을 때만. */

	u64 msi_addr_pattern;
	/* [한국어] 위 마스크와 짝을 이루는 비교 대상 패턴(비트 0~51).
	 * mask로 걸러 낸 뒤 이 값과 일치하면 MSI로 간주해 msiptp의 테이블로
	 * 재라우팅하고, 아니면 평범한 메모리 쓰기로 변환한다.
	 * 설정자/읽는 자/존재 조건: msi_addr_mask와 동일. */

	u64 _reserved;
	/* [한국어] 예약 필드 — 구조체를 64바이트로 맞추는 패딩이기도 하다.
	 * 소프트웨어는 0으로 두어야 하며, 향후 스펙 확장에 쓰일 자리다.
	 * 이 필드 덕분에 DC 배열의 인덱싱이 2의 거듭제곱 크기로 떨어져
	 * 주소 계산이 시프트 한 번으로 끝난다. */
};

/* Translation control fields */
/* [한국어] DC 유효 비트. 나머지 필드를 모두 채운 뒤 마지막에 세워야
 * 하드웨어가 반쯤 구성된 엔트리를 쓰지 않는다. */
#define RISCV_IOMMU_DC_TC_V		BIT_ULL(0)
/* [한국어] 이 디바이스의 PCIe ATS를 허용한다. 세우면 디바이스가 변환
 * 요청을 보내고 결과를 자기 ATC에 캐시할 수 있다. */
#define RISCV_IOMMU_DC_TC_EN_ATS	BIT_ULL(1)
/* [한국어] 이 디바이스의 PCIe PRI(Page Request Interface)를 허용한다.
 * 세우면 디바이스가 페이지 요청을 보낼 수 있고, 그것이 PQ에 쌓인다.
 * ATS가 켜져 있어야 의미가 있다. */
#define RISCV_IOMMU_DC_TC_EN_PRI	BIT_ULL(2)
/* [한국어] ATS 변환 응답으로 최종 물리 주소 대신 게스트 물리 주소를
 * 돌려준다. 디바이스에 호스트 주소를 노출하지 않으려는 가상화 설정이다. */
#define RISCV_IOMMU_DC_TC_T2GPA		BIT_ULL(3)
/* [한국어] Disable Translation Fault reporting — 이 디바이스의 폴트를
 * 폴트 큐에 기록하지 않는다. 폴트가 폭주하는 디바이스를 격리할 때 쓴다. */
#define RISCV_IOMMU_DC_TC_DTF		BIT_ULL(4)
/* [한국어] Process Directory Table Valid — 위에서 설명한 fsc 필드의
 * 해석을 바꾸는 결정적인 비트다. 1이면 fsc가 PDT를 가리킨다. */
#define RISCV_IOMMU_DC_TC_PDTV		BIT_ULL(5)
/* [한국어] Page Request Response Required — 페이지 요청에 반드시 응답을
 * 보내야 하는지 표시한다. */
#define RISCV_IOMMU_DC_TC_PRPR		BIT_ULL(6)
/* [한국어] G-stage Access/Dirty Enable — 2단계 페이지 테이블의 A/D 비트를
 * 하드웨어가 갱신하게 한다. 꺼져 있으면 소프트웨어가 미리 세워야 한다. */
#define RISCV_IOMMU_DC_TC_GADE		BIT_ULL(7)
/* [한국어] S-stage Access/Dirty Enable — 1단계 테이블의 A/D 비트를
 * 하드웨어가 갱신하게 한다. */
#define RISCV_IOMMU_DC_TC_SADE		BIT_ULL(8)
/* [한국어] Default Process Enable — process_id 없이 온 요청을 PDT의
 * 0번 엔트리로 처리한다. PASID를 쓰지 않는 트래픽에 기본 주소공간을
 * 주는 장치다. */
#define RISCV_IOMMU_DC_TC_DPE		BIT_ULL(9)
/* [한국어] S-stage Big-Endian — 1단계 페이지 테이블을 빅엔디언으로
 * 해석한다. 리눅스에서는 쓰지 않는다. */
#define RISCV_IOMMU_DC_TC_SBE		BIT_ULL(10)
/* [한국어] S-stage XLEN — 1단계 변환을 32비트 모드(Sv32)로 동작시킨다.
 * 이 비트에 따라 fsc의 MODE 값 해석이 달라진다. */
#define RISCV_IOMMU_DC_TC_SXL		BIT_ULL(11)

/* Second-stage (aka G-stage) context fields */
/* [한국어] iohgatp의 2단계 페이지 테이블 물리 페이지 번호(비트 0~43).
 * ATP 계열이라 공통 PPN 필드가 아닌 ATP_PPN 필드를 쓴다. */
#define RISCV_IOMMU_DC_IOHGATP_PPN	RISCV_IOMMU_ATP_PPN_FIELD
/* [한국어] GSCID(Guest Soft-Context ID, 비트 44~59) — 게스트 하나를
 * 식별한다. 2단계 TLB 엔트리를 게스트별로 구분해, 게스트가 바뀔 때
 * 그 게스트 것만 무효화할 수 있게 한다(IOTINVAL.GVMA의 GSCID 필드). */
#define RISCV_IOMMU_DC_IOHGATP_GSCID	GENMASK_ULL(59, 44)
/* [한국어] 2단계 변환 모드(비트 60~63) — BARE 또는 Sv*x4 중 하나. */
#define RISCV_IOMMU_DC_IOHGATP_MODE	RISCV_IOMMU_ATP_MODE_FIELD

/**
 * enum riscv_iommu_dc_iohgatp_modes - Guest address translation/protection modes
 * @RISCV_IOMMU_DC_IOHGATP_MODE_BARE: No translation/protection
 * @RISCV_IOMMU_DC_IOHGATP_MODE_SV32X4: Sv32x4 (2-bit extension of Sv32), when fctl.GXL == 1
 * @RISCV_IOMMU_DC_IOHGATP_MODE_SV39X4: Sv39x4 (2-bit extension of Sv39), when fctl.GXL == 0
 * @RISCV_IOMMU_DC_IOHGATP_MODE_SV48X4: Sv48x4 (2-bit extension of Sv48), when fctl.GXL == 0
 * @RISCV_IOMMU_DC_IOHGATP_MODE_SV57X4: Sv57x4 (2-bit extension of Sv57), when fctl.GXL == 0
 */
/* [한국어] 2단계 변환 모드 값들.
 * 값 8이 SV32X4와 SV39X4에 중복 배정된 것에 주목: 어느 쪽으로 해석할지는
 * FCTL의 GXL 비트가 결정한다(GXL==1이면 Sv32x4, 0이면 Sv39x4).
 * 즉 같은 숫자가 문맥에 따라 다른 뜻이 되는 인코딩이다.
 * "x4"는 게스트 물리 주소가 2비트 넓어 최상위 테이블이 4배(16KB)가 된다는 뜻이다. */
enum riscv_iommu_dc_iohgatp_modes {
	RISCV_IOMMU_DC_IOHGATP_MODE_BARE = 0,
	/* [한국어] 2단계 변환 없음 — 1단계 결과가 곧 호스트 물리 주소다.
	 * 베어메탈 리눅스가 쓰는 설정이다. */

	RISCV_IOMMU_DC_IOHGATP_MODE_SV32X4 = 8,
	/* [한국어] 32비트 게스트 물리 주소 공간(2단계 테이블).
	 * FCTL.GXL == 1일 때만 이 해석이 적용된다. */

	RISCV_IOMMU_DC_IOHGATP_MODE_SV39X4 = 8,
	/* [한국어] 39비트 게스트 물리 주소 공간(3단계 테이블).
	 * FCTL.GXL == 0일 때의 해석으로, 64비트 게스트의 기본이다. */

	RISCV_IOMMU_DC_IOHGATP_MODE_SV48X4 = 9,

	RISCV_IOMMU_DC_IOHGATP_MODE_SV57X4 = 10
	/* [한국어] 57비트 게스트 물리 주소 공간 — 5단계 2단계-테이블을 쓴다.
	 * 설정자: 중첩 변환을 설정할 때 DC 의 iohgatp 필드 MODE 에 넣는다.
	 * 읽는 자: 하드웨어가 2단계(게스트 물리 → 호스트 물리) 워크를 할 때.
	 * 이름의 X4 가 중요하다: 2단계 테이블의 최상위 단계는 보통보다 네 배 큰
	 *   16KB 다. 게스트 물리 주소가 입력이라 최상위에서 두 비트를 더 받아야 하기
	 *   때문이며, 그래서 1단계용 SV57 과 이름을 구분한다.
	 * 값 범위: 10. 이 enum 의 마지막 값이며, 더 넓은 주소 공간이 필요해지면
	 *   스펙이 다음 번호를 정의할 자리다. */
};

/* Translation attributes fields */
/* [한국어] DC의 ta 필드에서 PSCID가 차지하는 자리(비트 12~31).
 * 1단계 변환의 주소공간 식별자로, TLB를 주소공간별로 구분하게 해 준다. */
#define RISCV_IOMMU_DC_TA_PSCID		GENMASK_ULL(31, 12)

/* First-stage context fields */
/* [한국어] fsc의 물리 페이지 번호 필드 — PDTV 값에 따라 1단계 페이지
 * 테이블 또는 PDT의 위치를 담는다. */
#define RISCV_IOMMU_DC_FSC_PPN		RISCV_IOMMU_ATP_PPN_FIELD
/* [한국어] fsc의 모드 필드 — 역시 PDTV에 따라 iosatp 모드(Sv*) 또는
 * PDT 레벨 수(PD8/PD17/PD20)로 해석된다. */
#define RISCV_IOMMU_DC_FSC_MODE		RISCV_IOMMU_ATP_MODE_FIELD

/**
 * enum riscv_iommu_dc_fsc_atp_modes - First stage address translation/protection modes
 * @RISCV_IOMMU_DC_FSC_MODE_BARE: No translation/protection
 * @RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV32: Sv32, when dc.tc.SXL == 1
 * @RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV39: Sv39, when dc.tc.SXL == 0
 * @RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV48: Sv48, when dc.tc.SXL == 0
 * @RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV57: Sv57, when dc.tc.SXL == 0
 * @RISCV_IOMMU_DC_FSC_PDTP_MODE_PD8: 1lvl PDT, 8bit process ids
 * @RISCV_IOMMU_DC_FSC_PDTP_MODE_PD17: 2lvl PDT, 17bit process ids
 * @RISCV_IOMMU_DC_FSC_PDTP_MODE_PD20: 3lvl PDT, 20bit process ids
 *
 * FSC holds IOSATP when RISCV_IOMMU_DC_TC_PDTV is 0 and PDTP otherwise.
 * IOSATP controls the first stage address translation (same as the satp register on
 * the RISC-V MMU), and PDTP holds the process directory table, used to select a
 * first stage page table based on a process id (for devices that support multiple
 * process ids).
 */
/* [한국어] fsc의 MODE 필드가 가질 수 있는 값들. 하나의 enum에 두 종류의
 * 값이 섞여 있는데, 이는 fsc 자체가 PDTV에 따라 두 가지로 해석되기 때문이다.
 * PDTV == 0이면 IOSATP_MODE_* 쪽(8/9/10)을, PDTV == 1이면 PDTP_MODE_*
 * 쪽(1/2/3)을 쓴다. 값 범위가 겹치지 않아 혼동이 생기지 않는다.
 * 여기서도 값 8이 SV32와 SV39에 중복 배정되어 있고, tc의 SXL 비트가
 * 어느 쪽인지 결정한다(iohgatp의 GXL과 같은 방식이다). */
enum riscv_iommu_dc_fsc_atp_modes {
	RISCV_IOMMU_DC_FSC_MODE_BARE = 0,
	/* [한국어] 1단계 변환 없음 — 디바이스가 낸 주소가 그대로 2단계 입력이
	 * 된다. 2단계만 쓰는 가상화 구성이나 통과 모드에서 쓴다. */

	RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV32 = 8,
	/* [한국어] 32비트 가상 주소(2단계 테이블). tc.SXL == 1일 때의 해석이다. */

	RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV39 = 8,
	/* [한국어] 39비트 가상 주소(3단계 테이블). tc.SXL == 0일 때의 해석으로,
	 * 64비트 리눅스가 기본으로 쓰는 모드다. */

	RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV48 = 9,

	RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV57 = 10,
	/* [한국어] 57비트 가상 주소 공간 — 5단계 1단계-테이블을 쓴다.
	 * 설정자: 1단계 변환을 설정할 때 DC 의 fsc 필드 MODE 에 넣는다.
	 * 읽는 자: 하드웨어가 1단계(IOVA → 게스트 물리) 워크를 할 때.
	 * 위 IOHGATP 의 SV57X4 와 값이 같은 10 이지만 다른 필드의 인코딩이다 —
	 *   이쪽은 최상위 테이블이 보통 크기(4KB)다. 두 상수를 섞어 쓰면 하드웨어가
	 *   잘못된 크기의 테이블을 읽는다.
	 * 같은 fsc 필드가 PDTP 모드로도 해석될 수 있어(아래 PDTP_MODE_* 참고),
	 *   어느 해석인지는 DC 의 다른 비트가 정한다. */

	RISCV_IOMMU_DC_FSC_PDTP_MODE_PD8 = 1,
	/* [한국어] 1단계 PDT — process_id 8비트(256개)를 다룬다.
	 * PASID를 조금만 쓰는 디바이스에 적합하며 워크가 한 번으로 끝난다. */

	RISCV_IOMMU_DC_FSC_PDTP_MODE_PD17 = 2,
	/* [한국어] 2단계 PDT — process_id 17비트를 다룬다. */

	RISCV_IOMMU_DC_FSC_PDTP_MODE_PD20 = 3
	/* [한국어] 3단계 PDT — process_id 20비트(PCIe PASID 전체 폭)를 다룬다.
	 * SVA를 온전히 지원하려면 이 모드가 필요하다. */
};

/* MSI page table pointer */
/* [한국어] msiptp의 MSI 페이지 테이블 물리 페이지 번호. */
#define RISCV_IOMMU_DC_MSIPTP_PPN	RISCV_IOMMU_ATP_PPN_FIELD
/* [한국어] msiptp의 모드 필드 — OFF 또는 FLAT. */
#define RISCV_IOMMU_DC_MSIPTP_MODE	RISCV_IOMMU_ATP_MODE_FIELD
/* [한국어] MSI 재라우팅 비활성 — 이 디바이스의 MSI는 평범한 메모리 쓰기로
 * 취급되어 일반 변환 경로를 탄다. */
#define RISCV_IOMMU_DC_MSIPTP_MODE_OFF	0
/* [한국어] 평면(flat) MSI 페이지 테이블 사용 — 다단계가 아니라 배열 하나이며
 * 모든 엔트리가 리프다(struct riscv_iommu_msipte). */
#define RISCV_IOMMU_DC_MSIPTP_MODE_FLAT	1

/* MSI address mask */
/* [한국어] MSI 감지 마스크 필드(비트 0~51). 페이지 번호 단위로 비교하므로
 * 52비트면 충분하다. */
#define RISCV_IOMMU_DC_MSI_ADDR_MASK	GENMASK_ULL(51, 0)

/* MSI address pattern */
/* [한국어] MSI 감지 패턴 필드(비트 0~51). 위 마스크로 걸러 낸 결과와
 * 이 값을 비교해 MSI 여부를 판정한다. */
#define RISCV_IOMMU_DC_MSI_PATTERN	GENMASK_ULL(51, 0)

/**
 * struct riscv_iommu_pc - Process Context
 * @ta: Translation Attributes
 * @fsc: First stage context
 *
 * This structure is used for leaf nodes on the Process Directory Table
 * See section 2.3 for more details
 */
/* [한국어] Process Context — PASID(process_id) 하나에 대응하는 1단계 변환 설정.
 * DC의 tc.PDTV가 1일 때만 쓰인다: device_id로 DC를 찾고, 그 fsc가 가리키는
 * PDT를 process_id로 걸어 이 구조체에 도달한다.
 * DC보다 훨씬 작은 것은 목적지/MSI 같은 디바이스 단위 설정이 필요 없기
 * 때문이다 — 프로세스마다 다른 것은 주소공간(fsc)과 그 식별자(ta의 PSCID)뿐이다.
 * 설정자: SVA 경로가 프로세스의 페이지 테이블을 여기에 연결한다.
 * 읽는 자: 하드웨어의 PDT 워크. 갱신 후에는 IODIR.INVAL_PDT로 캐시를 비운다. */
struct riscv_iommu_pc {
	u64 ta;
	/* [한국어] Translation Attributes — 유효 비트, 슈퍼바이저 접근 허용,
	 * SUM(사용자 페이지 접근 허용), 그리고 PSCID를 담는다.
	 * DC의 ta와 이름은 같지만 유효 비트가 추가되어 있다는 점이 다르다.
	 * 설정자: SVA attach가 mm의 주소공간 ID를 PSCID로 넣고 V를 세운다.
	 * 읽는 자: 하드웨어. V가 0이면 그 PASID로 온 요청은 폴트가 된다. */

	u64 fsc;
	/* [한국어] First Stage Context — 이 프로세스의 1단계 페이지 테이블
	 * 위치와 모드. DC와 달리 여기서는 항상 페이지 테이블이며 PDT일 수 없다
	 * (PDT를 또 가리키는 재귀는 없다).
	 * 필드 구성: PPN(0~43) | MODE(60~63).
	 * 설정자: SVA가 프로세스 mm의 페이지 테이블 물리 주소를 넣는다.
	 * 읽는 자: 하드웨어의 1단계 워크. */
};

/* Translation attributes fields */
/* [한국어] PC의 유효 비트. 0이면 이 PASID는 사용할 수 없다. */
#define RISCV_IOMMU_PC_TA_V	BIT_ULL(0)
/* [한국어] ENS(Enable Supervisor) — 이 프로세스 문맥에서 슈퍼바이저 권한
 * 접근을 허용한다. */
#define RISCV_IOMMU_PC_TA_ENS	BIT_ULL(1)
/* [한국어] SUM(permit Supervisor User Memory access) — CPU의 sstatus.SUM과
 * 같은 의미로, 슈퍼바이저 접근이 사용자 페이지(U 비트가 선 페이지)에
 * 접근하는 것을 허용한다. */
#define RISCV_IOMMU_PC_TA_SUM	BIT_ULL(2)
/* [한국어] 이 프로세스 문맥의 PSCID(비트 12~31) — TLB 구분자다. */
#define RISCV_IOMMU_PC_TA_PSCID	GENMASK_ULL(31, 12)

/* First stage context fields */
/* [한국어] PC의 fsc가 담는 페이지 테이블 물리 페이지 번호. */
#define RISCV_IOMMU_PC_FSC_PPN	RISCV_IOMMU_ATP_PPN_FIELD
/* [한국어] PC의 fsc가 담는 변환 모드(Sv39 등). */
#define RISCV_IOMMU_PC_FSC_MODE	RISCV_IOMMU_ATP_MODE_FIELD

/*
 * Chapter 3: In-memory queue interface
 */

/**
 * struct riscv_iommu_command - Generic IOMMU command structure
 * @dword0: Includes the opcode and the function identifier
 * @dword1: Opcode specific data
 *
 * The commands are interpreted as two 64bit fields, where the first
 * 7bits of the first field are the opcode which also defines the
 * command's format, followed by a 3bit field that specifies the
 * function invoked by that command, and the rest is opcode-specific.
 * This is a generic struct which will be populated differently
 * according to each command. For more infos on the commands and
 * the command queue check section 3.1.
 */
/* [한국어] 커맨드 큐에 넣는 명령 하나(16바이트).
 * 모든 명령이 같은 크기와 같은 머리 구조를 가지므로, 큐는 이 구조체의
 * 단순 배열이 된다. 명령 종류에 따라 나머지 비트의 의미만 달라진다.
 * 설정자: 파일 끝의 riscv_iommu_cmd_* 빌더들이 채운다.
 * 읽는 자: IOMMU 하드웨어가 CQT가 올라간 뒤 순서대로 읽어 처리한다. */
struct riscv_iommu_command {
	u64 dword0;
	/* [한국어] 명령의 머리 — 최하위 7비트가 opcode(명령 종류),
	 * 그다음 3비트가 func(같은 opcode 안의 세부 동작)이고,
	 * 나머지는 명령별 필드(PSCID, GSCID, DID, PID 등)다.
	 * 설정자: 빌더 함수가 FIELD_PREP으로 조립한다.
	 * 읽는 자: 하드웨어가 opcode/func를 먼저 보고 나머지 해석을 결정한다. */

	u64 dword1;
	/* [한국어] 명령별 데이터 — 무효화 명령이면 주소, IOFENCE면 완료를
	 * 알릴 주소, IODIR이면 예약(0), ATS면 페이로드가 들어간다.
	 * 설정자: 빌더 함수. 기본 빌더는 0으로 두고 set_* 함수가 채운다.
	 * 읽는 자: 하드웨어. */
};

/* Fields on dword0, common for all commands */
/* [한국어] 모든 명령이 공유하는 opcode 필드(비트 0~6). 이 값이 명령의
 * 전체 형식을 결정한다. */
#define RISCV_IOMMU_CMD_OPCODE	GENMASK_ULL(6, 0)
/* [한국어] 같은 opcode 안에서 세부 동작을 고르는 func 필드(비트 7~9).
 * 예를 들어 IOTINVAL opcode에서 func가 0이면 VMA(1단계), 1이면 GVMA(2단계)다.
 * 매크로 이름 앞의 탭 문자는 원본 그대로다. */
#define	RISCV_IOMMU_CMD_FUNC	GENMASK_ULL(9, 7)

/* 3.1.1 IOMMU Page-table cache invalidation */
/* Fields on dword0 */
/* [한국어] 페이지 테이블 캐시(IOTLB) 무효화 명령의 opcode(1). */
#define RISCV_IOMMU_CMD_IOTINVAL_OPCODE		1
/* [한국어] func = VMA — 1단계(S/VS-stage) 변환 캐시를 무효화한다.
 * PSCID로 주소공간을 좁힐 수 있다. */
#define RISCV_IOMMU_CMD_IOTINVAL_FUNC_VMA	0
/* [한국어] func = GVMA — 2단계(G-stage) 변환 캐시를 무효화한다.
 * GSCID로 게스트를 좁힐 수 있다. */
#define RISCV_IOMMU_CMD_IOTINVAL_FUNC_GVMA	1
/* [한국어] Address Valid — dword1의 주소 필드가 유효함을 표시한다.
 * 세우지 않으면 주소 무관하게 전체를 무효화한다. */
#define RISCV_IOMMU_CMD_IOTINVAL_AV		BIT_ULL(10)
/* [한국어] 무효화 대상 PSCID(비트 12~31) — 특정 주소공간만 비운다. */
#define RISCV_IOMMU_CMD_IOTINVAL_PSCID		GENMASK_ULL(31, 12)
/* [한국어] PSCID Valid — 위 PSCID 필드가 유효함을 표시한다.
 * 없으면 모든 주소공간이 대상이 된다. */
#define RISCV_IOMMU_CMD_IOTINVAL_PSCV		BIT_ULL(32)
/* [한국어] GSCID Valid — 아래 GSCID 필드가 유효함을 표시한다. */
#define RISCV_IOMMU_CMD_IOTINVAL_GV		BIT_ULL(33)
/* [한국어] 무효화 대상 GSCID(비트 44~59) — 특정 게스트만 비운다. */
#define RISCV_IOMMU_CMD_IOTINVAL_GSCID		GENMASK_ULL(59, 44)
/* dword1[61:10] is the 4K-aligned page address */
/* [한국어] 무효화할 페이지의 주소 필드(dword1의 비트 10~61).
 * 주소가 아니라 페이지 번호가 들어가므로, 빌더가 PHYS_PFN()으로 변환해
 * 넣는다. 하위 10비트가 비어 있는 것은 이 필드가 비트 10부터 시작하기 때문이다. */
#define RISCV_IOMMU_CMD_IOTINVAL_ADDR		GENMASK_ULL(61, 10)

/* 3.1.2 IOMMU Command Queue Fences */
/* Fields on dword0 */
/* [한국어] 커맨드 큐 펜스 명령의 opcode(2). 앞선 모든 명령이 완료될
 * 때까지 기다리게 하는 동기화 지점이다. */
#define RISCV_IOMMU_CMD_IOFENCE_OPCODE		2
/* [한국어] func = C — 유일하게 정의된 펜스 종류. */
#define RISCV_IOMMU_CMD_IOFENCE_FUNC_C		0
/* [한국어] Address Valid — 펜스 완료 시 dword1이 가리키는 주소에
 * DATA 필드 값을 쓰라고 지시한다. 소프트웨어는 그 메모리를 폴링해
 * 완료를 감지한다(인터럽트 없이 대기하는 방법). */
#define RISCV_IOMMU_CMD_IOFENCE_AV		BIT_ULL(10)
/* [한국어] Wired-Signaled Interrupt — 펜스 완료를 배선 인터럽트로 알린다. */
#define RISCV_IOMMU_CMD_IOFENCE_WSI		BIT_ULL(11)
/* [한국어] Previous Reads — 앞선 읽기들이 완료되기를 기다린다. */
#define RISCV_IOMMU_CMD_IOFENCE_PR		BIT_ULL(12)
/* [한국어] Previous Writes — 앞선 쓰기들이 완료되기를 기다린다.
 * 기본 빌더가 PR과 PW를 함께 세우는 이유는, 무효화 후 모든 진행 중인
 * 트랜잭션이 끝났음을 보장해야 하기 때문이다. */
#define RISCV_IOMMU_CMD_IOFENCE_PW		BIT_ULL(13)
/* [한국어] AV가 켜졌을 때 목적지 주소에 쓸 32비트 값(비트 32~63).
 * 소프트웨어가 미리 정한 표식을 넣어, 그 값이 나타나면 완료로 판단한다. */
#define RISCV_IOMMU_CMD_IOFENCE_DATA		GENMASK_ULL(63, 32)
/* dword1 is the address, word-size aligned and shifted to the right by two bits. */

/* 3.1.3 IOMMU Directory cache invalidation */
/* Fields on dword0 */
/* [한국어] 디렉토리 캐시(DDT/PDT 캐시) 무효화 명령의 opcode(3).
 * DC나 PC를 고친 뒤 반드시 보내야 하드웨어가 새 값을 읽는다. */
#define RISCV_IOMMU_CMD_IODIR_OPCODE		3
/* [한국어] func = DDT 무효화 — 디바이스 컨텍스트 캐시를 비운다. */
#define RISCV_IOMMU_CMD_IODIR_FUNC_INVAL_DDT	0
/* [한국어] func = PDT 무효화 — 프로세스 컨텍스트 캐시를 비운다. */
#define RISCV_IOMMU_CMD_IODIR_FUNC_INVAL_PDT	1
/* [한국어] 무효화 대상 process_id(비트 12~31). PDT 무효화에서 특정
 * PASID만 비울 때 쓴다. */
#define RISCV_IOMMU_CMD_IODIR_PID		GENMASK_ULL(31, 12)
/* [한국어] Device Valid — 아래 DID 필드가 유효함을 표시한다.
 * 없으면 모든 디바이스가 대상이 된다. */
#define RISCV_IOMMU_CMD_IODIR_DV		BIT_ULL(33)
/* [한국어] 무효화 대상 device_id(비트 40~63). */
#define RISCV_IOMMU_CMD_IODIR_DID		GENMASK_ULL(63, 40)
/* dword1 is reserved for standard use */

/* 3.1.4 IOMMU PCIe ATS */
/* Fields on dword0 */
/* [한국어] PCIe ATS 관련 명령의 opcode(4). 디바이스의 ATC(주소 변환 캐시)를
 * 무효화하거나 페이지 요청에 응답할 때 쓴다. */
#define RISCV_IOMMU_CMD_ATS_OPCODE		4
/* [한국어] func = INVAL — 디바이스의 ATC를 무효화한다. 디바이스가 응답을
 * 보내야 완료되므로, 이 명령은 타임아웃(CMD_TO)이 날 수 있다. */
#define RISCV_IOMMU_CMD_ATS_FUNC_INVAL		0
/* [한국어] func = PRGR — Page Request Group Response. 디바이스가 보낸
 * 페이지 요청 그룹에 대한 응답을 돌려준다. */
#define RISCV_IOMMU_CMD_ATS_FUNC_PRGR		1
/* [한국어] 대상 process_id(비트 12~31). */
#define RISCV_IOMMU_CMD_ATS_PID			GENMASK_ULL(31, 12)
/* [한국어] PID Valid — 위 PID가 유효함을 표시한다. */
#define RISCV_IOMMU_CMD_ATS_PV			BIT_ULL(32)
/* [한국어] Destination Segment Valid — 아래 DSEG 필드가 유효함을 표시한다.
 * 여러 PCIe 세그먼트가 있는 시스템에서 필요하다. */
#define RISCV_IOMMU_CMD_ATS_DSV			BIT_ULL(33)
/* [한국어] 대상 디바이스의 PCIe Requester ID(비트 40~55) —
 * bus/device/function을 합친 16비트 값이다. */
#define RISCV_IOMMU_CMD_ATS_RID			GENMASK_ULL(55, 40)
/* [한국어] 대상 PCIe 세그먼트 번호(비트 56~63). */
#define RISCV_IOMMU_CMD_ATS_DSEG		GENMASK_ULL(63, 56)
/* dword1 is the ATS payload, two different payload types for INVAL and PRGR */

/* ATS.INVAL payload*/
/* [한국어] Global — 이 무효화가 전역인지(모든 PASID) 표시한다. */
#define RISCV_IOMMU_CMD_ATS_INVAL_G		BIT_ULL(0)
/* Bits 1 - 10 are zeroed */
/* [한국어] Size — 무효화 범위가 한 페이지보다 큰지 표시한다. 세우면
 * 아래 UADDR의 하위 비트들이 범위 크기를 인코딩하는 방식으로 해석된다
 * (PCIe ATS 스펙의 무효화 크기 인코딩). */
#define RISCV_IOMMU_CMD_ATS_INVAL_S		BIT_ULL(11)
/* [한국어] 무효화할 미변환(untranslated) 주소의 페이지 부분(비트 12~63). */
#define RISCV_IOMMU_CMD_ATS_INVAL_UADDR		GENMASK_ULL(63, 12)

/* ATS.PRGR payload */
/* Bits 0 - 31 are zeroed */
/* [한국어] 응답할 페이지 요청 그룹의 인덱스(비트 32~40). 디바이스가
 * 요청할 때 붙인 값을 그대로 돌려줘야 짝이 맞는다. */
#define RISCV_IOMMU_CMD_ATS_PRGR_PRG_INDEX	GENMASK_ULL(40, 32)
/* Bits 41 - 43 are zeroed */
/* [한국어] 응답 코드(비트 44~47) — 성공/실패/영구 실패 등을 나타낸다.
 * 실패를 보내면 디바이스가 해당 트랜잭션을 포기한다. */
#define RISCV_IOMMU_CMD_ATS_PRGR_RESP_CODE	GENMASK_ULL(47, 44)
/* [한국어] 응답을 보낼 대상 디바이스 ID(비트 48~63). */
#define RISCV_IOMMU_CMD_ATS_PRGR_DST_ID		GENMASK_ULL(63, 48)

/**
 * struct riscv_iommu_fq_record - Fault/Event Queue Record
 * @hdr: Header, includes fault/event cause, PID/DID, transaction type etc
 * @_reserved: Low 32bits for custom use, high 32bits for standard use
 * @iotval: Transaction-type/cause specific format
 * @iotval2: Cause specific format
 *
 * The fault/event queue reports events and failures raised when
 * processing transactions. Each record is a 32byte structure where
 * the first dword has a fixed format for providing generic infos
 * regarding the fault/event, and two more dwords are there for
 * fault/event-specific information. For more details see section
 * 3.2.
 */
/* [한국어] 폴트 큐에 하드웨어가 써 넣는 기록 하나(32바이트).
 * 첫 dword만 형식이 고정되어 있고 나머지는 원인에 따라 달라진다 —
 * 이 유연성 덕분에 페이지 폴트든 테이블 손상이든 같은 큐로 보고할 수 있다.
 * 설정자: IOMMU 하드웨어.
 * 읽는 자: 드라이버의 폴트 인터럽트 처리 경로. */
struct riscv_iommu_fq_record {
	u64 hdr;
	/* [한국어] 고정 형식의 머리 — 원인 코드(0~11), process_id(12~31),
	 * PID 유효 비트(32), 특권 접근 여부(33), 트랜잭션 종류(34~39),
	 * device_id(40~63)를 담는다.
	 * 이 한 워드만 봐도 "어느 디바이스의 어떤 요청이 왜 실패했는지"를
	 * 알 수 있게 설계되어 있다.
	 * 읽는 자: 드라이버가 FIELD_GET으로 각 필드를 뽑아 로그를 남긴다. */

	u64 _reserved;
	/* [한국어] 예약 필드 — 하위 32비트는 구현 정의(커스텀) 용도,
	 * 상위 32비트는 향후 표준 확장용이다.
	 * 소프트웨어는 이 값을 해석하지 않는다. 구조체를 32바이트로 맞추는
	 * 역할도 겸해, 큐 인덱싱이 시프트로 끝나게 해 준다. */

	u64 iotval;
	/* [한국어] 원인/트랜잭션 종류에 따라 형식이 달라지는 값.
	 * 대개 폴트를 일으킨 가상 주소(IOVA)가 들어간다.
	 * 읽는 자: 드라이버가 어떤 주소가 문제였는지 보고할 때. */

	u64 iotval2;
	/* [한국어] 두 번째 원인별 값. 2단계 변환 폴트에서는 게스트 물리
	 * 주소가 들어가는 등, iotval만으로 부족한 정보를 담는다.
	 * 읽는 자: 드라이버의 폴트 보고. */
};

/* Fields on header */
/* [한국어] 폴트 원인 코드(비트 0~11) — enum riscv_iommu_fq_causes 값이 들어간다. */
#define RISCV_IOMMU_FQ_HDR_CAUSE	GENMASK_ULL(11, 0)
/* [한국어] 폴트를 일으킨 요청의 process_id(비트 12~31). */
#define RISCV_IOMMU_FQ_HDR_PID		GENMASK_ULL(31, 12)
/* [한국어] 위 PID 필드가 유효한지 표시하는 비트. PASID 없는 요청이면 0이다. */
#define RISCV_IOMMU_FQ_HDR_PV		BIT_ULL(32)
/* [한국어] 그 요청이 특권(supervisor) 접근이었는지 표시하는 비트. */
#define RISCV_IOMMU_FQ_HDR_PRIV		BIT_ULL(33)
/* [한국어] 트랜잭션 종류(비트 34~39) — enum riscv_iommu_fq_ttypes 값.
 * 읽기였는지 쓰기였는지 명령어 인출이었는지 알려 준다. */
#define RISCV_IOMMU_FQ_HDR_TTYP		GENMASK_ULL(39, 34)
/* [한국어] 폴트를 일으킨 device_id(비트 40~63). 어느 디바이스인지 특정한다. */
#define RISCV_IOMMU_FQ_HDR_DID		GENMASK_ULL(63, 40)

/**
 * enum riscv_iommu_fq_causes - Fault/event cause values
 * @RISCV_IOMMU_FQ_CAUSE_INST_FAULT: Instruction access fault
 * @RISCV_IOMMU_FQ_CAUSE_RD_ADDR_MISALIGNED: Read address misaligned
 * @RISCV_IOMMU_FQ_CAUSE_RD_FAULT: Read load fault
 * @RISCV_IOMMU_FQ_CAUSE_WR_ADDR_MISALIGNED: Write/AMO address misaligned
 * @RISCV_IOMMU_FQ_CAUSE_WR_FAULT: Write/AMO access fault
 * @RISCV_IOMMU_FQ_CAUSE_INST_FAULT_S: Instruction page fault
 * @RISCV_IOMMU_FQ_CAUSE_RD_FAULT_S: Read page fault
 * @RISCV_IOMMU_FQ_CAUSE_WR_FAULT_S: Write/AMO page fault
 * @RISCV_IOMMU_FQ_CAUSE_INST_FAULT_VS: Instruction guest page fault
 * @RISCV_IOMMU_FQ_CAUSE_RD_FAULT_VS: Read guest page fault
 * @RISCV_IOMMU_FQ_CAUSE_WR_FAULT_VS: Write/AMO guest page fault
 * @RISCV_IOMMU_FQ_CAUSE_DMA_DISABLED: All inbound transactions disallowed
 * @RISCV_IOMMU_FQ_CAUSE_DDT_LOAD_FAULT: DDT entry load access fault
 * @RISCV_IOMMU_FQ_CAUSE_DDT_INVALID: DDT entry invalid
 * @RISCV_IOMMU_FQ_CAUSE_DDT_MISCONFIGURED: DDT entry misconfigured
 * @RISCV_IOMMU_FQ_CAUSE_TTYP_BLOCKED: Transaction type disallowed
 * @RISCV_IOMMU_FQ_CAUSE_MSI_LOAD_FAULT: MSI PTE load access fault
 * @RISCV_IOMMU_FQ_CAUSE_MSI_INVALID: MSI PTE invalid
 * @RISCV_IOMMU_FQ_CAUSE_MSI_MISCONFIGURED: MSI PTE misconfigured
 * @RISCV_IOMMU_FQ_CAUSE_MRIF_FAULT: MRIF access fault
 * @RISCV_IOMMU_FQ_CAUSE_PDT_LOAD_FAULT: PDT entry load access fault
 * @RISCV_IOMMU_FQ_CAUSE_PDT_INVALID: PDT entry invalid
 * @RISCV_IOMMU_FQ_CAUSE_PDT_MISCONFIGURED: PDT entry misconfigured
 * @RISCV_IOMMU_FQ_CAUSE_DDT_CORRUPTED: DDT data corruption
 * @RISCV_IOMMU_FQ_CAUSE_PDT_CORRUPTED: PDT data corruption
 * @RISCV_IOMMU_FQ_CAUSE_MSI_PT_CORRUPTED: MSI page table data corruption
 * @RISCV_IOMMU_FQ_CAUSE_MRIF_CORRUIPTED: MRIF data corruption
 * @RISCV_IOMMU_FQ_CAUSE_INTERNAL_DP_ERROR: Internal data path error
 * @RISCV_IOMMU_FQ_CAUSE_MSI_WR_FAULT: IOMMU MSI write access fault
 * @RISCV_IOMMU_FQ_CAUSE_PT_CORRUPTED: First/second stage page table data corruption
 *
 * Values are on table 11 of the spec, encodings 275 - 2047 are reserved for standard
 * use, and 2048 - 4095 for custom use.
 */
/* [한국어] 폴트 원인 코드. 값의 구간이 의미를 나눈다:
 *  1~23  — RISC-V CPU의 예외 코드와 같은 번호 체계다. 즉 IOMMU가 겪는
 *          접근/페이지 폴트를 CPU와 동일하게 표현해, 처리 코드를 공유하기 쉽다.
 *  256~274 — IOMMU 고유의 원인(디렉토리 오류, MSI 테이블 오류, 데이터 손상 등).
 *  275~2047는 향후 표준용, 2048~4095는 구현 정의용으로 비어 있다.
 * 원인의 종류를 보면 이 하드웨어가 실패할 수 있는 지점이 전부 드러난다 —
 * 테이블을 읽는 단계, 엔트리가 무효한 경우, 설정이 잘못된 경우, 그리고
 * 메모리 자체가 손상된 경우까지 각각 다른 코드를 갖는다. */
enum riscv_iommu_fq_causes {
	RISCV_IOMMU_FQ_CAUSE_INST_FAULT = 1,
	/* [한국어] 명령어 인출 접근 폴트 — 그 물리 메모리에 접근할 수 없었다.
	 * 설정자: 하드웨어가 폴트 큐 항목의 CAUSE 필드에 넣는다.
	 * 읽는 자: riscv_iommu_fault() 가 이 값으로 로그를 남기고, 상위에 보고할
	 *   폴트 종류를 정한다.
	 * "접근 폴트"와 "페이지 폴트"의 차이가 이 목록 전체를 이해하는 열쇠다.
	 *   접근 폴트는 변환은 끝났는데 그 물리 주소 자체가 없거나 접근 불가인 경우,
	 *   페이지 폴트(아래 12/13/21/23)는 페이지 테이블에 매핑이나 권한이 없는 경우다.
	 *   전자는 시스템 설정 문제이고 후자는 소프트웨어의 매핑 문제다.
	 * 값 범위: 1. 1~23 은 RISC-V 특권 스펙의 예외 코드와 같은 번호를 쓴다. */

	RISCV_IOMMU_FQ_CAUSE_RD_ADDR_MISALIGNED = 4,
	/* [한국어] 읽기 주소 정렬 오류 — 요청 크기에 맞지 않는 주소였다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 언제 나는가: 장치가 8바이트 읽기를 8바이트 정렬되지 않은 주소로 보낸 경우
	 *   등이다. 대개 장치 드라이버의 버그이거나, 장치가 정렬을 지키지 않는
	 *   구성으로 설정된 경우다.
	 * 매핑을 고쳐도 해결되지 않는 종류의 폴트라, 소프트웨어가 재시도할 수 없다. */

	RISCV_IOMMU_FQ_CAUSE_RD_FAULT = 5,
	/* [한국어] 읽기 접근 폴트 — 변환은 됐지만 그 물리 주소를 읽을 수 없었다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 언제 나는가: 페이지 테이블이 존재하지 않는 물리 주소를 가리키거나, PMP/PMA
	 *   가 그 영역의 접근을 막고 있을 때.
	 * 위 INST_FAULT 와 같은 부류이며 접근 종류만 다르다. 매핑이 잘못된 것이
	 *   아니라 그 매핑이 가리키는 곳이 잘못된 것이라, 페이지를 채워 넣는 식으로는
	 *   해결되지 않는다. */

	RISCV_IOMMU_FQ_CAUSE_WR_ADDR_MISALIGNED = 6,
	/* [한국어] 쓰기 또는 원자적 연산의 주소 정렬 오류.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 위 RD_ADDR_MISALIGNED 의 쓰기 판이다. 원자적 연산이 함께 묶이는 이유는
	 *   RISC-V 에서 원자적 연산이 읽기-변경-쓰기라 쓰기 쪽으로 분류되기 때문이다.
	 * 원자적 연산은 정렬 요구가 더 엄격해, 보통의 쓰기는 통과하는 주소에서도
	 *   이 폴트가 날 수 있다. */

	RISCV_IOMMU_FQ_CAUSE_WR_FAULT = 7,
	/* [한국어] 쓰기 또는 원자적 연산의 접근 폴트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 위 RD_FAULT 의 쓰기 판이다. 변환 결과인 물리 주소에 쓸 수 없었다는 뜻으로,
	 *   읽기는 되지만 쓰기는 안 되는 영역(ROM, 읽기 전용으로 표시된 PMP 영역)을
	 *   가리키는 매핑에서 난다.
	 * 1~7 까지가 접근 폴트 계열이고, 12 부터가 페이지 폴트 계열이다. */

	RISCV_IOMMU_FQ_CAUSE_INST_FAULT_S = 12,
	/* [한국어] 1단계(S-stage) 명령어 페이지 폴트 — 페이지 테이블에
	 * 매핑이 없거나 X 권한이 없었다. */

	RISCV_IOMMU_FQ_CAUSE_RD_FAULT_S = 13,
	/* [한국어] 1단계 읽기 페이지 폴트 — 매핑이 없거나 R 권한이 없었다.
	 * 가장 흔히 보게 되는 원인이다. */

	RISCV_IOMMU_FQ_CAUSE_WR_FAULT_S = 15,
	/* [한국어] 1단계 쓰기 페이지 폴트 — 매핑이 없거나 W 권한이 없었다. */

	RISCV_IOMMU_FQ_CAUSE_INST_FAULT_VS = 20,
	/* [한국어] 2단계(게스트) 명령어 페이지 폴트 — 게스트 물리를 호스트
	 * 물리로 바꾸는 단계에서 실패했다. */

	RISCV_IOMMU_FQ_CAUSE_RD_FAULT_VS = 21,

	RISCV_IOMMU_FQ_CAUSE_WR_FAULT_VS = 23,
	/* [한국어] 2단계(VS-stage) 쓰기 페이지 폴트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기. 중첩 변환에서 이 값은 게스트가 아니라 호스트의
	 *   2단계 매핑에 문제가 있다는 뜻이다.
	 * 왜 단계를 구분하는가: 1단계 폴트(13/12)는 게스트가 자기 페이지 테이블을
	 *   고쳐 해결할 문제이고, 2단계 폴트는 호스트가 해결할 문제다. 구분이 없으면
	 *   누구에게 폴트를 전달해야 할지 알 수 없다.
	 * 값 범위: 23. 12~23 이 페이지 폴트 계열이며, RISC-V 특권 스펙의 예외 코드를
	 *   그대로 따른다. */

	RISCV_IOMMU_FQ_CAUSE_DMA_DISABLED = 256,
	/* [한국어] DDTP 모드가 OFF 라 모든 인바운드 트랜잭션이 차단됐다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 언제 나는가: IOMMU 를 아직 초기화하지 않았거나 일부러 꺼 둔 상태에서 장치가
	 *   DMA 를 시도한 경우. 부팅 중 펌웨어가 남긴 DMA 가 이어질 때 흔히 보인다.
	 * 256 부터가 IOMMU 고유의 원인이다. 1~23 이 RISC-V 예외 코드와 번호를 공유하는
	 *   것과 달리, 이쪽은 IOMMU 만의 실패라 겹치지 않는 번호대를 쓴다.
	 * 이 폴트가 계속 나면 IOMMU 초기화 순서를 의심해야 한다. */

	RISCV_IOMMU_FQ_CAUSE_DDT_LOAD_FAULT = 257,
	/* [한국어] DDT 엔트리를 메모리에서 읽는 데 실패했다 — DDTP 주소가
	 * 잘못됐거나 그 메모리에 접근할 수 없다. */

	RISCV_IOMMU_FQ_CAUSE_DDT_INVALID = 258,
	/* [한국어] DDT 엔트리의 V 비트가 0이다 — 등록되지 않은 device_id가
	 * DMA를 시도했다는 뜻이다. */

	RISCV_IOMMU_FQ_CAUSE_DDT_MISCONFIGURED = 259,
	/* [한국어] DDT 엔트리(DC)의 필드 조합이 유효하지 않다 — 예를 들어
	 * 지원하지 않는 MODE 값이 들어 있다. 드라이버 버그의 신호다. */

	RISCV_IOMMU_FQ_CAUSE_TTYP_BLOCKED = 260,
	/* [한국어] 그 종류의 트랜잭션이 이 디바이스에 허용되지 않는다
	 * (예: ATS가 꺼진 디바이스가 변환 요청을 보냈다). */

	RISCV_IOMMU_FQ_CAUSE_MSI_LOAD_FAULT = 261,

	RISCV_IOMMU_FQ_CAUSE_MSI_INVALID = 262,
	/* [한국어] MSI 페이지 테이블 엔트리의 V(유효) 비트가 0 이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 언제 나는가: 장치가 MSI 를 보냈는데 그 주소에 해당하는 MSI 테이블 항목을
	 *   드라이버가 아직 만들지 않았거나 이미 지운 경우.
	 * 위 MSI_LOAD_FAULT(261)와의 차이: 그쪽은 항목을 읽지도 못한 것이고, 이쪽은
	 *   읽었더니 비어 있던 것이다. 전자는 테이블 주소가 잘못됐다는 뜻이고
	 *   후자는 항목이 준비되지 않았다는 뜻이라 원인이 전혀 다르다. */

	RISCV_IOMMU_FQ_CAUSE_MSI_MISCONFIGURED = 263,
	/* [한국어] MSI 페이지 테이블 엔트리의 설정이 유효하지 않다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 언제 나는가: 항목의 V 비트는 서 있는데 나머지 필드의 조합이 스펙에 맞지
	 *   않는 경우 — 예약 비트가 서 있거나, 모드와 주소가 어긋난 경우다.
	 * 위 MSI_INVALID 와 나뉘어 있는 이유: 하나는 "아직 만들지 않았다"이고 다른
	 *   하나는 "잘못 만들었다"이다. 전자는 경쟁 상황일 수 있지만 후자는 드라이버
	 *   버그라, 진단할 때 구분이 중요하다. */

	RISCV_IOMMU_FQ_CAUSE_MRIF_FAULT = 264,
	/* [한국어] 메모리 상주 인터럽트 파일(MRIF)에 접근하지 못했다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * MRIF 가 무엇인가: 인터럽트를 CPU 에 바로 보내는 대신 메모리의 비트로
	 *   기록해 두는 방식이다. 가상화에서 목적지 vCPU 가 실행 중이 아닐 때 쓴다 —
	 *   AMD 의 GA 로그, ARM 의 vAPIC 백킹 페이지와 같은 발상이다.
	 * 언제 나는가: 그 메모리 영역에 접근할 수 없을 때. 게스트가 살아 있는 동안
	 *   그 페이지가 유효해야 하므로, 이 폴트는 대개 호스트 쪽 설정 문제다. */

	RISCV_IOMMU_FQ_CAUSE_PDT_LOAD_FAULT = 265,
	/* [한국어] PDT(프로세스 디렉토리 테이블) 엔트리를 읽는 데 실패했다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 언제 나는가: PASID 를 쓰는 요청이 왔는데 DC 의 fsc 가 가리키는 PDT 를
	 *   읽을 수 없을 때 — 주소가 잘못됐거나 그 메모리에 접근할 수 없다.
	 * DDT 와 PDT 의 관계: DDT 가 device_id 로 장치를 찾고, PDT 가 그 장치 안에서
	 *   process_id(PASID)로 주소 공간을 찾는다. 두 단계 모두 워크가 실패할 수
	 *   있어 각각의 LOAD_FAULT 코드가 있다(257 과 265). */

	RISCV_IOMMU_FQ_CAUSE_PDT_INVALID = 266,
	/* [한국어] PDT 엔트리(PC)의 V 비트가 0이다 — 등록되지 않은 PASID로
	 * 요청이 왔다는 뜻이다. */

	RISCV_IOMMU_FQ_CAUSE_PDT_MISCONFIGURED = 267,
	/* [한국어] PDT 엔트리의 설정이 유효하지 않다. */

	RISCV_IOMMU_FQ_CAUSE_DDT_CORRUPTED = 268,
	/* [한국어] DDT 데이터가 손상됐다(ECC 오류 등) — 소프트웨어 버그가
	 * 아니라 하드웨어/메모리 문제의 신호다. */

	RISCV_IOMMU_FQ_CAUSE_PDT_CORRUPTED = 269,

	RISCV_IOMMU_FQ_CAUSE_MSI_PT_CORRUPTED = 270,
	/* [한국어] MSI 페이지 테이블 데이터가 손상됐다.
	 * 설정자: 하드웨어가 ECC 오류 등으로 데이터 무결성 실패를 감지했을 때.
	 * 읽는 자: 폴트 처리기.
	 * 269~271 의 CORRUPTED 계열이 다른 원인과 근본적으로 다른 점: 이것들은
	 *   소프트웨어의 잘못이 아니라 메모리 자체가 망가졌다는 뜻이다. 매핑을
	 *   고치거나 재시도해서 해결되지 않으며, 하드웨어 고장을 알리는 신호다.
	 * 이 폴트를 보면 그 메모리 영역을 더 이상 믿을 수 없으므로, 로그를 남기는
	 *   것 외에 소프트웨어가 할 수 있는 일이 사실상 없다. */

	RISCV_IOMMU_FQ_CAUSE_MRIF_CORRUIPTED = 271,
	/* [한국어] MRIF 데이터가 손상됐다. 이름의 "CORRUIPTED"는 스펙 헤더의
	 * 오타가 그대로 옮겨진 것으로, 원본을 고치지 않는다. */

	RISCV_IOMMU_FQ_CAUSE_INTERNAL_DP_ERROR = 272,
	/* [한국어] IOMMU 내부 데이터 경로 오류 — 하드웨어 자체의 결함이다. */

	RISCV_IOMMU_FQ_CAUSE_MSI_WR_FAULT = 273,
	/* [한국어] IOMMU가 MSI를 쓰는 데 실패했다(자기 자신이 인터럽트를
	 * 보내려다 실패한 경우). */

	RISCV_IOMMU_FQ_CAUSE_PT_CORRUPTED = 274
	/* [한국어] 1단계 또는 2단계 페이지 테이블 데이터가 손상됐다. */
};

/**
 * enum riscv_iommu_fq_ttypes: Fault/event transaction types
 * @RISCV_IOMMU_FQ_TTYP_NONE: None. Fault not caused by an inbound transaction.
 * @RISCV_IOMMU_FQ_TTYP_UADDR_INST_FETCH: Instruction fetch from untranslated address
 * @RISCV_IOMMU_FQ_TTYP_UADDR_RD: Read from untranslated address
 * @RISCV_IOMMU_FQ_TTYP_UADDR_WR: Write/AMO to untranslated address
 * @RISCV_IOMMU_FQ_TTYP_TADDR_INST_FETCH: Instruction fetch from translated address
 * @RISCV_IOMMU_FQ_TTYP_TADDR_RD: Read from translated address
 * @RISCV_IOMMU_FQ_TTYP_TADDR_WR: Write/AMO to translated address
 * @RISCV_IOMMU_FQ_TTYP_PCIE_ATS_REQ: PCIe ATS translation request
 * @RISCV_IOMMU_FQ_TTYP_PCIE_MSG_REQ: PCIe message request
 *
 * Values are on table 12 of the spec, type 4 and 10 - 31 are reserved for standard use
 * and 31 - 63 for custom use.
 */
/* [한국어] 폴트를 일으킨 트랜잭션의 종류. 크게 세 갈래다 —
 * UADDR_*(미변환 주소로 온 일반 DMA), TADDR_*(ATS로 이미 변환을 받아
 * 물리 주소로 온 요청), 그리고 PCIe 프로토콜 요청.
 * 드라이버는 이 값으로 폴트를 읽기/쓰기/실행 중 무엇으로 상위에 보고할지
 * 결정한다 — MTK v1처럼 구분하지 못하는 하드웨어와 대비되는 지점이다. */
enum riscv_iommu_fq_ttypes {
	RISCV_IOMMU_FQ_TTYP_NONE = 0,
	/* [한국어] 인바운드 트랜잭션 때문이 아닌 폴트 — 내부 오류 등.
	 * 설정자: 하드웨어가 폴트 큐 항목의 TTYP 필드에 넣는다.
	 * 읽는 자: riscv_iommu_fault() 가 폴트를 읽기/쓰기/실행 중 무엇으로 상위에
	 *   보고할지 정할 때.
	 * 이 값일 때는 보고할 접근 종류가 없다. 장치의 요청과 무관하게 IOMMU 자신이
	 *   낸 오류(자체 진단, 설정 오류)이므로 특정 DMA 에 귀속시킬 수 없다.
	 * 0 인 이유: 필드를 0 으로 남긴 항목이 "해당 없음"으로 해석되게 하기 위해서다. */

	RISCV_IOMMU_FQ_TTYP_UADDR_INST_FETCH = 1,
	/* [한국어] 미변환 주소로 온 명령어 인출.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기가 IOMMU_FAULT_PERM_EXEC 로 보고할지 정할 때.
	 * "미변환(untranslated)" 이란: 장치가 ATS 를 쓰지 않고 IOVA 를 그대로 보낸
	 *   경우다. 보통의 DMA 가 모두 여기 해당한다.
	 * 명령어 인출이 IOMMU 에 오는 경우: 장치가 CPU 처럼 메모리에서 명령을 읽어
	 *   실행하는 구조일 때다. 보통의 DMA 장치에서는 거의 볼 수 없다. */

	RISCV_IOMMU_FQ_TTYP_UADDR_RD = 2,
	/* [한국어] 미변환 주소로 온 읽기 — 가장 흔한 DMA 읽기다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기가 IOMMU_FAULT_PERM_READ 로 보고할 때.
	 * 이 값과 아래 UADDR_WR 이 실제 시스템에서 압도적으로 많이 보인다. 매핑을
	 *   만들지 않았거나 이미 해제한 IOVA 로 장치가 DMA 를 보내면 여기 걸린다.
	 * 이렇게 종류를 구분해 주는 것이 이 하드웨어의 장점이다 — 구분하지 못하는
	 *   하드웨어에서는 폴트가 읽기였는지 쓰기였는지 알 수 없어 진단이 어렵다. */

	RISCV_IOMMU_FQ_TTYP_UADDR_WR = 3,
	/* [한국어] 미변환 주소로 온 쓰기 또는 원자적 연산.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기가 IOMMU_FAULT_PERM_WRITE 로 보고할 때.
	 * 읽기보다 위험한 이유: 실패한 쓰기는 데이터를 잃는 것으로 끝나지만, 만약
	 *   변환이 잘못된 주소로 성공했다면 엉뚱한 메모리를 덮어썼을 것이다. 그래서
	 *   쓰기 폴트는 격리가 실제로 작동하고 있다는 증거이기도 하다.
	 * 원자적 연산이 함께 묶이는 것은 위 WR_FAULT 와 같은 이유다. */

	RISCV_IOMMU_FQ_TTYP_TADDR_INST_FETCH = 5,
	/* [한국어] 이미 변환된 주소로 온 명령어 인출(ATS 사용 디바이스).
	 * 값 4가 건너뛰어진 것은 스펙이 예약해 두었기 때문이다. */

	RISCV_IOMMU_FQ_TTYP_TADDR_RD = 6,
	/* [한국어] 이미 변환된 주소로 온 읽기. 이 경우에도 폴트가 날 수 있는
	 * 이유는, 소프트웨어가 매핑을 해제했는데 디바이스의 ATC에 옛 변환이
	 * 남아 있을 수 있기 때문이다. */

	RISCV_IOMMU_FQ_TTYP_TADDR_WR = 7,

	RISCV_IOMMU_FQ_TTYP_PCIE_ATS_REQ = 8,
	/* [한국어] PCIe ATS 변환 요청 자체가 실패했다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 무슨 상황인가: 장치가 "이 IOVA 의 물리 주소를 알려 달라"고 ATS 로 물었는데
	 *   IOMMU 가 답할 수 없었던 경우다. 실제 데이터 접근이 아니라 변환 조회가
	 *   실패한 것이라, 위 UADDR/TADDR 계열과 성격이 다르다.
	 * 이 경우 장치는 변환을 받지 못해 그 DMA 를 시작조차 하지 않는다 — 데이터가
	 *   잘못 흐를 위험이 없다는 점에서 오히려 안전한 실패다. */

	RISCV_IOMMU_FQ_TTYP_PCIE_MSG_REQ = 9,
	/* [한국어] PCIe 메시지 요청이 실패했다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 폴트 처리기.
	 * 어떤 요청인가: PCIe 의 메시지 트랜잭션(PRI 페이지 요청, 무효화 완료 통지 등)
	 *   으로, 메모리 읽기/쓰기가 아닌 제어 성격의 패킷이다.
	 * 이 enum 의 마지막 값이며, 읽기/쓰기/실행 중 어느 것으로도 보고할 수 없는
	 *   종류다 — 위 TTYP_NONE 과 마찬가지로 접근 권한과 무관한 실패다. */
};

/**
 * struct riscv_iommu_pq_record - PCIe Page Request record
 * @hdr: Header, includes PID, DID etc
 * @payload: Holds the page address, request group and permission bits
 *
 * For more infos on the PCIe Page Request queue see chapter 3.3.
 */
/* [한국어] 페이지 요청 큐에 하드웨어가 써 넣는 기록 하나(16바이트).
 * PCIe PRI를 지원하는 디바이스가 "이 주소를 매핑해 달라"고 보낸 요청이다.
 * SVA에서 디바이스가 아직 매핑되지 않은 사용자 메모리에 접근할 때 발생하며,
 * 드라이버는 이것을 받아 페이지를 확보한 뒤 ATS.PRGR 명령으로 응답한다.
 * 설정자: IOMMU 하드웨어. 읽는 자: 드라이버의 페이지 요청 처리 경로. */
struct riscv_iommu_pq_record {
	u64 hdr;
	/* [한국어] 요청의 머리 — process_id(12~31), PID 유효(32),
	 * 특권 접근 여부(33), 실행 접근 여부(34), device_id(40~63).
	 * 어느 디바이스의 어느 PASID가 요청했는지 여기서 알 수 있다.
	 * 읽는 자: 드라이버가 대응하는 mm과 페이지 테이블을 찾는 근거. */

	u64 payload;
	/* [한국어] 요청 내용 — 요구 권한(R/W/L 비트), 페이지 요청 그룹
	 * 인덱스(3~11), 그리고 요청 주소의 페이지 부분(12~63).
	 * 그룹 인덱스는 응답을 보낼 때 그대로 되돌려 줘야 짝이 맞는다.
	 * 읽는 자: 드라이버가 어떤 주소에 어떤 권한을 매핑할지 결정한다. */
};

/* Header fields */
/* [한국어] 요청을 보낸 process_id(비트 12~31). */
#define RISCV_IOMMU_PQ_HDR_PID		GENMASK_ULL(31, 12)
/* [한국어] 위 PID가 유효한지 표시하는 비트. */
#define RISCV_IOMMU_PQ_HDR_PV		BIT_ULL(32)
/* [한국어] 특권(supervisor) 접근으로 요청했는지 표시하는 비트. */
#define RISCV_IOMMU_PQ_HDR_PRIV		BIT_ULL(33)
/* [한국어] 실행(instruction fetch) 접근으로 요청했는지 표시하는 비트. */
#define RISCV_IOMMU_PQ_HDR_EXEC		BIT_ULL(34)
/* [한국어] 요청을 보낸 device_id(비트 40~63). */
#define RISCV_IOMMU_PQ_HDR_DID		GENMASK_ULL(63, 40)

/* Payload fields */
/* [한국어] 읽기 권한 요청 비트. */
#define RISCV_IOMMU_PQ_PAYLOAD_R	BIT_ULL(0)
/* [한국어] 쓰기 권한 요청 비트. */
#define RISCV_IOMMU_PQ_PAYLOAD_W	BIT_ULL(1)
/* [한국어] Last 비트 — 이 요청이 그룹의 마지막임을 표시한다.
 * 그룹의 마지막 요청에 응답해야 디바이스가 재개된다. */
#define RISCV_IOMMU_PQ_PAYLOAD_L	BIT_ULL(2)
/* [한국어] 위 R/W/L 세 비트를 한 번에 다루는 마스크. R과 W가 모두 0이면
 * "정지(stop marker)" 요청이라는 특수한 의미가 되므로, 세 비트를 묶어
 * 판별하는 것이 편하다. */
#define RISCV_IOMMU_PQ_PAYLOAD_RWL_MASK	GENMASK_ULL(2, 0)
/* [한국어] 페이지 요청 그룹 인덱스(비트 3~11). 하나의 논리적 작업에 속한
 * 여러 요청을 묶는 식별자로, 응답 시 그대로 돌려줘야 한다. */
#define RISCV_IOMMU_PQ_PAYLOAD_PRGI	GENMASK_ULL(11, 3) /* Page Request Group Index */
/* [한국어] 요청된 주소의 페이지 부분(비트 12~63). */
#define RISCV_IOMMU_PQ_PAYLOAD_ADDR	GENMASK_ULL(63, 12)

/**
 * struct riscv_iommu_msipte - MSI Page Table Entry
 * @pte: MSI PTE
 * @mrif_info: Memory-resident interrupt file info
 *
 * The MSI Page Table is used for virtualizing MSIs, so that when
 * a device sends an MSI to a guest, the IOMMU can reroute it
 * by translating the MSI address, either to a guest interrupt file
 * or a memory resident interrupt file (MRIF). Note that this page table
 * is an array of MSI PTEs, not a multi-level pt, each entry
 * is a leaf entry. For more infos check out the AIA spec, chapter 9.5.
 *
 * Also in basic mode the mrif_info field is ignored by the IOMMU and can
 * be used by software, any other reserved fields on pte must be zeroed-out
 * by software.
 */
/* [한국어] MSI 페이지 테이블의 엔트리(16바이트).
 * 이 테이블은 다단계가 아니라 평면 배열이고 모든 엔트리가 리프다 —
 * DC의 msi_addr_mask/pattern으로 MSI를 감지한 뒤, 주소의 일부를 인덱스로
 * 삼아 이 배열을 직접 인덱싱한다.
 * 목적: 가상화 환경에서 디바이스가 보낸 MSI를 게스트의 인터럽트 파일로
 *       돌려보내는 것. 게스트가 직접 디바이스를 다루면서도 인터럽트는
 *       하이퍼바이저가 통제할 수 있게 하는 장치다.
 * 두 가지 모드가 있다 — 기본(basic) 모드는 게스트 인터럽트 파일로 그냥
 * 리다이렉트하고, MRIF 모드는 메모리에 상주하는 인터럽트 파일에 기록한다.
 * 자세한 내용은 RISC-V AIA 스펙 9.5절에 있다. */
struct riscv_iommu_msipte {
	u64 pte;
	/* [한국어] 엔트리 본체 — 유효 비트(0), 모드(1~2), 그리고 모드에 따라
	 * MRIF 주소(7~53) 또는 목적지 PPN(10~53), 마지막으로 C 비트(63).
	 * 모드 값이 필드 해석을 바꾸는 구조라, 마스크가 겹쳐 정의되어 있다.
	 * 설정자: 하이퍼바이저/드라이버가 게스트의 인터럽트 파일 위치를 넣는다.
	 * 읽는 자: 하드웨어가 MSI 쓰기를 감지했을 때. */

	u64 mrif_info;
	/* [한국어] MRIF 모드에서만 의미가 있는 부가 정보 — 알림 인터럽트
	 * 식별자(NID)와 그 알림을 보낼 주소(NPPN).
	 * 기본 모드에서는 하드웨어가 이 필드를 무시하므로, 원본 주석이
	 * 밝히듯 소프트웨어가 자기 용도로 쓸 수 있다(예: 참조 카운트나
	 * 역참조 포인터를 숨겨 두는 식).
	 * 설정자/읽는 자: MRIF 모드에서는 하드웨어, 기본 모드에서는 소프트웨어. */
};

/* Fields on pte */
/* [한국어] MSI PTE의 유효 비트. 0이면 그 MSI는 폴트(MSI_INVALID)가 된다. */
#define RISCV_IOMMU_MSIPTE_V		BIT_ULL(0)
/* [한국어] 모드 필드(비트 1~2) — 1이면 MRIF 모드, 3이면 기본(basic) 모드다.
 * 이 값에 따라 아래 두 주소 필드 중 어느 쪽이 유효한지 갈린다. */
#define RISCV_IOMMU_MSIPTE_M		GENMASK_ULL(2, 1)
/* [한국어] MRIF 모드(M == 1)일 때 유효한 MRIF 주소 필드(비트 7~53).
 * 기본 모드의 PPN 필드와 자리가 겹치므로, 모드를 보고 해석해야 한다. */
#define RISCV_IOMMU_MSIPTE_MRIF_ADDR	GENMASK_ULL(53, 7)	/* When M == 1 (MRIF mode) */
/* [한국어] 기본 모드(M == 3)일 때 유효한 목적지 물리 페이지 번호.
 * 게스트의 인터럽트 파일이 있는 페이지를 가리킨다. */
#define RISCV_IOMMU_MSIPTE_PPN		RISCV_IOMMU_PPN_FIELD	/* When M == 3 (basic mode) */
/* [한국어] C 비트(최상위) — 사용자 정의/커스텀 용도로 예약된 비트다. */
#define RISCV_IOMMU_MSIPTE_C		BIT_ULL(63)

/* Fields on mrif_info */
/* [한국어] MRIF 모드에서 보낼 알림 인터럽트의 식별자 하위 10비트(NID). */
#define RISCV_IOMMU_MSIPTE_MRIF_NID	GENMASK_ULL(9, 0)
/* [한국어] 알림 인터럽트를 보낼 대상 페이지 번호(NPPN).
 * MRIF에 기록한 뒤 실제 인터럽트를 어디로 보낼지 지정한다. */
#define RISCV_IOMMU_MSIPTE_MRIF_NPPN	RISCV_IOMMU_PPN_FIELD
/* [한국어] NID의 최상위 비트(비트 60). 10비트 하위와 합쳐 11비트 식별자를
 * 만드는데, 필드가 떨어져 있는 것은 PPN 필드가 사이를 차지하기 때문이다. */
#define RISCV_IOMMU_MSIPTE_MRIF_NID_MSB	BIT_ULL(60)

/* Helper functions: command structure builders. */

/*
 * [한국어]
 * riscv_iommu_cmd_inval_vma - 1단계 IOTLB 무효화 명령의 기본 형태를 만든다
 *
 * @cmd: 채울 커맨드 구조체(호출자의 스택에 있는 것이 보통이다).
 * @return: 없음.
 *
 * 왜 빌더로 나누어 두는가: 무효화 명령은 "무엇을 얼마나 좁게 비울지"에 따라
 * 필드 조합이 달라진다. 기본 형태를 만드는 함수 하나와, 범위를 좁히는
 * set_* 함수 여러 개로 나누면 호출부가 필요한 것만 골라 조합할 수 있다.
 * 예: 특정 주소만 → cmd_inval_vma() + cmd_inval_set_addr()
 *     특정 주소공간 전체 → cmd_inval_vma() + cmd_inval_set_pscid()
 *     전부 → cmd_inval_vma()만
 * 아무것도 덧붙이지 않으면 AV/PSCV/GV가 모두 0이라 "전체 무효화"가 된다.
 *
 * 실행 컨텍스트: 무효화 경로. atomic 컨텍스트일 수 있으나 메모리 쓰기뿐이라
 * 잠들지 않는다.
 *
 * 호출 체인:
 *   riscv/iommu.c의 iotlb_sync/flush 경로 → [riscv_iommu_cmd_inval_vma]
 *   → (선택적으로) set_addr/set_pscid/set_gscid → 커맨드 큐에 삽입
 */
static inline void riscv_iommu_cmd_inval_vma(struct riscv_iommu_command *cmd)
{
	/* [한국어] opcode에 IOTINVAL(1)을, func에 VMA(0)를 넣어 "1단계 변환
	 * 캐시를 무효화하라"는 명령의 뼈대를 만든다. 나머지 필드는 0이므로
	 * 이 상태로 보내면 모든 주소공간의 모든 주소가 대상이 된다. */
	cmd->dword0 = FIELD_PREP(RISCV_IOMMU_CMD_OPCODE, RISCV_IOMMU_CMD_IOTINVAL_OPCODE) |
		      FIELD_PREP(RISCV_IOMMU_CMD_FUNC, RISCV_IOMMU_CMD_IOTINVAL_FUNC_VMA);
	/* [한국어] 주소 필드를 비워 둔다. set_addr가 나중에 채울 수 있다. */
	cmd->dword1 = 0;
}

/*
 * [한국어]
 * riscv_iommu_cmd_inval_set_addr - 무효화 명령에 대상 주소를 덧붙인다
 *
 * @cmd: 이미 cmd_inval_vma() 등으로 기본 형태가 만들어진 커맨드.
 * @addr: 무효화할 페이지의 물리/가상 주소(호출부가 IOVA를 넘긴다).
 * @return: 없음.
 *
 * 왜 AV 비트를 함께 세우는가: 주소 필드만 채우고 AV를 세우지 않으면
 * 하드웨어가 그 값을 무시하고 전체를 무효화한다. 주소와 AV는 반드시
 * 짝으로 설정해야 하므로, 실수를 막으려 한 함수 안에서 둘 다 처리한다.
 *
 * PHYS_PFN()을 쓰는 이유: 이 필드는 주소가 아니라 페이지 번호를 담는다.
 * 12비트 시프트를 직접 쓰지 않고 매크로를 쓰는 편이 의도가 분명하다.
 *
 * 실행 컨텍스트: 무효화 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   riscv/iommu.c의 범위 무효화 → [riscv_iommu_cmd_inval_set_addr]
 */
static inline void riscv_iommu_cmd_inval_set_addr(struct riscv_iommu_command *cmd,
						  u64 addr)
{
	/* [한국어] 주소를 페이지 번호로 바꿔 dword1의 ADDR 필드에 넣는다.
	 * dword1은 이 명령에서 주소 전용이라 통째로 대입해도 안전하다. */
	cmd->dword1 =
		FIELD_PREP(RISCV_IOMMU_CMD_IOTINVAL_ADDR, PHYS_PFN(addr));
	/* [한국어] AV(Address Valid) 비트를 세워 하드웨어가 위 주소를 실제로
	 * 쓰게 한다. 이 비트가 없으면 주소를 채워도 전체 무효화가 된다. */
	cmd->dword0 |= RISCV_IOMMU_CMD_IOTINVAL_AV;
}

/*
 * [한국어]
 * riscv_iommu_cmd_inval_set_pscid - 무효화 범위를 특정 주소공간으로 좁힌다
 *
 * @cmd: 기본 형태가 만들어진 무효화 커맨드.
 * @pscid: 대상 주소공간 식별자(도메인마다 하나씩 할당된다).
 * @return: 없음.
 *
 * 왜 필요한가: PSCID를 지정하지 않으면 이 IOMMU의 모든 주소공간 캐시가
 * 비워져, 무관한 디바이스들의 성능까지 떨어진다. 도메인 단위 무효화에서
 * 반드시 이 함수로 범위를 좁혀야 한다.
 *
 * set_addr와 마찬가지로 값과 유효 비트(PSCV)를 한 번에 세운다.
 *
 * 실행 컨텍스트: 무효화 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   riscv/iommu.c의 도메인 무효화 → [riscv_iommu_cmd_inval_set_pscid]
 */
static inline void riscv_iommu_cmd_inval_set_pscid(struct riscv_iommu_command *cmd,
						   int pscid)
{
	/* [한국어] PSCID 값과 그 유효 비트(PSCV)를 함께 OR 한다. dword0에는
	 * 이미 opcode/func가 들어 있으므로 대입이 아니라 OR를 써야 한다. */
	cmd->dword0 |= FIELD_PREP(RISCV_IOMMU_CMD_IOTINVAL_PSCID, pscid) |
		       RISCV_IOMMU_CMD_IOTINVAL_PSCV;
}

/*
 * [한국어]
 * riscv_iommu_cmd_inval_set_gscid - 무효화 범위를 특정 게스트로 좁힌다
 *
 * @cmd: 기본 형태가 만들어진 무효화 커맨드.
 * @gscid: 대상 게스트 문맥 식별자.
 * @return: 없음.
 *
 * 왜 필요한가: 가상화 환경에서 한 게스트의 2단계 매핑을 바꿨을 때,
 * 다른 게스트의 캐시까지 비울 이유가 없다. GSCID로 범위를 좁히면
 * 게스트 간 간섭을 막을 수 있다.
 * 보통 IOTINVAL.GVMA(func = 1)와 함께 쓰이지만, 중첩 변환에서는
 * VMA 명령에 GSCID를 붙여 "이 게스트의 이 주소공간"으로 좁히기도 한다.
 *
 * 실행 컨텍스트: 무효화 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   riscv/iommu.c의 게스트 무효화 → [riscv_iommu_cmd_inval_set_gscid]
 */
static inline void riscv_iommu_cmd_inval_set_gscid(struct riscv_iommu_command *cmd,
						   int gscid)
{
	/* [한국어] GSCID 값과 그 유효 비트(GV)를 함께 세운다. */
	cmd->dword0 |= FIELD_PREP(RISCV_IOMMU_CMD_IOTINVAL_GSCID, gscid) |
		       RISCV_IOMMU_CMD_IOTINVAL_GV;
}

/*
 * [한국어]
 * riscv_iommu_cmd_iofence - 커맨드 큐 펜스 명령을 만든다
 *
 * @cmd: 채울 커맨드 구조체.
 * @return: 없음.
 *
 * 왜 필요한가: 무효화 명령을 큐에 넣는 것만으로는 그것이 완료됐음을
 * 알 수 없다. 펜스를 뒤에 넣고 그 완료를 기다려야 비로소 "앞선 무효화가
 * 모두 반영됐다"고 확신할 수 있다. unmap 후 페이지를 재사용하기 전에
 * 반드시 거쳐야 하는 관문이다.
 *
 * PR|PW를 기본으로 세우는 이유: 앞선 읽기와 쓰기가 모두 완료되기를
 * 기다리게 한다. 하나만 세우면 진행 중인 반대 방향 트랜잭션이 남아
 * 이미 해제된 페이지를 건드릴 수 있다.
 *
 * 이 기본 형태는 완료를 알리는 수단(AV/WSI)이 없으므로, 소프트웨어는
 * CQH가 이 명령을 지나갔는지 폴링해 완료를 감지한다.
 *
 * 실행 컨텍스트: 무효화 경로의 마지막 단계.
 *
 * 호출 체인:
 *   riscv/iommu.c의 iotlb_sync → [riscv_iommu_cmd_iofence] → 큐 삽입 후 대기
 */
static inline void riscv_iommu_cmd_iofence(struct riscv_iommu_command *cmd)
{
	/* [한국어] opcode에 IOFENCE(2), func에 C(0)를 넣고, 앞선 읽기(PR)와
	 * 쓰기(PW)가 모두 완료되기를 기다리라는 비트를 함께 세운다. */
	cmd->dword0 = FIELD_PREP(RISCV_IOMMU_CMD_OPCODE, RISCV_IOMMU_CMD_IOFENCE_OPCODE) |
		      FIELD_PREP(RISCV_IOMMU_CMD_FUNC, RISCV_IOMMU_CMD_IOFENCE_FUNC_C) |
		      RISCV_IOMMU_CMD_IOFENCE_PR | RISCV_IOMMU_CMD_IOFENCE_PW;
	/* [한국어] 완료 통지 주소를 쓰지 않으므로 0으로 둔다. */
	cmd->dword1 = 0;
}

/*
 * [한국어]
 * riscv_iommu_cmd_iofence_set_av - 완료를 메모리 쓰기로 알리는 펜스를 만든다
 *
 * @cmd: 채울 커맨드 구조체.
 * @addr: 완료 시 하드웨어가 값을 쓸 메모리 주소(4바이트 정렬).
 * @data: 그 주소에 쓸 32비트 표식 값.
 * @return: 없음.
 *
 * 왜 별도 함수인가: 이름은 "set_"으로 시작하지만 위 iofence()에 덧붙이는
 * 것이 아니라 dword0을 통째로 다시 만든다. 그래서 iofence()를 먼저 부를
 * 필요가 없고, 불렀더라도 PR|PW 비트가 덮여 사라진다는 점에 주의해야 한다.
 *
 * 어떻게 완료를 감지하는가: 소프트웨어가 미리 그 메모리를 다른 값으로
 * 채워 두고, 펜스를 큐에 넣은 뒤 그 자리가 @data로 바뀌기를 폴링한다.
 * CQH 레지스터를 MMIO로 반복해서 읽는 것보다 훨씬 싸다 — 캐시에 있는
 * 메모리를 읽는 것이기 때문이다.
 *
 * 주소를 2비트 오른쪽으로 시프트하는 이유: 스펙이 dword1을 워드 단위
 * 주소로 정의해, 하위 2비트(항상 0)를 빼고 저장하기 때문이다.
 *
 * 실행 컨텍스트: 무효화 완료를 기다려야 하는 경로.
 *
 * 호출 체인:
 *   riscv/iommu.c의 동기 무효화 → [riscv_iommu_cmd_iofence_set_av]
 *   → 큐 삽입 후 메모리 폴링
 */
static inline void riscv_iommu_cmd_iofence_set_av(struct riscv_iommu_command *cmd,
						  u64 addr, u32 data)
{
	/* [한국어] 펜스 명령을 다시 조립하면서 완료 시 쓸 데이터와 AV 비트를
	 * 넣는다. PR/PW를 세우지 않는 점에 주목 — 이 형태는 "완료 통지"에
	 * 초점이 있고, 순서 보장은 큐의 순차 처리에 맡긴다. */
	cmd->dword0 = FIELD_PREP(RISCV_IOMMU_CMD_OPCODE, RISCV_IOMMU_CMD_IOFENCE_OPCODE) |
		      FIELD_PREP(RISCV_IOMMU_CMD_FUNC, RISCV_IOMMU_CMD_IOFENCE_FUNC_C) |
		      FIELD_PREP(RISCV_IOMMU_CMD_IOFENCE_DATA, data) |
		      RISCV_IOMMU_CMD_IOFENCE_AV;
	/* [한국어] 완료 통지를 쓸 주소를 워드 단위로 바꿔 넣는다.
	 * 스펙이 dword1 전체를 이 주소로 정의하므로 FIELD_PREP이 필요 없다. */
	cmd->dword1 = addr >> 2;
}

/*
 * [한국어]
 * riscv_iommu_cmd_iodir_inval_ddt - 디바이스 컨텍스트 캐시 무효화 명령을 만든다
 *
 * @cmd: 채울 커맨드 구조체.
 * @return: 없음.
 *
 * 왜 필요한가: 하드웨어는 DDT를 걸어 얻은 DC를 내부 캐시에 보관한다.
 * 소프트웨어가 DC를 고쳐도 이 명령을 보내지 않으면 하드웨어는 옛 설정으로
 * 계속 동작한다 — attach/detach 후 반드시 보내야 하는 명령이다.
 *
 * 이 기본 형태는 DID를 지정하지 않으므로 모든 디바이스의 캐시가 대상이다.
 * 특정 디바이스만 비우려면 cmd_iodir_set_did()를 이어 붙인다.
 *
 * 실행 컨텍스트: attach/detach 경로.
 *
 * 호출 체인:
 *   riscv/iommu.c의 DC 갱신 후 → [riscv_iommu_cmd_iodir_inval_ddt]
 *   → (선택적으로) set_did → 큐 삽입
 */
static inline void riscv_iommu_cmd_iodir_inval_ddt(struct riscv_iommu_command *cmd)
{
	/* [한국어] opcode에 IODIR(3), func에 INVAL_DDT(0)를 넣는다.
	 * DV 비트가 없으므로 이 상태로는 모든 디바이스가 대상이다. */
	cmd->dword0 = FIELD_PREP(RISCV_IOMMU_CMD_OPCODE, RISCV_IOMMU_CMD_IODIR_OPCODE) |
		      FIELD_PREP(RISCV_IOMMU_CMD_FUNC, RISCV_IOMMU_CMD_IODIR_FUNC_INVAL_DDT);
	/* [한국어] 이 명령의 dword1은 스펙상 예약이므로 0으로 둔다. */
	cmd->dword1 = 0;
}

/*
 * [한국어]
 * riscv_iommu_cmd_iodir_inval_pdt - 프로세스 컨텍스트 캐시 무효화 명령을 만든다
 *
 * @cmd: 채울 커맨드 구조체.
 * @return: 없음.
 *
 * 왜 필요한가: DDT 캐시와 마찬가지로 PDT에서 얻은 PC도 캐시된다.
 * SVA에서 PASID를 붙이거나 떼어낸 뒤(즉 PC를 고친 뒤) 이 명령을 보내야
 * 하드웨어가 새 프로세스 컨텍스트를 읽는다.
 *
 * 보통 set_did()와 set_pid()를 이어 붙여 "이 디바이스의 이 PASID"로
 * 범위를 좁혀 쓴다.
 *
 * 실행 컨텍스트: SVA attach/detach 경로.
 *
 * 호출 체인:
 *   riscv/iommu.c의 PC 갱신 후 → [riscv_iommu_cmd_iodir_inval_pdt]
 *   → set_did/set_pid → 큐 삽입
 */
static inline void riscv_iommu_cmd_iodir_inval_pdt(struct riscv_iommu_command *cmd)
{
	/* [한국어] opcode에 IODIR(3), func에 INVAL_PDT(1)를 넣는다. */
	cmd->dword0 = FIELD_PREP(RISCV_IOMMU_CMD_OPCODE, RISCV_IOMMU_CMD_IODIR_OPCODE) |
		      FIELD_PREP(RISCV_IOMMU_CMD_FUNC, RISCV_IOMMU_CMD_IODIR_FUNC_INVAL_PDT);
	/* [한국어] dword1은 예약이므로 0으로 둔다. */
	cmd->dword1 = 0;
}

/*
 * [한국어]
 * riscv_iommu_cmd_iodir_set_did - 디렉토리 무효화 대상을 한 디바이스로 좁힌다
 *
 * @cmd: 기본 형태가 만들어진 IODIR 커맨드.
 * @devid: 대상 device_id.
 * @return: 없음.
 *
 * 왜 필요한가: 지정하지 않으면 모든 디바이스의 디렉토리 캐시가 비워져,
 * 무관한 디바이스들이 다음 DMA에서 DDT를 다시 걸어야 한다. 한 디바이스의
 * DC만 고쳤다면 그 디바이스만 지정하는 것이 옳다.
 *
 * 값과 유효 비트(DV)를 함께 세우는 패턴은 무효화 빌더들과 동일하다.
 *
 * 실행 컨텍스트: attach/detach 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   riscv/iommu.c → [riscv_iommu_cmd_iodir_set_did]
 */
static inline void riscv_iommu_cmd_iodir_set_did(struct riscv_iommu_command *cmd,
						 unsigned int devid)
{
	/* [한국어] device_id와 그 유효 비트(DV)를 함께 OR 한다. */
	cmd->dword0 |= FIELD_PREP(RISCV_IOMMU_CMD_IODIR_DID, devid) |
		       RISCV_IOMMU_CMD_IODIR_DV;
}

/*
 * [한국어]
 * riscv_iommu_cmd_iodir_set_pid - 디렉토리 무효화 대상을 한 PASID로 좁힌다
 *
 * @cmd: 기본 형태가 만들어진 IODIR 커맨드(보통 INVAL_PDT).
 * @pasid: 대상 process_id.
 * @return: 없음.
 *
 * 다른 set_* 함수들과 달리 유효 비트를 함께 세우지 않는 점에 주목:
 * IODIR 명령에는 PID 유효 비트가 정의되어 있지 않다. 대신 func가
 * INVAL_PDT인지 여부가 PID 사용 여부를 결정하므로, 값만 넣으면 된다.
 * (INVAL_DDT에 PID를 넣는 것은 의미가 없다.)
 *
 * 실행 컨텍스트: SVA attach/detach 경로. 순수 비트 조작이다.
 *
 * 호출 체인:
 *   riscv/iommu.c → [riscv_iommu_cmd_iodir_set_pid]
 */
static inline void riscv_iommu_cmd_iodir_set_pid(struct riscv_iommu_command *cmd,
						 unsigned int pasid)
{
	/* [한국어] process_id를 PID 필드에 넣는다. 별도의 유효 비트가 없어
	 * 값만 채우면 된다 — func가 INVAL_PDT라는 사실이 곧 "PID를 쓰겠다"는
	 * 신호이기 때문이다. */
	cmd->dword0 |= FIELD_PREP(RISCV_IOMMU_CMD_IODIR_PID, pasid);
}

/* [한국어] _RISCV_IOMMU_BITS_H_ 인클루드 가드의 끝. */
#endif /* _RISCV_IOMMU_BITS_H_ */
