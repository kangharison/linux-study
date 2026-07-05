/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] blk-mq debugfs 인터페이스 선언 헤더 (block/blk-mq-debugfs.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 blk-mq(멀티큐 블록 계층)의 런타임 상태를
 * /sys/kernel/debug/block/<disk>/ 아래에 seq_file 기반 텍스트 파일로 노출하는
 * debugfs 인터페이스의 "선언부"다. 실제 파일 생성/출력 로직은
 * block/blk-mq-debugfs.c에 구현되어 있고, 이 헤더는 그 구현이 block/blk-mq.c,
 * block/blk-mq-sched.c, block/blk-wbt.c, block/blk-zoned.c, block/mq-deadline.c,
 * block/kyber-iosched.c 등 다른 컴파일 단위에서 호출될 수 있도록 함수
 * 프로토타입과 struct blk_mq_debugfs_attr 타입을 공개한다.
 * CONFIG_BLK_DEBUG_FS가 꺼진 커널에서도 호출부 코드가 매크로 분기 없이
 * 동일한 함수 이름을 쓸 수 있도록, 커널 옵션이 꺼졌을 때를 위한 no-op
 * static inline 스텁까지 이 파일 하나에서 함께 제공한다.
 * 요약하면 "debugfs로 blk-mq/hctx/스케줄러/rq-qos 상태를 보여주는 기능의
 * ON/OFF 여부와 무관하게 안전하게 호출 가능한 API 표면"을 정의하는 파일이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 흐름 관점에서 이 헤더가 선언하는 함수들은 blk-mq의 "정상 I/O 경로"가
 * 아니라 그 옆에 붙는 관찰/디버깅 경로에 속한다. 정상 경로는
 * submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_tag() -> ...
 * -> nvme_queue_rq() -> doorbell 갱신 순으로 흐르지만, 이 헤더의 함수들은
 * 그 경로가 만들어내는 자료구조(request_queue, blk_mq_hw_ctx, blk_mq_ctx,
 * rq_qos, elevator)의 "현재 스냅샷"을 사용자 공간에 문자열로 보여주는
 * 별도의 얕은 경로다. 호출 체인은 두 갈래로 나뉜다.
 *   (1) 등록/해제 체인: 디스크 등록(add_disk -> blk_register_queue),
 *       하드웨어 큐 재구성(block/blk-mq.c), 스케줄러 교체(elevator switch,
 *       block/blk-mq-sched.c), rq-qos(WBT) 초기화(block/blk-wbt.c)가
 *       각각 blk_mq_debugfs_register*()/unregister*()를 호출해 debugfs
 *       트리를 만들고 없앤다.
 *   (2) 열람 체인: 사용자가 debugfs 파일을 open/read하면 VFS ->
 *       block/blk-mq-debugfs.c의 공용 file_operations -> 이 헤더가 선언한
 *       show/seq_ops 콜백(__blk_mq_debugfs_rq_show, blk_mq_debugfs_rq_show,
 *       queue_zone_wplugs_show 등)이 실행되어 실시간 상태를 문자열로 만든다.
 * 실행 컨텍스트는 등록/해제 체인은 드라이버 probe/재구성 스레드,
 * 열람 체인은 debugfs 파일을 읽는 유저 프로세스의 syscall 컨텍스트다.
 * 인터럽트나 소프트IRQ 컨텍스트에서는 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈: block/blk-mq.h(struct request_queue, struct blk_mq_hw_ctx의
 * 실제 정의), linux/seq_file.h(seq_file/seq_operations), linux/debugfs.h
 * (실 구현부인 .c 파일이 사용). 이 헤더 자체는 전방 선언(struct blk_mq_hw_ctx)
 * 과 포인터 타입만 사용하므로 완전한 정의를 include하지 않는다.
 * 이 헤더에 의존하는 모듈: block/blk-sysfs.c(디스크 등록 시
 * blk_mq_debugfs_register 호출), block/blk-mq.c(하드웨어 큐 개수 재조정 시
 * *_hctxs() 호출), block/blk-mq-sched.c(elevator switch 시 *_sched*() 호출),
 * block/blk-wbt.c(WBT rq-qos 초기화/파라미터 변경 시 *_rq_qos() 호출),
 * block/mq-deadline.c와 block/kyber-iosched.c(자신의 debugfs seq_ops에서
 * 공용 .show=blk_mq_debugfs_rq_show 및 __blk_mq_debugfs_rq_show()를 재사용),
 * block/blk-zoned.c(queue_zone_wplugs_show()의 실제 구현 제공).
 * 데이터 흐름: request_queue/blk_mq_hw_ctx/blk_mq_ctx/rq_qos/elevator가
 * 보유한 실시간 필드(tags 비트맵, dispatch 리스트, WBT 상태, zone write
 * plug 등) -> 이 헤더가 선언한 show/seq_ops 콜백이 읽어 seq_printf()로
 * 문자열화 -> debugfs VFS 계층을 통해 사용자 공간 read(2) 결과로 전달.
 * 데이터는 커널 -> 사용자 방향으로만 흐르며(대부분 0400/read-only), "state"
 * 등 일부만 .write 콜백을 통해 사용자 -> 커널 방향으로 제어 신호를 받는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct blk_mq_debugfs_attr: debugfs 파일 1개(이름/권한/show 또는
 *   seq_ops/write)를 기술하는 테이블 엔트리 타입. block/blk-mq-debugfs.c의
 *   여러 정적 배열(blk_mq_debugfs_queue_attrs[], blk_mq_debugfs_hctx_attrs[],
 *   blk_mq_debugfs_ctx_attrs[] 등)이 이 타입의 배열로 정의된다.
 * - blk_mq_debugfs_register()/unregister 계열: request_queue 단위로
 *   debugfs 트리 전체(hctx 서브 디렉터리 포함)를 등록/해제한다.
 * - blk_mq_debugfs_register_hctx()/unregister_hctx(): 하드웨어 큐(hctx)
 *   1개 단위로 debugfs 서브 디렉터리를 등록/해제한다. NVMe에서는 SQ 1개에
 *   대응하는 디버깅 단위다.
 * - blk_mq_debugfs_register_sched*()/unregister_sched*(): I/O 스케줄러
 *   (mq-deadline/bfq/kyber)가 elevator switch 시 자신의 debugfs 노드를
 *   등록/해제하는 훅.
 * - blk_mq_debugfs_register_rq_qos(): WBT 등 rq-qos 플러그인의 상태를
 *   debugfs로 노출한다.
 * - __blk_mq_debugfs_rq_show()/blk_mq_debugfs_rq_show(): 단일 request의
 *   상세 필드를 seq_file에 출력하는 공용 헬퍼. blk-mq core뿐 아니라
 *   mq-deadline/kyber의 자체 debugfs 파일도 재사용한다.
 * - queue_zone_wplugs_show(): ZNS(Zoned Namespace) 등 zoned block
 *   device의 zone별 write plug 상태를 출력한다(CONFIG_BLK_DEV_ZONED 필요).
 */

#ifndef INT_BLK_MQ_DEBUGFS_H /* 컴파일 중복 포함 방지; NVMe 드라이버가 blk-mq.h와 함께 이 헤더를 참조할 때 매크로 중복 정의를 막는다 */
#define INT_BLK_MQ_DEBUGFS_H /* 위 매크로를 정의해 재포함 시 아래 선언이 다시 파싱되지 않도록 가드 */

#ifdef CONFIG_BLK_DEBUG_FS /* 이 분기가 참이어야 NVMe 큐 상태를 /sys/kernel/debug/block 경로로 노출할 수 있다 */

/*
 * [한국어] #ifdef CONFIG_BLK_DEBUG_FS 분기 이유:
 * debugfs 자체가 커널 디버깅/개발용 파일시스템이라 프로덕션 빌드에서는
 * 꺼 두는 경우가 많고, 이 파일이 선언하는 show/seq_ops 콜백들은 성능에
 * 민감한 I/O 경로에는 전혀 관여하지 않으면서 struct blk_mq_hw_ctx 등에
 * 필드를 추가로 요구하지 않는 "순수 부가 기능"이다. 따라서
 * CONFIG_BLK_DEBUG_FS가 켜진 경우에만 실제 등록/표시 로직(및 그것이
 * 필요로 하는 <linux/seq_file.h>)을 컴파일하고, 꺼진 경우에는 이 헤더의
 * #else 분기가 제공하는 빈 인라인 함수로 대체해 호출부(block/blk-sysfs.c,
 * block/blk-mq.c, block/blk-mq-sched.c, block/blk-wbt.c 등)가 #ifdef 없이
 * 그대로 blk_mq_debugfs_register(q) 등을 호출할 수 있게 한다.
 * NVMe 장치에서도 이 옵션 없이는 /sys/kernel/debug/block/<disk> 경로를
 * 통해 큐 상태를 볼 수 없고, 대신 아무 부작용 없는 no-op이 실행된다.
 */

#include <linux/seq_file.h> /* seq_file 기반 debugfs 출력에 사용; NVMe SQ/CQ 상태를 사용자 공간에 문자열로 전달할 때 seq_printf 등이 쓰인다 */

/*
 * [한국어]
 * struct blk_mq_hw_ctx - 전방 선언(forward declaration)만 존재.
 * 완전한 정의는 block/blk-mq.h에 있으며, 하나의 하드웨어 디스패치 큐
 * (HW queue)를 나타낸다. NVMe 드라이버에서는 흔히 nvme_queue 하나가 이
 * blk_mq_hw_ctx 하나와 1:1 또는 N:1로 매핑되어, SQ(Submission Queue) 단위의
 * outstanding 명령 수·tag 사용량을 이 구조체를 통해 추적할 수 있다.
 * 이 헤더는 포인터로만 이 타입을 참조(함수 인자)하므로 완전한 정의 없이
 * 전방 선언만으로 컴파일이 가능하며, 실제 필드 접근은
 * block/blk-mq-debugfs.c 안에서 "blk-mq.h"를 include한 뒤 이뤄진다.
 */
struct blk_mq_hw_ctx;

/*
 * [한국어]
 * struct blk_mq_debugfs_attr - debugfs 파일 하나를 기술하는 속성 테이블 엔트리.
 *
 * block/blk-mq-debugfs.c의 blk_mq_debugfs_queue_attrs[](큐 단위 파일),
 * blk_mq_debugfs_hctx_attrs[]("state"/"tags"/"sched_tags"/"dispatch"/
 * "busy" 등 hctx 단위 파일), blk_mq_debugfs_ctx_attrs[](CPU별 rq_list 파일)
 * 등은 모두 이 구조체의 배열로 정의되며, name이 NULL인 sentinel({})을
 * 만날 때까지 순회하며 debugfs_create_file_aux()로 실제 파일을 만든다
 * (block/blk-mq-debugfs.c의 debugfs_create_files() 참고).
 * NVMe 디버깅 관점에서는 이 배열의 각 엔트리 하나하나가 특정 hctx(SQ)의
 * 상태 파일 하나(예: 몇 개의 CID가 in-flight인지 보여주는 "tags",
 * doorbell 직전 대기 목록을 보여주는 "dispatch")에 대응한다.
 */
struct blk_mq_debugfs_attr {
	const char *name;
	/* [한국어] debugfs에 생성될 파일 이름 문자열.
	 * 설정자: blk_mq_debugfs_hctx_attrs[] 등 정적 테이블 초기화 시 리터럴로
	 *   지정된다. 실제 값 예시: "state", "flags", "dispatch", "busy",
	 *   "ctx_map", "tags", "tags_bitmap", "sched_tags", "sched_tags_bitmap",
	 *   "active", "dispatch_busy", "type"(hctx 단위), "requeue_list"(큐 단위).
	 * 읽는 자: debugfs_create_files()가 이 배열을 순회하며
	 *   debugfs_create_file_aux()의 파일명 인자로 전달한다. 최종적으로
	 *   /sys/kernel/debug/block/<disk>/mq/hctx<N>/<name> 경로에 노출된다.
	 * 값 범위: 유효한 NUL 종료 문자열 포인터. 배열의 마지막 원소는 name이
	 *   NULL인 sentinel이며, for (; attr->name; attr++) 순회의 종료 조건이다.
	 * 동기화: 컴파일 타임에 고정되는 정적 데이터이므로 런타임 동기화가
	 *   필요 없다. */

	umode_t mode;
	/* [한국어] debugfs 파일의 접근 권한 비트마스크(8진수, 예: 0400, 0600).
	 * 설정자: 각 attr 테이블 엔트리에서 리터럴로 지정된다.
	 * 읽는 자: debugfs_create_file_aux()가 파일(inode) 생성 시 접근 권한으로
	 *   그대로 적용한다.
	 * 값 범위: 대부분 0400(root 읽기 전용, tags/dispatch/busy 등 상태 열람용)
	 *   이며, .write 콜백이 존재하는 "state" 등 제어용 파일만 관리자가
	 *   run/kick 트리거를 쓸 수 있도록 쓰기 권한이 추가된 모드를 쓴다.
	 * 동기화: 파일 권한 검사는 VFS/debugfs 레이어가 수행하며, 이 구조체는
	 *   설정값을 보관하는 역할만 한다. */

	int (*show)(void *, struct seq_file *);
	/* [한국어] 단일(비반복) 값을 출력하는 콜백. .seq_ops를 쓰지 않는
	 * 파일에서 사용한다(예: hctx_state_show, hctx_tags_show,
	 * hctx_sched_tags_show, queue_zone_wplugs_show).
	 * 설정자: block/blk-mq-debugfs.c(및 block/blk-zoned.c)에 정의된 개별
	 *   show 함수의 포인터가 테이블 초기화 시 대입된다.
	 * 읽는 자: 파일 open 시 커널이 이 콜백을 single_open() 계열로 감싸
	 *   read(2) 처리 중 호출한다. 첫 인자 void *에는 debugfs 등록 시
	 *   넘긴 data(보통 hctx 또는 request_queue 포인터)가 전달된다.
	 * 값 범위: NULL이면 아래 .seq_ops가 대신 쓰인 것으로 간주한다(둘 중
	 *   정확히 하나만 설정 — 아래 "Set either .show or .seq_ops." 참고).
	 * 동기화: 호출 전에 blk_mq_debugfs_register() 계열이
	 *   lockdep_assert_not_held()로 q->elevator_lock/q->rq_qos_mutex를
	 *   쥐지 않은 상태를 강제하므로, show 콜백 내부에서 hctx/tags 관련
	 *   락은 각 show 함수가 스스로 획득/해제한다. */

	ssize_t (*write)(void *, const char __user *, size_t, loff_t *);
	/* [한국어] 사용자 공간에서 write(2)로 이 debugfs 파일에 값을 쓸 때
	 * 호출되는 콜백. 대부분의 읽기 전용 attr에서는 NULL이며, "state"처럼
	 * 관리자가 강제로 hctx dispatch를 재실행시키는 제어용 파일에만 설정된다.
	 * 설정자: 제어 가능한 일부 attr 엔트리에서만 명시적으로 지정된다.
	 * 읽는 자: block/blk-mq-debugfs.c의 공용 write file_operations
	 *   핸들러가 사용자 버퍼(const char __user *)와 길이(size_t),
	 *   파일 오프셋(loff_t *)을 그대로 이 콜백에 전달한다.
	 * 값 범위: 반환값은 처리한 바이트 수(성공, size_t 이내 양수) 또는
	 *   음수 errno(실패). __user 포인터이므로 콜백 내부에서 반드시
	 *   copy_from_user() 등 안전한 접근자를 통해서만 역참조해야 한다.
	 * 동기화: 쓰기 도중 다른 CPU가 같은 hctx의 상태를 바꿀 수 있으므로,
	 *   콜백 구현이 필요한 락(hctx->lock 등)을 스스로 확보한다. */

	/* Set either .show or .seq_ops. */
	const struct seq_operations *seq_ops;
	/* [한국어] 리스트(여러 항목)를 순회 출력할 때 쓰는 seq_file 반복자
	 * 테이블(.start/.next/.stop/.show 4개 콜백 묶음).
	 * 설정자: hctx_dispatch_seq_ops(hctx->dispatch 리스트),
	 *   ctx_default_rq_list_seq_ops/ctx_read_rq_list_seq_ops/
	 *   ctx_poll_rq_list_seq_ops(blk_mq_ctx의 per-CPU rq_list),
	 *   queue_requeue_list_seq_ops(큐의 requeue_list) 등
	 *   block/blk-mq-debugfs.c에 정의된 정적 seq_operations 인스턴스.
	 * 읽는 자: 파일 open 시 이 포인터가 non-NULL이면 seq_open()으로 열어
	 *   커널이 리스트 순회를 자동 처리하고, 각 노드의 최종 텍스트 출력은
	 *   등록된 .show(대개 이 헤더의 blk_mq_debugfs_rq_show)가 담당한다.
	 * 값 범위: NULL이면 위 .show 콜백이 대신 쓰인다(상호 배타적 — 위 원본
	 *   영어 주석 "Set either .show or .seq_ops."가 이를 명시).
	 * 동기화: .next 콜백 사이에 리스트가 변경될 수 있으므로(예: NVMe
	 *   dispatch 리스트에 request가 새로 들어오거나 빠짐), 개별 seq_ops
	 *   구현이 hctx->lock/spinlock으로 순회 구간을 보호한다. */
};

/*
 * [한국어]
 * __blk_mq_debugfs_rq_show - 단일 request의 핵심 필드(op, cmd_flags,
 *   tag/CID, internal_tag 등)를 seq_file에 한 줄로 출력하는 공용 헬퍼.
 *
 * @m: 출력 대상 seq_file. 호출자가 이미 seq_open()으로 연 상태이며, 이
 *     함수는 seq_printf() 계열로 m에 텍스트를 append하기만 한다.
 * @rq: 출력할 struct request 포인터. blk_mq_debugfs_rq_show()의
 *      seq_ops iterator가 리스트 노드에서 list_entry_rq()로 복원해
 *      전달하거나, mq-deadline/kyber 같은 I/O 스케줄러의 자체 debugfs
 *      seq_ops가 자신의 내부 자료구조(fifo list, rb-tree 등)에서 뽑아낸
 *      request를 직접 전달한다.
 * @return: 항상 0(seq_show 콜백 규약상 이 함수 자체는 실패를 표현하지
 *          않는다).
 *
 * request의 필드를 사람이 읽을 문자열로 바꾸는 로직은 blk-mq core의
 * debugfs 파일뿐 아니라 mq-deadline, kyber 스케줄러의 debugfs 파일에서도
 * 그대로 재사용되므로, 공통 헬퍼로 분리해 EXPORT_SYMBOL_GPL로 다른 모듈에
 * 공개한다. NVMe 디버깅 시 rq->tag(하드웨어 CID)와 rq->cmd_flags를 통해
 * 큐에 걸린 특정 NVMe 커맨드의 종류(READ/WRITE/FLUSH 등)와 상태를 식별하는
 * 데 쓰인다.
 * 실행 컨텍스트: debugfs 파일을 연 유저 프로세스의 read(2) 시스템 호출을
 * 처리하는 커널 컨텍스트에서 실행되며, 인터럽트/소프트IRQ 컨텍스트에서는
 * 호출되지 않는다. 여러 프로세스가 동시에 같은 debugfs 파일을 read할 수
 * 있으므로 재진입 가능해야 하고, 이 함수 자체는 rq를 읽기만 하고 수정하지
 * 않으므로 별도 락 없이도 안전하다(단, rq가 그 사이 free/재사용되지 않는
 * 것은 호출자의 리스트 락이 보장한다).
 * 호출자: 이 파일이 선언하는 blk_mq_debugfs_rq_show()(공용 seq_ops
 *   .show 콜백), block/mq-deadline.c의 자체 debugfs .show 콜백들.
 * 피호출자: seq_printf(), 그리고 request 필드를 문자열로 변환하는
 *   block/blk-mq-debugfs.c 내부 헬퍼(op/flags 이름 테이블 등).
 * 에러 경로: 별도 에러 반환이 없으며, seq_file 버퍼가 가득 차는 경우의
 *   재시도/확장은 seq_file 레이어(block/blk-mq-debugfs.c 상위)의 책임이다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_rq_show() 또는 mq-deadline/kyber 자체 .show
 *     → [__blk_mq_debugfs_rq_show()] → seq_printf()
 */
int __blk_mq_debugfs_rq_show(struct seq_file *m, struct request *rq);

/*
 * [한국어]
 * blk_mq_debugfs_rq_show - request 리스트를 순회하는 seq_operations가
 *   공유하는 .show 콜백. 현재 iterator 위치를 struct request로 변환해
 *   __blk_mq_debugfs_rq_show()에 위임한다.
 *
 * @m: 출력 대상 seq_file.
 * @v: 해당 seq_operations의 .start/.next 콜백이 반환한 현재 iterator 위치.
 *     실제로는 struct request의 리스트 노드(예: rq->queuelist) 포인터이며,
 *     list_entry_rq() 매크로로 컨테이너인 struct request *로 변환된다.
 * @return: __blk_mq_debugfs_rq_show()의 반환값을 그대로 전달한다(항상 0).
 *
 * hctx_dispatch_seq_ops, ctx_default_rq_list_seq_ops,
 * queue_requeue_list_seq_ops 등 struct blk_mq_debugfs_attr.seq_ops를
 * 사용하는 모든 debugfs 파일이 공유하는 .show 진입점이다. NVMe 관점에서는
 * hctx->dispatch 리스트(다음 doorbell로 나갈 후보)나 blk_mq_ctx의
 * per-CPU rq_lists[](아직 hctx에 옮겨지지 않은 대기열)에 쌓인 request들을
 * 한 줄씩 출력할 때 리스트 항목마다 이 함수가 호출된다.
 * 실행 컨텍스트: read(2) 시스템 호출 컨텍스트에서, 해당 seq_ops의
 * .start/.next 구현이 리스트 보호 락(hctx->lock 등)을 쥔 상태로 호출하는
 * 것이 일반적이다.
 * 호출자: seq_file core(seq_read()가 각 리스트 항목마다 .show를 호출).
 * 피호출자: list_entry_rq(), __blk_mq_debugfs_rq_show().
 * 에러 경로: 없음(seq_show 규약상 int를 반환하지만 이 구현은 실패를
 *   만들지 않는다).
 *
 * 호출 체인:
 *   seq_read() → .show=[blk_mq_debugfs_rq_show()] → __blk_mq_debugfs_rq_show()
 */
int blk_mq_debugfs_rq_show(struct seq_file *m, void *v);

/*
 * [한국어]
 * blk_mq_debugfs_register - request_queue의 최상위 debugfs 디렉터리와
 *   그 하위(hctx, rq_qos) 항목들을 한꺼번에 등록한다.
 *
 * @q: debugfs 노드를 생성할 대상 request_queue. q->debugfs_dir에 상위
 *     디렉터리(/sys/kernel/debug/block/<disk>)가 이미 만들어져 있어야
 *     하며, 이 함수는 그 아래에 큐 단위 attr 파일들과 mq/hctx<N>/ 서브
 *     디렉터리를 채운다.
 * @return: void. q->debugfs_dir이 IS_ERR_OR_NULL이면 아무 것도 하지 않고
 *          조용히 반환하는 방어적 구조다(debugfs가 마운트되지 않은 등의
 *          경우에도 디스크 등록 자체는 실패하지 않도록).
 *
 * 디스크가 새로 등록될 때마다 debugfs 트리를 새로 만들어야 하므로 디스크
 * 등록 경로의 일부로 호출된다. NVMe 컨트롤러가 네임스페이스를 attach하여
 * add_disk()가 성공하면, 그 결과로 만들어지는 request_queue마다 이 함수가
 * 한 번 호출되어 /sys/kernel/debug/block/nvme<N>n<M>/ 트리를 만든다.
 * 내부적으로 (1) 큐 단위 attr 배열(blk_mq_debugfs_queue_attrs)로 파일
 * 생성, (2) 모든 hctx를 순회하며 아직 디렉터리가 없는 hctx마다
 * blk_mq_debugfs_register_hctx() 호출, (3) rq-qos 상태를 노출하는
 * blk_mq_debugfs_register_rq_qos() 호출 순서로 진행된다.
 * 실행 컨텍스트: 디스크 등록 경로(보통 드라이버 probe 스레드)에서
 * 동기적으로 실행되며, q->elevator_lock/q->rq_qos_mutex를 잡지 않은
 * 상태에서만 호출 가능함을 lockdep_assert_not_held()로 강제한다(락 중첩
 * 데드락 방지).
 * 호출자: block/blk-sysfs.c의 blk_register_queue() — sysfs 등록과 함께
 *   디스크가 커널에 공개되는 시점에 호출된다.
 * 피호출자: debugfs_create_files(), blk_mq_debugfs_register_hctx(),
 *   blk_mq_debugfs_register_rq_qos().
 * 에러 경로: debugfs 생성 실패(예: CONFIG_DEBUG_FS는 켜져 있으나 debugfs가
 *   마운트되지 않은 경우)는 치명적 오류로 취급하지 않는다 — 디버깅 편의
 *   기능이므로 실패해도 I/O 경로에는 영향이 없고, 단지 노드가 노출되지
 *   않을 뿐이다.
 *
 * 호출 체인:
 *   blk_register_queue() → [blk_mq_debugfs_register()]
 *     → blk_mq_debugfs_register_hctx() (hctx마다)
 *     → blk_mq_debugfs_register_rq_qos()
 */
void blk_mq_debugfs_register(struct request_queue *q);

/*
 * [한국어]
 * blk_mq_debugfs_register_hctx - 단일 blk_mq_hw_ctx(NVMe SQ에 대응하는
 *   하드웨어 디스패치 큐)의 debugfs 디렉터리(mq/hctx<N>/)와 그 하위 상태
 *   파일들을 생성한다.
 *
 * @q: hctx가 소속된 request_queue. 상위 디렉터리(q->debugfs_dir/mq)를
 *     경로의 기준점으로 사용한다.
 * @hctx: debugfs 노드를 생성할 대상 하드웨어 큐. 생성된 디렉터리 포인터는
 *        hctx->debugfs_dir 필드에 저장되어 이후 조회/해제 시 재사용된다.
 * @return: void.
 *
 * blk_mq_debugfs_register()가 큐의 모든 hctx를 순회하며 호출하거나,
 * nr_hw_queues가 런타임에 바뀌는 경우
 * blk_mq_debugfs_register_hctxs()를 통해 새로 생긴 hctx에 대해서만
 * 개별 호출된다. NVMe에서 인터럽트 벡터/CPU 토폴로지 변경으로 하드웨어
 * 큐 개수가 재조정될 때 이 경로를 탄다. 내부적으로
 * blk_mq_debugfs_hctx_attrs[](state/flags/dispatch/busy/ctx_map/tags/
 * tags_bitmap/sched_tags/sched_tags_bitmap/active/dispatch_busy/type)와
 * blk_mq_debugfs_ctx_attrs[](default_rq_list/read_rq_list/poll_rq_list)를
 * 각각 debugfs_create_files()로 등록한다.
 * 실행 컨텍스트: 디스크 등록 또는 큐 개수 재조정 경로에서 동기적으로
 * 실행된다.
 * 호출자: blk_mq_debugfs_register(), blk_mq_debugfs_register_hctxs().
 * 피호출자: debugfs_create_dir(), debugfs_create_files().
 * 에러 경로: 상위 디렉터리가 없거나 오류 포인터면(IS_ERR_OR_NULL) 조용히
 *   반환하여 해당 hctx의 디버깅 노드만 비어 있게 된다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register() 또는 blk_mq_debugfs_register_hctxs()
 *     → [blk_mq_debugfs_register_hctx()] → debugfs_create_files()
 */
void blk_mq_debugfs_register_hctx(struct request_queue *q,
				  struct blk_mq_hw_ctx *hctx);

/*
 * [한국어]
 * blk_mq_debugfs_unregister_hctx - 특정 hctx에 대해 만들어 두었던 debugfs
 *   디렉터리 전체(state/tags/dispatch 등 하위 파일 포함)를 제거한다.
 *
 * @hctx: 제거 대상 하드웨어 큐. hctx->debugfs_dir을 재귀 삭제한 뒤,
 *        해당 필드를 다시 사용 가능한 상태로 정리해 중복 해제를 막는다.
 * @return: void.
 *
 * NVMe 컨트롤러가 리셋되거나 하드웨어 큐 개수가 줄어들 때, 더 이상 존재
 * 하지 않는 hctx의 debugfs 항목이 stale 상태로 남아 사용자에게 잘못된
 * 정보를 보여주는 것을 막기 위해 필요하다.
 * blk_mq_debugfs_unregister_hctxs()가 큐 전체를 순회하며 호출하는 경로가
 * 일반적이다.
 * 실행 컨텍스트: 큐 재구성/디스크 제거 경로에서 동기적으로 실행되며, 이
 * 함수가 실행되는 동안에는 해당 hctx가 더 이상 I/O 디스패치에 쓰이지
 * 않는 상태여야 한다(그렇지 않으면 디렉터리 삭제 도중 show 콜백이 해제된
 * hctx를 참조할 위험이 있다).
 * 호출자: blk_mq_debugfs_unregister_hctxs().
 * 피호출자: debugfs_remove_recursive() 계열.
 * 에러 경로: hctx->debugfs_dir이 이미 NULL이면 no-op으로 처리되어 중복
 *   호출에 안전하다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_unregister_hctxs() → [blk_mq_debugfs_unregister_hctx()]
 */
void blk_mq_debugfs_unregister_hctx(struct blk_mq_hw_ctx *hctx);

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
 * 새 hctx 배열 전체에 대해 debugfs를 다시 채워야 한다. 이 함수는 모든
 * hctx를 순회하며 아직 debugfs_dir이 없는 hctx만 새로 등록해, 이미 존재
 * 하는 hctx의 노드를 중복 생성하지 않는다.
 * 실행 컨텍스트: 큐가 freeze된 상태이거나 재구성 락을 쥔 상태에서
 * 호출되는 것이 일반적이다(block/blk-mq.c 호출부 참고).
 * 호출자: block/blk-mq.c(하드웨어 큐 개수 재조정 경로, blk_mq_sysfs_
 *   register_hctxs()와 짝을 이루는 지점).
 * 피호출자: blk_mq_debugfs_register_hctx().
 * 에러 경로: 개별 hctx 등록 실패는 blk_mq_debugfs_register_hctx() 쪽에서
 *   조용히 무시되므로 이 함수도 별도 에러를 반환하지 않는다.
 *
 * 호출 체인:
 *   block/blk-mq.c(큐 재구성) → [blk_mq_debugfs_register_hctxs()]
 *     → blk_mq_debugfs_register_hctx() (hctx마다)
 */
void blk_mq_debugfs_register_hctxs(struct request_queue *q);

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
 * 피호출자: blk_mq_debugfs_unregister_hctx() (hctx마다).
 * 에러 경로: 없음(각 hctx 해제가 개별적으로 안전하게 no-op 처리됨).
 *
 * 호출 체인:
 *   block/blk-mq.c(큐 재구성 시작) → [blk_mq_debugfs_unregister_hctxs()]
 *     → blk_mq_debugfs_unregister_hctx() (hctx마다)
 */
void blk_mq_debugfs_unregister_hctxs(struct request_queue *q);

/*
 * [한국어]
 * blk_mq_debugfs_register_sched - I/O 스케줄러(mq-deadline/bfq/kyber)
 *   단위의 debugfs 노드를 큐에 등록한다.
 *
 * @q: 스케줄러가 붙어 있는 request_queue. q->elevator가 가리키는
 *     스케줄러가 자신의 debugfs attr 배열을 제공하면 그것을 등록한다.
 * @return: void.
 *
 * 사용자가 /sys/block/<disk>/queue/scheduler에 값을 써서 I/O 스케줄러를
 * 교체(elevator switch)하면, 이전 스케줄러의 debugfs 노드는 제거되고 새
 * 스케줄러의 노드가 새로 생성되어야 한다. 이 함수가 그 등록 절반을
 * 담당한다. NVMe 멀티 큐 환경에서 SQ로 보내기 전 merge/batch 여부를
 * 좌우하는 스케줄러 내부 상태(예: mq-deadline의 fifo list, bfq의 서비스
 * 트리)를 확인하는 진입점이 된다.
 * 실행 컨텍스트: elevator switch 경로(block/blk-mq-sched.c)에서
 * q->elevator_lock을 보유한 상태로 호출된다.
 * 호출자: block/blk-mq-sched.c의 elevator switch 처리 함수.
 * 피호출자: debugfs_create_files(). 호출자는 이어서 hctx별로
 *   blk_mq_debugfs_register_sched_hctx()도 호출한다.
 * 에러 경로: q->elevator가 NULL이거나 스케줄러가 debugfs attr을 제공하지
 *   않으면 아무 파일도 생성하지 않고 반환한다.
 *
 * 호출 체인:
 *   elevator switch(block/blk-mq-sched.c) → [blk_mq_debugfs_register_sched()]
 *     → debugfs_create_files(q->elevator의 큐 단위 debugfs attr)
 */
void blk_mq_debugfs_register_sched(struct request_queue *q);

/*
 * [한국어]
 * blk_mq_debugfs_unregister_sched - 큐에 등록되어 있던 I/O 스케줄러
 *   debugfs 노드를 제거한다.
 *
 * @q: 대상 request_queue.
 * @return: void.
 *
 * elevator switch로 스케줄러를 교체하거나 스케줄러 없는 none 모드로
 * 전환할 때, 이전 스케줄러가 등록해 둔 debugfs 파일들이 stale 상태로
 * 남지 않도록 제거한다.
 * 실행 컨텍스트: elevator switch 경로에서 q->elevator_lock을 보유한 채
 * 호출된다.
 * 호출자: block/blk-mq-sched.c(elevator switch의 이전 스케줄러 해제
 *   단계).
 * 피호출자: debugfs_remove_recursive() 계열.
 * 에러 경로: 노드가 이미 없으면 no-op.
 *
 * 호출 체인:
 *   elevator switch(해제 단계) → [blk_mq_debugfs_unregister_sched()]
 */
void blk_mq_debugfs_unregister_sched(struct request_queue *q);

/*
 * [한국어]
 * blk_mq_debugfs_register_sched_hctx - 특정 hctx에 대한 스케줄러별
 *   debugfs 노드(예: mq-deadline의 hctx당 fifo 상태)를 등록한다.
 *
 * @q: 스케줄러가 붙어 있는 request_queue.
 * @hctx: 대상 하드웨어 큐. hctx의 스케줄러 전용 데이터가 이미 초기화되어
 *        있어야 스케줄러가 자신의 hctx별 상태를 노출할 수 있다.
 * @return: void.
 *
 * 스케줄러 전체 단위(blk_mq_debugfs_register_sched)와 별개로, 스케줄러가
 * hctx마다 별도의 상태(예: 해당 SQ에 대기 중인 fifo 순서)를 가질 때 이
 * 함수가 그 hctx 전용 서브 디렉터리를 채운다. NVMe 멀티 큐 환경에서
 * SQ별로 스케줄링 정책이 어떻게 동작하는지 세밀하게 관찰할 때 쓰인다.
 * 실행 컨텍스트: elevator switch 경로에서 hctx를 순회하는 도중 호출된다.
 * 호출자: block/blk-mq-sched.c(elevator switch의 hctx 순회 루프).
 * 피호출자: debugfs_create_files(hctx의 스케줄러 debugfs 디렉터리에).
 * 에러 경로: 스케줄러가 hctx별 debugfs attr을 제공하지 않으면 아무 것도
 *   생성하지 않는다.
 *
 * 호출 체인:
 *   elevator switch(hctx 순회) → [blk_mq_debugfs_register_sched_hctx()]
 */
void blk_mq_debugfs_register_sched_hctx(struct request_queue *q,
					struct blk_mq_hw_ctx *hctx);

/*
 * [한국어]
 * blk_mq_debugfs_unregister_sched_hctx - 특정 hctx의 스케줄러별 debugfs
 *   노드를 제거한다.
 *
 * @hctx: 대상 하드웨어 큐.
 * @return: void.
 *
 * blk_mq_debugfs_register_sched_hctx()로 만든 hctx별 스케줄러 노드를
 * 스케줄러 교체/해제 시 되돌리는 짝 함수다.
 * 실행 컨텍스트: elevator switch 해제 경로에서 hctx를 순회하는 도중
 * 호출된다.
 * 호출자: block/blk-mq-sched.c(elevator switch 해제 단계의 hctx 순회
 *   루프).
 * 피호출자: debugfs_remove_recursive() 계열.
 * 에러 경로: 노드가 없으면 no-op.
 *
 * 호출 체인:
 *   elevator switch(해제, hctx 순회) → [blk_mq_debugfs_unregister_sched_hctx()]
 */
void blk_mq_debugfs_unregister_sched_hctx(struct blk_mq_hw_ctx *hctx);

/*
 * [한국어]
 * blk_mq_debugfs_register_rq_qos - rq-qos(요청 품질 서비스: WBT 쓰기
 *   스로틀, io-latency, iocost 등) 정책들이 큐에 등록한 상태를 debugfs로
 *   노출한다.
 *
 * @q: 대상 request_queue. q에 연결된 rq_qos 리스트를 순회하며 각 정책이
 *     제공하는 debugfs attr이 있으면 등록한다.
 * @return: void.
 *
 * rq-qos는 blk-mq 디스패치 앞단에서 조건에 따라 request를 지연/제한하는
 * 플러그인 프레임워크다(block/blk-rq-qos.c 참고). WBT(Writeback
 * Throttling)가 활성화된 NVMe 디스크에서 현재 스로틀 상태(대기 큐 길이,
 * 목표 latency)를 확인하려면 이 경로로 노출된 debugfs 파일을 읽는다.
 * block/blk-wbt.c가 WBT를 초기화하거나(wbt_init) 파라미터를 바꿀 때마다
 * 이 함수를 다시 호출해 debugfs 노드를 최신 상태로 갱신한다.
 * 실행 컨텍스트: WBT/rq-qos 초기화 또는 파라미터 변경 경로에서
 * q->rq_qos_mutex를 보유한 상태로 호출된다.
 * 호출자: blk_mq_debugfs_register()(큐 최초 등록 시), block/blk-wbt.c의
 *   wbt_init() 및 WBT 파라미터 변경 함수들.
 * 피호출자: debugfs_create_files()(q의 rq_qos 리스트를 순회하며 각 정책의
 *   debugfs attr마다 호출).
 * 에러 경로: 등록된 rq-qos 정책이 없으면 아무 파일도 생성하지 않는다.
 *
 * 호출 체인:
 *   blk_mq_debugfs_register() 또는 wbt_init()/WBT 파라미터 변경
 *     → [blk_mq_debugfs_register_rq_qos()]
 */
void blk_mq_debugfs_register_rq_qos(struct request_queue *q);

#else /* CONFIG_BLK_DEBUG_FS 미정의 시: NVMe 런타임 상태를 사용자 공간에 노출할 수 없고, 아래 no-op 스텁만 컴파일된다 */

/*
 * [한국어]
 * blk_mq_debugfs_register - CONFIG_BLK_DEBUG_FS 비활성 빌드용 no-op 스텁.
 *
 * @q: 사용되지 않음(무시). 매개변수 이름만 CONFIG_BLK_DEBUG_FS 활성
 *     버전과 맞춰, 호출부가 재컴파일만으로 자연스럽게 전환되게 한다.
 * @return: void.
 *
 * debugfs 관련 심벌 자체가 이 커널 설정에서는 컴파일되지 않으므로, 이
 * 헤더를 include하는 코드(block/blk-sysfs.c 등)가 #ifdef 분기 없이
 * blk_mq_debugfs_register(q)를 그대로 호출할 수 있도록 동일한 이름의
 * 빈 인라인 함수를 제공한다. static inline이므로 컴파일러가 호출부에
 * 인라인 전개한 뒤 빈 함수 호출은 dead code로 제거되어 런타임 비용이
 * 0이다.
 * 실행 컨텍스트: 호출자와 동일한 디스크 등록 경로이지만 실제로 아무
 * 작업도 수행하지 않는다.
 * 호출자: block/blk-sysfs.c의 blk_register_queue() 등, CONFIG_BLK_DEBUG_FS
 *   활성 빌드와 동일한 호출 지점.
 * 피호출자: 없음.
 * 에러 경로: 없음(항상 아무 것도 하지 않고 반환).
 */
static inline void blk_mq_debugfs_register(struct request_queue *q)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_register_hctx - CONFIG_BLK_DEBUG_FS 비활성 빌드용 no-op
 *   스텁. hctx별 debugfs 서브 디렉터리를 만들지 않는다.
 *
 * @q: 사용되지 않음(무시).
 * @hctx: 사용되지 않음(무시).
 * @return: void.
 *
 * 활성 빌드의 blk_mq_debugfs_register_hctx()와 동일한 시그니처를 유지해
 * block/blk-mq.c 등 호출부가 조건부 컴파일 없이 그대로 이 이름을 쓸 수
 * 있게 한다.
 * 실행 컨텍스트: 호출자와 동일하나 실질적 동작 없음.
 * 호출자: blk_mq_debugfs_register_hctxs() 스텁(같은 파일, 아래 정의).
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_register_hctx(struct request_queue *q,
						struct blk_mq_hw_ctx *hctx)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_hctx - CONFIG_BLK_DEBUG_FS 비활성 빌드용
 *   no-op 스텁.
 *
 * @hctx: 사용되지 않음(무시).
 * @return: void.
 *
 * 제거할 debugfs 노드 자체가 애초에 만들어지지 않았으므로, 아무 것도
 * 하지 않는 것이 올바른 동작이다.
 * 호출자: blk_mq_debugfs_unregister_hctxs() 스텁.
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_unregister_hctx(struct blk_mq_hw_ctx *hctx)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_register_hctxs - CONFIG_BLK_DEBUG_FS 비활성 빌드용
 *   no-op 스텁. 모든 hctx에 대해 아무 것도 등록하지 않는다.
 *
 * @q: 사용되지 않음(무시).
 * @return: void.
 *
 * 활성 빌드에서는 이 함수가 hctx마다 blk_mq_debugfs_register_hctx()를
 * 호출하지만, 이 빌드에서는 그 대상 자체가 no-op이므로 순회 로직도 함께
 * 생략된다.
 * 호출자: block/blk-mq.c(하드웨어 큐 개수 재조정 경로) — 활성 빌드와
 *   동일한 호출 지점.
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_register_hctxs(struct request_queue *q)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_hctxs - CONFIG_BLK_DEBUG_FS 비활성 빌드용
 *   no-op 스텁.
 *
 * @q: 사용되지 않음(무시).
 * @return: void.
 *
 * 등록된 노드가 없으므로 해제할 것도 없다.
 * 호출자: block/blk-mq.c(하드웨어 큐 개수 재조정 경로 시작 지점).
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_unregister_hctxs(struct request_queue *q)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_register_sched - CONFIG_BLK_DEBUG_FS 비활성 빌드용
 *   no-op 스텁. I/O 스케줄러 상태를 debugfs에 노출하지 않는다.
 *
 * @q: 사용되지 않음(무시).
 * @return: void.
 *
 * elevator switch 로직(block/blk-mq-sched.c)은 이 빌드에서도 동일하게
 * 실행되지만, debugfs 등록 단계만 아무 효과 없이 지나간다.
 * 호출자: block/blk-mq-sched.c(elevator switch 등록 단계).
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_register_sched(struct request_queue *q)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_sched - CONFIG_BLK_DEBUG_FS 비활성 빌드용
 *   no-op 스텁.
 *
 * @q: 사용되지 않음(무시).
 * @return: void.
 *
 * 등록된 scheduler debugfs 노드가 없으므로 해제도 no-op이다.
 * 호출자: block/blk-mq-sched.c(elevator switch 해제 단계).
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_unregister_sched(struct request_queue *q)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_register_sched_hctx - CONFIG_BLK_DEBUG_FS 비활성
 *   빌드용 no-op 스텁. hctx별 scheduler debugfs 생성을 생략한다.
 *
 * @q: 사용되지 않음(무시).
 * @hctx: 사용되지 않음(무시).
 * @return: void.
 *
 * 호출자: block/blk-mq-sched.c(elevator switch 등록 단계의 hctx 순회
 *   루프).
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_register_sched_hctx(struct request_queue *q,
						      struct blk_mq_hw_ctx *hctx)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_unregister_sched_hctx - CONFIG_BLK_DEBUG_FS 비활성
 *   빌드용 no-op 스텁.
 *
 * @hctx: 사용되지 않음(무시).
 * @return: void.
 *
 * SQ별 scheduler 노드가 없으므로 해제도 no-op이다.
 * 호출자: block/blk-mq-sched.c(elevator switch 해제 단계의 hctx 순회
 *   루프).
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_unregister_sched_hctx(struct blk_mq_hw_ctx *hctx)
{
}

/*
 * [한국어]
 * blk_mq_debugfs_register_rq_qos - CONFIG_BLK_DEBUG_FS 비활성 빌드용
 *   no-op 스텁. NVMe rq-qos(WBT 등) 정책 상태를 debugfs에 노출하지 않는다.
 *
 * @q: 사용되지 않음(무시).
 * @return: void.
 *
 * block/blk-wbt.c의 WBT 초기화/파라미터 변경 로직은 이 빌드에서도 그대로
 * 실행되지만, debugfs 갱신 호출만 아무 효과 없이 지나간다.
 * 호출자: blk_mq_debugfs_register() 스텁, block/blk-wbt.c의 wbt_init()
 *   및 파라미터 변경 함수들.
 * 피호출자: 없음.
 * 에러 경로: 없음.
 */
static inline void blk_mq_debugfs_register_rq_qos(struct request_queue *q)
{
}

#endif /* CONFIG_BLK_DEBUG_FS */

/*
 * [한국어] #if defined(CONFIG_BLK_DEV_ZONED) && defined(CONFIG_BLK_DEBUG_FS)
 * 분기 이유:
 * zone write plug(zwplug) 상태 출력은 zoned block device(CONFIG_BLK_DEV_ZONED,
 * 예: NVMe ZNS 네임스페이스)에서만 의미가 있는 자료구조이므로, 그 옵션이
 * 꺼진 커널에서는 struct blk_zone_wplug 자체가 존재하지 않는다. 따라서
 * CONFIG_BLK_DEV_ZONED와 CONFIG_BLK_DEBUG_FS가 모두 켜져 있을 때만 실제
 * 구현(block/blk-zoned.c의 queue_zone_wplugs_show())을 선언하고, 둘 중
 * 하나라도 꺼지면 항상 성공(0)을 반환하는 no-op 스텁으로 대체해 호출부
 * (block/blk-mq-debugfs.c의 blk_mq_debugfs_queue_attrs[] 등)가 조건부
 * 컴파일 없이 이 이름을 그대로 쓸 수 있게 한다.
 */
#if defined(CONFIG_BLK_DEV_ZONED) && defined(CONFIG_BLK_DEBUG_FS) /* 두 조건이 모두 참일 때만 NVMe ZNS zone 단위 wplug 상태를 노출 */

/*
 * [한국어]
 * queue_zone_wplugs_show - zoned block device의 zone별 write plug(zwplug)
 *   상태를 seq_file에 출력하는 debugfs show 콜백.
 *
 * @data: void* 로 전달되지만 실제로는 struct request_queue * 이다.
 *        구현(block/blk-zoned.c)에서 q->disk->zone_wplugs_hash를 순회하며
 *        각 zone의 write plug 엔트리를 찾는다.
 * @m: 출력 대상 seq_file.
 * @return: 0(성공). q->disk->zone_wplugs_hash가 아직 없으면(zoned가 아닌
 *          디스크이거나 초기화 전) 아무 것도 출력하지 않고 0을 반환한다.
 *
 * NVMe ZNS(Zoned Namespace) 네임스페이스는 zone(순차 쓰기 영역)마다
 * "현재 어느 오프셋까지 썼는지", "이 zone에 대해 대기 중인 bio가 몇 개인지"
 * 등을 추적해야 순차 쓰기 제약을 지킬 수 있다. 이 상태를 담은 것이
 * struct blk_zone_wplug이며, 이 함수는 각 zwplug을 zwplug->lock으로 잠깐
 * 잠그고 zone_no/flags/ref/cond/wp_offset/대기 bio 개수를 읽은 뒤 잠금을
 * 풀고 seq_printf()로 한 줄씩 출력한다.
 * 실행 컨텍스트: debugfs 파일을 읽는 유저 프로세스의 read(2) 컨텍스트.
 * 짧은 구간만 spin_lock_irqsave(&zwplug->lock, ...)로 보호하므로 인터럽트
 * 컨텍스트와의 경쟁도 안전하게 처리된다.
 * 호출자: block/blk-mq-debugfs.c의 blk_mq_debugfs_queue_attrs[]에 등록된
 *   "zone_wplugs" 파일의 .show 콜백으로 호출된다.
 * 피호출자: refcount_read(), bio_list_size(), blk_zone_cond_str(),
 *   seq_printf() (모두 block/blk-zoned.c 내부에서 사용).
 * 에러 경로: zone_wplugs_hash가 없으면 조기 반환(0)하여 아무 출력도
 *   만들지 않는다. 그 외 실패 경로는 없다.
 *
 * 호출 체인:
 *   debugfs read(2) → blk_mq_debugfs 공용 show 핸들러
 *     → [queue_zone_wplugs_show()] → seq_printf() (zone마다)
 */
int queue_zone_wplugs_show(void *data, struct seq_file *m);

#else /* ZNS 또는 debugfs 미활성 시: NVMe ZNS zone wplug 디버깅 경로가 컴파일되지 않는다 */

/*
 * [한국어]
 * queue_zone_wplugs_show - CONFIG_BLK_DEV_ZONED 또는 CONFIG_BLK_DEBUG_FS가
 *   꺼진 빌드용 no-op 스텁.
 *
 * @data: 사용되지 않음(무시).
 * @m: 사용되지 않음(무시).
 * @return: 항상 0(성공). 아무 것도 출력하지 않는다.
 *
 * CONFIG_BLK_DEV_ZONED가 꺼진 커널에는 struct blk_zone_wplug이 정의조차
 * 되지 않으므로, 이 스텁은 그 자료구조에 전혀 접근하지 않고 즉시 0을
 * 반환해 "zone_wplugs" debugfs 파일이 항상 빈 내용을 보여주도록 만든다.
 * 호출자: block/blk-mq-debugfs.c의 "zone_wplugs" attr .show 콜백 슬롯
 *   (활성 빌드와 동일한 호출 지점).
 * 피호출자: 없음.
 * 에러 경로: 없음(항상 성공 고정).
 */
static inline int queue_zone_wplugs_show(void *data, struct seq_file *m)
{
	return 0; /* 출력 없이 성공 반환: NVMe ZNS zone wplug 정보는 이 빌드에서 노출되지 않음 */
}
#endif

#endif /* INT_BLK_MQ_DEBUGFS_H */

/*
 * [한국어] NVMe 관점 실전 디버깅 시나리오 요약
 *
 * - 큐 전체 요약: /sys/kernel/debug/block/nvme0n1/ 아래 큐 단위 파일들
 *   (예: requeue_list, zone_wplugs)로 타임아웃/abort 후 재큐잉 대기 중인
 *   request나 ZNS zone 상태를 확인한다.
 * - hctx(SQ) 단위 상태: mq/hctx<N>/state로 STOPPED/TAG_ACTIVE 등 SQ
 *   상태를, tags/tags_bitmap으로 CID(하드웨어 tag) 사용 현황을,
 *   sched_tags/sched_tags_bitmap으로 스케줄러가 관리하는 가상 큐 depth를,
 *   dispatch로 doorbell 직전 대기 리스트를, active로 현재 in-flight
 *   NVMe 명령 수를 확인한다 — 이 값들이 바로 이 헤더가 선언하는
 *   blk_mq_debugfs_register_hctx()가 만드는 파일들이다.
 * - request 단위 상세: dispatch/busy/*_rq_list 파일을 열면 각 줄이
 *   __blk_mq_debugfs_rq_show()가 만든 op/flags/tag 정보이며, 특정 CID의
 *   생명주기(대기 -> dispatch -> 완료)를 추적할 수 있다.
 * - 정책 상태: 스케줄러(mq-deadline/bfq/kyber)와 rq-qos(WBT) debugfs
 *   노드는 NVMe I/O가 SQ로 들어가기 전 merge/batch/스로틀 정책이 어떻게
 *   작동하는지 보여주어 성능 문제 분석에 활용된다.
 * - 실제 노드 생성/출력 구현은 block/blk-mq-debugfs.c(및 zone 관련은
 *   block/blk-zoned.c)에 있고, CONFIG_BLK_DEBUG_FS가 꺼진 빌드에서는
 *   이 파일의 #else 분기가 제공하는 no-op 스텁으로 대체되어 어떤 debugfs
 *   노드도 생성되지 않는다.
 */
