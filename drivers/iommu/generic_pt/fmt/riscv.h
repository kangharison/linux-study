/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES
 *
 * RISC-V page table
 *
 * This is described in Sections:
 *  12.3. Sv32: Page-Based 32-bit Virtual-Memory Systems
 *  12.4. Sv39: Page-Based 39-bit Virtual-Memory System
 *  12.5. Sv48: Page-Based 48-bit Virtual-Memory System
 *  12.6. Sv57: Page-Based 57-bit Virtual-Memory System
 * of the "The RISC-V Instruction Set Manual: Volume II"
 *
 * This includes the contiguous page extension from:
 *  Chapter 13. "Svnapot" Extension for NAPOT Translation Contiguity,
 *     Version 1.0
 *
 * The table format is sign extended and supports leafs in every level. The spec
 * doesn't talk a lot about levels, but level here is the same as i=LEVELS-1 in
 * the spec.
 */
/*
 * [한국어 설명] RISC-V Sv 계열 페이지 테이블 형식 구현 (riscv.h)
 *
 * === 파일의 역할 ===
 * RISC-V 의 Sv32/39/48/57 페이지 테이블을 generic_pt 의 형식 API 로 감싼다.
 * 출처는 원 주석이 든 RISC-V 명세 12.3~12.6 절과, 연속 페이지를 다루는
 * Svnapot 확장(13장)이다.
 *
 * 이 형식이 다른 둘과 크게 다른 점이 셋 있다.
 *
 * 첫째, 모든 단계에 잎을 놓을 수 있다. x86 계열이 3단계까지만 큰 페이지를
 * 허용하는 것과 달리, RISC-V 는 최상위에도 잎을 둘 수 있다.
 *
 * 둘째, 표와 잎을 권한 비트로 가른다. 유효 비트가 서 있는데 R/W/X 가
 * 하나도 없으면 그것이 아래 표를 가리킨다는 뜻이다 — 별도의 "페이지 크기"
 * 비트가 없다.
 *
 * 셋째, 연속 페이지(Svnapot)의 표현이 독특하다. 16개의 인접 항목을 같은
 * 값으로 채우고 N 비트를 세우면 64KB 하나로 인식되는데, 그 값의 페이지
 * 번호 하위에 크기 부호가 들어간다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fmt/iommu_riscv64.c → iommu_template.h → defs_riscv.h → pt_defs.h →
 *   [이 파일] → pt_common.h → iommu_pt.h → drivers/iommu/riscv
 *
 * 32비트 변종은 PT_RISCV_32BIT 로 갈린다 — 항목 폭과 최대 단계가 달라진다.
 *
 * 실행 컨텍스트: 전부 인라인.
 *
 * === 타 모듈과의 연결 ===
 * 위: pt_common.h 의 API 선언, iommu_pt.h 의 매핑 경로.
 * 아래: <linux/bitfield.h>, <linux/sizes.h>(SZ_64K).
 * fmt_hw_info 가 넘기는 ppn 과 모드 코드를 drivers/iommu/riscv 가
 * 장치 컨텍스트의 fsc 필드에 적는다.
 *
 * === 주요 함수/구조체 요약 ===
 * riscvpt_load_entry_raw: 유효 비트로 존재를, R/W/X 유무로 잎 여부를 가른다.
 * riscvpt_entry_oa / riscvpt_entry_num_contig_lg2: Svnapot 항목이면 넓은
 *   페이지 번호 필드를 쓰고 묶음 크기를 16 으로 답한다.
 * riscvpt_install_leaf_entry: 연속 페이지면 16 자리를 같은 값으로 채운다.
 * riscvpt_iommu_set_prot: 권한이 하나도 없으면 거절한다 — 그 조합이
 *   표 항목으로 해석되기 때문이다.
 * riscvpt_iommu_fmt_init: 주소 폭에서 단계 수를 정한다.
 */
#ifndef __GENERIC_PT_FMT_RISCV_H	/* [한국어] 중복 포함 방지 */
#define __GENERIC_PT_FMT_RISCV_H	/* [한국어] 같은 이름으로 표시 */

#include "defs_riscv.h"	/* [한국어] 이 형식의 주소 타입과 쓰기 속성 */
#include "../pt_defs.h"	/* [한국어] 순회 상태와 산술 */

#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP */
#include <linux/container_of.h>	/* [한국어] IOMMU 객체와 공통 상태를 오간다 */
#include <linux/log2.h>	/* [한국어] ilog2 — 표 항목 수와 묶음 크기 */
#include <linux/sizes.h>	/* [한국어] SZ_64K — Svnapot 묶음의 크기 */

/*
 * [한국어] 이 형식의 크기 상수들.
 * 32비트 변종(Sv32)과 64비트 변종(Sv39/48/57)이 갈린다.
 */
enum {
	PT_ITEM_WORD_SIZE = sizeof(pt_riscv_entry_t),
	/* [한국어] 페이지 테이블 항목 하나의 바이트 폭.
	 * 설정자: pt_riscv_entry_t 의 크기 — 32비트 변종(Sv32)에서는 u32, 64비트
	 *   변종(Sv39/48/57)에서는 u64 로 typedef 되어 있다.
	 * 읽는 자: generic_pt 의 공통 순회 코드. 표 안에서 다음 항목으로 넘어가는
	 *   보폭과, 표 하나에 항목이 몇 개 들어가는지를 이 값으로 계산한다.
	 * 값 범위: 4 또는 8.
	 * sizeof 로 쓴 이유: 두 변종이 같은 소스를 두 번 컴파일해 만들어지므로,
	 *   숫자를 직접 적으면 변종마다 #ifdef 가 하나 더 필요하다. 타입에서 끌어내면
	 *   타입 정의 한 곳만 보면 된다. */
#ifdef PT_RISCV_32BIT
	PT_MAX_VA_ADDRESS_LG2 = 32,	/* [한국어] Sv32: 입력 32비트 */
	PT_MAX_OUTPUT_ADDRESS_LG2 = 34,
	/* [한국어] (Sv32) 출력 물리 주소의 비트 폭.
	 * 읽는 자: 매핑하려는 물리 주소가 이 형식으로 표현 가능한지 검사하는 코드.
	 * 값이 34 인 것이 눈여겨볼 점이다 — 입력(32)보다 출력이 넓다. 32비트 RISC-V
	 *   시스템도 최대 16GB 의 물리 메모리를 붙일 수 있고, IOMMU 가 좁은 IOVA 를
	 *   넓은 물리 주소로 옮기는 것이 바로 그런 시스템에서 필요한 일이다.
	 * 넘는 주소를 매핑하려 하면 io-pgtable 이 -ERANGE 로 거절한다. */
	PT_MAX_TOP_LEVEL = 1,
	/* [한국어] (Sv32) 최상위 단계의 번호 — 0 부터 세므로 전체 2단계라는 뜻이다.
	 * 읽는 자: 표를 만들 때 몇 단계를 잡을지, 그리고 순회가 어디서 시작할지.
	 * 값 범위: 1. Sv32 는 단계 수가 고정이라 조정할 여지가 없다.
	 * 0 기반이라는 점을 놓치기 쉽다 — 이 값이 1 이면 단계는 0 과 1 둘이다.
	 * 아래 64비트 변종의 4(=5단계)와 대비된다. */
#else
	PT_MAX_VA_ADDRESS_LG2 = 57,	/* [한국어] Sv57 까지: 입력 57비트 */
	PT_MAX_OUTPUT_ADDRESS_LG2 = 56,
	/* [한국어] (Sv39/48/57) 출력 물리 주소의 비트 폭.
	 * 읽는 자: 매핑 요청의 물리 주소가 이 폭에 드는지 검사하는 코드.
	 * 값이 56 인 이유: RISC-V 규격이 PPN 을 44비트로 정했고, 여기에 페이지 안
	 *   오프셋 12비트를 더하면 56비트가 된다.
	 * 입력 폭(57)보다 좁다는 점이 32비트 변종과 반대다. 64비트에서는 주소 공간이
	 *   물리 메모리보다 넓어, 넓은 IOVA 를 좁은 물리 주소로 모으는 쪽이 된다. */
	PT_MAX_TOP_LEVEL = 4,
	/* [한국어] (Sv39/48/57) 최상위 단계의 번호 — 0 기반이라 최대 5단계라는 뜻이다.
	 * 읽는 자: 표를 만들 때. 실제로 몇 단계를 쓸지는 요청한 주소 공간 크기에
	 *   따라 이 값 이하에서 정해진다.
	 * 값 범위: 4. Sv39 는 3단계, Sv48 은 4단계, Sv57 은 5단계를 쓰는데, 셋이 같은
	 *   항목 형식을 쓰고 단계 수만 다르다. 그래서 상한만 여기 적어 두고 실제
	 *   단계 수는 실행 중에 고른다.
	 * 이 유연함이 RISC-V 페이지 테이블 형식의 특징이다 — 상위 단계를 하나 더
	 *   얹으면 그대로 더 넓은 주소 공간이 된다. */
#endif
	PT_GRANULE_LG2SZ = 12,	/* [한국어] 최소 페이지 4KB */
	PT_TABLEMEM_LG2SZ = 12,
	/* [한국어] 표 하나가 차지하는 메모리 크기의 로그 값 — 2^12 = 4KB.
	 * 읽는 자: 표를 할당할 때, 그리고 표 하나에 항목이 몇 개 들어가는지 계산할 때
	 *   (PT_TABLEMEM_LG2SZ - log2(PT_ITEM_WORD_SIZE) 가 항목 수의 로그다).
	 * 왜 위 PT_GRANULE_LG2SZ 와 따로 두는가: 둘은 우연히 같은 12 지만 뜻이 다르다.
	 *   GRANULE 은 매핑할 수 있는 최소 페이지 크기이고, 이쪽은 표 자체의 크기다.
	 *   형식에 따라 둘이 다를 수 있어(표는 16KB 인데 페이지는 4KB 인 형식도 있다)
	 *   공통 코드는 언제나 둘을 구분해 쓴다. */

	/* fsc.PPN is 44 bits wide, all PPNs are 4k aligned */
	PT_TOP_PHYS_MASK = GENMASK_ULL(55, 12),
	/* [한국어] (원 주석: fsc.PPN 은 44비트이고 모든 PPN 은 4K 정렬이다) 최상위 표의 물리 주소로 쓸 수 있는 비트들.
	 * 읽는 자: 최상위 표의 주소를 하드웨어 레지스터(RISC-V IOMMU 의 fsc 필드)에
	 *   실을 때, 그리고 그 값이 유효한지 검사할 때.
	 * 값 범위: 비트 12 부터 55 까지 — 44비트의 PPN 에 4KB 정렬을 곱한 범위다.
	 * 하위 12비트가 빠져 있는 이유가 원 주석에 있다: 모든 PPN 은 4K 정렬이라
	 *   하위 12비트가 언제나 0 이고, 하드웨어는 그 자리를 다른 필드로 쓴다.
	 *   마스크에 그 자리를 넣으면 다른 필드를 덮어쓴다. */
};

/* PTE bits */
/*
 * [한국어] (위 영어 주석에 이어)
 * 페이지 테이블 항목의 비트 배치.
 * R/W/X 가 하나도 없으면 그 항목은 아래 표를 가리킨다 — 별도의 표/잎
 * 구분 비트가 없는 것이 이 형식의 특징이다.
 */
enum {
	RISCVPT_V = BIT(0),
	/* [한국어] 유효 비트 — 0 이면 이 항목은 비어 있다.
	 * 설정자: 매핑을 만들 때 세우고, 지울 때 내린다.
	 * 읽는 자: 하드웨어와, 항목이 비었는지 보는 모든 순회 코드.
	 * 값 범위: 0 또는 1. 0 이면 나머지 비트는 모두 무의미하다.
	 * 왜 0번 비트인가: 하드웨어가 항목을 읽자마자 판정할 수 있게 규격이 그렇게
	 *   정했다. 대부분의 페이지 테이블 형식이 같은 선택을 한다.
	 * 이 비트를 내리는 것만으로 매핑이 사라지므로, 항목 전체를 지우지 않아도
	 *   된다 — 다만 나머지 비트가 남아 있으면 진단이 헷갈려 보통 통째로 0 을 쓴다. */
	RISCVPT_R = BIT(1),
	/* [한국어] 읽기 허용 비트.
	 * 설정자: 매핑을 만들 때 IOMMU_READ 권한이 있으면 세운다.
	 * 읽는 자: 하드웨어가 읽기 요청을 허용할지 판정할 때.
	 * 이 형식에서 R/W/X 는 권한만 뜻하지 않는다. 셋이 모두 0 이면 그 항목은 잎이
	 *   아니라 다음 단계 표를 가리킨다 — 별도의 표/잎 구분 비트가 없는 것이 이
	 *   형식의 특징이다(위 블록 주석 참고).
	 * 그래서 잎을 만들 때는 R/W/X 중 적어도 하나를 반드시 세워야 한다. 권한이
	 *   하나도 없는 매핑은 이 형식으로 표현할 수 없다. */
	RISCVPT_W = BIT(2),
	/* [한국어] 쓰기 허용 비트.
	 * 설정자: IOMMU_WRITE 권한이 있으면 세운다.
	 * 읽는 자: 하드웨어.
	 * 중요한 제약: 읽기(R) 없이 쓰기(W)만 세운 조합은 규격이 예약으로 남겨 둔
	 *   것이라 쓰면 안 된다. 그런 항목을 만나면 하드웨어가 폴트를 낸다.
	 *   그래서 쓰기 전용 매핑을 요청받아도 R 을 함께 세워야 한다.
	 * 위 R 의 설명대로, 이 비트도 "표인가 잎인가"를 가르는 데 함께 쓰인다. */
	RISCVPT_X = BIT(3),
	/* [한국어] 실행 허용 비트.
	 * 설정자: IOMMU_EXEC 권한이 있으면 세운다. 장치 DMA 에서는 거의 쓰지 않는다.
	 * 읽는 자: 하드웨어.
	 * IOMMU 문맥에서 실행 권한이 의미를 갖는 경우: 장치가 CPU 처럼 명령을
	 *   가져와 실행하는 구조(일부 가속기, IOMMU 뒤의 보조 프로세서)일 때다.
	 *   보통의 DMA 에는 해당하지 않아 대개 0 이다.
	 * 위 R/W 와 함께 셋이 모두 0 이면 표를 가리키는 항목이 된다. */
	RISCVPT_U = BIT(4),
	/* [한국어] 사용자 권한 접근 허용 비트.
	 * 설정자: 이 형식을 IOMMU 로 쓸 때는 언제나 세운다.
	 * 읽는 자: 하드웨어.
	 * 왜 언제나 세우는가: RISC-V 는 CPU 의 페이지 테이블과 IOMMU 의 페이지 테이블이
	 *   같은 형식이다. CPU 쪽에서는 이 비트가 커널/사용자 권한을 가르지만,
	 *   장치의 DMA 는 항상 사용자 권한 수준으로 취급된다. 세우지 않으면 하드웨어가
	 *   모든 장치 접근을 거부한다.
	 * CPU 와 형식을 공유하는 데서 오는, 이 형식에만 있는 함정이다. */
	RISCVPT_G = BIT(5),
	/* [한국어] 전역 매핑 비트 — ASID 단위 무효화에서 살아남는다.
	 * 설정자: IOMMU 용 표에서는 세우지 않는다.
	 * 읽는 자: 하드웨어의 TLB 무효화 판정.
	 * 왜 IOMMU 에서는 쓰지 않는가: 전역 매핑은 모든 주소 공간에 공통인 항목
	 *   (커널 매핑 등)을 위한 것이다. 장치의 도메인은 서로 격리되어야 하므로
	 *   전역이어서는 안 되고, 세워 두면 다른 도메인의 무효화가 이 항목을 비우지
	 *   못해 해제한 매핑이 TLB 에 남는다.
	 * 이것도 CPU 형식을 공유하는 데서 오는 항목이다. */
	RISCVPT_A = BIT(6),
	/* [한국어] 접근됨(accessed) 비트.
	 * 설정자: io-pgtable 이 항목을 만들 때 미리 1 로 세운다.
	 * 읽는 자: 하드웨어.
	 * 왜 미리 세우는가: 규격상 하드웨어는 이 비트가 0 인 항목을 만나면 스스로
	 *   1 로 갱신해야 한다(또는 폴트를 낸다). 그 갱신은 하드웨어가 페이지 테이블
	 *   메모리에 쓰기를 하는 것이라, 캐시 일관성과 원자성 문제를 만든다.
	 *   처음부터 세워 두면 그 쓰기가 아예 일어나지 않는다.
	 * IOMMU 는 CPU 와 달리 접근 여부를 추적할 이유가 없어 잃는 것도 없다. */
	RISCVPT_D = BIT(7),
	/* [한국어] 더티(dirty) 비트 — 이 페이지에 쓰기가 있었다는 표시.
	 * 설정자: 위 A 비트와 같은 이유로 미리 세워 둔다. 더티 추적 기능을 쓰는
	 *   경우에는 대신 0 으로 두고 하드웨어가 세우게 한다.
	 * 읽는 자: 하드웨어, 그리고 더티 추적을 쓰는 경우 그 결과를 읽는 코드.
	 * 더티 추적이 필요한 곳: 라이브 마이그레이션. 장치가 어느 페이지를 고쳤는지
	 *   알아야 그 페이지만 다시 보낼 수 있다.
	 * 평소에 미리 세워 두는 이유는 A 비트와 같다 — 하드웨어의 표 쓰기를 없앤다. */
	RISCVPT_RSW = GENMASK(9, 8),
	/* [한국어] 소프트웨어 예약 필드 — 하드웨어가 무시한다.
	 * 설정자/읽는 자: 소프트웨어가 자유롭게 쓸 수 있다. 이 드라이버는 쓰지 않는다.
	 * 왜 규격이 이런 자리를 남겨 두는가: 운영체제가 항목마다 자기만의 표시를
	 *   달고 싶을 때가 있다(스왑 상태, 참조 계수 등). 별도의 자료구조를 두는 대신
	 *   항목 안에 담을 수 있게 두 비트를 비워 둔 것이다.
	 * 값 범위: 2비트. 하드웨어는 이 자리를 읽지도 쓰지도 않는다. */
	RISCVPT_PPN32 = GENMASK(31, 10),
	/* [한국어] (Sv32) 물리 페이지 번호가 들어가는 자리.
	 * 설정자: 잎 항목에서는 매핑할 물리 주소를, 표 항목에서는 다음 단계 표의
	 *   주소를 페이지 번호로 바꿔 넣는다.
	 * 읽는 자: 하드웨어, 그리고 항목에서 주소를 되꺼내는 코드.
	 * 값 범위: 비트 10 부터 31 까지 22비트. 페이지 번호이므로 실제 주소는 이
	 *   값에 12비트를 왼쪽으로 민 것이다 — 그래서 34비트 물리 주소를 표현한다
	 *   (위 PT_MAX_OUTPUT_ADDRESS_LG2 참고).
	 * 아래 PPN64 와 이름만 다르고 역할이 같다. 어느 쪽을 쓸지는 파일 아래쪽의
	 *   RISCVPT_PPN 매크로가 변종에 따라 정한다. */

	RISCVPT_PPN64 = GENMASK_ULL(53, 10),
	/* [한국어] (Sv39/48/57) 물리 페이지 번호가 들어가는 자리.
	 * 설정자/읽는 자: 위 PPN32 와 같다.
	 * 값 범위: 비트 10 부터 53 까지 44비트. 12비트를 밀면 56비트 물리 주소가 된다.
	 * 세 변종(Sv39/48/57)이 같은 필드를 쓴다는 점이 중요하다 — 단계 수만 다르고
	 *   항목 형식은 같아서, 이 파일 하나가 셋을 모두 다룬다. */
	RISCVPT_PPN64_64K = GENMASK_ULL(53, 14),
	/* [한국어] Svnapot 항목에서 쓰는, 하위 4비트를 뺀 좁은 페이지 번호.
	 * 설정자: 64KB 연속 묶음을 만들 때. 그 묶음 안의 모든 항목이 같은 값을 갖는다.
	 * 읽는 자: 하드웨어가 묶음의 시작 주소를 알아낼 때.
	 * 왜 좁은가: NAPOT 방식에서는 페이지 번호의 하위 비트가 주소가 아니라 묶음의
	 *   크기를 나타내는 부호로 쓰인다. 64KB 묶음은 4KB 페이지 열여섯 개라
	 *   하위 4비트가 그 부호에 넘어가고, 남은 자리가 이 마스크다.
	 * 그 부호 값이 아래 RISCVPT_PPN64_64K_SZ 다. */
	RISCVPT_PBMT = GENMASK_ULL(62, 61),
	/* [한국어] 페이지 기반 메모리 타입(PBMT) 필드.
	 * 설정자: 이 드라이버는 기본값(0, PMA 가 정한 대로)을 쓴다.
	 * 읽는 자: 하드웨어.
	 * 무엇을 정하는가: 그 페이지의 접근이 캐시 가능한지, 쓰기 결합이 되는지,
	 *   IO 순서를 지켜야 하는지를 항목 단위로 지정한다. 같은 물리 주소라도
	 *   매핑에 따라 다른 성질을 줄 수 있다.
	 * 값 범위: 2비트. 0 은 "물리 메모리 속성(PMA)이 정한 대로", 나머지는
	 *   각각 비캐시/IO 등을 강제한다.
	 * 이 필드는 확장(Svpbmt)이라 지원하지 않는 하드웨어에서는 0 이어야 한다. */
	RISCVPT_N = BIT_ULL(63),
	/* [한국어] 연속 묶음(NAPOT) 표시 비트.
	 * 설정자: 여러 개의 이웃한 항목을 하나의 큰 매핑으로 묶을 때, 그 묶음에 속한
	 *   모든 항목에 세운다.
	 * 읽는 자: 하드웨어. 이 비트를 본 하드웨어는 묶음 전체를 TLB 항목 하나로
	 *   담을 수 있다.
	 * 왜 성능에 중요한가: 64KB 를 4KB 항목 열여섯 개로 매핑하면 TLB 항목도
	 *   열여섯 개를 먹는다. 묶으면 하나로 줄어 TLB 적중률이 크게 오른다.
	 * 제약: 묶음의 시작 주소와 크기가 자연 정렬되어야 하고, 묶음 안의 모든
	 *   항목이 같은 값을 가져야 한다. 그 조건을 이 파일의 contig 관련 함수들이 검사한다. */

	/* Svnapot encodings for ppn[0] */
	RISCVPT_PPN64_64K_SZ = BIT(13),
	/* [한국어] (원 주석: ppn[0] 자리의 Svnapot 인코딩) 64KB 묶음을 뜻하는 크기 부호.
	 * 설정자: NAPOT 묶음 항목을 만들 때 페이지 번호와 함께 OR 로 넣는다.
	 * 읽는 자: 하드웨어가 묶음의 크기를 판정할 때.
	 * 값 범위: 비트 13. 위 PPN64_64K 마스크가 비트 14 부터 시작하는 것과 맞물려,
	 *   이 비트가 그 아래 남은 자리를 차지한다.
	 * 왜 크기를 별도 필드가 아니라 주소 자리에 숨기는가: NAPOT 은 "주소의 하위
	 *   비트 패턴이 곧 크기"라는 규칙이다. 1 이 처음 나타나는 자리가 묶음 크기를
	 *   말해 주므로, 별도 필드 없이 크기를 인코딩할 수 있다.
	 * 현재 규격은 64KB 묶음 하나만 정의해 두어, 이 상수도 하나뿐이다. */
};

#ifdef PT_RISCV_32BIT	/* [한국어] 32비트 변종을 찍어 내는 중인가 */
#define RISCVPT_PPN RISCVPT_PPN32	/* [한국어] 32비트 변종의 페이지 번호 필드 */
#define pt_riscv pt_riscv_32	/* [한국어] 32비트 변종의 구조체 이름 */
#else
#define RISCVPT_PPN RISCVPT_PPN64	/* [한국어] 64비트 변종의 페이지 번호 필드 */
#define pt_riscv pt_riscv_64	/* [한국어] 64비트 변종의 구조체 이름 */
#endif

/*
 * [한국어] 공통 상태에서 이 형식의 구조체로 되짚는 매크로.
 * container_of_const 라 const 포인터를 넘겨도 const 가 유지된다.
 */
#define common_to_riscvpt(common_ptr) \
	container_of_const(common_ptr, struct pt_riscv, common)	/* [한국어] 공통 부분을 품은 형식별 구조체로 되짚는다 */
#define to_riscvpt(pts) common_to_riscvpt((pts)->range->common)	/* [한국어] 순회 상태에서 한 번에 되짚는 지름길 */

/*
 * [한국어]
 * riscvpt_table_pa - 표 항목이 가리키는 아래 표의 물리 주소
 *
 * @pts: 볼 항목(PT_ENTRY_TABLE).
 * @return: 아래 표의 물리 주소.
 *
 * 표 항목에는 Svnapot 이 쓰이지 않으므로 늘 기본 페이지 번호 필드를 쓴다.
 */
static inline pt_oaddr_t riscvpt_table_pa(const struct pt_state *pts)
{
	return oalog2_mul(FIELD_GET(RISCVPT_PPN, pts->entry), PT_GRANULE_LG2SZ);	/* [한국어] 표 항목에는 Svnapot 이 없어 기본 필드를 쓴다 */
}
#define pt_table_pa riscvpt_table_pa	/* [한국어] 공통 API 이름으로 잇는다 */

/*
 * [한국어]
 * riscvpt_entry_oa - 잎 항목이 내는 출력 주소
 *
 * @pts: 볼 항목.
 * @return: 출력 주소.
 *
 * Svnapot 항목이면 페이지 번호 필드가 좁아진다. 64KB 정렬이 강제되므로
 * 하위 4비트가 필요 없고, 그 자리에 크기 부호가 들어가기 때문이다.
 *
 * 그래서 그 경우에는 넓은 필드에서 값을 꺼내 64KB 를 곱한다.
 */
static inline pt_oaddr_t riscvpt_entry_oa(const struct pt_state *pts)
{
	if (pts_feature(pts, PT_FEAT_RISCV_SVNAPOT_64K) &&	/* [한국어] 연속 페이지 확장을 쓰고 */
	    pts->entry & RISCVPT_N) {	/* [한국어] N 비트가 서 있으면 */
		PT_WARN_ON(pts->level != 0);	/* [한국어] Svnapot 은 0단계에서만 쓰인다 */
		return oalog2_mul(FIELD_GET(RISCVPT_PPN64_64K, pts->entry),	/* [한국어] 하위 4비트가 크기 부호라 좁은 필드에서 꺼내 */
				  ilog2(SZ_64K));	/* [한국어] 64KB 를 곱한다 */
	}
	return oalog2_mul(FIELD_GET(RISCVPT_PPN, pts->entry), PT_GRANULE_LG2SZ);	/* [한국어] 보통 항목은 기본 필드 */
}
#define pt_entry_oa riscvpt_entry_oa	/* [한국어] 이쪽을 구현하면 pt_item_oa 는 기본이 만들어 준다 */

/*
 * [한국어]
 * riscvpt_can_have_leaf - 이 단계에 잎을 놓을 수 있는가
 *
 * @pts: 현재 단계(쓰지 않는다).
 * @return: 늘 참.
 *
 * 이 형식은 모든 단계에 잎을 허용한다 — 최상위에도 큰 페이지를 놓을 수
 * 있어, x86 계열보다 표현 범위가 넓다.
 */
static inline bool riscvpt_can_have_leaf(const struct pt_state *pts)
{
	return true;	/* [한국어] 모든 단계에 잎을 놓을 수 있다 — 최상위에도 */
}
#define pt_can_have_leaf riscvpt_can_have_leaf	/* [한국어] 공통 API 이름으로 */

/* Body in pt_fmt_defaults.h */
/*
 * [한국어] (원 주석: 몸통은 pt_fmt_defaults.h 에 있다)
 * 아래 함수들이 먼저 쓰므로 여기서 선언만 해 둔다.
 */
static inline unsigned int pt_table_item_lg2sz(const struct pt_state *pts);

/*
 * [한국어]
 * riscvpt_entry_num_contig_lg2 - 이 잎이 몇 개의 항목으로 이루어졌는가
 *
 * @pts: 볼 항목.
 * @return: Svnapot 이면 4(16개), 아니면 0.
 *
 * Svnapot 은 0단계에서만 쓰이고 묶음 크기도 16 하나뿐이다 — 그래서 답이
 * 두 값 중 하나로 정해진다.
 *
 * 바깥 조건이 PT_SUPPORTED_FEATURE 인 것이 눈에 띈다. 그 기능을 아예
 * 지원하지 않는 빌드에서는 컴파일 시 거짓이 되어 이 갈래가 통째로 사라진다.
 */
static inline unsigned int
riscvpt_entry_num_contig_lg2(const struct pt_state *pts)
{
	if (PT_SUPPORTED_FEATURE(PT_FEAT_RISCV_SVNAPOT_64K) &&	/* [한국어] 컴파일 시 상수 — 지원하지 않는 빌드에서는 이 갈래가 사라진다 */
	    pts->entry & RISCVPT_N) {	/* [한국어] N 비트가 서 있으면 */
		PT_WARN_ON(!pts_feature(pts, PT_FEAT_RISCV_SVNAPOT_64K));	/* [한국어] 인스턴스가 그 기능을 켜지 않았는데 항목에 있으면 모순이다 */
		PT_WARN_ON(pts->level);	/* [한국어] Svnapot 은 0단계에서만 */
		return ilog2(16);	/* [한국어] 16개가 한 묶음 */
	}
	return ilog2(1);	/* [한국어] 그 밖에는 단일 항목 */
}
#define pt_entry_num_contig_lg2 riscvpt_entry_num_contig_lg2	/* [한국어] 이 정의가 있으면 연속 페이지를 지원한다는 뜻이다 */

/*
 * [한국어]
 * riscvpt_num_items_lg2 - 이 단계 표의 항목 수(지수)
 *
 * @pts: 현재 단계.
 * @return: 늘 9(4KB 표에 8바이트 항목이면 512개).
 */
static inline unsigned int riscvpt_num_items_lg2(const struct pt_state *pts)
{
	return PT_TABLEMEM_LG2SZ - ilog2(sizeof(u64));	/* [한국어] 4KB 표에 8바이트 항목 → 512개 */
}
#define pt_num_items_lg2 riscvpt_num_items_lg2	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * riscvpt_contig_count_lg2 - 이 단계에서 만들 수 있는 연속 묶음의 크기
 *
 * @pts: 현재 단계.
 * @return: 0단계에서 Svnapot 을 쓰면 4(16개), 아니면 0.
 *
 * pt_fmt_defaults.h 의 pt_possible_sizes 가 이 값으로 크기 비트맵을
 * 조립한다 — 즉 이 형식은 단계 크기와 64KB 두 가지를 만들 수 있다.
 */
static inline unsigned short
riscvpt_contig_count_lg2(const struct pt_state *pts)
{
	if (pts->level == 0 && pts_feature(pts, PT_FEAT_RISCV_SVNAPOT_64K))	/* [한국어] 0단계에서만, 기능이 켜져 있을 때만 */
		return ilog2(16);	/* [한국어] 16개 묶음 = 64KB */
	return ilog2(1);	/* [한국어] 그 밖에는 묶을 수 없다 */
}
#define pt_contig_count_lg2 riscvpt_contig_count_lg2	/* [한국어] 기본 pt_possible_sizes 가 이 값으로 비트맵을 만든다 */

/*
 * [한국어]
 * riscvpt_load_entry_raw - 항목을 읽고 종류를 가른다
 *
 * @pts: 읽을 위치.
 * @return: 비었는가, 표인가, 출력 주소인가.
 *
 * 유효 비트가 존재를 가르고, R/W/X 중 하나라도 서 있으면 잎이다. 셋 다
 * 없으면 아래 표를 가리킨다 — 이 형식에는 별도의 구분 비트가 없다.
 *
 * riscvpt_iommu_set_prot 이 권한 없는 매핑을 거절하는 이유가 이것이다.
 */
static inline enum pt_entry_type riscvpt_load_entry_raw(struct pt_state *pts)
{
	const pt_riscv_entry_t *tablep = pt_cur_table(pts, pt_riscv_entry_t);	/* [한국어] 이 단계의 표 */
	pt_riscv_entry_t entry;	/* [한국어] 읽은 값 */

	pts->entry = entry = READ_ONCE(tablep[pts->index]);	/* [한국어] 한 번만 읽어 담는다 */
	if (!(entry & RISCVPT_V))	/* [한국어] 유효 비트가 0 이면 */
		return PT_ENTRY_EMPTY;	/* [한국어] 매핑이 없다 */
	if (pts->level == 0 ||	/* [한국어] 0단계에는 아래 표가 없고 */
	    ((entry & (RISCVPT_X | RISCVPT_W | RISCVPT_R)) != 0))	/* [한국어] 권한이 하나라도 서 있으면 */
		return PT_ENTRY_OA;	/* [한국어] 잎이다 */
	return PT_ENTRY_TABLE;	/* [한국어] 셋 다 없으면 아래 표 — 별도 구분 비트가 없다 */
}
#define pt_load_entry_raw riscvpt_load_entry_raw	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * riscvpt_install_leaf_entry - 표에 잎 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @oa: 출력 주소.
 * @oasz_lg2: 이 잎이 덮는 크기의 지수.
 * @attrs: 얹을 권한 비트.
 *
 * 보통은 한 자리만 쓴다. 64KB 연속 페이지면 16 자리를 같은 값으로 채우고
 * N 비트와 크기 부호를 함께 세운다 — 하드웨어가 그 묶음을 TLB 한 자리로
 * 잡는다.
 *
 * 원 주석의 FIXME 가 남아 있다: 이 쓰기를 cmpxchg 로 해야 하는지 아직
 * 결론이 나지 않았다. 표 설치와 달리 잎 설치는 상위 락이 직렬화한다는
 * 것이 다른 형식의 전제인데, RISC-V 에서 그 전제가 성립하는지 확인이
 * 필요하다는 뜻이다.
 */
static inline void
riscvpt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			   unsigned int oasz_lg2,
			   const struct pt_write_attrs *attrs)
{
	pt_riscv_entry_t *tablep = pt_cur_table(pts, pt_riscv_entry_t);	/* [한국어] 이 단계의 표 */
	pt_riscv_entry_t entry;	/* [한국어] 만들 항목 값 */

	if (!pt_check_install_leaf_args(pts, oa, oasz_lg2))	/* [한국어] 정렬과 크기를 먼저 확인 */
		return;	/* [한국어] 어긋나면 쓰지 않는다 */

	entry = RISCVPT_V |	/* [한국어] 유효 비트 */
		FIELD_PREP(RISCVPT_PPN, log2_div(oa, PT_GRANULE_LG2SZ)) |	/* [한국어] 주소를 페이지 번호로 */
		attrs->descriptor_bits;	/* [한국어] 권한 — 하나라도 있어야 잎으로 읽힌다 */

	if (pts_feature(pts, PT_FEAT_RISCV_SVNAPOT_64K) && pts->level == 0 &&	/* [한국어] 연속 페이지를 쓸 수 있는 자리이고 */
	    oasz_lg2 != PT_GRANULE_LG2SZ) {	/* [한국어] 기본 페이지보다 크면 */
		u64 *end;	/* [한국어] 채울 마지막 다음 자리 */

		entry |= RISCVPT_N | RISCVPT_PPN64_64K_SZ;	/* [한국어] 묶음 표시와 크기 부호 */
		tablep += pts->index;	/* [한국어] 첫 자리로 */
		end = tablep + log2_div(SZ_64K, PT_GRANULE_LG2SZ);	/* [한국어] 16 자리 */
		for (; tablep != end; tablep++)	/* [한국어] 같은 값을 모두에 */
			WRITE_ONCE(*tablep, entry);	/* [한국어] 하드웨어가 그 묶음을 TLB 한 자리로 잡는다 */
	} else {
		/* FIXME does riscv need this to be cmpxchg? */
		WRITE_ONCE(tablep[pts->index], entry);	/* [한국어] (원 주석의 FIXME: cmpxchg 가 필요한지 아직 미결) */
	}
	pts->entry = entry;	/* [한국어] 순회 상태도 새 값으로 */
}
#define pt_install_leaf_entry riscvpt_install_leaf_entry	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * riscvpt_install_table - 아래 표를 가리키는 항목을 쓴다
 *
 * @pts: 쓸 위치.
 * @table_pa: 아래 표의 물리 주소.
 * @attrs: 속성(이 형식에서는 쓰지 않는다).
 * @return: 내가 꽂았으면 참.
 *
 * 유효 비트와 주소만 세운다. 권한 비트를 하나도 세우지 않는 것이 곧
 * "이것은 표"라는 신호다 — 다른 형식이 표 단계에 권한을 열어 두는 것과
 * 정반대의 규칙이다.
 */
static inline bool riscvpt_install_table(struct pt_state *pts,
					 pt_oaddr_t table_pa,
					 const struct pt_write_attrs *attrs)
{
	pt_riscv_entry_t entry;	/* [한국어] 만들 항목 값 */

	entry = RISCVPT_V |	/* [한국어] 유효 비트만 */
		FIELD_PREP(RISCVPT_PPN, log2_div(table_pa, PT_GRANULE_LG2SZ));	/* [한국어] 권한을 세우지 않는 것이 곧 "표"라는 신호다 */
	return pt_table_install64(pts, entry);	/* [한국어] 경합에서 지면 거짓 */
}
#define pt_install_table riscvpt_install_table	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * riscvpt_attr_from_entry - 항목에서 권한 비트만 다시 꺼낸다
 *
 * @pts: 읽을 항목.
 * @attrs: 채울 결과.
 *
 * 권한·전역·접근·더티를 옮긴다. 주소와 N 비트는 새 항목을 만들 때 다시
 * 정해진다.
 */
static inline void riscvpt_attr_from_entry(const struct pt_state *pts,
					   struct pt_write_attrs *attrs)
{
	attrs->descriptor_bits =	/* [한국어] 새 항목에 그대로 옮길 비트만 */
		pts->entry & (RISCVPT_R | RISCVPT_W | RISCVPT_X | RISCVPT_U |	/* [한국어] 권한과 사용자 접근 */
			      RISCVPT_G | RISCVPT_A | RISCVPT_D);	/* [한국어] 전역·접근·더티 — 주소와 N 은 다시 정해진다 */
}
#define pt_attr_from_entry riscvpt_attr_from_entry	/* [한국어] 공통 API 이름으로 */

/* --- iommu */
#include <linux/generic_pt/iommu.h>	/* [한국어] (원 주석: 여기부터 IOMMU 계층과의 접점) */
#include <linux/iommu.h>	/* [한국어] IOMMU_READ/WRITE/NOEXEC 권한 상수 */

#define pt_iommu_table pt_iommu_riscv_64	/* [한국어] 공통 코드가 부르는 이름을 이 형식의 구조체로 */

/* The common struct is in the per-format common struct */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * common_from_iommu - IOMMU 객체에서 공통 페이지 테이블 상태로
 *
 * @iommu_table: IOMMU 계층의 객체.
 * @return: 그 안에 박힌 pt_common.
 */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)	/* [한국어] IOMMU 객체에서 형식별 구조체로 */
			->riscv_64pt.common;	/* [한국어] 그 안의 공통 부분으로 */
}

/*
 * [한국어]
 * iommu_from_common - 공통 상태에서 IOMMU 객체로 되돌아간다
 *
 * @common: 공통 페이지 테이블 상태.
 * @return: 그것을 품은 IOMMU 객체.
 */
static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, riscv_64pt.common)	/* [한국어] 공통 상태에서 */
			->iommu;	/* [한국어] IOMMU 객체로 되돌아간다 */
}

/*
 * [한국어]
 * riscvpt_iommu_set_prot - IOMMU 권한을 항목 비트로 옮긴다
 *
 * @common: 페이지 테이블 인스턴스.
 * @attrs: 채울 쓰기 속성.
 * @iommu_prot: IOMMU_READ/WRITE/NOEXEC 조합.
 * @return: 0 성공, -EOPNOTSUPP 이면 만들 수 없는 조합이다.
 *
 * 쓰기가 읽기를 함께 세우는 것이 요점이다. RISC-V 는 "쓰기만 되고 읽기는
 * 안 되는" 조합을 예약된 것으로 두므로, 그 항목을 만들면 안 된다.
 *
 * 권한이 하나도 없으면 거절한다 — 그런 항목은 하드웨어가 아래 표를
 * 가리키는 것으로 읽는다.
 *
 * 실행 권한이 기본으로 켜지는 것도 다른 형식과 다르다. IOMMU_NOEXEC 를
 * 명시해야 꺼진다.
 */
static inline int riscvpt_iommu_set_prot(struct pt_common *common,
					 struct pt_write_attrs *attrs,
					 unsigned int iommu_prot)
{
	u64 pte;	/* [한국어] 만들 속성 비트 */

	pte = RISCVPT_A | RISCVPT_U;	/* [한국어] 접근 비트를 미리 세워 하드웨어의 표 쓰기를 없앤다. 장치 접근은 사용자 권한 */
	if (iommu_prot & IOMMU_WRITE)	/* [한국어] 쓰기 허용이면 */
		pte |= RISCVPT_W | RISCVPT_R | RISCVPT_D;	/* [한국어] "쓰기만 되고 읽기는 안 되는" 조합은 예약이라 읽기도 함께 세운다 */
	if (iommu_prot & IOMMU_READ)	/* [한국어] 읽기 허용이면 */
		pte |= RISCVPT_R;	/* [한국어] 읽기 비트 */
	if (!(iommu_prot & IOMMU_NOEXEC))	/* [한국어] 실행 금지를 명시하지 않았으면 */
		pte |= RISCVPT_X;	/* [한국어] 기본으로 실행을 허용한다 */

	/* Caller must specify a supported combination of flags */
	if (unlikely((pte & (RISCVPT_X | RISCVPT_W | RISCVPT_R)) == 0))	/* [한국어] (원 주석: 호출자가 지원되는 조합을 줘야 한다) */
		return -EOPNOTSUPP;	/* [한국어] 셋 다 없으면 하드웨어가 표 항목으로 읽는다 */

	attrs->descriptor_bits = pte;	/* [한국어] 잎을 쓸 때 그대로 얹힌다 */
	return 0;	/* [한국어] 성공 */
}
#define pt_iommu_set_prot riscvpt_iommu_set_prot	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * riscvpt_iommu_fmt_init - 형식별 초기화
 *
 * @iommu_table: 초기화할 객체.
 * @cfg: 드라이버가 준 설정.
 * @return: 0 성공, -EINVAL 이면 지원하지 않는 주소 폭이다.
 *
 * 주소 폭이 곧 단계 수를 정한다. Sv39·Sv48·Sv57 이 각각 3·4·5단계이며,
 * 그 밖의 폭은 명세에 없다.
 */
static inline int
riscvpt_iommu_fmt_init(struct pt_iommu_riscv_64 *iommu_table,
		       const struct pt_iommu_riscv_64_cfg *cfg)
{
	struct pt_riscv *table = &iommu_table->riscv_64pt;	/* [한국어] 형식별 페이지 테이블 상태 */

	switch (cfg->common.hw_max_vasz_lg2) {	/* [한국어] 주소 폭이 곧 단계 수를 정한다 */
	case 39:	/* [한국어] Sv39 */
		pt_top_set_level(&table->common, 2);	/* [한국어] 3단계 */
		break;
	case 48:	/* [한국어] Sv48 */
		pt_top_set_level(&table->common, 3);	/* [한국어] 4단계 */
		break;
	case 57:	/* [한국어] Sv57 */
		pt_top_set_level(&table->common, 4);	/* [한국어] 5단계 */
		break;
	default:	/* [한국어] 명세에 없는 주소 폭 */
		return -EINVAL;	/* [한국어] 그 밖의 폭은 명세에 없다 */
	}
	table->common.max_oasz_lg2 =	/* [한국어] 출력 주소 폭은 */
		min(PT_MAX_OUTPUT_ADDRESS_LG2, cfg->common.hw_max_oasz_lg2);	/* [한국어] 형식과 하드웨어 중 작은 쪽 */
	return 0;	/* [한국어] 성공 */
}
#define pt_iommu_fmt_init riscvpt_iommu_fmt_init	/* [한국어] 공통 API 이름으로 */

/*
 * [한국어]
 * riscvpt_iommu_fmt_hw_info - 하드웨어에 적을 값을 꺼낸다
 *
 * @table: 페이지 테이블 객체.
 * @top_range: 최상위 정보를 담은 범위.
 * @info: 채울 결과.
 *
 * drivers/iommu/riscv 가 이 둘을 장치 컨텍스트의 fsc 필드에 적는다.
 * 모드 코드는 원 주석의 대응표대로 단계 수와 6 만큼 어긋나 있다.
 */
static inline void
riscvpt_iommu_fmt_hw_info(struct pt_iommu_riscv_64 *table,
			  const struct pt_range *top_range,
			  struct pt_iommu_riscv_64_hw_info *info)
{
	phys_addr_t top_phys = virt_to_phys(top_range->top_table);	/* [한국어] 최상위 표의 물리 주소 */

	info->ppn = oalog2_div(top_phys, PT_GRANULE_LG2SZ);	/* [한국어] fsc 필드는 페이지 번호로 받는다 */
	PT_WARN_ON(top_phys & ~PT_TOP_PHYS_MASK);	/* [한국어] 그 필드에 담기지 않는 비트가 있으면 안 된다 */

	/*
	 * See Table 3. Encodings of iosatp.MODE field" for DC.tx.SXL = 0:
	 *  8 = Sv39 = top level 2
	 *  9 = Sv38 = top level 3
	 *  10 = Sv57 = top level 4
	 */
	info->fsc_iosatp_mode = top_range->top_level + 6;	/* [한국어] (원 주석: iosatp.MODE 인코딩 표) 단계 수와 6 만큼 어긋나 있다 */
}
#define pt_iommu_fmt_hw_info riscvpt_iommu_fmt_hw_info	/* [한국어] 공통 API 이름으로 */

#if defined(GENERIC_PT_KUNIT)	/* [한국어] 시험 모듈을 빌드하는 중이면 */
/*
 * [한국어] kunit 이 시험할 설정 목록.
 * Sv39·Sv48·Sv57 을 모두 돌되, 가운데 하나는 Svnapot 을 꺼 두어 연속
 * 페이지가 없는 경로도 함께 검증한다.
 */
static const struct pt_iommu_riscv_64_cfg riscv_64_kunit_fmt_cfgs[] = {
	[0] = { .common.features = BIT(PT_FEAT_RISCV_SVNAPOT_64K),	/* [한국어] Sv39 에 연속 페이지를 켜고 */
		.common.hw_max_oasz_lg2 = 56,
		.common.hw_max_vasz_lg2 = 39 },
	[1] = { .common.features = 0,	/* [한국어] Sv48 은 꺼서 연속 없는 경로도 검증한다 */
		.common.hw_max_oasz_lg2 = 56,
		.common.hw_max_vasz_lg2 = 48 },
	[2] = { .common.features = BIT(PT_FEAT_RISCV_SVNAPOT_64K),	/* [한국어] Sv57 에 다시 켠다 */
		.common.hw_max_oasz_lg2 = 56,
		.common.hw_max_vasz_lg2 = 57 },
};
#define kunit_fmt_cfgs riscv_64_kunit_fmt_cfgs	/* [한국어] 시험 코드가 부르는 이름으로 */
enum {
	KUNIT_FMT_FEATURES = BIT(PT_FEAT_RISCV_SVNAPOT_64K),
	/* [한국어] KUnit 시험에서 켤 형식 기능들.
	 * 설정자: 이 상수 자체가 정의다.
	 * 읽는 자: generic_pt 의 KUnit 시험 골격이 표를 만들 때 이 값을 기능 마스크로 넘긴다.
	 * 왜 시험에서 NAPOT 을 켜는가: 연속 묶음은 이 형식에서 가장 까다로운 부분이다
	 *   — 정렬 조건, 묶음 안 항목의 일치, 묶음 경계에서의 분할이 모두 얽혀 있다.
	 *   시험에서 켜 두어야 그 경로가 실제로 검증된다.
	 * 값 범위: PT_FEAT_* 비트들의 OR. 지금은 하나뿐이다. */
};
#endif	/* [한국어] 포함 방지 끝 */

#endif
