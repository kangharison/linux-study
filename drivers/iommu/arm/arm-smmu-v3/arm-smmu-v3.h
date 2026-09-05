/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IOMMU API for ARM architected SMMUv3 implementations.
 *
 * Copyright (C) 2015 ARM Limited
 */

/*
 * [한국어 설명] ARM SMMUv3 드라이버의 공용 헤더 — 하드웨어 규격과 자료 모델 (arm-smmu-v3.h)
 *
 * === 파일의 역할 ===
 * ARM System MMU 3세대(SMMUv3)의 하드웨어 규격 전부와, 그 위에 얹힌 커널
 * 자료 모델을 한 파일에 모아 둔 헤더다. 레지스터 오프셋과 비트 자리, 스트림
 * 표 항목(STE)과 문맥 서술자(CD)의 워드 구성, 명령 큐에 실리는 명령 워드의
 * 짜임 같은 "하드웨어가 정한 것"이 앞쪽 절반을 채우고, 그 하드웨어를 다루기
 * 위해 커널이 만든 구조체 — 장치 하나(arm_smmu_device), 붙어 있는 마스터
 * (arm_smmu_master), 주소 공간 하나(arm_smmu_domain), 무효화 대상 배열
 * (arm_smmu_invs) — 이 뒤쪽 절반을 채운다.
 * v1/v2 용 arm-smmu.h 와 이름은 비슷하지만 하드웨어가 완전히 다르다. v2 가
 * 문맥 뱅크 128개라는 물리적 한계에 묶여 있던 것과 달리, v3 는 스트림 표와
 * 문맥 서술자 표를 메모리에 두고 하드웨어가 그것을 걸어가게 하는 구조라
 * 도메인 수에 사실상 제한이 없다. 그 대신 표를 고쳐 쓰는 순서가 까다로워졌고,
 * 이 헤더가 그 규약(arm_smmu_entry_writer)까지 함께 정의한다.
 * 파일이 하나의 구현부에 딸린 헤더가 아니라는 점도 중요하다. arm-smmu-v3.c
 * 외에 SVA, iommufd 연동, Tegra 보조 큐, 셀프테스트가 모두 이 헤더를 공유하며,
 * 그래서 static inline 헬퍼와 외부 선언이 함께 들어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치가 DMA 주소를 내밀면 SMMUv3 는 이렇게 걸어간다:
 *
 *   장치가 낸 트랜잭션 (스트림 id + 선택적 PASID)
 *     → 스트림 표(1단계 평면 또는 2단계) 에서 STE 한 항목을 찾는다
 *     → STE 가 문맥 서술자 표를 가리키면, PASID 로 CD 한 항목을 찾는다
 *     → CD 의 TTBR 이 1단계 페이지 테이블을 가리킨다 (가상 → 중간 물리)
 *     → STE 의 s2ttb 가 2단계 페이지 테이블을 가리킨다 (중간 물리 → 물리)
 *     → 결과 주소로 메모리에 접근한다
 *
 * 이 헤더의 정의들은 그 경로의 각 단계에 하나씩 대응한다. STRTAB_* 는 첫
 * 단계를, CTXDESC_* 는 둘째 단계를, arm_smmu_domain 의 s1_cfg/s2_cfg 는
 * 셋째·넷째 단계를 다룬다.
 * 반대 방향, 즉 커널이 하드웨어에게 말을 거는 길은 세 개의 큐다. 명령 큐
 * (CMDQ)로 무효화와 설정 변경을 보내고, 이벤트 큐(EVTQ)로 폴트를 돌려받고,
 * PRI 큐로 페이지 요청을 받는다. arm_smmu_queue / arm_smmu_cmdq / arm_smmu_evtq
 * / arm_smmu_priq 가 그 셋을 각각 담는다.
 * 실행 컨텍스트는 여러 겹이다 — 프로브와 붙이기는 프로세스 문맥, 무효화는
 * 원자적 문맥, 이벤트·PRI 처리는 인터럽트 스레드, 그리고 iommufd 를 거치면
 * 사용자 공간의 ioctl 이 직접 이 자료 구조를 흔든다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 iommu 코어(drivers/iommu/iommu.c)의 iommu_ops 를 구현한다.
 * 도메인을 만들고 장치를 붙이고 매핑을 거는 모든 요청이 그 연산표를 거쳐
 * 여기로 내려온다. DMA API 를 쓰는 평범한 드라이버는 iommu-dma 를 통해,
 * 사용자 공간에 장치를 넘기는 VFIO/iommufd 는 iommufd 를 통해 닿는다.
 * 아래쪽으로는 io-pgtable(ARM LPAE 형식)이 실제 페이지 테이블을 짓는다.
 * 이 드라이버는 테이블을 직접 만들지 않고, io_pgtable_cfg 로 형식만 정한 뒤
 * 만들어진 TTBR 값을 CD/STE 에 옮겨 담는다. 테이블이 바뀌면 io_pgtable 이
 * tlb_flush_ops 콜백으로 이 드라이버를 다시 불러 무효화를 시킨다.
 * 옆으로는 같은 디렉토리의 형제 파일들과 이 헤더를 공유한다 —
 * arm-smmu-v3-sva.c(프로세스 주소 공간 공유), arm-smmu-v3-iommufd.c(게스트에게
 * SMMU 를 그대로 보여 주는 중첩 변환), tegra241-cmdqv.c(보조 명령 큐),
 * arm-smmu-v3-test.c(STE/CD 쓰기 규약의 단위 시험).
 * 데이터 흐름으로 보면, 하드웨어가 쓰는 것(스트림 표·문맥 표·페이지 테이블·
 * 큐 링 버퍼)은 모두 dma 로 잡힌 일관 메모리이고, 커널만 쓰는 것(마스터 목록·
 * 무효화 배열·xarray)은 보통 메모리다. 두 세계를 잇는 지점이 STE/CD 쓰기와
 * 큐 포인터 갱신이며, 그 두 곳에서만 메모리 장벽과 캐시 관리가 필요하다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct arm_smmu_device: SMMU 하드웨어 한 개. 레지스터 주소, 능력 비트,
 *   세 개의 큐, 스트림 표, ASID/VMID 풀, 붙어 있는 스트림의 rb 트리를 쥔다.
 * - struct arm_smmu_domain: 주소 공간 하나. 1단계면 cd(ASID·TTBR), 2단계면
 *   s2_cfg(VMID·s2ttb)를 담고, invs 에 "이 도메인이 바뀌면 무엇을 무효화해야
 *   하는가"를 미리 모아 둔다.
 * - struct arm_smmu_master: 붙어 있는 장치 하나. 스트림 번호 목록, 문맥 서술자
 *   표, ATS 상태를 쥔다. 도메인과는 arm_smmu_master_domain 으로 다대다로 엮인다.
 * - struct arm_smmu_invs: RCU 로 읽는 무효화 대상 배열. 도메인마다 하나씩 걸려
 *   있어, 무효화할 때 붙은 장치를 뒤지지 않고 이 배열만 훑으면 된다.
 * - struct arm_smmu_cmdq / arm_smmu_ll_queue: 락 없는 명령 큐. valid_map 비트맵과
 *   owner_prod 로 여러 CPU 가 락 없이 자기 자리를 차지하고 대표 하나가 하드웨어에
 *   알리는 구조를 만든다.
 * - struct arm_smmu_entry_writer(_ops): STE/CD 처럼 여러 워드짜리 항목을, 하드웨어가
 *   중간의 어긋난 상태를 절대 보지 못하도록 순서를 지켜 고쳐 쓰는 규약.
 * - arm_smmu_strtab_l1_idx()/l2_idx(), arm_smmu_cdtab_l1_idx()/l2_idx():
 *   스트림 번호와 PASID 를 2단계 표의 위·아래 첨자로 쪼개는 헬퍼.
 * - arm_smmu_domain_inv_range()/arm_smmu_domain_inv(): 도메인 단위 무효화 진입점.
 * - arm_smmu_attach_prepare()/attach_commit(): 실패할 수 있는 일을 앞 단계로 몰고
 *   하드웨어에 쓴 뒤에는 실패하지 않게 만드는 붙이기 2단계 규약.
 */
#ifndef _ARM_SMMU_V3_H	/* [한국어] 이 헤더가 두 번 펼쳐지는 것을 막는 보호 매크로. */
#define _ARM_SMMU_V3_H	/* [한국어] 처음 펼쳐질 때 표시를 남긴다. */

#include <linux/bitfield.h>	/* [한국어] FIELD_PREP / FIELD_GET. 아래 정의가 거의 모두 이것으로 쓰였다. */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 도메인과 연산표. */
#include <linux/iommufd.h>	/* [한국어] vIOMMU 와 중첩 변환을 iommufd 로 노출하는 데 필요하다. */
#include <linux/kernel.h>	/* [한국어] 기본 매크로들. */
#include <linux/mmzone.h>	/* [한국어] NUMA 노드 정보. 큐와 표를 가까운 노드에서 잡는다. */
#include <linux/sizes.h>	/* [한국어] SZ_4K 등 크기 상수. */

struct arm_smmu_device;	/* [한국어] 아래 여러 선언이 이것을 포인터로 참조해 미리 선언한다. */
struct arm_vsmmu;	/* [한국어] iommufd 쪽 가상 SMMU. 같은 이유의 전방 선언이다. */

/* MMIO registers */
#define ARM_SMMU_IDR0			0x0	/* [한국어] 능력 레지스터 0. 이 하드웨어가 무엇을 할 수 있는지 담는다. */
#define IDR0_ST_LVL			GENMASK(28, 27)	/* [한국어] 스트림 표가 몇 단계인가. 2단계면 큰 표를 나눠 잡을 수 있다. */
#define IDR0_ST_LVL_2LVL		1	/* [한국어] 2단계 표를 지원한다는 값. */
#define IDR0_STALL_MODEL		GENMASK(25, 24)	/* [한국어] 폴트 때 트랜잭션을 멈춰 세울 수 있는가. */
#define IDR0_STALL_MODEL_STALL		0	/* [한국어] 멈춰 세우기를 지원한다. */
#define IDR0_STALL_MODEL_FORCE		2	/* [한국어] 늘 멈춰 세운다 — 끌 수 없다는 뜻이라 폴트 처리기가 반드시 있어야 한다. */
#define IDR0_TTENDIAN			GENMASK(22, 21)	/* [한국어] 표를 어느 엔디언으로 읽는가. */
#define IDR0_TTENDIAN_MIXED		0	/* [한국어] 둘 다 지원해 골라 쓸 수 있다. */
#define IDR0_TTENDIAN_LE		2	/* [한국어] 리틀 엔디언만. */
#define IDR0_TTENDIAN_BE		3	/* [한국어] 빅 엔디언만. */
#define IDR0_CD2L			(1 << 19)	/* [한국어] 문맥 서술자 표를 2단계로 만들 수 있다. PASID 가 많을 때 필요하다. */
#define IDR0_VMID16			(1 << 18)	/* [한국어] 16비트 VMID 를 쓸 수 있다. */
#define IDR0_PRI			(1 << 16)	/* [한국어] PCI 페이지 요청 인터페이스를 지원한다. */
#define IDR0_SEV			(1 << 14)	/* [한국어] 명령 큐가 찼을 때 이벤트로 깨울 수 있다 — 폴링 대신 WFE 로 기다린다. */
#define IDR0_MSI			(1 << 13)	/* [한국어] 인터럽트를 MSI 로 낼 수 있다. 그러면 인터럽트 선이 따로 필요 없다. */
#define IDR0_ASID16			(1 << 12)	/* [한국어] 16비트 ASID 를 쓸 수 있다. */
#define IDR0_ATS			(1 << 10)	/* [한국어] 장치 쪽 변환 캐시(ATS)를 지원한다. */
#define IDR0_HYP			(1 << 9)	/* [한국어] 하이퍼바이저 관련 기능을 지원한다. */
#define IDR0_HTTU			GENMASK(7, 6)	/* [한국어] 하드웨어가 접근·더티 비트를 갱신할 수 있는가. */
#define IDR0_HTTU_ACCESS		1	/* [한국어] 접근 비트만 갱신한다. */
#define IDR0_HTTU_ACCESS_DIRTY		2	/* [한국어] 더티 비트까지 갱신한다. 마이그레이션에 쓸 수 있다는 뜻이다. */
#define IDR0_COHACC			(1 << 4)	/* [한국어] 표 순회가 캐시 일관성을 갖는다. */
#define IDR0_TTF			GENMASK(3, 2)	/* [한국어] 지원하는 표 형식. */
#define IDR0_TTF_AARCH64		2	/* [한국어] 64비트 형식만 지원한다는 값. */
#define IDR0_S1P			(1 << 1)	/* [한국어] 1단계 변환을 지원한다. */
#define IDR0_S2P			(1 << 0)	/* [한국어] 2단계 변환을 지원한다. */

#define ARM_SMMU_IDR1			0x4	/* [한국어] 능력 레지스터 1. 주로 크기를 담는다. */
#define IDR1_TABLES_PRESET		(1 << 30)	/* [한국어] 표 주소가 하드웨어에 고정되어 있다 — 커널이 정할 수 없다. */
#define IDR1_QUEUES_PRESET		(1 << 29)	/* [한국어] 큐 주소도 고정되어 있다. */
#define IDR1_REL			(1 << 28)	/* [한국어] 큐 기준 주소가 상대 주소다. */
#define IDR1_ATTR_TYPES_OVR		(1 << 27)	/* [한국어] 메모리 속성을 덮어쓸 수 있다. */
#define IDR1_CMDQS			GENMASK(25, 21)	/* [한국어] 명령 큐의 최대 크기(로그 값). */
#define IDR1_EVTQS			GENMASK(20, 16)	/* [한국어] 이벤트 큐의 최대 크기. */
#define IDR1_PRIQS			GENMASK(15, 11)	/* [한국어] 페이지 요청 큐의 최대 크기. */
#define IDR1_SSIDSIZE			GENMASK(10, 6)	/* [한국어] 부스트림 id(PASID)의 비트 수. */
#define IDR1_SIDSIZE			GENMASK(5, 0)	/* [한국어] 스트림 id 의 비트 수. 이것이 스트림 표의 크기를 정한다. */

#define ARM_SMMU_IDR3			0xc	/* [한국어] 능력 레지스터 3. */
#define IDR3_FWB			(1 << 8)	/* [한국어] 2단계가 1단계 메모리 속성을 강제로 덮어쓸 수 있다. */
#define IDR3_RIL			(1 << 10)	/* [한국어] 범위 기반 무효화를 지원한다 — 한 명령으로 여러 페이지를 비운다. */
#define IDR3_BBM			GENMASK(12, 11)	/* [한국어] 블록 단위 매핑 변경 수준. 큰 페이지를 쪼갤 때의 제약을 정한다. */

#define ARM_SMMU_IDR5			0x14	/* [한국어] 능력 레지스터 5. 주소 크기와 페이지 크기를 담는다. */
#define IDR5_STALL_MAX			GENMASK(31, 16)	/* [한국어] 동시에 멈춰 세울 수 있는 트랜잭션 수. */
#define IDR5_GRAN64K			(1 << 6)	/* [한국어] 64KB 페이지를 지원한다. */
#define IDR5_GRAN16K			(1 << 5)	/* [한국어] 16KB 페이지. */
#define IDR5_GRAN4K			(1 << 4)	/* [한국어] 4KB 페이지. */
#define IDR5_OAS			GENMASK(2, 0)	/* [한국어] 출력(물리) 주소 크기 부호. */
#define IDR5_OAS_32_BIT			0	/* [한국어] 32비트. */
#define IDR5_OAS_36_BIT			1	/* [한국어] 36비트. */
#define IDR5_OAS_40_BIT			2	/* [한국어] 40비트. */
#define IDR5_OAS_42_BIT			3	/* [한국어] 42비트. */
#define IDR5_OAS_44_BIT			4	/* [한국어] 44비트. */
#define IDR5_OAS_48_BIT			5	/* [한국어] 48비트. 오늘날 가장 흔하다. */
#define IDR5_OAS_52_BIT			6	/* [한국어] 52비트. */
#define IDR5_VAX			GENMASK(11, 10)	/* [한국어] 가상 주소 확장. 52비트 입력 주소를 쓸 수 있는지. */
#define IDR5_VAX_52_BIT			1	/* [한국어] 52비트 입력 주소를 지원한다는 값. */

#define ARM_SMMU_IIDR			0x18	/* [한국어] 구현 식별 레지스터. 어느 회사의 어느 판인지 담는다. */
#define IIDR_PRODUCTID			GENMASK(31, 20)	/* [한국어] 제품 번호. */
#define IIDR_VARIANT			GENMASK(19, 16)	/* [한국어] 변종 번호. */
#define IIDR_REVISION			GENMASK(15, 12)	/* [한국어] 개정 번호. 결함 우회를 가려내는 데 쓴다. */
#define IIDR_IMPLEMENTER		GENMASK(11, 0)	/* [한국어] 만든 회사의 JEP106 코드. */

#define ARM_SMMU_AIDR			0x1C	/* [한국어] 아키텍처 판 레지스터. 규격 개정을 알려 준다. */

#define ARM_SMMU_CR0			0x20	/* [한국어] 제어 레지스터 0. SMMU 전체를 켜고 끈다. */
#define CR0_ATSCHK			(1 << 4)	/* [한국어] ATS 요청을 검사한다. 켜면 장치가 요청한 주소를 확인한다. */
#define CR0_CMDQEN			(1 << 3)	/* [한국어] 명령 큐를 켠다. */
#define CR0_EVTQEN			(1 << 2)	/* [한국어] 이벤트 큐를 켠다. */
#define CR0_PRIQEN			(1 << 1)	/* [한국어] 페이지 요청 큐를 켠다. */
#define CR0_SMMUEN			(1 << 0)	/* [한국어] SMMU 를 켠다. 이 비트가 없으면 모든 트래픽이 그냥 지나간다. */

#define ARM_SMMU_CR0ACK			0x24	/* [한국어] CR0 쓰기가 실제로 반영됐는지 알려 준다. 쓴 뒤 이 값이 같아질 때까지 기다린다. */

#define ARM_SMMU_CR1			0x28	/* [한국어] 제어 레지스터 1. 표와 큐를 읽을 때의 캐시 속성을 정한다. */
#define CR1_TABLE_SH			GENMASK(11, 10)	/* [한국어] 표 접근의 공유 속성. */
#define CR1_TABLE_OC			GENMASK(9, 8)	/* [한국어] 표 접근의 바깥 캐시 정책. */
#define CR1_TABLE_IC			GENMASK(7, 6)	/* [한국어] 표 접근의 안쪽 캐시 정책. */
#define CR1_QUEUE_SH			GENMASK(5, 4)	/* [한국어] 큐 접근의 공유 속성. */
#define CR1_QUEUE_OC			GENMASK(3, 2)	/* [한국어] 큐 접근의 바깥 캐시 정책. */
#define CR1_QUEUE_IC			GENMASK(1, 0)	/* [한국어] 큐 접근의 안쪽 캐시 정책. */
/* CR1 cacheability fields don't quite follow the usual TCR-style encoding */
#define CR1_CACHE_NC			0	/* [한국어] (위 영어 주석대로 이 필드의 부호는 TCR 과 다르다) 캐시하지 않음. */
#define CR1_CACHE_WB			1	/* [한국어] 되쓰기 캐시. 성능이 가장 좋다. */
#define CR1_CACHE_WT			2	/* [한국어] 통과 쓰기 캐시. */

#define ARM_SMMU_CR2			0x2c	/* [한국어] 제어 레지스터 2. */
#define CR2_PTM				(1 << 2)	/* [한국어] 페이지 테이블 공유 브로드캐스트를 막는다. */
#define CR2_RECINVSID			(1 << 1)	/* [한국어] 잘못된 스트림 id 로 온 무효화를 기록한다. */
#define CR2_E2H				(1 << 0)	/* [한국어] 호스트 확장 모드. 커널이 EL2 에서 도는 경우에 쓴다. */

#define ARM_SMMU_GBPA			0x44	/* [한국어] 전역 우회 속성. SMMU 가 꺼져 있을 때의 동작을 정한다. */
#define GBPA_UPDATE			(1 << 31)	/* [한국어] 이 비트를 세워 써야 값이 반영된다 — 원자적 갱신을 위한 방식이다. */
#define GBPA_ABORT			(1 << 20)	/* [한국어] 꺼져 있을 때 모든 접근을 중단시킨다. 지우면 그냥 통과한다. */

#define ARM_SMMU_IRQ_CTRL		0x50	/* [한국어] 인터럽트 제어. */
#define IRQ_CTRL_EVTQ_IRQEN		(1 << 2)	/* [한국어] 이벤트 큐 인터럽트를 켠다. */
#define IRQ_CTRL_PRIQ_IRQEN		(1 << 1)	/* [한국어] 페이지 요청 큐 인터럽트를 켠다. */
#define IRQ_CTRL_GERROR_IRQEN		(1 << 0)	/* [한국어] 전역 오류 인터럽트를 켠다. */

#define ARM_SMMU_IRQ_CTRLACK		0x54	/* [한국어] 그 설정이 반영됐는지 알려 준다. */

#define ARM_SMMU_GERROR			0x60	/* [한국어] 전역 오류 상태. 아래 비트들이 무엇이 잘못됐는지 말해 준다. */
#define GERROR_SFM_ERR			(1 << 8)	/* [한국어] 서비스 실패 모드 — SMMU 가 스스로 멈췄다는 뜻으로, 가장 심각하다. */
#define GERROR_MSI_GERROR_ABT_ERR	(1 << 7)	/* [한국어] 전역 오류 MSI 를 쓰다 중단됐다. */
#define GERROR_MSI_PRIQ_ABT_ERR		(1 << 6)	/* [한국어] 페이지 요청 큐 MSI 를 쓰다 중단됐다. */
#define GERROR_MSI_EVTQ_ABT_ERR		(1 << 5)	/* [한국어] 이벤트 큐 MSI 를 쓰다 중단됐다. */
#define GERROR_MSI_CMDQ_ABT_ERR		(1 << 4)	/* [한국어] 명령 큐 MSI 를 쓰다 중단됐다. */
#define GERROR_PRIQ_ABT_ERR		(1 << 3)	/* [한국어] 페이지 요청 큐를 읽고 쓰다 중단됐다. */
#define GERROR_EVTQ_ABT_ERR		(1 << 2)	/* [한국어] 이벤트 큐를 쓰다 중단됐다 — 큐 메모리가 잘못됐다는 뜻이다. */
#define GERROR_CMDQ_ERR			(1 << 0)	/* [한국어] 명령 큐에서 오류가 났다. 잘못된 명령을 넣었을 때 난다. */
#define GERROR_ERR_MASK			0x1fd	/* [한국어] 실제로 쓰이는 오류 비트들의 마스크. 예약 비트를 걸러 낸다. */

#define ARM_SMMU_GERRORN		0x64	/* [한국어] 오류 확인 레지스터. GERROR 와 같아질 때까지 쓰면 그 오류를 확인했다는 뜻이 된다. */

#define ARM_SMMU_GERROR_IRQ_CFG0	0x68	/* [한국어] 전역 오류 MSI 의 주소. */
#define ARM_SMMU_GERROR_IRQ_CFG1	0x70	/* [한국어] 그 데이터 값. */
#define ARM_SMMU_GERROR_IRQ_CFG2	0x74	/* [한국어] 그 메모리 속성. */

#define ARM_SMMU_STRTAB_BASE		0x80	/* [한국어] 스트림 표의 기준 주소. 장치의 스트림 id 로 찾아가는 표다. */
#define STRTAB_BASE_RA			(1UL << 62)	/* [한국어] 읽기 할당 힌트. 표를 캐시에 담아 두라고 알린다. */
#define STRTAB_BASE_ADDR_MASK		GENMASK_ULL(51, 6)	/* [한국어] 그 주소 자리. 64바이트 정렬이라 하위 6비트가 비어 있다. */

#define ARM_SMMU_STRTAB_BASE_CFG	0x88	/* [한국어] 스트림 표의 모양을 정한다. */
#define STRTAB_BASE_CFG_FMT		GENMASK(17, 16)	/* [한국어] 1단계인가 2단계인가. */
#define STRTAB_BASE_CFG_FMT_LINEAR	0	/* [한국어] 1단계. 모든 항목을 이어진 메모리에 둔다. */
#define STRTAB_BASE_CFG_FMT_2LVL	1	/* [한국어] 2단계. id 공간이 넓을 때 필요한 부분만 잡을 수 있다. */
#define STRTAB_BASE_CFG_SPLIT		GENMASK(10, 6)	/* [한국어] 2단계에서 상위·하위를 가르는 비트 자리. */
#define STRTAB_BASE_CFG_LOG2SIZE	GENMASK(5, 0)	/* [한국어] 표가 덮는 id 공간의 크기(로그 값). */

#define ARM_SMMU_CMDQ_BASE		0x90	/* [한국어] 명령 큐의 기준 주소. 커널이 이 큐로 SMMU 에 명령을 보낸다. */
#define ARM_SMMU_CMDQ_PROD		0x98	/* [한국어] 커널이 어디까지 넣었는지. */
#define ARM_SMMU_CMDQ_CONS		0x9c	/* [한국어] SMMU 가 어디까지 처리했는지. */

#define ARM_SMMU_EVTQ_BASE		0xa0	/* [한국어] 이벤트 큐의 기준 주소. SMMU 가 오류를 여기에 쌓는다. */
#define ARM_SMMU_EVTQ_PROD		0xa8	/* [한국어] SMMU 가 어디까지 넣었는지. */
#define ARM_SMMU_EVTQ_CONS		0xac	/* [한국어] 커널이 어디까지 읽었는지. 명령 큐와 방향이 반대다. */
#define ARM_SMMU_EVTQ_IRQ_CFG0		0xb0	/* [한국어] 이벤트 큐 MSI 의 주소. */
#define ARM_SMMU_EVTQ_IRQ_CFG1		0xb8	/* [한국어] 그 데이터 값. */
#define ARM_SMMU_EVTQ_IRQ_CFG2		0xbc	/* [한국어] 그 메모리 속성. */

#define ARM_SMMU_PRIQ_BASE		0xc0	/* [한국어] 페이지 요청 큐의 기준 주소. 장치가 페이지를 요청하면 여기 쌓인다. */
#define ARM_SMMU_PRIQ_PROD		0xc8	/* [한국어] SMMU 가 어디까지 넣었는지. */
#define ARM_SMMU_PRIQ_CONS		0xcc	/* [한국어] 커널이 어디까지 읽었는지. */
#define ARM_SMMU_PRIQ_IRQ_CFG0		0xd0	/* [한국어] 페이지 요청 큐 MSI 의 주소. */
#define ARM_SMMU_PRIQ_IRQ_CFG1		0xd8	/* [한국어] 그 데이터 값. */
#define ARM_SMMU_PRIQ_IRQ_CFG2		0xdc	/* [한국어] 그 메모리 속성. */

#define ARM_SMMU_REG_SZ			0xe00	/* [한국어] 레지스터 창의 크기. 이만큼을 매핑한다. */

/* Common MSI config fields */
#define MSI_CFG0_ADDR_MASK		GENMASK_ULL(51, 2)	/* [한국어] (위 영어 주석에 이어) MSI 를 쓸 주소. 세 큐가 같은 배치를 쓴다. */
#define MSI_CFG2_SH			GENMASK(5, 4)	/* [한국어] 그 쓰기의 공유 속성. */
#define MSI_CFG2_MEMATTR		GENMASK(3, 0)	/* [한국어] 그 메모리 속성. */

/* Common memory attribute values */
#define ARM_SMMU_SH_NSH			0	/* [한국어] (위 영어 주석에 이어) 공유하지 않음. */
#define ARM_SMMU_SH_OSH			2	/* [한국어] 바깥 공유 가능. */
#define ARM_SMMU_SH_ISH			3	/* [한국어] 안쪽 공유 가능. 같은 클러스터의 CPU 들과 일관성을 유지한다. */
#define ARM_SMMU_MEMATTR_DEVICE_nGnRE	0x1	/* [한국어] 장치 메모리. 모으지 않고 순서를 지키며 일찍 응답하지 않는다. */
#define ARM_SMMU_MEMATTR_OIWB		0xf	/* [한국어] 안팎 모두 되쓰기 캐시. 큐와 표에 쓴다. */

#define Q_IDX(llq, p)			((p) & ((1 << (llq)->max_n_shift) - 1))	/* [한국어] 큐 포인터에서 배열 첨자를 꺼낸다. 크기가 2의 거듭제곱이라 마스크로 끝난다. */
#define Q_WRP(llq, p)			((p) & (1 << (llq)->max_n_shift))	/* [한국어] 한 바퀴 돌았는지 알리는 비트. 첨자 바로 위 비트를 쓴다. */
#define Q_OVERFLOW_FLAG			(1U << 31)	/* [한국어] 큐가 넘쳤음을 알리는 비트. */
#define Q_OVF(p)			((p) & Q_OVERFLOW_FLAG)	/* [한국어] 그 비트를 꺼낸다. */
/* [한국어] 큐 포인터가 가리키는 항목의 주소. 항목 크기는 큐마다 다르다. */
#define Q_ENT(q, p)			((q)->base +			\
					 Q_IDX(&((q)->llq), p) *	\
					 (q)->ent_dwords)	/* [한국어] 항목 크기를 곱해 실제 주소로 만든다 — 큐마다 항목 워드 수가 다르다. */

#define Q_BASE_RWA			(1UL << 62)	/* [한국어] 읽기·쓰기 할당 힌트. */
#define Q_BASE_ADDR_MASK		GENMASK_ULL(51, 5)	/* [한국어] 큐의 주소 자리. */
#define Q_BASE_LOG2SIZE			GENMASK(4, 0)	/* [한국어] 큐 크기(로그 값). */

/* Ensure DMA allocations are naturally aligned */
#ifdef CONFIG_CMA_ALIGNMENT	/* [한국어] (위 영어 주석에 이어) CMA 를 쓰는 커널이면 */
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + CONFIG_CMA_ALIGNMENT)	/* [한국어] 그 정렬 한계까지만 잡을 수 있다. */
#else	/* [한국어] 아니면 */
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + MAX_PAGE_ORDER)	/* [한국어] 버디 할당기의 최대 차수까지. */
#endif	/* [한국어] 큐 크기 상한 정의의 끝. */

/*
 * Stream table.
 *
 * Linear: Enough to cover 1 << IDR1.SIDSIZE entries
 * 2lvl: 128k L1 entries,
 *       256 lazy entries per table (each table covers a PCI bus)
 */
#define STRTAB_SPLIT			8	/* [한국어] (위 영어 주석에 이어) 2단계 표에서 하위 첨자로 쓸 비트 수. 8이면 한 표가 256개를 덮어 PCI 버스 하나에 해당한다. */

#define STRTAB_L1_DESC_SPAN		GENMASK_ULL(4, 0)	/* [한국어] 1단계 항목이 덮는 범위. 0 이면 그 아래 표가 아직 없다는 뜻이다. */
#define STRTAB_L1_DESC_L2PTR_MASK	GENMASK_ULL(51, 6)	/* [한국어] 2단계 표의 주소 자리. */

#define STRTAB_STE_DWORDS		8	/* [한국어] 스트림 표 항목 하나의 크기(64비트 낱말 수). 8이면 64바이트다. */

/* [한국어] 스트림 표 항목 하나.
 *
 * 장치의 스트림 id 로 찾아가는 표의 항목이다. 그 장치를 어떻게 다룰지
 * (중단·통과·1단계·2단계·중첩), 문맥 서술자 표가 어디 있는지, 2단계
 * 변환 표가 어디 있는지가 모두 여기 담긴다.
 *
 * 64바이트라 캐시 줄 하나에 딱 맞는다. 항목을 고칠 때 여러 낱말을
 * 순서에 맞춰 써야 하는 까다로움이 arm-smmu-v3.c 의 큰 주제 중 하나다. */
struct arm_smmu_ste {
	/* [한국어] 여덟 개의 64비트 낱말.
	 *  설정자: STE 를 짓는 함수들이 낱말마다 채운다.
	 *  읽는 자: 하드웨어가 직접 읽는다 — 그래서 리틀 엔디언 고정이다. */
	__le64 data[STRTAB_STE_DWORDS];
};

#define STRTAB_NUM_L2_STES		(1 << STRTAB_SPLIT)
/* [한국어] 2단계 스트림 표의 아래쪽 표 하나.
 *
 * 항목 256개를 담아 PCI 버스 하나에 해당한다. 필요할 때만 잡아,
 * 스트림 id 공간이 넓어도 메모리를 아낀다. */
struct arm_smmu_strtab_l2 {
	/* [한국어] 그 표의 항목들.
	 *  이어진 메모리에 있어 하위 첨자로 곧장 찾아간다. */
	struct arm_smmu_ste stes[STRTAB_NUM_L2_STES];
};

/* [한국어] 2단계 스트림 표의 위쪽 항목 하나.
 *
 * 아래쪽 표를 가리키는 포인터와 그 범위를 담는다. */
struct arm_smmu_strtab_l1 {
	/* [한국어] 아래쪽 표의 주소와 범위를 담은 값.
	 *  설정자: 그 표를 처음 잡을 때. 읽는 자: 하드웨어.
	 *  값이 0 이면 그 아래 표가 아직 없다는 뜻이다. */
	__le64 l2ptr;
};
#define STRTAB_MAX_L1_ENTRIES		(1 << 17)	/* [한국어] 1단계 항목의 최대 개수. 위 주석의 128k 가 이 값이다. */

/*
 * [한국어]
 * arm_smmu_strtab_l1_idx - 스트림 id 에서 위쪽 첨자를 구한다
 *
 * @sid: 스트림 id.
 * @return: 1단계 표에서의 첨자.
 */
static inline u32 arm_smmu_strtab_l1_idx(u32 sid)
{
	return sid / STRTAB_NUM_L2_STES;	/* [한국어] 2단계 표 하나가 담는 항목 수로 나누면 1단계 첨자가 나온다. */
}

/*
 * [한국어]
 * arm_smmu_strtab_l2_idx - 스트림 id 에서 아래쪽 첨자를 구한다
 *
 * @sid: 스트림 id.
 * @return: 2단계 표 안에서의 첨자.
 */
static inline u32 arm_smmu_strtab_l2_idx(u32 sid)
{
	return sid % STRTAB_NUM_L2_STES;	/* [한국어] 나머지가 곧 그 2단계 표 안에서의 자리다. */
}

#define STRTAB_STE_0_V			(1UL << 0)	/* [한국어] 이 항목이 유효하다. 없으면 그 스트림은 다뤄지지 않는다. */
#define STRTAB_STE_0_CFG		GENMASK_ULL(3, 1)	/* [한국어] 이 스트림을 어떻게 다룰지. */
#define STRTAB_STE_0_CFG_ABORT		0	/* [한국어] 모든 접근을 중단시킨다. 차단 도메인이 이것을 쓴다. */
#define STRTAB_STE_0_CFG_BYPASS		4	/* [한국어] 변환 없이 통과시킨다. 항등 도메인이 쓴다. */
#define STRTAB_STE_0_CFG_S1_TRANS	5	/* [한국어] 1단계만 변환한다. 가장 흔한 모양이다. */
#define STRTAB_STE_0_CFG_S2_TRANS	6	/* [한국어] 2단계만 변환한다. */
#define STRTAB_STE_0_CFG_NESTED		7	/* [한국어] 두 단계를 겹쳐 쓴다. 게스트에게 장치를 넘길 때다. */

#define STRTAB_STE_0_S1FMT		GENMASK_ULL(5, 4)	/* [한국어] 문맥 서술자 표의 모양. */
#define STRTAB_STE_0_S1FMT_LINEAR	0	/* [한국어] 1단계. PASID 를 조금만 쓸 때. */
#define STRTAB_STE_0_S1FMT_64K_L2	2	/* [한국어] 2단계. PASID 가 많을 때 필요한 부분만 잡는다. */
#define STRTAB_STE_0_S1CTXPTR_MASK	GENMASK_ULL(51, 6)	/* [한국어] 그 표의 주소 자리. */
#define STRTAB_STE_0_S1CDMAX		GENMASK_ULL(63, 59)	/* [한국어] 쓸 수 있는 PASID 의 비트 수. */

#define STRTAB_STE_1_S1DSS		GENMASK_ULL(1, 0)	/* [한국어] PASID 없이 온 요청을 어떻게 다룰지. */
#define STRTAB_STE_1_S1DSS_TERMINATE	0x0	/* [한국어] 거절한다. */
#define STRTAB_STE_1_S1DSS_BYPASS	0x1	/* [한국어] 변환 없이 통과시킨다. */
#define STRTAB_STE_1_S1DSS_SSID0	0x2	/* [한국어] 0번 PASID 의 서술자를 쓴다 — 그것이 기본 주소 공간이 된다. */

#define STRTAB_STE_1_S1C_CACHE_NC	0UL	/* [한국어] 문맥 서술자 표를 캐시하지 않음. */
#define STRTAB_STE_1_S1C_CACHE_WBRA	1UL	/* [한국어] 되쓰기·읽기 할당. */
#define STRTAB_STE_1_S1C_CACHE_WT	2UL	/* [한국어] 통과 쓰기. */
#define STRTAB_STE_1_S1C_CACHE_WB	3UL	/* [한국어] 되쓰기. 가장 빠르다. */
#define STRTAB_STE_1_S1CIR		GENMASK_ULL(3, 2)	/* [한국어] 그 표 접근의 안쪽 캐시 정책. */
#define STRTAB_STE_1_S1COR		GENMASK_ULL(5, 4)	/* [한국어] 바깥 캐시 정책. */
#define STRTAB_STE_1_S1CSH		GENMASK_ULL(7, 6)	/* [한국어] 공유 속성. */

#define STRTAB_STE_1_MEV		(1UL << 19)	/* [한국어] 이벤트를 합칠 수 있다 — 같은 오류가 쏟아질 때 큐가 넘치지 않게 한다. */
#define STRTAB_STE_1_S2FWB		(1UL << 25)	/* [한국어] 2단계가 1단계 메모리 속성을 덮어쓴다. */
#define STRTAB_STE_1_S1STALLD		(1UL << 27)	/* [한국어] 1단계 폴트에서 멈춰 세우기를 끈다. 폴트를 다룰 수 없는 장치에 쓴다. */

#define STRTAB_STE_1_EATS		GENMASK_ULL(29, 28)	/* [한국어] 장치 쪽 변환 캐시(ATS)를 어떻게 다룰지. */
#define STRTAB_STE_1_EATS_ABT		0UL	/* [한국어] ATS 요청을 중단시킨다. */
#define STRTAB_STE_1_EATS_TRANS		1UL	/* [한국어] 변환해 준다. */
#define STRTAB_STE_1_EATS_S1CHK		2UL	/* [한국어] 1단계 권한까지 검사한다. */

#define STRTAB_STE_1_STRW		GENMASK_ULL(31, 30)	/* [한국어] 이 스트림이 어느 세계에 속하는가. */
#define STRTAB_STE_1_STRW_NSEL1		0UL	/* [한국어] 비보안 EL1. 보통의 커널이 쓰는 값이다. */
#define STRTAB_STE_1_STRW_EL2		2UL	/* [한국어] EL2. 커널이 하이퍼바이저 수준에서 돌 때. */

#define STRTAB_STE_1_SHCFG		GENMASK_ULL(45, 44)	/* [한국어] 공유 속성을 어떻게 정할지. */
#define STRTAB_STE_1_SHCFG_INCOMING	1UL	/* [한국어] 장치가 보낸 값을 그대로 쓴다. */

#define STRTAB_STE_2_S2VMID		GENMASK_ULL(15, 0)	/* [한국어] 2단계의 VMID. TLB 항목을 가르는 열쇠다. */
#define STRTAB_STE_2_VTCR		GENMASK_ULL(50, 32)	/* [한국어] 2단계 변환 제어. 아래 필드들이 그 안에 담긴다. */
#define STRTAB_STE_2_VTCR_S2T0SZ	GENMASK_ULL(5, 0)	/* [한국어] 2단계 입력 주소 공간의 크기. */
#define STRTAB_STE_2_VTCR_S2SL0		GENMASK_ULL(7, 6)	/* [한국어] 2단계 표를 몇 단계부터 타는지. */
#define STRTAB_STE_2_VTCR_S2IR0		GENMASK_ULL(9, 8)	/* [한국어] 2단계 표 순회의 안쪽 캐시 정책. */
#define STRTAB_STE_2_VTCR_S2OR0		GENMASK_ULL(11, 10)	/* [한국어] 바깥 캐시 정책. */
#define STRTAB_STE_2_VTCR_S2SH0		GENMASK_ULL(13, 12)	/* [한국어] 공유 속성. */
#define STRTAB_STE_2_VTCR_S2TG		GENMASK_ULL(15, 14)	/* [한국어] 2단계 페이지 크기. */
#define STRTAB_STE_2_VTCR_S2PS		GENMASK_ULL(18, 16)	/* [한국어] 2단계 물리 주소 크기. */
#define STRTAB_STE_2_S2AA64		(1UL << 51)	/* [한국어] 2단계가 64비트 형식이다. */
#define STRTAB_STE_2_S2ENDI		(1UL << 52)	/* [한국어] 2단계 표를 빅 엔디언으로 읽는다. */
#define STRTAB_STE_2_S2PTW		(1UL << 54)	/* [한국어] 1단계 표 순회도 2단계로 변환한다 — 중첩 변환의 핵심 비트다. */
#define STRTAB_STE_2_S2S		(1UL << 57)	/* [한국어] 2단계 폴트에서 멈춰 세운다. */
#define STRTAB_STE_2_S2R		(1UL << 58)	/* [한국어] 2단계 오류를 기록한다. */

#define STRTAB_STE_3_S2TTB_MASK		GENMASK_ULL(51, 4)	/* [한국어] 2단계 변환 표의 기준 주소. */

/* These bits can be controlled by userspace for STRTAB_STE_0_CFG_NESTED */
/* [한국어] 중첩 변환에서 사용자가 정할 수 있는 낱말 0 의 비트들.
 * 게스트가 자기 문맥 표를 가리키게 하되, 그 밖의 설정은 커널이 쥔다.
 * 이 마스크가 곧 "게스트에게 맡겨도 안전한 범위"의 정의다. */
#define STRTAB_STE_0_NESTING_ALLOWED                                         \
	cpu_to_le64(STRTAB_STE_0_V | STRTAB_STE_0_CFG | STRTAB_STE_0_S1FMT | \
		    STRTAB_STE_0_S1CTXPTR_MASK | STRTAB_STE_0_S1CDMAX)
/* [한국어] 낱말 1 에서 사용자가 정할 수 있는 비트들.
 * 캐시 속성과 폴트 처리 방식, ATS 다루기가 여기 든다. */
#define STRTAB_STE_1_NESTING_ALLOWED                            \
	cpu_to_le64(STRTAB_STE_1_S1DSS | STRTAB_STE_1_S1CIR |   \
		    STRTAB_STE_1_S1COR | STRTAB_STE_1_S1CSH |   \
		    STRTAB_STE_1_S1STALLD | STRTAB_STE_1_EATS)	/* [한국어] 여기까지가 게스트가 STE 두 번째 워드에서 만질 수 있는 전부다. */

/*
 * Context descriptors.
 *
 * Linear: when less than 1024 SSIDs are supported
 * 2lvl: at most 1024 L1 entries,
 *       1024 lazy entries per table.
 */
#define CTXDESC_L2_ENTRIES		1024	/* [한국어] (위 영어 주석에 이어) 2단계 문맥 표 하나가 담는 항목 수. */

#define CTXDESC_L1_DESC_V		(1UL << 0)	/* [한국어] 1단계 항목이 유효하다. */
#define CTXDESC_L1_DESC_L2PTR_MASK	GENMASK_ULL(51, 12)	/* [한국어] 2단계 표의 주소 자리. */

#define CTXDESC_CD_DWORDS		8	/* [한국어] 문맥 서술자 하나의 크기(64비트 낱말 수). */

/* [한국어] 문맥 서술자 하나.
 *
 * 한 PASID 의 주소 공간을 정의한다 — 표 주소, ASID, 변환 규칙이 담긴다.
 * CPU 의 TTBR + TCR + MAIR 을 한 덩어리로 묶은 셈이다.
 *
 * PASID 를 쓰지 않는 장치도 0번 서술자 하나는 있어야 한다. */
struct arm_smmu_cd {
	/* [한국어] 여덟 개의 64비트 낱말.
	 *  설정자: CD 를 짓는 함수들. 읽는 자: 하드웨어. */
	__le64 data[CTXDESC_CD_DWORDS];
};

/* [한국어] 2단계 문맥 표의 아래쪽 표 하나. 1024개를 담는다. */
struct arm_smmu_cdtab_l2 {
	/* [한국어] 그 표의 서술자들. */
	struct arm_smmu_cd cds[CTXDESC_L2_ENTRIES];
};

/* [한국어] 2단계 문맥 표의 위쪽 항목 하나. */
struct arm_smmu_cdtab_l1 {
	/* [한국어] 아래쪽 표의 주소. 0 이면 아직 잡지 않았다. */
	__le64 l2ptr;
};

/*
 * [한국어]
 * arm_smmu_cdtab_l1_idx - PASID 에서 위쪽 첨자를 구한다
 *
 * @ssid: PASID.
 * @return: 1단계 표에서의 첨자.
 */
static inline unsigned int arm_smmu_cdtab_l1_idx(unsigned int ssid)
{
	return ssid / CTXDESC_L2_ENTRIES;	/* [한국어] 2단계 서술자 표 하나가 담는 PASID 수로 나눈다. */
}

/*
 * [한국어]
 * arm_smmu_cdtab_l2_idx - PASID 에서 아래쪽 첨자를 구한다
 *
 * @ssid: PASID.
 * @return: 2단계 표 안에서의 첨자.
 */
static inline unsigned int arm_smmu_cdtab_l2_idx(unsigned int ssid)
{
	return ssid % CTXDESC_L2_ENTRIES;	/* [한국어] 나머지가 그 표 안에서의 자리다. */
}

#define CTXDESC_CD_0_TCR_T0SZ		GENMASK_ULL(5, 0)	/* [한국어] 1단계 주소 공간의 크기. 64 에서 이 값을 뺀 만큼이 유효 비트 수다. */
#define CTXDESC_CD_0_TCR_TG0		GENMASK_ULL(7, 6)	/* [한국어] 1단계 페이지 크기. */
#define CTXDESC_CD_0_TCR_IRGN0		GENMASK_ULL(9, 8)	/* [한국어] 표 순회의 안쪽 캐시 정책. */
#define CTXDESC_CD_0_TCR_ORGN0		GENMASK_ULL(11, 10)	/* [한국어] 바깥 캐시 정책. */
#define CTXDESC_CD_0_TCR_SH0		GENMASK_ULL(13, 12)	/* [한국어] 표 순회의 공유 속성. */
#define CTXDESC_CD_0_TCR_EPD0		(1ULL << 14)	/* [한국어] TTBR0 쪽 표 순회를 끈다. */
#define CTXDESC_CD_0_TCR_EPD1		(1ULL << 30)	/* [한국어] TTBR1 쪽 표 순회를 끈다. SMMU 는 대개 TTBR0 만 써서 이쪽을 끈다. */

#define CTXDESC_CD_0_ENDI		(1UL << 15)	/* [한국어] 표를 빅 엔디언으로 읽는다. */
#define CTXDESC_CD_0_V			(1UL << 31)	/* [한국어] 이 서술자가 유효하다. */

#define CTXDESC_CD_0_TCR_IPS		GENMASK_ULL(34, 32)	/* [한국어] 중간 물리 주소 크기. */
#define CTXDESC_CD_0_TCR_TBI0		(1ULL << 38)	/* [한국어] 상위 바이트를 무시한다. 태그 포인터를 쓰는 경우다. */

#define CTXDESC_CD_0_TCR_HA            (1UL << 43)	/* [한국어] 하드웨어가 접근 비트를 갱신한다. */
#define CTXDESC_CD_0_TCR_HD            (1UL << 42)	/* [한국어] 하드웨어가 더티 비트를 갱신한다. 마이그레이션의 바탕이다. */

#define CTXDESC_CD_0_AA64		(1UL << 41)	/* [한국어] 64비트 형식이다. */
#define CTXDESC_CD_0_S			(1UL << 44)	/* [한국어] 폴트에서 멈춰 세운다. */
#define CTXDESC_CD_0_R			(1UL << 45)	/* [한국어] 오류를 기록한다. */
#define CTXDESC_CD_0_A			(1UL << 46)	/* [한국어] 접근 비트 오류를 하드웨어가 다룬다. */
#define CTXDESC_CD_0_ASET		(1UL << 47)	/* [한국어] 이 ASID 를 전역이 아닌 것으로 표시한다 — 무효화가 이 문맥에만 미친다. */
#define CTXDESC_CD_0_ASID		GENMASK_ULL(63, 48)	/* [한국어] 이 문맥의 ASID. */

#define CTXDESC_CD_1_TTB0_MASK		GENMASK_ULL(51, 4)	/* [한국어] 1단계 변환 표의 기준 주소. */

/*
 * When the SMMU only supports linear context descriptor tables, pick a
 * reasonable size limit (64kB).
 */
#define CTXDESC_LINEAR_CDMAX		ilog2(SZ_64K / sizeof(struct arm_smmu_cd))	/* [한국어] (위 영어 주석에 이어) 1단계 문맥 표의 크기 상한. 64KB 를 서술자 크기로 나눈 값의 로그다. */

/* Command queue */
#define CMDQ_ENT_SZ_SHIFT		4	/* [한국어] 명령 하나의 크기(로그 값). 16바이트다. */
#define CMDQ_ENT_DWORDS			((1 << CMDQ_ENT_SZ_SHIFT) >> 3)	/* [한국어] 그것을 64비트 낱말 수로 옮긴 값. */
#define CMDQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - CMDQ_ENT_SZ_SHIFT)	/* [한국어] 명령 큐의 최대 크기(항목 수의 로그). */

#define CMDQ_CONS_ERR			GENMASK(30, 24)	/* [한국어] 명령 처리 중 난 오류의 종류. */
#define CMDQ_ERR_CERROR_NONE_IDX	0	/* [한국어] 오류 없음. */
#define CMDQ_ERR_CERROR_ILL_IDX		1	/* [한국어] 잘못된 명령이다. */
#define CMDQ_ERR_CERROR_ABT_IDX		2	/* [한국어] 명령을 읽다 중단됐다 — 큐 메모리가 잘못됐다. */
#define CMDQ_ERR_CERROR_ATC_INV_IDX	3	/* [한국어] ATC 무효화가 시간을 다했다. 장치가 응답하지 않는다는 뜻이다. */

#define CMDQ_PROD_OWNED_FLAG		Q_OVERFLOW_FLAG	/* [한국어] 이 CPU 가 그 자리를 차지했음을 알리는 비트. 락 없는 큐 삽입에 쓴다. */

/*
 * This is used to size the command queue and therefore must be at least
 * BITS_PER_LONG so that the valid_map works correctly (it relies on the
 * total number of queue entries being a multiple of BITS_PER_LONG).
 */
#define CMDQ_BATCH_ENTRIES		BITS_PER_LONG	/* [한국어] (위 영어 주석에 이어) 한 번에 넣는 명령 수. 유효 비트맵이 낱말 단위로 도는 탓에 최소 BITS_PER_LONG 이어야 한다. */

#define CMDQ_0_OP			GENMASK_ULL(7, 0)	/* [한국어] 명령 종류. 모든 명령의 첫 바이트다. */
#define CMDQ_0_SSV			(1UL << 11)	/* [한국어] PASID 필드가 유효하다. */

#define CMDQ_PREFETCH_0_SID		GENMASK_ULL(63, 32)	/* [한국어] 미리 읽을 스트림 id. */
#define CMDQ_PREFETCH_1_SIZE		GENMASK_ULL(4, 0)	/* [한국어] 미리 읽을 범위의 크기. */
#define CMDQ_PREFETCH_1_ADDR_MASK	GENMASK_ULL(63, 12)	/* [한국어] 그 주소. */

#define CMDQ_CFGI_0_SSID		GENMASK_ULL(31, 12)	/* [한국어] 설정 무효화의 대상 PASID. */
#define CMDQ_CFGI_0_SID			GENMASK_ULL(63, 32)	/* [한국어] 그 스트림 id. */
#define CMDQ_CFGI_1_LEAF		(1UL << 0)	/* [한국어] 마지막 단계만 무효화한다. */
#define CMDQ_CFGI_1_RANGE		GENMASK_ULL(4, 0)	/* [한국어] 한 번에 무효화할 범위(로그 값). */

#define CMDQ_TLBI_0_NUM			GENMASK_ULL(16, 12)	/* [한국어] 범위 무효화의 개수 부분. */
#define CMDQ_TLBI_RANGE_NUM_MAX		31	/* [한국어] 그 최댓값. 이보다 크면 명령을 나눠야 한다. */
#define CMDQ_TLBI_0_SCALE		GENMASK_ULL(24, 20)	/* [한국어] 범위 무효화의 배율 부분. 개수와 배율을 곱해 실제 범위가 정해진다. */
#define CMDQ_TLBI_0_VMID		GENMASK_ULL(47, 32)	/* [한국어] 2단계 무효화의 VMID. */
#define CMDQ_TLBI_0_ASID		GENMASK_ULL(63, 48)	/* [한국어] 1단계 무효화의 ASID. */
#define CMDQ_TLBI_1_LEAF		(1UL << 0)	/* [한국어] 마지막 단계만 비운다. */
#define CMDQ_TLBI_1_TTL			GENMASK_ULL(9, 8)	/* [한국어] 표의 어느 단계인지 알려 준다. 하드웨어가 그 단계만 뒤져 빨라진다. */
#define CMDQ_TLBI_1_TG			GENMASK_ULL(11, 10)	/* [한국어] 페이지 크기. 범위 무효화에 필요하다. */
#define CMDQ_TLBI_1_VA_MASK		GENMASK_ULL(63, 12)	/* [한국어] 1단계 무효화의 가상 주소. */
#define CMDQ_TLBI_1_IPA_MASK		GENMASK_ULL(51, 12)	/* [한국어] 2단계 무효화의 중간 물리 주소. */

#define CMDQ_ATC_0_SSID			GENMASK_ULL(31, 12)	/* [한국어] ATC 무효화의 대상 PASID. */
#define CMDQ_ATC_0_SID			GENMASK_ULL(63, 32)	/* [한국어] 그 스트림 id. */
#define CMDQ_ATC_0_GLOBAL		(1UL << 9)	/* [한국어] 그 장치의 캐시를 통째로 비운다. */
#define CMDQ_ATC_1_SIZE			GENMASK_ULL(5, 0)	/* [한국어] 비울 범위의 크기(로그 값). */
#define CMDQ_ATC_1_ADDR_MASK		GENMASK_ULL(63, 12)	/* [한국어] 그 주소. */

#define CMDQ_PRI_0_SSID			GENMASK_ULL(31, 12)	/* [한국어] 페이지 요청 응답의 PASID. */
#define CMDQ_PRI_0_SID			GENMASK_ULL(63, 32)	/* [한국어] 그 스트림 id. */
#define CMDQ_PRI_1_GRPID		GENMASK_ULL(8, 0)	/* [한국어] 하드웨어가 붙인 요청 묶음 번호. */
#define CMDQ_PRI_1_RESP			GENMASK_ULL(13, 12)	/* [한국어] 응답 내용. */

#define CMDQ_RESUME_0_RESP_TERM		0UL	/* [한국어] 멈춘 트랜잭션을 끝낸다. */
#define CMDQ_RESUME_0_RESP_RETRY	1UL	/* [한국어] 다시 시도하게 한다 — 매핑을 만들어 준 뒤 쓴다. */
#define CMDQ_RESUME_0_RESP_ABORT	2UL	/* [한국어] 중단시킨다. 장치에 오류를 알린다. */
#define CMDQ_RESUME_0_RESP		GENMASK_ULL(13, 12)	/* [한국어] 그 응답 필드. */
#define CMDQ_RESUME_0_SID		GENMASK_ULL(63, 32)	/* [한국어] 대상 스트림 id. */
#define CMDQ_RESUME_1_STAG		GENMASK_ULL(15, 0)	/* [한국어] 멈춤 태그. 어느 트랜잭션인지 가리킨다. */

#define CMDQ_SYNC_0_CS			GENMASK_ULL(13, 12)	/* [한국어] 완료를 어떻게 알릴지. */
#define CMDQ_SYNC_0_CS_NONE		0	/* [한국어] 알리지 않는다. 큐 소비 포인터로만 안다. */
#define CMDQ_SYNC_0_CS_IRQ		1	/* [한국어] MSI 로 알린다. */
#define CMDQ_SYNC_0_CS_SEV		2	/* [한국어] 이벤트로 깨운다 — WFE 로 기다리는 CPU 를 깨운다. */
#define CMDQ_SYNC_0_MSH			GENMASK_ULL(23, 22)	/* [한국어] 그 MSI 쓰기의 공유 속성. */
#define CMDQ_SYNC_0_MSIATTR		GENMASK_ULL(27, 24)	/* [한국어] 그 메모리 속성. */
#define CMDQ_SYNC_0_MSIDATA		GENMASK_ULL(63, 32)	/* [한국어] MSI 로 쓸 값. 이것으로 어느 동기화인지 가린다. */
#define CMDQ_SYNC_1_MSIADDR_MASK	GENMASK_ULL(51, 2)	/* [한국어] 그 주소. */

/* Event queue */
#define EVTQ_ENT_SZ_SHIFT		5	/* [한국어] 이벤트 하나의 크기(로그 값). 32바이트다. */
#define EVTQ_ENT_DWORDS			((1 << EVTQ_ENT_SZ_SHIFT) >> 3)	/* [한국어] 그것을 64비트 낱말 수로. */
#define EVTQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - EVTQ_ENT_SZ_SHIFT)	/* [한국어] 이벤트 큐의 최대 크기. */

#define EVTQ_0_ID			GENMASK_ULL(7, 0)	/* [한국어] 이벤트 종류. 아래 값들이 무엇이 잘못됐는지 말해 준다. */

#define EVT_ID_BAD_STREAMID_CONFIG	0x02	/* [한국어] 스트림 id 가 표 범위를 벗어났다. */
#define EVT_ID_STE_FETCH_FAULT		0x03	/* [한국어] 스트림 표 항목을 읽지 못했다. */
#define EVT_ID_BAD_STE_CONFIG		0x04	/* [한국어] 그 항목의 설정이 잘못됐다. */
#define EVT_ID_STREAM_DISABLED_FAULT	0x06	/* [한국어] 그 스트림이 꺼져 있다. */
#define EVT_ID_BAD_SUBSTREAMID_CONFIG	0x08	/* [한국어] PASID 가 표 범위를 벗어났다. */
#define EVT_ID_CD_FETCH_FAULT		0x09	/* [한국어] 문맥 서술자를 읽지 못했다. */
#define EVT_ID_BAD_CD_CONFIG		0x0a	/* [한국어] 그 서술자의 설정이 잘못됐다. */
#define EVT_ID_TRANSLATION_FAULT	0x10	/* [한국어] 매핑이 없다 — 가장 흔한 오류다. */
#define EVT_ID_ADDR_SIZE_FAULT		0x11	/* [한국어] 주소가 지원 범위를 넘었다. */
#define EVT_ID_ACCESS_FAULT		0x12	/* [한국어] 접근 비트가 0 이었다. */
#define EVT_ID_PERMISSION_FAULT		0x13	/* [한국어] 매핑은 있지만 그 접근이 허용되지 않았다. */
#define EVT_ID_VMS_FETCH_FAULT		0x25	/* [한국어] 가상 머신 구조를 읽지 못했다. */

#define EVTQ_0_SSV			(1UL << 11)	/* [한국어] PASID 필드가 유효하다. */
#define EVTQ_0_SSID			GENMASK_ULL(31, 12)	/* [한국어] 오류를 낸 PASID. */
#define EVTQ_0_SID			GENMASK_ULL(63, 32)	/* [한국어] 오류를 낸 스트림 id. */
#define EVTQ_1_STAG			GENMASK_ULL(15, 0)	/* [한국어] 멈춤 태그. 응답할 때 이 값으로 짝을 찾는다. */
#define EVTQ_1_STALL			(1UL << 31)	/* [한국어] 그 트랜잭션이 멈춰 서 있다. */
#define EVTQ_1_PnU			(1UL << 33)	/* [한국어] 특권 없는 접근이었다. */
#define EVTQ_1_InD			(1UL << 34)	/* [한국어] 명령 인출이었다. */
#define EVTQ_1_RnW			(1UL << 35)	/* [한국어] 읽기였다. 없으면 쓰기. */
#define EVTQ_1_S2			(1UL << 39)	/* [한국어] 2단계에서 난 오류다. */
#define EVTQ_1_CLASS			GENMASK_ULL(41, 40)	/* [한국어] 어느 종류의 접근에서 났는가. */
#define EVTQ_1_CLASS_TT			0x01	/* [한국어] 표 순회 중에 났다는 값. */
#define EVTQ_1_TT_READ			(1UL << 44)	/* [한국어] 그 표 순회가 읽기였다. */
#define EVTQ_2_ADDR			GENMASK_ULL(63, 0)	/* [한국어] 오류가 난 주소. */
#define EVTQ_3_IPA			GENMASK_ULL(51, 12)	/* [한국어] 중간 물리 주소. 2단계 오류에서 뜻이 있다. */
#define EVTQ_3_FETCH_ADDR		GENMASK_ULL(51, 3)	/* [한국어] 표를 읽다 난 오류일 때 그 표의 주소. */

/* PRI queue */
#define PRIQ_ENT_SZ_SHIFT		4	/* [한국어] 페이지 요청 하나의 크기(로그 값). 16바이트다. */
#define PRIQ_ENT_DWORDS			((1 << PRIQ_ENT_SZ_SHIFT) >> 3)	/* [한국어] 그것을 64비트 낱말 수로. */
#define PRIQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - PRIQ_ENT_SZ_SHIFT)	/* [한국어] 페이지 요청 큐의 최대 크기. */

#define PRIQ_0_SID			GENMASK_ULL(31, 0)	/* [한국어] 요청한 장치의 스트림 id. */
#define PRIQ_0_SSID			GENMASK_ULL(51, 32)	/* [한국어] 그 PASID. */
#define PRIQ_0_PERM_PRIV		(1UL << 58)	/* [한국어] 특권 접근을 요구한다. */
#define PRIQ_0_PERM_EXEC		(1UL << 59)	/* [한국어] 실행 권한을 요구한다. */
#define PRIQ_0_PERM_READ		(1UL << 60)	/* [한국어] 읽기 권한을 요구한다. */
#define PRIQ_0_PERM_WRITE		(1UL << 61)	/* [한국어] 쓰기 권한을 요구한다. */
#define PRIQ_0_PRG_LAST			(1UL << 62)	/* [한국어] 이 묶음의 마지막 요청이다. 응답은 묶음 단위로 한다. */
#define PRIQ_0_SSID_V			(1UL << 63)	/* [한국어] PASID 필드가 유효하다. */

#define PRIQ_1_PRG_IDX			GENMASK_ULL(8, 0)	/* [한국어] 묶음 안에서의 번호. */
#define PRIQ_1_ADDR_MASK		GENMASK_ULL(63, 12)	/* [한국어] 요청한 주소. */

/* High-level queue structures */
#define ARM_SMMU_POLL_TIMEOUT_US	1000000 /* 1s! */	/* [한국어] 명령 완료를 기다리는 최대 시간. 주석대로 1초다 — 그만큼 걸리면 하드웨어가 멈춘 것이다. */
#define ARM_SMMU_POLL_SPIN_COUNT	10	/* [한국어] 잠들기 전에 돌며 기다릴 횟수. */

#define MSI_IOVA_BASE			0x8000000	/* [한국어] MSI doorbell 을 놓을 IOVA. 커널이 정하고 사용자는 그 자리를 쓸 수 없다. */
#define MSI_IOVA_LENGTH			0x100000	/* [한국어] 그 창의 크기(1MB). */

/* [한국어] 페이지 요청에 대한 응답. */
enum pri_resp {
	/* [한국어] 거절한다. 그 주소는 매핑할 수 없다. */
	PRI_RESP_DENY = 0,
	/* [한국어] 실패했다. 장치가 그 요청을 접는다. */
	PRI_RESP_FAIL = 1,
	/* [한국어] 성공했다. 매핑을 만들었으니 다시 시도하라. */
	PRI_RESP_SUCC = 2,
};

/* [한국어] 하드웨어에 보낼 명령 하나를 커널 쪽 형식으로 담은 것.
 *
 * 실제 명령은 16바이트의 비트 묶음이지만, 그것을 손으로 짜면 읽기 어렵고
 * 틀리기 쉽다. 그래서 이 구조체로 받아 arm_smmu_cmdq_build_cmd 가 옮긴다.
 *
 * 명령마다 필요한 인자가 달라 union 으로 겹쳐 두었다. 각 멤버 위에
 * 그 명령의 번호를 #define 으로 붙여, 어느 명령이 어느 인자를 쓰는지
 * 눈으로 바로 알 수 있다. */
struct arm_smmu_cmdq_ent {
	/* Common fields */
	u8				opcode;	/* [한국어] 명령 종류. 아래 CMDQ_OP_* 중 하나다. */
	bool				substream_valid;	/* [한국어] PASID 필드가 유효한가. */

	/* Command-specific fields */
	/* [한국어] 명령별 인자를 겹쳐 담는 union.
	 * 설정자: arm_smmu_domain_inv_range() 같은 상위 함수가 opcode 를 정한 뒤
	 *         그 opcode 에 맞는 멤버만 채운다.
	 * 읽는 자: arm_smmu_cmdq_build_cmd() 가 opcode 로 갈래를 나눠 해당 멤버만 읽고
	 *         64비트 두 워드로 짜 넣는다.
	 * 값 범위: 채워지지 않은 멤버의 값은 쓰레기다 — opcode 와 짝이 맞지 않는
	 *         멤버를 읽으면 안 된다.
	 * 동기화: 명령을 만드는 쪽의 지역 변수라 공유되지 않는다. */
	union {
		#define CMDQ_OP_PREFETCH_CFG	0x1	/* [한국어] 설정을 미리 읽어 두라는 명령. 첫 접근의 지연을 줄인다. */
		struct {
			/* [한국어] 설정을 미리 읽어 둘 스트림의 번호.
			 * 설정자: 장치를 붙인 직후 그 장치의 스트림 id 로 채운다.
			 * 읽는 자: 하드웨어가 이 번호로 스트림 표 항목을 찾아 캐시에 올린다.
			 * 값 범위: 0 ~ (1 << sid_bits) - 1.
			 * 동기화: 없음 — 값만 실어 보내는 인자다. */
			u32			sid;
		/* [한국어] CMDQ_OP_PREFETCH_CFG 의 인자. 그 스트림의 설정을 미리 읽어 둔다. */
		} prefetch;

		#define CMDQ_OP_CFGI_STE	0x3	/* [한국어] 스트림 표 항목 하나를 무효화한다. */
		#define CMDQ_OP_CFGI_ALL	0x4	/* [한국어] 모든 설정 캐시를 무효화한다. */
		#define CMDQ_OP_CFGI_CD		0x5	/* [한국어] 문맥 서술자 하나를 무효화한다. */
		#define CMDQ_OP_CFGI_CD_ALL	0x6	/* [한국어] 한 스트림의 모든 서술자를 무효화한다. */
		struct {
			/* [한국어] 무효화할 스트림의 번호.
			 * 설정자: 스트림 표 항목을 고쳐 쓴 뒤 그 스트림 번호로 채운다.
			 * 읽는 자: 하드웨어가 이 번호에 해당하는 설정 캐시를 버린다.
			 * 값 범위: CFGI_ALL 에서는 무시된다.
			 * 동기화: 없음. */
			u32			sid;
			/* [한국어] 문맥 서술자를 무효화할 때의 PASID.
			 * 설정자: CFGI_CD 를 만들 때 그 PASID 로 채운다.
			 * 읽는 자: 하드웨어가 그 스트림의 그 PASID 항목만 버린다.
			 * 값 범위: CFGI_STE 계열에서는 쓰이지 않는다.
			 * 동기화: 없음. */
			u32			ssid;
			/* [한국어] 한 항목만 지울지, 아래 구간을 통째로 지울지 겹쳐 둔 자리.
			 * 설정자: CD 계열은 leaf 를, STE 계열은 span 을 쓴다.
			 * 읽는 자: arm_smmu_cmdq_build_cmd() 가 opcode 로 갈래를 나눠 읽는다.
			 * 값 범위: 둘 중 하나만 유효하다.
			 * 동기화: 없음. */
			union {
				/* [한국어] 참이면 마지막 단계 항목만 버린다.
				 * 설정자: 표의 중간 단계를 건드리지 않았을 때 참으로 준다.
				 * 읽는 자: 하드웨어가 이 값에 따라 캐시를 버리는 범위를 좁힌다.
				 * 값 범위: true 는 잎만, false 는 걸어온 경로 전부.
				 * 동기화: 없음. */
				bool		leaf;
				/* [한국어] 한 번에 무효화할 스트림 번호 구간의 폭(2의 지수).
				 * 설정자: 여러 스트림 표 항목을 한꺼번에 지울 때 채운다.
				 * 읽는 자: 하드웨어가 sid 부터 2^span 개 항목을 버린다.
				 * 값 범위: 0 이면 항목 하나.
				 * 동기화: 없음. */
				u8		span;
			};
		/* [한국어] 설정 무효화 명령들의 인자. 스트림 표나 문맥 서술자를 지운다. */
		} cfgi;

		#define CMDQ_OP_TLBI_NH_ALL     0x10	/* [한국어] 비하이퍼바이저 TLB 를 통째로 비운다. */
		#define CMDQ_OP_TLBI_NH_ASID	0x11	/* [한국어] 그 ASID 의 항목을 비운다. */
		#define CMDQ_OP_TLBI_NH_VA	0x12	/* [한국어] ASID 와 주소로 비운다. */
		#define CMDQ_OP_TLBI_NH_VAA	0x13	/* [한국어] 주소로만 비운다 — 모든 ASID 에 걸친다. */
		#define CMDQ_OP_TLBI_EL2_ALL	0x20	/* [한국어] EL2 TLB 를 통째로 비운다. */
		#define CMDQ_OP_TLBI_EL2_ASID	0x21	/* [한국어] EL2 의 그 ASID 를 비운다. */
		#define CMDQ_OP_TLBI_EL2_VA	0x22	/* [한국어] EL2 를 주소로 비운다. */
		#define CMDQ_OP_TLBI_S12_VMALL	0x28	/* [한국어] 그 VMID 의 1·2단계를 모두 비운다. */
		#define CMDQ_OP_TLBI_S2_IPA	0x2a	/* [한국어] 2단계를 중간 물리 주소로 비운다. */
		#define CMDQ_OP_TLBI_NSNH_ALL	0x30	/* [한국어] 비보안·비하이퍼바이저 항목을 모두 비운다. 초기화 때 쓴다. */
		struct {
			/* [한국어] 범위 무효화에서 한 번에 지울 페이지 묶음의 개수 - 1.
			 * 설정자: arm_smmu_tlb_inv_range_domain() 이 크기를 나눠 채운다.
			 * 읽는 자: 하드웨어가 scale 과 함께 읽어 실제 범위를 계산한다.
			 * 값 범위: 0 ~ 31. 범위 무효화(RIL)를 지원할 때만 뜻이 있다.
			 * 동기화: 없음. */
			u8			num;
			/* [한국어] num 에 곱해질 배율(2의 지수).
			 * 설정자: 위와 같은 자리에서 남은 크기에 맞춰 정한다.
			 * 읽는 자: 하드웨어가 (num+1) << (5*scale) 개 알갱이를 지운다.
			 * 값 범위: 0 ~ 3.
			 * 동기화: 없음. */
			u8			scale;
			/* [한국어] 1단계 변환의 주소 공간 번호.
			 * 설정자: 1단계 도메인의 cd.asid 값을 그대로 옮겨 담는다.
			 * 읽는 자: 하드웨어가 그 ASID 로 태그된 항목만 버린다.
			 * 값 범위: SMMU 가 지원하는 ASID 폭 안.
			 * 동기화: 없음 — 값 복사다. */
			u16			asid;
			/* [한국어] 2단계 변환의 가상 기계 번호.
			 * 설정자: 2단계/중첩 도메인의 s2_cfg.vmid 를 옮겨 담는다.
			 * 읽는 자: 하드웨어가 그 VMID 로 태그된 항목만 버린다.
			 * 값 범위: vmid 비트맵에서 받은 값.
			 * 동기화: 없음. */
			u16			vmid;
			/* [한국어] 참이면 마지막 단계 변환만 버린다.
			 * 설정자: 표의 중간 단계를 고치지 않은 무효화에서 참으로 준다.
			 * 읽는 자: 하드웨어가 걸어온 경로 캐시를 살릴지 정한다.
			 * 값 범위: true / false.
			 * 동기화: 없음. */
			bool			leaf;
			/* [한국어] 무효화할 변환이 놓인 표의 단계(translation table level).
			 * 설정자: 페이지 크기에서 역산해 채운다.
			 * 읽는 자: 하드웨어가 그 단계만 골라 버려 무효화 비용을 줄인다.
			 * 값 범위: 0 이면 단계를 가리지 않는다.
			 * 동기화: 없음. */
			u8			ttl;
			/* [한국어] 알갱이 크기(translation granule) 코드.
			 * 설정자: 4K/16K/64K 중 그 도메인이 쓰는 크기로 채운다.
			 * 읽는 자: 하드웨어가 num·scale 과 함께 지울 범위를 계산한다.
			 * 값 범위: 0 이면 범위 무효화가 아니라 주소 하나를 지운다.
			 * 동기화: 없음. */
			u8			tg;
			/* [한국어] 무효화를 시작할 입력 주소.
			 * 설정자: 무효화 구간의 첫 주소를 알갱이 크기로 내림해 채운다.
			 * 읽는 자: 하드웨어가 이 주소부터 계산된 범위를 버린다.
			 * 값 범위: 알갱이 크기로 정렬되어야 한다.
			 * 동기화: 없음. */
			u64			addr;
		/* [한국어] TLB 무효화 명령들의 인자. 범위 무효화를 쓰면 num 과 scale 로 넓은 구간을 한 번에 비운다. */
		} tlbi;

		#define CMDQ_OP_ATC_INV		0x40	/* [한국어] 장치 쪽 변환 캐시를 무효화한다. */
		#define ATC_INV_SIZE_ALL	52	/* [한국어] 그 장치의 캐시 전체를 뜻하는 크기 값. */
		struct {
			/* [한국어] 캐시를 비울 장치의 스트림 번호.
			 * 설정자: ATS 를 켠 장치마다 하나씩 명령을 만들며 채운다.
			 * 읽는 자: 하드웨어가 그 장치에게 무효화 요청을 보낸다.
			 * 값 범위: 유효한 스트림 id.
			 * 동기화: 없음. */
			u32			sid;
			/* [한국어] 장치 캐시 무효화 대상 PASID.
			 * 설정자: PASID 별 무효화일 때 채우고, 아니면 global 을 쓴다.
			 * 읽는 자: 하드웨어가 그 PASID 로 태그된 장치 캐시만 지운다.
			 * 값 범위: global 이 참이면 무시된다.
			 * 동기화: 없음. */
			u32			ssid;
			/* [한국어] 장치 캐시에서 비울 구간의 시작 주소.
			 * 설정자: 무효화 구간을 size 가 표현할 수 있는 크기로 맞춰 자른다.
			 * 읽는 자: 하드웨어가 이 주소와 size 로 구간을 정한다.
			 * 값 범위: 2^size 로 정렬된 주소여야 한다.
			 * 동기화: 없음. */
			u64			addr;
			/* [한국어] 장치 캐시에서 비울 구간의 크기(2의 지수).
			 * 설정자: 구간 길이에서 계산하거나, 전부 비울 때 ATC_INV_SIZE_ALL 을 쓴다.
			 * 읽는 자: 하드웨어가 2^size 바이트를 대상으로 삼는다.
			 * 값 범위: 12(4K) ~ 52(전체).
			 * 동기화: 없음. */
			u8			size;
			/* [한국어] 참이면 PASID 를 가리지 않고 그 장치의 캐시를 모두 비운다.
			 * 설정자: 도메인 전체 무효화에서 참으로 준다.
			 * 읽는 자: 명령을 짤 때 ssid 필드를 무시하게 만든다.
			 * 값 범위: true / false.
			 * 동기화: 없음. */
			bool			global;
		/* [한국어] 장치 쪽 변환 캐시 무효화의 인자. */
		} atc;

		#define CMDQ_OP_PRI_RESP	0x41	/* [한국어] 페이지 요청에 응답한다. */
		struct {
			/* [한국어] 페이지 요청을 보냈던 장치의 스트림 번호.
			 * 설정자: PRI 큐에서 읽은 요청의 값을 그대로 옮긴다.
			 * 읽는 자: 하드웨어가 그 장치에게 응답을 돌려준다.
			 * 값 범위: 요청에 실려 온 값과 같아야 한다.
			 * 동기화: 없음. */
			u32			sid;
			/* [한국어] 요청을 보낸 PASID.
			 * 설정자: PRI 큐 항목에서 읽어 옮긴다.
			 * 읽는 자: 하드웨어가 어느 문맥의 요청인지 짝을 맞춘다.
			 * 값 범위: substream_valid 가 참일 때만 뜻이 있다.
			 * 동기화: 없음. */
			u32			ssid;
			/* [한국어] 페이지 요청 그룹 번호.
			 * 설정자: 마지막 요청에 실려 온 그룹 번호를 그대로 옮긴다.
			 * 읽는 자: 하드웨어가 그 그룹의 모든 요청을 이 응답으로 닫는다.
			 * 값 범위: 요청에 실려 온 값.
			 * 동기화: 없음 — 그룹 하나에 응답은 한 번뿐이다. */
			u16			grpid;
			/* [한국어] 응답의 종류 — 실패인지, 무효인지, 성공인지.
			 * 설정자: 폴트 처리기가 페이지를 채웠는지에 따라 정한다.
			 * 읽는 자: 하드웨어가 장치에게 그대로 전달한다.
			 * 값 범위: PRI_RESP_FAILURE / INVALID / SUCCESS.
			 * 동기화: 없음. */
			enum pri_resp		resp;
		/* [한국어] 페이지 요청 응답의 인자. */
		} pri;

		#define CMDQ_OP_RESUME		0x44	/* [한국어] 멈춘 트랜잭션을 다시 시작시키거나 끝낸다. */
		struct {
			/* [한국어] 멈춰 있는 트랜잭션을 낸 장치의 스트림 번호.
			 * 설정자: 이벤트 큐에서 읽은 폴트 기록의 값을 옮긴다.
			 * 읽는 자: 하드웨어가 그 장치의 멈춘 트랜잭션을 찾는다.
			 * 값 범위: 폴트 기록에 실려 온 값.
			 * 동기화: 없음. */
			u32			sid;
			/* [한국어] 멈춘 트랜잭션에 붙은 꼬리표(stall tag).
			 * 설정자: 폴트 기록에서 읽어 옮긴다 — 이 값이 트랜잭션의 신원이다.
			 * 읽는 자: 하드웨어가 이 꼬리표로 어느 트랜잭션을 깨울지 고른다.
			 * 값 범위: 폴트 기록의 값과 정확히 같아야 한다.
			 * 동기화: 없음 — 꼬리표 하나에 재개 명령은 한 번뿐이다. */
			u16			stag;
			/* [한국어] 멈춘 트랜잭션을 다시 굴릴지, 버릴지 고르는 값.
			 * 설정자: 페이지를 채웠으면 재시도, 못 채웠으면 중단으로 정한다.
			 * 읽는 자: 하드웨어가 그대로 따라 트랜잭션을 처리한다.
			 * 값 범위: CMDQ_RESUME_0_RESP_RETRY / _ABORT / _TERM.
			 * 동기화: 없음. */
			u8			resp;
		/* [한국어] 멈춘 트랜잭션을 다시 시작시키는 명령의 인자. */
		} resume;

		#define CMDQ_OP_CMD_SYNC	0x46	/* [한국어] 앞선 명령이 모두 끝나기를 기다린다. 무효화 뒤에 반드시 넣는다. */
		struct {
			/* [한국어] 완료를 알릴 자리의 물리 주소.
			 * 설정자: MSI 방식 완료 대기를 쓸 때 큐의 완료 표시 자리를 준다.
			 * 읽는 자: 하드웨어가 앞선 명령을 모두 끝낸 뒤 이 주소에 값을 쓴다.
			 * 값 범위: 0 이면 MSI 대신 레지스터 폴링으로 기다린다.
			 * 동기화: 쓰기가 보이는 시점이 곧 완료 시점이므로, 읽는 쪽은
			 *         메모리 장벽을 걸고 값을 확인한다. */
			u64			msiaddr;
		/* [한국어] 완료 대기 명령의 인자. MSI 주소를 주면 그 자리에 값을 써 알린다. */
		} sync;
	};
};

/* [한국어] 큐의 생산·소비 포인터.
 *
 * 두 32비트 값을 한 64비트로 겹쳐 두어, 한 번의 원자 연산으로 둘을 함께
 * 읽고 쓸 수 있다. 락 없는 명령 큐 삽입이 그 성질에 기댄다.
 *
 * 캐시 줄 정렬을 강제하는 것도 요점 — 여러 CPU 가 이 값을 다투므로,
 * 다른 자료와 같은 줄에 있으면 거짓 공유가 생긴다. */
struct arm_smmu_ll_queue {
	union {
			/* [한국어] 두 포인터를 한 번에 다루기 위한 겹침. */
		u64			val;
		struct {
			/* [한국어] 생산자가 어디까지 넣었는지. 명령 큐는 커널이, 이벤트 큐는 하드웨어가 올린다. */
			u32		prod;
			/* [한국어] 소비자가 어디까지 처리했는지. */
			u32		cons;
		};
		struct {
			/* [한국어] 원자 연산으로 다룰 때의 같은 자리. */
			atomic_t	prod;
			/* [한국어] 같음. */
			atomic_t	cons;
		} atomic;
			/* [한국어] 캐시 줄을 통째로 차지하게 만드는 채움. 거짓 공유를 막는다. */
		u8			__pad[SMP_CACHE_BYTES];
	} ____cacheline_aligned_in_smp;
	/* [한국어] 큐 크기(항목 수)의 로그 값.
	 *  설정자: 큐를 만들 때. 읽는 자: 포인터에서 첨자를 꺼내는 매크로들. */
	u32				max_n_shift;
};

/* [한국어] 하드웨어와 주고받는 원형 큐 하나.
 *
 * 명령·이벤트·페이지 요청 세 큐가 모두 이 구조를 쓴다. 다른 것은 항목
 * 크기와 방향뿐이다. */
struct arm_smmu_queue {
	/* [한국어] 생산·소비 포인터. 자주 다투는 값이라 따로 떼어 두었다. */
	struct arm_smmu_ll_queue	llq;
	/* [한국어] (위 영어 주석 참고) 이 큐의 인터럽트 번호.
	 *  MSI 를 쓰면 그쪽으로, 아니면 이 선으로 알린다. */
	int				irq; /* Wired interrupt */

	/* [한국어] 큐 메모리의 커널 주소.
	 *  설정자: 큐를 만들 때 DMA 로 잡는다. */
	__le64				*base;
	/* [한국어] 그 메모리의 장치 쪽 주소. 하드웨어에 알려 준다. */
	dma_addr_t			base_dma;
	/* [한국어] 기준 주소 레지스터에 쓸 값. 주소와 크기가 함께 담긴다. */
	u64				q_base;

	/* [한국어] 항목 하나의 크기(64비트 낱말 수).
	 *  큐마다 달라 매크로가 이 값으로 주소를 계산한다. */
	size_t				ent_dwords;

	/* [한국어] 생산 포인터 레지스터의 주소. */
	u32 __iomem			*prod_reg;
	/* [한국어] 소비 포인터 레지스터의 주소. */
	u32 __iomem			*cons_reg;
};

/* [한국어] 큐를 기다리는 동안의 상태.
 *
 * 시간 제한과 물러남 방식을 담는다. 하드웨어가 이벤트로 깨워 줄 수 있으면
 * WFE 로 자고, 아니면 돌면서 기다린다. */
struct arm_smmu_queue_poll {
	/* [한국어] 언제까지 기다릴지. */
	ktime_t				timeout;
	/* [한국어] 다음에 잠들 시간. 두 배씩 늘려 간다. */
	unsigned int			delay;
	/* [한국어] 잠들기 전에 돌며 기다릴 남은 횟수. */
	unsigned int			spin_cnt;
	/* [한국어] WFE 로 잘 수 있는가.
	 *  하드웨어가 SEV 를 지원하면 참이다 — 그때는 돌지 않고 자도 제때 깨어난다. */
	bool				wfe;
};

/* [한국어] 명령 큐.
 *
 * 여러 CPU 가 락 없이 명령을 넣을 수 있게 만든 것이 이 구조의 핵심이다.
 * 각 CPU 가 자기 자리를 원자적으로 차지하고, 유효 비트맵으로 "내 명령이
 * 다 쓰였다"를 알린 뒤, 한 CPU 가 대표로 하드웨어에 알린다. */
struct arm_smmu_cmdq {
	/* [한국어] 명령 큐의 공통 부분 — 링 버퍼의 주소, 생산·소비 포인터, 레지스터 위치.
	 * 설정자: arm_smmu_cmdq_init() 이 arm_smmu_init_one_queue() 로 채운다.
	 * 읽는 자: 명령을 넣는 모든 CPU 와, 하드웨어에 생산 포인터를 알리는 대표 CPU.
	 * 값 범위: q.llq.max_n_shift 로 정해진 크기의 링. 항목은 2워드(16바이트)다.
	 * 동기화: llq 의 prod/cons 는 원자 연산으로 다루고, 아래 valid_map 이
	 *         "내 자리는 다 쓰였다"를 알리는 역할을 맡는다. */
	struct arm_smmu_queue		q;
	/* [한국어] 각 자리의 명령이 다 쓰였는지 알리는 비트맵.
	 *  설정자: 명령을 다 쓴 CPU 가 자기 비트를 뒤집는다.
	 *  읽는 자: 대표 CPU 가 앞선 명령이 모두 준비됐는지 확인할 때.
	 *  한 바퀴 돌 때마다 비트의 뜻이 뒤집히는 방식이라 지우는 일이 없다. */
	atomic_long_t			*valid_map;
	/* [한국어] 대표 CPU 가 하드웨어에 알릴 지점.
	 *  자리를 차지한 CPU 중 하나가 대표가 되어 여기까지를 한꺼번에 알린다. */
	atomic_t			owner_prod;
	/* [한국어] 큐가 찼을 때만 쓰는 완만한 락.
	 *  평소에는 잡히지 않아 락 없는 경로가 유지된다. */
	atomic_t			lock;
	/* [한국어] 이 큐가 그 명령을 받을 수 있는지 묻는 콜백.
	 *  Tegra 의 보조 큐처럼 일부 명령만 받는 큐가 있어 필요하다.
	 *  NULL 이면 모든 명령을 받는다. */
	bool				(*supports_cmd)(struct arm_smmu_cmdq_ent *ent);
};

/*
 * [한국어]
 * arm_smmu_cmdq_supports_cmd - 이 큐가 그 명령을 받는가
 *
 * @cmdq: 대상 큐.
 * @ent: 보낼 명령.
 * @return: 받을 수 있으면 참.
 *
 * 콜백이 없으면 모두 받는 것으로 본다 — 흔한 경우를 짧게 쓰려는 방식이다.
 */
static inline bool arm_smmu_cmdq_supports_cmd(struct arm_smmu_cmdq *cmdq,
					      struct arm_smmu_cmdq_ent *ent)
{
	return cmdq->supports_cmd ? cmdq->supports_cmd(ent) : true;	/* [한국어] 콜백이 없으면 모든 명령을 받는다고 본다 — 기본 큐가 그렇다. */
}

/* [한국어] 한 번에 보낼 명령 묶음.
 *
 * 명령을 하나씩 넣으면 큐 포인터를 다투는 비용이 커, 여러 개를 모아
 * 한 번에 넣는다. 무효화가 대표적이다. */
struct arm_smmu_cmdq_batch {
	/* [한국어] 모아 둔 명령들. 이미 하드웨어 형식으로 옮겨져 있다. */
	u64				cmds[CMDQ_BATCH_ENTRIES * CMDQ_ENT_DWORDS];
	/* [한국어] 보낼 큐. 보조 큐가 있는 하드웨어에서는 명령마다 달라질 수 있다. */
	struct arm_smmu_cmdq		*cmdq;
	/* [한국어] 모아 둔 개수. */
	int				num;
};

/*
 * The order here also determines the sequence in which commands are sent to the
 * command queue. E.g. TLBI must be done before ATC_INV.
 */
/* [한국어]
 * (위 영어 주석에 이어) 무효화의 종류.
 *
 * 원 주석대로 이 순서가 곧 명령을 보내는 순서다 — TLB 를 먼저 비우고
 * 장치 캐시를 나중에 비워야, 그 사이 장치가 낡은 항목을 다시 받아 오지
 * 않는다. */
enum arm_smmu_inv_type {
	/* [한국어] 1단계 ASID 단위 무효화. */
	INV_TYPE_S1_ASID,
	/* [한국어] 2단계 VMID 단위 무효화. */
	INV_TYPE_S2_VMID,
	/* [한국어] 2단계를 비우면서 그 아래 1단계 항목도 함께 지운다.
	 *  중첩 변환에서 2단계가 바뀌면 1단계 결과도 못 믿기 때문이다. */
	INV_TYPE_S2_VMID_S1_CLEAR,
	/* [한국어] 장치 쪽 변환 캐시를 주소 범위로 비운다. */
	INV_TYPE_ATS,
	/* [한국어] 그 장치의 캐시를 통째로 비운다. */
	INV_TYPE_ATS_FULL,
};

/* [한국어] 무효화 하나를 어떻게 보낼지 적어 둔 항목.
 *
 * 도메인마다 이 항목들의 배열을 들고 있어, 무효화가 필요할 때 그 배열을
 * 훑으며 명령을 만든다. 매번 어디에 무엇을 보낼지 계산하지 않아 빠르다. */
struct arm_smmu_inv {
	/* [한국어] 이 무효화를 보낼 SMMU.
	 *  한 도메인이 여러 SMMU 에 걸칠 수 있어 항목마다 들고 있다. */
	struct arm_smmu_device *smmu;
	/* [한국어] 위 enum 의 종류. 정렬 순서를 정하는 열쇠이기도 하다. */
	u8 type;
	/* [한국어] 범위를 지정한 무효화의 명령 번호. */
	u8 size_opcode;
	/* [한국어] 범위 없이 통째로 비우는 명령 번호.
	 *  구간이 넓으면 통째로 비우는 편이 빨라 둘을 함께 들고 있다. */
	u8 nsize_opcode;
	/* [한국어] (위 영어 주석 참고) 종류에 따라 ASID, VMID, 스트림 id 중 하나. */
	u32 id; /* ASID or VMID or SID */
	union {
	/* [한국어] (위 영어 주석 참고) 범위 무효화에 쓸 페이지 크기. */
		size_t pgsize; /* ARM_SMMU_FEAT_RANGE_INV */
	/* [한국어] (위 영어 주석 참고) ATS 무효화의 PASID. */
		u32 ssid; /* INV_TYPE_ATS */
	};

	/* [한국어] (위 영어 주석 참고) 이 항목을 필요로 하는 장치 수.
	 *  0 이 되면 쓰레기로 표시되어 나중에 걷힌다.
	 *  곧바로 지우지 않는 이유: 무효화 경로가 락 없이 이 배열을 훑고 있다. */
	int users; /* users=0 to mark as a trash to be purged */
};

/*
 * [한국어]
 * arm_smmu_inv_is_ats - 이 항목이 장치 캐시 무효화인가
 *
 * @inv: 그 항목.
 * @return: 맞으면 참.
 *
 * ATS 무효화는 장치의 응답을 기다려야 해서 다른 것들과 다르게 다뤄진다.
 */
static inline bool arm_smmu_inv_is_ats(const struct arm_smmu_inv *inv)
{
	return inv->type == INV_TYPE_ATS || inv->type == INV_TYPE_ATS_FULL;	/* [한국어] 장치 캐시 무효화만 따로 골라내야 하는 곳이 많아 조건을 함수로 묶었다. */
}

/**
 * struct arm_smmu_invs - Per-domain invalidation array
 * @max_invs: maximum capacity of the flexible array
 * @num_invs: number of invalidations in the flexible array. May be smaller than
 *            @max_invs after a tailing trash entry is excluded, but must not be
 *            greater than @max_invs
 * @num_trashes: number of trash entries in the array for arm_smmu_invs_purge().
 *               Must not be greater than @num_invs
 * @rwlock: optional rwlock to fence ATS operations
 * @has_ats: flag if the array contains an INV_TYPE_ATS or INV_TYPE_ATS_FULL
 * @rcu: rcu head for kfree_rcu()
 * @inv: flexible invalidation array
 *
 * The arm_smmu_invs is an RCU data structure. During a ->attach_dev callback,
 * arm_smmu_invs_merge(), arm_smmu_invs_unref() and arm_smmu_invs_purge() will
 * be used to allocate a new copy of an old array for addition and deletion in
 * the old domain's and new domain's invs arrays.
 *
 * The arm_smmu_invs_unref() mutates a given array, by internally reducing the
 * users counts of some given entries. This exists to support a no-fail routine
 * like attaching to an IOMMU_DOMAIN_BLOCKED. And it could pair with a followup
 * arm_smmu_invs_purge() call to generate a new clean array.
 *
 * Concurrent invalidation thread will push every invalidation described in the
 * array into the command queue for each invalidation event. It is designed like
 * this to optimize the invalidation fast path by avoiding locks.
 *
 * A domain can be shared across SMMU instances. When an instance gets removed,
 * it would delete all the entries that belong to that SMMU instance. Then, a
 * synchronize_rcu() would have to be called to sync the array, to prevent any
 * concurrent invalidation thread accessing the old array from issuing commands
 * to the command queue of a removed SMMU instance.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * 도메인마다의 무효화 배열.
 *
 * 원 주석이 설계를 밝혀 두었다. 요점은 무효화 빠른 경로에서 락을 잡지
 * 않는다는 것이다 — 그 대신 배열을 통째로 갈아 끼우고 RCU 로 옛 것을
 * 늦게 놓는다.
 *
 * unref 가 배열을 그 자리에서 고치는 것은 실패할 수 없는 경로(차단
 * 도메인 붙이기)를 위해서다. 그렇게 표시만 해 두고, 나중에 purge 가
 * 깨끗한 새 배열을 만든다.
 *
 * SMMU 하나가 빠질 때 synchronize_rcu 가 필요한 이유도 원 주석에 있다 —
 * 옛 배열을 보고 있던 스레드가 이미 사라진 SMMU 의 큐에 명령을 넣으면
 * 안 되기 때문이다. */
struct arm_smmu_invs {
	/* [한국어] (위 kernel-doc 참고) 배열의 용량. */
	size_t max_invs;
	/* [한국어] (위 kernel-doc 참고) 실제로 든 항목 수. */
	size_t num_invs;
	/* [한국어] (위 kernel-doc 참고) 걷어 내야 할 쓰레기 항목 수. */
	size_t num_trashes;
	/* [한국어] (위 kernel-doc 참고) ATS 연산을 막는 선택적 락.
	 *  ATS 무효화는 장치 응답을 기다려, 그 사이 장치가 떨어지면 안 된다. */
	rwlock_t rwlock;
	/* [한국어] (위 kernel-doc 참고) ATS 항목이 하나라도 있는가.
	 *  없으면 위 락을 아예 잡지 않아 빠르다. */
	bool has_ats;
	/* [한국어] (위 kernel-doc 참고) 늦은 해제에 쓰는 머리. */
	struct rcu_head rcu;
	/* [한국어] (위 kernel-doc 참고) 항목들.
	 *  __counted_by 로 그 개수를 알려, 컴파일러가 범위 검사를 넣을 수 있다. */
	struct arm_smmu_inv inv[] __counted_by(max_invs);
};

/*
 * [한국어]
 * arm_smmu_invs_alloc - 무효화 배열을 잡는다
 *
 * @num_invs: 담을 항목 수.
 * @return: 잡은 배열, 실패하면 NULL.
 *
 * 용량과 개수를 같게 두고 시작한다 — 나중에 항목이 걷히면 개수만 줄어든다.
 */
static inline struct arm_smmu_invs *arm_smmu_invs_alloc(size_t num_invs)
{
	struct arm_smmu_invs *new_invs;	/* [한국어] 잡은 배열을 담아 둘 지역 포인터 — 실패하면 NULL 로 돌려준다. */

	new_invs = kzalloc(struct_size(new_invs, inv, num_invs), GFP_KERNEL);	/* [한국어] 머리와 가변 배열을 한 덩이로 잡는다 — struct_size 가 곱셈 넘침까지 막아 준다. */
	if (!new_invs)	/* [한국어] 메모리가 없으면 무효화 배열을 바꿀 수 없다 — 호출자가 붙이기를 접는다. */
		return NULL;
	new_invs->max_invs = num_invs;	/* [한국어] 실제로 잡은 자리 수. 이 값이 곧 배열의 물리적 한계다. */
	new_invs->num_invs = num_invs;	/* [한국어] 처음에는 자리를 모두 쓴다고 본다 — 나중에 항목이 걷히면 이 값만 줄어든다. */
	rwlock_init(&new_invs->rwlock);	/* [한국어] 읽는 쪽(무효화 실행)과 쓰는 쪽(항목 걷기)이 함께 도는 자리라 락이 필요하다. */
	return new_invs;	/* [한국어] 호출자가 RCU 로 도메인에 걸어 둔다. */
}

/* [한국어] 이벤트 큐. 하드웨어가 오류를 여기에 쌓는다. */
struct arm_smmu_evtq {
	struct arm_smmu_queue		q;
	/* [한국어] 폴트를 상위 계층으로 보내는 작업 큐.
	 *  멈춰 세우기를 지원할 때만 쓴다 — 그때만 폴트에 응답할 수 있다. */
	struct iopf_queue		*iopf;
	/* [한국어] 동시에 멈춰 세울 수 있는 트랜잭션 수.
	 *  설정자: 능력 레지스터. 읽는 자: 폴트 큐의 깊이를 정할 때. */
	u32				max_stalls;
};

/* [한국어] 페이지 요청 큐. 장치가 페이지를 요청하면 여기 쌓인다.
 *
 * 멈춰 세우기와 달리 트랜잭션을 붙잡아 두지 않고, 장치가 나중에 다시
 * 시도한다. PCI 의 PRI 가 이 방식이다. */
struct arm_smmu_priq {
	/* [한국어] 페이지 요청 큐의 몸통 — 링 버퍼 주소와 생산·소비 포인터.
	 * 설정자: arm_smmu_init_queues() 가 arm_smmu_init_one_queue() 로 채운다.
	 * 읽는 자: PRI 인터럽트 처리 스레드가 소비 포인터를 밀며 요청을 꺼낸다.
	 * 값 범위: 항목 하나가 PRIQ_ENT_DWORDS(2) 워드다.
	 * 동기화: 생산자는 하드웨어, 소비자는 인터럽트 스레드 하나뿐이라
	 *         소비 쪽에는 락이 필요 없다. */
	struct arm_smmu_queue		q;
};

/* High-level stream table and context descriptor structures */
/* [한국어] 문맥 서술자의 커널 쪽 요약.
 *
 * 지금은 ASID 만 담는다 — 나머지는 페이지 테이블 설정에서 그때그때
 * 만들어 내기 때문이다. */
struct arm_smmu_ctx_desc {
	/* [한국어] 이 문맥의 ASID.
	 *  설정자: 도메인을 만들 때 전역 풀에서 배정받는다.
	 *  읽는 자: 서술자를 짓는 곳과 TLB 무효화. */
	u16				asid;
};

/* [한국어] 한 장치의 문맥 서술자 표.
 *
 * PASID 를 조금만 쓰면 1단계로, 많이 쓰면 2단계로 만든다. union 이
 * 그 두 모양을 겹쳐 담는다. */
struct arm_smmu_ctx_desc_cfg {
	/* [한국어] 1단계 평면 표와 2단계 표, 두 모양을 겹쳐 담는 자리.
	 * 설정자: arm_smmu_alloc_cd_tables() 가 s1cdmax 를 보고 한 쪽만 채운다.
	 * 읽는 자: 어느 쪽이 유효한지는 아래 s1fmt 가 알려 준다 —
	 *         CTXDESC_CD_0_S1FMT_LINEAR 이면 linear, 아니면 l2 다.
	 * 값 범위: 두 모양을 동시에 쓰는 일은 없다.
	 * 동기화: 표를 바꾸는 일은 장치를 붙이고 떼는 경로에서만 일어난다. */
	union {
		struct {
			/* [한국어] 평면 문맥 서술자 표의 커널 쪽 시작 주소.
			 * 설정자: dma_alloc_coherent() 로 잡아 채운다.
			 * 읽는 자: arm_smmu_get_cd_ptr() 이 &table[ssid] 로 항목을 짚는다.
			 * 값 범위: 잡히기 전에는 NULL — 그 NULL 여부가 곧 "표가 없다"의 뜻이다.
			 * 동기화: 붙이기 경로의 mutex 아래에서만 바뀐다. */
			struct arm_smmu_cd *table;
			/* [한국어] 그 표가 담는 서술자 항목의 수.
			 * 설정자: 1 << s1cdmax 를 CTXDESC_L2_ENTRIES 로 잘라 정한다.
			 * 읽는 자: PASID 가 표 밖을 가리키지 않는지 검사할 때.
			 * 값 범위: 1 ~ CTXDESC_L2_ENTRIES.
			 * 동기화: 표와 함께 한 번만 정해진다. */
			unsigned int num_ents;
		/* [한국어] PASID 를 조금만 쓸 때의 1단계(평면) 문맥 서술자 표.
		 * 설정자: arm_smmu_alloc_cd_tables() 가 s1cdmax 가 작을 때 이 쪽을 잡는다.
		 * 읽는 자: arm_smmu_get_cd_ptr() 이 PASID 를 첨자로 바로 인덱싱한다.
		 * 값 범위: 항목 수는 최대 CTXDESC_L2_ENTRIES(1024).
		 * 동기화: 표 자체는 붙이기·떼기 경로의 group mutex 아래에서만 바뀐다. */
		} linear;
		struct {
			/* [한국어] 2단계 구조에서 위쪽(1단계) 표의 커널 쪽 주소.
			 * 설정자: dma_alloc_coherent() 로 잡아 채운다 — 하드웨어도 이 표를 읽는다.
			 * 읽는 자: 아래쪽 표를 새로 달 때 그 주소를 여기에 써 넣는다.
			 * 값 범위: 잡히기 전에는 NULL.
			 * 동기화: 붙이기 경로의 mutex 아래에서만 바뀐다. */
			struct arm_smmu_cdtab_l1 *l1tab;
			/* [한국어] 아래쪽(2단계) 표들의 커널 주소를 모아 둔 배열.
			 * 설정자: 아래쪽 표를 게으르게 잡을 때마다 그 자리에 채운다.
			 * 읽는 자: arm_smmu_get_cd_ptr() 이 위쪽 첨자로 이 배열을 먼저 보고,
			 *         해제할 때도 이 배열을 훑어 잡힌 표를 되돌린다.
			 * 값 범위: 아직 안 쓴 자리는 NULL.
			 * 동기화: 하드웨어는 l1tab 의 값으로만 찾아가므로 이 배열은 커널 전용이다.
			 *         (위 영어 주석 참고) */
			struct arm_smmu_cdtab_l2 **l2ptrs;
			/* [한국어] 위쪽 표가 담는 항목의 수.
			 * 설정자: 지원할 PASID 수를 CTXDESC_L2_ENTRIES 로 나눠 올림해 정한다.
			 * 읽는 자: 위쪽 첨자 범위 검사와, 표 전체를 훑는 해제 경로.
			 * 값 범위: 1 이상.
			 * 동기화: 표와 함께 한 번만 정해진다. */
			unsigned int num_l1_ents;
		/* [한국어] PASID 를 많이 쓸 때의 2단계 문맥 서술자 표.
		 * 설정자: 위와 같은 자리에서 s1cdmax 가 클 때 이 쪽을 잡는다.
		 * 읽는 자: arm_smmu_get_cd_ptr() 이 위쪽 첨자로 아래쪽 표를 찾은 뒤
		 *         (없으면 그때 잡아서) 아래쪽 첨자로 항목에 닿는다.
		 * 값 범위: 아래쪽 표는 처음 쓰일 때 게으르게 잡혀 메모리를 아낀다.
		 * 동기화: 아래쪽 표를 새로 다는 일은 붙이기 경로에서만 일어난다. */
		} l2;
	};
	/* [한국어] 표의 장치 쪽 주소. 스트림 표 항목에 담는다. */
	dma_addr_t			cdtab_dma;
	/* [한국어] 0번을 뺀, 실제로 쓰이는 PASID 수.
	 *  0 이면 그 장치가 PASID 를 쓰지 않는다는 뜻이라 몇 가지 최적화가 열린다. */
	unsigned int			used_ssids;
	/* [한국어] 이 표가 스트림 표 항목에 실려 있는가.
	 *  표를 바꾸려면 먼저 항목에서 떼어야 해서 그 상태를 기억한다. */
	u8				in_ste;
	/* [한국어] 1단계인지 2단계인지. 스트림 표 항목에 담는 값이다. */
	u8				s1fmt;
	/* log2 of the maximum number of CDs supported by this table */
	/* [한국어] (위 영어 주석 참고) 이 표가 담을 수 있는 서술자 수의 로그. */
	u8				s1cdmax;
};

/*
 * [한국어]
 * arm_smmu_cdtab_allocated - 문맥 표가 잡혀 있는가
 *
 * @cfg: 그 표 설정.
 * @return: 잡혀 있으면 참.
 *
 * union 이라 어느 쪽이든 0 이 아니면 잡힌 것이다.
 */
static inline bool
arm_smmu_cdtab_allocated(struct arm_smmu_ctx_desc_cfg *cfg)
{
	return cfg->linear.table || cfg->l2.l1tab;	/* [한국어] 둘 중 하나라도 잡혀 있으면 표가 준비된 것이다 — union 이라 어느 쪽인지는 안 따진다. */
}

/* True if the cd table has SSIDS > 0 in use. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * arm_smmu_ssids_in_use - 0번 말고 쓰이는 PASID 가 있는가
 *
 * @cd_table: 그 표 설정.
 * @return: 있으면 참.
 *
 * 없으면 스트림 표 항목을 더 자유롭게 바꿀 수 있다 — PASID 를 쓰는
 * 장치는 그 사이 요청이 끼어들 수 있어 조심해야 한다.
 */
static inline bool arm_smmu_ssids_in_use(struct arm_smmu_ctx_desc_cfg *cd_table)
{
	return cd_table->used_ssids;	/* [한국어] PASID 를 하나라도 쓰고 있는지로 표를 접을 수 있는지 판단한다. */
}

/* [한국어] 2단계 도메인의 설정. 지금은 VMID 만 담는다. */
struct arm_smmu_s2_cfg {
	/* [한국어] 이 2단계 도메인의 VMID.
	 *  설정자: 도메인을 만들 때 ida 에서 배정받는다.
	 *  읽는 자: 스트림 표 항목과 TLB 무효화. */
	u16				vmid;
};

/* [한국어] 스트림 표 전체의 설정.
 *
 * 문맥 표와 마찬가지로 1단계·2단계 두 모양을 union 으로 겹쳐 둔다.
 * 스트림 id 공간이 넓으면 2단계라야 메모리를 아낄 수 있다. */
struct arm_smmu_strtab_cfg {
	/* [한국어] 평면 스트림 표와 2단계 스트림 표, 두 모양을 겹쳐 담는 자리.
	 * 설정자: arm_smmu_init_strtab() 이 sid_bits 와 STRTAB_MAX_L1_ENTRIES 를 견줘
	 *         한 쪽만 잡는다.
	 * 읽는 자: 어느 쪽인지는 smmu->features 의 ARM_SMMU_FEAT_2_LVL_STRTAB 이 가른다.
	 * 값 범위: 두 모양을 동시에 쓰는 일은 없다.
	 * 동기화: 프로브 때 한 번 정해지고, 이후 표의 뼈대는 바뀌지 않는다. */
	union {
		/* [한국어] 스트림 id 공간이 좁을 때의 평면 스트림 표.
		 * 설정자: 항목 수가 STRTAB_MAX_L1_ENTRIES 를 넘지 않으면 이 쪽을 잡는다.
		 * 읽는 자: arm_smmu_get_step_for_sid() 이 sid 로 바로 인덱싱한다.
		 * 값 범위: 항목 하나가 64바이트 STE 다.
		 * 동기화: 항목 내용은 STE 쓰기 규약(entry_writer)이 따로 지킨다. */
		struct {
			/* [한국어] 평면 스트림 표의 커널 쪽 시작 주소.
			 * 설정자: dmam_alloc_coherent() 로 잡아 채운다.
			 * 읽는 자: sid 로 STE 를 짚는 모든 곳.
			 * 값 범위: NULL 이 아니어야 한다 — 잡기에 실패하면 프로브가 접힌다.
			 * 동기화: 프로브 이후 불변. */
			struct arm_smmu_ste *table;
			/* [한국어] 그 표의 장치 쪽(물리) 주소.
			 * 설정자: 위와 같은 자리에서 dma 핸들을 받아 채운다.
			 * 읽는 자: STRTAB_BASE 레지스터에 실어 하드웨어에게 알린다.
			 * 값 범위: 표 크기에 맞춰 정렬되어 있어야 한다.
			 * 동기화: 프로브 이후 불변. */
			dma_addr_t ste_dma;
			/* [한국어] 표가 담는 STE 의 수.
			 * 설정자: 1 << sid_bits.
			 * 읽는 자: 스트림 id 가 표 밖을 가리키는지 검사할 때.
			 * 값 범위: STRTAB_MAX_L1_ENTRIES 이하.
			 * 동기화: 프로브 이후 불변. */
			unsigned int num_ents;
		} linear;
		/* [한국어] 스트림 id 공간이 넓을 때의 2단계 스트림 표.
		 * 설정자: 평면으로 잡기엔 표가 너무 클 때 이 쪽을 잡는다.
		 * 읽는 자: 위쪽 첨자로 아래쪽 표를 찾고, 없으면 그때 잡아 단다.
		 * 값 범위: 아래쪽 표는 실제로 쓰이는 스트림 구간에만 생긴다.
		 * 동기화: 아래쪽 표를 새로 다는 일은 장치 프로브 경로에서만 일어난다. */
		struct {
			/* [한국어] 위쪽(1단계) 스트림 표의 커널 쪽 주소.
			 * 설정자: dmam_alloc_coherent() 로 잡아 채운다.
			 * 읽는 자: 아래쪽 표를 새로 달 때 그 주소를 여기에 써 넣는다.
			 * 값 범위: NULL 이 아니어야 한다.
			 * 동기화: 프로브 이후 뼈대는 불변, 항목 값만 바뀐다. */
			struct arm_smmu_strtab_l1 *l1tab;
			/* [한국어] 아래쪽(2단계) 표들의 커널 주소를 모아 둔 배열.
			 * 설정자: 아래쪽 표를 게으르게 잡을 때마다 채운다.
			 * 읽는 자: sid 로 STE 를 짚는 경로가 이 배열을 먼저 본다.
			 * 값 범위: 아직 안 쓴 자리는 NULL.
			 * 동기화: 하드웨어는 l1tab 만 읽으므로 이 배열은 커널 전용이다. */
			struct arm_smmu_strtab_l2 **l2ptrs;
			/* [한국어] 위쪽 표의 장치 쪽(물리) 주소.
			 * 설정자: 위쪽 표를 잡을 때 받은 dma 핸들.
			 * 읽는 자: STRTAB_BASE 레지스터에 실어 하드웨어에게 알린다.
			 * 값 범위: 표 크기에 맞춰 정렬되어 있어야 한다.
			 * 동기화: 프로브 이후 불변. */
			dma_addr_t l1_dma;
			/* [한국어] 위쪽 표가 담는 항목의 수.
			 * 설정자: 1 << sid_bits 를 STRTAB_NUM_L2_STES 로 나눠 올림해 정한다.
			 * 읽는 자: 위쪽 첨자 범위 검사와 표 전체를 훑는 경로.
			 * 값 범위: STRTAB_MAX_L1_ENTRIES 이하.
			 * 동기화: 프로브 이후 불변. */
			unsigned int num_l1_ents;
		} l2;	/* [한국어] 여기까지가 2단계 스트림 표 모양이다. */
	};
};

/* [한국어] 구현체별 갈고리표.
 *
 * SMMUv3 는 v1/v2 보다 규격이 촘촘해 벗어난 구현이 적다. 그래도 Tegra
 * 처럼 보조 명령 큐를 더한 하드웨어가 있어 이 표를 둔다. */
struct arm_smmu_impl_ops {
	/* [한국어] 초기화 뒤 구현체가 손볼 것이 있으면. */
	int (*device_reset)(struct arm_smmu_device *smmu);
	/* [한국어] 걷어 낼 때의 정리. */
	void (*device_remove)(struct arm_smmu_device *smmu);
	/* [한국어] 자료 구조를 잡은 뒤 구현체가 자기 것을 더할 자리. */
	int (*init_structures)(struct arm_smmu_device *smmu);
	/* [한국어] 이 명령을 어느 큐로 보낼지 고른다.
	 *  Tegra 는 CPU 마다 다른 큐를 주어 다툼을 줄인다. */
	struct arm_smmu_cmdq *(*get_secondary_cmdq)(
		struct arm_smmu_device *smmu, struct arm_smmu_cmdq_ent *ent);
	/*
	 * An implementation should define its own type other than the default
	 * IOMMU_HW_INFO_TYPE_ARM_SMMUV3. And it must validate the input @type
	 * to return its own structure.
	 */
	/* [한국어] (위 영어 주석 참고) 사용자에게 알릴 하드웨어 정보를 만든다.
	 *  구현체는 자기만의 형식 번호를 정하고 그 입력을 검증해야 한다. */
	void *(*hw_info)(struct arm_smmu_device *smmu, u32 *length,
			 enum iommu_hw_info_type *type);
	/* [한국어] 가상 SMMU 구조체의 크기를 알려 준다.
	 *  코어가 자기 것과 한 덩어리로 잡으려고 먼저 묻는다. */
	size_t (*get_viommu_size)(enum iommu_viommu_type viommu_type);
	/* [한국어] 그 가상 SMMU 를 세운다. */
	int (*vsmmu_init)(struct arm_vsmmu *vsmmu,
			  const struct iommu_user_data *user_data);
};

/* An SMMUv3 instance */
/* [한국어]
 * (위 영어 주석에 이어) SMMUv3 하드웨어 하나.
 *
 * 능력 레지스터에서 읽어 낸 성질(features), 우회가 필요한 부분(options),
 * 세 개의 큐, 그리고 스트림 표를 든다.
 *
 * v1/v2 와 가장 다른 점은 컨텍스트 뱅크가 없다는 것이다. 뱅크 수가
 * 동시 도메인 수를 제한하던 자리를, 여기서는 메모리에 놓인 스트림 표와
 * 문맥 표가 대신한다 — 그래서 도메인 수에 사실상 제한이 없다. */
struct arm_smmu_device {
	/* [한국어] 이 SMMU 의 플랫폼 장치. */
	/* [한국어] 스트림 id 로 되찾은 장치. 로그에 이름을 남기는 데 쓴다. */
	struct device			*dev;
	/* [한국어] 구현체가 별도의 장치를 갖는 경우 그 장치.
	 *  Tegra 의 보조 큐 블록이 그렇다. */
	struct device			*impl_dev;
	/* [한국어] 구현체 갈고리표(없을 수 있다). */
	const struct arm_smmu_impl_ops	*impl_ops;

	/* [한국어] 레지스터 창의 가상 주소. */
	void __iomem			*base;
	/* [한국어] 두 번째 레지스터 페이지.
	 *  큐 포인터가 그쪽에 있어 자주 쓰인다. 없는 하드웨어에서는 첫 페이지를 가리킨다. */
	void __iomem			*page1;

#define ARM_SMMU_FEAT_2_LVL_STRTAB	(1 << 0)	/* [한국어] 스트림 표를 2단계로 만든다. */
#define ARM_SMMU_FEAT_2_LVL_CDTAB	(1 << 1)	/* [한국어] 문맥 표를 2단계로 만든다. */
#define ARM_SMMU_FEAT_TT_LE		(1 << 2)	/* [한국어] 표를 리틀 엔디언으로 읽는다. */
#define ARM_SMMU_FEAT_TT_BE		(1 << 3)	/* [한국어] 표를 빅 엔디언으로 읽는다. */
#define ARM_SMMU_FEAT_PRI		(1 << 4)	/* [한국어] PCI 페이지 요청 인터페이스를 쓸 수 있다. */
#define ARM_SMMU_FEAT_ATS		(1 << 5)	/* [한국어] 장치 쪽 변환 캐시를 쓸 수 있다. */
#define ARM_SMMU_FEAT_SEV		(1 << 6)	/* [한국어] 이벤트로 깨울 수 있다 — 폴링 대신 WFE 로 기다린다. */
#define ARM_SMMU_FEAT_MSI		(1 << 7)	/* [한국어] 인터럽트를 MSI 로 낼 수 있다. */
#define ARM_SMMU_FEAT_COHERENCY		(1 << 8)	/* [한국어] 표 순회가 캐시 일관성을 갖는다. */
#define ARM_SMMU_FEAT_TRANS_S1		(1 << 9)	/* [한국어] 1단계 변환을 할 수 있다. */
#define ARM_SMMU_FEAT_TRANS_S2		(1 << 10)	/* [한국어] 2단계 변환을 할 수 있다. */
#define ARM_SMMU_FEAT_STALLS		(1 << 11)	/* [한국어] 폴트 때 멈춰 세울 수 있다. */
#define ARM_SMMU_FEAT_HYP		(1 << 12)	/* [한국어] 하이퍼바이저 관련 기능이 있다. */
#define ARM_SMMU_FEAT_STALL_FORCE	(1 << 13)	/* [한국어] 늘 멈춰 세운다 — 끌 수 없다. */
#define ARM_SMMU_FEAT_VAX		(1 << 14)	/* [한국어] 52비트 입력 주소를 쓸 수 있다. */
#define ARM_SMMU_FEAT_RANGE_INV		(1 << 15)	/* [한국어] 범위 기반 무효화를 쓸 수 있다. 한 명령으로 여러 페이지를 비운다. */
#define ARM_SMMU_FEAT_BTM		(1 << 16)	/* [한국어] 브로드캐스트 TLB 유지. CPU 의 무효화가 SMMU 에도 미친다. */
#define ARM_SMMU_FEAT_SVA		(1 << 17)	/* [한국어] 공유 가상 주소를 쓸 수 있다 — 장치가 프로세스의 주소 공간을 그대로 본다. */
#define ARM_SMMU_FEAT_E2H		(1 << 18)	/* [한국어] 호스트 확장 모드. 커널이 EL2 에서 돌 때. */
#define ARM_SMMU_FEAT_NESTING		(1 << 19)	/* [한국어] 중첩 변환을 할 수 있다. */
#define ARM_SMMU_FEAT_ATTR_TYPES_OVR	(1 << 20)	/* [한국어] 메모리 속성을 덮어쓸 수 있다. */
#define ARM_SMMU_FEAT_HA		(1 << 21)	/* [한국어] 하드웨어가 접근 비트를 갱신한다. */
#define ARM_SMMU_FEAT_HD		(1 << 22)	/* [한국어] 하드웨어가 더티 비트를 갱신한다. 마이그레이션의 바탕이다. */
#define ARM_SMMU_FEAT_S2FWB		(1 << 23)	/* [한국어] 2단계가 1단계 메모리 속성을 덮어쓴다. */
#define ARM_SMMU_FEAT_BBML2		(1 << 24)	/* [한국어] 큰 페이지를 쪼갤 때 중간에 무효 상태를 거치지 않아도 된다. */
	/* [한국어] 위 ARM_SMMU_FEAT_* 비트의 모음.
	 *  설정자: 능력 레지스터를 읽어 채운다.
	 *  읽는 자: 무엇을 할 수 있는지 판단하는 모든 곳. */
	u32				features;

#define ARM_SMMU_OPT_SKIP_PREFETCH	(1 << 0)	/* [한국어] 설정 미리 읽기를 건너뛴다. 그 명령이 온전하지 않은 하드웨어가 있다. */
#define ARM_SMMU_OPT_PAGE0_REGS_ONLY	(1 << 1)	/* [한국어] 두 번째 레지스터 페이지가 없어 모든 접근을 첫 페이지로 보낸다. */
#define ARM_SMMU_OPT_MSIPOLL		(1 << 2)	/* [한국어] 완료 대기를 MSI 값 폴링으로 한다. 소비 포인터를 읽는 것보다 빠르다. */
#define ARM_SMMU_OPT_CMDQ_FORCE_SYNC	(1 << 3)	/* [한국어] 명령마다 완료 대기를 넣는다. 큐 처리가 온전하지 않은 하드웨어 우회다. */
#define ARM_SMMU_OPT_TEGRA241_CMDQV	(1 << 4)	/* [한국어] Tegra241 의 보조 명령 큐를 쓴다. */
	/* [한국어] 위 ARM_SMMU_OPT_* 비트의 모음.
	 *  설정자: 장치 트리와 구현 식별 레지스터를 보고 정한다.
	 *  하드웨어의 결함이나 통합상의 차이를 나타낸다. */
	u32				options;

	/* [한국어] 명령 큐. 커널이 SMMU 에 명령을 보낸다. */
	struct arm_smmu_cmdq		cmdq;
	/* [한국어] 이벤트 큐. SMMU 가 오류를 쌓는다. */
	struct arm_smmu_evtq		evtq;
	/* [한국어] 페이지 요청 큐. 장치가 페이지를 요청한다. */
	struct arm_smmu_priq		priq;

	/* [한국어] 전역 오류 인터럽트 번호. */
	int				gerr_irq;
	/* [한국어] 세 인터럽트를 한 선으로 묶은 경우의 번호.
	 *  있으면 그것 하나만 등록하고 개별 인터럽트는 쓰지 않는다. */
	int				combined_irq;

	/* [한국어] (위 영어 주석 참고) 출력(물리) 주소의 폭. */
	unsigned long			oas; /* PA */
	/* [한국어] 쓸 수 있는 페이지 크기의 비트맵. */
	unsigned long			pgsize_bitmap;

#define ARM_SMMU_MAX_ASIDS		(1 << 16)	/* [한국어] ASID 의 최대 개수. 16비트라 65536개다. */
	/* [한국어] ASID 의 비트 수. 8 또는 16 이다. */
	unsigned int			asid_bits;

#define ARM_SMMU_MAX_VMIDS		(1 << 16)	/* [한국어] VMID 의 최대 개수. */
	/* [한국어] VMID 의 비트 수. */
	unsigned int			vmid_bits;
	/* [한국어] VMID 를 배정하는 id 할당기.
	 *  ASID 는 여러 SMMU 가 공유해 전역 풀을 쓰지만, VMID 는 SMMU 마다 따로다. */
	struct ida			vmid_map;

	/* [한국어] PASID 의 비트 수. 문맥 표의 크기를 정한다. */
	/* [한국어] 이 장치가 쓸 수 있는 PASID 의 비트 수.
	 *  장치와 SMMU 중 작은 쪽이다. */
	unsigned int			ssid_bits;
	/* [한국어] 스트림 id 의 비트 수. 스트림 표의 크기를 정한다. */
	unsigned int			sid_bits;

	/* [한국어] 스트림 표. 이 SMMU 아래 모든 장치가 여기 담긴다. */
	struct arm_smmu_strtab_cfg	strtab_cfg;

	/* IOMMU core code handle */
	/* [한국어] (위 영어 주석 참고) iommu 코어에 등록하는 손잡이. */
	struct iommu_device		iommu;

	/* [한국어] 스트림 id 로 장치를 찾는 트리.
	 *  오류가 났을 때 어느 장치인지 알아내는 데 쓴다. */
	struct rb_root			streams;
	/* [한국어] 그 트리를 지키는 뮤텍스. */
	struct mutex			streams_mutex;
};

/* [한국어] 스트림 id 하나와 그것을 쓰는 장치의 짝.
 *
 * 오류가 났을 때 스트림 id 밖에 알 수 없어, 그것으로 장치를 되찾는
 * 트리를 만든다. */
struct arm_smmu_stream {
	/* [한국어] 그 스트림 id. */
	u32				id;
	/* [한국어] 그 id 를 쓰는 장치. */
	struct arm_smmu_master		*master;
	/* [한국어] SMMU 의 streams 트리에 매다는 마디. */
	struct rb_node			node;
};

/* [한국어] 가상 SMMU 에서 본 이 장치.
 *
 * 게스트가 붙인 번호(vsid)를 들고 있어, 오류를 게스트에게 전할 때
 * 그 번호로 알린다. */
struct arm_smmu_vmaster {
	/* [한국어] 속한 가상 SMMU. */
	struct arm_vsmmu		*vsmmu;
	/* [한국어] 게스트가 아는 스트림 id.
	 *  호스트 id 를 그대로 전하면 게스트가 알아보지 못한다. */
	unsigned long			vsid;
};

/* [한국어] 이벤트 큐에서 읽은 오류 하나를 풀어 담은 것.
 *
 * 하드웨어 형식은 비트 묶음이라 그대로 다루기 번거로워, 뜻이 드러나는
 * 필드로 옮겨 담는다. */
struct arm_smmu_event {
	/* [한국어] 그 트랜잭션이 멈춰 선 채(stalled) 응답을 기다리는가.
	 * 설정자: arm_smmu_decode_event() 가 이벤트 기록의 STALL 비트를 풀어 담는다.
	 * 읽는 자: 폴트 처리기가 참이면 CMDQ_OP_RESUME 으로 답을 돌려줘야 한다 —
	 *         답하지 않으면 그 트랜잭션이 영원히 멈춰 있다.
	 * 값 범위: 0/1. 멈춤을 지원하는 스트림에서만 1 이 될 수 있다.
	 * 동기화: 이벤트 큐를 소비하는 스레드 하나만 이 구조를 다룬다. */
	u8				stall : 1,
	/* [한국어] 아래 ssid 필드가 유효한가(SubStream Valid).
	 * 설정자: 같은 해독 자리에서 SSV 비트를 옮겨 담는다.
	 * 읽는 자: 폴트를 어느 PASID 에 돌릴지 정할 때 먼저 이 값을 본다.
	 * 값 범위: 0 이면 ssid 는 쓰레기다.
	 * 동기화: 위와 같다. */
					ssv : 1,
	/* [한국어] 특권(커널/EL1) 접근에서 난 오류인가.
	 * 설정자: 이벤트 기록의 PnU 비트.
	 * 읽는 자: iommu 폴트 보고에 IOMMU_FAULT_PERM_PRIV 로 옮겨 담는다.
	 * 값 범위: 0/1.
	 * 동기화: 위와 같다. */
					privileged : 1,
	/* [한국어] 명령 인출(instruction fetch)에서 난 오류인가.
	 * 설정자: 이벤트 기록의 InD 비트.
	 * 읽는 자: 폴트 보고의 실행 권한 표시로 옮겨 담는다.
	 * 값 범위: 0/1.
	 * 동기화: 위와 같다. */
					instruction : 1,
	/* [한국어] 2단계(게스트 물리 → 실제 물리) 변환에서 난 오류인가.
	 * 설정자: 이벤트 기록의 S2 비트.
	 * 읽는 자: 오류를 게스트에게 돌릴지 호스트가 처리할지 가르는 기준.
	 * 값 범위: 0 이면 1단계 오류다.
	 * 동기화: 위와 같다. */
					s2 : 1,
	/* [한국어] 읽기 접근이었는가 — 거짓이면 쓰기다.
	 * 설정자: 이벤트 기록의 RnW 비트.
	 * 읽는 자: 폴트 보고의 READ/WRITE 권한 표시.
	 * 값 범위: 0/1.
	 * 동기화: 위와 같다. */
					read : 1,
	/* [한국어] 표를 순회하던 접근이 읽기였는가(TT Read-not-Write).
	 * 설정자: 이벤트 기록의 TTRnW 비트.
	 * 읽는 자: 표 순회 중에 난 오류를 사람이 읽을 문장으로 찍을 때.
	 * 값 범위: class_tt 가 참일 때만 뜻이 있다.
	 * 동기화: 위와 같다. */
					ttrnw : 1,
	/* [한국어] 아래 class 필드가 "표 순회(table walk)"를 가리키는가.
	 * 설정자: class 값이 TT 계열인지 보고 정한다.
	 * 읽는 자: 참이면 fetch_addr 이 뜻을 가진다 — 어느 표를 읽다 넘어졌는지.
	 * 값 범위: 0/1.
	 * 동기화: 위와 같다. */
					class_tt : 1;
	/* [한국어] 이벤트의 종류 번호.
	 * 설정자: 이벤트 기록 첫 워드의 ID 필드.
	 * 읽는 자: 이 값으로 사람이 읽을 설명 문장을 고르고, 처리 갈래를 나눈다.
	 * 값 범위: EVT_ID_BAD_STREAMID_CONFIG 부터 EVT_ID_VMS_FETCH_FAULT 까지.
	 * 동기화: 위와 같다. */
	u8				id;
	/* [한국어] 오류가 난 접근의 갈래(CLASS) — 표 순회인지, 문맥 서술자 인출인지 등.
	 * 설정자: 이벤트 기록의 CLASS 필드.
	 * 읽는 자: class_tt 를 정하는 근거이자, 로그 문장의 재료.
	 * 값 범위: 하드웨어가 정의한 코드.
	 * 동기화: 위와 같다. */
	u8				class;
	/* [한국어] 멈춘 트랜잭션에 붙은 꼬리표(stall tag).
	 * 설정자: 이벤트 기록의 STAG 필드.
	 * 읽는 자: CMDQ_OP_RESUME 을 지을 때 그대로 실어 보낸다 — 이 값이 짝을 맞춘다.
	 * 값 범위: stall 이 참일 때만 뜻이 있다.
	 * 동기화: 위와 같다. */
	u16				stag;
	/* [한국어] 오류를 낸 장치의 스트림 번호.
	 * 설정자: 이벤트 기록의 SID 필드.
	 * 읽는 자: 이 번호로 arm_smmu_master 를 되찾아 어느 장치인지 알아낸다.
	 * 값 범위: 표 밖을 가리키는 값이 올 수도 있다 — 그 자체가 오류 종류 중 하나다.
	 * 동기화: 위와 같다. */
	u32				sid;
	/* [한국어] 오류를 낸 문맥의 PASID.
	 * 설정자: 이벤트 기록의 SSID 필드.
	 * 읽는 자: SVA 폴트를 어느 주소 공간에 돌릴지 정할 때.
	 * 값 범위: ssv 가 참일 때만 뜻이 있다.
	 * 동기화: 위와 같다. */
	u32				ssid;
	/* [한국어] 오류가 난 입력 주소(장치가 내민 주소).
	 * 설정자: 이벤트 기록의 ADDR 필드.
	 * 읽는 자: 폴트 처리기가 이 주소의 페이지를 채워 넣는다.
	 * 값 범위: 페이지 경계로 내림해 쓰는 곳이 많다.
	 * 동기화: 위와 같다. */
	u64				iova;
	/* [한국어] 2단계 오류에서의 중간 물리 주소.
	 * 설정자: 이벤트 기록의 IPA 필드.
	 * 읽는 자: 게스트에게 오류를 돌릴 때 게스트가 아는 주소로 쓰인다.
	 * 값 범위: s2 가 참일 때만 뜻이 있다.
	 * 동기화: 위와 같다. */
	u64				ipa;
	/* [한국어] 표를 읽다 넘어졌을 때, 읽으려던 표 항목의 주소.
	 * 설정자: 이벤트 기록의 FETCH_ADDR 필드.
	 * 읽는 자: 표 자체가 망가졌는지 진단할 때 — 어느 항목이 문제인지 알려 준다.
	 * 값 범위: class_tt 가 참일 때만 뜻이 있다.
	 * 동기화: 위와 같다. */
	u64				fetch_addr;
	/* [한국어] 오류를 낸 장치의 커널 쪽 device 포인터.
	 * 설정자: sid 로 스트림 트리를 뒤져 찾은 master 의 dev 를 담는다.
	 * 읽는 자: 오류 로그를 dev_err() 로 찍을 때와, 폴트를 그 장치의
	 *         iopf 큐로 넘길 때.
	 * 값 범위: 스트림 번호가 어느 장치에도 닿지 않으면 NULL 이다 —
	 *         그때는 장치 이름 없이 SMMU 쪽 로그로만 남긴다.
	 * 동기화: 이벤트를 다루는 동안 그 장치가 떨어져 나가지 않도록,
	 *         찾는 일은 스트림 트리의 rwlock 아래에서 한다. */
	struct device			*dev;
};

/* SMMU private data for each master */
/* [한국어]
 * (위 영어 주석에 이어) 장치 하나에 대한 이 드라이버의 상태.
 *
 * 그 장치가 쓰는 스트림 id 들, 문맥 서술자 표, 그리고 ATS 나 폴트
 * 처리를 쓸 수 있는지가 담긴다. */
struct arm_smmu_master {
	/* [한국어] 이 장치가 매인 SMMU. */
	struct arm_smmu_device		*smmu;
	/* [한국어] 그 장치. */
	struct device			*dev;
	/* [한국어] 이 장치가 쓰는 스트림 id 들.
	 *  가변 개수라 따로 잡는다. */
	struct arm_smmu_stream		*streams;
	/*
	 * Scratch memory for a to_merge or to_unref array to build a per-domain
	 * invalidation array. It'll be pre-allocated with enough enries for all
	 * possible build scenarios. It can be used by only one caller at a time
	 * until the arm_smmu_invs_merge/unref() finishes. Must be locked by the
	 * iommu_group mutex.
	 */
	/* [한국어] (위 영어 주석 참고) 무효화 배열을 지을 때 쓰는 작업 공간.
	 *  미리 잡아 두어 붙이기 경로에서 할당이 실패하지 않게 한다.
	 *  동기화: iommu 그룹 뮤텍스가 지킨다. */
	struct arm_smmu_invs		*build_invs;
	/* [한국어] (위 영어 주석 참고) 가상 SMMU 에서 본 모습(없을 수 있다). */
	struct arm_smmu_vmaster		*vmaster; /* use smmu->streams_mutex */
	/* Locked by the iommu core using the group mutex */
	/* [한국어] (위 영어 주석 참고) 이 장치의 문맥 서술자 표.
	 *  동기화: iommu 코어가 그룹 뮤텍스로 지킨다. */
	struct arm_smmu_ctx_desc_cfg	cd_table;
	/* [한국어] 스트림 id 의 개수. */
	unsigned int			num_streams;
	/* [한국어] 이 장치의 ATS 를 켜기로 했는가. */
	bool				ats_enabled : 1;
	/* [한국어] 스트림 표 항목에 실제로 그렇게 적혀 있는가.
	 *  둘을 나눈 이유: 켜기로 정하는 것과 하드웨어에 반영하는 것 사이에 창이 있다. */
	bool				ste_ats_enabled : 1;
	/* [한국어] 폴트 때 멈춰 세울 것인가.
	 *  그 장치가 폴트를 다룰 수 있어야 켤 수 있다. */
	bool				stall_enabled;
	unsigned int			ssid_bits;
	/* [한국어] 폴트 보고를 켠 도메인 수.
	 *  여러 PASID 가 함께 쓸 수 있어 참조로 센다. */
	unsigned int			iopf_refcount;
};

/* SMMU private data for an IOMMU domain */
/* [한국어]
 * (위 영어 주석에 이어) 도메인이 어느 변환 단계를 쓰는가. */
enum arm_smmu_domain_stage {
	/* [한국어] 1단계. 호스트가 장치의 주소 공간을 관리한다.
	 *  0 인 것은 지정하지 않았을 때의 기본값이 되게 하려는 것이다. */
	ARM_SMMU_DOMAIN_S1 = 0,
	/* [한국어] 2단계. 게스트에게 장치를 넘길 때의 바깥쪽이다. */
	ARM_SMMU_DOMAIN_S2,
	/* [한국어] 공유 가상 주소. 장치가 프로세스의 주소 공간을 그대로 본다.
	 *  표를 새로 만들지 않고 CPU 의 것을 그대로 가리킨다. */
	ARM_SMMU_DOMAIN_SVA,
};

/* [한국어] 한 주소 공간.
 *
 * 1단계면 문맥 서술자를, 2단계면 VMID 를 든다 — 둘은 union 이다.
 *
 * invs 배열이 이 구조체의 요점이다. 무효화가 필요할 때 어디에 무엇을
 * 보낼지 미리 적어 두어, 빠른 경로에서 락 없이 훑기만 하면 된다. */
struct arm_smmu_domain {
	/* [한국어] 이 도메인이 매인 SMMU.
	 *  NULL 이면 아직 어느 장치도 붙지 않았다. */
	struct arm_smmu_device		*smmu;

	/* [한국어] 페이지 테이블 조작 함수들.
	 *  SVA 도메인은 표를 만들지 않아 NULL 이다. */
	struct io_pgtable_ops		*pgtbl_ops;
	/* [한국어] 이 도메인에 붙은 ATS 장치 수.
	 *  0 이면 장치 캐시 무효화를 아예 건너뛴다 — 그 경로가 비싸다. */
	atomic_t			nr_ats_masters;

	/* [한국어] 쓰는 변환 단계. */
	enum arm_smmu_domain_stage	stage;
	union {
	/* [한국어] 1단계일 때의 문맥 서술자 요약. */
		struct arm_smmu_ctx_desc	cd;
	/* [한국어] 2단계일 때의 VMID. */
		struct arm_smmu_s2_cfg		s2_cfg;
	};

	/* [한국어] 코어가 보는 도메인. */
	struct iommu_domain		domain;

	/* [한국어] 무효화 항목 배열.
	 *  설정자: 장치를 붙이고 뗄 때 새 배열로 갈아 끼운다.
	 *  읽는 자: 무효화 경로가 RCU 로 훑는다 — 락이 없어 빠르다. */
	struct arm_smmu_invs __rcu	*invs;

	/* List of struct arm_smmu_master_domain */
	/* [한국어] (위 영어 주석 참고) 이 도메인에 붙은 장치들. */
	struct list_head		devices;
	/* [한국어] 그 목록을 지키는 스핀락. */
	spinlock_t			devices_lock;
	/* [한국어] 캐시 일관성을 강제하는가.
	 *  한 번 켜지면 이 도메인에는 그러지 못하는 장치를 붙일 수 없다. */
	bool				enforce_cache_coherency : 1;
	/* [한국어] 중첩 변환의 바깥쪽으로 쓰이는가.
	 *  그러면 게스트 도메인들이 이것을 가리킨다. */
	bool				nest_parent : 1;

	/* [한국어] SVA 도메인이 프로세스의 주소 공간 변화를 받는 통로.
	 *  CPU 쪽에서 매핑이 바뀌면 이것을 통해 SMMU 도 비운다. */
	struct mmu_notifier		mmu_notifier;
};

/* [한국어] 게스트가 만든 중첩 도메인.
 *
 * 게스트가 준 스트림 표 항목의 일부를 그대로 들고 있다가, 붙일 때
 * 커널이 쥔 부분과 합쳐 실제 항목을 만든다. */
struct arm_smmu_nested_domain {
	/* [한국어] 코어가 보는 도메인. */
	struct iommu_domain domain;
	/* [한국어] 이 도메인을 거느린 가상 SMMU. */
	struct arm_vsmmu *vsmmu;
	/* [한국어] 게스트가 ATS 를 요청했는가. */
	bool enable_ats : 1;

	/* [한국어] 게스트가 준 스트림 표 항목의 낱말 0 과 1.
	 *  NESTING_ALLOWED 마스크가 걸러 낸 부분만 담긴다 — 그 밖의 비트는 커널이 정한다. */
	__le64 ste[2];
};

/* The following are exposed for testing purposes. */
	/* [한국어] (위 영어 주석 참고) 아래 구조체가 포인터로 참조해 미리 선언한다. */
struct arm_smmu_entry_writer_ops;
/* [한국어] 표 항목을 안전하게 고치는 일꾼.
 *
 * 스트림 표 항목이나 문맥 서술자는 여러 낱말로 이루어져 있는데,
 * 하드웨어가 그 중간 상태를 볼 수 있다. 그래서 "지금 쓰이는 비트"를
 * 알아내 순서를 짜야 한다. 그 알고리즘이 arm_smmu_write_entry 이고,
 * 이 구조체가 항목 종류별 차이를 흡수한다. */
struct arm_smmu_entry_writer {
	/* [한국어] 항목 종류별 콜백표. */
	const struct arm_smmu_entry_writer_ops *ops;
	/* [한국어] 그 항목이 속한 장치. 동기화 명령을 보낼 때 필요하다. */
	struct arm_smmu_master *master;
};

/* [한국어] 항목 종류별 차이를 흡수하는 콜백표. */
struct arm_smmu_entry_writer_ops {
	/* [한국어] 이 항목에서 지금 하드웨어가 실제로 쓰는 비트를 알려 준다.
	 *  그 비트를 건드리지 않는 쓰기는 언제든 안전하다는 것이 알고리즘의 바탕이다. */
	void (*get_used)(const __le64 *entry, __le64 *used);
	/* [한국어] 한 번에 바꾸어도 되는 비트를 알려 준다.
	 *  두 상태 사이에 하드웨어가 볼 수 있는 중간 상태가 모두 유효할 때만 그렇다. */
	void (*get_update_safe)(const __le64 *cur, const __le64 *target,
				__le64 *safe_bits);
	/* [한국어] 쓴 내용을 하드웨어가 보게 만든다.
	 *  설정 캐시 무효화 명령을 보내고 완료를 기다린다. */
	void (*sync)(struct arm_smmu_entry_writer *writer);
};

	/* [한국어] 모든 접근을 중단시키는 항목을 짓는다. */
void arm_smmu_make_abort_ste(struct arm_smmu_ste *target);
	/* [한국어] 2단계 도메인의 항목을 짓는다. */
void arm_smmu_make_s2_domain_ste(struct arm_smmu_ste *target,
				 struct arm_smmu_master *master,
				 struct arm_smmu_domain *smmu_domain,
				 bool ats_enabled);

	/* [한국어] (위 영어 주석 참고) 아래 선언은 시험을 위해 드러낸 것이다.
	 *  평소에는 static 이어야 할 함수들이다. */
#if IS_ENABLED(CONFIG_KUNIT)
	/* [한국어] 스트림 표 항목에서 쓰이는 비트를 알아낸다. */
void arm_smmu_get_ste_used(const __le64 *ent, __le64 *used_bits);
	/* [한국어] 한 번에 바꾸어도 되는 비트를 알아낸다. */
void arm_smmu_get_ste_update_safe(const __le64 *cur, const __le64 *target,
				  __le64 *safe_bits);
	/* [한국어] 항목을 안전한 순서로 고치는 알고리즘. */
void arm_smmu_write_entry(struct arm_smmu_entry_writer *writer, __le64 *cur,
			  const __le64 *target);
	/* [한국어] 문맥 서술자에서 쓰이는 비트를 알아낸다. */
void arm_smmu_get_cd_used(const __le64 *ent, __le64 *used_bits);
	/* [한국어] 변환 없이 통과시키는 항목을 짓는다. */
void arm_smmu_make_bypass_ste(struct arm_smmu_device *smmu,
			      struct arm_smmu_ste *target);
	/* [한국어] 문맥 서술자 표를 가리키는 항목을 짓는다. 1단계 변환의 모양이다. */
void arm_smmu_make_cdtable_ste(struct arm_smmu_ste *target,
			       struct arm_smmu_master *master, bool ats_enabled,
			       unsigned int s1dss);
	/* [한국어] 프로세스의 주소 공간을 가리키는 서술자를 짓는다. */
void arm_smmu_make_sva_cd(struct arm_smmu_cd *target,
			  struct arm_smmu_master *master, struct mm_struct *mm,
			  u16 asid);

	/* [한국어] 두 무효화 배열을 합친 새 배열을 만든다. */
struct arm_smmu_invs *arm_smmu_invs_merge(struct arm_smmu_invs *invs,
					  struct arm_smmu_invs *to_merge);
	/* [한국어] 배열의 항목 참조를 줄여 쓰레기로 표시한다. 실패하지 않는다. */
void arm_smmu_invs_unref(struct arm_smmu_invs *invs,
			 struct arm_smmu_invs *to_unref);
	/* [한국어] 쓰레기를 걷어 낸 새 배열을 만든다. */
struct arm_smmu_invs *arm_smmu_invs_purge(struct arm_smmu_invs *invs);
#endif

/* [한국어] "이 장치의 이 PASID 가 이 도메인에 붙어 있다"는 기록.
 *
 * 한 장치가 PASID 마다 다른 도메인에 붙을 수 있어, 그 조합마다 하나씩
 * 만들어 도메인의 목록에 매단다. */
struct arm_smmu_master_domain {
	/* [한국어] 도메인의 devices 목록에 매다는 고리. */
	struct list_head devices_elm;
	/* [한국어] 그 장치. */
	struct arm_smmu_master *master;
	/*
	 * For nested domains the master_domain is threaded onto the S2 parent,
	 * this points to the IOMMU_DOMAIN_NESTED to disambiguate the masters.
	 */
	/* [한국어] (위 영어 주석 참고) 중첩 도메인일 때 그것을 가리킨다.
	 *  중첩 기록은 바깥쪽 2단계 도메인의 목록에 매달리므로, 어느 게스트
	 *  도메인의 것인지 이 포인터로 가른다. */
	struct iommu_domain *domain;
	/* [한국어] 그 PASID. */
	ioasid_t ssid;
	/* [한국어] 중첩에서 ATS 무효화가 필요한가. */
	bool nested_ats_flush : 1;
	/* [한국어] 이 조합이 폴트 보고를 쓰는가. 뗄 때 참조를 놓는 데 쓴다. */
	bool using_iopf : 1;
};

/*
 * [한국어]
 * to_smmu_domain - 코어 도메인에서 이 드라이버의 도메인으로 되짚는다
 *
 * @dom: 코어가 준 도메인.
 * @return: 그것을 품은 구조체.
 */
static inline struct arm_smmu_domain *to_smmu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_domain, domain);	/* [한국어] 핵심 계층이 넘겨준 iommu_domain 을 감싸는 바깥 구조체로 되돌린다. */
}

/*
 * [한국어]
 * to_smmu_nested_domain - 코어 도메인에서 중첩 도메인으로 되짚는다
 *
 * @dom: 코어가 준 도메인.
 * @return: 그것을 품은 중첩 도메인.
 */
static inline struct arm_smmu_nested_domain *
to_smmu_nested_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_nested_domain, domain);	/* [한국어] 중첩 도메인도 같은 방식으로 되돌린다 — 첫 필드가 iommu_domain 이다. */
}

	/* [한국어] ASID 를 배정하는 전역 xarray.
	 *  VMID 와 달리 전역인 이유: 여러 SMMU 가 같은 ASID 공간을 공유하고,
	 *  SVA 는 CPU 의 ASID 와도 맞춰야 한다. */
extern struct xarray arm_smmu_asid_xa;
	/* [한국어] 그 배정을 지키는 뮤텍스. */
extern struct mutex arm_smmu_asid_lock;

	/* [한국어] 도메인의 공통 부분을 만든다. */
struct arm_smmu_domain *arm_smmu_domain_alloc(void);

/*
 * [한국어]
 * arm_smmu_domain_free - 도메인을 해제한다
 *
 * @smmu_domain: 해제할 도메인.
 *
 * 원 주석대로 이 시점에는 무효화가 동시에 돌 수 없어, RCU 유예 없이
 * 곧바로 배열을 놓아도 된다.
 */
static inline void arm_smmu_domain_free(struct arm_smmu_domain *smmu_domain)
{
	/* No concurrency with invalidation is possible at this point */
	kfree(rcu_dereference_protected(smmu_domain->invs, true));	/* [한국어] 여기서는 아무도 이 도메인을 못 보므로 RCU 유예 없이 바로 놓아도 된다. */
	kfree(smmu_domain);	/* [한국어] 도메인 몸통을 놓는다 — 이 시점에 붙어 있는 장치는 없다. */
}

	/* [한국어] 그 PASID 의 문맥 서술자를 지운다. */
void arm_smmu_clear_cd(struct arm_smmu_master *master, ioasid_t ssid);
	/* [한국어] 그 PASID 의 서술자 자리를 얻는다. 없으면 표를 잡아 준다. */
struct arm_smmu_cd *arm_smmu_get_cd_ptr(struct arm_smmu_master *master,
					u32 ssid);
	/* [한국어] 1단계 도메인의 서술자를 짓는다. */
void arm_smmu_make_s1_cd(struct arm_smmu_cd *target,
			 struct arm_smmu_master *master,
			 struct arm_smmu_domain *smmu_domain);
/*
 * [한국어]
 * arm_smmu_write_cd_entry - 문맥 서술자 한 항목을 안전하게 고쳐 쓴다
 *
 * @master: 그 서술자를 쓰는 장치.
 * @ssid: 고칠 항목의 PASID.
 * @cdptr: 표 안의 그 항목이 놓인 자리.
 * @target: 최종적으로 들어가야 할 값.
 *
 * 서술자는 8워드짜리라 한 번에 쓸 수 없다. 중간 상태를 하드웨어가 보면
 * 엉뚱한 페이지 테이블을 걷는 사고가 나므로, arm_smmu_entry_writer 규약을
 * 써서 "쓰이는 비트"를 먼저 지우고 → 나머지를 채우고 → 유효 비트를 켜는
 * 순서를 지킨다. 각 단계 사이에는 CFGI_CD 무효화를 넣어 하드웨어의 설정
 * 캐시를 씻어 낸다.
 *
 * 실행 컨텍스트: 장치를 붙이거나 PASID 를 다는 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_set_pasid()/arm_smmu_attach_dev() → [이 함수]
 *     → arm_smmu_write_entry() → arm_smmu_cmdq_issue_cmdlist()
 */
void arm_smmu_write_cd_entry(struct arm_smmu_master *master, int ssid,
			     struct arm_smmu_cd *cdptr,
			     const struct arm_smmu_cd *target);

/*
 * [한국어]
 * arm_smmu_set_pasid - 한 PASID 를 그 도메인으로 이어 붙인다
 *
 * @master: 대상 장치.
 * @smmu_domain: 그 PASID 가 쓸 1단계 도메인.
 * @pasid: 붙일 PASID 번호.
 * @cd: 그 도메인으로 만들어 둔 문맥 서술자 값.
 * @old: 그 PASID 가 쓰던 이전 도메인 (없으면 NULL).
 * @return: 0 성공, 음수 오류.
 *
 * 한 장치가 여러 주소 공간을 동시에 쓰게 하는 것이 PASID 의 요점이다.
 * 이 함수는 문맥 서술자 표에 자리를 마련하고, 무효화 배열을 새 도메인
 * 쪽으로 옮긴 뒤, 서술자를 써 넣는다. 옮기기가 실패할 수 있는 구간을
 * 먼저 지나고 나서야 실제 쓰기를 하므로, 쓰기 이후에는 되돌릴 일이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. group mutex 아래.
 *
 * 호출 체인:
 *   iommu_attach_device_pasid() → arm_smmu_attach_dev_pasid()
 *     → [이 함수] → arm_smmu_write_cd_entry()
 */
int arm_smmu_set_pasid(struct arm_smmu_master *master,
		       struct arm_smmu_domain *smmu_domain, ioasid_t pasid,
		       struct arm_smmu_cd *cd, struct iommu_domain *old);

/*
 * [한국어]
 * arm_smmu_domain_inv_range - 한 도메인의 주소 구간을 무효화한다
 *
 * @smmu_domain: 대상 도메인.
 * @iova: 무효화를 시작할 입력 주소.
 * @size: 구간 길이. 0 이면 "이 도메인 전부"라는 뜻이다.
 * @granule: 한 걸음의 크기 (보통 페이지 크기).
 * @leaf: 참이면 마지막 단계 항목만 버린다.
 *
 * 이 도메인에 걸린 무효화 배열(domain->invs)을 RCU 로 읽어, 거기 적힌
 * 대상들 — SMMU 의 TLB, 장치의 ATC, 중첩 도메인의 상위 VMID 까지 —
 * 에 각각 알맞은 명령을 만들어 한 묶음으로 큐에 넣는다. 무효화 대상이
 * 배열에 미리 모여 있으므로, 도메인에 장치가 몇 개 붙어 있든 명령을
 * 만드는 비용은 배열 길이에 비례한다.
 *
 * 실행 컨텍스트: 매핑 해제 경로. 원자적 문맥에서 불릴 수 있어 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu_unmap()/io_pgtable flush → arm_smmu_tlb_inv_* → [이 함수]
 *     → arm_smmu_cmdq_issue_cmdlist()
 */
void arm_smmu_domain_inv_range(struct arm_smmu_domain *smmu_domain,
			       unsigned long iova, size_t size,
			       unsigned int granule, bool leaf);

/*
 * [한국어]
 * arm_smmu_domain_inv - 그 도메인의 변환 캐시를 통째로 비운다
 *
 * @smmu_domain: 대상 도메인.
 *
 * 구간 무효화의 특수한 경우 — 크기 0 을 "전부"로 약속해 두고 같은
 * 경로를 그대로 쓴다. 도메인의 페이지 테이블을 크게 갈아엎었거나,
 * 어느 범위가 바뀌었는지 추적하기보다 통째로 비우는 편이 싼 자리에서
 * 쓴다.
 *
 * 실행 컨텍스트: 호출자와 같다 — 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev()/arm_smmu_flush_iotlb_all() → [이 함수]
 *     → arm_smmu_domain_inv_range()
 */
static inline void arm_smmu_domain_inv(struct arm_smmu_domain *smmu_domain)
{
	arm_smmu_domain_inv_range(smmu_domain, 0, 0, 0, false);	/* [한국어] 크기 0 이 곧 "전체"라는 약속 — 범위 무효화 경로를 그대로 재사용한다. */
}

/*
 * [한국어]
 * __arm_smmu_cmdq_skip_err - 하드웨어가 걸려 넘어진 명령을 건너뛴다
 *
 * @smmu: 그 SMMU.
 * @cmdq: 문제가 난 명령 큐.
 *
 * 하드웨어가 잘못된 명령을 만나면 그 자리에서 멈춰 선다. 그대로 두면
 * 뒤의 명령이 하나도 진행되지 않으므로, 드라이버가 그 자리를 CMD_SYNC
 * (아무 일도 하지 않는 안전한 명령)로 덮어써서 큐를 다시 굴린다.
 * 명령을 잃는 대신 시스템이 멈추지 않게 하는 선택이다.
 *
 * 실행 컨텍스트: gerror 인터럽트 처리기. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_gerror_handler() → [이 함수]
 */
void __arm_smmu_cmdq_skip_err(struct arm_smmu_device *smmu,
			      struct arm_smmu_cmdq *cmdq);
/*
 * [한국어]
 * arm_smmu_init_one_queue - 큐 하나의 링 버퍼와 레지스터 자리를 마련한다
 *
 * @smmu: 그 SMMU.
 * @q: 채울 큐 구조체.
 * @page: 그 큐의 포인터 레지스터가 놓인 MMIO 페이지.
 * @prod_off: 생산 포인터 레지스터의 오프셋.
 * @cons_off: 소비 포인터 레지스터의 오프셋.
 * @dwords: 항목 하나가 몇 워드인가 (명령 2, 이벤트 4, PRI 2).
 * @name: 로그에 찍을 이름.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 링 버퍼를 dma 로 잡되, 요청한 크기를 다 못 잡으면 절반씩 줄여 가며
 * 다시 시도한다 — 큐가 작아도 동작은 하기 때문이다. 명령 큐와 이벤트 큐,
 * PRI 큐가 모두 이 함수를 거쳐 같은 모양으로 만들어진다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_structures() → arm_smmu_init_queues() → [이 함수]
 */
int arm_smmu_init_one_queue(struct arm_smmu_device *smmu,
			    struct arm_smmu_queue *q, void __iomem *page,
			    unsigned long prod_off, unsigned long cons_off,
			    size_t dwords, const char *name);
/*
 * [한국어]
 * arm_smmu_cmdq_init - 명령 큐의 락 없는 삽입 장치를 준비한다
 *
 * @smmu: 그 SMMU.
 * @cmdq: 준비할 명령 큐.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 링 버퍼 자체는 arm_smmu_init_one_queue() 가 이미 잡아 두었고, 여기서는
 * 그 위에 얹히는 valid_map 비트맵을 잡는다. 이 비트맵이 있어야 여러 CPU 가
 * 락 없이 자기 자리에 명령을 쓴 뒤 "다 썼다"를 서로에게 알릴 수 있다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_queues() → [이 함수]
 */
int arm_smmu_cmdq_init(struct arm_smmu_device *smmu,
		       struct arm_smmu_cmdq *cmdq);

/*
 * [한국어]
 * arm_smmu_master_canwbs - 이 장치의 DMA 가 캐시를 거쳐 오는가
 *
 * @master: 대상 장치.
 * @return: 캐시 일관(coherent) 경로면 참.
 *
 * PCI 루트 컴플렉스가 "쓰기가 캐시에 반영된다(write-back cacheable)"고
 * 펌웨어에 적어 두면 이 값이 참이 된다. 참이면 게스트에게 캐시 조작
 * 권한을 넘겨도 안전하므로, 중첩 변환을 허용할지 판단하는 근거가 된다.
 * 거짓인 장치에 중첩 도메인을 붙이면 게스트가 캐시를 못 씻어 데이터가
 * 어긋날 수 있다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_vsmmu_init()/arm_smmu_attach_prepare() → [이 함수]
 *     → dev_iommu_fwspec_get()
 */
static inline bool arm_smmu_master_canwbs(struct arm_smmu_master *master)
{
	/* [한국어] 펌웨어(ACPI IORT / DT)가 적어 둔 플래그에서 그 비트만 본다 —
	 * 드라이버가 스스로 알 수 없는 성질이라 펌웨어의 선언에 기댄다. */
	return dev_iommu_fwspec_get(master->dev)->flags &
	       IOMMU_FWSPEC_PCI_RC_CANWBS;
}

/**
 * struct arm_smmu_inv_state - Per-domain invalidation array state
 * @invs_ptr: points to the domain->invs (unwinding nesting/etc.) or is NULL if
 *            no change should be made
 * @old_invs: the original invs array
 * @new_invs: for new domain, this is the new invs array to update domain->invs;
 *            for old domain, this is the master->build_invs to pass in as the
 *            to_unref argument to an arm_smmu_invs_unref() call
 */
/* [한국어] 붙이기 도중 무효화 배열을 갈아 끼우기 위해 들고 다니는 중간 상태.
 *
 * 도메인의 무효화 배열은 "실패하지 않는 교체"가 되어야 한다. 그래서
 * 실패할 수 있는 일(새 배열 잡기, 병합)은 prepare 단계에서 미리 끝내
 * 이 구조에 담아 두고, commit 단계에서는 포인터만 바꿔 끼운다.
 * (위 영어 kernel-doc 참고) */
struct arm_smmu_inv_state {
	/* [한국어] 바꿔 끼울 자리 — 보통 &domain->invs 를 가리킨다.
	 * 설정자: arm_smmu_attach_prepare() 가 어느 도메인을 건드릴지 정하며 채운다.
	 * 읽는 자: commit 단계가 이 자리에 new_invs 를 rcu_assign_pointer 로 건다.
	 * 값 범위: NULL 이면 "이 도메인은 손대지 않는다"는 뜻이다 —
	 *         중첩 도메인처럼 부모 쪽 배열을 그대로 쓰는 경우가 그렇다.
	 * 동기화: 읽는 쪽은 RCU 로 보므로, 바꿔 끼운 뒤 옛 배열은 유예 기간을
	 *         지나서야 놓는다. */
	struct arm_smmu_invs __rcu **invs_ptr;
	/* [한국어] 갈아 끼우기 전에 걸려 있던 배열.
	 * 설정자: prepare 단계가 rcu_dereference_protected() 로 읽어 담는다.
	 * 읽는 자: commit 이 끝난 뒤 유예 기간을 기다렸다가 이 배열을 놓는다.
	 *         붙이기가 도중에 접히면 이 값을 도로 걸어 원상 복구한다.
	 * 값 범위: 항상 유효한 배열 — 도메인은 빈 배열이라도 하나는 들고 있다.
	 * 동기화: 위와 같다. */
	struct arm_smmu_invs *old_invs;
	/* [한국어] 새 도메인 쪽에서는 걸어 둘 새 배열, 옛 도메인 쪽에서는
	 * "빼낼 항목 목록"으로 쓰인다 — 한 필드를 두 뜻으로 겸한다.
	 * 설정자: prepare 단계가 arm_smmu_invs_merge() 로 만들어 담거나,
	 *         master->build_invs 를 그대로 가리키게 한다.
	 * 읽는 자: commit 단계가 새 도메인에는 이 배열을 걸고, 옛 도메인에는
	 *         이 목록을 arm_smmu_invs_unref() 의 인자로 넘긴다.
	 * 값 범위: invs_ptr 이 NULL 이면 이 값도 쓰이지 않는다.
	 * 동기화: 만드는 일은 실패할 수 있으므로 반드시 prepare 에서 끝낸다 —
	 *         commit 은 실패하면 안 되는 구간이다. (위 영어 주석 참고) */
	struct arm_smmu_invs *new_invs;
};

/* [한국어] 장치 하나를 도메인에 붙이는 동안 두 단계가 주고받는 상태 묶음.
 *
 * 붙이기는 prepare → (STE/CD 쓰기) → commit 세 걸음으로 나뉜다. 앞 걸음에서
 * 실패할 수 있는 일을 모두 끝내 이 구조에 담아 두고, 하드웨어에 실제로
 * 쓰고 난 뒤의 commit 은 절대 실패하지 않는 일만 한다. 그래야 하드웨어가
 * 이미 새 설정을 보고 있는데 커널 쪽 상태만 옛것으로 남는 어긋남이 없다. */
struct arm_smmu_attach_state {
	/* Inputs */
	/* [한국어] 이 장치(또는 PASID)가 지금까지 붙어 있던 도메인.
	 * 설정자: 붙이기를 시작하는 콜백이 iommu 코어에게 받은 값으로 채운다.
	 * 읽는 자: prepare 가 옛 도메인의 무효화 배열에서 이 장치를 빼내려고 본다.
	 * 값 범위: 처음 붙이는 경우에도 코어가 blocked/identity 도메인을 준다.
	 * 동기화: group mutex 아래에서만 다룬다. */
	struct iommu_domain *old_domain;
	/* [한국어] 붙이는 대상 장치의 SMMU 쪽 상태.
	 * 설정자: 호출자가 dev_iommu_priv_get() 으로 얻어 채운다.
	 * 읽는 자: 스트림 목록, 문맥 표, 무효화 재료를 모두 여기서 꺼낸다.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: group mutex 아래. */
	struct arm_smmu_master *master;
	/* [한국어] 문맥 서술자 쪽 사정 때문에 ATS 를 켜야 하는가.
	 * 설정자: SVA 처럼 장치 캐시 무효화가 반드시 필요한 경로가 참으로 준다.
	 * 읽는 자: ats_enabled 를 정하는 계산에 들어간다.
	 * 값 범위: true/false.
	 * 동기화: group mutex 아래. */
	bool cd_needs_ats;
	/* [한국어] 이번 붙이기에서는 ATS 를 끄고 가야 하는가.
	 * 설정자: 중첩 변환처럼 장치 캐시를 믿을 수 없는 설정이 참으로 준다.
	 * 읽는 자: 위 cd_needs_ats 보다 이 값이 우선한다.
	 * 값 범위: true/false.
	 * 동기화: group mutex 아래. */
	bool disable_ats;
	/* [한국어] 붙이는 대상 PASID.
	 * 설정자: PASID 없는 붙이기는 IOMMU_NO_PASID(0) 로 둔다.
	 * 읽는 자: 문맥 서술자 표의 어느 항목을 고칠지 정하고, 무효화 항목의
	 *         범위를 정할 때 쓴다.
	 * 값 범위: 0 ~ 장치가 지원하는 최대 PASID.
	 * 동기화: group mutex 아래. */
	ioasid_t ssid;
	/* Resulting state */
	/* [한국어] 중첩 변환일 때 이 장치를 게스트 쪽에 이어 줄 다리.
	 * 설정자: arm_smmu_attach_prepare_vmaster() 가 만들어 담는다.
	 * 읽는 자: commit 이 master->vmaster 자리에 옮겨 건다.
	 * 값 범위: 중첩이 아니면 NULL.
	 * 동기화: 실제로 거는 일은 vmaster_lock 아래에서 한다 — 이벤트를
	 *         전달하는 인터럽트 경로와 겹치기 때문이다. */
	struct arm_smmu_vmaster *vmaster;
	/* [한국어] 옛 도메인 쪽 무효화 배열을 되돌리기 위한 중간 상태.
	 * 설정자: prepare 가 옛 도메인에서 이 장치의 항목을 빼낼 준비를 하며 채운다.
	 * 읽는 자: commit 이 이 상태를 보고 실제로 항목을 걷어 낸다.
	 * 값 범위: 옛 도메인이 없거나 손댈 필요가 없으면 invs_ptr 이 NULL 이다.
	 * 동기화: group mutex 아래. */
	struct arm_smmu_inv_state old_domain_invst;
	/* [한국어] 새 도메인 쪽 무효화 배열을 갈아 끼우기 위한 중간 상태.
	 * 설정자: prepare 가 새 배열을 미리 만들어 담는다 — 여기서 실패하면
	 *         하드웨어를 건드리기 전이라 그냥 접으면 된다.
	 * 읽는 자: commit 이 포인터만 바꿔 끼운다.
	 * 값 범위: 위와 같다.
	 * 동기화: group mutex 아래. */
	struct arm_smmu_inv_state new_domain_invst;
	/* [한국어] 이번 붙이기 결과 ATS 를 켜기로 했는가.
	 * 설정자: prepare 가 장치 능력·cd_needs_ats·disable_ats 를 견줘 정한다.
	 * 읽는 자: STE 를 짓는 곳이 EATS 필드를 이 값으로 채우고, commit 이
	 *         실제 PCI ATS 를 켠다. 무효화 배열에 ATS 항목을 넣을지도
	 *         이 값이 가른다.
	 * 값 범위: true/false.
	 * 동기화: group mutex 아래. */
	bool ats_enabled;
};

/*
 * [한국어]
 * arm_smmu_attach_prepare - 붙이기에서 실패할 수 있는 일을 먼저 끝낸다
 *
 * @state: 붙이기 상태 묶음. 입력 필드는 채워져 있어야 하고, 결과 필드가 채워진다.
 * @new_domain: 붙일 도메인.
 * @return: 0 성공, 음수면 아무것도 바뀌지 않은 채 실패.
 *
 * ATS 를 켤지 정하고, 새 도메인의 무효화 배열을 미리 만들고, 중첩이면
 * vmaster 를 준비한다. 이 함수가 성공한 뒤에야 호출자가 STE/CD 를 실제로
 * 쓰고, 그다음 arm_smmu_attach_commit() 을 부른다. 실패하면 하드웨어는
 * 아직 옛 설정을 보고 있으므로 되돌릴 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, group mutex 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev() → [이 함수] → arm_smmu_invs_merge()
 */
int arm_smmu_attach_prepare(struct arm_smmu_attach_state *state,
			    struct iommu_domain *new_domain);
/*
 * [한국어]
 * arm_smmu_attach_commit - 하드웨어에 쓴 뒤의 뒷정리를 마무리한다
 *
 * @state: prepare 가 채워 둔 상태 묶음.
 *
 * 실제 PCI ATS 를 켜거나 끄고, 무효화 배열을 새 것으로 바꿔 끼우고,
 * 옛 도메인에서 이 장치의 항목을 걷어 낸다. 여기서 하는 일은 모두
 * 실패할 수 없는 일들이다 — 이미 하드웨어가 새 설정을 보고 있으므로
 * 중간에 접을 방법이 없기 때문이다.
 *
 * 실행 컨텍스트: prepare 와 같은 문맥, 같은 mutex 아래.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev() → [이 함수] → arm_smmu_invs_unref()
 */
void arm_smmu_attach_commit(struct arm_smmu_attach_state *state);
/*
 * [한국어]
 * arm_smmu_install_ste_for_dev - 그 장치의 모든 스트림에 같은 STE 를 써 넣는다
 *
 * @master: 대상 장치.
 * @target: 써 넣을 스트림 표 항목 값.
 *
 * 한 장치가 스트림 id 를 여러 개 가질 수 있으므로(별칭, 다기능 장치),
 * 그 목록을 돌며 같은 값을 쓴다. 각 쓰기는 arm_smmu_write_ste() 의
 * 다단계 규약을 거쳐, 하드웨어가 중간의 어긋난 상태를 보지 않게 한다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev() → [이 함수] → arm_smmu_write_ste()
 */
void arm_smmu_install_ste_for_dev(struct arm_smmu_master *master,
				  const struct arm_smmu_ste *target);

/*
 * [한국어]
 * arm_smmu_cmdq_issue_cmdlist - 명령 여러 개를 락 없이 큐에 밀어 넣는다
 *
 * @smmu: 그 SMMU.
 * @cmdq: 넣을 명령 큐.
 * @cmds: 이미 짜인 명령 워드들.
 * @n: 명령 개수.
 * @sync: 참이면 뒤에 CMD_SYNC 를 붙이고 완료까지 기다린다.
 * @return: 0 성공, 음수면 큐가 막혔거나 시간이 다 됐다.
 *
 * 이 드라이버에서 가장 정교한 함수다. 여러 CPU 가 동시에 들어와도
 * 락을 잡지 않는다 — 각 CPU 가 원자 연산으로 자기 자리(구간)를 차지하고,
 * 그 자리에 명령을 쓴 뒤 valid_map 비트를 뒤집어 "내 몫은 다 됐다"를 알린다.
 * 구간의 첫 자리를 차지한 CPU 가 대표(owner)가 되어, 앞선 모든 CPU 가
 * 쓰기를 마칠 때까지 기다렸다가 하드웨어에 생산 포인터를 한 번만 알린다.
 * 이렇게 하면 MMIO 쓰기 횟수가 줄고, 락 경합도 사라진다.
 *
 * 실행 컨텍스트: 무효화 경로라 원자적 문맥에서도 불린다. 기다릴 때는
 * 잠들지 않고 cpu_relax() 로 돈다.
 *
 * 호출 체인:
 *   arm_smmu_domain_inv_range()/arm_smmu_write_entry() → [이 함수]
 */
int arm_smmu_cmdq_issue_cmdlist(struct arm_smmu_device *smmu,
				struct arm_smmu_cmdq *cmdq, u64 *cmds, int n,
				bool sync);

/* [한국어] 여기부터는 SVA(공유 가상 주소) 지원이 켜졌을 때만 있는 선언들.
 * SVA 는 장치가 CPU 프로세스의 페이지 테이블을 그대로 쓰게 하는 기능이라,
 * mmu_notifier 와 폴트 처리가 함께 필요하다. 끄고 빌드하면 아래 #else 의
 * 빈 껍데기들이 대신 들어가 호출부를 그대로 둘 수 있다. */
#ifdef CONFIG_ARM_SMMU_V3_SVA
bool arm_smmu_sva_supported(struct arm_smmu_device *smmu);
/*
 * [한국어]
 * arm_smmu_sva_notifier_synchronize - mmu 알림이 모두 끝나기를 기다린다
 *
 * 모듈을 내릴 때, 아직 돌고 있는 mmu_notifier 콜백이 있으면 이미 사라진
 * 코드를 밟게 된다. 그래서 모듈 해제 직전에 이 함수로 남은 콜백이 다
 * 빠져나가기를 기다린다.
 *
 * 실행 컨텍스트: 모듈 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_driver_exit() → [이 함수] → mmu_notifier_synchronize()
 */
void arm_smmu_sva_notifier_synchronize(void);
/*
 * [한국어]
 * arm_smmu_sva_domain_alloc - 프로세스 주소 공간을 쓰는 도메인을 만든다
 *
 * @dev: 그 주소 공간을 쓸 장치.
 * @mm: 붙일 프로세스의 메모리 서술자.
 * @return: 만들어진 도메인, 실패하면 ERR_PTR.
 *
 * 보통 도메인과 달리 페이지 테이블을 새로 만들지 않는다 — CPU 가 쓰던
 * mm 의 페이지 테이블을 그대로 가리키고, mmu_notifier 를 달아 CPU 쪽
 * 변경이 SMMU TLB 에도 반영되게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_sva_bind_device() → [이 함수] → arm_smmu_mmu_notifier_get()
 */
struct iommu_domain *arm_smmu_sva_domain_alloc(struct device *dev,
					       struct mm_struct *mm);
#else /* CONFIG_ARM_SMMU_V3_SVA */
/*
 * [한국어]
 * arm_smmu_sva_supported - SVA 를 끈 빌드에서의 빈 껍데기
 *
 * @smmu: 쓰이지 않는다.
 * @return: 항상 거짓 — 어떤 SMMU 도 공유 주소 공간을 못 쓴다고 답한다.
 *
 * 호출부가 #ifdef 없이 그대로 쓸 수 있게 하려고 둔 자리다.
 */
static inline bool arm_smmu_sva_supported(struct arm_smmu_device *smmu)
{
	return false;	/* [한국어] SVA 를 끄고 빌드하면 어떤 SMMU 도 공유 주소 공간을 못 쓴다고 답한다. */
}

/*
 * [한국어]
 * arm_smmu_sva_notifier_synchronize - SVA 를 끈 빌드에서의 빈 껍데기
 *
 * 기다릴 알림 자체가 없으므로 아무 일도 하지 않는다. 호출부에
 * #ifdef 를 뿌리지 않으려고 이렇게 둔다.
 */
static inline void arm_smmu_sva_notifier_synchronize(void) {}

#define arm_smmu_sva_domain_alloc NULL	/* [한국어] SVA 를 끄면 도메인 할당 콜백 자리를 NULL 로 채워 핵심 계층이 건너뛰게 한다. */

#endif /* CONFIG_ARM_SMMU_V3_SVA */

/* [한국어] NVIDIA Tegra241 은 표준 명령 큐 하나 대신 여러 개의 보조 큐를 얹어
 * 무효화 처리량을 늘렸다. 그 확장을 켜고 빌드했을 때만 아래 탐지 함수가 실제
 * 구현으로 이어지고, 끄면 "그런 하드웨어 없음"으로 답하는 껍데기가 들어간다. */
#ifdef CONFIG_TEGRA241_CMDQV
/*
 * [한국어]
 * tegra241_cmdqv_probe - Tegra241 보조 명령 큐 확장이 있는지 알아본다
 *
 * @smmu: 방금 찾아낸 SMMU.
 * @return: 확장이 있으면 그것을 품은 더 큰 구조체, 없으면 ERR_PTR.
 *
 * 표준 SMMUv3 프로브 도중에 불려, 이 하드웨어가 Tegra 의 CMDQV 확장을
 * 달고 있는지 ACPI 표를 보고 판단한다. 있으면 arm_smmu_device 를 품은
 * 더 큰 구조체로 바꿔 돌려주고, 그 뒤로는 명령을 보조 큐들에 나눠 넣는다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수]
 */
struct arm_smmu_device *tegra241_cmdqv_probe(struct arm_smmu_device *smmu);
#else /* CONFIG_TEGRA241_CMDQV */
/*
 * [한국어]
 * tegra241_cmdqv_probe - Tegra 확장을 끈 빌드에서의 빈 껍데기
 *
 * @smmu: 쓰이지 않는다.
 * @return: 항상 ERR_PTR(-ENODEV) — 호출자가 "확장 없음"으로 읽고
 *          표준 명령 큐 하나만 쓰는 경로로 간다.
 */
static inline struct arm_smmu_device *
tegra241_cmdqv_probe(struct arm_smmu_device *smmu)
{
	return ERR_PTR(-ENODEV);	/* [한국어] Tegra 확장을 끄고 빌드하면 이 장치는 없는 것으로 친다. */
}
#endif /* CONFIG_TEGRA241_CMDQV */

struct arm_vsmmu {
	/* [한국어] iommufd 가 아는 가상 IOMMU 몸통 — 반드시 첫 필드여야 한다.
	 * 설정자: iommufd 가 객체를 만들며 채우고, arm_vsmmu_init() 이 이어받는다.
	 * 읽는 자: container_of 로 이 구조를 되찾는 모든 곳.
	 * 값 범위: 첫 필드라는 약속이 깨지면 되찾기가 무너진다.
	 * 동기화: iommufd 객체 수명 규칙(참조 계수)을 따른다. */
	struct iommufd_viommu core;
	/* [한국어] 이 가상 IOMMU 를 떠받치는 실제 SMMU.
	 * 설정자: arm_vsmmu_init() 이 부모 도메인에서 꺼내 채운다.
	 * 읽는 자: 게스트가 낸 무효화 명령을 실제 큐에 밀어 넣을 때.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 이 객체가 사는 동안 바뀌지 않는다. */
	struct arm_smmu_device *smmu;
	/* [한국어] 게스트 변환의 바깥을 감싸는 2단계 도메인.
	 * 설정자: 사용자가 부모로 지정한 도메인을 검사한 뒤 담는다.
	 * 읽는 자: 중첩 도메인을 만들 때 그 부모로 쓰이고, VMID 도 여기서 온다.
	 * 값 범위: 반드시 2단계 도메인이어야 한다 — 아니면 만들기가 거부된다.
	 * 동기화: 참조를 잡아 두므로 이 객체보다 먼저 사라지지 않는다. */
	struct arm_smmu_domain *s2_parent;
	/* [한국어] 이 가상 기계에 배정된 VMID.
	 * 설정자: s2_parent 의 s2_cfg.vmid 를 그대로 옮겨 담는다.
	 * 읽는 자: 게스트가 낸 무효화를 실제 명령으로 옮길 때, 게스트가 적은
	 *         VMID 대신 이 값을 강제로 써 넣는다 — 게스트가 남의 VMID 를
	 *         지우지 못하게 막는 핵심 장치다.
	 * 값 범위: SMMU 가 지원하는 VMID 폭 안.
	 * 동기화: 이 객체가 사는 동안 바뀌지 않는다. */
	u16 vmid;
};

/* [한국어] 여기부터는 iommufd 연동(사용자 공간이 직접 IOMMU 를 다루는 길)이
 * 켜졌을 때만 있는 선언들. 게스트에게 SMMU 를 그대로 보여 주는 중첩 변환이
 * 여기에 얹힌다. 끄고 빌드하면 아래 #else 의 NULL 매크로와 빈 껍데기가
 * 대신 들어간다. */
#if IS_ENABLED(CONFIG_ARM_SMMU_V3_IOMMUFD)
void *arm_smmu_hw_info(struct device *dev, u32 *length,
		       enum iommu_hw_info_type *type);
/*
 * [한국어]
 * arm_smmu_get_viommu_size - 가상 IOMMU 객체를 담을 크기를 알려 준다
 *
 * @dev: 그 가상 IOMMU 에 붙을 장치.
 * @viommu_type: 사용자가 요청한 가상 IOMMU 종류.
 * @return: 필요한 바이트 수, 지원하지 않는 종류면 0.
 *
 * iommufd 는 객체를 자기가 잡되 크기는 드라이버에게 묻는다. 이 드라이버는
 * struct arm_vsmmu 크기를 답하며, 그 과정에서 이 장치와 이 SMMU 가
 * 중첩 변환을 정말 지원하는지도 함께 검사한다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd_viommu_alloc_ioctl() → [이 함수]
 */
size_t arm_smmu_get_viommu_size(struct device *dev,
				enum iommu_viommu_type viommu_type);
/*
 * [한국어]
 * arm_vsmmu_init - 가상 SMMU 객체를 실제 SMMU 에 이어 붙인다
 *
 * @viommu: iommufd 가 잡아 둔 객체.
 * @parent_domain: 게스트 변환을 감쌀 2단계 도메인.
 * @user_data: 사용자가 함께 넘긴 설정.
 * @return: 0 성공, 음수 오류.
 *
 * 부모 도메인이 정말 2단계인지, 이 SMMU 가 중첩을 지원하는지 검사한 뒤
 * VMID 를 이어받고 콜백표를 건다. 이 뒤로 게스트는 자기 스트림 표와
 * 문맥 표를 직접 짓고, 무효화 명령도 직접 낼 수 있게 된다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd_viommu_alloc_ioctl() → [이 함수]
 */
int arm_vsmmu_init(struct iommufd_viommu *viommu,
		   struct iommu_domain *parent_domain,
		   const struct iommu_user_data *user_data);
/*
 * [한국어]
 * arm_smmu_attach_prepare_vmaster - 중첩 붙이기에서 게스트 쪽 다리를 미리 만든다
 *
 * @state: 붙이기 상태 묶음.
 * @nested_domain: 붙일 중첩 도메인.
 * @return: 0 성공, -ENOMEM 등 음수 오류.
 *
 * 하드웨어가 낸 사건(폴트 등)을 게스트에게 그대로 돌려주려면, 실제
 * 스트림 번호와 게스트가 아는 스트림 번호를 잇는 자리가 필요하다.
 * 그 자리(struct arm_smmu_vmaster)를 여기서 미리 잡아 state 에 담는다 —
 * 잡기가 실패할 수 있으므로 prepare 단계에 둔다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare() → [이 함수]
 */
int arm_smmu_attach_prepare_vmaster(struct arm_smmu_attach_state *state,
				    struct arm_smmu_nested_domain *nested_domain);
/*
 * [한국어]
 * arm_smmu_attach_commit_vmaster - 준비해 둔 게스트 다리를 실제로 건다
 *
 * @state: prepare 가 채워 둔 상태 묶음.
 *
 * master->vmaster 자리를 새 값으로 바꾸고 옛 값을 놓는다. 이벤트를
 * 전달하는 인터럽트 경로와 겹치는 자리라, 바꾸는 동안 vmaster_lock 을
 * 잡는다. 잡을 것은 이미 다 잡아 두었으므로 이 함수는 실패하지 않는다.
 *
 * 실행 컨텍스트: 붙이기 경로. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_commit() → [이 함수]
 */
void arm_smmu_attach_commit_vmaster(struct arm_smmu_attach_state *state);
/*
 * [한국어]
 * arm_smmu_master_clear_vmaster - 그 장치의 게스트 다리를 끊는다
 *
 * @master: 대상 장치.
 *
 * 장치를 떼거나 중첩 도메인을 내릴 때, 게스트에게 사건을 전달하던
 * 연결을 끊고 자리를 놓는다. 인터럽트 경로가 이미 이 값을 잡고 있을 수
 * 있으므로 락 아래에서 떼어 낸 뒤에 놓는다.
 *
 * 실행 컨텍스트: 떼기 경로. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   arm_smmu_release_device()/붙이기 경로 → [이 함수]
 */
void arm_smmu_master_clear_vmaster(struct arm_smmu_master *master);
/*
 * [한국어]
 * arm_vmaster_report_event - 하드웨어 사건을 게스트에게 그대로 넘긴다
 *
 * @vmaster: 그 장치의 게스트 쪽 다리.
 * @evt: 이벤트 큐에서 읽은 원본 워드들.
 * @return: 0 성공, 음수면 넘기지 못했다 — 호스트가 대신 처리해야 한다.
 *
 * 이벤트 안의 스트림 번호를 게스트가 아는 번호로 바꿔 끼운 뒤, iommufd 의
 * 사건 큐에 넣는다. 게스트 커널은 자기 SMMU 의 이벤트 큐에서 읽은 것처럼
 * 그 기록을 보게 된다.
 *
 * 실행 컨텍스트: 이벤트 큐 인터럽트 스레드.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread() → [이 함수] → iommufd_viommu_report_event()
 */
int arm_vmaster_report_event(struct arm_smmu_vmaster *vmaster, u64 *evt);
/*
 * [한국어]
 * arm_vsmmu_alloc_domain_nested - 게스트가 지은 1단계 설정을 도메인으로 감싼다
 *
 * @viommu: 그 가상 SMMU.
 * @flags: 사용자가 준 플래그.
 * @user_data: 게스트가 지은 STE 원본 값.
 * @return: 만들어진 중첩 도메인, 실패하면 ERR_PTR.
 *
 * 게스트가 넘긴 STE 값에서 커널이 쥐고 있어야 할 비트를 걷어 내고
 * (STRTAB_STE_0/1_NESTING_ALLOWED 마스크가 그 경계다), 나머지만 받아들여
 * 도메인에 담는다. 이렇게 걸러야 게스트가 남의 문맥이나 물리 주소에
 * 손대지 못한다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd_hwpt_alloc() → [이 함수]
 */
struct iommu_domain *
arm_vsmmu_alloc_domain_nested(struct iommufd_viommu *viommu, u32 flags,
			      const struct iommu_user_data *user_data);
/*
 * [한국어]
 * arm_vsmmu_cache_invalidate - 게스트가 낸 무효화 명령을 대신 실행한다
 *
 * @viommu: 그 가상 SMMU.
 * @array: 게스트가 넘긴 명령 배열.
 * @return: 0 성공, 음수 오류. 처리한 개수는 array 에 적어 돌려준다.
 *
 * 게스트는 자기 명령 큐에 넣듯 무효화 명령을 짓지만, 실제 하드웨어 큐에는
 * 넣을 수 없다. 이 함수가 그 명령들을 하나씩 검사해 — 허용된 종류인지,
 * VMID/ASID 를 남의 것으로 적지는 않았는지 — 걸러 낸 뒤 실제 큐에 넣는다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd_viommu_invalidate_ioctl() → [이 함수]
 *     → arm_smmu_cmdq_issue_cmdlist()
 */
int arm_vsmmu_cache_invalidate(struct iommufd_viommu *viommu,
			       struct iommu_user_data_array *array);
#else
#define arm_smmu_get_viommu_size NULL	/* [한국어] iommufd 가 없으면 가상 IOMMU 크기를 물어볼 일도 없다. */
#define arm_smmu_hw_info NULL	/* [한국어] 하드웨어 정보 노출도 iommufd 전용이라 자리만 비운다. */
#define arm_vsmmu_init NULL	/* [한국어] 가상 SMMU 초기화 콜백 자리를 비운다. */
#define arm_vsmmu_alloc_domain_nested NULL	/* [한국어] 중첩 도메인 할당 콜백 자리를 비운다. */
#define arm_vsmmu_cache_invalidate NULL	/* [한국어] 게스트가 낸 무효화를 받아 줄 콜백 자리를 비운다. */

/*
 * [한국어]
 * arm_smmu_attach_prepare_vmaster - iommufd 를 끈 빌드에서의 빈 껍데기
 *
 * @state: 쓰이지 않는다.
 * @nested_domain: 쓰이지 않는다.
 * @return: 항상 0 — 준비할 게스트 다리가 없으므로 성공으로 친다.
 */
static inline int
arm_smmu_attach_prepare_vmaster(struct arm_smmu_attach_state *state,
				struct arm_smmu_nested_domain *nested_domain)
{
	return 0;	/* [한국어] iommufd 가 없으면 vmaster 를 붙일 것이 없으니 성공으로 친다. */
}

/*
 * [한국어]
 * arm_smmu_attach_commit_vmaster - iommufd 를 끈 빌드에서의 빈 껍데기
 *
 * 게스트 다리를 걸 일이 없으므로 아무 일도 하지 않는다.
 */
static inline void
arm_smmu_attach_commit_vmaster(struct arm_smmu_attach_state *state)
{
}

/*
 * [한국어]
 * arm_smmu_master_clear_vmaster - iommufd 를 끈 빌드에서의 빈 껍데기
 *
 * 끊을 다리가 없으므로 아무 일도 하지 않는다.
 */
static inline void
arm_smmu_master_clear_vmaster(struct arm_smmu_master *master)
{
}

/*
 * [한국어]
 * arm_vmaster_report_event - iommufd 를 끈 빌드에서의 빈 껍데기
 *
 * @vmaster: 쓰이지 않는다.
 * @evt: 쓰이지 않는다.
 * @return: 항상 -EOPNOTSUPP — 호출자가 "게스트에게 못 넘겼다"로 읽고
 *          호스트 쪽 경로로 사건을 처리한다.
 */
static inline int arm_vmaster_report_event(struct arm_smmu_vmaster *vmaster,
					   u64 *evt)
{
	return -EOPNOTSUPP;	/* [한국어] iommufd 를 끄고 빌드하면 게스트에게 사건을 전달할 길이 없다. */
}
#endif /* CONFIG_ARM_SMMU_V3_IOMMUFD */

#endif /* _ARM_SMMU_V3_H */
