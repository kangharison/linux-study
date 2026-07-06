// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어]
 * blk-timeout.c - 블록 레이어 request 타임아웃 감지·복구를 위한 저수준 타이머 인프라
 *
 * === 파일의 역할 ===
 * 이 파일은 blk-mq 기반 블록 계층에서 in-flight 상태로 하드웨어에 발행된
 * struct request 가 지정된 시간(deadline) 안에 완료되지 않는 상황에 대비한
 * "타이머 설정/조정" 하부 계층을 제공한다. 실제로 만료 여부를 스캔하고
 * 드라이버 콜백을 호출하는 주체는 block/blk-mq.c 의 blk_mq_timeout_work()
 * 이며, 이 파일은 (1) request 가 하드웨어로 발행될 때마다 개별 deadline 을
 * 계산해 req->deadline 에 기록하고, 큐 전역에 단 하나만 존재하는
 * q->timeout 타이머를 필요한 경우에만 재프로그래밍하는 blk_add_timer(),
 * (2) 상위 코드(LLD)가 특정 request 에 대해 강제로 즉시 recovery 를
 * 시작시키고 싶을 때 deadline 을 현재 시각으로 앞당겨 타임아웃 워크를
 * 즉시 깨우는 blk_abort_request() 라는 두 핵심 유틸리티를 제공한다.
 * 그 밖에 CONFIG_FAIL_IO_TIMEOUT 빌드 옵션이 켜졌을 때는 fault-injection
 * 프레임워크(linux/fault-inject.h)를 이용해 인위적으로 타임아웃을
 * 발생시켜, SCSI/NVMe 등 LLD(Low Level Driver, 하위 계층 드라이버)의
 * 에러 복구 경로를 실제 하드웨어 장애 없이도 검증할 수 있는 훅을 제공한다.
 * NVMe 관점에서 보면, 컨트롤러가 응답을 멈춘 hung command 를 감지하는
 * 최종 신호가 바로 이 파일이 관리하는 req->deadline / q->timeout 이며,
 * 그 신호가 만료되는 순간부터 nvme_timeout() 을 통한 abort/reset 복구
 * 절차가 시작된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * (제출 경로) submit_bio() -> blk_mq_submit_bio() -> request 할당 이후
 * blk_mq_start_request() 코드 경로에서 이 파일의 blk_add_timer() 가
 * 호출되어 req->deadline 이 설정되고, 필요하면 q->timeout 타이머가
 * mod_timer() 로 재조정된다. 이후 request 는 q->mq_ops->queue_rq()
 * (NVMe 는 nvme_queue_rq(), SQ(Submission Queue) 슬롯에 커맨드를 쓰고
 * 도어벨 레지스터를 울림)를 통해 실제 하드웨어로 전달된다.
 * (정상 완료 경로) 컨트롤러가 CQ(Completion Queue)에 완료 엔트리를 쓰고
 * 인터럽트(또는 폴링)를 통해 알리면 blk_mq_complete_request() 가 호출되어
 * 이 request 는 tag 가 반환되며 더 이상 timeout 스캔의 대상이 아니게 된다.
 * (타임아웃 경로) q->timeout 타이머가 만료되면 그 콜백인
 * blk_rq_timed_out_timer()(block/blk-core.c, softirq 컨텍스트에서 실행,
 * blk_alloc_queue() 가 timer_setup() 으로 등록)가
 * kblockd_schedule_work(&q->timeout_work) 로 워크를 예약하고, kblockd
 * 워커 스레드 컨텍스트에서 blk_mq_timeout_work()(block/blk-mq.c, blk-mq
 * 큐 초기화 시 INIT_WORK() 로 등록되어 blk_alloc_queue() 가 최초 등록한
 * 빈 blk_timeout_work() 를 덮어씀)가 실행되어 두 단계로 만료 request 를
 * 스캔한다: 1차로 blk_mq_check_expired() 가 만료 존재 유무만 확인하고,
 * 만료가 있으면 2차로 blk_mq_handle_expired() 가 blk_mq_rq_timed_out() 을
 * 호출해 req->q->mq_ops->timeout(req)(NVMe: nvme_timeout())를 실제로
 * 호출한다. nvme_timeout() 이 BLK_EH_RESET_TIMER 를 반환하면
 * blk_mq_rq_timed_out() 이 이 파일의 blk_add_timer() 를 다시 호출해
 * 타이머를 재무장한다 - 즉 이 파일과 blk-mq.c 는 "설정(add_timer) ->
 * 만료 감지(timeout_work) -> 재설정(add_timer)" 의 순환 관계로 연결된다.
 * 실행 컨텍스트: blk_add_timer()/blk_abort_request() 는 request 를
 * 제출/완료 처리하는 호출자의 컨텍스트(태스크, kblockd 워크 등)에서
 * 실행되며, part_timeout_show()/part_timeout_store() 는 sysfs 파일의
 * read()/write() 시스템 콜을 처리하는 유저 프로세스 컨텍스트에서 실행된다.
 * 이 파일 자체는 커널 모듈이 아니라 블록 서브시스템 코어의 일부로 항상
 * 빌트인되어 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 쪽: block/blk.h, block/blk-mq.h 가 선언하는 request_queue/
 * request 내부 필드(deadline, rq_flags, timeout, timeout_work 등)와
 * block/blk.h 가 정의하는 BLK_MAX_TIMEOUT(5*HZ, "Max future timer expiry
 * for timeouts")에 의존한다. 또한 큐 초기화 시점에 등록되는 q->timeout
 * 타이머 콜백(block/blk-core.c 의 blk_rq_timed_out_timer)과 q->timeout_work
 * 의 실제 워크 함수(blk-mq 큐라면 block/blk-mq.c 의 blk_mq_timeout_work)
 * 가 "이 파일이 설정한 값을 나중에 소비하는" 짝이 된다.
 * 의존받는 쪽: 이 파일이 EXPORT_SYMBOL_GPL 로 공개하는 blk_abort_request()
 * 는 SCSI/NVMe 등 각 LLD 가 자체 에러 복구 로직에서 특정 request 를 즉시
 * 강제로 timeout 처리하고 싶을 때 호출하며, __blk_should_fake_timeout()
 * 도 EXPORT 되어 블록 계층의 완료 경로 어딘가에서 "이 request 에 인위적
 * timeout 을 주입해야 하는가"를 물어볼 때 사용된다(호출부는 이 파일에는
 * 없음).
 * 데이터 흐름: request->deadline(request 개별 만료 시각, 이 파일에서
 * WRITE_ONCE() 로 쓰고, block/blk-mq.c 의 blk_mq_req_expired() 가
 * READ_ONCE() 로 읽어 torn read 를 방지)과 request_queue->timeout
 * (struct timer_list, 큐 전체에서 가장 이른 만료 시각 "하나"만 추적)이
 * 이 파일과 blk-mq.c 가 공유하는 핵심 상태다. part_timeout_show()/
 * part_timeout_store() 는 sysfs 를 통해 struct gendisk->queue->
 * queue_flags 의 QUEUE_FLAG_FAIL_IO 비트("fake timeout" 플래그)를
 * 노출해 유저스페이스가 파티션이 속한 큐 단위로 인위적 fail_io 동작을
 * 켜고 끌 수 있게 연결한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blk_add_timer(): request 제출 시 deadline 계산 + 필요할 때만 q->timeout
 *   타이머를 재프로그래밍 - 이 파일의 핵심 함수.
 * - blk_abort_request(): 특정 request 의 deadline 을 즉시 현재 시각으로
 *   당기고 timeout_work 를 바로 깨워 강제 recovery 를 시작.
 * - blk_rq_timeout(): q->timeout 타이머에 실제로 프로그래밍될 후보 만료
 *   시각을 "지금부터 최대 BLK_MAX_TIMEOUT(5초) 이내"로 clamping(다만
 *   request 자체의 req->deadline 은 이 clamp 의 영향을 받지 않음).
 * - blk_round_jiffies(): 만료 시각에 HZ 근사(2의 거듭제곱으로 올림한 HZ)
 *   만큼의 slack 을 더해, "정확한 tick 은 중요하지 않다"는 원문 주석대로
 *   과도하게 잦은 mod_timer() 재호출을 피한다.
 * - __blk_should_fake_timeout()/part_timeout_show()/part_timeout_store():
 *   CONFIG_FAIL_IO_TIMEOUT 하에서 인위적 timeout 주입을 제어하는
 *   fault-injection·sysfs 인터페이스.
 * - 이 파일은 자체 정의 구조체/enum 이 없으며, include/linux/blkdev.h 와
 *   include/linux/blk-mq.h 가 정의하는 struct request/request_queue/
 *   gendisk 의 필드 일부만 사용한다.
 *
 * Functions related to generic timeout handling of requests.
 */
#include <linux/kernel.h>	/* [한국어] 커널 기본 타입/매크로(printk, min/max 등) - 이 파일 전반에서 사용하는 커널 공통 유틸리티 */
#include <linux/module.h>	/* [한국어] EXPORT_SYMBOL_GPL 등 모듈 심볼 export 매크로 - blk_abort_request/__blk_should_fake_timeout 공개에 필요 */
#include <linux/blkdev.h>	/* [한국어] struct request_queue/request/gendisk, kblockd_schedule_work() 등 블록 레이어 핵심 선언 - 이 파일이 조작하는 자료구조의 정의 출처 */
#include <linux/fault-inject.h>	/* [한국어] struct fault_attr, DECLARE_FAULT_ATTR, should_fail() 등 fault-injection 프레임워크 - CONFIG_FAIL_IO_TIMEOUT 블록에서 인위적 timeout 주입에 사용 */

#include "blk.h"		/* [한국어] 블록 레이어 내부(비공개) 헬퍼 선언 - BLK_MAX_TIMEOUT 매크로가 이 헤더에 정의되어 있어 blk_rq_timeout()이 참조 */
#include "blk-mq.h"		/* [한국어] blk-mq 내부 선언(tag/hctx 등) - timeout_work 를 소비하는 blk_mq_timeout_work() 와 이 파일이 같은 내부 API 계열을 공유함을 나타냄 */

#ifdef CONFIG_FAIL_IO_TIMEOUT
/* [한국어] CONFIG_FAIL_IO_TIMEOUT: 커널 빌드 옵션 분기 - 이 옵션이 꺼지면 아래 fault-injection
 * 관련 코드(전역 fail_io_timeout 속성, sysfs part_timeout 속성 등)가 통째로 컴파일에서
 * 제외되어, 프로덕션 빌드에서 인위적 timeout 주입 경로의 오버헤드/공격 표면을 없앤다. */

/*
 * [한국어]
 * fail_io_timeout - 인위적 I/O 타임아웃 주입을 제어하는 fault_attr 전역 인스턴스
 *
 * DECLARE_FAULT_ATTR() 매크로가 "static struct fault_attr fail_io_timeout =
 * FAULT_ATTR_INITIALIZER;" 형태로 확장되어, probability/interval/times 등의
 * 필드를 가진 struct fault_attr 을 기본값으로 초기화한다.
 * 설정자: setup_fail_io_timeout()(부팅 커맨드라인)과 fail_io_timeout_debugfs()
 * 가 만드는 debugfs 노드(런타임)를 통해 사용자가 확률/횟수를 조정한다.
 * 읽는 자: __blk_should_fake_timeout() 이 should_fail() 을 통해 이 속성을 읽어
 * "이번 호출에서 인위적으로 실패(=timeout 유발)를 낼지" 확률적으로 판단한다.
 * 동시성: should_fail() 내부에서 자체적으로 원자 연산/락을 사용하므로 이 파일
 * 쪽에서 별도 동기화가 필요 없다.
 */
static DECLARE_FAULT_ATTR(fail_io_timeout);	/* [한국어] fail_io_timeout 전역 fault_attr 정의 - should_fail()/설정 인터페이스들이 공유하는 상태 */

/*
 * [한국어]
 * setup_fail_io_timeout - "fail_io_timeout=" 커널 부팅 파라미터 파서
 *
 * @str: 부팅 커맨드라인에서 "fail_io_timeout=" 뒤에 오는 문자열(예: 확률/간격 설정 값)
 * @return: 1(성공) 또는 0(파싱 실패) - __setup() 매크로가 기대하는 관례적 반환값
 *
 * 커널 부팅 시 "fail_io_timeout=<value>" 형태의 커맨드라인 인자가 주어지면
 * __setup() 인프라가 이 함수를 자동으로 호출한다. 실제 파싱 로직은
 * setup_fault_attr() (fault-injection 공통 프레임워크)에 위임하며, 이
 * 함수는 그 결과를 그대로 반환하는 얇은 어댑터 역할만 한다.
 * 실행 컨텍스트: 커널 초기화(init) 단계, 다른 CPU 와 경쟁하지 않는 단일
 * 스레드 부팅 컨텍스트.
 * 호출자: __setup() 매크로가 등록한 init 콜백 테이블을 통해 커널
 * 커맨드라인 파서가 호출.
 * 피호출자: setup_fault_attr().
 *
 * 호출 체인:
 *   커널 부팅 커맨드라인 파서 → [setup_fail_io_timeout] → setup_fault_attr
 */
static int __init setup_fail_io_timeout(char *str)
{
	return setup_fault_attr(&fail_io_timeout, str);	/* [한국어] "fail_io_timeout=" 뒤 문자열을 파싱해 fail_io_timeout 의 확률/간격 필드를 채움 - 실패 시 0 반환 */
}
__setup("fail_io_timeout=", setup_fail_io_timeout);	/* [한국어] "fail_io_timeout=" 부팅 파라미터를 setup_fail_io_timeout 에 바인딩하는 매크로 - 링커 섹션(.init.setup)에 엔트리 등록 */

/*
 * [한국어]
 * __blk_should_fake_timeout - 이 큐에 대해 인위적 timeout 을 발생시켜야 하는지 판단
 *
 * @q: 판단 대상 request_queue (현재 구현에서는 실제로 사용하지 않고 함수 시그니처만 보존)
 * @return: true 면 호출자가 인위적으로 timeout 인 것처럼 처리해야 함, false 면 정상 진행
 *
 * CONFIG_FAIL_IO_TIMEOUT 이 켜진 빌드에서, fault-injection 프레임워크가
 * 관리하는 확률/간격 설정에 따라 should_fail() 이 참/거짓을 반환한다.
 * 이를 통해 실제 하드웨어 장애 없이도 SCSI/NVMe 등 LLD 의 timeout 복구
 * 경로(재시도, abort, 컨트롤러 리셋 등)를 반복 가능하게 테스트할 수 있다.
 * 실행 컨텍스트: 호출자의 컨텍스트를 그대로 이어받음(별도 sleep 없음).
 * 호출자: 블록 계층의 완료/제출 경로 어딘가에서 "인위적 timeout 주입
 * 여부"를 물어볼 때 사용(이 파일 내부에는 호출부가 없음 - EXPORT_SYMBOL_GPL
 * 로 공개된 외부 API).
 * 피호출자: should_fail().
 *
 * 호출 체인:
 *   (블록 계층 완료/제출 경로) → [__blk_should_fake_timeout] → should_fail
 */
bool __blk_should_fake_timeout(struct request_queue *q)
{
	return should_fail(&fail_io_timeout, 1);	/* [한국어] fail_io_timeout 설정에 따라 확률적으로 true 반환 - size 인자 1 은 "이번 한 번의 시도"를 의미 */
}
EXPORT_SYMBOL_GPL(__blk_should_fake_timeout);	/* [한국어] GPL 모듈에도 심볼 공개 - 블록 계층 밖(다른 GPL 서브시스템/드라이버)에서도 호출 가능하게 함 */

/*
 * [한국어]
 * fail_io_timeout_debugfs - fail_io_timeout 속성을 위한 debugfs 노드 생성
 *
 * @return: 0 성공, 음수 errno 실패 - late_initcall 관례상 실패해도 부팅을 막지 않음
 *
 * /sys/kernel/debug/fail_io_timeout 아래에 확률(probability)/간격
 * (interval)/횟수(times) 등을 조정할 수 있는 파일들을 생성해, 사용자가
 * 런타임에 fail_io_timeout 의 동작을 세밀하게 제어할 수 있게 한다.
 * 실행 컨텍스트: late_initcall 단계(디바이스 드라이버 초기화가 대체로
 * 끝난 이후), 단일 스레드 부팅 컨텍스트.
 * 호출자: late_initcall() 매크로가 등록한 init 콜백 테이블.
 * 피호출자: fault_create_debugfs_attr(), PTR_ERR_OR_ZERO().
 * 에러 처리: fault_create_debugfs_attr() 이 ERR_PTR 을 반환하면
 * PTR_ERR_OR_ZERO() 가 이를 음수 errno 로 변환해 그대로 반환한다.
 *
 * 호출 체인:
 *   late_initcall 프레임워크 → [fail_io_timeout_debugfs] → fault_create_debugfs_attr
 */
static int __init fail_io_timeout_debugfs(void)
{
	struct dentry *dir = fault_create_debugfs_attr("fail_io_timeout",	/* [한국어] debugfs 디렉터리/속성 파일 생성 - 이름 "fail_io_timeout" 으로 /sys/kernel/debug 아래 노출 */
						NULL, &fail_io_timeout);	/* [한국어] 부모 dentry NULL(디버그 루트 바로 아래), 제어 대상은 위에서 정의한 fail_io_timeout 속성 */

	return PTR_ERR_OR_ZERO(dir);	/* [한국어] dir 이 에러 포인터면 그 errno 를, 정상 포인터면 0 을 반환 - late_initcall 의 표준 성공/실패 신호 */
}

late_initcall(fail_io_timeout_debugfs);	/* [한국어] 부팅 후반부(late)에 이 debugfs 등록 함수를 1회 실행하도록 커널 initcall 테이블에 등록 */

/*
 * [한국어]
 * part_timeout_show - 큐의 QUEUE_FLAG_FAIL_IO("fake timeout") 플래그 상태를 sysfs 로 노출
 *
 * @dev: sysfs 속성이 붙어 있는 struct device (파티션/디스크에 대응)
 * @attr: 이 show 콜백을 호출한 device_attribute (사용하지 않음, sysfs 콜백 시그니처상 필요)
 * @buf: 결과 문자열을 써 넣을 PAGE_SIZE 크기의 사용자 버퍼(sysfs 코어가 제공)
 * @return: buf 에 쓴 바이트 수(개행 포함) - sprintf() 의 반환값 그대로
 *
 * 유저스페이스가 "/sys/block/<disk>/<part>/timeout" 류의 sysfs 파일을
 * read() 할 때 sysfs 코어를 통해 호출되는 device_attribute 의 show
 * 콜백이다. 이 플래그가 1이면 해당 큐는 __blk_should_fake_timeout() 이
 * 참조하는 fault-injection 대상이 될 수 있음을 사용자에게 알려준다.
 * 실행 컨텍스트: read() 시스템 콜을 호출한 유저 프로세스 컨텍스트
 * (sysfs 코어가 커널 스레드로 위임하지 않는 한 sleep 가능).
 * 호출자: sysfs 코어(kernfs)가 device_attribute.show 를 통해 호출.
 * 피호출자: dev_to_disk(), test_bit(), sprintf().
 *
 * 호출 체인:
 *   sysfs read() 시스템 콜 → kernfs → [part_timeout_show] → sprintf
 */
ssize_t part_timeout_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);	/* [한국어] struct device 임베딩을 역참조해 대응하는 struct gendisk(디스크/파티션 표현) 획득 */
	int set = test_bit(QUEUE_FLAG_FAIL_IO, &disk->queue->queue_flags);	/* [한국어] disk->queue->queue_flags 비트마스크에서 QUEUE_FLAG_FAIL_IO 비트만 검사 - 0 또는 1 이 아닌 0-이 아닌 정수를 반환할 수 있어 아래서 정규화함 */

	return sprintf(buf, "%d\n", set != 0);	/* [한국어] test_bit 결과를 순수 0/1 로 정규화해 "0\n" 또는 "1\n" 형태로 buf 에 기록 - 반환값은 기록한 바이트 수 */
}

/*
 * [한국어]
 * part_timeout_store - 사용자가 쓴 0/1 값으로 QUEUE_FLAG_FAIL_IO 플래그를 설정/해제
 *
 * @dev: sysfs 속성이 붙어 있는 struct device (파티션/디스크에 대응)
 * @attr: 이 store 콜백을 호출한 device_attribute (사용하지 않음, sysfs 콜백 시그니처상 필요)
 * @buf: 사용자가 write() 한 원본 문자열(개행 포함 가능, NUL 종료는 보장되지 않을 수 있음)
 * @count: buf 에 쓰인 바이트 수 - write() 시스템 콜의 3번째 인자에서 유래
 * @return: 처리한 바이트 수(count 그대로) - sysfs 관례상 성공 시 count 를 그대로 돌려줌
 *
 * "/sys/block/<disk>/<part>/timeout" 류의 sysfs 파일에 write() 하면
 * 호출되는 device_attribute 의 store 콜백이다. 값이 0이 아니면
 * QUEUE_FLAG_FAIL_IO 를 세팅해 이 큐가 인위적 timeout 주입 대상이
 * 되도록 하고, 0이면 해제한다.
 * 실행 컨텍스트: write() 시스템 콜을 호출한 유저 프로세스 컨텍스트.
 * 호출자: sysfs 코어(kernfs)가 device_attribute.store 를 통해 호출.
 * 피호출자: dev_to_disk(), simple_strtoul(), blk_queue_flag_set(),
 * blk_queue_flag_clear().
 * 에러 처리: count 가 0이면(빈 write) 아무 것도 하지 않고 그대로 0 을
 * 반환하며, simple_strtoul() 은 파싱 실패 시 단순히 val=0 이 되어
 * "플래그 해제"로 처리될 뿐 별도 에러를 신호하지 않는다.
 *
 * 호출 체인:
 *   sysfs write() 시스템 콜 → kernfs → [part_timeout_store] →
 *   blk_queue_flag_set / blk_queue_flag_clear
 */
ssize_t part_timeout_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct gendisk *disk = dev_to_disk(dev);	/* [한국어] struct device 임베딩을 역참조해 대응하는 struct gendisk 획득 */
	int val;	/* [한국어] 사용자가 쓴 문자열을 파싱한 정수 값(0 또는 0이 아님)을 담을 변수 - 초기화 없이 선언, if(count) 블록 안에서만 대입됨 */

	if (count) {	/* [한국어] write() 로 전달된 바이트 수가 0보다 클 때만 처리 - 빈 write 는 무시 */
		struct request_queue *q = disk->queue;	/* [한국어] 이 디스크(파티션)가 속한 request_queue - 플래그를 실제로 세팅/클리어할 대상 */
		char *p = (char *) buf;	/* [한국어] buf 는 const char * 이지만 simple_strtoul() 이 비-const char ** 를 요구하므로 캐스트해 지역 포인터 p 로 별도 보관 */

		val = simple_strtoul(p, &p, 10);	/* [한국어] p 가 가리키는 문자열을 10진수로 파싱해 val 에 저장 - p 자체는 파싱이 끝난 지점으로 갱신됨(이 함수에서는 이후 사용 안 함) */
		if (val)	/* [한국어] 파싱된 값이 0이 아니면(사용자가 "1" 등을 씀) */
			blk_queue_flag_set(QUEUE_FLAG_FAIL_IO, q);	/* [한국어] q->queue_flags 의 QUEUE_FLAG_FAIL_IO 비트를 원자적으로 세팅 - 이후 이 큐는 인위적 timeout 주입 대상이 될 수 있음 */
		else	/* [한국어] 파싱된 값이 0이면(사용자가 "0"을 씀) */
			blk_queue_flag_clear(QUEUE_FLAG_FAIL_IO, q);	/* [한국어] q->queue_flags 의 QUEUE_FLAG_FAIL_IO 비트를 원자적으로 해제 - 인위적 timeout 주입 대상에서 제외 */
	}

	return count;	/* [한국어] sysfs store 콜백 관례: 성공적으로 소비한 바이트 수(=count)를 그대로 반환 */
}

#endif /* CONFIG_FAIL_IO_TIMEOUT */	/* [한국어] CONFIG_FAIL_IO_TIMEOUT 분기 종료 - 이 지점부터는 옵션과 무관하게 항상 컴파일되는 공통 코드 */

/**
 * blk_abort_request - Request recovery for the specified command
 * @req:	pointer to the request of interest
 *
 * This function requests that the block layer start recovery for the
 * request by deleting the timer and calling the q's timeout function.
 * LLDDs who implement their own error recovery MAY ignore the timeout
 * event if they generated blk_abort_request.
 */
/*
 * [한국어]
 * blk_abort_request - 지정한 request 에 대한 recovery(복구)를 즉시 시작하도록 강제 요청
 *
 * @req: 강제로 timeout 처리할 대상 request(이미 하드웨어에 발행되어 in-flight 상태여야 의미가 있음)
 * @return: 없음(void)
 *
 * 정상적으로는 req->deadline 이 지나야 q->timeout 타이머가 만료되어
 * blk_mq_timeout_work() 가 이를 감지하지만, 이 함수는 그 대기를 건너뛰고
 * "지금 당장" recovery 절차가 시작되도록 강제한다. req->deadline 을
 * 현재 jiffies 로 덮어써 사실상 이미 만료된 것으로 만들고, q->timeout
 * 타이머 자체는 건드리지 않은 채 q->timeout_work 를 곧바로 kblockd 에
 * 스케줄하여 blk_mq_timeout_work() 가 즉시 실행되도록 만든다.
 * 동시성: 별도의 락 없이 WRITE_ONCE() 하나로 충분한 이유는, 최악의
 * 경우 다른 CPU 의 스캔이 이 값을 아주 조금 늦게 보더라도 "recovery
 * 시작이 살짝 늦어지는" 정도의 손해만 있을 뿐 정확성 자체가 깨지지
 * 않기 때문이다(원문 주석 "No need for fancy synchronizations" 참고).
 * 실행 컨텍스트: 임의의 프로세스/워크 컨텍스트에서 호출 가능(내부에
 * sleep 이 없고, 원자적 대입과 워크 큐 예약만 수행하는 가벼운 함수).
 * 호출자: SCSI/NVMe 등 LLD 가 자체 에러 복구 로직에서 특정 request 를
 * 강제로 포기시키고 싶을 때 호출하는 EXPORT_SYMBOL_GPL API. LLDD 가
 * 자체 에러 복구를 이미 구현했다면, 이 함수가 유발한 timeout 이벤트를
 * 무시해도 된다는 것이 원본 kerneldoc 의 의미다.
 * 피호출자: WRITE_ONCE(직접 메모리 대입), kblockd_schedule_work().
 *
 * 호출 체인:
 *   (LLD 자체 에러 복구 코드, 예: 컨트롤러 강제 리셋 경로) →
 *   [blk_abort_request] → kblockd_schedule_work(&req->q->timeout_work)
 *   → blk_mq_timeout_work → blk_mq_handle_expired →
 *   blk_mq_rq_timed_out → req->q->mq_ops->timeout(req)
 */
void blk_abort_request(struct request *req)
{
	/*
	 * All we need to ensure is that timeout scan takes place
	 * immediately and that scan sees the new timeout value.
	 * No need for fancy synchronizations.
	 */
	WRITE_ONCE(req->deadline, jiffies);	/* [한국어] req->deadline 을 현재 jiffies 로 덮어써 "이미 만료됨" 상태로 강제 전환 - WRITE_ONCE 는 컴파일러의 분할/재배치 최적화를 막아 다른 CPU 의 READ_ONCE 스캔이 온전한 값을 보게 함 */
	kblockd_schedule_work(&req->q->timeout_work);	/* [한국어] q->timeout 타이머 만료를 기다리지 않고 timeout_work 를 kblockd workqueue 에 즉시 예약 - blk_mq_timeout_work 가 다음 스케줄링 기회에 바로 실행됨 */
}
EXPORT_SYMBOL_GPL(blk_abort_request);	/* [한국어] GPL 라이선스 모듈(대부분의 인트리 LLD)에서 blk_abort_request 를 호출할 수 있도록 심볼 공개 */

/*
 * [한국어]
 * blk_timeout_mask - blk_round_jiffies() 가 사용하는 HZ 기반 slack 마스크
 *
 * 값의 의미: roundup_pow_of_two(HZ) - 1 (예: HZ=1000 이면 1023).
 * 설정자: blk_timeout_init() 이 부팅 시 단 한 번 계산해 대입.
 * 읽는 자: blk_round_jiffies() 가 매 request 타이머 계산마다 참조.
 * 동시성: 부팅 이후에는 값이 바뀌지 않는 사실상 읽기 전용 상수이므로
 * 락이 필요 없다. __read_mostly 는 이 값이 거의 쓰이지 않고 아주 자주
 * 읽히는 데이터임을 컴파일러/캐시 배치 힌트로 알려, 자주 쓰이는(dirty)
 * 캐시라인과 분리해 캐시 효율을 높이기 위한 속성이다.
 */
static unsigned long blk_timeout_mask __read_mostly;	/* [한국어] blk_round_jiffies() 전용 slack 마스크 전역 변수 - 부팅 시 1회 계산 후 불변 */

/*
 * [한국어]
 * blk_timeout_init - blk_timeout_mask 를 HZ 기반으로 초기화하는 late initcall
 *
 * @return: 항상 0(성공) - late_initcall 관례상 실패를 신호할 상황이 없음
 *
 * HZ(초당 타이머 틱 수, 커널 설정에 따라 100/250/1000 등)를 넘는 최소
 * 2의 거듭제곱을 구한 뒤 1을 빼서, 하위 비트가 모두 1인 마스크를
 * 만든다. 이 값은 blk_round_jiffies() 에서 만료 시각에 대략 HZ 만큼의
 * slack 을 더하는 데 쓰인다.
 * 실행 컨텍스트: late_initcall 단계, 단일 스레드 부팅 컨텍스트.
 * 호출자: late_initcall() 매크로가 등록한 init 콜백 테이블.
 * 피호출자: roundup_pow_of_two().
 *
 * 호출 체인:
 *   late_initcall 프레임워크 → [blk_timeout_init] → roundup_pow_of_two
 */
static int __init blk_timeout_init(void)
{
	blk_timeout_mask = roundup_pow_of_two(HZ) - 1;	/* [한국어] HZ 를 넘는 최소 2의 거듭제곱에서 1을 뺀 값 계산 - 예: HZ=1000 → roundup_pow_of_two(1000)=1024 → mask=1023 */
	return 0;	/* [한국어] late_initcall 성공 반환 - 이 initcall 은 실패할 조건이 없음 */
}

late_initcall(blk_timeout_init);	/* [한국어] 부팅 후반부에 blk_timeout_init 을 1회 실행하도록 initcall 테이블에 등록 - blk_round_jiffies 가 쓰이기 전에 mask 가 준비되어야 함 */

/*
 * Just a rough estimate, we don't care about specific values for timeouts.
 */
/*
 * [한국어]
 * blk_round_jiffies - 만료 시각 j 에 HZ 근사만큼의 고정 slack 을 더한다
 *
 * @j: slack 을 더하기 전 기준 jiffies(대개 "지금 + request timeout")
 * @return: j 에 blk_timeout_mask+1(HZ 를 넘는 최소 2의 거듭제곱, 예: 1024)을 더한 값
 *
 * 원문 주석대로 "정확한 tick 값에 신경 쓰지 않는다(rough estimate)"는
 * 취지로, 매 request 마다 미세하게 다른 만료 시각을 그대로 mod_timer()
 * 에 넘기면 타이머가 갱신될 때마다 커널 timer wheel 을 재조정해야
 * 하는 비용이 커진다. 이 함수는 만료 시각에 고정된 slack(대략 HZ,
 * 즉 약 1초)을 더해줌으로써, blk_add_timer() 의 "HZ/2 이상 차이 날
 * 때만 mod_timer() 호출" 로직과 함께 불필요한 타이머 재프로그래밍
 * 빈도를 낮춘다. 유의할 점은 이 함수가 j 의 하위 비트를 마스킹해
 * "절대 시각 그리드"에 맞추는 것이 아니라, 단순히 고정 값을 더하는
 * 것뿐이라는 점이다(비트마스크 AND 연산이 없음).
 * 실행 컨텍스트: 호출자 컨텍스트를 그대로 이어받는 인라인 함수.
 * 호출자: blk_rq_timeout(), blk_add_timer().
 * 피호출자: 없음(단순 산술 연산).
 *
 * 호출 체인:
 *   blk_add_timer / blk_rq_timeout → [blk_round_jiffies]
 */
static inline unsigned long blk_round_jiffies(unsigned long j)
{
	return (j + blk_timeout_mask) + 1;	/* [한국어] j + (blk_timeout_mask + 1) 과 동일 - 예: mask=1023 이면 j+1024, 즉 j 에 대략 HZ(약 1초)만큼의 slack 을 더함 */
}

/**
 * blk_rq_timeout - request의 타임아웃 시각을 BLK_MAX_TIMEOUT으로 제한
 * @timeout: 후보 만료 jiffies
 *
 * BLK_MAX_TIMEOUT(5*HZ, 약 5초)을 초과하면 최대값으로 clamping한다.
 * NVMe admin queue 명령이나 장시간 대기 가능한 PASSTHROUGH 명령이
 * 훨씬 긴 req->timeout 을 요청하더라도, q->timeout 전역 타이머
 * 자체는 "지금부터 최대 5초 이내"로만 프로그래밍되어 주기적으로
 * 깨어난다(req->deadline 자체는 이 함수의 영향을 받지 않고 온전한
 * 값을 유지한다).
 */
/*
 * [한국어]
 * blk_rq_timeout - q->timeout 타이머에 실제로 프로그래밍할 만료 시각을 상한으로 clamping
 *
 * @timeout: blk_round_jiffies() 를 거친 후보 만료 jiffies(request 자체의 deadline 후보)
 * @return: timeout 이 "지금부터 BLK_MAX_TIMEOUT(5*HZ) 이내"이면 그대로, 아니면 그 상한 값
 *
 * BLK_MAX_TIMEOUT 은 block/blk.h 에 "Max future timer expiry for
 * timeouts" 로 정의된 5*HZ(약 5초) 상수다. jiffies 는 unsigned long
 * 이므로 아주 먼 미래 값을 그대로 타이머에 등록하면 워크로드에 따라
 * wrap-around(오버플로) 위험이 있고, 또한 q->timeout 은 "큐 전체에서
 * 가장 이른 만료"만 추적하는 단일 타이머이므로 실제 request 의
 * deadline 이 아무리 멀어도 주기적으로 깨어나 최신 상태를 재확인할
 * 필요가 있다. 이 함수는 그 두 목적을 위해 q->timeout 에 실제
 * 프로그래밍되는 값만 5초 이내로 제한하며, request 의 진짜
 * req->deadline(blk_add_timer 가 별도로 WRITE_ONCE 로 기록)은 그대로
 * 유지된다 - 즉 "아직 만료 안 됨"이라는 판단은 항상 req->deadline
 * 기준으로 정확하게 내려진다.
 * 실행 컨텍스트: 호출자 컨텍스트를 그대로 이어받음(부작용 없는 순수
 * 계산 함수).
 * 호출자: blk_add_timer().
 * 피호출자: blk_round_jiffies(), time_after().
 *
 * 호출 체인:
 *   blk_add_timer → [blk_rq_timeout] → blk_round_jiffies
 */
unsigned long blk_rq_timeout(unsigned long timeout)
{
	unsigned long maxt;	/* [한국어] "지금부터 BLK_MAX_TIMEOUT 이내"의 상한 jiffies 값을 담을 지역 변수 - 아래에서 계산 후 timeout 과 비교 */

	maxt = blk_round_jiffies(jiffies + BLK_MAX_TIMEOUT);	/* [한국어] 현재 시각 + 5초를 구한 뒤 HZ 근사 slack 을 더해 상한을 계산 - blk_add_timer 쪽 타이머 grouping 과 동일한 반올림 규칙 적용 */
	if (time_after(timeout, maxt))	/* [한국어] jiffies 오버플로를 고려한 안전한 비교 매크로로 timeout 이 상한을 넘는지 검사 - 단순 '>' 비교는 wrap-around 시 오판할 수 있어 사용 불가 */
		timeout = maxt;	/* [한국어] 상한을 넘었다면 timeout 을 상한 값으로 clamp - q->timeout 타이머는 최대 5초 뒤에는 반드시 한 번 더 깨어나게 됨 */

	return timeout;	/* [한국어] clamp 적용된(또는 원래 그대로인) 값을 반환 - 호출자인 blk_add_timer 가 이 값을 mod_timer() 인자로 사용 */
}

/**
 * blk_add_timer - Start timeout timer for a single request
 * @req:	request that is about to start running.
 *
 * Notes:
 *    Each request has its own timer, and as it is added to the queue, we
 *    set up the timer. When the request completes, we cancel the timer.
 */
/*
 * [한국어]
 * blk_add_timer - 단일 request 에 대한 타임아웃 감시를 시작(또는 재시작)한다
 *
 * @req: 곧(또는 이미) 하드웨어에서 실행되기 시작하는 request
 * @return: 없음(void)
 *
 * 이 함수는 request 가 드라이버로 발행되는 시점(blk_mq_start_request()
 * 경로)마다 호출되어, 개별 request 전용 만료 시각(req->deadline)을
 * 계산해 기록하고, 큐 전역에 단 하나뿐인 q->timeout 타이머를 필요한
 * 경우에만 앞당겨 재설정한다. "개별 request 마다 자체 타이머를
 * 갖는다"는 원문 주석("Each request has its own timer")은 실제로는
 * 커널 timer_list 를 request 수만큼 만든다는 뜻이 아니라, req->deadline
 * 이라는 논리적 만료 시각을 request 마다 따로 기록하고, 실제 하드웨어
 * 타이머(q->timeout)는 그 중 가장 이른 deadline 하나만 추적한다는
 * 최적화를 의미한다. request 완료 시에는 이 함수가 별도의 취소 동작을
 * 하지 않고, 완료 경로(blk_mq_complete_request 등)에서 request 상태가
 * 바뀌어(RQF_TIMED_OUT 이 없고 MQ_RQ_IN_FLIGHT 도 아니게 됨) 스캔
 * 대상에서 자연히 제외되는 방식으로 처리된다(block/blk-mq.c 의
 * blk_mq_req_expired 참고).
 * 실행 컨텍스트: request 제출/재시도 경로의 호출자 컨텍스트를 그대로
 * 이어받음. blk_mq_rq_timed_out() 이 BLK_EH_RESET_TIMER 를 받아 이
 * 함수를 재호출하는 경우에는 kblockd 워크큐 컨텍스트에서 실행된다.
 * 호출자: blk_mq_start_request()(최초 발행 시), blk_mq_rq_timed_out()
 * (timeout 후 재시도를 위해 타이머 재무장 시, block/blk-mq.c).
 * 피호출자: blk_rq_timeout(), blk_round_jiffies(), timer_pending(),
 * time_before(), mod_timer().
 *
 * 호출 체인:
 *   blk_mq_start_request → [blk_add_timer] → mod_timer(&q->timeout)
 *   (재무장 시) blk_mq_rq_timed_out → [blk_add_timer]
 */
void blk_add_timer(struct request *req)
{
	struct request_queue *q = req->q;	/* [한국어] request 가 속한 request_queue 포인터 - 아래에서 큐 전역 timeout 필드(q->rq_timeout, q->timeout)에 접근하기 위해 캐시 */
	unsigned long expiry;	/* [한국어] 이 request 의 만료 시각(jiffies)을 담을 지역 변수 - 아래에서 두 가지 용도(req->deadline 기록용, q->timeout 타이머 프로그래밍용)로 재사용됨 */

	/*
	 * Some LLDs, like scsi, peek at the timeout to prevent a
	 * command from being retried forever.
	 */
	if (!req->timeout)	/* [한국어] 이 request 에 개별 timeout 값이 설정되어 있지 않은 경우(0) - 대부분의 blk-mq 제출 경로는 미리 설정하지만, 일부 경로는 큐 기본값에 의존 */
		req->timeout = q->rq_timeout;	/* [한국어] 큐의 기본 timeout(q->rq_timeout, 드라이버가 blk_mq_init 시 설정)을 이 request 에 복사 - SCSI 등 LLD 가 req->timeout 을 직접 참조해 무한 재시도를 막는 데 쓰기도 함(원문 주석) */

	req->rq_flags &= ~RQF_TIMED_OUT;	/* [한국어] 이전에 timeout 처리 이력이 있었다면(재시도/재큐잉 케이스) RQF_TIMED_OUT 플래그를 지워, blk_mq_req_expired() 가 이 request 를 다시 정상적으로 스캔 대상에 포함시키도록 초기화 */

	expiry = jiffies + req->timeout;	/* [한국어] 현재 시각에 이 request 전용 timeout 을 더해 "진짜" 만료 시각 계산 - 이 값이 이 request 에 대한 최종 판정 기준이 됨 */
	WRITE_ONCE(req->deadline, expiry);	/* [한국어] req->deadline 을 원자적으로 발행 - block/blk-mq.c 의 blk_mq_req_expired() 가 READ_ONCE 로 읽으므로, WRITE_ONCE 로 컴파일러의 재배치/분할 저장을 막아 다른 CPU 에서 항상 온전한 값을 보게 함(torn read 방지) */

	/*
	 * If the timer isn't already pending or this timeout is earlier
	 * than an existing one, modify the timer. Round up to next nearest
	 * second.
	 */
	expiry = blk_rq_timeout(blk_round_jiffies(expiry));	/* [한국어] req->deadline 이 아니라 "q->timeout 에 실제로 프로그래밍할 값"을 별도로 재계산 - HZ 근사 slack 을 더하고(blk_round_jiffies) 5초 상한을 적용(blk_rq_timeout)해 expiry 변수를 덮어씀 */

	if (!timer_pending(&q->timeout) ||	/* [한국어] q->timeout 타이머가 아직 활성화(armed)되어 있지 않다면 - 이번이 첫 request 이거나 이전 타이머가 이미 발동/취소된 경우, 무조건 새로 세팅해야 함 */
	    time_before(expiry, q->timeout.expires)) {	/* [한국어] 또는 타이머는 이미 동작 중이지만, 이번 request 의 만료가 현재 예약된 만료(q->timeout.expires)보다 더 이르다면 - 더 급한 request 가 생겼으므로 타이머를 앞당겨야 함 */
		unsigned long diff = q->timeout.expires - expiry;	/* [한국어] 기존 예약 만료와 새 expiry 의 차이(jiffies 단위) - 타이머가 pending 이 아닌 경우 q->timeout.expires 는 과거의 낡은 값일 수 있지만, 그 경우 아래 조건이 첫 항(!timer_pending)에서 이미 단락평가로 참이 되므로 diff 값 자체는 사용되지 않아 무해함 */

		/*
		 * Due to added timer slack to group timers, the timer
		 * will often be a little in front of what we asked for.
		 * So apply some tolerance here too, otherwise we keep
		 * modifying the timer because expires for value X
		 * will be X + something.
		 */
		if (!timer_pending(&q->timeout) || (diff >= HZ / 2))	/* [한국어] 타이머가 아예 꺼져 있거나, 기존 예약과 새 expiry 의 차이가 HZ/2(약 0.5초) 이상일 때만 실제로 재프로그래밍 - blk_round_jiffies 의 slack 때문에 매번 근소하게 다른 expiry 가 계산되어 불필요하게 mod_timer 를 반복 호출하는 것을 방지 */
			mod_timer(&q->timeout, expiry);	/* [한국어] q->timeout 커널 타이머를 새 expiry 시각으로 재프로그래밍 - 만료되면 blk_rq_timed_out_timer(block/blk-core.c) 가 softirq 컨텍스트에서 실행되어 q->timeout_work 를 kblockd 에 예약함 */
	}

}
