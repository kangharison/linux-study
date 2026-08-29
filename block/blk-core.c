// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1991, 1992 Linus Torvalds
 * Copyright (C) 1994,      Karl Keyte: Added support for disk statistics
 * Elevator latency, (C) 2000  Andrea Arcangeli <andrea@suse.de> SuSE
 * Queue request tables / lock, selectable elevator, Jens Axboe <axboe@suse.de>
 * kernel-doc documentation started by NeilBrown <neilb@cse.unsw.edu.au>
 *	-  July2000
 * bio rewrite, highmem i/o, etc, Jens Axboe <axboe@suse.de> - may 2001
 */

/*
 * This handles all read/write requests to block devices
 */
/*
 * [한국어 설명] blk-core.c — 블록 계층 핵심 제출/완료 경로
 *
 * === 파일의 역할 ===
 * block/blk-core.c 는 파일시스템이나 상위 계층이 생성한 struct bio 단위 I/O 를
 * request_queue 로 받아들이고, 멀티큐(blk-mq) 경로를 통해 NVMe 드라이버가 실제
 * SQ(Submission Queue)에 넣을 수 있는 요청으로 변환하는 리눅스 블록 계층의 핵심 파일이다.
 * bio 의 유효성 검사, 파티션 sector remap, per-task plug/unplug 배치(batching),
 * I/O polling, 요청 타임아웃 처리, I/O 통계 계정(diskstats), queue freeze/drain
 * 메커니즘 등을 담당하며, 블록 계층과 하위 드라이버 사이의 핵심 추상화 경계 역할을 한다.
 * 모든 파일시스템 I/O 와 NVMe passthrough 요청은 이 파일의 submit_bio() 를 통해 진입한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트: 호스트 커널 — 유저 프로세스 컨텍스트(submit_bio 호출 경로),
 *   softirq 완료 컨텍스트(bio_endio → bi_end_io 콜백), kblockd kthread(timeout_work/unplug)
 * 호출 체인 (NVMe 기준):
 *   vfs_read/write → ext4_readpages → submit_bio [여기]
 *   → submit_bio_noacct [여기] → submit_bio_noacct_nocheck [여기]
 *   → __submit_bio_noacct_mq [여기] → __submit_bio [여기]
 *   → blk_mq_submit_bio [block/blk-mq.c] → blk_mq_get_request
 *   → nvme_queue_rq [drivers/nvme/host/pci.c] → nvme_submit_cmd(doorbell)
 * plug 배치 경로: blk_start_plug [여기] → (bio 제출) → blk_finish_plug [여기]
 *   → __blk_flush_plug [여기] → blk_mq_flush_plug_list [blk-mq.c] → nvme_queue_rq
 * queue freeze 경로: blk_queue_start_drain [여기] → blk_freeze_queue_start
 *   → blk_queue_enter 대기 [여기] → in-flight 전부 완료 → blk_unfreeze_queue
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   block/blk-mq.c         — blk_mq_submit_bio, blk_mq_flush_plug_list, blk_mq_poll,
 *                            blk_mq_wake_waiters, blk_mq_free_plug_rqs
 *   block/blk-cgroup.c     — blkcg_set_ioprio, blk_cgroup_bio_start (cgroup I/O 계정)
 *   block/blk-throttle.c   — blk_throtl_bio (cgroup I/O 속도 제한)
 *   block/blk-pm.c         — blk_pm_resume_queue (runtime PM 상태 확인)
 *   include/linux/blkdev.h — struct request_queue, struct bio, struct blk_plug 등 핵심 자료구조
 * 이 파일에 의존하는 모듈:
 *   drivers/nvme/host/*.c  — submit_bio, blk_queue_enter/exit, blk_sync_queue 사용
 *   fs/ext4, fs/xfs 등      — submit_bio 를 통해 블록 I/O 제출
 *   block/blk-mq.c         — blk_queue_start_drain, blk_alloc_queue, blk_put_queue 사용
 * 핵심 공유 자료구조:
 *   struct request_queue   — NVMe SQ/CQ/tagset 과 연결되는 블록 계층 핸들;
 *                            q->limits 에 NVMe 최대 전송 크기·PRP/SGL 제약이 반영됨
 *   struct bio             — 단일 I/O 요청 단위 (sector 범위, bvec 목록, 연산 종류, crypt ctx 포함)
 *   struct blk_plug        — per-task request 배치 버퍼; mq_list 에 request 를 모아 NVMe doorbell 최소화
 *
 * === 주요 함수/구조체 요약 ===
 * submit_bio()                 — 파일시스템 I/O 최상위 진입점; vm 통계·ioprio 설정 후 noacct 호출
 * submit_bio_noacct()          — bio 유효성 검사·파티션 remap·op 분류·throttle 적용 후 nocheck 호출
 * submit_bio_noacct_nocheck()  — trace·cgroup 계정·plug 경로 분기 후 __submit_bio 호출
 * __submit_bio()               — blk_mq_submit_bio 또는 fops->submit_bio 로 실제 하부 제출
 * blk_queue_enter/exit()       — queue 사용 참조 카운터 획득/반납; freeze·drain 동기화의 핵심
 * blk_start_plug/finish_plug() — per-task I/O 배치 시작/종료; NVMe doorbell 호출 횟수 최소화
 * blk_queue_start_drain()      — NVMe reset/remove 시 신규 I/O 차단·기존 I/O drain 시작
 * bio_poll()                   — NVMe polled I/O 완료 확인; blk_mq_poll → nvme_poll_cq
 * blk_alloc_queue()            — request_queue 슬랩 할당·초기화; NVMe probe 시 호출
 * blk_dev_init()               — 블록 계층 부트 초기화; kblockd workqueue·슬랩 캐시 생성
 */

#include <linux/kernel.h>        /* [한국어] 커널 기본 매크로·타입(printk, BUG_ON 등) */
#include <linux/module.h>        /* [한국어] EXPORT_SYMBOL 등 모듈 심벌 내보내기 */
#include <linux/bio.h>           /* [한국어] struct bio, bio_endio, bio_alloc 등 bio 핵심 API */
#include <linux/blkdev.h>        /* [한국어] struct request_queue, struct gendisk, blk_queue_* 핵심 자료구조 */
#include <linux/blk-pm.h>        /* [한국어] 블록 계층 runtime PM(Power Management) 지원 */
#include <linux/blk-integrity.h> /* [한국어] T10 PI(Data Integrity Field) 무결성 지원 */
#include <linux/highmem.h>       /* [한국어] 고메모리(high memory) 페이지 kmap/kunmap 지원 */
#include <linux/mm.h>            /* [한국어] struct page, GFP_* 플래그, page 할당 API */
#include <linux/pagemap.h>       /* [한국어] 페이지 캐시 조회·lock 관련 함수 */
#include <linux/kernel_stat.h>   /* [한국어] count_vm_events(PGPGIN/PGPGOUT) 등 커널 통계 */
#include <linux/string.h>        /* [한국어] memset, memcpy 등 문자열/메모리 유틸 */
#include <linux/init.h>          /* [한국어] __init, __setup, late_initcall 등 초기화 매크로 */
#include <linux/completion.h>    /* [한국어] struct completion 동기화 원시 */
#include <linux/slab.h>          /* [한국어] kmem_cache_alloc/free, kzalloc 등 슬랩 할당자 */
#include <linux/swap.h>          /* [한국어] 스왑 페이지 처리; 메모리 회수와 I/O 상호작용 */
#include <linux/writeback.h>     /* [한국어] 페이지 writeback 제어; dirty page flush 경로 */
#include <linux/task_io_accounting_ops.h> /* [한국어] task_io_account_read/write — 태스크별 I/O 통계 */
#include <linux/fault-inject.h>  /* [한국어] fail_make_request 등 fault injection 프레임워크 */
#include <linux/list_sort.h>     /* [한국어] list_sort — plug list 정렬 등에 활용 */
#include <linux/delay.h>         /* [한국어] msleep/udelay 등 지연 함수 */
#include <linux/ratelimit.h>     /* [한국어] pr_info_ratelimited 등 속도 제한 로깅 */
#include <linux/pm_runtime.h>    /* [한국어] pm_runtime_get/put — 장치 runtime PM 참조 카운터 */
#include <linux/t10-pi.h>        /* [한국어] T10 PI(Protection Information) 구조체·함수 */
#include <linux/debugfs.h>       /* [한국어] debugfs_create_dir/file — 블록 계층 디버그 파일시스템 */
#include <linux/bpf.h>           /* [한국어] BPF 프로그램 연결; blk_mq BPF 통계 지원 */
#include <linux/part_stat.h>     /* [한국어] part_stat_lock/inc/add — 파티션별 I/O 통계 갱신 */
#include <linux/sched/sysctl.h>  /* [한국어] sysctl_hung_task_timeout_secs — hang 탐지 타임아웃 */
#include <linux/blk-crypto.h>    /* [한국어] 인라인 암호화(inline crypto) bio 처리 지원 */

#define CREATE_TRACE_POINTS      /* [한국어] 이 파일에서 tracepoint 정의를 생성(trace_block_* 심벌 소유) */
#include <trace/events/block.h>  /* [한국어] block tracepoint 이벤트 정의 (block_bio_queue, block_rq_insert 등) */

#include "blk.h"                 /* [한국어] 블록 계층 내부 공통 선언(blk_freeze_*, blk_set_default_limits 등) */
#include "blk-mq-sched.h"        /* [한국어] blk-mq 스케줄러 내부 API */
#include "blk-pm.h"              /* [한국어] 블록 계층 PM 내부 함수(blk_pm_resume_queue 등) */
#include "blk-cgroup.h"          /* [한국어] blk-cgroup 내부 API(blkg_init_queue, blk_cgroup_bio_start 등) */
#include "blk-throttle.h"        /* [한국어] cgroup I/O throttle 내부 API(blk_throtl_bio 등) */
#include "blk-ioprio.h"          /* [한국어] cgroup I/O 우선순위 내부 API(blkcg_set_ioprio 등) */

struct dentry *blk_debugfs_root; /* [한국어] block debugfs 루트 디렉터리; /sys/kernel/debug/block/ 아래에
                                  * NVMe queue 상태·hctx 정보가 노출됨 (blk_dev_init 에서 생성) */

EXPORT_TRACEPOINT_SYMBOL_GPL(block_bio_remap);   /* [한국어] bio sector remap tracepoint 심벌 내보내기 */
EXPORT_TRACEPOINT_SYMBOL_GPL(block_rq_remap);    /* [한국어] request sector remap tracepoint 심벌 내보내기 */
EXPORT_TRACEPOINT_SYMBOL_GPL(block_bio_complete); /* [한국어] bio 완료 tracepoint 심벌 내보내기 */
EXPORT_TRACEPOINT_SYMBOL_GPL(block_split);        /* [한국어] bio split tracepoint 심벌 내보내기 */
EXPORT_TRACEPOINT_SYMBOL_GPL(block_unplug);       /* [한국어] plug flush(unplug) tracepoint 심벌 내보내기 */
EXPORT_TRACEPOINT_SYMBOL_GPL(block_rq_insert);   /* [한국어] request 스케줄러 삽입 tracepoint 심벌 내보내기 */

static DEFINE_IDA(blk_queue_ida); /* [한국어] request_queue 고유 ID 할당용 IDA(ID Allocator);
                                   * blk_alloc_queue 에서 q->id 를 발급받고 blk_free_queue 에서 반납 */

/*
 * For queue allocation
 */
static struct kmem_cache *blk_requestq_cachep; /* [한국어] request_queue 슬랩 캐시;
                                                 * blk_dev_init 에서 KMEM_CACHE 로 생성,
                                                 * blk_alloc_queue/blk_free_queue_rcu 에서 할당/반납 */

/*
 * Controlling structure to kblockd
 */
static struct workqueue_struct *kblockd_workqueue; /* [한국어] kblockd 커널 스레드 workqueue;
                                                    * NVMe timeout 처리, plug unplug, MQ dispatch
                                                    * 지연 작업이 여기서 실행됨 (WQ_HIGHPRI) */

/**
 * blk_queue_flag_set - atomically set a queue flag
 * @flag: flag to be set
 * @q: request queue
 */
/*
 * [한국어]
 * blk_queue_flag_set - request_queue 의 queue_flags 를 원자적으로 설정한다.
 *
 * @flag: 설정할 비트 플래그 (QUEUE_FLAG_DEAD, QUEUE_FLAG_POLL, QUEUE_FLAG_DYING 등)
 * @q:    대상 request_queue 포인터
 *
 * NVMe 컨트롤러 reset·삭제·기능 변경 시 QUEUE_FLAG_DEAD, QUEUE_FLAG_POLL 등의
 * 플래그를 원자적으로 설정하여 이후 blk_queue_enter() 에서의 I/O 진입을 제어한다.
 * set_bit() 은 메모리 배리어 없이 원자성만 보장하므로, 순서가 중요한 경우 호출자가
 * 별도 배리어를 삽입해야 한다.
 * 실행 컨텍스트: 임의 컨텍스트 (NVMe reset kthread, 드라이버 probe 등)
 *
 * 호출 체인:
 *   nvme_reset_ctrl / nvme_set_queue_count → [blk_queue_flag_set] → set_bit
 */
void blk_queue_flag_set(unsigned int flag, struct request_queue *q)
{
	set_bit(flag, &q->queue_flags); /* [한국어] q->queue_flags 의 원자적 비트 설정;
	                                  * NVMe 에서 QUEUE_FLAG_DEAD/POLL/DYING 등 상태 전환에 사용 */
}
EXPORT_SYMBOL(blk_queue_flag_set);

/**
 * blk_queue_flag_clear - atomically clear a queue flag
 * @flag: flag to be cleared
 * @q: request queue
 */
/*
 * [한국어]
 * blk_queue_flag_clear - request_queue 의 queue_flags 를 원자적으로 해제한다.
 *
 * @flag: 해제할 비트 플래그 (QUEUE_FLAG_DEAD, QUEUE_FLAG_POLL 등)
 * @q:    대상 request_queue 포인터
 *
 * NVMe 컨트롤러가 reset 복구 후 queue 를 재활성화하거나, poll 기능을 동적으로
 * 비활성화할 때 이 함수로 해당 플래그를 클리어한다.
 * clear_bit() 는 set_bit() 과 동일하게 메모리 배리어 없이 원자성만 보장한다.
 * 실행 컨텍스트: 임의 컨텍스트 (NVMe reset 복구 경로, 드라이버 remove 등)
 *
 * 호출 체인:
 *   nvme_reset_ctrl_work / nvme_remove → [blk_queue_flag_clear] → clear_bit
 */
void blk_queue_flag_clear(unsigned int flag, struct request_queue *q)
{
	clear_bit(flag, &q->queue_flags); /* [한국어] q->queue_flags 의 원자적 비트 클리어;
	                                    * NVMe queue 상태 복구·poll 비활성화 등에 사용 */
}
EXPORT_SYMBOL(blk_queue_flag_clear);

#define REQ_OP_NAME(name) [REQ_OP_##name] = #name /* [한국어] REQ_OP_<name> 열거값을 문자열 "name" 으로 매핑하는 헬퍼 매크로 */
static const char *const blk_op_name[] = {
	REQ_OP_NAME(READ),          /* [한국어] REQ_OP_READ → NVMe Read command (opcode 0x02) */
	REQ_OP_NAME(WRITE),         /* [한국어] REQ_OP_WRITE → NVMe Write command (opcode 0x01) */
	REQ_OP_NAME(FLUSH),         /* [한국어] REQ_OP_FLUSH → NVMe Flush command (opcode 0x00) */
	REQ_OP_NAME(DISCARD),       /* [한국어] REQ_OP_DISCARD → NVMe Dataset Management(Deallocate) opcode 0x09 */
	REQ_OP_NAME(SECURE_ERASE),  /* [한국어] REQ_OP_SECURE_ERASE → NVMe Sanitize/Secure Erase 범주 */
	REQ_OP_NAME(ZONE_RESET),    /* [한국어] REQ_OP_ZONE_RESET → NVMe ZNS(Zoned Namespace) Zone Reset */
	REQ_OP_NAME(ZONE_RESET_ALL),/* [한국어] REQ_OP_ZONE_RESET_ALL → NVMe ZNS Zone Reset All */
	REQ_OP_NAME(ZONE_OPEN),     /* [한국어] REQ_OP_ZONE_OPEN → NVMe ZNS Zone Open */
	REQ_OP_NAME(ZONE_CLOSE),    /* [한국어] REQ_OP_ZONE_CLOSE → NVMe ZNS Zone Close */
	REQ_OP_NAME(ZONE_FINISH),   /* [한국어] REQ_OP_ZONE_FINISH → NVMe ZNS Zone Finish */
	REQ_OP_NAME(ZONE_APPEND),   /* [한국어] REQ_OP_ZONE_APPEND → NVMe ZNS Zone Append (opcode 0x7D) */
	REQ_OP_NAME(WRITE_ZEROES),  /* [한국어] REQ_OP_WRITE_ZEROES → NVMe Write Zeroes command (opcode 0x08) */
	REQ_OP_NAME(DRV_IN),        /* [한국어] REQ_OP_DRV_IN → NVMe 드라이버 전용 passthrough(Admin/IO) 입력 */
	REQ_OP_NAME(DRV_OUT),       /* [한국어] REQ_OP_DRV_OUT → NVMe 드라이버 전용 passthrough 출력 */
};
#undef REQ_OP_NAME /* [한국어] 로컬 매크로 정의 해제; 외부 심벌 오염 방지 */

/**
 * blk_op_str - Return the string "name" for an operation REQ_OP_name.
 * @op: a request operation.
 *
 * Convert a request operation REQ_OP_name into the string "name". Useful for
 * debugging and tracing BIOs and requests. For an invalid request operation
 * code, the string "UNKNOWN" is returned.
 */
/*
 * [한국어]
 * blk_op_str - enum req_op 값을 디버깅용 문자열로 변환한다.
 *
 * @op:    변환할 요청 연산 열거값 (REQ_OP_READ, REQ_OP_WRITE 등)
 * @return: 해당 연산명 문자열 ("READ", "WRITE" 등); 유효하지 않으면 "UNKNOWN"
 *
 * blk_op_name[] 배열을 인덱스로 조회하여 NVMe opcode 에 대응하는 문자열을 반환한다.
 * trace_block_bio_queue 등 tracepoint 와 dmesg 로그에서 opcode 의미를 파악할 때 사용된다.
 * 실행 컨텍스트: 임의 컨텍스트 (tracepoint 핸들러, 디버깅 출력)
 *
 * 호출 체인:
 *   trace_block_bio_queue(bio) → [blk_op_str] → blk_op_name[]
 */
inline const char *blk_op_str(enum req_op op)
{
	const char *op_str = "UNKNOWN"; /* [한국어] 기본값 "UNKNOWN"; 범위 초과·미매핑 opcode 시 이 값 반환 */

	if (op < ARRAY_SIZE(blk_op_name) && blk_op_name[op]) /* [한국어] opcode 배열 범위 및 NULL 엔트리 검사;
	                                                         * 잘못된 op 값으로 인한 NULL 역참조 방지 */
		op_str = blk_op_name[op]; /* [한국어] 유효한 opcode 이면 해당 문자열로 교체;
		                            * NVMe tracepoint 인자로 "READ"/"WRITE" 등이 출력됨 */

	return op_str; /* [한국어] 변환 결과 반환; tracepoint·pr_warn 출력에 활용 */
}
EXPORT_SYMBOL_GPL(blk_op_str);

static const struct {
	int		errno;
	const char	*name;
} blk_errors[] = {
	[BLK_STS_OK]		= { 0,		"" },                          /* [한국어] 정상 완료; NVMe CQE SC(Status Code)=0 에 대응 */
	[BLK_STS_NOTSUPP]	= { -EOPNOTSUPP, "operation not supported" }, /* [한국어] 연산 미지원; NVMe ZNS 등 미지원 opcode 요청 시 */
	[BLK_STS_TIMEOUT]	= { -ETIMEDOUT,	"timeout" },               /* [한국어] 명령 타임아웃; NVMe CID 타임아웃 → abort 경로 진입 */
	[BLK_STS_NOSPC]		= { -ENOSPC,	"critical space allocation" }, /* [한국어] 공간 부족; NVMe namespace 공간 고갈 */
	[BLK_STS_TRANSPORT]	= { -ENOLINK,	"recoverable transport" }, /* [한국어] 전송 계층 복구 가능 오류; NVMe CQE DNR(Do Not Retry)=0 상태 */
	[BLK_STS_TARGET]	= { -EREMOTEIO,	"critical target" },       /* [한국어] 타겟 치명적 오류; NVMe CQE SC 치명적 분류 */
	[BLK_STS_RESV_CONFLICT]	= { -EBADE,	"reservation conflict" },  /* [한국어] 예약 충돌; NVMe PR(Persistent Reservation) 충돌 */
	[BLK_STS_MEDIUM]	= { -ENODATA,	"critical medium" },       /* [한국어] 매체 치명적 오류; NVMe 미디어·ECC 불복구 오류 */
	[BLK_STS_PROTECTION]	= { -EILSEQ,	"protection" },            /* [한국어] 데이터 보호 오류; T10 PI(Data Integrity) 검증 실패 */
	[BLK_STS_RESOURCE]	= { -ENOMEM,	"kernel resource" },       /* [한국어] 커널 자원 부족; NVMe tag/CID 할당 실패 등 */
	[BLK_STS_DEV_RESOURCE]	= { -EBUSY,	"device resource" },       /* [한국어] 장치 자원 부족; NVMe SQ full 또는 queue depth 초과 */
	[BLK_STS_AGAIN]		= { -EAGAIN,	"nonblocking retry" },     /* [한국어] 비블로킹 재시도 필요; REQ_NOWAIT NVMe I/O 에서 즉시 반환 */
	[BLK_STS_OFFLINE]	= { -ENODEV,	"device offline" },        /* [한국어] 장치 오프라인; NVMe controller dead 또는 queue DYING 상태 */

	/* device mapper special case, should not leak out: */
	[BLK_STS_DM_REQUEUE]	= { -EREMCHG, "dm internal retry" },      /* [한국어] DM 내부 재요청; 상위 계층으로 누출되면 안 됨 */

	/* zone device specific errors */
	[BLK_STS_ZONE_OPEN_RESOURCE]	= { -ETOOMANYREFS, "open zones exceeded" }, /* [한국어] ZNS open zone 수 초과; NVMe Zone Resource 제약 */
	[BLK_STS_ZONE_ACTIVE_RESOURCE]	= { -EOVERFLOW, "active zones exceeded" },  /* [한국어] ZNS active zone 수 초과; NVMe Zone Resource 제약 */

	/* Command duration limit device-side timeout */
	[BLK_STS_DURATION_LIMIT]	= { -ETIME, "duration limit exceeded" }, /* [한국어] 명령 지속 시간 제한 초과; NVMe CDL(Command Duration Limit) 기능 */

	[BLK_STS_INVAL]		= { -EINVAL,	"invalid" },               /* [한국어] 유효하지 않은 요청; bio 파라미터 검증 실패 */

	/* everything else not covered above: */
	[BLK_STS_IOERR]		= { -EIO,	"I/O" },                   /* [한국어] 기타 I/O 오류; 위 분류에 해당하지 않는 NVMe 오류 */
};

/*
 * [한국어]
 * errno_to_blk_status - 커널 errno 를 blk_status_t 로 변환한다.
 *
 * @errno:  변환할 커널 errno 값 (예: -EIO, -ETIMEDOUT, -ENOMEM 등)
 * @return: 대응하는 blk_status_t; 매핑 없으면 BLK_STS_IOERR
 *
 * blk_errors[] 테이블을 선형 탐색하여 errno 에 대응하는 blk_status_t 를 찾는다.
 * NVMe 드라이버가 nvme_complete_rq → blk_mq_end_request 로 완료를 통보할 때
 * 드라이버 내부 errno 를 블록 계층 상태 코드로 변환하는 데 주로 사용된다.
 * 실행 컨텍스트: softirq 완료 경로 또는 임의 컨텍스트
 *
 * 호출 체인:
 *   nvme_complete_rq → blk_mq_end_request → [errno_to_blk_status] → blk_errors[]
 */
blk_status_t errno_to_blk_status(int errno)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(blk_errors); i++) { /* [한국어] blk_errors[] 선형 탐색; errno → blk_status_t 매핑 */
		if (blk_errors[i].errno == errno) /* [한국어] 일치하는 errno 발견 시 해당 인덱스가 blk_status_t 값 */
			return (__force blk_status_t)i; /* [한국어] 인덱스를 blk_status_t 로 강제 캐스팅하여 반환 */
	}

	return BLK_STS_IOERR; /* [한국어] 매핑 실패 시 BLK_STS_IOERR(-EIO) 로 fallback; 미분류 NVMe 오류 처리 */
}
EXPORT_SYMBOL_GPL(errno_to_blk_status);

/*
 * [한국어]
 * blk_status_to_errno - blk_status_t 를 사용자 공간으로 반환할 errno 로 변환한다.
 *
 * @status: 변환할 blk_status_t 값 (BLK_STS_TIMEOUT, BLK_STS_IOERR 등)
 * @return: 사용자 공간에 반환할 errno 음수값 (예: -ETIMEDOUT, -EIO)
 *
 * NVMe I/O 완료 후 bio_endio → bi_end_io 콜백을 통해 파일시스템이나 상위 계층이
 * 최종 오류 코드를 받을 때, blk_status_t 를 표준 POSIX errno 로 변환한다.
 * blk_errors[] 배열을 인덱스로 직접 접근하며, 범위 초과 시 WARN 하고 -EIO 반환.
 * 실행 컨텍스트: softirq 완료 경로 또는 임의 컨텍스트
 *
 * 호출 체인:
 *   bio_endio → bi_end_io → [blk_status_to_errno] → blk_errors[].errno
 */
int blk_status_to_errno(blk_status_t status)
{
	int idx = (__force int)status; /* [한국어] blk_status_t 를 배열 인덱스로 강제 변환 */

	if (WARN_ON_ONCE(idx >= ARRAY_SIZE(blk_errors))) /* [한국어] 유효 범위 초과 검사; 잘못된 상태값 경고 출력 */
		return -EIO; /* [한국어] 범위 초과 시 -EIO 로 안전하게 fallback */
	return blk_errors[idx].errno; /* [한국어] 테이블에서 대응 errno 반환; 파일시스템에 전달될 최종 오류 코드 */
}
EXPORT_SYMBOL_GPL(blk_status_to_errno);

/*
 * [한국어]
 * blk_status_to_str - blk_status_t 를 디버깅용 사람 가독 문자열로 변환한다.
 *
 * @status: 변환할 blk_status_t 값
 * @return: 상태를 설명하는 문자열 ("timeout", "I/O" 등); 범위 초과 시 "<null>"
 *
 * NVMe I/O 실패 시 dmesg·tracepoint 출력에서 BLK_STS_TIMEOUT("timeout"),
 * BLK_STS_TRANSPORT("recoverable transport") 등의 의미를 파악할 때 사용된다.
 * blk_errors[].name 필드를 그대로 반환하며, 범위 초과 시 WARN 후 "<null>" 반환.
 *
 * 호출 체인:
 *   blk_mq_rq_timed_out → [blk_status_to_str] → blk_errors[].name
 */
const char *blk_status_to_str(blk_status_t status)
{
	int idx = (__force int)status; /* [한국어] blk_status_t 를 배열 인덱스로 강제 변환 */

	if (WARN_ON_ONCE(idx >= ARRAY_SIZE(blk_errors))) /* [한국어] 유효 범위 검사; 잘못된 상태값 경고 */
		return "<null>"; /* [한국어] 범위 초과 시 null 플레이스홀더 문자열 반환 */
	return blk_errors[idx].name; /* [한국어] 상태에 대응하는 가독 문자열 반환; dmesg 출력에 활용 */
}
EXPORT_SYMBOL_GPL(blk_status_to_str);

/**
 * blk_sync_queue - cancel any pending callbacks on a queue
 * @q: the queue
 *
 * Description:
 *     The block layer may perform asynchronous callback activity
 *     on a queue, such as calling the unplug function after a timeout.
 *     A block device may call blk_sync_queue to ensure that any
 *     such activity is cancelled, thus allowing it to release resources
 *     that the callbacks might use. The caller must already have made sure
 *     that its ->submit_bio will not re-add plugging prior to calling
 *     this function.
 *
 *     This function does not cancel any asynchronous activity arising
 *     out of elevator or throttling code. That would require elevator_exit()
 *     and blkcg_exit_queue() to be called with queue lock initialized.
 *
 */
/*
 * [한국어]
 * blk_sync_queue - request_queue 에 등록된 비동기 callback(타이머·work)을 동기적으로 취소한다.
 *
 * @q: 대상 request_queue 포인터
 *
 * NVMe 컨트롤러 제거·reset 시점에 q->timeout 타이머와 q->timeout_work 가
 * 더 이상 동작하지 않도록 동기적으로(실행 완료까지 대기) 취소한다.
 * timer_delete_sync() 는 타이머 핸들러가 다른 CPU 에서 실행 중이면 완료를 기다리고,
 * cancel_work_sync() 는 work 가 실행 중이면 완료를 기다린 뒤 취소한다.
 * 이 함수 이후에는 타이머/work 재등록이 없다고 호출자가 보장해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트만 허용 (sleep 가능; cancel_work_sync 내부 대기)
 *
 * 호출 체인:
 *   nvme_shutdown_ctrl / nvme_remove → [blk_sync_queue] → timer_delete_sync + cancel_work_sync
 */
void blk_sync_queue(struct request_queue *q)
{
	timer_delete_sync(&q->timeout); /* [한국어] NVMe 명령 타임아웃 타이머를 동기적으로 삭제;
	                                  * 다른 CPU 에서 실행 중인 경우 완료 대기 후 취소 */
	cancel_work_sync(&q->timeout_work); /* [한국어] kblockd 의 timeout_work 동기 취소;
	                                      * NVMe reset/remove 후 불필요한 abort 작업 방지 */
}
EXPORT_SYMBOL(blk_sync_queue);

/**
 * blk_set_pm_only - increment pm_only counter
 * @q: request queue pointer
 */
/*
 * [한국어]
 * blk_set_pm_only - request_queue 의 pm_only 카운터를 원자적으로 증가시킨다.
 *
 * @q: 대상 request_queue 포인터
 *
 * pm_only > 0 이면 blk_queue_enter() 에서 BLK_MQ_REQ_PM 플래그 없는 일반 I/O 는
 * 대기 상태로 들어간다. NVMe runtime PM·ACPI D-state 전환 시 queue 를 일시 동결하고
 * PM 경로 전용 I/O 만 허용하기 위해 이 카운터를 올린다.
 * blk_clear_pm_only() 와 반드시 짝을 이뤄야 한다.
 * 실행 컨텍스트: 임의 컨텍스트 (runtime PM suspend 경로)
 *
 * 호출 체인:
 *   nvme_runtime_suspend / blk_pre_runtime_suspend → [blk_set_pm_only] → atomic_inc
 */
void blk_set_pm_only(struct request_queue *q)
{
	atomic_inc(&q->pm_only); /* [한국어] pm_only 원자 증가; 이후 BLK_MQ_REQ_PM 없는 I/O 는 대기 */
}
EXPORT_SYMBOL_GPL(blk_set_pm_only);

/*
 * [한국어]
 * blk_clear_pm_only - pm_only 카운터를 감소시키고 0이 되면 freeze 대기자를 깨운다.
 *
 * @q: 대상 request_queue 포인터
 *
 * NVMe 장치가 runtime PM 에서 깨어나(resume) queue 를 다시 활성화할 때 호출한다.
 * pm_only 가 0이 되면 mq_freeze_wq 에서 대기 중인 일반 I/O 진입자를 wake_up_all 로
 * 깨워 blk_queue_enter() 루프를 재시도하게 한다.
 * pm_only 가 음수가 되면 blk_set_pm_only/blk_clear_pm_only 짝이 맞지 않는 버그이다.
 * 실행 컨텍스트: 임의 컨텍스트 (runtime PM resume 경로)
 *
 * 호출 체인:
 *   nvme_runtime_resume / blk_post_runtime_resume → [blk_clear_pm_only] → wake_up_all
 */
void blk_clear_pm_only(struct request_queue *q)
{
	int pm_only; /* [한국어] pm_only 반환값을 로컬에 저장하여 0 여부 판단 */

	pm_only = atomic_dec_return(&q->pm_only); /* [한국어] pm_only 원자 감소; 반환값 = 감소 후 값 */
	WARN_ON_ONCE(pm_only < 0); /* [한국어] 음수이면 set/clear 짝이 맞지 않는 버그; WARN 출력 */
	if (pm_only == 0) /* [한국어] 0이 되면 PM 동결 해제; 대기 중인 I/O 진입 허용 */
		wake_up_all(&q->mq_freeze_wq); /* [한국어] mq_freeze_wq 에서 대기 중인 모든 태스크 깨움;
		                                  * blk_queue_enter() 루프에서 pm_only 조건 재검사 */
}
EXPORT_SYMBOL_GPL(blk_clear_pm_only);

/*
 * [한국어]
 * blk_free_queue_rcu - RCU grace period 완료 후 request_queue 메모리를 최종 해제한다.
 *
 * @rcu_head: request_queue 내부의 rcu_head 포인터 (call_rcu 콜백 인자)
 *
 * NVMe 장치 제거 후 모든 RCU 리더가 grace period 를 통과하면 이 콜백이 호출된다.
 * percpu_ref_exit() 로 q_usage_counter 의 percpu 변수를 정리하고,
 * kmem_cache_free() 로 request_queue 슬랩 메모리를 반납한다.
 * 이 시점 이후 q 포인터는 무효화되므로 어떠한 접근도 금지된다.
 * 실행 컨텍스트: RCU callback (softirq 또는 rcuo kthread)
 *
 * 호출 체인:
 *   blk_put_queue → blk_free_queue → call_rcu → [blk_free_queue_rcu]
 */
static void blk_free_queue_rcu(struct rcu_head *rcu_head)
{
	struct request_queue *q = container_of(rcu_head, /* [한국어] rcu_head 에서 request_queue 역참조;
	                                                    * container_of 로 외부 구조체 포인터 복원 */
			struct request_queue, rcu_head);

	percpu_ref_exit(&q->q_usage_counter); /* [한국어] percpu_ref 정리; q_usage_counter percpu 메모리 해제 */
	kmem_cache_free(blk_requestq_cachep, q); /* [한국어] request_queue 슬랩 반납; NVMe queue 핸들 완전 소멸 */
}

/*
 * [한국어]
 * blk_free_queue - request_queue 의 모든 하위 자원을 해제하고 RCU 지연 해제를 예약한다.
 *
 * @q: 해제할 request_queue 포인터 (refcount 가 0이 된 후 호출)
 *
 * blk_put_queue() 에서 refcount 가 0이 되면 이 함수가 호출된다.
 * queue 통계·MQ 자원(hctx, tagset)·IDA 슬롯·lockdep 키를 정리한 뒤,
 * call_rcu() 로 blk_free_queue_rcu() 를 예약하여 percpu_ref 와 슬랩 메모리를
 * RCU grace period 이후에 안전하게 해제한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_put_queue 호출자)
 *
 * 호출 체인:
 *   blk_put_queue → [blk_free_queue] → blk_mq_release + call_rcu → blk_free_queue_rcu
 */
static void blk_free_queue(struct request_queue *q)
{
	blk_free_queue_stats(q->stats); /* [한국어] queue 통계 구조체 해제; NVMe I/O accounting 정리 */
	if (queue_is_mq(q)) /* [한국어] blk-mq 기반 queue(NVMe 포함)이면 MQ 전용 자원 해제 */
		blk_mq_release(q); /* [한국어] hctx 배열·tagset 연결·sysfs 항목 등 MQ 자원 반납 */

	ida_free(&blk_queue_ida, q->id); /* [한국어] queue ID 를 IDA 에 반납; 이후 다른 queue 에 재사용 가능 */
	lockdep_unregister_key(&q->io_lock_cls_key); /* [한국어] I/O 경로 lockdep 키 등록 해제 */
	lockdep_unregister_key(&q->q_lock_cls_key); /* [한국어] queue 상태 lockdep 키 등록 해제 */
	call_rcu(&q->rcu_head, blk_free_queue_rcu); /* [한국어] RCU grace period 후 percpu_ref·슬랩 해제 예약;
	                                               * RCU 리더가 q 포인터를 들고 있을 수 있으므로 즉시 해제 금지 */
}

/**
 * blk_put_queue - decrement the request_queue refcount
 * @q: the request_queue structure to decrement the refcount for
 *
 * Decrements the refcount of the request_queue and free it when the refcount
 * reaches 0.
 */
/*
 * [한국어]
 * blk_put_queue - request_queue 의 참조 카운트를 감소시키고 0이 되면 해제한다.
 *
 * @q: 참조 카운트를 반납할 request_queue 포인터
 *
 * blk_get_queue() 로 올린 refcount 를 반납한다. refcount_dec_and_test() 가 true
 * (즉, 감소 후 0) 를 반환하면 blk_free_queue() 를 호출하여 모든 자원을 해제한다.
 * NVMe 드라이버의 namespace 제거, gendisk 해제 등 queue 생명주기 종료 시 호출된다.
 * 실행 컨텍스트: 임의 컨텍스트 (refcount 가 0이 되는 경로에 따라 다름)
 *
 * 호출 체인:
 *   blk_cleanup_disk / nvme_remove_ns → [blk_put_queue] → blk_free_queue
 */
void blk_put_queue(struct request_queue *q)
{
	if (refcount_dec_and_test(&q->refs)) /* [한국어] refcount 감소; 0이 되면 true 반환 — 마지막 참조자 */
		blk_free_queue(q); /* [한국어] 마지막 참조자이면 queue 전체 자원 해제 */
}
EXPORT_SYMBOL(blk_put_queue);

/*
 * [한국어]
 * blk_queue_start_drain - queue 를 DYING 상태로 만들고 신규 I/O 진입을 차단한다.
 *
 * @q:      drain 을 시작할 request_queue 포인터
 * @return: queue 를 실제로 freeze 했으면 true, 이미 freeze 중이었으면 false
 *
 * NVMe 컨트롤러 reset·remove·surprise removal 시 기존 in-flight I/O 는 drain 하면서
 * 새로운 submit_bio 호출이 -ENODEV 를 받도록 queue 를 DYING 상태로 전환한다.
 * __blk_freeze_queue_start() 로 q_usage_counter 를 kill 하여 blk_queue_enter() 가
 * 더 이상 성공하지 못하게 막고, 대기 중인 태스크를 깨워 DYING 상태를 재검사하게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (NVMe reset kthread, nvme_remove)
 *
 * 호출 체인:
 *   nvme_remove / nvme_reset_ctrl → blk_cleanup_queue → [blk_queue_start_drain]
 *   → __blk_freeze_queue_start + blk_mq_wake_waiters + wake_up_all
 */
bool blk_queue_start_drain(struct request_queue *q)
{
	/*
	 * When queue DYING flag is set, we need to block new req
	 * entering queue, so we call blk_freeze_queue_start() to
	 * prevent I/O from crossing blk_queue_enter().
	 */
	bool freeze = __blk_freeze_queue_start(q, current); /* [한국어] q_usage_counter 를 kill 하여
	                                                       * 신규 blk_queue_enter() 차단 시작 */
	if (queue_is_mq(q)) /* [한국어] blk-mq 기반 queue(NVMe 포함)이면 태그 대기자도 깨움 */
		blk_mq_wake_waiters(q); /* [한국어] tag 할당 대기 중인 태스크를 깨워 DYING 상태 재검사 유도 */
	/* Make blk_queue_enter() reexamine the DYING flag. */
	wake_up_all(&q->mq_freeze_wq); /* [한국어] mq_freeze_wq 에서 대기 중인 모든 태스크를 깨움;
	                                  * blk_queue_enter() 가 dying 플래그를 보고 -ENODEV 반환하게 함 */

	return freeze; /* [한국어] 실제 freeze 수행 여부 반환; 호출자가 unfreeze 시점 결정에 사용 */
}

/**
 * blk_queue_enter() - try to increase q->q_usage_counter
 * @q: request queue pointer
 * @flags: BLK_MQ_REQ_NOWAIT and/or BLK_MQ_REQ_PM
 */
/*
 * [한국어]
 * blk_queue_enter - request_queue 사용 카운터를 획득하여 I/O 진입 허가를 받는다.
 *
 * @q:     진입할 request_queue 포인터
 * @flags: BLK_MQ_REQ_NOWAIT (비블로킹), BLK_MQ_REQ_PM (PM 경로 I/O) 플래그 조합
 * @return: 0 = 성공(q_usage_counter 획득), -EAGAIN = NOWAIT 조건 불충족, -ENODEV = queue DYING
 *
 * blk_try_enter_queue() 로 percpu_ref q_usage_counter 획득을 시도한다.
 * 실패 시: NOWAIT 이면 즉시 -EAGAIN 반환; 아니면 mq_freeze_wq 에서 조건 충족까지 대기.
 * queue 가 DYING 상태가 되면 대기에서 깨어나 -ENODEV 를 반환한다.
 * NVMe controller reset 중에 신규 I/O 가 SQ 에 진입하지 못하도록 여기서 차단한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (NOWAIT 아닌 경우 sleep 가능)
 *
 * 호출 체인:
 *   blk_mq_get_request → [blk_queue_enter] → blk_try_enter_queue / wait_event
 */
int blk_queue_enter(struct request_queue *q, blk_mq_req_flags_t flags)
{
	const bool pm = flags & BLK_MQ_REQ_PM; /* [한국어] PM 경로 여부 추출; true 이면 pm_only > 0 인 queue 도 진입 허용 */

	while (!blk_try_enter_queue(q, pm)) { /* [한국어] percpu_ref 획득 시도; freeze·dying 상태면 실패하여 루프 진입 */
		if (flags & BLK_MQ_REQ_NOWAIT) /* [한국어] NOWAIT 플래그: 대기 없이 즉시 실패 반환 */
			return -EAGAIN; /* [한국어] NOWAIT 이면 즉시 -EAGAIN 반환; caller 가 REQ_NOWAIT bio 에 오류 전달 */

		/*
		 * read pair of barrier in blk_freeze_queue_start(), we need to
		 * order reading __PERCPU_REF_DEAD flag of .q_usage_counter and
		 * reading .mq_freeze_depth or queue dying flag, otherwise the
		 * following wait may never return if the two reads are
		 * reordered.
		 */
		smp_rmb(); /* [한국어] 읽기 메모리 배리어; blk_freeze_queue_start() 의 쓰기 배리어와 짝을 이뤄
		             * __PERCPU_REF_DEAD 플래그 읽기와 mq_freeze_depth/dying 플래그 읽기 순서 보장 */
		wait_event(q->mq_freeze_wq, /* [한국어] freeze_wq 에서 아래 조건 중 하나가 참이 될 때까지 대기 */
			   (!q->mq_freeze_depth && /* [한국어] freeze depth 가 0이고 (unfrozen 상태) */
			    blk_pm_resume_queue(pm, q)) || /* [한국어] PM resume 조건 충족 (pm_only=0 또는 PM 경로) */
			   blk_queue_dying(q)); /* [한국어] QUEUE_FLAG_DYING 설정 시 즉시 깨어나 오류 경로 진입 */
		if (blk_queue_dying(q)) /* [한국어] 깨어난 후 DYING 상태이면 -ENODEV 반환 */
			return -ENODEV; /* [한국어] NVMe queue DYING → -ENODEV; 상위에서 I/O 오류로 처리 */
	}

	rwsem_acquire_read(&q->q_lockdep_map, 0, 0, _RET_IP_); /* [한국어] lockdep: queue enter 읽기 획득 마크 */
	rwsem_release(&q->q_lockdep_map, _RET_IP_); /* [한국어] lockdep: 즉시 해제 (실제 락은 percpu_ref 로 관리) */
	return 0; /* [한국어] queue 사용 카운터 획득 성공; 이제 NVMe SQ 로의 bio 전달 허가됨 */
}

/*
 * [한국어]
 * __bio_queue_enter - bio 경로 전용 request_queue 사용 카운터 획득 함수.
 *
 * @q:   진입할 request_queue 포인터
 * @bio: 제출 중인 bio (GD_DEAD 확인·bio_io_error 호출에 사용)
 * @return: 0 = 성공, -EAGAIN = NOWAIT 불충족, -ENODEV = GD_DEAD (bio_io_error 도 호출됨)
 *
 * blk_queue_enter() 와 유사하지만 bio 기반 장치 경로 전용으로, queue dying 대신
 * GD_DEAD(gendisk 소멸) 비트를 감시한다. GD_DEAD 이면 bio_io_error() 를 호출하고
 * -ENODEV 를 반환하여 caller(bio_queue_enter 매크로)가 즉시 반환하게 한다.
 * fops->submit_bio 를 가진 장치(DM, loopback 등)의 submit 경로에서 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (NOWAIT 아닌 경우 sleep 가능)
 *
 * 호출 체인:
 *   bio_queue_enter(bio) → [__bio_queue_enter] → blk_try_enter_queue / wait_event
 */
int __bio_queue_enter(struct request_queue *q, struct bio *bio)
{
	while (!blk_try_enter_queue(q, false)) { /* [한국어] percpu_ref 획득 시도; 실패 시 루프 진입 */
		struct gendisk *disk = bio->bi_bdev->bd_disk; /* [한국어] bio 의 gendisk 획득; GD_DEAD 상태 확인용 */

		if (bio->bi_opf & REQ_NOWAIT) { /* [한국어] NOWAIT bio 이면 대기 없이 즉시 분기 */
			if (test_bit(GD_DEAD, &disk->state)) /* [한국어] GD_DEAD: gendisk 가 이미 소멸된 경우 */
				goto dead; /* [한국어] dead 레이블로 이동; bio_io_error + -ENODEV 반환 */
			bio_wouldblock_error(bio); /* [한국어] 살아있지만 would-block → bio 에 -EAGAIN 완료 설정 */
			return -EAGAIN; /* [한국어] NOWAIT + queue freeze 중 → -EAGAIN 반환 */
		}

		/*
		 * read pair of barrier in blk_freeze_queue_start(), we need to
		 * order reading __PERCPU_REF_DEAD flag of .q_usage_counter and
		 * reading .mq_freeze_depth or queue dying flag, otherwise the
		 * following wait may never return if the two reads are
		 * reordered.
		 */
		smp_rmb(); /* [한국어] 읽기 배리어; blk_freeze_queue_start() 와 짝을 이뤄 순서 보장 */
		wait_event(q->mq_freeze_wq, /* [한국어] freeze_wq 에서 조건 충족까지 대기 */
			   (!q->mq_freeze_depth && /* [한국어] freeze depth 0 (unfrozen) 조건 */
			    blk_pm_resume_queue(false, q)) || /* [한국어] PM resume 조건 (bio 경로는 pm=false) */
			   test_bit(GD_DEAD, &disk->state)); /* [한국어] GD_DEAD 설정 시 즉시 깨어남 */
		if (test_bit(GD_DEAD, &disk->state)) /* [한국어] 깨어난 후 GD_DEAD 이면 dead 경로 */
			goto dead; /* [한국어] gendisk 소멸 확인 → dead 레이블로 */
	}

	rwsem_acquire_read(&q->io_lockdep_map, 0, 0, _RET_IP_); /* [한국어] lockdep: bio 경로 queue enter 마크 */
	rwsem_release(&q->io_lockdep_map, _RET_IP_); /* [한국어] lockdep: 즉시 해제 */
	return 0; /* [한국어] bio 경로 queue 사용 카운터 획득 성공 */
dead:
	bio_io_error(bio); /* [한국어] bio 에 BLK_STS_IOERR(-EIO) 설정하고 bi_end_io 콜백 호출 */
	return -ENODEV; /* [한국어] -ENODEV 반환; caller 가 submit 중단 */
}

/*
 * [한국어]
 * blk_queue_exit - request_queue 사용 카운터(q_usage_counter)를 반납한다.
 *
 * @q: 카운터를 반납할 request_queue 포인터
 *
 * blk_queue_enter() 로 획득한 percpu_ref 참조를 반납한다. 모든 참조가 반납되면
 * blk_queue_usage_counter_release() 콜백이 호출되어 mq_freeze_wq 대기자를 깨운다.
 * NVMe I/O 경로의 끝(blk_mq_end_request 이후)에서 반드시 호출해야 한다.
 * 실행 컨텍스트: 임의 컨텍스트 (I/O 완료 콜백 포함)
 *
 * 호출 체인:
 *   blk_mq_end_request → [blk_queue_exit] → percpu_ref_put → (0 되면) 콜백 호출
 */
void blk_queue_exit(struct request_queue *q)
{
	percpu_ref_put(&q->q_usage_counter); /* [한국어] percpu_ref 반납; 0이 되면 release 콜백으로 freeze 완료 알림 */
}

/*
 * [한국어]
 * blk_queue_usage_counter_release - q_usage_counter 가 0이 되면 freeze 대기자를 깨운다.
 *
 * @ref: q_usage_counter percpu_ref 포인터
 *
 * percpu_ref 의 release 콜백으로, q_usage_counter 가 0(모든 I/O 완료)이 되면
 * wake_up_all() 로 mq_freeze_wq 에서 대기 중인 freeze/drain 완료 대기자를 깨운다.
 * NVMe controller reset 시 blk_freeze_queue() 가 이 이벤트를 기다린다.
 * 실행 컨텍스트: percpu_ref 가 kill 된 후 마지막 put 의 컨텍스트 (임의)
 *
 * 호출 체인:
 *   (마지막 blk_queue_exit) → percpu_ref_put → [blk_queue_usage_counter_release] → wake_up_all
 */
static void blk_queue_usage_counter_release(struct percpu_ref *ref)
{
	struct request_queue *q = /* [한국어] percpu_ref 를 포함하는 request_queue 역참조 */
		container_of(ref, struct request_queue, q_usage_counter); /* [한국어] container_of 로 q 포인터 복원 */

	wake_up_all(&q->mq_freeze_wq); /* [한국어] in-flight I/O 전부 완료 → freeze 대기자 깨움; drain 완료 신호 */
}

/*
 * [한국어]
 * blk_rq_timed_out_timer - request_queue timeout 타이머의 softirq 콜백.
 *
 * @t: 만료된 timer_list 포인터 (request_queue.timeout 필드)
 *
 * NVMe 명령이 q->timeout 기간 내에 CQ(Completion Queue)에서 완료되지 않으면
 * 이 타이머 핸들러가 softirq 컨텍스트에서 호출되어, kblockd workqueue 에
 * timeout_work 를 예약한다. 실제 타임아웃 처리는 blk-mq 스케줄러나
 * NVMe 드라이버의 mq_ops->timeout 콜백에서 수행된다.
 * 실행 컨텍스트: softirq (타이머 핸들러)
 *
 * 호출 체인:
 *   kernel timer expire → [blk_rq_timed_out_timer] → kblockd_schedule_work(timeout_work)
 */
static void blk_rq_timed_out_timer(struct timer_list *t)
{
	struct request_queue *q = timer_container_of(q, t, timeout); /* [한국어] timeout 타이머로부터 request_queue 복원 */

	kblockd_schedule_work(&q->timeout_work); /* [한국어] kblockd workqueue 에 timeout_work 예약;
	                                           * 프로세스 컨텍스트에서 abort/reset 처리 시작 */
}

/*
 * [한국어]
 * blk_timeout_work - timeout_work 의 work 함수 (현재 커널 버전에서는 빈 구현).
 *
 * @work: timeout_work 의 work_struct 포인터
 *
 * 실제 타임아웃 처리는 blk_mq_timeout_work() 등 상위에서 처리되며,
 * 이 함수는 timer 에서 schedule 된 work 의 진입점으로만 사용된다.
 * NVMe 드라이버의 mq_ops->timeout 콜백이 개별 request 타임아웃을 처리한다.
 * 실행 컨텍스트: kblockd workqueue kthread
 */
static void blk_timeout_work(struct work_struct *work)
{
}

/*
 * [한국어]
 * blk_alloc_queue - 새로운 request_queue 를 슬랩에서 할당하고 완전히 초기화한다.
 *
 * @lim:     드라이버가 설정한 queue limits (최대 전송 크기, segment 수, PRP/SGL 등)
 * @node_id: NUMA 노드 ID (NVMe 컨트롤러 소속 노드)
 * @return:  초기화된 request_queue 포인터; 실패 시 ERR_PTR(-errno)
 *
 * NVMe 드라이버가 blk_mq_init_queue() → blk_alloc_queue() 경로로 호출하며,
 * 이 queue 가 이후 NVMe SQ/CQ/tagset 이 연결되는 블록 계층 핸들이 된다.
 * 슬랩 할당 → IDA ID 발급 → 통계 구조체 → limits 복사 → 타이머/워크 초기화
 * → 각종 mutex/spinlock → percpu_ref 초기화 → lockdep 순서로 진행된다.
 * 실패 시 goto 레이블을 통해 역순으로 자원을 해제한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (NVMe probe/init 경로)
 *
 * 호출 체인:
 *   nvme_alloc_ns / blk_mq_init_queue → [blk_alloc_queue] → percpu_ref_init + ...
 */
struct request_queue *blk_alloc_queue(struct queue_limits *lim, int node_id)
{
	struct request_queue *q; /* [한국어] 새로 할당할 request_queue 포인터 */
	int error; /* [한국어] 오류 코드; goto 정리 경로에서 ERR_PTR 인자로 사용 */

	q = kmem_cache_alloc_node(blk_requestq_cachep, GFP_KERNEL | __GFP_ZERO, /* [한국어] 슬랩 캐시에서 0-초기화 할당;
	                                                                             * NVMe SQ/CQ 연결 대상 핸들 생성 */
				  node_id); /* [한국어] NVMe 컨트롤러 NUMA 노드에서 할당; NUMA-local 접근 최적화 */
	if (!q) /* [한국어] 슬랩 할당 실패 */
		return ERR_PTR(-ENOMEM); /* [한국어] -ENOMEM 반환; NVMe probe 실패 */

	q->last_merge = NULL; /* [한국어] 마지막 merge 후보 초기화; NVMe 는 일반적으로 request merge 를 제한 */

	q->id = ida_alloc(&blk_queue_ida, GFP_KERNEL); /* [한국어] queue 고유 ID 할당; debugfs 경로 등에 사용 */
	if (q->id < 0) { /* [한국어] IDA 할당 실패 검사 */
		error = q->id; /* [한국어] IDA 오류 코드 저장 */
		goto fail_q; /* [한국어] 슬랩만 해제하는 정리 경로로 이동 */
	}

	q->stats = blk_alloc_queue_stats(); /* [한국어] queue I/O 통계 구조체 할당; diskstats 갱신에 사용 */
	if (!q->stats) { /* [한국어] 통계 구조체 할당 실패 */
		error = -ENOMEM; /* [한국어] -ENOMEM 설정 */
		goto fail_id; /* [한국어] ID 반납 후 슬랩 해제 경로로 이동 */
	}

	error = blk_set_default_limits(lim); /* [한국어] limits 구조체에 커널 기본값 설정 */
	if (error) /* [한국어] limits 초기화 실패 */
		goto fail_stats; /* [한국어] 통계 해제 경로로 이동 */
	q->limits = *lim; /* [한국어] 드라이버가 설정한 limits 복사; NVMe 최대 전송 크기·PRP/SGL·ZNS·poll 등 반영 */

	q->node = node_id; /* [한국어] NUMA 노드 저장; hctx CPU 매핑·메모리 할당 최적화에 활용 */

	atomic_set(&q->nr_active_requests_shared_tags, 0); /* [한국어] shared tag set 기준 활성 요청 카운터 초기화 */

	timer_setup(&q->timeout, blk_rq_timed_out_timer, 0); /* [한국어] 명령 타임아웃 타이머 초기화;
	                                                         * q->timeout 기간 내 CQ 미완료 시 blk_rq_timed_out_timer 호출 */
	INIT_WORK(&q->timeout_work, blk_timeout_work); /* [한국어] timeout_work 초기화; 타이머 콜백에서 kblockd 로 예약됨 */
	INIT_LIST_HEAD(&q->icq_list); /* [한국어] I/O context 목록 초기화; elevator 가 사용하는 per-process context */

	refcount_set(&q->refs, 1); /* [한국어] refcount=1 로 초기화; 이 queue 를 만든 드라이버가 초기 참조를 보유 */
	mutex_init(&q->debugfs_mutex); /* [한국어] debugfs 항목 보호 mutex 초기화 */
	mutex_init(&q->elevator_lock); /* [한국어] elevator(I/O 스케줄러) 교체 보호 mutex */
	mutex_init(&q->sysfs_lock); /* [한국어] sysfs 항목 접근 보호 mutex */
	mutex_init(&q->limits_lock); /* [한국어] queue limits 변경 보호 mutex */
	mutex_init(&q->rq_qos_mutex); /* [한국어] rq-qos(request QoS) 플러그인 목록 보호 mutex */
	spin_lock_init(&q->queue_lock); /* [한국어] queue 상태 변경 spinlock; elevator 삽입/dispatch 시 사용 */

	init_waitqueue_head(&q->mq_freeze_wq); /* [한국어] freeze/drain 대기 큐 초기화; NVMe reset 시 여기서 대기 */
	mutex_init(&q->mq_freeze_lock); /* [한국어] freeze depth 변경 보호 mutex */

	blkg_init_queue(q); /* [한국어] blk-cgroup queue 초기화; cgroup I/O 계정·throttle·ioprio 연결 */

	/*
	 * Init percpu_ref in atomic mode so that it's faster to shutdown.
	 * See blk_register_queue() for details.
	 */
	error = percpu_ref_init(&q->q_usage_counter, /* [한국어] q_usage_counter percpu_ref 초기화;
	                                               * atomic 모드로 시작하여 shutdown 가속 */
				blk_queue_usage_counter_release, /* [한국어] 카운터 0 시 호출될 release 콜백 등록 */
				PERCPU_REF_INIT_ATOMIC, GFP_KERNEL); /* [한국어] ATOMIC 모드: percpu 변수 없이 단일 원자 카운터로 동작 */
	if (error) /* [한국어] percpu_ref 초기화 실패 */
		goto fail_stats; /* [한국어] 통계 해제 경로로 이동 */
	lockdep_register_key(&q->io_lock_cls_key); /* [한국어] I/O 경로 lockdep 클래스 키 등록 */
	lockdep_register_key(&q->q_lock_cls_key); /* [한국어] queue 상태 lockdep 클래스 키 등록 */
	lockdep_init_map(&q->io_lockdep_map, "&q->q_usage_counter(io)", /* [한국어] I/O 경로 lockdep 맵 초기화;
	                                                                    * bio_queue_enter 경로 추적용 */
			 &q->io_lock_cls_key, 0); /* [한국어] I/O 경로 lockdep 클래스 연결 */
	lockdep_init_map(&q->q_lockdep_map, "&q->q_usage_counter(queue)", /* [한국어] queue 경로 lockdep 맵 초기화;
	                                                                      * blk_queue_enter 경로 추적용 */
			 &q->q_lock_cls_key, 0); /* [한국어] queue 경로 lockdep 클래스 연결 */

	/* Teach lockdep about lock ordering (reclaim WRT queue freeze lock). */
	fs_reclaim_acquire(GFP_KERNEL); /* [한국어] lockdep: 메모리 회수 컨텍스트 진입 표시 (순서 학습용) */
	rwsem_acquire_read(&q->io_lockdep_map, 0, 0, _RET_IP_); /* [한국어] lockdep: queue enter read 획득 표시 */
	rwsem_release(&q->io_lockdep_map, _RET_IP_); /* [한국어] lockdep: 즉시 해제 */
	fs_reclaim_release(GFP_KERNEL); /* [한국어] lockdep: 메모리 회수 컨텍스트 종료 표시 */

	q->nr_requests = BLKDEV_DEFAULT_RQ; /* [한국어] 최대 동시 처리 요청 수; NVMe SQ 깊이·tagset depth 와 대응 */
	q->async_depth = BLKDEV_DEFAULT_RQ; /* [한국어] 비동기 dispatch 가능 깊이; NVMe SQ batch 시 doorbell 빈도에 영향 */

	return q; /* [한국어] 완전히 초기화된 request_queue 반환; NVMe probe 계속 진행 */

fail_stats: /* [한국어] 통계 구조체까지 생성된 경우 정리 */
	blk_free_queue_stats(q->stats); /* [한국어] queue 통계 구조체 해제 */
fail_id: /* [한국어] IDA ID 까지 발급된 경우 정리 */
	ida_free(&blk_queue_ida, q->id); /* [한국어] queue ID 를 IDA 에 반납 */
fail_q: /* [한국어] 슬랩 할당만 된 경우 정리 */
	kmem_cache_free(blk_requestq_cachep, q); /* [한국어] request_queue 슬랩 반납 */
	return ERR_PTR(error); /* [한국어] 오류 포인터 반환; NVMe 드라이버 probe 실패 */
}

/**
 * blk_get_queue - increment the request_queue refcount
 * @q: the request_queue structure to increment the refcount for
 *
 * Increment the refcount of the request_queue kobject.
 *
 * Context: Any context.
 */
/*
 * [한국어]
 * blk_get_queue - request_queue 의 참조 카운트를 증가시킨다.
 *
 * @q:     참조를 획득할 request_queue 포인터
 * @return: true = 획득 성공, false = queue 가 DYING 상태여서 획득 불가
 *
 * NVMe I/O 시작 전 queue 가 여전히 살아있는지(dying 아닌지) 확인하고 refcount 를 올린다.
 * DYING 상태이면 false 를 반환하여 caller 가 I/O 를 포기하게 한다.
 * 반드시 blk_put_queue() 로 짝을 맞춰야 한다.
 * 실행 컨텍스트: 임의 컨텍스트
 *
 * 호출 체인:
 *   bdev_open / nvme_open → [blk_get_queue] → refcount_inc
 */
bool blk_get_queue(struct request_queue *q)
{
	if (unlikely(blk_queue_dying(q))) /* [한국어] QUEUE_FLAG_DYING 검사; NVMe reset/remove 중이면 false */
		return false; /* [한국어] queue DYING → 신규 I/O 차단; caller 가 -ENODEV 반환 */
	refcount_inc(&q->refs); /* [한국어] refcount 증가; queue 생명주기 연장 */
	return true; /* [한국어] 참조 획득 성공 */
}
EXPORT_SYMBOL(blk_get_queue);

#ifdef CONFIG_FAIL_MAKE_REQUEST

static DECLARE_FAULT_ATTR(fail_make_request);

/*
 * [한국어]
 * setup_fail_make_request - 커널 부팅 파라미터 "fail_make_request=" 파서
 *
 * @str: 부팅 커맨드라인에서 "fail_make_request=" 뒤에 온 문자열
 * @return: setup_fault_attr()의 결과(성공 1, 실패 0)
 *
 * fault injection 프레임워크의 표준 파라미터 형식
 * (확률<interval>,<times>,<space>,<verbose>)을 파싱해 전역 fail_make_request
 * 속성에 채운다. 부팅 시점에 이미 I/O 실패 주입을 켜 두고 싶을 때 쓴다.
 *
 * 실행 컨텍스트: 부팅 초기(__setup 매크로가 등록한 파라미터 핸들러).
 * 이 시점에는 아직 대부분의 커널 서브시스템이 초기화되지 않았으므로
 * 단순 파싱만 수행한다.
 *
 * 호출 체인:
 *   커널 부팅 파라미터 파서 → [setup_fail_make_request] → setup_fault_attr
 */
static int __init setup_fail_make_request(char *str)
{
	return setup_fault_attr(&fail_make_request, str);
}
__setup("fail_make_request=", setup_fail_make_request);

/*
 * [한국어]
 * should_fail_request - fault injection 으로 요청 실패를 시뮬레이션한다.
 *
 * @part:  대상 block_device (파티션 또는 whole disk)
 * @bytes: 요청 크기 (bytes); fault injection 확률 계산에 사용
 * @return: true = 인위적 실패 발생, false = 정상 처리
 *
 * BD_MAKE_IT_FAIL 플래그가 설정된 장치에서 fail_make_request 파라미터(debugfs)에
 * 따라 일정 확률로 I/O 실패를 유발한다. NVMe 드라이버의 오류 처리 경로 테스트에 활용.
 * 실행 컨텍스트: submit_bio 경로 (프로세스 컨텍스트)
 */
bool should_fail_request(struct block_device *part, unsigned int bytes)
{
	return bdev_test_flag(part, BD_MAKE_IT_FAIL) && /* [한국어] BD_MAKE_IT_FAIL 설정 장치인지 확인 */
	       should_fail(&fail_make_request, bytes); /* [한국어] fault injection 프레임워크로 실패 여부 결정 */
}

/*
 * [한국어]
 * fail_make_request_debugfs - fault injection 제어 파일을 debugfs에 노출
 *
 * @return: 0 성공, 음수 errno 실패
 *
 * /sys/kernel/debug/fail_make_request/ 아래에 probability, times, interval,
 * verbose 같은 조정 파일을 만든다. 이것으로 실행 중에 I/O 실패 주입 확률을
 * 바꿀 수 있다.
 *
 * 실제로 특정 장치에 실패를 주입하려면 두 가지가 모두 필요하다:
 *   1) 여기서 만든 debugfs 파일로 확률 설정
 *   2) 대상 장치에 BD_MAKE_IT_FAIL 플래그 설정
 *      (/sys/block/nvme0n1/make-it-fail에 1을 쓴다)
 * should_fail_request()가 두 조건을 함께 확인한다.
 *
 * NVMe 오류 처리 경로(nvme_decide_disposition의 재시도 로직, 멀티패스
 * 경로 전환)를 실제 하드웨어 고장 없이 시험하는 데 쓴다.
 *
 * 실행 컨텍스트: late_initcall — debugfs가 마운트 가능해진 이후의 부팅 후반.
 *
 * 호출 체인:
 *   late_initcall → [fail_make_request_debugfs] → fault_create_debugfs_attr
 */
static int __init fail_make_request_debugfs(void)
{
	struct dentry *dir = fault_create_debugfs_attr("fail_make_request",
						NULL, &fail_make_request);

	return PTR_ERR_OR_ZERO(dir);
}

late_initcall(fail_make_request_debugfs);
#endif /* CONFIG_FAIL_MAKE_REQUEST */

/*
 * [한국어]
 * bio_check_ro - read-only block device 에 대한 쓰기 bio 를 검사하고 경고한다.
 *
 * @bio: 검사할 bio
 *
 * 쓰기 연산이면서 bdev 가 read-only 인 경우 pr_warn 으로 경고를 출력한다.
 * Flush-only bio (sector 없는 flush)는 예외적으로 허용한다.
 * BD_RO_WARNED 플래그로 경고 중복을 방지한다.
 * NVMe namespace 가 ioctl 로 read-only 설정된 경우 이 경고가 발생한다.
 * 실행 컨텍스트: submit_bio 경로
 */
static inline void bio_check_ro(struct bio *bio)
{
	if (op_is_write(bio_op(bio)) && bdev_read_only(bio->bi_bdev)) { /* [한국어] 쓰기 연산 && read-only bdev 조합 검사 */
		if (op_is_flush(bio->bi_opf) && !bio_sectors(bio)) /* [한국어] Flush-only bio 예외: sector 없는 flush 는 허용 */
			return; /* [한국어] Flush-only bio 는 read-only 장치에도 허용 */

		if (bdev_test_flag(bio->bi_bdev, BD_RO_WARNED)) /* [한국어] 이미 경고한 적 있으면 중복 출력 방지 */
			return; /* [한국어] 경고 중복 방지를 위해 반환 */

		bdev_set_flag(bio->bi_bdev, BD_RO_WARNED); /* [한국어] BD_RO_WARNED 설정; 이후 동일 장치 경고 억제 */

		/*
		 * Use ioctl to set underlying disk of raid/dm to read-only
		 * will trigger this.
		 */
		pr_warn("Trying to write to read-only block-device %pg\n", /* [한국어] read-only 쓰기 시도 경고 출력; dmesg 에 기록 */
			bio->bi_bdev); /* [한국어] 장치 이름 출력용 bdev 포인터 */
	}
}

/*
 * [한국어]
 * should_fail_bio - bio 단위 fault injection 검사.
 *
 * @bio:    검사할 bio
 * @return: -EIO = 인위적 실패 주입, 0 = 정상 처리
 *
 * bdev_whole() 로 파티션을 whole disk 로 올려 should_fail_request() 를 호출한다.
 * 파티션별로 fault injection 을 설정하더라도 전체 디스크 기준으로 확률이 계산된다.
 * 실행 컨텍스트: submit_bio_noacct 경로
 */
int should_fail_bio(struct bio *bio)
{
	if (should_fail_request(bdev_whole(bio->bi_bdev), bio->bi_iter.bi_size)) /* [한국어] whole disk 기준 fault injection 검사 */
		return -EIO; /* [한국어] 인위적 -EIO 반환; 상위 계층의 오류 처리 경로 테스트 */
	return 0; /* [한국어] 정상: fault injection 미발생 */
}
ALLOW_ERROR_INJECTION(should_fail_bio, ERRNO);

/*
 * Check whether this bio extends beyond the end of the device or partition.
 * This may well happen - the kernel calls bread() without checking the size of
 * the device, e.g., when mounting a file system.
 */
/*
 * [한국어]
 * bio_check_eod - bio 가 장치(또는 파티션) 끝을 넘어서지 않는지 검사한다.
 *
 * @bio:    검사할 bio
 * @return: 0 = 정상 범위, -EIO = 장치 끝 초과
 *
 * NVMe namespace 의 마지막 LBA 를 초과하는 read/write 는 여기서 -EIO 로 거부된다.
 * 파일시스템이 마운트 시 장치 크기를 확인하지 않고 bread() 를 호출하는 경우에도
 * 이 검사가 안전 보호막 역할을 한다.
 * 실행 컨텍스트: submit_bio_noacct 경로 (프로세스 컨텍스트)
 */
static inline int bio_check_eod(struct bio *bio)
{
	sector_t maxsector = bdev_nr_sectors(bio->bi_bdev); /* [한국어] 장치/파티션 총 sector 수; NVMe namespace 마지막 LBA+1 */
	unsigned int nr_sectors = bio_sectors(bio); /* [한국어] 이 bio 의 sector 수 */

	if (nr_sectors && /* [한국어] sector 수 > 0 인 bio 만 검사; 0-sector bio(flush 등)는 통과 */
	    (nr_sectors > maxsector || /* [한국어] 요청 크기 자체가 장치 크기 초과 (언더플로우 방지) */
	     bio->bi_iter.bi_sector > maxsector - nr_sectors)) { /* [한국어] 시작 sector + 크기가 끝 초과; NVMe LBA 범위 위반 */
		if (!maxsector) /* [한국어] maxsector=0: 크기가 0인 장치(빈 네임스페이스 등) */
			return -EIO; /* [한국어] 빈 장치에 대한 접근 → -EIO */
		pr_info_ratelimited("%s: attempt to access beyond end of device\n"
				    "%pg: rw=%d, sector=%llu, nr_sectors = %u limit=%llu\n",
				    current->comm, bio->bi_bdev, bio->bi_opf,
				    bio->bi_iter.bi_sector, nr_sectors, maxsector); /* [한국어] 범위 초과 접근 정보 출력 (ratelimited) */
		return -EIO; /* [한국어] LBA 범위 초과 → -EIO 반환 */
	}
	return 0; /* [한국어] 정상 범위 내; I/O 계속 진행 */
}

/*
 * Remap block n of partition p to block n+start(p) of the disk.
 */
/*
 * [한국어]
 * blk_partition_remap - 파티션 상대 sector 를 전체 디스크 절대 sector(LBA) 로 변환한다.
 *
 * @bio:    remap 할 bio (bi_bdev 가 파티션 block_device 를 가리킴)
 * @return: 0 = remap 성공, -EIO = fault injection 에 의한 실패
 *
 * 파티션 block_device 의 bd_start_sect 를 bi_sector 에 더해 NVMe 가 이해하는
 * 절대 LBA 로 변환한다. 예: /dev/nvme0n1p1 의 sector 0 → namespace 의 sector 2048.
 * BIO_REMAPPED 플래그를 설정하여 이중 변환을 방지한다.
 * sector 가 없는 bio(flush 등)는 sector 변환을 건너뛴다.
 * 실행 컨텍스트: submit_bio_noacct 경로
 *
 * 호출 체인:
 *   submit_bio_noacct → [blk_partition_remap] → trace_block_bio_remap
 */
static int blk_partition_remap(struct bio *bio)
{
	struct block_device *p = bio->bi_bdev; /* [한국어] bio 의 block_device; NVMe namespace 상의 파티션 */

	if (unlikely(should_fail_request(p, bio->bi_iter.bi_size))) /* [한국어] fault injection: 파티션 단위 인위적 실패 */
		return -EIO; /* [한국어] 인위적 -EIO 반환 */
	if (bio_sectors(bio)) { /* [한국어] sector 수 > 0 인 bio 만 sector 변환 수행 */
		bio->bi_iter.bi_sector += p->bd_start_sect; /* [한국어] 파티션 시작 sector 를 더해 절대 LBA 산출 */
		trace_block_bio_remap(bio, p->bd_dev, /* [한국어] bio remap tracepoint; 변환 전/후 sector 정보 기록 */
				      bio->bi_iter.bi_sector -
				      p->bd_start_sect); /* [한국어] 변환 전 원래 sector 를 역산하여 tracepoint 에 전달 */
	}
	bio_set_flag(bio, BIO_REMAPPED); /* [한국어] BIO_REMAPPED 플래그 설정; 이중 remap 방지 */
	return 0; /* [한국어] remap 성공 */
}

/*
 * Check write append to a zoned block device.
 */
/*
 * [한국어]
 * blk_check_zone_append - NVMe ZNS Zone Append bio 의 제약 조건을 검사한다.
 *
 * @q:   request_queue (ZNS limits 접근용)
 * @bio: Zone Append bio
 * @return: BLK_STS_OK = 조건 충족, BLK_STS_NOTSUPP = ZNS 아님, BLK_STS_IOERR = 제약 위반
 *
 * NVMe ZNS Zone Append 명령(opcode 0x7D)은 다음 조건을 모두 만족해야 한다:
 *   1) Zoned 장치일 것
 *   2) bio 의 시작 sector 가 sequential zone 의 시작점일 것
 *   3) bio 크기가 zone 경계를 넘지 않을 것 (chunk_sectors)
 *   4) bio 크기가 max_zone_append_sectors 를 넘지 않을 것
 * 조건을 만족하면 REQ_NOMERGE 를 설정하여 이 bio 가 다른 bio 와 합쳐지지 않게 한다.
 * 실행 컨텍스트: submit_bio_noacct 경로
 */
static inline blk_status_t blk_check_zone_append(struct request_queue *q,
						 struct bio *bio)
{
	int nr_sectors = bio_sectors(bio); /* [한국어] Zone Append 요청의 sector 수 */

	/* Only applicable to zoned block devices */
	if (!bdev_is_zoned(bio->bi_bdev)) /* [한국어] ZNS(Zoned Namespace) 장치가 아니면 미지원 */
		return BLK_STS_NOTSUPP; /* [한국어] BLK_STS_NOTSUPP: NVMe ZNS 미지원 장치 */

	/* The bio sector must point to the start of a sequential zone */
	if (!bdev_is_zone_start(bio->bi_bdev, bio->bi_iter.bi_sector)) /* [한국어] Zone Append 는 zone 시작점(WPTR 기준)에만 가능 */
		return BLK_STS_IOERR; /* [한국어] 정렬 위반 → I/O 오류 */

	/*
	 * Not allowed to cross zone boundaries. Otherwise, the BIO will be
	 * split and could result in non-contiguous sectors being written in
	 * different zones.
	 */
	if (nr_sectors > q->limits.chunk_sectors) /* [한국어] chunk_sectors = zone 크기; 이를 초과하면 zone crossing */
		return BLK_STS_IOERR; /* [한국어] zone 경계 초과 → I/O 오류 */

	/* Make sure the BIO is small enough and will not get split */
	if (nr_sectors > q->limits.max_zone_append_sectors) /* [한국어] NVMe 컨트롤러의 Zone Append 최대 크기 초과 검사 */
		return BLK_STS_IOERR; /* [한국어] 최대 append 크기 초과 → I/O 오류 */

	bio->bi_opf |= REQ_NOMERGE; /* [한국어] REQ_NOMERGE 설정: Zone Append bio 는 다른 요청과 merge 금지 */

	return BLK_STS_OK; /* [한국어] 모든 ZNS Zone Append 조건 충족 */
}

/*
 * [한국어]
 * __submit_bio - 단일 bio 를 실제 하위 block 드라이버 queue 로 전달한다.
 *
 * @bio: 전달할 bio
 *
 * BD_HAS_SUBMIT_BIO 플래그 여부에 따라 두 경로로 분기된다:
 *   - 미설정(NVMe 등 표준 blk-mq 장치): blk_mq_submit_bio() → nvme_queue_rq() → doorbell
 *   - 설정(DM, loopback 등): bio_queue_enter() 후 fops->submit_bio() 직접 호출
 * plug 를 임시로 설치하여 nanosecond 타임스탬프 캐싱(ktime 오버헤드 최소화)을 한다.
 * REQ_POLLED bio 이면서 장치가 poll 미지원이면 BLK_STS_NOTSUPP 로 즉시 종료한다.
 * 실행 컨텍스트: __submit_bio_noacct_mq 또는 __submit_bio_noacct 내부
 *
 * 호출 체인:
 *   __submit_bio_noacct_mq / __submit_bio_noacct → [__submit_bio]
 *   → blk_mq_submit_bio → nvme_queue_rq → nvme_submit_cmd(doorbell)
 */
static void __submit_bio(struct bio *bio)
{
	/* If plug is not used, add new plug here to cache nsecs time. */
	struct blk_plug plug; /* [한국어] 임시 plug; ktime 캐싱으로 nanosecond 오버헤드 최소화 */

	blk_start_plug(&plug); /* [한국어] plug 시작; 이 bio 처리 중 추가 bio 를 배치로 모을 수 있음 */

	if (!bdev_test_flag(bio->bi_bdev, BD_HAS_SUBMIT_BIO)) { /* [한국어] BD_HAS_SUBMIT_BIO 미설정 = 표준 blk-mq 경로(NVMe 등) */
		blk_mq_submit_bio(bio); /* [한국어] 표준 blk-mq 경로: blk_mq_get_request → nvme_queue_rq → doorbell */
	} else if (likely(bio_queue_enter(bio) == 0)) { /* [한국어] 커스텀 submit_bio 장치: queue enter 성공 시 진입 */
		struct gendisk *disk = bio->bi_bdev->bd_disk; /* [한국어] gendisk 획득; fops 접근 및 queue_exit 에 사용 */

		if ((bio->bi_opf & REQ_POLLED) &&
		    !(disk->queue->limits.features & BLK_FEAT_POLL)) { /* [한국어] poll bio 이지만 장치가 poll 미지원인 경우 */
			bio->bi_status = BLK_STS_NOTSUPP; /* [한국어] poll 미지원 → BLK_STS_NOTSUPP 설정 */
			bio_endio(bio); /* [한국어] 즉시 bio 완료 처리 */
		} else {
			disk->fops->submit_bio(bio); /* [한국어] 커스텀 submit_bio 호출; DM/loopback 등이 하위 NVMe 로 전달 */
		}
		blk_queue_exit(disk->queue); /* [한국어] bio_queue_enter 로 획득한 q_usage_counter 반납 */
	}

	blk_finish_plug(&plug); /* [한국어] plug flush; 배치로 모인 request 를 NVMe SQ 로 dispatch */
}

/*
 * The loop in this function may be a bit non-obvious, and so deserves some
 * explanation:
 *
 *  - Before entering the loop, bio->bi_next is NULL (as all callers ensure
 *    that), so we have a list with a single bio.
 *  - We pretend that we have just taken it off a longer list, so we assign
 *    bio_list to a pointer to the bio_list_on_stack, thus initialising the
 *    bio_list of new bios to be added.  ->submit_bio() may indeed add some more
 *    bios through a recursive call to submit_bio_noacct.  If it did, we find a
 *    non-NULL value in bio_list and re-enter the loop from the top.
 *  - In this case we really did just take the bio off the top of the list (no
 *    pretending) and so remove it from bio_list, and call into ->submit_bio()
 *    again.
 *
 * bio_list_on_stack[0] contains bios submitted by the current ->submit_bio.
 * bio_list_on_stack[1] contains bios that were submitted before the current
 *	->submit_bio(), but that haven't been processed yet.
 */
/*
 * [한국어]
 * __submit_bio_noacct - stacked block device 를 위한 재귀적 bio 제출 처리.
 *
 * @bio: 제출할 첫 번째 bio (bi_next == NULL 이어야 함)
 *
 * fops->submit_bio() 내부에서 다시 submit_bio_noacct() 를 호출하면 재귀가 발생한다.
 * 이 함수는 current->bio_list 를 스택 로컬 bio_list 로 설정하여 재귀 중 추가된
 * bio 를 인터럽트하지 않고 큐에 쌓은 뒤, 루프로 순차 처리함으로써 스택 오버플로를 방지한다.
 * bio_list_on_stack[0]: 현재 __submit_bio 가 생성한 새 bio
 * bio_list_on_stack[1]: 이전 레벨에서 아직 처리 안 된 bio
 * 하위 queue(lower)를 상위 queue(same)보다 먼저 처리하여 계층 순서를 유지한다.
 * NVMe 위에 DM-multipath/RAID 가 있을 때 이 경로를 거친다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (fops->submit_bio 가 있는 장치 경로)
 *
 * 호출 체인:
 *   submit_bio_noacct_nocheck → [__submit_bio_noacct] → __submit_bio (반복)
 */
static void __submit_bio_noacct(struct bio *bio)
{
	struct bio_list bio_list_on_stack[2]; /* [한국어] 스택 기반 bio 리스트; 재귀 중 추가된 bio 를 여기에 수집 */

	BUG_ON(bio->bi_next); /* [한국어] bi_next != NULL 이면 단일 bio 전제 위반; 개발 중 버그 검출 */

	bio_list_init(&bio_list_on_stack[0]); /* [한국어] 현재 레벨에서 생성된 새 bio 수집 리스트 초기화 */
	current->bio_list = bio_list_on_stack; /* [한국어] current->bio_list 를 스택 리스트로 교체; 재귀 감지 활성화 */

	do { /* [한국어] 재귀로 추가된 bio 를 포함하여 모두 처리할 때까지 반복 */
		struct request_queue *q = bdev_get_queue(bio->bi_bdev); /* [한국어] 현재 bio 의 request_queue; 레벨 비교 기준 */
		struct bio_list lower, same; /* [한국어] lower = 하위 queue의 bio, same = 동일 queue의 bio */

		/*
		 * Create a fresh bio_list for all subordinate requests.
		 */
		bio_list_on_stack[1] = bio_list_on_stack[0]; /* [한국어] 이전에 수집된 bio 를 [1] 에 보존 */
		bio_list_init(&bio_list_on_stack[0]); /* [한국어] [0] 초기화; 이번 __submit_bio 가 새로 생성할 bio 수집 */

		__submit_bio(bio); /* [한국어] bio 를 blk-mq 또는 fops->submit_bio 로 전달; 재귀 bio 가 [0] 에 쌓임 */

		/*
		 * Sort new bios into those for a lower level and those for the
		 * same level.
		 */
		bio_list_init(&lower); /* [한국어] 하위 레벨 bio 수집 리스트 초기화 */
		bio_list_init(&same); /* [한국어] 동일 레벨 bio 수집 리스트 초기화 */
		while ((bio = bio_list_pop(&bio_list_on_stack[0])) != NULL) /* [한국어] 이번에 새로 추가된 bio 순회 */
			if (q == bdev_get_queue(bio->bi_bdev)) /* [한국어] 동일 queue → 같은 레벨; DM 내부 bio 재시도 등 */
				bio_list_add(&same, bio); /* [한국어] same 리스트에 추가 */
			else /* [한국어] 다른 queue → 하위 레벨; DM → NVMe 등 계층 하강 */
				bio_list_add(&lower, bio); /* [한국어] lower 리스트에 추가 */

		/*
		 * Now assemble so we handle the lowest level first.
		 */
		bio_list_merge(&bio_list_on_stack[0], &lower); /* [한국어] 하위 레벨 bio 를 처리 큐 앞에 배치 */
		bio_list_merge(&bio_list_on_stack[0], &same); /* [한국어] 동일 레벨 bio 그 다음 배치 */
		bio_list_merge(&bio_list_on_stack[0], &bio_list_on_stack[1]); /* [한국어] 이전 레벨에서 보류된 bio 마지막 배치 */
	} while ((bio = bio_list_pop(&bio_list_on_stack[0]))); /* [한국어] 남은 bio 가 없으면 루프 종료 */

	current->bio_list = NULL; /* [한국어] bio_list 원복; 재귀 감지 비활성화 */
}

/*
 * [한국어]
 * __submit_bio_noacct_mq - blk-mq 전용 재귀적 bio 제출 처리.
 *
 * @bio: 제출할 첫 번째 bio
 *
 * BD_HAS_SUBMIT_BIO 가 없는 표준 blk-mq 장치(NVMe 등) 전용으로, 레벨 정렬이
 * 필요 없기 때문에 __submit_bio_noacct() 보다 단순하다.
 * current->bio_list 를 설정하여 재귀 중 추가된 bio 를 bio_list[0] 에 모은 뒤
 * 순차적으로 __submit_bio() 를 호출한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (NVMe 표준 submit 경로)
 *
 * 호출 체인:
 *   submit_bio_noacct_nocheck → [__submit_bio_noacct_mq] → __submit_bio (반복)
 *   → blk_mq_submit_bio → nvme_queue_rq → nvme_submit_cmd(doorbell)
 */
static void __submit_bio_noacct_mq(struct bio *bio)
{
	struct bio_list bio_list[2] = { }; /* [한국어] bio 수집 리스트; [0]=새 bio, [1]=미사용(mq 경로에선 불필요) */

	current->bio_list = bio_list; /* [한국어] current->bio_list 교체; 재귀 submit_bio 를 [0] 에 수집 */

	do { /* [한국어] 수집된 bio 를 모두 처리할 때까지 반복 */
		__submit_bio(bio); /* [한국어] blk_mq_submit_bio → nvme_queue_rq → doorbell */
	} while ((bio = bio_list_pop(&bio_list[0]))); /* [한국어] 재귀로 추가된 bio 를 꺼내 계속 처리 */

	current->bio_list = NULL; /* [한국어] bio_list 원복; 재귀 감지 비활성화 */
}

/*
 * [한국어]
 * submit_bio_noacct_nocheck - trace·cgroup 계정·plug 경로 분기 후 __submit_bio 호출.
 *
 * @bio:   제출할 bio
 * @split: true = split bio (리스트 앞에 추가), false = 일반 bio (리스트 뒤에 추가)
 *
 * blk_throtl_bio() 통과 이후 또는 throttle 미사용 경로에서 호출되며:
 *   1) blk-cgroup I/O 계정(blk_cgroup_bio_start) 시작
 *   2) block_bio_queue tracepoint 발행
 *   3) current->bio_list 유무에 따라 재귀 경로 또는 실제 submit 경로 선택
 * "nocheck" 는 submit_bio_noacct() 의 유효성 검사를 이미 통과한 bio 에만 사용함을 의미한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio_noacct 및 blk_throtl_bio 콜백 경로)
 *
 * 호출 체인:
 *   submit_bio_noacct → [submit_bio_noacct_nocheck] → __submit_bio_noacct_mq / __submit_bio_noacct
 */
void submit_bio_noacct_nocheck(struct bio *bio, bool split)
{
	blk_cgroup_bio_start(bio); /* [한국어] blk-cgroup I/O 계정 시작; cgroup 별 bytes/ios 통계 기록 */

	if (!bio_flagged(bio, BIO_TRACE_COMPLETION)) { /* [한국어] 아직 trace 완료 마크가 없는 bio 만 처리 */
		trace_block_bio_queue(bio); /* [한국어] block_bio_queue tracepoint; blktrace/perf 로 queue 진입 추적 */
		/*
		 * Now that enqueuing has been traced, we need to trace
		 * completion as well.
		 */
		bio_set_flag(bio, BIO_TRACE_COMPLETION); /* [한국어] 완료 시 block_bio_complete 를 발행하도록 마킹 */
	}

	/*
	 * We only want one ->submit_bio to be active at a time, else stack
	 * usage with stacked devices could be a problem.  Use current->bio_list
	 * to collect a list of requests submitted by a ->submit_bio method
	 * while it is active, and then process them after it returned.
	 */
	if (current->bio_list) { /* [한국어] 재귀 중 (이미 __submit_bio_noacct* 가 실행 중) */
		if (split) /* [한국어] split bio 는 처리 순서 보존을 위해 리스트 앞에 추가 */
			bio_list_add_head(&current->bio_list[0], bio); /* [한국어] split bio → [0] 리스트 맨 앞 삽입 */
		else
			bio_list_add(&current->bio_list[0], bio); /* [한국어] 일반 bio → [0] 리스트 뒤에 추가 */
	} else if (!bdev_test_flag(bio->bi_bdev, BD_HAS_SUBMIT_BIO)) { /* [한국어] 표준 blk-mq 장치(NVMe 등) */
		__submit_bio_noacct_mq(bio); /* [한국어] NVMe 표준 경로: blk_mq_submit_bio → nvme_queue_rq → doorbell */
	} else { /* [한국어] 커스텀 submit_bio 를 가진 stacked 장치(DM, loopback 등) */
		__submit_bio_noacct(bio); /* [한국어] stacked 장치 경로; 재귀 방지 + 레벨 정렬 처리 */
	}
}

/*
 * [한국어]
 * blk_validate_atomic_write_op_size - REQ_ATOMIC bio 의 크기 제약을 검사한다.
 *
 * @q:   request_queue (atomic write limits 접근용)
 * @bio: REQ_ATOMIC 플래그가 설정된 write bio
 * @return: BLK_STS_OK = 조건 충족, BLK_STS_INVAL = 크기 제약 위반
 *
 * NVMe Atomic Write 기능(NVMe 2.0 spec) 사용 시 bio 크기가
 * 최대 단위(atomic_write_unit_max)를 넘지 않고, 최소 단위(atomic_write_unit_min)
 * 의 배수여야 한다. 위반 시 BLK_STS_INVAL(-EINVAL) 을 반환한다.
 * 실행 컨텍스트: submit_bio_noacct 의 REQ_OP_WRITE 분기
 */
static blk_status_t blk_validate_atomic_write_op_size(struct request_queue *q,
						 struct bio *bio)
{
	if (bio->bi_iter.bi_size > queue_atomic_write_unit_max_bytes(q)) /* [한국어] atomic_write_unit_max 초과 검사 */
		return BLK_STS_INVAL; /* [한국어] 최대 atomic write 단위 초과 → EINVAL */

	if (bio->bi_iter.bi_size % queue_atomic_write_unit_min_bytes(q)) /* [한국어] atomic_write_unit_min 배수 정렬 검사 */
		return BLK_STS_INVAL; /* [한국어] 최소 단위 정렬 위반 → EINVAL */

	return BLK_STS_OK; /* [한국어] atomic write 크기 조건 모두 충족 */
}

/**
 * submit_bio_noacct - re-submit a bio to the block device layer for I/O
 * @bio:  The bio describing the location in memory and on the device.
 *
 * This is a version of submit_bio() that shall only be used for I/O that is
 * resubmitted to lower level drivers by stacking block drivers.  All file
 * systems and other upper level users of the block layer should use
 * submit_bio() instead.
 */
/*
 * [한국어]
 * submit_bio_noacct - 상위 계층 bio 의 유효성을 검사하고 하위로 전달한다.
 *
 * @bio: 제출할 bio (bi_bdev, bi_opf, bi_iter, bi_ioprio 등이 설정된 상태)
 *
 * stacking driver 가 하위 장치로 bio 를 재제출할 때 사용하는 함수이다.
 * (파일시스템 등 상위 계층은 submit_bio() 를 사용해야 한다.)
 * bio_op(bio) 를 switch 로 분류하여:
 *   REQ_OP_READ/WRITE → NVMe Read/Write command
 *   REQ_OP_FLUSH      → request 단위 flush (bio 로 직접 불가, not_supported)
 *   REQ_OP_DISCARD    → NVMe Dataset Management (Deallocate)
 *   REQ_OP_ZONE_APPEND → NVMe ZNS Zone Append
 *   REQ_OP_WRITE_ZEROES → NVMe Write Zeroes
 *   REQ_OP_ZONE_*     → NVMe ZNS Zone 관리 명령
 * 유효하지 않은 op 나 미지원 기능은 not_supported 레이블로 분기하여 상위에 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (might_sleep 포함)
 *
 * 호출 체인:
 *   submit_bio → [submit_bio_noacct] → submit_bio_noacct_nocheck → __submit_bio
 */
void submit_bio_noacct(struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev; /* [한국어] bio 의 target block_device; NVMe namespace */
	struct request_queue *q = bdev_get_queue(bdev); /* [한국어] bdev 의 request_queue; limits 조회에 사용 */
	blk_status_t status = BLK_STS_IOERR; /* [한국어] 초기값 IOERR; not_supported/end_io 기본 상태 */

	might_sleep(); /* [한국어] 이 함수가 sleep 할 수 있음을 lockdep 에 알림 */

	/*
	 * For a REQ_NOWAIT based request, return -EOPNOTSUPP
	 * if queue does not support NOWAIT.
	 */
	if ((bio->bi_opf & REQ_NOWAIT) && !bdev_nowait(bdev)) /* [한국어] NOWAIT bio 이지만 장치가 nowait 미지원 */
		goto not_supported; /* [한국어] BLK_STS_NOTSUPP(-EOPNOTSUPP) 로 즉시 종료 */

	if (bio_has_crypt_ctx(bio)) { /* [한국어] inline crypto(blk-crypto) ctx 가 있는 bio 검사 */
		if (WARN_ON_ONCE(!bio_has_data(bio))) /* [한국어] crypt ctx 있는데 data 없으면 비정상; WARN */
			goto end_io; /* [한국어] 비정상 bio → end_io 경로 */
		if (!blk_crypto_supported(bio)) /* [한국어] 장치가 inline crypto 미지원 */
			goto not_supported; /* [한국어] inline crypto 미지원 → BLK_STS_NOTSUPP */
	}

	if (should_fail_bio(bio)) /* [한국어] fault injection 검사; 인위적 실패 시 end_io */
		goto end_io;
	bio_check_ro(bio); /* [한국어] read-only 장치에 대한 쓰기 경고 검사 */
	if (!bio_flagged(bio, BIO_REMAPPED)) { /* [한국어] 아직 파티션 remap 이 안 된 bio 처리 */
		if (unlikely(bio_check_eod(bio))) /* [한국어] 장치 끝 초과 검사; NVMe namespace LBA 범위 */
			goto end_io; /* [한국어] 범위 초과 → -EIO */
		if (bdev_is_partition(bdev) && /* [한국어] 파티션 block_device 이면 sector remap 수행 */
		    unlikely(blk_partition_remap(bio))) /* [한국어] remap 실패(fault injection 등) 시 end_io */
			goto end_io;
	}

	/*
	 * Filter flush bio's early so that bio based drivers without flush
	 * support don't have to worry about them.
	 */
	if (op_is_flush(bio->bi_opf)) { /* [한국어] REQ_PREFLUSH 또는 REQ_FUA 가 설정된 flush bio 처리 */
		if (WARN_ON_ONCE(bio_op(bio) != REQ_OP_WRITE && /* [한국어] flush 는 WRITE 또는 ZONE_APPEND 에만 허용 */
				 bio_op(bio) != REQ_OP_ZONE_APPEND)) /* [한국어] 잘못된 flush 조합 경고 */
			goto end_io; /* [한국어] 잘못된 flush 조합 → end_io */
		if (!bdev_write_cache(bdev)) { /* [한국어] write cache 없는 장치(NVMe FUA 미지원 등)에서 flush 처리 */
			bio->bi_opf &= ~(REQ_PREFLUSH | REQ_FUA); /* [한국어] write cache 없으면 PREFLUSH/FUA 플래그 클리어 */
			if (!bio_sectors(bio)) { /* [한국어] flush 전용 bio (데이터 없음)이면 즉시 성공 처리 */
				status = BLK_STS_OK; /* [한국어] 상태 OK: write cache 없으니 flush 불필요 */
				goto end_io; /* [한국어] 성공 상태로 즉시 bio 완료 */
			}
		}
	}

	switch (bio_op(bio)) {
	case REQ_OP_READ: /* [한국어] REQ_OP_READ → NVMe Read command (opcode 0x02) */
		break;
	case REQ_OP_WRITE: /* [한국어] REQ_OP_WRITE → NVMe Write command (opcode 0x01) */
		if (bio->bi_opf & REQ_ATOMIC) { /* [한국어] REQ_ATOMIC: NVMe Atomic Write 기능 사용 요청 */
			status = blk_validate_atomic_write_op_size(q, bio); /* [한국어] atomic write 크기 제약 검사 */
			if (status != BLK_STS_OK) /* [한국어] 크기 제약 위반 시 end_io */
				goto end_io;
		}
		break;
	case REQ_OP_FLUSH:
		/*
		 * REQ_OP_FLUSH can't be submitted through bios, it is only
		 * synthetized in struct request by the flush state machine.
		 */
		goto not_supported; /* [한국어] REQ_OP_FLUSH 는 bio 로 직접 제출 불가; flush 상태 머신이 request 로 합성 */
	case REQ_OP_DISCARD: /* [한국어] REQ_OP_DISCARD → NVMe Dataset Management (Deallocate, opcode 0x09) */
		if (!bdev_max_discard_sectors(bdev)) /* [한국어] discard 최대 sector = 0이면 장치 미지원 */
			goto not_supported; /* [한국어] discard 미지원 → BLK_STS_NOTSUPP */
		break;
	case REQ_OP_SECURE_ERASE: /* [한국어] REQ_OP_SECURE_ERASE → NVMe Sanitize (ACMD: 0x84) 또는 secure erase */
		if (!bdev_max_secure_erase_sectors(bdev)) /* [한국어] secure erase 미지원 장치 검사 */
			goto not_supported; /* [한국어] secure erase 미지원 → BLK_STS_NOTSUPP */
		break;
	case REQ_OP_ZONE_APPEND: /* [한국어] REQ_OP_ZONE_APPEND → NVMe ZNS Zone Append (opcode 0x7D) */
		status = blk_check_zone_append(q, bio); /* [한국어] ZNS Zone Append 조건 (정렬·크기) 검사 */
		if (status != BLK_STS_OK) /* [한국어] 조건 위반 시 end_io */
			goto end_io;
		break;
	case REQ_OP_WRITE_ZEROES: /* [한국어] REQ_OP_WRITE_ZEROES → NVMe Write Zeroes (opcode 0x08) */
		if (!q->limits.max_write_zeroes_sectors) /* [한국어] write zeroes 미지원 장치 검사 */
			goto not_supported; /* [한국어] write zeroes 미지원 → BLK_STS_NOTSUPP */
		break;
	case REQ_OP_ZONE_RESET:      /* [한국어] NVMe ZNS Zone Reset 명령 */
	case REQ_OP_ZONE_OPEN:       /* [한국어] NVMe ZNS Zone Open 명령 */
	case REQ_OP_ZONE_CLOSE:      /* [한국어] NVMe ZNS Zone Close 명령 */
	case REQ_OP_ZONE_FINISH:     /* [한국어] NVMe ZNS Zone Finish 명령 */
	case REQ_OP_ZONE_RESET_ALL:  /* [한국어] NVMe ZNS Zone Reset All 명령 */
		if (!bdev_is_zoned(bio->bi_bdev)) /* [한국어] ZNS 장치가 아니면 미지원 */
			goto not_supported; /* [한국어] ZNS 미지원 → BLK_STS_NOTSUPP */
		break;
	case REQ_OP_DRV_IN:  /* [한국어] NVMe passthrough Admin/IO command 입력 */
	case REQ_OP_DRV_OUT: /* [한국어] NVMe passthrough Admin/IO command 출력 */
		/*
		 * Driver private operations are only used with passthrough
		 * requests.
		 */
		fallthrough; /* [한국어] bio 경로로는 passthrough 불가 → not_supported 로 fall through */
	default: /* [한국어] 위에 해당하지 않는 알 수 없는 op */
		goto not_supported; /* [한국어] 미지원 op → BLK_STS_NOTSUPP */
	}

	if (blk_throtl_bio(bio)) /* [한국어] blk-cgroup throttle 검사; 제한 초과 시 지연 후 나중에 재제출 */
		return; /* [한국어] throttle 이 bio 를 가져갔으므로 여기서 반환; 완료는 throttle 이 책임 */
	submit_bio_noacct_nocheck(bio, false); /* [한국어] 모든 검사 통과 후 실제 submit 경로로 전달 */
	return; /* [한국어] 정상 반환 */

not_supported: /* [한국어] 미지원 op/기능 레이블 */
	status = BLK_STS_NOTSUPP; /* [한국어] BLK_STS_NOTSUPP(-EOPNOTSUPP) 설정 */
end_io: /* [한국어] 오류 완료 레이블 */
	bio->bi_status = status; /* [한국어] bio 에 최종 상태 코드 설정 */
	bio_endio(bio); /* [한국어] bi_end_io 콜백 호출; 상위 계층에 I/O 완료(오류) 통보 */
}
EXPORT_SYMBOL(submit_bio_noacct);

/*
 * [한국어]
 * bio_set_ioprio - bio 의 I/O 우선순위(ioprio)를 설정한다.
 *
 * @bio: ioprio 를 설정할 bio
 *
 * bi_ioprio 가 IOPRIO_CLASS_NONE 이면 현재 태스크의 nice 값 기반 ioprio 로 초기화한다.
 * 이후 blkcg_set_ioprio() 로 blk-cgroup 이 오버라이드할 수 있다.
 * ioprio 는 이후 blk-mq 스케줄러(BFQ, mq-deadline)나 NVMe 드라이버의 우선순위
 * 처리(WRR: Weighted Round Robin)에 반영될 수 있다.
 * 실행 컨텍스트: submit_bio 경로 (프로세스 컨텍스트)
 *
 * 호출 체인:
 *   submit_bio → [bio_set_ioprio] → get_current_ioprio + blkcg_set_ioprio
 */
static void bio_set_ioprio(struct bio *bio)
{
	/* Nobody set ioprio so far? Initialize it based on task's nice value */
	if (IOPRIO_PRIO_CLASS(bio->bi_ioprio) == IOPRIO_CLASS_NONE) /* [한국어] ioprio 미설정 시 task nice 기반 우선순위 산출 */
		bio->bi_ioprio = get_current_ioprio(); /* [한국어] 현재 태스크의 ioprio 를 bio 에 적용 */
	blkcg_set_ioprio(bio); /* [한국어] blk-cgroup ioprio 오버라이드; cgroup 설정이 태스크보다 우선 */
}

/**
 * submit_bio - submit a bio to the block device layer for I/O
 * @bio: The &struct bio which describes the I/O
 *
 * submit_bio() is used to submit I/O requests to block devices.  It is passed a
 * fully set up &struct bio that describes the I/O that needs to be done.  The
 * bio will be sent to the device described by the bi_bdev field.
 *
 * The success/failure status of the request, along with notification of
 * completion, is delivered asynchronously through the ->bi_end_io() callback
 * in @bio.  The bio must NOT be touched by the caller until ->bi_end_io() has
 * been called.
 */
/*
 * [한국어]
 * submit_bio - 파일시스템·페이지 캐시 등 상위 계층의 최상위 I/O 진입점.
 *
 * @bio: 제출할 bio (bi_bdev, bi_opf, bi_iter, bi_end_io 등이 완전히 설정된 상태)
 *
 * 모든 파일시스템 I/O 와 페이지 캐시 write 가 이 함수를 통해 블록 계층으로 진입한다.
 * (stacking driver 내부 재제출은 submit_bio_noacct() 를 사용해야 함)
 * 동작 순서:
 *   1) READ: task_io_account_read() + count_vm_events(PGPGIN) — 태스크·VM 통계
 *      WRITE: count_vm_events(PGPGOUT) — VM 통계
 *   2) bio_set_ioprio() — ioprio 설정
 *   3) submit_bio_noacct() — 유효성 검사·remap·throttle → 실제 submit
 * 실행 컨텍스트: 프로세스 컨텍스트 (파일시스템 write/readpage 경로)
 *
 * 호출 체인:
 *   ext4_readpages / generic_writepages → [submit_bio]
 *   → submit_bio_noacct → submit_bio_noacct_nocheck → __submit_bio
 *   → blk_mq_submit_bio → nvme_queue_rq → nvme_submit_cmd(doorbell)
 */
void submit_bio(struct bio *bio)
{
	if (bio_op(bio) == REQ_OP_READ) { /* [한국어] READ bio: 태스크 I/O 통계와 VM 이벤트 기록 */
		task_io_account_read(bio->bi_iter.bi_size); /* [한국어] 태스크별 read 바이트 누적; /proc/<pid>/io 반영 */
		count_vm_events(PGPGIN, bio_sectors(bio)); /* [한국어] PGPGIN VM 이벤트 증가; /proc/vmstat 반영 */
	} else if (bio_op(bio) == REQ_OP_WRITE) { /* [한국어] WRITE bio: VM 이벤트 기록 */
		count_vm_events(PGPGOUT, bio_sectors(bio)); /* [한국어] PGPGOUT VM 이벤트 증가; dirty page writeback 통계 */
	}

	bio_set_ioprio(bio); /* [한국어] task·cgroup ioprio 를 bio 에 설정 */
	submit_bio_noacct(bio); /* [한국어] 유효성 검사·remap·throttle 후 NVMe 경로로 전달 */
}
EXPORT_SYMBOL(submit_bio);

/**
 * bio_poll - poll for BIO completions
 * @bio: bio to poll for
 * @iob: batches of IO
 * @flags: BLK_POLL_* flags that control the behavior
 *
 * Poll for completions on queue associated with the bio. Returns number of
 * completed entries found.
 *
 * Note: the caller must either be the context that submitted @bio, or
 * be in a RCU critical section to prevent freeing of @bio.
 */
/*
 * [한국어]
 * bio_poll - bio 에 연결된 CQ(Completion Queue)를 소프트웨어 폴링하여 완료를 확인한다.
 *
 * @bio:   폴링할 bio (bi_cookie 로 CQ 식별)
 * @iob:   I/O 완료 배치 객체; 복수 완료를 일괄 처리
 * @flags: BLK_POLL_* 플래그 (NOSLEEP 등)
 * @return: 처리된 완료 항목 수 (0 = 없음)
 *
 * NVMe 폴 지원 장치에서는 blk_mq_poll() → nvme_poll_cq() 로 인터럽트 없이
 * CQ 링 버퍼를 직접 확인하여 완료 항목을 가져온다.
 * freeze 상태에서도 percpu_ref_tryget() 으로 진입하여 폴링 완료가 freeze 를
 * 막지 않도록 한다. (인터럽트 폴링과 달리 타임아웃 없음)
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 io_uring 폴링 스레드
 *
 * 호출 체인:
 *   io_uring iopoll → iocb_bio_iopoll → [bio_poll]
 *   → blk_mq_poll → nvme_poll_cq [drivers/nvme/host/pci.c]
 */
int bio_poll(struct bio *bio, struct io_comp_batch *iob, unsigned int flags)
{
	blk_qc_t cookie = READ_ONCE(bio->bi_cookie); /* [한국어] bi_cookie 원자 읽기; blk-mq 가 설정한 CQ 식별자 */
	struct block_device *bdev; /* [한국어] bio 의 block_device 포인터 */
	struct request_queue *q; /* [한국어] 폴링 대상 request_queue */
	int ret = 0; /* [한국어] 완료 항목 수 초기값 */

	bdev = READ_ONCE(bio->bi_bdev); /* [한국어] bi_bdev 원자 읽기; RCU 보호 하에 안전하게 접근 */
	if (!bdev) /* [한국어] bio 가 이미 해제된 경우 (슬랩 재할당); bi_bdev = NULL */
		return 0; /* [한국어] 폴링 불가 → 0 반환 */

	q = bdev_get_queue(bdev); /* [한국어] request_queue 획득; NVMe hctx 폴링 진입점 */
	if (cookie == BLK_QC_T_NONE) /* [한국어] bi_cookie = NONE: 이 bio 는 폴 불가 (non-polled 장치 등) */
		return 0; /* [한국어] 폴링 대상 없음 */

	blk_flush_plug(current->plug, false); /* [한국어] 미전송 plug 를 먼저 flush; 폴링 전 SQ 에 모든 요청 제출 */

	/*
	 * We need to be able to enter a frozen queue, similar to how
	 * timeouts also need to do that. If that is blocked, then we can
	 * have pending IO when a queue freeze is started, and then the
	 * wait for the freeze to finish will wait for polled requests to
	 * timeout as the poller is preventer from entering the queue and
	 * completing them. As long as we prevent new IO from being queued,
	 * that should be all that matters.
	 */
	if (!percpu_ref_tryget(&q->q_usage_counter)) /* [한국어] freeze 상태에서도 tryget 으로 진입 시도;
	                                               * 실패 시 폴링 포기 (freeze 진행 방해 금지) */
		return 0; /* [한국어] q_usage_counter 획득 실패 → 0 반환 */
	if (queue_is_mq(q)) { /* [한국어] blk-mq 기반 장치(NVMe 등): 표준 mq 폴링 경로 */
		ret = blk_mq_poll(q, cookie, iob, flags); /* [한국어] hctx 선택 → nvme_poll_cq → CQ 링 직접 확인 */
	} else { /* [한국어] bio 기반(fops->poll_bio) 장치 경로 */
		struct gendisk *disk = q->disk; /* [한국어] gendisk 획득; poll_bio 콜백 접근용 */

		if ((q->limits.features & BLK_FEAT_POLL) && disk && /* [한국어] 장치가 BLK_FEAT_POLL 지원 && disk 유효 */
		    disk->fops->poll_bio) /* [한국어] poll_bio 콜백이 구현되어 있으면 */
			ret = disk->fops->poll_bio(bio, iob, flags); /* [한국어] 장치별 poll_bio 호출; 완료 항목 수 반환 */
	}
	blk_queue_exit(q); /* [한국어] percpu_ref_tryget 으로 획득한 q_usage_counter 반납 */
	return ret; /* [한국어] 폴링에서 처리된 완료 항목 수 반환 */
}
EXPORT_SYMBOL_GPL(bio_poll);

/*
 * Helper to implement file_operations.iopoll.  Requires the bio to be stored
 * in iocb->private, and cleared before freeing the bio.
 */
/*
 * [한국어]
 * iocb_bio_iopoll - io_uring iopoll 경로의 bio 폴링 래퍼.
 *
 * @kiocb:  iocb (kiocb->private 에 bio 포인터 저장)
 * @iob:    I/O 완료 배치 객체
 * @flags:  BLK_POLL_* 플래그
 * @return: 처리된 완료 항목 수
 *
 * bio 슬랩은 SLAB_TYPESAFE_BY_RCU 를 사용하므로, RCU read lock 으로
 * bio 슬랩 재할당을 방지하면서 kiocb->private 에서 bio 를 읽는다.
 * bio 가 재할당되어 다른 장치를 가리키더라도 bio_poll() 내부에서 안전하게 처리된다.
 * 실행 컨텍스트: io_uring 폴링 태스크 컨텍스트
 *
 * 호출 체인:
 *   io_uring iopoll → [iocb_bio_iopoll] → bio_poll → blk_mq_poll → nvme_poll_cq
 */
int iocb_bio_iopoll(struct kiocb *kiocb, struct io_comp_batch *iob,
		    unsigned int flags)
{
	struct bio *bio; /* [한국어] kiocb->private 에서 읽은 bio 포인터; RCU 보호 하에 접근 */
	int ret = 0; /* [한국어] 완료 항목 수 초기값 */

	/*
	 * Note: the bio cache only uses SLAB_TYPESAFE_BY_RCU, so bio can
	 * point to a freshly allocated bio at this point.  If that happens
	 * we have a few cases to consider:
	 *
	 *  1) the bio is being initialized and bi_bdev is NULL.  We can just
	 *     simply nothing in this case
	 *  2) the bio points to a not poll enabled device.  bio_poll will catch
	 *     this and return 0
	 *  3) the bio points to a poll capable device, including but not
	 *     limited to the one that the original bio pointed to.  In this
	 *     case we will call into the actual poll method and poll for I/O,
	 *     even if we don't need to, but it won't cause harm either.
	 *
	 * For cases 2) and 3) above the RCU grace period ensures that bi_bdev
	 * is still allocated. Because partitions hold a reference to the whole
	 * device bdev and thus disk, the disk is also still valid.  Grabbing
	 * a reference to the queue in bio_poll() ensures the hctxs and requests
	 * are still valid as well.
	 */
	rcu_read_lock(); /* [한국어] RCU read lock; SLAB_TYPESAFE_BY_RCU bio 슬랩 재할당 방지 */
	bio = READ_ONCE(kiocb->private); /* [한국어] kiocb->private 에서 bio 원자 읽기; RCU 보호 필수 */
	if (bio) /* [한국어] bio 가 아직 유효하면 폴링 시도 */
		ret = bio_poll(bio, iob, flags); /* [한국어] bio_poll → blk_mq_poll → nvme_poll_cq */
	rcu_read_unlock(); /* [한국어] RCU read unlock; 이후 bio 슬랩 재할당 허용 */

	return ret; /* [한국어] 폴링 완료 항목 수 반환 */
}
EXPORT_SYMBOL_GPL(iocb_bio_iopoll);

/*
 * [한국어]
 * update_io_ticks - block_device 의 io_ticks 통계를 갱신한다.
 *
 * @part:  통계를 갱신할 block_device (파티션 또는 whole disk)
 * @now:   현재 jiffies 값
 * @end:   true = I/O 완료 시점, false = I/O 시작 시점
 *
 * bd_stamp 에 CAS(Compare-And-Swap)를 사용하여 io_ticks 를 원자적으로 갱신한다.
 * I/O 가 진행 중(inflight > 0)이거나 완료 시점(end=true)에만 ticks 를 증가시킨다.
 * 파티션인 경우 bdev_whole() 로 whole disk 포인터로 전환하여 재귀 없이 반복 처리한다.
 * /proc/diskstats 와 /sys/block/<disk>/stat 의 io_ticks 필드에 반영된다.
 * 실행 컨텍스트: part_stat_lock() 보호 하에 호출
 */
void update_io_ticks(struct block_device *part, unsigned long now, bool end)
{
	unsigned long stamp; /* [한국어] 이전 bd_stamp 값 저장용; CAS 입력으로 사용 */
again: /* [한국어] 파티션 → whole disk 으로 올라가는 반복 레이블 */
	stamp = READ_ONCE(part->bd_stamp); /* [한국어] 이전 타임스탬프 원자 읽기 */
	if (unlikely(time_after(now, stamp)) && /* [한국어] 시간이 경과했고 (now > stamp) */
	    likely(try_cmpxchg(&part->bd_stamp, &stamp, now)) && /* [한국어] CAS 성공(다른 CPU 와 경쟁 방지) */
	    (end || bdev_count_inflight(part))) /* [한국어] 완료 시점이거나 in-flight I/O 가 있는 경우에만 통계 갱신 */
		__part_stat_add(part, io_ticks, now - stamp); /* [한국어] io_ticks 에 경과 jiffies 추가 */

	if (bdev_is_partition(part)) { /* [한국어] 파티션이면 whole disk 통계도 갱신 */
		part = bdev_whole(part); /* [한국어] whole disk block_device 포인터로 변경 */
		goto again; /* [한국어] whole disk 에 대해 동일한 io_ticks 갱신 반복 */
	}
}

/*
 * [한국어]
 * bdev_start_io_acct - bdev 단위 I/O 계정을 시작한다.
 *
 * @bdev:       I/O 가 시작된 block_device
 * @op:         요청 연산 (REQ_OP_READ, REQ_OP_WRITE 등)
 * @start_time: I/O 시작 jiffies (호출 전 jiffies 값)
 * @return:     start_time 그대로 반환; bdev_end_io_acct() 에 전달할 용도
 *
 * in_flight 카운터를 증가시키고 io_ticks 를 갱신한다.
 * blk-mq 경로에서는 blk_account_io_start() 를 통해 이 함수가 호출된다.
 * 반환된 start_time 은 bdev_end_io_acct() 에서 지속 시간 계산에 사용된다.
 * 실행 컨텍스트: blk-mq dispatch 경로 (softirq 또는 프로세스 컨텍스트)
 */
unsigned long bdev_start_io_acct(struct block_device *bdev, enum req_op op,
				 unsigned long start_time)
{
	part_stat_lock(); /* [한국어] per-CPU 통계 lock 획득 */
	update_io_ticks(bdev, start_time, false); /* [한국어] io_ticks 갱신: I/O 시작 시점, end=false */
	part_stat_local_inc(bdev, in_flight[op_is_write(op)]); /* [한국어] in_flight[0(read)/1(write)] 증가 */
	part_stat_unlock(); /* [한국어] per-CPU 통계 lock 해제 */

	return start_time; /* [한국어] 시작 시간 반환; 완료 시 bdev_end_io_acct() 의 인자로 전달 */
}
EXPORT_SYMBOL(bdev_start_io_acct);

/**
 * bio_start_io_acct - start I/O accounting for bio based drivers
 * @bio:	bio to start account for
 *
 * Returns the start time that should be passed back to bio_end_io_acct().
 */
/*
 * [한국어]
 * bio_start_io_acct - bio 기반 드라이버의 I/O 계정을 시작한다.
 *
 * @bio:    계정을 시작할 bio
 * @return: 시작 jiffies (bio_end_io_acct() 에 전달)
 *
 * jiffies 를 start_time 으로 하여 bdev_start_io_acct() 를 호출하는 편의 래퍼.
 * bio 기반 장치(fops->submit_bio) 나 NVMe passthrough 드라이버에서 사용한다.
 * 실행 컨텍스트: fops->submit_bio 경로 (프로세스 컨텍스트)
 */
unsigned long bio_start_io_acct(struct bio *bio)
{
	return bdev_start_io_acct(bio->bi_bdev, bio_op(bio), jiffies); /* [한국어] 현재 jiffies 를 시작 시간으로 I/O 계정 시작 */
}
EXPORT_SYMBOL_GPL(bio_start_io_acct);

/*
 * [한국어]
 * bdev_end_io_acct - bdev 단위 I/O 계정을 종료한다.
 *
 * @bdev:       I/O 가 완료된 block_device
 * @op:         완료된 요청 연산
 * @sectors:    완료된 sector 수
 * @start_time: bdev_start_io_acct() 가 반환한 시작 jiffies
 *
 * ios[sgrp], sectors[sgrp], nsecs[sgrp] 를 증가시키고 in_flight 를 감소시킨다.
 * /proc/diskstats 와 /sys/block/<disk>/stat 의 완료 통계를 갱신한다.
 * blk-mq 에서는 blk_account_io_done() 을 통해 호출된다.
 * 실행 컨텍스트: softirq 완료 컨텍스트 또는 프로세스 컨텍스트
 */
void bdev_end_io_acct(struct block_device *bdev, enum req_op op,
		      unsigned int sectors, unsigned long start_time)
{
	const int sgrp = op_stat_group(op); /* [한국어] op 를 통계 그룹(read/write/discard 등)으로 변환 */
	unsigned long now = READ_ONCE(jiffies); /* [한국어] 완료 시점 jiffies 원자 읽기 */
	unsigned long duration = now - start_time; /* [한국어] I/O 지속 시간(jiffies); nsecs 로 변환하여 기록 */

	part_stat_lock(); /* [한국어] per-CPU 통계 lock 획득 */
	update_io_ticks(bdev, now, true); /* [한국어] io_ticks 갱신: 완료 시점, end=true */
	part_stat_inc(bdev, ios[sgrp]); /* [한국어] 완료 I/O 횟수 증가 */
	part_stat_add(bdev, sectors[sgrp], sectors); /* [한국어] 완료 sector 수 누적 */
	part_stat_add(bdev, nsecs[sgrp], jiffies_to_nsecs(duration)); /* [한국어] 소요 시간(ns) 누적 */
	part_stat_local_dec(bdev, in_flight[op_is_write(op)]); /* [한국어] in_flight 감소; bdev_start_io_acct 와 짝 */
	part_stat_unlock(); /* [한국어] per-CPU 통계 lock 해제 */
}
EXPORT_SYMBOL(bdev_end_io_acct);

/*
 * [한국어]
 * bio_end_io_acct_remapped - remap 된 bio 의 I/O 계정을 원래 bdev 기준으로 종료한다.
 *
 * @bio:        완료된 bio (현재 bi_bdev 는 remap 이후의 bdev 를 가리킴)
 * @start_time: 시작 jiffies
 * @orig_bdev:  remap 전 원래 block_device (통계를 기록할 대상)
 *
 * DM/RAID 등 stacked 장치에서 bio 가 하위 장치로 remapped 된 경우,
 * 통계는 원래 장치(orig_bdev)에 기록해야 한다.
 * 실행 컨텍스트: softirq 완료 컨텍스트
 */
void bio_end_io_acct_remapped(struct bio *bio, unsigned long start_time,
			      struct block_device *orig_bdev)
{
	bdev_end_io_acct(orig_bdev, bio_op(bio), bio_sectors(bio), start_time); /* [한국어] remap 전 원래 bdev 에 완료 통계 기록 */
}
EXPORT_SYMBOL_GPL(bio_end_io_acct_remapped);

/**
 * blk_lld_busy - Check if underlying low-level drivers of a device are busy
 * @q : the queue of the device being checked
 *
 * Description:
 *    Check if underlying low-level drivers of a device are busy.
 *    If the drivers want to export their busy state, they must set own
 *    exporting function using blk_queue_lld_busy() first.
 *
 *    Basically, this function is used only by request stacking drivers
 *    to stop dispatching requests to underlying devices when underlying
 *    devices are busy.  This behavior helps more I/O merging on the queue
 *    of the request stacking driver and prevents I/O throughput regression
 *    on burst I/O load.
 *
 * Return:
 *    0 - Not busy (The request stacking driver should dispatch request)
 *    1 - Busy (The request stacking driver should stop dispatching request)
 */
/*
 * [한국어]
 * blk_lld_busy - 하위 드라이버(Low Level Driver)가 바쁜지 확인한다.
 *
 * @q:     확인할 request_queue
 * @return: 1 = 바쁨(dispatch 중단 권고), 0 = 여유 있음
 *
 * NVMe 드라이버가 mq_ops->busy 를 구현한 경우, SQ full/tag 부족 등
 * 장치 자원 부족 여부를 DM-multipath 같은 상위 스택에 알릴 수 있다.
 * 상위 스택은 이 값을 참고하여 dispatch 를 일시 중단하고 merge 를 늘린다.
 * 실행 컨텍스트: 임의 컨텍스트 (DM dispatch 경로)
 */
int blk_lld_busy(struct request_queue *q)
{
	if (queue_is_mq(q) && q->mq_ops->busy) /* [한국어] blk-mq 장치이고 busy 콜백이 구현된 경우 */
		return q->mq_ops->busy(q); /* [한국어] 드라이버의 busy 콜백 호출; NVMe SQ depth/tag 부족 여부 반환 */

	return 0; /* [한국어] busy 콜백 없으면 기본값 0 (여유 있음) */
}
EXPORT_SYMBOL_GPL(blk_lld_busy);

/*
 * [한국어]
 * kblockd_schedule_work - kblockd workqueue 에 work 를 예약한다.
 *
 * @work:   예약할 work_struct 포인터
 * @return: true = 새로 enqueue, false = 이미 큐에 있음
 *
 * kblockd_workqueue 는 WQ_MEM_RECLAIM|WQ_HIGHPRI 로 생성된 고우선순위 workqueue 이다.
 * NVMe 명령 타임아웃, blk-mq hctx unplug, plug flush 등의 지연 작업이
 * 이 workqueue 의 kblockd kthread 에서 실행된다.
 * 실행 컨텍스트: 임의 컨텍스트 (softirq 타이머 핸들러, IRQ handler 포함)
 */
int kblockd_schedule_work(struct work_struct *work)
{
	return queue_work(kblockd_workqueue, work); /* [한국어] kblockd workqueue 에 work 예약; HIGHPRI 로 빠른 처리 보장 */
}
EXPORT_SYMBOL(kblockd_schedule_work);

/*
 * [한국어]
 * kblockd_mod_delayed_work_on - 특정 CPU 에서 kblockd delayed work 의 실행 시간을 조정한다.
 *
 * @cpu:   작업을 실행할 CPU 번호 (-1 이면 현재 CPU)
 * @dwork: 조정할 delayed_work 포인터
 * @delay: 지연 시간 (jiffies)
 * @return: true = 새로 예약 또는 시간 변경, false = 이미 실행 중
 *
 * NVMe hctx 의 지연 dispatch 작업이 NVMe 컨트롤러 소속 CPU 에서 실행되도록
 * 예약하거나 만료 시간을 변경한다.
 * 실행 컨텍스트: 임의 컨텍스트
 */
int kblockd_mod_delayed_work_on(int cpu, struct delayed_work *dwork,
				unsigned long delay)
{
	return mod_delayed_work_on(cpu, kblockd_workqueue, dwork, delay); /* [한국어] 특정 CPU 에서 지연 실행될 work 등록/갱신 */
}
EXPORT_SYMBOL(kblockd_mod_delayed_work_on);

/*
 * [한국어]
 * blk_start_plug_nr_ios - blk_plug 를 초기화하고 현재 태스크에 등록한다.
 *
 * @plug:   초기화할 blk_plug 구조체 (호출자가 스택에 할당)
 * @nr_ios: 배치로 모을 최대 I/O 수; BLK_MAX_REQUEST_COUNT 로 상한 클리핑
 *
 * blk_start_plug() 의 확장판으로, 모을 I/O 수를 지정할 수 있다.
 * current->plug 에 이미 plug 가 있으면(중첩 plug) 새 plug 를 등록하지 않는다.
 * mq_list 에 request 를 모아 blk_finish_plug() 시 NVMe SQ doorbell 을 일괄 전달한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (프리엠션 가능)
 *
 * 호출 체인:
 *   blk_start_plug → [blk_start_plug_nr_ios] → tsk->plug = plug
 */
void blk_start_plug_nr_ios(struct blk_plug *plug, unsigned short nr_ios)
{
	struct task_struct *tsk = current; /* [한국어] 현재 태스크 구조체; plug 등록 대상 */

	/*
	 * If this is a nested plug, don't actually assign it.
	 */
	if (tsk->plug) /* [한국어] 중첩 plug: 이미 plug 가 등록된 경우 새 plug 무시 */
		return; /* [한국어] 중첩 plug 이면 외부 plug 유지 */

	plug->cur_ktime = 0; /* [한국어] ktime 캐시 초기화; 배치 시간 추적용 */
	rq_list_init(&plug->mq_list); /* [한국어] blk-mq request 배치 리스트 초기화; NVMe SQ 배치 저장소 */
	rq_list_init(&plug->cached_rqs); /* [한국어] 재사용 request 캐시 리스트 초기화 */
	plug->nr_ios = min_t(unsigned short, nr_ios, BLK_MAX_REQUEST_COUNT); /* [한국어] 최대 배치 I/O 수 설정; 상한 클리핑 */
	plug->rq_count = 0; /* [한국어] 현재 배치된 request 수 0 으로 초기화 */
	plug->multiple_queues = false; /* [한국어] 복수 queue 사용 여부 초기화; 단일 NVMe hctx 가정 */
	plug->has_elevator = false; /* [한국어] elevator(스케줄러) 사용 여부 초기화 */
	INIT_LIST_HEAD(&plug->cb_list); /* [한국어] unplug callback 리스트 초기화 */

	/*
	 * Store ordering should not be needed here, since a potential
	 * preempt will imply a full memory barrier
	 */
	tsk->plug = plug; /* [한국어] current->plug 에 등록; 이후 bio 제출 시 이 plug 에 request 수집 */
}

/**
 * blk_start_plug - initialize blk_plug and track it inside the task_struct
 * @plug:	The &struct blk_plug that needs to be initialized
 *
 * Description:
 *   blk_start_plug() indicates to the block layer an intent by the caller
 *   to submit multiple I/O requests in a batch.  The block layer may use
 *   this hint to defer submitting I/Os from the caller until blk_finish_plug()
 *   is called.  However, the block layer may choose to submit requests
 *   before a call to blk_finish_plug() if the number of queued I/Os
 *   exceeds %BLK_MAX_REQUEST_COUNT, or if the size of the I/O is larger than
 *   %BLK_PLUG_FLUSH_SIZE.  The queued I/Os may also be submitted early if
 *   the task schedules (see below).
 *
 *   Tracking blk_plug inside the task_struct will help with auto-flushing the
 *   pending I/O should the task end up blocking between blk_start_plug() and
 *   blk_finish_plug(). This is important from a performance perspective, but
 *   also ensures that we don't deadlock. For instance, if the task is blocking
 *   for a memory allocation, memory reclaim could end up wanting to free a
 *   page belonging to that request that is currently residing in our private
 *   plug. By flushing the pending I/O when the process goes to sleep, we avoid
 *   this kind of deadlock.
 */
/*
 * [한국어]
 * blk_start_plug - blk_plug 를 1개 I/O 기본값으로 초기화하고 task_struct 에 등록한다.
 *
 * @plug: 초기화할 blk_plug 구조체 (호출자 스택에 할당)
 *
 * blk_start_plug_nr_ios(plug, 1) 의 편의 래퍼이다.
 * 파일시스템·VM 등이 I/O 배치 제출 구간의 시작을 알리기 위해 호출하며,
 * blk_finish_plug() 와 반드시 짝을 이뤄야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트
 *
 * 호출 체인:
 *   ext4_writepages → [blk_start_plug] → blk_start_plug_nr_ios
 */
void blk_start_plug(struct blk_plug *plug)
{
	blk_start_plug_nr_ios(plug, 1); /* [한국어] nr_ios=1 로 blk_start_plug_nr_ios 호출; 기본 1 I/O 배치 시작 */
}
EXPORT_SYMBOL(blk_start_plug);

/*
 * [한국어]
 * flush_plug_callbacks - plug 에 등록된 unplug callback 을 모두 실행한다.
 *
 * @plug:          callback 을 실행할 blk_plug
 * @from_schedule: true = 태스크 schedule 시 호출, false = 명시적 finish_plug
 *
 * cb_list 에서 callback 을 꺼내 실행한다. callback 실행 중 새로운 callback 이
 * 추가될 수 있으므로 outer while 루프로 반복 처리한다.
 * elevator(BFQ 등)나 stacked 장치가 unplug 시 작업을 수행하기 위해 등록한다.
 * 실행 컨텍스트: __blk_flush_plug 내부 (프로세스 컨텍스트)
 */
static void flush_plug_callbacks(struct blk_plug *plug, bool from_schedule)
{
	LIST_HEAD(callbacks); /* [한국어] 임시 callback 리스트; cb_list 에서 옮겨와 실행 중 추가 방지 */

	while (!list_empty(&plug->cb_list)) { /* [한국어] cb_list 가 빌 때까지 반복; 실행 중 새 callback 추가 가능 */
		list_splice_init(&plug->cb_list, &callbacks); /* [한국어] cb_list 전체를 callbacks 로 이동 후 cb_list 초기화 */

		while (!list_empty(&callbacks)) { /* [한국어] callbacks 를 순회하여 각 callback 실행 */
			struct blk_plug_cb *cb = list_first_entry(&callbacks, /* [한국어] 첫 번째 callback entry 획득 */
							  struct blk_plug_cb,
							  list);
			list_del(&cb->list); /* [한국어] callbacks 리스트에서 제거 */
			cb->callback(cb, from_schedule); /* [한국어] unplug callback 실행; elevator/DM 이 등록한 작업 처리 */
		}
	}
}

/*
 * [한국어]
 * blk_check_plugged - plug 에 unplug callback 의 등록 여부를 확인하고, 없으면 추가한다.
 *
 * @unplug: 등록할 callback 함수 포인터
 * @data:   callback 의 식별 데이터
 * @size:   할당할 blk_plug_cb 파생 구조체 크기 (sizeof(*cb) 이상)
 * @return: 기존 또는 새로 할당된 blk_plug_cb; plug 없거나 할당 실패 시 NULL
 *
 * 동일한 (unplug, data) 쌍이 이미 등록되어 있으면 기존 cb 를 반환(중복 방지).
 * 없으면 GFP_ATOMIC 으로 size 바이트 할당 후 cb_list 에 추가한다.
 * elevator(BFQ) 나 writeback 등이 unplug 시 추가 처리를 등록하는 데 사용한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio 경로)
 */
struct blk_plug_cb *blk_check_plugged(blk_plug_cb_fn unplug, void *data,
				      int size)
{
	struct blk_plug *plug = current->plug; /* [한국어] 현재 태스크 plug; NULL 이면 배치 비활성 상태 */
	struct blk_plug_cb *cb; /* [한국어] 검색/할당된 callback 구조체 포인터 */

	if (!plug) /* [한국어] plug 없으면 callback 등록 불가 */
		return NULL; /* [한국어] 배치 비활성 상태 → NULL 반환 */

	list_for_each_entry(cb, &plug->cb_list, list) /* [한국어] 기존 callback 리스트 선형 탐색 */
		if (cb->callback == unplug && cb->data == data) /* [한국어] 동일 (callback, data) 쌍 발견 */
			return cb; /* [한국어] 중복 등록 방지; 기존 cb 반환 */

	/* Not currently on the callback list */
	BUG_ON(size < sizeof(*cb)); /* [한국어] size 가 base 구조체보다 작으면 버그; 개발 중 검출 */
	cb = kzalloc(size, GFP_ATOMIC); /* [한국어] 새 callback 구조체 할당; GFP_ATOMIC (submit 경로는 sleep 불가 가능) */
	if (cb) { /* [한국어] 할당 성공 시 초기화 후 등록 */
		cb->data = data; /* [한국어] callback 식별 데이터 설정 */
		cb->callback = unplug; /* [한국어] unplug 시 호출될 함수 포인터 설정 */
		list_add(&cb->list, &plug->cb_list); /* [한국어] plug callback 리스트 앞에 추가 */
	}
	return cb; /* [한국어] 새로 할당된 cb 반환; 할당 실패 시 NULL */
}
EXPORT_SYMBOL(blk_check_plugged);

/*
 * [한국어]
 * __blk_flush_plug - plug 에 모인 callback·request·cached_rqs 를 모두 flush 한다.
 *
 * @plug:          flush 할 blk_plug
 * @from_schedule: true = 태스크 schedule 시 호출, false = blk_finish_plug 에서 호출
 *
 * 동작 순서:
 *   1) cb_list 의 unplug callback 을 모두 실행 (flush_plug_callbacks)
 *   2) mq_list 의 request 를 NVMe SQ 로 dispatch (blk_mq_flush_plug_list)
 *   3) cached_rqs 를 해제 (from_schedule 이어도 강제 처리; freeze 차단 방지)
 *   4) cur_ktime 리셋, PF_BLOCK_TS 플래그 클리어
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_finish_plug 또는 schedule 직전)
 *
 * 호출 체인:
 *   blk_finish_plug → [__blk_flush_plug] → blk_mq_flush_plug_list
 *   → nvme_queue_rq → nvme_submit_cmd(doorbell)
 */
void __blk_flush_plug(struct blk_plug *plug, bool from_schedule)
{
	if (!list_empty(&plug->cb_list)) /* [한국어] unplug callback 이 등록된 경우 먼저 실행 */
		flush_plug_callbacks(plug, from_schedule); /* [한국어] elevator/DM 등의 unplug callback 호출 */
	blk_mq_flush_plug_list(plug, from_schedule); /* [한국어] mq_list 의 request 를 NVMe SQ 로 dispatch;
	                                               * nvme_queue_rq → nvme_submit_cmd(doorbell) */
	/*
	 * Unconditionally flush out cached requests, even if the unplug
	 * event came from schedule. Since we know hold references to the
	 * queue for cached requests, we don't want a blocked task holding
	 * up a queue freeze/quiesce event.
	 */
	if (unlikely(!rq_list_empty(&plug->cached_rqs))) /* [한국어] cached_rqs 가 남아있으면 무조건 해제 */
		blk_mq_free_plug_rqs(plug); /* [한국어] cached request 해제; freeze/quiesce 가 이 태스크에 막히지 않도록 */

	plug->cur_ktime = 0; /* [한국어] ktime 캐시 리셋 */
	current->flags &= ~PF_BLOCK_TS; /* [한국어] PF_BLOCK_TS 플래그 클리어; 블록 타임스탬프 캐싱 종료 */
}

/**
 * blk_finish_plug - mark the end of a batch of submitted I/O
 * @plug:	The &struct blk_plug passed to blk_start_plug()
 *
 * Description:
 * Indicate that a batch of I/O submissions is complete.  This function
 * must be paired with an initial call to blk_start_plug().  The intent
 * is to allow the block layer to optimize I/O submission.  See the
 * documentation for blk_start_plug() for more information.
 */
/*
 * [한국어]
 * blk_finish_plug - I/O 배치 제출 구간을 종료하고 plug 를 flush/해제한다.
 *
 * @plug: blk_start_plug() 로 시작된 blk_plug 포인터
 *
 * plug 가 현재 태스크의 plug 와 일치하는 경우에만 __blk_flush_plug() 를 호출하고
 * current->plug 를 NULL 로 클리어한다. (중첩 plug 지원: 내부 plug 는 무시)
 * NVMe 관점에서 이 시점에 mq_list 의 request 가 SQ 로 일괄 전달된다.
 * 실행 컨텍스트: 프로세스 컨텍스트
 *
 * 호출 체인:
 *   ext4_writepages → [blk_finish_plug] → __blk_flush_plug → nvme_queue_rq → doorbell
 */
void blk_finish_plug(struct blk_plug *plug)
{
	if (plug == current->plug) { /* [한국어] 현재 태스크의 active plug 인지 확인; 중첩 plug 는 무시 */
		__blk_flush_plug(plug, false); /* [한국어] plug flush; 모인 request 를 NVMe SQ 로 dispatch */
		current->plug = NULL; /* [한국어] current->plug 클리어; 이후 bio 는 직접 submit */
	}
}
EXPORT_SYMBOL(blk_finish_plug);

/*
 * [한국어]
 * blk_io_schedule - I/O 대기 중 hung task 탐지를 피하며 태스크를 sleep 시킨다.
 *
 * 장시간 NVMe I/O 대기(freeze, throttle 등)에서 hung_task watchdog 이
 * 오탐하지 않도록, sysctl_hung_task_timeout_secs 의 절반 시간마다 깨어나
 * io_schedule_timeout 을 반복 호출한다.
 * sysctl_hung_task_timeout_secs = 0 이면 io_schedule() 로 무기한 대기한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (I/O 대기 루프 내)
 *
 * 호출 체인:
 *   blk_mq_freeze_queue / throttle wait → [blk_io_schedule] → io_schedule_timeout
 */
void blk_io_schedule(void)
{
	/* Prevent hang_check timer from firing at us during very long I/O */
	unsigned long timeout = sysctl_hung_task_timeout_secs * HZ / 2; /* [한국어] hung task 타임아웃의 절반; 이 주기로 깨어나 watchdog 리셋 */

	if (timeout) /* [한국어] hung task 타임아웃이 설정된 경우 제한 시간 sleep */
		io_schedule_timeout(timeout); /* [한국어] timeout jiffies 동안 I/O sleep; hung task 오탐 방지 */
	else /* [한국어] 타임아웃 = 0: hung task 감시 비활성 → 무기한 sleep */
		io_schedule(); /* [한국어] 무기한 I/O sleep; I/O 완료 시 wake_up 됨 */
}
EXPORT_SYMBOL_GPL(blk_io_schedule);

/*
 * [한국어]
 * blk_dev_init - 블록 계층 부트 타임 초기화 함수.
 *
 * @return: 항상 0 (성공); 실패 시 panic
 *
 * 커널 부트 시 한 번 호출되어 블록 계층 전역 자원을 초기화한다:
 *   1) 컴파일 타임 검사: REQ_OP_BITS, REQ_FLAG_BITS 가 cmd_flags/bi_opf 에 맞는지
 *   2) kblockd_workqueue 생성 (WQ_MEM_RECLAIM|WQ_HIGHPRI)
 *   3) request_queue 슬랩 캐시 생성 (blk_requestq_cachep)
 *   4) /sys/kernel/debug/block debugfs 디렉터리 생성
 * kblockd_workqueue 생성 실패 시 panic 으로 부트 중단.
 * 실행 컨텍스트: 부트 시 __init 함수 (단일 CPU, 인터럽트 비활성)
 *
 * 호출 체인:
 *   start_kernel → [blk_dev_init] → alloc_workqueue + KMEM_CACHE + debugfs_create_dir
 */
int __init blk_dev_init(void)
{
	BUILD_BUG_ON((__force u32)REQ_OP_LAST >= (1 << REQ_OP_BITS)); /* [한국어] REQ_OP_LAST 가 REQ_OP_BITS 에 맞는지 컴파일 검사 */
	BUILD_BUG_ON(REQ_OP_BITS + REQ_FLAG_BITS > 8 * /* [한국어] request.cmd_flags 필드에 op+flag 가 모두 들어가는지 검사 */
			sizeof_field(struct request, cmd_flags));
	BUILD_BUG_ON(REQ_OP_BITS + REQ_FLAG_BITS > 8 * /* [한국어] bio.bi_opf 필드에 op+flag 가 모두 들어가는지 검사 */
			sizeof_field(struct bio, bi_opf));

	/* used for unplugging and affects IO latency/throughput - HIGHPRI */
	kblockd_workqueue = alloc_workqueue("kblockd", /* [한국어] "kblockd" workqueue 생성 */
					    WQ_MEM_RECLAIM | WQ_HIGHPRI, 0); /* [한국어] MEM_RECLAIM: 메모리 회수 경로에서도 동작; HIGHPRI: 저지연 처리 */
	if (!kblockd_workqueue) /* [한국어] workqueue 생성 실패 */
		panic("Failed to create kblockd\n"); /* [한국어] 커널 패닉; 블록 계층 없이는 부트 불가 */

	blk_requestq_cachep = KMEM_CACHE(request_queue, SLAB_PANIC); /* [한국어] request_queue 슬랩 캐시 생성; 실패 시 SLAB_PANIC */

	blk_debugfs_root = debugfs_create_dir("block", NULL); /* [한국어] /sys/kernel/debug/block 디렉터리 생성; NVMe hctx 상태 노출 */

	return 0; /* [한국어] 초기화 성공 */
}

/*
 * [한국어] NVMe 관점 핵심 요약 — blk-core.c 에서 NVMe 와의 연결 정리
 *
 * - block/blk-core.c 는 상위 bio 를 request_queue 로 받아 NVMe SQ 에 넣기
 *   위한 준비 단계(유효성 검사·remap·operation 분류·plug)를 수행한다.
 * - 주요 NVMe I/O 흐름:
 *   submit_bio → submit_bio_noacct → submit_bio_noacct_nocheck → __submit_bio
 *   → blk_mq_submit_bio [blk-mq.c] → blk_mq_get_request → nvme_queue_rq
 *   → nvme_submit_cmd(doorbell) [drivers/nvme/host/pci.c]
 * - q->limits 는 NVMe 최대 전송 크기·PRP/SGL·ZNS·poll 등 컨트롤러 제약을 담는다.
 * - q->nr_requests / q->timeout 은 NVMe SQ 깊이(tagset depth)·명령 타임아웃과 직결된다.
 * - blk_queue_enter/exit(), blk_queue_start_drain(), blk_sync_queue() 는 NVMe
 *   컨트롤러 reset/remove 시 in-flight I/O 를 안전하게 drain 하는 핵심 메커니즘이다.
 * - bio_poll() / blk_lld_busy() 는 NVMe polled 완료 확인 및 하위 SQ 부하 상태를
 *   상위 스택에 노출하는 연결점이다.
 * - blk_start_plug()/blk_finish_plug() 는 NVMe doorbell 호출 빈도를 최소화하기
 *   위한 per-task request 배치 메커니즘을 구현한다.
 */

