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
	DMAR_LATENCY_INV_IOTLB = 0,
	/* [한국어] IOTLB 무효화 명령의 지연을 재는 계측 종류.
	 * 설정자: 이 값은 계측 배열의 첨자다. debugfs 로 계측을 켤 때 종류를 지정한다.
	 * 읽는 자: qi_submit_sync() 가 무효화를 보낼 때 이 종류로 시각을 기록한다.
	 * 왜 가장 먼저인가: 매핑을 풀 때마다 일어나므로 가장 자주 측정되고, 성능
	 *   문제를 조사할 때 가장 먼저 보게 되는 값이다.
	 * 값이 0 으로 명시된 이유: 이 enum 이 배열 첨자로 쓰이므로 시작이 0 임을
	 *   분명히 해 둔 것이다. */
	DMAR_LATENCY_INV_DEVTLB,
	/* [한국어] 장치 TLB(ATC) 무효화 명령의 지연을 재는 계측 종류.
	 * 설정자/읽는 자: 위 INV_IOTLB 와 같은 방식.
	 * 왜 따로 재는가: 이 무효화는 PCIe 를 왕복해 장치의 응답을 기다리므로,
	 *   IOTLB 무효화보다 자릿수가 다르게 느릴 수 있다. 둘을 한 통계에 섞으면
	 *   분포가 뭉개져 어느 쪽이 문제인지 알 수 없다.
	 * 느린 장치 하나가 시스템 전체의 unmap 지연을 끌어올리는 상황을 이 계측으로
	 *   잡아낸다. */
	DMAR_LATENCY_INV_IEC,
	/* [한국어] 인터럽트 항목 캐시(IEC) 무효화의 지연을 재는 계측 종류.
	 * 설정자/읽는 자: 위와 같은 방식.
	 * 언제 일어나는가: 인터럽트 재매핑 항목을 고쳤을 때 — 인터럽트 친화도를
	 *   바꾸거나 장치를 붙이고 뗄 때다. 위 둘과 달리 DMA 경로가 아니라
	 *   인터럽트 설정 경로에서만 일어나므로 훨씬 드물다.
	 * 드물지만 재 두는 이유: 인터럽트 친화도 변경이 느리면 스케줄러의 부하
	 *   분산이 지연되어, 원인을 찾기 어려운 성능 문제로 나타난다. */
	DMAR_LATENCY_NUM
	/* [한국어] 계측 종류의 총 개수 — 통계 배열의 크기가 된다.
	 * 설정자: enum 의 마지막 값이라 컴파일러가 자동으로 정한다.
	 * 읽는 자: struct intel_iommu 의 perf_statistic 배열 선언과, 종류를 훑는 루프의 상한.
	 * 왜 이 관용구를 쓰는가: 계측 종류를 하나 추가하면 위에 이름을 넣기만 하면
	 *   되고 배열 크기가 자동으로 따라간다. 숫자를 따로 적으면 둘이 어긋나
	 *   배열 밖을 넘어선다.
	 * 값 범위: 현재 3. */
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
	COUNTS_10e2 = 0,	/* < 0.1us	*/
	/* [한국어] 지연이 0.1us(100ns) 미만 구간에 든 표본의 개수.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 하나 올린다.
	 * 읽는 자: dmar_latency_snapshot() 이 히스토그램을 문자열로 만들 때.
	 * 이름의 10e2 는 100ns 를 뜻한다 — 지수 표기를 이름에 그대로 옮긴 것이다.
	 * 정상적인 IOTLB 무효화가 대개 이 칸에 든다. 여기에 표본이 몰려 있으면
	 *   무효화 경로가 건강하다는 뜻이다.
	 * 값 범위: 표본 수. 오버플로를 확인하지 않는데, 계측을 켠 채 그만큼 오래
	 *   돌릴 일이 없고 정확한 회계가 목적도 아니기 때문이다.
	 * 동기화: 없다. 여러 CPU 가 동시에 올리면 몇 개를 놓칠 수 있지만, 경향을
	 *   보는 것이 목적이라 락을 걸어 계측 자체를 병목으로 만들지 않는다. */
	COUNTS_10e3,		/* 0.1us ~ 1us	*/
	/* [한국어] 지연이 0.1us 이상 1us 미만 구간에 든 표본의 개수.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 하나 올린다.
	 * 읽는 자: dmar_latency_snapshot() 이 히스토그램을 문자열로 만들 때.
	 * 무효화가 몰려 큐가 붐빌 때 이 칸으로 넘어온다. 정상 범위의 위쪽이다.
	 * 값 범위: 표본 수. 오버플로를 확인하지 않는데, 계측을 켠 채 그만큼 오래
	 *   돌릴 일이 없고 정확한 회계가 목적도 아니기 때문이다.
	 * 동기화: 없다. 여러 CPU 가 동시에 올리면 몇 개를 놓칠 수 있지만, 경향을
	 *   보는 것이 목적이라 락을 걸어 계측 자체를 병목으로 만들지 않는다. */
	COUNTS_10e4,		/* 1us ~ 10us	*/
	/* [한국어] 지연이 1us 이상 10us 미만 구간에 든 표본의 개수.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 하나 올린다.
	 * 읽는 자: dmar_latency_snapshot() 이 히스토그램을 문자열로 만들 때.
	 * 여기부터는 무언가 기다리고 있다는 신호다. 장치 TLB 무효화라면 정상일 수
	 *   있지만, IOTLB 무효화가 이 칸에 있으면 큐 경쟁을 의심해야 한다.
	 * 값 범위: 표본 수. 오버플로를 확인하지 않는데, 계측을 켠 채 그만큼 오래
	 *   돌릴 일이 없고 정확한 회계가 목적도 아니기 때문이다.
	 * 동기화: 없다. 여러 CPU 가 동시에 올리면 몇 개를 놓칠 수 있지만, 경향을
	 *   보는 것이 목적이라 락을 걸어 계측 자체를 병목으로 만들지 않는다. */
	COUNTS_10e5,		/* 10us ~ 100us	*/
	/* [한국어] 지연이 10us 이상 100us 미만 구간에 든 표본의 개수.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 하나 올린다.
	 * 읽는 자: dmar_latency_snapshot() 이 히스토그램을 문자열로 만들 때.
	 * DMA 매핑 해제가 이 정도 걸리면 처리량에 뚜렷이 드러난다. 장치 TLB
	 *   무효화에서 느린 장치가 섞여 있을 때 흔히 보인다.
	 * 값 범위: 표본 수. 오버플로를 확인하지 않는데, 계측을 켠 채 그만큼 오래
	 *   돌릴 일이 없고 정확한 회계가 목적도 아니기 때문이다.
	 * 동기화: 없다. 여러 CPU 가 동시에 올리면 몇 개를 놓칠 수 있지만, 경향을
	 *   보는 것이 목적이라 락을 걸어 계측 자체를 병목으로 만들지 않는다. */
	COUNTS_10e6,		/* 100us ~ 1ms	*/
	/* [한국어] 지연이 100us 이상 1ms 미만 구간에 든 표본의 개수.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 하나 올린다.
	 * 읽는 자: dmar_latency_snapshot() 이 히스토그램을 문자열로 만들 때.
	 * 정상 동작으로 설명하기 어려운 구간이다. 장치가 응답을 늦게 주거나
	 *   큐가 가득 차 대기가 길어진 경우다.
	 * 값 범위: 표본 수. 오버플로를 확인하지 않는데, 계측을 켠 채 그만큼 오래
	 *   돌릴 일이 없고 정확한 회계가 목적도 아니기 때문이다.
	 * 동기화: 없다. 여러 CPU 가 동시에 올리면 몇 개를 놓칠 수 있지만, 경향을
	 *   보는 것이 목적이라 락을 걸어 계측 자체를 병목으로 만들지 않는다. */
	COUNTS_10e7,		/* 1ms ~ 10ms	*/
	/* [한국어] 지연이 1ms 이상 10ms 미만 구간에 든 표본의 개수.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 하나 올린다.
	 * 읽는 자: dmar_latency_snapshot() 이 히스토그램을 문자열로 만들 때.
	 * 이 구간의 표본은 거의 언제나 문제의 징후다. 무효화 하나에 밀리초가
	 *   걸리면 그 경로를 쓰는 모든 DMA 가 함께 느려진다.
	 * 값 범위: 표본 수. 오버플로를 확인하지 않는데, 계측을 켠 채 그만큼 오래
	 *   돌릴 일이 없고 정확한 회계가 목적도 아니기 때문이다.
	 * 동기화: 없다. 여러 CPU 가 동시에 올리면 몇 개를 놓칠 수 있지만, 경향을
	 *   보는 것이 목적이라 락을 걸어 계측 자체를 병목으로 만들지 않는다. */
	COUNTS_10e8_plus,	/* 10ms and plus*/
	/* [한국어] 지연이 10ms 이상 구간에 든 표본의 개수.
	 * 설정자: dmar_latency_update() 가 잰 값의 자릿수를 보고 알맞은 칸을 하나 올린다.
	 * 읽는 자: dmar_latency_snapshot() 이 히스토그램을 문자열로 만들 때.
	 * 마지막 칸이라 상한이 없다. 여기에 표본이 쌓이면 무언가 심각하게 잘못된
	 *   것이다 — 장치가 응답하지 않거나 하드웨어가 멈추기 직전일 수 있다.
	 * 로그 스케일을 쓰는 이유가 이 칸에 있다: 정상(수백 ns)과 병리적인 경우
	 *   (수 ms) 사이가 네 자릿수라, 선형 구간으로는 둘을 함께 담을 수 없다.
	 * 값 범위: 표본 수. 오버플로를 확인하지 않는데, 계측을 켠 채 그만큼 오래
	 *   돌릴 일이 없고 정확한 회계가 목적도 아니기 때문이다.
	 * 동기화: 없다. 여러 CPU 가 동시에 올리면 몇 개를 놓칠 수 있지만, 경향을
	 *   보는 것이 목적이라 락을 걸어 계측 자체를 병목으로 만들지 않는다. */
	COUNTS_MIN,
	/* [한국어] 지금까지 잰 값 중 최솟값 — 히스토그램 칸이 아니라 실제 지연 값이다.
	 * 설정자: dmar_latency_update() 가 잰 값이 더 작으면 갱신한다.
	 * 읽는 자: snapshot 이 요약을 출력할 때.
	 * 같은 배열에 겹쳐 둔 것이 이 enum 의 특징이다. 앞의 일곱 칸은 개수를 세고,
	 *   이 뒤 세 칸은 값 자체를 담는다 — 인덱스에 따라 의미가 달라지므로,
	 *   이 배열을 다루는 코드는 그 경계를 알고 있어야 한다.
	 * 왜 따로 배열을 두지 않는가: 통계 하나가 캐시 줄 안에 들어가는 편이 갱신
	 *   경로에 유리하고, 두 배열을 짝지어 관리하는 번거로움도 없다. */
	COUNTS_MAX,
	/* [한국어] 지금까지 잰 값 중 최댓값 — 역시 실제 지연 값이다.
	 * 설정자: dmar_latency_update() 가 잰 값이 더 크면 갱신한다.
	 * 읽는 자: snapshot.
	 * 왜 히스토그램만으로 부족한가: 마지막 칸은 "10ms 이상" 이라 상한이 없어,
	 *   실제로 얼마나 나빴는지 알 수 없다. 최댓값을 따로 두면 최악의 경우를
	 *   숫자로 볼 수 있다.
	 * 동기화: 없다. 두 CPU 가 동시에 갱신하면 더 큰 값이 묻힐 수 있지만,
	 *   경향을 보는 것이 목적이라 감수한다. */
	COUNTS_SUM,
	/* [한국어] 지금까지 잰 값의 합계 — 표본 수로 나누면 평균이 된다.
	 * 설정자: dmar_latency_update() 가 매번 더한다.
	 * 읽는 자: snapshot 이 SUM / samples 로 평균을 계산할 때.
	 * 왜 평균을 미리 계산해 두지 않는가: 이동 평균을 유지하려면 나눗셈이 갱신
	 *   경로에 들어간다. 합계만 더해 두고 읽을 때 한 번 나누는 편이 훨씬 싸다.
	 * 오버플로: u64 라 현실적인 시간 안에는 넘치지 않는다.
	 * 평균과 히스토그램을 함께 보는 이유: 평균은 분포의 꼬리를 감춘다. 평균은
	 *   낮은데 마지막 칸에 표본이 있으면 드물게 아주 나쁜 경우가 있다는 뜻이다. */
	COUNTS_NUM
	/* [한국어] 칸의 총 개수 — 히스토그램 일곱 + 최소/최대/합계 셋.
	 * 설정자: enum 의 마지막 값이라 자동으로 정해진다.
	 * 읽는 자: latency_statistic 의 counter 배열 선언과, 그 배열을 훑는 루프의 상한.
	 * 위 DMAR_LATENCY_NUM 과 같은 관용구다 — 칸을 추가하면 배열 크기가 따라간다.
	 * 값 범위: 현재 10. */
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
/*
 * [한국어]
 * dmar_latency_enable - (CONFIG_DMAR_PERF 미설정) 지연 계측을 켤 수 없다고 답한다
 *
 * @iommu:  계측을 켜려던 IOMMU 유닛. 여기서는 쓰이지 않는다.
 * @type:   계측 종류(무효화 명령별 지연 등). 여기서는 쓰이지 않는다.
 * @return: 항상 -EINVAL.
 *
 * 켠 빌드에서는 이 호출이 iommu->perf_statistic 배열을 할당해, 이후 무효화
 * 경로가 ktime 을 재어 히스토그램에 쌓기 시작한다. 끈 빌드에는 그 배열도
 * 갱신 코드도 없으므로 켤 수 없다.
 *
 * 에러 경로: -EINVAL 은 debugfs 의 dmar_perf_latency write 핸들러까지 올라가,
 * 사용자가 계측을 켜려는 시도를 실패로 끝낸다.
 *
 * 실행 컨텍스트: debugfs write(프로세스 문맥).
 *
 * 호출 체인:
 *   dmar_perf_latency_write() (intel/debugfs.c) → [이 빈 구현]
 */
static inline int
dmar_latency_enable(struct intel_iommu *iommu, enum latency_type type)	/* [한국어] 계측을 끈 빌드의 빈 구현들. 호출부에 #ifdef 를 흩지 않는 관용구다 */
{
	return -EINVAL;	/* [한국어] 켤 수 없다 */
}

/*
 * [한국어]
 * dmar_latency_disable - (CONFIG_DMAR_PERF 미설정) 끌 계측이 없다
 *
 * @iommu: 대상 IOMMU 유닛. 여기서는 쓰이지 않는다.
 * @type:  계측 종류. 여기서는 쓰이지 않는다.
 *
 * enable 이 항상 실패하므로 켜져 있는 계측도 없다. 그래도 함수가 존재해야
 * debugfs 핸들러가 enable/disable 을 짝지어 부르는 형태를 #ifdef 없이 유지한다.
 *
 * 실행 컨텍스트: debugfs write(프로세스 문맥).
 *
 * 호출 체인:
 *   dmar_perf_latency_write() (intel/debugfs.c) → [이 빈 구현]
 */
static inline void	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_disable(struct intel_iommu *iommu, enum latency_type type)	/* [한국어] 끄기도 할 일이 없다 */
{
}

/*
 * [한국어]
 * dmar_latency_enabled - (CONFIG_DMAR_PERF 미설정) 항상 꺼져 있다고 답한다
 *
 * @iommu:  대상 IOMMU 유닛. 여기서는 쓰이지 않는다.
 * @type:   계측 종류. 여기서는 쓰이지 않는다.
 * @return: 항상 false.
 *
 * 이 다섯 개 중 성능에 가장 중요한 함수다. 무효화 경로(qi_submit_sync 등)는
 * 시각을 재기 전에 이 함수로 물어보는데, false 를 상수로 돌려주면 컴파일러가
 * 그 뒤의 ktime_get() 호출과 계산을 통째로 걷어낸다. 계측을 뺀 빌드에서
 * 무효화 핫패스에 아무 비용도 남지 않는 이유가 이것이다.
 *
 * 실행 컨텍스트: 무효화 핫패스. 인터럽트 비활성 구간에서도 불린다.
 *
 * 호출 체인:
 *   qi_submit_sync() (intel/dmar.c) → [이 빈 구현]
 */
static inline bool	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_enabled(struct intel_iommu *iommu, enum latency_type type)	/* [한국어] 항상 꺼져 있다고 답해 */
{
	return false;	/* [한국어] 무효화 경로가 시각을 재지 않게 한다 */
}

/*
 * [한국어]
 * dmar_latency_update - (CONFIG_DMAR_PERF 미설정) 기록할 통계가 없다
 *
 * @iommu:   대상 IOMMU 유닛. 여기서는 쓰이지 않는다.
 * @type:    계측 종류. 여기서는 쓰이지 않는다.
 * @latency: 잰 지연 값(ns). 여기서는 버려진다.
 *
 * 켠 빌드에서는 이 호출이 latency_statistic 의 min/max/총합/표본 수를 갱신하고
 * 히스토그램 구간 하나를 늘린다. 끈 빌드에는 그 구조체가 없다. 바로 위
 * dmar_latency_enabled() 가 false 를 주므로 실제로는 호출 자체가 사라진다.
 *
 * 실행 컨텍스트: 무효화 완료 직후. 인터럽트 비활성 구간일 수 있다.
 *
 * 호출 체인:
 *   qi_submit_sync() (intel/dmar.c) → [이 빈 구현]
 */
static inline void	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_update(struct intel_iommu *iommu, enum latency_type type, u64 latency)	/* [한국어] 기록할 곳이 없다 */
{
}

/*
 * [한국어]
 * dmar_latency_snapshot - (CONFIG_DMAR_PERF 미설정) 보여 줄 통계가 없다
 *
 * @iommu: 대상 IOMMU 유닛. 여기서는 쓰이지 않는다.
 * @str:   결과를 담을 버퍼. 여기서는 건드리지 않는다 — 호출자가 미리
 *         비워 둔 내용이 그대로 남아 빈 출력이 된다.
 * @size:  그 버퍼의 크기. 여기서는 쓰이지 않는다.
 *
 * 켠 빌드에서는 평균/최소/최대와 구간별 분포를 사람이 읽을 문자열로 만들어
 * debugfs 의 dmar_perf_latency 읽기에 실어 준다.
 *
 * 실행 컨텍스트: debugfs read(프로세스 문맥).
 *
 * 호출 체인:
 *   latency_show() (intel/debugfs.c) → [이 빈 구현]
 */
static inline void	/* [한국어] 아래도 같은 빈 구현 */
dmar_latency_snapshot(struct intel_iommu *iommu, char *str, size_t size)	/* [한국어] 보여 줄 것도 없다 */
{
}
#endif /* CONFIG_DMAR_PERF */
