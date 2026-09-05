// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2006, Intel Corporation.
 *
 * Copyright (C) 2006-2008 Intel Corporation
 * Author: Ashok Raj <ashok.raj@intel.com>
 * Author: Shaohua Li <shaohua.li@intel.com>
 * Author: Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>
 *
 * This file implements early detection/parsing of Remapping Devices
 * reported to OS through BIOS via DMA remapping reporting (DMAR) ACPI
 * tables.
 *
 * These routines are used by both DMA-remapping and Interrupt-remapping
 */

/*
 * [한국어 설명] DMAR ACPI 표 파싱과 무효화 큐의 하부 구현 (intel/dmar.c)
 *
 * === 파일의 역할 ===
 * VT-d 드라이버가 하드웨어를 처음 발견하는 곳이자, 하드웨어와 실제로
 * 명령을 주고받는 곳이다. 위 영어 주석이 말하듯 "펌웨어가 ACPI DMAR 표로
 * 보고한 재매핑 장치를 부팅 초기에 찾아내고 해석하는" 것이 출발점이고,
 * 거기서 만들어진 struct intel_iommu 를 DMA 재매핑과 인터럽트 재매핑이
 * 함께 쓴다 — 두 기능이 같은 하드웨어 유닛을 공유하기 때문이다.
 * 크게 네 덩어리다.
 *   [1] ACPI DMAR 표 파싱: DRHD(유닛), RMRR(예약 메모리), ATSR, SATC, ANDD
 *       항목을 훑어 커널 자료구조로 만든다. 이 과정이 부팅 아주 초기에,
 *       메모리 할당기조차 제한적인 시점에 일어난다.
 *   [2] device scope 해석: 표가 "버스 x 의 슬롯 y" 처럼 경로로 지목한 장치를
 *       실제 struct device 로 연결한다. 그 장치가 아직 없으면 나중에
 *       핫플러그 알림으로 이어 붙인다.
 *   [3] 유닛 초기화: 레지스터를 매핑하고 능력을 읽어 struct intel_iommu 를
 *       채운다. 폴트 인터럽트도 여기서 건다.
 *   [4] 무효화 큐(QI): 서술자를 큐에 넣고 완료를 기다리는 구현. 이 파일에서
 *       가장 정교한 부분이며, 상위 계층(cache.c, pasid.c)의 모든 무효화가
 *       결국 여기로 모인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * VT-d 스택의 가장 아래층이다.
 *   펌웨어 ACPI DMAR 표 → [이 파일] → struct intel_iommu 목록
 *     → iommu.c(DMA 재매핑) 와 irq_remapping.c(인터럽트 재매핑) 가 함께 쓴다
 *   상위 계층의 무효화 → qi_flush_* → [이 파일의 qi_submit_sync] → 하드웨어
 * 부팅 순서상 이 파일이 가장 먼저 실행된다. dmar_table_init() 이 성공해야
 * intel_iommu_init() 이 유닛들을 세울 수 있다.
 * 실행 컨텍스트: 파싱은 __init(부팅 초기, 단일 스레드). 무효화 제출은
 * 인터럽트를 끈 문맥에서도 불리므로 raw 스핀락을 쓴다. 폴트 처리는
 * 인터럽트 핸들러다.
 *
 * === 타 모듈과의 연결 ===
 * - <linux/dmar.h>: 이 파일이 만들고 다른 파일이 쓰는 DRHD 목록과 순회 매크로.
 * - iommu.c: 파싱 결과를 받아 유닛을 세우고, RMRR/ATSR/SATC 항목을 정책
 *   판단에 쓴다. dmar_parse_one_* 콜백들이 그쪽에 구현되어 있다.
 * - irq_remapping.c: 같은 유닛 목록을 써서 인터럽트 재매핑을 설정한다.
 * - cache.c/pasid.c: 무효화를 qi_submit_sync 로 보낸다.
 * - perf.c: 무효화 지연을 잰다.
 * - ACPI 서브시스템: 표를 읽고 핫플러그 알림을 전달한다.
 * 데이터 흐름: 부팅 → DMAR 표 파싱 → 유닛 자료구조 생성 → 레지스터 매핑 →
 * 능력 읽기 → (iommu.c 가) 루트 테이블·무효화 큐 설정 → 이후 모든 무효화가
 * 이 파일의 큐를 통과한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - dmar_table_init(): 표 전체를 훑어 모든 항목을 파싱한다. 부팅의 시작점.
 * - dmar_parse_one_drhd(): 유닛 하나를 발견해 자료구조를 만든다.
 * - dmar_alloc_dev_scope()/dmar_insert_dev_scope(): 표의 장치 경로를 실제
 *   struct device 로 잇는다. 핫플러그로 나중에 이어지는 경로도 여기 있다.
 * - alloc_iommu()/free_iommu(): 유닛의 레지스터를 매핑하고 능력을 읽는다.
 * - dmar_enable_qi(): 무효화 큐를 만들고 하드웨어에 알린다.
 * - qi_submit_sync(): 이 파일의 핵심. 서술자를 큐에 넣고 완료를 기다리며,
 *   오류가 나면 어디까지 처리되었는지 판별해 다시 제출한다.
 * - qi_check_fault(): 큐 오류를 해석한다. 서술자 하나가 거부되면 그 뒤의
 *   것들도 무효가 되므로, 어디서부터 다시 보낼지 정하는 것이 관건이다.
 * - dmar_fault(): 폴트 인터럽트 핸들러. 폴트 기록을 훑어 로그를 남긴다.
 * - dmar_set_interrupt()/dmar_alloc_hwirq(): 유닛의 인터럽트를 건다.
 */
#define pr_fmt(fmt)     "DMAR: " fmt	/* [한국어] 이 파일의 모든 로그에 붙는 접두사 */

#include <linux/pci.h>	/* [한국어] 장치 경로를 실제 PCI 장치로 해석한다 */
#include <linux/dmar.h>	/* [한국어] DRHD 목록과 순회 매크로 — 이 파일이 채우고 다른 파일이 쓴다 */
#include <linux/iova.h>	/* [한국어] IOVA 할당기 타입 */
#include <linux/timer.h>	/* [한국어] 무효화 완료를 기다릴 때의 시간 측정 */
#include <linux/irq.h>	/* [한국어] 인터럽트 서술자 */
#include <linux/interrupt.h>	/* [한국어] 폴트 인터럽트 등록 */
#include <linux/tboot.h>	/* [한국어] TXT 측정 부팅 여부 확인 */
#include <linux/dmi.h>	/* [한국어] BIOS 벤더·버전 — 펌웨어 버그를 보고할 때 함께 남긴다 */
#include <linux/slab.h>	/* [한국어] 자료구조 할당 */
#include <linux/iommu.h>	/* [한국어] 코어 타입 */
#include <linux/numa.h>	/* [한국어] 유닛과 가까운 노드에서 테이블을 잡는다 */
#include <linux/limits.h>	/* [한국어] 정수 상한값 */
#include <asm/irq_remapping.h>	/* [한국어] 아키텍처별 인터럽트 재매핑 인터페이스 */

#include "iommu.h"	/* [한국어] struct intel_iommu, 레지스터 정의, 서술자 형식 */
#include "../irq_remapping.h"	/* [한국어] 인터럽트 재매핑 코어와의 접점 */
#include "../iommu-pages.h"	/* [한국어] 큐 버퍼를 잡는 공용 할당기 */
#include "perf.h"	/* [한국어] 무효화 지연 계측 */
#include "trace.h"	/* [한국어] 서술자 제출을 추적 이벤트로 남긴다 */
#include "perfmon.h"	/* [한국어] 유닛의 하드웨어 성능 카운터 */

typedef int (*dmar_res_handler_t)(struct acpi_dmar_header *, void *);	/* [한국어] DMAR 표의 항목 하나를 처리하는 콜백의 형태. 항목 종류마다 다른 함수를 꽂아 같은 순회 코드를 재사용한다 */
/*
 * [한국어] struct dmar_res_callback — DMAR 표 순회의 콜백 묶음
 *
 * DMAR 표에는 종류가 다른 항목(DRHD, RMRR, ATSR, SATC, ANDD)이 섞여 있고,
 * 그것을 훑는 코드는 하나(dmar_walk_remapping_entries)뿐이다. 항목 종류를
 * 인덱스로 삼는 콜백 배열을 넘겨, 같은 순회 코드가 상황마다 다르게 동작하게
 * 한다.
 *
 * 같은 표를 여러 목적으로 훑기 때문에 이 구조가 필요하다.
 *   - 부팅 시 파싱: 모든 종류에 파싱 함수를 꽂는다.
 *   - 핫플러그 삽입/제거: 그때 필요한 종류에만 함수를 꽂고 나머지는 NULL.
 *   - 검증만: 제거해도 되는지 묻는 check 함수들.
 * NULL 인 종류를 만났을 때 어떻게 할지는 ignore_unhandled 가 정한다.
 */
struct dmar_res_callback {
	dmar_res_handler_t	cb[ACPI_DMAR_TYPE_RESERVED];
	/* [한국어] 항목 종류를 인덱스로 하는 콜백 배열.
	 * 설정자: 순회를 시작하는 쪽이 필요한 종류에만 함수를 꽂는다.
	 * 읽는 자: dmar_walk_remapping_entries() 가 항목의 type 으로 색인해 부른다.
	 * 값 범위: NULL 인 자리는 "이 종류는 다루지 않는다"는 뜻이며, 그때의 동작은
	 *   아래 ignore_unhandled 가 정한다.
	 * 배열 크기가 ACPI_DMAR_TYPE_RESERVED 인 것은 그 값이 정의된 종류의 개수
	 *   다음 값이기 때문이다 — 종류 번호를 그대로 인덱스로 쓸 수 있다. */
	void			*arg[ACPI_DMAR_TYPE_RESERVED];
	/* [한국어] 각 콜백에 함께 넘길 인자.
	 * 설정자/읽는 자: cb 와 짝을 이룬다.
	 * 왜 종류마다 따로인가: 같은 순회에서 종류마다 다른 문맥이 필요할 수 있다.
	 *   예를 들어 핫플러그 삽입은 DRHD 콜백에는 유닛 정보를, ATSR 콜백에는
	 *   아무것도 넘기지 않는다. */
	bool			ignore_unhandled;
	/* [한국어] 콜백이 없는 종류를 만났을 때 그냥 넘어갈지, 오류로 볼지.
	 * 설정자: 순회를 시작하는 쪽.
	 * 읽는 자: dmar_walk_remapping_entries().
	 * 왜 두 방식이 다 필요한가: 부팅 파싱은 모르는 항목을 만나면 알려야 하지만
	 *   (펌웨어가 우리가 모르는 것을 보고했다는 뜻이다), 핫플러그처럼 특정
	 *   종류만 처리하는 순회에서는 나머지를 조용히 건너뛰어야 한다. */
	bool			print_entry;
	/* [한국어] 항목을 훑으며 그 내용을 로그에 찍을지.
	 * 설정자: 부팅 파싱에서만 참으로 둔다.
	 * 읽는 자: dmar_walk_remapping_entries().
	 * 왜 부팅에서만 찍는가: dmesg 에 남은 그 목록이 "이 시스템의 IOMMU 구성이
	 *   어떠했는가"를 알려 주는 유일한 기록인 경우가 많다. 핫플러그마다 다시
	 *   찍으면 로그만 지저분해진다. */
};

/*
 * Assumptions:
 * 1) The hotplug framework guarentees that DMAR unit will be hot-added
 *    before IO devices managed by that unit.
 * 2) The hotplug framework guarantees that DMAR unit will be hot-removed
 *    after IO devices managed by that unit.
 * 3) Hotplug events are rare.
 *
 * Locking rules for DMA and interrupt remapping related global data structures:
 * 1) Use dmar_global_lock in process context
 * 2) Use RCU in interrupt context
 */
DECLARE_RWSEM(dmar_global_lock);	/* [한국어] DMAR 전역 자료구조를 지키는 읽기/쓰기 세마포어. 위 영어 주석의 잠금 규칙 — 프로세스 컨텍스트에서는 이 락을, 인터럽트 컨텍스트에서는 RCU 를 쓴다 */
LIST_HEAD(dmar_drhd_units);	/* [한국어] 발견된 모든 DRHD 유닛의 목록. 이 파일이 채우고 iommu.c 와 irq_remapping.c 가 순회한다 */

struct acpi_table_header * __initdata dmar_tbl;	/* [한국어] 매핑된 DMAR 표. __initdata 라 부팅이 끝나면 해제된다 — 그래서 표 내용을 계속 참조할 항목(ATSR/SATC)은 사본을 뜬다 */
static int dmar_dev_scope_status = 1;	/* [한국어] device scope 연결의 결과. 1 은 "아직 시도하지 않음"이며, 성공하면 0 이 된다. 나중에 나타나는 장치를 표에 이어 붙일지 판단하는 근거다 */
static DEFINE_IDA(dmar_seq_ids);	/* [한국어] 유닛 순번 할당기. 이 번호가 "dmar0" 같은 이름과 도메인의 iommu_array 색인이 된다 */

static int alloc_iommu(struct dmar_drhd_unit *drhd);	/* [한국어] 전방 선언 — 유닛의 레지스터를 매핑하고 능력을 읽는다 */
static void free_iommu(struct intel_iommu *iommu);	/* [한국어] 그 반대 */

/*
 * [한국어]
 * dmar_register_drhd_unit - 발견한 DRHD 유닛을 전역 목록에 등록한다
 *
 * @drhd: 방금 파싱한 유닛.
 * @return: 없음.
 *
 * 넣는 위치가 이 함수의 전부다. include_all 유닛 — "이 세그먼트의 나머지
 * 장치를 모두 담당한다"고 신고한 유닛 — 은 목록의 꼬리에, 나머지는 앞에
 * 넣는다.
 *
 * 왜 그런가(코드 안 영어 주석): 장치를 담당할 유닛을 찾을 때 목록을 앞에서부터
 * 훑는데, 특정 장치를 지목한 유닛이 먼저 나와야 한다. include_all 이 앞에
 * 있으면 그것이 먼저 매치되어, 실제로 그 장치를 담당하는 유닛이 있는데도
 * 엉뚱한 유닛이 선택된다. 즉 "구체적인 것이 일반적인 것보다 먼저"라는
 * 규칙을 목록 순서로 구현한 것이다.
 *
 * list_add_rcu 를 쓰는 것은 이 목록이 인터럽트 문맥에서도 순회되기 때문이다.
 *
 * 실행 컨텍스트: 표 파싱(부팅) 또는 핫플러그. 프로세스 컨텍스트.
 */
static void dmar_register_drhd_unit(struct dmar_drhd_unit *drhd)
{
	/*
	 * add INCLUDE_ALL at the tail, so scan the list will find it at
	 * the very end.
	 */
	if (drhd->include_all)	/* [한국어] "나머지를 모두 담당한다"고 신고한 유닛이면 (위 영어 주석) */
		list_add_tail_rcu(&drhd->list, &dmar_drhd_units);	/* [한국어] 목록의 꼬리에 넣는다. 앞에 있으면 특정 장치를 지목한 유닛보다 먼저 매치되어 엉뚱한 유닛이 선택된다 */
	else
		list_add_rcu(&drhd->list, &dmar_drhd_units);	/* [한국어] 특정 장치를 지목한 유닛은 앞에. "구체적인 것이 일반적인 것보다 먼저"를 목록 순서로 구현한 것이다 */
}

/*
 * [한국어]
 * dmar_alloc_dev_scope - ACPI 항목 뒤에 이어진 device scope 배열을 파싱한다
 *
 * @start: device scope 가 시작되는 주소(항목 구조체 바로 뒤).
 * @end: 항목의 끝.
 * @cnt: 출력 — 지원하는 장치의 개수.
 * @return: 그만큼 잡은 dmar_dev_scope 배열, 장치가 없으면 NULL.
 *
 * DMAR 표의 여러 항목(DRHD, RMRR, ATSR, SATC)은 뒤에 "이 항목이 적용되는
 * 장치들"의 목록을 가변 길이로 붙인다. 이 함수가 그 목록을 훑는다.
 *
 * 두 번 훑는 구조다. 먼저 개수를 세고, 그만큼 한 번에 할당한다. 항목마다
 * 길이가 달라(scope->length) 미리 개수를 알 수 없기 때문이다.
 *
 * 세는 대상: 네임스페이스 장치, 엔드포인트, 브리지 셋만 센다. IOAPIC 과
 * HPET 은 인터럽트 재매핑 쪽에서 따로 다루므로 여기서는 건너뛰되 경고도
 * 내지 않는다. 그 밖의 종류는 우리가 모르는 것이라 경고를 남긴다 —
 * 펌웨어가 새 종류를 보고했거나 표가 손상되었다는 뜻이다.
 *
 * 이 배열은 처음에는 비어 있고(경로만 표에 있다), 그 경로의 장치가 실제로
 * 나타나면 dmar_insert_dev_scope 가 포인터를 채워 넣는다.
 *
 * 실행 컨텍스트: 표 파싱 또는 핫플러그. 프로세스 컨텍스트.
 */
void *dmar_alloc_dev_scope(void *start, void *end, int *cnt)
{
	struct acpi_dmar_device_scope *scope;	/* [한국어] 순회 커서 */

	*cnt = 0;	/* [한국어] 개수부터 센다 */
	while (start < end) {	/* [한국어] 항목의 끝까지 */
		scope = start;	/* [한국어] 현재 device scope 항목 */
		if (scope->entry_type == ACPI_DMAR_SCOPE_TYPE_NAMESPACE ||	/* [한국어] ACPI 네임스페이스 장치이거나 */
		    scope->entry_type == ACPI_DMAR_SCOPE_TYPE_ENDPOINT ||	/* [한국어] PCI 엔드포인트이거나 */
		    scope->entry_type == ACPI_DMAR_SCOPE_TYPE_BRIDGE)	/* [한국어] 브리지면 */
			(*cnt)++;	/* [한국어] 우리가 다루는 종류다 */
		else if (scope->entry_type != ACPI_DMAR_SCOPE_TYPE_IOAPIC &&	/* [한국어] IOAPIC 과 */
			scope->entry_type != ACPI_DMAR_SCOPE_TYPE_HPET) {	/* [한국어] HPET 은 인터럽트 재매핑 쪽에서 다루므로 조용히 건너뛰고, */
			pr_warn("Unsupported device scope\n");	/* [한국어] 그 밖의 종류는 경고한다 — 펌웨어가 우리가 모르는 것을 보고했다는 뜻이다 */
		}
		start += scope->length;	/* [한국어] 항목마다 길이가 달라 그만큼 전진한다 */
	}
	if (*cnt == 0)	/* [한국어] 지원하는 장치가 하나도 없으면 */
		return NULL;	/* [한국어] 배열을 만들지 않는다 */

	return kzalloc_objs(struct dmar_dev_scope, *cnt);	/* [한국어] 센 만큼 한 번에 잡는다. 처음에는 비어 있고, 그 경로의 장치가 나타나면 채워진다 */
}

/*
 * [한국어]
 * dmar_free_dev_scope - device scope 배열과 그것이 잡은 장치 참조를 반납한다
 *
 * @devices: 배열 포인터의 주소(NULL 로 되돌리기 위해 이중 포인터).
 * @cnt: 개수의 주소(0 으로 되돌린다).
 * @return: 없음.
 *
 * 배열만 해제하면 안 된다. 연결된 각 장치에 대해 get_device 로 참조를
 * 잡아 두었으므로, 그것을 먼저 놓아야 한다 — 그러지 않으면 그 장치들이
 * 영원히 해제되지 않는다.
 *
 * for_each_active_dev_scope 는 실제로 연결된(포인터가 채워진) 항목만
 * 훑는다. 경로만 있고 장치가 아직 나타나지 않은 자리는 참조도 없다.
 *
 * 이중 포인터로 받는 이유: 호출자의 변수를 NULL 과 0 으로 되돌려, 두 번
 * 해제되거나 해제된 배열을 순회하는 일이 없게 한다.
 *
 * 실행 컨텍스트: 항목 해제. 프로세스 컨텍스트.
 */
void dmar_free_dev_scope(struct dmar_dev_scope **devices, int *cnt)
{
	int i;	/* [한국어] 순회 인덱스 */
	struct device *tmp_dev;	/* [한국어] 순회 커서 */

	if (*devices && *cnt) {	/* [한국어] 배열이 있고 항목이 있으면 */
		for_each_active_dev_scope(*devices, *cnt, i, tmp_dev)	/* [한국어] 실제로 연결된 장치들에 대해 */
			put_device(tmp_dev);	/* [한국어] 잡아 두었던 참조를 놓는다. 그러지 않으면 그 장치들이 영원히 해제되지 않는다 */
		kfree(*devices);	/* [한국어] 그 다음 배열을 반납한다 */
	}

	*devices = NULL;	/* [한국어] 호출자의 변수를 되돌려 */
	*cnt = 0;	/* [한국어] 두 번 해제되거나 해제된 배열을 순회하는 일이 없게 한다 */
}

/* Optimize out kzalloc()/kfree() for normal cases */
static char dmar_pci_notify_info_buf[64];	/* [한국어] 흔한 크기의 알림 정보를 담을 정적 버퍼. 매번 kzalloc/kfree 하지 않으려는 최적화다 (위 영어 주석). PCI 계층 깊이가 얕은 대부분의 장치가 여기 들어간다 */

/*
 * [한국어]
 * dmar_alloc_pci_notify_info - PCI 장치 알림을 DMAR 표와 대조할 형태로 만든다
 *
 * @dev: 알림의 대상 장치. @event: BUS_NOTIFY_ADD_DEVICE 또는 REMOVED.
 * @return: 채워진 알림 정보, 이 장치를 다룰 수 없으면 NULL.
 *
 * 왜 경로가 필요한가: ACPI DMAR 표는 장치를 "세그먼트 s, 루트 버스 b 에서
 * 슬롯/함수를 몇 번 거쳐 도달"하는 경로로 지목한다. 그런데 커널이 받은
 * struct pci_dev 는 그 반대 방향(자신에서 부모로)만 알고 있다. 그래서
 * 루트까지 거슬러 올라가며 깊이를 세고, 그만큼의 배열에 역순으로 채운다 —
 * 표의 표현과 같은 방향이 되도록.
 *
 * 두 번 훑는 이유가 여기 있다. 먼저 깊이를 세어 배열 크기를 정하고
 * (struct_size), 그 다음 실제 경로를 채운다.
 *
 * 정적 버퍼를 먼저 시도하는 것이 최적화다(위 영어 주석). PCI 계층이 얕은
 * 대부분의 장치는 64바이트에 들어가므로 할당 없이 끝난다. 그래서 해제할
 * 때도 그 버퍼인지 확인해야 한다(dmar_free_pci_notify_info).
 *
 * 세그먼트 상한 검사: DMAR 표의 세그먼트 필드가 16비트라, 그보다 큰 도메인
 * 번호를 쓰는 장치(VMD 서브디바이스 등)는 애초에 표에서 찾을 수 없다
 * (코드 안 영어 주석). 조회를 시도할 이유가 없으므로 NULL 을 돌려준다.
 *
 * 제거 알림에는 경로를 만들지 않는다. 제거는 이미 연결된 포인터를 비교해
 * 찾으므로 경로가 필요 없기 때문이다.
 *
 * 할당 실패를 dmar_dev_scope_status 에 기록하는 이유: 이 알림 하나를
 * 놓치면 그 장치가 표에 영영 연결되지 않는다. 부팅 후반의 검증이 그 상태를
 * 알아야 한다.
 *
 * 실행 컨텍스트: PCI 버스 알림. 프로세스 컨텍스트.
 */
static struct dmar_pci_notify_info *
dmar_alloc_pci_notify_info(struct pci_dev *dev, unsigned long event)
{
	int level = 0;	/* [한국어] 루트까지의 계층 깊이 */
	size_t size;	/* [한국어] 필요한 구조체 크기 */
	struct pci_dev *tmp;	/* [한국어] 계층을 거슬러 올라가는 커서 */
	struct dmar_pci_notify_info *info;	/* [한국어] 만들 알림 정보 */

	/*
	 * Ignore devices that have a domain number higher than what can
	 * be looked up in DMAR, e.g. VMD subdevices with domain 0x10000
	 */
	if (pci_domain_nr(dev->bus) > U16_MAX)	/* [한국어] DMAR 표의 세그먼트 필드는 16비트라 그보다 큰 도메인은 애초에 찾을 수 없다 (위 영어 주석) */
		return NULL;	/* [한국어] 조회를 시도할 이유가 없다 */

	/* Only generate path[] for device addition event */
	if (event == BUS_NOTIFY_ADD_DEVICE)	/* [한국어] 장치 추가일 때만 경로가 필요하다 (위 영어 주석) */
		for (tmp = dev; tmp; tmp = tmp->bus->self)	/* [한국어] 루트까지 거슬러 올라가며 */
			level++;	/* [한국어] 깊이를 센다 */

	size = struct_size(info, path, level);	/* [한국어] 그 깊이만큼의 배열을 포함한 크기 */
	if (size <= sizeof(dmar_pci_notify_info_buf)) {	/* [한국어] 흔한 크기면 */
		info = (struct dmar_pci_notify_info *)dmar_pci_notify_info_buf;	/* [한국어] 정적 버퍼를 쓴다 — 할당 없이 끝난다 */
	} else {
		info = kzalloc(size, GFP_KERNEL);	/* [한국어] 계층이 깊으면 따로 잡는다 */
		if (!info) {	/* [한국어] 할당 실패 */
			if (dmar_dev_scope_status == 0)	/* [한국어] 아직 오류가 기록되지 않았으면 */
				dmar_dev_scope_status = -ENOMEM;	/* [한국어] 기록한다. 이 알림을 놓치면 그 장치가 표에 영영 연결되지 않으므로, 부팅 후반의 검증이 알아야 한다 */
			return NULL;	/* [한국어] 처리할 수 없다 */
		}
	}

	info->event = event;	/* [한국어] 추가인지 제거인지 */
	info->dev = dev;	/* [한국어] 대상 장치 */
	info->seg = pci_domain_nr(dev->bus);	/* [한국어] PCI 세그먼트 */
	info->level = level;	/* [한국어] 계층 깊이 */
	if (event == BUS_NOTIFY_ADD_DEVICE) {	/* [한국어] 추가일 때만 경로를 채운다 */
		for (tmp = dev; tmp; tmp = tmp->bus->self) {	/* [한국어] 다시 루트까지 거슬러 올라가며 */
			level--;	/* [한국어] 뒤에서부터 채운다 — 표는 루트에서 시작하는 순서로 적으므로 방향을 뒤집어야 한다 */
			info->path[level].bus = tmp->bus->number;	/* [한국어] 그 단계의 버스 번호 */
			info->path[level].device = PCI_SLOT(tmp->devfn);	/* [한국어] 슬롯 번호 */
			info->path[level].function = PCI_FUNC(tmp->devfn);	/* [한국어] 함수 번호 */
			if (pci_is_root_bus(tmp->bus))	/* [한국어] 루트 버스에 닿으면 */
				info->bus = tmp->bus->number;	/* [한국어] 그 번호를 따로 기억한다. 표의 항목도 루트 버스로 시작하므로 비교의 첫 조건이 된다 */
		}
	}

	return info;	/* [한국어] 표와 대조할 준비가 되었다 */
}

/*
 * [한국어]
 * dmar_free_pci_notify_info - 알림 정보를 반납한다(정적 버퍼면 아무것도 하지 않는다)
 *
 * @info: 반납할 정보.
 * @return: 없음.
 *
 * alloc 이 정적 버퍼를 쓸 수도 kzalloc 을 할 수도 있으므로, 포인터를 비교해
 * 어느 쪽인지 판별한다. 정적 버퍼를 kfree 하면 커널이 죽는다.
 *
 * 정적 버퍼가 하나뿐이라는 것은 이 경로가 동시에 두 번 실행되지 않는다는
 * 전제를 담고 있다 — PCI 버스 알림이 dmar_global_lock 아래에서 직렬화되기
 * 때문에 성립한다.
 *
 * 실행 컨텍스트: PCI 버스 알림. 프로세스 컨텍스트.
 */
static inline void dmar_free_pci_notify_info(struct dmar_pci_notify_info *info)
{
	if ((void *)info != dmar_pci_notify_info_buf)	/* [한국어] 정적 버퍼가 아니면 */
		kfree(info);	/* [한국어] 반납한다. 정적 버퍼를 kfree 하면 커널이 죽는다 */
}

/*
 * [한국어]
 * dmar_match_pci_path - 장치의 PCI 경로가 표에 적힌 경로와 같은지 본다
 *
 * @info: 장치의 경로 정보. @bus: 표가 적은 루트 버스 번호.
 * @path: 표가 적은 경로 배열. @count: 그 길이.
 * @return: true 면 같은 장치다.
 *
 * 정상 비교는 단순하다: 루트 버스가 같고, 깊이가 같고, 각 단계의
 * 슬롯·함수가 모두 같으면 같은 장치다.
 *
 * fallback 경로가 이 함수의 본론이다. 일부 BIOS 가 RMRR 항목을 잘못 적는다 —
 * 루트 버스에서 시작하는 전체 경로 대신, 장치가 실제로 붙어 있는 버스 번호와
 * 그 한 단계만 적는 것이다. 스펙 위반이지만 그런 시스템이 실제로 존재하고,
 * 그 항목을 무시하면 RMRR 이 적용되지 않아 장치가 동작하지 않는다.
 * 그래서 "경로 길이가 1 이고, 그 한 단계가 장치의 마지막 단계와 일치하면"
 * 같은 장치로 인정하고 FW_BUG 경고를 남긴다.
 *
 * count != 1 을 먼저 거르는 것이 중요하다. 이 우회는 "한 단계만 적힌" 경우에만
 * 적용해야 하며, 그러지 않으면 서로 다른 장치를 같다고 판정할 수 있다.
 *
 * 실행 컨텍스트: 표와 장치를 대조하는 경로. 순수 비교.
 */
static bool dmar_match_pci_path(struct dmar_pci_notify_info *info, int bus,
				struct acpi_dmar_pci_path *path, int count)
{
	int i;	/* [한국어] 경로 순회 */

	if (info->bus != bus)	/* [한국어] 루트 버스가 다르면 */
		goto fallback;	/* [한국어] 정상 비교로는 다른 장치다 */
	if (info->level != count)	/* [한국어] 계층 깊이가 다르면 */
		goto fallback;	/* [한국어] 역시 */

	for (i = 0; i < count; i++) {	/* [한국어] 각 단계를 비교한다 */
		if (path[i].device != info->path[i].device ||	/* [한국어] 슬롯이나 */
		    path[i].function != info->path[i].function)	/* [한국어] 함수가 다르면 */
			goto fallback;	/* [한국어] 다른 장치다 */
	}

	return true;	/* [한국어] 모든 단계가 일치 — 같은 장치다 */

fallback:	/* [한국어] 일부 BIOS 의 잘못된 RMRR 항목을 구제하는 경로 */

	if (count != 1)	/* [한국어] 표가 한 단계만 적은 경우에만 이 우회를 쓴다 */
		return false;	/* [한국어] 그 밖에는 정말 다른 장치다 */

	i = info->level - 1;	/* [한국어] 장치의 마지막 단계(자기 자신) */
	if (bus              == info->path[i].bus &&	/* [한국어] 표가 적은 버스와 */
	    path[0].device   == info->path[i].device &&	/* [한국어] 슬롯, */
	    path[0].function == info->path[i].function) {	/* [한국어] 함수가 그것과 일치하면 */
		pr_info(FW_BUG "RMRR entry for device %02x:%02x.%x is broken - applying workaround\n",	/* [한국어] 펌웨어가 루트에서 시작하는 전체 경로 대신 장치가 붙은 버스와 한 단계만 적은 것이다. 스펙 위반이지만 무시하면 그 장치가 동작하지 않는다 */
			bus, path[0].device, path[0].function);	/* [한국어] 문제의 항목 */
		return true;	/* [한국어] 같은 장치로 인정한다 */
	}

	return false;	/* [한국어] 정말 다른 장치다 */
}

/* Return: > 0 if match found, 0 if no match found, < 0 if error happens */
/*
 * [한국어] (위 영어 주석에 이어)
 * dmar_insert_dev_scope - 표의 device scope 항목에 실제 struct device 를 연결한다
 *
 * @info: 나타난 장치의 경로 정보.
 * @start, @end: 훑을 device scope 배열의 범위.
 * @segment: 이 항목의 PCI 세그먼트.
 * @devices: 연결할 자리들. @devices_cnt: 그 개수.
 * @return: 1 이면 연결했다, 0 이면 이 항목의 것이 아니다, 음수면 오류.
 *
 * ACPI 표는 장치를 경로로만 지목하므로, 그 경로의 장치가 실제로 나타난
 * 시점에 포인터를 채워야 한다. 이 함수가 그 연결을 한다.
 *
 * 반환값 셋을 구분하는 것이 호출자에게 중요하다(위 영어 주석). 1 이면 더
 * 볼 필요가 없어 순회를 멈추고, 0 이면 다음 항목을 계속 보고, 음수면
 * 알림 처리 전체를 실패시킨다.
 *
 * scope type 검증(코드 안 영어 주석)이 흥미롭다. 엔드포인트로 적힌 항목은
 * 일반 PCI 헤더를, 브리지로 적힌 항목은 브리지 헤더를 가진 장치와 맞아야
 * 한다. 그런데 PCI NTB(Non-Transparent Bridge) 장치는 브리지로 신고되면서
 * 일반 헤더를 갖는다. 그래서 클래스가 BRIDGE_OTHER(0680h)인 경우는 예외로
 * 두어, 정상적인 구성을 오류로 판정하지 않는다.
 *
 * 빈 자리를 찾아 채우는 부분: for_each_dev_scope 로 아직 NULL 인 자리를
 * 찾는다. rcu_assign_pointer 를 쓰는 것은 이 배열을 인터럽트 문맥에서
 * RCU 로 순회하기 때문이고, get_device 로 참조를 잡는 것은 그 장치가
 * 해제되어도 이 포인터가 살아 있어야 하기 때문이다.
 *
 * 빈 자리가 없으면 WARN 한다 — 표가 신고한 개수보다 많은 장치가 그 경로에
 * 매치되었다는 뜻이라, 파싱이나 표에 문제가 있다.
 *
 * 실행 컨텍스트: PCI 버스 알림 또는 부팅 후반의 연결. 프로세스 컨텍스트.
 */
int dmar_insert_dev_scope(struct dmar_pci_notify_info *info,
			  void *start, void*end, u16 segment,
			  struct dmar_dev_scope *devices,
			  int devices_cnt)
{
	int i, level;	/* [한국어] 순회 인덱스와 표의 경로 깊이 */
	struct device *tmp, *dev = &info->dev->dev;	/* [한국어] 빈 자리 순회 커서와, 연결할 장치 */
	struct acpi_dmar_device_scope *scope;	/* [한국어] 표의 device scope 항목 */
	struct acpi_dmar_pci_path *path;	/* [한국어] 그 뒤에 이어진 경로 배열 */

	if (segment != info->seg)	/* [한국어] 세그먼트가 다르면 */
		return 0;	/* [한국어] 이 항목의 장치가 아니다 */

	for (; start < end; start += scope->length) {	/* [한국어] device scope 항목들을 훑는다. 항목마다 길이가 달라 그만큼 전진한다 */
		scope = start;	/* [한국어] 현재 항목 */
		if (scope->entry_type != ACPI_DMAR_SCOPE_TYPE_ENDPOINT &&	/* [한국어] 엔드포인트도 */
		    scope->entry_type != ACPI_DMAR_SCOPE_TYPE_BRIDGE)	/* [한국어] 브리지도 아니면 */
			continue;	/* [한국어] PCI 장치가 아니므로 건너뛴다 */

		path = (struct acpi_dmar_pci_path *)(scope + 1);	/* [한국어] 경로는 항목 구조체 바로 뒤에 이어진다 */
		level = (scope->length - sizeof(*scope)) / sizeof(*path);	/* [한국어] 그 길이에서 경로 깊이를 계산한다 */
		if (!dmar_match_pci_path(info, scope->bus, path, level))	/* [한국어] 경로가 다르면 */
			continue;	/* [한국어] 다음 항목 */

		/*
		 * We expect devices with endpoint scope to have normal PCI
		 * headers, and devices with bridge scope to have bridge PCI
		 * headers.  However PCI NTB devices may be listed in the
		 * DMAR table with bridge scope, even though they have a
		 * normal PCI header.  NTB devices are identified by class
		 * "BRIDGE_OTHER" (0680h) - we don't declare a socpe mismatch
		 * for this special case.
		 */
		if ((scope->entry_type == ACPI_DMAR_SCOPE_TYPE_ENDPOINT &&	/* [한국어] 엔드포인트로 적혔는데 */
		     info->dev->hdr_type != PCI_HEADER_TYPE_NORMAL) ||	/* [한국어] 일반 PCI 헤더가 아니거나 (위 영어 주석) */
		    (scope->entry_type == ACPI_DMAR_SCOPE_TYPE_BRIDGE &&	/* [한국어] 브리지로 적혔는데 */
		     (info->dev->hdr_type == PCI_HEADER_TYPE_NORMAL &&	/* [한국어] 일반 헤더이면서 */
		      info->dev->class >> 16 != PCI_BASE_CLASS_BRIDGE))) {	/* [한국어] 브리지 클래스도 아니면 — PCI NTB 장치는 이 예외에 걸리지 않도록 클래스로 구제한다 */
			pr_warn("Device scope type does not match for %s\n",	/* [한국어] 표와 실제 장치의 종류가 어긋난다 */
				pci_name(info->dev));	/* [한국어] 어느 장치인지 */
			return -EINVAL;	/* [한국어] 알림 처리를 실패시킨다 */
		}

		for_each_dev_scope(devices, devices_cnt, i, tmp)	/* [한국어] 빈 자리를 찾는다 */
			if (tmp == NULL) {	/* [한국어] 아직 연결되지 않은 자리면 */
				devices[i].bus = info->dev->bus->number;	/* [한국어] 그 장치의 버스와 */
				devices[i].devfn = info->dev->devfn;	/* [한국어] devfn 을 기록하고 */
				rcu_assign_pointer(devices[i].dev,	/* [한국어] 포인터를 연결한다. RCU 로 배열을 순회하는 쪽이 있어 assign 매크로를 쓴다 */
						   get_device(dev));	/* [한국어] 참조를 잡는다 — 그 장치가 해제되어도 이 포인터가 살아 있어야 한다 */
				return 1;	/* [한국어] 연결 완료. 호출자는 더 볼 필요가 없다 */
			}
		if (WARN_ON(i >= devices_cnt))	/* [한국어] 빈 자리가 없으면 표가 신고한 개수보다 많은 장치가 매치된 것이다 */
			return -EINVAL;	/* [한국어] 파싱이나 표에 문제가 있다 */
	}

	return 0;	/* [한국어] 이 항목들 중에 이 장치는 없었다 */
}

/*
 * [한국어]
 * dmar_remove_dev_scope - 사라진 장치를 device scope 에서 끊는다
 *
 * @info: 사라진 장치의 알림 정보. @segment: 이 항목의 세그먼트.
 * @devices: 끊을 자리들. @count: 그 개수.
 * @return: 1 이면 끊었다, 0 이면 이 항목에 없었다.
 *
 * insert 의 반대이며, 순서가 이 함수의 전부다.
 *   1) RCU_INIT_POINTER 로 포인터를 NULL 로 만든다. 이 시점 이후 새로
 *      시작하는 순회는 이 자리를 건너뛴다.
 *   2) synchronize_rcu 로 이미 진행 중인 순회가 끝나기를 기다린다.
 *   3) 그제서야 참조를 놓는다.
 * 이 순서를 어기면, RCU 순회 중인 다른 CPU 가 해제된 struct device 를 읽는다.
 *
 * 경로가 아니라 포인터를 직접 비교하는 것이 insert 와 다른 점이다 —
 * 이미 연결된 것을 찾는 것이므로 경로를 다시 대조할 필요가 없다.
 *
 * 실행 컨텍스트: PCI 버스 알림. synchronize_rcu 를 부르므로 잠들 수 있는
 * 문맥이어야 한다.
 */
int dmar_remove_dev_scope(struct dmar_pci_notify_info *info, u16 segment,
			  struct dmar_dev_scope *devices, int count)
{
	int index;	/* [한국어] 순회 인덱스 */
	struct device *tmp;	/* [한국어] 순회 커서 */

	if (info->seg != segment)	/* [한국어] 세그먼트가 다르면 */
		return 0;	/* [한국어] 이 항목의 장치가 아니다 */

	for_each_active_dev_scope(devices, count, index, tmp)	/* [한국어] 연결된 장치들을 훑으며 */
		if (tmp == &info->dev->dev) {	/* [한국어] 사라진 장치를 찾으면 */
			RCU_INIT_POINTER(devices[index].dev, NULL);	/* [한국어] 먼저 포인터를 끊는다. 이 시점 이후 새 순회는 이 자리를 건너뛴다 */
			synchronize_rcu();	/* [한국어] 이미 진행 중인 순회가 끝나기를 기다린다 */
			put_device(tmp);	/* [한국어] 그제서야 참조를 놓는다. 순서를 어기면 RCU 순회 중인 CPU 가 해제된 장치를 읽는다 */
			return 1;	/* [한국어] 끊었다 */
		}

	return 0;	/* [한국어] 이 항목에는 없었다 */
}

/*
 * [한국어]
 * dmar_pci_bus_add_dev - 새로 나타난 PCI 장치를 DMAR 표의 모든 항목에 연결한다
 *
 * @info: 그 장치의 경로 정보.
 * @return: 0 이상이면 성공, 음수면 실패.
 *
 * 장치 하나가 나타나면 여러 표 항목이 그것을 지목하고 있을 수 있다.
 * 그래서 세 단계로 훑는다.
 *   1) DRHD 항목들 — 어느 유닛이 이 장치를 담당하는지. include_all 유닛은
 *      건너뛴다(장치 목록이 없다).
 *   2) RMRR/ATSR/SATC — dmar_iommu_notify_scope_dev() 가 iommu.c 에서
 *      그 셋을 훑는다.
 *   3) 인터럽트 재매핑 — intel_irq_remap_add_device().
 *
 * DRHD 순회에서 성공(ret > 0)해도 break 하는 것을 눈여겨볼 것: 한 장치는
 * 하나의 유닛에만 속하므로 찾으면 더 볼 필요가 없다.
 *
 * dmar_dev_scope_status 에 오류를 기록하는 이유: 이 연결이 실패하면 그
 * 장치가 표에 반영되지 않은 채 남는다. 부팅 후반의 dmar_dev_scope_init 이
 * 그 상태를 보고 초기화를 실패시킨다 — 조용히 넘어가면 그 장치의 RMRR 이나
 * ATS 설정이 빠진 채 시스템이 동작하게 된다.
 *
 * 실행 컨텍스트: PCI 버스 알림. dmar_global_lock 쓰기 락 아래.
 */
static int dmar_pci_bus_add_dev(struct dmar_pci_notify_info *info)
{
	int ret = 0;	/* [한국어] 각 단계의 결과 */
	struct dmar_drhd_unit *dmaru;	/* [한국어] 유닛 순회 커서 */
	struct acpi_dmar_hardware_unit *drhd;	/* [한국어] 그 유닛의 원본 ACPI 항목 */

	for_each_drhd_unit(dmaru) {	/* [한국어] 등록된 유닛들을 훑으며 */
		if (dmaru->include_all)	/* [한국어] "나머지를 모두 담당한다"는 유닛은 */
			continue;	/* [한국어] 장치 목록 자체가 없으므로 건너뛴다 */

		drhd = container_of(dmaru->hdr,	/* [한국어] 보관된 ACPI 항목으로 */
				    struct acpi_dmar_hardware_unit, header);	/* [한국어] DRHD 형식으로 해석 */
		ret = dmar_insert_dev_scope(info, (void *)(drhd + 1),	/* [한국어] 항목 뒤에 이어진 device scope 에서 이 장치를 찾아 연결한다 */
				((void *)drhd) + drhd->header.length,	/* [한국어] 항목의 끝까지 */
				dmaru->segment,	/* [한국어] 이 유닛의 세그먼트 */
				dmaru->devices, dmaru->devices_cnt);	/* [한국어] 연결할 자리들 */
		if (ret)	/* [한국어] 연결했거나(1) 오류가 났으면(<0) */
			break;	/* [한국어] 한 장치는 하나의 유닛에만 속하므로 더 볼 필요가 없다 */
	}
	if (ret >= 0)	/* [한국어] DRHD 처리가 실패하지 않았으면 */
		ret = dmar_iommu_notify_scope_dev(info);	/* [한국어] RMRR/ATSR/SATC 항목에도 연결한다 */
	if (ret < 0 && dmar_dev_scope_status == 0)	/* [한국어] 오류가 났고 아직 기록되지 않았으면 */
		dmar_dev_scope_status = ret;	/* [한국어] 기록한다. 이 장치가 표에 반영되지 않은 채 남으면 그 RMRR 이나 ATS 설정이 빠진 상태로 동작하게 되므로, 부팅 후반의 검증이 알아야 한다 */

	if (ret >= 0)	/* [한국어] 여기까지 성공했으면 */
		intel_irq_remap_add_device(info);	/* [한국어] 인터럽트 재매핑에도 등록한다 */

	return ret;	/* [한국어] 결과 */
}

/*
 * [한국어]
 * dmar_pci_bus_del_dev - 사라진 PCI 장치를 표의 모든 항목에서 끊는다
 *
 * @info: 그 장치의 알림 정보.
 * @return: 없음.
 *
 * add 의 반대이지만 훨씬 단순하다. 실패할 것이 없고(포인터를 NULL 로
 * 만드는 일뿐이다), 인터럽트 재매핑 쪽은 별도 경로로 정리된다.
 *
 * DRHD 순회에서 찾으면 break 하는 것은 add 와 같은 이유다 — 한 장치는
 * 하나의 유닛에만 연결되어 있다.
 *
 * 이 함수가 반드시 불려야 하는 이유: 끊지 않으면 표의 device scope 가
 * 해제된 struct device 를 가리킨 채 남는다. 나중에 그 항목을 훑는 코드가
 * use-after-free 를 일으킨다.
 *
 * 실행 컨텍스트: PCI 버스 알림. dmar_global_lock 쓰기 락 아래.
 */
static void  dmar_pci_bus_del_dev(struct dmar_pci_notify_info *info)
{
	struct dmar_drhd_unit *dmaru;	/* [한국어] 유닛 순회 커서 */

	for_each_drhd_unit(dmaru)	/* [한국어] 등록된 유닛들을 훑으며 */
		if (dmar_remove_dev_scope(info, dmaru->segment,	/* [한국어] 이 장치가 연결되어 있으면 끊는다 */
			dmaru->devices, dmaru->devices_cnt))	/* [한국어] 그 유닛의 장치 목록에서 */
			break;	/* [한국어] 한 유닛에만 연결되어 있으므로 더 볼 필요가 없다 */
	dmar_iommu_notify_scope_dev(info);	/* [한국어] RMRR/ATSR/SATC 에서도 끊는다. 끊지 않으면 해제된 struct device 를 가리킨 채 남아 나중에 use-after-free 가 된다 */
}

/*
 * [한국어]
 * vf_inherit_msi_domain - SR-IOV 가상 함수가 물리 함수의 인터럽트 도메인을 물려받게 한다
 *
 * @pdev: 가상 함수(VF) 장치.
 * @return: 없음.
 *
 * 인터럽트 도메인은 "이 장치의 인터럽트를 누가 할당하고 재매핑하는가"를
 * 정한다. VF 는 자기 DMAR 유닛 정보를 갖지 않고 PF 의 것을 따르므로,
 * 인터럽트 도메인도 PF 의 것을 그대로 써야 한다.
 *
 * 왜 버스에서 물려받지 않는가(호출부의 영어 주석): 보통은 장치가 버스에서
 * 도메인을 물려받는 것이 자연스럽다. 그런데 DMAR 은 한 버스에 여러 유닛이
 * 있을 수 있어 버스 하나에 도메인 하나를 붙일 수 없다. VF 의 가상 버스가
 * PF 에서 물려받게 하는 방법도 있지만, 그것은 x86 특유의 사정을 다른
 * 아키텍처에까지 강요하는 셈이라 이렇게 장치 단위로 복사한다.
 *
 * 실행 컨텍스트: PCI 버스 알림(VF 추가). 프로세스 컨텍스트.
 */
static inline void vf_inherit_msi_domain(struct pci_dev *pdev)
{
	struct pci_dev *physfn = pci_physfn(pdev);	/* [한국어] 이 VF 의 물리 함수 */

	dev_set_msi_domain(&pdev->dev, dev_get_msi_domain(&physfn->dev));	/* [한국어] PF 의 인터럽트 도메인을 그대로 복사한다. VF 는 자기 DMAR 정보를 갖지 않고 PF 를 통해 조회되므로 인터럽트 도메인도 PF 의 것을 써야 한다 */
}

/*
 * [한국어]
 * dmar_pci_bus_notifier - PCI 장치 추가/제거 알림을 받아 DMAR 표를 갱신한다
 *
 * @nb: 등록해 둔 알림 블록. @action: 어떤 이벤트인지. @data: 대상 PCI 장치.
 * @return: NOTIFY_OK 또는 NOTIFY_DONE.
 *
 * ACPI DMAR 표는 부팅 시점의 정적인 기록이지만, PCI 장치는 나중에 나타나고
 * 사라진다. 이 콜백이 그 간극을 메운다 — 표의 경로와 실제 장치를 잇고 끊는
 * 모든 일의 출발점이다.
 *
 * VF 를 특별 취급하는 것이 첫 분기다(위 영어 주석). VF 는 자기 DMAR 정보를
 * 갖지 않고 device_to_iommu() 가 PF 를 통해 조회하므로, 표에 연결할 것이
 * 없다. 다만 인터럽트 도메인만은 PF 의 것을 물려받아야 해서 그것만 하고
 * 돌아간다.
 *
 * 추가와 제거 외의 이벤트는 무시한다. 이 파일이 관심 있는 것은 장치의
 * 존재 여부뿐이다.
 *
 * dmar_global_lock 을 쓰기 모드로 잡는 이유: 표의 device scope 배열을
 * 고치기 때문이다. 읽기 쪽(인터럽트 문맥)은 RCU 로 보호되므로 이 락과
 * 겹치지 않는다.
 *
 * 실행 컨텍스트: PCI 버스 알림 체인. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PCI 장치 등록/해제 → 버스 알림 → [dmar_pci_bus_notifier]
 *     → dmar_pci_bus_add_dev()/del_dev()
 */
static int dmar_pci_bus_notifier(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct pci_dev *pdev = to_pci_dev(data);	/* [한국어] 알림의 대상 장치 */
	struct dmar_pci_notify_info *info;	/* [한국어] 표와 대조할 경로 정보 */

	/* Only care about add/remove events for physical functions.
	 * For VFs we actually do the lookup based on the corresponding
	 * PF in device_to_iommu() anyway. */
	if (pdev->is_virtfn) {	/* [한국어] 가상 함수면 (위 영어 주석) */
		/*
		 * Ensure that the VF device inherits the irq domain of the
		 * PF device. Ideally the device would inherit the domain
		 * from the bus, but DMAR can have multiple units per bus
		 * which makes this impossible. The VF 'bus' could inherit
		 * from the PF device, but that's yet another x86'sism to
		 * inflict on everybody else.
		 */
		if (action == BUS_NOTIFY_ADD_DEVICE)	/* [한국어] 추가 알림일 때만 */
			vf_inherit_msi_domain(pdev);	/* [한국어] 인터럽트 도메인을 PF 에서 물려받는다. VF 는 표에 연결할 것이 없어 이것만 하고 돌아간다 */
		return NOTIFY_DONE;	/* [한국어] 더 처리하지 않는다 */
	}

	if (action != BUS_NOTIFY_ADD_DEVICE &&	/* [한국어] 추가도 */
	    action != BUS_NOTIFY_REMOVED_DEVICE)	/* [한국어] 제거도 아니면 */
		return NOTIFY_DONE;	/* [한국어] 이 파일이 관심 있는 것은 장치의 존재 여부뿐이다 */

	info = dmar_alloc_pci_notify_info(pdev, action);	/* [한국어] 표와 대조할 경로 정보를 만든다 */
	if (!info)	/* [한국어] 만들 수 없으면(세그먼트가 범위 밖이거나 할당 실패) */
		return NOTIFY_DONE;	/* [한국어] 처리하지 않는다 */

	down_write(&dmar_global_lock);	/* [한국어] 표의 device scope 배열을 고치므로 쓰기 락 */
	if (action == BUS_NOTIFY_ADD_DEVICE)	/* [한국어] 장치가 나타났으면 */
		dmar_pci_bus_add_dev(info);	/* [한국어] 표의 모든 항목에 연결한다 */
	else if (action == BUS_NOTIFY_REMOVED_DEVICE)	/* [한국어] 사라졌으면 */
		dmar_pci_bus_del_dev(info);	/* [한국어] 모든 항목에서 끊는다 */
	up_write(&dmar_global_lock);	/* [한국어] 락 해제 */

	dmar_free_pci_notify_info(info);	/* [한국어] 정적 버퍼가 아니었으면 반납한다 */

	return NOTIFY_OK;	/* [한국어] 처리했다 */
}

static struct notifier_block dmar_pci_bus_nb = {	/* [한국어] PCI 버스 알림 체인에 등록할 블록 */
	.notifier_call = dmar_pci_bus_notifier,	/* [한국어] 알림이 오면 부를 함수 */
	.priority = 1,	/* [한국어] 우선순위. 0 보다 높아 다른 소비자보다 먼저 불린다 — 장치가 IOMMU 아래에 들어간 뒤에야 다른 코드가 그것을 쓸 수 있어야 하기 때문이다 */
};

/*
 * [한국어]
 * dmar_find_dmaru - 이미 등록된 DRHD 유닛 중 같은 것을 찾는다
 *
 * @drhd: 방금 파싱한 ACPI DRHD 항목.
 * @return: 같은 유닛이 이미 있으면 그 자료구조, 없으면 NULL.
 *
 * 유닛의 정체는 (세그먼트, 레지스터 기저 주소) 한 쌍으로 정해진다. 그
 * 조합이 같으면 같은 하드웨어다 — 표의 다른 필드가 달라 보여도 마찬가지다.
 *
 * 왜 중복 검사가 필요한가: 핫플러그로 같은 표를 다시 파싱하는 경우가 있다.
 * 그대로 등록하면 같은 유닛이 목록에 두 번 들어가고, 그 유닛에 두 번
 * 초기화를 시도하게 된다.
 *
 * 동기화: dmar_drhd_units 는 RCU 목록이며 dmar_rcu_check() 가 유효한 보호
 * 조건(rcu_read_lock 안이거나 dmar_global_lock 을 쥐었음)을 lockdep 에 알린다.
 *
 * 실행 컨텍스트: 표 파싱 또는 핫플러그. 프로세스 컨텍스트.
 */
static struct dmar_drhd_unit *
dmar_find_dmaru(struct acpi_dmar_hardware_unit *drhd)
{
	struct dmar_drhd_unit *dmaru;	/* [한국어] 순회 커서 */

	list_for_each_entry_rcu(dmaru, &dmar_drhd_units, list,	/* [한국어] 등록된 유닛들을 RCU 순회 */
				dmar_rcu_check())	/* [한국어] 유효한 보호 조건임을 lockdep 에 알린다 */
		if (dmaru->segment == drhd->segment &&	/* [한국어] 세그먼트가 같고 */
		    dmaru->reg_base_addr == drhd->address)	/* [한국어] 레지스터 기저 주소도 같으면 */
			return dmaru;	/* [한국어] 같은 하드웨어다. 이 한 쌍이 유닛의 정체를 정한다 */

	return NULL;	/* [한국어] 등록된 적이 없다 */
}

/*
 * dmar_parse_one_drhd - parses exactly one DMA remapping hardware definition
 * structure which uniquely represent one DMA remapping hardware unit
 * present in the platform
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * dmar_parse_one_drhd - DRHD 항목 하나를 유닛 자료구조로 만들고 초기화한다
 *
 * @header: DMAR 표 안의 DRHD 항목.
 * @arg: NULL 이 아니면 처리한 개수를 세는 카운터.
 * @return: 0 성공(중복이라 아무것도 안 한 경우 포함), 음수면 실패.
 *
 * VT-d 하드웨어 하나를 커널이 처음 인식하는 지점이다. 여기서 만들어진
 * struct intel_iommu 를 DMA 재매핑과 인터럽트 재매핑이 함께 쓴다.
 *
 * 순서:
 *   1) 이미 등록된 유닛인지 확인한다(핫플러그로 같은 표를 다시 파싱할 수 있다).
 *   2) 자료구조와 ACPI 항목 사본을 한 번에 잡는다. _DSM 이 준 버퍼는 반환
 *      즉시 해제되므로 참조만 들고 있을 수 없다(코드 안 영어 주석).
 *   3) 레지스터 주소·크기·세그먼트를 옮긴다. 크기가 2^(size+12) 인 것은
 *      필드가 "4KB 페이지 수의 지수"이기 때문이다.
 *   4) device scope 를 파싱한다.
 *   5) alloc_iommu 로 실제 하드웨어를 만진다 — 레지스터를 매핑하고 능력을
 *      읽는다. 여기서 실패하면 그 유닛은 쓸 수 없다.
 *   6) 목록에 등록한다. include_all 여부에 따라 위치가 갈린다.
 *
 * include_all 플래그: DRHD 의 flags 비트 0 으로, "이 세그먼트에서 다른
 * 유닛이 담당하지 않는 나머지 장치를 모두 담당한다"는 뜻이다. 이 유닛이
 * 목록의 꼬리로 가는 이유가 그것이다.
 *
 * arg 카운터: 표를 훑으며 유닛이 몇 개나 있었는지 세는 데 쓴다. 하나도
 * 없으면 VT-d 를 쓸 수 없다는 판단의 근거가 된다.
 *
 * 실행 컨텍스트: 표 파싱(부팅) 또는 핫플러그. 프로세스 컨텍스트.
 */
static int dmar_parse_one_drhd(struct acpi_dmar_header *header, void *arg)
{
	struct acpi_dmar_hardware_unit *drhd;	/* [한국어] ACPI 항목 */
	struct dmar_drhd_unit *dmaru;	/* [한국어] 만들 커널 자료구조 */
	int ret;	/* [한국어] 결과 */

	drhd = (struct acpi_dmar_hardware_unit *)header;	/* [한국어] 헤더를 DRHD 항목으로 */
	dmaru = dmar_find_dmaru(drhd);	/* [한국어] 이미 등록된 유닛인지 */
	if (dmaru)	/* [한국어] 있으면 */
		goto out;	/* [한국어] 중복 등록하지 않는다. 핫플러그로 같은 표를 다시 파싱할 수 있다 */

	dmaru = kzalloc(sizeof(*dmaru) + header->length, GFP_KERNEL);	/* [한국어] 자료구조와 ACPI 사본 자리를 한 번에 잡는다 */
	if (!dmaru)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 이 유닛을 쓸 수 없다 */

	/*
	 * If header is allocated from slab by ACPI _DSM method, we need to
	 * copy the content because the memory buffer will be freed on return.
	 */
	dmaru->hdr = (void *)(dmaru + 1);	/* [한국어] 사본 자리는 구조체 바로 뒤 */
	memcpy(dmaru->hdr, header, header->length);	/* [한국어] 내용을 복사한다. ACPI _DSM 이 준 버퍼는 반환 즉시 해제되므로 참조만 들고 있을 수 없다 (위 영어 주석) */
	dmaru->reg_base_addr = drhd->address;	/* [한국어] 유닛 레지스터의 물리 주소. 세그먼트와 함께 이 유닛의 정체를 이룬다 */
	dmaru->segment = drhd->segment;	/* [한국어] PCI 세그먼트 */
	/* The size of the register set is 2 ^ N 4 KB pages. */
	dmaru->reg_size = 1UL << (drhd->size + 12);	/* [한국어] 레지스터 영역의 크기. 필드가 "4KB 페이지 수의 지수"라 +12 로 바이트가 된다 (위 영어 주석) */
	dmaru->include_all = drhd->flags & 0x1; /* BIT0: INCLUDE_ALL */	/* [한국어] 비트 0 이 INCLUDE_ALL — 다른 유닛이 담당하지 않는 나머지 장치를 모두 맡는다는 뜻이다 (위 영어 주석) */
	dmaru->devices = dmar_alloc_dev_scope((void *)(drhd + 1),	/* [한국어] 항목 뒤에 이어진 장치 목록을 파싱한다 */
					      ((void *)drhd) + drhd->header.length,	/* [한국어] 항목의 끝까지 */
					      &dmaru->devices_cnt);	/* [한국어] 개수를 받는다 */
	if (dmaru->devices_cnt && dmaru->devices == NULL) {	/* [한국어] 장치가 있다고 했는데 파싱에 실패했으면 */
		kfree(dmaru);	/* [한국어] 자료구조를 반납하고 */
		return -ENOMEM;	/* [한국어] 실패 */
	}

	ret = alloc_iommu(dmaru);	/* [한국어] 실제 하드웨어를 만진다 — 레지스터를 매핑하고 능력을 읽는다 */
	if (ret) {	/* [한국어] 실패하면 */
		dmar_free_dev_scope(&dmaru->devices,	/* [한국어] 장치 목록을 반납하고 */
				    &dmaru->devices_cnt);	/* [한국어] 개수도 되돌린다 */
		kfree(dmaru);	/* [한국어] 자료구조 반납 */
		return ret;	/* [한국어] 이 유닛은 쓸 수 없다 */
	}
	dmar_register_drhd_unit(dmaru);	/* [한국어] 전역 목록에 등록한다. include_all 여부에 따라 위치가 갈린다 */

out:	/* [한국어] 중복이었던 경우가 합류 */
	if (arg)	/* [한국어] 카운터를 넘겼으면 */
		(*(int *)arg)++;	/* [한국어] 처리한 유닛 수를 센다. 하나도 없으면 VT-d 를 쓸 수 없다는 판단의 근거가 된다 */

	return 0;	/* [한국어] 처리 완료 */
}

/*
 * [한국어]
 * dmar_free_drhd - 유닛 자료구조와 그것이 잡은 자원을 모두 반납한다
 *
 * @dmaru: 반납할 유닛.
 * @return: 없음.
 *
 * 세 가지를 순서대로 놓는다: device scope 배열(과 그것이 잡은 장치 참조),
 * struct intel_iommu(레지스터 매핑과 순번 포함), 그리고 자료구조 자신.
 * ACPI 항목 사본은 자료구조와 같은 할당 안에 있어 별도 해제가 없다.
 *
 * 각각을 확인하고 해제하는 것은 파싱이 중간에 실패했을 수도 있기 때문이다 —
 * device scope 는 만들었지만 alloc_iommu 가 실패한 경우 등.
 *
 * 실행 컨텍스트: 파싱 실패 정리 또는 유닛 제거. 프로세스 컨텍스트.
 */
static void dmar_free_drhd(struct dmar_drhd_unit *dmaru)
{
	if (dmaru->devices && dmaru->devices_cnt)	/* [한국어] 장치 목록이 있으면 */
		dmar_free_dev_scope(&dmaru->devices, &dmaru->devices_cnt);	/* [한국어] 그것과 잡아 둔 장치 참조를 놓는다 */
	if (dmaru->iommu)	/* [한국어] 유닛 구조체가 만들어졌으면 */
		free_iommu(dmaru->iommu);	/* [한국어] 레지스터 매핑과 순번을 반납한다 */
	kfree(dmaru);	/* [한국어] 자료구조 자신. ACPI 사본은 같은 할당 안에 있어 별도 해제가 없다 */
}

/*
 * [한국어]
 * dmar_parse_one_andd - ANDD 항목(ACPI 네임스페이스 장치 선언)을 검증하고 기록한다
 *
 * @header: ANDD 항목. @arg: 쓰지 않는다.
 * @return: 0 성공, -EINVAL 이면 펌웨어가 이름을 잘못 적었다.
 *
 * ANDD(ACPI Name-space Device Declaration)는 PCI 가 아닌 장치 — SoC 내부의
 * 가속기 등 — 를 ACPI 이름으로 지목하는 항목이다. 그 이름으로 나중에
 * probe_acpi_namespace_devices() 가 실제 장치를 찾는다.
 *
 * 이 함수가 하는 일은 사실상 검증 하나다: 이름 문자열이 항목 길이 안에서
 * NUL 로 끝나는지. 끝나지 않으면 그 이름을 문자열로 다루는 순간 항목 밖의
 * 메모리를 읽게 되므로, 그 항목을 거부하고 커널을 오염 표시한다.
 *
 * header->length - 8 이 이름 필드의 최대 길이다 — 앞의 8바이트가 헤더와
 * device_number 필드이기 때문이다. strnlen 이 그 길이를 그대로 돌려주면
 * NUL 이 없었다는 뜻이다.
 *
 * BIOS 벤더·버전을 함께 찍는 이유는 이 파일의 다른 FW_BUG 들과 같다 —
 * 이후의 버그 리포트에서 펌웨어를 의심할 근거를 남긴다.
 *
 * 실행 컨텍스트: 표 파싱(__init). 부팅 초기.
 */
static int __init dmar_parse_one_andd(struct acpi_dmar_header *header,
				      void *arg)
{
	struct acpi_dmar_andd *andd = (void *)header;	/* [한국어] 헤더를 ANDD 항목으로 */

	/* Check for NUL termination within the designated length */
	if (strnlen(andd->device_name, header->length - 8) == header->length - 8) {	/* [한국어] 이름이 항목 길이 안에서 NUL 로 끝나지 않으면 (앞 8바이트는 헤더와 device_number 다) */
		pr_warn(FW_BUG	/* [한국어] 문자열로 다루는 순간 항목 밖을 읽게 되므로 거부한다 */
			   "Your BIOS is broken; ANDD object name is not NUL-terminated\n"	/* [한국어] 펌웨어 버그임을 명시 */
			   "BIOS vendor: %s; Ver: %s; Product Version: %s\n",	/* [한국어] 어느 BIOS 인지 */
			   dmi_get_system_info(DMI_BIOS_VENDOR),	/* [한국어] 벤더 */
			   dmi_get_system_info(DMI_BIOS_VERSION),	/* [한국어] 버전 */
			   dmi_get_system_info(DMI_PRODUCT_VERSION));	/* [한국어] 제품 버전 */
		add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);	/* [한국어] 커널에 오염 표시를 남긴다 */
		return -EINVAL;	/* [한국어] 이 항목을 쓰지 않는다 */
	}
	pr_info("ANDD device: %x name: %s\n", andd->device_number,	/* [한국어] 정상이면 기록만 남긴다. 이 이름으로 나중에 실제 장치를 찾는다 */
		andd->device_name);	/* [한국어] ACPI 네임스페이스 이름 */

	return 0;	/* [한국어] 항목 처리 완료 */
}

#ifdef CONFIG_ACPI_NUMA	/* [한국어] NUMA 를 켠 빌드에서만 RHSA 항목을 해석한다 */
/*
 * [한국어]
 * dmar_parse_one_rhsa - RHSA 항목으로 유닛의 NUMA 노드를 정한다
 *
 * @header: RHSA(Remapping Hardware Static Affinity) 항목. @arg: 쓰지 않는다.
 * @return: 항상 0 — 잘못된 항목이어도 부팅을 막지 않는다.
 *
 * RHSA 는 "이 주소의 DMAR 유닛은 이 근접 도메인(NUMA 노드)에 속한다"를
 * 알려 주는 항목이다. 그 정보로 유닛의 node 를 정하면, 이후 루트 테이블과
 * 페이지 테이블을 그 노드에서 잡아 하드웨어의 접근 지연을 줄일 수 있다.
 *
 * 레지스터 기저 주소로 유닛을 찾는다 — 그것이 유닛의 정체이기 때문이다.
 * 찾지 못하면 펌웨어가 존재하지 않는 유닛을 가리킨 것이라 FW_BUG 로 남기고
 * 커널을 오염 표시하되, 0 을 돌려주어 부팅은 계속한다. NUMA 정보가 없으면
 * 성능이 조금 나빠질 뿐 동작에는 지장이 없기 때문이다.
 *
 * 온라인이 아닌 노드를 NUMA_NO_NODE 로 되돌리는 것도 같은 태도다: 펌웨어가
 * 아직 온라인이 아닌 노드를 가리켰다면 그 노드에서 메모리를 잡을 수 없으므로,
 * 노드 지정 없이 잡게 한다.
 *
 * CONFIG_ACPI_NUMA 를 끈 빌드에서는 dmar_res_noop 으로 대체되어 항목을
 * 조용히 건너뛴다.
 *
 * 실행 컨텍스트: 표 파싱. 프로세스 컨텍스트.
 */
static int dmar_parse_one_rhsa(struct acpi_dmar_header *header, void *arg)
{
	struct acpi_dmar_rhsa *rhsa;	/* [한국어] ACPI 항목 */
	struct dmar_drhd_unit *drhd;	/* [한국어] 유닛 순회 커서 */

	rhsa = (struct acpi_dmar_rhsa *)header;	/* [한국어] 헤더를 RHSA 항목으로 */
	for_each_drhd_unit(drhd) {	/* [한국어] 등록된 유닛들을 훑으며 */
		if (drhd->reg_base_addr == rhsa->base_address) {	/* [한국어] 레지스터 주소가 같은 유닛을 찾으면 — 그것이 유닛의 정체다 */
			int node = pxm_to_node(rhsa->proximity_domain);	/* [한국어] 근접 도메인을 NUMA 노드 번호로 */

			if (node != NUMA_NO_NODE && !node_online(node))	/* [한국어] 아직 온라인이 아닌 노드를 가리켰으면 */
				node = NUMA_NO_NODE;	/* [한국어] 노드 지정 없이 잡게 한다 — 그 노드에서는 메모리를 잡을 수 없다 */
			drhd->iommu->node = node;	/* [한국어] 유닛에 기록한다. 이후 테이블을 이 노드에서 잡아 하드웨어의 접근 지연을 줄인다 */
			return 0;	/* [한국어] 처리 완료 */
		}
	}
	pr_warn(FW_BUG	/* [한국어] 존재하지 않는 유닛을 가리켰다 */
		"Your BIOS is broken; RHSA refers to non-existent DMAR unit at %llx\n"	/* [한국어] 어느 주소인지 */
		"BIOS vendor: %s; Ver: %s; Product Version: %s\n",	/* [한국어] 어느 BIOS 인지 */
		rhsa->base_address,	/* [한국어] 문제의 주소 */
		dmi_get_system_info(DMI_BIOS_VENDOR),	/* [한국어] 벤더 */
		dmi_get_system_info(DMI_BIOS_VERSION),	/* [한국어] 버전 */
		dmi_get_system_info(DMI_PRODUCT_VERSION));	/* [한국어] 제품 버전 */
	add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);	/* [한국어] 오염 표시 */

	return 0;	/* [한국어] 그래도 0 을 돌려준다 — NUMA 정보가 없으면 성능이 조금 나빠질 뿐 동작에는 지장이 없다 */
}
#else
#define	dmar_parse_one_rhsa		dmar_res_noop	/* [한국어] NUMA 를 끈 빌드에서는 RHSA 항목을 조용히 건너뛴다. 노드 정보를 쓸 곳이 없기 때문이다 */
#endif

/*
 * [한국어]
 * dmar_table_print_dmar_entry - DMAR 표 항목 하나를 사람이 읽을 수 있게 로그에 찍는다
 *
 * @header: 찍을 항목.
 * @return: 없음.
 *
 * 부팅 파싱에서만 불린다. dmesg 에 남은 이 목록이 "이 시스템의 IOMMU 구성이
 * 어떠했는가"를 알려 주는 유일한 기록인 경우가 많다 — 나중에 문제를
 * 조사할 때 표를 다시 읽을 방법이 없기 때문이다(DMAR 표는 __initdata 라
 * 부팅이 끝나면 해제된다).
 *
 * 항목 종류마다 의미 있는 필드만 골라 찍는다. DRHD 는 레지스터 주소와
 * 플래그, RMRR 은 예약 구간의 범위, RHSA 는 근접 도메인 같은 식이다.
 *
 * ANDD 만 여기서 찍지 않는다(코드 안 영어 주석). 이름 문자열이 NUL 로
 * 끝나는지 먼저 검증해야 안전하게 찍을 수 있어서, 그 검증을 하는
 * dmar_parse_one_andd 에서 대신 찍는다.
 *
 * 실행 컨텍스트: 표 파싱(부팅). 프로세스 컨텍스트.
 */
static void
dmar_table_print_dmar_entry(struct acpi_dmar_header *header)
{
	struct acpi_dmar_hardware_unit *drhd;	/* [한국어] DRHD 항목으로 해석할 때 */
	struct acpi_dmar_reserved_memory *rmrr;	/* [한국어] RMRR 일 때 */
	struct acpi_dmar_atsr *atsr;	/* [한국어] ATSR 일 때 */
	struct acpi_dmar_rhsa *rhsa;	/* [한국어] RHSA 일 때 */
	struct acpi_dmar_satc *satc;	/* [한국어] SATC 일 때 */

	switch (header->type) {	/* [한국어] 항목 종류에 따라 */
	case ACPI_DMAR_TYPE_HARDWARE_UNIT:	/* [한국어] VT-d 유닛 */
		drhd = container_of(header, struct acpi_dmar_hardware_unit,	/* [한국어] 그 형식으로 */
				    header);	/* [한국어] 해석하고 */
		pr_info("DRHD base: %#016Lx flags: %#x\n",	/* [한국어] 레지스터 주소와 플래그를 찍는다 */
			(unsigned long long)drhd->address, drhd->flags);	/* [한국어] 그 값들 */
		break;	/* [한국어] 다음 */
	case ACPI_DMAR_TYPE_RESERVED_MEMORY:	/* [한국어] 예약 메모리 구간 */
		rmrr = container_of(header, struct acpi_dmar_reserved_memory,	/* [한국어] 그 형식으로 */
				    header);	/* [한국어] 해석하고 */
		pr_info("RMRR base: %#016Lx end: %#016Lx\n",	/* [한국어] 구간의 범위를 찍는다 */
			(unsigned long long)rmrr->base_address,	/* [한국어] 시작 */
			(unsigned long long)rmrr->end_address);	/* [한국어] 끝 */
		break;	/* [한국어] 다음 */
	case ACPI_DMAR_TYPE_ROOT_ATS:	/* [한국어] ATS 능력 보고 */
		atsr = container_of(header, struct acpi_dmar_atsr, header);	/* [한국어] 그 형식으로 */
		pr_info("ATSR flags: %#x\n", atsr->flags);	/* [한국어] 플래그만 찍는다(비트 0 이 include_all 이다) */
		break;	/* [한국어] 다음 */
	case ACPI_DMAR_TYPE_HARDWARE_AFFINITY:	/* [한국어] NUMA 근접성 */
		rhsa = container_of(header, struct acpi_dmar_rhsa, header);	/* [한국어] 그 형식으로 */
		pr_info("RHSA base: %#016Lx proximity domain: %#x\n",	/* [한국어] 어느 유닛이 어느 노드인지 */
		       (unsigned long long)rhsa->base_address,	/* [한국어] 유닛의 레지스터 주소 */
		       rhsa->proximity_domain);	/* [한국어] 근접 도메인 번호 */
		break;	/* [한국어] 다음 */
	case ACPI_DMAR_TYPE_NAMESPACE:	/* [한국어] ACPI 네임스페이스 장치는 */
		/* We don't print this here because we need to sanity-check
		   it first. So print it in dmar_parse_one_andd() instead. */
		break;	/* [한국어] 여기서 찍지 않는다. 이름이 NUL 로 끝나는지 먼저 검증해야 안전해서, 그 검증을 하는 dmar_parse_one_andd 가 대신 찍는다 (위 영어 주석) */
	case ACPI_DMAR_TYPE_SATC:	/* [한국어] SoC 통합 ATS 신고 */
		satc = container_of(header, struct acpi_dmar_satc, header);	/* [한국어] 그 형식으로 */
		pr_info("SATC flags: 0x%x\n", satc->flags);	/* [한국어] 플래그를 찍는다(비트 0 이 atc_required 다) */
		break;	/* [한국어] 끝 */
	}
}

/**
 * dmar_table_detect - checks to see if the platform supports DMAR devices
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * dmar_table_detect - 이 플랫폼에 DMAR 표가 있는지 확인하고 매핑한다
 *
 * @return: 0 이면 표가 있다, -ENOENT 면 없거나 매핑에 실패했다.
 *
 * VT-d 를 쓸 수 있는지의 첫 판단이다. DMAR 표가 없다는 것은 펌웨어가
 * IOMMU 하드웨어를 보고하지 않았다는 뜻이고, 그러면 아무것도 할 수 없다.
 *
 * acpi_get_table 이 성공했는데 dmar_tbl 이 NULL 인 경우를 따로 확인한다 —
 * 표는 있는데 매핑에 실패한 상황이라, 있는 것으로 착각하고 진행하면
 * NULL 을 역참조한다.
 *
 * 이 함수는 두 번 불린다: 아주 이른 시점의 탐지와, parse_dmar_table 안에서
 * 한 번 더. 그 이유는 그쪽 주석에 있다.
 *
 * 실행 컨텍스트: 부팅 초기(__init).
 */
static int __init dmar_table_detect(void)
{
	acpi_status status = AE_OK;	/* [한국어] ACPI 호출의 결과 */

	/* if we could find DMAR table, then there are DMAR devices */
	status = acpi_get_table(ACPI_SIG_DMAR, 0, &dmar_tbl);	/* [한국어] DMAR 표를 찾아 매핑한다. 표가 있다는 것이 곧 VT-d 하드웨어가 있다는 뜻이다 (위 영어 주석) */

	if (ACPI_SUCCESS(status) && !dmar_tbl) {	/* [한국어] 표는 찾았는데 매핑이 안 된 경우 */
		pr_warn("Unable to map DMAR\n");	/* [한국어] 있는 것으로 착각하고 진행하면 NULL 을 역참조한다 */
		status = AE_NOT_FOUND;	/* [한국어] 없는 것으로 처리한다 */
	}

	return ACPI_SUCCESS(status) ? 0 : -ENOENT;	/* [한국어] 표가 없으면 VT-d 를 쓸 수 없다 */
}

/*
 * [한국어]
 * dmar_walk_remapping_entries - DMAR 표의 항목들을 훑으며 종류별 콜백을 부른다
 *
 * @start: 첫 항목. @len: 훑을 전체 길이. @cb: 종류별 콜백 묶음.
 * @return: 0 성공, 음수면 콜백이 실패했거나 표가 손상되었다.
 *
 * 이 파일의 모든 표 파싱이 이 한 함수를 거친다. 항목 종류를 인덱스로 하는
 * 콜백 배열 덕분에, 같은 순회 코드가 부팅 파싱·핫플러그 삽입·제거 검증에
 * 모두 쓰인다.
 *
 * 표가 손상된 경우에 대한 방어가 두 겹이다. ACPI 표는 펌웨어가 준 것이라
 * 신뢰할 수 없다.
 *   - 길이가 0 인 항목: next 가 iter 와 같아져 무한 루프가 된다. 그래서
 *     그 자리에서 멈춘다(코드 안 영어 주석).
 *   - 표 끝을 넘어가는 항목: 그대로 두면 표 밖의 메모리를 항목으로 해석한다.
 *     오류로 처리한다.
 *
 * 모르는 종류를 만났을 때의 처리가 두 갈래인 것도 의도적이다.
 *   - 정의된 범위를 넘는 종류(>= ACPI_DMAR_TYPE_RESERVED): 앞으로의 호환성을
 *     위해 조용히 건너뛴다(코드 안 영어 주석). 새 스펙의 항목을 옛 커널이
 *     만났을 때 부팅이 실패하면 안 된다.
 *   - 정의된 종류인데 콜백이 없는 경우: ignore_unhandled 가 정한다. 부팅
 *     파싱에서는 오류로 보고, 특정 종류만 처리하는 순회에서는 건너뛴다.
 *
 * 실행 컨텍스트: 표 파싱 또는 핫플러그. 프로세스 컨텍스트.
 */
static int dmar_walk_remapping_entries(struct acpi_dmar_header *start,
				       size_t len, struct dmar_res_callback *cb)
{
	struct acpi_dmar_header *iter, *next;	/* [한국어] 현재 항목과 다음 항목 */
	struct acpi_dmar_header *end = ((void *)start) + len;	/* [한국어] 훑을 영역의 끝 */

	for (iter = start; iter < end; iter = next) {	/* [한국어] 항목을 하나씩 */
		next = (void *)iter + iter->length;	/* [한국어] 다음 항목은 이 항목의 길이만큼 뒤에 있다 */
		if (iter->length == 0) {	/* [한국어] 길이가 0 이면 next 가 iter 와 같아져 무한 루프가 된다 (위 영어 주석) */
			/* Avoid looping forever on bad ACPI tables */
			pr_debug(FW_BUG "Invalid 0-length structure\n");	/* [한국어] 펌웨어가 준 표가 손상되었다 */
			break;	/* [한국어] 그 자리에서 멈춘다 */
		} else if (next > end) {	/* [한국어] 항목이 표 끝을 넘어가면 (위 영어 주석) */
			/* Avoid passing table end */
			pr_warn(FW_BUG "Record passes table end\n");	/* [한국어] 그대로 두면 표 밖의 메모리를 항목으로 해석하게 된다 */
			return -EINVAL;	/* [한국어] 오류로 처리한다 */
		}

		if (cb->print_entry)	/* [한국어] 로그를 남기라고 했으면 */
			dmar_table_print_dmar_entry(iter);	/* [한국어] 항목 내용을 찍는다 */

		if (iter->type >= ACPI_DMAR_TYPE_RESERVED) {	/* [한국어] 우리가 아는 범위를 넘는 종류면 */
			/* continue for forward compatibility */
			pr_debug("Unknown DMAR structure type %d\n",	/* [한국어] 조용히 건너뛴다 — 새 스펙의 항목을 옛 커널이 만났을 때 부팅이 실패하면 안 된다 (위 영어 주석) */
				 iter->type);	/* [한국어] 어떤 종류였는지 */
		} else if (cb->cb[iter->type]) {	/* [한국어] 아는 종류이고 콜백이 있으면 */
			int ret;	/* [한국어] 그 결과 */

			ret = cb->cb[iter->type](iter, cb->arg[iter->type]);	/* [한국어] 종류에 맞는 처리를 한다 */
			if (ret)	/* [한국어] 실패하면 */
				return ret;	/* [한국어] 순회를 멈추고 전파한다 */
		} else if (!cb->ignore_unhandled) {	/* [한국어] 콜백이 없는데 건너뛰지 말라고 했으면 */
			pr_warn("No handler for DMAR structure type %d\n",	/* [한국어] 다룰 수 없는 항목이다 */
				iter->type);	/* [한국어] 어떤 종류였는지 */
			return -EINVAL;	/* [한국어] 오류로 처리한다 */
		}
	}

	return 0;	/* [한국어] 표 전체를 훑었다 */
}

/*
 * [한국어]
 * dmar_walk_dmar_table - DMAR 표 전체를 훑는다
 *
 * @dmar: 매핑된 DMAR 표. @cb: 종류별 콜백 묶음.
 * @return: dmar_walk_remapping_entries 의 결과.
 *
 * 표 헤더 바로 뒤가 첫 항목이고, 전체 길이에서 헤더 크기를 뺀 만큼이
 * 항목 영역이다. 그 계산만 해 주는 얇은 껍데기다.
 *
 * 이 계산을 한 곳에 모아 둔 이유: 헤더 크기를 잘못 빼면 첫 항목의 시작이나
 * 마지막 항목의 끝이 어긋나는데, 그것은 표 밖을 읽거나 항목 하나를 놓치는
 * 결과로 이어진다. 호출부마다 반복하지 않는 편이 안전하다.
 *
 * 실행 컨텍스트: 표 파싱 또는 핫플러그.
 */
static inline int dmar_walk_dmar_table(struct acpi_table_dmar *dmar,
				       struct dmar_res_callback *cb)
{
	return dmar_walk_remapping_entries((void *)(dmar + 1),	/* [한국어] 첫 항목은 표 헤더 바로 뒤에 있다 */
			dmar->header.length - sizeof(*dmar), cb);	/* [한국어] 전체 길이에서 헤더 크기를 뺀 만큼이 항목 영역이다. 이 계산이 어긋나면 표 밖을 읽거나 항목 하나를 놓친다 */
}

/**
 * parse_dmar_table - parses the DMA reporting table
 */
/*
 * [한국어] (위 영어 kernel-doc 에 이어)
 * parse_dmar_table - DMAR 표 전체를 파싱해 모든 항목을 커널 자료구조로 만든다
 *
 * @return: 0 성공, 음수면 표가 없거나 손상되었거나 항목 처리가 실패했다.
 *
 * 부팅 시 VT-d 초기화의 실질적인 시작점이다. 콜백 묶음을 채워
 * dmar_walk_dmar_table 에 넘기면, 종류마다 알맞은 파서가 불려 DRHD·RMRR·
 * ATSR·RHSA·ANDD·SATC 가 모두 등록된다.
 *
 * ignore_unhandled 를 참으로 두는 것을 눈여겨볼 것: 모든 종류에 콜백을
 * 꽂았으므로 사실상 걸릴 일이 없지만, 앞으로 새 종류가 생겨도 부팅이
 * 실패하지 않게 하는 안전판이다.
 *
 * dmar_table_detect 를 다시 부르는 이유(코드 안 영어 주석): 앞서의 매핑은
 * fixmap 으로 되어 있을 수 있다. 부팅이 진행되어 정상적인 매핑이 가능해진
 * 지금 다시 얻어야 표 전체를 안전하게 훑을 수 있다.
 *
 * tboot_get_dmar_table 를 거치는 이유(코드 안 영어 주석): TXT 로 부팅한
 * 경우 ACPI 표가 DMA 로부터 보호되지 않을 수 있다. 그때는 SINIT 이 TXT
 * 힙에 저장해 둔 사본 — DMA 보호된 영역에 있는 — 을 대신 쓴다. 측정 부팅의
 * 신뢰 사슬이 여기서도 이어진다.
 *
 * width 검사: 호스트 주소 폭이 페이지 시프트보다 작으면 표가 손상된 것이다.
 * DRHD 가 하나도 없으면 경고만 남기는데(FW_BUG), 표는 있는데 유닛이 없는
 * 이상한 상태이기 때문이다.
 *
 * 실행 컨텍스트: 부팅 초기(__init). 단일 스레드.
 *
 * 호출 체인:
 *   dmar_table_init() → [parse_dmar_table] → dmar_walk_dmar_table()
 *     → dmar_parse_one_drhd() 등
 */
static int __init
parse_dmar_table(void)
{
	struct acpi_table_dmar *dmar;	/* [한국어] 매핑된 표 */
	int drhd_count = 0;	/* [한국어] 발견한 유닛 수 */
	int ret;	/* [한국어] 결과 */
	struct dmar_res_callback cb = {	/* [한국어] 종류별 파서를 꽂은 콜백 묶음 */
		.print_entry = true,	/* [한국어] 부팅에서만 항목 내용을 로그에 남긴다. 이 기록이 나중에 구성을 확인할 유일한 단서가 되는 경우가 많다 */
		.ignore_unhandled = true,	/* [한국어] 모든 종류에 콜백을 꽂았지만, 앞으로 새 종류가 생겨도 부팅이 실패하지 않게 하는 안전판이다 */
		.arg[ACPI_DMAR_TYPE_HARDWARE_UNIT] = &drhd_count,	/* [한국어] DRHD 파서에 카운터를 넘겨 유닛 수를 세게 한다 */
		.cb[ACPI_DMAR_TYPE_HARDWARE_UNIT] = &dmar_parse_one_drhd,	/* [한국어] VT-d 유닛 */
		.cb[ACPI_DMAR_TYPE_RESERVED_MEMORY] = &dmar_parse_one_rmrr,	/* [한국어] 예약 메모리 구간 */
		.cb[ACPI_DMAR_TYPE_ROOT_ATS] = &dmar_parse_one_atsr,	/* [한국어] ATS 능력 보고 */
		.cb[ACPI_DMAR_TYPE_HARDWARE_AFFINITY] = &dmar_parse_one_rhsa,	/* [한국어] NUMA 근접성 */
		.cb[ACPI_DMAR_TYPE_NAMESPACE] = &dmar_parse_one_andd,	/* [한국어] ACPI 네임스페이스 장치 */
		.cb[ACPI_DMAR_TYPE_SATC] = &dmar_parse_one_satc,	/* [한국어] SoC 통합 ATS 신고 */
	};

	/*
	 * Do it again, earlier dmar_tbl mapping could be mapped with
	 * fixed map.
	 */
	dmar_table_detect();	/* [한국어] 다시 얻는다 — 앞서의 매핑은 fixmap 이었을 수 있고, 지금은 정상 매핑이 가능하다 (위 영어 주석) */

	/*
	 * ACPI tables may not be DMA protected by tboot, so use DMAR copy
	 * SINIT saved in SinitMleData in TXT heap (which is DMA protected)
	 */
	dmar_tbl = tboot_get_dmar_table(dmar_tbl);	/* [한국어] TXT 로 부팅했으면 SINIT 이 DMA 보호 영역에 저장해 둔 사본을 쓴다. ACPI 표 자체는 DMA 로부터 보호되지 않을 수 있어, 측정 부팅의 신뢰 사슬이 여기서도 이어진다 (위 영어 주석) */

	dmar = (struct acpi_table_dmar *)dmar_tbl;	/* [한국어] 표를 DMAR 형식으로 */
	if (!dmar)	/* [한국어] 표가 없으면 */
		return -ENODEV;	/* [한국어] VT-d 하드웨어가 없다 */

	if (dmar->width < PAGE_SHIFT - 1) {	/* [한국어] 호스트 주소 폭이 페이지 시프트보다 작으면 */
		pr_warn("Invalid DMAR haw\n");	/* [한국어] 표가 손상된 것이다 */
		return -EINVAL;	/* [한국어] 파싱하지 않는다 */
	}

	pr_info("Host address width %d\n", dmar->width + 1);	/* [한국어] 이 시스템이 다룰 수 있는 물리 주소 폭 */
	ret = dmar_walk_dmar_table(dmar, &cb);	/* [한국어] 표 전체를 훑으며 종류별 파서를 부른다 */
	if (ret == 0 && drhd_count == 0)	/* [한국어] 성공했는데 유닛이 하나도 없으면 */
		pr_warn(FW_BUG "No DRHD structure found in DMAR table\n");	/* [한국어] 표는 있는데 하드웨어가 없다는 이상한 상태다 */

	return ret;	/* [한국어] 파싱 결과 */
}

/*
 * [한국어]
 * dmar_pci_device_match - 이 장치나 그 조상이 device scope 목록에 있는지 본다
 *
 * @devices: 유닛의 장치 목록. @cnt: 그 개수. @dev: 찾을 장치.
 * @return: 1 이면 이 유닛이 담당하는 장치다.
 *
 * 부모를 거슬러 올라가며 찾는 것이 핵심이다. DMAR 표가 브리지를 지목했다면
 * 그 아래의 모든 장치가 그 유닛에 속한다 — 표에 장치 하나하나를 적을 필요가
 * 없도록 설계되어 있다.
 *
 * 그래서 자기 자신이 목록에 없어도 부모 브리지가 있으면 매치된다. while
 * 루프가 dev->bus->self 로 한 단계씩 올라가는 것이 그 구현이다.
 *
 * 실행 컨텍스트: 유닛 조회. RCU 순회 안에서 불린다.
 */
static int dmar_pci_device_match(struct dmar_dev_scope devices[],
				 int cnt, struct pci_dev *dev)
{
	int index;	/* [한국어] 순회 인덱스 */
	struct device *tmp;	/* [한국어] 순회 커서 */

	while (dev) {	/* [한국어] 장치에서 시작해 부모를 거슬러 올라가며 */
		for_each_active_dev_scope(devices, cnt, index, tmp)	/* [한국어] 목록의 연결된 장치들과 */
			if (dev_is_pci(tmp) && dev == to_pci_dev(tmp))	/* [한국어] 비교한다 */
				return 1;	/* [한국어] 이 유닛이 담당한다 */

		/* Check our parent */
		dev = dev->bus->self;	/* [한국어] 부모 브리지로 올라간다 (위 영어 주석). 표가 브리지를 지목했다면 그 아래 모든 장치가 그 유닛에 속하므로, 표에 장치를 하나하나 적을 필요가 없다 */
	}

	return 0;	/* [한국어] 이 유닛의 것이 아니다 */
}

/*
 * [한국어]
 * dmar_find_matched_drhd_unit - 이 PCI 장치를 담당하는 유닛을 찾는다
 *
 * @dev: 찾을 장치.
 * @return: 담당 유닛, 없으면 NULL.
 *
 * 목록 순서가 결과를 정한다. dmar_register_drhd_unit 이 include_all 유닛을
 * 꼬리에 넣어 두었으므로, 앞에서부터 훑으면 특정 장치를 지목한 유닛이 먼저
 * 매치된다. 그 순서가 없으면 include_all 이 모든 장치를 가로챈다.
 *
 * 두 조건 중 하나면 매치다.
 *   - include_all 유닛이고 세그먼트가 같다.
 *   - device scope 목록에 이 장치나 그 조상이 있다.
 *
 * pci_physfn 으로 시작하는 이유: VF 는 자기 DMAR 정보를 갖지 않고 PF 를
 * 통해 조회된다.
 *
 * 동기화: RCU 읽기 락. 목록이 핫플러그로 바뀔 수 있다.
 * 실행 컨텍스트: 인터럽트 재매핑 설정 등. 프로세스 컨텍스트.
 */
struct dmar_drhd_unit *
dmar_find_matched_drhd_unit(struct pci_dev *dev)
{
	struct dmar_drhd_unit *dmaru;	/* [한국어] 순회 커서 */
	struct acpi_dmar_hardware_unit *drhd;	/* [한국어] 그 유닛의 원본 ACPI 항목 */

	dev = pci_physfn(dev);	/* [한국어] VF 는 자기 DMAR 정보를 갖지 않고 PF 를 통해 조회된다 */

	rcu_read_lock();	/* [한국어] 목록이 핫플러그로 바뀔 수 있다 */
	for_each_drhd_unit(dmaru) {	/* [한국어] 앞에서부터 훑는다. 목록 순서 덕분에 특정 장치를 지목한 유닛이 include_all 보다 먼저 매치된다 */
		drhd = container_of(dmaru->hdr,	/* [한국어] 보관된 ACPI 항목으로 */
				    struct acpi_dmar_hardware_unit,	/* [한국어] DRHD 형식으로 */
				    header);	/* [한국어] 해석 */

		if (dmaru->include_all &&	/* [한국어] "나머지를 모두 담당한다"는 유닛이고 */
		    drhd->segment == pci_domain_nr(dev->bus))	/* [한국어] 세그먼트가 같으면 */
			goto out;	/* [한국어] 이 유닛이 담당한다 */

		if (dmar_pci_device_match(dmaru->devices,	/* [한국어] 장치 목록에 이 장치나 그 조상이 있으면 */
					  dmaru->devices_cnt, dev))	/* [한국어] 역시 */
			goto out;	/* [한국어] 이 유닛이 담당한다 */
	}
	dmaru = NULL;	/* [한국어] 어느 유닛도 담당하지 않는다 */
out:	/* [한국어] 두 결론이 합류 */
	rcu_read_unlock();	/* [한국어] 순회 끝 */

	return dmaru;	/* [한국어] 담당 유닛 또는 NULL */
}

static void __init dmar_acpi_insert_dev_scope(u8 device_number,
					      struct acpi_device *adev)
{
	struct dmar_drhd_unit *dmaru;
	struct acpi_dmar_hardware_unit *drhd;
	struct acpi_dmar_device_scope *scope;
	struct device *tmp;
	int i;
	struct acpi_dmar_pci_path *path;

	for_each_drhd_unit(dmaru) {
		drhd = container_of(dmaru->hdr,
				    struct acpi_dmar_hardware_unit,
				    header);

		for (scope = (void *)(drhd + 1);
		     (unsigned long)scope < ((unsigned long)drhd) + drhd->header.length;
		     scope = ((void *)scope) + scope->length) {
			if (scope->entry_type != ACPI_DMAR_SCOPE_TYPE_NAMESPACE)
				continue;
			if (scope->enumeration_id != device_number)
				continue;

			path = (void *)(scope + 1);
			pr_info("ACPI device \"%s\" under DMAR at %llx as %02x:%02x.%d\n",
				dev_name(&adev->dev), dmaru->reg_base_addr,
				scope->bus, path->device, path->function);
			for_each_dev_scope(dmaru->devices, dmaru->devices_cnt, i, tmp)
				if (tmp == NULL) {
					dmaru->devices[i].bus = scope->bus;
					dmaru->devices[i].devfn = PCI_DEVFN(path->device,
									    path->function);
					rcu_assign_pointer(dmaru->devices[i].dev,
							   get_device(&adev->dev));
					return;
				}
			BUG_ON(i >= dmaru->devices_cnt);
		}
	}
	pr_warn("No IOMMU scope found for ANDD enumeration ID %d (%s)\n",
		device_number, dev_name(&adev->dev));
}

static int __init dmar_acpi_dev_scope_init(void)
{
	struct acpi_dmar_andd *andd;

	if (dmar_tbl == NULL)
		return -ENODEV;

	for (andd = (void *)dmar_tbl + sizeof(struct acpi_table_dmar);
	     ((unsigned long)andd) < ((unsigned long)dmar_tbl) + dmar_tbl->length;
	     andd = ((void *)andd) + andd->header.length) {
		if (andd->header.type == ACPI_DMAR_TYPE_NAMESPACE) {
			acpi_handle h;
			struct acpi_device *adev;

			if (!ACPI_SUCCESS(acpi_get_handle(ACPI_ROOT_OBJECT,
							  andd->device_name,
							  &h))) {
				pr_err("Failed to find handle for ACPI object %s\n",
				       andd->device_name);
				continue;
			}
			adev = acpi_fetch_acpi_dev(h);
			if (!adev) {
				pr_err("Failed to get device for ACPI object %s\n",
				       andd->device_name);
				continue;
			}
			dmar_acpi_insert_dev_scope(andd->device_number, adev);
		}
	}
	return 0;
}

int __init dmar_dev_scope_init(void)
{
	struct pci_dev *dev = NULL;
	struct dmar_pci_notify_info *info;

	if (dmar_dev_scope_status != 1)
		return dmar_dev_scope_status;

	if (list_empty(&dmar_drhd_units)) {
		dmar_dev_scope_status = -ENODEV;
	} else {
		dmar_dev_scope_status = 0;

		dmar_acpi_dev_scope_init();

		for_each_pci_dev(dev) {
			if (dev->is_virtfn)
				continue;

			info = dmar_alloc_pci_notify_info(dev,
					BUS_NOTIFY_ADD_DEVICE);
			if (!info) {
				pci_dev_put(dev);
				return dmar_dev_scope_status;
			} else {
				dmar_pci_bus_add_dev(info);
				dmar_free_pci_notify_info(info);
			}
		}
	}

	return dmar_dev_scope_status;
}

void __init dmar_register_bus_notifier(void)
{
	bus_register_notifier(&pci_bus_type, &dmar_pci_bus_nb);
}


int __init dmar_table_init(void)
{
	static int dmar_table_initialized;
	int ret;

	if (dmar_table_initialized == 0) {
		ret = parse_dmar_table();
		if (ret < 0) {
			if (ret != -ENODEV)
				pr_info("Parse DMAR table failure.\n");
		} else  if (list_empty(&dmar_drhd_units)) {
			pr_info("No DMAR devices found\n");
			ret = -ENODEV;
		}

		if (ret < 0)
			dmar_table_initialized = ret;
		else
			dmar_table_initialized = 1;
	}

	return dmar_table_initialized < 0 ? dmar_table_initialized : 0;
}

static void warn_invalid_dmar(u64 addr, const char *message)
{
	pr_warn_once(FW_BUG
		"Your BIOS is broken; DMAR reported at address %llx%s!\n"
		"BIOS vendor: %s; Ver: %s; Product Version: %s\n",
		addr, message,
		dmi_get_system_info(DMI_BIOS_VENDOR),
		dmi_get_system_info(DMI_BIOS_VERSION),
		dmi_get_system_info(DMI_PRODUCT_VERSION));
	add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);
}

static int __ref
dmar_validate_one_drhd(struct acpi_dmar_header *entry, void *arg)
{
	struct acpi_dmar_hardware_unit *drhd;
	void __iomem *addr;
	u64 cap, ecap;

	drhd = (void *)entry;
	if (!drhd->address) {
		warn_invalid_dmar(0, "");
		return -EINVAL;
	}

	if (arg)
		addr = ioremap(drhd->address, VTD_PAGE_SIZE);
	else
		addr = early_ioremap(drhd->address, VTD_PAGE_SIZE);
	if (!addr) {
		pr_warn("Can't validate DRHD address: %llx\n", drhd->address);
		return -EINVAL;
	}

	cap = readq(addr + DMAR_CAP_REG);
	ecap = readq(addr + DMAR_ECAP_REG);

	if (arg)
		iounmap(addr);
	else
		early_iounmap(addr, VTD_PAGE_SIZE);

	if (cap == (uint64_t)-1 && ecap == (uint64_t)-1) {
		warn_invalid_dmar(drhd->address, " returns all ones");
		return -EINVAL;
	}

	return 0;
}

void __init detect_intel_iommu(void)
{
	int ret;
	struct dmar_res_callback validate_drhd_cb = {
		.cb[ACPI_DMAR_TYPE_HARDWARE_UNIT] = &dmar_validate_one_drhd,
		.ignore_unhandled = true,
	};

	down_write(&dmar_global_lock);
	ret = dmar_table_detect();
	if (!ret)
		ret = dmar_walk_dmar_table((struct acpi_table_dmar *)dmar_tbl,
					   &validate_drhd_cb);
	if (!ret && !no_iommu && !iommu_detected &&
	    (!dmar_disabled || dmar_platform_optin())) {
		iommu_detected = 1;
		/* Make sure ACS will be enabled */
		pci_request_acs();
	}

	if (!ret) {
		x86_init.iommu.iommu_init = intel_iommu_init;
		x86_platform.iommu_shutdown = intel_iommu_shutdown;
	}

	if (dmar_tbl) {
		acpi_put_table(dmar_tbl);
		dmar_tbl = NULL;
	}
	up_write(&dmar_global_lock);
}

static void unmap_iommu(struct intel_iommu *iommu)
{
	iounmap(iommu->reg);
	release_mem_region(iommu->reg_phys, iommu->reg_size);
}

/**
 * map_iommu: map the iommu's registers
 * @iommu: the iommu to map
 * @drhd: DMA remapping hardware definition structure
 *
 * Memory map the iommu's registers.  Start w/ a single page, and
 * possibly expand if that turns out to be insufficent.
 */
static int map_iommu(struct intel_iommu *iommu, struct dmar_drhd_unit *drhd)
{
	u64 phys_addr = drhd->reg_base_addr;
	int map_size, err=0;

	iommu->reg_phys = phys_addr;
	iommu->reg_size = drhd->reg_size;

	if (!request_mem_region(iommu->reg_phys, iommu->reg_size, iommu->name)) {
		pr_err("Can't reserve memory\n");
		err = -EBUSY;
		goto out;
	}

	iommu->reg = ioremap(iommu->reg_phys, iommu->reg_size);
	if (!iommu->reg) {
		pr_err("Can't map the region\n");
		err = -ENOMEM;
		goto release;
	}

	iommu->cap = readq(iommu->reg + DMAR_CAP_REG);
	iommu->ecap = readq(iommu->reg + DMAR_ECAP_REG);

	if (iommu->cap == (uint64_t)-1 && iommu->ecap == (uint64_t)-1) {
		err = -EINVAL;
		warn_invalid_dmar(phys_addr, " returns all ones");
		goto unmap;
	}

	/* the registers might be more than one page */
	map_size = max_t(int, ecap_max_iotlb_offset(iommu->ecap),
			 cap_max_fault_reg_offset(iommu->cap));
	map_size = VTD_PAGE_ALIGN(map_size);
	if (map_size > iommu->reg_size) {
		iounmap(iommu->reg);
		release_mem_region(iommu->reg_phys, iommu->reg_size);
		iommu->reg_size = map_size;
		if (!request_mem_region(iommu->reg_phys, iommu->reg_size,
					iommu->name)) {
			pr_err("Can't reserve memory\n");
			err = -EBUSY;
			goto out;
		}
		iommu->reg = ioremap(iommu->reg_phys, iommu->reg_size);
		if (!iommu->reg) {
			pr_err("Can't map the region\n");
			err = -ENOMEM;
			goto release;
		}
	}

	if (cap_ecmds(iommu->cap)) {
		int i;

		for (i = 0; i < DMA_MAX_NUM_ECMDCAP; i++) {
			iommu->ecmdcap[i] = readq(iommu->reg + DMAR_ECCAP_REG +
						  i * DMA_ECMD_REG_STEP);
		}
	}

	err = 0;
	goto out;

unmap:
	iounmap(iommu->reg);
release:
	release_mem_region(iommu->reg_phys, iommu->reg_size);
out:
	return err;
}

static int alloc_iommu(struct dmar_drhd_unit *drhd)
{
	struct intel_iommu *iommu;
	u32 ver, sts;
	int agaw = -1;
	int msagaw = -1;
	int err;

	if (!drhd->reg_base_addr) {
		warn_invalid_dmar(0, "");
		return -EINVAL;
	}

	iommu = kzalloc_obj(*iommu);
	if (!iommu)
		return -ENOMEM;

	iommu->seq_id = ida_alloc_range(&dmar_seq_ids, 0,
					DMAR_UNITS_SUPPORTED - 1, GFP_KERNEL);
	if (iommu->seq_id < 0) {
		pr_err("Failed to allocate seq_id\n");
		err = iommu->seq_id;
		goto error;
	}
	snprintf(iommu->name, sizeof(iommu->name), "dmar%d", iommu->seq_id);

	err = map_iommu(iommu, drhd);
	if (err) {
		pr_err("Failed to map %s\n", iommu->name);
		goto error_free_seq_id;
	}

	if (!cap_sagaw(iommu->cap) &&
	    (!ecap_smts(iommu->ecap) || ecap_slts(iommu->ecap))) {
		pr_info("%s: No supported address widths. Not attempting DMA translation.\n",
			iommu->name);
		drhd->ignored = 1;
	}

	if (!drhd->ignored) {
		agaw = iommu_calculate_agaw(iommu);
		if (agaw < 0) {
			pr_err("Cannot get a valid agaw for iommu (seq_id = %d)\n",
			       iommu->seq_id);
			drhd->ignored = 1;
		}
	}
	if (!drhd->ignored) {
		msagaw = iommu_calculate_max_sagaw(iommu);
		if (msagaw < 0) {
			pr_err("Cannot get a valid max agaw for iommu (seq_id = %d)\n",
			       iommu->seq_id);
			drhd->ignored = 1;
			agaw = -1;
		}
	}
	iommu->agaw = agaw;
	iommu->msagaw = msagaw;
	iommu->segment = drhd->segment;
	iommu->device_rbtree = RB_ROOT;
	spin_lock_init(&iommu->device_rbtree_lock);
	mutex_init(&iommu->iopf_lock);
	iommu->node = NUMA_NO_NODE;
	spin_lock_init(&iommu->lock);
	ida_init(&iommu->domain_ida);
	mutex_init(&iommu->did_lock);

	ver = readl(iommu->reg + DMAR_VER_REG);
	pr_info("%s: reg_base_addr %llx ver %d:%d cap %llx ecap %llx\n",
		iommu->name,
		(unsigned long long)drhd->reg_base_addr,
		DMAR_VER_MAJOR(ver), DMAR_VER_MINOR(ver),
		(unsigned long long)iommu->cap,
		(unsigned long long)iommu->ecap);

	/* Reflect status in gcmd */
	sts = readl(iommu->reg + DMAR_GSTS_REG);
	if (sts & DMA_GSTS_IRES)
		iommu->gcmd |= DMA_GCMD_IRE;
	if (sts & DMA_GSTS_TES)
		iommu->gcmd |= DMA_GCMD_TE;
	if (sts & DMA_GSTS_QIES)
		iommu->gcmd |= DMA_GCMD_QIE;

	if (alloc_iommu_pmu(iommu))
		pr_debug("Cannot alloc PMU for iommu (seq_id = %d)\n", iommu->seq_id);

	raw_spin_lock_init(&iommu->register_lock);

	/*
	 * A value of N in PSS field of eCap register indicates hardware
	 * supports PASID field of N+1 bits.
	 */
	if (pasid_supported(iommu))
		iommu->iommu.max_pasids = 2UL << ecap_pss(iommu->ecap);

	/*
	 * This is only for hotplug; at boot time intel_iommu_enabled won't
	 * be set yet. When intel_iommu_init() runs, it registers the units
	 * present at boot time, then sets intel_iommu_enabled.
	 */
	if (intel_iommu_enabled && !drhd->ignored) {
		err = iommu_device_sysfs_add(&iommu->iommu, NULL,
					     intel_iommu_groups,
					     "%s", iommu->name);
		if (err)
			goto err_unmap;

		err = iommu_device_register(&iommu->iommu, &intel_iommu_ops, NULL);
		if (err)
			goto err_sysfs;

		iommu_pmu_register(iommu);
	}

	drhd->iommu = iommu;
	iommu->drhd = drhd;

	return 0;

err_sysfs:
	iommu_device_sysfs_remove(&iommu->iommu);
err_unmap:
	free_iommu_pmu(iommu);
	unmap_iommu(iommu);
error_free_seq_id:
	ida_free(&dmar_seq_ids, iommu->seq_id);
error:
	kfree(iommu);
	return err;
}

static void free_iommu(struct intel_iommu *iommu)
{
	if (intel_iommu_enabled && !iommu->drhd->ignored) {
		iommu_pmu_unregister(iommu);
		iommu_device_unregister(&iommu->iommu);
		iommu_device_sysfs_remove(&iommu->iommu);
	}

	free_iommu_pmu(iommu);

	if (iommu->irq) {
		if (iommu->pr_irq) {
			free_irq(iommu->pr_irq, iommu);
			dmar_free_hwirq(iommu->pr_irq);
			iommu->pr_irq = 0;
		}
		free_irq(iommu->irq, iommu);
		dmar_free_hwirq(iommu->irq);
		iommu->irq = 0;
	}

	if (iommu->qi) {
		iommu_free_pages(iommu->qi->desc);
		kfree(iommu->qi->desc_status);
		kfree(iommu->qi);
	}

	if (iommu->reg)
		unmap_iommu(iommu);

	ida_destroy(&iommu->domain_ida);
	ida_free(&dmar_seq_ids, iommu->seq_id);
	kfree(iommu);
}

/*
 * Reclaim all the submitted descriptors which have completed its work.
 */
static inline void reclaim_free_desc(struct q_inval *qi)
{
	while (qi->desc_status[qi->free_tail] == QI_FREE && qi->free_tail != qi->free_head) {
		qi->free_tail = (qi->free_tail + 1) % QI_LENGTH;
		qi->free_cnt++;
	}
}

static const char *qi_type_string(u8 type)
{
	switch (type) {
	case QI_CC_TYPE:
		return "Context-cache Invalidation";
	case QI_IOTLB_TYPE:
		return "IOTLB Invalidation";
	case QI_DIOTLB_TYPE:
		return "Device-TLB Invalidation";
	case QI_IEC_TYPE:
		return "Interrupt Entry Cache Invalidation";
	case QI_IWD_TYPE:
		return "Invalidation Wait";
	case QI_EIOTLB_TYPE:
		return "PASID-based IOTLB Invalidation";
	case QI_PC_TYPE:
		return "PASID-cache Invalidation";
	case QI_DEIOTLB_TYPE:
		return "PASID-based Device-TLB Invalidation";
	case QI_PGRP_RESP_TYPE:
		return "Page Group Response";
	default:
		return "UNKNOWN";
	}
}

static void qi_dump_fault(struct intel_iommu *iommu, u32 fault)
{
	unsigned int head = readl(iommu->reg + DMAR_IQH_REG);
	u64 iqe_err = readq(iommu->reg + DMAR_IQER_REG);
	struct qi_desc *desc = iommu->qi->desc + head;

	if (fault & DMA_FSTS_IQE)
		pr_err("VT-d detected Invalidation Queue Error: Reason %llx",
		       DMAR_IQER_REG_IQEI(iqe_err));
	if (fault & DMA_FSTS_ITE)
		pr_err("VT-d detected Invalidation Time-out Error: SID %llx",
		       DMAR_IQER_REG_ITESID(iqe_err));
	if (fault & DMA_FSTS_ICE)
		pr_err("VT-d detected Invalidation Completion Error: SID %llx",
		       DMAR_IQER_REG_ICESID(iqe_err));

	pr_err("QI HEAD: %s qw0 = 0x%llx, qw1 = 0x%llx\n",
	       qi_type_string(desc->qw0 & 0xf),
	       (unsigned long long)desc->qw0,
	       (unsigned long long)desc->qw1);

	head = ((head >> qi_shift(iommu)) + QI_LENGTH - 1) % QI_LENGTH;
	head <<= qi_shift(iommu);
	desc = iommu->qi->desc + head;

	pr_err("QI PRIOR: %s qw0 = 0x%llx, qw1 = 0x%llx\n",
	       qi_type_string(desc->qw0 & 0xf),
	       (unsigned long long)desc->qw0,
	       (unsigned long long)desc->qw1);
}

static int qi_check_fault(struct intel_iommu *iommu, int index, int wait_index)
{
	u32 fault;
	int head, tail;
	struct device *dev;
	u64 iqe_err, ite_sid;
	struct q_inval *qi = iommu->qi;
	int shift = qi_shift(iommu);

	if (qi->desc_status[wait_index] == QI_ABORT)
		return -EAGAIN;

	fault = readl(iommu->reg + DMAR_FSTS_REG);
	if (fault & (DMA_FSTS_IQE | DMA_FSTS_ITE | DMA_FSTS_ICE))
		qi_dump_fault(iommu, fault);

	/*
	 * If IQE happens, the head points to the descriptor associated
	 * with the error. No new descriptors are fetched until the IQE
	 * is cleared.
	 */
	if (fault & DMA_FSTS_IQE) {
		head = readl(iommu->reg + DMAR_IQH_REG);
		if ((head >> shift) == index) {
			struct qi_desc *desc = qi->desc + head;

			/*
			 * desc->qw2 and desc->qw3 are either reserved or
			 * used by software as private data. We won't print
			 * out these two qw's for security consideration.
			 */
			memcpy(desc, qi->desc + (wait_index << shift),
			       1 << shift);
			writel(DMA_FSTS_IQE, iommu->reg + DMAR_FSTS_REG);
			pr_info("Invalidation Queue Error (IQE) cleared\n");
			return -EINVAL;
		}
	}

	/*
	 * If ITE happens, all pending wait_desc commands are aborted.
	 * No new descriptors are fetched until the ITE is cleared.
	 */
	if (fault & DMA_FSTS_ITE) {
		head = readl(iommu->reg + DMAR_IQH_REG);
		head = ((head >> shift) - 1 + QI_LENGTH) % QI_LENGTH;
		tail = readl(iommu->reg + DMAR_IQT_REG);
		tail = ((tail >> shift) - 1 + QI_LENGTH) % QI_LENGTH;

		/*
		 * SID field is valid only when the ITE field is Set in FSTS_REG
		 * see Intel VT-d spec r4.1, section 11.4.9.9
		 */
		iqe_err = readq(iommu->reg + DMAR_IQER_REG);
		ite_sid = DMAR_IQER_REG_ITESID(iqe_err);

		writel(DMA_FSTS_ITE, iommu->reg + DMAR_FSTS_REG);
		pr_info("Invalidation Time-out Error (ITE) cleared\n");

		do {
			if (qi->desc_status[head] == QI_IN_USE)
				qi->desc_status[head] = QI_ABORT;
			head = (head - 1 + QI_LENGTH) % QI_LENGTH;
		} while (head != tail);

		/*
		 * If device was released or isn't present, no need to retry
		 * the ATS invalidate request anymore.
		 *
		 * 0 value of ite_sid means old VT-d device, no ite_sid value.
		 * see Intel VT-d spec r4.1, section 11.4.9.9
		 */
		if (ite_sid) {
			dev = device_rbtree_find(iommu, ite_sid);
			if (!dev || !dev_is_pci(dev) ||
			    !pci_device_is_present(to_pci_dev(dev)))
				return -ETIMEDOUT;
		}
		if (qi->desc_status[wait_index] == QI_ABORT)
			return -EAGAIN;
	}

	if (fault & DMA_FSTS_ICE) {
		writel(DMA_FSTS_ICE, iommu->reg + DMAR_FSTS_REG);
		pr_info("Invalidation Completion Error (ICE) cleared\n");
	}

	return 0;
}

/*
 * Function to submit invalidation descriptors of all types to the queued
 * invalidation interface(QI). Multiple descriptors can be submitted at a
 * time, a wait descriptor will be appended to each submission to ensure
 * hardware has completed the invalidation before return. Wait descriptors
 * can be part of the submission but it will not be polled for completion.
 */
int qi_submit_sync(struct intel_iommu *iommu, struct qi_desc *desc,
		   unsigned int count, unsigned long options)
{
	struct q_inval *qi = iommu->qi;
	s64 devtlb_start_ktime = 0;
	s64 iotlb_start_ktime = 0;
	s64 iec_start_ktime = 0;
	struct qi_desc wait_desc;
	int wait_index, index;
	unsigned long flags;
	int offset, shift;
	int rc, i;
	u64 type;

	if (!qi)
		return 0;

	type = desc->qw0 & GENMASK_ULL(3, 0);

	if ((type == QI_IOTLB_TYPE || type == QI_EIOTLB_TYPE) &&
	    dmar_latency_enabled(iommu, DMAR_LATENCY_INV_IOTLB))
		iotlb_start_ktime = ktime_to_ns(ktime_get());

	if ((type == QI_DIOTLB_TYPE || type == QI_DEIOTLB_TYPE) &&
	    dmar_latency_enabled(iommu, DMAR_LATENCY_INV_DEVTLB))
		devtlb_start_ktime = ktime_to_ns(ktime_get());

	if (type == QI_IEC_TYPE &&
	    dmar_latency_enabled(iommu, DMAR_LATENCY_INV_IEC))
		iec_start_ktime = ktime_to_ns(ktime_get());

restart:
	rc = 0;

	raw_spin_lock_irqsave(&qi->q_lock, flags);
	/*
	 * Check if we have enough empty slots in the queue to submit,
	 * the calculation is based on:
	 * # of desc + 1 wait desc + 1 space between head and tail
	 */
	while (qi->free_cnt < count + 2) {
		raw_spin_unlock_irqrestore(&qi->q_lock, flags);
		cpu_relax();
		raw_spin_lock_irqsave(&qi->q_lock, flags);
	}

	index = qi->free_head;
	wait_index = (index + count) % QI_LENGTH;
	shift = qi_shift(iommu);

	for (i = 0; i < count; i++) {
		offset = ((index + i) % QI_LENGTH) << shift;
		memcpy(qi->desc + offset, &desc[i], 1 << shift);
		qi->desc_status[(index + i) % QI_LENGTH] = QI_IN_USE;
		trace_qi_submit(iommu, desc[i].qw0, desc[i].qw1,
				desc[i].qw2, desc[i].qw3);
	}
	qi->desc_status[wait_index] = QI_IN_USE;

	wait_desc.qw0 = QI_IWD_STATUS_DATA(QI_DONE) |
			QI_IWD_STATUS_WRITE | QI_IWD_TYPE;
	if (options & QI_OPT_WAIT_DRAIN)
		wait_desc.qw0 |= QI_IWD_PRQ_DRAIN;
	wait_desc.qw1 = virt_to_phys(&qi->desc_status[wait_index]);
	wait_desc.qw2 = 0;
	wait_desc.qw3 = 0;

	offset = wait_index << shift;
	memcpy(qi->desc + offset, &wait_desc, 1 << shift);

	qi->free_head = (qi->free_head + count + 1) % QI_LENGTH;
	qi->free_cnt -= count + 1;

	/*
	 * update the HW tail register indicating the presence of
	 * new descriptors.
	 */
	writel(qi->free_head << shift, iommu->reg + DMAR_IQT_REG);

	while (READ_ONCE(qi->desc_status[wait_index]) != QI_DONE) {
		/*
		 * We will leave the interrupts disabled, to prevent interrupt
		 * context to queue another cmd while a cmd is already submitted
		 * and waiting for completion on this cpu. This is to avoid
		 * a deadlock where the interrupt context can wait indefinitely
		 * for free slots in the queue.
		 */
		rc = qi_check_fault(iommu, index, wait_index);
		if (rc)
			break;

		raw_spin_unlock(&qi->q_lock);
		cpu_relax();
		raw_spin_lock(&qi->q_lock);
	}

	/*
	 * The reclaim code can free descriptors from multiple submissions
	 * starting from the tail of the queue. When count == 0, the
	 * status of the standalone wait descriptor at the tail of the queue
	 * must be set to QI_FREE to allow the reclaim code to proceed.
	 * It is also possible that descriptors from one of the previous
	 * submissions has to be reclaimed by a subsequent submission.
	 */
	for (i = 0; i <= count; i++)
		qi->desc_status[(index + i) % QI_LENGTH] = QI_FREE;

	reclaim_free_desc(qi);
	raw_spin_unlock_irqrestore(&qi->q_lock, flags);

	if (rc == -EAGAIN)
		goto restart;

	if (iotlb_start_ktime)
		dmar_latency_update(iommu, DMAR_LATENCY_INV_IOTLB,
				ktime_to_ns(ktime_get()) - iotlb_start_ktime);

	if (devtlb_start_ktime)
		dmar_latency_update(iommu, DMAR_LATENCY_INV_DEVTLB,
				ktime_to_ns(ktime_get()) - devtlb_start_ktime);

	if (iec_start_ktime)
		dmar_latency_update(iommu, DMAR_LATENCY_INV_IEC,
				ktime_to_ns(ktime_get()) - iec_start_ktime);

	return rc;
}

/*
 * Flush the global interrupt entry cache.
 */
void qi_global_iec(struct intel_iommu *iommu)
{
	struct qi_desc desc;

	desc.qw0 = QI_IEC_TYPE;
	desc.qw1 = 0;
	desc.qw2 = 0;
	desc.qw3 = 0;

	/* should never fail */
	qi_submit_sync(iommu, &desc, 1, 0);
}

void qi_flush_context(struct intel_iommu *iommu, u16 did, u16 sid, u8 fm,
		      u64 type)
{
	struct qi_desc desc;

	desc.qw0 = QI_CC_FM(fm) | QI_CC_SID(sid) | QI_CC_DID(did)
			| QI_CC_GRAN(type) | QI_CC_TYPE;
	desc.qw1 = 0;
	desc.qw2 = 0;
	desc.qw3 = 0;

	qi_submit_sync(iommu, &desc, 1, 0);
}

void qi_flush_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
		    unsigned int size_order, u64 type)
{
	struct qi_desc desc;

	qi_desc_iotlb(iommu, did, addr, size_order, type, &desc);
	qi_submit_sync(iommu, &desc, 1, 0);
}

void qi_flush_dev_iotlb(struct intel_iommu *iommu, u16 sid, u16 pfsid,
			u16 qdep, u64 addr, unsigned mask)
{
	struct qi_desc desc;

	/*
	 * VT-d spec, section 4.3:
	 *
	 * Software is recommended to not submit any Device-TLB invalidation
	 * requests while address remapping hardware is disabled.
	 */
	if (!(iommu->gcmd & DMA_GCMD_TE))
		return;

	qi_desc_dev_iotlb(sid, pfsid, qdep, addr, mask, &desc);
	qi_submit_sync(iommu, &desc, 1, 0);
}

/* PASID-selective IOTLB invalidation */
void qi_flush_piotlb_all(struct intel_iommu *iommu, u16 did, u32 pasid)
{
	struct qi_desc desc = {};

	qi_desc_piotlb_all(did, pasid, &desc);
	qi_submit_sync(iommu, &desc, 1, 0);
}

/* PASID-based device IOTLB Invalidate */
void qi_flush_dev_iotlb_pasid(struct intel_iommu *iommu, u16 sid, u16 pfsid,
			      u32 pasid,  u16 qdep, u64 addr, unsigned int size_order)
{
	struct qi_desc desc = {.qw1 = 0, .qw2 = 0, .qw3 = 0};

	/*
	 * VT-d spec, section 4.3:
	 *
	 * Software is recommended to not submit any Device-TLB invalidation
	 * requests while address remapping hardware is disabled.
	 */
	if (!(iommu->gcmd & DMA_GCMD_TE))
		return;

	qi_desc_dev_iotlb_pasid(sid, pfsid, pasid,
				qdep, addr, size_order,
				&desc);
	qi_submit_sync(iommu, &desc, 1, 0);
}

void qi_flush_pasid_cache(struct intel_iommu *iommu, u16 did,
			  u64 granu, u32 pasid)
{
	struct qi_desc desc = {.qw1 = 0, .qw2 = 0, .qw3 = 0};

	desc.qw0 = QI_PC_PASID(pasid) | QI_PC_DID(did) |
			QI_PC_GRAN(granu) | QI_PC_TYPE;
	qi_submit_sync(iommu, &desc, 1, 0);
}

/*
 * Disable Queued Invalidation interface.
 */
void dmar_disable_qi(struct intel_iommu *iommu)
{
	unsigned long flags;
	u32 sts;
	cycles_t start_time = get_cycles();

	if (!ecap_qis(iommu->ecap))
		return;

	raw_spin_lock_irqsave(&iommu->register_lock, flags);

	sts =  readl(iommu->reg + DMAR_GSTS_REG);
	if (!(sts & DMA_GSTS_QIES))
		goto end;

	/*
	 * Give a chance to HW to complete the pending invalidation requests.
	 */
	while ((readl(iommu->reg + DMAR_IQT_REG) !=
		readl(iommu->reg + DMAR_IQH_REG)) &&
		(DMAR_OPERATION_TIMEOUT > (get_cycles() - start_time)))
		cpu_relax();

	iommu->gcmd &= ~DMA_GCMD_QIE;
	writel(iommu->gcmd, iommu->reg + DMAR_GCMD_REG);

	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, readl,
		      !(sts & DMA_GSTS_QIES), sts);
end:
	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);
}

/*
 * Enable queued invalidation.
 */
static void __dmar_enable_qi(struct intel_iommu *iommu)
{
	u32 sts;
	unsigned long flags;
	struct q_inval *qi = iommu->qi;
	u64 val = virt_to_phys(qi->desc);

	qi->free_head = qi->free_tail = 0;
	qi->free_cnt = QI_LENGTH;

	/*
	 * Set DW=1 and QS=1 in IQA_REG when Scalable Mode capability
	 * is present.
	 */
	if (ecap_smts(iommu->ecap))
		val |= BIT_ULL(11) | BIT_ULL(0);

	raw_spin_lock_irqsave(&iommu->register_lock, flags);

	/* write zero to the tail reg */
	writel(0, iommu->reg + DMAR_IQT_REG);

	writeq(val, iommu->reg + DMAR_IQA_REG);

	iommu->gcmd |= DMA_GCMD_QIE;
	writel(iommu->gcmd, iommu->reg + DMAR_GCMD_REG);

	/* Make sure hardware complete it */
	IOMMU_WAIT_OP(iommu, DMAR_GSTS_REG, readl, (sts & DMA_GSTS_QIES), sts);

	raw_spin_unlock_irqrestore(&iommu->register_lock, flags);
}

/*
 * Enable Queued Invalidation interface. This is a must to support
 * interrupt-remapping. Also used by DMA-remapping, which replaces
 * register based IOTLB invalidation.
 */
int dmar_enable_qi(struct intel_iommu *iommu)
{
	struct q_inval *qi;
	void *desc;

	if (!ecap_qis(iommu->ecap))
		return -ENOENT;

	/*
	 * queued invalidation is already setup and enabled.
	 */
	if (iommu->qi)
		return 0;

	iommu->qi = kmalloc_obj(*qi, GFP_ATOMIC);
	if (!iommu->qi)
		return -ENOMEM;

	qi = iommu->qi;

	/*
	 * Need two pages to accommodate 256 descriptors of 256 bits each
	 * if the remapping hardware supports scalable mode translation.
	 */
	desc = iommu_alloc_pages_node_sz(iommu->node, GFP_ATOMIC,
					 ecap_smts(iommu->ecap) ? SZ_8K :
								  SZ_4K);
	if (!desc) {
		kfree(qi);
		iommu->qi = NULL;
		return -ENOMEM;
	}

	qi->desc = desc;

	qi->desc_status = kzalloc_objs(int, QI_LENGTH, GFP_ATOMIC);
	if (!qi->desc_status) {
		iommu_free_pages(qi->desc);
		kfree(qi);
		iommu->qi = NULL;
		return -ENOMEM;
	}

	raw_spin_lock_init(&qi->q_lock);

	__dmar_enable_qi(iommu);

	return 0;
}

/* iommu interrupt handling. Most stuff are MSI-like. */

enum faulttype {
	DMA_REMAP,
	INTR_REMAP,
	UNKNOWN,
};

static const char *dma_remap_fault_reasons[] =
{
	"Software",
	"Present bit in root entry is clear",
	"Present bit in context entry is clear",
	"Invalid context entry",
	"Access beyond MGAW",
	"PTE Write access is not set",
	"PTE Read access is not set",
	"Next page table ptr is invalid",
	"Root table address invalid",
	"Context table ptr is invalid",
	"non-zero reserved fields in RTP",
	"non-zero reserved fields in CTP",
	"non-zero reserved fields in PTE",
	"PCE for translation request specifies blocking",
};

static const char * const dma_remap_sm_fault_reasons[] = {
	"SM: Invalid Root Table Address",
	"SM: TTM 0 for request with PASID",
	"SM: TTM 0 for page group request",
	"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", /* 0x33-0x37 */
	"SM: Error attempting to access Root Entry",
	"SM: Present bit in Root Entry is clear",
	"SM: Non-zero reserved field set in Root Entry",
	"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", /* 0x3B-0x3F */
	"SM: Error attempting to access Context Entry",
	"SM: Present bit in Context Entry is clear",
	"SM: Non-zero reserved field set in the Context Entry",
	"SM: Invalid Context Entry",
	"SM: DTE field in Context Entry is clear",
	"SM: PASID Enable field in Context Entry is clear",
	"SM: PASID is larger than the max in Context Entry",
	"SM: PRE field in Context-Entry is clear",
	"SM: RID_PASID field error in Context-Entry",
	"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", /* 0x49-0x4F */
	"SM: Error attempting to access the PASID Directory Entry",
	"SM: Present bit in Directory Entry is clear",
	"SM: Non-zero reserved field set in PASID Directory Entry",
	"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", /* 0x53-0x57 */
	"SM: Error attempting to access PASID Table Entry",
	"SM: Present bit in PASID Table Entry is clear",
	"SM: Non-zero reserved field set in PASID Table Entry",
	"SM: Invalid Scalable-Mode PASID Table Entry",
	"SM: ERE field is clear in PASID Table Entry",
	"SM: SRE field is clear in PASID Table Entry",
	"Unknown", "Unknown",/* 0x5E-0x5F */
	"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", /* 0x60-0x67 */
	"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", /* 0x68-0x6F */
	"SM: Error attempting to access first-level paging entry",
	"SM: Present bit in first-level paging entry is clear",
	"SM: Non-zero reserved field set in first-level paging entry",
	"SM: Error attempting to access FL-PML4 entry",
	"SM: First-level entry address beyond MGAW in Nested translation",
	"SM: Read permission error in FL-PML4 entry in Nested translation",
	"SM: Read permission error in first-level paging entry in Nested translation",
	"SM: Write permission error in first-level paging entry in Nested translation",
	"SM: Error attempting to access second-level paging entry",
	"SM: Read/Write permission error in second-level paging entry",
	"SM: Non-zero reserved field set in second-level paging entry",
	"SM: Invalid second-level page table pointer",
	"SM: A/D bit update needed in second-level entry when set up in no snoop",
	"Unknown", "Unknown", "Unknown", /* 0x7D-0x7F */
	"SM: Address in first-level translation is not canonical",
	"SM: U/S set 0 for first-level translation with user privilege",
	"SM: No execute permission for request with PASID and ER=1",
	"SM: Address beyond the DMA hardware max",
	"SM: Second-level entry address beyond the max",
	"SM: No write permission for Write/AtomicOp request",
	"SM: No read permission for Read/AtomicOp request",
	"SM: Invalid address-interrupt address",
	"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", /* 0x88-0x8F */
	"SM: A/D bit update needed in first-level entry when set up in no snoop",
};

static const char *irq_remap_fault_reasons[] =
{
	"Detected reserved fields in the decoded interrupt-remapped request",
	"Interrupt index exceeded the interrupt-remapping table size",
	"Present field in the IRTE entry is clear",
	"Error accessing interrupt-remapping table pointed by IRTA_REG",
	"Detected reserved fields in the IRTE entry",
	"Blocked a compatibility format interrupt request",
	"Blocked an interrupt request due to source-id verification failure",
};

static const char *dmar_get_fault_reason(u8 fault_reason, int *fault_type)
{
	if (fault_reason >= 0x20 && (fault_reason - 0x20 <
					ARRAY_SIZE(irq_remap_fault_reasons))) {
		*fault_type = INTR_REMAP;
		return irq_remap_fault_reasons[fault_reason - 0x20];
	} else if (fault_reason >= 0x30 && (fault_reason - 0x30 <
			ARRAY_SIZE(dma_remap_sm_fault_reasons))) {
		*fault_type = DMA_REMAP;
		return dma_remap_sm_fault_reasons[fault_reason - 0x30];
	} else if (fault_reason < ARRAY_SIZE(dma_remap_fault_reasons)) {
		*fault_type = DMA_REMAP;
		return dma_remap_fault_reasons[fault_reason];
	} else {
		*fault_type = UNKNOWN;
		return "Unknown";
	}
}


static inline int dmar_msi_reg(struct intel_iommu *iommu, int irq)
{
	if (iommu->irq == irq)
		return DMAR_FECTL_REG;
	else if (iommu->pr_irq == irq)
		return DMAR_PECTL_REG;
	else if (iommu->perf_irq == irq)
		return DMAR_PERFINTRCTL_REG;
	else
		BUG();
}

void dmar_msi_unmask(struct irq_data *data)
{
	struct intel_iommu *iommu = irq_data_get_irq_handler_data(data);
	int reg = dmar_msi_reg(iommu, data->irq);
	unsigned long flag;

	/* unmask it */
	raw_spin_lock_irqsave(&iommu->register_lock, flag);
	writel(0, iommu->reg + reg);
	/* Read a reg to force flush the post write */
	readl(iommu->reg + reg);
	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);
}

void dmar_msi_mask(struct irq_data *data)
{
	struct intel_iommu *iommu = irq_data_get_irq_handler_data(data);
	int reg = dmar_msi_reg(iommu, data->irq);
	unsigned long flag;

	/* mask it */
	raw_spin_lock_irqsave(&iommu->register_lock, flag);
	writel(DMA_FECTL_IM, iommu->reg + reg);
	/* Read a reg to force flush the post write */
	readl(iommu->reg + reg);
	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);
}

void dmar_msi_write(int irq, struct msi_msg *msg)
{
	struct intel_iommu *iommu = irq_get_handler_data(irq);
	int reg = dmar_msi_reg(iommu, irq);
	unsigned long flag;

	raw_spin_lock_irqsave(&iommu->register_lock, flag);
	writel(msg->data, iommu->reg + reg + 4);
	writel(msg->address_lo, iommu->reg + reg + 8);
	writel(msg->address_hi, iommu->reg + reg + 12);
	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);
}

static int dmar_fault_do_one(struct intel_iommu *iommu, int type,
		u8 fault_reason, u32 pasid, u16 source_id,
		unsigned long long addr)
{
	const char *reason;
	int fault_type;

	reason = dmar_get_fault_reason(fault_reason, &fault_type);

	if (fault_type == INTR_REMAP) {
		pr_err("[INTR-REMAP] Request device [%02x:%02x.%d] fault index 0x%llx [fault reason 0x%02x] %s\n",
		       source_id >> 8, PCI_SLOT(source_id & 0xFF),
		       PCI_FUNC(source_id & 0xFF), addr >> 48,
		       fault_reason, reason);

		return 0;
	}

	if (pasid == IOMMU_PASID_INVALID)
		pr_err("[%s NO_PASID] Request device [%02x:%02x.%d] fault addr 0x%llx [fault reason 0x%02x] %s\n",
		       type ? "DMA Read" : "DMA Write",
		       source_id >> 8, PCI_SLOT(source_id & 0xFF),
		       PCI_FUNC(source_id & 0xFF), addr,
		       fault_reason, reason);
	else
		pr_err("[%s PASID 0x%x] Request device [%02x:%02x.%d] fault addr 0x%llx [fault reason 0x%02x] %s\n",
		       type ? "DMA Read" : "DMA Write", pasid,
		       source_id >> 8, PCI_SLOT(source_id & 0xFF),
		       PCI_FUNC(source_id & 0xFF), addr,
		       fault_reason, reason);

	dmar_fault_dump_ptes(iommu, source_id, addr, pasid);

	return 0;
}

#define PRIMARY_FAULT_REG_LEN (16)
irqreturn_t dmar_fault(int irq, void *dev_id)
{
	struct intel_iommu *iommu = dev_id;
	int reg, fault_index;
	u32 fault_status;
	unsigned long flag;
	static DEFINE_RATELIMIT_STATE(rs,
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	raw_spin_lock_irqsave(&iommu->register_lock, flag);
	fault_status = readl(iommu->reg + DMAR_FSTS_REG);
	if (fault_status && __ratelimit(&rs))
		pr_err("DRHD: handling fault status reg %x\n", fault_status);

	/* TBD: ignore advanced fault log currently */
	if (!(fault_status & DMA_FSTS_PPF))
		goto unlock_exit;

	fault_index = dma_fsts_fault_record_index(fault_status);
	reg = cap_fault_reg_offset(iommu->cap);
	while (1) {
		/* Disable printing, simply clear the fault when ratelimited */
		bool ratelimited = !__ratelimit(&rs);
		u8 fault_reason;
		u16 source_id;
		u64 guest_addr;
		u32 pasid;
		int type;
		u32 data;
		bool pasid_present;

		/* highest 32 bits */
		data = readl(iommu->reg + reg +
				fault_index * PRIMARY_FAULT_REG_LEN + 12);
		if (!(data & DMA_FRCD_F))
			break;

		if (!ratelimited) {
			fault_reason = dma_frcd_fault_reason(data);
			type = dma_frcd_type(data);

			pasid = dma_frcd_pasid_value(data);
			data = readl(iommu->reg + reg +
				     fault_index * PRIMARY_FAULT_REG_LEN + 8);
			source_id = dma_frcd_source_id(data);

			pasid_present = dma_frcd_pasid_present(data);
			guest_addr = readq(iommu->reg + reg +
					   fault_index * PRIMARY_FAULT_REG_LEN);
			guest_addr = dma_frcd_page_addr(guest_addr);
		}

		/* clear the fault */
		writel(DMA_FRCD_F, iommu->reg + reg +
			fault_index * PRIMARY_FAULT_REG_LEN + 12);

		raw_spin_unlock_irqrestore(&iommu->register_lock, flag);

		if (!ratelimited)
			/* Using pasid -1 if pasid is not present */
			dmar_fault_do_one(iommu, type, fault_reason,
					  pasid_present ? pasid : IOMMU_PASID_INVALID,
					  source_id, guest_addr);

		fault_index++;
		if (fault_index >= cap_num_fault_regs(iommu->cap))
			fault_index = 0;
		raw_spin_lock_irqsave(&iommu->register_lock, flag);
	}

	writel(DMA_FSTS_PFO | DMA_FSTS_PPF | DMA_FSTS_PRO,
	       iommu->reg + DMAR_FSTS_REG);

unlock_exit:
	raw_spin_unlock_irqrestore(&iommu->register_lock, flag);
	return IRQ_HANDLED;
}

int dmar_set_interrupt(struct intel_iommu *iommu)
{
	int irq, ret;

	/*
	 * Check if the fault interrupt is already initialized.
	 */
	if (iommu->irq)
		return 0;

	irq = dmar_alloc_hwirq(iommu->seq_id, iommu->node, iommu);
	if (irq > 0) {
		iommu->irq = irq;
	} else {
		pr_err("No free IRQ vectors\n");
		return -EINVAL;
	}

	ret = request_irq(irq, dmar_fault, IRQF_NO_THREAD, iommu->name, iommu);
	if (ret)
		pr_err("Can't request irq\n");
	return ret;
}

int enable_drhd_fault_handling(unsigned int cpu)
{
	struct dmar_drhd_unit *drhd;
	struct intel_iommu *iommu;

	/*
	 * Enable fault control interrupt.
	 */
	guard(rwsem_read)(&dmar_global_lock);
	for_each_iommu(iommu, drhd) {
		u32 fault_status;
		int ret;

		if (iommu->irq || iommu->node != cpu_to_node(cpu))
			continue;

		ret = dmar_set_interrupt(iommu);

		if (ret) {
			pr_err("DRHD %Lx: failed to enable fault, interrupt, ret %d\n",
			       (unsigned long long)drhd->reg_base_addr, ret);
			return -1;
		}

		/*
		 * Clear any previous faults.
		 */
		dmar_fault(iommu->irq, iommu);
		fault_status = readl(iommu->reg + DMAR_FSTS_REG);
		writel(fault_status, iommu->reg + DMAR_FSTS_REG);
	}

	return 0;
}

/*
 * Re-enable Queued Invalidation interface.
 */
int dmar_reenable_qi(struct intel_iommu *iommu)
{
	if (!ecap_qis(iommu->ecap))
		return -ENOENT;

	if (!iommu->qi)
		return -ENOENT;

	/*
	 * First disable queued invalidation.
	 */
	dmar_disable_qi(iommu);
	/*
	 * Then enable queued invalidation again. Since there is no pending
	 * invalidation requests now, it's safe to re-enable queued
	 * invalidation.
	 */
	__dmar_enable_qi(iommu);

	return 0;
}

/*
 * Check interrupt remapping support in DMAR table description.
 */
int __init dmar_ir_support(void)
{
	struct acpi_table_dmar *dmar;
	dmar = (struct acpi_table_dmar *)dmar_tbl;
	if (!dmar)
		return 0;
	return dmar->flags & 0x1;
}

/* Check whether DMAR units are in use */
static inline bool dmar_in_use(void)
{
	return irq_remapping_enabled || intel_iommu_enabled;
}

static int __init dmar_free_unused_resources(void)
{
	struct dmar_drhd_unit *dmaru, *dmaru_n;

	if (dmar_in_use())
		return 0;

	if (dmar_dev_scope_status != 1 && !list_empty(&dmar_drhd_units))
		bus_unregister_notifier(&pci_bus_type, &dmar_pci_bus_nb);

	down_write(&dmar_global_lock);
	list_for_each_entry_safe(dmaru, dmaru_n, &dmar_drhd_units, list) {
		list_del(&dmaru->list);
		dmar_free_drhd(dmaru);
	}
	up_write(&dmar_global_lock);

	return 0;
}

late_initcall(dmar_free_unused_resources);

/*
 * DMAR Hotplug Support
 * For more details, please refer to Intel(R) Virtualization Technology
 * for Directed-IO Architecture Specifiction, Rev 2.2, Section 8.8
 * "Remapping Hardware Unit Hot Plug".
 */
static guid_t dmar_hp_guid =
	GUID_INIT(0xD8C1A3A6, 0xBE9B, 0x4C9B,
		  0x91, 0xBF, 0xC3, 0xCB, 0x81, 0xFC, 0x5D, 0xAF);

/*
 * Currently there's only one revision and BIOS will not check the revision id,
 * so use 0 for safety.
 */
#define	DMAR_DSM_REV_ID			0
#define	DMAR_DSM_FUNC_DRHD		1
#define	DMAR_DSM_FUNC_ATSR		2
#define	DMAR_DSM_FUNC_RHSA		3
#define	DMAR_DSM_FUNC_SATC		4

static inline bool dmar_detect_dsm(acpi_handle handle, int func)
{
	return acpi_check_dsm(handle, &dmar_hp_guid, DMAR_DSM_REV_ID, 1 << func);
}

static int dmar_walk_dsm_resource(acpi_handle handle, int func,
				  dmar_res_handler_t handler, void *arg)
{
	int ret = -ENODEV;
	union acpi_object *obj;
	struct acpi_dmar_header *start;
	struct dmar_res_callback callback;
	static int res_type[] = {
		[DMAR_DSM_FUNC_DRHD] = ACPI_DMAR_TYPE_HARDWARE_UNIT,
		[DMAR_DSM_FUNC_ATSR] = ACPI_DMAR_TYPE_ROOT_ATS,
		[DMAR_DSM_FUNC_RHSA] = ACPI_DMAR_TYPE_HARDWARE_AFFINITY,
		[DMAR_DSM_FUNC_SATC] = ACPI_DMAR_TYPE_SATC,
	};

	if (!dmar_detect_dsm(handle, func))
		return 0;

	obj = acpi_evaluate_dsm_typed(handle, &dmar_hp_guid, DMAR_DSM_REV_ID,
				      func, NULL, ACPI_TYPE_BUFFER);
	if (!obj)
		return -ENODEV;

	memset(&callback, 0, sizeof(callback));
	callback.cb[res_type[func]] = handler;
	callback.arg[res_type[func]] = arg;
	start = (struct acpi_dmar_header *)obj->buffer.pointer;
	ret = dmar_walk_remapping_entries(start, obj->buffer.length, &callback);

	ACPI_FREE(obj);

	return ret;
}

static int dmar_hp_add_drhd(struct acpi_dmar_header *header, void *arg)
{
	int ret;
	struct dmar_drhd_unit *dmaru;

	dmaru = dmar_find_dmaru((struct acpi_dmar_hardware_unit *)header);
	if (!dmaru)
		return -ENODEV;

	ret = dmar_ir_hotplug(dmaru, true);
	if (ret == 0)
		ret = dmar_iommu_hotplug(dmaru, true);

	return ret;
}

static int dmar_hp_remove_drhd(struct acpi_dmar_header *header, void *arg)
{
	int i, ret;
	struct device *dev;
	struct dmar_drhd_unit *dmaru;

	dmaru = dmar_find_dmaru((struct acpi_dmar_hardware_unit *)header);
	if (!dmaru)
		return 0;

	/*
	 * All PCI devices managed by this unit should have been destroyed.
	 */
	if (!dmaru->include_all && dmaru->devices && dmaru->devices_cnt) {
		for_each_active_dev_scope(dmaru->devices,
					  dmaru->devices_cnt, i, dev)
			return -EBUSY;
	}

	ret = dmar_ir_hotplug(dmaru, false);
	if (ret == 0)
		ret = dmar_iommu_hotplug(dmaru, false);

	return ret;
}

static int dmar_hp_release_drhd(struct acpi_dmar_header *header, void *arg)
{
	struct dmar_drhd_unit *dmaru;

	dmaru = dmar_find_dmaru((struct acpi_dmar_hardware_unit *)header);
	if (dmaru) {
		list_del_rcu(&dmaru->list);
		synchronize_rcu();
		dmar_free_drhd(dmaru);
	}

	return 0;
}

static int dmar_hotplug_insert(acpi_handle handle)
{
	int ret;
	int drhd_count = 0;

	ret = dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
				     &dmar_validate_one_drhd, (void *)1);
	if (ret)
		goto out;

	ret = dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
				     &dmar_parse_one_drhd, (void *)&drhd_count);
	if (ret == 0 && drhd_count == 0) {
		pr_warn(FW_BUG "No DRHD structures in buffer returned by _DSM method\n");
		goto out;
	} else if (ret) {
		goto release_drhd;
	}

	ret = dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_RHSA,
				     &dmar_parse_one_rhsa, NULL);
	if (ret)
		goto release_drhd;

	ret = dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_ATSR,
				     &dmar_parse_one_atsr, NULL);
	if (ret)
		goto release_atsr;

	ret = dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
				     &dmar_hp_add_drhd, NULL);
	if (!ret)
		return 0;

	dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
			       &dmar_hp_remove_drhd, NULL);
release_atsr:
	dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_ATSR,
			       &dmar_release_one_atsr, NULL);
release_drhd:
	dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
			       &dmar_hp_release_drhd, NULL);
out:
	return ret;
}

static int dmar_hotplug_remove(acpi_handle handle)
{
	int ret;

	ret = dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_ATSR,
				     &dmar_check_one_atsr, NULL);
	if (ret)
		return ret;

	ret = dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
				     &dmar_hp_remove_drhd, NULL);
	if (ret == 0) {
		WARN_ON(dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_ATSR,
					       &dmar_release_one_atsr, NULL));
		WARN_ON(dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
					       &dmar_hp_release_drhd, NULL));
	} else {
		dmar_walk_dsm_resource(handle, DMAR_DSM_FUNC_DRHD,
				       &dmar_hp_add_drhd, NULL);
	}

	return ret;
}

static acpi_status dmar_get_dsm_handle(acpi_handle handle, u32 lvl,
				       void *context, void **retval)
{
	acpi_handle *phdl = retval;

	if (dmar_detect_dsm(handle, DMAR_DSM_FUNC_DRHD)) {
		*phdl = handle;
		return AE_CTRL_TERMINATE;
	}

	return AE_OK;
}

static int dmar_device_hotplug(acpi_handle handle, bool insert)
{
	int ret;
	acpi_handle tmp = NULL;
	acpi_status status;

	if (!dmar_in_use())
		return 0;

	if (dmar_detect_dsm(handle, DMAR_DSM_FUNC_DRHD)) {
		tmp = handle;
	} else {
		status = acpi_walk_namespace(ACPI_TYPE_DEVICE, handle,
					     ACPI_UINT32_MAX,
					     dmar_get_dsm_handle,
					     NULL, NULL, &tmp);
		if (ACPI_FAILURE(status)) {
			pr_warn("Failed to locate _DSM method.\n");
			return -ENXIO;
		}
	}
	if (tmp == NULL)
		return 0;

	down_write(&dmar_global_lock);
	if (insert)
		ret = dmar_hotplug_insert(tmp);
	else
		ret = dmar_hotplug_remove(tmp);
	up_write(&dmar_global_lock);

	return ret;
}

int dmar_device_add(acpi_handle handle)
{
	return dmar_device_hotplug(handle, true);
}

int dmar_device_remove(acpi_handle handle)
{
	return dmar_device_hotplug(handle, false);
}

/*
 * dmar_platform_optin - Is %DMA_CTRL_PLATFORM_OPT_IN_FLAG set in DMAR table
 *
 * Returns true if the platform has %DMA_CTRL_PLATFORM_OPT_IN_FLAG set in
 * the ACPI DMAR table. This means that the platform boot firmware has made
 * sure no device can issue DMA outside of RMRR regions.
 */
bool dmar_platform_optin(void)
{
	struct acpi_table_dmar *dmar;
	acpi_status status;
	bool ret;

	status = acpi_get_table(ACPI_SIG_DMAR, 0,
				(struct acpi_table_header **)&dmar);
	if (ACPI_FAILURE(status))
		return false;

	ret = !!(dmar->flags & DMAR_PLATFORM_OPT_IN);
	acpi_put_table((struct acpi_table_header *)dmar);

	return ret;
}
EXPORT_SYMBOL_GPL(dmar_platform_optin);
