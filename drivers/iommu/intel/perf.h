/* SPDX-License-Identifier: GPL-2.0 */
/*
 * perf.h - performance monitor header
 *
 * Copyright (C) 2021 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 */

/*
 * [한국어 설명] 무효화 지연 계측의 자료구조 정의 (intel/perf.h)
 *
 * === 파일의 역할 ===
 * VT-d 의 무효화가 실제로 얼마나 걸리는지를 소프트웨어로 재는 계측 계층의
 * 자료구조를 정의한다. 유닛의 하드웨어 성능 카운터(iommu_pmu)와는 다른
 * 것이다 — 이쪽은 커널이 직접 시각을 재서 히스토그램에 쌓는다.
 * 왜 필요한가: 무효화는 하드웨어 완료를 기다리는 동기 동작이라, 그것이
 * 느려지면 매핑·언매핑 전체가 느려진다. 그런데 평균만 봐서는 "가끔 아주
 * 오래 걸리는" 문제를 놓친다. 그래서 구간별 히스토그램(0.1us 미만부터
 * 10ms 이상까지)으로 분포를 남기고, 최소·최대·합계도 함께 기록한다.
 * 계측은 기본으로 꺼져 있고 debugfs 로 켠다 — 켜면 무효화마다 시각을 읽는
 * 비용이 붙기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 진단 전용 곁가지다. 번역 동작에는 관여하지 않고, 무효화 경로가 지나갈 때
 * 시각을 재서 통계에 더하기만 한다.
 *   dmar.c 의 qi_submit_sync → 시작 시각 기록 → 무효화 → 끝 시각
 *   → dmar_latency_update() → [이 파일의 자료구조]
 *   → debugfs 가 dmar_latency_snapshot() 으로 사람이 읽을 문자열을 만든다
 * CONFIG_DMAR_PERF 를 끄면 모든 함수가 빈 구현으로 대체되어, 호출부에
 * #ifdef 를 흩지 않고도 코드가 통째로 사라진다.
 *
 * === 타 모듈과의 연결 ===
 * - perf.c: 이 헤더가 선언한 함수들의 구현.
 * - dmar.c: 무효화를 보내면서 시각을 재고 dmar_latency_update 를 부른다.
 * - debugfs.c: 켜고 끄는 인터페이스와 통계 출력.
 * - iommu.h: struct intel_iommu 의 perf_statistic 필드가 이 통계 배열을
 *   가리킨다.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum latency_type: 무엇을 재는지 — IOTLB 무효화, 디바이스 TLB 무효화,
 *   인터럽트 항목 캐시 무효화. 셋의 성격이 달라 따로 센다.
 * - enum latency_count: 히스토그램의 칸. 10배씩 늘어나는 구간 일곱 개와,
 *   최소/최대/합계를 담는 세 칸을 한 배열에 겹쳐 둔다.
 * - struct latency_statistic: 종류 하나의 통계. 켜짐 여부, 칸별 값, 표본 수.
 * - dmar_latency_enable()/disable()/enabled(): 계측을 켜고 끈다.
 * - dmar_latency_update(): 잰 값을 알맞은 칸에 더한다.
 * - dmar_latency_snapshot(): debugfs 로 보여 줄 문자열을 만든다.
 */
/*
 * [한국어] enum latency_type — 무엇의 지연을 재는지
 *
 * 무효화 셋을 따로 세는 이유는 성격이 아주 다르기 때문이다.
 *   IOTLB  — 유닛 안에서 끝난다. 가장 빠르고 가장 자주 일어난다.
 *   DevTLB — 장치의 응답을 기다린다. 장치에 따라 수십 배 느릴 수 있고,
 *            응답하지 않는 장치가 있으면 시간 초과까지 간다.
 *   IEC    — 인터럽트 항목 캐시. 인터럽트를 재설정할 때만 일어난다.
 * 이 셋을 합쳐 평균을 내면 DevTLB 의 이상 징후가 IOTLB 표본에 묻힌다.
 */
enum latency_type {
	DMAR_LATENCY_INV_IOTLB = 0,	/* [한국어] IOTLB 무효화의 지연. 매핑을 풀 때마다 일어나므로 가장 자주 측정된다 */
	DMAR_LATENCY_INV_DEVTLB,	/* [한국어] 디바이스 TLB 무효화. 장치의 응답을 기다리므로 훨씬 오래 걸릴 수 있다 */
	DMAR_LATENCY_INV_IEC,	/* [한국어] 인터럽트 항목 캐시 무효화. 인터럽트를 재설정할 때만 일어난다 */
	DMAR_LATENCY_NUM	/* [한국어] 종류의 개수. 통계 배열의 크기가 된다 */
};

/*
 * [한국어] enum latency_count — 히스토그램의 칸과, 그 뒤에 겹쳐 둔 요약 값들
 *
 * 앞의 일곱은 10배씩 늘어나는 지연 구간이다. 로그 스케일을 쓰는 이유는
 * 정상 무효화(수백 ns)와 병리적인 경우(수 ms) 사이의 차이가 네 자릿수라,
 * 선형 구간으로는 둘 다 담을 수 없기 때문이다.
 *
 * 뒤의 셋(MIN/MAX/SUM)은 히스토그램 칸이 아니라 실제 지연 값이다. 같은
 * 배열에 겹쳐 두어 인덱스에 따라 의미가 달라지므로, 이 배열을 다루는
 * 코드는 그 경계를 알고 있어야 한다.
 */
enum latency_count {
	COUNTS_10e2 = 0,	/* < 0.1us	*/	/* [한국어] 0.1us 미만 (위 영어 주석). 이름의 10e2 는 100ns 를 뜻한다 */
	COUNTS_10e3,		/* 0.1us ~ 1us	*/	/* [한국어] 0.1us ~ 1us */
	COUNTS_10e4,		/* 1us ~ 10us	*/	/* [한국어] 1us ~ 10us */
	COUNTS_10e5,		/* 10us ~ 100us	*/	/* [한국어] 10us ~ 100us */
	COUNTS_10e6,		/* 100us ~ 1ms	*/	/* [한국어] 100us ~ 1ms */
	COUNTS_10e7,		/* 1ms ~ 10ms	*/	/* [한국어] 1ms ~ 10ms */
	COUNTS_10e8_plus,	/* 10ms and plus*/	/* [한국어] 10ms 이상. 여기에 표본이 쌓이면 무언가 심각하게 잘못된 것이다 */
	COUNTS_MIN,	/* [한국어] 같은 배열에 겹쳐 둔 최솟값 칸. 히스토그램 칸이 아니라 실제 값이다 */
	COUNTS_MAX,	/* [한국어] 최댓값 칸 */
	COUNTS_SUM,	/* [한국어] 합계 칸. 표본 수로 나누면 평균이 된다 */
	COUNTS_NUM	/* [한국어] 칸의 총 개수. 히스토그램 일곱 + 최소/최대/합계 셋 */
};

/*
 * [한국어] struct latency_statistic — 무효화 한 종류의 지연 통계
 *
 * 유닛마다 이 구조체가 latency_type 개수만큼 배열로 있고, 그 배열을
 * struct intel_iommu 의 perf_statistic 이 가리킨다.
 *
 * 락이 없는 것이 의도적이다. 여러 CPU 가 동시에 무효화를 보내며 같은 통계를
 * 갱신할 수 있지만, 이 값들은 정확한 회계가 아니라 경향을 보기 위한 것이다.
 * 락을 걸면 계측 자체가 무효화 경로의 병목이 되어, 재려던 것을 왜곡한다.
 */
struct latency_statistic {
	bool enabled;
	/* [한국어] 이 종류의 계측이 켜져 있는지.
	 * 설정자: dmar_latency_enable()/disable() — debugfs 를 통해 관리자가 켠다.
	 * 읽는 자: dmar_latency_enabled() 를 통해 무효화 경로가 확인한다. 꺼져 있으면
	 *   시각을 읽는 비용조차 들이지 않는다.
	 * 기본값이 꺼짐인 이유: 무효화마다 두 번의 시각 읽기가 붙으면 뜨거운 경로에
	 *   부담이 된다. 문제를 조사할 때만 켠다. */
	u64 counter[COUNTS_NUM];
	/* [한국어] 히스토그램 칸과 최소/최대/합계를 함께 담는 배열.
	 * 앞의 일곱 칸(COUNTS_10e2 ~ COUNTS_10e8_plus)은 그 구간에 든 표본의 개수이고,
	 * 뒤의 세 칸(MIN/MAX/SUM)은 개수가 아니라 실제 지연 값이다. 성격이 다른 두
	 *   종류를 한 배열에 겹쳐 둔 것이라, 인덱스에 따라 의미가 달라진다.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 올리고,
	 *   MIN/MAX/SUM 을 함께 갱신한다.
	 * 읽는 자: dmar_latency_snapshot() 이 문자열로 만든다.
	 * 동기화: 별도 락이 없다. 계측은 정확한 값보다 경향을 보는 것이 목적이라,
	 *   드물게 갱신이 어긋나도 문제가 되지 않는다. */
	u64 samples;
	/* [한국어] 지금까지 잰 표본의 총 개수.
	 * 설정자: dmar_latency_update() 가 매번 하나씩 늘린다.
	 * 읽는 자: snapshot 이 평균을 계산할 때(SUM / samples)와, 히스토그램의
	 *   비율을 보여 줄 때.
	 * 값 범위: 0 이면 아직 잰 적이 없다는 뜻이라 평균 계산에서 0 으로 나누지
	 *   않도록 확인해야 한다. */
};

#ifdef CONFIG_DMAR_PERF	/* [한국어] 계측을 켠 빌드에서만 실제 구현을 쓴다 */
int dmar_latency_enable(struct intel_iommu *iommu, enum latency_type type);	/* [한국어] 그 종류의 계측을 켠다 */
void dmar_latency_disable(struct intel_iommu *iommu, enum latency_type type);	/* [한국어] 끈다 */
bool dmar_latency_enabled(struct intel_iommu *iommu, enum latency_type type);	/* [한국어] 켜져 있는지. 무효화 경로가 시각을 잴지 정하는 데 쓴다 */
void dmar_latency_update(struct intel_iommu *iommu, enum latency_type type,	/* [한국어] 잰 값을 통계에 더한다 */
			 u64 latency);
void dmar_latency_snapshot(struct intel_iommu *iommu, char *str, size_t size);	/* [한국어] 통계를 사람이 읽을 문자열로 만든다 */
#else
static inline int
dmar_latency_enable(struct intel_iommu *iommu, enum latency_type type)	/* [한국어] 계측을 끈 빌드의 빈 구현들. 호출부에 #ifdef 를 흩지 않는 관용구다 */
{
	return -EINVAL;	/* [한국어] 켤 수 없다 */
}

static inline void	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_disable(struct intel_iommu *iommu, enum latency_type type)	/* [한국어] 끄기도 할 일이 없다 */
{
}

static inline bool	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_enabled(struct intel_iommu *iommu, enum latency_type type)	/* [한국어] 항상 꺼져 있다고 답해 */
{
	return false;	/* [한국어] 무효화 경로가 시각을 재지 않게 한다 */
}

static inline void	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_update(struct intel_iommu *iommu, enum latency_type type, u64 latency)	/* [한국어] 기록할 곳이 없다 */
{
}

static inline void	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_snapshot(struct intel_iommu *iommu, char *str, size_t size)	/* [한국어] 보여 줄 것도 없다 */
{
}
#endif /* CONFIG_DMAR_PERF */
