/* SPDX-License-Identifier: GPL-2.0 */

/*
 * [한국어 설명] VT-d 하드웨어 성능 카운터의 레지스터 배치 정의 (intel/perfmon.h)
 *
 * === 파일의 역할 ===
 * 유닛에 내장된 성능 카운터(PerfMon)의 레지스터 오프셋과 비트필드를 정의한다.
 * perf.c 의 소프트웨어 지연 계측과는 다른 것이다 — 이쪽은 하드웨어가 직접
 * 세는 이벤트(번역 횟수, 캐시 적중/실패, 페이지 워크 횟수 등)이며, 커널은
 * 카운터를 설정하고 값을 읽기만 한다.
 * 정의하는 것은 셋이다.
 *   [1] 레지스터 영역의 배치 — 설정, 필터, 카운터, 오버플로 레지스터가
 *       각각 어디서 시작하고 얼마 간격으로 놓이는지.
 *   [2] 능력 레지스터의 비트필드를 뽑는 매크로(iommu_cntrcap_*).
 *   [3] 필터 종류 — 어떤 요청만 셀지 좁히는 조건들.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 진단 전용 곁가지이며 번역 동작에는 관여하지 않는다.
 *   유저스페이스 perf → perf 코어 → perfmon.c 의 PMU 콜백
 *   → [이 헤더의 오프셋으로 계산한 주소] → 유닛의 MMIO 레지스터
 * 오프셋을 헤더로 뺀 이유는, 그 계산이 능력 레지스터에서 읽은 값과 여러
 * 상수의 조합이라 코드에 흩어 두면 틀리기 쉽기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * - perfmon.c: 이 헤더의 정의로 카운터를 설정하고 읽는다.
 * - iommu.h: struct iommu_pmu 가 여기서 계산한 주소들을 담고,
 *   DMAR_PERF*_REG 오프셋이 이 영역의 시작을 알려 준다.
 * - perf 서브시스템: iommu_pmu_register() 로 등록되어 perf stat 으로 쓰인다.
 * - 확장 명령(ecmd): 카운터를 켜고 끄고 멈추는 것은 MMIO 가 아니라 확장
 *   명령으로 한다 — iommu.h 의 DMA_ECMD_ECCAP3_ESSENTIAL 이 그 넷을 확인한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - IOMMU_PMU_CFG_* : 설정 레지스터 영역 안의 오프셋들. 카운터마다
 *   설정·필터·능력 레지스터가 일정 간격으로 반복된다.
 * - iommu_cntrcap_* : 카운터 능력 레지스터의 비트필드 — 카운터 폭, 지원
 *   이벤트 그룹 수 등.
 * - IOMMU_PMU_FILTER_* : 요청자 id, 도메인, PASID, ATS, 페이지 테이블 종류로
 *   세는 대상을 좁히는 필터들.
 * - alloc_iommu_pmu()/free/register/unregister: perfmon.c 가 구현하는
 *   수명 인터페이스. CONFIG_INTEL_IOMMU_PERF_EVENTS 를 끄면 빈 구현이 된다.
 */
/*
 * PERFCFGOFF_REG, PERFFRZOFF_REG
 * PERFOVFOFF_REG, PERFCNTROFF_REG
 */
#define IOMMU_PMU_NUM_OFF_REGS			4	/* [한국어] 오프셋 레지스터의 개수 — 설정, 정지, 오버플로, 카운터 넷이다 (위 영어 주석) */
#define IOMMU_PMU_OFF_REGS_STEP			4	/* [한국어] 그 레지스터들 사이의 간격(바이트) */

#define IOMMU_PMU_FILTER_REQUESTER_ID		0x01	/* [한국어] 특정 소스 id 의 요청만 센다 */
#define IOMMU_PMU_FILTER_DOMAIN			0x02	/* [한국어] 특정 도메인 id 의 요청만 */
#define IOMMU_PMU_FILTER_PASID			0x04	/* [한국어] 특정 PASID 의 요청만 */
#define IOMMU_PMU_FILTER_ATS			0x08	/* [한국어] ATS 요청인지로 구분 */
#define IOMMU_PMU_FILTER_PAGE_TABLE		0x10	/* [한국어] 어떤 페이지 테이블(1단계/2단계 등)을 쓰는 요청인지로 구분 */

#define IOMMU_PMU_FILTER_EN			BIT(31)	/* [한국어] 필터를 실제로 적용하라는 활성화 비트. 조건만 써 두고 이 비트를 켜지 않으면 필터가 동작하지 않는다 */

#define IOMMU_PMU_CFG_OFFSET			0x100	/* [한국어] 설정 레지스터 영역이 시작되는 오프셋 */
#define IOMMU_PMU_CFG_CNTRCAP_OFFSET		0x80	/* [한국어] 그 안에서 카운터 능력 레지스터의 위치 */
#define IOMMU_PMU_CFG_CNTREVCAP_OFFSET		0x84	/* [한국어] 카운터별 이벤트 능력 레지스터의 위치 */
#define IOMMU_PMU_CFG_SIZE			0x8	/* [한국어] 카운터 하나가 차지하는 설정 영역의 크기 */
#define IOMMU_PMU_CFG_FILTERS_OFFSET		0x4	/* [한국어] 그 안에서 필터 레지스터의 위치 */

#define IOMMU_PMU_CAP_REGS_STEP			8	/* [한국어] 능력 레지스터들 사이의 간격 */

#define iommu_cntrcap_pcc(p)			((p) & 0x1)	/* [한국어] 이 카운터가 프로그램 가능한지(Programmable Counter Capability) */
#define iommu_cntrcap_cw(p)			(((p) >> 8) & 0xff)	/* [한국어] 카운터의 비트 폭. 랩어라운드를 올바로 처리하려면 필요하다 */
#define iommu_cntrcap_ios(p)			(((p) >> 16) & 0x1)	/* [한국어] 인터럽트 오버플로 지원 여부 */
#define iommu_cntrcap_egcnt(p)			(((p) >> 28) & 0xf)	/* [한국어] 이 카운터가 지원하는 이벤트 그룹 수 */

#define IOMMU_EVENT_CFG_EGI_SHIFT		8	/* [한국어] 이벤트 그룹 인덱스가 설정 레지스터에서 차지하는 위치 */
#define IOMMU_EVENT_CFG_ES_SHIFT		32	/* [한국어] 이벤트 선택 필드의 위치 */
#define IOMMU_EVENT_CFG_INT			BIT_ULL(1)	/* [한국어] 오버플로 시 인터럽트를 내라는 비트 */

#define iommu_event_select(p)			((p) & 0xfffffff)	/* [한국어] perf 이벤트 설정값에서 하드웨어 이벤트 번호를 뽑는다 */
#define iommu_event_group(p)			(((p) >> 28) & 0xf)	/* [한국어] 그 이벤트가 속한 그룹 번호를 뽑는다. 카운터마다 지원하는 그룹이 달라 이 값으로 배정할 카운터를 고른다 */

#ifdef CONFIG_INTEL_IOMMU_PERF_EVENTS	/* [한국어] 성능 카운터를 켠 빌드에서만 실제 구현을 쓴다 */
int alloc_iommu_pmu(struct intel_iommu *iommu);	/* [한국어] 유닛의 PMU 구조체를 만들고 능력을 읽어 채운다 */
void free_iommu_pmu(struct intel_iommu *iommu);	/* [한국어] 그것을 반납한다 */
void iommu_pmu_register(struct intel_iommu *iommu);	/* [한국어] perf 서브시스템에 등록해 perf stat 으로 쓸 수 있게 한다 */
void iommu_pmu_unregister(struct intel_iommu *iommu);	/* [한국어] 등록을 해제한다 */
#else
/*
 * [한국어]
 * alloc_iommu_pmu - (CONFIG_INTEL_IOMMU_PERF_EVENTS 미설정) PMU 를 만들지 않고 성공을 답한다
 *
 * @iommu:  PMU 를 붙이려던 IOMMU 유닛. 여기서는 쓰이지 않는다.
 * @return: 항상 0(성공).
 *
 * 켠 빌드에서는 유닛의 성능 카운터 능력(카운터 개수, 지원 이벤트 그룹, MMIO
 * 오프셋)을 읽어 struct iommu_pmu 를 채운다. 끈 빌드에는 그 구조체가 없다.
 *
 * 여기서 0(성공)을 돌려주는 것이 이 파일의 다른 스텁과 다른 점이자 핵심이다.
 * 호출자인 intel_iommu_init() 의 유닛 초기화 경로는 이 반환값이 음수면 그
 * IOMMU 유닛 자체의 초기화를 실패로 처리한다. 성능 카운터가 없다는 이유로
 * IOMMU 를 못 쓰게 만들 수는 없으므로, "만들 것이 없으니 할 일을 다 했다"는
 * 뜻으로 0 을 준다.
 *
 * 실행 컨텍스트: 부팅 중 유닛 초기화(프로세스 문맥).
 *
 * 호출 체인:
 *   init_iommu_hw()/intel_iommu_init() → [이 빈 구현]
 */
static inline int
alloc_iommu_pmu(struct intel_iommu *iommu)	/* [한국어] 성능 카운터를 끈 빌드의 빈 구현들 */
{
	return 0;	/* [한국어] 만들 것이 없으니 성공으로 답한다 — 호출자가 실패로 오해하지 않게 */
}

/*
 * [한국어]
 * free_iommu_pmu - (CONFIG_INTEL_IOMMU_PERF_EVENTS 미설정) 반납할 PMU 가 없다
 *
 * @iommu: 대상 IOMMU 유닛. 여기서는 쓰이지 않는다.
 *
 * alloc 이 아무것도 만들지 않았으므로 짝이 되는 해제도 할 일이 없다. 유닛
 * 해제 경로가 alloc/free 를 짝지어 부르는 형태를 #ifdef 없이 유지하기 위해
 * 함수만 남겨 둔다.
 *
 * 실행 컨텍스트: 유닛 해제(프로세스 문맥).
 *
 * 호출 체인:
 *   free_iommu() (intel/dmar.c) → [이 빈 구현]
 */
static inline void	/* [한국어] 아래도 같은 빈 구현 */
free_iommu_pmu(struct intel_iommu *iommu)	/* [한국어] 반납할 것도 없다 */
{
}

/*
 * [한국어]
 * iommu_pmu_register - (CONFIG_INTEL_IOMMU_PERF_EVENTS 미설정) perf 에 등록하지 않는다
 *
 * @iommu: 대상 IOMMU 유닛. 여기서는 쓰이지 않는다.
 *
 * 켠 빌드에서는 struct pmu 를 perf 서브시스템에 등록해, 유저스페이스가
 * perf stat -e dmar0/... 형태로 IOMMU 카운터를 읽을 수 있게 만든다. 끈
 * 빌드에서는 그 이벤트 소스가 아예 나타나지 않는다.
 *
 * 반환값이 없는 이유: 등록이 실패해도 IOMMU 동작에는 지장이 없어, 켠 빌드의
 * 실제 구현도 실패를 로그로만 남기고 삼킨다.
 *
 * 실행 컨텍스트: 부팅 중 유닛 초기화(프로세스 문맥).
 *
 * 호출 체인:
 *   intel_iommu_init() → [이 빈 구현]
 */
static inline void	/* [한국어] 아래도 같은 빈 구현 */
iommu_pmu_register(struct intel_iommu *iommu)	/* [한국어] 등록할 것도 없다 */
{
}

/*
 * [한국어]
 * iommu_pmu_unregister - (CONFIG_INTEL_IOMMU_PERF_EVENTS 미설정) 해제할 등록이 없다
 *
 * @iommu: 대상 IOMMU 유닛. 여기서는 쓰이지 않는다.
 *
 * register 가 아무것도 하지 않았으므로 해제도 할 일이 없다. 짝을 맞춰
 * 두는 이유는 위 free_iommu_pmu() 와 같다.
 *
 * 실행 컨텍스트: 유닛 해제(프로세스 문맥).
 *
 * 호출 체인:
 *   free_iommu() (intel/dmar.c) → [이 빈 구현]
 */
static inline void	/* [한국어] 아래도 같은 빈 구현 */
iommu_pmu_unregister(struct intel_iommu *iommu)	/* [한국어] 해제할 것도 없다 */
{
}
#endif /* CONFIG_INTEL_IOMMU_PERF_EVENTS */
