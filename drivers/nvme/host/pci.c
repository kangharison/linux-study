// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver
 * Copyright (c) 2011-2014, Intel Corporation.
 */

/*
 * [한국어 설명] NVMe host PCIe 트랜스포트 드라이버 (drivers/nvme/host/pci.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVM Express 스펙의 **PCIe 메모리 맵 트랜스포트** 구현체다.
 * 컨트롤러 MMIO(BAR0 레지스터 창), Admin/I/O Submission·Completion Queue 쌍,
 * 도어벨 링, MSI/MSI-X 완료 인터럽트, PRP/SGL DMA 매핑(iod), 타임아웃·Abort·
 * 컨트롤러 리셋, FLR/PCI AER 복구, CMB(Controller Memory Buffer)/HMB(Host
 * Memory Buffer)/P2PDMA, probe·remove·런타임/시스템 PM 을 한 모듈에서 담당한다.
 * 블록 계층(blk-mq) 입장에서는 "nvme" 디스크의 실제 하드웨어 백엔드이며,
 * core.c 가 소유하는 공통 nvme_ctrl 상태기계에 PCIe 전용 ops 를 꽂아 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   사용자/FS → bio → blk-mq → nvme_queue_rq / nvme_queue_rqs
 *        → nvme_prep_rq (cmd 구성 + PRP/SGL map)
 *        → SQ 메모리 복사 + SQ doorbell MMIO(또는 shadow dbbuf)
 *        → 컨트롤러 실행 → CQ 에 CQE 기록 + MSI-X
 *        → nvme_irq / nvme_poll → nvme_handle_cqe → unmap + complete
 * 초기화 경로:
 *   pci_probe → BAR map → CAP 읽기 → Admin Q(AQA/ASQ/ACQ) + CC.EN
 *        → Identify/features(core) → HMB/CMB → I/O Q Create + irq affinity
 *        → LIVE + ns scan
 * 실패·복구:
 *   timeout/CFS/NSSRO/AER frozen → nvme_dev_disable → reset_work → 재 enable
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/core.c : nvme_init_ctrl, enable/disable_ctrl, tagset,
 *   ns scan, 타임아웃 공통 정책. 이 파일은 reg_read/write, queue_rq, timeout
 *   훅, free_ctrl 등 PCIe 특수부를 제공한다.
 * - drivers/nvme/host/nvme.h : struct nvme_ctrl / nvme_request / quirks.
 * - include/linux/nvme.h : CAP/CC/CSTS, SQE/CQE, admin opcode, CMB/HMB 레지스터.
 * - block/blk-mq*.c : tagset ops(queue_rq, map_queues, poll, timeout).
 * - drivers/pci/⋆ : BAR, MSI-X, FLR, AER err_handler, p2pdma, PM.
 * - dma-mapping / dma-iova : PRP list·SGL·iod 페이로드 매핑.
 *
 * === 주요 자료구조 ===
 * - nvme_dev : PCI 함수 단위 상태. BAR, dbs, queues[], CMB/HMB, tagset, dbbuf.
 * - nvme_queue : SQ/CQ 링, phase bit, doorbell, irq vector, CMB SQ 여부.
 * - nvme_iod : request PDU. 커맨드 템플릿 + descriptor 풀 포인터 + DMA 상태.
 *
 * === 핫패스 vs 컨트롤 플레인 ===
 * 핫패스: prep_rq → map_data/metadata → sq_copy → write_sq_db → irq → poll_cq
 * 컨트롤: probe/reset_work, setup_io_queues, create/delete queue, HMB, suspend
 *
 * === BAR0 MMIO 창 레이아웃 (요지) ===
 * 오프셋 0x00 CAP(64), 0x08 VS, 0x0C INTMS/INTMC, 0x14 CC, 0x1C CSTS,
 * 0x20 NSSR, 0x24 AQA, 0x28 ASQ, 0x30 ACQ, 0x38 CMBLOC, 0x3C CMBSZ,
 * 0x50 CMBMSC, 0x1000+ 도어벨 창(NVME_REG_DBS). CAP.DSTRD 가 도어벨 stride.
 * 호스트는 ioremap 으로 이 창을 잡고 readl/writel·lo_hi_* 로 접근한다.
 *
 * === SQ/CQ 와 도어벨 ===
 * 큐마다 Submission Queue(호스트 생산) + Completion Queue(컨트롤러 생산) 쌍.
 * SQ tail doorbell 로 새 작업 알림, CQ head doorbell 로 슬롯 재사용 허가.
 * phase 비트로 링 full/empty 모호성을 해소한다. OACS.DBBUF 면 shadow
 * doorbell 로 MMIO 트래픽을 줄일 수 있다.
 *
 * === PRP vs SGL ===
 * PRP: 페이지 리스트. PRP1/PRP2 또는 PRP2=리스트 DMA. 정렬·갭에 약함.
 * SGL: 명시 길이 디스크립터. 비정렬·usercmd·P2P 에 유리. sgl_threshold 로
 * 평균 세그먼트 크기 기반 선택. Admin 큐는 관례상 PRP.
 *
 * === MSI-X 와 폴링 ===
 * 벡터0=admin, 이후 I/O. write_queues/poll_queues 로 DEFAULT/READ/POLL 맵 분할.
 * 폴링 큐는 인터럽트 없이 blk-mq poll 이 CQ phase 를 본다.
 *
 * === 리셋 사다리 ===
 * 요청 타임아웃 → (옵션) CQ 강제 폴 → Abort 1회 → 컨트롤러 disable+reset_work.
 * CFS/NSSRO/핫제거/AER frozen 은 즉시 가혹 경로. FLR 은 CC 리셋 실패 시 사용.
 *
 * === CMB / HMB ===
 * CMB: 장치 로컬 버퍼에 I/O SQ 배치 가능(use_cmb_sqes) — host→dev DMA 절감.
 * HMB: 호스트 DRAM 을 컨트롤러 캐시로 제공(Set Features). optional, 실패해도 I/O 가능.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_probe / nvme_remove: PCI 장치 바인딩의 양 끝. BAR 매핑, DMA 마스크 설정,
 *   admin 큐 구성, nvme_ctrl 등록까지가 probe 의 몫이다.
 * - nvme_reset_work: 리셋 상태 기계의 본체. 컨트롤러를 끄고 다시 켠 뒤 admin 큐를
 *   세우고, I/O 큐를 재생성하고, 네임스페이스를 다시 스캔한다. 오류 복구가
 *   결국 이 함수로 수렴한다.
 * - nvme_queue_rq / nvme_queue_rqs: 핫패스 제출. 후자는 blk-mq 가 모아 준 요청
 *   묶음을 한 번에 처리해 도어벨 쓰기를 줄인다.
 * - nvme_map_data / nvme_pci_setup_prps / nvme_pci_setup_sgls: 데이터 전송 서술자
 *   구성. 세그먼트 모양과 컨트롤러 지원에 따라 PRP 와 SGL 중 하나를 고른다.
 * - nvme_irq / nvme_poll / nvme_process_cq: 완료 수확. phase 비트로 새 CQE 를
 *   구분하고, poll 큐는 인터럽트 없이 이 경로만 쓴다.
 * - nvme_timeout: 응답 없는 명령 처리. Abort 를 먼저 시도하고, 그것마저 실패하면
 *   컨트롤러 리셋으로 올라간다.
 * - nvme_setup_io_queues / nvme_create_queue / nvme_delete_queue: I/O 큐 개수 협상과
 *   개별 큐의 생성·삭제. MSI-X 벡터 수가 큐 개수의 상한을 만든다.
 * - nvme_map_cmb: Controller Memory Buffer 매핑. P2PDMA 로 호스트 메모리를 우회하는
 *   경로의 출발점이다.
 * - nvme_setup_host_mem: Host Memory Buffer 설정. 컨트롤러가 호스트 DRAM 일부를
 *   자기 캐시로 빌려 쓰게 한다.
 * - struct nvme_dev: PCI 컨트롤러 전역 상태. nvme_ctrl 을 품고 BAR, 큐 배열,
 *   descriptor 풀, CMB/HMB 상태를 갖는다.
 * - struct nvme_queue: SQ/CQ 한 쌍. DMA 링, 도어벨 주소, phase, 벡터 번호.
 * - struct nvme_iod: 요청별 매핑 상태. PRP/SGL 서술자와 sg 테이블을 추적한다.
 */

#include <linux/acpi.h>		/* [한국어] ACPI storage D3 / 플랫폼 suspend quirk 조회 */
#include <linux/async.h>	/* [한국어] 비동기 probe 와 연관된 커널 async 헬퍼 */
#include <linux/blkdev.h>	/* [한국어] block_device/gendisk 및 블록 공통 타입 */
#include <linux/blk-mq-dma.h>	/* [한국어] blk_rq_dma_map_iter — PRP/SGL 매핑 iterator */
#include <linux/blk-integrity.h> /* [한국어] T10 PI / integrity bio 세그먼트 */
#include <linux/dmi.h>		/* [한국어] 보드·제품 DMI 매칭 기반 플랫폼 quirk */
#include <linux/init.h>		/* [한국어] __init/__exit 섹션 매크로 */
#include <linux/interrupt.h>	/* [한국어] IRQ 핸들러 등록·반환 코드 */
#include <linux/io.h>		/* [한국어] ioremap/readl/writel MMIO 접근 */
#include <linux/kstrtox.h>	/* [한국어] 모듈 파라미터 정수 파싱 */
#include <linux/memremap.h>	/* [한국어] CMB 정렬 memremap_compat_align */
#include <linux/mm.h>		/* [한국어] 페이지/오더 상수 (HMB chunk 크기) */
#include <linux/module.h>	/* [한국어] module_param, MODULE_* 메타데이터 */
#include <linux/mutex.h>	/* [한국어] shutdown_lock 등 슬립 가능 직렬화 */
#include <linux/nodemask.h>	/* [한국어] nr_node_ids — descriptor pool 배열 크기 */
#include <linux/once.h>		/* [한국어] dev_err_once 등 once 헬퍼 */
#include <linux/pci.h>		/* [한국어] pci_driver, MSI-X, FLR, resource, AER */
#include <linux/suspend.h>	/* [한국어] pm_suspend_via_firmware — suspend 전략 */
#include <linux/t10-pi.h>	/* [한국어] 보호 정보(PI) 관련 정의 */
#include <linux/types.h>	/* [한국어] 커널 기본 타입 */
#include <linux/io-64-nonatomic-lo-hi.h> /* [한국어] lo_hi_readq/writeq — CAP·ASQ 등 */
#include <linux/io-64-nonatomic-hi-lo.h> /* [한국어] hi_lo_writeq — CMBMSC 등 */
#include <linux/sed-opal.h>	/* [한국어] OPAL SED (core 경유 연결) */

#include "trace.h"		/* [한국어] trace_nvme_sq 등 추적점 */
#include "nvme.h"		/* [한국어] host 내부 nvme_ctrl/request/quirks API */

#define SQ_SIZE(q)	((q)->q_depth << (q)->sqes)	/* [한국어] SQ 링 바이트 = depth << sqes(로그2 엔트리) */
#define CQ_SIZE(q)	((q)->q_depth * sizeof(struct nvme_completion))	/* [한국어] CQ 링 바이트 = depth * CQE 크기 */

/* Optimisation for I/Os between 4k and 128k */
/* [한국어] 짧은 I/O 용 small dma_pool 크기 — 4K~128K 구간 PRP/SGL 리스트 최적화 */
#define NVME_SMALL_POOL_SIZE	256	/* [한국어] 위 영어 주석대로 4k~128k I/O 를 겨냥한다 — 그 범위의 PRP 리스트는 256바이트에 들어가 페이지 하나를 낭비하지 않는다 */

/*
 * Arbitrary upper bound.
 */
/* [한국어] 단일 요청 페이로드·디스크립터 개수 드라이버 상한 (스펙 최대보다 보수적) */
#define NVME_MAX_BYTES		SZ_8M	/* [한국어] 요청 최대 8MiB — iod/sg 한 페이지 할당 폭주 방지 */
#define NVME_MAX_NR_DESCRIPTORS	5	/* [한국어] iod 가 동시에 붙드는 descriptor 페이지 수 상한 */

/*
 * For data SGLs we support a single descriptors worth of SGL entries.
 * For PRPs, segments don't matter at all.
 */
/* [한국어] 데이터 SGL: 디스크립터 페이지 하나에 들어가는 엔트리 수가 세그먼트 상한.
 * PRP 는 링크 리스트로 확장되므로 이 상한이 직접 제약이 되지 않는다. */
#define NVME_MAX_SEGS \
	(NVME_CTRL_PAGE_SIZE / sizeof(struct nvme_sgl_desc))	/* [한국어] SGL 서술자 기준으로 정한다 — 페이지 하나에 들어가는 서술자 개수가 세그먼트 상한이 된다 */

/*
 * For metadata SGLs, only the small descriptor is supported, and the first
 * entry is the segment descriptor, which for the data pointer sits in the SQE.
 */
/* [한국어] 메타 SGL 은 small pool 만 사용. 첫 엔트리는 last-segment 디스크립터
 * (데이터 경로의 SQE dptr 역할)이므로 사용 가능 데이터 칸은 -1. */
#define NVME_MAX_META_SEGS \
	((NVME_SMALL_POOL_SIZE / sizeof(struct nvme_sgl_desc)) - 1)

/*
 * The last entry is used to link to the next descriptor.
 */
/* [한국어] PRP 리스트 페이지 마지막 슬롯은 다음 리스트 DMA 주소(링크) 전용 */
#define PRPS_PER_PAGE \
	(((NVME_CTRL_PAGE_SIZE / sizeof(__le64))) - 1)

/*
 * I/O could be non-aligned both at the beginning and end.
 */
/* [한국어] 선두·말미 비정렬 여유(각 최대 페이지-1)를 포함한 최악 PRP 바이트 span */
#define MAX_PRP_RANGE \
	(NVME_MAX_BYTES + 2 * (NVME_CTRL_PAGE_SIZE - 1))

/* [한국어] PRP1 + 디스크립터 페이지들로 MAX_PRP_RANGE 를 항상 커버 가능한지 컴파일 타임 검증 */
static_assert(MAX_PRP_RANGE / NVME_CTRL_PAGE_SIZE <=
	(1 /* prp1 */ + NVME_MAX_NR_DESCRIPTORS * PRPS_PER_PAGE));

/* [한국어] 모듈 파라미터 동적 quirk 한 줄: PCI VID/DID 와 on/off 비트마스크.
 * probe 시 id->driver_data 및 DMI quirk 와 OR/AND 되어 ctrl.quirks 가 된다. */
struct quirk_entry {
	u16 vendor_id;		/* [한국어] PCI Vendor ID (16진 파싱) */
	u16 dev_id;		/* [한국어] PCI Device ID */
	u32 enabled_quirks;	/* [한국어] 강제로 켤 NVME_QUIRK_* 비트 */
	u32 disabled_quirks;	/* [한국어] '^name' 문법으로 끌 비트 */
};

static int use_threaded_interrupts;	/* [한국어] true 면 primary=check + threaded=nvme_irq */
module_param(use_threaded_interrupts, int, 0444);	/* [한국어] hardirq 부담 완화 옵션 */

static bool use_cmb_sqes = true;	/* [한국어] CMB 에 I/O SQ 본문을 둘지 (가능 시) */
module_param(use_cmb_sqes, bool, 0444);	/* [한국어] 호스트→장치 SQ DMA 트래픽 절감 */
MODULE_PARM_DESC(use_cmb_sqes, "use controller's memory buffer for I/O SQes");	/* [한국어] SQ 를 컨트롤러 메모리에 두면 도어벨 뒤의 PCIe 왕복이 하나 줄어든다 */

static unsigned int max_host_mem_size_mb = 128;	/* [한국어] 컨트롤러당 HMB 상한 MiB */
module_param(max_host_mem_size_mb, uint, 0444);	/* [한국어] 호스트 DRAM 잠식 제한 */
MODULE_PARM_DESC(max_host_mem_size_mb,	/* [한국어] HMB 상한 — 컨트롤러에 빌려 줄 호스트 메모리의 최대치 */
	"Maximum Host Memory Buffer (HMB) size per controller (in MiB)");

static unsigned int sgl_threshold = SZ_32K;	/* [한국어] 평균 세그먼트 ≥ 이 값이면 SGL 선택 */
module_param(sgl_threshold, uint, 0644);	/* [한국어] 0 이면 SGL 자발 선택 비활성(강제 분만) */
MODULE_PARM_DESC(sgl_threshold,	/* [한국어] 작은 요청은 PRP 가 더 싸고, 커지면 SGL 이 서술자 수를 줄인다 */
		"Use SGLs when average request segment size is larger or equal to "
		"this size. Use 0 to disable SGLs.");	/* [한국어] 0 은 SGL 을 아예 쓰지 않겠다는 뜻 */

#define NVME_PCI_MIN_QUEUE_SIZE 2	/* [한국어] I/O 큐 깊이 하한 (빈/가득 구분 최소 2) */
#define NVME_PCI_MAX_QUEUE_SIZE 4095	/* [한국어] 드라이버 I/O 깊이 상한 (스펙 MQES 보다 작음) */
static int io_queue_depth_set(const char *val, const struct kernel_param *kp);	/* [한국어] 깊이 setter 전방 선언 */
/* [한국어] io_queue_depth 모듈 파라미터 연산 벡터 — minmax 검증 set + uint get */
static const struct kernel_param_ops io_queue_depth_ops = {
	.set = io_queue_depth_set,	/* [한국어] [2,4095] 클램프 후 저장 */
	.get = param_get_uint,		/* [한국어] 현재 깊이 읽기 */
};

static unsigned int io_queue_depth = 1024;	/* [한국어] 기본 I/O 큐 깊이 — CAP.MQES 와 min */
module_param_cb(io_queue_depth, &io_queue_depth_ops, &io_queue_depth, 0644);	/* [한국어] 런타임 조정 가능 깊이 */
MODULE_PARM_DESC(io_queue_depth, "set io queue depth, should >= 2 and < 4096");	/* [한국어] 2 미만이면 큐가 성립하지 않고, 4096 은 스펙 상한이다 */

static struct quirk_entry *nvme_pci_quirk_list;	/* [한국어] quirks= 파라미터로 설치한 동적 테이블 */
static unsigned int nvme_pci_quirk_count;	/* [한국어] 동적 테이블 엔트리 수 */

/* Helper to parse individual quirk names */
/*
 * [한국어]
 * nvme_parse_quirk_names - 모듈 파라미터 quirks= 문자열의 quirk 이름 목록 해석
 *
 * @quirk_str: 쉼표 구분 이름. '^' 접두 시 disabled_quirks 로 기록.
 * @entry: enabled/disabled 비트마스크를 채울 quirk_entry.
 * @return: 0 성공, -EINVAL 알 수 없는 이름.
 *
 * 왜 필요한가: pci_device_id.driver_data 정적 테이블만으로는 현장 디버깅 때
 * 특정 VID:DID quirk 를 즉시 on/off 할 수 없다. 부팅/insmod 파라미터로
 * 동적 오버레이를 허용한다.
 * 동작: strsep 로 필드를 나누고 nvme_quirk_name(bit) 과 문자열 매칭.
 * 호출 체인: quirks_param_set → nvme_parse_quirk_entry → [여기]
 */
static int nvme_parse_quirk_names(char *quirk_str, struct quirk_entry *entry)
{
	int i;				/* [한국어] quirk 비트 인덱스 0..31 순회 */
	size_t field_len;		/* [한국어] 사용자 필드 길이 — 부분 문자열 오매칭 방지 */
	bool disabled, found;		/* [한국어] '^' 비활성 여부 / 이름 매칭 성공 여부 */
	char *p = quirk_str, *field;	/* [한국어] strsep 진행 포인터와 현재 토큰 */

	while ((field = strsep(&p, ",")) && *field) {	/* [한국어] 쉼표 구분 quirk 이름을 하나씩 소비 */
		disabled = false;	/* [한국어] 기본은 enabled_quirks 에 OR */
		found = false;		/* [한국어] 32비트 이름 테이블에서 미발견 시 에러 */

		if (*field == '^') {	/* [한국어] '^NAME' 문법은 해당 quirk 를 강제 해제 */
			/* Skip the '^' character */
			disabled = true;	/* [한국어] disabled_quirks 경로로 전환 */
			field++;		/* [한국어] 실제 이름은 캐럿 다음부터 */
		}

		field_len = strlen(field);	/* [한국어] 정확 길이 비교용 */
		for (i = 0; i < 32; i++) {	/* [한국어] NVME_QUIRK_* 비트 공간을 선형 스캔 */
			unsigned int bit = 1U << i;	/* [한국어] i번째 quirk 플래그 마스크 */
			char *q_name = nvme_quirk_name(bit);	/* [한국어] 비트→정규 문자열 (core/constants) */
			size_t q_len = strlen(q_name);	/* [한국어] 정규 이름 길이 */

			if (!strcmp(q_name, "unknown"))	/* [한국어] 더 이상 정의된 이름 없음 — 테이블 끝 */
				break;	/* [한국어] 미매칭으로 빠져 사용자 입력 거부 경로 */

			if (!strcmp(q_name, field) &&
				    q_len == field_len) {	/* [한국어] 이름·길이 모두 일치해야 수락 */
				if (disabled)
					entry->disabled_quirks |= bit;	/* [한국어] 이후 probe 에서 마스크로 제거 */
				else
					entry->enabled_quirks |= bit;	/* [한국어] 정적 ID quirk 위에 추가 OR */
				found = true;	/* [한국어] 유효 토큰 처리 완료 */
				break;		/* [한국어] 동일 비트 재검색 불필요 */
			}
		}

		if (!found) {	/* [한국어] 오타·구버전 이름 — 조용히 무시하면 디버깅 사고 */
			pr_err("nvme: unrecognized quirk %s\n", field);	/* [한국어] 부팅 로그에 잘못된 이름 노출 */
			return -EINVAL;	/* [한국어] 파라미터 설치 전체를 실패시킴 */
		}
	}
	return 0;	/* [한국어] 모든 토큰이 알려진 quirk 로 해석됨 */
}

/* Helper to parse a single VID:DID:quirk_names entry */
/*
 * [한국어]
 * nvme_parse_quirk_entry - "VID:DID:quirk_names" 한 엔트리 파싱
 *
 * @s: 파괴적 파싱 대상 (strsep 가 ':' 를 NUL 로 자름).
 * @entry: vendor_id/dev_id/quirks 출력.
 * @return: 0 또는 -EINVAL.
 * 호출 체인: quirks_param_set → [여기] → nvme_parse_quirk_names
 */
static int nvme_parse_quirk_entry(char *s, struct quirk_entry *entry)
{
	char *field;	/* [한국어] 콜론으로 분리된 현재 필드 */

	field = strsep(&s, ":");	/* [한국어] 첫 필드 = Vendor ID */
	if (!field || kstrtou16(field, 16, &entry->vendor_id))	/* [한국어] 16진 VID 필수 */
		return -EINVAL;	/* [한국어] 형식 오류 — 엔트리 폐기 */

	field = strsep(&s, ":");	/* [한국어] 둘째 필드 = Device ID */
	if (!field || kstrtou16(field, 16, &entry->dev_id))	/* [한국어] 16진 DID 필수 */
		return -EINVAL;	/* [한국어] DID 파싱 실패 */

	field = strsep(&s, ":");	/* [한국어] 셋째 필드 = quirk 이름 목록 */
	if (!field)
		return -EINVAL;	/* [한국어] 이름 목록 누락 거부 */

	return nvme_parse_quirk_names(field, entry);	/* [한국어] 쉼표 목록을 비트마스크로 */
}

/*
 * [한국어]
 * quirks_param_set - module_param quirks 문자열을 전역 동적 quirk 테이블로 설치
 *
 * @value: "VID:DID:names-VID:DID:names-..." 형태. '-' 가 엔트리 구분자.
 * @return: 0 성공, 음수 errno.
 *
 * 기존 nvme_pci_quirk_list 를 kfree 후 새 배열로 교체. 파싱 실패 시 이전
 * 리스트를 유지하지 않고 에러 반환. probe 시 detect_dynamic_quirks() 가 참조.
 */
static int quirks_param_set(const char *value, const struct kernel_param *kp)
{
	int count, err, i;		/* [한국어] 엔트리 수 / 에러 / 루프 인덱스 */
	struct quirk_entry *qlist;	/* [한국어] 새로 조립할 동적 테이블 */
	char *field, *val, *sep_ptr;	/* [한국어] 토큰·복사본·strsep 커서 */

	err = param_set_copystring(value, kp);	/* [한국어] 원본 문자열을 quirks_param 버퍼에 보관 */
	if (err)
		return err;	/* [한국어] 버퍼 초과 등 — 테이블 불변 */

	val = kstrdup(value, GFP_KERNEL);	/* [한국어] strsep 파괴 파싱용 가변 복사본 */
	if (!val)
		return -ENOMEM;	/* [한국어] 메모리 부족 — 파라미터 거부 */

	if (!*val)
		goto out_free_val;	/* [한국어] 빈 문자열은 테이블 비우지 않고 복사만 성공 처리 */

	count = 1;	/* [한국어] 구분자 없으면 엔트리 1개 */
	for (i = 0; val[i]; i++) {	/* [한국어] 순회 루프 */
		if (val[i] == '-')
			count++;	/* [한국어] '-' 개수+1 = 엔트리 수 */
	}

	qlist = kcalloc(count, sizeof(*qlist), GFP_KERNEL);	/* [한국어] 제로 초기화 quirk 배열 */
	if (!qlist) {	/* [한국어] 파싱 목록을 못 잡았다 — quirk 없이 진행하는 것이 무해하다 */
		err = -ENOMEM;	/* [한국어] 할당 실패 기록 */
		goto out_free_val;	/* [한국어] val 만 해제 */
	}

	i = 0;			/* [한국어] 채워 넣을 슬롯 */
	sep_ptr = val;		/* [한국어] 전체 문자열 커서 */
	while ((field = strsep(&sep_ptr, "-"))) {	/* [한국어] 엔트리 단위 분리 */
		if (nvme_parse_quirk_entry(field, &qlist[i])) {	/* [한국어] VID:DID:names 해석 */
			pr_err("nvme: failed to parse quirk string %s\n",
				value);	/* [한국어] 원문 전체를 로그에 남겨 수정 유도 */
			goto out_free_qlist;	/* [한국어] 부분 적용 없이 롤백 */
		}

		i++;	/* [한국어] 다음 슬롯 */
	}

	kfree(nvme_pci_quirk_list);	/* [한국어] 이전 동적 테이블 교체 */
	nvme_pci_quirk_count = count;	/* [한국어] 검색 루프 상한 */
	nvme_pci_quirk_list  = qlist;	/* [한국어] 전역 포인터 원자적 교체(파라미터 경로 단일 스레드) */
	goto out_free_val;	/* [한국어] 성공 — qlist 소유권은 전역이 가짐 */

out_free_qlist:
	kfree(qlist);	/* [한국어] 파싱 실패 시 새 테이블 폐기 */
out_free_val:
	kfree(val);	/* [한국어] strsep 작업 버퍼 반환 */
	return err;	/* [한국어] 0 또는 음수 errno */
}

static char quirks_param[128];	/* [한국어] 파라미터 원문 보관 버퍼 (modinfo/sysfs get) */
/* [한국어] quirks 파라미터 ops — 커스텀 set + 문자열 get */
static const struct kernel_param_ops quirks_param_ops = {
	.set = quirks_param_set,	/* [한국어] 파싱·테이블 설치 */
	.get = param_get_string,	/* [한국어] 보관 문자열 반환 */
};

/* [한국어] kparam_string 기술자 — maxlen/string 포인터를 프레임워크에 전달 */
static struct kparam_string quirks_param_string = {
	.maxlen = sizeof(quirks_param),	/* [한국어] 복사 상한 */
	.string = quirks_param,		/* [한국어] 실제 버퍼 */
};

module_param_cb(quirks, &quirks_param_ops, &quirks_param_string, 0444);	/* [한국어] quirks=VID:DID:... 등록 */
MODULE_PARM_DESC(quirks, "Enable/disable NVMe quirks by specifying "	/* [한국어] 드라이버에 없는 장치의 quirk 를 사용자가 부팅 시 붙일 수 있게 한다 */
						"quirks=VID:DID:quirk_names");

/*
 * [한국어]
 * io_queue_count_set - write_queues/poll_queues 검증 setter
 *
 * blk_mq_num_possible_queues(0) 초과를 거부해 affinity 단계 전 불가능한 큐 수를 차단.
 */
static int io_queue_count_set(const char *val, const struct kernel_param *kp)
{
	unsigned int n;	/* [한국어] 파싱된 큐 개수 후보 */
	int ret;	/* [한국어] kstrtouint 결과 */

	ret = kstrtouint(val, 10, &n);	/* [한국어] 10진 부호 없는 정수 해석 */
	if (ret != 0 || n > blk_mq_num_possible_queues(0))	/* [한국어] 파싱 실패 또는 CPU 가능 큐 초과 */
		return -EINVAL;	/* [한국어] 잘못된 값 거부 */
	return param_set_uint(val, kp);	/* [한국어] 프레임워크 기본 uint 저장 */
}

/* [한국어] write/poll 큐 수 파라미터 공통 ops */
static const struct kernel_param_ops io_queue_count_ops = {
	.set = io_queue_count_set,	/* [한국어] 상한 검증 setter */
	.get = param_get_uint,		/* [한국어] 현재 값 읽기 */
};

static unsigned int write_queues;	/* [한국어] 쓰기(DEFAULT) 전용 큐 목표 수 — 0 이면 R/W 공유 */
module_param_cb(write_queues, &io_queue_count_ops, &write_queues, 0644);	/* [한국어] 리셋 시 nr_write_queues 로 샘플 */
MODULE_PARM_DESC(write_queues,	/* [한국어] 읽기와 쓰기를 다른 큐로 갈라 서로의 지연에 끼어들지 않게 한다 */
	"Number of queues to use for writes. If not set, reads and writes "
	"will share a queue set.");	/* [한국어] 설정하지 않으면 한 큐 집합을 공유한다 */

static unsigned int poll_queues;	/* [한국어] 인터럽트 없는 폴링 완료 큐 목표 수 */
module_param_cb(poll_queues, &io_queue_count_ops, &poll_queues, 0644);	/* [한국어] HCTX_TYPE_POLL 맵 크기 결정 */
MODULE_PARM_DESC(poll_queues, "Number of queues to use for polled IO.");	/* [한국어] 인터럽트 없이 CPU 가 직접 훑는 큐 — 지연이 중요한 작업용 */

static bool noacpi;	/* [한국어] true 면 ACPI storage D3 simple-suspend quirk 무시 */
module_param(noacpi, bool, 0444);	/* [한국어] 플랫폼 펌웨어 힌트 우회 디버그용 */
MODULE_PARM_DESC(noacpi, "disable acpi bios quirks");	/* [한국어] ACPI 가 알려 주는 quirk 가 오히려 문제를 일으킬 때 끄는 탈출구 */

struct nvme_dev;	/* [한국어] 전방 선언 — nvme_queue 와 상호 참조 */
struct nvme_queue;	/* [한국어] 전방 선언 — 함수 프로토타입용 */

static void nvme_dev_disable(struct nvme_dev *dev, bool shutdown);	/* [한국어] 정지/리셋 중추 — 후방 참조 */
static void nvme_delete_io_queues(struct nvme_dev *dev);	/* [한국어] Delete SQ/CQ 일괄 */
static void nvme_update_attrs(struct nvme_dev *dev);	/* [한국어] CMB/HMB sysfs 가시성 갱신 */

/* [한국어] NUMA 노드 하나 분의 PRP/SGL descriptor dma_pool 쌍.
 * large=페이지, small=256B. nvme_dev.descriptor_pools[] 유연 배열 원소. */
struct nvme_descriptor_pools {
	struct dma_pool *large;	/* [한국어] NVME_CTRL_PAGE_SIZE 정렬 PRP/SGL 페이지 풀 */
	struct dma_pool *small;	/* [한국어] 짧은 리스트용 256B(또는 512 정렬 quirk) 풀 */
};

/*
 * Represents an NVM Express device.  Each nvme_dev is a PCI function.
 */
/*
 * [한국어] PCIe 함수 하나 = NVMe 컨트롤러 인스턴스.
 * queues: [0]=admin, [1..]=I/O. tagset/admin_tagset 은 blk-mq 태그.
 * dbs: BAR 도어벨 창. bar/bar_mapped_size: MMIO 매핑.
 * cmb_*: 컨트롤러 메모리 버퍼. host_mem_*: HMB. dbbuf_*: shadow doorbell.
 * ctrl: core 공통 상태(임베드). shutdown_lock: disable vs setup 직렬화.
 * descriptor_pools[]: 노드 수만큼 말단 유연 배열.
 */
struct nvme_dev {
	struct nvme_queue *queues;		/* [한국어] 큐 배열 — [0] admin, 이후 I/O */
	struct blk_mq_tag_set tagset;		/* [한국어] I/O blk-mq tag set */
	struct blk_mq_tag_set admin_tagset;	/* [한국어] Admin blk-mq tag set */
	u32 __iomem *dbs;			/* [한국어] BAR 도어벨 창 포인터 (SQ/CQ 쌍) */
	struct device *dev;			/* [한국어] 보통 &pdev->dev — DMA/IRQ API 인자 */
	unsigned online_queues;			/* [한국어] ENABLED 된 큐 개수 */
	unsigned max_qid;			/* [한국어] 생성 대상 최대 큐 ID */
	unsigned io_queues[HCTX_MAX_TYPES];	/* [한국어] DEFAULT/READ/POLL 맵별 큐 수 */
	unsigned int num_vecs;			/* [한국어] 할당된 IRQ 벡터 수 */
	u32 q_depth;				/* [한국어] 큐 깊이 (엔트리 수, 1-based) */
	int io_sqes;				/* [한국어] I/O SQE 크기 log2 (보통 6→64B) */
	u32 db_stride;				/* [한국어] 도어벨 DWORD stride (CAP.DSTRD) */
	void __iomem *bar;			/* [한국어] BAR0 MMIO 매핑 가상주소 */
	unsigned long bar_mapped_size;		/* [한국어] 현재 ioremap 크기 — remap 판단 */
	struct mutex shutdown_lock;		/* [한국어] disable ↔ BAR/큐 setup 상호배제 */
	bool subsystem;				/* [한국어] NSSRC 지원 서브시스템 컨트롤러 */
	u64 cmb_size;				/* [한국어] 사용 가능 CMB 바이트 (0=없음) */
	bool cmb_use_sqes;			/* [한국어] I/O SQ 를 CMB 에 둘지 */
	u32 cmbsz;				/* [한국어] CMBSZ 레지스터 캐시 */
	u32 cmbloc;				/* [한국어] CMBLOC 레지스터 캐시 */
	struct nvme_ctrl ctrl;			/* [한국어] 임베드된 공통 nvme_ctrl (core 상태기계) */
	u32 last_ps;				/* [한국어] suspend 직전 전원 상태 (resume 복귀) */
	bool hmb;				/* [한국어] HMB feature enable 여부 */
	struct sg_table *hmb_sgt;		/* [한국어] noncontiguous 단일 세그먼트 HMB */
	mempool_t *dmavec_mempool;		/* [한국어] non-IOVA PRP unmap 용 dma_vec 풀 */

	/* shadow doorbell buffer support: */
	/* [한국어] OACS.DBBUF — MMIO 도어벨을 호스트 메모리 쓰기로 완화 */
	__le32 *dbbuf_dbs;			/* [한국어] shadow doorbell 값 배열 VA */
	dma_addr_t dbbuf_dbs_dma_addr;		/* [한국어] dbbuf_dbs DMA 주소 (컨트롤러에 전달) */
	__le32 *dbbuf_eis;			/* [한국어] shadow event index 배열 VA */
	dma_addr_t dbbuf_eis_dma_addr;		/* [한국어] dbbuf_eis DMA 주소 */

	/* host memory buffer support: */
	/* [한국어] 컨트롤러 캐시 대체용 호스트 메모리 — Set Features(HMB) */
	u64 host_mem_size;			/* [한국어] 할당된 HMB 총 바이트 */
	u32 nr_host_mem_descs;			/* [한국어] HMB 디스크립터 개수 */
	u32 host_mem_descs_size;		/* [한국어] 디스크립터 배열 바이트 */
	dma_addr_t host_mem_descs_dma;		/* [한국어] 디스크립터 배열 DMA 주소 */
	struct nvme_host_mem_buf_desc *host_mem_descs;	/* [한국어] HMB 디스크립터 배열 VA */
	void **host_mem_desc_bufs;		/* [한국어] multi 할당 시 청크 VA 배열 */
	unsigned int nr_allocated_queues;	/* [한국어] queues[] 할당 슬롯 수 (admin 포함) */
	unsigned int nr_write_queues;		/* [한국어] 리셋 시 샘플한 write_queues */
	unsigned int nr_poll_queues;		/* [한국어] 리셋 시 샘플한 poll_queues */
	struct nvme_descriptor_pools descriptor_pools[]; /* [한국어] 노드별 pool 유연 배열 */
};

/*
 * [한국어]
 * io_queue_depth_set - I/O 큐 깊이를 [2, 4095] 로 클램프하는 파라미터 setter
 *
 * 실제 동작 깊이는 nvme_pci_enable 에서 CAP.MQES+1 과 min 한다.
 */
static int io_queue_depth_set(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, NVME_PCI_MIN_QUEUE_SIZE,
			NVME_PCI_MAX_QUEUE_SIZE);	/* [한국어] 드라이버 허용 범위 밖 값 거부 */
}

/*
 * [한국어]
 * sq_idx - shadow doorbell 배열에서 Submission Queue doorbell 슬롯 오프셋
 *
 * 큐 qid 당 SQ/CQ 두 도어벨 × stride(DWORD). SQ 는 짝수 슬롯. CAP.DSTRD 반영.
 */
static inline unsigned int sq_idx(unsigned int qid, u32 stride)
{
	return qid * 2 * stride;	/* [한국어] (SQ,CQ) 쌍에서 SQ 가 선행 */
}

/*
 * [한국어]
 * cq_idx - shadow doorbell 배열에서 Completion Queue doorbell 슬롯 오프셋
 */
static inline unsigned int cq_idx(unsigned int qid, u32 stride)
{
	return (qid * 2 + 1) * stride;	/* [한국어] SQ 다음 슬롯이 CQ */
}

/*
 * [한국어]
 * to_nvme_dev - 공통 nvme_ctrl 를 PCIe nvme_dev 로 역참조
 *
 * core 콜백이 ctrl 만 넘길 때 BAR/queues 접근하는 단일 관문.
 */
static inline struct nvme_dev *to_nvme_dev(struct nvme_ctrl *ctrl)
{
	return container_of(ctrl, struct nvme_dev, ctrl);	/* [한국어] 임베드 ctrl 멤버 기준 부모 복원 */
}

/*
 * An NVM Express queue.  Each device has at least two (one for admin
 * commands and one for I/O commands).
 */
/*
 * [한국어] Admin 또는 I/O 큐 쌍(SQ+CQ) 런타임 상태.
 * sq_cmds/cqes + DMA(또는 CMB). q_db=SQ doorbell MMIO.
 * sq_tail/last_sq_tail: 배치 도어벨 최적화. cq_head/cq_phase: 완료 소비자.
 * flags: ENABLED, SQ_CMB, DELETE_ERROR, POLLED. dbbuf_*: shadow 포인터.
 */
struct nvme_queue {
	struct nvme_dev *dev;			/* [한국어] 소속 컨트롤러 */
	struct nvme_descriptor_pools descriptor_pools; /* [한국어] hctx NUMA 풀 캐시 사본 */
	spinlock_t sq_lock;			/* [한국어] SQ 복사·tail·doorbell 직렬화 */
	void *sq_cmds;				/* [한국어] SQ 링 VA (호스트 DMA 또는 CMB) */
	 /* only used for poll queues: */
	/* [한국어] 폴링/reap 과 IRQ 폴 직렬화 — 캐시라인 정렬로 false sharing 완화 */
	/* [한국어] 폴링 큐의 완료 큐를 보호하는 락.
	 * 왜 sq_lock 과 따로 두는가: 제출과 완료가 서로 다른 CPU 에서 동시에
	 *   진행되는 것이 정상 경로이므로, 하나의 락으로 묶으면 두 방향이
	 *   불필요하게 직렬화된다.
	 * 왜 캐시라인 정렬인가: sq_lock 과 같은 라인에 놓이면 서로 다른 CPU 가
	 *   각자의 락만 만져도 라인이 오가는 거짓 공유가 생긴다. 정렬이
	 *   그 라인을 갈라 준다.
	 * 설정자/읽는 자: nvme_poll 이 잡고, 인터럽트 큐에서는 쓰이지 않는다.
	 * 동기화: 폴링은 프로세스 문맥에서만 불리므로 irqsave 가 필요 없다. */
	spinlock_t cq_poll_lock ____cacheline_aligned_in_smp;
	struct nvme_completion *cqes;		/* [한국어] CQ 링 VA (coherent) */
	dma_addr_t sq_dma_addr;			/* [한국어] SQ 베이스 DMA/버스 주소 (Create SQ PRP1) */
	dma_addr_t cq_dma_addr;			/* [한국어] CQ 베이스 DMA 주소 (Create CQ PRP1) */
	u32 __iomem *q_db;			/* [한국어] 이 큐 SQ doorbell MMIO — +stride 가 CQ db */
	u32 q_depth;				/* [한국어] 링 엔트리 수 */
	u16 cq_vector;				/* [한국어] MSI-X 벡터 인덱스 */
	u16 sq_tail;				/* [한국어] 다음에 쓸 SQ 슬롯 (호스트 생산자) */
	u16 last_sq_tail;			/* [한국어] 마지막으로 BAR/dbbuf 에 울린 tail */
	u16 cq_head;				/* [한국어] 다음에 읽을 CQ 슬롯 (호스트 소비자) */
	u16 qid;				/* [한국어] 큐 ID — 0=admin */
	u8 cq_phase;				/* [한국어] 기대 phase 비트 (초기 1, wrap 시 토글) */
	u8 sqes;				/* [한국어] SQE 크기 log2 */
	unsigned long flags;			/* [한국어] NVMEQ_* 비트필드 */
#define NVMEQ_ENABLED		0	/* [한국어] 제출 허용 — queue_rq 게이트 */
#define NVMEQ_SQ_CMB		1	/* [한국어] SQ 본문이 CMB p2pmem */
#define NVMEQ_DELETE_ERROR	2	/* [한국어] Delete CQ 실패 기록 */
#define NVMEQ_POLLED		3	/* [한국어] 인터럽트 없이 blk-mq poll 완료 */
	__le32 *dbbuf_sq_db;			/* [한국어] shadow SQ doorbell 슬롯 */
	__le32 *dbbuf_cq_db;			/* [한국어] shadow CQ doorbell 슬롯 */
	__le32 *dbbuf_sq_ei;			/* [한국어] SQ event index (컨트롤러 갱신) */
	__le32 *dbbuf_cq_ei;			/* [한국어] CQ event index */
	struct completion delete_done;		/* [한국어] 비동기 Delete SQ/CQ 완료 대기 */
};

/* bits for iod->flags */
/* [한국어] 요청 PDU(iod) 수명 동안의 매핑/중단 상태 비트 — unmap 경로 분기 핵심 */
enum nvme_iod_flags {
	/* this command has been aborted by the timeout handler */
	IOD_ABORTED		= 1U << 0,	/* [한국어] timeout 이 Abort 시도함 — 재차 시 리셋 */

	/* uses the small descriptor pool */
	IOD_SMALL_DESCRIPTOR	= 1U << 1,	/* [한국어] descriptor 가 small pool 출신 */

	/* single segment dma mapping */
	IOD_SINGLE_SEGMENT	= 1U << 2,	/* [한국어] 고속 단일 bvec 경로 — unmap_page */

	/* Data payload contains p2p memory */
	IOD_DATA_P2P		= 1U << 3,	/* [한국어] 데이터 P2P bus addr 매핑 */

	/* Metadata contains p2p memory */
	IOD_META_P2P		= 1U << 4,	/* [한국어] 메타 P2P bus addr */

	/* Data payload contains MMIO memory */
	IOD_DATA_MMIO		= 1U << 5,	/* [한국어] 데이터 host-bridge MMIO P2P */

	/* Metadata contains MMIO memory */
	IOD_META_MMIO		= 1U << 6,	/* [한국어] 메타 host-bridge MMIO */

	/* Metadata using non-coalesced MPTR */
	IOD_SINGLE_META_SEGMENT	= 1U << 7,	/* [한국어] 단일 integrity bvec MPTR 경로 */
};

/* [한국어] non-IOVA PRP 경로에서 unmap 용 (dma_addr, len) 한 쌍 */
struct nvme_dma_vec {
	dma_addr_t addr;	/* [한국어] 매핑된 DMA 주소 */
	unsigned int len;	/* [한국어] 바이트 길이 */
};

/*
 * The nvme_iod describes the data in an I/O.
 */
/*
 * [한국어] blk-mq request PDU: NVMe 명령 + DMA 매핑 상태.
 * req=core 메타, cmd=64B SQE. descriptors[]=PRP/SGL 페이지.
 * 제출 전 prep, 완료 시 unmap 이 쌍으로 동작. sizeof 가 tagset cmd_size.
 */
struct nvme_iod {
	struct nvme_request req;	/* [한국어] core 요청 메타 (status/ctrl/flags) */
	struct nvme_command cmd;	/* [한국어] 제출용 64바이트 SQE 템플릿 */
	u8 flags;			/* [한국어] enum nvme_iod_flags 비트 OR */
	u8 nr_descriptors;		/* [한국어] descriptors[] 유효 개수 */

	size_t total_len;		/* [한국어] 매핑된 데이터 바이트 누적 */
	struct dma_iova_state dma_state; /* [한국어] 데이터 DMA/IOVA 상태 */
	void *descriptors[NVME_MAX_NR_DESCRIPTORS]; /* [한국어] PRP list/SGL 페이지 VA */
	struct nvme_dma_vec *dma_vecs;	/* [한국어] phys unmap 벡터 (mempool) */
	unsigned int nr_dma_vecs;	/* [한국어] dma_vecs 유효 개수 */

	dma_addr_t meta_dma;		/* [한국어] 메타 버퍼 또는 meta desc DMA */
	size_t meta_total_len;		/* [한국어] 메타 매핑 총 길이 */
	struct dma_iova_state meta_dma_state; /* [한국어] 메타 DMA/IOVA 상태 */
	struct nvme_sgl_desc *meta_descriptor; /* [한국어] meta SGL small-pool 리스트 */
};

/*
 * [한국어]
 * nvme_dbbuf_size - Shadow Doorbell 버퍼 바이트 크기
 *
 * 큐마다 SQ+CQ 도어벨 2×4B×stride. OACS.DBBUF 지원 시에만 할당.
 */
static inline unsigned int nvme_dbbuf_size(struct nvme_dev *dev)
{
	return dev->nr_allocated_queues * 8 * dev->db_stride;	/* [한국어] 큐당 8바이트×stride */
}

/*
 * [한국어]
 * nvme_dbbuf_dma_alloc - dbbuf doorbell + event index DMA 버퍼 할당
 *
 * 이미 있으면 memset 으로 stale 제거(리셋 재사용). 실패 시 MMIO 도어벨 폴백.
 * 호출: probe/reset_work 컨트롤 플레인.
 */
static void nvme_dbbuf_dma_alloc(struct nvme_dev *dev)
{
	unsigned int mem_size = nvme_dbbuf_size(dev);	/* [한국어] dbs·eis 각각 동일 크기 */

	if (!(dev->ctrl.oacs & NVME_CTRL_OACS_DBBUF_SUPP))	/* [한국어] Identify 가 미지원이면 생략 */
		return;	/* [한국어] optional 기능 — 실패 아닌 정상 스킵 */

	if (dev->dbbuf_dbs) {	/* [한국어] 이미 있으면 재사용한다 — 리셋마다 다시 잡을 이유가 없다 */
		/*
		 * Clear the dbbuf memory so the driver doesn't observe stale
		 * values from the previous instantiation.
		 */
		/* [한국어] 리셋 후 재사용: 이전 tail/event 잔존 시 오동작 방지 */
		memset(dev->dbbuf_dbs, 0, mem_size);	/* [한국어] doorbell 값 제로화 */
		memset(dev->dbbuf_eis, 0, mem_size);	/* [한국어] event index 제로화 */
		return;	/* [한국어] 재할당 없이 기존 DMA 버퍼 유지 */
	}

	dev->dbbuf_dbs = dma_alloc_coherent(dev->dev, mem_size,
					    &dev->dbbuf_dbs_dma_addr,
					    GFP_KERNEL);	/* [한국어] 컨트롤러가 snooping 할 doorbell 메모리 */
	if (!dev->dbbuf_dbs)
		goto fail;	/* [한국어] dbs 실패 시 eis 시도 불필요 */
	dev->dbbuf_eis = dma_alloc_coherent(dev->dev, mem_size,
					    &dev->dbbuf_eis_dma_addr,
					    GFP_KERNEL);	/* [한국어] 컨트롤러가 갱신하는 event index */
	if (!dev->dbbuf_eis)
		goto fail_free_dbbuf_dbs;	/* [한국어] 쌍이 불완전하면 dbs 롤백 */
	return;	/* [한국어] 성공 — 이후 dbbuf_set 이 admin 으로 등록 */

fail_free_dbbuf_dbs:
	dma_free_coherent(dev->dev, mem_size, dev->dbbuf_dbs,
			  dev->dbbuf_dbs_dma_addr);	/* [한국어] 짝 실패 시 부분 할당 해제 */
	dev->dbbuf_dbs = NULL;	/* [한국어] free 후 dangling 포인터 제거 */
fail:
	dev_warn(dev->dev, "unable to allocate dma for dbbuf\n");	/* [한국어] 경고만 — I/O 는 MMIO 도어벨로 계속 */
}

/*
 * [한국어]
 * nvme_dbbuf_dma_free - shadow doorbell DMA 쌍 해제 (remove·set 실패 롤백)
 */
static void nvme_dbbuf_dma_free(struct nvme_dev *dev)
{
	unsigned int mem_size = nvme_dbbuf_size(dev);	/* [한국어] alloc 과 동일 크기 계산 */

	if (dev->dbbuf_dbs) {	/* [한국어] 두 버퍼를 각각 확인하는 이유: 하나만 할당된 채 실패했을 수 있다 */
		dma_free_coherent(dev->dev, mem_size,
				  dev->dbbuf_dbs, dev->dbbuf_dbs_dma_addr);	/* [한국어] doorbell 배열 반환 */
		dev->dbbuf_dbs = NULL;	/* [한국어] 이중 free 방지 */
	}
	if (dev->dbbuf_eis) {	/* [한국어] 이벤트 인덱스 버퍼도 같은 방식으로 */
		dma_free_coherent(dev->dev, mem_size,
				  dev->dbbuf_eis, dev->dbbuf_eis_dma_addr);	/* [한국어] event index 배열 반환 */
		dev->dbbuf_eis = NULL;	/* [한국어] 포인터를 지워야 다음 리셋이 해제된 메모리를 컨트롤러에 알리지 않는다 */
	}
}

/*
 * [한국어]
 * nvme_dbbuf_init - 개별 I/O 큐의 dbbuf 포인터를 전역 배열 슬롯에 연결
 *
 * Admin qid=0 은 shadow 미사용. I/O 큐만 인덱싱.
 */
static void nvme_dbbuf_init(struct nvme_dev *dev,
			    struct nvme_queue *nvmeq, int qid)
{
	if (!dev->dbbuf_dbs || !qid)	/* [한국어] 미할당 또는 admin 큐 — 연결 생략 */
		return;	/* [한국어] shadow 도어벨을 안 쓰거나 admin 큐면 걸 것이 없다 — admin 은 최적화 대상이 아니다 */

	nvmeq->dbbuf_sq_db = &dev->dbbuf_dbs[sq_idx(qid, dev->db_stride)];	/* [한국어] SQ shadow doorbell */
	nvmeq->dbbuf_cq_db = &dev->dbbuf_dbs[cq_idx(qid, dev->db_stride)];	/* [한국어] CQ shadow doorbell */
	nvmeq->dbbuf_sq_ei = &dev->dbbuf_eis[sq_idx(qid, dev->db_stride)];	/* [한국어] SQ event index */
	nvmeq->dbbuf_cq_ei = &dev->dbbuf_eis[cq_idx(qid, dev->db_stride)];	/* [한국어] CQ event index */
}

/*
 * [한국어]
 * nvme_dbbuf_free - 큐 구조체 dbbuf 포인터만 NULL (DMA 본체는 유지/별도 free)
 */
static void nvme_dbbuf_free(struct nvme_queue *nvmeq)
{
	if (!nvmeq->qid)	/* [한국어] admin 은 원래 연결 없음 */
		return;	/* [한국어] admin 큐는 shadow 도어벨을 쓰지 않아 지울 것도 없다 */

	nvmeq->dbbuf_sq_db = NULL;	/* [한국어] 이후 write_sq_db 가 MMIO 강제 경로 */
	nvmeq->dbbuf_cq_db = NULL;	/* [한국어] 네 포인터를 모두 지워야 제출 경로가 MMIO 도어벨로 되돌아간다 */
	nvmeq->dbbuf_sq_ei = NULL;
	nvmeq->dbbuf_cq_ei = NULL;
}

/*
 * [한국어]
 * nvme_dbbuf_set - Admin 명령으로 컨트롤러에 shadow doorbell DMA 주소 등록
 *
 * prp1=dbs, prp2=eis. 실패 시 메모리 해제 후 전 큐 free — optional 기능.
 * 호출 체인: probe/reset LIVE 직전 → [여기] → nvme_submit_sync_cmd
 */
static void nvme_dbbuf_set(struct nvme_dev *dev)
{
	struct nvme_command c = { };	/* [한국어] dbbuf admin SQE 제로 초기화 */
	unsigned int i;			/* [한국어] online 큐 순회 인덱스 */

	if (!dev->dbbuf_dbs)	/* [한국어] 할당 실패·미지원이면 등록 불필요 */
		return;	/* [한국어] shadow 도어벨을 쓰지 않는 구성이면 컨트롤러에 알릴 것이 없다 */

	c.dbbuf.opcode = nvme_admin_dbbuf;	/* [한국어] Shadow Doorbell Buffer 설정 opcode */
	c.dbbuf.prp1 = cpu_to_le64(dev->dbbuf_dbs_dma_addr);	/* [한국어] doorbell 메모리 DMA */
	c.dbbuf.prp2 = cpu_to_le64(dev->dbbuf_eis_dma_addr);	/* [한국어] event index 메모리 DMA */

	if (nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0)) {	/* [한국어] 컨트롤러에 주소 쌍 등록 */
		dev_warn(dev->ctrl.device, "unable to set dbbuf\n");	/* [한국어] 기능 포기, I/O 계속 */
		/* Free memory and continue on */
		nvme_dbbuf_dma_free(dev);	/* [한국어] 쓸모 없어진 DMA 반환 */

		for (i = 1; i < dev->online_queues; i++)	/* [한국어] admin 제외 전 큐 포인터 절단 */
			nvme_dbbuf_free(&dev->queues[i]);	/* [한국어] 이후 전부 BAR writel 경로 */
	}
}

/*
 * [한국어]
 * nvme_dbbuf_need_event - 스펙 Shadow doorbell 이벤트 인덱스 판정
 *
 * (new-event-1) < (new-old) 이면 컨트롤러 미인지 → MMIO doorbell 필수.
 */
static inline int nvme_dbbuf_need_event(u16 event_idx, u16 new_idx, u16 old)
{
	return (u16)(new_idx - event_idx - 1) < (u16)(new_idx - old);	/* [한국어] wrap-safe u16 모듈러 비교 */
}

/* Update dbbuf and return true if an MMIO is required */
/*
 * [한국어]
 * nvme_dbbuf_update_and_check_event - dbbuf 갱신 후 MMIO 필요 여부 반환
 *
 * wmb(SQ/CQ 가시화) → doorbell 메모리 기록 → mb → event index 읽기.
 * @return true 면 호출자가 writel 로 BAR doorbell 을 울려야 함.
 */
static bool nvme_dbbuf_update_and_check_event(u16 value, __le32 *dbbuf_db,
					      volatile __le32 *dbbuf_ei)
{
	if (dbbuf_db) {	/* [한국어] shadow 미연결이면 무조건 MMIO */
		u16 old_value, event_idx;	/* [한국어] 이전 doorbell 값 / 컨트롤러 event index */

		/*
		 * Ensure that the queue is written before updating
		 * the doorbell in memory
		 */
		wmb();	/* [한국어] SQE/CQE 페이로드 스토어가 doorbell 전에 전역 가시 */

		old_value = le32_to_cpu(*dbbuf_db);	/* [한국어] 직전 호스트가 쓴 tail/head */
		*dbbuf_db = cpu_to_le32(value);	/* [한국어] 새 인덱스를 공유 메모리에 게시 */

		/*
		 * Ensure that the doorbell is updated before reading the event
		 * index from memory.  The controller needs to provide similar
		 * ordering to ensure the event index is updated before reading
		 * the doorbell.
		 */
		mb();	/* [한국어] doorbell 스토어 vs event index 로드 양방향 순서 */

		event_idx = le32_to_cpu(*dbbuf_ei);	/* [한국어] 컨트롤러가 마지막으로 인지한 지점 */
		if (!nvme_dbbuf_need_event(event_idx, value, old_value))	/* [한국어] 이미 추적 중이면 MMIO 생략 */
			return false;	/* [한국어] 호스트 메모리 갱신만으로 충분 */
	}

	return true;	/* [한국어] BAR doorbell writel 필요 (또는 dbbuf 없음) */
}

/*
 * [한국어]
 * nvme_setup_descriptor_pools - NUMA 노드별 PRP/SGL descriptor DMA pool 생성
 *
 * large=페이지 정렬, small=256B(또는 512 quirk). hctx init 시 큐에 캐시.
 * 호출 체인: nvme_init_hctx_common → [여기]
 */
static struct nvme_descriptor_pools *
nvme_setup_descriptor_pools(struct nvme_dev *dev, unsigned numa_node)
{
	struct nvme_descriptor_pools *pools = &dev->descriptor_pools[numa_node];	/* [한국어] 노드 전용 풀 슬롯 */
	size_t small_align = NVME_SMALL_POOL_SIZE;	/* [한국어] small 풀 정렬 — quirk 시 512 로 완화 */

	if (pools->small)
		return pools; /* already initialized */	/* [한국어] 노드당 1회 생성 — 재호출 시 캐시 반환 */

	pools->large = dma_pool_create_node("nvme descriptor page", dev->dev,
			NVME_CTRL_PAGE_SIZE, NVME_CTRL_PAGE_SIZE, 0, numa_node);	/* [한국어] 4K PRP/SGL 페이지 풀 */
	if (!pools->large)
		return ERR_PTR(-ENOMEM);	/* [한국어] large 풀 실패 — hctx init 중단 */

	if (dev->ctrl.quirks & NVME_QUIRK_DMAPOOL_ALIGN_512)	/* [한국어] 일부 컨트롤러 512 정렬 요구 */
		small_align = 512;	/* [한국어] small pool 정렬 완화/강제 */

	pools->small = dma_pool_create_node("nvme descriptor small", dev->dev,
			NVME_SMALL_POOL_SIZE, small_align, 0, numa_node);	/* [한국어] 256B 짧은 PRP/SGL 풀 */
	if (!pools->small) {	/* [한국어] 작은 풀이 없으면 큰 풀만으로도 동작하지만, 여기서는 실패로 처리해 구성을 단순하게 유지한다 */
		dma_pool_destroy(pools->large);	/* [한국어] 짝 실패 시 large 롤백 */
		pools->large = NULL;	/* [한국어] 재시도 시 재생성 허용 */
		return ERR_PTR(-ENOMEM);	/* [한국어] hctx 초기화 실패 전파 */
	}

	return pools;	/* [한국어] 노드 풀 준비 — 핫패스 dma_pool_alloc 대상 */
}

/*
 * [한국어]
 * nvme_release_descriptor_pools - 전 NUMA 노드 PRP/SGL pool 파괴 (remove)
 */
static void nvme_release_descriptor_pools(struct nvme_dev *dev)
{
	unsigned i;	/* [한국어] 노드 인덱스 0..nr_node_ids-1 */

	for (i = 0; i < nr_node_ids; i++) {	/* [한국어] 유연 배열 전 슬롯 */
		struct nvme_descriptor_pools *pools = &dev->descriptor_pools[i];	/* [한국어] 노드 풀 쌍 */

		dma_pool_destroy(pools->large);	/* [한국어] 4K 페이지 풀 (NULL-safe) */
		dma_pool_destroy(pools->small);	/* [한국어] small 풀 */
	}
}

/*
 * [한국어]
 * nvme_init_hctx_common - hctx↔nvme_queue·NUMA descriptor pool 바인딩
 *
 * tags 일치 WARN. pool setup 실패 시 PTR_ERR. driver_data=nvmeq 로 핫패스 연결.
 */
static int nvme_init_hctx_common(struct blk_mq_hw_ctx *hctx, void *data,
		unsigned qid)	/* [한국어] 지역 변수 */
{
	struct nvme_dev *dev = to_nvme_dev(data);	/* [한국어] tagset driver_data=ctrl */
	struct nvme_queue *nvmeq = &dev->queues[qid];	/* [한국어] 이 hctx 의 하드웨어 큐 */
	struct nvme_descriptor_pools *pools;	/* [한국어] 노드 풀 포인터 */
	struct blk_mq_tags *tags;	/* [한국어] 기대 태그셋 */

	tags = qid ? dev->tagset.tags[qid - 1] : dev->admin_tagset.tags[0];	/* [한국어] I/O 는 qid-1 슬롯 */
	WARN_ON(tags != hctx->tags);	/* [한국어] blk-mq 태그/큐 불변식 */
	pools = nvme_setup_descriptor_pools(dev, hctx->numa_node);	/* [한국어] hctx NUMA 지역 풀 */
	if (IS_ERR(pools))
		return PTR_ERR(pools);	/* [한국어] 풀 생성 실패 errno */

	nvmeq->descriptor_pools = *pools;	/* [한국어] 큐에 풀 사본 캐시 — 핫패스 간접 제거 */
	hctx->driver_data = nvmeq;	/* [한국어] queue_rq/irq 가 큐를 즉시 찾음 */
	return 0;	/* [한국어] hctx 초기화 성공 */
}

/*
 * [한국어]
 * nvme_admin_init_hctx - Admin tagset 유일 hctx → qid=0
 */
static int nvme_admin_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
				unsigned int hctx_idx)	/* [한국어] 지역 변수 */
{
	WARN_ON(hctx_idx != 0);	/* [한국어] admin 은 단일 hctx */
	return nvme_init_hctx_common(hctx, data, 0);	/* [한국어] queues[0] 바인딩 */
}

/*
 * [한국어]
 * nvme_init_hctx - I/O hctx_idx → qid = hctx_idx+1 (admin 제외 번호)
 */
static int nvme_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			     unsigned int hctx_idx)	/* [한국어] 지역 변수 */
{
	return nvme_init_hctx_common(hctx, data, hctx_idx + 1);	/* [한국어] I/O qid 오프셋 */
}

/*
 * [한국어]
 * nvme_pci_init_request - tag 할당 시 iod.cmd 를 nvme_req→cmd 에 고정
 *
 * 매 요청 재설정 아님 — setup_cmd 이 동일 SQE 버퍼에 기록.
 */
static int nvme_pci_init_request(struct blk_mq_tag_set *set,
		struct request *req, unsigned int hctx_idx,	/* [한국어] 지역 변수 */
		unsigned int numa_node)	/* [한국어] 지역 변수 */
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] cmd_size=sizeof(nvme_iod) PDU */

	nvme_req(req)->ctrl = set->driver_data;	/* [한국어] core 요청에 ctrl 연결 */
	nvme_req(req)->cmd = &iod->cmd;	/* [한국어] SQE 템플릿 포인터 고정 */
	return 0;	/* [한국어] 태그 PDU 초기화 완료 */
}

/*
 * [한국어]
 * queue_irq_offset - blk-mq CPU 맵에서 admin 벡터(0) 를 건너뛸 오프셋
 */
static int queue_irq_offset(struct nvme_dev *dev)
{
	/* if we have more than 1 vec, admin queue offsets us by 1 */
	/* [한국어] 다중 벡터면 I/O 는 vector 1 부터 affinity */
	if (dev->num_vecs > 1)
		return 1;	/* [한국어] admin=0 스킵 */

	return 0;	/* [한국어] 단일 벡터 장치는 전부 0 공유 */
}

/*
 * [한국어]
 * nvme_pci_map_queues - DEFAULT/READ/POLL 맵에 큐 수·IRQ affinity 연결
 *
 * POLL 은 인터럽트 없음 → blk_mq_map_queues. 나머지는 offset 반영 hw map.
 */
static void nvme_pci_map_queues(struct blk_mq_tag_set *set)
{
	struct nvme_dev *dev = to_nvme_dev(set->driver_data);	/* [한국어] io_queues[] 보유 */
	int i, qoff, offset;	/* [한국어] 맵 인덱스 / 큐 누적 오프셋 / IRQ 오프셋 */

	offset = queue_irq_offset(dev);	/* [한국어] admin 벡터 보정 */
	for (i = 0, qoff = 0; i < set->nr_maps; i++) {	/* [한국어] DEFAULT→READ→POLL 순 */
		struct blk_mq_queue_map *map = &set->map[i];	/* [한국어] 이 유형의 CPU↔hctx 맵 */

		map->nr_queues = dev->io_queues[i];	/* [한국어] setup_irqs/calc 가 채운 큐 수 */
		if (!map->nr_queues) {	/* [한국어] 이 종류의 큐를 하나도 안 만들었다 — 매핑을 비워 두면 blk-mq 가 이 종류를 쓰지 않는다 */
			BUG_ON(i == HCTX_TYPE_DEFAULT);	/* [한국어] 기본 맵 0 은 치명 */
			continue;	/* [한국어] READ/POLL 비활성 스킵 */
		}

		/*
		 * The poll queue(s) doesn't have an IRQ (and hence IRQ
		 * affinity), so use the regular blk-mq cpu mapping
		 */
		/* [한국어] 폴링 맵은 MSI-X affinity 없이 CPU 라운드로빈 */
		map->queue_offset = qoff;	/* [한국어] 전역 hctx 번호 시작 */
		if (i != HCTX_TYPE_POLL && offset)	/* [한국어] 인터럽트 큐 + admin 오프셋 */
			blk_mq_map_hw_queues(map, dev->dev, offset);	/* [한국어] PCI IRQ affinity 반영 */
		else
			blk_mq_map_queues(map);	/* [한국어] 일반 CPU 맵 (폴 또는 단일벡터) */
		qoff += map->nr_queues;	/* [한국어] 다음 맵 큐 오프셋 누적 */
		offset += map->nr_queues;	/* [한국어] 다음 맵 IRQ 벡터 오프셋 누적 */
	}
}

/*
 * Write sq tail if we are asked to, or if the next command would wrap.
 */
/*
 * [한국어]
 * nvme_write_sq_db - SQ tail doorbell 링 (배치 최적화 포함)
 *
 * write_sq=false 이면 "다음 커맨드가 wrap 직전"이 아닐 때 MMIO 생략 —
 * 여러 SQE 를 쌓은 뒤 한 번만 울려 PCIe 트래픽 절감. dbbuf 면 이벤트 필요 시에만 writel.
 * 전제: sq_lock 보유 또는 단일 제출자.
 */
static inline void nvme_write_sq_db(struct nvme_queue *nvmeq, bool write_sq)
{
	if (!write_sq) {	/* [한국어] 배치 중 — wrap 직전이 아니면 doorbell 연기 */
		u16 next_tail = nvmeq->sq_tail + 1;	/* [한국어] 한 슬롯 더 쓸 때의 예상 tail */

		if (next_tail == nvmeq->q_depth)	/* [한국어] 링 wrap */
			next_tail = 0;	/* [한국어] 모듈러 순환 */
		if (next_tail != nvmeq->last_sq_tail)	/* [한국어] wrap 전까지 여유가 있으면 */
			return;	/* [한국어] MMIO 생략 — commit_rqs/last 가 나중에 플러시 */
	}

	if (nvme_dbbuf_update_and_check_event(nvmeq->sq_tail,
			nvmeq->dbbuf_sq_db, nvmeq->dbbuf_sq_ei))	/* [한국어] shadow 갱신; true 면 BAR 필요 */
		writel(nvmeq->sq_tail, nvmeq->q_db);	/* [한국어] SQ doorbell MMIO — 컨트롤러에 새 작업 통지 */
	nvmeq->last_sq_tail = nvmeq->sq_tail;	/* [한국어] 마지막으로 울린 값 기록 (배치 비교용) */
}

/*
 * [한국어]
 * nvme_sq_copy_cmd - 64B SQE 를 SQ 링(호스트 또는 CMB)에 복사하고 tail++
 *
 * CMB SQ 면 장치 로컬 기록으로 host→device DMA 감소. wrap 시 tail=0.
 */
static inline void nvme_sq_copy_cmd(struct nvme_queue *nvmeq,
				    struct nvme_command *cmd)
{
	memcpy(nvmeq->sq_cmds + (nvmeq->sq_tail << nvmeq->sqes),
		absolute_pointer(cmd), sizeof(*cmd));	/* [한국어] 현재 tail 슬롯에 SQE 64B 기록 */
	if (++nvmeq->sq_tail == nvmeq->q_depth)	/* [한국어] 생산자 인덱스 증가 후 wrap 검사 */
		nvmeq->sq_tail = 0;	/* [한국어] 링 처음으로 */
}

/*
 * [한국어]
 * nvme_commit_rqs - blk-mq 배치 제출 끝에서 미기록 SQ doorbell 플러시
 *
 * queue_rq 가 last=false 로 도어벨을 미룬 경우 스케줄러 종료 시 강제 울림.
 */
static void nvme_commit_rqs(struct blk_mq_hw_ctx *hctx)
{
	struct nvme_queue *nvmeq = hctx->driver_data;	/* [한국어] hctx 에 바인딩된 하드웨어 큐 */

	spin_lock(&nvmeq->sq_lock);	/* [한국어] tail 과 doorbell 원자적 관찰 */
	if (nvmeq->sq_tail != nvmeq->last_sq_tail)	/* [한국어] 아직 컨트롤러에 안 알린 SQE 존재 */
		nvme_write_sq_db(nvmeq, true);	/* [한국어] 강제 doorbell */
	spin_unlock(&nvmeq->sq_lock);	/* [한국어] SQ 임계구역 종료 */
}

/* [한국어] 데이터 경로 SGL 정책 3단: 불가 / 선택 가능 / 강제 */
enum nvme_use_sgl {
	SGL_UNSUPPORTED,	/* [한국어] admin 또는 컨트롤러 미지원 → 항상 PRP */
	SGL_SUPPORTED,		/* [한국어] 평균 세그먼트·threshold 로 선택 */
	SGL_FORCED,		/* [한국어] gap/usercmd/multi-integrity — PRP 불가 */
};

/*
 * [한국어]
 * nvme_pci_metadata_use_sgls - 메타/PI 경로에서 SGL 강제 여부
 *
 * 컨트롤러 meta SGL 지원 + (다중 integrity segment 또는 usercmd).
 * 단일 커널 MPTR 이 싸지만 usercmd 길이 검증은 SGL 이 안전.
 */
static inline bool nvme_pci_metadata_use_sgls(struct request *req)
{
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] 소속 컨트롤러 조회 */
	struct nvme_dev *dev = nvmeq->dev;	/* [한국어] Identify 능력 캐시 */

	if (!nvme_ctrl_meta_sgl_supported(&dev->ctrl))	/* [한국어] meta SGL 미지원 */
		return false;	/* [한국어] MPTR 경로 강제 */
	return req->nr_integrity_segments > 1 ||
		nvme_req(req)->flags & NVME_REQ_USERCMD;	/* [한국어] 다중/유저 → SGL */
}

/*
 * [한국어]
 * nvme_pci_use_sgls - 데이터 페이로드 PRP vs SGL 정책 판정
 *
 * SGL_FORCED: 페이지 gap·usercmd·multi integrity. SGL_SUPPORTED: threshold 선택.
 * Admin(qid=0) 또는 컨트롤러 미지원 → SGL_UNSUPPORTED (항상 PRP).
 */
static inline enum nvme_use_sgl nvme_pci_use_sgls(struct nvme_dev *dev,
		struct request *req)	/* [한국어] 지역 변수 */
{
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] 요청이 어느 큐로 갔는지는 hctx 가 알고 있다 — 요청에 따로 새기지 않는다 */

	if (nvmeq->qid && nvme_ctrl_sgl_supported(&dev->ctrl)) {	/* [한국어] I/O 큐 + SGL 능력 */
		/*
		 * When the controller is capable of using SGL, there are
		 * several conditions that we force to use it:
		 *
		 * 1. A request containing page gaps within the controller's
		 *    mask can not use the PRP format.
		 *
		 * 2. User commands use SGL because that lets the device
		 *    validate the requested transfer lengths.
		 *
		 * 3. Multiple integrity segments must use SGL as that's the
		 *    only way to describe such a command in NVMe.
		 */
		/* [한국어] PRP 불가 조건이면 강제 SGL — 정렬 갭/유저커맨드/다중 메타 */
		if (req_phys_gap_mask(req) & (NVME_CTRL_PAGE_SIZE - 1) ||
		    nvme_req(req)->flags & NVME_REQ_USERCMD ||
		    req->nr_integrity_segments > 1)
			return SGL_FORCED;	/* [한국어] PRP 포맷 불가 */
		return SGL_SUPPORTED;	/* [한국어] 선택 가능 — 평균 세그먼트 임계값 적용 */
	}

	return SGL_UNSUPPORTED;	/* [한국어] admin 또는 SGL 미지원 → PRP 전용 */
}

/*
 * [한국어]
 * nvme_pci_avg_seg_size - SGL 임계(sgl_threshold) 비교용 평균 물리 세그먼트 바이트
 *
 * coalesce 시 nseg=1 — 큰 연속 세그먼트일수록 SGL 오버헤드 대비 이득.
 */
static unsigned int nvme_pci_avg_seg_size(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] dma_state coalesce 판정 */
	unsigned int nseg;	/* [한국어] 유효 물리 세그먼트 수 */

	if (blk_rq_dma_map_coalesce(&iod->dma_state))	/* [한국어] IOVA/맵 계층이 병합함 */
		nseg = 1;	/* [한국어] 단일 논리 세그먼트 */
	else
		nseg = blk_rq_nr_phys_segments(req);	/* [한국어] bio 물리 조각 수 */
	return DIV_ROUND_UP(blk_rq_payload_bytes(req), nseg);	/* [한국어] 평균 바이트/세그먼트 */
}

/*
 * [한국어]
 * nvme_dma_pool - iod 플래그로 small(256B) 또는 large(4K) descriptor pool 선택
 */
static inline struct dma_pool *nvme_dma_pool(struct nvme_queue *nvmeq,
		struct nvme_iod *iod)
{
	if (iod->flags & IOD_SMALL_DESCRIPTOR)	/* [한국어] 짧은 PRP/SGL 리스트 */
		return nvmeq->descriptor_pools.small;	/* [한국어] 256B 풀 — 핫패스 절약 */
	return nvmeq->descriptor_pools.large;	/* [한국어] 페이지 정렬 4K 풀 */
}

/*
 * [한국어]
 * nvme_pci_cmd_use_meta_sgl - SQE flags 가 메타 SGL 세그먼트(METASEG) 형식인지
 */
static inline bool nvme_pci_cmd_use_meta_sgl(struct nvme_command *cmd)
{
	return (cmd->common.flags & NVME_CMD_SGL_ALL) == NVME_CMD_SGL_METASEG;	/* [한국어] unmap_metadata 분기 */
}

/*
 * [한국어]
 * nvme_pci_cmd_use_sgl - SQE 가 데이터 SGL(METABUF 또는 METASEG) 인지
 */
static inline bool nvme_pci_cmd_use_sgl(struct nvme_command *cmd)
{
	return cmd->common.flags &
		(NVME_CMD_SGL_METABUF | NVME_CMD_SGL_METASEG);	/* [한국어] PRP 대비 SGL dptr 해석 */
}

/*
 * [한국어]
 * nvme_pci_first_desc_dma_addr - 첫 descriptor 페이지 DMA (unmap/free 체인 시작점)
 *
 * SGL 이면 dptr.sgl.addr, PRP 면 prp2(리스트). 단일 세그먼트 단순 경로는 0 가능.
 */
static inline dma_addr_t nvme_pci_first_desc_dma_addr(struct nvme_command *cmd)
{
	if (nvme_pci_cmd_use_sgl(cmd))	/* [한국어] SGL dptr */
		return le64_to_cpu(cmd->common.dptr.sgl.addr);	/* [한국어] 인라인 또는 last-seg DMA */
	return le64_to_cpu(cmd->common.dptr.prp2);	/* [한국어] PRP 리스트 시작 (또는 2nd data) */
}

/*
 * [한국어]
 * nvme_free_descriptors - PRP/SGL 에 쓴 dma_pool 페이지들을 체인 따라 반환
 *
 * 다중 페이지 PRP 는 마지막 엔트리가 다음 리스트 DMA. free 전 next 로드 필수.
 */
static void nvme_free_descriptors(struct request *req)
{
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] pool 소유 큐 */
	const int last_prp = NVME_CTRL_PAGE_SIZE / sizeof(__le64) - 1;	/* [한국어] 링크 슬롯 인덱스 */
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] descriptors[] 배열 */
	dma_addr_t dma_addr = nvme_pci_first_desc_dma_addr(&iod->cmd);	/* [한국어] 첫 페이지 DMA (prp2 또는 sgl) */
	int i;	/* [한국어] 디스크립터 페이지 인덱스 */

	if (iod->nr_descriptors == 1) {	/* [한국어] small/large 단일 페이지 */
		dma_pool_free(nvme_dma_pool(nvmeq, iod), iod->descriptors[0],
				dma_addr);	/* [한국어] 플래그에 맞는 풀로 반환 */
		return;	/* [한국어] 체인 없음 */
	}

	for (i = 0; i < iod->nr_descriptors; i++) {	/* [한국어] 링크 체인 순회 free */
		__le64 *prp_list = iod->descriptors[i];	/* [한국어] 현재 리스트 페이지 VA */
		dma_addr_t next_dma_addr = le64_to_cpu(prp_list[last_prp]);	/* [한국어] free 전 다음 주소 확보 */

		dma_pool_free(nvmeq->descriptor_pools.large, prp_list,
				dma_addr);	/* [한국어] 다중 페이지는 large 풀 */
		dma_addr = next_dma_addr;	/* [한국어] 다음 이터레이션 대상 */
	}
}

/*
 * [한국어]
 * nvme_free_prps - non-IOVA PRP 경로: 저장된 phys 벡터 일괄 unmap + mempool 반환
 */
static void nvme_free_prps(struct request *req, unsigned int attrs)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] dma_vecs 소유 */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] DMA device·mempool */
	unsigned int i;	/* [한국어] 벡터 인덱스 */

	for (i = 0; i < iod->nr_dma_vecs; i++)	/* [한국어] prep 시 저장한 모든 물리 세그먼트 */
		dma_unmap_phys(nvmeq->dev->dev, iod->dma_vecs[i].addr,
			       iod->dma_vecs[i].len, rq_dma_dir(req), attrs);	/* [한국어] attrs 에 MMIO 등 반영 */
	mempool_free(iod->dma_vecs, nvmeq->dev->dmavec_mempool);	/* [한국어] GFP_ATOMIC 벡터 풀 반환 */
}

/*
 * [한국어]
 * nvme_free_sgls - SGL DATA_DESC 단일 또는 세그먼트 리스트 항목별 unmap_phys
 *
 * sge 가 Last Segment 면 length=리스트 바이트, sg_list 가 실제 데이터 디스크립터.
 */
static void nvme_free_sgls(struct request *req, struct nvme_sgl_desc *sge,
		struct nvme_sgl_desc *sg_list, unsigned int attrs)
{
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] DMA device */
	enum dma_data_direction dir = rq_dma_dir(req);	/* [한국어] R/W 방향 */
	unsigned int len = le32_to_cpu(sge->length);	/* [한국어] DATA=바이트, SEG=리스트 바이트 */
	struct device *dma_dev = nvmeq->dev->dev;	/* [한국어] unmap 대상 device */
	unsigned int i;	/* [한국어] 세그먼트 인덱스 */

	if (sge->type == (NVME_SGL_FMT_DATA_DESC << 4)) {	/* [한국어] 인라인/단일 데이터 디스크립터 */
		dma_unmap_phys(dma_dev, le64_to_cpu(sge->addr), len, dir,
			       attrs);	/* [한국어] 한 버퍼 unmap */
		return;	/* [한국어] 리스트 순회 불필요 */
	}

	for (i = 0; i < len / sizeof(*sg_list); i++)	/* [한국어] last-seg 가 가리킨 엔트리 수 */
		dma_unmap_phys(dma_dev, le64_to_cpu(sg_list[i].addr),
			le32_to_cpu(sg_list[i].length), dir, attrs);	/* [한국어] 각 데이터 블록 unmap */
}

/*
 * [한국어]
 * nvme_unmap_metadata - integrity/메타 버퍼 DMA 해제
 *
 * SINGLE_META_SEGMENT→unmap_page. P2P/MMIO attrs 반영 후 iterator unmap 또는
 * free_sgls/phys. small pool meta descriptor 반환.
 */
static void nvme_unmap_metadata(struct request *req)
{
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] 메타데이터 풀도 큐(=NUMA 노드)마다 다르므로 큐를 먼저 찾는다 */
	enum pci_p2pdma_map_type map = PCI_P2PDMA_MAP_NONE;	/* [한국어] 기본 호스트 DMA unmap */
	enum dma_data_direction dir = rq_dma_dir(req);	/* [한국어] 읽기/쓰기 방향 */
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] 매핑 상태가 요청 뒤에 붙어 있다 */
	struct device *dma_dev = nvmeq->dev->dev;	/* [한국어] 지역 변수 */
	struct nvme_sgl_desc *sge = iod->meta_descriptor;	/* [한국어] meta SGL 리스트(있을 때) */
	unsigned int attrs = 0;	/* [한국어] 지역 변수 */

	if (iod->flags & IOD_SINGLE_META_SEGMENT) {	/* [한국어] 단일 bvec MPTR 경로 */
		dma_unmap_page(dma_dev, iod->meta_dma,
			       rq_integrity_vec(req).bv_len,
			       rq_dma_dir(req));	/* [한국어] 페이지 단위 unmap */
		return;	/* [한국어] 단일 세그먼트 메타데이터는 풀도 서술자도 쓰지 않아 여기서 끝난다 */
	}

	if (iod->flags & IOD_META_P2P)	/* [한국어] 메타가 피어 버스 주소 */
		map = PCI_P2PDMA_MAP_BUS_ADDR;	/* [한국어] 메타 P2P bus addr */
	else if (iod->flags & IOD_META_MMIO) {	/* [한국어] 메타 host-bridge 경유 */
		map = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;	/* [한국어] 메타 host-bridge MMIO */
		attrs |= DMA_ATTR_MMIO;	/* [한국어] unmap 속성 정합 */
	}

	if (!blk_rq_dma_unmap(req, dma_dev, &iod->meta_dma_state,
			      iod->meta_total_len, map)) {	/* [한국어] IOVA 아니면 수동 unmap */
		if (nvme_pci_cmd_use_meta_sgl(&iod->cmd))	/* [한국어] meta SGL 형식 */
			nvme_free_sgls(req, sge, &sge[1], attrs);	/* [한국어] 세그먼트 리스트 unmap */
		else
			dma_unmap_phys(dma_dev, iod->meta_dma,
				       iod->meta_total_len, dir, attrs);	/* [한국어] MPTR phys unmap */
	}

	if (iod->meta_descriptor)	/* [한국어] small pool 에 빌린 meta desc 있음 */
		dma_pool_free(nvmeq->descriptor_pools.small,
			      iod->meta_descriptor, iod->meta_dma);	/* [한국어] small pool 디스크립터 반환 */
}

/*
 * [한국어]
 * nvme_unmap_data - 데이터 페이로드 DMA 해제 (완료·에러 공통)
 *
 * 단일 세그먼트 고속 경로 vs iterator 경로. P2P/MMIO 플래그로 unmap 속성 정합.
 * descriptor 페이지는 마지막에 free. 호출: complete / prep 실패 롤백.
 */
static void nvme_unmap_data(struct request *req)
{
	enum pci_p2pdma_map_type map = PCI_P2PDMA_MAP_NONE;	/* [한국어] 기본=일반 호스트 DMA */
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] flags/dptr/descriptors */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] pool·device */
	struct device *dma_dev = nvmeq->dev->dev;	/* [한국어] unmap 대상 */
	unsigned int attrs = 0;	/* [한국어] DMA_ATTR_MMIO 등 */

	if (iod->flags & IOD_SINGLE_SEGMENT) {	/* [한국어] simple 경로 — descriptor 없음 */
		static_assert(offsetof(union nvme_data_ptr, prp1) ==
				offsetof(union nvme_data_ptr, sgl.addr));	/* [한국어] prp1/sgl.addr 동위치 */
		dma_unmap_page(dma_dev, le64_to_cpu(iod->cmd.common.dptr.prp1),
				iod->total_len, rq_dma_dir(req));	/* [한국어] 단일 페이지 unmap */
		return;	/* [한국어] descriptor/풀 없음 — 조기 종료 */
	}

	if (iod->flags & IOD_DATA_P2P)	/* [한국어] 데이터 P2P bus addr */
		map = PCI_P2PDMA_MAP_BUS_ADDR;	/* [한국어] P2P bus addr unmap 경로 */
	else if (iod->flags & IOD_DATA_MMIO) {	/* [한국어] 데이터 host-bridge MMIO */
		map = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;	/* [한국어] host-bridge MMIO */
		attrs |= DMA_ATTR_MMIO;	/* [한국어] unmap 시 MMIO 속성 전달 */
	}

	if (!blk_rq_dma_unmap(req, dma_dev, &iod->dma_state, iod->total_len,
			      map)) {	/* [한국어] IOVA 경로가 아니면 false → 수동 unmap */
		if (nvme_pci_cmd_use_sgl(&iod->cmd))	/* [한국어] SGL dptr 형식 */
			nvme_free_sgls(req, &iod->cmd.common.dptr.sgl,
			               iod->descriptors[0], attrs);	/* [한국어] SGL 엔트리별 unmap */
		else
			nvme_free_prps(req, attrs);	/* [한국어] 저장된 dma_vec 일괄 unmap */
	}

	if (iod->nr_descriptors)	/* [한국어] PRP list/SGL 페이지를 빌렸으면 */
		nvme_free_descriptors(req);	/* [한국어] pool 페이지 체인 반환 */
}

/*
 * [한국어]
 * nvme_pci_prp_save_mapping - unmap 을 위해 DMA 주소/길이를 iod 벡터에 기록
 *
 * IOVA 또는 need_unmap=false 면 저장 불필요. GFP_ATOMIC mempool — 제출 경로.
 */
static bool nvme_pci_prp_save_mapping(struct request *req,
				      struct device *dma_dev,	/* [한국어] 지역 변수 */
				      struct blk_dma_iter *iter)	/* [한국어] 지역 변수 */
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] dma_vecs 소유 PDU */

	if (dma_use_iova(&iod->dma_state) || !dma_need_unmap(dma_dev))	/* [한국어] IOVA 또는 unmap 불필요 플랫폼 */
		return true;	/* [한국어] 벡터 저장 생략 — unmap_data 가 IOVA 경로 */

	if (!iod->nr_dma_vecs) {	/* [한국어] 첫 세그먼트 — 벡터 배열 대여 */
		struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] mempool 소유 dev */

		iod->dma_vecs = mempool_alloc(nvmeq->dev->dmavec_mempool,
				GFP_ATOMIC);	/* [한국어] 제출 핫패스 — atomic */
		if (!iod->dma_vecs) {	/* [한국어] 핫패스라 GFP_ATOMIC 이고, 실패는 흔한 일이므로 재시도 가능한 오류로 올린다 */
			iter->status = BLK_STS_RESOURCE;	/* [한국어] 풀 고갈 — 재시도 가능 */
			return false;	/* [한국어] 매핑 중단 */
		}
	}

	iod->dma_vecs[iod->nr_dma_vecs].addr = iter->addr;	/* [한국어] unmap 용 DMA 주소 */
	iod->dma_vecs[iod->nr_dma_vecs].len = iter->len;	/* [한국어] unmap 용 길이 */
	iod->nr_dma_vecs++;	/* [한국어] 유효 벡터 개수 증가 */
	return true;	/* [한국어] 다음 PRP 엔트리 진행 가능 */
}

/*
 * [한국어]
 * nvme_pci_prp_iter_next - DMA iterator 다음 청크 + unmap 용 매핑 저장
 */
static bool nvme_pci_prp_iter_next(struct request *req, struct device *dma_dev,
		struct blk_dma_iter *iter)	/* [한국어] 지역 변수 */
{
	if (iter->len)	/* [한국어] 현재 청크 잔여 바이트 있음 */
		return true;	/* [한국어] 동일 물리 세그먼트 계속 */
	if (!blk_rq_dma_map_iter_next(req, dma_dev, iter))	/* [한국어] 다음 phys 세그먼트 맵 */
		return false;	/* [한국어] 끝 또는 맵 실패 */
	return nvme_pci_prp_save_mapping(req, dma_dev, iter);	/* [한국어] 새 매핑 기록 */
}

/*
 * [한국어]
 * nvme_pci_setup_data_prp - 요청 페이로드를 NVMe PRP1/PRP2/PRP list 로 기술
 *
 * PRP1: 첫 DMA(비정렬 가능). 잔여 ≤1페이지면 PRP2=데이터, 아니면 PRP2=리스트.
 * 리스트는 small/large pool. 페이지 가득 차면 링크 엔트리로 다음 리스트 연결.
 * 실패 시에도 dptr 를 채워 unmap_data 가 tear-down 가능하게 함.
 * 호출 체인: nvme_map_data → [여기]
 */
static blk_status_t nvme_pci_setup_data_prp(struct request *req,
		struct blk_dma_iter *iter)	/* [한국어] 지역 변수 */
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] dptr/descriptor 상태 */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] 서술자 풀이 큐마다 있어 큐를 먼저 찾는다 */
	unsigned int length = blk_rq_payload_bytes(req);	/* [한국어] 남은 전송 바이트 */
	dma_addr_t prp1_dma, prp2_dma = 0;	/* [한국어] SQE dptr 필드에 기록할 DMA 주소 */
	unsigned int prp_len, i;	/* [한국어] 이번 청크 길이 / 리스트 슬롯 인덱스 */
	__le64 *prp_list;	/* [한국어] 현재 PRP 리스트 페이지 VA */

	if (!nvme_pci_prp_save_mapping(req, nvmeq->dev->dev, iter))	/* [한국어] unmap 용 첫 매핑 저장 */
		return iter->status;	/* [한국어] mempool 실패 등 */

	/*
	 * PRP1 always points to the start of the DMA transfers.
	 *
	 * This is the only PRP (except for the list entries) that could be
	 * non-aligned.
	 */
	/* [한국어] PRP1 만 페이지 중간 offset 허용 — 이후 엔트리는 페이지 정렬 */
	prp1_dma = iter->addr;	/* [한국어] 전송 시작 DMA */
	prp_len = min(length, NVME_CTRL_PAGE_SIZE -
			(iter->addr & (NVME_CTRL_PAGE_SIZE - 1)));	/* [한국어] 첫 페이지 잔여 */
	iod->total_len += prp_len;	/* [한국어] 매핑 누적 */
	iter->addr += prp_len;	/* [한국어] iterator 전진 */
	iter->len -= prp_len;	/* [한국어] 현재 phys 세그먼트 잔여 감소 */
	length -= prp_len;	/* [한국어] 남은 페이로드 */
	if (!length)
		goto done;	/* [한국어] 단일 PRP1 로 끝 — prp2=0 */

	if (!nvme_pci_prp_iter_next(req, nvmeq->dev->dev, iter)) {	/* [한국어] 다음 데이터 DMA 청크 */
		if (WARN_ON_ONCE(!iter->status))	/* [한국어] 끝인데 status 없음=형 오류 */
			goto bad_sgl;	/* [한국어] 비정상 SGL/PRP 구성 */
		goto done;	/* [한국어] 맵 에러 — dptr 채운 뒤 unmap */
	}

	/*
	 * PRP2 is usually a list, but can point to data if all data to be
	 * transferred fits into PRP1 + PRP2:
	 */
	/* [한국어] 잔여 ≤1 페이지면 PRP2 가 데이터 직주소 (리스트 불필요) */
	if (length <= NVME_CTRL_PAGE_SIZE) {	/* [한국어] 한 페이지 안에 끝나면 PRP2 에 다음 페이지 주소만 넣으면 되고 리스트가 필요 없다 */
		prp2_dma = iter->addr;	/* [한국어] 두 번째 데이터 페이지 DMA */
		iod->total_len += length;	/* [한국어] 잔여 전부 누적 */
		goto done;	/* [한국어] PRP1+PRP2 로 완료 */
	}

	if (DIV_ROUND_UP(length, NVME_CTRL_PAGE_SIZE) <=
	    NVME_SMALL_POOL_SIZE / sizeof(__le64))	/* [한국어] small 풀로 리스트 수용 가능 */
		iod->flags |= IOD_SMALL_DESCRIPTOR;	/* [한국어] free 시 small pool 경로 */

	prp_list = dma_pool_alloc(nvme_dma_pool(nvmeq, iod), GFP_ATOMIC,
			&prp2_dma);	/* [한국어] PRP2=리스트 DMA 주소 */
	if (!prp_list) {	/* [한국어] 리스트를 못 잡았다 — 이미 매핑한 것은 호출자가 되돌린다 */
		iter->status = BLK_STS_RESOURCE;	/* [한국어] 디스크립터 풀 고갈 */
		goto done;	/* [한국어] 부분 dptr 로 unmap 가능하게 */
	}
	iod->descriptors[iod->nr_descriptors++] = prp_list;	/* [한국어] free_descriptors 추적 */

	i = 0;	/* [한국어] 현재 리스트 페이지 슬롯 */
	for (;;) {	/* [한국어] 잔여 페이로드를 PRP 엔트리로 채움 */
		prp_list[i++] = cpu_to_le64(iter->addr);	/* [한국어] 정렬된 데이터 페이지 DMA */
		prp_len = min(length, NVME_CTRL_PAGE_SIZE);	/* [한국어] 보통 풀 페이지 */
		if (WARN_ON_ONCE(iter->len < prp_len))	/* [한국어] iterator 길이 불일치 */
			goto bad_sgl;	/* [한국어] 요청 형성 오류 */

		iod->total_len += prp_len;	/* [한국어] 매핑 누적 */
		iter->addr += prp_len;	/* [한국어] 청크 전진 */
		iter->len -= prp_len;	/* [한국어] phys 세그먼트 잔여 */
		length -= prp_len;	/* [한국어] 전체 잔여 */
		if (!length)
			break;	/* [한국어] 페이로드 소진 — 리스트 완성 */

		if (!nvme_pci_prp_iter_next(req, nvmeq->dev->dev, iter)) {	/* [한국어] 다음 청크 */
			if (WARN_ON_ONCE(!iter->status))
				goto bad_sgl;	/* [한국어] 조기 종료 이상 */
			goto done;	/* [한국어] 에러 status 유지 후 unmap */
		}

		/*
		 * If we've filled the entire descriptor, allocate a new that is
		 * pointed to be the last entry in the previous PRP list.  To
		 * accommodate for that move the last actual entry to the new
		 * descriptor.
		 */
		/* [한국어] 페이지 가득: 마지막 실데이터 엔트리를 새 페이지로 옮기고 링크 삽입 */
		if (i == NVME_CTRL_PAGE_SIZE >> 3) {	/* [한국어] 512 슬롯 가득 (마지막은 링크) */
			__le64 *old_prp_list = prp_list;	/* [한국어] 이전 리스트 페이지 */
			dma_addr_t prp_list_dma;	/* [한국어] 새 리스트 DMA */

			prp_list = dma_pool_alloc(nvmeq->descriptor_pools.large,
					GFP_ATOMIC, &prp_list_dma);	/* [한국어] 다음 4K 리스트 */
			if (!prp_list) {	/* [한국어] 체인을 이어 갈 페이지가 없다 */
				iter->status = BLK_STS_RESOURCE;	/* [한국어] 추가 페이지 실패 */
				goto done;	/* [한국어] 여기까지 채운 것은 정리 경로가 되돌린다 */
			}
			iod->descriptors[iod->nr_descriptors++] = prp_list;	/* [한국어] 체인 추적 */

			prp_list[0] = old_prp_list[i - 1];	/* [한국어] 마지막 데이터 엔트리 이사 */
			old_prp_list[i - 1] = cpu_to_le64(prp_list_dma);	/* [한국어] 링크=다음 리스트 DMA */
			i = 1;	/* [한국어] 새 페이지는 슬롯1 부터 채움 */
		}
	}

done:
	/*
	 * nvme_unmap_data uses the DPT field in the SQE to tear down the
	 * mapping, so initialize it even for failures.
	 */
	/* [한국어] 실패 시에도 dptr 필요 — unmap_data 가 prp1/prp2 로 tear-down */
	iod->cmd.common.dptr.prp1 = cpu_to_le64(prp1_dma);	/* [한국어] SQE PRP1 */
	iod->cmd.common.dptr.prp2 = cpu_to_le64(prp2_dma);	/* [한국어] SQE PRP2 또는 리스트 DMA */
	if (unlikely(iter->status))	/* [한국어] 맵/풀 실패 */
		nvme_unmap_data(req);	/* [한국어] 부분 매핑 즉시 해제 */
	return iter->status;	/* [한국어] BLK_STS_OK 또는 에러 */

bad_sgl:
	dev_err_once(nvmeq->dev->dev,
		"Incorrectly formed request for payload:%d nents:%d\n",
		blk_rq_payload_bytes(req), blk_rq_nr_phys_segments(req));	/* [한국어] 드라이버/블록 계층 버그 단서 */
	return BLK_STS_IOERR;	/* [한국어] 재시도 무의미한 형식 오류 */
}

/*
 * [한국어]
 * nvme_pci_sgl_set_data - SGL Data Block descriptor 한 칸 채움 (addr/len/type)
 */
static void nvme_pci_sgl_set_data(struct nvme_sgl_desc *sge,
		struct blk_dma_iter *iter)	/* [한국어] 지역 변수 */
{
	sge->addr = cpu_to_le64(iter->addr);	/* [한국어] 데이터 버퍼 DMA */
	sge->length = cpu_to_le32(iter->len);	/* [한국어] 바이트 길이 — 명시적 */
	sge->type = NVME_SGL_FMT_DATA_DESC << 4;	/* [한국어] Data Block 디스크립터 타입 */
}

/*
 * [한국어]
 * nvme_pci_sgl_set_seg - Last Segment descriptor (SQE dptr 또는 meta 가 가리킴)
 */
static void nvme_pci_sgl_set_seg(struct nvme_sgl_desc *sge,
		dma_addr_t dma_addr, int entries)	/* [한국어] 지역 변수 */
{
	sge->addr = cpu_to_le64(dma_addr);	/* [한국어] 데이터 디스크립터 리스트 DMA */
	sge->length = cpu_to_le32(entries * sizeof(*sge));	/* [한국어] 리스트 바이트=엔트리×16 */
	sge->type = NVME_SGL_FMT_LAST_SEG_DESC << 4;	/* [한국어] Last Segment — 다음 체인 없음 */
}

/*
 * [한국어]
 * nvme_pci_setup_data_sgl - 데이터 SGL 구성. flags=SGL_METABUF
 *
 * 1 entry 또는 coalesce 면 SQE 인라인 DATA_DESC. 아니면 pool 리스트 + SEG.
 */
static blk_status_t nvme_pci_setup_data_sgl(struct request *req,
		struct blk_dma_iter *iter)	/* [한국어] 지역 변수 */
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] dptr/flags 기록 대상 */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] descriptor pool 소스 */
	unsigned int entries = blk_rq_nr_phys_segments(req);	/* [한국어] 물리 세그먼트 수 */
	struct nvme_sgl_desc *sg_list;	/* [한국어] 세그먼트 리스트 VA */
	dma_addr_t sgl_dma;	/* [한국어] 리스트 페이지 DMA — SQE dptr 에 기록 */
	unsigned int mapped = 0;	/* [한국어] 채운 데이터 디스크립터 개수 */

	/* set the transfer type as SGL */
	/* [한국어] 컨트롤러가 dptr 를 SGL 로 해석하도록 flags 설정 */
	iod->cmd.common.flags = NVME_CMD_SGL_METABUF;	/* [한국어] SQE 가 SGL 형식임을 표시 */

	if (entries == 1 || blk_rq_dma_map_coalesce(&iod->dma_state)) {	/* [한국어] 단일/병합 세그먼트 */
		nvme_pci_sgl_set_data(&iod->cmd.common.dptr.sgl, iter);	/* [한국어] 인라인 단일 DATA_DESC */
		iod->total_len += iter->len;	/* [한국어] unmap 길이 기록 */
		return BLK_STS_OK;	/* [한국어] pool 리스트 없이 완료 */
	}

	if (entries <= NVME_SMALL_POOL_SIZE / sizeof(*sg_list))	/* [한국어] 256B 풀 수용 범위 */
		iod->flags |= IOD_SMALL_DESCRIPTOR;	/* [한국어] 짧은 리스트 → small pool */

	sg_list = dma_pool_alloc(nvme_dma_pool(nvmeq, iod), GFP_ATOMIC,
			&sgl_dma);	/* [한국어] 제출 경로 — atomic 할당 */
	if (!sg_list)
		return BLK_STS_RESOURCE;	/* [한국어] 풀 고갈 — 재시도 가능 */
	iod->descriptors[iod->nr_descriptors++] = sg_list;	/* [한국어] free 시 추적 */

	do {	/* [한국어] 최소 한 번은 돈다 — 세그먼트가 0 인 요청은 여기 오지 않는다 */
		if (WARN_ON_ONCE(mapped == entries)) {	/* [한국어] 세그먼트 수 불일치 방어 */
			iter->status = BLK_STS_IOERR;	/* [한국어] 세는 개수와 실제가 어긋났다 — 재시도해도 같으므로 I/O 오류다 */
			break;
		}
		nvme_pci_sgl_set_data(&sg_list[mapped++], iter);	/* [한국어] 데이터 블록 디스크립터 */
		iod->total_len += iter->len;	/* [한국어] 완료 시 언매핑할 총 길이를 누적한다 */
	} while (blk_rq_dma_map_iter_next(req, nvmeq->dev->dev, iter));	/* [한국어] 다음 물리 세그먼트 */

	nvme_pci_sgl_set_seg(&iod->cmd.common.dptr.sgl, sgl_dma, mapped);	/* [한국어] SQE 가 last-seg 가리킴 */
	if (unlikely(iter->status))
		nvme_unmap_data(req);	/* [한국어] 부분 실패 tear-down */
	return iter->status;	/* [한국어] 성공이면 0, 실패면 위에서 이미 언매핑까지 마쳤다 */
}

/*
 * [한국어]
 * nvme_pci_setup_data_simple - 단일 phys segment 고속 경로 (iterator 생략)
 *
 * PRP 로 최대 2 페이지 span. P2P 는 AGAIN→iterator. IOD_SINGLE_SEGMENT.
 */
/*
 * [한국어]
 * nvme_pci_setup_data_simple - 단일 phys 세그먼트 고속 경로 (iterator 생략)
 *
 * PRP 로 최대 2 페이지 span 가능해야 함. P2P 는 iterator 경로(BLK_STS_AGAIN).
 * dma_map_bvec 1회 + IOD_SINGLE_SEGMENT.
 */
static blk_status_t nvme_pci_setup_data_simple(struct request *req,
		enum nvme_use_sgl use_sgl)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] dptr/flags */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] DMA device */
	struct bio_vec bv = req_bvec(req);	/* [한국어] 유일한 물리 세그먼트 */
	unsigned int prp1_offset = bv.bv_offset & (NVME_CTRL_PAGE_SIZE - 1);	/* [한국어] 페이지 내 시작 offset */
	bool prp_possible = prp1_offset + bv.bv_len <= NVME_CTRL_PAGE_SIZE * 2;	/* [한국어] PRP1+PRP2 로 커버 가능? */
	dma_addr_t dma_addr;	/* [한국어] 단일 맵 결과 */

	if (!use_sgl && !prp_possible)	/* [한국어] SGL 불가·PRP 도 불능 */
		return BLK_STS_AGAIN;	/* [한국어] PRP 불가·SGL 미지원 → 상위 폴백 */
	if (is_pci_p2pdma_page(bv.bv_page))	/* [한국어] P2P 페이지는 속성 필요 */
		return BLK_STS_AGAIN;	/* [한국어] P2P 는 iterator 경로 필요 */

	dma_addr = dma_map_bvec(nvmeq->dev->dev, &bv, rq_dma_dir(req), 0);	/* [한국어] 단일 bvec 맵 */
	if (dma_mapping_error(nvmeq->dev->dev, dma_addr))
		return BLK_STS_RESOURCE;	/* [한국어] 맵 실패 — 재시도 가능 */
	iod->total_len = bv.bv_len;	/* [한국어] unmap 길이 */
	iod->flags |= IOD_SINGLE_SEGMENT;	/* [한국어] unmap_page 경로 */

	if (use_sgl == SGL_FORCED || !prp_possible) {	/* [한국어] SGL 인라인 데이터 디스크립터 */
		iod->cmd.common.flags = NVME_CMD_SGL_METABUF;	/* [한국어] SQE SGL 형식 */
		iod->cmd.common.dptr.sgl.addr = cpu_to_le64(dma_addr);	/* [한국어] 데이터 DMA */
		iod->cmd.common.dptr.sgl.length = cpu_to_le32(bv.bv_len);	/* [한국어] 명시 길이 */
		iod->cmd.common.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;	/* [한국어] 인라인 SGL */
	} else {
		unsigned int first_prp_len = NVME_CTRL_PAGE_SIZE - prp1_offset;	/* [한국어] PRP1 이 커버하는 바이트 */

		iod->cmd.common.dptr.prp1 = cpu_to_le64(dma_addr);	/* [한국어] 첫 페이지 (비정렬 가능) */
		iod->cmd.common.dptr.prp2 = 0;	/* [한국어] 한 페이지 이내면 prp2 불필요 */
		if (bv.bv_len > first_prp_len)	/* [한국어] 두 번째 페이지 필요 */
			iod->cmd.common.dptr.prp2 =
				cpu_to_le64(dma_addr + first_prp_len);	/* [한국어] 다음 페이지 직주소 */
	}

	return BLK_STS_OK;	/* [한국어] 고속 경로 매핑 완료 */
}

/*
 * [한국어]
 * nvme_map_data - 요청 데이터 버퍼 전체를 NVMe dptr 로 매핑하는 총괄
 *
 * 1) single segment simple 시도 2) dma_map_iter_start + P2P 플래그
 * 3) SGL 또는 PRP setup. prep_rq 의 핵심 비용 구간.
 */
static blk_status_t nvme_map_data(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] dptr/플래그 기록 대상 */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] 큐·device */
	struct nvme_dev *dev = nvmeq->dev;	/* [한국어] SGL 능력·DMA device */
	enum nvme_use_sgl use_sgl = nvme_pci_use_sgls(dev, req);	/* [한국어] PRP/SGL 정책 */
	struct blk_dma_iter iter;	/* [한국어] 물리 세그먼트 DMA iterator */
	blk_status_t ret;	/* [한국어] simple 경로 결과 */

	/*
	 * Try to skip the DMA iterator for single segment requests, as that
	 * significantly improves performances for small I/O sizes.
	 */
	/* [한국어] 단일 세그먼트 고속 경로 — 작은 I/O 핫패스 최적화 */
	if (blk_rq_nr_phys_segments(req) == 1) {	/* [한국어] phys nseg=1 이면 simple 시도 */
		ret = nvme_pci_setup_data_simple(req, use_sgl);	/* [한국어] dma_map_bvec 1회 */
		if (ret != BLK_STS_AGAIN)	/* [한국어] AGAIN 이면 iterator 폴백 (P2P 등) */
			return ret;	/* [한국어] OK 또는 영구 에러 */
	}

	if (!blk_rq_dma_map_iter_start(req, dev->dev, &iod->dma_state, &iter))	/* [한국어] 전체 페이로드 맵 시작 */
		return iter.status;	/* [한국어] 맵 실패 상태 */

	switch (iter.p2pdma.map) {	/* [한국어] P2P 유형 → iod 플래그 */
	case PCI_P2PDMA_MAP_BUS_ADDR:	/* [한국어] 피어 버스 주소 직접 */
		iod->flags |= IOD_DATA_P2P;	/* [한국어] unmap 시 P2P 속성 */
		break;
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:	/* [한국어] 호스트 브리지 경유 MMIO */
		iod->flags |= IOD_DATA_MMIO;	/* [한국어] unmap 시 MMIO attrs */
		break;
	case PCI_P2PDMA_MAP_NONE:	/* [한국어] 일반 호스트 메모리 */
		break;
	default:
		return BLK_STS_RESOURCE;	/* [한국어] 미지원 맵 유형 */
	}

	if (use_sgl == SGL_FORCED ||
	    (use_sgl == SGL_SUPPORTED &&
	     (sgl_threshold && nvme_pci_avg_seg_size(req) >= sgl_threshold)))	/* [한국어] 강제 또는 임계값 충족 */
		return nvme_pci_setup_data_sgl(req, &iter);	/* [한국어] SGL dptr */
	return nvme_pci_setup_data_prp(req, &iter);	/* [한국어] 전통 PRP1/2/list */
}

/*
 * [한국어]
 * nvme_pci_setup_meta_iter - integrity/메타 DMA iterator 로 MPTR 또는 meta SGL
 *
 * 커널 단일 segment+비 usercmd 는 효율적 MPTR. usercmd/다중/P2P 는 명시 길이
 * SGL_METASEG. small pool 에 세그먼트 리스트.
 */
static blk_status_t nvme_pci_setup_meta_iter(struct request *req)
{
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] 풀·device 소스 */
	unsigned int entries = req->nr_integrity_segments;	/* [한국어] 메타 물리 세그먼트 수 */
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] metadata 필드·플래그 */
	struct nvme_dev *dev = nvmeq->dev;	/* [한국어] DMA 장치 */
	struct nvme_sgl_desc *sg_list;	/* [한국어] meta SGL 리스트 VA */
	struct blk_dma_iter iter;	/* [한국어] integrity DMA iterator */
	dma_addr_t sgl_dma;	/* [한국어] 리스트 DMA — SQE metadata 에 기록 */
	int i = 0;	/* [한국어] 데이터 디스크립터 채움 인덱스 */

	if (!blk_rq_integrity_dma_map_iter_start(req, dev->dev,
						&iod->meta_dma_state, &iter))	/* [한국어] 메타 버퍼 맵 시작 */
		return iter.status;	/* [한국어] 맵 실패 상태 */

	switch (iter.p2pdma.map) {	/* [한국어] P2P 유형 → unmap attrs */
	case PCI_P2PDMA_MAP_BUS_ADDR:	/* [한국어] 피어 버스 주소 */
		iod->flags |= IOD_META_P2P;	/* [한국어] unmap 시 P2P 경로 */
		break;
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:	/* [한국어] 호스트 브리지 MMIO */
		iod->flags |= IOD_META_MMIO;	/* [한국어] DMA_ATTR_MMIO */
		break;
	case PCI_P2PDMA_MAP_NONE:	/* [한국어] 일반 호스트 메모리 */
		break;
	default:
		return BLK_STS_RESOURCE;	/* [한국어] 미지원 맵 유형 */
	}

	if (blk_rq_dma_map_coalesce(&iod->meta_dma_state))	/* [한국어] coalesce 되면 단일 세그먼트 */
		entries = 1;	/* [한국어] MPTR/단일 SGL 후보 */

	/*
	 * The NVMe MPTR descriptor has an implicit length that the host and
	 * device must agree on to avoid data/memory corruption. We trust the
	 * kernel allocated correctly based on the format's parameters, so use
	 * the more efficient MPTR to avoid extra dma pool allocations for the
	 * SGL indirection.
	 *
	 * But for user commands, we don't necessarily know what they do, so
	 * the driver can't validate the metadata buffer size. The SGL
	 * descriptor provides an explicit length, so we're relying on that
	 * mechanism to catch any misunderstandings between the application and
	 * device.
	 *
	 * P2P DMA also needs to use the blk_dma_iter method, so mptr setup
	 * leverages this routine when that happens.
	 */
	/* [한국어] MPTR=암시 길이(커널 PI) vs SGL=명시 길이(usercmd 검증) */
	if (!nvme_ctrl_meta_sgl_supported(&dev->ctrl) ||
	    (entries == 1 && !(nvme_req(req)->flags & NVME_REQ_USERCMD))) {	/* [한국어] 효율 MPTR 경로 */
		iod->cmd.common.metadata = cpu_to_le64(iter.addr);	/* [한국어] SQE MPTR 필드 */
		iod->meta_total_len = iter.len;	/* [한국어] unmap 길이 */
		iod->meta_dma = iter.addr;	/* [한국어] unmap 주소 */
		iod->meta_descriptor = NULL;	/* [한국어] pool 디스크립터 없음 */
		return BLK_STS_OK;	/* [한국어] 메타 매핑 완료 */
	}

	sg_list = dma_pool_alloc(nvmeq->descriptor_pools.small, GFP_ATOMIC,
			&sgl_dma);	/* [한국어] meta last-seg+data 디스크립터 페이지 */
	if (!sg_list)
		return BLK_STS_RESOURCE;	/* [한국어] 풀 고갈 — 재시도 가능 */

	iod->meta_descriptor = sg_list;	/* [한국어] free 시 추적 */
	iod->meta_dma = sgl_dma;	/* [한국어] 리스트 DMA */
	iod->cmd.common.flags = NVME_CMD_SGL_METASEG;	/* [한국어] SQE 메타가 SGL 세그먼트 형식 */
	iod->cmd.common.metadata = cpu_to_le64(sgl_dma);	/* [한국어] metadata=세그먼트 리스트 DMA */
	if (entries == 1) {	/* [한국어] 단일 데이터 디스크립터 + last-seg 인라인 가능 */
		iod->meta_total_len = iter.len;	/* [한국어] 단일 길이 */
		nvme_pci_sgl_set_data(sg_list, &iter);	/* [한국어] 첫 칸 DATA_DESC */
		return BLK_STS_OK;	/* [한국어] 메타데이터가 한 조각이면 서술자 하나로 끝난다 — 리스트가 필요 없다 */
	}

	sgl_dma += sizeof(*sg_list);	/* [한국어] 데이터 엔트리는 last-seg 다음부터 */
	do {	/* [한국어] 두 조각 이상이라 SGL 리스트를 채운다 */
		nvme_pci_sgl_set_data(&sg_list[++i], &iter);	/* [한국어] 데이터 블록 디스크립터 채움 */
		iod->meta_total_len += iter.len;	/* [한국어] 총 메타 바이트 누적 */
	} while (blk_rq_integrity_dma_map_iter_next(req, dev->dev, &iter));	/* [한국어] 다음 integrity 세그먼트 */

	nvme_pci_sgl_set_seg(sg_list, sgl_dma, i);	/* [한국어] 슬롯0=Last Segment → 데이터 리스트 */
	if (unlikely(iter.status))	/* [한국어] 순회 중 맵 실패 */
		nvme_unmap_metadata(req);	/* [한국어] 부분 매핑 tear-down */
	return iter.status;	/* [한국어] OK 또는 에러 상태 */
}

/*
 * [한국어]
 * nvme_pci_setup_meta_mptr - 단일 integrity bvec 를 MPTR 로 매핑
 *
 * P2P 페이지는 iter 경로 위임. 성공 시 IOD_SINGLE_META_SEGMENT.
 */
static blk_status_t nvme_pci_setup_meta_mptr(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] metadata/flags */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] DMA device */
	struct bio_vec bv = rq_integrity_vec(req);	/* [한국어] 유일 integrity bvec */

	if (is_pci_p2pdma_page(bv.bv_page))	/* [한국어] P2P 는 iterator/속성 필요 */
		return nvme_pci_setup_meta_iter(req);	/* [한국어] P2P 플래그 경로 위임 */

	iod->meta_dma = dma_map_bvec(nvmeq->dev->dev, &bv, rq_dma_dir(req), 0);	/* [한국어] 단일 페이지 맵 */
	if (dma_mapping_error(nvmeq->dev->dev, iod->meta_dma))
		return BLK_STS_IOERR;	/* [한국어] 맵 실패 — 영구/즉시 오류 */
	iod->cmd.common.metadata = cpu_to_le64(iod->meta_dma);	/* [한국어] SQE MPTR */
	iod->flags |= IOD_SINGLE_META_SEGMENT;	/* [한국어] unmap_page 경로 */
	return BLK_STS_OK;	/* [한국어] 메타 준비 완료 */
}

/*
 * [한국어]
 * nvme_map_metadata - PI/메타 매핑 엔트리. 데이터 SGL 이면 meta SGL 정책 연동
 */
static blk_status_t nvme_map_metadata(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] cmd flags 검사 */

	if ((iod->cmd.common.flags & NVME_CMD_SGL_METABUF) &&
	    nvme_pci_metadata_use_sgls(req))	/* [한국어] 데이터 SGL+메타 SGL 강제 조건 */
		return nvme_pci_setup_meta_iter(req);	/* [한국어] meta SGL/iter */
	return nvme_pci_setup_meta_mptr(req);	/* [한국어] 기본 MPTR 고속 경로 */
}

/*
 * [한국어]
 * nvme_prep_rq - SQ 투입 직전 요청 준비: cmd 빌드 + DMA map + start_request
 *
 * iod 플래그/카운터 리셋 → nvme_setup_cmd(core) → map_data → map_metadata.
 * 실패 시 역순 unmap/cleanup. start_request 로 타임아웃 시계 시작.
 * 호출: queue_rq / prep_rq_batch
 */
static blk_status_t nvme_prep_rq(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] 요청 PDU — cmd/매핑 상태 */
	blk_status_t ret;	/* [한국어] 단계별 블록 상태 누적 */

	iod->flags = 0;			/* [한국어] 이전 재사용 태그 잔여 플래그 제거 */
	iod->nr_descriptors = 0;	/* [한국어] descriptor 페이지 개수 리셋 */
	iod->total_len = 0;		/* [한국어] 데이터 매핑 길이 리셋 */
	iod->meta_total_len = 0;	/* [한국어] 메타 매핑 길이 리셋 */
	iod->nr_dma_vecs = 0;		/* [한국어] phys 벡터 개수 리셋 */

	ret = nvme_setup_cmd(req->q->queuedata, req);	/* [한국어] core 가 opcode/ns/SLBA 등 SQE 채움 */
	if (ret)
		return ret;	/* [한국어] 명령 구성 실패 — DMA 전 단계라 unmap 불필요 */

	if (blk_rq_nr_phys_segments(req)) {	/* [한국어] 페이로드 있는 요청만 데이터 매핑 */
		ret = nvme_map_data(req);	/* [한국어] PRP/SGL 로 dptr 구성 — 핫패스 비용 핵심 */
		if (ret)
			goto out_free_cmd;	/* [한국어] 맵 실패 → cleanup_cmd */
	}

	if (blk_integrity_rq(req)) {	/* [한국어] PI/메타 보호 정보 경로 */
		ret = nvme_map_metadata(req);	/* [한국어] MPTR 또는 meta SGL */
		if (ret)
			goto out_unmap_data;	/* [한국어] 메타 실패 시 이미 맵된 데이터 해제 */
	}

	nvme_start_request(req);	/* [한국어] 타임아웃 추적 시작 — 이후 하드웨어 소유 */
	return BLK_STS_OK;	/* [한국어] SQ 복사 준비 완료 */
out_unmap_data:
	if (blk_rq_nr_phys_segments(req))	/* [한국어] 데이터 맵이 있었다면 */
		nvme_unmap_data(req);	/* [한국어] 데이터 DMA 롤백 */
out_free_cmd:
	nvme_cleanup_cmd(req);	/* [한국어] core 명령 부가 상태 정리 */
	return ret;	/* [한국어] 실패 상태 전파 → blk-mq 재시도/에러 */
}

/*
 * [한국어]
 * nvme_queue_rq - blk-mq 단일 요청 제출 핫패스
 *
 * NVMEQ_ENABLED·ctrl ready 검사 → prep → sq_lock 하 copy + doorbell.
 * bd->last 가 false 면 도어벨 배치 지연 가능. dying 큐는 IOERR.
 * 호출 체인: blk_mq → [여기] → nvme_prep_rq → hardware
 */
static blk_status_t nvme_queue_rq(struct blk_mq_hw_ctx *hctx,
			 const struct blk_mq_queue_data *bd)
{
	struct nvme_queue *nvmeq = hctx->driver_data;	/* [한국어] 이 hctx 의 하드웨어 큐 */
	struct nvme_dev *dev = nvmeq->dev;		/* [한국어] 소속 PCIe 컨트롤러 */
	struct request *req = bd->rq;			/* [한국어] 제출 대상 blk-mq 요청 */
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] SQE/매핑 PDU */
	blk_status_t ret;	/* [한국어] prep 결과 */

	/*
	 * We should not need to do this, but we're still using this to
	 * ensure we can drain requests on a dying queue.
	 */
	/* [한국어] suspend/disable 가 ENABLED 를 내린 뒤에도 들어온 요청을 즉시 실패 */
	if (unlikely(!test_bit(NVMEQ_ENABLED, &nvmeq->flags)))	/* [한국어] dying 큐 게이트 */
		return BLK_STS_IOERR;	/* [한국어] 재시도 없이 실패 완료 유도 */

	if (unlikely(!nvme_check_ready(&dev->ctrl, req, true)))	/* [한국어] LIVE 등 상태·패브릭 비해당 검사 */
		return nvme_fail_nonready_command(&dev->ctrl, req);	/* [한국어] 상태별 실패/재시도 정책 */

	ret = nvme_prep_rq(req);	/* [한국어] cmd+DMA+timeout 시작 */
	if (unlikely(ret))
		return ret;	/* [한국어] 맵 자원 부족 등은 blk-mq 가 재큐잉 */
	spin_lock(&nvmeq->sq_lock);	/* [한국어] SQ tail/copy/doorbell 임계구역 */
	nvme_sq_copy_cmd(nvmeq, &iod->cmd);	/* [한국어] 링에 SQE 기록 */
	nvme_write_sq_db(nvmeq, bd->last);	/* [한국어] last 면 즉시, 아니면 배치 가능 doorbell */
	spin_unlock(&nvmeq->sq_lock);	/* [한국어] 제출 임계구역 종료 */
	return BLK_STS_OK;	/* [한국어] 하드웨어가 소유 — 완료는 IRQ/poll */
}

/*
 * [한국어]
 * nvme_submit_cmds - prep 완료 요청 리스트를 한 큐에 일괄 SQ 복사 + doorbell 1회
 *
 * queue_rqs 배치 효율 핵심. empty 면 no-op.
 */
static void nvme_submit_cmds(struct nvme_queue *nvmeq, struct rq_list *rqlist)
{
	struct request *req;	/* [한국어] 리스트에서 꺼낸 blk-mq 요청 */

	if (rq_list_empty(rqlist))	/* [한국어] 배치 없음 — doorbell 불필요 */
		return;	/* [한국어] no-op */

	spin_lock(&nvmeq->sq_lock);	/* [한국어] SQ 생산자 임계구역 */
	while ((req = rq_list_pop(rqlist))) {	/* [한국어] prep 완료분 전부 복사 */
		struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] SQE 템플릿 */

		nvme_sq_copy_cmd(nvmeq, &iod->cmd);	/* [한국어] tail 슬롯에 64B 기록 */
	}
	nvme_write_sq_db(nvmeq, true);	/* [한국어] 배치 끝 강제 doorbell 1회 */
	spin_unlock(&nvmeq->sq_lock);	/* [한국어] 컨트롤러가 일괄 인지 */
}

/*
 * [한국어]
 * nvme_prep_rq_batch - queue_rqs 용 prep. ENABLED/ready 실패 시 false→requeue
 */
static bool nvme_prep_rq_batch(struct nvme_queue *nvmeq, struct request *req)
{
	/*
	 * We should not need to do this, but we're still using this to
	 * ensure we can drain requests on a dying queue.
	 */
	/* [한국어] dying 큐 drain — ENABLED 클리어 후 유입 차단 */
	if (unlikely(!test_bit(NVMEQ_ENABLED, &nvmeq->flags)))	/* [한국어] suspend/disable 게이트 */
		return false;	/* [한국어] requeue 리스트로 */
	if (unlikely(!nvme_check_ready(&nvmeq->dev->ctrl, req, true)))	/* [한국어] LIVE 등 상태 */
		return false;	/* [한국어] 상태 부적합 — 재시도/실패는 상위 */

	return nvme_prep_rq(req) == BLK_STS_OK;	/* [한국어] cmd+DMA 성공 시 SQ 투입 후보 */
}

/*
 * [한국어]
 * nvme_queue_rqs - 다중 요청 배치 제출. 큐가 바뀌면 이전 큐 flush
 *
 * prep 실패 요청은 입력 리스트에 남겨 blk-mq 가 재시도. doorbell 은 큐당 1회.
 */
static void nvme_queue_rqs(struct rq_list *rqlist)
{
	struct rq_list submit_list = { };	/* [한국어] prep 성공 → SQ 투입 대기 */
	struct rq_list requeue_list = { };	/* [한국어] prep 실패 → 호출자 재시도 */
	struct nvme_queue *nvmeq = NULL;	/* [한국어] 현재 배치 대상 큐 */
	struct request *req;	/* [한국어] 지역 변수 */

	while ((req = rq_list_pop(rqlist))) {	/* [한국어] 입력 리스트 소비 */
		if (nvmeq && nvmeq != req->mq_hctx->driver_data)
			nvme_submit_cmds(nvmeq, &submit_list);	/* [한국어] 큐 전환 시 이전 배치 플러시 */
		nvmeq = req->mq_hctx->driver_data;	/* [한국어] 이 요청의 하드웨어 큐 */

		if (nvme_prep_rq_batch(nvmeq, req))	/* [한국어] cmd+DMA 준비 */
			rq_list_add_tail(&submit_list, req);	/* [한국어] SQ 복사 대기열 */
		else
			rq_list_add_tail(&requeue_list, req);	/* [한국어] 자원 부족 등 재큐잉 */
	}

	if (nvmeq)
		nvme_submit_cmds(nvmeq, &submit_list);	/* [한국어] 마지막 큐 배치 투입 */
	*rqlist = requeue_list;	/* [한국어] 실패분만 호출자에 반환 */
}

/*
 * [한국어]
 * nvme_pci_unmap_rq - 완료 직전 데이터+메타 DMA 해제 (단일/배치 공용)
 *
 * integrity 가 있으면 메타 먼저, 페이로드 세그먼트가 있으면 데이터 unmap.
 */
static __always_inline void nvme_pci_unmap_rq(struct request *req)
{
	if (blk_integrity_rq(req))	/* [한국어] PI/메타 매핑 존재 */
		nvme_unmap_metadata(req);	/* [한국어] MPTR 또는 meta SGL 해제 */
	if (blk_rq_nr_phys_segments(req))	/* [한국어] 데이터 페이로드 있음 */
		nvme_unmap_data(req);	/* [한국어] PRP/SGL 페이로드 해제 */
}

/*
 * [한국어]
 * nvme_pci_complete_rq - 단일 완료: unmap 후 core nvme_complete_rq
 */
static void nvme_pci_complete_rq(struct request *req)
{
	nvme_pci_unmap_rq(req);	/* [한국어] 하드웨어 소유 끝 — DMA 역매핑 */
	nvme_complete_rq(req);	/* [한국어] status→blk 에러·end_io·재시도 정책 */
}

/*
 * [한국어]
 * nvme_pci_complete_batch - IRQ 배치 완료: unmap 훅을 complete_batch 에 전달
 */
static void nvme_pci_complete_batch(struct io_comp_batch *iob)
{
	nvme_complete_batch(iob, nvme_pci_unmap_rq);	/* [한국어] 배치 각 요청 unmap+core complete */
}

/* We read the CQE phase first to check if the rest of the entry is valid */
/*
 * [한국어]
 * nvme_cqe_pending - CQ phase bit 로 새 CQE 유효성 검사
 *
 * status LSB 가 phase. 호스트 소유 슬롯과 컨트롤러 기록이 교차할 때 토글.
 * true 면 나머지 CQE 필드 읽기 전 dma_rmb 필요.
 */
static inline bool nvme_cqe_pending(struct nvme_queue *nvmeq)
{
	struct nvme_completion *hcqe = &nvmeq->cqes[nvmeq->cq_head];	/* [한국어] 소비자 head 슬롯 */

	return (le16_to_cpu(READ_ONCE(hcqe->status)) & 1) == nvmeq->cq_phase;	/* [한국어] phase 일치=새 완료 */
}

/*
 * [한국어]
 * nvme_ring_cq_doorbell - CQ head 를 컨트롤러에 알려 슬롯 재사용 허용
 *
 * q_db+db_stride 가 CQ doorbell. dbbuf 동일 적용.
 */
static inline void nvme_ring_cq_doorbell(struct nvme_queue *nvmeq)
{
	u16 head = nvmeq->cq_head;	/* [한국어] 호스트가 소비를 끝낸 다음 슬롯 인덱스 */

	if (nvme_dbbuf_update_and_check_event(head, nvmeq->dbbuf_cq_db,
					      nvmeq->dbbuf_cq_ei))	/* [한국어] shadow CQ head 갱신 */
		writel(head, nvmeq->q_db + nvmeq->dev->db_stride);	/* [한국어] CQ doorbell MMIO */
}

/*
 * [한국어]
 * nvme_queue_tagset - qid 대응 blk_mq_tags (admin vs io)
 */
static inline struct blk_mq_tags *nvme_queue_tagset(struct nvme_queue *nvmeq)
{
	if (!nvmeq->qid)	/* [한국어] admin 큐는 admin_tagset */
		return nvmeq->dev->admin_tagset.tags[0];	/* [한국어] 단일 admin hctx 태그 */
	return nvmeq->dev->tagset.tags[nvmeq->qid - 1];	/* [한국어] I/O qid→tagset 인덱스 보정 */
}

/*
 * [한국어]
 * nvme_handle_cqe - 단일 CQE 처리: AEN 특수 경로 또는 request 완료
 *
 * AEN 은 request 없이 예약 command_id. 일반은 find_rq → try_complete/batch.
 */
static inline void nvme_handle_cqe(struct nvme_queue *nvmeq,
				   struct io_comp_batch *iob, u16 idx)	/* [한국어] 지역 변수 */
{
	struct nvme_completion *cqe = &nvmeq->cqes[idx];	/* [한국어] 완료 엔트리 */
	__u16 command_id = READ_ONCE(cqe->command_id);	/* [한국어] 태그/CID — 요청 역참조 키 */
	struct request *req;	/* [한국어] 매칭된 blk-mq 요청 */

	/*
	 * AEN requests are special as they don't time out and can
	 * survive any kind of queue freeze and often don't respond to
	 * aborts.  We don't even bother to allocate a struct request
	 * for them but rather special case them here.
	 */
	/* [한국어] Async Event 는 상주 슬롯 — request 수명/타임아웃 모델 밖 */
	if (unlikely(nvme_is_aen_req(nvmeq->qid, command_id))) {	/* [한국어] admin 고정 AEN CID */
		nvme_complete_async_event(&nvmeq->dev->ctrl,
				cqe->status, &cqe->result);	/* [한국어] core 가 이벤트 디스패치·재무장 */
		return;	/* [한국어] tagset 조회 없음 */
	}

	req = nvme_find_rq(nvme_queue_tagset(nvmeq), command_id);	/* [한국어] CID→inflight request */
	if (unlikely(!req)) {	/* [한국어] stale/이중완료/펌웨어 버그 */
		dev_warn(nvmeq->dev->ctrl.device,
			"invalid id %d completed on queue %d\n",
			command_id, le16_to_cpu(cqe->sq_id));	/* [한국어] 진단 로그 후 드롭 */
		return;	/* [한국어] 태그가 이 큐의 범위 밖이다 — 완료를 요청과 짝지을 수 없으니 무시한다 */
	}

	trace_nvme_sq(req, cqe->sq_head, nvmeq->sq_tail);	/* [한국어] SQ head/tail 추적점 */
	if (!nvme_try_complete_req(req, cqe->status, cqe->result) &&	/* [한국어] status 적용 후 빠른 완료 시도 */
	    !blk_mq_add_to_batch(req, iob,
				 nvme_req(req)->status != NVME_SC_SUCCESS,	/* [한국어] 실패는 배치 제외 경향 */
				 nvme_pci_complete_batch))	/* [한국어] 배치 플러시 훅=unmap+complete */
		nvme_pci_complete_rq(req);	/* [한국어] 즉시 unmap + core complete */
}

/*
 * [한국어]
 * nvme_update_cq_head - CQ 소비자 인덱스 증가, wrap 시 phase 토글
 *
 * 스펙: 링 한 바퀴마다 phase 반전으로 full/empty 모호성 해소.
 */
static inline void nvme_update_cq_head(struct nvme_queue *nvmeq)
{
	u32 tmp = nvmeq->cq_head + 1;	/* [한국어] 다음 소비자 슬롯 후보 */

	if (tmp == nvmeq->q_depth) {	/* [한국어] 링 wrap */
		nvmeq->cq_head = 0;	/* [한국어] 처음으로 */
		nvmeq->cq_phase ^= 1;	/* [한국어] phase 비트 반전 — 다음 바퀴 CQE 유효 조건 */
	} else {
		nvmeq->cq_head = tmp;	/* [한국어] 단순 전진 */
	}
}

/*
 * [한국어]
 * nvme_poll_cq - pending CQE 전부 drain. 발견 시 CQ doorbell
 *
 * phase 확인 후 dma_rmb 로 CQE 필드 순서 보장. IRQ·poll·reap 공용.
 */
static inline bool nvme_poll_cq(struct nvme_queue *nvmeq,
			        struct io_comp_batch *iob)	/* [한국어] 지역 변수 */
{
	bool found = false;	/* [한국어] 하나라도 완료를 봤는지 — doorbell/IRQ 반환용 */

	while (nvme_cqe_pending(nvmeq)) {	/* [한국어] phase 가 맞는 동안 연속 소비 */
		found = true;	/* [한국어] 일감 있음 */
		/*
		 * load-load control dependency between phase and the rest of
		 * the cqe requires a full read memory barrier
		 */
		dma_rmb();	/* [한국어] phase 로드 후 status/result/cid 로드 순서 보장 */
		nvme_handle_cqe(nvmeq, iob, nvmeq->cq_head);	/* [한국어] 현재 head CQE 완료 처리 */
		nvme_update_cq_head(nvmeq);	/* [한국어] 소비자 전진 */
	}

	if (found)
		nvme_ring_cq_doorbell(nvmeq);	/* [한국어] 일괄 head 통지 — 슬롯 재사용 허가 */
	return found;	/* [한국어] IRQ_HANDLED / poll 진척 여부 */
}

/*
 * [한국어]
 * nvme_irq - MSI/MSI-X 완료 핸들러 (또는 threaded 의 thread fn)
 *
 * poll_cq + 배치 complete. 일감 없으면 IRQ_NONE (공유 벡터 대비).
 */
static irqreturn_t nvme_irq(int irq, void *data)
{
	struct nvme_queue *nvmeq = data;	/* [한국어] request_irq 때 넘긴 큐 쿠키 */
	DEFINE_IO_COMP_BATCH(iob);	/* [한국어] 스택 배치 complete 리스트 */

	if (nvme_poll_cq(nvmeq, &iob)) {	/* [한국어] CQ drain */
		if (!rq_list_empty(&iob.req_list))	/* [한국어] 배치에 쌓인 요청 플러시 */
			nvme_pci_complete_batch(&iob);	/* [한국어] unmap 훅 포함 일괄 완료 */
		return IRQ_HANDLED;	/* [한국어] 이 장치가 인터럽트 소비 */
	}
	return IRQ_NONE;	/* [한국어] 스퓨리어스/공유선 — 다른 장치 기회 */
}

/*
 * [한국어]
 * nvme_irq_check - threaded IRQ 의 hard-irq primary handler
 *
 * phase 만 보고 WAKE_THREAD 여부 결정 — hardirq 에서 무거운 완료 회피.
 */
static irqreturn_t nvme_irq_check(int irq, void *data)
{
	struct nvme_queue *nvmeq = data;	/* [한국어] 동일 큐 쿠키 */

	if (nvme_cqe_pending(nvmeq))	/* [한국어] 값싼 phase 스모크 테스트 */
		return IRQ_WAKE_THREAD;	/* [한국어] 스레드 컨텍스트에서 nvme_irq 실행 */
	return IRQ_NONE;	/* [한국어] 완료 없음 */
}

/*
 * Poll for completions for any interrupt driven queue
 * Can be called from any context.
 */
/*
 * [한국어]
 * nvme_poll_irqdisable - timeout 경로: IRQ 끄고 CQ 폴링 (핸들러와 배타)
 *
 * disable_irq 로 MSI-X 핸들러 진입 차단 후 cq_poll_lock 하 drain.
 * 폴링 큐에서는 호출 금지(WARN). 미스드 인터럽트 수확 핵심.
 */
static void nvme_poll_irqdisable(struct nvme_queue *nvmeq)
{
	struct pci_dev *pdev = to_pci_dev(nvmeq->dev->dev);	/* [한국어] 벡터 번호 조회 */
	int irq;	/* [한국어] Linux IRQ 번호 */

	WARN_ON_ONCE(test_bit(NVMEQ_POLLED, &nvmeq->flags));	/* [한국어] 폴링 큐는 IRQ 없음 */

	irq = pci_irq_vector(pdev, nvmeq->cq_vector);	/* [한국어] MSI-X 벡터 → IRQ */
	disable_irq(irq);	/* [한국어] 핸들러와 상호배제 — timeout 컨텍스트 */
	spin_lock(&nvmeq->cq_poll_lock);	/* [한국어] poll() 과 직렬화 */
	nvme_poll_cq(nvmeq, NULL);	/* [한국어] 즉시 complete 경로 (배치 없음) */
	spin_unlock(&nvmeq->cq_poll_lock);	/* [한국어] CQ 락 해제 */
	enable_irq(irq);	/* [한국어] 인터럽트 재무장 */
}

/*
 * [한국어]
 * nvme_poll - blk-mq poll 훅. NVMEQ_POLLED 큐만 CQ 검사
 *
 * spin cq_poll_lock. 반환: 완료 발견 시 비0.
 */
static int nvme_poll(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob)
{
	struct nvme_queue *nvmeq = hctx->driver_data;	/* [한국어] 폴링 대상 하드웨어 큐 */
	bool found;	/* [한국어] poll_cq 진척 여부 */

	if (!test_bit(NVMEQ_POLLED, &nvmeq->flags) ||	/* [한국어] 인터럽트 큐는 poll 스킵 */
	    !nvme_cqe_pending(nvmeq))	/* [한국어] phase 없는 빠른 경로 */
		return 0;	/* [한국어] 일감 없음 */

	spin_lock(&nvmeq->cq_poll_lock);	/* [한국어] IRQ disable 폴과 직렬화 */
	found = nvme_poll_cq(nvmeq, iob);	/* [한국어] CQE drain → 배치 iob */
	spin_unlock(&nvmeq->cq_poll_lock);	/* [한국어] CQ 임계구역 종료 */

	return found;	/* [한국어] blk-mq poll 루프 진척 표시 */
}

/*
 * [한국어]
 * nvme_pci_submit_async_event - Admin SQ 에 AEN 커맨드 수동 제출
 *
 * request 없이 고정 command_id(NVME_AQ_BLK_MQ_DEPTH). core 가 AEN 재무장 시 호출.
 * 핫패스와 동일하게 sq_lock + copy + doorbell.
 */
static void nvme_pci_submit_async_event(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);	/* [한국어] admin 큐 접근 */
	struct nvme_queue *nvmeq = &dev->queues[0];	/* [한국어] Admin SQ/CQ */
	struct nvme_command c = { };	/* [한국어] AEN SQE 제로 초기화 */

	c.common.opcode = nvme_admin_async_event;	/* [한국어] Async Event Request opcode */
	c.common.command_id = NVME_AQ_BLK_MQ_DEPTH;	/* [한국어] 예약 CID — tagset 밖 */

	spin_lock(&nvmeq->sq_lock);	/* [한국어] admin SQ 직렬화 */
	nvme_sq_copy_cmd(nvmeq, &c);	/* [한국어] AEN 을 링에 기록 */
	nvme_write_sq_db(nvmeq, true);	/* [한국어] 즉시 doorbell — 배치 없음 */
	spin_unlock(&nvmeq->sq_lock);	/* [한국어] admin 제출 완료 */
}

/*
 * [한국어]
 * nvme_pci_subsystem_reset - NSSR 레지스터에 매직 기록 (서브시스템 리셋)
 *
 * shutdown_lock 으로 BAR 맵과 remove 레이스 방지. 상태 RESETTING→CONNECTING→LIVE
 * 를 빠르게 통과해 sysfs NSSR 경로가 막히지 않게 한다. CSTS 읽기로 write flush.
 */
static int nvme_pci_subsystem_reset(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);	/* [한국어] BAR/NSSR 접근 */
	int ret = 0;	/* [한국어] 기본 성공 — 단계 실패 시 갱신 */

	/*
	 * Taking the shutdown_lock ensures the BAR mapping is not being
	 * altered by reset_work. Holding this lock before the RESETTING state
	 * change, if successful, also ensures nvme_remove won't be able to
	 * proceed to iounmap until we're done.
	 */
	/* [한국어] BAR iounmap/remap 과 NSSR MMIO 상호배제 */
	mutex_lock(&dev->shutdown_lock);	/* [한국어] 맵 수명 보호 */
	if (!dev->bar_mapped_size) {	/* [한국어] 이미 unmap 됨 */
		ret = -ENODEV;	/* [한국어] MMIO 불가 */
		goto unlock;	/* [한국어] BAR 매핑이 없으면 레지스터에 손댈 수 없다 */
	}

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING)) {	/* [한국어] 리셋 상태 선점 */
		ret = -EBUSY;	/* [한국어] 다른 리셋/삭제 진행 중 */
		goto unlock;	/* [한국어] 이미 리셋 중이거나 삭제 중이다 — 전이가 거부됐다 */
	}

	writel(NVME_SUBSYS_RESET, dev->bar + NVME_REG_NSSR);	/* [한국어] 스펙 매직 → 서브시스템 리셋 트리거 */

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING) ||
	    !nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE))	/* [한국어] sysfs 경로 상태 복귀 시도 */
		goto unlock;	/* [한국어] 전이 실패해도 NSSR 은 이미 기록됨 */

	/*
	 * Read controller status to flush the previous write and trigger a
	 * pcie read error.
	 */
	/* [한국어] posted write 플러시 — 링크 오류 시 읽기에서 조기 감지 */
	readl(dev->bar + NVME_REG_CSTS);	/* [한국어] CSTS 더미 로드 */
unlock:
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] BAR 보호 종료 */
	return ret;	/* [한국어] 0/-ENODEV/-EBUSY */
}

/*
 * [한국어]
 * adapter_delete_queue - Delete SQ/CQ admin 동기 제출 공통 헬퍼
 */
static int adapter_delete_queue(struct nvme_dev *dev, u8 opcode, u16 id)
{
	struct nvme_command c = { };	/* [한국어] Delete Queue SQE */

	c.delete_queue.opcode = opcode;	/* [한국어] delete_sq 또는 delete_cq */
	c.delete_queue.qid = cpu_to_le16(id);	/* [한국어] 대상 큐 ID */

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);	/* [한국어] admin 동기 완료 */
}

/*
 * [한국어]
 * adapter_alloc_cq - Create I/O Completion Queue admin
 *
 * PRP1=CQ DMA, 물리 연속. 폴링이면 IRQ_ENABLED 클리어. vector=MSI-X 인덱스.
 */
static int adapter_alloc_cq(struct nvme_dev *dev, u16 qid,
		struct nvme_queue *nvmeq, s16 vector)
{
	struct nvme_command c = { };	/* [한국어] Create CQ SQE */
	int flags = NVME_QUEUE_PHYS_CONTIG;	/* [한국어] 단일 연속 CQ 버퍼 */

	if (!test_bit(NVMEQ_POLLED, &nvmeq->flags))	/* [한국어] 큐/요청 플래그 비트 조작 */
		flags |= NVME_CQ_IRQ_ENABLED;	/* [한국어] 인터럽트 완료 통지 요청 */

	/*
	 * Note: we (ab)use the fact that the prp fields survive if no data
	 * is attached to the request.
	 */
	/* [한국어] 데이터 버퍼 없는 admin 요청 — prp1 필드에 CQ 베이스 직접 기록 */
	c.create_cq.opcode = nvme_admin_create_cq;	/* [한국어] Create I/O CQ opcode */
	c.create_cq.prp1 = cpu_to_le64(nvmeq->cq_dma_addr);	/* [한국어] 호스트 CQ 링 DMA */
	c.create_cq.cqid = cpu_to_le16(qid);	/* [한국어] 새 CQ ID */
	c.create_cq.qsize = cpu_to_le16(nvmeq->q_depth - 1);	/* [한국어] 0-based 크기 */
	c.create_cq.cq_flags = cpu_to_le16(flags);	/* [한국어] PHYS_CONTIG | IEN */
	c.create_cq.irq_vector = cpu_to_le16(vector);	/* [한국어] MSI-X 벡터 번호 */

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);	/* [한국어] admin 동기 완료 대기 */
}

/*
 * [한국어]
 * adapter_alloc_sq - Create I/O Submission Queue admin
 *
 * 동일 qid CQ 에 연결. MEDIUM_PRIO quirk 로 WRRU 오동작 회피.
 */
static int adapter_alloc_sq(struct nvme_dev *dev, u16 qid,
						struct nvme_queue *nvmeq)
{
	struct nvme_ctrl *ctrl = &dev->ctrl;	/* [한국어] quirks 조회용 */
	struct nvme_command c = { };	/* [한국어] Create SQ admin SQE */
	int flags = NVME_QUEUE_PHYS_CONTIG;	/* [한국어] 연속 SQ 버퍼 (호스트 또는 CMB) */

	/*
	 * Some drives have a bug that auto-enables WRRU if MEDIUM isn't
	 * set. Since URGENT priority is zeroes, it makes all queues
	 * URGENT.
	 */
	/* [한국어] 우선순위 0=URGENT 버그 — MEDIUM 비트로 균등 스케줄 강제 */
	if (ctrl->quirks & NVME_QUIRK_MEDIUM_PRIO_SQ)
		flags |= NVME_SQ_PRIO_MEDIUM;	/* [한국어] WRRU 오동작 회피 */

	/*
	 * Note: we (ab)use the fact that the prp fields survive if no data
	 * is attached to the request.
	 */
	/* [한국어] 데이터 없는 admin — prp1 에 SQ 베이스 직접 */
	c.create_sq.opcode = nvme_admin_create_sq;	/* [한국어] Create I/O SQ opcode */
	c.create_sq.prp1 = cpu_to_le64(nvmeq->sq_dma_addr);	/* [한국어] SQ 링 DMA/버스 주소 */
	c.create_sq.sqid = cpu_to_le16(qid);	/* [한국어] 새 SQ ID (=CQ ID 관례) */
	c.create_sq.qsize = cpu_to_le16(nvmeq->q_depth - 1);	/* [한국어] 0-based SQ 깊이 */
	c.create_sq.sq_flags = cpu_to_le16(flags);	/* [한국어] PHYS_CONTIG | PRIO */
	c.create_sq.cqid = cpu_to_le16(qid);	/* [한국어] 완료를 받을 CQ = 동일 qid */

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);	/* [한국어] Create SQ 동기 완료 */
}

/*
 * [한국어]
 * adapter_delete_cq - Delete Completion Queue admin 래퍼
 */
static int adapter_delete_cq(struct nvme_dev *dev, u16 cqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_cq, cqid);	/* [한국어] CQ 삭제 동기 제출 */
}

/*
 * [한국어]
 * adapter_delete_sq - Delete Submission Queue admin 래퍼
 */
static int adapter_delete_sq(struct nvme_dev *dev, u16 sqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_sq, sqid);	/* [한국어] SQ 삭제 동기 제출 */
}

/*
 * [한국어]
 * abort_endio - Abort admin 완료: abort_limit 토큰 반환 + request free
 */
static enum rq_end_io_ret abort_endio(struct request *req, blk_status_t error,	/* [한국어] blk-mq 요청/태그/큐 계층 연동 */
				      const struct io_comp_batch *iob)
{
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] abort 가 실린 admin 큐 */

	dev_warn(nvmeq->dev->ctrl.device,
		 "Abort status: 0x%x", nvme_req(req)->status);	/* [한국어] Abort 자체 성공/실패 가시성 */
	atomic_inc(&nvmeq->dev->ctrl.abort_limit);	/* [한국어] 동시 Abort 토큰 복구 */
	blk_mq_free_request(req);	/* [한국어] admin 태그 반환 */
	return RQ_END_IO_NONE;	/* [한국어] 추가 complete 없음 */
}

/*
 * [한국어]
 * nvme_should_reset - CSTS.CFS 또는 NSSRO 시 리셋 필요 여부
 *
 * 이미 RESETTING/CONNECTING 이면 중복 리셋 억제.
 */
static bool nvme_should_reset(struct nvme_dev *dev, u32 csts)
{
	/* If true, indicates loss of adapter communication, possibly by a
	 * NVMe Subsystem reset.
	 */
	bool nssro = dev->subsystem && (csts & NVME_CSTS_NSSRO);	/* [한국어] 서브시스템 리셋 발생 플래그 */

	/* If there is a reset/reinit ongoing, we shouldn't reset again. */
	/* [한국어] 이미 복구 중이면 중복 리셋 폭풍 방지 */
	switch (nvme_ctrl_state(&dev->ctrl)) {	/* [한국어] 위 영어 주석대로, 이미 복구가 진행 중이면 또 걸지 않는다 */
	case NVME_CTRL_RESETTING:	/* [한국어] 이미 리셋 중 */
	case NVME_CTRL_CONNECTING:	/* [한국어] 초기화 중 — 추가 리셋 금지 */
		return false;	/* [한국어] timeout 이 Abort 등 다른 수단 사용 */
	default:
		break;	/* [한국어] LIVE 등 — CFS/NSSRO 검사로 진행 */
	}

	/* We shouldn't reset unless the controller is on fatal error state
	 * _or_ if we lost the communication with it.
	 */
	/* [한국어] 정상 CSTS 면 리셋 불필요 — 단순 느린 I/O 가능 */
	if (!(csts & NVME_CSTS_CFS) && !nssro)	/* [한국어] fatal 도 NSSRO 도 아니면 정상 */
		return false;	/* [한국어] Abort 경로로 */

	return true;	/* [한국어] timeout 경로에서 즉시 disable+reset */
}

/*
 * [한국어]
 * nvme_warn_reset - 리셋 직전 CSTS/PCI_STATUS 진단 로그 (절전 모드 힌트 포함)
 *
 * CSTS=~0xFFFFFFFF 는 링크 다운/전원 이슈 징후 — ASPM/PS 디버그 메시지.
 */
static void nvme_warn_reset(struct nvme_dev *dev, u32 csts)
{
	/* Read a config register to help see what died. */
	/* [한국어] MMIO 와 별개로 cfg space 생존 여부 확인 */
	u16 pci_status;	/* [한국어] PCI_STATUS 스냅샷 */
	int result;	/* [한국어] config 접근 결과 */

	result = pci_read_config_word(to_pci_dev(dev->dev), PCI_STATUS,
				      &pci_status);	/* [한국어] 구성 공간 상태 비트 */
	if (result == PCIBIOS_SUCCESSFUL)	/* [한국어] cfg 접근 성공 */
		dev_warn(dev->ctrl.device,
			 "controller is down; will reset: CSTS=0x%x, PCI_STATUS=0x%hx\n",
			 csts, pci_status);	/* [한국어] 프로토콜+링크 단서 동시 로그 */
	else
		dev_warn(dev->ctrl.device,
			 "controller is down; will reset: CSTS=0x%x, PCI_STATUS read failed (%d)\n",
			 csts, result);	/* [한국어] cfg 도 죽음 — 핫제거/전원 의심 */

	if (csts != ~0)	/* [한국어] 전비트1 아니면 일반 fatal */
		return;	/* [한국어] 절전 힌트 불필요 */

	dev_warn(dev->ctrl.device,
		 "Does your device have a faulty power saving mode enabled?\n");	/* [한국어] 절전 버그 의심 안내 */
	dev_warn(dev->ctrl.device,
		 "Try \"nvme_core.default_ps_max_latency_us=0 pcie_aspm=off pcie_port_pm=off\" and report a bug\n");	/* [한국어] 현장 완화 파라미터 */
}

/*
 * [한국어]
 * nvme_timeout - blk-mq 요청 타임아웃 핸들러 (에러 복구 핵심)
 *
 * 순서: (1) PCIe disconnect/terminal → disable
 * (2) channel offline → 타이머 연장 (AER 충돌 방지)
 * (3) CFS/NSSRO → 즉시 리셋 (4) 인터럽트 미스 가능 → poll
 * (5) CONNECTING/DELETING 타임아웃 → cancel+disable
 * (6) Admin 또는 이미 abort 된 I/O → 컨트롤러 리셋
 * (7) 그 외 I/O → Abort 1회 + 타이머 재무장
 * 리셋: RESETTING → dev_disable → try_sched_reset
 */
static enum blk_eh_timer_return nvme_timeout(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] ABORTED 플래그 등 */
	struct nvme_queue *nvmeq = req->mq_hctx->driver_data;	/* [한국어] 타임아웃 난 큐 */
	struct nvme_dev *dev = nvmeq->dev;	/* [한국어] 컨트롤러 */
	struct request *abort_req;	/* [한국어] Abort admin 요청 */
	struct nvme_command cmd = { };	/* [한국어] Abort SQE */
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] PCIe 상태 조회용 */
	u32 csts = readl(dev->bar + NVME_REG_CSTS);	/* [한국어] RDY/CFS/NSSRO 스냅샷 */
	u8 opcode;	/* [한국어] 로그용 원 명령 opcode */

	/*
	 * Shutdown the device immediately if we see it is disconnected. This
	 * unblocks PCIe error handling if the nvme driver is waiting in
	 * error_resume for a device that has been removed. We can't unbind the
	 * driver while the driver's error callback is waiting to complete, so
	 * we're relying on a timeout to break that deadlock if a removal
	 * occurs while reset work is running.
	 */
	/* [한국어] 핫제거+AER 교착 해소: DELETING 으로 remove 경로 진전 허용 */
	if (pci_dev_is_disconnected(pdev))	/* [한국어] 구성 공간 접근 불가 */
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);	/* [한국어] 상태기계 종단 유도 */
	if (nvme_state_terminal(&dev->ctrl))	/* [한국어] DELETING/DEAD 등 */
		goto disable;	/* [한국어] 복구 대신 정리 */

	/* If PCI error recovery process is happening, we cannot reset or
	 * the recovery mechanism will surely fail.
	 */
	mb();	/* [한국어] error_state 관찰과 타임아웃 경로 순서 */
	if (pci_channel_offline(pdev))	/* [한국어] AER frozen 등 — 드라이버 리셋 금지 */
		return BLK_EH_RESET_TIMER;	/* [한국어] slot_reset 이 복구할 때까지 대기 */

	/*
	 * Reset immediately if the controller is failed
	 */
	if (nvme_should_reset(dev, csts)) {	/* [한국어] CFS 또는 NSSRO */
		nvme_warn_reset(dev, csts);	/* [한국어] CSTS/PCI_STATUS 진단 로그 */
		goto disable;	/* [한국어] Abort 없이 즉시 리셋 */
	}

	/*
	 * Did we miss an interrupt?
	 */
	/* [한국어] 인터럽트 유실 가능성 — 강제 폴링으로 자연 완료 수확 */
	if (test_bit(NVMEQ_POLLED, &nvmeq->flags))	/* [한국어] 폴링 큐 */
		nvme_poll(req->mq_hctx, NULL);	/* [한국어] blk-mq poll 경로 */
	else
		nvme_poll_irqdisable(nvmeq);	/* [한국어] IRQ 끄고 CQ drain */

	if (blk_mq_rq_state(req) != MQ_RQ_IN_FLIGHT) {	/* [한국어] 폴링이 이미 완료 처리함 */
		dev_warn(dev->ctrl.device,
			 "I/O tag %d (%04x) QID %d timeout, completion polled\n",
			 req->tag, nvme_cid(req), nvmeq->qid);	/* [한국어] 미스드 인터럽트 단서 */
		return BLK_EH_DONE;	/* [한국어] 추가 조치 없음 */
	}

	/*
	 * Shutdown immediately if controller times out while starting. The
	 * reset work will see the pci device disabled when it gets the forced
	 * cancellation error. All outstanding requests are completed on
	 * shutdown, so we return BLK_EH_DONE.
	 */
	switch (nvme_ctrl_state(&dev->ctrl)) {	/* [한국어] 상태가 타임아웃 처리 방식을 가른다 — 위 영어 주석에 각 경우의 근거가 있다 */
	case NVME_CTRL_CONNECTING:	/* [한국어] 초기화 중 타임아웃은 치명적 */
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);	/* [한국어] 연결 포기 */
		fallthrough;	/* [한국어] DELETING 과 동일 정리 */
	case NVME_CTRL_DELETING:	/* [한국어] 제거 중 — 복구 루프 금지 */
		dev_warn_ratelimited(dev->ctrl.device,	/* [한국어] 해체 중에는 타임아웃이 무더기로 나므로 로그를 제한한다 */
			 "I/O tag %d (%04x) QID %d timeout, disable controller\n",	/* [한국어] 태그와 큐를 남겨야 어느 요청이었는지 추적할 수 있다 */
			 req->tag, nvme_cid(req), nvmeq->qid);
		nvme_req(req)->flags |= NVME_REQ_CANCELLED;	/* [한국어] 재시도 억제 취소 표시 */
		nvme_dev_disable(dev, true);	/* [한국어] shutdown 경로로 전원/큐 정리 */
		return BLK_EH_DONE;	/* [한국어] 타이머 재무장 불필요 */
	case NVME_CTRL_RESETTING:	/* [한국어] 이미 reset_work 진행 중 */
		return BLK_EH_RESET_TIMER;	/* [한국어] 리셋 완료 대기 */
	default:
		break;	/* [한국어] LIVE 등 — Abort 또는 리셋 정책으로 */
	}

	/*
	 * Shutdown the controller immediately and schedule a reset if the
	 * command was already aborted once before and still hasn't been
	 * returned to the driver, or if this is the admin queue.
	 */
	opcode = nvme_req(req)->cmd->common.opcode;	/* [한국어] 로그·진단용 */
	if (!nvmeq->qid || (iod->flags & IOD_ABORTED)) {	/* [한국어] admin 또는 2차 타임아웃 */
		dev_warn(dev->ctrl.device,	/* [한국어] admin 큐이거나 이미 abort 를 시도한 요청이다 — 이번에는 리셋으로 간다 */
			 "I/O tag %d (%04x) opcode %#x (%s) QID %d timeout, reset controller\n",	/* [한국어] opcode 까지 남긴다 — 특정 명령에서만 멈추는 펌웨어 버그를 가려내기 위해 */
			 req->tag, nvme_cid(req), opcode,
			 nvme_opcode_str(nvmeq->qid, opcode), nvmeq->qid);
		nvme_req(req)->flags |= NVME_REQ_CANCELLED;	/* [한국어] 강제 취소 */
		goto disable;	/* [한국어] Abort 대신 컨트롤러 리셋 */
	}

	if (atomic_dec_return(&dev->ctrl.abort_limit) < 0) {	/* [한국어] 동시 Abort 토큰 고갈 */
		atomic_inc(&dev->ctrl.abort_limit);	/* [한국어] 토큰 복구 */
		return BLK_EH_RESET_TIMER;	/* [한국어] 여유 생길 때까지 대기 */
	}
	iod->flags |= IOD_ABORTED;	/* [한국어] 다음 타임아웃은 리셋 경로 */

	cmd.abort.opcode = nvme_admin_abort_cmd;	/* [한국어] Abort Command admin */
	cmd.abort.cid = nvme_cid(req);	/* [한국어] 대상 명령 ID */
	cmd.abort.sqid = cpu_to_le16(nvmeq->qid);	/* [한국어] 대상 SQ */

	dev_warn(nvmeq->dev->ctrl.device,
		 "I/O tag %d (%04x) opcode %#x (%s) QID %d timeout, aborting req_op:%s(%u) size:%u\n",
		 req->tag, nvme_cid(req), opcode, nvme_get_opcode_str(opcode),
		 nvmeq->qid, blk_op_str(req_op(req)), req_op(req),	/* [한국어] blk-mq 요청/태그/큐 계층 연동 */
		 blk_rq_bytes(req));	/* [한국어] 운영 가시성 로그 */

	abort_req = blk_mq_alloc_request(dev->ctrl.admin_q, nvme_req_op(&cmd),
					 BLK_MQ_REQ_NOWAIT);	/* [한국어] 타임아웃 경로 — sleep 금지 */
	if (IS_ERR(abort_req)) {	/* [한국어] admin 태그 고갈 */
		atomic_inc(&dev->ctrl.abort_limit);	/* [한국어] 토큰 반환 */
		return BLK_EH_RESET_TIMER;	/* [한국어] 재시도 여지 */
	}
	nvme_init_request(abort_req, &cmd);	/* [한국어] Abort SQE 를 admin 요청에 장착 */

	abort_req->end_io = abort_endio;	/* [한국어] 완료 시 abort_limit 복구 */
	abort_req->end_io_data = NULL;	/* [한국어] abort 완료는 요청을 반납하는 일만 하므로 넘길 문맥이 없다 */
	blk_execute_rq_nowait(abort_req, false);	/* [한국어] 비동기 Abort 발사 */

	/*
	 * The aborted req will be completed on receiving the abort req.
	 * We enable the timer again. If hit twice, it'll cause a device reset,
	 * as the device then is in a faulty state.
	 */
	return BLK_EH_RESET_TIMER;	/* [한국어] 원 요청 타이머 재무장 — 2차 만료 시 리셋 */

disable:
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_RESETTING)) {	/* [한국어] 리셋 상태 선점 */
		if (nvme_state_terminal(&dev->ctrl))	/* [한국어] 이미 종단 상태 */
			nvme_dev_disable(dev, true);	/* [한국어] 정리만 */
		return BLK_EH_DONE;	/* [한국어] 타이머 종료 — 다른 경로가 처리 중 */
	}

	nvme_dev_disable(dev, false);	/* [한국어] 비-shutdown disable — 재 enable 예정 */
	if (nvme_try_sched_reset(&dev->ctrl))	/* [한국어] reset_work 스케줄 실패 시 */
		nvme_unquiesce_io_queues(&dev->ctrl);	/* [한국어] 교착 방지 큐 재개 */
	return BLK_EH_DONE;	/* [한국어] 리셋 워크가 이후 담당 */
}

/*
 * [한국어]
 * nvme_free_queue - 단일 큐 CQ/SQ DMA 또는 CMB p2pmem 해제
 */
static void nvme_free_queue(struct nvme_queue *nvmeq)
{
	dma_free_coherent(nvmeq->dev->dev, CQ_SIZE(nvmeq),
				(void *)nvmeq->cqes, nvmeq->cq_dma_addr);	/* [한국어] CQ 항상 host coherent */
	if (!nvmeq->sq_cmds)
		return;	/* [한국어] 할당 실패 중간 상태 방어 */

	if (test_and_clear_bit(NVMEQ_SQ_CMB, &nvmeq->flags)) {	/* [한국어] CMB SQ 면 p2pmem 경로 */
		pci_free_p2pmem(to_pci_dev(nvmeq->dev->dev),
				nvmeq->sq_cmds, SQ_SIZE(nvmeq));	/* [한국어] CMB 영역 반환 */
	} else {
		dma_free_coherent(nvmeq->dev->dev, SQ_SIZE(nvmeq),
				nvmeq->sq_cmds, nvmeq->sq_dma_addr);	/* [한국어] 호스트 SQ 반환 */
	}
}

/*
 * [한국어]
 * nvme_free_queues - queue_count-1 부터 lowest 까지 역순 free
 */
static void nvme_free_queues(struct nvme_dev *dev, int lowest)
{
	int i;	/* [한국어] 역순 qid */

	for (i = dev->ctrl.queue_count - 1; i >= lowest; i--) {	/* [한국어] 높은 qid 부터 해제 */
		dev->ctrl.queue_count--;	/* [한국어] 카운트와 실제 free 동기 */
		nvme_free_queue(&dev->queues[i]);	/* [한국어] SQ/CQ DMA 또는 CMB 반환 */
	}
}

/*
 * [한국어]
 * nvme_suspend_queue - ENABLED 클리어, online--, IRQ free, admin quiesce
 *
 * mb 로 queue_rq 가 ENABLED 를 보도록 순서. 폴링 큐는 IRQ free 생략.
 */
static void nvme_suspend_queue(struct nvme_dev *dev, unsigned int qid)
{
	struct nvme_queue *nvmeq = &dev->queues[qid];	/* [한국어] 큐 배열은 인덱스가 곧 큐 번호다 */

	if (!test_and_clear_bit(NVMEQ_ENABLED, &nvmeq->flags))	/* [한국어] 이미 비활성이면 no-op */
		return;	/* [한국어] 이미 내려간 큐다 — test_and_clear 라 두 경로가 겹쳐도 한 번만 통과한다 */

	/* ensure that nvme_queue_rq() sees NVMEQ_ENABLED cleared */
	mb();	/* [한국어] 플래그 클리어가 제출 경로에 보이도록 */

	nvmeq->dev->online_queues--;	/* [한국어] online 카운트 감소 */
	if (!nvmeq->qid && nvmeq->dev->ctrl.admin_q)
		nvme_quiesce_admin_queue(&nvmeq->dev->ctrl);	/* [한국어] admin 제출 정지 */
	if (!test_and_clear_bit(NVMEQ_POLLED, &nvmeq->flags))	/* [한국어] 폴링이면 IRQ 없음 */
		pci_free_irq(to_pci_dev(dev->dev), nvmeq->cq_vector, nvmeq);	/* [한국어] 벡터 핸들러 해제 */
}

/*
 * [한국어]
 * nvme_suspend_io_queues - admin 제외 전 I/O 큐 suspend
 */
static void nvme_suspend_io_queues(struct nvme_dev *dev)
{
	int i;	/* [한국어] 지역 변수 */

	for (i = dev->ctrl.queue_count - 1; i > 0; i--)	/* [한국어] qid 0 admin 제외 */
		nvme_suspend_queue(dev, i);
}

/*
 * Called only on a device that has been disabled and after all other threads
 * that can check this device's completion queues have synced, except
 * nvme_poll(). This is the last chance for the driver to see a natural
 * completion before nvme_cancel_request() terminates all incomplete requests.
 */
/*
 * [한국어]
 * nvme_reap_pending_cqes - disable 직후 cancel 직전 I/O CQ 잔여 CQE 수확
 *
 * IRQ 는 이미 free 된 상태. cq_poll_lock 으로 poll() 과 배타적으로
 * nvme_poll_cq 를 돌려 자연 완료를 놓치지 않는다. admin(qid0) 은 별도 경로.
 */
static void nvme_reap_pending_cqes(struct nvme_dev *dev)
{
	int i;	/* [한국어] I/O 큐 역순 인덱스 — admin 제외 */

	for (i = dev->ctrl.queue_count - 1; i > 0; i--) {	/* [한국어] 할당된 I/O 큐만 순회 */
		spin_lock(&dev->queues[i].cq_poll_lock);	/* [한국어] poll/IRQ 공유 소비 락 */
		nvme_poll_cq(&dev->queues[i], NULL);	/* [한국어] phase 기반 CQE drain + CQ doorbell */
		spin_unlock(&dev->queues[i].cq_poll_lock);	/* [한국어] CQ 임계구역 종료 */
	}
}

/*
 * [한국어]
 * nvme_cmb_qdepth - CMB 용량에 맞춰 I/O SQ 깊이 축소 계산
 *
 * 전 I/O 큐 SQ 를 CMB 에 두면 cmb_size 가 부족할 수 있다. 페이지 정렬 후
 * 큐당 몫으로 depth 를 낮춘다. 64 미만이면 호스트 메모리 SQ 폴백(-ENOMEM).
 */
static int nvme_cmb_qdepth(struct nvme_dev *dev, int nr_io_queues,
				int entry_size)	/* [한국어] 지역 변수 */
{
	int q_depth = dev->q_depth;	/* [한국어] CAP.MQES 기반 현재 깊이 후보 */
	unsigned q_size_aligned = roundup(q_depth * entry_size,	/* [한국어] 지역 변수 */
					  NVME_CTRL_PAGE_SIZE);	/* [한국어] 큐당 CMB 소요(페이지 정렬) */

	if (q_size_aligned * nr_io_queues > dev->cmb_size) {	/* [한국어] CMB 총량이 전 큐 SQ 에 부족 */
		u64 mem_per_q = div_u64(dev->cmb_size, nr_io_queues);	/* [한국어] 큐당 균등 몫 */

		mem_per_q = round_down(mem_per_q, NVME_CTRL_PAGE_SIZE);	/* [한국어] 정렬 이하로 버림 */
		q_depth = div_u64(mem_per_q, entry_size);	/* [한국어] 엔트리 수로 환산한 축소 깊이 */

		/*
		 * Ensure the reduced q_depth is above some threshold where it
		 * would be better to map queues in system memory with the
		 * original depth
		 */
		/* [한국어] 과도 축소보다 호스트 DMA SQ + 원래 깊이가 유리 */
		if (q_depth < 64)	/* [한국어] 실무 하한 — CMB SQ 포기 신호 */
			return -ENOMEM;	/* [한국어] 호출자가 cmb_use_sqes=false 로 폴백 */
	}

	return q_depth;	/* [한국어] CMB 에 맞는 (또는 원본) 깊이 */
}

/*
 * [한국어]
 * nvme_alloc_sq_cmds - SQ 본문 할당: 가능하면 CMB p2pmem, 아니면 coherent DMA
 *
 * CMB SQS + use_cmb_sqes 시 장치 로컬 메모리에 SQ 를 두어 PCIe 트래픽 절감.
 * bus addr 변환 실패 시 p2pmem 반환 후 호스트 DMA 폴백.
 */
static int nvme_alloc_sq_cmds(struct nvme_dev *dev, struct nvme_queue *nvmeq,
				int qid)	/* [한국어] 지역 변수 */
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] CMB p2pmem API 인자 */

	if (qid && dev->cmb_use_sqes && (dev->cmbsz & NVME_CMBSZ_SQS)) {	/* [한국어] I/O 큐 + CMB SQ 지원 */
		nvmeq->sq_cmds = pci_alloc_p2pmem(pdev, SQ_SIZE(nvmeq));	/* [한국어] CMB 영역에서 SQ 링 확보 */
		if (nvmeq->sq_cmds) {	/* [한국어] CMB 용량 충분 */
			nvmeq->sq_dma_addr = pci_p2pmem_virt_to_bus(pdev,
							nvmeq->sq_cmds);	/* [한국어] Create SQ 에 넣을 버스 주소 */
			if (nvmeq->sq_dma_addr) {	/* [한국어] 버스 주소 변환 성공 */
				set_bit(NVMEQ_SQ_CMB, &nvmeq->flags);	/* [한국어] free 시 p2pmem 경로 */
				return 0;	/* [한국어] CMB SQ 준비 완료 — host DMA 절감 */
			}

			pci_free_p2pmem(pdev, nvmeq->sq_cmds, SQ_SIZE(nvmeq));	/* [한국어] 버스 주소 실패 롤백 */
		}
	}

	nvmeq->sq_cmds = dma_alloc_coherent(dev->dev, SQ_SIZE(nvmeq),
				&nvmeq->sq_dma_addr, GFP_KERNEL);	/* [한국어] 호스트 메모리 SQ 폴백 */
	if (!nvmeq->sq_cmds)
		return -ENOMEM;	/* [한국어] CQ 는 이미 할당됐을 수 있음 — 호출자 롤백 */
	return 0;	/* [한국어] 호스트 coherent SQ 준비 완료 */
}

/*
 * [한국어]
 * nvme_alloc_queue - 큐 슬롯 최초 할당: CQ DMA + SQ + 락/phase/db 포인터
 *
 * 이미 queue_count > qid 면 재진입 no-op. 실패 시 CQ 롤백.
 * Create admin 이전에 호스트 측 링 메모리를 준비한다.
 */
static int nvme_alloc_queue(struct nvme_dev *dev, int qid, int depth)
{
	struct nvme_queue *nvmeq = &dev->queues[qid];	/* [한국어] 큐 구조체는 probe 에서 배열로 미리 잡아 두었다 — 여기서는 메모리만 붙인다 */

	if (dev->ctrl.queue_count > qid)
		return 0;	/* [한국어] 이미 할당된 슬롯 — 리셋 재진입 안전 */

	nvmeq->sqes = qid ? dev->io_sqes : NVME_ADM_SQES;	/* [한국어] admin=6(64B), I/O 는 quirk 반영 */
	nvmeq->q_depth = depth;	/* [한국어] 링 엔트리 수 */
	nvmeq->cqes = dma_alloc_coherent(dev->dev, CQ_SIZE(nvmeq),
					 &nvmeq->cq_dma_addr, GFP_KERNEL);	/* [한국어] CQ 는 항상 호스트 coherent */
	if (!nvmeq->cqes)	/* [한국어] 완료 큐 없이는 큐가 성립하지 않는다 */
		goto free_nvmeq;	/* [한국어] 아직 붙인 것이 없어 되돌릴 것도 없다 */

	if (nvme_alloc_sq_cmds(dev, nvmeq, qid))	/* [한국어] SQ: CMB 우선 후 DMA 폴백 */
		goto free_cqdma;	/* [한국어] SQ 실패 — 방금 잡은 CQ 를 되돌려야 한다 */

	nvmeq->dev = dev;	/* [한국어] 역참조 부모 */
	spin_lock_init(&nvmeq->sq_lock);	/* [한국어] 제출 경로 직렬화 */
	spin_lock_init(&nvmeq->cq_poll_lock);	/* [한국어] 폴/IRQ 공유 소비 락 */
	nvmeq->cq_head = 0;	/* [한국어] 소비자 인덱스 초기값 */
	nvmeq->cq_phase = 1;	/* [한국어] 스펙: 초기 phase=1 */
	nvmeq->q_db = &dev->dbs[qid * 2 * dev->db_stride];	/* [한국어] SQ doorbell MMIO 슬롯 */
	nvmeq->qid = qid;	/* [한국어] 도어벨 주소를 이미 계산했으므로 번호는 식별용으로만 남는다 */
	dev->ctrl.queue_count++;	/* [한국어] 할당된 큐 개수 (online 과 별개) */

	return 0;	/* [한국어] 성공 반환 */

 free_cqdma:
	dma_free_coherent(dev->dev, CQ_SIZE(nvmeq), (void *)nvmeq->cqes,
			  nvmeq->cq_dma_addr);	/* [한국어] SQ 실패 시 CQ 롤백 */
 free_nvmeq:
	return -ENOMEM;	/* [한국어] 두 실패 모두 메모리 부족이다 */
}

/*
 * [한국어]
 * queue_request_irq - 큐 벡터에 nvme_irq (또는 threaded check+irq) 등록
 */
static int queue_request_irq(struct nvme_queue *nvmeq)
{
	struct pci_dev *pdev = to_pci_dev(nvmeq->dev->dev);	/* [한국어] 벡터 번호로 IRQ 를 찾으려면 PCI 장치가 필요하다 */
	int nr = nvmeq->dev->ctrl.instance;	/* [한국어] irq 이름 nvme{N}q{M} 의 N */

	if (use_threaded_interrupts) {	/* [한국어] 스레드 처리로 돌리면 하드 인터럽트 구간이 짧아져 지연 스파이크가 준다 */
		return pci_request_irq(pdev, nvmeq->cq_vector, nvme_irq_check,
				nvme_irq, nvmeq, "nvme%dq%d", nr, nvmeq->qid);	/* [한국어] hardirq=phase 검사, thread=완료 */
	} else {
		return pci_request_irq(pdev, nvmeq->cq_vector, nvme_irq,
				NULL, nvmeq, "nvme%dq%d", nr, nvmeq->qid);	/* [한국어] 단일 핸들러에서 poll_cq */
	}
}

/*
 * [한국어]
 * nvme_init_queue - tail/head/phase 리셋, CQ zero, dbbuf 연결, online++
 *
 * wmb 로 첫 인터럽트 전 초기화 가시성 확보.
 */
static void nvme_init_queue(struct nvme_queue *nvmeq, u16 qid)
{
	struct nvme_dev *dev = nvmeq->dev;	/* [한국어] 도어벨 베이스와 shadow 버퍼가 장치 쪽에 있다 */

	nvmeq->sq_tail = 0;	/* [한국어] 생산자 인덱스 리셋 */
	nvmeq->last_sq_tail = 0;	/* [한국어] doorbell 배치 비교 기준 */
	nvmeq->cq_head = 0;	/* [한국어] 소비자 인덱스 */
	nvmeq->cq_phase = 1;	/* [한국어] 스펙 초기 phase */
	nvmeq->q_db = &dev->dbs[qid * 2 * dev->db_stride];	/* [한국어] (재)맵 후 doorbell 포인터 */
	memset((void *)nvmeq->cqes, 0, CQ_SIZE(nvmeq));	/* [한국어] stale CQE/phase 잔존 제거 */
	nvme_dbbuf_init(dev, nvmeq, qid);	/* [한국어] shadow doorbell 슬롯 연결 */
	dev->online_queues++;	/* [한국어] ENABLED 직전 online 카운트 */
	wmb(); /* ensure the first interrupt sees the initialization */ /* [한국어] 인터럽트 가시성 장벽 */
}

/*
 * Try getting shutdown_lock while setting up IO queues.
 */
/*
 * [한국어]
 * nvme_setup_io_queues_trylock - shutdown_lock trylock + CONNECTING 검증
 *
 * disable 와 교착 피하려 blocking lock 대신 trylock. 실패=-ENODEV.
 */
static int nvme_setup_io_queues_trylock(struct nvme_dev *dev)
{
	/*
	 * Give up if the lock is being held by nvme_dev_disable.
	 */
	if (!mutex_trylock(&dev->shutdown_lock))	/* [한국어] disable 점유 중이면 즉시 포기 */
		return -ENODEV;	/* [한국어] 위 영어 주석대로, 해체가 진행 중이면 큐를 만들지 않고 물러난다 */

	/*
	 * Controller is in wrong state, fail early.
	 */
	if (nvme_ctrl_state(&dev->ctrl) != NVME_CTRL_CONNECTING) {	/* [한국어] 초기화 중이 아니면 큐 setup 금지 */
		mutex_unlock(&dev->shutdown_lock);	/* [한국어] trylock 으로 얻은 락을 놓고 나간다 */
		return -ENODEV;	/* [한국어] CONNECTING 이 아니라면 이 큐 생성은 이미 무의미하다 */
	}

	return 0;	/* [한국어] 락 보유 상태로 반환 — 호출자가 unlock */
}

/*
 * [한국어]
 * nvme_create_queue - Create CQ/SQ admin + init + IRQ + ENABLED
 *
 * polled 면 벡터 없이 POLLED 비트. 실패 시 delete 로 부분 생성 정리.
 * create 중 shutdown_lock 구간으로 disable 과 인터리브 제한.
 */
static int nvme_create_queue(struct nvme_queue *nvmeq, int qid, bool polled)
{
	struct nvme_dev *dev = nvmeq->dev;	/* [한국어] admin 큐로 Create CQ/SQ 를 보내야 한다 */
	int result;	/* [한국어] Create/IRQ 단계 결과 */
	u16 vector = 0;	/* [한국어] MSI-X 벡터 — 폴링이면 미사용 */

	clear_bit(NVMEQ_DELETE_ERROR, &nvmeq->flags);	/* [한국어] 이전 delete 실패 잔상 제거 */

	/*
	 * A queue's vector matches the queue identifier unless the controller
	 * has only one vector available.
	 */
	if (!polled)
		vector = dev->num_vecs == 1 ? 0 : qid;	/* [한국어] 단일 벡터면 전부 0 공유 */
	else
		set_bit(NVMEQ_POLLED, &nvmeq->flags);	/* [한국어] 인터럽트 없는 완료 경로 */

	result = adapter_alloc_cq(dev, qid, nvmeq, vector);	/* [한국어] Create I/O CQ admin */
	if (result)
		return result;	/* [한국어] CQ 실패 시 SQ 시도 불필요 */

	result = adapter_alloc_sq(dev, qid, nvmeq);	/* [한국어] Create I/O SQ — 동일 qid CQ 연결 */
	if (result < 0)
		return result;	/* [한국어] 음수=전송/자원 실패 */
	if (result)
		goto release_cq;	/* [한국어] 양수 NVMe status — CQ 롤백 */

	nvmeq->cq_vector = vector;	/* [한국어] request_irq 에 쓸 벡터 저장 */

	result = nvme_setup_io_queues_trylock(dev);	/* [한국어] disable 중이면 중단 */
	if (result)	/* [한국어] 해체와 경쟁 중이다 — 만든 CQ/SQ 는 아래 라벨이 되돌린다 */
		return result;
	nvme_init_queue(nvmeq, qid);	/* [한국어] head/tail/phase/online++ */
	if (!polled) {	/* [한국어] 폴링 큐는 인터럽트를 걸지 않는다 — 그것이 폴링 큐의 정의다 */
		result = queue_request_irq(nvmeq);	/* [한국어] MSI-X 핸들러 등록 */
		if (result < 0)
			goto release_sq;	/* [한국어] IRQ 실패 → SQ/CQ 삭제 */
	}

	set_bit(NVMEQ_ENABLED, &nvmeq->flags);	/* [한국어] queue_rq 게이트 개방 */
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] trylock 구간 종료 */
	return result;	/* [한국어] ENABLED 비트가 서야 제출 경로가 이 큐를 쓴다 */

release_sq:
	dev->online_queues--;	/* [한국어] init_queue 증가분 되돌림 */
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] SQ 까지 만든 뒤 실패했으므로 순서대로 되돌린다 */
	adapter_delete_sq(dev, qid);	/* [한국어] 부분 생성 SQ 제거 */
release_cq:
	adapter_delete_cq(dev, qid);	/* [한국어] CQ 제거 */
	return result;	/* [한국어] CQ 도 지웠다 — 컨트롤러 쪽에 남은 것이 없다 */
}

/* [한국어] Admin tagset blk-mq ops — 단일 큐, 배치/poll/map 없음 */
static const struct blk_mq_ops nvme_mq_admin_ops = {
	.queue_rq	= nvme_queue_rq,	/* [한국어] 제출 핫패스 (admin 도 동일) */
	.complete	= nvme_pci_complete_rq,	/* [한국어] unmap + core complete */
	.init_hctx	= nvme_admin_init_hctx,	/* [한국어] qid=0 바인딩 */
	.init_request	= nvme_pci_init_request,	/* [한국어] iod/cmd 포인터 고정 */
	.timeout	= nvme_timeout,	/* [한국어] admin 타임아웃→리셋 정책 */
};

/* [한국어] I/O tagset blk-mq ops — 배치 제출·affinity map·poll 포함 */
static const struct blk_mq_ops nvme_mq_ops = {
	.queue_rq	= nvme_queue_rq,	/* [한국어] 단일 요청 제출 */
	.queue_rqs	= nvme_queue_rqs,	/* [한국어] 다중 요청 배치 제출 */
	.complete	= nvme_pci_complete_rq,	/* [한국어] 완료 unmap */
	.commit_rqs	= nvme_commit_rqs,	/* [한국어] 배치 doorbell 플러시 */
	.init_hctx	= nvme_init_hctx,	/* [한국어] hctx→qid+1 */
	.init_request	= nvme_pci_init_request,
	.map_queues	= nvme_pci_map_queues,	/* [한국어] DEFAULT/READ/POLL affinity */
	.timeout	= nvme_timeout,	/* [한국어] Abort→리셋 정책 */
	.poll		= nvme_poll,	/* [한국어] 폴링 큐 완료 */
};

/*
 * [한국어]
 * nvme_dev_remove_admin - Admin request_queue/tagset 제거
 *
 * reset 중 quiesce 된 admin 에 대기 요청이 있을 수 있어 unquiesce 후
 * 실패 완료를 흘려보낸 뒤 tagset 을 해제한다. remove/probe 실패 공용.
 */
static void nvme_dev_remove_admin(struct nvme_dev *dev)
{
	if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q)) {	/* [한국어] 아직 유효한 admin_q */
		/*
		 * If the controller was reset during removal, it's possible
		 * user requests may be waiting on a stopped queue. Start the
		 * queue to flush these to completion.
		 */
		/* [한국어] stopped 큐에 걸린 요청을 완료 경로로 흘려 교착 방지 */
		nvme_unquiesce_admin_queue(&dev->ctrl);	/* [한국어] admin 제출/완료 재개 */
		nvme_remove_admin_tag_set(&dev->ctrl);	/* [한국어] admin tagset·queue 파괴 */
	}
}

/*
 * [한국어]
 * db_bar_size - admin+I/O 큐 전 도어벨을 커버하는 BAR0 맵 최소 바이트
 *
 * NVME_REG_DBS 오프셋 + (큐수)×2 도어벨×4B×db_stride. remap_bar 입력.
 */
static unsigned long db_bar_size(struct nvme_dev *dev, unsigned nr_io_queues)
{
	return NVME_REG_DBS + ((nr_io_queues + 1) * 8 * dev->db_stride);	/* [한국어] +1=admin, ×8=SQ+CQ DWORD 쌍 */
}

/*
 * [한국어]
 * nvme_remap_bar - BAR0 ioremap 크기 확장 (도어벨 창 포함)
 *
 * 축소는 안 함. 실패 시 bar=NULL. dbs=bar+NVME_REG_DBS.
 * shutdown_lock 또는 초기화 단일 스레드에서 호출.
 */
static int nvme_remap_bar(struct nvme_dev *dev, unsigned long size)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] BAR 리소스 소유 PCI 함수 */

	if (size <= dev->bar_mapped_size)	/* [한국어] 이미 충분하면 재맵 불필요 */
		return 0;	/* [한국어] 성공 반환 */
	if (size > pci_resource_len(pdev, 0))	/* [한국어] 물리 BAR 길이 초과 불가 */
		return -ENOMEM;	/* [한국어] 큐 수를 줄여 재시도하는 상위 루프 유도 */
	if (dev->bar)
		iounmap(dev->bar);	/* [한국어] 이전 매핑 해제 후 확장 재맵 */
	dev->bar = ioremap(pci_resource_start(pdev, 0), size);	/* [한국어] 레지스터+도어벨 창 MMIO */
	if (!dev->bar) {	/* [한국어] BAR 재매핑 실패 — 레지스터에 접근할 수 없다 */
		dev->bar_mapped_size = 0;	/* [한국어] 실패 시 크기 무효화 */
		return -ENOMEM;	/* [한국어] 크기를 0 으로 지워 이후 경로가 유효한 매핑으로 오인하지 않게 한다 */
	}
	dev->bar_mapped_size = size;	/* [한국어] 현재 유효 매핑 길이 */
	dev->dbs = dev->bar + NVME_REG_DBS;	/* [한국어] 도어벨 창 시작 — 큐별 오프셋 기준점 */

	return 0;	/* [한국어] 성공 반환 */
}

/*
 * [한국어]
 * nvme_pci_configure_admin_queue - Admin Q 메모리 등록 및 CC.EN 활성화
 *
 * 1) BAR admin db 까지 map 2) VS/NSSRC subsystem 플래그 3) NSSRO 클리어
 * 4) 이미 enable 상태면 disable_ctrl (shutdown 비트 대신 — 미지 호스트 메모리
 *    complete 위험 회피) 5) RDY 타임아웃 시 PCIe FLR+restore 후 재시도
 * 6) alloc admin queue, AQA/ASQ/ACQ, enable_ctrl, IRQ, ENABLED
 * CAP/CC/CSTS 상태기계 핵심 진입점.
 */
static int nvme_pci_configure_admin_queue(struct nvme_dev *dev)
{
	int result;	/* [한국어] 단계별 에러 코드 */
	u32 aqa;	/* [한국어] Admin Queue Attributes 레지스터 값 */
	struct nvme_queue *nvmeq;	/* [한국어] queues[0] admin */

	result = nvme_remap_bar(dev, db_bar_size(dev, 0));	/* [한국어] admin 도어벨까지 최소 맵 */
	if (result < 0)	/* [한국어] admin 큐 도어벨까지만 덮는 최소 크기로 먼저 매핑한다 */
		return result;

	dev->subsystem = readl(dev->bar + NVME_REG_VS) >= NVME_VS(1, 1, 0) ?	/* [한국어] MMIO 레지스터 접근 */
				NVME_CAP_NSSRC(dev->ctrl.cap) : 0;	/* [한국어] 1.1+ 이고 NSSRC 면 서브시스템 */

	if (dev->subsystem &&
	    (readl(dev->bar + NVME_REG_CSTS) & NVME_CSTS_NSSRO))	/* [한국어] 서브시스템 리셋 잔존 플래그 */
		writel(NVME_CSTS_NSSRO, dev->bar + NVME_REG_CSTS);	/* [한국어] W1C 로 클리어 */

	/*
	 * If the device has been passed off to us in an enabled state, just
	 * clear the enabled bit.  The spec says we should set the 'shutdown
	 * notification bits', but doing so may cause the device to complete
	 * commands to the admin queue ... and we don't know what memory that
	 * might be pointing at!
	 */
	/* [한국어] kexec/이전 드라이버 잔존 enable 상태 — CC.EN=0 만으로 정지 */
	result = nvme_disable_ctrl(&dev->ctrl, false);	/* [한국어] CSTS.RDY=0 대기 */
	if (result < 0) {	/* [한국어] CC.EN 을 내리지 못했다 — 컨트롤러가 반응하지 않는다 */
		struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] PCI 수준 리셋으로 한 번 더 시도하기 위해 */

		/*
		 * The NVMe Controller Reset method did not get an expected
		 * CSTS.RDY transition, so something with the device appears to
		 * be stuck. Use the lower level and bigger hammer PCIe
		 * Function Level Reset to attempt restoring the device to its
		 * initial state, and try again.
		 */
		/* [한국어] 컨트롤러 리셋 실패 → PCIe FLR 로 기능 수준 초기화 */
		result = pcie_reset_flr(pdev, false);	/* [한국어] Function Level Reset */
		if (result < 0)
			return result;	/* [한국어] FLR 불가 — probe/reset 실패 */

		pci_restore_state(pdev);	/* [한국어] FLR 전 save 한 cfg 복구 (enable 시 save) */
		result = nvme_disable_ctrl(&dev->ctrl, false);	/* [한국어] FLR 후 재차 CC disable */
		if (result < 0)	/* [한국어] PCI 리셋 뒤에도 안 되면 포기한다 */
			return result;

		dev_info(dev->ctrl.device,
			"controller reset completed after pcie flr\n");	/* [한국어] 복구 성공 가시성 */
	}

	result = nvme_alloc_queue(dev, 0, NVME_AQ_DEPTH);	/* [한국어] admin SQ/CQ DMA 링 할당 */
	if (result)	/* [한국어] admin 큐 메모리 할당 실패 */
		return result;

	dev->ctrl.numa_node = dev_to_node(dev->dev);	/* [한국어] 컨트롤러 NUMA 친화도 */

	nvmeq = &dev->queues[0];	/* [한국어] admin 큐 */
	aqa = nvmeq->q_depth - 1;	/* [한국어] 0-based SQ 크기 */
	aqa |= aqa << 16;	/* [한국어] 상위 16비트=CQ 크기 (동일 depth) */

	writel(aqa, dev->bar + NVME_REG_AQA);	/* [한국어] Admin Queue Attributes */
	lo_hi_writeq(nvmeq->sq_dma_addr, dev->bar + NVME_REG_ASQ);	/* [한국어] Admin SQ 베이스 */
	lo_hi_writeq(nvmeq->cq_dma_addr, dev->bar + NVME_REG_ACQ);	/* [한국어] Admin CQ 베이스 */

	result = nvme_enable_ctrl(&dev->ctrl);	/* [한국어] CC 필드 설정 후 EN=1, RDY=1 대기 */
	if (result)
		return result;	/* [한국어] 핸드셰이크 실패 */

	nvmeq->cq_vector = 0;	/* [한국어] admin 은 벡터 0 */
	nvme_init_queue(nvmeq, 0);	/* [한국어] tail/head/phase/online++ */
	result = queue_request_irq(nvmeq);	/* [한국어] admin MSI-X 핸들러 등록 */
	if (result) {	/* [한국어] admin 큐 인터럽트를 걸지 못했다 — 완료를 받을 길이 없다 */
		dev->online_queues--;	/* [한국어] init_queue 증가분 롤백 */
		return result;	/* [한국어] 온라인 개수를 되돌려 해체 경로가 없는 큐를 건드리지 않게 한다 */
	}

	set_bit(NVMEQ_ENABLED, &nvmeq->flags);	/* [한국어] admin 제출 허용 */
	return result;	/* [한국어] 0 성공 */
}

/*
 * [한국어]
 * nvme_create_io_queues - max_qid 까지 호스트 링 할당 후 Create CQ/SQ
 *
 * 1) alloc_queue 로 SQ/CQ DMA(또는 CMB SQ) 준비
 * 2) DEFAULT+READ 뒤 큐는 POLLED (인터럽트 없음)
 * 3) Create 실패는 음수만 치명 — 양수 status 는 부분 성공으로 0 처리
 * admin-only 컨트롤러도 펌웨어 복구에 유용하므로 I/O 전멸을 허용한다.
 */
static int nvme_create_io_queues(struct nvme_dev *dev)
{
	unsigned i, max, rw_queues;	/* [한국어] 큐 id / 생성 상한 / 비폴 큐 수 */
	int ret = 0;	/* [한국어] 마지막 Create 결과 — 음수만 전파 */

	for (i = dev->ctrl.queue_count; i <= dev->max_qid; i++) {	/* [한국어] 미할당 슬롯 호스트 링 확보 */
		if (nvme_alloc_queue(dev, i, dev->q_depth)) {	/* [한국어] CQ DMA + SQ(CMB/호스트) */
			ret = -ENOMEM;	/* [한국어] 메모리 부족 — 여기까지 확보분으로 진행 */
			break;
		}
	}

	max = min(dev->max_qid, dev->ctrl.queue_count - 1);	/* [한국어] 실제 생성 가능 최대 qid */
	if (max != 1 && dev->io_queues[HCTX_TYPE_POLL]) {	/* [한국어] 폴링 맵이 있으면 뒤쪽을 polled */
		rw_queues = dev->io_queues[HCTX_TYPE_DEFAULT] +
				dev->io_queues[HCTX_TYPE_READ];	/* [한국어] MSI-X 완료 큐 개수 */
	} else {
		rw_queues = max;	/* [한국어] 전부 인터럽트 큐 */
	}

	for (i = dev->online_queues; i <= max; i++) {	/* [한국어] 아직 ENABLED 아닌 큐 Create */
		bool polled = i > rw_queues;	/* [한국어] qid 가 비폴 영역을 넘으면 폴링 */

		ret = nvme_create_queue(&dev->queues[i], i, polled);	/* [한국어] Create CQ/SQ+IRQ+ENABLED */
		if (ret)
			break;	/* [한국어] 실패 지점 이후는 미생성 — 부분 토폴로지 허용 */
	}

	/*
	 * Ignore failing Create SQ/CQ commands, we can continue with less
	 * than the desired amount of queues, and even a controller without
	 * I/O queues can still be used to issue admin commands.  This might
	 * be useful to upgrade a buggy firmware for example.
	 */
	/* [한국어] NVMe status(양수)는 무시하고 음수 errno 만 실패로 전파 */
	return ret >= 0 ? 0 : ret;	/* [한국어] 부분 성공도 probe/reset 계속 */
}

/*
 * [한국어]
 * nvme_cmb_size_unit - CMBSZ.SZU 를 바이트 단위로 (4KB << 4*szu)
 */
static u64 nvme_cmb_size_unit(struct nvme_dev *dev)
{
	u8 szu = (dev->cmbsz >> NVME_CMBSZ_SZU_SHIFT) & NVME_CMBSZ_SZU_MASK;	/* [한국어] 크기 단위 지수 */

	return 1ULL << (12 + 4 * szu);	/* [한국어] 4KiB × 16^szu */
}

/*
 * [한국어]
 * nvme_cmb_size - CMBSZ.SZ 필드 (unit 개수)
 */
static u32 nvme_cmb_size(struct nvme_dev *dev)
{
	return (dev->cmbsz >> NVME_CMBSZ_SZ_SHIFT) & NVME_CMBSZ_SZ_MASK;	/* [한국어] 단위 개수 */
}

/*
 * [한국어]
 * nvme_map_cmb - CMB 위치/크기 읽고 P2PDMA 리소스로 등록
 *
 * CAP.CMBS 면 CMBMSC CRE/CMSE 로 호스트 측 디코드 주소 설정.
 * BAR 보다 큰 CMB 는 BAR 잔여로 clamp. WDS+RDS 면 p2pmem publish.
 * SQ 용 여부는 cmbsz.SQS && use_cmb_sqes.
 */
static void nvme_map_cmb(struct nvme_dev *dev)
{
	u64 size, offset;	/* [한국어] CMB 바이트 크기 / BAR 내 오프셋 */
	resource_size_t bar_size;	/* [한국어] CMB 가 앉은 PCI BAR 길이 */
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] CMB 는 BAR 안의 영역이라 PCI 자원 정보가 필요하다 */
	int bar;	/* [한국어] CMBLOC.BIR — BAR 번호 */

	if (dev->cmb_size)	/* [한국어] 이미 맵됨 — 리셋 재진입 시 스킵 */
		return;	/* [한국어] 이미 매핑돼 있다 — 리셋마다 다시 등록하지 않는다 */

	if (NVME_CAP_CMBS(dev->ctrl.cap))
		writel(NVME_CMBMSC_CRE, dev->bar + NVME_REG_CMBMSC);	/* [한국어] CMB 메모리 공간 enable */

	dev->cmbsz = readl(dev->bar + NVME_REG_CMBSZ);	/* [한국어] 크기/단위/지원 비트 */
	if (!dev->cmbsz)
		return;	/* [한국어] CMB 없음 */
	dev->cmbloc = readl(dev->bar + NVME_REG_CMBLOC);	/* [한국어] BIR + offset */

	size = nvme_cmb_size_unit(dev) * nvme_cmb_size(dev);	/* [한국어] 총 CMB 바이트 */
	offset = nvme_cmb_size_unit(dev) * NVME_CMB_OFST(dev->cmbloc);	/* [한국어] BAR 내 시작 */
	bar = NVME_CMB_BIR(dev->cmbloc);	/* [한국어] 어느 BAR 에 앉는지 */
	bar_size = pci_resource_len(pdev, bar);	/* [한국어] 컨트롤러가 알린 크기가 BAR 을 넘을 수 있어 실제 길이를 확인한다 */

	if (offset > bar_size)
		return;	/* [한국어] 위치 비정상 */

	/*
	 * Controllers may support a CMB size larger than their BAR, for
	 * example, due to being behind a bridge. Reduce the CMB to the
	 * reported size of the BAR
	 */
	/* [한국어] 브리지 뒤 등 BAR 가 잘린 경우 CMB 를 BAR 잔여로 clamp */
	size = min(size, bar_size - offset);	/* [한국어] 위 영어 주석대로, 오프셋 이후 남은 만큼으로 잘라 BAR 밖을 가리키지 않게 한다 */

	if (!IS_ALIGNED(size, memremap_compat_align()) ||
	    !IS_ALIGNED(pci_resource_start(pdev, bar),
			memremap_compat_align()))	/* [한국어] p2pdma/memremap 정렬 요구 */
		return;	/* [한국어] 정렬이 맞지 않으면 memremap 이 실패하므로 아예 쓰지 않는다 */

	/*
	 * Tell the controller about the host side address mapping the CMB,
	 * and enable CMB decoding for the NVMe 1.4+ scheme:
	 */
	if (NVME_CAP_CMBS(dev->ctrl.cap)) {	/* [한국어] NVMe 1.4 부터는 호스트가 CMB 베이스를 직접 지정하고 디코딩을 켜야 한다 */
		hi_lo_writeq(NVME_CMBMSC_CRE | NVME_CMBMSC_CMSE |	/* [한국어] 64bit MMIO 분할 접근 */
			     (pci_bus_address(pdev, bar) + offset),
			     dev->bar + NVME_REG_CMBMSC);	/* [한국어] 호스트 버스 주소 + 디코드 enable */
	}

	if (pci_p2pdma_add_resource(pdev, bar, size, offset)) {	/* [한국어] P2PDMA 리소스 등록 */
		dev_warn(dev->ctrl.device,	/* [한국어] P2PDMA 등록 실패 — CMB 없이도 동작하므로 경고만 남긴다 */
			 "failed to register the CMB\n");
		hi_lo_writeq(0, dev->bar + NVME_REG_CMBMSC);	/* [한국어] 실패 시 CMBMSC 클리어 */
		return;	/* [한국어] 방금 켠 디코딩을 다시 꺼 두었다 */
	}

	dev->cmb_size = size;	/* [한국어] 사용 가능 용량 확정 */
	dev->cmb_use_sqes = use_cmb_sqes && (dev->cmbsz & NVME_CMBSZ_SQS);	/* [한국어] SQ 본문 CMB 배치 가능 */

	if ((dev->cmbsz & (NVME_CMBSZ_WDS | NVME_CMBSZ_RDS)) ==
			(NVME_CMBSZ_WDS | NVME_CMBSZ_RDS))	/* [한국어] 데이터 R/W 모두 지원 */
		pci_p2pmem_publish(pdev, true);	/* [한국어] 피어 장치가 CMB 를 쓰도록 공개 */
}

/*
 * [한국어]
 * nvme_set_host_mem - Set Features(HMB) 로 컨트롤러에 호스트 버퍼 등록/해제
 *
 * dword11=ENABLE/RETURN 비트, dword12=페이지 수, 13-14=디스크립터 DMA,
 * dword15=디스크립터 개수. bits=0 이면 HMB disable (suspend/remove).
 */
static int nvme_set_host_mem(struct nvme_dev *dev, u32 bits)
{
	u32 host_mem_size = dev->host_mem_size >> NVME_CTRL_PAGE_SHIFT;	/* [한국어] 스펙 단위=컨트롤러 페이지 수 */
	u64 dma_addr = dev->host_mem_descs_dma;	/* [한국어] 디스크립터 배열 DMA — 컨트롤러가 읽음 */
	struct nvme_command c = { };	/* [한국어] Set Features SQE */
	int ret;	/* [한국어] admin 동기 결과 */

	c.features.opcode	= nvme_admin_set_features;	/* [한국어] admin opcode */
	c.features.fid		= cpu_to_le32(NVME_FEAT_HOST_MEM_BUF);	/* [한국어] HMB FID */
	c.features.dword11	= cpu_to_le32(bits);	/* [한국어] ENABLE/RETURN 플래그 */
	c.features.dword12	= cpu_to_le32(host_mem_size);	/* [한국어] 총 HMB 페이지 수 */
	c.features.dword13	= cpu_to_le32(lower_32_bits(dma_addr));	/* [한국어] desc 배열 하위 주소 */
	c.features.dword14	= cpu_to_le32(upper_32_bits(dma_addr));	/* [한국어] desc 배열 상위 주소 */
	c.features.dword15	= cpu_to_le32(dev->nr_host_mem_descs);	/* [한국어] 디스크립터 엔트리 수 */

	ret = nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);	/* [한국어] admin_q 동기 제출 */
	if (ret) {	/* [한국어] Set Features(HMB) 가 거절됐다 */
		dev_warn(dev->ctrl.device,
			 "failed to set host mem (err %d, flags %#x).\n",
			 ret, bits);	/* [한국어] HMB 실패 진단 — I/O 는 계속 가능 */
	} else
		dev->hmb = bits & NVME_HOST_MEM_ENABLE;	/* [한국어] 로컬 enable 캐시 (sysfs/suspend) */

	return ret;	/* [한국어] 0 또는 NVMe status/errno */
}

/*
 * [한국어]
 * nvme_free_host_mem_multi - 청크 단위 multi HMB 버퍼 배열 해제
 *
 * 각 디스크립터의 DMA attrs 청크와 VA 추적 배열을 반환. 디스크립터 배열
 * 자체는 nvme_free_host_mem 이 해제한다.
 */
static void nvme_free_host_mem_multi(struct nvme_dev *dev)
{
	int i;	/* [한국어] 디스크립터 인덱스 */

	for (i = 0; i < dev->nr_host_mem_descs; i++) {	/* [한국어] 할당된 청크 전부 */
		struct nvme_host_mem_buf_desc *desc = &dev->host_mem_descs[i];	/* [한국어] 온와이어 desc */
		size_t size = le32_to_cpu(desc->size) * NVME_CTRL_PAGE_SIZE;	/* [한국어] 청크 바이트 */

		dma_free_attrs(dev->dev, size, dev->host_mem_desc_bufs[i],	/* [한국어] 지역 변수 */
			       le64_to_cpu(desc->addr),
			       DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_NO_WARN);	/* [한국어] 커널 맵 없는 HMB 청크 반환 */
	}

	kfree(dev->host_mem_desc_bufs);	/* [한국어] 청크 VA 추적 배열 */
	dev->host_mem_desc_bufs = NULL;	/* [한국어] dangling 방지 */
}

/*
 * [한국어]
 * nvme_free_host_mem - HMB 페이로드+디스크립터 전체 해제
 *
 * single(noncontiguous sgt) 또는 multi 경로 분기 후 coherent desc 배열 free.
 * remove/setup 실패/리셋 재할당 전 호출.
 */
static void nvme_free_host_mem(struct nvme_dev *dev)
{
	if (dev->hmb_sgt)	/* [한국어] single noncontiguous 할당 경로 */
		dma_free_noncontiguous(dev->dev, dev->host_mem_size,	/* [한국어] 지역 변수 */
				dev->hmb_sgt, DMA_BIDIRECTIONAL);	/* [한국어] sgt 일괄 반환 */
	else
		nvme_free_host_mem_multi(dev);	/* [한국어] multi 청크 경로 */

	dma_free_coherent(dev->dev, dev->host_mem_descs_size,
			dev->host_mem_descs, dev->host_mem_descs_dma);	/* [한국어] 디스크립터 배열 DMA 해제 */
	dev->host_mem_descs = NULL;	/* [한국어] 재사용 판정 필드 클리어 */
	dev->host_mem_descs_size = 0;	/* [한국어] 크기 무효 */
	dev->nr_host_mem_descs = 0;	/* [한국어] 엔트리 수 무효 */
}

/*
 * [한국어]
 * nvme_alloc_host_mem_single - IOMMU merge 가능 시 단일 세그먼트 HMB
 *
 * dma_alloc_noncontiguous 로 preferred 크기 확보 후 desc[0] 한 칸에 기록.
 * 컨트롤러 디스크립터 수 상한(hmmaxd) 압박을 줄이는 최적 경로.
 */
static int nvme_alloc_host_mem_single(struct nvme_dev *dev, u64 size)
{
	dev->hmb_sgt = dma_alloc_noncontiguous(dev->dev, size,
				DMA_BIDIRECTIONAL, GFP_KERNEL, 0);	/* [한국어] 가상 비연속·물리 merge HMB */
	if (!dev->hmb_sgt)
		return -ENOMEM;	/* [한국어] multi 폴백 유도 */

	dev->host_mem_descs = dma_alloc_coherent(dev->dev,
			sizeof(*dev->host_mem_descs), &dev->host_mem_descs_dma,
			GFP_KERNEL);	/* [한국어] 단일 디스크립터 슬롯 */
	if (!dev->host_mem_descs) {	/* [한국어] 서술자 배열은 컨트롤러가 DMA 로 읽으므로 coherent 여야 한다 */
		dma_free_noncontiguous(dev->dev, size, dev->hmb_sgt,	/* [한국어] 지역 변수 */
				DMA_BIDIRECTIONAL);	/* [한국어] desc 실패 시 페이로드 롤백 */
		dev->hmb_sgt = NULL;	/* [한국어] free 후 포인터 정리 */
		return -ENOMEM;	/* [한국어] 이미 잡은 SGT 를 풀고 나간다 */
	}
	dev->host_mem_size = size;	/* [한국어] 총 바이트 */
	dev->host_mem_descs_size = sizeof(*dev->host_mem_descs);	/* [한국어] desc 배열 바이트 */
	dev->nr_host_mem_descs = 1;	/* [한국어] 단일 세그먼트 */

	dev->host_mem_descs[0].addr =
		cpu_to_le64(dev->hmb_sgt->sgl->dma_address);	/* [한국어] 컨트롤러가 쓸 시작 DMA */
	dev->host_mem_descs[0].size = cpu_to_le32(size / NVME_CTRL_PAGE_SIZE);	/* [한국어] 페이지 수 */
	return 0;	/* [한국어] Set Features 준비 완료 */
}

/*
 * [한국어]
 * nvme_alloc_host_mem_multi - chunk_size 단위로 HMB 디스크립터 채움
 *
 * preferred/chunk 와 hmmaxd 로 엔트리 상한. NO_KERNEL_MAPPING 청크는
 * 컨트롤러 전용 캐시 — 호스트 CPU 는 내용에 접근하지 않는다.
 */
static int nvme_alloc_host_mem_multi(struct nvme_dev *dev, u64 preferred,
		u32 chunk_size)	/* [한국어] 지역 변수 */
{
	struct nvme_host_mem_buf_desc *descs;	/* [한국어] coherent 디스크립터 배열 */
	u32 max_entries, len, descs_size;	/* [한국어] 상한/청크 길이/배열 바이트 */
	dma_addr_t descs_dma;	/* [한국어] 디스크립터 배열 DMA */
	int i = 0;	/* [한국어] 채운 엔트리 수 */
	void **bufs;	/* [한국어] free 용 VA 추적 (맵 없음 속성) */
	u64 size, tmp;	/* [한국어] 누적 바이트 / 나눗셈 임시 */

	tmp = (preferred + chunk_size - 1);	/* [한국어] 올림 나눗셈 피제수 */
	do_div(tmp, chunk_size);	/* [한국어] 필요 청크 수 */
	max_entries = tmp;	/* [한국어] 희망 엔트리 수 */

	if (dev->ctrl.hmmaxd && dev->ctrl.hmmaxd < max_entries)	/* [한국어] Identify hmmaxd 상한 */
		max_entries = dev->ctrl.hmmaxd;	/* [한국어] 컨트롤러 허용 디스크립터 수 */

	descs_size = max_entries * sizeof(*descs);	/* [한국어] 디스크립터 배열 바이트 */
	descs = dma_alloc_coherent(dev->dev, descs_size, &descs_dma,
			GFP_KERNEL);	/* [한국어] 컨트롤러가 읽는 desc 테이블 */
	if (!descs)
		goto out;	/* [한국어] 테이블 자체 할당 실패 */

	bufs = kzalloc_objs(*bufs, max_entries);	/* [한국어] 청크 VA 배열 */
	if (!bufs)
		goto out_free_descs;	/* [한국어] desc 롤백 */

	for (size = 0; size < preferred && i < max_entries; size += len) {	/* [한국어] preferred 채울 때까지 */
		dma_addr_t dma_addr;	/* [한국어] 이번 청크 DMA */

		len = min_t(u64, chunk_size, preferred - size);	/* [한국어] 마지막 청크 잔여 반영 */
		bufs[i] = dma_alloc_attrs(dev->dev, len, &dma_addr, GFP_KERNEL,
				DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_NO_WARN);	/* [한국어] 컨트롤러 전용 DRAM 청크 */
		if (!bufs[i])
			break;	/* [한국어] 부분 성공 가능 — size 로 판정 */

		descs[i].addr = cpu_to_le64(dma_addr);	/* [한국어] 온와이어 시작 주소 */
		descs[i].size = cpu_to_le32(len / NVME_CTRL_PAGE_SIZE);	/* [한국어] 온와이어 페이지 수 */
		i++;	/* [한국어] 다음 디스크립터 슬롯 */
	}

	if (!size)	/* [한국어] 한 청크도 못 얻음 */
		goto out_free_bufs;	/* [한국어] 완전 실패 */

	dev->nr_host_mem_descs = i;	/* [한국어] 실제 등록할 엔트리 수 */
	dev->host_mem_size = size;	/* [한국어] 실제 확보 바이트 (preferred 미만 가능) */
	dev->host_mem_descs = descs;	/* [한국어] 테이블 VA */
	dev->host_mem_descs_dma = descs_dma;	/* [한국어] 테이블 DMA — Set Features 전달 */
	dev->host_mem_descs_size = descs_size;	/* [한국어] free 크기 */
	dev->host_mem_desc_bufs = bufs;	/* [한국어] free 용 청크 포인터 */
	return 0;	/* [한국어] 부분 할당도 min 검사 전 성공 후보 */

out_free_bufs:
	kfree(bufs);	/* [한국어] VA 배열 폐기 */
out_free_descs:
	dma_free_coherent(dev->dev, descs_size, descs, descs_dma);	/* [한국어] desc 테이블 폐기 */
out:
	dev->host_mem_descs = NULL;	/* [한국어] 실패 상태 명시 */
	return -ENOMEM;	/* [한국어] 상위가 chunk_size 축소 재시도 */
}

/*
 * [한국어]
 * nvme_alloc_host_mem - preferred HMB 확보: single 시도 후 multi 청크 축소 루프
 *
 * min 미달 할당은 free 후 다음 chunk_size. hmminds 아래로는 내려가지 않음.
 */
static int nvme_alloc_host_mem(struct nvme_dev *dev, u64 min, u64 preferred)
{
	unsigned long dma_merge_boundary = dma_get_merge_boundary(dev->dev);	/* [한국어] IOMMU merge 경계 */
	u64 min_chunk = min_t(u64, preferred, PAGE_SIZE * MAX_ORDER_NR_PAGES);	/* [한국어] multi 시작 청크 상한 */
	u64 hmminds = max_t(u32, dev->ctrl.hmminds * 4096, PAGE_SIZE * 2);	/* [한국어] Identify 최소 청크 */
	u64 chunk_size;	/* [한국어] 현재 시도 청크 크기 */

	/*
	 * If there is an IOMMU that can merge pages, try a virtually
	 * non-contiguous allocation for a single segment first.
	 */
	/* [한국어] IOMMU 가 페이지 merge 가능하면 단일 디스크립터 경로 우선 */
	if (dma_merge_boundary && (PAGE_SIZE & dma_merge_boundary) == 0) {	/* [한국어] PAGE 가 merge 경계에 정렬 */
		if (!nvme_alloc_host_mem_single(dev, preferred))	/* [한국어] preferred 단일 sgt */
			return 0;	/* [한국어] 최적 경로 성공 */
	}

	/* start big and work our way down */
	/* [한국어] 큰 청크부터 절반씩 줄이며 multi 할당 시도 */
	for (chunk_size = min_chunk; chunk_size >= hmminds; chunk_size /= 2) {	/* [한국어] hmminds 하한까지 */
		if (!nvme_alloc_host_mem_multi(dev, preferred, chunk_size)) {	/* [한국어] multi 성공 후보 */
			if (!min || dev->host_mem_size >= min)	/* [한국어] 컨트롤러 min 충족 또는 min=0 */
				return 0;	/* [한국어] Set Features 가능 */
			nvme_free_host_mem(dev);	/* [한국어] min 미달 — 버리고 더 작은 청크로 재시도 */
		}
	}

	return -ENOMEM;	/* [한국어] HMB optional — 호출자가 warn 후 계속 */
}

/*
 * [한국어]
 * nvme_setup_host_mem - hmpre/hmmin 과 모듈 max 로 HMB 정책 적용
 *
 * 기존 버퍼 재사용 시 RETURN 비트. 할당 실패는 warn 후 0 (optional 기능).
 * 컨트롤러 DRAM 캐시를 호스트 메모리로 확장해 성능 향상.
 */
static int nvme_setup_host_mem(struct nvme_dev *dev)
{
	u64 max = (u64)max_host_mem_size_mb * SZ_1M;	/* [한국어] 모듈 파라미터 상한 */
	u64 preferred = (u64)dev->ctrl.hmpre * 4096;	/* [한국어] Identify 선호 크기 */
	u64 min = (u64)dev->ctrl.hmmin * 4096;	/* [한국어] 컨트롤러 최소 요구 */
	u32 enable_bits = NVME_HOST_MEM_ENABLE;	/* [한국어] Set Features enable 플래그 */
	int ret;	/* [한국어] 지역 변수 */

	if (!dev->ctrl.hmpre)
		return 0;	/* [한국어] HMB 미지원/미선호 — 스킵 */

	preferred = min(preferred, max);	/* [한국어] 호스트 정책으로 선호 크기 클램프 */
	if (min > max) {	/* [한국어] 컨트롤러가 요구하는 최소가 사용자 상한을 넘는다 — HMB 를 켤 수 없다 */
		dev_warn(dev->ctrl.device,	/* [한국어] 왜 안 켜졌는지 알려 준다 — 조용히 넘어가면 성능 차이를 설명할 수 없다 */
			"min host memory (%lld MiB) above limit (%d MiB).\n",
			min >> ilog2(SZ_1M), max_host_mem_size_mb);
		nvme_free_host_mem(dev);	/* [한국어] 기존 버퍼 정리 */
		return 0;	/* [한국어] optional — 실패해도 probe 계속 */
	}

	/*
	 * If we already have a buffer allocated check if we can reuse it.
	 */
	/* [한국어] 리셋 경로: min 만족 시 RETURN 으로 재등록, 아니면 재할당 */
	if (dev->host_mem_descs) {	/* [한국어] 재시작이라 이미 버퍼가 있다 — 크기가 여전히 충분한지만 본다 */
		if (dev->host_mem_size >= min)
			enable_bits |= NVME_HOST_MEM_RETURN;	/* [한국어] 기존 버퍼 재사용 알림 */
		else
			nvme_free_host_mem(dev);	/* [한국어] 너무 작음 — 새로 할당 */
	}

	if (!dev->host_mem_descs) {	/* [한국어] 없거나 방금 버렸다면 새로 잡는다 */
		if (nvme_alloc_host_mem(dev, min, preferred)) {	/* [한국어] 최소치도 못 잡았다 */
			dev_warn(dev->ctrl.device,	/* [한국어] HMB 는 선택 기능이라 실패해도 컨트롤러는 동작한다 */
				"failed to allocate host memory buffer.\n");
			return 0; /* controller must work without HMB */ /* [한국어] HMB 없이도 I/O 가능 */
		}

		dev_info(dev->ctrl.device,	/* [한국어] 실제로 얼마를 빌려 줬는지 남긴다 — 요청량과 다를 수 있다 */
			"allocated %lld MiB host memory buffer (%u segment%s).\n",	/* [한국어] 세그먼트 수도 함께 — 조각날수록 컨트롤러 쪽 효율이 떨어진다 */
			dev->host_mem_size >> ilog2(SZ_1M),
			dev->nr_host_mem_descs,
			str_plural(dev->nr_host_mem_descs));
	}

	ret = nvme_set_host_mem(dev, enable_bits);	/* [한국어] 버퍼 주소를 알리고 사용을 켠다 */
	if (ret)	/* [한국어] 컨트롤러가 거절했다면 빌려 준 메모리를 회수한다 */
		nvme_free_host_mem(dev);
	return ret;
}

/*
 * [한국어] sysfs cmb — CMBLOC/CMBSZ 레지스터 스냅샷 (디버그·용량 확인)
 */
static ssize_t cmb_show(struct device *dev, struct device_attribute *attr,
		char *buf)	/* [한국어] 지역 변수 */
{
	struct nvme_dev *ndev = to_nvme_dev(dev_get_drvdata(dev));	/* [한국어] char-dev → nvme_dev */

	return sysfs_emit(buf, "cmbloc : 0x%08x\ncmbsz  : 0x%08x\n",
		       ndev->cmbloc, ndev->cmbsz);	/* [한국어] BAR 위치·크기/능력 비트 */
}
static DEVICE_ATTR_RO(cmb);	/* [한국어] /sys/.../cmb 읽기 전용 */

/*
 * [한국어] sysfs cmbloc — CMB 위치 레지스터 단독 노출
 */
static ssize_t cmbloc_show(struct device *dev, struct device_attribute *attr,
		char *buf)	/* [한국어] 지역 변수 */
{
	struct nvme_dev *ndev = to_nvme_dev(dev_get_drvdata(dev));	/* [한국어] PCIe 트랜스포트 인스턴스 */

	return sysfs_emit(buf, "%u\n", ndev->cmbloc);	/* [한국어] BIR+offset 원시 값 */
}
static DEVICE_ATTR_RO(cmbloc);	/* [한국어] /sys/.../cmbloc */

/*
 * [한국어] sysfs cmbsz — CMB 크기/SQS·WDS·RDS 능력 비트
 */
static ssize_t cmbsz_show(struct device *dev, struct device_attribute *attr,
		char *buf)	/* [한국어] 지역 변수 */
{
	struct nvme_dev *ndev = to_nvme_dev(dev_get_drvdata(dev));	/* [한국어] 컨트롤러 */

	return sysfs_emit(buf, "%u\n", ndev->cmbsz);	/* [한국어] CMBSZ 캐시 */
}
static DEVICE_ATTR_RO(cmbsz);	/* [한국어] /sys/.../cmbsz */

/*
 * [한국어] sysfs hmb — Host Memory Buffer feature enable 여부 (0/1)
 */
static ssize_t hmb_show(struct device *dev, struct device_attribute *attr,
			char *buf)	/* [한국어] 지역 변수 */
{
	struct nvme_dev *ndev = to_nvme_dev(dev_get_drvdata(dev));	/* [한국어] hmb 캐시 보유자 */

	return sysfs_emit(buf, "%d\n", ndev->hmb);	/* [한국어] Set Features 반영 로컬 상태 */
}

/*
 * [한국어]
 * hmb_store - 런타임 HMB on/off. on→setup_host_mem, off→Set Features 0 + free
 */
static ssize_t hmb_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct nvme_dev *ndev = to_nvme_dev(dev_get_drvdata(dev));	/* [한국어] 대상 컨트롤러 */
	bool new;	/* [한국어] 사용자 요청 enable 여부 */
	int ret;	/* [한국어] setup/set 결과 */

	if (kstrtobool(buf, &new) < 0)	/* [한국어] "0"/"1"/"Y"/"N" 파싱 */
		return -EINVAL;	/* [한국어] 형식 오류 */

	if (new == ndev->hmb)	/* [한국어] 상태 동일 — no-op */
		return count;	/* [한국어] 성공처럼 바이트 수 반환 */

	if (new) {	/* [한국어] sysfs 로 HMB 를 켜는 요청 — 끄는 요청과 경로가 다르다 */
		ret = nvme_setup_host_mem(ndev);	/* [한국어] 할당+Set Features ENABLE */
	} else {
		ret = nvme_set_host_mem(ndev, 0);	/* [한국어] 컨트롤러 HMB disable */
		if (!ret)
			nvme_free_host_mem(ndev);	/* [한국어] 호스트 측 버퍼 반환 */
	}

	if (ret < 0)
		return ret;	/* [한국어] 에러를 sysfs write 에 전파 */

	return count;	/* [한국어] 전체 입력 소비 */
}
static DEVICE_ATTR_RW(hmb);	/* [한국어] /sys/.../hmb 읽기/쓰기 */

/*
 * [한국어]
 * nvme_pci_attrs_are_visible - CMB/HMB sysfs 노드 조건부 노출
 *
 * cmbsz=0 이면 CMB 속성 숨김. hmpre=0 이면 hmb 숨김. 리셋 후 update 로 재평가.
 */
static umode_t nvme_pci_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)	/* [한국어] 지역 변수 */
{
	struct nvme_ctrl *ctrl =
		dev_get_drvdata(container_of(kobj, struct device, kobj));	/* [한국어] char device → ctrl */
	struct nvme_dev *dev = to_nvme_dev(ctrl);	/* [한국어] PCIe 전용 필드 */

	if (a == &dev_attr_cmb.attr ||
	    a == &dev_attr_cmbloc.attr ||
	    a == &dev_attr_cmbsz.attr) {	/* [한국어] CMB 계열 속성 */
	    	if (!dev->cmbsz)	/* [한국어] CMB 미지원/미맵 */
			return 0;	/* [한국어] 노드 숨김 */
	}
	if (a == &dev_attr_hmb.attr && !ctrl->hmpre)	/* [한국어] Identify 가 HMB 미선호 */
		return 0;	/* [한국어] hmb sysfs 숨김 */

	return a->mode;	/* [한국어] 기본 권한(RO/RW) 유지 */
}

/* [한국어] PCIe 전용 sysfs 속성 목록 — CMB/HMB */
static struct attribute *nvme_pci_attrs[] = {
	&dev_attr_cmb.attr,	/* [한국어] 복합 CMB 덤프 */
	&dev_attr_cmbloc.attr,	/* [한국어] CMB 위치 */
	&dev_attr_cmbsz.attr,	/* [한국어] CMB 크기/능력 */
	&dev_attr_hmb.attr,	/* [한국어] HMB enable 토글 */
	NULL,	/* [한국어] 센티널 */
};

/* [한국어] is_visible 훅이 붙은 PCIe attr group */
static const struct attribute_group nvme_pci_dev_attrs_group = {
	.attrs		= nvme_pci_attrs,	/* [한국어] 속성 배열 */
	.is_visible	= nvme_pci_attrs_are_visible,	/* [한국어] 능력 기반 노출 */
};

/* [한국어] ctrl_ops.dev_attr_groups — 공통+PCIe 그룹 연결 */
static const struct attribute_group *nvme_pci_dev_attr_groups[] = {
	&nvme_dev_attrs_group,	/* [한국어] host 공통 속성 */
	&nvme_pci_dev_attrs_group,	/* [한국어] CMB/HMB */
	NULL,	/* [한국어] 센티널 */
};

/*
 * [한국어]
 * nvme_update_attrs - 리셋/HMB 변경 후 CMB·HMB sysfs 가시성 재계산
 */
static void nvme_update_attrs(struct nvme_dev *dev)
{
	sysfs_update_group(&dev->ctrl.device->kobj, &nvme_pci_dev_attrs_group);	/* [한국어] is_visible 재평가 */
}

/*
 * nirqs is the number of interrupts available for write and read
 * queues. The core already reserved an interrupt for the admin queue.
 */
/*
 * [한국어]
 * nvme_calc_irq_sets - 가용 I/O 인터럽트로 DEFAULT/READ affinity 세트 분할
 *
 * write_queues 모듈 파라미터와 벡터 수로 읽기 전용 맵 크기를 결정.
 * admin 벡터는 pre_vectors 로 이미 제외된 nrirqs 가 들어온다.
 */
static void nvme_calc_irq_sets(struct irq_affinity *affd, unsigned int nrirqs)
{
	struct nvme_dev *dev = affd->priv;	/* [한국어] setup_irqs 가 넣은 nvme_dev */
	unsigned int nr_read_queues, nr_write_queues = dev->nr_write_queues;	/* [한국어] 분할 입력 */

	/*
	 * If there is no interrupt available for queues, ensure that
	 * the default queue is set to 1. The affinity set size is
	 * also set to one, but the irq core ignores it for this case.
	 *
	 * If only one interrupt is available or 'write_queue' == 0, combine
	 * write and read queues.
	 *
	 * If 'write_queues' > 0, ensure it leaves room for at least one read
	 * queue.
	 */
	/* [한국어] 벡터 부족 시 R/W 공유, 충분하면 write 전용+최소 1 read */
	if (!nrirqs) {	/* [한국어] I/O 벡터 0 — 이론상 방어 */
		nrirqs = 1;	/* [한국어] 최소 1 큐 가정 */
		nr_read_queues = 0;	/* [한국어] 전부 DEFAULT */
	} else if (nrirqs == 1 || !nr_write_queues) {	/* [한국어] 단일 벡터 또는 write 분리 안 함 */
		nr_read_queues = 0;	/* [한국어] R/W 공유 DEFAULT 맵 */
	} else if (nr_write_queues >= nrirqs) {	/* [한국어] write 요청이 벡터를 잠식 */
		nr_read_queues = 1;	/* [한국어] 읽기 최소 1 보장 */
	} else {
		nr_read_queues = nrirqs - nr_write_queues;	/* [한국어] 나머지 벡터=READ 맵 */
	}

	dev->io_queues[HCTX_TYPE_DEFAULT] = nrirqs - nr_read_queues;	/* [한국어] 쓰기 포함 기본 맵 큐 수 */
	affd->set_size[HCTX_TYPE_DEFAULT] = nrirqs - nr_read_queues;	/* [한국어] irq 코어 affinity 세트 크기 */
	dev->io_queues[HCTX_TYPE_READ] = nr_read_queues;	/* [한국어] 읽기 전용 맵 */
	affd->set_size[HCTX_TYPE_READ] = nr_read_queues;	/* [한국어] READ affinity 세트 */
	affd->nr_sets = nr_read_queues ? 2 : 1;	/* [한국어] 세트 개수 — map_queues 가 참조 */
}

/*
 * [한국어]
 * nvme_setup_irqs - admin+I/O 용 MSI/MSI-X 벡터 할당 + affinity sets
 *
 * poll 큐는 벡터 불필요. SINGLE_VECTOR quirk 는 1벡터. BROKEN_MSI 시 MSI 제외.
 * pre_vectors=1 (admin). calc_sets 가 DEFAULT/READ 분할.
 */
static int nvme_setup_irqs(struct nvme_dev *dev, unsigned int nr_io_queues)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] 벡터 할당은 PCI 계층이 한다 */
	struct irq_affinity affd = {	/* [한국어] 지역 변수 */
		.pre_vectors	= 1,	/* [한국어] 벡터0=admin 예약 */
		.calc_sets	= nvme_calc_irq_sets,	/* [한국어] 가용 벡터로 write/read 세트 계산 */
		.priv		= dev,	/* [한국어] calc_sets 가 읽는 nvme_dev */
	};
	unsigned int irq_queues, poll_queues;	/* [한국어] 지역 변수 */
	unsigned int flags = PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY;	/* [한국어] 타입 자동 + CPU affinity */

	/*
	 * Poll queues don't need interrupts, but we need at least one I/O queue
	 * left over for non-polled I/O.
	 */
	poll_queues = min(dev->nr_poll_queues, nr_io_queues - 1);	/* [한국어] 비폴 I/O 최소 1 확보 */
	dev->io_queues[HCTX_TYPE_POLL] = poll_queues;	/* [한국어] 폴링 맵 크기 */

	/*
	 * Initialize for the single interrupt case, will be updated in
	 * nvme_calc_irq_sets().
	 */
	dev->io_queues[HCTX_TYPE_DEFAULT] = 1;	/* [한국어] calc 전 기본값 */
	dev->io_queues[HCTX_TYPE_READ] = 0;	/* [한국어] 위 영어 주석대로의 최소 구성 — 기본 큐 하나만 두고 읽기 전용 큐는 없다 */

	/*
	 * We need interrupts for the admin queue and each non-polled I/O queue,
	 * but some Apple controllers require all queues to use the first
	 * vector.
	 */
	irq_queues = 1;	/* [한국어] admin 최소 1 */
	if (!(dev->ctrl.quirks & NVME_QUIRK_SINGLE_VECTOR))
		irq_queues += (nr_io_queues - poll_queues);	/* [한국어] 비폴 I/O 큐당 벡터 */
	if (dev->ctrl.quirks & NVME_QUIRK_BROKEN_MSI)
		flags &= ~PCI_IRQ_MSI;	/* [한국어] 깨진 MSI 제외 — MSI-X/INTx */
	return pci_alloc_irq_vectors_affinity(pdev, 1, irq_queues, flags,
					      &affd);	/* [한국어] 실제 할당 수 반환(>=1) */
}

/*
 * [한국어]
 * nvme_max_io_queues - 호스트가 목표로 하는 최대 I/O 큐 수
 *
 * SHARED_TAGS quirk(Apple) 는 admin 과 태그 풀 공유 → I/O 큐 1개 강제.
 * 그 외 CPU 가능 큐 + write/poll 특수 큐 가산 (이후 컨트롤러와 협상).
 */
static unsigned int nvme_max_io_queues(struct nvme_dev *dev)
{
	/*
	 * If tags are shared with admin queue (Apple bug), then
	 * make sure we only use one IO queue.
	 */
	/* [한국어] admin 태그 고갈 방지 — 단일 I/O 큐로 풀 공유 폭 제한 */
	if (dev->ctrl.quirks & NVME_QUIRK_SHARED_TAGS)
		return 1;	/* [한국어] Apple 등 공유 태그 컨트롤러 */
	return blk_mq_num_possible_queues(0) + dev->nr_write_queues +
		dev->nr_poll_queues;	/* [한국어] 기본 맵 + write/poll 가산 상한 */
}

/*
 * [한국어]
 * nvme_setup_io_queues - I/O 큐 토폴로지 전체 구성 (리셋/probe 공통)
 *
 * set_queue_count → admin IRQ 임시 해제 → CMB 깊이 조정 → BAR remap 루프
 * → free_irq_vectors → setup_irqs → admin IRQ 재등록 → create_io_queues.
 * 실제 online < max 면 축소 재시도. shutdown_lock 구간이 disable 과 레이스.
 */
static int nvme_setup_io_queues(struct nvme_dev *dev)
{
	struct nvme_queue *adminq = &dev->queues[0];	/* [한국어] 벡터 재할당 중 건드리는 admin */
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] 벡터 재할당과 CMB 크기 조회에 필요하다 */
	unsigned int nr_io_queues;	/* [한국어] 협상·축소되는 I/O 큐 목표 */
	unsigned long size;	/* [한국어] 필요 BAR 맵 크기 */
	int result;	/* [한국어] 단계 에러 */

	/*
	 * Sample the module parameters once at reset time so that we have
	 * stable values to work with.
	 */
	/* [한국어] 리셋 중 모듈 파라미터 변경 레이스 방지 — 스냅샷 */
	dev->nr_write_queues = write_queues;	/* [한국어] DEFAULT 맵 분할 입력 */
	dev->nr_poll_queues = poll_queues;	/* [한국어] POLL 맵 입력 */

	if (dev->ctrl.tagset) {	/* [한국어] 리셋 재진입이다 — 이미 태그셋이 있으면 큐 수만 조정하는 경로로 간다 */
		/*
		 * The set's maps are allocated only once at initialization
		 * time. We can't add special queues later if their mq_map
		 * wasn't preallocated.
		 */
		/* [한국어] 기존 tagset 맵 슬롯 부족 시 poll/write 특수 맵 비활성 */
		if (dev->ctrl.tagset->nr_maps < 3)	/* [한국어] POLL 맵 미사전할당 */
			dev->nr_poll_queues = 0;	/* [한국어] 폴링 큐 요청 무시 */
		if (dev->ctrl.tagset->nr_maps < 2)	/* [한국어] READ 맵 없음 */
			dev->nr_write_queues = 0;	/* [한국어] write 분리 불가 */
	}

	/*
	 * The initial number of allocated queue slots may be too large if the
	 * user reduced the special queue parameters. Cap the value to the
	 * number we need for this round.
	 */
	/* [한국어] queues[] 슬롯과 목표 큐 수 상한 교집합 */
	nr_io_queues = min(nvme_max_io_queues(dev),
			   dev->nr_allocated_queues - 1);	/* [한국어] -1=admin 슬롯 제외 */
	result = nvme_set_queue_count(&dev->ctrl, &nr_io_queues);	/* [한국어] Set Features Number of Queues 협상 */
	if (result < 0)
		return result;	/* [한국어] admin 협상 실패 */

	if (nr_io_queues == 0)	/* [한국어] 컨트롤러가 I/O 큐 0 개만 허용 */
		return 0;	/* [한국어] admin-only 로 진행 가능 */

	/*
	 * Free IRQ resources as soon as NVMEQ_ENABLED bit transitions
	 * from set to unset. If there is a window to it is truely freed,
	 * pci_free_irq_vectors() jumping into this window will crash.
	 * And take lock to avoid racing with pci_free_irq_vectors() in
	 * nvme_dev_disable() path.
	 */
	/* [한국어] ENABLED 클리어와 free_irq_vectors 레이스 방지 — shutdown_lock */
	result = nvme_setup_io_queues_trylock(dev);	/* [한국어] disable 중이면 -ENODEV */
	if (result)	/* [한국어] 해체와 경쟁 중이면 큐 재구성을 시작하지 않는다 */
		return result;
	if (test_and_clear_bit(NVMEQ_ENABLED, &adminq->flags))	/* [한국어] admin 제출 게이트 닫기 */
		pci_free_irq(pdev, 0, adminq);	/* [한국어] 벡터0 핸들러 선해제 — 재할당 준비 */

	if (dev->cmb_use_sqes) {	/* [한국어] CMB 에 I/O SQ 배치 시도 */
		result = nvme_cmb_qdepth(dev, nr_io_queues,
				sizeof(struct nvme_command));	/* [한국어] CMB 용량→깊이 */
		if (result > 0) {	/* [한국어] CMB 에 SQ 를 넣을 수 있는 깊이가 나왔다 — 그만큼으로 낮춘다 */
			dev->q_depth = result;	/* [한국어] 축소된 깊이 적용 */
			dev->ctrl.sqsize = result - 1;	/* [한국어] 0-based Create 필드 */
		} else {
			dev->cmb_use_sqes = false;	/* [한국어] 호스트 메모리 SQ 폴백 */
		}
	}

	do {	/* [한국어] BAR 재매핑은 실패할 수 있어 성공할 때까지 큐 수를 줄여 가며 시도한다 */
		size = db_bar_size(dev, nr_io_queues);	/* [한국어] 전 도어벨 커버 맵 크기 */
		result = nvme_remap_bar(dev, size);	/* [한국어] BAR0 ioremap 확장 */
		if (!result)
			break;	/* [한국어] 맵 성공 */
		if (!--nr_io_queues) {	/* [한국어] 큐 수 줄여 재시도 — 0 되면 실패 */
			result = -ENOMEM;	/* [한국어] BAR 물리 길이 부족 */
			goto out_unlock;	/* [한국어] 락 해제 후 반환 */
		}
	} while (1);	/* [한국어] remap 성공할 때까지 큐 수 축소 */
	adminq->q_db = dev->dbs;	/* [한국어] remap 후 admin SQ doorbell 포인터 갱신 */

 retry:
	/* Deregister the admin queue's interrupt */
	/* [한국어] 벡터 풀 재할당 전 admin 핸들러 제거 (ENABLED 원자 클리어) */
	if (test_and_clear_bit(NVMEQ_ENABLED, &adminq->flags))
		pci_free_irq(pdev, 0, adminq);	/* [한국어] 벡터0 해제 */

	/*
	 * If we enable msix early due to not intx, disable it again before
	 * setting up the full range we need.
	 */
	/* [한국어] pci_enable 때 임시 1벡터 포함 전부 풀 해제 후 본 할당 */
	pci_free_irq_vectors(pdev);	/* [한국어] MSI-X/MSI 풀 전체 반환 */

	result = nvme_setup_irqs(dev, nr_io_queues);	/* [한국어] admin+I/O 벡터+affinity */
	if (result <= 0) {	/* [한국어] 벡터를 하나도 못 받았다 — 인터럽트 없이는 I/O 큐를 쓸 수 없다 */
		result = -EIO;	/* [한국어] 벡터 할당 실패 */
		goto out_unlock;
	}

	dev->num_vecs = result;	/* [한국어] 실제 할당 벡터 수 (admin 포함) */
	result = max(result - 1, 1);	/* [한국어] 비폴 I/O 큐 수 하한 1 */
	dev->max_qid = result + dev->io_queues[HCTX_TYPE_POLL];	/* [한국어] 폴링 큐 가산 최대 qid */

	/*
	 * Should investigate if there's a performance win from allocating
	 * more queues than interrupt vectors; it might allow the submission
	 * path to scale better, even if the receive path is limited by the
	 * number of interrupts.
	 */
	/* [한국어] admin 벡터 핸들러 재등록 — Create I/O 전 admin 경로 복구 */
	result = queue_request_irq(adminq);	/* [한국어] 벡터0 ← nvme_irq */
	if (result)	/* [한국어] 벡터 배치가 바뀌었으므로 admin 큐 인터럽트를 다시 건다 */
		goto out_unlock;
	set_bit(NVMEQ_ENABLED, &adminq->flags);	/* [한국어] admin 제출 재허용 */
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] Create 중 일부 구간은 락 밖 */

	result = nvme_create_io_queues(dev);	/* [한국어] Create CQ/SQ 루프 */
	if (result || dev->online_queues < 2)	/* [한국어] I/O 전멸 또는 치명 에러 */
		return result;	/* [한국어] admin-only 허용 시 0 가능 */

	if (dev->online_queues - 1 < dev->max_qid) {	/* [한국어] 희망보다 적게 생성됨 */
		nr_io_queues = dev->online_queues - 1;	/* [한국어] 실제 수에 맞춰 재시도 */
		nvme_delete_io_queues(dev);	/* [한국어] 부분 생성분 Delete */
		result = nvme_setup_io_queues_trylock(dev);	/* [한국어] 재진입 락 */
		if (result)	/* [한국어] 큐를 지운 뒤 다시 락을 잡는 사이에 해체가 시작됐을 수 있다 */
			return result;
		nvme_suspend_io_queues(dev);	/* [한국어] ENABLED/IRQ 정리 후 retry */
		goto retry;	/* [한국어] 벡터 수·max_qid 재계산 */
	}
	dev_info(dev->ctrl.device, "%d/%d/%d default/read/poll queues\n",
					dev->io_queues[HCTX_TYPE_DEFAULT],
					dev->io_queues[HCTX_TYPE_READ],
					dev->io_queues[HCTX_TYPE_POLL]);	/* [한국어] 최종 토폴로지 로그 */
	return 0;	/* [한국어] I/O 큐 토폴로지 완성 */
out_unlock:
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] 에러 경로 락 해제 */
	return result;	/* [한국어] 음수 errno */
}

/*
 * [한국어]
 * nvme_del_queue_end - 비동기 Delete SQ/CQ 완료 공통 end_io
 *
 * request free 후 delete_done 시그널 — __nvme_delete_io_queues 가 대기.
 */
static enum rq_end_io_ret nvme_del_queue_end(struct request *req,
					     blk_status_t error,	/* [한국어] 지역 변수 */
					     const struct io_comp_batch *iob)
{
	struct nvme_queue *nvmeq = req->end_io_data;	/* [한국어] 삭제 대상 큐 */

	blk_mq_free_request(req);	/* [한국어] admin 태그 반환 */
	complete(&nvmeq->delete_done);	/* [한국어] 대기자(delete 루프) 깨움 */
	return RQ_END_IO_NONE;	/* [한국어] 추가 complete 없음 */
}

/*
 * [한국어]
 * nvme_del_cq_end - Delete CQ 전용 end_io. 에러 시 DELETE_ERROR 비트
 *
 * CQ delete 실패는 컨트롤러 상태 불일치 단서 — 이후 정책에 사용.
 */
static enum rq_end_io_ret nvme_del_cq_end(struct request *req,
					  blk_status_t error,	/* [한국어] 지역 변수 */
					  const struct io_comp_batch *iob)
{
	struct nvme_queue *nvmeq = req->end_io_data;	/* [한국어] 삭제 대상 CQ 큐 */

	if (error)	/* [한국어] NVMe status 또는 전송 실패 */
		set_bit(NVMEQ_DELETE_ERROR, &nvmeq->flags);	/* [한국어] 비정상 tear-down 기록 */

	return nvme_del_queue_end(req, error, iob);	/* [한국어] free+complete 공통 */
}

/*
 * [한국어]
 * nvme_delete_queue - Delete SQ 또는 CQ admin 을 nowait 제출
 *
 * NOWAIT 할당 실패 시 호출자가 배치를 중단. end_io_data=nvmeq 로 완료 연결.
 */
static int nvme_delete_queue(struct nvme_queue *nvmeq, u8 opcode)
{
	struct request_queue *q = nvmeq->dev->ctrl.admin_q;	/* [한국어] Delete 는 admin 경로 */
	struct request *req;	/* [한국어] admin 태그 요청 */
	struct nvme_command cmd = { };	/* [한국어] Delete Queue SQE */

	cmd.delete_queue.opcode = opcode;	/* [한국어] delete_sq 또는 delete_cq */
	cmd.delete_queue.qid = cpu_to_le16(nvmeq->qid);	/* [한국어] 대상 큐 ID */

	req = blk_mq_alloc_request(q, nvme_req_op(&cmd), BLK_MQ_REQ_NOWAIT);	/* [한국어] 블로킹 없이 태그 */
	if (IS_ERR(req))
		return PTR_ERR(req);	/* [한국어] 태그 고갈 — 배치 중단 */
	nvme_init_request(req, &cmd);	/* [한국어] core 요청 메타+cmd 연결 */

	if (opcode == nvme_admin_delete_cq)	/* [한국어] CQ 삭제는 에러 비트 경로 */
		req->end_io = nvme_del_cq_end;	/* [한국어] DELETE_ERROR 가능 */
	else
		req->end_io = nvme_del_queue_end;	/* [한국어] SQ 삭제 공통 완료 */
	req->end_io_data = nvmeq;	/* [한국어] completion 대상 큐 */

	init_completion(&nvmeq->delete_done);	/* [한국어] 대기 전 completion 재초기화 */
	blk_execute_rq_nowait(req, false);	/* [한국어] admin SQ 비동기 투입 */
	return 0;	/* [한국어] 제출 성공 — 완료는 end_io */
}

/*
 * [한국어]
 * __nvme_delete_io_queues - 전 I/O 큐에 동일 Delete opcode 병렬 전송·대기
 *
 * admin 타임아웃 예산 안에서 배치 제출 후 completion 대기. 타임아웃 시
 * false — 호출자가 CQ delete 를 건너뛸 수 있다.
 */
static bool __nvme_delete_io_queues(struct nvme_dev *dev, u8 opcode)
{
	int nr_queues = dev->online_queues - 1, sent = 0;	/* [한국어] 남은 큐 / 비행 중 Delete 수 */
	unsigned long timeout;	/* [한국어] 공유 admin 타임아웃 잔여 jiffies */

 retry:
	timeout = NVME_ADMIN_TIMEOUT;	/* [한국어] 배치마다 타임아웃 재장전 */
	while (nr_queues > 0) {	/* [한국어] 아직 제출 안 한 큐 */
		if (nvme_delete_queue(&dev->queues[nr_queues], opcode))	/* [한국어] 태그 실패 시 중단 */
			break;
		nr_queues--;	/* [한국어] 다음 낮은 qid */
		sent++;	/* [한국어] 비행 중 카운트 */
	}
	while (sent) {	/* [한국어] 제출분 완료 대기 */
		struct nvme_queue *nvmeq = &dev->queues[nr_queues + sent];	/* [한국어] 가장 최근 제출 큐 */

		timeout = wait_for_completion_io_timeout(&nvmeq->delete_done,
				timeout);	/* [한국어] I/O 대기 — 잔여 시간 갱신 */
		if (timeout == 0)	/* [한국어] admin 타임아웃 소진 */
			return false;	/* [한국어] 하드웨어 무응답 — 상위에서 중단 */

		sent--;	/* [한국어] 한 건 완료 */
		if (nr_queues)	/* [한국어] 태그 부족으로 남은 큐 재제출 */
			goto retry;	/* [한국어] 다음 배치 */
	}
	return true;	/* [한국어] 전 큐 Delete 완료 */
}

/*
 * [한국어]
 * nvme_delete_io_queues - 스펙 순서: 먼저 전 SQ Delete, 성공 시 CQ Delete
 *
 * SQ 가 남아 있는 채 CQ 를 지우면 컨트롤러 미정의 동작 — SQ 선행 필수.
 */
static void nvme_delete_io_queues(struct nvme_dev *dev)
{
	if (__nvme_delete_io_queues(dev, nvme_admin_delete_sq))	/* [한국어] Submission Queue 먼저 */
		__nvme_delete_io_queues(dev, nvme_admin_delete_cq);	/* [한국어] SQ 성공 후에만 CQ */
}

/*
 * [한국어]
 * nvme_pci_nr_maps - blk-mq nr_maps (1=DEFAULT, 2=+READ, 3=+POLL)
 */
static unsigned int nvme_pci_nr_maps(struct nvme_dev *dev)
{
	if (dev->io_queues[HCTX_TYPE_POLL])	/* [한국어] 폴링 큐 존재 */
		return 3;	/* [한국어] DEFAULT+READ+POLL */
	if (dev->io_queues[HCTX_TYPE_READ])	/* [한국어] 읽기 분리 맵 */
		return 2;	/* [한국어] DEFAULT+READ */
	return 1;	/* [한국어] 단일 DEFAULT 맵 */
}

/*
 * [한국어]
 * nvme_pci_update_nr_queues - online I/O 큐 수를 tagset 에 반영
 *
 * tagset 없으면 최초 alloc. 있으면 trylock 하 update_nr_hw_queues 후
 * 잉여 queues[] 슬롯 free. disable 레이스 시 false.
 */
static bool nvme_pci_update_nr_queues(struct nvme_dev *dev)
{
	if (!dev->ctrl.tagset) {	/* [한국어] 첫 LIVE — tagset 미생성 */
		nvme_alloc_io_tag_set(&dev->ctrl, &dev->tagset, &nvme_mq_ops,
				nvme_pci_nr_maps(dev), sizeof(struct nvme_iod));	/* [한국어] I/O blk-mq+iod PDU */
		return true;	/* [한국어] 신규 할당 성공 취급 */
	}

	/* Give up if we are racing with nvme_dev_disable() */
	/* [한국어] disable 가 shutdown_lock 보유 중이면 포기 */
	if (!mutex_trylock(&dev->shutdown_lock))
		return false;	/* [한국어] reset_work 가 실패 경로로 */

	/* Check if nvme_dev_disable() has been executed already */
	/* [한국어] 이미 online=0 이면 갱신 무의미 */
	if (!dev->online_queues) {	/* [한국어] 위 영어 주석대로, 이미 해체가 끝났으면 두 번 하지 않는다 */
		mutex_unlock(&dev->shutdown_lock);
		return false;	/* [한국어] 컨트롤러 정지됨 */
	}

	blk_mq_update_nr_hw_queues(&dev->tagset, dev->online_queues - 1);	/* [한국어] hctx 수= I/O 큐 수 */
	/* free previously allocated queues that are no longer usable */
	/* [한국어] online 너머 슬롯의 SQ/CQ DMA 반환 */
	nvme_free_queues(dev, dev->online_queues);	/* [한국어] 잉여 큐 free */
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] 큐를 모두 반납했다 */
	return true;	/* [한국어] tagset 과 하드웨어 큐 수 일치 */
}

/*
 * [한국어]
 * nvme_pci_enable - PCI 메모리 enable + 1벡터 + CAP 파싱 + admin configure
 *
 * CSTS 전1 이면 장치 미응답. q_depth=min(MQES+1,module). db_stride,
 * Apple 128B SQE, Samsung PM1725 등 quirk. CMB map, save_state, admin queue.
 */
static int nvme_pci_enable(struct nvme_dev *dev)
{
	int result = -ENOMEM;	/* [한국어] 기본 실패 코드 — 단계별 갱신 */
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] 장치 활성화와 벡터 할당에 필요하다 */
	unsigned int flags = PCI_IRQ_ALL_TYPES;	/* [한국어] MSI-X/MSI/INTx 모두 시도 */

	if (pci_enable_device_mem(pdev))	/* [한국어] MEM BAR 디코드 enable */
		return result;	/* [한국어] 전원/리소스 실패 */

	pci_set_master(pdev);	/* [한국어] Bus Master — 컨트롤러 DMA 허용 */

	if (readl(dev->bar + NVME_REG_CSTS) == -1) {	/* [한국어] 전비트1 = 링크/장치 사망 징후 */
		dev_dbg(dev->ctrl.device, "reading CSTS register failed\n");	/* [한국어] 모두 1 이 읽히면 장치가 버스에서 사라진 것이다 */
		result = -ENODEV;	/* [한국어] 장치 없음으로 처리 */
		goto disable;	/* [한국어] 더 진행해도 모든 레지스터 접근이 헛돈다 */
	}

	/*
	 * Some devices and/or platforms don't advertise or work with INTx
	 * interrupts. Pre-enable a single MSIX or MSI vec for setup. We'll
	 * adjust this later.
	 */
	/* [한국어] setup 단계 단일 벡터 — 이후 setup_io_queues 가 본 할당 */
	if (dev->ctrl.quirks & NVME_QUIRK_BROKEN_MSI)
		flags &= ~PCI_IRQ_MSI;	/* [한국어] 깨진 MSI 제외 */
	result = pci_alloc_irq_vectors(pdev, 1, 1, flags);	/* [한국어] admin 용 임시 1벡터 */
	if (result < 0)	/* [한국어] admin 큐용 벡터 하나조차 못 받았다 */
		goto disable;

	dev->ctrl.cap = lo_hi_readq(dev->bar + NVME_REG_CAP);	/* [한국어] CAP — MQES/DSTRD/CSS/CMBS 등 */

	dev->q_depth = min_t(u32, NVME_CAP_MQES(dev->ctrl.cap) + 1,
				io_queue_depth);	/* [한국어] 스펙 상한과 모듈 파라미터 교집합 */
	dev->db_stride = 1 << NVME_CAP_STRIDE(dev->ctrl.cap);	/* [한국어] 도어벨 간격 DWORD */
	dev->dbs = dev->bar + 4096;	/* [한국어] 초기 도어벨 창 — remap 이 재설정 */

	/*
	 * Some Apple controllers require a non-standard SQE size.
	 * Interestingly they also seem to ignore the CC:IOSQES register
	 * so we don't bother updating it here.
	 */
	if (dev->ctrl.quirks & NVME_QUIRK_128_BYTES_SQES)
		dev->io_sqes = 7;	/* [한국어] 128B SQE → log2=7 */
	else
		dev->io_sqes = NVME_NVM_IOSQES;	/* [한국어] 표준 64B → 6 */

	if (dev->ctrl.quirks & NVME_QUIRK_QDEPTH_ONE) {	/* [한국어] 깊이 1 을 요구하는 장치 — 큐잉을 사실상 끈다 */
		dev->q_depth = 2;	/* [한국어] 사실상 깊이1 장치 — 최소 2 슬롯 */
	} else if (pdev->vendor == PCI_VENDOR_ID_SAMSUNG &&
		   (pdev->device == 0xa821 || pdev->device == 0xa822) &&
		   NVME_CAP_MQES(dev->ctrl.cap) == 0) {	/* [한국어] PM1725 MQES=0 버그 */
		dev->q_depth = 64;	/* [한국어] 경험적 안전 깊이 */
		dev_err(dev->ctrl.device, "detected PM1725 NVMe controller, "	/* [한국어] MQES 가 0 인 것은 스펙 위반이라 어느 장치인지 밝혀 둔다 */
                        "set queue depth=%u\n", dev->q_depth);
	}

	/*
	 * Controllers with the shared tags quirk need the IO queue to be
	 * big enough so that we get 32 tags for the admin queue
	 */
	if ((dev->ctrl.quirks & NVME_QUIRK_SHARED_TAGS) &&
	    (dev->q_depth < (NVME_AQ_DEPTH + 2))) {	/* [한국어] admin 태그와 풀 공유 */
		dev->q_depth = NVME_AQ_DEPTH + 2;	/* [한국어] admin 여유 확보 */
		dev_warn(dev->ctrl.device, "IO queue depth clamped to %d\n",	/* [한국어] admin 큐가 쓸 최소치보다 얕으면 동작할 수 없어 끌어올린다 */
			 dev->q_depth);
	}
	dev->ctrl.sqsize = dev->q_depth - 1; /* 0's based queue depth */	/* [한국어] Create 시 0-based */

	nvme_map_cmb(dev);	/* [한국어] CMB 레지스터 읽고 P2PDMA 등록 시도 */

	pci_save_state(pdev);	/* [한국어] FLR/리셋 후 restore 용 cfg 스냅샷 */

	result = nvme_pci_configure_admin_queue(dev);	/* [한국어] AQA/ASQ/ACQ + CC.EN + admin IRQ */
	if (result)	/* [한국어] admin 큐를 세우지 못하면 아무 명령도 보낼 수 없다 */
		goto free_irq;	/* [한국어] 벡터를 먼저 반납한다 */
	return result;	/* [한국어] 0 — admin 경로 기동 완료 */

 free_irq:
	pci_free_irq_vectors(pdev);	/* [한국어] 임시/admin 벡터 해제 */
 disable:
	pci_disable_device(pdev);	/* [한국어] MEM/master 비활성 */
	return result;	/* [한국어] 장치를 끈 상태로 나간다 — 호출자가 리셋 실패로 처리한다 */
}

/*
 * [한국어]
 * nvme_dev_unmap - BAR0 iounmap + pci_request_mem_regions 대칭 해제
 *
 * remove/probe 실패. bar=NULL 이면 unmap 생략 (remap 실패 상태).
 */
static void nvme_dev_unmap(struct nvme_dev *dev)
{
	if (dev->bar)	/* [한국어] 유효 MMIO 매핑이 있을 때만 */
		iounmap(dev->bar);	/* [한국어] BAR0 VA 반환 — dbs 도 무효화됨 */
	pci_release_mem_regions(to_pci_dev(dev->dev));	/* [한국어] MEM BAR 리소스 소유권 반환 */
}

/*
 * [한국어]
 * nvme_pci_ctrl_is_dead - PCI 비활성/부재/AER/CFS/!RDY 로 통신 불능 판정
 *
 * true 면 Delete Queue admin 을 시도하지 않고 로컬 tear-down 만 수행.
 */
static bool nvme_pci_ctrl_is_dead(struct nvme_dev *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] 버스에서 사라졌는지 확인하려면 PCI 상태가 필요하다 */
	u32 csts;	/* [한국어] Controller Status 스냅샷 */

	if (!pci_is_enabled(pdev) || !pci_device_is_present(pdev))	/* [한국어] 전원 off 또는 핫제거 */
		return true;	/* [한국어] MMIO/admin 불가 */
	if (pdev->error_state != pci_channel_io_normal)	/* [한국어] AER 채널 이상 */
		return true;	/* [한국어] 성공/판정 결과 반환 */

	csts = readl(dev->bar + NVME_REG_CSTS);	/* [한국어] 프로토콜 레벨 생존 확인 */
	return (csts & NVME_CSTS_CFS) || !(csts & NVME_CSTS_RDY);	/* [한국어] fatal 또는 not ready */
}

/*
 * [한국어]
 * nvme_dev_disable - 컨트롤러를 안전하게 정지 (shutdown 또는 reset 준비)
 *
 * freeze/wait → quiesce I/O → (살아 있으면) delete I/O Q + disable_ctrl +
 * admin poll → suspend queues → free vectors → pci_disable → reap CQ →
 * cancel tagsets → shutdown 이면 unquiesce 로 실패 완료 플러시.
 * shutdown_lock 전체 구간. 핫언플러그/timeout/suspend/remove 공용 중추.
 */
static void nvme_dev_disable(struct nvme_dev *dev, bool shutdown)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(&dev->ctrl);	/* [한국어] LIVE/RESETTING 등 */
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] 마지막에 장치를 끄기 위해 */
	bool dead;	/* [한국어] 하드웨어 통신 가능 여부 */

	mutex_lock(&dev->shutdown_lock);	/* [한국어] setup_io/BAR remap 과 상호배제 */
	dead = nvme_pci_ctrl_is_dead(dev);	/* [한국어] admin delete 시도 가치 판정 */
	if (state == NVME_CTRL_LIVE || state == NVME_CTRL_RESETTING) {	/* [한국어] 살아 있던 상태에서만 큐를 얼려 진행 중 I/O 를 멈춘다 */
		if (pci_is_enabled(pdev))
			nvme_start_freeze(&dev->ctrl);	/* [한국어] 새 I/O 진입 동결 */
		/*
		 * Give the controller a chance to complete all entered requests
		 * if doing a safe shutdown.
		 */
		if (!dead && shutdown)	/* [한국어] 정상 종료 — 진행 중 요청 완료 여유 */
			nvme_wait_freeze_timeout(&dev->ctrl, NVME_IO_TIMEOUT);	/* [한국어] 제한 시간 대기 */
	}

	nvme_quiesce_io_queues(&dev->ctrl);	/* [한국어] I/O 큐 정지 — queue_rq 차단 */

	if (!dead && dev->ctrl.queue_count > 0) {	/* [한국어] 살아 있으면 스펙 순서 tear-down */
		nvme_delete_io_queues(dev);	/* [한국어] Delete SQ 후 Delete CQ */
		nvme_disable_ctrl(&dev->ctrl, shutdown);	/* [한국어] CC.EN=0 또는 SHN */
		nvme_poll_irqdisable(&dev->queues[0]);	/* [한국어] admin CQ 잔여 완료 수확 */
	}
	nvme_suspend_io_queues(dev);	/* [한국어] I/O ENABLED 클리어·IRQ free */
	nvme_suspend_queue(dev, 0);	/* [한국어] admin 큐 suspend */
	pci_free_irq_vectors(pdev);	/* [한국어] MSI-X 풀 전체 해제 */
	if (pci_is_enabled(pdev))
		pci_disable_device(pdev);	/* [한국어] PCI 기능 disable */
	nvme_reap_pending_cqes(dev);	/* [한국어] cancel 전 마지막 자연 완료 */

	nvme_cancel_tagset(&dev->ctrl);	/* [한국어] I/O 미완료 → 취소 완료 */
	nvme_cancel_admin_tagset(&dev->ctrl);	/* [한국어] admin 미완료 취소 */

	/*
	 * The driver will not be starting up queues again if shutting down so
	 * must flush all entered requests to their failed completion to avoid
	 * deadlocking blk-mq hot-cpu notifier.
	 */
	/* [한국어] shutdown 시 quiesce 상태면 실패 완료가 흐르지 않아 교착 — unquiesce */
	if (shutdown) {	/* [한국어] 정식 종료면 쓰기 캐시를 내리고, 아니면 곧바로 CC.EN 을 내린다 */
		nvme_unquiesce_io_queues(&dev->ctrl);	/* [한국어] 실패 완료 플러시 */
		if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q))
			nvme_unquiesce_admin_queue(&dev->ctrl);	/* [한국어] admin 도 동일 */
	}
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] 정지 절차 종료 */
}

/*
 * [한국어]
 * nvme_disable_prepare_reset - RESETTING 확보 후 nvme_dev_disable
 *
 * wait_reset 실패(-EBUSY)면 다른 리셋/제거가 진행 중. shutdown=true 면
 * CC.SHN 정상 종료 경로 (suspend/remove/shutdown).
 */
static int nvme_disable_prepare_reset(struct nvme_dev *dev, bool shutdown)
{
	if (!nvme_wait_reset(&dev->ctrl))	/* [한국어] RESETTING 상태 선점 대기 */
		return -EBUSY;	/* [한국어] 동시 리셋/삭제와 충돌 */
	nvme_dev_disable(dev, shutdown);	/* [한국어] 큐·PCI·cancel 중추 */
	return 0;	/* [한국어] 정지 완료 — 호출자가 재기동 또는 종료 */
}

/*
 * [한국어]
 * nvme_pci_alloc_iod_mempool - non-IOVA PRP unmap 용 dma_vec 배열 mempool
 *
 * 핫패스 GFP_ATOMIC 친화. 노드 지역 할당. NVME_MAX_SEGS 쌍 크기.
 */
static int nvme_pci_alloc_iod_mempool(struct nvme_dev *dev)
{
	size_t alloc_size = sizeof(struct nvme_dma_vec) * NVME_MAX_SEGS;	/* [한국어] 최악 세그먼트 벡터 바이트 */

	dev->dmavec_mempool = mempool_create_node(1,
			mempool_kmalloc, mempool_kfree,
			(void *)alloc_size, GFP_KERNEL,
			dev_to_node(dev->dev));	/* [한국어] 최소 1 예비 객체 — 제출 경로 고갈 완화 */
	if (!dev->dmavec_mempool)
		return -ENOMEM;	/* [한국어] probe 실패 */
	return 0;	/* [한국어] PRP phys 경로 준비 완료 */
}

/*
 * [한국어]
 * nvme_free_tagset - I/O blk-mq tagset 제거 및 ctrl.tagset NULL
 *
 * 리셋 중 I/O 큐 전멸 또는 remove 경로. tags 유효할 때만 remove.
 */
static void nvme_free_tagset(struct nvme_dev *dev)
{
	if (dev->tagset.tags)	/* [한국어] 아직 할당된 tagset */
		nvme_remove_io_tag_set(&dev->ctrl);	/* [한국어] blk-mq 인프라 해제 */
	dev->ctrl.tagset = NULL;	/* [한국어] 재생성 판정 필드 클리어 */
}

/* pairs with nvme_pci_alloc_dev */
/*
 * [한국어]
 * nvme_pci_free_ctrl - ctrl ref=0 시 nvme_dev 최종 소멸 (ctrl_ops.free_ctrl)
 *
 * tagset·pdev get_device·queues[]·본체 kfree. BAR/HMB 는 이미 remove 가 처리.
 */
static void nvme_pci_free_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);	/* [한국어] 임베드 부모 복원 */

	nvme_free_tagset(dev);	/* [한국어] 잔존 I/O tagset 방어적 해제 */
	put_device(dev->dev);	/* [한국어] alloc_dev 의 get_device 균형 */
	kfree(dev->queues);	/* [한국어] 큐 배열 */
	kfree(dev);	/* [한국어] nvme_dev + descriptor_pools 유연 배열 */
}

/*
 * [한국어]
 * nvme_reset_work - RESETTING 상태에서 전체 재초기화 워크
 *
 * live 면 disable → pci_enable → admin unquiesce → CONNECTING →
 * init_ctrl_finish → dbbuf/HMB → setup_io_queues → nr_queues 갱신 → LIVE →
 * start_ctrl. 실패 시 DELETING→disable→DEAD.
 * 호출: timeout, AER slot_reset, resume, FLR done 등 try_sched_reset.
 */
static void nvme_reset_work(struct work_struct *work)
{
	struct nvme_dev *dev =
		container_of(work, struct nvme_dev, ctrl.reset_work);	/* [한국어] 임베드 work → 컨트롤러 */
	bool was_suspend = !!(dev->ctrl.ctrl_config & NVME_CC_SHN_NORMAL);	/* [한국어] 직전 정상 shutdown 여부 */
	int result;	/* [한국어] 단계 실패 코드 */

	if (nvme_ctrl_state(&dev->ctrl) != NVME_CTRL_RESETTING) {	/* [한국어] 스케줄 시점과 실행 시점 상태 불일치 */
		dev_warn(dev->ctrl.device, "ctrl state %d is not RESETTING\n",	/* [한국어] 다른 경로가 상태를 바꿔 갔다 — 이 리셋은 이미 무효다 */
			 dev->ctrl.state);
		result = -ENODEV;
		goto out;	/* [한국어] 실패 종단 경로 */
	}

	/*
	 * If we're called to reset a live controller first shut it down before
	 * moving on.
	 */
	if (dev->ctrl.ctrl_config & NVME_CC_ENABLE)	/* [한국어] 아직 enable 비트가 남아 있으면 */
		nvme_dev_disable(dev, false);	/* [한국어] 재기동 전 정지 (shutdown 아님) */
	nvme_sync_queues(&dev->ctrl);	/* [한국어] 잔여 요청 동기 */

	mutex_lock(&dev->shutdown_lock);	/* [한국어] enable 과 동시 disable 방지 */
	result = nvme_pci_enable(dev);	/* [한국어] PCI+CAP+admin 재기동 */
	if (result)	/* [한국어] 장치를 켜지 못했다 */
		goto out_unlock;
	nvme_unquiesce_admin_queue(&dev->ctrl);	/* [한국어] Identify 등 admin 제출 허용 */
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] 아래는 오래 걸리는 admin 명령이라 락을 들고 있으면 해체가 막힌다 */

	/*
	 * Introduce CONNECTING state from nvme-fc/rdma transports to mark the
	 * initializing procedure here.
	 */
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] 초기화 구간 표시 — timeout 정책 */
		dev_warn(dev->ctrl.device,	/* [한국어] 전이가 거부됐다 — 삭제가 시작된 것이다 */
			"failed to mark controller CONNECTING\n");
		result = -EBUSY;	/* [한국어] 상태 전이 경쟁 */
		goto out;	/* [한국어] 실패 종단 */
	}

	result = nvme_init_ctrl_finish(&dev->ctrl, was_suspend);	/* [한국어] Identify/재설정 (suspend 복귀 힌트) */
	if (result)
		goto out;	/* [한국어] Identify 등 admin 실패 */

	if (nvme_ctrl_meta_sgl_supported(&dev->ctrl))	/* [한국어] 메타 SGL 능력 */
		dev->ctrl.max_integrity_segments = NVME_MAX_META_SEGS;	/* [한국어] small-pool 메타 상한 */
	else
		dev->ctrl.max_integrity_segments = 1;	/* [한국어] MPTR 단일 세그먼트 */

	nvme_dbbuf_dma_alloc(dev);	/* [한국어] shadow doorbell 재할당/재클리어 */

	result = nvme_setup_host_mem(dev);	/* [한국어] HMB 재설정 */
	if (result < 0)
		goto out;	/* [한국어] 치명 HMB 실패(드묾) */

	nvme_update_attrs(dev);	/* [한국어] CMB/HMB sysfs 가시성 갱신 */

	result = nvme_setup_io_queues(dev);	/* [한국어] I/O 큐 토폴로지 재구성 */
	if (result)	/* [한국어] I/O 큐를 못 만들었다 — admin 만으로는 쓸모가 없다 */
		goto out;

	/*
	 * Freeze and update the number of I/O queues as those might have
	 * changed.  If there are no I/O queues left after this reset, keep the
	 * controller around but remove all namespaces.
	 */
	if (dev->online_queues > 1) {	/* [한국어] I/O 큐 생존 */
		nvme_dbbuf_set(dev);	/* [한국어] dbbuf 재등록 */
		nvme_unquiesce_io_queues(&dev->ctrl);	/* [한국어] I/O 제출 재개 준비 */
		nvme_wait_freeze(&dev->ctrl);	/* [한국어] nr_hw_queues 변경 전 freeze */
		if (!nvme_pci_update_nr_queues(dev))	/* [한국어] tagset 큐 수 갱신 */
			goto out;	/* [한국어] 큐 수 갱신에 실패했다 — 태그셋과 실제 큐가 어긋난 채로 둘 수 없다 */
		nvme_unfreeze(&dev->ctrl);	/* [한국어] 정상 서비스 재개 */
	} else {
		dev_warn(dev->ctrl.device, "IO queues lost\n");	/* [한국어] admin-only 잔존 */
		nvme_mark_namespaces_dead(&dev->ctrl);	/* [한국어] 기존 ns I/O 실패 처리 */
		nvme_unquiesce_io_queues(&dev->ctrl);	/* [한국어] 죽은 것으로 표시한 뒤 큐를 열어야 대기 중 I/O 가 실패로 빠져나간다 */
		nvme_remove_namespaces(&dev->ctrl);	/* [한국어] 디스크 제거 */
		nvme_free_tagset(dev);	/* [한국어] I/O tagset 폐기 */
	}

	/*
	 * If only admin queue live, keep it to do further investigation or
	 * recovery.
	 */
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_LIVE)) {	/* [한국어] LIVE 로 못 올렸다 — 그 사이 삭제가 시작됐다 */
		dev_warn(dev->ctrl.device,
			"failed to mark controller live state\n");
		result = -ENODEV;
		goto out;
	}

	nvme_start_ctrl(&dev->ctrl);	/* [한국어] AEN·scan 등 정상 가동 */
	return;	/* [한국어] 리셋 성공 */

 out_unlock:
	mutex_unlock(&dev->shutdown_lock);	/* [한국어] 락을 든 채 실패한 경로만 여기로 온다 */
 out:
	/*
	 * Set state to deleting now to avoid blocking nvme_wait_reset(), which
	 * may be holding this pci_dev's device lock.
	 */
	/* [한국어] 실패 종단: wait_reset 교착 피하려 먼저 DELETING */
	dev_warn(dev->ctrl.device, "Disabling device after reset failure: %d\n",	/* [한국어] 왜 장치가 사라지는지 남긴다 — 사용자에게는 갑작스러운 일이다 */
		 result);
	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);	/* [한국어] 더 복구하지 않겠다는 선언 — 이후 리셋 요청이 거부된다 */
	nvme_dev_disable(dev, true);	/* [한국어] 최종 정지 */
	nvme_sync_queues(&dev->ctrl);	/* [한국어] 진행 중 완료 처리가 끝나기를 기다려야 자원을 반납할 수 있다 */
	nvme_mark_namespaces_dead(&dev->ctrl);	/* [한국어] 사용자 I/O 에 실패 전파 */
	nvme_unquiesce_io_queues(&dev->ctrl);	/* [한국어] 실패 완료 플러시 */
	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DEAD);	/* [한국어] 영구 사망 — 재사용 금지 */
}

/*
 * [한국어] core 가 CAP/CC/CSTS 등 레지스터를 읽을 때 쓰는 BAR MMIO 훅 묶음.
 */
static int nvme_pci_reg_read32(struct nvme_ctrl *ctrl, u32 off, u32 *val)
{
	*val = readl(to_nvme_dev(ctrl)->bar + off);	/* [한국어] 32bit MMIO 로드 — CSTS/CC 등 */
	return 0;	/* [한국어] PCIe 맵 성공 전제 — 항상 0 */
}

static int nvme_pci_reg_write32(struct nvme_ctrl *ctrl, u32 off, u32 val)	/* [한국어] 같은 묶음의 쓰기 훅 — CC 로 컨트롤러를 켜고 끄는 통로다 */
{
	writel(val, to_nvme_dev(ctrl)->bar + off);	/* [한국어] 32bit MMIO 스토어 (CC 등) */
	return 0;	/* [한국어] posted write 완료 표시 */
}

static int nvme_pci_reg_read64(struct nvme_ctrl *ctrl, u32 off, u64 *val)	/* [한국어] CAP 처럼 64비트인 레지스터. lo→hi 순서가 스펙 요구다 */
{
	*val = lo_hi_readq(to_nvme_dev(ctrl)->bar + off);	/* [한국어] CAP 등 64bit lo→hi */
	return 0;	/* [한국어] 64bit 능력 레지스터 스냅샷 성공 */
}

static int nvme_pci_get_address(struct nvme_ctrl *ctrl, char *buf, int size)	/* [한국어] sysfs 의 address 속성 — 사용자가 어느 슬롯의 장치인지 알 수 있게 한다 */
{
	struct pci_dev *pdev = to_pci_dev(to_nvme_dev(ctrl)->dev);	/* [한국어] BDF 문자열 소스 */

	return snprintf(buf, size, "%s\n", dev_name(&pdev->dev));	/* [한국어] PCI BDF 장치명 */
}

static void nvme_pci_print_device_info(struct nvme_ctrl *ctrl)	/* [한국어] 오류 로그에 장치를 특정할 정보를 한 줄로 남기는 훅 */
{
	struct pci_dev *pdev = to_pci_dev(to_nvme_dev(ctrl)->dev);	/* [한국어] VID/DID */
	struct nvme_subsystem *subsys = ctrl->subsys;	/* [한국어] Identify model/fw */

	dev_err(ctrl->device,
		"VID:DID %04x:%04x model:%.*s firmware:%.*s\n",
		pdev->vendor, pdev->device,
		nvme_strlen(subsys->model, sizeof(subsys->model)),
		subsys->model, nvme_strlen(subsys->firmware_rev,
					   sizeof(subsys->firmware_rev)),
		subsys->firmware_rev);	/* [한국어] 장애 진단용 식별 정보 */
}

static bool nvme_pci_supports_pci_p2pdma(struct nvme_ctrl *ctrl)	/* [한국어] CMB 를 다른 장치가 직접 읽을 수 있는지 — 코어가 P2P 정책을 정할 때 묻는다 */
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);	/* [한국어] DMA device 소유자 */

	return dma_pci_p2pdma_supported(dev->dev);	/* [한국어] 이 함수 DMA 가 P2PDMA 가능? */
}

/*
 * [한국어]
 * nvme_pci_get_virt_boundary - SGL 미지원/admin 이면 4K 경계, SGL I/O 면 0
 *
 * blk-mq 가상 경계 병합 정책에 영향 — PRP 는 페이지 경계를 넘기 어렵다.
 */
static unsigned long nvme_pci_get_virt_boundary(struct nvme_ctrl *ctrl,
						bool is_admin)	/* [한국어] 지역 변수 */
{
	if (!nvme_ctrl_sgl_supported(ctrl) || is_admin)
		return NVME_CTRL_PAGE_SIZE - 1;	/* [한국어] PRP 정렬 경계 마스크 */
	return 0;	/* [한국어] SGL I/O 는 가상 경계 제한 없음 */
}

/*
 * [한국어] core 가 호출하는 PCIe 트랜스포트 ops 테이블.
 * reg_* 는 BAR MMIO, free_ctrl 은 ref=0 해제, AEN/NSSR/P2PDMA/boundary 훅.
 */
static const struct nvme_ctrl_ops nvme_pci_ctrl_ops = {
	.name			= "pcie",	/* [한국어] 트랜스포트 이름 (sysfs 등) */
	.module			= THIS_MODULE,	/* [한국어] 모듈 참조 핀 */
	.flags			= NVME_F_METADATA_SUPPORTED,	/* [한국어] PI/메타 지원 광고 */
	.dev_attr_groups	= nvme_pci_dev_attr_groups,	/* [한국어] CMB/HMB sysfs */
	.reg_read32		= nvme_pci_reg_read32,	/* [한국어] BAR+off 32bit 읽기 */
	.reg_write32		= nvme_pci_reg_write32,	/* [한국어] BAR+off 32bit 쓰기 */
	.reg_read64		= nvme_pci_reg_read64,	/* [한국어] CAP 등 64bit */
	.free_ctrl		= nvme_pci_free_ctrl,	/* [한국어] nvme_dev 최종 free */
	.submit_async_event	= nvme_pci_submit_async_event,	/* [한국어] AEN SQ 수동 제출 */
	.subsystem_reset	= nvme_pci_subsystem_reset,	/* [한국어] NSSR 레지스터 */
	.get_address		= nvme_pci_get_address,	/* [한국어] PCI BDF 문자열 */
	.print_device_info	= nvme_pci_print_device_info,	/* [한국어] 에러 시 VID/model/fw */
	.supports_pci_p2pdma	= nvme_pci_supports_pci_p2pdma,	/* [한국어] P2PDMA 가능 여부 */
	.get_virt_boundary	= nvme_pci_get_virt_boundary,	/* [한국어] SGL 유무에 따른 병합 경계 */
};

/*
 * [한국어]
 * nvme_dev_map - MEM BAR claim + 최소 창 ioremap (레지스터+초기 도어벨)
 */
static int nvme_dev_map(struct nvme_dev *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);	/* [한국어] BAR 영역 예약과 매핑에 필요하다 */

	if (pci_request_mem_regions(pdev, "nvme"))	/* [한국어] BAR 리소스 소유권 확보 */
		return -ENODEV;	/* [한국어] 다른 드라이버가 이 BAR 을 쥐고 있다 */

	if (nvme_remap_bar(dev, NVME_REG_DBS + 4096))	/* [한국어] 도어벨 영역까지 덮는 최소 크기 — 실제 크기는 큐 수가 정해진 뒤 다시 잡는다 */
		goto release;	/* [한국어] 예약한 BAR 영역을 돌려주고 나간다 */

	return 0;	/* [한국어] 성공 반환 */
  release:
	pci_release_mem_regions(pdev);	/* [한국어] ioremap 실패 시 region 반환 */
	return -ENODEV;	/* [한국어] 예약한 영역을 돌려주고 나간다 */
}

/*
 * [한국어]
 * check_vendor_combination_bug - DMI 보드×PCI VID:DID 조합 suspend/APST quirk
 *
 * 정적 ID 테이블만으로는 잡지 못하는 플랫폼 한정 버스 드롭/절전 버그.
 * 반환 비트는 alloc_dev 에서 ctrl.quirks 에 OR 된다.
 */
static unsigned long check_vendor_combination_bug(struct pci_dev *pdev)
{
	if (pdev->vendor == 0x144d && pdev->device == 0xa802) {	/* [한국어] Samsung SM951/PM951/950 PRO 계열 */
		/*
		 * Several Samsung devices seem to drop off the PCIe bus
		 * randomly when APST is on and uses the deepest sleep state.
		 * This has been observed on a Samsung "SM951 NVMe SAMSUNG
		 * 256GB", a "PM951 NVMe SAMSUNG 512GB", and a "Samsung SSD
		 * 950 PRO 256GB", but it seems to be restricted to two Dell
		 * laptops.
		 */
		/* [한국어] Dell XPS/Precision 에서 최심 PS 진입 시 링크 드롭 */
		if (dmi_match(DMI_SYS_VENDOR, "Dell Inc.") &&	/* [한국어] Dell 시스템 */
		    (dmi_match(DMI_PRODUCT_NAME, "XPS 15 9550") ||
		     dmi_match(DMI_PRODUCT_NAME, "Precision 5510")))	/* [한국어] 해당 노트북 모델 */
			return NVME_QUIRK_NO_DEEPEST_PS;	/* [한국어] APST 최심 상태 금지 */
	} else if (pdev->vendor == 0x144d && pdev->device == 0xa804) {	/* [한국어] Samsung 960 EVO */
		/*
		 * Samsung SSD 960 EVO drops off the PCIe bus after system
		 * suspend on a Ryzen board, ASUS PRIME B350M-A, as well as
		 * within few minutes after bootup on a Coffee Lake board -
		 * ASUS PRIME Z370-A
		 */
		/* [한국어] 특정 ASUS 보드에서 suspend/부팅 직후 버스 드롭 — APST 전면 금지 */
		if (dmi_match(DMI_BOARD_VENDOR, "ASUSTeK COMPUTER INC.") &&
		    (dmi_match(DMI_BOARD_NAME, "PRIME B350M-A") ||
		     dmi_match(DMI_BOARD_NAME, "PRIME Z370-A")))
			return NVME_QUIRK_NO_APST;	/* [한국어] Autonomous Power State Transition 비활성 */
	} else if ((pdev->vendor == 0x144d && (pdev->device == 0xa801 ||
		    pdev->device == 0xa808 || pdev->device == 0xa809)) ||
		   (pdev->vendor == 0x1e0f && pdev->device == 0x0001)) {	/* [한국어] Samsung/Toshiba 일부 */
		/*
		 * Forcing to use host managed nvme power settings for
		 * lowest idle power with quick resume latency on
		 * Samsung and Toshiba SSDs based on suspend behavior
		 * on Coffee Lake board for LENOVO C640
		 */
		/* [한국어] Lenovo C640 계 — 커널 simple suspend(풀 shutdown) 강제 */
		if ((dmi_match(DMI_BOARD_VENDOR, "LENOVO")) &&
		     dmi_match(DMI_BOARD_NAME, "LNVNB161216"))
			return NVME_QUIRK_SIMPLE_SUSPEND;	/* [한국어] 프로토콜 PS 대신 full disable */
	} else if (pdev->vendor == 0x2646 && (pdev->device == 0x2263 ||
		   pdev->device == 0x500f)) {	/* [한국어] Kingston NV1/A2000 */
		/*
		 * Exclude some Kingston NV1 and A2000 devices from
		 * NVME_QUIRK_SIMPLE_SUSPEND. Do a full suspend to save a
		 * lot of energy with s2idle sleep on some TUXEDO platforms.
		 */
		/* [한국어] TUXEDO s2idle 절전 — simple suspend 강제 제외(풀 경로 유도) */
		if (dmi_match(DMI_BOARD_NAME, "NS5X_NS7XAU") ||
		    dmi_match(DMI_BOARD_NAME, "NS5x_7xAU") ||
		    dmi_match(DMI_BOARD_NAME, "NS5x_7xPU") ||
		    dmi_match(DMI_BOARD_NAME, "PH4PRX1_PH6PRX1"))
			return NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND;	/* [한국어] ACPI D3 simple 우회 금지 */
	} else if (pdev->vendor == 0x144d && pdev->device == 0xa80d) {	/* [한국어] Samsung 990 EVO(Plus 동일 ID) */
		/*
		 * Exclude Samsung 990 Evo from NVME_QUIRK_SIMPLE_SUSPEND
		 * because of high power consumption (> 2 Watt) in s2idle
		 * sleep. Only some boards with Intel CPU are affected.
		 * (Note for testing: Samsung 990 Evo Plus has same PCI ID)
		 */
		/* [한국어] 특정 보드 s2idle 고소비 — simple suspend 제외 */
		if (dmi_match(DMI_BOARD_NAME, "DN50Z-140HC-YD") ||
		    dmi_match(DMI_BOARD_NAME, "GMxPXxx") ||
		    dmi_match(DMI_BOARD_NAME, "GXxMRXx") ||
		    dmi_match(DMI_BOARD_NAME, "NS5X_NS7XAU") ||
		    dmi_match(DMI_BOARD_NAME, "PH4PG31") ||
		    dmi_match(DMI_BOARD_NAME, "PH4PRX1_PH6PRX1") ||
		    dmi_match(DMI_BOARD_NAME, "PH6PG01_PH6PG71"))
			return NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND;	/* [한국어] 프로토콜/PCI PM 경로 유지 */
	}

	/*
	 * NVMe SSD drops off the PCIe bus after system idle
	 * for 10 hours on a Lenovo N60z board.
	 */
	/* [한국어] Lenovo N60z 장기 유휴 후 버스 드롭 — APST 금지 */
	if (dmi_match(DMI_BOARD_NAME, "LXKT-ZXEG-N6"))
		return NVME_QUIRK_NO_APST;	/* [한국어] 자율 전원 전이 비활성 */

	return 0;	/* [한국어] 해당 조합 없음 — 추가 quirk 없음 */
}

/*
 * [한국어]
 * detect_dynamic_quirks - quirks= 모듈 파라미터 테이블에서 VID:DID 검색
 *
 * probe 시 정적 ID+DMI 위에 enabled OR / disabled AND-NOT 오버레이.
 */
static struct quirk_entry *detect_dynamic_quirks(struct pci_dev *pdev)
{
	int i;	/* [한국어] 동적 테이블 선형 스캔 인덱스 */

	for (i = 0; i < nvme_pci_quirk_count; i++)	/* [한국어] quirks_param_set 이 채운 배열 */
		if (pdev->vendor == nvme_pci_quirk_list[i].vendor_id &&
		    pdev->device == nvme_pci_quirk_list[i].dev_id)	/* [한국어] 16bit VID:DID 정확 일치 */
			return &nvme_pci_quirk_list[i];	/* [한국어] on/off 마스크 엔트리 */

	return NULL;	/* [한국어] 동적 오버레이 없음 */
}

/*
 * [한국어]
 * nvme_pci_alloc_dev - nvme_dev 할당, queues, quirks 병합, nvme_init_ctrl
 *
 * descriptor_pools 를 nr_node_ids 유연 배열로 함께 할당. DMA mask 48/64,
 * min_align 4K-1, max_hw_sectors/segments 상한. probe 최전방.
 */
static struct nvme_dev *nvme_pci_alloc_dev(struct pci_dev *pdev,
		const struct pci_device_id *id)
{
	unsigned long quirks = id->driver_data;	/* [한국어] PCI ID 테이블 정적 quirk 시드 */
	int node = dev_to_node(&pdev->dev);	/* [한국어] 장치 NUMA 노드 — 지역 할당 */
	struct nvme_dev *dev;	/* [한국어] 큐 배열까지 포함한 장치 상태 — 아래에서 노드 지역 메모리로 잡는다 */
	struct quirk_entry *qentry;	/* [한국어] 모듈 파라미터 동적 quirk */
	int ret = -ENOMEM;	/* [한국어] 지역 변수 */

	dev = kzalloc_node(struct_size(dev, descriptor_pools, nr_node_ids),
			GFP_KERNEL, node);	/* [한국어] 본체+노드별 pool 배열 일괄 할당 */
	if (!dev)	/* [한국어] 가변 길이 꼬리(노드별 서술자 풀)까지 한 번에 잡는다 */
		return ERR_PTR(-ENOMEM);
	INIT_WORK(&dev->ctrl.reset_work, nvme_reset_work);	/* [한국어] 리셋 워크 핸들러 연결 */
	mutex_init(&dev->shutdown_lock);	/* [한국어] disable↔setup 직렬화 락 */

	dev->nr_write_queues = write_queues;	/* [한국어] 초기 스냅샷 — setup_io 가 재샘플 */
	dev->nr_poll_queues = poll_queues;	/* [한국어] 모듈 파라미터를 장치에 복사한다 — 이후 런타임 변경과 무관해진다 */
	dev->nr_allocated_queues = nvme_max_io_queues(dev) + 1;	/* [한국어] admin 포함 슬롯 */
	dev->queues = kcalloc_node(dev->nr_allocated_queues,
			sizeof(struct nvme_queue), GFP_KERNEL, node);	/* [한국어] 큐 배열 */
	if (!dev->queues)	/* [한국어] 큐 구조체 배열 — 노드 지역이라야 인터럽트 처리에서 원격 접근이 없다 */
		goto out_free_dev;

	dev->dev = get_device(&pdev->dev);	/* [한국어] pdev 수명 핀 — free_ctrl 에서 put */

	quirks |= check_vendor_combination_bug(pdev);	/* [한국어] DMI×SSD 조합 quirk OR */
	if (!noacpi &&	/* [한국어] ACPI 가 이 장치의 절전 방식을 알려 준다 — noacpi 로 끌 수 있다 */
	    !(quirks & NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND) &&
	    acpi_storage_d3(&pdev->dev)) {
		/*
		 * Some systems use a bios work around to ask for D3 on
		 * platforms that support kernel managed suspend.
		 */
		/* [한국어] ACPI StorageD3 → 커널 simple suspend(풀 shutdown) 강제 */
		dev_info(&pdev->dev,	/* [한국어] 왜 단순 절전으로 바뀌는지 남긴다 — 성능이 아니라 안정성 선택이다 */
			 "platform quirk: setting simple suspend\n");
		quirks |= NVME_QUIRK_SIMPLE_SUSPEND;	/* [한국어] APST 대신 D3 로 내린다 */
	}
	qentry = detect_dynamic_quirks(pdev);	/* [한국어] quirks= 파라미터 매칭 */
	if (qentry) {	/* [한국어] 사용자가 부팅 인자로 붙인 quirk 가 있다 */
		quirks |= qentry->enabled_quirks;	/* [한국어] 강제 on */
		quirks &= ~qentry->disabled_quirks;	/* [한국어] 강제 off */
	}
	ret = nvme_init_ctrl(&dev->ctrl, &pdev->dev, &nvme_pci_ctrl_ops,
			     quirks);	/* [한국어] core 컨트롤러 객체 초기화 + ops 연결 */
	if (ret)	/* [한국어] 코어 등록 실패 — 이후 자원을 붙일 수 없다 */
		goto out_put_device;

	if (dev->ctrl.quirks & NVME_QUIRK_DMA_ADDRESS_BITS_48)
		dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(48));	/* [한국어] 일부 플랫폼 48bit */
	else
		dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));	/* [한국어] 표준 64bit DMA */
	dma_set_min_align_mask(&pdev->dev, NVME_CTRL_PAGE_SIZE - 1);	/* [한국어] PRP 페이지 정렬 힌트 */
	dma_set_max_seg_size(&pdev->dev, 0xffffffff);	/* [한국어] 세그먼트 상한 사실상 무제한 */

	/*
	 * Limit the max command size to prevent iod->sg allocations going
	 * over a single page.
	 */
	/* [한국어] 단일 요청 크기/세그먼트 상한 — iod 메모리 폭주 방지 */
	dev->ctrl.max_hw_sectors = min_t(u32,
			NVME_MAX_BYTES >> SECTOR_SHIFT,
			dma_opt_mapping_size(&pdev->dev) >> 9);	/* [한국어] 8MiB 와 DMA opt 중 작은 쪽 */
	dev->ctrl.max_segments = NVME_MAX_SEGS;	/* [한국어] SGL 페이지 한도 */
	dev->ctrl.max_integrity_segments = 1;	/* [한국어] Identify 전 기본 1 — 이후 갱신 */
	return dev;	/* [한국어] 자원과 한계값이 모두 세팅된 장치를 돌려준다 */

out_put_device:
	put_device(dev->dev);	/* [한국어] get_device 롤백 */
	kfree(dev->queues);	/* [한국어] 붙인 역순으로 되돌린다 */
out_free_dev:
	kfree(dev);
	return ERR_PTR(ret);	/* [한국어] probe 가 PTR_ERR 로 해석 */
}

/*
 * [한국어]
 * nvme_probe - PCI probe 전체: map→enable→admin tagset→init→HMB→I/O→LIVE
 *
 * CONNECTING 표시 후 admin 타임아웃 정책 활성화. online_queues>1 일 때만
 * I/O tagset+dbbuf_set. 실패 시 역순 정리. 비동기 probe.
 * 호출 체인: pci_driver.probe → [여기] → nvme_start_ctrl → scan_work
 */
static int nvme_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct nvme_dev *dev;	/* [한국어] 이 PCI 함수의 트랜스포트 인스턴스 */
	int result = -ENOMEM;	/* [한국어] 기본 에러 — 단계별 갱신 */

	dev = nvme_pci_alloc_dev(pdev, id);	/* [한국어] nvme_dev+queues+quirks+init_ctrl */
	if (IS_ERR(dev))
		return PTR_ERR(dev);	/* [한국어] 할당/init 실패 errno */

	result = nvme_add_ctrl(&dev->ctrl);	/* [한국어] 전역 컨트롤러 목록·문자 장치 등록 */
	if (result)	/* [한국어] 코어에 등록해야 /dev/nvmeX 수명이 시작된다 */
		goto out_put_ctrl;	/* [한국어] 등록 전이므로 참조만 놓으면 된다 */

	result = nvme_dev_map(dev);	/* [한국어] BAR 을 예약하고 최소 크기로 매핑한다 */
	if (result)
		goto out_uninit_ctrl;	/* [한국어] 등록 뒤이므로 uninit 이 필요하다 */

	result = nvme_pci_alloc_iod_mempool(dev);	/* [한국어] I/O 경로가 GFP_ATOMIC 으로 쓸 예비 풀 — 미리 잡아 두어야 메모리 압박에서도 진행한다 */
	if (result)
		goto out_dev_unmap;

	dev_info(dev->ctrl.device, "pci function %s\n", dev_name(&pdev->dev));	/* [한국어] BDF 가시성 */

	result = nvme_pci_enable(dev);	/* [한국어] 장치를 켜고 admin 큐를 세운다 */
	if (result)
		goto out_release_iod_mempool;

	result = nvme_alloc_admin_tag_set(&dev->ctrl, &dev->admin_tagset,
				&nvme_mq_admin_ops, sizeof(struct nvme_iod));	/* [한국어] admin blk-mq + iod PDU */
	if (result)	/* [한국어] admin 태그셋 없이는 Identify 를 보낼 수 없다 */
		goto out_disable;

	/*
	 * Mark the controller as connecting before sending admin commands to
	 * allow the timeout handler to do the right thing.
	 */
	/* [한국어] Identify 등 admin 중 타임아웃 시 CONNECTING 정책 적용 */
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_CONNECTING)) {	/* [한국어] probe 중에 이미 삭제가 시작됐다 */
		dev_warn(dev->ctrl.device,
			"failed to mark controller CONNECTING\n");
		result = -EBUSY;	/* [한국어] 상태 전이 경쟁 */
		goto out_disable;
	}

	result = nvme_init_ctrl_finish(&dev->ctrl, false);	/* [한국어] Identify 로 컨트롤러 능력을 읽어 코어를 완성한다 */
	if (result)
		goto out_disable;

	if (nvme_ctrl_meta_sgl_supported(&dev->ctrl))
		dev->ctrl.max_integrity_segments = NVME_MAX_META_SEGS;	/* [한국어] meta SGL 상한 */
	else
		dev->ctrl.max_integrity_segments = 1;	/* [한국어] MPTR 단일 세그먼트만 */

	nvme_dbbuf_dma_alloc(dev);	/* [한국어] optional shadow doorbell 버퍼 */

	result = nvme_setup_host_mem(dev);	/* [한국어] HMB 는 선택이라 음수만 실패로 본다 */
	if (result < 0)
		goto out_disable;

	nvme_update_attrs(dev);	/* [한국어] CMB/HMB sysfs 가시성 갱신 */

	result = nvme_setup_io_queues(dev);	/* [한국어] 여기까지 와야 실제 I/O 가 가능해진다 */
	if (result)
		goto out_disable;

	if (dev->online_queues > 1) {	/* [한국어] admin 외에 I/O 큐가 있음 */
		nvme_alloc_io_tag_set(&dev->ctrl, &dev->tagset, &nvme_mq_ops,
				nvme_pci_nr_maps(dev), sizeof(struct nvme_iod));	/* [한국어] I/O blk-mq */
		nvme_dbbuf_set(dev);	/* [한국어] 컨트롤러에 dbbuf 주소 등록 */
	}

	if (!dev->ctrl.tagset)
		dev_warn(dev->ctrl.device, "IO queues not created\n");	/* [한국어] admin-only 로 생존 가능 */

	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_LIVE)) {	/* [한국어] LIVE 로 올려야 네임스페이스 스캔이 시작된다 */
		dev_warn(dev->ctrl.device,
			"failed to mark controller live state\n");
		result = -ENODEV;
		goto out_disable;
	}

	pci_set_drvdata(pdev, dev);	/* [한국어] remove/PM/AER 진입점에서 nvme_dev 회수 */

	nvme_start_ctrl(&dev->ctrl);	/* [한국어] AEN·keep-alive·ns scan 시작 */
	nvme_put_ctrl(&dev->ctrl);	/* [한국어] add_ctrl 때 올린 참조 균형 — 소유는 drvdata */
	flush_work(&dev->ctrl.scan_work);	/* [한국어] 최초 네임스페이스 스캔 동기 완료 */
	return 0;	/* [한국어] probe 성공 — 디스크 노출 */

out_disable:
	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);	/* [한국어] 실패 언와인드 상태 */
	nvme_dev_disable(dev, true);	/* [한국어] 하드웨어/큐 정지 */
	nvme_free_host_mem(dev);	/* [한국어] HMB 반환 */
	nvme_dev_remove_admin(dev);	/* [한국어] admin tagset 제거 */
	nvme_dbbuf_dma_free(dev);	/* [한국어] shadow doorbell 반환 */
	nvme_free_queues(dev, 0);	/* [한국어] 전 큐 DMA free */
out_release_iod_mempool:
	mempool_destroy(dev->dmavec_mempool);	/* [한국어] iod 벡터 풀 파괴 */
out_dev_unmap:
	nvme_dev_unmap(dev);	/* [한국어] BAR unmap + region release */
out_uninit_ctrl:
	nvme_uninit_ctrl(&dev->ctrl);	/* [한국어] core 컨트롤러 등록 해제 */
out_put_ctrl:
	nvme_put_ctrl(&dev->ctrl);	/* [한국어] 최종 ref → free_ctrl */
	dev_err_probe(&pdev->dev, result, "probe failed\n");	/* [한국어] deferred probe 친화 에러 로그 */
	return result;	/* [한국어] dev_err_probe 가 이유를 남겼으므로 값만 올린다 */
}

/*
 * [한국어]
 * nvme_reset_prepare - PCI FLR/slot reset 직전 드라이버 quiesce
 *
 * device lock 보유 → remove 레이스 없음. disable_prepare_reset + sync.
 */
static void nvme_reset_prepare(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);	/* [한국어] PCI 코어가 리셋을 걸기 전에 부르는 훅 */

	/*
	 * We don't need to check the return value from waiting for the reset
	 * state as pci_dev device lock is held, making it impossible to race
	 * with ->remove().
	 */
	nvme_disable_prepare_reset(dev, false);	/* [한국어] 비-shutdown disable, RESETTING 대기 */
	nvme_sync_queues(&dev->ctrl);	/* [한국어] inflight 완료 동기 */
}

/*
 * [한국어]
 * nvme_reset_done - FLR 완료 후 reset_work 스케줄 및 flush
 */
static void nvme_reset_done(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);	/* [한국어] 버스 리셋이 끝난 뒤 — 컨트롤러 상태가 모두 사라졌다 */

	if (!nvme_try_sched_reset(&dev->ctrl))	/* [한국어] 스케줄 성공이면 */
		flush_work(&dev->ctrl.reset_work);	/* [한국어] FLR 경로에서 재기동 완료까지 대기 */
}

/*
 * [한국어]
 * nvme_shutdown - 시스템 종료 시 정상 shutdown_notification 경로 disable
 */
static void nvme_shutdown(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);	/* [한국어] 시스템 종료 경로 */

	nvme_disable_prepare_reset(dev, true);	/* [한국어] CC.SHN 정상 종료 */
}

/*
 * The driver's remove may be called on a device in a partially initialized
 * state. This function must not have any dependencies on the device state in
 * order to proceed.
 */
/*
 * [한국어]
 * nvme_remove - 언바인드. 부분 초기화 상태에서도 동작해야 함
 *
 * DELETING, 부재 시 DEAD 즉시 disable. reset_work flush 후 ns 제거·자원 해제.
 */
static void nvme_remove(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);	/* [한국어] probe 가 저장한 인스턴스 */

	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);	/* [한국어] 신규 복구/제출 억제 */
	pci_set_drvdata(pdev, NULL);	/* [한국어] 이후 PCI 콜백이 stale 포인터 쓰지 않게 */

	if (!pci_device_is_present(pdev)) {	/* [한국어] 핫제거 — 하드웨어 이미 없음 */
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DEAD);	/* [한국어] 통신 단절 명시 */
		nvme_dev_disable(dev, true);	/* [한국어] 로컬 정리 우선 */
	}

	flush_work(&dev->ctrl.reset_work);	/* [한국어] 진행 중 리셋과 remove 직렬화 */
	nvme_stop_ctrl(&dev->ctrl);	/* [한국어] keep-alive/AEN 등 백그라운드 정지 */
	nvme_remove_namespaces(&dev->ctrl);	/* [한국어] gendisk/ns 제거 */
	nvme_dev_disable(dev, true);	/* [한국어] 큐·PCI·cancel 전체 정지 */
	nvme_free_host_mem(dev);	/* [한국어] HMB 반환 */
	nvme_dev_remove_admin(dev);	/* [한국어] admin tagset */
	nvme_dbbuf_dma_free(dev);	/* [한국어] shadow doorbell */
	nvme_free_queues(dev, 0);	/* [한국어] 전 큐 링 메모리 */
	mempool_destroy(dev->dmavec_mempool);	/* [한국어] iod 벡터 풀 */
	nvme_release_descriptor_pools(dev);	/* [한국어] NUMA descriptor pools */
	nvme_dev_unmap(dev);	/* [한국어] BAR/regions */
	nvme_uninit_ctrl(&dev->ctrl);	/* [한국어] core ref 경로로 free_ctrl */
}

#ifdef CONFIG_PM_SLEEP
/*
 * [한국어]
 * nvme_get_power_state - Get Features(Power Management) 로 현재 PS 읽기
 *
 * suspend 직전 last_ps 저장용. resume 이 동일 PS 로 복귀 시도.
 */
static int nvme_get_power_state(struct nvme_ctrl *ctrl, u32 *ps)
{
	return nvme_get_features(ctrl, NVME_FEAT_POWER_MGMT, 0, NULL, 0, ps);	/* [한국어] admin Get Features PS */
}

/*
 * [한국어]
 * nvme_set_power_state - Set Features 로 NVMe 전원 상태 진입/복귀
 *
 * npss(최심 비동작) 또는 last_ps 복귀에 사용. PCI D-state 와 별개 프로토콜 PS.
 */
static int nvme_set_power_state(struct nvme_ctrl *ctrl, u32 ps)
{
	return nvme_set_features(ctrl, NVME_FEAT_POWER_MGMT, ps, NULL, 0, NULL);	/* [한국어] admin Set Features PS */
}

/*
 * [한국어]
 * nvme_resume - 가능하면 이전 PS 복귀+HMB, 실패 시 full reset
 */
static int nvme_resume(struct device *dev)
{
	struct nvme_dev *ndev = pci_get_drvdata(to_pci_dev(dev));	/* [한국어] 절전 복귀 — 저전력 상태에서 돌아온다 */
	struct nvme_ctrl *ctrl = &ndev->ctrl;

	if (ndev->last_ps == U32_MAX ||
	    nvme_set_power_state(ctrl, ndev->last_ps) != 0)	/* [한국어] PS 미저장 또는 복귀 실패 */
		goto reset;	/* [한국어] full 컨트롤러 리셋 폴백 */
	if (ctrl->hmpre && nvme_setup_host_mem(ndev))	/* [한국어] HMB 재설정 실패도 리셋 */
		goto reset;	/* [한국어] HMB 를 다시 세우지 못하면 부분 복구가 불가능하므로 전체 리셋으로 간다 */

	return 0;	/* [한국어] 가벼운 프로토콜 PS 복귀 성공 */
reset:
	return nvme_try_sched_reset(ctrl);	/* [한국어] reset_work 로 재초기화 */
}

/*
 * [한국어]
 * nvme_suspend - 호스트 관리 NVMe PS 또는 full shutdown
 *
 * firmware suspend / npss=0 / ASPM off / SIMPLE_SUSPEND quirk 면
 * disable_prepare_reset. 아니면 freeze 후 HMB off, last_ps 저장, npss 진입.
 * 실패 시 unfreeze 후 에러 또는 full reset 폴백.
 */
static int nvme_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] D3 진입 여부 판단에 PCI 상태가 필요하다 */
	struct nvme_dev *ndev = pci_get_drvdata(pdev);	/* [한국어] 이 함수의 컨트롤러 */
	struct nvme_ctrl *ctrl = &ndev->ctrl;
	int ret = -EBUSY;	/* [한국어] 기본 — unfreeze 경로에서 덮어씀 */

	ndev->last_ps = U32_MAX;	/* [한국어] resume 시 "저장 없음" 표식 */

	/*
	 * The platform does not remove power for a kernel managed suspend so
	 * use host managed nvme power settings for lowest idle power if
	 * possible. This should have quicker resume latency than a full device
	 * shutdown.  But if the firmware is involved after the suspend or the
	 * device does not support any non-default power states, shut down the
	 * device fully.
	 *
	 * If ASPM is not enabled for the device, shut down the device and allow
	 * the PCI bus layer to put it into D3 in order to take the PCIe link
	 * down, so as to allow the platform to achieve its minimum low-power
	 * state (which may not be possible if the link is up).
	 */
	/* [한국어] 프로토콜 PS 가 부적합하면 PCI D3/full shutdown 경로 */
	if (pm_suspend_via_firmware() || !ctrl->npss ||
	    !pcie_aspm_enabled(pdev) ||
	    (ndev->ctrl.quirks & NVME_QUIRK_SIMPLE_SUSPEND))
		return nvme_disable_prepare_reset(ndev, true);	/* [한국어] CC shutdown + 리셋 대기 */

	nvme_start_freeze(ctrl);	/* [한국어] I/O 동결 시작 */
	nvme_wait_freeze(ctrl);	/* [한국어] 모든 hctx freeze 완료 */
	nvme_sync_queues(ctrl);	/* [한국어] inflight 완료 동기 */

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)	/* [한국어] 이미 비정상 — PS 진입 금지 */
		goto unfreeze;	/* [한국어] LIVE 가 아니면 절전 상태를 협상할 admin 큐가 없다 */

	/*
	 * Host memory access may not be successful in a system suspend state,
	 * but the specification allows the controller to access memory in a
	 * non-operational power state.
	 */
	/* [한국어] suspend 중 호스트 메모리 접근 불안정 — HMB 선제 disable */
	if (ndev->hmb) {	/* [한국어] 절전 중에는 호스트 메모리가 사라질 수 있어 먼저 회수한다 */
		ret = nvme_set_host_mem(ndev, 0);	/* [한국어] HMB feature off */
		if (ret < 0)	/* [한국어] 컨트롤러가 회수를 거부했다 — 이 상태로 재우면 안 된다 */
			goto unfreeze;
	}

	ret = nvme_get_power_state(ctrl, &ndev->last_ps);	/* [한국어] 복귀 시 되돌릴 수 있도록 현재 전력 상태를 기억한다 */
	if (ret < 0)
		goto unfreeze;

	/*
	 * A saved state prevents pci pm from generically controlling the
	 * device's power. If we're using protocol specific settings, we don't
	 * want pci interfering.
	 */
	pci_save_state(pdev);	/* [한국어] PCI 제네릭 PM 이 D-state 를 건드리지 않게 앵커 */

	ret = nvme_set_power_state(ctrl, ctrl->npss);	/* [한국어] 가장 낮은 전력 상태로 — D3 대신 컨트롤러 자체 절전을 쓴다 */
	if (ret < 0)
		goto unfreeze;

	if (ret) {	/* [한국어] 양수는 NVMe 상태 코드다 — 컨트롤러가 이 전력 상태를 거절했다 */
		/* discard the saved state */
		pci_load_saved_state(pdev, NULL);	/* [한국어] 프로토콜 PS 실패 — PCI PM 다시 허용 */

		/*
		 * Clearing npss forces a controller reset on resume. The
		 * correct value will be rediscovered then.
		 */
		ret = nvme_disable_prepare_reset(ndev, true);	/* [한국어] full shutdown 폴백 */
		ctrl->npss = 0;	/* [한국어] resume 이 단순 PS 복귀 대신 리셋 경로 */
	}
unfreeze:
	nvme_unfreeze(ctrl);	/* [한국어] freeze 해제 (실패/성공 공통) */
	return ret;	/* [한국어] 실패하면 호출자가 D3 경로로 되돌린다 */
}

/*
 * [한국어]
 * nvme_simple_suspend - freeze/poweroff 용 무조건 컨트롤러 shutdown
 */
static int nvme_simple_suspend(struct device *dev)
{
	struct nvme_dev *ndev = pci_get_drvdata(to_pci_dev(dev));	/* [한국어] APST 를 못 믿는 장치용 단순 경로 */

	return nvme_disable_prepare_reset(ndev, true);	/* [한국어] CC SHN + 리셋 대기 */
}

/*
 * [한국어]
 * nvme_simple_resume - thaw/restore 용 reset 스케줄
 */
static int nvme_simple_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] D3 에서 올라오므로 PCI 쪽 복원이 먼저다 */
	struct nvme_dev *ndev = pci_get_drvdata(pdev);

	return nvme_try_sched_reset(&ndev->ctrl);	/* [한국어] full 재초기화 */
}

/* [한국어] 시스템 슬립 PM ops — suspend 는 프로토콜 PS 시도, freeze/poweroff 는 simple */
static const struct dev_pm_ops nvme_dev_pm_ops = {
	.suspend	= nvme_suspend,	/* [한국어] 호스트 관리 PS 또는 full shutdown */
	.resume		= nvme_resume,	/* [한국어] PS 복귀 또는 reset */
	.freeze		= nvme_simple_suspend,	/* [한국어] 하이버네이션 이미지 전 shutdown */
	.thaw		= nvme_simple_resume,	/* [한국어] freeze 취소 시 리셋 */
	.poweroff	= nvme_simple_suspend,	/* [한국어] 전원 차단 전 shutdown */
	.restore	= nvme_simple_resume,	/* [한국어] 하이버네이션 복원 후 리셋 */
};
#endif /* CONFIG_PM_SLEEP */

/*
 * [한국어]
 * nvme_error_detected - PCI AER/DPC 등 채널 상태 통지
 *
 * normal→CAN_RECOVER, frozen→RESETTING+disable→NEED_RESET,
 * perm_failure→DISCONNECT.
 */
static pci_ers_result_t nvme_error_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);	/* [한국어] 오류 난 컨트롤러 */

	/*
	 * A frozen channel requires a reset. When detected, this method will
	 * shutdown the controller to quiesce. The controller will be restarted
	 * after the slot reset through driver's slot_reset callback.
	 */
	switch (state) {	/* [한국어] 상태/유형 다중 분기 */
	case pci_channel_io_normal:	/* [한국어] 채널 정상 — 드라이버 조치 최소 */
		return PCI_ERS_RESULT_CAN_RECOVER;	/* [한국어] 링크 복구만으로 충분 가능 */
	case pci_channel_io_frozen:	/* [한국어] 동결 — MMIO/DMA 중단, 슬롯 리셋 필요 */
		dev_warn(dev->ctrl.device,	/* [한국어] 버스가 얼어붙었다 — 슬롯 리셋 없이는 복구되지 않는다 */
			"frozen state error detected, reset controller\n");
		if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_RESETTING)) {	/* [한국어] 상태 선점 실패 */
			nvme_dev_disable(dev, true);	/* [한국어] 정리 후 연결 끊기 */
			return PCI_ERS_RESULT_DISCONNECT;	/* [한국어] 드라이버 분리 요청 */
		}
		nvme_dev_disable(dev, false);	/* [한국어] 재기동 전제 disable */
		return PCI_ERS_RESULT_NEED_RESET;	/* [한국어] 플랫폼 슬롯 리셋 요청 */
	case pci_channel_io_perm_failure:	/* [한국어] 영구 실패 */
		dev_warn(dev->ctrl.device,	/* [한국어] 영구 장애 — 이 장치는 다시 살아나지 않는다 */
			"failure state error detected, request disconnect\n");
		return PCI_ERS_RESULT_DISCONNECT;	/* [한국어] 복구 불가 — remove */
	}
	return PCI_ERS_RESULT_NEED_RESET;	/* [한국어] 미지 상태 기본: 리셋 시도 */
}

/*
 * [한국어]
 * nvme_slot_reset - 슬롯 리셋 후 config restore + try_sched_reset
 */
static pci_ers_result_t nvme_slot_reset(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);	/* [한국어] 슬롯 리셋이 끝나 장치가 초기 상태로 돌아왔다 */

	dev_info(dev->ctrl.device, "restart after slot reset\n");	/* [한국어] 복구 시도를 기록에 남긴다 */
	pci_restore_state(pdev);	/* [한국어] 리셋으로 날아간 cfg 복구 */
	if (nvme_try_sched_reset(&dev->ctrl))	/* [한국어] reset_work 스케줄 */
		nvme_unquiesce_io_queues(&dev->ctrl);	/* [한국어] 스케줄 실패 시 교착 방지 */
	return PCI_ERS_RESULT_RECOVERED;	/* [한국어] 드라이버 측 복구 착수 완료 선언 */
}

/*
 * [한국어]
 * nvme_error_resume - 링크 복구 후 reset_work 완료 대기
 */
static void nvme_error_resume(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);	/* [한국어] 오류 복구가 끝난 뒤 정상 경로로 돌아오는 훅 */

	flush_work(&dev->ctrl.reset_work);	/* [한국어] AER resume 전에 리셋 완료 보장 */
}

/* [한국어] PCI AER/FLR 콜백 테이블 — frozen→reset→resume 흐름 연결 */
static const struct pci_error_handlers nvme_err_handler = {
	.error_detected	= nvme_error_detected,	/* [한국어] 채널 상태 1차 통지 */
	.slot_reset	= nvme_slot_reset,	/* [한국어] 슬롯 리셋 직후 */
	.resume		= nvme_error_resume,	/* [한국어] 링크 정상화 후 */
	.reset_prepare	= nvme_reset_prepare,	/* [한국어] FLR 직전 quiesce */
	.reset_done	= nvme_reset_done,	/* [한국어] FLR 직후 reset_work */
};

/*
 * [한국어] PCI ID 테이블: 특정 VID:DID 에 정적 quirk 를 붙이고, 말미
 * PCI_DEVICE_CLASS(STORAGE_EXPRESS) 로 나머지 NVMe 클래스 장치를 포괄 매칭.
 * driver_data 비트는 nvme_pci_alloc_dev 에서 ctrl.quirks 시드가 된다.
 */
static const struct pci_device_id nvme_id_table[] = {
	/* [한국어] 아래 각 엔트리: probe 매치 + driver_data 정적 quirk 시드 */
	{ PCI_VDEVICE(INTEL, 0x0953),	/* Intel 750/P3500/P3600/P3700 */ /* [한국어] 초기 Intel + stripe/dealloc */
		.driver_data = NVME_QUIRK_STRIPE_SIZE |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_DEALLOCATE_ZEROES, },	/* [한국어] 스트라이프·deallocate=zero */
	{ PCI_VDEVICE(INTEL, 0x0a53),	/* Intel P3520 */ /* [한국어] 동일 stripe/dealloc 계열 */
		.driver_data = NVME_QUIRK_STRIPE_SIZE |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_DEALLOCATE_ZEROES, },	/* [한국어] 스트라이프 정렬 quirk */
	{ PCI_VDEVICE(INTEL, 0x0a54),	/* Intel P4500/P4600 */ /* [한국어] subnqn/nid 이슈 동반 */
		.driver_data = NVME_QUIRK_STRIPE_SIZE |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_IGNORE_DEV_SUBNQN |
				NVME_QUIRK_BOGUS_NID, },	/* [한국어] Identify 필드 신뢰도 낮음 */
	{ PCI_VDEVICE(INTEL, 0x0a55),	/* Dell Express Flash P4600 */ /* [한국어] Dell OEM Intel */
		.driver_data = NVME_QUIRK_STRIPE_SIZE, },	/* [한국어] 스트라이프 크기 준수 */
	{ PCI_VDEVICE(INTEL, 0xf1a5),	/* Intel 600P/P3100 */ /* [한국어] 클라이언트 NVMe 다수 quirk */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_MEDIUM_PRIO_SQ |
				NVME_QUIRK_NO_TEMP_THRESH_CHANGE |
				NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] PS/우선순위/온도/WZ */
	{ PCI_VDEVICE(INTEL, 0xf1a6),	/* Intel 760p/Pro 7600p */ /* [한국어] subnqn 무시 */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN, },	/* [한국어] 잘못된 subnqn 무시 */
	{ PCI_VDEVICE(INTEL, 0x5845),	/* Qemu emulated controller */ /* [한국어] 가상화 에뮬레이션 */
		.driver_data = NVME_QUIRK_IDENTIFY_CNS |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_DISABLE_WRITE_ZEROES |
				NVME_QUIRK_BOGUS_NID, },	/* [한국어] QEMU Identify/NID 제한 */
	{ PCI_VDEVICE(REDHAT, 0x0010),	/* Qemu emulated controller */ /* [한국어] RedHat virtio-nvme 계 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] 가상 NID 비신뢰 */
	{ PCI_DEVICE(0x1217, 0x8760), /* O2 Micro 64GB Steam Deck */ /* [한국어] Steam Deck eMMC/NVMe 브리지 */
		.driver_data = NVME_QUIRK_DMAPOOL_ALIGN_512, },	/* [한국어] descriptor pool 512 정렬 */
	{ PCI_DEVICE(0x126f, 0x1001),	/* Silicon Motion generic */ /* [한국어] SM226x 계열 공통 */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },	/* [한국어] 최심 PS·subnqn 회피 */
	{ PCI_DEVICE(0x126f, 0x2262),	/* Silicon Motion generic */ /* [한국어] SM2262 */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_BOGUS_NID, },	/* [한국어] APST/NID 이슈 */
	{ PCI_DEVICE(0x126f, 0x2263),	/* Silicon Motion unidentified */ /* [한국어] SM2263 */
		.driver_data = NVME_QUIRK_NO_NS_DESC_LIST |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_BOGUS_NID, },	/* [한국어] NS desc list 미사용 */
	{ PCI_DEVICE(0x1bb1, 0x0100),   /* Seagate Nytro Flash Storage */ /* [한국어] 엔터프라이즈 Seagate */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_NO_NS_DESC_LIST, },	/* [한국어] RDY 대기 지연 필요 */
	{ PCI_DEVICE(0x1c58, 0x0003),	/* HGST adapter */ /* [한국어] HGST 어댑터 */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },	/* [한국어] enable 후 RDY 지연 */
	{ PCI_DEVICE(0x1c58, 0x0023),	/* WDC SN200 adapter */ /* [한국어] WD 엔터프라이즈 */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },	/* [한국어] RDY 폴링 전 지연 */
	{ PCI_DEVICE(0x1c5f, 0x0540),	/* Memblaze Pblaze4 adapter */ /* [한국어] Memblaze */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },	/* [한국어] 초기화 타이밍 quirk */
	{ PCI_DEVICE(0x144d, 0xa821),   /* Samsung PM1725 */ /* [한국어] 삼성 엔터프라이즈 PM1725 */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },	/* [한국어] MQES=0 버그와 연관 지연 */
	{ PCI_DEVICE(0x144d, 0xa822),   /* Samsung PM1725a */ /* [한국어] PM1725a */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_DISABLE_WRITE_ZEROES|
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },	/* [한국어] WZ/subnqn/RDY 복합 */
	{ PCI_DEVICE(0x15b7, 0x5008),   /* Sandisk SN530 */ /* [한국어] WD/SanDisk 클라이언트 */
		.driver_data = NVME_QUIRK_BROKEN_MSI },	/* [한국어] MSI 깨짐 — MSI-X/INTx 만 */
	{ PCI_DEVICE(0x15b7, 0x5009),   /* Sandisk SN550 */ /* [한국어] SN550 */
		.driver_data = NVME_QUIRK_BROKEN_MSI |	/* [한국어] MSI-X 완료 인터럽트 경로 */
				NVME_QUIRK_NO_DEEPEST_PS },	/* [한국어] MSI+최심 PS 금지 */
	{ PCI_DEVICE(0x1987, 0x5012),	/* Phison E12 */ /* [한국어] Phison 컨트롤러 OEM 다수 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NGUID/EUI 비고유 */
	{ PCI_DEVICE(0x1987, 0x5016),	/* Phison E16 */ /* [한국어] E16 */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_BOGUS_NID, },	/* [한국어] subnqn+NID */
	{ PCI_DEVICE(0x1987, 0x5019),  /* phison E19 */ /* [한국어] E19 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] Write Zeroes 비활성 */
	{ PCI_DEVICE(0x1987, 0x5021),   /* Phison E21 */ /* [한국어] E21 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ 미구현/버그 */
	{ PCI_DEVICE(0x1b4b, 0x1092),	/* Lexar 256 GB SSD */ /* [한국어] Lexar/Marvell 계 */
		.driver_data = NVME_QUIRK_NO_NS_DESC_LIST |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },	/* [한국어] NS list/subnqn */
	{ PCI_DEVICE(0x1cc1, 0x33f8),   /* ADATA IM2P33F8ABR1 1 TB */ /* [한국어] ADATA */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] 중복 NID */
	{ PCI_DEVICE(0x10ec, 0x5762),   /* ADATA SX6000LNP */ /* [한국어] Realtek NVMe */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_BOGUS_NID, },	/* [한국어] Realtek Identify 품질 */
	{ PCI_DEVICE(0x10ec, 0x5763),  /* ADATA SX6000PNP */ /* [한국어] Realtek 5763 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] 비고유 식별자 */
	{ PCI_DEVICE(0x1cc1, 0x8201),   /* ADATA SX8200PNP 512GB */ /* [한국어] SX8200 */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },	/* [한국어] APST/subnqn */
	 { PCI_DEVICE(0x1344, 0x5407), /* Micron Technology Inc NVMe SSD */ /* [한국어] Micron */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN },	/* [한국어] subnqn 무시 */
	 { PCI_DEVICE(0x1344, 0x6001),   /* Micron Nitro NVMe */ /* [한국어] Nitro */
		 .driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID 비신뢰 */
	{ PCI_DEVICE(0x1c5c, 0x1504),   /* SK Hynix PC400 */ /* [한국어] SK hynix */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ 비활성 */
	{ PCI_DEVICE(0x1c5c, 0x174a),   /* SK Hynix P31 SSD */ /* [한국어] P31 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] 중복 식별 */
	{ PCI_DEVICE(0x1c5c, 0x1D59),   /* SK Hynix BC901 */ /* [한국어] BC901 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ 미지원 */
	{ PCI_DEVICE(0x15b7, 0x2001),   /*  Sandisk Skyhawk */ /* [한국어] SanDisk 엔터프라이즈 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ 비활성 */
	{ PCI_DEVICE(0x1d97, 0x2263),   /* SPCC */ /* [한국어] SPCC/OEM */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x144d, 0xa80b),   /* Samsung PM9B1 256G and 512G */ /* [한국어] 삼성 PM9B1 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_BOGUS_NID, },	/* [한국어] WZ+NID */
	{ PCI_DEVICE(0x144d, 0xa809),   /* Samsung MZALQ256HBJD 256G */ /* [한국어] 삼성 클라이언트 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] Write Zeroes 끔 */
	{ PCI_DEVICE(0x144d, 0xa802),   /* Samsung SM953 */ /* [한국어] SM953 — DMI quirk 와 병행 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1cc4, 0x6303),   /* UMIS RPJTJ512MGE1QDY 512G */ /* [한국어] UMIS */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x1cc4, 0x6302),   /* UMIS RPJTJ256MGE1QDY 256G */ /* [한국어] UMIS 256G */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x2646, 0x2262),   /* KINGSTON SKC2000 NVMe SSD */ /* [한국어] Kingston */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS, },	/* [한국어] 최심 절전 금지 */
	{ PCI_DEVICE(0x2646, 0x2263),   /* KINGSTON A2000 NVMe SSD  */ /* [한국어] A2000 — DMI 조합 있음 */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS, },	/* [한국어] APST 최심 금지 */
	{ PCI_DEVICE(0x2646, 0x5013),   /* Kingston KC3000, Kingston FURY Renegade */ /* [한국어] KC3000 */
		.driver_data = NVME_QUIRK_NO_SECONDARY_TEMP_THRESH, },	/* [한국어] 2차 온도 임계 무시 */
	{ PCI_DEVICE(0x2646, 0x5018),   /* KINGSTON OM8SFP4xxxxP OS21012 NVMe SSD */ /* [한국어] OEM Kingston */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x2646, 0x5016),   /* KINGSTON OM3PGP4xxxxP OS21011 NVMe SSD */ /* [한국어] OEM */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x2646, 0x501A),   /* KINGSTON OM8PGP4xxxxP OS21005 NVMe SSD */ /* [한국어] OEM */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x2646, 0x501B),   /* KINGSTON OM8PGP4xxxxQ OS21005 NVMe SSD */ /* [한국어] OEM */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x2646, 0x501E),   /* KINGSTON OM3PGP4xxxxQ OS21011 NVMe SSD */ /* [한국어] OEM */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x2646, 0x502F),   /* KINGSTON OM3SGP4xxxxK NVMe SSD */ /* [한국어] OEM */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },	/* [한국어] WZ */
	{ PCI_DEVICE(0x1f40, 0x1202),   /* Netac Technologies Co. NV3000 NVMe SSD */ /* [한국어] Netac */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] 비고유 NID */
	{ PCI_DEVICE(0x1f40, 0x5236),   /* Netac Technologies Co. NV7000 NVMe SSD */ /* [한국어] Netac */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1e4B, 0x1001),   /* MAXIO MAP1001 */ /* [한국어] MAXIO 컨트롤러 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1e4B, 0x1002),   /* MAXIO MAP1002 */ /* [한국어] MAP1002 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1e4B, 0x1202),   /* MAXIO MAP1202 */ /* [한국어] MAP1202 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1e4B, 0x1602),   /* MAXIO MAP1602 */ /* [한국어] MAP1602 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1cc1, 0x5350),   /* ADATA XPG GAMMIX S50 */ /* [한국어] ADATA XPG */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1dbe, 0x5216),   /* Acer/INNOGRIT FA100/5216 NVMe SSD */ /* [한국어] Innogrit */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1dbe, 0x5236),   /* ADATA XPG GAMMIX S70 */ /* [한국어] Innogrit/ADATA */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1e49, 0x0021),   /* ZHITAI TiPro5000 NVMe SSD */ /* [한국어] ZHITAI */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS, },	/* [한국어] 최심 PS 금지 */
	{ PCI_DEVICE(0x1e49, 0x0041),   /* ZHITAI TiPro7000 NVMe SSD */ /* [한국어] TiPro7000 */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS, },	/* [한국어] APST 제한 */
	{ PCI_DEVICE(0x1fa0, 0x2283),   /* Wodposit WPBSNM8-256GTP */ /* [한국어] Wodposit */
		.driver_data = NVME_QUIRK_NO_SECONDARY_TEMP_THRESH, },	/* [한국어] 2차 온도 임계 무시 */
	{ PCI_DEVICE(0x025e, 0xf1ac),   /* SOLIDIGM  P44 pro SSDPFKKW020X7  */ /* [한국어] Solidigm/Intel 후속 */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS, },	/* [한국어] 최심 절전 금지 */
	{ PCI_DEVICE(0xc0a9, 0x540a),   /* Crucial P2 */ /* [한국어] Crucial */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1d97, 0x2263), /* Lexar NM610 */ /* [한국어] Lexar */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1d97, 0x1d97), /* Lexar NM620 */ /* [한국어] NM620 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1d97, 0x2269), /* Lexar NM760 */ /* [한국어] NM760 */
		.driver_data = NVME_QUIRK_BOGUS_NID |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },	/* [한국어] NID+subnqn */
	{ PCI_DEVICE(0x10ec, 0x5763), /* TEAMGROUP T-FORCE CARDEA ZERO Z330 SSD */ /* [한국어] Realtek OEM */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x1e4b, 0x1602), /* HS-SSD-FUTURE 2048G  */ /* [한국어] MAXIO OEM */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(0x10ec, 0x5765), /* TEAMGROUP MP33 2TB SSD */ /* [한국어] Realtek */
		.driver_data = NVME_QUIRK_BOGUS_NID, },	/* [한국어] NID */
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x0061),	/* [한국어] AWS Nitro/EFA 계 NVMe */
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },	/* [한국어] 48bit DMA 마스크 */
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x0065),	/* [한국어] Amazon NVMe */
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },	/* [한국어] 플랫폼 48bit 제한 */
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x8061),	/* [한국어] Amazon NVMe */
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },	/* [한국어] 48bit DMA */
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd00),	/* [한국어] Amazon NVMe */
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },	/* [한국어] 48bit DMA */
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd01),	/* [한국어] Amazon NVMe */
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },	/* [한국어] 48bit DMA */
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd02),	/* [한국어] Amazon NVMe */
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },	/* [한국어] 48bit DMA */
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2001),	/* [한국어] Apple ANS MacBook 구형 */
		/*
		 * Fix for the Apple controller found in the MacBook8,1 and
		 * some MacBook7,1 to avoid controller resets and data loss.
		 */
		/* [한국어] 단일 벡터+깊이1 — 리셋/데이터 손실 방지 */
		.driver_data = NVME_QUIRK_SINGLE_VECTOR |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_QDEPTH_ONE },	/* [한국어] 극단 단순 큐 토폴로지 */
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2003) },	/* [한국어] Apple ANS — 기본 매치, 추가 quirk 없음 */
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2005),	/* [한국어] Apple ANS2 — 비표준 SQE/태그 */
		.driver_data = NVME_QUIRK_SINGLE_VECTOR |	/* [한국어] PCI ID 테이블 quirk 시드 */
				NVME_QUIRK_128_BYTES_SQES |
				NVME_QUIRK_SHARED_TAGS |
				NVME_QUIRK_SKIP_CID_GEN |
				NVME_QUIRK_IDENTIFY_CNS },	/* [한국어] 128B SQE·공유태그·CID 스킵 */
	{ PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) },	/* [한국어] 나머지 모든 NVMe Express 클래스 포괄 매치 */
	{ 0, }	/* [한국어] 테이블 종료 센티널 */
};
MODULE_DEVICE_TABLE(pci, nvme_id_table);	/* [한국어] depmod/자동 로드용 ID 테이블 노출 */

/*
 * [한국어] PCI 서브시스템에 등록되는 nvme 호스트 드라이버 디스크립터.
 * 클래스 매치(STORAGE_EXPRESS)로 대부분 SSD 를 잡고, id_table quirk 가 우선.
 */
static struct pci_driver nvme_driver = {
	.name		= "nvme",	/* [한국어] sysfs/드라이버 이름 */
	.id_table	= nvme_id_table,	/* [한국어] VID:DID + 클래스 매치 + quirk */
	.probe		= nvme_probe,	/* [한국어] 바인드 시 전체 초기화 */
	.remove		= nvme_remove,	/* [한국어] 언바인드 tear-down */
	.shutdown	= nvme_shutdown,	/* [한국어] 시스템 종료 시 정상 SHN */
	.driver		= {
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,	/* [한국어] 느린 Identify 를 비동기 probe */
#ifdef CONFIG_PM_SLEEP
		.pm		= &nvme_dev_pm_ops,	/* [한국어] suspend/resume/freeze 훅 */
#endif
	},
	.sriov_configure = pci_sriov_configure_simple,	/* [한국어] SR-IOV VF 수 설정 위임 */
	.err_handler	= &nvme_err_handler,	/* [한국어] AER/FLR 복구 콜백 */
};

/*
 * [한국어]
 * nvme_init - 모듈 로드: SQE 크기 불변식 검증 후 pci_register_driver
 */
static int __init nvme_init(void)
{
	BUILD_BUG_ON(sizeof(struct nvme_create_cq) != 64);	/* [한국어] Create CQ SQE 는 정확히 64B */
	BUILD_BUG_ON(sizeof(struct nvme_create_sq) != 64);	/* [한국어] Create SQ SQE 64B */
	BUILD_BUG_ON(sizeof(struct nvme_delete_queue) != 64);	/* [한국어] Delete Q SQE 64B */
	BUILD_BUG_ON(IRQ_AFFINITY_MAX_SETS < 2);	/* [한국어] DEFAULT+READ 최소 2 affinity set */

	return pci_register_driver(&nvme_driver);	/* [한국어] 기존 NVMe PCI 함수에 probe 매칭 */
}

/*
 * [한국어]
 * nvme_exit - quirk 리스트 free, pci_unregister, nvme_wq flush
 */
static void __exit nvme_exit(void)
{
	kfree(nvme_pci_quirk_list);	/* [한국어] 동적 quirks= 테이블 */
	pci_unregister_driver(&nvme_driver);	/* [한국어] 전 장치 remove 경로 유도 */
	flush_workqueue(nvme_wq);	/* [한국어] core 공유 워크큐 잔여 작업 배수 */
}

MODULE_AUTHOR("Matthew Wilcox <willy@linux.intel.com>");	/* [한국어] 원저자 메타 */
MODULE_LICENSE("GPL");	/* [한국어] GPL 라이선스 — 커널 모듈 연계 */
MODULE_VERSION("1.0");	/* [한국어] 모듈 버전 문자열 */
MODULE_DESCRIPTION("NVMe host PCIe transport driver");	/* [한국어] modinfo 설명 — PCIe 호스트 트랜스포트 */
module_init(nvme_init);	/* [한국어] 로드 진입점 */
module_exit(nvme_exit);	/* [한국어] 언로드 진입점 */

