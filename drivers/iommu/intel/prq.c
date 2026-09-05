// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Intel Corporation
 *
 * Originally split from drivers/iommu/intel/svm.c
 */

/*
 * [한국어 설명] 페이지 요청 큐(PRQ) — 장치가 낸 페이지 폴트를 받아 처리한다 (intel/prq.c)
 *
 * === 파일의 역할 ===
 * SVA 처럼 매핑을 미리 다 만들어 두지 않는 방식에서는, 장치가 아직 매핑되지
 * 않은 주소에 접근하는 일이 정상적으로 일어난다. 그때 하드웨어는 그 접근을
 * 실패시키지 않고 "이 주소를 매핑해 달라"는 요청(PCIe PRI)을 큐에 넣고,
 * 커널이 페이지를 채운 뒤 "다시 시도하라"고 답해 준다. 그 큐가 PRQ 이고,
 * 이 파일이 그 양쪽 끝을 다룬다.
 * 무효화 큐(QI)와 방향이 반대라는 점이 중요하다 — QI 는 커널이 채우고
 * 하드웨어가 소비하지만, PRQ 는 하드웨어가 채우고 커널이 소비한다.
 *
 * 이 파일이 지는 가장 무거운 책임은 "모든 요청에 정확히 한 번 답한다"이다.
 * 응답하지 않은 페이지 요청은 장치를 영원히 멈춰 세우기 때문이다. 그래서
 * 잘못된 요청도 그냥 버리지 않고 거절 응답을 보내고(handle_bad_prq_event),
 * PASID 를 내릴 때는 남은 요청을 반드시 배수한다(drain_pasid_prq).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 폴트 처리의 하드웨어 쪽 끝이다.
 *   장치 → PRI 요청 → [PRQ 링 버퍼] → 이 파일의 인터럽트 스레드
 *   → iommu_report_device_fault() → 코어의 io-pgfault 계층
 *   → SVA 라면 프로세스의 VMA 를 확인하고 handle_mm_fault()
 *   → 응답 → [이 파일의 page_response] → 장치가 재시도
 * 위쪽으로는 drivers/iommu/io-pgfault.c 가 요청을 그룹으로 묶어 처리하고,
 * 아래쪽으로는 dmar.c 의 qi_submit_sync 로 응답 서술자를 보낸다.
 * 실행 컨텍스트: prq_event_thread 는 threaded IRQ 핸들러라 잠들 수 있다 —
 * 그래서 iopf_lock(뮤텍스)을 잡을 수 있고, 그 안에서 소스 id 로 장치를 찾는다.
 *
 * === 타 모듈과의 연결 ===
 * - io-pgfault.c: iommu_report_device_fault() 로 요청을 넘기면 그쪽이 그룹을
 *   모아 드라이버나 SVA 핸들러에 전달한다.
 * - svm.c: SVA 도메인이 이 경로로 폴트를 받는다.
 * - pasid.c: PASID 를 내릴 때 intel_iommu_drain_pasid_prq() 를 불러 남은
 *   요청이 없음을 보장한다.
 * - iommu.c: device_rbtree_find() 로 소스 id 에서 장치를 되찾고, iopf_lock 이
 *   장치 해제와의 경쟁을 막는다.
 * - dmar.c: qi_submit_sync() 로 응답과 배수 서술자를 보낸다.
 * 데이터 흐름: 장치가 폴트 → 하드웨어가 PRQ 에 서술자를 넣고 인터럽트 →
 * 이 파일이 검증하고 장치를 찾아 코어에 보고 → 코어가 페이지를 채움 →
 * intel_iommu_page_response() 가 응답 서술자를 큐에 넣음 → 장치가 재시도.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct page_req_dsc: 하드웨어가 큐에 넣는 요청 하나. 비트필드와 u64 를
 *   union 으로 겹쳐, 필드로도 워드로도 읽을 수 있게 했다.
 * - prq_event_thread(): 인터럽트 스레드. 큐를 훑으며 검증하고 코어에 보고한다.
 *   이 파일의 중심이다.
 * - intel_iommu_drain_pasid_prq(): PASID 를 내리기 전에 그 PASID 의 요청을
 *   소프트웨어와 하드웨어 양쪽에서 모두 배수한다. 스펙 7.10 의 절차다.
 * - handle_bad_prq_event(): 잘못된 요청에 거절 응답을 보낸다. 버리지 않는
 *   이유는 응답 없는 요청이 장치를 멈춰 세우기 때문이다.
 * - intel_iommu_page_response(): 코어가 정한 응답을 하드웨어로 보낸다.
 * - intel_iommu_enable_prq()/finish_prq(): 큐와 인터럽트의 수명.
 */
#include <linux/pci.h>	/* [한국어] 소스 id 와 PCI 능력 */
#include <linux/pci-ats.h>	/* [한국어] ATS/PRI 능력 */

#include "iommu.h"	/* [한국어] 유닛 자료구조, 레지스터 정의, 응답 서술자 매크로 */
#include "pasid.h"	/* [한국어] PASID 항목 형식 */
#include "../iommu-pages.h"	/* [한국어] 큐 버퍼를 잡는 공용 할당기 */
#include "trace.h"	/* [한국어] 요청 하나하나를 추적 이벤트로 남긴다 */

/* Page request queue descriptor */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct page_req_dsc — 하드웨어가 PRQ 에 넣는 페이지 요청 하나
 *
 * 32바이트(u64 네 개)이며, 앞 두 워드가 비트필드와 u64 의 union 으로 겹쳐
 * 있다. 필드 하나하나를 이름으로 읽을 수도 있고(검증과 해석), 워드 통째로
 * 읽을 수도 있다(로그와 추적 이벤트). 하드웨어가 채우는 자료구조라 비트
 * 배치가 스펙에 고정되어 있고, 커널은 읽기만 한다.
 *
 * 담긴 정보가 무엇을 결정하는지:
 *   rid           — 요청을 낸 장치의 소스 id. device_rbtree_find 로 struct
 *                   device 를 되찾는 유일한 단서다.
 *   pasid_present/pasid — 어느 주소 공간인지. PASID 가 없으면 그 장치의
 *                   기본 주소 공간(RID2PASID)이다.
 *   addr          — 접근하려던 페이지 주소(하위 12비트는 없다).
 *   rd_req/wr_req/exe_req/pm_req — 요청한 권한. SVA 에서는 이 값과 VMA 의
 *                   권한을 대조하는 것이 실질적인 보안 경계다.
 *   lpig          — Last Page In Group. 이 요청이 그룹의 마지막이라는 표시로,
 *                   응답은 그룹 단위로 하므로 이 비트가 와야 답할 수 있다.
 *   prg_index     — 그룹 번호. 응답에 그대로 실어야 장치가 짝을 찾는다.
 *
 * 응답이 필요 없는 예외가 하나 있다: lpig 만 서 있고 읽기·쓰기 요청이 없는
 * 것은 "Stop Marker" 로, 장치가 그 그룹을 스스로 포기했다는 통보다.
 */
struct page_req_dsc {
	union {
	/* [한국어] 서술자의 첫 워드를 비트필드로도, 통째의 u64 로도 볼 수 있게 겹친 것.
	 * 왜 둘 다 필요한가: 요청을 해석할 때는 필드 단위가 편하지만, 잘못된 요청을
	 *   로그에 남기거나 추적 이벤트에 넘길 때는 원본 워드를 그대로 두는 편이
	 *   사후 분석에 낫다. 필드를 하나씩 찍으면 드라이버가 해석한 결과만 남고,
	 *   하드웨어가 실제로 무엇을 썼는지는 사라진다.
	 * 설정자: 하드웨어가 PRQ 큐에 이 서술자를 쓴다.
	 * 읽는 자: prq_event_thread() 가 필드로 읽고, 오류 경로가 워드로 읽는다. */
		struct {
		/* [한국어] 첫 워드의 필드 배치 — VT-d 규격이 정한 순서 그대로다.
		 * 읽는 자: 요청을 해석하는 코드.
		 * 비트필드 순서를 바꿀 수 없는 이유: 이 구조체는 하드웨어가 메모리에 쓴
		 *   바이트를 그대로 덮어 보는 것이다. 순서가 어긋나면 조용히 엉뚱한 값을
		 *   읽으며, 그 오류는 컴파일러가 잡아 주지 않는다.
		 * 아래 두 번째 union 과 합쳐 서술자의 앞 두 워드를 이룬다. */
			u64 type:8;
			/* [한국어] 서술자의 종류. PRQ 에 들어오는 것은 페이지 요청뿐이라 실질적으로는
			 * 고정값이며, 커널은 이 필드를 검사하지 않는다.
			 * 설정자: 하드웨어. 읽는 자: 없음(로그에 워드 통째로 찍힐 때만). */
			u64 pasid_present:1;
			/* [한국어] 아래 pasid 필드가 유효한지.
			 * 설정자: 하드웨어. 요청에 PASID 가 실려 있었으면 1.
			 * 읽는 자: 요청을 코어에 보고할 때 PASID_VALID 플래그로 옮기고, 배수 경로가
			 *   "이 요청이 내가 찾는 PASID 의 것인가"를 판단할 때도 본다.
			 * 값 범위: 0 이면 그 장치의 기본 주소 공간(RID2PASID)에 대한 요청이다. */
			u64 rsvd:7;
			/* [한국어] 예약 필드. 하드웨어가 0 으로 채우며 커널은 읽지 않는다.
			 * 비트필드로 자리를 잡아 두어야 뒤의 필드들이 올바른 위치에 온다. */
			u64 rid:16;
			/* [한국어] 요청을 낸 장치의 소스 id(버스 8비트 + devfn 8비트).
			 * 설정자: 하드웨어.
			 * 읽는 자: prq_event_thread 가 device_rbtree_find 로 struct device 를
			 *   되찾는 유일한 단서다. 배수 경로도 이 값으로 대상 요청을 골라낸다.
			 * 값 범위: 트리에 없는 id 면 이미 해제된 장치이거나 설정 오류이므로,
			 *   거절 응답을 보내고 넘어간다. */
			u64 pasid:20;
			/* [한국어] 요청한 주소 공간의 PASID. pasid_present 가 1 일 때만 유효하다.
			 * 설정자: 하드웨어.
			 * 읽는 자: 코어에 보고할 때, 그리고 응답 서술자에 그대로 실린다.
			 * 값 범위: 20비트. SVA 에서는 이 값이 어느 프로세스의 주소 공간인지를 가른다. */
			u64 exe_req:1;
			/* [한국어] 실행 권한 요청인지.
			 * 설정자: 하드웨어. 읽는 자: prq_to_iommu_prot 이 IOMMU_FAULT_PERM_EXEC 로 옮긴다.
			 * 검증: 읽기 요청과 함께 오면 잘못된 조합이라 거절한다 — 실행 권한 요청은
			 *   단독으로만 의미가 있다. */
			u64 pm_req:1;
			/* [한국어] 특권(privileged) 모드 요청인지.
			 * 설정자: 하드웨어. 읽는 자: prq_to_iommu_prot 이 IOMMU_FAULT_PERM_PRIV 로 옮긴다.
			 * 검증: 읽기나 쓰기 요청과 함께 오면 거절한다. 특권 요청은 SRE 를 켠 항목에서만
			 *   성립하는데, 그 조합을 이 드라이버는 지원하지 않는다. */
			u64 rsvd2:10;
			/* [한국어] 예약 필드. 위 rsvd 와 같은 이유로 자리만 잡는다. */
		};
		u64 qw_0;
		/* [한국어] 위 비트필드들과 같은 메모리를 덮는 u64.
		 * 읽는 자: 잘못된 요청을 로그에 찍을 때와 추적 이벤트에 넘길 때. 필드를
		 *   하나씩 찍는 것보다 원본 워드를 남기는 편이 사후 분석에 낫다. */
	};
	union {
	/* [한국어] 서술자의 둘째 워드를 비트필드와 u64 로 겹쳐 보는 창.
	 * 왜 첫 워드와 따로인가: 서술자가 여러 워드로 나뉘어 있고 각 워드가 독립된
	 *   필드 묶음이다. 하나의 큰 비트필드로 묶으면 64비트 경계를 넘는 필드가
	 *   생겨 컴파일러마다 배치가 달라질 위험이 있다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 권한 요청과 그룹 정보를 해석하는 코드, 그리고 로그·추적 경로. */
		struct {
		/* [한국어] 둘째 워드의 필드 배치 — 권한 요청과 그룹 정보가 들어 있다.
		 * 읽는 자: prq_to_iommu_prot() 이 rd_req/wr_req 를 권한으로 옮기고,
		 *   응답을 만드는 코드가 lpig 와 prg_index 를 읽는다.
		 * 이 워드가 폴트 처리의 핵심이다: 어떤 권한을 요청했는지가 SVA 의 실질적인
		 *   보안 경계이고, 그룹 정보가 응답을 어디로 보낼지를 정한다.
		 * 첫 워드와 마찬가지로 필드 순서는 규격이 정한 것이라 바꿀 수 없다. */
			u64 rd_req:1;
			/* [한국어] 읽기 권한 요청인지.
			 * 설정자: 하드웨어. 읽는 자: prq_to_iommu_prot.
			 * SVA 에서는 이 권한과 그 주소의 VMA 권한을 대조하는 것이 실질적인 보안
			 *   경계다 — 장치가 쓰기 권한 없는 페이지에 쓰기를 요청하면 거절된다. */
			u64 wr_req:1;
			/* [한국어] 쓰기 권한 요청인지. rd_req 와 같은 방식으로 쓰인다. */
			u64 lpig:1;
			/* [한국어] Last Page In Group — 이 요청이 그룹의 마지막이라는 표시.
			 * 왜 중요한가: 장치는 여러 페이지를 한 그룹으로 묶어 요청할 수 있고, 응답은
			 *   그룹 단위로 한 번만 한다. 그래서 이 비트가 온 요청에만 응답을 보낸다.
			 *   중간 요청에 응답하면 장치가 혼란스러워하고, 마지막 요청에 응답하지
			 *   않으면 장치가 영원히 기다린다.
			 * 읽는 자: handle_bad_prq_event 가 응답을 보낼지 정할 때, 그리고 코어에
			 *   LAST_PAGE 플래그로 전달할 때.
			 * 특수한 경우: lpig 만 있고 읽기·쓰기 요청이 없으면 Stop Marker 다 —
			 *   장치가 그 그룹을 포기했다는 통보이므로 응답하지 않고 버린다. */
			u64 prg_index:9;
			/* [한국어] 페이지 요청 그룹의 번호.
			 * 설정자: 하드웨어(장치가 정한 값을 그대로 옮긴다).
			 * 읽는 자: 응답 서술자의 QI_PGRP_IDX 에 그대로 실린다. 이 값이 틀리면
			 *   장치가 어느 요청에 대한 답인지 알지 못해, 그 그룹은 영원히 미완으로 남는다. */
			u64 addr:52;
			/* [한국어] 접근하려던 주소의 페이지 프레임 번호(하위 12비트는 없다).
			 * 설정자: 하드웨어.
			 * 읽는 자: << VTD_PAGE_SHIFT 로 바이트 주소를 복원해 정규(canonical) 주소인지
			 *   검사하고, 코어에 보고할 때 그 값을 넘긴다.
			 * 값 범위: 52비트. 정규 주소가 아니면 하드웨어나 장치의 오동작이므로 거절한다. */
		};
		u64 qw_1;
		/* [한국어] 위 둘째 비트필드 묶음과 같은 메모리를 덮는 u64.
		 * 설정자: 하드웨어(비트필드와 같은 자리를 쓴다).
		 * 읽는 자: 잘못된 요청을 로그에 찍을 때와 추적 이벤트에 넘길 때.
		 * qw_0 과 같은 용도이며, 같은 이유로 존재한다 — 드라이버가 해석한 필드가
		 *   아니라 하드웨어가 실제로 쓴 비트를 남겨야 사후 분석이 된다.
		 * 이름의 qw 는 quad word(64비트)를 뜻하며, 숫자는 서술자 안에서의 순서다. */
	};
	u64 qw_2;
	/* [한국어] 셋째 워드. 장치가 실어 보낸 사설 데이터(private data)가 들어갈 수 있다.
	 * 커널은 해석하지 않고 추적 이벤트에 그대로 남긴다 — 그 값의 의미는 장치
	 *   드라이버만 안다. */
	u64 qw_3;
	/* [한국어] 넷째 워드. qw_2 와 같다. */
};

/**
 * intel_iommu_drain_pasid_prq - Drain page requests and responses for a pasid
 * @dev: target device
 * @pasid: pasid for draining
 *
 * Drain all pending page requests and responses related to @pasid in both
 * software and hardware. This is supposed to be called after the device
 * driver has stopped DMA, the pasid entry has been cleared, and both IOTLB
 * and DevTLB have been invalidated.
 *
 * It waits until all pending page requests for @pasid in the page fault
 * queue are completed by the prq handling thread. Then follow the steps
 * described in VT-d spec CH7.10 to drain all page requests and page
 * responses pending in the hardware.
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * intel_iommu_drain_pasid_prq - 이 PASID 의 페이지 요청을 소프트웨어와 하드웨어 양쪽에서 모두 배수한다
 *
 * @dev: 대상 장치. @pasid: 배수할 PASID.
 * @return: 없음(모두 배수될 때까지 기다린 뒤 돌아온다).
 *
 * 왜 필요한가: PASID 항목을 내린 뒤에도 그 PASID 로 온 페이지 요청이
 * 하드웨어 큐나 소프트웨어 큐에 남아 있을 수 있다. 그것을 처리하지 않고
 * 놔두면 두 가지 문제가 생긴다 — 응답을 받지 못한 장치가 영원히 멈추고,
 * 이미 해제된 자료구조(mm, 도메인)를 참조하는 처리가 나중에 실행된다.
 *
 * 호출 전 조건이 kernel-doc 에 명시되어 있다: 드라이버가 DMA 를 멈췄고,
 * PASID 항목이 지워졌고, IOTLB 와 디바이스 TLB 가 무효화된 뒤여야 한다.
 * 그래야 "새로운 요청이 더 들어오지 않는다"가 보장되어 배수가 끝날 수 있다.
 *
 * 두 단계로 배수한다.
 *
 *   [1] 소프트웨어 큐 — PRQ 링을 head~tail 로 훑으며 이 (장치, PASID) 의
 *       요청이 남아 있는지 본다. 있으면 prq_event_thread 가 그것을 처리하고
 *       complete() 를 부를 때까지 기다린 뒤 처음부터 다시 훑는다. 다시
 *       훑는 이유는 기다리는 동안 head/tail 이 움직였기 때문이다.
 *       그 다음 iopf_queue_flush_dev 로 코어 쪽 큐도 비운다.
 *
 *   [2] 하드웨어 큐 — 스펙 7.10 의 절차를 따른다. 서술자 셋을 한 묶음으로
 *       보내는데, 순서가 곧 의미다.
 *         desc[0] — FENCE 를 세운 Invalidation Wait. 이 앞의 것이 모두
 *                   끝나야 뒤가 시작된다는 장벽이다.
 *         desc[1] — IOTLB 무효화(PASID 유무에 따라 형식이 다르다).
 *         desc[2] — 디바이스 TLB 무효화.
 *       QI_OPT_WAIT_DRAIN 옵션이 "대기 중인 페이지 요청까지 배수하라"를
 *       하드웨어에 알린다.
 *       그 뒤 PRS 레지스터의 오버플로 비트를 확인하는데, 켜져 있으면 배수
 *       도중 큐가 넘쳤다는 뜻이라 처음부터 다시 한다.
 *
 * iopf_refcount 가 0 이면 곧바로 돌아간다 — 이 장치는 애초에 페이지 요청을
 * 쓰지 않으므로 배수할 것이 없다.
 *
 * 실행 컨텍스트: PASID 해제. 프로세스 컨텍스트(완료를 기다리므로 잠들 수
 * 있어야 한다).
 *
 * 호출 체인:
 *   intel_pasid_tear_down_entry() → [intel_iommu_drain_pasid_prq]
 *     → iopf_queue_flush_dev() → qi_submit_sync(QI_OPT_WAIT_DRAIN)
 */
void intel_iommu_drain_pasid_prq(struct device *dev, u32 pasid)
{
	struct device_domain_info *info;	/* [한국어] 장치 정보 */
	struct dmar_domain *domain;	/* [한국어] 그 장치가 붙어 있는 도메인 */
	struct intel_iommu *iommu;	/* [한국어] 담당 유닛 */
	struct qi_desc desc[3];	/* [한국어] 배수용 서술자 셋 — 장벽, IOTLB, 디바이스 TLB */
	int head, tail;	/* [한국어] PRQ 링의 소비·생산 지점 */
	u16 sid, did;	/* [한국어] 장치의 소스 id 와 도메인 id */

	info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	if (!info->iopf_refcount)	/* [한국어] 폴트 처리를 쓰지 않는 장치면 */
		return;	/* [한국어] 배수할 요청도 없다 */

	iommu = info->iommu;	/* [한국어] 담당 유닛 */
	domain = info->domain;	/* [한국어] 붙어 있는 도메인 */
	sid = PCI_DEVID(info->bus, info->devfn);	/* [한국어] 16비트 소스 id */
	did = domain ? domain_id_iommu(domain, iommu) : FLPT_DEFAULT_DID;	/* [한국어] 도메인이 없으면 예약 id 를 쓴다 — 이미 떨어져 나가는 중일 수 있다 */

	/*
	 * Check and wait until all pending page requests in the queue are
	 * handled by the prq handling thread.
	 */
prq_retry:	/* [한국어] 기다린 뒤 head/tail 이 움직였으므로 처음부터 다시 훑는다 */
	reinit_completion(&iommu->prq_complete);	/* [한국어] 폴트 스레드가 한 바퀴를 끝냈음을 알릴 신호를 초기화 */
	tail = readq(iommu->reg + DMAR_PQT_REG) & PRQ_RING_MASK;	/* [한국어] 하드웨어가 채운 지점 */
	head = readq(iommu->reg + DMAR_PQH_REG) & PRQ_RING_MASK;	/* [한국어] 커널이 소비한 지점 */
	while (head != tail) {	/* [한국어] 아직 처리되지 않은 요청들을 훑으며 (위 영어 주석) */
		struct page_req_dsc *req;	/* [한국어] 현재 요청 */

		req = &iommu->prq[head / sizeof(*req)];	/* [한국어] 링에서 그 자리 */
		if (req->rid != sid ||	/* [한국어] 다른 장치의 요청이거나 */
		    (req->pasid_present && pasid != req->pasid) ||	/* [한국어] 다른 PASID 의 요청이거나 */
		    (!req->pasid_present && pasid != IOMMU_NO_PASID)) {	/* [한국어] PASID 없는 요청인데 우리가 찾는 것은 특정 PASID 이면 */
			head = (head + sizeof(*req)) & PRQ_RING_MASK;	/* [한국어] 건너뛴다 */
			continue;	/* [한국어] 다음 요청 */
		}

		wait_for_completion(&iommu->prq_complete);	/* [한국어] 우리 요청이 남아 있다 — 폴트 스레드가 처리할 때까지 기다린다 */
		goto prq_retry;	/* [한국어] 기다리는 동안 링이 움직였으므로 처음부터 다시 */
	}

	iopf_queue_flush_dev(dev);	/* [한국어] 코어 쪽 큐에 남은 것도 비운다. 여기까지가 소프트웨어 배수다 */

	/*
	 * Perform steps described in VT-d spec CH7.10 to drain page
	 * requests and responses in hardware.
	 */
	memset(desc, 0, sizeof(desc));	/* [한국어] 서술자를 깨끗이 (아래는 스펙 7.10 의 절차 — 위 영어 주석) */
	desc[0].qw0 = QI_IWD_STATUS_DATA(QI_DONE) |	/* [한국어] 완료 시 기록할 값과 */
			QI_IWD_FENCE |	/* [한국어] 펜스 — 이 앞의 것이 모두 끝나야 뒤가 시작된다 */
			QI_IWD_TYPE;	/* [한국어] Invalidation Wait 서술자 */
	if (pasid == IOMMU_NO_PASID) {	/* [한국어] PASID 를 쓰지 않는 기본 트래픽이면 */
		qi_desc_iotlb(iommu, did, 0, 0, DMA_TLB_DSI_FLUSH, &desc[1]);	/* [한국어] 도메인 단위 IOTLB 무효화 */
		qi_desc_dev_iotlb(sid, info->pfsid, info->ats_qdep, 0,	/* [한국어] 장치 캐시도 전체 범위로 */
				  MAX_AGAW_PFN_WIDTH, &desc[2]);	/* [한국어] 주소 공간 전체 */
	} else {
		qi_desc_piotlb_all(did, pasid, &desc[1]);	/* [한국어] PASID 트래픽이면 그 PASID 의 IOTLB 전체 */
		qi_desc_dev_iotlb_pasid(sid, info->pfsid, pasid, info->ats_qdep,	/* [한국어] PASID 를 지정한 장치 캐시 무효화 */
					0, MAX_AGAW_PFN_WIDTH, &desc[2]);	/* [한국어] 주소 공간 전체 */
	}
qi_retry:	/* [한국어] 오버플로가 났으면 여기로 돌아온다 */
	reinit_completion(&iommu->prq_complete);	/* [한국어] 신호를 초기화 */
	qi_submit_sync(iommu, desc, 3, QI_OPT_WAIT_DRAIN);	/* [한국어] 셋을 한 묶음으로 보낸다. WAIT_DRAIN 이 "대기 중인 페이지 요청까지 배수하라"를 하드웨어에 알린다 */
	if (readl(iommu->reg + DMAR_PRS_REG) & DMA_PRS_PRO) {	/* [한국어] 배수 도중 큐가 넘쳤으면 */
		wait_for_completion(&iommu->prq_complete);	/* [한국어] 폴트 스레드가 그것을 정리할 때까지 기다린 뒤 */
		goto qi_retry;	/* [한국어] 다시 시도한다 */
	}
}

/*
 * [한국어]
 * is_canonical_address - x86 정규 주소인지 검사한다
 *
 * @addr: 검사할 주소.
 * @return: true 면 정규 주소다.
 *
 * x86-64 는 48비트(또는 5레벨에서 57비트)만 실제로 쓰고, 그 위 비트는 모두
 * 최상위 유효 비트와 같아야 한다(부호 확장). 그 규칙을 어긴 주소를
 * non-canonical 이라 하며, CPU 는 그런 주소에 접근하면 예외를 낸다.
 *
 * 검사 방법이 그 정의를 그대로 코드로 옮긴 것이다: 유효 비트 수만큼 왼쪽으로
 * 밀었다가 산술 시프트로 되돌린다. 부호 확장이 올바로 되어 있었다면 원래
 * 값이 그대로 나오고, 아니면 달라진다.
 *
 * 왜 여기서 검사하는가: SVA 는 프로세스의 페이지 테이블을 쓰므로 장치가
 * 요청하는 주소도 CPU 주소 공간의 규칙을 따라야 한다. 정규 주소가 아닌
 * 요청은 장치나 하드웨어의 오동작이므로 거절한다.
 *
 * 실행 컨텍스트: 폴트 처리 스레드. 순수 계산.
 */
static bool is_canonical_address(u64 addr)
{
	int shift = 64 - (__VIRTUAL_MASK_SHIFT + 1);	/* [한국어] 유효 비트 위쪽에 남는 비트 수 */
	long saddr = (long)addr;	/* [한국어] 부호 있는 정수로 — 산술 시프트를 쓰기 위해서다 */

	return (((saddr << shift) >> shift) == saddr);	/* [한국어] 왼쪽으로 밀었다가 산술 시프트로 되돌린다. 부호 확장이 올바랐다면 원래 값이 그대로 나온다 */
}

/*
 * [한국어]
 * handle_bad_prq_event - 잘못된 페이지 요청에 거절 응답을 보낸다
 *
 * @iommu: 요청을 받은 유닛. @req: 그 요청. @result: 응답 코드(QI_RESP_*).
 * @return: 없음.
 *
 * 왜 그냥 버리지 않는가: 응답하지 않은 페이지 요청은 장치를 영원히 멈춰
 * 세운다. 요청이 잘못되었더라도 "이 요청은 처리할 수 없다"고 답해 줘야
 * 장치가 다음으로 넘어간다.
 *
 * lpig 가 아니면 응답하지 않는 것이 중요하다. 응답은 그룹 단위이므로 중간
 * 요청에 답하면 장치가 혼란스러워한다. 그룹의 마지막 요청이 올 때 그때
 * 거절하면 된다.
 *
 * 요청 내용을 워드 통째로 로그에 찍는 이유: 무엇이 잘못되었는지 필드로
 * 해석해 찍는 것보다 원본을 남기는 편이 사후 분석에 낫다. 애초에 우리가
 * 이해하지 못하는 값이라 잘못되었다고 판단한 것이기 때문이다.
 *
 * 실행 컨텍스트: 폴트 처리 스레드. qi_submit_sync 로 완료를 기다린다.
 */
static void handle_bad_prq_event(struct intel_iommu *iommu,
				 struct page_req_dsc *req, int result)
{
	struct qi_desc desc = { };	/* [한국어] 보낼 응답 서술자 */

	pr_err("%s: Invalid page request: %08llx %08llx\n",	/* [한국어] 요청 워드를 통째로 남긴다 — 우리가 해석하지 못한 값이라 원본이 더 유용하다 */
	       iommu->name, ((unsigned long long *)req)[0],	/* [한국어] 첫 워드 */
	       ((unsigned long long *)req)[1]);	/* [한국어] 둘째 워드 */

	if (!req->lpig)	/* [한국어] 그룹의 마지막 요청이 아니면 */
		return;	/* [한국어] 응답하지 않는다. 응답은 그룹 단위이므로 중간에 답하면 장치가 혼란스러워한다 */

	desc.qw0 = QI_PGRP_PASID(req->pasid) |	/* [한국어] 원래 요청의 PASID 와 */
			QI_PGRP_DID(req->rid) |	/* [한국어] 소스 id 를 그대로 실어 */
			QI_PGRP_PASID_P(req->pasid_present) |	/* [한국어] PASID 유효 표시도 그대로 */
			QI_PGRP_RESP_CODE(result) |	/* [한국어] 거절 코드와 함께 */
			QI_PGRP_RESP_TYPE;	/* [한국어] 응답 서술자로 만든다 */
	desc.qw1 = QI_PGRP_IDX(req->prg_index);	/* [한국어] 그룹 번호. 이 값이 틀리면 장치가 짝을 찾지 못한다 */

	qi_submit_sync(iommu, &desc, 1, 0);	/* [한국어] 응답을 보내고 완료를 기다린다 */
}

/*
 * [한국어]
 * prq_to_iommu_prot - 하드웨어 요청의 권한 비트를 코어의 권한 플래그로 옮긴다
 *
 * @req: 페이지 요청.
 * @return: IOMMU_FAULT_PERM_* 조합.
 *
 * VT-d 서술자의 비트를 벤더 중립적인 표현으로 바꾸는 얇은 변환이다. 코어의
 * 폴트 처리 계층은 하드웨어 형식을 모르므로, 이 경계에서 번역해 넘긴다.
 *
 * 이 값이 실제로 무엇을 결정하는가: SVA 에서 코어는 이 권한과 그 주소의
 * VMA 권한을 대조한다. 장치가 쓰기 권한 없는 페이지에 쓰기를 요청하면
 * 거절되는데, 그 판단의 입력이 여기서 만들어진다. 즉 이 변환이 SVA 의
 * 실질적인 보안 경계로 이어진다.
 *
 * 실행 컨텍스트: 폴트 처리 스레드. 순수 계산.
 */
static int prq_to_iommu_prot(struct page_req_dsc *req)
{
	int prot = 0;	/* [한국어] 만들 권한 조합 */

	if (req->rd_req)	/* [한국어] 읽기 요청이면 */
		prot |= IOMMU_FAULT_PERM_READ;	/* [한국어] 읽기 권한 */
	if (req->wr_req)	/* [한국어] 쓰기 요청이면 */
		prot |= IOMMU_FAULT_PERM_WRITE;	/* [한국어] 쓰기 권한 */
	if (req->exe_req)	/* [한국어] 실행 요청이면 */
		prot |= IOMMU_FAULT_PERM_EXEC;	/* [한국어] 실행 권한 */
	if (req->pm_req)	/* [한국어] 특권 요청이면 */
		prot |= IOMMU_FAULT_PERM_PRIV;	/* [한국어] 특권 */

	return prot;	/* [한국어] 코어가 VMA 권한과 대조할 값. 이 대조가 SVA 의 실질적인 보안 경계다 */
}

/*
 * [한국어]
 * intel_prq_report - 페이지 요청을 코어의 폴트 처리 계층에 넘긴다
 *
 * @iommu: 유닛(여기서는 쓰지 않는다). @dev: 요청을 낸 장치.
 * @desc: 하드웨어 서술자.
 * @return: 없음.
 *
 * 하드웨어 형식의 요청을 벤더 중립적인 struct iopf_fault 로 옮겨
 * iommu_report_device_fault() 에 넘긴다. 그쪽이 요청을 그룹으로 모아
 * SVA 핸들러나 장치 드라이버에 전달하고, 응답이 정해지면
 * intel_iommu_page_response() 를 통해 돌아온다.
 *
 * 옮기는 값들: 주소(페이지 번호를 바이트 주소로 복원), PASID, 그룹 번호,
 * 그리고 권한. 여기에 두 플래그가 붙는다.
 *   LAST_PAGE          — 그룹의 마지막 요청. 코어가 그룹의 완성을 아는 근거다.
 *   PASID_VALID 와 RESPONSE_NEEDS_PASID — PASID 가 실려 있었으면 응답에도
 *     그 PASID 를 실어야 한다. 장치가 응답을 자기 요청과 짝지을 때 쓴다.
 *
 * 실행 컨텍스트: 폴트 처리 스레드. iopf_lock 을 쥔 채 불린다 — 그래야
 * 보고하는 도중에 장치가 해제되지 않는다.
 */
static void intel_prq_report(struct intel_iommu *iommu, struct device *dev,
			     struct page_req_dsc *desc)
{
	struct iopf_fault event = { };	/* [한국어] 코어에 넘길 벤더 중립 형식 */

	/* Fill in event data for device specific processing */
	event.fault.type = IOMMU_FAULT_PAGE_REQ;	/* [한국어] 페이지 요청 폴트 (위 영어 주석) */
	event.fault.prm.addr = (u64)desc->addr << VTD_PAGE_SHIFT;	/* [한국어] 페이지 번호를 바이트 주소로 복원 */
	event.fault.prm.pasid = desc->pasid;	/* [한국어] 어느 주소 공간인지 */
	event.fault.prm.grpid = desc->prg_index;	/* [한국어] 그룹 번호. 응답에 그대로 실려야 한다 */
	event.fault.prm.perm = prq_to_iommu_prot(desc);	/* [한국어] 요청한 권한 */

	if (desc->lpig)	/* [한국어] 그룹의 마지막 요청이면 */
		event.fault.prm.flags |= IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE;	/* [한국어] 코어가 그룹의 완성을 아는 근거 */
	if (desc->pasid_present) {	/* [한국어] PASID 가 실려 있었으면 */
		event.fault.prm.flags |= IOMMU_FAULT_PAGE_REQUEST_PASID_VALID;	/* [한국어] PASID 필드가 유효하다고 표시하고 */
		event.fault.prm.flags |= IOMMU_FAULT_PAGE_RESPONSE_NEEDS_PASID;	/* [한국어] 응답에도 그 PASID 를 실어야 한다고 알린다 */
	}

	iommu_report_device_fault(dev, &event);	/* [한국어] 코어의 폴트 처리 계층으로 넘긴다 */
}

/*
 * [한국어]
 * prq_event_thread - PRQ 인터럽트 스레드. 큐에 쌓인 페이지 요청을 처리한다
 *
 * @irq: 인터럽트 번호(쓰지 않는다). @d: 이 유닛.
 * @return: 처리한 요청이 있었으면 IRQ_HANDLED, 없었으면 IRQ_NONE.
 *
 * 이 파일의 중심이며, 하드웨어가 채우는 큐를 커널이 소비하는 유일한 곳이다.
 *
 * 맨 앞에서 PPR 비트를 먼저 지우는 것이 중요하다(코드 안 영어 주석).
 * head/tail 을 읽기 전에 지워야, 읽은 뒤 새 요청이 들어왔을 때 인터럽트를
 * 다시 받는다. 순서를 바꾸면 그 사이에 들어온 요청이 인터럽트 없이 큐에
 * 남아 장치가 영원히 기다리게 된다.
 *
 * 각 요청에 대해 네 가지를 검증한다. 모두 "장치나 하드웨어가 이상한 값을
 * 보냈다"는 경우이고, 거절 응답을 보낸 뒤 다음으로 넘어간다.
 *   - 주소가 정규(canonical) 주소인가.
 *   - 특권 요청과 읽기/쓰기 요청이 함께 오지 않았는가.
 *   - 실행 요청과 읽기 요청이 함께 오지 않았는가.
 *   - 소스 id 로 장치를 찾을 수 있는가(이미 해제된 장치일 수 있다).
 * Stop Marker(lpig 만 있고 읽기·쓰기가 없는 것)는 예외로, 응답 없이 버린다 —
 * 장치가 그 그룹을 스스로 포기했다는 통보이기 때문이다.
 *
 * iopf_lock 을 잡는 구간이 좁은 이유: 장치를 찾고 보고하는 동안만 잡으면
 * 된다. 이 락이 장치 해제(intel_iommu_release_device)와의 경쟁을 막는다 —
 * 그쪽이 같은 락 안에서 소스 id 트리를 지우므로, 여기서 찾은 장치는 보고가
 * 끝날 때까지 살아 있다.
 *
 * 마지막에 head 레지스터를 tail 로 밀어 "여기까지 소비했다"를 하드웨어에
 * 알린다. 그 다음 오버플로를 확인하는데(코드 안 영어 주석), 넘쳤다면 잃어버린
 * 요청이 있다는 뜻이다. 그 경우 코어의 부분 그룹을 버리고 오버플로 비트를
 * 지운다 — 잃어버린 요청 때문에 영영 완성되지 않을 그룹들이기 때문이다.
 * head 와 tail 이 같을 때만 지우는 것은, 큐가 비어야 새로 시작할 수 있어서다.
 *
 * complete() 로 끝나는 것은 배수를 기다리는 쪽(drain_pasid_prq)에 한 바퀴가
 * 끝났음을 알리기 위해서다.
 *
 * 실행 컨텍스트: threaded IRQ 핸들러. 잠들 수 있어 뮤텍스를 잡을 수 있다.
 *
 * 호출 체인:
 *   PRQ 인터럽트 → [prq_event_thread] → device_rbtree_find()
 *     → intel_prq_report() → iommu_report_device_fault()
 */
static irqreturn_t prq_event_thread(int irq, void *d)
{
	struct intel_iommu *iommu = d;	/* [한국어] 인터럽트 등록 때 넘긴 유닛 */
	struct page_req_dsc *req;	/* [한국어] 현재 처리할 요청 */
	int head, tail, handled;	/* [한국어] 링의 소비·생산 지점과, 처리한 것이 있었는지 */
	struct device *dev;	/* [한국어] 요청을 낸 장치 */
	u64 address;	/* [한국어] 폴트가 난 주소 */

	/*
	 * Clear PPR bit before reading head/tail registers, to ensure that
	 * we get a new interrupt if needed.
	 */
	writel(DMA_PRS_PPR, iommu->reg + DMAR_PRS_REG);	/* [한국어] head/tail 을 읽기 전에 PPR 비트를 먼저 지운다. 순서를 바꾸면 읽은 뒤 들어온 요청이 인터럽트 없이 큐에 남는다 (위 영어 주석) */

	tail = readq(iommu->reg + DMAR_PQT_REG) & PRQ_RING_MASK;	/* [한국어] 하드웨어가 채운 지점 */
	head = readq(iommu->reg + DMAR_PQH_REG) & PRQ_RING_MASK;	/* [한국어] 우리가 소비한 지점 */
	handled = (head != tail);	/* [한국어] 처리할 것이 있었는지 기억한다 — 반환값이 된다 */
	while (head != tail) {	/* [한국어] 요청을 하나씩 */
		req = &iommu->prq[head / sizeof(*req)];	/* [한국어] 링에서 그 자리 */
		address = (u64)req->addr << VTD_PAGE_SHIFT;	/* [한국어] 페이지 번호를 바이트 주소로 */

		if (unlikely(!is_canonical_address(address))) {	/* [한국어] x86 정규 주소가 아니면 */
			pr_err("IOMMU: %s: Address is not canonical\n",	/* [한국어] 장치나 하드웨어의 오동작이다 */
			       iommu->name);	/* [한국어] 어느 유닛인지 */
bad_req:	/* [한국어] 잘못된 요청들이 합류 */
			handle_bad_prq_event(iommu, req, QI_RESP_INVALID);	/* [한국어] 거절 응답을 보낸다. 버리면 장치가 영원히 기다린다 */
			goto prq_advance;	/* [한국어] 다음 요청으로 */
		}

		if (unlikely(req->pm_req && (req->rd_req | req->wr_req))) {	/* [한국어] 특권 요청과 읽기/쓰기 요청이 함께 왔으면 */
			pr_err("IOMMU: %s: Page request in Privilege Mode\n",	/* [한국어] 이 드라이버가 지원하지 않는 조합이다 */
			       iommu->name);	/* [한국어] 어느 유닛인지 */
			goto bad_req;	/* [한국어] 거절 */
		}

		if (unlikely(req->exe_req && req->rd_req)) {	/* [한국어] 실행 요청과 읽기 요청이 함께 왔으면 */
			pr_err("IOMMU: %s: Execution request not supported\n",	/* [한국어] 역시 지원하지 않는 조합이다 */
			       iommu->name);	/* [한국어] 어느 유닛인지 */
			goto bad_req;	/* [한국어] 거절 */
		}

		/* Drop Stop Marker message. No need for a response. */
		if (unlikely(req->lpig && !req->rd_req && !req->wr_req))	/* [한국어] Stop Marker — 장치가 그 그룹을 스스로 포기했다는 통보다 (위 영어 주석) */
			goto prq_advance;	/* [한국어] 응답 없이 버린다 */

		/*
		 * If prq is to be handled outside iommu driver via receiver of
		 * the fault notifiers, we skip the page response here.
		 */
		mutex_lock(&iommu->iopf_lock);	/* [한국어] 장치 해제와의 경쟁을 막는다 — 이 락 안에서 찾은 장치는 보고가 끝날 때까지 살아 있다 */
		dev = device_rbtree_find(iommu, req->rid);	/* [한국어] 소스 id 로 장치를 되찾는다 */
		if (!dev) {	/* [한국어] 이미 해제된 장치이거나 등록되지 않은 id 면 */
			mutex_unlock(&iommu->iopf_lock);	/* [한국어] 락을 놓고 */
			goto bad_req;	/* [한국어] 거절 응답을 보낸다 */
		}

		intel_prq_report(iommu, dev, req);	/* [한국어] 코어의 폴트 처리 계층으로 넘긴다 (위 영어 주석: 드라이버가 자체 처리하는 경우 응답도 그쪽이 한다) */
		trace_prq_report(iommu, dev, req->qw_0, req->qw_1,	/* [한국어] 추적 이벤트에 원본 워드를 남긴다 */
				 req->qw_2, req->qw_3,	/* [한국어] 사설 데이터 워드까지 */
				 iommu->prq_seq_number++);	/* [한국어] 누적 번호. 배수가 끝났는지 판단하는 기준점이 된다 */
		mutex_unlock(&iommu->iopf_lock);	/* [한국어] 락 해제 */
prq_advance:	/* [한국어] 처리했거나 버린 요청이 합류 */
		head = (head + sizeof(*req)) & PRQ_RING_MASK;	/* [한국어] 다음 자리로 (링이라 마스크로 감싼다) */
	}

	writeq(tail, iommu->reg + DMAR_PQH_REG);	/* [한국어] 여기까지 소비했다고 하드웨어에 알린다 */

	/*
	 * Clear the page request overflow bit and wake up all threads that
	 * are waiting for the completion of this handling.
	 */
	if (readl(iommu->reg + DMAR_PRS_REG) & DMA_PRS_PRO) {	/* [한국어] 큐가 넘쳤으면 (위 영어 주석) */
		pr_info_ratelimited("IOMMU: %s: PRQ overflow detected\n",	/* [한국어] 잃어버린 요청이 있다는 뜻이다 */
				    iommu->name);	/* [한국어] 어느 유닛인지 */
		head = readq(iommu->reg + DMAR_PQH_REG) & PRQ_RING_MASK;	/* [한국어] 현재 상태를 다시 읽어 */
		tail = readq(iommu->reg + DMAR_PQT_REG) & PRQ_RING_MASK;	/* [한국어] 큐가 비었는지 확인한다 */
		if (head == tail) {	/* [한국어] 비었으면 */
			iopf_queue_discard_partial(iommu->iopf_queue);	/* [한국어] 코어의 미완성 그룹들을 버린다. 잃어버린 요청 때문에 영영 완성되지 않을 그룹들이다 */
			writel(DMA_PRS_PRO, iommu->reg + DMAR_PRS_REG);	/* [한국어] 오버플로 비트를 지워 다시 시작한다 */
			pr_info_ratelimited("IOMMU: %s: PRQ overflow cleared",	/* [한국어] 회복되었음을 남긴다 */
					    iommu->name);	/* [한국어] 어느 유닛인지 */
		}
	}

	if (!completion_done(&iommu->prq_complete))	/* [한국어] 배수를 기다리는 쪽이 있으면 */
		complete(&iommu->prq_complete);	/* [한국어] 한 바퀴가 끝났음을 알린다 */

	return IRQ_RETVAL(handled);	/* [한국어] 처리한 것이 있었으면 HANDLED. 공유 인터럽트에서 다른 장치의 것이 아님을 알린다 */
}

/*
 * [한국어]
 * intel_iommu_enable_prq - 페이지 요청 큐와 그 인터럽트를 세운다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, 음수면 실패(그 경우 아무것도 남지 않는다).
 *
 * 네 가지를 순서대로 준비한다.
 *   1) PRQ 링 버퍼(64KB). 하드웨어가 직접 채우므로 물리적으로 연속이어야 한다.
 *   2) 인터럽트 벡터. IOMMU_IRQ_ID_OFFSET_PRQ 를 더해 폴트 인터럽트와 다른
 *      id 구간을 쓴다 — 유닛 하나가 폴트·PRQ·성능 셋을 가질 수 있어서다.
 *   3) 코어의 iopf 큐. 요청을 그룹으로 모아 처리하는 워커가 여기 붙는다.
 *   4) threaded IRQ 등록. 상위 핸들러 없이(NULL) 스레드만 두는 것은, 이
 *      처리가 잠들 수 있는 작업(장치 조회, 페이지 채우기)이기 때문이다.
 *      IRQF_ONESHOT 은 스레드가 끝날 때까지 인터럽트를 마스크한다.
 *
 * 하드웨어에 알리는 것은 마지막이다: head/tail 을 0 으로 초기화하고 큐의
 * 물리 주소와 크기(PRQ_ORDER)를 PQA 에 쓴다. 이 순간부터 하드웨어가 요청을
 * 넣기 시작하므로, 그 전에 인터럽트와 처리 경로가 모두 준비되어 있어야 한다.
 *
 * 실패 경로가 세 라벨로 정확히 역순으로 되돌린다.
 *
 * 실행 컨텍스트: 유닛 초기화. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   init_dmars()/intel_iommu_add() → [intel_iommu_enable_prq]
 *     → iopf_queue_alloc() → request_threaded_irq()
 */
int intel_iommu_enable_prq(struct intel_iommu *iommu)
{
	struct iopf_queue *iopfq;	/* [한국어] 코어의 폴트 처리 큐 */
	int irq, ret;	/* [한국어] 인터럽트 번호와 결과 */

	iommu->prq =	/* [한국어] PRQ 링 버퍼를 잡는다 */
		iommu_alloc_pages_node_sz(iommu->node, GFP_KERNEL, PRQ_SIZE);	/* [한국어] 64KB 링 버퍼. 하드웨어가 직접 채우므로 물리적으로 연속이어야 한다 */
	if (!iommu->prq) {	/* [한국어] 할당 실패 */
		pr_warn("IOMMU: %s: Failed to allocate page request queue\n",	/* [한국어] 이유를 남기고 */
			iommu->name);	/* [한국어] 어느 유닛인지 */
		return -ENOMEM;	/* [한국어] PRI 를 쓸 수 없다 */
	}

	irq = dmar_alloc_hwirq(IOMMU_IRQ_ID_OFFSET_PRQ + iommu->seq_id, iommu->node, iommu);	/* [한국어] PRQ 전용 id 구간에서 인터럽트 벡터를 잡는다. 유닛 하나가 폴트·PRQ·성능 셋을 가질 수 있어 구간을 나눠 쓴다 */
	if (irq <= 0) {	/* [한국어] 실패 */
		pr_err("IOMMU: %s: Failed to create IRQ vector for page request queue\n",	/* [한국어] 이유를 남기고 */
		       iommu->name);	/* [한국어] 어느 유닛인지 */
		ret = -EINVAL;	/* [한국어] 실패 */
		goto free_prq;	/* [한국어] 버퍼를 반납한다 */
	}
	iommu->pr_irq = irq;	/* [한국어] 해제 경로가 쓸 수 있게 기억한다 */

	snprintf(iommu->iopfq_name, sizeof(iommu->iopfq_name),	/* [한국어] 폴트 큐의 이름을 만든다 */
		 "dmar%d-iopfq", iommu->seq_id);	/* [한국어] 유닛 순번을 붙여 구분한다 */
	iopfq = iopf_queue_alloc(iommu->iopfq_name);	/* [한국어] 요청을 그룹으로 모아 처리하는 코어 큐 */
	if (!iopfq) {	/* [한국어] 할당 실패 */
		pr_err("IOMMU: %s: Failed to allocate iopf queue\n", iommu->name);	/* [한국어] 이유를 남기고 */
		ret = -ENOMEM;	/* [한국어] 실패 */
		goto free_hwirq;	/* [한국어] 인터럽트를 반납한다 */
	}
	iommu->iopf_queue = iopfq;	/* [한국어] 유닛에 매단다 */

	snprintf(iommu->prq_name, sizeof(iommu->prq_name), "dmar%d-prq", iommu->seq_id);	/* [한국어] 인터럽트 이름. /proc/interrupts 에 나타난다 */

	ret = request_threaded_irq(irq, NULL, prq_event_thread, IRQF_ONESHOT,	/* [한국어] 상위 핸들러 없이 스레드만 둔다 — 폴트 처리가 잠들 수 있는 작업이기 때문이다. ONESHOT 은 스레드가 끝날 때까지 인터럽트를 마스크한다 */
				   iommu->prq_name, iommu);	/* [한국어] 이름과, 핸들러에 넘길 유닛 */
	if (ret) {	/* [한국어] 등록 실패 */
		pr_err("IOMMU: %s: Failed to request IRQ for page request queue\n",	/* [한국어] 이유를 남기고 */
		       iommu->name);	/* [한국어] 어느 유닛인지 */
		goto free_iopfq;	/* [한국어] 폴트 큐를 반납한다 */
	}
	writeq(0ULL, iommu->reg + DMAR_PQH_REG);	/* [한국어] 소비 지점을 0 으로 */
	writeq(0ULL, iommu->reg + DMAR_PQT_REG);	/* [한국어] 생산 지점도 0 으로 */
	writeq(virt_to_phys(iommu->prq) | PRQ_ORDER, iommu->reg + DMAR_PQA_REG);	/* [한국어] 마지막으로 버퍼 주소와 크기를 알린다. 이 순간부터 하드웨어가 요청을 넣기 시작하므로, 그 전에 처리 경로가 모두 준비되어 있어야 한다 */

	init_completion(&iommu->prq_complete);	/* [한국어] 배수를 기다리는 쪽에 쓸 신호 */

	return 0;	/* [한국어] 페이지 요청을 받을 준비가 되었다 */

free_iopfq:	/* [한국어] 인터럽트 등록 실패 경로 */
	iopf_queue_free(iommu->iopf_queue);	/* [한국어] 폴트 큐 반납 */
	iommu->iopf_queue = NULL;	/* [한국어] 두 번 해제되지 않게 */
free_hwirq:	/* [한국어] 폴트 큐 실패가 합류 */
	dmar_free_hwirq(irq);	/* [한국어] 인터럽트 벡터 반납 */
	iommu->pr_irq = 0;	/* [한국어] 기록도 지운다 */
free_prq:	/* [한국어] 인터럽트 할당 실패가 합류 */
	iommu_free_pages(iommu->prq);	/* [한국어] 링 버퍼 반납 */
	iommu->prq = NULL;	/* [한국어] 두 번 해제되지 않게 */

	return ret;	/* [한국어] 실패 이유 */
}

/*
 * [한국어]
 * intel_iommu_finish_prq - 페이지 요청 큐를 정지시키고 자원을 반납한다
 *
 * @iommu: 대상 유닛.
 * @return: 항상 0.
 *
 * enable 의 역순이다. 하드웨어를 먼저 멈추는 것이 핵심이다 — PQA 를 0 으로
 * 만들어 하드웨어가 더 이상 이 버퍼에 요청을 넣지 않게 한 뒤에야 인터럽트를
 * 풀고 버퍼를 반납할 수 있다. 순서를 바꾸면 해제된 메모리에 하드웨어가
 * 요청을 쓰게 된다.
 *
 * free_irq 가 dmar_free_hwirq 보다 먼저 오는 것도 같은 원칙이다: 핸들러를
 * 먼저 떼어 내야 벡터를 반납할 수 있다.
 *
 * 각 자원을 확인하고 해제하는 것은 enable 이 중간에 실패했을 수도 있기
 * 때문이며, 해제 후 NULL/0 으로 되돌려 두 번 해제되지 않게 한다.
 *
 * 실행 컨텍스트: 유닛 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   free_dmar_iommu()/disable_dmar_iommu() → [intel_iommu_finish_prq]
 */
int intel_iommu_finish_prq(struct intel_iommu *iommu)
{
	writeq(0ULL, iommu->reg + DMAR_PQH_REG);	/* [한국어] 소비 지점을 0 으로 */
	writeq(0ULL, iommu->reg + DMAR_PQT_REG);	/* [한국어] 생산 지점도 0 으로 */
	writeq(0ULL, iommu->reg + DMAR_PQA_REG);	/* [한국어] 버퍼 주소를 지운다 — 하드웨어를 먼저 멈춰야 아래에서 버퍼를 반납할 수 있다 */

	if (iommu->pr_irq) {	/* [한국어] 인터럽트를 걸었었으면 */
		free_irq(iommu->pr_irq, iommu);	/* [한국어] 핸들러를 먼저 떼어 내고 */
		dmar_free_hwirq(iommu->pr_irq);	/* [한국어] 그 다음 벡터를 반납한다 */
		iommu->pr_irq = 0;	/* [한국어] 기록을 지운다 */
	}

	if (iommu->iopf_queue) {	/* [한국어] 폴트 큐가 있으면 */
		iopf_queue_free(iommu->iopf_queue);	/* [한국어] 반납하고 */
		iommu->iopf_queue = NULL;	/* [한국어] 기록을 지운다 */
	}

	iommu_free_pages(iommu->prq);	/* [한국어] 링 버퍼 반납. 하드웨어가 이미 멈췄으므로 안전하다 */
	iommu->prq = NULL;	/* [한국어] 기록을 지운다 */

	return 0;	/* [한국어] 정리 완료 */
}

/*
 * [한국어]
 * intel_iommu_page_response - 코어가 정한 응답을 하드웨어로 보낸다
 *
 * @dev: 응답을 받을 장치. @evt: 원래 폴트 이벤트(그 안에 요청 정보가 있다).
 * @msg: 코어가 정한 응답(code 가 성공/실패를 나타낸다).
 * @return: 없음.
 *
 * 폴트 처리의 마지막 단계다. 이 응답이 도착해야 장치가 그 그룹의 접근을
 * 다시 시도하거나 포기한다. 응답이 오지 않으면 장치는 영원히 기다린다 —
 * 이 파일 전체가 "모든 요청에 정확히 한 번 답한다"를 지키는 이유다.
 *
 * 응답 서술자에 실리는 값들이 원래 요청과 정확히 맞아야 한다.
 *   소스 id  — 어느 장치에 보낼지.
 *   PASID    — 요청에 실려 있었으면 응답에도 실어야 한다.
 *   grpid    — 어느 그룹에 대한 답인지. 이 값이 틀리면 장치가 짝을 찾지
 *              못해 그 그룹은 영영 미완으로 남는다.
 *   code     — SUCCESS 면 재시도, INVALID/FAILURE 면 포기.
 *
 * qw2/qw3 를 0 으로 미는 이유는 다른 서술자들과 같다 — scalable 모드의
 * 32바이트 서술자에서 하드웨어가 읽으므로 예약 필드를 비워야 한다.
 *
 * 실행 컨텍스트: 코어의 폴트 처리 워커. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   io-pgfault.c 의 응답 경로 → iommu_ops.page_response
 *     → [intel_iommu_page_response] → qi_submit_sync()
 */
void intel_iommu_page_response(struct device *dev, struct iopf_fault *evt,
			       struct iommu_page_response *msg)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);	/* [한국어] 장치 정보 */
	struct intel_iommu *iommu = info->iommu;	/* [한국어] 담당 유닛 */
	u8 bus = info->bus, devfn = info->devfn;	/* [한국어] 소스 id 의 두 부분 */
	struct iommu_fault_page_request *prm;	/* [한국어] 원래 요청의 정보 */
	struct qi_desc desc;	/* [한국어] 보낼 응답 서술자 */
	bool pasid_present;	/* [한국어] PASID 가 실려 있었는지 */
	u16 sid;	/* [한국어] 16비트 소스 id */

	prm = &evt->fault.prm;	/* [한국어] 원래 요청 정보 */
	sid = PCI_DEVID(bus, devfn);	/* [한국어] 소스 id 를 조립 */
	pasid_present = prm->flags & IOMMU_FAULT_PAGE_REQUEST_PASID_VALID;	/* [한국어] 요청에 PASID 가 있었으면 응답에도 실어야 한다 */

	desc.qw0 = QI_PGRP_PASID(prm->pasid) | QI_PGRP_DID(sid) |	/* [한국어] PASID 와 대상 장치 */
			QI_PGRP_PASID_P(pasid_present) |	/* [한국어] PASID 유효 표시 */
			QI_PGRP_RESP_CODE(msg->code) |	/* [한국어] 코어가 정한 응답 코드 — SUCCESS 면 재시도, 아니면 포기 */
			QI_PGRP_RESP_TYPE;	/* [한국어] 응답 서술자 */
	desc.qw1 = QI_PGRP_IDX(prm->grpid);	/* [한국어] 그룹 번호. 이 값이 틀리면 장치가 짝을 찾지 못해 그 그룹은 영영 미완으로 남는다 */
	desc.qw2 = 0;	/* [한국어] 예약 워드를 비운다 */
	desc.qw3 = 0;	/* [한국어] 같음 */

	qi_submit_sync(iommu, &desc, 1, 0);	/* [한국어] 응답을 보낸다. 이것이 도착해야 장치가 다시 시도하거나 포기한다 */
}
