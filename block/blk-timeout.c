// SPDX-License-Identifier: GPL-2.0
/*
 * blk-timeout.c - 블록 레이어 요청(request) 타임아웃 처리
 *
 * 이 파일은 NVMe SSD를 포함한 블록 디바이스에서 I/O 요청이 일정 시간 내에
 * 완료되지 않을 때 만료(deadline)를 감지하고 상위/하위 레이어에 알리는
 * 타임아웃 인프라를 제공한다.
 *
 * NVMe 관점에서 본 호출 흐름:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   로 제출된 명령(CID)이 NVMe SQ/CQ를 통해 완료되지 않으면,
 *   이 파일의 타이머가 만료되어 nvme_timeout -> nvme_abort_work 같은
 *   NVMe 드라이버 recovery 경로로 이어진다 (추정).
 *
 * 연관 파일:
 *   - block/blk-mq.c: tag/큐 관리, request 할당 및 완료
 *   - block/blk-core.c: bio -> request 변환
 *   - drivers/nvme/host/pci.c 또는 core.c: NVMe abort/reset 처리
 *
 * Functions related to generic timeout handling of requests.
 */
#include <linux/kernel.h>   /* 커널 기본 타입/매크로; NVMe 드라이버와 동일한 커널 ABI 사용 */
#include <linux/module.h>   /* 모듈 인프라; NVMe host 드라이버도 이 헤더 기반 */
#include <linux/blkdev.h>   /* request_queue, request, gendisk 등 블록 레이어 핵심 구조체 */
#include <linux/fault-inject.h> /* 인위적 fault injection; NVMe error-recovery 검증용 */

#include "blk.h"            /* request_queue 낮은 수준 함수, queue_flags 선언 */
#include "blk-mq.h"         /* blk-mq tag, hctx, request 할당/완료 인터페이스 (NVMe mq 직결) */

#ifdef CONFIG_FAIL_IO_TIMEOUT

/*
 * fail_io_timeout: 개발/테스트용 fault-injection 속성.
 * 이것이 활성화되면 블록 레이어가 의도적으로 타임아웃을 발생시켜
 * NVMe error-recovery 경로(nvme_timeout, nvme_abort_work 등)를
 * 검증할 수 있게 한다.
 */
static DECLARE_FAULT_ATTR(fail_io_timeout); /* NVMe admin/io queue 모두 대상으로 인위 timeout 주입 가능 (추정) */

static int __init setup_fail_io_timeout(char *str)
{
	return setup_fault_attr(&fail_io_timeout, str); /* debugfs/boot 파라미터 파싱 -> fault 확률/간격 설정 */
}
__setup("fail_io_timeout=", setup_fail_io_timeout); /* 부팅 시 "fail_io_timeout=..."로 NVMe recovery 검증 */

/*
 * __blk_should_fake_timeout - 인위적 타임아웃을 발생시켜야 하는지 판단
 * @q: 대상 request_queue
 *
 * NVMe admin/io queue 모두 이 함수를 통해 테스트용 timeout을
 * 주입받을 수 있다. 실제 운영 환경에서는 보통 false를 반환한다.
 */
bool __blk_should_fake_timeout(struct request_queue *q)
{
	return should_fail(&fail_io_timeout, 1); /* 확률적으로 true -> NVMe SQ/CQ 완료 없이 timeout 처리 */
}
EXPORT_SYMBOL_GPL(__blk_should_fake_timeout); /* NVMe 드라이버 등 LLD에서 호출 가능 (추정) */

static int __init fail_io_timeout_debugfs(void)
{
	/* debugfs 아래 fail_io_timeout 노드를 만들어 사용자가 주입 빈도를 조절할 수 있게 한다. */
	struct dentry *dir = fault_create_debugfs_attr("fail_io_timeout",
						NULL, &fail_io_timeout);

	return PTR_ERR_OR_ZERO(dir); /* 생성 실패 시 NVMe fault-injection 테스트 불가 */
}

late_initcall(fail_io_timeout_debugfs); /* 늦은 initcall로 NVMe 드라이버 초기화 이후 노드 생성 */

/*
 * part_timeout_show - 파티션의 QUEUE_FLAG_FAIL_IO 플래그 상태를 sysfs로 노출
 * @dev: 블록 디바이스
 * @attr: 디바이스 속성
 * @buf: 사용자 버퍼
 *
 * 이 플래그가 설정되면 해당 큐의 I/O 타임아웃이 실제로 상위에 전달된다.
 * NVMe에서도 이 플래그는 error recovery 중 추가 timeout 처리 여부를
 * 결정하는 데 참조될 수 있다 (추정).
 */
ssize_t part_timeout_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct gendisk *disk = dev_to_disk(dev); /* 디바이스 -> gendisk; NVMe namespace 매핑의 시작점 */
	int set = test_bit(QUEUE_FLAG_FAIL_IO, &disk->queue->queue_flags); /* queue_flags 비트 테스트: NVMe reset 중 timeout 상향 전파 여부 (추정) */

	return sprintf(buf, "%d\n", set != 0); /* 0/1 반환; NVMe 사용자는 이 값으로 fail_io 동작 확인 */
}

/*
 * part_timeout_store - 사용자가 0/1로 QUEUE_FLAG_FAIL_IO를 설정/해제
 * @dev: 블록 디바이스
 * @attr: 디바이스 속성
 * @buf: 입력 버퍼
 * @count: 입력 길이
 *
 * NVMe 컨트롤러 reset/삭제 중 타임아웃 전파를 제어할 때 사용될 수 있다.
 */
ssize_t part_timeout_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct gendisk *disk = dev_to_disk(dev); /* 대상 NVMe namespace/파티션의 디스크 */
	int val;

	if (count) { /* count가 0이면 아무 동작 안 함 */
		struct request_queue *q = disk->queue; /* NVMe I/O queue (또는 admin queue 매핑) */
		char *p = (char *) buf;

		/* 사용자가 쓴 10진수 값을 파싱하여 플래그를 갱신한다. */
		val = simple_strtoul(p, &p, 10); /* 0: timeout 상위 전파 억제; 1: timeout 시 상위 오류 전파 */
		if (val)
			blk_queue_flag_set(QUEUE_FLAG_FAIL_IO, q); /* NVMe timeout -> upper-layer error (추정) */
		else
			blk_queue_flag_clear(QUEUE_FLAG_FAIL_IO, q); /* NVMe timeout은 낮은 레이어에서 회복 시도 */
	}

	return count;
}

#endif /* CONFIG_FAIL_IO_TIMEOUT */

/**
 * blk_abort_request - 지정한 request에 대한 recovery 시작 요청
 * @req: 대상 request
 *
 * 목적:
 *   이 request의 deadline을 현재 jiffies로 강제 설정하고, kblockd 워커의
 *   timeout_work를 즉시 스케줄하여 타임아웃 스캔이 일어나게 한다.
 *
 * 호출 경로 (NVMe):
 *   상위 계층의 강제 abort 또는 NVMe 드라이버 코드에서 호출 가능하다.
 *   NVMe에서는 nvme_timeout, nvme_delete_ctrl 과정에서
 *   특정 req를 대상으로 호출될 수 있다 (추정).
 *
 * NVMe 연결:
 *   - req->deadline을 jiffies로 덮어쓰면 blk_timeout_work()가 해당 request를
 *     곧바로 만료로 판정한다.
 *   - 이후 q->timeout_fn (NVMe의 경우 보통 nvme_timeout)이 호출되어
 *     해당 CID를 abort하거나 컨트롤러를 reset한다 (추정).
 */
void blk_abort_request(struct request *req)
{
	/*
	 * All we need to ensure is that timeout scan takes place
	 * immediately and that scan sees the new timeout value.
	 * No need for fancy synchronizations.
	 */
	/* req->deadline를 현재 jiffies로 설정해 즉시 timeout 대상이 되게 한다. */
	WRITE_ONCE(req->deadline, jiffies); /* NVMe CQ 완료 미도달 시점을 현재로 강제 맞춤; 다른 CPU timeout 스캔에 즉시 가시화 (smp 의미 보장) */
	/* kblockd 워커에 timeout_work를 예약하여 비동기적으로 timeout 스캔을 트리거한다. */
	kblockd_schedule_work(&req->q->timeout_work); /* workqueue kick -> blk_timeout_work -> q->timeout_fn(NVMe nvme_timeout) (추정) */
}
EXPORT_SYMBOL_GPL(blk_abort_request); /* NVMe delete_ctrl/reset에서 강제 abort 시 호출 가능 */

/*
 * blk_timeout_mask: blk_round_jiffies()에서 사용하는 HZ 기반 마스크.
 * __read_mostly로 지정되어 타이머 그룹화에 따른 캐시 효율을 높인다.
 */
static unsigned long blk_timeout_mask __read_mostly; /* timeout 타이머 그룹화 마스크: NVMe 고깊이 큐에서 timer wheel 갱신 빈도 감소 */

static int __init blk_timeout_init(void)
{
	/*
	 * HZ를 기준으로 2의 거듭제곱을 올림한 뒤 1을 빼서,
	 * 하위 비트가 모두 1인 마스크를 만든다. 예: HZ=1000이면 1023.
	 * 이 마스크는 blk_round_jiffies()에서 타이머를 대략 1초 단위로
	 * 그룹화하는 데 쓰인다.
	 */
	blk_timeout_mask = roundup_pow_of_two(HZ) - 1; /* HZ=1000 -> 1023; NVMe CID별 deadline을 1초 단위로 묶어 timer 관리 */
	return 0;
}

late_initcall(blk_timeout_init); /* 블록 레이어 초기화 후 NVMe 타임아웃 인프라 준비 */

/*
 * Just a rough estimate, we don't care about specific values for timeouts.
 */
/*
 * blk_round_jiffies - 타이머 만료 시각을 HZ 단위로 반올림
 * @j: 기준 jiffies
 *
 * 타이머 슬랙(slack)을 주어 여러 request의 timeout 타이머를
 * 같은 만료 지점에 모아 처리 부하를 줄인다.
 * NVMe의 경우 큐 깊이(queue depth)만큼 많은 request가 동시에 진행될 수
 * 있으므로, 이 반올림은 timer wheel 갱신 빈도를 낮춘다.
 */
static inline unsigned long blk_round_jiffies(unsigned long j)
{
	return (j + blk_timeout_mask) + 1; /* 상한 올림: NVMe SQ에 있는 여러 CID deadline을 HZ 경계로 그룹화 */
}

/**
 * blk_rq_timeout - request의 타임아웃 시각을 BLK_MAX_TIMEOUT으로 제한
 * @timeout: 후보 만료 jiffies
 *
 * BLK_MAX_TIMEOUT(일반적으로 30분)을 초과하면 최대값으로 clamping한다.
 * NVMe admin queue 명령이나 장시간 대기 가능한 PASSTHROUGH 명령에도
 * 시스템 전체 타임아웃 상한이 적용된다.
 */
unsigned long blk_rq_timeout(unsigned long timeout)
{
	unsigned long maxt;

	maxt = blk_round_jiffies(jiffies + BLK_MAX_TIMEOUT); /* 시스템 전체 상한 시각; NVMe admin identify/format 등도 이 범위로 제한 */
	if (time_after(timeout, maxt)) /* deadline이 상한을 넘어가면 clamping */
		timeout = maxt; /* NVMe firmware 장시간 명령이라도 커널은 BLK_MAX_TIMEOUT까지만 기다림 */

	return timeout; /* NVMe timeout_fn이 보게 될 최종 expiry */
}

/**
 * blk_add_timer - 단일 request에 대한 타임아웃 타이머 시작
 * @req: 곧 실행될 request
 *
 * 목적:
 *   request를 큐에 넣을 때마다 deadline 타이머를 설정하고,
 *   request 완료 시 취소한다.
 *
 * 호출 경로 (NVMe):
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 이전/이후에
 *   blk_mq_start_request() 코드에서 blk_add_timer()가 호출된다 (추정).
 *
 * NVMe 연결:
 *   - req->timeout이 0이면 q->rq_timeout(기본 30초)을 상속받는다.
 *   - req->deadline에 만료 시각을 기록하여 blk_timeout_work()가
 *     만료 request를 찾을 수 있게 한다.
 *   - RQF_TIMED_OUT 플래그를 클리어하여 이전 타임아웃 이력을 초기화한다.
 *   - q->timeout 타이머를 expiry로 갱신; 이미 더 빠른 만료가 있으면
 *     무시하거나 HZ/2 이상 차이가 있을 때만 mod_timer()한다.
 */
void blk_add_timer(struct request *req)
{
	struct request_queue *q = req->q; /* NVMe I/O queue (또는 admin queue); request가 속한 블록 큐 */
	unsigned long expiry;

	/*
	 * Some LLDs, like scsi, peek at the timeout to prevent a
	 * command from being retried forever.
	 */
	/*
	 * req->timeout가 아직 설정되지 않았으면 큐의 기본 타임아웃(q->rq_timeout)을
	 * 상속받는다. NVMe I/O request도 이 경로에서 기본 timeout을 갖게 된다.
	 */
	if (!req->timeout)
		req->timeout = q->rq_timeout; /* NVMe default timeout(보통 30초)을 CID별 request에 복사 */

	/* 이전에 timeout이 발생했던 이력을 지워 새 타이머 주기를 시작한다. */
	req->rq_flags &= ~RQF_TIMED_OUT; /* requeue 후에도 NVMe CID 타이머 이력 초기화 -> 재시도 시 새로운 timeout 기회 */

	/* 현재 시간에 request 전용 timeout을 더해 최초 deadline을 계산한다. */
	expiry = jiffies + req->timeout; /* doorbell 이후 NVMe CQ 미완료 시 만료 시각 */
	/* 다른 CPU에서 동시에 스캔할 수 있으므로 WRITE_ONCE로 deadline을 발행한다. */
	WRITE_ONCE(req->deadline, expiry); /* NVMe ISR -> blk_mq_complete_request -> del_timer 경로와 race 방지; store release 의미 (추정) */

	/*
	 * If the timer isn't already pending or this timeout is earlier
	 * than an existing one, modify the timer. Round up to next nearest
	 * second.
	 */
	/* 만료 시각을 반올림하고 BLK_MAX_TIMEOUT 상한을 적용한다. */
	expiry = blk_rq_timeout(blk_round_jiffies(expiry)); /* HZ 단위 그룹화 + 30분 상한: NVMe 다중 CID 타이머 batching */

	if (!timer_pending(&q->timeout) || /* 큐 전역 timeout 타이머가 아직 동작 중이 아니면 갱신 필요 */
	    time_before(expiry, q->timeout.expires)) { /* 새 CID deadline이 기존 가장 빠른 만료보다 이륙 */
		unsigned long diff = q->timeout.expires - expiry;

		/*
		 * Due to added timer slack to group timers, the timer
		 * will often be a little in front of what we asked for.
		 * So apply some tolerance here too, otherwise we keep
		 * modifying the timer because expires for value X
		 * will be X + something.
		 */
		/*
		 * 타이머가 아직 예약되지 않았거나, 기존 만료 시각과 HZ/2 이상
		 * 차이가 날 때만 mod_timer()로 갱신한다. 그룹화로 인한
		 * 미세한 시각 차이로 인해 불필요한 timer wheel 수정을 막는다.
		 */
		if (!timer_pending(&q->timeout) || (diff >= HZ / 2)) /* NVMe doorbell burst로 CID deadline이 약간 당겨져도 timer 재프로그래밍 억제 */
			mod_timer(&q->timeout, expiry); /* 큐 전역 타이머 재설정 -> blk_timeout_work -> NVMe timeout callback (추정) */
	}

}

/* NVMe 관점 핵심 요약 */
/*
 * - blk_add_timer는 submit_bio -> ... -> nvme_submit_cmd(doorbell) 직전에
 *   각 CID에 대한 deadline을 설정하며, 이 값은 NVMe 컨트롤러가 CQ에
 *   완료 항목을 기록하지 못했을 때 만료된다.
 * - blk_abort_request는 req->deadline을 강제로 현재 시간으로 당겨
 *   kblockd의 timeout_work를 즉시 깨워 recovery를 시작하게 한다.
 * - blk_round_jiffies는 NVMe의 높은 queue depth에서 수많은 타이머가
 *   흩어져 갱신되는 것을 방지하기 위해 만료 시각을 HZ 단위로 그룹화한다.
 * - 이 파일은 blk-mq.c의 tag 할당/완료 흐름과 함께 동작하며,
 *   실제 NVMe abort/reset 동작은 drivers/nvme/host/의
 *   nvme_timeout, nvme_abort_work 같은 함수가 담당한다 (추정).
 * - CONFIG_FAIL_IO_TIMEOUT은 개발/테스트용으로 인위적 타임아웃을
 *   주입하여 NVMe error-recovery 경로를 검증할 수 있게 한다.
 */
