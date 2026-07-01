/*
 * block/blk-rq-qos.h - NVMe SSD 관점 요청 품질(QoS) 프레임워크 헤더
 *
 * 이 파일은 블록 계층의 request QoS 추상화를 정의한다. NVMe 입장에서 볼 때,
 * 상위 응용/파일시스템이 만든 bio가 request로 변환된 뒤, 실제 NVMe 하드웨어
 * 큐(nvme_queue / blk_mq_hw_ctx)로 발행되기 전/후에 IO 대역폭, 지연 시간,
 * 쓰기 버스트(write burst)를 제어하는 중간 제어 지점이다.
 *
 * blk-mq(blk-mq.c, blk-mq.h) 위에 위치하며, wbt, latency, cost 기반 정책이
 * NVMe queue depth, doorbell 발행 시점, CID/SQ 엔트리 소비 속도를 간접적으로
 * 조절하는 데 사용된다.
 *
 * 주요 호출 경로 예시:
 *   blk_mq_submit_bio -> blk_mq_get_request -> rq_qos_throttle
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RQ_QOS_H
#define RQ_QOS_H

#include <linux/kernel.h>		/* NVMe 도어 벨 타이밍 계산에 쓰이는 기본 타입(추정) */
#include <linux/blkdev.h>		/* request_queue, gendisk: NVMe namespace 큐의 상위 추상 */
#include <linux/blk_types.h>		/* bio, request 플래그: NVMe 명령으로 변환될 상위 IO 단위 */
#include <linux/atomic.h>		/* NVMe in-flight CID/SQ 엔트리 카운터의 원자적 조작 */
#include <linux/wait.h>		/* SQ 꽉 참/queue depth 초과 시 태스크 대기 큐 */
#include <linux/blk-mq.h>		/* blk_mq_hw_ctx: NVMe SQ/CQ와 1:1 또는 N:M 매핑되는 hctx */

#include "blk-mq-debugfs.h"		/* NVMe QoS 파라미터를 debugfs로 노출하기 위한 로컬 헤더 */

/*
 * 이 헤더는 blk-mq에서 제공하는 request_queue와 blk_mq_hw_ctx를 기반으로 동작한다.
 * NVMe 호스트 드라이버(drivers/nvme/host/pci.c, tcp.c 등)가 생성한 request_queue에
 * wbt, latency, cost 정책이 등록되며, NVMe SQ/CQ 사이에서 명령 발행/완료를
 * 중재하는 역할을 한다(추정).
 */

struct blk_mq_debugfs_attr;		/* NVMe QoS 디버깅 속성 테이블 전방 선언(추정) */

/*
 * 등록 가능한 QoS 정책 식별자다. NVMe namespace의 request_queue에 연결되어
 * queue depth, latency, writeback 제한 등 다양한 정책을 동시에 적용할 수 있다.
 */
enum rq_qos_id {
	RQ_QOS_WBT,		/* writeback throttling: NVMe 쓰기 버스트 억제 */
	RQ_QOS_LATENCY,		/* IO latency 기반 throttling: NVMe 평균 지연 제어 */
	RQ_QOS_COST,		/* 비용 기반 throttling: NVMe 자원 사용량 조절(추정) */
};

/*
 * struct rq_wait - 동시 진행 중인 request 수(in-flight)를 제한하기 위한 대기 구조체
 *
 * NVMe 관점에서 @inflight는 현재 NVMe controller의 SQ에 배치되어 응답(CQ)을
 * 기다리는 명령(CID) 수의 상한을 간접적으로 제어하는 카운터다. @wait는 SQ가
 * 가득 찼을 때(또는 정책상 한도에 도달했을 때) 태스크를 재우는 대기 큐다.
 *
 *   wait:       SQ/CQ 엔트리 여유가 생길 때까지 대기하는 태스크 큐
 *   inflight:   현재 NVMe로 발행된 request/CID 수 (atomic)
 */
struct rq_wait {
	wait_queue_head_t wait;		/* SQ/CQ 여유 발생 시 sleep/wakeup하는 태스크 대기 큐 */
	atomic_t inflight;		/* NVMe SQ에 배치된 CID 수의 상한 제어용 원자 카운터 */
};

/*
 * struct rq_qos - request_queue에 부착되는 QoS 객체
 *
 * NVMe namespace의 request_queue(q->rq_qos)에 연결되어, request의 생애 주기
 * (bio 도착 -> request 할당 -> 발행 -> 완료 -> bio 종료)마다 정책 콜백을
 * 호출한다. @next 필드를 통해 여러 정책을 단일 연결 리스트로 관리한다.
 *
 *   ops:            NVMe request 처리 단계별 정책 콜백 테이블
 *   disk:           NVMe namespace를 표현하는 gendisk
 *   id:             이 QoS 객체가 wbt/latency/cost 중 어느 정책인지
 *   next:           같은 request_queue에 연결된 다음 QoS 객체
 *   debugfs_dir:    NVMe QoS 상태를 디버깅하기 위한 debugfs 디렉터리(추정)
 */
struct rq_qos {
	const struct rq_qos_ops *ops;	/* NVMe request 생애 주기별 정책 훅 테이블 */
	struct gendisk *disk;		/* 이 QoS가 부착된 NVMe namespace의 gendisk */
	enum rq_qos_id id;		/* wbt/latency/cost 중 NVMe 큐에 적용할 정책 식별자 */
	struct rq_qos *next;		/* 동일 request_queue에 연결된 다음 QoS 객체(연결 리스트) */
#ifdef CONFIG_BLK_DEBUG_FS
	struct dentry *debugfs_dir;	/* NVMe QoS 파라미터를 debugfs에 노출하는 디렉터리(추정) */
#endif
};

/*
 * struct rq_qos_ops - QoS 정책이 구현해야 하는 콜백 집합
 *
 * NVMe 입장에서는 bio/request가 NVMe SQ/CQ와 만나는 각 지점에서 정책이
 * 개입할 수 있도록 열어둔 훅(hook)이다. 각 콜백은 다음과 같은 NVMe 행위와
 * 연결된다(일부는 구현에 따라 상이할 수 있음 - 추정).
 *
 *   throttle:      bio가 NVMe request로 할당되기 전에 대역폭/지연 제한
 *                  (blk_mq_get_request -> rq_qos_throttle 경로)
 *   track:         bio가 request에 매핑될 때 호출, NVMe CID 할당 직전 통계
 *   merge:         bio가 기존 request와 병합될 때 NVMe SQ 엔트리 절약 여부
 *   issue:         request가 곧 nvme_queue_rq로 NVMe SQ에 배치되기 직전
 *   requeue:       NVMe timeout/abort 등으로 request가 다시 큐로 돌아갈 때
 *   done:          NVMe CQ에서 명령 완료 인터럽트가 온 뒤 request 종료 시
 *   done_bio:      bio 단위 완료 처리 (NVMe multipath 상위/하위 큐 모두 고려)
 *   cleanup:       bio/request가 에러 등으로 중간 정리될 때
 *   queue_depth_changed: NVMe queue depth(nr_requests 등) 변경 시 재조정
 *   exit:          NVMe namespace 제거 또는 QoS 정책 해제 시
 *   debugfs_attrs: NVMe QoS 파라미터를 debugfs로 노출하기 위한 속성
 */
struct rq_qos_ops {
	void (*throttle)(struct rq_qos *, struct bio *);		/* bio -> request 변환 전 NVMe 대역폭/지연 제한 */
	void (*track)(struct rq_qos *, struct request *, struct bio *);		/* NVMe CID 할당 직전 bio/request 매핑 추적 */
	void (*merge)(struct rq_qos *, struct request *, struct bio *);		/* 인접 bio 병합으로 NVMe SQ 엔트리 절약 시 통계 갱신 */
	void (*issue)(struct rq_qos *, struct request *);		/* nvme_queue_rq 직전: 마지막 queue depth/doorbell 시점 조정 */
	void (*requeue)(struct rq_qos *, struct request *);		/* NVMe timeout/abort로 request가 SQ에서 빠져 다시 큐잉될 때 */
	void (*done)(struct rq_qos *, struct request *);		/* NVMe CQ 완료 후 request 종료, in-flight CID 회수 */
	void (*done_bio)(struct rq_qos *, struct bio *);		/* NVMe multipath 상위/하위 큐의 bio 단위 QoS 완료 */
	void (*cleanup)(struct rq_qos *, struct bio *);		/* bio/request 중간 정리 시 예약된 NVMe QoS 상태 회수 */
	void (*queue_depth_changed)(struct rq_qos *);		/* NVMe SQ depth(nr_requests) 변경 시 max_depth 재계산 */
	void (*exit)(struct rq_qos *);		/* NVMe namespace 제거 또는 정책 해제 시 정리 */
	const struct blk_mq_debugfs_attr *debugfs_attrs;		/* NVMe QoS 파라미터 debugfs 노출 속성 테이블 */
};

/*
 * struct rq_depth - NVMe queue depth를 동적으로 조절하는 상태
 *
 * NVMe controller의 Submission Queue(SQ) 크기를 초과하지 않으면서도,
 * 지연 시간에 따라 동시 발행 명령 수를 조절하기 위해 사용된다. 너무 많은
 * in-flight 명령은 SQ 꽉 참과 doorbell 지연을, 너무 적으면 대역폭 저하를
 * 유발할 수 있다(추정).
 *
 *   max_depth:      NVMe SQ/hardware queue가 허용하는 최대 엔트리 수
 *   scale_step:     지연/성능 피드백에 따른 깊이 스케일링 단계
 *   scaled_max:     max_depth 기준으로 이미 스케일링되었는지 여부
 *   queue_depth:    현재 허용하는 동시 in-flight 명령 수
 *   default_depth:  정책 초기화 시 사용하는 기본 queue depth
 */
struct rq_depth {
	unsigned int max_depth;		/* NVMe SQ/hard queue가 허용하는 최대 엔트리 수 */

	int scale_step;		/* NVMe 평균 지연/완료 시간 피드백에 따른 depth 스케일링 단계 */
	bool scaled_max;		/* max_depth 기준으로 이미 스케일링 완료되었는지 플래그 */

	unsigned int queue_depth;		/* 현재 NVMe SQ에 동시 발행 허용되는 CID/request 수 */
	unsigned int default_depth;		/* QoS 초기화 시 사용하는 NVMe SQ 기본 queue depth */
};

/*
 * rq_qos_id() - request_queue에 부착된 특정 QoS 객체를 검색
 *
 * 목적: NVMe namespace의 request_queue에서 wbt/latency/cost 중 원하는
 *       정책 핸들을 얻는다.
 * 호출 경로: wbt_rq_qos()/iolat_rq_qos() -> rq_qos_id()
 * NVMe 연결: NVMe disk의 request_queue에 등록된 QoS 정책을 찾아
 *            nvme_queue_rq 전후 정책 콜백을 활성화한다(추정).
 */
static inline struct rq_qos *rq_qos_id(struct request_queue *q,
				       enum rq_qos_id id)
{
	struct rq_qos *rqos;
	for (rqos = q->rq_qos; rqos; rqos = rqos->next) { /* q->rq_qos 연결 리스트 순회: NVMe 큐에 등록된 wbt/latency/cost 탐색 */
		if (rqos->id == id)		/* 찾고자 하는 NVMe QoS 정책 식별자와 일치하는지 비교 */
			break;		/* 일치하면 NVMe QoS 객체 획득, 순회 중단 */
	}
	return rqos;		/* NVMe namespace 큐에 부착된 해당 QoS 핸들 반환(없으면 NULL) */
}

/*
 * wbt_rq_qos() - NVMe namespace의 request_queue에서 WBT 정책 핸들을 반환
 *
 * 호출 경로: blk-wbt.c 낶 -> wbt_rq_qos()
 * NVMe 연결: NVMe 쓰기 경로에서 writeback throttling이 필요한지 확인할 때 사용.
 */
static inline struct rq_qos *wbt_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_WBT);		/* NVMe write path의 버스트 억제를 위한 WBT 정책 핸들 검색 */
}

/*
 * iolat_rq_qos() - NVMe namespace의 request_queue에서 IO latency 정책 핸들 반환
 *
 * 호출 경로: blk-iolatency.c 낶 -> iolat_rq_qos()
 * NVMe 연결: NVMe IO 지연 목표를 설정/확인할 때 사용.
 */
static inline struct rq_qos *iolat_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_LATENCY);		/* NVMe IO latency 목표를 적용할 latency QoS 핸들 검색 */
}

/*
 * rq_wait_init() - rq_wait 구조체 초기화
 *
 * NVMe 관점: SQ/CQ 기반 in-flight 명령 카운터를 0으로 두고,
 *            queue depth 한도 도달 시 대기할 대기 큐를 초기화한다.
 */
static inline void rq_wait_init(struct rq_wait *rq_wait)
{
	atomic_set(&rq_wait->inflight, 0);		/* NVMe SQ에 배치된 CID 수를 0으로 초기화(원자 연산) */
	init_waitqueue_head(&rq_wait->wait);		/* SQ 꽉 참/queue depth 초과 시 태스크가 대기할 waitqueue 초기화 */
}

/*
 * rq_qos_add()/rq_qos_del() - QoS 객체를 NVMe namespace의 request_queue에
 *                             등록/해제
 *
 * 호출 경로: wbt/latency/cost 초기화/종료 -> rq_qos_add()/rq_qos_del()
 * NVMe 연결: NVMe gendisk의 request_queue에 QoS 정책을 붙여/떼서
 *            nvme_queue_rq 전후 정책 개입을 활성화/비활성화한다(구현은
 *            blk-rq-qos.c에 있음 - 추정).
 */
int rq_qos_add(struct rq_qos *rqos, struct gendisk *disk, enum rq_qos_id id,
		const struct rq_qos_ops *ops);		/* NVMe namespace gendisk의 request_queue에 QoS 정책 등록 */
void rq_qos_del(struct rq_qos *rqos);		/* NVMe namespace 제거/정책 비활성화 시 QoS 객체 해제 */

/*
 * rq_qos_wait()에서 사용하는 콜백 타입들이다.
 * @acquire_inflight_cb: in-flight 카운터를 안전하게 증가시킬 수 있는지 검사
 * @cleanup_cb:          대기 중 취소/에러 시 정리
 *
 * NVMe 연결: NVMe SQ가 가득 찼을 때 bio/request 할당을 차단(block)하고,
 *            SQ/CQ 여유가 생기면 깨워 CID를 할당하는 데 사용(추정).
 */
typedef bool (acquire_inflight_cb_t)(struct rq_wait *rqw, void *private_data);		/* NVMe SQ/CQ 여유 시 CID 원자 획득 가능(추정) */
typedef void (cleanup_cb_t)(struct rq_wait *rqw, void *private_data);		/* NVMe 대기 중 취소/timeout/abort 발생 시 정리 콜백 */

void rq_qos_wait(struct rq_wait *rqw, void *private_data,
		 acquire_inflight_cb_t *acquire_inflight_cb,
		 cleanup_cb_t *cleanup_cb);		/* NVMe SQ/CQ 여유 생길 때까지 bio/request 할당을 sleep로 차단 */

/*
 * rq_wait_inc_below() - in-flight 카운터가 limit 미만일 때만 원자적 증가
 *
 * NVMe 연결: NVMe SQ에 배치 가능한 CID 수가 제한을 넘지 않도록 세마포어처럼
 *            사용된다(추정). 실패하면 호출자는 대기 후 재시도한다.
 */
bool rq_wait_inc_below(struct rq_wait *rq_wait, unsigned int limit);		/* limit 미만이면 NVMe in-flight CID 원자 증가 */

/*
 * rq_depth_scale_up/down()/calc_max_depth() - NVMe queue depth 동적 조절
 *
 * 목적: 측정된 IO latency/완료 시간을 보고 NVMe SQ에 동시에 둘 수 있는
 *       명령 수를 늘리거나 줄인다.
 * 호출 경로: wbt/latency 정책 낶 -> rq_depth_*()
 * NVMe 연결: queue_depth 값이 작으면 NVMe doorbell 발행 빈도가 줄고,
 *            크면 SQ가 가득 차서 tail doorbell 지연과 head-of-line blocking
 *            가능성이 커진다(추정).
 */
bool rq_depth_scale_up(struct rq_depth *rqd);		/* NVMe 평균 지연 여유 시 SQ in-flight 한도 확대 */
bool rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle);		/* NVMe 지연 증가/버스트 시 SQ in-flight 한도 축소 */
bool rq_depth_calc_max_depth(struct rq_depth *rqd);		/* NVMe SQ 특성과 nr_requests를 반영해 max_depth 재계산 */

/*
 * __rq_qos_*() - 단일 QoS 객체(또는 연결 리스트)에 대한 낮은 수준 콜백
 *
 * 이 함수들은 rq_qos_*() 래퍼 낶에서 호출되며, 등록된 모든 QoS 정책의
 * ops->throttle/track/issue/done/... 콜백을 순차적으로 실행한다.
 * NVMe 연결: NVMe request가 blk-mq를 지나 nvme_queue_rq로 가거나,
 *            nvme_process_cq 이후 blk-mq 완료 경로를 지날 때 정책 훅을
 *            연쇄 호출한다(추정).
 */
void __rq_qos_cleanup(struct rq_qos *rqos, struct bio *bio);		/* NVMe request 할당 실패/취소 시 QoS 상태 회수 */
void __rq_qos_done(struct rq_qos *rqos, struct request *rq);		/* NVMe CQ 완료 후 request 종료, in-flight CID 감소 */
void __rq_qos_issue(struct rq_qos *rqos, struct request *rq);		/* nvme_queue_rq 직전: NVMe SQ 발행 직전 정책 개입 */
void __rq_qos_requeue(struct rq_qos *rqos, struct request *rq);		/* NVMe timeout/abort로 request 재큐잉 시 상태 되돌림 */
void __rq_qos_throttle(struct rq_qos *rqos, struct bio *bio);		/* bio -> request 전: NVMe queue depth/지연에 따른 제한 */
void __rq_qos_track(struct rq_qos *rqos, struct request *rq, struct bio *bio);		/* NVMe CID 할당 직전 bio(rw/크기)를 QoS에 등록 */
void __rq_qos_merge(struct rq_qos *rqos, struct request *rq, struct bio *bio);		/* bio 병합: NVMe SQ 엔트리 절약 및 통계 갱신 */
void __rq_qos_done_bio(struct rq_qos *rqos, struct bio *bio);		/* NVMe multipath 등에서 bio 단위 QoS 완료 통보 */
void __rq_qos_queue_depth_changed(struct rq_qos *rqos);		/* NVMe queue depth 변경 시 QoS 낶 max_depth/queue_depth 재조정 */

/*
 * rq_qos_cleanup() - bio/request가 중간에 정리(cleanup)될 때 QoS 상태 회수
 *
 * 호출 경로: blk-mq 에러/취소 경로 -> rq_qos_cleanup()
 * NVMe 연결: NVMe 명령이 SQ에 배치되지 못하고 bio가 해제될 때,
 *            throttle/merge 단계에서 예약된 정책 상태를 되돌린다.
 */
static inline void rq_qos_cleanup(struct request_queue *q, struct bio *bio)
{
	/* QOS_ENABLED 비트와 rq_qos 리스트 둘 다 확인해야 실제 정책이 존재함(추정) */
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)		/* NVMe 큐 QoS 활성화 및 정책 존재 확인(추정) */
		__rq_qos_cleanup(q->rq_qos, bio);		/* NVMe 할당 취소/에러 시 등록된 QoS 정책들의 cleanup 콜백 연쇄 호출 */
}

/*
 * rq_qos_done() - NVMe CQ 완료 후 request 단위 QoS 종료 처리
 *
 * 호출 경로: blk_mq_end_request() 등 -> rq_qos_done()
 * NVMe 연결: nvme_process_cq -> blk_mq_complete_request -> rq_qos_done 로
 *            NVMe 명령이 CQ에서 완료되면 in-flight 통계를 갱신한다.
 * 특이사항: passthrough request(예: NVMe admin 명령)는 일반 IO QoS 통계에서
 *           제외된다.
 */
static inline void rq_qos_done(struct request_queue *q, struct request *rq)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) &&
	    q->rq_qos && !blk_rq_is_passthrough(rq)) /* NVMe admin/passthrough 제외 */
		__rq_qos_done(q->rq_qos, rq);		/* NVMe CQ 완료 후 in-flight CID 카운터 및 latency 통계 갱신 */
}

/*
 * rq_qos_issue() - request가 NVMe SQ로 발행되기 직전 QoS 콜백
 *
 * 호출 경로: blk_mq_dispatch_rq_list() / blk_mq_try_issue_list_directly()
 *           -> rq_qos_issue() -> __rq_qos_issue()
 * NVMe 연결: nvme_queue_rq 호출 직전에 위치하며, 마지막으로 queue depth나
 *            latency 제한을 확인한 뒤 doorbell을 울린다(추정).
 */
static inline void rq_qos_issue(struct request_queue *q, struct request *rq)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)		/* NVMe 큐 QoS 활성화 및 정책 존재 확인(추정) */
		__rq_qos_issue(q->rq_qos, rq);		/* nvme_queue_rq 직전: 마지막 queue depth/doorbell 시점 제어 콜백 연쇄 호출 */
}

/*
 * rq_qos_requeue() - NVMe 명령이 timeout/abort 등으로 재큐잉될 때 호출
 *
 * 호출 경로: NVMe 에러 복구 / blk_execute_rq_nowait 재시도 경로
 * NVMe 연결: nvme_queue_rq로 복귀하기 전에 in-flight 통계를 되돌리고
 *            latency 정책이 재시도 지연을 판단한다(추정).
 */
static inline void rq_qos_requeue(struct request_queue *q, struct request *rq)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)		/* NVMe 큐 QoS 활성화 및 정책 존재 확인(추정) */
		__rq_qos_requeue(q->rq_qos, rq);		/* NVMe timeout/abort 후 request를 다시 큐로 돌릴 때 QoS 상태 복원 */
}

/*
 * rq_qos_done_bio() - bio 단위 QoS 완료 처리
 *
 * 목적: bio에 BIO_QOS_THROTTLED/BIO_QOS_MERGED 플래그가 붙어 있을 때
 *       해당 bio가 실제로 완료되었음을 QoS 정책에 통보한다.
 * NVMe 연결: NVMe multipath 같은 stacked block 장치에서 상위 큐와 하위 큐의
 *            QoS 상태가 다를 수 있으므로, bio가 속한 bdev의 request_queue를
 *            다시 얻어서 확인해야 한다.
 */
static inline void rq_qos_done_bio(struct bio *bio)
{
	struct request_queue *q;

	/* bio에 연결된 bdev가 없거나, QoS 관련 플래그가 없으면 처리 불필요 */
	if (!bio->bi_bdev || (!bio_flagged(bio, BIO_QOS_THROTTLED) &&
			     !bio_flagged(bio, BIO_QOS_MERGED)))		/* bio가 NVMe QoS throttle/merge 경로를 거쳤는지 플래그 비트 테스트 */
		return;		/* NVMe QoS와 무관한 bio면 조기 리턴으로 하위 큐 탐색 회피 */

	q = bdev_get_queue(bio->bi_bdev);		/* NVMe multipath 시 실제 하위 큐를 얻음 */

	/*
	 * A BIO may carry BIO_QOS_* flags even if the associated request_queue
	 * does not have rq_qos enabled. This can happen with stacked block
	 * devices — for example, NVMe multipath, where it's possible that the
	 * bottom device has QoS enabled but the top device does not. Therefore,
	 * always verify that q->rq_qos is present and QoS is enabled before
	 * calling __rq_qos_done_bio().
	 */
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)		/* NVMe multipath 하위 큐 QoS 및 정책 재확인(추정) */
		__rq_qos_done_bio(q->rq_qos, bio);		/* NVMe 하위 큐의 QoS 정책에 bio 완료를 통보하여 throttle/merge 통계 정리 */
}

/*
 * rq_qos_throttle() - bio가 NVMe request로 변환되기 전에 QoS throttle 적용
 *
 * 호출 경로: blk_mq_submit_bio -> blk_mq_get_request -> rq_qos_throttle()
 * NVMe 연결: 실제로 request/CID를 할당하고 nvme_queue_rq로 복사하기 전에
 *            latency 또는 writeback 한도에 따라 bio를 대기시킬 수 있다.
 */
static inline void rq_qos_throttle(struct request_queue *q, struct bio *bio)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos) {		/* NVMe 큐 QoS 활성화 및 정책 존재 확인(추정) */
		bio_set_flag(bio, BIO_QOS_THROTTLED);		/* 이 bio가 throttle 거쳤음 표시: 이후 done_bio에서 하위 큐에 통보 */
		__rq_qos_throttle(q->rq_qos, bio);		/* NVMe request/CID 할당 전 queue depth/지연/버스트 한도 검사 및 대기 */
	}
}

/*
 * rq_qos_track() - bio와 request를 매핑할 때 QoS 추적 콜백
 *
 * 호출 경로: blk_mq_get_request() -> rq_qos_track()
 * NVMe 연결: NVMe CID가 할당되기 직전, bio 특성(read/write, 크기 등)을
 *            기반으로 정책이 request를 추적한다(추정).
 */
static inline void rq_qos_track(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)		/* NVMe 큐 QoS 활성화 비트 테스트(추정) */
		__rq_qos_track(q->rq_qos, rq, bio);		/* NVMe CID 할당 직전 bio(rw/크기)를 QoS 정책에 등록 */
}

/*
 * rq_qos_merge() - bio 병합 시 QoS 콜백
 *
 * 호출 경로: blk_attempt_bio_merge() -> rq_qos_merge()
 * NVMe 연결: NVMe SQ 엔트리를 아끼기 위해 인접 bio를 하나의 request로
 *            병합할 때, merge 통계/제한을 갱신한다(추정).
 */
static inline void rq_qos_merge(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos) {		/* NVMe 큐 QoS 활성화 및 정책 존재 확인(추정) */
		bio_set_flag(bio, BIO_QOS_MERGED);		/* 이 bio가 merge 과정에 참여함: 완료 시 하위 큐 QoS에 통보 */
		__rq_qos_merge(q->rq_qos, rq, bio);		/* 인접 bio 병합으로 NVMe SQ 엔트리를 절약할 때 QoS merge 통계 갱신 */
	}
}

/*
 * rq_qos_queue_depth_changed() - NVMe queue depth 변경 시 QoS 정책 재조정
 *
 * 호출 경로: queue sysfs nr_requests 변경 / NVMe reset 후 -> rq_qos_queue_depth_changed()
 * NVMe 연결: NVMe controller reset이나 sysfs를 통해 SQ depth가 바뀌면
 *            max_depth, queue_depth 값을 새로 계산해야 한다.
 */
static inline void rq_qos_queue_depth_changed(struct request_queue *q)
{
	if (test_bit(QUEUE_FLAG_QOS_ENABLED, &q->queue_flags) && q->rq_qos)		/* NVMe 큐 QoS 활성화 및 정책 존재 확인(추정) */
		__rq_qos_queue_depth_changed(q->rq_qos);		/* NVMe SQ depth(nr_requests) 변경 시 wbt/latency/cost의 max_depth 재계산 */
}

/*
 * rq_qos_exit() - request_queue 종료 시 모든 QoS 객체를 정리
 *
 * 호출 경로: del_gendisk() / blk_cleanup_queue() -> rq_qos_exit()
 * NVMe 연결: NVMe namespace가 제거되거나 드라이버가 unload될 때
 *            등록된 wbt/latency/cost 정책을 해제한다(추정).
 */
void rq_qos_exit(struct request_queue *);		/* NVMe namespace 제거/드라이버 unload 시 request_queue의 모든 QoS 객체 해제 */

#endif

/* NVMe 관점 핵심 요약 */
/*
 * - blk-rq-qos.h는 NVMe queue로 request가 날아가기 전/후에 개입하는
 *   QoS 프레임워크 헤더다. throttle -> track -> issue -> done 경로가
 *   NVMe SQ/CQ 입출력 흐름과 직접 연결된다.
 * - rq_depth와 rq_wait는 NVMe controller의 queue depth를 초과하지 않도록
 *   in-flight 명령(CID) 수를 제어하는 핵심 자료구조다.
 * - rq_qos_ops.issue()는 nvme_queue_rq() 직전에 호출되며, 이 시점에서
 *   queue depth와 doorbell 발행 시기를 간접적으로 조절한다(추정).
 * - wbt/latency/cost 정책은 NVMe namespace의 request_queue에 등록되며,
 *   NVMe multipath 같은 stacked 환경에서는 상위/하위 큐 각각 다른 QoS
 *   상태를 가질 수 있다.
 * - passthrough request는 blk_rq_is_passthrough()로 필터링되어 NVMe admin
 *   명령 등은 일반 IO QoS 통계/제한 대상에서 제외된다.
 */
