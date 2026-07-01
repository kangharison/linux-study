// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Facebook
 */

/*
 * blk-mq 디버그파일시스템(debugfs) 등록/표시 모듈.
 *
 * NVMe SSD 입장에서 본 위치:
 *   응용 -> VFS -> 파일시스템 -> 블록 계층(blk-mq) -> NVMe 드라이버 -> PCIe/NVMe 컨트롤러
 * 이 파일은 blk-mq 의 request_queue, blk_mq_hw_ctx, blk_mq_ctx, rq_qos 등의
 * 런타임 상태를 /sys/kernel/debug/block/ 아래에 노출한다.
 * NVMe 드라이버는 blk-mq 위에서 동작하며, 각 nvme_queue 는 대개 하나의 blk_mq_hw_ctx 와
 * 연결되고, 이를 통해 SQ(Submission Queue)/CQ(Completion Queue) 및 doorbell 상태를
 * 추론할 수 있다. 디버깅 시에는 이 파일이 제공하는 tag/CID, hctx 상태, dispatch 리스트 등을
 * 통해 NVMe I/O 경로를 분석하게 된다.
 *
 * 밀접한 연관 파일:
 *   block/blk-mq.c       - blk-mq 핵심 I/O 스케줄링 및 hctx 관리
 *   block/blk-mq-tag.c   - tag/CID 할당/해제
 *   drivers/nvme/host/pci.c - nvme_queue 생성, doorbell 갱신, CQ 처리
 */
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/build_bug.h>
#include <linux/debugfs.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-sched.h"
#include "blk-rq-qos.h"

/*
 * poll_stat 은 현재 빈 구현이며, NVMe polled I/O 성능 통계를 보여줄 수 있는 자리(추정).
 */
static int queue_poll_stat_show(void *data, struct seq_file *m) // NVMe polled I/O (nvme_poll) 성능 통계를 출력할 자리; 현재는 비어 있음
{
	return 0; // 현재 NVMe poll 통계는 비어 있음
}

/*
 * queue_requeue_list_start - request_queue 의 requeue_list 를 seq_file 로 노출하기 위해
 *     q->requeue_lock 을 획득하고 리스트 순회를 시작한다.
 *
 * 호출 경로(추정):
 *   사용자 cat /sys/kernel/debug/block/<disk>/requeue_list
 *     -> seq_read() -> queue_requeue_list_start()
 *
 * NVMe 연결:
 *   NVMe 드라이버에서 nvme_queue_timeout() 등으로 인해 request 가 재삽입되면
 *   requeue_list 에 추가된다. 이 리스트는 SQ 에 아직 doorbell 이 울리지 않은
 *   대기 중인 request 를 의미할 수 있다.
 */
static void *queue_requeue_list_start(struct seq_file *m, loff_t *pos)
	__acquires(&q->requeue_lock)
{
	struct request_queue *q = m->private; // 이 request_queue 는 NVMe 장치의 blk-mq 큐

	spin_lock_irq(&q->requeue_lock); // timeout/abort 로 인한 requeue 와 dispatch 경쟁 방지; NVMe SQ doorbell 직전 상태 보호
	return seq_list_start(&q->requeue_list, *pos); // 재삽입된 request 순회: NVMe CID 재할당 전 단계
}

static void *queue_requeue_list_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct request_queue *q = m->private; // requeue_list 가 속한 NVMe request_queue

	return seq_list_next(v, &q->requeue_list, pos); // 다음 재시도할 NVMe 명령
}

static void queue_requeue_list_stop(struct seq_file *m, void *v)
	__releases(&q->requeue_lock)
{
	struct request_queue *q = m->private; // requeue_list 가 속한 request_queue

	spin_unlock_irq(&q->requeue_lock); // lock 해제 후 다시 dispatch(run) 되면 NVMe doorbell 재개
}

/*
 * queue_requeue_list_seq_ops - requeue_list 를 seq_file 로 보여주는 연산자 집합.
 * .show 는 blk_mq_debugfs_rq_show 로 연결되어 개별 request 의 op, tag/CID,
 * rq_flags 등을 출력한다.
 */
static const struct seq_operations queue_requeue_list_seq_ops = {
	.start	= queue_requeue_list_start, // requeue_list 순회 시작
	.next	= queue_requeue_list_next, // 다음 재삽입 request
	.stop	= queue_requeue_list_stop, // lock 해제
	.show	= blk_mq_debugfs_rq_show, // request 상세 출력(CID/flag)
};

/*
 * blk_flags_show - unsigned long flags 값을 인간이 읽을 수 있는 문자열로 변환하여
 *     seq_file 에 기록한다.
 *
 * NVMe 연결:
 *   request->cmd_flags 의 __REQ_POLLED 비트는 NVMe polled completion 경로를,
 *   __REQ_FUA 는 NVMe Flush/FUA 명령 의미를 갖는다.
 */
static int blk_flags_show(struct seq_file *m, const unsigned long flags,
			  const char *const *flag_name, int flag_name_count)
{
	bool sep = false; // flag 이름 사이 구분자; NVMe cmd_flags/rq_flags 해석용
	int i; // 비트 인덱스 = cmd_flags/rq_flags 의 각 NVMe 관련 플래그

	for (i = 0; i < sizeof(flags) * BITS_PER_BYTE; i++) { // flags 의 모든 비트를 순회하며 NVMe 관련 플래그(POLLED/FUA 등) 탐색
		if (!(flags & BIT(i))) // 설정되지 않은 비트는 skip; NVMe SQ/CQ 동작에 영향 없음
			continue;
		if (sep) // 이전 플래그와 구분
			seq_puts(m, "|");
		sep = true; // 이후 플래그 구분자 추가
		if (i < flag_name_count && flag_name[i]) // 알려진 NVMe 관련 이름(POLLED, FUA 등) 출력
			seq_puts(m, flag_name[i]);
		else
			seq_printf(m, "%d", i); // 미정의 비트; 드라이버 전용 NVMe flag 일 수 있음(추정)
	}
	return 0;
}

/*
 * queue_pm_only_show - request_queue 의 전원 관리 카운터 pm_only 를 출력한다.
 * NVMe 연결:
 *   NVMe 컨트롤러가 D3cold 등 저전력 상태로 들어가면 q->pm_only 가 증가하여
 *   새로운 I/O 를 일시적으로 차단할 수 있다.
 */
static int queue_pm_only_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data; // NVMe 장치의 request_queue 참조

	seq_printf(m, "%d\n", atomic_read(&q->pm_only)); // q->pm_only 원자값; NVMe D3cold 등 저전력 시 I/O 차단 카운터
	return 0;
}

#define QUEUE_FLAG_NAME(name) [QUEUE_FLAG_##name] = #name // queue_flags 비트 -> 이름 매핑; NVMe controller state 비트
/*
 * blk_queue_flag_name - request_queue->queue_flags 의 각 비트 이름.
 * NVMe 연결:
 *   QUEUE_FLAG_DYING   : NVMe 컨트롤러 제거/장애 시 큐가 죽어가는 상태.
 *   QUEUE_FLAG_QUIESCED: NVMe reset/SUSPEND 중 I/O 를 멈춘 상태.
 *   QUEUE_FLAG_REGISTERED: /sys/block/<device>/queue 생성 완료.
 */
static const char *const blk_queue_flag_name[] = {
	QUEUE_FLAG_NAME(DYING), // NVMe 장치 제거/죽음 시 큐 상태
	QUEUE_FLAG_NAME(NOMERGES), // NVMe SQ 엔트리 경계 등으로 merge 금지(추정)
	QUEUE_FLAG_NAME(SAME_COMP), // 동일 completion CPU 힌트; NVMe CQ affinity(추정)
	QUEUE_FLAG_NAME(FAIL_IO), // NVMe controller fatal error; 모든 I/O 실패
	QUEUE_FLAG_NAME(NOXMERGES), // cross-request merge 금지(추정)
	QUEUE_FLAG_NAME(SAME_FORCE), // force same queue; NVMe SQ/CQ affinity 강제(추정)
	QUEUE_FLAG_NAME(INIT_DONE), // queue 초기화 완료; NVMe admin queue 생성 후
	QUEUE_FLAG_NAME(STATS), // I/O 통계 수집; NVMe 성능/지연 분석용
	QUEUE_FLAG_NAME(REGISTERED), // /sys/block 등록 완료
	QUEUE_FLAG_NAME(QUIESCED), // NVMe reset/suspend 동안 I/O 정지 상태
	QUEUE_FLAG_NAME(RQ_ALLOC_TIME), // request 할당 시간 측정; timeout 추적용(추정)
	QUEUE_FLAG_NAME(HCTX_ACTIVE), // 이 hctx/SQ 가 현재 활성화됨
	QUEUE_FLAG_NAME(SQ_SCHED), // single queue scheduler 사용
	QUEUE_FLAG_NAME(DISABLE_WBT_DEF), // writeback throttling 기본 비활성; NVMe 저지연 경로(추정)
	QUEUE_FLAG_NAME(NO_ELV_SWITCH), // 스케줄러 전환 금지; NVMe 장치 안정성(추정)
	QUEUE_FLAG_NAME(QOS_ENABLED), // rq_qos(WBT/latency) 활성; NVMe queue depth/CID 생성 속도 제어
	QUEUE_FLAG_NAME(BIO_ISSUE_TIME), // bio 발행 시간 기록; NVMe latency 측정
	QUEUE_FLAG_NAME(ZONED_QD1_WRITES), // zoned 장치 쓰기 queue depth 1; NVMe ZNS 대응(추정)
};
#undef QUEUE_FLAG_NAME

static int queue_state_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;

	BUILD_BUG_ON(ARRAY_SIZE(blk_queue_flag_name) != QUEUE_FLAG_MAX); // queue_flags 비트 수와 배열 크기 일치 검증
	blk_flags_show(m, q->queue_flags, blk_queue_flag_name, // QUEUE_FLAG_DYING/QUIESCED 등 NVMe controller 상태 해석
		       ARRAY_SIZE(blk_queue_flag_name));
	seq_puts(m, "\n"); // 줄바꿈
	return 0;
}

/*
 * queue_state_write - 디버깅용 state 파일에 "run"/"start"/"kick" 을 써서
 *     큐의 동작을 강제로 제어한다.
 *
 * 호출 경로(추정):
 *   echo run > /sys/kernel/debug/block/<disk>/state
 *     -> blk_mq_debugfs_write() -> queue_state_write()
 *     -> blk_mq_run_hw_queues() -> blk_mq_run_hw_queue()
 *     -> nvme_queue_rq() -> nvme_submit_cmd(doorbell)
 */
static ssize_t queue_state_write(void *data, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct request_queue *q = data; // state 파일 쓰기 대상 request_queue
	char opbuf[16] = { }, *op; // run/start/kick 명령 버퍼

	/*
	 * The "state" attribute is removed when the queue is removed.  Don't
	 * allow setting the state on a dying queue to avoid a use-after-free.
	 */
	if (blk_queue_dying(q)) // dying queue 이면 NVMe reset/remove 중이므로 state 변경 금지
		return -ENOENT;

	if (count >= sizeof(opbuf)) { // 입력이 버퍼보다 크면 잘못된 디버깅 명령
		pr_err("%s: operation too long\n", __func__); // pr_err; 잘못된 명령 길이
		goto inval; // inval 레이블로 점프
	}

	if (copy_from_user(opbuf, buf, count)) // 사용자 공간에서 state 문자열 복사
		return -EFAULT;
	op = strstrip(opbuf); // 앞뒤 공백 제거
	if (strcmp(op, "run") == 0) { // "run" -> 모든 hctx/SQ doorbell 재활성화(추정)
/*
 * NVMe SQ/CQ 의 doorbell 을 다시 울려 대기 중인 request 를 전송(추정).
 */
		blk_mq_run_hw_queues(q, true); // blk_mq_run_hw_queues() -> nvme_queue_rq() -> nvme_submit_cmd(doorbell)
	} else if (strcmp(op, "start") == 0) { // 정지된 NVMe I/O 엔진 재개
/*
 * 정지되었던 NVMe I/O 엔진을 재개.
 */
		blk_mq_start_stopped_hw_queues(q, true); // blk_mq_start_stopped_hw_queues() -> NVMe SQ/CQ 재개
	} else if (strcmp(op, "kick") == 0) { // timeout/reset 으로 되돌아온 request 를 다시 dispatch
/*
 * timeout/reset 등으로 되돌아온 request 를 다시 dispatch.
 */
		blk_mq_kick_requeue_list(q); // blk_mq_kick_requeue_list() -> requeue_list -> NVMe SQ 재시도
	} else { // 지원하지 않는 디버깅 명령
		pr_err("%s: unsupported operation '%s'\n", __func__, op); // pr_err; 지원하지 않는 operation
inval: // inval 레이블: 사용법 안내
		pr_err("%s: use 'run', 'start' or 'kick'\n", __func__); // 잘못된 명령 거부
		return -EINVAL; // state 쓰기 성공
	}
	return count;
}

/*
 * blk_mq_debugfs_queue_attrs - 디스크 단위 /sys/kernel/debug/block/<disk>/ 하위 파일.
 * 각 항목은 request_queue 수준의 상태를 노출한다.
 */
static const struct blk_mq_debugfs_attr blk_mq_debugfs_queue_attrs[] = {
	{ "poll_stat", 0400, queue_poll_stat_show }, // NVMe polled I/O 통계(현재 비어 있음)
	{ "requeue_list", 0400, .seq_ops = &queue_requeue_list_seq_ops }, // timeout/abort 로 NVMe SQ doorbell 직전에 대기한 request 목록
	{ "pm_only", 0600, queue_pm_only_show, NULL }, // q->pm_only; NVMe 저전력 상태 카운터
	{ "state", 0600, queue_state_show, queue_state_write }, // queue_state_show/write; NVMe SQ/CQ run/start/kick 제어
	{ "zone_wplugs", 0400, queue_zone_wplugs_show, NULL }, // zone write plug 상태
	{ },
};

#define HCTX_STATE_NAME(name) [BLK_MQ_S_##name] = #name // hctx->state 비트 -> 이름 매핑; NVMe SQ/CQ 활성/정지 상태
/*
 * hctx_state_name - blk_mq_hw_ctx->state 플래그 이름.
 * NVMe 연결:
 *   BLK_MQ_S_STOPPED    : NVMe SQ/CQ 가 일시 정지되어 새 CID 를 doorbell 로 전송하지 않음.
 *   BLK_MQ_S_TAG_ACTIVE : 현재 hctx 에서 tag/CID 가 활발히 사용 중임.
 *   BLK_MQ_S_SCHED_RESTART: NVMe I/O 가 막혀 스케줄러 재시작이 필요함.
 *   BLK_MQ_S_INACTIVE   : 이 hctx/SQ 가 현재 비활성 상태.
 */
static const char *const hctx_state_name[] = {
	HCTX_STATE_NAME(STOPPED), // NVMe SQ/CQ 일시 정지; 새 CID doorbell 전송 안 함
	HCTX_STATE_NAME(TAG_ACTIVE), // 이 hctx/SQ 에서 tag/CID 활발히 사용 중
	HCTX_STATE_NAME(SCHED_RESTART), // NVMe I/O 가 막혀 스케줄러 재시작 필요
	HCTX_STATE_NAME(INACTIVE), // 이 hctx/SQ 비활성; NVMe queue disable 상태(추정)
};
#undef HCTX_STATE_NAME

static int hctx_state_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;

	BUILD_BUG_ON(ARRAY_SIZE(hctx_state_name) != BLK_MQ_S_MAX); // hctx_state 비트 수와 배열 크기 일치 검증
	blk_flags_show(m, hctx->state, hctx_state_name, // hctx->state 해석: STOPPED/TAG_ACTIVE/SCHED_RESTART/INACTIVE
		       ARRAY_SIZE(hctx_state_name));
	seq_puts(m, "\n");
	return 0;
}

#define HCTX_FLAG_NAME(name) [ilog2(BLK_MQ_F_##name)] = #name // hctx->flags 비트 -> 이름 매핑; NVMe SQ/CQ 특성
/*
 * hctx_flag_name - blk_mq_hw_ctx->flags 의 비트 이름.
 * NVMe 연결:
 *   BLK_MQ_F_TAG_QUEUE_SHARED: 여러 NVMe SQ 가 tagset/CID 공간을 공유.
 *   BLK_MQ_F_BLOCKING       : NVMe 드라이버의 .queue_rq() 가 sleep 할 수 있음.
 *   BLK_MQ_F_NO_SCHED_BY_DEFAULT: NVMe 같은 polling/저지연 장치에서 스케줄러 사용 안 함.
 */
static const char *const hctx_flag_name[] = {
	HCTX_FLAG_NAME(TAG_QUEUE_SHARED), // 여러 NVMe SQ 가 tagset/CID 공간을 공유
	HCTX_FLAG_NAME(STACKING), // stacking 장치용(추정)
	HCTX_FLAG_NAME(TAG_HCTX_SHARED), // tag 공간을 hctx 간 공유(추정)
	HCTX_FLAG_NAME(BLOCKING), // NVMe 드라이버 .queue_rq() 가 sleep 가능
	HCTX_FLAG_NAME(TAG_RR), // tag round-robin; NVMe SQ 로드밸런싱(추정)
	HCTX_FLAG_NAME(NO_SCHED_BY_DEFAULT), // NVMe 같은 polling/저지연 장치에서 스케줄러 사용 안 함
};
#undef HCTX_FLAG_NAME

static int hctx_flags_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data;

	BUILD_BUG_ON(ARRAY_SIZE(hctx_flag_name) != ilog2(BLK_MQ_F_MAX)); // hctx flags 비트 수와 배열 크기 일치 검증

	blk_flags_show(m, hctx->flags, hctx_flag_name, // hctx->flags 해석: TAG_QUEUE_SHARED/BLOCKING/NO_SCHED_BY_DEFAULT 등
			ARRAY_SIZE(hctx_flag_name));
	seq_puts(m, "\n");
	return 0;
}

#define CMD_FLAG_NAME(name) [__REQ_##name] = #name // request->cmd_flags 비트 -> 이름; NVMe 명령 특성
/*
 * cmd_flag_name - struct request->cmd_flags 비트 이름.
 * NVMe 연결:
 *   __REQ_POLLED  : NVMe polled completion 경로 사용.
 *   __REQ_FUA     : NVMe FUA(Force Unit Access) 명령 의미.
 *   __REQ_PREFLUSH: NVMe Flush 명령으로 매핑될 수 있음.
 *   __REQ_NOMERGE : NVMe SQ 엔트리 경계 등으로 병합 금지.
 */
static const char *const cmd_flag_name[] = {
	CMD_FLAG_NAME(FAILFAST_DEV), // 디바이스 fast fail; NVMe 장치 오류 시 즉시 완료
	CMD_FLAG_NAME(FAILFAST_TRANSPORT), // 트랜스포트 fast fail; PCIe/NVMe 링크 오류(추정)
	CMD_FLAG_NAME(FAILFAST_DRIVER), // 드라이버 fast fail; NVMe 드라이버 내부 오류(추정)
	CMD_FLAG_NAME(SYNC), // 동기식 I/O; NVMe flush/sync 의미
	CMD_FLAG_NAME(META), // 메타데이터 버퍼; NVMe extended LBA/메타데이터(추정)
	CMD_FLAG_NAME(PRIO), // I/O 우선순위; NVMe arbitration(추정)
	CMD_FLAG_NAME(NOMERGE), // merge 금지; NVMe SQ 엔트리 경계 고려
	CMD_FLAG_NAME(IDLE), // idle I/O; NVMe power management 힌트(추정)
	CMD_FLAG_NAME(INTEGRITY), // T10-DIF/DIX; NVMe end-to-end data protection(추정)
	CMD_FLAG_NAME(FUA), // NVMe FUA(Force Unit Access) 명령
	CMD_FLAG_NAME(PREFLUSH), // NVMe Flush/PREFLUSH 명령으로 매핑 가능
	CMD_FLAG_NAME(RAHEAD), // readahead; NVMe 순차 읽기 힌트
	CMD_FLAG_NAME(BACKGROUND), // background I/O; NVMe low-priority(추정)
	CMD_FLAG_NAME(NOWAIT), // alloc no wait; NVMe queue full 시 -EAGAIN(추정)
	CMD_FLAG_NAME(POLLED), // NVMe polled completion 경로 사용
	CMD_FLAG_NAME(ALLOC_CACHE), // 할당 캐시; NVMe hot-tag 재사용(추정)
	CMD_FLAG_NAME(SWAP), // swap I/O; NVMe swapin/swapout
	CMD_FLAG_NAME(DRV), // 드라이버 전용 flag; nvme_req_private 등(추정)
	CMD_FLAG_NAME(FS_PRIVATE), // 파일시스템 전용 flag(추정)
	CMD_FLAG_NAME(ATOMIC), // atomic I/O; NVMe atomic write(추정)
	CMD_FLAG_NAME(NOUNMAP), // unmap 금지; NVMe Deallocate/Trim 방지(추정)
};
#undef CMD_FLAG_NAME

#define RQF_NAME(name) [__RQF_##name] = #name // request->rq_flags 비트 -> 이름; NVMe 생명주기/상태
/*
 * rqf_name - struct request->rq_flags 비트 이름.
 * NVMe 연결:
 *   RQF_STARTED      : request 가 NVMe SQ 에 삽입되어 doorbell 가능 상태.
 *   RQF_FAILED       : NVMe 컨트롤러/트랜스포트 오류로 완료.
 *   RQF_TIMED_OUT    : NVMe I/O timeout 발생.
 *   RQF_SPECIAL_PAYLOAD: PRP/SGL 을 위한 특수 바이오 payload.
 */
static const char *const rqf_name[] = {
	RQF_NAME(STARTED), // request 가 NVMe SQ 에 삽입되어 doorbell 가능 상태
	RQF_NAME(FLUSH_SEQ), // flush sequence; NVMe flush/cmd sequence
	RQF_NAME(MIXED_MERGE), // mixed merge; NVMe PRP/SGL 경계를 넘는 merge(추정)
	RQF_NAME(DONTPREP), // prep 생략; NVMe cmd 재구성 skip(추정)
	RQF_NAME(SCHED_TAGS), // 스케줄러 tag 사용
	RQF_NAME(USE_SCHED), // 스케줄러가 이 request 관여
	RQF_NAME(FAILED), // NVMe 컨트롤러/트랜스포트 오류로 완료
	RQF_NAME(QUIET), // 오류 메시지 억제
	RQF_NAME(IO_STAT), // I/O 통계 수집 대상
	RQF_NAME(PM), // 전원 관리 관련 request
	RQF_NAME(HASHED), // hash table 에 등록; NVMe timeout hash(추정)
	RQF_NAME(STATS), // 디버깅/통계 수집
	RQF_NAME(SPECIAL_PAYLOAD), // PRP/SGL/special buffer 용 특수 payload
	RQF_NAME(ZONE_WRITE_PLUGGING), // zone write plugging; NVMe ZNS 순차 쓰기(추정)
	RQF_NAME(TIMED_OUT), // NVMe I/O timeout 발생
	RQF_NAME(RESV), // 예약/내부 flag
};
#undef RQF_NAME

/*
 * blk_mq_rq_state_name_array - request 의 생명주기 상태 이름.
 * NVMe 연결:
 *   MQ_RQ_IDLE     : request 가 아직 NVMe SQ 에 할당되지 않음.
 *   MQ_RQ_IN_FLIGHT: CID 가 할당되어 NVMe SQ/CQ 에서 진행 중.
 *   MQ_RQ_COMPLETE : NVMe CQ 엔트리 수신 및 완료 처리됨.
 */
static const char *const blk_mq_rq_state_name_array[] = {
	[MQ_RQ_IDLE]		= "idle", // request 가 아직 NVMe SQ 에 할당되지 않음
	[MQ_RQ_IN_FLIGHT]	= "in_flight", // CID 할당되어 NVMe SQ/CQ 에서 진행 중
	[MQ_RQ_COMPLETE]	= "complete", // NVMe CQ 엔트리 수신 및 완료 처리됨
};

static const char *blk_mq_rq_state_name(enum mq_rq_state rq_state)
{
	if (WARN_ON_ONCE((unsigned int)rq_state >= // rq_state 범위 검증; 잘못된 NVMe 상태 방지
			 ARRAY_SIZE(blk_mq_rq_state_name_array)))
		return "(?)";
	return blk_mq_rq_state_name_array[rq_state]; // NVMe request 생명주기 상태 문자열 반환
}

/*
 * __blk_mq_debugfs_rq_show - 단일 request 의 핵심 필드를 seq_file 에 출력.
 *
 * 출력 항목:
 *   .op          : NVMe opcode 대응(req_op), e.g. REQ_OP_READ -> NVMe Read.
 *   .cmd_flags   : POLLED, FUA, PREFLUSH 등 NVMe 명령 특성.
 *   .rq_flags    : STARTED, FAILED, TIMED_OUT 등 생명주기/상태 표시.
 *   .state       : NVMe SQ/CQ 생명주기(idle/in_flight/complete).
 *   .tag         : NVMe CID(command identifier)로 매핑되는 tag.
 *   .internal_tag: 스케줄러 나이부 tag.
 *   .show_rq     : NVMe 드라이버가 추가로 PRP/SGL, sq_head, cq_head 등을 출력.
 *
 * 호출 경로(추정):
 *   blk_mq_debugfs_rq_show()
 *     -> list_entry_rq(v) -> __blk_mq_debugfs_rq_show()
 *     -> mq_ops->show_rq()  (nvme_show_rq() 등)
 */
int __blk_mq_debugfs_rq_show(struct seq_file *m, struct request *rq)
{
	const struct blk_mq_ops *const mq_ops = rq->q->mq_ops; // mq_ops; NVMe 드라이버 ops (nvme_mq_ops) 참조
	const enum req_op op = req_op(rq); // req_op; NVMe opcode(Read/Write/Flush/Discard 등) 대응
	const char *op_str = blk_op_str(op); // op 문자열; NVMe admin/io 명령 해석용

	BUILD_BUG_ON(ARRAY_SIZE(cmd_flag_name) != __REQ_NR_BITS); // cmd_flags 비트 수와 배열 크기 일치 검증
	BUILD_BUG_ON(ARRAY_SIZE(rqf_name) != __RQF_BITS); // rq_flags 비트 수와 배열 크기 일치 검증

	seq_printf(m, "%p {.op=", rq); // 출력 시작: request 포인터
	if (strcmp(op_str, "UNKNOWN") == 0) // UNKNOWN 이면 숫자 opcode 출력; NVMe vendor specific 등
		seq_printf(m, "%u", op);
	else
		seq_printf(m, "%s", op_str); // 알려진 op 이름 출력(REQ_OP_READ -> NVMe Read 등)
	seq_puts(m, ", .cmd_flags=");
	blk_flags_show(m, (__force unsigned int)(rq->cmd_flags & ~REQ_OP_MASK), // cmd_flags 에서 req_op 비트 마스크; POLLED/FUA/FLUSH 등만 표시
		       cmd_flag_name, ARRAY_SIZE(cmd_flag_name));
	seq_puts(m, ", .rq_flags=");
	blk_flags_show(m, (__force unsigned int)rq->rq_flags, rqf_name, // rq_flags 표시: RQF_STARTED/FAILED/TIMED_OUT 등 NVMe 생명주기
		       ARRAY_SIZE(rqf_name));
	seq_printf(m, ", .state=%s", blk_mq_rq_state_name(blk_mq_rq_state(rq))); // MQ_RQ_IDLE/IN_FLIGHT/COMPLETE; NVMe SQ/CQ 진행 상태
	seq_printf(m, ", .tag=%d, .internal_tag=%d", rq->tag, // rq->tag = NVMe CID, internal_tag = 스케줄러 내부 tag
		   rq->internal_tag);
/*
 * NVMe 드라이버가 PRP/SGL, sq_head/cq_head 등 추가 정보를 출력(추정).
 */
	if (mq_ops->show_rq) // NVMe 드라이버가 PRP/SGL, sq_head/cq_head 등 추가 정보 출력(추정)
		mq_ops->show_rq(m, rq); // NVMe 드라이버의 show_rq() 호출; PRP/SGL/sq_head/cq_head 출력
	seq_puts(m, "}\n"); // request 레코드 종료
	return 0;
}
EXPORT_SYMBOL_GPL(__blk_mq_debugfs_rq_show);

int blk_mq_debugfs_rq_show(struct seq_file *m, void *v)
{
	return __blk_mq_debugfs_rq_show(m, list_entry_rq(v)); // list node -> struct request; NVMe SQ/CQ 분석 대상
}
EXPORT_SYMBOL_GPL(blk_mq_debugfs_rq_show);

/*
 * hctx_dispatch_start - hctx->dispatch 리스트를 seq_file 로 보여주기 위해
 *     hctx->lock 을 획득하고 순회를 시작한다.
 *
 * NVMe 연결:
 *   hctx->dispatch 에는 NVMe SQ 로 곧바로 삽입될 예정인 request 가 있다.
 *   이 리스트는 소프트웨어 dispatch 큐이며, NVMe doorbell 직전 단계로 볼 수 있다.
 */
static void *hctx_dispatch_start(struct seq_file *m, loff_t *pos)
	__acquires(&hctx->lock)
{
	struct blk_mq_hw_ctx *hctx = m->private; // 이 hctx 는 NVMe SQ/CQ 와 연결된 하드웨어 컨텍스트

	spin_lock(&hctx->lock); // hctx->dispatch 보호; doorbell 직전 소프트웨어 큐
	return seq_list_start(&hctx->dispatch, *pos); // hctx->dispatch 에 대기 중인 NVMe 명령 순회 시작
}

static void *hctx_dispatch_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct blk_mq_hw_ctx *hctx = m->private; // 이 hctx 의 dispatch 리스트

	return seq_list_next(v, &hctx->dispatch, pos); // doorbell 직전 다음 NVMe 명령
}

static void hctx_dispatch_stop(struct seq_file *m, void *v)
	__releases(&hctx->lock)
{
	struct blk_mq_hw_ctx *hctx = m->private; // 이 hctx 의 dispatch 리스트

	spin_unlock(&hctx->lock); // dispatch lock 해제; 이후 NVMe doorbell 가능
}

static const struct seq_operations hctx_dispatch_seq_ops = {
	.start	= hctx_dispatch_start, // dispatch 리스트 순회 시작
	.next	= hctx_dispatch_next, // 다음 dispatch 후보 request
	.stop	= hctx_dispatch_stop, // spinlock 해제
	.show	= blk_mq_debugfs_rq_show, // 개별 request 출력(CID/flag)
};

/*
 * show_busy_params - busy request 를 출력할 때 사용하는 콜백 매개변수.
 * .m    : 출력 대상 seq_file.
 * .hctx : NVMe SQ/CQ 와 연결된 하드웨어 컨텍스트.
 */
struct show_busy_params {
	struct seq_file		*m; // 출력 대상 seq_file
	struct blk_mq_hw_ctx	*hctx; // NVMe SQ/CQ 와 연결된 하드웨어 컨텍스트
};

/*
 * Note: the state of a request may change while this function is in progress,
 * e.g. due to a concurrent blk_mq_finish_request() call. Returns true to
 * keep iterating requests.
 */
/*
 * hctx_show_busy_rq - tagset 에서 hctx 에 속한 busy request 만 출력.
 *
 * NVMe 연결:
 *   rq->mq_hctx 가 지정 hctx 와 일치하면 해당 request 는 이 NVMe SQ/CQ 에
 *   현재 할당된 CID 상태이며, 아직 NVMe CQ completion 을 받지 않은 것으로 본다.
 */
static bool hctx_show_busy_rq(struct request *rq, void *data)
{
	const struct show_busy_params *params = data; // 콜백 매개변수

	if (rq->mq_hctx == params->hctx) // 같은 hctx/SQ 에 배정된 request 만 busy 로 간주
		__blk_mq_debugfs_rq_show(params->m, rq); // 해당 CID 의 request 상세 출력

	return true; // tagset 전체를 계속 순회; 모든 NVMe CID 스캔
}

/*
 * hctx_busy_show - hctx 에 속한 모든 in-flight request 를 출력.
 *
 * 호출 경로(추정):
 *   cat /sys/kernel/debug/block/<disk>/mq/hctx<N>/busy
 *     -> seq_read() -> hctx_busy_show()
 *     -> blk_mq_tagset_busy_iter() -> hctx_show_busy_rq()
 *
 * NVMe 연결:
 *   출력 결과는 NVMe SQ 에 현재 삽입되어 진행 중인 command 와 CID 목록으로
 *   해석할 수 있다.
 */
static int hctx_busy_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx
	struct show_busy_params params = { .m = m, .hctx = hctx }; // 출력 매개변수 초기화
	int res; // mutex 획득 결과

	res = mutex_lock_interruptible(&hctx->queue->elevator_lock); // elevator lock; CID in-flight 뷰 일관성 유지
	if (res) // mutex 획득 실패 시 오류 반환
		return res;
	blk_mq_tagset_busy_iter(hctx->queue->tag_set, hctx_show_busy_rq, // tagset 의 모든 busy request 순회 -> NVMe CID 스캔
				&params); // 이 hctx 에 속한 NVMe 명령만 필터 출력
	mutex_unlock(&hctx->queue->elevator_lock); // elevator lock 해제

	return 0;
}

/*
 * hctx_types - blk_mq_hw_ctx 가 처리하는 I/O 유형.
 * NVMe 연결:
 *   HCTX_TYPE_DEFAULT: 일반 NVMe SQ/CQ 경로.
 *   HCTX_TYPE_READ   : 읽기 전용 SQ/CQ(저지연 read 최적화).
 *   HCTX_TYPE_POLL   : polled completion 전용 SQ/CQ.
 */
static const char *const hctx_types[] = {
	[HCTX_TYPE_DEFAULT]	= "default", // 일반 NVMe SQ/CQ 경로
	[HCTX_TYPE_READ]	= "read", // 읽기 전용 SQ/CQ; 저지연 read 최적화(추정)
	[HCTX_TYPE_POLL]	= "poll", // polled completion 전용 SQ/CQ
};

static int hctx_type_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx

	BUILD_BUG_ON(ARRAY_SIZE(hctx_types) != HCTX_MAX_TYPES); // hctx 타입 배열 크기 검증
	seq_printf(m, "%s\n", hctx_types[hctx->type]); // 이 hctx 의 유형 출력; NVMe SQ/CQ 용도 구분
	return 0;
}

/*
 * hctx_ctx_map_show - hctx->ctx_map 를 비트맵으로 출력.
 *
 * NVMe 연결:
 *   ctx_map 는 어떤 CPU/blk_mq_ctx 가 이 hctx/SQ 에 매핑되었는지 보여준다.
 *   NVMe 멀티큐 컨트롤러는 CPU 와 SQ 사이의 affinity 를 이 비트맵으로 파악할 수 있다.
 */
static int hctx_ctx_map_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx

	sbitmap_bitmap_show(&hctx->ctx_map, m); // CPU -> hctx/SQ 매핑 비트맵 출력
	return 0;
}

/*
 * blk_mq_debugfs_tags_show - blk_mq_tags 의 tag/CID 할당 상태를 출력.
 *
 * 출력 항목:
 *   nr_tags          : 이 SQ 의 최대 CID 수(NVMe queue depth).
 *   nr_reserved_tags : 관리/flush/poll 등을 위해 예약된 tag/CID 수.
 *   active_queues    : 현재 활성 큐 개수.
 *   bitmap_tags      : 일반 request 용 tag/CID 비트맵.
 *   breserved_tags   : 예약 tag/CID 비트맵.
 */
static void blk_mq_debugfs_tags_show(struct seq_file *m,
				     struct blk_mq_tags *tags)
{
	seq_printf(m, "nr_tags=%u\n", tags->nr_tags); // 이 SQ 의 최대 CID 수(NVMe queue depth)
	seq_printf(m, "nr_reserved_tags=%u\n", tags->nr_reserved_tags); // 관리/flush/poll 등을 위해 예약된 tag/CID 수
	seq_printf(m, "active_queues=%d\n",
		   READ_ONCE(tags->active_queues)); // 현재 이 tagset 을 사용 중인 active hctx/SQ 수

	seq_puts(m, "\nbitmap_tags:\n");
	sbitmap_queue_show(&tags->bitmap_tags, m); // 일반 request 용 tag/CID 비트맵 출력

	if (tags->nr_reserved_tags) { // 예약 tag 가 있을 때만 출력
		seq_puts(m, "\nbreserved_tags:\n");
		sbitmap_queue_show(&tags->breserved_tags, m); // 예약 tag/CID 비트맵 출력
	}
}

/*
 * hctx_tags_show - hctx 의 실제 tagset 상태를 출력.
 *
 * NVMe 연결:
 *   hctx->tags 는 NVMe CID 공간을 나타낸다. CID 는 NVMe 명령의 16비트
 *   identifier 이므로, nr_tags 가 NVMe queue depth 를 초과하지 않도록
 *   매핑된다(추정).
 */
static int hctx_tags_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx
	struct request_queue *q = hctx->queue; // 이 hctx 가 속한 request_queue
	int res; // mutex 획득 결과

	res = mutex_lock_interruptible(&q->elevator_lock); // elevator lock; tagset/CID 뷰 보호
	if (res) // mutex 획득 실패 시 오류 반환
		return res;
	if (hctx->tags) // hctx->tags 가 있을 때만 NVMe CID 공간 출력
		blk_mq_debugfs_tags_show(m, hctx->tags); // tag/CID 할당 상태 출력
	mutex_unlock(&q->elevator_lock); // elevator lock 해제

	return 0;
}

/*
 * hctx_tags_bitmap_show - hctx->tags 의 bitmap_tags 비트맵만 출력.
 */
static int hctx_tags_bitmap_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx
	struct request_queue *q = hctx->queue; // 이 hctx 가 속한 request_queue
	int res; // mutex 획득 결과

	res = mutex_lock_interruptible(&q->elevator_lock); // elevator lock; tagset/CID 뷰 보호
	if (res) // mutex 획득 실패 시 오류 반환
		return res;
	if (hctx->tags) // hctx->tags 존재 시 NVMe CID 비트맵 출력
		sbitmap_bitmap_show(&hctx->tags->bitmap_tags.sb, m); // 일반 tag/CID 비트맵 출력
	mutex_unlock(&q->elevator_lock); // elevator lock 해제

	return 0;
}

/*
 * hctx_sched_tags_show - I/O 스케줄러가 사용하는 tag 상태를 출력.
 *
 * NVMe 연결:
 *   스케줄러 tag 는 NVMe CID 할당 전 단계의 가상 queue depth 로 볼 수 있다.
 *   예를 들어 mq-deadline 의 read/write 큐가 이 공간을 사용한다.
 */
static int hctx_sched_tags_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx
	struct request_queue *q = hctx->queue; // 이 hctx 가 속한 request_queue
	int res; // mutex 획득 결과

	res = mutex_lock_interruptible(&q->elevator_lock); // elevator lock; 스케줄러 tag 뷰 보호
	if (res) // mutex 획득 실패 시 오류 반환
		return res;
	if (hctx->sched_tags) // 스케줄러가 사용하는 가상 queue depth 출력
		blk_mq_debugfs_tags_show(m, hctx->sched_tags); // scheduler tag/CID 상태 출력
	mutex_unlock(&q->elevator_lock); // elevator lock 해제

	return 0;
}

/*
 * hctx_sched_tags_bitmap_show - hctx->sched_tags 의 bitmap_tags 비트맵만 출력.
 */
static int hctx_sched_tags_bitmap_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx
	struct request_queue *q = hctx->queue; // 이 hctx 가 속한 request_queue
	int res; // mutex 획득 결과

	res = mutex_lock_interruptible(&q->elevator_lock); // elevator lock; 스케줄러 tag 뷰 보호
	if (res) // mutex 획득 실패 시 오류 반환
		return res;
	if (hctx->sched_tags) // hctx->sched_tags 존재 시 scheduler CID 비트맵 출력
		sbitmap_bitmap_show(&hctx->sched_tags->bitmap_tags.sb, m); // scheduler tag 비트맵 출력
	mutex_unlock(&q->elevator_lock); // elevator lock 해제

	return 0;
}

/*
 * hctx_active_show - 현재 hctx 에서 active/in-flight request 개수 출력.
 * NVMe 연결:
 *   이 값은 NVMe SQ 에서 아직 CQ completion 을 받지 않은 명령 수로 해석된다.
 */
static int hctx_active_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx

	seq_printf(m, "%d\n", __blk_mq_active_requests(hctx)); // 현재 hctx/SQ 의 in-flight NVMe 명령 수
	return 0;
}

/*
 * hctx_dispatch_busy_show - hctx->dispatch_busy 카운터 출력.
 * NVMe 연결:
 *   dispatch_busy 가 높으면 NVMe SQ/CQ 가 포화되어 있어 소프트웨어 큐에서
 *   대기하는 상태일 가능성이 크다(추정).
 */
static int hctx_dispatch_busy_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; // NVMe SQ/CQ 와 연결된 hctx

	seq_printf(m, "%u\n", hctx->dispatch_busy); // dispatch_busy 카운터; NVMe SQ/CQ 포화 여부 힌트(추정)
	return 0;
}

#define CTX_RQ_SEQ_OPS(name, type)					\
static void *ctx_##name##_rq_list_start(struct seq_file *m, loff_t *pos)  /* ctx별 rq_list 순회 시작; per-CPU s/w queue */ \
	__acquires(&ctx->lock)						 /* per-CPU queue lock 획득 선언 */ \
{									 /* per-CPU queue lock 획득 선언 */ \
	struct blk_mq_ctx *ctx = m->private;				 /* 현재 blk_mq_ctx; NVMe SQ 로 향하는 per-CPU 경로 */ \
									\
	spin_lock(&ctx->lock);						 /* per-CPU s/w queue lock; dispatch 경쟁 방지 */ \
	return seq_list_start(&ctx->rq_lists[type], *pos);		 /* CPU별 NVMe SQ 대기열(type=default/read/poll) 순회 시작 */ \
}									\
									\
static void *ctx_##name##_rq_list_next(struct seq_file *m, void *v,	 /* rq_list 다음 항목 함수 */ \
				     loff_t *pos)			\
{									 /* seq_file 와 이전 항목 */ \
	struct blk_mq_ctx *ctx = m->private;				\
									\
	return seq_list_next(v, &ctx->rq_lists[type], pos);		 /* type별 rq_list 다음 노드 */ \
}									\
									\
static void ctx_##name##_rq_list_stop(struct seq_file *m, void *v)	 /* rq_list 순회 종료 */ \
	__releases(&ctx->lock)						 /* per-CPU queue lock 해제 선언 */ \
{									\
	struct blk_mq_ctx *ctx = m->private;				 /* 현재 blk_mq_ctx */ \
									\
	spin_unlock(&ctx->lock);					 /* per-CPU s/w queue lock 해제 */ \
}									\
									\
static const struct seq_operations ctx_##name##_rq_list_seq_ops = {	 /* seq_ops 정의 */ \
	.start	= ctx_##name##_rq_list_start,				 /* rq_list 순회 시작 */ \
	.next	= ctx_##name##_rq_list_next,				 /* 다음 request */ \
	.stop	= ctx_##name##_rq_list_stop,				 /* lock 해제 */ \
	.show	= blk_mq_debugfs_rq_show,				 /* request 상세 출력(CID/flag) */ \
}

CTX_RQ_SEQ_OPS(default, HCTX_TYPE_DEFAULT); // default 타입 per-CPU NVMe 대기열 seq_ops 생성
CTX_RQ_SEQ_OPS(read, HCTX_TYPE_READ); // read 전용 per-CPU NVMe 대기열 seq_ops 생성
CTX_RQ_SEQ_OPS(poll, HCTX_TYPE_POLL); // poll 전용 per-CPU NVMe 대기열 seq_ops 생성

/*
 * blk_mq_debugfs_show - debugfs read() 콜백.
 *
 * 호출 경로(추정):
 *   read() -> seq_read() -> blk_mq_debugfs_show()
 *     -> attr->show() -> hctx_state_show()/hctx_busy_show() 등
 */
static int blk_mq_debugfs_show(struct seq_file *m, void *v)
{
	const struct blk_mq_debugfs_attr *attr = m->private; // 보여줄 debugfs attribute
	void *data = debugfs_get_aux(m->file); // request_queue/hctx/ctx 등 실제 데이터

	return attr->show(data, m); // attr->show() 호출; hctx_state_show/hctx_busy_show 등
}

/*
 * blk_mq_debugfs_write - debugfs write() 콜백.
 *
 * 호출 경로(추정):
 *   echo ... > /sys/kernel/debug/block/<disk>/state
 *     -> blk_mq_debugfs_write() -> attr->write() -> queue_state_write()
 */
static ssize_t blk_mq_debugfs_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data; // seq_file
	const struct blk_mq_debugfs_attr *attr = m->private; // debugfs attribute
	void *data = debugfs_get_aux(file); // request_queue/hctx/ctx 데이터

	/*
	 * Attributes that only implement .seq_ops are read-only and 'attr' is
	 * the same with 'data' in this case.
	 */
	if (attr == data || !attr->write) // seq_ops 전용 속성은 read-only; 쓰기 거부
		return -EPERM;

	return attr->write(data, buf, count, ppos); // attr->write() 호출; queue_state_write 등
}

/*
 * blk_mq_debugfs_open - debugfs 파일 열기 콜백.
 *
 * .seq_ops 가 있으면 seq_file 기반, 그렇지 않으면 single_open() 기반으로 연다.
 * m->private 에 attr, debugfs aux 에 data(request_queue/hctx/ctx 등)를 넣는다.
 */
static int blk_mq_debugfs_open(struct inode *inode, struct file *file)
{
	const struct blk_mq_debugfs_attr *attr = inode->i_private; // debugfs attribute(inode private)
	void *data = debugfs_get_aux(file); // request_queue/hctx/ctx auxiliary 데이터
	struct seq_file *m; // seq_file 구조체
	int ret; // open 반환값

	if (attr->seq_ops) {
		ret = seq_open(file, attr->seq_ops); // seq_file 기반 attribute?
		if (!ret) { // seq_open 초기화
			m = file->private_data; // seq_file 포인터
			m->private = data; // seq_file private 에 data 저장
		}
		return ret; // seq_open 결과 반환
	}

	if (WARN_ON_ONCE(!attr->show)) // show 콜백이 없으면 read-only
		return -EPERM;

	return single_open(file, blk_mq_debugfs_show, inode->i_private); // single_open 기반으로 read/show 콜백 등록
}

static int blk_mq_debugfs_release(struct inode *inode, struct file *file)
{
	const struct blk_mq_debugfs_attr *attr = inode->i_private; // debugfs attribute

	if (attr->show) // show 가 있으면 single_open 파일
		return single_release(inode, file); // single_release

	return seq_release(inode, file); // seq_release
}

/*
 * blk_mq_debugfs_fops - blk-mq debugfs 파일들이 공유하는 file_operations.
 * .open    : seq_file 또는 single_open 초기화.
 * .read    : seq_read, request 의 op/tag/CID 등을 사용자 버퍼로 복사.
 * .write   : state 파일 등에 run/start/kick 쓰기.
 * .llseek  : seq_lseek.
 * .release : single_release/seq_release.
 */
static const struct file_operations blk_mq_debugfs_fops = {
	.open		= blk_mq_debugfs_open, // seq_file/single_open 파일 열기
	.read		= seq_read, // request 의 op/tag/CID 등을 사용자 버퍼로 복사
	.write		= blk_mq_debugfs_write, // state 파일 등에 run/start/kick 쓰기
	.llseek		= seq_lseek, // seq_lseek
	.release	= blk_mq_debugfs_release, // single_release 또는 seq_release
};

/*
 * blk_mq_debugfs_hctx_attrs - /sys/kernel/debug/block/<disk>/mq/hctx<N>/ 하위 파일.
 * NVMe 연결:
 *   state, tags, busy, active 등은 각 NVMe SQ/CQ 의 상태를 모니터링하는 데 사용된다.
 */
static const struct blk_mq_debugfs_attr blk_mq_debugfs_hctx_attrs[] = {
	{"state", 0400, hctx_state_show}, // hctx/SQ 상태(STOPPED/TAG_ACTIVE/SCHED_RESTART/INACTIVE)
	{"flags", 0400, hctx_flags_show}, // hctx/SQ 특성(TAG_QUEUE_SHARED/BLOCKING/NO_SCHED_BY_DEFAULT)
	{"dispatch", 0400, .seq_ops = &hctx_dispatch_seq_ops}, // doorbell 직전 hctx->dispatch 리스트
	{"busy", 0400, hctx_busy_show}, // 이 SQ/CQ 에 할당된 busy request/CID 목록
	{"ctx_map", 0400, hctx_ctx_map_show}, // CPU -> hctx/SQ 매핑 비트맵
	{"tags", 0400, hctx_tags_show}, // NVMe CID 공간 상세(nr_tags, active_queues)
	{"tags_bitmap", 0400, hctx_tags_bitmap_show}, // 일반 tag/CID 비트맵
	{"sched_tags", 0400, hctx_sched_tags_show}, // 스케줄러 가상 queue depth
	{"sched_tags_bitmap", 0400, hctx_sched_tags_bitmap_show}, // 스케줄러 tag 비트맵
	{"active", 0400, hctx_active_show}, // 현재 in-flight NVMe 명령 수
	{"dispatch_busy", 0400, hctx_dispatch_busy_show}, // dispatch 큐 포화 힌트
	{"type", 0400, hctx_type_show}, // hctx 유형(default/read/poll); NVMe SQ/CQ 용도
	{},
};

/*
 * blk_mq_debugfs_ctx_attrs - CPU 단위 blk_mq_ctx 의 per-hctx 리스트를 보여준다.
 * NVMe 연결:
 *   default/read/poll 리스트는 NVMe SQ 로 본냥소프트웨어 큐에 대기 중인
 *   request 를 CPU 단위로 분류한 것이다.
 */
static const struct blk_mq_debugfs_attr blk_mq_debugfs_ctx_attrs[] = {
	{"default_rq_list", 0400, .seq_ops = &ctx_default_rq_list_seq_ops}, // default 타입 per-CPU NVMe 대기열
	{"read_rq_list", 0400, .seq_ops = &ctx_read_rq_list_seq_ops}, // read 전용 per-CPU NVMe 대기열
	{"poll_rq_list", 0400, .seq_ops = &ctx_poll_rq_list_seq_ops}, // poll 전용 per-CPU NVMe 대기열
	{},
};

/*
 * debugfs_create_files - parent 디렉터리 아래 attr[] 에 정의된 파일들을 생성.
 *
 * NVMe 연결:
 *   각 attr->name 이 /sys/kernel/debug/block/<disk>/.../ 하위에 생성되며,
 *   NVMe 드라이버/운영자가 런타임에 SQ/CQ 상태, tag/CID, dispatch 큐 등을
 *   조회할 수 있게 된다.
 */
static void debugfs_create_files(struct request_queue *q, struct dentry *parent,
				 void *data,
				 const struct blk_mq_debugfs_attr *attr)
{
	lockdep_assert_held(&q->debugfs_mutex); // debugfs 등록 직렬화; NVMe 장치 등록/제거 경쟁 방지
	/*
	 * debugfs_mutex should not be nested under other locks that can be
	 * grabbed while queue is frozen.
	 */
	lockdep_assert_not_held(&q->elevator_lock); // elevator_lock 과 중첩 금지; deadlock 방지
	lockdep_assert_not_held(&q->rq_qos_mutex); // rq_qos_mutex 와 중첩 금지

	if (IS_ERR_OR_NULL(parent)) // 상위 디렉터리 없으면 등록 중단
		return;

	for (; attr->name; attr++) // attr 배열 끝까지 파일 생성
		debugfs_create_file_aux(attr->name, attr->mode, parent, // debugfs 파일 생성; NVMe 상태 노출
				    (void *)attr, data, &blk_mq_debugfs_fops);
}

/*
 * blk_mq_debugfs_register - request_queue 의 최상위 debugfs 디렉터리와
 *     하위 hctx/rq_qos 디렉터리 및 파일들을 등록한다.
 *
 * 호출 경로(추정):
 *   blk_register_queue() -> blk_mq_debugfs_register()
 *     -> debugfs_create_files(queue attrs)
 *     -> blk_mq_debugfs_register_hctx() per hctx
 *     -> blk_mq_debugfs_register_rq_qos()
 *
 * NVMe 연결:
 *   NVMe 장치가 blk_register_queue() 로 등록될 때 이 함수가 호출되어
 *   /sys/kernel/debug/block/nvmeXnY/ 밑에 mq/, rqos/ 등을 만든다.
 */
void blk_mq_debugfs_register(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx; // 순회용 hctx 포인터
	unsigned long i; // hctx 인덱스

	debugfs_create_files(q, q->debugfs_dir, q, blk_mq_debugfs_queue_attrs); // 디스크 단위 debugfs 파일 등록

	queue_for_each_hw_ctx(q, hctx, i) { // 모든 hctx/SQ 를 순회
		if (!hctx->debugfs_dir) // 아직 debugfs 가 없는 hctx 만 등록
			blk_mq_debugfs_register_hctx(q, hctx); // 개별 hctx/SQ debugfs 디렉터리 생성
	}

	blk_mq_debugfs_register_rq_qos(q); // rq_qos(WBT/latency) debugfs 등록
}

/*
 * blk_mq_debugfs_register_ctx - hctx 아래 CPU 단위 blk_mq_ctx 디렉터리 생성.
 *
 * NVMe 연결:
 *   ctx->cpu 에 해당하는 CPU 가 이 NVMe SQ 에 I/O 를 제출하는 경로를
 *   디버깅할 수 있다. ctx->rq_lists[type] 은 소프트웨어 단계의 per-CPU 대기열.
 */
static void blk_mq_debugfs_register_ctx(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_ctx *ctx)
{
	struct dentry *ctx_dir; // cpuN 디렉터리 dentry
	char name[20]; // cpu 번호 문자열 버퍼

	snprintf(name, sizeof(name), "cpu%u", ctx->cpu); // cpuN 형식 문자열 생성
	ctx_dir = debugfs_create_dir(name, hctx->debugfs_dir); // hctx 아래 per-CPU 디렉터리 생성

	debugfs_create_files(hctx->queue, ctx_dir, ctx, // ctx 단위 debugfs 파일 생성
			     blk_mq_debugfs_ctx_attrs); // default/read/poll rq_list 노출
}

/*
 * blk_mq_debugfs_register_hctx - 단일 blk_mq_hw_ctx 의 debugfs 디렉터리를 생성하고
 *     하위 attr 파일과 per-cpu ctx 디렉터리를 등록한다.
 *
 * 호출 경로(추정):
 *   blk_mq_debugfs_register() -> blk_mq_debugfs_register_hctx()
 *     -> debugfs_create_files(hctx attrs)
 *     -> blk_mq_debugfs_register_ctx() per ctx
 *
 * NVMe 연결:
 *   hctx->queue_num 은 NVMe SQ 번호와 1:1 또는 N:1 로 매핑될 수 있다(추정).
 *   hctx->tags 는 NVMe CID 공간, hctx->dispatch 는 doorbell 직전 큐.
 */
void blk_mq_debugfs_register_hctx(struct request_queue *q,
				  struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_ctx *ctx; // hctx 아래 ctx 순회용 포인터
	char name[20]; // hctxN 문자열 버퍼
	int i; // ctx 인덱스

	if (!q->debugfs_dir) // 상위 queue 디렉터리 없으면 등록 불가
		return;

	snprintf(name, sizeof(name), "hctx%u", hctx->queue_num); // hctxN 형식 문자열 생성
	hctx->debugfs_dir = debugfs_create_dir(name, q->debugfs_dir); // queue 디렉터리 아래 hctx/SQ 디렉터리 생성

	debugfs_create_files(q, hctx->debugfs_dir, hctx, // hctx 단위 debugfs 파일 등록
			     blk_mq_debugfs_hctx_attrs); // state/tags/busy/active 등 NVMe SQ/CQ 상태

	hctx_for_each_ctx(hctx, ctx, i) // 이 hctx/SQ 에 매핑된 모든 CPU 순회
		blk_mq_debugfs_register_ctx(hctx, ctx); // per-CPU ctx 디렉터리 등록
}

/*
 * blk_mq_debugfs_unregister_hctx - hctx 의 debugfs 디렉터리를 제거한다.
 *
 * NVMe 연결:
 *   NVMe 장치 제거/reset 시 관련 SQ/CQ 디버깅 디렉터리를 정리한다.
 */
void blk_mq_debugfs_unregister_hctx(struct blk_mq_hw_ctx *hctx)
{
	if (!hctx->queue->debugfs_dir) // hctx 가 속한 queue 의 debugfs_dir 확인
		return;
	debugfs_remove_recursive(hctx->debugfs_dir); // hctx/SQ 디버깅 디렉터리 재귀 삭제
	hctx->sched_debugfs_dir = NULL; // scheduler 디렉터리 참조 초기화
	hctx->debugfs_dir = NULL; // hctx debugfs_dir 참조 초기화
}

void blk_mq_debugfs_register_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx; // 순회용 hctx 포인터
	unsigned int memflags; // debugfs lock 저장용 플래그
	unsigned long i; // hctx 인덱스

	memflags = blk_debugfs_lock(q); // q->debugfs_mutex 잠금; NVMe 장치 등록 직렬화
	queue_for_each_hw_ctx(q, hctx, i) // 모든 hctx/SQ 등록
		blk_mq_debugfs_register_hctx(q, hctx);
	blk_debugfs_unlock(q, memflags); // debugfs 잠금 해제
}

void blk_mq_debugfs_unregister_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx; // 순회용 hctx 포인터
	unsigned long i; // hctx 인덱스

	queue_for_each_hw_ctx(q, hctx, i) // 모든 hctx/SQ 의 debugfs 제거
		blk_mq_debugfs_unregister_hctx(hctx); // NVMe 장치 제거/reset 시 정리
}

/*
 * blk_mq_debugfs_register_sched - I/O 스케줄러(mq-deadline, kyber, bfq 등)의
 *     debugfs 파일을 "sched/" 하위에 등록한다.
 *
 * NVMe 연결:
 *   NVMe 장치는 종종 mq-deadline/kyber 를 사용하며, 이 디렉터리에서
 *   read/write 큐 분리, latency target 등을 확인할 수 있다.
 */
void blk_mq_debugfs_register_sched(struct request_queue *q)
{
	struct elevator_type *e = q->elevator->type; // 현재 I/O 스케줄러 타입(mq-deadline/kyber/bfq)

	lockdep_assert_held(&q->debugfs_mutex); // debugfs_mutex 보유 검증

	/*
	 * If the parent directory has not been created yet, return, we will be
	 * called again later on and the directory/files will be created then.
	 */
	if (!q->debugfs_dir) // 상위 디렉터리가 없으면 나중에 다시 호출됨
		return;

	if (!e->queue_debugfs_attrs) // 스케줄러별 debugfs attr 없으면 생략
		return;

	q->sched_debugfs_dir = debugfs_create_dir("sched", q->debugfs_dir); // "sched" 디렉터리 생성

	debugfs_create_files(q, q->sched_debugfs_dir, q, e->queue_debugfs_attrs); // 스케줄러 debugfs 파일 등록
}

void blk_mq_debugfs_unregister_sched(struct request_queue *q)
{
	lockdep_assert_held(&q->debugfs_mutex); // debugfs_mutex 보유 검증

	debugfs_remove_recursive(q->sched_debugfs_dir); // sched 디렉터리 재귀 삭제
	q->sched_debugfs_dir = NULL; // sched_debugfs_dir 참조 초기화
}

/*
 * rq_qos_id_to_name - rq_qos(WBT, latency, cost) ID 를 디렉터리 이름으로 변환.
 * NVMe 연결:
 *   NVMe SSD 에서는 WBT(writeback throttling) 와 latency-based QoS 가
 *   queue depth/CID 생성 속도에 영향을 줄 수 있다.
 */
static const char *rq_qos_id_to_name(enum rq_qos_id id)
{
	switch (id) {
	case RQ_QOS_WBT: // Writeback Throttling; NVMe queue depth/CID 생성 속도 제어(추정)
		return "wbt";
	case RQ_QOS_LATENCY: // Latency QoS; NVMe I/O 지연 목표 제어(추정)
		return "latency";
	case RQ_QOS_COST: // Cost QoS; NVMe I/O 비용 기반 조절(추정)
		return "cost";
	}
	return "unknown"; // 알 수 없는 rq_qos ID
}

/*
 * blk_mq_debugfs_register_rqos - 단일 rq_qos 의 debugfs 디렉터리를 "rqos/" 아래 생성.
 */
static void blk_mq_debugfs_register_rqos(struct rq_qos *rqos)
{
	struct request_queue *q = rqos->disk->queue; // 이 rqos 가 속한 request_queue
	const char *dir_name = rq_qos_id_to_name(rqos->id); // rqos 디렉터리 이름

	lockdep_assert_held(&q->debugfs_mutex); // debugfs_mutex 보유 검증

	if (rqos->debugfs_dir || !rqos->ops->debugfs_attrs) // 이미 등록되었거나 debugfs attr 없으면 skip
		return;

	if (!q->rqos_debugfs_dir) // rqos 상위 디렉터리가 없으면 생성
		q->rqos_debugfs_dir = debugfs_create_dir("rqos", // "rqos" 디렉터리 생성
							 q->debugfs_dir);

	rqos->debugfs_dir = debugfs_create_dir(dir_name, q->rqos_debugfs_dir); // wbt/latency/cost 서브디렉터리 생성
	debugfs_create_files(q, rqos->debugfs_dir, rqos, // rqos debugfs 파일 등록
			     rqos->ops->debugfs_attrs); // NVMe QoS 런타임 통계 노출
}

/*
 * blk_mq_debugfs_register_rq_qos - request_queue 에 연결된 모든 rq_qos 를 debugfs 에 등록.
 *
 * NVMe 연결:
 *   NVMe QoS 정책(WBT, latency)이 CID 생성/완료 속도를 조절할 수 있으며,
 *   이 디렉터리에서 해당 정책의 런타임 통계를 확인한다.
 */
void blk_mq_debugfs_register_rq_qos(struct request_queue *q)
{
	lockdep_assert_held(&q->debugfs_mutex); // debugfs_mutex 보유 검증

	if (q->rq_qos) { // rq_qos 리스트가 연결되어 있으면 순회
		struct rq_qos *rqos = q->rq_qos; // 첫 번째 rqos

		while (rqos) { // 모든 rqos 를 순회하며 등록
			blk_mq_debugfs_register_rqos(rqos); // 개별 rqos 디렉터리/파일 생성
			rqos = rqos->next; // 다음 rqos
		}
	}
}

/*
 * blk_mq_debugfs_register_sched_hctx - hctx 단위 스케줄러 debugfs 디렉터리 생성.
 *
 * NVMe 연결:
 *   hctx/SQ 단위로 mq-deadline 의 read/write 큐 상태, kyber 의 latency
 *   통계 등을 확인할 수 있다.
 */
void blk_mq_debugfs_register_sched_hctx(struct request_queue *q,
					struct blk_mq_hw_ctx *hctx)
{
	struct elevator_type *e = q->elevator->type; // 현재 I/O 스케줄러 타입

	lockdep_assert_held(&q->debugfs_mutex); // debugfs_mutex 보유 검증

	/*
	 * If the parent debugfs directory has not been created yet, return;
	 * We will be called again later on with appropriate parent debugfs
	 * directory from blk_register_queue()
	 */
	if (!hctx->debugfs_dir) // hctx debugfs_dir 이 없으면 나중에 다시 호출
		return;

	if (!e->hctx_debugfs_attrs) // hctx 단위 스케줄러 attr 없으면 생략
		return;

	hctx->sched_debugfs_dir = debugfs_create_dir("sched", // "sched" 서브디렉터리 생성
						     hctx->debugfs_dir);
	debugfs_create_files(q, hctx->sched_debugfs_dir, hctx, // hctx/SQ 단위 스케줄러 debugfs 파일 등록
			     e->hctx_debugfs_attrs);
}

void blk_mq_debugfs_unregister_sched_hctx(struct blk_mq_hw_ctx *hctx)
{
	lockdep_assert_held(&hctx->queue->debugfs_mutex); // debugfs_mutex 보유 검증

	if (!hctx->queue->debugfs_dir) // 상위 queue 디렉터리 없으면 skip
		return;
	debugfs_remove_recursive(hctx->sched_debugfs_dir); // hctx/SQ 단위 sched 디렉터리 재귀 삭제
	hctx->sched_debugfs_dir = NULL; // sched_debugfs_dir 참조 초기화
}

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 blk-mq 의 request_queue, blk_mq_hw_ctx, blk_mq_ctx, rq_qos 등을
 *   /sys/kernel/debug/block/<disk>/ 아래에 노출하여 NVMe I/O 경로를 진단할 수
 *   있게 한다.
 *
 * - 각 blk_mq_hw_ctx 는 NVMe SQ/CQ 와 밀접하게 연결되며, hctx->tags 의 각 비트는
 *   NVMe command identifier(CID)로 대응될 수 있다.
 *
 * - request 의 .tag, .state, .cmd_flags(POLLED/FUA/PREFLUSH) 값을 통해 NVMe
 *   명령이 SQ 에 삽입되었는지, doorbell 이 발생했는지, CQ completion 을
 *   받았는지를 추론할 수 있다(추정).
 *
 * - queue_state_write() 의 "run"/"start"/"kick" 동작은 NVMe 드라이버의
 *   nvme_queue_rq() -> nvme_submit_cmd(doorbell) 경로를 다시 활성화하는
 *   디버깅 수단으로 사용된다(추정).
 *
 * - 이 파일은 block/blk-mq.c(핵심 I/O 흐름), block/blk-mq-tag.c(CID/tag 관리),
 *   drivers/nvme/host/pci.c(NVMe SQ/CQ/doorbell) 와 논리적으로 연결된다.
 */
