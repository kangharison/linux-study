// SPDX-License-Identifier: GPL-2.0
/*
 * Apple ANS NVM Express device driver
 * Copyright The Asahi Linux Contributors
 *
 * Based on the pci.c NVM Express device driver
 * Copyright (c) 2011-2014, Intel Corporation.
 * and on the rdma.c NVMe over Fabrics RDMA host code.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */

/*
 * [한국어 설명] Apple ANS(Apple NVMe Storage) 플랫폼 컨트롤러 드라이버 (apple.c)
 *
 * === 파일의 역할 ===
 * Apple Silicon(T8015/T8103 계열 등) SoC 에 내장된 ANS 코프로세서 NVMe
 * 컨트롤러를 구동하는 플랫폼(non-PCI) 호스트 드라이버다. 표준 PCIe NVMe
 * 와 달리 (1) SQ 가 태그 인덱스 배열 + 도어벨에 태그 기록 방식,
 * (2) 임베디드 NVMMU TCB(Translation Control Block) 가 명령마다 필수,
 * (3) Admin/IO 가 단일 공유 태그 공간(실효 깊이 최대 0x40),
 * (4) RTKit 펌웨어 부팅 + SART DMA 허용 창,
 * (5) genpd 전원 도메인 다수 부착
 * 이라는 SoC 특수부를 처리한다. 블록 I/O 의미론은 core.c 의 nvme_ctrl 에
 * 올라타며, 트랜스포트 ops 로 reg_read/write, queue_rq, timeout 을 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   platform_probe → MMIO map → RTKit boot → SART → NVMMU/큐 할당
 *        → CC.EN → Identify(core) → LIVE
 *   핫패스:
 *     queue_rq → setup PRP + TCB 기록 → submit_cmd(태그 도어벨)
 *        → IRQ → poll_cq → handle_cqe → unmap → complete
 *
 * === 타 모듈과의 연결 ===
 * - core.c : nvme_init_ctrl, enable/disable, tagset, ns scan
 * - apple,rtkit / apple,sart : 코프록 펌웨어·DMA 허용
 * - genpd / reset_control : 전원·리셋 라인
 * - dma-mapping / dmapool / mempool : PRP 리스트·iod
 * - blk-mq : admin/io tagset (깊이가 Apple 한도에 클램프)
 *
 * === 주요 자료구조 ===
 * - apple_nvme : 장치 전역. MMIO, rtk/sart, adminq/ioq, tagset
 * - apple_nvme_queue : SQE/CQE/TCB DMA 링, 도어벨, phase
 * - apple_nvmmu_tcb : 태그별 128B TCB (opcode, DMA 방향, PRP, AES 자리)
 * - apple_nvme_iod : 요청별 PRP/sg 매핑 상태
 * - apple_nvme_hw : SoC 변형(has_lsq_nvmmu, max_queue_depth)
 *
 * === 핫패스 vs 컨트롤 플레인 ===
 * 핫패스: queue_rq → map_data/setup_prps → TCB → doorbell → irq/poll
 * 컨트롤: probe/remove, rtkit boot, reset_work, suspend/resume, create sq/cq
 *
 * === 주요 함수/구조체 요약 ===
 * - apple_nvme_probe / apple_nvme_alloc: 플랫폼 장치에서 MMIO·RTKit·SART·genpd 를
 *   갖추고 nvme_ctrl 을 등록하는 진입점. 여기서 실패하면 컨트롤러가 존재하지 않는다.
 * - apple_nvme_reset_work: CC.EN 재인가와 큐 재생성을 묶은 리셋 상태 기계.
 *   RTKit 펌웨어 부팅과 NVMMU 초기화가 이 안에서 순서대로 일어난다.
 * - apple_nvme_queue_rq: 핫패스 제출. PRP 를 채우고 태그별 TCB 를 기록한 뒤
 *   도어벨에 태그를 써 넣는다. 표준 NVMe 가 SQ tail 을 쓰는 것과 다른 지점이다.
 * - apple_nvme_handle_cq / apple_nvme_irq: 완료 수확. phase 비트로 새 CQE 를
 *   가려내고 NVMMU 무효화 후 blk-mq 에 완료를 넘긴다.
 * - apple_nvme_timeout: 응답 없는 명령의 처리. 표준 abort 대신 컨트롤러 리셋으로
 *   간다 -- 단일 공유 태그 공간이라 개별 abort 의 이득이 적기 때문이다.
 * - struct apple_nvme: 장치 전역 상태(MMIO, rtk/sart, adminq/ioq, tagset).
 * - struct apple_nvme_queue: SQE/CQE/TCB DMA 링과 도어벨, phase.
 * - struct apple_nvmmu_tcb: 태그마다 하나씩 있는 128B 변환 제어 블록.
 * - struct apple_nvme_iod: 요청별 PRP/sg 매핑 상태.
 */

#include <linux/async.h>	/* [한국어] async_schedule — probe 후 reset/scan flush 비동기화 */

#include <linux/blkdev.h>	/* [한국어] 블록 장치 코어 타입 */
#include <linux/blk-mq.h>	/* [한국어] blk-mq tagset/hctx/request */
#include <linux/device.h>	/* [한국어] struct device */
#include <linux/dma-mapping.h>	/* [한국어] DMA map/unmap API */
#include <linux/dmapool.h>	/* [한국어] DMA 풀 — PRP 리스트 */
#include <linux/interrupt.h>	/* [한국어] request_irq / irqreturn_t */
#include <linux/io-64-nonatomic-lo-hi.h>	/* [한국어] 64bit MMIO 분할 접근 */
#include <linux/io.h>	/* [한국어] ioremap/readl/writel */
#include <linux/iopoll.h>	/* [한국어] readl_poll_timeout — 부팅 상태 대기 */
#include <linux/jiffies.h>	/* [한국어] 시간 단위 */
#include <linux/mempool.h>	/* [한국어] iod+sg mempool */
#include <linux/module.h>	/* [한국어] 모듈 매크로 */
#include <linux/of.h>	/* [한국어] Device Tree 파싱 */
#include <linux/of_platform.h>	/* [한국어] OF platform 헬퍼 */
#include <linux/once.h>	/* [한국어] DO_ONCE — bad SGL 1회 덤프 */
#include <linux/platform_device.h>	/* [한국어] platform_driver */
#include <linux/pm_domain.h>	/* [한국어] genpd 부착 */
#include <linux/soc/apple/rtkit.h>	/* [한국어] Apple RTKit 코프록 펌웨어 */
#include <linux/soc/apple/sart.h>	/* [한국어] Apple SART DMA 허용 창 */
#include <linux/reset.h>	/* [한국어] reset_control */
#include <linux/time64.h>	/* [한국어] USEC_PER_SEC 등 시간 상수 */

#include "nvme.h"	/* [한국어] host 내부 nvme_ctrl/요청 API */

#define APPLE_ANS_BOOT_TIMEOUT	  USEC_PER_SEC	/* [한국어] ANS 펌웨어 부팅 폴링 상한 1초 */

#define APPLE_ANS_COPROC_CPU_CONTROL	 0x44	/* [한국어] 코프록 CPU 제어 레지스터 오프셋 */
#define APPLE_ANS_COPROC_CPU_CONTROL_RUN BIT(4)	/* [한국어] 코프록 RUN 비트 — 펌웨어 실행 */

#define APPLE_ANS_ACQ_DB  0x1004	/* [한국어] Admin CQ 도어벨 MMIO */
#define APPLE_ANS_IOCQ_DB 0x100c	/* [한국어] I/O CQ 도어벨 MMIO */

#define APPLE_ANS_MAX_PEND_CMDS_CTRL 0x1210	/* [한국어] 큐별 최대 대기 명령 수 설정 */

#define APPLE_ANS_BOOT_STATUS	 0x1300	/* [한국어] ANS 부팅 상태 레지스터 */
#define APPLE_ANS_BOOT_STATUS_OK 0xde71ce55	/* [한국어] 부팅 완료 매직 (de71ce55) */

#define APPLE_ANS_UNKNOWN_CTRL	 0x24008	/* [한국어] 벤더 제어 — PRP null 검사 등 */
#define APPLE_ANS_PRP_NULL_CHECK BIT(11)	/* [한국어] PRP=0 거부 비트 — 클리어해야 정상 동작 */

#define APPLE_ANS_LINEAR_SQ_CTRL 0x24908	/* [한국어] 선형 SQ 모드 제어 */
#define APPLE_ANS_LINEAR_SQ_EN	 BIT(0)	/* [한국어] 선형 SQ+NVMMU 활성화 */

#define APPLE_ANS_LINEAR_ASQ_DB	 0x2490c	/* [한국어] 선형 Admin SQ 도어벨(태그 기록) */
#define APPLE_ANS_LINEAR_IOSQ_DB 0x24910	/* [한국어] 선형 I/O SQ 도어벨(태그 기록) */

#define APPLE_NVMMU_NUM_TCBS	  0x28100	/* [한국어] TCB 슬롯 수(깊이-1) 설정 */
#define APPLE_NVMMU_ASQ_TCB_BASE  0x28108	/* [한국어] Admin TCB 배열 DMA 베이스 */
#define APPLE_NVMMU_IOSQ_TCB_BASE 0x28110	/* [한국어] I/O TCB 배열 DMA 베이스 */
#define APPLE_NVMMU_TCB_INVAL	  0x28118	/* [한국어] 태그 TCB 무효화 도어벨 */
#define APPLE_NVMMU_TCB_STAT	  0x28120	/* [한국어] TCB 무효화 상태(비0=실패) */

/*
 * This controller is a bit weird in the way command tags works: Both the
 * admin and the IO queue share the same tag space. Additionally, tags
 * cannot be higher than 0x40 which effectively limits the combined
 * queue depth to 0x40. Instead of wasting half of that on the admin queue
 * which gets much less traffic we instead reduce its size here.
 * The controller also doesn't support async event such that no space must
 * be reserved for NVME_NR_AEN_COMMANDS.
 */
#define APPLE_NVME_AQ_DEPTH	   2	/* [한국어] Admin 큐 깊이 — 공유 태그 공간 절약 */
#define APPLE_NVME_AQ_MQ_TAG_DEPTH (APPLE_NVME_AQ_DEPTH - 1)	/* [한국어] blk-mq admin 태그 깊이(1 예약) */

#define APPLE_NVME_IOSQES	7	/* [한국어] I/O SQE 크기 log2 — 128B 슬롯(구형 경로) */

/*
 * These can be higher, but we need to ensure that any command doesn't
 * require an sg allocation that needs more than a page of data.
 */
#define NVME_MAX_KB_SZ 4096	/* [한국어] 단일 요청 최대 킬로바이트 — iod 페이지 한도 */
#define NVME_MAX_SEGS  127	/* [한국어] 최대 물리 세그먼트 — sg 가 한 페이지 이내 */

/*
 * This controller comes with an embedded IOMMU known as NVMMU.
 * The NVMMU is pointed to an array of TCBs indexed by the command tag.
 * Each command must be configured inside this structure before it's allowed
 * to execute, including commands that don't require DMA transfers.
 *
 * An exception to this are Apple's vendor-specific commands (opcode 0xD8 on the
 * admin queue): Those commands must still be added to the NVMMU but the DMA
 * buffers cannot be represented as PRPs and must instead be allowed using SART.
 *
 * Programming the PRPs to the same values as those in the submission queue
 * looks rather silly at first. This hardware is however designed for a kernel
 * that runs the NVMMU code in a higher exception level than the NVMe driver.
 * In that setting the NVMe driver first programs the submission queue entry
 * and then executes a hypercall to the code that is allowed to program the
 * NVMMU. The NVMMU driver then creates a shadow copy of the PRPs while
 * verifying that they don't point to kernel text, data, pagetables, or similar
 * protected areas before programming the TCB to point to this shadow copy.
 * Since Linux doesn't do any of that we may as well just point both the queue
 * and the TCB PRP pointer to the same memory.
 */
/*
 * [한국어] apple_nvmmu_tcb — 태그 인덱스 TCB 128B
 * 제출 전 필수 프로그램, 완료 후 inval MMIO. SQE 의 PRP 와 동일 포인터를
 * 가리키는 단순 모델(하이퍼바이저 없는 Linux). AES 필드는 HW 자리.
 */
struct apple_nvmmu_tcb {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	u8 opcode;	/* [한국어] 명령 opcode — SQE 와 동기 */

#define APPLE_ANS_TCB_DMA_FROM_DEVICE BIT(0)	/* [한국어] 디바이스→호스트(읽기) */
#define APPLE_ANS_TCB_DMA_TO_DEVICE   BIT(1)	/* [한국어] 호스트→디바이스(쓰기) */
	u8 dma_flags;	/* [한국어] DMA 방향 비트 — nvme_is_write 로 설정 */

	u8 command_id;	/* [한국어] 태그/CID — TCB 슬롯 인덱스와 동일 */
	u8 _unk0;	/* [한국어] 미문서 예약 */
	__le16 length;	/* [한국어] 전송 길이(NLB 계열) */
	u8 _unk1[18];	/* [한국어] 미문서 패딩 */
	__le64 prp1;	/* [한국어] PRP1 — SQE dptr 와 동일 DMA */
	__le64 prp2;	/* [한국어] PRP2 또는 PRP list */
	u8 _unk2[16];	/* [한국어] 미문서 패딩 */
	u8 aes_iv[8];	/* [한국어] AES IV 자리(미사용 가능) */
	u8 _aes_unk[64];	/* [한국어] AES 관련 예약 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * The Apple NVMe controller only supports a single admin and a single IO queue
 * which are both limited to 64 entries and share a single interrupt.
 *
 * The completion queue works as usual. The submission "queue" instead is
 * an array indexed by the command tag on this hardware. Commands must also be
 * present in the NVMMU's tcb array. They are triggered by writing their tag to
 * a MMIO register.
 */
/*
 * [한국어] apple_nvme_queue — SQE 배열·CQE 링·TCB 배열·도어벨·phase
 * 표준 링 tail 대신 태그 인덱스 제출(t8103) 또는 소형 링 tail(t8015).
 */
struct apple_nvme_queue {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	struct nvme_command *sqes;	/* [한국어] 제출 엔트리 배열 VA */
	struct nvme_completion *cqes;	/* [한국어] 완료 링 VA */
	struct apple_nvmmu_tcb *tcbs;	/* [한국어] NVMMU TCB 배열(선형 SQ 시) */

	dma_addr_t sq_dma_addr;	/* [한국어] SQ DMA 베이스 */
	dma_addr_t cq_dma_addr;	/* [한국어] CQ DMA 베이스 */
	dma_addr_t tcb_dma_addr;	/* [한국어] TCB 배열 DMA 베이스 */

	u32 __iomem *sq_db;	/* [한국어] 제출 도어벨 MMIO */
	u32 __iomem *cq_db;	/* [한국어] 완료 도어벨 MMIO */

	u16 sq_tail;	/* [한국어] 호스트 tail(구형 링 모델) */
	u16 cq_head;	/* [한국어] 완료 head 인덱스 */
	u8 cq_phase;	/* [한국어] 기대 phase 비트(1로 시작) */

	bool is_adminq;	/* [한국어] admin 큐 여부 */
	bool enabled;	/* [한국어] 제출 허용 — disable 시 queue_rq 거부 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * The apple_nvme_iod describes the data in an I/O.
 *
 * The sg pointer contains the list of PRP chunk allocations in addition
 * to the actual struct scatterlist.
 */
/*
 * [한국어] apple_nvme_iod — 요청별 PRP/sg 매핑 (npages, nents, first_dma)
 * blk-mq PDU 로 붙으며 cmd 사본과 소속 큐 포인터를 보관.
 */
struct apple_nvme_iod {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	struct nvme_request req;	/* [한국어] core 요청 메타(status/flags) — 반드시 선두 */
	struct nvme_command cmd;	/* [한국어] 제출용 64B SQE 사본 */
	struct apple_nvme_queue *q;	/* [한국어] 소속 admin/io 큐 */
	int npages; /* In the PRP list. 0 means small pool in use */	/* [한국어] PRP 리스트 페이지 수; 0=small pool, -1=미설정 */
	int nents; /* Used in scatterlist */	/* [한국어] dma_map_sg 엔트리 수 */
	dma_addr_t first_dma;	/* [한국어] 첫 PRP 리스트 DMA 또는 단일 맵 주소 */
	unsigned int dma_len; /* length of single DMA segment mapping */	/* [한국어] 단순 단일 세그먼트 맵 길이; 0이면 sg 경로 */
	struct scatterlist *sg;	/* [한국어] scatterlist(+PRP 페이지 포인터 뒤쪽) */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] SoC 변형 테이블 — OF match data 로 probe 시 선택 */
struct apple_nvme_hw {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	bool has_lsq_nvmmu;	/* [한국어] 선형 SQ+NVMMU(t8103+) 여부 */
	u32 max_queue_depth;	/* [한국어] 공유 태그 공간 최대 깊이 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어] apple_nvme — 장치 전역 상태
 * MMIO(ans/nvme), RTKit/SART, genpd, 두 큐, tagset, 공유 IRQ/락.
 */
struct apple_nvme {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	struct device *dev;	/* [한국어] platform device */

	void __iomem *mmio_coproc;	/* [한국어] ANS 코프록 MMIO 창 */
	void __iomem *mmio_nvme;	/* [한국어] NVMe 레지스터 MMIO 창 */
	const struct apple_nvme_hw *hw;	/* [한국어] SoC 변형 상수 */

	struct device **pd_dev;	/* [한국어] genpd 디바이스 배열 */
	struct device_link **pd_link;	/* [한국어] device_link 배열 */
	int pd_count;	/* [한국어] genpd 개수(≤1 이면 부착 생략) */

	struct apple_sart *sart;	/* [한국어] SART 핸들 — 벤더 DMA 허용 */
	struct apple_rtkit *rtk;	/* [한국어] RTKit 펌웨어 핸들 */
	struct reset_control *reset;	/* [한국어] soft-reset 라인 */

	struct dma_pool *prp_page_pool;	/* [한국어] 4K PRP 리스트 풀 */
	struct dma_pool *prp_small_pool;	/* [한국어] 256B 짧은 PRP 풀 */
	mempool_t *iod_mempool;	/* [한국어] iod 부가 sg 할당 풀 */

	struct nvme_ctrl ctrl;	/* [한국어] 임베드 공통 컨트롤러 */
	struct work_struct remove_work;	/* [한국어] 데드 컨트롤러 제거 워크 */

	struct apple_nvme_queue adminq;	/* [한국어] admin 큐 임베드 */
	struct apple_nvme_queue ioq;	/* [한국어] I/O 큐 임베드(단일) */

	struct blk_mq_tag_set admin_tagset;	/* [한국어] admin tagset */
	struct blk_mq_tag_set tagset;	/* [한국어] I/O tagset */

	int irq;	/* [한국어] 공유 인터럽트 번호 */
	spinlock_t lock;	/* [한국어] 제출/완료 직렬화 — 펌웨어 버그 회피 포함 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static_assert(sizeof(struct nvme_command) == 64);	/* [한국어] SQE 64B 스펙 고정 — 빌드타임 검증 */
static_assert(sizeof(struct apple_nvmmu_tcb) == 128);	/* [한국어] TCB 128B HW 레이아웃 고정 */

/*
 * [한국어]
 * ctrl_to_apple_nvme - nvme_ctrl → apple_nvme container_of 관문
 *
 * core 콜백이 ctrl 만 넘길 때 MMIO/큐/rtk 접근 단일 입구.
 * 호출 체인: reg_read/write, free_ctrl, get_address → [여기]
 */
static inline struct apple_nvme *ctrl_to_apple_nvme(struct nvme_ctrl *ctrl)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	return container_of(ctrl, struct apple_nvme, ctrl);	/* [한국어] 임베드 ctrl 역참조 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * queue_to_apple_nvme - queue 가 adminq/ioq 중 어디 임베드인지로 장치 역참조
 *
 * is_adminq 로 container_of 대상을 분기. 핫패스/IRQ 공통.
 */
static inline struct apple_nvme *queue_to_apple_nvme(struct apple_nvme_queue *q)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (q->is_adminq)	/* [한국어] admin 큐면 apple_nvme.adminq 임베드 */
		return container_of(q, struct apple_nvme, adminq);	/* [한국어] adminq 멤버 기준 역참조 */

	return container_of(q, struct apple_nvme, ioq);	/* [한국어] ioq 멤버 기준 역참조 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_queue_depth - admin 은 AQ_DEPTH, IO 는 hw->max_queue_depth
 *
 * 선형 SQ+NVMMU 에서 admin 깊이를 2 로 줄여 공유 태그 공간을 I/O 에 양보.
 */
static unsigned int apple_nvme_queue_depth(struct apple_nvme_queue *q)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] 소속 장치 — 깊이 정책 조회 */

	if (q->is_adminq && anv->hw->has_lsq_nvmmu)	/* [한국어] 신형: admin 만 축소 깊이 */
		return APPLE_NVME_AQ_DEPTH;	/* [한국어] admin=2 — 태그 공간 절약 */

	return anv->hw->max_queue_depth;	/* [한국어] SoC 최대(16 또는 64) */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_rtkit_crashed - ANS 펌웨어 크래시 콜백
 *
 * 복구 불가 경고 후 nvme_reset_ctrl. 실제 soft-reset 도 실패할 수 있음.
 * 호출 체인: RTKit → [여기] → reset_work
 */
static void apple_nvme_rtkit_crashed(void *cookie, const void *crashlog, size_t crashlog_size)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = cookie;	/* [한국어] rtkit 등록 시 넘긴 장치 포인터 */

	dev_warn(anv->dev, "RTKit crashed; unable to recover without a reboot");	/* [한국어] 재부팅 없이는 복구 불가 경고 */
	nvme_reset_ctrl(&anv->ctrl);	/* [한국어] 리셋 워크 스케줄 — 크래시 시 실패 가능 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_sart_dma_setup - SART 창에 DMA 허용 영역 등록
 *
 * RTKit 공유 메모리용 coherent 할당 후 SART allow. 벤더 명령 버퍼 경로.
 * 실패 시 -ENOMEM/-EINVAL. 호출 체인: RTKit shmem_setup → [여기]
 */
static int apple_nvme_sart_dma_setup(void *cookie,	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
				     struct apple_rtkit_shmem *bfr)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = cookie;	/* [한국어] 장치 컨텍스트 */
	int ret;	/* [한국어] SART 등록 결과 */

	if (bfr->iova)	/* [한국어] 이미 IOVA 있으면 프로토콜 오류 */
		return -EINVAL;	/* [한국어] 호출자에 잘못된 인자 전파 */
	if (!bfr->size)	/* [한국어] 크기 0 거부 */
		return -EINVAL;	/* [한국어] 빈 창 불가 */

	bfr->buffer =	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		dma_alloc_coherent(anv->dev, bfr->size, &bfr->iova, GFP_KERNEL);	/* [한국어] 일관 DMA 버퍼+IOVA 확보 */
	if (!bfr->buffer)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */

	ret = apple_sart_add_allowed_region(anv->sart, bfr->iova, bfr->size);	/* [한국어] SART 에 DMA 허용 창 등록 */
	if (ret) {	/* [한국어] SART 실패 시 coherent 롤백 */
		dma_free_coherent(anv->dev, bfr->size, bfr->buffer, bfr->iova);	/* [한국어] 부분 성공 잔존 방지 */
		bfr->buffer = NULL;	/* [한국어] 호출자에 무효 버퍼 표기 */
		return -ENOMEM;	/* [한국어] 상위는 메모리 실패로 해석 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return 0;	/* [한국어] 공유 메모리 창 준비 완료 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_sart_dma_destroy - SART 창 해제 및 coherent free
 *
 * setup 의 대칭. 호출 체인: RTKit shmem_destroy → [여기]
 */
static void apple_nvme_sart_dma_destroy(void *cookie,	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
					struct apple_rtkit_shmem *bfr)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = cookie;	/* [한국어] 장치 컨텍스트 */

	apple_sart_remove_allowed_region(anv->sart, bfr->iova, bfr->size);	/* [한국어] SART 허용 창 제거 */
	dma_free_coherent(anv->dev, bfr->size, bfr->buffer, bfr->iova);	/* [한국어] 일관 DMA 버퍼 반환 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] RTKit ops — 크래시·공유메모리 수명을 이 드라이버에 연결 */
static const struct apple_rtkit_ops apple_nvme_rtkit_ops = {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	.crashed = apple_nvme_rtkit_crashed,	/* [한국어] 펌웨어 크래시 → 리셋 시도 */
	.shmem_setup = apple_nvme_sart_dma_setup,	/* [한국어] 공유 mem 할당+SART */
	.shmem_destroy = apple_nvme_sart_dma_destroy,	/* [한국어] 공유 mem 해제 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvmmu_inval - 태그 슬롯 TCB 무효화 MMIO
 *
 * 완료/타임아웃 후 태그 재사용 전 필수. STAT 비0 이면 경고(드묾).
 * 호출 체인: handle_cqe → [여기]
 */
static void apple_nvmmu_inval(struct apple_nvme_queue *q, unsigned int tag)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] MMIO 베이스 접근용 장치 */

	writel(tag, anv->mmio_nvme + APPLE_NVMMU_TCB_INVAL);	/* [한국어] 태그 인덱스로 TCB 무효화 요청 */
	if (readl(anv->mmio_nvme + APPLE_NVMMU_TCB_STAT))	/* [한국어] HW 가 실패를 보고하면 */
		dev_warn_ratelimited(anv->dev,	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
				     "NVMMU TCB invalidation failed\n");	/* [한국어] rate-limit 경고 — 태그 누수 위험 신호 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_submit_cmd_t8015 - T8015: 링 tail 에 SQE 복사 후 도어벨
 *
 * 구형: 표준에 가까운 tail 증가 모델. spin_lock 으로 제출 직렬화.
 * 핫패스. 호출 체인: queue_rq → [여기]
 */
static void apple_nvme_submit_cmd_t8015(struct apple_nvme_queue *q,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
				  struct nvme_command *cmd)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] 깊이·락 소유 장치 */

	spin_lock_irq(&anv->lock);	/* [한국어] 제출/IRQ 완료와 직렬화 */

	if (q->is_adminq)	/* [한국어] admin: 64B 슬롯 배열 */
		memcpy(&q->sqes[q->sq_tail], cmd, sizeof(*cmd));	/* [한국어] tail 슬롯에 SQE 기록 */
	else	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		memcpy((void *)q->sqes + (q->sq_tail << APPLE_NVME_IOSQES),	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
			cmd, sizeof(*cmd));	/* [한국어] I/O: 128B 스트라이드 슬롯에 SQE */

	if (++q->sq_tail == anv->hw->max_queue_depth)	/* [한국어] tail 랩어라운드 */
		q->sq_tail = 0;	/* [한국어] 링 시작으로 */

	writel(q->sq_tail, q->sq_db);	/* [한국어] 새 tail 을 SQ 도어벨에 게시 — HW 실행 시작 */
	spin_unlock_irq(&anv->lock);	/* [한국어] 제출 임계구역 종료 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */


/*
 * [한국어]
 * apple_nvme_submit_cmd_t8103 - T8103: TCB 프로그램 + 태그 인덱스 SQE + 태그 도어벨
 *
 * 선형 SQ+NVMMU 핫패스. TCB 에 opcode/PRP/방향 기록 후 sqes[tag] 복사,
 * 도어벨에 tag 기록. 락은 펌웨어 버그(완료 IRQ 구간 레이스) 회피용.
 * 호출 체인: queue_rq → [여기]
 */
static void apple_nvme_submit_cmd_t8103(struct apple_nvme_queue *q,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
				  struct nvme_command *cmd)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] 락·장치 컨텍스트 */
	u32 tag = nvme_tag_from_cid(cmd->common.command_id);	/* [한국어] CID=공유 태그 공간 인덱스 */
	struct apple_nvmmu_tcb *tcb = &q->tcbs[tag];	/* [한국어] 태그 슬롯 TCB — 제출 전 필수 */

	tcb->opcode = cmd->common.opcode;	/* [한국어] TCB opcode = SQE opcode */
	tcb->prp1 = cmd->common.dptr.prp1;	/* [한국어] PRP1 를 TCB 에 미러 */
	tcb->prp2 = cmd->common.dptr.prp2;	/* [한국어] PRP2/list 미러 */
	tcb->length = cmd->rw.length;	/* [한국어] 전송 길이 필드 */
	tcb->command_id = tag;	/* [한국어] TCB 의 CID = 태그 */

	if (nvme_is_write(cmd))	/* [한국어] 쓰기면 호스트→디바이스 DMA */
		tcb->dma_flags = APPLE_ANS_TCB_DMA_TO_DEVICE;	/* [한국어] TO_DEVICE 방향 비트 */
	else	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		tcb->dma_flags = APPLE_ANS_TCB_DMA_FROM_DEVICE;	/* [한국어] FROM_DEVICE(읽기) */

	memcpy(&q->sqes[tag], cmd, sizeof(*cmd));	/* [한국어] 태그 인덱스 슬롯에 SQE 배치 */

	/*
	 * This lock here doesn't make much sense at a first glance but
	 * removing it will result in occasional missed completion
	 * interrupts even though the commands still appear on the CQ.
	 * It's unclear why this happens but our best guess is that
	 * there is a bug in the firmware triggered when a new command
	 * is issued while we're inside the irq handler between the
	 * NVMMU invalidation (and making the tag available again)
	 * and the final CQ update.
	 */
	/* [한국어] 제출 도어벨을 IRQ 핸들러(TCB inval~CQ 갱신)와 직렬화 — 미완료 인터럽트 버그 회피 */
	spin_lock_irq(&anv->lock);	/* [한국어] 공유 lock — submit 과 handle_cq 직렬화 */
	writel(tag, q->sq_db);	/* [한국어] 도어벨에 태그 기록 = 실행 트리거 */
	spin_unlock_irq(&anv->lock);	/* [한국어] 임계구역 종료 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * From pci.c:
 * Will slightly overestimate the number of pages needed.  This is OK
 * as it only leads to a small amount of wasted memory for the lifetime of
 * the I/O.
 */
/*
 * [한국어]
 * apple_nvme_iod_alloc_size - iod 부가 sg+PRP 포인터 배열 바이트 수
 *
 * 단일 페이지 이하로 제한해 GFP_ATOMIC mempool 안전성 확보.
 */
static inline size_t apple_nvme_iod_alloc_size(void)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	const unsigned int nprps = DIV_ROUND_UP(	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		NVME_MAX_KB_SZ + NVME_CTRL_PAGE_SIZE, NVME_CTRL_PAGE_SIZE);	/* [한국어] 최대 페이로드 기준 PRP 개수 상한 */
	const int npages = DIV_ROUND_UP(8 * nprps, PAGE_SIZE - 8);	/* [한국어] PRP list 페이지 체인 길이 */
	const size_t alloc_size = sizeof(__le64 *) * npages +	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
				  sizeof(struct scatterlist) * NVME_MAX_SEGS;	/* [한국어] 포인터 배열 + sg[] 합산 */

	return alloc_size;	/* [한국어] mempool 오브젝트 크기 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_iod_list - iod 내부 PRP 페이지 포인터 배열 접근
 *
 * sg 배열 직후에 void* 페이지 포인터를 배치한 레이아웃.
 */
static void **apple_nvme_iod_list(struct request *req)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] 요청 PDU = iod */

	return (void **)(iod->sg + blk_rq_nr_phys_segments(req));	/* [한국어] sg[] 끝 다음이 PRP 페이지 포인터 배열 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_free_prps - PRP 리스트 페이지를 page pool 로 반환
 *
 * 체인: 각 페이지 마지막 엔트리가 다음 DMA. 호출 체인: unmap_data → [여기]
 */
static void apple_nvme_free_prps(struct apple_nvme *anv, struct request *req)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	const int last_prp = NVME_CTRL_PAGE_SIZE / sizeof(__le64) - 1;	/* [한국어] 페이지 마지막 슬롯 = next 링크 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] iod 에서 first_dma/npages */
	dma_addr_t dma_addr = iod->first_dma;	/* [한국어] 현재 해제할 PRP 페이지 DMA */
	int i;	/* [한국어] 페이지 인덱스 */

	for (i = 0; i < iod->npages; i++) {	/* [한국어] 체인된 PRP list 페이지 순회 */
		__le64 *prp_list = apple_nvme_iod_list(req)[i];	/* [한국어] i 번째 list VA */
		dma_addr_t next_dma_addr = le64_to_cpu(prp_list[last_prp]);	/* [한국어] 다음 페이지 DMA(LE) */

		dma_pool_free(anv->prp_page_pool, prp_list, dma_addr);	/* [한국어] 현재 페이지를 풀에 반환 */
		dma_addr = next_dma_addr;	/* [한국어] 체인 전진 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_unmap_data - sg/page DMA unmap + PRP free + mempool free
 *
 * dma_len>0 이면 단순 page 맵 경로. 아니면 sg 경로와 small/page pool 분기.
 * 호출 체인: complete_rq/unmap_rq → [여기]
 */
static void apple_nvme_unmap_data(struct apple_nvme *anv, struct request *req)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] 매핑 상태 보유 iod */

	if (iod->dma_len) {	/* [한국어] 단순 단일 세그먼트 맵 경로 */
		dma_unmap_page(anv->dev, iod->first_dma, iod->dma_len,	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
			       rq_dma_dir(req));	/* [한국어] page DMA 언맵 */
		return;	/* [한국어] sg/mempool 없음 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	WARN_ON_ONCE(!iod->nents);	/* [한국어] sg 경로인데 nents=0 이면 버그 */

	dma_unmap_sg(anv->dev, iod->sg, iod->nents, rq_dma_dir(req));	/* [한국어] scatterlist DMA 언맵 */
	if (iod->npages == 0)	/* [한국어] small pool 단일 PRP list */
		dma_pool_free(anv->prp_small_pool, apple_nvme_iod_list(req)[0],	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
			      iod->first_dma);	/* [한국어] 256B 풀 반환 */
	else	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		apple_nvme_free_prps(anv, req);	/* [한국어] 다중 페이지 PRP 체인 해제 */
	mempool_free(iod->sg, anv->iod_mempool);	/* [한국어] sg+포인터 배열 mempool 반환 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static void apple_nvme_print_sgl(struct scatterlist *sgl, int nents)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	int i;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct scatterlist *sg;	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */

	for_each_sg(sgl, sg, nents, i) {	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		dma_addr_t phys = sg_phys(sg);	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */

		pr_warn("sg[%d] phys_addr:%pad offset:%d length:%d dma_address:%pad dma_length:%d\n",	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
			i, &phys, sg->offset, sg->length, &sg_dma_address(sg),	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
			sg_dma_len(sg));	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_setup_prps - DMA 매핑된 sg 로 PRP1/PRP2/PRP list 구성
 *
 * pci.c 와 유사. small(256B) vs page(4K) 풀 선택. 핫패스 맵 경로.
 * 호출 체인: map_data → [여기]
 */
static blk_status_t apple_nvme_setup_prps(struct apple_nvme *anv,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
					  struct request *req,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
					  struct nvme_rw_command *cmnd)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] 매핑 상태 기록 대상 */
	struct dma_pool *pool;	/* [한국어] small 또는 page PRP 풀 */
	int length = blk_rq_payload_bytes(req);	/* [한국어] 남은 페이로드 바이트 */
	struct scatterlist *sg = iod->sg;	/* [한국어] 현재 sg 커서 */
	int dma_len = sg_dma_len(sg);	/* [한국어] 현재 세그먼트 DMA 길이 */
	u64 dma_addr = sg_dma_address(sg);	/* [한국어] 현재 세그먼트 DMA 주소 */
	int offset = dma_addr & (NVME_CTRL_PAGE_SIZE - 1);	/* [한국어] 페이지 내 오프셋 */
	__le64 *prp_list;	/* [한국어] 현재 PRP list 페이지 VA */
	void **list = apple_nvme_iod_list(req);	/* [한국어] 페이지 포인터 배열 */
	dma_addr_t prp_dma;	/* [한국어] PRP list 페이지 DMA */
	int nprps, i;	/* [한국어] PRP 개수·페이지 내 인덱스 */

	length -= (NVME_CTRL_PAGE_SIZE - offset);	/* [한국어] PRP1 이 담는 첫 부분 제외 */
	if (length <= 0) {	/* [한국어] 단일 페이지로 충분 — PRP2 불필요 */
		iod->first_dma = 0;	/* [한국어] PRP2=0 */
		goto done;	/* [한국어] PRP1 만 기록 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	dma_len -= (NVME_CTRL_PAGE_SIZE - offset);	/* [한국어] 첫 세그먼트 잔여 */
	if (dma_len) {	/* [한국어] 같은 세그먼트에 다음 페이지 있음 */
		dma_addr += (NVME_CTRL_PAGE_SIZE - offset);	/* [한국어] 다음 페이지 경계로 */
	} else {	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		sg = sg_next(sg);	/* [한국어] 다음 물리 세그먼트 */
		dma_addr = sg_dma_address(sg);	/* [한국어] 새 DMA 주소 */
		dma_len = sg_dma_len(sg);	/* [한국어] 새 길이 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (length <= NVME_CTRL_PAGE_SIZE) {	/* [한국어] PRP2 단일 페이지로 충분 */
		iod->first_dma = dma_addr;	/* [한국어] PRP2 = 두 번째 페이지 */
		goto done;	/* [한국어] Apple-ANS: 공통 정리 라벨로 점프 — 부분 성공 롤백 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	nprps = DIV_ROUND_UP(length, NVME_CTRL_PAGE_SIZE);	/* [한국어] 리스트에 넣을 PRP 수 */
	if (nprps <= (256 / 8)) {	/* [한국어] 32 엔트리 이하면 small pool */
		pool = anv->prp_small_pool;	/* [한국어] 256B 풀 */
		iod->npages = 0;	/* [한국어] 0 = small pool 표식 */
	} else {	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		pool = anv->prp_page_pool;	/* [한국어] 4K 풀 */
		iod->npages = 1;	/* [한국어] 최소 1 페이지 체인 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	prp_list = dma_pool_alloc(pool, GFP_ATOMIC, &prp_dma);	/* [한국어] 핫패스 atomic 할당 */
	if (!prp_list) {	/* [한국어] 풀 고갈 */
		iod->first_dma = dma_addr;	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
		iod->npages = -1;	/* [한국어] 실패 표식 */
		return BLK_STS_RESOURCE;	/* [한국어] 자원 부족 — 재시도 가능 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	list[0] = prp_list;	/* [한국어] 첫 list 페이지 VA 보관 */
	iod->first_dma = prp_dma;	/* [한국어] PRP2 = list DMA */
	i = 0;	/* [한국어] list 내 슬롯 인덱스 */
	for (;;) {	/* [한국어] 잔여 길이 소진까지 PRP 채움 */
		if (i == NVME_CTRL_PAGE_SIZE >> 3) {	/* [한국어] 페이지 가득 — 체인 확장 */
			__le64 *old_prp_list = prp_list;	/* [한국어] 이전 페이지 */

			prp_list = dma_pool_alloc(pool, GFP_ATOMIC, &prp_dma);	/* [한국어] 다음 list 페이지 */
			if (!prp_list)	/* [한국어] 할당 실패 → 부분 해제 */
				goto free_prps;	/* [한국어] 언와인드 */
			list[iod->npages++] = prp_list;	/* [한국어] 새 페이지 등록 */
			prp_list[0] = old_prp_list[i - 1];	/* [한국어] 마지막 데이터 엔트리 이동 */
			old_prp_list[i - 1] = cpu_to_le64(prp_dma);	/* [한국어] 이전 끝 = next 링크 */
			i = 1;	/* [한국어] 새 페이지는 슬롯 1 부터 */
		}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		prp_list[i++] = cpu_to_le64(dma_addr);	/* [한국어] 현재 페이지 DMA 를 LE64 로 기록 */
		dma_len -= NVME_CTRL_PAGE_SIZE;	/* [한국어] 세그먼트 잔여 감소 */
		dma_addr += NVME_CTRL_PAGE_SIZE;	/* [한국어] 다음 페이지 주소 */
		length -= NVME_CTRL_PAGE_SIZE;	/* [한국어] 전체 잔여 감소 */
		if (length <= 0)	/* [한국어] 페이로드 끝 */
			break;	/* [한국어] 루프 종료 */
		if (dma_len > 0)	/* [한국어] 같은 세그먼트 계속 */
			continue;	/* [한국어] sg 전진 없이 다음 페이지 */
		if (unlikely(dma_len < 0))	/* [한국어] 세그먼트/길이 불일치 */
			goto bad_sgl;	/* [한국어] 잘못된 SGL */
		sg = sg_next(sg);	/* [한국어] 다음 세그먼트 */
		dma_addr = sg_dma_address(sg);	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
		dma_len = sg_dma_len(sg);	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
done:	/* [한국어] SQE dptr 기록 공통 출구 */
	cmnd->dptr.prp1 = cpu_to_le64(sg_dma_address(iod->sg));	/* [한국어] PRP1 = 첫 데이터 페이지 */
	cmnd->dptr.prp2 = cpu_to_le64(iod->first_dma);	/* [한국어] PRP2 = 두 번째 또는 list */
	return BLK_STS_OK;	/* [한국어] 맵 성공 */
free_prps:	/* [한국어] list 할당 실패 언와인드 */
	apple_nvme_free_prps(anv, req);	/* [한국어] 이미 할당된 list 페이지 반환 */
	return BLK_STS_RESOURCE;	/* [한국어] 자원 부족 */
bad_sgl:	/* [한국어] SGL 무결성 오류 */
	WARN(DO_ONCE(apple_nvme_print_sgl, iod->sg, iod->nents),	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	     "Invalid SGL for payload:%d nents:%d\n", blk_rq_payload_bytes(req),	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	     iod->nents);	/* [한국어] 1회 덤프 후 IOERR */
	return BLK_STS_IOERR;	/* [한국어] 재시도 무의미한 맵 오류 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_setup_prp_simple - 단일/이중 페이지 단순 PRP (리스트 없음)
 *
 * 1 phys segment 이고 2 페이지 이하면 dma_map_bvec 만으로 충분.
 */
static blk_status_t apple_nvme_setup_prp_simple(struct apple_nvme *anv,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
						struct request *req,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
						struct nvme_rw_command *cmnd,	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
						struct bio_vec *bv)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] iod 에 단일 맵 상태 기록 */
	unsigned int offset = bv->bv_offset & (NVME_CTRL_PAGE_SIZE - 1);	/* [한국어] 페이지 정렬 오프셋 */
	unsigned int first_prp_len = NVME_CTRL_PAGE_SIZE - offset;	/* [한국어] PRP1 이 담는 바이트 */

	iod->first_dma = dma_map_bvec(anv->dev, bv, rq_dma_dir(req), 0);	/* [한국어] bvec 단일 DMA 맵 */
	if (dma_mapping_error(anv->dev, iod->first_dma))	/* [한국어] 맵 실패 */
		return BLK_STS_RESOURCE;	/* [한국어] 자원/IOMMU 실패 */
	iod->dma_len = bv->bv_len;	/* [한국어] unmap_page 경로 표시 */

	cmnd->dptr.prp1 = cpu_to_le64(iod->first_dma);	/* [한국어] PRP1 */
	if (bv->bv_len > first_prp_len)	/* [한국어] 두 페이지에 걸치면 PRP2 */
		cmnd->dptr.prp2 = cpu_to_le64(iod->first_dma + first_prp_len);	/* [한국어] 연속 두 번째 페이지 */
	return BLK_STS_OK;	/* [한국어] 단순 맵 성공 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_map_data - dma_map_sg 후 PRP 설정 (핫패스)
 *
 * 1세그먼트 소형은 simple, 아니면 mempool sg + setup_prps.
 * 실패 시 unmap/mempool free. 호출 체인: queue_rq → [여기]
 */
static blk_status_t apple_nvme_map_data(struct apple_nvme *anv,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
					struct request *req,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
					struct nvme_command *cmnd)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] 요청 iod */
	blk_status_t ret = BLK_STS_RESOURCE;	/* [한국어] 기본 실패 가정 */
	int nr_mapped;	/* [한국어] dma_map_sg 결과 엔트리 수 */

	if (blk_rq_nr_phys_segments(req) == 1) {	/* [한국어] 단일 물리 세그먼트 고속 경로 */
		struct bio_vec bv = req_bvec(req);	/* [한국어] 첫 bvec */

		if (bv.bv_offset + bv.bv_len <= NVME_CTRL_PAGE_SIZE * 2)	/* [한국어] 최대 2 페이지 */
			return apple_nvme_setup_prp_simple(anv, req, &cmnd->rw,	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
							   &bv);	/* [한국어] list 없이 완료 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	iod->dma_len = 0;	/* [한국어] sg 경로 (page unmap 아님) */
	iod->sg = mempool_alloc(anv->iod_mempool, GFP_ATOMIC);	/* [한국어] 핫패스 sg 버퍼 */
	if (!iod->sg)	/* [한국어] 풀 고갈 */
		return BLK_STS_RESOURCE;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
	sg_init_table(iod->sg, blk_rq_nr_phys_segments(req));	/* [한국어] sg 테이블 초기화 */
	iod->nents = blk_rq_map_sg(req, iod->sg);	/* [한국어] bio → sg */
	if (!iod->nents)	/* [한국어] 맵 실패 */
		goto out_free_sg;	/* [한국어] mempool 반환 */

	nr_mapped = dma_map_sg_attrs(anv->dev, iod->sg, iod->nents,	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
				     rq_dma_dir(req), DMA_ATTR_NO_WARN);	/* [한국어] 장치 DMA 맵 */
	if (!nr_mapped)	/* [한국어] IOMMU/맵 실패 */
		goto out_free_sg;	/* [한국어] Apple-ANS: 공통 정리 라벨로 점프 — 부분 성공 롤백 */

	ret = apple_nvme_setup_prps(anv, req, &cmnd->rw);	/* [한국어] PRP 리스트 구성 */
	if (ret != BLK_STS_OK)	/* [한국어] PRP 할당 실패 */
		goto out_unmap_sg;	/* [한국어] DMA unmap 후 free */
	return BLK_STS_OK;	/* [한국어] 맵 완료 — 제출 가능 */

out_unmap_sg:	/* [한국어] PRP 실패 후 DMA 롤백 */
	dma_unmap_sg(anv->dev, iod->sg, iod->nents, rq_dma_dir(req));	/* [한국어] sg DMA 언맵 */
out_free_sg:	/* [한국어] sg 버퍼 반환 */
	mempool_free(iod->sg, anv->iod_mempool);	/* [한국어] mempool 반환 */
	return ret;	/* [한국어] 에러 상태 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_unmap_rq - 완료 시 페이로드 unmap 래퍼
 */
static __always_inline void apple_nvme_unmap_rq(struct request *req)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] iod → 큐/맵 상태 */
	struct apple_nvme *anv = queue_to_apple_nvme(iod->q);	/* [한국어] 장치 */

	if (blk_rq_nr_phys_segments(req))	/* [한국어] 페이로드 있었던 요청만 */
		apple_nvme_unmap_data(anv, req);	/* [한국어] DMA/PRP/mempool 해제 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_complete_rq - unmap 후 nvme_complete_rq
 */
static void apple_nvme_complete_rq(struct request *req)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	apple_nvme_unmap_rq(req);	/* [한국어] 완료 전 DMA 자원 해제 */
	nvme_complete_rq(req);	/* [한국어] core 완료(재시도/errno) */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_complete_batch - 배치 완료 콜백
 */
static void apple_nvme_complete_batch(struct io_comp_batch *iob)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nvme_complete_batch(iob, apple_nvme_unmap_rq);	/* [한국어] 각 rq unmap 후 배치 complete */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_cqe_pending - CQ phase 비트로 신규 CQE 존재 여부
 */
static inline bool apple_nvme_cqe_pending(struct apple_nvme_queue *q)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct nvme_completion *hcqe = &q->cqes[q->cq_head];	/* [한국어] head 슬롯 */

	return (le16_to_cpu(READ_ONCE(hcqe->status)) & 1) == q->cq_phase;	/* [한국어] phase 일치=신규 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_queue_tagset - admin/io 큐에 따른 tags 선택
 */
static inline struct blk_mq_tags *	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
apple_nvme_queue_tagset(struct apple_nvme *anv, struct apple_nvme_queue *q)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (q->is_adminq)	/* [한국어] admin 태그 공간 */
		return anv->admin_tagset.tags[0];	/* [한국어] admin tags */
	else	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		return anv->tagset.tags[0];	/* [한국어] I/O tags (공유 공간 예약 포함) */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_handle_cqe - CQE 한 건 처리: TCB inval → 태그로 rq → complete
 *
 * 핫패스. 배치 가능하면 iob 에 넣고, 아니면 즉시 unmap+complete.
 * 호출 체인: poll_cq → [여기]
 */
static inline void apple_nvme_handle_cqe(struct apple_nvme_queue *q,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
					 struct io_comp_batch *iob, u16 idx)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] 장치 — tagset/inval */
	struct nvme_completion *cqe = &q->cqes[idx];	/* [한국어] 완료 링 슬롯 */
	__u16 command_id = READ_ONCE(cqe->command_id);	/* [한국어] 완료된 태그/CID */
	struct request *req;	/* [한국어] 매칭 blk-mq 요청 */

	if (anv->hw->has_lsq_nvmmu)	/* [한국어] 신형: 완료 즉시 TCB 슬롯 무효화 */
		apple_nvmmu_inval(q, command_id);	/* [한국어] 태그 재사용 전 NVMMU 클리어 */

	req = nvme_find_rq(apple_nvme_queue_tagset(anv, q), command_id);	/* [한국어] 태그로 진행 중 요청 검색 */
	if (unlikely(!req)) {	/* [한국어] 유령 완료 — 드라이버/펌웨어 불일치 */
		dev_warn(anv->dev, "invalid id %d completed", command_id);	/* [한국어] 잘못된 CID 경고 */
		return;	/* [한국어] 무시하고 다음 CQE */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (!nvme_try_complete_req(req, cqe->status, cqe->result) &&	/* [한국어] core 가 status/result 반영, 즉시 완료 시도 */
	    !blk_mq_add_to_batch(req, iob,	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
				 nvme_req(req)->status != NVME_SC_SUCCESS,	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
				 apple_nvme_complete_batch))	/* [한국어] 배치 가능 시 iob 적재(에러는 배치 제외 경향) */
		apple_nvme_complete_rq(req);	/* [한국어] 배치 실패/비배치 → 즉시 unmap+complete */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static inline void apple_nvme_update_cq_head(struct apple_nvme_queue *q)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	u32 tmp = q->cq_head + 1;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (tmp == apple_nvme_queue_depth(q)) {	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		q->cq_head = 0;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		q->cq_phase ^= 1;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	} else {	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		q->cq_head = tmp;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static bool apple_nvme_poll_cq(struct apple_nvme_queue *q,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
			       struct io_comp_batch *iob)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	bool found = false;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	while (apple_nvme_cqe_pending(q)) {	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		found = true;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

		/*
		 * load-load control dependency between phase and the rest of
		 * the cqe requires a full read memory barrier
		 */
		dma_rmb();	/* [한국어] Apple-ANS: DMA 매핑 — 장치 접근 가능 주소 */
		apple_nvme_handle_cqe(q, iob, q->cq_head);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		apple_nvme_update_cq_head(q);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (found)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		writel(q->cq_head, q->cq_db);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return found;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static bool apple_nvme_handle_cq(struct apple_nvme_queue *q, bool force)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	bool found;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	DEFINE_IO_COMP_BATCH(iob);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (!READ_ONCE(q->enabled) && !force)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		return false;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */

	found = apple_nvme_poll_cq(q, &iob);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (!rq_list_empty(&iob.req_list))	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		apple_nvme_complete_batch(&iob);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return found;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static irqreturn_t apple_nvme_irq(int irq, void *data)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = data;	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	bool handled = false;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	unsigned long flags;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	spin_lock_irqsave(&anv->lock, flags);	/* [한국어] Apple-ANS: 동기화 — 큐/연결/상태 보호 */
	if (apple_nvme_handle_cq(&anv->ioq, false))	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		handled = true;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (apple_nvme_handle_cq(&anv->adminq, false))	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		handled = true;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	spin_unlock_irqrestore(&anv->lock, flags);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (handled)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		return IRQ_HANDLED;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
	return IRQ_NONE;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_create_cq(struct apple_nvme *anv)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct nvme_command c = {};	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */

	/*
	 * Note: we (ab)use the fact that the prp fields survive if no data
	 * is attached to the request.
	 */
	c.create_cq.opcode = nvme_admin_create_cq;	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	c.create_cq.prp1 = cpu_to_le64(anv->ioq.cq_dma_addr);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_cq.cqid = cpu_to_le16(1);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_cq.qsize = cpu_to_le16(anv->hw->max_queue_depth - 1);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_cq.cq_flags = cpu_to_le16(NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_cq.irq_vector = cpu_to_le16(0);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return nvme_submit_sync_cmd(anv->ctrl.admin_q, &c, NULL, 0);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_remove_cq(struct apple_nvme *anv)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct nvme_command c = {};	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */

	c.delete_queue.opcode = nvme_admin_delete_cq;	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	c.delete_queue.qid = cpu_to_le16(1);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return nvme_submit_sync_cmd(anv->ctrl.admin_q, &c, NULL, 0);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_create_sq(struct apple_nvme *anv)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct nvme_command c = {};	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */

	/*
	 * Note: we (ab)use the fact that the prp fields survive if no data
	 * is attached to the request.
	 */
	c.create_sq.opcode = nvme_admin_create_sq;	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	c.create_sq.prp1 = cpu_to_le64(anv->ioq.sq_dma_addr);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_sq.sqid = cpu_to_le16(1);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_sq.qsize = cpu_to_le16(anv->hw->max_queue_depth - 1);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_sq.sq_flags = cpu_to_le16(NVME_QUEUE_PHYS_CONTIG);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	c.create_sq.cqid = cpu_to_le16(1);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return nvme_submit_sync_cmd(anv->ctrl.admin_q, &c, NULL, 0);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_remove_sq(struct apple_nvme *anv)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct nvme_command c = {};	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */

	c.delete_queue.opcode = nvme_admin_delete_sq;	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	c.delete_queue.qid = cpu_to_le16(1);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return nvme_submit_sync_cmd(anv->ctrl.admin_q, &c, NULL, 0);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_queue_rq - blk-mq 핫패스 제출 진입점
 *
 * enabled/ready 검사 → setup_cmd → map_data(PRP) → start_request
 * → SoC별 submit(TCB+도어벨). 실패 시 cleanup_cmd.
 * 호출 체인: blk_mq → [여기] → submit_cmd_*
 */
static blk_status_t apple_nvme_queue_rq(struct blk_mq_hw_ctx *hctx,	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
					const struct blk_mq_queue_data *bd)	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct nvme_ns *ns = hctx->queue->queuedata;	/* [한국어] 네임스페이스(admin 은 NULL 가능) */
	struct apple_nvme_queue *q = hctx->driver_data;	/* [한국어] hctx 에 바인딩된 admin/io 큐 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] 장치 전역 */
	struct request *req = bd->rq;	/* [한국어] 제출 대상 blk-mq 요청 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] 요청 private iod */
	struct nvme_command *cmnd = &iod->cmd;	/* [한국어] 제출용 SQE 슬롯 */
	blk_status_t ret;	/* [한국어] 맵/setup 결과 */

	iod->npages = -1;	/* [한국어] PRP 미구성 표식 */
	iod->nents = 0;	/* [한국어] sg 미맵 */

	/*
	 * We should not need to do this, but we're still using this to
	 * ensure we can drain requests on a dying queue.
	 */
	if (unlikely(!READ_ONCE(q->enabled)))	/* [한국어] disable 중 유입 요청 거부 — 드레인 안전 */
		return BLK_STS_IOERR;	/* [한국어] I/O 오류로 상위에 실패 전달 */

	if (!nvme_check_ready(&anv->ctrl, req, true))	/* [한국어] 컨트롤러 LIVE/허용 상태 검사 */
		return nvme_fail_nonready_command(&anv->ctrl, req);	/* [한국어] 비준비 시 정책적 실패/재시도 */

	ret = nvme_setup_cmd(ns, req);	/* [한국어] core 가 opcode/NSID/CDW 공통 필드 채움 */
	if (ret)	/* [한국어] setup 실패(한계/권한 등) */
		return ret;	/* [한국어] 맵 없이 즉시 반환 */

	if (blk_rq_nr_phys_segments(req)) {	/* [한국어] 페이로드 있으면 DMA/PRP 필요 */
		ret = apple_nvme_map_data(anv, req, cmnd);	/* [한국어] sg map + PRP1/2 기록 */
		if (ret)	/* [한국어] 맵 자원 부족/SGL 오류 */
			goto out_free_cmd;	/* [한국어] setup_cmd 롤백 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	nvme_start_request(req);	/* [한국어] 타임아웃 시계 시작 — 제출 직전 필수 */

	if (anv->hw->has_lsq_nvmmu)	/* [한국어] 신형: TCB+태그 도어벨 */
		apple_nvme_submit_cmd_t8103(q, cmnd);	/* [한국어] NVMMU 경로 제출 */
	else	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		apple_nvme_submit_cmd_t8015(q, cmnd);	/* [한국어] 구형 링 tail 제출 */

	return BLK_STS_OK;	/* [한국어] 비동기 완료 대기 — 성공 큐잉 */

out_free_cmd:	/* [한국어] 맵 실패 언와인드 */
	nvme_cleanup_cmd(req);	/* [한국어] setup_cmd 부가 상태 정리 */
	return ret;	/* [한국어] 맵 에러 상태 그대로 전파 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
				unsigned int hctx_idx)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	hctx->driver_data = data;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	return 0;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_init_request(struct blk_mq_tag_set *set,	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
				   struct request *req, unsigned int hctx_idx,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
				   unsigned int numa_node)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_queue *q = set->driver_data;	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
	struct nvme_request *nreq = nvme_req(req);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */

	iod->q = q;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nreq->ctrl = &anv->ctrl;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nreq->cmd = &iod->cmd;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return 0;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_disable - CC.EN 클리어·큐 비활성·잔여 요청 실패 완료
 *
 * @anv: 장치
 * @shutdown: true 면 안전 종료(동결 대기+disable shutdown), false 면 리셋 경로
 *
 * 죽은 컨트롤러(CFS/!RDY/rtkit crash)면 Delete SQ/CQ 생략.
 * enabled 클리어 후 CQ 강제 drain, cancel tagset.
 * 호출 체인: remove/reset/suspend → [여기]
 */
static void apple_nvme_disable(struct apple_nvme *anv, bool shutdown)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	enum nvme_ctrl_state state = nvme_ctrl_state(&anv->ctrl);	/* [한국어] 현재 상태기계 */
	u32 csts = readl(anv->mmio_nvme + NVME_REG_CSTS);	/* [한국어] CSTS 스냅샷 */
	bool dead = false, freeze = false;	/* [한국어] dead=HW 응답 불가, freeze=동결 필요 */
	unsigned long flags;	/* [한국어] IRQ 저장 플래그 */

	if (apple_rtkit_is_crashed(anv->rtk))	/* [한국어] 펌웨어 크래시 = 복구 불가 dead */
		dead = true;	/* [한국어] admin 경로 스킵 */
	if (!(csts & NVME_CSTS_RDY))	/* [한국어] 미준비 */
		dead = true;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (csts & NVME_CSTS_CFS)	/* [한국어] Controller Fatal Status */
		dead = true;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (state == NVME_CTRL_LIVE ||	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
	    state == NVME_CTRL_RESETTING) {	/* [한국어] 서비스/리셋 중이면 동결 */
		freeze = true;	/* [한국어] freeze 경로 활성 */
		nvme_start_freeze(&anv->ctrl);	/* [한국어] 신규 I/O 차단 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	/*
	 * Give the controller a chance to complete all entered requests if
	 * doing a safe shutdown.
	 */
	if (!dead && shutdown && freeze)	/* [한국어] 안전 종료: 잔여 I/O 대기 */
		nvme_wait_freeze_timeout(&anv->ctrl, NVME_IO_TIMEOUT);	/* [한국어] 타임아웃 내 동결 완료 대기 */

	nvme_quiesce_io_queues(&anv->ctrl);	/* [한국어] I/O 큐 제출 정지 */

	if (!dead) {	/* [한국어] 응답 가능 시 정상 Delete/Disable */
		if (READ_ONCE(anv->ioq.enabled)) {	/* [한국어] I/O 큐가 살아 있으면 */
			apple_nvme_remove_sq(anv);	/* [한국어] Delete SQ */
			apple_nvme_remove_cq(anv);	/* [한국어] Delete CQ */
		}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

		/*
		 * Always disable the NVMe controller after shutdown.
		 * We need to do this to bring it back up later anyway, and we
		 * can't do it while the firmware is not running (e.g. in the
		 * resume reset path before RTKit is initialized), so for Apple
		 * controllers it makes sense to unconditionally do it here.
		 * Additionally, this sequence of events is reliable, while
		 * others (like disabling after bringing back the firmware on
		 * resume) seem to run into trouble under some circumstances.
		 *
		 * Both U-Boot and m1n1 also use this convention (i.e. an ANS
		 * NVMe controller is handed off with firmware shut down, in an
		 * NVMe disabled state, after a clean shutdown).
		 */
		/* [한국어] Apple: shutdown 후 무조건 CC.EN=0 — U-Boot/m1n1 과 동일 핸드오프 */
		if (shutdown)	/* [한국어] 정상 종료 시 shutdown 비트 포함 disable */
			nvme_disable_ctrl(&anv->ctrl, shutdown);	/* [한국어] CC.SHN 경로 */
		nvme_disable_ctrl(&anv->ctrl, false);	/* [한국어] EN 클리어 (재기동 준비) */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	WRITE_ONCE(anv->ioq.enabled, false);	/* [한국어] I/O 제출 거부 */
	WRITE_ONCE(anv->adminq.enabled, false);	/* [한국어] admin 제출 거부 */
	mb(); /* ensure that nvme_queue_rq() sees that enabled is cleared */	/* [한국어] queue_rq 가 enabled=0 을 보도록 배리어 */
	nvme_quiesce_admin_queue(&anv->ctrl);	/* [한국어] admin 큐 정지 */

	/* last chance to complete any requests before nvme_cancel_request */
	/* [한국어] cancel 전 마지막 CQ 드레인 — force 로 disabled 큐도 소비 */
	spin_lock_irqsave(&anv->lock, flags);	/* [한국어] 제출/IRQ 와 직렬화 */
	apple_nvme_handle_cq(&anv->ioq, true);	/* [한국어] I/O CQ 강제 드레인 */
	apple_nvme_handle_cq(&anv->adminq, true);	/* [한국어] admin CQ 강제 드레인 */
	spin_unlock_irqrestore(&anv->lock, flags);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	nvme_cancel_tagset(&anv->ctrl);	/* [한국어] 잔여 I/O 요청 취소 완료 */
	nvme_cancel_admin_tagset(&anv->ctrl);	/* [한국어] 잔여 admin 요청 취소 */

	/*
	 * The driver will not be starting up queues again if shutting down so
	 * must flush all entered requests to their failed completion to avoid
	 * deadlocking blk-mq hot-cpu notifier.
	 */
	if (shutdown) {	/* [한국어] 셧다운: unquiesce 로 실패한 완료를 흘려보냄 */
		nvme_unquiesce_io_queues(&anv->ctrl);	/* [한국어] I/O unquiesce — 데드락 방지 */
		nvme_unquiesce_admin_queue(&anv->ctrl);	/* [한국어] admin unquiesce */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_timeout - 응답 없는 요청을 처리한다. 이 하드웨어에는 Abort 가 없다
 *
 * @req: 시한을 넘긴 요청
 * @return: 항상 BLK_EH_DONE — 어느 경로로 가든 요청은 여기서 끝난다.
 *
 * 세 갈래다.
 *
 * 첫째, 컨트롤러가 LIVE 가 아니면 즉시 완료시킨다. 위 영어 주석이
 * rdma.c 에서 가져온 근거를 그대로 옮겨 두었다 -- 이 요청들은 컨트롤러를
 * 내리거나 세우는 절차의 일부이므로, 복구가 정리해 주기를 기다리면 그
 * 복구가 이 요청 때문에 막히는 교착이 된다.
 *
 * 둘째, 살아 있다면 인터럽트를 놓쳤을 가능성을 먼저 본다. 완료 큐를 직접
 * 훑어 보고 실제로 완료돼 있으면 그것으로 끝이다. 하나뿐인 SoC 인터럽트를
 * 공유하는 구조라 이 경우가 실제로 일어난다. 이 확인이 없으면 놓친
 * 인터럽트 하나가 컨트롤러 전체 리셋으로 이어진다.
 *
 * 셋째, 그래도 안 끝났으면 리셋뿐이다. 위 영어 주석대로 이 하드웨어는
 * Abort 명령을 지원하지 않아 개별 요청을 취소할 방법이 없다. PCIe NVMe 가
 * Abort 를 먼저 시도하고 FC 가 ABTS 를 보내는 것과 대비된다.
 *
 * CSTS.CFS(Controller Fatal Status)를 함께 보는 이유: 이미 치명적 오류
 * 상태라면 완료 큐를 훑어도 나올 것이 없으므로 곧바로 리셋으로 간다.
 *
 * 실행 컨텍스트: blk-mq 타임아웃 문맥. spin_lock_irqsave 로 완료 경로와
 * 배타적으로 큐를 훑는다.
 *
 * 호출 체인:
 *   blk-mq 타임아웃 → [이 함수] → apple_nvme_handle_cq 또는 nvme_reset_ctrl
 */
static enum blk_eh_timer_return apple_nvme_timeout(struct request *req)
{
	struct apple_nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct apple_nvme_queue *q = iod->q;
	struct apple_nvme *anv = queue_to_apple_nvme(q);
	unsigned long flags;
	u32 csts = readl(anv->mmio_nvme + NVME_REG_CSTS);	/* [한국어] 컨트롤러가 치명적 오류 상태인지 미리 읽어 둔다 */

	if (nvme_ctrl_state(&anv->ctrl) != NVME_CTRL_LIVE) {
		/*
		 * From rdma.c:
		 * If we are resetting, connecting or deleting we should
		 * complete immediately because we may block controller
		 * teardown or setup sequence
		 * - ctrl disable/shutdown fabrics requests
		 * - connect requests
		 * - initialization admin requests
		 * - I/O requests that entered after unquiescing and
		 *   the controller stopped responding
		 *
		 * All other requests should be cancelled by the error
		 * recovery work, so it's fine that we fail it here.
		 */
		/* [한국어] 위 영어 주석은 rdma.c 에서 옮겨 온 것이다. 요지는 이
		 * 요청들이 teardown/setup 절차의 일부라, 복구를 기다리면 그 복구가
		 * 이 요청 때문에 막히는 교착이 된다는 것이다. */
		dev_warn(anv->dev,
			 "I/O %d(aq:%d) timeout while not in live state\n",
			 req->tag, q->is_adminq);
		if (blk_mq_request_started(req) &&	/* [한국어] 시작됐고 아직 안 끝난 것만 */
		    !blk_mq_request_completed(req)) {	/* [한국어] 그 사이 완료됐을 수 있어 두 번 완료시키지 않도록 확인한다 */
			nvme_req(req)->status = NVME_SC_HOST_ABORTED_CMD;
			nvme_req(req)->flags |= NVME_REQ_CANCELLED;	/* [한국어] 재시도가 아니라 취소임을 알린다 */
			blk_mq_complete_request(req);
		}
		return BLK_EH_DONE;
	}

	/* check if we just missed an interrupt if we're still alive */
	/* [한국어] 위 영어 주석대로 인터럽트를 놓쳤을 가능성을 먼저 본다.
	 * 하나뿐인 SoC 인터럽트를 두 큐가 공유하는 구조라 실제로 일어나며,
	 * 이 확인이 없으면 놓친 인터럽트 하나가 컨트롤러 전체 리셋이 된다. */
	if (!apple_rtkit_is_crashed(anv->rtk) && !(csts & NVME_CSTS_CFS)) {	/* [한국어] 이미 치명적 오류면 훑어도 나올 것이 없다 */
		spin_lock_irqsave(&anv->lock, flags);	/* [한국어] 완료 경로와 배타적으로 큐를 훑는다 */
		apple_nvme_handle_cq(q, false);	/* [한국어] 완료 큐를 직접 수확한다 */
		spin_unlock_irqrestore(&anv->lock, flags);
		if (blk_mq_request_completed(req)) {	/* [한국어] 실제로 완료돼 있었다 — 리셋할 이유가 없다 */
			dev_warn(anv->dev,
				 "I/O %d(aq:%d) timeout: completion polled\n",
				 req->tag, q->is_adminq);
			return BLK_EH_DONE;
		}
	}

	/*
	 * aborting commands isn't supported which leaves a full reset as our
	 * only option here
	 */
	/* [한국어] 위 영어 주석대로 이 하드웨어는 Abort 를 지원하지 않는다.
	 * PCIe NVMe 가 Abort 를 먼저 시도하고 FC 가 ABTS 를 보내는 것과 달리,
	 * 여기서는 개별 취소 수단이 없어 컨트롤러 리셋뿐이다. */
	dev_warn(anv->dev, "I/O %d(aq:%d) timeout: resetting controller\n",
		 req->tag, q->is_adminq);
	nvme_req(req)->flags |= NVME_REQ_CANCELLED;
	apple_nvme_disable(anv, false);	/* [한국어] 리셋 전에 컨트롤러를 내려 진행 중인 것을 정리한다 */
	nvme_reset_ctrl(&anv->ctrl);	/* [한국어] 코프로세서 재부팅까지 포함한 전체 리셋 */
	return BLK_EH_DONE;	/* [한국어] 요청은 disable 이 정리하므로 blk-mq 가 더 기다릴 필요가 없다 */
}

static int apple_nvme_poll(struct blk_mq_hw_ctx *hctx,	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
			   struct io_comp_batch *iob)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme_queue *q = hctx->driver_data;	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	bool found;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	unsigned long flags;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	spin_lock_irqsave(&anv->lock, flags);	/* [한국어] Apple-ANS: 동기화 — 큐/연결/상태 보호 */
	found = apple_nvme_poll_cq(q, iob);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	spin_unlock_irqrestore(&anv->lock, flags);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return found;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] Admin blk-mq ops — poll 없음, 공유 queue_rq/timeout */
static const struct blk_mq_ops apple_nvme_mq_admin_ops = {	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
	.queue_rq = apple_nvme_queue_rq,	/* [한국어] 제출 핫패스 */
	.complete = apple_nvme_complete_rq,	/* [한국어] unmap+complete */
	.init_hctx = apple_nvme_init_hctx,	/* [한국어] hctx→queue 바인딩 */
	.init_request = apple_nvme_init_request,	/* [한국어] iod 초기화 */
	.timeout = apple_nvme_timeout,	/* [한국어] 타임아웃/리셋 정책 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] I/O blk-mq ops — poll 포함, 단일 hctx */
static const struct blk_mq_ops apple_nvme_mq_ops = {	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
	.queue_rq = apple_nvme_queue_rq,	/* [한국어] 제출 핫패스 */
	.complete = apple_nvme_complete_rq,	/* [한국어] unmap+complete */
	.init_hctx = apple_nvme_init_hctx,	/* [한국어] hctx→ioq */
	.init_request = apple_nvme_init_request,	/* [한국어] iod 초기화 */
	.timeout = apple_nvme_timeout,	/* [한국어] 타임아웃 */
	.poll = apple_nvme_poll,	/* [한국어] blk-mq poll 훅 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_init_queue - sq/cq head·phase·enabled 초기화
 *
 * enable 직후 호출. TCB/CQE 제로잉 후 enabled=true + wmb.
 */
static void apple_nvme_init_queue(struct apple_nvme_queue *q)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	unsigned int depth = apple_nvme_queue_depth(q);	/* [한국어] 큐 깊이 */
	struct apple_nvme *anv = queue_to_apple_nvme(q);	/* [한국어] 장치 */

	q->cq_head = 0;	/* [한국어] 완료 head 리셋 */
	q->cq_phase = 1;	/* [한국어] 기대 phase=1 (빈 메모리가 full 로 오인 방지) */
	if (anv->hw->has_lsq_nvmmu)	/* [한국어] 신형: TCB 배열 클리어 */
		memset(q->tcbs, 0, anv->hw->max_queue_depth	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
			* sizeof(struct apple_nvmmu_tcb));	/* [한국어] 전 태그 TCB 무효 */
	memset(q->cqes, 0, depth * sizeof(struct nvme_completion));	/* [한국어] CQ 링 클리어 */
	WRITE_ONCE(q->enabled, true);	/* [한국어] queue_rq 허용 */
	wmb(); /* ensure the first interrupt sees the initialization */	/* [한국어] IRQ 가 enabled 를 보도록 배리어 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_reset_work - 리셋 워크: disable → RTKit 재기동 → NVMMU/큐 → enable
 *
 * Apple 특수 boot status·선형 SQ·NVMMU 베이스 설정 포함.
 * 실패 시 DELETING + remove_work. 호출 체인: nvme_reset_ctrl → [여기]
 */
/*
 * [한국어]
 * apple_nvme_reset_work - ANS 코프로세서를 부팅하고 NVMe 컨트롤러를 세운다
 *
 * @work: ctrl.reset_work
 * @return: 없음. 실패하면 컨트롤러를 삭제 경로로 보낸다.
 *
 * 이 파일에서 가장 긴 함수이자, Apple ANS 가 다른 NVMe 컨트롤러와 어떻게
 * 다른지가 모두 드러나는 자리다. 일반적인 NVMe 리셋은 CC.EN 을 껐다 켜는
 * 것이지만, 여기서는 그 앞에 코프로세서 하나를 통째로 부팅해야 한다.
 *
 * 절차는 크게 셋이다:
 *   1) RTKit 코프로세서 정지 → 리셋 → 재부팅
 *   2) ANS 고유 레지스터 설정(NVMMU, 선형 SQ, PRP null 검사 해제)
 *   3) 일반적인 NVMe 초기화(AQA/ASQ/ACQ, CC.EN, Identify, 큐 생성)
 *
 * RTKit 이 깨진 경우를 맨 앞에서 거르는 이유는 위 영어 주석이 밝힌다 --
 * 복구할 방법이 알려져 있지 않다. 소프트 리셋조차 RTKit 이 정상적으로
 * 종료되어야 동작하므로, 깨진 상태에서는 시도할 것이 없다.
 *
 * CPU_CONTROL 레지스터를 확인해 소프트 리셋 여부를 가르는 것도 그래서다.
 * CPU 가 멈춰 있어야(우리가 껐거나 이전 단계가 정상 종료했거나) 리셋과
 * 재부팅이 안전하고, 아니면 깨우기만 한다.
 *
 * NVMMU 설정이 T6000 이후 필수라는 점이 이 드라이버의 핵심 제약이다.
 * 명령마다 TCB(Translation Control Block)를 채워야 하고, 그 개수 상한이
 * 곧 큐 깊이 상한이 된다.
 *
 * PRP null 검사 해제는 위 영어 주석이 "chicken bit 으로 보인다"고 적은
 * 그대로다. 끄지 않으면 PRP 필드가 0 인 모든 명령이 -- 그 필드를 쓰지
 * 않는 명령까지 -- 실패한다.
 *
 * I/O 큐가 정확히 하나여야 하는 것도 특징이다. 하드웨어가 그 이상을
 * 지원하지 않으므로, 협상 결과가 1 이 아니면 -ENXIO 로 실패시킨다.
 *
 * 실패 경로가 삭제로 직행하는 이유: 코프로세서 부팅이 실패했다는 것은
 * 재시도로 해결될 문제가 아니다. 네임스페이스를 죽은 것으로 표시해
 * 상위가 곧바로 실패를 보게 하고, remove_work 에 정리를 넘긴다.
 *
 * 실행 컨텍스트: nvme_reset_wq 워크큐. 코프로세서 부팅을 기다리며 오래 잠든다.
 *
 * 호출 체인:
 *   apple_nvme_probe / nvme_reset_ctrl → [이 함수]
 *     → apple_rtkit_boot → nvme_enable_ctrl → apple_nvme_create_cq/sq
 */
static void apple_nvme_reset_work(struct work_struct *work)
{
	unsigned int nr_io_queues = 1;	/* [한국어] 이 하드웨어는 I/O 큐가 정확히 하나다 */
	int ret;
	u32 boot_status, aqa;
	struct apple_nvme *anv =
		container_of(work, struct apple_nvme, ctrl.reset_work);
	enum nvme_ctrl_state state = nvme_ctrl_state(&anv->ctrl);

	if (state != NVME_CTRL_RESETTING) {	/* [한국어] 코어가 상태를 옮긴 뒤 부르므로 그렇지 않으면 경로가 어긋난 것이다 */
		dev_warn(anv->dev, "ctrl state %d is not RESETTING\n", state);
		ret = -ENODEV;
		goto out;
	}

	/* there's unfortunately no known way to recover if RTKit crashed :( */
	if (apple_rtkit_is_crashed(anv->rtk)) {	/* [한국어] 위 영어 주석대로 복구 방법이 알려져 있지 않다 */
		dev_err(anv->dev,
			"RTKit has crashed without any way to recover.");
		ret = -EIO;
		goto out;
	}

	/* RTKit must be shut down cleanly for the (soft)-reset to work */
	if (apple_rtkit_is_running(anv->rtk)) {	/* [한국어] 위 영어 주석대로 소프트 리셋은 정상 종료를 전제한다 */
		/* reset the controller if it is enabled */
		if (anv->ctrl.ctrl_config & NVME_CC_ENABLE)
			apple_nvme_disable(anv, false);	/* [한국어] 코프로세서를 끄기 전에 NVMe 쪽을 먼저 내린다 */
		dev_dbg(anv->dev, "Trying to shut down RTKit before reset.");
		ret = apple_rtkit_shutdown(anv->rtk);
		if (ret)
			goto out;

		writel(0, anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL);	/* [한국어] 코프로세서 CPU 를 멈춘다 */
	}

	/*
	 * Only do the soft-reset if the CPU is not running, which means either we
	 * or the previous stage shut it down cleanly.
	 */
	/* [한국어] 위 영어 주석대로 CPU 가 멈춰 있어야 리셋과 재부팅이 안전하다.
	 * 돌고 있다면 부트로더가 이미 띄워 둔 것이므로 깨우기만 한다. */
	if (!(readl(anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL) &
		APPLE_ANS_COPROC_CPU_CONTROL_RUN)) {

		ret = reset_control_assert(anv->reset);	/* [한국어] 리셋 라인을 걸고 */
		if (ret)
			goto out;

		ret = apple_rtkit_reinit(anv->rtk);	/* [한국어] RTKit 상태를 초기화한 뒤 */
		if (ret)
			goto out;

		ret = reset_control_deassert(anv->reset);	/* [한국어] 리셋을 푼다 */
		if (ret)
			goto out;

		writel(APPLE_ANS_COPROC_CPU_CONTROL_RUN,	/* [한국어] 코프로세서 CPU 를 다시 돌린다 */
		       anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL);

		ret = apple_rtkit_boot(anv->rtk);	/* [한국어] 펌웨어를 올려 부팅한다 */
	} else {
		ret = apple_rtkit_wake(anv->rtk);	/* [한국어] 이미 돌고 있으면 깨우기만 */
	}

	if (ret) {
		dev_err(anv->dev, "ANS did not boot");
		goto out;
	}

	ret = readl_poll_timeout(anv->mmio_nvme + APPLE_ANS_BOOT_STATUS,	/* [한국어] 펌웨어가 NVMe 부분을 초기화할 때까지 폴링한다 */
				 boot_status,
				 boot_status == APPLE_ANS_BOOT_STATUS_OK,
				 USEC_PER_MSEC, APPLE_ANS_BOOT_TIMEOUT);
	if (ret) {
		dev_err(anv->dev, "ANS did not initialize");
		goto out;
	}

	dev_dbg(anv->dev, "ANS booted successfully.");

	/*
	 * Limit the max command size to prevent iod->sg allocations going
	 * over a single page.
	 */
	/* [한국어] 위 영어 주석대로 요청당 sg 배열이 한 페이지를 넘지 않게 제한한다.
	 * 이 값이 블록 계층의 요청 크기 상한이 되어 bio 분할 지점을 정한다. */
	anv->ctrl.max_hw_sectors = min_t(u32, NVME_MAX_KB_SZ << 1,
					 dma_max_mapping_size(anv->dev) >> 9);
	anv->ctrl.max_segments = NVME_MAX_SEGS;

	dma_set_max_seg_size(anv->dev, 0xffffffff);	/* [한국어] 세그먼트 하나의 크기는 제한하지 않는다 — 개수만 위에서 묶었다 */

	if (anv->hw->has_lsq_nvmmu) {
		/*
		 * Enable NVMMU and linear submission queues which is required
		 * since T6000.
		 */
		/* [한국어] 위 영어 주석대로 T6000 이후에는 필수다. 선형 SQ 는 태그를
		 * 도어벨에 쓰는 이 하드웨어 고유의 제출 방식을 뜻한다. */
		writel(APPLE_ANS_LINEAR_SQ_EN,
			anv->mmio_nvme + APPLE_ANS_LINEAR_SQ_CTRL);

		/* Allow as many pending command as possible for both queues */
		writel(anv->hw->max_queue_depth	/* [한국어] 하위 16비트가 한 큐, 상위 16비트가 다른 큐의 한도다 */
			| (anv->hw->max_queue_depth << 16), anv->mmio_nvme
			+ APPLE_ANS_MAX_PEND_CMDS_CTRL);

		/* Setup the NVMMU for the maximum admin and IO queue depth */
		writel(anv->hw->max_queue_depth - 1,	/* [한국어] TCB 개수 상한이 곧 큐 깊이 상한이다 — 명령마다 TCB 하나가 필요하다 */
			anv->mmio_nvme + APPLE_NVMMU_NUM_TCBS);

		/*
		 * This is probably a chicken bit: without it all commands
		 * where any PRP is set to zero (including those that don't use
		 * that field) fail and the co-processor complains about
		 * "completed with err BAD_CMD-" or a "NULL_PRP_PTR_ERR" in the
		 * syslog
		 */
		/* [한국어] 위 영어 주석대로 원인이 불분명한 우회다. 끄지 않으면 PRP 가
		 * 0 인 모든 명령이 — 그 필드를 쓰지 않는 명령까지 — 실패한다. */
		writel(readl(anv->mmio_nvme + APPLE_ANS_UNKNOWN_CTRL) &
			~APPLE_ANS_PRP_NULL_CHECK,
			anv->mmio_nvme + APPLE_ANS_UNKNOWN_CTRL);
	}

	/* Setup the admin queue */
	if (anv->hw->has_lsq_nvmmu)
		aqa = APPLE_NVME_AQ_DEPTH - 1;	/* [한국어] NVMMU 세대는 admin 깊이가 별도로 정해져 있다 */
	else
		aqa = anv->hw->max_queue_depth - 1;	/* [한국어] 구세대는 공유 태그 공간 전체를 쓴다 */
	aqa |= aqa << 16;	/* [한국어] AQA 는 SQ 와 CQ 깊이를 상하위 16비트에 나눠 담는다 */
	writel(aqa, anv->mmio_nvme + NVME_REG_AQA);
	writeq(anv->adminq.sq_dma_addr, anv->mmio_nvme + NVME_REG_ASQ);	/* [한국어] 표준 NVMe 레지스터 — 여기서부터는 일반적인 초기화다 */
	writeq(anv->adminq.cq_dma_addr, anv->mmio_nvme + NVME_REG_ACQ);

	if (anv->hw->has_lsq_nvmmu) {
		/* Setup NVMMU for both queues */
		writeq(anv->adminq.tcb_dma_addr,	/* [한국어] TCB 배열의 위치를 하드웨어에 알린다 — 명령마다 여기에 기록한다 */
			anv->mmio_nvme + APPLE_NVMMU_ASQ_TCB_BASE);
		writeq(anv->ioq.tcb_dma_addr,
			anv->mmio_nvme + APPLE_NVMMU_IOSQ_TCB_BASE);
	}

	anv->ctrl.sqsize =
		anv->hw->max_queue_depth - 1; /* 0's based queue depth */
	anv->ctrl.cap = readq(anv->mmio_nvme + NVME_REG_CAP);	/* [한국어] 컨트롤러 능력 레지스터 — nvme_enable_ctrl 이 타임아웃 계산에 쓴다 */

	dev_dbg(anv->dev, "Enabling controller now");
	ret = nvme_enable_ctrl(&anv->ctrl);	/* [한국어] CC.EN 을 세우고 CSTS.RDY 를 기다린다 */
	if (ret)
		goto out;

	dev_dbg(anv->dev, "Starting admin queue");
	apple_nvme_init_queue(&anv->adminq);	/* [한국어] phase 와 도어벨 상태를 초기화한다 */
	nvme_unquiesce_admin_queue(&anv->ctrl);

	if (!nvme_change_ctrl_state(&anv->ctrl, NVME_CTRL_CONNECTING)) {
		dev_warn(anv->ctrl.device,
			 "failed to mark controller CONNECTING\n");
		ret = -ENODEV;
		goto out;
	}

	ret = nvme_init_ctrl_finish(&anv->ctrl, false);	/* [한국어] Identify 를 읽어 능력을 확정한다 */
	if (ret)
		goto out;

	dev_dbg(anv->dev, "Creating IOCQ");
	ret = apple_nvme_create_cq(anv);	/* [한국어] CQ 를 먼저 만든다 — SQ 가 완료를 올릴 곳이 있어야 한다 */
	if (ret)
		goto out;
	dev_dbg(anv->dev, "Creating IOSQ");
	ret = apple_nvme_create_sq(anv);
	if (ret)
		goto out_remove_cq;

	apple_nvme_init_queue(&anv->ioq);
	nr_io_queues = 1;
	ret = nvme_set_queue_count(&anv->ctrl, &nr_io_queues);	/* [한국어] 형식상 협상하지만 이 하드웨어는 하나뿐이다 */
	if (ret)
		goto out_remove_sq;
	if (nr_io_queues != 1) {	/* [한국어] 하나가 아니면 이 드라이버의 전제가 어긋난 것이다 */
		ret = -ENXIO;
		goto out_remove_sq;
	}

	anv->ctrl.queue_count = nr_io_queues + 1;	/* [한국어] admin 하나를 더해 둘 */

	nvme_unquiesce_io_queues(&anv->ctrl);
	nvme_wait_freeze(&anv->ctrl);	/* [한국어] 진행 중 I/O 가 없어야 하드웨어 큐 수를 갱신할 수 있다 */
	blk_mq_update_nr_hw_queues(&anv->tagset, 1);
	nvme_unfreeze(&anv->ctrl);

	if (!nvme_change_ctrl_state(&anv->ctrl, NVME_CTRL_LIVE)) {
		dev_warn(anv->ctrl.device,
			 "failed to mark controller live state\n");
		ret = -ENODEV;
		goto out_remove_sq;
	}

	nvme_start_ctrl(&anv->ctrl);	/* [한국어] keep-alive 를 걸고 네임스페이스를 스캔한다 */

	dev_dbg(anv->dev, "ANS boot and NVMe init completed.");
	return;

out_remove_sq:
	apple_nvme_remove_sq(anv);
out_remove_cq:
	apple_nvme_remove_cq(anv);
out:
	dev_warn(anv->ctrl.device, "Reset failure status: %d\n", ret);
	/* [한국어] 코프로세서 부팅 실패는 재시도로 해결되지 않으므로 삭제로 직행한다 */
	nvme_change_ctrl_state(&anv->ctrl, NVME_CTRL_DELETING);
	nvme_get_ctrl(&anv->ctrl);	/* [한국어] remove_work 가 돌 때까지 컨트롤러가 살아 있도록 참조를 든다 */
	apple_nvme_disable(anv, false);
	nvme_mark_namespaces_dead(&anv->ctrl);	/* [한국어] 상위가 곧바로 실패를 보게 한다 — 기다리게 두지 않는다 */
	if (!queue_work(nvme_wq, &anv->remove_work))
		nvme_put_ctrl(&anv->ctrl);	/* [한국어] 이미 큐에 있었다면 방금 든 참조를 놓는다 */
}

static void apple_nvme_remove_dead_ctrl_work(struct work_struct *work)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv =	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
		container_of(work, struct apple_nvme, remove_work);	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */

	nvme_put_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	device_release_driver(anv->dev);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_reg_read32 - ctrl_ops: mmio_nvme + off 32b 읽기
 *
 * core enable/identify 가 Property 대용 MMIO 접근에 사용.
 */
static int apple_nvme_reg_read32(struct nvme_ctrl *ctrl, u32 off, u32 *val)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	*val = readl(ctrl_to_apple_nvme(ctrl)->mmio_nvme + off);	/* [한국어] NVMe BAR 창 32b 로드 */
	return 0;	/* [한국어] MMIO 는 동기 성공 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_reg_write32 - ctrl_ops: MMIO 32b 기록 (CC 등)
 */
static int apple_nvme_reg_write32(struct nvme_ctrl *ctrl, u32 off, u32 val)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	writel(val, ctrl_to_apple_nvme(ctrl)->mmio_nvme + off);	/* [한국어] NVMe 레지스터 32b 스토어 */
	return 0;	/* [한국어] 성공 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_reg_read64 - ctrl_ops: CAP 등 64b 읽기
 */
static int apple_nvme_reg_read64(struct nvme_ctrl *ctrl, u32 off, u64 *val)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	*val = readq(ctrl_to_apple_nvme(ctrl)->mmio_nvme + off);	/* [한국어] 64b MMIO (lo_hi 가능) */
	return 0;	/* [한국어] 성공 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_get_address - sysfs address 에 platform 디바이스 이름
 */
static int apple_nvme_get_address(struct nvme_ctrl *ctrl, char *buf, int size)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct device *dev = ctrl_to_apple_nvme(ctrl)->dev;	/* [한국어] platform device */

	return snprintf(buf, size, "%s\n", dev_name(dev));	/* [한국어] 디바이스 노드 이름 출력 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_free_ctrl - admin_q put 및 device 참조 해제
 */
static void apple_nvme_free_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = ctrl_to_apple_nvme(ctrl);	/* [한국어] 장치 */

	if (anv->ctrl.admin_q)	/* [한국어] admin request_queue 가 있으면 */
		blk_put_queue(anv->ctrl.admin_q);	/* [한국어] 큐 참조 반환 */
	put_device(anv->dev);	/* [한국어] get_device 대칭 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] Apple 트랜스포트 nvme_ctrl_ops — fabrics 플래그 없음(로컬 MMIO) */
static const struct nvme_ctrl_ops nvme_ctrl_ops = {	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	.name = "apple-nvme",	/* [한국어] sysfs/로그 이름 */
	.module = THIS_MODULE,	/* [한국어] 모듈 소유 */
	.flags = 0,	/* [한국어] FABRICS 아님 — 로컬 플랫폼 */
	.reg_read32 = apple_nvme_reg_read32,	/* [한국어] MMIO 32 읽기 */
	.reg_write32 = apple_nvme_reg_write32,	/* [한국어] MMIO 32 쓰기 */
	.reg_read64 = apple_nvme_reg_read64,	/* [한국어] MMIO 64 읽기 */
	.free_ctrl = apple_nvme_free_ctrl,	/* [한국어] 최종 해제 */
	.get_address = apple_nvme_get_address,	/* [한국어] sysfs address */
	.get_virt_boundary = nvme_get_virt_boundary,	/* [한국어] 공통 virt boundary */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static void apple_nvme_async_probe(void *data, async_cookie_t cookie)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = data;	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */

	flush_work(&anv->ctrl.reset_work);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	flush_work(&anv->ctrl.scan_work);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nvme_put_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static void devm_apple_nvme_put_tag_set(void *data)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	blk_mq_free_tag_set(data);	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_alloc_tagsets - admin 과 I/O 태그셋을 만든다. 태그 공간을 공유한다
 *
 * @anv: 대상 장치
 * @return: 0 이면 성공. 음수면 할당 실패.
 *
 * 이 함수의 전부는 reserved_tags 한 줄에 있다. 위 영어 주석이 그 이유를
 * 밝힌다 -- 태그가 NVMMU 의 TCB 배열 인덱스로 쓰이므로 두 큐에 걸쳐
 * 유일해야 한다. admin 이 앞쪽 APPLE_NVME_AQ_DEPTH 개를 차지하고, I/O
 * 태그셋은 그만큼을 예약된 것으로 표시해 겹치지 않게 한다.
 *
 * 다른 NVMe 드라이버에서는 admin 과 I/O 가 완전히 별개의 태그 공간을 쓴다.
 * 여기서만 이런 제약이 있는 것은 TCB 가 하드웨어 자원이고 그 인덱스가
 * 곧 태그이기 때문이다. 그래서 실효 I/O 큐 깊이도 그만큼 줄어든다.
 *
 * nr_hw_queues 가 둘 다 1 인 것은 이 하드웨어가 큐를 하나씩만 갖기 때문이고,
 * nr_maps 도 1 이라 읽기 전용이나 폴링 큐 구분이 없다.
 *
 * devm_add_action_or_reset 으로 해제를 등록하는 이유: 태그셋은 devm 이
 * 직접 다루는 자원이 아니라 해제 동작을 손으로 걸어 줘야 한다.
 * or_reset 이라 등록 자체가 실패하면 그 자리에서 해제까지 해 준다.
 *
 * 실행 컨텍스트: probe 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   apple_nvme_alloc → [이 함수] → blk_mq_alloc_tag_set
 */
static int apple_nvme_alloc_tagsets(struct apple_nvme *anv)
{
	int ret;

	anv->admin_tagset.ops = &apple_nvme_mq_admin_ops;
	anv->admin_tagset.nr_hw_queues = 1;	/* [한국어] 이 하드웨어는 admin 큐가 하나뿐이다 */
	anv->admin_tagset.queue_depth = APPLE_NVME_AQ_MQ_TAG_DEPTH;
	anv->admin_tagset.timeout = NVME_ADMIN_TIMEOUT;
	anv->admin_tagset.numa_node = NUMA_NO_NODE;	/* [한국어] SoC 내장이라 NUMA 지역성이 의미 없다 */
	anv->admin_tagset.cmd_size = sizeof(struct apple_nvme_iod);	/* [한국어] 태그마다 요청별 상태를 미리 잡는다 */
	anv->admin_tagset.driver_data = &anv->adminq;	/* [한국어] hctx 에서 큐로 돌아가는 통로 */

	ret = blk_mq_alloc_tag_set(&anv->admin_tagset);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(anv->dev, devm_apple_nvme_put_tag_set,	/* [한국어] 태그셋은 devm 이 직접 다루지 않아 해제 동작을 걸어 준다 */
				       &anv->admin_tagset);
	if (ret)
		return ret;

	anv->tagset.ops = &apple_nvme_mq_ops;
	anv->tagset.nr_hw_queues = 1;	/* [한국어] I/O 큐도 하나뿐이다 */
	anv->tagset.nr_maps = 1;	/* [한국어] 읽기 전용이나 폴링 큐 구분이 없다 */
	/*
	 * Tags are used as an index to the NVMMU and must be unique across
	 * both queues. The admin queue gets the first APPLE_NVME_AQ_DEPTH which
	 * must be marked as reserved in the IO queue.
	 */
	/* [한국어] 위 영어 주석이 이 파일에서 가장 중요한 제약을 밝힌다. 태그가
	 * NVMMU 의 TCB 배열 인덱스로 쓰이므로 두 큐에 걸쳐 유일해야 한다. 다른
	 * NVMe 드라이버는 admin 과 I/O 가 별개의 태그 공간을 쓰지만, 여기서는
	 * TCB 가 하드웨어 자원이고 그 인덱스가 곧 태그라 공유할 수밖에 없다.
	 * 그래서 실효 I/O 큐 깊이도 admin 몫만큼 줄어든다. */
	if (anv->hw->has_lsq_nvmmu)
		anv->tagset.reserved_tags = APPLE_NVME_AQ_DEPTH;	/* [한국어] 앞쪽 admin 몫을 예약해 겹치지 않게 한다 */
	anv->tagset.queue_depth = anv->hw->max_queue_depth - 1;
	anv->tagset.timeout = NVME_IO_TIMEOUT;
	anv->tagset.numa_node = NUMA_NO_NODE;
	anv->tagset.cmd_size = sizeof(struct apple_nvme_iod);
	anv->tagset.driver_data = &anv->ioq;

	ret = blk_mq_alloc_tag_set(&anv->tagset);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(anv->dev, devm_apple_nvme_put_tag_set,
					&anv->tagset);
	if (ret)
		return ret;

	anv->ctrl.admin_tagset = &anv->admin_tagset;	/* [한국어] 코어가 취소·순회에 쓸 수 있도록 등록한다 */
	anv->ctrl.tagset = &anv->tagset;

	return 0;
}

static int apple_nvme_queue_alloc(struct apple_nvme *anv,	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
				  struct apple_nvme_queue *q)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	unsigned int depth = apple_nvme_queue_depth(q);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	size_t iosq_size;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	q->cqes = dmam_alloc_coherent(anv->dev,	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
				      depth * sizeof(struct nvme_completion),	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
				      &q->cq_dma_addr, GFP_KERNEL);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (!q->cqes)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		return -ENOMEM;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */

	if (anv->hw->has_lsq_nvmmu)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		iosq_size = depth * sizeof(struct nvme_command);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	else	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		iosq_size = depth << APPLE_NVME_IOSQES;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	q->sqes = dmam_alloc_coherent(anv->dev, iosq_size,	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
				      &q->sq_dma_addr, GFP_KERNEL);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (!q->sqes)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		return -ENOMEM;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */

	if (anv->hw->has_lsq_nvmmu) {	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		/*
		 * We need the maximum queue depth here because the NVMMU only
		 * has a single depth configuration shared between both queues.
		 */
		q->tcbs = dmam_alloc_coherent(anv->dev,	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
			anv->hw->max_queue_depth *	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
				sizeof(struct apple_nvmmu_tcb),	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
			&q->tcb_dma_addr, GFP_KERNEL);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		if (!q->tcbs)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
			return -ENOMEM;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	/*
	 * initialize phase to make sure the allocated and empty memory
	 * doesn't look like a full cq already.
	 */
	q->cq_phase = 1;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	return 0;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static void apple_nvme_detach_genpd(struct apple_nvme *anv)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	int i;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (anv->pd_count <= 1)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		return;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */

	for (i = anv->pd_count - 1; i >= 0; i--) {	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		if (anv->pd_link[i])	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
			device_link_del(anv->pd_link[i]);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		if (!IS_ERR_OR_NULL(anv->pd_dev[i]))	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
			dev_pm_domain_detach(anv->pd_dev[i], true);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_attach_genpd - 이 장치가 요구하는 전원 도메인들을 모두 붙인다
 *
 * @anv: 대상 장치
 * @return: 0 이면 성공(도메인이 하나뿐이면 할 일 없이 0). 음수면 실패.
 *
 * ANS 는 전원 도메인을 여러 개 요구한다 -- 코프로세서, NVMe 블록, PHY 등이
 * 각각 별도 도메인이다. 그것들이 모두 켜져야 MMIO 접근이 성립하므로,
 * 이 함수가 실패하면 그 뒤의 어떤 단계도 진행할 수 없다.
 *
 * 하나 이하면 곧바로 0 을 돌려주는 이유: 도메인이 하나뿐이면 드라이버
 * 코어가 자동으로 붙여 준다. 여러 개일 때만 손으로 관리해야 한다.
 *
 * device_link 를 함께 거는 것이 핵심이다. RPM_ACTIVE 로 걸면 이 장치가
 * 활성인 동안 도메인이 꺼지지 않고, PM_RUNTIME 으로 걸면 절전 전환이
 * 순서대로 일어난다. 링크 없이 도메인만 붙이면 런타임 절전에서 도메인이
 * 먼저 꺼져 MMIO 접근이 죽는다.
 *
 * STATELESS 는 이 링크의 수명을 드라이버가 직접 관리하겠다는 뜻이다 --
 * detach_genpd 가 명시적으로 끊는다.
 *
 * 실패 시 곧바로 detach_genpd 를 부르는 것은, 이미 붙인 도메인들을
 * 되돌리기 위해서다. devm 이 다루지 않는 자원이라 손으로 풀어야 한다.
 *
 * 실행 컨텍스트: probe 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   apple_nvme_alloc → [이 함수] → dev_pm_domain_attach_by_id → device_link_add
 */
static int apple_nvme_attach_genpd(struct apple_nvme *anv)
{
	struct device *dev = anv->dev;
	int i;

	anv->pd_count = of_count_phandle_with_args(	/* [한국어] 디바이스 트리가 몇 개의 도메인을 요구하는지 센다 */
		dev->of_node, "power-domains", "#power-domain-cells");
	if (anv->pd_count <= 1)
		return 0;	/* [한국어] 하나뿐이면 드라이버 코어가 자동으로 붙여 준다 */

	anv->pd_dev = devm_kcalloc(dev, anv->pd_count, sizeof(*anv->pd_dev),
				   GFP_KERNEL);
	if (!anv->pd_dev)
		return -ENOMEM;

	anv->pd_link = devm_kcalloc(dev, anv->pd_count, sizeof(*anv->pd_link),	/* [한국어] 도메인마다 링크를 하나씩 건다 */
				    GFP_KERNEL);
	if (!anv->pd_link)
		return -ENOMEM;

	for (i = 0; i < anv->pd_count; i++) {
		anv->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);	/* [한국어] 코프로세서·NVMe 블록·PHY 등이 각각 별도 도메인이다 */
		if (IS_ERR(anv->pd_dev[i])) {
			apple_nvme_detach_genpd(anv);	/* [한국어] 이미 붙인 것들을 되돌린다 — devm 이 다루지 않는 자원이다 */
			return PTR_ERR(anv->pd_dev[i]);
		}

		anv->pd_link[i] = device_link_add(dev, anv->pd_dev[i],	/* [한국어] 링크 없이 붙이기만 하면 런타임 절전에서 도메인이 먼저 꺼져 MMIO 가 죽는다 */
						  DL_FLAG_STATELESS |	/* [한국어] 수명을 드라이버가 직접 관리한다 — detach 가 명시적으로 끊는다 */
						  DL_FLAG_PM_RUNTIME |	/* [한국어] 절전 전환이 순서대로 일어나게 한다 */
						  DL_FLAG_RPM_ACTIVE);	/* [한국어] 이 장치가 활성인 동안 도메인이 꺼지지 않는다 */
		if (!anv->pd_link[i]) {
			apple_nvme_detach_genpd(anv);
			return -EINVAL;
		}
	}

	return 0;
}

static void devm_apple_nvme_mempool_destroy(void *data)	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	mempool_destroy(data);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/*
 * [한국어]
 * apple_nvme_alloc - 플랫폼 장치에서 필요한 자원을 모두 확보한다
 *
 * @pdev: 디바이스 트리에서 매칭된 플랫폼 장치
 * @return: 준비된 apple_nvme. 실패하면 ERR_PTR.
 *
 * PCI NVMe 의 probe 가 BAR 하나를 매핑하고 MSI-X 를 잡으면 끝나는 것과
 * 달리, 여기서는 SoC 에 흩어진 자원을 하나씩 모아야 한다. 이 함수가 그
 * 목록이고, 무엇이 없으면 ANS 가 동작하지 않는지를 그대로 보여 준다:
 * 전원 도메인, 두 개의 MMIO 창, SART, 리셋 라인, IRQ, RTKit.
 *
 * devm_ 계열을 일관되게 쓰는 것이 이 함수의 구조를 단순하게 만든다.
 * 대부분의 자원이 장치 해제 시 자동으로 풀리므로, 실패 경로가
 * genpd 분리와 장치 참조 반납 두 줄로 끝난다. 그 둘만 devm 이 다루지
 * 못하는 것이다.
 *
 * MMIO 창이 둘인 것이 이 하드웨어의 구조를 드러낸다. "ans" 는 코프로세서
 * 제어(CPU 시작/정지)이고 "nvme" 는 NVMe 레지스터와 ANS 확장 레지스터다.
 * 두 창이 별개라 코프로세서를 세우는 일과 NVMe 를 세우는 일이 분리된다.
 *
 * 도어벨 주소를 세대별로 다르게 잡는 것이 이 함수의 핵심 분기다.
 * NVMMU 세대는 선형 SQ 도어벨(태그를 쓰는 곳)이 ANS 전용 레지스터에 있고,
 * 구세대는 표준 NVMe 도어벨 영역을 쓴다. 이 주소가 제출 경로의 마지막
 * 한 줄이 쓰는 곳이므로, 여기서 잘못 잡으면 명령이 하드웨어에 닿지 않는다.
 *
 * SART 는 코프로세서가 접근해도 되는 물리 주소 범위를 정하는 장치다.
 * 이것 없이는 ANS 가 호스트 메모리를 읽지 못한다.
 *
 * 두 DMA 풀을 크기별로 나누는 이유는 PRP 리스트 대부분이 작기 때문이다.
 * 256바이트 풀이 흔한 경우를 받아 페이지 하나를 통째로 쓰는 낭비를 막는다.
 *
 * quirk 두 개를 붙여 코어를 등록한다. SKIP_CID_GEN 은 명령 ID 를 코어가
 * 생성하지 않게 하는 것이고 -- 이 하드웨어는 태그가 곧 ID 다 --
 * IDENTIFY_CNS 는 Identify 의 일부 CNS 값을 쓰지 않게 한다.
 *
 * 실행 컨텍스트: probe 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   apple_nvme_probe → [이 함수] → nvme_init_ctrl
 */
static struct apple_nvme *apple_nvme_alloc(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_nvme *anv;
	int ret;

	anv = devm_kzalloc(dev, sizeof(*anv), GFP_KERNEL);	/* [한국어] devm — 장치 해제 시 자동으로 풀린다 */
	if (!anv)
		return ERR_PTR(-ENOMEM);

	anv->dev = get_device(dev);	/* [한국어] devm 이 다루지 못하는 참조. 실패 경로가 직접 놓는다 */
	anv->adminq.is_adminq = true;	/* [한국어] 두 큐를 구분하는 유일한 표식 */
	platform_set_drvdata(pdev, anv);

	anv->hw = of_device_get_match_data(&pdev->dev);	/* [한국어] SoC 세대별 차이(has_lsq_nvmmu, max_queue_depth)가 여기서 온다 */
	if (!anv->hw) {
		ret = -ENODEV;
		goto put_dev;
	}

	ret = apple_nvme_attach_genpd(anv);	/* [한국어] 전원 도메인 — 켜지지 않으면 코프로세서가 존재하지 않는다 */
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to attach power domains");
		goto put_dev;
	}
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))) {	/* [한국어] 64비트 DMA — SART 가 허용하는 범위 안에서 쓴다 */
		ret = -ENXIO;
		goto put_dev;
	}

	anv->irq = platform_get_irq(pdev, 0);	/* [한국어] 완료 인터럽트. PCI 와 달리 MSI-X 가 아니라 SoC 인터럽트 하나다 */
	if (anv->irq < 0) {
		ret = anv->irq;
		goto put_dev;
	}
	if (!anv->irq) {	/* [한국어] 0 은 유효한 IRQ 번호가 아니다 */
		ret = -ENXIO;
		goto put_dev;
	}

	anv->mmio_coproc = devm_platform_ioremap_resource_byname(pdev, "ans");	/* [한국어] 코프로세서 제어 창 — CPU 시작/정지가 여기 있다 */
	if (IS_ERR(anv->mmio_coproc)) {
		ret = PTR_ERR(anv->mmio_coproc);
		goto put_dev;
	}
	anv->mmio_nvme = devm_platform_ioremap_resource_byname(pdev, "nvme");	/* [한국어] NVMe 레지스터와 ANS 확장 레지스터 창 */
	if (IS_ERR(anv->mmio_nvme)) {	/* [한국어] 두 창이 별개라 코프로세서를 세우는 일과 NVMe 를 세우는 일이 분리된다 */
		ret = PTR_ERR(anv->mmio_nvme);
		goto put_dev;
	}

	if (anv->hw->has_lsq_nvmmu) {	/* [한국어] 도어벨 주소가 세대별로 다르다 — 제출 경로의 마지막 한 줄이 쓰는 곳이다 */
		anv->adminq.sq_db = anv->mmio_nvme + APPLE_ANS_LINEAR_ASQ_DB;	/* [한국어] 선형 SQ 도어벨 — 태그를 쓰는 ANS 전용 레지스터 */
		anv->adminq.cq_db = anv->mmio_nvme + APPLE_ANS_ACQ_DB;
		anv->ioq.sq_db = anv->mmio_nvme + APPLE_ANS_LINEAR_IOSQ_DB;
		anv->ioq.cq_db = anv->mmio_nvme + APPLE_ANS_IOCQ_DB;
	} else {
		anv->adminq.sq_db = anv->mmio_nvme + NVME_REG_DBS;	/* [한국어] 구세대는 표준 NVMe 도어벨 영역을 쓴다 */
		anv->adminq.cq_db = anv->mmio_nvme + APPLE_ANS_ACQ_DB;
		anv->ioq.sq_db = anv->mmio_nvme + NVME_REG_DBS + 8;	/* [한국어] 큐 하나당 8바이트 간격 — 표준 도어벨 배치다 */
		anv->ioq.cq_db = anv->mmio_nvme + APPLE_ANS_IOCQ_DB;
	}

	anv->sart = devm_apple_sart_get(dev);	/* [한국어] 코프로세서가 접근해도 되는 물리 주소 범위를 정한다 — 없으면 ANS 가 호스트 메모리를 못 읽는다 */
	if (IS_ERR(anv->sart)) {
		ret = dev_err_probe(dev, PTR_ERR(anv->sart),
				    "Failed to initialize SART");
		goto put_dev;
	}

	anv->reset = devm_reset_control_array_get_exclusive(anv->dev);	/* [한국어] 소프트 리셋에 쓸 리셋 라인 */
	if (IS_ERR(anv->reset)) {
		ret = dev_err_probe(dev, PTR_ERR(anv->reset),
				    "Failed to get reset control");
		goto put_dev;
	}

	INIT_WORK(&anv->ctrl.reset_work, apple_nvme_reset_work);
	INIT_WORK(&anv->remove_work, apple_nvme_remove_dead_ctrl_work);
	spin_lock_init(&anv->lock);	/* [한국어] 단일 공유 태그 공간이라 제출과 완료를 이 하나로 직렬화한다 */

	ret = apple_nvme_queue_alloc(anv, &anv->adminq);	/* [한국어] SQE/CQE/TCB 링을 DMA 로 잡는다 */
	if (ret)
		goto put_dev;
	ret = apple_nvme_queue_alloc(anv, &anv->ioq);
	if (ret)
		goto put_dev;

	anv->prp_page_pool = dmam_pool_create("prp list page", anv->dev,	/* [한국어] 큰 PRP 리스트용 — 페이지 크기 */
					      NVME_CTRL_PAGE_SIZE,
					      NVME_CTRL_PAGE_SIZE, 0);
	if (!anv->prp_page_pool) {
		ret = -ENOMEM;
		goto put_dev;
	}

	anv->prp_small_pool =
		dmam_pool_create("prp list 256", anv->dev, 256, 256, 0);	/* [한국어] PRP 리스트 대부분이 작아 흔한 경우를 여기서 받는다 — 페이지를 통째로 쓰는 낭비를 막는다 */
	if (!anv->prp_small_pool) {
		ret = -ENOMEM;
		goto put_dev;
	}

	WARN_ON_ONCE(apple_nvme_iod_alloc_size() > PAGE_SIZE);	/* [한국어] 요청별 상태가 한 페이지를 넘으면 mempool 전제가 깨진다 */
	anv->iod_mempool =
		mempool_create_kmalloc_pool(1, apple_nvme_iod_alloc_size());	/* [한국어] 메모리 압박에서도 최소 하나는 확보되도록 예비를 둔다 */
	if (!anv->iod_mempool) {
		ret = -ENOMEM;
		goto put_dev;
	}
	ret = devm_add_action_or_reset(anv->dev,	/* [한국어] mempool 은 devm 이 직접 다루지 않아 해제 동작을 등록해 준다 */
			devm_apple_nvme_mempool_destroy, anv->iod_mempool);
	if (ret)
		goto put_dev;

	ret = apple_nvme_alloc_tagsets(anv);	/* [한국어] admin 과 I/O 태그셋. 이 하드웨어는 태그 공간을 공유한다 */
	if (ret)
		goto put_dev;

	ret = devm_request_irq(anv->dev, anv->irq, apple_nvme_irq, 0,	/* [한국어] 완료 인터럽트를 건다. 두 큐가 하나의 IRQ 를 공유한다 */
			       "nvme-apple", anv);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to request IRQ");
		goto put_dev;
	}

	anv->rtk =
		devm_apple_rtkit_init(dev, anv, NULL, 0, &apple_nvme_rtkit_ops);	/* [한국어] 코프로세서 펌웨어와의 메시지 채널 */
	if (IS_ERR(anv->rtk)) {
		ret = dev_err_probe(dev, PTR_ERR(anv->rtk),
				    "Failed to initialize RTKit");
		goto put_dev;
	}

	ret = nvme_init_ctrl(&anv->ctrl, anv->dev, &nvme_ctrl_ops,	/* [한국어] 이제 코어에 등록한다 */
			     NVME_QUIRK_SKIP_CID_GEN | NVME_QUIRK_IDENTIFY_CNS);	/* [한국어] 태그가 곧 명령 ID 라 코어가 ID 를 생성하면 안 되고, 일부 Identify CNS 도 쓰지 않는다 */
	if (ret) {
		dev_err_probe(dev, ret, "Failed to initialize nvme_ctrl");
		goto put_dev;
	}

	return anv;
put_dev:	/* [한국어] 대부분이 devm 이라 되감을 것이 둘뿐이다 */
	apple_nvme_detach_genpd(anv);
	put_device(anv->dev);
	return ERR_PTR(ret);
}

static int apple_nvme_probe(struct platform_device *pdev)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv;	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	int ret;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	anv = apple_nvme_alloc(pdev);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (IS_ERR(anv))	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		return PTR_ERR(anv);	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */

	ret = nvme_add_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	if (ret)	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		goto out_put_ctrl;	/* [한국어] Apple-ANS: 공통 정리 라벨로 점프 — 부분 성공 롤백 */

	anv->ctrl.admin_q = blk_mq_alloc_queue(&anv->admin_tagset, NULL, NULL);	/* [한국어] Apple-ANS: blk-mq 연동 — 태그·제출·완료 경로 */
	if (IS_ERR(anv->ctrl.admin_q)) {	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		ret = -ENOMEM;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		anv->ctrl.admin_q = NULL;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		goto out_uninit_ctrl;	/* [한국어] Apple-ANS: 공통 정리 라벨로 점프 — 부분 성공 롤백 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	nvme_reset_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	async_schedule(apple_nvme_async_probe, anv);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return 0;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */

out_uninit_ctrl:	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nvme_uninit_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
out_put_ctrl:	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nvme_put_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	apple_nvme_detach_genpd(anv);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	return ret;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static void apple_nvme_remove(struct platform_device *pdev)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = platform_get_drvdata(pdev);	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */

	nvme_change_ctrl_state(&anv->ctrl, NVME_CTRL_DELETING);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	flush_work(&anv->ctrl.reset_work);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nvme_stop_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	nvme_remove_namespaces(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
	apple_nvme_disable(anv, true);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	nvme_uninit_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */

	if (apple_rtkit_is_running(anv->rtk)) {	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		apple_rtkit_shutdown(anv->rtk);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

		writel(0, anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	apple_nvme_detach_genpd(anv);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static void apple_nvme_shutdown(struct platform_device *pdev)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = platform_get_drvdata(pdev);	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */

	apple_nvme_disable(anv, true);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	if (apple_rtkit_is_running(anv->rtk)) {	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		apple_rtkit_shutdown(anv->rtk);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

		writel(0, anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_resume(struct device *dev)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = dev_get_drvdata(dev);	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */

	return nvme_reset_ctrl(&anv->ctrl);	/* [한국어] Apple-ANS: NVMe 코어/호스트 API 호출 — 컨트롤러·요청 생명주기와 연결 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static int apple_nvme_suspend(struct device *dev)	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
{	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	struct apple_nvme *anv = dev_get_drvdata(dev);	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	int ret = 0;	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	apple_nvme_disable(anv, true);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	if (apple_rtkit_is_running(anv->rtk)) {	/* [한국어] Apple-ANS: 제어 분기 — 오류·상태·자원 조건에 따른 경로 선택 */
		ret = apple_rtkit_shutdown(anv->rtk);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

		writel(0, anv->mmio_coproc + APPLE_ANS_COPROC_CPU_CONTROL);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

	return ret;	/* [한국어] Apple-ANS: 반환 — 상위 계층에 성공/에러/상태 전달 */
}	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

static DEFINE_SIMPLE_DEV_PM_OPS(apple_nvme_pm_ops, apple_nvme_suspend,	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
				apple_nvme_resume);	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] T8015: 구형 링 tail 제출, 깊이 16 */
static const struct apple_nvme_hw apple_nvme_t8015_hw = {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	.has_lsq_nvmmu = false,	/* [한국어] NVMMU 선형 SQ 없음 */
	.max_queue_depth = 16,	/* [한국어] 소형 깊이 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] T8103+: 선형 SQ+NVMMU, 공유 태그 깊이 64 */
static const struct apple_nvme_hw apple_nvme_t8103_hw = {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	.has_lsq_nvmmu = true,	/* [한국어] TCB+태그 도어벨 경로 */
	.max_queue_depth = 64,	/* [한국어] 공유 태그 상한 0x40 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */

/* [한국어] DT compatible → hw 변형 테이블 */
static const struct of_device_id apple_nvme_of_match[] = {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	{ .compatible = "apple,t8015-nvme-ans2", .data = &apple_nvme_t8015_hw },	/* [한국어] A11 계열 ANS2 */
	{ .compatible = "apple,t8103-nvme-ans2", .data = &apple_nvme_t8103_hw },	/* [한국어] M1 계열 ANS2 */
	{ .compatible = "apple,nvme-ans2", .data = &apple_nvme_t8103_hw },	/* [한국어] 제네릭 ANS2 → t8103 정책 */
	{},	/* [한국어] 센티널 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
MODULE_DEVICE_TABLE(of, apple_nvme_of_match);	/* [한국어] 모듈 OF 별칭 테이블 */

/* [한국어] platform_driver — probe/remove/shutdown + PM */
static struct platform_driver apple_nvme_driver = {	/* [한국어] Apple-ANS: 자료구조/열거 — 트랜스포트 상태 모델 */
	.driver = {	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
		.name = "nvme-apple",	/* [한국어] 드라이버 이름 */
		.of_match_table = apple_nvme_of_match,	/* [한국어] DT 매치 */
		.pm = pm_sleep_ptr(&apple_nvme_pm_ops),	/* [한국어] suspend/resume */
	},	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
	.probe = apple_nvme_probe,	/* [한국어] 장치 탐지 */
	.remove = apple_nvme_remove,	/* [한국어] 장치 제거 */
	.shutdown = apple_nvme_shutdown,	/* [한국어] 시스템 셧다운 */
};	/* [한국어] Apple-ANS: 트랜스포트 경로 실행 — 제출/완료/복구 파이프라인의 한 단계 */
module_platform_driver(apple_nvme_driver);	/* [한국어] 모듈 init/exit 자동 등록 */

MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");	/* [한국어] 저자 */
MODULE_DESCRIPTION("Apple ANS NVM Express device driver");	/* [한국어] 설명 */
MODULE_LICENSE("GPL");	/* [한국어] 라이선스 */
