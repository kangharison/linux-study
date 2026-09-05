// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU API for ARM architected SMMUv3 implementations.
 *
 * Copyright (C) 2015 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 *
 * This driver is powered by bad coffee and bombay mix.
 */

/*
 * [한국어 설명] ARM SMMUv3 드라이버의 본체 (arm-smmu-v3.c)
 *
 * === 파일의 역할 ===
 * ARM System MMU 3세대 하드웨어를 커널의 iommu 계층에 이어 붙이는 드라이버
 * 본체다. 하드웨어를 찾아 초기화하고, 스트림 표와 문맥 서술자 표를 짓고,
 * 장치를 도메인에 붙이고, 페이지 테이블 변경을 하드웨어에 알리고, 폴트를
 * 받아 처리하는 일이 모두 여기 있다. 5천 줄이 넘지만 관심사는 뚜렷하게
 * 다섯 덩이로 나뉜다.
 * 첫째는 큐다. 명령 큐·이벤트 큐·PRI 큐가 링 버퍼 하나에 생산·소비 포인터
 * 두 개라는 같은 모양을 쓰며, 파일 앞머리의 queue_* 헬퍼가 그 공통 부분을
 * 맡는다. 그 위에 명령 큐만의 락 없는 삽입 규약이 얹힌다 — 여러 CPU 가
 * 락 없이 자기 자리를 차지하고 대표 하나가 하드웨어에 알리는 구조로,
 * 이 드라이버에서 가장 정교한 대목이다.
 * 둘째는 항목 쓰기 규약이다. 스트림 표 항목(STE)과 문맥 서술자(CD)는
 * 8워드짜리라 한 번에 못 쓰는데, 하드웨어는 언제든 그것을 읽어 간다.
 * "지금 설정이 실제로 읽는 비트"를 계산해 안전한 순서로 나눠 쓰는
 * arm_smmu_write_entry() 가 그 문제를 푼다.
 * 셋째는 무효화다. 도메인마다 "무엇을 무효화해야 하는가"를 배열에 미리
 * 모아 두고(arm_smmu_invs), 매핑이 바뀌면 그 배열만 훑어 명령을 짓는다.
 * 넷째는 붙이기다. 실패할 수 있는 일을 앞 단계로 몰고 하드웨어에 쓴 뒤에는
 * 실패하지 않게 만드는 prepare/commit 2단계 규약이 그 뼈대다.
 * 다섯째는 프로브다 — 능력 레지스터를 읽어 기능을 가리고, 큐와 표를 잡고,
 * 인터럽트를 걸고, iommu 코어에 등록한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치가 낸 DMA 주소가 물리 주소가 되기까지의 길:
 *
 *   장치 (스트림 id [+ PASID])
 *     → 스트림 표에서 STE 한 항목        ← 이 파일이 짓는다
 *     → 문맥 서술자 표에서 CD 한 항목    ← 이 파일이 짓는다
 *     → 1단계 페이지 테이블 (io-pgtable) → 중간 물리 주소
 *     → 2단계 페이지 테이블 (io-pgtable) → 실제 물리 주소
 *
 * 반대로 커널이 하드웨어에게 말을 거는 길은 명령 큐 하나뿐이다. 무효화도,
 * 설정 캐시 비우기도, 폴트 응답도 모두 그 큐에 명령을 넣어 전한다.
 * 하드웨어가 커널에게 말을 거는 길은 인터럽트 셋이다 — 이벤트 큐(폴트),
 * PRI 큐(페이지 요청), gerror(드라이버·하드웨어 사이의 규약 위반).
 * 실행 컨텍스트가 여러 겹이라는 점이 이 드라이버를 어렵게 만든다. 프로브와
 * 붙이기는 프로세스 문맥에서 잠들 수 있고, 무효화는 스핀락 아래 원자적
 * 문맥에서도 불리며, 이벤트·PRI 처리는 인터럽트 스레드, gerror 는 진짜
 * 인터럽트 문맥에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽으로는 iommu 코어(drivers/iommu/iommu.c)의 iommu_ops 를 구현한다.
 * DMA API 를 쓰는 평범한 드라이버는 iommu-dma 를 거쳐, 장치를 사용자
 * 공간에 넘기는 VFIO/iommufd 는 iommufd 를 거쳐 여기로 내려온다.
 * 아래쪽으로는 io-pgtable(ARM LPAE)이 실제 페이지 테이블을 짓는다. 이
 * 드라이버는 형식만 정하고, 만들어진 TTBR/VTTBR 값을 CD/STE 에 옮겨 담으며,
 * 테이블이 바뀌면 io-pgtable 의 tlb_flush_ops 콜백을 통해 다시 불려 온다.
 * 옆으로는 같은 디렉토리의 형제 파일들이 이 파일의 함수를 쓴다 —
 * arm-smmu-v3-sva.c(프로세스 주소 공간 공유), arm-smmu-v3-iommufd.c(게스트에게
 * SMMU 노출), tegra241-cmdqv.c(보조 명령 큐), arm-smmu-v3-test.c(단위 시험).
 * 확장 하드웨어는 impl_ops 갈고리표로 붙어, 본체 코드에 조건문을 뿌리지 않는다.
 * 펌웨어와는 ACPI IORT 또는 장치 트리로 만난다 — 어느 쪽이든 스트림 id 와
 * 인터럽트, MMIO 창을 알려 주고, 그 정보로 프로브가 시작된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - arm_smmu_cmdq_issue_cmdlist(): 락 없이 명령을 큐에 넣는 핵심 함수.
 *   valid_map 비트맵과 owner_prod 로 여러 CPU 를 조율한다.
 * - arm_smmu_write_entry(): STE/CD 를 하드웨어가 중간 상태를 보지 못하게
 *   나눠 쓴다. arm_smmu_get_ste_used()/get_cd_used() 가 그 계산의 재료다.
 * - arm_smmu_invs_merge()/unref()/purge(): 도메인의 무효화 대상 배열을
 *   합치고 걷고 정리한다. unref 는 실패할 수 없어야 해서 제자리에서 고친다.
 * - arm_smmu_domain_inv_range(): 그 배열을 훑어 무효화 명령을 한 묶음으로 낸다.
 * - arm_smmu_attach_prepare()/attach_commit(): 붙이기의 2단계 규약.
 * - arm_smmu_evtq_thread()/priq_thread(): 폴트와 페이지 요청을 처리한다.
 * - arm_smmu_device_probe(): 능력을 읽고 자원을 잡고 코어에 등록하는 진입점.
 */

#include <linux/acpi.h>	/* [한국어] ACPI 로 이 하드웨어를 찾는 경로. */
#include <linux/acpi_iort.h>	/* [한국어] IORT 표 — ACPI 시스템에서 스트림 id 와 SMMU 정보가 여기 적혀 있다. */
#include <linux/bitops.h>	/* [한국어] 비트맵과 비트 연산 — 명령 큐의 유효 비트맵이 이것을 쓴다. */
#include <linux/crash_dump.h>	/* [한국어] 크래시 커널에서는 앞선 커널이 남긴 DMA 가 살아 있어 초기화 방식을 달리해야 한다. */
#include <linux/delay.h>	/* [한국어] udelay — 큐가 찰 때 점점 늘려 가며 기다린다. */
#include <linux/err.h>	/* [한국어] ERR_PTR 계열. */
#include <linux/interrupt.h>	/* [한국어] 이벤트·PRI·gerror 인터럽트 등록. */
#include <linux/io-pgtable.h>	/* [한국어] 실제 페이지 테이블을 짓는 계층 — 이 드라이버는 형식만 정한다. */
#include <linux/iopoll.h>	/* [한국어] 레지스터가 바뀌기를 기다리는 폴링 헬퍼. */
#include <linux/module.h>	/* [한국어] 모듈 인자와 메타데이터. */
#include <linux/msi.h>	/* [한국어] MSI 로 인터럽트를 받고, 명령 완료도 MSI 쓰기로 알 수 있다. */
#include <linux/of.h>	/* [한국어] 장치 트리로 이 하드웨어를 찾는 경로. */
#include <linux/of_address.h>	/* [한국어] 장치 트리에서 MMIO 창 주소를 읽는다. */
#include <linux/of_platform.h>	/* [한국어] 자식 노드(보조 큐 확장 등)를 플랫폼 장치로 만든다. */
#include <linux/pci.h>	/* [한국어] PCI 장치의 별칭 처리와 ATS 제어. */
#include <linux/pci-ats.h>	/* [한국어] 장치 쪽 변환 캐시(ATS)를 켜고 끄는 API. */
#include <linux/platform_device.h>	/* [한국어] SMMU 자체가 플랫폼 장치로 나타난다. */
#include <linux/sort.h>	/* [한국어] 무효화 배열을 정렬해 두면 병합이 선형 시간에 끝난다. */
#include <linux/string_choices.h>	/* [한국어] str_enabled_disabled 같은 로그용 문자열 헬퍼. */
#include <kunit/visibility.h>	/* [한국어] 단위 시험에서만 static 함수를 열어 주는 매크로. */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자 공간과 주고받는 구조체 — 중첩 변환에 필요하다. */

#include "arm-smmu-v3.h"	/* [한국어] 이 드라이버의 하드웨어 규격과 자료 모델. */
#include "../../dma-iommu.h"	/* [한국어] 예약 구간(MSI 창 등)을 코어에 알리는 헬퍼. */

static bool disable_msipolling;	/* [한국어] 명령 완료를 MSI 쓰기로 감지하는 최적화를 끌지 정하는 인자. 그 방식이 미심쩍은 하드웨어에서 쓴다. */
module_param(disable_msipolling, bool, 0444);	/* [한국어] 부팅 인자로만 준다 — 프로브 때 기능 판단에 쓰이는 값이라 나중에 바꿔도 뜻이 없다. */
MODULE_PARM_DESC(disable_msipolling,	/* [한국어] modinfo 에 이 인자의 뜻을 남긴다. */
	"Disable MSI-based polling for CMD_SYNC completion.");	/* [한국어] modinfo 에 찍힐 설명. */

static const struct iommu_ops arm_smmu_ops;	/* [한국어] 코어에 등록할 연산표. 정의는 파일 끝에 있어 미리 알려 둔다. */
static struct iommu_dirty_ops arm_smmu_dirty_ops;	/* [한국어] 더티 페이지 추적 연산표. 마이그레이션에서 바뀐 페이지를 고를 때 쓴다. */

/* [한국어] MSI 를 쓰는 인터럽트 셋에 붙인 번호.
 *
 * 아래 arm_smmu_msi_cfg 배열의 첨자로 쓰이며, 마지막 값이 곧 개수다 —
 * 커널에서 흔한 관용구다. */
enum arm_smmu_msi_index {
	/* [한국어] 이벤트 큐(폴트 보고) 인터럽트.
	 * 설정자: MSI 를 잡을 때 이 첨자로 레지스터 묶음을 고른다.
	 * 읽는 자: arm_smmu_write_msi_msg().
	 * 값 범위: 0.
	 * 동기화: 상수다. */
	EVTQ_MSI_INDEX,
	/* [한국어] 전역 오류(gerror) 인터럽트 — 드라이버와 하드웨어 사이의 규약이
	 * 깨졌을 때 온다.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 1.
	 * 동기화: 상수다. */
	GERROR_MSI_INDEX,
	/* [한국어] 페이지 요청(PRI) 큐 인터럽트.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 2.
	 * 동기화: 상수다. */
	PRIQ_MSI_INDEX,
	/* [한국어] 개수를 뜻하는 마지막 값 — 배열 크기로 쓴다.
	 * 값 범위: 3.
	 * 동기화: 상수다. */
	ARM_SMMU_MAX_MSIS,
};

#define NUM_ENTRY_QWORDS 8	/* [한국어] STE 도 CD 도 8워드(64바이트)다. 두 항목을 같은 쓰기 규약으로 다룰 수 있는 근거다. */
static_assert(sizeof(struct arm_smmu_ste) == NUM_ENTRY_QWORDS * sizeof(u64));	/* [한국어] 구조체 정의가 규격과 어긋나면 빌드를 막는다 — 어긋나면 하드웨어가 엉뚱한 자리를 읽는다. */
static_assert(sizeof(struct arm_smmu_cd) == NUM_ENTRY_QWORDS * sizeof(u64));	/* [한국어] 문맥 서술자도 같은 검사. */

/* [한국어] 인터럽트 종류마다 MSI 주소·데이터·설정을 담는 레지스터 세 개의 오프셋.
 *
 * 세 인터럽트가 같은 모양의 레지스터 묶음을 갖고 있어, 표로 만들어 두면
 * MSI 를 거는 코드를 하나로 쓸 수 있다. */
static phys_addr_t arm_smmu_msi_cfg[ARM_SMMU_MAX_MSIS][3] = {
	[EVTQ_MSI_INDEX] = {	/* [한국어] 이벤트 큐 인터럽트의 레지스터 묶음. */
		ARM_SMMU_EVTQ_IRQ_CFG0,	/* [한국어] MSI 를 쓸 주소. */
		ARM_SMMU_EVTQ_IRQ_CFG1,	/* [한국어] 그때 쓸 데이터 값. */
		ARM_SMMU_EVTQ_IRQ_CFG2,	/* [한국어] 메모리 속성 등 부가 설정. */
	},
	[GERROR_MSI_INDEX] = {	/* [한국어] 전역 오류 인터럽트의 묶음. */
		ARM_SMMU_GERROR_IRQ_CFG0,	/* [한국어] 전역 오류 인터럽트의 MSI 주소 레지스터. */
		ARM_SMMU_GERROR_IRQ_CFG1,
		ARM_SMMU_GERROR_IRQ_CFG2,
	},
	[PRIQ_MSI_INDEX] = {	/* [한국어] 페이지 요청 큐 인터럽트의 묶음. */
		ARM_SMMU_PRIQ_IRQ_CFG0,	/* [한국어] 페이지 요청 큐 인터럽트의 MSI 주소 레지스터. */
		ARM_SMMU_PRIQ_IRQ_CFG1,
		ARM_SMMU_PRIQ_IRQ_CFG2,
	},
};

/* [한국어] 장치 트리 속성 이름과 그것이 켜는 우회 옵션을 잇는 표의 한 칸. */
struct arm_smmu_option_prop {
	/* [한국어] 그 속성이 켜는 옵션 비트.
	 * 설정자: 아래 표에 상수로 적혀 있다.
	 * 읽는 자: parse_driver_options() 가 smmu->options 에 더한다.
	 * 값 범위: ARM_SMMU_OPT_* 중 하나. 0 이면 표의 끝이다.
	 * 동기화: 읽기 전용 표다. */
	u32 opt;
	/* [한국어] 장치 트리에서 찾을 속성 이름.
	 * 설정자: 아래 표에 상수로 적혀 있다.
	 * 읽는 자: of_property_read_bool() 에 그대로 넘긴다.
	 * 값 범위: NULL 이면 표의 끝이다.
	 * 동기화: 읽기 전용 표다. */
	const char *prop;
};

DEFINE_XARRAY_ALLOC1(arm_smmu_asid_xa);	/* [한국어] 시스템 전체가 나눠 쓰는 ASID 풀. 1부터 배정한다(0은 예약). SVA 도메인과 보통 도메인이 같은 공간을 쓰므로 하나로 관리한다. */
DEFINE_MUTEX(arm_smmu_asid_lock);	/* [한국어] ASID 배정과 문맥 서술자 표 상태를 함께 지키는 전역 락. 붙이기 경로가 이 락 아래에서 돈다. */

/* [한국어] 알려진 하드웨어 결함과 그 우회를 켜는 장치 트리 속성 표.
 *
 * 규격을 벗어난 구현을 드라이버가 알아서 감지할 수는 없으므로, 펌웨어가
 * 속성으로 알려 주는 방식을 쓴다. 마지막 칸이 { 0, NULL } 인 것이 끝 표시다. */
static struct arm_smmu_option_prop arm_smmu_options[] = {
	{ ARM_SMMU_OPT_SKIP_PREFETCH, "hisilicon,broken-prefetch-cmd" },	/* [한국어] 설정 미리 읽기 명령이 망가진 하드웨어 — 그 명령을 아예 보내지 않는다. */
	{ ARM_SMMU_OPT_PAGE0_REGS_ONLY, "cavium,cn9900-broken-page1-regspace"},	/* [한국어] 두 번째 레지스터 페이지가 없는 하드웨어 — 모든 레지스터를 첫 페이지에서 찾는다. */
	{ 0, NULL},	/* [한국어] 표의 끝. 순회 조건이 이 값을 보고 멈춘다. */
};

/* [한국어] 이벤트 번호를 사람이 읽을 이름으로 바꾸는 표.
 *
 * 폴트 로그에 숫자만 찍히면 규격서를 뒤져야 하므로, 규격이 쓰는 이름을
 * 그대로 적어 두었다. C_ 로 시작하면 설정 오류, F_ 로 시작하면 변환 실패다. */
static const char * const event_str[] = {
	[EVT_ID_BAD_STREAMID_CONFIG] = "C_BAD_STREAMID",	/* [한국어] 스트림 번호가 표 밖을 가리켰다. */
	[EVT_ID_STE_FETCH_FAULT] = "F_STE_FETCH",	/* [한국어] 스트림 표 항목을 읽다 넘어졌다 — 표 자체가 잘못된 메모리에 있다. */
	[EVT_ID_BAD_STE_CONFIG] = "C_BAD_STE",	/* [한국어] 항목 내용이 규격에 맞지 않는다. */
	[EVT_ID_STREAM_DISABLED_FAULT] = "F_STREAM_DISABLED",	/* [한국어] 꺼 둔 스트림에서 트랜잭션이 왔다. */
	[EVT_ID_BAD_SUBSTREAMID_CONFIG] = "C_BAD_SUBSTREAMID",	/* [한국어] PASID 가 문맥 표 밖을 가리켰다. */
	[EVT_ID_CD_FETCH_FAULT] = "F_CD_FETCH",	/* [한국어] 문맥 서술자를 읽다 넘어졌다. */
	[EVT_ID_BAD_CD_CONFIG] = "C_BAD_CD",	/* [한국어] 서술자 내용이 규격에 맞지 않는다 — 서술자를 지우는 대신 "모두 폴트"로 바꾸는 이유가 이것이다. */
	[EVT_ID_TRANSLATION_FAULT] = "F_TRANSLATION",	/* [한국어] 매핑이 없다 — 가장 흔한 폴트이며 SVA 에서는 페이지를 채워 주면 된다. */
	[EVT_ID_ADDR_SIZE_FAULT] = "F_ADDR_SIZE",	/* [한국어] 주소가 설정된 폭을 넘었다. */
	[EVT_ID_ACCESS_FAULT] = "F_ACCESS",	/* [한국어] 접근 플래그가 꺼져 있다. */
	[EVT_ID_PERMISSION_FAULT] = "F_PERMISSION",	/* [한국어] 권한이 없다 — 쓰기 금지 페이지에 쓰려 한 경우 등. */
	[EVT_ID_VMS_FETCH_FAULT] = "F_VMS_FETCH",	/* [한국어] 가상 기계 구조를 읽다 넘어졌다. */
};

/* [한국어] 폴트가 어느 단계의 접근에서 났는지 알려 주는 표.
 *
 * 같은 "변환 실패"라도 서술자를 읽다 난 것인지, 표를 걷다 난 것인지,
 * 장치가 낸 주소 자체가 문제인지에 따라 원인이 전혀 다르다. */
static const char * const event_class_str[] = {
	[0] = "CD fetch",	/* [한국어] 문맥 서술자를 읽는 중 — 서술자 표의 주소나 내용이 의심스럽다. */
	[1] = "Stage 1 translation table fetch",	/* [한국어] 1단계 페이지 테이블을 걷는 중 — 테이블이 놓인 메모리가 의심스럽다. */
	[2] = "Input address caused fault",	/* [한국어] 장치가 낸 주소 자체가 문제 — 드라이버가 매핑하지 않은 주소를 쓴 경우다. */
	[3] = "Reserved",	/* [한국어] 규격이 아직 쓰지 않는 값. */
};

/* [한국어] 아래에서 서로를 부르는 두 함수를 미리 알려 둔다 — 정의는 훨씬 뒤에 있다. */
static int arm_smmu_alloc_cd_tables(struct arm_smmu_master *master);	/* [한국어] 장치의 문맥 서술자 표를 잡는다. */
static bool arm_smmu_ats_supported(struct arm_smmu_master *master);	/* [한국어] 그 장치가 ATS 를 쓸 수 있는지 판단한다. */

/*
 * [한국어]
 * parse_driver_options - 펌웨어가 알려 준 하드웨어 결함 우회를 켠다
 *
 * @smmu: 대상 SMMU.
 *
 * 규격을 벗어난 구현은 드라이버가 스스로 알아낼 수 없으므로, 펌웨어가
 * 장치 트리 속성으로 알려 준다. 그 속성이 있으면 해당 우회 옵션을 켜고
 * 로그에 남긴다 — 나중에 성능이나 동작이 이상할 때 이 로그가 단서가 된다.
 *
 * do-while 로 도는 것이 눈에 띈다. 표의 마지막 칸은 opt 가 0 이라 조건에서
 * 걸러지는데, 첫 칸을 반드시 한 번은 보게 하려고 뒤에서 조건을 검사한다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_dt_probe() → [이 함수] → of_property_read_bool()
 */
static void parse_driver_options(struct arm_smmu_device *smmu)
{
	int i = 0;	/* [한국어] 표 첨자. */

	do {	/* [한국어] 첫 칸은 조건을 보기 전에 반드시 한 번 검사한다. */
		if (of_property_read_bool(smmu->dev->of_node,	/* [한국어] 이 SMMU 노드에 그 속성이 있는가. */
						arm_smmu_options[i].prop)) {
			smmu->options |= arm_smmu_options[i].opt;	/* [한국어] 우회 옵션을 켠다. 이후 코드가 이 비트를 보고 동작을 바꾼다. */
			dev_notice(smmu->dev, "option %s\n",	/* [한국어] 결함 우회가 켜졌다는 사실은 눈에 띄어야 하므로 notice 로 남긴다. */
				arm_smmu_options[i].prop);
		}
	} while (arm_smmu_options[++i].opt);	/* [한국어] 다음 칸으로 옮기고, 끝 표시(opt==0)를 만나면 멈춘다. */
}

/* Low-level queue manipulation functions */
/*
 * [한국어]
 * queue_has_space - 큐에 명령 n 개를 넣을 자리가 있는가
 *
 * @q: 검사할 큐의 포인터 쌍.
 * @n: 넣으려는 항목 수.
 * @return: 자리가 있으면 참.
 *
 * (위 영어 주석 참고) 링 버퍼의 빈 자리 계산이다. 포인터가 한 바퀴를
 * 돌았는지(wrap 비트)에 따라 계산이 갈리는 것이 요점이다 — 두 포인터의
 * wrap 비트가 같으면 소비자가 생산자보다 뒤에 있는 평범한 상태이고,
 * 다르면 생산자가 이미 한 바퀴를 앞서 있는 상태다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static bool queue_has_space(struct arm_smmu_ll_queue *q, u32 n)
{
	u32 space, prod, cons;	/* [한국어] 빈 자리 수와 두 포인터의 첨자 부분. */

	prod = Q_IDX(q, q->prod);	/* [한국어] 생산 포인터에서 배열 첨자만 꺼낸다. */
	cons = Q_IDX(q, q->cons);	/* [한국어] 소비 포인터도 마찬가지. */

	if (Q_WRP(q, q->prod) == Q_WRP(q, q->cons))	/* [한국어] 두 포인터가 같은 바퀴에 있는가. */
		space = (1 << q->max_n_shift) - (prod - cons);	/* [한국어] 그렇다면 전체 크기에서 이미 쓰인 만큼을 뺀다. */
	else
		space = cons - prod;	/* [한국어] 생산자가 한 바퀴 앞서 있으면, 소비자를 따라잡기 전까지가 빈 자리다. */

	return space >= n;	/* [한국어] 요청한 만큼 들어가는지. */
}

/*
 * [한국어]
 * queue_full - 큐가 가득 찼는가
 *
 * @q: 검사할 큐의 포인터 쌍.
 * @return: 가득 찼으면 참.
 *
 * 첨자는 같은데 바퀴가 다르면 생산자가 소비자를 정확히 한 바퀴 앞선 것,
 * 곧 가득 찬 상태다. 이 판정을 위해 포인터에 첨자보다 한 비트 위에
 * "바퀴" 비트를 두는 것이며, 그 덕분에 "비었음"과 "가득 참"을 구분할 수 있다.
 *
 * 실행 컨텍스트: 큐를 다루는 곳 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_poll_until_not_full() 등 → [이 함수]
 */
static bool queue_full(struct arm_smmu_ll_queue *q)
{
	return Q_IDX(q, q->prod) == Q_IDX(q, q->cons) &&	/* [한국어] 첨자가 같고. */
	       Q_WRP(q, q->prod) != Q_WRP(q, q->cons);	/* [한국어] 바퀴가 다르면 정확히 한 바퀴 차이 — 가득 찼다. */
}

/*
 * [한국어]
 * queue_empty - 큐가 비었는가
 *
 * @q: 검사할 큐의 포인터 쌍.
 * @return: 비었으면 참.
 *
 * 첨자도 같고 바퀴도 같으면 아무것도 들어 있지 않다. 위 queue_full 과
 * 바퀴 비교만 뒤집힌 것이 전부다.
 *
 * 실행 컨텍스트: 큐를 소비하는 곳. 잠들지 않는다.
 *
 * 호출 체인:
 *   queue_remove_raw()/이벤트 처리 스레드 → [이 함수]
 */
static bool queue_empty(struct arm_smmu_ll_queue *q)
{
	return Q_IDX(q, q->prod) == Q_IDX(q, q->cons) &&	/* [한국어] 첨자가 같고. */
	       Q_WRP(q, q->prod) == Q_WRP(q, q->cons);	/* [한국어] 바퀴도 같으면 비었다. */
}

/*
 * [한국어]
 * queue_consumed - 그 자리까지 하드웨어가 처리했는가
 *
 * @q: 큐의 포인터 쌍.
 * @prod: 확인하고 싶은 자리(그 자리에 명령을 넣었던 생산 포인터 값).
 * @return: 그 자리가 이미 소비됐으면 참.
 *
 * 명령을 넣은 CPU 가 "내 명령이 처리됐는가"를 기다릴 때 쓴다. 링 버퍼라
 * 단순 비교로는 판정할 수 없어, 바퀴가 같으면 소비 첨자가 더 커야 하고
 * 바퀴가 다르면 소비 첨자가 작거나 같아야 한다는 두 경우로 나눈다.
 *
 * 실행 컨텍스트: 완료 대기 폴링. 잠들지 않는다.
 *
 * 호출 체인:
 *   __arm_smmu_cmdq_poll_until_consumed() → [이 함수]
 */
static bool queue_consumed(struct arm_smmu_ll_queue *q, u32 prod)
{
	return ((Q_WRP(q, q->cons) == Q_WRP(q, prod)) &&	/* [한국어] 같은 바퀴라면. */
		(Q_IDX(q, q->cons) > Q_IDX(q, prod))) ||	/* [한국어] 소비자가 그 자리를 지나쳐야 소비된 것이다. */
	       ((Q_WRP(q, q->cons) != Q_WRP(q, prod)) &&	/* [한국어] 다른 바퀴라면 소비자가 이미 한 바퀴 돌았다는 뜻이므로. */
		(Q_IDX(q, q->cons) <= Q_IDX(q, prod)));	/* [한국어] 첨자가 작거나 같아도 지나친 것이다. */
}

/*
 * [한국어]
 * queue_sync_cons_out - 소비 포인터를 하드웨어에 알린다
 *
 * @q: 대상 큐.
 *
 * 큐에서 항목을 꺼내 읽은 뒤, 그 자리를 하드웨어가 다시 써도 된다고
 * 알리는 일이다. 순서가 결정적으로 중요하다 — 포인터를 먼저 밀면
 * 하드웨어가 아직 우리가 읽는 중인 자리에 새 항목을 덮어쓸 수 있다.
 * 그래서 메모리 장벽으로 앞선 읽기가 모두 끝났음을 보장한 뒤에야 쓴다.
 *
 * 실행 컨텍스트: 큐를 소비하는 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   queue_remove_raw()/queue_sync_cons_ovf() → [이 함수]
 */
static void queue_sync_cons_out(struct arm_smmu_queue *q)
{
	/*
	 * Ensure that all CPU accesses (reads and writes) to the queue
	 * are complete before we update the cons pointer.
	 */
	/* [한국어] (위 영어 주석 참고) 이 장벽이 없으면 CPU 나 컴파일러가 순서를
	 * 바꿔, 아직 읽지 않은 자리를 "다 읽었다"고 알릴 수 있다. 그러면
	 * 하드웨어가 그 자리를 덮어써 우리가 쓰레기를 읽게 된다. */
	__iomb();	/* [한국어] 메모리 접근과 MMIO 쓰기 사이의 순서를 세운다. */
	writel_relaxed(q->llq.cons, q->cons_reg);	/* [한국어] 장벽을 이미 걸었으므로 쓰기 자체는 relaxed 로 충분하다. */
}

/*
 * [한국어]
 * queue_inc_cons - 소비 포인터를 한 칸 밀어 올린다
 *
 * @q: 큐의 포인터 쌍.
 *
 * 첨자와 바퀴 비트를 함께 하나 올린 뒤, 넘침 표시는 원래 값을 유지한다.
 * 첨자가 끝에 닿으면 자연스럽게 바퀴 비트로 올림이 일어나는 것이 이
 * 표현의 묘미다 — 별도 분기 없이 링 버퍼가 한 바퀴 돈다.
 *
 * 실행 컨텍스트: 큐를 소비하는 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   queue_remove_raw() → [이 함수]
 */
static void queue_inc_cons(struct arm_smmu_ll_queue *q)
{
	u32 cons = (Q_WRP(q, q->cons) | Q_IDX(q, q->cons)) + 1;	/* [한국어] 첨자와 바퀴만 남겨 하나 더하면, 끝에서 바퀴 비트로 자동 올림된다. */
	q->cons = Q_OVF(q->cons) | Q_WRP(q, cons) | Q_IDX(q, cons);	/* [한국어] 넘침 표시는 따로 관리하는 값이라 그대로 살려 둔다. */
}

/*
 * [한국어]
 * queue_sync_cons_ovf - 넘침 상태에서 벗어났음을 하드웨어에 알린다
 *
 * @q: 대상 큐.
 *
 * 이벤트나 페이지 요청이 소비 속도보다 빨리 쌓이면 하드웨어가 생산
 * 포인터의 넘침 비트를 세우고 새 항목 넣기를 멈춘다. 드라이버가 쌓인
 * 것을 다 처리한 뒤 소비 포인터의 넘침 비트를 생산 쪽과 맞추면, 그것이
 * "이제 다시 넣어도 된다"는 신호가 된다.
 *
 * 두 비트가 이미 같으면 넘침이 없었다는 뜻이라 아무 일도 하지 않는다 —
 * likely 로 표시된 흔한 경우다.
 *
 * 실행 컨텍스트: 이벤트·PRI 처리 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread()/priq_thread() → [이 함수]
 *     → queue_sync_cons_out()
 */
static void queue_sync_cons_ovf(struct arm_smmu_queue *q)
{
	struct arm_smmu_ll_queue *llq = &q->llq;	/* [한국어] 포인터 쌍만 따로 잡아 짧게 쓴다. */

	if (likely(Q_OVF(llq->prod) == Q_OVF(llq->cons)))	/* [한국어] 넘침이 없었던 흔한 경우. */
		return;	/* [한국어] 알릴 것이 없다. */

	llq->cons = Q_OVF(llq->prod) | Q_WRP(llq, llq->cons) |	/* [한국어] 넘침 비트만 생산 쪽에 맞추고, 첨자와 바퀴는 우리가 처리한 자리 그대로 둔다. */
		      Q_IDX(llq, llq->cons);
	queue_sync_cons_out(q);	/* [한국어] 하드웨어에 알려, 다시 항목을 넣기 시작하게 한다. */
}

/*
 * [한국어]
 * queue_sync_prod_in - 하드웨어가 어디까지 넣었는지 읽어 온다
 *
 * @q: 대상 큐.
 * @return: 0 정상, -EOVERFLOW 넘침이 일어났음.
 *
 * 이벤트·PRI 큐처럼 하드웨어가 생산자인 큐에서, 새 항목이 들어왔는지
 * 확인하는 함수다. 넘침 비트가 우리가 알던 것과 달라졌으면 그 사이에
 * 항목이 버려졌다는 뜻이라 호출자에게 알린다.
 *
 * relaxed 를 쓰지 않는 이유가 주석에 있다 — 생산 포인터를 읽기 전에
 * CPU 가 큐 내용을 미리 읽어 버리면, 아직 쓰이지 않은 자리를 읽게 된다.
 *
 * 실행 컨텍스트: 이벤트·PRI 처리 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread()/priq_thread() → [이 함수]
 */
static int queue_sync_prod_in(struct arm_smmu_queue *q)
{
	u32 prod;	/* [한국어] 하드웨어가 알려 준 생산 포인터. */
	int ret = 0;	/* [한국어] 기본은 정상. */

	/*
	 * We can't use the _relaxed() variant here, as we must prevent
	 * speculative reads of the queue before we have determined that
	 * prod has indeed moved.
	 */
	/* [한국어] (위 영어 주석 참고) relaxed 로 읽으면 CPU 가 큐 내용을 앞질러
	 * 읽을 수 있다. 그러면 하드웨어가 아직 쓰지 않은 자리를 읽어 쓰레기를
	 * 폴트 기록으로 해석하게 된다. */
	prod = readl(q->prod_reg);	/* [한국어] 순서를 세우는 판으로 읽는다. */

	if (Q_OVF(prod) != Q_OVF(q->llq.prod))	/* [한국어] 넘침 비트가 달라졌는가. */
		ret = -EOVERFLOW;	/* [한국어] 그 사이 항목이 버려졌다 — 호출자가 경고를 남기고 넘침을 푼다. */

	q->llq.prod = prod;	/* [한국어] 새 위치를 기억해 그만큼 소비한다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * queue_inc_prod_n - 생산 포인터를 n 칸 앞으로 옮긴 값을 계산한다
 *
 * @q: 큐의 포인터 쌍.
 * @n: 앞으로 옮길 칸 수.
 * @return: 옮겨진 포인터 값 (실제로 쓰지는 않는다).
 *
 * 값을 계산만 하고 저장하지 않는 것이 요점이다. 락 없는 명령 큐 삽입에서
 * 여러 CPU 가 원자 연산으로 자리를 다투므로, 계산과 저장이 분리되어야
 * 한다 — "내가 이만큼 가져가겠다"를 먼저 계산해 두고 원자적으로 겨룬다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static u32 queue_inc_prod_n(struct arm_smmu_ll_queue *q, int n)
{
	u32 prod = (Q_WRP(q, q->prod) | Q_IDX(q, q->prod)) + n;	/* [한국어] 첨자와 바퀴를 함께 더해 링을 자연스럽게 넘어가게 한다. */
	return Q_OVF(q->prod) | Q_WRP(q, prod) | Q_IDX(q, prod);	/* [한국어] 넘침 표시는 그대로 살려 돌려준다. */
}

/*
 * [한국어]
 * queue_poll_init - 기다리기 상태를 초기화한다
 *
 * @smmu: 그 하드웨어 (기다리는 방식을 정하는 데 쓴다).
 * @qp: 초기화할 폴링 상태.
 *
 * 큐가 차거나 명령이 끝나기를 기다릴 때, 무작정 도는 대신 단계적으로
 * 물러나는 전략을 쓴다. 그 전략의 상태를 여기서 초기화한다.
 *
 * 하드웨어가 SEV(이벤트로 깨우기)를 지원하면 WFE 로 잠깐 멈춰 전력을
 * 아낄 수 있어, 그 여부를 미리 기록해 둔다.
 *
 * 실행 컨텍스트: 기다리기 직전. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_poll_until_* → [이 함수] → ktime_get()
 */
static void queue_poll_init(struct arm_smmu_device *smmu,
			    struct arm_smmu_queue_poll *qp)
{
	qp->delay = 1;	/* [한국어] 물러나는 시간은 1마이크로초에서 시작해 두 배씩 늘어난다. */
	qp->spin_cnt = 0;	/* [한국어] 아직 한 번도 돌지 않았다. */
	qp->wfe = !!(smmu->features & ARM_SMMU_FEAT_SEV);	/* [한국어] 이벤트로 깨울 수 있으면 WFE 로 기다려 전력을 아낀다. */
	qp->timeout = ktime_add_us(ktime_get(), ARM_SMMU_POLL_TIMEOUT_US);	/* [한국어] 절대 시각으로 마감을 정해 둔다 — 상대 시간을 세면 오차가 쌓인다. */
}

/*
 * [한국어]
 * queue_poll - 한 번 기다리고 마감을 넘겼는지 알린다
 *
 * @qp: 폴링 상태.
 * @return: 0 더 기다려도 된다, -ETIMEDOUT 마감을 넘겼다.
 *
 * 세 단계로 물러난다. WFE 를 쓸 수 있으면 그것으로 멈추고, 아니면 처음
 * 얼마 동안은 cpu_relax() 로 바쁘게 돌다가, 그래도 안 되면 점점 긴
 * 시간을 실제로 쉰다. 짧게 끝날 기다림은 바쁜 대기가 빠르고, 길어지면
 * CPU 를 놓아 주는 편이 낫다는 절충이다.
 *
 * 실행 컨텍스트: 원자적 문맥일 수 있어 잠들지 않는다 — udelay 는 바쁜
 * 대기이지 수면이 아니다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_poll_until_* → [이 함수]
 */
static int queue_poll(struct arm_smmu_queue_poll *qp)
{
	if (ktime_compare(ktime_get(), qp->timeout) > 0)	/* [한국어] 마감을 넘겼는가. */
		return -ETIMEDOUT;	/* [한국어] 하드웨어가 멈춰 선 것으로 보고 호출자가 오류를 낸다. */

	if (qp->wfe) {	/* [한국어] 이벤트로 깨울 수 있는 하드웨어라면. */
		wfe();	/* [한국어] 이벤트가 올 때까지 코어를 멈춘다 — 가장 전력 효율이 좋다. */
	} else if (++qp->spin_cnt < ARM_SMMU_POLL_SPIN_COUNT) {	/* [한국어] 아직 초반이라면. */
		cpu_relax();	/* [한국어] 바쁘게 돌되 다른 하이퍼스레드에게 자원을 양보한다. */
	} else {	/* [한국어] 오래 걸리는 기다림으로 판명되면. */
		udelay(qp->delay);	/* [한국어] 실제로 쉬어 버스 트래픽을 줄인다. */
		qp->delay *= 2;	/* [한국어] 지수적으로 늘려, 아주 오래 걸릴 때 도는 횟수를 줄인다. */
		qp->spin_cnt = 0;	/* [한국어] 다음 단계를 위해 세기를 초기화한다. */
	}

	return 0;	/* [한국어] 아직 마감 전이니 호출자가 다시 조건을 확인한다. */
}

/*
 * [한국어]
 * queue_write - 명령 워드를 하드웨어 형식으로 큐에 쓴다
 *
 * @dst: 큐 안의 자리.
 * @src: 호스트 엔디안으로 지어 둔 명령.
 * @n_dwords: 워드 수.
 *
 * 하드웨어는 리틀엔디안으로 읽으므로, 빅엔디안 커널에서도 같은 비트가
 * 같은 뜻이 되도록 변환해 담는다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_write_entries() → [이 함수]
 */
static void queue_write(__le64 *dst, u64 *src, size_t n_dwords)
{
	int i;	/* [한국어] 워드 반복자. */

	for (i = 0; i < n_dwords; ++i)	/* [한국어] 명령 하나가 여러 워드다. */
		*dst++ = cpu_to_le64(*src++);	/* [한국어] 하드웨어 형식으로 바꿔 담는다. */
}

/*
 * [한국어]
 * queue_read - 큐에서 읽은 워드를 호스트 형식으로 바꾼다
 *
 * @dst: 담을 자리.
 * @src: 큐 안의 항목.
 * @n_dwords: 워드 수.
 *
 * queue_write 의 반대 방향. 이벤트 큐와 PRI 큐에서 하드웨어가 쓴 기록을
 * 읽을 때 쓴다.
 *
 * 실행 컨텍스트: 이벤트·PRI 처리 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   queue_remove_raw() → [이 함수]
 */
static void queue_read(u64 *dst, __le64 *src, size_t n_dwords)
{
	int i;	/* [한국어] 워드 반복자. */

	for (i = 0; i < n_dwords; ++i)	/* [한국어] 기록 하나가 여러 워드다. */
		*dst++ = le64_to_cpu(*src++);	/* [한국어] 호스트 엔디안으로 바꿔 담는다. */
}

/*
 * [한국어]
 * queue_remove_raw - 큐에서 항목 하나를 꺼낸다
 *
 * @q: 대상 큐.
 * @ent: 꺼낸 내용을 담을 자리.
 * @return: 0 꺼냈다, -EAGAIN 큐가 비었다.
 *
 * 읽고, 소비 포인터를 밀고, 하드웨어에 알리는 세 걸음을 묶었다. 순서가
 * 중요하다 — 내용을 다 읽은 뒤에야 그 자리를 놓아 줘야 한다.
 *
 * 실행 컨텍스트: 이벤트·PRI 처리 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread()/priq_thread() → [이 함수]
 *     → queue_read() → queue_inc_cons() → queue_sync_cons_out()
 */
static int queue_remove_raw(struct arm_smmu_queue *q, u64 *ent)
{
	if (queue_empty(&q->llq))	/* [한국어] 꺼낼 것이 없으면. */
		return -EAGAIN;	/* [한국어] 호출자가 생산 포인터를 다시 읽어 보게 한다. */

	queue_read(ent, Q_ENT(q, q->llq.cons), q->ent_dwords);	/* [한국어] 소비 포인터가 가리키는 자리를 호스트 형식으로 읽어 온다. */
	queue_inc_cons(&q->llq);	/* [한국어] 소프트웨어 쪽 포인터를 한 칸 민다. */
	queue_sync_cons_out(q);	/* [한국어] 장벽을 걸고 하드웨어에 알린다 — 이제 이 자리를 덮어써도 된다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/* High-level queue accessors */
/*
 * [한국어]
 * arm_smmu_cmdq_build_cmd - 명령 서술 구조를 하드웨어 워드 두 개로 짠다
 *
 * @cmd: 지어 담을 자리 (2워드).
 * @ent: 어떤 명령을 어떤 인자로 낼지 적어 둔 구조.
 * @return: 0 성공, -EINVAL 인자가 잘못됨, -ENOENT 모르는 명령.
 *
 * 이 드라이버가 하드웨어에게 말을 거는 모든 문장이 여기서 만들어진다.
 * 명령마다 인자가 놓이는 비트 자리가 다르므로 큰 switch 로 갈래를 나누며,
 * fallthrough 를 적극적으로 쓴 것이 눈에 띈다 — 예컨대 CFGI_CD 는
 * CFGI_STE 가 채우는 필드를 모두 쓰고 PASID 하나만 더 채우므로,
 * 자기 몫만 채운 뒤 아래로 흘려보낸다.
 *
 * 처음에 통째로 0 으로 지우는 것이 중요하다. 명령 버퍼는 재사용되는
 * 스택 배열이라, 앞 명령의 찌꺼기가 남으면 하드웨어가 엉뚱한 인자로 읽는다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 원자적 문맥일 수 있어 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_batch_add()/issue_cmd() 등 → [이 함수]
 */
static int arm_smmu_cmdq_build_cmd(u64 *cmd, struct arm_smmu_cmdq_ent *ent)
{
	memset(cmd, 0, 1 << CMDQ_ENT_SZ_SHIFT);	/* [한국어] 앞 명령의 찌꺼기를 지운다 — 아래는 모두 |= 로만 채우기 때문이다. */
	cmd[0] |= FIELD_PREP(CMDQ_0_OP, ent->opcode);	/* [한국어] 명령 종류는 모든 명령이 같은 자리에 담는다. */

	switch (ent->opcode) {	/* [한국어] 명령마다 인자가 놓이는 자리가 달라 갈래를 나눈다. */
	case CMDQ_OP_TLBI_EL2_ALL:	/* [한국어] EL2 TLB 전체 비우기. */
	case CMDQ_OP_TLBI_NSNH_ALL:	/* [한국어] 비보안·비하이퍼바이저 전체 비우기. */
		break;	/* [한국어] 둘 다 인자가 없다 — 종류만으로 뜻이 완결된다. */
	case CMDQ_OP_PREFETCH_CFG:	/* [한국어] 설정 미리 읽기. */
		cmd[0] |= FIELD_PREP(CMDQ_PREFETCH_0_SID, ent->prefetch.sid);	/* [한국어] 어느 스트림의 설정을 미리 읽을지. */
		break;
	case CMDQ_OP_CFGI_CD:	/* [한국어] 문맥 서술자 하나 무효화. */
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SSID, ent->cfgi.ssid);	/* [한국어] 어느 PASID 의 서술자인지 — 이 명령만의 추가 인자다. */
		fallthrough;	/* [한국어] 나머지 필드는 STE 무효화와 같으므로 아래로 흘려보낸다. */
	case CMDQ_OP_CFGI_STE:	/* [한국어] 스트림 표 항목 하나 무효화. */
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);	/* [한국어] 대상 스트림 번호. */
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_LEAF, ent->cfgi.leaf);	/* [한국어] 마지막 단계만 버릴지, 걸어온 경로까지 버릴지. */
		break;
	case CMDQ_OP_CFGI_CD_ALL:	/* [한국어] 한 스트림의 서술자 전부 무효화. */
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);	/* [한국어] 스트림 번호만 있으면 된다 — PASID 를 가리지 않기 때문이다. */
		break;
	case CMDQ_OP_CFGI_ALL:	/* [한국어] 모든 설정 캐시 무효화. */
		/* Cover the entire SID range */
		/* [한국어] (위 영어 주석 참고) 범위를 2^31 로 주어 스트림 번호 공간
		 * 전체를 덮는다 — "전부"를 표현하는 규격의 관용구다. */
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_RANGE, 31);	/* [한국어] 표현 가능한 최대 범위. */
		break;
	case CMDQ_OP_TLBI_NH_VA:	/* [한국어] VMID 와 ASID, 주소로 1단계 TLB 비우기. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);	/* [한국어] 중첩 변환에서는 VMID 로도 걸러야 한다. */
		fallthrough;	/* [한국어] 나머지 주소 관련 필드는 EL2 판과 같다. */
	case CMDQ_OP_TLBI_EL2_VA:	/* [한국어] EL2 의 주소 기반 비우기. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_NUM, ent->tlbi.num);	/* [한국어] 범위 무효화에서 한 번에 지울 묶음 수. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_SCALE, ent->tlbi.scale);	/* [한국어] 그 묶음에 곱할 배율. num 과 scale 이 함께 범위를 정한다. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);	/* [한국어] 어느 주소 공간인지. */
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);	/* [한국어] 마지막 단계만 버릴지. */
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TTL, ent->tlbi.ttl);	/* [한국어] 어느 표 단계를 겨냥할지 — 좁힐수록 무효화가 싸다. */
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TG, ent->tlbi.tg);	/* [한국어] 알갱이 크기 — 범위 계산의 단위다. */
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_VA_MASK;	/* [한국어] 주소는 자리가 맞아떨어져 마스크만 씌워 넣는다. */
		break;
	case CMDQ_OP_TLBI_S2_IPA:	/* [한국어] 2단계 변환을 중간 물리 주소로 비우기. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_NUM, ent->tlbi.num);	/* [한국어] 범위 무효화 인자. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_SCALE, ent->tlbi.scale);	/* [한국어] num 에 곱할 배율 — 둘이 함께 범위를 정한다. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);	/* [한국어] 2단계는 ASID 가 아니라 VMID 로 태그된다. */
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);	/* [한국어] 마지막 단계만 버릴지. */
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TTL, ent->tlbi.ttl);	/* [한국어] 어느 표 단계를 겨냥할지 — 좁힐수록 무효화가 싸다. */
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TG, ent->tlbi.tg);	/* [한국어] 알갱이 크기 — 범위 계산의 단위다. */
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_IPA_MASK;	/* [한국어] 중간 물리 주소는 마스크가 다르다 — 표현 범위가 가상 주소와 다르기 때문이다. */
		break;
	case CMDQ_OP_TLBI_NH_ASID:	/* [한국어] ASID 하나를 통째로 비우기. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);	/* [한국어] 그 ASID. */
		fallthrough;	/* [한국어] VMID 는 아래에서 함께 채운다. */
	case CMDQ_OP_TLBI_NH_ALL:	/* [한국어] 그 VMID 아래 1단계 전부. */
	case CMDQ_OP_TLBI_S12_VMALL:	/* [한국어] 그 VMID 의 1·2단계 전부. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);	/* [한국어] 셋 다 VMID 로 범위를 가둔다 — 게스트끼리 서로의 TLB 를 못 지우게 하는 근거다. */
		break;
	case CMDQ_OP_TLBI_EL2_ASID:	/* [한국어] EL2 의 ASID 하나 비우기. */
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);	/* [한국어] EL2 에는 VMID 개념이 없어 ASID 만 있으면 된다. */
		break;
	case CMDQ_OP_ATC_INV:	/* [한국어] 장치 쪽 변환 캐시 무효화 — 이 명령만 장치까지 왕복한다. */
		cmd[0] |= FIELD_PREP(CMDQ_0_SSV, ent->substream_valid);	/* [한국어] PASID 필드가 유효한지 알린다. */
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_GLOBAL, ent->atc.global);	/* [한국어] 참이면 PASID 를 가리지 않고 그 장치 전체를 비운다. */
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_SSID, ent->atc.ssid);	/* [한국어] PASID 별 무효화일 때의 대상. */
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_SID, ent->atc.sid);	/* [한국어] 어느 장치인지. */
		cmd[1] |= FIELD_PREP(CMDQ_ATC_1_SIZE, ent->atc.size);	/* [한국어] 비울 구간의 크기(2의 지수). */
		cmd[1] |= ent->atc.addr & CMDQ_ATC_1_ADDR_MASK;	/* [한국어] 그 구간의 시작 주소. */
		break;
	case CMDQ_OP_PRI_RESP:	/* [한국어] 페이지 요청에 대한 응답. */
		cmd[0] |= FIELD_PREP(CMDQ_0_SSV, ent->substream_valid);	/* [한국어] 요청에 PASID 가 실려 있었는지. */
		cmd[0] |= FIELD_PREP(CMDQ_PRI_0_SSID, ent->pri.ssid);	/* [한국어] 그 PASID. */
		cmd[0] |= FIELD_PREP(CMDQ_PRI_0_SID, ent->pri.sid);	/* [한국어] 요청을 보낸 장치. */
		cmd[1] |= FIELD_PREP(CMDQ_PRI_1_GRPID, ent->pri.grpid);	/* [한국어] 그 요청 그룹 — 이 값으로 짝을 맞춘다. */
		switch (ent->pri.resp) {	/* [한국어] 응답 값은 규격이 정한 셋뿐이다. */
		case PRI_RESP_DENY:	/* [한국어] 거부. */
		case PRI_RESP_FAIL:	/* [한국어] 실패 — 장치가 더 이상 요청하지 않게 만든다. */
		case PRI_RESP_SUCC:	/* [한국어] 성공 — 페이지를 채웠으니 다시 시도하라. */
			break;	/* [한국어] 셋 중 하나면 통과. */
		default:	/* [한국어] 그 밖의 값 — 아래에서 함께 처리한다. */
			return -EINVAL;	/* [한국어] 그 밖의 값을 하드웨어에 보내면 규약 위반이므로 여기서 막는다. */
		}
		cmd[1] |= FIELD_PREP(CMDQ_PRI_1_RESP, ent->pri.resp);	/* [한국어] 검사를 통과한 응답을 담는다. */
		break;
	case CMDQ_OP_RESUME:	/* [한국어] 멈춰 선 트랜잭션을 다시 굴리거나 버린다. */
		cmd[0] |= FIELD_PREP(CMDQ_RESUME_0_SID, ent->resume.sid);	/* [한국어] 어느 장치의 트랜잭션인지. */
		cmd[0] |= FIELD_PREP(CMDQ_RESUME_0_RESP, ent->resume.resp);	/* [한국어] 재시도할지 중단할지. */
		cmd[1] |= FIELD_PREP(CMDQ_RESUME_1_STAG, ent->resume.stag);	/* [한국어] 멈춤 꼬리표 — 이 값이 트랜잭션의 신원이다. */
		break;
	case CMDQ_OP_CMD_SYNC:	/* [한국어] 앞선 명령이 모두 끝나기를 기다린다 — 무효화 뒤에 반드시 붙는다. */
		if (ent->sync.msiaddr) {	/* [한국어] 완료를 MSI 쓰기로 알리게 할 것인가. */
			cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_IRQ);	/* [한국어] 완료 신호 방식을 MSI 로 정한다. */
			cmd[1] |= ent->sync.msiaddr & CMDQ_SYNC_1_MSIADDR_MASK;	/* [한국어] 그 값을 쓸 자리 — 큐 안의 그 명령 자리를 그대로 쓴다. */
		} else {	/* [한국어] MSI 를 못 쓰는 하드웨어라면. */
			cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);	/* [한국어] 이벤트로 깨우게 한다 — 기다리는 CPU 가 WFE 에서 풀린다. */
		}
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_MSH, ARM_SMMU_SH_ISH);	/* [한국어] MSI 쓰기의 공유 영역 — CPU 가 그 값을 볼 수 있어야 한다. */
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_MSIATTR, ARM_SMMU_MEMATTR_OIWB);	/* [한국어] 캐시 가능 쓰기로 지정해, CPU 가 캐시를 거쳐 값을 보게 한다. */
		break;
	default:	/* [한국어] 그 밖의 값 — 아래에서 함께 처리한다. */
		return -ENOENT;	/* [한국어] 드라이버가 모르는 명령 — 코드 버그이므로 만들지 않고 오류를 낸다. */
	}

	return 0;	/* [한국어] 명령 두 워드가 완성됐다. */
}

/*
 * [한국어]
 * arm_smmu_get_cmdq - 이번 명령을 넣을 큐를 고른다
 *
 * @smmu: 대상 하드웨어.
 * @ent: 넣으려는 명령.
 * @return: 쓸 명령 큐 (언제나 유효한 포인터).
 *
 * 확장 하드웨어가 보조 큐를 제공하면 그쪽에 물어보고, 없거나 이번 명령을
 * 받지 못한다고 하면 기본 큐로 돌아간다. 본체 코드에 확장 조건문을 뿌리지
 * 않으려고 갈고리 하나로 감싼 것이다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_batch_init()/issue_cmd() → [이 함수]
 *     → impl_ops->get_secondary_cmdq
 */
static struct arm_smmu_cmdq *arm_smmu_get_cmdq(struct arm_smmu_device *smmu,
					       struct arm_smmu_cmdq_ent *ent)
{
	struct arm_smmu_cmdq *cmdq = NULL;	/* [한국어] 확장이 골라 준 큐 — 없으면 NULL 로 남는다. */

	if (smmu->impl_ops && smmu->impl_ops->get_secondary_cmdq)	/* [한국어] 확장이 있고 그 갈고리를 제공한다면. */
		cmdq = smmu->impl_ops->get_secondary_cmdq(smmu, ent);	/* [한국어] 이번 명령에 알맞은 큐를 고르게 한다. */

	return cmdq ?: &smmu->cmdq;	/* [한국어] 확장이 NULL 을 주면 기본 큐로 돌아간다 — 언제나 유효한 큐가 나온다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_needs_busy_polling - 이 큐는 완료를 바쁘게 기다려야 하는가
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 검사할 큐.
 * @return: 바쁜 대기가 필요하면 참.
 *
 * 기본 큐는 MSI 나 이벤트로 완료를 알 수 있지만, Tegra 의 보조 큐는 그
 * 신호 방식을 지원하지 않는다. 그래서 그 큐에 넣은 CMD_SYNC 는 완료 신호
 * 없이(CS_NONE) 만들고, 소비 포인터를 직접 지켜보며 기다려야 한다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_build_sync_cmd()/poll_until_sync() → [이 함수]
 */
static bool arm_smmu_cmdq_needs_busy_polling(struct arm_smmu_device *smmu,
					     struct arm_smmu_cmdq *cmdq)
{
	if (cmdq == &smmu->cmdq)	/* [한국어] 기본 큐라면. */
		return false;	/* [한국어] 평소의 완료 신호를 그대로 쓸 수 있다. */

	return smmu->options & ARM_SMMU_OPT_TEGRA241_CMDQV;	/* [한국어] 보조 큐이면서 Tegra 확장이라면 바쁜 대기가 필요하다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_build_sync_cmd - 완료 대기 명령을 짓는다
 *
 * @cmd: 지어 담을 자리 (2워드).
 * @smmu: 대상 하드웨어.
 * @cmdq: 이 명령이 들어갈 큐.
 * @prod: 이 명령이 놓일 자리의 생산 포인터 값.
 *
 * MSI 방식을 쓸 때 완료 표시를 쓸 주소로 "그 명령 자신이 놓인 자리"를
 * 준다는 점이 흥미롭다. 하드웨어가 그 자리에 값을 써 버리면 명령이
 * 덮이지만, 이미 처리가 끝난 명령이므로 상관없고, 기다리는 CPU 는 그
 * 자리를 지켜보다 값이 바뀌는 것을 보고 완료를 안다.
 *
 * 주석이 경고하는 Hi16xx 문제도 그래서 해가 없다 — MSI 쓰기가 32비트를
 * 더 써서 명령 전체가 0 이 되어도, 그 자리는 이미 소비된 뒤다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수] → arm_smmu_cmdq_build_cmd()
 */
static void arm_smmu_cmdq_build_sync_cmd(u64 *cmd, struct arm_smmu_device *smmu,
					 struct arm_smmu_cmdq *cmdq, u32 prod)
{
	struct arm_smmu_queue *q = &cmdq->q;	/* [한국어] 링 버퍼 정보 — 명령 자리의 주소를 계산해야 한다. */
	struct arm_smmu_cmdq_ent ent = {	/* [한국어] 명령 서술 구조를 스택에 짓는다. */
		.opcode = CMDQ_OP_CMD_SYNC,	/* [한국어] 완료 대기 명령. */
	};

	/*
	 * Beware that Hi16xx adds an extra 32 bits of goodness to its MSI
	 * payload, so the write will zero the entire command on that platform.
	 */
	/* [한국어] (위 영어 주석 참고) 어떤 하드웨어는 MSI 쓰기가 예상보다 넓어
	 * 명령 두 워드를 통째로 0 으로 만든다. 그래도 문제가 없는 이유는, 그
	 * 시점에 그 명령은 이미 처리가 끝나 하드웨어가 다시 읽지 않기 때문이다. */
	if (smmu->options & ARM_SMMU_OPT_MSIPOLL) {	/* [한국어] MSI 로 완료를 감지할 수 있는 하드웨어라면. */
		ent.sync.msiaddr = q->base_dma + Q_IDX(&q->llq, prod) *	/* [한국어] 이 명령이 놓일 자리의 물리 주소를 계산한다. */
				   q->ent_dwords * 8;	/* [한국어] 항목 하나가 몇 바이트인지 곱한다. */
	}

	arm_smmu_cmdq_build_cmd(cmd, &ent);	/* [한국어] 서술 구조를 실제 워드로 짠다. */
	if (arm_smmu_cmdq_needs_busy_polling(smmu, cmdq))	/* [한국어] 완료 신호를 못 쓰는 큐라면. */
		u64p_replace_bits(cmd, CMDQ_SYNC_0_CS_NONE, CMDQ_SYNC_0_CS);	/* [한국어] 신호 방식을 "없음"으로 바꿔 끼운다 — 기다리는 쪽이 포인터를 직접 지켜본다. */
}

/*
 * [한국어]
 * __arm_smmu_cmdq_skip_err - 하드웨어가 걸려 넘어진 명령을 건너뛴다
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 오류가 난 명령 큐.
 *
 * 하드웨어가 해석할 수 없는 명령을 만나면 그 자리에서 멈춰 선다. 그대로
 * 두면 뒤에 쌓인 무효화가 하나도 처리되지 않아 시스템이 굳으므로, 그
 * 자리를 아무 일도 하지 않는 CMD_SYNC 로 덮어써서 큐를 다시 굴린다.
 * 명령 하나를 잃는 대신 시스템을 살리는 선택이며, 잃어버린 것이 무효화라면
 * 그 자체로 위험할 수 있어 오류 로그를 상세히 남긴다.
 *
 * 오류 종류에 따라 손대지 않고 돌아가는 갈래가 셋 있다 — 명령을 읽다
 * 실패한 경우(하드웨어가 스스로 다시 읽는다), 오류가 없는 경우, 그리고
 * ATC 무효화 시간 초과(그 자리는 CMD_SYNC 이지 잘못된 명령이 아니다).
 *
 * 큐의 소프트웨어 쪽 상태(shadow state)를 건드리지 않는 것이 중요하다 —
 * 다른 CPU 가 동시에 명령을 넣고 있을 수 있으므로, 하드웨어 레지스터가
 * 알려 준 자리만 직접 고친다.
 *
 * 실행 컨텍스트: gerror 인터럽트 처리기. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_gerror_handler() → arm_smmu_cmdq_skip_err() → [이 함수]
 */
void __arm_smmu_cmdq_skip_err(struct arm_smmu_device *smmu,
			      struct arm_smmu_cmdq *cmdq)
{
	static const char * const cerror_str[] = {	/* [한국어] 오류 코드를 사람이 읽을 이름으로 바꾸는 표. */
		[CMDQ_ERR_CERROR_NONE_IDX]	= "No error",	/* [한국어] 오류가 아니다 — 인터럽트가 다른 이유로 왔다. */
		[CMDQ_ERR_CERROR_ILL_IDX]	= "Illegal command",	/* [한국어] 해석할 수 없는 명령 — 드라이버 버그일 가능성이 높다. */
		[CMDQ_ERR_CERROR_ABT_IDX]	= "Abort on command fetch",	/* [한국어] 큐 메모리를 읽다 실패 — 링 버퍼 자체가 문제다. */
		[CMDQ_ERR_CERROR_ATC_INV_IDX]	= "ATC invalidate timeout",	/* [한국어] 장치가 무효화 응답을 돌려주지 않았다 — 장치 쪽 문제다. */
	};
	struct arm_smmu_queue *q = &cmdq->q;	/* [한국어] 링 버퍼 정보. */

	int i;	/* [한국어] 명령 워드 반복자. */
	u64 cmd[CMDQ_ENT_DWORDS];	/* [한국어] 문제의 명령을 읽어 담을 자리. */
	u32 cons = readl_relaxed(q->cons_reg);	/* [한국어] 하드웨어가 멈춰 선 자리 — 소프트웨어 쪽 값이 아니라 레지스터를 직접 읽는다. */
	u32 idx = FIELD_GET(CMDQ_CONS_ERR, cons);	/* [한국어] 소비 포인터에 얹혀 온 오류 코드. */
	struct arm_smmu_cmdq_ent cmd_sync = {	/* [한국어] 덮어쓸 안전한 명령. */
		.opcode = CMDQ_OP_CMD_SYNC,	/* [한국어] 아무 상태도 바꾸지 않으면서 큐를 진행시킨다. */
	};

	dev_err(smmu->dev, "CMDQ error (cons 0x%08x): %s\n", cons,	/* [한국어] 어디서 무슨 오류가 났는지 남긴다 — 무효화를 잃었을 수 있어 반드시 눈에 띄어야 한다. */
		idx < ARRAY_SIZE(cerror_str) ?  cerror_str[idx] : "Unknown");	/* [한국어] 표 밖의 코드도 대비한다. */

	switch (idx) {	/* [한국어] 오류 종류에 따라 손댈지 정한다. */
	case CMDQ_ERR_CERROR_ABT_IDX:	/* [한국어] 명령을 읽다 실패한 경우. */
		dev_err(smmu->dev, "retrying command fetch\n");	/* [한국어] 하드웨어가 스스로 다시 읽으므로 우리가 할 일은 없다. */
		return;
	case CMDQ_ERR_CERROR_NONE_IDX:	/* [한국어] 오류가 아니다. */
		return;	/* [한국어] 건드릴 것이 없다. */
	case CMDQ_ERR_CERROR_ATC_INV_IDX:	/* [한국어] 장치 캐시 무효화가 시간 초과된 경우. */
		/*
		 * ATC Invalidation Completion timeout. CONS is still pointing
		 * at the CMD_SYNC. Attempt to complete other pending commands
		 * by repeating the CMD_SYNC, though we might well end up back
		 * here since the ATC invalidation may still be pending.
		 */
		/* [한국어] (위 영어 주석 참고) 이때 멈춰 선 자리는 이미 CMD_SYNC 이므로
		 * 덮어쓸 필요가 없다. 하드웨어가 그 명령을 다시 시도하게 두는데,
		 * 장치가 계속 응답하지 않으면 같은 오류로 다시 여기 오게 된다 —
		 * 그 자체가 장치가 고장 났다는 신호다. */
		return;
	case CMDQ_ERR_CERROR_ILL_IDX:	/* [한국어] 해석할 수 없는 명령. */
	default:	/* [한국어] 모르는 코드도 같은 방식으로 다룬다. */
		break;	/* [한국어] 아래에서 그 자리를 덮어쓴다. */
	}

	/*
	 * We may have concurrent producers, so we need to be careful
	 * not to touch any of the shadow cmdq state.
	 */
	/* [한국어] (위 영어 주석 참고) 다른 CPU 가 지금도 명령을 넣고 있을 수 있다.
	 * 소프트웨어 쪽 포인터를 건드리면 그 CPU 들의 계산이 어긋나므로,
	 * 하드웨어가 알려 준 자리만 직접 읽고 쓴다. */
	queue_read(cmd, Q_ENT(q, cons), q->ent_dwords);	/* [한국어] 문제의 명령을 읽어 온다 — 로그에 남기기 위해서다. */
	dev_err(smmu->dev, "skipping command in error state:\n");	/* [한국어] 어떤 명령이 버려지는지 알려야 원인을 찾을 수 있다. */
	for (i = 0; i < ARRAY_SIZE(cmd); ++i)	/* [한국어] 명령 워드를 하나씩. */
		dev_err(smmu->dev, "\t0x%016llx\n", (unsigned long long)cmd[i]);	/* [한국어] 16진수로 그대로 찍는다 — 규격서와 대조할 수 있게. */

	/* Convert the erroneous command into a CMD_SYNC */
	/* [한국어] (위 영어 주석 참고) 잘못된 명령을 아무 일도 하지 않는 명령으로
	 * 바꿔치기한다. 그러면 하드웨어가 그것을 처리하고 다음으로 넘어간다. */
	arm_smmu_cmdq_build_cmd(cmd, &cmd_sync);	/* [한국어] 안전한 명령을 짓는다. */
	if (arm_smmu_cmdq_needs_busy_polling(smmu, cmdq))	/* [한국어] 완료 신호를 못 쓰는 큐라면. */
		u64p_replace_bits(cmd, CMDQ_SYNC_0_CS_NONE, CMDQ_SYNC_0_CS);	/* [한국어] 신호 방식을 "없음"으로 바꾼다 — 아무도 이 완료를 기다리지 않는다. */

	queue_write(Q_ENT(q, cons), cmd, q->ent_dwords);	/* [한국어] 그 자리에 덮어쓴다. 하드웨어가 다시 읽으면 이제 정상 명령을 보게 된다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_skip_err - 기본 명령 큐의 오류를 건너뛴다
 *
 * @smmu: 대상 하드웨어.
 *
 * 위 함수에 기본 큐를 넣어 부르는 얇은 껍데기다. 보조 큐를 가진 하드웨어는
 * 자기 인터럽트 처리기에서 __arm_smmu_cmdq_skip_err() 를 직접 부른다.
 *
 * 실행 컨텍스트: gerror 인터럽트 처리기. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_gerror_handler() → [이 함수] → __arm_smmu_cmdq_skip_err()
 */
static void arm_smmu_cmdq_skip_err(struct arm_smmu_device *smmu)
{
	__arm_smmu_cmdq_skip_err(smmu, &smmu->cmdq);	/* [한국어] 기본 큐를 지목해 넘긴다. */
}

/*
 * Command queue locking.
 * This is a form of bastardised rwlock with the following major changes:
 *
 * - The only LOCK routines are exclusive_trylock() and shared_lock().
 *   Neither have barrier semantics, and instead provide only a control
 *   dependency.
 *
 * - The UNLOCK routines are supplemented with shared_tryunlock(), which
 *   fails if the caller appears to be the last lock holder (yes, this is
 *   racy). All successful UNLOCK routines have RELEASE semantics.
 */
/* [한국어] (위 영어 주석 참고) 명령 큐 전용으로 변형한 읽기/쓰기 락.
 *
 * 왜 표준 rwlock 이 아니라 이런 물건이 필요한가. 큐가 가득 찼을 때 자리를
 * 비우려면 하드웨어의 소비 포인터를 읽어 소프트웨어 쪽 값을 갱신해야 하는데,
 * 그 갱신은 한 번에 한 CPU 만 해야 한다(배타). 반면 평소에 명령을 넣는
 * 일은 여러 CPU 가 동시에 해도 된다(공유). 그런데 표준 rwlock 은 장벽을
 * 걸어 비싸고, 여기서는 장벽 대신 제어 의존만 있으면 충분하다.
 *
 * 락 값이 음수(INT_MIN)면 배타 소유라는 표현이 핵심이다. 공유 잠그기는
 * 무조건 하나를 더하는데, 배타 상태에서는 값이 여전히 음수로 남아
 * 대기해야 함을 알 수 있고, 동시에 "기다리는 공유 요청이 있다"는 신호도 된다. */
/*
 * [한국어]
 * arm_smmu_cmdq_shared_lock - 명령 큐 락을 공유 모드로 잡는다
 *
 * @cmdq: 대상 큐.
 *
 * 흔한 경로는 원자적 증가 한 번으로 끝난다 — 값이 음수가 아니었다면
 * 아무도 배타 소유가 아니라는 뜻이므로 그대로 진행한다.
 * 음수였다면 누군가 자리를 비우는 중이므로, 그 일이 끝나 값이 양수가 될
 * 때까지 기다린다. 이미 값을 올려 두었으므로 그동안 새로운 배타 잠그기는
 * 성립하지 않는다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static void arm_smmu_cmdq_shared_lock(struct arm_smmu_cmdq *cmdq)
{
	/*
	 * When held in exclusive state, the lock counter is set to INT_MIN
	 * so these increments won't hurt as the value will remain negative.
	 * The increment will also signal the exclusive locker that there are
	 * shared waiters.
	 */
	/* [한국어] (위 영어 주석 참고) 배타 소유일 때 값은 INT_MIN 이므로, 우리가
	 * 하나 더해도 여전히 음수다 — 곧 "아직 못 잡았다"가 그대로 드러난다.
	 * 동시에 그 증가가 배타 소유자에게 "기다리는 사람이 있다"를 알린다. */
	if (atomic_fetch_inc_relaxed(&cmdq->lock) >= 0)	/* [한국어] 올리기 전 값이 음수가 아니었다면 아무도 배타 소유가 아니다. */
		return;	/* [한국어] 흔한 경로 — 원자 연산 하나로 끝난다. */

	/*
	 * Someone else is holding the lock in exclusive state, so wait
	 * for them to finish. Since we already incremented the lock counter,
	 * no exclusive lock can be acquired until we finish. We don't need
	 * the return value since we only care that the exclusive lock is
	 * released (i.e. the lock counter is non-negative).
	 * Once the exclusive locker releases the lock, the sign bit will
	 * be cleared and our increment will make the lock counter positive,
	 * allowing us to proceed.
	 */
	/* [한국어] (위 영어 주석 참고) 배타 소유자가 부호 비트만 내리면, 우리가
	 * 이미 더해 둔 값 덕분에 카운터가 양수가 되어 우리가 풀려난다.
	 * 배타 잠그기는 카운터가 정확히 0 일 때만 성립하므로, 우리가 값을
	 * 올려 둔 동안에는 새로운 배타 소유가 끼어들 수 없다. */
	atomic_cond_read_relaxed(&cmdq->lock, VAL > 0);	/* [한국어] 값이 양수가 될 때까지 기다린다 — 아키텍처가 지원하면 저전력 대기를 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_shared_unlock - 공유 모드 락을 놓는다
 *
 * @cmdq: 대상 큐.
 *
 * 값을 하나 내린다. release 의미가 붙어, 락 안에서 한 큐 접근이 이
 * 감소보다 먼저 보이는 것이 보장된다 — 그래야 다음 배타 소유자가
 * 우리가 쓰던 자리를 안전하게 다룰 수 있다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static void arm_smmu_cmdq_shared_unlock(struct arm_smmu_cmdq *cmdq)
{
	(void)atomic_dec_return_release(&cmdq->lock);	/* [한국어] 반환값은 쓰지 않지만, release 의미를 얻으려고 이 판을 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_shared_tryunlock - 마지막 소유자가 아닐 때만 락을 놓는다
 *
 * @cmdq: 대상 큐.
 * @return: 놓았으면 참, 마지막 소유자로 보여 놓지 않았으면 거짓.
 *
 * 완료를 기다리는 CPU 가 락을 계속 쥐고 있어야 하는지 판단할 때 쓴다.
 * 자기가 마지막 소유자라면 락을 놓는 순간 다른 CPU 가 배타 잠그기로
 * 큐 상태를 갱신할 수 있게 되는데, 아직 완료를 기다리는 중이라면
 * 그 갱신이 곤란하다.
 *
 * 주석이 "racy"라고 인정하듯 검사와 감소 사이에 값이 바뀔 수 있다.
 * 그래도 안전한 이유는, 잘못 판단해도 락을 조금 더 오래 쥐는 것뿐이고
 * 정확성은 깨지지 않기 때문이다.
 *
 * 실행 컨텍스트: 완료 대기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static bool arm_smmu_cmdq_shared_tryunlock(struct arm_smmu_cmdq *cmdq)
{
	if (atomic_read(&cmdq->lock) == 1)	/* [한국어] 값이 1 이면 지금 소유자는 우리뿐으로 보인다. */
		return false;	/* [한국어] 놓지 않고 계속 쥔다 — 배타 갱신이 끼어들지 못하게. */

	arm_smmu_cmdq_shared_unlock(cmdq);	/* [한국어] 다른 소유자가 남아 있으니 놓아도 된다. */
	return true;	/* [한국어] 조건을 모두 통과했다. */
}

/* [한국어] 명령 큐 락을 배타 모드로 잡아 보는 매크로.
 *
 * 카운터가 정확히 0 일 때만 INT_MIN 으로 바꿔 성공한다 — 공유 소유자가
 * 하나라도 있으면 실패한다. 함수가 아니라 매크로인 이유는 인터럽트
 * 상태(flags)를 호출자의 지역 변수에 저장해야 하기 때문이다.
 * 인터럽트를 먼저 막는 것도 중요하다. 배타 구간에서 인터럽트가 들어와
 * 같은 큐에 명령을 넣으려 하면 스스로를 기다리는 교착이 된다. */
#define arm_smmu_cmdq_exclusive_trylock_irqsave(cmdq, flags)		\
({									\
	bool __ret;							\
	local_irq_save(flags);						\
	__ret = !atomic_cmpxchg_relaxed(&cmdq->lock, 0, INT_MIN);	\
	if (!__ret)							\
		local_irq_restore(flags);				\
	__ret;								\
})	/* [한국어] 매크로 본문의 끝 — 값을 내는 식이라 괄호로 감쌌다. */

/*
 * Only clear the sign bit when releasing the exclusive lock this will
 * allow any shared_lock() waiters to proceed without the possibility
 * of entering the exclusive lock in a tight loop.
 */
/* [한국어] (위 영어 주석 참고) 배타 락을 놓는 매크로.
 *
 * 카운터를 0 으로 되돌리지 않고 부호 비트만 내리는 것이 요점이다. 그동안
 * 쌓인 공유 대기자들의 증가분이 그대로 남아 카운터가 양수가 되고, 그
 * 대기자들이 곧바로 풀려난다. 만약 0 으로 되돌리면 어떤 CPU 가 곧바로
 * 다시 배타 락을 잡아, 공유 대기자들이 굶주릴 수 있다. */
#define arm_smmu_cmdq_exclusive_unlock_irqrestore(cmdq, flags)		\
({									\
	atomic_fetch_andnot_release(INT_MIN, &cmdq->lock);		\
	local_irq_restore(flags);					\
})	/* [한국어] 매크로 본문의 끝 — 값을 내는 식이라 괄호로 감쌌다. */


/*
 * Command queue insertion.
 * This is made fiddly by our attempts to achieve some sort of scalability
 * since there is one queue shared amongst all of the CPUs in the system.  If
 * you like mixed-size concurrency, dependency ordering and relaxed atomics,
 * then you'll *love* this monstrosity.
 *
 * The basic idea is to split the queue up into ranges of commands that are
 * owned by a given CPU; the owner may not have written all of the commands
 * itself, but is responsible for advancing the hardware prod pointer when
 * the time comes. The algorithm is roughly:
 *
 * 	1. Allocate some space in the queue. At this point we also discover
 *	   whether the head of the queue is currently owned by another CPU,
 *	   or whether we are the owner.
 *
 *	2. Write our commands into our allocated slots in the queue.
 *
 *	3. Mark our slots as valid in arm_smmu_cmdq.valid_map.
 *
 *	4. If we are an owner:
 *		a. Wait for the previous owner to finish.
 *		b. Mark the queue head as unowned, which tells us the range
 *		   that we are responsible for publishing.
 *		c. Wait for all commands in our owned range to become valid.
 *		d. Advance the hardware prod pointer.
 *		e. Tell the next owner we've finished.
 *
 *	5. If we are inserting a CMD_SYNC (we may or may not have been an
 *	   owner), then we need to stick around until it has completed:
 *		a. If we have MSIs, the SMMU can write back into the CMD_SYNC
 *		   to clear the first 4 bytes.
 *		b. Otherwise, we spin waiting for the hardware cons pointer to
 *		   advance past our command.
 *
 * The devil is in the details, particularly the use of locking for handling
 * SYNC completion and freeing up space in the queue before we think that it is
 * full.
 */
/* [한국어] (위 영어 주석의 알고리즘 설명 참고) 락 없는 명령 큐 삽입.
 *
 * 문제의 뿌리는 이렇다 — 명령 큐는 시스템에 하나뿐인데 모든 CPU 가 무효화를
 * 낸다. 락 하나로 감싸면 CPU 수가 늘수록 그 락이 병목이 된다.
 *
 * 해법의 핵심 착상은 "자리 차지"와 "발표"를 분리한 것이다. 각 CPU 는
 * 원자 연산 한 번으로 자기가 쓸 구간을 차지하고(1단계), 그 구간에 명령을
 * 쓴 뒤(2단계), 비트맵에 "내 몫은 다 됐다"를 표시한다(3단계). 여기까지는
 * 서로 겹치지 않는 자리를 다루므로 락이 필요 없다.
 *
 * 문제는 하드웨어에게 알리는 일이다. 생산 포인터는 하나뿐이고, 순서대로만
 * 밀 수 있다. 그래서 구간의 첫 자리를 차지한 CPU 가 "대표(owner)"가 되어,
 * 앞선 대표가 끝나기를 기다렸다가, 자기 구간의 비트가 모두 서기를 기다린
 * 뒤, 포인터를 한 번에 밀어 준다(4단계). 다른 CPU 들은 자기 몫만 쓰고
 * 곧바로 돌아갈 수 있다.
 *
 * 완료를 기다려야 하는 CMD_SYNC 만 예외적으로 더 머문다(5단계). MSI 를
 * 쓰면 하드웨어가 그 명령 자리에 값을 써 주므로 그 자리를 지켜보고,
 * 아니면 소비 포인터가 우리 명령을 지나치기를 기다린다.
 *
 * 앞의 공유/배타 락은 이 알고리즘의 곁가지다 — 큐가 찼다고 판단하기 전에
 * 소비 포인터를 다시 읽어 자리를 확보하는 일만 배타로 하고, 나머지는
 * 공유로 돈다. */
/*
 * [한국어]
 * __arm_smmu_cmdq_poll_set_valid_map - 유효 비트맵 구간을 세우거나 기다린다
 *
 * @cmdq: 대상 큐.
 * @sprod: 구간의 시작 생산 포인터.
 * @eprod: 구간의 끝 생산 포인터.
 * @set: 참이면 비트를 세우고, 거짓이면 비트가 지워지기를 기다린다.
 *
 * 세우기와 기다리기를 한 함수로 묶은 이유는, 두 동작이 비트맵을 훑는
 * 방식이 완전히 같기 때문이다. 구간이 여러 워드에 걸칠 수 있고 시작과
 * 끝이 워드 가운데일 수 있어, 마스크를 만들어 한 워드씩 처리한다.
 *
 * 비트의 뜻이 한 바퀴마다 뒤집힌다는 것이 이 설계의 묘미다. 링을 한 바퀴
 * 돌면 "세움"과 "지움"의 의미가 바뀌므로, 비트맵을 지우는 별도 단계가
 * 필요 없다 — 다음 바퀴의 CPU 가 자연스럽게 반대로 쓴다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_set_valid_map()/poll_valid_map() → [이 함수]
 */
static void __arm_smmu_cmdq_poll_set_valid_map(struct arm_smmu_cmdq *cmdq,
					       u32 sprod, u32 eprod, bool set)
{
	u32 swidx, sbidx, ewidx, ebidx;	/* [한국어] 시작·끝의 워드 첨자와 그 안에서의 비트 자리. */
	struct arm_smmu_ll_queue llq = {	/* [한국어] 첨자 계산에만 쓰는 임시 포인터 쌍 — 실제 큐 상태를 건드리지 않는다. */
		.max_n_shift	= cmdq->q.llq.max_n_shift,	/* [한국어] 링 크기 — Q_IDX 매크로가 이 값을 쓴다. */
		.prod		= sprod,	/* [한국어] 훑기를 시작할 자리. */
	};

	ewidx = BIT_WORD(Q_IDX(&llq, eprod));	/* [한국어] 끝 자리가 놓인 워드 첨자. */
	ebidx = Q_IDX(&llq, eprod) % BITS_PER_LONG;	/* [한국어] 그 워드 안에서의 비트 자리. */
	while (llq.prod != eprod) {	/* [한국어] 구간의 끝에 닿을 때까지 워드 단위로 훑는다. */
		unsigned long mask;	/* [한국어] 이 워드에서 다룰 비트만 고르는 마스크. */
		atomic_long_t *ptr;	/* [한국어] 그 워드의 주소 — 여러 CPU 가 동시에 만지므로 원자 연산으로 다룬다. */
		u32 limit = BITS_PER_LONG;	/* [한국어] 이 워드에서 어디까지 다룰지. 기본은 워드 끝까지다. */

		swidx = BIT_WORD(Q_IDX(&llq, llq.prod));	/* [한국어] 지금 자리가 놓인 워드 첨자. */
		sbidx = Q_IDX(&llq, llq.prod) % BITS_PER_LONG;	/* [한국어] 그 워드 안에서의 비트 자리. */

		ptr = &cmdq->valid_map[swidx];	/* [한국어] 다룰 워드. */

		if ((swidx == ewidx) && (sbidx < ebidx))	/* [한국어] 끝 자리와 같은 워드에 있고 아직 끝을 지나지 않았다면. */
			limit = ebidx;	/* [한국어] 워드 끝이 아니라 구간 끝까지만 다룬다. */

		mask = GENMASK(limit - 1, sbidx);	/* [한국어] 시작 비트부터 한계 직전까지의 마스크. */

		/*
		 * The valid bit is the inverse of the wrap bit. This means
		 * that a zero-initialised queue is invalid and, after marking
		 * all entries as valid, they become invalid again when we
		 * wrap.
		 */
		/* [한국어] (위 영어 주석 참고) 유효 비트의 뜻이 바퀴마다 뒤집힌다는 것이
		 * 이 설계의 요점이다. 0 으로 초기화된 큐는 첫 바퀴에서 "무효"이고,
		 * 모두 세워 "유효"로 만들면 다음 바퀴에서는 그 값이 다시 "무효"가 된다.
		 * 덕분에 비트맵을 지우는 별도 단계가 아예 필요 없다. */
		if (set) {	/* [한국어] 내 몫을 다 썼다고 표시하는 경우. */
			atomic_long_xor(mask, ptr);	/* [한국어] 뒤집기로 표시한다 — 바퀴마다 뜻이 반대라 세우기가 아니라 뒤집기다. */
		} else { /* Poll */	/* [한국어] (위 영어 주석 참고) 앞선 CPU 들이 다 쓰기를 기다리는 경우. */
			unsigned long valid;	/* [한국어] 이 바퀴에서 "유효"를 뜻하는 비트 패턴. */

			valid = (ULONG_MAX + !!Q_WRP(&llq, llq.prod)) & mask;	/* [한국어] 바퀴 비트가 0 이면 전부 1, 1 이면 전부 0 이 유효를 뜻한다 — 정수 넘침을 이용한 관용구다. */
			atomic_long_cond_read_relaxed(ptr, (VAL & mask) == valid);	/* [한국어] 그 패턴이 될 때까지 기다린다. */
		}

		llq.prod = queue_inc_prod_n(&llq, limit - sbidx);	/* [한국어] 이 워드에서 다룬 만큼 앞으로 옮긴다. */
	}
}

/* Mark all entries in the range [sprod, eprod) as valid */
/*
 * [한국어]
 * arm_smmu_cmdq_set_valid_map - 내가 쓴 구간을 "다 됐다"로 표시한다
 *
 * @cmdq: 대상 큐.
 * @sprod: 구간 시작.
 * @eprod: 구간 끝 (그 자리는 포함하지 않는다).
 *
 * (위 영어 주석 참고) 명령을 다 쓴 CPU 가 대표에게 보내는 신호다. 대표는
 * 자기 구간의 비트가 모두 설 때까지 기다렸다가 하드웨어에 알린다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static void arm_smmu_cmdq_set_valid_map(struct arm_smmu_cmdq *cmdq,
					u32 sprod, u32 eprod)
{
	__arm_smmu_cmdq_poll_set_valid_map(cmdq, sprod, eprod, true);	/* [한국어] 세우기 모드로 공통 함수를 부른다. */
}

/* Wait for all entries in the range [sprod, eprod) to become valid */
/*
 * [한국어]
 * arm_smmu_cmdq_poll_valid_map - 그 구간을 모두가 다 쓰기를 기다린다
 *
 * @cmdq: 대상 큐.
 * @sprod: 구간 시작.
 * @eprod: 구간 끝.
 *
 * (위 영어 주석 참고) 대표가 하드웨어에 생산 포인터를 밀기 전에 반드시
 * 거치는 단계다. 이것을 건너뛰면 아직 쓰이지 않은 자리를 하드웨어가
 * 명령으로 읽어 버린다.
 *
 * 실행 컨텍스트: 명령 발행 경로(대표 CPU). 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static void arm_smmu_cmdq_poll_valid_map(struct arm_smmu_cmdq *cmdq,
					 u32 sprod, u32 eprod)
{
	__arm_smmu_cmdq_poll_set_valid_map(cmdq, sprod, eprod, false);	/* [한국어] 기다리기 모드로 공통 함수를 부른다. */
}

/* Wait for the command queue to become non-full */
/*
 * [한국어]
 * arm_smmu_cmdq_poll_until_not_full - 큐에 자리가 날 때까지 기다린다
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 대상 큐.
 * @llq: 호출자가 들고 있는 포인터 사본 — 갱신된 값을 여기 담아 준다.
 * @return: 0 자리가 났다, -ETIMEDOUT 시간이 다 됐다.
 *
 * 큐가 찼다고 보이는 이유는 대개 소프트웨어 쪽 소비 포인터가 낡았기
 * 때문이다. 하드웨어는 이미 처리했는데 아무도 그 사실을 읽어 오지
 * 않은 것이다. 그래서 먼저 배타 락을 잡아 직접 갱신해 본다.
 *
 * 배타 락을 못 잡았다면 다른 CPU 가 이미 갱신하는 중이라는 뜻이므로,
 * 그 결과가 보일 때까지 기다리기만 하면 된다 — 모두가 갱신하려 달려들면
 * 레지스터 읽기가 몰려 오히려 느려진다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수] → queue_poll()
 */
static int arm_smmu_cmdq_poll_until_not_full(struct arm_smmu_device *smmu,
					     struct arm_smmu_cmdq *cmdq,
					     struct arm_smmu_ll_queue *llq)
{
	unsigned long flags;	/* [한국어] 배타 락이 저장할 인터럽트 상태. */
	struct arm_smmu_queue_poll qp;	/* [한국어] 물러나며 기다리기 상태. */
	int ret = 0;	/* [한국어] 결과. */

	/*
	 * Try to update our copy of cons by grabbing exclusive cmdq access. If
	 * that fails, spin until somebody else updates it for us.
	 */
	/* [한국어] (위 영어 주석 참고) 갱신은 한 CPU 만 하면 된다. 못 잡았다면
	 * 남이 하고 있다는 뜻이므로 그 결과를 기다리는 편이 낫다. */
	if (arm_smmu_cmdq_exclusive_trylock_irqsave(cmdq, flags)) {	/* [한국어] 공유 소유자가 하나도 없을 때만 성공한다. */
		WRITE_ONCE(cmdq->q.llq.cons, readl_relaxed(cmdq->q.cons_reg));	/* [한국어] 하드웨어의 실제 소비 위치를 읽어 공유 사본을 갱신한다. */
		arm_smmu_cmdq_exclusive_unlock_irqrestore(cmdq, flags);	/* [한국어] 부호 비트만 내려 대기자들을 풀어 준다. */
		llq->val = READ_ONCE(cmdq->q.llq.val);	/* [한국어] 갱신된 값을 호출자의 사본에도 담는다 — 두 포인터를 한 번에 읽는다. */
		return 0;	/* [한국어] 이제 자리가 있을 것이다. */
	}

	queue_poll_init(smmu, &qp);	/* [한국어] 기다리기 상태를 준비한다. */
	do {
		llq->val = READ_ONCE(cmdq->q.llq.val);	/* [한국어] 남이 갱신했는지 다시 읽는다. */
		if (!queue_full(llq))	/* [한국어] 자리가 났으면. */
			break;	/* [한국어] 기다림을 끝낸다. */

		ret = queue_poll(&qp);	/* [한국어] 한 번 물러나 기다린다. 마감을 넘기면 오류가 나온다. */
	} while (!ret);

	return ret;	/* [한국어] 시간이 다 됐으면 호출자가 명령을 포기한다. */
}

/*
 * Wait until the SMMU signals a CMD_SYNC completion MSI.
 * Must be called with the cmdq lock held in some capacity.
 */
/*
 * [한국어]
 * __arm_smmu_cmdq_poll_until_msi - 하드웨어가 완료 표시를 쓸 때까지 기다린다
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 대상 큐.
 * @llq: 포인터 사본 — 어느 자리를 지켜볼지 여기서 얻고, 결과도 담는다.
 * @return: 0 완료됐다, -ETIMEDOUT 시간이 다 됐다.
 *
 * (위 영어 주석 참고) CMD_SYNC 를 지을 때 완료 표시를 쓸 주소로 그 명령
 * 자신이 놓인 자리를 주었다. 하드웨어가 앞선 명령을 모두 끝내면 그 자리에
 * MSI 쓰기를 하고, 그러면 명령의 첫 워드가 0 이 된다. 그 순간을 지켜본다.
 *
 * WFE 를 끄는 이유가 중요하다 — 이 MSI 는 메모리 쓰기일 뿐 이벤트를
 * 발생시키지 않으므로, WFE 로 멈추면 아무도 깨워 주지 않는다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 큐 락을 어떤 형태로든 쥔 채 불려야 한다.
 * 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_poll_until_sync() → [이 함수]
 */
static int __arm_smmu_cmdq_poll_until_msi(struct arm_smmu_device *smmu,
					  struct arm_smmu_cmdq *cmdq,
					  struct arm_smmu_ll_queue *llq)
{
	int ret = 0;	/* [한국어] 결과 — 아래 조건식 안에서 갱신된다. */
	struct arm_smmu_queue_poll qp;	/* [한국어] 기다리기 상태. */
	u32 *cmd = (u32 *)(Q_ENT(&cmdq->q, llq->prod));	/* [한국어] 우리 CMD_SYNC 가 놓인 자리 — 하드웨어가 이 자리에 0 을 쓴다. */

	queue_poll_init(smmu, &qp);	/* [한국어] 마감과 물러나기 상태를 준비한다. */

	/*
	 * The MSI won't generate an event, since it's being written back
	 * into the command queue.
	 */
	/* [한국어] (위 영어 주석 참고) 이 MSI 는 인터럽트가 아니라 메모리 쓰기라
	 * WFE 를 깨우지 않는다. 그대로 WFE 로 기다리면 영원히 깨어나지 못한다. */
	qp.wfe = false;	/* [한국어] 저전력 대기 대신 바쁜 대기를 쓴다. */
	smp_cond_load_relaxed(cmd, !VAL || (ret = queue_poll(&qp)));	/* [한국어] 값이 0 이 되거나 마감을 넘길 때까지 지켜본다 — 조건식 안에서 마감 검사도 함께 한다. */
	llq->cons = ret ? llq->prod : queue_inc_prod_n(llq, 1);	/* [한국어] 성공했으면 우리 명령 다음 자리까지 소비된 것이고, 실패했으면 그대로 둔다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * Wait until the SMMU cons index passes llq->prod.
 * Must be called with the cmdq lock held in some capacity.
 */
/*
 * [한국어]
 * __arm_smmu_cmdq_poll_until_consumed - 소비 포인터가 우리 명령을 지나기를 기다린다
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 대상 큐.
 * @llq: 포인터 사본.
 * @return: 0 지나갔다, -ETIMEDOUT 시간이 다 됐다.
 *
 * (위 영어 주석 참고) MSI 를 쓸 수 없는 하드웨어에서 완료를 기다리는 방법.
 * 소비 포인터 레지스터를 반복해 읽으며 우리 자리를 지나쳤는지 본다.
 *
 * 안쪽 주석이 설명하는 readl 의 이유가 이 함수의 핵심이다. relaxed 로
 * 읽으면 다른 CPU 의 shared_lock() 획득이 우리에게 보이지 않을 수 있고,
 * 그러면 뒤이은 tryunlock() 이 "내가 마지막"이라고 잘못 판단해 아직
 * 기다리는 CPU 가 있는데도 큐 상태를 갱신해 버릴 수 있다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 큐 락을 쥔 채 불려야 한다. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_poll_until_sync() → [이 함수]
 */
static int __arm_smmu_cmdq_poll_until_consumed(struct arm_smmu_device *smmu,
					       struct arm_smmu_cmdq *cmdq,
					       struct arm_smmu_ll_queue *llq)
{
	struct arm_smmu_queue_poll qp;	/* [한국어] 기다리기 상태. */
	u32 prod = llq->prod;	/* [한국어] 우리 명령이 놓인 자리를 붙잡아 둔다 — 아래에서 llq 가 갱신되기 때문이다. */
	int ret = 0;	/* [한국어] 결과. */

	queue_poll_init(smmu, &qp);	/* [한국어] 마감을 정한다. */
	llq->val = READ_ONCE(cmdq->q.llq.val);	/* [한국어] 지금 알려진 포인터 쌍을 읽어 시작한다. */
	do {
		if (queue_consumed(llq, prod))	/* [한국어] 소비 포인터가 우리 자리를 지나쳤는가. */
			break;	/* [한국어] 우리 명령이 처리됐다. */

		ret = queue_poll(&qp);	/* [한국어] 한 번 물러나 기다린다. */

		/*
		 * This needs to be a readl() so that our subsequent call
		 * to arm_smmu_cmdq_shared_tryunlock() can fail accurately.
		 *
		 * Specifically, we need to ensure that we observe all
		 * shared_lock()s by other CMD_SYNCs that share our owner,
		 * so that a failing call to tryunlock() means that we're
		 * the last one out and therefore we can safely advance
		 * cmdq->q.llq.cons. Roughly speaking:
		 *
		 * CPU 0		CPU1			CPU2 (us)
		 *
		 * if (sync)
		 * 	shared_lock();
		 *
		 * dma_wmb();
		 * set_valid_map();
		 *
		 * 			if (owner) {
		 *				poll_valid_map();
		 *				<control dependency>
		 *				writel(prod_reg);
		 *
		 *						readl(cons_reg);
		 *						tryunlock();
		 *
		 * Requires us to see CPU 0's shared_lock() acquisition.
		 */
		/* [한국어] (위 영어 주석과 다이어그램 참고) 왜 relaxed 가 아닌 readl 인가.
		 * 이 읽기가 순서를 세워 주지 않으면, 같은 대표 아래 있던 다른 CPU 의
		 * shared_lock() 이 우리 눈에 보이지 않을 수 있다. 그러면 아래에서
		 * tryunlock() 이 "락을 쥔 것은 나뿐"이라고 잘못 판단하고, 아직
		 * 기다리는 CPU 가 있는데도 소비 포인터를 밀어 버린다.
		 * 다이어그램의 사슬 — CPU0 의 락 획득 → 유효 표시 → CPU1(대표)의
		 * 포인터 쓰기 → 우리의 레지스터 읽기 — 가 성립해야 그 판단이 옳다. */
		llq->cons = readl(cmdq->q.cons_reg);	/* [한국어] 순서를 세우는 판으로 읽는다. */
	} while (!ret);

	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_poll_until_sync - 완료를 기다리는 방식을 골라 부른다
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 대상 큐.
 * @llq: 포인터 사본.
 * @return: 0 완료, -ETIMEDOUT 시간 초과.
 *
 * MSI 방식이 쓸 수 있으면 그쪽이 훨씬 싸다 — 메모리 한 자리만 지켜보면
 * 되고 MMIO 읽기가 필요 없기 때문이다. 보조 큐처럼 그 방식을 못 쓰는
 * 경우에만 소비 포인터를 직접 읽는 쪽으로 간다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수]
 */
static int arm_smmu_cmdq_poll_until_sync(struct arm_smmu_device *smmu,
					 struct arm_smmu_cmdq *cmdq,
					 struct arm_smmu_ll_queue *llq)
{
	if (smmu->options & ARM_SMMU_OPT_MSIPOLL &&	/* [한국어] MSI 로 완료를 감지할 수 있고. */
	    !arm_smmu_cmdq_needs_busy_polling(smmu, cmdq))	/* [한국어] 이 큐가 그 방식을 쓸 수 있다면. */
		return __arm_smmu_cmdq_poll_until_msi(smmu, cmdq, llq);	/* [한국어] 메모리 한 자리만 지켜본다 — 싼 쪽이다. */

	return __arm_smmu_cmdq_poll_until_consumed(smmu, cmdq, llq);	/* [한국어] 아니면 소비 포인터 레지스터를 반복해 읽는다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_write_entries - 차지한 구간에 명령들을 써 넣는다
 *
 * @cmdq: 대상 큐.
 * @cmds: 지어 둔 명령 워드들.
 * @prod: 내 구간이 시작하는 자리.
 * @n: 명령 개수.
 *
 * 자리는 이미 원자적으로 차지해 두었으므로, 여기서는 락 없이 그냥 쓴다 —
 * 다른 CPU 는 다른 구간을 쓰고 있어 겹치지 않는다. 링을 넘어가는 계산만
 * 매번 다시 해 주면 된다.
 *
 * 실행 컨텍스트: 명령 발행 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmdlist() → [이 함수] → queue_write()
 */
static void arm_smmu_cmdq_write_entries(struct arm_smmu_cmdq *cmdq, u64 *cmds,
					u32 prod, int n)
{
	int i;	/* [한국어] 명령 반복자. */
	struct arm_smmu_ll_queue llq = {	/* [한국어] 첨자 계산용 임시 구조 — 공유 상태를 건드리지 않는다. */
		.max_n_shift	= cmdq->q.llq.max_n_shift,	/* [한국어] 링 크기. */
		.prod		= prod,	/* [한국어] 내 구간의 시작. */
	};

	for (i = 0; i < n; ++i) {	/* [한국어] 명령을 하나씩. */
		u64 *cmd = &cmds[i * CMDQ_ENT_DWORDS];	/* [한국어] 원본에서 i 번째 명령의 시작. */

		prod = queue_inc_prod_n(&llq, i);	/* [한국어] 링을 넘어가는 것까지 고려해 i 칸 앞의 자리를 구한다. */
		queue_write(Q_ENT(&cmdq->q, prod), cmd, CMDQ_ENT_DWORDS);	/* [한국어] 하드웨어 형식으로 그 자리에 쓴다. 아직 유효 표시는 하지 않는다. */
	}
}

/*
 * This is the actual insertion function, and provides the following
 * ordering guarantees to callers:
 *
 * - There is a dma_wmb() before publishing any commands to the queue.
 *   This can be relied upon to order prior writes to data structures
 *   in memory (such as a CD or an STE) before the command.
 *
 * - On completion of a CMD_SYNC, there is a control dependency.
 *   This can be relied upon to order subsequent writes to memory (e.g.
 *   freeing an IOVA) after completion of the CMD_SYNC.
 *
 * - Command insertion is totally ordered, so if two CPUs each race to
 *   insert their own list of commands then all of the commands from one
 *   CPU will appear before any of the commands from the other CPU.
 */
/*
 * [한국어]
 * arm_smmu_cmdq_issue_cmdlist - 명령 여러 개를 락 없이 큐에 밀어 넣는다
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 넣을 명령 큐.
 * @cmds: 이미 지어 둔 명령 워드들.
 * @n: 명령 개수.
 * @sync: 참이면 뒤에 CMD_SYNC 를 붙이고 완료까지 기다린다.
 * @return: 0 성공, -ETIMEDOUT 큐가 막혔거나 완료를 못 기다렸다.
 *
 * 이 드라이버에서 가장 정교한 함수다. 위 알고리즘 설명(앞의 큰 주석)의
 * 다섯 단계가 본문에 그대로 번호로 표시되어 있다.
 *
 * (위 영어 주석 참고) 호출자에게 세 가지를 약속한다.
 * 첫째, 명령을 발표하기 전에 dma_wmb() 가 있다 — 그래서 이 함수 앞에서
 * 고쳐 쓴 STE 나 CD 가 반드시 명령보다 먼저 하드웨어에 보인다. 무효화
 * 명령보다 항목 쓰기가 늦게 보이면 하드웨어가 옛 값을 다시 캐시에 올린다.
 * 둘째, CMD_SYNC 가 끝난 뒤에는 제어 의존이 있다 — 그래서 무효화가 끝난
 * 뒤에야 IOVA 를 놓는 코드가 안전하다.
 * 셋째, 삽입은 완전히 순서가 정해진다 — 두 CPU 가 각자 명령 목록을 넣으면
 * 한쪽이 통째로 앞서거나 통째로 뒤선다. 목록이 뒤섞이지 않는다.
 *
 * 인터럽트를 막고 도는 것도 중요하다. 자리를 차지한 뒤 유효 표시를 하기
 * 전에 선점되면, 대표 CPU 가 그 구간의 비트가 서기를 하염없이 기다린다.
 *
 * 실행 컨텍스트: 원자적 문맥에서도 불린다. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_domain_inv_range()/write_entry()/batch_submit() → [이 함수]
 */
int arm_smmu_cmdq_issue_cmdlist(struct arm_smmu_device *smmu,
				struct arm_smmu_cmdq *cmdq, u64 *cmds, int n,
				bool sync)
{
	u64 cmd_sync[CMDQ_ENT_DWORDS];	/* [한국어] 뒤에 붙일 완료 대기 명령. */
	u32 prod;	/* [한국어] 여러 곳에서 자리 계산에 재사용하는 임시 값. */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장 자리. */
	bool owner;	/* [한국어] 내가 이 구간의 대표인가 — 하드웨어에 알리는 책임을 진다. */
	struct arm_smmu_ll_queue llq, head;	/* [한국어] 차지하기 전 상태와 차지한 뒤 상태. */
	int ret = 0;	/* [한국어] 결과. */

	llq.max_n_shift = cmdq->q.llq.max_n_shift;	/* [한국어] 링 크기는 바뀌지 않으므로 미리 담아 둔다. */

	/* 1. Allocate some space in the queue */
	/* [한국어] (위 영어 주석 참고) 1단계 — 원자적으로 내 구간을 차지한다. */
	local_irq_save(flags);	/* [한국어] 차지한 뒤 유효 표시를 마칠 때까지 선점되면 대표가 하염없이 기다리게 된다. */
	llq.val = READ_ONCE(cmdq->q.llq.val);	/* [한국어] 두 포인터를 한 번에 읽는다 — 겹쳐 둔 union 덕분이다. */
	do {
		u64 old;	/* [한국어] 겨루기에서 실제로 있던 값. */

		while (!queue_has_space(&llq, n + sync)) {	/* [한국어] 내 명령들과 완료 명령까지 들어갈 자리가 있는가. */
			local_irq_restore(flags);	/* [한국어] 기다리는 동안은 인터럽트를 열어 둔다 — 오래 걸릴 수 있다. */
			if (arm_smmu_cmdq_poll_until_not_full(smmu, cmdq, &llq))	/* [한국어] 소비 포인터를 갱신하며 자리가 나기를 기다린다. */
				dev_err_ratelimited(smmu->dev, "CMDQ timeout\n");	/* [한국어] 시간이 다 돼도 계속 시도한다 — 명령을 버릴 수는 없기 때문이다. 로그만 제한한다. */
			local_irq_save(flags);	/* [한국어] 다시 막고 겨루기를 이어 간다. */
		}

		head.cons = llq.cons;	/* [한국어] 소비 포인터는 그대로 둔다 — 우리가 바꾸는 것은 생산 쪽뿐이다. */
		head.prod = queue_inc_prod_n(&llq, n + sync) |	/* [한국어] 내가 차지할 구간의 끝. */
					     CMDQ_PROD_OWNED_FLAG;	/* [한국어] "이 머리는 임자가 있다"는 표시를 함께 켠다 — 다음 CPU 는 이것을 보고 자기가 대표가 아님을 안다. */

		old = cmpxchg_relaxed(&cmdq->q.llq.val, llq.val, head.val);	/* [한국어] 읽었던 값 그대로일 때만 바꿔치기한다 — 이 한 번의 원자 연산이 자리 차지의 전부다. */
		if (old == llq.val)	/* [한국어] 성공했다면. */
			break;	/* [한국어] 내 구간이 정해졌다. */

		llq.val = old;	/* [한국어] 남이 먼저 차지했으니 그 값으로 다시 겨룬다. */
	} while (1);
	owner = !(llq.prod & CMDQ_PROD_OWNED_FLAG);	/* [한국어] 내가 읽었던 값에 임자 표시가 없었다면 내가 이 구간의 첫 주인, 곧 대표다. */
	head.prod &= ~CMDQ_PROD_OWNED_FLAG;	/* [한국어] 이제부터는 순수한 자리 값으로만 쓴다. */
	llq.prod &= ~CMDQ_PROD_OWNED_FLAG;	/* [한국어] 시작 자리도 마찬가지. */

	/*
	 * 2. Write our commands into the queue
	 * Dependency ordering from the cmpxchg() loop above.
	 */
	/* [한국어] (위 영어 주석 참고) 2단계 — 차지한 자리에 명령을 쓴다. 위 원자
	 * 연산이 의존 관계를 세워 주므로 여기에 별도 장벽이 필요 없다. */
	arm_smmu_cmdq_write_entries(cmdq, cmds, llq.prod, n);	/* [한국어] 다른 CPU 와 겹치지 않는 구간이라 락 없이 쓴다. */
	if (sync) {	/* [한국어] 완료를 기다려야 하는 경우. */
		prod = queue_inc_prod_n(&llq, n);	/* [한국어] 내 명령들 바로 뒤 자리. */
		arm_smmu_cmdq_build_sync_cmd(cmd_sync, smmu, cmdq, prod);	/* [한국어] 그 자리를 완료 표시 주소로 삼는 CMD_SYNC 를 짓는다. */
		queue_write(Q_ENT(&cmdq->q, prod), cmd_sync, CMDQ_ENT_DWORDS);	/* [한국어] 그 자리에 쓴다. */

		/*
		 * In order to determine completion of our CMD_SYNC, we must
		 * ensure that the queue can't wrap twice without us noticing.
		 * We achieve that by taking the cmdq lock as shared before
		 * marking our slot as valid.
		 */
		/* [한국어] (위 영어 주석 참고) 완료를 기다리는 동안 큐가 두 바퀴를 돌면,
		 * 우리가 지켜보던 자리에 전혀 다른 명령이 들어와 있을 수 있다. 그러면
		 * 완료를 잘못 판정한다. 공유 락을 쥐고 있으면 그 사이에 소비 포인터를
		 * 크게 밀어붙이는 배타 갱신이 끼어들 수 없어, 두 바퀴가 돌지 않는다. */
		arm_smmu_cmdq_shared_lock(cmdq);	/* [한국어] 유효 표시를 하기 전에 잡아야 한다 — 표시한 뒤에는 이미 늦다. */
	}

	/* 3. Mark our slots as valid, ensuring commands are visible first */
	/* [한국어] (위 영어 주석 참고) 3단계 — "내 몫은 다 됐다"를 알린다. */
	dma_wmb();	/* [한국어] 명령 내용이 먼저 보이고 그다음 유효 표시가 보이게 한다. 순서가 뒤집히면 대표가 아직 안 쓰인 명령을 발표한다. 호출자가 앞서 고친 STE/CD 도 이 장벽 덕분에 명령보다 먼저 보인다. */
	arm_smmu_cmdq_set_valid_map(cmdq, llq.prod, head.prod);	/* [한국어] 내 구간의 비트를 뒤집어 표시한다. */

	/* 4. If we are the owner, take control of the SMMU hardware */
	/* [한국어] (위 영어 주석 참고) 4단계 — 대표만 하는 일. 나머지 CPU 는 여기를 건너뛴다. */
	if (owner) {
		/* a. Wait for previous owner to finish */
		/* [한국어] (위 영어 주석 참고) 생산 포인터는 순서대로만 밀 수 있으므로
		 * 앞선 대표가 자기 몫을 발표할 때까지 기다린다. */
		atomic_cond_read_relaxed(&cmdq->owner_prod, VAL == llq.prod);	/* [한국어] 앞 대표가 내 시작 자리까지 밀어 놓기를 기다린다. */

		/* b. Stop gathering work by clearing the owned flag */
		/* [한국어] (위 영어 주석 참고) 임자 표시를 내려, 이 뒤에 오는 CPU 가
		 * 새 대표가 되게 한다. 그 순간의 값이 곧 내가 발표할 범위의 끝이다. */
		prod = atomic_fetch_andnot_relaxed(CMDQ_PROD_OWNED_FLAG,	/* [한국어] 표시를 내리면서 그 직전 값을 얻는다. */
						   &cmdq->q.llq.atomic.prod);
		prod &= ~CMDQ_PROD_OWNED_FLAG;	/* [한국어] 순수한 자리 값만 남긴다 — 내 구간 뒤에 붙은 다른 CPU 들의 명령까지 포함한다. */

		/*
		 * c. Wait for any gathered work to be written to the queue.
		 * Note that we read our own entries so that we have the control
		 * dependency required by (d).
		 */
		/* [한국어] (위 영어 주석 참고) 내 구간뿐 아니라 그 뒤에 붙은 CPU 들의
		 * 명령까지 모두 쓰였는지 확인한다. 내 자리도 함께 읽는 것이 요점인데,
		 * 그 읽기가 아래 포인터 쓰기와의 제어 의존을 만들어 준다. */
		arm_smmu_cmdq_poll_valid_map(cmdq, llq.prod, prod);	/* [한국어] 그 범위의 비트가 모두 설 때까지 기다린다. */

		/*
		 * d. Advance the hardware prod pointer
		 * Control dependency ordering from the entries becoming valid.
		 */
		/* [한국어] (위 영어 주석 참고) 이제 하드웨어에게 알린다. 위 기다림이
		 * 만든 제어 의존 덕분에, 아직 쓰이지 않은 자리를 발표하는 일이 없다. */
		writel_relaxed(prod, cmdq->q.prod_reg);	/* [한국어] 여러 CPU 몫을 한 번의 MMIO 쓰기로 발표한다 — 이것이 이 알고리즘의 성능 이득이다. */

		/*
		 * e. Tell the next owner we're done
		 * Make sure we've updated the hardware first, so that we don't
		 * race to update prod and potentially move it backwards.
		 */
		/* [한국어] (위 영어 주석 참고) 다음 대표를 풀어 준다. release 로 써서
		 * 위 MMIO 쓰기가 반드시 먼저 보이게 한다 — 순서가 뒤집히면 다음 대표가
		 * 먼저 포인터를 밀어 값이 거꾸로 갈 수 있다. */
		atomic_set_release(&cmdq->owner_prod, prod);	/* [한국어] 내가 어디까지 발표했는지 알린다. */
	}

	/* 5. If we are inserting a CMD_SYNC, we must wait for it to complete */
	/* [한국어] (위 영어 주석 참고) 5단계 — 완료를 기다려야 하는 경우에만. */
	if (sync) {
		llq.prod = queue_inc_prod_n(&llq, n);	/* [한국어] 우리 CMD_SYNC 가 놓인 자리. */
		ret = arm_smmu_cmdq_poll_until_sync(smmu, cmdq, &llq);	/* [한국어] 그것이 처리될 때까지 기다린다. */
		if (ret) {	/* [한국어] 시간이 다 됐다면. */
			dev_err_ratelimited(smmu->dev,	/* [한국어] 하드웨어가 멈춰 선 것이므로 포인터 값을 함께 남겨 진단을 돕는다. */
					    "CMD_SYNC timeout at 0x%08x [hwprod 0x%08x, hwcons 0x%08x]\n",
					    llq.prod,
					    readl_relaxed(cmdq->q.prod_reg),	/* [한국어] 하드웨어가 아는 생산 위치. */
					    readl_relaxed(cmdq->q.cons_reg));	/* [한국어] 하드웨어가 아는 소비 위치 — 여기서 멈춰 있다. */
		}

		/*
		 * Try to unlock the cmdq lock. This will fail if we're the last
		 * reader, in which case we can safely update cmdq->q.llq.cons
		 */
		/* [한국어] (위 영어 주석 참고) 내가 마지막 대기자라면, 이 시점에 소비
		 * 포인터 사본을 갱신해도 안전하다 — 다른 CPU 가 그 값에 기대어 기다리고
		 * 있지 않기 때문이다. 그 판정을 "놓기가 실패했는가"로 대신한다. */
		if (!arm_smmu_cmdq_shared_tryunlock(cmdq)) {	/* [한국어] 놓지 못했다면 내가 마지막이라는 뜻. */
			WRITE_ONCE(cmdq->q.llq.cons, llq.cons);	/* [한국어] 우리가 확인한 소비 위치를 공유 사본에 반영해, 다음 CPU 들이 자리를 더 쓸 수 있게 한다. */
			arm_smmu_cmdq_shared_unlock(cmdq);	/* [한국어] 그러고 나서 락을 놓는다. */
		}
	}

	local_irq_restore(flags);	/* [한국어] 인터럽트를 되돌린다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * __arm_smmu_cmdq_issue_cmd - 명령 하나를 짓고 큐에 넣는다
 *
 * @smmu: 대상 하드웨어.
 * @ent: 명령 서술 구조.
 * @sync: 완료까지 기다릴 것인가.
 * @return: 0 성공, -EINVAL 모르는 명령, 그 밖의 음수는 큐 오류.
 *
 * 명령을 짓는 일과 큐에 넣는 일을 묶은 얇은 껍데기다. 명령 하나를 낼
 * 때마다 큐 삽입 절차 전체를 돌므로, 여러 개를 낼 때는 batch 계열을
 * 쓰는 편이 훨씬 싸다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_cmdq_issue_cmd()/issue_cmd_with_sync() → [이 함수]
 */
static int __arm_smmu_cmdq_issue_cmd(struct arm_smmu_device *smmu,
				     struct arm_smmu_cmdq_ent *ent,
				     bool sync)
{
	u64 cmd[CMDQ_ENT_DWORDS];	/* [한국어] 지어 담을 자리. */

	if (unlikely(arm_smmu_cmdq_build_cmd(cmd, ent))) {	/* [한국어] 짓기에 실패하면 — 드라이버 버그일 때만 일어난다. */
		dev_warn(smmu->dev, "ignoring unknown CMDQ opcode 0x%x\n",	/* [한국어] 조용히 넘기지 않고 흔적을 남긴다. */
			 ent->opcode);
		return -EINVAL;	/* [한국어] 인자나 상태가 조건에 맞지 않는다. */
	}

	return arm_smmu_cmdq_issue_cmdlist(	/* [한국어] 명령 하나짜리 목록으로 넣는다. */
		smmu, arm_smmu_get_cmdq(smmu, ent), cmd, 1, sync);	/* [한국어] 큐 선택도 여기서 함께 한다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_issue_cmd - 명령 하나를 넣고 기다리지 않는다
 *
 * @smmu: 대상 하드웨어.
 * @ent: 명령 서술 구조.
 * @return: 0 성공, 음수 오류.
 *
 * 완료를 확인할 필요가 없는 명령에 쓴다 — 미리 읽기 힌트처럼 실패해도
 * 정확성에 영향이 없는 것들이다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_prefetch_cfg() 등 → [이 함수]
 */
static int arm_smmu_cmdq_issue_cmd(struct arm_smmu_device *smmu,
				   struct arm_smmu_cmdq_ent *ent)
{
	return __arm_smmu_cmdq_issue_cmd(smmu, ent, false);	/* [한국어] 기다리지 않고 돌아온다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_issue_cmd_with_sync - 명령 하나를 넣고 완료까지 기다린다
 *
 * @smmu: 대상 하드웨어.
 * @ent: 명령 서술 구조.
 * @return: 0 성공, 음수 오류.
 *
 * 무효화처럼 "정말 끝났음"을 확인해야 하는 명령에 쓴다. 돌아온 뒤에는
 * 그 무효화가 하드웨어에 반영됐다고 믿어도 된다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다. 잠들지 않는다.
 *
 * 호출 체인:
 *   설정 무효화 경로 등 → [이 함수]
 */
static int arm_smmu_cmdq_issue_cmd_with_sync(struct arm_smmu_device *smmu,
					     struct arm_smmu_cmdq_ent *ent)
{
	return __arm_smmu_cmdq_issue_cmd(smmu, ent, true);	/* [한국어] 완료 대기를 붙인다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_batch_init - 명령 묶음을 시작한다
 *
 * @smmu: 대상 하드웨어.
 * @cmds: 초기화할 묶음.
 * @ent: 앞으로 넣을 명령의 본보기 — 어느 큐를 쓸지 고르는 데 쓴다.
 *
 * 명령을 하나씩 넣으면 삽입 절차와 MMIO 쓰기가 매번 일어난다. 여러 개를
 * 모아 한 번에 넣으면 그 비용이 개수로 나뉘므로, 무효화처럼 명령이 여럿
 * 나오는 경로는 모두 이 묶음을 쓴다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_domain_inv_range() 등 → [이 함수] → arm_smmu_get_cmdq()
 */
static void arm_smmu_cmdq_batch_init(struct arm_smmu_device *smmu,
				     struct arm_smmu_cmdq_batch *cmds,
				     struct arm_smmu_cmdq_ent *ent)
{
	cmds->num = 0;	/* [한국어] 아직 아무것도 담기지 않았다. */
	cmds->cmdq = arm_smmu_get_cmdq(smmu, ent);	/* [한국어] 이 묶음이 통째로 들어갈 큐를 미리 정한다 — 묶음 안에서 큐가 바뀌면 안 된다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_batch_add - 명령을 묶음에 담는다
 *
 * @smmu: 대상 하드웨어.
 * @cmds: 담을 묶음.
 * @cmd: 담을 명령의 서술 구조.
 *
 * 담기 전에 두 가지를 검사해 필요하면 묶음을 먼저 비운다.
 * 첫째, 이 명령을 지금 큐가 받지 못한다면(게스트 소유 보조 큐의 제한)
 * 지금까지 모은 것을 내보내고 큐를 다시 고른다.
 * 둘째, 명령마다 완료 대기를 강제해야 하는 결함 하드웨어라면 묶음이 거의
 * 찼을 때 미리 내보낸다.
 * 그러고도 묶음이 가득 찼으면 비우고 이어 담는다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다. 잠들지 않는다.
 *
 * 호출 체인:
 *   무효화 경로 → [이 함수] → arm_smmu_cmdq_build_cmd()
 */
static void arm_smmu_cmdq_batch_add(struct arm_smmu_device *smmu,
				    struct arm_smmu_cmdq_batch *cmds,
				    struct arm_smmu_cmdq_ent *cmd)
{
	bool unsupported_cmd = !arm_smmu_cmdq_supports_cmd(cmds->cmdq, cmd);	/* [한국어] 지금 큐가 이 명령을 받을 수 있는가 — 보조 큐는 받는 종류가 제한된다. */
	bool force_sync = (cmds->num == CMDQ_BATCH_ENTRIES - 1) &&	/* [한국어] 묶음이 한 칸 남았고. */
			  (smmu->options & ARM_SMMU_OPT_CMDQ_FORCE_SYNC);	/* [한국어] 명령마다 완료 대기를 강제해야 하는 결함 하드웨어라면. */
	int index;	/* [한국어] 묶음 버퍼에서 이 명령이 놓일 자리. */

	if (force_sync || unsupported_cmd) {	/* [한국어] 둘 중 하나라도 걸리면 지금까지 모은 것을 먼저 내보낸다. */
		arm_smmu_cmdq_issue_cmdlist(smmu, cmds->cmdq, cmds->cmds,	/* [한국어] 완료 대기를 붙여 내보낸다. */
					    cmds->num, true);
		arm_smmu_cmdq_batch_init(smmu, cmds, cmd);	/* [한국어] 새 명령에 맞는 큐로 다시 시작한다. */
	}

	if (cmds->num == CMDQ_BATCH_ENTRIES) {	/* [한국어] 묶음이 가득 찼다면. */
		arm_smmu_cmdq_issue_cmdlist(smmu, cmds->cmdq, cmds->cmds,	/* [한국어] 완료 대기 없이 내보낸다 — 마지막에 한 번만 기다리면 되기 때문이다. */
					    cmds->num, false);
		arm_smmu_cmdq_batch_init(smmu, cmds, cmd);	/* [한국어] 빈 묶음으로 이어 담는다. */
	}

	index = cmds->num * CMDQ_ENT_DWORDS;	/* [한국어] 담을 자리의 워드 오프셋. */
	if (unlikely(arm_smmu_cmdq_build_cmd(&cmds->cmds[index], cmd))) {	/* [한국어] 그 자리에 명령을 짓는다. */
		dev_warn(smmu->dev, "ignoring unknown CMDQ opcode 0x%x\n",	/* [한국어] 짓지 못하면 드라이버 버그다. */
			 cmd->opcode);
		return;	/* [한국어] 개수를 올리지 않아 이 명령은 없던 것이 된다. */
	}

	cmds->num++;	/* [한국어] 담긴 개수를 올린다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_batch_submit - 모아 둔 명령을 내보내고 완료까지 기다린다
 *
 * @smmu: 대상 하드웨어.
 * @cmds: 내보낼 묶음.
 * @return: 0 성공, 음수 오류.
 *
 * 묶음의 마지막 단계다. 완료 대기를 붙여 내보내므로, 돌아온 뒤에는 그
 * 명령들이 모두 하드웨어에 반영됐다고 믿을 수 있다.
 *
 * 실행 컨텍스트: 호출자에 따라 다르다. 잠들지 않는다.
 *
 * 호출 체인:
 *   무효화 경로 → [이 함수] → arm_smmu_cmdq_issue_cmdlist()
 */
static int arm_smmu_cmdq_batch_submit(struct arm_smmu_device *smmu,
				      struct arm_smmu_cmdq_batch *cmds)
{
	return arm_smmu_cmdq_issue_cmdlist(smmu, cmds->cmdq, cmds->cmds,	/* [한국어] 모은 것을 한 번에. */
					   cmds->num, true);	/* [한국어] 완료 대기를 붙인다. */
}

/*
 * [한국어]
 * arm_smmu_page_response - 페이지 폴트에 대한 답을 하드웨어에 돌려준다
 *
 * @dev: 폴트를 낸 장치.
 * @unused: 원본 폴트 기록 (여기서는 쓰지 않는다).
 * @resp: 상위 폴트 처리기가 정한 응답.
 *
 * SVA 처럼 폴트를 처리해 주는 경로에서, 페이지를 채웠거나 못 채웠음을
 * 하드웨어에 알린다. 멈춰 선 트랜잭션은 이 응답을 받아야 다시 굴러가거나
 * 끝나므로, 응답하지 않으면 그 장치가 영원히 멈춰 있게 된다.
 *
 * 마지막 주석이 설명하듯 완료 대기를 붙이지 않는다 — 이 명령은 "언젠가"
 * 트랜잭션을 끝내겠다는 약속일 뿐이라, 명령이 소비된 시점과 트랜잭션이
 * 실제로 끝나는 시점이 다르다. 그러니 기다려도 얻을 것이 없다.
 *
 * 실행 컨텍스트: 폴트 처리 작업 큐. 잠들 수 있다.
 *
 * 호출 체인:
 *   iopf 처리기 → iommu_ops->page_response = [이 함수]
 *     → arm_smmu_cmdq_issue_cmd()
 */
static void arm_smmu_page_response(struct device *dev, struct iopf_fault *unused,
				   struct iommu_page_response *resp)
{
	struct arm_smmu_cmdq_ent cmd = {0};	/* [한국어] 지을 명령. 0 으로 채워 시작한다. */
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 그 장치의 SMMU 쪽 상태. */
	int sid = master->streams[0].id;	/* [한국어] 멈춤을 쓰는 장치는 스트림이 하나라고 보고 첫 번째를 쓴다. */

	if (WARN_ON(!master->stall_enabled))	/* [한국어] 멈춤을 쓰지 않는 장치에는 답할 트랜잭션이 없다 — 왔다면 상위 계층의 버그다. */
		return;

	cmd.opcode		= CMDQ_OP_RESUME;	/* [한국어] 멈춘 트랜잭션을 다시 굴리거나 끝내는 명령. */
	cmd.resume.sid		= sid;	/* [한국어] 어느 장치의 트랜잭션인지. */
	cmd.resume.stag		= resp->grpid;	/* [한국어] 폴트 기록에 실려 왔던 멈춤 꼬리표 — 이 값이 트랜잭션의 신원이다. */
	switch (resp->code) {	/* [한국어] 상위가 정한 결과를 하드웨어 코드로 옮긴다. */
	case IOMMU_PAGE_RESP_INVALID:	/* [한국어] 요청 자체가 잘못됐다. */
	case IOMMU_PAGE_RESP_FAILURE:	/* [한국어] 페이지를 채워 줄 수 없다. */
		cmd.resume.resp = CMDQ_RESUME_0_RESP_ABORT;	/* [한국어] 둘 다 트랜잭션을 중단시킨다 — 다시 시도해도 같은 결과이기 때문이다. */
		break;
	case IOMMU_PAGE_RESP_SUCCESS:	/* [한국어] 페이지를 채웠다. */
		cmd.resume.resp = CMDQ_RESUME_0_RESP_RETRY;	/* [한국어] 다시 시도하게 한다 — 이번에는 변환이 성공할 것이다. */
		break;
	default:	/* [한국어] 모르는 코드. */
		break;	/* [한국어] resp 를 0 으로 둔다 — 아래 명령 짓기가 그 값을 그대로 쓴다. */
	}

	arm_smmu_cmdq_issue_cmd(master->smmu, &cmd);	/* [한국어] 완료를 기다리지 않고 보낸다. */
	/*
	 * Don't send a SYNC, it doesn't do anything for RESUME or PRI_RESP.
	 * RESUME consumption guarantees that the stalled transaction will be
	 * terminated... at some point in the future. PRI_RESP is fire and
	 * forget.
	 */
	/* [한국어] (위 영어 주석 참고) 완료 대기를 붙여도 얻을 것이 없다. 명령이
	 * 소비됐다는 사실은 "언젠가 트랜잭션이 끝난다"만 보장할 뿐, 그 시점을
	 * 알려 주지는 않기 때문이다. 기다리는 비용만 늘어난다. */
}

/* Invalidation array manipulation functions */
/*
 * [한국어]
 * arm_smmu_invs_iter_next - 무효화 배열에서 살아 있는 다음 항목을 찾는다
 *
 * @invs: 훑을 배열.
 * @next: 어디서부터 볼지.
 * @idx: 찾은 자리를 적어 줄 곳.
 * @return: 찾은 항목, 끝까지 없으면 NULL.
 *
 * 배열에는 참조 계수가 0 이 된 "버려진 자리"가 섞여 있다. 그것을 곧바로
 * 지우지 않는 이유는, 지우려면 배열을 옮겨야 하고 그 사이 무효화를 읽는
 * 쪽이 항목을 놓칠 수 있기 때문이다. 대신 훑을 때 건너뛴다.
 *
 * users 를 READ_ONCE 로 읽는 것이 중요하다 — 다른 CPU 가 지금 그 값을
 * 내리고 있을 수 있고, 컴파일러가 값을 캐시해 두면 무한 반복이 된다.
 *
 * 실행 컨텍스트: 무효화 경로와 배열 연산 양쪽. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_invs_for_each_entry 매크로 → [이 함수]
 */
static inline struct arm_smmu_inv *
arm_smmu_invs_iter_next(struct arm_smmu_invs *invs, size_t next, size_t *idx)
{
	while (true) {	/* [한국어] 살아 있는 항목을 만날 때까지. */
		if (next >= invs->num_invs) {	/* [한국어] 배열 끝을 넘었으면. */
			*idx = next;	/* [한국어] 끝 위치를 알려 준다 — 호출자의 반복이 여기서 멈춘다. */
			return NULL;
		}
		if (!READ_ONCE(invs->inv[next].users)) {	/* [한국어] 참조가 0 이면 버려진 자리다. */
			next++;	/* [한국어] 건너뛴다. */
			continue;	/* [한국어] 이 항목은 건너뛰고 다음으로 넘어간다. */
		}
		*idx = next;	/* [한국어] 찾은 자리를 알려 준다. */
		return &invs->inv[next];
	}
}

/**
 * arm_smmu_invs_for_each_entry - Iterate over all non-trash entries in invs
 * @invs: the base invalidation array
 * @idx: a stack variable of 'size_t', to store the array index
 * @cur: a stack variable of 'struct arm_smmu_inv *'
 */
/* [한국어] (위 영어 kernel-doc 참고) 버려진 자리를 건너뛰며 배열을 훑는 매크로.
 *
 * 첨자와 항목 포인터를 둘 다 필요로 하는 곳이 많아, 두 변수를 함께
 * 갱신하는 형태로 만들었다. */
#define arm_smmu_invs_for_each_entry(invs, idx, cur)                           \
	for (cur = arm_smmu_invs_iter_next(invs, 0, &(idx)); cur;              \
	     cur = arm_smmu_invs_iter_next(invs, idx + 1, &(idx)))	/* [한국어] 다음 살아 있는 항목으로 옮긴다 — 버려진 자리는 건너뛴다. */

/*
 * [한국어]
 * arm_smmu_inv_cmp - 무효화 항목 두 개의 순서를 정한다
 *
 * @inv_l: 왼쪽 항목.
 * @inv_r: 오른쪽 항목.
 * @return: 음수면 왼쪽이 앞, 0 이면 같음, 양수면 오른쪽이 앞.
 *
 * 배열을 정렬해 두면 병합과 해제가 두 배열을 한 번씩만 훑는 선형 연산이
 * 된다. 그 정렬 기준을 정하는 함수다.
 *
 * 비교 순서에 뜻이 있다 — 먼저 어느 하드웨어인지, 그다음 무효화 종류,
 * 그다음 대상 번호, 그리고 장치 캐시 무효화일 때만 PASID 까지 본다.
 * PASID 를 마지막에, 그것도 ATS 항목에서만 보는 이유는 TLB 무효화에는
 * PASID 개념이 없어 그 필드가 뜻을 갖지 않기 때문이다.
 *
 * 실행 컨텍스트: 배열 연산. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_invs_iter_next_cmp()/merge() → [이 함수]
 */
static int arm_smmu_inv_cmp(const struct arm_smmu_inv *inv_l,
			    const struct arm_smmu_inv *inv_r)
{
	if (inv_l->smmu != inv_r->smmu)	/* [한국어] 서로 다른 하드웨어의 항목이라면. */
		return cmp_int((uintptr_t)inv_l->smmu, (uintptr_t)inv_r->smmu);	/* [한국어] 포인터 값으로 순서를 정한다 — 값 자체에 뜻은 없고 일관되기만 하면 된다. */
	if (inv_l->type != inv_r->type)	/* [한국어] 무효화 종류가 다르면. */
		return cmp_int(inv_l->type, inv_r->type);	/* [한국어] 종류 번호 순. */
	if (inv_l->id != inv_r->id)	/* [한국어] 대상 번호(VMID/ASID/스트림)가 다르면. */
		return cmp_int(inv_l->id, inv_r->id);
	if (arm_smmu_inv_is_ats(inv_l))	/* [한국어] 장치 캐시 무효화일 때만. */
		return cmp_int(inv_l->ssid, inv_r->ssid);	/* [한국어] PASID 까지 본다 — 같은 장치의 다른 PASID 는 별개 항목이다. */
	return 0;	/* [한국어] 완전히 같은 항목 — 병합에서 참조 계수만 올린다. */
}

/*
 * [한국어]
 * arm_smmu_invs_iter_next_cmp - 두 정렬 배열을 나란히 훑으며 견준다
 *
 * @invs_l: 기준 배열 (버려진 자리가 섞여 있을 수 있다).
 * @next_l: 왼쪽에서 볼 자리.
 * @idx_l: 왼쪽에서 실제로 고른 자리를 적어 줄 곳.
 * @invs_r: 합치거나 걷어 낼 배열 (버려진 자리가 없다).
 * @next_r: 오른쪽에서 볼 자리.
 * @idx_r: 오른쪽 자리를 적어 줄 곳.
 * @return: 음수면 왼쪽만, 0 이면 양쪽 모두, 양수면 오른쪽만 다뤄야 한다.
 *
 * 정렬 병합의 한 걸음이다. 한쪽이 끝났으면 반대쪽을 계속 소진하도록
 * 값을 정해 돌려주는 것이 요령이다.
 *
 * 왼쪽만 iter_next 를 쓰는 이유가 주석에 있다 — 오른쪽 배열(장치가 들고
 * 다니는 build_invs)은 참조 계수를 쓰지 않아 "버려진 자리"라는 개념이
 * 아예 없기 때문이다.
 *
 * 실행 컨텍스트: 배열 연산. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_invs_for_each_cmp 매크로 → [이 함수] → arm_smmu_inv_cmp()
 */
static inline int arm_smmu_invs_iter_next_cmp(struct arm_smmu_invs *invs_l,
					      size_t next_l, size_t *idx_l,
					      struct arm_smmu_invs *invs_r,
					      size_t next_r, size_t *idx_r)
{
	struct arm_smmu_inv *cur_l =	/* [한국어] 왼쪽에서 살아 있는 다음 항목. */
		arm_smmu_invs_iter_next(invs_l, next_l, idx_l);

	/*
	 * We have to update the idx_r manually, because the invs_r cannot call
	 * arm_smmu_invs_iter_next() as the invs_r never sets any users counter.
	 */
	/* [한국어] (위 영어 주석 참고) 오른쪽 배열에는 참조 계수가 없어 건너뛸
	 * 항목이라는 개념이 없다. 그래서 그냥 다음 자리를 그대로 쓴다. */
	*idx_r = next_r;

	/*
	 * Compare of two sorted arrays items. If one side is past the end of
	 * the array, return the other side to let it run out the iteration.
	 *
	 * If the left entry is empty, return 1 to pick the right entry.
	 * If the right entry is empty, return -1 to pick the left entry.
	 */
	/* [한국어] (위 영어 주석 참고) 한쪽이 소진되면 반대쪽만 계속 고르게 만든다 —
	 * 그래야 남은 항목이 빠짐없이 처리된다. */
	if (!cur_l)	/* [한국어] 왼쪽이 끝났으면. */
		return 1;	/* [한국어] 오른쪽만 고른다. */
	if (next_r >= invs_r->num_invs)	/* [한국어] 오른쪽이 끝났으면. */
		return -1;	/* [한국어] 왼쪽만 고른다. */
	return arm_smmu_inv_cmp(cur_l, &invs_r->inv[next_r]);	/* [한국어] 둘 다 남아 있으면 순서를 견준다. */
}

/**
 * arm_smmu_invs_for_each_cmp - Iterate over two sorted arrays computing for
 *                              arm_smmu_invs_merge() or arm_smmu_invs_unref()
 * @invs_l: the base invalidation array
 * @idx_l: a stack variable of 'size_t', to store the base array index
 * @invs_r: the build_invs array as to_merge or to_unref
 * @idx_r: a stack variable of 'size_t', to store the build_invs index
 * @cmp: a stack variable of 'int', to store return value (-1, 0, or 1)
 */
/* [한국어] (위 영어 kernel-doc 참고) 두 정렬 배열을 나란히 훑는 매크로.
 *
 * 견준 결과에 따라 어느 쪽 첨자를 올릴지 정하는 것이 핵심이다. 왼쪽이
 * 앞서면(cmp<0) 왼쪽만, 오른쪽이 앞서면(cmp>0) 오른쪽만, 같으면(cmp==0)
 * 양쪽을 함께 올린다. 이 규칙 하나로 병합과 해제가 모두 표현된다. */
#define arm_smmu_invs_for_each_cmp(invs_l, idx_l, invs_r, idx_r, cmp)          \
	for (idx_l = idx_r = 0,                                                \
	     cmp = arm_smmu_invs_iter_next_cmp(invs_l, 0, &(idx_l),            \
					       invs_r, 0, &(idx_r));           \
	     idx_l < invs_l->num_invs || idx_r < invs_r->num_invs;             \
	     cmp = arm_smmu_invs_iter_next_cmp(                                \
		     invs_l, idx_l + (cmp <= 0 ? 1 : 0), &(idx_l),             \
		     invs_r, idx_r + (cmp >= 0 ? 1 : 0), &(idx_r)))	/* [한국어] 견준 결과에 따라 한쪽 또는 양쪽 첨자를 올린다. */

/**
 * arm_smmu_invs_merge() - Merge @to_merge into @invs and generate a new array
 * @invs: the base invalidation array
 * @to_merge: an array of invalidations to merge
 *
 * Return: a newly allocated array on success, or ERR_PTR
 *
 * This function must be locked and serialized with arm_smmu_invs_unref() and
 * arm_smmu_invs_purge(), but do not lockdep on any lock for KUNIT test.
 *
 * Both @invs and @to_merge must be sorted, to ensure the returned array will be
 * sorted as well.
 *
 * Caller is responsible for freeing the @invs and the returned new one.
 *
 * Entries marked as trash will be purged in the returned array.
 */
/*
 * [한국어]
 * arm_smmu_invs_merge - 무효화 항목들을 합쳐 새 배열을 만든다
 *
 * @invs: 지금 걸려 있는 배열.
 * @to_merge: 합칠 항목들 (장치가 들고 다니는 build_invs).
 * @return: 새로 잡은 배열, 실패하면 ERR_PTR.
 *
 * (위 영어 kernel-doc 참고) 장치를 도메인에 붙일 때 그 장치가 필요로 하는
 * 무효화 대상을 도메인의 배열에 더하는 함수다.
 *
 * 제자리에서 고치지 않고 새 배열을 만드는 이유가 중요하다. 무효화를 읽는
 * 쪽은 RCU 로 락 없이 배열을 훑는데, 제자리에서 크기를 늘리면 그 훑기가
 * 어긋난다. 새 배열을 만들어 통째로 갈아 끼우면 읽는 쪽은 옛 배열이나
 * 새 배열 중 하나를 온전히 보게 된다.
 *
 * 두 번 훑는 것도 그래서다 — 먼저 세어 크기를 정하고, 그다음 채운다.
 * 이미 있던 항목은 참조 계수만 올리고, 새 항목은 계수 1 로 들어간다.
 * 버려진 자리는 iter_next 가 건너뛰므로 자연스럽게 정리된다.
 *
 * 실행 컨텍스트: 붙이기 경로(prepare 단계). 잠들 수 있다 — 실패할 수 있는
 * 일을 여기서 끝내야 commit 이 실패하지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare() → [이 함수] → arm_smmu_invs_alloc()
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 함수를 직접 부를 수 있게 연다. */
struct arm_smmu_invs *arm_smmu_invs_merge(struct arm_smmu_invs *invs,
					  struct arm_smmu_invs *to_merge)
{
	struct arm_smmu_invs *new_invs;	/* [한국어] 만들 새 배열. */
	struct arm_smmu_inv *new;	/* [한국어] 채워 넣을 자리를 가리키는 커서. */
	size_t num_invs = 0;	/* [한국어] 합친 뒤의 항목 수. */
	size_t i, j;	/* [한국어] 두 배열의 첨자. */
	int cmp;	/* [한국어] 견준 결과. */

	arm_smmu_invs_for_each_cmp(invs, i, to_merge, j, cmp)	/* [한국어] 첫 번째 훑기 — 세기만 한다. */
		num_invs++;	/* [한국어] 한 걸음이 결과 항목 하나에 대응한다. */

	new_invs = arm_smmu_invs_alloc(num_invs);	/* [한국어] 정확한 크기로 새 배열을 잡는다. */
	if (!new_invs)	/* [한국어] 메모리가 없으면. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 붙이기가 접힌다 — 아직 하드웨어를 건드리기 전이라 안전하다. */

	new = new_invs->inv;	/* [한국어] 채우기 커서를 처음에 놓는다. */
	arm_smmu_invs_for_each_cmp(invs, i, to_merge, j, cmp) {	/* [한국어] 두 번째 훑기 — 실제로 채운다. 같은 순서로 도므로 개수가 정확히 맞는다. */
		if (cmp < 0) {	/* [한국어] 기존 배열에만 있는 항목. */
			*new = invs->inv[i];	/* [한국어] 그대로 옮긴다 — 참조 계수도 그대로다. */
		} else if (cmp == 0) {	/* [한국어] 양쪽에 다 있는 항목. */
			*new = invs->inv[i];	/* [한국어] 기존 것을 옮기고. */
			WRITE_ONCE(new->users, READ_ONCE(new->users) + 1);	/* [한국어] 참조 계수를 하나 올린다 — 이 항목을 필요로 하는 장치가 하나 늘었다. */
		} else {	/* [한국어] 새로 더해지는 항목. */
			*new = to_merge->inv[j];	/* [한국어] 새 항목을 옮기고. */
			WRITE_ONCE(new->users, 1);	/* [한국어] 참조 계수를 1 로 시작한다 — build_invs 쪽 값은 뜻이 없다. */
		}

		/*
		 * Check that the new array is sorted. This also validates that
		 * to_merge is sorted.
		 */
		/* [한국어] (위 영어 주석 참고) 결과가 정렬되어 있는지 확인한다. 이 검사가
		 * 깨지면 입력 배열이 정렬되어 있지 않았다는 뜻이고, 그러면 이후의
		 * 병합·해제가 모두 어긋난다. */
		if (new != new_invs->inv)	/* [한국어] 첫 항목이 아니면 앞 항목과 견줄 수 있다. */
			WARN_ON_ONCE(arm_smmu_inv_cmp(new - 1, new) == 1);	/* [한국어] 앞이 뒤보다 크면 정렬이 깨진 것이다. */
		if (arm_smmu_inv_is_ats(new))	/* [한국어] 장치 캐시 무효화 항목이 하나라도 있으면. */
			new_invs->has_ats = true;	/* [한국어] 배열에 표시해 둔다 — 무효화 경로가 이 값으로 빠른 길을 고른다. */
		new++;	/* [한국어] 다음 자리로. */
	}

	WARN_ON(new != new_invs->inv + new_invs->num_invs);	/* [한국어] 세기와 채우기의 개수가 어긋나면 두 훑기가 다르게 돈 것이다 — 있어서는 안 될 일이다. */

	return new_invs;	/* [한국어] 호출자가 RCU 로 도메인에 갈아 끼운다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_invs_merge);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/**
 * arm_smmu_invs_unref() - Find in @invs for all entries in @to_unref, decrease
 *                         the user counts without deletions
 * @invs: the base invalidation array
 * @to_unref: an array of invalidations to decrease their user counts
 *
 * Return: the number of trash entries in the array, for arm_smmu_invs_purge()
 *
 * This function will not fail. Any entry with users=0 will be marked as trash,
 * and caller will be notified about the trashed entry via @to_unref by setting
 * a users=0.
 *
 * All tailing trash entries in the array will be dropped. And the size of the
 * array will be trimmed properly. All trash entries in-between will remain in
 * the @invs until being completely deleted by the next arm_smmu_invs_merge()
 * or an arm_smmu_invs_purge() function call.
 *
 * This function must be locked and serialized with arm_smmu_invs_merge() and
 * arm_smmu_invs_purge(), but do not lockdep on any mutex for KUNIT test.
 *
 * Note that the final @invs->num_invs might not reflect the actual number of
 * invalidations due to trash entries. Any reader should take the read lock to
 * iterate each entry and check its users counter till the last entry.
 */
/*
 * [한국어]
 * arm_smmu_invs_unref - 무효화 항목들의 참조를 내리고 버려진 자리로 표시한다
 *
 * @invs: 지금 걸려 있는 배열 (제자리에서 고쳐진다).
 * @to_unref: 참조를 내릴 항목들. 결과가 여기 되돌아 적힌다.
 *
 * (위 영어 kernel-doc 참고) 장치를 도메인에서 뗄 때 그 장치가 필요로 하던
 * 무효화 대상을 걷어 내는 함수다.
 *
 * 병합과 달리 새 배열을 만들지 않고 제자리에서 고친다. 그 이유가 이
 * 함수의 존재 이유이기도 하다 — 떼기는 실패하면 안 되는 자리에서 불린다.
 * 메모리를 잡아야 한다면 실패할 수 있고, 실패하면 하드웨어는 이미 떨어진
 * 장치를 커널만 붙어 있다고 여기는 어긋남이 생긴다.
 *
 * 제자리 수정의 대가로 "버려진 자리"가 생긴다. 참조가 0 이 된 항목을
 * 곧바로 지우려면 배열을 옮겨야 하는데, 그러면 RCU 로 락 없이 읽는 쪽이
 * 항목을 놓친다. 그래서 계수만 0 으로 두고 자리는 남긴다 — 읽는 쪽은
 * 계수가 0 인 항목을 건너뛴다.
 *
 * 다만 배열 꼬리의 버려진 자리는 곧바로 잘라 낼 수 있다. 길이만 줄이면
 * 되고, 읽는 쪽이 그 자리를 보더라도 이미 계수가 0 이라 건너뛰기 때문이다.
 * 그 길이 갱신만 rwlock 으로 감싸는데, 장치 캐시 무효화 경로와 겹치기
 * 때문이다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로(commit 단계). 스핀락을 잡는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_commit() → [이 함수]
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 직접 부를 수 있게 연다. */
void arm_smmu_invs_unref(struct arm_smmu_invs *invs,
			 struct arm_smmu_invs *to_unref)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장 자리. */
	size_t num_invs = 0;	/* [한국어] 꼬리를 잘라 낸 뒤의 길이 — 마지막으로 살아 있던 항목 다음 자리다. */
	size_t i, j;	/* [한국어] 두 배열의 첨자. */
	int cmp;	/* [한국어] 견준 결과. */

	arm_smmu_invs_for_each_cmp(invs, i, to_unref, j, cmp) {	/* [한국어] 두 정렬 배열을 나란히 훑는다. */
		if (cmp < 0) {	/* [한국어] 기존 배열에만 있는 항목 — 걷어 낼 대상이 아니다. */
			/* not found in to_unref, leave alone */
			num_invs = i + 1;	/* [한국어] 살아 있으므로 여기까지는 잘라 낼 수 없다. */
		} else if (cmp == 0) {	/* [한국어] 양쪽에 다 있는 항목 — 참조를 내릴 대상이다. */
			int users = READ_ONCE(invs->inv[i].users) - 1;	/* [한국어] 내린 뒤의 값을 미리 계산한다. */

			if (WARN_ON(users < 0))	/* [한국어] 0 인 항목을 또 내리려 한다면 붙이기·떼기 짝이 어긋난 것이다. */
				continue;	/* [한국어] 더 망가뜨리지 않고 넘어간다. */

			/* same item */
			WRITE_ONCE(invs->inv[i].users, users);	/* [한국어] 읽는 쪽이 락 없이 보므로 WRITE_ONCE 로 쓴다. */
			if (users) {	/* [한국어] 아직 이 항목을 쓰는 장치가 남아 있으면. */
				WRITE_ONCE(to_unref->inv[j].users, 1);	/* [한국어] 호출자에게 "이 항목은 아직 살아 있다"고 알린다. */
				num_invs = i + 1;	/* [한국어] 살아 있으므로 여기까지는 자를 수 없다. */
				continue;	/* [한국어] 이 항목은 건너뛰고 다음으로 넘어간다. */
			}

			/* Notify the caller about the trash entry */
			/* [한국어] (위 영어 주석 참고) 계수가 0 이 된 항목을 호출자에게 알린다 —
			 * 호출자는 그 정보로 무엇이 실제로 사라졌는지 안다. */
			WRITE_ONCE(to_unref->inv[j].users, 0);	/* [한국어] "이 항목은 버려졌다"는 표시. */
			invs->num_trashes++;	/* [한국어] 버려진 자리를 센다 — 많아지면 정리(purge)를 부른다. */
		} else {	/* [한국어] 걷어 내려는 항목이 배열에 없는 경우. */
			/* item in to_unref is not in invs or already a trash */
			WARN_ON(true);	/* [한국어] (위 영어 주석 참고) 붙이기와 떼기가 짝이 맞지 않는다는 뜻 — 코드 버그다. */
		}
	}

	/* Exclude any tailing trash */
	/* [한국어] (위 영어 주석 참고) 꼬리에 몰린 버려진 자리는 아래에서 길이를
	 * 줄여 통째로 잘라 내므로, 세어 둔 개수에서도 빼 준다. */
	invs->num_trashes -= invs->num_invs - num_invs;	/* [한국어] 잘려 나갈 만큼 빼면 남는 것이 가운데의 버려진 자리 수다. */

	/* The lock is required to fence concurrent ATS operations. */
	/* [한국어] (위 영어 주석 참고) 장치 캐시 무효화는 배열을 훑으며 장치에 명령을
	 * 보내는데, 그 도중에 길이가 줄면 이미 떨어진 장치에 명령을 보내게 된다.
	 * 그 경로가 읽기 락을 잡으므로 여기서는 쓰기 락으로 막는다. */
	write_lock_irqsave(&invs->rwlock, flags);	/* [한국어] 인터럽트 문맥에서도 이 배열을 읽으므로 irqsave 가 필요하다. */
	WRITE_ONCE(invs->num_invs, num_invs); /* Remove tailing trash entries */	/* [한국어] (위 영어 주석 참고) 길이만 줄여 꼬리를 잘라 낸다 — 메모리는 그대로 둔다. */
	write_unlock_irqrestore(&invs->rwlock, flags);	/* [한국어] 길이 갱신이 끝났으니 쓰기 락을 놓는다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_invs_unref);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/**
 * arm_smmu_invs_purge() - Purge all the trash entries in the @invs
 * @invs: the base invalidation array
 *
 * Return: a newly allocated array on success removing all the trash entries, or
 *         NULL if there is no trash entry in the array or if allocation failed
 *
 * This function must be locked and serialized with arm_smmu_invs_merge() and
 * arm_smmu_invs_unref(), but do not lockdep on any lock for KUNIT test.
 *
 * Caller is responsible for freeing the @invs and the returned new one.
 */
/*
 * [한국어]
 * arm_smmu_invs_purge - 버려진 자리를 걷어 낸 새 배열을 만든다
 *
 * @invs: 지금 걸려 있는 배열.
 * @return: 정리된 새 배열, 정리할 것이 없거나 메모리가 없으면 NULL.
 *
 * (위 영어 kernel-doc 참고) 버려진 자리가 쌓이면 무효화할 때마다 그 자리를
 * 훑고 건너뛰는 헛수고가 늘어난다. 그것이 눈에 띌 만큼 쌓였을 때 이 함수로
 * 정리한다.
 *
 * NULL 을 실패가 아니라 "지금은 정리하지 않는다"로 쓰는 것이 특징이다.
 * 정리는 성능을 위한 선택일 뿐 정확성에 필요하지 않으므로, 메모리가 없으면
 * 그냥 다음 기회로 미룬다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_commit() 등 → [이 함수] → arm_smmu_invs_alloc()
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 직접 부를 수 있게 연다. */
struct arm_smmu_invs *arm_smmu_invs_purge(struct arm_smmu_invs *invs)
{
	struct arm_smmu_invs *new_invs;	/* [한국어] 만들 새 배열. */
	struct arm_smmu_inv *inv;	/* [한국어] 훑기 커서. */
	size_t i, num_invs = 0;	/* [한국어] 첨자와 실제로 옮긴 개수. */

	if (WARN_ON(invs->num_invs < invs->num_trashes))	/* [한국어] 버려진 자리가 전체보다 많다면 계수가 망가진 것이다. */
		return NULL;
	if (!invs->num_invs || !invs->num_trashes)	/* [한국어] 비었거나 정리할 것이 없으면. */
		return NULL;	/* [한국어] 새 배열을 만들 이유가 없다. */

	new_invs = arm_smmu_invs_alloc(invs->num_invs - invs->num_trashes);	/* [한국어] 살아 있는 항목 수만큼만 잡는다. */
	if (!new_invs)	/* [한국어] 메모리가 없으면. */
		return NULL;	/* [한국어] 정리를 미룬다 — 실패가 아니라 연기다. */

	arm_smmu_invs_for_each_entry(invs, i, inv) {	/* [한국어] 살아 있는 항목만 훑는다 — 버려진 자리는 자동으로 건너뛴다. */
		new_invs->inv[num_invs] = *inv;	/* [한국어] 그대로 옮긴다. */
		if (arm_smmu_inv_is_ats(inv))	/* [한국어] 장치 캐시 무효화 항목이 있으면. */
			new_invs->has_ats = true;	/* [한국어] 새 배열에도 표시를 이어 준다. */
		num_invs++;	/* [한국어] 결과 항목을 하나 세거나 채웠다. */
	}

	WARN_ON(num_invs != new_invs->num_invs);	/* [한국어] 미리 계산한 크기와 실제로 옮긴 수가 다르면 계수가 어긋난 것이다. */
	return new_invs;	/* [한국어] 호출자가 RCU 로 갈아 끼우고 옛 배열을 놓는다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_invs_purge);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/* Context descriptor manipulation functions */

/*
 * Based on the value of ent report which bits of the STE the HW will access. It
 * would be nice if this was complete according to the spec, but minimally it
 * has to capture the bits this driver uses.
 */
/*
 * [한국어]
 * arm_smmu_get_ste_used - 이 STE 설정이 실제로 읽는 비트를 계산한다
 *
 * @ent: 검사할 항목.
 * @used_bits: 계산 결과를 담을 마스크 (8워드).
 *
 * (위 영어 주석 참고) 끊김 없는 항목 갱신의 토대가 되는 계산이다. 하드웨어가
 * 읽지 않는 비트는 무엇이 들어 있든 동작에 영향이 없으므로, 그런 비트는
 * 아무 때나 바꿔도 안전하다. 반대로 읽는 비트를 잘못 바꾸면 하드웨어가
 * 엉뚱한 설정을 보게 된다.
 *
 * 계산의 뼈대는 설정 갈래(cfg)다. 유효 비트가 꺼져 있으면 하드웨어는 그
 * 비트 하나만 보고 나머지를 아예 읽지 않는다. 켜져 있으면 갈래에 따라
 * 1단계 관련 필드(비트 0), 2단계 관련 필드(비트 1)를 더한다. 중첩 변환은
 * 두 비트가 모두 서 있어 양쪽 필드를 다 읽는다.
 *
 * 규격 전체를 완벽히 옮기지는 않았다고 주석이 밝히는데, 이 드라이버가
 * 실제로 쓰는 비트만 담으면 충분하기 때문이다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → ops->get_used = [이 함수]
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 계산을 직접 검증할 수 있게 연다. */
void arm_smmu_get_ste_used(const __le64 *ent, __le64 *used_bits)
{
	unsigned int cfg = FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(ent[0]));	/* [한국어] 설정 갈래 — 이 값이 어느 필드를 읽을지 정한다. */

	used_bits[0] = cpu_to_le64(STRTAB_STE_0_V);	/* [한국어] 유효 비트는 언제나 읽힌다 — 그것이 나머지를 읽을지 정하기 때문이다. */
	if (!(ent[0] & cpu_to_le64(STRTAB_STE_0_V)))	/* [한국어] 유효하지 않은 항목이라면. */
		return;	/* [한국어] 하드웨어가 나머지를 읽지 않으므로 그 비트들은 자유롭게 바꿔도 된다. */

	used_bits[0] |= cpu_to_le64(STRTAB_STE_0_CFG);	/* [한국어] 유효하다면 갈래 필드도 읽힌다. */

	/* S1 translates */
	/* [한국어] (위 영어 주석 참고) 갈래의 비트 0 이 서 있으면 1단계 변환을 한다. */
	if (cfg & BIT(0)) {
		used_bits[0] |= cpu_to_le64(STRTAB_STE_0_S1FMT |	/* [한국어] 문맥 서술자 표의 형식. */
					    STRTAB_STE_0_S1CTXPTR_MASK |	/* [한국어] 그 표의 주소 — 잘못 바꾸면 엉뚱한 메모리를 서술자로 읽는다. */
					    STRTAB_STE_0_S1CDMAX);	/* [한국어] 표가 담는 서술자 수. */
		used_bits[1] |=	/* [한국어] 1단계 변환이 읽는 둘째 워드 필드들. */
			cpu_to_le64(STRTAB_STE_1_S1DSS | STRTAB_STE_1_S1CIR |	/* [한국어] PASID 기본 동작과 서술자 표를 읽을 때의 캐시 정책. */
				    STRTAB_STE_1_S1COR | STRTAB_STE_1_S1CSH |	/* [한국어] 바깥 캐시 정책과 공유 영역. */
				    STRTAB_STE_1_S1STALLD | STRTAB_STE_1_STRW |	/* [한국어] 멈춤 허용 여부와 어느 세계의 변환인지. */
				    STRTAB_STE_1_EATS | STRTAB_STE_1_MEV);	/* [한국어] ATS 처리 방식과 이벤트 병합. */
		used_bits[2] |= cpu_to_le64(STRTAB_STE_2_S2VMID);	/* [한국어] 1단계만 하는 경우에도 VMID 는 TLB 태그로 쓰인다. */

		/*
		 * See 13.5 Summary of attribute/permission configuration fields
		 * for the SHCFG behavior.
		 */
		/* [한국어] (위 영어 주석 참고) 공유 속성 필드는 특정 설정에서만 읽힌다.
		 * 서술자 없는 PASID 를 우회시키는 설정일 때, 그 우회 트래픽의 공유
		 * 속성을 이 필드가 정하기 때문이다. */
		if (FIELD_GET(STRTAB_STE_1_S1DSS, le64_to_cpu(ent[1])) ==	/* [한국어] 그 설정인지 확인한다. */
		    STRTAB_STE_1_S1DSS_BYPASS)
			used_bits[1] |= cpu_to_le64(STRTAB_STE_1_SHCFG);	/* [한국어] 그때만 이 필드가 읽힌다. */
	}

	/* S2 translates */
	/* [한국어] (위 영어 주석 참고) 갈래의 비트 1 이 서 있으면 2단계 변환을 한다. */
	if (cfg & BIT(1)) {
		used_bits[1] |=	/* [한국어] 2단계 변환이 읽는 둘째 워드 필드들. */
			cpu_to_le64(STRTAB_STE_1_S2FWB | STRTAB_STE_1_EATS |	/* [한국어] 캐시 속성 강제와 ATS 처리. */
				    STRTAB_STE_1_SHCFG | STRTAB_STE_1_MEV);	/* [한국어] 공유 속성과 이벤트 병합. */
		used_bits[2] |=	/* [한국어] 2단계 변환이 읽는 셋째 워드 필드들. */
			cpu_to_le64(STRTAB_STE_2_S2VMID | STRTAB_STE_2_VTCR |	/* [한국어] VMID 와 2단계 변환 제어. */
				    STRTAB_STE_2_S2AA64 | STRTAB_STE_2_S2ENDI |	/* [한국어] 64비트 형식과 엔디안. */
				    STRTAB_STE_2_S2PTW | STRTAB_STE_2_S2S |	/* [한국어] 1단계 표 순회도 2단계를 거치는지, 폴트 때 멈출지. */
				    STRTAB_STE_2_S2R);	/* [한국어] 폴트를 보고할지. */
		used_bits[3] |= cpu_to_le64(STRTAB_STE_3_S2TTB_MASK);	/* [한국어] 2단계 페이지 테이블의 주소 — 가장 위험한 필드다. */
	}

	if (cfg == STRTAB_STE_0_CFG_BYPASS)	/* [한국어] 변환을 아예 하지 않는 우회 설정이라면. */
		used_bits[1] |= cpu_to_le64(STRTAB_STE_1_SHCFG);	/* [한국어] 그 통과 트래픽의 공유 속성만 읽힌다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_get_ste_used);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * [한국어]
 * arm_smmu_get_ste_update_safe - 갱신 도중 달라져도 안전한 비트를 계산한다
 *
 * @cur: 지금 항목.
 * @target: 도달할 항목.
 * @safe_bits: 결과를 담을 마스크.
 *
 * "읽히는 비트"만으로 판정하면 지나치게 엄격해져, 실제로는 안전한 전환도
 * 여러 단계로 쪼개진다. 이 함수는 그 판정을 완화할 수 있는 두 경우를
 * 짚어 낸다.
 *
 * 첫째는 EATS_TRANS 다. 이 필드는 신뢰할 수 없는 장치가 PCI 설정에서 ATS 가
 * 꺼져 있는데도 ATS 요청을 보내는 것을 막는 보호 장치인데, 붙이기 코드가
 * 이 값을 바꾸는 시점에는 이미 PCI 쪽 ATS 를 꺼 두었으므로 그 보호가
 * 필요 없다. 다만 S2S 와 함께 바꿀 때는 중간에 규격이 금지한 조합이
 * 나올 수 있어 그 경우는 제외한다.
 *
 * 둘째는 MEV 다. 이벤트를 몇 개로 합쳐 보고하느냐만 달라질 뿐 동작이
 * 바뀌지 않으며, 규격 자체가 "MEV 가 0 이어도 합쳐진 기록이 올 수 있다"고
 * 못 박고 있어 언제나 안전하다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → ops->get_update_safe = [이 함수]
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 계산을 검증할 수 있게 연다. */
void arm_smmu_get_ste_update_safe(const __le64 *cur, const __le64 *target,
				  __le64 *safe_bits)
{
	const __le64 eats_s1chk =	/* [한국어] "1단계로 검사" 설정 값 — 아래에서 이 값이 관여하면 완화를 포기한다. */
		FIELD_PREP(STRTAB_STE_1_EATS, STRTAB_STE_1_EATS_S1CHK);
	const __le64 eats_trans =	/* [한국어] "변환 허용" 설정 값 — 완화 대상 비트다. */
		FIELD_PREP(STRTAB_STE_1_EATS, STRTAB_STE_1_EATS_TRANS);

	/*
	 * When an STE changes EATS_TRANS, the sequencing code in the attach
	 * logic already will have the PCI cap for ATS disabled. Thus at this
	 * moment we can expect that the device will not generate ATS queries
	 * and so we don't care about the sequencing of EATS. The purpose of
	 * EATS_TRANS is to protect the system from hostile untrusted devices
	 * that issue ATS when the PCI config space is disabled. However, if
	 * EATS_TRANS is being changed, then we must have already trusted the
	 * device as the EATS_TRANS security block is being disabled.
	 *
	 *  Note: now the EATS_TRANS update is moved to the first entry_set().
	 *  Changing S2S and EATS might transiently result in S2S=1 and EATS=1
	 *  which is a bad STE (see "5.2 Stream Table Entry"). In such a case,
	 *  we can't do a hitless update. Also, it should not be added to the
	 *  safe bits with STRTAB_STE_1_EATS_S1CHK, because EATS=0b11 would be
	 *  effectively an errant 0b00 configuration.
	 */
	/* [한국어] (위 영어 주석 참고) EATS 는 신뢰할 수 없는 장치를 막는 보호 장치인데,
	 * 이 값을 바꾸는 시점에는 붙이기 코드가 이미 PCI 쪽 ATS 를 꺼 두었으므로
	 * 장치가 ATS 요청을 낼 수 없다. 곧 이 순간의 순서는 뜻이 없다.
	 * 다만 두 예외가 있다 — S2S 와 함께 바뀌면 중간에 규격이 금지한 조합
	 * (S2S=1, EATS=1)이 나올 수 있고, S1CHK 값이 얽히면 두 비트가 다 서서
	 * 사실상 잘못된 설정이 된다. 그 경우는 완화하지 않는다. */
	if (!((cur[1] | target[1]) & cpu_to_le64(eats_s1chk)) &&	/* [한국어] 양쪽 어디에도 S1CHK 가 없고. */
	    !((cur[2] | target[2]) & cpu_to_le64(STRTAB_STE_2_S2S))) 	/* [한국어] 멈춤 설정도 얽히지 않았다면. */
		safe_bits[1] |= cpu_to_le64(eats_trans);	/* [한국어] 이 비트는 갱신 도중 달라져도 안전하다고 표시한다. */

	/*
	 * MEV does not meaningfully impact the operation of the HW, it only
	 * changes how many fault events are generated, thus we can relax it
	 * when computing the ordering. The spec notes the device can act like
	 * MEV=1 anyhow:
	 *
	 *  Note: Software must expect, and be able to deal with, coalesced
	 *  fault records even when MEV == 0.
	 */
	/* [한국어] (위 영어 주석 참고) 이벤트 병합 여부는 폴트 기록이 몇 개로 합쳐
	 * 보고되느냐만 바꾼다. 규격이 "MEV 가 0 이어도 합쳐진 기록이 올 수 있다"고
	 * 못 박고 있으므로, 드라이버는 어차피 두 경우를 모두 감당해야 한다.
	 * 따라서 이 비트는 언제나 자유롭게 바꿔도 된다. */
	safe_bits[1] |= cpu_to_le64(STRTAB_STE_1_MEV);	/* [한국어] 조건 없이 완화한다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_get_ste_update_safe);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * Figure out if we can do a hitless update of entry to become target. Returns a
 * bit mask where 1 indicates that qword needs to be set disruptively.
 * unused_update is an intermediate value of entry that has unused bits set to
 * their new values.
 */
/*
 * [한국어]
 * arm_smmu_entry_qword_diff - 끊김 없는 갱신이 가능한지 판정한다
 *
 * @writer: 항목 기록기 (사용 비트 계산 갈고리를 들고 있다).
 * @entry: 지금 항목.
 * @target: 도달할 항목.
 * @unused_update: 중간 단계 값을 담을 자리 — 읽히지 않는 비트만 새 값으로 바꾼 모습.
 * @return: 워드별 비트마스크. 1 이면 그 워드의 읽히는 비트를 바꿔야 한다.
 *
 * (위 영어 주석 참고) 끊김 없는 갱신의 핵심 계산이다. 착상은 이렇다 —
 * 지금 설정이 읽지 않는 비트는 아무 때나 새 값으로 바꿔도 하드웨어가
 * 눈치채지 못한다. 그렇게 먼저 바꿔 두면, 남는 것은 "읽히는 비트"의 차이뿐이다.
 * 그 차이가 워드 하나에만 몰려 있으면, 그 워드를 64비트 원자 쓰기 한 번으로
 * 바꿔 전환을 끝낼 수 있다 — 하드웨어는 옛 설정이나 새 설정 중 하나만 본다.
 *
 * 반환값의 비트 수를 세어 그 판정을 한다. 1 이면 끊김 없이 가능하고,
 * 2 이상이면 유효 비트를 내렸다 올리는 끊김 있는 갱신이 필요하다.
 *
 * WARN_ON_ONCE 검사도 중요하다 — 항목을 짓는 함수가 "읽히지 않는다"고
 * 계산된 비트를 1 로 세워 두면, 그 비트는 영원히 정리되지 않고 남는다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → [이 함수] → ops->get_used()/get_update_safe()
 */
static u8 arm_smmu_entry_qword_diff(struct arm_smmu_entry_writer *writer,
				    const __le64 *entry, const __le64 *target,
				    __le64 *unused_update)
{
	__le64 target_used[NUM_ENTRY_QWORDS] = {};	/* [한국어] 도착 설정이 읽는 비트. */
	__le64 cur_used[NUM_ENTRY_QWORDS] = {};	/* [한국어] 지금 설정이 읽는 비트. */
	__le64 safe[NUM_ENTRY_QWORDS] = {};	/* [한국어] 갱신 도중 달라져도 안전한 비트. */
	u8 used_qword_diff = 0;	/* [한국어] 결과 — 워드마다 한 비트씩. */
	unsigned int i;	/* [한국어] 워드 반복자. */

	writer->ops->get_used(entry, cur_used);	/* [한국어] 지금 설정 기준으로 계산한다. */
	writer->ops->get_used(target, target_used);	/* [한국어] 도착 설정 기준으로도 계산한다. */
	if (writer->ops->get_update_safe)	/* [한국어] CD 기록기에는 이 갈고리가 없다. */
		writer->ops->get_update_safe(entry, target, safe);	/* [한국어] 완화할 수 있는 비트를 찾는다. */

	for (i = 0; i != NUM_ENTRY_QWORDS; i++) {	/* [한국어] 워드마다 따로 판정한다 — 원자 쓰기 단위가 64비트이기 때문이다. */
		/*
		 * Safe is only used for bits that are used by both entries,
		 * otherwise it is sequenced according to the unused entry.
		 */
		/* [한국어] (위 영어 주석 참고) 한쪽에서만 읽히는 비트는 이미 "읽히지 않는
		 * 쪽"의 규칙으로 자유롭게 다뤄지므로 완화가 필요 없다. 양쪽 모두
		 * 읽는 비트에만 완화를 적용한다. */
		safe[i] &= target_used[i] & cur_used[i];	/* [한국어] 교집합으로 좁힌다. */

		/*
		 * Check that masks are up to date, the make functions are not
		 * allowed to set a bit to 1 if the used function doesn't say it
		 * is used.
		 */
		/* [한국어] (위 영어 주석 참고) 항목을 짓는 함수와 사용 비트를 계산하는
		 * 함수가 어긋나면, 세워 둔 비트가 영원히 정리되지 않고 남는다.
		 * 그 어긋남을 여기서 잡는다. */
		WARN_ON_ONCE(target[i] & ~target_used[i]);	/* [한국어] 읽히지 않는 자리에 1 이 서 있으면 안 된다. */

		/* Bits can change because they are not currently being used */
		/* [한국어] (위 영어 주석 참고) 완화된 비트는 "지금 읽히는" 목록에서 빼,
		 * 자유롭게 바꿀 수 있는 쪽으로 옮긴다. */
		cur_used[i] &= ~safe[i];
		unused_update[i] = (entry[i] & cur_used[i]) |	/* [한국어] 읽히는 비트는 지금 값 그대로 두고. */
				   (target[i] & ~cur_used[i]);	/* [한국어] 읽히지 않는 비트만 새 값으로 바꾼 중간 모습을 만든다. */
		/*
		 * Each bit indicates that a used bit in a qword needs to be
		 * changed after unused_update is applied.
		 */
		/* [한국어] (위 영어 주석 참고) 중간 모습을 적용하고 나서도 도착 값과
		 * 다르다면, 그 워드는 읽히는 비트를 바꿔야 한다는 뜻이다. */
		if ((unused_update[i] & target_used[i]) != target[i])	/* [한국어] 도착이 읽는 비트만 견준다. */
			used_qword_diff |= 1 << i;	/* [한국어] 그 워드를 표시한다. */
	}
	return used_qword_diff;	/* [한국어] 표시된 워드가 하나면 끊김 없이 갈 수 있다. */
}

/*
 * [한국어]
 * entry_set - 항목의 한 구간을 쓰고, 바뀌었으면 하드웨어에 알린다
 *
 * @writer: 항목 기록기.
 * @entry: 고칠 항목.
 * @target: 그 구간의 새 값.
 * @start: 시작 워드.
 * @len: 워드 수.
 *
 * 값이 실제로 달라진 경우에만 sync 를 부르는 것이 요점이다. sync 는 설정
 * 캐시 무효화 명령을 큐에 넣고 완료까지 기다리는 비싼 일이라, 바뀐 것이
 * 없는데 부르면 순수한 낭비다.
 *
 * WRITE_ONCE 로 쓰는 이유는 컴파일러가 쓰기를 쪼개거나 합치지 못하게
 * 막기 위해서다 — 하드웨어는 64비트 단위 원자성만 보장하므로, 한 워드가
 * 두 번에 나뉘어 쓰이면 중간의 어긋난 값을 볼 수 있다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로. sync 안에서 명령 큐를 다룬다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → [이 함수] → ops->sync()
 */
static void entry_set(struct arm_smmu_entry_writer *writer, __le64 *entry,
		      const __le64 *target, unsigned int start,
		      unsigned int len)
{
	bool changed = false;	/* [한국어] 하나라도 실제로 바뀌었는가. */
	unsigned int i;	/* [한국어] 워드 반복자. */

	for (i = start; len != 0; len--, i++) {	/* [한국어] 지정된 구간만. */
		if (entry[i] != target[i]) {	/* [한국어] 값이 다를 때만 쓴다 — 같은 값을 다시 쓰면 캐시 라인이 괜히 더럽혀진다. */
			WRITE_ONCE(entry[i], target[i]);	/* [한국어] 64비트 원자 쓰기로 한 번에. 컴파일러가 쪼개지 못하게 막는다. */
			changed = true;	/* [한국어] 실제로 바뀐 것이 있으니 아래에서 하드웨어에 알려야 한다. */
		}
	}

	if (changed)	/* [한국어] 바뀐 것이 있을 때만. */
		writer->ops->sync(writer);	/* [한국어] 하드웨어의 설정 캐시를 씻고 완료까지 기다린다 — 다음 단계로 넘어가기 전에 반드시 필요하다. */
}

/*
 * Update the STE/CD to the target configuration. The transition from the
 * current entry to the target entry takes place over multiple steps that
 * attempts to make the transition hitless if possible. This function takes care
 * not to create a situation where the HW can perceive a corrupted entry. HW is
 * only required to have a 64 bit atomicity with stores from the CPU, while
 * entries are many 64 bit values big.
 *
 * The difference between the current value and the target value is analyzed to
 * determine which of three updates are required - disruptive, hitless or no
 * change.
 *
 * In the most general disruptive case we can make any update in three steps:
 *  - Disrupting the entry (V=0)
 *  - Fill now unused qwords, execpt qword 0 which contains V
 *  - Make qword 0 have the final value and valid (V=1) with a single 64
 *    bit store
 *
 * However this disrupts the HW while it is happening. There are several
 * interesting cases where a STE/CD can be updated without disturbing the HW
 * because only a small number of bits are changing (S1DSS, CONFIG, etc) or
 * because the used bits don't intersect. We can detect this by calculating how
 * many 64 bit values need update after adjusting the unused bits and skip the
 * V=0 process. This relies on the IGNORED behavior described in the
 * specification.
 */
/*
 * [한국어]
 * arm_smmu_write_entry - STE/CD 를 하드웨어가 중간 상태를 못 보게 고쳐 쓴다
 *
 * @writer: 항목 기록기 (사용 비트 계산과 sync 갈고리를 들고 있다).
 * @entry: 고칠 항목 (하드웨어가 읽고 있는 실제 메모리).
 * @target: 도달할 값.
 *
 * (위 영어 주석 참고) 이 드라이버의 두 번째 핵심 함수다. 문제의 뿌리는
 * 하드웨어가 64비트 원자성만 보장하는데 항목은 8워드라는 점이다. 순진하게
 * 앞에서부터 쓰면 하드웨어가 "앞의 절반은 새 설정, 뒤의 절반은 옛 설정"인
 * 어중간한 항목을 읽어 엉뚱한 페이지 테이블을 걷는다.
 *
 * 갈래가 셋이다.
 * 하나, 읽히는 비트의 차이가 워드 하나에만 있는 경우 — 끊김 없이 간다.
 * 먼저 읽히지 않는 비트를 모두 새 값으로 바꾸고(하드웨어는 못 느낀다),
 * 그다음 그 한 워드를 원자적으로 바꾸면 전환이 끝난다. 마지막으로 이제는
 * 읽히지 않게 된 비트를 정리한다.
 * 둘, 차이가 여러 워드에 걸친 경우 — 어쩔 수 없이 끊긴다. 유효 비트를
 * 내려 항목을 무효로 만들고(그동안 그 스트림의 접근은 폴트가 된다),
 * 나머지 워드를 다 쓴 뒤, 마지막에 유효 비트를 켜며 첫 워드를 완성한다.
 * 셋, 읽히는 비트가 하나도 안 바뀐 경우 — 한 번에 다 써도 안전하다.
 *
 * 맨 앞의 dma_wmb() 도 필수다. 항목 안에 다른 표의 주소가 들어 있는 경우가
 * 많은데, 그 표의 내용이 먼저 보이지 않으면 하드웨어가 아직 채워지지 않은
 * 표를 따라간다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. sync 안에서 명령 큐를 다룬다.
 *
 * 호출 체인:
 *   arm_smmu_write_ste()/write_cd_entry() → [이 함수]
 *     → arm_smmu_entry_qword_diff() → entry_set() → ops->sync()
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 규약을 직접 검증할 수 있게 연다. */
void arm_smmu_write_entry(struct arm_smmu_entry_writer *writer, __le64 *entry,
			  const __le64 *target)
{
	__le64 unused_update[NUM_ENTRY_QWORDS];	/* [한국어] 읽히지 않는 비트만 새 값으로 바꾼 중간 모습. */
	u8 used_qword_diff;	/* [한국어] 읽히는 비트를 바꿔야 하는 워드들. */

	/*
	 * Many of the entry structures have pointers to other structures that
	 * need to have their updates be visible before any writes of the entry
	 * happen.
	 */
	/* [한국어] (위 영어 주석 참고) 항목에는 문맥 서술자 표나 페이지 테이블의
	 * 주소가 들어 있다. 그 표를 채운 쓰기가 항목 쓰기보다 늦게 보이면,
	 * 하드웨어가 아직 준비되지 않은 표를 따라가 쓰레기를 읽는다. */
	dma_wmb();	/* [한국어] 앞선 자료 구조 쓰기가 먼저 보이게 한다. */

	used_qword_diff =	/* [한국어] 어떤 갈래로 갈지 판정한다. */
		arm_smmu_entry_qword_diff(writer, entry, target, unused_update);
	if (hweight8(used_qword_diff) == 1) {	/* [한국어] 바꿔야 할 워드가 정확히 하나라면 — 끊김 없이 갈 수 있다. */
		/*
		 * Only one qword needs its used bits to be changed. This is a
		 * hitless update, update all bits the current STE/CD is
		 * ignoring to their new values, then update a single "critical
		 * qword" to change the STE/CD and finally 0 out any bits that
		 * are now unused in the target configuration.
		 */
		/* [한국어] (위 영어 주석 참고) 세 걸음이다 — 무시되는 비트를 미리 채우고,
		 * 결정적인 한 워드를 원자적으로 바꾸고, 남은 찌꺼기를 정리한다. */
		unsigned int critical_qword_index = ffs(used_qword_diff) - 1;	/* [한국어] 그 결정적인 워드의 번호. */

		/*
		 * Skip writing unused bits in the critical qword since we'll be
		 * writing it in the next step anyways. This can save a sync
		 * when the only change is in that qword.
		 */
		/* [한국어] (위 영어 주석 참고) 어차피 다음 걸음에서 통째로 쓸 워드이므로
		 * 지금 건드리지 않는다. 그 워드만 바뀌는 흔한 경우에는 첫 걸음에서
		 * 아무것도 바뀌지 않아 sync 한 번을 아낀다. */
		unused_update[critical_qword_index] =	/* [한국어] 그 워드는 지금 값 그대로 둔다. */
			entry[critical_qword_index];
		entry_set(writer, entry, unused_update, 0, NUM_ENTRY_QWORDS);	/* [한국어] 1걸음 — 무시되는 비트를 새 값으로. 하드웨어는 아무 변화도 못 느낀다. */
		entry_set(writer, entry, target, critical_qword_index, 1);	/* [한국어] 2걸음 — 결정적인 한 워드를 원자적으로 바꾼다. 이 순간 전환이 일어난다. */
		entry_set(writer, entry, target, 0, NUM_ENTRY_QWORDS);	/* [한국어] 3걸음 — 이제 읽히지 않게 된 비트를 정리한다. */
	} else if (used_qword_diff) {	/* [한국어] 바꿔야 할 워드가 둘 이상이면 — 끊김을 피할 수 없다. */
		/*
		 * At least two qwords need their inuse bits to be changed. This
		 * requires a breaking update, zero the V bit, write all qwords
		 * but 0, then set qword 0
		 */
		/* [한국어] (위 영어 주석 참고) 유효 비트를 내려 항목을 무효로 만들면,
		 * 그동안 그 스트림의 접근은 폴트가 된다. 그 대가를 치르는 대신
		 * 하드웨어가 어중간한 설정을 보는 일은 절대 없다. */
		unused_update[0] = 0;	/* [한국어] 첫 워드를 통째로 0 으로 — 유효 비트가 여기 있다. */
		entry_set(writer, entry, unused_update, 0, 1);	/* [한국어] 1걸음 — 항목을 무효로 만든다. */
		entry_set(writer, entry, target, 1, NUM_ENTRY_QWORDS - 1);	/* [한국어] 2걸음 — 나머지 워드를 마음껏 쓴다. 하드웨어가 읽지 않는 상태다. */
		entry_set(writer, entry, target, 0, 1);	/* [한국어] 3걸음 — 첫 워드를 완성하며 유효 비트를 켠다. 이 한 번의 원자 쓰기로 새 설정이 살아난다. */
	} else {	/* [한국어] 읽히는 비트가 하나도 안 바뀐 경우. */
		/*
		 * No inuse bit changed, though safe bits may have changed.
		 */
		/* [한국어] (위 영어 주석 참고) 완화된 비트만 달라졌을 수 있으므로 쓰기는
		 * 필요하지만, 어떤 순서로 써도 하드웨어의 해석은 달라지지 않는다. */
		entry_set(writer, entry, target, 0, NUM_ENTRY_QWORDS);	/* [한국어] 한 번에 다 쓴다. */
	}
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_write_entry);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * [한국어]
 * arm_smmu_sync_cd - 문맥 서술자 캐시를 씻는다
 *
 * @master: 대상 장치.
 * @ssid: 무효화할 PASID.
 * @leaf: 참이면 서술자만, 거짓이면 표를 걷는 경로까지 버린다.
 *
 * 서술자를 고쳐 쓴 뒤에는 하드웨어가 캐시에 들고 있던 옛 값을 버리게 해야
 * 한다. 한 장치가 스트림 번호를 여럿 가질 수 있으므로 그 목록을 돌며
 * 명령을 만들되, 묶음으로 모아 한 번에 낸다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로(sync 갈고리). 완료까지 기다린다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → ops->sync → arm_smmu_cd_writer_sync_entry()
 *     → [이 함수] → arm_smmu_cmdq_batch_submit()
 */
static void arm_smmu_sync_cd(struct arm_smmu_master *master,
			     int ssid, bool leaf)
{
	size_t i;	/* [한국어] 스트림 반복자. */
	struct arm_smmu_cmdq_batch cmds;	/* [한국어] 명령 묶음. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 명령을 넣을 하드웨어. */
	struct arm_smmu_cmdq_ent cmd = {	/* [한국어] 명령의 본보기 — 스트림 번호만 바꿔 가며 쓴다. */
		.opcode	= CMDQ_OP_CFGI_CD,	/* [한국어] 문맥 서술자 무효화. */
		.cfgi	= {
			.ssid	= ssid,	/* [한국어] 어느 PASID 의 서술자인지. */
			.leaf	= leaf,	/* [한국어] 표를 걷는 경로까지 버릴지. */
		},
	};

	arm_smmu_cmdq_batch_init(smmu, &cmds, &cmd);	/* [한국어] 묶음을 시작한다. */
	for (i = 0; i < master->num_streams; i++) {	/* [한국어] 이 장치의 모든 스트림 번호에 대해. */
		cmd.cfgi.sid = master->streams[i].id;	/* [한국어] 본보기의 스트림 번호만 갈아 끼운다. */
		arm_smmu_cmdq_batch_add(smmu, &cmds, &cmd);	/* [한국어] 묶음에 담는다 — 여기서 명령 워드로 복사되므로 본보기를 재사용해도 된다. */
	}

	arm_smmu_cmdq_batch_submit(smmu, &cmds);	/* [한국어] 한 번에 내보내고 완료까지 기다린다 — 돌아오면 캐시가 비었다고 믿어도 된다. */
}

/*
 * [한국어]
 * arm_smmu_write_cd_l1_desc - 2단계 문맥 표의 위쪽 항목을 채운다
 *
 * @dst: 위쪽 표의 그 자리.
 * @l2ptr_dma: 아래쪽 표의 장치 쪽 주소.
 *
 * 아래쪽 표를 게으르게 잡은 뒤 그 주소를 위쪽 표에 걸어 주는 일이다.
 * 하드웨어가 이 자리를 64비트 원자 쓰기로 읽는다고 규격이 보장하므로,
 * 한 번의 WRITE_ONCE 로 안전하게 발표할 수 있다.
 *
 * 실행 컨텍스트: 문맥 서술자 자리를 잡는 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_alloc_cd_ptr() → [이 함수]
 */
static void arm_smmu_write_cd_l1_desc(struct arm_smmu_cdtab_l1 *dst,
				      dma_addr_t l2ptr_dma)
{
	u64 val = (l2ptr_dma & CTXDESC_L1_DESC_L2PTR_MASK) | CTXDESC_L1_DESC_V;	/* [한국어] 주소에서 규격이 쓰는 비트만 남기고 유효 표시를 켠다. */

	/* The HW has 64 bit atomicity with stores to the L2 CD table */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 이 자리를 원자적으로 읽으므로,
	 * 주소와 유효 비트를 한 번에 발표할 수 있다 — 나눠 쓸 필요가 없다. */
	WRITE_ONCE(dst->l2ptr, cpu_to_le64(val));	/* [한국어] 컴파일러가 쪼개지 못하게 막으며 하드웨어 형식으로 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_cd_l1_get_desc - 위쪽 항목에서 아래쪽 표의 주소를 꺼낸다
 *
 * @src: 위쪽 표의 그 자리.
 * @return: 아래쪽 표의 장치 쪽 주소 (없으면 0).
 *
 * 표를 해제할 때, 잡아 두었던 dma 주소를 되찾기 위해 쓴다. 커널 쪽 주소는
 * 별도 배열(l2ptrs)에 있지만 dma 주소는 여기밖에 없어서다.
 *
 * 실행 컨텍스트: 표 해제 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_free_cd_tables() → [이 함수]
 */
static dma_addr_t arm_smmu_cd_l1_get_desc(const struct arm_smmu_cdtab_l1 *src)
{
	return le64_to_cpu(src->l2ptr) & CTXDESC_L1_DESC_L2PTR_MASK;	/* [한국어] 유효 비트를 걷어 내고 주소만 남긴다. */
}

/*
 * [한국어]
 * arm_smmu_get_cd_ptr - 그 PASID 의 문맥 서술자가 놓인 자리를 찾는다
 *
 * @master: 대상 장치.
 * @ssid: 찾을 PASID.
 * @return: 서술자 자리, 없으면 NULL.
 *
 * 표가 평면인지 2단계인지에 따라 찾는 방법이 다르다. 평면이면 첨자 하나로
 * 바로 닿고, 2단계면 위쪽 첨자로 아래쪽 표를 찾은 뒤 다시 첨자를 쓴다.
 *
 * 새로 잡지는 않는다는 점이 arm_smmu_alloc_cd_ptr 와 다르다 — 이미 있는
 * 자리를 찾기만 하며, 아래쪽 표가 아직 없으면 NULL 을 돌려준다.
 *
 * 실행 컨텍스트: 서술자를 다루는 곳 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_write_cd_entry()/clear_cd()/SVA 경로 → [이 함수]
 */
struct arm_smmu_cd *arm_smmu_get_cd_ptr(struct arm_smmu_master *master,
					u32 ssid)
{
	struct arm_smmu_cdtab_l2 *l2;	/* [한국어] 2단계일 때의 아래쪽 표. */
	struct arm_smmu_ctx_desc_cfg *cd_table = &master->cd_table;	/* [한국어] 이 장치의 문맥 표 설정. */

	if (!arm_smmu_cdtab_allocated(cd_table))	/* [한국어] 표가 아예 없으면. */
		return NULL;	/* [한국어] PASID 를 쓰지 않는 장치다. */

	if (cd_table->s1fmt == STRTAB_STE_0_S1FMT_LINEAR)	/* [한국어] 평면 표라면. */
		return &cd_table->linear.table[ssid];	/* [한국어] PASID 를 첨자로 바로 짚는다. */

	l2 = cd_table->l2.l2ptrs[arm_smmu_cdtab_l1_idx(ssid)];	/* [한국어] 2단계라면 위쪽 첨자로 아래쪽 표를 찾는다. */
	if (!l2)	/* [한국어] 그 구간의 표가 아직 잡히지 않았다면. */
		return NULL;	/* [한국어] 자리가 없다 — 필요하면 호출자가 alloc 판을 쓴다. */
	return &l2->cds[arm_smmu_cdtab_l2_idx(ssid)];	/* [한국어] 아래쪽 첨자로 항목에 닿는다. */
}

/*
 * [한국어]
 * arm_smmu_alloc_cd_ptr - 그 PASID 의 서술자 자리를 마련해 돌려준다
 *
 * @master: 대상 장치.
 * @ssid: 자리를 마련할 PASID.
 * @return: 서술자 자리, 메모리가 없으면 NULL.
 *
 * 찾기만 하는 get 판과 달리, 없으면 잡아 준다. 두 단계가 게으르게 잡힌다 —
 * 표 자체가 없으면 표를 잡고, 2단계 구조에서 그 구간의 아래쪽 표가 없으면
 * 그것을 잡는다. PASID 공간이 넓어도 실제로 쓰는 구간만 메모리를 쓰게 하는
 * 설계다.
 *
 * 아래쪽 표를 새로 단 뒤 무효화를 내는 것이 중요하다. 하드웨어가 "이 구간의
 * 위쪽 항목은 무효"라는 사실을 캐시에 들고 있을 수 있어, 새로 달아도
 * 그 캐시 때문에 못 보기 때문이다.
 *
 * 실행 컨텍스트: 붙이기 경로, group mutex 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_set_pasid() → [이 함수]
 *     → arm_smmu_alloc_cd_tables() → dma_alloc_coherent() → arm_smmu_sync_cd()
 */
static struct arm_smmu_cd *arm_smmu_alloc_cd_ptr(struct arm_smmu_master *master,
						 u32 ssid)
{
	struct arm_smmu_ctx_desc_cfg *cd_table = &master->cd_table;	/* [한국어] 이 장치의 문맥 표 설정. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] dma 를 잡을 장치. */

	might_sleep();	/* [한국어] 잠들 수 있는 함수임을 디버그 빌드에서 못 박는다 — 원자적 문맥에서 부르면 바로 드러난다. */
	iommu_group_mutex_assert(master->dev);	/* [한국어] 이 표는 group mutex 로 지켜진다 — 잡고 들어왔는지 확인한다. */

	if (!arm_smmu_cdtab_allocated(cd_table)) {	/* [한국어] 표가 아직 없으면. */
		if (arm_smmu_alloc_cd_tables(master))	/* [한국어] 지금 잡는다 — PASID 를 처음 쓰는 순간이다. */
			return NULL;	/* [한국어] 메모리가 없으면 자리를 줄 수 없다. */
	}

	if (cd_table->s1fmt == STRTAB_STE_0_S1FMT_64K_L2) {	/* [한국어] 2단계 구조라면 아래쪽 표도 필요하다. */
		unsigned int idx = arm_smmu_cdtab_l1_idx(ssid);	/* [한국어] 이 PASID 가 속한 구간. */
		struct arm_smmu_cdtab_l2 **l2ptr = &cd_table->l2.l2ptrs[idx];	/* [한국어] 그 구간의 아래쪽 표 자리. */

		if (!*l2ptr) {	/* [한국어] 아직 잡히지 않았다면. */
			dma_addr_t l2ptr_dma;	/* [한국어] 하드웨어가 쓸 주소. */

			*l2ptr = dma_alloc_coherent(smmu->dev, sizeof(**l2ptr),	/* [한국어] 하드웨어도 읽는 표라 일관 메모리로 잡는다. */
						    &l2ptr_dma, GFP_KERNEL);
			if (!*l2ptr)	/* [한국어] 메모리가 없으면. */
				return NULL;

			arm_smmu_write_cd_l1_desc(&cd_table->l2.l1tab[idx],	/* [한국어] 위쪽 표에 그 주소를 걸어 하드웨어가 찾아갈 수 있게 한다. */
						  l2ptr_dma);
			/* An invalid L1CD can be cached */
			/* [한국어] (위 영어 주석 참고) 하드웨어가 "이 구간은 무효"라는 사실을
			 * 캐시에 들고 있을 수 있다. 새 표를 달아도 그 캐시 때문에 못 보므로
			 * 반드시 무효화를 내야 한다. */
			arm_smmu_sync_cd(master, ssid, false);	/* [한국어] leaf=false 로 걷는 경로까지 버리게 한다 — 바뀐 것이 중간 단계이기 때문이다. */
		}
	}
	return arm_smmu_get_cd_ptr(master, ssid);	/* [한국어] 이제 반드시 찾아지는 자리를 돌려준다. */
}

/* [한국어] 문맥 서술자 전용 기록기 — 공통 기록기에 PASID 를 덧붙인 것.
 *
 * 서술자를 고쳐 쓴 뒤 무효화 명령을 내려면 어느 PASID 인지 알아야 하는데,
 * 공통 기록기 구조에는 그 자리가 없어 이렇게 감싼다. */
struct arm_smmu_cd_writer {
	/* [한국어] 공통 기록기 몸통 — 반드시 첫 필드여야 되짚기가 성립한다.
	 * 설정자: arm_smmu_write_cd_entry() 가 연산표와 장치를 채운다.
	 * 읽는 자: arm_smmu_write_entry() 가 이 포인터로 갈고리를 부른다.
	 * 값 범위: ops 는 언제나 arm_smmu_cd_writer_ops.
	 * 동기화: 스택 변수라 공유되지 않는다. */
	struct arm_smmu_entry_writer writer;
	/* [한국어] 고쳐 쓰는 서술자의 PASID.
	 * 설정자: 위와 같은 자리.
	 * 읽는 자: sync 갈고리가 무효화 명령의 인자로 쓴다.
	 * 값 범위: 0(IOMMU_NO_PASID) 부터 장치가 지원하는 최대까지.
	 * 동기화: 없음. */
	unsigned int ssid;
};

/*
 * [한국어]
 * arm_smmu_get_cd_used - 이 서술자 설정이 실제로 읽는 비트를 계산한다
 *
 * @ent: 검사할 서술자.
 * @used_bits: 결과 마스크.
 *
 * STE 쪽 계산보다 훨씬 단순하다. 유효 비트가 꺼져 있으면 그 비트 하나만
 * 읽히고, 켜져 있으면 사실상 전부 읽힌다고 본다.
 *
 * 예외가 하나 있는데 그것이 SVA 안전의 토대다. EPD0 가 켜지면 하드웨어가
 * TTB0 와 그에 딸린 변환 설정을 아예 읽지 않는다. 곧 정상 서술자와
 * "모두 폴트" 서술자 사이에서 그 필드들이 자유로워지고, 유효 비트를
 * 내리지 않고도 두 상태를 오갈 수 있다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → ops->get_used = [이 함수]
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 계산을 검증할 수 있게 연다. */
void arm_smmu_get_cd_used(const __le64 *ent, __le64 *used_bits)
{
	used_bits[0] = cpu_to_le64(CTXDESC_CD_0_V);	/* [한국어] 유효 비트는 언제나 읽힌다. */
	if (!(ent[0] & cpu_to_le64(CTXDESC_CD_0_V)))	/* [한국어] 유효하지 않으면. */
		return;	/* [한국어] 나머지는 읽히지 않으므로 자유롭게 바꿔도 된다. */
	memset(used_bits, 0xFF, sizeof(struct arm_smmu_cd));	/* [한국어] 유효하면 일단 전부 읽힌다고 본다 — 아래에서 예외만 걷어 낸다. */

	/*
	 * If EPD0 is set by the make function it means
	 * T0SZ/TG0/IR0/OR0/SH0/TTB0 are IGNORED
	 */
	/* [한국어] (위 영어 주석 참고) EPD0 는 "TTB0 를 쓰지 말라"는 뜻이므로,
	 * 하드웨어가 그 테이블과 관련 설정을 아예 읽지 않는다. 이 예외 덕분에
	 * SVA 가 프로세스를 놓을 때 서술자를 끊김 없이 바꿀 수 있다. */
	if (ent[0] & cpu_to_le64(CTXDESC_CD_0_TCR_EPD0)) {	/* [한국어] TTB0 사용이 막혀 있으면. */
		used_bits[0] &= ~cpu_to_le64(	/* [한국어] 변환 설정 필드들을 "읽히지 않음"으로 되돌린다. */
			CTXDESC_CD_0_TCR_T0SZ | CTXDESC_CD_0_TCR_TG0 |	/* [한국어] 주소 폭과 알갱이 크기. */
			CTXDESC_CD_0_TCR_IRGN0 | CTXDESC_CD_0_TCR_ORGN0 |	/* [한국어] 캐시 정책. */
			CTXDESC_CD_0_TCR_SH0);	/* [한국어] 공유 영역. */
		used_bits[1] &= ~cpu_to_le64(CTXDESC_CD_1_TTB0_MASK);	/* [한국어] 페이지 테이블 주소 자체도 읽히지 않는다. */
	}
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_get_cd_used);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * [한국어]
 * arm_smmu_cd_writer_sync_entry - 서술자 쓰기 단계마다 캐시를 씻는다
 *
 * @writer: 공통 기록기 (감싼 구조에서 PASID 를 꺼낸다).
 *
 * 항목 쓰기 규약이 각 단계 사이에 부르는 갈고리다. 이 자리에서 무효화를
 * 내고 완료를 기다려야, 다음 단계로 넘어갈 때 하드웨어가 이미 새 값을
 * 보고 있다고 믿을 수 있다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로. 완료까지 기다린다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → ops->sync = [이 함수] → arm_smmu_sync_cd()
 */
static void arm_smmu_cd_writer_sync_entry(struct arm_smmu_entry_writer *writer)
{
	struct arm_smmu_cd_writer *cd_writer =	/* [한국어] 공통 기록기에서 서술자 전용 구조로 되짚는다. */
		container_of(writer, struct arm_smmu_cd_writer, writer);

	arm_smmu_sync_cd(writer->master, cd_writer->ssid, true);	/* [한국어] 그 PASID 의 서술자 캐시만 버린다 — leaf=true 라 표를 걷는 경로는 살려 둔다. */
}

/* [한국어] 문맥 서술자용 기록기 연산표.
 *
 * get_update_safe 가 없다는 점이 STE 쪽과 다르다. 서술자에는 "갱신 도중
 * 달라져도 안전한 비트"라는 개념이 필요 없어, 판정이 더 엄격하게 돈다. */
static const struct arm_smmu_entry_writer_ops arm_smmu_cd_writer_ops = {
	.sync = arm_smmu_cd_writer_sync_entry,	/* [한국어] 단계마다 서술자 캐시를 씻는 갈고리. */
	.get_used = arm_smmu_get_cd_used,	/* [한국어] 읽히는 비트를 계산하는 갈고리. */
};

/*
 * [한국어]
 * arm_smmu_write_cd_entry - 문맥 서술자 한 항목을 안전하게 고쳐 쓴다
 *
 * @master: 그 서술자를 쓰는 장치.
 * @ssid: 고칠 항목의 PASID.
 * @cdptr: 표 안의 그 항목이 놓인 자리.
 * @target: 최종적으로 들어가야 할 값.
 *
 * 공통 쓰기 규약에 서술자용 연산표를 얹어 부른다. 그에 더해 "쓰이는
 * PASID 수"를 여기서 세는데, 그 값이 0 인지 여부가 장치를 중첩 변환에
 * 넘길 수 있는지를 가르기 때문이다.
 *
 * PASID 0 은 세지 않는다 — 그 자리는 장치의 기본 문맥이라 언제나 있다고
 * 보고, "PASID 를 쓰고 있는가"의 판단에서는 제외한다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 완료를 기다리므로 시간이 걸린다.
 *
 * 호출 체인:
 *   arm_smmu_set_pasid()/clear_cd()/SVA 경로 → [이 함수]
 *     → arm_smmu_write_entry()
 */
void arm_smmu_write_cd_entry(struct arm_smmu_master *master, int ssid,
			     struct arm_smmu_cd *cdptr,
			     const struct arm_smmu_cd *target)
{
	bool target_valid = target->data[0] & cpu_to_le64(CTXDESC_CD_0_V);	/* [한국어] 새 값이 유효한 서술자인가. */
	bool cur_valid = cdptr->data[0] & cpu_to_le64(CTXDESC_CD_0_V);	/* [한국어] 지금 값은 어떤가. */
	struct arm_smmu_cd_writer cd_writer = {	/* [한국어] 서술자용 기록기를 스택에 짓는다. */
		.writer = {	/* [한국어] 기록기 몸통 — 검증 대상 코드는 이 부분만 본다. */
			.ops = &arm_smmu_cd_writer_ops,	/* [한국어] 서술자용 연산표. */
			.master = master,	/* [한국어] 무효화 명령을 낼 장치. */
		},
		.ssid = ssid,	/* [한국어] 무효화 명령의 인자가 된다. */
	};

	if (ssid != IOMMU_NO_PASID && cur_valid != target_valid) {	/* [한국어] 기본 문맥이 아니고 유효 여부가 바뀌는 경우에만 센다. */
		if (cur_valid)	/* [한국어] 유효하던 것이 무효가 되면. */
			master->cd_table.used_ssids--;	/* [한국어] 쓰는 PASID 가 하나 줄었다. */
		else	/* [한국어] 무효하던 것이 유효해지면. */
			master->cd_table.used_ssids++;	/* [한국어] 하나 늘었다 — 이 값이 0 이 아니면 중첩 변환에 넘길 수 없다. */
	}

	arm_smmu_write_entry(&cd_writer.writer, cdptr->data, target->data);	/* [한국어] 하드웨어가 중간 상태를 못 보게 나눠 쓴다. */
}

/*
 * [한국어]
 * arm_smmu_make_s1_cd - 1단계 변환용 문맥 서술자를 짓는다
 *
 * @target: 지어 담을 자리.
 * @master: 이 서술자를 쓸 장치.
 * @smmu_domain: 그 주소 공간 (페이지 테이블 설정과 ASID 를 여기서 가져온다).
 *
 * io-pgtable 이 만들어 둔 페이지 테이블 설정을 서술자 형식으로 옮겨 담는
 * 일이다. SVA 판(arm_smmu_make_sva_cd)이 CPU 레지스터에서 값을 읽어 오는
 * 것과 달리, 여기서는 io-pgtable 이 계산해 준 값을 쓴다 — 커널이 이
 * 도메인을 위해 따로 지은 테이블이기 때문이다.
 *
 * 더티 추적 처리가 눈에 띈다. 마이그레이션에서 바뀐 페이지를 골라내려면
 * 하드웨어가 접근·더티 플래그를 스스로 갱신해야 하고, 그 두 비트를 함께
 * 켜야 동작한다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들지 않는다 (값을 짓기만 한다).
 *
 * 호출 체인:
 *   arm_smmu_attach_dev()/set_pasid() → [이 함수]
 */
void arm_smmu_make_s1_cd(struct arm_smmu_cd *target,
			 struct arm_smmu_master *master,
			 struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_ctx_desc *cd = &smmu_domain->cd;	/* [한국어] 이 도메인의 ASID 가 들어 있다. */
	const struct io_pgtable_cfg *pgtbl_cfg =	/* [한국어] io-pgtable 이 계산해 둔 설정. */
		&io_pgtable_ops_to_pgtable(smmu_domain->pgtbl_ops)->cfg;
	typeof(&pgtbl_cfg->arm_lpae_s1_cfg.tcr) tcr =	/* [한국어] 변환 제어 필드 묶음 — 이름이 길어 별칭을 둔다. */
		&pgtbl_cfg->arm_lpae_s1_cfg.tcr;

	memset(target, 0, sizeof(*target));	/* [한국어] 남은 찌꺼기가 하드웨어에게 엉뚱한 뜻으로 읽히지 않게 지운다. */

	target->data[0] = cpu_to_le64(	/* [한국어] 첫 워드에 변환 제어와 유효 표시를 담는다. */
		FIELD_PREP(CTXDESC_CD_0_TCR_T0SZ, tcr->tsz) |	/* [한국어] 입력 주소 폭 — io-pgtable 이 정한 값. */
		FIELD_PREP(CTXDESC_CD_0_TCR_TG0, tcr->tg) |	/* [한국어] 알갱이 크기. */
		FIELD_PREP(CTXDESC_CD_0_TCR_IRGN0, tcr->irgn) |	/* [한국어] 표를 읽을 때의 안쪽 캐시 정책. */
		FIELD_PREP(CTXDESC_CD_0_TCR_ORGN0, tcr->orgn) |	/* [한국어] 바깥 캐시 정책. */
		FIELD_PREP(CTXDESC_CD_0_TCR_SH0, tcr->sh) |	/* [한국어] 공유 영역. */
#ifdef __BIG_ENDIAN	/* [한국어] 빅엔디안 커널에서만. */
		CTXDESC_CD_0_ENDI |	/* [한국어] 페이지 테이블 항목을 빅엔디안으로 읽으라고 알린다. */
#endif
		CTXDESC_CD_0_TCR_EPD1 |	/* [한국어] TTBR1(커널 절반)은 쓰지 않는다 — 장치는 사용자 주소만 낸다. */
		CTXDESC_CD_0_V |	/* [한국어] 이 서술자가 유효하다는 표시. */
		FIELD_PREP(CTXDESC_CD_0_TCR_IPS, tcr->ips) |	/* [한국어] 출력 물리 주소 폭. */
		CTXDESC_CD_0_AA64 |	/* [한국어] AArch64 형식의 페이지 테이블. */
		(master->stall_enabled ? CTXDESC_CD_0_S : 0) |	/* [한국어] 멈춤을 지원하는 장치면 폴트 때 트랜잭션을 붙잡아 둔다. */
		CTXDESC_CD_0_R |	/* [한국어] 접근 플래그 오류를 폴트로 보고한다. */
		CTXDESC_CD_0_A |	/* [한국어] 하드웨어가 접근 플래그를 스스로 갱신한다. */
		CTXDESC_CD_0_ASET |	/* [한국어] 브로드캐스트 TLB 무효화를 받지 않는다 — 무효화는 명령 큐로만 한다. */
		FIELD_PREP(CTXDESC_CD_0_ASID, cd->asid)	/* [한국어] 이 주소 공간의 번호. TLB 항목에 태그로 붙는다. */
		);

	/* To enable dirty flag update, set both Access flag and dirty state update */
	/* [한국어] (위 영어 주석 참고) 더티 추적은 접근 플래그 갱신과 더티 상태 갱신
	 * 두 비트가 함께 켜져야 동작한다. 하나만 켜면 하드웨어가 무시한다. */
	if (pgtbl_cfg->quirks & IO_PGTABLE_QUIRK_ARM_HD)	/* [한국어] io-pgtable 이 더티 추적용으로 테이블을 지었다면. */
		target->data[0] |= cpu_to_le64(CTXDESC_CD_0_TCR_HA |	/* [한국어] 접근 플래그 하드웨어 갱신. */
					       CTXDESC_CD_0_TCR_HD);	/* [한국어] 더티 상태 하드웨어 갱신 — 이것이 있어야 바뀐 페이지를 골라낼 수 있다. */

	target->data[1] = cpu_to_le64(pgtbl_cfg->arm_lpae_s1_cfg.ttbr &	/* [한국어] 페이지 테이블의 뿌리 주소. */
				      CTXDESC_CD_1_TTB0_MASK);	/* [한국어] 규격이 쓰는 비트만 남긴다. */
	target->data[3] = cpu_to_le64(pgtbl_cfg->arm_lpae_s1_cfg.mair);	/* [한국어] 메모리 속성 표 — 페이지 테이블의 속성 인덱스가 이 표를 거쳐 풀린다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_make_s1_cd);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * [한국어]
 * arm_smmu_clear_cd - 그 PASID 의 서술자를 지운다
 *
 * @master: 대상 장치.
 * @ssid: 지울 PASID.
 *
 * 전부 0 인 서술자를 써 넣는다 — 유효 비트가 꺼지므로 그 PASID 로 오는
 * 트랜잭션은 모두 오류가 된다. PASID 를 떼는 경로에서 쓴다.
 *
 * 표가 아예 없으면 아무 일도 하지 않는데, PASID 를 한 번도 쓰지 않은
 * 장치라는 뜻이라 지울 것도 없기 때문이다.
 *
 * 실행 컨텍스트: 떼기 경로. 완료를 기다린다.
 *
 * 호출 체인:
 *   arm_smmu_remove_dev_pasid() 등 → [이 함수] → arm_smmu_write_cd_entry()
 */
void arm_smmu_clear_cd(struct arm_smmu_master *master, ioasid_t ssid)
{
	struct arm_smmu_cd target = {};	/* [한국어] 전부 0 — 유효 비트가 꺼진 서술자다. */
	struct arm_smmu_cd *cdptr;	/* [한국어] 표 안의 자리. */

	if (!arm_smmu_cdtab_allocated(&master->cd_table))	/* [한국어] 표가 없으면. */
		return;	/* [한국어] 지울 것도 없다. */
	cdptr = arm_smmu_get_cd_ptr(master, ssid);	/* [한국어] 그 PASID 의 자리를 찾는다 — 새로 잡지는 않는다. */
	if (WARN_ON(!cdptr))	/* [한국어] 붙어 있던 PASID 인데 자리가 없다면 자료 구조가 어긋난 것이다. */
		return;
	arm_smmu_write_cd_entry(master, ssid, cdptr, &target);	/* [한국어] 안전한 순서로 지운다 — 쓰고 있는 PASID 수도 함께 줄어든다. */
}

/*
 * [한국어]
 * arm_smmu_alloc_cd_tables - 그 장치의 문맥 서술자 표를 잡는다
 *
 * @master: 대상 장치.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * PASID 를 처음 쓰는 순간에 불린다. 지원할 PASID 수를 보고 평면 표로
 * 잡을지 2단계로 잡을지 정하는데, 그 판단이 이 함수의 요점이다.
 * PASID 가 1024개 이하이거나 하드웨어가 2단계를 지원하지 않으면 평면으로
 * 잡고, 그보다 많으면 2단계로 잡아 실제로 쓰는 구간만 게으르게 채운다.
 * 평면으로 잡으면 PASID 하나당 64바이트라, 20비트 PASID 를 다 지원하려면
 * 64MB 가 필요해 현실적이지 않다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_alloc_cd_ptr() → [이 함수] → dma_alloc_coherent()
 */
static int arm_smmu_alloc_cd_tables(struct arm_smmu_master *master)
{
	int ret;	/* [한국어] 되감기 경로에서 쓸 오류 코드. */
	size_t l1size;	/* [한국어] 잡을 표의 바이트 크기. */
	size_t max_contexts;	/* [한국어] 지원할 PASID 수. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] dma 를 잡을 장치. */
	struct arm_smmu_ctx_desc_cfg *cd_table = &master->cd_table;	/* [한국어] 채울 표 설정. */

	cd_table->s1cdmax = master->ssid_bits;	/* [한국어] 이 장치가 지원하는 PASID 폭 — 펌웨어와 하드웨어가 함께 정한 값이다. */
	max_contexts = 1 << cd_table->s1cdmax;	/* [한국어] 그 폭이 뜻하는 PASID 개수. */

	if (!(smmu->features & ARM_SMMU_FEAT_2_LVL_CDTAB) ||	/* [한국어] 하드웨어가 2단계 표를 지원하지 않거나. */
	    max_contexts <= CTXDESC_L2_ENTRIES) {	/* [한국어] PASID 가 적어 평면으로도 충분하면. */
		cd_table->s1fmt = STRTAB_STE_0_S1FMT_LINEAR;	/* [한국어] 평면 표로 정한다 — 찾기가 첨자 하나로 끝나 빠르다. */
		cd_table->linear.num_ents = max_contexts;	/* [한국어] 항목 수를 기록한다. */

		l1size = max_contexts * sizeof(struct arm_smmu_cd);	/* [한국어] 서술자 하나가 64바이트다. */
		cd_table->linear.table = dma_alloc_coherent(smmu->dev, l1size,	/* [한국어] 하드웨어도 읽는 표라 일관 메모리로. */
							    &cd_table->cdtab_dma,	/* [한국어] STE 에 담을 장치 쪽 주소를 함께 받는다. */
							    GFP_KERNEL);
		if (!cd_table->linear.table)	/* [한국어] 메모리가 없으면. */
			return -ENOMEM;
	} else {	/* [한국어] PASID 가 많아 평면으로는 감당할 수 없는 경우. */
		cd_table->s1fmt = STRTAB_STE_0_S1FMT_64K_L2;	/* [한국어] 2단계 표로 정한다. */
		cd_table->l2.num_l1_ents =	/* [한국어] 위쪽 표가 담을 항목 수. */
			DIV_ROUND_UP(max_contexts, CTXDESC_L2_ENTRIES);	/* [한국어] 아래쪽 표 하나가 담는 수로 나눠 올림한다. */

		cd_table->l2.l2ptrs = kzalloc_objs(*cd_table->l2.l2ptrs,	/* [한국어] 아래쪽 표들의 커널 주소를 담을 배열 — 해제할 때 필요하다. */
						   cd_table->l2.num_l1_ents);
		if (!cd_table->l2.l2ptrs)	/* [한국어] 메모리가 없으면. */
			return -ENOMEM;

		l1size = cd_table->l2.num_l1_ents * sizeof(struct arm_smmu_cdtab_l1);	/* [한국어] 위쪽 표의 크기 — 항목 하나가 8바이트뿐이라 훨씬 작다. */
		cd_table->l2.l1tab = dma_alloc_coherent(smmu->dev, l1size,	/* [한국어] 위쪽 표만 지금 잡고, 아래쪽 표는 쓰일 때 잡는다. */
							&cd_table->cdtab_dma,
							GFP_KERNEL);
		if (!cd_table->l2.l1tab) {	/* [한국어] 메모리가 없으면. */
			ret = -ENOMEM;	/* [한국어] 아래쪽 표를 못 잡으면 이 PASID 를 쓸 수 없다. */
			goto err_free_l2ptrs;	/* [한국어] 방금 잡은 포인터 배열부터 되돌린다. */
		}
	}
	return 0;	/* [한국어] 표가 준비됐다. */

err_free_l2ptrs:	/* [한국어] 포인터 배열만 잡고 실패했을 때의 되감기 지점. */
	kfree(cd_table->l2.l2ptrs);	/* [한국어] 배열을 놓고. */
	cd_table->l2.l2ptrs = NULL;	/* [한국어] "표가 없다"는 판정이 옳게 나오도록 반드시 비워 둔다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_free_cd_tables - 그 장치의 문맥 서술자 표를 놓는다
 *
 * @master: 대상 장치.
 *
 * 2단계 구조라면 잡아 둔 아래쪽 표를 모두 훑어 놓은 뒤 위쪽 표를 놓고,
 * 평면이면 표 하나만 놓는다. 아래쪽 표의 dma 주소는 커널 쪽 배열에 없고
 * 위쪽 표 항목에만 있어, 그 값을 되읽어 해제에 쓴다.
 *
 * 이 함수가 불릴 때는 이미 스트림 표 항목에서 이 표를 떼어 낸 뒤여야
 * 한다 — 그러지 않으면 하드웨어가 놓인 메모리를 계속 읽는다.
 *
 * 실행 컨텍스트: 장치 해제 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_release_device() → [이 함수] → dma_free_coherent()
 */
static void arm_smmu_free_cd_tables(struct arm_smmu_master *master)
{
	int i;	/* [한국어] 아래쪽 표 반복자. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] dma 를 놓을 장치. */
	struct arm_smmu_ctx_desc_cfg *cd_table = &master->cd_table;	/* [한국어] 놓을 표 설정. */

	if (cd_table->s1fmt != STRTAB_STE_0_S1FMT_LINEAR) {	/* [한국어] 2단계 구조라면. */
		for (i = 0; i < cd_table->l2.num_l1_ents; i++) {	/* [한국어] 위쪽 표의 모든 자리를 훑는다. */
			if (!cd_table->l2.l2ptrs[i])	/* [한국어] 게으르게 잡는 구조라 비어 있는 자리가 많다. */
				continue;

			dma_free_coherent(smmu->dev,	/* [한국어] 아래쪽 표 하나를 놓는다. */
					  sizeof(*cd_table->l2.l2ptrs[i]),
					  cd_table->l2.l2ptrs[i],	/* [한국어] 커널 쪽 주소는 배열에서. */
					  arm_smmu_cd_l1_get_desc(&cd_table->l2.l1tab[i]));	/* [한국어] 장치 쪽 주소는 위쪽 표 항목에서 되읽는다 — 따로 저장해 두지 않았다. */
		}
		kfree(cd_table->l2.l2ptrs);	/* [한국어] 커널 주소 배열을 놓는다. */

		dma_free_coherent(smmu->dev,	/* [한국어] 위쪽 표를 놓는다. */
				  cd_table->l2.num_l1_ents *
					  sizeof(struct arm_smmu_cdtab_l1),
				  cd_table->l2.l1tab, cd_table->cdtab_dma);
	} else {	/* [한국어] 평면 표라면. */
		dma_free_coherent(smmu->dev,	/* [한국어] 표 하나만 놓으면 끝이다. */
				  cd_table->linear.num_ents *
					  sizeof(struct arm_smmu_cd),
				  cd_table->linear.table, cd_table->cdtab_dma);
	}
}

/* Stream table manipulation functions */
/*
 * [한국어]
 * arm_smmu_write_strtab_l1_desc - 2단계 스트림 표의 위쪽 항목을 채운다
 *
 * @dst: 위쪽 표의 그 자리.
 * @l2ptr_dma: 아래쪽 표의 장치 쪽 주소.
 *
 * 문맥 표의 같은 이름 함수와 짜임이 같지만 SPAN 필드가 더 있다. 그 필드는
 * 아래쪽 표가 몇 개의 스트림을 담는지 알려 주며, 값이 항목 수의 로그에
 * 1 을 더한 형태라는 것이 규격의 약속이다(0 은 "무효"를 뜻하기 때문이다).
 *
 * 실행 컨텍스트: 스트림 표 자리를 잡는 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_init_l2_strtab() → [이 함수]
 */
static void arm_smmu_write_strtab_l1_desc(struct arm_smmu_strtab_l1 *dst,
					  dma_addr_t l2ptr_dma)
{
	u64 val = 0;	/* [한국어] 지어 넣을 값. */

	val |= FIELD_PREP(STRTAB_L1_DESC_SPAN, STRTAB_SPLIT + 1);	/* [한국어] 아래쪽 표가 담는 항목 수의 로그에 1 을 더한 값 — 0 은 "무효"라서 1 부터 센다. */
	val |= l2ptr_dma & STRTAB_L1_DESC_L2PTR_MASK;	/* [한국어] 아래쪽 표의 주소. */

	/* The HW has 64 bit atomicity with stores to the L2 STE table */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 이 자리를 원자적으로 읽으므로
	 * 주소와 범위를 한 번에 발표할 수 있다. */
	WRITE_ONCE(dst->l2ptr, cpu_to_le64(val));	/* [한국어] 컴파일러가 쪼개지 못하게 막으며 하드웨어 형식으로 쓴다. */
}

/* [한국어] 스트림 표 항목 전용 기록기 — 공통 기록기에 스트림 번호를 덧붙인 것.
 *
 * 항목을 고쳐 쓴 뒤 무효화 명령을 내려면 어느 스트림인지 알아야 하는데,
 * 공통 구조에는 그 자리가 없어 이렇게 감싼다. */
struct arm_smmu_ste_writer {
	/* [한국어] 공통 기록기 몸통 — 반드시 첫 필드여야 한다.
	 * 설정자: arm_smmu_write_ste() 가 연산표와 장치를 채운다.
	 * 읽는 자: arm_smmu_write_entry() 가 갈고리를 부를 때.
	 * 값 범위: ops 는 언제나 arm_smmu_ste_writer_ops.
	 * 동기화: 스택 변수라 공유되지 않는다. */
	struct arm_smmu_entry_writer writer;
	/* [한국어] 고쳐 쓰는 항목의 스트림 번호.
	 * 설정자: 위와 같은 자리.
	 * 읽는 자: sync 갈고리가 무효화 명령의 인자로 쓴다.
	 * 값 범위: 이 장치가 가진 스트림 번호 중 하나.
	 * 동기화: 없음. */
	u32 sid;
};

/*
 * [한국어]
 * arm_smmu_ste_writer_sync_entry - 항목 쓰기 단계마다 설정 캐시를 씻는다
 *
 * @writer: 공통 기록기 (감싼 구조에서 스트림 번호를 꺼낸다).
 *
 * 서술자 쪽과 달리 묶음을 쓰지 않고 명령 하나를 바로 낸다. 스트림 번호가
 * 하나뿐이라 묶을 것이 없기 때문이다 — 여러 스트림을 도는 일은 바깥의
 * arm_smmu_install_ste_for_dev() 가 맡는다.
 *
 * 실행 컨텍스트: 항목 쓰기 경로. 완료까지 기다린다.
 *
 * 호출 체인:
 *   arm_smmu_write_entry() → ops->sync = [이 함수]
 *     → arm_smmu_cmdq_issue_cmd_with_sync()
 */
static void arm_smmu_ste_writer_sync_entry(struct arm_smmu_entry_writer *writer)
{
	struct arm_smmu_ste_writer *ste_writer =	/* [한국어] 공통 기록기에서 항목 전용 구조로 되짚는다. */
		container_of(writer, struct arm_smmu_ste_writer, writer);
	struct arm_smmu_cmdq_ent cmd = {	/* [한국어] 낼 무효화 명령. */
		.opcode	= CMDQ_OP_CFGI_STE,	/* [한국어] 스트림 표 항목 하나를 무효화한다. */
		.cfgi	= {
			.sid	= ste_writer->sid,	/* [한국어] 방금 고쳐 쓴 항목. */
			.leaf	= true,	/* [한국어] 항목만 버린다 — 위쪽 표는 건드리지 않았기 때문이다. */
		},
	};

	arm_smmu_cmdq_issue_cmd_with_sync(writer->master->smmu, &cmd);	/* [한국어] 완료까지 기다린다 — 다음 단계로 넘어가기 전에 하드웨어가 새 값을 봐야 한다. */
}

/* [한국어] 스트림 표 항목용 기록기 연산표.
 *
 * 서술자 쪽과 달리 get_update_safe 가 있다 — EATS 나 MEV 처럼 갱신 도중
 * 달라져도 안전한 비트가 STE 에는 존재하기 때문이다. */
static const struct arm_smmu_entry_writer_ops arm_smmu_ste_writer_ops = {
	.sync = arm_smmu_ste_writer_sync_entry,	/* [한국어] 단계마다 설정 캐시를 씻는 갈고리. */
	.get_used = arm_smmu_get_ste_used,	/* [한국어] 읽히는 비트를 계산하는 갈고리. */
	.get_update_safe = arm_smmu_get_ste_update_safe,	/* [한국어] 완화할 수 있는 비트를 찾는 갈고리. */
};

/*
 * [한국어]
 * arm_smmu_write_ste - 스트림 표 항목 하나를 안전하게 고쳐 쓴다
 *
 * @master: 그 스트림을 가진 장치.
 * @sid: 고칠 스트림 번호.
 * @ste: 표 안의 그 항목이 놓인 자리.
 * @target: 최종적으로 들어가야 할 값.
 *
 * 공통 쓰기 규약에 항목용 연산표를 얹어 부른 뒤, 미리 읽기 명령을 하나
 * 덧붙인다. 방금 항목을 바꿨다는 것은 곧 그 스트림이 쓰일 참이라는 뜻이라,
 * 하드웨어가 미리 읽어 캐시에 올려 두면 첫 접근의 지연이 줄어든다.
 * 그 명령이 망가진 하드웨어에서는 옵션으로 건너뛴다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 완료를 기다리므로 시간이 걸린다.
 *
 * 호출 체인:
 *   arm_smmu_install_ste_for_dev() → [이 함수] → arm_smmu_write_entry()
 */
static void arm_smmu_write_ste(struct arm_smmu_master *master, u32 sid,
			       struct arm_smmu_ste *ste,
			       const struct arm_smmu_ste *target)
{
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 명령을 넣을 하드웨어. */
	struct arm_smmu_ste_writer ste_writer = {	/* [한국어] 항목용 기록기를 스택에 짓는다. */
		.writer = {	/* [한국어] 기록기 몸통 — 항목 쓰기 규약이 이 포인터로 갈고리를 부른다. */
			.ops = &arm_smmu_ste_writer_ops,	/* [한국어] 항목용 연산표. */
			.master = master,	/* [한국어] 무효화 명령을 낼 장치. */
		},
		.sid = sid,	/* [한국어] 무효화 명령의 인자가 된다. */
	};

	arm_smmu_write_entry(&ste_writer.writer, ste->data, target->data);	/* [한국어] 하드웨어가 중간 상태를 못 보게 나눠 쓴다. */

	/* It's likely that we'll want to use the new STE soon */
	/* [한국어] (위 영어 주석 참고) 항목을 바꿨다는 것은 곧 그 스트림이 쓰일
	 * 참이라는 뜻이다. 미리 읽어 두면 첫 DMA 의 지연이 줄어든다. */
	if (!(smmu->options & ARM_SMMU_OPT_SKIP_PREFETCH)) {	/* [한국어] 그 명령이 망가진 하드웨어에서는 건너뛴다. */
		struct arm_smmu_cmdq_ent	/* [한국어] 미리 읽기 명령. */
			prefetch_cmd = { .opcode = CMDQ_OP_PREFETCH_CFG,
					 .prefetch = {
						 .sid = sid,	/* [한국어] 방금 고친 스트림. */
					 } };

		arm_smmu_cmdq_issue_cmd(smmu, &prefetch_cmd);	/* [한국어] 완료를 기다리지 않는다 — 실패해도 정확성에는 영향이 없는 힌트일 뿐이다. */
	}
}

/*
 * [한국어]
 * arm_smmu_make_abort_ste - 모든 접근을 거부하는 항목을 짓는다
 *
 * @target: 지어 담을 자리.
 *
 * 가장 단순한 항목이다. 유효 비트와 "중단" 설정 갈래만 있으면 되고,
 * 나머지 워드는 읽히지 않으므로 0 으로 둔다. 장치를 떼거나, 아직 도메인이
 * 없거나, 게스트가 무효 항목을 요청했을 때 쓴다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   붙이기·떼기 경로, 중첩 도메인 경로 → [이 함수]
 */
void arm_smmu_make_abort_ste(struct arm_smmu_ste *target)
{
	memset(target, 0, sizeof(*target));	/* [한국어] 나머지 워드는 읽히지 않지만 깨끗이 해 두면 진단이 쉽다. */
	target->data[0] = cpu_to_le64(	/* [한국어] 첫 워드를 짓는다 — 하드웨어가 리틀엔디안으로 읽으므로 변환해 담는다. */
		STRTAB_STE_0_V |	/* [한국어] 항목 자체는 유효하다 — 무효로 두면 하드웨어가 다른 오류를 낸다. */
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_ABORT));	/* [한국어] 설정 갈래를 "중단"으로 — 이 스트림의 모든 접근이 거부된다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_make_abort_ste);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * [한국어]
 * arm_smmu_make_bypass_ste - 변환 없이 통과시키는 항목을 짓는다
 *
 * @smmu: 대상 하드웨어 (능력에 따라 필드가 하나 더 붙는다).
 * @target: 지어 담을 자리.
 *
 * 그 스트림의 DMA 주소를 그대로 물리 주소로 쓰게 한다. IOMMU 를 켜기 전
 * 상태를 이어 가야 하는 장치나, 사용자가 통과 도메인을 요청했을 때 쓴다.
 *
 * 메모리 속성 덮어쓰기를 지원하는 하드웨어에서는 공유 속성 필드를 하나 더
 * 채운다 — 통과 트래픽의 캐시 일관성을 장치가 낸 값 그대로 따르게 한다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   통과 도메인 붙이기, 시험 준비 → [이 함수]
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 값을 기준으로 전환을 검증한다. */
void arm_smmu_make_bypass_ste(struct arm_smmu_device *smmu,
			      struct arm_smmu_ste *target)
{
	memset(target, 0, sizeof(*target));	/* [한국어] 깨끗이 하고 시작한다. */
	target->data[0] = cpu_to_le64(	/* [한국어] 첫 워드 — 유효 표시와 설정 갈래. */
		STRTAB_STE_0_V |	/* [한국어] 유효 표시. */
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_BYPASS));	/* [한국어] 변환하지 않고 통과시킨다. */

	if (smmu->features & ARM_SMMU_FEAT_ATTR_TYPES_OVR)	/* [한국어] 메모리 속성을 덮어쓸 수 있는 하드웨어라면. */
		target->data[1] = cpu_to_le64(FIELD_PREP(STRTAB_STE_1_SHCFG,	/* [한국어] 공유 속성을 장치가 낸 값 그대로 쓰게 한다. */
							 STRTAB_STE_1_SHCFG_INCOMING));	/* [한국어] 덮어쓰지 않고 들어온 대로 — 통과의 뜻에 맞다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_make_bypass_ste);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * [한국어]
 * arm_smmu_make_cdtable_ste - 문맥 서술자 표를 가리키는 1단계 항목을 짓는다
 *
 * @target: 지어 담을 자리.
 * @master: 그 표를 가진 장치.
 * @ats_enabled: 이번 붙이기에서 ATS 를 켜기로 했는가.
 * @s1dss: 서술자 없는 PASID 로 온 트랜잭션을 어떻게 다룰지.
 *
 * 가장 흔한 항목이다. 장치가 낸 주소를 그 장치의 문맥 서술자 표를 거쳐
 * 1단계 페이지 테이블로 번역하게 만든다.
 *
 * 멈춤 처리가 뒤집혀 있는 것에 주의해야 한다. S1STALLD 는 "멈춤을 끄라"는
 * 뜻이므로, 하드웨어가 멈춤을 지원하는데 이 장치가 쓰지 않을 때 세운다.
 *
 * 스트림 세계(STRW) 선택도 중요하다. CPU 가 가상화 호스트 확장(E2H)으로
 * 도는 시스템에서는 ASID 브로드캐스트가 EL2 기준으로 오가므로, SMMU 도
 * 같은 세계로 맞춰야 SVA 에서 CPU 의 TLB 무효화가 SMMU 에도 닿는다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev() → [이 함수]
 */
VISIBLE_IF_KUNIT	/* [한국어] 단위 시험이 이 항목으로 전환을 검증한다. */
void arm_smmu_make_cdtable_ste(struct arm_smmu_ste *target,
			       struct arm_smmu_master *master, bool ats_enabled,
			       unsigned int s1dss)
{
	struct arm_smmu_ctx_desc_cfg *cd_table = &master->cd_table;	/* [한국어] 항목에 담을 표 주소와 형식이 여기 있다. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 능력 비트를 볼 하드웨어. */

	memset(target, 0, sizeof(*target));	/* [한국어] 깨끗이 하고 시작한다. */
	target->data[0] = cpu_to_le64(	/* [한국어] 첫 워드 — 유효 표시, 갈래, 그리고 문맥 표 정보. */
		STRTAB_STE_0_V |	/* [한국어] 유효 표시. */
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S1_TRANS) |	/* [한국어] 1단계 변환만 한다. */
		FIELD_PREP(STRTAB_STE_0_S1FMT, cd_table->s1fmt) |	/* [한국어] 서술자 표가 평면인지 2단계인지. */
		(cd_table->cdtab_dma & STRTAB_STE_0_S1CTXPTR_MASK) |	/* [한국어] 그 표의 장치 쪽 주소. */
		FIELD_PREP(STRTAB_STE_0_S1CDMAX, cd_table->s1cdmax));	/* [한국어] 표가 담는 서술자 수 — 이 값을 넘는 PASID 는 오류가 된다. */

	target->data[1] = cpu_to_le64(	/* [한국어] 둘째 워드 — 서술자 표를 읽는 방식과 ATS 처리. */
		FIELD_PREP(STRTAB_STE_1_S1DSS, s1dss) |	/* [한국어] 서술자 없는 PASID 를 어떻게 다룰지 — 우회, 중단, 또는 0번 서술자 사용. */
		FIELD_PREP(STRTAB_STE_1_S1CIR, STRTAB_STE_1_S1C_CACHE_WBRA) |	/* [한국어] 서술자 표를 읽을 때의 안쪽 캐시 정책 — 되쓰기·할당. */
		FIELD_PREP(STRTAB_STE_1_S1COR, STRTAB_STE_1_S1C_CACHE_WBRA) |	/* [한국어] 바깥 캐시 정책도 같게. */
		FIELD_PREP(STRTAB_STE_1_S1CSH, ARM_SMMU_SH_ISH) |	/* [한국어] 안쪽 공유 영역 — CPU 가 고친 표를 SMMU 가 보려면 같은 영역이어야 한다. */
		((smmu->features & ARM_SMMU_FEAT_STALLS &&	/* [한국어] 하드웨어가 멈춤을 지원하는데. */
		  !master->stall_enabled) ?	/* [한국어] 이 장치는 멈춤을 쓰지 않는다면. */
			 STRTAB_STE_1_S1STALLD :	/* [한국어] 멈춤을 끄라고 세운다 — 이름 그대로 "STALL Disable"이라 뜻이 뒤집혀 있다. */
			 0) |	/* [한국어] 그 밖에는 세우지 않는다. */
		FIELD_PREP(STRTAB_STE_1_EATS,	/* [한국어] 장치 쪽 변환 캐시 처리 방식. */
			   ats_enabled ? STRTAB_STE_1_EATS_TRANS : 0));	/* [한국어] 켜면 장치의 ATS 요청을 받아 주고, 끄면 거부한다. */

	if ((smmu->features & ARM_SMMU_FEAT_ATTR_TYPES_OVR) &&	/* [한국어] 메모리 속성 덮어쓰기를 지원하고. */
	    s1dss == STRTAB_STE_1_S1DSS_BYPASS)	/* [한국어] 서술자 없는 PASID 를 통과시키는 설정이라면. */
		target->data[1] |= cpu_to_le64(FIELD_PREP(	/* [한국어] 그 통과 트래픽의 공유 속성을 정한다. */
			STRTAB_STE_1_SHCFG, STRTAB_STE_1_SHCFG_INCOMING));	/* [한국어] 장치가 낸 값 그대로 쓴다. */

	if (smmu->features & ARM_SMMU_FEAT_E2H) {	/* [한국어] CPU 가 가상화 호스트 확장으로 도는 시스템이라면. */
		/*
		 * To support BTM the streamworld needs to match the
		 * configuration of the CPU so that the ASID broadcasts are
		 * properly matched. This means either S/NS-EL2-E2H (hypervisor)
		 * or NS-EL1 (guest). Since an SVA domain can be installed in a
		 * PASID this should always use a BTM compatible configuration
		 * if the HW supports it.
		 */
		/* [한국어] (위 영어 주석 참고) SVA 에서는 CPU 가 TLB 를 비울 때 그
		 * 브로드캐스트가 SMMU 에도 닿아야 한다. 그러려면 SMMU 가 CPU 와 같은
		 * "스트림 세계"에 있어야 ASID 가 짝지어진다. 어느 PASID 에나 SVA
		 * 도메인이 붙을 수 있으므로, 하드웨어가 지원하면 언제나 맞춰 둔다. */
		target->data[1] |= cpu_to_le64(
			FIELD_PREP(STRTAB_STE_1_STRW, STRTAB_STE_1_STRW_EL2));	/* [한국어] EL2 세계로 맞춘다. */
	} else {	/* [한국어] 그 확장이 없으면. */
		target->data[1] |= cpu_to_le64(	/* [한국어] 스트림 세계를 정하는 필드를 덧붙인다. */
			FIELD_PREP(STRTAB_STE_1_STRW, STRTAB_STE_1_STRW_NSEL1));	/* [한국어] 비보안 EL1 세계를 쓴다. */

		/*
		 * VMID 0 is reserved for stage-2 bypass EL1 STEs, see
		 * arm_smmu_domain_alloc_id()
		 */
		/* [한국어] (위 영어 주석 참고) EL1 세계에서는 2단계를 하지 않아도 VMID 가
		 * TLB 태그에 들어간다. 그래서 0번을 "2단계 없음"용으로 예약해 두고,
		 * 실제 2단계 도메인에는 절대 배정하지 않는다. */
		target->data[2] =
			cpu_to_le64(FIELD_PREP(STRTAB_STE_2_S2VMID, 0));	/* [한국어] 예약된 0번을 명시적으로 넣는다. */
	}
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_make_cdtable_ste);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * [한국어]
 * arm_smmu_make_s2_domain_ste - 2단계 변환 항목을 짓는다
 *
 * @target: 지어 담을 자리.
 * @master: 이 항목을 쓸 장치.
 * @smmu_domain: 그 2단계 도메인 (VMID 와 페이지 테이블이 여기 있다).
 * @ats_enabled: ATS 를 켤 것인가.
 *
 * 게스트에게 장치를 넘길 때 그 게스트를 가두는 항목이다. 1단계 항목과 달리
 * 문맥 서술자 표를 거치지 않고 곧바로 2단계 페이지 테이블로 간다.
 *
 * S2PTW 를 언제나 켜는 것이 중요하다 — 1단계 페이지 테이블을 읽는 접근도
 * 2단계를 거치게 만든다. 게스트가 자기 1단계 테이블을 아무 물리 주소에나
 * 두어도 그 주소가 2단계를 통과해야 하므로, 게스트가 울타리를 넘을 수 없다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev()/중첩 도메인 경로 → [이 함수]
 */
void arm_smmu_make_s2_domain_ste(struct arm_smmu_ste *target,
				 struct arm_smmu_master *master,
				 struct arm_smmu_domain *smmu_domain,
				 bool ats_enabled)
{
	struct arm_smmu_s2_cfg *s2_cfg = &smmu_domain->s2_cfg;	/* [한국어] 이 도메인의 VMID 가 여기 있다. */
	const struct io_pgtable_cfg *pgtbl_cfg =	/* [한국어] io-pgtable 이 계산해 둔 2단계 설정. */
		&io_pgtable_ops_to_pgtable(smmu_domain->pgtbl_ops)->cfg;
	typeof(&pgtbl_cfg->arm_lpae_s2_cfg.vtcr) vtcr =	/* [한국어] 변환 제어 필드 묶음의 별칭. */
		&pgtbl_cfg->arm_lpae_s2_cfg.vtcr;
	u64 vtcr_val;	/* [한국어] 그 필드들을 하나로 짠 값. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 능력 비트를 볼 하드웨어. */

	memset(target, 0, sizeof(*target));	/* [한국어] 깨끗이 하고 시작한다. */
	target->data[0] = cpu_to_le64(	/* [한국어] 첫 워드 — 유효 표시와 2단계 갈래. */
		STRTAB_STE_0_V |	/* [한국어] 유효 표시. */
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S2_TRANS));	/* [한국어] 2단계 변환만 한다 — 1단계는 건너뛴다. */

	target->data[1] = cpu_to_le64(	/* [한국어] 둘째 워드 — ATS 처리. */
		FIELD_PREP(STRTAB_STE_1_EATS,	/* [한국어] 장치 쪽 변환 캐시 처리. */
			   ats_enabled ? STRTAB_STE_1_EATS_TRANS : 0));

	if (pgtbl_cfg->quirks & IO_PGTABLE_QUIRK_ARM_S2FWB)	/* [한국어] 2단계가 캐시 속성을 강제하도록 테이블을 지었다면. */
		target->data[1] |= cpu_to_le64(STRTAB_STE_1_S2FWB);	/* [한국어] 게스트가 캐시를 우회하지 못하게 막는 비트다 — 중첩 변환의 안전 조건 중 하나. */
	if (smmu->features & ARM_SMMU_FEAT_ATTR_TYPES_OVR)	/* [한국어] 메모리 속성 덮어쓰기를 지원하면. */
		target->data[1] |= cpu_to_le64(FIELD_PREP(STRTAB_STE_1_SHCFG,	/* [한국어] 공유 속성을 들어온 값 그대로 쓴다. */
							  STRTAB_STE_1_SHCFG_INCOMING));

	vtcr_val = FIELD_PREP(STRTAB_STE_2_VTCR_S2T0SZ, vtcr->tsz) |	/* [한국어] 입력 주소 폭. */
		   FIELD_PREP(STRTAB_STE_2_VTCR_S2SL0, vtcr->sl) |	/* [한국어] 표를 걷기 시작할 단계 — 2단계 표는 주소 폭에 따라 시작 단계가 다르다. */
		   FIELD_PREP(STRTAB_STE_2_VTCR_S2IR0, vtcr->irgn) |	/* [한국어] 안쪽 캐시 정책. */
		   FIELD_PREP(STRTAB_STE_2_VTCR_S2OR0, vtcr->orgn) |	/* [한국어] 바깥 캐시 정책. */
		   FIELD_PREP(STRTAB_STE_2_VTCR_S2SH0, vtcr->sh) |	/* [한국어] 공유 영역. */
		   FIELD_PREP(STRTAB_STE_2_VTCR_S2TG, vtcr->tg) |	/* [한국어] 알갱이 크기. */
		   FIELD_PREP(STRTAB_STE_2_VTCR_S2PS, vtcr->ps);	/* [한국어] 출력 물리 주소 폭. */
	target->data[2] = cpu_to_le64(	/* [한국어] 셋째 워드 — VMID 와 2단계 변환 제어. */
		FIELD_PREP(STRTAB_STE_2_S2VMID, s2_cfg->vmid) |	/* [한국어] 이 가상 기계의 번호 — TLB 항목의 태그가 된다. */
		FIELD_PREP(STRTAB_STE_2_VTCR, vtcr_val) |	/* [한국어] 위에서 짠 변환 제어 값. */
		STRTAB_STE_2_S2AA64 |	/* [한국어] AArch64 형식의 2단계 테이블. */
#ifdef __BIG_ENDIAN	/* [한국어] 빅엔디안 커널에서만. */
		STRTAB_STE_2_S2ENDI |	/* [한국어] 표 항목을 빅엔디안으로 읽으라고 알린다. */
#endif
		STRTAB_STE_2_S2PTW |	/* [한국어] 1단계 표를 읽는 접근도 2단계를 거치게 한다 — 게스트를 가두는 핵심 비트다. */
		(master->stall_enabled ? STRTAB_STE_2_S2S : 0) |	/* [한국어] 멈춤을 쓰는 장치면 2단계 폴트에서도 트랜잭션을 붙잡아 둔다. */
		STRTAB_STE_2_S2R);	/* [한국어] 2단계 폴트를 이벤트 큐에 보고한다. */

	target->data[3] = cpu_to_le64(pgtbl_cfg->arm_lpae_s2_cfg.vttbr &	/* [한국어] 2단계 페이지 테이블의 뿌리 주소. */
				      STRTAB_STE_3_S2TTB_MASK);	/* [한국어] 규격이 쓰는 비트만 남긴다. */
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_make_s2_domain_ste);	/* [한국어] 단위 시험 모듈이 링크할 수 있게 내보낸다. */

/*
 * This can safely directly manipulate the STE memory without a sync sequence
 * because the STE table has not been installed in the SMMU yet.
 */
/*
 * [한국어]
 * arm_smmu_init_initial_stes - 새 스트림 표를 모두 "중단"으로 채운다
 *
 * @strtab: 채울 표의 시작.
 * @nent: 항목 수.
 *
 * (위 영어 주석 참고) 표를 하드웨어에 걸기 전이라 쓰기 규약을 지킬 필요가
 * 없다 — 아무도 이 메모리를 읽고 있지 않기 때문이다. 그래서 항목마다
 * 무효화를 내는 비싼 경로를 건너뛰고 곧바로 채운다.
 *
 * 초기값을 "중단"으로 두는 것이 안전의 기본이다. 아직 어느 장치도 붙지
 * 않았으므로, 그 사이에 들어온 DMA 는 모두 거부되어야 한다.
 *
 * 실행 컨텍스트: 프로브·표 확장 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_init_strtab()/init_l2_strtab() → [이 함수]
 */
static void arm_smmu_init_initial_stes(struct arm_smmu_ste *strtab,
				       unsigned int nent)
{
	unsigned int i;	/* [한국어] 항목 반복자. */

	for (i = 0; i < nent; ++i) {	/* [한국어] 모든 항목을. */
		arm_smmu_make_abort_ste(strtab);	/* [한국어] 접근을 거부하는 값으로 곧바로 쓴다. */
		strtab++;	/* [한국어] 다음 항목으로. */
	}
}

/*
 * [한국어]
 * arm_smmu_init_l2_strtab - 그 스트림 번호가 속한 아래쪽 표를 마련한다
 *
 * @smmu: 대상 하드웨어.
 * @sid: 쓰려는 스트림 번호.
 * @return: 0 성공(이미 있어도 성공), -ENOMEM 실패.
 *
 * 2단계 스트림 표에서 실제로 쓰이는 구간만 게으르게 잡는 함수다. 장치를
 * 처음 프로브할 때 그 장치의 스트림 번호로 불려, 그 구간의 표가 없으면
 * 잡아 채우고 위쪽 표에 걸어 준다.
 *
 * 순서가 중요하다 — 표를 "중단"으로 다 채운 뒤에야 위쪽 표에 건다.
 * 반대로 하면 아직 쓰레기가 든 표를 하드웨어가 읽을 수 있다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_insert_master() → [이 함수] → dmam_alloc_coherent()
 */
static int arm_smmu_init_l2_strtab(struct arm_smmu_device *smmu, u32 sid)
{
	dma_addr_t l2ptr_dma;	/* [한국어] 잡은 표의 장치 쪽 주소. */
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;	/* [한국어] 스트림 표 설정. */
	struct arm_smmu_strtab_l2 **l2table;	/* [한국어] 그 구간의 표 자리. */

	l2table = &cfg->l2.l2ptrs[arm_smmu_strtab_l1_idx(sid)];	/* [한국어] 이 스트림 번호가 속한 구간. */
	if (*l2table)	/* [한국어] 이미 잡혀 있으면. */
		return 0;	/* [한국어] 할 일이 없다 — 같은 구간의 다른 장치가 먼저 잡았을 수 있다. */

	*l2table = dmam_alloc_coherent(smmu->dev, sizeof(**l2table),	/* [한국어] devres 판이라 장치가 사라질 때 저절로 놓인다. */
				       &l2ptr_dma, GFP_KERNEL);
	if (!*l2table) {	/* [한국어] 메모리가 없으면. */
		dev_err(smmu->dev,	/* [한국어] 그 장치는 IOMMU 뒤에 설 수 없게 되므로 오류로 남긴다. */
			"failed to allocate l2 stream table for SID %u\n",
			sid);
		return -ENOMEM;	/* [한국어] 메모리가 없어 더 진행할 수 없다. */
	}

	arm_smmu_init_initial_stes((*l2table)->stes,	/* [한국어] 새 표를 모두 "중단"으로 채운다 — 아직 걸기 전이라 규약 없이 써도 된다. */
				   ARRAY_SIZE((*l2table)->stes));
	arm_smmu_write_strtab_l1_desc(&cfg->l2.l1tab[arm_smmu_strtab_l1_idx(sid)],	/* [한국어] 다 채운 뒤에야 위쪽 표에 건다. */
				      l2ptr_dma);
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_streams_cmp_key - 스트림 번호와 트리 노드를 견준다
 *
 * @lhs: 찾는 스트림 번호의 주소.
 * @rhs: 트리 노드.
 * @return: 음수/0/양수.
 *
 * 스트림 번호로 장치를 되찾는 rb 트리의 검색 비교 함수다. 폴트가 났을 때
 * 그 번호로 어느 장치인지 알아내야 하는데, 스트림 번호 공간이 넓어 배열로는
 * 감당할 수 없어 트리를 쓴다.
 *
 * 실행 컨텍스트: 이벤트 처리와 장치 등록. 잠들지 않는다.
 *
 * 호출 체인:
 *   rb_find() → [이 함수]
 */
static int arm_smmu_streams_cmp_key(const void *lhs, const struct rb_node *rhs)
{
	struct arm_smmu_stream *stream_rhs =	/* [한국어] 노드에서 스트림 구조로 되짚는다. */
		rb_entry(rhs, struct arm_smmu_stream, node);
	const u32 *sid_lhs = lhs;	/* [한국어] 찾는 번호. */

	if (*sid_lhs < stream_rhs->id)	/* [한국어] 작으면 왼쪽으로. */
		return -1;
	if (*sid_lhs > stream_rhs->id)	/* [한국어] 크면 오른쪽으로. */
		return 1;
	return 0;	/* [한국어] 찾았다. */
}

/*
 * [한국어]
 * arm_smmu_streams_cmp_node - 트리 노드 두 개를 견준다
 *
 * @lhs: 넣으려는 노드.
 * @rhs: 이미 트리에 있는 노드.
 * @return: 음수/0/양수.
 *
 * 삽입용 비교 함수. 왼쪽 노드에서 번호를 꺼내 위의 검색 비교를 그대로
 * 재사용한다 — 두 비교가 어긋나면 트리가 망가지므로 한 곳에 모은 것이다.
 *
 * 실행 컨텍스트: 장치 등록. 잠들지 않는다.
 *
 * 호출 체인:
 *   rb_find_add() → [이 함수] → arm_smmu_streams_cmp_key()
 */
static int arm_smmu_streams_cmp_node(struct rb_node *lhs,
				     const struct rb_node *rhs)
{
	return arm_smmu_streams_cmp_key(	/* [한국어] 검색 비교를 그대로 쓴다. */
		&rb_entry(lhs, struct arm_smmu_stream, node)->id, rhs);
}

/*
 * [한국어]
 * arm_smmu_find_master - 스트림 번호로 장치를 되찾는다
 *
 * @smmu: 대상 하드웨어.
 * @sid: 찾을 스트림 번호.
 * @return: 그 장치의 master, 없으면 NULL.
 *
 * 하드웨어가 낸 폴트에는 스트림 번호만 실려 있어, 그것으로 어느 장치인지
 * 알아내야 로그를 찍고 폴트를 그 장치의 처리기로 넘길 수 있다.
 *
 * 락을 잡고 들어와야 한다는 조건이 중요하다 — 찾은 장치가 곧바로 사라지면
 * 안 되기 때문이다. 호출자가 락 안에서 참조를 잡는다.
 *
 * 실행 컨텍스트: 이벤트 처리. streams_mutex 를 쥔 채 불려야 한다.
 *
 * 호출 체인:
 *   arm_smmu_decode_event()/priq_thread() → [이 함수] → rb_find()
 */
static struct arm_smmu_master *
arm_smmu_find_master(struct arm_smmu_device *smmu, u32 sid)
{
	struct rb_node *node;	/* [한국어] 찾은 노드. */

	lockdep_assert_held(&smmu->streams_mutex);	/* [한국어] 락을 쥐고 들어왔는지 디버그 빌드에서 확인한다. */

	node = rb_find(&sid, &smmu->streams, arm_smmu_streams_cmp_key);	/* [한국어] 번호로 트리를 뒤진다. */
	if (!node)	/* [한국어] 등록되지 않은 번호라면. */
		return NULL;	/* [한국어] 펌웨어가 기술하지 않은 장치가 DMA 를 낸 경우다. */
	return rb_entry(node, struct arm_smmu_stream, node)->master;	/* [한국어] 그 스트림을 가진 장치. */
}

/* IRQ and event handlers */
/*
 * [한국어]
 * arm_smmu_decode_event - 이벤트 큐의 원시 워드를 구조체로 푼다
 *
 * @smmu: 대상 하드웨어.
 * @raw: 큐에서 읽은 워드들.
 * @event: 풀어 담을 구조체.
 *
 * 하드웨어가 비트로 욱여넣은 폴트 기록을 사람과 코드가 다루기 좋은 형태로
 * 펼친다. 필드 하나하나가 규격이 정한 자리에서 나온다.
 *
 * 마지막에 스트림 번호로 장치를 찾아 참조를 잡는 것이 중요하다. 폴트를
 * 처리하는 동안 그 장치가 떨어져 나가면 안 되므로, 락 안에서 참조를 올려
 * 둔다 — 처리기가 나중에 put_device 로 놓는다.
 *
 * 실행 컨텍스트: 이벤트 큐 인터럽트 스레드. mutex 를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread() → [이 함수] → arm_smmu_find_master()
 */
static void arm_smmu_decode_event(struct arm_smmu_device *smmu, u64 *raw,
				  struct arm_smmu_event *event)
{
	struct arm_smmu_master *master;	/* [한국어] 찾아낸 장치. */

	event->id = FIELD_GET(EVTQ_0_ID, raw[0]);	/* [한국어] 이벤트 종류 — 처리 갈래를 가르는 값이다. */
	event->sid = FIELD_GET(EVTQ_0_SID, raw[0]);	/* [한국어] 어느 스트림에서 났는가. */
	event->ssv = FIELD_GET(EVTQ_0_SSV, raw[0]);	/* [한국어] PASID 가 실려 있는가. */
	event->ssid = event->ssv ? FIELD_GET(EVTQ_0_SSID, raw[0]) : IOMMU_NO_PASID;	/* [한국어] 유효할 때만 꺼내고, 아니면 "PASID 없음"으로 둔다 — 쓰레기 값을 들고 다니지 않는다. */
	event->privileged = FIELD_GET(EVTQ_1_PnU, raw[1]);	/* [한국어] 특권 접근이었는가. */
	event->instruction = FIELD_GET(EVTQ_1_InD, raw[1]);	/* [한국어] 명령 인출이었는가. */
	event->s2 = FIELD_GET(EVTQ_1_S2, raw[1]);	/* [한국어] 2단계에서 난 오류인가 — 게스트에게 돌릴지 가르는 기준. */
	event->read = FIELD_GET(EVTQ_1_RnW, raw[1]);	/* [한국어] 읽기였는가 쓰기였는가. */
	event->stag = FIELD_GET(EVTQ_1_STAG, raw[1]);	/* [한국어] 멈춤 꼬리표 — 응답할 때 이 값으로 짝을 찾는다. */
	event->stall = FIELD_GET(EVTQ_1_STALL, raw[1]);	/* [한국어] 트랜잭션이 멈춰 서 있는가 — 참이면 반드시 응답해야 한다. */
	event->class = FIELD_GET(EVTQ_1_CLASS, raw[1]);	/* [한국어] 어느 단계의 접근에서 났는가. */
	event->iova = FIELD_GET(EVTQ_2_ADDR, raw[2]);	/* [한국어] 폴트가 난 입력 주소 — 폴트 처리기가 이 주소를 채운다. */
	event->ipa = raw[3] & EVTQ_3_IPA;	/* [한국어] 2단계 오류에서의 중간 물리 주소. */
	event->fetch_addr = raw[3] & EVTQ_3_FETCH_ADDR;	/* [한국어] 표를 읽다 넘어졌다면 그 표 항목의 주소. */
	event->ttrnw = FIELD_GET(EVTQ_1_TT_READ, raw[1]);	/* [한국어] 표 순회가 읽기였는가. */
	event->class_tt = false;	/* [한국어] 기본은 거짓 — 아래에서 조건에 맞을 때만 세운다. */
	event->dev = NULL;	/* [한국어] 장치를 못 찾을 수도 있으므로 먼저 비워 둔다. */

	if (event->id == EVT_ID_PERMISSION_FAULT)	/* [한국어] 권한 오류일 때만. */
		event->class_tt = (event->class == EVTQ_1_CLASS_TT);	/* [한국어] 표 순회 중이었는지 판정한다 — 그때만 fetch_addr 이 뜻을 가진다. */

	mutex_lock(&smmu->streams_mutex);	/* [한국어] 트리를 뒤지는 동안 장치가 등록·해제되면 안 된다. */
	master = arm_smmu_find_master(smmu, event->sid);	/* [한국어] 스트림 번호로 장치를 찾는다. */
	if (master)	/* [한국어] 찾았다면. */
		event->dev = get_device(master->dev);	/* [한국어] 참조를 올려 둔다 — 폴트를 처리하는 동안 사라지지 않게. 나중에 put_device 로 놓는다. */
	mutex_unlock(&smmu->streams_mutex);	/* [한국어] 트리 조작이 끝났으니 락을 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_handle_event - 폴트를 처리할 수 있는 곳으로 넘긴다
 *
 * @smmu: 대상 하드웨어.
 * @evt: 원시 이벤트 워드 (게스트에게 그대로 올릴 때 쓴다).
 * @event: 풀어 둔 이벤트 구조.
 * @return: 0 처리했다, 음수면 처리하지 못했다(호출자가 로그를 남긴다).
 *
 * 폴트가 갈 수 있는 곳은 세 갈래다.
 * 하나, 멈춰 선 트랜잭션이면 iommu 코어의 폴트 처리 계층으로 올린다 —
 * SVA 라면 페이지를 채워 주고 재시도시킬 수 있다.
 * 둘, 게스트에게 넘긴 장치의 1단계 폴트면 게스트에게 그대로 올린다.
 * 게스트가 자기 페이지 테이블을 관리하므로 게스트가 답해야 한다.
 * 셋, 그 밖에는 처리할 방법이 없어 실패를 돌려주고, 호출자가 로그를 남긴다.
 *
 * 2단계 폴트를 게스트에게 올리지 않는 것(!event->s2 조건)이 중요하다.
 * 2단계는 호스트가 관리하므로 게스트가 답할 수 없고, 그런 폴트는 호스트의
 * 설정 오류를 뜻한다.
 *
 * 실행 컨텍스트: 이벤트 큐 인터럽트 스레드. mutex 를 잡는다.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread() → [이 함수]
 *     → iommu_report_device_fault() / arm_vmaster_report_event()
 */
static int arm_smmu_handle_event(struct arm_smmu_device *smmu, u64 *evt,
				 struct arm_smmu_event *event)
{
	int ret = 0;	/* [한국어] 결과. */
	u32 perm = 0;	/* [한국어] 폴트 보고에 담을 권한 플래그 묶음. */
	struct arm_smmu_master *master;	/* [한국어] 폴트를 낸 장치. */
	struct iopf_fault fault_evt = { };	/* [한국어] 코어에 올릴 폴트 기록. */
	struct iommu_fault *flt = &fault_evt.fault;	/* [한국어] 그 안의 실제 내용. */

	switch (event->id) {	/* [한국어] 처리할 수 있는 종류인지 먼저 가른다. */
	case EVT_ID_BAD_STE_CONFIG:	/* [한국어] 아래 여덟 가지는 장치가 낸 접근에서 비롯된 것이라 */
	case EVT_ID_STREAM_DISABLED_FAULT:	/* [한국어] 그 장치에게 돌려줄 수 있다. */
	case EVT_ID_BAD_SUBSTREAMID_CONFIG:	/* [한국어] 이 값일 때의 처리. */
	case EVT_ID_BAD_CD_CONFIG:	/* [한국어] 이 값일 때의 처리. */
	case EVT_ID_TRANSLATION_FAULT:	/* [한국어] 이 값일 때의 처리. */
	case EVT_ID_ADDR_SIZE_FAULT:	/* [한국어] 이 값일 때의 처리. */
	case EVT_ID_ACCESS_FAULT:	/* [한국어] 이 값일 때의 처리. */
	case EVT_ID_PERMISSION_FAULT:	/* [한국어] 이 값일 때의 처리. */
		break;	/* [한국어] 아래로 진행한다. */
	default:	/* [한국어] 표 인출 실패처럼 드라이버 자신의 문제인 것들. */
		return -EOPNOTSUPP;	/* [한국어] 넘길 곳이 없다 — 호출자가 상세 로그를 남긴다. */
	}

	if (event->stall) {	/* [한국어] 트랜잭션이 멈춰 서 있다면 답을 돌려줄 수 있다. */
		if (event->read)	/* [한국어] 읽기였는가. */
			perm |= IOMMU_FAULT_PERM_READ;
		else
			perm |= IOMMU_FAULT_PERM_WRITE;	/* [한국어] 아니면 쓰기다. */

		if (event->instruction)	/* [한국어] 명령 인출이었다면. */
			perm |= IOMMU_FAULT_PERM_EXEC;	/* [한국어] 실행 권한이 필요하다고 알린다. */

		if (event->privileged)	/* [한국어] 특권 접근이었다면. */
			perm |= IOMMU_FAULT_PERM_PRIV;	/* [한국어] 그 사실도 함께 알린다. */

		flt->type = IOMMU_FAULT_PAGE_REQ;	/* [한국어] 페이지를 채워 달라는 요청으로 올린다. */
		flt->prm = (struct iommu_fault_page_request){	/* [한국어] 페이지 요청 형태로 폴트 내용을 채운다. */
			.flags = IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE,	/* [한국어] 이 요청이 그룹의 마지막임을 알린다 — 멈춤 방식에는 그룹 개념이 없어 언제나 마지막이다. */
			.grpid = event->stag,	/* [한국어] 멈춤 꼬리표를 그룹 번호 자리에 담는다 — 응답할 때 이 값으로 짝을 찾는다. */
			.perm = perm,	/* [한국어] 위에서 모은 권한. */
			.addr = event->iova,	/* [한국어] 채워야 할 주소. */
		};

		if (event->ssv) {	/* [한국어] PASID 가 실려 있었다면. */
			flt->prm.flags |= IOMMU_FAULT_PAGE_REQUEST_PASID_VALID;	/* [한국어] 그 사실을 표시하고. */
			flt->prm.pasid = event->ssid;	/* [한국어] 어느 주소 공간의 폴트인지 알린다. */
		}
	}

	mutex_lock(&smmu->streams_mutex);	/* [한국어] 장치를 찾아 쓰는 동안 등록·해제가 끼어들면 안 된다. */
	master = arm_smmu_find_master(smmu, event->sid);	/* [한국어] 스트림 번호로 장치를 찾는다. */
	if (!master) {	/* [한국어] 등록되지 않은 스트림이라면. */
		ret = -EINVAL;	/* [한국어] 넘길 곳이 없다 — 펌웨어 기술이 빠졌거나 하드웨어가 이상한 번호를 냈다. */
		goto out_unlock;	/* [한국어] 락을 풀고 나가는 공통 자리로 간다. */
	}

	if (event->stall)	/* [한국어] 멈춘 트랜잭션이면. */
		ret = iommu_report_device_fault(master->dev, &fault_evt);	/* [한국어] 코어의 폴트 처리 계층으로 올린다 — SVA 라면 페이지를 채우고 재시도시킨다. */
	else if (master->vmaster && !event->s2)	/* [한국어] 게스트에게 넘긴 장치의 1단계 폴트라면. */
		ret = arm_vmaster_report_event(master->vmaster, evt);	/* [한국어] 게스트가 자기 페이지 테이블을 관리하므로 게스트에게 올린다. 2단계는 호스트 몫이라 제외한다. */
	else
		ret = -EOPNOTSUPP; /* Unhandled events should be pinned */	/* [한국어] (위 영어 주석 참고) 처리할 길이 없으면 로그로 남겨 사람이 보게 한다. */
out_unlock:	/* [한국어] 성공·실패 모두 이 자리를 지난다. */
	mutex_unlock(&smmu->streams_mutex);	/* [한국어] 트리 조작이 끝났으니 락을 놓는다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_dump_raw_event - 이벤트 원본 워드를 로그에 찍는다
 *
 * @smmu: 대상 하드웨어.
 * @raw: 큐에서 읽은 원본.
 * @event: 풀어 둔 구조 (종류 번호만 쓴다).
 *
 * 풀어 낸 값만으로는 설명되지 않는 문제가 있을 때, 규격서와 직접 대조할 수
 * 있도록 원본을 그대로 남긴다.
 *
 * 실행 컨텍스트: 이벤트 처리 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_dump_event() → [이 함수]
 */
static void arm_smmu_dump_raw_event(struct arm_smmu_device *smmu, u64 *raw,
				    struct arm_smmu_event *event)
{
	int i;	/* [한국어] 워드 반복자. */

	dev_err(smmu->dev, "event 0x%02x received:\n", event->id);	/* [한국어] 어떤 종류인지 먼저 알린다. */

	for (i = 0; i < EVTQ_ENT_DWORDS; ++i)	/* [한국어] 기록 하나가 여러 워드다. */
		dev_err(smmu->dev, "\t0x%016llx\n", raw[i]);	/* [한국어] 16진수로 그대로 — 규격서와 대조할 수 있게. */
}

#define ARM_SMMU_EVT_KNOWN(e)	((e)->id < ARRAY_SIZE(event_str) && event_str[(e)->id])	/* [한국어] 이름표를 아는 종류인가 — 표 밖의 값이 와도 안전하게 다루려는 검사다. */
#define ARM_SMMU_LOG_EVT_STR(e) ARM_SMMU_EVT_KNOWN(e) ? event_str[(e)->id] : "UNKNOWN"	/* [한국어] 알면 이름을, 모르면 UNKNOWN 을 찍는다. */
#define ARM_SMMU_LOG_CLIENT(e)	(e)->dev ? dev_name((e)->dev) : "(unassigned sid)"	/* [한국어] 장치를 찾았으면 이름을, 못 찾았으면 그 사실을 남긴다 — 후자는 펌웨어 기술이 빠졌다는 단서다. */

/*
 * [한국어]
 * arm_smmu_dump_event - 처리하지 못한 폴트를 사람이 읽을 형태로 남긴다
 *
 * @smmu: 대상 하드웨어.
 * @raw: 원본 워드.
 * @evt: 풀어 둔 구조.
 * @rs: 로그 빈도 제한 상태.
 *
 * 폴트는 한 번 나기 시작하면 초당 수천 번씩 쏟아질 수 있어, 그대로 찍으면
 * 로그가 넘치고 시스템이 느려진다. 그래서 빈도 제한을 먼저 통과해야 찍는다.
 *
 * 종류에 따라 찍는 내용이 다른 것이 요점이다. 변환 실패 계열은 주소와
 * 권한이 중요하고, 표 인출 실패 계열은 어느 표 항목을 읽다 넘어졌는지가
 * 중요하다 — 후자는 대개 드라이버나 메모리 손상 문제다.
 *
 * 실행 컨텍스트: 이벤트 처리 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_evtq_thread() → [이 함수] → arm_smmu_dump_raw_event()
 */
static void arm_smmu_dump_event(struct arm_smmu_device *smmu, u64 *raw,
				struct arm_smmu_event *evt,
				struct ratelimit_state *rs)
{
	if (!__ratelimit(rs))	/* [한국어] 너무 자주 오면 건너뛴다 — 폴트 폭주로 시스템이 멈추는 것을 막는다. */
		return;

	arm_smmu_dump_raw_event(smmu, raw, evt);	/* [한국어] 먼저 원본을 남긴다. */

	switch (evt->id) {	/* [한국어] 종류에 따라 중요한 정보가 다르다. */
	case EVT_ID_TRANSLATION_FAULT:	/* [한국어] 아래 넷은 장치가 낸 접근이 실패한 경우 — */
	case EVT_ID_ADDR_SIZE_FAULT:	/* [한국어] 주소와 권한이 진단의 핵심이다. */
	case EVT_ID_ACCESS_FAULT:	/* [한국어] 이 값일 때의 처리. */
	case EVT_ID_PERMISSION_FAULT:	/* [한국어] 이 값일 때의 처리. */
		dev_err(smmu->dev, "event: %s client: %s sid: %#x ssid: %#x iova: %#llx ipa: %#llx",	/* [한국어] 어느 장치가 어느 주소를 건드렸는지. */
			ARM_SMMU_LOG_EVT_STR(evt), ARM_SMMU_LOG_CLIENT(evt),
			evt->sid, evt->ssid, evt->iova, evt->ipa);

		dev_err(smmu->dev, "%s %s %s %s \"%s\"%s%s stag: %#x",	/* [한국어] 어떤 성격의 접근이었는지 — 드라이버 버그를 좁히는 데 쓰인다. */
			evt->privileged ? "priv" : "unpriv",	/* [한국어] 특권 여부. */
			evt->instruction ? "inst" : "data",	/* [한국어] 명령 인출인지 데이터 접근인지. */
			str_read_write(evt->read),	/* [한국어] 읽기인지 쓰기인지. */
			evt->s2 ? "s2" : "s1", event_class_str[evt->class],	/* [한국어] 어느 단계에서, 어느 갈래의 접근에서 났는지. */
			evt->class_tt ? (evt->ttrnw ? " ttd_read" : " ttd_write") : "",	/* [한국어] 표 순회 중이었다면 그 방향까지. */
			evt->stall ? " stall" : "", evt->stag);	/* [한국어] 멈춰 있는지와 그 꼬리표. */

		break;

	case EVT_ID_STE_FETCH_FAULT:	/* [한국어] 아래 셋은 드라이버가 만든 자료 구조를 읽다 넘어진 경우 — */
	case EVT_ID_CD_FETCH_FAULT:	/* [한국어] 어느 주소를 읽으려다 실패했는지가 핵심이다. */
	case EVT_ID_VMS_FETCH_FAULT:	/* [한국어] 이 값일 때의 처리. */
		dev_err(smmu->dev, "event: %s client: %s sid: %#x ssid: %#x fetch_addr: %#llx",	/* [한국어] 그 표 항목의 주소를 남긴다 — 대개 드라이버 버그나 메모리 손상이다. */
			ARM_SMMU_LOG_EVT_STR(evt), ARM_SMMU_LOG_CLIENT(evt),
			evt->sid, evt->ssid, evt->fetch_addr);

		break;

	default:	/* [한국어] 그 밖의 종류. */
		dev_err(smmu->dev, "event: %s client: %s sid: %#x ssid: %#x",	/* [한국어] 최소한 어느 장치인지는 남긴다. */
			ARM_SMMU_LOG_EVT_STR(evt), ARM_SMMU_LOG_CLIENT(evt),
			evt->sid, evt->ssid);
	}
}

/*
 * [한국어]
 * arm_smmu_evtq_thread - 이벤트 큐에 쌓인 폴트를 모두 처리한다
 *
 * @irq: 인터럽트 번호 (쓰지 않는다).
 * @dev: 등록할 때 넘긴 SMMU 포인터.
 * @return: 항상 IRQ_HANDLED.
 *
 * 큐가 빌 때까지 도는 이중 반복이다. 안쪽 반복은 지금 알려진 만큼을
 * 비우고, 바깥 반복은 그 사이에 새로 들어온 것이 있는지 다시 확인한다.
 * 그렇게 해야 인터럽트를 한 번 받고 여러 폴트를 몰아 처리할 수 있다.
 *
 * cond_resched() 가 중요하다. 폴트가 폭주하면 이 반복이 아주 오래 도는데,
 * 스레드 인터럽트 문맥이라 선점될 수 있으므로 다른 일에 CPU 를 양보한다.
 *
 * 넘침에는 손쓸 방법이 없다 — 이미 잃어버린 폴트는 되살릴 수 없어 로그만
 * 남긴다. 주석이 "더 열심히 하는 척한다"고 자조하는 대목이다.
 *
 * 실행 컨텍스트: 이벤트 큐 인터럽트 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 *     → arm_smmu_decode_event() → arm_smmu_handle_event()
 */
static irqreturn_t arm_smmu_evtq_thread(int irq, void *dev)
{
	u64 evt[EVTQ_ENT_DWORDS];	/* [한국어] 꺼낸 원본 워드. */
	struct arm_smmu_event event = {0};	/* [한국어] 풀어 담을 구조. */
	struct arm_smmu_device *smmu = dev;	/* [한국어] 등록할 때 넘겨 둔 문맥. */
	struct arm_smmu_queue *q = &smmu->evtq.q;	/* [한국어] 이벤트 큐. */
	struct arm_smmu_ll_queue *llq = &q->llq;	/* [한국어] 그 포인터 쌍. */
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,	/* [한국어] 로그 빈도 제한 상태 — static 이라 호출 사이에 유지된다. */
				      DEFAULT_RATELIMIT_BURST);

	do {
		while (!queue_remove_raw(q, evt)) {	/* [한국어] 지금 알려진 만큼을 모두 꺼낸다. */
			arm_smmu_decode_event(smmu, evt, &event);	/* [한국어] 비트를 펼치고 장치 참조를 잡는다. */
			if (arm_smmu_handle_event(smmu, evt, &event))	/* [한국어] 처리할 곳으로 넘겨 본다. */
				arm_smmu_dump_event(smmu, evt, &event, &rs);	/* [한국어] 넘기지 못했으면 사람이 보도록 로그를 남긴다. */

			put_device(event.dev);	/* [한국어] decode 에서 잡은 참조를 놓는다 — NULL 이어도 안전하다. */
			cond_resched();	/* [한국어] 폴트가 폭주해도 다른 일이 굶지 않도록 양보한다. */
		}

		/*
		 * Not much we can do on overflow, so scream and pretend we're
		 * trying harder.
		 */
		/* [한국어] (위 영어 주석 참고) 넘침이 일어났다면 이미 폴트를 잃은 뒤라
		 * 되살릴 방법이 없다. 로그로 알리는 것이 전부다. */
		if (queue_sync_prod_in(q) == -EOVERFLOW)	/* [한국어] 새 항목이 있는지 확인하며 넘침도 함께 본다. */
			dev_err(smmu->dev, "EVTQ overflow detected -- events lost\n");
	} while (!queue_empty(llq));	/* [한국어] 그 사이 새로 들어온 것이 있으면 다시 비운다. */

	/* Sync our overflow flag, as we believe we're up to speed */
	/* [한국어] (위 영어 주석 참고) 쌓인 것을 다 처리했으니, 넘침 표시를 맞춰
	 * 하드웨어가 다시 항목을 넣기 시작하게 한다. */
	queue_sync_cons_ovf(q);
	return IRQ_HANDLED;	/* [한국어] 이 인터럽트는 우리 것이 맞다고 알린다. */
}

/*
 * [한국어]
 * arm_smmu_handle_ppr - 페이지 요청을 로그로 남기고 거부한다
 *
 * @smmu: 대상 하드웨어.
 * @evt: PRI 큐에서 읽은 원본 워드.
 *
 * PCI 의 페이지 요청 인터페이스(PRI)로 온 요청을 다룬다. 이 드라이버는
 * 아직 그 방식을 지원하지 않아 — SVA 는 멈춤 방식만 쓴다 — 요청이 온다는
 * 것 자체가 예상 밖이다. 그래서 정보를 남기고 거부로 답한다.
 *
 * 마지막 요청에만 답하는 것은 규격의 약속이다. 한 그룹의 여러 요청은
 * 마지막 것에 대한 응답 하나로 함께 닫힌다.
 *
 * 실행 컨텍스트: PRI 큐 인터럽트 스레드. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_priq_thread() → [이 함수] → arm_smmu_cmdq_issue_cmd()
 */
static void arm_smmu_handle_ppr(struct arm_smmu_device *smmu, u64 *evt)
{
	u32 sid, ssid;	/* [한국어] 요청을 보낸 장치와 그 PASID. */
	u16 grpid;	/* [한국어] 요청 그룹 번호. */
	bool ssv, last;	/* [한국어] PASID 가 유효한지, 그룹의 마지막인지. */

	sid = FIELD_GET(PRIQ_0_SID, evt[0]);	/* [한국어] 어느 장치인지. */
	ssv = FIELD_GET(PRIQ_0_SSID_V, evt[0]);	/* [한국어] PASID 가 실려 있는가. */
	ssid = ssv ? FIELD_GET(PRIQ_0_SSID, evt[0]) : IOMMU_NO_PASID;	/* [한국어] 유효할 때만 꺼낸다. */
	last = FIELD_GET(PRIQ_0_PRG_LAST, evt[0]);	/* [한국어] 이 요청이 그룹의 마지막인가. */
	grpid = FIELD_GET(PRIQ_1_PRG_IDX, evt[1]);	/* [한국어] 그룹 번호 — 응답에 그대로 실어 보낸다. */

	dev_info(smmu->dev, "unexpected PRI request received:\n");	/* [한국어] 지원하지 않는 기능이라 요청이 오는 것 자체가 예상 밖이다. */
	dev_info(smmu->dev,	/* [한국어] 어떤 요청이었는지 자세히 남겨, 왜 왔는지 추적할 수 있게 한다. */
		 "\tsid 0x%08x.0x%05x: [%u%s] %sprivileged %s%s%s access at iova 0x%016llx\n",
		 sid, ssid, grpid, last ? "L" : "",	/* [한국어] 마지막 요청이면 L 로 표시한다. */
		 evt[0] & PRIQ_0_PERM_PRIV ? "" : "un",	/* [한국어] 특권 접근 여부. */
		 evt[0] & PRIQ_0_PERM_READ ? "R" : "",	/* [한국어] 읽기 권한을 요청했는가. */
		 evt[0] & PRIQ_0_PERM_WRITE ? "W" : "",	/* [한국어] 쓰기 권한. */
		 evt[0] & PRIQ_0_PERM_EXEC ? "X" : "",	/* [한국어] 실행 권한. */
		 evt[1] & PRIQ_1_ADDR_MASK);	/* [한국어] 요청한 주소. */

	if (last) {	/* [한국어] 그룹의 마지막 요청에만 답한다 — 규격의 약속이다. */
		struct arm_smmu_cmdq_ent cmd = {	/* [한국어] 거부 응답을 짓는다. */
			.opcode			= CMDQ_OP_PRI_RESP,	/* [한국어] 페이지 요청에 대한 응답 명령. */
			.substream_valid	= ssv,	/* [한국어] 요청과 같게 맞춘다. */
			.pri			= {
				.sid	= sid,	/* [한국어] 요청을 보낸 장치. */
				.ssid	= ssid,	/* [한국어] 그 PASID. */
				.grpid	= grpid,	/* [한국어] 그 그룹 — 이 값으로 짝이 맞춰진다. */
				.resp	= PRI_RESP_DENY,	/* [한국어] 거부한다 — 지원하지 않는 기능이라 채워 줄 수 없다. */
			},
		};

		arm_smmu_cmdq_issue_cmd(smmu, &cmd);	/* [한국어] 완료를 기다리지 않는다 — 응답은 보내고 잊는 명령이다. */
	}
}

/*
 * [한국어]
 * arm_smmu_priq_thread - PRI 큐에 쌓인 요청을 모두 처리한다
 *
 * @irq: 인터럽트 번호 (쓰지 않는다).
 * @dev: 등록할 때 넘긴 SMMU 포인터.
 * @return: 항상 IRQ_HANDLED.
 *
 * 이벤트 큐 처리와 짜임이 같다 — 큐가 빌 때까지 이중으로 돌고, 마지막에
 * 넘침 표시를 맞춘다. 다만 각 요청을 처리하는 대신 로그를 남기고 거부할
 * 뿐이라 훨씬 단순하다.
 *
 * 실행 컨텍스트: PRI 큐 인터럽트 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수] → arm_smmu_handle_ppr()
 */
static irqreturn_t arm_smmu_priq_thread(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;	/* [한국어] 등록할 때 넘겨 둔 문맥. */
	struct arm_smmu_queue *q = &smmu->priq.q;	/* [한국어] 페이지 요청 큐. */
	struct arm_smmu_ll_queue *llq = &q->llq;	/* [한국어] 그 포인터 쌍. */
	u64 evt[PRIQ_ENT_DWORDS];	/* [한국어] 꺼낸 원본 워드. */

	do {
		while (!queue_remove_raw(q, evt))	/* [한국어] 지금 알려진 만큼을 모두 꺼내. */
			arm_smmu_handle_ppr(smmu, evt);	/* [한국어] 로그를 남기고 거부한다. */

		if (queue_sync_prod_in(q) == -EOVERFLOW)	/* [한국어] 새 항목이 있는지 확인하며 넘침도 본다. */
			dev_err(smmu->dev, "PRIQ overflow detected -- requests lost\n");	/* [한국어] 잃은 요청은 되살릴 수 없다 — 장치가 다시 요청할 것이다. */
	} while (!queue_empty(llq));	/* [한국어] 그 사이 들어온 것이 있으면 다시 비운다. */

	/* Sync our overflow flag, as we believe we're up to speed */
	/* [한국어] (위 영어 주석 참고) 다 처리했으니 하드웨어가 다시 넣게 한다. */
	queue_sync_cons_ovf(q);
	return IRQ_HANDLED;	/* [한국어] 이 인터럽트는 우리 것이 맞다고 알린다. */
}

/* [한국어] gerror 처리기가 하드웨어를 꺼야 하는 경우에 부른다 — 정의는 훨씬 뒤에 있다. */
static int arm_smmu_device_disable(struct arm_smmu_device *smmu);

/*
 * [한국어]
 * arm_smmu_gerror_handler - 전역 오류 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호 (쓰지 않는다).
 * @dev: 등록할 때 넘긴 SMMU 포인터.
 * @return: 우리 오류였으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * gerror 는 "드라이버와 하드웨어 사이의 약속이 깨졌다"는 신호다. 페이지
 * 폴트처럼 흔한 일이 아니라, 대개 드라이버 버그나 하드웨어 고장을 뜻한다.
 * 그래서 처리가 거의 다 로그 남기기다.
 *
 * 오류 판정 방식이 독특하다. GERROR 와 GERRORN 두 레지스터를 XOR 해
 * 다른 비트가 곧 "새로 난 오류"가 된다 — 드라이버가 확인한 오류는
 * GERRORN 에 그 값을 되써서 표시하기 때문이다. 마지막 줄이 그 확인이다.
 *
 * 두 갈래만 실제 조치를 한다. 서비스 실패 모드는 하드웨어가 스스로
 * 포기한 상태라 아예 꺼 버리고, 명령 큐 오류는 걸려 넘어진 명령을 건너뛴다.
 *
 * 실행 컨텍스트: 진짜 인터럽트 문맥(스레드가 아니다). 잠들 수 없다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 *     → arm_smmu_device_disable() / arm_smmu_cmdq_skip_err()
 */
static irqreturn_t arm_smmu_gerror_handler(int irq, void *dev)
{
	u32 gerror, gerrorn, active;	/* [한국어] 지금 오류, 확인된 오류, 그리고 새로 난 오류. */
	struct arm_smmu_device *smmu = dev;	/* [한국어] 등록할 때 넘겨 둔 문맥. */

	gerror = readl_relaxed(smmu->base + ARM_SMMU_GERROR);	/* [한국어] 하드웨어가 알리는 오류 비트들. */
	gerrorn = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);	/* [한국어] 드라이버가 이미 확인한 오류 비트들. */

	active = gerror ^ gerrorn;	/* [한국어] 두 값이 다른 자리가 곧 아직 확인하지 않은 새 오류다. */
	if (!(active & GERROR_ERR_MASK))	/* [한국어] 새 오류가 하나도 없으면. */
		return IRQ_NONE; /* No errors pending */	/* [한국어] (위 영어 주석 참고) 인터럽트 선을 나눠 쓰는 다른 장치의 것이다. */

	dev_warn(smmu->dev,	/* [한국어] gerror 자체가 비정상이라는 뜻이므로 반드시 남긴다. */
		 "unexpected global error reported (0x%08x), this could be serious\n",
		 active);

	if (active & GERROR_SFM_ERR) {	/* [한국어] 하드웨어가 서비스 실패 모드에 빠진 경우. */
		dev_err(smmu->dev, "device has entered Service Failure Mode!\n");	/* [한국어] 하드웨어가 스스로 포기한 상태다. */
		arm_smmu_device_disable(smmu);	/* [한국어] 더 이상 믿을 수 없으므로 꺼 버린다 — 이 뒤로 모든 DMA 가 막힌다. */
	}

	if (active & GERROR_MSI_GERROR_ABT_ERR)	/* [한국어] 아래 넷은 MSI 쓰기가 실패한 경우 — */
		dev_warn(smmu->dev, "GERROR MSI write aborted\n");	/* [한국어] 인터럽트를 못 받게 되지만 폴링으로 동작은 이어진다. */

	if (active & GERROR_MSI_PRIQ_ABT_ERR)	/* [한국어] 페이지 요청 큐의 MSI 쓰기가 실패했다. */
		dev_warn(smmu->dev, "PRIQ MSI write aborted\n");

	if (active & GERROR_MSI_EVTQ_ABT_ERR)	/* [한국어] 이벤트 큐의 MSI 쓰기가 실패했다. */
		dev_warn(smmu->dev, "EVTQ MSI write aborted\n");

	if (active & GERROR_MSI_CMDQ_ABT_ERR)	/* [한국어] 명령 큐의 MSI 쓰기가 실패했다. */
		dev_warn(smmu->dev, "CMDQ MSI write aborted\n");

	if (active & GERROR_PRIQ_ABT_ERR)	/* [한국어] 큐 메모리에 쓰지 못한 경우 — 요청을 잃는다. */
		dev_err(smmu->dev, "PRIQ write aborted -- events may have been lost\n");

	if (active & GERROR_EVTQ_ABT_ERR)	/* [한국어] 이벤트 큐도 마찬가지 — 폴트를 잃으면 진단이 어려워진다. */
		dev_err(smmu->dev, "EVTQ write aborted -- events may have been lost\n");

	if (active & GERROR_CMDQ_ERR)	/* [한국어] 명령 큐가 걸려 넘어진 경우. */
		arm_smmu_cmdq_skip_err(smmu);	/* [한국어] 그 명령을 건너뛰어 큐를 다시 굴린다 — 이것만이 실질적인 복구다. */

	writel(gerror, smmu->base + ARM_SMMU_GERRORN);	/* [한국어] 확인했음을 알린다 — 이 값을 되써야 같은 오류로 다시 인터럽트가 오지 않는다. */
	return IRQ_HANDLED;	/* [한국어] 이 인터럽트는 우리 것이 맞다고 알린다. */
}

/*
 * [한국어]
 * arm_smmu_combined_irq_thread - 인터럽트 선이 하나인 하드웨어의 스레드 처리
 *
 * @irq: 인터럽트 번호.
 * @dev: SMMU 포인터.
 * @return: 항상 IRQ_HANDLED.
 *
 * 인터럽트를 종류별로 나누지 않고 하나로 묶어 내보내는 하드웨어가 있다.
 * 그때는 어느 큐에서 왔는지 알 수 없으므로, 둘 다 훑어 본다. 큐가 비어
 * 있으면 곧바로 돌아오므로 헛수고가 크지 않다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 *     → arm_smmu_evtq_thread() → arm_smmu_priq_thread()
 */
static irqreturn_t arm_smmu_combined_irq_thread(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;	/* [한국어] 등록할 때 넘겨 둔 문맥. */

	arm_smmu_evtq_thread(irq, dev);	/* [한국어] 폴트가 쌓였는지 본다. */
	if (smmu->features & ARM_SMMU_FEAT_PRI)	/* [한국어] 페이지 요청 큐를 가진 하드웨어라면. */
		arm_smmu_priq_thread(irq, dev);	/* [한국어] 그쪽도 본다. */

	return IRQ_HANDLED;	/* [한국어] 이 인터럽트는 우리 것이 맞다고 알린다. */
}

/*
 * [한국어]
 * arm_smmu_combined_irq_handler - 인터럽트 선이 하나일 때의 상위 처리기
 *
 * @irq: 인터럽트 번호.
 * @dev: SMMU 포인터.
 * @return: 항상 IRQ_WAKE_THREAD.
 *
 * gerror 는 잠들 수 없는 자리에서 처리해야 하므로 여기서 곧바로 다루고,
 * 큐 처리는 시간이 걸리므로 스레드로 넘긴다. 이 두 겹 구조가 인터럽트를
 * 짧게 유지하면서도 큐를 충분히 비울 수 있게 한다.
 *
 * 실행 컨텍스트: 진짜 인터럽트 문맥. 잠들 수 없다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수] → arm_smmu_gerror_handler()
 */
static irqreturn_t arm_smmu_combined_irq_handler(int irq, void *dev)
{
	arm_smmu_gerror_handler(irq, dev);	/* [한국어] 잠들 수 없는 일은 여기서 끝낸다. */
	return IRQ_WAKE_THREAD;	/* [한국어] 큐 비우기는 스레드에 맡긴다 — 오래 걸릴 수 있기 때문이다. */
}

/*
 * [한국어]
 * arm_smmu_atc_inv_to_cmd - 장치 캐시 무효화 명령을 짓는다
 *
 * @ssid: 대상 PASID (IOMMU_NO_PASID 면 PASID 없는 트래픽).
 * @iova: 무효화할 구간의 시작.
 * @size: 그 길이. 0 이면 "그 장치 전부".
 * @cmd: 지어 담을 자리.
 *
 * ATS 무효화는 다른 무효화와 성격이 다르다. SMMU 안에서 끝나지 않고
 * PCIe 를 타고 장치까지 갔다 와야 하므로 훨씬 비싸고, 규격이 요구하는
 * 주소 형식도 까다롭다.
 *
 * 그 까다로움이 이 함수의 절반을 차지한다. ATS 무효화는 주소가 범위 크기에
 * 정렬되어 있어야 하고 범위는 2의 거듭제곱이어야 한다. 임의의 구간을 그
 * 형식에 맞추려면 여러 명령으로 쪼개거나 범위를 넓혀야 하는데, 여기서는
 * 단순함을 택해 넓히는 쪽을 쓴다 — 시작과 끝 주소의 가장 높은 다른 비트를
 * 찾으면 그것이 필요한 범위의 크기다.
 *
 * PASID 처리의 함정도 주석이 길게 설명한다. PASID 없이 낸 무효화는 그
 * 구간의 모든 항목을 — PASID 가 붙은 것까지 — 지워 버리며, 그것을 피할
 * 방법이 규격에 없다.
 *
 * 실행 컨텍스트: 무효화 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_atc_inv_master()/domain_inv_range() → [이 함수]
 */
static void
arm_smmu_atc_inv_to_cmd(int ssid, unsigned long iova, size_t size,
			struct arm_smmu_cmdq_ent *cmd)
{
	size_t log2_span;	/* [한국어] 덮어야 할 범위의 크기(2의 지수). */
	size_t span_mask;	/* [한국어] 그 범위에 맞춰 주소를 내림할 마스크. */
	/* ATC invalidates are always on 4096-bytes pages */
	/* [한국어] (위 영어 주석 참고) 장치 캐시는 커널 페이지 크기와 무관하게
	 * 언제나 4K 단위로 다룬다 — PCIe 규격이 그렇게 정했다. */
	size_t inval_grain_shift = 12;	/* [한국어] 4096 = 1 << 12. */
	unsigned long page_start, page_end;	/* [한국어] 구간의 첫 페이지와 마지막 페이지 번호. */

	/*
	 * ATS and PASID:
	 *
	 * If substream_valid is clear, the PCIe TLP is sent without a PASID
	 * prefix. In that case all ATC entries within the address range are
	 * invalidated, including those that were requested with a PASID! There
	 * is no way to invalidate only entries without PASID.
	 *
	 * When using STRTAB_STE_1_S1DSS_SSID0 (reserving CD 0 for non-PASID
	 * traffic), translation requests without PASID create ATC entries
	 * without PASID, which must be invalidated with substream_valid clear.
	 * This has the unpleasant side-effect of invalidating all PASID-tagged
	 * ATC entries within the address range.
	 */
	/* [한국어] (위 영어 주석 참고) PASID 없이 낸 무효화는 그 구간의 모든 항목을
	 * 지운다 — PASID 가 붙은 것까지 함께 날아간다. "PASID 없는 것만" 지우는
	 * 방법이 규격에 없기 때문이다. 성능만 손해일 뿐 정확성은 깨지지 않지만,
	 * PASID 를 여럿 쓰는 장치에서는 눈에 띌 수 있다. */
	*cmd = (struct arm_smmu_cmdq_ent) {	/* [한국어] 명령 서술 구조를 통째로 짓는다. */
		.opcode			= CMDQ_OP_ATC_INV,	/* [한국어] 장치 캐시 무효화. */
		.substream_valid	= (ssid != IOMMU_NO_PASID),	/* [한국어] PASID 별 무효화인지 표시한다. */
		.atc.ssid		= ssid,	/* [한국어] 그 PASID. */
	};

	if (!size) {	/* [한국어] 길이가 0 이면 "전부"라는 약속. */
		cmd->atc.size = ATC_INV_SIZE_ALL;	/* [한국어] 그 장치의 캐시를 통째로 비운다. */
		return;	/* [한국어] 주소 계산이 필요 없다. */
	}

	page_start	= iova >> inval_grain_shift;	/* [한국어] 시작 주소의 4K 페이지 번호. */
	page_end	= (iova + size - 1) >> inval_grain_shift;	/* [한국어] 마지막 바이트가 속한 페이지 번호. */

	/*
	 * In an ATS Invalidate Request, the address must be aligned on the
	 * range size, which must be a power of two number of page sizes. We
	 * thus have to choose between grossly over-invalidating the region, or
	 * splitting the invalidation into multiple commands. For simplicity
	 * we'll go with the first solution, but should refine it in the future
	 * if multiple commands are shown to be more efficient.
	 *
	 * Find the smallest power of two that covers the range. The most
	 * significant differing bit between the start and end addresses,
	 * fls(start ^ end), indicates the required span. For example:
	 *
	 * We want to invalidate pages [8; 11]. This is already the ideal range:
	 *		x = 0b1000 ^ 0b1011 = 0b11
	 *		span = 1 << fls(x) = 4
	 *
	 * To invalidate pages [7; 10], we need to invalidate [0; 15]:
	 *		x = 0b0111 ^ 0b1010 = 0b1101
	 *		span = 1 << fls(x) = 16
	 */
	/* [한국어] (위 영어 주석과 예시 참고) 규격이 "주소는 범위 크기에 정렬되고
	 * 범위는 2의 거듭제곱"을 요구한다. 임의의 구간을 그 형식에 맞추려면
	 * 여러 명령으로 쪼개거나 범위를 넓혀야 하는데, 여기서는 단순함을 택해
	 * 넓히는 쪽을 쓴다.
	 * 계산은 간단하다 — 시작과 끝 페이지 번호를 XOR 해 가장 높은 다른 비트를
	 * 찾으면, 그 비트까지가 두 주소를 함께 담는 데 필요한 범위다.
	 * 두 번째 예시처럼 경계를 살짝 넘으면 범위가 네 배로 뛸 수 있다는 것이
	 * 이 방식의 대가다. */
	log2_span	= fls_long(page_start ^ page_end);	/* [한국어] 가장 높은 다른 비트의 자리. */
	span_mask	= (1ULL << log2_span) - 1;	/* [한국어] 그 크기에 맞춰 주소를 내림할 마스크. */

	page_start	&= ~span_mask;	/* [한국어] 시작 주소를 범위 경계로 내린다 — 규격의 정렬 요구를 맞춘다. */

	cmd->atc.addr	= page_start << inval_grain_shift;	/* [한국어] 페이지 번호를 다시 바이트 주소로. */
	cmd->atc.size	= log2_span;	/* [한국어] 범위 크기(2의 지수)를 담는다. */
}

/*
 * [한국어]
 * arm_smmu_atc_inv_master - 그 장치의 캐시를 통째로 비운다
 *
 * @master: 대상 장치.
 * @ssid: 대상 PASID.
 * @return: 0 성공, 음수 오류.
 *
 * 장치를 떼거나 PASID 를 놓을 때, 그 장치가 캐시에 들고 있던 변환을 모두
 * 지운다. 구간이 아니라 전체를 지우므로 주소 계산이 필요 없다.
 *
 * 한 장치가 스트림 번호를 여럿 가질 수 있으므로 그 목록을 돌며 명령을
 * 만들되, 묶음으로 모아 한 번에 낸다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 완료를 기다린다.
 *
 * 호출 체인:
 *   arm_smmu_disable_ats() 등 → [이 함수] → arm_smmu_cmdq_batch_submit()
 */
static int arm_smmu_atc_inv_master(struct arm_smmu_master *master,
				   ioasid_t ssid)
{
	int i;	/* [한국어] 스트림 반복자. */
	struct arm_smmu_cmdq_ent cmd;	/* [한국어] 명령의 본보기. */
	struct arm_smmu_cmdq_batch cmds;	/* [한국어] 명령 묶음. */

	arm_smmu_atc_inv_to_cmd(ssid, 0, 0, &cmd);	/* [한국어] 크기 0 으로 "전부 비우기" 명령을 짓는다. */

	arm_smmu_cmdq_batch_init(master->smmu, &cmds, &cmd);	/* [한국어] 묶음을 시작한다. */
	for (i = 0; i < master->num_streams; i++) {	/* [한국어] 이 장치의 모든 스트림 번호에. */
		cmd.atc.sid = master->streams[i].id;	/* [한국어] 본보기의 번호만 갈아 끼운다. */
		arm_smmu_cmdq_batch_add(master->smmu, &cmds, &cmd);	/* [한국어] 묶음에 담는다. */
	}

	return arm_smmu_cmdq_batch_submit(master->smmu, &cmds);	/* [한국어] 한 번에 내보내고 완료까지 기다린다 — 장치가 캐시를 다 비웠음을 확인해야 한다. */
}

/* IO_PGTABLE API */
/*
 * [한국어]
 * arm_smmu_tlb_inv_context - 그 도메인의 변환 캐시를 통째로 비운다
 *
 * @cookie: io-pgtable 에 등록해 둔 도메인 포인터.
 *
 * io-pgtable 이 페이지 테이블을 크게 바꿨을 때 부르는 갈고리다. 어느 범위가
 * 바뀌었는지 추적하기보다 통째로 비우는 편이 싼 경우에 쓰인다.
 *
 * 긴 주석이 설명하는 것은 다른 CPU 의 페이지 테이블 쓰기가 이 무효화보다
 * 먼저 하드웨어에 보이는 이유다. 비엄격 모드에서는 다른 CPU 가 테이블만
 * 고치고 무효화를 내지 않을 수 있는데, 명령을 발행할 때의 dma_wmb() 가
 * 그 CPU 의 쓰기까지 함께 발표해 준다.
 *
 * 실행 컨텍스트: io-pgtable 콜백. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   io-pgtable → tlb_flush_all 갈고리 = [이 함수] → arm_smmu_domain_inv()
 */
static void arm_smmu_tlb_inv_context(void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] io-pgtable 에 등록해 둔 도메인. */

	/*
	 * If the DMA API is running in non-strict mode then another CPU could
	 * have changed the page table and not invoked any flush op. Instead the
	 * other CPU will do an atomic_read() and this CPU will have done an
	 * atomic_write(). That handshake is enough to acquire the page table
	 * writes from the other CPU.
	 *
	 * All command execution has a dma_wmb() to release all the in-memory
	 * structures written by this CPU, that barrier must also release the
	 * writes acquired from all the other CPUs too.
	 *
	 * There are other barriers and atomics on this path, but the above is
	 * the essential mechanism for ensuring that HW sees the page table
	 * writes from another CPU before it executes the IOTLB invalidation.
	 */
	/* [한국어] (위 영어 주석 참고) 비엄격 모드에서는 매핑을 푼 CPU 가 무효화를
	 * 곧바로 내지 않고 나중에 몰아서 낸다. 그 사이 다른 CPU 가 무효화를 내는데,
	 * 그때 그 CPU 가 앞 CPU 의 페이지 테이블 쓰기를 이미 획득했다는 것이
	 * 원자 연산의 짝(읽기/쓰기)으로 보장된다.
	 * 그리고 명령을 발행할 때의 dma_wmb() 가 이 CPU 가 획득한 모든 쓰기를
	 * — 남의 것까지 포함해 — 함께 발표한다. 그래서 하드웨어가 무효화를
	 * 실행할 때는 이미 새 페이지 테이블을 볼 수 있다. */
	arm_smmu_domain_inv(smmu_domain);	/* [한국어] 이 도메인의 모든 변환 캐시를 비운다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_batch_add_range - 주소 구간을 덮는 무효화 명령들을 담는다
 *
 * @smmu: 대상 하드웨어.
 * @cmds: 담을 묶음.
 * @cmd: 명령의 본보기 (주소와 범위 필드만 바꿔 가며 쓴다).
 * @iova: 구간의 시작.
 * @size: 그 길이.
 * @granule: 한 걸음의 크기 (보통 페이지 크기).
 * @pgsize: 알갱이 크기의 로그.
 *
 * 넓은 구간을 무효화하는 두 가지 길을 다룬다.
 * 하드웨어가 범위 무효화를 지원하면, 명령 하나가 (num+1) × 2^scale 개의
 * 알갱이를 한 번에 지울 수 있다. 다만 num 이 5비트뿐이라 한 번에 덮을 수
 * 있는 크기가 제한되므로, 남은 크기의 아래쪽 5비트씩 잘라 가며 여러 명령을
 * 만든다 — 반복문 안의 scale/num 계산이 그 일이다.
 * 지원하지 않으면 알갱이 하나씩 도는 수밖에 없다.
 *
 * TTL 계산이 눈에 띈다. 무효화가 어느 표 단계를 겨냥하는지 알려 주면
 * 하드웨어가 그 단계만 뒤져 더 빠른데, 마지막 단계 무효화일 때만 그 값을
 * 확신할 수 있다. 중간 단계 무효화는 io-pgtable 도 SVA 도 실제 단계를
 * 모르므로 0(모든 단계)으로 둔다.
 *
 * 실행 컨텍스트: 무효화 경로. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_inv_to_cmdq_batch() → [이 함수] → arm_smmu_cmdq_batch_add()
 */
static void arm_smmu_cmdq_batch_add_range(struct arm_smmu_device *smmu,
					  struct arm_smmu_cmdq_batch *cmds,
					  struct arm_smmu_cmdq_ent *cmd,
					  unsigned long iova, size_t size,
					  size_t granule, size_t pgsize)
{
	unsigned long end = iova + size, num_pages = 0, tg = pgsize;	/* [한국어] 구간의 끝, 남은 알갱이 수, 알갱이 크기의 로그. */
	size_t inv_range = granule;	/* [한국어] 이번 명령이 덮는 길이 — 범위 무효화가 없으면 알갱이 하나다. */

	if (WARN_ON_ONCE(!size))	/* [한국어] 길이 0 은 "전부"를 뜻해 다른 경로로 가야 한다 — 여기 왔다면 호출자의 버그다. */
		return;

	if (smmu->features & ARM_SMMU_FEAT_RANGE_INV) {	/* [한국어] 범위 무효화를 지원하는 하드웨어라면. */
		num_pages = size >> tg;	/* [한국어] 구간이 몇 알갱이인지. */

		/* Convert page size of 12,14,16 (log2) to 1,2,3 */
		/* [한국어] (위 영어 주석 참고) 4K/16K/64K 의 로그가 12/14/16 인데,
		 * 레지스터 필드는 1/2/3 을 요구한다. 그 변환식이다. */
		cmd->tlbi.tg = (tg - 10) / 2;

		/*
		 * Determine what level the granule is at. For non-leaf, both
		 * io-pgtable and SVA pass a nominal last-level granule because
		 * they don't know what level(s) actually apply, so ignore that
		 * and leave TTL=0. However for various errata reasons we still
		 * want to use a range command, so avoid the SVA corner case
		 * where both scale and num could be 0 as well.
		 */
		/* [한국어] (위 영어 주석 참고) 무효화가 어느 표 단계를 겨냥하는지 알려
		 * 주면 하드웨어가 그 단계만 뒤져 빠르다. 마지막 단계 무효화일 때만
		 * 그 값을 확신할 수 있으므로, 그때만 계산해 담는다.
		 * 중간 단계 무효화에서는 실제 단계를 알 수 없어 0(모든 단계)으로 두되,
		 * 결함 우회를 위해 여전히 범위 명령을 쓰고 싶다. 그래서 scale 과 num 이
		 * 둘 다 0 이 되는 구석 사례만 피해 간다. */
		if (cmd->tlbi.leaf)	/* [한국어] 마지막 단계만 지우는 무효화라면. */
			cmd->tlbi.ttl = 4 - ((ilog2(granule) - 3) / (tg - 3));	/* [한국어] 알갱이 크기에서 그 단계를 역산한다. */
		else if ((num_pages & CMDQ_TLBI_RANGE_NUM_MAX) == 1)	/* [한국어] 알갱이가 딱 하나 남는 구석 사례라면. */
			num_pages++;	/* [한국어] 하나 늘려 범위 명령이 성립하게 만든다 — 조금 넓게 지우지만 안전하다. */
	}

	while (iova < end) {	/* [한국어] 구간을 다 덮을 때까지. */
		if (smmu->features & ARM_SMMU_FEAT_RANGE_INV) {	/* [한국어] 범위 무효화를 쓸 수 있다면. */
			/*
			 * On each iteration of the loop, the range is 5 bits
			 * worth of the aligned size remaining.
			 * The range in pages is:
			 *
			 * range = (num_pages & (0x1f << __ffs(num_pages)))
			 */
			/* [한국어] (위 영어 주석 참고) num 필드가 5비트뿐이라 한 번에 덮을 수
			 * 있는 크기가 제한된다. 남은 알갱이 수에서 가장 낮은 1 비트부터
			 * 5비트씩 잘라 가며 명령을 만든다 — 그래야 명령 수가 최소가 된다. */
			unsigned long scale, num;	/* [한국어] 이번에 쓸 배율과 개수. */

			/* Determine the power of 2 multiple number of pages */
			/* [한국어] (위 영어 주석 참고) 남은 수의 가장 낮은 1 비트가 곧 배율이다. */
			scale = __ffs(num_pages);
			cmd->tlbi.scale = scale;	/* [한국어] 한 묶음이 2^scale 알갱이라는 뜻. */

			/* Determine how many chunks of 2^scale size we have */
			/* [한국어] (위 영어 주석 참고) 그 묶음이 몇 개인지 — 5비트로 자른다. */
			num = (num_pages >> scale) & CMDQ_TLBI_RANGE_NUM_MAX;
			cmd->tlbi.num = num - 1;	/* [한국어] 필드는 0 이 "하나"를 뜻하므로 하나 뺀다. */

			/* range is num * 2^scale * pgsize */
			/* [한국어] (위 영어 주석 참고) 이번 명령이 실제로 덮는 바이트 수. */
			inv_range = num << (scale + tg);

			/* Clear out the lower order bits for the next iteration */
			/* [한국어] (위 영어 주석 참고) 처리한 만큼 빼면 다음 반복에서 그다음
			 * 5비트 묶음이 가장 낮은 자리로 올라온다. */
			num_pages -= num << scale;
		}

		cmd->tlbi.addr = iova;	/* [한국어] 이번 명령이 시작할 주소. */
		arm_smmu_cmdq_batch_add(smmu, cmds, cmd);	/* [한국어] 묶음에 담는다 — 실제 발행은 나중에 한꺼번에. */
		iova += inv_range;	/* [한국어] 덮은 만큼 앞으로 옮긴다. */
	}
}

/*
 * [한국어]
 * arm_smmu_inv_size_too_big - 구간별 무효화보다 통째로 비우는 편이 나은가
 *
 * @smmu: 대상 하드웨어.
 * @size: 무효화할 길이.
 * @granule: 한 걸음의 크기.
 * @return: 통째로 비워야 하면 참.
 *
 * 범위 무효화를 지원하지 않는 하드웨어에서 넓은 구간을 알갱이 단위로
 * 지우면 명령이 수만 개가 되어, 그 자체로 CPU 가 몇 초씩 붙잡히는
 * 소프트 락업이 난다. 그럴 바에는 도메인 전체를 한 번에 비우는 편이 낫다.
 *
 * 문턱값은 CPU 쪽 TLB 무효화 코드에서 그대로 빌려 왔다 — 같은 종류의
 * 절충이라 같은 기준을 쓰는 것이 자연스럽다.
 *
 * 실행 컨텍스트: 무효화 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_inv_to_cmdq_batch() → [이 함수]
 */
static bool arm_smmu_inv_size_too_big(struct arm_smmu_device *smmu, size_t size,
				      size_t granule)
{
	size_t max_tlbi_ops;	/* [한국어] 이 알갱이 크기에서 허용할 최대 명령 수. */

	/* 0 size means invalidate all */
	/* [한국어] (위 영어 주석 참고) 길이 0 과 SIZE_MAX 는 둘 다 "전부"를 뜻한다. */
	if (!size || size == SIZE_MAX)
		return true;	/* [한국어] 통째로 비우는 경로로 간다. */

	if (smmu->features & ARM_SMMU_FEAT_RANGE_INV)	/* [한국어] 범위 무효화를 지원하면. */
		return false;	/* [한국어] 명령 수가 로그 규모로 줄어 문턱을 걱정할 필요가 없다. */

	/*
	 * Borrowed from the MAX_TLBI_OPS in arch/arm64/include/asm/tlbflush.h,
	 * this is used as a threshold to replace "size_opcode" commands with a
	 * single "nsize_opcode" command, when SMMU doesn't implement the range
	 * invalidation feature, where there can be too many per-granule TLBIs,
	 * resulting in a soft lockup.
	 */
	/* [한국어] (위 영어 주석 참고) 알갱이 하나씩 지우는 명령이 수만 개가 되면
	 * CPU 가 그 자리에서 몇 초씩 묶여 소프트 락업 경고가 뜬다. CPU 쪽 TLB
	 * 코드가 쓰는 것과 같은 문턱값을 빌려 와, 그 이상이면 통째로 비운다. */
	max_tlbi_ops = 1 << (ilog2(granule) - 3);	/* [한국어] 알갱이가 클수록 더 많은 명령을 허용한다. */
	return size >= max_tlbi_ops * granule;	/* [한국어] 구간이 그 문턱을 넘으면 통째로. */
}

/* Used by non INV_TYPE_ATS* invalidations */
/*
 * [한국어]
 * arm_smmu_inv_to_cmdq_batch - 무효화 항목 하나를 명령으로 옮겨 담는다
 *
 * @inv: 무효화 대상 항목 (어떤 명령을 쓸지 알고 있다).
 * @cmds: 담을 묶음.
 * @cmd: 명령의 본보기 (대상 번호는 이미 채워져 있다).
 * @iova: 구간의 시작.
 * @size: 그 길이.
 * @granule: 한 걸음의 크기.
 *
 * (위 영어 주석 참고) 장치 캐시 무효화가 아닌 항목들이 쓴다. 구간이 너무
 * 넓으면 "전부 비우기" 명령 하나로 바꾸고, 아니면 구간을 덮는 명령들을
 * 만든다. 그 두 명령의 번호가 항목에 미리 적혀 있어(size_opcode /
 * nsize_opcode) 여기서 갈래를 나누기만 하면 된다.
 *
 * 실행 컨텍스트: 무효화 경로. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   __arm_smmu_domain_inv_range() → [이 함수]
 *     → arm_smmu_cmdq_batch_add_range()
 */
static void arm_smmu_inv_to_cmdq_batch(struct arm_smmu_inv *inv,
				       struct arm_smmu_cmdq_batch *cmds,
				       struct arm_smmu_cmdq_ent *cmd,
				       unsigned long iova, size_t size,
				       unsigned int granule)
{
	if (arm_smmu_inv_size_too_big(inv->smmu, size, granule)) {	/* [한국어] 구간별로 지우기엔 너무 넓다면. */
		cmd->opcode = inv->nsize_opcode;	/* [한국어] "전부 비우기" 명령으로 바꾼다. */
		arm_smmu_cmdq_batch_add(inv->smmu, cmds, cmd);	/* [한국어] 명령 하나로 끝난다. */
		return;
	}

	cmd->opcode = inv->size_opcode;	/* [한국어] 구간을 지정하는 명령. */
	arm_smmu_cmdq_batch_add_range(inv->smmu, cmds, cmd, iova, size, granule,	/* [한국어] 구간을 덮는 명령들을 만든다. */
				      inv->pgsize);
}

/*
 * [한국어]
 * arm_smmu_invs_end_batch - 여기서 묶음을 끊고 완료를 기다려야 하는가
 *
 * @cur: 지금 항목.
 * @next: 다음 항목.
 * @return: 끊어야 하면 참.
 *
 * 무효화 명령들은 대개 한 묶음으로 몰아 넣으면 되지만, 순서를 지켜야 하는
 * 자리가 셋 있다.
 * 하나, 다른 하드웨어의 명령은 다른 큐로 가므로 묶을 수 없다.
 * 둘, 2단계 무효화가 끝나야 그 아래 1단계 ASID 를 지울 수 있다 — 순서가
 * 뒤집히면 2단계가 지워지기 전에 1단계가 다시 채워질 수 있다.
 * 셋, 장치 캐시 무효화는 SMMU 쪽 무효화가 모두 끝난 뒤에 가야 한다.
 * 그러지 않으면 장치가 아직 지워지지 않은 변환을 다시 캐시에 담는다.
 *
 * 실행 컨텍스트: 무효화 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   __arm_smmu_domain_inv_range() → [이 함수]
 */
static inline bool arm_smmu_invs_end_batch(struct arm_smmu_inv *cur,
					   struct arm_smmu_inv *next)
{
	/* Changing smmu means changing command queue */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 다르면 큐도 다르다 — 한 묶음에
	 * 담을 수 없다. */
	if (cur->smmu != next->smmu)
		return true;
	/* The batch for S2 TLBI must be done before nested S1 ASIDs */
	/* [한국어] (위 영어 주석 참고) 2단계를 먼저 지워야 그 아래 1단계 변환이
	 * 되살아나지 않는다. 순서를 지키려면 그 사이에 완료 대기가 필요하다. */
	if (cur->type != INV_TYPE_S2_VMID_S1_CLEAR &&
	    next->type == INV_TYPE_S2_VMID_S1_CLEAR)
		return true;
	/* ATS must be after a sync of the S1/S2 invalidations */
	/* [한국어] (위 영어 주석 참고) 장치 캐시를 먼저 비우면, 아직 지워지지 않은
	 * SMMU 쪽 변환을 장치가 다시 물어다 캐시에 담는다. 반드시 SMMU 쪽이
	 * 끝난 뒤여야 한다. */
	if (!arm_smmu_inv_is_ats(cur) && arm_smmu_inv_is_ats(next))
		return true;
	return false;	/* [한국어] 그 밖에는 계속 모아 담아도 된다. */
}

/*
 * [한국어]
 * __arm_smmu_domain_inv_range - 무효화 배열을 훑어 명령을 모두 낸다
 *
 * @invs: 그 도메인의 무효화 대상 배열.
 * @iova: 구간의 시작.
 * @size: 그 길이 (0 이면 전부).
 * @granule: 한 걸음의 크기.
 * @leaf: 마지막 단계만 지울 것인가.
 *
 * 무효화의 실제 몸통이다. 배열에 미리 모아 둔 대상들을 훑으며 종류에 맞는
 * 명령을 짓고, 순서를 지켜야 하는 자리에서만 묶음을 끊는다.
 *
 * 배열을 쓰는 설계의 이점이 여기서 드러난다. 도메인에 장치가 몇 개 붙어
 * 있든, 무효화할 대상은 이미 정렬된 배열에 모여 있어 한 번만 훑으면 된다.
 * 장치 목록을 뒤지며 중복을 걸러 낼 필요가 없다.
 *
 * 버려진 자리(참조 0)를 건너뛰는 코드가 두 군데 있는데, 그것이 제자리 해제
 * 설계의 대가다. 대신 해제 경로가 절대 실패하지 않는다.
 *
 * 실행 컨텍스트: 무효화 경로. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_domain_inv_range() → [이 함수] → arm_smmu_cmdq_batch_submit()
 */
static void __arm_smmu_domain_inv_range(struct arm_smmu_invs *invs,
					unsigned long iova, size_t size,
					unsigned int granule, bool leaf)
{
	struct arm_smmu_cmdq_batch cmds = {};	/* [한국어] 모아 담을 명령 묶음. */
	struct arm_smmu_inv *cur;	/* [한국어] 지금 다루는 항목. */
	struct arm_smmu_inv *end;	/* [한국어] 배열의 끝. */

	cur = invs->inv;	/* [한국어] 처음부터. */
	end = cur + READ_ONCE(invs->num_invs);	/* [한국어] 길이는 다른 CPU 가 줄일 수 있어 READ_ONCE 로 읽는다. */
	/* Skip any leading entry marked as a trash */
	/* [한국어] (위 영어 주석 참고) 앞머리의 버려진 자리를 건너뛴다 — 아래
	 * 반복이 "지금 항목은 살아 있다"를 전제하기 때문이다. */
	for (; cur != end; cur++)
		if (READ_ONCE(cur->users))	/* [한국어] 참조가 남아 있으면 살아 있는 항목이다. */
			break;
	while (cur != end) {	/* [한국어] 살아 있는 항목마다. */
		struct arm_smmu_device *smmu = cur->smmu;	/* [한국어] 이 항목이 향할 하드웨어. */
		struct arm_smmu_cmdq_ent cmd = {	/* [한국어] 지을 명령. */
			/*
			 * Pick size_opcode to run arm_smmu_get_cmdq(). This can
			 * be changed to nsize_opcode, which would result in the
			 * same CMDQ pointer.
			 */
			/* [한국어] (위 영어 주석 참고) 큐를 고를 때 명령 종류를 보는데,
			 * 두 종류 어느 쪽이든 같은 큐로 가므로 아무 쪽이나 넣어 두면 된다. */
			.opcode = cur->size_opcode,
		};
		struct arm_smmu_inv *next;	/* [한국어] 다음으로 살아 있는 항목. */

		if (!cmds.num)	/* [한국어] 묶음이 비어 있으면 (처음이거나 방금 내보낸 뒤). */
			arm_smmu_cmdq_batch_init(smmu, &cmds, &cmd);	/* [한국어] 이 하드웨어의 큐로 묶음을 새로 시작한다. */

		switch (cur->type) {	/* [한국어] 항목 종류에 따라 명령을 다르게 짓는다. */
		case INV_TYPE_S1_ASID:	/* [한국어] 1단계 주소 공간 무효화. */
			cmd.tlbi.asid = cur->id;	/* [한국어] 그 ASID. */
			cmd.tlbi.leaf = leaf;	/* [한국어] 호출자가 정한 깊이. */
			arm_smmu_inv_to_cmdq_batch(cur, &cmds, &cmd, iova, size,	/* [한국어] 구간이 넓으면 전부 비우기로 바뀐다. */
						   granule);
			break;
		case INV_TYPE_S2_VMID:	/* [한국어] 2단계 무효화. */
			cmd.tlbi.vmid = cur->id;	/* [한국어] 그 VMID. */
			cmd.tlbi.leaf = leaf;	/* [한국어] 호출자가 정한 깊이를 그대로 쓴다. */
			arm_smmu_inv_to_cmdq_batch(cur, &cmds, &cmd, iova, size,	/* [한국어] 구간이 넓으면 전부 비우기로 바뀐다. */
						   granule);
			break;
		case INV_TYPE_S2_VMID_S1_CLEAR:	/* [한국어] 중첩에서 그 VMID 아래 1단계까지 함께 지우는 항목. */
			/* CMDQ_OP_TLBI_S12_VMALL already flushed S1 entries */
			/* [한국어] (위 영어 주석 참고) 구간이 넓어 앞의 2단계 무효화가
			 * "전부 비우기"로 바뀌었다면, 그 명령이 이미 1단계까지 지웠다.
			 * 여기서 또 낼 필요가 없다. */
			if (arm_smmu_inv_size_too_big(cur->smmu, size, granule))
				break;
			cmd.tlbi.vmid = cur->id;	/* [한국어] 그 VMID 아래의 1단계 항목을 지운다. */
			arm_smmu_cmdq_batch_add(smmu, &cmds, &cmd);	/* [한국어] 주소를 가리지 않는 명령이라 범위 계산이 없다. */
			break;
		case INV_TYPE_ATS:	/* [한국어] 장치 캐시의 그 구간 무효화. */
			arm_smmu_atc_inv_to_cmd(cur->ssid, iova, size, &cmd);	/* [한국어] 정렬 요구를 맞춘 명령을 짓는다. */
			cmd.atc.sid = cur->id;	/* [한국어] 대상 장치. */
			arm_smmu_cmdq_batch_add(smmu, &cmds, &cmd);	/* [한국어] 주소를 가리지 않는 명령이라 범위 계산이 없다. */
			break;
		case INV_TYPE_ATS_FULL:	/* [한국어] 장치 캐시 전체 무효화. */
			arm_smmu_atc_inv_to_cmd(IOMMU_NO_PASID, 0, 0, &cmd);	/* [한국어] PASID 를 가리지 않고 크기 0(전부)으로 짓는다. */
			cmd.atc.sid = cur->id;	/* [한국어] 대상 장치의 스트림 번호. */
			arm_smmu_cmdq_batch_add(smmu, &cmds, &cmd);	/* [한국어] 묶음에 담는다 — 발행은 나중에 한꺼번에. */
			break;
		default:	/* [한국어] 모르는 종류. */
			WARN_ON_ONCE(1);	/* [한국어] 배열에 잘못된 항목이 들어갔다는 뜻이다. */
			break;
		}

		/* Skip any trash entry in-between */
		/* [한국어] (위 영어 주석 참고) 가운데의 버려진 자리도 건너뛴다 —
		 * 제자리 해제 설계가 남긴 흔적이다. */
		for (next = cur + 1; next != end; next++)
			if (READ_ONCE(next->users))
				break;

		if (cmds.num &&	/* [한국어] 담긴 것이 있고. */
		    (next == end || arm_smmu_invs_end_batch(cur, next))) {	/* [한국어] 배열이 끝났거나 순서를 지켜야 하는 경계라면. */
			arm_smmu_cmdq_batch_submit(smmu, &cmds);	/* [한국어] 여기서 내보내고 완료를 기다린다. */
			cmds.num = 0;	/* [한국어] 다음 묶음은 새 하드웨어·새 종류로 시작한다. */
		}
		cur = next;	/* [한국어] 다음 항목으로. */
	}
}

/*
 * [한국어]
 * arm_smmu_domain_inv_range - 도메인의 주소 구간을 무효화한다
 *
 * @smmu_domain: 대상 도메인.
 * @iova: 구간의 시작.
 * @size: 그 길이 (0 이면 도메인 전부).
 * @granule: 한 걸음의 크기.
 * @leaf: 마지막 단계만 지울 것인가.
 *
 * 무효화의 공개 진입점이다. 하는 일은 배열을 RCU 로 읽어 몸통 함수에
 * 넘기는 것뿐이지만, 그 앞뒤의 동기화가 이 함수의 전부다.
 *
 * 맨 앞의 smp_mb() 가 미묘하다. 주석의 다이어그램이 설명하듯, 페이지 테이블을
 * 고친 CPU 와 도메인을 붙이는 CPU 가 서로를 기다리는 상황이 생길 수 있다.
 * 붙이는 쪽은 배열을 바꾼 뒤 STE 를 쓰며 dma_wmb() 로 순서를 세우고,
 * 무효화하는 쪽은 테이블을 고친 뒤 배열을 읽는다. 두 순서가 짝을 이뤄야
 * "테이블 변경이 하드웨어에 보인 뒤에 새 배열을 읽는다"가 성립한다.
 *
 * ATS 가 없으면 락을 잡지 않는 것도 요점이다. 장치 캐시 무효화는 배열
 * 길이가 줄어드는 것과 겹치면 안 되지만, ATS 항목이 없으면 그 걱정이
 * 없어 락 비용을 아낀다.
 *
 * 실행 컨텍스트: 매핑 해제 경로. 원자적 문맥에서도 불린다.
 *
 * 호출 체인:
 *   io-pgtable/SVA/도메인 관리 → [이 함수] → __arm_smmu_domain_inv_range()
 */
void arm_smmu_domain_inv_range(struct arm_smmu_domain *smmu_domain,
			       unsigned long iova, size_t size,
			       unsigned int granule, bool leaf)
{
	struct arm_smmu_invs *invs;	/* [한국어] RCU 로 읽어 올 무효화 배열. */

	/*
	 * An invalidation request must follow some IOPTE change and then load
	 * an invalidation array. In the meantime, a domain attachment mutates
	 * the array and then stores an STE/CD asking SMMU HW to acquire those
	 * changed IOPTEs.
	 *
	 * When running alone, a domain attachment relies on the dma_wmb() in
	 * arm_smmu_write_entry() used by arm_smmu_install_ste_for_dev().
	 *
	 * But in a race, these two can be interdependent, making it a special
	 * case requiring an additional smp_mb() for the write->read ordering.
	 * Pairing with the dma_wmb() in arm_smmu_install_ste_for_dev(), this
	 * makes sure that IOPTE update prior to this point is visible to SMMU
	 * hardware before we load the updated invalidation array.
	 *
	 *  [CPU0]                        | [CPU1]
	 *  change IOPTE on new domain:   |
	 *  arm_smmu_domain_inv_range() { | arm_smmu_install_new_domain_invs()
	 *    smp_mb(); // ensures IOPTE  | arm_smmu_install_ste_for_dev {
	 *              // seen by SMMU   |   dma_wmb(); // ensures invs update
	 *    // load the updated invs    |              // before updating STE
	 *    invs = rcu_dereference();   |   STE = TTB0;
	 *    ...                         |   ...
	 *  }                             | }
	 */
	/* [한국어] (위 영어 주석과 다이어그램 참고) 두 CPU 가 서로 의존하는 드문
	 * 경합이다. CPU0 은 페이지 테이블을 고치고 배열을 읽으려 하고, CPU1 은
	 * 배열을 바꾸고 STE 를 쓰려 한다.
	 * CPU1 쪽 순서는 STE 쓰기의 dma_wmb() 가 세워 준다. CPU0 쪽은 "쓰기 뒤
	 * 읽기" 순서라 그 장벽으로는 부족해 여기에 전체 장벽이 하나 더 필요하다.
	 * 이 장벽이 없으면 CPU0 이 옛 배열을 읽어 새로 붙은 장치를 빼먹고,
	 * 그 장치는 이미 고쳐진 페이지 테이블을 낡은 TLB 로 보게 된다. */
	smp_mb();

	rcu_read_lock();	/* [한국어] 배열이 갈아 끼워져도 우리가 읽는 것은 살아 있게 한다. */
	invs = rcu_dereference(smmu_domain->invs);	/* [한국어] 지금 걸린 배열을 잡는다. */

	/*
	 * Avoid locking unless ATS is being used. No ATC invalidation can be
	 * going on after a domain is detached.
	 */
	/* [한국어] (위 영어 주석 참고) 장치 캐시 무효화만이 배열 길이 축소와
	 * 겹쳐서는 안 된다 — 이미 떨어진 장치에 명령을 보내면 안 되기 때문이다.
	 * ATS 항목이 없으면 그 걱정이 없어 락을 건너뛴다. */
	if (invs->has_ats) {	/* [한국어] 장치 캐시 무효화 항목이 있다면. */
		unsigned long flags;	/* [한국어] 인터럽트 상태 저장 자리. */

		read_lock_irqsave(&invs->rwlock, flags);	/* [한국어] 길이를 줄이는 쪽(unref)과 겹치지 않게 막는다. */
		__arm_smmu_domain_inv_range(invs, iova, size, granule, leaf);	/* [한국어] 실제 명령을 낸다. */
		read_unlock_irqrestore(&invs->rwlock, flags);	/* [한국어] 배열 훑기가 끝났으니 읽기 락을 놓는다. */
	} else {	/* [한국어] ATS 가 없으면. */
		__arm_smmu_domain_inv_range(invs, iova, size, granule, leaf);	/* [한국어] 락 없이 곧바로 — 흔한 경로다. */
	}

	rcu_read_unlock();	/* [한국어] 이제 옛 배열이 놓여도 된다. */
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_page_nosync - 페이지 하나를 무효화 목록에 모아 둔다
 *
 * @gather: 모아 둘 목록.
 * @iova: 그 페이지의 주소.
 * @granule: 페이지 크기.
 * @cookie: 도메인 포인터.
 *
 * 매핑을 풀 때마다 곧바로 무효화를 내면 명령이 폭주한다. 그래서 코어의
 * "모으기" 장치에 주소를 쌓아 두고, 나중에 한 번에 무효화한다.
 * 이름의 nosync 가 그 뜻이다 — 지금 하드웨어에 아무것도 보내지 않는다.
 *
 * 실행 컨텍스트: io-pgtable 콜백. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   io-pgtable → tlb_add_page 갈고리 = [이 함수]
 *     → iommu_iotlb_gather_add_page()
 */
static void arm_smmu_tlb_inv_page_nosync(struct iommu_iotlb_gather *gather,
					 unsigned long iova, size_t granule,
					 void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] io-pgtable 에 등록해 둔 도메인. */
	struct iommu_domain *domain = &smmu_domain->domain;	/* [한국어] 코어가 아는 형태. */

	iommu_iotlb_gather_add_page(domain, gather, iova, granule);	/* [한국어] 인접한 주소는 하나로 합쳐 쌓인다 — 나중에 범위 무효화 하나로 끝날 수 있다. */
}

/*
 * [한국어]
 * arm_smmu_tlb_inv_walk - 표를 걷는 경로까지 포함해 구간을 무효화한다
 *
 * @iova: 구간의 시작.
 * @size: 그 길이.
 * @granule: 한 걸음의 크기.
 * @cookie: 도메인 포인터.
 *
 * io-pgtable 이 중간 단계 표를 바꿨을 때 부르는 갈고리다. leaf 를 거짓으로
 * 넘기는 것이 요점 — 마지막 단계뿐 아니라 걸어온 경로의 캐시까지 버려야
 * 하드웨어가 새 표를 다시 읽는다.
 *
 * 실행 컨텍스트: io-pgtable 콜백. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   io-pgtable → tlb_flush_walk 갈고리 = [이 함수]
 *     → arm_smmu_domain_inv_range()
 */
static void arm_smmu_tlb_inv_walk(unsigned long iova, size_t size,
				  size_t granule, void *cookie)
{
	struct arm_smmu_domain *smmu_domain = cookie;	/* [한국어] io-pgtable 에 등록해 둔 도메인. */

	arm_smmu_domain_inv_range(smmu_domain, iova, size, granule, false);	/* [한국어] leaf=false — 중간 단계 캐시까지 버린다. */
}

/* [한국어] io-pgtable 이 이 드라이버를 되부르는 갈고리표.
 *
 * 페이지 테이블을 짓고 푸는 일은 io-pgtable 이 하지만, 그 변경을 하드웨어에
 * 알리는 일은 이 드라이버만 할 수 있다. 이 표가 그 경계다. */
static const struct iommu_flush_ops arm_smmu_flush_ops = {
	.tlb_flush_all	= arm_smmu_tlb_inv_context,	/* [한국어] 통째로 비우기 — 크게 바뀌었을 때. */
	.tlb_flush_walk = arm_smmu_tlb_inv_walk,	/* [한국어] 중간 단계까지 포함한 구간 무효화. */
	.tlb_add_page	= arm_smmu_tlb_inv_page_nosync,	/* [한국어] 나중에 몰아 낼 목록에 쌓아 두기. */
};

/*
 * [한국어]
 * arm_smmu_dbm_capable - 이 하드웨어가 더티 페이지 추적을 지원하는가
 *
 * @smmu: 검사할 하드웨어.
 * @return: 지원하면 참.
 *
 * 두 조건이 함께 필요하다. 하드웨어가 더티 비트를 스스로 갱신할 수 있어야
 * 하고(HD), 그 갱신이 CPU 에게 곧바로 보여야 한다(캐시 일관성). 일관성이
 * 없으면 CPU 가 페이지 테이블을 읽어도 하드웨어가 표시한 더티 비트를
 * 못 보므로, 바뀐 페이지를 놓쳐 마이그레이션이 깨진다.
 *
 * 실행 컨텍스트: 능력 조회. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_capable()/domain_alloc → [이 함수]
 */
static bool arm_smmu_dbm_capable(struct arm_smmu_device *smmu)
{
	u32 features = (ARM_SMMU_FEAT_HD | ARM_SMMU_FEAT_COHERENCY);	/* [한국어] 함께 있어야 하는 두 기능. */

	return (smmu->features & features) == features;	/* [한국어] 둘 다 있을 때만 참. */
}

/* IOMMU API */
/*
 * [한국어]
 * arm_smmu_capable - 코어가 묻는 능력에 답한다
 *
 * @dev: 그 장치.
 * @cap: 묻는 능력.
 * @return: 지원하면 참.
 *
 * 코어가 이 IOMMU 로 무엇을 할 수 있는지 판단할 때 부른다. 상위 계층이
 * 이 답을 보고 DMA 전략이나 사용자 공간 노출 여부를 정하므로, 잘못
 * 답하면 상위가 잘못된 가정을 하게 된다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu 코어 → iommu_ops->capable = [이 함수]
 */
static bool arm_smmu_capable(struct device *dev, enum iommu_cap cap)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 그 장치의 SMMU 쪽 상태. */

	switch (cap) {	/* [한국어] 값에 따라 갈래를 나눈다. */
	case IOMMU_CAP_CACHE_COHERENCY:	/* [한국어] 표 순회가 CPU 캐시와 일관적인가. */
		/* Assume that a coherent TCU implies coherent TBUs */
		/* [한국어] (위 영어 주석 참고) 규격상 둘이 따로일 수 있지만, 변환 제어
		 * 유닛이 일관적이면 그 아래 버퍼 유닛도 그렇다고 본다 — 실제 하드웨어가
		 * 그렇게 만들어져 있다. */
		return master->smmu->features & ARM_SMMU_FEAT_COHERENCY;
	case IOMMU_CAP_ENFORCE_CACHE_COHERENCY:	/* [한국어] 장치가 캐시를 우회하지 못하게 강제할 수 있는가. */
		return arm_smmu_master_canwbs(master);	/* [한국어] 펌웨어가 그렇게 선언한 장치에서만 참. */
	case IOMMU_CAP_NOEXEC:	/* [한국어] 실행 금지 매핑을 만들 수 있는가. */
	case IOMMU_CAP_DEFERRED_FLUSH:	/* [한국어] 무효화를 미뤄 모아 낼 수 있는가. */
		return true;	/* [한국어] 둘 다 언제나 지원한다. */
	case IOMMU_CAP_DIRTY_TRACKING:	/* [한국어] 바뀐 페이지를 추적할 수 있는가. */
		return arm_smmu_dbm_capable(master->smmu);	/* [한국어] 하드웨어가 더티 비트를 스스로 갱신할 수 있어야 한다. */
	case IOMMU_CAP_PCI_ATS_SUPPORTED:	/* [한국어] 장치 쪽 변환 캐시를 쓸 수 있는가. */
		return arm_smmu_ats_supported(master);	/* [한국어] 하드웨어·펌웨어·장치가 모두 받쳐 줘야 참이다. */
	default:	/* [한국어] 모르는 능력. */
		return false;	/* [한국어] 지원하지 않는다고 답한다 — 모르는 것을 참으로 답하면 위험하다. */
	}
}

/*
 * [한국어]
 * arm_smmu_enforce_cache_coherency - 이 도메인의 모든 장치가 캐시 일관적인가
 *
 * @domain: 검사할 도메인.
 * @return: 모두 일관적이면 참.
 *
 * 사용자 공간에 넘긴 도메인에서, 장치가 캐시를 우회해 메모리를 건드릴 수
 * 없음을 보장해야 할 때 부른다. 하나라도 아니면 거짓이다 — 도메인의 안전은
 * 가장 약한 장치가 결정한다.
 *
 * 결과를 도메인에 기록해 두는 것이 중요하다. 이후 이 도메인에 장치를 더
 * 붙일 때, 그 장치가 일관적이지 않으면 거부해야 하기 때문이다.
 *
 * 실행 컨텍스트: 붙이기 경로. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   iommu 코어 → domain_ops->enforce_cache_coherency = [이 함수]
 */
static bool arm_smmu_enforce_cache_coherency(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인으로 되짚는다. */
	struct arm_smmu_master_domain *master_domain;	/* [한국어] 장치 목록 반복자. */
	unsigned long flags;	/* [한국어] 인터럽트 상태. */
	bool ret = true;	/* [한국어] 기본은 참 — 하나라도 아니면 거짓이 된다. */

	spin_lock_irqsave(&smmu_domain->devices_lock, flags);	/* [한국어] 훑는 동안 장치가 붙거나 떨어지면 안 된다. */
	list_for_each_entry(master_domain, &smmu_domain->devices,	/* [한국어] 이 도메인에 매달린 모든 (장치, PASID) 짝을. */
			    devices_elm) {
		if (!arm_smmu_master_canwbs(master_domain->master)) {	/* [한국어] 하나라도 캐시를 우회할 수 있다면. */
			ret = false;	/* [한국어] 도메인 전체가 보장할 수 없다. */
			break;	/* [한국어] 더 볼 필요가 없다. */
		}
	}
	smmu_domain->enforce_cache_coherency = ret;	/* [한국어] 결과를 기억해 둔다 — 이후 붙이기에서 이 값을 조건으로 쓴다. */
	spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);	/* [한국어] 목록 조작이 끝났으니 락과 인터럽트 상태를 되돌린다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_domain_alloc - 도메인 껍데기와 빈 무효화 배열을 잡는다
 *
 * @return: 만들어진 도메인, 실패하면 ERR_PTR.
 *
 * 종류를 가리지 않는 공통 부분만 마련한다 — 장치 목록, 그 락, 그리고 빈
 * 무효화 배열. 페이지 테이블과 ASID/VMID 는 도메인의 갈래가 정해진 뒤
 * finalise 단계에서 붙는다.
 *
 * 빈 배열이라도 반드시 잡아 두는 것이 중요하다. 무효화 경로가 배열이
 * NULL 인 경우를 다루지 않아도 되게 만들어, 그 경로가 단순해진다.
 *
 * 실행 컨텍스트: 도메인 할당. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_domain_alloc_paging_flags()/SVA → [이 함수]
 */
struct arm_smmu_domain *arm_smmu_domain_alloc(void)
{
	struct arm_smmu_domain *smmu_domain;	/* [한국어] 만들 도메인. */
	struct arm_smmu_invs *new_invs;	/* [한국어] 걸어 둘 빈 배열. */

	smmu_domain = kzalloc_obj(*smmu_domain);	/* [한국어] 0 으로 채워 잡는다. */
	if (!smmu_domain)	/* [한국어] 껍데기를 못 잡았으면 더 진행할 수 없다. */
		return ERR_PTR(-ENOMEM);

	new_invs = arm_smmu_invs_alloc(0);	/* [한국어] 항목 0 개짜리 배열 — 무효화 경로가 NULL 검사를 하지 않아도 되게 한다. */
	if (!new_invs) {	/* [한국어] 빈 배열조차 못 잡았다면. */
		kfree(smmu_domain);	/* [한국어] 되감기 — 방금 잡은 도메인을 놓는다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 방금 잡은 도메인을 놓고 오류를 돌려준다. */
	}

	INIT_LIST_HEAD(&smmu_domain->devices);	/* [한국어] 붙을 장치 목록을 빈 상태로 시작한다. */
	spin_lock_init(&smmu_domain->devices_lock);	/* [한국어] 그 목록을 지킬 락 — 인터럽트에서도 훑으므로 스핀락이다. */
	rcu_assign_pointer(smmu_domain->invs, new_invs);	/* [한국어] RCU 규약을 지켜 건다 — 아직 아무도 안 보지만 형태를 지킨다. */

	return smmu_domain;	/* [한국어] 만들어진 도메인을 돌려준다. */
}

/*
 * [한국어]
 * arm_smmu_domain_free_paging - 페이지 테이블 도메인을 놓는다
 *
 * @domain: 놓을 도메인.
 *
 * 페이지 테이블을 놓고, 배정받았던 번호(ASID 또는 VMID)를 돌려주고,
 * 도메인 자체를 놓는다.
 *
 * ASID 를 놓을 때 락을 잡는 이유가 미묘하다. SVA 가 같은 ASID 공간을
 * 나눠 쓰므로, 우리가 번호를 돌려주는 사이에 SVA 가 그 번호를 받아
 * 서술자를 짓기 시작하면 어긋난다.
 *
 * 실행 컨텍스트: 도메인 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free = [이 함수]
 *     → free_io_pgtable_ops() → arm_smmu_domain_free()
 */
static void arm_smmu_domain_free_paging(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인으로 되짚는다. */
	struct arm_smmu_device *smmu = smmu_domain->smmu;	/* [한국어] VMID 를 돌려줄 하드웨어. */

	free_io_pgtable_ops(smmu_domain->pgtbl_ops);	/* [한국어] 페이지 테이블을 통째로 놓는다 — 이 시점에 이 도메인을 쓰는 장치는 없다. */

	/* Free the ASID or VMID */
	/* [한국어] (위 영어 주석 참고) 도메인 갈래에 따라 돌려줄 번호가 다르다. */
	if (smmu_domain->stage == ARM_SMMU_DOMAIN_S1) {	/* [한국어] 1단계 도메인이면 ASID. */
		/* Prevent SVA from touching the CD while we're freeing it */
		/* [한국어] (위 영어 주석 참고) SVA 가 같은 ASID 공간을 쓰므로, 번호를
		 * 돌려주는 순간 SVA 가 그것을 받아 서술자를 지을 수 있다. 그 사이에
		 * 우리가 아직 정리 중이면 어긋나므로 락으로 막는다. */
		mutex_lock(&arm_smmu_asid_lock);
		xa_erase(&arm_smmu_asid_xa, smmu_domain->cd.asid);	/* [한국어] 전역 풀에 번호를 돌려준다. */
		mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기 구간이 끝났으니 락을 놓는다. */
	} else {	/* [한국어] 2단계 도메인이면 VMID. */
		struct arm_smmu_s2_cfg *cfg = &smmu_domain->s2_cfg;	/* [한국어] 그 설정. */
		if (cfg->vmid)	/* [한국어] 0 은 "2단계 없음"용 예약 번호라 돌려줄 것이 아니다. */
			ida_free(&smmu->vmid_map, cfg->vmid);	/* [한국어] VMID 는 하드웨어마다 따로 관리한다. */
	}

	arm_smmu_domain_free(smmu_domain);	/* [한국어] 무효화 배열과 도메인 몸통을 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_domain_finalise_s1 - 1단계 도메인에 ASID 를 배정한다
 *
 * @smmu: 대상 하드웨어 (ASID 폭을 여기서 본다).
 * @smmu_domain: 채울 도메인.
 * @return: 0 성공, 음수면 번호가 동났다.
 *
 * 전역 ASID 풀에서 번호 하나를 받아 도메인에 기록한다. 0 번은 예약되어
 * 있어 1 부터 배정한다.
 *
 * SVA 와 같은 풀을 쓰므로 락이 필요하다 — 주석이 말하듯 서술자에 그 번호가
 * 실릴 때까지 SVA 가 끼어들면 안 된다.
 *
 * 실행 컨텍스트: 도메인 마무리. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_domain_finalise() → [이 함수] → xa_alloc()
 */
static int arm_smmu_domain_finalise_s1(struct arm_smmu_device *smmu,
				       struct arm_smmu_domain *smmu_domain)
{
	int ret;	/* [한국어] 배정 결과. */
	u32 asid = 0;	/* [한국어] 받은 번호. */
	struct arm_smmu_ctx_desc *cd = &smmu_domain->cd;	/* [한국어] 번호를 기록할 자리. */

	/* Prevent SVA from modifying the ASID until it is written to the CD */
	/* [한국어] (위 영어 주석 참고) SVA 도 같은 풀을 쓰므로, 번호를 받고 기록할
	 * 때까지는 다른 쪽이 그 번호를 건드리지 못하게 막는다. */
	mutex_lock(&arm_smmu_asid_lock);
	ret = xa_alloc(&arm_smmu_asid_xa, &asid, smmu_domain,	/* [한국어] 빈 번호 하나를 받아 이 도메인에 묶는다. */
		       XA_LIMIT(1, (1 << smmu->asid_bits) - 1), GFP_KERNEL);	/* [한국어] 0 은 예약이라 1 부터, 위쪽 한계는 하드웨어의 ASID 폭이 정한다. */
	cd->asid	= (u16)asid;	/* [한국어] 서술자를 지을 때 이 값이 쓰인다. */
	mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기 구간이 끝났으니 락을 놓는다. */
	return ret;	/* [한국어] 번호가 동나면 도메인을 만들 수 없다. */
}

/*
 * [한국어]
 * arm_smmu_domain_finalise_s2 - 2단계 도메인에 VMID 를 배정한다
 *
 * @smmu: 대상 하드웨어 (VMID 풀이 여기 있다).
 * @smmu_domain: 채울 도메인.
 * @return: 0 성공, 음수면 번호가 동났다.
 *
 * ASID 와 달리 VMID 는 하드웨어마다 따로 관리한다 — TLB 태그가 그
 * 하드웨어 안에서만 뜻을 갖기 때문이다.
 *
 * 0 번을 예약하는 것이 중요하다. 1단계만 하는 STE 도 VMID 필드를 갖는데,
 * 거기에 0 을 넣기로 약속해 두었다. 실제 2단계 도메인에 0 을 주면 그
 * 두 종류가 같은 태그를 공유해 무효화가 뒤섞인다.
 *
 * 실행 컨텍스트: 도메인 마무리. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_domain_finalise() → [이 함수] → ida_alloc_range()
 */
static int arm_smmu_domain_finalise_s2(struct arm_smmu_device *smmu,
				       struct arm_smmu_domain *smmu_domain)
{
	int vmid;	/* [한국어] 받은 번호. */
	struct arm_smmu_s2_cfg *cfg = &smmu_domain->s2_cfg;	/* [한국어] 기록할 자리. */

	/* Reserve VMID 0 for stage-2 bypass STEs */
	/* [한국어] (위 영어 주석 참고) 1단계만 하는 STE 가 VMID 필드에 0 을 쓰기로
	 * 약속되어 있다. 실제 도메인에 0 을 주면 그 둘이 같은 태그를 공유하게 된다. */
	vmid = ida_alloc_range(&smmu->vmid_map, 1, (1 << smmu->vmid_bits) - 1,	/* [한국어] 1 부터 하드웨어의 VMID 폭까지. */
			       GFP_KERNEL);
	if (vmid < 0)	/* [한국어] 번호가 동났다면. */
		return vmid;	/* [한국어] 이 하드웨어에 더 이상 2단계 도메인을 만들 수 없다. */

	cfg->vmid	= (u16)vmid;	/* [한국어] STE 를 지을 때 이 값이 쓰인다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_domain_finalise - 도메인의 갈래를 정하고 페이지 테이블을 짓는다
 *
 * @smmu_domain: 마무리할 도메인.
 * @smmu: 이 도메인이 붙을 하드웨어.
 * @flags: 사용자가 요청한 옵션 (더티 추적, 중첩 부모 등).
 * @return: 0 성공, 음수 오류.
 *
 * 도메인 할당의 두 번째 단계다. 껍데기는 이미 있고, 여기서 실제 변환
 * 자원을 붙인다 — 페이지 테이블 형식을 정하고, io-pgtable 에게 테이블을
 * 짓게 하고, ASID 나 VMID 를 배정한다.
 *
 * 첫 단계에서 하드웨어를 몰랐기 때문에 이 단계가 따로 있다. 주소 폭도,
 * 지원하는 페이지 크기도, 캐시 일관성도 모두 하드웨어가 정하는데, 도메인을
 * 만들 때는 어느 하드웨어에 붙을지 아직 모를 수 있다.
 *
 * 1단계와 2단계의 입력 주소 폭이 다른 것에 주의할 만하다. 1단계는 CPU 의
 * 가상 주소 폭을 넘을 수 없고, 2단계는 물리 주소 폭까지 받는다 — 게스트
 * 물리 주소를 입력으로 받기 때문이다.
 *
 * 실행 컨텍스트: 도메인 할당·첫 붙이기. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_domain_alloc_paging_flags()/attach_dev() → [이 함수]
 *     → alloc_io_pgtable_ops() → finalise_s1()/finalise_s2()
 */
static int arm_smmu_domain_finalise(struct arm_smmu_domain *smmu_domain,
				    struct arm_smmu_device *smmu, u32 flags)
{
	int ret;	/* [한국어] 중간 결과. */
	enum io_pgtable_fmt fmt;	/* [한국어] 지을 페이지 테이블의 형식. */
	struct io_pgtable_cfg pgtbl_cfg;	/* [한국어] io-pgtable 에 넘길 설정. */
	struct io_pgtable_ops *pgtbl_ops;	/* [한국어] 만들어진 테이블의 연산표. */
	int (*finalise_stage_fn)(struct arm_smmu_device *smmu,	/* [한국어] 갈래에 따라 다른 번호 배정 함수. */
				 struct arm_smmu_domain *smmu_domain);
	bool enable_dirty = flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING;	/* [한국어] 더티 추적을 요청했는가. */

	pgtbl_cfg = (struct io_pgtable_cfg) {	/* [한국어] 갈래와 무관한 공통 설정을 먼저 채운다. */
		.pgsize_bitmap	= smmu->pgsize_bitmap,	/* [한국어] 이 하드웨어가 다룰 수 있는 페이지 크기들. */
		.coherent_walk	= smmu->features & ARM_SMMU_FEAT_COHERENCY,	/* [한국어] 표 순회가 캐시 일관적인가 — 아니면 io-pgtable 이 캐시를 직접 씻는다. */
		.tlb		= &arm_smmu_flush_ops,	/* [한국어] 테이블이 바뀔 때 우리를 되부를 갈고리표. */
		.iommu_dev	= smmu->dev,	/* [한국어] 표 메모리를 잡을 장치. */
	};

	switch (smmu_domain->stage) {	/* [한국어] 도메인의 갈래에 따라 나머지를 채운다. */
	case ARM_SMMU_DOMAIN_S1: {	/* [한국어] 1단계 — 장치가 낸 주소를 곧바로 물리 주소로. */
		unsigned long ias = (smmu->features &	/* [한국어] 하드웨어가 다룰 수 있는 입력 주소 폭. */
				     ARM_SMMU_FEAT_VAX) ? 52 : 48;	/* [한국어] 확장 가상 주소를 지원하면 52비트. */

		pgtbl_cfg.ias = min_t(unsigned long, ias, VA_BITS);	/* [한국어] CPU 의 가상 주소 폭도 넘을 수 없다 — SVA 에서 같은 테이블을 써야 하기 때문이다. */
		pgtbl_cfg.oas = smmu->oas;	/* [한국어] 출력은 하드웨어의 물리 주소 폭까지. */
		if (enable_dirty)	/* [한국어] 더티 추적을 요청했다면. */
			pgtbl_cfg.quirks |= IO_PGTABLE_QUIRK_ARM_HD;	/* [한국어] 하드웨어가 더티 비트를 갱신하도록 테이블을 짓게 한다. */
		fmt = ARM_64_LPAE_S1;	/* [한국어] 1단계 LPAE 형식. */
		finalise_stage_fn = arm_smmu_domain_finalise_s1;	/* [한국어] ASID 를 배정한다. */
		break;
	}
	case ARM_SMMU_DOMAIN_S2:	/* [한국어] 2단계 — 게스트 물리 주소를 실제 물리 주소로. */
		if (enable_dirty)	/* [한국어] 2단계에서는 더티 추적을 지원하지 않는다. */
			return -EOPNOTSUPP;
		pgtbl_cfg.ias = smmu->oas;	/* [한국어] 입력이 게스트 물리 주소라 물리 주소 폭만큼 받는다. */
		pgtbl_cfg.oas = smmu->oas;	/* [한국어] 출력도 같다. */
		fmt = ARM_64_LPAE_S2;	/* [한국어] 2단계 LPAE 형식 — 표를 걷는 방식이 1단계와 다르다. */
		finalise_stage_fn = arm_smmu_domain_finalise_s2;	/* [한국어] VMID 를 배정한다. */
		if ((smmu->features & ARM_SMMU_FEAT_S2FWB) &&	/* [한국어] 캐시 속성 강제를 지원하고. */
		    (flags & IOMMU_HWPT_ALLOC_NEST_PARENT))	/* [한국어] 이 도메인이 중첩의 부모로 쓰일 것이라면. */
			pgtbl_cfg.quirks |= IO_PGTABLE_QUIRK_ARM_S2FWB;	/* [한국어] 게스트가 캐시를 우회하지 못하게 하는 설정으로 짓는다. */
		break;
	default:	/* [한국어] 갈래가 정해지지 않았거나 SVA 도메인이 잘못 온 경우. */
		return -EINVAL;	/* [한국어] 인자나 상태가 조건에 맞지 않는다. */
	}

	pgtbl_ops = alloc_io_pgtable_ops(fmt, &pgtbl_cfg, smmu_domain);	/* [한국어] 실제 페이지 테이블을 짓는다 — 도메인 포인터가 무효화 갈고리의 cookie 가 된다. */
	if (!pgtbl_ops)	/* [한국어] 메모리가 없거나 설정이 잘못됐다면. */
		return -ENOMEM;

	smmu_domain->domain.pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;	/* [한국어] io-pgtable 이 실제로 쓰기로 한 크기들을 코어에 알린다 — 요청한 것보다 좁을 수 있다. */
	smmu_domain->domain.geometry.aperture_end = (1UL << pgtbl_cfg.ias) - 1;	/* [한국어] 이 도메인이 다룰 수 있는 주소의 상한. */
	smmu_domain->domain.geometry.force_aperture = true;	/* [한국어] 코어가 그 범위 밖의 매핑 요청을 거부하게 한다. */
	if (enable_dirty && smmu_domain->stage == ARM_SMMU_DOMAIN_S1)	/* [한국어] 더티 추적이 실제로 켜진 경우에만. */
		smmu_domain->domain.dirty_ops = &arm_smmu_dirty_ops;	/* [한국어] 바뀐 페이지를 읽어 갈 연산표를 건다. */

	ret = finalise_stage_fn(smmu, smmu_domain);	/* [한국어] ASID 또는 VMID 를 배정한다. */
	if (ret < 0) {	/* [한국어] 번호가 동났다면. */
		free_io_pgtable_ops(pgtbl_ops);	/* [한국어] 방금 지은 테이블을 되돌린다. */
		return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
	}

	smmu_domain->pgtbl_ops = pgtbl_ops;	/* [한국어] 이제 매핑을 걸 수 있다. */
	smmu_domain->smmu = smmu;	/* [한국어] 어느 하드웨어의 도메인인지 확정한다 — 다른 하드웨어에는 붙일 수 없게 된다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_get_step_for_sid - 그 스트림 번호의 표 항목이 놓인 자리를 찾는다
 *
 * @smmu: 대상 하드웨어.
 * @sid: 찾을 스트림 번호.
 * @return: 그 항목의 주소.
 *
 * 표가 평면인지 2단계인지에 따라 찾는 길이 다르다. 2단계에서 아래쪽 표가
 * 없는 경우를 다루지 않는데, 장치를 프로브할 때 이미 그 표를 잡아 두었기
 * 때문이다 — 등록되지 않은 스트림 번호로 이 함수를 부르면 안 된다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_install_ste_for_dev() → [이 함수]
 */
static struct arm_smmu_ste *
arm_smmu_get_step_for_sid(struct arm_smmu_device *smmu, u32 sid)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;	/* [한국어] 스트림 표 설정. */

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {	/* [한국어] 2단계 표라면. */
		/* Two-level walk */
		/* [한국어] (위 영어 주석 참고) 위쪽 첨자로 아래쪽 표를 찾고, 아래쪽
		 * 첨자로 항목에 닿는다. 아래쪽 표는 장치 프로브 때 이미 잡혀 있다. */
		return &cfg->l2.l2ptrs[arm_smmu_strtab_l1_idx(sid)]
				->stes[arm_smmu_strtab_l2_idx(sid)];
	} else {	/* [한국어] 평면 표라면. */
		/* Simple linear lookup */
		/* [한국어] (위 영어 주석 참고) 번호가 곧 첨자다. */
		return &cfg->linear.table[sid];
	}
}

/*
 * [한국어]
 * arm_smmu_install_ste_for_dev - 그 장치의 모든 스트림에 같은 항목을 심는다
 *
 * @master: 대상 장치.
 * @target: 심을 항목 값.
 *
 * 한 장치가 스트림 번호를 여럿 가질 수 있다 — PCI 브리지 뒤의 장치가
 * 별칭 번호를 갖거나, 다기능 장치가 기능마다 번호를 갖는 경우다. 그
 * 목록을 돌며 같은 값을 심는다.
 *
 * 중복 번호를 건너뛰는 것이 중요하다. 브리지 뒤의 장치들이 같은 번호로
 * 접히면 목록에 같은 값이 여러 번 들어오는데, 그대로 두면 같은 항목을
 * 여러 번 고쳐 쓰며 그때마다 무효화 명령을 내게 된다.
 *
 * 항목을 심기 전에 두 상태를 기록해 두는 것도 요점이다 — 이후 코드가
 * "지금 하드웨어에 실제로 무엇이 걸려 있는가"를 이 값으로 판단한다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 완료를 기다리므로 시간이 걸린다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev() 등 → [이 함수] → arm_smmu_write_ste()
 */
void arm_smmu_install_ste_for_dev(struct arm_smmu_master *master,
				  const struct arm_smmu_ste *target)
{
	int i, j;	/* [한국어] 바깥 반복자와 중복 검사용 반복자. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 표를 가진 하드웨어. */

	master->cd_table.in_ste =	/* [한국어] 문맥 서술자 표가 지금 항목에 실려 있는가. */
		FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(target->data[0])) ==
		STRTAB_STE_0_CFG_S1_TRANS;	/* [한국어] 1단계 변환 설정일 때만 표가 실린다 — 표를 바꾸려면 먼저 떼어야 해서 이 상태가 필요하다. */
	master->ste_ats_enabled =	/* [한국어] 하드웨어가 지금 ATS 를 받아 주는가. */
		FIELD_GET(STRTAB_STE_1_EATS, le64_to_cpu(target->data[1])) ==
		STRTAB_STE_1_EATS_TRANS;	/* [한국어] PCI 쪽 ATS 를 켜고 끄는 순서를 정할 때 이 값을 본다. */

	for (i = 0; i < master->num_streams; ++i) {	/* [한국어] 이 장치의 모든 스트림 번호에. */
		u32 sid = master->streams[i].id;	/* [한국어] 그 번호. */
		struct arm_smmu_ste *step =	/* [한국어] 그 번호의 표 항목이 놓인 자리. */
			arm_smmu_get_step_for_sid(smmu, sid);

		/* Bridged PCI devices may end up with duplicated IDs */
		/* [한국어] (위 영어 주석 참고) 브리지 뒤의 장치들은 같은 스트림 번호로
		 * 접힐 수 있어 목록에 같은 값이 여러 번 들어온다. 앞에서 이미 처리한
		 * 번호면 건너뛴다 — 그러지 않으면 무효화 명령이 헛되이 늘어난다. */
		for (j = 0; j < i; j++)	/* [한국어] 앞쪽을 훑어. */
			if (master->streams[j].id == sid)	/* [한국어] 같은 번호가 있었는지 본다. */
				break;
		if (j < i)	/* [한국어] 있었다면. */
			continue;	/* [한국어] 이미 심었으므로 넘어간다. */

		arm_smmu_write_ste(master, sid, step, target);	/* [한국어] 하드웨어가 중간 상태를 못 보게 나눠 쓴다. */
	}
}

/*
 * [한국어]
 * arm_smmu_ats_supported - 이 장치에 ATS 를 쓸 수 있는가
 *
 * @master: 검사할 장치.
 * @return: 쓸 수 있으면 참.
 *
 * 세 조건이 모두 필요하다. SMMU 가 ATS 를 지원하고, 펌웨어가 이 루트
 * 컴플렉스에서 ATS 를 써도 된다고 선언했으며, 장치 자신이 PCI 이고 ATS
 * 능력을 갖고 있어야 한다.
 *
 * 펌웨어 선언이 필요한 이유는, ATS 가 켜지면 장치가 스스로 번역된 주소로
 * 접근할 수 있게 되어 IOMMU 를 우회하는 셈이 되기 때문이다. 그 신뢰를
 * 플랫폼이 보증해야 한다.
 *
 * 실행 컨텍스트: 붙이기 경로와 능력 조회. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_capable()/attach_prepare() → [이 함수]
 */
static bool arm_smmu_ats_supported(struct arm_smmu_master *master)
{
	struct device *dev = master->dev;	/* [한국어] 그 장치. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 그 하드웨어. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 펌웨어가 적어 둔 정보. */

	if (!(smmu->features & ARM_SMMU_FEAT_ATS))	/* [한국어] SMMU 가 ATS 를 다룰 수 없으면. */
		return false;

	if (!(fwspec->flags & IOMMU_FWSPEC_PCI_RC_ATS))	/* [한국어] 펌웨어가 이 루트 컴플렉스에서 ATS 를 허용하지 않았다면. */
		return false;	/* [한국어] 플랫폼이 보증하지 않는 신뢰를 드라이버가 가정할 수는 없다. */

	return dev_is_pci(dev) && pci_ats_supported(to_pci_dev(dev));	/* [한국어] 장치 자신도 그 능력을 갖고 있어야 한다. */
}

/*
 * [한국어]
 * arm_smmu_enable_ats - 그 장치의 변환 캐시를 켠다
 *
 * @master: 대상 장치.
 *
 * 켜기 전에 캐시를 먼저 비우는 것이 요점이다. 앞서 이 장치를 쓰던 주인이
 * 남긴 항목이 캐시에 남아 있을 수 있고, 그것을 그대로 두면 새 주인의
 * 주소가 옛 매핑으로 번역된다.
 *
 * 최소 변환 단위(STU)로 SMMU 가 지원하는 가장 작은 페이지 크기를 준다.
 * 장치가 그보다 잘게 캐시를 관리하면 무효화가 그 알갱이를 못 덮는다.
 *
 * 실행 컨텍스트: 붙이기 경로(commit 단계). 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_commit() → [이 함수] → pci_enable_ats()
 */
static void arm_smmu_enable_ats(struct arm_smmu_master *master)
{
	size_t stu;	/* [한국어] 최소 변환 단위(2의 지수). */
	struct pci_dev *pdev;	/* [한국어] PCI 장치. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 페이지 크기 정보를 가진 하드웨어. */

	/* Smallest Translation Unit: log2 of the smallest supported granule */
	/* [한국어] (위 영어 주석 참고) 장치가 이보다 잘게 캐시를 나누면 무효화
	 * 명령이 그 알갱이를 덮지 못한다. */
	stu = __ffs(smmu->pgsize_bitmap);	/* [한국어] 지원하는 가장 작은 페이지 크기의 로그. */
	pdev = to_pci_dev(master->dev);	/* [한국어] ATS 는 PCI 장치에만 있다 — 호출 전에 확인되어 있다. */

	/*
	 * ATC invalidation of PASID 0 causes the entire ATC to be flushed.
	 */
	/* [한국어] (위 영어 주석 참고) PASID 없이 낸 무효화는 그 장치의 캐시를
	 * 통째로 비운다. 앞 주인이 남긴 항목을 지우려는 것이므로 그 성질이
	 * 오히려 필요하다. */
	arm_smmu_atc_inv_master(master, IOMMU_NO_PASID);	/* [한국어] 켜기 전에 반드시 비운다. */
	if (pci_enable_ats(pdev, stu))	/* [한국어] PCI 설정 공간에서 실제로 켠다. */
		dev_err(master->dev, "Failed to enable ATS (STU %zu)\n", stu);	/* [한국어] 실패해도 붙이기는 이어 간다 — ATS 없이도 동작하기 때문이다. */
}

/*
 * [한국어]
 * arm_smmu_enable_pasid - 그 장치의 PASID 기능을 켠다
 *
 * @master: 대상 장치.
 * @return: 0 성공, 음수 오류.
 *
 * 장치가 여러 주소 공간을 동시에 쓸 수 있게 한다. 장치와 SMMU 가 지원하는
 * PASID 폭 중 작은 쪽을 택해 기록해 두고, 그 값이 나중에 문맥 서술자 표의
 * 크기를 정한다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_probe_device() → [이 함수] → pci_enable_pasid()
 */
static int arm_smmu_enable_pasid(struct arm_smmu_master *master)
{
	int ret;	/* [한국어] 결과. */
	int features;	/* [한국어] 장치가 지원하는 PASID 기능 조합. */
	int num_pasids;	/* [한국어] 장치가 지원하는 PASID 개수. */
	struct pci_dev *pdev;	/* [한국어] PCI 장치. */

	if (!dev_is_pci(master->dev))	/* [한국어] PASID 는 PCI 개념이다. */
		return -ENODEV;

	pdev = to_pci_dev(master->dev);	/* [한국어] PASID 는 PCI 개념이라 그 형으로 되짚는다. */

	features = pci_pasid_features(pdev);	/* [한국어] 실행 권한·특권 요청 등 어떤 기능을 지원하는지. */
	if (features < 0)	/* [한국어] PASID 능력 자체가 없으면. */
		return features;

	num_pasids = pci_max_pasids(pdev);	/* [한국어] 몇 개까지 쓸 수 있는지. */
	if (num_pasids <= 0)	/* [한국어] 쓸 수 있는 PASID 가 없다면. */
		return num_pasids;

	ret = pci_enable_pasid(pdev, features);	/* [한국어] 장치 쪽 기능을 켠다. */
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_err(&pdev->dev, "Failed to enable PASID\n");	/* [한국어] 장치 쪽 기능을 못 켰다. */
		return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
	}

	master->ssid_bits = min_t(u8, ilog2(num_pasids),	/* [한국어] 장치가 지원하는 폭과. */
				  master->smmu->ssid_bits);	/* [한국어] SMMU 가 지원하는 폭 중 작은 쪽 — 이 값이 문맥 표 크기를 정한다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_disable_pasid - 그 장치의 PASID 기능을 끈다
 *
 * @master: 대상 장치.
 *
 * 장치를 놓을 때 켜 두었던 것을 되돌린다. ssid_bits 를 0 으로 만들어,
 * 이후 코드가 "이 장치는 PASID 를 쓰지 않는다"고 판단하게 한다.
 *
 * 실행 컨텍스트: 장치 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_release_device() → [이 함수] → pci_disable_pasid()
 */
static void arm_smmu_disable_pasid(struct arm_smmu_master *master)
{
	struct pci_dev *pdev;	/* [한국어] PCI 장치. */

	if (!dev_is_pci(master->dev))	/* [한국어] PCI 가 아니면 켠 적도 없다. */
		return;

	pdev = to_pci_dev(master->dev);	/* [한국어] PCI 장치로 되짚는다. */

	if (!pdev->pasid_enabled)	/* [한국어] 켜지 않았으면 끌 것도 없다. */
		return;

	master->ssid_bits = 0;	/* [한국어] 이후 코드가 PASID 를 쓰지 않는 장치로 보게 한다. */
	pci_disable_pasid(pdev);	/* [한국어] 장치 쪽 기능을 끈다. */
}

/*
 * [한국어]
 * arm_smmu_find_master_domain - 도메인 목록에서 그 (장치, PASID) 짝을 찾는다
 *
 * @smmu_domain: 뒤질 도메인.
 * @domain: 코어가 아는 도메인 (SVA 처럼 감싸는 종류를 구분하려고 함께 본다).
 * @master: 찾을 장치.
 * @ssid: 그 PASID.
 * @nested_ats_flush: 중첩 경로의 장치 캐시 무효화가 필요한 짝인가.
 * @return: 찾은 고리, 없으면 NULL.
 *
 * 도메인과 장치는 다대다로 엮이고, 그 고리 하나하나가
 * struct arm_smmu_master_domain 이다. 같은 장치가 같은 도메인에 여러
 * PASID 로 붙을 수 있어 네 값을 모두 견줘야 짝을 특정할 수 있다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. devices_lock 을 쥔 채 불려야 한다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare()/attach_commit() → [이 함수]
 */
static struct arm_smmu_master_domain *
arm_smmu_find_master_domain(struct arm_smmu_domain *smmu_domain,
			    struct iommu_domain *domain,
			    struct arm_smmu_master *master,
			    ioasid_t ssid, bool nested_ats_flush)
{
	struct arm_smmu_master_domain *master_domain;	/* [한국어] 목록 반복자. */

	lockdep_assert_held(&smmu_domain->devices_lock);	/* [한국어] 락을 쥐고 들어왔는지 확인한다 — 목록이 바뀌면 안 된다. */

	list_for_each_entry(master_domain, &smmu_domain->devices,	/* [한국어] 이 도메인에 매달린 모든 고리를. */
			    devices_elm) {
		if (master_domain->master == master &&	/* [한국어] 같은 장치이고. */
		    master_domain->domain == domain &&	/* [한국어] 같은 코어 도메인이고 — SVA 도메인은 감싸는 종류가 달라 이 비교가 필요하다. */
		    master_domain->ssid == ssid &&	/* [한국어] 같은 PASID 이고. */
		    master_domain->nested_ats_flush == nested_ats_flush)	/* [한국어] 무효화 성격도 같아야 한다. */
			return master_domain;	/* [한국어] 찾았다. */
	}
	return NULL;	/* [한국어] 없다 — 처음 붙는 짝이라는 뜻이다. */
}

/*
 * If the domain uses the smmu_domain->devices list return the arm_smmu_domain
 * structure, otherwise NULL. These domains track attached devices so they can
 * issue invalidations.
 */
/*
 * [한국어]
 * to_smmu_domain_devices - 장치 목록을 가진 도메인이면 그것을 돌려준다
 *
 * @domain: 코어가 준 도메인 (NULL 일 수 있다).
 * @return: 장치 목록을 가진 도메인, 아니면 NULL.
 *
 * (위 영어 주석 참고) 도메인 종류가 여럿이라 "이 도메인이 장치를 추적하는가"를
 * 한 곳에서 판단할 필요가 있다. 페이지 테이블 도메인과 SVA 도메인은 자기
 * 목록을 갖고, 중첩 도메인은 목록을 갖지 않고 부모 2단계 도메인의 것을
 * 쓴다 — 무효화도 결국 그 부모가 내기 때문이다.
 * 통과·중단 도메인처럼 무효화할 것이 없는 종류는 NULL 이다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare()/remove_master_domain() → [이 함수]
 */
static struct arm_smmu_domain *
to_smmu_domain_devices(struct iommu_domain *domain)
{
	/* The domain can be NULL only when processing the first attach */
	/* [한국어] (위 영어 주석 참고) 장치를 처음 붙일 때는 이전 도메인이 없다. */
	if (!domain)
		return NULL;
	if ((domain->type & __IOMMU_DOMAIN_PAGING) ||	/* [한국어] 페이지 테이블을 가진 도메인이거나. */
	    domain->type == IOMMU_DOMAIN_SVA)	/* [한국어] 프로세스 주소 공간을 빌려 쓰는 도메인이면. */
		return to_smmu_domain(domain);	/* [한국어] 자기 장치 목록을 갖는다. */
	if (domain->type == IOMMU_DOMAIN_NESTED)	/* [한국어] 중첩 도메인이면. */
		return to_smmu_nested_domain(domain)->vsmmu->s2_parent;	/* [한국어] 목록은 부모 2단계 도메인이 갖는다 — 무효화도 그쪽이 낸다. */
	return NULL;	/* [한국어] 통과·중단 도메인은 무효화할 것이 없다. */
}

/*
 * [한국어]
 * arm_smmu_enable_iopf - 그 장치의 폴트 처리 큐 등록을 켠다
 *
 * @master: 대상 장치.
 * @master_domain: 이번에 만드는 (장치, 도메인) 고리.
 * @return: 0 성공(필요 없는 경우도 성공), 음수 오류.
 *
 * 폴트를 받아 페이지를 채워 주려면 그 장치를 폴트 처리 큐에 등록해야 한다.
 * 멈춤을 쓰지 않는 장치는 폴트가 나도 답할 방법이 없으므로 등록하지 않고,
 * 그것을 실패로 보지도 않는다 — 그런 장치는 자기 방식으로 폴트를 다룬다.
 *
 * 참조 계수를 쓰는 이유는 한 장치에 여러 도메인(여러 PASID)이 붙을 수
 * 있기 때문이다. 마지막 하나가 떨어질 때 등록을 푼다.
 *
 * 스트림이 하나뿐인 장치만 지원하는 것은 폴트 기록에 스트림 번호가
 * 하나만 실리기 때문이다 — 여러 개면 어느 것인지 알 수 없다.
 *
 * 실행 컨텍스트: 붙이기 경로, group mutex 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare() → [이 함수] → iopf_queue_add_device()
 */
static int arm_smmu_enable_iopf(struct arm_smmu_master *master,
				struct arm_smmu_master_domain *master_domain)
{
	int ret;	/* [한국어] 등록 결과. */

	iommu_group_mutex_assert(master->dev);	/* [한국어] 참조 계수를 지키는 락을 쥐고 들어왔는지 확인한다. */

	if (!IS_ENABLED(CONFIG_ARM_SMMU_V3_SVA))	/* [한국어] SVA 를 끄고 빌드했다면 폴트 처리 계층 자체가 없다. */
		return -EOPNOTSUPP;

	/*
	 * Drivers for devices supporting PRI or stall require iopf others have
	 * device-specific fault handlers and don't need IOPF, so this is not a
	 * failure.
	 */
	/* [한국어] (위 영어 주석 참고) 멈춤을 쓰지 않는 장치는 폴트가 나도 답할
	 * 방법이 없어 등록하지 않는다. 그런 장치는 자기 드라이버가 폴트를
	 * 다루므로 이것을 실패로 보면 안 된다. */
	if (!master->stall_enabled)
		return 0;	/* [한국어] 등록하지 않고 성공으로 친다. */

	/* We're not keeping track of SIDs in fault events */
	/* [한국어] (위 영어 주석 참고) 폴트를 처리 계층에 올릴 때 스트림 번호를
	 * 함께 전달하지 않는다. 그래서 스트림이 여럿인 장치는 어느 것에서 난
	 * 폴트인지 되짚을 수 없어 지원하지 않는다. */
	if (master->num_streams != 1)
		return -EOPNOTSUPP;

	if (master->iopf_refcount) {	/* [한국어] 이미 등록되어 있으면. */
		master->iopf_refcount++;	/* [한국어] 참조만 올린다. */
		master_domain->using_iopf = true;	/* [한국어] 이 고리가 그 참조를 쥐고 있음을 기록한다 — 뗄 때 이 값으로 판단한다. */
		return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
	}

	ret = iopf_queue_add_device(master->smmu->evtq.iopf, master->dev);	/* [한국어] 폴트 처리 큐에 이 장치를 등록한다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;
	master->iopf_refcount = 1;	/* [한국어] 첫 참조. */
	master_domain->using_iopf = true;	/* [한국어] 이 고리가 그 참조의 주인이다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_disable_iopf - 그 장치의 폴트 처리 큐 등록을 되돌린다
 *
 * @master: 대상 장치.
 * @master_domain: 떼어 내는 고리 (NULL 일 수 있다).
 *
 * 참조를 하나 내리고, 0 이 되면 실제로 등록을 푼다. 고리가 그 참조를
 * 쥐고 있지 않았다면 — 멈춤을 쓰지 않아 애초에 등록하지 않았다면 —
 * 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 떼기 경로, group mutex 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_remove_master_domain() → [이 함수]
 *     → iopf_queue_remove_device()
 */
static void arm_smmu_disable_iopf(struct arm_smmu_master *master,
				  struct arm_smmu_master_domain *master_domain)
{
	iommu_group_mutex_assert(master->dev);	/* [한국어] 참조 계수를 지키는 락. */

	if (!IS_ENABLED(CONFIG_ARM_SMMU_V3_SVA))	/* [한국어] 끄고 빌드했으면 등록한 적도 없다. */
		return;

	if (!master_domain || !master_domain->using_iopf)	/* [한국어] 고리가 없거나 참조를 쥐지 않았다면. */
		return;	/* [한국어] 되돌릴 것이 없다. */

	master->iopf_refcount--;	/* [한국어] 참조를 내린다. */
	if (master->iopf_refcount == 0)	/* [한국어] 마지막 참조였다면. */
		iopf_queue_remove_device(master->smmu->evtq.iopf, master->dev);	/* [한국어] 실제로 등록을 푼다 — 이 뒤로 이 장치의 폴트는 처리되지 않는다. */
}

/*
 * [한국어]
 * arm_smmu_master_build_inv - 무효화 항목 하나를 임시 배열에 담는다
 *
 * @master: 대상 장치 (임시 배열을 들고 있다).
 * @type: 무효화 종류.
 * @id: 대상 번호 (ASID/VMID/스트림).
 * @ssid: PASID (장치 캐시 무효화에서만 뜻이 있다).
 * @pgsize: 알갱이 크기의 로그 (범위 무효화 계산에 쓴다).
 * @return: 담긴 항목, 자리가 없으면 NULL.
 *
 * 항목의 종류에 따라 어떤 명령을 쓸지 미리 정해 담는 것이 요점이다.
 * 무효화를 실제로 낼 때는 원자적 문맥일 수 있어 조건 판단을 최소로
 * 줄여야 하는데, 그 판단을 여기 붙이기 시점으로 옮겨 둔 것이다.
 *
 * 종류마다 명령이 둘씩인 이유는 구간 무효화와 전체 무효화를 상황에 따라
 * 골라 쓰기 때문이다 — size_opcode 와 nsize_opcode 가 그 짝이다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_master_build_invs() → [이 함수]
 */
static struct arm_smmu_inv *
arm_smmu_master_build_inv(struct arm_smmu_master *master,
			  enum arm_smmu_inv_type type, u32 id, ioasid_t ssid,
			  size_t pgsize)
{
	struct arm_smmu_invs *build_invs = master->build_invs;	/* [한국어] 장치마다 하나씩 미리 잡아 둔 임시 배열. */
	struct arm_smmu_inv *cur, inv = {	/* [한국어] 담을 자리와, 채워 넣을 값. */
		.smmu = master->smmu,	/* [한국어] 어느 하드웨어의 명령인지 — 배열 정렬의 첫 기준이다. */
		.type = type,	/* [한국어] 무효화 종류. */
		.id = id,	/* [한국어] 대상 번호. */
		.pgsize = pgsize,	/* [한국어] 범위 무효화 계산에 쓸 알갱이 크기. */
	};

	if (WARN_ON(build_invs->num_invs >= build_invs->max_invs))	/* [한국어] 임시 배열은 최악의 경우를 계산해 미리 잡아 두었다 — 넘치면 그 계산이 틀린 것이다. */
		return NULL;
	cur = &build_invs->inv[build_invs->num_invs];	/* [한국어] 다음 빈 자리. */
	build_invs->num_invs++;

	*cur = inv;	/* [한국어] 공통 필드를 먼저 채운다. */
	switch (type) {	/* [한국어] 종류에 따라 쓸 명령을 미리 정해 둔다. */
	case INV_TYPE_S1_ASID:	/* [한국어] 1단계 주소 공간 무효화. */
		/*
		 * For S1 page tables the driver always uses VMID=0, and the
		 * invalidation logic for this type will set it as well.
		 */
		/* [한국어] (위 영어 주석 참고) 1단계만 하는 경우 VMID 는 언제나 0 이라
		 * 여기서 따로 담지 않고, 명령을 지을 때 0 이 들어간다. */
		if (master->smmu->features & ARM_SMMU_FEAT_E2H) {	/* [한국어] CPU 가 가상화 호스트 확장으로 도는 시스템이면. */
			cur->size_opcode = CMDQ_OP_TLBI_EL2_VA;	/* [한국어] EL2 세계의 주소 무효화. */
			cur->nsize_opcode = CMDQ_OP_TLBI_EL2_ASID;	/* [한국어] 그 세계의 ASID 전체 무효화. */
		} else {	/* [한국어] 아니면 비보안 EL1 세계. */
			cur->size_opcode = CMDQ_OP_TLBI_NH_VA;	/* [한국어] 주소 무효화. */
			cur->nsize_opcode = CMDQ_OP_TLBI_NH_ASID;	/* [한국어] ASID 전체 무효화. */
		}
		break;
	case INV_TYPE_S2_VMID:	/* [한국어] 2단계 무효화. */
		cur->size_opcode = CMDQ_OP_TLBI_S2_IPA;	/* [한국어] 중간 물리 주소로 지운다. */
		cur->nsize_opcode = CMDQ_OP_TLBI_S12_VMALL;	/* [한국어] 전체는 1·2단계를 함께 지운다 — 그래서 아래 S1_CLEAR 이 생략될 수 있다. */
		break;
	case INV_TYPE_S2_VMID_S1_CLEAR:	/* [한국어] 중첩에서 그 VMID 아래 1단계까지 지우는 항목. */
		cur->size_opcode = cur->nsize_opcode = CMDQ_OP_TLBI_NH_ALL;	/* [한국어] 주소를 가리지 않는 명령이라 두 경우가 같다. */
		break;
	case INV_TYPE_ATS:	/* [한국어] 장치 캐시의 구간 무효화. */
	case INV_TYPE_ATS_FULL:	/* [한국어] 장치 캐시 전체 무효화. */
		cur->size_opcode = cur->nsize_opcode = CMDQ_OP_ATC_INV;	/* [한국어] 명령은 같고, 인자로 범위를 가른다. */
		cur->ssid = ssid;	/* [한국어] 이 종류에서만 PASID 가 뜻을 가진다 — 정렬 비교도 그렇게 되어 있다. */
		break;
	}

	return cur;	/* [한국어] 담은 항목을 호출자에게 돌려준다. */
}

/*
 * Use the preallocated scratch array at master->build_invs, to build a to_merge
 * or to_unref array, to pass into a following arm_smmu_invs_merge/unref() call.
 *
 * Do not free the returned invs array. It is reused, and will be overwritten by
 * the next arm_smmu_master_build_invs() call.
 */
/*
 * [한국어]
 * arm_smmu_master_build_invs - 이 장치가 필요로 하는 무효화 목록을 짓는다
 *
 * @master: 대상 장치.
 * @ats_enabled: 장치 캐시 무효화도 필요한가.
 * @ssid: 붙이는 PASID.
 * @smmu_domain: 붙일 도메인.
 * @return: 지어진 임시 배열, 실패하면 NULL.
 *
 * (위 영어 주석 참고) 장치를 도메인에 붙이거나 뗄 때, "이 장치 때문에
 * 무효화해야 하는 대상들"을 목록으로 만든다. 그 목록을 도메인의 배열에
 * 합치면 붙이기가 되고, 빼면 떼기가 된다.
 *
 * 미리 잡아 둔 배열을 재사용하는 것이 요점이다. 떼기 경로는 실패하면 안
 * 되는데 메모리를 잡으면 실패할 수 있으므로, 장치를 프로브할 때 최악의
 * 크기로 한 번 잡아 두고 계속 덮어쓴다.
 *
 * 담는 순서가 곧 정렬 순서라는 점도 중요하다. 마지막 주석이 그것을
 * 못 박고 있으며, 이 순서가 어긋나면 병합·해제가 모두 어긋난다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로, group mutex 아래. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare() → [이 함수] → arm_smmu_master_build_inv()
 */
static struct arm_smmu_invs *
arm_smmu_master_build_invs(struct arm_smmu_master *master, bool ats_enabled,
			   ioasid_t ssid, struct arm_smmu_domain *smmu_domain)
{
	const bool nesting = smmu_domain->nest_parent;	/* [한국어] 이 도메인이 중첩의 부모로 쓰이는가. */
	size_t pgsize = 0, i;	/* [한국어] 알갱이 크기의 로그와 반복자. */

	iommu_group_mutex_assert(master->dev);	/* [한국어] 임시 배열을 재사용하므로, 두 경로가 동시에 쓰면 안 된다. */

	master->build_invs->num_invs = 0;	/* [한국어] 앞선 내용을 버리고 처음부터 담는다. */

	/* Range-based invalidation requires the leaf pgsize for calculation */
	/* [한국어] (위 영어 주석 참고) 범위 무효화는 알갱이 크기를 알아야 몇 개를
	 * 한 번에 지울지 계산할 수 있다. 지원하지 않으면 0 으로 둔다. */
	if (master->smmu->features & ARM_SMMU_FEAT_RANGE_INV)
		pgsize = __ffs(smmu_domain->domain.pgsize_bitmap);	/* [한국어] 가장 작은 페이지 크기의 로그. */

	switch (smmu_domain->stage) {	/* [한국어] 도메인 갈래에 따라 기본 항목이 다르다. */
	case ARM_SMMU_DOMAIN_SVA:	/* [한국어] SVA 도 1단계와 같은 방식으로 무효화한다. */
	case ARM_SMMU_DOMAIN_S1:	/* [한국어] 이 값일 때의 처리. */
		if (!arm_smmu_master_build_inv(master, INV_TYPE_S1_ASID,	/* [한국어] 그 ASID 를 지우는 항목. */
					       smmu_domain->cd.asid,
					       IOMMU_NO_PASID, pgsize))
			return NULL;
		break;
	case ARM_SMMU_DOMAIN_S2:	/* [한국어] 2단계 도메인. */
		if (!arm_smmu_master_build_inv(master, INV_TYPE_S2_VMID,	/* [한국어] 그 VMID 를 지우는 항목. */
					       smmu_domain->s2_cfg.vmid,
					       IOMMU_NO_PASID, pgsize))
			return NULL;
		break;
	default:	/* [한국어] 갈래가 없는 도메인은 여기 오면 안 된다. */
		WARN_ON(true);	/* [한국어] 있어서는 안 될 상태 — 흔적을 남긴다. */
		return NULL;	/* [한국어] 돌려줄 것이 없다. */
	}

	/* All the nested S1 ASIDs have to be flushed when S2 parent changes */
	/* [한국어] (위 영어 주석 참고) 게스트가 지은 1단계 변환은 그 아래 2단계
	 * 매핑을 전제로 캐시되어 있다. 2단계가 바뀌면 그 전제가 깨지므로,
	 * 게스트의 ASID 들까지 모두 지워야 한다. 게스트가 어떤 ASID 를 쓰는지
	 * 호스트는 모르므로 VMID 아래를 통째로 지운다. */
	if (nesting) {	/* [한국어] 이 도메인이 중첩의 부모라면. */
		if (!arm_smmu_master_build_inv(	/* [한국어] 담을 자리가 없으면 임시 배열 크기 계산이 틀린 것이다. */
			    master, INV_TYPE_S2_VMID_S1_CLEAR,
			    smmu_domain->s2_cfg.vmid, IOMMU_NO_PASID, 0))
			return NULL;
	}

	for (i = 0; ats_enabled && i < master->num_streams; i++) {	/* [한국어] ATS 를 켠 경우에만, 스트림 번호마다 하나씩. */
		/*
		 * If an S2 used as a nesting parent is changed we have no
		 * option but to completely flush the ATC.
		 */
		/* [한국어] (위 영어 주석 참고) 중첩에서는 게스트가 어떤 PASID 로 어떤
		 * 주소를 캐시했는지 호스트가 알 수 없다. 그래서 구간을 좁혀 지울
		 * 방법이 없고 장치 캐시를 통째로 비우는 수밖에 없다. */
		if (!arm_smmu_master_build_inv(
			    master, nesting ? INV_TYPE_ATS_FULL : INV_TYPE_ATS,	/* [한국어] 중첩이면 전체, 아니면 구간 무효화. */
			    master->streams[i].id, ssid, 0))
			return NULL;
	}

	/* Note this build_invs must have been sorted */
	/* [한국어] (위 영어 주석 참고) 담은 순서가 곧 정렬 순서여야 한다 —
	 * 하드웨어, 종류, 번호, PASID 순으로 담기게끔 위 코드가 짜여 있다.
	 * 이 순서가 어긋나면 병합·해제가 모두 어긋난다. */

	return master->build_invs;	/* [한국어] 호출자가 곧바로 병합·해제에 넘긴다. 놓지는 않는다 — 재사용되는 배열이다. */
}

/*
 * [한국어]
 * arm_smmu_remove_master_domain - 도메인과 장치를 잇던 고리를 끊는다
 *
 * @master: 대상 장치.
 * @domain: 떠나는 도메인.
 * @ssid: 그 PASID.
 *
 * 목록에서 고리를 빼고, 폴트 처리 등록을 되돌리고, 고리를 놓는다.
 * ATS 를 쓰던 짝이었으면 도메인의 ATS 장치 수도 하나 줄인다 — 그 값이
 * 0 인지 여부로 무효화 경로가 빠른 길을 고르기 때문이다.
 *
 * 폴트 등록 해제를 락 밖에서 하는 것이 중요하다. 그 안에서 잠들 수 있어
 * 스핀락을 쥔 채로는 부를 수 없다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 스핀락을 잡았다 놓은 뒤 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_commit() 등 → [이 함수]
 *     → arm_smmu_find_master_domain() → arm_smmu_disable_iopf()
 */
static void arm_smmu_remove_master_domain(struct arm_smmu_master *master,
					  struct iommu_domain *domain,
					  ioasid_t ssid)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain_devices(domain);	/* [한국어] 장치 목록을 가진 도메인을 찾는다. */
	struct arm_smmu_master_domain *master_domain;	/* [한국어] 끊을 고리. */
	bool nested_ats_flush = false;	/* [한국어] 중첩 경로의 무효화 성격 — 고리를 특정하는 데 필요하다. */
	unsigned long flags;	/* [한국어] 인터럽트 상태. */

	if (!smmu_domain)	/* [한국어] 장치를 추적하지 않는 도메인이면. */
		return;	/* [한국어] 끊을 고리가 없다. */

	if (domain->type == IOMMU_DOMAIN_NESTED)	/* [한국어] 중첩 도메인이면. */
		nested_ats_flush = to_smmu_nested_domain(domain)->enable_ats;	/* [한국어] 붙일 때 쓴 값과 같아야 고리를 찾을 수 있다. */

	spin_lock_irqsave(&smmu_domain->devices_lock, flags);	/* [한국어] 목록을 고치는 동안 막는다. */
	master_domain = arm_smmu_find_master_domain(smmu_domain, domain, master,	/* [한국어] 네 값이 모두 맞는 고리를 찾는다. */
						    ssid, nested_ats_flush);
	if (master_domain) {	/* [한국어] 찾았다면. */
		list_del(&master_domain->devices_elm);	/* [한국어] 목록에서 뺀다 — 이 뒤로 무효화 경로가 이 짝을 보지 않는다. */
		if (master->ats_enabled)	/* [한국어] ATS 를 쓰던 장치였다면. */
			atomic_dec(&smmu_domain->nr_ats_masters);	/* [한국어] 세어 둔 수를 줄인다 — 0 이면 무효화가 빠른 길로 간다. */
	}
	spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);	/* [한국어] 목록 조작이 끝났으니 놓는다. */

	arm_smmu_disable_iopf(master, master_domain);	/* [한국어] 락 밖에서 부른다 — 안에서 잠들 수 있기 때문이다. */
	kfree(master_domain);	/* [한국어] 고리를 놓는다. NULL 이어도 안전하다. */
}

/*
 * During attachment, the updates of the two domain->invs arrays are sequenced:
 *  1. new domain updates its invs array, merging master->build_invs
 *  2. new domain starts to include the master during its invalidation
 *  3. master updates its STE switching from the old domain to the new domain
 *  4. old domain still includes the master during its invalidation
 *  5. old domain updates its invs array, unreferencing master->build_invs
 *
 * For 1 and 5, prepare the two updated arrays in advance, handling any changes
 * that can possibly failure. So the actual update of either 1 or 5 won't fail.
 * arm_smmu_asid_lock ensures that the old invs in the domains are intact while
 * we are sequencing to update them.
 */
/*
 * [한국어]
 * arm_smmu_attach_prepare_invs - 두 도메인의 무효화 배열 교체를 미리 준비한다
 *
 * @state: 붙이기 상태 묶음.
 * @new_domain: 붙일 도메인.
 * @return: 0 성공, 음수면 아무것도 바뀌지 않은 채 실패.
 *
 * (위 영어 주석의 5단계 순서 참고) 붙이기에서 가장 미묘한 대목이다.
 * 장치가 옛 도메인에서 새 도메인으로 옮겨 가는 동안, 어느 순간에도
 * "무효화를 받을 곳이 없는" 상태가 있어서는 안 된다.
 *
 * 그래서 순서를 이렇게 잡는다 — 새 도메인에 먼저 이 장치를 더하고(1),
 * 그다음 STE 를 바꾸고(3), 마지막에 옛 도메인에서 뺀다(5). 잠깐 동안
 * 두 도메인이 모두 이 장치를 무효화 대상으로 삼지만, 그 낭비는 안전을
 * 위해 감수한다. 반대 순서였다면 그 사이 무효화가 이 장치를 건너뛴다.
 *
 * 그리고 1 과 5 는 절대 실패하면 안 된다 — 이미 하드웨어가 바뀐 뒤이기
 * 때문이다. 그래서 실패할 수 있는 일(새 배열 만들기)을 모두 이 함수로
 * 옮겨 두고, 실제 교체는 포인터만 바꾸는 일로 만든다.
 *
 * 실행 컨텍스트: 붙이기 경로, arm_smmu_asid_lock 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare() → [이 함수]
 *     → arm_smmu_master_build_invs() → arm_smmu_invs_merge()
 */
static int arm_smmu_attach_prepare_invs(struct arm_smmu_attach_state *state,
					struct iommu_domain *new_domain)
{
	struct arm_smmu_domain *old_smmu_domain =	/* [한국어] 떠나는 도메인 (장치 목록을 가진 것). */
		to_smmu_domain_devices(state->old_domain);
	struct arm_smmu_domain *new_smmu_domain =	/* [한국어] 들어가는 도메인. */
		to_smmu_domain_devices(new_domain);
	struct arm_smmu_master *master = state->master;	/* [한국어] 옮겨 가는 장치. */
	ioasid_t ssid = state->ssid;	/* [한국어] 그 PASID. */

	/*
	 * At this point a NULL domain indicates the domain doesn't use the
	 * IOTLB, see to_smmu_domain_devices().
	 */
	/* [한국어] (위 영어 주석 참고) 통과·중단 도메인은 무효화할 것이 없어
	 * NULL 이 온다. 그때는 그 쪽 준비를 건너뛴다. */
	if (new_smmu_domain) {	/* [한국어] 새 도메인이 무효화를 추적한다면. */
		struct arm_smmu_inv_state *invst = &state->new_domain_invst;	/* [한국어] 그 교체 상태를 담을 자리. */
		struct arm_smmu_invs *build_invs;	/* [한국어] 이 장치가 필요로 하는 무효화 목록. */

		invst->invs_ptr = &new_smmu_domain->invs;	/* [한국어] commit 이 바꿔 끼울 자리. */
		invst->old_invs = rcu_dereference_protected(	/* [한국어] 지금 걸려 있는 배열 — 나중에 놓아야 한다. */
			new_smmu_domain->invs,
			lockdep_is_held(&arm_smmu_asid_lock));	/* [한국어] 이 락이 그 사이 아무도 바꾸지 않음을 보장한다. */
		build_invs = arm_smmu_master_build_invs(	/* [한국어] 이 장치가 더할 항목들을 짓는다. */
			master, state->ats_enabled, ssid, new_smmu_domain);
		if (!build_invs)	/* [한국어] 지을 수 없으면. */
			return -EINVAL;

		invst->new_invs =	/* [한국어] 합친 새 배열을 미리 만든다 — 실패할 수 있는 일은 여기서 끝난다. */
			arm_smmu_invs_merge(invst->old_invs, build_invs);
		if (IS_ERR(invst->new_invs))	/* [한국어] 메모리가 없으면. */
			return PTR_ERR(invst->new_invs);	/* [한국어] 아직 하드웨어를 건드리기 전이라 그냥 접으면 된다. */
	}

	if (old_smmu_domain) {	/* [한국어] 옛 도메인도 무효화를 추적한다면. */
		struct arm_smmu_inv_state *invst = &state->old_domain_invst;	/* [한국어] 그 쪽 교체 상태. */

		invst->invs_ptr = &old_smmu_domain->invs;	/* [한국어] commit 이 손댈 자리. */
		/* A re-attach case might have a different ats_enabled state */
		/* [한국어] (위 영어 주석 참고) 같은 도메인에 다시 붙이는 경우 — 예컨대
		 * ATS 만 켜고 끄는 경우 — 새 배열이 이미 만들어져 있다. 그것을
		 * 기준으로 삼아야 항목이 어긋나지 않는다. */
		if (new_smmu_domain == old_smmu_domain)
			invst->old_invs = state->new_domain_invst.new_invs;	/* [한국어] 방금 만든 배열이 곧 옛 배열이 된다. */
		else
			invst->old_invs = rcu_dereference_protected(	/* [한국어] 다른 도메인이면 지금 걸린 것을 그대로. */
				old_smmu_domain->invs,
				lockdep_is_held(&arm_smmu_asid_lock));
		/* For old_smmu_domain, new_invs points to master->build_invs */
		/* [한국어] (위 영어 주석 참고) 옛 도메인 쪽에서는 이 필드가 "새 배열"이
		 * 아니라 "빼낼 항목 목록"이라는 뜻으로 쓰인다. 한 필드를 두 뜻으로
		 * 겸하는 대신, 해제가 메모리를 잡지 않아도 되게 만든다. */
		invst->new_invs = arm_smmu_master_build_invs(	/* [한국어] 이 장치가 빼낼 항목들 — 붙일 때 쓴 ATS 상태를 그대로 써야 짝이 맞는다. */
			master, master->ats_enabled, ssid, old_smmu_domain);
	}

	return 0;	/* [한국어] 실패할 수 있는 일이 모두 끝났다 — 이 뒤로는 접을 필요가 없다. */
}

/* Must be installed before arm_smmu_install_ste_for_dev() */
/*
 * [한국어]
 * arm_smmu_install_new_domain_invs - 새 도메인의 무효화 배열을 갈아 끼운다
 *
 * @state: prepare 가 채워 둔 상태.
 *
 * (위 영어 주석 참고) STE 를 쓰기 전에 반드시 해야 한다. 순서가 뒤집히면
 * 하드웨어가 이미 새 도메인으로 변환하는데 그 도메인의 무효화가 이 장치를
 * 건너뛰는 창이 생긴다.
 *
 * 포인터 하나를 바꾸고 옛 배열을 RCU 로 놓는 것이 전부다 — 실패할 일이
 * 없어야 하므로 그렇게 만들어 두었다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_prepare() → [이 함수] → rcu_assign_pointer()
 */
static void
arm_smmu_install_new_domain_invs(struct arm_smmu_attach_state *state)
{
	struct arm_smmu_inv_state *invst = &state->new_domain_invst;	/* [한국어] 새 도메인 쪽 교체 상태. */

	if (!invst->invs_ptr)	/* [한국어] 무효화를 추적하지 않는 도메인이면. */
		return;	/* [한국어] 할 일이 없다. */

	rcu_assign_pointer(*invst->invs_ptr, invst->new_invs);	/* [한국어] 미리 만들어 둔 배열로 갈아 끼운다 — 읽는 쪽은 둘 중 하나를 온전히 본다. */
	kfree_rcu(invst->old_invs, rcu);	/* [한국어] 옛 배열은 유예 기간이 지난 뒤 놓인다 — 지금 읽고 있는 쪽이 있을 수 있다. */
}

/*
 * [한국어]
 * arm_smmu_inv_flush_iotlb_tag - 더 이상 쓰이지 않는 태그를 캐시에서 지운다
 *
 * @inv: 그 태그를 담은 무효화 항목.
 *
 * ASID 나 VMID 가 어느 배열에도 남지 않게 되면, 그 번호로 태그된 TLB
 * 항목을 지금 지워야 한다. 그러지 않으면 나중에 그 번호를 넘겨받은
 * 도메인이 앞 도메인의 낡은 변환을 그대로 쓰게 된다.
 *
 * 그 규칙을 아래 호출부의 주석이 한 줄로 못 박고 있다 — "배열에 없는
 * ASID/VMID 는 TLB 에서 비어 있어야 한다".
 *
 * 실행 컨텍스트: 떼기 경로(commit 단계). 완료를 기다린다.
 *
 * 호출 체인:
 *   arm_smmu_install_old_domain_invs() → [이 함수]
 *     → arm_smmu_cmdq_issue_cmd_with_sync()
 */
static void arm_smmu_inv_flush_iotlb_tag(struct arm_smmu_inv *inv)
{
	struct arm_smmu_cmdq_ent cmd = {};	/* [한국어] 지을 명령. */

	switch (inv->type) {	/* [한국어] 태그의 종류에 따라 필드가 다르다. */
	case INV_TYPE_S1_ASID:	/* [한국어] 1단계 주소 공간. */
		cmd.tlbi.asid = inv->id;	/* [한국어] 그 ASID 를 지운다. */
		break;
	case INV_TYPE_S2_VMID:	/* [한국어] 2단계 가상 기계. */
		/* S2_VMID using nsize_opcode covers S2_VMID_S1_CLEAR */
		/* [한국어] (위 영어 주석 참고) 이 종류의 "전체" 명령은 1·2단계를 함께
		 * 지우므로, 중첩용 항목을 따로 처리할 필요가 없다. */
		cmd.tlbi.vmid = inv->id;
		break;
	default:	/* [한국어] 장치 캐시 항목 등은 태그 개념이 없다. */
		return;	/* [한국어] 지울 것이 없다. */
	}

	cmd.opcode = inv->nsize_opcode;	/* [한국어] 주소를 가리지 않는 "전체" 명령을 쓴다. */
	arm_smmu_cmdq_issue_cmd_with_sync(inv->smmu, &cmd);	/* [한국어] 완료까지 기다린다 — 번호를 돌려주기 전에 정말 비었음을 확인해야 한다. */
}

/* Should be installed after arm_smmu_install_ste_for_dev() */
/*
 * [한국어]
 * arm_smmu_install_old_domain_invs - 옛 도메인에서 이 장치의 항목을 걷어 낸다
 *
 * @state: prepare 가 채워 둔 상태.
 *
 * (위 영어 주석 참고) STE 를 쓴 뒤에 해야 한다. 먼저 걷어 내면 하드웨어가
 * 아직 옛 도메인으로 변환하는데 그 도메인의 무효화가 이 장치를 건너뛴다.
 *
 * 세 걸음이다 — 참조를 내리고, 아무도 쓰지 않게 된 태그를 캐시에서 지우고,
 * 버려진 자리가 쌓였으면 정리한다. 앞의 둘은 실패할 수 없고, 정리는
 * 실패해도 다음 기회로 미루면 되므로 이 함수 전체가 실패하지 않는다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로, arm_smmu_asid_lock 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_commit() → [이 함수]
 *     → arm_smmu_invs_unref() → arm_smmu_invs_purge()
 */
static void
arm_smmu_install_old_domain_invs(struct arm_smmu_attach_state *state)
{
	struct arm_smmu_inv_state *invst = &state->old_domain_invst;	/* [한국어] 옛 도메인 쪽 교체 상태. */
	struct arm_smmu_invs *old_invs = invst->old_invs;	/* [한국어] 지금 걸려 있는 배열. */
	struct arm_smmu_invs *new_invs;	/* [한국어] 정리한 새 배열 (필요할 때만). */

	lockdep_assert_held(&arm_smmu_asid_lock);	/* [한국어] 교체 순서를 지키는 락을 쥐고 들어왔는지 확인한다. */

	if (!invst->invs_ptr)	/* [한국어] 무효화를 추적하지 않는 도메인이면. */
		return;

	arm_smmu_invs_unref(old_invs, invst->new_invs);	/* [한국어] 제자리에서 참조를 내린다 — 실패하지 않는다. */
	/*
	 * When an IOTLB tag (the first entry in invs->new_invs) is no longer used,
	 * it means the ASID or VMID will no longer be invalidated by map/unmap and
	 * must be cleaned right now. The rule is that any ASID/VMID not in an invs
	 * array must be left cleared in the IOTLB.
	 */
	/* [한국어] (위 영어 주석 참고) 목록의 첫 항목이 늘 그 도메인의 태그이고,
	 * 그 참조가 0 이 되었다면 이 장치가 마지막이었다는 뜻이다. 이제 그
	 * 번호로는 아무도 무효화를 내지 않으므로, 남아 있는 캐시를 지금 지워야
	 * 한다. "배열에 없는 태그는 TLB 에서 비어 있어야 한다"가 그 규칙이다. */
	if (!READ_ONCE(invst->new_invs->inv[0].users))	/* [한국어] unref 가 0 으로 표시해 돌려준 값. */
		arm_smmu_inv_flush_iotlb_tag(&invst->new_invs->inv[0]);	/* [한국어] 그 태그를 캐시에서 지운다. */

	new_invs = arm_smmu_invs_purge(old_invs);	/* [한국어] 버려진 자리가 쌓였으면 걷어 낸 새 배열을 만든다. */
	if (!new_invs)	/* [한국어] 정리할 것이 없거나 메모리가 없으면. */
		return;	/* [한국어] 그대로 둔다 — 정확성에는 영향이 없다. */

	rcu_assign_pointer(*invst->invs_ptr, new_invs);	/* [한국어] 정리된 배열로 갈아 끼운다. */
	kfree_rcu(old_invs, rcu);	/* [한국어] 옛 배열은 유예 기간 뒤에 놓는다. */
}

/*
 * Start the sequence to attach a domain to a master. The sequence contains three
 * steps:
 *  arm_smmu_attach_prepare()
 *  arm_smmu_install_ste_for_dev()
 *  arm_smmu_attach_commit()
 *
 * If prepare succeeds then the sequence must be completed. The STE installed
 * must set the STE.EATS field according to state.ats_enabled.
 *
 * If the device supports ATS then this determines if EATS should be enabled
 * in the STE, and starts sequencing EATS disable if required.
 *
 * The change of the EATS in the STE and the PCI ATS config space is managed by
 * this sequence to be in the right order so that if PCI ATS is enabled then
 * STE.ETAS is enabled.
 *
 * new_domain can be a non-paging domain. In this case ATS will not be enabled,
 * and invalidations won't be tracked.
 */
/*
 * [한국어]
 * arm_smmu_attach_prepare - 붙이기에서 실패할 수 있는 일을 모두 끝낸다
 *
 * @state: 붙이기 상태 묶음. 입력 필드는 채워져 있어야 하고, 결과가 채워진다.
 * @new_domain: 붙일 도메인.
 * @return: 0 성공, 음수면 아무것도 바뀌지 않은 채 실패.
 *
 * (위 영어 주석 참고) 붙이기 3단계 규약의 첫 걸음이다. 이 함수가 성공하면
 * 호출자는 반드시 나머지 두 걸음(STE 쓰기, commit)을 끝내야 한다 — 중간에
 * 그만두면 커널과 하드웨어의 상태가 어긋난다.
 *
 * 하는 일은 넷이다. ATS 를 켤지 정하고, 두 도메인의 무효화 배열 교체를
 * 준비하고, (장치, 도메인) 고리를 만들어 목록에 걸고, ATS 를 꺼야 한다면
 * 지금 꺼 둔다.
 *
 * 고리를 STE 를 바꾸기 전에 목록에 거는 것이 중요하다 — 주석이 설명하듯,
 * 그래야 옛 도메인과 새 도메인이 모두 이 장치에 무효화를 보내는 겹치는
 * 구간이 생기고, 어느 순간에도 무효화가 이 장치를 건너뛰지 않는다.
 *
 * ATS 를 끄는 일도 여기서 하는데, PCI 설정을 먼저 끄고 STE 를 나중에 바꿔야
 * 오류 보고가 시끄러워지지 않기 때문이다.
 *
 * 실행 컨텍스트: 붙이기 경로, arm_smmu_asid_lock 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev()/set_pasid() → [이 함수]
 *     → arm_smmu_attach_prepare_invs() → arm_smmu_install_new_domain_invs()
 */
int arm_smmu_attach_prepare(struct arm_smmu_attach_state *state,
			    struct iommu_domain *new_domain)
{
	struct arm_smmu_master *master = state->master;	/* [한국어] 붙일 장치. */
	struct arm_smmu_master_domain *master_domain;	/* [한국어] 만들 (장치, 도메인) 고리. */
	struct arm_smmu_domain *smmu_domain =	/* [한국어] 장치 목록을 가진 도메인 (없을 수 있다). */
		to_smmu_domain_devices(new_domain);
	unsigned long flags;	/* [한국어] 인터럽트 상태. */
	int ret;	/* [한국어] 중간 결과. */

	/*
	 * arm_smmu_share_asid() must not see two domains pointing to the same
	 * arm_smmu_master_domain contents otherwise it could randomly write one
	 * or the other to the CD.
	 */
	/* [한국어] (위 영어 주석 참고) 이 락이 붙이기 전체를 감싸므로, 그동안
	 * STE 와 장치 목록이 서로 어긋나 있어도 아무도 그 어긋남을 보지 못한다. */
	lockdep_assert_held(&arm_smmu_asid_lock);

	if (smmu_domain || state->cd_needs_ats) {	/* [한국어] 무효화를 추적하는 도메인이거나 서술자 쪽 사정으로 ATS 가 필요하면. */
		/*
		 * The SMMU does not support enabling ATS with bypass/abort.
		 * When the STE is in bypass (STE.Config[2:0] == 0b100), ATS
		 * Translation Requests and Translated transactions are denied
		 * as though ATS is disabled for the stream (STE.EATS == 0b00),
		 * causing F_BAD_ATS_TREQ and F_TRANSL_FORBIDDEN events
		 * (IHI0070Ea 5.2 Stream Table Entry).
		 *
		 * However, if we have installed a CD table and are using S1DSS
		 * then ATS will work in S1DSS bypass. See "13.6.4 Full ATS
		 * skipping stage 1".
		 *
		 * Disable ATS if we are going to create a normal 0b100 bypass
		 * STE.
		 */
		/* [한국어] (위 영어 주석 참고) 통과·중단 STE 에서는 ATS 요청이 거부되고
		 * 오류 이벤트만 쏟아진다. 그래서 그런 STE 를 심을 때는 ATS 를 켜지
		 * 않는다. 다만 문맥 표를 가진 채 s1dss 로 통과시키는 경우는 다르다 —
		 * 그때는 ATS 가 정상 동작하므로 이 조건 안에 들어온다. */
		state->ats_enabled = !state->disable_ats &&	/* [한국어] 상위가 끄라고 했으면 끈다. */
				     arm_smmu_ats_supported(master);	/* [한국어] 그리고 하드웨어·펌웨어·장치가 모두 받쳐 줘야 한다. */
	}

	ret = arm_smmu_attach_prepare_invs(state, new_domain);	/* [한국어] 두 도메인의 배열 교체를 미리 준비한다 — 실패할 수 있는 일이다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;	/* [한국어] 아직 아무것도 바뀌지 않았다. */

	if (smmu_domain) {	/* [한국어] 무효화를 추적하는 도메인이라면 고리를 만들어야 한다. */
		if (new_domain->type == IOMMU_DOMAIN_NESTED) {	/* [한국어] 중첩 도메인이면. */
			ret = arm_smmu_attach_prepare_vmaster(	/* [한국어] 게스트에게 사건을 돌려줄 다리도 미리 잡는다. */
				state, to_smmu_nested_domain(new_domain));
			if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
				goto err_unprepare_invs;	/* [한국어] 앞서 만든 배열부터 되돌린다. */
		}

		master_domain = kzalloc_obj(*master_domain);	/* [한국어] 도메인과 장치를 잇는 고리. */
		if (!master_domain) {	/* [한국어] 고리를 못 잡았으면. */
			ret = -ENOMEM;	/* [한국어] 메모리 부족으로 붙이기를 접는다. */
			goto err_free_vmaster;	/* [한국어] 게스트 다리부터 되돌린다. */
		}
		master_domain->domain = new_domain;	/* [한국어] 코어가 아는 도메인 — SVA 처럼 감싸는 종류를 구분하는 데 쓴다. */
		master_domain->master = master;	/* [한국어] 그 장치. */
		master_domain->ssid = state->ssid;	/* [한국어] 그 PASID. */
		if (new_domain->type == IOMMU_DOMAIN_NESTED)	/* [한국어] 중첩이면. */
			master_domain->nested_ats_flush =	/* [한국어] 무효화 성격도 기록해 둔다 — 뗄 때 이 값으로 고리를 찾는다. */
				to_smmu_nested_domain(new_domain)->enable_ats;

		if (new_domain->iopf_handler) {	/* [한국어] 이 도메인이 폴트를 처리하겠다고 했다면. */
			ret = arm_smmu_enable_iopf(master, master_domain);	/* [한국어] 폴트 처리 큐에 장치를 등록한다. */
			if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
				goto err_free_master_domain;
		}

		/*
		 * During prepare we want the current smmu_domain and new
		 * smmu_domain to be in the devices list before we change any
		 * HW. This ensures that both domains will send ATS
		 * invalidations to the master until we are done.
		 *
		 * It is tempting to make this list only track masters that are
		 * using ATS, but arm_smmu_share_asid() also uses this to change
		 * the ASID of a domain, unrelated to ATS.
		 *
		 * Notice if we are re-attaching the same domain then the list
		 * will have two identical entries and commit will remove only
		 * one of them.
		 */
		/* [한국어] (위 영어 주석 참고) 하드웨어를 바꾸기 전에 두 도메인이 모두
		 * 이 장치를 목록에 갖고 있어야 한다 — 그래야 어느 순간에도 장치 캐시
		 * 무효화가 이 장치를 건너뛰지 않는다.
		 * 같은 도메인에 다시 붙이는 경우 목록에 똑같은 고리가 둘 생기는데,
		 * commit 이 그중 하나만 걷어 내므로 결과는 맞아떨어진다. */
		spin_lock_irqsave(&smmu_domain->devices_lock, flags);	/* [한국어] 목록을 고치는 동안 막는다. */
		if (smmu_domain->enforce_cache_coherency &&	/* [한국어] 이 도메인이 캐시 일관성을 보장하기로 했는데. */
		    !arm_smmu_master_canwbs(master)) {	/* [한국어] 이 장치가 그것을 지킬 수 없다면. */
			spin_unlock_irqrestore(&smmu_domain->devices_lock,	/* [한국어] 목록 조작이 끝났으니 락을 놓는다 (인자가 길어 줄을 나눴다). */
					       flags);
			ret = -EINVAL;	/* [한국어] 붙일 수 없다 — 도메인의 보장이 깨진다. */
			goto err_iopf;	/* [한국어] 폴트 등록부터 되돌린다. */
		}

		if (state->ats_enabled)	/* [한국어] ATS 를 켜기로 했다면. */
			atomic_inc(&smmu_domain->nr_ats_masters);	/* [한국어] 세어 둔다 — 이 값이 0 이면 무효화가 빠른 길로 간다. */
		list_add(&master_domain->devices_elm, &smmu_domain->devices);	/* [한국어] 목록에 건다 — 이제 이 도메인의 무효화가 이 장치에도 간다. */
		spin_unlock_irqrestore(&smmu_domain->devices_lock, flags);	/* [한국어] 목록 조작이 끝났으니 락과 인터럽트 상태를 되돌린다. */

		arm_smmu_install_new_domain_invs(state);	/* [한국어] 무효화 배열도 갈아 끼운다 — STE 를 쓰기 전에 반드시 해야 한다. */
	}

	if (!state->ats_enabled && master->ats_enabled) {	/* [한국어] ATS 를 끄기로 했고 지금 켜져 있다면. */
		pci_disable_ats(to_pci_dev(master->dev));	/* [한국어] PCI 쪽을 먼저 끈다 — STE 를 먼저 바꾸면 장치가 낸 ATS 요청이 거부되어 오류가 쏟아진다. */
		/*
		 * This is probably overkill, but the config write for disabling
		 * ATS should complete before the STE is configured to generate
		 * UR to avoid AER noise.
		 */
		/* [한국어] (위 영어 주석 참고) 설정 쓰기가 STE 변경보다 먼저 끝나야
		 * 장치가 ATS 요청을 멈춘다. 그러지 않으면 PCI 오류 보고가 시끄러워진다.
		 * 주석 자신이 "지나칠 수 있다"고 인정하지만, 조용한 편이 낫다. */
		wmb();
	}
	return 0;	/* [한국어] 실패할 수 있는 일이 모두 끝났다 — 호출자는 반드시 나머지를 마쳐야 한다. */

err_iopf:	/* [한국어] 폴트 등록까지 마친 뒤 실패했을 때. */
	arm_smmu_disable_iopf(master, master_domain);	/* [한국어] 등록을 되돌린다. */
err_free_master_domain:	/* [한국어] 고리를 만든 뒤 실패했을 때. */
	kfree(master_domain);	/* [한국어] 고리를 놓는다. */
err_free_vmaster:	/* [한국어] 게스트 다리를 잡은 뒤 실패했을 때. */
	kfree(state->vmaster);	/* [한국어] 그 다리를 놓는다. */
err_unprepare_invs:	/* [한국어] 배열을 만든 뒤 실패했을 때. */
	kfree(state->new_domain_invst.new_invs);	/* [한국어] 아직 걸지 않았으므로 곧바로 놓아도 된다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * Commit is done after the STE/CD are configured with the EATS setting. It
 * completes synchronizing the PCI device's ATC and finishes manipulating the
 * smmu_domain->devices list.
 */
/*
 * [한국어]
 * arm_smmu_attach_commit - 하드웨어에 쓴 뒤의 뒷정리를 마무리한다
 *
 * @state: prepare 가 채워 둔 상태 묶음.
 *
 * (위 영어 주석 참고) 붙이기 3단계의 마지막이다. 여기서 하는 일은 모두
 * 실패할 수 없는 것들이다 — 이미 하드웨어가 새 설정으로 동작하고 있어
 * 되돌릴 방법이 없기 때문이다.
 *
 * ATS 를 세 갈래로 나눠 다루는 것이 이 함수의 요점이다. 새로 켜는 경우는
 * 켜기 전에 캐시를 비우고, 계속 켜져 있는 경우는 변환이 바뀌었으므로
 * 캐시를 비우고, 끄는 경우는 남은 캐시를 통째로 비운다. 어느 경우든
 * "장치가 낡은 변환을 들고 있지 않게" 만드는 것이 목적이다.
 *
 * 실행 컨텍스트: 붙이기 경로, arm_smmu_asid_lock 아래. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev()/set_pasid() → [이 함수]
 *     → arm_smmu_atc_inv_master() → arm_smmu_install_old_domain_invs()
 */
void arm_smmu_attach_commit(struct arm_smmu_attach_state *state)
{
	struct arm_smmu_master *master = state->master;	/* [한국어] 붙인 장치. */

	lockdep_assert_held(&arm_smmu_asid_lock);	/* [한국어] prepare 부터 이어진 락을 그대로 쥐고 있어야 한다. */

	arm_smmu_attach_commit_vmaster(state);	/* [한국어] 게스트 다리를 실제로 건다 (중첩이 아니면 NULL 을 건다). */

	if (state->ats_enabled && !master->ats_enabled) {	/* [한국어] 새로 켜는 경우. */
		arm_smmu_enable_ats(master);	/* [한국어] 캐시를 비우고 PCI 쪽을 켠다. */
	} else if (state->ats_enabled && master->ats_enabled) {	/* [한국어] 계속 켜져 있는 경우. */
		/*
		 * The translation has changed, flush the ATC. At this point the
		 * SMMU is translating for the new domain and both the old&new
		 * domain will issue invalidations.
		 */
		/* [한국어] (위 영어 주석 참고) 변환이 바뀌었으므로 장치가 들고 있던
		 * 옛 변환을 버려야 한다. 이 시점에 두 도메인이 모두 이 장치를
		 * 무효화 대상으로 삼고 있어, 그 사이에 새 항목이 들어와도 안전하다. */
		arm_smmu_atc_inv_master(master, state->ssid);	/* [한국어] 그 PASID 의 캐시를 비운다. */
	} else if (!state->ats_enabled && master->ats_enabled) {	/* [한국어] 끄는 경우. */
		/* ATS is being switched off, invalidate the entire ATC */
		/* [한국어] (위 영어 주석 참고) 끈 뒤에는 무효화를 보낼 수 없으므로,
		 * 끄기 직전에 남은 것을 통째로 비운다. */
		arm_smmu_atc_inv_master(master, IOMMU_NO_PASID);	/* [한국어] PASID 를 가리지 않으면 전체가 비워진다. */
	}

	arm_smmu_remove_master_domain(master, state->old_domain, state->ssid);	/* [한국어] 옛 도메인의 목록에서 이 장치를 뺀다. */
	arm_smmu_install_old_domain_invs(state);	/* [한국어] 그 도메인의 무효화 배열에서도 걷어 낸다 — STE 를 쓴 뒤여야 한다. */
	master->ats_enabled = state->ats_enabled;	/* [한국어] 실제 상태를 기록한다 — 다음 붙이기가 이 값을 기준으로 판단한다. */
}

/*
 * [한국어]
 * arm_smmu_attach_dev - 장치를 페이지 테이블 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙일 장치.
 * @old_domain: 그 장치가 쓰던 이전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * 가장 흔한 붙이기 경로다. 3단계 규약을 그대로 밟는다 — 준비하고,
 * 하드웨어(CD 와 STE)에 쓰고, 마무리한다.
 *
 * 1단계와 2단계의 쓰기 순서가 반대인 것에 주목할 만하다. 1단계는 서술자를
 * 먼저 쓰고 그것을 가리키는 STE 를 나중에 쓴다 — 아직 채워지지 않은 표를
 * 하드웨어가 따라가지 않게. 2단계는 STE 를 먼저 쓰고 서술자를 나중에
 * 지운다 — 하드웨어가 더 이상 그 표를 보지 않게 된 뒤에 지워야 한다.
 *
 * 2단계 도메인에 붙일 때 PASID 를 쓰고 있으면 거부하는 것도 중요하다.
 * 2단계 STE 에는 문맥 서술자 표를 가리킬 자리가 없어, PASID 를 쓰던
 * 장치를 그대로 옮기면 그 PASID 들이 갈 곳을 잃는다.
 *
 * 실행 컨텍스트: iommu 코어의 붙이기. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev = [이 함수]
 *     → arm_smmu_attach_prepare() → arm_smmu_install_ste_for_dev()
 *     → arm_smmu_attach_commit()
 */
static int arm_smmu_attach_dev(struct iommu_domain *domain, struct device *dev,
			       struct iommu_domain *old_domain)
{
	int ret = 0;	/* [한국어] 결과. */
	struct arm_smmu_ste target;	/* [한국어] 심을 스트림 표 항목. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 펌웨어가 이 장치를 기술했는지 확인용. */
	struct arm_smmu_device *smmu;	/* [한국어] 그 장치가 매달린 하드웨어. */
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct arm_smmu_attach_state state = {	/* [한국어] 3단계가 주고받을 상태. */
		.old_domain = old_domain,	/* [한국어] 떠나는 도메인. */
		.ssid = IOMMU_NO_PASID,	/* [한국어] 장치 전체를 붙이는 것이라 PASID 는 기본 자리다. */
	};
	struct arm_smmu_master *master;	/* [한국어] 장치의 SMMU 쪽 상태. */
	struct arm_smmu_cd *cdptr;	/* [한국어] 1단계일 때 쓸 서술자 자리. */

	if (!fwspec)	/* [한국어] 펌웨어가 이 장치를 IOMMU 뒤에 있다고 기술하지 않았다면. */
		return -ENOENT;	/* [한국어] 붙일 수 없다. */

	state.master = master = dev_iommu_priv_get(dev);	/* [한국어] 장치 상태를 꺼내 상태 묶음에도 담는다. */
	smmu = master->smmu;	/* [한국어] 그 하드웨어. */

	if (smmu_domain->smmu != smmu)	/* [한국어] 도메인이 다른 하드웨어에 묶여 있다면. */
		return -EINVAL;	/* [한국어] 페이지 테이블 설정이 그 하드웨어에 맞춰져 있어 옮길 수 없다. */

	if (smmu_domain->stage == ARM_SMMU_DOMAIN_S1) {	/* [한국어] 1단계 도메인이면 서술자 자리가 필요하다. */
		cdptr = arm_smmu_alloc_cd_ptr(master, IOMMU_NO_PASID);	/* [한국어] 없으면 표까지 잡아 준다 — 실패할 수 있는 일이라 먼저 한다. */
		if (!cdptr)	/* [한국어] 서술자 자리를 못 마련했으면 붙일 수 없다. */
			return -ENOMEM;
	} else if (arm_smmu_ssids_in_use(&master->cd_table))	/* [한국어] 2단계인데 이 장치가 PASID 를 쓰고 있다면. */
		return -EBUSY;	/* [한국어] 2단계 STE 에는 문맥 표를 가리킬 자리가 없어 그 PASID 들이 갈 곳을 잃는다. */

	/*
	 * Prevent arm_smmu_share_asid() from trying to change the ASID
	 * of either the old or new domain while we are working on it.
	 * This allows the STE and the smmu_domain->devices list to
	 * be inconsistent during this routine.
	 */
	/* [한국어] (위 영어 주석 참고) 이 락이 붙이기 전체를 감싸므로, 그동안
	 * STE 와 장치 목록이 서로 어긋나 있어도 아무도 그것을 보지 못한다.
	 * 그 덕분에 3단계를 나눠 진행할 수 있다. */
	mutex_lock(&arm_smmu_asid_lock);

	ret = arm_smmu_attach_prepare(&state, domain);	/* [한국어] 1단계 — 실패할 수 있는 일을 모두 끝낸다. */
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기 구간이 끝났으니 락을 놓는다. */
		return ret;	/* [한국어] 하드웨어는 아직 옛 설정 그대로다. */
	}

	switch (smmu_domain->stage) {	/* [한국어] 2단계 — 하드웨어에 쓴다. 갈래에 따라 순서가 다르다. */
	case ARM_SMMU_DOMAIN_S1: {	/* [한국어] 1단계 변환. */
		struct arm_smmu_cd target_cd;	/* [한국어] 지을 서술자. */

		arm_smmu_make_s1_cd(&target_cd, master, smmu_domain);	/* [한국어] 페이지 테이블 설정을 서술자로 옮긴다. */
		arm_smmu_write_cd_entry(master, IOMMU_NO_PASID, cdptr,	/* [한국어] 서술자를 먼저 쓴다 — STE 가 이 표를 가리키기 전에 채워져 있어야 한다. */
					&target_cd);
		arm_smmu_make_cdtable_ste(&target, master, state.ats_enabled,	/* [한국어] 그 표를 가리키는 항목을 짓는다. */
					  STRTAB_STE_1_S1DSS_SSID0);	/* [한국어] 서술자 없는 PASID 는 0번 서술자로 보낸다 — 장치 전체를 붙이는 흔한 설정이다. */
		arm_smmu_install_ste_for_dev(master, &target);	/* [한국어] 이 순간부터 하드웨어가 새 도메인으로 변환한다. */
		break;
	}
	case ARM_SMMU_DOMAIN_S2:	/* [한국어] 2단계 변환. */
		arm_smmu_make_s2_domain_ste(&target, master, smmu_domain,	/* [한국어] 2단계 항목을 짓는다. */
					    state.ats_enabled);
		arm_smmu_install_ste_for_dev(master, &target);	/* [한국어] STE 를 먼저 바꾼다 — 하드웨어가 더 이상 문맥 표를 보지 않게 만든 뒤에야. */
		arm_smmu_clear_cd(master, IOMMU_NO_PASID);	/* [한국어] 서술자를 지운다. 순서가 반대면 하드웨어가 빈 서술자를 읽는다. */
		break;
	default:	/* [한국어] SVA 도메인이 잘못 온 경우 등. */
		WARN_ON(true);	/* [한국어] 있어서는 안 될 상태 — 흔적을 남긴다. */
		break;
	}

	arm_smmu_attach_commit(&state);	/* [한국어] 3단계 — ATS 를 켜고, 옛 도메인에서 걷어 내고, 상태를 확정한다. */
	mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기 구간이 끝났으니 락을 놓는다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_s1_set_dev_pasid - 그 PASID 를 1단계 도메인에 붙인다
 *
 * @domain: 붙일 1단계 도메인.
 * @dev: 붙일 장치.
 * @id: 붙일 PASID.
 * @old: 그 PASID 가 쓰던 이전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * 장치 전체가 아니라 PASID 하나만 도메인에 잇는다. 서술자를 짓고 공통
 * 경로에 넘기는 것이 전부다.
 *
 * ASID 를 락 밖에서 읽는 것에 주석이 해명을 달아 두었다 — 그 사이 값이
 * 바뀌더라도 arm_smmu_set_pasid() 가 락 안에서 다시 고쳐 넣으므로 문제가
 * 없다. 락을 잡는 구간을 줄이려는 의도다.
 *
 * 실행 컨텍스트: iommu 코어의 PASID 붙이기. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_attach_device_pasid() → domain_ops->set_dev_pasid = [이 함수]
 *     → arm_smmu_make_s1_cd() → arm_smmu_set_pasid()
 */
static int arm_smmu_s1_set_dev_pasid(struct iommu_domain *domain,
				     struct device *dev, ioasid_t id,
				     struct iommu_domain *old)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치 상태. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 그 하드웨어. */
	struct arm_smmu_cd target_cd;	/* [한국어] 지을 서술자. */

	if (smmu_domain->smmu != smmu)	/* [한국어] 다른 하드웨어의 도메인이면. */
		return -EINVAL;

	if (smmu_domain->stage != ARM_SMMU_DOMAIN_S1)	/* [한국어] PASID 는 1단계 변환에서만 뜻이 있다. */
		return -EINVAL;

	/*
	 * We can read cd.asid outside the lock because arm_smmu_set_pasid()
	 * will fix it
	 */
	/* [한국어] (위 영어 주석 참고) 여기서 읽은 ASID 가 그 사이 바뀌더라도,
	 * 아래 함수가 락 안에서 다시 고쳐 넣는다. 락 구간을 줄이려는 의도다. */
	arm_smmu_make_s1_cd(&target_cd, master, smmu_domain);	/* [한국어] 서술자를 미리 지어 둔다. */
	return arm_smmu_set_pasid(master, to_smmu_domain(domain), id,	/* [한국어] 공통 경로에 넘긴다. */
				  &target_cd, old);
}

/*
 * [한국어]
 * arm_smmu_update_ste - PASID 를 다는 김에 STE 도 문맥 표 형태로 맞춘다
 *
 * @master: 대상 장치.
 * @sid_domain: 장치 전체에 걸려 있는 도메인 (통과 또는 차단).
 * @ats_enabled: 이번에 정한 ATS 상태.
 *
 * PASID 를 쓰려면 STE 가 문맥 서술자 표를 가리켜야 한다. 그런데 장치
 * 전체는 통과나 차단 상태일 수 있는데, 그 상태를 유지하면서도 PASID 는
 * 동작하게 만들어야 한다.
 *
 * s1dss 필드가 그 문제를 푼다 — "서술자 없는 PASID(곧 장치 전체 트래픽)를
 * 어떻게 할 것인가"를 정하는 값이라, 통과 도메인이면 우회로, 차단
 * 도메인이면 중단으로 두면 두 요구가 함께 성립한다.
 *
 * 이미 문맥 표 형태이고 ATS 상태도 같으면 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: PASID 붙이기 경로. 완료를 기다린다.
 *
 * 호출 체인:
 *   arm_smmu_set_pasid() → [이 함수] → arm_smmu_install_ste_for_dev()
 */
static void arm_smmu_update_ste(struct arm_smmu_master *master,
				struct iommu_domain *sid_domain,
				bool ats_enabled)
{
	unsigned int s1dss = STRTAB_STE_1_S1DSS_TERMINATE;	/* [한국어] 기본은 중단 — 차단 도메인에 맞는 값이다. */
	struct arm_smmu_ste ste;	/* [한국어] 지을 항목. */

	if (master->cd_table.in_ste && master->ste_ats_enabled == ats_enabled)	/* [한국어] 이미 문맥 표 형태이고 ATS 상태도 같으면. */
		return;	/* [한국어] 바꿀 것이 없다. */

	if (sid_domain->type == IOMMU_DOMAIN_IDENTITY)	/* [한국어] 장치 전체가 통과 상태라면. */
		s1dss = STRTAB_STE_1_S1DSS_BYPASS;	/* [한국어] 서술자 없는 트래픽을 우회시켜 그 상태를 유지한다. */
	else
		WARN_ON(sid_domain->type != IOMMU_DOMAIN_BLOCKED);	/* [한국어] 그 밖에는 차단 도메인이어야 한다 — 다른 종류면 호출부의 검사가 잘못된 것이다. */

	/*
	 * Change the STE into a cdtable one with SID IDENTITY/BLOCKED behavior
	 * using s1dss if necessary. If the cd_table is already installed then
	 * the S1DSS is correct and this will just update the EATS. Otherwise it
	 * installs the entire thing. This will be hitless.
	 */
	/* [한국어] (위 영어 주석 참고) 문맥 표를 가리키면서도 장치 전체는 통과·차단인
	 * 항목을 짓는다. 이미 표가 걸려 있었다면 ATS 필드만 바뀌어 한 번의 쓰기로
	 * 끝나고, 아니면 전체가 새로 심긴다. 어느 쪽이든 끊김이 없다. */
	arm_smmu_make_cdtable_ste(&ste, master, ats_enabled, s1dss);
	arm_smmu_install_ste_for_dev(master, &ste);	/* [한국어] 하드웨어에 심는다 — 이 순간부터 새 설정으로 동작한다. */
}

/*
 * [한국어]
 * arm_smmu_set_pasid - 그 PASID 를 도메인에 잇는다 (공통 경로)
 *
 * @master: 대상 장치.
 * @smmu_domain: 그 PASID 가 쓸 1단계 도메인.
 * @pasid: 붙일 PASID.
 * @cd: 미리 지어 둔 서술자 값.
 * @old: 그 PASID 가 쓰던 이전 도메인.
 * @return: 0 성공, 음수 오류.
 *
 * 보통 도메인과 SVA 도메인이 함께 쓰는 경로다. 3단계 규약을 밟되,
 * 하드웨어에 쓰는 것이 STE 가 아니라 문맥 서술자라는 점이 다르다.
 *
 * ASID 를 락 안에서 다시 채워 넣는 대목이 중요하다. 호출자가 락 밖에서
 * 서술자를 지었으므로 그 사이 ASID 가 바뀌었을 수 있고, 그대로 쓰면
 * 엉뚱한 주소 공간을 가리키게 된다.
 *
 * 실행 컨텍스트: PASID 붙이기. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_s1_set_dev_pasid()/SVA → [이 함수]
 *     → arm_smmu_attach_prepare() → arm_smmu_write_cd_entry()
 *     → arm_smmu_update_ste() → arm_smmu_attach_commit()
 */
int arm_smmu_set_pasid(struct arm_smmu_master *master,
		       struct arm_smmu_domain *smmu_domain, ioasid_t pasid,
		       struct arm_smmu_cd *cd, struct iommu_domain *old)
{
	struct iommu_domain *sid_domain =	/* [한국어] 장치 전체에 걸려 있는 도메인 — STE 를 맞출 때 필요하다. */
		iommu_driver_get_domain_for_dev(master->dev);
	struct arm_smmu_attach_state state = {	/* [한국어] 3단계가 주고받을 상태. */
		.master = master,	/* [한국어] 붙이는 대상 장치. */
		.ssid = pasid,	/* [한국어] 이번에는 특정 PASID 를 붙인다. */
		.old_domain = old,
	};
	struct arm_smmu_cd *cdptr;	/* [한국어] 표 안의 서술자 자리. */
	int ret;	/* [한국어] 결과. */

	/* The core code validates pasid */
	/* [한국어] (위 영어 주석 참고) PASID 범위 검사는 코어가 이미 했다. */

	if (smmu_domain->smmu != master->smmu)	/* [한국어] 다른 하드웨어의 도메인이면. */
		return -EINVAL;

	if (!master->cd_table.in_ste &&	/* [한국어] STE 가 아직 문맥 표를 가리키지 않는데. */
	    sid_domain->type != IOMMU_DOMAIN_IDENTITY &&	/* [한국어] 장치 전체가 통과도 아니고. */
	    sid_domain->type != IOMMU_DOMAIN_BLOCKED)	/* [한국어] 차단도 아니라면. */
		return -EINVAL;	/* [한국어] STE 를 문맥 표 형태로 바꿀 방법이 없다 — 실제 변환 도메인이 걸려 있다는 뜻이다. */

	cdptr = arm_smmu_alloc_cd_ptr(master, pasid);	/* [한국어] 서술자 자리를 마련한다 — 실패할 수 있어 락 밖에서 먼저 한다. */
	if (!cdptr)	/* [한국어] 서술자 자리를 못 마련했으면. */
		return -ENOMEM;

	mutex_lock(&arm_smmu_asid_lock);	/* [한국어] 여기부터 3단계 규약. */
	ret = arm_smmu_attach_prepare(&state, &smmu_domain->domain);	/* [한국어] 1단계 — 실패할 수 있는 일을 끝낸다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		goto out_unlock;

	/*
	 * We don't want to obtain to the asid_lock too early, so fix up the
	 * caller set ASID under the lock in case it changed.
	 */
	/* [한국어] (위 영어 주석 참고) 호출자가 락 밖에서 서술자를 지었으므로 그
	 * 사이 ASID 가 바뀌었을 수 있다. 락 안에서 지금 값으로 다시 채워야
	 * 엉뚱한 주소 공간을 가리키지 않는다. */
	cd->data[0] &= ~cpu_to_le64(CTXDESC_CD_0_ASID);	/* [한국어] 옛 값을 지우고. */
	cd->data[0] |= cpu_to_le64(	/* [한국어] 지금 값을 넣는다. */
		FIELD_PREP(CTXDESC_CD_0_ASID, smmu_domain->cd.asid));

	arm_smmu_write_cd_entry(master, pasid, cdptr, cd);	/* [한국어] 2단계 — 서술자를 안전한 순서로 쓴다. */
	arm_smmu_update_ste(master, sid_domain, state.ats_enabled);	/* [한국어] STE 도 문맥 표 형태로 맞춘다 (필요할 때만). */

	arm_smmu_attach_commit(&state);	/* [한국어] 3단계 — 뒷정리. */

out_unlock:	/* [한국어] 성공·실패 모두 이 자리를 지난다. */
	mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기 구간이 끝났으니 락을 놓는다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_blocking_set_dev_pasid - 그 PASID 를 차단 상태로 되돌린다
 *
 * @new_domain: 차단 도메인 (여기서는 쓰지 않는다).
 * @dev: 대상 장치.
 * @pasid: 떼어 낼 PASID.
 * @old_domain: 그 PASID 가 쓰던 도메인.
 * @return: 항상 0 — 떼기는 실패할 수 없다.
 *
 * PASID 를 놓는 경로다. 서술자를 지우고, 장치 캐시를 비우고, 도메인의
 * 목록과 무효화 배열에서 걷어 낸다. 이 모든 일이 실패할 수 없게 짜여
 * 있다 — 코어가 떼기 실패를 다룰 방법이 없기 때문이다.
 *
 * 마지막 대목이 흥미롭다. 이 장치가 더 이상 PASID 를 쓰지 않게 되면
 * STE 를 문맥 표 형태로 유지할 이유가 없으므로, 장치 전체의 도메인을
 * 다시 붙여 원래 형태(통과 또는 차단)로 되돌린다.
 *
 * 실행 컨텍스트: PASID 떼기. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_detach_device_pasid() → domain_ops->set_dev_pasid = [이 함수]
 *     → arm_smmu_clear_cd() → arm_smmu_install_old_domain_invs()
 */
static int arm_smmu_blocking_set_dev_pasid(struct iommu_domain *new_domain,
					   struct device *dev, ioasid_t pasid,
					   struct iommu_domain *old_domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(old_domain);	/* [한국어] 떠나는 도메인. */
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치 상태. */
	struct arm_smmu_attach_state state = {	/* [한국어] 배열 교체에 쓸 상태 — 새 도메인이 없는 반쪽짜리다. */
		.master = master,	/* [한국어] 대상 장치. */
		.old_domain = old_domain,
		.ssid = pasid,
	};

	mutex_lock(&arm_smmu_asid_lock);	/* [한국어] 배열 교체 구간을 락으로 감싼다. */
	arm_smmu_attach_prepare_invs(&state, NULL);	/* [한국어] 새 도메인을 NULL 로 넘겨, 옛 도메인에서 걷어 낼 준비만 한다. */
	arm_smmu_clear_cd(master, pasid);	/* [한국어] 서술자를 지운다 — 이 뒤로 그 PASID 의 접근은 오류가 된다. */
	if (master->ats_enabled)	/* [한국어] ATS 를 쓰던 장치라면. */
		arm_smmu_atc_inv_master(master, pasid);	/* [한국어] 그 PASID 로 캐시된 변환을 지운다. */
	arm_smmu_remove_master_domain(master, &smmu_domain->domain, pasid);	/* [한국어] 도메인의 장치 목록에서 뺀다. */
	arm_smmu_install_old_domain_invs(&state);	/* [한국어] 무효화 배열에서도 걷어 낸다. */
	mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기 구간이 끝났으니 락을 놓는다. */

	/*
	 * When the last user of the CD table goes away downgrade the STE back
	 * to a non-cd_table one, by re-attaching its sid_domain.
	 */
	/* [한국어] (위 영어 주석 참고) 마지막 PASID 가 사라지면 문맥 표를 가리킬
	 * 이유가 없다. 장치 전체의 도메인을 다시 붙여 원래 형태로 되돌린다 —
	 * 그래야 하드웨어가 쓸데없이 표를 읽지 않는다. */
	if (!arm_smmu_ssids_in_use(&master->cd_table)) {	/* [한국어] 쓰는 PASID 가 하나도 없게 됐다면. */
		struct iommu_domain *sid_domain =	/* [한국어] 장치 전체에 걸린 도메인. */
			iommu_driver_get_domain_for_dev(master->dev);

		if (sid_domain->type == IOMMU_DOMAIN_IDENTITY ||	/* [한국어] 통과이거나. */
		    sid_domain->type == IOMMU_DOMAIN_BLOCKED)	/* [한국어] 차단 도메인일 때만. */
			sid_domain->ops->attach_dev(sid_domain, dev,	/* [한국어] 자기 자신을 다시 붙여 STE 를 원래 형태로 되돌린다. */
						    sid_domain);
	}
	return 0;	/* [한국어] 떼기는 실패하지 않는다. */
}

/*
 * [한국어]
 * arm_smmu_attach_dev_ste - 지정한 STE 를 심어 장치를 붙인다
 *
 * @domain: 붙일 도메인 (통과 또는 차단).
 * @old_domain: 떠나는 도메인.
 * @dev: 대상 장치.
 * @ste: 심을 항목 (문맥 표를 쓰는 경우 여기서 다시 지어진다).
 * @s1dss: 문맥 표를 써야 할 때 서술자 없는 트래픽을 어떻게 다룰지.
 *
 * 통과·차단 도메인이 함께 쓰는 경로다. 두 도메인 모두 페이지 테이블이
 * 없어 하는 일이 거의 같고, 다른 것은 심을 항목뿐이라 이렇게 묶었다.
 *
 * 문맥 표를 쓰고 있으면 이야기가 달라진다. PASID 가 살아 있는 채로 장치
 * 전체만 통과·차단으로 바꾸는 상황이므로, 넘겨받은 항목 대신 문맥 표를
 * 가리키는 항목을 s1dss 로 지어야 한다.
 *
 * 그때 ATS 를 켜 두는 것도 요점이다. 어느 PASID 가 ATS 를 쓰고 있는지
 * 알 수 없으므로 쓰고 있다고 가정하는 편이 안전하다.
 *
 * 마지막에 서술자를 지우는 순서도 주석이 설명한다 — 목록에서 뺀 뒤에
 * 지워야, 같은 서술자를 고치려는 다른 경로와 부딪히지 않는다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_attach_dev_identity()/blocked() → [이 함수]
 *     → arm_smmu_attach_prepare() → arm_smmu_install_ste_for_dev()
 */
static void arm_smmu_attach_dev_ste(struct iommu_domain *domain,
				    struct iommu_domain *old_domain,
				    struct device *dev,
				    struct arm_smmu_ste *ste,
				    unsigned int s1dss)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치 상태. */
	struct arm_smmu_attach_state state = {	/* [한국어] 3단계가 주고받을 상태. */
		.master = master,	/* [한국어] 대상 장치. */
		.old_domain = old_domain,
		.ssid = IOMMU_NO_PASID,	/* [한국어] 장치 전체를 다룬다. */
	};

	/*
	 * Do not allow any ASID to be changed while are working on the STE,
	 * otherwise we could miss invalidations.
	 */
	/* [한국어] (위 영어 주석 참고) 항목을 고치는 동안 ASID 가 바뀌면, 그
	 * 사이의 무효화가 어느 쪽에도 닿지 않는 창이 생긴다. */
	mutex_lock(&arm_smmu_asid_lock);

	/*
	 * If the CD table is not in use we can use the provided STE, otherwise
	 * we use a cdtable STE with the provided S1DSS.
	 */
	/* [한국어] (위 영어 주석 참고) PASID 가 살아 있으면 문맥 표를 계속 가리켜야
	 * 하므로, 넘겨받은 항목을 그대로 쓸 수 없다. */
	if (arm_smmu_ssids_in_use(&master->cd_table)) {	/* [한국어] 쓰는 PASID 가 있다면. */
		/*
		 * If a CD table has to be present then we need to run with ATS
		 * on because we have to assume a PASID is using ATS. For
		 * IDENTITY this will setup things so that S1DSS=bypass which
		 * follows the explanation in "13.6.4 Full ATS skipping stage 1"
		 * and allows for ATS on the RID to work.
		 */
		/* [한국어] (위 영어 주석 참고) 어느 PASID 가 ATS 를 쓰는지 알 수 없으므로
		 * 쓰고 있다고 가정한다. 통과 도메인에서는 s1dss 가 우회이므로 장치
		 * 전체 트래픽도 ATS 로 동작하게 되는데, 규격이 그 조합을 허용한다. */
		state.cd_needs_ats = true;	/* [한국어] ATS 를 켜 두라고 알린다. */
		arm_smmu_attach_prepare(&state, domain);	/* [한국어] 1단계. */
		arm_smmu_make_cdtable_ste(ste, master, state.ats_enabled, s1dss);	/* [한국어] 넘겨받은 항목을 문맥 표 형태로 덮어쓴다. */
	} else {	/* [한국어] PASID 를 쓰지 않는다면. */
		arm_smmu_attach_prepare(&state, domain);	/* [한국어] 넘겨받은 항목을 그대로 쓴다. */
	}
	arm_smmu_install_ste_for_dev(master, ste);	/* [한국어] 2단계 — 하드웨어에 심는다. */
	arm_smmu_attach_commit(&state);	/* [한국어] 3단계 — 뒷정리. */
	mutex_unlock(&arm_smmu_asid_lock);	/* [한국어] 붙이기 구간이 끝났으니 락을 놓는다. */

	/*
	 * This has to be done after removing the master from the
	 * arm_smmu_domain->devices to avoid races updating the same context
	 * descriptor from arm_smmu_share_asid().
	 */
	/* [한국어] (위 영어 주석 참고) 목록에서 빠진 뒤에 지워야, 같은 서술자를
	 * 고치려는 다른 경로와 부딪히지 않는다. 순서를 지키려고 락 밖으로 뺐다. */
	arm_smmu_clear_cd(master, IOMMU_NO_PASID);	/* [한국어] 기본 서술자를 지운다 — PASID 를 쓰고 있었다면 그 자리는 건드리지 않는다. */
}

/*
 * [한국어]
 * arm_smmu_attach_dev_identity - 장치를 통과 상태로 만든다
 *
 * @domain: 통과 도메인.
 * @dev: 대상 장치.
 * @old_domain: 떠나는 도메인.
 * @return: 항상 0.
 *
 * 장치가 낸 주소를 그대로 물리 주소로 쓰게 한다. IOMMU 를 켜기 전 상태를
 * 이어 가야 하는 장치나, 사용자가 통과를 요청했을 때 쓴다.
 *
 * 게스트 다리를 먼저 끊는 것이 중요하다 — 통과 상태에서는 게스트에게
 * 사건을 돌려줄 일이 없다.
 *
 * 실행 컨텍스트: 붙이기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev = [이 함수]
 *     → arm_smmu_make_bypass_ste() → arm_smmu_attach_dev_ste()
 */
static int arm_smmu_attach_dev_identity(struct iommu_domain *domain,
					struct device *dev,
					struct iommu_domain *old_domain)
{
	struct arm_smmu_ste ste;	/* [한국어] 심을 항목. */
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치 상태. */

	arm_smmu_master_clear_vmaster(master);	/* [한국어] 게스트 다리를 끊는다 — 통과 상태에서는 돌려줄 사건이 없다. */
	arm_smmu_make_bypass_ste(master->smmu, &ste);	/* [한국어] 변환하지 않고 통과시키는 항목. */
	arm_smmu_attach_dev_ste(domain, old_domain, dev, &ste,	/* [한국어] 공통 경로에 넘긴다. */
				STRTAB_STE_1_S1DSS_BYPASS);	/* [한국어] 문맥 표를 써야 하는 경우, 서술자 없는 트래픽도 우회시켜 통과의 뜻을 지킨다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/* [한국어] 통과 도메인의 연산표 — 붙이기 하나뿐이다.
 *
 * 매핑을 걸 것도, 놓을 것도 없다. 이 도메인은 시스템에 하나만 있으면
 * 되므로 아래에서 정적 인스턴스로 둔다. */
static const struct iommu_domain_ops arm_smmu_identity_ops = {
	.attach_dev = arm_smmu_attach_dev_identity,	/* [한국어] 장치를 통과 상태로 만드는 갈고리. */
};

/* [한국어] 시스템에 하나뿐인 통과 도메인.
 *
 * 상태를 갖지 않으므로 모든 장치가 같은 인스턴스를 공유해도 된다.
 * 정적으로 두면 할당 실패를 걱정할 필요도 없다. */
static struct iommu_domain arm_smmu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,	/* [한국어] 코어가 이 종류로 알아본다. */
	.ops = &arm_smmu_identity_ops,	/* [한국어] 위의 짧은 연산표. */
};

/*
 * [한국어]
 * arm_smmu_attach_dev_blocked - 장치의 모든 DMA 를 막는다
 *
 * @domain: 차단 도메인.
 * @dev: 대상 장치.
 * @old_domain: 떠나는 도메인.
 * @return: 항상 0.
 *
 * 장치를 놓거나, 사용자 공간에 넘기기 전 안전한 상태로 만들 때 쓴다.
 * 통과와 짜임이 같고 심는 항목만 다르다.
 *
 * 실행 컨텍스트: 붙이기·떼기 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_detach_device() 등 → domain_ops->attach_dev = [이 함수]
 *     → arm_smmu_make_abort_ste() → arm_smmu_attach_dev_ste()
 */
static int arm_smmu_attach_dev_blocked(struct iommu_domain *domain,
				       struct device *dev,
				       struct iommu_domain *old_domain)
{
	struct arm_smmu_ste ste;	/* [한국어] 심을 항목. */
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치 상태. */

	arm_smmu_master_clear_vmaster(master);	/* [한국어] 게스트 다리를 끊는다. */
	arm_smmu_make_abort_ste(&ste);	/* [한국어] 모든 접근을 거부하는 항목. */
	arm_smmu_attach_dev_ste(domain, old_domain, dev, &ste,	/* [한국어] 공통 경로에 넘긴다. */
				STRTAB_STE_1_S1DSS_TERMINATE);	/* [한국어] 문맥 표를 써야 하는 경우, 서술자 없는 트래픽은 중단시켜 차단의 뜻을 지킨다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/* [한국어] 차단 도메인의 연산표.
 *
 * 통과와 달리 set_dev_pasid 가 있다 — PASID 를 놓는 경로가 "차단 도메인에
 * 붙인다"는 형태로 표현되기 때문이다. */
static const struct iommu_domain_ops arm_smmu_blocked_ops = {
	.attach_dev = arm_smmu_attach_dev_blocked,	/* [한국어] 장치 전체를 막는 갈고리. */
	.set_dev_pasid = arm_smmu_blocking_set_dev_pasid,	/* [한국어] PASID 하나를 놓는 갈고리. */
};

/* [한국어] 시스템에 하나뿐인 차단 도메인. 통과 도메인과 같은 이유로 정적이다. */
static struct iommu_domain arm_smmu_blocked_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,	/* [한국어] 코어가 이 종류로 알아본다. */
	.ops = &arm_smmu_blocked_ops,
};

/*
 * [한국어]
 * arm_smmu_domain_alloc_paging_flags - 요청한 성질의 페이지 테이블 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 장치 (어느 하드웨어인지 여기서 안다).
 * @flags: 사용자가 요청한 성질.
 * @user_data: 하드웨어별 추가 데이터 (이 드라이버는 받지 않는다).
 * @return: 만들어진 도메인, 실패하면 ERR_PTR.
 *
 * 플래그 조합을 그대로 case 로 늘어놓은 것이 눈에 띈다. 조합마다 허용
 * 여부와 도메인 갈래가 정해져 있고, 모르는 조합은 거부한다 — 새 플래그가
 * 생겼을 때 조용히 무시되지 않게 하는 방식이다.
 *
 * 중첩 부모는 반드시 2단계여야 하고, 더티 추적과 PASID 는 1단계여야
 * 한다. 그 둘을 함께 요청할 수 없는 이유가 여기서 드러난다.
 *
 * 실행 컨텍스트: 도메인 할당. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_paging_domain_alloc()/iommufd → iommu_ops 갈고리 = [이 함수]
 *     → arm_smmu_domain_alloc() → arm_smmu_domain_finalise()
 */
static struct iommu_domain *
arm_smmu_domain_alloc_paging_flags(struct device *dev, u32 flags,
				   const struct iommu_user_data *user_data)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치 상태. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 이 도메인이 붙을 하드웨어. */
	const u32 PAGING_FLAGS = IOMMU_HWPT_ALLOC_DIRTY_TRACKING |	/* [한국어] 이 드라이버가 아는 플래그 전부. */
				 IOMMU_HWPT_ALLOC_PASID |
				 IOMMU_HWPT_ALLOC_NEST_PARENT;
	struct arm_smmu_domain *smmu_domain;	/* [한국어] 만들 도메인. */
	int ret;	/* [한국어] 중간 결과. */

	if (flags & ~PAGING_FLAGS)	/* [한국어] 모르는 플래그가 섞여 있으면. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 조용히 무시하지 않고 거부한다 — 무시하면 나중에 뜻이 생겼을 때 호환이 깨진다. */
	if (user_data)	/* [한국어] 하드웨어별 추가 데이터. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 이 드라이버는 받지 않는다. */

	smmu_domain = arm_smmu_domain_alloc();	/* [한국어] 껍데기와 빈 무효화 배열. */
	if (IS_ERR(smmu_domain))	/* [한국어] 껍데기를 못 잡았으면 그대로 올린다. */
		return ERR_CAST(smmu_domain);

	switch (flags) {	/* [한국어] 조합마다 허용 여부와 갈래가 다르다. */
	case 0:	/* [한국어] 아무 성질도 요청하지 않은 흔한 경우. */
		/* Prefer S1 if available */
		/* [한국어] (위 영어 주석 참고) 1단계가 있으면 그쪽을 쓴다 — 페이지
		 * 테이블이 한 겹이라 변환이 빠르고, PASID 도 쓸 수 있다. */
		if (smmu->features & ARM_SMMU_FEAT_TRANS_S1)
			smmu_domain->stage = ARM_SMMU_DOMAIN_S1;
		else
			smmu_domain->stage = ARM_SMMU_DOMAIN_S2;	/* [한국어] 1단계가 없는 하드웨어도 있다. */
		break;
	case IOMMU_HWPT_ALLOC_NEST_PARENT:	/* [한국어] 게스트를 가둘 바깥 울타리로 쓸 도메인. */
		if (!(smmu->features & ARM_SMMU_FEAT_NESTING)) {	/* [한국어] 중첩을 지원하지 않으면. */
			ret = -EOPNOTSUPP;	/* [한국어] 중첩을 지원하지 않는 하드웨어다. */
			goto err_free;	/* [한국어] 잡아 둔 것을 되돌리고 나간다. */
		}
		smmu_domain->stage = ARM_SMMU_DOMAIN_S2;	/* [한국어] 부모는 반드시 2단계다 — 게스트가 1단계를 쓰기 때문이다. */
		smmu_domain->nest_parent = true;	/* [한국어] 무효화 목록을 지을 때 이 표시를 본다. */
		break;
	case IOMMU_HWPT_ALLOC_DIRTY_TRACKING:	/* [한국어] 더티 추적. */
	case IOMMU_HWPT_ALLOC_DIRTY_TRACKING | IOMMU_HWPT_ALLOC_PASID:	/* [한국어] 더티 추적과 PASID 를 함께. */
	case IOMMU_HWPT_ALLOC_PASID:	/* [한국어] PASID 만. */
		if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1)) {	/* [한국어] 셋 다 1단계가 있어야 한다. */
			ret = -EOPNOTSUPP;	/* [한국어] 1단계 변환이 없는 하드웨어다. */
			goto err_free;	/* [한국어] 잡아 둔 것을 되돌리고 나간다. */
		}
		smmu_domain->stage = ARM_SMMU_DOMAIN_S1;	/* [한국어] 더티 비트도 PASID 도 1단계 기능이다. */
		break;
	default:	/* [한국어] 그 밖의 조합 — 예컨대 중첩 부모와 더티 추적을 함께. */
		ret = -EOPNOTSUPP;	/* [한국어] 지원하지 않는다. */
		goto err_free;	/* [한국어] 잡아 둔 것을 되돌리고 나간다. */
	}

	smmu_domain->domain.type = IOMMU_DOMAIN_UNMANAGED;	/* [한국어] 매핑을 코어가 아니라 사용자가 직접 건다는 뜻. */
	smmu_domain->domain.ops = arm_smmu_ops.default_domain_ops;	/* [한국어] map/unmap 을 가진 완전한 연산표를 건다. */
	ret = arm_smmu_domain_finalise(smmu_domain, smmu, flags);	/* [한국어] 페이지 테이블을 짓고 번호를 배정한다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		goto err_free;
	return &smmu_domain->domain;	/* [한국어] 코어가 아는 형태로 돌려준다. */

err_free:	/* [한국어] 어느 단계에서 실패했든 여기로 온다. */
	arm_smmu_domain_free(smmu_domain);	/* [한국어] 껍데기와 배열을 놓는다. */
	return ERR_PTR(ret);	/* [한국어] 실패 이유를 오류 포인터로 감싸 돌려준다. */
}

/*
 * [한국어]
 * arm_smmu_map_pages - 그 도메인에 매핑을 건다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 입력 주소.
 * @paddr: 이어 줄 물리 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @prot: 접근 권한.
 * @gfp: 표를 잡을 때 쓸 할당 플래그.
 * @mapped: 실제로 매핑된 바이트 수를 적어 줄 자리.
 * @return: 0 성공, 음수 오류.
 *
 * io-pgtable 에 그대로 넘기는 얇은 껍데기다. 이 드라이버는 페이지 테이블을
 * 직접 짓지 않으므로, 매핑을 거는 일에 할 말이 없다.
 *
 * 표가 없는 경우를 검사하는 이유는, 도메인이 아직 마무리되지 않았거나
 * SVA 처럼 자기 테이블을 갖지 않는 종류일 수 있기 때문이다.
 *
 * 실행 컨텍스트: 매핑 경로. 원자적 문맥일 수도 있다(gfp 가 알려 준다).
 *
 * 호출 체인:
 *   iommu_map() → domain_ops->map_pages = [이 함수] → io-pgtable
 */
static int arm_smmu_map_pages(struct iommu_domain *domain, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int prot, gfp_t gfp, size_t *mapped)
{
	struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;	/* [한국어] 실제 테이블을 다루는 연산표. */

	if (!ops)	/* [한국어] 아직 테이블이 없는 도메인이면. */
		return -ENODEV;

	return ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, gfp, mapped);	/* [한국어] 그대로 넘긴다 — 무효화는 필요할 때 io-pgtable 이 우리를 되부른다. */
}

/*
 * [한국어]
 * arm_smmu_unmap_pages - 그 도메인의 매핑을 푼다
 *
 * @domain: 대상 도메인.
 * @iova: 풀 입력 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @gather: 무효화를 모아 둘 자리.
 * @return: 실제로 푼 바이트 수.
 *
 * 매핑을 거는 쪽과 마찬가지로 io-pgtable 에 넘긴다. gather 를 함께 넘기는
 * 것이 요점 — io-pgtable 이 우리 tlb_add_page 갈고리를 통해 그 자리에
 * 주소를 쌓아 두고, 나중에 iotlb_sync 가 한 번에 무효화한다.
 *
 * 실행 컨텍스트: 매핑 해제 경로. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   iommu_unmap() → domain_ops->unmap_pages = [이 함수] → io-pgtable
 */
static size_t arm_smmu_unmap_pages(struct iommu_domain *domain, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct io_pgtable_ops *ops = smmu_domain->pgtbl_ops;	/* [한국어] 테이블 연산표. */

	if (!ops)	/* [한국어] 테이블이 없으면. */
		return 0;	/* [한국어] 푼 것이 없다. */

	return ops->unmap_pages(ops, iova, pgsize, pgcount, gather);	/* [한국어] 그대로 넘긴다 — 무효화는 gather 에 쌓였다가 나중에 한꺼번에 나간다. */
}

/*
 * [한국어]
 * arm_smmu_flush_iotlb_all - 그 도메인의 변환 캐시를 통째로 비운다
 *
 * @domain: 대상 도메인.
 *
 * 코어가 "이 도메인의 캐시를 다 비워 달라"고 할 때 부른다. 아직 하드웨어에
 * 묶이지 않은 도메인이면 비울 것도 없다.
 *
 * 실행 컨텍스트: 코어의 요청. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   iommu_flush_iotlb_all() → domain_ops 갈고리 = [이 함수]
 *     → arm_smmu_tlb_inv_context()
 */
static void arm_smmu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */

	if (smmu_domain->smmu)	/* [한국어] 하드웨어에 묶인 도메인일 때만 — 아직 마무리 전이면 캐시에 아무것도 없다. */
		arm_smmu_tlb_inv_context(smmu_domain);	/* [한국어] 통째로 비운다. */
}

/*
 * [한국어]
 * arm_smmu_iotlb_sync - 모아 둔 무효화를 한 번에 낸다
 *
 * @domain: 대상 도메인.
 * @gather: 매핑을 풀며 쌓아 둔 구간.
 *
 * 매핑을 풀 때마다 무효화를 내지 않고 모아 두었다가, 여기서 한 번에
 * 처리한다. 그 사이에 인접한 구간들이 하나로 합쳐져 명령 수가 크게 준다.
 *
 * leaf 를 참으로 넘기는 것이 요점이다. 매핑만 풀었을 뿐 중간 단계 표는
 * 그대로이므로, 걸어온 경로의 캐시까지 버릴 필요가 없다.
 *
 * 실행 컨텍스트: 매핑 해제 경로의 끝. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   iommu_iotlb_sync() → domain_ops 갈고리 = [이 함수]
 *     → arm_smmu_domain_inv_range()
 */
static void arm_smmu_iotlb_sync(struct iommu_domain *domain,
				struct iommu_iotlb_gather *gather)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */

	if (!gather->pgsize)	/* [한국어] 쌓인 것이 없으면. */
		return;	/* [한국어] 낼 명령도 없다. */

	arm_smmu_domain_inv_range(smmu_domain, gather->start,	/* [한국어] 쌓인 구간을 통째로 무효화한다. */
				  gather->end - gather->start + 1,	/* [한국어] 끝 주소가 포함이라 하나를 더해 길이를 구한다. */
				  gather->pgsize, true);	/* [한국어] leaf=true — 매핑만 풀렸으므로 마지막 단계만 버리면 된다. */
}

/*
 * [한국어]
 * arm_smmu_iova_to_phys - 입력 주소가 어느 물리 주소로 번역되는지 알려 준다
 *
 * @domain: 대상 도메인.
 * @iova: 물어볼 입력 주소.
 * @return: 번역된 물리 주소, 매핑이 없으면 0.
 *
 * 페이지 테이블을 소프트웨어로 걸어 답한다 — 하드웨어의 주소 변환 기능을
 * 쓰지 않는다. 진단이나 일부 드라이버가 매핑을 확인할 때 쓴다.
 *
 * 실행 컨텍스트: 어디서나. 원자적 문맥일 수 있다.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops 갈고리 = [이 함수] → io-pgtable
 */
static phys_addr_t
arm_smmu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct io_pgtable_ops *ops = to_smmu_domain(domain)->pgtbl_ops;	/* [한국어] 테이블 연산표. */

	if (!ops)	/* [한국어] 테이블이 없으면. */
		return 0;	/* [한국어] 번역할 수 없다. */

	return ops->iova_to_phys(ops, iova);	/* [한국어] io-pgtable 이 표를 걸어 답한다. */
}

/* [한국어] 아래에서 이 드라이버를 찾는 데 쓰므로 미리 알려 둔다 — 정의는 파일 끝에 있다. */
static struct platform_driver arm_smmu_driver;

/*
 * [한국어]
 * arm_smmu_get_by_fwnode - 펌웨어 노드로 SMMU 장치를 찾는다
 *
 * @fwnode: 펌웨어가 이 SMMU 를 가리키는 노드.
 * @return: 그 SMMU, 아직 프로브되지 않았으면 NULL.
 *
 * 장치가 프로브될 때 펌웨어는 "이 장치는 저 SMMU 뒤에 있다"고 노드로
 * 알려 준다. 그 노드에서 실제 드라이버 상태를 되찾는 함수다.
 *
 * put_device 를 곧바로 부르는 것이 눈에 띈다. 찾기 함수가 참조를 올려
 * 주지만, SMMU 장치는 이 시점에 사라질 수 없으므로(장치가 그 뒤에
 * 프로브되는 중이다) 참조를 들고 있을 필요가 없다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_probe_device() → [이 함수] → bus_find_device_by_fwnode()
 */
static
struct arm_smmu_device *arm_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev = bus_find_device_by_fwnode(&platform_bus_type, fwnode);	/* [한국어] 플랫폼 버스에서 그 노드를 가진 장치를 찾는다. */

	put_device(dev);	/* [한국어] 찾기가 올려 준 참조를 곧바로 놓는다 — SMMU 는 이 시점에 사라지지 않는다. */
	return dev ? dev_get_drvdata(dev) : NULL;	/* [한국어] 드라이버가 붙여 둔 상태를 꺼낸다. 아직 프로브 전이면 NULL 이다. */
}

/*
 * [한국어]
 * arm_smmu_sid_in_range - 그 스트림 번호를 표가 담을 수 있는가
 *
 * @smmu: 대상 하드웨어.
 * @sid: 검사할 번호.
 * @return: 담을 수 있으면 참.
 *
 * 펌웨어가 적어 준 번호가 표의 범위를 넘으면 그 장치는 IOMMU 뒤에 설 수
 * 없다. 2단계 표에서는 위쪽 첨자가 범위 안인지 보고, 평면 표에서는 번호
 * 자체가 항목 수보다 작은지 본다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_init_sid_strtab() → [이 함수]
 */
static bool arm_smmu_sid_in_range(struct arm_smmu_device *smmu, u32 sid)
{
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)	/* [한국어] 2단계 표라면. */
		return arm_smmu_strtab_l1_idx(sid) < smmu->strtab_cfg.l2.num_l1_ents;	/* [한국어] 위쪽 첨자가 표 안이어야 한다. */
	return sid < smmu->strtab_cfg.linear.num_ents;	/* [한국어] 평면 표라면 번호가 곧 첨자다. */
}

/*
 * [한국어]
 * arm_smmu_init_sid_strtab - 그 스트림 번호를 쓸 준비를 한다
 *
 * @smmu: 대상 하드웨어.
 * @sid: 준비할 번호.
 * @return: 0 성공, -ERANGE 범위 밖, -ENOMEM 메모리 부족.
 *
 * 범위를 검사하고, 2단계 표라면 그 구간의 아래쪽 표를 마련한다. 평면
 * 표는 프로브 때 통째로 잡혀 있어 할 일이 없다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_insert_master() → [이 함수] → arm_smmu_init_l2_strtab()
 */
static int arm_smmu_init_sid_strtab(struct arm_smmu_device *smmu, u32 sid)
{
	/* Check the SIDs are in range of the SMMU and our stream table */
	/* [한국어] (위 영어 주석 참고) 펌웨어 기술이 하드웨어 능력과 어긋날 수 있다. */
	if (!arm_smmu_sid_in_range(smmu, sid))
		return -ERANGE;	/* [한국어] 그 장치는 IOMMU 뒤에 설 수 없다. */

	/* Ensure l2 strtab is initialised */
	/* [한국어] (위 영어 주석 참고) 2단계 표는 쓰이는 구간만 게으르게 잡는다. */
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)
		return arm_smmu_init_l2_strtab(smmu, sid);	/* [한국어] 없으면 지금 잡는다. */

	return 0;	/* [한국어] 평면 표는 이미 통째로 잡혀 있다. */
}

/*
 * [한국어]
 * arm_smmu_stream_id_cmp - 스트림 번호 두 개를 견준다
 *
 * @_l: 왼쪽 번호의 주소.
 * @_r: 오른쪽 번호의 주소.
 * @return: 음수/0/양수.
 *
 * 장치의 스트림 번호 목록을 정렬하는 데 쓴다. 그 목록이 정렬되어 있어야
 * 무효화 목록도 정렬된 채로 만들어지고, 그래야 병합·해제가 성립한다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들지 않는다.
 *
 * 호출 체인:
 *   sort_nonatomic() → [이 함수]
 */
static int arm_smmu_stream_id_cmp(const void *_l, const void *_r)
{
	const typeof_member(struct arm_smmu_stream, id) *l = _l;	/* [한국어] 타입을 구조체 필드에서 따 와, 필드 타입이 바뀌어도 함께 따라가게 한다. */
	const typeof_member(struct arm_smmu_stream, id) *r = _r;	/* [한국어] 오른쪽 번호도 같은 방식으로 타입을 딴다. */

	return cmp_int(*l, *r);	/* [한국어] 오름차순. */
}

/*
 * [한국어]
 * arm_smmu_insert_master - 장치의 스트림 번호들을 등록한다
 *
 * @smmu: 대상 하드웨어.
 * @master: 등록할 장치.
 * @return: 0 성공, 음수 오류.
 *
 * 장치 프로브의 핵심이다. 하는 일이 넷이다 — 스트림 목록을 잡고,
 * 무효화 임시 배열을 잡고, 목록을 정렬하고, 각 번호를 rb 트리에 넣는다.
 *
 * 무효화 임시 배열의 크기 계산이 흥미롭다. 이 배열은 나중에 메모리를
 * 잡을 수 없는 자리에서 쓰이므로, 최악의 경우를 지금 계산해 잡아 두어야
 * 한다 — 기본 항목 둘(ASID 하나, 또는 VMID 와 중첩용 둘)에, ATS 를 쓸
 * 수 있는 장치면 스트림 수만큼 더한다.
 *
 * 목록을 정렬하는 것도 그 배열을 위해서다. 스트림 번호가 정렬되어 있어야
 * 그것으로 만든 무효화 목록도 정렬되고, 정렬된 배열끼리만 병합할 수 있다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_probe_device() → [이 함수]
 *     → arm_smmu_init_sid_strtab() → rb_find_add()
 */
static int arm_smmu_insert_master(struct arm_smmu_device *smmu,
				  struct arm_smmu_master *master)
{
	int i;	/* [한국어] 스트림 반복자. */
	int ret = 0;	/* [한국어] 결과. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(master->dev);	/* [한국어] 펌웨어가 적어 준 스트림 번호들. */
	bool ats_supported = dev_is_pci(master->dev) &&	/* [한국어] 이 장치가 ATS 를 쓸 수 있는가 — 임시 배열 크기를 정하는 데 쓴다. */
			     pci_ats_supported(to_pci_dev(master->dev));

	master->streams = kzalloc_objs(*master->streams, fwspec->num_ids);	/* [한국어] 스트림 번호마다 하나씩. */
	if (!master->streams)	/* [한국어] 스트림 목록을 못 잡았으면. */
		return -ENOMEM;
	master->num_streams = fwspec->num_ids;	/* [한국어] 개수를 기록한다. */

	if (!ats_supported) {	/* [한국어] ATS 를 못 쓰는 장치라면. */
		/* Base case has 1 ASID entry or maximum 2 VMID entries */
		/* [한국어] (위 영어 주석 참고) 1단계면 ASID 항목 하나, 2단계 중첩이면
		 * VMID 항목과 1단계 정리 항목 둘 — 최악이 둘이다. */
		master->build_invs = arm_smmu_invs_alloc(2);
	} else {	/* [한국어] ATS 를 쓸 수 있다면. */
		/* ATS case adds num_ids of entries, on top of the base case */
		/* [한국어] (위 영어 주석 참고) 스트림 번호마다 장치 캐시 무효화 항목이
		 * 하나씩 더 붙는다. 이 배열은 실패할 수 없는 자리에서 쓰이므로
		 * 최악의 크기를 지금 잡아 두어야 한다. */
		master->build_invs = arm_smmu_invs_alloc(2 + fwspec->num_ids);
	}
	if (!master->build_invs) {	/* [한국어] 메모리가 없으면. */
		kfree(master->streams);	/* [한국어] 방금 잡은 목록을 되돌린다. */
		return -ENOMEM;	/* [한국어] 메모리가 없어 더 진행할 수 없다. */
	}

	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 펌웨어가 준 번호를 하나씩. */
		struct arm_smmu_stream *new_stream = &master->streams[i];	/* [한국어] 담을 자리. */

		new_stream->id = fwspec->ids[i];	/* [한국어] 그 번호. */
		new_stream->master = master;	/* [한국어] 폴트가 났을 때 이 번호로 장치를 되찾는 길. */
	}

	/* Put the ids into order for sorted to_merge/to_unref arrays */
	/* [한국어] (위 영어 주석 참고) 이 목록이 정렬되어 있어야 그것으로 만든
	 * 무효화 목록도 정렬된다. 정렬되지 않은 배열끼리는 병합할 수 없다. */
	sort_nonatomic(master->streams, master->num_streams,	/* [한국어] 잠들 수 있는 문맥이라 비원자적 정렬을 쓴다. */
		       sizeof(master->streams[0]), arm_smmu_stream_id_cmp,
		       NULL);

	mutex_lock(&smmu->streams_mutex);	/* [한국어] 트리를 고치는 동안 막는다 — 폴트 처리도 이 트리를 읽는다. */
	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 정렬된 순서로 하나씩 등록한다. */
		struct arm_smmu_stream *new_stream = &master->streams[i];	/* [한국어] 등록할 스트림. */
		struct rb_node *existing;	/* [한국어] 이미 그 번호가 있었다면 그 노드. */
		u32 sid = new_stream->id;	/* [한국어] 그 번호. */

		ret = arm_smmu_init_sid_strtab(smmu, sid);	/* [한국어] 표에 그 자리를 마련한다. */
		if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
			break;	/* [한국어] 범위 밖이거나 메모리가 없으면 되감는다. */

		/* Insert into SID tree */
		/* [한국어] (위 영어 주석 참고) 폴트가 났을 때 번호로 장치를 되찾는 트리. */
		existing = rb_find_add(&new_stream->node, &smmu->streams,	/* [한국어] 넣으면서 중복도 함께 확인한다. */
				       arm_smmu_streams_cmp_node);
		if (existing) {	/* [한국어] 이미 그 번호가 있었다면. */
			struct arm_smmu_master *existing_master =	/* [한국어] 누가 쓰고 있는지. */
				rb_entry(existing, struct arm_smmu_stream, node)
					->master;

			/* Bridged PCI devices may end up with duplicated IDs */
			/* [한국어] (위 영어 주석 참고) 브리지 뒤의 장치는 별칭 때문에 같은
			 * 번호를 여러 번 갖는다. 그것이 자기 자신이면 문제가 아니다. */
			if (existing_master == master)
				continue;	/* [한국어] 이미 등록했으므로 넘어간다. */

			dev_warn(master->dev,	/* [한국어] 다른 장치가 그 번호를 쓰고 있다면 둘의 DMA 를 구분할 수 없다. */
				 "Aliasing StreamID 0x%x (from %s) unsupported, expect DMA to be broken\n",
				 sid, dev_name(existing_master->dev));
			ret = -ENODEV;	/* [한국어] 이 장치를 IOMMU 뒤에 세울 수 없다. */
			break;
		}
	}

	if (ret) {	/* [한국어] 도중에 실패했다면 되감는다. */
		for (i--; i >= 0; i--)	/* [한국어] 이미 넣은 것부터 거꾸로. */
			rb_erase(&master->streams[i].node, &smmu->streams);	/* [한국어] 트리에서 뺀다. */
		kfree(master->streams);	/* [한국어] 목록을 놓는다. */
		kfree(master->build_invs);	/* [한국어] 임시 배열도 놓는다. */
	}
	mutex_unlock(&smmu->streams_mutex);	/* [한국어] 트리 조작이 끝났으니 락을 놓는다. */

	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_remove_master - 장치의 스트림 번호 등록을 되돌린다
 *
 * @master: 대상 장치.
 *
 * 트리에서 번호들을 빼고, 목록과 임시 배열을 놓는다. 이 뒤로 그 번호로
 * 폴트가 오면 장치를 찾지 못해 "등록되지 않은 스트림" 로그가 남는다.
 *
 * 실행 컨텍스트: 장치 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_release_device() → [이 함수] → rb_erase()
 */
static void arm_smmu_remove_master(struct arm_smmu_master *master)
{
	int i;	/* [한국어] 스트림 반복자. */
	struct arm_smmu_device *smmu = master->smmu;	/* [한국어] 트리를 가진 하드웨어. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(master->dev);	/* [한국어] 번호 개수를 여기서 본다. */

	if (!smmu || !master->streams)	/* [한국어] 등록에 실패해 목록이 없는 장치일 수 있다. */
		return;

	mutex_lock(&smmu->streams_mutex);	/* [한국어] 폴트 처리가 이 트리를 읽는다. */
	for (i = 0; i < fwspec->num_ids; i++)	/* [한국어] 등록했던 번호를 모두. */
		rb_erase(&master->streams[i].node, &smmu->streams);	/* [한국어] 트리에서 뺀다. */
	mutex_unlock(&smmu->streams_mutex);	/* [한국어] 트리 조작이 끝났으니 락을 놓는다. */

	kfree(master->streams);	/* [한국어] 목록을 놓는다. */
	kfree(master->build_invs);	/* [한국어] 무효화 임시 배열도 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_probe_device - 이 장치를 IOMMU 뒤에 세울 준비를 한다
 *
 * @dev: 프로브할 장치.
 * @return: 이 장치를 담당할 iommu 장치, 실패하면 ERR_PTR.
 *
 * 코어가 "이 장치가 당신 뒤에 있다"고 알려 올 때 부른다. 장치별 상태를
 * 만들고, 스트림 번호를 등록하고, PASID 와 멈춤 능력을 정한다.
 *
 * PASID 를 ATS 보다 먼저 켜야 한다는 주석이 중요하다 — PCI 규격이
 * "ATS 가 켜진 상태에서 PASID 설정을 바꾸면 동작이 정의되지 않는다"고
 * 못 박고 있어, 순서를 어기면 하드웨어마다 다르게 오동작한다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu 코어 → iommu_ops->probe_device = [이 함수]
 *     → arm_smmu_insert_master() → arm_smmu_enable_pasid()
 */
static struct iommu_device *arm_smmu_probe_device(struct device *dev)
{
	int ret;	/* [한국어] 중간 결과. */
	struct arm_smmu_device *smmu;	/* [한국어] 이 장치를 담당할 하드웨어. */
	struct arm_smmu_master *master;	/* [한국어] 만들 장치별 상태. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 펌웨어가 적어 준 정보. */

	if (WARN_ON_ONCE(dev_iommu_priv_get(dev)))	/* [한국어] 이미 프로브된 장치를 또 프로브하면 코어 쪽 버그다. */
		return ERR_PTR(-EBUSY);

	smmu = arm_smmu_get_by_fwnode(fwspec->iommu_fwnode);	/* [한국어] 펌웨어가 가리킨 SMMU 를 찾는다. */
	if (!smmu)	/* [한국어] 아직 프로브되지 않았다면. */
		return ERR_PTR(-ENODEV);	/* [한국어] 코어가 나중에 다시 시도한다. */

	master = kzalloc_obj(*master);	/* [한국어] 장치별 상태를 잡는다. */
	if (!master)	/* [한국어] 장치별 상태를 못 잡았으면. */
		return ERR_PTR(-ENOMEM);

	master->dev = dev;	/* [한국어] 되짚어 갈 수 있게. */
	master->smmu = smmu;	/* [한국어] 어느 하드웨어 뒤에 있는지. */
	dev_iommu_priv_set(dev, master);	/* [한국어] 장치에 붙여 둔다 — 이후 모든 경로가 이 포인터로 상태를 꺼낸다. */

	ret = arm_smmu_insert_master(smmu, master);	/* [한국어] 스트림 번호를 등록하고 표 자리를 마련한다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		goto err_free_master;

	device_property_read_u32(dev, "pasid-num-bits", &master->ssid_bits);	/* [한국어] 펌웨어가 PASID 폭을 적어 두었을 수 있다 — 없으면 값이 그대로 0 이다. */
	master->ssid_bits = min(smmu->ssid_bits, master->ssid_bits);	/* [한국어] 하드웨어 한계를 넘을 수는 없다. */

	/*
	 * Note that PASID must be enabled before, and disabled after ATS:
	 * PCI Express Base 4.0r1.0 - 10.5.1.3 ATS Control Register
	 *
	 *   Behavior is undefined if this bit is Set and the value of the PASID
	 *   Enable, Execute Requested Enable, or Privileged Mode Requested bits
	 *   are changed.
	 */
	/* [한국어] (위 영어 주석과 인용 참고) PCI 규격이 "ATS 가 켜진 채 PASID
	 * 설정을 바꾸면 동작이 정의되지 않는다"고 못 박고 있다. 그래서 켤 때는
	 * PASID 를 먼저, 끌 때는 ATS 를 먼저 다뤄야 한다. */
	arm_smmu_enable_pasid(master);	/* [한국어] 실패해도 그냥 진행한다 — PASID 없이도 장치는 동작한다. */

	if (!(smmu->features & ARM_SMMU_FEAT_2_LVL_CDTAB))	/* [한국어] 2단계 문맥 표를 지원하지 않는 하드웨어라면. */
		master->ssid_bits = min_t(u8, master->ssid_bits,	/* [한국어] 평면 표로 감당할 수 있는 만큼으로 줄인다. */
					  CTXDESC_LINEAR_CDMAX);

	if ((smmu->features & ARM_SMMU_FEAT_STALLS &&	/* [한국어] 하드웨어가 멈춤을 지원하고. */
	     device_property_read_bool(dev, "dma-can-stall")) ||	/* [한국어] 펌웨어가 이 장치는 멈춰도 된다고 적었거나. */
	    smmu->features & ARM_SMMU_FEAT_STALL_FORCE)	/* [한국어] 하드웨어가 멈춤을 강제한다면. */
		master->stall_enabled = true;	/* [한국어] 폴트 때 트랜잭션을 붙잡아 둘 수 있다 — SVA 의 전제 조건이다. */

	if (dev_is_pci(dev)) {	/* [한국어] PCI 장치라면. */
		unsigned int stu = __ffs(smmu->pgsize_bitmap);	/* [한국어] 최소 변환 단위. */

		pci_prepare_ats(to_pci_dev(dev), stu);	/* [한국어] ATS 를 켤 준비만 해 둔다 — 실제로 켜는 것은 붙이기 때다. */
	}

	return &smmu->iommu;	/* [한국어] 코어에게 "이 SMMU 가 담당한다"고 알린다. */

err_free_master:	/* [한국어] 등록에 실패했을 때. */
	kfree(master);	/* [한국어] 상태를 놓는다 — 장치에 붙여 둔 포인터는 코어가 정리한다. */
	return ERR_PTR(ret);	/* [한국어] 실패 이유를 오류 포인터로 감싸 돌려준다. */
}

/*
 * [한국어]
 * arm_smmu_release_device - 장치가 떠날 때 자원을 거둔다
 *
 * @dev: 떠나는 장치.
 *
 * 프로브에서 잡은 것을 역순으로 되돌린다. 이 시점에는 이미 차단 도메인이
 * 붙어 있어 그 장치의 DMA 는 모두 막힌 상태다.
 *
 * 폴트 참조가 남아 있으면 경고한다 — 붙이기와 떼기의 짝이 맞지 않았다는
 * 뜻이고, 그대로 두면 사라진 장치의 폴트를 처리하려 들게 된다.
 *
 * 실행 컨텍스트: 장치 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu 코어 → iommu_ops->release_device = [이 함수]
 *     → arm_smmu_remove_master() → arm_smmu_free_cd_tables()
 */
static void arm_smmu_release_device(struct device *dev)
{
	struct arm_smmu_master *master = dev_iommu_priv_get(dev);	/* [한국어] 장치별 상태. */

	WARN_ON(master->iopf_refcount);	/* [한국어] 붙이기·떼기의 짝이 맞지 않았다는 뜻 — 사라진 장치의 폴트를 처리하려 들게 된다. */

	arm_smmu_disable_pasid(master);	/* [한국어] PASID 를 끈다 — ATS 는 이미 떼기 경로에서 꺼졌다. */
	arm_smmu_remove_master(master);	/* [한국어] 스트림 등록을 되돌린다. */
	if (arm_smmu_cdtab_allocated(&master->cd_table))	/* [한국어] 문맥 표를 잡았던 장치라면. */
		arm_smmu_free_cd_tables(master);	/* [한국어] 그 표를 놓는다. */
	kfree(master);	/* [한국어] 상태를 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_read_and_clear_dirty - 바뀐 페이지를 읽어 내고 표시를 지운다
 *
 * @domain: 대상 도메인.
 * @iova: 훑을 구간의 시작.
 * @size: 그 길이.
 * @flags: 읽기만 할지, 지우기도 할지.
 * @dirty: 결과를 담을 비트맵.
 * @return: 0 성공, 음수 오류.
 *
 * 마이그레이션에서 어느 페이지가 바뀌었는지 알아낼 때 쓴다. 하드웨어가
 * 페이지 테이블에 표시해 둔 더티 비트를 io-pgtable 이 훑어 비트맵으로
 * 옮겨 주고, 동시에 그 표시를 지워 다음 회차를 준비한다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommufd → dirty_ops 갈고리 = [이 함수] → io-pgtable
 */
static int arm_smmu_read_and_clear_dirty(struct iommu_domain *domain,
					 unsigned long iova, size_t size,
					 unsigned long flags,
					 struct iommu_dirty_bitmap *dirty)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct io_pgtable_ops *ops = smmu_domain->pgtbl_ops;	/* [한국어] 표를 훑을 연산표. */

	return ops->read_and_clear_dirty(ops, iova, size, flags, dirty);	/* [한국어] 표를 걸으며 더티 비트를 모으고 지운다 — 지우기에 따르는 무효화도 io-pgtable 이 우리를 되불러 처리한다. */
}

/*
 * [한국어]
 * arm_smmu_set_dirty_tracking - 더티 추적을 켜고 끈다
 *
 * @domain: 대상 도메인 (쓰지 않는다).
 * @enabled: 켤 것인가 (쓰지 않는다).
 * @return: 항상 0.
 *
 * (아래 영어 주석 참고) 이 하드웨어에서는 더티 추적이 페이지 테이블
 * 설정으로 정해져 언제나 켜져 있다. 그리고 코어가 이 함수를 부르기 전에
 * 비트맵을 지우므로, 여기서 따로 할 일이 없다.
 *
 * 실행 컨텍스트: iommufd ioctl. 잠들지 않는다.
 *
 * 호출 체인:
 *   iommufd → dirty_ops 갈고리 = [이 함수]
 */
static int arm_smmu_set_dirty_tracking(struct iommu_domain *domain,
				       bool enabled)
{
	/*
	 * Always enabled and the dirty bitmap is cleared prior to
	 * set_dirty_tracking().
	 */
	/* [한국어] (위 영어 주석 참고) 더티 추적은 도메인을 만들 때 페이지 테이블
	 * 설정으로 정해지고 그 뒤로는 바뀌지 않는다. 비트맵 지우기도 코어가
	 * 이 함수를 부르기 전에 끝내므로 할 일이 없다. */
	return 0;
}

/*
 * [한국어]
 * arm_smmu_device_group - 이 장치가 속할 격리 그룹을 정한다
 *
 * @dev: 대상 장치.
 * @return: 그 그룹.
 *
 * IOMMU 그룹은 "서로 격리할 수 없는 장치들의 묶음"이다. 같은 그룹의
 * 장치들은 반드시 같은 도메인에 붙어야 한다.
 *
 * PCI 는 별칭과 브리지 구조 때문에 격리 경계가 복잡해 코어의 전용
 * 계산을 쓰고, 그 밖의 장치는 하나씩 따로 둔다.
 *
 * 주석이 밝히듯 스트림 번호를 공유하는 장치는 지원하지 않는다 —
 * 번호로 장치를 되찾을 수 없게 되기 때문이다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu 코어 → iommu_ops->device_group = [이 함수]
 */
static struct iommu_group *arm_smmu_device_group(struct device *dev)
{
	struct iommu_group *group;	/* [한국어] 정한 그룹. */

	/*
	 * We don't support devices sharing stream IDs other than PCI RID
	 * aliases, since the necessary ID-to-device lookup becomes rather
	 * impractical given a potential sparse 32-bit stream ID space.
	 */
	/* [한국어] (위 영어 주석 참고) 스트림 번호를 공유하는 장치를 지원하려면
	 * 번호 하나에 장치 여럿을 매다는 표가 필요한데, 번호 공간이 32비트로
	 * 성기게 흩어져 있어 현실적이지 않다. PCI 별칭만 예외로 다룬다. */
	if (dev_is_pci(dev))	/* [한국어] PCI 는 별칭과 브리지 때문에 격리 경계가 복잡하다. */
		group = pci_device_group(dev);	/* [한국어] 코어의 전용 계산에 맡긴다. */
	else
		group = generic_device_group(dev);	/* [한국어] 그 밖의 장치는 하나씩 따로 둔다. */

	return group;	/* [한국어] 정한 그룹을 코어에 돌려준다. */
}

/*
 * [한국어]
 * arm_smmu_of_xlate - 장치 트리의 iommus 속성을 스트림 번호로 옮긴다
 *
 * @dev: 대상 장치.
 * @args: 장치 트리가 적어 준 인자들.
 * @return: 0 성공, 음수 오류.
 *
 * 장치 트리에서 "iommus = <&smmu 0x1234>" 같은 형태로 적힌 스트림 번호를
 * 코어의 목록에 더한다. 이 드라이버는 인자를 하나만 쓰므로 그대로 넘긴다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   of_iommu_configure() → iommu_ops->of_xlate = [이 함수]
 */
static int arm_smmu_of_xlate(struct device *dev,
			     const struct of_phandle_args *args)
{
	return iommu_fwspec_add_ids(dev, args->args, 1);	/* [한국어] 인자 하나가 곧 스트림 번호 하나다. */
}

/*
 * [한국어]
 * arm_smmu_get_resv_regions - 매핑하면 안 되는 주소 구간을 알린다
 *
 * @dev: 대상 장치.
 * @head: 구간들을 담을 목록.
 *
 * MSI 를 쓰는 장치는 특정 주소로 쓰기를 보내 인터럽트를 일으키는데,
 * 그 주소가 일반 매핑에 쓰이면 인터럽트가 엉뚱한 메모리 쓰기가 된다.
 * 그래서 그 구간을 예약으로 알려 코어가 비켜 가게 한다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu 코어 → iommu_ops->get_resv_regions = [이 함수]
 *     → iommu_dma_get_resv_regions()
 */
static void arm_smmu_get_resv_regions(struct device *dev,
				      struct list_head *head)
{
	struct iommu_resv_region *region;	/* [한국어] 알릴 구간. */
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;	/* [한국어] MSI 는 쓰기뿐이고, 실행할 것도 없으며, 장치 레지스터 영역이다. */

	region = iommu_alloc_resv_region(MSI_IOVA_BASE, MSI_IOVA_LENGTH,	/* [한국어] 소프트웨어가 정한 MSI 창 — 하드웨어가 정한 주소가 아니라 커널이 고른 자리다. */
					 prot, IOMMU_RESV_SW_MSI, GFP_KERNEL);
	if (!region)	/* [한국어] 메모리가 없으면 알리지 못한다. */
		return;

	list_add_tail(&region->list, head);	/* [한국어] 코어에게 넘길 목록에 담는다. */

	iommu_dma_get_resv_regions(dev, head);	/* [한국어] 펌웨어가 따로 적어 둔 예약 구간도 함께 더한다. */
}

/*
 * HiSilicon PCIe tune and trace device can be used to trace TLP headers on the
 * PCIe link and save the data to memory by DMA. The hardware is restricted to
 * use identity mapping only.
 */
/* [한국어] (위 영어 주석 참고) HiSilicon 의 PCIe 추적 장치를 알아보는 매크로.
 *
 * 이 장치는 링크를 지나는 패킷 머리를 메모리에 기록하는데, 하드웨어가
 * 변환된 주소를 다루지 못해 통과 매핑만 쓸 수 있다. 그래서 아래에서
 * 기본 도메인을 통과로 강제한다. */
#define IS_HISI_PTT_DEVICE(pdev)	((pdev)->vendor == PCI_VENDOR_ID_HUAWEI && \
					 (pdev)->device == 0xa12e)	/* [한국어] 그 제품 번호까지 맞아야 이 장치다. */

/*
 * [한국어]
 * arm_smmu_def_domain_type - 이 장치의 기본 도메인 종류를 정한다
 *
 * @dev: 대상 장치.
 * @return: 강제할 종류, 강제하지 않으면 0.
 *
 * 대부분의 장치는 코어가 정한 기본값을 쓰지만, 변환된 주소를 다루지
 * 못하는 장치는 반드시 통과 도메인이어야 한다. 그런 예외를 여기서 짚는다.
 *
 * 실행 컨텍스트: 장치 프로브. 잠들지 않는다.
 *
 * 호출 체인:
 *   iommu 코어 → iommu_ops->def_domain_type = [이 함수]
 */
static int arm_smmu_def_domain_type(struct device *dev)
{
	if (dev_is_pci(dev)) {	/* [한국어] 예외 목록이 모두 PCI 장치다. */
		struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] PCI 장치로 되짚어 제조사와 제품을 본다. */

		if (IS_HISI_PTT_DEVICE(pdev))	/* [한국어] 그 추적 장치라면. */
			return IOMMU_DOMAIN_IDENTITY;	/* [한국어] 통과 도메인을 강제한다 — 변환된 주소를 못 다루기 때문이다. */
	}

	return 0;	/* [한국어] 강제하지 않는다 — 코어가 알아서 정한다. */
}

/* [한국어] iommu 코어에 등록하는 이 드라이버의 연산표.
 *
 * 코어가 이 표만 보고 드라이버를 다루므로, 여기 없는 기능은 지원하지
 * 않는 것이 된다. 도메인 종류별 갈고리와 장치 수명 갈고리, 그리고
 * 페이지 테이블 도메인이 쓰는 기본 연산표가 함께 들어 있다. */
static const struct iommu_ops arm_smmu_ops = {
	.identity_domain	= &arm_smmu_identity_domain,	/* [한국어] 통과 도메인 — 정적 인스턴스를 그대로 준다. */
	.blocked_domain		= &arm_smmu_blocked_domain,	/* [한국어] 차단 도메인. */
	.release_domain		= &arm_smmu_blocked_domain,	/* [한국어] 장치를 놓을 때 붙일 도메인 — 차단이 가장 안전하다. */
	.capable		= arm_smmu_capable,	/* [한국어] 능력 조회. */
	.hw_info		= arm_smmu_hw_info,	/* [한국어] 하드웨어 정보 노출 (iommufd 를 끄면 NULL 이 된다). */
	.domain_alloc_sva       = arm_smmu_sva_domain_alloc,	/* [한국어] SVA 도메인 할당 (SVA 를 끄면 NULL). */
	.domain_alloc_paging_flags = arm_smmu_domain_alloc_paging_flags,	/* [한국어] 페이지 테이블 도메인 할당. */
	.probe_device		= arm_smmu_probe_device,	/* [한국어] 장치를 IOMMU 뒤에 세운다. */
	.release_device		= arm_smmu_release_device,	/* [한국어] 그 자원을 거둔다. */
	.device_group		= arm_smmu_device_group,	/* [한국어] 격리 그룹을 정한다. */
	.of_xlate		= arm_smmu_of_xlate,	/* [한국어] 장치 트리의 스트림 번호를 옮긴다. */
	.get_resv_regions	= arm_smmu_get_resv_regions,	/* [한국어] 매핑하면 안 될 구간을 알린다. */
	.page_response		= arm_smmu_page_response,	/* [한국어] 폴트에 대한 답을 하드웨어에 돌려준다. */
	.def_domain_type	= arm_smmu_def_domain_type,	/* [한국어] 통과를 강제해야 하는 장치를 짚는다. */
	.get_viommu_size	= arm_smmu_get_viommu_size,	/* [한국어] 중첩 변환을 켤 수 있는지 판단한다 (iommufd 전용). */
	.viommu_init		= arm_vsmmu_init,	/* [한국어] 가상 SMMU 를 초기화한다 (iommufd 전용). */
	.user_pasid_table	= 1,	/* [한국어] 사용자 공간이 문맥 서술자 표를 직접 관리할 수 있다는 표시 — 중첩 변환의 전제다. */
	.owner			= THIS_MODULE,	/* [한국어] 모듈이 쓰이는 동안 내려가지 않게 한다. */
	.default_domain_ops = &(const struct iommu_domain_ops) {	/* [한국어] 페이지 테이블 도메인이 쓰는 연산표 — 익명으로 여기 박아 둔다. */
		.attach_dev		= arm_smmu_attach_dev,	/* [한국어] 장치를 붙인다. */
		.enforce_cache_coherency = arm_smmu_enforce_cache_coherency,	/* [한국어] 캐시 우회를 막을 수 있는지 확인하고 기록한다. */
		.set_dev_pasid		= arm_smmu_s1_set_dev_pasid,	/* [한국어] PASID 하나를 붙인다. */
		.map_pages		= arm_smmu_map_pages,	/* [한국어] 매핑을 건다. */
		.unmap_pages		= arm_smmu_unmap_pages,	/* [한국어] 매핑을 푼다. */
		.flush_iotlb_all	= arm_smmu_flush_iotlb_all,	/* [한국어] 캐시를 통째로 비운다. */
		.iotlb_sync		= arm_smmu_iotlb_sync,	/* [한국어] 모아 둔 무효화를 낸다. */
		.iova_to_phys		= arm_smmu_iova_to_phys,	/* [한국어] 주소 번역을 소프트웨어로 답한다. */
		.free			= arm_smmu_domain_free_paging,	/* [한국어] 도메인을 놓는다. */
	}
};

/* [한국어] 더티 페이지 추적 연산표.
 *
 * 마이그레이션에서 바뀐 페이지를 골라낼 때만 쓰이며, 더티 추적을 요청한
 * 도메인에만 걸린다. */
static struct iommu_dirty_ops arm_smmu_dirty_ops = {
	.read_and_clear_dirty	= arm_smmu_read_and_clear_dirty,	/* [한국어] 바뀐 페이지를 읽고 표시를 지운다. */
	.set_dirty_tracking     = arm_smmu_set_dirty_tracking,	/* [한국어] 켜고 끄기 — 이 하드웨어에서는 할 일이 없다. */
};

/* Probing and initialisation functions */
/*
 * [한국어]
 * arm_smmu_init_one_queue - 큐 하나의 링 버퍼를 잡고 레지스터 자리를 기록한다
 *
 * @smmu: 대상 하드웨어.
 * @q: 채울 큐.
 * @page: 그 큐의 포인터 레지스터가 놓인 MMIO 페이지.
 * @prod_off: 생산 포인터 레지스터의 오프셋.
 * @cons_off: 소비 포인터 레지스터의 오프셋.
 * @dwords: 항목 하나가 몇 워드인가.
 * @name: 로그에 찍을 이름.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 세 큐(명령·이벤트·PRI)가 모두 이 함수를 거쳐 같은 모양으로 만들어진다.
 *
 * 크기를 줄여 가며 다시 시도하는 것이 요점이다. 큰 연속 메모리를 못 잡는
 * 상황은 흔한데, 큐가 작아도 동작은 하므로 절반씩 줄여 본다. 한 페이지
 * 아래로는 줄이지 않는데, 그보다 작으면 어차피 할당이 실패할 이유가 없기
 * 때문이다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_queues()/tegra 확장 → [이 함수] → dmam_alloc_coherent()
 */
int arm_smmu_init_one_queue(struct arm_smmu_device *smmu,
			    struct arm_smmu_queue *q, void __iomem *page,
			    unsigned long prod_off, unsigned long cons_off,
			    size_t dwords, const char *name)
{
	size_t qsz;	/* [한국어] 잡을 링 버퍼의 바이트 크기. */

	do {
		qsz = ((1 << q->llq.max_n_shift) * dwords) << 3;	/* [한국어] 항목 수 × 워드 수 × 8바이트. */
		q->base = dmam_alloc_coherent(smmu->dev, qsz, &q->base_dma,	/* [한국어] 하드웨어도 읽고 쓰는 링이라 일관 메모리로 잡는다. */
					      GFP_KERNEL);
		if (q->base || qsz < PAGE_SIZE)	/* [한국어] 잡았거나, 더 줄여도 소용없을 만큼 작아졌으면. */
			break;

		q->llq.max_n_shift--;	/* [한국어] 절반으로 줄여 다시 시도한다 — 큐가 작아도 동작은 한다. */
	} while (1);

	if (!q->base) {	/* [한국어] 끝내 못 잡았으면. */
		dev_err(smmu->dev,	/* [한국어] 큐 메모리를 못 잡으면 그 큐를 쓸 수 없다. */
			"failed to allocate queue (0x%zx bytes) for %s\n",
			qsz, name);
		return -ENOMEM;	/* [한국어] 메모리가 없어 더 진행할 수 없다. */
	}

	if (!WARN_ON(q->base_dma & (qsz - 1))) {	/* [한국어] 링 버퍼는 자기 크기에 정렬되어야 한다 — 하드웨어가 아래 비트를 첨자로 쓰기 때문이다. */
		dev_info(smmu->dev, "allocated %u entries for %s\n",	/* [한국어] 실제로 몇 개를 잡았는지 남긴다 — 줄어들었을 수 있다. */
			 1 << q->llq.max_n_shift, name);
	}

	q->prod_reg	= page + prod_off;	/* [한국어] 생산 포인터 레지스터의 커널 주소. */
	q->cons_reg	= page + cons_off;	/* [한국어] 소비 포인터 레지스터. */
	q->ent_dwords	= dwords;	/* [한국어] 항목 크기 — 자리 계산에 쓴다. */

	q->q_base  = Q_BASE_RWA;	/* [한국어] 읽기·쓰기 할당 힌트를 켠다 — 하드웨어가 링을 캐시에 올려도 좋다는 뜻이다. */
	q->q_base |= q->base_dma & Q_BASE_ADDR_MASK;	/* [한국어] 링의 물리 주소. */
	q->q_base |= FIELD_PREP(Q_BASE_LOG2SIZE, q->llq.max_n_shift);	/* [한국어] 크기도 같은 값에 담는다 — 레지스터 하나로 둘을 알린다. */

	q->llq.prod = q->llq.cons = 0;	/* [한국어] 빈 큐로 시작한다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_cmdq_init - 명령 큐의 락 없는 삽입 장치를 준비한다
 *
 * @smmu: 대상 하드웨어.
 * @cmdq: 준비할 명령 큐.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 링 버퍼는 이미 잡혀 있고, 여기서는 그 위에 얹히는 유효 비트맵과 두
 * 원자 변수를 준비한다. 이것들이 있어야 여러 CPU 가 락 없이 자기 자리를
 * 차지하고 서로에게 완료를 알릴 수 있다.
 *
 * 비트맵을 0 으로 시작하는 것에 뜻이 있다. 첫 바퀴에서는 0 이 "아직 안
 * 쓰임"을 뜻하므로, 빈 큐가 자연스럽게 표현된다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_queues()/tegra 확장 → [이 함수] → devm_bitmap_zalloc()
 */
int arm_smmu_cmdq_init(struct arm_smmu_device *smmu,
		       struct arm_smmu_cmdq *cmdq)
{
	unsigned int nents = 1 << cmdq->q.llq.max_n_shift;	/* [한국어] 항목 수 — 비트 하나가 항목 하나에 대응한다. */

	atomic_set(&cmdq->owner_prod, 0);	/* [한국어] 아직 아무도 발표하지 않았다. */
	atomic_set(&cmdq->lock, 0);	/* [한국어] 아무도 락을 쥐지 않았다 — 0 이라야 배타 잠그기가 성립한다. */

	cmdq->valid_map = (atomic_long_t *)devm_bitmap_zalloc(smmu->dev, nents,	/* [한국어] 0 으로 시작해야 첫 바퀴에서 "아직 안 쓰임"이 된다. */
							      GFP_KERNEL);
	if (!cmdq->valid_map)	/* [한국어] 비트맵을 못 잡으면 락 없는 삽입이 성립하지 않는다. */
		return -ENOMEM;

	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_init_queues - 이 하드웨어의 큐들을 모두 마련한다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, 음수 오류.
 *
 * 명령 큐는 언제나 필요하고, 이벤트 큐도 마찬가지다. 폴트 처리 큐는
 * SVA 와 멈춤이 모두 있을 때만, PRI 큐는 하드웨어가 그 기능을 가졌을
 * 때만 만든다.
 *
 * 명령 큐가 첫 페이지에, 나머지가 둘째 페이지에 있는 것에 주목할 만하다.
 * 규격이 그렇게 나눠 둔 것이며, 결함이 있는 하드웨어에서는 둘 다 첫
 * 페이지를 가리키게 옵션으로 바꾼다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → arm_smmu_init_one_queue()
 */
static int arm_smmu_init_queues(struct arm_smmu_device *smmu)
{
	int ret;	/* [한국어] 중간 결과. */

	/* cmdq */
	/* [한국어] (위 영어 주석 참고) 명령 큐 — 없으면 하드웨어에게 아무 말도 할 수 없다. */
	ret = arm_smmu_init_one_queue(smmu, &smmu->cmdq.q, smmu->base,	/* [한국어] 명령 큐 레지스터는 첫 페이지에 있다. */
				      ARM_SMMU_CMDQ_PROD, ARM_SMMU_CMDQ_CONS,
				      CMDQ_ENT_DWORDS, "cmdq");
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	ret = arm_smmu_cmdq_init(smmu, &smmu->cmdq);	/* [한국어] 락 없는 삽입 장치를 얹는다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	/* evtq */
	/* [한국어] (위 영어 주석 참고) 이벤트 큐 — 폴트를 받을 길이다. */
	ret = arm_smmu_init_one_queue(smmu, &smmu->evtq.q, smmu->page1,	/* [한국어] 이벤트 큐 레지스터는 둘째 페이지에 있다. */
				      ARM_SMMU_EVTQ_PROD, ARM_SMMU_EVTQ_CONS,
				      EVTQ_ENT_DWORDS, "evtq");
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	if ((smmu->features & ARM_SMMU_FEAT_SVA) &&	/* [한국어] SVA 를 쓸 수 있고. */
	    (smmu->features & ARM_SMMU_FEAT_STALLS)) {	/* [한국어] 멈춤도 지원한다면. */
		smmu->evtq.iopf = iopf_queue_alloc(dev_name(smmu->dev));	/* [한국어] 폴트를 받아 페이지를 채워 줄 작업 큐를 만든다. */
		if (!smmu->evtq.iopf)	/* [한국어] 폴트 처리 큐를 못 잡았다면. */
			return -ENOMEM;
	}

	/* priq */
	/* [한국어] (위 영어 주석 참고) 페이지 요청 큐 — 있는 하드웨어에서만. */
	if (!(smmu->features & ARM_SMMU_FEAT_PRI))	/* [한국어] 그 기능이 없으면. */
		return 0;	/* [한국어] 만들지 않는다. */

	return arm_smmu_init_one_queue(smmu, &smmu->priq.q, smmu->page1,	/* [한국어] PRI 큐도 둘째 페이지. */
				       ARM_SMMU_PRIQ_PROD, ARM_SMMU_PRIQ_CONS,
				       PRIQ_ENT_DWORDS, "priq");
}

/*
 * [한국어]
 * arm_smmu_init_strtab_2lvl - 2단계 스트림 표의 위쪽 표를 마련한다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 스트림 번호 공간이 넓으면 평면 표로는 감당할 수 없다 — 번호 하나당
 * 64바이트이므로 20비트만 되어도 64MB 다. 그래서 위쪽 표만 잡아 두고
 * 아래쪽은 실제로 쓰이는 구간만 게으르게 잡는다.
 *
 * 위쪽 표조차 너무 커지면 상한으로 자르는데, 그때 경고를 남긴다 —
 * 그 위의 스트림 번호를 가진 장치는 IOMMU 뒤에 설 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_strtab() → [이 함수] → dmam_alloc_coherent()
 */
static int arm_smmu_init_strtab_2lvl(struct arm_smmu_device *smmu)
{
	u32 l1size;	/* [한국어] 위쪽 표의 바이트 크기. */
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;	/* [한국어] 채울 설정. */
	unsigned int last_sid_idx =	/* [한국어] 가장 큰 스트림 번호가 속한 위쪽 첨자. */
		arm_smmu_strtab_l1_idx((1ULL << smmu->sid_bits) - 1);

	/* Calculate the L1 size, capped to the SIDSIZE. */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 지원하는 번호를 모두 담으려면
	 * 얼마나 필요한지 계산하되, 그것이 너무 크면 상한으로 자른다. */
	cfg->l2.num_l1_ents = min(last_sid_idx + 1, STRTAB_MAX_L1_ENTRIES);
	if (cfg->l2.num_l1_ents <= last_sid_idx)	/* [한국어] 잘렸다면. */
		dev_warn(smmu->dev,	/* [한국어] 그 위의 번호를 가진 장치는 IOMMU 뒤에 설 수 없다 — 눈에 띄어야 한다. */
			 "2-level strtab only covers %u/%u bits of SID\n",
			 ilog2(cfg->l2.num_l1_ents * STRTAB_NUM_L2_STES),	/* [한국어] 실제로 덮는 폭. */
			 smmu->sid_bits);	/* [한국어] 하드웨어가 지원하는 폭. */

	l1size = cfg->l2.num_l1_ents * sizeof(struct arm_smmu_strtab_l1);	/* [한국어] 항목 하나가 8바이트뿐이라 평면 표보다 훨씬 작다. */
	cfg->l2.l1tab = dmam_alloc_coherent(smmu->dev, l1size, &cfg->l2.l1_dma,	/* [한국어] 하드웨어도 읽는 표라 일관 메모리로. */
					    GFP_KERNEL);
	if (!cfg->l2.l1tab) {	/* [한국어] 위쪽 표를 못 잡았다면. */
		dev_err(smmu->dev,	/* [한국어] 표가 없으면 어떤 장치도 붙일 수 없다. */
			"failed to allocate l1 stream table (%u bytes)\n",
			l1size);
		return -ENOMEM;	/* [한국어] 메모리가 없어 더 진행할 수 없다. */
	}

	cfg->l2.l2ptrs = devm_kcalloc(smmu->dev, cfg->l2.num_l1_ents,	/* [한국어] 아래쪽 표들의 커널 주소를 담을 배열 — 하드웨어는 안 보고 커널만 쓴다. */
				      sizeof(*cfg->l2.l2ptrs), GFP_KERNEL);
	if (!cfg->l2.l2ptrs)	/* [한국어] 커널 주소 배열을 못 잡았다면. */
		return -ENOMEM;

	return 0;	/* [한국어] 아래쪽 표는 장치가 프로브될 때 하나씩 잡힌다. */
}

/*
 * [한국어]
 * arm_smmu_init_strtab_linear - 평면 스트림 표를 통째로 잡는다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 스트림 번호 공간이 좁으면 표 전체를 잡아 두는 편이 단순하고 빠르다 —
 * 찾기가 첨자 하나로 끝나고, 아래쪽 표를 게으르게 잡는 코드도 필요 없다.
 *
 * 잡자마자 모두 "중단"으로 채우는 것이 중요하다. 아직 어느 장치도 붙지
 * 않았으므로 그 사이의 DMA 는 모두 거부되어야 한다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_strtab() → [이 함수] → arm_smmu_init_initial_stes()
 */
static int arm_smmu_init_strtab_linear(struct arm_smmu_device *smmu)
{
	u32 size;	/* [한국어] 표의 바이트 크기. */
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;	/* [한국어] 채울 설정. */

	size = (1 << smmu->sid_bits) * sizeof(struct arm_smmu_ste);	/* [한국어] 번호마다 64바이트 항목 하나. */
	cfg->linear.table = dmam_alloc_coherent(smmu->dev, size,	/* [한국어] 하드웨어도 읽는 표. */
						&cfg->linear.ste_dma,
						GFP_KERNEL);
	if (!cfg->linear.table) {	/* [한국어] 평면 표를 못 잡았다면. */
		dev_err(smmu->dev,	/* [한국어] 표가 없으면 어떤 장치도 붙일 수 없다. */
			"failed to allocate linear stream table (%u bytes)\n",
			size);
		return -ENOMEM;	/* [한국어] 메모리가 없어 더 진행할 수 없다. */
	}
	cfg->linear.num_ents = 1 << smmu->sid_bits;	/* [한국어] 범위 검사에 쓴다. */

	arm_smmu_init_initial_stes(cfg->linear.table, cfg->linear.num_ents);	/* [한국어] 모두 "중단"으로 채운다 — 아직 걸기 전이라 쓰기 규약 없이 써도 된다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_init_strtab - 스트림 표와 VMID 풀을 마련한다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, 음수 오류.
 *
 * 하드웨어가 2단계 표를 지원하면 그쪽을, 아니면 평면 표를 잡는다.
 * 그리고 이 하드웨어 전용 VMID 할당기를 준비한다 — VMID 는 TLB 태그가
 * 그 하드웨어 안에서만 뜻을 가지므로 전역이 아니라 장치별이다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_init_structures() → [이 함수]
 */
static int arm_smmu_init_strtab(struct arm_smmu_device *smmu)
{
	int ret;	/* [한국어] 중간 결과. */

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)	/* [한국어] 2단계를 지원하면. */
		ret = arm_smmu_init_strtab_2lvl(smmu);	/* [한국어] 위쪽 표만 잡는다. */
	else
		ret = arm_smmu_init_strtab_linear(smmu);	/* [한국어] 아니면 통째로 잡는다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	ida_init(&smmu->vmid_map);	/* [한국어] VMID 는 하드웨어마다 따로 관리한다 — 태그가 그 안에서만 뜻을 갖기 때문이다. */

	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_init_structures - 프로브의 자료 구조 단계를 모두 처리한다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, 음수 오류.
 *
 * 스트림 트리, 큐들, 스트림 표를 차례로 마련하고, 확장이 있으면 그쪽의
 * 초기화도 부른다. 확장 초기화를 마지막에 두는 이유는, 그 안에서
 * 표준 자료 구조(큐 등)를 쓸 수 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수]
 *     → arm_smmu_init_queues() → arm_smmu_init_strtab()
 */
static int arm_smmu_init_structures(struct arm_smmu_device *smmu)
{
	int ret;	/* [한국어] 중간 결과. */

	mutex_init(&smmu->streams_mutex);	/* [한국어] 스트림 트리를 지킬 락 — 폴트 처리도 이 트리를 읽는다. */
	smmu->streams = RB_ROOT;	/* [한국어] 빈 트리로 시작한다. */

	ret = arm_smmu_init_queues(smmu);	/* [한국어] 명령·이벤트·PRI 큐. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	ret = arm_smmu_init_strtab(smmu);	/* [한국어] 스트림 표와 VMID 풀. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	if (smmu->impl_ops && smmu->impl_ops->init_structures)	/* [한국어] 확장이 자기 자료 구조를 더 필요로 한다면. */
		return smmu->impl_ops->init_structures(smmu);	/* [한국어] 표준 구조가 다 준비된 뒤에 부른다 — 그 안에서 큐를 쓸 수 있어야 한다. */

	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_write_reg_sync - 레지스터를 쓰고 하드웨어가 확인하기를 기다린다
 *
 * @smmu: 대상 하드웨어.
 * @val: 쓸 값.
 * @reg_off: 쓸 레지스터의 오프셋.
 * @ack_off: 확인을 읽을 레지스터의 오프셋.
 * @return: 0 성공, -ETIMEDOUT 하드웨어가 응답하지 않음.
 *
 * SMMU 의 설정 레지스터들은 대부분 짝이 되는 확인 레지스터를 갖는다.
 * 쓴 값이 확인 레지스터에 그대로 나타나야 그 설정이 실제로 반영된 것이며,
 * 그 전에는 하드웨어가 아직 옛 설정으로 동작한다.
 *
 * 실행 컨텍스트: 프로브·리셋. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_reset()/disable() → [이 함수]
 */
static int arm_smmu_write_reg_sync(struct arm_smmu_device *smmu, u32 val,
				   unsigned int reg_off, unsigned int ack_off)
{
	u32 reg;	/* [한국어] 읽은 확인 값. */

	writel_relaxed(val, smmu->base + reg_off);	/* [한국어] 설정을 쓴다. */
	return readl_relaxed_poll_timeout(smmu->base + ack_off, reg, reg == val,	/* [한국어] 확인 레지스터가 같은 값이 될 때까지 기다린다. */
					  1, ARM_SMMU_POLL_TIMEOUT_US);	/* [한국어] 1마이크로초 간격으로. */
}

/* GBPA is "special" */
/*
 * [한국어]
 * arm_smmu_update_gbpa - 전역 우회 설정을 고쳐 쓴다
 *
 * @smmu: 대상 하드웨어.
 * @set: 세울 비트.
 * @clr: 내릴 비트.
 * @return: 0 성공, -ETIMEDOUT 하드웨어가 응답하지 않음.
 *
 * (위 영어 주석 참고) 이 레지스터만 유별나다. 확인 레지스터가 따로 있는
 * 것이 아니라, 자기 안의 UPDATE 비트가 내려가는 것으로 반영을 알린다.
 * 그래서 쓰기 전후로 그 비트가 내려가기를 두 번 기다려야 한다.
 *
 * 이 레지스터가 정하는 것은 "스트림 표에 항목이 없는 스트림을 어떻게
 * 할 것인가"다. 부팅 초기에는 펌웨어가 통과로 두었을 수 있고, 드라이버가
 * 표를 준비한 뒤 중단으로 바꾼다.
 *
 * 실행 컨텍스트: 프로브·리셋·해제. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_reset()/disable() → [이 함수]
 */
static int arm_smmu_update_gbpa(struct arm_smmu_device *smmu, u32 set, u32 clr)
{
	int ret;	/* [한국어] 기다림 결과. */
	u32 reg, __iomem *gbpa = smmu->base + ARM_SMMU_GBPA;	/* [한국어] 읽은 값과 레지스터 주소. */

	ret = readl_relaxed_poll_timeout(gbpa, reg, !(reg & GBPA_UPDATE),	/* [한국어] 앞선 갱신이 끝나기를 먼저 기다린다. */
					 1, ARM_SMMU_POLL_TIMEOUT_US);
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	reg &= ~clr;	/* [한국어] 읽은 값에서 내릴 비트를 지우고. */
	reg |= set;	/* [한국어] 세울 비트를 더한다 — 나머지 설정은 그대로 살린다. */
	writel_relaxed(reg | GBPA_UPDATE, gbpa);	/* [한국어] UPDATE 비트를 함께 세워 "이 값을 반영하라"고 알린다. */
	ret = readl_relaxed_poll_timeout(gbpa, reg, !(reg & GBPA_UPDATE),	/* [한국어] 그 비트가 내려가면 반영이 끝난 것이다. */
					 1, ARM_SMMU_POLL_TIMEOUT_US);

	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		dev_err(smmu->dev, "GBPA not responding to update\n");	/* [한국어] 하드웨어가 멈춰 선 것이다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_free_msis - 장치가 사라질 때 MSI 를 놓는다
 *
 * @data: 등록할 때 넘긴 장치 포인터.
 *
 * devres 가 되감기 때 부르는 콜백이다. 이렇게 등록해 두면 프로브가
 * 중간에 실패해도 MSI 가 새지 않는다.
 *
 * 실행 컨텍스트: 장치 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   devres 되감기 → [이 함수] → platform_device_msi_free_irqs_all()
 */
static void arm_smmu_free_msis(void *data)
{
	struct device *dev = data;	/* [한국어] 등록할 때 넘겨 둔 장치. */

	platform_device_msi_free_irqs_all(dev);	/* [한국어] 이 장치가 잡았던 MSI 를 모두 놓는다. */
}

/*
 * [한국어]
 * arm_smmu_write_msi_msg - 배정받은 MSI 주소를 하드웨어에 알린다
 *
 * @desc: MSI 서술자 (몇 번째 MSI 인지 알려 준다).
 * @msg: 인터럽트 컨트롤러가 정해 준 주소와 데이터.
 *
 * MSI 를 쓰려면 하드웨어에게 "인터럽트를 낼 때 이 주소에 이 값을 써라"고
 * 알려 줘야 한다. 인터럽트 컨트롤러가 그 주소와 값을 정해 주면 이 콜백이
 * 불려 레지스터에 옮겨 담는다.
 *
 * 메모리 속성을 장치 영역으로 지정하는 것이 중요하다 — MSI 쓰기는 실제
 * 메모리가 아니라 인터럽트 컨트롤러의 레지스터로 가므로, 캐시에 담기거나
 * 순서가 바뀌면 인터럽트가 사라지거나 늦게 도착한다.
 *
 * 실행 컨텍스트: MSI 할당. 잠들 수 있다.
 *
 * 호출 체인:
 *   platform_device_msi_init_and_alloc_irqs() → 이 콜백
 */
static void arm_smmu_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	phys_addr_t doorbell;	/* [한국어] 인터럽트를 낼 때 쓸 주소. */
	struct device *dev = msi_desc_to_dev(desc);	/* [한국어] 이 MSI 를 가진 장치. */
	struct arm_smmu_device *smmu = dev_get_drvdata(dev);	/* [한국어] 그 드라이버 상태. */
	phys_addr_t *cfg = arm_smmu_msi_cfg[desc->msi_index];	/* [한국어] 몇 번째 MSI 인지로 레지스터 묶음을 고른다. */

	doorbell = (((u64)msg->address_hi) << 32) | msg->address_lo;	/* [한국어] 32비트 둘로 나뉘어 온 주소를 하나로 합친다. */
	doorbell &= MSI_CFG0_ADDR_MASK;	/* [한국어] 하드웨어가 쓰는 비트만 남긴다. */

	writeq_relaxed(doorbell, smmu->base + cfg[0]);	/* [한국어] 그 주소를 알린다. */
	writel_relaxed(msg->data, smmu->base + cfg[1]);	/* [한국어] 그때 쓸 값 — 이 값으로 인터럽트 컨트롤러가 어느 인터럽트인지 안다. */
	writel_relaxed(ARM_SMMU_MEMATTR_DEVICE_nGnRE, smmu->base + cfg[2]);	/* [한국어] 장치 영역 속성 — 캐시에 담기거나 순서가 바뀌면 인터럽트를 놓친다. */
}

/*
 * [한국어]
 * arm_smmu_setup_msis - 인터럽트를 MSI 로 받도록 준비한다
 *
 * @smmu: 대상 하드웨어.
 *
 * MSI 를 쓸 수 있으면 배정받고, 그렇지 않으면 조용히 물러나 배선
 * 인터럽트로 돌아간다 — 둘 다 동작하므로 실패로 보지 않는다.
 *
 * 시작할 때 주소 레지스터를 0 으로 지우는 것이 중요하다. 펌웨어나 앞선
 * 커널이 남긴 값이 있으면, MSI 를 안 쓰기로 했는데도 하드웨어가 그
 * 주소에 쓰기를 보내 엉뚱한 메모리를 건드린다.
 *
 * 명령 큐에는 MSI 를 배정하지 않는데, 명령 완료는 인터럽트가 아니라
 * 메모리 쓰기로 감지하기 때문이다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_setup_unique_irqs() → [이 함수]
 *     → platform_device_msi_init_and_alloc_irqs()
 */
static void arm_smmu_setup_msis(struct arm_smmu_device *smmu)
{
	int ret, nvec = ARM_SMMU_MAX_MSIS;	/* [한국어] 결과와, 배정받을 MSI 개수. */
	struct device *dev = smmu->dev;	/* [한국어] 대상 장치. */

	/* Clear the MSI address regs */
	/* [한국어] (위 영어 주석 참고) 펌웨어나 앞선 커널이 남긴 주소가 있으면
	 * 하드웨어가 그리로 쓰기를 보낸다. 반드시 지우고 시작해야 한다. */
	writeq_relaxed(0, smmu->base + ARM_SMMU_GERROR_IRQ_CFG0);
	writeq_relaxed(0, smmu->base + ARM_SMMU_EVTQ_IRQ_CFG0);	/* [한국어] 이벤트 큐의 MSI 주소도 지운다. */

	if (smmu->features & ARM_SMMU_FEAT_PRI)	/* [한국어] PRI 큐가 있는 하드웨어면. */
		writeq_relaxed(0, smmu->base + ARM_SMMU_PRIQ_IRQ_CFG0);	/* [한국어] 그 자리도 지운다. */
	else
		nvec--;	/* [한국어] 없으면 배정받을 개수를 하나 줄인다. */

	if (!(smmu->features & ARM_SMMU_FEAT_MSI))	/* [한국어] 하드웨어가 MSI 를 못 쓰면. */
		return;	/* [한국어] 배선 인터럽트로 간다. */

	if (!dev->msi.domain) {	/* [한국어] 플랫폼에 MSI 를 나눠 줄 곳이 없으면. */
		dev_info(smmu->dev, "msi_domain absent - falling back to wired irqs\n");	/* [한국어] 오류가 아니라 정보로 남긴다 — 배선으로도 동작한다. */
		return;
	}

	/* Allocate MSIs for evtq, gerror and priq. Ignore cmdq */
	/* [한국어] (위 영어 주석 참고) 명령 큐에는 배정하지 않는다 — 명령 완료는
	 * 인터럽트가 아니라 큐 안의 메모리 쓰기로 감지하기 때문이다. */
	ret = platform_device_msi_init_and_alloc_irqs(dev, nvec, arm_smmu_write_msi_msg);
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_warn(dev, "failed to allocate MSIs - falling back to wired irqs\n");	/* [한국어] 배선으로 물러난다. */
		return;
	}

	smmu->evtq.q.irq = msi_get_virq(dev, EVTQ_MSI_INDEX);	/* [한국어] 배정받은 인터럽트 번호를 각 큐에 기록한다. */
	smmu->gerr_irq = msi_get_virq(dev, GERROR_MSI_INDEX);	/* [한국어] 전역 오류 인터럽트 번호. */
	smmu->priq.q.irq = msi_get_virq(dev, PRIQ_MSI_INDEX);	/* [한국어] PRI 가 없으면 배정되지 않아 0 이 된다. */

	/* Add callback to free MSIs on teardown */
	/* [한국어] (위 영어 주석 참고) devres 에 걸어 두면 프로브가 중간에 실패해도
	 * MSI 가 새지 않는다. */
	devm_add_action_or_reset(dev, arm_smmu_free_msis, dev);
}

/*
 * [한국어]
 * arm_smmu_setup_unique_irqs - 인터럽트 선이 여럿일 때 각각 등록한다
 *
 * @smmu: 대상 하드웨어.
 *
 * 이벤트 큐와 gerror 는 필수에 가깝고, PRI 는 그 기능이 있을 때만 등록한다.
 * 어느 것도 실패를 치명적으로 보지 않고 경고만 남기는데, 인터럽트 없이도
 * 동작은 하기 때문이다 — 다만 폴트나 오류를 알 수 없게 된다.
 *
 * gerror 만 스레드가 아닌 것에 주목할 만하다. 그 처리는 짧고 잠들지
 * 않으므로 인터럽트 문맥에서 곧바로 끝낼 수 있다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_setup_irqs() → [이 함수] → devm_request_threaded_irq()
 */
static void arm_smmu_setup_unique_irqs(struct arm_smmu_device *smmu)
{
	int irq, ret;	/* [한국어] 인터럽트 번호와 등록 결과. */

	arm_smmu_setup_msis(smmu);	/* [한국어] 먼저 MSI 를 배정받아 본다 — 성공하면 아래에서 그 번호를 쓴다. */

	/* Request interrupt lines */
	/* [한국어] (위 영어 주석 참고) 배정받은(또는 펌웨어가 알려 준) 번호로 등록한다. */
	irq = smmu->evtq.q.irq;	/* [한국어] 이벤트 큐 인터럽트. */
	if (irq) {	/* [한국어] 이벤트 큐 인터럽트가 있다면. */
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,	/* [한국어] 상위 처리기 없이 스레드만 — 폴트 처리가 잠들 수 있다. */
						arm_smmu_evtq_thread,
						IRQF_ONESHOT,	/* [한국어] 스레드가 끝날 때까지 같은 인터럽트를 막는다. */
						"arm-smmu-v3-evtq", smmu);
		if (ret < 0)	/* [한국어] 등록에 실패했다. */
			dev_warn(smmu->dev, "failed to enable evtq irq\n");	/* [한국어] 폴트를 못 받게 되지만 동작은 한다. */
	} else {
		dev_warn(smmu->dev, "no evtq irq - events will not be reported!\n");	/* [한국어] 폴트가 조용히 사라진다 — 진단이 매우 어려워진다. */
	}

	irq = smmu->gerr_irq;	/* [한국어] 전역 오류 인터럽트. */
	if (irq) {	/* [한국어] 전역 오류 인터럽트가 있다면. */
		ret = devm_request_irq(smmu->dev, irq, arm_smmu_gerror_handler,	/* [한국어] 스레드가 아니다 — 처리가 짧고 잠들지 않는다. */
				       0, "arm-smmu-v3-gerror", smmu);
		if (ret < 0)	/* [한국어] 등록에 실패했다. */
			dev_warn(smmu->dev, "failed to enable gerror irq\n");
	} else {
		dev_warn(smmu->dev, "no gerr irq - errors will not be reported!\n");	/* [한국어] 명령 큐가 막혀도 알 수 없게 된다. */
	}

	if (smmu->features & ARM_SMMU_FEAT_PRI) {	/* [한국어] 페이지 요청 큐를 가진 하드웨어라면. */
		irq = smmu->priq.q.irq;	/* [한국어] 페이지 요청 큐 인터럽트. */
		if (irq) {	/* [한국어] 그 인터럽트가 있다면. */
			ret = devm_request_threaded_irq(smmu->dev, irq, NULL,	/* [한국어] 역시 스레드로. */
							arm_smmu_priq_thread,
							IRQF_ONESHOT,
							"arm-smmu-v3-priq",
							smmu);
			if (ret < 0)	/* [한국어] 등록에 실패했다. */
				dev_warn(smmu->dev,
					 "failed to enable priq irq\n");
		} else {
			dev_warn(smmu->dev, "no priq irq - PRI will be broken\n");	/* [한국어] 요청이 쌓이기만 하고 처리되지 않는다. */
		}
	}
}

/*
 * [한국어]
 * arm_smmu_setup_irqs - 인터럽트를 등록하고 하드웨어에서 켠다
 *
 * @smmu: 대상 하드웨어.
 * @return: 항상 0 (등록 실패는 경고로만 다룬다).
 *
 * 먼저 하드웨어 쪽 인터럽트를 모두 끄고 시작한다 — 등록하기 전에 인터럽트가
 * 오면 아직 준비되지 않은 처리기가 불려 크래시가 난다.
 *
 * 인터럽트 선이 하나뿐인 하드웨어(ThunderX2)가 있어 두 갈래로 나뉜다.
 * 그때는 어느 큐에서 왔는지 알 수 없으므로 처리기가 둘 다 훑는다.
 *
 * 실행 컨텍스트: 프로브·리셋. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_reset() → [이 함수] → arm_smmu_setup_unique_irqs()
 */
static int arm_smmu_setup_irqs(struct arm_smmu_device *smmu)
{
	int ret, irq;	/* [한국어] 결과와 인터럽트 번호. */
	u32 irqen_flags = IRQ_CTRL_EVTQ_IRQEN | IRQ_CTRL_GERROR_IRQEN;	/* [한국어] 켤 인터럽트들 — 이벤트와 전역 오류는 언제나. */

	/* Disable IRQs first */
	/* [한국어] (위 영어 주석 참고) 등록 전에 인터럽트가 오면 준비되지 않은
	 * 처리기가 불린다. 반드시 먼저 꺼야 한다. */
	ret = arm_smmu_write_reg_sync(smmu, 0, ARM_SMMU_IRQ_CTRL,
				      ARM_SMMU_IRQ_CTRLACK);
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_err(smmu->dev, "failed to disable irqs\n");	/* [한국어] 하드웨어가 응답하지 않으면 더 진행할 수 없다. */
		return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
	}

	irq = smmu->combined_irq;	/* [한국어] 인터럽트 선이 하나로 묶인 하드웨어인가. */
	if (irq) {	/* [한국어] 하나로 묶인 인터럽트 선이 있다면. */
		/*
		 * Cavium ThunderX2 implementation doesn't support unique irq
		 * lines. Use a single irq line for all the SMMUv3 interrupts.
		 */
		/* [한국어] (위 영어 주석 참고) 규격은 종류별로 선을 나누라고 하지만,
		 * 그것을 지키지 않은 구현이 있다. 그때는 하나로 받아 처리기가 갈래를
		 * 나눈다. */
		ret = devm_request_threaded_irq(smmu->dev, irq,
					arm_smmu_combined_irq_handler,	/* [한국어] 상위 처리기 — gerror 를 곧바로 다룬다. */
					arm_smmu_combined_irq_thread,	/* [한국어] 스레드 — 큐를 비운다. */
					IRQF_ONESHOT,
					"arm-smmu-v3-combined-irq", smmu);
		if (ret < 0)	/* [한국어] 등록에 실패했다. */
			dev_warn(smmu->dev, "failed to enable combined irq\n");
	} else
		arm_smmu_setup_unique_irqs(smmu);	/* [한국어] 종류별로 따로 등록한다 — 흔한 경우다. */

	if (smmu->features & ARM_SMMU_FEAT_PRI)	/* [한국어] PRI 큐가 있으면. */
		irqen_flags |= IRQ_CTRL_PRIQ_IRQEN;	/* [한국어] 그 인터럽트도 켠다. */

	/* Enable interrupt generation on the SMMU */
	/* [한국어] (위 영어 주석 참고) 처리기가 모두 등록된 뒤에야 하드웨어 쪽을 켠다. */
	ret = arm_smmu_write_reg_sync(smmu, irqen_flags,
				      ARM_SMMU_IRQ_CTRL, ARM_SMMU_IRQ_CTRLACK);
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		dev_warn(smmu->dev, "failed to enable irqs\n");	/* [한국어] 경고만 남긴다 — 인터럽트 없이도 변환은 동작한다. */

	return 0;	/* [한국어] 인터럽트 문제로 프로브를 접지는 않는다. */
}

/*
 * [한국어]
 * arm_smmu_device_disable - SMMU 를 끈다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, 음수 실패.
 *
 * 제어 레지스터를 0 으로 만들어 변환과 큐 처리를 모두 멈춘다. 이 뒤로
 * 장치가 낸 DMA 는 전역 우회 설정(GBPA)이 정하는 대로 처리된다.
 *
 * 실행 컨텍스트: 프로브·리셋·해제·치명적 오류. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_reset()/gerror_handler() 등 → [이 함수]
 */
static int arm_smmu_device_disable(struct arm_smmu_device *smmu)
{
	int ret;	/* [한국어] 결과. */

	ret = arm_smmu_write_reg_sync(smmu, 0, ARM_SMMU_CR0, ARM_SMMU_CR0ACK);	/* [한국어] 모든 기능을 끄고 하드웨어가 확인하기를 기다린다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		dev_err(smmu->dev, "failed to clear cr0\n");	/* [한국어] 끄지 못했다면 하드웨어가 응답하지 않는 것이다. */

	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_write_strtab - 스트림 표의 주소와 형식을 하드웨어에 알린다
 *
 * @smmu: 대상 하드웨어.
 *
 * 2단계 표는 형식과 함께 "아래쪽 표 하나가 몇 개를 담는지(SPLIT)"도
 * 알려야 하고, 평면 표는 전체 크기만 알리면 된다.
 *
 * 크기를 로그로 담는 것에 주의할 만하다. 2단계에서는 위쪽 항목 수의
 * 로그에 SPLIT 을 더해 "전체 스트림 번호 폭"을 표현한다.
 *
 * 실행 컨텍스트: 리셋. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_reset() → [이 함수]
 */
static void arm_smmu_write_strtab(struct arm_smmu_device *smmu)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;	/* [한국어] 표 설정. */
	dma_addr_t dma;	/* [한국어] 하드웨어에 알릴 표 주소. */
	u32 reg;	/* [한국어] 형식과 크기를 담은 값. */

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {	/* [한국어] 2단계 표라면. */
		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,	/* [한국어] 2단계 표임을 알리는 형식 값. */
				 STRTAB_BASE_CFG_FMT_2LVL) |	/* [한국어] 형식을 2단계로 알린다. */
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE,
				 ilog2(cfg->l2.num_l1_ents) + STRTAB_SPLIT) |	/* [한국어] 위쪽 항목 수의 로그에 아래쪽 폭을 더하면 전체 번호 폭이 된다. */
		      FIELD_PREP(STRTAB_BASE_CFG_SPLIT, STRTAB_SPLIT);	/* [한국어] 어디서 위·아래를 가르는지 알린다. */
		dma = cfg->l2.l1_dma;	/* [한국어] 위쪽 표의 주소. */
	} else {	/* [한국어] 평면 표라면. */
		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,	/* [한국어] 평면 표임을 알리는 형식 값. */
				 STRTAB_BASE_CFG_FMT_LINEAR) |	/* [한국어] 형식을 평면으로. */
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE, smmu->sid_bits);	/* [한국어] 크기는 번호 폭 그대로다. */
		dma = cfg->linear.ste_dma;	/* [한국어] 표의 주소. */
	}
	writeq_relaxed((dma & STRTAB_BASE_ADDR_MASK) | STRTAB_BASE_RA,	/* [한국어] 주소와 함께 읽기 할당 힌트를 켠다 — 하드웨어가 표를 캐시에 올려도 좋다는 뜻이다. */
		       smmu->base + ARM_SMMU_STRTAB_BASE);
	writel_relaxed(reg, smmu->base + ARM_SMMU_STRTAB_BASE_CFG);	/* [한국어] 형식과 크기를 알린다. */
}

/*
 * [한국어]
 * arm_smmu_device_reset - 하드웨어를 알려진 상태로 만들고 켠다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, 음수 오류.
 *
 * 프로브의 마지막이자 가장 긴 함수다. 순서가 곧 안전이라 한 줄도 자리를
 * 바꿀 수 없다.
 *
 * 큰 흐름은 이렇다 — 먼저 끄고, 메모리 속성과 전역 설정을 쓰고, 스트림
 * 표를 알리고, 명령 큐를 켠다. 명령 큐가 켜져야 무효화를 낼 수 있으므로,
 * 그다음에 앞선 커널이나 펌웨어가 남긴 캐시를 모두 씻는다. 그러고 나서
 * 이벤트 큐와 PRI 큐를 켜고, 인터럽트를 걸고, 마지막에 변환을 켠다.
 *
 * 이미 켜져 있는 SMMU 를 만나면 경고와 함께 전역 우회를 중단으로 바꾸는
 * 대목이 눈에 띈다 — 앞선 커널(kexec 등)이 남긴 매핑으로 DMA 가 계속
 * 오고 있을 수 있어, 표를 갈아엎기 전에 막아 두는 것이다.
 *
 * 크래시 커널에서 이벤트 큐를 켜지 않는 것도 같은 맥락이다. 앞선 커널이
 * 남긴 DMA 가 폴트를 쏟아 내면 크래시 덤프를 뜨지 못한다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수]
 *     → arm_smmu_device_disable() → arm_smmu_setup_irqs()
 */
static int arm_smmu_device_reset(struct arm_smmu_device *smmu)
{
	int ret;	/* [한국어] 중간 결과. */
	u32 reg, enables;	/* [한국어] 레지스터 값과, 지금까지 켠 기능들의 누적. */
	struct arm_smmu_cmdq_ent cmd;	/* [한국어] 캐시를 씻을 명령. */

	/* Clear CR0 and sync (disables SMMU and queue processing) */
	/* [한국어] (위 영어 주석 참고) 알려진 상태에서 시작해야 한다. */
	reg = readl_relaxed(smmu->base + ARM_SMMU_CR0);
	if (reg & CR0_SMMUEN) {	/* [한국어] 이미 켜져 있다면 — 앞선 커널이 남긴 상태다. */
		dev_warn(smmu->dev, "SMMU currently enabled! Resetting...\n");	/* [한국어] 흔한 일이 아니므로 남긴다. */
		arm_smmu_update_gbpa(smmu, GBPA_ABORT, 0);	/* [한국어] 표를 갈아엎기 전에 항목 없는 스트림을 중단시킨다 — 앞선 매핑으로 DMA 가 계속 올 수 있다. */
	}

	ret = arm_smmu_device_disable(smmu);	/* [한국어] 변환과 큐 처리를 모두 멈춘다. */
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;

	/* CR1 (table and queue memory attributes) */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 표와 큐를 읽을 때의 캐시 속성.
	 * CPU 가 쓴 내용을 하드웨어가 보려면 같은 공유 영역에 되쓰기 캐시로
	 * 맞춰야 한다. */
	reg = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |	/* [한국어] 표는 안쪽 공유 영역. */
	      FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |	/* [한국어] 바깥 캐시 되쓰기. */
	      FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |	/* [한국어] 안쪽 캐시 되쓰기. */
	      FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |	/* [한국어] 큐도 같은 설정 — CPU 와 주고받는 링이기 때문이다. */
	      FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
	writel_relaxed(reg, smmu->base + ARM_SMMU_CR1);	/* [한국어] 표와 큐의 캐시 속성을 알린다. */

	/* CR2 (random crap) */
	/* [한국어] (위 영어 주석 참고) 이름 그대로 잡다한 설정이 모인 레지스터다. */
	reg = CR2_PTM | CR2_RECINVSID;	/* [한국어] 브로드캐스트 TLB 무효화를 받지 않고(명령 큐로만 한다), 잘못된 스트림 번호를 기록한다. */

	if (smmu->features & ARM_SMMU_FEAT_E2H)	/* [한국어] CPU 가 가상화 호스트 확장으로 돈다면. */
		reg |= CR2_E2H;	/* [한국어] SMMU 도 같은 세계로 맞춘다 — SVA 의 ASID 브로드캐스트가 짝지어지려면 필요하다. */

	writel_relaxed(reg, smmu->base + ARM_SMMU_CR2);	/* [한국어] 잡다한 전역 설정을 알린다. */

	/* Stream table */
	/* [한국어] (위 영어 주석 참고) 표의 주소와 형식을 알린다 — 아직 켜지는 않았다. */
	arm_smmu_write_strtab(smmu);

	/* Command queue */
	/* [한국어] (위 영어 주석 참고) 명령 큐를 가장 먼저 켠다 — 무효화를 내려면
	 * 이것이 있어야 한다. */
	writeq_relaxed(smmu->cmdq.q.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);	/* [한국어] 링의 주소와 크기. */
	writel_relaxed(smmu->cmdq.q.llq.prod, smmu->base + ARM_SMMU_CMDQ_PROD);	/* [한국어] 빈 큐로 시작한다. */
	writel_relaxed(smmu->cmdq.q.llq.cons, smmu->base + ARM_SMMU_CMDQ_CONS);	/* [한국어] 소비 포인터도 0 으로 — 빈 큐로 시작한다. */

	enables = CR0_CMDQEN;	/* [한국어] 명령 큐만 켠다 — 기능을 하나씩 더해 가며 확인하는 방식이다. */
	ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,	/* [한국어] 명령 큐만 켜고 하드웨어가 확인하기를 기다린다. */
				      ARM_SMMU_CR0ACK);
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_err(smmu->dev, "failed to enable command queue\n");	/* [한국어] 명령 큐 없이는 아무것도 할 수 없다. */
		return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
	}

	/* Invalidate any cached configuration */
	/* [한국어] (위 영어 주석 참고) 앞선 커널이나 펌웨어가 남긴 설정 캐시를 씻는다.
	 * 그러지 않으면 새로 만든 표 대신 옛 항목이 쓰인다. */
	cmd.opcode = CMDQ_OP_CFGI_ALL;
	arm_smmu_cmdq_issue_cmd_with_sync(smmu, &cmd);	/* [한국어] 완료까지 기다린다. */

	/* Invalidate any stale TLB entries */
	/* [한국어] (위 영어 주석 참고) 변환 캐시도 마찬가지로 씻어야 한다. */
	if (smmu->features & ARM_SMMU_FEAT_HYP) {	/* [한국어] 하이퍼바이저 세계를 지원하는 하드웨어면. */
		cmd.opcode = CMDQ_OP_TLBI_EL2_ALL;	/* [한국어] 그쪽 캐시도 씻는다. */
		arm_smmu_cmdq_issue_cmd_with_sync(smmu, &cmd);	/* [한국어] EL2 쪽 변환 캐시를 씻고 완료까지 기다린다. */
	}

	cmd.opcode = CMDQ_OP_TLBI_NSNH_ALL;	/* [한국어] 비보안·비하이퍼바이저 캐시를 모두 씻는다. */
	arm_smmu_cmdq_issue_cmd_with_sync(smmu, &cmd);	/* [한국어] 비보안·비하이퍼바이저 변환 캐시를 씻는다. */

	/* Event queue */
	/* [한국어] (위 영어 주석 참고) 이제 폴트를 받을 큐를 켠다. */
	writeq_relaxed(smmu->evtq.q.q_base, smmu->base + ARM_SMMU_EVTQ_BASE);	/* [한국어] 링 주소는 첫 페이지에서 알린다. */
	writel_relaxed(smmu->evtq.q.llq.prod, smmu->page1 + ARM_SMMU_EVTQ_PROD);	/* [한국어] 포인터는 둘째 페이지에 있다 — 규격이 그렇게 나눴다. */
	writel_relaxed(smmu->evtq.q.llq.cons, smmu->page1 + ARM_SMMU_EVTQ_CONS);	/* [한국어] 이벤트 큐의 소비 포인터도 0 으로. */

	enables |= CR0_EVTQEN;	/* [한국어] 앞서 켠 것에 더한다. */
	ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,	/* [한국어] 이벤트 큐를 더해 켠다. */
				      ARM_SMMU_CR0ACK);
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_err(smmu->dev, "failed to enable event queue\n");	/* [한국어] 이벤트 큐 없이는 폴트를 받을 수 없다. */
		return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
	}

	/* PRI queue */
	/* [한국어] (위 영어 주석 참고) 그 기능이 있는 하드웨어에서만. */
	if (smmu->features & ARM_SMMU_FEAT_PRI) {
		writeq_relaxed(smmu->priq.q.q_base,	/* [한국어] PRI 큐의 링 주소와 크기. */
			       smmu->base + ARM_SMMU_PRIQ_BASE);
		writel_relaxed(smmu->priq.q.llq.prod,	/* [한국어] 생산 포인터를 0 으로. */
			       smmu->page1 + ARM_SMMU_PRIQ_PROD);
		writel_relaxed(smmu->priq.q.llq.cons,	/* [한국어] 소비 포인터도 0 으로. */
			       smmu->page1 + ARM_SMMU_PRIQ_CONS);

		enables |= CR0_PRIQEN;	/* [한국어] PRI 큐를 켜는 비트를 더한다. */
		ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,	/* [한국어] 앞서 켠 것에 더해 다시 쓴다. */
					      ARM_SMMU_CR0ACK);
		if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
			dev_err(smmu->dev, "failed to enable PRI queue\n");	/* [한국어] PRI 큐를 못 켰다. */
			return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
		}
	}

	if (smmu->features & ARM_SMMU_FEAT_ATS) {	/* [한국어] 장치 쪽 변환 캐시를 다룰 수 있는 하드웨어면. */
		enables |= CR0_ATSCHK;	/* [한국어] 장치가 낸 ATS 요청이 허용된 것인지 검사하게 한다 — 신뢰할 수 없는 장치를 막는 장치다. */
		ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,	/* [한국어] ATS 요청 검사를 더해 켠다. */
					      ARM_SMMU_CR0ACK);
		if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
			dev_err(smmu->dev, "failed to enable ATS check\n");	/* [한국어] ATS 검사를 못 켰다 — 신뢰할 수 없는 장치를 막지 못한다. */
			return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
		}
	}

	ret = arm_smmu_setup_irqs(smmu);	/* [한국어] 변환을 켜기 전에 인터럽트를 준비한다 — 켜자마자 폴트가 올 수 있다. */
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_err(smmu->dev, "failed to setup irqs\n");	/* [한국어] 인터럽트를 못 걸면 폴트와 오류를 알 수 없다. */
		return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
	}

	if (is_kdump_kernel())	/* [한국어] 크래시 덤프를 뜨는 커널이라면. */
		enables &= ~(CR0_EVTQEN | CR0_PRIQEN);	/* [한국어] 앞선 커널이 남긴 DMA 가 폴트를 쏟아 내면 덤프를 뜨지 못한다 — 큐를 꺼 둔다. */

	/* Enable the SMMU interface */
	/* [한국어] (위 영어 주석 참고) 마지막으로 변환을 켠다. 이 순간부터 모든
	 * DMA 가 스트림 표를 거치며, 표는 "중단"으로 채워져 있다. */
	enables |= CR0_SMMUEN;
	ret = arm_smmu_write_reg_sync(smmu, enables, ARM_SMMU_CR0,	/* [한국어] 마지막으로 변환을 켠다. */
				      ARM_SMMU_CR0ACK);
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_err(smmu->dev, "failed to enable SMMU interface\n");	/* [한국어] 변환을 못 켰다면 이 하드웨어는 쓸 수 없다. */
		return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
	}

	if (smmu->impl_ops && smmu->impl_ops->device_reset) {	/* [한국어] 확장이 자기 리셋을 필요로 하면. */
		ret = smmu->impl_ops->device_reset(smmu);	/* [한국어] 표준 초기화가 끝난 뒤에 부른다. */
		if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
			dev_err(smmu->dev, "failed to reset impl\n");	/* [한국어] 확장 초기화가 실패했다. */
			return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
		}
	}

	return 0;	/* [한국어] 하드웨어가 준비됐다. */
}

/* [한국어] 하드웨어를 만든 곳과 제품을 알아보는 값들.
 *
 * 아래 결함 우회는 "어떤 회사의 어떤 제품의 어떤 개정판"까지 특정해
 * 적용해야 하므로, 그 값들을 이름으로 정의해 둔다. */
#define IIDR_IMPLEMENTER_ARM		0x43b	/* [한국어] ARM 이 직접 만든 구현. */
#define IIDR_PRODUCTID_ARM_MMU_600	0x483	/* [한국어] MMU-600 — 초기 세대라 결함이 몇 있다. */
#define IIDR_PRODUCTID_ARM_MMU_700	0x487	/* [한국어] MMU-700 — 주석이 "결함이 많다"고 적을 만큼. */
#define IIDR_PRODUCTID_ARM_MMU_L1	0x48a	/* [한국어] 최신 세대. */
#define IIDR_PRODUCTID_ARM_MMU_S3	0x498	/* [한국어] 역시 최신 세대. */

/*
 * [한국어]
 * arm_smmu_device_iidr_probe - 제품 번호를 보고 알려진 결함을 우회한다
 *
 * @smmu: 대상 하드웨어.
 *
 * 능력 레지스터는 "무엇을 지원한다"고 말하지만, 그 기능에 결함이 있는지는
 * 말해 주지 않는다. 그래서 제품과 개정판을 직접 확인해, 알려진 결함이
 * 있는 조합에서는 그 기능을 아예 꺼 버린다.
 *
 * 기능을 끄는 방식이 대부분인 것에 주목할 만하다 — 결함을 우회하는 코드를
 * 따로 쓰기보다, 그 기능을 쓰지 않는 편이 단순하고 안전하기 때문이다.
 *
 * 실행 컨텍스트: 프로브. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수]
 */
static void arm_smmu_device_iidr_probe(struct arm_smmu_device *smmu)
{
	u32 reg;	/* [한국어] 신원 레지스터 값. */
	unsigned int implementer, productid, variant, revision;	/* [한국어] 만든 곳, 제품, 변형, 개정. */

	reg = readl_relaxed(smmu->base + ARM_SMMU_IIDR);	/* [한국어] 하드웨어의 신원. */
	implementer = FIELD_GET(IIDR_IMPLEMENTER, reg);	/* [한국어] 어느 회사가 만들었는가. */
	productid = FIELD_GET(IIDR_PRODUCTID, reg);	/* [한국어] 어떤 제품인가. */
	variant = FIELD_GET(IIDR_VARIANT, reg);	/* [한국어] 큰 변형 번호. */
	revision = FIELD_GET(IIDR_REVISION, reg);	/* [한국어] 작은 개정 번호. */

	switch (implementer) {	/* [한국어] 값에 따라 갈래를 나눈다. */
	case IIDR_IMPLEMENTER_ARM:	/* [한국어] ARM 이 직접 만든 구현에만 알려진 결함 목록이 있다. */
		switch (productid) {	/* [한국어] 값에 따라 갈래를 나눈다. */
		case IIDR_PRODUCTID_ARM_MMU_600:	/* [한국어] 이 값일 때의 처리. */
			/* Arm erratum 1076982 */
			/* [한국어] (위 영어 주석 참고) 이벤트로 깨우기가 제대로 동작하지 않는다. */
			if (variant == 0 && revision <= 2)
				smmu->features &= ~ARM_SMMU_FEAT_SEV;	/* [한국어] 그 기능을 꺼 폴링으로 기다리게 한다 — 조금 느릴 뿐 정확하다. */
			/* Arm erratum 1209401 */
			/* [한국어] (위 영어 주석 참고) 중첩 변환에 결함이 있다. */
			if (variant < 2)
				smmu->features &= ~ARM_SMMU_FEAT_NESTING;	/* [한국어] 게스트에게 SMMU 를 넘길 수 없게 막는다. */
			break;
		case IIDR_PRODUCTID_ARM_MMU_700:	/* [한국어] 이 값일 때의 처리. */
			/* Many errata... */
			/* [한국어] (위 영어 주석 참고) 이 제품은 결함이 많아 개정판과 무관하게
			 * 브로드캐스트 TLB 무효화를 아예 쓰지 않는다. */
			smmu->features &= ~ARM_SMMU_FEAT_BTM;
			if (variant < 1 || revision < 1) {	/* [한국어] 초기 개정판에는 결함이 더 있다. */
				/* Arm erratum 2812531 */
				/* [한국어] (위 영어 주석 참고) 명령을 모아 넣으면 문제가 생긴다. */
				smmu->options |= ARM_SMMU_OPT_CMDQ_FORCE_SYNC;	/* [한국어] 묶음마다 완료 대기를 강제해 안전하게 만든다 — 느려지지만 정확하다. */
				/* Arm errata 2268618, 2812531 */
				/* [한국어] (위 영어 주석 참고) 중첩 변환에도 결함이 있다. */
				smmu->features &= ~ARM_SMMU_FEAT_NESTING;
			}
			break;
		case IIDR_PRODUCTID_ARM_MMU_L1:	/* [한국어] 최신 세대 둘. */
		case IIDR_PRODUCTID_ARM_MMU_S3:	/* [한국어] 이 값일 때의 처리. */
			/* Arm errata 3878312/3995052 */
			/* [한국어] (위 영어 주석 참고) 브로드캐스트 무효화에 결함이 있어
			 * 명령 큐로만 무효화한다. */
			smmu->features &= ~ARM_SMMU_FEAT_BTM;
			break;
		}
		break;
	}
}

/*
 * [한국어]
 * arm_smmu_get_httu - 하드웨어의 접근·더티 비트 갱신 능력을 정한다
 *
 * @smmu: 대상 하드웨어.
 * @reg: IDR0 레지스터 값.
 *
 * 하드웨어가 페이지 테이블의 접근 플래그와 더티 상태를 스스로 갱신할 수
 * 있는지 알아낸다. 그 능력이 있어야 마이그레이션에서 바뀐 페이지를
 * 골라낼 수 있다.
 *
 * 장치 트리와 ACPI 의 처리가 다른 것이 요점이다. 장치 트리에는 이 정보를
 * 적는 자리가 없어 하드웨어 값을 그대로 믿고, ACPI IORT 에는 그 자리가
 * 있어 펌웨어 값을 우선한다 — 펌웨어가 결함을 알고 낮춰 적었을 수 있기
 * 때문이다. 다만 둘이 어긋나면 경고를 남긴다.
 *
 * 실행 컨텍스트: 프로브. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_hw_probe() → [이 함수]
 */
static void arm_smmu_get_httu(struct arm_smmu_device *smmu, u32 reg)
{
	u32 fw_features = smmu->features & (ARM_SMMU_FEAT_HA | ARM_SMMU_FEAT_HD);	/* [한국어] 펌웨어가 이미 적어 둔 값. */
	u32 hw_features = 0;	/* [한국어] 하드웨어가 스스로 말하는 값. */

	switch (FIELD_GET(IDR0_HTTU, reg)) {	/* [한국어] 값에 따라 갈래를 나눈다. */
	case IDR0_HTTU_ACCESS_DIRTY:	/* [한국어] 더티까지 갱신할 수 있다면. */
		hw_features |= ARM_SMMU_FEAT_HD;	/* [한국어] 더티 갱신. */
		fallthrough;	/* [한국어] 더티를 갱신하면 접근 플래그도 당연히 갱신한다. */
	case IDR0_HTTU_ACCESS:	/* [한국어] 접근 플래그만 갱신할 수 있다면. */
		hw_features |= ARM_SMMU_FEAT_HA;	/* [한국어] 접근 갱신. */
	}

	if (smmu->dev->of_node)	/* [한국어] 장치 트리로 찾은 하드웨어라면. */
		smmu->features |= hw_features;	/* [한국어] 적을 자리가 없어 하드웨어 값을 그대로 믿는다. */
	else if (hw_features != fw_features)	/* [한국어] ACPI 인데 둘이 어긋난다면. */
		/* ACPI IORT sets the HTTU bits */
		/* [한국어] (위 영어 주석 참고) 펌웨어 값을 우선하되 — 결함을 알고 낮춰
		 * 적었을 수 있다 — 어긋남 자체는 알려 둔다. */
		dev_warn(smmu->dev,
			 "IDR0.HTTU features(0x%x) overridden by FW configuration (0x%x)\n",
			  hw_features, fw_features);
}

/*
 * [한국어]
 * arm_smmu_device_hw_probe - 능력 레지스터를 읽어 이 하드웨어의 성질을 정한다
 *
 * @smmu: 대상 하드웨어.
 * @return: 0 성공, -ENXIO 이 드라이버가 다룰 수 없는 하드웨어.
 *
 * 프로브의 첫 실질 단계다. IDR0/1/3/5 네 개의 능력 레지스터를 읽어
 * 기능 비트를 세우고, 큐 크기와 주소 폭 같은 한계값을 정한다. 이후 코드가
 * 하드웨어를 다루는 모든 판단이 여기서 정해진 값에 기댄다.
 *
 * 몇 가지는 지원하지 않으면 아예 프로브를 접는다 — 변환 자체를 못 하거나,
 * 표 형식이나 엔디안이 맞지 않거나, 명령 큐가 너무 작은 경우다. 그런
 * 하드웨어는 이 드라이버가 다룰 수 없다.
 *
 * 실행 컨텍스트: 프로브. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → arm_smmu_get_httu()
 */
static int arm_smmu_device_hw_probe(struct arm_smmu_device *smmu)
{
	u32 reg;	/* [한국어] 읽은 레지스터 값. */
	bool coherent = smmu->features & ARM_SMMU_FEAT_COHERENCY;	/* [한국어] 펌웨어가 적어 둔 캐시 일관성 — 아래에서 하드웨어 값과 견준다. */

	/* IDR0 */
	/* [한국어] (위 영어 주석 참고) 가장 기본적인 능력들이 모여 있다. */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR0);

	/* 2-level structures */
	/* [한국어] (위 영어 주석 참고) 표를 2단계로 만들 수 있는가 — 넓은 번호
	 * 공간을 감당하려면 필요하다. */
	if (FIELD_GET(IDR0_ST_LVL, reg) == IDR0_ST_LVL_2LVL)
		smmu->features |= ARM_SMMU_FEAT_2_LVL_STRTAB;	/* [한국어] 스트림 표를 2단계로. */

	if (reg & IDR0_CD2L)	/* [한국어] 문맥 서술자 표도 2단계로 만들 수 있는가. */
		smmu->features |= ARM_SMMU_FEAT_2_LVL_CDTAB;	/* [한국어] 문맥 서술자 표도 2단계로. */

	/*
	 * Translation table endianness.
	 * We currently require the same endianness as the CPU, but this
	 * could be changed later by adding a new IO_PGTABLE_QUIRK.
	 */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 페이지 테이블 항목을 읽는 엔디안이
	 * CPU 와 같아야 한다. 다르면 io-pgtable 이 만든 항목을 하드웨어가
	 * 뒤집어 읽어 엉뚱한 주소를 걷는다. */
	switch (FIELD_GET(IDR0_TTENDIAN, reg)) {
	case IDR0_TTENDIAN_MIXED:	/* [한국어] 둘 다 지원하면. */
		smmu->features |= ARM_SMMU_FEAT_TT_LE | ARM_SMMU_FEAT_TT_BE;	/* [한국어] 어느 쪽이든 맞출 수 있다. */
		break;
#ifdef __BIG_ENDIAN	/* [한국어] 빅엔디안 커널에서는. */
	case IDR0_TTENDIAN_BE:
		smmu->features |= ARM_SMMU_FEAT_TT_BE;	/* [한국어] 하드웨어도 빅엔디안이어야 한다. */
		break;
#else	/* [한국어] 리틀엔디안 커널에서는. */
	case IDR0_TTENDIAN_LE:
		smmu->features |= ARM_SMMU_FEAT_TT_LE;	/* [한국어] 하드웨어도 리틀엔디안이어야 한다. */
		break;
#endif
	default:	/* [한국어] CPU 와 맞지 않는 하드웨어. */
		dev_err(smmu->dev, "unknown/unsupported TT endianness!\n");	/* [한국어] CPU 와 엔디안이 맞지 않는 하드웨어다. */
		return -ENXIO;	/* [한국어] 다룰 수 없다. */
	}

	/* Boolean feature flags */
	/* [한국어] (위 영어 주석 참고) 여기부터는 있고 없고만 따지는 기능들이다. */
	if (IS_ENABLED(CONFIG_PCI_PRI) && reg & IDR0_PRI)	/* [한국어] 커널도 켜져 있고 하드웨어도 지원해야 한다. */
		smmu->features |= ARM_SMMU_FEAT_PRI;	/* [한국어] 페이지 요청 인터페이스. */

	if (IS_ENABLED(CONFIG_PCI_ATS) && reg & IDR0_ATS)	/* [한국어] 커널도 켜져 있고 하드웨어도 지원해야 한다. */
		smmu->features |= ARM_SMMU_FEAT_ATS;	/* [한국어] 장치 쪽 변환 캐시. */

	if (reg & IDR0_SEV)	/* [한국어] 이벤트로 깨우기를 지원하는가. */
		smmu->features |= ARM_SMMU_FEAT_SEV;	/* [한국어] 이벤트로 깨우기 — WFE 로 기다릴 수 있게 된다. */

	if (reg & IDR0_MSI) {	/* [한국어] MSI 로 인터럽트를 받을 수 있는가. */
		smmu->features |= ARM_SMMU_FEAT_MSI;	/* [한국어] MSI 로 인터럽트를 받을 수 있다. */
		if (coherent && !disable_msipolling)	/* [한국어] 캐시 일관성이 있어야 MSI 쓰기를 메모리에서 볼 수 있다. */
			smmu->options |= ARM_SMMU_OPT_MSIPOLL;	/* [한국어] 명령 완료도 MSI 쓰기로 감지한다 — MMIO 읽기보다 훨씬 싸다. */
	}

	if (reg & IDR0_HYP) {	/* [한국어] 하이퍼바이저 세계를 다룰 수 있다면. */
		smmu->features |= ARM_SMMU_FEAT_HYP;	/* [한국어] 하이퍼바이저 세계를 다룰 수 있다. */
		if (cpus_have_cap(ARM64_HAS_VIRT_HOST_EXTN))	/* [한국어] CPU 도 가상화 호스트 확장으로 돌고 있다면. */
			smmu->features |= ARM_SMMU_FEAT_E2H;	/* [한국어] SMMU 를 CPU 와 같은 세계로 맞춘다 — SVA 의 ASID 짝짓기에 필요하다. */
	}

	arm_smmu_get_httu(smmu, reg);	/* [한국어] 접근·더티 비트 갱신 능력. */

	/*
	 * The coherency feature as set by FW is used in preference to the ID
	 * register, but warn on mismatch.
	 */
	/* [한국어] (위 영어 주석 참고) 캐시 일관성은 하드웨어 혼자 결정하는 것이
	 * 아니라 시스템 연결 방식이 함께 정한다. 그래서 펌웨어 값을 우선하되,
	 * 어긋나면 알려 둔다 — 둘 중 하나가 틀렸다는 뜻이기 때문이다. */
	if (!!(reg & IDR0_COHACC) != coherent)
		dev_warn(smmu->dev, "IDR0.COHACC overridden by FW configuration (%s)\n",
			 str_true_false(coherent));

	switch (FIELD_GET(IDR0_STALL_MODEL, reg)) {	/* [한국어] 폴트 때 트랜잭션을 멈춰 세울 수 있는가. */
	case IDR0_STALL_MODEL_FORCE:	/* [한국어] 멈춤을 강제하는 하드웨어 — 끌 수 없다. */
		smmu->features |= ARM_SMMU_FEAT_STALL_FORCE;	/* [한국어] 멈춤을 강제하는 하드웨어 — 끌 수 없다. */
		fallthrough;	/* [한국어] 강제한다면 당연히 지원도 한다. */
	case IDR0_STALL_MODEL_STALL:	/* [한국어] 멈춤을 지원한다. */
		smmu->features |= ARM_SMMU_FEAT_STALLS;	/* [한국어] SVA 가 폴트를 처리하려면 이것이 필요하다. */
	}

	if (reg & IDR0_S1P)	/* [한국어] 1단계 변환을 할 수 있는가. */
		smmu->features |= ARM_SMMU_FEAT_TRANS_S1;	/* [한국어] 1단계 변환을 할 수 있다. */

	if (reg & IDR0_S2P)	/* [한국어] 2단계 변환을 할 수 있는가. */
		smmu->features |= ARM_SMMU_FEAT_TRANS_S2;	/* [한국어] 2단계 변환을 할 수 있다. */

	if (!(reg & (IDR0_S1P | IDR0_S2P))) {	/* [한국어] 둘 다 못 한다면. */
		dev_err(smmu->dev, "no translation support!\n");	/* [한국어] IOMMU 라고 부를 수 없다. */
		return -ENXIO;	/* [한국어] 이 드라이버가 다룰 수 없는 하드웨어다. */
	}

	/* We only support the AArch64 table format at present */
	/* [한국어] (위 영어 주석 참고) 32비트 형식 표는 지원하지 않는다 — io-pgtable
	 * 쪽 코드가 없기 때문이다. */
	if (!(FIELD_GET(IDR0_TTF, reg) & IDR0_TTF_AARCH64)) {
		dev_err(smmu->dev, "AArch64 table format not supported!\n");	/* [한국어] 32비트 형식 표만 지원하는 하드웨어는 다룰 수 없다. */
		return -ENXIO;	/* [한국어] 이 드라이버가 다룰 수 없는 하드웨어다. */
	}

	/* ASID/VMID sizes */
	/* [한국어] (위 영어 주석 참고) 태그 폭 — 동시에 존재할 수 있는 주소 공간과
	 * 가상 기계의 수를 정한다. */
	smmu->asid_bits = reg & IDR0_ASID16 ? 16 : 8;	/* [한국어] 16비트면 65536개, 8비트면 256개. */
	smmu->vmid_bits = reg & IDR0_VMID16 ? 16 : 8;	/* [한국어] VMID 폭 — 동시에 존재할 수 있는 가상 기계 수를 정한다. */

	/* IDR1 */
	/* [한국어] (위 영어 주석 참고) 큐와 표의 크기 한계가 모여 있다. */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR1);
	if (reg & (IDR1_TABLES_PRESET | IDR1_QUEUES_PRESET | IDR1_REL)) {	/* [한국어] 표나 큐의 위치가 하드웨어에 박혀 있는 구현이라면. */
		dev_err(smmu->dev, "embedded implementation not supported\n");	/* [한국어] 이 드라이버는 표를 직접 잡아 알리는 방식만 안다. */
		return -ENXIO;	/* [한국어] 이 드라이버가 다룰 수 없는 하드웨어다. */
	}

	if (reg & IDR1_ATTR_TYPES_OVR)	/* [한국어] 메모리 속성을 STE 에서 덮어쓸 수 있는가. */
		smmu->features |= ARM_SMMU_FEAT_ATTR_TYPES_OVR;	/* [한국어] 메모리 속성을 STE 에서 덮어쓸 수 있다. */

	/* Queue sizes, capped to ensure natural alignment */
	/* [한국어] (위 영어 주석 참고) 링 버퍼는 자기 크기에 정렬되어야 하는데,
	 * 너무 크면 그런 연속 메모리를 잡을 수 없다. 그래서 상한을 둔다. */
	smmu->cmdq.q.llq.max_n_shift = min_t(u32, CMDQ_MAX_SZ_SHIFT,	/* [한국어] 드라이버가 정한 상한과. */
					     FIELD_GET(IDR1_CMDQS, reg));	/* [한국어] 하드웨어 한계 중 작은 쪽. */
	if (smmu->cmdq.q.llq.max_n_shift <= ilog2(CMDQ_BATCH_ENTRIES)) {	/* [한국어] 명령 묶음 하나도 못 담을 만큼 작다면. */
		/*
		 * We don't support splitting up batches, so one batch of
		 * commands plus an extra sync needs to fit inside the command
		 * queue. There's also no way we can handle the weird alignment
		 * restrictions on the base pointer for a unit-length queue.
		 */
		/* [한국어] (위 영어 주석 참고) 묶음을 쪼개는 코드가 없어, 묶음 하나에
		 * 완료 대기까지 더한 것이 큐에 들어가야 한다. 항목 하나짜리 큐는
		 * 정렬 규칙도 이상해져 다룰 수 없다. */
		dev_err(smmu->dev, "command queue size <= %d entries not supported\n",
			CMDQ_BATCH_ENTRIES);
		return -ENXIO;	/* [한국어] 이 드라이버가 다룰 수 없는 하드웨어다. */
	}

	smmu->evtq.q.llq.max_n_shift = min_t(u32, EVTQ_MAX_SZ_SHIFT,	/* [한국어] 이벤트 큐도 같은 방식으로. */
					     FIELD_GET(IDR1_EVTQS, reg));
	smmu->priq.q.llq.max_n_shift = min_t(u32, PRIQ_MAX_SZ_SHIFT,	/* [한국어] PRI 큐도. */
					     FIELD_GET(IDR1_PRIQS, reg));

	/* SID/SSID sizes */
	/* [한국어] (위 영어 주석 참고) 스트림 번호와 PASID 의 폭. */
	smmu->ssid_bits = FIELD_GET(IDR1_SSIDSIZE, reg);	/* [한국어] 문맥 서술자 표의 크기를 정한다. */
	smmu->sid_bits = FIELD_GET(IDR1_SIDSIZE, reg);	/* [한국어] 스트림 표의 크기를 정한다. */
	smmu->iommu.max_pasids = 1UL << smmu->ssid_bits;	/* [한국어] 코어에게 이 하드웨어가 지원하는 PASID 수를 알린다. */

	/*
	 * If the SMMU supports fewer bits than would fill a single L2 stream
	 * table, use a linear table instead.
	 */
	/* [한국어] (위 영어 주석 참고) 번호 공간이 아래쪽 표 하나에도 못 미칠 만큼
	 * 좁으면, 2단계로 만들어 봐야 위쪽 표만 낭비다. 평면으로 간다. */
	if (smmu->sid_bits <= STRTAB_SPLIT)
		smmu->features &= ~ARM_SMMU_FEAT_2_LVL_STRTAB;

	/* IDR3 */
	/* [한국어] (위 영어 주석 참고) 무효화와 캐시 관련 기능들. */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR3);
	if (FIELD_GET(IDR3_RIL, reg))	/* [한국어] 범위 무효화를 지원하는가. */
		smmu->features |= ARM_SMMU_FEAT_RANGE_INV;	/* [한국어] 범위 무효화 — 넓은 구간을 명령 몇 개로 지울 수 있게 된다. */
	if (FIELD_GET(IDR3_FWB, reg))	/* [한국어] 2단계가 캐시 속성을 강제할 수 있는가. */
		smmu->features |= ARM_SMMU_FEAT_S2FWB;	/* [한국어] 2단계가 캐시 속성을 강제할 수 있다 — 중첩 변환의 안전 조건이다. */

	if (FIELD_GET(IDR3_BBM, reg) == 2)	/* [한국어] 블록 매핑을 중단 없이 쪼개고 합칠 수 있는가. */
		smmu->features |= ARM_SMMU_FEAT_BBML2;	/* [한국어] 블록 매핑을 중단 없이 쪼개고 합칠 수 있다 — SVA 에서 CPU 와 맞춰야 하는 성질이다. */

	/* IDR5 */
	/* [한국어] (위 영어 주석 참고) 페이지 크기와 주소 폭. */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR5);

	/* Maximum number of outstanding stalls */
	/* [한국어] (위 영어 주석 참고) 동시에 멈춰 세울 수 있는 트랜잭션 수 —
	 * 폴트 큐의 깊이를 정하는 근거가 된다. */
	smmu->evtq.max_stalls = FIELD_GET(IDR5_STALL_MAX, reg);

	/* Page sizes */
	/* [한국어] (위 영어 주석 참고) 알갱이 크기마다 그 위의 블록 크기도 함께
	 * 쓸 수 있다 — 큰 매핑을 적은 항목으로 표현할 수 있게 된다. */
	if (reg & IDR5_GRAN64K)
		smmu->pgsize_bitmap |= SZ_64K | SZ_512M;	/* [한국어] 64K 알갱이와 그 블록. */
	if (reg & IDR5_GRAN16K)	/* [한국어] 16K 알갱이와 그 블록. */
		smmu->pgsize_bitmap |= SZ_16K | SZ_32M;	/* [한국어] 16K 알갱이와 그 블록. */
	if (reg & IDR5_GRAN4K)	/* [한국어] 4K 는 두 단계의 블록을 함께 쓸 수 있다. */
		smmu->pgsize_bitmap |= SZ_4K | SZ_2M | SZ_1G;	/* [한국어] 4K 는 두 단계의 블록을 함께 쓸 수 있다. */

	/* Input address size */
	/* [한국어] (위 영어 주석 참고) 확장된 가상 주소 폭을 지원하는가. */
	if (FIELD_GET(IDR5_VAX, reg) == IDR5_VAX_52_BIT)
		smmu->features |= ARM_SMMU_FEAT_VAX;	/* [한국어] 52비트 입력 주소 — 52비트 커널과 짝을 이뤄야 SVA 를 쓸 수 있다. */

	/* Output address size */
	/* [한국어] (위 영어 주석 참고) 하드웨어가 내보낼 수 있는 물리 주소 폭.
	 * 페이지 테이블이 그보다 넓은 주소를 담으면 잘려 나간다. */
	switch (FIELD_GET(IDR5_OAS, reg)) {
	case IDR5_OAS_32_BIT:	/* [한국어] 아래 값들은 규격이 정한 코드다. */
		smmu->oas = 32;	/* [한국어] 32비트 출력 주소. */
		break;
	case IDR5_OAS_36_BIT:	/* [한국어] 이 값일 때의 처리. */
		smmu->oas = 36;	/* [한국어] 36비트. */
		break;
	case IDR5_OAS_40_BIT:	/* [한국어] 이 값일 때의 처리. */
		smmu->oas = 40;	/* [한국어] 40비트. */
		break;
	case IDR5_OAS_42_BIT:	/* [한국어] 이 값일 때의 처리. */
		smmu->oas = 42;	/* [한국어] 42비트. */
		break;
	case IDR5_OAS_44_BIT:	/* [한국어] 이 값일 때의 처리. */
		smmu->oas = 44;	/* [한국어] 44비트. */
		break;
	case IDR5_OAS_52_BIT:	/* [한국어] 가장 넓은 경우. */
		smmu->oas = 52;	/* [한국어] 52비트 — 가장 넓다. */
		smmu->pgsize_bitmap |= 1ULL << 42; /* 4TB */	/* [한국어] (위 영어 주석 참고) 주소가 넓어야 표현할 수 있는 아주 큰 블록 — 매핑 하나로 4TB 를 덮는다. */
		break;
	default:	/* [한국어] 규격에 없는 코드 — 새 개정판일 수 있다. */
		dev_info(smmu->dev,	/* [한국어] 규격에 없는 코드라 안전한 값으로 낮춘다. */
			"unknown output address size. Truncating to 48-bit\n");	/* [한국어] 안전한 값으로 낮춘다 — 좁게 잡는 것은 안전하지만 넓게 잡으면 위험하다. */
		fallthrough;	/* [한국어] 아래 갈래의 처리를 그대로 이어 쓴다. */
	case IDR5_OAS_48_BIT:	/* [한국어] 가장 흔한 값. */
		smmu->oas = 48;	/* [한국어] 가장 흔한 48비트. */
	}

	/* Set the DMA mask for our table walker */
	/* [한국어] (위 영어 주석 참고) 표와 큐를 잡을 때 이 폭 안의 주소만 받도록
	 * 알린다. 그러지 않으면 하드웨어가 닿지 못하는 주소에 표가 잡힐 수 있다. */
	if (dma_set_mask_and_coherent(smmu->dev, DMA_BIT_MASK(smmu->oas)))
		dev_warn(smmu->dev,
			 "failed to set DMA mask for table walker\n");	/* [한국어] 경고만 남긴다 — 기본 마스크로도 대개 동작한다. */

	if ((smmu->features & ARM_SMMU_FEAT_TRANS_S1) &&	/* [한국어] 1단계와. */
	    (smmu->features & ARM_SMMU_FEAT_TRANS_S2))	/* [한국어] 2단계를 모두 할 수 있으면. */
		smmu->features |= ARM_SMMU_FEAT_NESTING;	/* [한국어] 둘을 겹쳐 중첩 변환을 할 수 있다 — 게스트에게 SMMU 를 넘길 조건이다. */

	arm_smmu_device_iidr_probe(smmu);	/* [한국어] 제품별 결함 우회 — 방금 세운 기능 비트를 다시 내릴 수 있으므로 순서가 중요하다. */

	if (arm_smmu_sva_supported(smmu))	/* [한국어] CPU 와 주소 공간을 나눌 수 있는지 검사한다. */
		smmu->features |= ARM_SMMU_FEAT_SVA;

	dev_info(smmu->dev, "oas %lu-bit (features 0x%08x)\n",	/* [한국어] 정해진 성질을 한 줄로 남긴다 — 문제를 되짚을 때 첫 단서가 된다. */
		 smmu->oas, smmu->features);
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

#ifdef CONFIG_TEGRA241_CMDQV	/* [한국어] Tegra 보조 큐 확장을 켜고 빌드했을 때만. */
/*
 * [한국어]
 * tegra_cmdqv_dt_probe - 장치 트리에서 Tegra 보조 큐 장치를 찾는다
 *
 * @smmu_node: 이 SMMU 의 장치 트리 노드.
 * @smmu: 대상 하드웨어.
 *
 * 보조 큐 확장은 별도의 플랫폼 장치로 기술되며, SMMU 노드가 그것을
 * phandle 로 가리킨다. 그 장치를 찾아 기억해 두면 나중에 프로브가
 * 그쪽 초기화를 부른다.
 *
 * 장치 참조를 놓지 않는 것이 요점이다 — 확장 드라이버가 그 참조를
 * 물려받아, 자기가 사라질 때 놓는다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_dt_probe() → [이 함수]
 */
static void tegra_cmdqv_dt_probe(struct device_node *smmu_node,
				 struct arm_smmu_device *smmu)
{
	struct platform_device *pdev;	/* [한국어] 찾은 확장 장치. */
	struct device_node *np;	/* [한국어] 그 노드. */

	np = of_parse_phandle(smmu_node, "nvidia,cmdqv", 0);	/* [한국어] SMMU 노드가 가리키는 확장 노드. */
	if (!np)	/* [한국어] 없으면 이 하드웨어에는 확장이 없다. */
		return;

	/* Tegra241 CMDQV driver is responsible for put_device() */
	/* [한국어] (위 영어 주석 참고) 여기서 올린 참조를 놓지 않는다 — 확장
	 * 드라이버가 물려받아 자기가 사라질 때 놓는다. */
	pdev = of_find_device_by_node(np);	/* [한국어] 그 노드의 플랫폼 장치를 찾는다. */
	of_node_put(np);	/* [한국어] 노드 참조는 여기서 놓는다 — 장치 참조와 별개다. */
	if (!pdev)	/* [한국어] 아직 그 장치가 만들어지지 않았다면. */
		return;

	smmu->impl_dev = &pdev->dev;	/* [한국어] 나중에 확장 프로브가 이 장치를 쓴다. */
	smmu->options |= ARM_SMMU_OPT_TEGRA241_CMDQV;	/* [한국어] 확장이 있다는 표시 — 프로브가 이 비트를 보고 확장 초기화를 부른다. */
	dev_dbg(smmu->dev, "found companion CMDQV device: %s\n",	/* [한국어] 어느 확장 장치를 찾았는지 남긴다. */
		dev_name(smmu->impl_dev));
}
#else	/* [한국어] 확장을 끄고 빌드했으면. */
/*
 * [한국어]
 * tegra_cmdqv_dt_probe - 확장을 끈 빌드에서의 빈 껍데기
 *
 * @smmu_node: 쓰이지 않는다.
 * @smmu: 쓰이지 않는다.
 *
 * 호출부에 #ifdef 를 뿌리지 않으려고 둔 자리다.
 */
static void tegra_cmdqv_dt_probe(struct device_node *smmu_node,
				 struct arm_smmu_device *smmu)
{
}
#endif

#ifdef CONFIG_ACPI	/* [한국어] ACPI 로 하드웨어를 찾는 경로 — 서버 계열이 이쪽이다. */
#ifdef CONFIG_TEGRA241_CMDQV	/* [한국어] 그중에서도 Tegra 확장을 켠 경우. */
/*
 * [한국어]
 * acpi_smmu_dsdt_probe_tegra241_cmdqv - ACPI 표에서 Tegra 보조 큐 장치를 찾는다
 *
 * @node: 이 SMMU 의 IORT 노드.
 * @smmu: 대상 하드웨어.
 *
 * ACPI 에서는 확장 장치가 DSDT 에 별도 장치로 적혀 있고, 그 _UID 가
 * SMMU 의 IORT 노드 번호와 같다는 약속으로 짝을 맞춘다. IORT 표에는
 * 그 관계를 적을 자리가 없어 이런 우회를 쓴다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   acpi_smmu_iort_probe_model() → [이 함수]
 */
static void acpi_smmu_dsdt_probe_tegra241_cmdqv(struct acpi_iort_node *node,
						struct arm_smmu_device *smmu)
{
	const char *uid = kasprintf(GFP_KERNEL, "%u", node->identifier);	/* [한국어] IORT 노드 번호를 문자열로 — _UID 와 견주려면 문자열이어야 한다. */
	struct acpi_device *adev;	/* [한국어] 찾은 ACPI 장치. */

	/* Look for an NVDA200C node whose _UID matches the SMMU node ID */
	/* [한국어] (위 영어 주석 참고) 하드웨어 ID 가 NVDA200C 이고 _UID 가 이
	 * SMMU 의 노드 번호와 같은 장치가 곧 이 SMMU 의 보조 큐다. */
	adev = acpi_dev_get_first_match_dev("NVDA200C", uid, -1);
	if (adev) {	/* [한국어] 찾았다면. */
		/* Tegra241 CMDQV driver is responsible for put_device() */
		/* [한국어] (위 영어 주석 참고) 여기서 올린 참조는 확장 드라이버가 놓는다. */
		smmu->impl_dev = get_device(acpi_get_first_physical_node(adev));	/* [한국어] 그 ACPI 장치에 대응하는 실제 장치. */
		smmu->options |= ARM_SMMU_OPT_TEGRA241_CMDQV;	/* [한국어] 확장이 있다는 표시. */
		dev_info(smmu->dev, "found companion CMDQV device: %s\n",	/* [한국어] 어느 확장 장치를 찾았는지 남긴다. */
			 dev_name(smmu->impl_dev));
		acpi_dev_put(adev);	/* [한국어] ACPI 장치 참조는 여기서 놓는다 — 위에서 실제 장치 참조를 따로 잡았다. */
	}
	kfree(uid);	/* [한국어] 문자열을 놓는다. */
}
#else	/* [한국어] Tegra 확장을 끄고 빌드했으면. */
/*
 * [한국어]
 * acpi_smmu_dsdt_probe_tegra241_cmdqv - 확장을 끈 빌드에서의 빈 껍데기
 *
 * @node: 쓰이지 않는다.
 * @smmu: 쓰이지 않는다.
 */
static void acpi_smmu_dsdt_probe_tegra241_cmdqv(struct acpi_iort_node *node,
						struct arm_smmu_device *smmu)
{
}
#endif

/*
 * [한국어]
 * acpi_smmu_iort_probe_model - IORT 가 알려 준 모델로 결함 우회를 켠다
 *
 * @node: 이 SMMU 의 IORT 노드.
 * @smmu: 대상 하드웨어.
 * @return: 항상 0.
 *
 * 장치 트리에서는 속성 이름으로 결함을 알려 주지만, ACPI 에서는 IORT 가
 * 모델 번호를 알려 주고 드라이버가 그 번호로 판단한다.
 *
 * 일반 모델일 때도 그냥 넘어가지 않고 Tegra 확장을 찾아보는 것이
 * 눈에 띈다 — 주석이 설명하듯 그 구현이 IORT 가 아니라 DSDT 에 정보를
 * 두었기 때문이다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_acpi_probe() → [이 함수]
 */
static int acpi_smmu_iort_probe_model(struct acpi_iort_node *node,
				      struct arm_smmu_device *smmu)
{
	struct acpi_iort_smmu_v3 *iort_smmu =	/* [한국어] IORT 가 이 SMMU 에 대해 적어 둔 내용. */
		(struct acpi_iort_smmu_v3 *)node->node_data;

	switch (iort_smmu->model) {	/* [한국어] 값에 따라 갈래를 나눈다. */
	case ACPI_IORT_SMMU_V3_CAVIUM_CN99XX:	/* [한국어] 두 번째 레지스터 페이지가 없는 구현. */
		smmu->options |= ARM_SMMU_OPT_PAGE0_REGS_ONLY;	/* [한국어] 모든 레지스터를 첫 페이지에서 찾게 한다. */
		break;
	case ACPI_IORT_SMMU_V3_HISILICON_HI161X:	/* [한국어] 설정 미리 읽기 명령이 망가진 구현. */
		smmu->options |= ARM_SMMU_OPT_SKIP_PREFETCH;	/* [한국어] 그 명령을 아예 보내지 않는다. */
		break;
	case ACPI_IORT_SMMU_V3_GENERIC:	/* [한국어] 특별한 결함이 없는 일반 구현. */
		/*
		 * Tegra241 implementation stores its SMMU options and impl_dev
		 * in DSDT. Thus, go through the ACPI tables unconditionally.
		 */
		/* [한국어] (위 영어 주석 참고) Tegra 는 자기를 일반 모델로 적어 두고
		 * 확장 정보는 DSDT 에 따로 둔다. 그래서 모델만 보고 넘어갈 수 없고
		 * 언제나 표를 뒤져 봐야 한다. */
		acpi_smmu_dsdt_probe_tegra241_cmdqv(node, smmu);
		break;
	}

	dev_notice(smmu->dev, "option mask 0x%x\n", smmu->options);	/* [한국어] 켜진 우회를 남긴다 — 성능이나 동작이 이상할 때 단서가 된다. */
	return 0;	/* [한국어] 여기까지 왔으면 성공이다. */
}

/*
 * [한국어]
 * arm_smmu_device_acpi_probe - ACPI IORT 가 알려 준 정보를 받아 담는다
 *
 * @pdev: 플랫폼 장치 (여기서는 쓰지 않는다).
 * @smmu: 채울 하드웨어 상태.
 * @return: 0 성공, 음수 오류.
 *
 * IORT 표에는 능력 레지스터가 말해 주지 않는 정보가 담긴다 — 캐시 일관성과
 * 접근·더티 비트 갱신 능력이 그것이다. 시스템 연결 방식이 함께 정하는
 * 성질이라 하드웨어 혼자 답할 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → acpi_smmu_iort_probe_model()
 */
static int arm_smmu_device_acpi_probe(struct platform_device *pdev,
				      struct arm_smmu_device *smmu)
{
	struct acpi_iort_smmu_v3 *iort_smmu;	/* [한국어] IORT 가 적어 둔 SMMU 정보. */
	struct device *dev = smmu->dev;	/* [한국어] 대상 장치. */
	struct acpi_iort_node *node;	/* [한국어] 그 IORT 노드. */

	node = *(struct acpi_iort_node **)dev_get_platdata(dev);	/* [한국어] 플랫폼 장치를 만들 때 IORT 코드가 붙여 둔 노드 포인터. */

	/* Retrieve SMMUv3 specific data */
	/* [한국어] (위 영어 주석 참고) 노드 뒤에 SMMUv3 전용 내용이 이어진다. */
	iort_smmu = (struct acpi_iort_smmu_v3 *)node->node_data;

	if (iort_smmu->flags & ACPI_IORT_SMMU_V3_COHACC_OVERRIDE)	/* [한국어] 펌웨어가 캐시 일관성을 보장한다고 적었다면. */
		smmu->features |= ARM_SMMU_FEAT_COHERENCY;	/* [한국어] 능력 레지스터보다 이 값을 우선한다 — 시스템 연결이 정하는 성질이기 때문이다. */

	switch (FIELD_GET(ACPI_IORT_SMMU_V3_HTTU_OVERRIDE, iort_smmu->flags)) {	/* [한국어] 접근·더티 갱신 능력도 펌웨어가 적어 준다. */
	case IDR0_HTTU_ACCESS_DIRTY:	/* [한국어] 더티까지. */
		smmu->features |= ARM_SMMU_FEAT_HD;	/* [한국어] 펌웨어가 더티 갱신까지 가능하다고 적었다. */
		fallthrough;	/* [한국어] 더티를 갱신하면 접근 플래그도 갱신한다. */
	case IDR0_HTTU_ACCESS:	/* [한국어] 접근 플래그만. */
		smmu->features |= ARM_SMMU_FEAT_HA;	/* [한국어] 접근 플래그 갱신은 그 경우에도 함께 가능하다. */
	}

	return acpi_smmu_iort_probe_model(node, smmu);	/* [한국어] 모델별 결함 우회를 켠다. */
}
#else	/* [한국어] ACPI 를 끄고 빌드했으면. */
/*
 * [한국어]
 * arm_smmu_device_acpi_probe - ACPI 를 끈 빌드에서의 빈 껍데기
 *
 * @pdev: 쓰이지 않는다.
 * @smmu: 쓰이지 않는다.
 * @return: 항상 -ENODEV — 호출자가 장치 트리 경로로 간다.
 */
static inline int arm_smmu_device_acpi_probe(struct platform_device *pdev,
					     struct arm_smmu_device *smmu)
{
	return -ENODEV;	/* [한국어] 그런 장치가 없다고 답한다. */
}
#endif

/*
 * [한국어]
 * arm_smmu_device_dt_probe - 장치 트리가 알려 준 정보를 받아 담는다
 *
 * @pdev: 플랫폼 장치.
 * @smmu: 채울 하드웨어 상태.
 * @return: 0 성공, -EINVAL 기술이 잘못됨.
 *
 * ACPI 판과 짝을 이루는 장치 트리 경로다. 셀 수를 검사하고, 결함 우회
 * 속성을 읽고, 캐시 일관성을 확인하고, Tegra 확장을 찾는다.
 *
 * 셀 수가 1 이어야 하는 이유는 이 드라이버의 of_xlate 가 인자 하나를
 * 스트림 번호 하나로 읽기 때문이다. 다르면 번호를 잘못 해석하게 된다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → parse_driver_options()
 */
static int arm_smmu_device_dt_probe(struct platform_device *pdev,
				    struct arm_smmu_device *smmu)
{
	struct device *dev = &pdev->dev;	/* [한국어] 대상 장치. */
	u32 cells;	/* [한국어] iommus 속성이 인자를 몇 개 쓰는가. */
	int ret = -EINVAL;	/* [한국어] 기본은 실패 — 검사를 통과해야 0 이 된다. */

	if (of_property_read_u32(dev->of_node, "#iommu-cells", &cells))	/* [한국어] 그 속성이 아예 없다면. */
		dev_err(dev, "missing #iommu-cells property\n");	/* [한국어] 장치들이 이 SMMU 를 가리킬 방법이 없다. */
	else if (cells != 1)	/* [한국어] 1 이 아니라면. */
		dev_err(dev, "invalid #iommu-cells value (%d)\n", cells);	/* [한국어] 이 드라이버의 of_xlate 가 인자 하나만 읽는다. */
	else
		ret = 0;	/* [한국어] 검사를 통과했다. */

	parse_driver_options(smmu);	/* [한국어] 결함 우회 속성을 읽는다. */

	if (of_dma_is_coherent(dev->of_node))	/* [한국어] 장치 트리가 캐시 일관성을 선언했다면. */
		smmu->features |= ARM_SMMU_FEAT_COHERENCY;	/* [한국어] 능력 레지스터보다 이 값을 우선한다. */

	if (of_device_is_compatible(dev->of_node, "nvidia,tegra264-smmu"))	/* [한국어] 보조 큐를 가진 하드웨어라면. */
		tegra_cmdqv_dt_probe(dev->of_node, smmu);	/* [한국어] 그 확장 장치를 찾아 둔다. */

	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_resource_size - 매핑할 레지스터 창의 크기를 정한다
 *
 * @smmu: 대상 하드웨어.
 * @return: 매핑할 바이트 수.
 *
 * 규격은 64K 페이지 두 개를 요구하지만, 둘째 페이지가 없는 구현이 있다.
 * 그 경우에는 첫 페이지만 매핑하고, 둘째 페이지에 있어야 할 레지스터도
 * 첫 페이지에서 찾는다.
 *
 * 실행 컨텍스트: 프로브. 잠들지 않는다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수]
 */
static unsigned long arm_smmu_resource_size(struct arm_smmu_device *smmu)
{
	if (smmu->options & ARM_SMMU_OPT_PAGE0_REGS_ONLY)	/* [한국어] 둘째 페이지가 없는 구현이면. */
		return SZ_64K;	/* [한국어] 첫 페이지만. */
	else
		return SZ_128K;	/* [한국어] 규격대로 두 페이지. */
}

/*
 * [한국어]
 * arm_smmu_ioremap - 주어진 물리 구간을 매핑한다
 *
 * @dev: 매핑을 소유할 장치 (devres 로 자동 해제된다).
 * @start: 시작 물리 주소.
 * @size: 길이.
 * @return: 매핑된 커널 주소, 실패하면 ERR_PTR.
 *
 * 자원 구조체를 만들어 표준 매핑 함수에 넘기는 얇은 껍데기다. 이 드라이버는
 * 자원 하나를 부분적으로 나눠 매핑해야 해서, 플랫폼이 준 자원을 그대로
 * 쓰는 대신 직접 구간을 짜야 한다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → devm_ioremap_resource()
 */
static void __iomem *arm_smmu_ioremap(struct device *dev, resource_size_t start,
				      resource_size_t size)
{
	struct resource res = DEFINE_RES_MEM(start, size);	/* [한국어] 임시 자원 구조체 — 매핑 함수가 그 형태를 요구한다. */

	return devm_ioremap_resource(dev, &res);	/* [한국어] devres 판이라 장치가 사라질 때 저절로 풀린다. */
}

/*
 * [한국어]
 * arm_smmu_rmr_install_bypass_ste - 펌웨어가 예약한 스트림을 통과로 열어 둔다
 *
 * @smmu: 대상 하드웨어.
 *
 * 펌웨어가 부팅 중에 쓰던 장치가 있다 — 화면 출력이나 부팅 디스크 같은
 * 것들이다. 그 장치의 DMA 는 커널이 인계받기 전까지 끊기면 안 되므로,
 * IORT 의 예약 메모리 구간(RMR)이 그 스트림 번호를 알려 준다.
 *
 * 그 번호의 항목을 통과로 만들어 두면, SMMU 를 켜는 순간에도 그 DMA 가
 * 계속 동작한다. 나중에 실제 드라이버가 그 장치를 프로브하며 제대로 된
 * 도메인으로 바꾼다.
 *
 * 주석이 밝히듯 아직 표가 하드웨어에 걸리기 전이라, 쓰기 규약 없이
 * 곧바로 항목을 채워도 된다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → arm_smmu_make_bypass_ste()
 */
static void arm_smmu_rmr_install_bypass_ste(struct arm_smmu_device *smmu)
{
	struct list_head rmr_list;	/* [한국어] 펌웨어가 알려 준 예약 구간들. */
	struct iommu_resv_region *e;	/* [한국어] 목록 반복자. */

	INIT_LIST_HEAD(&rmr_list);	/* [한국어] 빈 목록으로 시작한다. */
	iort_get_rmr_sids(dev_fwnode(smmu->dev), &rmr_list);	/* [한국어] 이 SMMU 뒤의 예약 구간과 그 스트림 번호를 받아 온다. */

	list_for_each_entry(e, &rmr_list, list) {	/* [한국어] 구간마다. */
		struct iommu_iort_rmr_data *rmr;	/* [한국어] 그 구간의 스트림 번호 목록. */
		int ret, i;	/* [한국어] 결과와 반복자. */

		rmr = container_of(e, struct iommu_iort_rmr_data, rr);	/* [한국어] 예약 구간 구조에서 IORT 전용 구조로 되짚는다. */
		for (i = 0; i < rmr->num_sids; i++) {	/* [한국어] 그 구간을 쓰는 스트림마다. */
			ret = arm_smmu_init_sid_strtab(smmu, rmr->sids[i]);	/* [한국어] 표에 그 자리를 마련한다. */
			if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
				dev_err(smmu->dev, "RMR SID(0x%x) bypass failed\n",	/* [한국어] 자리를 못 만들면 그 장치의 DMA 가 SMMU 를 켜는 순간 끊긴다. */
					rmr->sids[i]);
				continue;	/* [한국어] 나머지 스트림은 계속 처리한다. */
			}

			/*
			 * STE table is not programmed to HW, see
			 * arm_smmu_initial_bypass_stes()
			 */
			/* [한국어] (위 영어 주석 참고) 아직 표가 하드웨어에 걸리기 전이라
			 * 아무도 이 메모리를 읽지 않는다. 그래서 쓰기 규약과 무효화 없이
			 * 곧바로 채워도 안전하다. */
			arm_smmu_make_bypass_ste(smmu,	/* [한국어] 변환 없이 통과시키는 항목을 그 자리에 직접 짓는다. */
				arm_smmu_get_step_for_sid(smmu, rmr->sids[i]));
		}
	}

	iort_put_rmr_sids(dev_fwnode(smmu->dev), &rmr_list);	/* [한국어] 받아 온 목록을 돌려준다. */
}

/*
 * [한국어]
 * arm_smmu_impl_remove - 확장이 잡은 자원을 거둔다
 *
 * @data: 등록할 때 넘긴 SMMU 포인터.
 *
 * devres 가 되감기 때 부르는 콜백이다. 확장 초기화 직후에 등록해 두면,
 * 그 뒤의 프로브가 어디서 실패하든 확장 자원이 새지 않는다.
 *
 * 실행 컨텍스트: 장치 해제·프로브 되감기. 잠들 수 있다.
 *
 * 호출 체인:
 *   devres 되감기 → [이 함수] → impl_ops->device_remove
 */
static void arm_smmu_impl_remove(void *data)
{
	struct arm_smmu_device *smmu = data;	/* [한국어] 등록할 때 넘겨 둔 하드웨어. */

	if (smmu->impl_ops && smmu->impl_ops->device_remove)	/* [한국어] 확장이 정리 갈고리를 제공한다면. */
		smmu->impl_ops->device_remove(smmu);	/* [한국어] 그쪽에 맡긴다. */
}

/*
 * Probe all the compiled in implementations. Each one checks to see if it
 * matches this HW and if so returns a devm_krealloc'd arm_smmu_device which
 * replaces the callers. Otherwise the original is returned or ERR_PTR.
 */
/*
 * [한국어]
 * arm_smmu_impl_probe - 이 하드웨어에 맞는 확장이 있는지 알아본다
 *
 * @smmu: 표준 프로브가 만든 하드웨어 상태.
 * @return: 확장을 품은 새 포인터(또는 원본 그대로), 실패하면 ERR_PTR.
 *
 * (위 영어 주석 참고) 확장이 있으면 구조체를 더 큰 것으로 바꿔 돌려주는
 * 것이 이 함수의 규약이다. 확장 구조체가 표준 구조체를 첫 필드로 품고
 * 있어, 늘린 뒤에도 같은 포인터로 표준 코드가 동작한다.
 *
 * 갈고리 짝을 검사하는 대목이 눈에 띈다 — 크기를 묻는 갈고리와 초기화
 * 갈고리가 짝을 이루지 않으면, 크기는 답했는데 초기화할 수 없는 상태가
 * 되어 나중에 널 포인터를 부르게 된다.
 *
 * 실행 컨텍스트: 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   arm_smmu_device_probe() → [이 함수] → tegra241_cmdqv_probe()
 */
static struct arm_smmu_device *arm_smmu_impl_probe(struct arm_smmu_device *smmu)
{
	struct arm_smmu_device *new_smmu = ERR_PTR(-ENODEV);	/* [한국어] 기본은 "맞는 확장 없음". */
	const struct arm_smmu_impl_ops *ops;	/* [한국어] 확장이 건 갈고리표. */
	int ret;	/* [한국어] 중간 결과. */

	if (smmu->impl_dev && (smmu->options & ARM_SMMU_OPT_TEGRA241_CMDQV))	/* [한국어] 펌웨어 단계에서 Tegra 확장을 찾아 두었다면. */
		new_smmu = tegra241_cmdqv_probe(smmu);	/* [한국어] 그쪽 프로브를 부른다. */

	if (new_smmu == ERR_PTR(-ENODEV))	/* [한국어] 맞는 확장이 없다면. */
		return smmu;	/* [한국어] 원본을 그대로 쓴다 — 실패가 아니다. */
	if (IS_ERR(new_smmu))	/* [한국어] 확장은 있는데 초기화가 실패했다면. */
		return new_smmu;	/* [한국어] 그 오류를 위로 올린다. */

	ops = new_smmu->impl_ops;	/* [한국어] 확장이 건 갈고리표. */
	if (ops) {	/* [한국어] 확장이 갈고리표를 걸었다면. */
		/* get_viommu_size and vsmmu_init ops must be paired */
		/* [한국어] (위 영어 주석 참고) 크기를 묻는 갈고리만 있고 초기화가 없으면,
		 * 크기는 답했는데 만들 수 없는 상태가 되어 널 포인터를 부르게 된다.
		 * 논리 부정으로 "둘 다 있거나 둘 다 없거나"를 검사한다. */
		if (WARN_ON(!ops->get_viommu_size != !ops->vsmmu_init)) {
			ret = -EINVAL;	/* [한국어] 갈고리 짝이 어긋났다 — 나중에 널 포인터를 부르게 된다. */
			goto err_remove;	/* [한국어] 확장이 잡은 것을 거두고 나간다. */
		}
	}

	ret = devm_add_action_or_reset(new_smmu->dev, arm_smmu_impl_remove,	/* [한국어] 이 뒤의 프로브가 어디서 실패해도 확장 자원이 새지 않게 한다. */
				       new_smmu);
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ERR_PTR(ret);	/* [한국어] 등록에 실패하면 그 함수가 이미 정리를 불러 준다. */
	return new_smmu;	/* [한국어] 확장을 품은 더 큰 구조체 — 표준 코드가 그대로 쓸 수 있다. */

err_remove:	/* [한국어] 갈고리표가 어긋났을 때. */
	arm_smmu_impl_remove(new_smmu);	/* [한국어] 확장이 잡은 것을 곧바로 거둔다 — 아직 devres 에 등록하기 전이다. */
	return ERR_PTR(ret);	/* [한국어] 실패 이유를 오류 포인터로 감싸 돌려준다. */
}

/*
 * [한국어]
 * arm_smmu_device_probe - SMMU 하드웨어 하나를 찾아 쓸 수 있게 만든다
 *
 * @pdev: 플랫폼이 알려 준 장치.
 * @return: 0 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다. 순서가 곧 안전이며, 큰 흐름은 이렇다 —
 * 펌웨어 정보를 읽고, 확장을 찾고, 레지스터를 매핑하고, 인터럽트 번호를
 * 챙기고, 능력을 읽고, 자료 구조를 만들고, 펌웨어가 쓰던 스트림을 열어
 * 두고, 하드웨어를 리셋해 켜고, 마지막에 iommu 코어에 등록한다.
 *
 * 코어 등록을 마지막에 하는 것이 중요하다. 등록하는 순간부터 장치들이
 * 프로브되며 이 드라이버를 부르기 시작하므로, 그전에 모든 준비가 끝나
 * 있어야 한다.
 *
 * 예약 스트림을 리셋 전에 열어 두는 것도 요점이다 — 리셋이 SMMU 를 켜는
 * 순간 그 항목이 이미 통과로 채워져 있어야 펌웨어가 쓰던 DMA 가 끊기지
 * 않는다.
 *
 * 실행 컨텍스트: 드라이버 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → .probe = [이 함수]
 *     → arm_smmu_device_hw_probe() → arm_smmu_init_structures()
 *     → arm_smmu_device_reset() → iommu_device_register()
 */
static int arm_smmu_device_probe(struct platform_device *pdev)
{
	int irq, ret;	/* [한국어] 인터럽트 번호와 중간 결과. */
	struct resource *res;	/* [한국어] MMIO 자원. */
	resource_size_t ioaddr;	/* [한국어] 그 물리 주소 — sysfs 이름에도 쓴다. */
	struct arm_smmu_device *smmu;	/* [한국어] 만들 하드웨어 상태. */
	struct device *dev = &pdev->dev;	/* [한국어] 대상 장치. */

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);	/* [한국어] devres 로 잡는다 — 확장이 있으면 나중에 이 할당을 늘린다. */
	if (!smmu)	/* [한국어] 하드웨어 상태를 못 잡았으면. */
		return -ENOMEM;
	smmu->dev = dev;	/* [한국어] 되짚어 갈 수 있게. */

	if (dev->of_node) {	/* [한국어] 장치 트리로 찾은 하드웨어라면. */
		ret = arm_smmu_device_dt_probe(pdev, smmu);	/* [한국어] 장치 트리 경로. */
	} else {	/* [한국어] 아니면 ACPI. */
		ret = arm_smmu_device_acpi_probe(pdev, smmu);	/* [한국어] ACPI 경로. */
	}
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;	/* [한국어] 펌웨어 기술이 잘못됐다. */

	smmu = arm_smmu_impl_probe(smmu);	/* [한국어] 확장이 있으면 구조체가 더 큰 것으로 바뀐다. */
	if (IS_ERR(smmu))	/* [한국어] 확장 프로브가 실패했다면. */
		return PTR_ERR(smmu);

	/* Base address */
	/* [한국어] (위 영어 주석 참고) 레지스터 창을 찾아 검사한다. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)	/* [한국어] 레지스터 창이 없으면 다룰 수 없다. */
		return -EINVAL;
	if (resource_size(res) < arm_smmu_resource_size(smmu)) {	/* [한국어] 창이 필요한 만큼 크지 않으면. */
		dev_err(dev, "MMIO region too small (%pr)\n", res);	/* [한국어] 펌웨어 기술이 잘못된 것이다. */
		return -EINVAL;	/* [한국어] 인자나 상태가 조건에 맞지 않는다. */
	}
	ioaddr = res->start;	/* [한국어] sysfs 이름에 쓸 물리 주소를 기억해 둔다. */

	/*
	 * Don't map the IMPLEMENTATION DEFINED regions, since they may contain
	 * the PMCG registers which are reserved by the PMU driver.
	 */
	/* [한국어] (위 영어 주석 참고) 창 전체가 아니라 규격이 정의한 부분만
	 * 매핑한다. 나머지에는 성능 카운터 레지스터가 있을 수 있고, 그것은
	 * 다른 드라이버가 잡아 두므로 겹치면 매핑이 실패한다. */
	smmu->base = arm_smmu_ioremap(dev, ioaddr, ARM_SMMU_REG_SZ);	/* [한국어] 첫 페이지의 규격 정의 부분만. */
	if (IS_ERR(smmu->base))	/* [한국어] 첫 페이지를 못 매핑했다면. */
		return PTR_ERR(smmu->base);

	if (arm_smmu_resource_size(smmu) > SZ_64K) {	/* [한국어] 둘째 페이지를 가진 정상 구현이면. */
		smmu->page1 = arm_smmu_ioremap(dev, ioaddr + SZ_64K,	/* [한국어] 그 페이지도 매핑한다 — 큐 포인터가 여기 있다. */
					       ARM_SMMU_REG_SZ);
		if (IS_ERR(smmu->page1))	/* [한국어] 둘째 페이지를 못 매핑했다면. */
			return PTR_ERR(smmu->page1);
	} else {	/* [한국어] 둘째 페이지가 없는 결함 구현이면. */
		smmu->page1 = smmu->base;	/* [한국어] 같은 자리를 가리키게 해, 이후 코드가 조건 없이 page1 을 쓸 수 있게 한다. */
	}

	/* Interrupt lines */
	/* [한국어] (위 영어 주석 참고) 펌웨어가 이름으로 알려 준 인터럽트를 챙긴다.
	 * 없어도 프로브를 접지 않는다 — MSI 로 받거나, 없이도 동작한다. */

	irq = platform_get_irq_byname_optional(pdev, "combined");	/* [한국어] 하나로 묶인 선이 있는가. */
	if (irq > 0)	/* [한국어] 펌웨어가 그 이름의 인터럽트를 알려 줬다면. */
		smmu->combined_irq = irq;	/* [한국어] 그러면 종류별 선은 보지 않는다. */
	else {
		irq = platform_get_irq_byname_optional(pdev, "eventq");	/* [한국어] 폴트 보고. */
		if (irq > 0)	/* [한국어] 이벤트 큐 인터럽트를 찾았다면. */
			smmu->evtq.q.irq = irq;

		irq = platform_get_irq_byname_optional(pdev, "priq");	/* [한국어] 페이지 요청. */
		if (irq > 0)	/* [한국어] 페이지 요청 큐 인터럽트를 찾았다면. */
			smmu->priq.q.irq = irq;

		irq = platform_get_irq_byname_optional(pdev, "gerror");	/* [한국어] 전역 오류. */
		if (irq > 0)	/* [한국어] 전역 오류 인터럽트를 찾았다면. */
			smmu->gerr_irq = irq;
	}
	/* Probe the h/w */
	/* [한국어] (위 영어 주석 참고) 이제 레지스터를 읽을 수 있으니 능력을 알아본다. */
	ret = arm_smmu_device_hw_probe(smmu);
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		return ret;	/* [한국어] 이 드라이버가 다룰 수 없는 하드웨어다. */

	/* Initialise in-memory data structures */
	/* [한국어] (위 영어 주석 참고) 능력을 알았으니 그에 맞는 크기로 큐와 표를 잡는다. */
	ret = arm_smmu_init_structures(smmu);
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		goto err_free_iopf;

	/* Record our private device structure */
	/* [한국어] (위 영어 주석 참고) 장치를 프로브할 때 펌웨어 노드로 이 상태를
	 * 되찾는데, 그 경로가 이 값을 읽는다. */
	platform_set_drvdata(pdev, smmu);

	/* Check for RMRs and install bypass STEs if any */
	/* [한국어] (위 영어 주석 참고) 반드시 리셋 전에 해야 한다 — 리셋이 SMMU 를
	 * 켜는 순간 그 항목이 이미 통과로 채워져 있어야 펌웨어가 쓰던 DMA 가
	 * 끊기지 않는다. */
	arm_smmu_rmr_install_bypass_ste(smmu);

	/* Reset the device */
	/* [한국어] (위 영어 주석 참고) 하드웨어를 알려진 상태로 만들고 켠다. */
	ret = arm_smmu_device_reset(smmu);
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		goto err_disable;

	/* And we're up. Go go go! */
	/* [한국어] (위 영어 주석 참고) sysfs 에 이름을 올린다 — 물리 주소를 넣어
	 * 여러 SMMU 를 구분할 수 있게 한다. */
	ret = iommu_device_sysfs_add(&smmu->iommu, dev, NULL,
				     "smmu3.%pa", &ioaddr);
	if (ret)	/* [한국어] 앞 단계가 실패했으면 여기서 접는다. */
		goto err_disable;

	ret = iommu_device_register(&smmu->iommu, &arm_smmu_ops, dev);	/* [한국어] 마지막 단계 — 이 순간부터 장치들이 프로브되며 우리를 부르기 시작한다. */
	if (ret) {	/* [한국어] 앞 단계가 실패한 경우. */
		dev_err(dev, "Failed to register iommu\n");	/* [한국어] 코어 등록에 실패하면 이 SMMU 를 쓸 수 없다. */
		goto err_free_sysfs;	/* [한국어] sysfs 항목부터 되돌린다. */
	}

	return 0;	/* [한국어] 이 SMMU 가 살아났다. */

err_free_sysfs:	/* [한국어] sysfs 에 올린 뒤 실패했을 때. */
	iommu_device_sysfs_remove(&smmu->iommu);	/* [한국어] sysfs 에 올린 이름을 지운다. */
err_disable:	/* [한국어] 하드웨어를 켠 뒤 실패했을 때. */
	arm_smmu_device_disable(smmu);	/* [한국어] 반쯤 켜진 채로 두면 안 된다. */
err_free_iopf:	/* [한국어] 폴트 큐를 만든 뒤 실패했을 때. */
	iopf_queue_free(smmu->evtq.iopf);	/* [한국어] devres 로 잡히지 않아 직접 놓아야 한다. NULL 이어도 안전하다. */
	return ret;	/* [한국어] 중간 단계의 결과를 그대로 호출자에게 넘긴다. */
}

/*
 * [한국어]
 * arm_smmu_device_remove - 이 SMMU 를 내린다
 *
 * @pdev: 대상 플랫폼 장치.
 *
 * 프로브의 역순으로 되돌린다. 코어에서 먼저 등록을 풀어 새 요청이 들어오지
 * 못하게 한 뒤, 하드웨어를 끄고 자원을 거둔다.
 *
 * 나머지 자원(매핑, 큐, 표)은 devres 가 알아서 놓는다.
 *
 * 실행 컨텍스트: 드라이버 제거. 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → .remove = [이 함수] → iommu_device_unregister()
 */
static void arm_smmu_device_remove(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);	/* [한국어] 프로브가 붙여 둔 상태. */

	iommu_device_unregister(&smmu->iommu);	/* [한국어] 먼저 코어에서 뗀다 — 새 요청이 들어오지 못하게. */
	iommu_device_sysfs_remove(&smmu->iommu);	/* [한국어] sysfs 항목을 지운다. */
	arm_smmu_device_disable(smmu);	/* [한국어] 하드웨어를 끈다. */
	iopf_queue_free(smmu->evtq.iopf);	/* [한국어] 폴트 큐를 놓는다. */
	ida_destroy(&smmu->vmid_map);	/* [한국어] VMID 할당기를 정리한다. */
}

/*
 * [한국어]
 * arm_smmu_device_shutdown - 시스템이 꺼질 때 SMMU 를 멈춘다
 *
 * @pdev: 대상 플랫폼 장치.
 *
 * kexec 로 다음 커널을 띄우거나 전원을 내릴 때, 변환이 켜진 채로 두면
 * 다음 커널이 알 수 없는 매핑을 물려받는다. 그래서 꺼 둔다.
 *
 * 자원을 거두지는 않는데, 곧 시스템이 사라지므로 뜻이 없기 때문이다.
 *
 * 실행 컨텍스트: 시스템 종료. 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 버스 → .shutdown = [이 함수] → arm_smmu_device_disable()
 */
static void arm_smmu_device_shutdown(struct platform_device *pdev)
{
	struct arm_smmu_device *smmu = platform_get_drvdata(pdev);	/* [한국어] 프로브가 붙여 둔 상태. */

	arm_smmu_device_disable(smmu);	/* [한국어] 변환을 멈춘다 — 다음 커널이 깨끗한 상태에서 시작하도록. */
}

/* [한국어] 장치 트리에서 이 드라이버가 맡을 노드를 알아보는 표.
 *
 * 규격을 따르는 구현이면 모두 같은 문자열을 쓴다 — 결함별 차이는
 * 별도 속성으로 알려 주므로 여기서 나눌 필요가 없다. */
static const struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v3", },	/* [한국어] 규격을 따르는 모든 구현. */
	{ },	/* [한국어] 표의 끝을 알리는 빈 항목. */
};
MODULE_DEVICE_TABLE(of, arm_smmu_of_match);	/* [한국어] 모듈 자동 적재가 이 표를 보고 동작한다. */

/*
 * [한국어]
 * arm_smmu_driver_unregister - 드라이버를 내리기 전에 남은 알림을 기다린다
 *
 * @drv: 이 드라이버.
 *
 * SVA 의 mmu_notifier 콜백이 아직 돌고 있으면, 모듈 코드가 사라진 뒤에
 * 그 함수를 부르게 된다. 그래서 등록을 풀기 전에 남은 콜백이 다
 * 빠져나가기를 기다린다.
 *
 * 실행 컨텍스트: 모듈 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   module_exit → [이 함수] → arm_smmu_sva_notifier_synchronize()
 */
static void arm_smmu_driver_unregister(struct platform_driver *drv)
{
	arm_smmu_sva_notifier_synchronize();	/* [한국어] 남은 알림 콜백이 다 끝나기를 기다린다 — SVA 를 끄고 빌드하면 빈 함수다. */
	platform_driver_unregister(drv);	/* [한국어] 그다음 드라이버를 뗀다. */
}

/* [한국어] 플랫폼 버스에 등록할 드라이버 서술.
 *
 * suppress_bind_attrs 가 눈에 띈다 — sysfs 로 이 드라이버를 임의로
 * 떼었다 붙였다 할 수 없게 막는다. IOMMU 를 실행 중에 떼면 그 뒤의
 * 모든 장치가 매핑을 잃기 때문이다. */
static struct platform_driver arm_smmu_driver = {
	.driver	= {	/* [한국어] 플랫폼 버스가 보는 드라이버 정보. */
		.name			= "arm-smmu-v3",	/* [한국어] sysfs 와 로그에 쓰일 이름. */
		.of_match_table		= arm_smmu_of_match,	/* [한국어] 장치 트리 매칭 표. ACPI 는 플랫폼 장치 이름으로 짝지어진다. */
		.suppress_bind_attrs	= true,	/* [한국어] 실행 중 떼기를 막는다 — 뗀 순간 뒤의 모든 장치가 매핑을 잃는다. */
	},
	.probe	= arm_smmu_device_probe,	/* [한국어] 하드웨어를 찾아 켠다. */
	.remove = arm_smmu_device_remove,	/* [한국어] 자원을 거둔다. */
	.shutdown = arm_smmu_device_shutdown,	/* [한국어] 시스템 종료 때 변환을 멈춘다. */
};
module_driver(arm_smmu_driver, platform_driver_register,	/* [한국어] 등록과 해제를 한 번에 정의한다. */
	      arm_smmu_driver_unregister);	/* [한국어] 해제는 SVA 알림을 기다리는 판을 쓴다. */

MODULE_DESCRIPTION("IOMMU API for ARM architected SMMUv3 implementations");	/* [한국어] modinfo 에 찍힐 설명. */
MODULE_AUTHOR("Will Deacon <will@kernel.org>");	/* [한국어] 원작자. */
MODULE_ALIAS("platform:arm-smmu-v3");	/* [한국어] 플랫폼 장치 이름으로도 자동 적재되게 한다 — ACPI 경로가 이 별칭을 쓴다. */
MODULE_LICENSE("GPL v2");	/* [한국어] GPL 심볼을 쓸 수 있게 하는 라이선스 선언. */
