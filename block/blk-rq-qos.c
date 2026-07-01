// SPDX-License-Identifier: GPL-2.0

/*
 * block/blk-rq-qos.c - NVMe SSD 관점 파일 요약
 *
 * 이 파일은 blk-mq 기반 블록 장치의 요청 품질 서비스(QoS) 인프라를 제공한다.
 * NVMe SSD 입장에서 볼 때, bio/request가 실제 nvme_queue_rq()를 통해
 * Submission Queue(SQ)에 삽입되기 전/후의 대역폭, 지연, 큐 깊이를 조절하는
 * 중간 계층이다. blk-mq 스케줄러 이후, nvme_submit_cmd() / doorbell ring
 * 이전 단계에서 동작하며, blk-wbt.c, blk-iocost.c 등의 정책 모듈이
 * 등록하는 rq_qos_ops 체인을 관리한다.
 *
 * 연관 파일: block/blk-mq.c (요청 할당/완료), block/blk-wbt.c (쓰기 지연 제어),
 *          block/blk-iocost.c (비용 기반 제어), drivers/nvme/host/*.c (SQ/CQ/doorbell).
 */

#include "blk-rq-qos.h"

/*
 * Increment 'v', if 'v' is below 'below'. Returns true if we succeeded,
 * false if 'v' + 1 would be bigger than 'below'.
 */
static bool atomic_inc_below(atomic_t *v, unsigned int below)
{
	unsigned int cur = atomic_read(v); /* 현재 outstanding CID/tag 수를 원자적으로 읽어 비교 기준 설정 */

	do {
		if (cur >= below) /* NVMe SQ depth/queue limit 초과: 새 CID 삽입 불가 */
			return false;
	} while (!atomic_try_cmpxchg(v, &cur, cur + 1)); /* CAS로 outstanding 카운트 증가, 실패 시 cur 재읽고 재시도 (SMP race 방지) */

	return true; /* atomic한 증가 성공: 이제 nvme_submit_cmd()로 doorbell 가능 */
}

/*
 * struct rq_wait 필드와 NVMe 동작의 관계:
 * - wait:        NVMe SQ budget이 부족하면 이 waitqueue에서 태스크가 잠든다.
 * - inflight:    현재 NVMe SQ에 진입해 완료되지 않은 명령(CID) 수.
 *                nvme_queue_rq() 성공 시 증가하고, nvme_completion() 경로에서
 *                감소한다.
 */
bool rq_wait_inc_below(struct rq_wait *rq_wait, unsigned int limit)
{
	return atomic_inc_below(&rq_wait->inflight, limit); /* limit은 NVMe queue depth 또는 QoS max_depth */
}

/*
 * struct rq_qos 필드와 NVMe 동작의 관계:
 * - ops:   throttle/issue/done 등 NVMe 명령 생명주기별 콜백 테이블.
 * - disk:  이 QoS가 붙은 NVMe namespace를 나타내는 gendisk.
 * - id:    RQ_QOS_WBT / LATENCY / COST 중 하나로, NVMe에 적용할 정책 식별자.
 * - next:  여러 QoS 모듈을 request_queue에 체인으로 연결한다.
 */

/*
 * __rq_qos_cleanup() - bio 경로상 오류/정리 시 등록된 모든 rq_qos 모듈의
 * cleanup 콜백을 순회 호출한다.
 *
 * 호출 경로(추정): nvme_queue_rq() 실패 등 -> blk_mq_end_request() 또는
 * bio_endio() 경로 -> rq_qos_cleanup() -> __rq_qos_cleanup().
 *
 * NVMe 연결: NVMe 드라이버가 doorbell ring 전 오류를 반환하면 QoS 모듈이
 * 이미 계산한 inflight/비용을 되돌릴 기회를 제공한다.
 */
void __rq_qos_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->cleanup) /* 등록된 정책(wbt/iocost 등)의 cleanup 콜백 존재 시 */
			rqos->ops->cleanup(rqos, bio); /* bio 단위로 NVMe 진입 전 소진된 budget 복구 */
		rqos = rqos->next; /* 다음 QoS 모듈로 체인 순회 */
	} while (rqos); /* 체인 끝(NULL)까지 반복 */
}

/*
 * __rq_qos_done() - NVMe 명령이 완료(CQ entry 처리)될 때 등록된 QoS 모듈의
 * done 콜백을 순회 호출한다.
 *
 * 호출 경로: nvme_irq() / nvme_poll() -> nvme_process_cq() ->
 * blk_mq_complete_request() -> rq_qos_done() -> __rq_qos_done().
 *
 * NVMe 연결: CQ entry 수신으로 request가 완료되면 wbt/iocost가
 * inflight 비용을 회수하고, 대기 중인 태스크를 깨울 기회를 얻는다.
 */
void __rq_qos_done(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->done) /* NVMe CQ 수신 후 per-policy 완료 콜백 */
			rqos->ops->done(rqos, rq); /* 완료된 CID에 해당하는 request의 비용 회수 */
		rqos = rqos->next; /* QoS 체인 순회: wbt -> iocost -> ... */
	} while (rqos);
}

/*
 * __rq_qos_issue() - NVMe request가 실제로 SQ로 발행될 때 QoS 모듈의
 * issue 콜백을 순회 호출한다.
 *
 * 호출 경로: blk_mq_dispatch_rq_list() ->
 * nvme_queue_rq() 직전 -> rq_qos_issue() -> __rq_qos_issue().
 *
 * NVMe 연결: PRP/SGL 설정이 끝나고 doorbell ring 직전에 호출되어,
 * QoS 모듈이 실제 NVMe SQ에 진입하는 시점을 정확히 관찰한다.
 */
void __rq_qos_issue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->issue) /* doorbell 직전 시점의 issue 콜백 */
			rqos->ops->issue(rqos, rq); /* 실제 SQ 진입하는 request를 각 정책에 기록 */
		rqos = rqos->next; /* 연결된 모든 QoS 모듈에 issue 이벤트 전파 */
	} while (rqos);
}

/*
 * __rq_qos_requeue() - NVMe request가 재배염될 때 QoS 모듈의 requeue 콜백을
 * 순회 호출한다.
 *
 * 호출 경로: nvme_queue_rq() 실패/재시도 -> blk_mq_requeue_request() ->
 * rq_qos_requeue() -> __rq_qos_requeue().
 *
 * NVMe 연결: doorbell을 울리지 못하고 request를 다시 스케줄러로 돌리면
 * 이미 할당된 QoS budget을 원상 복구해야 한다.
 */
void __rq_qos_requeue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->requeue) /* 재배열 시 정책 상태 롤백 콜백 */
			rqos->ops->requeue(rqos, rq); /* 아직 doorbell 안 울린 request의 budget 반납 */
		rqos = rqos->next; /* QoS 체인 순회 */
	} while (rqos);
}

/*
 * __rq_qos_throttle() - bio가 request로 변환되기 전, 등록된 QoS 모듈의
 * throttle 콜백을 순회 호출한다.
 *
 * 호출 경로: blk_mq_submit_bio() -> blk_mq_get_request() 이전 ->
 * rq_qos_throttle() -> __rq_qos_throttle().
 *
 * NVMe 연결: 이 단계에서 지연/비용 한도를 초과하면 bio가 잠자거나
 * merge될 때까지 대기하며, 결과적으로 nvme_queue_rq()로 들어가는
 * outstanding 명령 수를 억제한다.
 */
void __rq_qos_throttle(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->throttle) /* bio -> request 변환 전 지연/비용 제어 콜백 */
			rqos->ops->throttle(rqos, bio); /* 필요 시 sleep으로 NVMe SQ 진입률 조절 */
		rqos = rqos->next; /* 체인 상 모든 throttle 콜백 적용 */
	} while (rqos);
}

/*
 * __rq_qos_track() - bio가 request에 묶일 때 QoS 모듈의 track 콜백을
 * 순회 호출한다.
 *
 * 호출 경로: blk_mq_submit_bio() -> blk_mq_get_request() ->
 * rq_qos_track() -> __rq_qos_track().
 *
 * NVMe 연결: bio -> request 매핑 시점에 QoS 모듈이 이 request가
 * 어떤 NVMe namespace/큐에 속하는지 추적한다.
 */
void __rq_qos_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->track) /* request 할당 시 bio를 추적하는 콜백 */
			rqos->ops->track(rqos, rq, bio); /* 이후 nvme_queue_rq()에서 사용할 QoS 컨텍스트 연결 */
		rqos = rqos->next; /* QoS 체인 순회 */
	} while (rqos);
}

/*
 * __rq_qos_merge() - bio가 기존 request에 병합될 때 QoS 모듈의 merge
 * 콜백을 순회 호출한다.
 *
 * 호출 경로: blk_mq_submit_bio() -> blk_attempt_bio_merge() ->
 * rq_qos_merge() -> __rq_qos_merge().
 *
 * NVMe 연결: 병합된 request는 단일 NVMe 명령(CID)으로 SQ에 들어가므로
 * QoS 모듈이 두 bio를 하나의 명령 비용으로 재계산해야 한다.
 */
void __rq_qos_merge(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->merge) /* bio merge 시점의 QoS 재계산 콜백 */
			rqos->ops->merge(rqos, rq, bio); /* 병합 후 단일 NVMe CID로 처리될 비용 조정 */
		rqos = rqos->next; /* QoS 체인 순회 */
	} while (rqos);
}

/*
 * __rq_qos_done_bio() - bio가 완료될 때 QoS 모듈의 done_bio 콜백을
 * 순회 호출한다.
 *
 * 호출 경로: bio_endio() -> rq_qos_done_bio() -> __rq_qos_done_bio().
 *
 * NVMe 연결: NVMe CQ entry 처리 후 상위 레이어로 bio 완료가 전파될 때
 * QoS 모듈이 bio 수준의 비용을 최종 정산한다.
 */
void __rq_qos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->done_bio) /* bio 완료 시점 콜백 */
			rqos->ops->done_bio(rqos, bio); /* NVMe CQ -> bio_endio() 경로의 비용 최종 정산 */
		rqos = rqos->next; /* QoS 체인 순회 */
	} while (rqos);
}

/*
 * __rq_qos_queue_depth_changed() - NVMe queue depth가 변경되었을 때
 * 등록된 QoS 모듈의 queue_depth_changed 콜백을 순회 호출한다.
 *
 * 호출 경로: nvme_set_queue_depth() / blk_mq_update_nr_hw_queues() ->
 * rq_qos_queue_depth_changed() -> __rq_qos_queue_depth_changed().
 *
 * NVMe 연결: NVMe 컨트롤러의 SQ/CQ 깊이가 바뀌면 wbt/iocost가
 * max_depth/default_depth를 새 물리 한도에 맞춰 재조정해야 한다.
 */
void __rq_qos_queue_depth_changed(struct rq_qos *rqos)
{
	do {
		if (rqos->ops->queue_depth_changed) /* SQ/CQ 깊이 변경 알림 콜백 */
			rqos->ops->queue_depth_changed(rqos); /* 새 queue depth에 맞춰 정책 파라미터 재조정 */
		rqos = rqos->next; /* QoS 체인 순회 */
	} while (rqos);
}

/*
 * struct rq_depth 필드와 NVMe 동작의 관계:
 * - max_depth:    현재 정책이 허용하는 NVMe outstanding 명령 최대 수.
 * - scale_step:   지연/처리량 피드백에 따른 SQ 깊이 조정 방향/폭.
 * - scaled_max:   이전 라운드에서 이미 max_depth에 도달했음을 표시.
 * - queue_depth:  NVMe 컨트롤러/호스트가 지원하는 SQ/CQ 물리 최대 깊이.
 * - default_depth: QoS 정책이 선호하는 기본 outstanding 명령 수.
 */

/*
 * Return true, if we can't increase the depth further by scaling
 */
bool rq_depth_calc_max_depth(struct rq_depth *rqd)
{
	unsigned int depth;
	bool ret = false;

	/*
	 * For QD=1 devices, this is a special case. It's important for those
	 * to have one request ready when one completes, so force a depth of
	 * 2 for those devices. On the backend, it'll be a depth of 1 anyway,
	 * since the device can't have more than that in flight. If we're
	 * scaling down, then keep a setting of 1/1/1.
	 */
	if (rqd->queue_depth == 1) { /* NVMe SQ 물리 깊이가 1인 특수 케이스 */
		if (rqd->scale_step > 0)
			rqd->max_depth = 1; /* NVMe SQ 물리 깊이가 1이면 제한 유지 */
		else {
			rqd->max_depth = 2; /* 다음 doorbell 준비를 위해 예비 공간 확보 */
			ret = true;
		}
	} else {
		/*
		 * scale_step == 0 is our default state. If we have suffered
		 * latency spikes, step will be > 0, and we shrink the
		 * allowed write depths. If step is < 0, we're only doing
		 * writes, and we allow a temporarily higher depth to
		 * increase performance.
		 */
		depth = min_t(unsigned int, rqd->default_depth,
			      rqd->queue_depth); /* NVMe SQ 물리 한도를 넘지 않도록 클립 */
		if (rqd->scale_step > 0)
			depth = 1 + ((depth - 1) >> min(31, rqd->scale_step)); /* 지연 급증 시 outstanding CID 수를 절반으로 축소 */
		else if (rqd->scale_step < 0) {
			unsigned int maxd = 3 * rqd->queue_depth / 4; /* NVMe SQ 깊이의 75%까지만 일시 확장 */

			depth = 1 + ((depth - 1) << -rqd->scale_step); /* 쓰기 집중 시 처리량을 높이기 위해 깊이 확장 */
			if (depth > maxd) {
				depth = maxd; /* NVMe SQ 물리 한도의 75% 상한 적용 */
				ret = true; /* 더 이상 확장 불가 표시 */
			}
		}

		rqd->max_depth = depth; /* 최종적으로 NVMe SQ에 들어갈 수 있는 CID 상한 갱신 */
	}

	return ret; /* true면 max 한도 도달, scale_up 중단 신호 */
}

/* Returns true on success and false if scaling up wasn't possible */
bool rq_depth_scale_up(struct rq_depth *rqd)
{
	/*
	 * Hit max in previous round, stop here
	 */
	if (rqd->scaled_max) /* 이전 라운드에서 NVMe SQ depth 상한에 도달했으면 확장 중단 */
		return false;

	rqd->scale_step--; /* outstanding CID 허용 한도를 단계적으로 상향 */

	rqd->scaled_max = rq_depth_calc_max_depth(rqd); /* 새로운 NVMe max_depth 산출 */
	return true;
}

/*
 * Scale rwb down. If 'hard_throttle' is set, do it quicker, since we
 * had a latency violation. Returns true on success and returns false if
 * scaling down wasn't possible.
 */
bool rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle)
{
	/*
	 * Stop scaling down when we've hit the limit. This also prevents
	 * ->scale_step from going to crazy values, if the device can't
	 * keep up.
	 */
	if (rqd->max_depth == 1) /* NVMe outstanding CID가 1개로 최소화되면 더 이상 축소 불가 */
		return false;

	if (rqd->scale_step < 0 && hard_throttle)
		rqd->scale_step = 0; /* 강력 쓰로틀: 확장 상태를 즉시 초기화 */
	else
		rqd->scale_step++; /* 지연/타임아웃에 따라 outstanding CID 한도 단계적 하향 */

	rqd->scaled_max = false; /* 축소 후에는 다시 확장 여지를 남김 */
	rq_depth_calc_max_depth(rqd); /* 새 NVMe max_depth 계산 및 적용 */
	return true;
}

/*
 * struct rq_qos_wait_data 필드와 NVMe 동작의 관계:
 * - wq:          대기 큐 진입점으로, NVMe SQ budget이 부족할 때 사용된다.
 * - rqw:         NVMe SQ budget을 관리하는 rq_wait 포인터.
 * - cb:          inflight 획득을 시도하는 acquire_inflight_cb_t 콜백.
 * - private_data: wbt/iocost 등 정책별 컨텍스트 데이터.
 * - got_token:   budget 획득 성공 여부를 나타낸다.
 */
struct rq_qos_wait_data {
	struct wait_queue_entry wq;
	struct rq_wait *rqw;
	acquire_inflight_cb_t *cb;
	void *private_data;
	bool got_token;
};

/*
 * rq_qos_wake_function() - rq_qos_wait()에서 대기 중인 태스크를 깨울 때
 * 실제로 inflight budget을 획득할 수 있는지 검사한다.
 *
 * 호출 경로: __wake_up_common() -> rq_qos_wake_function()
 * -> acquire_inflight_cb() -> rq_wait_inc_below().
 *
 * NVMe 연결: 깨어난 태스크가 NVMe doorbell 경로로 진입하기 전에
 * SQ budget을 획득했는지 확인한다. 획득 실패 시 wake loop를 중단(-1)하여
 * 다른 waiter가 불필요하게 깨어나는 것을 막는다.
 */
static int rq_qos_wake_function(struct wait_queue_entry *curr,
				unsigned int mode, int wake_flags, void *key)
{
	struct rq_qos_wait_data *data = container_of(curr,
						     struct rq_qos_wait_data,
						     wq); /* wait_queue_entry에서 rq_qos_wait_data 추출 */

	/*
	 * If we fail to get a budget, return -1 to interrupt the wake up loop
	 * in __wake_up_common.
	 */
	if (!data->cb(data->rqw, data->private_data)) /* NVMe SQ budget 획득 시도, 실패 시 깨우지 않음 */
		return -1; /* wake loop 즉시 중단: 다른 waiter가 헛되이 깨어나는 것 방지 */

	data->got_token = true; /* budget 획득 성공, 깨어난 태스크는 곧 doorbell 경로 진입 */
	/*
	 * autoremove_wake_function() removes the wait entry only when it
	 * actually changed the task state. We want the wait always removed.
	 * Remove explicitly and use default_wake_function().
	 */
	default_wake_function(curr, mode, wake_flags, key); /* 태스크를 RUNNABLE로 전환 */
	/*
	 * Note that the order of operations is important as finish_wait()
	 * tests whether @curr is removed without grabbing the lock. This
	 * should be the last thing to do to make sure we will not have a
	 * UAF access to @data. And the semantics of memory barrier in it
	 * also make sure the waiter will see the latest @data->got_token
	 * once list_empty_careful() in finish_wait() returns true.
	 */
	list_del_init_careful(&curr->entry); /* waitqueue에서 현재 entry 안전하게 제거 (smp_mb 내장) */
	return 1; /* wake 성공: 하나의 waiter를 실제로 깨움 */
}

/**
 * rq_qos_wait - throttle on a rqw if we need to
 * @rqw: rqw to throttle on
 * @private_data: caller provided specific data
 * @acquire_inflight_cb: inc the rqw->inflight counter if we can
 * @cleanup_cb: the callback to cleanup in case we race with a waker
 *
 * This provides a uniform place for the rq_qos users to do their throttling.
 * Since you can end up with a lot of things sleeping at once, this manages the
 * waking up based on the resources available.  The acquire_inflight_cb should
 * inc the rqw->inflight if we have the ability to do so, or return false if not
 * and then we will sleep until the room becomes available.
 *
 * cleanup_cb is in case that we race with a waker and need to cleanup the
 * inflight count accordingly.
 */
/*
 * rq_qos_wait() - NVMe queue에 진입할 budget이 없을 때 현재 태스크를
 * 대기시키고, budget이 생기면 깨워준다.
 *
 * 호출 경로(추정):
 * blk_mq_submit_bio() -> blk_mq_get_request() -> wbt/iocost throttle
 * -> rq_qos_wait() -> 깨어나면 nvme_queue_rq() -> nvme_submit_cmd(doorbell).
 *
 * NVMe 연결:
 * - rq_wait->inflight는 현재 NVMe SQ에 삽입되어 완료 대기 중인 명령 수다.
 * - limit은 NVMe SQ depth나 QoS 정책이 정한 최대 outstanding 명령 수다.
 * - acquire_inflight_cb가 true를 반환하면 caller는 곧바로 nvme_submit_cmd()
 *   를 통해 doorbell을 울릴 수 있다.
 * - false면 태스크를 TASK_UNINTERRUPTIBLE로 재우고, 완료 인터럽트(CQ
 *   entry 처리)에 의해 깨어난다.
 */
void rq_qos_wait(struct rq_wait *rqw, void *private_data,
		 acquire_inflight_cb_t *acquire_inflight_cb,
		 cleanup_cb_t *cleanup_cb)
{
	struct rq_qos_wait_data data = {
		.rqw		= rqw,
		.cb		= acquire_inflight_cb,
		.private_data	= private_data,
		.got_token	= false,
	}; /* 대기 엔트리 초기화: 아직 NVMe SQ budget 미획득 상태 */
	bool first_waiter;

	/*
	 * If there are no waiters in the waiting queue, try to increase the
	 * inflight counter if we can. Otherwise, prepare for adding ourselves
	 * to the waiting queue.
	 */
	if (!waitqueue_active(&rqw->wait) && acquire_inflight_cb(rqw, private_data)) /* 대기자 없고 budget 획득 성공 시 */
		return; /* NVMe SQ에 여유가 있으면 대기 없이 바로 doorbell 경로 진입 */

	init_wait_func(&data.wq, rq_qos_wake_function); /* 커스텀 wake 함수 등록: budget 획득 실패 시 wake loop 중단 */
	first_waiter = prepare_to_wait_exclusive(&rqw->wait, &data.wq,
						 TASK_UNINTERRUPTIBLE); /* waitqueue에 exclusive waiter로 등록 */
	/*
	 * Make sure there is at least one inflight process; otherwise, waiters
	 * will never be woken up. Since there may be no inflight process before
	 * adding ourselves to the waiting queue above, we need to try to
	 * increase the inflight counter for ourselves. And it is sufficient to
	 * guarantee that at least the first waiter to enter the waiting queue
	 * will re-check the waiting condition before going to sleep, thus
	 * ensuring forward progress.
	 */
	if (!data.got_token && first_waiter && acquire_inflight_cb(rqw, private_data)) { /* 첫 waiter가 추가 직후 budget 재시도 */
		finish_wait(&rqw->wait, &data.wq); /* waitqueue에서 제거하고 바로 진행 */
		/*
		 * We raced with rq_qos_wake_function() getting a token,
		 * which means we now have two. Put our local token
		 * and wake anyone else potentially waiting for one.
		 *
		 * Enough memory barrier in list_empty_careful() in
		 * finish_wait() is paired with list_del_init_careful()
		 * in rq_qos_wake_function() to make sure we will see
		 * the latest @data->got_token.
		 */
		if (data.got_token)
			cleanup_cb(rqw, private_data); /* race로 인해 중복 획득한 SQ budget 반납 */
		return;
	}

	/* we are now relying on the waker to increase our inflight counter. */
	do {
		if (data.got_token)
			break; /* NVMe CQ 완료 처리가 budget을 반납하여 획득 완료 */
		io_schedule(); /* NVMe SQ에 빈자리가 날 때까지 CPU 양보 */
		set_current_state(TASK_UNINTERRUPTIBLE); /* 다음 wake까지 UNINTERRUPTIBLE 상태 재설정 */
	} while (1);
	finish_wait(&rqw->wait, &data.wq); /* 정상 깨어남: waitqueue에서 안전하게 제거 */
}

/*
 * rq_qos_exit() - request_queue에 등록된 모든 rq_qos 모듈을 제거한다.
 *
 * 호출 경로: del_gendisk() / blk_cleanup_queue() -> rq_qos_exit()
 * -> 각 rq_qos->ops->exit().
 *
 * NVMe 연결: NVMe 장치 제거(hot-unplug) 시 wbt/iocost 등이 NVMe queue
 * 와 관련된 상태를 정리할 기회를 준다.
 */
void rq_qos_exit(struct request_queue *q)
{
	mutex_lock(&q->rq_qos_mutex); /* QoS 체인 수정을 위한 동기화: NVMe queue 제거 중 race 방지 */
	while (q->rq_qos) { /* 등록된 모든 QoS 모듈을 순회하며 제거 */
		struct rq_qos *rqos = q->rq_qos;
		q->rq_qos = rqos->next; /* 연결 리스트에서 제거 후 다음 QoS 모듈로 이동 */
		rqos->ops->exit(rqos); /* 정책 모듈별 NVMe 관련 상태 해제 */
	}
	blk_queue_flag_clear(QUEUE_FLAG_QOS_ENABLED, q); /* 이후 blk_mq_submit_bio()에서 QoS 경로 비활성화 */
	mutex_unlock(&q->rq_qos_mutex);
}

/*
 * rq_qos_add() - 새로운 QoS 모듈(wbt, iocost 등)을 request_queue의
 * rq_qos 연결 리스트에 등록한다.
 *
 * 호출 경로: blk-wbt.c 또는 blk-iocost.c 초기화 -> rq_qos_add()
 * -> 이후 blk_mq_submit_bio() 경로에서 __rq_qos_throttle() 등이 호출됨.
 *
 * NVMe 연결: NVMe 장치 초기화 단계에서 queue depth/지연 기반 QoS를
 * 활성화한다. 등록 후 bio는 blk_mq_get_request() 이전에 throttle을
 * 거치며, 이는 nvme_queue_rq() -> nvme_submit_cmd()로 흘러가는 명령
 * 수를 제어한다.
 */
int rq_qos_add(struct rq_qos *rqos, struct gendisk *disk, enum rq_qos_id id,
		const struct rq_qos_ops *ops)
{
	struct request_queue *q = disk->queue; /* NVMe namespace의 request_queue 획득 */
	unsigned int memflags;

	lockdep_assert_held(&q->rq_qos_mutex); /* QoS 등록은 mutex 보유 상태에서만 수행 */

	rqos->disk = disk; /* NVMe namespace gendisk 연결 */
	rqos->id = id; /* RQ_QOS_WBT / LATENCY / COST 중 하나 */
	rqos->ops = ops; /* throttle/issue/done/... NVMe 생명주기 콜백 테이블 연결 */

	/*
	 * No IO can be in-flight when adding rqos, so freeze queue, which
	 * is fine since we only support rq_qos for blk-mq queue.
	 */
	memflags = blk_mq_freeze_queue(q); /* NVMe queue에 새로운 request가 들어오지 않도록 동결 */

	if (rq_qos_id(q, rqos->id))
		goto ebusy; /* 동일 id의 QoS 모듈이 이미 등록된 NVMe queue */
	rqos->next = q->rq_qos; /* 기존 체인의 head 앞에 삽입 */
	q->rq_qos = rqos; /* 새 QoS 모듈을 체인 head로 설정 */
	blk_queue_flag_set(QUEUE_FLAG_QOS_ENABLED, q); /* 이후 blk_mq_submit_bio()에서 QoS 경로 활성화 */

	blk_mq_unfreeze_queue(q, memflags); /* NVMe queue 동결 해제, 이제 QoS 적용됨 */
	return 0;
ebusy:
	blk_mq_unfreeze_queue(q, memflags); /* 등록 실패 시에도 동결 해제 */
	return -EBUSY;
}

/*
 * rq_qos_del() - 지정한 rq_qos 모듈을 request_queue에서 제거한다.
 *
 * 호출 경로: 정책 모듈 종료 시 -> rq_qos_del() -> rq_qos 체인에서 제거.
 *
 * NVMe 연결: NVMe queue의 QoS 정책을 런타임에 해제할 때 사용된다.
 * 제거 후에는 __rq_qos_* 호출 체인에서 해당 모듈이 빠진다.
 */
void rq_qos_del(struct rq_qos *rqos)
{
	struct request_queue *q = rqos->disk->queue; /* 대상 NVMe namespace의 request_queue */
	struct rq_qos **cur;
	unsigned int memflags;

	lockdep_assert_held(&q->rq_qos_mutex); /* QoS 체인 수정 동기화 */

	memflags = blk_mq_freeze_queue(q); /* 제거 중 NVMe queue 동결 */
	for (cur = &q->rq_qos; *cur; cur = &(*cur)->next) { /* QoS 연결 리스트 순회 */
		if (*cur == rqos) {
			*cur = rqos->next; /* rq_qos 연결 리스트에서 제거 */
			break;
		}
	}
	if (!q->rq_qos)
		blk_queue_flag_clear(QUEUE_FLAG_QOS_ENABLED, q); /* 더 이상 QoS 모듈이 없으면 플래그 해제 */
	blk_mq_unfreeze_queue(q, memflags); /* NVMe queue 동결 해제 */
}

/* NVMe 관점 핵심 요약
 * - blk-rq-qos.c는 blk-mq와 NVMe 드라이버 사이의 QoS 중간층으로, bio/request가
 *   nvme_queue_rq() -> nvme_submit_cmd(doorbell)로 진입하기 전 대역폭/지연/깊이를
 *   조절한다.
 * - rq_depth의 max_depth/scale_step은 NVMe SQ에 동시에 삽입될 수 있는
 *   outstanding 명령 수(CID 단위)의 상한을 결정한다.
 * - rq_qos_wait()는 SQ budget이 부족하면 태스크를 재우고, CQ 완료 인터럽트
 *   경로에서 깨워 doorbell 진입 기회를 공정하게 분배한다.
 * - __rq_qos_*() 함수들은 wbt/iocost 같은 정책 모듈의 콜백 체인을 순회하며,
 *   각 단계에서 NVMe 명령의 생성/발행/완료/재배열/병합/정리를 관찰한다.
 * - 이 파일은 block/blk-mq.c(요청 할당/완료), block/blk-wbt.c(쓰기 지연 제어),
 *   block/blk-iocost.c(비용 기반 제어), drivers/nvme/host/pci.c(SQ/CQ/doorbell)와
 *   긴접하게 연결된다.
 */
