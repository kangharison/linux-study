/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 */

/*
 * [한국어 설명] Qualcomm MSM IOMMU 의 자료 구조와 매핑 속성 헤더 (msm_iommu.h)
 *
 * === 파일의 역할 ===
 * 초기 Qualcomm MSM SoC 에 들어간 IOMMU 의 하드웨어 모델을 커널 자료
 * 구조로 옮겨 놓은 헤더다. 매핑의 캐시·공유 속성 값, 문맥 뱅크와 장치
 * 식별자(MID)의 한계, 그리고 IOMMU 인스턴스와 문맥 뱅크를 담는 구조체
 * 둘이 전부다.
 * 이 하드웨어의 모델은 ARM SMMU v1 과 비슷하되 더 단순하다. 버스에서
 * 온 트랜잭션에는 MID 라는 식별자가 붙어 오고, 그 MID 가 어느 문맥
 * 뱅크로 갈지 하드웨어 표가 정한다. 문맥 뱅크 하나가 곧 주소 공간
 * 하나이며, 그 안에 ARM v7 짧은 서술자 형식의 페이지 테이블이 걸린다.
 * SMMU v3 의 스트림 표와 견주면 훨씬 작은 구조다 — 표가 아니라 뱅크
 * 개수(최대 128)가 곧 동시에 존재할 수 있는 도메인 수의 한계가 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치가 낸 DMA 는 이렇게 흘러간다:
 *
 *   장치 (버스 트랜잭션 + MID)
 *     → IOMMU 의 MID→문맥 뱅크 대응표
 *     → 그 뱅크에 걸린 페이지 테이블 (ARM v7 짧은 서술자)
 *     → 물리 주소
 *
 * 커널 쪽에서는 iommu 코어 → msm_iommu.c 의 연산표 → io-pgtable 의
 * ARM v7s 구현이 그 페이지 테이블을 짓는다. 이 헤더는 그 사이에서
 * "하드웨어 인스턴스와 뱅크를 어떻게 표현할 것인가"만 정한다.
 *
 * === 타 모듈과의 연결 ===
 * - msm_iommu.c: 여기 정의된 구조체를 실제로 다루는 드라이버 본체.
 * - msm_iommu_hw-8xxx.h: 레지스터 오프셋과 비트 정의.
 * - io-pgtable-arm-v7s.c: 실제 페이지 테이블을 짓는다.
 * - iommu 코어: iommu_device 와 iommu_domain 을 통해 만난다.
 * - 장치 트리: MID 목록과 뱅크 번호를 펌웨어가 알려 준다 — 그 값이
 *   struct msm_iommu_ctx_dev 로 옮겨 담긴다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct msm_iommu_dev: IOMMU 하드웨어 하나. 레지스터 주소, 클럭,
 *   인터럽트, 그리고 뱅크 사용 여부를 담은 비트맵을 쥔다.
 * - struct msm_iommu_ctx_dev: 문맥 뱅크 하나. 그 뱅크로 보낼 MID 목록을
 *   들고 있어, 하드웨어 대응표를 채울 때 쓰인다.
 * - msm_iommu_fault_handler(): 폴트 인터럽트 처리기. 지금은 오류를
 *   기록하는 것이 전부다 — 폴트를 상위로 올리는 통로가 아직 없다.
 * - MSM_IOMMU_ATTR_*: 매핑의 캐시·공유 속성 값.
 */

#ifndef MSM_IOMMU_H	/* [한국어] 이 헤더가 두 번 펼쳐지는 것을 막는 보호 매크로. */
#define MSM_IOMMU_H	/* [한국어] 처음 펼쳐질 때 표시를 남긴다. */

#include <linux/interrupt.h>	/* [한국어] 아래 폴트 처리기의 반환형(irqreturn_t). */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 iommu_device 등. */
#include <linux/clk.h>	/* [한국어] 이 하드웨어는 클럭을 켜야 레지스터에 닿는다. */

/* Sharability attributes of MSM IOMMU mappings */
/* [한국어] (위 영어 주석 참고) 매핑이 다른 코어와 캐시를 공유하는지 정하는 값.
 * 공유로 두면 캐시 일관성이 지켜지지만 성능이 떨어진다. */
#define MSM_IOMMU_ATTR_NON_SH		0x0	/* [한국어] 공유하지 않는다 — 이 매핑을 통한 접근은 캐시 일관성을 보장받지 않는다. */
#define MSM_IOMMU_ATTR_SH		0x4	/* [한국어] 공유한다 — CPU 와 캐시 일관성이 지켜진다. */

/* Cacheability attributes of MSM IOMMU mappings */
/* [한국어] (위 영어 주석 참고) 이 매핑을 통한 접근이 캐시를 어떻게 쓸지 정한다.
 * 되쓰기(WB)는 빠르지만 캐시를 씻어야 할 때가 있고, 통과 쓰기(WT)는
 * 느리지만 메모리와 늘 같다. */
#define MSM_IOMMU_ATTR_NONCACHED	0x0	/* [한국어] 캐시를 쓰지 않는다 — 장치와 CPU 가 같은 메모리를 다툴 때 안전하다. */
#define MSM_IOMMU_ATTR_CACHED_WB_WA	0x1	/* [한국어] 되쓰기 + 쓰기 할당 — 쓰기가 잦은 부하에 좋다. */
#define MSM_IOMMU_ATTR_CACHED_WB_NWA	0x2	/* [한국어] 되쓰기 + 쓰기 비할당 — 한 번 쓰고 다시 안 읽는 자료에 좋다. */
#define MSM_IOMMU_ATTR_CACHED_WT	0x3	/* [한국어] 통과 쓰기 — 메모리와 늘 같아 씻을 필요가 없다. */

/* Mask for the cache policy attribute */
/* [한국어] (위 영어 주석 참고) 위 네 값이 두 비트에 담기므로 그 자리를 고르는 마스크. */
#define MSM_IOMMU_CP_MASK		0x03	/* [한국어] 아래 두 비트가 캐시 정책이다. */

/* Maximum number of Machine IDs that we are allowing to be mapped to the same
 * context bank. The number of MIDs mapped to the same CB does not affect
 * performance, but there is a practical limit on how many distinct MIDs may
 * be present. These mappings are typically determined at design time and are
 * not expected to change at run time.
 */
/* [한국어] (위 영어 주석 참고) 한 문맥 뱅크로 보낼 수 있는 MID 의 최대 개수.
 * MID 는 버스 신호로 장치의 기능을 구분하는 값이며, 어느 MID 가 어느
 * 뱅크로 갈지는 칩 설계 단계에서 정해져 실행 중에 바뀌지 않는다.
 * 여러 MID 를 한 뱅크에 몰아도 성능에는 영향이 없고, 실제로 존재할 수
 * 있는 서로 다른 MID 수가 많지 않아 32 로 충분하다. */
#define MAX_NUM_MIDS	32	/* [한국어] 아래 배열의 크기가 된다. */

/* Maximum number of context banks that can be present in IOMMU */
/* [한국어] (위 영어 주석 참고) 이 하드웨어가 가질 수 있는 문맥 뱅크의 최대 수.
 * 뱅크 하나가 주소 공간 하나이므로, 이 값이 곧 동시에 존재할 수 있는
 * 도메인 수의 상한이다 — SMMU v3 처럼 표를 메모리에 두는 구조와 달리
 * 물리적 한계가 그대로 드러난다. */
#define IOMMU_MAX_CBS	128	/* [한국어] 아래 비트맵의 크기가 된다. */

/**
 * struct msm_iommu_dev - a single IOMMU hardware instance
 * ncb		Number of context banks present on this IOMMU HW instance
 * dev:		IOMMU device
 * irq:		Interrupt number
 * clk:		The bus clock for this IOMMU hardware instance
 * pclk:	The clock for the IOMMU bus interconnect
 * dev_node:	list head in qcom_iommu_device_list
 * dom_node:	list head for domain
 * ctx_list:	list of 'struct msm_iommu_ctx_dev'
 * context_map: Bitmap to track allocated context banks
 */
/* [한국어] IOMMU 하드웨어 하나. (위 영어 kernel-doc 참고)
 *
 * SoC 에 이런 인스턴스가 여러 개 있고, 각각이 자기 문맥 뱅크 묶음을
 * 갖는다. 도메인 하나가 여러 인스턴스에 걸칠 수도 있어 — 한 도메인에
 * 붙은 장치들이 서로 다른 IOMMU 뒤에 있을 수 있다 — 그래서 도메인 쪽
 * 목록에 매달릴 고리(dom_node)도 함께 들고 있다. */
struct msm_iommu_dev {
	/* [한국어] 이 인스턴스의 레지스터가 매핑된 커널 주소.
	 * 설정자: 프로브에서 ioremap 으로 얻는다.
	 * 읽는 자: 모든 레지스터 접근의 기준점.
	 * 값 범위: NULL 이면 프로브가 실패한 것이다.
	 * 동기화: 프로브 이후 불변. */
	void __iomem *base;
	/* [한국어] 이 인스턴스가 실제로 가진 문맥 뱅크 수.
	 * 설정자: 프로브가 능력 레지스터를 읽어 정한다.
	 * 읽는 자: 뱅크 번호가 범위 안인지 검사할 때.
	 * 값 범위: 1 ~ IOMMU_MAX_CBS.
	 * 동기화: 프로브 이후 불변. */
	int ncb;
	/* [한국어] 이 IOMMU 의 커널 장치.
	 * 설정자: 프로브.
	 * 읽는 자: 로그와 자원 관리.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. */
	struct device *dev;
	/* [한국어] 폴트 인터럽트 번호.
	 * 설정자: 프로브가 플랫폼 자원에서 얻는다.
	 * 읽는 자: 인터럽트를 걸 때.
	 * 값 범위: 0 이하면 인터럽트가 없다 — 폴트를 알 수 없게 된다.
	 * 동기화: 불변. */
	int irq;
	/* [한국어] 이 IOMMU 의 동작 클럭.
	 * 설정자: 프로브가 장치 트리에서 얻는다.
	 * 읽는 자: 레지스터를 만지기 전에 켜고, 끝나면 끈다 — 전력을 아끼려고
	 *         쓰지 않을 때는 꺼 두기 때문이다.
	 * 값 범위: NULL 이면 클럭 제어가 필요 없는 플랫폼이다.
	 * 동기화: 클럭 계층이 자체 참조 계수를 관리한다. */
	struct clk *clk;
	/* [한국어] 버스 연결부(interconnect)의 클럭.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 위 clk 와 함께 켜고 꺼야 레지스터에 닿는다.
	 * 동기화: 위와 같다. */
	struct clk *pclk;
	/* [한국어] 이 인스턴스를 전역 IOMMU 목록에 매다는 고리.
	 * 설정자: 프로브가 목록에 단다.
	 * 읽는 자: 장치를 붙일 때 어느 인스턴스인지 찾는 경로.
	 * 값 범위: 목록에서 빠지면 초기화된 상태.
	 * 동기화: 전역 목록 락 아래. */
	struct list_head dev_node;
	/* [한국어] 이 인스턴스를 도메인의 목록에 매다는 고리.
	 * 설정자: 이 인스턴스의 뱅크가 어느 도메인에 배정될 때.
	 * 읽는 자: 도메인이 무효화를 낼 때 어느 하드웨어를 건드릴지 정하는 경로.
	 * 값 범위: 한 인스턴스는 도메인 하나에만 매달린다.
	 * 동기화: 도메인 조작 락 아래. */
	struct list_head dom_node;
	/* [한국어] 이 인스턴스에 딸린 문맥 뱅크들의 목록.
	 * 설정자: 장치 트리의 자식 노드를 훑으며 뱅크마다 하나씩 단다.
	 * 읽는 자: MID 대응표를 채우거나 뱅크를 고를 때.
	 * 값 범위: 뱅크 수만큼.
	 * 동기화: 프로브 이후 목록 자체는 바뀌지 않는다. */
	struct list_head ctx_list;
	/* [한국어] 어느 뱅크가 이미 쓰이고 있는지 표시하는 비트맵.
	 * 설정자: 도메인에 뱅크를 배정하고 놓을 때.
	 * 읽는 자: 빈 뱅크를 고를 때 — 이 비트맵이 다 차면 더 이상 도메인을
	 *         만들 수 없다.
	 * 값 범위: IOMMU_MAX_CBS 비트.
	 * 동기화: 뱅크 배정 락 아래. */
	DECLARE_BITMAP(context_map, IOMMU_MAX_CBS);

	/* [한국어] iommu 코어가 아는 장치 몸통.
	 * 설정자: 프로브가 코어에 등록하며 채운다.
	 * 읽는 자: 코어가 이 드라이버를 부를 때의 기준점.
	 * 값 범위: 등록 전에는 비어 있다.
	 * 동기화: 코어의 규칙을 따른다. */
	struct iommu_device iommu;
};

/**
 * struct msm_iommu_ctx_dev - an IOMMU context bank instance
 * of_node	node ptr of client device
 * num		Index of this context bank within the hardware
 * mids		List of Machine IDs that are to be mapped into this context
 *		bank, terminated by -1. The MID is a set of signals on the
 *		AXI bus that identifies the function associated with a specific
 *		memory request. (See ARM spec).
 * num_mids	Total number of mids
 * node		list head in ctx_list
 */
/* [한국어] 문맥 뱅크 하나. (위 영어 kernel-doc 참고)
 *
 * 뱅크는 곧 주소 공간이고, 그 뱅크로 어떤 트랜잭션을 보낼지는 MID 목록이
 * 정한다. MID 는 AXI 버스의 신호 묶음으로 "이 메모리 요청이 어느 기능에서
 * 왔는가"를 알리는 값이며, 그 대응은 칩 설계 때 정해져 장치 트리에 적혀 온다. */
struct msm_iommu_ctx_dev {
	/* [한국어] 이 뱅크를 쓰는 클라이언트 장치의 트리 노드.
	 * 설정자: 장치 트리를 훑으며 채운다.
	 * 읽는 자: 어느 장치가 이 뱅크를 쓰는지 짝을 맞출 때.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 프로브 이후 불변. */
	struct device_node *of_node;
	/* [한국어] 하드웨어 안에서의 뱅크 번호.
	 * 설정자: 장치 트리가 알려 준 값.
	 * 읽는 자: 레지스터 주소를 계산할 때 — 뱅크마다 레지스터 묶음이 따로 있다.
	 * 값 범위: 0 ~ ncb-1.
	 * 동기화: 불변. */
	int num;
	/* [한국어] 이 뱅크로 보낼 MID 들의 목록.
	 * 설정자: 장치 트리가 알려 준 값들을 옮겨 담는다.
	 * 읽는 자: 하드웨어의 MID 대응표를 채울 때 하나씩 훑는다.
	 * 값 범위: -1 로 끝나는 목록. 최대 MAX_NUM_MIDS 개.
	 * 동기화: 불변 — 칩 설계 때 정해진 값이라 실행 중에 바뀌지 않는다. */
	int mids[MAX_NUM_MIDS];
	/* [한국어] 그 목록에 실제로 담긴 개수.
	 * 설정자: 장치 트리를 읽으며 센다.
	 * 읽는 자: 목록을 훑는 반복문의 한계.
	 * 값 범위: 0 ~ MAX_NUM_MIDS.
	 * 동기화: 불변. */
	int num_mids;
	/* [한국어] 이 뱅크를 인스턴스의 ctx_list 에 매다는 고리.
	 * 설정자: 프로브가 목록에 단다.
	 * 읽는 자: 인스턴스의 뱅크들을 훑는 경로.
	 * 값 범위: 목록에서 빠지면 초기화된 상태.
	 * 동기화: 프로브 이후 목록은 바뀌지 않는다.
	 *         (kernel-doc 의 이름은 node 지만 실제 필드 이름은 list 다.) */
	struct list_head list;
};

/*
 * Interrupt handler for the IOMMU context fault interrupt. Hooking the
 * interrupt is not supported in the API yet, but this will print an error
 * message and dump useful IOMMU registers.
 */
/*
 * [한국어]
 * msm_iommu_fault_handler - 문맥 폴트 인터럽트를 처리한다
 *
 * @irq: 인터럽트 번호.
 * @dev_id: 등록할 때 넘긴 IOMMU 포인터.
 * @return: 처리했으면 IRQ_HANDLED.
 *
 * (위 영어 주석 참고) 장치가 매핑되지 않은 주소를 건드리면 이 인터럽트가
 * 온다. 다만 이 드라이버는 폴트를 상위 계층으로 올려 페이지를 채우는
 * 통로를 아직 갖고 있지 않아, 오류 메시지와 진단에 쓸 레지스터 값을
 * 남기는 것이 전부다.
 *
 * 그래도 그 로그가 중요한 이유는, 드라이버의 DMA 버그를 잡는 가장 직접적인
 * 단서이기 때문이다 — 어느 주소를 건드리다 실패했는지가 거기 적힌다.
 *
 * 실행 컨텍스트: 인터럽트. 잠들지 않는다.
 *
 * 호출 체인:
 *   하드웨어 인터럽트 → [이 함수]
 */
irqreturn_t msm_iommu_fault_handler(int irq, void *dev_id);

#endif
