// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Facebook
 */

/*
 * [한국어 설명] blk-mq debugfs 등록과 show/seq_ops 콜백 구현부 (block/blk-mq-debugfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block/blk-mq-debugfs.h가 선언한 blk_mq_debugfs_register*() 계열
 * 등록/해제 함수와, 개별 debugfs 파일이 read(2)될 때 실제로 텍스트를 만들어
 * 내는 show/seq_ops 콜백들의 "구현부"이다. request_queue, blk_mq_hw_ctx(NVMe
 * SQ(Submission Queue)에 대응하는 하드웨어 디스패치 큐), blk_mq_ctx(per-CPU
 * 소프트웨어 큐), blk_mq_tags(tag/CID 비트맵), rq_qos(WBT 등 스로틀 플러그인)
 * 의 런타임 상태를 seq_printf() 등을 통해 사람이 읽을 수 있는 문자열로
 * 변환해 /sys/kernel/debug/block/<disk>/ 트리에 노출한다. 커널 코드를 다시
 * 빌드하지 않고도 NVMe I/O가 SQ/CQ(Completion Queue)의 어느 단계에서
 * 지연되는지, 어떤 tag(CID, Command Identifier)가 in-flight 상태인지, 어떤
 * debugfs 제어 파일에 값을 써서 강제로 디스패치를 재개시킬 수 있는지를 이
 * 파일이 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 정상 I/O 경로는 submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_tag()
 * -> blk_mq_get_driver_tag() -> hctx->dispatch 삽입 -> mq_ops->queue_rq()
 * (NVMe라면 nvme_queue_rq() -> nvme_sq_copy_cmd/nvme_write_sq_db() 로 SQ doorbell 갱신)로
 * 흐르지만, 이 파일의 함수들은 그 경로에 직접 관여하지 않고 옆에서 상태를
 * "관찰"만 하는 별도의 얕은 경로다. 호출 체인은 두 갈래로 나뉜다.
 *   (1) 등록/해제 체인: 디스크 등록(block/blk-sysfs.c의
 *       blk_register_queue()), 하드웨어 큐 재구성(block/blk-mq.c), 스케줄러
 *       교체(block/blk-mq-sched.c의 elevator switch), rq-qos 초기화
 *       (block/blk-wbt.c의 wbt_init())가 각각 이 파일의
 *       blk_mq_debugfs_register*()/unregister*()를 호출해 debugfs 트리를
 *       만들고 없앤다. 이 체인은 드라이버 probe/재구성 스레드 컨텍스트에서
 *       동기적으로 실행된다.
 *   (2) 열람/제어 체인: 사용자가 cat/echo로 debugfs 파일을 열면 VFS ->
 *       blk_mq_debugfs_fops(이 파일의 공용 file_operations) ->
 *       blk_mq_debugfs_open()/show()/write() -> attr->show 또는
 *       attr->seq_ops가 가리키는 개별 콜백(queue_state_show,
 *       hctx_tags_show, blk_mq_debugfs_rq_show 등)이 실행된다. 이 체인은
 *       debugfs 파일을 읽거나 쓰는 유저 프로세스의 syscall 컨텍스트에서
 *       실행되며, 인터럽트/소프트IRQ 컨텍스트에서는 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈: block/blk.h, block/blk-mq.h(struct request_queue/
 * blk_mq_hw_ctx/blk_mq_ctx/blk_mq_tags의 실제 필드 정의),
 * block/blk-mq-debugfs.h(struct blk_mq_debugfs_attr 타입과 이 파일이
 * 구현하는 함수들의 프로토타입 - 라인바이라인 주석 완료), block/blk-mq-sched.h
 * (elevator_type의 queue_debugfs_attrs/hctx_debugfs_attrs), block/blk-rq-qos.h
 * (struct rq_qos/rq_qos_id). 이 파일에 의존하는 모듈: block/blk-sysfs.c(디스크
 * 등록 시 blk_mq_debugfs_register() 호출), block/blk-mq.c(하드웨어 큐 개수
 * 재조정 시 *_hctxs() 호출), block/blk-mq-sched.c(elevator switch 시
 * *_sched*() 호출), block/blk-wbt.c(WBT 초기화/파라미터 변경 시 *_rq_qos()
 * 호출), block/mq-deadline.c와 block/kyber-iosched.c(자신의 debugfs seq_ops
 * 에서 이 파일이 EXPORT_SYMBOL_GPL로 공개하는 blk_mq_debugfs_rq_show()/
 * __blk_mq_debugfs_rq_show()를 재사용), block/blk-zoned.c(이 파일이 attr
 * 테이블에서 참조하는 queue_zone_wplugs_show()의 실제 구현 제공).
 * 데이터 흐름: request_queue/blk_mq_hw_ctx/blk_mq_ctx/blk_mq_tags/rq_qos가
 * 보유한 실시간 필드(queue_flags, hctx->state/flags, tags 비트맵, dispatch
 * 리스트, rq_lists[], rq_qos 체인) -> 이 파일의 show/seq_ops 콜백이 읽어
 * seq_printf()/seq_puts()로 문자열화 -> debugfs VFS 계층 -> 사용자 공간
 * read(2) 결과. "state" 파일만 예외적으로 사용자 -> 커널 방향으로
 * run/start/kick 문자열을 받아 queue_state_write()가 해석한다.
 * 공유 핵심 자료구조: struct blk_mq_debugfs_attr(파일 1개를 기술하는 테이블
 * 엔트리, 정의는 헤더), struct show_busy_params(busy 순회 콜백에 seq_file과
 * hctx를 함께 넘기기 위한 이 파일 내부 전용 구조체).
 *
 * === 주요 함수/구조체 요약 ===
 * - blk_flags_show(): unsigned long 비트마스크를 "NAME1|NAME2|3" 형태의
 *   사람이 읽는 문자열로 바꾸는 공용 헬퍼. queue_flags/hctx->state/
 *   hctx->flags/cmd_flags/rq_flags 출력에 모두 재사용된다.
 * - __blk_mq_debugfs_rq_show()/blk_mq_debugfs_rq_show(): 단일 request의
 *   op/cmd_flags/rq_flags/state/tag(CID)/internal_tag를 한 줄로 출력하는
 *   공용 헬퍼와, 리스트 순회형 seq_ops가 공유하는 .show 진입점.
 * - hctx_busy_show()/hctx_show_busy_rq(): tagset 전체를
 *   blk_mq_tagset_busy_iter()로 순회하며 특정 hctx에 속한 busy(in-flight)
 *   request만 골라 출력한다.
 * - blk_mq_debugfs_open()/show()/write()/release()와
 *   blk_mq_debugfs_fops: 모든 debugfs 파일이 공유하는 공용 file_operations
 *   4종 세트. .seq_ops가 있으면 seq_open(), 없으면 single_open() 경로로
 *   분기한다.
 * - blk_mq_debugfs_register()/register_hctx()/register_hctxs()/
 *   register_sched()/register_sched_hctx()/register_rq_qos()와 그 짝인
 *   unregister 계열: request_queue -> hctx -> ctx 순으로 debugfs 디렉터리
 *   트리를 생성/해제한다.
 * - struct show_busy_params: .m(출력 대상 seq_file), .hctx(필터링 기준이
 *   되는 하드웨어 큐) 두 필드로 hctx_show_busy_rq() 콜백에 문맥을 전달한다.
 */

#include <linux/kernel.h> /* [한국어] 기본 커널 타입/매크로(ARRAY_SIZE, BIT 등) 제공 - 플래그 이름 배열 처리에 필요 */
#include <linux/blkdev.h> /* [한국어] struct request_queue, blk_queue_dying() 등 블록 계층 핵심 타입/헬퍼 선언 */
#include <linux/build_bug.h> /* [한국어] BUILD_BUG_ON() 제공 - 플래그 이름 배열 크기와 실제 비트 수 불일치를 컴파일 타임에 검출 */
#include <linux/debugfs.h> /* [한국어] debugfs_create_dir/debugfs_create_file_aux/debugfs_get_aux 등 debugfs 코어 API 선언 */

#include "blk.h" /* [한국어] 블록 계층 내부 전용 선언(request_queue 내부 필드 접근 등) */
#include "blk-mq.h" /* [한국어] struct blk_mq_hw_ctx/blk_mq_ctx/blk_mq_tags 등 blk-mq 핵심 자료구조의 완전한 정의 - hctx->tags/dispatch 등 필드 접근에 필요 */
#include "blk-mq-debugfs.h" /* [한국어] 이 파일이 구현하는 함수 프로토타입과 struct blk_mq_debugfs_attr 선언(라인바이라인 주석 완료) */
#include "blk-mq-sched.h" /* [한국어] struct elevator_type의 queue_debugfs_attrs/hctx_debugfs_attrs 필드 참조용 */
#include "blk-rq-qos.h" /* [한국어] struct rq_qos/rq_qos_id, rq_qos 체인 순회(rqos->next) 관련 선언 */

/*
 * [한국어]
 * queue_poll_stat_show - "poll_stat" debugfs 파일의 .show 콜백(현재는 빈 구현).
 *
 * @data: debugfs 등록 시 넘겨진 aux 데이터(blk_mq_debugfs_queue_attrs[]의
 *        경우 request_queue *). 이 구현은 data를 전혀 사용하지 않는다.
 * @m: 출력 대상 seq_file. 이 구현은 아무 것도 쓰지 않는다.
 * @return: 항상 0(성공).
 *
 * 폴링(polled) I/O 통계를 노출하려는 의도로 blk_mq_debugfs_queue_attrs[]에
 * "poll_stat" 항목이 남아 있지만, 실제 통계 수집 로직이 빠진 채로 남아
 * 있는 자리 지킴이(placeholder)다. 즉 이 함수는 "poll_stat" 파일을 열어도
 * 항상 빈 내용만 보이게 만든다. 다른 show 콜백과 동일한 시그니처
 * int (*)(void *, struct seq_file *)를 유지해야 테이블
 * (blk_mq_debugfs_queue_attrs[])의 타입 일관성이 깨지지 않는다.
 * 실행 컨텍스트: "poll_stat" debugfs 파일을 read(2)하는 유저 프로세스의
 * syscall 컨텍스트에서 blk_mq_debugfs_show()를 통해 호출된다.
 * 호출자: blk_mq_debugfs_show()(attr->show를 통한 간접 호출).
 * 피호출자: 없음.
 * 에러 경로: 없음(항상 성공 고정).
 *
 * 호출 체인:
 *   read(2) -> seq_read() -> blk_mq_debugfs_show() -> [queue_poll_stat_show()]
 */
static int queue_poll_stat_show(void *data, struct seq_file *m)
{
	return 0; /* [한국어] 항상 성공 반환 - 이 debugfs 파일은 아직 아무 통계도 출력하지 않는 빈 구현 */
}

/*
 * [한국어]
 * queue_requeue_list_start - "requeue_list" debugfs 파일의 seq_ops .start 콜백.
 *   q->requeue_lock을 획득하고 q->requeue_list의 순회 위치를 반환한다.
 *
 * @m: 이 debugfs 파일의 seq_file. m->private에는 blk_mq_debugfs_open()이
 *     저장해 둔 대상 request_queue 포인터가 들어 있다.
 * @pos: 순회 재개 위치(오프셋). 첫 호출 시 *pos=0이며, seq_file core가
 *       read(2) 재호출 시 이전 위치를 복원하려고 넘긴다.
 * @return: seq_list_start()가 반환한 리스트 노드 포인터. 리스트가 비어
 *          있거나 pos가 범위를 벗어나면 NULL(순회 종료 의미).
 *
 * requeue_list는 blk_mq_requeue_request()로 인해 다시 디스패치를 기다리는
 * request들의 리스트로, NVMe에서는 타임아웃/abort로 되돌아와 아직 SQ에
 * 재삽입되지 않은 명령들이 여기 쌓인다. seq_file 리스트 순회 규약상 .start
 * 콜백이 락을 잡고 첫 노드를 돌려주면, .show가 그 노드를 출력하고 .next가
 * 다음 노드로 진행하다가 리스트 끝에서 .stop이 락을 놓는 흐름이다. 이
 * 함수는 그 흐름의 시작점으로, spin_lock_irq()를 쓰는 이유는 requeue_list가
 * 인터럽트 컨텍스트(NVMe 완료 인터럽트 -> blk_mq_requeue_request())에서도
 * 조작될 수 있기 때문이다.
 * 실행 컨텍스트: "requeue_list" 파일을 read(2)하는 유저 프로세스의 syscall
 * 컨텍스트. __acquires 애노테이션은 sparse/lockdep에 "이 함수를 나가면
 * requeue_lock을 쥔 채"임을 알려, 짝인 queue_requeue_list_stop()의
 * __releases와 페어링된다.
 * 호출자: seq_file core(seq_read()가 파일을 열 때/재개할 때).
 * 피호출자: spin_lock_irq(), seq_list_start().
 * 에러 경로: 없음(리스트가 비었으면 NULL을 그대로 반환해 즉시 EOF 처리).
 *
 * 호출 체인:
 *   seq_read() -> [queue_requeue_list_start()] -> seq_list_start()
 */
static void *queue_requeue_list_start(struct seq_file *m, loff_t *pos)
	__acquires(&q->requeue_lock)
{
	struct request_queue *q = m->private; /* [한국어] blk_mq_debugfs_open()이 m->private에 저장해 둔 대상 request_queue */

	spin_lock_irq(&q->requeue_lock); /* [한국어] requeue_list 보호 락 획득 - NVMe 완료 인터럽트에서도 이 리스트를 건드리므로 irq 버전 사용 */
	return seq_list_start(&q->requeue_list, *pos); /* [한국어] pos번째 노드부터 순회 시작; 리스트가 비어 있으면 NULL 반환 */
}

/*
 * [한국어]
 * queue_requeue_list_next - "requeue_list" seq_ops .next 콜백. 다음 리스트
 *   노드로 순회 위치를 진행시킨다.
 *
 * @m: seq_file. m->private에서 request_queue를 복원한다.
 * @v: 현재 순회 위치(직전 .start 또는 .next가 반환한 노드).
 * @pos: 갱신할 순회 오프셋. seq_list_next()가 내부에서 증가시킨다.
 * @return: 다음 리스트 노드, 마지막 노드였다면 NULL(순회 종료).
 *
 * .start가 잡아 둔 q->requeue_lock을 계속 쥔 상태로 호출되므로 이 함수
 * 자체는 별도로 락을 잡지 않는다. NVMe 디버깅 관점에서는 requeue_list의
 * 다음 항목, 즉 다음으로 재시도될 명령으로 이동하는 것에 대응한다.
 * 실행 컨텍스트: read(2) 도중 seq_file core가 반복 호출.
 * 호출자: seq_file core.
 * 피호출자: seq_list_next().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   seq_read() -> [queue_requeue_list_next()] -> seq_list_next()
 */
static void *queue_requeue_list_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct request_queue *q = m->private; /* [한국어] requeue_list가 속한 request_queue */

	return seq_list_next(v, &q->requeue_list, pos); /* [한국어] v 다음 리스트 노드 반환, pos 증가 */
}

/*
 * [한국어]
 * queue_requeue_list_stop - "requeue_list" seq_ops .stop 콜백. .start에서
 *   잡은 q->requeue_lock을 해제한다.
 *
 * @m: seq_file. m->private에서 request_queue를 복원한다.
 * @v: 마지막으로 순회한 위치(사용하지 않음, 콜백 시그니처 규약상 존재).
 * @return: 없음(void).
 *
 * seq_file core는 파일을 닫거나 한 번의 read(2) 호출이 끝날 때마다 반드시
 * .stop을 호출하도록 보장하므로, .start/.stop 쌍이 락 획득/해제의 임계
 * 구간을 이룬다. 이 락이 풀려야 blk_mq_requeue_work() 등이 requeue_list를
 * 다시 조작(재삽입/재시도)할 수 있다.
 * 실행 컨텍스트: read(2) 처리가 끝나가는 시점의 유저 syscall 컨텍스트.
 * __releases 애노테이션은 .start의 __acquires와 정적 분석 도구(sparse)
 * 상에서 짝을 이룬다.
 * 호출자: seq_file core.
 * 피호출자: spin_unlock_irq().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   seq_read() 종료 -> [queue_requeue_list_stop()] -> spin_unlock_irq()
 */
static void queue_requeue_list_stop(struct seq_file *m, void *v)
	__releases(&q->requeue_lock)
{
	struct request_queue *q = m->private; /* [한국어] requeue_list가 속한 request_queue */

	spin_unlock_irq(&q->requeue_lock); /* [한국어] .start에서 획득한 requeue_lock 해제 - 이후 requeue_list 재조작 가능 */
}

/*
 * [한국어]
 * queue_requeue_list_seq_ops - "requeue_list" debugfs 파일이 사용하는
 *   seq_operations 4종 세트. .show는 이 파일이 별도 구현 없이 공용
 *   blk_mq_debugfs_rq_show()(아래 정의, __blk_mq_debugfs_rq_show()의
 *   래퍼)를 그대로 재사용한다.
 */
static const struct seq_operations queue_requeue_list_seq_ops = {
	.start	= queue_requeue_list_start, /* [한국어] requeue_lock 획득 + 순회 시작 */
	.next	= queue_requeue_list_next, /* [한국어] 다음 requeue 대기 request로 이동 */
	.stop	= queue_requeue_list_stop, /* [한국어] requeue_lock 해제 */
	.show	= blk_mq_debugfs_rq_show, /* [한국어] 리스트 노드 -> struct request 로 변환해 한 줄 출력(공용 헬퍼) */
};

/*
 * [한국어]
 * blk_flags_show - unsigned long 비트마스크를 "NAME1|NAME2|7" 형식의
 *   사람이 읽는 문자열로 변환해 seq_file에 기록하는 공용 헬퍼.
 *
 * @m: 출력 대상 seq_file.
 * @flags: 출력할 비트마스크 값(queue_flags, hctx->state, hctx->flags,
 *         cmd_flags, rq_flags 등 호출부마다 의미가 다르다).
 * @flag_name: 비트 인덱스 -> 이름 문자열 테이블(blk_queue_flag_name,
 *             hctx_state_name, hctx_flag_name, cmd_flag_name, rqf_name 등).
 *             인덱스에 대응하는 이름이 없으면 해당 원소는 NULL일 수 있다.
 * @flag_name_count: flag_name 배열의 원소 개수(ARRAY_SIZE로 계산해 전달).
 * @return: 항상 0(성공. seq_show류 규약과 시그니처를 맞추기 위함).
 *
 * queue_flags/hctx->state/hctx->flags/cmd_flags/rq_flags 등 이 파일 전역의
 * 모든 비트마스크 출력 로직이 이 한 함수로 통일되어 있어, 새 플래그가
 * 추가되어도 출력 형식을 일관되게 유지할 수 있다. NVMe 디버깅 시
 * "STOPPED|TAG_ACTIVE" 처럼 여러 상태가 동시에 켜져 있는 hctx(SQ)의 상태를
 * 한 줄로 파악할 수 있게 해준다. 이름 테이블에 등록되지 않은 비트(신규
 * 커널 버전과의 배열 불일치, 혹은 드라이버 전용 비트)는 숫자로 그대로
 * 출력해 정보 손실을 막는다.
 * 실행 컨텍스트: 각 show 콜백을 통해 debugfs read(2) 컨텍스트에서 호출된다.
 * 별도 락 없이 인자로 받은 값만 읽으므로 재진입 안전하다.
 * 호출자: queue_state_show(), hctx_state_show(), hctx_flags_show(),
 *   __blk_mq_debugfs_rq_show()(cmd_flags/rq_flags 출력).
 * 피호출자: seq_puts(), seq_printf().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   queue_state_show() 등 -> [blk_flags_show()] -> seq_puts()/seq_printf()
 */
static int blk_flags_show(struct seq_file *m, const unsigned long flags,
			  const char *const *flag_name, int flag_name_count)
{
	bool sep = false; /* [한국어] 두 번째 이름부터 "|" 구분자를 앞에 붙이기 위한 플래그 - 첫 항목 전에는 구분자 없음 */
	int i; /* [한국어] 0부터 flags의 비트 폭(바이트*8)까지 순회하는 비트 인덱스 */

	for (i = 0; i < sizeof(flags) * BITS_PER_BYTE; i++) { /* [한국어] unsigned long의 모든 비트 위치를 순회 (예: 64비트 시스템이면 0~63) */
		if (!(flags & BIT(i))) /* [한국어] 이 비트가 꺼져 있으면 출력할 것이 없으므로 건너뜀 */
			continue;
		if (sep) /* [한국어] 이전에 이미 이름을 하나 출력했다면 구분자 삽입 */
			seq_puts(m, "|");
		sep = true; /* [한국어] 이번 항목을 출력했으니 다음부터는 구분자 필요 */
		if (i < flag_name_count && flag_name[i]) /* [한국어] 이 비트에 대응하는 이름이 테이블 범위 안에 있고 NULL이 아니면 */
			seq_puts(m, flag_name[i]); /* [한국어] 알려진 이름(예: "STOPPED", "FUA") 그대로 출력 */
		else
			seq_printf(m, "%d", i); /* [한국어] 이름이 없는 비트 - 숫자 인덱스로 대체 출력해 정보 유실 방지 */
	}
	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * queue_pm_only_show - "pm_only" debugfs 파일의 .show 콜백.
 *   request_queue->pm_only 카운터를 정수로 출력한다.
 *
 * @data: debugfs aux 데이터로 넘어온 struct request_queue * (void* 로 소거됨).
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * q->pm_only는 전원 관리(런타임 PM) 목적의 참조 카운터로, 0보다 크면 큐가
 * "PM 전용" 모드여서 RQF_PM이 표시된 특수 request만 통과시키고 일반 I/O는
 * 지연시킨다. NVMe 컨트롤러가 런타임 서스펜드(D3cold 등 저전력 상태)로
 * 들어가려는 과정에서 이 카운터가 증가하며, 값이 0으로 돌아와야 일반 I/O
 * 재개가 가능하다. 이 파일을 반복 read하면 서스펜드/레주메 타이밍에 큐가
 * 얼마나 오래 pm_only 상태였는지 관찰할 수 있다.
 * 실행 컨텍스트: "pm_only" 파일 read(2) 컨텍스트. atomic_read()로 값을
 * 원자적으로 읽으므로 별도 락 없이도 안전하다(다른 CPU가 동시에
 * atomic_inc/dec 하더라도 읽는 시점의 유효한 스냅샷을 얻는다).
 * 호출자: blk_mq_debugfs_show()(attr->show를 통한 간접 호출).
 * 피호출자: atomic_read(), seq_printf().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [queue_pm_only_show()] -> atomic_read()
 */
static int queue_pm_only_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data; /* [한국어] debugfs aux로 전달된 request_queue 포인터 복원 */

	seq_printf(m, "%d\n", atomic_read(&q->pm_only)); /* [한국어] pm_only 참조 카운터를 원자적으로 읽어 그대로 출력 - NVMe 런타임 PM 진행 상태 지표 */
	return 0; /* [한국어] 항상 성공 반환 */
}

#define QUEUE_FLAG_NAME(name) [QUEUE_FLAG_##name] = #name /* [한국어] QUEUE_FLAG_xxx 열거값을 배열 인덱스로, 문자열 "xxx"를 값으로 하는 지정 초기화자를 생성하는 매크로 - 비트 값과 이름을 한 줄로 묶어 오타/누락을 줄임 */
/*
 * [한국어]
 * blk_queue_flag_name - request_queue->queue_flags 각 비트의 이름 테이블.
 *   QUEUE_FLAG_##name 값을 인덱스로 사용하는 지정 초기화(designated
 *   initializer) 배열이라 순서가 바뀌어도 매핑이 깨지지 않는다.
 *
 * NVMe 관점에서 주요 비트: QUEUE_FLAG_DYING(컨트롤러 제거/치명적 오류로
 * 큐가 죽어가는 중 - 이후 I/O는 모두 실패 처리), QUEUE_FLAG_QUIESCED
 * (리셋/서스펜드 동안 디스패치가 일시 정지됨), QUEUE_FLAG_REGISTERED
 * (sysfs/debugfs 노출까지 끝난 상태), QUEUE_FLAG_QOS_ENABLED(WBT 등
 * rq-qos 플러그인이 붙어 있어 큐 depth가 소프트웨어적으로 제한될 수 있음).
 * 이 배열은 blk_flags_show()의 flag_name 인자로 queue_state_show()에서
 * 사용된다.
 */
static const char *const blk_queue_flag_name[] = {
	QUEUE_FLAG_NAME(DYING), /* [한국어] 컨트롤러 제거/치명적 오류 - 큐가 종료 중 */
	QUEUE_FLAG_NAME(NOMERGES), /* [한국어] bio 병합(merge) 비활성화 */
	QUEUE_FLAG_NAME(SAME_COMP), /* [한국어] 완료 인터럽트를 제출 CPU와 동일하게 강제 */
	QUEUE_FLAG_NAME(FAIL_IO), /* [한국어] 모든 I/O를 강제로 실패 처리(테스트/장애 시뮬레이션용) */
	QUEUE_FLAG_NAME(NOXMERGES), /* [한국어] 인접하지 않은 요청 간 교차 병합(cross merge) 비활성화 */
	QUEUE_FLAG_NAME(SAME_FORCE), /* [한국어] SAME_COMP를 항상 강제 적용 */
	QUEUE_FLAG_NAME(INIT_DONE), /* [한국어] 큐 초기화 완료 표시 */
	QUEUE_FLAG_NAME(STATS), /* [한국어] I/O 통계(blk_stat) 수집 활성 */
	QUEUE_FLAG_NAME(REGISTERED), /* [한국어] sysfs/debugfs 등록 완료 */
	QUEUE_FLAG_NAME(QUIESCED), /* [한국어] 디스패치가 일시 정지된 quiesce 상태 - 리셋/서스펜드 중 */
	QUEUE_FLAG_NAME(RQ_ALLOC_TIME), /* [한국어] request 할당 시각 기록 활성화(지연 분석용) */
	QUEUE_FLAG_NAME(HCTX_ACTIVE), /* [한국어] 하나 이상의 hctx가 활성 상태 */
	QUEUE_FLAG_NAME(SQ_SCHED), /* [한국어] 단일 큐(single-queue) 스케줄러 사용 중 */
	QUEUE_FLAG_NAME(DISABLE_WBT_DEF), /* [한국어] WBT(Writeback Throttling) 기본 활성화를 끄도록 지정됨 */
	QUEUE_FLAG_NAME(NO_ELV_SWITCH), /* [한국어] 런타임 elevator(스케줄러) 교체 금지 */
	QUEUE_FLAG_NAME(QOS_ENABLED), /* [한국어] rq-qos(WBT/latency/cost) 플러그인이 하나 이상 등록됨 */
	QUEUE_FLAG_NAME(BIO_ISSUE_TIME), /* [한국어] bio 발행 시각 기록 활성화 */
	QUEUE_FLAG_NAME(ZONED_QD1_WRITES), /* [한국어] zoned 장치에서 zone당 쓰기 큐 depth를 1로 강제(순차 쓰기 보장) */
};
#undef QUEUE_FLAG_NAME /* [한국어] 배열 정의 후 매크로를 해제해 다른 코드의 QUEUE_FLAG_NAME 재사용/충돌 방지 */

/*
 * [한국어]
 * queue_state_show - "state" debugfs 파일의 .show 콜백. request_queue->
 *   queue_flags 비트마스크를 이름 문자열로 출력한다.
 *
 * @data: debugfs aux로 전달된 struct request_queue *.
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * 큐 전체의 현재 상태(죽어가는 중인지, quiesce 상태인지, QoS가 걸려
 * 있는지 등)를 한눈에 보기 위한 파일이다. BUILD_BUG_ON은 blk_queue_flag_name
 * 배열 크기와 실제 QUEUE_FLAG_MAX(비트 개수)가 일치하는지 컴파일 타임에
 * 검증해, 커널 버전이 바뀌며 새 QUEUE_FLAG_*가 추가됐는데 이 배열을 갱신하지
 * 않는 실수를 방지한다. 이 파일은 같은 이름의 "state" 파일이 .write도
 * 지원하므로(queue_state_write, 아래 정의) 읽기/쓰기 양방향 제어 파일이다.
 * 실행 컨텍스트: debugfs read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: BUILD_BUG_ON(컴파일 타임 검증), blk_flags_show(), seq_puts().
 * 에러 경로: 없음(런타임 에러 없음, 불일치는 컴파일 실패로 드러남).
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [queue_state_show()] -> blk_flags_show()
 */
static int queue_state_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data; /* [한국어] debugfs aux로 전달된 request_queue 포인터 복원 */

	BUILD_BUG_ON(ARRAY_SIZE(blk_queue_flag_name) != QUEUE_FLAG_MAX); /* [한국어] 컴파일 타임 검증 - 이름 배열 크기와 실제 플래그 비트 수(QUEUE_FLAG_MAX)가 다르면 빌드 실패시켜 누락을 조기에 발견 */
	blk_flags_show(m, q->queue_flags, blk_queue_flag_name, /* [한국어] queue_flags 비트마스크를 이름 문자열로 변환해 출력 */
		       ARRAY_SIZE(blk_queue_flag_name));
	seq_puts(m, "\n"); /* [한국어] 줄바꿈으로 출력 마무리 */
	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * queue_state_write - "state" debugfs 파일의 .write 콜백. 사용자가
 *   "run"/"start"/"kick" 문자열을 써서 큐 디스패치를 강제로 제어한다.
 *
 * @data: debugfs aux로 전달된 struct request_queue *.
 * @buf: 사용자 공간 버퍼(__user 포인터) - echo run > .../state 의 "run" 등.
 * @count: buf에 담긴 바이트 수.
 * @ppos: 파일 오프셋(사용하지 않음, write 콜백 시그니처 규약상 존재).
 * @return: 성공 시 count(쓴 바이트 수 전체를 소비했다고 보고), 실패 시
 *          -ENOENT(큐가 dying 상태), -EFAULT(사용자 버퍼 접근 실패),
 *          -EINVAL(알 수 없는 명령).
 *
 * 이 디버깅 인터페이스는 정상적으로는 타이머/워크큐가 자동으로 트리거하는
 * 디스패치 재개 동작을 관리자가 수동으로 강제 실행할 수 있게 한다. "run"은
 * 모든 hctx(SQ)의 디스패치를 다시 실행시켜(NVMe라면 nvme_queue_rq() ->
 * doorbell 갱신까지 이어질 수 있음) 멈춰 있던 I/O를 흘려보낸다. "start"는
 * BLK_MQ_S_STOPPED로 명시적으로 정지된 hctx만 재개시킨다. "kick"은
 * requeue_list에 쌓인, 재시도를 기다리는 request들을 강제로 다시
 * 디스패치시킨다. 큐가 이미 dying(제거 진행 중) 상태면 use-after-free를
 * 막기 위해 어떤 명령도 거부한다.
 * 실행 컨텍스트: "state" 파일에 write(2)하는 유저 프로세스의 syscall
 * 컨텍스트. copy_from_user()가 페이지 폴트를 유발할 수 있으므로 이
 * 함수는 sleep 가능한 컨텍스트에서만 호출되어야 한다(debugfs write 경로는
 * 항상 그렇다).
 * 호출자: blk_mq_debugfs_write()(attr->write를 통한 간접 호출).
 * 피호출자: blk_queue_dying(), copy_from_user(), strstrip(), strcmp(),
 *   blk_mq_run_hw_queues(), blk_mq_start_stopped_hw_queues(),
 *   blk_mq_kick_requeue_list().
 * 에러 경로: 길이 초과/알 수 없는 명령은 공용 inval 레이블로 점프해 사용법
 *   메시지를 남기고 -EINVAL 반환. 사용자 버퍼 접근 실패는 -EFAULT.
 *
 * 호출 체인:
 *   write(2) -> blk_mq_debugfs_write() -> [queue_state_write()]
 *     -> blk_mq_run_hw_queues() / blk_mq_start_stopped_hw_queues() /
 *        blk_mq_kick_requeue_list()
 */
static ssize_t queue_state_write(void *data, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct request_queue *q = data; /* [한국어] debugfs aux로 전달된 대상 request_queue */
	char opbuf[16] = { }, *op; /* [한국어] "run"/"start"/"kick" 명령 문자열을 담을 스택 버퍼(0으로 초기화, NUL 종료 보장) + strstrip() 결과 포인터 */

	/*
	 * The "state" attribute is removed when the queue is removed.  Don't
	 * allow setting the state on a dying queue to avoid a use-after-free.
	 */
	if (blk_queue_dying(q)) /* [한국어] 큐가 제거 진행 중(dying)이면 - 이 시점 이후 관련 자료구조가 해제될 수 있으므로 */
		return -ENOENT; /* [한국어] 쓰기 자체를 거부해 use-after-free 방지 */

	if (count >= sizeof(opbuf)) { /* [한국어] 사용자 입력이 16바이트 버퍼보다 크거나 같으면 - 정상 명령("run" 등)보다 훨씬 길므로 잘못된 입력 */
		pr_err("%s: operation too long\n", __func__); /* [한국어] 커널 로그에 오류 기록 - __func__는 "queue_state_write" */
		goto inval; /* [한국어] 공용 에러 처리 레이블로 점프 */
	}

	if (copy_from_user(opbuf, buf, count)) /* [한국어] 사용자 공간 버퍼(buf)에서 커널 스택 버퍼(opbuf)로 count바이트 복사 - 페이지 폴트 등으로 실패 가능 */
		return -EFAULT; /* [한국어] 복사 실패 시 잘못된 주소를 가리킨 것이므로 즉시 오류 반환 */
	op = strstrip(opbuf); /* [한국어] 앞뒤 공백/개행 제거("run\n" -> "run") - echo 명령이 붙이는 개행 문자 대응 */
	if (strcmp(op, "run") == 0) { /* [한국어] "run" 명령이면 모든 hctx의 디스패치를 강제로 재실행 */
		blk_mq_run_hw_queues(q, true); /* [한국어] 큐의 모든 hctx(SQ)에 대해 디스패치 재시도 - async=true로 워크큐를 통해 비동기 실행 */
	} else if (strcmp(op, "start") == 0) { /* [한국어] "start" 명령이면 BLK_MQ_S_STOPPED로 정지된 hctx만 재개 */
		blk_mq_start_stopped_hw_queues(q, true); /* [한국어] 정지 상태였던 hctx만 골라 재개 - async=true */
	} else if (strcmp(op, "kick") == 0) { /* [한국어] "kick" 명령이면 requeue_list에 대기 중인 request를 강제로 재시도 */
		blk_mq_kick_requeue_list(q); /* [한국어] requeue_list에 쌓인 request들을 다시 디스패치 대상으로 편입 */
	} else { /* [한국어] run/start/kick 어느 것도 아닌 알 수 없는 명령 */
		pr_err("%s: unsupported operation '%s'\n", __func__, op); /* [한국어] 어떤 명령이 잘못됐는지 로그로 남김 */
inval: /* [한국어] 길이 초과 또는 알 수 없는 명령의 공용 에러 처리 지점 */
		pr_err("%s: use 'run', 'start' or 'kick'\n", __func__); /* [한국어] 올바른 사용법 안내 로그 */
		return -EINVAL; /* [한국어] 잘못된 인자 오류 반환 */
	}
	return count; /* [한국어] 성공 - 사용자가 쓴 바이트 수 전체를 소비했다고 보고 */
}

/*
 * [한국어]
 * blk_mq_debugfs_queue_attrs - request_queue(디스크) 단위 debugfs 파일
 *   테이블. /sys/kernel/debug/block/<disk>/ 바로 아래에 이 배열의 각
 *   name이 파일로 생성된다(blk_mq_debugfs_register()가
 *   debugfs_create_files()로 순회). name이 NULL인 마지막 원소가 순회 종료
 *   sentinel이다.
 * 각 행: {name, mode, show, write, seq_ops} 순서의 위치 초기화 또는
 * 일부는 .seq_ops 지정 초기화를 사용(구조체 정의는 blk-mq-debugfs.h 참고).
 * NVMe 디버깅 관점: poll_stat(현재 빈 값), requeue_list(SQ 재삽입 대기
 * 목록), pm_only(런타임 PM 카운터), state(전체 큐 상태 조회 + run/start/
 * kick 제어), zone_wplugs(ZNS zone별 write plug 상태)를 각각 노출한다.
 */
static const struct blk_mq_debugfs_attr blk_mq_debugfs_queue_attrs[] = {
	{ "poll_stat", 0400, queue_poll_stat_show }, /* [한국어] 폴링 통계 - 현재 빈 구현, root 읽기 전용(0400) */
	{ "requeue_list", 0400, .seq_ops = &queue_requeue_list_seq_ops }, /* [한국어] requeue_list 순회 출력 - .show 대신 seq_ops 지정(리스트형) */
	{ "pm_only", 0600, queue_pm_only_show, NULL }, /* [한국어] pm_only 카운터 조회 전용, .write는 NULL(쓰기 불가), mode 0600 */
	{ "state", 0600, queue_state_show, queue_state_write }, /* [한국어] 큐 상태 조회 + run/start/kick 제어, 읽기/쓰기 모두 지원 */
	{ "zone_wplugs", 0400, queue_zone_wplugs_show, NULL }, /* [한국어] ZNS zone write plug 상태 조회 전용(block/blk-zoned.c 구현) */
	{ }, /* [한국어] name=NULL sentinel - debugfs_create_files()의 for (; attr->name; attr++) 순회 종료 조건 */
};

#define HCTX_STATE_NAME(name) [BLK_MQ_S_##name] = #name /* [한국어] BLK_MQ_S_xxx 비트 인덱스를 배열 인덱스로, "xxx"를 이름으로 하는 지정 초기화자 생성 매크로 */
/*
 * [한국어]
 * hctx_state_name - blk_mq_hw_ctx->state 비트 이름 테이블(atomic 플래그
 *   BLK_MQ_S_* 대응). "hctx/state" debugfs 파일에서 blk_flags_show()의
 *   flag_name 인자로 쓰인다.
 *
 * NVMe 관점: BLK_MQ_S_STOPPED는 이 hctx(SQ)가 일시 정지되어 새 명령을
 * doorbell로 내보내지 않는 상태, BLK_MQ_S_TAG_ACTIVE는 이 hctx에서 tag가
 * 활발히 사용 중이라는 표시(공유 tagset의 active_queues 계산에 관여),
 * BLK_MQ_S_SCHED_RESTART는 tag 고갈 등으로 막혔던 디스패치가 재시작
 * 대기 중임을, BLK_MQ_S_INACTIVE는 CPU 오프라인 등으로 이 hctx가 더 이상
 * 사용되지 않는 상태를 의미한다.
 */
static const char *const hctx_state_name[] = {
	HCTX_STATE_NAME(STOPPED), /* [한국어] hctx 디스패치 일시 정지 - 새 명령을 SQ로 내보내지 않음 */
	HCTX_STATE_NAME(TAG_ACTIVE), /* [한국어] 이 hctx에서 tag(CID)가 활발히 사용 중 */
	HCTX_STATE_NAME(SCHED_RESTART), /* [한국어] 스케줄러 디스패치 재시작이 대기/필요한 상태 */
	HCTX_STATE_NAME(INACTIVE), /* [한국어] hctx가 비활성(예: 연결된 CPU가 오프라인) 상태 */
};
#undef HCTX_STATE_NAME /* [한국어] 배열 정의 후 매크로 해제 - 재사용/충돌 방지 */

/*
 * [한국어]
 * hctx_state_show - "hctx<N>/state" debugfs 파일의 .show 콜백.
 *   blk_mq_hw_ctx->state 비트마스크를 이름 문자열로 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * 이 hctx(NVMe SQ에 대응)가 현재 정지(STOPPED)해 있는지, tag가 활발히
 * 쓰이는지(TAG_ACTIVE), 재시작이 필요한지(SCHED_RESTART)를 한눈에 보여줘
 * "이 SQ가 왜 명령을 내보내지 않는가"를 디버깅할 때 첫 확인 지점이 된다.
 * BUILD_BUG_ON은 hctx_state_name 배열 크기와 BLK_MQ_S_MAX(실제 상태 비트
 * 개수)가 일치하는지 컴파일 타임에 검증한다.
 * 실행 컨텍스트: debugfs read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: BUILD_BUG_ON, blk_flags_show(), seq_puts().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_state_show()] -> blk_flags_show()
 */
static int hctx_state_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx(SQ) */

	BUILD_BUG_ON(ARRAY_SIZE(hctx_state_name) != BLK_MQ_S_MAX); /* [한국어] 컴파일 타임 검증 - 이름 배열 크기와 실제 상태 비트 수 불일치 시 빌드 실패 */
	blk_flags_show(m, hctx->state, hctx_state_name, /* [한국어] hctx->state 비트마스크를 이름 문자열로 변환해 출력 */
		       ARRAY_SIZE(hctx_state_name));
	seq_puts(m, "\n"); /* [한국어] 줄바꿈으로 출력 마무리 */
	return 0; /* [한국어] 항상 성공 반환 */
}

#define HCTX_FLAG_NAME(name) [ilog2(BLK_MQ_F_##name)] = #name /* [한국어] BLK_MQ_F_xxx는 값 자체가 아니라 비트 위치를 나타내므로 ilog2()로 비트 인덱스를 구해 배열 인덱스로 사용 */
/*
 * [한국어]
 * hctx_flag_name - blk_mq_hw_ctx->flags(생성 시 고정되는 정적 특성
 *   BLK_MQ_F_*) 비트 이름 테이블. "hctx/flags" debugfs 파일에서 사용된다.
 *
 * NVMe 관점: BLK_MQ_F_TAG_QUEUE_SHARED는 여러 SQ가 하나의 tag(CID) 공간을
 * 공유함(멀티 네임스페이스 공유 tagset 등), BLK_MQ_F_BLOCKING은 드라이버의
 * queue_rq()가 sleep할 수 있음(대개 NVMe PCIe는 아니지만 NVMe-fabrics 계열
 * 드라이버는 해당될 수 있음), BLK_MQ_F_NO_SCHED_BY_DEFAULT는 이 하드웨어가
 * 이미 충분히 병렬적이라 mq-deadline 등 스케줄러 없이 none으로 기본
 * 설정됨을 의미한다.
 */
static const char *const hctx_flag_name[] = {
	HCTX_FLAG_NAME(TAG_QUEUE_SHARED), /* [한국어] tag(CID) 공간을 여러 hctx/SQ가 공유 */
	HCTX_FLAG_NAME(STACKING), /* [한국어] dm 등 스태킹 드라이버 위에서 동작 */
	HCTX_FLAG_NAME(TAG_HCTX_SHARED), /* [한국어] hctx 간 tag 세트를 공유 */
	HCTX_FLAG_NAME(BLOCKING), /* [한국어] queue_rq() 콜백이 sleep 가능 */
	HCTX_FLAG_NAME(TAG_RR), /* [한국어] tag 할당을 라운드로빈 방식으로 수행 */
	HCTX_FLAG_NAME(NO_SCHED_BY_DEFAULT), /* [한국어] 기본적으로 I/O 스케줄러를 붙이지 않음(none) */
};
#undef HCTX_FLAG_NAME /* [한국어] 배열 정의 후 매크로 해제 */

/*
 * [한국어]
 * hctx_flags_show - "hctx<N>/flags" debugfs 파일의 .show 콜백.
 *   blk_mq_hw_ctx->flags 비트마스크를 이름 문자열로 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * hctx->flags는 hctx 생성 시 드라이버(mq_ops)가 고정한 정적 특성이므로
 * state(런타임 가변)와 달리 이 값은 큐 수명 동안 변하지 않는다. 이
 * 파일을 통해 특정 NVMe hctx가 TAG_QUEUE_SHARED인지(다른 네임스페이스와
 * CID 공간을 공유하는지), BLOCKING인지(드라이버가 sleep 가능한지) 등을
 * 확인할 수 있다.
 * 실행 컨텍스트: debugfs read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: BUILD_BUG_ON, blk_flags_show(), seq_puts().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_flags_show()] -> blk_flags_show()
 */
static int hctx_flags_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx(SQ) */

	BUILD_BUG_ON(ARRAY_SIZE(hctx_flag_name) != ilog2(BLK_MQ_F_MAX)); /* [한국어] 컴파일 타임 검증 - BLK_MQ_F_MAX의 비트 위치(ilog2)와 이름 배열 크기 불일치 시 빌드 실패 */

	blk_flags_show(m, hctx->flags, hctx_flag_name, /* [한국어] hctx->flags 비트마스크를 이름 문자열로 변환해 출력 */
			ARRAY_SIZE(hctx_flag_name));
	seq_puts(m, "\n"); /* [한국어] 줄바꿈으로 출력 마무리 */
	return 0; /* [한국어] 항상 성공 반환 */
}

#define CMD_FLAG_NAME(name) [__REQ_##name] = #name /* [한국어] __REQ_xxx 비트 인덱스를 배열 인덱스로 사용하는 지정 초기화자 생성 매크로 - struct request->cmd_flags용 */
/*
 * [한국어]
 * cmd_flag_name - struct request->cmd_flags(요청 특성 비트, req_op를 뺀
 *   나머지 __REQ_* 비트) 이름 테이블. __blk_mq_debugfs_rq_show()가
 *   ".cmd_flags=" 출력에 사용한다.
 *
 * NVMe 관점 주요 비트: FUA(Force Unit Access - NVMe Write 명령의 FUA
 * 비트로 매핑되어 캐시를 우회하고 매체에 직접 반영), PREFLUSH(NVMe Flush
 * 명령 삽입 필요), POLLED(완료를 인터럽트 대신 폴링으로 확인하는 NVMe
 * 폴링 큐 경로), NOMERGE(다른 요청과 병합 금지), INTEGRITY(T10 DIF/DIX
 * 등 데이터 무결성 메타데이터 동반).
 */
static const char *const cmd_flag_name[] = {
	CMD_FLAG_NAME(FAILFAST_DEV), /* [한국어] 장치 계층에서 재시도 없이 즉시 실패 처리 */
	CMD_FLAG_NAME(FAILFAST_TRANSPORT), /* [한국어] 트랜스포트(PCIe/네트워크) 계층 fast-fail */
	CMD_FLAG_NAME(FAILFAST_DRIVER), /* [한국어] 드라이버 계층 fast-fail */
	CMD_FLAG_NAME(SYNC), /* [한국어] 동기식 I/O - 완료까지 대기 의미 부여 */
	CMD_FLAG_NAME(META), /* [한국어] 메타데이터 I/O */
	CMD_FLAG_NAME(PRIO), /* [한국어] 우선순위가 지정된 I/O */
	CMD_FLAG_NAME(NOMERGE), /* [한국어] 다른 요청과 병합 금지 */
	CMD_FLAG_NAME(IDLE), /* [한국어] idle I/O - readahead류의 낮은 긴급도 */
	CMD_FLAG_NAME(INTEGRITY), /* [한국어] 데이터 무결성(T10 DIF/DIX) 메타데이터 동반 */
	CMD_FLAG_NAME(FUA), /* [한국어] Force Unit Access - 캐시 우회, NVMe Write FUA 비트 대응 */
	CMD_FLAG_NAME(PREFLUSH), /* [한국어] 이 요청 전에 캐시 flush 필요 - NVMe Flush 명령 삽입 */
	CMD_FLAG_NAME(RAHEAD), /* [한국어] readahead 요청 - 실패해도 치명적이지 않음 */
	CMD_FLAG_NAME(BACKGROUND), /* [한국어] 백그라운드 우선순위 I/O */
	CMD_FLAG_NAME(NOWAIT), /* [한국어] 리소스 부족 시 대기하지 않고 즉시 실패 반환 */
	CMD_FLAG_NAME(POLLED), /* [한국어] 완료를 인터럽트 대신 폴링으로 확인 - NVMe 폴링 큐 경로 */
	CMD_FLAG_NAME(ALLOC_CACHE), /* [한국어] 완료 후 재사용을 위해 per-task 캐시에 반환된 요청 */
	CMD_FLAG_NAME(SWAP), /* [한국어] 스왑 I/O */
	CMD_FLAG_NAME(DRV), /* [한국어] 드라이버 전용(driver-private) 플래그 */
	CMD_FLAG_NAME(FS_PRIVATE), /* [한국어] 파일시스템 전용(fs-private) 플래그 */
	CMD_FLAG_NAME(ATOMIC), /* [한국어] 원자적 쓰기(atomic write) 요청 */
	CMD_FLAG_NAME(NOUNMAP), /* [한국어] discard 시 실제 unmap(할당 해제)을 하지 않음 */
};
#undef CMD_FLAG_NAME /* [한국어] 배열 정의 후 매크로 해제 */

#define RQF_NAME(name) [__RQF_##name] = #name /* [한국어] __RQF_xxx 비트 인덱스를 배열 인덱스로 사용하는 지정 초기화자 생성 매크로 - struct request->rq_flags용 */
/*
 * [한국어]
 * rqf_name - struct request->rq_flags(블록 계층 내부 생명주기/상태 비트
 *   RQF_*) 이름 테이블. __blk_mq_debugfs_rq_show()가 ".rq_flags=" 출력에
 *   사용한다. cmd_flags가 "이 I/O에 요청된 특성"이라면 rq_flags는 "이
 *   request가 지금 blk-mq 내부에서 어떤 처리 단계/상태에 있는가"를 나타낸다.
 *
 * NVMe 관점 주요 비트: STARTED(디스패치 시작 - SQ에 삽입되어 doorbell이
 * 울릴 수 있는 상태), FAILED(컨트롤러/트랜스포트 오류로 실패 완료),
 * TIMED_OUT(타임아웃 핸들러가 개입한 상태 - NVMe 타임아웃 복구 경로),
 * SPECIAL_PAYLOAD(PRP/SGL로 매핑되지 않는 특수 payload를 가진 요청).
 */
static const char *const rqf_name[] = {
	RQF_NAME(STARTED), /* [한국어] 디스패치 시작됨 - SQ 삽입/doorbell 가능 상태로 전이 */
	RQF_NAME(FLUSH_SEQ), /* [한국어] flush 시퀀스(pre-flush/data/post-flush)의 일부 */
	RQF_NAME(MIXED_MERGE), /* [한국어] 서로 다른 특성의 요청이 병합되어 원본 특성이 섞임 */
	RQF_NAME(DONTPREP), /* [한국어] prep 단계를 다시 수행하지 않음(재시도 시 재검증 생략) */
	RQF_NAME(SCHED_TAGS), /* [한국어] 스케줄러 전용 tag 공간 사용 중 */
	RQF_NAME(USE_SCHED), /* [한국어] I/O 스케줄러가 이 요청에 관여함 */
	RQF_NAME(FAILED), /* [한국어] 컨트롤러/트랜스포트 오류로 실패 완료 처리됨 */
	RQF_NAME(QUIET), /* [한국어] 오류 발생 시 커널 로그 메시지 억제 */
	RQF_NAME(IO_STAT), /* [한국어] I/O 통계(디스크 accounting) 집계 대상 */
	RQF_NAME(PM), /* [한국어] 전원 관리(PM) 목적 요청 - pm_only 상태에서도 통과 허용 */
	RQF_NAME(HASHED), /* [한국어] 타임아웃 추적용 해시 테이블에 등록됨 */
	RQF_NAME(STATS), /* [한국어] 지연시간 통계(blk_stat) 수집 대상 */
	RQF_NAME(SPECIAL_PAYLOAD), /* [한국어] 특수 payload(PRP/SGL로 직접 매핑되지 않는 버퍼) 동반 */
	RQF_NAME(ZONE_WRITE_PLUGGING), /* [한국어] zone write plugging 대상 - ZNS 순차 쓰기 순서 보장 중 */
	RQF_NAME(TIMED_OUT), /* [한국어] 타임아웃 핸들러가 개입한 상태 */
	RQF_NAME(RESV), /* [한국어] 예약(reserved) tag를 사용하는 요청 */
};
#undef RQF_NAME /* [한국어] 배열 정의 후 매크로 해제 */

/*
 * [한국어]
 * blk_mq_rq_state_name_array - enum mq_rq_state(요청의 3단계 생명주기)를
 *   문자열로 매핑하는 배열. blk_mq_rq_state_name()이 이 배열을 감싸
 *   범위를 벗어난 접근을 방어한다.
 *
 * NVMe 관점: MQ_RQ_IDLE(아직 SQ에 제출되지 않고 tag만 보유하거나 자유
 * 상태), MQ_RQ_IN_FLIGHT(SQ에 제출되어 CQ 완료를 기다리는 중 - CID가
 * 하드웨어에 노출된 상태), MQ_RQ_COMPLETE(CQ 완료 엔트리를 받아 완료
 * 처리 중)를 나타낸다.
 */
static const char *const blk_mq_rq_state_name_array[] = {
	[MQ_RQ_IDLE]		= "idle", /* [한국어] SQ 제출 전 - tag만 보유하거나 자유 상태 */
	[MQ_RQ_IN_FLIGHT]	= "in_flight", /* [한국어] SQ 제출 완료, CQ 완료 대기 중 - CID가 하드웨어에 노출됨 */
	[MQ_RQ_COMPLETE]	= "complete", /* [한국어] CQ 완료 엔트리 수신, 완료 처리 중 */
};

/*
 * [한국어]
 * blk_mq_rq_state_name - enum mq_rq_state 값을 안전하게 문자열로 변환.
 *
 * @rq_state: blk_mq_rq_state(rq)로 얻은 현재 요청의 생명주기 상태 값.
 * @return: "idle"/"in_flight"/"complete" 중 하나, 범위를 벗어난 값이면
 *          "(?)"(WARN_ON_ONCE로 커널 로그에 경고도 함께 남김).
 *
 * mq_rq_state는 blk_mq_rq_state()가 request->state 원자 필드에서 읽어오는
 * 값으로, 이 함수가 실행되는 순간과 실제 상태 조회 시점 사이에 다른
 * CPU(예: NVMe 완료 인터럽트 핸들러)가 상태를 바꿀 수 있어 이론상 아주
 * 짧은 경쟁 구간이 존재하지만, 배열 범위를 벗어난 값은 정의되지 않은
 * enum 값이 들어온 것이므로 방어적으로 "(?)"를 반환하고 WARN_ON_ONCE로
 * 한 번만 경고해 디버깅 정보를 남긴다.
 * 실행 컨텍스트: __blk_mq_debugfs_rq_show() 호출 경로와 동일(debugfs
 * read(2) 컨텍스트).
 * 호출자: __blk_mq_debugfs_rq_show().
 * 피호출자: WARN_ON_ONCE().
 * 에러 경로: 범위 초과 시 "(?)"를 반환하되 크래시하지 않음(디버깅 파일이
 *   프로덕션 안정성에 영향을 주면 안 되므로).
 *
 * 호출 체인:
 *   __blk_mq_debugfs_rq_show() -> [blk_mq_rq_state_name()]
 */
static const char *blk_mq_rq_state_name(enum mq_rq_state rq_state)
{
	if (WARN_ON_ONCE((unsigned int)rq_state >= /* [한국어] rq_state를 unsigned로 캐스팅 후 배열 크기와 비교 - 음수/비정상 enum 값 방어 */
			 ARRAY_SIZE(blk_mq_rq_state_name_array)))
		return "(?)"; /* [한국어] 알 수 없는 상태값 - 크래시 대신 물음표 문자열 반환 */
	return blk_mq_rq_state_name_array[rq_state]; /* [한국어] 정상 범위 - idle/in_flight/complete 문자열 반환 */
}

/*
 * [한국어]
 * __blk_mq_debugfs_rq_show - 단일 struct request의 핵심 필드를 seq_file에
 *   한 줄로 출력하는 공용 헬퍼. block/blk-mq-debugfs.h에 프로토타입이
 *   선언되어 EXPORT_SYMBOL_GPL로 mq-deadline/kyber 등 다른 모듈에도 공개된다.
 *
 * @m: 출력 대상 seq_file.
 * @rq: 출력할 struct request. dispatch/busy/*_rq_list 등 이 파일의 리스트형
 *      seq_ops에서는 list_entry_rq(v)로 리스트 노드에서 복원되고,
 *      mq-deadline/kyber 자체 debugfs 파일에서는 그 내부 자료구조(fifo,
 *      rb-tree)에서 직접 뽑아 전달한다.
 * @return: 항상 0.
 *
 * 출력 형식은 "<rq 포인터> {.op=READ, .cmd_flags=FUA, .rq_flags=STARTED,
 * .state=in_flight, .tag=5, .internal_tag=-1}\n" 형태다. op은 req_op(rq)로
 * 얻은 오퍼레이션(NVMe Read/Write/Flush/Discard 등에 대응), cmd_flags/
 * rq_flags는 blk_flags_show()로 비트 이름 나열, state는
 * blk_mq_rq_state_name()으로 idle/in_flight/complete, tag는 하드웨어
 * tag(NVMe라면 SQ에 실제 삽입될 때 쓰는 CID에 대응), internal_tag는 아직
 * 하드웨어 tag를 못 받아 스케줄러가 임시로 부여한 tag(스케줄러 미사용
 * 또는 이미 하드웨어 tag를 받았으면 -1)다. mq_ops->show_rq가 있으면
 * (NVMe 드라이버는 보통 구현하지 않지만 일부 드라이버가 sector 등 추가
 * 필드를 덧붙일 수 있다) 그 결과도 같은 줄에 이어 붙인다.
 * 실행 컨텍스트: debugfs read(2) 컨텍스트에서, 호출자의 리스트 락(hctx->lock
 * 등)이 rq가 그 사이 free/재사용되지 않도록 보장한 상태에서 호출된다.
 * 이 함수 자체는 rq를 읽기만 하므로 재진입 가능하다.
 * 호출자: blk_mq_debugfs_rq_show()(아래), hctx_show_busy_rq(),
 *   block/mq-deadline.c의 자체 debugfs .show 콜백들(모듈 경계를 넘는
 *   재사용이므로 EXPORT_SYMBOL_GPL 필요).
 * 피호출자: BUILD_BUG_ON, req_op(), blk_op_str(), seq_printf()/seq_puts(),
 *   blk_flags_show(), blk_mq_rq_state_name(), mq_ops->show_rq().
 * 에러 경로: 없음(항상 0 반환).
 *
 * 호출 체인:
 *   blk_mq_debugfs_rq_show() 또는 hctx_show_busy_rq() 또는 mq-deadline
 *     자체 .show -> [__blk_mq_debugfs_rq_show()] -> seq_printf()
 */
int __blk_mq_debugfs_rq_show(struct seq_file *m, struct request *rq)
{
	const struct blk_mq_ops *const mq_ops = rq->q->mq_ops; /* [한국어] 이 request가 속한 큐의 드라이버 ops(NVMe라면 nvme_mq_ops) - show_rq 콜백 유무 확인용 */
	const enum req_op op = req_op(rq); /* [한국어] cmd_flags 하위 비트에서 추출한 오퍼레이션 코드(READ/WRITE/FLUSH/DISCARD 등) */
	const char *op_str = blk_op_str(op); /* [한국어] op을 사람이 읽는 문자열로 변환("UNKNOWN"이면 알려지지 않은 op) */

	BUILD_BUG_ON(ARRAY_SIZE(cmd_flag_name) != __REQ_NR_BITS); /* [한국어] 컴파일 타임 검증 - cmd_flag_name 배열 크기와 실제 __REQ_* 비트 총수(__REQ_NR_BITS) 불일치 시 빌드 실패 */
	BUILD_BUG_ON(ARRAY_SIZE(rqf_name) != __RQF_BITS); /* [한국어] 컴파일 타임 검증 - rqf_name 배열 크기와 실제 RQF_* 비트 총수(__RQF_BITS) 불일치 시 빌드 실패 */

	seq_printf(m, "%p {.op=", rq); /* [한국어] "<rq 포인터> {.op=" 로 출력 시작 - 포인터값은 동일 request 여러 줄 출력을 서로 구분하는 식별자 역할 */
	if (strcmp(op_str, "UNKNOWN") == 0) /* [한국어] blk_op_str이 이름을 못 찾은 경우 */
		seq_printf(m, "%u", op); /* [한국어] 이름 대신 숫자 오퍼레이션 코드 그대로 출력 */
	else
		seq_printf(m, "%s", op_str); /* [한국어] 알려진 이름(REQ_OP_READ 등) 출력 */
	seq_puts(m, ", .cmd_flags="); /* [한국어] 다음 필드(cmd_flags) 출력 준비 */
	blk_flags_show(m, (__force unsigned int)(rq->cmd_flags & ~REQ_OP_MASK), /* [한국어] cmd_flags에서 req_op 비트(REQ_OP_MASK)를 제거한 나머지 특성 비트만 이름으로 출력 - op은 이미 위에서 별도 출력했으므로 중복 방지 */
		       cmd_flag_name, ARRAY_SIZE(cmd_flag_name));
	seq_puts(m, ", .rq_flags="); /* [한국어] 다음 필드(rq_flags) 출력 준비 */
	blk_flags_show(m, (__force unsigned int)rq->rq_flags, rqf_name, /* [한국어] rq_flags(STARTED/FAILED/TIMED_OUT 등 생명주기 비트) 이름으로 출력 */
		       ARRAY_SIZE(rqf_name));
	seq_printf(m, ", .state=%s", blk_mq_rq_state_name(blk_mq_rq_state(rq))); /* [한국어] blk_mq_rq_state()로 현재 원자적 상태를 읽어 idle/in_flight/complete 문자열로 출력 */
	seq_printf(m, ", .tag=%d, .internal_tag=%d", rq->tag, /* [한국어] rq->tag(하드웨어 tag, NVMe에서는 SQ 삽입 시 CID로 쓰이는 값) 출력 */
		   rq->internal_tag); /* [한국어] rq->internal_tag(스케줄러가 부여한 임시 tag, 하드웨어 tag를 아직 못 받았으면 유효, 아니면 -1) 출력 */
	if (mq_ops->show_rq) /* [한국어] 드라이버가 추가 필드 출력을 지원하면 */
		mq_ops->show_rq(m, rq); /* [한국어] 드라이버 전용 show_rq() 호출 - 드라이버별 부가 정보(예: sector)를 같은 줄에 이어 붙임 */
	seq_puts(m, "}\n"); /* [한국어] 레코드 종료 괄호와 줄바꿈 */
	return 0; /* [한국어] 항상 성공 반환 */
}
EXPORT_SYMBOL_GPL(__blk_mq_debugfs_rq_show); /* [한국어] mq-deadline.c/kyber-iosched.c 등 다른 모듈이 자체 debugfs 파일에서 이 헬퍼를 재사용할 수 있도록 심벌 공개(GPL 전용) */

/*
 * [한국어]
 * blk_mq_debugfs_rq_show - request 리스트를 순회하는 seq_operations들이
 *   공유하는 .show 콜백. 리스트 노드를 struct request로 변환해
 *   __blk_mq_debugfs_rq_show()에 위임한다.
 *
 * @m: 출력 대상 seq_file.
 * @v: 해당 seq_ops의 .start/.next가 반환한 현재 순회 위치. 실제로는
 *     rq->queuelist(리스트 연결 노드) 포인터다.
 * @return: __blk_mq_debugfs_rq_show()의 반환값(항상 0).
 *
 * queue_requeue_list_seq_ops, hctx_dispatch_seq_ops,
 * ctx_default/read/poll_rq_list_seq_ops 등 이 파일의 모든 리스트형 seq_ops
 * .show 슬롯에 공통으로 대입되는 함수다. list_entry_rq()는
 * container_of(v, struct request, queuelist) 매크로로, 리스트 노드
 * 포인터에서 그 노드를 포함하는 struct request 전체를 복원한다.
 * 실행 컨텍스트: 각 seq_ops의 .start/.next가 리스트 보호 락(q->requeue_lock,
 * hctx->lock, ctx->lock 등)을 쥔 상태에서 seq_file core가 호출한다.
 * 호출자: seq_file core(seq_read()가 리스트의 각 노드마다 호출).
 * 피호출자: list_entry_rq(), __blk_mq_debugfs_rq_show().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   seq_read() -> [blk_mq_debugfs_rq_show()] -> list_entry_rq()
 *     -> __blk_mq_debugfs_rq_show()
 */
int blk_mq_debugfs_rq_show(struct seq_file *m, void *v)
{
	return __blk_mq_debugfs_rq_show(m, list_entry_rq(v)); /* [한국어] 리스트 노드(v)를 감싸는 struct request로 변환 후 공용 출력 헬퍼에 위임 */
}
EXPORT_SYMBOL_GPL(blk_mq_debugfs_rq_show); /* [한국어] mq-deadline.c 등이 자신의 리스트형 debugfs seq_ops .show에 이 함수를 그대로 재사용할 수 있도록 공개 */

/*
 * [한국어]
 * hctx_dispatch_start - "hctx<N>/dispatch" debugfs 파일의 seq_ops .start
 *   콜백. hctx->lock을 획득하고 hctx->dispatch 리스트 순회를 시작한다.
 *
 * @m: seq_file. m->private에 대상 blk_mq_hw_ctx가 들어 있다.
 * @pos: 순회 시작 오프셋.
 * @return: 첫 리스트 노드 포인터, 리스트가 비었으면 NULL.
 *
 * hctx->dispatch는 이 hctx(NVMe SQ)로 곧바로 내보낼 예정인 소프트웨어
 * 디스패치 큐다. mq_ops->queue_rq()가 -EBUSY 등을 반환해 즉시 디스패치에
 * 실패한 request나, 스케줄러가 배치로 모아 놓은 request가 여기 쌓인다.
 * 이 리스트를 열람하면 "다음 doorbell로 나갈 후보"가 무엇인지 알 수
 * 있다. hctx->lock은 이 리스트에 대한 삽입/제거(디스패치 워커, 스케줄러)
 * 와의 경쟁을 막기 위해 잡는다.
 * 실행 컨텍스트: "dispatch" 파일 read(2) 컨텍스트.
 * 호출자: seq_file core.
 * 피호출자: spin_lock(), seq_list_start().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   seq_read() -> [hctx_dispatch_start()] -> seq_list_start()
 */
static void *hctx_dispatch_start(struct seq_file *m, loff_t *pos)
	__acquires(&hctx->lock)
{
	struct blk_mq_hw_ctx *hctx = m->private; /* [한국어] blk_mq_debugfs_open()이 저장해 둔 대상 hctx(SQ) */

	spin_lock(&hctx->lock); /* [한국어] hctx->dispatch 리스트 보호 락 획득 - 디스패치 워커의 삽입/제거와 경쟁 방지 */
	return seq_list_start(&hctx->dispatch, *pos); /* [한국어] pos번째 노드부터 순회 시작 */
}

/*
 * [한국어]
 * hctx_dispatch_next - "hctx<N>/dispatch" seq_ops .next 콜백. 다음
 *   dispatch 리스트 노드로 순회 위치를 진행시킨다.
 *
 * @m: seq_file(사용하지 않지만 콜백 시그니처 규약상 존재).
 * @v: 현재 순회 위치.
 * @pos: 갱신할 순회 오프셋.
 * @return: 다음 노드, 마지막이면 NULL.
 *
 * .start에서 잡은 hctx->lock을 계속 쥔 채 호출된다.
 * 실행 컨텍스트: read(2) 도중 seq_file core가 반복 호출.
 * 호출자: seq_file core.
 * 피호출자: seq_list_next().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   seq_read() -> [hctx_dispatch_next()] -> seq_list_next()
 */
static void *hctx_dispatch_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct blk_mq_hw_ctx *hctx = m->private; /* [한국어] dispatch 리스트가 속한 hctx */

	return seq_list_next(v, &hctx->dispatch, pos); /* [한국어] v 다음 dispatch 후보 request로 이동 */
}

/*
 * [한국어]
 * hctx_dispatch_stop - "hctx<N>/dispatch" seq_ops .stop 콜백. .start에서
 *   잡은 hctx->lock을 해제한다.
 *
 * @m: seq_file.
 * @v: 마지막 순회 위치(사용하지 않음).
 * @return: 없음.
 *
 * 이 락이 풀려야 디스패치 워커/스케줄러가 dispatch 리스트를 다시
 * 조작(새 request 삽입, doorbell 전송 후 제거)할 수 있다.
 * 실행 컨텍스트: read(2) 처리 종료 시점.
 * 호출자: seq_file core.
 * 피호출자: spin_unlock().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   seq_read() 종료 -> [hctx_dispatch_stop()] -> spin_unlock()
 */
static void hctx_dispatch_stop(struct seq_file *m, void *v)
	__releases(&hctx->lock)
{
	struct blk_mq_hw_ctx *hctx = m->private; /* [한국어] dispatch 리스트가 속한 hctx */

	spin_unlock(&hctx->lock); /* [한국어] .start에서 획득한 hctx->lock 해제 - 이후 dispatch 리스트 재조작 가능 */
}

/*
 * [한국어]
 * hctx_dispatch_seq_ops - "hctx<N>/dispatch" 파일이 사용하는 seq_operations
 *   4종 세트. .show는 공용 blk_mq_debugfs_rq_show()를 재사용한다.
 */
static const struct seq_operations hctx_dispatch_seq_ops = {
	.start	= hctx_dispatch_start, /* [한국어] hctx->lock 획득 + dispatch 리스트 순회 시작 */
	.next	= hctx_dispatch_next, /* [한국어] 다음 dispatch 후보로 이동 */
	.stop	= hctx_dispatch_stop, /* [한국어] hctx->lock 해제 */
	.show	= blk_mq_debugfs_rq_show, /* [한국어] 노드 -> struct request 변환 후 한 줄 출력(공용 헬퍼) */
};

/*
 * [한국어]
 * show_busy_params - "hctx<N>/busy" 파일을 출력할 때
 *   blk_mq_tagset_busy_iter()의 콜백(hctx_show_busy_rq)에 문맥을 넘기기
 *   위한 이 파일 내부 전용 매개변수 묶음. blk_mq_tagset_busy_iter()의
 *   콜백 시그니처가 void *data 하나만 받으므로, seq_file과 필터링 기준
 *   hctx를 함께 묶어 그 자리에 전달한다.
 */
struct show_busy_params {
	struct seq_file		*m;
	/* [한국어] busy request 출력 결과를 기록할 대상 seq_file.
	 * 설정자: hctx_busy_show()가 자신에게 전달된 m을 그대로 이 필드에
	 *   담아 스택 변수 params를 초기화한다.
	 * 읽는 자: hctx_show_busy_rq()가 이 포인터를 통해
	 *   __blk_mq_debugfs_rq_show(params->m, rq)를 호출해 실제 출력을
	 *   수행한다.
	 * 값 범위: 유효한 seq_file 포인터(NULL 불가) - hctx_busy_show()의
	 *   지역 변수 params가 스택에 있는 동안에만 유효하다.
	 * 동기화: 별도 락 불필요 - params는 hctx_busy_show() 호출 프레임의
	 *   스택에 있고, blk_mq_tagset_busy_iter()가 동기적으로 콜백을
	 *   호출하는 동안에만 참조되므로 다른 스레드와 공유되지 않는다. */

	struct blk_mq_hw_ctx	*hctx;
	/* [한국어] 이 debugfs 파일이 속한 hctx(NVMe SQ에 대응) - busy request
	 * 필터링 기준.
	 * 설정자: hctx_busy_show()가 자신에게 전달된 data(hctx 자체)를 이
	 *   필드에 담는다.
	 * 읽는 자: hctx_show_busy_rq()가 tagset 전체를 순회하며 만나는 각
	 *   request의 rq->mq_hctx와 이 필드를 비교해, 같을 때만 출력한다
	 *   (tagset은 여러 hctx가 공유할 수 있으므로 이 필터링이 필요).
	 * 값 범위: 유효한 blk_mq_hw_ctx 포인터(NULL 불가).
	 * 동기화: show_busy_params.m과 동일 - 스택 수명 동안만 유효, 별도
	 *   락 불필요. */
};

/*
 * Note: the state of a request may change while this function is in progress,
 * e.g. due to a concurrent blk_mq_finish_request() call. Returns true to
 * keep iterating requests.
 */
/*
 * [한국어]
 * hctx_show_busy_rq - blk_mq_tagset_busy_iter()가 tagset의 각 busy(할당된)
 *   request마다 호출하는 콜백. rq->mq_hctx가 대상 hctx와 일치하는 것만
 *   골라 출력한다.
 *
 * @rq: tagset 순회 중 현재 만난 request(할당되어 사용 중인 tag에 대응).
 * @data: hctx_busy_show()가 넘긴 struct show_busy_params * (void* 로 소거).
 * @return: 항상 true - blk_mq_tagset_busy_iter()에게 "순회를 계속하라"고
 *          알려 tagset 전체(다른 hctx에 속한 request 포함)를 끝까지 스캔.
 *
 * blk_mq_tags(태그셋)는 여러 hctx가 공유할 수 있으므로(예:
 * BLK_MQ_F_TAG_QUEUE_SHARED), 특정 hctx만의 busy request를 얻으려면 태그셋
 * 전체를 순회하면서 원치 않는 hctx의 request는 걸러내야 한다. 이 함수가
 * 그 필터 역할을 한다. 주석(위 영어 원문)이 지적하듯, 순회 도중 다른
 * CPU에서 blk_mq_finish_request()가 동시에 실행되어 rq의 상태가 바뀔 수
 * 있지만, 이 debugfs 출력은 스냅샷 성격이므로 약간의 시점 불일치는
 * 허용된다(치명적 오류가 아님).
 * 실행 컨텍스트: hctx_busy_show()가 쥔 q->elevator_lock 아래,
 * blk_mq_tagset_busy_iter()가 태그셋을 순회하며 동기적으로 호출한다.
 * 호출자: blk_mq_tagset_busy_iter()(block/blk-mq-tag.c, 태그셋의 모든
 *   busy tag를 순회).
 * 피호출자: __blk_mq_debugfs_rq_show().
 * 에러 경로: 없음(항상 true 반환).
 *
 * 호출 체인:
 *   hctx_busy_show() -> blk_mq_tagset_busy_iter() -> [hctx_show_busy_rq()]
 *     -> __blk_mq_debugfs_rq_show()
 */
static bool hctx_show_busy_rq(struct request *rq, void *data)
{
	const struct show_busy_params *params = data; /* [한국어] void* data를 원래 타입인 show_busy_params*로 복원 */

	if (rq->mq_hctx == params->hctx) /* [한국어] 이 request가 우리가 보고 싶은 hctx에 배정된 것인지 확인 - 태그셋이 여러 hctx에 공유될 수 있으므로 필요 */
		__blk_mq_debugfs_rq_show(params->m, rq); /* [한국어] 일치하면 seq_file에 한 줄 출력 */

	return true; /* [한국어] 항상 true 반환 - 남은 태그셋 전체(다른 hctx 소속 포함)를 계속 순회하도록 지시 */
}

/*
 * [한국어]
 * hctx_busy_show - "hctx<N>/busy" debugfs 파일의 .show 콜백. 이 hctx에
 *   현재 할당되어 있는(busy) 모든 request를 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 0(성공), 또는 mutex_lock_interruptible() 실패 시 그 음수 errno
 *          (예: -EINTR - 대기 중 시그널 수신).
 *
 * "dispatch"가 아직 SQ에 내보내지 못한 소프트웨어 큐라면, "busy"는 이미
 * tag(NVMe CID에 대응)를 할당받아 어떤 단계로든 진행 중인 모든 request를
 * 보여준다 - dispatch 대기 중이든, SQ에 삽입되어 CQ 완료를 기다리는
 * 중이든 모두 포함된다. q->elevator_lock을 잡는 이유는 순회 도중
 * 스케줄러 교체(elevator switch)가 동시에 일어나 태그셋/스케줄러 내부
 * 상태가 바뀌는 것을 막기 위함이다.
 * 실행 컨텍스트: "busy" 파일 read(2) 컨텍스트. mutex_lock_interruptible()을
 * 쓰는 이유는 elevator_lock이 상황에 따라 오래 걸릴 수 있는 뮤텍스라
 * 시그널로 중단 가능해야 하기 때문이다.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: mutex_lock_interruptible(), blk_mq_tagset_busy_iter(),
 *   mutex_unlock().
 * 에러 경로: 락 획득이 시그널로 중단되면 그 errno를 그대로 반환하고 순회
 *   자체는 시작하지 않는다.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_busy_show()]
 *     -> blk_mq_tagset_busy_iter() -> hctx_show_busy_rq()
 */
static int hctx_busy_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx(SQ) */
	struct show_busy_params params = { .m = m, .hctx = hctx }; /* [한국어] 콜백에 넘길 문맥(출력 대상 + 필터 기준 hctx) 스택에 구성 */
	int res; /* [한국어] mutex_lock_interruptible()의 반환값(0=성공, 음수=시그널 등으로 중단) */

	res = mutex_lock_interruptible(&hctx->queue->elevator_lock); /* [한국어] elevator_lock 획득 시도 - 순회 도중 스케줄러 교체와의 경쟁 방지, 시그널로 중단 가능 */
	if (res) /* [한국어] 락 획득 실패(시그널 등)면 */
		return res; /* [한국어] 순회 없이 그 오류 코드 그대로 반환 */
	blk_mq_tagset_busy_iter(hctx->queue->tag_set, hctx_show_busy_rq, /* [한국어] 이 큐의 태그셋 전체를 순회하며 busy request마다 콜백 호출 */
				&params); /* [한국어] 콜백에 넘길 문맥(seq_file + 필터 hctx) */
	mutex_unlock(&hctx->queue->elevator_lock); /* [한국어] elevator_lock 해제 */

	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * hctx_types - enum hctx_type(hctx가 처리하는 I/O 용도 분류) 값을 문자열로
 *   매핑. "hctx<N>/type" 파일에서 hctx_type_show()가 사용한다.
 *
 * NVMe 관점: 멀티큐 NVMe 컨트롤러는 인터럽트 벡터를 default/read/poll
 * 세 그룹으로 나눠 배정할 수 있다. HCTX_TYPE_DEFAULT는 일반 read/write
 * SQ, HCTX_TYPE_READ는 읽기 전용으로 분리된 저지연 SQ(쓰기가 읽기 지연에
 * 영향을 주지 않도록), HCTX_TYPE_POLL은 인터럽트 없이 폴링으로만 완료를
 * 확인하는 SQ(REQ_POLLED 요청 전용, 인터럽트 오버헤드 제거)에 대응한다.
 */
static const char *const hctx_types[] = {
	[HCTX_TYPE_DEFAULT]	= "default", /* [한국어] 일반 read/write 겸용 SQ */
	[HCTX_TYPE_READ]	= "read", /* [한국어] 읽기 전용으로 분리된 저지연 SQ */
	[HCTX_TYPE_POLL]	= "poll", /* [한국어] 인터럽트 없이 폴링으로만 완료 확인하는 SQ */
};

/*
 * [한국어]
 * hctx_type_show - "hctx<N>/type" debugfs 파일의 .show 콜백. 이 hctx의
 *   용도 분류(default/read/poll)를 문자열로 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * hctx->type은 hctx 생성 시(태그셋의 map[] 배열 설정에 따라) 고정되며,
 * 어떤 인터럽트 벡터/CPU 집합이 이 hctx에 매핑됐는지에 대한 정책 정보를
 * 담는다. NVMe 디버깅 시 특정 hctx가 "poll" 타입인지 확인하면 그 SQ에는
 * REQ_POLLED 요청만 몰리고 인터럽트 기반 완료 통지가 없다는 것을 알 수
 * 있다.
 * 실행 컨텍스트: debugfs read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: BUILD_BUG_ON, seq_printf().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_type_show()] -> seq_printf()
 */
static int hctx_type_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */

	BUILD_BUG_ON(ARRAY_SIZE(hctx_types) != HCTX_MAX_TYPES); /* [한국어] 컴파일 타임 검증 - hctx_types 배열 크기와 실제 enum hctx_type 종류 수(HCTX_MAX_TYPES) 불일치 시 빌드 실패 */
	seq_printf(m, "%s\n", hctx_types[hctx->type]); /* [한국어] hctx->type(정수 enum)을 인덱스로 이름 문자열을 찾아 출력 */
	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * hctx_ctx_map_show - "hctx<N>/ctx_map" debugfs 파일의 .show 콜백.
 *   hctx->ctx_map(이 hctx에 매핑된 CPU 집합을 나타내는 sbitmap) 원시
 *   비트맵을 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * ctx_map은 blk_mq_ctx(CPU 하나에 대응하는 소프트웨어 큐) 중 어떤 CPU가
 * 이 hctx로 I/O를 흘려보내는지, 그리고 그 CPU의 rq_lists[]에 아직 옮겨지지
 * 않은 대기 항목이 있는지를 나타내는 비트맵이다(정확한 비트 의미는
 * block/blk-mq.c의 사용처 참고). NVMe 멀티큐 환경에서 CPU 토폴로지와
 * SQ 배정 관계를 시각적으로 확인할 때 쓰인다.
 * 실행 컨텍스트: debugfs read(2) 컨텍스트. sbitmap_bitmap_show() 내부에서
 * 필요한 동기화를 자체적으로 수행한다.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: sbitmap_bitmap_show().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_ctx_map_show()]
 *     -> sbitmap_bitmap_show()
 */
static int hctx_ctx_map_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */

	sbitmap_bitmap_show(&hctx->ctx_map, m); /* [한국어] CPU -> hctx 매핑 비트맵을 그대로 seq_file에 출력(sbitmap 코어 헬퍼) */
	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * blk_mq_debugfs_tags_show - struct blk_mq_tags(하드웨어 또는 스케줄러
 *   tag 공간) 하나의 요약 정보와 비트맵을 출력하는 공용 헬퍼.
 *   hctx_tags_show()와 hctx_sched_tags_show() 양쪽에서 재사용된다.
 *
 * @m: 출력 대상 seq_file.
 * @tags: 출력할 tag 공간. hctx->tags(하드웨어 tag, NVMe CID에 대응) 또는
 *        hctx->sched_tags(스케줄러가 관리하는 가상 tag) 둘 다 이 함수로
 *        들어올 수 있다.
 * @return: 없음(void).
 *
 * nr_tags는 이 tag 공간의 전체 슬롯 수(NVMe라면 SQ의 queue depth에
 * 대응하는 상한), nr_reserved_tags는 관리 명령/flush 등을 위해 미리
 * 떼어 둔 예약 tag 수, active_queues는 이 tag 공간을 현재 공유해 쓰고
 * 있는 hctx 개수(TAG_QUEUE_SHARED일 때 1보다 커질 수 있음)다.
 * bitmap_tags/breserved_tags는 sbitmap_queue(계층적 비트맵 + 대기열)로
 * 구현된 실제 할당 상태이며, sbitmap_queue_show()가 각 워드의 사용/대기
 * 현황까지 상세히 출력한다.
 * 실행 컨텍스트: 호출자(hctx_tags_show/hctx_sched_tags_show)가 이미
 * q->elevator_lock을 쥔 상태에서 호출되어, tags 포인터 자체가 그 사이
 * 해제(스케줄러 교체 등)되지 않음을 보장받는다.
 * 호출자: hctx_tags_show(), hctx_sched_tags_show().
 * 피호출자: seq_printf(), READ_ONCE(), sbitmap_queue_show().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   hctx_tags_show() 또는 hctx_sched_tags_show()
 *     -> [blk_mq_debugfs_tags_show()] -> sbitmap_queue_show()
 */
static void blk_mq_debugfs_tags_show(struct seq_file *m,
				     struct blk_mq_tags *tags)
{
	seq_printf(m, "nr_tags=%u\n", tags->nr_tags); /* [한국어] 이 tag 공간의 전체 슬롯 수 - NVMe SQ queue depth 상한에 대응 */
	seq_printf(m, "nr_reserved_tags=%u\n", tags->nr_reserved_tags); /* [한국어] 관리 명령/flush 등을 위해 예약된 tag 수 */
	seq_printf(m, "active_queues=%d\n", /* [한국어] 이 tag 공간을 현재 공유 중인 hctx 개수 출력 준비 */
		   READ_ONCE(tags->active_queues)); /* [한국어] 다른 CPU가 동시에 갱신할 수 있으므로 READ_ONCE()로 컴파일러 재정렬/캐시 없이 한 번만 읽음 */

	seq_puts(m, "\nbitmap_tags:\n"); /* [한국어] 일반(비예약) tag 비트맵 섹션 제목 */
	sbitmap_queue_show(&tags->bitmap_tags, m); /* [한국어] 일반 tag의 sbitmap_queue 상세 상태(워드별 사용/대기) 출력 */

	if (tags->nr_reserved_tags) { /* [한국어] 예약 tag가 하나라도 존재하면 */
		seq_puts(m, "\nbreserved_tags:\n"); /* [한국어] 예약 tag 비트맵 섹션 제목 */
		sbitmap_queue_show(&tags->breserved_tags, m); /* [한국어] 예약 tag의 sbitmap_queue 상세 상태 출력 */
	}
}

/*
 * [한국어]
 * hctx_tags_show - "hctx<N>/tags" debugfs 파일의 .show 콜백. 이 hctx의
 *   하드웨어 tag 공간(hctx->tags) 상태를 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 0(성공) 또는 mutex_lock_interruptible() 실패 시 음수 errno.
 *
 * hctx->tags는 NVMe에서 실제 SQ에 삽입될 때 쓰는 CID 공간에 대응하는
 * 하드웨어 tag 세트다. 스케줄러 교체(elevator switch) 도중에는 tags가
 * 잠깐 재할당될 수 있으므로 q->elevator_lock으로 그 사이의 접근을
 * 막는다. hctx->tags가 아직 초기화 전(NULL)일 수도 있으므로(큐 생성
 * 초기 타이밍 등) 조건부로만 출력한다.
 * 실행 컨텍스트: "tags" 파일 read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: mutex_lock_interruptible(), blk_mq_debugfs_tags_show(),
 *   mutex_unlock().
 * 에러 경로: 락 획득 실패 시 그 errno 반환.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_tags_show()]
 *     -> blk_mq_debugfs_tags_show()
 */
static int hctx_tags_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */
	struct request_queue *q = hctx->queue; /* [한국어] 이 hctx가 속한 request_queue - elevator_lock 접근용 */
	int res; /* [한국어] mutex_lock_interruptible() 반환값 */

	res = mutex_lock_interruptible(&q->elevator_lock); /* [한국어] elevator_lock 획득 - 스케줄러 교체 중 tags 재할당과의 경쟁 방지 */
	if (res) /* [한국어] 락 획득 실패(시그널 등)면 */
		return res; /* [한국어] 그 오류 코드 그대로 반환 */
	if (hctx->tags) /* [한국어] 하드웨어 tag 공간이 아직 초기화되어 있으면 */
		blk_mq_debugfs_tags_show(m, hctx->tags); /* [한국어] nr_tags/active_queues/비트맵 출력 */
	mutex_unlock(&q->elevator_lock); /* [한국어] elevator_lock 해제 */

	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * hctx_tags_bitmap_show - "hctx<N>/tags_bitmap" debugfs 파일의 .show
 *   콜백. hctx->tags의 원시(raw) 비트맵만 간략하게 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 0(성공) 또는 mutex_lock_interruptible() 실패 시 음수 errno.
 *
 * "tags"가 nr_tags/active_queues 등 부가 정보까지 포함한 상세 뷰라면,
 * 이 파일은 sbitmap_bitmap_show()로 순수 비트 패턴(어느 tag/CID가
 * 사용 중인지)만 빠르게 확인하려는 용도다.
 * 실행 컨텍스트: "tags_bitmap" 파일 read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: mutex_lock_interruptible(), sbitmap_bitmap_show(),
 *   mutex_unlock().
 * 에러 경로: 락 획득 실패 시 그 errno 반환.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_tags_bitmap_show()]
 *     -> sbitmap_bitmap_show()
 */
static int hctx_tags_bitmap_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */
	struct request_queue *q = hctx->queue; /* [한국어] elevator_lock 접근을 위한 상위 request_queue */
	int res; /* [한국어] mutex_lock_interruptible() 반환값 */

	res = mutex_lock_interruptible(&q->elevator_lock); /* [한국어] elevator_lock 획득 - tags 재할당과의 경쟁 방지 */
	if (res) /* [한국어] 락 획득 실패면 */
		return res; /* [한국어] 오류 코드 반환 */
	if (hctx->tags) /* [한국어] 하드웨어 tag 공간이 초기화되어 있으면 */
		sbitmap_bitmap_show(&hctx->tags->bitmap_tags.sb, m); /* [한국어] 일반 tag의 원시 비트맵만 출력(예약 tag 제외) */
	mutex_unlock(&q->elevator_lock); /* [한국어] elevator_lock 해제 */

	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * hctx_sched_tags_show - "hctx<N>/sched_tags" debugfs 파일의 .show 콜백.
 *   이 hctx의 스케줄러 tag 공간(hctx->sched_tags) 상태를 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 0(성공) 또는 mutex_lock_interruptible() 실패 시 음수 errno.
 *
 * I/O 스케줄러(mq-deadline 등)가 붙어 있으면, request는 먼저
 * sched_tags(스케줄러가 관리하는 가상 tag 공간)에서 internal_tag를 받고,
 * 실제 디스패치 시점에야 hctx->tags에서 하드웨어 tag(NVMe CID)를 받는
 * 2단계 할당을 거친다. 이 파일은 그 1단계, 즉 아직 하드웨어에 내려가기
 * 전 스케줄러 레벨의 대기 상황을 보여준다. 스케줄러가 없는 none 모드나
 * BLK_MQ_F_NO_SCHED_BY_DEFAULT 장치에서는 hctx->sched_tags가 NULL일 수
 * 있어 조건부로만 출력한다.
 * 실행 컨텍스트: "sched_tags" 파일 read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: mutex_lock_interruptible(), blk_mq_debugfs_tags_show(),
 *   mutex_unlock().
 * 에러 경로: 락 획득 실패 시 그 errno 반환.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_sched_tags_show()]
 *     -> blk_mq_debugfs_tags_show()
 */
static int hctx_sched_tags_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */
	struct request_queue *q = hctx->queue; /* [한국어] elevator_lock 접근을 위한 상위 request_queue */
	int res; /* [한국어] mutex_lock_interruptible() 반환값 */

	res = mutex_lock_interruptible(&q->elevator_lock); /* [한국어] elevator_lock 획득 - 스케줄러 교체 중 sched_tags 재할당과의 경쟁 방지 */
	if (res) /* [한국어] 락 획득 실패면 */
		return res; /* [한국어] 오류 코드 반환 */
	if (hctx->sched_tags) /* [한국어] 스케줄러 tag 공간이 존재하면(스케줄러가 붙어 있으면) */
		blk_mq_debugfs_tags_show(m, hctx->sched_tags); /* [한국어] nr_tags/active_queues/비트맵 출력 */
	mutex_unlock(&q->elevator_lock); /* [한국어] elevator_lock 해제 */

	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * hctx_sched_tags_bitmap_show - "hctx<N>/sched_tags_bitmap" debugfs
 *   파일의 .show 콜백. hctx->sched_tags의 원시 비트맵만 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 0(성공) 또는 mutex_lock_interruptible() 실패 시 음수 errno.
 *
 * hctx_tags_bitmap_show()의 스케줄러 tag 버전 - nr_tags 등 부가 정보 없이
 * 순수 비트 패턴만 빠르게 확인하려는 용도다.
 * 실행 컨텍스트: "sched_tags_bitmap" 파일 read(2) 컨텍스트.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: mutex_lock_interruptible(), sbitmap_bitmap_show(),
 *   mutex_unlock().
 * 에러 경로: 락 획득 실패 시 그 errno 반환.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_sched_tags_bitmap_show()]
 *     -> sbitmap_bitmap_show()
 */
static int hctx_sched_tags_bitmap_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */
	struct request_queue *q = hctx->queue; /* [한국어] elevator_lock 접근을 위한 상위 request_queue */
	int res; /* [한국어] mutex_lock_interruptible() 반환값 */

	res = mutex_lock_interruptible(&q->elevator_lock); /* [한국어] elevator_lock 획득 - sched_tags 재할당과의 경쟁 방지 */
	if (res) /* [한국어] 락 획득 실패면 */
		return res; /* [한국어] 오류 코드 반환 */
	if (hctx->sched_tags) /* [한국어] 스케줄러 tag 공간이 존재하면 */
		sbitmap_bitmap_show(&hctx->sched_tags->bitmap_tags.sb, m); /* [한국어] 스케줄러 tag의 원시 비트맵만 출력 */
	mutex_unlock(&q->elevator_lock); /* [한국어] elevator_lock 해제 */

	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * hctx_active_show - "hctx<N>/active" debugfs 파일의 .show 콜백. 이 hctx
 *   에서 현재 활성(할당되어 사용 중)인 request 개수를 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * __blk_mq_active_requests()는 hctx->tags를 기준으로 현재 할당된(busy)
 * tag 개수를 세는 헬퍼로, NVMe 관점에서는 이 SQ에 현재 in-flight 상태인
 * 명령 수(아직 CQ 완료를 못 받은 CID 개수)에 대응한다. 값이 nr_tags에
 * 가까울수록 이 SQ가 포화 상태에 근접했다는 뜻이다.
 * 실행 컨텍스트: "active" 파일 read(2) 컨텍스트. 별도 락 없이 원자적
 * 카운터를 읽으므로 스냅샷 값이며, 락을 잡지 않는 이유는 단순 카운터
 * 조회이고 잠깐의 시점 불일치는 디버깅 목적상 허용되기 때문이다.
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: __blk_mq_active_requests(), seq_printf().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_active_show()]
 *     -> __blk_mq_active_requests()
 */
static int hctx_active_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */

	seq_printf(m, "%d\n", __blk_mq_active_requests(hctx)); /* [한국어] 이 hctx에서 현재 할당된 tag(=in-flight NVMe 명령) 개수를 세어 출력 */
	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * hctx_dispatch_busy_show - "hctx<N>/dispatch_busy" debugfs 파일의 .show
 *   콜백. hctx->dispatch_busy(EWMA 기반 혼잡도 추정치)를 출력한다.
 *
 * @data: debugfs aux로 전달된 struct blk_mq_hw_ctx *.
 * @m: 출력 대상 seq_file.
 * @return: 항상 0(성공).
 *
 * dispatch_busy는 mq_ops->queue_rq()가 -EBUSY 등을 반환해 디스패치가
 * 실패한 빈도를 지수가중이동평균(EWMA)으로 추적하는 값으로, 이 값이
 * 높을수록 하드웨어(NVMe SQ/CQ)가 포화 상태에 가까워 소프트웨어 큐에서
 * 대기하는 시간이 길어지고 있다는 신호다. block/blk-mq.c의 디스패치
 * 경로가 이 값을 근거로 배치 크기 등을 조절하는 데도 쓰인다.
 * 실행 컨텍스트: "dispatch_busy" 파일 read(2) 컨텍스트. 별도 락 없이
 * 단일 unsigned int 필드를 읽으므로 원자성 문제는 크지 않다(정밀한
 * 순간값보다 추세를 보는 용도).
 * 호출자: blk_mq_debugfs_show().
 * 피호출자: seq_printf().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   read(2) -> blk_mq_debugfs_show() -> [hctx_dispatch_busy_show()]
 */
static int hctx_dispatch_busy_show(void *data, struct seq_file *m)
{
	struct blk_mq_hw_ctx *hctx = data; /* [한국어] debugfs aux로 전달된 대상 hctx */

	seq_printf(m, "%u\n", hctx->dispatch_busy); /* [한국어] EWMA 기반 디스패치 혼잡도 추정치 그대로 출력 - 값이 클수록 SQ/CQ 포화 가능성 시사 */
	return 0; /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어]
 * CTX_RQ_SEQ_OPS - blk_mq_ctx(CPU 하나에 대응하는 소프트웨어 큐)의
 *   rq_lists[type] 하나를 debugfs로 노출하는 seq_ops 4종 세트를 통째로
 *   찍어내는 매크로. HCTX_TYPE_DEFAULT/READ/POLL 세 가지 type마다 거의
 *   동일한 start/next/stop/seq_ops 코드를 반복해서 손으로 쓰지 않기 위해
 *   전처리기 단계에서 이름을 조합(##)해 생성한다.
 *
 * @name: 생성될 함수/변수 이름에 들어갈 접미사(default/read/poll). 최종
 *        심벌 이름은 ctx_##name##_rq_list_start 등으로 만들어진다.
 * @type: blk_mq_ctx->rq_lists[] 배열의 인덱스(HCTX_TYPE_DEFAULT/READ/POLL).
 *
 * ctx->rq_lists[type]는 이 CPU에서 제출된 request가 아직 어떤 hctx의
 * dispatch 리스트로도 옮겨지지 않고 대기 중인 per-CPU 소프트웨어 큐다.
 * NVMe 멀티큐 환경에서 default/read/poll 세 그룹으로 인터럽트 벡터가
 * 나뉘어 있다면, 이 리스트들은 "이 CPU에서 나온 요청이 아직 어느 SQ로도
 * 안 갔다"는 가장 이른 단계의 대기 상태를 보여준다. 생성되는 함수들의
 * 락/순회 패턴은 hctx_dispatch_start/next/stop과 동일하되 보호 대상이
 * ctx->lock과 ctx->rq_lists[type]라는 점만 다르다.
 *
 * 호출 체인(생성된 각 함수 공통):
 *   seq_read() -> [ctx_<name>_rq_list_start/next/stop()]
 *     -> seq_list_start()/seq_list_next() 또는 spin_lock/unlock()
 */
#define CTX_RQ_SEQ_OPS(name, type)					\
static void *ctx_##name##_rq_list_start(struct seq_file *m, loff_t *pos) \
	__acquires(&ctx->lock)						\
{									\
	struct blk_mq_ctx *ctx = m->private;	/* [한국어] blk_mq_debugfs_open()이 저장한 대상 blk_mq_ctx(per-CPU 큐) */ \
									\
	spin_lock(&ctx->lock);			/* [한국어] ctx->rq_lists[] 보호 락 획득 - 제출/dispatch 이관 경로와의 경쟁 방지 */ \
	return seq_list_start(&ctx->rq_lists[type], *pos); /* [한국어] type(default/read/poll)에 해당하는 리스트의 순회 시작 */ \
}									\
									\
static void *ctx_##name##_rq_list_next(struct seq_file *m, void *v,	\
				     loff_t *pos)			\
{									\
	struct blk_mq_ctx *ctx = m->private;	/* [한국어] rq_lists[]가 속한 blk_mq_ctx */ \
									\
	return seq_list_next(v, &ctx->rq_lists[type], pos); /* [한국어] v 다음 노드로 순회 위치 이동 */ \
}									\
									\
static void ctx_##name##_rq_list_stop(struct seq_file *m, void *v)	\
	__releases(&ctx->lock)						\
{									\
	struct blk_mq_ctx *ctx = m->private;	/* [한국어] rq_lists[]가 속한 blk_mq_ctx */ \
									\
	spin_unlock(&ctx->lock);		/* [한국어] .start에서 획득한 ctx->lock 해제 */ \
}									\
									\
static const struct seq_operations ctx_##name##_rq_list_seq_ops = {	\
	.start	= ctx_##name##_rq_list_start,	/* [한국어] ctx->lock 획득 + rq_lists[type] 순회 시작 */ \
	.next	= ctx_##name##_rq_list_next,	/* [한국어] 다음 대기 request로 이동 */ \
	.stop	= ctx_##name##_rq_list_stop,	/* [한국어] ctx->lock 해제 */ \
	.show	= blk_mq_debugfs_rq_show,	/* [한국어] 노드 -> struct request 변환 후 한 줄 출력(공용 헬퍼) */ \
}

CTX_RQ_SEQ_OPS(default, HCTX_TYPE_DEFAULT); /* [한국어] "default_rq_list" 파일용 seq_ops 4종(ctx_default_rq_list_*) 생성 - 일반 read/write 겸용 큐 */
CTX_RQ_SEQ_OPS(read, HCTX_TYPE_READ); /* [한국어] "read_rq_list" 파일용 seq_ops 생성 - 읽기 전용 저지연 큐 */
CTX_RQ_SEQ_OPS(poll, HCTX_TYPE_POLL); /* [한국어] "poll_rq_list" 파일용 seq_ops 생성 - 폴링 완료 전용 큐 */

/*
 * [한국어]
 * blk_mq_debugfs_show - .show 콜백 방식(단일 값 출력, seq_ops 아님) 파일의
 *   공용 read 어댑터. single_open()이 감싸는 실제 진입점이며, attr->show를
 *   찾아 위임한다.
 *
 * @m: single_open()이 생성한 seq_file. m->private에 blk_mq_debugfs_open()이
 *     저장해 둔 struct blk_mq_debugfs_attr *가 들어 있다(개별 파일의
 *     show/write 콜백 테이블 엔트리).
 * @v: single_open() 규약상 항상 SEQ_START_TOKEN 등 단일 토큰(사용하지 않음).
 * @return: attr->show(data, m)의 반환값을 그대로 전달.
 *
 * blk_mq_debugfs_attr.show가 채워진 모든 debugfs 파일(state, tags,
 * sched_tags, pm_only, zone_wplugs 등)의 공통 진입점이다. seq_ops 기반
 * 리스트 파일(dispatch, busy, *_rq_list 등)은 이 경로를 타지 않고 곧바로
 * attr->seq_ops의 .show가 호출된다.
 * 실행 컨텍스트: debugfs 파일 read(2) 컨텍스트.
 * 호출자: single_open()이 등록한 내부 seq_operations의 .show(커널 seq_file
 *   core, fs/seq_file.c).
 * 피호출자: debugfs_get_aux(), attr->show(개별 hctx_state_show() 등).
 * 에러 경로: attr->show가 반환하는 값을 그대로 전달(이 함수 자체는 에러를
 *   만들지 않음).
 *
 * 호출 체인:
 *   seq_read() -> single 방식 내부 .show -> [blk_mq_debugfs_show()]
 *     -> attr->show()
 */
static int blk_mq_debugfs_show(struct seq_file *m, void *v)
{
	const struct blk_mq_debugfs_attr *attr = m->private; /* [한국어] blk_mq_debugfs_open()이 저장해 둔, 이 파일을 기술하는 attr 테이블 엔트리 */
	void *data = debugfs_get_aux(m->file); /* [한국어] debugfs_create_file_aux()로 등록해 둔 실제 대상(request_queue/hctx/ctx 등) 복원 */

	return attr->show(data, m); /* [한국어] 개별 show 콜백(hctx_state_show 등)에 위임 - 실제 텍스트 생성은 그 안에서 이루어짐 */
}

/*
 * [한국어]
 * blk_mq_debugfs_write - 모든 debugfs 파일이 공유하는 공용 write(2)
 *   file_operations 핸들러. attr->write가 있는 파일에만 실제 처리를
 *   위임하고, 나머지(읽기 전용 파일)는 -EPERM으로 거부한다.
 *
 * @file: 사용자가 write(2)한 대상 파일. file->private_data에 seq_file이
 *        들어 있다(open 시 seq_open() 또는 single_open()이 설정).
 * @buf: 사용자 공간 버퍼(__user 포인터).
 * @count: buf의 바이트 수.
 * @ppos: 파일 오프셋.
 * @return: attr->write()의 반환값, 또는 쓰기 불가 파일이면 -EPERM.
 *
 * .seq_ops만 구현한 파일(dispatch, busy, *_rq_list 등 리스트형 읽기
 * 전용 파일)은 open 시 m->private에 attr 대신 data(hctx 등) 자체가
 * 저장되므로, "attr == data"라는 비교로 그런 파일을 식별해 즉시 거부한다.
 * 이는 미묘하지만 의도된 트릭으로, .show 콜백 방식 파일만 m->private에
 * attr 포인터 자체가 들어가기 때문에 성립한다. "state"처럼 .write가
 * 채워진 제어 파일만 실제로 여기서 attr->write(queue_state_write 등)로
 * 이어진다.
 * 실행 컨텍스트: 사용자 프로세스의 write(2) syscall 컨텍스트.
 * 호출자: VFS(파일의 file_operations.write로 등록됨).
 * 피호출자: debugfs_get_aux(), attr->write().
 * 에러 경로: seq_ops 전용(읽기 전용) 파일이거나 attr->write가 NULL이면
 *   -EPERM.
 *
 * 호출 체인:
 *   write(2) -> VFS -> [blk_mq_debugfs_write()] -> attr->write()
 *     (예: queue_state_write())
 */
static ssize_t blk_mq_debugfs_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data; /* [한국어] open() 단계에서 seq_open()/single_open()이 설정해 둔 seq_file */
	const struct blk_mq_debugfs_attr *attr = m->private; /* [한국어] .show 방식 파일이면 attr 테이블 엔트리, seq_ops 방식 파일이면 data 자체(아래에서 구분) */
	void *data = debugfs_get_aux(file); /* [한국어] debugfs 등록 시 넘긴 실제 대상(request_queue/hctx/ctx) */

	/*
	 * Attributes that only implement .seq_ops are read-only and 'attr' is
	 * the same with 'data' in this case.
	 */
	if (attr == data || !attr->write) /* [한국어] seq_ops 전용(읽기 전용) 파일이거나 이 attr에 write 콜백이 없으면 */
		return -EPERM; /* [한국어] 쓰기 권한 없음 오류 반환 */

	return attr->write(data, buf, count, ppos); /* [한국어] 개별 write 콜백(queue_state_write 등)에 실제 처리 위임 */
}

/*
 * [한국어]
 * blk_mq_debugfs_open - 모든 blk-mq debugfs 파일이 공유하는 공용 open(2)
 *   file_operations 핸들러. attr->seq_ops 유무로 리스트형(seq_open)과
 *   단일값형(single_open) 두 경로로 분기한다.
 *
 * @inode: 열리는 파일의 inode. inode->i_private에 debugfs_create_file_aux()
 *         호출 시 넘긴 struct blk_mq_debugfs_attr *가 저장되어 있다.
 * @file: 새로 열리는 struct file.
 * @return: 0(성공) 또는 seq_open()/single_open()이 반환하는 음수 errno,
 *          seq_ops도 show도 없는 잘못된 attr이면 -EPERM.
 *
 * .seq_ops가 채워진 attr(리스트형: dispatch/busy/*_rq_list/requeue_list)는
 * seq_open()으로 열어 커널이 자동으로 .start/.next/.stop/.show 반복
 * 호출을 처리하게 하고, 연 직후 m->private에 data(실제 대상 hctx/ctx/큐)
 * 를 심어 각 seq_ops 콜백이 m->private에서 그 대상을 꺼내 쓸 수 있게
 * 한다. .show만 채워진 attr(단일값형: state/tags/pm_only 등)는
 * single_open()으로 열어 blk_mq_debugfs_show()가 attr->show를 호출하게
 * 하며, 이 경우 m->private에는 attr 자체가 들어간다(위 blk_mq_debugfs_write
 * 의 attr==data 판별 트릭이 성립하는 이유).
 * 실행 컨텍스트: 사용자 프로세스의 open(2) syscall 컨텍스트.
 * 호출자: VFS(blk_mq_debugfs_fops.open으로 등록됨).
 * 피호출자: debugfs_get_aux(), seq_open(), single_open().
 * 에러 경로: seq_ops도 show도 없는 손상된 attr 테이블은 WARN_ON_ONCE로
 *   커널 로그에 경고를 남기고 -EPERM 반환(정상 빌드에서는 도달하지 않는
 *   방어적 코드).
 *
 * 호출 체인:
 *   open(2) -> VFS -> [blk_mq_debugfs_open()] -> seq_open() 또는
 *     single_open(blk_mq_debugfs_show)
 */
static int blk_mq_debugfs_open(struct inode *inode, struct file *file)
{
	const struct blk_mq_debugfs_attr *attr = inode->i_private; /* [한국어] debugfs_create_file_aux() 등록 시 넘긴 attr 테이블 엔트리 */
	void *data = debugfs_get_aux(file); /* [한국어] 이 파일이 나타내는 실제 대상(request_queue/hctx/ctx 등) */
	struct seq_file *m; /* [한국어] seq_open() 성공 후 얻을 seq_file 포인터 */
	int ret; /* [한국어] seq_open() 반환값 */

	if (attr->seq_ops) { /* [한국어] 리스트형(순회) 파일이면 */
		ret = seq_open(file, attr->seq_ops); /* [한국어] seq_ops(.start/.next/.stop/.show)로 seq_file 열기 */
		if (!ret) { /* [한국어] 성공하면 */
			m = file->private_data; /* [한국어] 새로 생성된 seq_file 포인터 획득 */
			m->private = data; /* [한국어] 각 seq_ops 콜백(예: hctx_dispatch_start)이 m->private에서 대상을 꺼낼 수 있도록 저장 */
		}
		return ret; /* [한국어] seq_open() 결과 그대로 반환 */
	}

	if (WARN_ON_ONCE(!attr->show)) /* [한국어] seq_ops도 show도 없는 손상된 attr - 정상 빌드에서는 발생하지 않아야 함 */
		return -EPERM; /* [한국어] 방어적으로 권한 오류 반환 */

	return single_open(file, blk_mq_debugfs_show, inode->i_private); /* [한국어] 단일값형 파일 - blk_mq_debugfs_show를 .show로, attr 자체를 m->private로 설정하며 열기 */
}

/*
 * [한국어]
 * blk_mq_debugfs_release - 모든 blk-mq debugfs 파일이 공유하는 공용
 *   release(2)(파일 닫기) file_operations 핸들러. open 시 사용한 방식에
 *   맞춰 single_release() 또는 seq_release()로 분기해 자원을 정리한다.
 *
 * @inode: 닫히는 파일의 inode. inode->i_private에서 attr을 복원해 어떤
 *         방식으로 열렸는지 판별한다.
 * @file: 닫히는 struct file.
 * @return: single_release() 또는 seq_release()의 반환값(보통 0).
 *
 * blk_mq_debugfs_open()의 분기(attr->show 유무)와 정확히 짝을 맞춰야
 * seq_file 내부 상태(버퍼 등)가 올바르게 해제된다 - single_open()으로
 * 연 파일을 seq_release()로 닫거나 그 반대로 하면 안 된다.
 * 실행 컨텍스트: 사용자 프로세스가 파일을 닫거나 프로세스가 종료될 때
 * VFS가 호출.
 * 호출자: VFS(blk_mq_debugfs_fops.release로 등록됨).
 * 피호출자: single_release(), seq_release().
 * 에러 경로: 없음(하위 함수의 반환값을 그대로 전달).
 *
 * 호출 체인:
 *   close(2) -> VFS -> [blk_mq_debugfs_release()] -> single_release() 또는
 *     seq_release()
 */
static int blk_mq_debugfs_release(struct inode *inode, struct file *file)
{
	const struct blk_mq_debugfs_attr *attr = inode->i_private; /* [한국어] 이 파일을 기술하는 attr - open 시 어떤 방식이었는지 재확인 */

	if (attr->show) /* [한국어] single_open() 방식으로 열렸던 파일이면 */
		return single_release(inode, file); /* [한국어] single_open()에 대응하는 해제 절차 */

	return seq_release(inode, file); /* [한국어] seq_open() 방식이었던 파일 - 표준 seq_file 해제 절차 */
}

/*
 * [한국어]
 * blk_mq_debugfs_fops - 이 파일이 생성하는 모든 debugfs 항목이 공유하는
 *   단일 file_operations 인스턴스. 개별 파일의 동작 차이는 이 fops가
 *   아니라 inode->i_private(attr)/debugfs aux(data)로 구분된다.
 */
static const struct file_operations blk_mq_debugfs_fops = {
	.open		= blk_mq_debugfs_open, /* [한국어] seq_ops 유무에 따라 seq_open()/single_open()으로 분기 */
	.read		= seq_read, /* [한국어] 표준 seq_file 커널 코어 read 구현 재사용(직접 구현할 필요 없음) */
	.write		= blk_mq_debugfs_write, /* [한국어] attr->write가 있는 제어 파일에만 실제 쓰기 위임, 나머지는 -EPERM */
	.llseek		= seq_lseek, /* [한국어] 표준 seq_file lseek 구현 재사용 */
	.release	= blk_mq_debugfs_release, /* [한국어] open 방식에 맞춰 single_release()/seq_release()로 분기 */
};

/*
 * [한국어]
 * blk_mq_debugfs_hctx_attrs - hctx(NVMe SQ에 대응하는 하드웨어 큐) 단위
 *   debugfs 파일 테이블. /sys/kernel/debug/block/<disk>/mq/hctx<N>/ 바로
 *   아래에 각 name이 파일로 생성된다(blk_mq_debugfs_register_hctx()가
 *   순회). 리스트형 항목("dispatch")만 .seq_ops를 쓰고 나머지는 .show만
 *   쓴다. 모두 0400(root 읽기 전용) - 이 디렉터리에는 쓰기 가능한 제어
 *   파일이 없다.
 */
static const struct blk_mq_debugfs_attr blk_mq_debugfs_hctx_attrs[] = {
	{"state", 0400, hctx_state_show}, /* [한국어] hctx->state(STOPPED/TAG_ACTIVE 등) 조회 */
	{"flags", 0400, hctx_flags_show}, /* [한국어] hctx->flags(TAG_QUEUE_SHARED 등 정적 특성) 조회 */
	{"dispatch", 0400, .seq_ops = &hctx_dispatch_seq_ops}, /* [한국어] hctx->dispatch 리스트 순회 출력 - .show 대신 seq_ops 사용 */
	{"busy", 0400, hctx_busy_show}, /* [한국어] 이 hctx에 배정된 in-flight request 전체 출력 */
	{"ctx_map", 0400, hctx_ctx_map_show}, /* [한국어] CPU -> hctx 매핑 비트맵 조회 */
	{"tags", 0400, hctx_tags_show}, /* [한국어] 하드웨어 tag(CID) 공간 상세 조회 */
	{"tags_bitmap", 0400, hctx_tags_bitmap_show}, /* [한국어] 하드웨어 tag 원시 비트맵만 조회 */
	{"sched_tags", 0400, hctx_sched_tags_show}, /* [한국어] 스케줄러 tag 공간 상세 조회 */
	{"sched_tags_bitmap", 0400, hctx_sched_tags_bitmap_show}, /* [한국어] 스케줄러 tag 원시 비트맵만 조회 */
	{"active", 0400, hctx_active_show}, /* [한국어] 현재 활성(in-flight) request 개수 조회 */
	{"dispatch_busy", 0400, hctx_dispatch_busy_show}, /* [한국어] EWMA 기반 디스패치 혼잡도 조회 */
	{"type", 0400, hctx_type_show}, /* [한국어] hctx 용도 분류(default/read/poll) 조회 */
	{}, /* [한국어] name=NULL sentinel - 순회 종료 조건 */
};

/*
 * [한국어]
 * blk_mq_debugfs_ctx_attrs - blk_mq_ctx(CPU 하나에 대응하는 소프트웨어
 *   큐) 단위 debugfs 파일 테이블. /sys/kernel/debug/block/<disk>/mq/
 *   hctx<N>/cpu<M>/ 아래에 생성된다(blk_mq_debugfs_register_ctx()가 순회).
 *   세 항목 모두 CTX_RQ_SEQ_OPS 매크로가 생성한 seq_ops를 사용하는
 *   리스트형 파일이다.
 */
static const struct blk_mq_debugfs_attr blk_mq_debugfs_ctx_attrs[] = {
	{"default_rq_list", 0400, .seq_ops = &ctx_default_rq_list_seq_ops}, /* [한국어] 이 CPU의 default 타입 대기 리스트 출력 */
	{"read_rq_list", 0400, .seq_ops = &ctx_read_rq_list_seq_ops}, /* [한국어] 이 CPU의 read 타입 대기 리스트 출력 */
	{"poll_rq_list", 0400, .seq_ops = &ctx_poll_rq_list_seq_ops}, /* [한국어] 이 CPU의 poll 타입 대기 리스트 출력 */
	{}, /* [한국어] name=NULL sentinel */
};

/*
 * [한국어]
 * debugfs_create_files - attr[] 테이블을 순회하며 parent 디렉터리 아래
 *   실제 debugfs 파일들을 생성하는 공용 헬퍼. 이 파일의 모든
 *   blk_mq_debugfs_register*() 함수가 최종적으로 이 함수를 거쳐 파일을
 *   만든다.
 *
 * @q: attr 테이블이 속한 request_queue. 잠금 상태(debugfs_mutex 보유,
 *     elevator_lock/rq_qos_mutex 비보유) 검증에 사용된다.
 * @parent: 파일들이 생성될 상위 debugfs 디렉터리(dentry). NULL이거나
 *          에러 포인터면 아무 것도 하지 않는다.
 * @data: 각 파일의 debugfs aux로 저장될 실제 대상(request_queue/hctx/ctx/
 *        rq_qos 등, attr 테이블의 종류에 따라 다름).
 * @attr: name이 NULL인 sentinel로 끝나는 blk_mq_debugfs_attr 배열.
 * @return: 없음(void). 개별 파일 생성 실패는 조용히 무시된다(디버깅
 *          편의 기능이므로 실패해도 치명적이지 않음).
 *
 * lockdep_assert_held(&q->debugfs_mutex)로 이 함수가 항상 debugfs 등록을
 * 직렬화하는 뮤텍스 아래에서만 호출되도록 강제하고,
 * lockdep_assert_not_held()로 그 뮤텍스가 elevator_lock/rq_qos_mutex와
 * 중첩되지 않도록 강제한다 - 큐가 freeze된 상태에서 잡힐 수 있는 락과
 * debugfs_mutex가 중첩되면 잠재적 데드락 경로가 생기기 때문이다.
 * 실행 컨텍스트: 디스크 등록/재구성/스케줄러 교체 등 각
 * blk_mq_debugfs_register*() 호출 경로와 동일.
 * 호출자: blk_mq_debugfs_register(), blk_mq_debugfs_register_ctx(),
 *   blk_mq_debugfs_register_hctx(), blk_mq_debugfs_register_sched(),
 *   blk_mq_debugfs_register_rqos(), blk_mq_debugfs_register_sched_hctx().
 * 피호출자: lockdep_assert_held()/lockdep_assert_not_held(),
 *   IS_ERR_OR_NULL(), debugfs_create_file_aux().
 * 에러 경로: parent가 유효하지 않으면 조기 반환. 개별
 *   debugfs_create_file_aux() 실패는 반환값을 확인하지 않고 무시한다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register() 등 -> [debugfs_create_files()]
 *     -> debugfs_create_file_aux() (attr마다)
 */
static void debugfs_create_files(struct request_queue *q, struct dentry *parent,
				 void *data,
				 const struct blk_mq_debugfs_attr *attr)
{
	lockdep_assert_held(&q->debugfs_mutex); /* [한국어] 이 함수가 항상 debugfs_mutex를 쥔 채로만 호출되는지 정적/동적으로 검증 - 등록 경로 직렬화 보장 */
	/*
	 * debugfs_mutex should not be nested under other locks that can be
	 * grabbed while queue is frozen.
	 */
	lockdep_assert_not_held(&q->elevator_lock); /* [한국어] elevator_lock을 쥔 채 이 함수를 호출하면 안 됨 - 락 중첩으로 인한 데드락 가능성 차단 */
	lockdep_assert_not_held(&q->rq_qos_mutex); /* [한국어] rq_qos_mutex도 마찬가지로 중첩 금지 */

	if (IS_ERR_OR_NULL(parent)) /* [한국어] 상위 디렉터리가 없거나(아직 생성 전) 에러 포인터면 - debugfs 미마운트 등의 상황 */
		return; /* [한국어] 파일 생성 없이 조용히 반환 - 디버깅 기능이므로 실패해도 치명적이지 않음 */

	for (; attr->name; attr++) /* [한국어] name이 NULL인 sentinel을 만날 때까지 attr 배열 순회 */
		debugfs_create_file_aux(attr->name, attr->mode, parent, /* [한국어] attr 하나당 debugfs 파일 하나 생성 - 이름/권한/상위 디렉터리 지정 */
				    (void *)attr, data, &blk_mq_debugfs_fops); /* [한국어] inode->i_private에 attr을, aux에 data를 저장하고 공용 fops 등록 */
}

/*
 * [한국어]
 * blk_mq_debugfs_register - request_queue의 최상위 debugfs 디렉터리와
 *   그 하위(hctx, rq_qos) 항목들을 한꺼번에 등록한다. 이 파일이 구현하는
 *   등록 API 중 가장 바깥쪽 진입점이다.
 *
 * @q: debugfs 노드를 생성할 대상 request_queue. q->debugfs_dir(보통
 *     block/blk-sysfs.c가 미리 만들어 둔 /sys/kernel/debug/block/<disk>)
 *     아래에 큐 단위 파일과 mq/hctx<N>/ 서브 디렉터리를 채운다.
 * @return: void. q->debugfs_dir이 없으면(마운트 안 됨 등)
 *          debugfs_create_files() 내부에서 조용히 무시된다.
 *
 * 디스크가 새로 등록될 때마다(add_disk() 성공 후 blk_register_queue())
 * 호출되어야 하므로 디스크 등록 경로의 일부다. NVMe 컨트롤러가
 * 네임스페이스를 attach해 새 request_queue가 만들어지면, 이 함수가
 * 한 번 호출되어 /sys/kernel/debug/block/nvme<N>n<M>/ 트리를 만든다.
 * (1) 큐 단위 attr 배열로 파일 생성, (2) 아직 debugfs_dir이 없는 hctx만
 * 골라 blk_mq_debugfs_register_hctx() 호출, (3) rq-qos 상태 등록
 * (blk_mq_debugfs_register_rq_qos()) 순서로 진행한다. hctx 중 이미
 * debugfs_dir이 있는 것을 건너뛰는 이유는, 이 함수가 재호출(예: 큐 재등록)
 * 되어도 이미 만든 hctx 디렉터리를 중복 생성하지 않기 위함이다.
 * 실행 컨텍스트: 디스크 등록 경로(드라이버 probe 스레드)에서 동기적으로
 * 실행되며, q->elevator_lock/q->rq_qos_mutex를 잡지 않은 상태에서만
 * 호출 가능함을 debugfs_create_files() 내부의 lockdep_assert_not_held()가
 * 강제한다.
 * 호출자: block/blk-sysfs.c의 blk_register_queue().
 * 피호출자: debugfs_create_files(), blk_mq_debugfs_register_hctx()(hctx마다),
 *   blk_mq_debugfs_register_rq_qos().
 * 에러 경로: debugfs 생성 실패는 치명적 오류로 취급하지 않는다 - 디버깅
 *   편의 기능이므로 실패해도 I/O 경로에는 영향이 없다.
 *
 * 호출 체인:
 *   blk_register_queue() -> [blk_mq_debugfs_register()]
 *     -> blk_mq_debugfs_register_hctx() (hctx마다)
 *     -> blk_mq_debugfs_register_rq_qos()
 */
void blk_mq_debugfs_register(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx; /* [한국어] queue_for_each_hw_ctx() 순회용 hctx 포인터 */
	unsigned long i; /* [한국어] queue_for_each_hw_ctx()가 쓰는 순회 인덱스(xarray 순회 커서) */

	debugfs_create_files(q, q->debugfs_dir, q, blk_mq_debugfs_queue_attrs); /* [한국어] 디스크(request_queue) 단위 파일들(poll_stat/requeue_list/pm_only/state/zone_wplugs) 생성 */

	queue_for_each_hw_ctx(q, hctx, i) { /* [한국어] 이 큐의 모든 hctx(SQ)를 순회 */
		if (!hctx->debugfs_dir) /* [한국어] 아직 debugfs 디렉터리가 없는 hctx만 - 재호출 시 중복 생성 방지 */
			blk_mq_debugfs_register_hctx(q, hctx); /* [한국어] 개별 hctx 디렉터리 및 하위 파일/ctx 등록 */
	}

	blk_mq_debugfs_register_rq_qos(q); /* [한국어] rq_qos(WBT 등) 체인의 debugfs 노드 등록 */
}

/*
 * [한국어]
 * blk_mq_debugfs_register_ctx - 특정 hctx 아래에 blk_mq_ctx(CPU 하나에
 *   대응하는 소프트웨어 큐) 하나의 "cpu<N>/" 디렉터리를 생성한다.
 *
 * @hctx: ctx가 매핑된 상위 하드웨어 큐. hctx->debugfs_dir을 부모 디렉터리
 *        기준점으로 사용한다.
 * @ctx: debugfs 노드를 만들 대상 blk_mq_ctx. ctx->cpu로 디렉터리 이름을
 *       짓는다.
 * @return: void.
 *
 * blk_mq_debugfs_register_hctx()가 이 hctx에 매핑된 모든 ctx(CPU)에 대해
 * 순회 호출한다. ctx->cpu에 대응하는 CPU가 바로 이 NVMe SQ로 I/O를
 * 제출하는 경로를 세밀하게 디버깅할 때 유용하며, 그 아래
 * blk_mq_debugfs_ctx_attrs[](default_rq_list/read_rq_list/poll_rq_list)가
 * 생성된다.
 * 실행 컨텍스트: blk_mq_debugfs_register_hctx() 호출 경로와 동일(디스크
 * 등록 또는 큐 재구성).
 * 호출자: blk_mq_debugfs_register_hctx().
 * 피호출자: snprintf(), debugfs_create_dir(), debugfs_create_files().
 * 에러 경로: debugfs_create_dir()가 실패하면(에러 포인터) 그 dentry를
 *   그대로 debugfs_create_files()에 넘기고, 그 함수 내부의
 *   IS_ERR_OR_NULL() 체크가 조용히 무시한다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register_hctx() -> [blk_mq_debugfs_register_ctx()]
 *     -> debugfs_create_files()
 */
static void blk_mq_debugfs_register_ctx(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_ctx *ctx)
{
	struct dentry *ctx_dir; /* [한국어] 새로 생성할 "cpu<N>" 디렉터리의 dentry */
	char name[20]; /* [한국어] "cpu"+CPU번호 문자열을 담을 버퍼(CPU 번호가 아무리 커도 20바이트면 충분) */

	snprintf(name, sizeof(name), "cpu%u", ctx->cpu); /* [한국어] 이 ctx가 대응하는 CPU 번호로 디렉터리 이름 구성 */
	ctx_dir = debugfs_create_dir(name, hctx->debugfs_dir); /* [한국어] hctx 디렉터리 아래 "cpu<N>" 서브 디렉터리 생성 */

	debugfs_create_files(hctx->queue, ctx_dir, ctx, /* [한국어] 이 ctx 전용 파일들(default/read/poll_rq_list) 생성 */
			     blk_mq_debugfs_ctx_attrs);
}

/*
 * [한국어]
 * blk_mq_debugfs_register_hctx - 단일 blk_mq_hw_ctx(NVMe SQ에 대응하는
 *   하드웨어 디스패치 큐)의 debugfs 디렉터리(mq/hctx<N>/)와 그 하위 상태
 *   파일들, 그리고 그 아래 모든 per-CPU ctx 디렉터리까지 생성한다.
 *
 * @q: hctx가 소속된 request_queue. 상위 디렉터리(q->debugfs_dir)를 기준점
 *     으로 사용한다.
 * @hctx: debugfs 노드를 생성할 대상 하드웨어 큐. 생성된 디렉터리 포인터는
 *        hctx->debugfs_dir 필드에 저장되어 이후 조회/해제 시 재사용된다.
 * @return: void. q->debugfs_dir이 아직 없으면 아무 것도 하지 않고 반환.
 *
 * blk_mq_debugfs_register()가 큐의 모든 hctx를 순회하며 호출하거나,
 * nr_hw_queues가 런타임에 바뀌는 경우 blk_mq_debugfs_register_hctxs()가
 * 새로 생긴 hctx에 대해서만 개별 호출한다. NVMe에서 인터럽트 벡터/CPU
 * 토폴로지 변경으로 하드웨어 큐 개수가 재조정될 때 이 경로를 탄다.
 * blk_mq_debugfs_hctx_attrs[](state/flags/dispatch/busy/ctx_map/tags/
 * tags_bitmap/sched_tags/sched_tags_bitmap/active/dispatch_busy/type)를
 * 채운 뒤, hctx에 매핑된 모든 ctx(CPU)마다 blk_mq_debugfs_register_ctx()
 * 를 호출해 "cpu<N>/" 서브 디렉터리까지 완성한다.
 * 실행 컨텍스트: 디스크 등록 또는 큐 개수 재조정 경로에서 동기적으로
 * 실행된다.
 * 호출자: blk_mq_debugfs_register(), blk_mq_debugfs_register_hctxs().
 * 피호출자: snprintf(), debugfs_create_dir(), debugfs_create_files(),
 *   hctx_for_each_ctx(매크로 순회), blk_mq_debugfs_register_ctx().
 * 에러 경로: q->debugfs_dir이 없으면 조기 반환 - 해당 hctx의 디버깅
 *   노드만 비어 있게 된다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register() 또는 blk_mq_debugfs_register_hctxs()
 *     -> [blk_mq_debugfs_register_hctx()] -> debugfs_create_files()
 *     -> blk_mq_debugfs_register_ctx() (ctx마다)
 */
void blk_mq_debugfs_register_hctx(struct request_queue *q,
				  struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_ctx *ctx; /* [한국어] hctx_for_each_ctx() 순회용 ctx 포인터 */
	char name[20]; /* [한국어] "hctx<N>" 디렉터리 이름 버퍼 */
	int i; /* [한국어] hctx_for_each_ctx()가 쓰는 순회 인덱스 */

	if (!q->debugfs_dir) /* [한국어] 상위 큐 디렉터리가 아직 없으면(예: debugfs 미마운트) */
		return; /* [한국어] 등록 불가 - 조용히 반환 */

	snprintf(name, sizeof(name), "hctx%u", hctx->queue_num); /* [한국어] hctx 순번으로 디렉터리 이름 구성 - NVMe SQ 번호에 대응하는 식별자 */
	hctx->debugfs_dir = debugfs_create_dir(name, q->debugfs_dir); /* [한국어] 큐 디렉터리 아래 이 hctx 전용 서브 디렉터리 생성, 포인터를 hctx에 캐시 */

	debugfs_create_files(q, hctx->debugfs_dir, hctx, /* [한국어] 이 hctx 전용 상태 파일들(state/tags/busy/active 등) 생성 */
			     blk_mq_debugfs_hctx_attrs);

	hctx_for_each_ctx(hctx, ctx, i) /* [한국어] 이 hctx에 매핑된 모든 CPU(ctx)를 순회 */
		blk_mq_debugfs_register_ctx(hctx, ctx); /* [한국어] CPU별 "cpu<N>/" 서브 디렉터리 등록 */
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_hctx - 특정 hctx에 대해 만들어 두었던 debugfs
 *   디렉터리 전체(state/tags/dispatch 등 하위 파일과 cpu<N>/ 서브 디렉터리
 *   포함)를 재귀적으로 제거한다.
 *
 * @hctx: 제거 대상 하드웨어 큐. hctx->debugfs_dir을 재귀 삭제한 뒤, 관련
 *        필드를 NULL로 되돌려 중복 해제를 막는다.
 * @return: void. 상위 큐의 debugfs_dir 자체가 없으면(이미 전체가 정리된
 *          상태) 아무 것도 하지 않는다.
 *
 * NVMe 컨트롤러가 리셋되거나 하드웨어 큐 개수가 줄어들 때, 더 이상
 * 존재하지 않는 hctx의 debugfs 항목이 stale 상태로 남아 사용자에게
 * 잘못된 정보(이미 해제된 hctx를 가리키는 파일)를 보여주는 것을 막기
 * 위해 필요하다. hctx->sched_debugfs_dir까지 함께 NULL로 지우는 이유는,
 * hctx->debugfs_dir을 재귀 삭제하면 그 하위에 있던 sched_debugfs_dir도
 * 함께 사라지므로 댕글링 포인터를 남기지 않기 위함이다.
 * 실행 컨텍스트: 큐 재구성/디스크 제거 경로에서 동기적으로 실행되며,
 * 이 함수가 실행될 때 해당 hctx는 더 이상 I/O 디스패치에 쓰이지 않는
 * 상태여야 한다(그렇지 않으면 디렉터리 삭제 도중 show 콜백이 해제 중인
 * hctx를 참조할 위험이 있다).
 * 호출자: blk_mq_debugfs_unregister_hctxs().
 * 피호출자: debugfs_remove_recursive().
 * 에러 경로: hctx->queue->debugfs_dir이 이미 없으면 no-op으로 처리되어
 *   중복 호출에 안전하다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_unregister_hctxs() -> [blk_mq_debugfs_unregister_hctx()]
 *     -> debugfs_remove_recursive()
 */
void blk_mq_debugfs_unregister_hctx(struct blk_mq_hw_ctx *hctx)
{
	if (!hctx->queue->debugfs_dir) /* [한국어] 상위 큐의 debugfs 트리 자체가 없으면(이미 전체 정리됨 등) */
		return; /* [한국어] 할 일이 없으므로 조용히 반환 */
	debugfs_remove_recursive(hctx->debugfs_dir); /* [한국어] 이 hctx 디렉터리와 그 하위(cpu<N>/, state/tags/dispatch 등) 전체 재귀 삭제 */
	hctx->sched_debugfs_dir = NULL; /* [한국어] 방금 재귀 삭제로 함께 사라진 sched 서브디렉터리 참조를 NULL로 정리 - 댕글링 포인터 방지 */
	hctx->debugfs_dir = NULL; /* [한국어] hctx debugfs_dir 참조를 NULL로 정리 - 재등록/중복 해제 판별에 사용 */
}

/*
 * [한국어]
 * blk_mq_debugfs_register_hctxs - request_queue에 속한 모든 하드웨어
 *   큐(hctx)에 대해 blk_mq_debugfs_register_hctx()를 일괄 호출한다.
 *
 * @q: 대상 request_queue.
 * @return: void.
 *
 * NVMe 드라이버가 CPU 토폴로지 변경이나 인터럽트 벡터 재분배로 하드웨어
 * 큐 개수(nr_hw_queues)를 바꾸는 경우, block/blk-mq.c의 큐 재구성 경로가
 * 새 hctx 배열 전체에 대해 debugfs를 다시 채워야 한다. blk_debugfs_lock()
 * 으로 q->debugfs_mutex를 획득해 등록 도중 다른 스레드가 동시에
 * debugfs 트리를 건드리지 못하게 막고, memflags에 저장해 짝인
 * blk_debugfs_unlock()에 그대로 넘긴다(메모리 할당 플래그를 잠금 구간에
 * 맞게 일시적으로 바꿨다가 복원하는 헬퍼 패턴).
 * blk_mq_debugfs_register_hctx() 내부에서 이미 debugfs_dir이 있는 hctx는
 * 건너뛰므로, 이 함수는 새로 생긴 hctx만 새로 등록하는 결과가 된다.
 * 실행 컨텍스트: 큐가 freeze된 상태이거나 재구성 락을 쥔 상태에서
 * 호출되는 것이 일반적이다(block/blk-mq.c 호출부 참고).
 * 호출자: block/blk-mq.c(하드웨어 큐 개수 재조정 경로).
 * 피호출자: blk_debugfs_lock(), blk_mq_debugfs_register_hctx()(hctx마다),
 *   blk_debugfs_unlock().
 * 에러 경로: 개별 hctx 등록 실패는 blk_mq_debugfs_register_hctx() 쪽에서
 *   조용히 무시되므로 이 함수도 별도 에러를 반환하지 않는다.
 *
 * 호출 체인:
 *   block/blk-mq.c(큐 재구성) -> [blk_mq_debugfs_register_hctxs()]
 *     -> blk_mq_debugfs_register_hctx() (hctx마다)
 */
void blk_mq_debugfs_register_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx; /* [한국어] queue_for_each_hw_ctx() 순회용 hctx 포인터 */
	unsigned int memflags; /* [한국어] blk_debugfs_lock()이 반환하는, 잠금 구간 동안의 메모리 할당 플래그(GFP) 조정값 - unlock 시 복원용으로 보관 */
	unsigned long i; /* [한국어] queue_for_each_hw_ctx() 순회 인덱스 */

	memflags = blk_debugfs_lock(q); /* [한국어] q->debugfs_mutex 잠금 - 등록 도중 트리 구조 변경 직렬화 */
	queue_for_each_hw_ctx(q, hctx, i) /* [한국어] 모든 hctx(SQ)를 순회하며 */
		blk_mq_debugfs_register_hctx(q, hctx); /* [한국어] 아직 없는 hctx만 새로 등록(함수 내부에서 중복 판별) */
	blk_debugfs_unlock(q, memflags); /* [한국어] debugfs_mutex 잠금 해제 + memflags로 GFP 상태 복원 */
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_hctxs - request_queue에 속한 모든 hctx의
 *   debugfs 노드를 일괄 제거한다.
 *
 * @q: 대상 request_queue.
 * @return: void.
 *
 * 하드웨어 큐 개수를 줄이기 전에 기존 hctx 배열 전체의 debugfs 트리를
 * 깨끗이 비워야, 이후 blk_mq_debugfs_register_hctxs()가 새 배열에 맞춰
 * 다시 생성할 때 이름 충돌이나 stale 디렉터리가 남지 않는다.
 * block/blk-mq.c의 큐 재구성 코드가 nr_hw_queues 변경 직전에 이 함수를
 * 호출한다.
 * 실행 컨텍스트: 큐 재구성 락을 보유한 상태에서 동기적으로 실행된다.
 * 호출자: block/blk-mq.c(하드웨어 큐 개수 재조정 경로의 시작 지점).
 * 피호출자: blk_mq_debugfs_unregister_hctx()(hctx마다).
 * 에러 경로: 없음(각 hctx 해제가 개별적으로 안전하게 no-op 처리됨).
 *
 * 호출 체인:
 *   block/blk-mq.c(큐 재구성 시작) -> [blk_mq_debugfs_unregister_hctxs()]
 *     -> blk_mq_debugfs_unregister_hctx() (hctx마다)
 */
void blk_mq_debugfs_unregister_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx; /* [한국어] queue_for_each_hw_ctx() 순회용 hctx 포인터 */
	unsigned long i; /* [한국어] 순회 인덱스 */

	queue_for_each_hw_ctx(q, hctx, i) /* [한국어] 모든 hctx(SQ)를 순회하며 */
		blk_mq_debugfs_unregister_hctx(hctx); /* [한국어] 각 hctx의 debugfs 디렉터리를 재귀 삭제 */
}

/*
 * [한국어]
 * blk_mq_debugfs_register_sched - I/O 스케줄러(mq-deadline/bfq/kyber)
 *   단위의 debugfs 노드를 "sched/" 서브 디렉터리에 등록한다.
 *
 * @q: 스케줄러가 붙어 있는 request_queue. q->elevator가 가리키는
 *     스케줄러가 자신의 debugfs attr 배열(queue_debugfs_attrs)을
 *     제공하면 그것을 등록한다.
 * @return: void.
 *
 * 사용자가 /sys/block/<disk>/queue/scheduler에 값을 써서 I/O 스케줄러를
 * 교체(elevator switch)하면, 이전 스케줄러의 debugfs 노드는 제거되고
 * (blk_mq_debugfs_unregister_sched()) 새 스케줄러의 노드가 새로
 * 생성되어야 한다. 이 함수가 그 등록 절반을 담당한다. NVMe 멀티 큐
 * 환경에서 SQ로 보내기 전 merge/batch 여부를 좌우하는 스케줄러 내부
 * 상태(예: mq-deadline의 fifo list, bfq의 서비스 트리)를 확인하는
 * 진입점이 된다. e->queue_debugfs_attrs가 없으면(스케줄러가 debugfs를
 * 지원하지 않으면) 아무 것도 만들지 않고 조용히 반환한다.
 * 실행 컨텍스트: elevator switch 경로(block/blk-mq-sched.c)에서
 * q->elevator_lock을 보유한 상태로 호출된다. lockdep_assert_held()로
 * debugfs_mutex 보유도 함께 강제한다.
 * 호출자: block/blk-mq-sched.c의 elevator switch 처리 함수.
 * 피호출자: lockdep_assert_held(), debugfs_create_dir(),
 *   debugfs_create_files().
 * 에러 경로: q->debugfs_dir이 아직 없으면(디스크 등록이 더 늦게 완료되는
 *   레이스) 나중에 다시 호출될 것을 기대하고 조기 반환한다.
 *
 * 호출 체인:
 *   elevator switch(block/blk-mq-sched.c) -> [blk_mq_debugfs_register_sched()]
 *     -> debugfs_create_files(q->elevator의 큐 단위 debugfs attr)
 */
void blk_mq_debugfs_register_sched(struct request_queue *q)
{
	struct elevator_type *e = q->elevator->type; /* [한국어] 현재 붙어 있는 I/O 스케줄러의 타입 디스크립터(mq-deadline/bfq/kyber 등) */

	lockdep_assert_held(&q->debugfs_mutex); /* [한국어] debugfs_mutex 보유 여부를 정적/동적으로 검증 - 등록 경로 직렬화 전제 확인 */

	/*
	 * If the parent directory has not been created yet, return, we will be
	 * called again later on and the directory/files will be created then.
	 */
	if (!q->debugfs_dir) /* [한국어] 상위 큐 디렉터리가 아직 만들어지기 전이면(디스크 등록 레이스) */
		return; /* [한국어] 나중에 다시 호출될 것이므로 지금은 조용히 반환 */

	if (!e->queue_debugfs_attrs) /* [한국어] 이 스케줄러가 큐 단위 debugfs attr 테이블을 제공하지 않으면 */
		return; /* [한국어] 만들 파일이 없으므로 반환 */

	q->sched_debugfs_dir = debugfs_create_dir("sched", q->debugfs_dir); /* [한국어] 큐 디렉터리 아래 "sched" 서브 디렉터리 생성, 포인터 캐시 */

	debugfs_create_files(q, q->sched_debugfs_dir, q, e->queue_debugfs_attrs); /* [한국어] 스케줄러가 제공한 attr 테이블로 실제 파일들 생성 */
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_sched - 큐에 등록되어 있던 I/O 스케줄러
 *   debugfs 노드("sched/" 서브 디렉터리 전체)를 제거한다.
 *
 * @q: 대상 request_queue.
 * @return: void.
 *
 * elevator switch로 스케줄러를 교체하거나 스케줄러 없는 none 모드로
 * 전환할 때, 이전 스케줄러가 등록해 둔 debugfs 파일들이 이미 해제된
 * 스케줄러 내부 상태를 가리키는 stale 상태로 남지 않도록 제거한다.
 * 실행 컨텍스트: elevator switch 경로에서 q->elevator_lock을 보유한 채
 * 호출된다.
 * 호출자: block/blk-mq-sched.c(elevator switch의 이전 스케줄러 해제
 *   단계).
 * 피호출자: lockdep_assert_held(), debugfs_remove_recursive().
 * 에러 경로: q->sched_debugfs_dir이 이미 NULL이어도
 *   debugfs_remove_recursive(NULL)는 안전하게 no-op이다.
 *
 * 호출 체인:
 *   elevator switch(해제 단계) -> [blk_mq_debugfs_unregister_sched()]
 */
void blk_mq_debugfs_unregister_sched(struct request_queue *q)
{
	lockdep_assert_held(&q->debugfs_mutex); /* [한국어] debugfs_mutex 보유 여부 검증 */

	debugfs_remove_recursive(q->sched_debugfs_dir); /* [한국어] "sched/" 디렉터리와 그 하위 파일 전체 재귀 삭제 */
	q->sched_debugfs_dir = NULL; /* [한국어] 참조를 NULL로 정리 - 중복 해제/재등록 판별에 사용 */
}

/*
 * [한국어]
 * rq_qos_id_to_name - enum rq_qos_id(rq-qos 플러그인 종류 식별자)를
 *   debugfs 서브 디렉터리 이름 문자열로 변환.
 *
 * @id: 변환할 rq_qos ID(RQ_QOS_WBT/LATENCY/COST 중 하나).
 * @return: "wbt"/"latency"/"cost" 중 하나, 알 수 없는 값이면 "unknown".
 *
 * rq-qos는 blk-mq 디스패치 앞단에서 조건에 따라 request를 지연/제한하는
 * 플러그인 프레임워크로, NVMe SSD에서는 WBT(Writeback Throttling)가
 * 쓰기 폭주 시 큐 depth를 소프트웨어적으로 줄여 읽기 지연을 보호하는
 * 용도로 흔히 쓰인다. LATENCY/COST는 각각 목표 지연시간 기반, cgroup
 * I/O 비용 기반 제어 정책이다. 이 이름이 그대로
 * /sys/kernel/debug/block/<disk>/rqos/<name>/ 디렉터리명이 된다.
 * 실행 컨텍스트: blk_mq_debugfs_register_rqos() 호출 경로와 동일.
 * 호출자: blk_mq_debugfs_register_rqos().
 * 피호출자: 없음(단순 switch-case).
 * 에러 경로: 알 수 없는 id는 "unknown"으로 대체해 디렉터리 이름 충돌 없이
 *   안전하게 처리.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register_rqos() -> [rq_qos_id_to_name()]
 */
static const char *rq_qos_id_to_name(enum rq_qos_id id)
{
	switch (id) { /* [한국어] rq_qos ID별로 디렉터리 이름 문자열 결정 */
	case RQ_QOS_WBT: /* [한국어] Writeback Throttling - 쓰기 폭주 시 큐 depth를 줄여 읽기 지연 보호 */
		return "wbt";
	case RQ_QOS_LATENCY: /* [한국어] 목표 지연시간(latency target) 기반 스로틀 정책 */
		return "latency";
	case RQ_QOS_COST: /* [한국어] cgroup I/O 비용(iocost) 기반 스로틀 정책 */
		return "cost";
	}
	return "unknown"; /* [한국어] 알려지지 않은 ID - 방어적 기본값 */
}

/*
 * [한국어]
 * blk_mq_debugfs_register_rqos - 단일 rq_qos 인스턴스의 debugfs
 *   디렉터리를 "rqos/<name>/" 아래 생성한다.
 *
 * @rqos: 등록할 rq_qos 인스턴스(WBT/latency/cost 정책 중 하나).
 * @return: void.
 *
 * q->rqos_debugfs_dir("rqos/" 상위 디렉터리)가 아직 없으면 이 함수가
 * 최초 호출 시 만들고, 이후 호출에서는 재사용한다. rqos->debugfs_dir이
 * 이미 있거나(중복 등록 방지) 이 정책이 debugfs_attrs를 제공하지 않으면
 * (예: 일부 rq-qos 정책은 debugfs 노출을 지원하지 않을 수 있음) 아무
 * 것도 하지 않는다. NVMe 디스크에서 WBT가 활성화되어 있으면 이 경로로
 * /sys/kernel/debug/block/<disk>/rqos/wbt/ 가 만들어져 현재 스로틀
 * 상태(대기 큐 길이, 목표 latency 등)를 확인할 수 있다.
 * 실행 컨텍스트: blk_mq_debugfs_register_rq_qos() 호출 경로와 동일,
 * q->debugfs_mutex를 보유한 상태로 호출된다.
 * 호출자: blk_mq_debugfs_register_rq_qos()(rq_qos 체인의 각 노드마다).
 * 피호출자: lockdep_assert_held(), debugfs_create_dir(),
 *   debugfs_create_files().
 * 에러 경로: 이미 등록됐거나 debugfs_attrs가 없으면 조기 반환.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register_rq_qos() -> [blk_mq_debugfs_register_rqos()]
 *     -> debugfs_create_files()
 */
static void blk_mq_debugfs_register_rqos(struct rq_qos *rqos)
{
	struct request_queue *q = rqos->disk->queue; /* [한국어] 이 rqos가 부착된 디스크의 request_queue - 상위 debugfs 디렉터리 기준점 */
	const char *dir_name = rq_qos_id_to_name(rqos->id); /* [한국어] rqos 종류(wbt/latency/cost)에 대응하는 디렉터리 이름 */

	lockdep_assert_held(&q->debugfs_mutex); /* [한국어] debugfs_mutex 보유 여부 검증 */

	if (rqos->debugfs_dir || !rqos->ops->debugfs_attrs) /* [한국어] 이미 등록됐거나(중복 방지) 이 정책이 debugfs attr을 제공하지 않으면 */
		return; /* [한국어] 아무 것도 만들지 않고 반환 */

	if (!q->rqos_debugfs_dir) /* [한국어] "rqos/" 상위 디렉터리가 아직 없으면(이 큐의 첫 rq_qos 등록) */
		q->rqos_debugfs_dir = debugfs_create_dir("rqos", /* [한국어] "rqos" 디렉터리를 최초 1회 생성 */
							 q->debugfs_dir);

	rqos->debugfs_dir = debugfs_create_dir(dir_name, q->rqos_debugfs_dir); /* [한국어] "rqos/" 아래 이 정책 전용 서브 디렉터리(wbt/latency/cost) 생성 */
	debugfs_create_files(q, rqos->debugfs_dir, rqos, /* [한국어] 이 정책의 attr 테이블로 실제 상태 파일 생성 */
			     rqos->ops->debugfs_attrs);
}

/*
 * [한국어]
 * blk_mq_debugfs_register_rq_qos - request_queue에 연결된 모든 rq_qos
 *   인스턴스를 순회하며 debugfs에 등록한다.
 *
 * @q: 대상 request_queue. q->rq_qos가 가리키는 연결 리스트(단일 연결,
 *     rqos->next로 다음 노드 접근)를 순회한다.
 * @return: void.
 *
 * WBT(wbt_init())가 큐에 처음 붙거나, 이미 등록된 rq_qos 체인이 있는
 * 상태에서 blk_mq_debugfs_register()가 큐 최초 등록 시 이 함수를 호출해
 * 그 시점까지 붙어 있는 모든 정책의 debugfs 노드를 한 번에 채운다.
 * NVMe QoS 정책(WBT, latency)이 CID 생성/완료 속도를 조절할 수 있으며,
 * 이 디렉터리에서 해당 정책의 런타임 통계를 확인한다.
 * 실행 컨텍스트: WBT/rq-qos 초기화 또는 파라미터 변경 경로에서
 * q->rq_qos_mutex... 가 아니라 q->debugfs_mutex를 보유한 상태로 호출된다
 * (lockdep_assert_held로 검증).
 * 호출자: blk_mq_debugfs_register()(큐 최초 등록 시), block/blk-wbt.c의
 *   wbt_init() 및 WBT 파라미터 변경 함수들.
 * 피호출자: lockdep_assert_held(), blk_mq_debugfs_register_rqos()(체인의
 *   각 노드마다).
 * 에러 경로: q->rq_qos가 NULL(등록된 rq-qos 정책이 없음)이면 아무 파일도
 *   생성하지 않는다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register() 또는 wbt_init()/WBT 파라미터 변경
 *     -> [blk_mq_debugfs_register_rq_qos()] -> blk_mq_debugfs_register_rqos()
 */
void blk_mq_debugfs_register_rq_qos(struct request_queue *q)
{
	lockdep_assert_held(&q->debugfs_mutex); /* [한국어] debugfs_mutex 보유 여부 검증 */

	if (q->rq_qos) { /* [한국어] 이 큐에 하나 이상의 rq_qos 정책이 연결되어 있으면 */
		struct rq_qos *rqos = q->rq_qos; /* [한국어] 연결 리스트의 첫 번째 rq_qos 노드 */

		while (rqos) { /* [한국어] 리스트 끝(NULL)까지 순회 */
			blk_mq_debugfs_register_rqos(rqos); /* [한국어] 이 노드의 debugfs 디렉터리/파일 등록 */
			rqos = rqos->next; /* [한국어] 다음 rq_qos 노드로 이동 */
		}
	}
}

/*
 * [한국어]
 * blk_mq_debugfs_register_sched_hctx - 특정 hctx에 대한 스케줄러별
 *   debugfs 노드(예: mq-deadline의 hctx당 fifo 상태)를 "hctx<N>/sched/"
 *   아래 등록한다.
 *
 * @q: 스케줄러가 붙어 있는 request_queue. q->elevator->type에서 hctx별
 *     debugfs attr 테이블(hctx_debugfs_attrs)을 얻는다.
 * @hctx: 대상 하드웨어 큐. hctx의 스케줄러 전용 데이터가 이미 초기화되어
 *        있어야 스케줄러가 자신의 hctx별 상태를 노출할 수 있다.
 * @return: void.
 *
 * 스케줄러 전체 단위(blk_mq_debugfs_register_sched(), "sched/" 큐 단위
 * 디렉터리)와 별개로, 스케줄러가 hctx마다 별도의 상태(예: 해당 SQ에
 * 대기 중인 fifo 순서)를 가질 때 이 함수가 그 hctx 전용 서브 디렉터리를
 * 채운다. NVMe 멀티 큐 환경에서 SQ별로 스케줄링 정책이 어떻게 동작하는지
 * 세밀하게 관찰할 때 쓰인다.
 * 실행 컨텍스트: elevator switch 경로에서 hctx를 순회하는 도중 호출되며,
 * q->debugfs_mutex를 보유한 상태여야 한다.
 * 호출자: block/blk-mq-sched.c(elevator switch의 hctx 순회 루프).
 * 피호출자: lockdep_assert_held(), debugfs_create_dir(),
 *   debugfs_create_files().
 * 에러 경로: hctx->debugfs_dir이 아직 없거나(디스크 등록이 늦게 완료되는
 *   레이스, blk_register_queue()가 나중에 적절한 부모 디렉터리로 다시
 *   호출해 줄 것을 기대) e->hctx_debugfs_attrs가 없으면 조기 반환.
 *
 * 호출 체인:
 *   elevator switch(hctx 순회) -> [blk_mq_debugfs_register_sched_hctx()]
 *     -> debugfs_create_files()
 */
void blk_mq_debugfs_register_sched_hctx(struct request_queue *q,
					struct blk_mq_hw_ctx *hctx)
{
	struct elevator_type *e = q->elevator->type; /* [한국어] 현재 붙어 있는 I/O 스케줄러의 타입 디스크립터 */

	lockdep_assert_held(&q->debugfs_mutex); /* [한국어] debugfs_mutex 보유 여부 검증 */

	/*
	 * If the parent debugfs directory has not been created yet, return;
	 * We will be called again later on with appropriate parent debugfs
	 * directory from blk_register_queue()
	 */
	if (!hctx->debugfs_dir) /* [한국어] 이 hctx의 상위 debugfs 디렉터리가 아직 없으면(등록 순서 레이스) */
		return; /* [한국어] blk_register_queue()가 나중에 다시 호출해 줄 것을 기대하고 조기 반환 */

	if (!e->hctx_debugfs_attrs) /* [한국어] 이 스케줄러가 hctx 단위 debugfs attr을 제공하지 않으면 */
		return; /* [한국어] 만들 파일이 없으므로 반환 */

	hctx->sched_debugfs_dir = debugfs_create_dir("sched", /* [한국어] hctx 디렉터리 아래 "sched" 서브 디렉터리 생성, 포인터 캐시 */
						     hctx->debugfs_dir);
	debugfs_create_files(q, hctx->sched_debugfs_dir, hctx, /* [한국어] 스케줄러가 제공한 hctx 단위 attr 테이블로 실제 파일 생성 */
			     e->hctx_debugfs_attrs);
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_sched_hctx - 특정 hctx의 스케줄러별 debugfs
 *   노드("hctx<N>/sched/" 서브 디렉터리 전체)를 제거한다.
 *
 * @hctx: 대상 하드웨어 큐.
 * @return: void.
 *
 * blk_mq_debugfs_register_sched_hctx()로 만든 hctx별 스케줄러 노드를
 * 스케줄러 교체/해제 시 되돌리는 짝 함수다.
 * 실행 컨텍스트: elevator switch 해제 경로에서 hctx를 순회하는 도중
 * 호출되며, hctx->queue->debugfs_mutex를 보유한 상태여야 한다.
 * 호출자: block/blk-mq-sched.c(elevator switch 해제 단계의 hctx 순회
 *   루프).
 * 피호출자: lockdep_assert_held(), debugfs_remove_recursive().
 * 에러 경로: hctx->queue->debugfs_dir이 없으면(전체 트리가 이미 정리됨)
 *   조기 반환.
 *
 * 호출 체인:
 *   elevator switch(해제, hctx 순회) -> [blk_mq_debugfs_unregister_sched_hctx()]
 */
void blk_mq_debugfs_unregister_sched_hctx(struct blk_mq_hw_ctx *hctx)
{
	lockdep_assert_held(&hctx->queue->debugfs_mutex); /* [한국어] debugfs_mutex 보유 여부 검증 */

	if (!hctx->queue->debugfs_dir) /* [한국어] 상위 큐의 debugfs 트리 자체가 없으면(이미 전체 정리됨) */
		return; /* [한국어] 할 일이 없으므로 반환 */
	debugfs_remove_recursive(hctx->sched_debugfs_dir); /* [한국어] hctx의 "sched/" 서브 디렉터리와 그 하위 파일 전체 재귀 삭제 */
	hctx->sched_debugfs_dir = NULL; /* [한국어] 참조를 NULL로 정리 - 중복 해제/재등록 판별에 사용 */
}
