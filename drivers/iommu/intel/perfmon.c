// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support Intel IOMMU PerfMon
 * Copyright(c) 2023 Intel Corporation.
 */
/*
 * [한국어 설명] Intel IOMMU 하드웨어 성능 카운터를 perf 에 연결하는 드라이버 (perfmon.c)
 *
 * === 파일의 역할 ===
 * VT-d 유닛 안에 들어 있는 하드웨어 성능 카운터(PerfMon)를 리눅스의 perf
 * 서브시스템에 PMU 로 등록한다. 등록되고 나면 사용자가 `perf stat -e
 * dmar0/iotlb_hit/` 같은 명령으로 IOMMU 내부의 캐시 적중률, 페이지 워크
 * 횟수, ATS 차단 횟수 같은 것을 CPU 이벤트와 똑같은 방법으로 측정할 수 있다.
 *
 * 이런 카운터가 필요한 이유는 IOMMU 의 비용이 보이지 않는 데 있다. DMA 가
 * 느릴 때 그것이 IOTLB 미스 때문인지, 컨텍스트 캐시 미스 때문인지, 페이지
 * 테이블을 몇 단계나 걸어 내려갔기 때문인지는 소프트웨어에서 알 방법이
 * 없다. 하드웨어만 아는 그 숫자를 밖으로 꺼내는 것이 이 파일의 전부다.
 *
 * 필터가 이 PMU 의 특징이다. 요청자 id, 도메인, PASID, ATS, 페이지 테이블
 * 종류로 카운팅 대상을 좁힐 수 있어, "이 장치의 이 PASID 만" 같은 측정이
 * 가능하다. 그래서 이벤트 설정이 config 하나가 아니라 config/config1/config2
 * 세 개로 나뉘어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * VT-d 드라이버와 perf 코어 사이의 다리다. 아래로는 iommu.h 의
 * struct intel_iommu 를 통해 유닛의 PerfMon MMIO 영역에 닿고, 위로는
 * struct pmu 를 perf 코어에 등록해 표준 콜백(event_init/add/del/start/stop)을
 * 받는다.
 *
 * 실행 컨텍스트가 둘로 나뉜다. 초기화(alloc_iommu_pmu/iommu_pmu_register)는
 * 부팅이나 유닛 핫플러그의 프로세스 문맥이고, 카운터를 켜고 끄고 읽는
 * 콜백들은 perf 코어가 인터럽트를 끈 상태에서 부른다. 오버플로 처리는
 * IOMMU 의 폴트 인터럽트 핸들러 안에서 실행된다.
 *
 * 호출 체인:
 *   perf stat → perf 코어 → iommu_pmu_event_init() → iommu_pmu_add()
 *     → iommu_pmu_start() → (측정) → iommu_pmu_stop() → iommu_pmu_del()
 *   dmar_fault() → iommu_pmu_irq_handler() → iommu_pmu_counter_overflow()
 *   intel_iommu_init 경로 → alloc_iommu_pmu() → iommu_pmu_register()
 *
 * === 타 모듈과의 연결 ===
 * perfmon.h 가 레지스터 오프셋과 struct iommu_pmu 를 정의하고, iommu.h 가
 * 유닛과 그 MMIO 기준 주소를 준다. dmar.c 의 폴트 인터럽트 핸들러가
 * 오버플로 알림을 이 파일로 넘긴다. 위쪽으로는 perf 코어(include/linux/
 * perf_event.h)와 sysfs 속성 그룹이 인터페이스다.
 *
 * 데이터 흐름: 하드웨어 카운터 → 이 파일이 주기적으로 읽어 누적 →
 * perf 코어의 event->count → 사용자. 반대 방향으로는 사용자의 이벤트
 * 설정이 config 워드로 내려와 카운터 설정 레지스터에 써진다.
 *
 * 공유 상태: iommu_pmu->used_mask 가 어느 카운터가 쓰이는지를 나타내는
 * 비트맵이고, perf 코어가 CPU 별 직렬화를 보장하므로 별도 락이 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - iommu_pmu_event_init(): 사용자가 요청한 이벤트가 이 하드웨어에서
 *   가능한지 검증하고 설정 워드를 만든다.
 * - iommu_pmu_add()/del(): 이벤트에 물리 카운터를 배정하고 회수한다.
 * - iommu_pmu_start()/stop(): 카운터를 실제로 돌리고 멈추며 값을 누적한다.
 * - iommu_pmu_event_update(): 카운터의 증가분을 event->count 에 더한다.
 * - iommu_pmu_counter_overflow(): 오버플로한 카운터를 찾아 값을 보존한다.
 * - alloc_iommu_pmu()/iommu_pmu_register(): 하드웨어 능력을 읽어 PMU 를 구성·등록한다.
 * - IOMMU_PMU_ATTR/IOMMU_PMU_EVENT_ATTR: sysfs 속성을 하드웨어 지원 여부에
 *   따라 보이거나 숨기는 매크로.
 */
#define pr_fmt(fmt)	"DMAR: " fmt	/* [한국어] 이 파일의 로그 접두사. DMA 쪽과 같은 "DMAR:" 를 쓴다 */
#define dev_fmt(fmt)	pr_fmt(fmt)	/* [한국어] dev_err/dev_warn 도 같은 접두사를 쓰게 한다 */

#include <linux/dmar.h>	/* [한국어] DRHD 유닛 목록과 폴트 인터럽트 연결 */
#include "iommu.h"	/* [한국어] struct intel_iommu 와 MMIO 기준 주소 */
#include "perfmon.h"	/* [한국어] PerfMon 레지스터 오프셋과 struct iommu_pmu */

PMU_FORMAT_ATTR(event,		"config:0-27");		/* ES: Events Select */	/* [한국어] 이벤트 선택(ES) 필드가 config 의 0~27비트임을 사용자에게 알리는 sysfs 속성. perf 도구가 이것을 읽어 event=0x1 같은 표기를 비트 위치로 번역한다 */
PMU_FORMAT_ATTR(event_group,	"config:28-31");	/* EGI: Event Group Index */	/* [한국어] 이벤트 그룹 인덱스(EGI)는 28~31비트. 이벤트가 그룹으로 묶여 있어 두 필드가 함께 하나의 이벤트를 지정한다 */

static struct attribute *iommu_pmu_format_attrs[] = {	/* [한국어] format 디렉터리에 항상 보이는 두 속성 */
	&format_attr_event_group.attr,
	/* [한국어] 이벤트 그룹 인덱스 속성.
	 * 왜 그룹이 먼저인가: sysfs 출력 순서일 뿐 의미상의 우선순위는 없다. */
	&format_attr_event.attr,
	/* [한국어] 이벤트 선택 속성. */
	NULL
	/* [한국어] 목록의 끝 표시. sysfs 속성 배열의 관례다. */
};

static struct attribute_group iommu_pmu_format_attr_group = {	/* [한국어] 그 둘을 "format" 이름으로 묶는다 */
	.name = "format",
	/* [한국어] sysfs 에서 이 그룹이 만들 하위 디렉터리 이름.
	 * perf 도구가 /sys/bus/event_source/devices/<pmu>/format/ 을 읽어
	 *   이벤트 이름을 비트 위치로 번역한다. */
	.attrs = iommu_pmu_format_attrs,
	/* [한국어] 그 디렉터리에 놓일 속성들. */
};

/* The available events are added in attr_update later */
static struct attribute *attrs_empty[] = {	/* [한국어] 비어 있는 속성 목록. 이벤트 속성은 하드웨어 능력을 본 뒤 attr_update 로 채워진다 */
	NULL	/* [한국어] 목록의 끝 표시 */
};

static struct attribute_group iommu_pmu_events_attr_group = {	/* [한국어] "events" 디렉터리. 처음에는 비어 있다 */
	.name = "events",
	/* [한국어] sysfs 의 events 디렉터리 이름. 여기에 이벤트 이름들이 놓인다. */
	.attrs = attrs_empty,
	/* [한국어] 처음에는 비어 있다. 실제 이벤트는 하드웨어의 evcap 을 확인한 뒤
	 *   attr_update 경로로 하나씩 보이게 되므로, 여기서 미리 채울 수 없다. */
};

static const struct attribute_group *iommu_pmu_attr_groups[] = {	/* [한국어] PMU 에 등록할 기본 속성 그룹 목록 */
	&iommu_pmu_format_attr_group,
	/* [한국어] format 디렉터리 — 이벤트 인코딩 방법을 알린다. */
	&iommu_pmu_events_attr_group,
	/* [한국어] events 디렉터리 — 쓸 수 있는 이벤트 이름들. */
	NULL
};

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * dev_to_iommu_pmu - sysfs 의 device 로부터 이 PMU 구조체를 되찾는다
 *
 * @dev: perf 코어가 PMU 마다 만들어 준 device.
 * @return: 그 device 에 딸린 struct iommu_pmu.
 *
 * sysfs 의 is_visible 콜백은 kobject 만 받으므로, 거기서 "이 PMU 가 무엇을
 * 지원하는가"를 알려면 이 되돌리기가 필요하다. perf 코어가 pmu_dev_alloc()
 * 에서 struct pmu 를 drvdata 로 걸어 두었기 때문에 container_of 로 바깥
 * 구조체까지 갈 수 있다.
 *
 * 호출 체인:
 *   sysfs 속성의 is_visible → [이 함수]
 */
static inline struct iommu_pmu *dev_to_iommu_pmu(struct device *dev)
{
	/*
	 * The perf_event creates its own dev for each PMU.
	 * See pmu_dev_alloc()
	 */
	return container_of(dev_get_drvdata(dev), struct iommu_pmu, pmu);	/* [한국어] perf 코어가 걸어 둔 struct pmu 로부터 바깥 구조체를 얻는다 */
}

/*
 * [한국어] 필터 속성 하나에 필요한 네 가지를 통째로 만들어 내는 매크로
 *
 * 필터가 열 개나 되고 각각 속성·배열·가시성 함수·그룹을 필요로 해서, 손으로
 * 쓰면 마흔 덩어리가 된다. 이름 하나에서 전부 찍어 내면 그 사이의 불일치가
 * 원천적으로 불가능해진다.
 *
 * 본체의 각 줄:
 *  - PMU_FORMAT_ATTR(_name, _format)
 *      "이 필터는 config1 의 몇 번 비트인가"를 sysfs 에 알리는 속성.
 *  - static struct attribute *_name##_attr[]
 *      그 속성 하나만 담은 배열. 그룹이 배열을 요구하기 때문에 필요하다.
 *  - _name##_is_visible(...)
 *      sysfs 가 파일을 만들지 말지 물을 때 답하는 콜백. dev_to_iommu_pmu 로
 *      PMU 를 되찾아 filter 비트맵에 이 필터가 있는지 본다. 없으면 0 을
 *      돌려 파일 자체가 생기지 않는다 — 사용자는 sysfs 만 보고 이 기계에서
 *      실제로 걸 수 있는 필터를 알 수 있다.
 *  - static struct attribute_group _name
 *      위 셋을 "format" 디렉터리용 그룹으로 묶는다. 이 그룹이
 *      iommu_pmu_attr_update 목록에 실려 등록된다.
 */
#define IOMMU_PMU_ATTR(_name, _format, _filter)				\
	PMU_FORMAT_ATTR(_name, _format);				\
									\
static struct attribute *_name##_attr[] = {				\
	&format_attr_##_name.attr,					\
	NULL								\
};									\
									\
static umode_t								\
_name##_is_visible(struct kobject *kobj, struct attribute *attr, int i)	\
{									\
	struct device *dev = kobj_to_dev(kobj);				\
	struct iommu_pmu *iommu_pmu = dev_to_iommu_pmu(dev);		\
									\
	if (!iommu_pmu)							\
		return 0;						\
	return (iommu_pmu->filter & _filter) ? attr->mode : 0;		\
}									\
									\
static struct attribute_group _name = {					\
	.name		= "format",					\
	.attrs		= _name##_attr,					\
	.is_visible	= _name##_is_visible,				\
};

IOMMU_PMU_ATTR(filter_requester_id_en,	"config1:0",		IOMMU_PMU_FILTER_REQUESTER_ID);	/* [한국어] 요청자 id 필터를 켤지 (config1의 0번 비트). 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_domain_en,	"config1:1",		IOMMU_PMU_FILTER_DOMAIN);	/* [한국어] 도메인 필터를 켤지. 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_pasid_en,		"config1:2",		IOMMU_PMU_FILTER_PASID);	/* [한국어] PASID 필터를 켤지. 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_ats_en,		"config1:3",		IOMMU_PMU_FILTER_ATS);	/* [한국어] ATS 필터를 켤지. 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_page_table_en,	"config1:4",		IOMMU_PMU_FILTER_PAGE_TABLE);	/* [한국어] 페이지 테이블 종류 필터를 켤지. 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_requester_id,	"config1:16-31",	IOMMU_PMU_FILTER_REQUESTER_ID);	/* [한국어] 걸러 낼 요청자 id 값 (config1의 16~31비트). 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_domain,		"config1:32-47",	IOMMU_PMU_FILTER_DOMAIN);	/* [한국어] 걸러 낼 도메인 id 값. 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_pasid,		"config2:0-21",		IOMMU_PMU_FILTER_PASID);	/* [한국어] 걸러 낼 PASID 값 — 22비트라 config2 로 넘어간다. 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_ats,		"config2:24-28",	IOMMU_PMU_FILTER_ATS);	/* [한국어] 걸러 낼 ATS 상태. 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */
IOMMU_PMU_ATTR(filter_page_table,	"config2:32-36",	IOMMU_PMU_FILTER_PAGE_TABLE);	/* [한국어] 걸러 낼 페이지 테이블 종류 (1단계/2단계/중첩 등). 하드웨어가 그 필터를 지원할 때만 sysfs 에 나타난다 */

#define iommu_pmu_en_requester_id(e)		((e) & 0x1)	/* [한국어] 사용자가 요청자 id 필터를 켰는지 검사 */
#define iommu_pmu_en_domain(e)			(((e) >> 1) & 0x1)	/* [한국어] 도메인 필터를 켰는지 */
#define iommu_pmu_en_pasid(e)			(((e) >> 2) & 0x1)	/* [한국어] PASID 필터를 켰는지 */
#define iommu_pmu_en_ats(e)			(((e) >> 3) & 0x1)	/* [한국어] ATS 필터를 켰는지 */
#define iommu_pmu_en_page_table(e)		(((e) >> 4) & 0x1)	/* [한국어] 페이지 테이블 필터를 켰는지 */
#define iommu_pmu_get_requester_id(filter)	(((filter) >> 16) & 0xffff)	/* [한국어] config1 에서 요청자 id 값을 꺼낸다 */
#define iommu_pmu_get_domain(filter)		(((filter) >> 32) & 0xffff)	/* [한국어] config1 상위에서 도메인 id 를 꺼낸다 */
#define iommu_pmu_get_pasid(filter)		((filter) & 0x3fffff)	/* [한국어] config2 하위 22비트가 PASID */
#define iommu_pmu_get_ats(filter)		(((filter) >> 24) & 0x1f)	/* [한국어] config2 에서 ATS 필터 값 */
#define iommu_pmu_get_page_table(filter)	(((filter) >> 32) & 0x1f)	/* [한국어] config2 상위에서 페이지 테이블 종류 */

/*
 * [한국어] 필터 값을 카운터의 필터 레지스터에 쓴다
 *
 * 두 조건을 모두 만족할 때만 쓴다: 하드웨어가 그 필터를 지원하고
 * (iommu_pmu->filter), 사용자가 그 필터를 켰을 때(iommu_pmu_en_##_name).
 * 둘 중 하나라도 아니면 아무것도 하지 않아, 필터가 걸리지 않은 상태로
 * 남는다 — 지원하지 않는 레지스터를 건드리는 것보다 안전하다.
 *
 * 쓰는 주소는 세 조각을 더해 구한다:
 *  - _idx * IOMMU_PMU_CFG_OFFSET  : 카운터마다 설정 블록이 하나씩 있다.
 *  - IOMMU_PMU_CFG_SIZE           : 그 블록에서 필터 영역이 시작하는 자리.
 *  - (ffs(_filter) - 1) * IOMMU_PMU_CFG_FILTERS_OFFSET
 *                                 : 필터 종류마다 한 칸씩. _filter 는 비트
 *                                   하나짜리 상수라 ffs 로 그 비트 위치를
 *                                   구하면 그대로 몇 번째 필터인지가 된다.
 *
 * 값에 IOMMU_PMU_FILTER_EN 을 함께 OR 하는 이유: 같은 레지스터의 활성화
 * 비트라, 값만 쓰고 이 비트를 빼면 필터가 동작하지 않는다.
 */
#define iommu_pmu_set_filter(_name, _config, _filter, _idx, _econfig)		\
{										\
	if ((iommu_pmu->filter & _filter) && iommu_pmu_en_##_name(_econfig)) {	\
		writel(iommu_pmu_get_##_name(_config) | IOMMU_PMU_FILTER_EN,	\
		       iommu_pmu->cfg_reg + _idx * IOMMU_PMU_CFG_OFFSET +	\
		       IOMMU_PMU_CFG_SIZE +					\
		       (ffs(_filter) - 1) * IOMMU_PMU_CFG_FILTERS_OFFSET);	\
	}									\
}

/*
 * [한국어] 필터를 해제한다 — 같은 자리에 0 을 쓴다
 *
 * 0 을 쓰면 값과 함께 활성화 비트도 내려가 필터가 풀린다. 주소 계산은
 * iommu_pmu_set_filter 와 완전히 같다.
 *
 * 이것이 왜 중요한가: 카운터는 이벤트가 끝나면 다른 이벤트에 재배정된다.
 * 앞 이벤트가 걸어 둔 "PASID 3만 센다" 같은 필터가 남아 있으면, 다음
 * 이벤트는 이유를 알 수 없는 0 을 세게 된다. 그래서 iommu_pmu_del 이
 * 다섯 필터를 모두 지운 뒤에야 자리를 반납한다.
 *
 * 지원하지 않는 필터는 건너뛰는 것도 set 과 같다.
 */
#define iommu_pmu_clear_filter(_filter, _idx)					\
{										\
	if (iommu_pmu->filter & _filter) {					\
		writel(0,							\
		       iommu_pmu->cfg_reg + _idx * IOMMU_PMU_CFG_OFFSET +	\
		       IOMMU_PMU_CFG_SIZE +					\
		       (ffs(_filter) - 1) * IOMMU_PMU_CFG_FILTERS_OFFSET);	\
	}									\
}

/*
 * Define the event attr related functions
 * Input: _name: event attr name
 *        _string: string of the event in sysfs
 *        _g_idx: event group encoding
 *        _event: event encoding
 */
/*
 * [한국어] (위 영어 주석에 이어) 이벤트 하나의 sysfs 속성을 통째로 만든다
 *
 * IOMMU_PMU_ATTR 와 같은 구조이고, 판단 근거만 다르다. 필터 쪽은
 * iommu_pmu->filter 를 보지만 이쪽은 evcap[그룹]에 이 이벤트의 비트가
 * 있는지를 본다.
 *
 * 본체의 각 줄:
 *  - PMU_EVENT_ATTR_STRING(_name, event_attr_##_name, _string)
 *      "event_group=0x3,event=0x002" 같은 문자열을 그대로 내보내는 속성.
 *      perf 도구가 이 문자열을 읽어 사용자의 이벤트 이름을 config 값으로
 *      번역하므로, 사용자는 비트 인코딩을 몰라도 된다.
 *  - static struct attribute *_name##_attr[]
 *      그 속성 하나만 담은 배열.
 *  - _name##_is_visible(...)
 *      evcap[_g_idx] 에 _event 비트가 없으면 0 을 돌려 파일을 만들지
 *      않는다. 그래서 sysfs 의 events 디렉터리는 곧 "이 기계가 실제로
 *      셀 수 있는 이벤트 목록"이 된다.
 *  - static struct attribute_group _name
 *      위 셋을 "events" 디렉터리용 그룹으로 묶는다.
 */
#define IOMMU_PMU_EVENT_ATTR(_name, _string, _g_idx, _event)			\
	PMU_EVENT_ATTR_STRING(_name, event_attr_##_name, _string)		\
										\
static struct attribute *_name##_attr[] = {					\
	&event_attr_##_name.attr.attr,						\
	NULL									\
};										\
										\
static umode_t									\
_name##_is_visible(struct kobject *kobj, struct attribute *attr, int i)		\
{										\
	struct device *dev = kobj_to_dev(kobj);					\
	struct iommu_pmu *iommu_pmu = dev_to_iommu_pmu(dev);			\
										\
	if (!iommu_pmu)								\
		return 0;							\
	return (iommu_pmu->evcap[_g_idx] & _event) ? attr->mode : 0;		\
}										\
										\
static struct attribute_group _name = {						\
	.name		= "events",						\
	.attrs		= _name##_attr,						\
	.is_visible	= _name##_is_visible,					\
};

IOMMU_PMU_EVENT_ATTR(iommu_clocks,		"event_group=0x0,event=0x001", 0x0, 0x001)	/* [한국어] IOMMU 내부 클록 — 다른 이벤트를 정규화하는 기준 */
IOMMU_PMU_EVENT_ATTR(iommu_requests,		"event_group=0x0,event=0x002", 0x0, 0x002)	/* [한국어] 유닛이 받은 변환 요청 수. 전체 부하의 척도 */
IOMMU_PMU_EVENT_ATTR(pw_occupancy,		"event_group=0x0,event=0x004", 0x0, 0x004)	/* [한국어] 페이지 워크가 진행 중이던 시간. 캐시 미스의 실제 비용을 보여 준다 */
IOMMU_PMU_EVENT_ATTR(ats_blocked,		"event_group=0x0,event=0x008", 0x0, 0x008)	/* [한국어] ATS 요청이 막힌 횟수. 장치가 캐시하려던 변환이 거부됐다는 뜻 */
IOMMU_PMU_EVENT_ATTR(iommu_mrds,		"event_group=0x1,event=0x001", 0x1, 0x001)	/* [한국어] 메모리 읽기 요청 수 */
IOMMU_PMU_EVENT_ATTR(iommu_mem_blocked,		"event_group=0x1,event=0x020", 0x1, 0x020)	/* [한국어] 메모리 접근이 막힌 횟수 */
IOMMU_PMU_EVENT_ATTR(pg_req_posted,		"event_group=0x1,event=0x040", 0x1, 0x040)	/* [한국어] 페이지 요청 큐에 올라간 요청 수 — SVA 의 페이지 폴트 빈도 */
IOMMU_PMU_EVENT_ATTR(ctxt_cache_lookup,		"event_group=0x2,event=0x001", 0x2, 0x001)	/* [한국어] 컨텍스트 캐시 조회 수 */
IOMMU_PMU_EVENT_ATTR(ctxt_cache_hit,		"event_group=0x2,event=0x002", 0x2, 0x002)	/* [한국어] 그중 적중. lookup 과의 비율이 장치 수 대비 캐시 크기를 말해 준다 */
IOMMU_PMU_EVENT_ATTR(pasid_cache_lookup,	"event_group=0x2,event=0x004", 0x2, 0x004)	/* [한국어] PASID 캐시 조회 수 */
IOMMU_PMU_EVENT_ATTR(pasid_cache_hit,		"event_group=0x2,event=0x008", 0x2, 0x008)	/* [한국어] 그중 적중. SVA 를 많이 쓰면 이 비율이 중요해진다 */
IOMMU_PMU_EVENT_ATTR(ss_nonleaf_lookup,		"event_group=0x2,event=0x010", 0x2, 0x010)	/* [한국어] 2단계(second-stage) 페이지 테이블의 비잎 노드 조회 */
IOMMU_PMU_EVENT_ATTR(ss_nonleaf_hit,		"event_group=0x2,event=0x020", 0x2, 0x020)	/* [한국어] 그중 적중. 중간 단계 캐시가 얼마나 도움이 되는지 */
IOMMU_PMU_EVENT_ATTR(fs_nonleaf_lookup,		"event_group=0x2,event=0x040", 0x2, 0x040)	/* [한국어] 1단계(first-stage) 비잎 노드 조회 */
IOMMU_PMU_EVENT_ATTR(fs_nonleaf_hit,		"event_group=0x2,event=0x080", 0x2, 0x080)	/* [한국어] 그중 적중 */
IOMMU_PMU_EVENT_ATTR(hpt_nonleaf_lookup,	"event_group=0x2,event=0x100", 0x2, 0x100)	/* [한국어] 호스트 페이지 테이블의 비잎 노드 조회 — 중첩 변환에서 나타난다 */
IOMMU_PMU_EVENT_ATTR(hpt_nonleaf_hit,		"event_group=0x2,event=0x200", 0x2, 0x200)	/* [한국어] 그중 적중 */
IOMMU_PMU_EVENT_ATTR(iotlb_lookup,		"event_group=0x3,event=0x001", 0x3, 0x001)	/* [한국어] IOTLB 조회 수. 변환마다 가장 먼저 닿는 캐시다 */
IOMMU_PMU_EVENT_ATTR(iotlb_hit,			"event_group=0x3,event=0x002", 0x3, 0x002)	/* [한국어] 그중 적중. 이 비율이 낮으면 페이지 워크 비용이 그대로 지연이 된다 */
IOMMU_PMU_EVENT_ATTR(hpt_leaf_lookup,		"event_group=0x3,event=0x004", 0x3, 0x004)	/* [한국어] 호스트 페이지 테이블의 잎 조회 */
IOMMU_PMU_EVENT_ATTR(hpt_leaf_hit,		"event_group=0x3,event=0x008", 0x3, 0x008)	/* [한국어] 그중 적중 */
IOMMU_PMU_EVENT_ATTR(int_cache_lookup,		"event_group=0x4,event=0x001", 0x4, 0x001)	/* [한국어] 인터럽트 항목 캐시 조회 수 */
IOMMU_PMU_EVENT_ATTR(int_cache_hit_nonposted,	"event_group=0x4,event=0x002", 0x4, 0x002)	/* [한국어] 평범한 재매핑 항목의 적중 */
IOMMU_PMU_EVENT_ATTR(int_cache_hit_posted,	"event_group=0x4,event=0x004", 0x4, 0x004)	/* [한국어] 포스티드 항목의 적중. 둘을 나눠 세는 이유는 전달 경로가 달라 비용도 다르기 때문 */

/*
 * [한국어] iommu_pmu_attr_update — 하드웨어 능력에 따라 나타나거나 사라지는 속성들
 *
 * 위의 기본 그룹(iommu_pmu_attr_groups)과 달리, 이 목록의 그룹들은 모두
 * is_visible 콜백을 갖고 있다. perf 코어가 sysfs 를 만들 때 그 콜백을
 * 불러 각 속성의 mode 를 정하므로, 이 유닛이 지원하지 않는 필터와
 * 세지 못하는 이벤트는 파일 자체가 생기지 않는다.
 *
 * 이 방식이 필요한 이유: VT-d 유닛마다 지원하는 이벤트와 필터가 다르다.
 * 목록을 하나로 두고 보이는 것만 다르게 하면, 사용자는 sysfs 를 훑는
 * 것만으로 "이 기계에서 실제로 잴 수 있는 것"을 알 수 있다.
 */
static const struct attribute_group *iommu_pmu_attr_update[] = {
	&filter_requester_id_en,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_domain_en,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_pasid_en,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_ats_en,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_page_table_en,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_requester_id,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_domain,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_pasid,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_ats,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&filter_page_table,	/* [한국어] 이 필터 속성은 하드웨어가 그 필터를 지원할 때만 보인다 */
	&iommu_clocks,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&iommu_requests,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&pw_occupancy,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&ats_blocked,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&iommu_mrds,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&iommu_mem_blocked,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&pg_req_posted,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&ctxt_cache_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&ctxt_cache_hit,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&pasid_cache_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&pasid_cache_hit,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&ss_nonleaf_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&ss_nonleaf_hit,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&fs_nonleaf_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&fs_nonleaf_hit,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&hpt_nonleaf_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&hpt_nonleaf_hit,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&iotlb_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&iotlb_hit,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&hpt_leaf_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&hpt_leaf_hit,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&int_cache_lookup,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&int_cache_hit_nonposted,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	&int_cache_hit_posted,	/* [한국어] 이 이벤트 속성은 유닛의 evcap 에 해당 비트가 있을 때만 보인다 */
	NULL	/* [한국어] 목록의 끝 */
};

/*
 * [한국어]
 * iommu_event_base - 카운터 idx 의 값 레지스터 주소
 *
 * @iommu_pmu: 대상 PMU.
 * @idx: 카운터 번호.
 * @return: 그 카운터의 MMIO 주소.
 *
 * 카운터 레지스터의 간격(cntr_stride)이 상수가 아니라 하드웨어가 알려 주는
 * 값인 점이 중요하다. 구현마다 카운터 폭이 달라 간격도 달라지므로,
 * 초기화 때 읽어 둔 값을 여기서 쓴다.
 *
 * 호출 체인:
 *   iommu_pmu_event_update()/start()/stop() → [이 함수]
 */
static inline void __iomem *
iommu_event_base(struct iommu_pmu *iommu_pmu, int idx)
{
	return iommu_pmu->cntr_reg + idx * iommu_pmu->cntr_stride;	/* [한국어] 간격이 하드웨어가 알려 준 값인 이유: 구현마다 카운터 폭이 다르다 */
}

/*
 * [한국어]
 * iommu_config_base - 카운터 idx 의 설정 레지스터 주소
 *
 * @iommu_pmu: 대상 PMU.
 * @idx: 카운터 번호.
 * @return: 그 카운터 설정 블록의 MMIO 주소.
 *
 * 이쪽 간격은 IOMMU_PMU_CFG_OFFSET 상수다 — 설정 블록의 크기는 스펙이
 * 정해 두었기 때문이다. 필터 레지스터도 이 주소를 기준으로 오프셋을
 * 더해 찾는다.
 *
 * 호출 체인:
 *   iommu_pmu_start()/stop()/enable_event() → [이 함수]
 */
static inline void __iomem *
iommu_config_base(struct iommu_pmu *iommu_pmu, int idx)
{
	return iommu_pmu->cfg_reg + idx * IOMMU_PMU_CFG_OFFSET;	/* [한국어] 설정 블록 크기는 스펙이 정한 상수다 */
}

/*
 * [한국어]
 * iommu_event_to_pmu - perf 이벤트에서 이 PMU 구조체를 얻는다
 *
 * @event: perf 이벤트.
 * @return: 그 이벤트가 속한 struct iommu_pmu.
 *
 * perf 콜백은 event 만 받으므로, 하드웨어에 손대려면 매번 이 변환이
 * 필요하다. event->pmu 가 우리 구조체 안에 박혀 있어 container_of 로
 * 바깥까지 갈 수 있다.
 *
 * 호출 체인:
 *   거의 모든 PMU 콜백 → [이 함수]
 */
static inline struct iommu_pmu *iommu_event_to_pmu(struct perf_event *event)
{
	return container_of(event->pmu, struct iommu_pmu, pmu);	/* [한국어] 이벤트에 박힌 pmu 포인터에서 바깥 구조체로 */
}

/*
 * [한국어]
 * iommu_event_config - 사용자의 이벤트 설정을 하드웨어 형식으로 바꾼다
 *
 * @event: perf 이벤트.
 * @return: 카운터 설정 레지스터에 쓸 값.
 *
 * 사용자는 config 하나에 이벤트 선택(하위 28비트)과 그룹 인덱스(28~31비트)를
 * 담아 보낸다. 하드웨어 레지스터는 그 둘을 다른 위치에 요구하므로, 꺼내서
 * 제자리에 다시 넣는 것이 이 함수가 하는 일이다.
 *
 * IOMMU_EVENT_CFG_INT 를 항상 붙이는 이유: 카운터가 넘칠 때 인터럽트를
 * 받겠다는 뜻이다. 이것이 없으면 오버플로를 놓쳐 카운트가 조용히 틀어진다.
 *
 * 호출 체인:
 *   iommu_pmu_event_init() → [이 함수]
 */
static inline u64 iommu_event_config(struct perf_event *event)
{
	u64 config = event->attr.config;	/* [한국어] 사용자가 지정한 이벤트 설정 */

	return (iommu_event_select(config) << IOMMU_EVENT_CFG_ES_SHIFT) |	/* [한국어] 이벤트 선택 비트를 하드웨어가 요구하는 자리로 옮긴다 */
	       (iommu_event_group(config) << IOMMU_EVENT_CFG_EGI_SHIFT) |	/* [한국어] 그룹 인덱스도 마찬가지로 */
	       IOMMU_EVENT_CFG_INT;	/* [한국어] 오버플로 인터럽트를 켠다. 없으면 카운트가 조용히 틀어진다 */
}

/*
 * [한국어]
 * is_iommu_pmu_event - 그 이벤트가 이 PMU 의 것인지 판별한다
 *
 * @iommu_pmu: 기준이 되는 PMU.
 * @event: 검사할 이벤트.
 * @return: 같은 PMU 의 이벤트면 참.
 *
 * perf 이벤트 그룹에는 서로 다른 PMU 의 이벤트가 섞일 수 있다(예: CPU
 * 사이클과 IOMMU IOTLB 미스를 함께 묶는 경우). 카운터 개수를 셀 때는 우리
 * 것만 세야 하므로 이 구분이 필요하다.
 *
 * 호출 체인:
 *   iommu_pmu_validate_group() → [이 함수]
 */
static inline bool is_iommu_pmu_event(struct iommu_pmu *iommu_pmu,
				      struct perf_event *event)
{
	return event->pmu == &iommu_pmu->pmu;	/* [한국어] 그룹에는 다른 PMU 의 이벤트가 섞일 수 있어 이 구분이 필요하다 */
}

/*
 * [한국어]
 * iommu_pmu_validate_event - 요청한 이벤트 그룹이 이 하드웨어에 있는지 확인한다
 *
 * @event: 검사할 이벤트.
 * @return: 0 이면 가능, -EINVAL 이면 없는 그룹이다.
 *
 * 이벤트 그룹 수(num_eg)는 유닛마다 다르고 초기화 때 하드웨어에서 읽어
 * 둔다. 없는 그룹을 요청하면 존재하지 않는 레지스터를 건드리게 되므로
 * 여기서 막는다.
 *
 * 그룹 안의 이벤트 비트까지 검사하지 않는 이유: 그쪽은 sysfs 의 is_visible
 * 이 이미 걸러 준다. 사용자가 숫자로 직접 지정하면 통과할 수 있지만, 그
 * 경우 하드웨어가 세지 않아 0 이 나올 뿐 위험하지는 않다.
 *
 * 호출 체인:
 *   iommu_pmu_event_init() → [이 함수]
 */
static int iommu_pmu_validate_event(struct perf_event *event)
{
	struct iommu_pmu *iommu_pmu = iommu_event_to_pmu(event);	/* [한국어] 대상 PMU */
	u32 event_group = iommu_event_group(event->attr.config);	/* [한국어] 사용자가 지정한 그룹 인덱스 */

	if (event_group >= iommu_pmu->num_eg)	/* [한국어] 이 유닛이 가진 그룹 수를 넘으면 */
		return -EINVAL;	/* [한국어] 존재하지 않는 레지스터를 건드리게 되므로 막는다 */

	return 0;	/* [한국어] 가능한 이벤트 */
}

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_pmu_validate_group - 이벤트 그룹이 카운터 수 안에 들어가는지 확인한다
 *
 * @event: 그룹의 한 이벤트.
 * @return: 0 이면 가능, -EINVAL 이면 카운터가 모자란다.
 *
 * perf 의 이벤트 그룹은 "함께 세어야 의미가 있는" 이벤트 묶음이다. 예를
 * 들어 iotlb_lookup 과 iotlb_hit 을 서로 다른 시간대에 재면 적중률이
 * 엉터리가 된다. 그래서 그룹은 전부 동시에 스케줄되어야 하고, 물리
 * 카운터가 모자라면 아예 만들지 않는 편이 낫다.
 *
 * 꺼져 있는 형제(state <= OFF)를 세지 않는 이유: 그런 이벤트는 카운터를
 * 차지하지 않는다.
 *
 * 호출 체인:
 *   iommu_pmu_event_init() → [이 함수] → is_iommu_pmu_event()
 */
static int iommu_pmu_validate_group(struct perf_event *event)
{
	struct iommu_pmu *iommu_pmu = iommu_event_to_pmu(event);	/* [한국어] 대상 PMU */
	struct perf_event *sibling;	/* [한국어] 그룹의 형제 이벤트 */
	int nr = 0;	/* [한국어] 우리 PMU 의 이벤트 수 */

	/*
	 * All events in a group must be scheduled simultaneously.
	 * Check whether there is enough counters for all the events.
	 */
	for_each_sibling_event(sibling, event->group_leader) {	/* [한국어] 그룹 전체를 훑는다 */
		if (!is_iommu_pmu_event(iommu_pmu, sibling) ||	/* [한국어] 다른 PMU 의 이벤트이거나 */
		    sibling->state <= PERF_EVENT_STATE_OFF)	/* [한국어] 꺼져 있는 이벤트면 */
			continue;	/* [한국어] 카운터를 차지하지 않으므로 세지 않는다 */

		if (++nr > iommu_pmu->num_cntr)	/* [한국어] 물리 카운터보다 많으면 */
			return -EINVAL;	/* [한국어] 동시에 셀 수 없으므로 아예 만들지 않는다 */
	}

	return 0;	/* [한국어] 전부 들어간다 */
}

/*
 * [한국어]
 * iommu_pmu_event_init - perf 이벤트를 이 PMU 용으로 검증하고 준비한다
 *
 * @event: 사용자가 만들려는 이벤트.
 * @return: 0 성공. -ENOENT 는 "내 것이 아니다", -EINVAL 은 "내 것인데 불가능하다".
 *
 * 두 반환값의 차이가 중요하다. perf 코어는 모든 PMU 에 이벤트를 돌려 가며
 * 물어보므로, 타입이 다르면 -ENOENT 로 "다른 데 물어보라"고 답해야 한다.
 * -EINVAL 을 주면 코어가 탐색을 멈춘다.
 *
 * 샘플링을 거부하는 이유: 이 하드웨어는 카운터가 넘칠 때 인터럽트를 낼 뿐,
 * "N번마다 현재 상태를 기록"하는 샘플링 기능이 없다. 그래서 계수(counting)
 * 모드만 지원한다.
 *
 * event->cpu < 0(태스크에 붙은 이벤트)을 거부하는 이유: IOMMU 카운터는
 * 시스템 전체를 세는 것이라 특정 태스크에 귀속시킬 수 없다.
 *
 * 마지막에 그룹 검증을 하는 이유: 개별 이벤트가 불가능하면 그룹을 볼
 * 필요가 없으므로 순서가 이렇다.
 *
 * 호출 체인:
 *   perf_event_open(2) → perf 코어 → [이 함수]
 *     → iommu_pmu_validate_event() → iommu_pmu_validate_group()
 */
static int iommu_pmu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;	/* [한국어] 하드웨어 설정을 담을 곳 */

	if (event->attr.type != event->pmu->type)	/* [한국어] 우리 PMU 를 지목한 이벤트가 아니면 */
		return -ENOENT;	/* [한국어] "다른 데 물어보라" — -EINVAL 을 주면 코어가 탐색을 멈춘다 */

	/* sampling not supported */
	if (event->attr.sample_period)	/* [한국어] 샘플링 요청이면 */
		return -EINVAL;	/* [한국어] 이 하드웨어는 계수만 되고 샘플링 기능이 없다 */

	if (event->cpu < 0)	/* [한국어] 특정 태스크에 붙이려 하면 */
		return -EINVAL;	/* [한국어] IOMMU 카운터는 시스템 전체를 세므로 태스크에 귀속시킬 수 없다 */

	if (iommu_pmu_validate_event(event))	/* [한국어] 이 하드웨어에 있는 이벤트인지 */
		return -EINVAL;	/* [한국어] 없는 그룹이면 거절 */

	hwc->config = iommu_event_config(event);	/* [한국어] 하드웨어 레지스터에 쓸 형식으로 미리 만들어 둔다 */

	return iommu_pmu_validate_group(event);	/* [한국어] 마지막으로 그룹이 카운터 수 안에 들어가는지 */
}

/*
 * [한국어]
 * iommu_pmu_event_update - 카운터의 증가분을 이벤트 누적값에 더한다
 *
 * @event: 갱신할 이벤트.
 *
 * 하드웨어 카운터는 폭이 좁고(예: 48비트) 계속 돌아가므로, perf 가 사용자에게
 * 보여 주는 64비트 누적값은 소프트웨어가 만들어야 한다. 방법은 "직전에 읽은
 * 값과 지금 값의 차이를 더한다"이다.
 *
 * 카운터가 한 바퀴 돌아도 맞는 이유가 shift 트릭이다. 두 값을 모두
 * (64 - 폭)만큼 왼쪽으로 밀면 카운터의 최상위 비트가 64비트의 최상위가
 * 되고, 그 상태에서 빼면 부호 없는 뺄셈의 자연스러운 랩어라운드가 정확히
 * "몇 번 증가했는가"를 준다. 다시 오른쪽으로 밀어 원래 자리로 되돌린다.
 * 폭을 상수로 두지 않는 이유는 구현마다 다르기 때문이다.
 *
 * again 루프의 이유: 오버플로 인터럽트 핸들러와 이 함수가 동시에 같은
 * 이벤트를 갱신할 수 있다. local64_xchg 로 prev_count 를 바꿔 놓고 그
 * 반환값이 우리가 읽은 값과 다르면 누군가 끼어든 것이므로 다시 시작한다.
 * 그러지 않으면 같은 구간을 두 번 더하거나 놓친다.
 *
 * 실행 컨텍스트: perf 콜백(인터럽트 비활성) 또는 오버플로 인터럽트 핸들러.
 *
 * 호출 체인:
 *   iommu_pmu_stop()/iommu_pmu_counter_overflow() → [이 함수]
 */
static void iommu_pmu_event_update(struct perf_event *event)
{
	struct iommu_pmu *iommu_pmu = iommu_event_to_pmu(event);	/* [한국어] 대상 PMU */
	struct hw_perf_event *hwc = &event->hw;	/* [한국어] 직전 값과 카운터 번호를 담고 있다 */
	u64 prev_count, new_count, delta;	/* [한국어] 직전 값, 현재 값, 그 차이 */
	int shift = 64 - iommu_pmu->cntr_width;	/* [한국어] 카운터의 최상위 비트를 64비트의 최상위로 올릴 시프트 양 */

again:	/* [한국어] 다른 CPU 가 끼어들어 prev_count 를 바꿨을 때 되돌아오는 지점 */
	prev_count = local64_read(&hwc->prev_count);	/* [한국어] 마지막으로 읽었던 값 */
	new_count = readq(iommu_event_base(iommu_pmu, hwc->idx));	/* [한국어] 지금의 하드웨어 값 */
	if (local64_xchg(&hwc->prev_count, new_count) != prev_count)	/* [한국어] 바꿔치기의 반환값이 예상과 다르면 누군가 먼저 갱신했다 */
		goto again;	/* [한국어] 같은 구간을 두 번 더하지 않도록 처음부터 다시 */

	/*
	 * The counter width is enumerated. Always shift the counter
	 * before using it.
	 */
	delta = (new_count << shift) - (prev_count << shift);	/* [한국어] 둘 다 최상위로 밀어 빼면 랩어라운드가 자연스럽게 처리된다 */
	delta >>= shift;	/* [한국어] 원래 자리로 되돌린다 */

	local64_add(delta, &event->count);	/* [한국어] 증가분만 사용자에게 보이는 누적값에 더한다 */
}

/*
 * [한국어]
 * iommu_pmu_start - 배정된 카운터를 실제로 돌리기 시작한다
 *
 * @event: 시작할 이벤트.
 * @flags: PERF_EF_RELOAD 등 perf 코어의 요청.
 *
 * 카운터를 0 으로 되돌리지 않고 현재 값을 prev_count 로 삼는 것이 핵심이다.
 * 하드웨어 카운터는 여러 이벤트가 시분할로 쓰고 리셋할 수단도 마땅치 않아,
 * "지금 값을 기준점으로 잡고 이후의 증가분만 센다"가 유일하게 맞는 방법이다.
 *
 * ecmd 오류를 무시하는 이유는 원 주석이 길게 설명한다. 요약하면 perf 의
 * start 콜백에는 실패를 알릴 통로가 없고(반환값이 void), 오직 이 PMU 만
 * 런타임 하드웨어 오류를 낼 수 있어서 그 하나를 위해 공용 인터페이스를
 * 바꾸지 않기로 한 것이다. 실패하면 사용자는 <not count> 를 보게 되는데,
 * 그것 자체가 힌트가 된다.
 *
 * perf_event_update_userpage: 사용자 공간이 mmap 으로 직접 읽는 페이지를
 * 갱신해, 시스템 콜 없이 카운터를 읽을 수 있게 한다.
 *
 * 호출 체인:
 *   perf 코어(또는 iommu_pmu_add) → [이 함수] → ecmd_submit_sync(ENABLE)
 */
static void iommu_pmu_start(struct perf_event *event, int flags)
{
	struct iommu_pmu *iommu_pmu = iommu_event_to_pmu(event);	/* [한국어] 대상 PMU */
	struct intel_iommu *iommu = iommu_pmu->iommu;	/* [한국어] ecmd 를 보낼 유닛 */
	struct hw_perf_event *hwc = &event->hw;	/* [한국어] 하드웨어 상태 */
	u64 count;	/* [한국어] 현재 카운터 값 */

	if (WARN_ON_ONCE(!(hwc->state & PERF_HES_STOPPED)))	/* [한국어] 멈춰 있지 않은데 시작하라는 것은 코어 쪽 상태 오류다 */
		return;	/* [한국어] 아무것도 하지 않는다 */

	if (WARN_ON_ONCE(hwc->idx < 0 || hwc->idx >= IOMMU_PMU_IDX_MAX))	/* [한국어] 배정되지 않았거나 범위를 벗어난 카운터 */
		return;	/* [한국어] 엉뚱한 레지스터를 건드리지 않게 */

	if (flags & PERF_EF_RELOAD)	/* [한국어] 재적재 요청이면 */
		WARN_ON_ONCE(!(event->hw.state & PERF_HES_UPTODATE));	/* [한국어] 누적값이 최신이어야 한다는 전제를 확인 */

	hwc->state = 0;	/* [한국어] 이제 돌고 있고 값도 갱신이 필요한 상태 */

	/* Always reprogram the period */
	count = readq(iommu_event_base(iommu_pmu, hwc->idx));	/* [한국어] 카운터를 0 으로 되돌릴 수 없으므로 */
	local64_set((&hwc->prev_count), count);	/* [한국어] 현재 값을 기준점으로 삼는다. 이후의 증가분만 센다 */

	/*
	 * The error of ecmd will be ignored.
	 * - The existing perf_event subsystem doesn't handle the error.
	 *   Only IOMMU PMU returns runtime HW error. We don't want to
	 *   change the existing generic interfaces for the specific case.
	 * - It's a corner case caused by HW, which is very unlikely to
	 *   happen. There is nothing SW can do.
	 * - The worst case is that the user will get <not count> with
	 *   perf command, which can give the user some hints.
	 */
	ecmd_submit_sync(iommu, DMA_ECMD_ENABLE, hwc->idx, 0);	/* [한국어] 이 카운터를 켠다. 오류를 무시하는 이유는 위 영어 주석이 설명한다 */

	perf_event_update_userpage(event);	/* [한국어] mmap 으로 직접 읽는 사용자 페이지를 갱신 */
}

/*
 * [한국어]
 * iommu_pmu_stop - 카운터를 멈추고 그때까지의 값을 거둔다
 *
 * @event: 멈출 이벤트.
 * @flags: PERF_EF_UPDATE 등.
 *
 * 순서가 중요하다. 먼저 하드웨어를 멈추고(DISABLE) 그다음에 값을 읽는다.
 * 반대로 하면 읽은 뒤 멈추기 전까지의 증가분이 사라진다.
 *
 * 이미 멈춰 있으면 아무것도 하지 않는다 — perf 코어가 stop 을 여러 번
 * 부를 수 있고, 그때마다 값을 다시 더하면 중복 계수가 된다.
 *
 * PERF_HES_UPTODATE 를 세우는 것은 "소프트웨어 누적값이 하드웨어와
 * 일치한다"는 선언이라, 이후 start 가 그것을 검사한다.
 *
 * 호출 체인:
 *   perf 코어/iommu_pmu_del() → [이 함수] → iommu_pmu_event_update()
 */
static void iommu_pmu_stop(struct perf_event *event, int flags)
{
	struct iommu_pmu *iommu_pmu = iommu_event_to_pmu(event);	/* [한국어] 대상 PMU */
	struct intel_iommu *iommu = iommu_pmu->iommu;	/* [한국어] ecmd 를 보낼 유닛 */
	struct hw_perf_event *hwc = &event->hw;	/* [한국어] 하드웨어 상태 */

	if (!(hwc->state & PERF_HES_STOPPED)) {	/* [한국어] 이미 멈춰 있으면 다시 거두면 중복 계수가 된다 */
		ecmd_submit_sync(iommu, DMA_ECMD_DISABLE, hwc->idx, 0);	/* [한국어] 먼저 하드웨어를 멈춘다 */

		iommu_pmu_event_update(event);	/* [한국어] 그다음 값을 거둔다. 순서를 바꾸면 그 사이 증가분이 사라진다 */

		hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;	/* [한국어] 멈췄고 누적값이 하드웨어와 일치한다는 표시 */
	}
}

/*
 * [한국어]
 * iommu_pmu_validate_per_cntr_event - 그 카운터가 이 이벤트를 셀 수 있는지 확인한다
 *
 * @iommu_pmu: 대상 PMU.
 * @idx: 후보 카운터 번호.
 * @event: 세려는 이벤트.
 * @return: 0 이면 가능, -EINVAL 이면 이 카운터로는 못 센다.
 *
 * VT-d PerfMon 의 카운터는 모두 같지 않다. 어떤 카운터는 특정 이벤트만
 * 셀 수 있고, 그 능력이 cntr_evcap[카운터][그룹] 에 비트맵으로 적혀 있다.
 * 그래서 "빈 카운터를 아무거나 준다"가 아니라 능력을 확인해야 한다.
 *
 * 호출 체인:
 *   iommu_pmu_assign_event() → [이 함수]
 */
static inline int
iommu_pmu_validate_per_cntr_event(struct iommu_pmu *iommu_pmu,
				  int idx, struct perf_event *event)
{
	u32 event_group = iommu_event_group(event->attr.config);	/* [한국어] 이벤트의 그룹 인덱스 */
	u32 select = iommu_event_select(event->attr.config);	/* [한국어] 그룹 안에서 어떤 이벤트인지 */

	if (!(iommu_pmu->cntr_evcap[idx][event_group] & select))	/* [한국어] 이 카운터가 그 이벤트를 셀 능력이 있는가 */
		return -EINVAL;	/* [한국어] 없으면 다른 카운터를 찾아야 한다 */

	return 0;	/* [한국어] 셀 수 있다 */
}

/*
 * [한국어]
 * iommu_pmu_assign_event - 이벤트에 물리 카운터를 배정하고 하드웨어를 설정한다
 *
 * @iommu_pmu: 대상 PMU.
 * @event: 배정할 이벤트.
 * @return: 0 성공, -EINVAL 이면 이 이벤트를 셀 수 있는 빈 카운터가 없다.
 *
 * 뒤에서부터 찾는 이유가 원 주석의 요점이다. 제한된 이벤트만 셀 수 있는
 * 카운터가 보통 뒤쪽에 몰려 있으므로, 그것들을 먼저 소진하면 범용 카운터가
 * 앞쪽에 남는다. 앞에서부터 찾으면 범용 카운터가 먼저 없어져, 나중에 오는
 * 특수 이벤트가 자리를 못 찾는 일이 생긴다.
 *
 * test_and_set → 검사 → 실패 시 clear 의 순서: 먼저 자리를 예약해 다른
 * CPU 와 경쟁하지 않게 한 뒤 능력을 확인하고, 맞지 않으면 놓아 준다.
 * 순서를 바꾸면 확인과 예약 사이에 다른 이벤트가 끼어들 수 있다.
 *
 * 필터 설정이 다섯 번 반복되는 이유: 필터마다 레지스터 자리가 다르고,
 * 하드웨어가 그 필터를 지원하는지도 따로 확인해야 한다. 매크로가 그 검사와
 * 오프셋 계산을 함께 처리한다.
 *
 * pasid/ats/page_table 은 값을 config2 에서 꺼내면서 "켰는지"는 config1 에서
 * 보는데, 이는 활성화 비트가 모두 config1 하위에 모여 있고 값만 넓은 필드가
 * 필요해 config2 로 넘어갔기 때문이다.
 *
 * 호출 체인:
 *   iommu_pmu_add() → [이 함수] → iommu_pmu_validate_per_cntr_event()
 */
static int iommu_pmu_assign_event(struct iommu_pmu *iommu_pmu,
				  struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;	/* [한국어] 배정 결과를 적을 곳 */
	int idx;	/* [한국어] 카운터 번호 */

	/*
	 * The counters which support limited events are usually at the end.
	 * Schedule them first to accommodate more events.
	 */
	for (idx = iommu_pmu->num_cntr - 1; idx >= 0; idx--) {	/* [한국어] 뒤에서부터 — 제한된 카운터를 먼저 소진해 범용 카운터를 남긴다 */
		if (test_and_set_bit(idx, iommu_pmu->used_mask))	/* [한국어] 이미 쓰이는 자리면 */
			continue;	/* [한국어] 다음 후보로 */
		/* Check per-counter event capabilities */
		if (!iommu_pmu_validate_per_cntr_event(iommu_pmu, idx, event))	/* [한국어] 예약해 둔 뒤 능력을 확인한다 */
			break;	/* [한국어] 셀 수 있으면 이 자리로 결정 */
		clear_bit(idx, iommu_pmu->used_mask);	/* [한국어] 못 세면 예약을 풀고 다음 후보로 */
	}
	if (idx < 0)	/* [한국어] 전부 훑었는데 맞는 자리가 없다 */
		return -EINVAL;	/* [한국어] 이 이벤트는 지금 셀 수 없다 */

	iommu_pmu->event_list[idx] = event;	/* [한국어] 오버플로 처리가 이 배열로 이벤트를 되찾는다 */
	hwc->idx = idx;	/* [한국어] 이벤트에도 카운터 번호를 기록 */

	/* config events */
	writeq(hwc->config, iommu_config_base(iommu_pmu, idx));	/* [한국어] 미리 만들어 둔 설정 워드를 카운터 설정 레지스터에 */

	iommu_pmu_set_filter(requester_id, event->attr.config1,	/* [한국어] 요청자 id 필터 — 특정 장치의 요청만 세고 싶을 때 */
			     IOMMU_PMU_FILTER_REQUESTER_ID, idx,
			     event->attr.config1);
	iommu_pmu_set_filter(domain, event->attr.config1,	/* [한국어] 도메인 필터 — 특정 도메인에 속한 요청만 */
			     IOMMU_PMU_FILTER_DOMAIN, idx,
			     event->attr.config1);
	iommu_pmu_set_filter(pasid, event->attr.config2,	/* [한국어] PASID 필터. 값은 config2 에 있지만 활성화 비트는 config1 에 있다 */
			     IOMMU_PMU_FILTER_PASID, idx,
			     event->attr.config1);
	iommu_pmu_set_filter(ats, event->attr.config2,	/* [한국어] ATS 필터 */
			     IOMMU_PMU_FILTER_ATS, idx,
			     event->attr.config1);
	iommu_pmu_set_filter(page_table, event->attr.config2,	/* [한국어] 페이지 테이블 종류 필터 — 1단계/2단계/중첩을 구분해 잴 수 있다 */
			     IOMMU_PMU_FILTER_PAGE_TABLE, idx,
			     event->attr.config1);

	return 0;	/* [한국어] 배정과 설정 완료 */
}

/*
 * [한국어]
 * iommu_pmu_add - 이벤트를 이 PMU 에 올린다 (perf 콜백)
 *
 * @event: 올릴 이벤트.
 * @flags: PERF_EF_START 면 곧바로 세기 시작한다.
 * @return: 0 성공, 음수면 카운터가 없다.
 *
 * perf 의 add/del 은 "이벤트를 이 CPU 에서 스케줄한다/뺀다"에 해당하고,
 * start/stop 은 그 안에서 세기를 켜고 끄는 단계다. 그래서 add 는 카운터
 * 배정까지만 하고, 실제 시작은 flags 를 보고 결정한다.
 *
 * 초기 state 를 STOPPED | UPTODATE 로 두는 이유: 아직 세지 않았고 소프트웨어
 * 누적값도 하드웨어와 어긋나지 않은 상태라는 뜻이다. start 가 그 두 비트를
 * 확인한다.
 *
 * 호출 체인:
 *   perf 코어 → [이 함수] → iommu_pmu_assign_event() → iommu_pmu_start()
 */
static int iommu_pmu_add(struct perf_event *event, int flags)
{
	struct iommu_pmu *iommu_pmu = iommu_event_to_pmu(event);	/* [한국어] 대상 PMU */
	struct hw_perf_event *hwc = &event->hw;	/* [한국어] 하드웨어 상태 */
	int ret;	/* [한국어] 배정 결과 */

	ret = iommu_pmu_assign_event(iommu_pmu, event);	/* [한국어] 물리 카운터를 잡고 설정까지 끝낸다 */
	if (ret < 0)	/* [한국어] 빈 카운터가 없다 */
		return ret;	/* [한국어] 코어가 이 이벤트를 다음 기회에 다시 시도한다 */

	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;	/* [한국어] 아직 세지 않았고 누적값도 어긋나지 않은 상태 */

	if (flags & PERF_EF_START)	/* [한국어] 코어가 곧바로 시작하라고 했으면 */
		iommu_pmu_start(event, 0);	/* [한국어] 카운팅을 켠다 */

	return 0;	/* [한국어] 올리기 성공 */
}

/*
 * [한국어]
 * iommu_pmu_del - 이벤트를 PMU 에서 내리고 카운터를 반납한다 (perf 콜백)
 *
 * @event: 내릴 이벤트.
 * @flags: 쓰지 않는다.
 *
 * 필터를 일일이 지우는 것이 이 함수에서 가장 중요한 부분이다. 카운터는
 * 재사용되므로, 앞 이벤트가 걸어 둔 필터가 남아 있으면 다음 이벤트가
 * 영문 모를 값을 세게 된다 — 예를 들어 "특정 PASID 만" 필터가 남으면
 * 다른 이벤트의 카운트가 대부분 0 으로 나온다.
 *
 * hw.idx 를 -1 로 되돌리는 이유: 이 이벤트가 더 이상 어떤 카운터에도
 * 붙어 있지 않음을 분명히 남긴다.
 *
 * 순서: stop 으로 값을 먼저 거둔 뒤 필터를 지우고 자리를 놓는다.
 *
 * 호출 체인:
 *   perf 코어 → [이 함수] → iommu_pmu_stop()
 */
static void iommu_pmu_del(struct perf_event *event, int flags)
{
	struct iommu_pmu *iommu_pmu = iommu_event_to_pmu(event);	/* [한국어] 대상 PMU */
	int idx = event->hw.idx;	/* [한국어] 반납할 카운터 번호 */

	iommu_pmu_stop(event, PERF_EF_UPDATE);	/* [한국어] 먼저 멈추고 값을 거둔다 */

	iommu_pmu_clear_filter(IOMMU_PMU_FILTER_REQUESTER_ID, idx);	/* [한국어] 필터를 지운다 — 남으면 다음 이벤트가 영문 모를 값을 센다 */
	iommu_pmu_clear_filter(IOMMU_PMU_FILTER_DOMAIN, idx);	/* [한국어] 도메인 필터 해제 */
	iommu_pmu_clear_filter(IOMMU_PMU_FILTER_PASID, idx);	/* [한국어] PASID 필터 해제 */
	iommu_pmu_clear_filter(IOMMU_PMU_FILTER_ATS, idx);	/* [한국어] ATS 필터 해제 */
	iommu_pmu_clear_filter(IOMMU_PMU_FILTER_PAGE_TABLE, idx);	/* [한국어] 페이지 테이블 필터 해제 */

	iommu_pmu->event_list[idx] = NULL;	/* [한국어] 오버플로 처리가 더 이상 이 이벤트를 찾지 않게 */
	event->hw.idx = -1;	/* [한국어] 어떤 카운터에도 붙어 있지 않음을 분명히 */
	clear_bit(idx, iommu_pmu->used_mask);	/* [한국어] 자리를 반납한다 */

	perf_event_update_userpage(event);	/* [한국어] 사용자 페이지를 갱신 */
}

/*
 * [한국어]
 * iommu_pmu_enable - PMU 전체의 카운팅을 재개한다 (perf 콜백)
 *
 * @pmu: 대상 PMU.
 *
 * perf 코어는 여러 이벤트의 설정을 바꿀 때 PMU 를 통째로 얼렸다가
 * (disable) 한꺼번에 녹인다(enable). 그래야 설정을 바꾸는 동안의 어정쩡한
 * 구간이 카운트에 섞이지 않는다.
 *
 * UNFREEZE 는 개별 카운터의 ENABLE 과 다른 층위다. 카운터별 ENABLE 이
 * "이 카운터를 쓴다"라면, UNFREEZE 는 "이제 세도 좋다"에 해당한다.
 *
 * 호출 체인:
 *   perf 코어 → [이 함수] → ecmd_submit_sync(UNFREEZE)
 */
static void iommu_pmu_enable(struct pmu *pmu)
{
	struct iommu_pmu *iommu_pmu = container_of(pmu, struct iommu_pmu, pmu);	/* [한국어] 바깥 구조체로 */
	struct intel_iommu *iommu = iommu_pmu->iommu;	/* [한국어] 명령을 보낼 유닛 */

	ecmd_submit_sync(iommu, DMA_ECMD_UNFREEZE, 0, 0);	/* [한국어] PMU 전체의 카운팅 재개. 카운터별 ENABLE 과는 다른 층위다 */
}

/*
 * [한국어]
 * iommu_pmu_disable - PMU 전체의 카운팅을 얼린다 (perf 콜백)
 *
 * @pmu: 대상 PMU.
 *
 * enable 의 짝이다. 설정을 바꾸는 동안 카운터가 돌면 그 구간이 어느
 * 이벤트의 것인지 애매해지므로, 코어가 먼저 여기를 부른다.
 *
 * 호출 체인:
 *   perf 코어 → [이 함수] → ecmd_submit_sync(FREEZE)
 */
static void iommu_pmu_disable(struct pmu *pmu)
{
	struct iommu_pmu *iommu_pmu = container_of(pmu, struct iommu_pmu, pmu);	/* [한국어] 바깥 구조체로 */
	struct intel_iommu *iommu = iommu_pmu->iommu;	/* [한국어] 명령을 보낼 유닛 */

	ecmd_submit_sync(iommu, DMA_ECMD_FREEZE, 0, 0);	/* [한국어] 설정을 바꾸는 동안 카운터를 얼려 어정쩡한 구간이 섞이지 않게 */
}

/*
 * [한국어]
 * iommu_pmu_counter_overflow - 넘친 카운터들의 값을 거둬 누적값에 반영한다
 *
 * @iommu_pmu: 대상 PMU.
 *
 * 하드웨어 카운터는 폭이 좁아 오래 세면 반드시 넘친다. 넘칠 때마다 값을
 * 거둬 64비트 누적값에 더해 두어야 사용자에게 보이는 숫자가 맞는다. 이
 * 함수가 그 일을 한다.
 *
 * while 루프인 이유는 원 주석대로다: 두 카운터가 거의 동시에 넘칠 수 있어,
 * 상태를 지운 직후 또 다른 오버플로가 올라와 있을 수 있다. 상태 레지스터가
 * 0 이 될 때까지 반복해야 놓치지 않는다.
 *
 * 상태를 마지막에 쓰는 이유: 오버플로 상태 레지스터는 "1을 써서 지우는"
 * 방식이라, 읽은 비트를 그대로 다시 쓰면 그 비트들만 정확히 지워진다.
 * 처리 전에 지우면 그 사이의 새 오버플로까지 함께 없애 버린다.
 *
 * event 가 NULL 인 경우: 카운터가 넘쳤는데 배정된 이벤트가 없다는 뜻이라
 * 소프트웨어와 하드웨어 상태가 어긋난 것이다. 한 번만 경고하고 넘어간다.
 *
 * 실행 컨텍스트: 인터럽트 핸들러. 그래서 iommu_pmu_event_update 의
 * again 루프가 다른 CPU 의 갱신과 부딪히는 경우를 다뤄야 한다.
 *
 * 호출 체인:
 *   iommu_pmu_irq_handler() → [이 함수] → iommu_pmu_event_update()
 */
static void iommu_pmu_counter_overflow(struct iommu_pmu *iommu_pmu)
{
	struct perf_event *event;	/* [한국어] 넘친 카운터에 배정된 이벤트 */
	u64 status;	/* [한국어] 오버플로 상태 비트맵 */
	int i;	/* [한국어] 카운터 인덱스 */

	/*
	 * Two counters may be overflowed very close. Always check
	 * whether there are more to handle.
	 */
	while ((status = readq(iommu_pmu->overflow))) {	/* [한국어] 지운 직후 또 넘칠 수 있어 0 이 될 때까지 반복 */
		for_each_set_bit(i, (unsigned long *)&status, iommu_pmu->num_cntr) {	/* [한국어] 넘친 카운터만 골라 */
			/*
			 * Find the assigned event of the counter.
			 * Accumulate the value into the event->count.
			 */
			event = iommu_pmu->event_list[i];	/* [한국어] 배정 때 기록해 둔 이벤트를 되찾는다 */
			if (!event) {	/* [한국어] 카운터는 넘쳤는데 이벤트가 없다 — 상태가 어긋난 것 */
				pr_warn_once("Cannot find the assigned event for counter %d\n", i);	/* [한국어] 한 번만 경고하고 */
				continue;	/* [한국어] 다음 카운터로 */
			}
			iommu_pmu_event_update(event);	/* [한국어] 증가분을 누적값에 반영한다 */
		}

		writeq(status, iommu_pmu->overflow);	/* [한국어] 읽은 비트를 그대로 다시 써 그것들만 지운다. 처리 전에 지우면 새 오버플로까지 없앤다 */
	}
}

/*
 * [한국어]
 * iommu_pmu_irq_handler - PerfMon 오버플로 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev_id: 등록 때 넘긴 struct intel_iommu.
 * @return: 우리 인터럽트였으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * PERFINTRSTS 를 먼저 확인하는 이유: 이 인터럽트 선은 다른 IOMMU 이벤트와
 * 공유될 수 있다. 상태 비트가 없으면 우리 것이 아니므로 IRQ_NONE 을 돌려
 * 다른 핸들러에 넘긴다.
 *
 * 상태를 마지막에 지우는 순서: 오버플로 처리를 먼저 끝내고 나서 지워야,
 * 처리 중에 발생한 새 오버플로가 다음 인터럽트로 다시 올라온다.
 *
 * 실행 컨텍스트: 하드웨어 인터럽트. 잠들 수 없고, 빨리 끝나야 한다.
 *
 * 호출 체인:
 *   IOMMU 폴트/성능 인터럽트 → [이 함수] → iommu_pmu_counter_overflow()
 */
static irqreturn_t iommu_pmu_irq_handler(int irq, void *dev_id)
{
	struct intel_iommu *iommu = dev_id;	/* [한국어] 등록 때 넘긴 유닛 */

	if (!readl(iommu->reg + DMAR_PERFINTRSTS_REG))	/* [한국어] 상태 비트가 없으면 우리 인터럽트가 아니다 */
		return IRQ_NONE;	/* [한국어] 공유된 선의 다른 핸들러에 넘긴다 */

	iommu_pmu_counter_overflow(iommu->pmu);	/* [한국어] 넘친 카운터들의 값을 거둔다 */

	/* Clear the status bit */
	writel(DMA_PERFINTRSTS_PIS, iommu->reg + DMAR_PERFINTRSTS_REG);	/* [한국어] 처리가 끝난 뒤 상태를 지운다. 그 사이의 새 오버플로는 다음 인터럽트로 온다 */

	return IRQ_HANDLED;	/* [한국어] 우리가 처리했다 */
}

/*
 * [한국어]
 * __iommu_pmu_register - struct pmu 를 채워 perf 코어에 등록한다
 *
 * @iommu: 대상 유닛.
 * @return: perf_pmu_register() 의 결과.
 *
 * 이 함수가 채우는 콜백 표가 곧 "이 PMU 가 perf 에서 어떻게 보이는가"다.
 * 몇 가지 선택이 눈에 띈다.
 *
 * task_ctx_nr = perf_invalid_context: 이 PMU 는 태스크에 붙을 수 없다.
 * IOMMU 카운터는 시스템 전체의 DMA 를 세는 것이라 특정 프로세스에
 * 귀속시킬 수 없기 때문이다. scope = SYS_WIDE 도 같은 뜻을 다른 층위에서
 * 말한다.
 *
 * PERF_PMU_CAP_NO_EXCLUDE: 유저/커널/게스트를 구분해 세는 기능이 없다.
 * DMA 요청에는 "어느 특권 수준에서 시작됐는가"라는 개념이 없으므로,
 * exclude_user 같은 옵션을 받으면 코어가 거절하게 만든다.
 *
 * -1 을 CPU 로 넘기는 이유: 어느 CPU 에도 매이지 않는 PMU 라는 뜻이다.
 *
 * 호출 체인:
 *   iommu_pmu_register() → [이 함수] → perf_pmu_register()
 */
static int __iommu_pmu_register(struct intel_iommu *iommu)
{
	struct iommu_pmu *iommu_pmu = iommu->pmu;	/* [한국어] alloc 단계에서 지어 둔 구조체 */

	iommu_pmu->pmu.name		= iommu->name;	/* [한국어] sysfs 와 perf 도구에 보일 이름(dmar0 등) */
	iommu_pmu->pmu.task_ctx_nr	= perf_invalid_context;	/* [한국어] 태스크에 붙일 수 없다 — 시스템 전체 DMA 를 세는 카운터다 */
	iommu_pmu->pmu.event_init	= iommu_pmu_event_init;	/* [한국어] 이벤트 검증과 준비 */
	iommu_pmu->pmu.pmu_enable	= iommu_pmu_enable;	/* [한국어] PMU 전체 해동 */
	iommu_pmu->pmu.pmu_disable	= iommu_pmu_disable;	/* [한국어] PMU 전체 동결 */
	iommu_pmu->pmu.add		= iommu_pmu_add;	/* [한국어] 카운터 배정 */
	iommu_pmu->pmu.del		= iommu_pmu_del;	/* [한국어] 카운터 반납 */
	iommu_pmu->pmu.start		= iommu_pmu_start;	/* [한국어] 카운팅 시작 */
	iommu_pmu->pmu.stop		= iommu_pmu_stop;	/* [한국어] 카운팅 중지와 값 수거 */
	iommu_pmu->pmu.read		= iommu_pmu_event_update;	/* [한국어] 사용자가 값을 물어볼 때 누적값을 갱신 */
	iommu_pmu->pmu.attr_groups	= iommu_pmu_attr_groups;	/* [한국어] 항상 보이는 format/events 디렉터리 */
	iommu_pmu->pmu.attr_update	= iommu_pmu_attr_update;	/* [한국어] 하드웨어 능력에 따라 나타나거나 사라지는 속성들 */
	iommu_pmu->pmu.capabilities	= PERF_PMU_CAP_NO_EXCLUDE;	/* [한국어] DMA 요청에는 특권 수준 개념이 없어 exclude_user 같은 옵션을 지원하지 않는다 */
	iommu_pmu->pmu.scope		= PERF_PMU_SCOPE_SYS_WIDE;	/* [한국어] 시스템 전역 범위 */
	iommu_pmu->pmu.module		= THIS_MODULE;	/* [한국어] 모듈 참조 계수를 위해 */

	return perf_pmu_register(&iommu_pmu->pmu, iommu_pmu->pmu.name, -1);	/* [한국어] -1 은 어느 CPU 에도 매이지 않는다는 뜻 */
}

/*
 * [한국어]
 * get_perf_reg_address - 간접 레지스터의 실제 주소를 구한다
 *
 * @iommu: 대상 유닛.
 * @offset: 오프셋을 담고 있는 레지스터의 위치.
 * @return: 그 오프셋이 가리키는 MMIO 주소.
 *
 * PerfMon 의 설정·카운터·오버플로 레지스터는 위치가 고정이 아니다.
 * 대신 "그것들이 어디 있는지"를 알려 주는 레지스터가 고정 위치에 있고,
 * 그 값을 읽어 기준 주소에 더해야 실제 위치가 나온다. 구현마다 레지스터
 * 배치를 달리할 수 있게 한 설계다.
 *
 * 호출 체인:
 *   alloc_iommu_pmu() → [이 함수]
 */
static inline void __iomem *
get_perf_reg_address(struct intel_iommu *iommu, u32 offset)
{
	u32 off = readl(iommu->reg + offset);	/* [한국어] 레지스터 위치가 고정이 아니라 다른 레지스터가 알려 준다 */

	return iommu->reg + off;	/* [한국어] 기준 주소에 더해 실제 위치를 얻는다 */
}

/*
 * [한국어]
 * alloc_iommu_pmu - 유닛의 PerfMon 능력을 읽어 PMU 구조체를 구성한다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공(지원하지 않는 유닛도 0). -ENODEV 면 쓸 수 없는 구성,
 *          -ENOMEM 이면 자원 부족.
 *
 * 이 파일에서 가장 긴 함수이고, 하는 일은 "하드웨어에게 무엇을 할 수
 * 있는지 물어보고 그대로 자료구조를 짓는 것"이다. 카운터 수, 카운터 폭,
 * 카운터 간격, 이벤트 그룹 수, 지원 필터가 모두 하드웨어가 알려 주는
 * 값이라 상수로 둘 수 없다.
 *
 * 전제 조건을 여럿 확인하는 이유:
 *  - ecap_pms 가 없으면 이 유닛에 PerfMon 자체가 없다. 오류가 아니라
 *    그냥 없는 것이므로 0 을 돌려준다.
 *  - ECMD 는 카운터를 켜고 끄는 유일한 수단이라 없으면 쓸 수 없다.
 *  - 오버플로 인터럽트가 없으면 카운터가 넘치는 것을 놓쳐 값이 조용히
 *    틀어지므로, 아예 지원하지 않는 편이 낫다.
 *
 * 이벤트 능력이 두 겹인 구조가 이 함수의 핵심이다. evcap[그룹]은 "유닛
 * 전체가 셀 수 있는 이벤트"이고, cntr_evcap[카운터][그룹]은 "그 카운터가
 * 셀 수 있는 이벤트"다. 처음에는 모든 카운터에 전역 능력을 복사해 두고,
 * 카운터별 능력(pcc 비트)이 있는 카운터만 그것으로 덮어쓴다. 그렇게 해야
 * "능력을 따로 알리지 않는 카운터는 전부 셀 수 있다"는 기본값이 성립한다.
 *
 * 그 반대 방향의 보정도 있다: 어떤 이벤트는 특정 카운터에서만 세어지므로,
 * 카운터별 능력을 읽으면서 evcap 에도 OR 로 더해 준다. 그러지 않으면
 * sysfs 의 is_visible 이 그 이벤트를 숨겨 사용자가 쓸 수 없게 된다.
 *
 * 능력이 어긋나는 카운터를 잘라 내는 이유: 원 주석대로 하드웨어 버그로
 * 카운터마다 폭이 다를 수 있는데, 그러면 iommu_pmu_event_update 의 shift
 * 계산이 그 카운터에서만 틀린다. 조용히 틀린 값을 내느니 카운터 수를
 * 줄여 그 뒤를 아예 쓰지 않는다.
 *
 * 실행 컨텍스트: 유닛 초기화(프로세스 문맥). GFP_KERNEL 가능.
 *
 * 호출 체인:
 *   alloc_iommu()/유닛 초기화 → [이 함수] → get_perf_reg_address()
 */
int alloc_iommu_pmu(struct intel_iommu *iommu)
{
	struct iommu_pmu *iommu_pmu;	/* [한국어] 지을 구조체 */
	int i, j, ret;	/* [한국어] 순회 인덱스와 결과 */
	u64 perfcap;	/* [한국어] PerfMon 능력 레지스터 값 */
	u32 cap;	/* [한국어] 카운터별 능력 */

	if (!ecap_pms(iommu->ecap))	/* [한국어] 이 유닛에 PerfMon 자체가 없다 */
		return 0;	/* [한국어] 오류가 아니라 그냥 없는 것이라 0 을 돌려준다 */

	/* The IOMMU PMU requires the ECMD support as well */
	if (!cap_ecmds(iommu->cap))	/* [한국어] ECMD 는 카운터를 켜고 끄는 유일한 수단이다 */
		return -ENODEV;	/* [한국어] 없으면 쓸 수 없다 */

	perfcap = readq(iommu->reg + DMAR_PERFCAP_REG);	/* [한국어] 하드웨어에게 무엇을 할 수 있는지 묻는다 */
	/* The performance monitoring is not supported. */
	if (!perfcap)	/* [한국어] 아무 능력도 없다 */
		return -ENODEV;	/* [한국어] 쓸 수 없다 */

	/* Sanity check for the number of the counters and event groups */
	if (!pcap_num_cntr(perfcap) || !pcap_num_event_group(perfcap))	/* [한국어] 카운터나 이벤트 그룹이 0 개면 */
		return -ENODEV;	/* [한국어] 셀 것이 없다 */

	/* The interrupt on overflow is required */
	if (!pcap_interrupt(perfcap))	/* [한국어] 오버플로 인터럽트가 없으면 */
		return -ENODEV;	/* [한국어] 카운터가 넘치는 것을 놓쳐 값이 조용히 틀어진다 */

	/* Check required Enhanced Command Capability */
	if (!ecmd_has_pmu_essential(iommu))	/* [한국어] PMU 에 꼭 필요한 ECMD 하위 명령이 없으면 */
		return -ENODEV;	/* [한국어] 쓸 수 없다 */

	iommu_pmu = kzalloc_obj(*iommu_pmu);	/* [한국어] 구조체 할당 */
	if (!iommu_pmu)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 되돌릴 것이 없다 */

	iommu_pmu->num_cntr = pcap_num_cntr(perfcap);	/* [한국어] 하드웨어가 가진 카운터 수 */
	if (iommu_pmu->num_cntr > IOMMU_PMU_IDX_MAX) {	/* [한국어] 드라이버가 다룰 수 있는 상한을 넘으면 */
		pr_warn_once("The number of IOMMU counters %d > max(%d), clipping!",	/* [한국어] 한 번만 알리고 */
			     iommu_pmu->num_cntr, IOMMU_PMU_IDX_MAX);
		iommu_pmu->num_cntr = IOMMU_PMU_IDX_MAX;	/* [한국어] 상한까지만 쓴다. 나머지는 놀리지만 동작은 정상이다 */
	}

	iommu_pmu->cntr_width = pcap_cntr_width(perfcap);	/* [한국어] 카운터 폭. event_update 의 shift 계산 근거 */
	iommu_pmu->filter = pcap_filters_mask(perfcap);	/* [한국어] 지원하는 필터 비트맵. sysfs 가시성과 필터 설정이 이 값을 본다 */
	iommu_pmu->cntr_stride = pcap_cntr_stride(perfcap);	/* [한국어] 카운터 레지스터 간격 */
	iommu_pmu->num_eg = pcap_num_event_group(perfcap);	/* [한국어] 이벤트 그룹 수 */

	iommu_pmu->evcap = kcalloc(iommu_pmu->num_eg, sizeof(u64), GFP_KERNEL);	/* [한국어] 그룹별 전역 이벤트 능력 */
	if (!iommu_pmu->evcap) {	/* [한국어] 할당 실패 */
		ret = -ENOMEM;	/* [한국어] 자원 부족 */
		goto free_pmu;	/* [한국어] 구조체만 되돌린다 */
	}

	/* Parse event group capabilities */
	for (i = 0; i < iommu_pmu->num_eg; i++) {	/* [한국어] 그룹마다 */
		u64 pcap;	/* [한국어] 그 그룹의 능력 레지스터 */

		pcap = readq(iommu->reg + DMAR_PERFEVNTCAP_REG +	/* [한국어] 그룹별 능력 레지스터는 일정 간격으로 늘어서 있다 */
			     i * IOMMU_PMU_CAP_REGS_STEP);	/* [한국어] 인덱스 × 간격 */
		iommu_pmu->evcap[i] = pecap_es(pcap);	/* [한국어] 이 그룹에서 셀 수 있는 이벤트 비트맵 */
	}

	iommu_pmu->cntr_evcap = kcalloc(iommu_pmu->num_cntr, sizeof(u32 *), GFP_KERNEL);	/* [한국어] 카운터별 능력 표의 바깥 배열 */
	if (!iommu_pmu->cntr_evcap) {	/* [한국어] 할당 실패 */
		ret = -ENOMEM;	/* [한국어] 자원 부족 */
		goto free_pmu_evcap;	/* [한국어] evcap 까지 되돌린다 */
	}
	for (i = 0; i < iommu_pmu->num_cntr; i++) {	/* [한국어] 카운터마다 */
		iommu_pmu->cntr_evcap[i] = kcalloc(iommu_pmu->num_eg, sizeof(u32), GFP_KERNEL);	/* [한국어] 그룹 수만큼의 안쪽 배열 */
		if (!iommu_pmu->cntr_evcap[i]) {	/* [한국어] 할당 실패 */
			ret = -ENOMEM;	/* [한국어] 자원 부족 */
			goto free_pmu_cntr_evcap;	/* [한국어] 지금까지 잡은 안쪽 배열까지 되돌린다 */
		}
		/*
		 * Set to the global capabilities, will adjust according
		 * to per-counter capabilities later.
		 */
		for (j = 0; j < iommu_pmu->num_eg; j++)	/* [한국어] 일단 전역 능력을 복사해 둔다 */
			iommu_pmu->cntr_evcap[i][j] = (u32)iommu_pmu->evcap[j];	/* [한국어] 능력을 따로 알리지 않는 카운터는 전부 셀 수 있다는 기본값 */
	}

	iommu_pmu->cfg_reg = get_perf_reg_address(iommu, DMAR_PERFCFGOFF_REG);	/* [한국어] 설정 레지스터 블록의 실제 위치 */
	iommu_pmu->cntr_reg = get_perf_reg_address(iommu, DMAR_PERFCNTROFF_REG);	/* [한국어] 카운터 값 레지스터의 위치 */
	iommu_pmu->overflow = get_perf_reg_address(iommu, DMAR_PERFOVFOFF_REG);	/* [한국어] 오버플로 상태 레지스터의 위치 */

	/*
	 * Check per-counter capabilities. All counters should have the
	 * same capabilities on Interrupt on Overflow Support and Counter
	 * Width.
	 */
	for (i = 0; i < iommu_pmu->num_cntr; i++) {	/* [한국어] 카운터마다 개별 능력을 확인한다 */
		cap = readl(iommu_pmu->cfg_reg +	/* [한국어] 카운터별 능력 레지스터를 읽는다 */
			    i * IOMMU_PMU_CFG_OFFSET +	/* [한국어] 카운터 인덱스 × 설정 블록 크기 */
			    IOMMU_PMU_CFG_CNTRCAP_OFFSET);	/* [한국어] 그 블록 안의 능력 필드 */
		if (!iommu_cntrcap_pcc(cap))	/* [한국어] 이 카운터가 별도 능력을 알리지 않으면 */
			continue;	/* [한국어] 전역 능력을 그대로 쓴다 */

		/*
		 * It's possible that some counters have a different
		 * capability because of e.g., HW bug. Check the corner
		 * case here and simply drop those counters.
		 */
		if ((iommu_cntrcap_cw(cap) != iommu_pmu->cntr_width) ||	/* [한국어] 폭이 다르거나 */
		    !iommu_cntrcap_ios(cap)) {	/* [한국어] 오버플로 인터럽트를 지원하지 않으면 */
			iommu_pmu->num_cntr = i;	/* [한국어] 이 카운터부터 아예 쓰지 않는다 — shift 계산이 틀리느니 줄이는 편이 낫다 */
			pr_warn("PMU counter capability inconsistent, counter number reduced to %d\n",	/* [한국어] 줄였다는 사실을 알린다 */
				iommu_pmu->num_cntr);
		}

		/* Clear the pre-defined events group */
		for (j = 0; j < iommu_pmu->num_eg; j++)	/* [한국어] 이 카운터는 능력을 따로 알렸으므로 */
			iommu_pmu->cntr_evcap[i][j] = 0;	/* [한국어] 복사해 둔 전역 능력을 지우고 */

		/* Override with per-counter event capabilities */
		for (j = 0; j < iommu_cntrcap_egcnt(cap); j++) {	/* [한국어] 알린 그룹 수만큼 */
			cap = readl(iommu_pmu->cfg_reg + i * IOMMU_PMU_CFG_OFFSET +	/* [한국어] 카운터별 이벤트 능력 레지스터를 읽어 */
				    IOMMU_PMU_CFG_CNTREVCAP_OFFSET +	/* [한국어] 그 오프셋에서 */
				    (j * IOMMU_PMU_OFF_REGS_STEP));	/* [한국어] 그룹마다 일정 간격으로 */
			iommu_pmu->cntr_evcap[i][iommu_event_group(cap)] = iommu_event_select(cap);	/* [한국어] 실제 능력으로 덮어쓴다 */
			/*
			 * Some events may only be supported by a specific counter.
			 * Track them in the evcap as well.
			 */
			iommu_pmu->evcap[iommu_event_group(cap)] |= iommu_event_select(cap);	/* [한국어] 전역 능력에도 더한다 — 그러지 않으면 sysfs 가 그 이벤트를 숨겨 쓸 수 없게 된다 */
		}
	}

	iommu_pmu->iommu = iommu;	/* [한국어] 거꾸로 참조. 콜백에서 ecmd 를 보낼 때 쓴다 */
	iommu->pmu = iommu_pmu;	/* [한국어] 유닛에 매단다. 이 시점부터 등록이 가능해진다 */

	return 0;	/* [한국어] 구성 완료 */

free_pmu_cntr_evcap:	/* [한국어] 카운터별 능력 배열까지 잡은 뒤 실패한 경우 */
	for (i = 0; i < iommu_pmu->num_cntr; i++)	/* [한국어] 잡은 안쪽 배열을 하나씩 */
		kfree(iommu_pmu->cntr_evcap[i]);	/* [한국어] 2차원 배열은 안쪽부터 놓아야 한다 */
	kfree(iommu_pmu->cntr_evcap);	/* [한국어] 바깥 배열 */
free_pmu_evcap:	/* [한국어] 전역 능력 배열까지 잡은 뒤 실패한 경우 */
	kfree(iommu_pmu->evcap);	/* [한국어] 전역 능력 배열 */
free_pmu:	/* [한국어] 구조체만 잡은 뒤 실패한 경우 */
	kfree(iommu_pmu);	/* [한국어] 구조체 자신 */

	return ret;	/* [한국어] 실패 원인 그대로 */
}

/*
 * [한국어]
 * free_iommu_pmu - PMU 구조체와 능력 배열을 모두 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * cntr_evcap 은 2차원이라 안쪽 배열부터 하나씩 놓아야 한다. 바깥만 놓으면
 * 각 카운터의 능력 배열이 새어 나간다.
 *
 * evcap 유무로 안쪽 해제를 감싸는 이유: alloc 이 단계별로 실패할 수 있어
 * cntr_evcap 이 아직 없는 상태로 여기 올 수 있다. evcap 이 있으면 그
 * 다음 단계까지 갔다는 뜻이라 이 검사가 대신 서 준다.
 *
 * 마지막에 iommu->pmu 를 NULL 로 만들어 이후 경로가 해제된 구조체를
 * 건드리지 않게 한다.
 *
 * 호출 체인:
 *   iommu_pmu_register() 실패 경로/유닛 해제 → [이 함수]
 */
void free_iommu_pmu(struct intel_iommu *iommu)
{
	struct iommu_pmu *iommu_pmu = iommu->pmu;	/* [한국어] 놓을 구조체 */

	if (!iommu_pmu)	/* [한국어] 지어진 적이 없으면 */
		return;	/* [한국어] 할 일이 없다 */

	if (iommu_pmu->evcap) {	/* [한국어] evcap 이 있으면 cntr_evcap 단계까지 갔다는 뜻 */
		int i;	/* [한국어] 순회 인덱스 */

		for (i = 0; i < iommu_pmu->num_cntr; i++)	/* [한국어] 카운터마다 */
			kfree(iommu_pmu->cntr_evcap[i]);	/* [한국어] 안쪽 배열을 먼저 */
		kfree(iommu_pmu->cntr_evcap);	/* [한국어] 그다음 바깥 배열 */
	}
	kfree(iommu_pmu->evcap);	/* [한국어] 전역 능력 배열 */
	kfree(iommu_pmu);	/* [한국어] 구조체 */
	iommu->pmu = NULL;	/* [한국어] 이후 경로가 해제된 구조체를 건드리지 않게 */
}

/*
 * [한국어]
 * iommu_pmu_set_interrupt - 오버플로 인터럽트를 잡아 핸들러를 건다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -EINVAL 이면 인터럽트를 얻지 못했다.
 *
 * IOMMU 는 폴트용 인터럽트와 별개로 PerfMon 전용 인터럽트를 쓴다.
 * IOMMU_IRQ_ID_OFFSET_PERF 로 오프셋을 두어 유닛마다 폴트/PerfMon 두 개의
 * 인터럽트를 구분한다.
 *
 * IRQF_ONESHOT 으로 threaded IRQ 를 쓰는 이유: 핸들러가 카운터를 읽고
 * 여러 이벤트의 누적값을 갱신하는 동안 같은 인터럽트가 다시 들어오면
 * 상태가 꼬인다. 스레드 핸들러가 끝날 때까지 인터럽트를 막는다.
 *
 * 이름을 "dmar<N>-perf" 로 짓는 이유: /proc/interrupts 에서 어느 유닛의
 * 어떤 인터럽트인지 바로 알아볼 수 있게.
 *
 * 실패하면 잡은 것을 즉시 되돌리고 perf_irq 를 0 으로 되돌려, unset 이
 * 나중에 없는 인터럽트를 해제하려 하지 않게 한다.
 *
 * 호출 체인:
 *   iommu_pmu_register() → [이 함수] → dmar_alloc_hwirq()
 */
static int iommu_pmu_set_interrupt(struct intel_iommu *iommu)
{
	struct iommu_pmu *iommu_pmu = iommu->pmu;	/* [한국어] 이름 버퍼가 여기 있다 */
	int irq, ret;	/* [한국어] 인터럽트 번호와 등록 결과 */

	irq = dmar_alloc_hwirq(IOMMU_IRQ_ID_OFFSET_PERF + iommu->seq_id, iommu->node, iommu);	/* [한국어] 폴트 인터럽트와 구분되도록 오프셋을 두고 잡는다 */
	if (irq <= 0)	/* [한국어] 얻지 못했다 */
		return -EINVAL;	/* [한국어] PMU 를 등록해도 오버플로를 놓치므로 실패로 본다 */

	snprintf(iommu_pmu->irq_name, sizeof(iommu_pmu->irq_name), "dmar%d-perf", iommu->seq_id);	/* [한국어] /proc/interrupts 에서 알아볼 수 있는 이름 */

	iommu->perf_irq = irq;	/* [한국어] 유닛에 기록. 해제 경로가 이 값을 본다 */
	ret = request_threaded_irq(irq, NULL, iommu_pmu_irq_handler,	/* [한국어] 상단 핸들러 없이 스레드 핸들러만 — 카운터 갱신이 짧지 않다 */
				   IRQF_ONESHOT, iommu_pmu->irq_name, iommu);	/* [한국어] 스레드가 끝날 때까지 같은 인터럽트를 막아 상태가 꼬이지 않게 */
	if (ret) {	/* [한국어] 등록 실패 */
		dmar_free_hwirq(irq);	/* [한국어] 잡은 인터럽트를 되돌리고 */
		iommu->perf_irq = 0;	/* [한국어] 기록도 지운다 — unset 이 없는 것을 해제하지 않게 */
		return ret;	/* [한국어] 실패 보고 */
	}
	return 0;	/* [한국어] 인터럽트 준비 완료 */
}

/*
 * [한국어]
 * iommu_pmu_unset_interrupt - 오버플로 인터럽트를 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * free_irq 가 진행 중인 핸들러가 끝날 때까지 기다려 주므로, 이 호출이
 * 반환된 뒤에는 핸들러가 PMU 구조체를 건드리지 않는다. 그래서 이 순서를
 * 지켜야 free_iommu_pmu 가 안전하다.
 *
 * perf_irq 를 0 으로 되돌려 두 번 해제하는 일을 막는다.
 *
 * 호출 체인:
 *   iommu_pmu_unregister() → [이 함수] → free_irq()
 */
static void iommu_pmu_unset_interrupt(struct intel_iommu *iommu)
{
	if (!iommu->perf_irq)	/* [한국어] 잡은 적이 없으면 */
		return;	/* [한국어] 할 일이 없다 */

	free_irq(iommu->perf_irq, iommu);	/* [한국어] 진행 중인 핸들러가 끝날 때까지 기다려 준다 */
	dmar_free_hwirq(iommu->perf_irq);	/* [한국어] 인터럽트 번호를 반납 */
	iommu->perf_irq = 0;	/* [한국어] 두 번 해제하지 않게 */
}

/*
 * [한국어]
 * iommu_pmu_register - 유닛의 PMU 를 perf 에 등록하고 인터럽트까지 건다
 *
 * @iommu: 대상 유닛.
 *
 * alloc_iommu_pmu 가 구조체를 지어 두었다면 그것을 실제로 쓸 수 있게
 * 만드는 마지막 단계다. 반환값이 void 인 이유: PMU 등록 실패는 성능
 * 측정 기능이 없어질 뿐 IOMMU 동작에는 아무 영향이 없다. 그래서 실패해도
 * 부팅을 막지 않고 경고만 남기며 자원을 되돌린다.
 *
 * 되감기 순서: 인터럽트 등록에 실패하면 이미 등록한 PMU 를 먼저 떼고
 * (perf_pmu_unregister) 구조체를 놓는다. 순서를 바꾸면 perf 코어가 해제된
 * 구조체를 참조하는 창이 생긴다.
 *
 * 호출 체인:
 *   유닛 초기화 → [이 함수] → __iommu_pmu_register() → iommu_pmu_set_interrupt()
 */
void iommu_pmu_register(struct intel_iommu *iommu)
{
	struct iommu_pmu *iommu_pmu = iommu->pmu;	/* [한국어] alloc 이 지어 둔 구조체 */

	if (!iommu_pmu)	/* [한국어] PerfMon 이 없는 유닛이면 */
		return;	/* [한국어] 등록할 것이 없다 */

	if (__iommu_pmu_register(iommu))	/* [한국어] perf 코어에 등록 */
		goto err;	/* [한국어] 실패하면 구조체를 놓고 끝낸다 */

	/* Set interrupt for overflow */
	if (iommu_pmu_set_interrupt(iommu))	/* [한국어] 오버플로 인터럽트를 건다 */
		goto unregister;	/* [한국어] 실패하면 등록부터 되돌린다 */

	return;	/* [한국어] 성공 — 이제 perf 로 측정할 수 있다 */

unregister:	/* [한국어] PMU 등록까지 성공한 뒤 인터럽트에서 실패한 경우 */
	perf_pmu_unregister(&iommu_pmu->pmu);	/* [한국어] 코어가 해제된 구조체를 참조하지 않도록 먼저 뗀다 */
err:	/* [한국어] 등록 자체가 실패한 경우도 여기로 모인다 */
	pr_err("Failed to register PMU for iommu (seq_id = %d)\n", iommu->seq_id);	/* [한국어] 실패해도 IOMMU 동작에는 영향이 없어 경고만 남긴다 */
	free_iommu_pmu(iommu);	/* [한국어] 구조체를 놓고 iommu->pmu 를 NULL 로 되돌린다 */
}

/*
 * [한국어]
 * iommu_pmu_unregister - PMU 를 perf 에서 떼고 인터럽트를 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * 순서가 중요하다. 인터럽트를 먼저 놓아 핸들러가 더 이상 실행되지 않게
 * 한 뒤에 PMU 를 뗀다. 반대로 하면 등록이 풀린 PMU 를 핸들러가 건드릴 수
 * 있다.
 *
 * 구조체 자체는 여기서 놓지 않는다 — free_iommu_pmu 가 따로 맡아, 등록
 * 여부와 할당 여부를 독립적으로 다룰 수 있게 되어 있다.
 *
 * 호출 체인:
 *   유닛 해제/핫플러그 제거 → [이 함수] → iommu_pmu_unset_interrupt()
 */
void iommu_pmu_unregister(struct intel_iommu *iommu)
{
	struct iommu_pmu *iommu_pmu = iommu->pmu;	/* [한국어] 대상 PMU */

	if (!iommu_pmu)	/* [한국어] 없으면 */
		return;	/* [한국어] 할 일이 없다 */

	iommu_pmu_unset_interrupt(iommu);	/* [한국어] 먼저 핸들러가 더 실행되지 않게 한 뒤 */
	perf_pmu_unregister(&iommu_pmu->pmu);	/* [한국어] PMU 를 뗀다. 순서를 바꾸면 핸들러가 등록 풀린 PMU 를 건드린다 */
}
