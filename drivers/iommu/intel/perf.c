// SPDX-License-Identifier: GPL-2.0
/*
 * perf.c - performance monitor
 *
 * Copyright (C) 2021 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *         Fenghua Yu <fenghua.yu@intel.com>
 */

/*
 * [한국어 설명] 무효화 지연을 재는 소프트웨어 계측 (intel/perf.c)
 *
 * === 파일의 역할 ===
 * perf.h 가 정의한 히스토그램에 실제로 값을 쌓고, debugfs 로 보여 줄 표를
 * 만든다. 유닛의 하드웨어 성능 카운터(perfmon.c)와는 다른 계층이다 —
 * 이쪽은 커널이 무효화 앞뒤로 시각을 읽어 그 차이를 기록한다.
 * 왜 소프트웨어로도 재는가: 하드웨어 카운터는 "몇 번 일어났는가"는 알려
 * 주지만 "그때 커널이 얼마나 기다렸는가"는 알려 주지 않는다. 무효화는
 * 하드웨어 완료를 기다리는 동기 동작이라, 그 대기 시간이 곧 매핑·언매핑
 * 경로의 지연이 된다.
 * 로그 스케일 히스토그램을 쓰는 이유는 정상값(수백 ns)과 병리적 값(수 ms)의
 * 차이가 네 자릿수여서, 평균만으로는 "가끔 아주 오래 걸리는" 문제가 묻히기
 * 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 진단 전용 곁가지이며, 번역 동작에는 관여하지 않는다.
 *   dmar.c 의 무효화 경로 → (켜져 있으면) 시각을 재고
 *   → dmar_latency_update() → 히스토그램에 누적
 *   → debugfs.c 가 dmar_latency_snapshot() 으로 표를 만들어 보여 준다
 * CONFIG_DMAR_PERF 를 끄면 이 파일 자체가 빌드되지 않고, perf.h 의 빈
 * 구현이 대신 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * - perf.h: 자료구조와 이 파일이 구현할 인터페이스.
 * - dmar.c: 무효화를 보내며 시각을 재고 update 를 부른다.
 * - debugfs.c: 켜고 끄는 인터페이스와 snapshot 출력.
 * - iommu.h: struct intel_iommu 의 perf_statistic 이 이 파일이 관리하는
 *   통계 배열을 가리킨다. 계측을 처음 켤 때 할당되고 유닛 해제 때 반납된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - dmar_latency_enabled(): 켜져 있는지. 무효화 경로가 시각을 잴지 정하는
 *   첫 관문이라 가장 자주 불린다.
 * - dmar_latency_enable(): 통계 배열이 없으면 만들고 그 종류를 켠다.
 *   최솟값 칸을 UINT_MAX 로 초기화하는 것이 요령이다.
 * - dmar_latency_disable(): 통계를 0 으로 되돌린다.
 * - dmar_latency_update(): 잰 값의 자릿수를 보고 알맞은 칸을 올리고
 *   최소/최대/합계를 갱신한다.
 * - dmar_latency_snapshot(): 고정 폭 표 문자열을 만든다. 내부적으로는
 *   나노초로 세고 출력할 때 마이크로초로 바꾼다.
 * - latency_counter_names/latency_type_names: 표의 열·행 이름.
 */
#include <linux/spinlock.h>	/* [한국어] 통계 갱신을 직렬화하는 락 */

#include "iommu.h"	/* [한국어] struct intel_iommu 의 perf_statistic 필드 */
#include "perf.h"	/* [한국어] 히스토그램 자료구조와 이 파일이 구현할 인터페이스 */

static DEFINE_SPINLOCK(latency_lock);	/* [한국어] 모든 유닛의 통계를 함께 지키는 전역 락. 유닛마다 두지 않은 것은 계측이 드물게 켜지는 진단 기능이라 경합이 문제되지 않기 때문이다 */

/*
 * [한국어]
 * dmar_latency_enabled - 이 종류의 계측이 켜져 있는지 본다
 *
 * @iommu: 대상 유닛. @type: 무효화 종류.
 * @return: true 면 켜져 있다.
 *
 * 무효화 경로가 시각을 읽기 전에 부르는 첫 관문이라, 이 파일에서 가장 자주
 * 불린다. 그래서 락을 잡지 않는다 — 켜고 끄는 것은 사람이 debugfs 로
 * 가끔 하는 일이고, 그 순간 한두 표본이 어긋나는 것은 문제가 아니다.
 *
 * perf_statistic 이 NULL 인 경우를 먼저 확인한다: 계측을 한 번도 켠 적이
 * 없는 유닛은 통계 배열 자체가 없다.
 *
 * 실행 컨텍스트: 무효화 경로. 잠들지 않는다.
 */
bool dmar_latency_enabled(struct intel_iommu *iommu, enum latency_type type)
{
	struct latency_statistic *lstat = iommu->perf_statistic;	/* [한국어] 이 유닛의 통계 배열 */

	return lstat && lstat[type].enabled;	/* [한국어] 배열이 없으면(한 번도 켠 적이 없으면) 꺼져 있는 것이다. 락 없이 읽는 것은 켜고 끄는 일이 드물고 한두 표본이 어긋나도 무방하기 때문이다 */
}

/*
 * [한국어]
 * dmar_latency_enable - 이 종류의 계측을 켠다(필요하면 통계 배열을 만든다)
 *
 * @iommu: 대상 유닛. @type: 무효화 종류.
 * @return: 0 성공(이미 켜져 있으면 0), -ENOMEM, -EBUSY.
 *
 * 통계 배열은 처음 켤 때 만든다. 유닛마다 미리 잡아 두면 계측을 쓰지 않는
 * 대부분의 시스템에서 낭비이기 때문이다. 한 번 만들면 유닛이 해제될 때까지
 * 유지되고, disable 은 내용을 0 으로 되돌리기만 한다.
 *
 * 최솟값 칸을 UINT_MAX 로 초기화하는 것이 요령이다. 0 으로 두면 첫 표본이
 * 들어와도 min(0, latency) 가 0 이라 영영 갱신되지 않는다. snapshot 이
 * UINT_MAX 를 만나면 "아직 표본이 없다"로 보고 0 을 출력한다.
 *
 * GFP_ATOMIC 인 것은 락을 쥔 채 할당하기 때문이다.
 *
 * 실행 컨텍스트: debugfs 쓰기. 프로세스 컨텍스트.
 */
int dmar_latency_enable(struct intel_iommu *iommu, enum latency_type type)
{
	struct latency_statistic *lstat;	/* [한국어] 통계 배열 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int ret = -EBUSY;	/* [한국어] 이미 켜져 있었다는 기본값 */

	if (dmar_latency_enabled(iommu, type))	/* [한국어] 이미 켜져 있으면 */
		return 0;	/* [한국어] 할 일이 없다 */

	spin_lock_irqsave(&latency_lock, flags);	/* [한국어] 통계 변경 구간 */
	if (!iommu->perf_statistic) {	/* [한국어] 아직 배열이 없으면 */
		iommu->perf_statistic = kzalloc_objs(*lstat, DMAR_LATENCY_NUM,	/* [한국어] 종류 수만큼 만든다. 계측을 쓰지 않는 대부분의 시스템에서 낭비하지 않으려고 처음 켤 때 만든다 */
						     GFP_ATOMIC);	/* [한국어] 락을 쥔 채 할당하므로 ATOMIC */
		if (!iommu->perf_statistic) {	/* [한국어] 할당 실패 */
			ret = -ENOMEM;	/* [한국어] 켤 수 없다 */
			goto unlock_out;	/* [한국어] 락을 놓고 나간다 */
		}
	}

	lstat = iommu->perf_statistic;	/* [한국어] 배열 */

	if (!lstat[type].enabled) {	/* [한국어] 아직 꺼져 있으면 */
		lstat[type].enabled = true;	/* [한국어] 켠다 */
		lstat[type].counter[COUNTS_MIN] = UINT_MAX;	/* [한국어] 최솟값을 최댓값으로 초기화한다. 0 으로 두면 min(0, latency) 가 항상 0 이라 영영 갱신되지 않는다 */
		ret = 0;	/* [한국어] 성공 */
	}
unlock_out:	/* [한국어] 할당 실패가 합류 */
	spin_unlock_irqrestore(&latency_lock, flags);	/* [한국어] 락 해제 */

	return ret;	/* [한국어] 결과 */
}

/*
 * [한국어]
 * dmar_latency_disable - 계측을 끄고 통계를 0 으로 되돌린다
 *
 * @iommu: 대상 유닛. @type: 무효화 종류.
 * @return: 없음.
 *
 * memset 이 enabled 필드까지 함께 0 으로 만들어, 끄기와 초기화가 한 번에
 * 일어난다. 그래서 다시 켤 때 깨끗한 통계로 시작한다.
 *
 * 통계 배열 자체는 해제하지 않는다 — 다시 켤 때 재할당하는 비용을 피하고,
 * 무효화 경로가 그 사이 NULL 을 만나지 않게 한다.
 *
 * 실행 컨텍스트: debugfs 쓰기. 프로세스 컨텍스트.
 */
void dmar_latency_disable(struct intel_iommu *iommu, enum latency_type type)
{
	struct latency_statistic *lstat = iommu->perf_statistic;	/* [한국어] 통계 배열 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */

	if (!dmar_latency_enabled(iommu, type))	/* [한국어] 켜져 있지 않으면 */
		return;	/* [한국어] 끌 것도 없다 */

	spin_lock_irqsave(&latency_lock, flags);	/* [한국어] 통계 변경 구간 */
	memset(&lstat[type], 0, sizeof(*lstat) * DMAR_LATENCY_NUM);	/* [한국어] enabled 필드까지 함께 0 이 되어 끄기와 초기화가 한 번에 일어난다. 배열 자체는 해제하지 않는다 — 다시 켤 때 재할당을 피하고 무효화 경로가 NULL 을 만나지 않게 한다 */
	spin_unlock_irqrestore(&latency_lock, flags);	/* [한국어] 락 해제 */
}

/*
 * [한국어]
 * dmar_latency_update - 잰 지연을 히스토그램과 요약 값에 반영한다
 *
 * @iommu: 대상 유닛. @type: 무효화 종류. @latency: 잰 시간(나노초).
 * @return: 없음.
 *
 * 계측의 실제 기록 지점이다. 무효화가 끝날 때마다 불리므로, 켜져 있는지를
 * 가장 먼저 확인해 꺼져 있으면 락도 잡지 않고 돌아간다.
 *
 * 자릿수를 if-else 사슬로 판별하는 것이 단순해 보이지만 의도적이다.
 * 대부분의 표본이 첫 칸(0.1us 미만)에 들어가므로, 그 경우 비교 한 번으로
 * 끝난다. ilog10 같은 계산보다 이쪽이 뜨거운 경로에 유리하다.
 *
 * 히스토그램을 올린 뒤 최소·최대·합계·표본 수를 갱신한다. 이 넷이 있어야
 * snapshot 이 평균과 극단값을 함께 보여 줄 수 있다 — 히스토그램만으로는
 * 구간 안의 분포를 알 수 없기 때문이다.
 *
 * 실행 컨텍스트: 무효화 경로. 락을 잡으므로 짧아야 하고, 잠들면 안 된다.
 */
void dmar_latency_update(struct intel_iommu *iommu, enum latency_type type, u64 latency)
{
	struct latency_statistic *lstat = iommu->perf_statistic;	/* [한국어] 통계 배열 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	u64 min, max;	/* [한국어] 현재 최소·최대 */

	if (!dmar_latency_enabled(iommu, type))	/* [한국어] 꺼져 있으면 */
		return;	/* [한국어] 락도 잡지 않고 돌아간다. 이 함수는 무효화마다 불린다 */

	spin_lock_irqsave(&latency_lock, flags);	/* [한국어] 통계 갱신 구간 */
	if (latency < 100)	/* [한국어] 100ns 미만이면 */
		lstat[type].counter[COUNTS_10e2]++;	/* [한국어] 첫 칸. 대부분의 표본이 여기 들어가므로 비교 한 번으로 끝난다 */
	else if (latency < 1000)	/* [한국어] 1us 미만 */
		lstat[type].counter[COUNTS_10e3]++;	/* [한국어] 둘째 칸 */
	else if (latency < 10000)	/* [한국어] 10us 미만 */
		lstat[type].counter[COUNTS_10e4]++;	/* [한국어] 셋째 칸 */
	else if (latency < 100000)	/* [한국어] 100us 미만 */
		lstat[type].counter[COUNTS_10e5]++;	/* [한국어] 넷째 칸 */
	else if (latency < 1000000)	/* [한국어] 1ms 미만 */
		lstat[type].counter[COUNTS_10e6]++;	/* [한국어] 다섯째 칸 */
	else if (latency < 10000000)	/* [한국어] 10ms 미만 */
		lstat[type].counter[COUNTS_10e7]++;	/* [한국어] 여섯째 칸 */
	else
		lstat[type].counter[COUNTS_10e8_plus]++;	/* [한국어] 10ms 이상. 여기에 표본이 쌓이면 무언가 심각하게 잘못된 것이다 */

	min = lstat[type].counter[COUNTS_MIN];	/* [한국어] 현재 최솟값 */
	max = lstat[type].counter[COUNTS_MAX];	/* [한국어] 현재 최댓값 */
	lstat[type].counter[COUNTS_MIN] = min_t(u64, min, latency);	/* [한국어] 더 작으면 갱신 */
	lstat[type].counter[COUNTS_MAX] = max_t(u64, max, latency);	/* [한국어] 더 크면 갱신 */
	lstat[type].counter[COUNTS_SUM] += latency;	/* [한국어] 평균을 내기 위한 합계 */
	lstat[type].samples++;	/* [한국어] 표본 수. 히스토그램만으로는 구간 안의 분포를 알 수 없어 이 넷을 함께 둔다 */
	spin_unlock_irqrestore(&latency_lock, flags);	/* [한국어] 락 해제 */
}

static char *latency_counter_names[] = {	/* [한국어] 표의 열 이름. 고정 폭에 맞춰 공백이 미리 채워져 있다 */
	"                  <0.1us",	/* [한국어] 첫 열은 행 이름 자리까지 겸해 더 넓다 */
	"   0.1us-1us", "    1us-10us", "  10us-100us",	/* [한국어] 히스토그램 구간들 */
	"   100us-1ms", "    1ms-10ms", "      >=10ms",	/* [한국어] 나머지 구간들 */
	"     min(us)", "     max(us)", " average(us)"	/* [한국어] 요약 값 셋. 히스토그램과 달리 개수가 아니라 시간이다 */
};

static char *latency_type_names[] = {	/* [한국어] 표의 행 이름 */
	"   inv_iotlb", "  inv_devtlb", "     inv_iec",	/* [한국어] 무효화 종류 셋 */
	"     svm_prq"	/* [한국어] SVA 페이지 요청. enum 에는 없지만 이름 배열에는 남아 있는 항목이다 */
};

/*
 * [한국어]
 * dmar_latency_snapshot - 통계를 debugfs 로 보여 줄 고정 폭 표로 만든다
 *
 * @iommu: 대상 유닛. @str: 출력 버퍼. @size: 그 크기.
 * @return: 없음(결과는 str 에 담긴다).
 *
 * 표의 모양: 첫 줄이 열 이름이고, 그 아래로 켜져 있는 무효화 종류마다
 * 한 줄씩 값이 온다. 모든 칸이 12자 고정 폭이라 이름과 값이 세로로 맞는다 —
 * 이름 배열의 문자열에 공백이 미리 채워져 있는 것이 그 때문이다.
 *
 * 단위 변환이 여기서 일어난다. 내부적으로는 나노초로 세지만 사람이 보기에는
 * 마이크로초가 편해서, 최소·최대·평균을 출력할 때 1000 으로 나눈다.
 * 히스토그램 칸은 개수이므로 나누지 않는다 — 같은 배열의 인덱스에 따라
 * 의미가 다르다는 점이 여기서 드러난다.
 *
 * 세 가지 특수 처리:
 *   MIN — UINT_MAX 면 아직 표본이 없다는 뜻이라 0 으로 보여 준다.
 *         (enable 이 그 값으로 초기화한다.)
 *   MAX — 그냥 마이크로초로 바꾼다.
 *   SUM — 표본 수로 나눠 평균을 만든다. 0 으로 나누지 않도록 확인한다.
 *
 * scnprintf 를 쓰는 이유: snprintf 와 달리 "실제로 쓴 바이트 수"를 돌려주어,
 * 버퍼가 가득 차도 bytes 가 size 를 넘지 않는다. 그래서 size - bytes 가
 * 음수가 되는 일이 없다.
 *
 * 실행 컨텍스트: debugfs 읽기. 락을 쥔 채 문자열을 만들므로 그동안 계측
 * 갱신이 막히지만, 사람이 가끔 하는 일이라 문제되지 않는다.
 */
void dmar_latency_snapshot(struct intel_iommu *iommu, char *str, size_t size)
{
	struct latency_statistic *lstat = iommu->perf_statistic;	/* [한국어] 통계 배열 */
	unsigned long flags;	/* [한국어] 인터럽트 상태 */
	int bytes = 0, i, j;	/* [한국어] 지금까지 쓴 바이트 수와 두 순회 인덱스 */

	memset(str, 0, size);	/* [한국어] 버퍼를 비운다 */

	for (i = 0; i < COUNTS_NUM; i++)	/* [한국어] 열 이름을 먼저 */
		bytes += scnprintf(str + bytes, size - bytes,	/* [한국어] scnprintf 는 실제로 쓴 바이트를 돌려주어 bytes 가 size 를 넘지 않는다 */
				  "%s", latency_counter_names[i]);	/* [한국어] 고정 폭 이름을 이어 붙인다 */

	spin_lock_irqsave(&latency_lock, flags);	/* [한국어] 통계를 읽는 동안 갱신을 막는다 */
	for (i = 0; i < DMAR_LATENCY_NUM; i++) {	/* [한국어] 무효화 종류마다 */
		if (!dmar_latency_enabled(iommu, i))	/* [한국어] 꺼져 있는 종류는 */
			continue;	/* [한국어] 건너뛴다 */

		bytes += scnprintf(str + bytes, size - bytes,	/* [한국어] 줄을 바꾸고 */
				  "\n%s", latency_type_names[i]);	/* [한국어] 행 이름을 쓴다 */

		for (j = 0; j < COUNTS_NUM; j++) {	/* [한국어] 칸마다 */
			u64 val = lstat[i].counter[j];	/* [한국어] 원본 값 */

			switch (j) {	/* [한국어] 칸의 종류에 따라 변환이 다르다 */
			case COUNTS_MIN:	/* [한국어] 최솟값 칸 */
				if (val == UINT_MAX)	/* [한국어] enable 이 넣어 둔 초기값 그대로면 */
					val = 0;	/* [한국어] 아직 표본이 없다는 뜻이라 0 으로 보여 준다 */
				else
					val = div_u64(val, 1000);	/* [한국어] 아니면 나노초를 마이크로초로 */
				break;	/* [한국어] 다음 칸 */
			case COUNTS_MAX:	/* [한국어] 최댓값 칸 */
				val = div_u64(val, 1000);	/* [한국어] 마이크로초로 */
				break;	/* [한국어] 다음 칸 */
			case COUNTS_SUM:	/* [한국어] 합계 칸은 평균으로 바꿔 보여 준다 */
				if (lstat[i].samples)	/* [한국어] 표본이 있으면 */
					val = div_u64(val, (lstat[i].samples * 1000));	/* [한국어] 표본 수로 나누고 마이크로초로 — 한 번의 나눗셈으로 둘을 함께 한다 */
				else
					val = 0;	/* [한국어] 표본이 없으면 0 으로 나누지 않는다 */
				break;	/* [한국어] 다음 칸 */
			default:	/* [한국어] 히스토그램 칸들은 */
				break;	/* [한국어] 개수이므로 변환하지 않는다 */
			}

			bytes += scnprintf(str + bytes, size - bytes,	/* [한국어] 값을 */
					  "%12lld", val);	/* [한국어] 12자 고정 폭으로 써서 열 이름과 세로로 맞춘다 */
		}
	}
	spin_unlock_irqrestore(&latency_lock, flags);	/* [한국어] 락 해제 */
}
