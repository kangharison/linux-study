/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IOMMU API for ARM architected SMMU implementations.
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/*
 * [한국어 설명] ARM SMMU v1/v2 의 레지스터와 자료 구조 (arm-smmu.h)
 *
 * === 파일의 역할 ===
 * ARM 이 규격으로 낸 SMMU(System MMU) 1판과 2판의 하드웨어 인터페이스를
 * 그대로 옮겨 적은 헤더다. 레지스터 오프셋, 그 안의 비트 자리, 그리고
 * 드라이버가 그 위에 세운 자료 구조가 들어 있다.
 *
 * SMMU 의 구조를 알면 이 파일이 읽힌다. 장치가 DMA 를 내면 그 요청에
 * 스트림 id 가 붙는다. SMMU 는 그 id 를 스트림 매칭 레지스터(SMR)로
 * 받아 스트림-컨텍스트 레지스터(S2CR)를 고르고, 그것이 가리키는
 * 컨텍스트 뱅크가 실제 변환을 한다.
 *
 * 컨텍스트 뱅크 하나가 곧 주소 공간 하나다. 페이지 테이블의 기준 주소와
 * 변환 규칙을 담고 있어, CPU 의 MMU 문맥과 같은 역할을 한다. 뱅크 수가
 * 하드웨어마다 정해져 있어(최대 128) 그것이 동시에 쓸 수 있는 도메인의
 * 수를 정한다.
 *
 * 레지스터는 페이지 단위로 나뉘어 있다. 전역 설정이 첫 페이지(GR0),
 * 뱅크 속성이 둘째(GR1), 그 뒤로 뱅크마다 한 페이지씩 이어진다. 페이지
 * 크기가 4KB 인지 64KB 인지도 하드웨어가 알려 준다.
 *
 * 구현체별 차이는 struct arm_smmu_impl 의 콜백으로 흡수한다. 퀄컴과
 * NVIDIA 가 규격을 조금씩 벗어나게 만들어, 그 차이를 이 갈고리로 덮는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치 DMA → 스트림 id → SMR 매칭 → S2CR → 컨텍스트 뱅크
 *   → 페이지 테이블(io-pgtable 의 ARM LPAE) → 물리 주소
 *
 * iommu 코어 → arm-smmu.c 의 연산표 → 이 헤더의 레지스터 접근 함수
 *   → arm_smmu_impl 콜백(구현체별) → 실제 MMIO
 *
 * 실행 컨텍스트: 대부분 프로세스 문맥. 오류 처리기만 인터럽트 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * arm-smmu.c 가 주 구현이고, arm-smmu-impl.c / -qcom.c / -nvidia.c 가
 * 구현체별 갈고리를 채운다. qcom_iommu.c 는 별도 드라이버라 이 헤더를
 * 쓰지 않는다.
 * 아래로는 io-pgtable 의 ARM LPAE 구현이 표를 만든다 — SMMU 가 CPU 와
 * 같은 표 형식을 쓰기 때문에 그 코드를 그대로 나눠 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct arm_smmu_device: 한 SMMU 하드웨어. 능력과 자원 목록을 든다.
 * struct arm_smmu_domain: 한 주소 공간. 컨텍스트 뱅크 하나와 짝을 이룬다.
 * struct arm_smmu_cfg: 그 뱅크의 설정(번호, ASID/VMID, 표 형식).
 * struct arm_smmu_impl: 구현체별 차이를 흡수하는 콜백표.
 * arm_smmu_readl / writel: 구현체 갈고리를 거치는 레지스터 접근.
 * arm_smmu_lpae_tcr / vtcr: io-pgtable 의 계산 결과를 SMMU 레지스터
 *   자리로 옮겨 담는다.
 */
#ifndef _ARM_SMMU_H	/* [한국어] 중복 포함 방지 가드. */
#define _ARM_SMMU_H

#include <linux/atomic.h>	/* [한국어] 인터럽트 번호 배정에 쓰는 원자 카운터. */
#include <linux/bitfield.h>	/* [한국어] FIELD_PREP / FIELD_GET — 레지스터 필드를 마스크로 넣고 꺼낸다. */
#include <linux/bits.h>	/* [한국어] BIT / GENMASK. 아래 레지스터 정의가 거의 모두 이것으로 쓰였다. */
#include <linux/clk.h>	/* [한국어] SMMU 는 별도 클럭을 받는 경우가 있어 그것을 켜고 끈다. */
#include <linux/device.h>	/* [한국어] 플랫폼 장치 모형. */
#include <linux/io-64-nonatomic-hi-lo.h>	/* [한국어] 64비트 레지스터를 32비트씩 나눠 접근해야 하는 하드웨어를 위한 판. 상위를 먼저 쓰는 순서를 지킨다. */
#include <linux/io-pgtable.h>	/* [한국어] ARM LPAE 페이지 테이블 구현. SMMU 는 CPU 와 같은 표 형식을 쓴다. */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 도메인과 연산표. */
#include <linux/irqreturn.h>	/* [한국어] 인터럽트 처리기의 반환형. */
#include <linux/mutex.h>	/* [한국어] 스트림 매핑 표를 지키는 뮤텍스. */
#include <linux/spinlock.h>	/* [한국어] 전역 동기화와 컨텍스트 뱅크 락. */
#include <linux/types.h>	/* [한국어] 기본 타입들. */

/* Configuration registers */
#define ARM_SMMU_GR0_sCR0		0x0	/* [한국어] 전역 설정 레지스터 0. 전체 SMMU 의 동작을 정한다. GR0 은 첫 번째 전역 레지스터 페이지다. */
#define ARM_SMMU_sCR0_VMID16EN		BIT(31)	/* [한국어] VMID 를 16비트로 쓴다. 끄면 8비트라 가상 머신을 256개까지만 구별한다. */
#define ARM_SMMU_sCR0_BSU		GENMASK(15, 14)	/* [한국어] 우회(bypass) 트래픽의 공유 속성. 변환을 거치지 않는 접근에 어떤 캐시 성질을 줄지 정한다. */
#define ARM_SMMU_sCR0_FB		BIT(13)	/* [한국어] 모든 TLB 무효화를 브로드캐스트로 만든다. 여러 SMMU 가 있을 때 쓴다. */
#define ARM_SMMU_sCR0_PTM		BIT(12)	/* [한국어] 페이지 테이블 공유 브로드캐스트를 막는다. */
#define ARM_SMMU_sCR0_VMIDPNE		BIT(11)	/* [한국어] VMID 개인 이름 공간 활성화. */
#define ARM_SMMU_sCR0_USFCFG		BIT(10)	/* [한국어] 매칭되지 않은 스트림을 오류로 처리한다. 끄면 그냥 통과시켜, 등록하지 않은 장치가 자유롭게 DMA 하게 된다 — 보안상 늘 켠다. */
#define ARM_SMMU_sCR0_GCFGFIE		BIT(5)	/* [한국어] 전역 설정 오류 인터럽트를 켠다. */
#define ARM_SMMU_sCR0_GCFGFRE		BIT(4)	/* [한국어] 전역 설정 오류를 보고한다. 끄면 조용히 무시된다. */
#define ARM_SMMU_sCR0_EXIDENABLE	BIT(3)	/* [한국어] 확장 스트림 id 를 쓴다. SMR 의 마스크 자리를 id 의 상위 비트로 바꿔, 더 넓은 id 공간을 얻는다. */
#define ARM_SMMU_sCR0_GFIE		BIT(2)	/* [한국어] 전역 오류 인터럽트를 켠다. */
#define ARM_SMMU_sCR0_GFRE		BIT(1)	/* [한국어] 전역 오류를 보고한다. */
#define ARM_SMMU_sCR0_CLIENTPD		BIT(0)	/* [한국어] 클라이언트 포트를 끈다. 1 이면 SMMU 가 모든 트래픽을 그냥 통과시킨다 — 초기화 중에만 이 상태다. */

/* Auxiliary Configuration register */
#define ARM_SMMU_GR0_sACR		0x10	/* [한국어] 구현체마다 뜻이 다른 보조 설정 레지스터. 아래 impl 콜백들이 이 자리를 쓴다. */

/* Identification registers */
#define ARM_SMMU_GR0_ID0		0x20	/* [한국어] 능력 레지스터 0. 이 하드웨어가 무엇을 할 수 있는지 읽어 낸다. */
#define ARM_SMMU_ID0_S1TS		BIT(30)	/* [한국어] 1단계 변환(장치 가상 주소 → 중간 물리 주소)을 지원한다. */
#define ARM_SMMU_ID0_S2TS		BIT(29)	/* [한국어] 2단계 변환(중간 물리 → 실제 물리)을 지원한다. 가상 머신에 쓰인다. */
#define ARM_SMMU_ID0_NTS		BIT(28)	/* [한국어] 두 단계를 겹쳐 쓰는 중첩 변환을 지원한다. */
#define ARM_SMMU_ID0_SMS		BIT(27)	/* [한국어] 스트림 매칭을 지원한다. 없으면 스트림 id 가 곧 인덱스다. */
#define ARM_SMMU_ID0_ATOSNS		BIT(26)	/* [한국어] 주소 변환 연산(ATOS)을 지원하지 않는다. 부정형 비트임에 주의. */
#define ARM_SMMU_ID0_PTFS_NO_AARCH32	BIT(25)	/* [한국어] 32비트 페이지 테이블 형식을 지원하지 않는다. */
#define ARM_SMMU_ID0_PTFS_NO_AARCH32S	BIT(24)	/* [한국어] 32비트 짧은 서술자 형식을 지원하지 않는다. */
#define ARM_SMMU_ID0_NUMIRPT		GENMASK(23, 16)	/* [한국어] 문맥 오류 인터럽트의 개수. */
#define ARM_SMMU_ID0_CTTW		BIT(14)	/* [한국어] 일관성 있는 표 순회. 이것이 서면 페이지 테이블을 캐시에 두어도 하드웨어가 알아본다. */
#define ARM_SMMU_ID0_NUMSIDB		GENMASK(12, 9)	/* [한국어] 스트림 id 의 비트 수(로그 값). */
#define ARM_SMMU_ID0_EXIDS		BIT(8)	/* [한국어] 확장 스트림 id 를 지원한다. */
#define ARM_SMMU_ID0_NUMSMRG		GENMASK(7, 0)	/* [한국어] 스트림 매칭 레지스터의 개수. 이 값이 곧 동시에 구별할 수 있는 장치 무리의 수다. */

#define ARM_SMMU_GR0_ID1		0x24	/* [한국어] 능력 레지스터 1. 주로 크기와 개수를 담는다. */
#define ARM_SMMU_ID1_PAGESIZE		BIT(31)	/* [한국어] 레지스터 페이지 크기가 64KB 인가. 0 이면 4KB. */
#define ARM_SMMU_ID1_NUMPAGENDXB	GENMASK(30, 28)	/* [한국어] 레지스터 페이지 개수의 로그 값. 컨텍스트 뱅크 레지스터가 어디부터 시작하는지 계산하는 데 쓴다. */
#define ARM_SMMU_ID1_NUMS2CB		GENMASK(23, 16)	/* [한국어] 2단계 전용 컨텍스트 뱅크의 개수. */
#define ARM_SMMU_ID1_NUMCB		GENMASK(7, 0)	/* [한국어] 전체 컨텍스트 뱅크의 개수. 이것이 곧 동시에 쓸 수 있는 주소 공간의 수다. */

#define ARM_SMMU_GR0_ID2		0x28	/* [한국어] 능력 레지스터 2. 주소 폭과 페이지 크기를 담는다. */
#define ARM_SMMU_ID2_VMID16		BIT(15)	/* [한국어] 16비트 VMID 를 지원한다. */
#define ARM_SMMU_ID2_PTFS_64K		BIT(14)	/* [한국어] 64KB 페이지 테이블 형식을 지원한다. */
#define ARM_SMMU_ID2_PTFS_16K		BIT(13)	/* [한국어] 16KB 형식. */
#define ARM_SMMU_ID2_PTFS_4K		BIT(12)	/* [한국어] 4KB 형식. 셋 중 하나 이상은 반드시 있다. */
#define ARM_SMMU_ID2_UBS		GENMASK(11, 8)	/* [한국어] 상위 주소 공간 크기(부호 확장 쪽). TTBR1 을 쓸 때의 폭이다. */
#define ARM_SMMU_ID2_OAS		GENMASK(7, 4)	/* [한국어] 출력 주소 크기. 물리 주소를 몇 비트까지 낼 수 있는가. */
#define ARM_SMMU_ID2_IAS		GENMASK(3, 0)	/* [한국어] 입력 주소 크기. 장치가 낼 수 있는 주소의 폭이다. */

#define ARM_SMMU_GR0_ID3		0x2c	/* [한국어] 예약된 능력 레지스터. 이 드라이버는 읽지 않는다. */
#define ARM_SMMU_GR0_ID4		0x30	/* [한국어] 같음. */
#define ARM_SMMU_GR0_ID5		0x34	/* [한국어] 같음. */
#define ARM_SMMU_GR0_ID6		0x38	/* [한국어] 같음. */

#define ARM_SMMU_GR0_ID7		0x3c	/* [한국어] 판 번호 레지스터. */
#define ARM_SMMU_ID7_MAJOR		GENMASK(7, 4)	/* [한국어] 주 판 번호. 구현체를 가려내는 데 쓴다. */
#define ARM_SMMU_ID7_MINOR		GENMASK(3, 0)	/* [한국어] 부 판 번호. */

#define ARM_SMMU_GR0_sGFSR		0x48	/* [한국어] 전역 오류 상태. 1 을 써서 지운다(write-one-to-clear). */
#define ARM_SMMU_sGFSR_USF		BIT(1)	/* [한국어] 매칭되지 않은 스트림 오류. 등록하지 않은 장치가 DMA 를 시도했다는 뜻이다. */

#define ARM_SMMU_GR0_sGFSYNR0		0x50	/* [한국어] 전역 오류의 부가 정보 0. 어떤 종류의 접근이었는지. */
#define ARM_SMMU_GR0_sGFSYNR1		0x54	/* [한국어] 부가 정보 1. 오류를 낸 스트림 id 가 들어 있다. */
#define ARM_SMMU_GR0_sGFSYNR2		0x58	/* [한국어] 부가 정보 2. */

/* Global TLB invalidation */
#define ARM_SMMU_GR0_TLBIVMID		0x64	/* [한국어] 그 VMID 의 모든 TLB 항목을 무효화한다. */
#define ARM_SMMU_GR0_TLBIALLNSNH	0x68	/* [한국어] 비보안·비하이퍼바이저 항목을 모두 무효화한다. */
#define ARM_SMMU_GR0_TLBIALLH		0x6c	/* [한국어] 하이퍼바이저 항목을 모두 무효화한다. */
#define ARM_SMMU_GR0_sTLBGSYNC		0x70	/* [한국어] 전역 무효화의 완료를 기다리게 한다. 여기 쓰면 아래 상태가 활성이 된다. */

#define ARM_SMMU_GR0_sTLBGSTATUS	0x74	/* [한국어] 전역 무효화 진행 상태. */
#define ARM_SMMU_sTLBGSTATUS_GSACTIVE	BIT(0)	/* [한국어] 아직 진행 중. 이 비트가 내려갈 때까지 돌며 기다린다. */

/* Stream mapping registers */
#define ARM_SMMU_GR0_SMR(n)		(0x800 + ((n) << 2))	/* [한국어] 스트림 매칭 레지스터 n. 어떤 스트림 id 를 이 항목이 받을지 정한다. */
#define ARM_SMMU_SMR_VALID		BIT(31)	/* [한국어] 이 항목이 유효하다. */
#define ARM_SMMU_SMR_MASK		GENMASK(31, 16)	/* [한국어] id 에서 무시할 비트들. 여러 장치를 한 항목으로 받는 데 쓴다. EXIDS 를 켜면 이 자리가 id 의 상위 비트가 된다. */
#define ARM_SMMU_SMR_ID			GENMASK(15, 0)	/* [한국어] 받아들일 스트림 id. */

#define ARM_SMMU_GR0_S2CR(n)		(0xc00 + ((n) << 2))	/* [한국어] 스트림-컨텍스트 레지스터 n. 매칭된 스트림을 어느 컨텍스트 뱅크로 보낼지 정한다. */
#define ARM_SMMU_S2CR_PRIVCFG		GENMASK(25, 24)	/* [한국어] 특권 속성을 어떻게 다룰지. */
/* [한국어] S2CR 의 특권 속성 처리 방식. */
enum arm_smmu_s2cr_privcfg {
	/* [한국어] 장치가 보낸 특권 표시를 그대로 쓴다.
	 *  읽는 자: S2CR 을 쓰는 코드. 이 드라이버는 늘 이 값을 쓴다. */
	S2CR_PRIVCFG_DEFAULT,
	/* [한국어] 특권 접근을 권한 없는 것으로 낮춘다.
	 *  CPU 의 PAN(Privileged Access Never)에 해당하는 장치 판이다. */
	S2CR_PRIVCFG_DIPAN,
	S2CR_PRIVCFG_UNPRIV,
	/* [한국어] 그 스트림의 모든 접근을 권한 없는(unprivileged) 것으로 강제한다.
	 * 설정자: S2CR 레지스터의 PRIVCFG 필드에 쓴다. 이 드라이버는 쓰지 않는다.
	 * 읽는 자: 하드웨어가 접근 권한을 판정할 때.
	 * 위 DIPAN 과의 차이: DIPAN 은 특권 접근만 낮추고 원래 권한 없는 접근은
	 *   그대로 두는 반면, 이쪽은 장치가 무엇을 주장하든 전부 권한 없는 것으로 본다.
	 * 왜 이런 선택지가 있는가: 장치가 보내는 권한 신호를 믿을 수 없을 때, 그것을
	 *   무시하고 IOMMU 쪽에서 못 박아 버리기 위해서다. 신뢰할 수 없는 장치를
	 *   다룰 때의 안전 장치다. */
	S2CR_PRIVCFG_PRIV,
	/* [한국어] 그 스트림의 모든 접근을 특권(privileged) 접근으로 강제한다.
	 * 설정자: S2CR 의 PRIVCFG 필드. 이 드라이버는 쓰지 않는다.
	 * 읽는 자: 하드웨어.
	 * 위 UNPRIV 의 반대다. 페이지 테이블에서 특권 접근만 허용한 매핑에 장치가
	 *   닿게 하려는 경우에 쓴다.
	 * 이 enum 의 네 값 중 이 드라이버가 실제로 쓰는 것은 DEFAULT 뿐이다 —
	 *   장치가 보내는 신호를 그대로 존중하는 것이 리눅스의 기본 방침이며,
	 *   나머지 셋은 규격이 제공하는 선택지를 완전히 적어 둔 것이다. */
};
#define ARM_SMMU_S2CR_TYPE		GENMASK(17, 16)	/* [한국어] 이 스트림을 어떻게 처리할지. */
/* [한국어] 매칭된 스트림을 어떻게 다룰지. */
enum arm_smmu_s2cr_type {
	/* [한국어] 컨텍스트 뱅크로 보내 변환한다. 보통의 경우다.
	 *  설정자: 장치를 도메인에 붙일 때. */
	S2CR_TYPE_TRANS,
	/* [한국어] 변환 없이 그대로 통과시킨다.
	 *  설정자: 아직 도메인이 없거나 항등 도메인일 때.
	 *  이 상태의 장치는 물리 주소를 직접 쓰므로 보호받지 못한다. */
	S2CR_TYPE_BYPASS,
	/* [한국어] 모든 접근을 오류로 만든다.
	 *  설정자: 차단 도메인. DMA 를 막아야 할 때 쓴다. */
	S2CR_TYPE_FAULT,
};
#define ARM_SMMU_S2CR_EXIDVALID		BIT(10)	/* [한국어] 확장 스트림 id 모드에서의 유효 비트. 그 모드에서는 SMR 의 유효 비트 자리가 id 로 쓰여, 유효 표시가 이리로 옮겨 온다. */
#define ARM_SMMU_S2CR_CBNDX		GENMASK(7, 0)	/* [한국어] 보낼 컨텍스트 뱅크의 번호. */

/* Context bank attribute registers */
#define ARM_SMMU_GR1_CBAR(n)		(0x0 + ((n) << 2))	/* [한국어] 컨텍스트 뱅크 n 의 속성 레지스터. GR1 은 두 번째 전역 페이지다. */
#define ARM_SMMU_CBAR_IRPTNDX		GENMASK(31, 24)	/* [한국어] 이 뱅크의 오류를 알릴 인터럽트 번호. */
#define ARM_SMMU_CBAR_TYPE		GENMASK(17, 16)	/* [한국어] 이 뱅크가 어떤 변환을 하는지. */
/* [한국어] 컨텍스트 뱅크가 하는 변환의 종류. */
enum arm_smmu_cbar_type {
	/* [한국어] 2단계 변환만 한다.
	 *  설정자: 도메인 단계가 S2 일 때.
	 *  가상 머신에 장치를 넘길 때 쓰는 모양이다. */
	CBAR_TYPE_S2_TRANS,
	/* [한국어] 1단계만 변환하고 2단계는 통과.
	 *  가장 흔한 모양 — 호스트가 장치의 주소 공간을 관리한다. */
	CBAR_TYPE_S1_TRANS_S2_BYPASS,
	CBAR_TYPE_S1_TRANS_S2_FAULT,
	/* [한국어] 1단계는 변환하고, 그 결과가 2단계에 닿으면 모두 오류로 만든다.
	 * 설정자: CBAR 레지스터의 TYPE 필드.
	 * 읽는 자: 하드웨어.
	 * 언제 쓰는가: 2단계 변환을 설정하지 않은 채 1단계만 쓰되, 실수로 2단계를
	 *   타는 접근이 조용히 통과하지 않게 하고 싶을 때다.
	 * 위 S2_BYPASS 와의 차이가 요점이다. BYPASS 는 2단계를 그냥 통과시켜 1단계
	 *   결과가 곧 물리 주소가 되고, FAULT 는 그런 접근을 오류로 잡는다. 후자가
	 *   안전하지만 2단계를 아예 쓰지 않는 구성에서는 정상 접근까지 막는다. */
	CBAR_TYPE_S1_TRANS_S2_TRANS,
	/* [한국어] 두 단계를 모두 변환한다 — 중첩 변환이다.
	 * 설정자: CBAR 의 TYPE 필드.
	 * 읽는 자: 하드웨어.
	 * 언제 쓰는가: 가상화에서 게스트가 1단계를 관리하고 호스트가 2단계를 관리할 때.
	 *   장치의 주소가 게스트 IOVA → 게스트 물리 → 호스트 물리 순으로 두 번 옮겨진다.
	 * 비용: 워크가 두 배가 아니라 그 이상으로 늘어난다. 1단계 테이블을 읽는 각
	 *   단계의 주소도 2단계 변환을 거쳐야 하기 때문이다. 그래서 TLB 적중률이
	 *   중첩에서 특히 중요해진다.
	 * 이 enum 의 네 값이 (1단계 변환/우회) × (2단계 변환/우회/오류) 조합을 이룬다. */
};
#define ARM_SMMU_CBAR_S1_MEMATTR	GENMASK(15, 12)	/* [한국어] 1단계 우회 트래픽의 메모리 속성. */
#define ARM_SMMU_CBAR_S1_MEMATTR_WB	0xf	/* [한국어] 쓰기 되쓰기(write-back) 캐시 가능. 성능이 가장 좋은 설정이다. */
#define ARM_SMMU_CBAR_S1_BPSHCFG	GENMASK(9, 8)	/* [한국어] 1단계 우회 트래픽의 공유 속성. */
#define ARM_SMMU_CBAR_S1_BPSHCFG_NSH	3	/* [한국어] 공유하지 않음. 2단계가 속성을 정하므로 여기서는 겹치지 않게 둔다. */
#define ARM_SMMU_CBAR_VMID		GENMASK(7, 0)	/* [한국어] 이 뱅크의 VMID. 2단계 변환에서 TLB 항목을 가르는 열쇠다. */

#define ARM_SMMU_GR1_CBFRSYNRA(n)	(0x400 + ((n) << 2))	/* [한국어] 뱅크 n 의 오류 부가 정보. 오류를 낸 스트림 id 가 들어 있다. */
#define ARM_SMMU_CBFRSYNRA_SID		GENMASK(15, 0)	/* [한국어] 그 스트림 id. */

#define ARM_SMMU_GR1_CBA2R(n)		(0x800 + ((n) << 2))	/* [한국어] 뱅크 n 의 두 번째 속성 레지스터. */
#define ARM_SMMU_CBA2R_VMID16		GENMASK(31, 16)	/* [한국어] 16비트 VMID. VMID16EN 을 켰을 때 이 자리를 쓴다. */
#define ARM_SMMU_CBA2R_VA64		BIT(0)	/* [한국어] 64비트 가상 주소를 쓴다. 끄면 32비트 형식이다. */

#define ARM_SMMU_CB_SCTLR		0x0	/* [한국어] 컨텍스트 뱅크의 제어 레지스터. 아래 CB_ 접두어는 뱅크마다 따로 있는 레지스터다. */
#define ARM_SMMU_SCTLR_S1_ASIDPNE	BIT(12)	/* [한국어] ASID 개인 이름 공간. 1단계 ASID 를 뱅크마다 따로 쓴다. */
#define ARM_SMMU_SCTLR_CFCFG		BIT(7)	/* [한국어] 오류가 나면 그 트랜잭션을 멈춰 세운다. 끄면 중단시킨다 — 폴트 처리를 하려면 켜야 한다. */
#define ARM_SMMU_SCTLR_HUPCF		BIT(8)	/* [한국어] 멈춰 세운 상태에서도 특권 접근은 통과시킨다. */
#define ARM_SMMU_SCTLR_CFIE		BIT(6)	/* [한국어] 문맥 오류 인터럽트를 켠다. */
#define ARM_SMMU_SCTLR_CFRE		BIT(5)	/* [한국어] 문맥 오류를 보고한다. */
#define ARM_SMMU_SCTLR_E		BIT(4)	/* [한국어] 빅 엔디언 변환 표를 쓴다. */
#define ARM_SMMU_SCTLR_AFE		BIT(2)	/* [한국어] 접근 플래그를 쓴다. 페이지 테이블의 AF 비트를 하드웨어가 검사하게 된다. */
#define ARM_SMMU_SCTLR_TRE		BIT(1)	/* [한국어] TEX 재매핑을 쓴다. 32비트 형식의 메모리 속성 방식이다. */
#define ARM_SMMU_SCTLR_M		BIT(0)	/* [한국어] MMU 를 켠다. 이 비트가 없으면 그 뱅크는 변환하지 않는다. */

#define ARM_SMMU_CB_ACTLR		0x4	/* [한국어] 구현체마다 뜻이 다른 보조 제어 레지스터. */
#define ARM_SMMU_GFX_PRR_CFG_LADDR	0x6008	/* [한국어] 퀄컴 GPU 전용 레지스터의 하위 주소. 표준 규격에는 없다. */
#define ARM_SMMU_GFX_PRR_CFG_UADDR	0x600C	/* [한국어] 그 상위 주소. */

#define ARM_SMMU_CB_RESUME		0x8	/* [한국어] 멈춰 세운 트랜잭션을 다시 시작시킨다. */
#define ARM_SMMU_RESUME_TERMINATE	BIT(0)	/* [한국어] 다시 시도하지 말고 끝내라. 폴트에 답할 수 없을 때 쓴다. */

#define ARM_SMMU_CB_TCR2		0x10	/* [한국어] 변환 제어 레지스터의 상위 부분. */
#define ARM_SMMU_TCR2_SEP		GENMASK(17, 15)	/* [한국어] 부호 확장 경계. 상위 주소 공간이 어디서 시작하는지 정한다. */
#define ARM_SMMU_TCR2_SEP_UPSTREAM	0x7	/* [한국어] 상위 장치가 보내는 주소 폭을 그대로 쓴다. */
#define ARM_SMMU_TCR2_AS		BIT(4)	/* [한국어] 16비트 ASID 를 쓴다. */
#define ARM_SMMU_TCR2_PASIZE		GENMASK(3, 0)	/* [한국어] 물리 주소 크기. */

#define ARM_SMMU_CB_TTBR0		0x20	/* [한국어] 변환 표의 기준 주소 0. 낮은 주소 공간을 맡는다. */
#define ARM_SMMU_CB_TTBR1		0x28	/* [한국어] 기준 주소 1. 높은 주소 공간을 맡는다. */
#define ARM_SMMU_TTBRn_ASID		GENMASK_ULL(63, 48)	/* [한국어] 기준 주소 레지스터의 상위에 ASID 를 함께 담는다. 한 번의 쓰기로 표와 ASID 를 함께 바꾸기 위한 배치다. */

#define ARM_SMMU_CB_TCR			0x30	/* [한국어] 변환 제어 레지스터. 표의 모양을 정한다. */
#define ARM_SMMU_TCR_EAE		BIT(31)	/* [한국어] 확장 주소를 쓴다. 32비트 형식에서 긴 서술자를 고르는 비트다. */
#define ARM_SMMU_TCR_EPD1		BIT(23)	/* [한국어] TTBR1 쪽 표 순회를 끈다. */
#define ARM_SMMU_TCR_A1			BIT(22)	/* [한국어] ASID 를 TTBR1 에서 읽는다. 끄면 TTBR0 에서 읽는다. */
#define ARM_SMMU_TCR_TG0		GENMASK(15, 14)	/* [한국어] TTBR0 쪽 페이지 크기(4K/16K/64K). */
#define ARM_SMMU_TCR_SH0		GENMASK(13, 12)	/* [한국어] 표 순회의 공유 속성. */
#define ARM_SMMU_TCR_ORGN0		GENMASK(11, 10)	/* [한국어] 표 순회의 바깥 캐시 정책. */
#define ARM_SMMU_TCR_IRGN0		GENMASK(9, 8)	/* [한국어] 표 순회의 안쪽 캐시 정책. */
#define ARM_SMMU_TCR_EPD0		BIT(7)	/* [한국어] TTBR0 쪽 표 순회를 끈다. */
#define ARM_SMMU_TCR_T0SZ		GENMASK(5, 0)	/* [한국어] TTBR0 쪽 주소 공간의 크기. 64 에서 이 값을 뺀 만큼이 유효 비트 수다. */

#define ARM_SMMU_VTCR_RES1		BIT(31)	/* [한국어] 2단계 변환 제어의 예약 비트. 반드시 1 이어야 한다. */
#define ARM_SMMU_VTCR_PS		GENMASK(18, 16)	/* [한국어] 2단계 물리 주소 크기. */
#define ARM_SMMU_VTCR_TG0		ARM_SMMU_TCR_TG0	/* [한국어] 2단계 페이지 크기. 1단계와 자리가 같아 그대로 쓴다. */
#define ARM_SMMU_VTCR_SH0		ARM_SMMU_TCR_SH0	/* [한국어] 2단계 공유 속성. */
#define ARM_SMMU_VTCR_ORGN0		ARM_SMMU_TCR_ORGN0	/* [한국어] 2단계 바깥 캐시 정책. */
#define ARM_SMMU_VTCR_IRGN0		ARM_SMMU_TCR_IRGN0	/* [한국어] 2단계 안쪽 캐시 정책. */
#define ARM_SMMU_VTCR_SL0		GENMASK(7, 6)	/* [한국어] 2단계 시작 단계. 표를 몇 단계부터 타는지 정한다. */
#define ARM_SMMU_VTCR_T0SZ		ARM_SMMU_TCR_T0SZ	/* [한국어] 2단계 주소 공간 크기. */

#define ARM_SMMU_CB_CONTEXTIDR		0x34	/* [한국어] 문맥 식별자. 32비트 형식에서 ASID 가 여기 들어간다. */
#define ARM_SMMU_CB_S1_MAIR0		0x38	/* [한국어] 메모리 속성 표 0. 페이지 테이블의 속성 번호를 실제 캐시 성질로 옮긴다. */
#define ARM_SMMU_CB_S1_MAIR1		0x3c	/* [한국어] 속성 표 1. 번호 4~7 을 맡는다. */

#define ARM_SMMU_CB_PAR			0x50	/* [한국어] 주소 변환 연산의 결과가 여기 나온다. */
#define ARM_SMMU_CB_PAR_F		BIT(0)	/* [한국어] 변환이 실패했다. */

#define ARM_SMMU_CB_FSR			0x58	/* [한국어] 문맥 오류 상태. 1 을 써서 지운다. */
#define ARM_SMMU_CB_FSR_MULTI		BIT(31)	/* [한국어] 여러 오류가 겹쳤다. 앞의 오류를 지우기 전에 새 오류가 났다는 뜻. */
#define ARM_SMMU_CB_FSR_SS		BIT(30)	/* [한국어] 트랜잭션이 멈춰 서 있다. RESUME 을 써야 진행된다. */
#define ARM_SMMU_CB_FSR_FORMAT		GENMASK(10, 9)	/* [한국어] 오류 정보의 형식. */
#define ARM_SMMU_CB_FSR_UUT		BIT(8)	/* [한국어] 예상치 못한 미사용 트랜잭션. */
#define ARM_SMMU_CB_FSR_ASF		BIT(7)	/* [한국어] 접근 플래그 오류. 페이지 테이블의 AF 비트가 0 이었다. */
#define ARM_SMMU_CB_FSR_TLBLKF		BIT(6)	/* [한국어] TLB 잠금 오류. */
#define ARM_SMMU_CB_FSR_TLBMCF		BIT(5)	/* [한국어] TLB 다중 일치 오류. 같은 주소에 두 항목이 맞았다 — 무효화를 빠뜨렸을 때 난다. */
#define ARM_SMMU_CB_FSR_EF		BIT(4)	/* [한국어] 외부 오류. 표를 읽다 버스 오류가 났다. */
#define ARM_SMMU_CB_FSR_PF		BIT(3)	/* [한국어] 권한 오류. 매핑은 있지만 그 접근이 허용되지 않았다. */
#define ARM_SMMU_CB_FSR_AFF		BIT(2)	/* [한국어] 주소 크기 오류. */
#define ARM_SMMU_CB_FSR_TF		BIT(1)	/* [한국어] 변환 오류. 매핑이 아예 없다 — 가장 흔한 오류다. */

/* [한국어] 로그에 자세히 남기지 않고 넘어가는 오류들. 드물거나 스스로 회복되는 종류다. */
#define ARM_SMMU_CB_FSR_IGN		(ARM_SMMU_CB_FSR_AFF |		\
					 ARM_SMMU_CB_FSR_ASF |		\
					 ARM_SMMU_CB_FSR_TLBMCF |	\
					 ARM_SMMU_CB_FSR_TLBLKF)	/* [한국어] TLB 잠금 오류까지가 넘어가는 종류다. */

/* [한국어] 오류로 다뤄야 하는 비트 전부. 상태 레지스터에서 이 비트들만 본다. */
#define ARM_SMMU_CB_FSR_FAULT		(ARM_SMMU_CB_FSR_MULTI |	\
					 ARM_SMMU_CB_FSR_SS |		\
					 ARM_SMMU_CB_FSR_UUT |		\
					 ARM_SMMU_CB_FSR_EF |		\
					 ARM_SMMU_CB_FSR_PF |		\
					 ARM_SMMU_CB_FSR_TF |		\
					 ARM_SMMU_CB_FSR_IGN)	/* [한국어] 위의 넘어가는 종류들도 오류로는 센다 — 상태를 지워야 하기 때문이다. */

#define ARM_SMMU_CB_FAR			0x60	/* [한국어] 오류가 난 주소. */

#define ARM_SMMU_CB_FSYNR0		0x68	/* [한국어] 오류의 부가 정보. */
#define ARM_SMMU_CB_FSYNR0_PLVL		GENMASK(1, 0)	/* [한국어] 오류가 난 표의 단계. */
#define ARM_SMMU_CB_FSYNR0_WNR		BIT(4)	/* [한국어] 쓰기였다. 0 이면 읽기. */
#define ARM_SMMU_CB_FSYNR0_PNU		BIT(5)	/* [한국어] 특권 없는 접근이었다. */
#define ARM_SMMU_CB_FSYNR0_IND		BIT(6)	/* [한국어] 명령 인출이었다. */
#define ARM_SMMU_CB_FSYNR0_NSATTR	BIT(8)	/* [한국어] 비보안 속성이었다. */
#define ARM_SMMU_CB_FSYNR0_PTWF		BIT(10)	/* [한국어] 표를 순회하다 난 오류다. 장치의 접근 자체가 아니라 표 읽기가 실패했다는 뜻. */
#define ARM_SMMU_CB_FSYNR0_AFR		BIT(11)	/* [한국어] 접근 플래그 관련 오류. */
#define ARM_SMMU_CB_FSYNR0_S1CBNDX	GENMASK(23, 16)	/* [한국어] 중첩 변환에서 1단계 뱅크의 번호. */

#define ARM_SMMU_CB_FSYNR1		0x6c	/* [한국어] 부가 정보 1. 구현체마다 뜻이 다르다. */

#define ARM_SMMU_CB_S1_TLBIVA		0x600	/* [한국어] 1단계 TLB 를 주소로 무효화한다. */
#define ARM_SMMU_CB_S1_TLBIASID		0x610	/* [한국어] 1단계 TLB 를 ASID 통째로 무효화한다. */
#define ARM_SMMU_CB_S1_TLBIVAL		0x620	/* [한국어] 1단계 TLB 를 주소로 무효화하되 마지막 단계만. */
#define ARM_SMMU_CB_S2_TLBIIPAS2	0x630	/* [한국어] 2단계 TLB 를 중간 물리 주소로 무효화한다. */
#define ARM_SMMU_CB_S2_TLBIIPAS2L	0x638	/* [한국어] 같되 마지막 단계만. */
#define ARM_SMMU_CB_TLBSYNC		0x7f0	/* [한국어] 이 뱅크의 무효화 완료를 기다리게 한다. */
#define ARM_SMMU_CB_TLBSTATUS		0x7f4	/* [한국어] 그 진행 상태. */
#define ARM_SMMU_CB_ATS1PR		0x800	/* [한국어] 1단계 특권 읽기 주소 변환을 시킨다. iova_to_phys 가 이것을 쓴다. */

#define ARM_SMMU_CB_ATSR		0x8f0	/* [한국어] 주소 변환 연산의 상태. */
#define ARM_SMMU_CB_ATSR_ACTIVE		BIT(0)	/* [한국어] 아직 진행 중. */

#define ARM_SMMU_RESUME_TERMINATE	BIT(0)	/* [한국어] 위와 같은 정의가 한 번 더 나온다. 값이 같아 문제는 없지만 중복이다. */

/* Maximum number of context banks per SMMU */
#define ARM_SMMU_MAX_CBS		128	/* [한국어] 컨텍스트 뱅크의 최대 개수. 규격이 정한 상한이라 비트맵을 정적으로 잡는다. */

#define TLB_LOOP_TIMEOUT		1000000	/* 1s! */	/* [한국어] 무효화 완료를 기다리는 최대 시간(마이크로초). 주석대로 1초다 — 그만큼 걸리면 하드웨어가 고장 난 것이다. */
#define TLB_SPIN_COUNT			10	/* [한국어] 잠들기 전에 돌며 기다릴 횟수. 대개 곧 끝나므로 먼저 돌아 본다. */

/* Shared driver definitions */
/* [한국어] SMMU 규격 판. 판마다 레지스터 배치와 능력이 다르다. */
enum arm_smmu_arch_version {
	/* [한국어] 1판. 4KB 레지스터 페이지를 쓴다.
	 *  설정자: 장치 트리의 compatible 문자열.
	 *  읽는 자: 판마다 다른 초기화 경로를 고르는 곳. */
	ARM_SMMU_V1,
	/* [한국어] 1판이되 64KB 레지스터 페이지를 쓰는 변종.
	 *  레지스터 배치는 1판이지만 페이지 크기가 달라 따로 둔다. */
	ARM_SMMU_V1_64K,
	ARM_SMMU_V2,
	/* [한국어] SMMU 아키텍처 2판 — 오늘날 대부분의 하드웨어가 이것이다.
	 * 설정자: 프로브가 IDR7 의 판 번호를 읽어 정한다.
	 * 읽는 자: 판마다 다른 초기화 경로와 레지스터 배치를 고르는 곳.
	 * 1판과의 차이: 2판은 컨텍스트 뱅크와 스트림 매칭의 구조가 정리되었고,
	 *   64비트 레지스터 접근이 규격화되었다. 1판 지원 코드가 남아 있는 것은
	 *   아직 쓰이는 하드웨어가 있기 때문이다.
	 * 값 범위: 이 enum 의 마지막 값. SMMUv3 는 레지스터 배치가 완전히 달라
	 *   별도의 드라이버(arm-smmu-v3)가 다루므로 여기 없다. */
};

/* [한국어] 어느 회사의 구현인가. 규격을 벗어난 부분을 가려내는 데 쓴다. */
enum arm_smmu_implementation {
	/* [한국어] 규격 그대로인 구현.
	 *  읽는 자: 구현체 갈고리를 고르는 arm_smmu_impl_init. */
	GENERIC_SMMU,
	ARM_MMU500,
	/* [한국어] ARM 의 MMU-500 구현 — 알려진 문제를 우회하는 리셋 절차가 필요하다.
	 * 설정자: 프로브가 구현 식별 레지스터를 읽어 정한다.
	 * 읽는 자: arm_smmu_impl_init() 이 이 값을 보고 전용 갈고리표를 꽂는다.
	 * 어떤 우회가 필요한가: 리셋 뒤 일부 레지스터가 규격이 정한 초기값을 갖지
	 *   않아, 드라이버가 명시적으로 다시 써야 한다. 그러지 않으면 프리페치가
	 *   잘못 동작해 성능이 떨어지거나 간헐적 오류가 난다.
	 * 이 enum 이 존재하는 이유가 그것이다 — 규격을 벗어난 부분을 구현별로 가려낸다. */
	CAVIUM_SMMUV2,
	/* [한국어] Cavium 의 2판 구현 — 컨텍스트 뱅크 배정 방식이 다르다.
	 * 설정자: 프로브의 구현 식별.
	 * 읽는 자: 컨텍스트 뱅크를 배정하는 경로가 이 값을 보고 다른 규칙을 쓴다.
	 * 무엇이 다른가: 이 하드웨어에서는 뱅크 번호와 ASID 사이에 고정된 관계가
	 *   있어, 뱅크를 자유롭게 고를 수 없다. 공통 코드의 "빈 뱅크 아무거나"
	 *   방식을 그대로 쓰면 ASID 가 충돌한다.
	 * 이런 차이는 능력 레지스터로 알 수 없어, 구현을 식별해 갈래를 나눌 수밖에 없다. */
	QCOM_SMMUV2,
	/* [한국어] 퀄컴의 2판 구현 — 펌웨어가 미리 설정해 둔 상태를 이어받아야 한다.
	 * 설정자: 프로브의 구현 식별.
	 * 읽는 자: 퀄컴 전용 갈고리표(arm-smmu-qcom.c)를 꽂는 곳.
	 * 왜 이어받아야 하는가: 부팅 중 펌웨어가 이미 디스플레이 컨트롤러 등에
	 *   매핑을 만들어 화면을 띄워 두었다. 드라이버가 SMMU 를 리셋하면 그 매핑이
	 *   사라져 화면이 꺼지거나 장치가 오류를 낸다. 그래서 기존 스트림 매핑을
	 *   읽어 그대로 물려받은 뒤에야 자기 설정을 얹는다.
	 * 이 enum 의 마지막 값이며, 새 구현이 추가되면 여기 이어 붙는다. */
};

/* [한국어] 스트림-컨텍스트 레지스터 하나의 그림자 상태.
 *
 * 하드웨어 레지스터는 읽기가 느리고 일부 구현은 되읽기가 부정확해,
 * 드라이버가 쓴 값을 메모리에 함께 들고 있는다. */
struct arm_smmu_s2cr {
	/* [한국어] 이 항목을 쓰는 iommu 그룹.
	 *  설정자·읽는 자: 장치 붙이기·떼기 경로.
	 *  같은 SMR 항목에 매칭되는 장치들은 서로 구별되지 않아 반드시 한 그룹이다. */
	struct iommu_group		*group;
	/* [한국어] 이 항목을 함께 쓰는 장치 수.
	 *  0 이 되면 항목을 놓아 다른 장치가 쓸 수 있게 한다. */
	int				count;
	/* [한국어] 변환·우회·오류 중 무엇으로 다룰지.
	 *  설정자: 붙이는 도메인의 종류에 따라 정해진다. */
	enum arm_smmu_s2cr_type		type;
	/* [한국어] 특권 속성 처리 방식.
	 *  이 드라이버는 늘 기본값을 쓴다. */
	enum arm_smmu_s2cr_privcfg	privcfg;
	/* [한국어] 보낼 컨텍스트 뱅크의 번호.
	 *  변환 종류일 때만 뜻이 있다. */
	u8				cbndx;
};

/* [한국어] 스트림 매칭 레지스터 하나의 그림자 상태.
 *
 * 어떤 스트림 id 를 이 항목이 받을지 정한다. 마스크로 여러 id 를 한
 * 항목에 묶을 수 있고, 그렇게 묶인 장치들은 한 그룹이 된다. */
struct arm_smmu_smr {
	/* [한국어] id 에서 무시할 비트들.
	 *  설정자: 장치 트리나 펌웨어가 알려 준 값.
	 *  확장 id 모드에서는 이 자리가 id 의 상위 비트로 쓰인다. */
	u16				mask;
	/* [한국어] 받아들일 스트림 id.
	 *  장치가 DMA 를 낼 때 붙이는 번호로, 버스 위의 장치를 가리킨다. */
	u16				id;
	/* [한국어] 이 항목이 쓰이고 있는가.
	 *  설정자: 항목을 배정하고 놓는 코드. */
	bool				valid;
	/* [한국어] 부트로더가 미리 설정해 두어 건드리면 안 되는 항목인가.
	 *  화면 출력처럼 부팅 중에도 계속 DMA 해야 하는 장치가 있어,
	 *  그 설정을 커널이 이어받아 쓰다가 나중에 놓는다. */
	bool				pinned;
};

/* [한국어] SMMU 하드웨어 하나.
 *
 * 능력 레지스터에서 읽어 낸 성질과, 그것에 맞춰 잡은 자원 목록을 든다.
 * 한 시스템에 SMMU 가 여럿일 수 있고 각자 이 구조체를 하나씩 갖는다. */
struct arm_smmu_device {
	/* [한국어] 이 SMMU 의 플랫폼 장치.
	 *  읽는 자: 로그 출력과 메모리 할당(어느 노드에서 잡을지). */
	struct device			*dev;

	/* [한국어] 레지스터 창의 가상 주소.
	 *  설정자: probe 가 ioremap 한 결과.
	 *  읽는 자: arm_smmu_page 가 페이지 번호를 더해 실제 주소를 만든다. */
	void __iomem			*base;
	/* [한국어] 그 창의 물리 주소.
	 *  읽는 자: sysfs 에 보이는 이름을 짓는 데 쓴다. */
	phys_addr_t			ioaddr;
	/* [한국어] 전역 레지스터 페이지의 수.
	 *  컨텍스트 뱅크 페이지가 이 뒤부터 시작한다.
	 *  설정자: ID1 레지스터에서 계산한다. */
	unsigned int			numpage;
	/* [한국어] 레지스터 페이지 크기의 로그 값(12 또는 16).
	 *  설정자: ID1 의 PAGESIZE 비트.
	 *  읽는 자: 페이지 번호를 주소로 바꾸는 모든 곳. */
	unsigned int			pgshift;

#define ARM_SMMU_FEAT_COHERENT_WALK	(1 << 0)	/* [한국어] 표 순회가 캐시 일관성을 갖는다. 페이지 테이블을 쓴 뒤 캐시를 비울 필요가 없다. */
#define ARM_SMMU_FEAT_STREAM_MATCH	(1 << 1)	/* [한국어] 스트림 매칭 레지스터가 있다. 없으면 스트림 id 가 곧 인덱스라 유연성이 없다. */
#define ARM_SMMU_FEAT_TRANS_S1		(1 << 2)	/* [한국어] 1단계 변환을 할 수 있다. */
#define ARM_SMMU_FEAT_TRANS_S2		(1 << 3)	/* [한국어] 2단계 변환을 할 수 있다. */
#define ARM_SMMU_FEAT_TRANS_NESTED	(1 << 4)	/* [한국어] 두 단계를 겹쳐 쓸 수 있다. */
#define ARM_SMMU_FEAT_TRANS_OPS		(1 << 5)	/* [한국어] 주소 변환 연산(ATOS)을 할 수 있다. iova_to_phys 를 하드웨어에 물어볼 수 있다는 뜻. */
#define ARM_SMMU_FEAT_VMID16		(1 << 6)	/* [한국어] 16비트 VMID 를 쓸 수 있다. */
#define ARM_SMMU_FEAT_FMT_AARCH64_4K	(1 << 7)	/* [한국어] 64비트 4KB 페이지 형식. */
#define ARM_SMMU_FEAT_FMT_AARCH64_16K	(1 << 8)	/* [한국어] 64비트 16KB 형식. */
#define ARM_SMMU_FEAT_FMT_AARCH64_64K	(1 << 9)	/* [한국어] 64비트 64KB 형식. */
#define ARM_SMMU_FEAT_FMT_AARCH32_L	(1 << 10)	/* [한국어] 32비트 긴 서술자 형식. */
#define ARM_SMMU_FEAT_FMT_AARCH32_S	(1 << 11)	/* [한국어] 32비트 짧은 서술자 형식. */
#define ARM_SMMU_FEAT_EXIDS		(1 << 12)	/* [한국어] 확장 스트림 id 를 쓸 수 있다. */
	/* [한국어] 위 ARM_SMMU_FEAT_* 비트의 모음.
	 *  설정자: 능력 레지스터를 읽어 채운다.
	 *  읽는 자: 도메인을 만들 때 쓸 수 있는 형식과 단계를 고른다. */
	u32				features;

	/* [한국어] 규격 판.
	 *  설정자: 장치 트리의 compatible. */
	enum arm_smmu_arch_version	version;
	/* [한국어] 구현체 종류.
	 *  설정자: 같은 곳. 읽는 자: 갈고리를 고르는 코드. */
	enum arm_smmu_implementation	model;
	/* [한국어] 구현체별 갈고리표(없을 수 있다).
	 *  설정자: arm_smmu_impl_init.
	 *  읽는 자: 레지스터 접근과 초기화의 거의 모든 곳. */
	const struct arm_smmu_impl	*impl;

	/* [한국어] 컨텍스트 뱅크의 총 개수. 동시에 쓸 수 있는 주소 공간의 수다.
	 *  설정자: ID1 레지스터. */
	u32				num_context_banks;
	/* [한국어] 2단계 전용 뱅크의 개수.
	 *  그 뱅크들은 앞쪽에 몰려 있어, 1단계 도메인은 그 뒤에서 배정한다. */
	u32				num_s2_context_banks;
	/* [한국어] 어느 뱅크가 쓰이는지의 비트맵.
	 *  설정자·읽는 자: 뱅크 배정과 반납.
	 *  동기화: 원자적 test_and_set 으로 다룬다. */
	DECLARE_BITMAP(context_map, ARM_SMMU_MAX_CBS);
	/* [한국어] 뱅크마다의 설정 그림자.
	 *  전원이 꺼졌다 돌아왔을 때 다시 써 넣기 위해 들고 있는다. */
	struct arm_smmu_cb		*cbs;
	/* [한국어] 다음에 배정할 인터럽트 번호.
	 *  뱅크보다 인터럽트가 적을 수 있어 돌려 가며 쓴다. */
	atomic_t			irptndx;

	/* [한국어] 스트림 매핑 항목(SMR/S2CR)의 개수.
	 *  설정자: ID0 의 NUMSMRG. */
	u32				num_mapping_groups;
	/* [한국어] 유효한 스트림 id 비트의 마스크.
	 *  설정자: ID0 의 NUMSIDB 로 계산.
	 *  이 범위를 넘는 id 를 요구하면 거절한다. */
	u16				streamid_mask;
	/* [한국어] SMR 마스크에서 실제로 쓸 수 있는 비트.
	 *  설정자: 마스크 레지스터에 전부 1 을 써 보고 되읽어 알아낸다 —
	 *  규격이 알려 주지 않아 실험으로 알아내는 값이다. */
	u16				smr_mask_mask;
	/* [한국어] SMR 그림자 배열.
	 *  스트림 매칭을 지원하지 않는 하드웨어에서는 NULL 이다. */
	struct arm_smmu_smr		*smrs;
	/* [한국어] S2CR 그림자 배열.
	 *  SMR 과 짝을 이뤄 같은 첨자로 다뤄진다. */
	struct arm_smmu_s2cr		*s2crs;
	/* [한국어] 위 두 배열과 그 배정을 지키는 뮤텍스.
	 *  장치를 붙이고 뗄 때만 잡히므로 경합이 드물다. */
	struct mutex			stream_map_mutex;

	/* [한국어] 입력(가상) 주소의 최대 폭.
	 *  설정자: ID2 의 IAS. 읽는 자: 도메인의 주소 범위를 정할 때. */
	unsigned long			va_size;
	/* [한국어] 중간 물리 주소의 폭. 1단계의 출력이자 2단계의 입력이다. */
	unsigned long			ipa_size;
	/* [한국어] 출력(물리) 주소의 폭.
	 *  설정자: ID2 의 OAS. */
	unsigned long			pa_size;
	/* [한국어] 쓸 수 있는 페이지 크기의 비트맵.
	 *  설정자: ID2 의 PTFS 비트들.
	 *  읽는 자: iommu 코어가 매핑을 쪼갤 단위를 고를 때. */
	unsigned long			pgsize_bitmap;

	/* [한국어] 문맥 오류 인터럽트의 개수.
	 *  뱅크마다 하나씩 있는 것이 이상적이지만 더 적을 수 있다. */
	int				num_context_irqs;
	/* [한국어] 이 SMMU 가 받는 클럭의 개수. */
	int				num_clks;
	/* [한국어] 인터럽트 번호 배열.
	 *  첫 번째가 전역 오류, 나머지가 문맥 오류다. */
	unsigned int			*irqs;
	/* [한국어] 클럭 목록. 한 번에 켜고 끈다. */
	struct clk_bulk_data		*clks;

	/* [한국어] 전역 TLB 무효화를 직렬화하는 스핀락.
	 *  무효화와 그 완료 대기가 한 쌍이라, 겹치면 남의 완료를 자기 것으로
	 *  오해하게 된다. */
	spinlock_t			global_sync_lock;

	/* IOMMU core code handle */
	/* [한국어] iommu 코어에 등록하는 손잡이.
	 *  읽는 자: 코어가 이것으로 드라이버를 되짚는다. */
	struct iommu_device		iommu;
};

/* [한국어] 컨텍스트 뱅크가 쓸 페이지 테이블 형식. */
enum arm_smmu_context_fmt {
	ARM_SMMU_CTX_FMT_NONE,
	/* [한국어] 페이지 테이블 형식을 아직 정하지 않았다 — 능력에 맞춰 고르라는 뜻이다.
	 * 설정자: 컨텍스트 설정 구조체를 0 으로 초기화하면 이 값이 된다.
	 * 읽는 자: arm_smmu_init_domain_context() 가 이 값을 보면 하드웨어 능력과
	 *   커널 구성을 보고 아래 셋 중 하나를 스스로 고른다.
	 * 왜 0 이 이 뜻인가: 초기화되지 않은 상태가 곧 "알아서 정하라"가 되도록
	 *   맞춘 것이다. 명시적으로 형식을 지정해야 하는 경우는 드물다.
	 * 값 범위: 이 값이 도메인 설정에 남아 있으면 안 된다 — 도메인을 붙일 때
	 *   반드시 구체적인 형식으로 바뀐다. */
	ARM_SMMU_CTX_FMT_AARCH64,
	/* [한국어] 32비트 긴 서술자(LPAE) 형식.
	 *  32비트 커널이 40비트 물리 주소를 쓸 때의 형식이다. */
	ARM_SMMU_CTX_FMT_AARCH32_L,
	ARM_SMMU_CTX_FMT_AARCH32_S,
	/* [한국어] 32비트 짧은 서술자(short descriptor) 형식 — 가장 오래된 방식이다.
	 * 설정자: 하드웨어가 긴 서술자를 지원하지 않거나, 커널이 그렇게 구성된 경우.
	 * 읽는 자: io-pgtable 을 만들 때 어느 형식 구현을 쓸지 정하는 곳
	 *   (io-pgtable-arm-v7s.c 가 이 형식을 다룬다).
	 * 한계: 물리 주소가 32비트를 넘지 못하고, 페이지 크기 조합도 제한적이다.
	 *   위 AARCH32_L(LPAE)이 40비트 물리 주소를 다룰 수 있는 것과 대비된다.
	 * 왜 아직 남아 있는가: 이 형식만 지원하는 오래된 SMMU 하드웨어가 있고,
	 *   그런 시스템에서는 다른 선택지가 없다. */
};

/* [한국어] 한 컨텍스트 뱅크의 설정.
 *
 * 도메인 하나가 이것을 하나 들고, 그것이 곧 하드웨어 뱅크 하나에
 * 대응한다. */
struct arm_smmu_cfg {
	/* [한국어] 배정받은 뱅크 번호.
	 *  설정자: 도메인을 처음 붙일 때 비트맵에서 잡는다.
	 *  읽는 자: 그 뱅크의 레지스터를 건드리는 모든 곳. */
	u8				cbndx;
	/* [한국어] 배정받은 인터럽트 번호.
	 *  값 범위: ARM_SMMU_INVALID_IRPTNDX 면 없음. */
	u8				irptndx;
	/* [한국어] 1단계면 ASID, 2단계면 VMID. 둘 다 TLB 항목을 가르는 열쇠라
	 *  한 자리를 나눠 쓴다. */
	union {
		u16			asid;
		/* [한국어] (1단계) 주소 공간 식별자 — TLB 항목을 도메인별로 가르는 열쇠다.
		 * 설정자: 도메인을 처음 붙일 때 ASID 할당기에서 받는다.
		 * 읽는 자: 컨텍스트 뱅크 레지스터(TTBR0 의 ASID 필드)에 실리고, ASID 단위
		 *   TLB 무효화 명령에 실린다.
		 * 왜 필요한가: 여러 도메인의 항목이 같은 TLB 에 섞여 있다. ASID 가 없으면
		 *   한 도메인의 매핑을 바꿀 때마다 TLB 를 통째로 비워야 해서, 다른 도메인의
		 *   성능까지 떨어진다.
		 * 아래 vmid 와 union 인 이유: 한 뱅크는 1단계이거나 2단계이지 둘 다일 수
		 *   없고, 두 값 모두 "TLB 항목을 가르는 열쇠"라는 같은 역할을 한다.
		 * 값 범위: 하드웨어의 ASID 비트 수에 따라 8비트 또는 16비트. */
		u16			vmid;
		/* [한국어] (2단계) 가상 머신 식별자 — 2단계 TLB 항목을 가르는 열쇠다.
		 * 설정자: 2단계 도메인을 처음 붙일 때 VMID 할당기에서 받는다.
		 * 읽는 자: 컨텍스트 뱅크 레지스터(VTTBR 의 VMID 필드)와 VMID 단위 무효화 명령.
		 * 위 asid 와 같은 자리를 나눠 쓰며, 이 뱅크가 어느 단계인지는 cbar 필드가 말해 준다.
		 * ASID 와 나뉘어 있는 이유: 두 단계의 TLB 항목이 서로 다른 이름 공간을 쓴다.
		 *   같은 번호의 ASID 와 VMID 가 있어도 충돌하지 않으며, 무효화 명령도 따로다.
		 * 값 범위: 하드웨어의 VMID 비트 수에 따라 8비트 또는 16비트. */
	};
	/* [한국어] 이 뱅크가 하는 변환의 종류.
	 *  설정자: 도메인 단계에 따라 정해진다. */
	enum arm_smmu_cbar_type		cbar;
	/* [한국어] 쓸 페이지 테이블 형식.
	 *  설정자: 하드웨어 능력과 커널 설정을 견줘 고른다. */
	enum arm_smmu_context_fmt	fmt;
	/* [한국어] 구간 무효화 대신 ASID 통째 무효화를 선호하는가.
	 *  설정자: 구현체 갈고리. 어떤 하드웨어는 구간 무효화가 매우 느리다. */
	bool				flush_walk_prefer_tlbiasid;
};
#define ARM_SMMU_INVALID_IRPTNDX	0xff	/* [한국어] 인터럽트 번호가 없음을 뜻하는 값. 0 은 유효한 번호라 쓸 수 없다. */

struct arm_smmu_cb {
	/* [한국어] 변환 표 기준 주소 두 개(낮은·높은 주소 공간).
	 *  설정자: 도메인을 만들 때 io-pgtable 이 알려 준 값.
	 *  읽는 자: 뱅크를 하드웨어에 써 넣을 때. */
	u64				ttbr[2];
	/* [한국어] 변환 제어 값. 표의 모양을 정한다.
	 *  두 칸인 것은 TCR 과 TCR2 를 함께 담기 때문이다. */
	u32				tcr[2];
	/* [한국어] 메모리 속성 표. 페이지 테이블의 속성 번호를 캐시 성질로 옮긴다. */
	u32				mair[2];
	/* [한국어] 이 뱅크를 쓰는 도메인의 설정.
	 *  값 범위: NULL 이면 쓰이지 않는 뱅크다. */
	struct arm_smmu_cfg		*cfg;
};

/* [한국어] 도메인이 어느 변환 단계를 쓰는가. */
enum arm_smmu_domain_stage {
	/* [한국어] 1단계만. 호스트가 장치의 주소 공간을 관리한다.
	 *  0 인 것은 지정하지 않았을 때의 기본값이 되게 하려는 것이다. */
	ARM_SMMU_DOMAIN_S1 = 0,
	/* [한국어] 2단계만. 가상 머신에 장치를 넘길 때 쓴다. */
	ARM_SMMU_DOMAIN_S2,
	/* [한국어] 두 단계를 겹쳐 쓴다.
	 *  설정자: 사용자가 요청하면. 하드웨어가 지원해야 한다. */
	ARM_SMMU_DOMAIN_NESTED,
};

/* [한국어] 한 주소 공간.
 *
 * iommu 코어의 도메인을 감싸, 그것이 어느 SMMU 의 어느 컨텍스트 뱅크로
 * 실현되는지를 담는다. */
struct arm_smmu_domain {
	/* [한국어] 이 도메인이 매인 SMMU.
	 *  값 범위: NULL 이면 아직 어느 장치도 붙지 않았다는 뜻 — 그때는
	 *  하드웨어 자원을 잡지 않는다.
	 *  동기화: 아래 init_mutex 가 지킨다. */
	struct arm_smmu_device		*smmu;
	/* [한국어] 페이지 테이블 조작 함수들.
	 *  설정자: 첫 장치를 붙일 때 io-pgtable 이 만들어 준다.
	 *  읽는 자: 매핑·해제 경로. */
	struct io_pgtable_ops		*pgtbl_ops;
	/* [한국어] 페이지 테이블 구현에 줄 예외 표시.
	 *  설정자: 구현체 갈고리나 도메인 속성. */
	unsigned long			pgtbl_quirks;
	/* [한국어] TLB 무효화 함수들.
	 *  단계와 하드웨어에 따라 다른 표를 고른다. */
	const struct iommu_flush_ops	*flush_ops;
	/* [한국어] 이 도메인이 쓰는 컨텍스트 뱅크의 설정. */
	struct arm_smmu_cfg		cfg;
	/* [한국어] 쓰는 변환 단계.
	 *  설정자: 도메인 속성이나 기본값. */
	enum arm_smmu_domain_stage	stage;
	/* [한국어] (위 영어 주석 참고) smmu 포인터를 지킨다.
	 *  도메인을 처음 붙일 때만 그 값이 정해지므로, 그 한 번을 직렬화한다. */
	struct mutex			init_mutex; /* Protects smmu pointer */
	/* [한국어] (위 영어 주석 참고) 주소 변환 연산과 TLB 동기화를 직렬화한다.
	 *  둘 다 "명령을 쓰고 결과를 읽는" 두 걸음이라, 겹치면 남의 결과를
	 *  자기 것으로 읽게 된다. */
	spinlock_t			cb_lock; /* Serialises ATS1* ops and TLB syncs */
	/* [한국어] iommu 코어가 보는 도메인.
	 *  이 위치가 마지막인 것은 관례일 뿐, container_of 로 되짚는다. */
	struct iommu_domain		domain;
};

/* [한국어] 장치 하나가 쓰는 스트림 매핑 항목들.
 *
 * 장치마다 스트림 id 가 여럿일 수 있어(PCI 함수 여럿 등), 그 각각이
 * 어느 SMR 항목을 쓰는지 기억해 둔다. */
struct arm_smmu_master_cfg {
	/* [한국어] 이 장치가 매인 SMMU.
	 *  읽는 자: 장치에서 SMMU 로 거슬러 올라가는 모든 곳. */
	struct arm_smmu_device		*smmu;
	/* [한국어] 스트림 id 마다 배정된 매핑 항목의 번호.
	 *  값 범위: INVALID_SMENDX(-1) 면 아직 배정되지 않았다.
	 *  부호 있는 타입인 이유가 그것이다.
	 *  가변 길이 배열이라 장치를 만들 때 id 수만큼 함께 잡는다. */
	s16				smendx[];
};

/*
 * [한국어]
 * arm_smmu_lpae_tcr - io-pgtable 의 계산 결과를 TCR 레지스터 값으로 옮긴다
 *
 * @cfg: io-pgtable 이 채운 페이지 테이블 설정.
 * @return: 컨텍스트 뱅크의 TCR 에 쓸 값.
 *
 * SMMU 는 CPU 와 같은 표 형식을 쓰므로, 표를 만드는 코드를 그대로 나눠
 * 쓴다. 다만 같은 뜻의 필드가 레지스터마다 다른 자리에 있어 옮겨 담아야
 * 한다.
 *
 * TTBR1 을 쓰는 경우가 특별하다. 그쪽 필드들은 같은 배치가 16비트 위에
 * 있어 통째로 밀면 되고, 쓰지 않는 TTBR0 쪽 표 순회는 꺼야 한다.
 */
static inline u32 arm_smmu_lpae_tcr(const struct io_pgtable_cfg *cfg)
{
	u32 tcr = FIELD_PREP(ARM_SMMU_TCR_TG0, cfg->arm_lpae_s1_cfg.tcr.tg) |	/* [한국어] io-pgtable 이 계산해 둔 값들을 SMMU 레지스터 자리로 옮겨 담는다. 두 쪽의 필드 뜻은 같고 위치만 다르다. */
		FIELD_PREP(ARM_SMMU_TCR_SH0, cfg->arm_lpae_s1_cfg.tcr.sh) |
		FIELD_PREP(ARM_SMMU_TCR_ORGN0, cfg->arm_lpae_s1_cfg.tcr.orgn) |
		FIELD_PREP(ARM_SMMU_TCR_IRGN0, cfg->arm_lpae_s1_cfg.tcr.irgn) |
		FIELD_PREP(ARM_SMMU_TCR_T0SZ, cfg->arm_lpae_s1_cfg.tcr.tsz);

       /*
	* When TTBR1 is selected shift the TCR fields by 16 bits and disable
	* translation in TTBR0
	*/
	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1) {	/* [한국어] 위 영어 주석대로 TTBR1 을 쓰는 경우. */
		tcr = (tcr << 16) & ~ARM_SMMU_TCR_A1;	/* [한국어] TTBR1 쪽 필드는 같은 배치가 16비트 위에 있어 통째로 민다. A1 자리와 겹치므로 지운다. */
		tcr |= ARM_SMMU_TCR_EPD0;	/* [한국어] TTBR0 쪽 표 순회를 끈다 — 그 주소 공간은 쓰지 않는다. */
	} else
		tcr |= ARM_SMMU_TCR_EPD1;	/* [한국어] 반대로 TTBR0 만 쓸 때는 TTBR1 쪽을 끈다. */

	return tcr;	/* [한국어] 완성된 레지스터 값. */
}

/*
 * [한국어]
 * arm_smmu_lpae_tcr2 - TCR2 레지스터 값을 만든다
 *
 * @cfg: 페이지 테이블 설정.
 * @return: TCR2 에 쓸 값.
 *
 * 물리 주소 크기와 부호 확장 경계만 담는다. 경계를 "상위 장치가 보내는
 * 폭 그대로"로 두어, 장치가 낸 주소를 그대로 해석하게 한다.
 */
static inline u32 arm_smmu_lpae_tcr2(const struct io_pgtable_cfg *cfg)
{
	return FIELD_PREP(ARM_SMMU_TCR2_PASIZE, cfg->arm_lpae_s1_cfg.tcr.ips) |	/* [한국어] 물리 주소 크기와 부호 확장 경계만 담는다. */
	       FIELD_PREP(ARM_SMMU_TCR2_SEP, ARM_SMMU_TCR2_SEP_UPSTREAM);
}

/*
 * [한국어]
 * arm_smmu_lpae_vtcr - 2단계 변환 제어 값을 만든다
 *
 * @cfg: 페이지 테이블 설정.
 * @return: VTCR 에 쓸 값.
 *
 * 1단계와 달리 시작 단계(SL0)를 명시해야 한다. 2단계 표는 입력 주소
 * 폭에 따라 몇 단계짜리인지가 달라지기 때문이다.
 */
static inline u32 arm_smmu_lpae_vtcr(const struct io_pgtable_cfg *cfg)
{
	return ARM_SMMU_VTCR_RES1 |	/* [한국어] 2단계용 값. RES1 은 반드시 1 이어야 하는 예약 비트다. */
	       FIELD_PREP(ARM_SMMU_VTCR_PS, cfg->arm_lpae_s2_cfg.vtcr.ps) |
	       FIELD_PREP(ARM_SMMU_VTCR_TG0, cfg->arm_lpae_s2_cfg.vtcr.tg) |
	       FIELD_PREP(ARM_SMMU_VTCR_SH0, cfg->arm_lpae_s2_cfg.vtcr.sh) |
	       FIELD_PREP(ARM_SMMU_VTCR_ORGN0, cfg->arm_lpae_s2_cfg.vtcr.orgn) |
	       FIELD_PREP(ARM_SMMU_VTCR_IRGN0, cfg->arm_lpae_s2_cfg.vtcr.irgn) |
	       FIELD_PREP(ARM_SMMU_VTCR_SL0, cfg->arm_lpae_s2_cfg.vtcr.sl) |
	       FIELD_PREP(ARM_SMMU_VTCR_T0SZ, cfg->arm_lpae_s2_cfg.vtcr.tsz);
}

/* Implementation details, yay! */
/* [한국어]
 * (위 영어 주석에 이어)
 * 구현체별 차이를 흡수하는 콜백표.
 *
 * 퀄컴과 NVIDIA 를 비롯한 여러 회사가 규격을 조금씩 벗어나게 만들었다.
 * 그 차이를 여기서 갈고리로 덮어, 본체 코드가 깨끗하게 유지된다.
 *
 * 모든 콜백이 선택 사항이라, 채우지 않으면 규격대로 동작한다. */
struct arm_smmu_impl {
	/* [한국어] 32비트 레지스터 읽기를 가로챈다.
	 *  읽는 자: arm_smmu_readl.
	 *  NVIDIA 처럼 레지스터 창이 여러 개로 나뉜 하드웨어가 이것을 채운다. */
	u32 (*read_reg)(struct arm_smmu_device *smmu, int page, int offset);
	/* [한국어] 32비트 레지스터 쓰기를 가로챈다.
	 *  여러 창에 같은 값을 함께 써야 하는 하드웨어에 필요하다. */
	void (*write_reg)(struct arm_smmu_device *smmu, int page, int offset,
			  u32 val);
	u64 (*read_reg64)(struct arm_smmu_device *smmu, int page, int offset);
	/* [한국어] 64비트 레지스터 읽기를 가로채는 갈고리(선택).
	 * 설정자: 구현체가 자기 impl 표에 채운다.
	 * 읽는 자: arm_smmu_readq() 계열이 이 갈고리가 있으면 그것을 부르고, 없으면
	 *   곧바로 readq 한다.
	 * 왜 필요한가: 64비트 접근을 지원하지 않아 32비트 두 번으로 나눠야 하는
	 *   하드웨어가 있고, 그때 두 절반을 읽는 사이에 값이 바뀌지 않도록 순서와
	 *   재시도가 필요하다. 그 처리를 구현체가 맡는다.
	 * @page/@offset 로 주소를 나눠 받는 이유: SMMU 의 레지스터는 페이지 단위로
	 *   나뉘어 있고, 페이지 크기가 하드웨어마다 달라 주소를 직접 계산할 수 없다.
	 * NULL 이면 기본 readq 를 쓴다. */
	void (*write_reg64)(struct arm_smmu_device *smmu, int page, int offset,
	/* [한국어] 64비트 레지스터 쓰기를 가로채는 갈고리(선택).
	 * 설정자: 구현체의 impl 표.
	 * 읽는 자: arm_smmu_writeq() 계열.
	 * 위 read_reg64 의 짝이며, 나누어 쓸 때의 순서 문제가 더 까다롭다 — 절반만
	 *   쓴 중간 상태를 하드웨어가 보면 엉뚱한 주소로 변환할 수 있어, 어느 절반을
	 *   먼저 쓸지가 정해져 있다.
	 * 같은 값을 여러 레지스터 창에 함께 써야 하는 하드웨어도 이 갈고리로 처리한다.
	 * NULL 이면 기본 writeq 를 쓴다. */
			    u64 val);
	/* [한국어] 능력을 읽어 낸 뒤 불린다.
	 *  규격이 알려 주지 않는 값을 손수 채우거나, 잘못 알려 주는 값을
	 *  바로잡는 자리다. */
	int (*cfg_probe)(struct arm_smmu_device *smmu);
	/* [한국어] 하드웨어 초기화 뒤 불린다.
	 *  ARM MMU-500 처럼 알려진 문제를 우회하는 절차를 넣는다. */
	int (*reset)(struct arm_smmu_device *smmu);
	/* [한국어] 도메인의 페이지 테이블 설정을 정한 뒤 불린다.
	 *  구현체가 표 형식이나 예외 표시를 바꿀 수 있다. */
	int (*init_context)(struct arm_smmu_domain *smmu_domain,
			struct io_pgtable_cfg *cfg, struct device *dev);
	/* [한국어] TLB 무효화 완료를 기다리는 방법을 바꾼다.
	 *  완료 표시가 규격과 다른 하드웨어가 있다. */
	void (*tlb_sync)(struct arm_smmu_device *smmu, int page, int sync,
			 int status);
	/* [한국어] 이 장치에 어떤 기본 도메인을 줄지 정한다.
	 *  GPU 처럼 항등 매핑이 필요한 장치를 가려내는 데 쓴다. */
	int (*def_domain_type)(struct device *dev);
	/* [한국어] 전역 오류 처리기를 바꾼다. */
	irqreturn_t (*global_fault)(int irq, void *dev);
	/* [한국어] 문맥 오류 처리기를 바꾼다.
	 *  구현체가 오류 정보를 더 자세히 읽거나 다르게 다룰 때. */
	irqreturn_t (*context_fault)(int irq, void *dev);
	/* [한국어] 문맥 오류 처리기가 잠들 수 있는가.
	 *  참이면 스레드 인터럽트로 등록한다 — 퀄컴처럼 처리 중에 전원 관리를
	 *  건드려야 하는 구현이 있다. */
	bool context_fault_needs_threaded_irq;
	/* [한국어] 컨텍스트 뱅크 배정 방식을 바꾼다.
	 *  어떤 하드웨어는 특정 장치가 특정 뱅크만 쓸 수 있다. */
	int (*alloc_context_bank)(struct arm_smmu_domain *smmu_domain,
				  struct arm_smmu_device *smmu,
				  struct device *dev, int start);
	/* [한국어] S2CR 쓰기를 가로챈다.
	 *  퀄컴은 이 레지스터에 규격에 없는 비트를 더 쓴다. */
	void (*write_s2cr)(struct arm_smmu_device *smmu, int idx);
	/* [한국어] 뱅크 제어 레지스터 쓰기를 가로챈다. */
	void (*write_sctlr)(struct arm_smmu_device *smmu, int idx, u32 reg);
	/* [한국어] 장치 probe 의 마지막에 불린다.
	 *  그 장치에 필요한 마무리 설정을 하는 자리다. */
	void (*probe_finalize)(struct arm_smmu_device *smmu, struct device *dev);
};

#define INVALID_SMENDX			-1	/* [한국어] 스트림 매핑 항목이 배정되지 않았음을 뜻하는 값. */
#define cfg_smendx(cfg, fw, i) \
	(i >= fw->num_ids ? INVALID_SMENDX : cfg->smendx[i])	/* [한국어] 범위를 넘으면 무효를 돌려주어, 아래 고리가 안전하게 끝나게 한다. */
#define for_each_cfg_sme(cfg, fw, i, idx) \
	for (i = 0; idx = cfg_smendx(cfg, fw, i), i < fw->num_ids; ++i)	/* [한국어] 쉼표 연산자로 idx 를 먼저 채운 뒤 조건을 본다. 고리 안에서 두 값을 함께 쓰기 위한 관용구다. */

/*
 * [한국어]
 * __arm_smmu_alloc_bitmap - 비트맵에서 빈 자리를 원자적으로 잡는다
 *
 * @map: 대상 비트맵.
 * @start: 찾기 시작할 자리.
 * @end: 그 끝(포함하지 않음).
 * @return: 잡은 자리, 없으면 -ENOSPC.
 *
 * 찾기와 잡기를 원자적으로 묶을 방법이 없어, 찾은 뒤 잡아 보고 실패하면
 * 다시 찾는 고리를 돈다. 락을 잡지 않고도 안전한 방식이다.
 *
 * 컨텍스트 뱅크와 인터럽트 번호를 배정하는 데 쓴다.
 */
static inline int __arm_smmu_alloc_bitmap(unsigned long *map, int start, int end)
{
	int idx;	/* [한국어] 찾은 빈 자리. */

	do {	/* [한국어] test_and_set 이 실패하면 다시 찾는다. */
		idx = find_next_zero_bit(map, end, start);	/* [한국어] 빈 자리를 찾는다. */
		if (idx == end)	/* [한국어] 끝까지 갔으면 */
			return -ENOSPC;	/* [한국어] 자리가 없다. */
	} while (test_and_set_bit(idx, map));	/* [한국어] 원자적으로 잡아 본다. 이미 남이 잡았으면 참을 돌려주어 다시 찾게 한다. */

	return idx;	/* [한국어] 잡은 자리. */
}

/*
 * [한국어]
 * arm_smmu_page - 레지스터 페이지 n 의 주소를 구한다
 *
 * @smmu: 대상 SMMU.
 * @n: 페이지 번호.
 * @return: 그 페이지의 가상 주소.
 *
 * 페이지 크기가 4KB 인지 64KB 인지는 하드웨어가 알려 준 값이라, 상수가
 * 아니라 pgshift 로 민다.
 */
static inline void __iomem *arm_smmu_page(struct arm_smmu_device *smmu, int n)
{
	return smmu->base + (n << smmu->pgshift);	/* [한국어] 레지스터 페이지 n 의 주소. 페이지 크기는 하드웨어가 알려 준 값(4K 또는 64K)이다. */
}

/*
 * [한국어]
 * arm_smmu_readl - 32비트 레지스터를 읽는다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안에서의 오프셋.
 * @return: 읽은 값.
 *
 * 구현체가 읽기를 가로챌 수 있어 먼저 확인한다. 그런 경우가 드물어
 * unlikely 를 붙여, 평범한 경로가 분기 예측에 유리하게 한다.
 *
 * relaxed 판을 쓰므로 장벽이 없다 — 순서가 중요한 곳은 호출자가 따로
 * 넣는다.
 */
static inline u32 arm_smmu_readl(struct arm_smmu_device *smmu, int page, int offset)
{
	if (smmu->impl && unlikely(smmu->impl->read_reg))	/* [한국어] 구현체가 읽기를 가로채면 그쪽으로 보낸다. unlikely 를 붙여 평범한 경로를 빠르게 한다. */
		return smmu->impl->read_reg(smmu, page, offset);	/* [한국어] 구현체별 우회 경로. */
	return readl_relaxed(arm_smmu_page(smmu, page) + offset);	/* [한국어] relaxed 판이라 장벽이 없다 — 순서가 중요한 곳은 호출자가 따로 장벽을 넣는다. */
}

/*
 * [한국어]
 * arm_smmu_writel - 32비트 레지스터에 쓴다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안에서의 오프셋.
 * @val: 쓸 값.
 *
 * 읽기와 같은 구조다. NVIDIA 처럼 레지스터 창이 여러 개인 하드웨어는
 * 이 갈고리로 모든 창에 함께 쓴다.
 */
static inline void arm_smmu_writel(struct arm_smmu_device *smmu, int page,
				   int offset, u32 val)
{
	if (smmu->impl && unlikely(smmu->impl->write_reg))	/* [한국어] 구현체가 쓰기를 가로채면 */
		smmu->impl->write_reg(smmu, page, offset, val);	/* [한국어] 그쪽으로 보낸다. */
	else
		writel_relaxed(val, arm_smmu_page(smmu, page) + offset);	/* [한국어] 아니면 곧장 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_readq - 64비트 레지스터를 읽는다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안에서의 오프셋.
 * @return: 읽은 값.
 *
 * 32비트 기계에서는 헤더가 두 번의 32비트 접근으로 나눠 준다. 그 순서가
 * 중요해(상위 먼저) 전용 헤더를 포함한다.
 */
static inline u64 arm_smmu_readq(struct arm_smmu_device *smmu, int page, int offset)
{
	if (smmu->impl && unlikely(smmu->impl->read_reg64))	/* [한국어] 64비트 읽기의 우회. */
		return smmu->impl->read_reg64(smmu, page, offset);	/* [한국어] 구현체 경로. */
	return readq_relaxed(arm_smmu_page(smmu, page) + offset);	/* [한국어] 64비트를 한 번에 읽는다. 32비트 기계에서는 헤더가 두 번으로 나눠 준다. */
}

/*
 * [한국어]
 * arm_smmu_writeq - 64비트 레지스터에 쓴다
 *
 * @smmu: 대상 SMMU.
 * @page: 레지스터 페이지 번호.
 * @offset: 그 안에서의 오프셋.
 * @val: 쓸 값.
 *
 * TTBR 처럼 64비트인 레지스터에 쓴다.
 */
static inline void arm_smmu_writeq(struct arm_smmu_device *smmu, int page,
				   int offset, u64 val)
{
	if (smmu->impl && unlikely(smmu->impl->write_reg64))	/* [한국어] 64비트 쓰기의 우회. */
		smmu->impl->write_reg64(smmu, page, offset, val);	/* [한국어] 구현체 경로. */
	else
		writeq_relaxed(val, arm_smmu_page(smmu, page) + offset);	/* [한국어] 아니면 곧장 쓴다. */
}

#define ARM_SMMU_GR0		0	/* [한국어] 첫 번째 전역 레지스터 페이지의 번호. */
#define ARM_SMMU_GR1		1	/* [한국어] 두 번째 전역 레지스터 페이지. */
#define ARM_SMMU_CB(s, n)	((s)->numpage + (n))	/* [한국어] 컨텍스트 뱅크 n 의 페이지 번호. 전역 페이지들 뒤에 이어진다. */

/* [한국어] GR0 읽기 줄임말. 아래 짝들이 페이지 번호를 감춰 호출부를 읽기 쉽게 한다. */
#define arm_smmu_gr0_read(s, o)		\
	arm_smmu_readl((s), ARM_SMMU_GR0, (o))	/* [한국어] 실제로는 페이지 0 에서 읽는다. */
/* [한국어] GR0 쓰기. */
#define arm_smmu_gr0_write(s, o, v)	\
	arm_smmu_writel((s), ARM_SMMU_GR0, (o), (v))	/* [한국어] 페이지 0 에 쓴다. */

/* [한국어] GR1 읽기. */
#define arm_smmu_gr1_read(s, o)		\
	arm_smmu_readl((s), ARM_SMMU_GR1, (o))	/* [한국어] 페이지 1 에서 읽는다. */
/* [한국어] GR1 쓰기. */
#define arm_smmu_gr1_write(s, o, v)	\
	arm_smmu_writel((s), ARM_SMMU_GR1, (o), (v))	/* [한국어] 페이지 1 에 쓴다. */

/* [한국어] 컨텍스트 뱅크 읽기. */
#define arm_smmu_cb_read(s, n, o)	\
	arm_smmu_readl((s), ARM_SMMU_CB((s), (n)), (o))	/* [한국어] 그 뱅크의 페이지에서 읽는다. */
/* [한국어] 컨텍스트 뱅크 쓰기. */
#define arm_smmu_cb_write(s, n, o, v)	\
	arm_smmu_writel((s), ARM_SMMU_CB((s), (n)), (o), (v))	/* [한국어] 그 뱅크의 페이지에 쓴다. */
/* [한국어] 컨텍스트 뱅크 64비트 읽기. */
#define arm_smmu_cb_readq(s, n, o)	\
	arm_smmu_readq((s), ARM_SMMU_CB((s), (n)), (o))	/* [한국어] TTBR 처럼 64비트인 레지스터에 쓴다. */
/* [한국어] 컨텍스트 뱅크 64비트 쓰기. */
#define arm_smmu_cb_writeq(s, n, o, v)	\
	arm_smmu_writeq((s), ARM_SMMU_CB((s), (n)), (o), (v))	/* [한국어] 같음. */

struct arm_smmu_device *arm_smmu_impl_init(struct arm_smmu_device *smmu);	/* [한국어] 일반 구현체 초기화. arm-smmu-impl.c 가 구현한다. */
struct arm_smmu_device *nvidia_smmu_impl_init(struct arm_smmu_device *smmu);	/* [한국어] NVIDIA 구현체. arm-smmu-nvidia.c. */
struct arm_smmu_device *qcom_smmu_impl_init(struct arm_smmu_device *smmu);	/* [한국어] 퀄컴 구현체. arm-smmu-qcom.c. */

int __init arm_smmu_impl_module_init(void);	/* [한국어] 구현체 모듈의 초기화 진입점. */
void __exit arm_smmu_impl_module_exit(void);	/* [한국어] 그 정리. */
int __init qcom_smmu_module_init(void);	/* [한국어] 퀄컴 구현체 모듈의 초기화. */
void __exit qcom_smmu_module_exit(void);	/* [한국어] 그 정리. */

void arm_smmu_write_context_bank(struct arm_smmu_device *smmu, int idx);	/* [한국어] 저장해 둔 뱅크 설정을 하드웨어에 실제로 쓴다. 구현체들도 부른다. */
int arm_mmu500_reset(struct arm_smmu_device *smmu);	/* [한국어] ARM MMU-500 전용 리셋. 그 구현체의 알려진 문제를 우회한다. */

/* [한국어] 문맥 오류의 내용을 한 번에 담아 두는 구조체.
 *
 * 오류 레지스터를 여러 번 읽어야 하는데, 그 사이 새 오류가 덮어쓸 수
 * 있다. 한꺼번에 읽어 이 구조체에 담아 두고 나중에 출력한다. */
struct arm_smmu_context_fault_info {
	unsigned long iova;
	/* [한국어] 오류가 난 주소 — FAR(Fault Address Register)에서 읽는다.
	 * 설정자: arm_smmu_read_context_fault_info() 가 오류 레지스터들을 한꺼번에
	 *   읽을 때 담는다.
	 * 읽는 자: 오류를 로그로 출력하는 코드, 그리고 폴트를 상위에 보고하는 경로.
	 * 왜 구조체에 담아 두는가: 오류 정보를 알려면 레지스터를 네 번 읽어야 하는데,
	 *   그 사이 새 오류가 나면 레지스터가 덮어써진다. 그러면 iova 는 첫 오류의
	 *   것이고 fsr 은 두 번째 오류의 것인, 앞뒤가 맞지 않는 보고가 나온다.
	 *   한꺼번에 읽어 담아 두면 적어도 한 시점의 일관된 모습이 남는다.
	 * 값 범위: 변환에 실패한 IOVA. 페이지 정렬이 아닐 수 있다. */
	u32 fsr;
	/* [한국어] 오류 상태 비트들 — FSR(Fault Status Register)에서 읽는다.
	 * 설정자/읽는 자: 위 iova 와 같은 방식으로 함께 읽고 함께 출력한다.
	 * 무엇이 담기는가: 어떤 종류의 오류인지(변환 실패, 권한 위반, 외부 접근
	 *   오류)와, 오류가 여러 번 겹쳤는지(multiple fault)를 나타내는 비트들이다.
	 * 왜 가장 중요한가: 이 값이 0 이면 처리할 오류가 없다는 뜻이라, 인터럽트
	 *   처리기가 가장 먼저 보는 것이 이 레지스터다. 그리고 오류를 처리한 뒤
	 *   이 레지스터에 되써서 오류 상태를 지워야 다음 오류를 받을 수 있다.
	 * 값 범위: ARM_SMMU_FSR_* 비트들의 조합. */
	u32 fsynr;
	/* [한국어] 오류를 낸 스트림 id(CBFRSYNRA).
	 *  어느 장치가 잘못했는지 알아내는 데 가장 중요한 값이다. */
	u32 cbfrsynra;
};

void arm_smmu_read_context_fault_info(struct arm_smmu_device *smmu, int idx,	/* [한국어] 오류 레지스터들을 한 번에 읽어 구조체에 담는다. */
				      struct arm_smmu_context_fault_info *cfi);

void arm_smmu_print_context_fault_info(struct arm_smmu_device *smmu, int idx,	/* [한국어] 그 정보를 사람이 읽을 수 있게 로그에 남긴다. */
				       const struct arm_smmu_context_fault_info *cfi);

#endif /* _ARM_SMMU_H */	/* [한국어] 포함 가드의 끝. */
