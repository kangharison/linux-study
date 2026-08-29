// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어 설명] 블록 계층 요청 QoS(Quality of Service) 플러그인 프레임워크 (blk-rq-qos.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 blk-mq 기반 블록 디바이스의 I/O 요청 품질 서비스(QoS) 플러그인 체인을
 * 관리하는 핵심 인프라를 구현한다. request_queue에 연결된 rq_qos 연결 리스트를 통해
 * 여러 QoS 정책 모듈(blk-wbt: 쓰기 대역폭 쓰로틀링, blk-iolatency: cgroup 지연 제어,
 * blk-iocost: 비용 기반 제어)을 동시에 적용할 수 있다. bio가 request로 변환되기 전부터
 * request가 완료되어 호출자에게 반환되기까지의 전 생명주기에 걸쳐 각 QoS 모듈의
 * ops vtable(throttle/track/merge/issue/done/requeue/cleanup/done_bio)을 순차 호출한다.
 * rq_depth를 통한 동적 queue depth 조절과 rq_qos_wait()/rq_qos_wake_function()을
 * 통한 sleep/wake 메커니즘을 제공하여 하드웨어의 실제 처리 능력에 맞게 I/O를 조절한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * I/O 흐름에서 이 파일은 blk-mq와 상위 정책 모듈 사이의 중간 계층에 위치한다.
 * 주요 호출 체인은 다음과 같다:
 *   [bio 제출] blk_mq_submit_bio → rq_qos_throttle(__rq_qos_throttle) [진입 쓰로틀]
 *            → blk_mq_get_request → rq_qos_track(__rq_qos_track) [bio-rq 매핑]
 *   [bio 병합] blk_attempt_bio_merge → rq_qos_merge(__rq_qos_merge) [병합 통보]
  *   [드라이버 발행] blk_mq_dispatch_rq_list → rq_qos_issue(__rq_qos_issue) [발행 직전]
  *   [완료]         blk_mq_complete_request → rq_qos_done(__rq_qos_done) [완료 정산]
 *   [bio 완료] bio_endio → rq_qos_done_bio(__rq_qos_done_bio) [bio 레벨 정산]
 *   [재배열]   blk_mq_requeue_request → rq_qos_requeue(__rq_qos_requeue) [budget 반납]
 *   [오류정리] blk_mq_end_request(error) → rq_qos_cleanup(__rq_qos_cleanup) [상태 복구]
 * 실행 컨텍스트: throttle/track/merge는 프로세스 컨텍스트(bio 제출 경로),
 *               done/done_bio는 softirq/IRQ(완료 경로), issue는 kworker/softirq.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 모듈:
 *   - blk-mq.c: blk_mq_freeze_queue()/blk_mq_unfreeze_queue()로 큐 동결을 수행하여
 *     rq_qos 등록/제거 시 in-flight request와의 race를 방지한다.
 *   - include/linux/blk-mq.h: request_queue, struct gendisk 등 핵심 자료구조 정의.
 * 이 파일에 의존하는 모듈(정책 플러그인):
 *   - blk-wbt.c: rq_qos_add()로 RQ_QOS_WBT 플러그인을 등록. throttle에서 토큰 획득을
 *     시도하고 없으면 rq_qos_wait()으로 대기. done에서 토큰 반납 및 waiter 깨움.
 *   - blk-iolatency.c: RQ_QOS_LATENCY 플러그인. 목표 지연 초과 시 throttle_delay 조정.
 *   - blk-iocost.c: RQ_QOS_COST 플러그인. vtime 기반 비용 모델로 cgroup I/O 제어.
 * 공유하는 핵심 자료구조:
 *   - struct rq_qos: 개별 QoS 플러그인 객체. ops vtable, disk, id, next 포함.
 *   - struct rq_wait: inflight 카운터와 waitqueue. budget 관리의 핵심.
 *   - struct rq_depth: max_depth, scale_step, queue_depth. 동적 depth 조절.
 *
 * === 주요 함수/구조체 요약 ===
 * __rq_qos_throttle()        — bio 제출 전 모든 QoS 플러그인의 throttle 콜백 체인 호출
 * rq_qos_wait()              — budget 부족 시 태스크를 TASK_UNINTERRUPTIBLE로 재우고 깨움
 * rq_qos_wake_function()     — waitqueue에서 깨어날 때 budget 획득을 시도하는 wake 핸들러
 * rq_depth_calc_max_depth()  — scale_step에 따라 max_depth를 동적으로 산출
 * rq_qos_add() / rq_qos_del()— 큐 동결 후 rq_qos 체인에 플러그인 등록/제거
 * struct rq_wait             — inflight 카운터 + waitqueue: QoS 쓰로틀링의 핵심 상태
 * struct rq_depth            — max_depth/scale_step으로 동적 queue depth 한도 표현
 * struct rq_qos_wait_data    — rq_qos_wait() 내부 대기 엔트리: budget 획득 콜백과 결과 저장
 */

#include "blk-rq-qos.h"
/* [한국어] blk-rq-qos.h: struct rq_qos, struct rq_wait, struct rq_depth, rq_qos_id 열거,
 *         acquire_inflight_cb_t, cleanup_cb_t 등 이 파일이 구현하는 모든 자료구조·타입 선언.
 *         또한 inline 래퍼(__rq_qos_*를 호출하는 rq_qos_*())도 이 헤더에 정의됨. */

/*
 * [한국어]
 * atomic_inc_below - 원자적으로 값을 증가시키되, 지정된 상한을 넘지 않도록 한다.
 *
 * @v:     증가시킬 atomic_t 변수. 일반적으로 rq_wait->inflight(현재 in-flight 요청 수).
 * @below: 허용하는 상한(exclusive). v의 현재 값이 below 이상이면 증가하지 않음.
 * @return: 증가에 성공하면 true(budget 획득 성공), 상한 초과로 실패하면 false.
 *
 * CAS(Compare-And-Swap) 루프를 사용하여 비원자적인 "읽기-비교-증가" 연산을 원자적으로
 * 수행한다. SMP 환경에서 두 CPU가 동시에 증가를 시도하더라도 정확히 하나만 상한에 도달할
 * 수 있다. atomic_try_cmpxchg()는 v의 현재 값이 여전히 cur와 같으면 cur+1로 바꾸고
 * true를 반환(성공), 다른 CPU가 먼저 바꿨으면 cur를 새 값으로 갱신하고 false를 반환.
 * 실행 컨텍스트: bio 제출 경로(프로세스) 또는 wakeup 함수(rq_qos_wake_function, softirq).
 * 재진입 안전: 락 없이 atomic 연산만 사용하므로 어느 컨텍스트에서도 안전.
 *
 * 호출 체인:
 *   rq_wait_inc_below → [이 함수]
 *   rq_qos_wake_function → acquire_inflight_cb → rq_wait_inc_below → [이 함수]
 */
static bool atomic_inc_below(atomic_t *v, unsigned int below)
{
	unsigned int cur = atomic_read(v);  /* [한국어] 현재 in-flight 요청 수를 원자적으로 읽어
	                                     *         CAS 비교 기준값(cur)으로 설정 */

	do {
		if (cur >= below)    /* [한국어] 현재 값이 상한에 도달하거나 초과: budget 없음.
		                      *         다른 CPU의 증가로 cur가 below 이상이 된 경우 포함 */
			return false;  /* [한국어] budget 획득 실패: 호출자는 rq_qos_wait()으로 대기해야 함 */
	} while (!atomic_try_cmpxchg(v, &cur, cur + 1));
	/* [한국어] atomic_try_cmpxchg(v, &cur, cur+1):
	 *         성공(true): v == cur 상태에서 v를 cur+1로 원자적으로 변경. 루프 탈출.
	 *         실패(false): 다른 CPU가 v를 변경한 경우. &cur에 새 값을 써넣고 재시도.
	 *         do-while 구조이므로 성공 시 while 조건이 false가 되어 루프 종료. */

	return true;  /* [한국어] CAS 성공: in-flight 카운터가 cur+1로 원자적으로 증가됨.
	               *         호출자는 이제 SQ에 새 request를 진입시킬 수 있다. */
}

/*
 * [한국어]
 * rq_wait_inc_below - rq_wait->inflight를 limit 미만에서 원자적으로 증가시킨다.
 *
 * @rq_wait: inflight 카운터와 waitqueue를 담은 rq_wait 포인터.
 *           일반적으로 rq_wb->rq_wait(wbt) 또는 iolatency의 rq_wait.
 * @limit:   허용하는 in-flight 상한. rq_depth->max_depth 또는 정책별 파라미터.
 * @return:  증가 성공 시 true, limit 초과로 실패 시 false.
 *
 * atomic_inc_below()의 공개 래퍼. QoS 정책 모듈이 acquire_inflight_cb_t 타입의
 * 콜백으로 이 함수(또는 이를 래핑한 함수)를 사용하여 rq_qos_wait()에 전달한다.
 * 실행 컨텍스트: 프로세스(bio 제출) 또는 wakeup softirq. atomic 연산만 사용.
 *
 * 호출 체인:
 *   wbt_acquire → rq_wait_inc_below → atomic_inc_below
 *   rq_qos_wake_function → cb(data->cb) → rq_wait_inc_below → atomic_inc_below
 */
bool rq_wait_inc_below(struct rq_wait *rq_wait, unsigned int limit)
{
	return atomic_inc_below(&rq_wait->inflight, limit);
	/* [한국어] rq_wait->inflight를 limit 미만에서만 원자적으로 증가:
	 *         limit은 보통 rq_depth->max_depth로, 동적으로 조절되는 SQ 깊이 한도 */
}

/*
 * [한국어] struct rq_qos 연결 리스트 체인 관련 참고:
 * request_queue->rq_qos는 등록된 QoS 플러그인의 연결 리스트 헤드이다.
 * 각 rq_qos는 ->next 포인터로 다음 플러그인을 가리키며, 마지막 원소의 next는 NULL.
 * __rq_qos_*() 함수들은 모두 이 체인을 do { ... } while (rqos) 패턴으로 순회한다.
 * 체인 순서: rq_qos_add()는 head에 삽입하므로 나중에 등록된 플러그인이 먼저 호출됨.
 */

/*
 * [한국어]
 * __rq_qos_cleanup - bio 오류/정리 시 등록된 모든 QoS 플러그인의 cleanup 콜백을 순회 호출.
 *
 * @rqos: request_queue->rq_qos (체인의 첫 번째 플러그인).
 * @bio:  정리 대상 bio. cleanup 콜백에 전달되어 각 플러그인이 이 bio와 관련된
 *        상태(inflight 카운터, cgroup 비용 등)를 롤백할 수 있게 한다.
 * @return: 없음 (void).
 *
 * bio가 request로 변환되기 전에 오류가 발생하거나, blk_mq_end_request()의
 * 오류 경로에서 이미 할당된 QoS budget을 되돌려야 할 때 호출된다.
 * cleanup은 throttle의 역방향 연산이다: throttle에서 inflight를 증가시켰거나
 * 비용을 예약했다면 cleanup에서 그것을 원상복구한다.
 * 실행 컨텍스트: 프로세스 또는 softirq(오류 완료 경로). ops->cleanup의 컨텍스트 의존.
 *
 * 호출 체인:
 *   rq_qos_cleanup (inline, blk-rq-qos.h) → [이 함수] → ops->cleanup (플러그인별)
 */
void __rq_qos_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->cleanup)             /* [한국어] 이 플러그인이 cleanup 콜백을 구현했는지 확인:
		                                     *         wbt는 cleanup 미구현, iocost는 구현 */
			rqos->ops->cleanup(rqos, bio);  /* [한국어] 이 bio에 대해 이미 예약된 비용/budget 반납:
			                                 *         예) iocost의 vtime 비용 환불 */
		rqos = rqos->next;                  /* [한국어] 체인의 다음 QoS 플러그인으로 이동 */
	} while (rqos);                         /* [한국어] next가 NULL이면(마지막 플러그인) 순회 종료 */
}

/*
 * [한국어]
 * __rq_qos_done - request 완료 시 등록된 모든 QoS 플러그인의 done 콜백을 순회 호출.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @rq:   완료된 request. done 콜백이 이 request에 할당된 비용·budget을 회수.
 * @return: 없음 (void).
 *
 * blk_mq_complete_request()에서 드라이버가 완료를 보고한 뒤 호출된다. 각 플러그인은 done에서
 * inflight 카운터를 감소시키고, waitqueue에 대기 중인 태스크를 깨운다.
 * wbt의 경우 done에서 wbt_rqw_done()이 호출되어 inflight를 감소하고
 * wake_up_nr(&rq_wait->wait, 1)로 대기 중인 bio 제출 경로를 깨운다.
 * 실행 컨텍스트: softirq 또는 하드 IRQ(완료 처리 경로). ops->done은 atomic 연산만 사용해야 함.
 *
 * 호출 체인:
 *   blk_mq_complete_request → rq_qos_done (inline) → [이 함수] → ops->done
 */
void __rq_qos_done(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->done)              /* [한국어] done 콜백 존재 확인: 대부분의 플러그인이 구현 */
			rqos->ops->done(rqos, rq);    /* [한국어] 이 request의 inflight 감소 및 waiter 깨움:
			                               *         예) wbt: atomic_dec(&rqw->inflight) + wake_up */
		rqos = rqos->next;                /* [한국어] 다음 QoS 플러그인으로 체인 순회 */
	} while (rqos);
}

/*
 * [한국어]
 * __rq_qos_issue - request가 드라이버에 발행되기 직전 모든 QoS 플러그인의 issue 콜백을 순회 호출.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @rq:   발행(issue)될 request. issue 콜백이 이 시점을 기록하거나 상태를 갱신.
 * @return: 없음 (void).
 *
 * blk_mq_dispatch_rq_list()에서 실제로 드라이버에 request를 넘기기 직전에
 * 호출된다. throttle에서 예약한 budget이 실제 발행으로 이어지는 시점을 플러그인에
 * 알려줘 더 정밀한 비용 계산이나 타이밍 측정이 가능하게 한다.
 * 실행 컨텍스트: kworker/softirq(dispatch 경로). ops->issue는 sleep 없이 짧게 끝나야 함.
 *
 * 호출 체인:
 *   blk_mq_dispatch_rq_list → rq_qos_issue (inline) → [이 함수] → ops->issue
 */
void __rq_qos_issue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->issue)             /* [한국어] issue 콜백 존재 확인 */
			rqos->ops->issue(rqos, rq);   /* [한국어] 드라이버 발행 시점 통보: 타이밍 기록이나 상태 갱신 */
		rqos = rqos->next;                /* [한국어] 다음 플러그인 순회 */
	} while (rqos);
}

/*
 * [한국어]
 * __rq_qos_requeue - request가 재배열(requeue)될 때 모든 QoS 플러그인의 requeue 콜백을
 *                   순회 호출한다.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @rq:   재배열될 request. requeue 콜백이 이 request에 사용된 budget을 원상복구.
 * @return: 없음 (void).
 *
 * blk_mq_requeue_request()에서 드라이버의 ->queue_rq()가 BLK_STS_RESOURCE(자원 부족)를
 * 반환하거나
 * 다른 이유로 request를 재배열할 때 호출된다. throttle/issue에서 할당된 budget이
 * 아직 완료되지 않은 채로 requeue되므로, 각 플러그인이 해당 budget을 되돌려야
 * forward progress가 보장된다.
 * 실행 컨텍스트: 프로세스 또는 softirq(에러 requeue 경로).
 *
 * 호출 체인:
 *   blk_mq_requeue_request → rq_qos_requeue (inline) → [이 함수] → ops->requeue
 */
void __rq_qos_requeue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->requeue)             /* [한국어] requeue 콜백 존재 확인 */
			rqos->ops->requeue(rqos, rq);   /* [한국어] 발행에 실패해 되돌아온 request의 budget 반납 및
			                                 *         waiter 깨우기 */
		rqos = rqos->next;                  /* [한국어] 다음 플러그인 순회 */
	} while (rqos);
}

/*
 * [한국어]
 * __rq_qos_throttle - bio를 request로 변환하기 전 모든 QoS 플러그인의 throttle 콜백을
 *                    순회 호출한다.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @bio:  throttle 대상 bio. throttle 콜백이 이 bio를 기반으로 budget 획득을 시도하거나
 *        rq_qos_wait()으로 대기.
 * @return: 없음 (void).
 *
 * blk_mq_submit_bio()에서 blk_mq_get_request() 이전에 호출된다. 이것이 QoS의 진입
 * 관문이다: 각 플러그인은 throttle에서 inflight budget을 획득하거나(rq_wait_inc_below),
 * budget이 부족하면 rq_qos_wait()으로 현재 태스크를 재운다. bio 제출 경로이므로
 * 프로세스 컨텍스트에서 실행되어 블로킹이 허용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(bio 제출 경로). 블로킹 가능(sleep).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → rq_qos_throttle (inline) → [이 함수] →
 *   ops->throttle → (budget 없으면) rq_qos_wait → io_schedule
 */
void __rq_qos_throttle(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->throttle)              /* [한국어] throttle 콜백 존재 확인 */
			rqos->ops->throttle(rqos, bio);   /* [한국어] 이 bio의 진입 허가 획득 시도:
			                                   *         budget 있으면 inflight 증가 후 즉시 반환,
			                                   *         없으면 rq_qos_wait()으로 블로킹 대기 */
		rqos = rqos->next;                    /* [한국어] 다음 플러그인 순회: 체인의 모든 정책 적용 */
	} while (rqos);
}

/*
 * [한국어]
 * __rq_qos_track - bio가 request에 연결(track)될 때 모든 QoS 플러그인의 track 콜백을
 *                 순회 호출한다.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @rq:   이 bio가 연결된 request.
 * @bio:  연결 대상 bio. track 콜백이 이 bio를 rq에 연결하여 QoS 컨텍스트를 추적.
 * @return: 없음 (void).
 *
 * blk_mq_get_request()에서 request가 할당된 직후, bio가 그 request에 매핑될 때 호출된다.
 * 각 플러그인은 이 시점에서 bio의 cgroup, 비용 등을 request에 연결하여 이후 issue/done
 * 단계에서 올바른 정책 처리가 이루어지게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(bio 제출 경로).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_mq_get_request → rq_qos_track (inline) → [이 함수] → ops->track
 */
void __rq_qos_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->track)               /* [한국어] track 콜백 존재 확인 */
			rqos->ops->track(rqos, rq, bio); /* [한국어] bio → request 연결: cgroup, 비용 컨텍스트 등
			                                  *         request에 추적 정보 첨부 */
		rqos = rqos->next;                  /* [한국어] 다음 플러그인 순회 */
	} while (rqos);
}

/*
 * [한국어]
 * __rq_qos_merge - bio가 기존 request에 병합될 때 모든 QoS 플러그인의 merge 콜백을
 *                 순회 호출한다.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @rq:   bio가 병합될 기존 request.
 * @bio:  병합될 bio.
 * @return: 없음 (void).
 *
 * blk_attempt_bio_merge()에서 bio가 기존 request에 병합될 때 호출된다. 두 개의 bio가
 * 하나의 request(단일 SQ 명령)로 합쳐지므로, 각 플러그인이 두 bio의 비용·budget을
 * 하나로 재계산해야 한다. 예를 들어 iocost는 병합된 bio의 총 크기로 vtime 비용을 재산출.
 * 실행 컨텍스트: 프로세스 컨텍스트(bio 제출 경로).
 *
 * 호출 체인:
 *   blk_mq_submit_bio → blk_attempt_bio_merge → rq_qos_merge (inline) →
 *   [이 함수] → ops->merge
 */
void __rq_qos_merge(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->merge)               /* [한국어] merge 콜백 존재 확인 */
			rqos->ops->merge(rqos, rq, bio); /* [한국어] bio 병합 후 단일 request의 비용 재계산:
			                                  *         두 bio의 합산 크기/지연 목표로 조정 */
		rqos = rqos->next;                  /* [한국어] 다음 플러그인 순회 */
	} while (rqos);
}

/*
 * [한국어]
 * __rq_qos_done_bio - bio가 완료(bio_endio)될 때 모든 QoS 플러그인의 done_bio 콜백을
 *                    순회 호출한다.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @bio:  완료된 bio. done_bio 콜백이 이 bio에 해당하는 비용을 최종 정산.
 * @return: 없음 (void).
 *
 * bio_endio()에서 호출된다. request 단위 완료(__rq_qos_done)와 별개로 bio 단위의
 * 완료 통지를 제공한다. request에 여러 bio가 병합된 경우 각 bio의 완료 시점에
 * 개별적으로 호출된다. iolatency의 경우 이 시점에서 지연을 측정하고 throttle_delay를 갱신.
 * 실행 컨텍스트: softirq 또는 IRQ(bio 완료 경로).
 *
 * 호출 체인:
 *   bio_endio → rq_qos_done_bio (inline) → [이 함수] → ops->done_bio
 */
void __rq_qos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->done_bio)              /* [한국어] done_bio 콜백 존재 확인 */
			rqos->ops->done_bio(rqos, bio);   /* [한국어] bio 레벨 완료 정산: 지연 측정·비용 최종 반영 */
		rqos = rqos->next;                    /* [한국어] 다음 플러그인 순회 */
	} while (rqos);
}

/*
 * [한국어]
 * __rq_qos_queue_depth_changed - queue depth가 변경됐을 때 모든 QoS 플러그인의
 *                                queue_depth_changed 콜백을 순회 호출한다.
 *
 * @rqos: 체인의 첫 번째 QoS 플러그인.
 * @return: 없음 (void).
 *
 * blk_mq_update_nr_hw_queues() 등에서 하드웨어 큐의 깊이가 변경될 때 호출된다.
 * 각 플러그인은 새 hardware queue depth에 맞게 rq_depth->queue_depth를 갱신하고,
 * 이를 바탕으로 rq_depth_calc_max_depth()를 재호출하여 max_depth를 재조정한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(queue 설정 변경 경로).
 *
 * 호출 체인:
 *   blk_mq_update_nr_hw_queues → rq_qos_queue_depth_changed (inline) →
 *   [이 함수] → ops->queue_depth_changed
 */
void __rq_qos_queue_depth_changed(struct rq_qos *rqos)
{
	do {
		if (rqos->ops->queue_depth_changed)             /* [한국어] queue_depth_changed 콜백 존재 확인 */
			rqos->ops->queue_depth_changed(rqos);       /* [한국어] 새 hardware queue depth로 max_depth 재조정 */
		rqos = rqos->next;                              /* [한국어] 다음 플러그인 순회 */
	} while (rqos);
}

/*
 * [한국어] struct rq_depth 관련 참고:
 * rq_depth는 QoS 플러그인이 in-flight request 수를 동적으로 조절하기 위한 상태 기계이다.
 * scale_step의 부호와 크기로 max_depth를 지수적으로 늘리거나 줄여, 지연 피드백에 빠르게
 * 반응하면서도 oscillation을 줄인다.
 *
 * 동작 원리:
 *   scale_step == 0 : 기본 상태. max_depth = min(default_depth, queue_depth)
 *   scale_step > 0  : 지연 증가/타임아웃. max_depth를 1 + (depth-1) >> scale_step으로 축소.
 *                     scale_step이 클수록 더 많이 축소 (지수적 감소).
 *   scale_step < 0  : 쓰기 집중 또는 지연 양호. max_depth를 1 + (depth-1) << (-scale_step)으로
 *                     확장. 단, queue_depth * 75%를 넘지 않음.
 */

/*
 * [한국어]
 * rq_depth_calc_max_depth - 현재 scale_step에 따라 rq_depth->max_depth를 산출하고 갱신한다.
 *
 * @rqd: 갱신할 rq_depth 포인터. max_depth, scale_step, queue_depth, default_depth 포함.
 * @return: true이면 이미 최대 한도에 도달하여 더 이상 확장 불가(rq_depth_scale_up 중단 신호),
 *          false이면 아직 확장 여지 있음.
 *
 * queue_depth==1인 특수 케이스를 처리하고, 일반 경우에는 scale_step 부호에 따라
 * max_depth를 지수적으로 축소(scale_step>0) 또는 확장(scale_step<0)한다.
 * scale_step<0 확장 시 queue_depth * 3/4를 상한으로 두어 하드웨어를 과부하하지 않는다.
 * 실행 컨텍스트: 타이머/wbt 콜백(프로세스 또는 softirq). rqd는 단일 경로에서 수정됨.
 *
 * 호출 체인:
 *   rq_depth_scale_up → [이 함수]
 *   rq_depth_scale_down → [이 함수]
 */
bool rq_depth_calc_max_depth(struct rq_depth *rqd)
{
	unsigned int depth;  /* [한국어] 계산 중간 max_depth 값 */
	bool ret = false;    /* [한국어] 반환값 초기화: 기본은 확장 여지 있음(false) */

	/*
	 * For QD=1 devices, this is a special case. It's important for those
	 * to have one request ready when one completes, so force a depth of
	 * 2 for those devices. On the backend, it'll be a depth of 1 anyway,
	 * since the device can't have more than that in flight. If we're
	 * scaling down, then keep a setting of 1/1/1.
	 */
	if (rqd->queue_depth == 1) {  /* [한국어] 한 번에 한 건만 받는 장치(구형 ATA, 일부 가상/루프백 장치 등).
	                               *         지수적 스케일링 공식이 의미가 없어 아예 따로 처리한다. */
		if (rqd->scale_step > 0) /* [한국어] 축소 방향으로 가고 있으면 더 줄일 곳이 1밖에 없다 */
			rqd->max_depth = 1;  /* [한국어] 지연 악화로 scale_down 중이면 max_depth도 1로 고정 */
		else {
			rqd->max_depth = 2;  /* [한국어] 굳이 2를 주는 이유: 한 건이 완료되는 순간 다음 한 건이
			                      *         이미 소프트웨어 큐에 서 있어야 장치가 놀지 않는다.
			                      *         하드웨어에 실제로 실리는 건 여전히 1개지만,
			                      *         '완료 → 다음 발행' 사이의 공백이 사라진다. */
			ret = true;          /* [한국어] depth=2가 이 장치의 사실상 최대 한도: 확장 불가 신호 */
		}
	} else {
		/*
		 * scale_step == 0 is our default state. If we have suffered
		 * latency spikes, step will be > 0, and we shrink the
		 * allowed write depths. If step is < 0, we're only doing
		 * writes, and we allow a temporarily higher depth to
		 * increase performance.
		 */
		/* [한국어] 기본 depth: default_depth와 hardware queue_depth 중 작은 값.
		 *         default_depth는 정책 모듈이 초기화 시 설정(예: wbt에서 queue_depth/4). */
		depth = min_t(unsigned int, rqd->default_depth,
			      rqd->queue_depth);  /* [한국어] 하드웨어 한도를 넘지 않도록 클립 */
		if (rqd->scale_step > 0)
			/* [한국어] 지연 증가/타임아웃 시 max_depth를 지수적으로 축소:
			 *         depth-1을 scale_step만큼 오른쪽 시프트 → 절반씩 감소.
			 *         min(31, scale_step)으로 시프트 오버플로우 방지.
			 *         +1은 최솟값 보장(depth가 1 미만이 되지 않도록). */
			depth = 1 + ((depth - 1) >> min(31, rqd->scale_step));
		else if (rqd->scale_step < 0) { /* [한국어] 음수 step은 '기본값보다 더 열어도 되는 상태' —
						 * wbt에서는 읽기가 하나도 없어 보호할 대상이 없을 때만 여기에 온다.
						 * 이때는 기본 depth를 넘어서 확장하므로 하드웨어 상한 검사가 필요하다. */
			/* [한국어] 처리량 개선 필요(scale_up 단계) 시 max_depth를 지수적으로 확장:
			 *         depth-1을 (-scale_step)만큼 왼쪽 시프트 → 두 배씩 증가. */
			unsigned int maxd = 3 * rqd->queue_depth / 4;
			/* [한국어] 확장 상한: queue_depth의 75%. 나머지 25%는 다른 시스템 부하를 위한 여유.
			 *         이 값을 넘으면 ret=true로 더 이상 scale_up을 시도하지 않음. */

			depth = 1 + ((depth - 1) << -rqd->scale_step);
			/* [한국어] 확장된 depth가 75% 상한을 넘으면 maxd로 클립 */
			if (depth > maxd) {
				depth = maxd;  /* [한국어] queue_depth * 3/4 상한 적용 */
				ret = true;    /* [한국어] 상한 도달: 더 이상 scale_up 불필요 신호 */
			}
		}

		rqd->max_depth = depth;  /* [한국어] 최종 산출된 max_depth 적용:
		                          *         이후 rq_wait_inc_below()의 limit으로 사용됨 */
	}

	return ret;  /* [한국어] true: 확장 한계에 도달(rq_depth_scale_up에서 scaled_max=true로 기록),
	              *         false: 아직 확장 여지 있음 */
}

/* Returns true on success and false if scaling up wasn't possible */
/*
 * [한국어]
 * rq_depth_scale_up - scale_step을 감소시켜 max_depth를 단계적으로 늘린다.
 *
 * @rqd: 갱신할 rq_depth 포인터.
 * @return: 성공(확장 수행) 시 true, 이미 최대 한도에 도달하여 확장 불가 시 false.
 *
 * 지연이 목표 범위 안으로 돌아오거나 처리량을 높여야 할 때 wbt/iolatency 타이머에서
 * 호출된다. scale_step을 1 감소시키고 rq_depth_calc_max_depth()로 새 max_depth를 산출.
 * 이미 scaled_max==true이면 더 이상 확장할 수 없으므로 false를 반환한다.
 * 실행 컨텍스트: 타이머 softirq(wbt_timer_fn 등).
 *
 * 호출 체인:
 *   wbt_timer_fn / iolatency_timer_fn → [이 함수] → rq_depth_calc_max_depth
 */
bool rq_depth_scale_up(struct rq_depth *rqd)
{
	/*
	 * Hit max in previous round, stop here
	 */
	if (rqd->scaled_max)   /* [한국어] 이전 라운드에서 max 한도에 도달했으면 확장 중단:
	                         *         oscillation 방지 및 불필요한 scale_up 억제 */
		return false;

	rqd->scale_step--;     /* [한국어] scale_step 감소: 음수에 가까워질수록 max_depth 증가.
	                         *         scale_step==0이 기본이므로 음수는 확장 단계 */

	rqd->scaled_max = rq_depth_calc_max_depth(rqd);  /* [한국어] 새 scale_step으로 max_depth 재산출:
	                                                    *         반환값이 true이면 scaled_max=true로 기록 */
	return true;  /* [한국어] 확장 수행 완료: max_depth가 갱신됨 */
}

/*
 * Scale rwb down. If 'hard_throttle' is set, do it quicker, since we
 * had a latency violation. Returns true on success and returns false if
 * scaling down wasn't possible.
 */
/*
 * [한국어]
 * rq_depth_scale_down - scale_step을 증가시켜 max_depth를 단계적으로 줄인다.
 *
 * @rqd:           갱신할 rq_depth 포인터.
 * @hard_throttle: true이면 scale_step을 0으로 즉시 리셋(강력 쓰로틀), false이면 1씩 증가.
 * @return: 성공(축소 수행) 시 true, 이미 최솟값(max_depth==1)이면 false.
 *
 * 지연이 목표를 초과하거나 타임아웃이 발생할 때 wbt/iolatency 타이머에서 호출된다.
 * hard_throttle=true는 심각한 지연 위반 시 확장 상태를 즉시 기본 상태로 되돌린다.
 * scale_step이 증가할수록 max_depth는 지수적으로 감소하여 in-flight 요청 수를 줄인다.
 * max_depth==1이면 더 이상 줄일 수 없으므로 false를 반환.
 * 실행 컨텍스트: 타이머 softirq(wbt_timer_fn 등).
 *
 * 호출 체인:
 *   wbt_timer_fn / iolatency_timer_fn → [이 함수] → rq_depth_calc_max_depth
 */
bool rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle)
{
	/*
	 * Stop scaling down when we've hit the limit. This also prevents
	 * ->scale_step from going to crazy values, if the device can't
	 * keep up.
	 */
	if (rqd->max_depth == 1)  /* [한국어] 이미 최솟값(max_depth=1): 더 이상 축소 불가.
	                            *         이 조건이 없으면 scale_step이 무한히 증가할 수 있음 */
		return false;

	if (rqd->scale_step < 0 && hard_throttle) /* [한국어] '기본보다 더 열어 둔 상태'에서 실제 지연 위반이 관측된 경우.
						   * 한 단계씩 되감으면 위반이 여러 윈도우 동안 계속되므로,
						   * 확장분을 통째로 취소해 곧바로 기본 상태로 되돌린다. */
		/* [한국어] 현재 확장 상태(scale_step<0)에서 강력 쓰로틀 요청:
		 *         scale_step을 즉시 0으로 리셋하여 기본 depth로 복귀.
		 *         점진적 축소 대신 즉각적인 정책 복귀가 필요한 심각한 지연 위반 상황. */
		rqd->scale_step = 0;
	else /* [한국어] 그 외(이미 기본/축소 구간이거나 soft 요청)는 한 단계씩만 조인다 */
		rqd->scale_step++;  /* [한국어] 점진적 축소: scale_step 1 증가 → max_depth 절반 감소.
		                      *         작은 지연 증가에는 점진적으로 반응하여 과잉 반응 방지 */

	rqd->scaled_max = false;         /* [한국어] 축소 후에는 scaled_max 리셋:
	                                   *         다음 라운드에서 scale_up이 다시 시도될 수 있게 허용 */
	rq_depth_calc_max_depth(rqd);    /* [한국어] 새 scale_step으로 max_depth 재산출 및 적용 */
	return true;  /* [한국어] 축소 수행 완료 */
}

/*
 * [한국어] struct rq_qos_wait_data — rq_qos_wait() 내부에서 대기 엔트리를 구성하는 구조체.
 *
 * rq_qos_wait()이 bio 제출 경로를 대기(sleep)시킬 때 스택에 할당하여 waitqueue에 등록.
 * wake 함수(rq_qos_wake_function)가 이 구조체를 통해 budget 획득을 시도하고 결과를 전달.
 */
struct rq_qos_wait_data {
	struct wait_queue_entry wq;
	/* [한국어] waitqueue에 등록되는 대기 엔트리.
	 * 설정자: rq_qos_wait()에서 init_wait_func()로 초기화 및 rq_qos_wake_function 등록.
	 * 읽는 자: __wake_up_common()이 wake 시 rq_qos_wake_function을 호출하여 budget 획득 시도.
	 * 값 범위: 유효한 wait_queue_entry (task, func, entry 포함).
	 * 동기화: rqw->wait의 spinlock으로 보호. list_del_init_careful()은 smp_mb 내장. */

	struct rq_wait *rqw;
	/* [한국어] 이 대기 엔트리가 budget을 기다리는 rq_wait 포인터.
	 * 설정자: rq_qos_wait()에서 data.rqw = rqw로 초기화.
	 * 읽는 자: rq_qos_wake_function()이 data->cb(data->rqw, ...)로 budget 획득 시도.
	 * 값 범위: NULL 불가. 유효한 rq_wait(inflight, wait 포함) 포인터.
	 * 동기화: wake 함수는 rqw->wait의 spinlock 보유 상태에서 호출됨. */

	acquire_inflight_cb_t *cb;
	/* [한국어] inflight budget 획득을 시도하는 콜백 함수 포인터 (acquire_inflight_cb_t).
	 * 설정자: rq_qos_wait()에서 data.cb = acquire_inflight_cb으로 설정.
	 * 읽는 자: rq_qos_wake_function()이 data->cb(data->rqw, data->private_data)로 호출.
	 * 값 범위: NULL 불가. 일반적으로 wbt_acquire 또는 iolatency_acquire.
	 * 동기화: 콜백 자체가 atomic 연산(atomic_inc_below)을 사용하므로 별도 락 불필요. */

	void *private_data;
	/* [한국어] acquire_inflight_cb에 전달되는 정책 모듈별 private 데이터.
	 * 설정자: rq_qos_wait()에서 data.private_data = private_data로 설정.
	 * 읽는 자: cb(rqw, private_data) 호출 시 인자로 전달. 정책 모듈이 내용 해석.
	 * 값 범위: NULL 가능. 내용은 정책 모듈에 따라 다름.
	 * 동기화: cb 내부에서만 접근하므로 콜백 동기화 정책에 위임. */

	bool got_token;
	/* [한국어] rq_qos_wake_function()이 budget 획득에 성공했는지 나타내는 플래그.
	 * 설정자: rq_qos_wake_function()이 cb() 성공 후 data->got_token = true로 설정.
	 * 읽는 자: rq_qos_wait()의 do-while 루프가 이 값으로 sleep 종료 여부를 결정.
	 * 값 범위: false(초기/획득 실패) 또는 true(budget 획득 성공).
	 * 동기화: list_del_init_careful()과 finish_wait()의 메모리 배리어로 가시성 보장.
	 *        wake 함수에서 true를 쓰고 list_del_init_careful()의 smp_mb 이후,
	 *        finish_wait()의 list_empty_careful()에서 정렬되어 최신 값이 보임. */
};

/*
 * [한국어]
 * rq_qos_wake_function - waitqueue에서 태스크를 깨울 때 budget 획득을 시도하는 커스텀 wake 함수.
 *
 * @curr:       현재 깨울 대기 엔트리. container_of로 rq_qos_wait_data를 복원.
 * @mode:       wake 모드(TASK_NORMAL 등). default_wake_function에 전달.
 * @wake_flags: 추가 wake 플래그. default_wake_function에 전달.
 * @key:        wake key(사용 안 함). default_wake_function에 전달.
 * @return:     -1이면 budget 획득 실패(wake loop 중단), 1이면 성공(태스크 깨움 완료).
 *
 * 이 함수는 __wake_up_common()에서 각 대기 엔트리를 깨울 때 호출된다. 표준 wake 함수와
 * 달리 budget 획득(cb() 호출)이 실패하면 -1을 반환하여 wake loop 자체를 중단시킨다.
 * 이는 budget이 하나 생겼을 때 딱 하나의 waiter만 깨우기 위한 핵심 메커니즘이다.
 * budget 획득 성공 시 got_token=true를 기록하고, default_wake_function()으로 태스크를
 * RUNNABLE 상태로 전환한 뒤, list_del_init_careful()로 waitqueue에서 제거한다.
 * 주의: list_del_init_careful()은 smp_mb()를 내장하여 got_token 쓰기가 리스트 제거보다
 * 먼저 완료됨을 보장한다. rq_qos_wait()의 finish_wait()가 list_empty_careful()로 항목
 * 제거를 확인할 때 got_token의 최신 값도 보임이 보장된다.
 * 실행 컨텍스트: __wake_up_common() 내부, rqw->wait.lock spinlock 보유 상태에서 호출.
 *
 * 호출 체인:
 *   __rq_qos_done → ops->done → wake_up → __wake_up_common → [이 함수]
 */
static int rq_qos_wake_function(struct wait_queue_entry *curr,
				unsigned int mode, int wake_flags, void *key)
{
	/* [한국어] wait_queue_entry에서 rq_qos_wait_data를 역산:
	 *         data.wq가 wait_queue_entry이므로 container_of로 포함 구조체 복원 */
	struct rq_qos_wait_data *data = container_of(curr,
						     struct rq_qos_wait_data,
						     wq);

	/*
	 * If we fail to get a budget, return -1 to interrupt the wake up loop
	 * in __wake_up_common.
	 */
	/* [한국어] budget 획득 시도: cb(rqw, private_data) → rq_wait_inc_below() → atomic_inc_below().
	 *         현재 inflight가 max_depth 미만이면 true(성공), 아니면 false(실패). */
	if (!data->cb(data->rqw, data->private_data))
		return -1;  /* [한국어] budget 획득 실패: -1을 반환하여 __wake_up_common의 wake loop를
		             *         즉시 중단. 이 budget에 대해 단 하나의 waiter만 깨우는 핵심 로직.
		             *         남은 waiter들은 다음 done 이벤트에서 깨어날 기회를 얻는다. */

	data->got_token = true;  /* [한국어] budget 획득 성공 표시: rq_qos_wait()의 do-while 루프가
	                          *         이 값을 보고 sleep을 종료한다 */
	/*
	 * autoremove_wake_function() removes the wait entry only when it
	 * actually changed the task state. We want the wait always removed.
	 * Remove explicitly and use default_wake_function().
	 */
	/* [한국어] 태스크를 RUNNABLE 상태로 전환: 이 태스크가 다음 스케줄링 기회에 CPU를 얻음 */
	default_wake_function(curr, mode, wake_flags, key);
	/*
	 * Note that the order of operations is important as finish_wait()
	 * tests whether @curr is removed without grabbing the lock. This
	 * should be the last thing to do to make sure we will not have a
	 * UAF access to @data. And the semantics of memory barrier in it
	 * also make sure the waiter will see the latest @data->got_token
	 * once list_empty_careful() in finish_wait() returns true.
	 */
	/* [한국어] waitqueue에서 엔트리를 안전하게 제거: list_del_init_careful()은 smp_mb()를
	 *         내장하여 got_token=true 쓰기와 태스크 wake 이후에 리스트 제거가 이루어짐을 보장.
	 *         finish_wait()의 list_empty_careful()이 smp_mb로 got_token 최신 값을 관측 가능.
	 *         이 호출 이후 @data는 스택 해제될 수 있으므로 UAF 방지를 위해 마지막으로 실행. */
	list_del_init_careful(&curr->entry);
	return 1;  /* [한국어] wake 성공: 이 대기 엔트리의 태스크가 깨어났음을 __wake_up_common에 알림 */
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
 * [한국어]
 * rq_qos_wait - budget이 부족할 때 현재 태스크를 TASK_UNINTERRUPTIBLE로 재우고,
 *               budget이 생기면 rq_qos_wake_function()을 통해 깨운다.
 *
 * @rqw:                 budget 상태(inflight, wait)를 담은 rq_wait 포인터.
 * @private_data:        acquire_inflight_cb과 cleanup_cb에 전달할 정책 모듈별 데이터.
 * @acquire_inflight_cb: inflight 카운터 획득 시도 콜백. 성공 시 true, 실패 시 false.
 * @cleanup_cb:          race 상황에서 이중 획득된 token을 반납하는 정리 콜백.
 * @return:              없음 (void). 반환 시 budget이 획득된 상태임이 보장됨.
 *
 * QoS 플러그인이 throttle 콜백에서 budget이 없을 때 이 함수를 호출한다. 호출 흐름:
 *   1. waitqueue에 대기자가 없고 budget을 즉시 획득할 수 있으면 바로 반환 (fast path).
 *   2. 그렇지 않으면 init_wait_func()로 커스텀 wake 함수를 등록하고, exclusive waiter로
 *      prepare_to_wait_exclusive()에 등록.
 *   3. 첫 번째 waiter인 경우 forward progress를 위해 budget 재시도(혹시 inflight가 비었을 때
 *      no waiter와 insert-to-waitqueue 사이의 race를 방지).
 *   4. io_schedule()로 CPU를 양보하며 대기. rq_qos_wake_function()이 budget 획득 후 깨움.
 *   5. got_token이 true이면 budget을 가진 채로 반환.
 * cleanup_cb는 3단계에서 acquire와 waker(rq_qos_wake_function)가 동시에 budget을 획득하는
 * race 상황에서 이중 획득된 token을 반납하기 위해 사용한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(bio 제출 경로). io_schedule()로 블로킹 가능.
 * 에러 경로: bio 제출은 TASK_UNINTERRUPTIBLE 대기이므로 시그널로는 깨어나지 않음.
 *            forward progress는 rq_qos_done() → wake_up_nr()에 의해 보장됨.
 *
 * 호출 체인:
 *   __rq_qos_throttle → ops->throttle (wbt/iolatency) → [이 함수] →
 *   io_schedule → (대기) → rq_qos_wake_function → (budget 획득) → 반환
 */
void rq_qos_wait(struct rq_wait *rqw, void *private_data,
		 acquire_inflight_cb_t *acquire_inflight_cb,
		 cleanup_cb_t *cleanup_cb)
{
	struct rq_qos_wait_data data = {
		.rqw		= rqw,               /* [한국어] 대기 대상 rq_wait: inflight, waitqueue 포함 */
		.cb		= acquire_inflight_cb,   /* [한국어] budget 획득 시도 콜백: wake 함수에서도 사용 */
		.private_data	= private_data,  /* [한국어] 정책별 private 데이터 */
		.got_token	= false,             /* [한국어] budget 미획득 초기 상태 */
	};  /* [한국어] 스택에 대기 엔트리 초기화: 아직 waitqueue에 등록되지 않은 상태 */
	bool first_waiter;  /* [한국어] 이 호출이 waitqueue에 추가된 첫 번째 waiter인지 여부 */

	/*
	 * If there are no waiters in the waiting queue, try to increase the
	 * inflight counter if we can. Otherwise, prepare for adding ourselves
	 * to the waiting queue.
	 */
	/* [한국어] Fast path: waitqueue에 대기자가 없고 budget을 즉시 획득 가능하면 바로 반환.
	 *         waitqueue_active()는 active waiter가 있는지 확인.
	 *         두 조건 모두 만족하면 sleep 없이 budget을 가진 채로 진행. */
	if (!waitqueue_active(&rqw->wait) && acquire_inflight_cb(rqw, private_data))
		return;  /* [한국어] budget 즉시 획득: 대기 없이 SQ 진입 가능 */

	/* [한국어] waitqueue 등록 준비: rq_qos_wake_function을 wake 핸들러로 설정.
	 *         표준 autoremove_wake_function 대신 커스텀 함수를 사용하는 이유:
	 *         budget 획득 실패 시 wake loop를 -1로 중단하여 불필요한 waiter 깨움 방지. */
	init_wait_func(&data.wq, rq_qos_wake_function);
	/* [한국어] waitqueue에 exclusive waiter로 등록 후 TASK_UNINTERRUPTIBLE 상태로 전환:
	 *         exclusive waiter는 wake_up_nr(q, 1)이 하나씩만 깨우는 대상이 됨.
	 *         first_waiter: 이 호출이 빈 waitqueue에 처음 진입했는지 여부. */
	first_waiter = prepare_to_wait_exclusive(&rqw->wait, &data.wq,
						 TASK_UNINTERRUPTIBLE);
	/*
	 * Make sure there is at least one inflight process; otherwise, waiters
	 * will never be woken up. Since there may be no inflight process before
	 * adding ourselves to the waiting queue above, we need to try to
	 * increase the inflight counter for ourselves. And it is sufficient to
	 * guarantee that at least the first waiter to enter the waiting queue
	 * will re-check the waiting condition before going to sleep, thus
	 * ensuring forward progress.
	 */
	/* [한국어] Forward progress 보장 로직 (첫 번째 waiter 전용):
	 *         fast path에서 budget 획득 실패 후 waitqueue에 추가되기 전에 모든 inflight가
	 *         완료되어 wake_up이 이미 발생했을 수 있다. 이 경우 아무도 이 waiter를 깨우지
	 *         않아 deadlock이 된다. 첫 waiter가 추가 직후 budget을 재시도하여 이를 방지. */
	if (!data.got_token && first_waiter && acquire_inflight_cb(rqw, private_data)) {
		/* [한국어] 재시도로 budget 획득 성공: waitqueue에서 제거하고 바로 진행 */
		finish_wait(&rqw->wait, &data.wq);  /* [한국어] waitqueue에서 안전하게 제거 및 RUNNING 복귀 */
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
		/* [한국어] Race 감지: rq_qos_wake_function()도 동시에 budget을 획득했을 수 있음.
		 *         finish_wait() 이후 got_token을 확인하여 이중 획득 여부를 감지.
		 *         smp_mb(finish_wait의 list_empty_careful)과
		 *         smp_mb(rq_qos_wake_function의 list_del_init_careful) 쌍으로 가시성 보장. */
		if (data.got_token)
			cleanup_cb(rqw, private_data);  /* [한국어] 이중 획득된 token 반납:
			                                 *         cleanup_cb은 atomic_dec(inflight)로
			                                 *         과도하게 획득한 budget을 되돌림 */
		return;  /* [한국어] budget 획득 완료 (재시도 경로) */
	}

	/* we are now relying on the waker to increase our inflight counter. */
	/* [한국어] Slow path: rq_qos_wake_function()이 budget을 획득하고 깨워줄 때까지 대기.
	 *         io_schedule()은 현재 태스크를 TASK_UNINTERRUPTIBLE로 수면 상태로 전환하고
	 *         CPU를 양보한다. I/O 대기 전용 스케줄러 통계(IO wait time)를 정확히 기록. */
	do {
		if (data.got_token)  /* [한국어] rq_qos_wake_function()이 budget 획득 후 got_token=true 설정:
		                      *         io_schedule 복귀 후 이 조건으로 루프 탈출 */
			break;
		io_schedule();  /* [한국어] CPU 양보 및 TASK_UNINTERRUPTIBLE 수면:
		                 *         rq_qos_done() → wake_up → rq_qos_wake_function()에 의해 깨어남 */
		set_current_state(TASK_UNINTERRUPTIBLE);  /* [한국어] 깨어난 후 다음 io_schedule을 위해
		                                           *         UNINTERRUPTIBLE 상태 재설정:
		                                           *         got_token이 false이면 다시 수면 */
	} while (1);
	finish_wait(&rqw->wait, &data.wq);  /* [한국어] 정상 깨어남: TASK_RUNNING 복귀 및
	                                      *         waitqueue에서 엔트리 제거 */
}

/*
 * [한국어]
 * rq_qos_exit - request_queue에 등록된 모든 QoS 플러그인을 순차적으로 제거한다.
 *
 * @q: QoS 플러그인을 제거할 request_queue.
 * @return: 없음 (void).
 *
 * del_gendisk() 또는 blk_cleanup_queue() 경로에서 디바이스 제거 시 호출된다.
 * rq_qos_mutex를 보유한 채 rq_qos 체인을 순회하며 각 플러그인의 ops->exit()을 호출하고,
 * 체인을 비운다. 마지막으로 QUEUE_FLAG_QOS_ENABLED를 클리어하여 이후 bio 제출 경로에서
 * QoS 체인 호출이 생략되게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 제거 경로). mutex_lock으로 블로킹 가능.
 *
 * 호출 체인:
 *   del_gendisk / blk_cleanup_queue → [이 함수] → ops->exit (각 플러그인 정리)
 */
void rq_qos_exit(struct request_queue *q)
{
	mutex_lock(&q->rq_qos_mutex);  /* [한국어] QoS 체인 수정을 위한 뮤텍스 획득:
	                                 *         rq_qos_add/del과의 race를 방지 */
	while (q->rq_qos) {            /* [한국어] 등록된 QoS 플러그인이 남아 있는 동안 반복 */
		struct rq_qos *rqos = q->rq_qos;  /* [한국어] 현재 체인 헤드(첫 번째 플러그인) 저장 */
		q->rq_qos = rqos->next;           /* [한국어] 체인 헤드를 다음으로 이동: 현재 플러그인 분리 */
		rqos->ops->exit(rqos);            /* [한국어] 플러그인별 정리: 타이머 삭제, 통계 해제,
		                                   *         메모리 반납 등 (예: wbt_exit, iolatency_exit) */
	}
	/* [한국어] QoS 플러그인 모두 제거 후 플래그 클리어:
	 *         이후 blk_mq_submit_bio()에서 rq_qos_throttle()을 건너뜀 */
	blk_queue_flag_clear(QUEUE_FLAG_QOS_ENABLED, q);
	mutex_unlock(&q->rq_qos_mutex);  /* [한국어] 뮤텍스 해제 */
}

/*
 * [한국어]
 * rq_qos_add - 새로운 QoS 플러그인을 request_queue의 rq_qos 체인에 등록한다.
 *
 * @rqos: 등록할 rq_qos 플러그인 객체. 호출자가 할당하여 ops/id/disk를 설정하기 전에 전달.
 * @disk: 이 QoS가 적용될 블록 디바이스의 gendisk.
 * @id:   플러그인 식별자 (RQ_QOS_WBT, RQ_QOS_LATENCY, RQ_QOS_COST 중 하나).
 * @ops:  플러그인의 ops vtable (throttle/done/issue 등 콜백 포인터 집합).
 * @return: 성공 시 0, 동일 id의 플러그인이 이미 등록된 경우 -EBUSY.
 *
 * 큐 동결(blk_mq_freeze_queue) 후 등록하여 in-flight request가 없는 상태에서 체인을
 * 변경한다. 체인의 헤드에 삽입하므로 나중에 등록된 플러그인이 먼저 호출된다.
 * 등록 후 QUEUE_FLAG_QOS_ENABLED를 설정하여 이후 bio 제출 경로에서 QoS가 적용됨.
 * 실행 컨텍스트: 프로세스 컨텍스트. q->rq_qos_mutex 보유 상태에서 호출 필요
 *               (lockdep_assert_held로 검증).
 * 에러 경로: -EBUSY 반환 시에도 큐 동결을 해제하여 정상 I/O가 재개되도록 보장.
 *
 * 호출 체인:
 *   blk_wbt_init / blk_iolatency_init / blk_iocost_init →
 *   mutex_lock(&q->rq_qos_mutex) → [이 함수] → mutex_unlock
 */
int rq_qos_add(struct rq_qos *rqos, struct gendisk *disk, enum rq_qos_id id,
		const struct rq_qos_ops *ops)
{
	struct request_queue *q = disk->queue;  /* [한국어] 대상 블록 디바이스의 request_queue:
	                                         *         rq_qos 체인이 이 큐에 연결됨 */
	unsigned int memflags;  /* [한국어] blk_mq_freeze_queue()가 반환하는 이전 memory 플래그:
	                         *         unfreeze 시 복원에 사용 */

	/* [한국어] 이 함수는 반드시 q->rq_qos_mutex를 보유한 상태에서 호출되어야 함.
	 *         lockdep이 활성화된 디버그 빌드에서 위반 시 경고 출력 */
	lockdep_assert_held(&q->rq_qos_mutex);

	rqos->disk = disk;    /* [한국어] gendisk 연결: 이후 rq_qos_del()에서 disk->queue로 큐를 역산 */
	rqos->id = id;        /* [한국어] 플러그인 ID 설정: rq_qos_id()로 중복 검사 시 사용 */
	rqos->ops = ops;      /* [한국어] ops vtable 연결: throttle/done/issue 등 콜백 포인터 집합 */

	/*
	 * No IO can be in-flight when adding rqos, so freeze queue, which
	 * is fine since we only support rq_qos for blk-mq queue.
	 */
	/* [한국어] 큐 동결: 새 request가 진입하지 못하도록 막고, 기존 in-flight request가
	 *         완료될 때까지 대기. blk-mq 전용(non-mq 큐는 rq_qos 미지원).
	 *         동결 없이 체인을 변경하면 __rq_qos_* 함수가 절반만 등록된 체인을 순회하는 race. */
	memflags = blk_mq_freeze_queue(q);

	/* [한국어] 동일 id의 플러그인이 이미 등록되어 있는지 확인:
	 *         rq_qos_id()는 q->rq_qos 체인을 순회하여 id 일치 항목을 반환. NULL이면 없음. */
	if (rq_qos_id(q, rqos->id))
		goto ebusy;  /* [한국어] 중복 등록 시도: 동일 정책을 두 번 등록하는 버그 또는 race */
	rqos->next = q->rq_qos;  /* [한국어] 기존 체인 헤드를 next로 저장: 새 플러그인을 헤드에 삽입 */
	q->rq_qos = rqos;         /* [한국어] 새 플러그인을 체인 헤드로 설정 */
	/* [한국어] QoS 활성 플래그 설정: blk_mq_submit_bio()에서 rq_qos_throttle() 호출 활성화 */
	blk_queue_flag_set(QUEUE_FLAG_QOS_ENABLED, q);

	blk_mq_unfreeze_queue(q, memflags);  /* [한국어] 큐 동결 해제: 이제 새 request가 QoS를
	                                       *         거치며 진입 가능. memflags로 이전 상태 복원 */
	return 0;  /* [한국어] 등록 성공 */
ebusy:
	blk_mq_unfreeze_queue(q, memflags);  /* [한국어] 등록 실패 시에도 큐 동결 해제 필수:
	                                       *         I/O가 재개될 수 있어야 함 */
	return -EBUSY;  /* [한국어] 동일 id 플러그인 이미 존재: 호출자는 재등록을 시도하지 말 것 */
}

/*
 * [한국어]
 * rq_qos_del - 지정한 QoS 플러그인을 request_queue의 rq_qos 체인에서 제거한다.
 *
 * @rqos: 제거할 rq_qos 플러그인 객체.
 * @return: 없음 (void).
 *
 * 큐 동결 후 연결 리스트를 순회하여 rqos를 체인에서 분리한다. 체인이 비면
 * QUEUE_FLAG_QOS_ENABLED를 클리어하여 bio 제출 경로의 QoS 호출을 비활성화한다.
 * 이 함수는 ops->exit()를 호출하지 않는다 — exit는 호출자가 별도로 수행해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. q->rq_qos_mutex 보유 상태에서 호출 필요.
 *
 * 호출 체인:
 *   blk_wbt_exit / blk_iolatency_exit / blk_iocost_exit →
 *   mutex_lock(&q->rq_qos_mutex) → [이 함수] → (이후) ops->exit(rqos) → mutex_unlock
 */
void rq_qos_del(struct rq_qos *rqos)
{
	struct request_queue *q = rqos->disk->queue;  /* [한국어] 플러그인이 연결된 request_queue 역산:
	                                               *         rqos->disk->queue로 접근 */
	struct rq_qos **cur;   /* [한국어] 체인 순회를 위한 포인터-투-포인터:
	                         *         *cur가 rqos이면 *cur = rqos->next로 체인에서 제거 */
	unsigned int memflags;  /* [한국어] freeze/unfreeze 상태 저장 */

	/* [한국어] q->rq_qos_mutex 보유 상태 검증 */
	lockdep_assert_held(&q->rq_qos_mutex);

	/* [한국어] 큐 동결: 제거 중 새 request가 half-removed 체인을 순회하는 race 방지 */
	memflags = blk_mq_freeze_queue(q);
	/* [한국어] rq_qos 연결 리스트를 순회하여 rqos 위치를 찾아 체인에서 분리 */
	for (cur = &q->rq_qos; *cur; cur = &(*cur)->next) {
		/* [한국어] *cur: 현재 검사 중인 rq_qos 포인터.
		 *         cur는 이전 원소의 ->next 필드 주소이므로 *cur = rqos->next로 제거 가능 */
		if (*cur == rqos) {
			*cur = rqos->next;  /* [한국어] 체인에서 rqos 분리: 이전 원소의 next를
			                     *         rqos->next로 덮어써 체인을 이어붙임 */
			break;  /* [한국어] 발견 즉시 루프 탈출 */
		}
	}
	/* [한국어] 체인이 완전히 비었으면 QoS 플래그 클리어:
	 *         이후 blk_mq_submit_bio()에서 QoS 체인 호출을 건너뜀 */
	if (!q->rq_qos)
		blk_queue_flag_clear(QUEUE_FLAG_QOS_ENABLED, q);
	blk_mq_unfreeze_queue(q, memflags);  /* [한국어] 큐 동결 해제: 이제 새 I/O가 진입 가능 */
}
