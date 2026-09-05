// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for RISC-V IOMMU implementations.
 *
 * Copyright © 2022-2024 Rivos Inc.
 * Copyright © 2023 FORTH-ICS/CARV
 *
 * Authors
 *	Tomasz Jeznach <tjeznach@rivosinc.com>
 *	Nick Kossifidis <mick@ics.forth.gr>
 */

/*
 * [한국어 설명] RISC-V IOMMU 드라이버 본체 (riscv/iommu.c)
 *
 * === 파일의 역할 ===
 * RISC-V IOMMU 규격(1.0)을 구현한 하드웨어를 리눅스 IOMMU API에
 * 붙인다. 페이지 테이블 자체는 generic_pt(pt_iommu_riscv_64)가
 * 맡으므로, 이 파일이 다루는 것은 그 위의 세 가지다:
 * **디바이스 디렉터리, 명령 큐, 무효화 프로토콜.**
 *
 * 이해에 필요한 개념이 다섯이다.
 *
 * (1) **디바이스 디렉터리 테이블(DDT).** 디바이스 ID를 받아
 *     디바이스 컨텍스트(DC)를 찾아 주는 1~3단계 테이블이다.
 *     DC 안에 그 디바이스가 쓸 페이지 테이블의 루트(fsc)와
 *     TLB 태그가 될 PSCID(ta)가 들어 있다. 즉 "어느 디바이스가
 *     어느 주소 공간을 쓰는가"가 여기서 결정된다.
 *
 * (2) **두 개의 링 버퍼 큐.** 명령 큐(CQ)로 무효화와 동기화
 *     명령을 보내고, 폴트 큐(FQ)로 오류를 받는다. 둘 다
 *     메모리에 있는 링 버퍼이고, 하드웨어와는 head/tail
 *     레지스터(도어벨)로 동기화한다. 큐 코드가 이 파일의
 *     앞 절반을 차지한다.
 *
 * (3) **WARL 레지스터로 능력을 발견한다.** RISC-V의 관례로,
 *     레지스터에 원하는 값을 쓰고 되읽어 하드웨어가 실제로
 *     받아들인 값을 확인한다. 큐 길이, DDT 단계 수, 큐의 고정
 *     주소가 모두 이 방식으로 정해진다 — 그래서 "쓰고 → 읽고
 *     → 비교"하는 코드가 반복된다.
 *
 * (4) **PSCID가 무효화의 단위다.** 각 도메인이 PSCID를 하나
 *     받아 그것으로 TLB 항목이 태그된다. 무효화는 주소가 아니라
 *     PSCID를 기준으로 이뤄지며, 범위가 2MB를 넘으면 아예
 *     그 PSCID 전체를 비운다.
 *
 * (5) **bond 목록과 그 정렬.** 도메인에 붙은 디바이스들을
 *     bond로 잇는데, **같은 IOMMU에 속한 것들이 이웃하도록**
 *     정렬해 둔다. 그러면 무효화 순회에서 직전 IOMMU와 같은지만
 *     비교해 중복 명령을 걸러 낼 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [ACPI RIMT 또는 디바이스 트리] iommus = <&iommu devid>
 *        ↓ of_xlate
 *   [이 파일] fwspec에 디바이스 ID를 등록
 *        ↓ probe_device
 *   [이 파일] DDT에 DC 자리를 만들되 아직 무효로 둔다
 *        ↓ attach_dev
 *   [이 파일] bond를 걸고 → DC에 fsc/ta를 쓰고 → 캐시를 무효화
 *
 *   [디바이스 드라이버] dma_map_*()
 *        ↓
 *   [dma-iommu] IOVA 할당
 *        ↓
 *   [generic_pt] 실제 페이지 테이블 조작
 *        ↓ iotlb_sync
 *   [이 파일] bond 목록의 모든 IOMMU에 IOTINVAL + IOFENCE.C
 *        ↑ FQ
 *   [이 파일] riscv_iommu_fault()로 폴트를 로그에 남긴다
 *
 * 실행 컨텍스트: 명령 전송은 local_irq_save로 감싼 락 없는
 * 다중 생산자 구조다. 큐 인터럽트는 스레드 처리기로 받는다.
 * 무효화 순회는 RCU 읽기 구역이다.
 *
 * === 타 모듈과의 연결 ===
 * - generic_pt/iommu.h와 pt_iommu_riscv_64: 페이지 테이블 전부.
 *   이 파일은 그 루트 주소와 모드를 DC에 옮겨 심을 뿐이다.
 * - iommu-bits.h: 규격이 정의한 모든 레지스터, 비트 필드, 명령
 *   조립 헬퍼(riscv_iommu_cmd_* 계열).
 * - iommu.h(이 디렉터리): riscv_iommu_device 구조체와 레지스터
 *   접근 래퍼(readl/writeq와 타임아웃 변형).
 * - acpi_rimt.h: ACPI 환경에서 IOMMU를 등록하는 경로.
 * 데이터 흐름: 디바이스 ID → DDT 워크 → DC → fsc(페이지 테이블
 * 루트) → generic_pt가 만든 테이블 → 물리 주소.
 *
 * === 주요 함수/구조체 요약 ===
 * - riscv_iommu_queue_send(): 링 버퍼에 항목을 넣고 도어벨을
 *   울린다. 일곱 단계의 메모리 순서 규약이 이 함수의 전부다.
 * - riscv_iommu_get_dc(): 디바이스 ID로 DC를 찾는다. 중간 단계가
 *   없으면 cmpxchg로 경쟁 없이 만들어 붙인다.
 * - riscv_iommu_iodir_update(): DC를 갱신한다. 유효 비트를 내리고
 *   → 무효화하고 → 값을 쓰고 → 유효 비트를 올리는 순서가 핵심이다.
 * - riscv_iommu_iotlb_inval(): bond 목록을 훑으며 IOMMU마다 한
 *   번씩 무효화한다. 정렬 덕분에 중복이 걸러진다.
 * - riscv_iommu_iodir_set_mode(): WARL로 지원되는 DDT 단계 수를
 *   찾아 내려가며 설정한다.
 */

/* [한국어] 이 드라이버의 모든 로그에 "riscv-iommu: " 접두사를 붙인다. */
#define pr_fmt(fmt) "riscv-iommu: " fmt

/* [한국어] acpi_disabled — ACPI 환경인지 판별한다. */
#include <linux/acpi.h>
/* [한국어] rimt_iommu_register — ACPI의 RIMT 표에 IOMMU를 등록한다. */
#include <linux/acpi_rimt.h>
/* [한국어] READ_ONCE/WRITE_ONCE 등 컴파일러 배리어. */
#include <linux/compiler.h>
/* [한국어] is_kdump_kernel() — 크래시 덤프 커널인지 확인해,
 * 앞 커널이 켜 둔 IOMMU를 강제로 끌지 정한다. */
#include <linux/crash_dump.h>
/* [한국어] 초기화 매크로. */
#include <linux/init.h>
/* [한국어] IOMMU 코어 계약. */
#include <linux/iommu.h>
/* [한국어] readx_poll_timeout — 큐 상태와 하드웨어 응답 대기. */
#include <linux/iopoll.h>
/* [한국어] max3, ilog2 등 기본 도우미. */
#include <linux/kernel.h>
/* [한국어] PCI 디바이스 그룹 판별. */
#include <linux/pci.h>
/* [한국어] generic_pt의 IOMMU 결합 계층. 페이지 테이블 전부를
 * 여기에 맡긴다 — 이 파일에는 PTE를 만지는 코드가 없다. */
#include <linux/generic_pt/iommu.h>

/* [한국어] iommu_alloc_pages_node_sz — NUMA 노드를 지정한 테이블 할당. */
#include "../iommu-pages.h"
/* [한국어] 규격이 정의한 레지스터/비트/명령 정의. 이 드라이버의
 * 하드웨어 지식이 전부 여기서 온다. */
#include "iommu-bits.h"
/* [한국어] riscv_iommu_device 구조체와 레지스터 접근 래퍼. */
#include "iommu.h"

/* Timeouts in [us] */
/* [한국어] 큐 제어 레지스터가 busy를 내릴 때까지 기다릴 시간(0.15초). */
#define RISCV_IOMMU_QCSR_TIMEOUT	150000
/* [한국어] 큐의 head/tail 레지스터 접근과 공간 확보를 기다릴 시간. */
#define RISCV_IOMMU_QUEUE_TIMEOUT	150000
/* [한국어] DDTP 레지스터의 busy를 기다릴 시간(10초).
 * 디렉터리 모드 전환은 하드웨어가 내부 캐시를 정리해야 해 느리다. */
#define RISCV_IOMMU_DDTP_TIMEOUT	10000000
/* [한국어] 무효화 명령의 완료를 기다릴 시간(90초).
 * 극단적으로 긴 이유: IOFENCE.C는 앞선 모든 명령의 완료를 보장하는데,
 * 큐에 쌓인 무효화가 많으면 그만큼 오래 걸릴 수 있다. */
#define RISCV_IOMMU_IOTINVAL_TIMEOUT	90000000

/* Number of entries per CMD/FLT queue, should be <= INT_MAX */
/* [한국어] 명령 큐의 기본 항목 수. 실제 크기는 하드웨어가
 * 허용하는 값으로 줄어들 수 있다(WARL). */
#define RISCV_IOMMU_DEF_CQ_COUNT	8192
/* [한국어] 폴트 큐의 기본 항목 수. 명령 큐보다 작은 것은
 * 폴트가 명령만큼 자주 나지 않는다는 가정이다. */
#define RISCV_IOMMU_DEF_FQ_COUNT	4096

/* RISC-V IOMMU PPN <> PHYS address conversions, PHYS <=> PPN[53:10] */
/* [한국어] 물리 주소를 레지스터의 PPN 필드 형식으로 바꾼다.
 * 2비트만 미는 이유: PPN은 페이지 번호(12비트 시프트)인데
 * 레지스터 안에서는 비트 10부터 놓이므로 차이가 10비트다. */
#define phys_to_ppn(pa)  (((pa) >> 2) & (((1ULL << 44) - 1) << 10))
/* [한국어] 그 역변환. 위와 정확히 대칭이다. */
#define ppn_to_phys(pn)	 (((pn) << 2) & (((1ULL << 44) - 1) << 12))

/* [한국어] 디바이스에서 그것을 담당하는 IOMMU를 얻는다.
 * 코어가 관리하는 역참조를 쓰므로 별도 포인터를 두지 않아도 된다. */
#define dev_to_iommu(dev) \
	iommu_get_iommu_dev(dev, struct riscv_iommu_device, iommu)	/* [한국어] 코어가 관리하는 역참조로 담당 IOMMU를 찾는다. */

/* IOMMU PSCID allocation namespace. */
/* [한국어] PSCID 할당기. 도메인마다 하나씩 받아 TLB 태그로 쓴다.
 * 시스템 전역인 이유: 여러 IOMMU가 같은 PSCID를 다른 의미로 쓰면
 * 무효화가 엉뚱한 곳에 미치기 때문이다. */
static DEFINE_IDA(riscv_iommu_pscids);
/* [한국어] PSCID의 최대값(20비트). 규격이 정한 폭이다. */
#define RISCV_IOMMU_MAX_PSCID		(BIT(20) - 1)

/* Device resource-managed allocations */
/* [한국어] devres로 관리되는 페이지 할당의 추적 정보.
 * 큐 버퍼와 DDT 노드를 디바이스 수명에 묶기 위한 장치다. */
struct riscv_iommu_devres {
	void *addr;
	/* [한국어] 관리 대상 페이지의 커널 가상 주소.
	 * 설정자: riscv_iommu_get_pages().
	 * 읽는 자: 해제 콜백과, 명시적 해제 시 대상을 찾는 비교 함수. */
};

/*
 * [한국어]
 * riscv_iommu_devres_pages_release - devres 해제 콜백
 *
 * @dev: 소유 디바이스.
 * @res: 추적 정보.
 * @return: 없음.
 *
 * 디바이스가 사라질 때 커널이 자동으로 부른다. 그래서 이 드라이버는
 * 큐 버퍼와 DDT 노드를 일일이 추적해 해제할 필요가 없다.
 *
 * 실행 컨텍스트: 디바이스 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   devres 코어 → [riscv_iommu_devres_pages_release]
 */
static void riscv_iommu_devres_pages_release(struct device *dev, void *res)
{
	/* [한국어] 추적 정보에서 해제할 주소를 꺼낸다. */
	struct riscv_iommu_devres *devres = res;

	iommu_free_pages(devres->addr);	/* [한국어] 추적하던 페이지를 반납한다. */
}

/*
 * [한국어]
 * riscv_iommu_devres_pages_match - 명시적 해제에서 대상을 찾는 비교 함수
 *
 * @dev: 소유 디바이스.
 * @res: 검사할 추적 정보.
 * @p: 찾는 대상(주소가 담긴 임시 구조체).
 * @return: 같으면 참.
 *
 * DDT 설치 경쟁에서 진 페이지처럼, 디바이스 수명을 기다리지 않고
 * 지금 반납해야 하는 경우에 쓰인다.
 *
 * 실행 컨텍스트: devres_release 안. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_free_pages() → devres 코어
 *   → [riscv_iommu_devres_pages_match]
 */
static int riscv_iommu_devres_pages_match(struct device *dev, void *res, void *p)
{
	/* [한국어] 목록에서 검사 중인 항목. */
	struct riscv_iommu_devres *devres = res;
	/* [한국어] 찾고 있는 주소를 담은 임시 구조체. */
	struct riscv_iommu_devres *target = p;

	/* [한국어] 주소가 같으면 그것이 반납할 항목이다. */
	return devres->addr == target->addr;
}

/*
 * [한국어]
 * riscv_iommu_get_pages - 디바이스 수명에 묶인 페이지를 할당한다
 *
 * @iommu: 소유 IOMMU.
 * @size: 요청 크기.
 * @return: 페이지의 커널 가상 주소, 실패하면 NULL.
 *
 * IOMMU가 있는 NUMA 노드에서 할당하는 것이 요점이다. 하드웨어가
 * DMA로 이 메모리를 읽으므로, 가까운 노드일수록 지연이 짧다.
 *
 * GFP_KERNEL_ACCOUNT를 쓰는 이유: 이 메모리가 결국 사용자의
 * 요청(도메인 생성, 디바이스 연결)에 따라 늘어나므로 cgroup
 * 메모리 회계에 잡혀야 한다.
 *
 * 실행 컨텍스트: 초기화와 DDT 확장. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   queue_alloc() / get_dc() / iodir_alloc() → [riscv_iommu_get_pages]
 */
static void *riscv_iommu_get_pages(struct riscv_iommu_device *iommu,
				   unsigned int size)
{
	/* [한국어] devres 추적 정보. */
	struct riscv_iommu_devres *devres;
	/* [한국어] 할당한 페이지. */
	void *addr;

	/* [한국어] IOMMU와 같은 NUMA 노드에서 잡는다 — 하드웨어가
	 * DMA로 읽을 메모리이므로 거리가 성능에 직결된다. */
	addr = iommu_alloc_pages_node_sz(dev_to_node(iommu->dev),
					 GFP_KERNEL_ACCOUNT, size);
	if (unlikely(!addr))	/* [한국어] 가까운 노드에서 잡지 못했다. */
		return NULL;

	/* [한국어] 이 페이지를 디바이스 수명에 묶을 추적 정보를 만든다. */
	devres = devres_alloc(riscv_iommu_devres_pages_release,
			      sizeof(struct riscv_iommu_devres), GFP_KERNEL);

	if (unlikely(!devres)) {
		/* [한국어] 추적할 수 없으면 누수가 되므로 되돌린다. */
		iommu_free_pages(addr);
		return NULL;	/* [한국어] 추적할 수 없으면 누수가 되므로 포기한다. */
	}

	/* [한국어] 해제 콜백이 쓸 주소를 기록한다. */
	devres->addr = addr;

	/* [한국어] 디바이스의 관리 목록에 등록한다. 이제 디바이스가
	 * 사라질 때 자동으로 반납된다. */
	devres_add(iommu->dev, devres);

	return addr;	/* [한국어] 등록까지 마친 페이지를 돌려준다. */
}

/*
 * [한국어]
 * riscv_iommu_free_pages - devres로 관리되는 페이지를 지금 반납한다
 *
 * @iommu: 소유 IOMMU.
 * @addr: 반납할 주소.
 * @return: 없음.
 *
 * 디바이스 수명을 기다리지 않고 즉시 반납해야 하는 경우가 있다 —
 * DDT 노드 설치 경쟁에서 졌을 때다. 그때 이미 devres에 등록된
 * 페이지를 목록에서 찾아 빼내야 하므로 비교 함수가 필요하다.
 *
 * 실행 컨텍스트: DDT 확장 경로.
 *
 * 호출 체인:
 *   riscv_iommu_get_dc() → [riscv_iommu_free_pages]
 */
static void riscv_iommu_free_pages(struct riscv_iommu_device *iommu, void *addr)
{
	/* [한국어] 찾을 주소만 담은 임시 구조체. 비교 함수가 이것과
	 * 목록의 항목을 견준다. */
	struct riscv_iommu_devres devres = { .addr = addr };

	/* [한국어] 목록에서 해당 항목을 찾아 해제 콜백을 부르고 뺀다. */
	devres_release(iommu->dev, riscv_iommu_devres_pages_release,
		       riscv_iommu_devres_pages_match, &devres);
}

/*
 * Hardware queue allocation and management.
 */

/* Setup queue base, control registers and default queue length */
/* [한국어] 큐 구조체에 그 큐의 레지스터 오프셋과 기본 크기를 채운다.
 * 이름 이어 붙이기(CQ/FQ)로 세 개의 상수를 한 번에 만들어 내는 것이
 * 이 매크로의 요점이다 — 큐마다 같은 코드를 반복하지 않게 한다.
 *
 * mask를 조건부로 두는 이유: 호출 전에 이미 값이 들어 있다면
 * (커널 파라미터 등으로 지정된 경우) 그것을 존중한다. */
#define RISCV_IOMMU_QUEUE_INIT(q, name) do {				\
	struct riscv_iommu_queue *_q = q;				\
	_q->qid = RISCV_IOMMU_INTR_ ## name;				\
	_q->qbr = RISCV_IOMMU_REG_ ## name ## B;			\
	_q->qcr = RISCV_IOMMU_REG_ ## name ## CSR;			\
	_q->mask = _q->mask ?: (RISCV_IOMMU_DEF_ ## name ## _COUNT) - 1;\
} while (0)	/* [한국어] 매크로 본문을 하나의 문장으로 묶는 관용구. */

/* Note: offsets are the same for all queues */
/* [한국어] 이 큐의 head(소비자) 레지스터 주소.
 * 원본 주석이 밝히듯 모든 큐에서 base 대비 오프셋이 같아,
 * CQ의 차이를 그대로 다른 큐에도 적용할 수 있다. */
#define Q_HEAD(q) ((q)->qbr + (RISCV_IOMMU_REG_CQH - RISCV_IOMMU_REG_CQB))
/* [한국어] 이 큐의 tail(생산자) 레지스터 주소. 같은 원리다. */
#define Q_TAIL(q) ((q)->qbr + (RISCV_IOMMU_REG_CQT - RISCV_IOMMU_REG_CQB))
/* [한국어] 무한히 증가하는 인덱스를 링 버퍼 안의 자리로 접는다.
 * mask가 (2^n - 1)이라 이 AND 하나로 나머지 연산을 대신한다. */
#define Q_ITEM(q, index) ((q)->mask & (index))
/* [한국어] 인터럽트 상태 레지스터에서 이 큐에 해당하는 비트.
 * 큐 ID가 곧 비트 번호다. */
#define Q_IPSR(q) BIT((q)->qid)

/*
 * Discover queue ring buffer hardware configuration, allocate in-memory
 * ring buffer or use fixed I/O memory location, configure queue base register.
 * Must be called before hardware queue is enabled.
 *
 * @queue - data structure, configured with RISCV_IOMMU_QUEUE_INIT()
 * @entry_size - queue single element size in bytes.
 */
/*
 * [한국어]
 * riscv_iommu_queue_alloc - 큐의 링 버퍼를 마련하고 하드웨어에 알린다
 *
 * @iommu: 대상 IOMMU.
 * @queue: 초기화된 큐 구조체.
 * @entry_size: 항목 하나의 크기.
 * @return: 0 성공, -ENOMEM/-ENODEV.
 *
 * RISC-V의 WARL 관례가 세 번 등장하는 함수다.
 *
 *  1) 크기 필드에 최대값을 써 보고 되읽으면, 하드웨어가 지원하는
 *     최대 크기를 알 수 있다. 요청한 크기가 그보다 크면 줄인다.
 *  2) 같은 읽기에서 PPN 필드가 0이 아니면, 하드웨어가 큐를 둘
 *     **고정 주소를 지정한 것**이다. 그러면 메모리를 할당하지 않고
 *     그 자리를 ioremap 한다.
 *  3) 마지막으로 완성한 값을 쓰고 되읽어, 하드웨어가 실제로
 *     받아들였는지 확인한다.
 *
 * 할당 실패 시 크기를 반으로 줄여 재시도하는 루프도 눈여겨볼 만하다 —
 * 큰 연속 메모리를 못 얻어도 작은 큐로 동작할 수 있게 한다.
 *
 * 실행 컨텍스트: 초기화. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_init() → [riscv_iommu_queue_alloc]
 */
static int riscv_iommu_queue_alloc(struct riscv_iommu_device *iommu,
				   struct riscv_iommu_queue *queue,
				   size_t entry_size)
{
	/* [한국어] 최종 결정된 크기의 로그값. */
	unsigned int logsz;
	/* [한국어] 쓰고 되읽을 base 레지스터 값. */
	u64 qb, rb;

	/*
	 * Use WARL base register property to discover maximum allowed
	 * number of entries and optional fixed IO address for queue location.
	 */
	/* [한국어] 크기 필드를 모두 1로 써 보고 되읽으면, 하드웨어가
	 * 받아들일 수 있는 최대값이 남는다 — WARL의 표준 사용법이다. */
	riscv_iommu_writeq(iommu, queue->qbr, RISCV_IOMMU_QUEUE_LOG2SZ_FIELD);
	qb = riscv_iommu_readq(iommu, queue->qbr);	/* [한국어] 하드웨어가 받아들인 값을 되읽어 능력을 알아낸다. */

	/*
	 * Calculate and verify hardware supported queue length, as reported
	 * by the field LOG2SZ, where max queue length is equal to 2^(LOG2SZ + 1).
	 * Update queue size based on hardware supported value.
	 */
	/* [한국어] 우리가 원한 크기의 로그값. mask가 (2^n - 1)이라
	 * ilog2가 곧 n이다. */
	logsz = ilog2(queue->mask);
	/* [한국어] 하드웨어가 지원하는 것보다 크면 그쪽에 맞춘다. */
	if (logsz > FIELD_GET(RISCV_IOMMU_QUEUE_LOG2SZ_FIELD, qb))
		logsz = FIELD_GET(RISCV_IOMMU_QUEUE_LOG2SZ_FIELD, qb);

	/*
	 * Use WARL base register property to discover an optional fixed IO
	 * address for queue ring buffer location. Otherwise allocate contiguous
	 * system memory.
	 */
	/* [한국어] 되읽은 값에 주소가 들어 있다면, 하드웨어가 큐를 둘
	 * 자리를 이미 정해 둔 것이다(내장 SRAM 등). */
	if (FIELD_GET(RISCV_IOMMU_PPN_FIELD, qb)) {
		/* [한국어] 항목 수는 2^(logsz+1)이므로 크기가 이렇게 나온다. */
		const size_t queue_size = entry_size << (logsz + 1);

		/* [한국어] 하드웨어가 지정한 물리 주소를 그대로 쓴다. */
		queue->phys = PFN_PHYS(FIELD_GET(RISCV_IOMMU_PPN_FIELD, qb));
		queue->base = devm_ioremap(iommu->dev, queue->phys, queue_size);	/* [한국어] 하드웨어가 지정한 자리를 그대로 매핑해 쓴다. */
	} else {
		/* [한국어] 시스템 메모리에서 연속 영역을 잡는다. */
		do {
			/* [한국어] 이번 시도의 크기. */
			const size_t queue_size = entry_size << (logsz + 1);

			/* [한국어] 최소 4KB는 잡는다 — 작은 큐라도 페이지
			 * 단위 할당이 더 단순하기 때문이다. */
			queue->base = riscv_iommu_get_pages(
				iommu, max(queue_size, SZ_4K));
			queue->phys = __pa(queue->base);
		/* [한국어] 실패하면 크기를 절반으로 줄여 다시 시도한다 —
		 * 큰 연속 메모리를 못 얻어도 동작하게 하려는 것이다. */
		} while (!queue->base && logsz-- > 0);
	}

	/* [한국어] 끝내 자리를 얻지 못했다. */
	if (!queue->base)
		return -ENOMEM;

	/* [한국어] 주소와 크기를 합쳐 base 레지스터 값을 조립한다. */
	qb = phys_to_ppn(queue->phys) |
	     FIELD_PREP(RISCV_IOMMU_QUEUE_LOG2SZ_FIELD, logsz);

	/* Update base register and read back to verify hw accepted our write */
	/* [한국어] 쓰고 되읽어 하드웨어가 그대로 받아들였는지 확인한다.
	 * 다르면 이 구성으로는 큐를 쓸 수 없다. */
	riscv_iommu_writeq(iommu, queue->qbr, qb);
	rb = riscv_iommu_readq(iommu, queue->qbr);	/* [한국어] 실제로 반영되었는지 되읽는다. */
	if (rb != qb) {	/* [한국어] 하드웨어가 우리 값을 받아들이지 않았다. */
		dev_err(iommu->dev, "queue #%u allocation failed\n", queue->qid);	/* [한국어] 어느 큐에서 실패했는지 남긴다. */
		return -ENODEV;	/* [한국어] 이 구성으로는 큐를 쓸 수 없다. */
	}

	/* Update actual queue mask */
	/* [한국어] 실제로 확정된 크기로 마스크를 갱신한다. 이후 모든
	 * 인덱스 접기(Q_ITEM)가 이 값을 쓴다. */
	queue->mask = (2U << logsz) - 1;

	dev_dbg(iommu->dev, "queue #%u allocated 2^%u entries",	/* [한국어] 확정된 항목 수를 디버그 로그로 남긴다. */
		queue->qid, logsz + 1);

	return 0;	/* [한국어] 큐 버퍼가 준비됐다. */
}

/* Check interrupt queue status, IPSR */
/*
 * [한국어]
 * riscv_iommu_queue_ipsr - 큐 인터럽트의 1차(하드) 처리기
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @data: 등록 시 넘긴 큐.
 * @return: 우리 큐의 인터럽트면 IRQ_WAKE_THREAD, 아니면 IRQ_NONE.
 *
 * 인터럽트를 여러 큐가 공유할 수 있으므로(IRQF_SHARED),
 * 상태 레지스터에서 자기 비트를 확인해 자기 것인지 가린다.
 *
 * 실제 처리는 스레드 처리기에 맡긴다 — 큐를 비우고 오류를 로그로
 * 남기는 일이 인터럽트 문맥에서 하기에는 무겁기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [riscv_iommu_queue_ipsr]
 */
static irqreturn_t riscv_iommu_queue_ipsr(int irq, void *data)
{
	/* [한국어] 등록 시 넘긴 큐. */
	struct riscv_iommu_queue *queue = (struct riscv_iommu_queue *)data;

	/* [한국어] 인터럽트 상태에서 이 큐의 비트가 서 있으면 우리 것이다. */
	if (riscv_iommu_readl(queue->iommu, RISCV_IOMMU_REG_IPSR) & Q_IPSR(queue))
		return IRQ_WAKE_THREAD;

	return IRQ_NONE;	/* [한국어] 인터럽트를 공유하는 다른 큐의 것이다. */
}

/*
 * [한국어]
 * riscv_iommu_queue_vec - 큐 번호로 인터럽트 벡터 인덱스를 얻는다
 *
 * @iommu: 대상 IOMMU.
 * @n: 큐 번호(= ICVEC 안의 필드 순서).
 * @return: 이 큐가 쓰는 인터럽트 벡터의 인덱스.
 *
 * ICVEC 레지스터는 큐마다 4비트씩 벡터 번호를 담는다. 그래서
 * 큐 번호에 4를 곱한 만큼 밀고 마스크하면 그 큐의 벡터가 나온다.
 * 원본 주석이 밝히듯 모든 필드의 폭이 같아 CIV 마스크를 재사용한다.
 *
 * 실행 컨텍스트: 큐 활성화와 비활성화.
 *
 * 호출 체인:
 *   riscv_iommu_queue_enable() / disable() → [riscv_iommu_queue_vec]
 */
static int riscv_iommu_queue_vec(struct riscv_iommu_device *iommu, int n)
{
	/* Reuse ICVEC.CIV mask for all interrupt vectors mapping. */
	/* [한국어] 큐마다 4비트씩 벡터 번호가 들어 있다. */
	return (iommu->icvec >> (n * 4)) & RISCV_IOMMU_ICVEC_CIV;
}

/*
 * Enable queue processing in the hardware, register interrupt handler.
 *
 * @queue - data structure, already allocated with riscv_iommu_queue_alloc()
 * @irq_handler - threaded interrupt handler.
 */
/*
 * [한국어]
 * riscv_iommu_queue_enable - 큐를 켜고 인터럽트 처리기를 등록한다
 *
 * @iommu: 대상 IOMMU.
 * @queue: 이미 버퍼가 마련된 큐.
 * @irq_handler: 스레드 처리기.
 * @return: 0 성공, -EBUSY/-ENODEV.
 *
 * 순서에 이유가 있다.
 *
 *  1) 인터럽트를 **먼저** 등록한다. 큐를 켠 뒤에 등록하면 그 사이
 *     발생한 인터럽트를 놓친다.
 *  2) 큐를 비운다. 명령 큐는 tail(생산자)을, 폴트 큐는 head(소비자)를
 *     0으로 되돌리는데, 각 큐에서 **소프트웨어가 쓰는 쪽**이 다르기
 *     때문이다 — 명령 큐는 우리가 넣고, 폴트 큐는 우리가 뺀다.
 *  3) 활성화 비트와 인터럽트 활성화, 그리고 메모리 폴트 지우기를
 *     한 번에 쓴다.
 *  4) busy가 내려갈 때까지 기다린 뒤, 정말 활성 상태가 되었고
 *     오류가 없는지 확인한다.
 *
 * 실행 컨텍스트: 초기화. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_init() → [riscv_iommu_queue_enable]
 */
static int riscv_iommu_queue_enable(struct riscv_iommu_device *iommu,
				    struct riscv_iommu_queue *queue,
				    irq_handler_t irq_handler)
{
	/* [한국어] 이 큐가 쓸 인터럽트 번호. */
	const unsigned int irq = iommu->irqs[riscv_iommu_queue_vec(iommu, queue->qid)];
	/* [한국어] 큐 제어 레지스터를 되읽은 값. */
	u32 csr;
	/* [한국어] 등록 결과. */
	int rc;

	/* [한국어] iommu가 이미 설정되어 있으면 켜진 큐다. */
	if (queue->iommu)
		return -EBUSY;

	/* Polling not implemented */
	/* [한국어] 인터럽트 없이 폴링으로 처리하는 경로는 아직 없다. */
	if (!irq)
		return -ENODEV;

	/* [한국어] 인터럽트 처리기가 이 값으로 IOMMU를 되찾으므로
	 * 등록 전에 채워 둔다. */
	queue->iommu = iommu;
	/* [한국어] 1차 처리기가 자기 것인지 가리고, 스레드 처리기가
	 * 실제 작업을 한다. ONESHOT은 스레드가 끝날 때까지 인터럽트를
	 * 막아 재진입을 없앤다. */
	rc = request_threaded_irq(irq, riscv_iommu_queue_ipsr, irq_handler,
				  IRQF_ONESHOT | IRQF_SHARED,
				  dev_name(iommu->dev), queue);
	if (rc) {
		/* [한국어] 등록에 실패했으니 표시를 되돌린다. */
		queue->iommu = NULL;
		return rc;	/* [한국어] 인터럽트를 등록하지 못했다. */
	}

	/* Empty queue before enabling it */
	/* [한국어] 소프트웨어가 쓰는 쪽의 인덱스를 0으로 되돌려 큐를
	 * 비운다. 명령 큐는 우리가 넣으므로 tail, 폴트 큐는 우리가
	 * 빼므로 head가 그 대상이다. */
	if (queue->qid == RISCV_IOMMU_INTR_CQ)
		riscv_iommu_writel(queue->iommu, Q_TAIL(queue), 0);
	else
		riscv_iommu_writel(queue->iommu, Q_HEAD(queue), 0);

	/*
	 * Enable queue with interrupts, clear any memory fault if any.
	 * Wait for the hardware to acknowledge request and activate queue
	 * processing.
	 * Note: All CSR bitfields are in the same offsets for all queues.
	 */
	/* [한국어] 활성화와 인터럽트를 켜면서, 밀린 메모리 폴트 표시도
	 * 함께 지운다(write-1-to-clear). */
	riscv_iommu_writel(iommu, queue->qcr,
			   RISCV_IOMMU_QUEUE_ENABLE |
			   RISCV_IOMMU_QUEUE_INTR_ENABLE |
			   RISCV_IOMMU_QUEUE_MEM_FAULT);

	/* [한국어] 하드웨어가 요청을 처리할 때까지 기다린다. */
	riscv_iommu_readl_timeout(iommu, queue->qcr,
				  csr, !(csr & RISCV_IOMMU_QUEUE_BUSY),
				  10, RISCV_IOMMU_QCSR_TIMEOUT);

	/* [한국어] 정확히 "활성이고, busy가 아니며, 메모리 폴트도 없는"
	 * 상태여야 한다. 세 비트를 한 번에 비교해 그것을 확인한다. */
	if (RISCV_IOMMU_QUEUE_ACTIVE != (csr & (RISCV_IOMMU_QUEUE_ACTIVE |
						RISCV_IOMMU_QUEUE_BUSY |
						RISCV_IOMMU_QUEUE_MEM_FAULT))) {
		/* Best effort to stop and disable failing hardware queue. */
		/* [한국어] 반쯤 켜진 큐를 남기지 않도록 끄고 되돌린다. */
		riscv_iommu_writel(iommu, queue->qcr, 0);
		free_irq(irq, queue);	/* [한국어] 등록해 둔 핸들러를 뗀다. */
		queue->iommu = NULL;	/* [한국어] 꺼진 큐임을 표시한다. */
		dev_err(iommu->dev, "queue #%u failed to start\n", queue->qid);	/* [한국어] 어느 큐가 시작하지 못했는지 남긴다. */
		return -EBUSY;	/* [한국어] 반쯤 켜진 큐를 남기지 않고 실패를 전한다. */
	}

	/* Clear any pending interrupt flag. */
	/* [한국어] 켜는 과정에서 생긴 인터럽트 표시를 지운다.
	 * 그러지 않으면 처리할 것이 없는데도 즉시 인터럽트가 뜬다. */
	riscv_iommu_writel(iommu, RISCV_IOMMU_REG_IPSR, Q_IPSR(queue));

	return 0;	/* [한국어] 큐가 정상적으로 활성화됐다. */
}

/*
 * Disable queue. Wait for the hardware to acknowledge request and
 * stop processing enqueued requests. Report errors but continue.
 */
/*
 * [한국어]
 * riscv_iommu_queue_disable - 큐를 끄고 인터럽트 처리기를 뗀다
 *
 * @queue: 대상 큐.
 * @return: 없음.
 *
 * 인터럽트를 먼저 떼는 것이 중요하다. 큐를 끄는 도중에 인터럽트가
 * 들어오면 이미 정리 중인 상태를 만지게 된다.
 *
 * 원본 주석이 밝히듯 실패해도 계속 진행한다 — 끄는 도중에 멈추면
 * 더 나쁜 상태가 남기 때문이다.
 *
 * 실행 컨텍스트: 초기화 실패 경로와 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_init()의 오류 경로 / riscv_iommu_remove()
 *   → [riscv_iommu_queue_disable]
 */
static void riscv_iommu_queue_disable(struct riscv_iommu_queue *queue)
{
	/* [한국어] 이 큐가 속한 IOMMU. */
	struct riscv_iommu_device *iommu = queue->iommu;
	/* [한국어] 되읽은 제어 레지스터 값. */
	u32 csr;

	/* [한국어] 애초에 켜지지 않은 큐면 할 일이 없다. */
	if (!iommu)
		return;

	/* [한국어] 인터럽트를 먼저 뗀다 — 정리 중에 처리기가 들어오면
	 * 반쯤 정리된 상태를 만지게 된다. */
	free_irq(iommu->irqs[riscv_iommu_queue_vec(iommu, queue->qid)], queue);
	/* [한국어] 모든 제어 비트를 내려 큐를 끈다. */
	riscv_iommu_writel(iommu, queue->qcr, 0);
	/* [한국어] 하드웨어가 요청을 처리할 때까지 기다린다. */
	riscv_iommu_readl_timeout(iommu, queue->qcr,
				  csr, !(csr & RISCV_IOMMU_QUEUE_BUSY),
				  10, RISCV_IOMMU_QCSR_TIMEOUT);

	/* [한국어] 여전히 활성이거나 바쁘면 하드웨어가 요청을 무시한
	 * 것이다. 되돌릴 방법이 없으니 기록만 남긴다. */
	if (csr & (RISCV_IOMMU_QUEUE_ACTIVE | RISCV_IOMMU_QUEUE_BUSY))
		dev_err(iommu->dev, "fail to disable hardware queue #%u, csr 0x%x\n",
			queue->qid, csr);

	/* [한국어] 이 표시가 곧 "꺼진 큐"의 정의다. */
	queue->iommu = NULL;
}

/*
 * Returns number of available valid queue entries and the first item index.
 * Update shadow producer index if necessary.
 */
/*
 * [한국어]
 * riscv_iommu_queue_consume - 처리할 항목이 몇 개인지 알아낸다
 *
 * @queue: 대상 큐(폴트 큐).
 * @index: 첫 항목의 인덱스를 돌려줄 곳.
 * @return: 처리 가능한 항목 수(0이면 없음).
 *
 * 그림자 인덱스가 이 코드의 핵심 개념이다. head와 tail은 링 크기로
 * 접히지 않은 **단조 증가 카운터**라, 둘의 차이가 곧 항목 수다.
 * 그래서 랩어라운드를 특별히 다루지 않아도 된다.
 *
 * 소프트웨어가 아는 tail(그림자)로 계산해 보고 항목이 없으면,
 * 그때 비로소 하드웨어의 tail 레지스터를 읽어 그림자를 갱신한다.
 * 레지스터 읽기는 비싸므로 꼭 필요할 때만 한다는 설계다.
 *
 * 하드웨어 값은 접힌 인덱스라, 직전 접힌 값과의 차이를 구해
 * 그림자에 더하는 방식으로 확장한다.
 *
 * 실행 컨텍스트: 폴트 큐 스레드 처리기.
 *
 * 호출 체인:
 *   riscv_iommu_fltq_process() → [riscv_iommu_queue_consume]
 */
static int riscv_iommu_queue_consume(struct riscv_iommu_queue *queue,
				     unsigned int *index)
{
	/* [한국어] 소프트웨어가 아는 소비자 위치(접히지 않은 카운터). */
	unsigned int head = atomic_read(&queue->head);
	/* [한국어] 소프트웨어가 아는 생산자 위치(그림자). */
	unsigned int tail = atomic_read(&queue->tail);
	/* [한국어] 그 그림자를 링 안의 자리로 접은 값. 하드웨어가
	 * 돌려주는 값과 견주기 위해 필요하다. */
	unsigned int last = Q_ITEM(queue, tail);
	/* [한국어] 두 카운터의 차이가 곧 항목 수다. 단조 증가라
	 * 랩어라운드를 따로 처리할 필요가 없다. */
	int available = (int)(tail - head);

	/* [한국어] 호출자가 훑기 시작할 위치를 알려 준다. */
	*index = head;

	/* [한국어] 이미 아는 것만으로 처리할 항목이 있으면 레지스터를
	 * 읽지 않는다 — MMIO 읽기는 비싸다. */
	if (available > 0)
		return available;

	/* read hardware producer index, check reserved register bits are not set. */
	/* [한국어] 하드웨어의 생산자 인덱스를 읽는다. 마스크 밖의
	 * 비트가 서 있으면 잘못된 값이므로 조건에 그것을 넣어 둔다. */
	if (riscv_iommu_readl_timeout(queue->iommu, Q_TAIL(queue),
				      tail, (tail & ~queue->mask) == 0,
				      0, RISCV_IOMMU_QUEUE_TIMEOUT)) {
		dev_err_once(queue->iommu->dev,	/* [한국어] 하드웨어가 응답하지 않는다 — 한 번만 알린다. */
			     "Hardware error: queue access timeout\n");
		return 0;	/* [한국어] 처리할 항목이 없다고 답한다. */
	}

	/* [한국어] 하드웨어의 위치가 그림자와 같으면 새 항목이 없다. */
	if (tail == last)
		return 0;

	/* update shadow producer index */
	/* [한국어] 접힌 값끼리의 차이를 구해 그림자 카운터를 그만큼
	 * 전진시킨다. 이렇게 접힌 인덱스를 단조 카운터로 확장한다. */
	return (int)(atomic_add_return((tail - last) & queue->mask, &queue->tail) - head);
}

/*
 * Release processed queue entries, should match riscv_iommu_queue_consume() calls.
 */
/*
 * [한국어]
 * riscv_iommu_queue_release - 처리한 항목을 하드웨어에 반납한다
 *
 * @queue: 대상 큐.
 * @count: 처리한 항목 수.
 * @return: 없음.
 *
 * 소비자 카운터를 전진시키고, 접힌 값을 head 레지스터에 써
 * 하드웨어에게 "여기까지 읽었다"고 알린다. 그래야 하드웨어가
 * 그 자리를 다시 쓸 수 있다.
 *
 * 실행 컨텍스트: 폴트 큐 스레드 처리기.
 *
 * 호출 체인:
 *   riscv_iommu_fltq_process() → [riscv_iommu_queue_release]
 */
static void riscv_iommu_queue_release(struct riscv_iommu_queue *queue, int count)
{
	/* [한국어] 소비자 카운터를 처리한 만큼 전진시킨다. */
	const unsigned int head = atomic_add_return(count, &queue->head);

	/* [한국어] 접힌 값을 도어벨에 써 하드웨어가 자리를 재사용하게 한다. */
	riscv_iommu_writel(queue->iommu, Q_HEAD(queue), Q_ITEM(queue, head));
}

/* Return actual consumer index based on hardware reported queue head index. */
/*
 * [한국어]
 * riscv_iommu_queue_cons - 하드웨어가 어디까지 처리했는지 알아낸다
 *
 * @queue: 대상 큐(명령 큐).
 * @return: 접히지 않은 소비자 카운터.
 *
 * 명령 큐에서는 **하드웨어가 소비자**다. 그래서 head 레지스터를
 * 읽어 "어디까지 실행했는가"를 알아내야 한다. consume()과 대칭
 * 관계인 셈이다.
 *
 * 접힌 값을 단조 카운터로 확장하는 방식도 같다: 소프트웨어가 아는
 * 위치를 접어 두고, 하드웨어 값과의 차이를 더한다.
 *
 * 읽기에 실패하면 아는 값을 그대로 돌려준다 — 진전이 없다고
 * 보는 것이 안전하다.
 *
 * 실행 컨텍스트: 명령 완료 대기(폴링). 잠들 수 있다.
 *
 * 호출 체인:
 *   riscv_iommu_queue_wait()의 readx_poll_timeout
 *   → [riscv_iommu_queue_cons]
 */
static unsigned int riscv_iommu_queue_cons(struct riscv_iommu_queue *queue)
{
	/* [한국어] 소프트웨어가 아는 소비자 위치. */
	const unsigned int cons = atomic_read(&queue->head);
	/* [한국어] 그것을 링 안의 자리로 접은 값. */
	const unsigned int last = Q_ITEM(queue, cons);
	/* [한국어] 하드웨어가 보고한 소비자 위치. */
	unsigned int head;

	/* [한국어] 읽기에 실패하면 진전이 없다고 보고 아는 값을 돌려준다. */
	if (riscv_iommu_readl_timeout(queue->iommu, Q_HEAD(queue), head,
				      !(head & ~queue->mask),
				      0, RISCV_IOMMU_QUEUE_TIMEOUT))
		return cons;

	/* [한국어] 접힌 값끼리의 차이를 더해 단조 카운터로 확장한다. */
	return cons + ((head - last) & queue->mask);
}

/* Wait for submitted item to be processed. */
/*
 * [한국어]
 * riscv_iommu_queue_wait - 특정 명령이 실행될 때까지 기다린다
 *
 * @queue: 명령 큐.
 * @index: 기다릴 명령의 생산자 인덱스.
 * @timeout_us: 최대 대기 시간.
 * @return: 0 성공, -ETIMEDOUT.
 *
 * 소비자 위치가 그 명령을 지나갔는지로 완료를 판정한다.
 * 부호 있는 뺄셈을 쓰는 것이 요점인데, 카운터가 랩어라운드해도
 * "지나갔는가"의 비교가 올바르게 동작하기 때문이다.
 *
 * 폴링 조건에 오류 비트 확인이 함께 들어 있다. 명령이 잘못되었거나
 * 타임아웃되면 소비자가 영영 전진하지 않을 수 있어, 그때는
 * 기다림을 끊어야 한다.
 *
 * 실행 컨텍스트: 동기화가 필요한 무효화 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   riscv_iommu_cmd_sync() → [riscv_iommu_queue_wait]
 */
static int riscv_iommu_queue_wait(struct riscv_iommu_queue *queue,
				  unsigned int index,
				  unsigned int timeout_us)
{
	/* [한국어] 현재 아는 소비자 위치. */
	unsigned int cons = atomic_read(&queue->head);
	/* [한국어] 기다림을 끊어야 하는 오류들 — 메모리 폴트,
	 * 명령 타임아웃, 잘못된 명령. */
	unsigned int flags = RISCV_IOMMU_CQCSR_CQMF | RISCV_IOMMU_CQCSR_CMD_TO |
			     RISCV_IOMMU_CQCSR_CMD_ILL;

	/* Already processed by the consumer */
	/* [한국어] 부호 있는 비교라 카운터가 랩어라운드해도 올바르다. */
	if ((int)(cons - index) > 0)
		return 0;

	/* Monitor consumer index */
	/* [한국어] 소비자가 지나갈 때까지, 또는 큐가 오류 상태가 될
	 * 때까지 기다린다. 오류 조건이 없으면 영영 기다릴 수 있다. */
	return readx_poll_timeout(riscv_iommu_queue_cons, queue, cons,
				 (riscv_iommu_readl(queue->iommu, queue->qcr) & flags) ||
				 (int)(cons - index) > 0, 0, timeout_us);
}

/* Enqueue an entry and wait to be processed if timeout_us > 0
 *
 * Error handling for IOMMU hardware not responding in reasonable time
 * will be added as separate patch series along with other RAS features.
 * For now, only report hardware failure and continue.
 */
/*
 * [한국어]
 * riscv_iommu_queue_send - 명령 하나를 링 버퍼에 넣고 도어벨을 울린다
 *
 * @queue: 대상 큐.
 * @entry: 넣을 항목.
 * @entry_size: 항목 크기.
 * @return: 이 항목의 생산자 인덱스(완료 대기에 쓴다).
 *
 * 이 파일에서 가장 섬세한 함수다. **락 없이 여러 CPU가 동시에
 * 명령을 넣을 수 있어야 한다.** 그 방법이 원본 주석의 일곱 단계다.
 *
 *  1) atomic_inc_return으로 자리를 하나 예약한다. 이 원자 연산이
 *     CPU들에게 서로 다른 자리를 배분하는 유일한 동기화다.
 *  2) 링이 가득 찼으면 소비자가 나아가기를 기다린다.
 *  3) 예약한 자리에 항목을 복사한다. 자리가 서로 다르므로
 *     이 복사는 서로 간섭하지 않는다.
 *  4) **자기 차례를 기다린다.** tail이 자기 인덱스와 같아질 때까지
 *     기다리는 것인데, 이것이 도어벨을 순서대로 울리게 하는 장치다.
 *     예약은 순서 없이 되지만 통보는 순서대로 되어야 한다.
 *  5) 쓰기 장벽 후 도어벨을 울린다.
 *  6) MMIO 쓰기가 끝난 뒤에 그림자 tail을 올려, 다음 차례의 CPU가
 *     진행하게 한다.
 *  7) 인터럽트를 되살린다.
 *
 * local_irq_save로 감싸는 이유: 4단계에서 자기 차례를 기다리는데,
 * 그 사이 선점되면 뒤 순번의 CPU들이 모두 멈춰 버린다.
 *
 * 실행 컨텍스트: 무효화 경로 전반. 인터럽트를 끄고 돈다.
 *
 * 호출 체인:
 *   riscv_iommu_cmd_send() / cmd_sync() → [riscv_iommu_queue_send]
 */
static unsigned int riscv_iommu_queue_send(struct riscv_iommu_queue *queue,
					   void *entry, size_t entry_size)
{
	/* [한국어] 이 항목에 배정된 생산자 인덱스. */
	unsigned int prod;
	/* [한국어] 소비자 위치(공간 확인용). */
	unsigned int head;
	/* [한국어] 그림자 생산자 위치(차례 확인용). */
	unsigned int tail;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;

	/* Do not preempt submission flow. */
	/* [한국어] 4단계에서 자기 차례를 기다리는데, 그 사이 선점되면
	 * 뒤 순번의 CPU들이 전부 멈춘다. */
	local_irq_save(flags);

	/* 1. Allocate some space in the queue */
	/* [한국어] 원자적 증가로 자리를 예약한다. 이것이 여러 CPU에게
	 * 서로 다른 자리를 배분하는 유일한 동기화 지점이다. */
	prod = atomic_inc_return(&queue->prod) - 1;
	head = atomic_read(&queue->head);	/* [한국어] 공간 확인을 위해 소비자 위치를 읽는다. */

	/* 2. Wait for space availability. */
	/* [한국어] 예약한 자리가 링을 한 바퀴 넘어섰다 — 소비자가
	 * 충분히 나아가기를 기다려야 한다. */
	if ((prod - head) > queue->mask) {
		if (readx_poll_timeout(atomic_read, &queue->head,	/* [한국어] 소비자가 충분히 나아갈 때까지 기다린다. */
				       head, (prod - head) < queue->mask,
				       0, RISCV_IOMMU_QUEUE_TIMEOUT))
			goto err_busy;
	/* [한국어] 정확히 한 바퀴 차이라면 링이 꽉 찬 경계다.
	 * 이때는 하드웨어의 head를 직접 읽어 그림자를 갱신한다 —
	 * 다른 CPU가 갱신해 주기를 기다릴 수 없는 상황이다. */
	} else if ((prod - head) == queue->mask) {
		/* [한국어] 아는 소비자 위치를 접은 값. */
		const unsigned int last = Q_ITEM(queue, head);

		/* [한국어] 하드웨어가 한 칸이라도 나아갔는지 본다. */
		if (riscv_iommu_readl_timeout(queue->iommu, Q_HEAD(queue), head,
					      !(head & ~queue->mask) && head != last,
					      0, RISCV_IOMMU_QUEUE_TIMEOUT))
			goto err_busy;
		/* [한국어] 나아간 만큼 그림자 소비자 카운터를 올린다. */
		atomic_add((head - last) & queue->mask, &queue->head);
	}

	/* 3. Store entry in the ring buffer */
	/* [한국어] 예약한 자리에 항목을 복사한다. CPU마다 자리가
	 * 달라 서로 간섭하지 않는다. */
	memcpy(queue->base + Q_ITEM(queue, prod) * entry_size, entry, entry_size);

	/* 4. Wait for all previous entries to be ready */
	/* [한국어] 자기 앞 순번들이 모두 도어벨을 울릴 때까지 기다린다.
	 * 예약은 순서 없이 되지만 통보는 순서대로 되어야 하기 때문이다. */
	if (readx_poll_timeout(atomic_read, &queue->tail, tail, prod == tail,
			       0, RISCV_IOMMU_QUEUE_TIMEOUT))
		goto err_busy;

	/*
	 * 5. Make sure the ring buffer update (whether in normal or I/O memory) is
	 *    completed and visible before signaling the tail doorbell to fetch
	 *    the next command. 'fence ow, ow'
	 */
	/* [한국어] 링 버퍼에 쓴 내용이 하드웨어에 보인 뒤에야 도어벨을
	 * 울려야 한다. 순서가 뒤집히면 하드웨어가 쓰레기를 실행한다. */
	dma_wmb();
	/* [한국어] tail을 다음 자리로 올려 "여기까지 채웠다"고 알린다. */
	riscv_iommu_writel(queue->iommu, Q_TAIL(queue), Q_ITEM(queue, prod + 1));

	/*
	 * 6. Make sure the doorbell write to the device has finished before updating
	 *    the shadow tail index in normal memory. 'fence o, w'
	 */
	/* [한국어] MMIO 쓰기가 실제로 나간 뒤에 그림자를 올려야,
	 * 다음 차례의 CPU가 자기 도어벨을 올바른 순서로 울린다. */
#ifdef CONFIG_MMIOWB
	mmiowb();
#endif
	/* [한국어] 그림자 tail을 올려 다음 순번을 풀어 준다. */
	atomic_inc(&queue->tail);

	/* 7. Complete submission and restore local interrupts */
	local_irq_restore(flags);	/* [한국어] 제출이 끝났으니 인터럽트를 되살린다. */

	return prod;

/* [한국어] 하드웨어가 응답하지 않아 진행할 수 없는 경우. */
err_busy:
	local_irq_restore(flags);
	/* [한국어] 원본 주석이 밝히듯 본격적인 오류 복구는 아직 없다 —
	 * 기록만 남기고 인덱스를 그대로 돌려준다. */
	dev_err_once(queue->iommu->dev, "Hardware error: command enqueue failed\n");

	return prod;	/* [한국어] 이 항목의 인덱스를 돌려준다 — 완료 대기의 기준이다. */
}

/*
 * IOMMU Command queue chapter 3.1
 */

/* Command queue interrupt handler thread function */
/*
 * [한국어]
 * riscv_iommu_cmdq_process - 명령 큐 인터럽트의 스레드 처리기
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @data: 등록 시 넘긴 큐.
 * @return: 항상 IRQ_HANDLED.
 *
 * 명령 큐 인터럽트는 정상 완료가 아니라 **오류**를 알린다.
 * 네 가지 오류를 확인하고, 읽은 값을 되써 지운다(write-1-to-clear).
 *
 * 오류가 나도 복구하지 않고 기록만 하는 점에 유의 — 원본 주석이
 * 밝히듯 복구는 아직 구현되지 않았다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [riscv_iommu_cmdq_process]
 */
static irqreturn_t riscv_iommu_cmdq_process(int irq, void *data)
{
	/* [한국어] 등록 시 넘긴 큐. */
	const struct riscv_iommu_queue *queue = (struct riscv_iommu_queue *)data;
	/* [한국어] 제어/상태 레지스터 값. */
	unsigned int ctrl;

	/* Clear MF/CQ errors, complete error recovery to be implemented. */
	/* [한국어] 네 가지 오류 중 하나라도 서 있는지 본다. */
	ctrl = riscv_iommu_readl(queue->iommu, queue->qcr);
	if (ctrl & (RISCV_IOMMU_CQCSR_CQMF | RISCV_IOMMU_CQCSR_CMD_TO |	/* [한국어] 네 가지 오류 중 하나라도 서 있는가. */
		    RISCV_IOMMU_CQCSR_CMD_ILL | RISCV_IOMMU_CQCSR_FENCE_W_IP)) {
		/* [한국어] 읽은 값을 되쓰면 선 비트들이 지워진다. */
		riscv_iommu_writel(queue->iommu, queue->qcr, ctrl);
		/* [한국어] 어떤 오류였는지 각각 0/1로 풀어 남긴다 —
		 * 원시 값만으로는 읽기 어렵기 때문이다. */
		dev_warn(queue->iommu->dev,
			 "Queue #%u error; fault:%d timeout:%d illegal:%d fence_w_ip:%d\n",
			 queue->qid,
			 !!(ctrl & RISCV_IOMMU_CQCSR_CQMF),
			 !!(ctrl & RISCV_IOMMU_CQCSR_CMD_TO),
			 !!(ctrl & RISCV_IOMMU_CQCSR_CMD_ILL),
			 !!(ctrl & RISCV_IOMMU_CQCSR_FENCE_W_IP));
	}

	/* Placeholder for command queue interrupt notifiers */

	/* Clear command interrupt pending. */
	/* [한국어] 인터럽트 표시를 지워 다시 무장시킨다. */
	riscv_iommu_writel(queue->iommu, RISCV_IOMMU_REG_IPSR, Q_IPSR(queue));

	return IRQ_HANDLED;	/* [한국어] 큐 오류를 처리했음을 알린다. */
}

/* Send command to the IOMMU command queue */
/*
 * [한국어]
 * riscv_iommu_cmd_send - 명령 하나를 큐에 넣는다(완료를 기다리지 않음)
 *
 * @iommu: 대상 IOMMU.
 * @cmd: 보낼 명령.
 * @return: 없음.
 *
 * 무효화 명령들을 연달아 넣은 뒤 마지막에 한 번만 동기화하는
 * 것이 이 드라이버의 패턴이다.
 *
 * 실행 컨텍스트: 무효화 경로.
 *
 * 호출 체인:
 *   무효화 함수들 → [riscv_iommu_cmd_send] → riscv_iommu_queue_send()
 */
static void riscv_iommu_cmd_send(struct riscv_iommu_device *iommu,
				 struct riscv_iommu_command *cmd)
{
	/* [한국어] 반환된 인덱스를 쓰지 않는다 — 기다리지 않기 때문이다. */
	riscv_iommu_queue_send(&iommu->cmdq, cmd, sizeof(*cmd));
}

/* Send IOFENCE.C command and wait for all scheduled commands to complete. */
/*
 * [한국어]
 * riscv_iommu_cmd_sync - 앞서 보낸 모든 명령이 끝날 때까지 기다린다
 *
 * @iommu: 대상 IOMMU.
 * @timeout_us: 최대 대기 시간. 0이면 기다리지 않는다.
 * @return: 없음.
 *
 * IOFENCE.C는 "이 명령 앞의 모든 명령이 완료되었다"를 보장하는
 * 장벽이다. 그것 하나를 큐에 넣고 그 명령이 소비되기를 기다리면,
 * 앞선 무효화가 모두 끝났음이 보장된다.
 *
 * timeout_us가 0이면 넣기만 하고 돌아간다 — 순서만 보장하면
 * 충분한 경우를 위한 것이다.
 *
 * 실행 컨텍스트: 무효화 경로의 마지막. 잠들 수 있다.
 *
 * 호출 체인:
 *   iotlb_inval() / iodir_update() / bond_unlink()
 *   → [riscv_iommu_cmd_sync] → riscv_iommu_queue_wait()
 */
static void riscv_iommu_cmd_sync(struct riscv_iommu_device *iommu,
				 unsigned int timeout_us)
{
	/* [한국어] 조립할 IOFENCE 명령. */
	struct riscv_iommu_command cmd;
	/* [한국어] 이 명령의 생산자 인덱스 — 완료 판정의 기준이다. */
	unsigned int prod;

	/* [한국어] 앞선 모든 명령의 완료를 보장하는 장벽 명령을 만든다. */
	riscv_iommu_cmd_iofence(&cmd);
	prod = riscv_iommu_queue_send(&iommu->cmdq, &cmd, sizeof(cmd));

	/* [한국어] 기다릴 필요가 없다면 넣은 것만으로 충분하다. */
	if (!timeout_us)
		return;

	/* [한국어] 이 장벽이 소비되면 앞선 무효화가 모두 끝난 것이다. */
	if (riscv_iommu_queue_wait(&iommu->cmdq, prod, timeout_us))
		dev_err_once(iommu->dev,
			     "Hardware error: command execution timeout\n");
}

/*
 * IOMMU Fault/Event queue chapter 3.2
 */

/*
 * [한국어]
 * riscv_iommu_fault - 폴트 기록 하나를 처리한다
 *
 * @iommu: 폴트를 보고한 IOMMU.
 * @event: 폴트 큐에서 꺼낸 기록.
 * @return: 없음.
 *
 * 아직 로그만 남긴다. 원본 주석이 밝히듯 본격적인 폴트 처리
 * (상위 핸들러 호출, 페이지 요청 응답)는 앞으로 추가될 부분이다.
 *
 * iotval과 iotval2는 폴트 원인에 따라 의미가 달라지는 값이라
 * 해석하지 않고 그대로 찍는다.
 *
 * 실행 컨텍스트: 폴트 큐 스레드 처리기.
 *
 * 호출 체인:
 *   riscv_iommu_fltq_process() → [riscv_iommu_fault]
 */
static void riscv_iommu_fault(struct riscv_iommu_device *iommu,
			      struct riscv_iommu_fq_record *event)
{
	/* [한국어] 폴트의 원인 코드. */
	unsigned int err = FIELD_GET(RISCV_IOMMU_FQ_HDR_CAUSE, event->hdr);
	/* [한국어] 폴트를 낸 디바이스의 ID. */
	unsigned int devid = FIELD_GET(RISCV_IOMMU_FQ_HDR_DID, event->hdr);

	/* Placeholder for future fault handling implementation, report only. */
	/* [한국어] 원인 코드가 0이면 오류가 아닌 기록이다.
	 * 잘못된 디바이스가 폴트를 쏟아낼 수 있어 로그를 제한한다. */
	if (err)
		dev_warn_ratelimited(iommu->dev,
				     "Fault %d devid: 0x%x iotval: %llx iotval2: %llx\n",
				     err, devid, event->iotval, event->iotval2);
}

/* Fault queue interrupt handler thread function */
/*
 * [한국어]
 * riscv_iommu_fltq_process - 폴트 큐 인터럽트의 스레드 처리기
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @data: 등록 시 넘긴 큐.
 * @return: 항상 IRQ_HANDLED.
 *
 * 인터럽트 표시를 **먼저** 지우고 큐를 비우는 순서가 중요하다.
 * 반대로 하면 비우는 동안 도착한 폴트의 인터럽트를 함께 지워
 * 놓치게 된다.
 *
 * 바깥 루프가 다시 도는 이유도 같다: 처리하는 동안 새 항목이
 * 들어왔을 수 있어, 소비할 것이 없어질 때까지 반복한다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [riscv_iommu_fltq_process]
 *   → riscv_iommu_queue_consume() → riscv_iommu_fault()
 */
static irqreturn_t riscv_iommu_fltq_process(int irq, void *data)
{
	/* [한국어] 등록 시 넘긴 큐. */
	struct riscv_iommu_queue *queue = (struct riscv_iommu_queue *)data;
	/* [한국어] 그 큐가 속한 IOMMU. */
	struct riscv_iommu_device *iommu = queue->iommu;
	/* [한국어] 링 버퍼를 폴트 기록의 배열로 본 것. */
	struct riscv_iommu_fq_record *events;
	/* [한국어] 제어 레지스터 값과 훑을 시작 인덱스. */
	unsigned int ctrl, idx;
	/* [한국어] 이번에 소비할 개수와 훑은 개수. */
	int cnt, len;

	/* [한국어] 링 버퍼의 타입을 확정한다. */
	events = (struct riscv_iommu_fq_record *)queue->base;

	/* Clear fault interrupt pending and process all received fault events. */
	/* [한국어] 비우기 **전에** 인터럽트를 지운다. 순서가 반대면
	 * 처리 중 도착한 폴트의 인터럽트까지 함께 지워 놓친다. */
	riscv_iommu_writel(iommu, RISCV_IOMMU_REG_IPSR, Q_IPSR(queue));

	do {
		/* [한국어] 처리할 항목 수와 시작 위치를 얻는다. */
		cnt = riscv_iommu_queue_consume(queue, &idx);
		/* [한국어] 각 기록을 처리한다. 인덱스는 단조 카운터이므로
		 * 배열 접근 시 Q_ITEM으로 접어야 한다. */
		for (len = 0; len < cnt; idx++, len++)
			riscv_iommu_fault(iommu, &events[Q_ITEM(queue, idx)]);
		/* [한국어] 처리한 만큼 하드웨어에 반납한다. */
		riscv_iommu_queue_release(queue, cnt);
	/* [한국어] 처리하는 동안 새 항목이 들어왔을 수 있어 반복한다. */
	} while (cnt > 0);

	/* Clear MF/OF errors, complete error recovery to be implemented. */
	/* [한국어] 큐 자체의 오류 — 메모리 폴트와 오버플로 — 를 확인한다.
	 * 오버플로는 폴트가 너무 빨리 쌓여 기록을 잃었다는 뜻이다. */
	ctrl = riscv_iommu_readl(iommu, queue->qcr);
	if (ctrl & (RISCV_IOMMU_FQCSR_FQMF | RISCV_IOMMU_FQCSR_FQOF)) {
		/* [한국어] 읽은 값을 되써 지운다. */
		riscv_iommu_writel(iommu, queue->qcr, ctrl);
		dev_warn(iommu->dev,	/* [한국어] 큐 자체의 오류를 사람이 읽을 형태로 남긴다. */
			 "Queue #%u error; memory fault:%d overflow:%d\n",
			 queue->qid,
			 !!(ctrl & RISCV_IOMMU_FQCSR_FQMF),
			 !!(ctrl & RISCV_IOMMU_FQCSR_FQOF));
	}

	return IRQ_HANDLED;	/* [한국어] 폴트 처리가 끝났음을 알린다. */
}

/* Lookup and initialize device context info structure. */
/*
 * [한국어]
 * riscv_iommu_get_dc - 디바이스 ID로 디바이스 컨텍스트를 찾는다(없으면 만든다)
 *
 * @iommu: 대상 IOMMU.
 * @devid: 찾을 디바이스 ID.
 * @return: DC의 주소, 실패하면 NULL.
 *
 * 디바이스 디렉터리 테이블(DDT)의 워크다. 세 가지가 얽혀 있다.
 *
 * (1) **ID를 쪼개는 방식이 형식에 따라 다르다.** MSI 평탄화를
 *     지원하는 확장 형식은 DC가 두 배로 커져(8 × 64비트),
 *     한 페이지에 절반만 들어간다. 그래서 1단계가 소비하는
 *     비트 수가 7에서 6으로 줄어든다.
 *
 * (2) **단계 수가 가변이다.** ddt_mode가 1~3단계를 정하고,
 *     그에 따라 ID의 유효 범위도 달라진다.
 *
 * (3) **중간 노드를 락 없이 만든다.** cmpxchg로 설치를 시도하고,
 *     졌으면 자기 페이지를 버리고 상대의 것을 쓴다. 다른
 *     드라이버들에서도 반복되는 표준 패턴이다.
 *
 * 마지막 줄의 인덱싱이 까다롭다. 잎 노드에서는 DC가 4개 또는 8개의
 * 64비트 워드이므로, 한 페이지에 들어가는 DC 수(64 또는 128)로
 * 마스킹한 뒤 DC 크기의 로그(3 또는 2)만큼 밀어 자리를 잡는다.
 *
 * 실행 컨텍스트: 디바이스 probe와 attach. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_probe_device() / iodir_update() → [riscv_iommu_get_dc]
 */
static struct riscv_iommu_dc *riscv_iommu_get_dc(struct riscv_iommu_device *iommu,
						 unsigned int devid)
{
	/* [한국어] MSI 평탄화를 지원하지 않으면 기본 형식이다.
	 * 확장 형식에서는 DC가 두 배로 커진다. */
	const bool base_format = !(iommu->caps & RISCV_IOMMU_CAPABILITIES_MSI_FLAT);
	/* [한국어] 남은 워크 단계 수. */
	unsigned int depth;
	/* [한국어] 읽은 엔트리, cmpxchg의 옛 값, 조립한 새 값. */
	unsigned long ddt, old, new;
	/* [한국어] 새로 만든 중간 노드. */
	void *ptr;
	/* [한국어] 각 단계까지의 누적 비트 수. */
	u8 ddi_bits[3] = { 0 };
	/* [한국어] 현재 훑고 있는 테이블의 위치. */
	u64 *ddtp = NULL;

	/* Make sure the mode is valid */
	/* [한국어] 디렉터리가 활성 모드가 아니면 워크할 대상이 없다. */
	if (iommu->ddt_mode < RISCV_IOMMU_DDTP_IOMMU_MODE_1LVL ||
	    iommu->ddt_mode > RISCV_IOMMU_DDTP_IOMMU_MODE_3LVL)
		return NULL;

	/*
	 * Device id partitioning for base format:
	 * DDI[0]: bits 0 - 6   (1st level) (7 bits)
	 * DDI[1]: bits 7 - 15  (2nd level) (9 bits)
	 * DDI[2]: bits 16 - 23 (3rd level) (8 bits)
	 *
	 * For extended format:
	 * DDI[0]: bits 0 - 5   (1st level) (6 bits)
	 * DDI[1]: bits 6 - 14  (2nd level) (9 bits)
	 * DDI[2]: bits 15 - 23 (3rd level) (9 bits)
	 */
	/* [한국어] 잎 노드에 DC가 몇 개 들어가느냐가 1단계 비트 수를
	 * 정한다. 기본 형식은 DC가 4워드라 페이지당 128개(7비트),
	 * 확장 형식은 8워드라 64개(6비트)다. */
	if (base_format) {
		ddi_bits[0] = 7;	/* [한국어] 기본 형식은 잎 노드에 DC가 128개 들어간다(7비트). */
		ddi_bits[1] = 7 + 9;	/* [한국어] 2단계는 512개 엔트리(9비트)를 더 덮는다. */
		ddi_bits[2] = 7 + 9 + 8;	/* [한국어] 3단계까지 합쳐 24비트의 디바이스 ID를 덮는다. */
	} else {
		ddi_bits[0] = 6;	/* [한국어] 확장 형식은 DC가 두 배라 잎에 64개만 들어간다(6비트). */
		ddi_bits[1] = 6 + 9;	/* [한국어] 2단계는 마찬가지로 9비트. */
		ddi_bits[2] = 6 + 9 + 9;	/* [한국어] 3단계가 9비트라 총합은 역시 24비트가 된다. */
	}

	/* Make sure device id is within range */
	/* [한국어] 단계 수가 곧 ID가 쓸 수 있는 비트 수를 정한다. */
	depth = iommu->ddt_mode - RISCV_IOMMU_DDTP_IOMMU_MODE_1LVL;
	if (devid >= (1 << ddi_bits[depth]))	/* [한국어] 이 단계 수로 표현할 수 없는 ID다. */
		return NULL;

	/* Get to the level of the non-leaf node that holds the device context */
	/* [한국어] 뿌리에서 시작해 잎 바로 위까지 내려간다.
	 * depth가 0이 되면 루프가 끝나고 ddtp가 잎 노드를 가리킨다. */
	for (ddtp = iommu->ddt_root; depth-- > 0;) {
		/* [한국어] 이 단계에서 ID를 몇 비트 밀어야 인덱스가 되는가. */
		const int split = ddi_bits[depth];
		/*
		 * Each non-leaf node is 64bits wide and on each level
		 * nodes are indexed by DDI[depth].
		 */
		/* [한국어] 중간 노드는 512개 엔트리(9비트)로 고정이다. */
		ddtp += (devid >> split) & 0x1FF;

		/*
		 * Check if this node has been populated and if not
		 * allocate a new level and populate it.
		 */
		do {
			/* [한국어] 다른 CPU가 동시에 설치할 수 있어 한 번만 읽는다. */
			ddt = READ_ONCE(*(unsigned long *)ddtp);
			/* [한국어] 이미 하위 노드가 있으면 그리로 내려간다. */
			if (ddt & RISCV_IOMMU_DDTE_V) {
				ddtp = __va(ppn_to_phys(ddt));	/* [한국어] 이미 있는 하위 노드로 내려간다. */
				break;
			}

			/* [한국어] 없으니 새 노드를 만든다(0으로 초기화된 4KB). */
			ptr = riscv_iommu_get_pages(iommu, SZ_4K);
			if (!ptr)	/* [한국어] 중간 노드를 만들 메모리가 없다. */
				return NULL;

			/* [한국어] 주소를 PPN 형식으로 바꾸고 유효 비트를 얹는다. */
			new = phys_to_ppn(__pa(ptr)) | RISCV_IOMMU_DDTE_V;
			/* [한국어] 읽었던 값 그대로일 때만 설치한다. relaxed인
			 * 이유는 아래에서 어차피 그 주소를 따라가며 의존
			 * 관계가 순서를 보장하기 때문이다. */
			old = cmpxchg_relaxed((unsigned long *)ddtp, ddt, new);

			/* [한국어] 설치에 성공했다 — 내 노드로 내려간다. */
			if (old == ddt) {
				ddtp = (u64 *)ptr;	/* [한국어] 내가 설치한 노드로 내려간다. */
				break;
			}

			/* Race setting DDT detected, re-read and retry. */
			/* [한국어] 경쟁에서 졌다. 내 페이지를 즉시 반납하고
			 * 다시 읽어 상대가 설치한 노드를 쓴다. */
			riscv_iommu_free_pages(iommu, ptr);
		} while (1);
	}

	/*
	 * Grab the node that matches DDI[depth], note that when using base
	 * format the device context is 4 * 64bits, and the extended format
	 * is 8 * 64bits, hence the (3 - base_format) below.
	 */
	/* [한국어] 잎 노드에서 DC의 자리를 잡는다.
	 * (64 << base_format)이 페이지당 DC 개수(확장 64, 기본 128)이고,
	 * (3 - base_format)이 DC 크기의 로그(확장 8워드=3, 기본 4워드=2)다.
	 * 두 값이 형식에 따라 반대 방향으로 움직이는 것이 요점이다. */
	ddtp += (devid & ((64 << base_format) - 1)) << (3 - base_format);

	return (struct riscv_iommu_dc *)ddtp;	/* [한국어] 잎 노드 안의 DC 주소를 돌려준다. */
}

/*
 * This is best effort IOMMU translation shutdown flow.
 * Disable IOMMU without waiting for hardware response.
 */
/*
 * [한국어]
 * riscv_iommu_disable - IOMMU를 즉시 끈다(응답을 기다리지 않음)
 *
 * @iommu: 대상 IOMMU.
 * @return: 없음.
 *
 * 앞 커널이 켜 둔 IOMMU를 크래시 덤프 커널이 넘겨받았을 때처럼,
 * 상태를 알 수 없는 하드웨어를 안전한 자리로 되돌리는 데 쓴다.
 *
 * BARE 모드는 "변환 없이 통과"라, 이 상태에서는 디바이스의 DMA가
 * 그대로 나간다. 끄는 것(OFF)보다 안전한 선택인데, OFF에서는
 * 모든 DMA가 폴트가 되어 진행 중이던 전송이 깨지기 때문이다.
 *
 * 응답을 기다리지 않는 이유: 크래시 상황에서는 하드웨어가
 * 응답하지 않을 수도 있어, 기다리면 덤프 자체가 멈춘다.
 *
 * 실행 컨텍스트: 크래시 덤프 커널의 초기화. 제한된 문맥.
 *
 * 호출 체인:
 *   riscv_iommu_init_check() / 플랫폼 코드 → [riscv_iommu_disable]
 */
void riscv_iommu_disable(struct riscv_iommu_device *iommu)
{
	/* [한국어] BARE 모드로 되돌린다 — 통과이지 차단이 아니다.
	 * 진행 중이던 DMA를 깨뜨리지 않으려는 선택이다. */
	riscv_iommu_writeq(iommu, RISCV_IOMMU_REG_DDTP,
			   FIELD_PREP(RISCV_IOMMU_DDTP_IOMMU_MODE,
				      RISCV_IOMMU_DDTP_IOMMU_MODE_BARE));
	/* [한국어] 명령 큐를 끈다. */
	riscv_iommu_writel(iommu, RISCV_IOMMU_REG_CQCSR, 0);
	/* [한국어] 폴트 큐를 끈다. */
	riscv_iommu_writel(iommu, RISCV_IOMMU_REG_FQCSR, 0);
	/* [한국어] 페이지 요청 큐를 끈다(이 드라이버는 쓰지 않지만
	 * 앞 커널이 켜 두었을 수 있다). */
	riscv_iommu_writel(iommu, RISCV_IOMMU_REG_PQCSR, 0);
}

/* [한국어] DDTP 레지스터를 busy가 내려갈 때까지 기다렸다 읽는다.
 * 식(statement expression)으로 만든 이유: 값을 돌려주면서
 * 타임아웃 대기까지 하나의 표현식으로 쓰기 위함이다.
 * 디렉터리 모드 전환은 하드웨어가 내부 캐시를 정리해야 해
 * 최대 10초까지 걸릴 수 있다. */
#define riscv_iommu_read_ddtp(iommu) ({ \
	u64 ddtp; \
	riscv_iommu_readq_timeout((iommu), RISCV_IOMMU_REG_DDTP, ddtp, \
				  !(ddtp & RISCV_IOMMU_DDTP_BUSY), 10, \
				  RISCV_IOMMU_DDTP_TIMEOUT); \
	ddtp; })	/* [한국어] 식의 값으로 읽은 DDTP를 내놓는다. */

/*
 * [한국어]
 * riscv_iommu_iodir_alloc - 디바이스 디렉터리의 뿌리를 마련한다
 *
 * @iommu: 대상 IOMMU.
 * @return: 0 성공, -EBUSY/-ENOMEM.
 *
 * 큐와 마찬가지로 WARL 관례를 쓴다. 현재 모드가 OFF나 BARE일 때
 * 모드만 다시 써 보면, 하드웨어가 디렉터리를 둘 **고정 주소**를
 * 알려 줄 수 있다(내장 메모리를 쓰는 구현). 그 자리가 있으면
 * ioremap 해서 쓰고, 없으면 시스템 메모리를 잡는다.
 *
 * 고정 주소를 쓸 때 memset으로 지우는 것이 중요하다 — 앞 커널이
 * 남긴 내용이 그대로 있을 수 있다.
 *
 * 실행 컨텍스트: 초기화. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_init() → [riscv_iommu_iodir_alloc]
 */
static int riscv_iommu_iodir_alloc(struct riscv_iommu_device *iommu)
{
	/* [한국어] 읽은 DDTP 값. */
	u64 ddtp;
	/* [한국어] 현재 디렉터리 모드. */
	unsigned int mode;

	/* [한국어] 하드웨어가 안정될 때까지 기다렸다 읽는다. */
	ddtp = riscv_iommu_read_ddtp(iommu);
	if (ddtp & RISCV_IOMMU_DDTP_BUSY)	/* [한국어] 하드웨어가 안정되지 않았다. */
		return -EBUSY;

	/*
	 * It is optional for the hardware to report a fixed address for device
	 * directory root page when DDT.MODE is OFF or BARE.
	 */
	/* [한국어] OFF나 BARE 상태에서만 고정 주소를 물어볼 수 있다. */
	mode = FIELD_GET(RISCV_IOMMU_DDTP_IOMMU_MODE, ddtp);
	if (mode == RISCV_IOMMU_DDTP_IOMMU_MODE_BARE ||	/* [한국어] 이 두 모드에서만 고정 주소를 물어볼 수 있다. */
	    mode == RISCV_IOMMU_DDTP_IOMMU_MODE_OFF) {
		/* Use WARL to discover hardware fixed DDT PPN */
		/* [한국어] 주소 없이 모드만 써 보고 되읽으면, 하드웨어가
		 * 자기가 정한 주소를 채워 돌려줄 수 있다. */
		riscv_iommu_writeq(iommu, RISCV_IOMMU_REG_DDTP,
				   FIELD_PREP(RISCV_IOMMU_DDTP_IOMMU_MODE, mode));
		ddtp = riscv_iommu_read_ddtp(iommu);	/* [한국어] 되읽어 하드웨어가 채운 주소를 확인한다. */
		if (ddtp & RISCV_IOMMU_DDTP_BUSY)	/* [한국어] 여전히 바쁘면 진행할 수 없다. */
			return -EBUSY;

		/* [한국어] 0이 아니면 하드웨어가 자리를 지정한 것이다. */
		iommu->ddt_phys = ppn_to_phys(ddtp);
		if (iommu->ddt_phys)	/* [한국어] 하드웨어가 자리를 지정했다면 그것을 매핑한다. */
			iommu->ddt_root = devm_ioremap(iommu->dev,
						       iommu->ddt_phys, PAGE_SIZE);
		/* [한국어] 앞 커널이 남긴 내용이 있을 수 있어 지운다. */
		if (iommu->ddt_root)
			memset(iommu->ddt_root, 0, PAGE_SIZE);
	}

	/* [한국어] 고정 주소가 없거나 매핑에 실패했으면 시스템 메모리에서
	 * 잡는다. 0으로 초기화되어 모든 엔트리가 무효로 시작한다. */
	if (!iommu->ddt_root) {
		iommu->ddt_root = riscv_iommu_get_pages(iommu, SZ_4K);	/* [한국어] 시스템 메모리에서 뿌리 페이지를 잡는다. */
		iommu->ddt_phys = __pa(iommu->ddt_root);	/* [한국어] 하드웨어에 알릴 물리 주소를 기억해 둔다. */
	}

	/* [한국어] 어느 쪽으로도 자리를 얻지 못했다. */
	if (!iommu->ddt_root)
		return -ENOMEM;

	return 0;	/* [한국어] 디렉터리 뿌리가 준비됐다. */
}

/*
 * Discover supported DDT modes starting from requested value,
 * configure DDTP register with accepted mode and root DDT address.
 * Accepted iommu->ddt_mode is updated on success.
 */
/*
 * [한국어]
 * riscv_iommu_iodir_set_mode - 지원되는 디렉터리 단계 수를 찾아 설정한다
 *
 * @iommu: 대상 IOMMU.
 * @ddtp_mode: 요청할 모드(보통 최대값에서 시작).
 * @return: 0 성공, -EBUSY/-EINVAL.
 *
 * WARL 탐색의 가장 정교한 예다. 원하는 단계 수를 써 보고 되읽어,
 * 하드웨어가 받아들이지 않으면 한 단계씩 낮추며 재시도한다.
 * 규격상 OFF/BARE와 최소 한 가지 xLVL은 반드시 지원되므로,
 * 이 루프는 반드시 끝난다.
 *
 * xLVL에서 xLVL로 직접 바꾸는 것을 금지하는 이유: 그 전환 중에는
 * 옛 디렉터리와 새 디렉터리가 섞여 해석될 수 있다. 반드시
 * OFF나 BARE를 거쳐야 한다.
 *
 * 마지막에 두 가지 무효화와 동기화를 보내는 것도 필수다.
 * 디렉터리가 통째로 바뀌었으므로 캐시된 디바이스 컨텍스트와
 * 주소 변환이 모두 무의미해진다.
 *
 * 실행 컨텍스트: 초기화와 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_init() / remove() → [riscv_iommu_iodir_set_mode]
 */
static int riscv_iommu_iodir_set_mode(struct riscv_iommu_device *iommu,
				      unsigned int ddtp_mode)
{
	/* [한국어] 로그의 기준 디바이스. */
	struct device *dev = iommu->dev;
	/* [한국어] 되읽은 값과, 우리가 쓴 값. */
	u64 ddtp, rq_ddtp;
	/* [한국어] 하드웨어가 받아들인 모드와, 이번에 요청할 모드. */
	unsigned int mode, rq_mode = ddtp_mode;
	/* [한국어] 마지막에 보낼 무효화 명령. */
	struct riscv_iommu_command cmd;

	/* [한국어] 현재 상태를 읽는다. */
	ddtp = riscv_iommu_read_ddtp(iommu);
	if (ddtp & RISCV_IOMMU_DDTP_BUSY)	/* [한국어] 하드웨어가 안정되지 않았다. */
		return -EBUSY;

	/* Disallow state transition from xLVL to xLVL. */
	/* [한국어] 단계 수가 있는 모드끼리 직접 바꾸면 옛 디렉터리와
	 * 새 디렉터리가 섞여 해석될 수 있다. OFF나 BARE를 거쳐야 한다. */
	mode = FIELD_GET(RISCV_IOMMU_DDTP_IOMMU_MODE, ddtp);
	if (mode != RISCV_IOMMU_DDTP_IOMMU_MODE_BARE &&	/* [한국어] 단계 있는 모드끼리의 직접 전환인지 검사한다. */
	    mode != RISCV_IOMMU_DDTP_IOMMU_MODE_OFF &&
	    rq_mode != RISCV_IOMMU_DDTP_IOMMU_MODE_BARE &&
	    rq_mode != RISCV_IOMMU_DDTP_IOMMU_MODE_OFF)
		return -EINVAL;

	do {
		/* [한국어] 이번에 시도할 값을 조립한다. */
		rq_ddtp = FIELD_PREP(RISCV_IOMMU_DDTP_IOMMU_MODE, rq_mode);
		/* [한국어] 단계가 있는 모드에서만 디렉터리 주소가 의미를 갖는다. */
		if (rq_mode > RISCV_IOMMU_DDTP_IOMMU_MODE_BARE)
			rq_ddtp |= phys_to_ppn(iommu->ddt_phys);

		/* [한국어] 쓰고 되읽어 하드웨어의 판단을 확인한다. */
		riscv_iommu_writeq(iommu, RISCV_IOMMU_REG_DDTP, rq_ddtp);
		ddtp = riscv_iommu_read_ddtp(iommu);	/* [한국어] 하드웨어의 판단을 되읽는다. */
		if (ddtp & RISCV_IOMMU_DDTP_BUSY) {	/* [한국어] 전환이 시간 안에 끝나지 않았다. */
			dev_err(dev, "timeout when setting ddtp (ddt mode: %u, read: %llx)\n",	/* [한국어] 어느 모드에서 막혔는지 남긴다. */
				rq_mode, ddtp);
			return -EBUSY;	/* [한국어] 더 시도할 수 없다. */
		}

		/* Verify IOMMU hardware accepts new DDTP config. */
		/* [한국어] 하드웨어가 실제로 채택한 모드. */
		mode = FIELD_GET(RISCV_IOMMU_DDTP_IOMMU_MODE, ddtp);

		/* [한국어] 원하던 그대로 받아들여졌다 — 끝. */
		if (rq_mode == mode)
			break;

		/* Hardware mandatory DDTP mode has not been accepted. */
		/* [한국어] OFF나 BARE는 규격상 반드시 지원해야 한다.
		 * 그것이 거부됐다면 하드웨어가 고장 났거나 규격 위반이다. */
		if (rq_mode < RISCV_IOMMU_DDTP_IOMMU_MODE_1LVL && rq_ddtp != ddtp) {
			dev_err(dev, "DDTP update failed hw: %llx vs %llx\n",	/* [한국어] 규격상 반드시 지원해야 할 모드가 거부됐다. */
				ddtp, rq_ddtp);
			return -EINVAL;	/* [한국어] 하드웨어를 신뢰할 수 없다. */
		}

		/*
		 * Mode field is WARL, an IOMMU may support a subset of
		 * directory table levels in which case if we tried to set
		 * an unsupported number of levels we'll readback either
		 * a valid xLVL or off/bare. If we got off/bare, try again
		 * with a smaller xLVL.
		 */
		/* [한국어] 요청한 단계 수를 지원하지 않아 OFF/BARE로
		 * 되돌아왔다 — 한 단계 낮춰 다시 시도한다. */
		if (mode < RISCV_IOMMU_DDTP_IOMMU_MODE_1LVL &&
		    rq_mode > RISCV_IOMMU_DDTP_IOMMU_MODE_1LVL) {
			dev_dbg(dev, "DDTP hw mode %u vs %u\n", mode, rq_mode);	/* [한국어] 요청한 단계 수가 거부됐음을 남긴다. */
			rq_mode--;	/* [한국어] 한 단계 낮춰 본다. */
			continue;	/* [한국어] 낮춘 값으로 다시 시도한다. */
		}

		/*
		 * We tried all supported modes and IOMMU hardware failed to
		 * accept new settings, something went very wrong since off/bare
		 * and at least one xLVL must be supported.
		 */
		/* [한국어] 1단계까지 낮췄는데도 거부됐다면 규격을 어긴
		 * 하드웨어다 — 더 시도할 것이 없다. */
		dev_err(dev, "DDTP hw mode %u, failed to set %u\n",
			mode, ddtp_mode);
		return -EINVAL;	/* [한국어] 1단계까지 낮췄는데도 실패했다 — 규격 위반이다. */
	} while (1);

	/* [한국어] 실제로 채택된 모드를 기록한다. get_dc가 이 값으로
	 * 워크 깊이를 정한다. */
	iommu->ddt_mode = mode;
	/* [한국어] 요청보다 낮은 단계로 정해졌다면 알려 둔다 —
	 * 쓸 수 있는 디바이스 ID 범위가 그만큼 줄어든다. */
	if (mode != ddtp_mode)
		dev_dbg(dev, "DDTP hw mode %u, requested %u\n", mode, ddtp_mode);

	/* Invalidate device context cache */
	/* [한국어] 디렉터리가 통째로 바뀌었으므로 캐시된 디바이스
	 * 컨텍스트가 모두 무의미하다. */
	riscv_iommu_cmd_iodir_inval_ddt(&cmd);
	riscv_iommu_cmd_send(iommu, &cmd);	/* [한국어] 디바이스 컨텍스트 캐시를 버리게 한다. */

	/* Invalidate address translation cache */
	/* [한국어] 주소 변환 캐시도 마찬가지다. */
	riscv_iommu_cmd_inval_vma(&cmd);
	riscv_iommu_cmd_send(iommu, &cmd);	/* [한국어] 주소 변환 캐시도 버리게 한다. */

	/* IOFENCE.C */
	/* [한국어] 두 무효화가 끝날 때까지 기다린다 — 이후 코드가
	 * 새 디렉터리를 전제로 동작하기 때문이다. */
	riscv_iommu_cmd_sync(iommu, RISCV_IOMMU_IOTINVAL_TIMEOUT);

	return 0;	/* [한국어] 디렉터리 모드가 확정됐다. */
}

/* This struct contains protection domain specific IOMMU driver data. */
/* [한국어] IOMMU 도메인 하나. */
struct riscv_iommu_domain {
	union {
	/* [한국어] 코어가 보는 도메인과 generic_pt 의 표 객체를 같은 메모리에 겹친 것.
	 * 왜 겹치는가: generic_pt 의 pt_iommu 구조체가 자기 안에 struct iommu_domain
	 *   을 품고 있다. 이 드라이버도 같은 iommu_domain 이 필요한데, 따로 두면
	 *   두 벌이 생겨 어느 것이 진짜인지 혼란스럽다. 같은 자리에 겹쳐 두면
	 *   변환 없이 두 관점을 오갈 수 있다.
	 * 읽는 자: 코어 진입점은 domain 으로 보고, 표를 다루는 코드는 generic_pt
	 *   객체로 본다.
	 * 제약: 두 관점의 iommu_domain 이 정확히 같은 오프셋에 있어야 한다. 그
	 *   전제가 깨지면 container_of 가 엉뚱한 곳을 가리킨다. */
		struct iommu_domain domain;
		/* [한국어] 코어가 보는 도메인.
		 * 공용체인 이유: generic_pt가 자기 구조체 안에 같은
		 * iommu_domain을 품고 있어, 두 관점을 같은 메모리에
		 * 겹쳐 두면 변환 없이 오갈 수 있다. */

		struct pt_iommu_riscv_64 riscvpt;
		/* [한국어] generic_pt가 관리하는 페이지 테이블 상태.
		 * 설정자: pt_iommu_riscv_64_init().
		 * 읽는 자: attach가 테이블 루트와 모드를 꺼내 DC에 심는다.
		 * 이 파일에 PTE를 만지는 코드가 없는 이유가 이것이다. */
	};

	struct list_head bonds;
	/* [한국어] 이 도메인에 붙어 있는 디바이스들의 목록.
	 * 설정자: bond_link가 추가, bond_unlink가 제거.
	 * 왜 정렬되는가: 같은 IOMMU에 속한 항목이 이웃하도록 넣어,
	 *                무효화 순회에서 직전과 비교하는 것만으로
	 *                중복 명령을 걸러 낼 수 있게 한다.
	 * 동기화: 갱신은 lock, 읽기는 RCU. */

	spinlock_t lock;		/* protect bonds list updates. */
	/* [한국어] bonds 목록의 갱신을 보호하는 락.
	 * 읽기 경로는 이 락을 잡지 않는다 — RCU가 대신한다. */

	int pscid;
	/* [한국어] 이 도메인의 TLB 태그.
	 * 설정자: domain_alloc이 ida로 받는다.
	 * 읽는 자: DC의 ta 필드에 실리고, 모든 무효화 명령의 대상이 된다.
	 * 값 범위: 1 ~ 2^20-1. 0을 쓰지 않는 것은 "할당되지 않음"과
	 *          구별하기 위함이다. */
};
/* [한국어] generic_pt가 요구하는 구조 검사. 공용체의 두 관점이
 * 실제로 같은 자리에 놓였는지 컴파일 시점에 확인한다. */
PT_IOMMU_CHECK_DOMAIN(struct riscv_iommu_domain, riscvpt.iommu, domain);

/* [한국어] 코어의 도메인 포인터에서 드라이버 쪽 도메인을 복원한다.
 * 공용체라 사실상 같은 주소지만, 형식을 지켜 둔다. */
#define iommu_domain_to_riscv(iommu_domain) \
	container_of(iommu_domain, struct riscv_iommu_domain, domain)	/* [한국어] 공용체라 사실상 같은 주소지만 형식을 지킨다. */

/* Private IOMMU data for managed devices, dev_iommu_priv_* */
/* [한국어] 디바이스에 매다는 최소한의 상태. */
struct riscv_iommu_info {
	struct riscv_iommu_domain *domain;
	/* [한국어] 현재 붙어 있는 도메인.
	 * 왜 필요한가: 새 도메인에 붙일 때 옛 도메인의 bond를 끊어야
	 *              하는데, 코어가 항상 old를 주지는 않는다.
	 * 값 범위: NULL이면 차단이나 통과 모드다 — 그 도메인들은
	 *          bond를 만들지 않는다. */
};

/*
 * Linkage between an iommu_domain and attached devices.
 *
 * Protection domain requiring IOATC and DevATC translation cache invalidations,
 * should be linked to attached devices using a riscv_iommu_bond structure.
 * Devices should be linked to the domain before first use and unlinked after
 * the translations from the referenced protection domain can no longer be used.
 * Blocking and identity domains are not tracked here, as the IOMMU hardware
 * does not cache negative and/or identity (BARE mode) translations, and DevATC
 * is disabled for those protection domains.
 *
 * The device pointer and IOMMU data remain stable in the bond struct after
 * _probe_device() where it's attached to the managed IOMMU, up to the
 * completion of the _release_device() call. The release of the bond structure
 * is synchronized with the device release.
 */
/* [한국어] 도메인과 디바이스를 잇는 고리.
 *
 * 원본 주석이 밝히는 핵심: **차단/통과 도메인은 bond를 만들지 않는다.**
 * 하드웨어가 "매핑 없음"이나 통과 변환을 캐시하지 않기 때문에
 * 무효화할 대상이 없어서다. 그래서 bond 목록은 곧 "무효화를
 * 보내야 할 디바이스들"의 목록이다. */
struct riscv_iommu_bond {
	struct list_head list;
	/* [한국어] 도메인의 bonds 목록에 매다는 고리.
	 * 동기화: 추가/제거는 락, 순회는 RCU. */

	struct rcu_head rcu;
	/* [한국어] 해제를 RCU 유예 뒤로 미루는 헤드.
	 * 왜 필요한가: 무효화 순회가 RCU 읽기 구역에서 이 항목을
	 *              보고 있을 수 있어, 즉시 해제하면 안 된다. */

	struct device *dev;
	/* [한국어] 이 고리가 가리키는 디바이스.
	 * 읽는 자: dev_to_iommu()로 어느 IOMMU에 명령을 보낼지 정한다.
	 * 수명: probe_device부터 release_device까지 유효함이 보장된다. */
};

/*
 * [한국어]
 * riscv_iommu_bond_link - 디바이스를 도메인의 무효화 대상에 등록한다
 *
 * @domain: 대상 도메인.
 * @dev: 붙일 디바이스.
 * @return: 0 성공, -ENOMEM.
 *
 * 삽입 위치를 고르는 것이 이 함수의 요점이다. **같은 IOMMU에
 * 속한 항목들 사이에 끼워 넣는다.** 그러면 목록이 IOMMU별로
 * 뭉쳐 있게 되어, 무효화 순회가 직전 항목과 비교하는 것만으로
 * 중복 명령을 걸러 낼 수 있다.
 *
 * 루프가 끝까지 돌면 bonds가 목록 머리를 가리키므로, 결국
 * 맨 뒤에 붙는다 — 처음 보는 IOMMU의 첫 항목이 되는 것이다.
 *
 * 마지막 smp_mb()가 중요하다. 이 등록이 보인 뒤에야 디렉터리
 * 갱신이 이뤄지도록 순서를 세우는데, iotlb_inval의 큰 주석이
 * 그 짝을 설명한다.
 *
 * 실행 컨텍스트: attach 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_attach_paging_domain() → [riscv_iommu_bond_link]
 */
static int riscv_iommu_bond_link(struct riscv_iommu_domain *domain,
				 struct device *dev)
{
	/* [한국어] 이 디바이스를 담당하는 IOMMU. */
	struct riscv_iommu_device *iommu = dev_to_iommu(dev);
	/* [한국어] 만들 고리. */
	struct riscv_iommu_bond *bond;
	/* [한국어] 삽입 위치를 찾는 커서. */
	struct list_head *bonds;

	bond = kzalloc_obj(*bond);	/* [한국어] 고리를 하나 만든다. */
	if (!bond)	/* [한국어] 고리를 만들지 못했다. */
		return -ENOMEM;
	/* [한국어] 이 고리가 가리킬 디바이스. */
	bond->dev = dev;

	/*
	 * List of devices attached to the domain is arranged based on
	 * managed IOMMU device.
	 */

	spin_lock(&domain->lock);
	/* [한국어] 같은 IOMMU에 속한 첫 항목을 찾는다. 그 앞에 끼워
	 * 넣으면 같은 IOMMU끼리 뭉치게 된다.
	 * 못 찾으면 커서가 목록 머리를 가리켜 맨 뒤에 붙는다. */
	list_for_each(bonds, &domain->bonds)
		if (dev_to_iommu(list_entry(bonds, struct riscv_iommu_bond, list)->dev) == iommu)
			break;
	/* [한국어] RCU 순회자가 도중에 봐도 안전한 방식으로 추가한다. */
	list_add_rcu(&bond->list, bonds);
	spin_unlock(&domain->lock);	/* [한국어] 목록 조작이 끝났으니 락을 놓는다. */

	/* Synchronize with riscv_iommu_iotlb_inval() sequence. See comment below. */
	/* [한국어] 이 등록이 다른 CPU에 보인 뒤에야 호출자가 디렉터리를
	 * 갱신하도록 순서를 세운다. 짝이 되는 장벽은 iotlb_inval의
	 * 첫 줄에 있고, 그 주석이 두 흐름의 대응을 그림으로 보여 준다. */
	smp_mb();

	return 0;	/* [한국어] 등록이 끝났다 — 이제 무효화가 이 디바이스에도 간다. */
}

/*
 * [한국어]
 * riscv_iommu_bond_unlink - 디바이스를 도메인의 무효화 대상에서 뺀다
 *
 * @domain: 대상 도메인(NULL일 수 있다).
 * @dev: 뺄 디바이스.
 * @return: 없음.
 *
 * 목록을 훑으며 두 가지를 한 번에 한다: 뺄 항목을 찾고, **같은
 * IOMMU에 속한 다른 항목이 남아 있는지 센다.** 남은 것이 없으면
 * 그 IOMMU는 이 도메인의 변환을 더 이상 쓰지 않으므로, 캐시된
 * 것을 전부 비워야 한다.
 *
 * 조기 종료 조건(found && count)이 세심하다 — 찾을 것을 찾았고
 * 다른 항목도 하나 이상 확인했으면 더 셀 필요가 없다.
 *
 * 해제를 kfree_rcu로 미루는 이유: 무효화 순회가 RCU 읽기 구역에서
 * 이 항목을 보고 있을 수 있다.
 *
 * 실행 컨텍스트: attach/detach 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   attach 계열 함수들 → [riscv_iommu_bond_unlink]
 */
static void riscv_iommu_bond_unlink(struct riscv_iommu_domain *domain,
				    struct device *dev)
{
	/* [한국어] 이 디바이스를 담당하는 IOMMU. */
	struct riscv_iommu_device *iommu = dev_to_iommu(dev);
	/* [한국어] 순회 커서와, 찾아낸 항목. */
	struct riscv_iommu_bond *bond, *found = NULL;
	/* [한국어] 마지막 고리였을 때 보낼 무효화 명령. */
	struct riscv_iommu_command cmd;
	/* [한국어] 같은 IOMMU에 속한 **다른** 항목의 수. */
	int count = 0;

	/* [한국어] 차단/통과 도메인에서 오면 도메인이 없다. */
	if (!domain)
		return;

	spin_lock(&domain->lock);	/* [한국어] 목록을 훑는 동안 갱신을 막는다. */
	list_for_each_entry(bond, &domain->bonds, list) {
		/* [한국어] 찾을 것을 찾았고 다른 항목도 확인했으면
		 * 더 셀 이유가 없다. */
		if (found && count)
			break;
		else if (bond->dev == dev)	/* [한국어] 뺄 대상을 찾았다. */
			found = bond;
		/* [한국어] 같은 IOMMU를 쓰는 다른 디바이스가 남아 있다. */
		else if (dev_to_iommu(bond->dev) == iommu)
			count++;
	}
	if (found)
		/* [한국어] RCU 순회자가 안전하게 빠져나갈 수 있는 제거다. */
		list_del_rcu(&found->list);
	spin_unlock(&domain->lock);
	/* [한국어] 순회 중인 읽기가 끝난 뒤 해제한다. found가 NULL이어도
	 * 안전하다. */
	kfree_rcu(found, rcu);

	/*
	 * If this was the last bond between this domain and the IOMMU
	 * invalidate all cached entries for domain's PSCID.
	 */
	/* [한국어] 이 IOMMU에서 이 도메인을 쓰는 디바이스가 사라졌다 —
	 * 캐시된 변환이 남아 있으면 나중에 다른 도메인이 같은 PSCID를
	 * 받았을 때 잘못 적중할 수 있다. */
	if (!count) {
		riscv_iommu_cmd_inval_vma(&cmd);	/* [한국어] 이 PSCID의 모든 변환을 버리는 명령을 만든다. */
		riscv_iommu_cmd_inval_set_pscid(&cmd, domain->pscid);	/* [한국어] 대상 PSCID를 지정한다. */
		riscv_iommu_cmd_send(iommu, &cmd);

		/* [한국어] 무효화가 실제로 끝났음을 확인하고 돌아간다. */
		riscv_iommu_cmd_sync(iommu, RISCV_IOMMU_IOTINVAL_TIMEOUT);
	}
}

/*
 * Send IOTLB.INVAL for whole address space for ranges larger than 2MB.
 * This limit will be replaced with range invalidations, if supported by
 * the hardware, when RISC-V IOMMU architecture specification update for
 * range invalidations update will be available.
 */
/* [한국어] 주소별 무효화와 전체 무효화의 손익분기점(2MB).
 * 1.0 규격에는 범위 무효화 명령이 없어, 넓은 범위는 페이지마다
 * 명령을 내야 한다. 512개(2MB/4KB)를 넘어가면 그 PSCID 전체를
 * 비우는 편이 싸다는 판단이다. */
#define RISCV_IOMMU_IOTLB_INVAL_LIMIT	(2 << 20)

/*
 * [한국어]
 * riscv_iommu_iotlb_inval - 도메인의 모든 IOMMU에서 주소 범위를 무효화한다
 *
 * @domain: 대상 도메인.
 * @start: 범위의 시작.
 * @end: 범위의 끝.
 * @return: 없음.
 *
 * 원본의 큰 주석이 이 함수의 가장 미묘한 부분을 설명한다:
 * **페이지 테이블 갱신 흐름과 디바이스 부착 흐름의 경쟁**이다.
 *
 *   테이블 갱신 + 무효화              디바이스 부착 + 디렉터리 갱신
 *   ------------------------          ------------------------
 *   엔트리를 고친다                   bond 목록에 디바이스를 넣는다
 *   FENCE RW,RW                       FENCE RW,RW
 *   목록의 IOMMU마다:                 DC의 fsc/PSCID를 갱신한다
 *     무효화 + IOFENCE.C                디렉터리 무효화
 *
 * 두 흐름 사이에 장벽을 대칭으로 두면 어느 순서로 겹쳐도 안전하다.
 * 부착이 먼저 보이면 무효화가 그 디바이스에도 전달되고, 부착이
 * 나중이면 그 디바이스는 이미 갱신된 테이블을 처음부터 본다.
 *
 * 순회를 두 번 도는 것도 의도적이다. 먼저 모든 IOMMU에 무효화를
 * 보내 두고, 그다음에 각각의 완료를 기다린다. 하나씩 보내고
 * 기다리면 IOMMU 수만큼 대기가 직렬화된다.
 *
 * 실행 컨텍스트: 무효화 경로. RCU 읽기 구역.
 *
 * 호출 체인:
 *   iotlb_sync() / flush_iotlb_all() → [riscv_iommu_iotlb_inval]
 */
static void riscv_iommu_iotlb_inval(struct riscv_iommu_domain *domain,
				    unsigned long start, unsigned long end)
{
	/* [한국어] 순회 커서. */
	struct riscv_iommu_bond *bond;
	/* [한국어] 현재 IOMMU와, 직전에 명령을 보낸 IOMMU. */
	struct riscv_iommu_device *iommu, *prev;
	/* [한국어] 조립할 무효화 명령. */
	struct riscv_iommu_command cmd;

	/*
	 * For each IOMMU linked with this protection domain (via bonds->dev),
	 * an IOTLB invaliation command will be submitted and executed.
	 *
	 * Possbile race with domain attach flow is handled by sequencing
	 * bond creation - riscv_iommu_bond_link(), and device directory
	 * update - riscv_iommu_iodir_update().
	 *
	 * PTE Update / IOTLB Inval           Device attach & directory update
	 * --------------------------         --------------------------
	 * update page table entries          add dev to the bond list
	 * FENCE RW,RW                        FENCE RW,RW
	 * For all IOMMUs: (can be empty)     Update FSC/PSCID
	 *   FENCE IOW,IOW                      FENCE IOW,IOW
	 *   IOTLB.INVAL                        IODIR.INVAL
	 *   IOFENCE.C
	 *
	 * If bond list is not updated with new device, directory context will
	 * be configured with already valid page table content. If an IOMMU is
	 * linked to the protection domain it will receive invalidation
	 * requests for updated page table entries.
	 */
	/* [한국어] 테이블 갱신이 보인 뒤에야 bond 목록을 읽도록 순서를
	 * 세운다. bond_link의 장벽과 짝을 이룬다. */
	smp_mb();

	/* [한국어] 목록을 락 없이 훑는다. */
	rcu_read_lock();

	/* [한국어] 첫 항목은 반드시 명령을 받도록 NULL로 시작한다. */
	prev = NULL;
	list_for_each_entry_rcu(bond, &domain->bonds, list) {	/* [한국어] bond 목록을 락 없이 훑는다. */
		iommu = dev_to_iommu(bond->dev);	/* [한국어] 이 고리가 가리키는 디바이스의 IOMMU. */

		/*
		 * IOTLB invalidation request can be safely omitted if already sent
		 * to the IOMMU for the same PSCID, and with domain->bonds list
		 * arranged based on the device's IOMMU, it's sufficient to check
		 * last device the invalidation was sent to.
		 */
		/* [한국어] 무효화는 PSCID 단위라 같은 IOMMU에 두 번 보낼
		 * 이유가 없다. 목록이 IOMMU별로 뭉쳐 있어 직전 항목만
		 * 비교하면 충분하다 — bond_link의 정렬이 여기서 값을 한다. */
		if (iommu == prev)
			continue;

		/* [한국어] 이 도메인의 PSCID를 대상으로 하는 무효화를 만든다. */
		riscv_iommu_cmd_inval_vma(&cmd);
		riscv_iommu_cmd_inval_set_pscid(&cmd, domain->pscid);
		/* [한국어] 범위가 좁으면 페이지마다 정확히 무효화한다. */
		if (end - start < RISCV_IOMMU_IOTLB_INVAL_LIMIT - 1) {
			/* [한국어] 현재 무효화할 주소. */
			unsigned long iova = start;

			do {
				/* [한국어] 명령에 주소를 얹어 보낸다. */
				riscv_iommu_cmd_inval_set_addr(&cmd, iova);
				riscv_iommu_cmd_send(iommu, &cmd);
			/* [한국어] 오버플로 검사를 겸한 전진 — end가
			 * 주소 공간의 끝일 때 무한 루프를 막는다. */
			} while (!check_add_overflow(iova, PAGE_SIZE, &iova) &&
				 iova < end);
		} else {
			/* [한국어] 넓은 범위는 주소 없이 보내 그 PSCID
			 * 전체를 비운다 — 페이지마다 보내는 것보다 싸다. */
			riscv_iommu_cmd_send(iommu, &cmd);
		}
		prev = iommu;	/* [한국어] 다음 항목의 중복 판정 기준이 된다. */
	}

	/* [한국어] 두 번째 순회 — 이제 각 IOMMU의 완료를 기다린다.
	 * 보내기와 기다리기를 나눈 덕분에 대기가 겹쳐 일어난다. */
	prev = NULL;
	list_for_each_entry_rcu(bond, &domain->bonds, list) {	/* [한국어] 두 번째 순회 — 이제 완료를 기다린다. */
		iommu = dev_to_iommu(bond->dev);
		/* [한국어] 같은 IOMMU는 한 번만 기다리면 된다. */
		if (iommu == prev)
			continue;

		riscv_iommu_cmd_sync(iommu, RISCV_IOMMU_IOTINVAL_TIMEOUT);	/* [한국어] 이 IOMMU의 무효화가 실제로 끝날 때까지 기다린다. */
		prev = iommu;	/* [한국어] 다음 항목의 중복 판정 기준. */
	}
	rcu_read_unlock();	/* [한국어] 순회가 끝났으니 읽기 구역을 빠져나온다. */
}

/* [한국어] DC의 fsc 필드에 쓸 "변환 없음" 값.
 * 차단과 통과 도메인이 모두 이 값을 쓰고, ta의 유효 비트로
 * 둘을 구별한다. */
#define RISCV_IOMMU_FSC_BARE 0
/*
 * This function sends IOTINVAL commands as required by the RISC-V
 * IOMMU specification (Section 6.3.1 and 6.3.2 in 1.0 spec version)
 * after modifying DDT or PDT entries
 */
/*
 * [한국어]
 * riscv_iommu_iodir_iotinval - DC/PC 변경 뒤 규격이 요구하는 무효화를 보낸다
 *
 * @iommu: 대상 IOMMU.
 * @inval_pdt: 프로세스 디렉터리를 고쳤는가.
 * @iohgatp: DC의 2단계 변환 설정(가상화 여부를 알려 준다).
 * @dc: 고친 디바이스 컨텍스트.
 * @pc: 고친 프로세스 컨텍스트(현재는 항상 NULL).
 * @return: 없음.
 *
 * 규격 6.3.1/6.3.2가 정한 표를 코드로 옮긴 것이다. 무효화 명령의
 * 범위를 어디까지 좁힐 수 있는지가 상황마다 다르다:
 *
 *  - 2단계 변환이 없고(BARE) PSCID를 알 수 있으면 → 그 PSCID만
 *  - 2단계 변환이 있으면 → GSCID를 지정하고, 필요하면 PSCID까지
 *  - 아무것도 특정할 수 없으면 → 전부 비운다(GV=AV=PSCV=0)
 *
 * 좁힐수록 무관한 변환이 살아남아 성능에 유리하므로, 이렇게
 * 세밀하게 나누는 것이다.
 *
 * 실행 컨텍스트: 디렉터리 갱신 경로.
 *
 * 호출 체인:
 *   riscv_iommu_iodir_update() → [riscv_iommu_iodir_iotinval]
 */
static void riscv_iommu_iodir_iotinval(struct riscv_iommu_device *iommu,
				       bool inval_pdt, unsigned long iohgatp,
				       struct riscv_iommu_dc *dc,
				       struct riscv_iommu_pc *pc)
{
	/* [한국어] 조립할 무효화 명령. */
	struct riscv_iommu_command cmd;

	/* [한국어] 기본은 "모두 비우기"이고, 아래에서 조건을 좁힌다. */
	riscv_iommu_cmd_inval_vma(&cmd);

	/* [한국어] 2단계(호스트 게스트) 변환이 없는 경우. */
	if (FIELD_GET(RISCV_IOMMU_DC_IOHGATP_MODE, iohgatp) ==
	    RISCV_IOMMU_DC_IOHGATP_MODE_BARE) {
		if (inval_pdt) {	/* [한국어] 프로세스 컨텍스트를 고쳤다면 그 PSCID로 좁힌다. */
			/*
			 * IOTINVAL.VMA with GV=AV=0, and PSCV=1, and
			 * PSCID=PC.PSCID
			 */
			/* [한국어] 프로세스 컨텍스트를 고쳤다면 그 PSCID만
			 * 비우면 된다. */
			riscv_iommu_cmd_inval_set_pscid(&cmd,
				FIELD_GET(RISCV_IOMMU_PC_TA_PSCID, pc->ta));
		} else {
			/* [한국어] 프로세스 디렉터리를 쓰지 않고(PDTV=0)
			 * 1단계 변환이 켜져 있다면, DC 자신의 PSCID가
			 * 무효화 대상을 특정해 준다. */
			if (!FIELD_GET(RISCV_IOMMU_DC_TC_PDTV, dc->tc) &&
			    FIELD_GET(RISCV_IOMMU_DC_FSC_MODE, dc->fsc) !=
			    RISCV_IOMMU_DC_FSC_MODE_BARE) {
				/*
				 * DC.tc.PDTV == 0 && DC.fsc.MODE != Bare
				 * IOTINVAL.VMA with GV=AV=0, and PSCV=1, and
				 * PSCID=DC.ta.PSCID
				 */
				/* [한국어] DC의 PSCID로 범위를 좁힌다. */
				riscv_iommu_cmd_inval_set_pscid(&cmd,
					FIELD_GET(RISCV_IOMMU_DC_TA_PSCID, dc->ta));
			}
			/* else: IOTINVAL.VMA with GV=AV=PSCV=0 */
			/* [한국어] 좁힐 근거가 없으면 조립해 둔 그대로,
			 * 즉 모두 비우는 명령이 나간다. */
		}
	} else {
		/* [한국어] 2단계 변환이 있다면 게스트 식별자로 범위를 좁힌다. */
		riscv_iommu_cmd_inval_set_gscid(&cmd,
			FIELD_GET(RISCV_IOMMU_DC_IOHGATP_GSCID, iohgatp));

		if (inval_pdt) {	/* [한국어] 게스트와 프로세스를 모두 특정할 수 있는 경우. */
			/*
			 * IOTINVAL.VMA with GV=1, AV=0, and PSCV=1, and
			 * GSCID=DC.iohgatp.GSCID, PSCID=PC.PSCID
			 */
			/* [한국어] 게스트와 프로세스를 모두 특정해 가장
			 * 좁은 범위로 만든다. */
			riscv_iommu_cmd_inval_set_pscid(&cmd,
				FIELD_GET(RISCV_IOMMU_PC_TA_PSCID, pc->ta));
		}
		/*
		 * else: IOTINVAL.VMA with GV=1,AV=PSCV=0,and
		 * GSCID=DC.iohgatp.GSCID
		 *
		 * IOTINVAL.GVMA with GV=1,AV=0,and
		 * GSCID=DC.iohgatp.GSCID
		 * TODO: For now, the Second-Stage feature have not yet been merged,
		 * also issue IOTINVAL.GVMA once second-stage support is merged.
		 */
	}
	/* [한국어] 상황에 맞게 좁혀진 명령을 보낸다. 완료 대기는
	 * 호출자가 모아서 한다. */
	riscv_iommu_cmd_send(iommu, &cmd);
}
/*
 * Update IODIR for the device.
 *
 * During the execution of riscv_iommu_probe_device(), IODIR entries are
 * allocated for the device's identifiers.  Device context invalidation
 * becomes necessary only if one of the updated entries was previously
 * marked as valid, given that invalid device context entries are not
 * cached by the IOMMU hardware.
 * In this implementation, updating a valid device context while the
 * device is not quiesced might be disruptive, potentially causing
 * interim translation faults.
 */
/*
 * [한국어]
 * riscv_iommu_iodir_update - 디바이스 컨텍스트를 새 설정으로 바꾼다
 *
 * @iommu: 대상 IOMMU.
 * @dev: 대상 디바이스.
 * @fsc: 새 1단계 변환 설정(페이지 테이블 루트와 모드).
 * @ta: 새 변환 속성(PSCID와 유효 비트).
 * @return: 없음.
 *
 * DC를 안전하게 바꾸는 순서가 이 함수의 전부다. 두 번의 순회로
 * 나뉘어 있다.
 *
 *  1) **먼저 유효 비트를 내리고 무효화한다.** 이미 유효했던
 *     DC를 그대로 덮어쓰면, 하드웨어가 절반만 바뀐 상태를 읽을
 *     수 있다. 무효로 만든 뒤 캐시를 비우면 그 창이 닫힌다.
 *     원래 무효였던 항목은 캐시되지 않으므로 건너뛴다.
 *  2) **그다음 값을 쓰고 유효 비트를 마지막에 올린다.** 쓰기
 *     장벽(dma_wmb) 뒤에 유효 비트를 올려, 하드웨어가 유효를
 *     본 순간에는 나머지 필드가 이미 제자리에 있게 한다.
 *
 * 원본 주석이 밝히듯, 디바이스가 정지하지 않은 상태에서 유효한
 * DC를 바꾸면 그 사이 일시적인 폴트가 날 수 있다 — 이것은
 * 알려진 한계다.
 *
 * 실행 컨텍스트: attach 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   attach 계열 함수들 → [riscv_iommu_iodir_update]
 *   → riscv_iommu_get_dc() → riscv_iommu_iodir_iotinval()
 */
static void riscv_iommu_iodir_update(struct riscv_iommu_device *iommu,
				     struct device *dev, u64 fsc, u64 ta)
{
	/* [한국어] 이 디바이스가 내는 모든 디바이스 ID. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 현재 다루는 디바이스 컨텍스트. */
	struct riscv_iommu_dc *dc;
	/* [한국어] 조립할 디렉터리 무효화 명령. */
	struct riscv_iommu_command cmd;
	/* [한국어] 1단계에서 실제로 무효화한 것이 있는가. */
	bool sync_required = false;
	/* [한국어] DC의 변환 제어 필드. */
	u64 tc;
	/* [한국어] ID 순회 인덱스. */
	int i;

	/* [한국어] 1단계: 이미 유효한 DC를 먼저 무효로 만든다. */
	for (i = 0; i < fwspec->num_ids; i++) {
		dc = riscv_iommu_get_dc(iommu, fwspec->ids[i]);	/* [한국어] 이 ID의 디바이스 컨텍스트를 찾는다. */
		tc = READ_ONCE(dc->tc);
		/* [한국어] 무효한 DC는 하드웨어가 캐시하지 않으므로
		 * 그냥 덮어써도 된다 — 건너뛴다. */
		if (!(tc & RISCV_IOMMU_DC_TC_V))
			continue;

		/* [한국어] 유효 비트를 내려 하드웨어가 이 DC를 쓰지 못하게 한다. */
		WRITE_ONCE(dc->tc, tc & ~RISCV_IOMMU_DC_TC_V);

		/* Invalidate device context cached values */
		/* [한국어] 이미 캐시된 옛 DC를 버리게 한다. */
		riscv_iommu_cmd_iodir_inval_ddt(&cmd);
		riscv_iommu_cmd_iodir_set_did(&cmd, fwspec->ids[i]);	/* [한국어] 무효화 대상 디바이스 ID를 지정한다. */
		riscv_iommu_cmd_send(iommu, &cmd);	/* [한국어] 디렉터리 캐시 무효화를 보낸다. */
		/*
		 * For now, the SVA and PASID features have not yet been merged, the
		 * default configuration is inval_pdt=false and pc=NULL.
		 */
		/* [한국어] 그 DC가 가리키던 주소 변환도 함께 비운다. */
		riscv_iommu_iodir_iotinval(iommu, false, dc->iohgatp, dc, NULL);
		sync_required = true;	/* [한국어] 무효화를 보냈으니 아래에서 완료를 기다려야 한다. */
	}

	/* [한국어] 무효화를 보냈다면 그것이 끝난 뒤에 값을 써야 한다 —
	 * 그러지 않으면 하드웨어가 옛 캐시로 새 설정을 덮어쓸 수 있다. */
	if (sync_required)
		riscv_iommu_cmd_sync(iommu, RISCV_IOMMU_IOTINVAL_TIMEOUT);

	/*
	 * For device context with DC_TC_PDTV = 0, translation attributes valid bit
	 * is stored as DC_TC_V bit (both sharing the same location at BIT(0)).
	 */
	/* [한국어] 2단계: 새 설정을 쓰고 마지막에 유효 비트를 올린다. */
	for (i = 0; i < fwspec->num_ids; i++) {
		dc = riscv_iommu_get_dc(iommu, fwspec->ids[i]);	/* [한국어] 같은 ID의 디바이스 컨텍스트를 다시 찾는다. */
		tc = READ_ONCE(dc->tc);
		/* [한국어] ta의 유효 비트가 곧 tc의 유효 비트다 — 둘이
		 * 같은 자리(비트 0)를 쓴다는 원본 주석의 설명대로다.
		 * 그래서 호출자가 ta로 유효 여부까지 전달할 수 있다. */
		tc |= ta & RISCV_IOMMU_DC_TC_V;

		/* [한국어] 페이지 테이블 루트와 모드를 쓴다. */
		WRITE_ONCE(dc->fsc, fsc);
		/* [한국어] PSCID를 쓴다. 유효 비트는 여기서 제외한다. */
		WRITE_ONCE(dc->ta, ta & RISCV_IOMMU_PC_TA_PSCID);
		/* Update device context, write TC.V as the last step. */
		/* [한국어] 앞의 두 쓰기가 하드웨어에 보인 뒤에 유효 비트를
		 * 올려야, 유효를 본 순간 나머지가 이미 제자리에 있다. */
		dma_wmb();
		WRITE_ONCE(dc->tc, tc);	/* [한국어] 유효 비트를 마지막에 올려 갱신을 확정한다. */

		/* Invalidate device context after update */
		/* [한국어] 새 DC를 하드웨어가 다시 읽도록 캐시를 비운다. */
		riscv_iommu_cmd_iodir_inval_ddt(&cmd);
		riscv_iommu_cmd_iodir_set_did(&cmd, fwspec->ids[i]);	/* [한국어] 무효화 대상 디바이스 ID를 지정한다. */
		riscv_iommu_cmd_send(iommu, &cmd);	/* [한국어] 새 DC를 하드웨어가 다시 읽게 한다. */
		/*
		 * For now, the SVA and PASID features have not yet been merged, the
		 * default configuration is inval_pdt=false and pc=NULL.
		 */
		/* [한국어] 관련된 주소 변환 캐시도 함께 비운다. */
		riscv_iommu_iodir_iotinval(iommu, false, dc->iohgatp, dc, NULL);
	}

	/* [한국어] 모든 갱신과 무효화가 실제로 끝난 뒤에 돌아간다 —
	 * 호출자는 이 시점부터 새 설정이 유효하다고 전제한다. */
	riscv_iommu_cmd_sync(iommu, RISCV_IOMMU_IOTINVAL_TIMEOUT);
}

/*
 * IOVA page translation tree management.
 */

/*
 * [한국어]
 * riscv_iommu_iotlb_flush_all - 이 도메인의 모든 변환 캐시를 비운다
 *
 * @iommu_domain: 대상 도메인.
 * @return: 없음.
 *
 * 주소 범위를 전체로 지정하면 inval 함수가 알아서 "PSCID 통째로
 * 비우기"를 선택한다 — 2MB 한계를 훨씬 넘기 때문이다.
 *
 * 실행 컨텍스트: 코어의 전체 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 flush_iotlb_all → [riscv_iommu_iotlb_flush_all]
 */
static void riscv_iommu_iotlb_flush_all(struct iommu_domain *iommu_domain)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct riscv_iommu_domain *domain = iommu_domain_to_riscv(iommu_domain);

	/* [한국어] 전체 범위를 넘기면 자연히 PSCID 통째 비우기가 된다. */
	riscv_iommu_iotlb_inval(domain, 0, ULONG_MAX);
}

/*
 * [한국어]
 * riscv_iommu_iotlb_sync - 모아 둔 해제 범위를 무효화한다
 *
 * @iommu_domain: 대상 도메인.
 * @gather: 코어가 모은 범위와 해제 대기 페이지 목록.
 * @return: 없음.
 *
 * 두 갈래가 있고, 그 이유가 규격의 한계에 있다.
 *
 * 해제 목록이 비어 있으면 말단 엔트리만 바뀐 것이므로 모은 범위만
 * 무효화하면 된다. 그러나 목록에 페이지가 있다면 **중간 단계
 * 테이블이 사라진 것**이고, 1.0 규격에는 비말단 엔트리를 지정해
 * 무효화하는 명령이 없다. 그래서 그 PSCID 전체를 비우는 수밖에
 * 없다 — 원본 주석이 그 사정을 밝힌다.
 *
 * 페이지 반납을 무효화 **뒤에** 하는 순서도 중요하다. 하드웨어가
 * 아직 그 테이블을 캐시하고 있을 수 있는 동안 메모리를 재사용하면
 * 안 된다.
 *
 * 실행 컨텍스트: 해제 후 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync → [riscv_iommu_iotlb_sync]
 */
static void riscv_iommu_iotlb_sync(struct iommu_domain *iommu_domain,
				   struct iommu_iotlb_gather *gather)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct riscv_iommu_domain *domain = iommu_domain_to_riscv(iommu_domain);

	/* [한국어] 반납할 테이블이 없다 = 말단 엔트리만 바뀌었다. */
	if (iommu_pages_list_empty(&gather->freelist)) {
		riscv_iommu_iotlb_inval(domain, gather->start, gather->end);	/* [한국어] 말단 엔트리만 바뀌었으니 모은 범위만 비운다. */
	} else {
		/*
		 * In 1.0 spec version, the smallest scope we can use to
		 * invalidate all levels of page table (i.e. leaf and non-leaf)
		 * is an invalidate-all-PSCID IOTINVAL.VMA with AV=0.
		 * This will be updated with hardware support for
		 * capability.NL (non-leaf) IOTINVAL command.
		 */
		/* [한국어] 중간 테이블이 사라졌는데 1.0 규격에는 그것을
		 * 지정해 비우는 명령이 없다 — PSCID 전체를 비운다. */
		riscv_iommu_iotlb_inval(domain, 0, ULONG_MAX);
		/* [한국어] 무효화가 끝난 뒤에야 메모리를 반납한다.
		 * 순서가 반대면 하드웨어가 캐시된 주소로 재사용된 메모리를
		 * 테이블로 읽을 수 있다. */
		iommu_put_pages_list(&gather->freelist);
	}
}

/*
 * [한국어]
 * riscv_iommu_free_paging_domain - 페이징 도메인을 해제한다
 *
 * @iommu_domain: 해제할 도메인.
 * @return: 없음.
 *
 * 도메인 생성이 실패했을 때의 정리 경로로도 쓰인다. 그래서
 * PSCID가 아직 할당되지 않은 경우(음수)를 검사한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 free / alloc_paging_domain의 오류 경로
 *   → [riscv_iommu_free_paging_domain]
 */
static void riscv_iommu_free_paging_domain(struct iommu_domain *iommu_domain)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct riscv_iommu_domain *domain = iommu_domain_to_riscv(iommu_domain);

	/* [한국어] 해제 시점에 붙어 있는 디바이스가 있으면 코어가
	 * detach를 빠뜨린 것이다. */
	WARN_ON(!list_empty(&domain->bonds));

	/* [한국어] 생성 도중 실패해 아직 PSCID를 못 받았을 수 있다. */
	if ((int)domain->pscid > 0)
		ida_free(&riscv_iommu_pscids, domain->pscid);

	/* [한국어] 페이지 테이블 전체는 generic_pt가 정리한다. */
	pt_iommu_deinit(&domain->riscvpt.iommu);
	kfree(domain);	/* [한국어] 도메인 구조체를 해제한다. */
}

/*
 * [한국어]
 * riscv_iommu_pt_supported - 이 하드웨어가 그 페이지 테이블 모드를 지원하는가
 *
 * @iommu: 대상 IOMMU.
 * @pgd_mode: 확인할 모드(Sv39/Sv48/Sv57).
 * @return: 지원하면 참.
 *
 * Sv39/48/57은 각각 3/4/5단계 테이블에 대응하며, IOVA 폭이
 * 39/48/57비트다. generic_pt가 만든 테이블의 모드가 이 IOMMU에서
 * 쓸 수 있는 것인지 attach 시점에 확인한다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   riscv_iommu_attach_paging_domain() → [riscv_iommu_pt_supported]
 */
static bool riscv_iommu_pt_supported(struct riscv_iommu_device *iommu, int pgd_mode)
{
	/* [한국어] 모드마다 대응하는 능력 비트가 따로 있다. */
	switch (pgd_mode) {
	/* [한국어] 3단계, 39비트 IOVA. */
	case RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV39:
		return iommu->caps & RISCV_IOMMU_CAPABILITIES_SV39;

	/* [한국어] 4단계, 48비트 IOVA. */
	case RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV48:
		return iommu->caps & RISCV_IOMMU_CAPABILITIES_SV48;

	/* [한국어] 5단계, 57비트 IOVA. */
	case RISCV_IOMMU_DC_FSC_IOSATP_MODE_SV57:
		return iommu->caps & RISCV_IOMMU_CAPABILITIES_SV57;	/* [한국어] 5단계 테이블을 지원하는지 본다. */
	}
	/* [한국어] 그 밖의 모드는 이 드라이버가 다루지 않는다. */
	return false;
}

/*
 * [한국어]
 * riscv_iommu_attach_paging_domain - 디바이스를 페이징 도메인에 붙인다
 *
 * @iommu_domain: 붙일 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(이 드라이버는 info->domain을 쓴다).
 * @return: 0 성공, -ENODEV/-ENOMEM.
 *
 * 순서가 이 함수의 핵심이다.
 *
 *  1) **먼저 bond를 만든다.** 아직 디렉터리는 옛 설정이지만,
 *     이 순간부터 무효화가 이 디바이스에도 전달된다. 반대로 하면
 *     디렉터리는 새 테이블을 가리키는데 무효화는 오지 않는 창이
 *     생긴다 — iotlb_inval의 큰 주석이 설명하는 바로 그 경쟁이다.
 *  2) 디렉터리를 갱신한다.
 *  3) 그다음에야 옛 도메인의 bond를 끊는다.
 *
 * 즉 겹치는 구간을 허용하되, 비는 구간은 만들지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [riscv_iommu_attach_paging_domain]
 *   → riscv_iommu_bond_link() → riscv_iommu_iodir_update()
 */
static int riscv_iommu_attach_paging_domain(struct iommu_domain *iommu_domain,
					    struct device *dev,
					    struct iommu_domain *old)
{
	/* [한국어] 붙일 도메인. */
	struct riscv_iommu_domain *domain = iommu_domain_to_riscv(iommu_domain);
	/* [한국어] 이 디바이스를 담당하는 IOMMU. */
	struct riscv_iommu_device *iommu = dev_to_iommu(dev);
	/* [한국어] 디바이스의 현재 도메인을 기억해 둔 상태. */
	struct riscv_iommu_info *info = dev_iommu_priv_get(dev);
	/* [한국어] generic_pt가 알려 줄 테이블 정보. */
	struct pt_iommu_riscv_64_hw_info pt_info;
	/* [한국어] DC에 쓸 두 값. */
	u64 fsc, ta;

	/* [한국어] 페이지 테이블의 루트 주소와 모드를 얻는다. */
	pt_iommu_riscv_64_hw_info(&domain->riscvpt, &pt_info);

	/* [한국어] 이 IOMMU가 그 단계 수를 지원하지 않으면 붙일 수 없다. */
	if (!riscv_iommu_pt_supported(iommu, pt_info.fsc_iosatp_mode))
		return -ENODEV;

	/* [한국어] 1단계 변환 설정 — 모드와 테이블 루트의 페이지 번호. */
	fsc = FIELD_PREP(RISCV_IOMMU_PC_FSC_MODE, pt_info.fsc_iosatp_mode) |
	      FIELD_PREP(RISCV_IOMMU_PC_FSC_PPN, pt_info.ppn);
	/* [한국어] 변환 속성 — TLB 태그가 될 PSCID와 유효 비트. */
	ta = FIELD_PREP(RISCV_IOMMU_PC_TA_PSCID, domain->pscid) |
	     RISCV_IOMMU_PC_TA_V;

	/* [한국어] 디렉터리를 고치기 전에 bond를 먼저 만든다 —
	 * 무효화가 오지 않는 창을 없애기 위함이다. */
	if (riscv_iommu_bond_link(domain, dev))
		return -ENOMEM;

	/* [한국어] 이제 디바이스 컨텍스트를 새 테이블로 바꾼다. */
	riscv_iommu_iodir_update(iommu, dev, fsc, ta);
	/* [한국어] 마지막에 옛 도메인에서 뗀다. 겹치는 구간은
	 * 무해하지만 비는 구간은 위험하다. */
	riscv_iommu_bond_unlink(info->domain, dev);
	info->domain = domain;	/* [한국어] 현재 도메인을 새 도메인으로 기록한다. */

	return 0;	/* [한국어] 붙이기가 끝났다. */
}

/* [한국어] 페이징 도메인의 연산 테이블.
 * map/unmap/iova_to_phys는 generic_pt의 매크로가 채워 넣고,
 * 이 파일은 붙이기와 무효화, 해제만 제공한다. */
static const struct iommu_domain_ops riscv_iommu_paging_domain_ops = {
	IOMMU_PT_DOMAIN_OPS(riscv_64),
	/* [한국어] 페이지 테이블 조작 콜백들을 generic_pt가 채운다 —
	 * 이 파일에 PTE를 만지는 코드가 없는 이유다. */

	.attach_dev = riscv_iommu_attach_paging_domain,
	/* [한국어] bond를 걸고 디바이스 컨텍스트를 갱신한다. */

	.free = riscv_iommu_free_paging_domain,
	/* [한국어] PSCID를 반납하고 테이블을 정리한다. */

	.iotlb_sync = riscv_iommu_iotlb_sync,
	/* [한국어] 해제 후 무효화. 중간 테이블이 사라졌으면 전체를 비운다. */

	.flush_iotlb_all = riscv_iommu_iotlb_flush_all,
	/* [한국어] 전체 무효화. */
};

/*
 * [한국어]
 * riscv_iommu_alloc_paging_domain - 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 디바이스.
 * @return: 새 도메인, 실패하면 ERR_PTR.
 *
 * 하드웨어가 지원하는 **가장 넓은** IOVA 폭을 고르는 것이 첫 일이다.
 * Sv57 → Sv48 → Sv39 순으로 확인해, 지원되는 최대치를 generic_pt에
 * 알려 준다. 그러면 generic_pt가 그에 맞는 단계 수의 테이블을 만든다.
 *
 * 기능 비트 셋도 눈여겨볼 만하다. SIGN_EXTEND는 상위 주소 공간을
 * 부호 확장으로 표현하는 RISC-V의 관례이고, SVNAPOT_64K는
 * 원본 주석이 인용하듯 규격이 필수로 요구하는 연속 매핑 확장이다.
 *
 * 오류 경로에서 free 함수를 재사용하는 점에 유의 — 그 함수가
 * PSCID 미할당을 검사하는 이유가 여기 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [riscv_iommu_alloc_paging_domain]
 *   → pt_iommu_riscv_64_init()
 */
static struct iommu_domain *riscv_iommu_alloc_paging_domain(struct device *dev)
{
	/* [한국어] generic_pt에 넘길 설정. */
	struct pt_iommu_riscv_64_cfg cfg = {};
	/* [한국어] 만들 도메인. */
	struct riscv_iommu_domain *domain;
	/* [한국어] 담당 IOMMU. */
	struct riscv_iommu_device *iommu;
	/* [한국어] 테이블 초기화 결과. */
	int ret;

	/* [한국어] 지원되는 가장 넓은 IOVA 폭을 고른다 — 넓을수록
	 * IOVA 할당이 자유롭다. */
	iommu = dev_to_iommu(dev);
	if (iommu->caps & RISCV_IOMMU_CAPABILITIES_SV57) {	/* [한국어] 5단계를 지원하면 57비트 IOVA를 쓴다. */
		cfg.common.hw_max_vasz_lg2 = 57;	/* [한국어] generic_pt에 그 폭을 알린다. */
	} else if (iommu->caps & RISCV_IOMMU_CAPABILITIES_SV48) {	/* [한국어] 그다음 넓은 것은 48비트다. */
		cfg.common.hw_max_vasz_lg2 = 48;	/* [한국어] 4단계 테이블에 해당한다. */
	} else if (iommu->caps & RISCV_IOMMU_CAPABILITIES_SV39) {	/* [한국어] 최소 구성은 39비트다. */
		cfg.common.hw_max_vasz_lg2 = 39;	/* [한국어] 3단계 테이블에 해당한다. */
	} else {
		/* [한국어] 셋 중 하나도 지원하지 않는 하드웨어는 다룰 수 없다. */
		dev_err(dev, "cannot find supported page table mode\n");
		return ERR_PTR(-ENODEV);	/* [한국어] 쓸 수 있는 페이지 테이블 모드가 없다. */
	}
	/* [한국어] 물리 주소 폭은 규격이 정한 56비트로 고정이다. */
	cfg.common.hw_max_oasz_lg2 = 56;

	/* [한국어] 도메인 뼈대를 0으로 초기화해 받는다. */
	domain = kzalloc_obj(*domain);
	if (!domain)	/* [한국어] 도메인 구조체를 잡지 못했다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] RCU로 읽힐 목록이라 전용 초기화를 쓴다. */
	INIT_LIST_HEAD_RCU(&domain->bonds);
	/* [한국어] 그 목록의 갱신을 보호할 락. */
	spin_lock_init(&domain->lock);
	/*
	 * 6.4 IOMMU capabilities [..] IOMMU implementations must support the
	 * Svnapot standard extension for NAPOT Translation Contiguity.
	 */
	/* [한국어] 테이블에 요구할 기능들. 부호 확장은 RISC-V의 상위
	 * 주소 표현 방식이고, SVNAPOT는 원본 주석이 인용하듯 규격이
	 * 필수로 요구하는 연속 매핑 확장이다(64KB 단위로 TLB 항목을 아낀다). */
	cfg.common.features = BIT(PT_FEAT_SIGN_EXTEND) |
			      BIT(PT_FEAT_FLUSH_RANGE) |
			      BIT(PT_FEAT_RISCV_SVNAPOT_64K);
	/* [한국어] 테이블 페이지를 IOMMU와 같은 NUMA 노드에서 잡게 한다. */
	domain->riscvpt.iommu.nid = dev_to_node(iommu->dev);
	/* [한국어] 코어가 쓸 연산 테이블을 꽂는다. */
	domain->domain.ops = &riscv_iommu_paging_domain_ops;

	/* [한국어] TLB 태그가 될 PSCID를 받는다. 0을 피하는 이유는
	 * "할당되지 않음"과 구별하기 위함이다. */
	domain->pscid = ida_alloc_range(&riscv_iommu_pscids, 1,
					RISCV_IOMMU_MAX_PSCID, GFP_KERNEL);
	if (domain->pscid < 0) {
		/* [한국어] 해제 함수를 재사용한다 — 그 함수가 PSCID
		 * 미할당을 검사하는 이유가 이 경로다. */
		riscv_iommu_free_paging_domain(&domain->domain);
		return ERR_PTR(-ENOMEM);	/* [한국어] PSCID가 동나면 도메인을 만들 수 없다. */
	}

	/* [한국어] 실제 페이지 테이블을 만든다. 이후 모든 map/unmap이
	 * generic_pt를 통해 이 테이블을 조작한다. */
	ret = pt_iommu_riscv_64_init(&domain->riscvpt, &cfg, GFP_KERNEL);
	if (ret) {	/* [한국어] 페이지 테이블 생성이 실패한 경우. */
		riscv_iommu_free_paging_domain(&domain->domain);	/* [한국어] PSCID까지 함께 되돌린다. */
		return ERR_PTR(ret);	/* [한국어] 생성 실패 이유를 전한다. */
	}
	return &domain->domain;	/* [한국어] 코어에는 임베드된 부분만 돌려준다. */
}

/*
 * [한국어]
 * riscv_iommu_attach_blocking_domain - 디바이스의 DMA를 차단한다
 *
 * @iommu_domain: 정적 차단 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인.
 * @return: 항상 0.
 *
 * DC를 **무효**로 만드는 것이 차단의 구현이다. fsc를 BARE로,
 * ta를 0으로 쓰면 유효 비트가 내려가 모든 변환 요청이 폴트
 * (원인 코드 258)가 된다.
 *
 * bond를 만들지 않는 것에 유의 — 하드웨어가 "매핑 없음"을
 * 캐시하지 않으므로 무효화할 대상이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(blocked) → [riscv_iommu_attach_blocking_domain]
 */
static int riscv_iommu_attach_blocking_domain(struct iommu_domain *iommu_domain,
					      struct device *dev,
					      struct iommu_domain *old)
{
	/* [한국어] 담당 IOMMU. */
	struct riscv_iommu_device *iommu = dev_to_iommu(dev);
	/* [한국어] 디바이스의 현재 도메인. */
	struct riscv_iommu_info *info = dev_iommu_priv_get(dev);

	/* Make device context invalid, translation requests will fault w/ #258 */
	/* [한국어] ta가 0이라 유효 비트도 0 — DC 자체가 무효가 되어
	 * 모든 요청이 폴트로 끝난다. */
	riscv_iommu_iodir_update(iommu, dev, RISCV_IOMMU_FSC_BARE, 0);
	/* [한국어] 옛 도메인의 무효화 대상에서 뺀다. */
	riscv_iommu_bond_unlink(info->domain, dev);
	/* [한국어] 차단 도메인은 bond를 만들지 않으므로 NULL이다. */
	info->domain = NULL;

	return 0;	/* [한국어] 차단은 실패하지 않는다. */
}

/* [한국어] 시스템 전체가 공유하는 정적 차단 도메인.
 * 상태가 없어 인스턴스 하나면 충분하다 — DC를 무효로 만드는
 * 동작만 있기 때문이다. */
static struct iommu_domain riscv_iommu_blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	/* [한국어] 코어에 차단 도메인임을 알리는 표시. */

	.ops = &(const struct iommu_domain_ops) {
		.attach_dev = riscv_iommu_attach_blocking_domain,
		/* [한국어] DC를 무효로 만드는 콜백. 익명 구조체로
		 * 인라인 정의한 것은 여기 말고 쓸 곳이 없어서다. */
	}
};

/*
 * [한국어]
 * riscv_iommu_attach_identity_domain - 디바이스를 통과 모드로 만든다
 *
 * @iommu_domain: 정적 통과 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인.
 * @return: 항상 0.
 *
 * 차단과 **딱 한 비트만 다르다**: ta에 유효 비트를 세우는 것이다.
 * fsc가 BARE라 변환은 하지 않지만, DC가 유효하므로 요청이
 * 폴트가 아니라 통과로 처리된다. 이 대비가 규격 설계의 우아한
 * 지점이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) → [riscv_iommu_attach_identity_domain]
 */
static int riscv_iommu_attach_identity_domain(struct iommu_domain *iommu_domain,
					      struct device *dev,
					      struct iommu_domain *old)
{
	/* [한국어] 담당 IOMMU. */
	struct riscv_iommu_device *iommu = dev_to_iommu(dev);
	/* [한국어] 디바이스의 현재 도메인. */
	struct riscv_iommu_info *info = dev_iommu_priv_get(dev);

	/* [한국어] 차단과 같은 BARE 모드이되 유효 비트를 세운다 —
	 * 그 한 비트가 "폴트"와 "통과"를 가른다. */
	riscv_iommu_iodir_update(iommu, dev, RISCV_IOMMU_FSC_BARE, RISCV_IOMMU_PC_TA_V);
	/* [한국어] 옛 도메인의 무효화 대상에서 뺀다. */
	riscv_iommu_bond_unlink(info->domain, dev);
	/* [한국어] 통과 도메인도 bond를 만들지 않는다 — 하드웨어가
	 * 통과 변환을 캐시하지 않기 때문이다. */
	info->domain = NULL;

	return 0;	/* [한국어] 통과 전환은 실패하지 않는다. */
}

/* [한국어] 시스템 전체가 공유하는 정적 통과 도메인. */
static struct iommu_domain riscv_iommu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 코어에 통과 모드임을 알리는 표시. */

	.ops = &(const struct iommu_domain_ops) {
		.attach_dev = riscv_iommu_attach_identity_domain,
		/* [한국어] DC를 유효한 BARE로 만드는 콜백. */
	}
};

/*
 * [한국어]
 * riscv_iommu_device_group - 이 디바이스가 속할 격리 단위를 정한다
 *
 * @dev: 대상 디바이스.
 * @return: 그룹.
 *
 * PCI만 전용 판별이 필요하다 — ACS 능력과 브리지 구조에 따라
 * 여러 함수가 서로 격리되지 않을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후.
 *
 * 호출 체인:
 *   IOMMU 코어 device_group → [riscv_iommu_device_group]
 */
static struct iommu_group *riscv_iommu_device_group(struct device *dev)
{
	/* [한국어] PCI는 토폴로지를 따져야 한다. */
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	/* [한국어] 그 밖에는 디바이스마다 독립 그룹이면 충분하다. */
	return generic_device_group(dev);
}

/*
 * [한국어]
 * riscv_iommu_of_xlate - 디바이스 트리의 iommus 인자를 디바이스 ID로 등록한다
 *
 * @dev: 대상 디바이스.
 * @args: 파싱된 인자.
 * @return: 0 성공, 음수 오류.
 *
 * 인자 하나가 곧 디바이스 ID이며, 그것이 DDT의 인덱스가 된다.
 *
 * 실행 컨텍스트: 디바이스 트리 파싱.
 *
 * 호출 체인:
 *   IOMMU 코어 of_xlate → [riscv_iommu_of_xlate]
 */
static int riscv_iommu_of_xlate(struct device *dev, const struct of_phandle_args *args)
{
	/* [한국어] 인자 하나를 디바이스 ID로 등록한다. */
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

/*
 * [한국어]
 * riscv_iommu_probe_device - 디바이스를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 iommu_device, 담당하지 않으면 ERR_PTR.
 *
 * DDT에 이 디바이스의 자리를 **미리 만들어 두되 무효로 남기는**
 * 것이 이 함수의 핵심이다. 중간 단계 테이블 할당은 잠들 수 있는
 * 작업이라, 나중의 attach 경로에서 하기보다 여기서 끝내 두는 것이
 * 낫다. 무효한 DC는 하드웨어가 캐시하지 않으므로 만들어 두어도
 * 안전하다.
 *
 * ddt_mode가 BARE 이하면 담당하지 않는다고 답한다 — 원본 주석이
 * 밝히듯 그 상태의 IOMMU는 어차피 모든 디바이스를 통과시키므로
 * 코어가 관여할 여지가 없다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [riscv_iommu_probe_device]
 *   → riscv_iommu_get_dc()
 */
static struct iommu_device *riscv_iommu_probe_device(struct device *dev)
{
	/* [한국어] 이 디바이스의 ID들. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	/* [한국어] 담당 IOMMU. */
	struct riscv_iommu_device *iommu;
	/* [한국어] 만들 디바이스 상태. */
	struct riscv_iommu_info *info;
	/* [한국어] 미리 만들어 둘 디바이스 컨텍스트. */
	struct riscv_iommu_dc *dc;
	/* [한국어] DC에 쓸 초기 변환 제어 값(무효). */
	u64 tc;
	/* [한국어] ID 순회 인덱스. */
	int i;

	/* [한국어] ID가 없거나 IOMMU 노드가 연결되지 않았으면
	 * 우리 담당이 아니다. */
	if (!fwspec || !fwspec->iommu_fwnode->dev || !fwspec->num_ids)
		return ERR_PTR(-ENODEV);

	/* [한국어] fwnode의 디바이스에서 드라이버 상태를 얻는다. */
	iommu = dev_get_drvdata(fwspec->iommu_fwnode->dev);
	if (!iommu)	/* [한국어] 그 fwnode에 드라이버 상태가 없다. */
		return ERR_PTR(-ENODEV);

	/*
	 * IOMMU hardware operating in fail-over BARE mode will provide
	 * identity translation for all connected devices anyway...
	 */
	/* [한국어] 디렉터리를 쓰지 않는 모드라면 모든 디바이스가 이미
	 * 통과 상태다 — 코어가 관여할 여지가 없다. */
	if (iommu->ddt_mode <= RISCV_IOMMU_DDTP_IOMMU_MODE_BARE)
		return ERR_PTR(-ENODEV);

	/* [한국어] 디바이스 상태를 잡는다. domain은 NULL로 시작한다. */
	info = kzalloc_obj(*info);
	if (!info)	/* [한국어] 디바이스 상태를 잡지 못했다. */
		return ERR_PTR(-ENOMEM);
	/*
	 * Allocate and pre-configure device context entries in
	 * the device directory. Do not mark the context valid yet.
	 */
	/* [한국어] 유효 비트가 없는 값 — 이 DC는 아직 쓰이지 않는다. */
	tc = 0;
	for (i = 0; i < fwspec->num_ids; i++) {
		/* [한국어] 중간 테이블까지 만들어 자리를 확보한다.
		 * 잠들 수 있는 할당이라 attach 경로가 아닌 여기서 한다. */
		dc = riscv_iommu_get_dc(iommu, fwspec->ids[i]);
		if (!dc) {	/* [한국어] 디렉터리에 자리를 만들지 못한 경우. */
			kfree(info);	/* [한국어] 잡아 둔 상태를 되돌린다. */
			return ERR_PTR(-ENODEV);	/* [한국어] 이 디바이스를 담당할 수 없다. */
		}
		/* [한국어] 이미 유효한 DC가 있다면 앞 커널이나 펌웨어가
		 * 남긴 것이다 — 덮어쓰되 알려 둔다. */
		if (READ_ONCE(dc->tc) & RISCV_IOMMU_DC_TC_V)
			dev_warn(dev, "already attached to IOMMU device directory\n");
		/* [한국어] 무효 상태로 초기화한다. 무효한 DC는 캐시되지
		 * 않으므로 무효화 명령이 필요 없다. */
		WRITE_ONCE(dc->tc, tc);
	}

	/* [한국어] 이후 attach 콜백들이 이 상태를 꺼내 쓴다. */
	dev_iommu_priv_set(dev, info);

	return &iommu->iommu;	/* [한국어] 담당 IOMMU 인스턴스를 코어에 알린다. */
}

/*
 * [한국어]
 * riscv_iommu_release_device - 디바이스 상태를 해제한다
 *
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * kfree_rcu_mightsleep을 쓰는 이유: bond 순회가 RCU 읽기 구역에서
 * 이 디바이스를 참조하고 있을 수 있어, 유예 기간이 지난 뒤에
 * 해제해야 한다.
 *
 * DDT 항목을 지우지 않는 점에 유의 — 코어가 release 전에 차단
 * 도메인으로 옮겨 이미 무효로 만들어 두기 때문이다(release_domain).
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 release_device → [riscv_iommu_release_device]
 */
static void riscv_iommu_release_device(struct device *dev)
{
	/* [한국어] 해제할 디바이스 상태. */
	struct riscv_iommu_info *info = dev_iommu_priv_get(dev);

	/* [한국어] bond 순회가 이 디바이스를 보고 있을 수 있어
	 * RCU 유예 뒤에 해제한다. */
	kfree_rcu_mightsleep(info);
}

/* [한국어] 이 드라이버가 IOMMU 코어에 제공하는 연산 테이블.
 * map/unmap이 없는 것은 도메인별 ops(generic_pt)에 있기 때문이다. */
static const struct iommu_ops riscv_iommu_ops = {
	.of_xlate = riscv_iommu_of_xlate,
	/* [한국어] 디바이스 트리의 인자를 디바이스 ID로 등록한다. */

	.identity_domain = &riscv_iommu_identity_domain,
	/* [한국어] 통과 모드 — DC를 유효한 BARE로 만든다. */

	.blocked_domain = &riscv_iommu_blocking_domain,
	/* [한국어] 차단 — DC를 무효로 만든다. */

	.release_domain = &riscv_iommu_blocking_domain,
	/* [한국어] 디바이스가 떠날 때 되돌아갈 도메인. 차단과 같은
	 * 것을 써, release_device 시점에는 DC가 이미 무효다. */

	.domain_alloc_paging = riscv_iommu_alloc_paging_domain,
	/* [한국어] 페이징 도메인 생성. PSCID와 페이지 테이블을 잡는다. */

	.device_group = riscv_iommu_device_group,
	/* [한국어] 격리 단위 결정. */

	.probe_device = riscv_iommu_probe_device,
	/* [한국어] DDT 자리를 미리 만들어 둔다. */

	.release_device	= riscv_iommu_release_device,
	/* [한국어] 디바이스 상태를 RCU 유예 뒤 해제한다. */
};

/*
 * [한국어]
 * riscv_iommu_init_check - 초기화 전 하드웨어 상태를 확인하고 맞춘다
 *
 * @iommu: 대상 IOMMU.
 * @return: 0 성공, -EBUSY/-EINVAL.
 *
 * 세 가지를 확인한다.
 *
 * (1) **이미 켜져 있는 IOMMU는 거부한다.** 예외는 크래시 덤프
 *     커널인데, 그때는 앞 커널이 켜 둔 것이므로 강제로 끄고 진행한다.
 *     정상 부팅에서 켜져 있다면 상태를 알 수 없어 위험하다.
 *
 * (2) **바이트 순서를 CPU에 맞춘다.** 하드웨어가 메모리의 자료
 *     구조를 읽는 순서가 커널과 달라선 안 된다. 다르면 FCTL의
 *     BE 비트를 뒤집어 보고, 하드웨어가 그 전환을 지원하지 않으면
 *     포기한다.
 *
 * (3) **인터럽트 벡터를 배분한다.** 큐마다 벡터를 하나씩 주되,
 *     인터럽트가 부족하면 나머지 연산으로 돌려 쓴다. 쓰고 되읽어
 *     하드웨어가 받아들인 배분을 확인하는 것도 WARL 관례다.
 *
 * 실행 컨텍스트: 초기화. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv_iommu_init() → [riscv_iommu_init_check]
 */
static int riscv_iommu_init_check(struct riscv_iommu_device *iommu)
{
	/* [한국어] 읽은 DDTP 값. */
	u64 ddtp;

	/*
	 * Make sure the IOMMU is switched off or in pass-through mode during
	 * regular boot flow and disable translation when we boot into a kexec
	 * kernel and the previous kernel left them enabled.
	 */
	/* [한국어] 여기서는 타임아웃 없이 즉시 읽는다 — 아직 하드웨어를
	 * 신뢰할 수 없는 단계이기 때문이다. */
	ddtp = riscv_iommu_readq(iommu, RISCV_IOMMU_REG_DDTP);
	if (ddtp & RISCV_IOMMU_DDTP_BUSY)	/* [한국어] 하드웨어가 바쁘면 상태를 판단할 수 없다. */
		return -EBUSY;

	/* [한국어] 이미 변환이 켜져 있다. */
	if (FIELD_GET(RISCV_IOMMU_DDTP_IOMMU_MODE, ddtp) >
	     RISCV_IOMMU_DDTP_IOMMU_MODE_BARE) {
		/* [한국어] 정상 부팅에서 이렇다면 누가 켰는지 알 수 없어
		 * 위험하다 — 손대지 않고 물러난다. */
		if (!is_kdump_kernel())
			return -EBUSY;
		/* [한국어] 크래시 덤프 커널이라면 앞 커널이 켜 둔 것이니
		 * 강제로 끄고 진행한다. */
		riscv_iommu_disable(iommu);
	}

	/* Configure accesses to in-memory data structures for CPU-native byte order. */
	/* [한국어] 하드웨어의 바이트 순서가 커널과 다르면, 메모리의
	 * 테이블과 큐를 서로 다르게 해석하게 된다. */
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN) !=
	    !!(iommu->fctl & RISCV_IOMMU_FCTL_BE)) {
		/* [한국어] 순서 전환을 지원하지 않는 하드웨어라면
		 * 맞출 방법이 없다. */
		if (!(iommu->caps & RISCV_IOMMU_CAPABILITIES_END))
			return -EINVAL;
		/* [한국어] BE 비트를 뒤집어 쓴다. */
		riscv_iommu_writel(iommu, RISCV_IOMMU_REG_FCTL,
				   iommu->fctl ^ RISCV_IOMMU_FCTL_BE);
		/* [한국어] 되읽어 실제로 바뀌었는지 확인한다. */
		iommu->fctl = riscv_iommu_readl(iommu, RISCV_IOMMU_REG_FCTL);
		if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN) !=	/* [한국어] 뒤집은 값이 실제로 반영됐는지 확인한다. */
		    !!(iommu->fctl & RISCV_IOMMU_FCTL_BE))
			return -EINVAL;
	}

	/*
	 * Distribute interrupt vectors, always use first vector for CIV.
	 * At least one interrupt is required. Read back and verify.
	 */
	/* [한국어] 인터럽트가 하나도 없으면 큐를 쓸 수 없다 —
	 * 이 드라이버에는 폴링 경로가 없다. */
	if (!iommu->irqs_count)
		return -EINVAL;

	/* [한국어] 큐마다 벡터를 하나씩 배분한다. 나머지 연산을 쓰는
	 * 이유: 인터럽트가 부족하면 앞 벡터를 돌려 쓴다. 명령 큐는
	 * 항상 0번을 쓰므로 필드가 없다. */
	iommu->icvec = FIELD_PREP(RISCV_IOMMU_ICVEC_FIV, 1 % iommu->irqs_count) |
		       FIELD_PREP(RISCV_IOMMU_ICVEC_PIV, 2 % iommu->irqs_count) |
		       FIELD_PREP(RISCV_IOMMU_ICVEC_PMIV, 3 % iommu->irqs_count);
	riscv_iommu_writeq(iommu, RISCV_IOMMU_REG_ICVEC, iommu->icvec);
	/* [한국어] 하드웨어가 받아들인 배분을 되읽는다 — 이 필드도
	 * WARL이라 값이 바뀔 수 있다. */
	iommu->icvec = riscv_iommu_readq(iommu, RISCV_IOMMU_REG_ICVEC);
	/* [한국어] 어느 벡터든 실제 인터럽트 수를 넘으면 배열 밖을
	 * 참조하게 된다 — 최대값 하나로 그것을 검사한다. */
	if (max3(FIELD_GET(RISCV_IOMMU_ICVEC_CIV, iommu->icvec),
		 FIELD_GET(RISCV_IOMMU_ICVEC_FIV, iommu->icvec),
		 max(FIELD_GET(RISCV_IOMMU_ICVEC_PIV, iommu->icvec),
		     FIELD_GET(RISCV_IOMMU_ICVEC_PMIV, iommu->icvec))) >= iommu->irqs_count)
		return -EINVAL;

	return 0;	/* [한국어] 하드웨어가 쓸 수 있는 상태임이 확인됐다. */
}

/*
 * [한국어]
 * riscv_iommu_remove - IOMMU를 걷어낸다
 *
 * @iommu: 대상 IOMMU.
 * @return: 없음.
 *
 * 초기화의 정확한 역순이다. 코어에서 먼저 빼야 새 요청이 오지
 * 않고, 디렉터리를 끈 뒤에 큐를 닫아야 마지막 무효화 명령이
 * 처리될 수 있다.
 *
 * 실행 컨텍스트: 드라이버 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼/ACPI 제거 경로 → [riscv_iommu_remove]
 */
void riscv_iommu_remove(struct riscv_iommu_device *iommu)
{
	/* [한국어] 코어에서 빼 새 콜백이 오지 않게 한다. */
	iommu_device_unregister(&iommu->iommu);
	/* [한국어] sysfs 항목을 지운다. */
	iommu_device_sysfs_remove(&iommu->iommu);
	/* [한국어] 디렉터리를 끈다. 이 안에서 마지막 무효화 명령이
	 * 나가므로 큐가 아직 살아 있어야 한다. */
	riscv_iommu_iodir_set_mode(iommu, RISCV_IOMMU_DDTP_IOMMU_MODE_OFF);
	/* [한국어] 그다음에 큐를 닫는다. */
	riscv_iommu_queue_disable(&iommu->cmdq);
	riscv_iommu_queue_disable(&iommu->fltq);	/* [한국어] 폴트 큐도 닫는다. */
}

/*
 * [한국어]
 * riscv_iommu_init - IOMMU 하드웨어를 초기화하고 코어에 등록한다
 *
 * @iommu: 초기화할 IOMMU(레지스터와 인터럽트는 이미 준비된 상태).
 * @return: 0 성공, 음수 오류.
 *
 * 플랫폼별 진입점(디바이스 트리 또는 ACPI)이 하드웨어 자원을
 * 준비한 뒤 부르는 공통 초기화다. 순서에 이유가 있다.
 *
 *  1) 큐 서술자를 채우고 하드웨어 상태를 확인한다.
 *  2) 디렉터리 뿌리를 마련한다.
 *  3) 두 큐의 버퍼를 잡는다.
 *  4) 큐를 켠다. **디렉터리 모드를 설정하기 전에** 켜야 하는데,
 *     그 설정이 무효화 명령을 보내기 때문이다.
 *  5) 디렉터리 모드를 최대치로 시도해 확정한다.
 *  6) sysfs와 (ACPI라면) RIMT에 등록한 뒤 코어에 등록한다.
 *
 * 오류 경로가 정확히 역순의 사다리를 이룬다.
 *
 * 실행 컨텍스트: 플랫폼 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   riscv-iommu-platform.c / -pci.c → [riscv_iommu_init]
 *   → iommu_device_register()
 */
int riscv_iommu_init(struct riscv_iommu_device *iommu)
{
	/* [한국어] 단계별 결과. */
	int rc;

	/* [한국어] 명령 큐의 레지스터 오프셋과 기본 크기를 채운다. */
	RISCV_IOMMU_QUEUE_INIT(&iommu->cmdq, CQ);
	/* [한국어] 폴트 큐도 마찬가지로 채운다. */
	RISCV_IOMMU_QUEUE_INIT(&iommu->fltq, FQ);

	/* [한국어] 하드웨어가 우리가 다룰 수 있는 상태인지 확인하고,
	 * 바이트 순서와 인터럽트 배분을 맞춘다. */
	rc = riscv_iommu_init_check(iommu);
	if (rc)	/* [한국어] 큐 서술자와 하드웨어 상태 확인이 실패했다. */
		return dev_err_probe(iommu->dev, rc, "unexpected device state\n");

	/* [한국어] 디바이스 디렉터리의 뿌리를 마련한다. */
	rc = riscv_iommu_iodir_alloc(iommu);
	if (rc)	/* [한국어] 디렉터리 뿌리를 마련하지 못했다. */
		return rc;

	/* [한국어] 명령 큐의 링 버퍼를 잡고 하드웨어에 알린다. */
	rc = riscv_iommu_queue_alloc(iommu, &iommu->cmdq,
				     sizeof(struct riscv_iommu_command));
	if (rc)	/* [한국어] 명령 큐 버퍼를 마련하지 못했다. */
		return rc;

	/* [한국어] 폴트 큐도 마찬가지로 마련한다. 항목 크기가 다르다. */
	rc = riscv_iommu_queue_alloc(iommu, &iommu->fltq,
				     sizeof(struct riscv_iommu_fq_record));
	if (rc)	/* [한국어] 폴트 큐 버퍼를 마련하지 못했다. */
		return rc;

	/* [한국어] 명령 큐를 켠다. 아래의 디렉터리 설정이 무효화
	 * 명령을 보내므로 그보다 먼저여야 한다. */
	rc = riscv_iommu_queue_enable(iommu, &iommu->cmdq, riscv_iommu_cmdq_process);
	if (rc)	/* [한국어] 명령 큐를 켜지 못했다 — 아직 되돌릴 것이 없다. */
		return rc;

	/* [한국어] 폴트 큐를 켠다. 이제 하드웨어가 오류를 보고할 수 있다. */
	rc = riscv_iommu_queue_enable(iommu, &iommu->fltq, riscv_iommu_fltq_process);
	if (rc)	/* [한국어] 폴트 큐를 켜지 못했다. */
		goto err_queue_disable;

	/* [한국어] 지원되는 최대 단계 수로 디렉터리를 켠다.
	 * 안 되면 함수가 알아서 낮춰 가며 시도한다. */
	rc = riscv_iommu_iodir_set_mode(iommu, RISCV_IOMMU_DDTP_IOMMU_MODE_MAX);
	if (rc)	/* [한국어] 디렉터리를 어떤 단계로도 켜지 못했다. */
		goto err_queue_disable;

	/* [한국어] sysfs에 IOMMU 인스턴스를 노출한다. */
	rc = iommu_device_sysfs_add(&iommu->iommu, NULL, NULL, "riscv-iommu@%s",
				    dev_name(iommu->dev));
	if (rc) {	/* [한국어] sysfs 등록이 실패한 경우. */
		dev_err_probe(iommu->dev, rc, "cannot register sysfs interface\n");	/* [한국어] 어느 단계에서 막혔는지 남긴다. */
		goto err_iodir_off;	/* [한국어] 켜 둔 디렉터리를 되돌리러 간다. */
	}

	/* [한국어] ACPI 환경이라면 RIMT 표에도 등록해야 디바이스와
	 * 연결이 성립한다. */
	if (!acpi_disabled) {
		rc = rimt_iommu_register(iommu->dev);	/* [한국어] ACPI 환경에서 IOMMU를 RIMT에 등록한다. */
		if (rc) {	/* [한국어] RIMT 등록이 실패한 경우. */
			dev_err_probe(iommu->dev, rc, "cannot register iommu with RIMT\n");	/* [한국어] 디바이스와 연결될 수 없음을 알린다. */
			goto err_remove_sysfs;	/* [한국어] sysfs 등록을 되돌리러 간다. */
		}
	}

	/* [한국어] 코어에 등록한다. 이 순간부터 디바이스들의
	 * probe_device가 불리기 시작한다. */
	rc = iommu_device_register(&iommu->iommu, &riscv_iommu_ops, iommu->dev);
	if (rc) {	/* [한국어] 코어 등록이 실패한 경우. */
		dev_err_probe(iommu->dev, rc, "cannot register iommu interface\n");	/* [한국어] 실패를 알린다. */
		goto err_remove_sysfs;	/* [한국어] sysfs 등록을 되돌리러 간다. */
	}

	return 0;

/* [한국어] 코어 등록 실패 — sysfs부터 되돌린다. */
err_remove_sysfs:
	iommu_device_sysfs_remove(&iommu->iommu);
/* [한국어] 디렉터리를 켠 뒤의 실패 — 다시 끈다. */
err_iodir_off:
	riscv_iommu_iodir_set_mode(iommu, RISCV_IOMMU_DDTP_IOMMU_MODE_OFF);
/* [한국어] 큐를 켠 뒤의 실패 — 켠 역순으로 닫는다. */
err_queue_disable:
	riscv_iommu_queue_disable(&iommu->fltq);	/* [한국어] 폴트 큐를 닫는다. */
	riscv_iommu_queue_disable(&iommu->cmdq);	/* [한국어] 명령 큐를 닫는다. */
	return rc;	/* [한국어] 실패 이유를 플랫폼 코드에 전한다. */
}

/* [한국어] generic_pt가 내보내는 심볼 네임스페이스를 들여온다.
 * IOMMU_PT_DOMAIN_OPS가 참조하는 함수들이 그 네임스페이스에 있어,
 * 이 선언이 없으면 링크 시점에 거부된다. */
MODULE_IMPORT_NS("GENERIC_PT_IOMMU");
