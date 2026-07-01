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
 * NVMe SSD 관점 파일 요약
 *
 * block/blk-core.c 는 파일시스템이나 상위 계층이 생성한 struct bio 단위 I/O 를
 * request_queue 로 받아들이고, 멀티큐(blk-mq) 경로를 통해 NVMe 드라이버가 실제
 * SQ(Submission Queue)에 넣을 수 있는 요청으로 변환하는 핵심 블록 계층이다.
 * 대표적인 NVMe I/O 흐름:
 *   submit_bio -> submit_bio_noacct -> __submit_bio -> blk_mq_submit_bio
 *   -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * 본 파일은 bio 의 유효성 검사, partition remap, plug, poll, timeout, 계정 처리를
 * 담당하며, NVMe 의 queue, doorbell, CID, SQ/CQ/PRP/SGL 용어는 원문 그대로 유지한다.
 * 이전/이후 연결: include/linux/blkdev.h, block/blk-mq.c, drivers/nvme/host/pci.c (추정)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-pm.h>
#include <linux/blk-integrity.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/kernel_stat.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/fault-inject.h>
#include <linux/list_sort.h>
#include <linux/delay.h>
#include <linux/ratelimit.h>
#include <linux/pm_runtime.h>
#include <linux/t10-pi.h>
#include <linux/debugfs.h>
#include <linux/bpf.h>
#include <linux/part_stat.h>
#include <linux/sched/sysctl.h>
#include <linux/blk-crypto.h>

#define CREATE_TRACE_POINTS
#include <trace/events/block.h>

#include "blk.h"
#include "blk-mq-sched.h"
#include "blk-pm.h"
#include "blk-cgroup.h"
#include "blk-throttle.h"
#include "blk-ioprio.h"

struct dentry *blk_debugfs_root; // block debugfs 루트; NVMe queue 상태 디버깅 정보가 여기 아래에 노출됨 (추정)

EXPORT_TRACEPOINT_SYMBOL_GPL(block_bio_remap);
EXPORT_TRACEPOINT_SYMBOL_GPL(block_rq_remap);
EXPORT_TRACEPOINT_SYMBOL_GPL(block_bio_complete);
EXPORT_TRACEPOINT_SYMBOL_GPL(block_split);
EXPORT_TRACEPOINT_SYMBOL_GPL(block_unplug);
EXPORT_TRACEPOINT_SYMBOL_GPL(block_rq_insert);

static DEFINE_IDA(blk_queue_ida); // request_queue ID 할당용 IDA; NVMe SQ/CQ 식별자 생성에 사용 (추정)

/*
 * For queue allocation
 */
static struct kmem_cache *blk_requestq_cachep; // request_queue 슬랩 캐시; NVMe SQ/CQ 연결을 위한 블록 계층 핸들 할당

/*
 * Controlling structure to kblockd
 */
static struct workqueue_struct *kblockd_workqueue; // kblockd workqueue; NVMe timeout/unplug/dispatch 지연 작업이 여기서 실행

/**
 * blk_queue_flag_set - atomically set a queue flag
 * @flag: flag to be set
 * @q: request queue
 */
/*
 * blk_queue_flag_set - request_queue 의 queue_flags 를 원자적으로 설정한다.
 * NVMe 컨트롤러 reset, 삭제 또는 기능 변경 시 QUEUE_FLAG_DEAD, QUEUE_FLAG_POLL
 * 등의 플래그를 설정하여 이후 I/O 진입을 제어한다.
 * 호출 경로(예): nvme_set_queue_count, nvme_reset_ctrl (추정)
 */
void blk_queue_flag_set(unsigned int flag, struct request_queue *q)
{
	set_bit(flag, &q->queue_flags);
	// q->queue_flags 의 원자적 비트 설정; NVMe 에서는 QUEUE_FLAG_DEAD/POLL 등에 사용
}
EXPORT_SYMBOL(blk_queue_flag_set);

/**
 * blk_queue_flag_clear - atomically clear a queue flag
 * @flag: flag to be cleared
 * @q: request queue
 */
/*
 * blk_queue_flag_clear - request_queue 의 queue_flags 를 원자적으로 해제한다.
 * NVMe에서 queue 를 다시 사용 가능 상태로 만들 때 dead/poll 등의 플래그를
 * 클리어한다.
 */
void blk_queue_flag_clear(unsigned int flag, struct request_queue *q)
{
	clear_bit(flag, &q->queue_flags);
	// q->queue_flags 의 원자적 비트 클리어; NVMe queue 상태 복구 시 사용
}
EXPORT_SYMBOL(blk_queue_flag_clear);

#define REQ_OP_NAME(name) [REQ_OP_##name] = #name
static const char *const blk_op_name[] = {
	REQ_OP_NAME(READ), // REQ_OP_READ -> NVMe Read opcode (0x02)
	REQ_OP_NAME(WRITE), // REQ_OP_WRITE -> NVMe Write opcode (0x01)
	REQ_OP_NAME(FLUSH), // REQ_OP_FLUSH -> NVMe Flush opcode (0x00)
	REQ_OP_NAME(DISCARD), // REQ_OP_DISCARD -> NVMe Dataset Management (Deallocate) opcode (0x09)
	REQ_OP_NAME(SECURE_ERASE), // REQ_OP_SECURE_ERASE -> NVMe Sanitize/Secure Erase 범주
	REQ_OP_NAME(ZONE_RESET), // REQ_OP_ZONE_RESET -> NVMe ZNS Zone Reset
	REQ_OP_NAME(ZONE_RESET_ALL), // REQ_OP_ZONE_RESET_ALL -> NVMe ZNS Zone Reset All
	REQ_OP_NAME(ZONE_OPEN), // REQ_OP_ZONE_OPEN -> NVMe ZNS Zone Open
	REQ_OP_NAME(ZONE_CLOSE), // REQ_OP_ZONE_CLOSE -> NVMe ZNS Zone Close
	REQ_OP_NAME(ZONE_FINISH), // REQ_OP_ZONE_FINISH -> NVMe ZNS Zone Finish
	REQ_OP_NAME(ZONE_APPEND), // REQ_OP_ZONE_APPEND -> NVMe ZNS Zone Append opcode (0x7D)
	REQ_OP_NAME(WRITE_ZEROES), // REQ_OP_WRITE_ZEROES -> NVMe Write Zeroes opcode (0x08)
	REQ_OP_NAME(DRV_IN), // REQ_OP_DRV_IN -> NVMe driver-specific passthrough (Admin/IO) input
	REQ_OP_NAME(DRV_OUT), // REQ_OP_DRV_OUT -> NVMe driver-specific passthrough output
};
#undef REQ_OP_NAME

/**
 * blk_op_str - Return the string "name" for an operation REQ_OP_name.
 * @op: a request operation.
 *
 * Convert a request operation REQ_OP_name into the string "name". Useful for
 * debugging and tracing BIOs and requests. For an invalid request operation
 * code, the string "UNKNOWN" is returned.
 */
/*
 * blk_op_str - enum req_op 값을 디버깅용 문자열로 변환한다.
 * NVMe 관련 trace 에서 READ/WRITE/FLUSH/DISCARD 등이 어떤 NVMe opcode 로
 * 매핑되는지 확인할 때 참조된다.
 */
inline const char *blk_op_str(enum req_op op)
{
	const char *op_str = "UNKNOWN"; // 기본값 UNKNOWN; NVMe trace 에서 인식 불가 opcode 로 표시

	if (op < ARRAY_SIZE(blk_op_name) && blk_op_name[op]) // opcode 배열 범위 및 NULL 검사; 잘못된 NVMe op mapping 방지
		op_str = blk_op_name[op]; // 유효한 경우 해당 NVMe op 문자열 반환

	return op_str; // 변환 결과 반환; NVMe tracepoint 의 인자로 사용 (추정)
}
EXPORT_SYMBOL_GPL(blk_op_str);

static const struct {
	int		errno;
	const char	*name;
} blk_errors[] = {
	[BLK_STS_OK]		= { 0,		"" }, // 정상 완료; NVMe CQE SC=0 에 대응
	[BLK_STS_NOTSUPP]	= { -EOPNOTSUPP, "operation not supported" },
	[BLK_STS_TIMEOUT]	= { -ETIMEDOUT,	"timeout" }, // NVMe 명령 timeout; CID 복구 또는 abort 시작
	[BLK_STS_NOSPC]		= { -ENOSPC,	"critical space allocation" },
	[BLK_STS_TRANSPORT]	= { -ENOLINK,	"recoverable transport" }, // 전송 계층 복구 가능 오류; NVMe CQ DNR=0 상태에 해당 (추정)
	[BLK_STS_TARGET]	= { -EREMOTEIO,	"critical target" }, // 타겟 치명적 오류; NVMe CQE SC/ASC 치명적 (추정)
	[BLK_STS_RESV_CONFLICT]	= { -EBADE,	"reservation conflict" },
	[BLK_STS_MEDIUM]	= { -ENODATA,	"critical medium" },
	[BLK_STS_PROTECTION]	= { -EILSEQ,	"protection" },
	[BLK_STS_RESOURCE]	= { -ENOMEM,	"kernel resource" }, // 커널 자원 부족; NVMe tag/CID 할당 실패 가능
	[BLK_STS_DEV_RESOURCE]	= { -EBUSY,	"device resource" }, // 장치 자원 부족; NVMe SQ full 또는 queue depth 초과 (추정)
	[BLK_STS_AGAIN]		= { -EAGAIN,	"nonblocking retry" }, // nonblocking retry; REQ_NOWAIT NVMe I/O 재시도 불가시 반환 (추정)
	[BLK_STS_OFFLINE]	= { -ENODEV,	"device offline" }, // 장치 offline; NVMe controller dead 또는 queue DYING

	/* device mapper special case, should not leak out: */
	[BLK_STS_DM_REQUEUE]	= { -EREMCHG, "dm internal retry" },

	/* zone device specific errors */
	[BLK_STS_ZONE_OPEN_RESOURCE]	= { -ETOOMANYREFS, "open zones exceeded" }, // ZNS open zones 초과; NVMe Zone Resource 제약
	[BLK_STS_ZONE_ACTIVE_RESOURCE]	= { -EOVERFLOW, "active zones exceeded" }, // ZNS active zones 초과; NVMe Zone Resource 제약

	/* Command duration limit device-side timeout */
	[BLK_STS_DURATION_LIMIT]	= { -ETIME, "duration limit exceeded" }, // 명령 지속 시간 제한 초과; NVMe CDW20/21 duration limit (추정)

	[BLK_STS_INVAL]		= { -EINVAL,	"invalid" },

	/* everything else not covered above: */
	[BLK_STS_IOERR]		= { -EIO,	"I/O" }, // 기타 I/O 오류; NVMe 미분류 CQE 상태
};

/*
 * errno_to_blk_status - 커널 errno 를 blk_status_t 로 변환한다.
 * NVMe 완료 처리에서 nvme_complete_rq -> blk_mq_end_request 로 전달되는
 * 상태를 블록 계층 상태 코드로 바꾸는 데 사용된다 (추정).
 */
blk_status_t errno_to_blk_status(int errno)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(blk_errors); i++) { // errno -> blk_status_t 매핑 루프; NVMe 완료 상태 변환
		if (blk_errors[i].errno == errno) // 일치하는 errno 찾으면 해당 NVMe/블록 상태 반환
			return (__force blk_status_t)i; // 상태값 캐스팅 반환
	}

	return BLK_STS_IOERR; // 매핑 실패 시 BLK_STS_IOERR; NVMe unknown error 로 처리
}
EXPORT_SYMBOL_GPL(errno_to_blk_status);

/*
 * blk_status_to_errno - blk_status_t 를 사용자 공간으로 반환할 errno 로 변환한다.
 * NVMe I/O 오류가 파일시스템이나 상위 계층에 전달될 때 최종 errno 를 결정한다.
 */
int blk_status_to_errno(blk_status_t status)
{
	int idx = (__force int)status; // blk_status_t 를 정수로 변환

	if (WARN_ON_ONCE(idx >= ARRAY_SIZE(blk_errors))) // 범위 초과 방지; 잘못된 NVMe 상태 코드 검출
		return -EIO; // 범위 초과 시 -EIO 로 안전하게 반환
	return blk_errors[idx].errno; // NVMe I/O 오류에 대응하는 errno 반환
}
EXPORT_SYMBOL_GPL(blk_status_to_errno);

/*
 * blk_status_to_str - blk_status_t 를 디버깅용 문자열로 변환한다.
 * NVMe I/O 실패 시 dmesg/trace 에서 BLK_STS_TIMEOUT, BLK_STS_TRANSPORT 등의
 * 의미를 파악할 때 사용된다.
 */
const char *blk_status_to_str(blk_status_t status)
{
	int idx = (__force int)status; // blk_status_t 정수 인덱스 추출

	if (WARN_ON_ONCE(idx >= ARRAY_SIZE(blk_errors))) // 유효 범위 검사; 잘못된 NVMe 상태값 경고
		return "<null>"; // 범위 초과 시 null 문자열 반환
	return blk_errors[idx].name; // NVMe 상태를 사람이 읽을 수 있는 문자열로 반환
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
 * blk_sync_queue - request_queue 에 등록된 비동기 callback(타이머, work)을 취소한다.
 * NVMe 컨트롤러 제거/reset 시점에 in-flight I/O 의 timeout 타이머와 timeout_work
 * 가 더 이상 동작하지 않도록 막는다.
 * 호출 경로(예): nvme_shutdown_ctrl -> blk_sync_queue (추정)
 */
void blk_sync_queue(struct request_queue *q)
{
	timer_delete_sync(&q->timeout); // NVMe 명령 timeout 타이머를 동기적으로 제거; in-flight 명령 timeout 방지
	// NVMe 명령 timeout 타이머와 timeout_work 를 취소하여 제거 후에도 실행되지 않도록 함
	cancel_work_sync(&q->timeout_work); // NVMe timeout_work 취소; reset/remove 후 불필요한 abort 방지
}
EXPORT_SYMBOL(blk_sync_queue);

/**
 * blk_set_pm_only - increment pm_only counter
 * @q: request queue pointer
 */
/*
 * blk_set_pm_only - request_queue 의 pm_only 카운터를 증가시킨다.
 * NVMe runtime PM/ACPI 상태에서 queue 를 일시적으로 얼리고 I/O 진입을
 * 제한할 때 사용된다 (추정).
 */
void blk_set_pm_only(struct request_queue *q)
{
	atomic_inc(&q->pm_only); // pm_only 원자 증가; NVMe runtime PM 상태에서 queue 진입 제한
}
EXPORT_SYMBOL_GPL(blk_set_pm_only);

/*
 * blk_clear_pm_only - pm_only 카운터를 감소시키고 0이면 freeze 대기자를 깨운다.
 * NVMe 가 low-power 상태에서 깨어나 queue 를 다시 활성화할 때 사용된다 (추정).
 */
void blk_clear_pm_only(struct request_queue *q)
{
	int pm_only; // pm_only 값 로컬 저장

	pm_only = atomic_dec_return(&q->pm_only); // pm_only 원자 감소; 0이면 NVMe queue 재개 가능
	WARN_ON_ONCE(pm_only < 0); // 음수이면 버그; NVMe PM/reset 상태 불일치 감지
	if (pm_only == 0) // pm_only 0이면 freeze 대기자 깨움; NVMe queue resume 완료
		wake_up_all(&q->mq_freeze_wq); // mq_freeze_wq 에서 대기 중인 NVMe I/O 진입자들을 깨움
}
EXPORT_SYMBOL_GPL(blk_clear_pm_only);

/*
 * blk_free_queue_rcu - RCU grace period 이후 request_queue 메모리를 해제한다.
 * NVMe 장치가 사라진 뒤 request_queue, tagset, percpu_ref 를 안전하게
 * 반납하는 마지막 단계이다.
 */
static void blk_free_queue_rcu(struct rcu_head *rcu_head)
{
	struct request_queue *q = container_of(rcu_head, // RCU 콜백에서 request_queue 포인터 복원
			struct request_queue, rcu_head);

	percpu_ref_exit(&q->q_usage_counter); // percpu_ref 종료; NVMe queue 사용 카운터 정리
	kmem_cache_free(blk_requestq_cachep, q); // request_queue 슬랩 반납; NVMe SQ/CQ 연결 해제
}

/*
 * blk_free_queue - request_queue 의 모든 하위 자원을 해제하고 RCU 지연 해제를 예약한다.
 * NVMe 드라이버가 blk_cleanup_queue() 등을 통해 queue 를 제거할 때
 * blk_mq_release() 와 함께 tagset, hctx, stats 를 반납한다 (추정).
 */
static void blk_free_queue(struct request_queue *q)
{
	blk_free_queue_stats(q->stats); // queue 통계 해제; NVMe I/O accounting 정리
	if (queue_is_mq(q)) // mq queue 이면 하위 자원 해제
		blk_mq_release(q); // blk-mq 자원(tagset, hctx) 반납; NVMe SQ/CQ mapping 제거

	ida_free(&blk_queue_ida, q->id); // queue id 반납; NVMe queue 식별자 재사용
	lockdep_unregister_key(&q->io_lock_cls_key); // I/O lockdep key 등록 해제
	lockdep_unregister_key(&q->q_lock_cls_key); // queue lockdep key 등록 해제
	call_rcu(&q->rcu_head, blk_free_queue_rcu); // RCU grace period 후 메모리 해제; NVMe controller 삭제 안전성 확보
}

/**
 * blk_put_queue - decrement the request_queue refcount
 * @q: the request_queue structure to decrement the refcount for
 *
 * Decrements the refcount of the request_queue and free it when the refcount
 * reaches 0.
 */
/*
 * blk_put_queue - request_queue 의 참조 카운트를 감소시키고 0이면 해제한다.
 * NVMe queue 제거 시 refcount 가 0이 되면 blk_free_queue 로 이어진다.
 */
void blk_put_queue(struct request_queue *q)
{
	if (refcount_dec_and_test(&q->refs)) // refcount 감소; NVMe queue 마지막 사용자인지 확인
		blk_free_queue(q); // refcount 0이면 request_queue 해제 진행
}
EXPORT_SYMBOL(blk_put_queue);

/*
 * blk_queue_start_drain - queue 를 DYING 상태로 만들고 새 I/O 진입을 차단한다.
 * NVMe 컨트롤러 reset, remove, surprise removal 시 기존 I/O 를 마무리하면서
 * 새로운 submit_bio 가 -ENODEV 를 받도록 한다.
 */
bool blk_queue_start_drain(struct request_queue *q)
{
	/*
	 * When queue DYING flag is set, we need to block new req
	 * entering queue, so we call blk_freeze_queue_start() to
	 * prevent I/O from crossing blk_queue_enter().
	 */
	bool freeze = __blk_freeze_queue_start(q, current); // queue freeze 시작; NVMe reset/remove 시 신규 I/O 차단
	// queue 를 freeze 시작; NVMe reset/remove 시 새로운 blk_queue_enter 대기
	if (queue_is_mq(q)) // mq queue 이면 대기자를 깨워 DYING 상태 재검사
		blk_mq_wake_waiters(q); // blk_mq_wake_waiters; NVMe in-flight 요청 drain 유도
	// mq 대기자를 깨워 DYING 상태를 재검사하도록 함; nvme_remove_queues (추정)
	/* Make blk_queue_enter() reexamine the DYING flag. */
	wake_up_all(&q->mq_freeze_wq); // mq_freeze_wq 대기자 전체 깨움; NVMe queue state transition 알림

	return freeze; // freeze 성공 여부 반환; NVMe drain 절차에 사용
}

/**
 * blk_queue_enter() - try to increase q->q_usage_counter
 * @q: request queue pointer
 * @flags: BLK_MQ_REQ_NOWAIT and/or BLK_MQ_REQ_PM
 */
/*
 * blk_queue_enter - request_queue 사용 카운터를 획득하여 I/O 진입을 허용한다.
 * NVMe 정상 동작 시 submit_bio -> ... -> blk_queue_enter 로 queue 를 사용
 * 가능 상태로 만들고, dying/freeze 상태라면 대기 또는 -ENODEV/-EAGAIN 을
 * 반환한다. 이는 NVMe controller reset 중 I/O 유입을 제어하는 데 사용된다.
 */
int blk_queue_enter(struct request_queue *q, blk_mq_req_flags_t flags)
{
	const bool pm = flags & BLK_MQ_REQ_PM; // BLK_MQ_REQ_PM 플래그 추출; NVMe runtime PM resume 상태의 I/O 허용 여부

	while (!blk_try_enter_queue(q, pm)) { // queue 사용 카운터 획득 시도 루프; NVMe SQ 진입 전 필수
	// BLK_MQ_REQ_PM 플래그는 NVMe runtime PM/ACPI resume 상태의 I/O 허용 여부
		if (flags & BLK_MQ_REQ_NOWAIT) // NOWAIT 요청이면 대기 없이 -EAGAIN; NVMe nonblocking I/O 실패
			return -EAGAIN;

		/*
		 * read pair of barrier in blk_freeze_queue_start(), we need to
		 * order reading __PERCPU_REF_DEAD flag of .q_usage_counter and
		 * reading .mq_freeze_depth or queue dying flag, otherwise the
		 * following wait may never return if the two reads are
		 * reordered.
		 */
		smp_rmb(); // smp_rmb; freeze barrier 쌍으로 q_usage_counter DEAD 와 mq_freeze_depth/dying flag 읽기 순서 보장
		wait_event(q->mq_freeze_wq, // freeze_wq 에서 queue 해제 또는 dying 대기; NVMe controller 상태 안정화까지 block
			   (!q->mq_freeze_depth && // mq_freeze_depth 0이고 PM resume 가능할 때까지 대기
			    blk_pm_resume_queue(pm, q)) || // NVMe runtime PM queue resume 조건 검사
			   blk_queue_dying(q)); // QUEUE_FLAG_DYING 설정 시 깨어나 NVMe controller dead 처리
		if (blk_queue_dying(q)) // queue DYING 이면 -ENODEV; NVMe SQ/CQ 폐쇄됨
			return -ENODEV;
	}

	rwsem_acquire_read(&q->q_lockdep_map, 0, 0, _RET_IP_); // lockdep read acquire; NVMe queue enter 추적
	rwsem_release(&q->q_lockdep_map, _RET_IP_); // lockdep read release
	return 0; // queue 진입 성공; 이제 NVMe SQ 로 bio 전달 가능
}

/*
 * __bio_queue_enter - bio 경로에서 request_queue 사용 카운터를 획득한다.
 * NVMe에 직접 도달하기 전 bio 기반 장치의 GD_DEAD 상태와 queue freeze 를
 * 검사하여 비정상 상황에서 bio_io_error 를 반환한다.
 */
int __bio_queue_enter(struct request_queue *q, struct bio *bio)
{
	while (!blk_try_enter_queue(q, false)) { // bio 경로 queue 사용 카운터 획득 루프
		struct gendisk *disk = bio->bi_bdev->bd_disk; // bio 의 gendisk 획득; NVMe namespace disk 상태 확인

		if (bio->bi_opf & REQ_NOWAIT) { // REQ_NOWAIT bio 이면 즉시 반환 시도
			if (test_bit(GD_DEAD, &disk->state)) // GD_DEAD 비트 검사; NVMe controller 제거 완료 여부
				goto dead; // 디스크 dead 이면 dead 레이블로; NVMe abort 및 -ENODEV
			bio_wouldblock_error(bio); // NOWAIT + alive 이면 -EAGAIN; NVMe nonblocking would-block
			return -EAGAIN;
		}

		/*
		 * read pair of barrier in blk_freeze_queue_start(), we need to
		 * order reading __PERCPU_REF_DEAD flag of .q_usage_counter and
		 * reading .mq_freeze_depth or queue dying flag, otherwise the
		 * following wait may never return if the two reads are
		 * reordered.
		 */
		smp_rmb(); // smp_rmb; bio 경로에서도 freeze barrier 순서 보장
		wait_event(q->mq_freeze_wq, // freeze_wq 대기; NVMe queue freeze 해제 또는 GD_DEAD 대기
			   (!q->mq_freeze_depth && // mq_freeze_depth 0 조건
			    blk_pm_resume_queue(false, q)) || // PM resume 조건 (bio 경로)
			   test_bit(GD_DEAD, &disk->state)); // GD_DEAD 상태 대기 조건
		if (test_bit(GD_DEAD, &disk->state)) // GD_DEAD 이면 dead 경로; NVMe controller 사라짐
			goto dead;
	}

	rwsem_acquire_read(&q->io_lockdep_map, 0, 0, _RET_IP_); // bio 경로 lockdep acquire
	rwsem_release(&q->io_lockdep_map, _RET_IP_); // bio 경로 lockdep release
	return 0; // bio queue 진입 성공
dead:
	bio_io_error(bio); // NVMe I/O 실패로 bio 종료; 상위에 -ENODEV 반환
	return -ENODEV; // -ENODEV 반환; NVMe controller dead
}

/*
 * blk_queue_exit - request_queue 사용 카운터를 반납한다.
 * I/O 처리가 끝난 후 q_usage_counter 를 감소시켜 queue freeze 가 완료될 수
 * 있게 한다.
 */
void blk_queue_exit(struct request_queue *q)
{
	percpu_ref_put(&q->q_usage_counter); // queue 사용 카운터 반납; NVMe in-flight 감소, freeze 완료 조건
}

/*
 * blk_queue_usage_counter_release - q_usage_counter 가 0이 되면 대기자를 깨운다.
 * NVMe queue freeze/Drain 시 모든 in-flight I/O 가 종료되면 wake_up_all 로
 * freeze 완료를 알린다.
 */
static void blk_queue_usage_counter_release(struct percpu_ref *ref)
{
	struct request_queue *q = // percpu_ref 에서 request_queue 역참조
		container_of(ref, struct request_queue, q_usage_counter); // container_of 로 queue 포인터 복원

	wake_up_all(&q->mq_freeze_wq); // 사용 카운터 0이면 freeze_wq 대기자 깨움; NVMe drain 완료
}

/*
 * blk_rq_timed_out_timer - request_queue timeout 타이머의 softirq 콜백이다.
 * NVMe 명령이 q->timeout 이내에 CQ(Completion Queue)에서 완료되지 않으면
 * kblockd workqueue 에 timeout_work 를 예약한다 (추정).
 */
static void blk_rq_timed_out_timer(struct timer_list *t)
{
	struct request_queue *q = timer_container_of(q, t, timeout); // timeout 타이머에서 request_queue 복원

	kblockd_schedule_work(&q->timeout_work); // kblockd workqueue 에 timeout_work 예약; NVMe abort 처리 준비
}

/*
 * blk_timeout_work - timeout 처리 work 함수 (현재 이 커널 버전에서는 빈 함수).
 * NVMe timeout 처리는 nvme_timeout() 등 드라이버 레벨에서 주도될 수 있다 (추정).
 */
static void blk_timeout_work(struct work_struct *work)
{
}

/*
 * blk_alloc_queue - 새로운 request_queue 를 할당하고 초기화한다.
 * NVMe 드라이버가 blk_mq_init_queue() 등을 통해 호출하며, queue 의 limits,
 * timeout, nr_requests, freeze lock 등을 설정한다. 이 queue 는 이후
 * NVMe SQ/CQ 와 tagset 이 연결되는 블록 계층 핸들이 된다 (추정).
 */
struct request_queue *blk_alloc_queue(struct queue_limits *lim, int node_id)
{
	struct request_queue *q; // 새 request_queue 포인터
	int error; // 에러 코드

	q = kmem_cache_alloc_node(blk_requestq_cachep, GFP_KERNEL | __GFP_ZERO, // request_queue 슬랩 할당; NVMe SQ/CQ 연결 대상
				  node_id);
	if (!q) // 할당 실패 시 -ENOMEM; NVMe probe 실패
		return ERR_PTR(-ENOMEM);

	q->last_merge = NULL; // last_merge 초기화; NVMe request merge 후보 없음

	q->id = ida_alloc(&blk_queue_ida, GFP_KERNEL); // queue ID 할당; NVMe queue 번호 생성
	if (q->id < 0) { // ID 할당 실패 검사
		error = q->id; // IDA 오류 코드 저장
		goto fail_q; // 할당 실패 시 정리
	}

	q->stats = blk_alloc_queue_stats(); // queue 통계 구조체 할당
	if (!q->stats) { // 통계 할당 실패
		error = -ENOMEM; // -ENOMEM 설정
		goto fail_id; // fail_id 레이블로 이동
	}

	error = blk_set_default_limits(lim); // 기본 queue limits 설정
	if (error) // limits 초기화 실패 검사
		goto fail_stats; // fail_stats 레이블
	q->limits = *lim; // NVMe max transfer, segment, PRP/SGL, poll 등의 limits 복사
	// NVMe 최대 전송 크기, PRP/SGL 지원, atomic write, poll 기능 등의 limits 설정

	q->node = node_id; // NUMA node 저장; NVMe queue CPU/NUMA affinity 에 활용

	atomic_set(&q->nr_active_requests_shared_tags, 0); // shared tag 활성 요청 카운터 0 초기화; NVMe shared tag queue depth 기초

	timer_setup(&q->timeout, blk_rq_timed_out_timer, 0); // timeout 타이머 초기화; NVMe 명령 timeout 감시 시작
	// NVMe 명령 타임아웃 타이머 초기화; q->timeout 기간 내 CQ 미완료 시 timeout
	INIT_WORK(&q->timeout_work, blk_timeout_work); // timeout_work 초기화
	INIT_LIST_HEAD(&q->icq_list); // icq_list 초기화

	refcount_set(&q->refs, 1); // refs=1 초기화; NVMe queue 생명주기 시작
	mutex_init(&q->debugfs_mutex); // debugfs mutex 초기화
	mutex_init(&q->elevator_lock); // elevator mutex 초기화
	mutex_init(&q->sysfs_lock); // sysfs mutex 초기화
	mutex_init(&q->limits_lock); // limits mutex 초기화
	mutex_init(&q->rq_qos_mutex); // rq_qos mutex 초기화
	spin_lock_init(&q->queue_lock); // queue spinlock 초기화; NVMe queue state 보호

	init_waitqueue_head(&q->mq_freeze_wq); // freeze waitqueue 초기화; NVMe controller reset 대기 지점
	mutex_init(&q->mq_freeze_lock); // freeze lock 초기화

	blkg_init_queue(q); // blkcg queue 초기화; NVMe cgroup I/O 제어 연결

	/*
	 * Init percpu_ref in atomic mode so that it's faster to shutdown.
	 * See blk_register_queue() for details.
	 */
	error = percpu_ref_init(&q->q_usage_counter, // queue 사용 카운터(percpu_ref) 초기화
				blk_queue_usage_counter_release, // release 콜백 등록
				PERCPU_REF_INIT_ATOMIC, GFP_KERNEL); // atomic 모드; NVMe queue shutdown 가속
	if (error) // percpu_ref_init 실패
		goto fail_stats; // fail_stats로 이동
	// queue 사용 카운터(percpu_ref) 초기화; NVMe freeze 시 0이 될 때까지 대기
	lockdep_register_key(&q->io_lock_cls_key); // I/O lockdep key 등록
	lockdep_register_key(&q->q_lock_cls_key); // queue lockdep key 등록
	lockdep_init_map(&q->io_lockdep_map, "&q->q_usage_counter(io)", // I/O lockdep map 초기화
			 &q->io_lock_cls_key, 0); // I/O lockdep 클래스 설정
	lockdep_init_map(&q->q_lockdep_map, "&q->q_usage_counter(queue)", // queue lockdep map 초기화
			 &q->q_lock_cls_key, 0); // queue lockdep 클래스 설정

	/* Teach lockdep about lock ordering (reclaim WRT queue freeze lock). */
	fs_reclaim_acquire(GFP_KERNEL); // fs reclaim lockdep 표시
	rwsem_acquire_read(&q->io_lockdep_map, 0, 0, _RET_IP_); // queue enter lockdep acquire
	rwsem_release(&q->io_lockdep_map, _RET_IP_); // queue enter lockdep release
	fs_reclaim_release(GFP_KERNEL); // fs reclaim release

	q->nr_requests = BLKDEV_DEFAULT_RQ; // 최대 동시 요청 수; NVMe tagset depth/SQ 깊이와 연결
	// NVMe tagset/SQ 깊이와 관련된 최대 동시 요청 수 (추정)
	q->async_depth = BLKDEV_DEFAULT_RQ; // 비동기 dispatch 깊이; NVMe batch submission 시 doorbell 빈도에 영향 (추정)
	// 비동기 dispatch 가능 개수; NVMe SQ batch 시 doorbell 빈도에 영향 (추정)

	return q; // 초기화된 request_queue 반환; NVMe probe 성공

fail_stats: // fail_stats 레이블 시작
	blk_free_queue_stats(q->stats); // queue stats 해제
fail_id: // fail_id 레이블
	ida_free(&blk_queue_ida, q->id); // queue id 반납
fail_q: // fail_q 레이블
	kmem_cache_free(blk_requestq_cachep, q); // request_queue 슬랩 반납
	return ERR_PTR(error); // 에러 포인터 반환; NVMe controller 초기화 실패
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
 * blk_get_queue - request_queue 의 참조 카운트를 증가시킨다.
 * NVMe I/O 시작 전 queue 가 여전히 살아 있는지 확인하고 refcount 를 올린다.
 * queue 가 DYING 상태면 false 를 반환한다.
 */
bool blk_get_queue(struct request_queue *q)
{
	if (unlikely(blk_queue_dying(q))) // queue DYING/DEAD 검사; NVMe controller reset/remove 중이면 false
		return false; // queue가 죽어있으면 false; 신규 NVMe I/O 차단
	refcount_inc(&q->refs); // refcount 증가; NVMe queue 사용 보장
	return true; // refcount 획득 성공
}
EXPORT_SYMBOL(blk_get_queue);

#ifdef CONFIG_FAIL_MAKE_REQUEST

static DECLARE_FAULT_ATTR(fail_make_request);

static int __init setup_fail_make_request(char *str)
{
	return setup_fault_attr(&fail_make_request, str);
}
__setup("fail_make_request=", setup_fail_make_request);

/*
 * should_fail_request - fault injection 을 통해 요청 실패를 시뮬레이션한다.
 * NVMe 장치에서도 fail_make_request 파라미터가 설정된 경우 인위적으로 -EIO 를
 * 발생시켜 상위 스택의 오류 처리를 테스트한다.
 */
bool should_fail_request(struct block_device *part, unsigned int bytes)
{
	return bdev_test_flag(part, BD_MAKE_IT_FAIL) && // BD_MAKE_IT_FAIL 플래그 및 fault injection 조건
	       should_fail(&fail_make_request, bytes); // fault injection에 의한 -EIO 반환; NVMe 드라이버 테스트
}

static int __init fail_make_request_debugfs(void)
{
	struct dentry *dir = fault_create_debugfs_attr("fail_make_request",
						NULL, &fail_make_request);

	return PTR_ERR_OR_ZERO(dir);
}

late_initcall(fail_make_request_debugfs);
#endif /* CONFIG_FAIL_MAKE_REQUEST */

/*
 * bio_check_ro - read-only block device 에 대한 쓰기 bio 를 검사한다.
 * NVMe namespace 가 read-only 로 설정된 경우 쓰기 시도를 경고하고 막는다.
 */
static inline void bio_check_ro(struct bio *bio)
{
	if (op_is_write(bio_op(bio)) && bdev_read_only(bio->bi_bdev)) { // write 연산 && read-only bdev 검사; NVMe namespace read-only 쓰기 차단
		if (op_is_flush(bio->bi_opf) && !bio_sectors(bio)) // flush 이면서 데이터 없으면 허용; NVMe Flush-only bio 예외
			return;

		if (bdev_test_flag(bio->bi_bdev, BD_RO_WARNED)) // 이미 경고한 장치인지 검사
			return; // 경고 플래그 설정; NVMe read-only 쓰기 경고 중복 방지

		bdev_set_flag(bio->bi_bdev, BD_RO_WARNED);

		/*
		 * Use ioctl to set underlying disk of raid/dm to read-only
		 * will trigger this.
		 */
		pr_warn("Trying to write to read-only block-device %pg\n", // read-only 쓰기 경고 출력; NVMe namespace write protection
			bio->bi_bdev);
	}
}

/*
 * should_fail_bio - bio 단위 fault injection 검사.
 * NVMe 디바이스에서 fail_make_request 가 활성화된 경우 인위적으로 I/O 실패를
 * 유발한다.
 */
int should_fail_bio(struct bio *bio)
{
	if (should_fail_request(bdev_whole(bio->bi_bdev), bio->bi_iter.bi_size)) // bdev_whole 기준 fault injection 검사
		return -EIO; // -EIO 반환; NVMe I/O 실패 시뮬레이션
	return 0;
}
ALLOW_ERROR_INJECTION(should_fail_bio, ERRNO);

/*
 * Check whether this bio extends beyond the end of the device or partition.
 * This may well happen - the kernel calls bread() without checking the size of
 * the device, e.g., when mounting a file system.
 */
/*
 * bio_check_eod - bio 가 장치 끝을 넘어서지 않는지 검사한다.
 * NVMe namespace 의 마지막 LBA 를 초과하는 read/write 는 -EIO 로 거부된다.
 */
static inline int bio_check_eod(struct bio *bio)
{
	sector_t maxsector = bdev_nr_sectors(bio->bi_bdev); // 장치/파티션 총 sector 수 획득; NVMe namespace 마지막 LBA 경계
	unsigned int nr_sectors = bio_sectors(bio); // bio sector 수 계산

	if (nr_sectors && // sector 수가 0이 아니고 경계 초과 여부 검사
	    (nr_sectors > maxsector || // 요청 sector 수가 최대 sector 초과
	     bio->bi_iter.bi_sector > maxsector - nr_sectors)) { // 시작 sector가 maxsector - nr_sectors 보다 큼; NVMe namespace 넘어감
		if (!maxsector) // maxsector 0이면 -EIO
			return -EIO; // 빈 장치 접근 실패
		pr_info_ratelimited("%s: attempt to access beyond end of device\n"
				    "%pg: rw=%d, sector=%llu, nr_sectors = %u limit=%llu\n",
				    current->comm, bio->bi_bdev, bio->bi_opf,
				    bio->bi_iter.bi_sector, nr_sectors, maxsector);
		return -EIO; // -EIO 반환; NVMe namespace 범위 초과
	}
	return 0; // 정상 범위 내
}

/*
 * Remap block n of partition p to block n+start(p) of the disk.
 */
/*
 * blk_partition_remap - 파티션 내 sector 를 전체 디스크 sector 로 변환한다.
 * NVMe namespace 위의 파티션이 submit_bio 에 sector 0 으로 들어오면
 * bd_start_sect 를 더해 NVMe가 이해하는 절대 LBA 로 변환한다.
 */
static int blk_partition_remap(struct bio *bio)
{
	struct block_device *p = bio->bi_bdev; // bio 의 block_device 획득; NVMe namespace 상 파티션

	if (unlikely(should_fail_request(p, bio->bi_iter.bi_size))) // fault injection 검사
		return -EIO; // 인위적 -EIO
	if (bio_sectors(bio)) { // 데이터 sector가 있는 bio 만 remap
		bio->bi_iter.bi_sector += p->bd_start_sect; // 파티션 시작 sector 더함; NVMe namespace 절대 LBA 산출
		trace_block_bio_remap(bio, p->bd_dev, // bio remap tracepoint; NVMe LBA 변환 추적
				      bio->bi_iter.bi_sector -
				      p->bd_start_sect);
	}
	bio_set_flag(bio, BIO_REMAPPED); // BIO_REMAPPED 플래그 설정; NVMe 파티션 remap 완료 표시
	return 0; // remap 성공
}

/*
 * Check write append to a zoned block device.
 */
/*
 * blk_check_zone_append - zoned block device 의 zone append 조건을 검사한다.
 * NVMe ZNS(Zoned Namespace)에서 Zone Append 명령은 zone 시작점에 정렬되어야
 * 하고 zone 경계를 넘어서면 안 된다. 조건을 만족하면 REQ_NOMERGE 를 설정한다.
 */
static inline blk_status_t blk_check_zone_append(struct request_queue *q,
						 struct bio *bio)
{
	int nr_sectors = bio_sectors(bio); // bio sector 수

	/* Only applicable to zoned block devices */
	if (!bdev_is_zoned(bio->bi_bdev)) // zoned 장치가 아니면 NOTSUPP; NVMe ZNS 아님
		return BLK_STS_NOTSUPP; // ZNS 미지원 반환

	/* The bio sector must point to the start of a sequential zone */
	if (!bdev_is_zone_start(bio->bi_bdev, bio->bi_iter.bi_sector)) // zone 시작 sector 여부 검사; NVMe Zone Append는 zone 시작에 정렬
		return BLK_STS_IOERR; // 정렬 위반이면 I/O 오류

	/*
	 * Not allowed to cross zone boundaries. Otherwise, the BIO will be
	 * split and could result in non-contiguous sectors being written in
	 * different zones.
	 */
	if (nr_sectors > q->limits.chunk_sectors) // chunk_sectors 경계 초과 검사; NVMe ZNS zone crossing 금지
		return BLK_STS_IOERR; // zone crossing 오류

	/* Make sure the BIO is small enough and will not get split */
	if (nr_sectors > q->limits.max_zone_append_sectors) // max_zone_append_sectors 초과 검사; NVMe ZNS 최대 append 크기 제한
		return BLK_STS_IOERR; // append 크기 초과

	bio->bi_opf |= REQ_NOMERGE; // REQ_NOMERGE 설정; NVMe Zone Append merge 불가

	return BLK_STS_OK; // ZNS append 조건 만족
}

/*
 * __submit_bio - 단일 bio 를 실제 하위 queue 로 전달한다.
 * NVMe SSD에서는 대부분 blk_mq_submit_bio() 경로를 타며, 이후
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 로
 * SQ 에 쓰인다. BD_HAS_SUBMIT_BIO 가 설정된 장치는 직접 fops->submit_bio 를
 * 사용한다 (예: DM, loopback).
 * 호출 경로: submit_bio_noacct_nocheck -> submit_bio_noacct -> __submit_bio
 */
static void __submit_bio(struct bio *bio)
{
	/* If plug is not used, add new plug here to cache nsecs time. */
	struct blk_plug plug; // plug 구조체; NVMe SQ batch 제어용 (추정)

	blk_start_plug(&plug);
	// NVMe batch submission 을 위한 plug 시작; 이후 여러 bio 를 모을 수 있음

	if (!bdev_test_flag(bio->bi_bdev, BD_HAS_SUBMIT_BIO)) { // BD_HAS_SUBMIT_BIO 플래그로 custom submit_bio 여부 결정
		blk_mq_submit_bio(bio);
	// 표준 NVMe 경로: blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> doorbell
	} else if (likely(bio_queue_enter(bio) == 0)) {
		struct gendisk *disk = bio->bi_bdev->bd_disk; // gendisk 획득; NVMe namespace disk
	
		if ((bio->bi_opf & REQ_POLLED) &&
		    !(disk->queue->limits.features & BLK_FEAT_POLL)) {
			bio->bi_status = BLK_STS_NOTSUPP;
			bio_endio(bio);
		} else {
			disk->fops->submit_bio(bio);
	// 커스텀 submit_bio 를 가진 장치(DM 등)는 NVMe 상위에서 처리
		}
		blk_queue_exit(disk->queue);
	}

	blk_finish_plug(&plug);
	// plug 에 모인 request 를 flush; NVMe SQ 로의 batch doorbell 기회를 만듦
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
 * __submit_bio_noacct - stacked block device 를 위한 재귀적 bio 제출 처리.
 * ->submit_bio() 안에서 다시 submit_bio_noacct 를 호출할 때
 * current->bio_list 를 사용해 스택 오버플로우를 방지하며 하위/동일 레벨 bio 를
 * 정렬한다. NVMe 위에 DM/RAID 가 있을 때 이 경로를 거친다.
 */
static void __submit_bio_noacct(struct bio *bio)
{
	struct bio_list bio_list_on_stack[2];

	BUG_ON(bio->bi_next); // bio->bi_next는 NULL이어야 함; NVMe 하위 재귀 방지

	bio_list_init(&bio_list_on_stack[0]); // bio_list_on_stack[0] 초기화
	current->bio_list = bio_list_on_stack; // current->bio_list 를 스택 기반 리스트로 설정

	do { // 하위/동일 레벨 bio 처리 루프
		struct request_queue *q = bdev_get_queue(bio->bi_bdev); // bio 의 request_queue 획득; NVMe SQ 선택 기준
		struct bio_list lower, same; // 하위 레벨/동일 레벨 bio 분류 리스트

		/*
		 * Create a fresh bio_list for all subordinate requests.
		 */
		bio_list_on_stack[1] = bio_list_on_stack[0]; // 이전에 쌓인 bio 보존
		bio_list_init(&bio_list_on_stack[0]); // 새로 추가된 bio 리스트 초기화

		__submit_bio(bio); // bio 를 하위 queue 로 제출; blk_mq_submit_bio -> nvme_queue_rq -> doorbell

		/*
		 * Sort new bios into those for a lower level and those for the
		 * same level.
		 */
		bio_list_init(&lower); // 하위 레벨 bio 리스트 초기화
		bio_list_init(&same); // 동일 레벨 bio 리스트 초기화
		while ((bio = bio_list_pop(&bio_list_on_stack[0])) != NULL) // 재귀적으로 추가된 bio 순회; NVMe SQ 선택별 분류
			if (q == bdev_get_queue(bio->bi_bdev)) // 같은 queue 인지 비교; NVMe SQ/CQ 쌍 재사용 여부
				bio_list_add(&same, bio); // 동일 queue 이면 same 리스트
			else // 다른 queue 이면 lower 리스트; NVMe 하위 장치로 내려감
				bio_list_add(&lower, bio);

		/*
		 * Now assemble so we handle the lowest level first.
		 */
		bio_list_merge(&bio_list_on_stack[0], &lower); // lower bio 먼저 병합; NVMe 하위 queue 우선 dispatch
		bio_list_merge(&bio_list_on_stack[0], &same); // same 레벨 bio 병합
		bio_list_merge(&bio_list_on_stack[0], &bio_list_on_stack[1]); // 이전에 대기하던 bio 병합
	} while ((bio = bio_list_pop(&bio_list_on_stack[0]))); // 다음 처리할 bio pop; NVMe bio dispatch 순서 제어

	current->bio_list = NULL; // bio_list 정리; NVMe submit 재귀 종료
}

/*
 * __submit_bio_noacct_mq - 멀티큐 전용 재귀적 bio 제출 처리.
 * stacked NVMe 장치(DM-multipath 등)에서 bio 가 재귀적으로 추가되어도
 * mq_list 를 통해 NVMe 하위 queue 로 전달된다.
 */
static void __submit_bio_noacct_mq(struct bio *bio)
{
	struct bio_list bio_list[2] = { }; // bio_list[2] 선언; mq 경로 재귀 처리

	current->bio_list = bio_list; // current->bio_list 설정

	do { // bio 제출 루프; NVMe mq 경로
		__submit_bio(bio); // __submit_bio 호출; NVMe SQ 로 전달
	} while ((bio = bio_list_pop(&bio_list[0]))); // 남은 bio pop; NVMe SQ batch 계속

	current->bio_list = NULL; // bio_list 정리
}

/*
 * submit_bio_noacct_nocheck - bio 유효성 검사 후 throttle 과 trace 를 처리.
 * blk_throtl_bio() 로 cgroup 기반 제한을 적용하고, 제한을 통과하면
 * __submit_bio 를 통해 NVMe queue 로 전달한다.
 */
void submit_bio_noacct_nocheck(struct bio *bio, bool split)
{
	blk_cgroup_bio_start(bio); // blk-cgroup I/O 계정 시작; NVMe namespace cgroup 통계
	// blk-cgroup I/O accounting 시작; NVMe namespace 의 cgroup 통계에 반영

	if (!bio_flagged(bio, BIO_TRACE_COMPLETION)) {
		trace_block_bio_queue(bio); // bio queue tracepoint; NVMe submit_bio 진입 추적
		/*
		 * Now that enqueuing has been traced, we need to trace
		 * completion as well.
		 */
		bio_set_flag(bio, BIO_TRACE_COMPLETION);
	// completion trace 를 위해 BIO_TRACE_COMPLETION 설정; NVMe CQ 완료 추적
	}

	/*
	 * We only want one ->submit_bio to be active at a time, else stack
	 * usage with stacked devices could be a problem.  Use current->bio_list
	 * to collect a list of requests submitted by a ->submit_bio method
	 * while it is active, and then process them after it returned.
	 */
	if (current->bio_list) { // 재귀적 submit_bio 진행 중이면 bio_list 사용
		if (split)
			bio_list_add_head(&current->bio_list[0], bio); // split bio 는 리스트 앞에 추가; NVMe bio 순서 보존
	// 재귀적 submit_bio 를 current->bio_list 에 보관하여 스택 오버플로우 방지
		else
			bio_list_add(&current->bio_list[0], bio); // 일반 bio 리스트 뒤에 추가
	} else if (!bdev_test_flag(bio->bi_bdev, BD_HAS_SUBMIT_BIO)) { // custom submit_bio 없는 표준 block 장치
		__submit_bio_noacct_mq(bio); // 멀티큐 경로로 NVMe queue 에 전달
	// 멀티큐 경로로 NVMe queue 로 전달; __submit_bio -> blk_mq_submit_bio
	} else { // custom submit_bio 를 가진 stacked 장치 경로
		__submit_bio_noacct(bio);
	// stacked block device 경로; 최종적으로도 NVMe 하위 queue 로 도달
	}
}

static blk_status_t blk_validate_atomic_write_op_size(struct request_queue *q,
						 struct bio *bio)
{
	if (bio->bi_iter.bi_size > queue_atomic_write_unit_max_bytes(q)) // atomic write 최대 단위 초과 검사; NVMe atomic write limit
		return BLK_STS_INVAL; // 크기 초과로 invalid

	if (bio->bi_iter.bi_size % queue_atomic_write_unit_min_bytes(q)) // atomic write 최소 단위 배수 검사; NVMe atomic write alignment
		return BLK_STS_INVAL; // 정렬 위반이면 invalid

	return BLK_STS_OK; // atomic write 크기 조건 만족
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
 * submit_bio_noacct - 상위 계층 bio 의 유효성을 검사하고 NVMe에 적합한
 * 연산으로 분류한다.
 * bio_op(bio) 를 확인하여 READ/WRITE/FLUSH/DISCARD/ZONE_APPEND/WRITE_ZEROES
 * 등을 각각 NVMe opcode (Read, Write, Flush, Dataset Management, Zone Append,
 * Write Zeroes)에 대응시킨 뒤, 조건에 맞는 bio 만 하위로 전달한다.
 * 호출 경로: submit_bio -> submit_bio_noacct -> __submit_bio -> blk_mq_submit_bio
 */
void submit_bio_noacct(struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev; // bio 의 block_device; NVMe namespace
	struct request_queue *q = bdev_get_queue(bdev); // bdev 의 request_queue; NVMe SQ/CQ 연결 queue
	blk_status_t status = BLK_STS_IOERR; // 초기 상태 IOERR; NVMe 실패 가정

	might_sleep(); // sleep 가능성 표시

	/*
	 * For a REQ_NOWAIT based request, return -EOPNOTSUPP
	 * if queue does not support NOWAIT.
	 */
	if ((bio->bi_opf & REQ_NOWAIT) && !bdev_nowait(bdev)) // REQ_NOWAIT && bdev nowait 미지원이면 not_supported; NVMe nonblocking 거부
		goto not_supported;

	if (bio_has_crypt_ctx(bio)) { // 암호화 ctx 보유 bio 검사
		if (WARN_ON_ONCE(!bio_has_data(bio))) // 암호화 ctx 있는데 데이터 없으면 end_io
			goto end_io;
		if (!blk_crypto_supported(bio)) // 암호화 지원 안 하면 not_supported; NVMe inline crypto 미지원
			goto not_supported;
	}

	if (should_fail_bio(bio)) // fault injection 검사
		goto end_io; // 인위적 실패로 end_io
	bio_check_ro(bio); // read-only 검사
	if (!bio_flagged(bio, BIO_REMAPPED)) { // 아직 remap 안 된 bio 처리
		if (unlikely(bio_check_eod(bio))) // EOD 검사; NVMe namespace 끝 초과
			goto end_io; // 초과 시 end_io
		if (bdev_is_partition(bdev) && // 파티션이면 remap
		    unlikely(blk_partition_remap(bio))) // remap 실패 시 end_io
			goto end_io;
	}

	/*
	 * Filter flush bio's early so that bio based drivers without flush
	 * support don't have to worry about them.
	 */
	if (op_is_flush(bio->bi_opf)) { // flush 조건 처리
		if (WARN_ON_ONCE(bio_op(bio) != REQ_OP_WRITE && // flush 는 WRITE/ZONE_APPEND 외 연산과 함께 오면 경고
				 bio_op(bio) != REQ_OP_ZONE_APPEND)) // 연산 조합 검사
			goto end_io; // 잘못된 flush 조합 end_io
		if (!bdev_write_cache(bdev)) { // write cache 없는 장치
			bio->bi_opf &= ~(REQ_PREFLUSH | REQ_FUA); // PREFLUSH/FUA 플래그 클리어
			if (!bio_sectors(bio)) { // 데이터 sector 없으면
				status = BLK_STS_OK; // 상태 OK
				goto end_io; // 바로 완료; NVMe Flush 불필요
			}
		}
	}

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		break;
			// REQ_OP_READ -> NVMe Read command
	case REQ_OP_WRITE: // NVMe Write command; REQ_ATOMIC 은 atomic write 제약
		if (bio->bi_opf & REQ_ATOMIC) { // REQ_ATOMIC 설정 시 atomic write 크기 검증
			status = blk_validate_atomic_write_op_size(q, bio); // atomic write op size 검사
			if (status != BLK_STS_OK) // 검사 실패 시 end_io
				goto end_io;
		}
		break;
			// REQ_OP_WRITE -> NVMe Write command; REQ_ATOMIC 은 Compare and Write 또는 FUSE (추정)
	case REQ_OP_FLUSH:
		/*
		 * REQ_OP_FLUSH can't be submitted through bios, it is only
		 * synthetized in struct request by the flush state machine.
		 */
		goto not_supported;
			// REQ_OP_FLUSH 는 request 단위로 재구성되어 NVMe Flush 로 변환
	case REQ_OP_DISCARD: // NVMe Dataset Management(Deallocate) 연산
		if (!bdev_max_discard_sectors(bdev)) // discard 최대 sector 검사
			goto not_supported; // discard 미지원
			// REQ_OP_DISCARD -> NVMe Dataset Management (Deallocate)
		break;
	case REQ_OP_SECURE_ERASE: // NVMe Sanitize/Secure Erase 연산
		if (!bdev_max_secure_erase_sectors(bdev)) // secure erase 최대 sector 검사
			goto not_supported; // secure erase 미지원
		break;
			// REQ_OP_SECURE_ERASE -> NVMe Sanitize 또는 secure erase (추정)
	case REQ_OP_ZONE_APPEND: // NVMe ZNS Zone Append
		status = blk_check_zone_append(q, bio); // zone append 조건 검사
		if (status != BLK_STS_OK) // 검사 실패 시 end_io
			goto end_io;
		break;
			// REQ_OP_ZONE_APPEND -> NVMe ZNS Zone Append; zone 시작점 기준 검사 완료
	case REQ_OP_WRITE_ZEROES: // NVMe Write Zeroes
		if (!q->limits.max_write_zeroes_sectors) // write zeroes 최대 sector 검사
			goto not_supported; // write zeroes 미지원
		break;
			// REQ_OP_WRITE_ZEROES -> NVMe Write Zeroes command
	case REQ_OP_ZONE_RESET: // NVMe Zoned Namespace 관리 명령
	case REQ_OP_ZONE_OPEN:
	case REQ_OP_ZONE_CLOSE:
	case REQ_OP_ZONE_FINISH:
	case REQ_OP_ZONE_RESET_ALL:
		if (!bdev_is_zoned(bio->bi_bdev)) // zoned 장치가 아니면 not_supported
			goto not_supported; // ZNS 미지원
		break;
			// REQ_OP_ZONE_* -> NVMe Zoned Namespace Reset/Open/Close/Finish
	case REQ_OP_DRV_IN: // NVMe passthrough Admin/IO command (input)
	case REQ_OP_DRV_OUT: // NVMe passthrough Admin/IO command (output)
		/*
		 * Driver private operations are only used with passthrough
		 * requests.
		 */
		fallthrough;
			// REQ_OP_DRV_IN/DRV_OUT -> NVMe passthrough (Admin/IO) command
	default: // default: 지원하지 않는 연산
		goto not_supported; // not_supported 처리
	}

	if (blk_throtl_bio(bio)) // cgroup throttle; NVMe queue depth/io limit 지연 가능
		return; // throttle 에 의해 지연됨
	// blk-cgroup throttle; NVMe queue depth 제한이 있을 경우 여기서 지연
	submit_bio_noacct_nocheck(bio, false); // 검사 통과 후 NVMe SQ로 전달
	// throttle 통과 후 __submit_bio 를 통해 NVMe SQ 로의 여정 시작
	return; // submit_bio_noacct 반환

not_supported: // not_supported 레이블
	status = BLK_STS_NOTSUPP; // BLK_STS_NOTSUPP 설정
end_io: // end_io 레이블
	bio->bi_status = status; // bio 상태 설정
	bio_endio(bio); // bio 완료 콜백 호출; NVMe CQ 미경유 상위 완료
}
EXPORT_SYMBOL(submit_bio_noacct);

/*
 * bio_set_ioprio - bio 의 I/O 우선순위를 설정한다.
 * NVMe Weighted Round Robin 또는 cgroup 기반 우선순위가 구현된 경우
 * 이 값이 요청(rq)에 반영되어 NVMe queue 의 스케줄링에 영향을 줄 수 있다 (추정).
 */
static void bio_set_ioprio(struct bio *bio)
{
	/* Nobody set ioprio so far? Initialize it based on task's nice value */
	if (IOPRIO_PRIO_CLASS(bio->bi_ioprio) == IOPRIO_CLASS_NONE) // ioprio 미설정 시 task nice 기반 우선순위 산출
		bio->bi_ioprio = get_current_ioprio(); // task 우선순위를 bio ioprio 로 설정; NVMe WRR에 영향 (추정)
	blkcg_set_ioprio(bio); // cgroup ioprio 설정; NVMe queue 우선순위 반영 (추정)
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
 * submit_bio - 파일시스템/페이지 캐시 등 상위 계층의 최상위 I/O 진입점.
 * 통계를 기록하고 우선순위를 설정한 뒤 submit_bio_noacct 로 전달한다.
 * NVMe SSD에서 read/write 요청은 이 함수를 통해 blk-mq 로 들어간다.
 * 호출 경로: vfs_read -> ... -> submit_bio -> submit_bio_noacct -> __submit_bio
 *          -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
void submit_bio(struct bio *bio)
{
	if (bio_op(bio) == REQ_OP_READ) { // READ 연산 계정
		task_io_account_read(bio->bi_iter.bi_size); // read 바이트 계산; NVMe read throughput 통계
		count_vm_events(PGPGIN, bio_sectors(bio)); // read sector 수 기록
	} else if (bio_op(bio) == REQ_OP_WRITE) { // WRITE 연산 계정
		count_vm_events(PGPGOUT, bio_sectors(bio)); // write sector 수 기록; NVMe write throughput 통계
	}

	bio_set_ioprio(bio); // ioprio 설정
	submit_bio_noacct(bio);
	// bio_set_ioprio 이후 submit_bio_noacct 로 NVMe 경로 진입
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
 * bio_poll - 완료될 때까지 bio 에 대한 폴을 수행한다.
 * NVMe 폴 지원 장치에서는 blk_mq_poll() 을 통해 CQ(Completion Queue) 항목을
 * 소프트웨어로 직접 확인하며, NVMe 인터럽트 없이 완료를 가져온다 (추정).
 * 호출 경로: iocb_bio_iopoll -> bio_poll -> blk_mq_poll -> nvme_poll_cq (추정)
 */
int bio_poll(struct bio *bio, struct io_comp_batch *iob, unsigned int flags)
{
	blk_qc_t cookie = READ_ONCE(bio->bi_cookie); // bio cookie 원자 읽기; NVMe CQ 폴링 식별자
	struct block_device *bdev; // bdev 변수
	struct request_queue *q; // queue 변수
	int ret = 0; // 폴링 결과 카운트

	bdev = READ_ONCE(bio->bi_bdev); // bio 의 bdev 원자 읽기; NVMe namespace
	if (!bdev) // bdev NULL이면 0; bio 해제 후 폴 (추정)
		return 0;

	q = bdev_get_queue(bdev); // request_queue 획득; NVMe SQ/CQ
	if (cookie == BLK_QC_T_NONE) // cookie 가 NONE 이면 폴 대상 없음
		return 0; // 폴 불필요

	blk_flush_plug(current->plug, false); // plug 에 모인 request 먼저 flush; NVMe SQ pending 제출

	/*
	 * We need to be able to enter a frozen queue, similar to how
	 * timeouts also need to do that. If that is blocked, then we can
	 * have pending IO when a queue freeze is started, and then the
	 * wait for the freeze to finish will wait for polled requests to
	 * timeout as the poller is preventer from entering the queue and
	 * completing them. As long as we prevent new IO from being queued,
	 * that should be all that matters.
	 */
	if (!percpu_ref_tryget(&q->q_usage_counter)) // queue 사용 카운터 획득 시도; freeze 상태에서도 polled I/O 접근 허용
	// queue 사용 카운터 획득; NVMe polled I/O 도 freeze 상태에서 안전하게 접근
		return 0; // queue 사용 불가 시 0
	if (queue_is_mq(q)) { // mq queue 이면 표준 mq 폴
		ret = blk_mq_poll(q, cookie, iob, flags);
	// NVMe poll 경로: blk_mq_poll -> hctx 폴 -> nvme_poll_cq -> doorbell(CQ) (추정)
	} else { // non-mq 장치 경로
		struct gendisk *disk = q->disk; // gendisk 획득

		if ((q->limits.features & BLK_FEAT_POLL) && disk && // poll 지원 장치 && poll_bio 콜백 존재
		    disk->fops->poll_bio) // poll_bio 콜백 호출
			ret = disk->fops->poll_bio(bio, iob, flags); // 폴 완료 개수
	// bio 기반 poll_bio 콜백을 가진 장치용; NVMe 는 일반적으로 blk_mq_poll 사용
	}
	blk_queue_exit(q); // queue 사용 카운터 반납
	return ret; // 폴 결과 반환
}
EXPORT_SYMBOL_GPL(bio_poll);

/*
 * Helper to implement file_operations.iopoll.  Requires the bio to be stored
 * in iocb->private, and cleared before freeing the bio.
 */
/*
 * iocb_bio_iopoll - io_uring/iopoll 용 bio 폴 래퍼.
 * RCU 로 bio 를 보호한 뒤 bio_poll() 을 호출하여 NVMe CQ 를 폴한다.
 * 호출 경로: io_uring -> iocb_bio_iopoll -> bio_poll -> blk_mq_poll (추정)
 */
int iocb_bio_iopoll(struct kiocb *kiocb, struct io_comp_batch *iob,
		    unsigned int flags)
{
	struct bio *bio; // bio 포인터
	int ret = 0; // 반환값

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
	rcu_read_lock(); // RCU read lock; bio 슬랩 재할당 방지
	bio = READ_ONCE(kiocb->private); // kiocb->private 에서 bio 원자 읽기
	if (bio) // bio 존재 시 폴 수행
		ret = bio_poll(bio, iob, flags); // bio_poll 호출; NVMe CQ 직접 확인
	rcu_read_unlock(); // RCU read unlock

	return ret; // 폴 결과 반환
}
EXPORT_SYMBOL_GPL(iocb_bio_iopoll);

/*
 * update_io_ticks - io_ticks 통계를 갱신한다.
 * NVMe I/O 가 진행 중일 때 경과 시간을 기록하고, 파티션의 경우 상위
 * whole disk 통계까지 갱신한다.
 */
void update_io_ticks(struct block_device *part, unsigned long now, bool end)
{
	unsigned long stamp; // stamp 로컬 변수
again: // loop 레이블; NVMe 파티션/whole disk 통계 갱신
	stamp = READ_ONCE(part->bd_stamp); // part->bd_stamp 원자 읽기
	if (unlikely(time_after(now, stamp)) && // 시간 경과 && cmpxchg 성공 && (end 또는 in_flight) 조건
	    likely(try_cmpxchg(&part->bd_stamp, &stamp, now)) && // bd_stamp CAS; io_ticks 업데이트 경쟁 방지
	    (end || bdev_count_inflight(part))) // end 또는 inflight 조건
		__part_stat_add(part, io_ticks, now - stamp); // io_ticks 통계 추가; NVMe 장치 활동 시간

	if (bdev_is_partition(part)) { // 파티션이면 whole disk 로 전파
		part = bdev_whole(part); // whole disk 포인터로 변경
		goto again; // whole disk 통계 갱신 반복
	}
}

/*
 * bdev_start_io_acct - bdev 단위 I/O 계정 시작.
 * NVMe I/O 가 시작될 때 in_flight 와 io_ticks 를 갱신하여 sysfs/diskstats
 * 통계에 반영한다.
 */
unsigned long bdev_start_io_acct(struct block_device *bdev, enum req_op op,
				 unsigned long start_time)
{
	part_stat_lock(); // 통계 lock 획득
	update_io_ticks(bdev, start_time, false); // io_ticks 갱신; NVMe I/O 시작 시간
	part_stat_local_inc(bdev, in_flight[op_is_write(op)]); // in_flight 증가; NVMe 진행 중 I/O 카운트
	part_stat_unlock(); // 통계 lock 해제

	return start_time; // 시작 시간 반환
}
EXPORT_SYMBOL(bdev_start_io_acct);

/**
 * bio_start_io_acct - start I/O accounting for bio based drivers
 * @bio:	bio to start account for
 *
 * Returns the start time that should be passed back to bio_end_io_acct().
 */
/*
 * bio_start_io_acct - bio 기반 드라이버의 I/O 계정 시작.
 * NVMe passthrough 또는 bio 기반 NVMe 드라이버에서 jiffies 기반 시작 시간을
 * 획득한다.
 */
unsigned long bio_start_io_acct(struct bio *bio)
{
	return bdev_start_io_acct(bio->bi_bdev, bio_op(bio), jiffies); // bio 기준 I/O 계정 시작; NVMe passthrough/bio-based 시작 시간
}
EXPORT_SYMBOL_GPL(bio_start_io_acct);

/*
 * bdev_end_io_acct - bdev 단위 I/O 계정 종료.
 * NVMe I/O 완료 시 ios, sectors, nsecs, in_flight 를 갱신하여
 * /sys/block/nvme*n*/stat 등에 반영한다.
 */
void bdev_end_io_acct(struct block_device *bdev, enum req_op op,
		      unsigned int sectors, unsigned long start_time)
{
	const int sgrp = op_stat_group(op); // 통계 그룹 결정
	unsigned long now = READ_ONCE(jiffies); // 현재 jiffies 원자 읽기
	unsigned long duration = now - start_time; // I/O 지속 시간 계산; NVMe 명령 소요시간

	part_stat_lock(); // 통계 lock
	update_io_ticks(bdev, now, true); // io_ticks 종료 갱신
	part_stat_inc(bdev, ios[sgrp]); // 완료 횟수 증가
	part_stat_add(bdev, sectors[sgrp], sectors); // 완료 sector 수 추가
	part_stat_add(bdev, nsecs[sgrp], jiffies_to_nsecs(duration)); // 소요 시간(ns) 추가
	part_stat_local_dec(bdev, in_flight[op_is_write(op)]); // in_flight 감소
	part_stat_unlock(); // 통계 lock 해제
}
EXPORT_SYMBOL(bdev_end_io_acct);

/*
 * bio_end_io_acct_remapped - remapped bio 의 I/O 계정 종료.
 * NVMe remapped bio 가 완료되면 원래 bdev 에 대해 bdev_end_io_acct 를
 * 호출한다.
 */
void bio_end_io_acct_remapped(struct bio *bio, unsigned long start_time,
			      struct block_device *orig_bdev)
{
	bdev_end_io_acct(orig_bdev, bio_op(bio), bio_sectors(bio), start_time); // remapped bio 의 원래 bdev 에 대해 I/O 계정 종료
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
 * blk_lld_busy - 하위 장치(Low Level Driver)가 바쁜지 확인한다.
 * NVMe 드라이버가 mq_ops->busy 를 구현한 경우 큐 깊이/자원 부족 여부를
 * 상위 스택(DM 등)에 알려 dispatch 를 일시 중단할 수 있다.
 */
int blk_lld_busy(struct request_queue *q)
{
	if (queue_is_mq(q) && q->mq_ops->busy) // mq queue 이고 busy 콜백이 등록되어 있으면
		return q->mq_ops->busy(q); // NVMe 드라이버의 busy 콜백; queue depth/자원 부족 알림
	// NVMe 드라이버의 mq_ops->busy 콜백으로 하위 큐 부하 상태를 상위에 노출

	return 0; // busy 상태 아님; NVMe SQ 여유 있음
}
EXPORT_SYMBOL_GPL(blk_lld_busy);

/*
 * kblockd_schedule_work - kblockd workqueue 에 work 를 예약한다.
 * NVMe timeout 처리, unplug, plug flush 등의 지연 작업이 이 workqueue 를 통해
 * 실행된다.
 */
int kblockd_schedule_work(struct work_struct *work)
{
	return queue_work(kblockd_workqueue, work); // kblockd workqueue 에 work 예약; NVMe timeout/unplug 지연 실행
}
EXPORT_SYMBOL(kblockd_schedule_work);

/*
 * kblockd_mod_delayed_work_on - 지정 CPU 에서 kblockd delayed work 를 조정한다.
 * NVMe timeout 이나 hctx 의 지연 dispatch 가 특정 CPU 에서 실행되도록
 * 예약한다 (추정).
 */
int kblockd_mod_delayed_work_on(int cpu, struct delayed_work *dwork,
				unsigned long delay)
{
	return mod_delayed_work_on(cpu, kblockd_workqueue, dwork, delay); // 특정 CPU 에 delayed work 예약; NVMe timeout/hctx dispatch 지연 (추정)
}
EXPORT_SYMBOL(kblockd_mod_delayed_work_on);

/*
 * blk_start_plug_nr_ios - 현재 태스크에 blk_plug 를 설치해 I/O 를 모은다.
 * NVMe SQ에 doorbell 을 줄이기 위해 여러 bio/request 를 일정 시간/개수
 * 모아서 한꺼번에 dispatch 한다 (추정).
 */
void blk_start_plug_nr_ios(struct blk_plug *plug, unsigned short nr_ios)
{
	struct task_struct *tsk = current; // 현재 태스크 구조체

	/*
	 * If this is a nested plug, don't actually assign it.
	 */
	if (tsk->plug) // 중첩 plug 이면 무시
		return; // 중첩 plug 반환

	plug->cur_ktime = 0; // plug 시작 시간 0; NVMe batch timeout 기준
	rq_list_init(&plug->mq_list); // mq request 리스트 초기화; NVMe multi-queue batch
	rq_list_init(&plug->cached_rqs); // cached request 리스트 초기화
	plug->nr_ios = min_t(unsigned short, nr_ios, BLK_MAX_REQUEST_COUNT); // 최대 모을 수 있는 request 수; NVMe SQ batch 깊이 제한
	plug->rq_count = 0; // 현재 모인 request 수 0
	plug->multiple_queues = false; // 복수 queue 사용 여부 false; NVMe SQ 선택 변경 추적
	plug->has_elevator = false; // elevator 사용 여부 false
	INIT_LIST_HEAD(&plug->cb_list); // plug callback 리스트 초기화

	/*
	 * Store ordering should not be needed here, since a potential
	 * preempt will imply a full memory barrier
	 */
	tsk->plug = plug; // current->plug 에 plug 등록; NVMe batch 시작
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
 * blk_start_plug - blk_plug 를 초기화하고 task_struct 에 등록한다.
 * NVMe I/O batch 의 시작을 알리며, blk_finish_plug 와 짝을 이룬다.
 */
void blk_start_plug(struct blk_plug *plug)
{
	blk_start_plug_nr_ios(plug, 1); // 기본 1개 I/O batch 시작; NVMe SQ doorbell batch
}
EXPORT_SYMBOL(blk_start_plug);

/*
 * flush_plug_callbacks - plug 에 등록된 callback 을 모두 실행한다.
 * NVMe unplug 이벤트 발생 시 stacked device 나 스케줄러가 등록한 callback 을
 * schedule 여부에 따라 호출한다.
 */
static void flush_plug_callbacks(struct blk_plug *plug, bool from_schedule)
{
	LIST_HEAD(callbacks); // 임시 callback 리스트

	while (!list_empty(&plug->cb_list)) { // plug callback 이 남아있는 동안
		list_splice_init(&plug->cb_list, &callbacks); // cb_list 를 callbacks 로 옮김

		while (!list_empty(&callbacks)) { // callbacks 순회
			struct blk_plug_cb *cb = list_first_entry(&callbacks, // 첫 번째 callback entry 획득
							  struct blk_plug_cb,
							  list);
			list_del(&cb->list); // callback entry 제거
			cb->callback(cb, from_schedule); // unplug callback 실행; NVMe scheduler/elevator 이벤트 전파
		}
	}
}

/*
 * blk_check_plugged - plug 에 unplug callback 이 이미 등록되었는지 확인/추가한다.
 * NVMe unplug 시점에 중복 callback 등록을 방지하고, 필요 시 새 callback 을
 * 할당하여 추가한다.
 */
struct blk_plug_cb *blk_check_plugged(blk_plug_cb_fn unplug, void *data,
				      int size)
{
	struct blk_plug *plug = current->plug; // 현재 태스크 plug 획득
	struct blk_plug_cb *cb; // callback 변수

	if (!plug) // plug 없으면 callback 등록 불가
		return NULL; // NULL 반환

	list_for_each_entry(cb, &plug->cb_list, list) // plug callback 리스트 순회
		if (cb->callback == unplug && cb->data == data) // 동일 callback/data 존재 확인
			return cb; // 중복 등록 방지

	/* Not currently on the callback list */
	BUG_ON(size < sizeof(*cb)); // callback 크기가 구조체 크기 이상이어야 함
	cb = kzalloc(size, GFP_ATOMIC); // callback 구조체 할당
	if (cb) { // 할당 성공 시
		cb->data = data; // callback 데이터 설정
		cb->callback = unplug; // callback 함수 설정
		list_add(&cb->list, &plug->cb_list); // plug callback 리스트에 추가
	}
	return cb; // callback 포인터 반환
}
EXPORT_SYMBOL(blk_check_plugged);

/*
 * __blk_flush_plug - plug 에 모인 request 와 callback 을 모두 하부로 본낸다.
 * blk_mq_flush_plug_list 를 통해 NVMe SQ 에 batch 로 기록할 요청들을
 * dispatch 하고, cached_rqs 를 정리한다.
 * 호출 경로: blk_finish_plug -> __blk_flush_plug -> blk_mq_flush_plug_list
 *          -> ... -> nvme_queue_rq -> nvme_submit_cmd(doorbell) (추정)
 */
void __blk_flush_plug(struct blk_plug *plug, bool from_schedule)
{
	if (!list_empty(&plug->cb_list)) // plug callback 이 있으면
		flush_plug_callbacks(plug, from_schedule); // callback 먼저 실행
	blk_mq_flush_plug_list(plug, from_schedule);
	// plug 에 모인 request 를 mq flush; nvme_queue_rq -> doorbell 까지 연결 (추정)
	/*
	 * Unconditionally flush out cached requests, even if the unplug
	 * event came from schedule. Since we know hold references to the
	 * queue for cached requests, we don't want a blocked task holding
	 * up a queue freeze/quiesce event.
	 */
	if (unlikely(!rq_list_empty(&plug->cached_rqs))) // cached_rqs 가 남아있으면
		blk_mq_free_plug_rqs(plug);
	// cached_rqs 를 해제; freeze 대기를 막기 위해 schedule 출발 시에도 강제 flush

	plug->cur_ktime = 0; // plug 시간 초기화
	current->flags &= ~PF_BLOCK_TS; // PF_BLOCK_TS 플래그 클리어
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
 * blk_finish_plug - I/O batch 제출의 끝을 알리고 plug 를 해제한다.
 * blk_start_plug 와 짝을 이루며, plug 해제 시점에 NVMe SQ 로 batch 가
 * 전달된다.
 */
void blk_finish_plug(struct blk_plug *plug)
{
	if (plug == current->plug) { // 현재 태스크 plug 와 일치하는지 확인
		__blk_flush_plug(plug, false); // plug flush; NVMe SQ batch doorbell
		current->plug = NULL; // plug 해제
	}
}
EXPORT_SYMBOL(blk_finish_plug);

/*
 * blk_io_schedule - 장시간 NVMe I/O 대기 중에 hang check 타이머가 발동하지
 * 않도록 io_schedule_timeout/io_schedule 를 호출한다.
 */
void blk_io_schedule(void)
{
	/* Prevent hang_check timer from firing at us during very long I/O */
	unsigned long timeout = sysctl_hung_task_timeout_secs * HZ / 2; // hang check timeout 절반 계산

	if (timeout) // timeout 있으면 제한 시간 스케줄
		io_schedule_timeout(timeout); // 긴 NVMe I/O 대기 중에도 hang task 탐지 방지
	else // timeout 0이면 무기한 io_schedule
		io_schedule();
}
EXPORT_SYMBOL_GPL(blk_io_schedule);

/*
 * blk_dev_init - 블록 계층 초기화. kblockd workqueue 와 request_queue
 * kmem_cache 를 생성한다. NVMe I/O 의 timeout 처리, unplug, MQ dispatch 등이
 * 이 workqueue 를 통해 이루어진다.
 */
int __init blk_dev_init(void)
{
	BUILD_BUG_ON((__force u32)REQ_OP_LAST >= (1 << REQ_OP_BITS)); // REQ_OP_LAST 가 req_op_bits 범위 내임을 컴파일 확인
	BUILD_BUG_ON(REQ_OP_BITS + REQ_FLAG_BITS > 8 * // request cmd_flags 비트 크기 확인
			sizeof_field(struct request, cmd_flags));
	BUILD_BUG_ON(REQ_OP_BITS + REQ_FLAG_BITS > 8 * // bio bi_opf 비트 크기 확인
			sizeof_field(struct bio, bi_opf));

	/* used for unplugging and affects IO latency/throughput - HIGHPRI */
	kblockd_workqueue = alloc_workqueue("kblockd", // kblockd workqueue 할당
					    WQ_MEM_RECLAIM | WQ_HIGHPRI, 0); // MEM_RECLAIM | HIGHPRI; NVMe timeout 빠른 처리
	if (!kblockd_workqueue) // 할당 실패
		panic("Failed to create kblockd\n"); // 커널 패닉; NVMe block layer 초기화 불가

	blk_requestq_cachep = KMEM_CACHE(request_queue, SLAB_PANIC); // request_queue 슬랩 캐시 생성

	blk_debugfs_root = debugfs_create_dir("block", NULL); // block debugfs 디렉터리 생성

	return 0; // 초기화 성공
}

/*
 * NVMe 관점 핵심 요약
 *
 * - block/blk-core.c 는 상위 bio 를 request_queue 로 받아 NVMe SQ 에 넣기
 *   위한 준비 단계(유효성 검사, remap, operation 분류, plug)를 수행한다.
 * - 주요 NVMe 흐름:
 *   submit_bio -> submit_bio_noacct -> __submit_bio -> blk_mq_submit_bio
 *   -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 * - q->limits, q->nr_requests, q->timeout 은 NVMe queue depth, 최대 전송 크기,
 *   명령 타임아웃 등과 직결된다.
 * - blk_queue_enter/exit, blk_queue_start_drain, blk_sync_queue 는 NVMe
 *   컨트롤러 reset/remove 시 in-flight I/O 를 안전하게 drain 하는 핵심 메커니즘이다.
 * - bio_poll / blk_lld_busy 는 NVMe 폴 완료 및 하위 큐 부하 상태를 상위
 *   스택에 노출하는 연결점이다.
 */

