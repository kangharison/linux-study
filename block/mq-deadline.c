// SPDX-License-Identifier: GPL-2.0
/*
 *  MQ Deadline i/o scheduler - adaptation of the legacy deadline scheduler,
 *  for the blk-mq scheduling framework
 *
 *  Copyright (C) 2016 Jens Axboe <axboe@kernel.dk>
 */

/*
 * [한국어 설명] mq-deadline I/O 스케줄러 구현 (block/mq-deadline.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 blk-mq(블록 멀티큐) 프레임워크 위에서 동작하는 mq-deadline I/O
 * 스케줄러의 전체 구현을 담는다. mq-deadline은 legacy(단일 큐) deadline
 * 스케줄러를 blk-mq용으로 재작성한 것으로, read/write 요청을 각각 "도착
 * 순서(FIFO, fifo_list)"와 "섹터 위치 순서(rb-tree, sort_list)"라는 두 개의
 * 자료구조에 동시에 유지하다가, deadline(read_expire/write_expire)이 임박한
 * 요청 또는 정렬 순서상 다음 요청을 골라 디스패치하는 단순하고 예측 가능한
 * (예측 가능한 지연시간을 보장하는) 스케줄러다. 연속된 같은 방향 요청을
 * 묶어서 처리하는 batching(fifo_batch)과, 한 방향이 다른 방향을 굶기지
 * 못하도록 하는 starvation 방지(writes_starved), 그리고 낮은 I/O 우선순위
 * 요청이 영원히 굶지 않도록 하는 prio aging(prio_aging_expire)이 이 파일의
 * 핵심 로직이다. 회전 디스크(HDD)에서는 헤드 이동(seek) 최소화가 주목적이고,
 * NVMe SSD에서도 여러 개의 PRP/SGL을 묶어 SQ(Submission Queue) 엔트리 수를
 * 줄이는 병합(merge) 기회를 늘리는 데 sort_list가 활용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널 블록 계층에서 이 파일은 blk-mq와 실제 블록 디바이스 드라이버(NVMe,
 * SCSI 등) 사이의 "소프트웨어 재배치" 계층에 위치한다. 전체 흐름은 다음과
 * 같다.
 *
 *   VFS / page cache
 *        ↓  bio 생성
 *   submit_bio() → blk_mq_submit_bio()
 *        ↓  기존 request와의 병합 시도 (elv_merge → ops.bio_merge=dd_bio_merge,
 *        ↓   ops.request_merge=dd_request_merge)
 *        ↓  병합 실패 시 새 request 할당 (ops.limit_depth=dd_limit_depth,
 *        ↓   ops.prepare_request=dd_prepare_request)
 *   [mq-deadline 스케줄러] ← 이 파일
 *     - dd_insert_requests(): request를 우선순위별 rb-tree/fifo_list에 삽입
 *     - dd_has_work():        디스패치할 작업이 있는지 blk_mq_run_hw_queue가 폴링
 *     - dd_dispatch_request(): deadline/batching/starvation 정책에 따라 다음
 *                              request 선택 후 hctx의 디스패치 리스트로 반환
 *        ↓  선택된 request
 *   blk_mq_dispatch_rq_list() → q->mq_ops->queue_rq (예: nvme_queue_rq)
 *        ↓                                         → nvme_submit_cmd (SQ doorbell)
 *   드라이버/디바이스 처리 완료 (NVMe CQ 인터럽트 등)
 *        ↓
 *   blk_mq_complete_request() → ops.finish_request=dd_finish_request
 *                              (완료 통계 갱신)
 *
 * 실행 컨텍스트: 삽입/병합/디스패치 경로는 대부분 제출자의 프로세스
 * 컨텍스트(또는 blk-mq의 소프트IRQ 디스패치 경로)에서 실행되고, 완료 경로는
 * 드라이버의 인터럽트 또는 소프트IRQ 컨텍스트에서 실행된다. sysfs
 * show/store 콜백은 사용자 프로세스가 /sys/block/<disk>/queue/iosched/*를
 * 읽고/쓸 때의 syscall 컨텍스트에서, debugfs show/seq_ops 콜백은
 * /sys/kernel/debug/block/<disk>/*를 읽는 syscall 컨텍스트에서 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/elevator.h/.c: struct elevator_type/elevator_mq_ops(콜백 vtable
 *     정의), elv_register()/elv_unregister()(스케줄러 등록/해제),
 *     elv_rb_add/del/find/former_request/latter_request(rb-tree 조작),
 *     elv_rqhash_add/del(back-merge 해시), elv_bio_merge_ok(병합 가능 여부
 *     1차 검사)를 그대로 재사용한다.
 *   - block/blk-mq.h: struct blk_mq_hw_ctx/blk_mq_alloc_data, blk_insert_t와
 *     BLK_MQ_INSERT_AT_HEAD, blk_mq_free_request()/blk_mq_free_requests().
 *   - block/blk-mq-sched.h: blk_mq_sched_try_merge()/try_insert_merge()(공용
 *     병합 헬퍼), blk_mq_is_sync_read()/blk_mq_set_min_shallow_depth().
 *   - block/blk-mq-debugfs.h: struct blk_mq_debugfs_attr, 공용
 *     __blk_mq_debugfs_rq_show()/blk_mq_debugfs_rq_show() — 이 파일의 자체
 *     debugfs seq_ops가 재사용.
 *   - block/blk.h: rq_mergeable(), blk_discard_mergable() 등 병합 조건 판정.
 *   - include/linux/rbtree.h, include/linux/sbitmap.h: rb-tree/tag 비트맵
 *     원시 자료구조.
 * 피의존 모듈:
 *   - block/blk-mq-sched.c: blk_mq_init_sched()/blk_mq_exit_sched()가 이
 *     파일의 ops.init_sched=dd_init_sched/ops.exit_sched=dd_exit_sched를 호출해
 *     스케줄러 인스턴스를 생성/파괴한다.
 *   - block/blk-mq.c: request 제출/디스패치/완료 경로 곳곳에서 elevator
 *     콜백을 통해 간접적으로 이 파일의 함수들을 호출한다.
 * 데이터 흐름: bio → (병합 실패 시) struct request 신규 할당 → dd_insert_request()가
 * struct dd_per_prio(rb-tree/fifo_list)에 편입 → dd_dispatch_request()가
 * 정책에 따라 struct request를 하나 골라 반환 → blk-mq가 하드웨어 큐로 전달 →
 * 완료 시 dd_finish_request()가 통계(io_stats_per_prio)를 갱신.
 * 공유 핵심 자료구조: struct deadline_data(스케줄러 인스턴스 전체 상태,
 * q->elevator->elevator_data에 저장), struct dd_per_prio(우선순위별 rb-tree/
 * fifo_list/통계), struct io_stats_per_prio(삽입/병합/디스패치/완료 카운터).
 * 이 구조체들은 전부 단일 spinlock(dd->lock)으로 보호되며, blk-mq의 여러
 * 하드웨어 큐(hctx)가 있어도 mq-deadline은 request_queue 전체에서 이
 * 스핀락 하나를 공유한다(QUEUE_FLAG_SQ_SCHED 참고).
 *
 * === 주요 함수/구조체 요약 ===
 * - dd_insert_requests()/dd_insert_request(): blk-mq가 넘긴 request를
 *   우선순위(dd_prio)별 rb-tree/fifo_list에 삽입하고 삽입 통계를 늘린다.
 * - dd_dispatch_request()/__dd_dispatch_request(): dd->lock을 쥔 채 예비
 *   dispatch 리스트 → prio-aging 대상 → RT/BE/IDLE 우선순위 순으로 다음에
 *   보낼 request 하나를 선택한다. batching/starvation 정책의 핵심.
 * - dd_bio_merge()/dd_request_merge()/dd_merged_requests()/dd_request_merged():
 *   bio 또는 request 병합 시도와 병합 후 rb-tree/fifo 위치 재조정을 담당.
 * - dd_init_sched()/dd_exit_sched(): 스케줄러 인스턴스(struct deadline_data)의
 *   할당/초기화와 해제/정합성 검증(WARN_ON_ONCE)을 담당.
 * - struct deadline_data: 이 스케줄러 인스턴스 전체의 런타임 상태(예비
 *   dispatch 리스트, 우선순위별 데이터, 마지막 방향/batching 카운터)와 sysfs로
 *   조정 가능한 설정값(fifo_expire, fifo_batch, writes_starved, front_merges,
 *   prio_aging_expire)을 담는다.
 * - struct dd_per_prio: 동일 I/O 우선순위(dd_prio) 안에서 read/write 각각의
 *   rb-tree(sort_list)와 fifo_list, 마지막 디스패치 위치(latest_pos), 통계
 *   (io_stats_per_prio)를 묶는다.
 * - 파일 하단의 SHOW_INT/STORE_FUNCTION 계열 매크로는 sysfs 튜너블 6종
 *   (read_expire, write_expire, writes_starved, front_merges, fifo_batch,
 *   prio_aging_expire)의 show/store 콜백을 찍어내고, DEADLINE_DEBUGFS_DDIR_ATTRS
 *   매크로는 CONFIG_BLK_DEBUG_FS용 6개 fifo_list 열람 seq_ops를 찍어낸다.
 */

#include <linux/kernel.h>	/* [한국어] BUG_ON, min/max 등 커널 공통 매크로 — 이 파일 곳곳의 불변조건 검사(BUG_ON)에 사용 */
#include <linux/fs.h>		/* [한국어] sysfs_emit() 등 파일/VFS 관련 헬퍼 — SHOW_INT 매크로가 sysfs 문자열 출력에 사용 */
#include <linux/blkdev.h>	/* [한국어] struct request_queue, struct request, blk_rq_pos() 등 블록 계층 핵심 타입/헬퍼 */
#include <linux/bio.h>		/* [한국어] struct bio, bio_data_dir(), bio_end_sector() — 병합 판단에 필요한 bio 접근자 */
#include <linux/module.h>	/* [한국어] MODULE_*, module_init/exit — 이 스케줄러를 로드 가능한 커널 모듈로 등록 */
#include <linux/slab.h>		/* [한국어] kzalloc_node()/kfree() — struct deadline_data를 NUMA 로컬로 할당/해제 */
#include <linux/init.h>		/* [한국어] __init/__exit 애트리뷰트 — deadline_init/deadline_exit이 모듈 (언)로드 시에만 필요함을 표시 */
#include <linux/compiler.h>	/* [한국어] likely/unlikely, __acquires/__releases 등 sparse/컴파일러 힌트 매크로 */
#include <linux/rbtree.h>	/* [한국어] struct rb_root/rb_node — sort_list[]가 사용하는 LBA(섹터) 정렬 rb-tree의 원시 타입 */
#include <linux/sbitmap.h>	/* [한국어] sbitmap 계열 타입 — blk-mq.h/blk-mq-sched.h가 참조하는 tag 비트맵과 연동 */

#include <trace/events/block.h>	/* [한국어] trace_block_rq_insert() — dd_insert_request()가 삽입 이벤트를 ftrace/perf에 통보할 때 사용 */

#include "elevator.h"		/* [한국어] struct elevator_type/elevator_mq_ops, elv_rb_add 등 rb-tree 헬퍼, elv_rqhash_add 등 해시 헬퍼, elv_bio_merge_ok — 이 파일이 구현하는 스케줄러 인터페이스와 rb-tree/해시 헬퍼 */
#include "blk.h"		/* [한국어] rq_mergeable(), blk_discard_mergable() — 병합 가능 여부를 판단하는 블록 계층 내부 헬퍼 */
#include "blk-mq.h"		/* [한국어] struct blk_mq_hw_ctx/blk_mq_alloc_data, blk_insert_t, BLK_MQ_INSERT_AT_HEAD — hctx 단위 삽입/디스패치 인터페이스 */
#include "blk-mq-debugfs.h"	/* [한국어] struct blk_mq_debugfs_attr, blk_mq_debugfs_rq_show() — 이 파일 하단의 CONFIG_BLK_DEBUG_FS 전용 열람 파일 정의에 사용 */
#include "blk-mq-sched.h"	/* [한국어] blk_mq_sched_try_merge()/try_insert_merge(), blk_mq_is_sync_read(), blk_mq_set_min_shallow_depth() — blk-mq 공용 스케줄러 헬퍼 */

/*
 * See Documentation/block/deadline-iosched.rst
 */
/*
 * [한국어] 아래 read_expire/write_expire/prio_aging_expire/writes_starved/
 * fifo_batch 다섯 개 상수는 dd_init_sched()가 struct deadline_data의 동일
 * 이름 필드(단, 시간 값은 static const int 그대로가 아니라 dd->fifo_expire[]
 * 등으로 복사)를 초기화할 때 쓰는 "공장 출하 기본값"이다. 이후
 * /sys/block/<disk>/queue/iosched/{read_expire,write_expire,writes_starved,
 * front_merges,fifo_batch,prio_aging_expire} sysfs 파일을 통해 인스턴스별로
 * 런타임에 재조정할 수 있다(이 파일 하단 STORE_* 매크로 참고). 여기 있는
 * static const 값은 그 sysfs 파일이 한 번도 쓰이지 않았을 때의 초기값일 뿐,
 * 실제 스케줄러 동작 중에는 dd->fifo_expire[]/dd->writes_starved/dd->fifo_batch/
 * dd->prio_aging_expire가 실효값이다.
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
/* [한국어] read 요청의 기본 deadline: HZ/2(약 500ms, HZ는 초당 타이머 틱 수).
 * dd_insert_request()가 rq->fifo_time = jiffies + dd->fifo_expire[DD_READ]로
 * 사용해 이 시각을 지나면 deadline_check_fifo()가 만료로 판단하고
 * __dd_dispatch_request()가 정렬 순서를 무시하고 FIFO 선두를 우선 디스패치한다. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
/* [한국어] write 요청의 기본 deadline: 5*HZ(약 5초) — read_expire보다 10배
 * 길다. write는 보통 페이지 캐시에 의해 버퍼링되어 read보다 지연에 덜
 * 민감하다고 가정하기 때문이다. 원본 주석의 "SOFT"는 이 deadline이 하드
 * 리얼타임 보장이 아니라 "가능한 한 이 시간 내에 처리하려는 목표치"임을
 * 의미한다 — started_after()에서 latest_start 기준을 지난 요청은 이번
 * 디스패치 라운드에서 아예 건너뛰므로 실제로는 더 늦게 처리될 수도 있다. */
/*
 * Time after which to dispatch lower priority requests even if higher
 * priority requests are pending.
 */
static const int prio_aging_expire = 10 * HZ;
/* [한국어] BE/IDLE 우선순위(dd_prio) 요청이 RT 우선순위에 밀려 굶주릴 수
 * 있는 최대 시간: 10*HZ(약 10초). dd_dispatch_prio_aged_requests()가 이
 * 값을 넘겨 대기 중인 BE/IDLE 요청을 발견하면, 우선순위 순서를 깨고 먼저
 * 디스패치한다. ionice(1) 등으로 IDLE 클래스를 지정한 백그라운드 작업도
 * 완전히 굶지 않고 최소한의 진행을 보장받는다. */
static const int writes_starved = 2;    /* max times reads can starve a write */
/* [한국어] read가 write를 연속으로 몇 번까지 앞지를 수 있는지의 한도.
 * __dd_dispatch_request()에서 dd->starved 카운터가 이 값 이상이 되면(read가
 * 이 값만큼 write보다 먼저 뽑혔으면) 강제로 write 쪽으로 전환한다(starvation
 * 방지). 기본값 2는 "read를 2번 우선하면 write도 한 번은 봐준다"는 의미로,
 * 값이 클수록 read 우선(지연시간)에, 작을수록 write 공정성(fairness)에
 * 가중치를 둔다. */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */
/* [한국어] 한 번의 batch에서 방향을 바꾸지 않고 연속 디스패치할 최대 요청
 * 수. 정렬된(sort_list) 순서로 최대 16개까지는 deadline/starvation 판단을
 * 건너뛰고 계속 같은 방향으로 디스패치해, 회전 디스크의 헤드 이동(seek)을
 * 줄이고 NVMe에서는 SQ doorbell 왕복 횟수를 줄여 처리량(throughput)을
 * 높인다. 이 숫자를 넘기면 __dd_dispatch_request()가 다시 방향/우선순위를
 * 재평가한다. */

enum dd_data_dir {
	/* [한국어] mq-deadline이 read/write 두 방향을 구분하는 열거형. blk-mq의
	 * REQ_OP_READ/REQ_OP_WRITE 계열 상수(<linux/blk_types.h>의 READ/WRITE)를
	 * 그대로 재사용해, rq_data_dir()/bio_data_dir()이 돌려주는 값과 별도
	 * 변환 없이 바로 배열 인덱스로 쓸 수 있게 한다. */
	DD_READ		= READ,
	/* [한국어] 읽기 방향. sort_list[DD_READ]/fifo_list[DD_READ]는 read
	 * 요청만 보관하며, read_expire(기본 500ms)가 이 방향의 deadline이다. */
	DD_WRITE	= WRITE,
	/* [한국어] 쓰기 방향. sort_list[DD_WRITE]/fifo_list[DD_WRITE]는 write
	 * 요청만 보관하며, write_expire(기본 5초)가 이 방향의 deadline이다. */
};

enum { DD_DIR_COUNT = 2 };
/* [한국어] dd_data_dir의 원소 개수(read/write 2개). sort_list[]/fifo_list[]/
 * latest_pos[]/fifo_expire[] 등 방향별 배열의 크기로 쓰인다. */

enum dd_prio {
	/* [한국어] I/O 우선순위 클래스(ioprio, <linux/ioprio.h>의 IOPRIO_CLASS_*)를
	 * mq-deadline 내부에서 다루기 쉬운 3단계(RT/BE/IDLE)로 압축한 열거형.
	 * ioprio_class_to_prio[] 배열이 IOPRIO_CLASS_*를 이 값으로 매핑한다. */
	DD_RT_PRIO	= 0,
	/* [한국어] 실시간(Real-Time) 우선순위 — IOPRIO_CLASS_RT가 매핑되는 값.
	 * dd_dispatch_request()의 for 루프가 prio=0부터 순회하므로 항상 가장
	 * 먼저 검사·디스패치되어 최우선으로 처리된다. */
	DD_BE_PRIO	= 1,
	/* [한국어] Best-Effort 우선순위 — IOPRIO_CLASS_NONE(우선순위 미지정)과
	 * IOPRIO_CLASS_BE가 모두 이 값으로 매핑되는 "보통" 우선순위이다. */
	DD_IDLE_PRIO	= 2,
	/* [한국어] 유휴(Idle) 우선순위 — IOPRIO_CLASS_IDLE이 매핑되는 값으로,
	 * 다른 우선순위가 전혀 없을 때만 디스패치되지만 prio_aging_expire를
	 * 통해 완전한 굶주림은 방지된다. */
	DD_PRIO_MAX	= 2,
	/* [한국어] 사용 가능한 최댓값(=DD_IDLE_PRIO와 동일한 2). "for (prio = 0;
	 * prio <= DD_PRIO_MAX; prio++)" 형태의 순회 상한으로 코드 곳곳에서
	 * 쓰인다. */
};

enum { DD_PRIO_COUNT = 3 };
/* [한국어] dd_prio의 원소 개수(RT/BE/IDLE 3개). struct deadline_data의
 * per_prio[DD_PRIO_COUNT] 배열 크기로 쓰인다. */

/*
 * I/O statistics per I/O priority. It is fine if these counters overflow.
 * What matters is that these counters are at least as wide as
 * log2(max_outstanding_requests).
 */
struct io_stats_per_prio {
	uint32_t inserted;
	/* [한국어] 이 우선순위로 스케줄러에 삽입된 누적 요청 수.
	 * 설정자: dd_insert_request()가 병합이 아닌 "새로 삽입되는" 요청에
	 *   대해서만(rq->elv.priv[0]이 이전에 NULL이었을 때) 1 증가시킨다.
	 * 읽는 자: dd_queued()가 inserted - completed로 "아직 완료되지 않고
	 *   스케줄러/드라이버에 머물러 있는 요청 수"를 계산할 때 사용.
	 * 값 범위: uint32_t 오버플로우가 나도 무방하다(주석 원문 참고) — 뺄셈
	 *   결과가 unsigned 산술로 자동 랩어라운드되어 올바른 차이를 낸다.
	 * 동기화: dd->lock 스핀락 보유 상태에서만 갱신/조회한다
	 *   (lockdep_assert_held(&dd->lock)로 강제). */
	uint32_t merged;
	/* [한국어] 이 우선순위에서 다른 요청에 병합되어 "사라진" 요청 수 누계.
	 * 설정자: dd_merged_requests()(requests_merged 콜백, 두 request가
	 *   하나로 합쳐질 때)가 1 증가시킨다.
	 * 읽는 자: dd_owned_by_driver()가 dispatched + merged - completed로
	 *   "드라이버가 현재 들고 있는 요청 수"를 근사할 때 사용(병합된 요청도
	 *   한 번은 드라이버로 내려갔던 것으로 계산에 포함).
	 * 값 범위: inserted와 마찬가지로 오버플로우 허용.
	 * 동기화: dd->lock 보유 상태에서만 갱신/조회. */
	uint32_t dispatched;
	/* [한국어] 이 우선순위에서 dd_start_request()를 거쳐 실제로 드라이버
	 * (hctx)로 넘어간 누적 요청 수.
	 * 설정자: dd_start_request()가 1 증가시킨다 — dd_dispatch_request()의
	 *   예비 dispatch 리스트 경로와 __dd_dispatch_request()의 일반 경로
	 *   양쪽 모두 최종적으로 이 함수를 거친다.
	 * 읽는 자: dd_owned_by_driver()가 이 값을 사용해 outstanding 요청 수를
	 *   추정한다.
	 * 값 범위: 오버플로우 허용.
	 * 동기화: dd->lock 보유 상태에서만 갱신/조회. */
	atomic_t completed;
	/* [한국어] 이 우선순위에서 완료(드라이버가 처리를 마쳐 blk-mq가
	 * request를 해제)된 누적 요청 수.
	 * 설정자: dd_finish_request()가 atomic_inc()로 증가시킨다 — 완료
	 *   경로는 드라이버 인터럽트/소프트IRQ 컨텍스트에서 dd->lock 없이
	 *   호출되므로, 이 필드만 유일하게 atomic_t로 별도 동기화한다.
	 * 읽는 자: dd_queued()/dd_owned_by_driver()가 atomic_read()로 읽어
	 *   inserted/dispatched+merged에서 빼는 데 사용.
	 * 값 범위: 오버플로우 허용(원본 주석 - log2(max_outstanding_requests)
	 *   비트 폭이면 충분).
	 * 동기화: 원자적 연산(atomic_t)으로 dd->lock 없이도 완료 경로와
	 *   조회 경로가 안전하게 동시 접근 가능하다. */
};

/*
 * Deadline scheduler data per I/O priority (enum dd_prio). Requests are
 * present on both sort_list[] and fifo_list[].
 */
struct dd_per_prio {
	struct rb_root sort_list[DD_DIR_COUNT];
	/* [한국어] 방향(read/write)별로 시작 섹터(LBA) 순서로 정렬된 rb-tree
	 * 루트. 하나의 요청은 삽입 시 rb_node를 통해 정확히 하나의 sort_list에
	 * 들어간다.
	 * 설정자: deadline_add_rq_rb()가 elv_rb_add()로 삽입, deadline_del_rq_rb()/
	 *   deadline_remove_request()가 elv_rb_del()로 제거.
	 * 읽는 자: deadline_from_pos()(어떤 LBA 이후 첫 요청 탐색),
	 *   dd_request_merge()(elv_rb_find()로 front-merge 후보 탐색).
	 * 값 범위: 비어있으면 RB_ROOT(초기화 값), 요청이 있으면 rb_node들의
	 *   트리.
	 * 동기화: 소유 deadline_data의 dd->lock으로 보호. */
	struct list_head fifo_list[DD_DIR_COUNT];
	/* [한국어] 방향별로 "도착(fifo_time 설정) 순서"를 유지하는 이중 연결
	 * 리스트. 헤드에 가장 먼저 만료될(가장 오래된) 요청이 위치한다.
	 * 설정자: dd_insert_request()가 list_add_tail()로 꼬리에 삽입,
	 *   deadline_remove_request()가 list_del_init()으로 제거.
	 * 읽는 자: deadline_check_fifo()/deadline_fifo_request()가 리스트
	 *   선두(.next)를 확인해 deadline 만료 여부와 FIFO 디스패치 후보를
	 *   판단.
	 * 값 범위: 비어있으면 리스트가 자기 자신을 가리킴, 요청이 있으면
	 *   struct request들의 queuelist로 연결된 체인.
	 * 동기화: dd->lock으로 보호. */
	/* Position of the most recently dispatched request. */
	sector_t latest_pos[DD_DIR_COUNT];
	/* [한국어] 방향별로 가장 최근에 디스패치된 요청의 시작 섹터(LBA).
	 * deadline_next_request()가 "다음에 이어서 디스패치할 정렬 순서상의
	 * 요청"을 찾는 기준점(스캔 시작 위치)으로 쓰인다.
	 * 설정자: dd_start_request()가 blk_rq_pos(rq)로 갱신 — 요청이 실제로
	 *   디스패치 확정될 때만 갱신된다.
	 * 읽는 자: deadline_next_request()/deadline_from_pos()가 이 위치
	 *   "이상"인 첫 요청을 rb-tree에서 찾아 순차 스캔을 이어간다.
	 * 값 범위: sector_t(보통 64비트) — 유효한 디스크 섹터 오프셋.
	 * 동기화: dd->lock으로 보호. */
	struct io_stats_per_prio stats;
	/* [한국어] 이 우선순위의 삽입/병합/디스패치/완료 카운터 묶음.
	 * 설정자/읽는 자: dd_insert_request(), dd_merged_requests(),
	 *   dd_start_request(), dd_finish_request(), dd_queued(),
	 *   dd_owned_by_driver() 등 이 파일 전역에서 광범위하게 사용.
	 * 값 범위: struct io_stats_per_prio 참고.
	 * 동기화: completed 필드만 atomic_t로 자체 동기화하고, 나머지는
	 *   dd->lock으로 보호. */
};

struct deadline_data {
	/*
	 * run time data
	 */

	struct list_head dispatch;
	/* [한국어] "우선순위 정책과 무관하게 다음 번 디스패치 요청 시 가장
	 * 먼저" 내보내야 할 요청들의 임시 대기열. dd_dispatch_request()는
	 * 이 리스트가 비어있지 않으면 아래의 우선순위 기반 로직(prio-aging,
	 * RT/BE/IDLE 순회)을 완전히 건너뛰고 이 리스트의 선두를 먼저 반환한다.
	 * 설정자: dd_insert_request()가 BLK_MQ_INSERT_AT_HEAD 플래그가 설 때
	 *   (예: 재제출/requeue되는 요청) list_add()로 리스트 머리에 삽입.
	 * 읽는 자: dd_dispatch_request()가 list_first_entry()로 선두를 꺼내
	 *   즉시 dd_start_request()로 확정한다.
	 * 값 범위: 비어있거나(정상 상태 대부분) requeue된 요청들의 짧은 체인.
	 * 동기화: dd->lock으로 보호. */
	struct dd_per_prio per_prio[DD_PRIO_COUNT];
	/* [한국어] RT/BE/IDLE 세 우선순위 각각의 rb-tree/fifo_list/통계 묶음
	 * 배열. dd_prio 값이 그대로 이 배열의 인덱스가 된다.
	 * 설정자: dd_init_sched()가 각 원소의 fifo_list/sort_list를 초기화.
	 * 읽는 자: 이 파일의 거의 모든 삽입/병합/디스패치 함수가
	 *   dd->per_prio[prio] 형태로 접근한다.
	 * 값 범위: 3개 원소 고정 배열.
	 * 동기화: dd->lock으로 보호. */

	/* Data direction of latest dispatched request. */
	enum dd_data_dir last_dir;
	/* [한국어] 가장 최근에 __dd_dispatch_request()가 선택한 방향
	 * (DD_READ 또는 DD_WRITE). 다음 호출에서 "아직 batching 한도 내라면
	 * 같은 방향을 이어갈지"를 판단하는 기준이 된다.
	 * 설정자: __dd_dispatch_request()의 dispatch_find_request 경로가
	 *   방향을 새로 정할 때마다 갱신. dd_init_sched()가 DD_WRITE로 초기화.
	 * 읽는 자: __dd_dispatch_request() 진입 시 deadline_next_request(dd,
	 *   per_prio, dd->last_dir)로 "이전과 같은 방향의 다음 순차 요청"을
	 *   먼저 조회.
	 * 값 범위: DD_READ 또는 DD_WRITE.
	 * 동기화: dd->lock으로 보호. */
	unsigned int batching;		/* number of sequential requests made */
	/* [한국어] 현재 진행 중인 batch에서 방향을 바꾸지 않고 연속으로
	 * 디스패치한 요청 수. fifo_batch에 도달하면 방향/우선순위를 재평가한다.
	 * 설정자: __dd_dispatch_request()가 매 디스패치마다 1 증가시키고,
	 *   새 batch를 시작할 때(dispatch_find_request 경로) 0으로 리셋.
	 * 읽는 자: __dd_dispatch_request() 진입부의
	 *   "dd->batching < dd->fifo_batch" 비교.
	 * 값 범위: 0 ~ dd->fifo_batch.
	 * 동기화: dd->lock으로 보호. */
	unsigned int starved;		/* times reads have starved writes */
	/* [한국어] read가 write보다 연속으로 우선 디스패치된 횟수 카운터.
	 * dd->writes_starved에 도달하면 강제로 write 방향으로 전환된다.
	 * 설정자: __dd_dispatch_request()가 write 후보가 있고 read를 우선
	 *   선택하려 할 때 1 증가(dd->starved++), write로 전환되면 0으로 리셋.
	 * 읽는 자: 같은 함수의 "dd->starved++ >= dd->writes_starved" 비교.
	 * 값 범위: 0 ~ dd->writes_starved(도달 시 리셋).
	 * 동기화: dd->lock으로 보호. */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[DD_DIR_COUNT];
	/* [한국어] 방향별 deadline(jiffies 단위 상대 시간). 요청 삽입 시
	 * rq->fifo_time = jiffies + fifo_expire[dir]로 절대 만료 시각을 계산하는
	 * 데 사용된다.
	 * 설정자: dd_init_sched()가 read_expire/write_expire 기본값으로 초기화.
	 *   deadline_read_expire_store()/deadline_write_expire_store() sysfs
	 *   콜백이 msecs_to_jiffies() 변환을 거쳐 런타임에 갱신.
	 * 읽는 자: dd_insert_request()(만료 시각 계산), started_after()(요청의
	 *   실제 도착 시각 역산).
	 * 값 범위: 0 ~ INT_MAX jiffies.
	 * 동기화: sysfs store는 dd->lock 없이 직접 대입하지만 int 단일 대입은
	 *   대부분의 아키텍처에서 사실상 원자적이며, 값이 약간 낡은 상태로
	 *   읽혀도 다음 삽입부터 반영되므로 안전하다(soft 상한이라는 설계
	 *   의도와 부합). */
	int fifo_batch;
	/* [한국어] batching 상한 — dd->batching이 이 값에 도달하면 방향/
	 * 우선순위를 재평가한다.
	 * 설정자: dd_init_sched()가 fifo_batch(전역 상수, 기본 16)로 초기화.
	 *   deadline_fifo_batch_store() sysfs 콜백으로 런타임 조정.
	 * 읽는 자: __dd_dispatch_request()의 배칭 계속 여부 판단.
	 * 값 범위: 0 ~ INT_MAX.
	 * 동기화: fifo_expire와 동일하게 락 없는 단일 대입. */
	int writes_starved;
	/* [한국어] read가 write를 몇 번까지 앞지를 수 있는지의 한도.
	 * 설정자: dd_init_sched()가 writes_starved(전역 상수, 기본 2)로 초기화.
	 *   deadline_writes_starved_store() sysfs 콜백으로 조정(음수 허용 —
	 *   INT_MIN까지 낮추면 read가 사실상 매번 write로 전환되어 write를
	 *   극단적으로 우대하게 된다).
	 * 읽는 자: __dd_dispatch_request()의 starvation 판단
	 *   ("dd->starved++ >= dd->writes_starved").
	 * 값 범위: INT_MIN ~ INT_MAX.
	 * 동기화: 락 없는 단일 대입. */
	int front_merges;
	/* [한국어] front merge(새 bio를 기존 요청의 "앞쪽" LBA로 병합) 허용
	 * 여부를 나타내는 불리언 성격의 정수(0 또는 1).
	 * 설정자: dd_init_sched()가 1(허용)로 초기화.
	 *   deadline_front_merges_store() sysfs 콜백으로 0/1 조정.
	 * 읽는 자: dd_request_merge()가 진입 직후 "!dd->front_merges"이면 즉시
	 *   ELEVATOR_NO_MERGE를 반환해 front-merge 탐색 자체를 건너뛴다.
	 * 값 범위: 0(비허용) 또는 1(허용) — STORE_INT(..., 0, 1)로 강제.
	 * 동기화: 락 없는 단일 대입. */
	int prio_aging_expire;
	/* [한국어] BE/IDLE 우선순위 요청이 최대 이만큼(jiffies) 대기하면
	 * 우선순위 순서를 무시하고 강제 디스패치되는 aging 한계.
	 * 설정자: dd_init_sched()가 prio_aging_expire(전역 상수, 기본 10*HZ)로
	 *   초기화. deadline_prio_aging_expire_store() sysfs 콜백으로 조정.
	 * 읽는 자: dd_dispatch_prio_aged_requests()가
	 *   "now - dd->prio_aging_expire"를 latest_start 기준으로 사용.
	 * 값 범위: 0 ~ INT_MAX jiffies.
	 * 동기화: 락 없는 단일 대입. */

	spinlock_t lock;
	/* [한국어] 이 deadline_data 인스턴스 전체(dispatch 리스트, per_prio[]의
	 * rb-tree/fifo_list/통계, last_dir/batching/starved)를 보호하는 단일
	 * 스핀락. mq-deadline은 QUEUE_FLAG_SQ_SCHED를 설정해 request_queue의
	 * 모든 하드웨어 큐(hctx)가 이 락 하나를 공유하도록 강제한다(dd_init_sched
	 * 참고) — 여러 하드웨어 큐가 있어도 정렬/FIFO/통계는 큐 전체에서
	 * 일관되게 유지되어야 하기 때문이다.
	 * 설정자: dd_init_sched()가 spin_lock_init()으로 초기화.
	 * 읽는 자/잠그는 자: dd_dispatch_request(), dd_insert_requests(),
	 *   dd_bio_merge(), dd_exit_sched() 등 rb-tree/fifo_list/통계를
	 *   조작하는 거의 모든 함수가 spin_lock()/spin_unlock() 쌍으로 감싼다.
	 *   dd_merged_requests()/dd_queued()/dd_owned_by_driver() 등은
	 *   lockdep_assert_held()로 "호출자가 이미 이 락을 쥐고 있어야 함"을
	 *   런타임에 검증한다.
	 * 값 범위: spinlock 상태(unlocked/locked). 인터럽트 컨텍스트에서는
	 *   잡지 않으므로(완료 경로 dd_finish_request는 atomic_t만 사용) 일반
	 *   spin_lock()/spin_unlock()이면 충분하다(irq 비활성화 불필요). */
};

/* Maps an I/O priority class to a deadline scheduler priority. */
static const enum dd_prio ioprio_class_to_prio[] = {
	/* [한국어] bio->bi_ioprio 또는 request의 ioprio에서 뽑아낸
	 * IOPRIO_CLASS_*(include/linux/ioprio.h) 값을 배열 인덱스로 사용해
	 * mq-deadline의 3단계 dd_prio로 축소 매핑하는 정적 룩업 테이블.
	 * dd_insert_request()/dd_request_merge()/dd_rq_ioclass() 경로에서
	 * "IOPRIO_PRIO_CLASS(ioprio)"의 결과를 그대로 이 배열의 첨자로 넣어
	 * O(1)에 dd_prio를 얻는다. 컴파일 타임에 고정되는 값이라 락 없이
	 * 어디서든 읽어도 안전하다. */
	[IOPRIO_CLASS_NONE]	= DD_BE_PRIO,
	/* [한국어] ioprio를 전혀 지정하지 않은(대부분의 일반 프로세스) 요청은
	 * Best-Effort로 취급한다. */
	[IOPRIO_CLASS_RT]	= DD_RT_PRIO,
	/* [한국어] 실시간 클래스로 지정된 요청은 최우선(RT) 취급 — 다른 모든
	 * 우선순위보다 먼저 디스패치 시도된다. */
	[IOPRIO_CLASS_BE]	= DD_BE_PRIO,
	/* [한국어] 명시적으로 Best-Effort로 지정된 요청도 동일하게 BE로 매핑. */
	[IOPRIO_CLASS_IDLE]	= DD_IDLE_PRIO,
	/* [한국어] Idle 클래스(ionice -c3 등)로 지정된 요청은 IDLE로 매핑되어
	 * 다른 우선순위가 없을 때만, 또는 prio_aging_expire 초과 시에만
	 * 디스패치된다. */
};

/*
 * [한국어]
 * deadline_rb_root - 요청 @rq의 방향(read/write)에 해당하는 rb-tree 루트를 반환.
 *
 * @per_prio: 대상 우선순위(dd_prio)의 rb-tree/fifo_list 묶음.
 * @rq:       방향을 판별할 대상 request.
 * @return:   per_prio->sort_list[rq_data_dir(rq)]의 주소 — read면
 *            sort_list[DD_READ], write면 sort_list[DD_WRITE].
 *
 * rb-tree 자체는 read/write 두 개로 분리되어 있으므로(같은 방향 요청끼리만
 * LBA 순서로 정렬), 이 헬퍼가 방향에 맞는 rb-tree를 골라주는 단일 지점
 * 역할을 한다. rq_data_dir()은 rq->cmd_flags에서 REQ_OP_WRITE 비트를
 * 검사해 DD_READ/DD_WRITE(READ/WRITE와 동일값)를 돌려준다.
 * 실행 컨텍스트: dd->lock을 쥔 호출자에서만 의미가 있다(반환된 rb_root를
 * 수정하려면 락이 필요) — 이 함수 자체는 락을 요구하지 않는 단순 조회.
 * 호출자: deadline_add_rq_rb(), deadline_del_rq_rb(), dd_request_merged().
 * 피호출자: rq_data_dir()(인라인 매크로/함수).
 *
 * 호출 체인:
 *   deadline_add_rq_rb/deadline_del_rq_rb/dd_request_merged → [deadline_rb_root]
 */
static inline struct rb_root *
deadline_rb_root(struct dd_per_prio *per_prio, struct request *rq)
{
	return &per_prio->sort_list[rq_data_dir(rq)]; /* rq_data_dir(rq)가 DD_READ/DD_WRITE를 반환 - 그 값을 그대로 배열 첨자로 사용해 방향에 맞는 rb-tree 루트의 주소를 돌려준다 */
}

/*
 * Returns the I/O priority class (IOPRIO_CLASS_*) that has been assigned to a
 * request.
 */
/*
 * [한국어]
 * dd_rq_ioclass - request에 부여된 I/O 우선순위 클래스(IOPRIO_CLASS_*)를 반환.
 *
 * @rq:     우선순위를 조회할 request.
 * @return: IOPRIO_CLASS_RT/BE/IDLE/NONE 중 하나의 값(u8).
 *
 * req_get_ioprio()는 request에 저장된 ioprio 값(원래 bio->bi_ioprio에서
 * 상속되며, 프로세스의 ioprio_set(2) 설정 또는 cgroup ioprio 설정에서
 * 유래한다)을 돌려주고, IOPRIO_PRIO_CLASS() 매크로가 그 상위 비트에서
 * 클래스만 추출한다. 이 값은 이후 ioprio_class_to_prio[]로 dd_prio에
 * 매핑되어 어느 rb-tree/fifo_list(per_prio[])에 들어갈지 결정한다.
 * 실행 컨텍스트: 락 불필요(rq는 호출자가 이미 소유/참조 중인 request).
 * 호출자: dd_request_merged(), dd_merged_requests(), dd_start_request() —
 *   모두 병합/디스패치 시점에 우선순위를 다시 확인해야 하는 경로.
 * 피호출자: req_get_ioprio(), IOPRIO_PRIO_CLASS()(둘 다 <linux/blk-mq.h>,
 *   <linux/ioprio.h>의 인라인/매크로).
 *
 * 호출 체인:
 *   dd_request_merged/dd_merged_requests/dd_start_request → [dd_rq_ioclass]
 *     → req_get_ioprio → IOPRIO_PRIO_CLASS
 */
static u8 dd_rq_ioclass(struct request *rq)
{
	return IOPRIO_PRIO_CLASS(req_get_ioprio(rq)); /* req_get_ioprio(rq)가 request에 저장된 전체 ioprio 값을 반환하고, IOPRIO_PRIO_CLASS()가 상위 비트에서 클래스(RT/BE/IDLE/NONE)만 추출한다 */
}

/*
 * Return the first request for which blk_rq_pos() >= @pos.
 */
/*
 * [한국어]
 * deadline_from_pos - @pos 이상의 LBA(섹터)를 가진 첫 request를 rb-tree에서 탐색.
 *
 * @per_prio: 탐색 대상 우선순위의 rb-tree/fifo_list 묶음.
 * @data_dir: 탐색할 방향(DD_READ 또는 DD_WRITE) — sort_list[data_dir]을 뒤진다.
 * @pos:      탐색 기준 섹터. 이 값 "이상"인 요청 중 가장 작은(=가장 가까운)
 *            것을 찾는다.
 * @return:   조건을 만족하는 request 포인터, 없으면 NULL.
 *
 * rb-tree는 시작 섹터(blk_rq_pos) 순으로 정렬되어 있으므로, 이진 탐색으로
 * @pos 이상인 노드 중 가장 왼쪽(가장 작은) 것을 찾으면 그것이 "다음 순차
 * 위치"의 요청이다. latest_pos[]에 저장된 마지막 디스패치 위치를 @pos로
 * 넘기면, 디스크 헤드가 방금 지나간 위치 바로 다음부터 이어지는 순차 I/O
 * 스트림을 계속 찾아낼 수 있다.
 * 실행 컨텍스트: dd->lock을 쥔 호출자에서 실행(rb-tree가 삽입/삭제로
 * 변경되지 않음을 보장).
 * 호출자: deadline_next_request()(dd->last_dir 또는 현재 data_dir 기준
 *   다음 순차 요청 탐색), DEADLINE_DEBUGFS_DDIR_ATTRS 매크로가 만드는
 *   *_next_rq_show() debugfs 콜백.
 * 피호출자: rb_entry_rq()(rb_node → struct request 변환), blk_rq_pos().
 *
 * 호출 체인:
 *   deadline_next_request → [deadline_from_pos] → rb_entry_rq/blk_rq_pos
 */
static inline struct request *deadline_from_pos(struct dd_per_prio *per_prio,
				enum dd_data_dir data_dir, sector_t pos)
{
	struct rb_node *node = per_prio->sort_list[data_dir].rb_node; /* 탐색을 시작할 rb-tree의 루트 노드 - 이 방향(read 또는 write)으로 정렬된 트리 전체를 순회할 진입점 */
	struct request *rq, *res = NULL; /* rq: 순회 중 현재 노드의 request, res: 지금까지 찾은 "@pos 이상 중 가장 작은" 후보(초기값 없음을 뜻하는 NULL) */

	while (node) { /* 트리 끝(리프의 자식 = NULL)에 도달할 때까지 이진 탐색 반복 */
		rq = rb_entry_rq(node); /* rb_node 포인터에서 그것을 포함하는 struct request 전체를 container_of 방식으로 복원 */
		if (blk_rq_pos(rq) >= pos) { /* 현재 노드의 시작 섹터가 기준 @pos 이상이면 이 노드는 유효한 후보 - 하지만 더 작은(더 가까운) 후보가 왼쪽 서브트리에 있을 수 있음 */
			res = rq; /* 지금까지 찾은 것 중 가장 작은 후보로 갱신 - 왼쪽으로 더 내려가며 계속 갱신될 수 있음 */
			node = node->rb_left; /* 더 작은(하지만 여전히 @pos 이상인) 후보를 찾아 왼쪽 서브트리로 계속 탐색 */
		} else { /* 현재 노드가 @pos보다 작으면(조건 불만족) 이 노드와 왼쪽 서브트리는 전부 @pos 미만이므로 후보가 될 수 없음 */
			node = node->rb_right; /* @pos 이상인 노드를 찾기 위해 오른쪽(더 큰 섹터) 서브트리로 이동 */
		}
	}
	return res; /* 발견된 최소 상한 후보(없으면 NULL)를 반환 - 순차 IO 스캔의 다음 지점 */
}

/*
 * [한국어]
 * deadline_add_rq_rb - request를 방향에 맞는 rb-tree에 삽입.
 *
 * @per_prio: 삽입 대상 우선순위의 rb-tree/fifo_list 묶음.
 * @rq:       삽입할 request(아직 rb-tree에 없는 상태여야 함).
 * @return:   없음.
 *
 * deadline_rb_root()로 방향에 맞는 rb-tree를 고른 뒤 elv_rb_add()(공용
 * elevator rb-tree 삽입 헬퍼, block/elevator.c)로 시작 섹터 기준 정렬
 * 위치에 삽입한다. 삽입된 요청은 이후 병합 시도(dd_request_merge)나 순차
 * 디스패치 탐색(deadline_from_pos)의 대상이 된다.
 * 실행 컨텍스트: dd->lock을 쥔 호출자에서 실행.
 * 호출자: dd_insert_request()(신규 삽입 시), dd_request_merged()(front
 *   merge로 위치가 바뀔 때 재삽입).
 * 피호출자: deadline_rb_root(), elv_rb_add().
 *
 * 호출 체인:
 *   dd_insert_request/dd_request_merged → [deadline_add_rq_rb]
 *     → deadline_rb_root, elv_rb_add
 */
static void
deadline_add_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	struct rb_root *root = deadline_rb_root(per_prio, rq); /* 이 request의 방향에 대응하는 rb-tree 루트를 조회 */

	elv_rb_add(root, rq); /* 시작 섹터(blk_rq_pos) 기준으로 트리에 정렬 삽입 - 구현은 block/elevator.c의 공용 헬퍼 */
}

/*
 * [한국어]
 * deadline_del_rq_rb - request를 방향에 맞는 rb-tree에서 제거.
 *
 * @per_prio: 제거 대상 우선순위의 rb-tree/fifo_list 묶음.
 * @rq:       제거할 request(현재 rb-tree에 들어있는 상태여야 함).
 * @return:   없음.
 *
 * deadline_add_rq_rb()의 역연산. elv_rb_del()이 실제 rb_erase()를 수행하고
 * rq->rb_node를 RB_CLEAR_NODE 상태로 되돌려, 이후 RB_EMPTY_NODE() 검사로
 * "이미 트리에서 빠졌다"를 판별할 수 있게 한다.
 * 실행 컨텍스트: dd->lock을 쥔 호출자에서 실행.
 * 호출자: deadline_remove_request()(단, 호출 전 RB_EMPTY_NODE()로 이미
 *   트리에 없는 경우를 걸러낸 뒤 호출), dd_request_merged()(front merge 시
 *   재삽입 전 제거).
 * 피호출자: deadline_rb_root(), elv_rb_del().
 *
 * 호출 체인:
 *   deadline_remove_request/dd_request_merged → [deadline_del_rq_rb]
 *     → deadline_rb_root, elv_rb_del
 */
static inline void
deadline_del_rq_rb(struct dd_per_prio *per_prio, struct request *rq)
{
	elv_rb_del(deadline_rb_root(per_prio, rq), rq); /* 방향에 맞는 rb-tree에서 이 request의 노드를 제거(rb_erase) - block/elevator.c 공용 헬퍼 */
}

/*
 * remove rq from rbtree and fifo.
 */
/*
 * [한국어]
 * deadline_remove_request - request를 rb-tree/fifo_list/병합 해시에서 모두 제거.
 *
 * @q:        request가 속한 request_queue. q->last_merge 갱신에 사용.
 * @per_prio: 이 request가 속한 우선순위의 rb-tree/fifo_list 묶음.
 * @rq:       제거할 request.
 * @return:   없음.
 *
 * 이 함수는 request가 (1) 실제로 디스패치되어 드라이버로 넘어가기 직전,
 * 또는 (2) 다른 request에 병합되어 더 이상 독립적으로 존재하지 않을 때
 * 호출되어, 스케줄러가 유지하는 세 가지 인덱스(도착순 fifo_list, LBA순
 * rb-tree, 끝섹터 기반 병합 해시)에서 이 request의 흔적을 모두 지운다.
 * q->last_merge가 바로 이 request를 가리키고 있었다면 NULL로 초기화해,
 * 이후 elv_merge()가 이미 사라진 request를 병합 후보로 잘못 재사용하는
 * 것을 방지한다.
 * 실행 컨텍스트: dd->lock을 쥔 호출자에서 실행.
 * 호출자: deadline_move_request()(디스패치 확정 직전),
 *   dd_merged_requests()(병합되어 사라지는 @next request 제거).
 * 피호출자: list_del_init(), RB_EMPTY_NODE(), deadline_del_rq_rb(),
 *   elv_rqhash_del().
 *
 * 호출 체인:
 *   deadline_move_request/dd_merged_requests → [deadline_remove_request]
 *     → deadline_del_rq_rb, elv_rqhash_del
 */
static void deadline_remove_request(struct request_queue *q,
				    struct dd_per_prio *per_prio,
				    struct request *rq)
{
	list_del_init(&rq->queuelist); /* fifo_list에서 제거하고 rq->queuelist 자체도 재초기화 - 이후 list_empty() 검사가 정확히 "비어있음"을 보게 함 */

	/*
	 * We might not be on the rbtree, if we are doing an insert merge
	 */
	if (!RB_EMPTY_NODE(&rq->rb_node)) /* insert merge(blk_mq_sched_try_insert_merge)로 애초에 rb-tree에 들어간 적이 없는 request일 수 있으므로 먼저 확인 */
		deadline_del_rq_rb(per_prio, rq); /* rb-tree에 있었던 경우에만 실제 제거 수행 - 없는 노드를 지우려 하면 오동작하므로 위 조건이 반드시 필요 */

	elv_rqhash_del(q, rq); /* 끝 섹터 기준 병합 해시에서도 제거 - 더 이상 back-merge 후보가 되지 않도록 함 */
	if (q->last_merge == rq) /* 마지막으로 병합에 성공했던 request가 바로 이 rq였는지 확인 */
		q->last_merge = NULL; /* 그렇다면 참조를 끊어 이미 제거된 request를 다음 병합 시도가 잘못 재사용하지 않도록 함 */
}

/*
 * [한국어]
 * dd_request_merged - request_merged 콜백. bio가 기존 @req에 병합된 직후 호출.
 *
 * @q:    request_queue.
 * @req:  bio가 병합되어 들어간(경계가 바뀐) 기존 request.
 * @type: 병합 종류 - ELEVATOR_FRONT_MERGE(앞쪽 확장)/BACK_MERGE(뒤쪽 확장)/
 *        DISCARD_MERGE 중 하나.
 * @return: 없음.
 *
 * elv_merge()가 bio를 @req에 병합하기로 확정한 뒤 elevator_mq_ops의
 * request_merged 훅으로 호출한다(elevator.h 참고). front merge는 요청의
 * "시작" 섹터가 앞으로 확장되는 경우이므로, rb-tree에서의 정렬 위치가
 * 바뀌어야 한다 — 그래서 일단 rb-tree에서 빼낸(elv_rb_del) 뒤 새 시작
 * 위치로 다시 삽입(deadline_add_rq_rb)한다. back merge는 끝 섹터만
 * 늘어나므로(시작 섹터는 그대로) rb-tree 위치를 바꿀 필요가 없다.
 * 실행 컨텍스트: bio 제출 경로의 병합 시도 중, dd->lock을 쥔 상태(dd_bio_merge
 *   또는 dd_request_merge를 호출한 blk_mq_sched_try_merge/insert_merge
 *   경로)에서 실행된다.
 * 호출자: block/elevator.c의 elv_merged_request()(elv_merge()가 병합을
 *   확정한 직후).
 * 피호출자: dd_rq_ioclass(), elv_rb_del(), deadline_rb_root(),
 *   deadline_add_rq_rb().
 *
 * 호출 체인:
 *   elv_merge() 성공 → elv_merged_request() → [dd_request_merged]
 *     → elv_rb_del/deadline_add_rq_rb
 */
static void dd_request_merged(struct request_queue *q, struct request *req,
			      enum elv_merge type)
{
	struct deadline_data *dd = q->elevator->elevator_data; /* elevator_data는 dd_init_sched()가 저장해 둔 이 스케줄러 인스턴스의 struct deadline_data */
	const u8 ioprio_class = dd_rq_ioclass(req); /* @req에 부여된 ioprio 클래스(RT/BE/IDLE/NONE)를 조회 */
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class]; /* ioprio 클래스를 mq-deadline의 3단계 dd_prio로 변환 */
	struct dd_per_prio *per_prio = &dd->per_prio[prio]; /* @req가 속한 우선순위의 rb-tree/fifo_list 묶음 */

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) { /* front merge인지 확인 - back/discard merge는 시작 섹터가 그대로라 rb-tree 재정렬이 불필요 */
		elv_rb_del(deadline_rb_root(per_prio, req), req); /* 옛 시작 섹터 기준 위치에서 먼저 제거 */
		deadline_add_rq_rb(per_prio, req); /* 새로 확장된(더 작아진) 시작 섹터 기준으로 다시 삽입해 정렬 순서를 바로잡음 */
	}
}

/*
 * Callback function that is invoked after @next has been merged into @req.
 */
/*
 * [한국어]
 * dd_merged_requests - requests_merged 콜백. 두 request @req/@next가 하나로
 * 합쳐진(@next가 사라지는) 직후 호출.
 *
 * @q:    request_queue.
 * @req:  병합 후에도 남는 request(결과적으로 @next의 범위까지 포함하게 됨).
 * @next: 병합되어 사라질 request - 이 함수가 끝나면 완전히 스케줄러 자료
 *        구조에서 제거된다.
 * @return: 없음.
 *
 * 두 개의 인접한 request가 (elv_attempt_insert_merge 등을 통해) 하나로
 * 합쳐질 때 호출된다. 이 함수는 (1) 통계상 merged 카운터를 증가시키고,
 * (2) 두 request 중 "더 급한" deadline(fifo_time이 더 이른 쪽)을 살아남는
 * @req가 물려받도록 fifo_list 상의 위치까지 옮긴 뒤, (3) @next를 rb-tree/
 * fifo_list/병합 해시에서 완전히 제거한다. @next의 deadline이 무시되면
 * 그 요청이 원래 보장받았어야 할 지연시간 상한이 깨지므로 (2) 단계가
 * 반드시 필요하다.
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출됨(lockdep_assert_held로 강제).
 * 호출자: block/elevator.c 계열의 병합 완료 처리 경로(elv_merge 성공 후
 *   두 request가 실제로 결합되는 지점, elevator_mq_ops.requests_merged).
 * 피호출자: dd_rq_ioclass(), list_empty(), time_before(), list_move(),
 *   deadline_remove_request().
 *
 * 호출 체인:
 *   (두 request 병합 확정) → [dd_merged_requests] → deadline_remove_request
 */
static void dd_merged_requests(struct request_queue *q, struct request *req,
			       struct request *next)
{
	struct deadline_data *dd = q->elevator->elevator_data; /* 이 스케줄러 인스턴스 */
	const u8 ioprio_class = dd_rq_ioclass(next); /* 사라질 @next 기준으로 우선순위를 조회 - @req와 @next는 병합 가능했으므로 보통 같은 우선순위 */
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class]; /* dd_prio로 변환 */

	lockdep_assert_held(&dd->lock); /* 호출자가 dd->lock을 이미 쥐고 있어야 함을 런타임에 검증 - rb-tree/fifo_list 동시 수정 방지 */

	dd->per_prio[prio].stats.merged++; /* 병합 통계 증가 - dd_owned_by_driver()가 outstanding 요청 수 추정에 사용 */

	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) { /* 두 request가 모두 아직 fifo_list에 남아있는 경우에만(디스패치 리스트로 옮겨진 요청이면 queuelist 의미가 달라지므로 제외) */
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)req->fifo_time)) { /* @next의 만료 시각이 @req보다 이르면(더 급하면) */
			list_move(&req->queuelist, &next->queuelist); /* @req를 @next가 있던 fifo_list 위치로 옮겨 더 급한 순서를 물려받음 */
			req->fifo_time = next->fifo_time; /* @req의 deadline도 @next의 것으로 갱신해 원래 @next가 보장받았어야 할 지연시간 상한을 유지 */
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	deadline_remove_request(q, &dd->per_prio[prio], next); /* @next를 rb-tree/fifo_list/병합 해시에서 완전히 제거 - 이후 blk_mq_free_request()가 실제 메모리 해제 담당 */
}

/*
 * move an entry to dispatch queue
 */
/*
 * [한국어]
 * deadline_move_request - request를 정렬/FIFO 자료구조에서 빼내어 디스패치 직전 상태로 전이.
 *
 * @dd:       스케줄러 인스턴스.
 * @per_prio: rq가 속한 우선순위의 rb-tree/fifo_list 묶음.
 * @rq:       디스패치하기로 확정된 request.
 * @return:   없음.
 *
 * __dd_dispatch_request()가 다음에 보낼 request를 확정한 직후 호출하는
 * 얇은 래퍼. deadline_remove_request()를 그대로 위임 호출해 rb-tree/
 * fifo_list/병합 해시에서 rq를 제거함으로써, 이후 dd_start_request()가
 * RQF_STARTED를 설정하고 드라이버로 넘길 준비를 마치게 한다.
 * 실행 컨텍스트: dd->lock을 쥔 __dd_dispatch_request() 안에서 호출.
 * 호출자: __dd_dispatch_request().
 * 피호출자: deadline_remove_request().
 *
 * 호출 체인:
 *   __dd_dispatch_request → [deadline_move_request] → deadline_remove_request
 */
static void
deadline_move_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      struct request *rq)
{
	/*
	 * take it off the sort and fifo list
	 */
	deadline_remove_request(rq->q, per_prio, rq); /* rq->q(이 request가 속한 request_queue)를 넘겨 rb-tree/fifo_list/병합 해시에서 제거 */
}

/* Number of requests queued for a given priority level. */
/*
 * [한국어]
 * dd_queued - 지정된 우선순위에서 "삽입되었지만 아직 완료되지 않은" 요청 수 계산.
 *
 * @dd:   스케줄러 인스턴스.
 * @prio: 조회할 우선순위(dd_prio).
 * @return: inserted - completed(부호 없는 산술이라 실제로는 항상 두 값의
 *          차이가 현재 "대기 중이거나 드라이버에서 처리 중인" 요청 수).
 *
 * inserted는 스케줄러에 새로 들어온 누적 수, completed는 드라이버 처리가
 * 끝난 누적 수이므로, 그 차이가 곧 "아직 완료되지 않고 어딘가(rb-tree/
 * fifo_list 또는 드라이버 내부)에 남아있는" 요청 수가 된다. 이 값은 이
 * 파일 곳곳에서 "아직 처리할 작업이 남았는지", "aging 검사를 할 만큼
 * 여러 우선순위가 동시에 활성 상태인지" 판단에 쓰인다.
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출(lockdep_assert_held로 강제) -
 *   completed는 atomic_t라 락 없이도 안전하지만, inserted와의 스냅숏
 *   일관성을 위해 락 보유가 요구된다.
 * 호출자: dd_dispatch_prio_aged_requests()(활성 우선순위 개수 판단),
 *   dd_dispatch_request()(우선순위 순회 중 조기 종료 판단), dd_exit_sched()
 *   (종료 시 통계 정합성 검증), dd_queued_show()(debugfs 열람).
 * 피호출자: atomic_read().
 *
 * 호출 체인:
 *   dd_dispatch_prio_aged_requests/dd_dispatch_request/dd_exit_sched/
 *   dd_queued_show → [dd_queued] → atomic_read
 */
static u32 dd_queued(struct deadline_data *dd, enum dd_prio prio)
{
	const struct io_stats_per_prio *stats = &dd->per_prio[prio].stats; /* 해당 우선순위의 통계 구조체 포인터 */

	lockdep_assert_held(&dd->lock); /* 호출자가 dd->lock을 쥐고 있어야 inserted와 completed 스냅숏이 일관됨을 보장할 수 있음 */

	return stats->inserted - atomic_read(&stats->completed); /* unsigned 뺄셈이므로 오버플로우가 나도 두 카운터의 "차이"는 올바르게 랩어라운드되어 계산됨(원본 주석 참고) */
}

/*
 * deadline_check_fifo returns true if and only if there are expired requests
 * in the FIFO list. Requires !list_empty(&dd->fifo_list[data_dir]).
 */
/*
 * [한국어]
 * deadline_check_fifo - FIFO 리스트 선두 request의 deadline 만료 여부 확인.
 *
 * @per_prio: 확인할 우선순위의 rb-tree/fifo_list 묶음.
 * @data_dir: 확인할 방향(DD_READ 또는 DD_WRITE).
 * @return:   true면 선두 request가 이미 deadline을 넘겨 즉시 디스패치되어야 함.
 *
 * fifo_list는 도착 순서(=만료 순서와 동일, fifo_time이 삽입 시각 +
 * fifo_expire로 단조 증가하므로)로 유지되므로, 선두 요소만 검사하면 리스트
 * 전체에 만료된 요청이 있는지 확인할 수 있다(선행 조건: 리스트가 비어있지
 * 않아야 함 - 호출자가 이를 보장). time_is_before_eq_jiffies()는 인자로
 * 받은 시각이 "현재 jiffies보다 같거나 이전"인지 검사하는 헬퍼다.
 * 실행 컨텍스트: dd->lock을 쥔 __dd_dispatch_request() 안에서 호출.
 * 호출자: __dd_dispatch_request()(정렬 순서 대신 FIFO 순서로 전환할지 판단).
 * 피호출자: rq_entry_fifo(), time_is_before_eq_jiffies().
 *
 * 호출 체인:
 *   __dd_dispatch_request → [deadline_check_fifo] → rq_entry_fifo,
 *     time_is_before_eq_jiffies
 */
static inline bool deadline_check_fifo(struct dd_per_prio *per_prio,
				       enum dd_data_dir data_dir)
{
	struct request *rq = rq_entry_fifo(per_prio->fifo_list[data_dir].next); /* fifo_list의 첫(가장 오래된) 노드를 struct request로 변환 - list_entry_rq와 동일한 container_of 패턴 */

	return time_is_before_eq_jiffies((unsigned long)rq->fifo_time); /* rq->fifo_time(만료 절대 시각)이 현재 jiffies 이하이면 이미 만료된 것으로 판단 */
}

/*
 * For the specified data direction, return the next request to
 * dispatch using arrival ordered lists.
 */
/*
 * [한국어]
 * deadline_fifo_request - 도착 순서(FIFO) 기준으로 다음에 디스패치할 request 반환.
 *
 * @dd:       스케줄러 인스턴스(이 함수 자체는 사용하지 않지만 호출 시그니처
 *            일관성을 위해 유지).
 * @per_prio: 조회할 우선순위의 rb-tree/fifo_list 묶음.
 * @data_dir: 조회할 방향.
 * @return:   fifo_list[data_dir]의 선두 request, 리스트가 비어있으면 NULL.
 *
 * deadline_check_fifo()가 만료를 확인한 뒤, 또는 순차 정렬 후보가 소진된
 * 뒤(deadline_next_request가 NULL을 반환한 뒤) __dd_dispatch_request()가
 * "그렇다면 가장 오래 기다린 요청부터"라는 폴백 정책으로 이 함수를 호출한다.
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출.
 * 호출자: __dd_dispatch_request(), dd_dispatch_prio_aged_requests()가
 *   호출하는 __dd_dispatch_request() 경로를 통해 간접적으로도 쓰임.
 * 피호출자: list_empty(), rq_entry_fifo().
 *
 * 호출 체인:
 *   __dd_dispatch_request → [deadline_fifo_request] → rq_entry_fifo
 */
static struct request *
deadline_fifo_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	if (list_empty(&per_prio->fifo_list[data_dir])) /* 해당 방향에 대기 중인 요청이 하나도 없으면 */
		return NULL; /* 반환할 후보가 없음을 알림 */

	return rq_entry_fifo(per_prio->fifo_list[data_dir].next); /* 리스트 선두(가장 먼저 도착한, 즉 가장 먼저 만료될) request를 반환 */
}

/*
 * For the specified data direction, return the next request to
 * dispatch using sector position sorted lists.
 */
/*
 * [한국어]
 * deadline_next_request - LBA 정렬 순서 기준으로 다음에 디스패치할 request 반환.
 *
 * @dd:       스케줄러 인스턴스(사용하지 않음 - 시그니처 일관성 유지용).
 * @per_prio: 조회할 우선순위의 rb-tree/fifo_list 묶음.
 * @data_dir: 조회할 방향.
 * @return:   latest_pos[data_dir] 이상의 LBA를 가진 첫 request, 없으면 NULL.
 *
 * per_prio->latest_pos[data_dir](마지막으로 디스패치된 요청의 시작 섹터)를
 * 기준으로 deadline_from_pos()를 호출해, "그 위치 이후 이어지는" 순차 I/O
 * 스트림의 다음 요청을 찾는다. 같은 방향으로 batching을 계속할 때
 * (__dd_dispatch_request의 배칭 유지 경로) 또는 새 batch를 시작할 때 모두
 * 이 함수로 순차성 있는 후보를 우선 탐색한다.
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출.
 * 호출자: __dd_dispatch_request().
 * 피호출자: deadline_from_pos().
 *
 * 호출 체인:
 *   __dd_dispatch_request → [deadline_next_request] → deadline_from_pos
 */
static struct request *
deadline_next_request(struct deadline_data *dd, struct dd_per_prio *per_prio,
		      enum dd_data_dir data_dir)
{
	return deadline_from_pos(per_prio, data_dir,
				 per_prio->latest_pos[data_dir]); /* 마지막 디스패치 위치 이상의 첫 request를 rb-tree에서 탐색 - 순차 IO 이어가기 */
}

/*
 * Returns true if and only if @rq started after @latest_start where
 * @latest_start is in jiffies.
 */
/*
 * [한국어]
 * started_after - @rq가 @latest_start 시각 이후에 스케줄러로 들어왔는지 확인.
 *
 * @dd:           스케줄러 인스턴스(fifo_expire[] 조회용).
 * @rq:           확인할 request.
 * @latest_start: 비교 기준 시각(jiffies 단위, 보통 "지금" 또는 "지금 -
 *                prio_aging_expire").
 * @return:        true면 @rq가 @latest_start보다 늦게 도착한 것이므로 이번
 *                 디스패치 라운드에서는 건너뛰어야 함.
 *
 * rq->fifo_time은 "도착 시각 + fifo_expire[dir]"로 계산된 절대 만료
 * 시각이므로, 여기서 fifo_expire[dir]을 다시 빼면 원래의 도착(삽입) 시각을
 * 역산할 수 있다. 이 함수는 __dd_dispatch_request()가 "latest_start 이후에
 * 도착한 요청은 아직 디스패치 대상이 아니다"라는 시간 상한을 강제하는 데
 * 쓰인다 — 특히 dd_dispatch_prio_aged_requests()가 "now - prio_aging_expire
 * 이전에 도착한 요청만" 강제 디스패치하도록 제한할 때 핵심적으로 사용된다.
 * 실행 컨텍스트: dd->lock을 쥔 __dd_dispatch_request() 안에서 호출.
 * 호출자: __dd_dispatch_request()(dispatch_request 레이블 직후).
 * 피호출자: time_after().
 *
 * 호출 체인:
 *   __dd_dispatch_request → [started_after] → time_after
 */
static bool started_after(struct deadline_data *dd, struct request *rq,
			  unsigned long latest_start)
{
	unsigned long start_time = (unsigned long)rq->fifo_time; /* rq->fifo_time(절대 만료 시각)을 시작점으로 잡음 */

	start_time -= dd->fifo_expire[rq_data_dir(rq)]; /* 만료 시각에서 해당 방향의 deadline 폭을 빼서 원래의 도착(삽입) 시각을 역산 */

	return time_after(start_time, latest_start); /* 역산된 도착 시각이 기준 시각보다 늦으면(더 최근이면) true - 아직 디스패치 대상이 아님을 의미 */
}

/*
 * [한국어]
 * dd_start_request - request를 디스패치 확정 상태로 전이시키고 통계/위치 갱신.
 *
 * @dd:       스케줄러 인스턴스.
 * @data_dir: 이 request의 방향 - latest_pos[data_dir] 갱신에 사용.
 * @rq:       디스패치 확정된 request.
 * @return:   인자로 받은 @rq를 그대로 반환(체이닝 편의를 위한 관용구).
 *
 * 이 함수가 호출되는 시점은 곧 이 request가 하드웨어 큐(hctx)로 넘어가는
 * "확정 순간"이다. (1) 이 우선순위·방향의 latest_pos를 이 request의 시작
 * 섹터로 갱신해 다음 순차 탐색(deadline_next_request)의 기준점으로 삼고,
 * (2) dispatched 통계를 늘려 outstanding 요청 수 계산에 반영하며, (3)
 * RQF_STARTED 플래그를 설정해 blk-mq/드라이버가 이 request를 "전송이
 * 시작된" 상태로 인식하게 한다(타임아웃 처리 등의 시작점).
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출.
 * 호출자: dd_dispatch_request()(예비 dispatch 리스트 경로),
 *   __dd_dispatch_request()(일반 우선순위 기반 경로).
 * 피호출자: dd_rq_ioclass().
 *
 * 호출 체인:
 *   dd_dispatch_request/__dd_dispatch_request → [dd_start_request]
 *     → dd_rq_ioclass
 */
static struct request *dd_start_request(struct deadline_data *dd,
					enum dd_data_dir data_dir,
					struct request *rq)
{
	u8 ioprio_class = dd_rq_ioclass(rq); /* 이 request의 ioprio 클래스 조회 */
	enum dd_prio prio = ioprio_class_to_prio[ioprio_class]; /* dd_prio로 변환 - 어느 per_prio[]를 갱신할지 결정 */

	dd->per_prio[prio].latest_pos[data_dir] = blk_rq_pos(rq); /* 이 request의 시작 섹터를 "마지막 디스패치 위치"로 기록 - 다음 순차 스캔의 시작점이 됨 */
	dd->per_prio[prio].stats.dispatched++; /* 디스패치 통계 증가 - dd_owned_by_driver()의 outstanding 계산에 반영 */
	rq->rq_flags |= RQF_STARTED; /* blk-mq에 이 request의 전송이 시작되었음을 알리는 플래그 설정 - 타임아웃 감시 등의 기준이 됨 */
	return rq; /* 확정된 request를 그대로 돌려주어 호출자가 이어서 반환할 수 있게 함 */
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc and with a start time <= @latest_start.
 */
/*
 * [한국어]
 * __dd_dispatch_request - 지정된 우선순위(per_prio)에서 디스패치할 다음
 * request 하나를 선택하는 핵심 정책 함수.
 *
 * @dd:           스케줄러 인스턴스(last_dir/batching/starved 등 전역 상태).
 * @per_prio:     대상 우선순위의 rb-tree/fifo_list.
 * @latest_start: 이 시각 이후에 도착한 요청은 선택 대상에서 제외(started_after
 *                로 검사) - 일반 호출은 "now"(사실상 제한 없음), prio-aging
 *                호출은 "now - prio_aging_expire"(오래 기다린 것만 허용).
 * @return: 선택된 request(디스패치 확정, RQF_STARTED 설정됨), 선택할 것이
 *          없으면 NULL.
 *
 * 이 함수의 결정 트리는 다음과 같다.
 *   1) 아직 batching 한도(dd->batching < dd->fifo_batch) 내이고 이전과 같은
 *      방향(dd->last_dir)으로 이어갈 순차 후보가 있으면 그대로 그 방향을
 *      유지한다(dispatch_request로 점프) - seek 최소화/SQ doorbell 절약.
 *   2) 그렇지 않으면 새 batch를 시작해야 하므로 방향을 다시 정한다: read
 *      대기열이 있으면 원칙적으로 read를 선택하되, write가 writes_starved
 *      한도만큼 굶었으면(dd->starved 카운터) write로 강제 전환한다
 *      (starvation 방지, dispatch_writes 레이블).
 *   3) 선택된 방향에서, deadline이 만료됐거나(deadline_check_fifo) 순차
 *      후보가 없으면(next_rq NULL) FIFO(가장 오래 기다린 것) 순서로,
 *      아니면 rb-tree 정렬(순차 IO 이어가기) 순서로 실제 request를 뽑는다.
 *   4) 마지막으로 started_after()로 @latest_start 제약을 확인해, 이 제약을
 *      만족하지 못하면(너무 최근에 도착) NULL을 반환한다(단, 배칭 유지
 *      경로로 들어온 경우도 동일 검사를 거친다).
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출(lockdep_assert_held로 강제).
 * 호출자: dd_dispatch_prio_aged_requests()(BE/IDLE에 aging 제약을 걸어 호출),
 *   dd_dispatch_request()(우선순위 순회 중 제약 없이 호출).
 * 피호출자: deadline_next_request(), deadline_fifo_request(),
 *   deadline_check_fifo(), started_after(), deadline_move_request(),
 *   dd_start_request().
 * 에러 경로: "선택할 request 없음"은 에러가 아니라 정상 경로(NULL 반환) -
 *   호출자가 다음 우선순위를 시도하거나 디스패치를 포기한다.
 *
 * 호출 체인:
 *   dd_dispatch_prio_aged_requests/dd_dispatch_request → [__dd_dispatch_request]
 *     → deadline_next_request/deadline_fifo_request/deadline_check_fifo/
 *       started_after/deadline_move_request/dd_start_request
 */
static struct request *__dd_dispatch_request(struct deadline_data *dd,
					     struct dd_per_prio *per_prio,
					     unsigned long latest_start)
{
	struct request *rq, *next_rq; /* rq: 최종 선택될 request, next_rq: rb-tree 순차 정렬 기준으로 찾은 후보(중간 변수) */
	enum dd_data_dir data_dir; /* 이번 라운드에 선택할 방향(read/write) */

	lockdep_assert_held(&dd->lock); /* 호출자가 dd->lock을 쥔 상태여야 함을 런타임에 검증 */

	/*
	 * batches are currently reads XOR writes
	 */
	rq = deadline_next_request(dd, per_prio, dd->last_dir); /* 이전과 같은 방향(dd->last_dir)으로 이어지는 순차 후보를 먼저 조회 */
	if (rq && dd->batching < dd->fifo_batch) { /* 순차 후보가 있고 아직 이번 batch에서 fifo_batch만큼 다 채우지 않았다면 */
		/* we have a next request and are still entitled to batch */
		data_dir = rq_data_dir(rq); /* 방향은 방금 찾은 rq의 방향(=dd->last_dir과 동일)으로 설정 */
		goto dispatch_request; /* 방향 재선택 로직을 건너뛰고 곧바로 최종 확정 단계로 이동 - batching 계속 */
	}

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (!list_empty(&per_prio->fifo_list[DD_READ])) { /* read 대기열에 요청이 하나라도 있으면 read를 우선 고려 */
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_READ])); /* fifo_list에 read가 있는데 sort_list가 비어있으면 두 자료구조 간 불변조건이 깨진 것 - 커널 버그이므로 즉시 패닉 */

		if (deadline_fifo_request(dd, per_prio, DD_WRITE) &&
		    (dd->starved++ >= dd->writes_starved)) /* write 대기열도 존재하고, read가 write를 writes_starved 횟수만큼 이미 앞질렀다면(부작용으로 dd->starved 1 증가) */
			goto dispatch_writes; /* starvation 방지를 위해 read 대신 write로 강제 전환 */

		data_dir = DD_READ; /* write 대기열이 없거나 아직 starvation 한도 내이면 예정대로 read 선택 */

		goto dispatch_find_request; /* 선택된 방향(read)에서 실제 request를 찾는 단계로 이동 */
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (!list_empty(&per_prio->fifo_list[DD_WRITE])) { /* read가 없거나 위에서 write로 전환된 경우 - write 대기열 존재 여부 확인 */
dispatch_writes: /* read starvation 한도 초과로 위에서 강제 점프해 들어오는 지점 */
		BUG_ON(RB_EMPTY_ROOT(&per_prio->sort_list[DD_WRITE])); /* fifo_list에 write가 있는데 sort_list가 비어있으면 불변조건 위반 - 패닉 */

		dd->starved = 0; /* write를 선택했으므로 starvation 카운터를 리셋해 다음 read 우선 구간을 새로 시작 */

		data_dir = DD_WRITE; /* 이번 라운드는 write 방향으로 확정 */

		goto dispatch_find_request; /* 선택된 방향(write)에서 실제 request를 찾는 단계로 이동 */
	}

	return NULL; /* read/write 대기열이 모두 비어있으면 이 우선순위에서 디스패치할 것이 없음 */

dispatch_find_request: /* 방향(data_dir)은 정해졌고 그 방향에서 구체적으로 어느 request를 뽑을지 결정하는 단계 */
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	next_rq = deadline_next_request(dd, per_prio, data_dir); /* 순차 정렬 기준 다음 후보를 조회(현재는 아직 정해진 latest_pos 기준) */
	if (deadline_check_fifo(per_prio, data_dir) || !next_rq) { /* deadline이 만료됐거나(우선 처리 필요) 순차 후보가 아예 없으면(끝까지 스캔함) */
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		rq = deadline_fifo_request(dd, per_prio, data_dir); /* FIFO 순서로 되돌아가 가장 오래 기다린 요청부터 다시 선택 */
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		rq = next_rq; /* 만료된 것도 없고 순차 후보도 있으므로 정렬 순서를 그대로 이어감(seek 최소화) */
	}

	if (!rq) /* 위 두 경로 모두에서 결국 후보를 못 찾은 경우(이론상 fifo_list가 비었으면 발생 가능) */
		return NULL; /* 디스패치할 것이 없음을 알림 */

	dd->last_dir = data_dir; /* 이번에 확정된 방향을 기록 - 다음 호출에서 배칭 유지 판단 기준이 됨 */
	dd->batching = 0; /* 새 batch가 시작되므로 배칭 카운터를 0으로 리셋 */

dispatch_request: /* 배칭 유지 경로(위쪽 조기 goto)와 새 batch 시작 경로가 합류하는 최종 확정 지점 */
	if (started_after(dd, rq, latest_start)) /* 선택된 rq가 @latest_start 기준보다 늦게 도착했다면(prio-aging 등에서 너무 최근 요청) */
		return NULL; /* 이번 라운드에서는 디스패치하지 않음 - 다음 기회로 미룸 */

	/*
	 * rq is the selected appropriate request.
	 */
	dd->batching++; /* 배칭 카운터 증가 - fifo_batch에 도달하면 다음 호출에서 방향을 재평가하게 됨 */
	deadline_move_request(dd, per_prio, rq); /* 확정된 rq를 rb-tree/fifo_list/병합 해시에서 제거 */
	return dd_start_request(dd, data_dir, rq); /* latest_pos/통계 갱신과 RQF_STARTED 설정을 마친 뒤 최종 request를 반환 */
}

/*
 * Check whether there are any requests with priority other than DD_RT_PRIO
 * that were inserted more than prio_aging_expire jiffies ago.
 */
/*
 * [한국어]
 * dd_dispatch_prio_aged_requests - RT가 아닌(BE/IDLE) 우선순위 중
 * prio_aging_expire보다 오래 대기한 요청이 있으면 우선 디스패치.
 *
 * @dd:  스케줄러 인스턴스.
 * @now: 현재 시각(jiffies, 호출자가 한 번만 읽어 전달 - 일관된 기준 시각
 *       사용을 위해).
 * @return: aging 대상에서 선택된 request, 없으면 NULL.
 *
 * RT 우선순위가 계속 활성 상태이면 BE/IDLE 우선순위는 dd_dispatch_request()의
 * 순회 로직상 영원히 뒤로 밀릴 수 있다. 이를 막기 위해, "활성 우선순위가
 * 2개 이상"인 경우에 한해(RT만 활성이면 애초에 뒤로 밀릴 다른 우선순위가
 * 없으므로 검사 자체가 무의미) BE부터 IDLE까지 순서대로
 * __dd_dispatch_request()를 "now - prio_aging_expire"라는 엄격한 시간
 * 상한으로 호출한다. 즉 prio_aging_expire보다 더 오래 전에 도착한 요청만
 * 디스패치 대상으로 인정하며, 그런 요청이 있으면 우선순위 순서를 깨고
 * 즉시 반환한다.
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출.
 * 호출자: dd_dispatch_request().
 * 피호출자: dd_queued(), __dd_dispatch_request().
 *
 * 호출 체인:
 *   dd_dispatch_request → [dd_dispatch_prio_aged_requests]
 *     → dd_queued, __dd_dispatch_request
 */
static struct request *dd_dispatch_prio_aged_requests(struct deadline_data *dd,
						      unsigned long now)
{
	struct request *rq; /* 최종 반환할(있다면) request */
	enum dd_prio prio; /* 순회 인덱스 - BE부터 IDLE까지 */
	int prio_cnt; /* 현재 대기 중인 요청이 있는 우선순위의 개수(0~3) */

	lockdep_assert_held(&dd->lock); /* 호출자가 dd->lock을 쥐고 있어야 함을 검증 */

	prio_cnt = !!dd_queued(dd, DD_RT_PRIO) + !!dd_queued(dd, DD_BE_PRIO) +
		   !!dd_queued(dd, DD_IDLE_PRIO); /* 각 우선순위별 dd_queued()가 0보다 크면 1로 강제(!!)하여 "활성 우선순위 개수"를 셈 */
	if (prio_cnt < 2) /* 활성 우선순위가 하나뿐이거나 전혀 없으면 - 서로 경쟁할 다른 우선순위가 없음 */
		return NULL; /* aging 강제 디스패치가 의미 없으므로 즉시 포기 */

	for (prio = DD_BE_PRIO; prio <= DD_PRIO_MAX; prio++) { /* RT는 제외(원본 주석 "priority other than DD_RT_PRIO")하고 BE, IDLE 순으로 순회 */
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio],
					   now - dd->prio_aging_expire); /* "now - prio_aging_expire"보다 이전에 도착한 요청만 허용하는 엄격한 시간 상한으로 조회 */
		if (rq) /* 이 우선순위에서 aging 조건을 만족하는 요청을 찾았다면 */
			return rq; /* 즉시 반환 - 더 낮은 우선순위는 검사하지 않음(첫 매치 우선) */
	}

	return NULL; /* BE, IDLE 어디에도 aging 대상이 없으면 정상 우선순위 순회로 넘어가도록 NULL 반환 */
}

/*
 * Called from blk_mq_run_hw_queue() -> __blk_mq_sched_dispatch_requests().
 *
 * One confusing aspect here is that we get called for a specific
 * hardware queue, but we may return a request that is for a
 * different hardware queue. This is because mq-deadline has shared
 * state for all hardware queues, in terms of sorting, FIFOs, etc.
 */
/*
 * [한국어]
 * dd_dispatch_request - elevator_mq_ops.dispatch_request 콜백. 이
 * 하드웨어 큐(hctx)의 디스패치 라운드에서 보낼 request 하나를 선택.
 *
 * @hctx: blk_mq_run_hw_queue()가 처리 중인 하드웨어 큐. 원본 영어 주석이
 *        강조하듯, mq-deadline은 request_queue 전체에서 단일
 *        deadline_data를 공유하므로(QUEUE_FLAG_SQ_SCHED) 이 함수가 반환하는
 *        request는 @hctx가 아닌 "다른" hctx에 속했던 것일 수도 있다.
 * @return: 디스패치할 request, 없으면 NULL(blk-mq가 이 hctx를 idle로 간주).
 *
 * 선택 순서는 다음과 같은 우선순위를 갖는다.
 *   1) dd->dispatch(예비 리스트)가 비어있지 않으면 무조건 그 선두를 먼저
 *      내보낸다 - requeue된 요청 등 즉시 재시도가 필요한 것들.
 *   2) dd_dispatch_prio_aged_requests()로 BE/IDLE 중 너무 오래 기다린
 *      요청이 있는지 확인 - 있으면 우선순위 순서를 깨고 그것을 보낸다.
 *   3) 위 둘 다 없으면 RT → BE → IDLE 순으로 __dd_dispatch_request()를
 *      호출하되, 어떤 우선순위에서 "선택된 request가 있거나(rq) 아직 대기
 *      중인 요청이 남아있으면(dd_queued)" 그 자리에서 순회를 멈춘다 - 더
 *      낮은 우선순위는 더 높은 우선순위에 남은 작업이 있는 한 절대
 *      건드리지 않는다(원본 주석 "Ignore lower priority requests if any
 *      higher priority requests are pending").
 * 실행 컨텍스트: blk_mq_run_hw_queue() → __blk_mq_sched_dispatch_requests()
 *   경로에서 호출되며, 이 함수 자신이 dd->lock을 잡고 푼다(호출자는 락을
 *   쥐지 않은 상태로 진입).
 * 호출자: block/blk-mq-sched.c의 __blk_mq_sched_dispatch_requests()
 *   (elevator_mq_ops.dispatch_request 경유).
 * 피호출자: dd_start_request(), dd_dispatch_prio_aged_requests(),
 *   __dd_dispatch_request(), dd_queued().
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue → __blk_mq_sched_dispatch_requests →
 *     ops.dispatch_request=[dd_dispatch_request] →
 *     dd_dispatch_prio_aged_requests/__dd_dispatch_request →
 *     blk_mq_dispatch_rq_list → q->mq_ops->queue_rq(예: nvme_queue_rq)
 */
static struct request *dd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data; /* hctx->queue(request_queue) 전체에서 공유하는 스케줄러 인스턴스를 조회 */
	const unsigned long now = jiffies; /* 이번 디스패치 라운드 전체에서 일관되게 쓸 기준 시각을 한 번만 샘플링 */
	struct request *rq; /* 최종 반환할 request */
	enum dd_prio prio; /* 우선순위 순회 인덱스 */

	spin_lock(&dd->lock); /* dd 전체(예비 리스트/per_prio[]/통계)에 대한 배타적 접근 시작 - 다른 hctx의 동시 디스패치/삽입과 경쟁 방지 */

	if (!list_empty(&dd->dispatch)) { /* 예비 dispatch 리스트에 즉시 내보낼 요청이 있는지 확인(requeue 등으로 head에 삽입된 것) */
		rq = list_first_entry(&dd->dispatch, struct request, queuelist); /* 리스트 선두 request 획득 */
		list_del_init(&rq->queuelist); /* 리스트에서 제거 */
		dd_start_request(dd, rq_data_dir(rq), rq); /* latest_pos/통계 갱신 및 RQF_STARTED 설정 - 반환값은 rq와 동일하므로 사용하지 않음 */
		goto unlock; /* 아래의 우선순위 기반 로직은 건너뛰고 곧바로 락 해제 및 반환 */
	}

	rq = dd_dispatch_prio_aged_requests(dd, now); /* BE/IDLE 중 prio_aging_expire를 초과해 대기한 요청이 있는지 확인 */
	if (rq) /* aging 대상 요청을 찾았다면 */
		goto unlock; /* 그것을 그대로 반환하도록 락 해제 단계로 이동 */

	/*
	 * Next, dispatch requests in priority order. Ignore lower priority
	 * requests if any higher priority requests are pending.
	 */
	for (prio = 0; prio <= DD_PRIO_MAX; prio++) { /* RT(0) → BE(1) → IDLE(2) 순서로 순회 */
		rq = __dd_dispatch_request(dd, &dd->per_prio[prio], now); /* 이 우선순위에서 시간 제약 없이(now 기준, 사실상 제한 없음) 다음 request 선택 시도 */
		if (rq || dd_queued(dd, prio)) /* 선택에 성공했거나, 실패했더라도 이 우선순위에 아직 처리되지 않은 요청이 남아있다면 */
			break; /* 더 낮은 우선순위는 검사하지 않고 순회 종료 - 우선순위 역전 방지 */
	}

unlock: /* 세 경로(예비 리스트/aging/우선순위 순회) 모두가 합류하는 락 해제 지점 */
	spin_unlock(&dd->lock); /* dd 전체에 대한 배타적 접근 종료 */

	return rq; /* 선택된 request(없으면 NULL)를 blk-mq에 반환 - blk-mq가 이후 hctx 디스패치 리스트로 옮겨 드라이버 queue_rq를 호출 */
}

/*
 * [한국어]
 * dd_limit_depth - elevator_mq_ops.limit_depth 콜백. 비동기(non sync-read)
 * 요청의 tag 할당 깊이를 async_depth로 제한.
 *
 * @opf:  할당하려는 request의 operation flags(REQ_OP_* 및 REQ_* 플래그 조합).
 * @data: tag 할당 in/out 매개변수 - shallow_depth 필드에 제한값을 써 넣는다.
 * @return: 없음.
 *
 * 동기 read는 호출자가 즉시 결과를 기다리는 지연시간 최우선 요청이므로
 * 제한을 걸지 않지만, 그 외(비동기 read, write 등)는 q->async_depth로
 * shallow_depth를 제한해 tag 풀의 앞쪽 일부 범위에서만 tag를 뽑도록
 * 강제한다(뒤쪽 tag는 동기 read를 위해 항상 여유를 남겨둠). 이렇게 하면
 * 비동기/쓰기 요청이 tag 풀 전체를 소진해 뒤이어 도착하는 지연시간 민감
 * 동기 read가 tag 부족으로 대기하는 상황을 방지한다.
 * 실행 컨텍스트: request 할당 경로(blk_mq_get_tag() 이전)에서 호출 -
 *   프로세스 컨텍스트, 락 불필요(data는 호출자 스택 값).
 * 호출자: block/blk-mq.c의 blk_mq_limit_depth()(elevator_mq_ops.limit_depth
 *   경유).
 * 피호출자: blk_mq_is_sync_read().
 *
 * 호출 체인:
 *   blk_mq_get_request → blk_mq_limit_depth → ops.limit_depth=[dd_limit_depth]
 *     → blk_mq_is_sync_read
 */
static void dd_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data)
{
	if (!blk_mq_is_sync_read(opf)) /* 동기 read가 "아닌" 모든 요청(비동기 read, 모든 write 등)에 대해 */
		data->shallow_depth = data->q->async_depth; /* tag 탐색 범위를 async_depth로 제한 - 동기 read를 위한 tag 여유분을 항상 확보 */
}

/* Called by blk_mq_init_sched() and blk_mq_update_nr_requests(). */
/*
 * [한국어]
 * dd_depth_updated - elevator_mq_ops.depth_updated 콜백. async_depth 값이
 * 바뀔 때마다 sched_tags 비트맵의 최소 shallow depth를 재적용.
 *
 * @q: 대상 request_queue.
 * @return: 없음.
 *
 * q->async_depth는 사용자가 nr_requests(대기열 깊이) sysfs 파일을 조정하거나
 * dd_init_sched()가 초기화할 때 함께 갱신되는데, 그때마다 실제로 모든
 * hctx의 sched_tags 비트맵에 그 하한값을 다시 적용해야 dd_limit_depth()가
 * 설정한 shallow_depth 제한이 실제 tag 할당 동작에 반영된다.
 * 실행 컨텍스트: nr_requests 변경 syscall 경로(프로세스 컨텍스트) 또는
 *   dd_init_sched() 안에서 동기 호출.
 * 호출자: block/blk-mq-sched.c의 blk_mq_init_sched()(스케줄러 최초 연결
 *   시), blk_mq_update_nr_requests()(nr_requests sysfs 조정 시), 그리고 이
 *   파일의 dd_init_sched().
 * 피호출자: blk_mq_set_min_shallow_depth().
 *
 * 호출 체인:
 *   blk_mq_init_sched/blk_mq_update_nr_requests/dd_init_sched →
 *     ops.depth_updated=[dd_depth_updated] → blk_mq_set_min_shallow_depth
 */
static void dd_depth_updated(struct request_queue *q)
{
	blk_mq_set_min_shallow_depth(q, q->async_depth); /* 모든 hctx의 sched_tags 비트맵에 대해 shallow depth 하한을 q->async_depth로 재설정 */
}

/*
 * [한국어]
 * dd_exit_sched - elevator_mq_ops.exit_sched 콜백. 스케줄러 인스턴스 해제와
 * 정합성 검증.
 *
 * @e: 해제할 elevator_queue(e->elevator_data가 이 파일의 struct deadline_data).
 * @return: 없음.
 *
 * 스케줄러가 다른 것으로 교체되거나(elevator switch) 디스크가 제거될 때
 * 호출된다. 각 우선순위에 대해 (1) fifo_list가 완전히 비어있어야 함을
 * WARN_ON_ONCE로 확인하고(비어있지 않다면 아직 완료 처리되지 않은 요청이
 * 스케줄러 안에 남아있다는 뜻 - 버그 징후), (2) dd_queued()(inserted -
 * completed)가 0이어야 함을 WARN_ONCE로 확인한다(0이 아니면 삽입/완료
 * 카운트가 불일치 - 통계 버그 또는 요청 누락 징후). 마지막으로 struct
 * deadline_data 자체를 kfree()로 해제한다.
 * 실행 컨텍스트: elevator switch(sysfs를 통한 스케줄러 교체) 또는 디스크
 * 제거 경로 - 프로세스 컨텍스트, 이 시점에는 새 I/O가 더 이상 들어오지
 * 않음이 상위 계층(block/elevator.c)에 의해 보장된다.
 * 호출자: block/blk-mq-sched.c의 blk_mq_exit_sched()(elevator_mq_ops.exit_sched
 *   경유).
 * 피호출자: dd_queued(), kfree().
 * 에러 경로: WARN_ON_ONCE/WARN_ONCE는 커널 로그에 경고만 남기고 계속
 *   진행한다(치명적 에러로 취급하지 않음 - 스케줄러 해제 자체는 막지 않음).
 *
 * 호출 체인:
 *   blk_mq_exit_sched → ops.exit_sched=[dd_exit_sched] → dd_queued, kfree
 */
static void dd_exit_sched(struct elevator_queue *e)
{
	struct deadline_data *dd = e->elevator_data; /* 이 elevator_queue에 연결된 스케줄러 인스턴스 */
	enum dd_prio prio; /* 우선순위 순회 인덱스 */

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) { /* RT, BE, IDLE 세 우선순위 모두에 대해 정합성 검사 */
		struct dd_per_prio *per_prio = &dd->per_prio[prio]; /* 이 우선순위의 rb-tree/fifo_list/통계 묶음 */
		const struct io_stats_per_prio *stats = &per_prio->stats; /* 경고 메시지 출력용 통계 포인터 */
		uint32_t queued; /* dd_queued() 결과를 담을 임시 변수 */

		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_READ])); /* read fifo_list가 비어있지 않으면 한 번만 경고 - 완료되지 않은 요청이 남아있다는 버그 징후 */
		WARN_ON_ONCE(!list_empty(&per_prio->fifo_list[DD_WRITE])); /* write fifo_list도 동일하게 확인 */

		spin_lock(&dd->lock); /* dd_queued()가 요구하는 lockdep_assert_held 조건을 만족시키기 위해 잠금 */
		queued = dd_queued(dd, prio); /* inserted - completed 계산 - 0이 아니면 통계 불일치 */
		spin_unlock(&dd->lock); /* 짧은 조회 구간이므로 즉시 해제 */

		WARN_ONCE(queued != 0,
			  "statistics for priority %d: i %u m %u d %u c %u\n",
			  prio, stats->inserted, stats->merged,
			  stats->dispatched, atomic_read(&stats->completed)); /* queued가 0이 아니면 어느 우선순위에서 얼마나 불일치했는지 4개 카운터를 모두 로그로 남겨 디버깅에 활용 */
	}

	kfree(dd); /* struct deadline_data 자체를 해제 - 이 시점에는 모든 하위 자료구조가 이미 비어있어야 함(위 검사로 보증) */
}

/*
 * initialize elevator private data (deadline_data).
 */
/*
 * [한국어]
 * dd_init_sched - elevator_mq_ops.init_sched 콜백. struct deadline_data를
 * 할당하고 초기 상태로 설정.
 *
 * @q:  이 스케줄러가 연결될 request_queue.
 * @eq: block/blk-mq-sched.c가 미리 할당해 둔 elevator_queue 컨테이너 -
 *      eq->elevator_data에 이 함수가 만든 struct deadline_data를 저장해야 함.
 * @return: 0(성공) 또는 -ENOMEM(할당 실패).
 *
 * 스케줄러가 처음 이 request_queue에 연결될 때(디스크 초기화 또는 elevator
 * switch) 호출된다. (1) q->node(NUMA 노드)에 로컬로 struct deadline_data를
 * 할당, (2) 예비 dispatch 리스트와 세 우선순위 각각의 fifo_list/sort_list를
 * 빈 상태로 초기화, (3) fifo_expire/writes_starved/front_merges/last_dir/
 * fifo_batch/prio_aging_expire를 전역 기본 상수로 설정, (4) 락 초기화,
 * (5) QUEUE_FLAG_SQ_SCHED를 설정해 "이 큐는 하드웨어 큐 단위가 아니라
 * request_queue 전체 단위로 디스패치한다"는 것을 blk-mq에 알리고, (6)
 * async_depth를 nr_requests로 초기화한 뒤 dd_depth_updated()로 실제
 * sched_tags 비트맵에 반영한다.
 * 실행 컨텍스트: 디스크 등록 또는 elevator switch 경로의 프로세스
 * 컨텍스트 - 아직 I/O가 들어오지 않는(또는 quiesce된) 상태에서 호출되므로
 * dd->lock 없이 초기화해도 안전하다.
 * 호출자: block/blk-mq-sched.c의 blk_mq_init_sched()(elevator_mq_ops.init_sched
 *   경유).
 * 피호출자: kzalloc_node(), INIT_LIST_HEAD(), spin_lock_init(),
 *   blk_queue_flag_set(), dd_depth_updated().
 * 에러 경로: kzalloc_node() 실패 시 -ENOMEM을 반환하며, 이 경우
 *   eq->elevator_data는 건드리지 않은 채 즉시 반환 - 호출자(blk_mq_init_sched)가
 *   실패 처리를 담당.
 *
 * 호출 체인:
 *   blk_mq_init_sched → ops.init_sched=[dd_init_sched] →
 *     kzalloc_node/INIT_LIST_HEAD/spin_lock_init/blk_queue_flag_set/
 *     dd_depth_updated
 */
static int dd_init_sched(struct request_queue *q, struct elevator_queue *eq)
{
	struct deadline_data *dd; /* 새로 할당할 스케줄러 인스턴스 */
	enum dd_prio prio; /* 초기화 루프 인덱스 */

	dd = kzalloc_node(sizeof(*dd), GFP_KERNEL, q->node); /* q->node(이 큐가 선호하는 NUMA 노드)에 0으로 채워진 struct deadline_data 할당 - zalloc이므로 별도 대입 없는 필드는 자동으로 0/NULL */
	if (!dd) /* 메모리 부족으로 할당 실패 시 */
		return -ENOMEM; /* 표준 커널 에러코드 반환 - 호출자가 elevator switch 실패 등으로 처리 */

	eq->elevator_data = dd; /* 이후 모든 콜백이 q->elevator->elevator_data로 이 인스턴스를 찾을 수 있도록 연결 */

	INIT_LIST_HEAD(&dd->dispatch); /* 예비 dispatch 리스트를 빈 자기순환 리스트로 초기화 */
	for (prio = 0; prio <= DD_PRIO_MAX; prio++) { /* RT, BE, IDLE 세 우선순위 각각에 대해 */
		struct dd_per_prio *per_prio = &dd->per_prio[prio]; /* 초기화할 대상 우선순위 슬롯 */

		INIT_LIST_HEAD(&per_prio->fifo_list[DD_READ]); /* read fifo_list를 빈 상태로 초기화 */
		INIT_LIST_HEAD(&per_prio->fifo_list[DD_WRITE]); /* write fifo_list를 빈 상태로 초기화 */
		per_prio->sort_list[DD_READ] = RB_ROOT; /* read rb-tree를 빈 트리(RB_ROOT)로 초기화 */
		per_prio->sort_list[DD_WRITE] = RB_ROOT; /* write rb-tree를 빈 트리로 초기화 */
	}
	dd->fifo_expire[DD_READ] = read_expire; /* read deadline 기본값(HZ/2) 적용 */
	dd->fifo_expire[DD_WRITE] = write_expire; /* write deadline 기본값(5*HZ) 적용 */
	dd->writes_starved = writes_starved; /* write starvation 한도 기본값(2) 적용 */
	dd->front_merges = 1; /* front merge 기본 허용(1) */
	dd->last_dir = DD_WRITE; /* 초기 방향을 write로 설정 - 최초 호출 시 deadline_next_request가 last_dir=WRITE 기준으로 조회하지만 아직 아무 것도 없으므로 실질적 영향은 없고, 첫 배칭 판단의 시작값 역할만 함 */
	dd->fifo_batch = fifo_batch; /* batching 상한 기본값(16) 적용 */
	dd->prio_aging_expire = prio_aging_expire; /* prio-aging 한도 기본값(10*HZ) 적용 */
	spin_lock_init(&dd->lock); /* dd 전체를 보호할 스핀락 초기화 */

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q); /* "이 큐는 hctx 단위가 아니라 request_queue 전체 단위로 디스패치한다"는 플래그 설정 - mq-deadline이 모든 hctx에서 dd->lock 하나를 공유하기 때문에 필요 */

	q->elevator = eq; /* request_queue가 이 elevator_queue를 사용하도록 연결 */
	q->async_depth = q->nr_requests; /* 비동기 요청의 tag 탐색 상한을 일단 nr_requests(대기열 전체 깊이)와 동일하게 설정 - 이후 sysfs/재조정으로 좁혀질 수 있음 */
	dd_depth_updated(q); /* 방금 설정한 async_depth를 실제 sched_tags 비트맵의 shallow depth 하한으로 반영 */
	return 0; /* 초기화 성공 */
}

/*
 * Try to merge @bio into an existing request. If @bio has been merged into
 * an existing request, store the pointer to that request into *@rq.
 */
/*
 * [한국어]
 * dd_request_merge - elevator_mq_ops.request_merge 콜백. @bio를 기존
 * request의 "앞쪽"(front merge)에 병합할 수 있는지 탐색.
 *
 * @q:   request_queue.
 * @rq:  성공 시 병합 대상 request 포인터를 저장할 out 매개변수.
 * @bio: 병합을 시도할 새 bio.
 * @return: ELEVATOR_NO_MERGE(병합 불가), ELEVATOR_FRONT_MERGE(front merge
 *          가능, *rq에 대상 저장), ELEVATOR_DISCARD_MERGE(discard 요청끼리의
 *          특수 병합, *rq에 대상 저장).
 *
 * front merge란 @bio의 끝(bio_end_sector)이 기존 request의 시작 섹터와
 * 정확히 맞닿아, @bio가 그 request의 "앞쪽으로" 이어붙는 경우다. front_merges
 * 튜너블이 꺼져 있으면 아예 탐색하지 않는다. rb-tree(sort_list)는 시작
 * 섹터로 정렬되어 있으므로, elv_rb_find()로 "정확히 @bio의 끝 섹터에서
 * 시작하는" request를 O(log N)에 찾는다. 찾았다면 elv_bio_merge_ok()로
 * 하드웨어 제약(최대 세그먼트 수, 정렬 등)과 방향 일치 여부를 다시 한 번
 * 검증한 뒤 병합 종류를 반환한다.
 * 실행 컨텍스트: dd->bio_merge 또는 dd_bio_merge()가 쥔 dd->lock 안에서
 *   elv_merge() 경유로 호출됨.
 * 호출자: block/elevator.c의 elv_merge()(elevator_mq_ops.request_merge 경유,
 *   dd_bio_merge()가 호출하는 blk_mq_sched_try_merge() 내부에서).
 * 피호출자: dd_rq_ioclass() 없이 직접 IOPRIO_PRIO_CLASS() 사용,
 *   elv_rb_find(), elv_bio_merge_ok(), blk_discard_mergable().
 *
 * 호출 체인:
 *   dd_bio_merge → blk_mq_sched_try_merge → elv_merge →
 *     ops.request_merge=[dd_request_merge] → elv_rb_find/elv_bio_merge_ok
 */
static int dd_request_merge(struct request_queue *q, struct request **rq,
			    struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data; /* 이 스케줄러 인스턴스 */
	const u8 ioprio_class = IOPRIO_PRIO_CLASS(bio->bi_ioprio); /* bio 자체에 저장된 ioprio에서 클래스 추출 - 아직 request가 없으므로 req_get_ioprio 대신 bio->bi_ioprio 직접 사용 */
	const enum dd_prio prio = ioprio_class_to_prio[ioprio_class]; /* dd_prio로 변환 */
	struct dd_per_prio *per_prio = &dd->per_prio[prio]; /* 탐색 대상 우선순위의 rb-tree */
	sector_t sector = bio_end_sector(bio); /* @bio가 끝나는 섹터 - front merge 대상은 이 섹터에서 "시작"해야 함 */
	struct request *__rq; /* 탐색으로 찾은 후보 request(있다면) */

	if (!dd->front_merges) /* front merge 기능 자체가 꺼져 있으면 */
		return ELEVATOR_NO_MERGE; /* 탐색 없이 즉시 병합 불가 반환 */

	__rq = elv_rb_find(&per_prio->sort_list[bio_data_dir(bio)], sector); /* bio와 같은 방향의 rb-tree에서 정확히 sector에서 시작하는 request를 탐색 */
	if (__rq) { /* 정확히 일치하는 시작 섹터를 가진 request를 찾았다면 */
		BUG_ON(sector != blk_rq_pos(__rq)); /* elv_rb_find()의 반환 불변조건(정확히 일치) 확인 - 어긋나면 rb-tree 구현 버그이므로 패닉 */

		if (elv_bio_merge_ok(__rq, bio)) { /* 하드웨어 제약/방향 등 병합 가능 여부를 한 번 더 확인 */
			*rq = __rq; /* out 매개변수에 병합 대상 저장 - 호출자가 이후 실제 병합(bio_attempt_front_merge 등)에 사용 */
			if (blk_discard_mergable(__rq)) /* discard(TRIM) 요청끼리의 병합은 일반 front/back merge와 다른 특수 규칙을 따름 */
				return ELEVATOR_DISCARD_MERGE; /* discard 전용 병합 종류로 알림 */
			return ELEVATOR_FRONT_MERGE; /* 일반적인 front merge 가능 알림 */
		}
	}

	return ELEVATOR_NO_MERGE; /* 후보가 없거나 elv_bio_merge_ok 검증에 실패하면 병합 불가 */
}

/*
 * Attempt to merge a bio into an existing request. This function is called
 * before @bio is associated with a request.
 */
/*
 * [한국어]
 * dd_bio_merge - elevator_mq_ops.bio_merge 콜백. bio가 아직 request로
 * 변환되기 전, 기존 request와 병합을 시도.
 *
 * @q:        request_queue.
 * @bio:      병합을 시도할 bio.
 * @nr_segs:  이 bio가 만들어내는 세그먼트(불연속 메모리 조각) 수 - 병합 후
 *            하드웨어 세그먼트 제한 재검사에 사용.
 * @return:   true면 병합 성공(@bio는 어떤 기존 request에 흡수됨), false면
 *            실패(호출자가 새 request를 할당해야 함).
 *
 * blk_mq_submit_bio()가 새 request를 만들기 전에 먼저 시도하는 "빠른
 * 경로"다. dd->lock을 잡은 채 공용 헬퍼 blk_mq_sched_try_merge()를 호출하는데,
 * 이 헬퍼는 내부적으로 끝 섹터 해시(elv_rqhash) 기반 back-merge와 이 파일의
 * dd_request_merge()가 제공하는 front-merge를 모두 시도한다. 병합 과정에서
 * 다른 request가 병합되어 불필요해지면(free) 락 해제 후 blk_mq_free_request()로
 * 반환한다.
 * 실행 컨텍스트: bio 제출 경로(프로세스 컨텍스트)에서 dd->lock을 이 함수
 *   내부에서 직접 획득/해제.
 * 호출자: block/blk-mq-sched.c의 __blk_mq_sched_bio_merge()(elevator_mq_ops.bio_merge
 *   경유, blk_mq_submit_bio()가 호출).
 * 피호출자: blk_mq_sched_try_merge(), blk_mq_free_request().
 *
 * 호출 체인:
 *   blk_mq_submit_bio → __blk_mq_sched_bio_merge →
 *     ops.bio_merge=[dd_bio_merge] → blk_mq_sched_try_merge
 *       → (내부적으로) dd_request_merge/dd_request_merged/dd_merged_requests
 */
static bool dd_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs)
{
	struct deadline_data *dd = q->elevator->elevator_data; /* 이 스케줄러 인스턴스 */
	struct request *free = NULL; /* 병합 과정에서 불필요해져 해제해야 할 request(있다면) */
	bool ret; /* 병합 성공 여부 */

	spin_lock(&dd->lock); /* rb-tree/fifo_list/해시를 동시에 건드리므로 배타적 접근 시작 */
	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free); /* 공용 병합 헬퍼 호출 - back-merge(해시)와 front-merge(dd_request_merge)를 모두 시도하고, 실제 병합이 이뤄지면 dd_request_merged/dd_merged_requests도 내부에서 호출됨 */
	spin_unlock(&dd->lock); /* 배타적 접근 종료 */

	if (free) /* 병합 과정에서 이제 필요 없어진 request가 있다면(예: 두 request가 하나로 합쳐지며 하나가 남는 경우) */
		blk_mq_free_request(free); /* 그 request를 blk-mq에 반환해 tag/메모리를 회수 - dd->lock 해제 후 수행(free 자체는 이미 스케줄러 자료구조에서 빠진 상태) */

	return ret; /* 병합 성공 여부를 호출자에게 알림 */
}

/*
 * add rq to rbtree and fifo
 */
/*
 * [한국어]
 * dd_insert_request - 개별 request 하나를 우선순위별 rb-tree/fifo_list(또는
 * 예비 dispatch 리스트)에 편입.
 *
 * @hctx:  이 request가 제출된 하드웨어 큐 - hctx->queue로 request_queue를
 *         얻는 용도로만 쓰이고, mq-deadline은 request_queue 전체에서 상태를
 *         공유하므로 실제 삽입 위치 결정에는 영향을 주지 않는다.
 * @rq:    삽입할 request.
 * @flags: BLK_MQ_INSERT_AT_HEAD가 설정되어 있으면 우선순위 로직을 건너뛰고
 *         예비 dispatch 리스트(dd->dispatch) 머리에 즉시 삽입(예: requeue).
 * @free:  이 삽입 과정에서 insert-merge에 성공해 필요 없어진 request를
 *         담을 리스트 - 호출자(dd_insert_requests)가 락 해제 후 일괄 반환.
 * @return: 없음.
 *
 * 우선순위(dd_prio)를 계산해 해당 per_prio를 고르고, rq->elv.priv[0]이
 * 아직 설정되지 않았을 때만(=이 함수가 이 request를 처음 다루는 것일 때만)
 * inserted 통계를 늘린다(같은 request가 나중에 재삽입되는 경로에서 중복
 * 카운트를 막기 위함). 이어서 blk_mq_sched_try_insert_merge()로 인접
 * request와의 병합을 한 번 더 시도하고, 성공하면 즉시 반환한다(이 rq는
 * 다른 request에 흡수되었으므로 rb-tree/fifo_list 삽입이 불필요). 병합에
 * 실패하면 (a) BLK_MQ_INSERT_AT_HEAD면 예비 dispatch 리스트에 즉시
 * 삽입하고 fifo_time을 "지금"으로 설정해 다음 라운드에 최우선 디스패치되게
 * 하거나, (b) 아니면 rb-tree에 정렬 삽입하고, 병합 가능한 요청이면 끝섹터
 * 해시에도 등록한 뒤, 방향별 deadline을 계산해 fifo_list 꼬리에 추가한다.
 * 실행 컨텍스트: dd->lock을 쥔 dd_insert_requests() 안에서 호출
 *   (lockdep_assert_held로 강제).
 * 호출자: dd_insert_requests()(리스트의 각 request마다 반복 호출).
 * 피호출자: blk_mq_sched_try_insert_merge(), trace_block_rq_insert(),
 *   deadline_add_rq_rb(), elv_rqhash_add(), rq_mergeable().
 *
 * 호출 체인:
 *   dd_insert_requests → [dd_insert_request] →
 *     blk_mq_sched_try_insert_merge / deadline_add_rq_rb / elv_rqhash_add
 */
static void dd_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
			      blk_insert_t flags, struct list_head *free)
{
	struct request_queue *q = hctx->queue; /* hctx가 속한 request_queue - mq-deadline은 이 큐 전체에서 상태를 공유 */
	struct deadline_data *dd = q->elevator->elevator_data; /* 스케줄러 인스턴스 */
	const enum dd_data_dir data_dir = rq_data_dir(rq); /* 이 request의 방향(read/write) */
	u16 ioprio = req_get_ioprio(rq); /* request에 부여된 전체 ioprio 값 */
	u8 ioprio_class = IOPRIO_PRIO_CLASS(ioprio); /* ioprio에서 클래스만 추출 */
	struct dd_per_prio *per_prio; /* 이 request가 속하게 될 우선순위 슬롯(아래에서 결정) */
	enum dd_prio prio; /* dd_prio 값(아래에서 결정) */

	lockdep_assert_held(&dd->lock); /* 호출자(dd_insert_requests)가 이미 dd->lock을 쥐고 있어야 함을 검증 */

	prio = ioprio_class_to_prio[ioprio_class]; /* ioprio 클래스를 dd_prio로 매핑 */
	per_prio = &dd->per_prio[prio]; /* 매핑된 우선순위의 rb-tree/fifo_list 묶음 획득 */
	if (!rq->elv.priv[0]) /* elv.priv[0]이 아직 NULL이면(=dd_prepare_request 이후 처음 삽입되는 request) */
		per_prio->stats.inserted++; /* 삽입 통계 증가 - 이미 한 번 삽입됐던 request(예: 재삽입)는 중복 카운트하지 않음 */
	rq->elv.priv[0] = per_prio; /* 이 request가 속한 per_prio를 기억해 두어, dd_finish_request()가 완료 시 어느 통계를 갱신할지 알 수 있게 함 */

	if (blk_mq_sched_try_insert_merge(q, rq, free)) /* 인접한 기존 request와 병합을 시도 - 성공하면 rq는 그 request에 흡수됨 */
		return; /* 병합되었으므로 rb-tree/fifo_list에 별도로 넣을 필요가 없음 - rq는 @free 리스트에 추가되어 호출자가 나중에 blk_mq_free_request()로 반환 */

	trace_block_rq_insert(rq); /* ftrace/perf에 "이 request가 스케줄러에 삽입됨" 이벤트 기록 */

	if (flags & BLK_MQ_INSERT_AT_HEAD) { /* requeue 등으로 "지금 당장 재시도해야 하는" 요청으로 표시된 경우 */
		list_add(&rq->queuelist, &dd->dispatch); /* 우선순위/정렬 로직을 완전히 건너뛰고 예비 dispatch 리스트 머리에 삽입 */
		rq->fifo_time = jiffies; /* deadline을 "지금"으로 설정 - 혹시라도 FIFO 경로로 다뤄지더라도 즉시 만료된 것으로 취급되어 최우선 처리됨 */
	} else { /* 일반적인 신규 삽입 경로 */
		deadline_add_rq_rb(per_prio, rq); /* 시작 섹터 기준으로 rb-tree에 정렬 삽입 */

		if (rq_mergeable(rq)) { /* 이 request가 향후 병합 대상이 될 수 있는 종류(discard가 아니고 flush 등이 아닌 일반 R/W)라면 */
			elv_rqhash_add(q, rq); /* 끝 섹터를 키로 하는 back-merge 해시에 등록 - 이후 도착하는 bio가 O(1)로 이 request를 찾을 수 있게 함 */
			if (!q->last_merge) /* 최근 병합 힌트가 비어있다면(캐시 미스 상태) */
				q->last_merge = rq; /* 이 request를 다음 병합 시도의 첫 후보 힌트로 설정 */
		}

		/*
		 * set expire time and add to fifo list
		 */
		rq->fifo_time = jiffies + dd->fifo_expire[data_dir]; /* 현재 시각에 방향별 deadline 폭을 더해 절대 만료 시각 계산 */
		list_add_tail(&rq->queuelist, &per_prio->fifo_list[data_dir]); /* 도착 순서를 유지하도록 fifo_list 꼬리에 추가 */
	}
}

/*
 * Called from blk_mq_insert_request() or blk_mq_dispatch_list().
 */
/*
 * [한국어]
 * dd_insert_requests - elevator_mq_ops.insert_requests 콜백. request 리스트
 * 전체를 한 번의 락 구간 안에서 일괄 삽입.
 *
 * @hctx:  제출된 하드웨어 큐(각 request의 dd_insert_request() 호출에 그대로
 *         전달).
 * @list:  삽입할 request들의 연결 리스트(이 함수가 소비 - 호출 후 비어있게 됨).
 * @flags: 모든 request에 동일하게 적용할 삽입 플래그(BLK_MQ_INSERT_AT_HEAD 등).
 * @return: 없음.
 *
 * blk-mq는 여러 request를 모아 한 번에(batch로) 스케줄러에 넘기는 경우가
 * 많다(plug list flush 등). 이 함수는 dd->lock을 한 번만 잡고 리스트의
 * 모든 request에 대해 dd_insert_request()를 반복 호출함으로써, request마다
 * 락을 잡고 푸는 오버헤드와 다중 하드웨어 큐 간의 락 경합을 줄인다. 병합
 * 등으로 필요 없어진 request들은 로컬 LIST_HEAD(free)에 모았다가, 락을
 * 해제한 뒤 blk_mq_free_requests()로 한 번에 반환한다(락을 쥔 채로 해제
 * 작업까지 하면 임계 구간이 불필요하게 길어지므로).
 * 실행 컨텍스트: blk_mq_insert_request() 또는 blk_mq_dispatch_list()가
 *   호출하는 프로세스/소프트IRQ 컨텍스트.
 * 호출자: block/blk-mq.c의 blk_mq_insert_request()/blk_mq_dispatch_list()
 *   (elevator_mq_ops.insert_requests 경유).
 * 피호출자: dd_insert_request(), blk_mq_free_requests().
 *
 * 호출 체인:
 *   blk_mq_insert_request/blk_mq_dispatch_list →
 *     ops.insert_requests=[dd_insert_requests] → dd_insert_request (반복)
 *       → blk_mq_free_requests
 */
static void dd_insert_requests(struct blk_mq_hw_ctx *hctx,
			       struct list_head *list,
			       blk_insert_t flags)
{
	struct request_queue *q = hctx->queue; /* 대상 request_queue */
	struct deadline_data *dd = q->elevator->elevator_data; /* 스케줄러 인스턴스 */
	LIST_HEAD(free); /* 병합으로 불필요해진 request들을 모아 둘 로컬 리스트(스택에 선언, 빈 상태로 초기화) */

	spin_lock(&dd->lock); /* 리스트 전체 삽입 동안 단 한 번만 잠금 - 매 request마다 잠그는 오버헤드 회피 */
	while (!list_empty(list)) { /* 입력 리스트가 빌 때까지(모든 request를 처리할 때까지) 반복 */
		struct request *rq; /* 이번 반복에서 처리할 request */

		rq = list_first_entry(list, struct request, queuelist); /* 리스트 선두 request 획득 */
		list_del_init(&rq->queuelist); /* 입력 리스트에서 제거(소비) */
		dd_insert_request(hctx, rq, flags, &free); /* 개별 삽입 로직 수행 - 병합되면 free에 추가될 수 있음 */
	}
	spin_unlock(&dd->lock); /* 모든 request 처리 후 잠금 해제 */

	blk_mq_free_requests(&free); /* 병합으로 필요 없어진 request들을 락 해제 후 한꺼번에 blk-mq에 반환 - tag/메모리 회수 */
}

/* Callback from inside blk_mq_rq_ctx_init(). */
/*
 * [한국어]
 * dd_prepare_request - elevator_mq_ops.prepare_request 콜백. request가
 * 스케줄러에 편입되기 전 elv.priv[0]을 초기 상태로 리셋.
 *
 * @rq: 이제 막 할당되어 아직 스케줄러 자료구조에 들어가지 않은 request.
 * @return: 없음.
 *
 * blk_mq_rq_ctx_init()이 request를 재사용/재할당할 때마다 호출되어,
 * 이전 사용에서 남아있을 수 있는 elv.priv[0](이전 per_prio 포인터)을
 * NULL로 되돌린다. 이렇게 해야 dd_insert_request()의 "!rq->elv.priv[0]"
 * 검사와 dd_finish_request()의 "per_prio가 NULL이면 bypass"라는 판단이
 * 이번 사용 주기에 대해 올바르게 동작한다.
 * 실행 컨텍스트: request 할당 경로(프로세스 컨텍스트), 락 불필요(다른
 *   스레드가 아직 이 rq를 보지 못하는 시점).
 * 호출자: block/blk-mq.c의 blk_mq_rq_ctx_init()(elevator_mq_ops.prepare_request
 *   경유).
 * 피호출자: 없음(단순 필드 대입).
 *
 * 호출 체인:
 *   blk_mq_rq_ctx_init → ops.prepare_request=[dd_prepare_request]
 */
static void dd_prepare_request(struct request *rq)
{
	rq->elv.priv[0] = NULL; /* per_prio 포인터를 초기화 - 아직 어느 우선순위에도 속하지 않은 상태임을 표시 */
}

/*
 * Callback from inside blk_mq_free_request().
 */
/*
 * [한국어]
 * dd_finish_request - elevator_mq_ops.finish_request 콜백. request가
 * 완료되어 해제되기 직전 완료 통계 갱신.
 *
 * @rq: 완료되어 곧 해제될 request.
 * @return: 없음.
 *
 * rq->elv.priv[0]에 저장해 둔 per_prio 포인터를 이용해, 이 request가
 * 어느 우선순위에 속했는지 O(1)에 알아내 completed 카운터를 증가시킨다.
 * per_prio가 NULL이면(dd_prepare_request가 초기화한 값이 dd_insert_request를
 * 거치지 않고 그대로 남아있으면) 이 request는 스케줄러를 완전히 우회해
 * 제출된 것(blk_mq_request_bypass_insert() 경로)이므로 통계에서 제외한다 -
 * 그렇지 않으면 inserted는 늘지 않았는데 completed만 늘어 dd_queued()가
 * 음수(언더플로우) 방향으로 어긋나게 된다.
 * 실행 컨텍스트: 드라이버 완료 처리(인터럽트 또는 소프트IRQ) 컨텍스트에서
 *   호출될 수 있음 - 그래서 dd->lock이 아니라 atomic_t(completed)로
 *   동기화한다(dd->lock을 여기서 잡으면 인터럽트 컨텍스트와 프로세스
 *   컨텍스트 간 락 순서 문제가 생길 수 있음).
 * 호출자: block/blk-mq.c의 blk_mq_free_request()(elevator_mq_ops.finish_request
 *   경유, NULL 체크 없이 무조건 호출).
 * 피호출자: atomic_inc().
 *
 * 호출 체인:
 *   blk_mq_free_request → ops.finish_request=[dd_finish_request] → atomic_inc
 */
static void dd_finish_request(struct request *rq)
{
	struct dd_per_prio *per_prio = rq->elv.priv[0]; /* dd_insert_request()가 저장해 둔 per_prio 포인터(없으면 NULL) 회수 */

	/*
	 * The block layer core may call dd_finish_request() without having
	 * called dd_insert_requests(). Skip requests that bypassed I/O
	 * scheduling. See also blk_mq_request_bypass_insert().
	 */
	if (per_prio) /* 정상적으로 스케줄러를 거쳐 삽입되었던 request라면(per_prio가 유효) */
		atomic_inc(&per_prio->stats.completed); /* 원자적으로 완료 카운터 증가 - 인터럽트 컨텍스트에서도 안전 */
}

/*
 * [한국어]
 * dd_has_work_for_prio - 지정된 우선순위에 아직 디스패치되지 않은 요청이
 * 있는지 저비용으로 확인.
 *
 * @per_prio: 확인할 우선순위의 rb-tree/fifo_list 묶음.
 * @return: read 또는 write fifo_list 중 하나라도 비어있지 않으면 true.
 *
 * list_empty_careful()은 일반 list_empty()보다 약간 더 보수적인 검사로,
 * 다른 CPU가 list_del_init()으로 리스트를 막 비우는 중인 상황에서도 오탐을
 * 줄인다(락 없이 폴링하는 용도로 설계됨). dd_has_work()가 락 없이 빠르게
 * "작업이 있을 가능성"만 확인할 때 사용하므로, 이 함수도 락을 잡지 않는다.
 * 실행 컨텍스트: dd_has_work() 안에서, 락 없이 호출.
 * 호출자: dd_has_work().
 * 피호출자: list_empty_careful().
 *
 * 호출 체인:
 *   dd_has_work → [dd_has_work_for_prio] → list_empty_careful
 */
static bool dd_has_work_for_prio(struct dd_per_prio *per_prio)
{
	return !list_empty_careful(&per_prio->fifo_list[DD_READ]) || /* read fifo_list가 비어있지 않으면(작업 있음) 이미 true */
		!list_empty_careful(&per_prio->fifo_list[DD_WRITE]); /* read가 비었으면 write fifo_list도 확인 */
}

/*
 * [한국어]
 * dd_has_work - elevator_mq_ops.has_work 콜백. 이 request_queue에 디스패치할
 * 작업이 하나라도 있는지 확인.
 *
 * @hctx: 조회를 요청한 하드웨어 큐 - mq-deadline은 큐 전체 상태를 공유하므로
 *        실제로는 hctx->queue 전체를 대표해 확인한다.
 * @return: 예비 dispatch 리스트 또는 RT/BE/IDLE 어느 우선순위든 작업이
 *          있으면 true, 아니면 false.
 *
 * blk-mq는 하드웨어 큐를 idle 상태로 둘지, 아니면 dispatch_request를 호출해
 * 계속 뽑아낼지를 결정하기 위해 이 콜백을 폴링한다. 예비 dispatch 리스트를
 * 가장 먼저 확인(가장 우선순위가 높은 작업 종류)한 뒤, 세 우선순위를
 * 순회하며 dd_has_work_for_prio()로 확인한다. 락을 잡지 않는 저비용
 * "힌트" 함수이므로 결과가 나온 직후 실제로 상태가 바뀔 수 있지만, blk-mq는
 * 이런 경우를 대비해 재시도/restart 메커니즘을 갖추고 있다.
 * 실행 컨텍스트: 락 없이 호출 - list_empty_careful()의 보수적 검사에 의존.
 * 호출자: block/blk-mq-sched.c 등에서 hctx가 idle로 전환하기 전에 마지막으로
 *   확인(elevator_mq_ops.has_work 경유).
 * 피호출자: list_empty_careful(), dd_has_work_for_prio().
 *
 * 호출 체인:
 *   blk_mq_run_hw_queue 계열 → ops.has_work=[dd_has_work] →
 *     dd_has_work_for_prio → list_empty_careful
 */
static bool dd_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data; /* hctx가 속한 request_queue의 스케줄러 인스턴스 */
	enum dd_prio prio; /* 우선순위 순회 인덱스 */

	if (!list_empty_careful(&dd->dispatch)) /* 예비 dispatch 리스트에 뭔가 있으면 */
		return true; /* 더 볼 것 없이 즉시 "작업 있음" 반환 */

	for (prio = 0; prio <= DD_PRIO_MAX; prio++) /* RT, BE, IDLE 순으로 */
		if (dd_has_work_for_prio(&dd->per_prio[prio])) /* 이 우선순위에 대기 중인 요청이 있으면 */
			return true; /* "작업 있음" 반환 */

	return false; /* 예비 리스트도, 어떤 우선순위에도 대기 중인 요청이 없으면 idle */
}
/*
 * sysfs parts below
 */

/*
 * [한국어] 아래 SHOW_INT/SHOW_JIFFIES 매크로는 sysfs show 콜백(파일을 읽을 때
 * 호출되는 함수)을 찍어내는 코드 생성 매크로다. mq-deadline은 front_merges/
 * fifo_batch처럼 정수 그대로 보여줄 값과, fifo_expire[]/prio_aging_expire처럼
 * jiffies 단위를 밀리초로 환산해 보여줘야 할 값을 함께 갖고 있어, 매번 손으로
 * "struct elevator_queue -> deadline_data 변환 + sysfs_emit() 호출"을 반복하는
 * 대신 매크로로 함수 본문을 공통화한다.
 *
 * 매크로 본문(아래 SHOW_INT)은 매크로 전개 후 실제 컴파일되는 코드이므로,
 * 백슬래시(\)로 이어지는 각 줄 안에는 별도의 한국어 인라인 주석을 달지 않는다.
 * 각 줄 끝의 백슬래시는 반드시 그 줄의 "마지막 문자"여야 매크로 연속줄로
 * 인식되므로, 뒤에 주석을 붙이면(설령 공백 하나라도 위치가 바뀌면) 백슬래시가
 * 문자열 중간에 파묻혀 매크로가 조용히 잘리는 사고로 이어질 수 있다 - 이
 * 파일을 작업하며 실제로 발견된 위험 패턴이라 이 부분은 원문을 단 한 글자도
 * 건드리지 않고, 매크로 정의 앞(이 블록)과 뒤(백슬래시가 없는 마지막 줄)에만
 * 설명을 붙인다.
 *
 * SHOW_INT(__FUNC, __VAR) 전개 결과:
 *   static ssize_t __FUNC(struct elevator_queue *e, char *page)
 *   {
 *       struct deadline_data *dd = e->elevator_data;
 *       return sysfs_emit(page, "%d\n", __VAR);
 *   }
 * 즉 "elevator_queue에서 deadline_data를 꺼내 __VAR 식을 %d로 출력하는
 * ssize_t __FUNC(...)" 함수 하나를 만들어낸다. __VAR 자리에는 보통
 * "dd->필드" 형태의 식이 들어가므로, 매크로 본문 안에 "dd"라는 지역 변수
 * 이름이 정확히 그 스펠링으로 선언되어 있어야 한다(그래서 위 전개에 dd
 * 선언이 필수 - 매크로 인자 __VAR이 그 변수를 참조하는 매크로 하이지닉 트릭).
 * @return(전개된 함수 기준): sysfs_emit()의 반환값 - 실제로 쓰여진 바이트 수.
 * 호출자(전개된 함수 기준): sysfs 파일 read(2) 처리 경로(block/blk-sysfs.c
 *   또는 elevator sysfs 헬퍼)가 struct elv_fs_entry.show를 통해 호출.
 */
#define SHOW_INT(__FUNC, __VAR)						\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct deadline_data *dd = e->elevator_data;			\
									\
	return sysfs_emit(page, "%d\n", __VAR);				\
} /* [한국어] SHOW_INT 매크로 정의 끝(마지막 줄이라 백슬래시 없음) - 이 함수 템플릿이 아래 SHOW_INT(...)/SHOW_JIFFIES(...) 호출 지점마다 그대로 복사되어 개별 show 함수로 전개된다 */
#define SHOW_JIFFIES(__FUNC, __VAR) SHOW_INT(__FUNC, jiffies_to_msecs(__VAR)) /* [한국어] SHOW_JIFFIES(__FUNC, __VAR): __VAR을 jiffies_to_msecs()로 밀리초 변환한 뒤 SHOW_INT에 위임 - jiffies 단위 필드(fifo_expire[], prio_aging_expire)를 사용자에게는 ms 단위로 보여주기 위한 래퍼 */
SHOW_JIFFIES(deadline_read_expire_show, dd->fifo_expire[DD_READ]); /* [한국어] deadline_read_expire_show(e, page) 생성 - /sys/block/<disk>/queue/iosched/read_expire read 시 dd->fifo_expire[DD_READ](jiffies)를 ms로 환산해 출력 */
SHOW_JIFFIES(deadline_write_expire_show, dd->fifo_expire[DD_WRITE]); /* [한국어] deadline_write_expire_show(e, page) 생성 - .../write_expire read 시 dd->fifo_expire[DD_WRITE](jiffies)를 ms로 환산해 출력 */
SHOW_JIFFIES(deadline_prio_aging_expire_show, dd->prio_aging_expire); /* [한국어] deadline_prio_aging_expire_show(e, page) 생성 - .../prio_aging_expire read 시 dd->prio_aging_expire(jiffies)를 ms로 환산해 출력 */
SHOW_INT(deadline_writes_starved_show, dd->writes_starved); /* [한국어] deadline_writes_starved_show(e, page) 생성 - .../writes_starved read 시 dd->writes_starved를 변환 없이 %d로 출력 */
SHOW_INT(deadline_front_merges_show, dd->front_merges); /* [한국어] deadline_front_merges_show(e, page) 생성 - .../front_merges read 시 dd->front_merges(0 또는 1)를 %d로 출력 */
SHOW_INT(deadline_fifo_batch_show, dd->fifo_batch); /* [한국어] deadline_fifo_batch_show(e, page) 생성 - .../fifo_batch read 시 dd->fifo_batch를 %d로 출력 */
#undef SHOW_INT /* [한국어] SHOW_INT는 여기까지만 필요 - 매크로 네임스페이스를 정리해 이후 STORE_* 매크로 정의와 이름이 섞이지 않도록 undef */
#undef SHOW_JIFFIES /* [한국어] SHOW_JIFFIES도 동일하게 정리 - SHOW_INT에 의존하는 매크로이므로 SHOW_INT보다 먼저 쓸 일이 없어 순서상 문제 없음 */


/*
 * [한국어] STORE_FUNCTION은 sysfs store 콜백(파일에 쓸 때 호출되는 함수)을
 * 찍어내는 매크로다. 사용자가 문자열로 값을 쓰면(예: "echo 100 >
 * .../fifo_batch") (1) kstrtoint()로 정수 파싱을 시도하고, (2) 실패하면
 * 파싱 에러를 그대로 반환하며, (3) 성공하면 [MIN, MAX] 범위로 값을 강제로
 * 잘라내고(clamp), (4) __CONV로 필요하면 단위 변환(예: ms -> jiffies) 후
 * *__PTR에 대입한다. 위 SHOW_INT와 마찬가지로 매크로 본문의 백슬래시 연속줄
 * 내부에는 인라인 주석을 달지 않는다(동일한 이유 - 매크로 손상 방지).
 *
 * STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV) 전개 결과:
 *   static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)
 *   {
 *       struct deadline_data *dd = e->elevator_data;
 *       int __data, __ret;
 *       __ret = kstrtoint(page, 0, &__data);
 *       if (__ret < 0) return __ret;
 *       if (__data < (MIN)) __data = (MIN);
 *       else if (__data > (MAX)) __data = (MAX);
 *       *(__PTR) = __CONV(__data);
 *       return count;
 *   }
 * @return(전개된 함수 기준): 성공 시 사용자가 write(2)한 바이트 수(count),
 *   kstrtoint() 파싱 실패 시 음수 errno.
 * 호출자(전개된 함수 기준): sysfs 파일 write(2) 처리 경로가 struct
 *   elv_fs_entry.store를 통해 호출.
 */
#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data, __ret;						\
									\
	__ret = kstrtoint(page, 0, &__data);				\
	if (__ret < 0)							\
		return __ret;						\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	*(__PTR) = __CONV(__data);					\
	return count;							\
} /* [한국어] STORE_FUNCTION 매크로 정의 끝(백슬래시 없는 마지막 줄) - 이 템플릿이 STORE_INT/STORE_JIFFIES를 거쳐 실제 store 함수로 전개된다 */
/*
 * [한국어] STORE_INT/STORE_JIFFIES는 STORE_FUNCTION에 __CONV 인자만 다르게
 * 채워 넣는 얇은 래퍼다. STORE_INT는 __CONV 자리를 비워(토큰 없음) 변환 없이
 * 그대로 대입하고, STORE_JIFFIES는 __CONV에 msecs_to_jiffies를 넣어 사용자가
 * 입력한 밀리초 값을 내부 jiffies 단위로 변환한 뒤 대입한다.
 */
#define STORE_INT(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, ) /* [한국어] STORE_INT(__FUNC, __PTR, MIN, MAX) 전개: STORE_FUNCTION(..., __CONV 없음) - 값 검증/clamp 후 변환 없이 *(__PTR)에 그대로 대입 */
#define STORE_JIFFIES(__FUNC, __PTR, MIN, MAX)				\
	STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, msecs_to_jiffies) /* [한국어] STORE_JIFFIES(__FUNC, __PTR, MIN, MAX) 전개: STORE_FUNCTION(..., msecs_to_jiffies) - 사용자가 쓴 ms 값을 msecs_to_jiffies()로 jiffies로 변환 후 대입 */
STORE_JIFFIES(deadline_read_expire_store, &dd->fifo_expire[DD_READ], 0, INT_MAX); /* [한국어] deadline_read_expire_store(e, page, count) 생성 - 사용자가 쓴 ms 값을 [0, INT_MAX] 범위로 clamp 후 jiffies로 변환해 dd->fifo_expire[DD_READ]에 대입 */
STORE_JIFFIES(deadline_write_expire_store, &dd->fifo_expire[DD_WRITE], 0, INT_MAX); /* [한국어] deadline_write_expire_store(e, page, count) 생성 - 동일하게 dd->fifo_expire[DD_WRITE]에 대입 */
STORE_JIFFIES(deadline_prio_aging_expire_store, &dd->prio_aging_expire, 0, INT_MAX); /* [한국어] deadline_prio_aging_expire_store(e, page, count) 생성 - dd->prio_aging_expire에 대입(ms -> jiffies 변환) */
STORE_INT(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX); /* [한국어] deadline_writes_starved_store(e, page, count) 생성 - [INT_MIN, INT_MAX] 범위로 clamp(사실상 무제한)하여 dd->writes_starved에 변환 없이 대입 */
STORE_INT(deadline_front_merges_store, &dd->front_merges, 0, 1); /* [한국어] deadline_front_merges_store(e, page, count) 생성 - [0, 1] 범위로 clamp해 dd->front_merges에 대입 - front merge를 불리언처럼 강제 */
STORE_INT(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX); /* [한국어] deadline_fifo_batch_store(e, page, count) 생성 - [0, INT_MAX] 범위로 clamp해 dd->fifo_batch에 대입 */
#undef STORE_FUNCTION /* [한국어] STORE_FUNCTION 매크로 정리 - 더 이상 필요 없는 매크로 이름을 남겨 다른 곳과 충돌하지 않도록 undef */
#undef STORE_INT /* [한국어] STORE_INT 정리 */
#undef STORE_JIFFIES /* [한국어] STORE_JIFFIES 정리 */


/*
 * [한국어] DD_ATTR(name)은 위에서 만든 deadline_<name>_show/deadline_<name>_store
 * 함수 쌍을 __ATTR() 매크로(include/linux/sysfs.h)로 감싸 struct elv_fs_entry
 * 하나를 만들어내는 매크로다. ## 토큰 결합(token pasting)으로
 * "deadline_" + name + "_show"/"_store" 형태의 함수 이름을 컴파일 타임에
 * 조립하므로, 위에서 SHOW_INT/SHOW_JIFFIES 및 STORE_INT/STORE_JIFFIES
 * 매크로로 만든 함수 이름과 정확히 일치해야 한다(오타가 나면 링크 에러로
 * 즉시 드러남).
 */
#define DD_ATTR(name) \
	__ATTR(name, 0644, deadline_##name##_show, deadline_##name##_store) /* [한국어] __ATTR(name, 0644, show_fn, store_fn) 전개 - sysfs attribute 이름 문자열 "name", 권한 0644(소유자 rw, 그룹/기타 r), show/store 함수 포인터를 담은 struct elv_fs_entry 리터럴을 만든다 */


/*
 * [한국어] 이 배열이 곧 mq_deadline.elevator_attrs(이 파일 하단 struct
 * elevator_type 정의 참고)이다. 스케줄러가 이 request_queue에 연결될 때
 * block/elevator.c가 이 배열을 순회하며 /sys/block/<disk>/queue/iosched/
 * 아래에 각 항목의 이름으로 sysfs 파일을 생성한다. 배열은 name이 NULL인
 * __ATTR_NULL sentinel로 끝나며, 순회 코드는 그 sentinel을 만날 때까지
 * 반복한다.
 */
static const struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire), /* [한국어] /sys/.../iosched/read_expire 파일 생성 - read 요청 deadline(ms) */
	DD_ATTR(write_expire), /* [한국어] /sys/.../iosched/write_expire 파일 생성 - write 요청 deadline(ms) */
	DD_ATTR(writes_starved), /* [한국어] /sys/.../iosched/writes_starved 파일 생성 - read의 write starvation 한도 */
	DD_ATTR(front_merges), /* [한국어] /sys/.../iosched/front_merges 파일 생성 - front merge 허용 여부(0/1) */
	DD_ATTR(fifo_batch), /* [한국어] /sys/.../iosched/fifo_batch 파일 생성 - batching 상한 */
	DD_ATTR(prio_aging_expire), /* [한국어] /sys/.../iosched/prio_aging_expire 파일 생성 - BE/IDLE aging 한도(ms) */
	__ATTR_NULL /* [한국어] __ATTR_NULL: name이 NULL인 sentinel - 배열 순회 종료 조건으로 사용됨 */
}; /* [한국어] deadline_attrs[] 정의 끝 */


/*
 * [한국어] #ifdef CONFIG_BLK_DEBUG_FS 분기 이유: 이 블록이 정의하는 모든
 * 것(개별 fifo_list 열람 seq_ops, next_rq/batching/starved/owned_by_driver/
 * queued show 콜백, deadline_queue_debugfs_attrs[] 배열)은 순수히
 * /sys/kernel/debug/block/<disk>/sched/ 아래에서 스케줄러 내부 상태를
 * 관찰하기 위한 디버깅 기능이며, I/O 처리 성능이나 정확성에는 전혀 관여하지
 * 않는다. 따라서 CONFIG_BLK_DEBUG_FS가 꺼진 프로덕션 빌드에서는 이 코드
 * 전체를 컴파일에서 제외해 바이너리 크기와 약간의 유지보수 부담을 줄인다.
 * 이 조건은 이 파일 하단의 "#endif"(deadline_queue_debugfs_attrs[] 정의
 * 직후)에서 닫히고, struct elevator_type 안의 .queue_debugfs_attrs 필드
 * 대입도 동일한 #ifdef로 감싸여 있다.
 */
#ifdef CONFIG_BLK_DEBUG_FS
/*
 * [한국어] DEADLINE_DEBUGFS_DDIR_ATTRS(prio, data_dir, name)는 RT/BE/IDLE x
 * READ/WRITE = 6가지 조합 각각에 대해 반복되는 보일러플레이트를 한 번만
 * 작성하기 위한 대형 코드 생성 매크로다. name이 예를 들어 "read0"이면, 한 번의
 * 매크로 호출(DEADLINE_DEBUGFS_DDIR_ATTRS(DD_RT_PRIO, DD_READ, read0))이
 * 아래 4가지를 한꺼번에 만들어낸다.
 *
 *   1) deadline_read0_fifo_start(m, pos)   - seq_file iterator 시작 콜백.
 *      dd->lock을 획득(__acquires 애트리뷰트로 sparse에 "이 함수는 락을 들고
 *      리턴한다"고 알림)하고 per_prio->fifo_list[DD_READ]의 첫 노드를 반환.
 *   2) deadline_read0_fifo_next(m, v, pos) - iterator 다음 노드 콜백.
 *   3) deadline_read0_fifo_stop(m, v)      - iterator 종료 콜백. dd->lock을
 *      해제(__releases)하여 start에서 잡은 락과 짝을 맞춘다.
 *   4) deadline_read0_fifo_seq_ops         - 위 세 콜백과 공용
 *      blk_mq_debugfs_rq_show(block/blk-mq-debugfs.h)를 묶은
 *      struct seq_operations - debugfs "read0_fifo_list" 파일의 실제 구현.
 *   5) deadline_read0_next_rq_show(data, m) - "다음에 순차적으로 디스패치될
 *      request"(deadline_from_pos 기준) 단 하나만 보여주는 단발성 show 콜백 -
 *      debugfs "read0_next_rq" 파일의 구현.
 *
 * 매크로 인자 prio/data_dir는 함수 본문 안에서 dd->per_prio[prio],
 * fifo_list[data_dir] 형태로 그대로 상수처럼 박혀 들어간다(매크로 전개 시
 * 결정되므로 런타임 분기 없이 인라인됨). name은 ## 토큰 결합으로 함수/구조체
 * 이름의 일부가 된다.
 * 이 거대한 매크로의 백슬래시 연속줄 내부에는 (SHOW_INT/STORE_FUNCTION과
 * 동일한 이유로) 인라인 주석을 추가하지 않는다 - 원문을 한 글자도 바꾸지
 * 않고 이 설명 블록과 마지막 줄(백슬래시 없는 "}") 뒤의 주석만으로 전체
 * 동작을 서술한다.
 * @return: fifo_start/next는 seq_file iterator 위치(void *) 또는 NULL,
 *   fifo_stop은 void, next_rq_show는 항상 0.
 * 호출자: block/blk-mq-debugfs.c의 debugfs_create_files()가
 *   deadline_queue_debugfs_attrs[] 테이블(이 파일 하단)을 통해 등록.
 */
#define DEADLINE_DEBUGFS_DDIR_ATTRS(prio, data_dir, name)		\
static void *deadline_##name##_fifo_start(struct seq_file *m,		\
					  loff_t *pos)			\
	__acquires(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	spin_lock(&dd->lock);						\
	return seq_list_start(&per_prio->fifo_list[data_dir], *pos);	\
}									\
									\
static void *deadline_##name##_fifo_next(struct seq_file *m, void *v,	\
					 loff_t *pos)			\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
									\
	return seq_list_next(v, &per_prio->fifo_list[data_dir], pos);	\
}									\
									\
static void deadline_##name##_fifo_stop(struct seq_file *m, void *v)	\
	__releases(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
									\
	spin_unlock(&dd->lock);						\
}									\
									\
static const struct seq_operations deadline_##name##_fifo_seq_ops = {	\
	.start	= deadline_##name##_fifo_start,				\
	.next	= deadline_##name##_fifo_next,				\
	.stop	= deadline_##name##_fifo_stop,				\
	.show	= blk_mq_debugfs_rq_show,				\
};									\
									\
static int deadline_##name##_next_rq_show(void *data,			\
					  struct seq_file *m)		\
{									\
	struct request_queue *q = data;					\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct dd_per_prio *per_prio = &dd->per_prio[prio];		\
	struct request *rq;						\
									\
	rq = deadline_from_pos(per_prio, data_dir,			\
			       per_prio->latest_pos[data_dir]);		\
	if (rq)								\
		__blk_mq_debugfs_rq_show(m, rq);			\
	return 0;							\
} /* [한국어] DEADLINE_DEBUGFS_DDIR_ATTRS 매크로 정의 끝(백슬래시 없는 마지막 줄) - 아래 6개 호출 지점마다 이 5개 함수/구조체 세트가 통째로 전개된다 */

DEADLINE_DEBUGFS_DDIR_ATTRS(DD_RT_PRIO, DD_READ, read0); /* [한국어] RT 우선순위 x read 방향 세트 생성 - deadline_read0_fifo_start/next/stop, deadline_read0_fifo_seq_ops, deadline_read0_next_rq_show */
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_RT_PRIO, DD_WRITE, write0); /* [한국어] RT 우선순위 x write 방향 세트 생성 - deadline_write0_* */
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_BE_PRIO, DD_READ, read1); /* [한국어] BE 우선순위 x read 방향 세트 생성 - deadline_read1_* */
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_BE_PRIO, DD_WRITE, write1); /* [한국어] BE 우선순위 x write 방향 세트 생성 - deadline_write1_* */
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_IDLE_PRIO, DD_READ, read2); /* [한국어] IDLE 우선순위 x read 방향 세트 생성 - deadline_read2_* */
DEADLINE_DEBUGFS_DDIR_ATTRS(DD_IDLE_PRIO, DD_WRITE, write2); /* [한국어] IDLE 우선순위 x write 방향 세트 생성 - deadline_write2_* */
#undef DEADLINE_DEBUGFS_DDIR_ATTRS /* [한국어] 6개 조합을 모두 전개했으므로 매크로 정리 - 이후 재사용/충돌 방지 */


/*
 * [한국어]
 * deadline_batching_show - debugfs "batching" 파일의 show 콜백. 현재
 * batching 카운터를 출력.
 *
 * @data: debugfs 등록 시 넘긴 request_queue 포인터(void* 로 소거된 상태).
 * @m:    출력 대상 seq_file.
 * @return: 항상 0.
 *
 * dd->batching(__dd_dispatch_request가 관리하는 "현재 batch에서 연속
 * 디스패치한 요청 수")을 락 없이 그대로 읽어 출력한다 - 단일 값 스냅숏이라
 * 약간의 읽기 경쟁은 문제되지 않는 디버깅 전용 정보이기 때문이다.
 * 실행 컨텍스트: debugfs 파일 read(2) syscall 컨텍스트.
 * 호출자: single_open() 계열이 감싸는 debugfs read 경로(struct
 *   blk_mq_debugfs_attr.show 경유).
 * 피호출자: seq_printf().
 *
 * 호출 체인:
 *   debugfs read(2) → .show=[deadline_batching_show] → seq_printf
 */
static int deadline_batching_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data; /* [한국어] data는 debugfs 등록 시 넘겨진 request_queue 포인터 - void*로 저장되어 있어 캐스팅 없이 대입 */
	struct deadline_data *dd = q->elevator->elevator_data; /* [한국어] request_queue에서 이 스케줄러 인스턴스를 조회 */

	seq_printf(m, "%u\n", dd->batching); /* [한국어] dd->batching(현재 batch 진행 카운터)을 부호 없는 정수로 출력 - 락 없이 스냅숏 읽기(디버깅 정보이므로 근사치로 충분) */
	return 0; /* [한국어] seq_show 콜백 규약상 항상 0 반환(실패를 표현하지 않음) */
}

/*
 * [한국어]
 * deadline_starved_show - debugfs "starved" 파일의 show 콜백. 현재
 * starvation 카운터를 출력.
 *
 * @data: request_queue 포인터.
 * @m:    출력 대상 seq_file.
 * @return: 항상 0.
 *
 * dd->starved(read가 write를 연속으로 앞지른 횟수)를 락 없이 읽어 출력한다.
 * writes_starved 설정값과 비교하면 "다음 디스패치에서 write로 강제 전환될
 * 때까지 얼마나 남았는지" 가늠할 수 있어 starvation 정책 튜닝에 쓰인다.
 * 실행 컨텍스트: debugfs read(2) syscall 컨텍스트.
 * 호출자: struct blk_mq_debugfs_attr.show 경유.
 * 피호출자: seq_printf().
 *
 * 호출 체인:
 *   debugfs read(2) → .show=[deadline_starved_show] → seq_printf
 */
static int deadline_starved_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data; /* [한국어] request_queue 포인터 획득 */
	struct deadline_data *dd = q->elevator->elevator_data; /* [한국어] 스케줄러 인스턴스 조회 */

	seq_printf(m, "%u\n", dd->starved); /* [한국어] dd->starved(read의 write 선점 누적 횟수)를 출력 - 락 없는 스냅숏 */
	return 0; /* [한국어] 항상 성공(0) 반환 */
}

/*
 * [한국어]
 * dd_queued_show - debugfs "queued" 파일의 show 콜백. RT/BE/IDLE 각
 * 우선순위별로 아직 완료되지 않은 요청 수(dd_queued())를 출력.
 *
 * @data: request_queue 포인터.
 * @m:    출력 대상 seq_file.
 * @return: 항상 0.
 *
 * dd_queued()는 dd->lock을 요구하므로(lockdep_assert_held), 세 우선순위
 * 모두를 조회하는 동안 락을 잡아 일관된 스냅숏을 얻는다. 출력 형식은
 * "rt be idle" 세 정수를 공백으로 구분한 한 줄이다.
 * 실행 컨텍스트: debugfs read(2) syscall 컨텍스트.
 * 호출자: struct blk_mq_debugfs_attr.show 경유.
 * 피호출자: dd_queued(), seq_printf().
 *
 * 호출 체인:
 *   debugfs read(2) → .show=[dd_queued_show] → dd_queued → seq_printf
 */
static int dd_queued_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data; /* [한국어] request_queue 포인터 획득 */
	struct deadline_data *dd = q->elevator->elevator_data; /* [한국어] 스케줄러 인스턴스 조회 */
	u32 rt, be, idle; /* [한국어] 세 우선순위 각각의 결과를 담을 지역 변수 */

	spin_lock(&dd->lock); /* [한국어] dd_queued()가 요구하는 lockdep 조건을 만족시키기 위해 잠금 - 세 우선순위 조회 동안 일관된 스냅숏 확보 */
	rt = dd_queued(dd, DD_RT_PRIO); /* [한국어] RT 우선순위의 미완료 요청 수(inserted - completed) */
	be = dd_queued(dd, DD_BE_PRIO); /* [한국어] BE 우선순위의 미완료 요청 수 */
	idle = dd_queued(dd, DD_IDLE_PRIO); /* [한국어] IDLE 우선순위의 미완료 요청 수 */
	spin_unlock(&dd->lock); /* [한국어] 조회 완료 - 잠금 해제 */

	seq_printf(m, "%u %u %u\n", rt, be, idle); /* [한국어] "rt be idle" 형식으로 한 줄 출력 */

	return 0; /* [한국어] 항상 성공(0) 반환 */
}

/* Number of requests owned by the block driver for a given priority. */
/*
 * [한국어]
 * dd_owned_by_driver - 지정된 우선순위에서 "드라이버가 현재 소유 중인"
 * (디스패치되었지만 아직 완료되지 않은) 요청 수를 근사 계산.
 *
 * @dd:   스케줄러 인스턴스.
 * @prio: 조회할 우선순위.
 * @return: dispatched + merged - completed.
 *
 * dispatched(드라이버로 넘어간 수)에 merged(병합되어 사라졌지만 한 번은
 * 드라이버로 내려갔던 것으로 계산에 포함해야 하는 수)를 더하고 completed
 * (처리 완료 수)를 빼면, "드라이버 쪽에 남아있는" 요청 수의 근사치가 된다.
 * dd_queued()(inserted - completed, 스케줄러+드라이버 합산)와 비교하면
 * "스케줄러 내부에만 있는 수"와 "드라이버가 들고 있는 수"를 구분해 볼 수
 * 있다.
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출(lockdep_assert_held로 강제).
 * 호출자: dd_owned_by_driver_show().
 * 피호출자: atomic_read().
 *
 * 호출 체인:
 *   dd_owned_by_driver_show → [dd_owned_by_driver] → atomic_read
 */
static u32 dd_owned_by_driver(struct deadline_data *dd, enum dd_prio prio)
{
	const struct io_stats_per_prio *stats = &dd->per_prio[prio].stats; /* [한국어] 해당 우선순위의 통계 구조체 포인터 획득 */

	lockdep_assert_held(&dd->lock); /* [한국어] 호출자가 dd->lock을 쥐고 있어야 함을 검증 */

	return stats->dispatched + stats->merged - /* [한국어] dispatched(드라이버로 넘어간 누적 수) + merged(병합되어 사라졌지만 한 번은 넘어갔던 수)를 합산 */
		atomic_read(&stats->completed); /* [한국어] 완료된(completed) 수를 원자적으로 읽어 뺌 - 결과가 현재 드라이버가 들고 있는 요청 수의 근사치 */
}

/*
 * [한국어]
 * dd_owned_by_driver_show - debugfs "owned_by_driver" 파일의 show 콜백.
 * RT/BE/IDLE 각 우선순위별 dd_owned_by_driver() 결과를 출력.
 *
 * @data: request_queue 포인터.
 * @m:    출력 대상 seq_file.
 * @return: 항상 0.
 *
 * dd_queued_show()와 동일한 패턴으로 dd->lock을 잡고 세 우선순위를 조회한
 * 뒤 "rt be idle" 형식으로 출력한다.
 * 실행 컨텍스트: debugfs read(2) syscall 컨텍스트.
 * 호출자: struct blk_mq_debugfs_attr.show 경유.
 * 피호출자: dd_owned_by_driver(), seq_printf().
 *
 * 호출 체인:
 *   debugfs read(2) → .show=[dd_owned_by_driver_show] →
 *     dd_owned_by_driver → seq_printf
 */
static int dd_owned_by_driver_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data; /* [한국어] request_queue 포인터 획득 */
	struct deadline_data *dd = q->elevator->elevator_data; /* [한국어] 스케줄러 인스턴스 조회 */
	u32 rt, be, idle; /* [한국어] 세 우선순위 각각의 결과를 담을 지역 변수 */

	spin_lock(&dd->lock); /* [한국어] 세 우선순위 조회 동안 일관된 스냅숏을 위해 잠금 */
	rt = dd_owned_by_driver(dd, DD_RT_PRIO); /* [한국어] RT 우선순위의 드라이버 소유 요청 수 */
	be = dd_owned_by_driver(dd, DD_BE_PRIO); /* [한국어] BE 우선순위의 드라이버 소유 요청 수 */
	idle = dd_owned_by_driver(dd, DD_IDLE_PRIO); /* [한국어] IDLE 우선순위의 드라이버 소유 요청 수 */
	spin_unlock(&dd->lock); /* [한국어] 조회 완료 - 잠금 해제 */

	seq_printf(m, "%u %u %u\n", rt, be, idle); /* [한국어] "rt be idle" 형식으로 한 줄 출력 */

	return 0; /* [한국어] 항상 성공(0) 반환 */
}

/*
 * [한국어]
 * deadline_dispatch_start - debugfs "dispatch" 파일의 seq_operations.start
 * 콜백. dd->dispatch(예비 dispatch 리스트) 순회를 시작.
 *
 * @m:   seq_file - m->private에 request_queue 포인터가 들어있음(debugfs
 *       등록 시 설정).
 * @pos: 순회 시작 위치(오프셋).
 * @return: seq_list_start()가 반환하는 iterator 위치(리스트 노드 또는 NULL).
 *
 * __acquires(&dd->lock) 애트리뷰트는 sparse 정적 분석 도구에게 "이 함수는
 * 락을 획득한 채로 반환한다"고 알리는 주석성 표시로, 실제로 spin_lock()을
 * 호출해 dd->dispatch 리스트를 순회하는 동안 다른 CPU의 삽입/제거로부터
 * 보호한다. 이 락은 대응하는 deadline_dispatch_stop()이 해제한다(start/stop
 * 쌍은 seq_file 인프라가 항상 짝지어 호출을 보장).
 * 실행 컨텍스트: debugfs 파일 read(2) syscall 컨텍스트에서 seq_file 코어가 호출.
 * 호출자: seq_file 코어(seq_read()가 순회 시작 시 .start 호출).
 * 피호출자: seq_list_start().
 *
 * 호출 체인:
 *   seq_read() → .start=[deadline_dispatch_start] → seq_list_start
 *     (짝: .stop=deadline_dispatch_stop)
 */
static void *deadline_dispatch_start(struct seq_file *m, loff_t *pos)
	__acquires(&dd->lock) /* [한국어] sparse 정적 분석용 애트리뷰트 - 이 함수가 dd->lock을 잡은 채로 반환함을 명시(실제 락 동작은 본문의 spin_lock) */
{
	struct request_queue *q = m->private; /* [한국어] m->private에 저장된 request_queue 포인터 회수 */
	struct deadline_data *dd = q->elevator->elevator_data; /* [한국어] 스케줄러 인스턴스 조회 */

	spin_lock(&dd->lock); /* [한국어] dd->dispatch 리스트 순회 동안 다른 CPU의 삽입/제거를 막기 위해 잠금 - deadline_dispatch_stop에서 해제 */
	return seq_list_start(&dd->dispatch, *pos); /* [한국어] dd->dispatch를 seq_file 리스트 iterator로 감싸 시작 위치를 반환 */
}

/*
 * [한국어]
 * deadline_dispatch_next - "dispatch" 파일 seq_operations.next 콜백.
 * 리스트의 다음 노드로 이동.
 *
 * @m:   seq_file(사용하지 않음 - 시그니처 일관성 유지).
 * @v:   현재 iterator 위치(리스트 노드).
 * @pos: 순회 위치 카운터(seq_list_next가 갱신).
 * @return: 다음 노드 위치, 리스트 끝이면 NULL.
 *
 * deadline_dispatch_start()가 잡은 dd->lock을 쥔 상태에서 seq_file 코어가
 * 반복 호출한다(각 request마다 한 번씩).
 * 실행 컨텍스트: dd->lock을 쥔 상태(start에서 획득, stop에서 해제).
 * 호출자: seq_file 코어(seq_read()).
 * 피호출자: seq_list_next().
 *
 * 호출 체인:
 *   seq_read() → .next=[deadline_dispatch_next] → seq_list_next
 */
static void *deadline_dispatch_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct request_queue *q = m->private; /* [한국어] m->private에서 request_queue 회수(이 함수 자체는 사용하지 않지만 대응하는 패턴 유지) */
	struct deadline_data *dd = q->elevator->elevator_data; /* [한국어] 스케줄러 인스턴스 조회(미사용이지만 관례상 조회) */

	return seq_list_next(v, &dd->dispatch, pos); /* [한국어] dd->dispatch 리스트에서 현재 위치(v) 다음 노드로 이동 */
}

/*
 * [한국어]
 * deadline_dispatch_stop - "dispatch" 파일 seq_operations.stop 콜백.
 * deadline_dispatch_start()가 잡은 락을 해제.
 *
 * @m: seq_file.
 * @v: 순회 종료 시점의 iterator 위치(사용하지 않음).
 * @return: 없음.
 *
 * __releases(&dd->lock) 애트리뷰트로 sparse에 "이 함수가 락을 해제하고
 * 반환한다"고 알린다. seq_file 코어는 순회가 끝나거나(리스트 소진) 중단되어도
 * (예: 사용자가 read를 중단) 반드시 .stop을 호출하도록 보장되어 있어, start/stop
 * 쌍의 락 획득/해제가 어긋나지 않는다.
 * 실행 컨텍스트: dd->lock을 쥔 상태에서 호출되어 해제.
 * 호출자: seq_file 코어(seq_read() 순회 종료 시).
 * 피호출자: spin_unlock() 매크로.
 *
 * 호출 체인:
 *   seq_read() → .stop=[deadline_dispatch_stop] (짝: .start=deadline_dispatch_start)
 */
static void deadline_dispatch_stop(struct seq_file *m, void *v)
	__releases(&dd->lock) /* [한국어] sparse 정적 분석용 애트리뷰트 - 이 함수가 dd->lock을 해제하고 반환함을 명시 */
{
	struct request_queue *q = m->private; /* [한국어] m->private에서 request_queue 회수 */
	struct deadline_data *dd = q->elevator->elevator_data; /* [한국어] 스케줄러 인스턴스 조회 */

	spin_unlock(&dd->lock); /* [한국어] deadline_dispatch_start()가 잡았던 락을 해제 - 순회 종료 */
}

/*
 * [한국어]
 * deadline_dispatch_seq_ops - debugfs "dispatch" 파일의 seq_operations 테이블.
 *
 * dd->dispatch(예비 dispatch 리스트)를 순회하며 각 request를 공용
 * blk_mq_debugfs_rq_show()(block/blk-mq-debugfs.h)로 출력한다. 이 리스트는
 * 보통 requeue된 요청이 잠깐 머무는 곳이라 평소에는 비어있는 경우가 많다.
 */
static const struct seq_operations deadline_dispatch_seq_ops = {
	.start	= deadline_dispatch_start, /* [한국어] 순회 시작 - dd->lock 획득 후 리스트 첫 노드 반환 */
	.next	= deadline_dispatch_next, /* [한국어] 순회 다음 - 리스트 다음 노드 반환 */
	.stop	= deadline_dispatch_stop, /* [한국어] 순회 종료 - dd->lock 해제 */
	.show	= blk_mq_debugfs_rq_show, /* [한국어] 각 노드(request)를 출력 - blk-mq core와 공유하는 공용 헬퍼 재사용 */
};


/*
 * [한국어] DEADLINE_QUEUE_DDIR_ATTRS(name)은 위에서 DEADLINE_DEBUGFS_DDIR_ATTRS로
 * 만든 deadline_<name>_fifo_seq_ops를 struct blk_mq_debugfs_attr 테이블 항목
 * ({"<name>_fifo_list", 0400, .seq_ops = ...})으로 감싼다. #name(stringizing)
 * 으로 매크로 인자를 문자열 리터럴로 바꾸고, ##(token pasting)으로 실제
 * seq_ops 심볼 이름을 조립한다. 0400은 "소유자(보통 root) 읽기 전용"
 * 권한이다.
 */
#define DEADLINE_QUEUE_DDIR_ATTRS(name)					\
	{#name "_fifo_list", 0400,					\
			.seq_ops = &deadline_##name##_fifo_seq_ops} /* [한국어] DEADLINE_QUEUE_DDIR_ATTRS 매크로 정의 끝(백슬래시 없는 마지막 줄) - {"<name>_fifo_list", 0400, .seq_ops=&deadline_<name>_fifo_seq_ops} 리터럴로 전개 */
#define DEADLINE_NEXT_RQ_ATTR(name)					\
	{#name "_next_rq", 0400, deadline_##name##_next_rq_show} /* [한국어] DEADLINE_NEXT_RQ_ATTR(name) 전개 - {"<name>_next_rq", 0400, deadline_<name>_next_rq_show} 리터럴로, .show(단발성 콜백)를 사용하는 항목 */
/*
 * [한국어] deadline_queue_debugfs_attrs[]는 이 파일의 struct elevator_type
 * (mq_deadline)의 .queue_debugfs_attrs 필드에 연결되어, block/blk-mq-debugfs.c
 * 의 debugfs_create_files()가 이 배열을 순회하며
 * /sys/kernel/debug/block/<disk>/sched/ 아래에 파일들을 만든다. 6개
 * 조합(RT/BE/IDLE x READ/WRITE) x 2종류(_fifo_list, _next_rq) = 12개 항목과,
 * batching/starved/dispatch/owned_by_driver/queued 5개 단일 항목, 마지막
 * NULL sentinel({})로 구성된다.
 */
static const struct blk_mq_debugfs_attr deadline_queue_debugfs_attrs[] = {
	DEADLINE_QUEUE_DDIR_ATTRS(read0), /* [한국어] "read0_fifo_list" - RT read 방향 fifo_list 전체 열람 */
	DEADLINE_QUEUE_DDIR_ATTRS(write0), /* [한국어] "write0_fifo_list" - RT write 방향 fifo_list 전체 열람 */
	DEADLINE_QUEUE_DDIR_ATTRS(read1), /* [한국어] "read1_fifo_list" - BE read 방향 fifo_list 전체 열람 */
	DEADLINE_QUEUE_DDIR_ATTRS(write1), /* [한국어] "write1_fifo_list" - BE write 방향 fifo_list 전체 열람 */
	DEADLINE_QUEUE_DDIR_ATTRS(read2), /* [한국어] "read2_fifo_list" - IDLE read 방향 fifo_list 전체 열람 */
	DEADLINE_QUEUE_DDIR_ATTRS(write2), /* [한국어] "write2_fifo_list" - IDLE write 방향 fifo_list 전체 열람 */
	DEADLINE_NEXT_RQ_ATTR(read0), /* [한국어] "read0_next_rq" - RT read 방향의 다음 순차 디스패치 후보 1개만 열람 */
	DEADLINE_NEXT_RQ_ATTR(write0), /* [한국어] "write0_next_rq" - RT write 방향의 다음 순차 디스패치 후보 */
	DEADLINE_NEXT_RQ_ATTR(read1), /* [한국어] "read1_next_rq" - BE read 방향의 다음 순차 디스패치 후보 */
	DEADLINE_NEXT_RQ_ATTR(write1), /* [한국어] "write1_next_rq" - BE write 방향의 다음 순차 디스패치 후보 */
	DEADLINE_NEXT_RQ_ATTR(read2), /* [한국어] "read2_next_rq" - IDLE read 방향의 다음 순차 디스패치 후보 */
	DEADLINE_NEXT_RQ_ATTR(write2), /* [한국어] "write2_next_rq" - IDLE write 방향의 다음 순차 디스패치 후보 */
	{"batching", 0400, deadline_batching_show}, /* [한국어] "batching" - dd->batching 현재값 열람 */
	{"starved", 0400, deadline_starved_show}, /* [한국어] "starved" - dd->starved 현재값 열람 */
	{"dispatch", 0400, .seq_ops = &deadline_dispatch_seq_ops}, /* [한국어] "dispatch" - dd->dispatch(예비 dispatch 리스트) 전체 열람 */
	{"owned_by_driver", 0400, dd_owned_by_driver_show}, /* [한국어] "owned_by_driver" - 우선순위별 드라이버 소유 요청 수 열람 */
	{"queued", 0400, dd_queued_show}, /* [한국어] "queued" - 우선순위별 미완료 요청 수 열람 */
	{}, /* [한국어] name이 NULL인 sentinel - debugfs_create_files() 순회 종료 조건 */
}; /* [한국어] deadline_queue_debugfs_attrs[] 정의 끝 */
#undef DEADLINE_QUEUE_DDIR_ATTRS /* [한국어] 배열 조립에만 쓰인 매크로 정리 - 네임스페이스 오염 방지 */
#endif /* [한국어] 이 지점에서 CONFIG_BLK_DEBUG_FS 분기(위의 #ifdef, 이 파일 상단 주석 참고)가 닫힘 */


/*
 * [한국어]
 * mq_deadline - 이 파일이 구현하는 스케줄러를 blk-mq/elevator 코어에 알리는
 * 등록 서술자(struct elevator_type, block/elevator.h 정의).
 *
 * .ops(struct elevator_mq_ops) 안의 각 함수 포인터가 elevator.h가 정의하는
 * 21개 콜백 vtable 중 mq-deadline이 실제로 구현하는 항목들이며, 값이 없는
 * 콜백(allow_merge, completed_request, requeue_request 등)은 mq-deadline이
 * 사용하지 않아 자동으로 NULL(호출부가 NULL 체크 후 건너뜀)이 된다.
 * next_request/former_request에는 mq-deadline 자체 함수 대신 elevator.c가
 * 제공하는 공용 rb-tree 헬퍼(elv_rb_latter_request/elv_rb_former_request)를
 * 그대로 등록해, rb-tree 기반 스케줄러(BFQ 등)와 구현을 공유한다.
 * elv_register(&mq_deadline)이 이 구조체를 전역 elevator 목록에 등록하고,
 * 사용자가 /sys/block/<disk>/queue/scheduler에 "mq-deadline" 또는
 * "deadline"(elevator_alias)을 쓰면 이 서술자가 선택되어 dd_init_sched()가
 * 호출된다.
 */
static struct elevator_type mq_deadline = {
	.ops = {
		.depth_updated		= dd_depth_updated, /* [한국어] q->async_depth 변경 시 sched_tags shallow depth 재적용 */
		.limit_depth		= dd_limit_depth, /* [한국어] 비동기 요청의 tag 할당 깊이 제한 */
		.insert_requests	= dd_insert_requests, /* [한국어] request 리스트 일괄 삽입(rb-tree/fifo_list 편입) */
		.dispatch_request	= dd_dispatch_request, /* [한국어] 다음에 디스패치할 request 선택(핵심 정책 함수) */
		.prepare_request	= dd_prepare_request, /* [한국어] request가 스케줄러에 편입되기 전 elv.priv[0] 초기화 */
		.finish_request		= dd_finish_request, /* [한국어] request 완료 시 통계(completed) 갱신 */
		.next_request		= elv_rb_latter_request, /* [한국어] elevator.c 공용 헬퍼 재사용 - LBA 순서상 다음 request(rb_next) */
		.former_request		= elv_rb_former_request, /* [한국어] elevator.c 공용 헬퍼 재사용 - LBA 순서상 이전 request(rb_prev) */
		.bio_merge		= dd_bio_merge, /* [한국어] bio를 기존 request에 병합 시도(신규 request 할당 전) */
		.request_merge		= dd_request_merge, /* [한국어] bio를 기존 request의 front merge 후보로 탐색 */
		.requests_merged	= dd_merged_requests, /* [한국어] 두 request가 하나로 병합된 직후 처리(사라지는 쪽 제거) */
		.request_merged		= dd_request_merged, /* [한국어] bio가 기존 request에 병합된 직후 처리(front merge 시 rb-tree 재정렬) */
		.has_work		= dd_has_work, /* [한국어] 디스패치할 작업이 있는지 확인 */
		.init_sched		= dd_init_sched, /* [한국어] 스케줄러 인스턴스(deadline_data) 할당/초기화 */
		.exit_sched		= dd_exit_sched, /* [한국어] 스케줄러 인스턴스 해제 및 통계 정합성 검증 */
	}, /* [한국어] .ops(elevator_mq_ops) 초기화 끝 */

#ifdef CONFIG_BLK_DEBUG_FS /* [한국어] 디버깅 전용 필드이므로 CONFIG_BLK_DEBUG_FS가 켜진 빌드에서만 대입 */
	.queue_debugfs_attrs = deadline_queue_debugfs_attrs, /* [한국어] 위에서 정의한 debugfs 속성 테이블 연결 - elevator switch 시 block/blk-mq-sched.c가 이 필드를 보고 debugfs 서브 항목을 등록 */
#endif /* [한국어] CONFIG_BLK_DEBUG_FS 분기 종료 */
	.elevator_attrs = deadline_attrs, /* [한국어] sysfs 튜너블 6종 테이블 연결(read_expire 등) - block/elevator.c가 elevator switch 시 이 배열로 /sys/.../iosched/* 파일을 생성 */
	.elevator_name = "mq-deadline", /* [한국어] /sys/block/<disk>/queue/scheduler에서 이 스케줄러를 선택하는 정식 이름 */
	.elevator_alias = "deadline", /* [한국어] 레거시 호환용 별칭 - 과거 단일 큐 deadline 스케줄러 이름으로도 이 스케줄러를 선택할 수 있게 함 */
	.elevator_owner = THIS_MODULE, /* [한국어] 이 스케줄러를 제공하는 커널 모듈 - 모듈 참조 카운트 관리(사용 중 rmmod 방지)에 사용 */
}; /* [한국어] mq_deadline 서술자 정의 끝 */
MODULE_ALIAS("mq-deadline-iosched"); /* [한국어] 모듈 별칭 등록 - "mq-deadline-iosched" 이름으로도 modprobe 자동 로드가 가능하게 함(예: elevator=mq-deadline-iosched 커널 커맨드라인 호환) */

/*
 * [한국어]
 * deadline_init - 모듈 초기화 함수. mq-deadline 스케줄러를 elevator 코어에 등록.
 *
 * @return: elv_register()의 반환값(0이면 성공, 음수면 실패 errno).
 *
 * 모듈이 insmod/modprobe로 적재되거나, 이 스케줄러가 빌트인이면 커널 부팅
 * 시 elevator_init 초기화 단계에서 호출된다. 이 함수가 성공해야 사용자가
 * /sys/block/<disk>/queue/scheduler에 "mq-deadline"을 선택할 수 있게 된다.
 * 실행 컨텍스트: 모듈 로드 경로 또는 커널 초기화 경로(프로세스 컨텍스트).
 * 호출자: 모듈 로더(module_init 매크로가 등록)가 insmod/modprobe 시,
 *   또는 빌트인이면 커널 초기화 시퀀스가 호출.
 * 피호출자: elv_register()(block/elevator.c - 전역 elevator 리스트에 등록).
 * 에러 경로: elv_register() 실패 시(예: 동일 이름 중복 등록) 음수 errno를
 *   그대로 반환해 모듈 로드 자체를 실패시킴.
 *
 * 호출 체인:
 *   module_init → [deadline_init] → elv_register
 */
static int __init deadline_init(void)
{
	return elv_register(&mq_deadline); /* [한국어] mq_deadline 서술자를 전역 elevator 목록에 등록 - 이후 스케줄러 선택 시 이 이름/별칭으로 발견 가능해짐 */
}

/*
 * [한국어]
 * deadline_exit - 모듈 정리 함수. mq-deadline 스케줄러를 elevator 코어에서 해제.
 *
 * @return: 없음.
 *
 * 모듈이 rmmod로 언로드될 때 호출된다(빌트인이면 호출되지 않음 - __exit
 * 애트리뷰트가 이를 반영해 빌트인 빌드에서는 코드 자체가 제거될 수 있음).
 * 실행 컨텍스트: 모듈 언로드 경로(프로세스 컨텍스트) - 이 시점에는 이미 이
 * 스케줄러를 사용 중인 큐가 없음이 모듈 참조 카운트(elevator_owner =
 * THIS_MODULE)로 보장된다.
 * 호출자: 모듈 언로더(module_exit 매크로가 등록)가 rmmod 시 호출.
 * 피호출자: elv_unregister()(block/elevator.c - 전역 목록에서 제거).
 *
 * 호출 체인:
 *   module_exit → [deadline_exit] → elv_unregister
 */
static void __exit deadline_exit(void)
{
	elv_unregister(&mq_deadline); /* [한국어] 전역 elevator 목록에서 mq_deadline 서술자 제거 - 이후로는 이 이름으로 스케줄러를 선택할 수 없음 */
}

module_init(deadline_init); /* [한국어] module_init 매크로 - 모듈 적재/커널 초기화 시 deadline_init()이 호출되도록 등록 */
module_exit(deadline_exit); /* [한국어] module_exit 매크로 - 모듈 언로드(rmmod) 시 deadline_exit()이 호출되도록 등록 */

MODULE_AUTHOR("Jens Axboe, Damien Le Moal and Bart Van Assche"); /* [한국어] 모듈 작성자 메타데이터 - modinfo(8) 등에 노출 */
MODULE_LICENSE("GPL"); /* [한국어] 모듈 라이선스 선언 - GPL 전용 커널 심볼 사용 가능함을 명시(필수 매크로, 없으면 로드 시 taint 경고) */
MODULE_DESCRIPTION("MQ deadline IO scheduler"); /* [한국어] modinfo(8)에 노출되는 한 줄 설명 */
