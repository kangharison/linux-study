/* SPDX-License-Identifier: GPL-2.0 */

/*
 * block/blk-pm.h - 블록 계층의 runtime power management 헤더
 *
 * 이 파일은 struct request_queue 와 연결된 struct device 의 runtime PM 상태를
 * 검사/제어하는 인라인 헬퍼를 제공한다. NVMe SSD 입장에서 볼 때, I/O가
 * 도착했을 때 컨트롤러가 suspend 상태라면 resume 요청을 발행하고, I/O
 * 완료 시 마지막 활동 시각을 갱신하여 다음 autosuspend 타이머를 늦춘다.
 *
 * NVMe 기준 호출 경로 예시:
 *   blk_mq_submit_bio -> blk_mq_get_request -> blk_pm_resume_queue
 *   nvme_complete_rq -> blk_mq_end_request -> blk_pm_mark_last_busy
 *
 * 관련 파일: block/blk-core.c, block/blk-mq.c, block/blk-pm.c,
 *           drivers/nvme/host/pci.c (nvme_reset_work, nvme_dev_disable 등)
 */

#ifndef _BLOCK_BLK_PM_H_
#define _BLOCK_BLK_PM_H_

#include <linux/pm_runtime.h>

#ifdef CONFIG_PM
/**
 * blk_pm_resume_queue - I/O 처리 전 request_queue 의 runtime resume 상태 확인
 *
 * @pm:  true면 power management 상태를 고려, false면 항상 허용 (추정)
 * @q:   bio/request가 도착한 request_queue, NVMe에서는 nvme_queue->dev_queue
 *       또는 nvme_ns->queue 에 해당 (추정)
 *
 * 목적:
 *   큐에 I/O가 들어왔을 때, 해당 큐가 suspend 상태이면 runtime resume을
 *   요청한다. NVMe SSD에서 컨트롤러가 sleep 상태일 때 doorbell을 치거나
 *   SQ/CQ를 접근하면 정상 동작하지 않으므로, 먼저 resume이 완료되어야 한다.
 *
 * NVMe 기준 호출 경로:
 *   blk_mq_submit_bio -> blk_mq_get_request -> blk_pm_resume_queue
 *   -> (resume 필요 시) pm_request_resume(q->dev)
 *   -> 이후 nvme_queue_rq -> nvme_submit_cmd(doorbell) 로 이어짐
 *
 * 반환값:
 *   1: I/O를 즉시 처리해도 됨 (큐가 이미 활성 상태이거나 PM을 사용 안 함)
 *   0: resume 요청만 하고 I/O 처리는 일시 중단, resume 완료 후 재시도
 */
static inline int blk_pm_resume_queue(const bool pm, struct request_queue *q)
{
	/* q->dev: request_queue에 바인딩된 struct device, NVMe에서는
	 *         PCIe function 디바이스를 가리킴 (추정)
	 * blk_queue_pm_only(q): 큐가 PM_ONLY 플래그를 가지면 runtime PM으로
	 *                       관리 중임을 의미 (추정)
	 */
	if (!q->dev || !blk_queue_pm_only(q))
		/* NVMe: q->dev가 NULL이면 아직 nvme_alloc_queue() 중이거나
		 *        host probe 이전 상태일 수 있음 (추정). blk_queue_pm_only
		 *        가 false면 해당 큐는 APST/Runtime PM 대상이 아니므로
		 *        doorbell/SQ/CQ 접근 제약 없이 즉시 I/O 진행 가능.
		 */
		return 1;	/* PM과 무관한 큐이므로 I/O 허용 */

	/* q->rpm_status: struct device의 현재 runtime PM 상태
	 * RPM_SUSPENDED: 디바이스가 현재 suspend 됨
	 * pm이 false면 상태 검사 없이 바로 1을 반환 (추정)
	 */
	if (pm && q->rpm_status != RPM_SUSPENDED)
		/* NVMe: 컨트롤러가 이미 활성(RPM_ACTIVE) 또는 활성화 중이면
		 *        CC.EN=1, CSTS.RDY=1 상태에서 SQ tail doorbell과 CQ head
		 *        doorbell 접근이 유효하므로 nvme_submit_cmd() 진행 가능.
		 *        RPM_RESUMING 중에도 doorbell 보이기 전까지는 (추정)
		 *        blk-mq가 hold하지 않고 1을 리턴할 수 있음.
		 */
		return 1;	/* 이미 활성화 상태이므로 I/O 허용 */

	/* NVMe 컨트롤러가 아직 켜지지 않았으면 resume 요청
	 * pm_request_resume은 비동기로 resume 작업을 예약하며,
	 * 완료되기 전까지는 해당 큐의 I/O를 blk-mq가 hold한다 (추정)
	 */
	pm_request_resume(q->dev);
		/* NVMe resume call-chain (추정):
		 *   pm_request_resume(q->dev)
		 *   -> nvme_pci_runtime_resume() / nvme_fc_resume() 등
		 *   -> nvme_disable_ctrl() 없이 nvme_enable_ctrl()
		 *   -> queue IRQ/SQ/CQ 복원 -> CC.EN=1 -> CSTS.RDY=1
		 *   -> resume 완료 후 blk-mq의 hctx/kblockd가 대기하던 I/O를
		 *      다시 dispatch -> nvme_queue_rq -> nvme_submit_cmd
		 *   이 시점에서는 doorbell을 치면 안 되므로 0을 반환하여
		 *   blk-mq가 request를 다시 plug/requeue 처리.
		 */
	return 0;	/* I/O 처리 보류, resume 완료 후 재시도 */
}

/**
 * blk_pm_mark_last_busy - I/O 완료 시 디바이스의 마지막 활동 시각 갱신
 *
 * @rq:  완료된 struct request, NVMe CID가 매핑된 request
 *
 * 목적:
 *   request 완료 시점에 runtime PM 프레임워크에 "아직 바쁨"을 알려
 *   autosuspend 타이머를 초기화한다. NVMe SSD가 idle 상태로 자동
 *   전환되는 시점을 늦춰, 빈번한 suspend/resume을 막는다.
 *
 * NVMe 기준 호출 경로:
 *   nvme_irq -> nvme_process_cq -> nvme_complete_rq
 *   -> blk_mq_end_request -> blk_pm_mark_last_busy
 *   -> pm_runtime_mark_last_busy(rq->q->dev)
 *
 * 주의:
 *   RQF_PM 플래그가 설정된 request는 PM resume과 연관된 내부 요청으로
 *   보이므로(추정), last_busy 갱신에서 제외하여 통계 왜곡을 피한다.
 */
static inline void blk_pm_mark_last_busy(struct request *rq)
{
	/* rq->q->dev: request가 속한 큐의 디바이스, NVMe 컨트롤러 디바이스
	 * RQF_PM:     runtime PM 관련 내부 request 플래그 (추정)
	 */
	if (rq->q->dev && !(rq->rq_flags & RQF_PM))
		/* NVMe CQ 완료 경로에서 rq->q->dev가 존재하면 APST 또는
		 * runtime PM autosuspend 타이머를 리셋. RQF_PM이 설정된
		 * request는 NVMe 컨트롤러 resume 자체를 유발하는 내부
		 * management 명령(Identify/Set Features 등)일 가능성이
		 * 있으므로(추정) last_busy 갱신에서 제외해 idle 시간
		 * 통계를 왜곡하지 않음.
		 */
		pm_runtime_mark_last_busy(rq->q->dev); /* autosuspend 타이머 리셋 */
			/* NVMe autosuspend/APST 영향 (추정):
			 *   타이머 갱신 -> dev->power.last_busy = ktime_get()
			 *   -> autosuspend 타이머 만료 지연 -> nvme_suspend/
			 *      APST entry 지연 -> 마지막 I/O 이후 일정 시간
			 *      경과 시에만 CC.EN=0, PCI D3hot, NVMe APST로
			 *      전환. 빈번한 doorbell 재개시 최소화.
			 */
}
#else
/* CONFIG_PM 미설정 시: 컴파일 타임에 PM 코드를 완전히 제거.
 * 임베디드/NVMe 서버 등에서 CONFIG_PM 없이 빌드될 때 사용됨.
 */
static inline int blk_pm_resume_queue(const bool pm, struct request_queue *q)
{
	/* NVMe: CONFIG_PM=n 이면 runtime PM, APST, ASPM 모두 비활성.
	 *        컨트롤러는 probe 시 설정된 전원 상태를 유지하며,
	 *        doorbell/SQ/CQ 접근에 추가 제약 없음. pm 파라미터는
	 *        컴파일 타임에 제거되어 nvme_queue_rq() 진입 직전
	 *        오버헤드가 사라짐.
	 */
	return 1;	/* 항상 I/O 허용 */
}

static inline void blk_pm_mark_last_busy(struct request *rq)
{
	/* NVMe: CONFIG_PM=n 환경에서는 완료 시점에 autosuspend 타이머가
	 *        없으므로 no-op. nvme_complete_rq -> blk_mq_end_request
	 *        경로에서 추가 PM 지연 없이 request 해제 및 CID/tag
	 *        반환이 바로 이어짐.
	 */
}
#endif

/* NVMe 관점 핵심 요약
 *
 * - blk_pm_resume_queue는 NVMe 컨트롤러가 suspend 상태일 때
 *   doorbell/SQ/CQ 접근을 막고, 먼저 pm_request_resume을 통해
 *   컨트롤러를 깨운 후 I/O를 진행한다.
 * - blk_pm_mark_last_busy는 I/O 완료 후 autosuspend 타이머를
 *   갱신하여, NVMe SSD가 불필요하게 빈번하게 sleep/wakeup 하는
 *   것을 완화한다.
 * - 이 파일은 block/blk-mq.c, block/blk-core.c의 큐 관리 코드와
 *   짝을 이루며, drivers/nvme/host/pci.c 등의 NVMe 드라이버가
 *   등록한 struct device의 runtime PM 콜백과 연결된다.
 * - CONFIG_PM이 꺼진 환경에서는 이 헬퍼들이 no-op이 되어, PM 없이도
 *   NVMe I/O 경로가 동일하게 동작한다.
 * - q->dev, q->rpm_status, RQF_PM 등은 NVMe 입장에서 호스트의
 *   전원 정책을 반영하는 인터페이스일 뿐, NVMe 내부 레지스터
 *   (doorbell, CC, CSTS 등)를 직접 건드리지는 않는다.
 */

#endif /* _BLOCK_BLK_PM_H_ */
