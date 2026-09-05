/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Intel IOMMU trace support
 *
 * Copyright (C) 2019 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 */
/*
 * [한국어 설명] VT-d 추적점 정의 — 무효화와 페이지 요청을 ftrace 로 관찰한다 (intel/trace.h)
 *
 * === 파일의 역할 ===
 * VT-d 드라이버가 남기는 ftrace 이벤트를 정의한다. 커널을 다시 빌드하지
 * 않고도 "무효화가 언제 어디로 몇 번 나갔는가", "페이지 요청이 어떤
 * 내용이었는가" 를 실행 중에 볼 수 있게 하는 것이 목적이다.
 * 네 종류가 있다.
 *   qi_submit         — 무효화 서술자 하나를 큐에 넣을 때.
 *   prq_report        — 페이지 요청 하나를 코어에 넘길 때.
 *   cache_tag_assign/unassign — 무효화 대상 목록이 바뀔 때.
 *   cache_tag_flush_range/_np — 실제로 무효화를 보낼 때.
 * 앞의 둘이 하드웨어와 주고받는 것이라면, 뒤의 둘은 cache.c 의 정규화가
 * 의도대로 동작하는지 — 같은 유닛의 무효화가 한 번으로 합쳐졌는지, 정렬
 * 때문에 범위가 얼마나 넓어졌는지 — 를 보기 위한 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 관측 계층이며 동작에는 관여하지 않는다. 추적점은 켜지지 않은 동안 거의
 * 비용이 없는(static key 로 분기 자체가 사라지는) 구조라, 뜨거운 경로에
 * 두어도 부담이 되지 않는다.
 * 이 헤더가 선언을, trace.c 가 정의를 만든다 — CREATE_TRACE_POINTS 를
 * 정의한 파일 하나에서만 실체가 생기는 커널 관용구다.
 *
 * === 타 모듈과의 연결 ===
 * - trace.c: CREATE_TRACE_POINTS 와 함께 이 헤더를 포함해 실체를 만든다.
 * - dmar.c: qi_submit 을 호출한다.
 * - prq.c: prq_report 를 호출하며, iommu.h 의 decode_prq_descriptor 로
 *   서술자를 사람이 읽을 문자열로 푼다.
 * - cache.c: cache_tag_* 넷을 호출한다.
 * - iommu.h: struct cache_tag 와 QI_*_TYPE 상수를 여기서 참조한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - TRACE_EVENT(qi_submit): 서술자 네 워드를 그대로 남기고, 명령 종류만
 *   __print_symbolic 으로 이름을 붙인다.
 * - TRACE_EVENT(prq_report): 요청 네 워드와 순번. 출력할 때
 *   decode_prq_descriptor 가 필드를 풀어 준다.
 * - DECLARE_EVENT_CLASS(cache_tag_log): assign 과 unassign 이 같은 형식이라
 *   클래스로 묶고 DEFINE_EVENT 로 둘을 만든다.
 * - DECLARE_EVENT_CLASS(cache_tag_flush): flush_range 와 range_np 도 마찬가지.
 *   요청 범위(start~end)와 실제 무효화 범위(addr/mask)를 함께 남기는 것이
 *   핵심이다 — 정렬 때문에 얼마나 넓어졌는지 볼 수 있다.
 * - 파일 끝의 TRACE_INCLUDE_* : 이 헤더를 다시 포함해 정의를 전개하도록
 *   ftrace 인프라에 경로를 알려 주는 관용구다.
 */
#undef TRACE_SYSTEM	/* [한국어] 이전 헤더가 정의했을 수 있어 먼저 지운다 */
#define TRACE_SYSTEM intel_iommu	/* [한국어] 추적 이벤트가 묶일 이름. /sys/kernel/tracing/events/intel_iommu/ 아래에 나타난다 */

#if !defined(_TRACE_INTEL_IOMMU_H) || defined(TRACE_HEADER_MULTI_READ)	/* [한국어] 보통의 중복 포함 방지에 예외가 하나 있다 — ftrace 인프라가 이 헤더를 여러 번 읽어 각기 다른 코드를 전개하기 때문이다 */
#define _TRACE_INTEL_IOMMU_H	/* [한국어] 한 번 읽었다는 표시 */

#include <linux/tracepoint.h>	/* [한국어] TRACE_EVENT 매크로들 */

#include "iommu.h"	/* [한국어] struct cache_tag, QI_*_TYPE 상수, decode_prq_descriptor */

#define MSG_MAX		256	/* [한국어] 페이지 요청을 풀어 쓸 문자열 버퍼의 크기 */

TRACE_EVENT(qi_submit,	/* [한국어] 무효화 서술자를 큐에 넣을 때 남기는 이벤트 */
	TP_PROTO(struct intel_iommu *iommu, u64 qw0, u64 qw1, u64 qw2, u64 qw3),	/* [한국어] 호출부가 넘길 인자들 — 유닛과 서술자 네 워드 */

	TP_ARGS(iommu, qw0, qw1, qw2, qw3),	/* [한국어] 그 인자 이름들 */

	TP_STRUCT__entry(	/* [한국어] 링 버퍼에 저장할 항목의 배치 */
		__field(u64, qw0)	/* [한국어] 서술자 첫 워드 */
		__field(u64, qw1)	/* [한국어] 둘째 워드 */
		__field(u64, qw2)	/* [한국어] 셋째 워드 */
		__field(u64, qw3)	/* [한국어] 넷째 워드 */
		__string(iommu, iommu->name)	/* [한국어] 유닛 이름. 포인터가 아니라 문자열을 복사한다 — 나중에 출력할 때 유닛이 살아 있다는 보장이 없다 */
	),

	TP_fast_assign(	/* [한국어] 이벤트가 켜져 있을 때만 실행되는 복사 코드 */
		__assign_str(iommu);	/* [한국어] 유닛 이름 복사 */
		__entry->qw0 = qw0;	/* [한국어] 네 워드를 그대로 저장한다. 해석하지 않고 원본을 남기는 편이 사후 분석에 낫다 */
		__entry->qw1 = qw1;	/* [한국어] 둘째 */
		__entry->qw2 = qw2;	/* [한국어] 셋째 */
		__entry->qw3 = qw3;	/* [한국어] 넷째 */
	),	/* [한국어] 출력 형식의 끝 */

	TP_printk("%s %s: 0x%llx 0x%llx 0x%llx 0x%llx",	/* [한국어] 출력 형식. 저장할 때가 아니라 읽을 때 실행되므로 비용이 뜨거운 경로에 들지 않는다 */
		  __print_symbolic(__entry->qw0 & 0xf,	/* [한국어] 첫 워드의 하위 4비트가 명령 종류다. 숫자 대신 이름으로 보여 준다 */
				   { QI_CC_TYPE,	"cc_inv" },	/* [한국어] 컨텍스트 캐시 무효화 */
				   { QI_IOTLB_TYPE,	"iotlb_inv" },	/* [한국어] IOTLB 무효화 */
				   { QI_DIOTLB_TYPE,	"dev_tlb_inv" },	/* [한국어] 디바이스 TLB 무효화 */
				   { QI_IEC_TYPE,	"iec_inv" },	/* [한국어] 인터럽트 항목 캐시 무효화 */
				   { QI_IWD_TYPE,	"inv_wait" },	/* [한국어] 완료 대기 표식 */
				   { QI_EIOTLB_TYPE,	"p_iotlb_inv" },	/* [한국어] PASID 인식 IOTLB 무효화 */
				   { QI_PC_TYPE,	"pc_inv" },	/* [한국어] PASID 캐시 무효화 */
				   { QI_DEIOTLB_TYPE,	"p_dev_tlb_inv" },	/* [한국어] PASID 인식 디바이스 TLB 무효화 */
				   { QI_PGRP_RESP_TYPE,	"page_grp_resp" }),	/* [한국어] 페이지 요청 그룹 응답 */
		__get_str(iommu),	/* [한국어] 유닛 이름 */
		__entry->qw0, __entry->qw1, __entry->qw2, __entry->qw3	/* [한국어] 네 워드를 16진수로 */
	)
);

TRACE_EVENT(prq_report,	/* [한국어] 페이지 요청 하나를 코어에 넘길 때 남기는 이벤트 */
	TP_PROTO(struct intel_iommu *iommu, struct device *dev,	/* [한국어] 호출부가 넘길 인자들 — 유닛, 장치, */
		 u64 dw0, u64 dw1, u64 dw2, u64 dw3,	/* [한국어] 요청 서술자 네 워드, */
		 unsigned long seq),	/* [한국어] 그리고 누적 순번 */

	TP_ARGS(iommu, dev, dw0, dw1, dw2, dw3, seq),	/* [한국어] 그 인자 이름들 */

	TP_STRUCT__entry(
		__field(u64, dw0)	/* [한국어] 요청 첫 워드 */
		__field(u64, dw1)	/* [한국어] 둘째 워드 */
		__field(u64, dw2)	/* [한국어] 셋째 워드 */
		__field(u64, dw3)	/* [한국어] 넷째 워드 */
		__field(unsigned long, seq)	/* [한국어] 누적 순번 */
		__string(iommu, iommu->name)
		__string(dev, dev_name(dev))	/* [한국어] 장치 이름을 복사해 둔다 */
		__dynamic_array(char, buff, MSG_MAX)	/* [한국어] 출력할 때 decode_prq_descriptor 가 쓸 임시 버퍼. 저장 시점이 아니라 읽을 때 채워진다 */
	),

	TP_fast_assign(
		__entry->dw0 = dw0;	/* [한국어] 네 워드를 그대로 저장한다 */
		__entry->dw1 = dw1;	/* [한국어] 요청 둘째 워드 */
		__entry->dw2 = dw2;	/* [한국어] 셋째 워드 */
		__entry->dw3 = dw3;	/* [한국어] 넷째 워드 — 사설 데이터가 들어 있을 수 있어 해석하지 않고 그대로 남긴다 */
		__entry->seq = seq;	/* [한국어] 누적 순번. 요청의 순서와 유실 여부를 사후에 확인할 수 있다 */
		__assign_str(iommu);	/* [한국어] 유닛 이름 복사 */
		__assign_str(dev);	/* [한국어] 장치 이름 복사. 포인터로 두면 출력 시점에 장치가 사라졌을 수 있다 */
	),	/* [한국어] 복사 코드의 끝 */

	TP_printk("%s/%s seq# %ld: %s",	/* [한국어] 유닛/장치, 순번, 그리고 풀어 쓴 요청 내용 */
		__get_str(iommu), __get_str(dev), __entry->seq,	/* [한국어] 복사해 둔 이름들과 순번 */
		decode_prq_descriptor(__get_str(buff), MSG_MAX, __entry->dw0,	/* [한국어] iommu.h 의 헬퍼가 비트필드를 사람이 읽을 문자열로 푼다 */
				      __entry->dw1, __entry->dw2, __entry->dw3)	/* [한국어] 나머지 세 워드 */
	)
);

DECLARE_EVENT_CLASS(cache_tag_log,	/* [한국어] 무효화 대상 목록이 바뀔 때의 이벤트 형식. assign 과 unassign 이 같은 형식이라 클래스로 묶는다 */
	TP_PROTO(struct cache_tag *tag),	/* [한국어] 태그 하나만 넘긴다 — 필요한 것이 모두 그 안에 있다 */
	TP_ARGS(tag),	/* [한국어] 그 인자 */
	TP_STRUCT__entry(
		__string(iommu, tag->iommu->name)	/* [한국어] 유닛 이름 복사 */
		__string(dev, dev_name(tag->dev))	/* [한국어] 장치 이름 복사 */
		__field(u16, type)	/* [한국어] 태그 종류 */
		__field(u16, domain_id)	/* [한국어] 도메인 id */
		__field(u32, pasid)	/* [한국어] 대상 PASID */
		__field(u32, users)	/* [한국어] 참조 수 */
	),
	TP_fast_assign(
		__assign_str(iommu);
		__assign_str(dev);	/* [한국어] 장치 이름 복사 */
		__entry->type = tag->type;	/* [한국어] 태그 종류(IOTLB/DEVTLB/중첩판) */
		__entry->domain_id = tag->domain_id;	/* [한국어] 그 유닛에서의 도메인 id */
		__entry->pasid = tag->pasid;	/* [한국어] 대상 PASID */
		__entry->users = tag->users;	/* [한국어] 참조 수. 이 값이 늘어나는 것을 보면 중복 제거가 동작하고 있다는 뜻이다 */
	),	/* [한국어] 복사 코드의 끝 */
	TP_printk("%s/%s type %s did %d pasid %d ref %d",	/* [한국어] 유닛/장치, 종류, 도메인 id, PASID, 참조 수 */
		  __get_str(iommu), __get_str(dev),	/* [한국어] 복사해 둔 이름들 */
		  __print_symbolic(__entry->type,	/* [한국어] 태그 종류를 숫자 대신 이름으로 */
			{ CACHE_TAG_IOTLB,		"iotlb" },	/* [한국어] 유닛 안의 캐시 */
			{ CACHE_TAG_DEVTLB,		"devtlb" },	/* [한국어] 장치 안의 캐시 */
			{ CACHE_TAG_NESTING_IOTLB,	"nesting_iotlb" },	/* [한국어] 중첩 부모 쪽 유닛 캐시 */
			{ CACHE_TAG_NESTING_DEVTLB,	"nesting_devtlb" }),	/* [한국어] 중첩용 장치 캐시 */
		__entry->domain_id, __entry->pasid, __entry->users	/* [한국어] 나머지 값들 */
	)
);

DEFINE_EVENT(cache_tag_log, cache_tag_assign,	/* [한국어] 태그를 등록할 때 */
	TP_PROTO(struct cache_tag *tag),	/* [한국어] 태그 하나만 넘긴다 — 필요한 것이 모두 그 안에 있다 */
	TP_ARGS(tag)	/* [한국어] 클래스의 인자를 그대로 */
);

DEFINE_EVENT(cache_tag_log, cache_tag_unassign,	/* [한국어] 놓을 때. 같은 클래스에서 이름만 다른 두 이벤트를 만든다 */
	TP_PROTO(struct cache_tag *tag),	/* [한국어] 태그 하나만 넘긴다 — 필요한 것이 모두 그 안에 있다 */
	TP_ARGS(tag)	/* [한국어] 클래스의 인자를 그대로 */
);

DECLARE_EVENT_CLASS(cache_tag_flush,	/* [한국어] 실제로 무효화를 보낼 때의 이벤트 형식. flush_range 와 range_np 가 공유한다 */
	TP_PROTO(struct cache_tag *tag, unsigned long start, unsigned long end,	/* [한국어] 태그와 요청받은 범위, */
		 unsigned long addr, unsigned long mask),	/* [한국어] 그리고 정렬을 맞춘 실제 범위 */
	TP_ARGS(tag, start, end, addr, mask),	/* [한국어] 그 인자들 */
	TP_STRUCT__entry(
		__string(iommu, tag->iommu->name)	/* [한국어] 유닛 이름 복사 */
		__string(dev, dev_name(tag->dev))	/* [한국어] 장치 이름 복사 */
		__field(u16, type)	/* [한국어] 태그 종류 */
		__field(u16, domain_id)	/* [한국어] 도메인 id */
		__field(u32, pasid)	/* [한국어] 대상 PASID */
		__field(unsigned long, start)	/* [한국어] 요청받은 범위의 시작 */
		__field(unsigned long, end)	/* [한국어] 그 끝 */
		__field(unsigned long, addr)	/* [한국어] 실제 무효화의 시작 주소 */
		__field(unsigned long, mask)	/* [한국어] 그 범위 크기 */
	),
	TP_fast_assign(
		__assign_str(iommu);
		__assign_str(dev);	/* [한국어] 장치 이름 복사 */
		__entry->type = tag->type;	/* [한국어] 태그 종류 */
		__entry->domain_id = tag->domain_id;	/* [한국어] 도메인 id */
		__entry->pasid = tag->pasid;	/* [한국어] 대상 PASID */
		__entry->start = start;	/* [한국어] 요청받은 범위의 시작 */
		__entry->end = end;	/* [한국어] 그 끝 */
		__entry->addr = addr;	/* [한국어] 정렬을 맞춘 뒤의 실제 시작 주소 */
		__entry->mask = mask;	/* [한국어] 실제 무효화 범위의 크기. start~end 와 addr/mask 를 함께 남기는 것이 이 이벤트의 핵심이다 — 정렬 때문에 범위가 얼마나 넓어졌는지 볼 수 있다 */
	),	/* [한국어] 복사 코드의 끝 */
	TP_printk("%s %s[%d] type %s did %d [0x%lx-0x%lx] addr 0x%lx mask 0x%lx",	/* [한국어] 요청 범위와 실제 범위를 나란히 보여 준다 */
		  __get_str(iommu), __get_str(dev), __entry->pasid,	/* [한국어] 유닛/장치/PASID */
		  __print_symbolic(__entry->type,	/* [한국어] 태그 종류를 숫자 대신 이름으로 */
			{ CACHE_TAG_IOTLB,		"iotlb" },	/* [한국어] 유닛 안의 캐시 */
			{ CACHE_TAG_DEVTLB,		"devtlb" },	/* [한국어] 장치 안의 캐시 */
			{ CACHE_TAG_NESTING_IOTLB,	"nesting_iotlb" },	/* [한국어] 중첩 부모 쪽 유닛 캐시 */
			{ CACHE_TAG_NESTING_DEVTLB,	"nesting_devtlb" }),	/* [한국어] 중첩용 장치 캐시 */
		__entry->domain_id, __entry->start, __entry->end,	/* [한국어] 도메인 id 와 요청 범위 */
		__entry->addr, __entry->mask	/* [한국어] 실제 무효화 범위 */
	)
);

DEFINE_EVENT(cache_tag_flush, cache_tag_flush_range,	/* [한국어] 매핑을 풀 때의 무효화 */
	TP_PROTO(struct cache_tag *tag, unsigned long start, unsigned long end,	/* [한국어] 태그와 요청받은 범위, */
		 unsigned long addr, unsigned long mask),	/* [한국어] 그리고 정렬을 맞춘 실제 범위 */
	TP_ARGS(tag, start, end, addr, mask)	/* [한국어] 클래스의 인자를 그대로 */
);

DEFINE_EVENT(cache_tag_flush, cache_tag_flush_range_np,	/* [한국어] 새 매핑을 보이게 하는 무효화. 같은 클래스에서 두 이벤트를 만든다 */
	TP_PROTO(struct cache_tag *tag, unsigned long start, unsigned long end,	/* [한국어] 태그와 요청받은 범위, */
		 unsigned long addr, unsigned long mask),	/* [한국어] 그리고 정렬을 맞춘 실제 범위 */
	TP_ARGS(tag, start, end, addr, mask)	/* [한국어] 클래스의 인자를 그대로 */
);
#endif /* _TRACE_INTEL_IOMMU_H */	/* [한국어] 중복 포함 방지의 끝 */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH	/* [한국어] 아래는 보호 구간 밖이어야 한다 (위 영어 주석) — ftrace 인프라가 이 헤더를 다시 읽어 정의를 전개하기 때문이다 */
#undef TRACE_INCLUDE_FILE	/* [한국어] 이전 값을 지운다 */
#define TRACE_INCLUDE_PATH ../../drivers/iommu/intel/	/* [한국어] 이 헤더가 있는 경로. include/trace 기준의 상대 경로다 */
#define TRACE_INCLUDE_FILE trace	/* [한국어] 파일 이름(확장자 없이) */
#include <trace/define_trace.h>	/* [한국어] 이 포함이 위에 지정한 경로로 이 헤더를 다시 읽어, TRACE_EVENT 들을 실제 정의로 전개한다 */
