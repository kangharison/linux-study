// SPDX-License-Identifier: GPL-2.0

#include <linux/blk-pm.h>		/* block layer runtime PM 헤더; request_queue의 rpm_status 필드 포함 */
#include <linux/blkdev.h>		/* struct request_queue, bio, request, queue flag 정의; NVMe SQ/CQ 선택의 상위 계층 */
#include <linux/pm_runtime.h>		/* pm_runtime_* API; NVMe APST/RTD3 등 저전력 상태 전이 제어 */
#include "blk-mq.h"			/* blk-mq 내부 헬퍼; blk_mq_unfreeze_queue_nomemrestore() 등 queue 동결/해제 */

/*
 * block/blk-pm.c - 블록 레이어 런타임 전원 관리(runtime PM)
 *
 * 이 파일은 struct request_queue 단위로 런타임 전원 상태를 제어하는
 * 블록 레이어의 PM 헬퍼를 제공합니다. NVMe SSD 입장에서 볼 때,
 * 컨트롤러가 Active 상태와 저전력(Suspended) 상태 사이를 오갈 때
 * queue에 진행 중인 I/O가 없도록 동결(freeze)하고, q_usage_counter를
 * 통해 blk_queue_enter() 참조가 모두 정리되었는지 확인하는 관문 역할을
 * 합니다.
 *
 * NVMe 드라이버는 nvme_probe 또는 nvme_reset_work에서
 * blk_pm_runtime_init()으로 queue에 대한 runtime PM을 초기화하고,
 * nvme_runtime_suspend/resume 콜백에서 blk_pre/post_runtime_suspend/resume을
 * 호출합니다. 이 파일은 blk-mq(blk-mq.c)와 blk-core의 queue 진입/퇴출
 * 카운터 위에서 동작하며, 실제 doorbell/SQ/CQ/CID 제어는
 * drivers/nvme/host/pci.c 등 하위 드라이버에서 이루어집니다.
 */

/*
 * blk_pm_runtime_init() - 큐의 런타임 PM 초기화
 * 목적:
 *   request_queue q와 struct device dev를 연결하고, runtime PM 자동
 *   suspend(auto suspend)을 사용하도록 설정합니다. autosuspend delay를
 *   -1로 두어 드라이버나 사용자가 명시적으로 값을 갱신하기 전까지는
 *   자동으로 suspend되지 않습니다.
 * NVMe 연결:
 *   NVMe 컨트롤러 또는 IO queue가 생성된 후 호출되어, 이 queue를 통해
 *   제출되는 I/O 경로(blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell))에 대한 runtime PM
 *   기반이 마련됩니다.
 * 호출 경로(추정):
 *   nvme_probe -> nvme_reset_work -> nvme_create_io_queues ->
 *   blk_mq_init_queue -> ... -> blk_pm_runtime_init()
 */

/**
 * blk_pm_runtime_init - Block layer runtime PM initialization routine
 * @q: the queue of the device
 * @dev: the device the queue belongs to
 *
 * Description:
 *    Initialize runtime-PM-related fields for @q and start auto suspend for
 *    @dev. Drivers that want to take advantage of request-based runtime PM
 *    should call this function after @dev has been initialized, and its
 *    request queue @q has been allocated, and runtime PM for it can not happen
 *    yet(either due to disabled/forbidden or its usage_count > 0). In most
 *    cases, driver should call this function before any I/O has taken place.
 *
 *    This function takes care of setting up using auto suspend for the device,
 *    the autosuspend delay is set to -1 to make runtime suspend impossible
 *    until an updated value is either set by user or by driver. Drivers do
 *    not need to touch other autosuspend settings.
 *
 *    The block layer runtime PM is request based, so only works for drivers
 *    that use request as their IO unit instead of those directly use bio's.
 */
void blk_pm_runtime_init(struct request_queue *q, struct device *dev)
{
	q->dev = dev;			/* NVMe 컨트롤러의 struct device를 queue에 연결; pm_runtime_* 호출 대상 */
					/* q->dev == NULL이면 이 파일의 모든 함수가 no-op; NVMe probe 실패/제거 시 방어선(추정) */
	q->rpm_status = RPM_ACTIVE;	/* queue의 runtime PM 상태를 Active로 초기화 */
					/* NVMe doorbell/SQ/CQ 경로 개방 상태; 이후 blk_queue_enter() -> nvme_queue_rq() 진입 허용(추정) */
	pm_runtime_set_autosuspend_delay(q->dev, -1);
					/* autosuspend delay -1 == NVMe 컨트롤러가 명시적 허용 전까지 APST/RTD3 자동 진입 금지 */
	pm_runtime_use_autosuspend(q->dev);
					/* autosuspend 사용 설정; 이후 유휴 시 nvme_runtime_suspend 호출 경로 활성화(추정) */
}
EXPORT_SYMBOL(blk_pm_runtime_init);
					/* NVMe 드라이버 및 기타 block 드라이버에서 blk_pm_runtime_init() 직접 호출 가능 */

/*
 * blk_pre_runtime_suspend() - 런타임 suspend 전 큐 상태 점검
 * 목적:
 *   q_usage_counter를 atomic 모드로 전환한 뒤, queue를 사용 중인 참조가
 *   남아 있는지 확인합니다. 진행 중인 요청이 있으면 -EBUSY를 반환하고
 *   ACTIVE 상태로 복원합니다. 참조가 0이면 컨트롤러의 실제 suspend를
 *   진행할 수 있습니다.
 * NVMe 연결:
 *   NVMe 컨트롤러가 APST/RTD3 등의 저전력 상태로 들어가기 전에,
 *   SQ(Submission Queue)로 제출된 명령과 CQ(Completion Queue) 처리가
 *   모두 종료되었는지 확인하는 관문입니다. 새로운 nvme_queue_rq() 진입을
 *   막아 doorbell을 통한 명령 제출이 중단된 후 컨트롤러를 재우게 됩니다.
 * 호출 경로(추정):
 *   PM core -> nvme_runtime_suspend -> blk_pre_runtime_suspend(q) ->
 *   blk_set_pm_only(q) -> blk_freeze_queue_start(q) ->
 *   percpu_ref_switch_to_atomic_sync(&q->q_usage_counter) ->
 *   blk_mq_unfreeze_queue_nomemrestore(q)
 */

/**
 * blk_pre_runtime_suspend - Pre runtime suspend check
 * @q: the queue of the device
 *
 * Description:
 *    This function will check if runtime suspend is allowed for the device
 *    by examining if there are any requests pending in the queue. If there
 *    are requests pending, the device can not be runtime suspended; otherwise,
 *    the queue's status will be updated to SUSPENDING and the driver can
 *    proceed to suspend the device.
 *
 *    For the not allowed case, we mark last busy for the device so that
 *    runtime PM core will try to autosuspend it some time later.
 *
 *    This function should be called near the start of the device's
 *    runtime_suspend callback.
 *
 * Return:
 *    0		- OK to runtime suspend the device
 *    -EBUSY	- Device should not be runtime suspended
 */
int blk_pre_runtime_suspend(struct request_queue *q)
{
	int ret = 0;			/* 기본값: suspend 가능 */
					/* 이후 q_usage_counter가 0이 아니면 -EBUSY로 덮어씀; NVMe SQ/CQ drain 실패 시 보류 */

	if (!q->dev)			/* q->dev 없으면 PM 관리 대상 아님; NVMe PCI probe 실패/제거 경로(추정) */
		return ret;		/* device가 없으면 PM 관리 불필요 */

	WARN_ON_ONCE(q->rpm_status != RPM_ACTIVE);
					/* suspend 전에는 반드시 ACTIVE 상태여야 함 */
					/* NVMe 컨트롤러가 이미 저전력 상태라면 doorbell/SQ 제출 경로가 망가진 상태(추정) */

	spin_lock_irq(&q->queue_lock);	/* queue_lock 획득; q->rpm_status와 pm_only 원자적 갱신을 보장 */
	q->rpm_status = RPM_SUSPENDING;	/* 컨트롤러 저전력 진입을 준비하는 상태 */
					/* 이후 blk_queue_enter()는 pm_only 카운터 검사를 만나 NVMe I/O 진입 차단(추정) */
	spin_unlock_irq(&q->queue_lock);	/* queue_lock 해제; 이 시점부터 q_usage_counter 전환으로 진행 중인 I/O 집계 시작 */

	/*
	 * Increase the pm_only counter before checking whether any
	 * non-PM blk_queue_enter() calls are in progress to avoid that any
	 * new non-PM blk_queue_enter() calls succeed before the pm_only
	 * counter is decreased again.
	 */
	blk_set_pm_only(q);		/* PM 전용 모드: 비PM 경로의 blk_queue_enter() 차단 */
					/* blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq() 진입 차단(추정) */
	ret = -EBUSY;			/* 일단 진행 중인 참조가 있다고 가정 */
					/* 아래 percpu_ref_is_zero()에서 0이면 ret=0으로 덮어씀; NVMe SQ/CQ에 남은 명령 없음 의미(추정) */
	/* Switch q_usage_counter from per-cpu to atomic mode. */
	blk_freeze_queue_start(q);	/* q_usage_counter를 per-cpu에서 atomic 모드로 전환 시작 */
					/* hctx별 진행 중인 request/tag 집계 준비; NVMe multi-queue 병렬 dispatch 일시 정지(추정) */
	/*
	 * Wait until atomic mode has been reached. Since that
	 * involves calling call_rcu(), it is guaranteed that later
	 * blk_queue_enter() calls see the pm-only state. See also
	 * http://lwn.net/Articles/573497/.
	 */
	percpu_ref_switch_to_atomic_sync(&q->q_usage_counter);
					/* 모든 CPU의 percpu 참조가 atomic counter로 병합될 때까지 대기 */
					/* RCU grace period 보장 -> 이후 doorbell 제출/완료 가시성이 컨트롤러에 도달함(추정) */
	if (percpu_ref_is_zero(&q->q_usage_counter))	/* q_usage_counter가 0이면 진행 중인 blk_queue_enter() 없음 */
		ret = 0;		/* queue 사용자가 없으면 실제 suspend 허용 */
					/* NVMe SQ/CQ에 남은 명령이 없어 컨트롤러 저전력 진입 안전(추정) */
	/* Switch q_usage_counter back to per-cpu mode. */
	blk_mq_unfreeze_queue_nomemrestore(q);
					/* 검사 후 per-cpu 모드로 복귀(이 단계는 메모리 복원 없음) */
					/* hctx 병렬 dispatch 준비 상태 복원; NVMe multi-queue SQ/CQ 병렬성 회복(추정) */

	if (ret < 0) {			/* ret이 -EBUSY면 진행 중인 I/O가 아직 남아 있음 */
		spin_lock_irq(&q->queue_lock);	/* rpm_status 원자적 복원을 위해 queue_lock 재획득 */
		q->rpm_status = RPM_ACTIVE;
					/* suspend 불가: Active 상태로 복원 */
					/* doorbell 경로 재개; NVMe 컨트롤러는 계속 Active(추정) */
		pm_runtime_mark_last_busy(q->dev);
					/* 나중에 다시 autosuspend 시도하도록 마지막 busy 시각 갱신 */
					/* NVMe I/O가 완료된 후 PM core가 다시 suspend를 시도하도록 힌트(추정) */
		spin_unlock_irq(&q->queue_lock);	/* Active 상태 복원 완료; 이후 blk_queue_enter() 정상 허용 */

		blk_clear_pm_only(q);	/* PM 전용 모드 해제: 다시 일반 I/O 허용 */
					/* nvme_queue_rq() 진입 재개; NVMe SQ doorbell 제출 다시 가능(추정) */
	}

	return ret;			/* 0이면 NVMe runtime_suspend 진행, -EBUSY면 보류 */
}
EXPORT_SYMBOL(blk_pre_runtime_suspend);
					/* NVMe 드라이버의 nvme_runtime_suspend()에서 직접 호출 */

/*
 * blk_post_runtime_suspend() - 런타임 suspend 완료 후 큐 상태 갱신
 * 목적:
 *   nvme_runtime_suspend() 등의 실제 suspend 콜백이 반환한 err 값에 따라
 *   queue의 rpm_status를 SUSPENDED(성공) 또는 ACTIVE(실패)로 갱신합니다.
 * NVMe 연결:
 *   NVMe 컨트롤러가 실제로 저전력 상태에 들어간 경우, queue를 SUSPENDED로
 *   표시하여 이 queue를 통한 새 nvme_submit_cmd() 경로가 정지되었음을
 *   나타냅니다. 실패 시에는 다시 ACTIVE로 복귀합니다.
 * 호출 경로(추정):
 *   PM core -> nvme_runtime_suspend -> ... -> blk_post_runtime_suspend(q, err)
 */

/**
 * blk_post_runtime_suspend - Post runtime suspend processing
 * @q: the queue of the device
 * @err: return value of the device's runtime_suspend function
 *
 * Description:
 *    Update the queue's runtime status according to the return value of the
 *    device's runtime suspend function and mark last busy for the device so
 *    that PM core will try to auto suspend the device at a later time.
 *
 *    This function should be called near the end of the device's
 *    runtime_suspend callback.
 */
void blk_post_runtime_suspend(struct request_queue *q, int err)
{
	if (!q->dev)			/* q->dev 없으면 상태 갱신 불필요; NVMe hot-unplug 후 경로(추정) */
		return;			/* device가 없으면 아무 작업도 하지 않음 */

	spin_lock_irq(&q->queue_lock);	/* queue_lock 획득; rpm_status 원자적 갱신 */
	if (!err) {			/* nvme_runtime_suspend()가 성공한 경우 */
		q->rpm_status = RPM_SUSPENDED;
					/* NVMe 컨트롤러가 저전력 상태에 진입함을 표시 */
					/* doorbell/SQ 제출 완전 정지; nvme_submit_cmd()는 blk_queue_enter()에서 차단(추정) */
	} else {			/* nvme_runtime_suspend() 실패: NVMe 컨트롤러가 여전히 동작 중 */
		q->rpm_status = RPM_ACTIVE;
					/* suspend 실패: Active 상태 유지 */
					/* doorbell/SQ/CQ 경로 계속 개방; CID/tag 재활용 가능(추정) */
		pm_runtime_mark_last_busy(q->dev);
					/* 나중에 다시 autosuspend 시도하도록 기록 */
					/* NVMe I/O 완료 후 PM core가 재시도하도록 busy 힌트(추정) */
	}
	spin_unlock_irq(&q->queue_lock);	/* 상태 갱신 완료; queue_lock 해제 */

	if (err)			/* suspend 실패 시에만 pm_only 해제 */
		blk_clear_pm_only(q);	/* suspend 실패 시 PM 전용 모드 해제 */
					/* I/O 재개 및 nvme_queue_rq() -> nvme_submit_cmd(doorbell) 허용(추정) */
}
EXPORT_SYMBOL(blk_post_runtime_suspend);
					/* NVMe runtime_suspend 콜백 종료 직전 호출 지점 */

/*
 * blk_pre_runtime_resume() - 런타임 resume 시작 전 큐 상태 전환
 * 목적:
 *   저전력 상태에서 깨어나기 직전 queue의 rpm_status를 RESUMING으로
 *   설정하여 resume 진행 중임을 표시합니다.
 * NVMe 연결:
 *   NVMe 컨트롤러의 PCIe 링크 복원/레지스터 접근 재개 직전에 호출되어,
 *   queue가 아직 완전히 활성화되지 않았음을 알립니다.
 * 호출 경로(추정):
 *   PM core -> nvme_runtime_resume -> blk_pre_runtime_resume(q)
 */

/**
 * blk_pre_runtime_resume - Pre runtime resume processing
 * @q: the queue of the device
 *
 * Description:
 *    Update the queue's runtime status to RESUMING in preparation for the
 *    runtime resume of the device.
 *
 *    This function should be called near the start of the device's
 *    runtime_resume callback.
 */
void blk_pre_runtime_resume(struct request_queue *q)
{
	if (!q->dev)			/* q->dev 없으면 상태 전환 불필요; NVMe 제거된 queue에서 no-op(추정) */
		return;			/* device가 없으면 상태 전환 불필요 */

	spin_lock_irq(&q->queue_lock);	/* queue_lock 획득; RESUMING 상태 원자적 설정 */
	q->rpm_status = RPM_RESUMING;	/* NVMe 컨트롤러 깨어나는 중임을 표시 */
					/* PCIe register/DMA 복원 직전 단계; 아직 doorbell/SQ 제출은 금지(추정) */
	spin_unlock_irq(&q->queue_lock);	/* RESUMING 표시 완료; 이후 NVMe 컨트롤러 복원 작업 진행 */
}
EXPORT_SYMBOL(blk_pre_runtime_resume);
					/* NVMe runtime_resume 콜백 시작 직전 호출 지점 */

/*
 * blk_post_runtime_resume() - 런타임 resume 완료 후 큐 재개
 * 목적:
 *   resume 콜백이 성공했든 실패했든 queue를 다시 ACTIVE로 만들고,
 *   pm_only 카운터를 해제하여 새 I/O가 들어올 수 있게 합니다.
 *   autosuspend를 다시 요청하여 유휴 시 자동으로 저전력 상태로 들어갈
 *   수 있도록 합니다.
 * NVMe 연결:
 *   NVMe 컨트롤러가 깨어난 후 queue가 재개되면,
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 경로가 다시 활성화됩니다.
 * 호출 경로(추정):
 *   PM core -> nvme_runtime_resume -> blk_post_runtime_resume(q)
 */

/**
 * blk_post_runtime_resume - Post runtime resume processing
 * @q: the queue of the device
 *
 * Description:
 *    Restart the queue of a runtime suspended device. It does this regardless
 *    of whether the device's runtime-resume succeeded; even if it failed the
 *    driver or error handler will need to communicate with the device.
 *
 *    This function should be called near the end of the device's
 *    runtime_resume callback to correct queue runtime PM status and re-enable
 *    peeking requests from the queue.
 */
void blk_post_runtime_resume(struct request_queue *q)
{
	int old_status;			/* 복귀 전 상태를 임시 저장 */
					/* SUSPENDED/RESUMING이었을 때만 blk_clear_pm_only() 호출 여부 결정(추정) */

	if (!q->dev)			/* q->dev 없으면 queue 재개 불필요; NVMe remove path(추정) */
		return;			/* device가 없으면 queue 재개 불필요 */

	spin_lock_irq(&q->queue_lock);	/* queue_lock 획득; rpm_status와 old_status 일관성 확보 */
	old_status = q->rpm_status;		/* 복귀 전 상태를 임시 저장 */
	q->rpm_status = RPM_ACTIVE;	/* queue를 다시 Active로 전환 */
					/* NVMe doorbell/SQ/CQ 경로 재개 준비 완료; nvme_queue_rq() 진입 재개(추정) */
	pm_runtime_mark_last_busy(q->dev);	/* resume 완료 시점을 busy 시각으로 기록 */
					/* NVMe 컨트롤러가 깨어난 직후 busy 상태를 PM core에 알림(추정) */
	pm_request_autosuspend(q->dev);	/* 유휴 시 자동으로 저전력 상태로 들어갈 수 있도록 */
					/* NVMe RTD3 재진입 타이머 재설정; 다음 idle 시 APST/RTD3 시도(추정) */
	spin_unlock_irq(&q->queue_lock);	/* Active 상태 복원 완료; 새로운 I/O 진입 허용 */

	if (old_status != RPM_ACTIVE)	/* SUSPENDED/RESUMING 상태였을 때만 pm_only 해제 */
		blk_clear_pm_only(q);	/* SUSPENDED/RESUMING 상태였다면 PM 전용 모드 해제, I/O 재개 */
					/* nvme_queue_rq() -> nvme_submit_cmd(doorbell) 허용; NVMe multi-queue 병렬성 복원(추정) */
}
EXPORT_SYMBOL(blk_post_runtime_resume);
					/* NVMe runtime_resume 콜백 종료 직전 호출; doorbell 경로 최종 개방 */

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 queue 단위 runtime PM을 위한 블록 레이어 정책 계층이며,
 *   NVMe 컨트롤러가 저전력 상태로 들어가기 전에 진행 중인 I/O가 없음을
 *   보장합니다.
 * - blk_pre_runtime_suspend()는 q_usage_counter를 통해
 *   blk_queue_enter()로 queue에 진입한 경로가 모두 빠져나갔는지 확인하고,
 *   이때 새로운 blk_mq_submit_bio -> nvme_queue_rq() 진입을 차단합니다.
 * - suspend 성공 시 queue는 RPM_SUSPENDED로, resume 후에는
 *   blk_post_runtime_resume()에서 RPM_ACTIVE로 복귀하여 doorbell을 통한
 *   nvme_submit_cmd() 경로가 다시 열립니다.
 * - 실제 NVMe register/doorbell/SQ/CID 조작은 이 파일에서 다루지 않으며,
 *   drivers/nvme/host/ 아래의 드라이버에서 수행됩니다.
 * - 이 파일은 blk-mq.c, blk-core.c의 queue 카운터와 연동되며,
 *   NVMe 드라이버의 runtime_suspend/resume 콜백 사이에서 호출됩니다.
 */
