// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to sysfs handling
 */
/*
 * [한국어 설명] request_queue의 sysfs(/sys/block/<disk>/queue/) 및 debugfs 등록/해제 (blk-sysfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block layer의 request_queue(q)가 보유한 튜닝 가능한 파라미터들을
 * kobject/sysfs 인터페이스로 캡슐화하여, 사용자공간이 /sys/block/<disk>/queue/
 * 아래의 파일들(nr_requests, max_sectors_kb, scheduler, io_timeout, write_cache,
 * nomerges, zoned_qd1_writes 등)을 통해 큐 동작을 열람·조정할 수 있게 한다.
 * 또한 request_queue와 짝을 이루는 debugfs 트리(/sys/kernel/debug/block/<disk>/)의
 * 등록/해제, blk-crypto sysfs, 독립 접근 범위(independent access ranges) sysfs를
 * 함께 관리하여 디스크 하나가 add_disk()/del_gendisk() 될 때 필요한 모든 사용자공간
 * 인터페이스 생명주기를 이 파일에서 조율한다. NVMe SSD 관점에서는 이 파일이
 * nr_requests(SQ, Submission Queue에 대응하는 소프트웨어 큐 깊이), io_timeout(명령
 * 타임아웃 복구 기준), write_cache(VWC, Volatile Write Cache 활성화 여부),
 * max_sectors_kb(PRP/SGL 준비에 쓰이는 최대 I/O 크기) 등 NVMe 큐 특성을 사용자공간에서
 * 조율하는 유일한 창구다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트: 커널 블록 레이어 내부이며, 사용자공간의 sysfs read(2)/write(2)
 * 시스템 호출을 처리하는 프로세스 컨텍스트에서 실행된다(커널 모듈/인터럽트
 * 컨텍스트에서는 호출되지 않는다).
 * 등록 경로: 드라이버 probe -> add_disk() -> blk_register_queue()(본 파일)가
 *   disk->queue_kobj를 disk_to_dev(disk)->kobj 아래 "queue"라는 이름으로
 *   kobject_add()하고, blk_queue_ktype(본 파일)의 sysfs_ops를 통해 이후의
 *   모든 read/write가 queue_attr_show()/queue_attr_store()로 라우팅되게 만든다.
 * 해제 경로: 드라이버 제거 -> del_gendisk() -> blk_unregister_queue()(본 파일)가
 *   등록의 역순으로 sysfs/debugfs 엔트리를 제거한다.
 * 조회/설정 경로: 사용자가 "echo N > /sys/block/<disk>/queue/nr_requests" 등을
 *   실행하면 VFS -> kernfs -> queue_sysfs_ops.store -> queue_attr_store() ->
 *   해당 struct queue_sysfs_entry의 .store 또는 .store_limit 콜백 순으로
 *   호출되며, 최종적으로 blk_mq_update_nr_requests(), queue_limits_commit_update_frozen()
 *   등을 거쳐 request_queue 상태가 바뀐다. 이렇게 바뀐 값들은 이후 I/O 경로인
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   -> nvme_submit_cmd(SQ doorbell 레지스터 기록)에서 실제로 참조된다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk.h: struct request_queue 세부 필드, blk_queue_flag_*(),
 *     blk_debugfs_lock()/blk_debugfs_unlock() 등 내부 헬퍼 선언.
 *   - block/blk-mq.h, block/blk-mq-sched.h: blk_mq_freeze_queue()/unfreeze_queue(),
 *     blk_mq_update_nr_requests(), struct elevator_tags(스케줄러 shadow tag 풀).
 *   - block/blk-mq-debugfs.h: blk_mq_debugfs_register() — hctx/SQ/CQ 상태 파일.
 *   - block/blk-rq-qos.h, block/blk-wbt.h: writeback throttle(WBT)의
 *     wbt_lat_usec 속성과 disk->rqos_state_mutex 동기화.
 *   - block/blk-cgroup.h, block/blk-throttle.h: cgroup/스로틀 관련 헬퍼
 *     (이 파일이 직접 속성을 노출하지는 않으나 헤더 체인을 통해 함께 관리됨, 추정).
 * 피의존 모듈(이 파일의 함수를 호출하는 쪽):
 *   - block/genhd.c: add_disk()/del_gendisk()가 각각
 *     blk_register_queue()/blk_unregister_queue()를 호출한다.
 *   - block/elevator.c: elv_iosched_store()(스케줄러 전환) 등이 본 파일과
 *     동일한 update_nr_hwq_lock/elevator_lock 잠금 규약을 공유한다.
 * 공유 자료구조:
 *   - struct request_queue(및 그 안의 struct queue_limits): 이 파일의 모든
 *     show/store 콜백이 읽거나 쓰는 대상. limits 필드는 q->limits_lock으로,
 *     elevator/nr_requests/async_depth는 q->elevator_lock으로 보호된다.
 *   - struct queue_sysfs_entry: 이 파일이 정의하는 sysfs attribute 캡슐화
 *     구조체로, kobj_type인 blk_queue_ktype과 attribute_group인
 *     queue_attr_group/blk_mq_queue_attr_group을 통해 kobject 코어와 연결된다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct queue_sysfs_entry  — 속성 이름/권한과 4가지 콜백(show/show_limit/
 *                             store/store_limit)을 담는 sysfs attribute 엔트리.
 * queue_attr_show()/queue_attr_store() — kobj_type.sysfs_ops의 실제 진입점.
 *                             entry의 콜백 종류에 따라 limits_lock 필요 여부를 분기.
 * queue_requests_store()   — nr_requests(NVMe SQ 소프트웨어 큐 깊이) 변경:
 *                             elevator_tags 재할당과 freeze/unfreeze를 수반.
 * queue_max_sectors_store()/queue_wc_store() 등 *_store_limit 계열 —
 *                             queue_limits_start_update()로 임시 사본을 만들어
 *                             검증한 뒤 queue_limits_commit_update_frozen()으로
 *                             원자적으로 반영하는 store_limit 콜백 규약을 따른다.
 * blk_register_queue()     — add_disk() 경로에서 queue_kobj/debugfs/crypto/
 *                             IA ranges를 등록하고 elevator 기본값을 설정한 뒤,
 *                             마지막에 QUEUE_FLAG_INIT_DONE 설정과 percpu 카운터 전환을 수행.
 * blk_unregister_queue()   — 등록의 역순 해제. del_gendisk() 경로에서 호출된다.
 */
/* linux/kernel.h: printk 계열(예: pr_info_ratelimited)과 기본 매크로(min() 등) 제공. */
#include <linux/kernel.h>
/*
 * linux/slab.h: kmalloc/kfree 계열 메모리 할당 인터페이스 선언 - 이 파일이 직접 호출하지는
 * 않지만(추정) 포함하는 다른 블록 레이어 헤더들과의 호환을 위해 포함된다.
 */
#include <linux/slab.h>
/*
 * linux/module.h: THIS_MODULE, EXPORT_SYMBOL 계열 매크로 - 블록 레이어 공통 헤더 체인의
 * 일부로 포함되며, 이 파일 자체가 심볼을 export 하지는 않는다.
 */
#include <linux/module.h>
/*
 * linux/bio.h: struct bio, bio 관련 헬퍼 선언 - bio 기반(non-mq) 큐와 요청 기반(mq) 큐를
 * 구분하는 queue_is_mq() 판단과 함께 쓰이는 블록 I/O 기본 타입을 제공한다.
 */
#include <linux/bio.h>
/*
 * linux/blkdev.h: struct request_queue, struct gendisk, struct queue_limits 등 이 파일이
 * sysfs로 노출하는 모든 자료구조의 정의가 들어 있는 핵심 헤더.
 */
#include <linux/blkdev.h>
/*
 * linux/backing-dev.h: struct backing_dev_info(disk->bdi) 정의 - queue_ra_show/store가
 * read_ahead_kb(readahead 페이지 수)를 읽고 쓸 때 사용.
 */
#include <linux/backing-dev.h>
/*
 * linux/blktrace_api.h: blk_trace_shutdown() 선언 - blk_debugfs_remove()에서 큐 제거 시
 * 진행 중이던 blktrace 세션을 정리하기 위해 호출.
 */
#include <linux/blktrace_api.h>
/*
 * linux/debugfs.h: debugfs_create_dir()/debugfs_remove_recursive() 등 - request_queue의
 * /sys/kernel/debug/block/<disk> 트리를 생성/삭제하는 데 사용.
 */
#include <linux/debugfs.h>

/*
 * "blk.h": 블록 레이어 내부 전용 헤더. blk_queue_flag_*() 매크로, blk_debugfs_lock()/
 * blk_debugfs_unlock() 등 이 파일이 의존하는 내부 헬퍼가 선언되어 있다.
 */
#include "blk.h"
/*
 * "blk-mq.h": blk-mq(멀티큐) 코어 선언. blk_mq_freeze_queue()/blk_mq_unfreeze_queue(),
 * blk_mq_update_nr_requests(), blk_mq_can_poll() 등 큐 재구성 시 필요한 함수들.
 */
#include "blk-mq.h"
/*
 * "blk-mq-debugfs.h": blk_mq_debugfs_register() 선언 - blk-mq 큐의 hctx(하드웨어
 * 디스패치 큐, NVMe라면 SQ/CQ에 대응) 단위 debugfs 파일들을 등록한다.
 */
#include "blk-mq-debugfs.h"
/*
 * "blk-mq-sched.h": I/O 스케줄러(mq-deadline/kyber/bfq) tag 풀인 elevator_tags를 관리하는
 * blk_mq_alloc_sched_tags()/blk_mq_free_sched_tags() 선언 - nr_requests 변경 시 사용.
 */
#include "blk-mq-sched.h"
/*
 * "blk-rq-qos.h": rq_qos(요청 QoS) 플러그인 프레임워크 선언 - WBT(Writeback Throttle)가
 * 이 프레임워크 위에 구현되어 있어 blk-wbt.h와 함께 포함된다.
 */
#include "blk-rq-qos.h"
/*
 * "blk-wbt.h": Writeback Throttle(WBT) 인터페이스 - wbt_rq_qos(), wbt_get_min_lat(),
 * wbt_set_lat(), wbt_init_enable_default() 등 wbt_lat_usec 속성 구현에 필요한 선언.
 */
#include "blk-wbt.h"
/*
 * "blk-cgroup.h": 블록 cgroup(blkcg) 관련 선언 - 이 파일이 직접 속성을 노출하지는
 * 않으나(추정) 큐 해제 경로 등에서 함께 정리되는 블록 레이어 공통 헤더.
 */
#include "blk-cgroup.h"
/*
 * "blk-throttle.h": BPS/IOPS 토큰 버킷 스로틀 관련 선언 - 이 파일에서 직접 호출하는
 * 흔적은 없지만(추정) 블록 레이어 헤더 체인의 관례상 함께 포함된다.
 */
#include "blk-throttle.h"

/*
 * [한국어]
 * struct queue_sysfs_entry - /sys/block/<disk>/queue/<attr> 파일 하나에 대응하는
 *   sysfs attribute 엔트리. attribute 이름/권한(attr)과 최대 4개의 콜백
 *   (show/show_limit/store/store_limit)을 하나로 묶어, kobj_type인
 *   blk_queue_ktype의 default_groups에 포함된 attribute_group(queue_attr_group,
 *   blk_mq_queue_attr_group)들이 배열로 나열한다.
 *
 * show/store 쌍과 show_limit/store_limit 쌍은 서로 배타적으로 쓰인다: 값이
 * struct queue_limits 안에 있어서 q->limits_lock으로 보호해야 하는 속성은
 * show_limit/store_limit을 쓰고(호출자인 queue_attr_show/queue_attr_store가
 * 락을 직접 잡아준다), q->elevator_lock 등 다른 락이 필요하거나 락이
 * 필요 없는 속성은 show/store를 써서 콜백 내부에서 스스로 락을 관리한다.
 * QUEUE_RO_ENTRY/QUEUE_RW_ENTRY/QUEUE_LIM_RO_ENTRY/QUEUE_LIM_RW_ENTRY
 * 매크로(아래)가 이 구조체의 정적 인스턴스를 생성하는 공장 역할을 한다.
 */
struct queue_sysfs_entry {
	/*
	 * attribute 이름과 파일 모드(0444/0644)를 담는 임베디드 struct attribute.
	 * 설정자: QUEUE_RO_ENTRY 등 매크로가 .attr = { .name = _name, .mode = ... }로 초기화.
	 * 읽는 자: kobject/sysfs core가 디렉터리 엔트리 이름과 파일 접근 권한 결정에 사용하고,
	 *   to_queue() 매크로가 container_of로 이 필드의 주소에서 상위 entry를 복원한다.
	 * 값 범위: name은 "nr_requests", "max_sectors_kb" 등 sysfs 파일명 문자열.
	 * 동기화: 컴파일 타임에 고정되는 정적 데이터라 런타임 동기화가 필요 없다.
	 */
	struct attribute attr;
	/*
	 * 잠금 없이(또는 자체 락으로) 값을 읽어 문자열로 만드는 show 콜백.
	 * 설정자: QUEUE_RO_ENTRY/QUEUE_RW_ENTRY 매크로가 <prefix>_show 함수 포인터로 초기화.
	 * 읽는 자: queue_attr_show()가 entry->show_limit이 NULL일 때 이 콜백을 호출한다.
	 * 값 범위: NULL이면 이 attribute는 show_limit 콜백만 쓰는 것으로 간주된다.
	 * 동기화: 콜백 스스로 필요한 락(q->elevator_lock 등)을 잡아야 한다 - queue_attr_show는
	 *   별도로 락을 걸어주지 않는다.
	 */
	ssize_t (*show)(struct gendisk *disk, char *page);
	/*
	 * struct queue_limits 필드를 q->limits_lock 보호 하에 읽는 show_limit 콜백.
	 * 설정자: QUEUE_LIM_RO_ENTRY/QUEUE_LIM_RW_ENTRY 매크로가 <prefix>_show로 초기화.
	 * 읽는 자: queue_attr_show()가 이 필드가 non-NULL이면 disk->queue->limits_lock을
	 *   직접 잡은 뒤 호출한다(호출자가 락을 대신 관리하는 것이 show와의 차이).
	 * 값 범위: NULL이면 위 show 콜백만 쓰는 것으로 간주된다.
	 * 동기화: 호출자(queue_attr_show)가 limits_lock을 잡고 호출하므로 콜백 내부에서
	 *   다시 잠글 필요는 없다.
	 */
	ssize_t (*show_limit)(struct gendisk *disk, char *page);

	/*
	 * q->elevator_lock 등 자체 락으로 보호되는 값을 사용자 입력으로 갱신하는 store 콜백.
	 * 설정자: QUEUE_RW_ENTRY 매크로가 <prefix>_store 함수 포인터로 초기화.
	 * 읽는 자: queue_attr_store()가 entry->store_limit이 NULL일 때 이 콜백을 호출한다.
	 * 값 범위: NULL이면 읽기 전용(QUEUE_RO_ENTRY) attribute라는 뜻.
	 * 동기화: 콜백 스스로 필요한 락/freeze를 처리해야 한다.
	 */
	ssize_t (*store)(struct gendisk *disk, const char *page, size_t count);
	/*
	 * struct queue_limits의 임시 사본(lim)을 받아 검증 후 값을 채우는 store_limit 콜백.
	 * 설정자: QUEUE_LIM_RW_ENTRY 매크로가 <prefix>_store로 초기화.
	 * 읽는 자: queue_attr_store()가 queue_limits_start_update()로 만든 lim 사본을 넘겨
	 *   이 콜백을 호출하고, 반환값이 0 이상이면 queue_limits_commit_update_frozen()으로
	 *   실제 q->limits에 원자적으로 반영한다.
	 * 값 범위: 반환값은 성공 시 0, 실패 시 음수 errno(예: -EINVAL). count는 사용하지 않고
	 *   파싱 성공/실패만 반환하는 방식(길이 자체는 항상 호출자가 처리).
	 * 동기화: 호출 시점에 이미 q->limits_lock이 잠겨 있으므로(queue_limits_start_update가
	 *   잠금) 콜백 내부에서 별도 락이 필요 없다 - lim은 스택/지역 사본이라 단일 호출자만 접근.
	 */
	int (*store_limit)(struct gendisk *disk, const char *page,
			size_t count, struct queue_limits *lim);
};

/*
 * [한국어]
 * queue_var_show - unsigned long 값을 십진 문자열로 sysfs 페이지에 출력
 *
 * @var: 출력할 값(예: q->nr_requests, q->async_depth, queue_limits의 각 필드).
 * @page: sysfs read(2)가 커널로부터 제공한 PAGE_SIZE 크기의 출력 버퍼.
 * @return: sysfs_emit()이 기록한 바이트 수(NUL 제외), page가 넘치면 잘림.
 *
 * 이 파일의 거의 모든 정수형 show 콜백(nr_requests, max_segments,
 * logical_block_size 등)이 공유하는 최소 공통 분모 헬퍼다. 단일 sysfs_emit()
 * 호출만 수행하며 락이나 부수효과가 없어 재진입이 자유롭다.
 * 실행 컨텍스트: sysfs read(2)를 처리하는 프로세스 컨텍스트.
 * 호출자: queue_requests_show(), queue_async_depth_show(), QUEUE_SYSFS_LIMIT_SHOW
 *   매크로가 생성하는 각 queue_<field>_show(), queue_nomerges_show() 등 다수.
 * 피호출자: sysfs_emit() (커널 sysfs 코어의 안전한 포맷 출력 함수).
 * 에러 경로: 없음(실패를 표현하지 않는 단순 포맷터).
 *
 * 호출 체인:
 *   각 queue_*_show() → [queue_var_show] → sysfs_emit
 */
static ssize_t
queue_var_show(unsigned long var, char *page)
{
	/* var를 "%lu\n" 형식으로 page에 기록 - sysfs read(2)의 최종 응답 문자열이 된다. */
	return sysfs_emit(page, "%lu\n", var);
}

/*
 * [한국어]
 * queue_var_store - 사용자가 write(2)로 넘긴 십진 문자열을 unsigned long으로 파싱
 *
 * @var: 파싱 결과를 저장할 출력 포인터(호출자의 지역 변수 주소).
 * @page: sysfs write(2)가 전달한 사용자 입력 버퍼(NUL 종료 문자열로 이미 변환됨).
 * @count: page의 바이트 길이(사용자가 write()에 전달한 길이).
 * @return: 성공 시 count(호출자가 그대로 사용자에게 반환할 처리 바이트 수),
 *   실패(파싱 오류 또는 UINT_MAX 초과) 시 -EINVAL.
 *
 * 이 파일의 거의 모든 정수형 store 콜백이 공유하는 입력 파싱 공통 헬퍼다.
 * kstrtoul()로 10진 파싱을 하되, 값이 unsigned int(UINT_MAX) 범위를 넘으면
 * 거부한다 - queue_limits의 여러 필드가 unsigned int이기 때문에 여기서
 * 미리 범위를 제한해 하위 저장소에서의 truncation을 막는다.
 * 실행 컨텍스트: sysfs write(2)를 처리하는 프로세스 컨텍스트.
 * 호출자: queue_requests_store(), queue_async_depth_store(), queue_ra_store(),
 *   queue_max_discard_sectors_store() 등 이 파일의 대다수 store 콜백.
 * 피호출자: kstrtoul() (커널 문자열->정수 안전 변환 함수).
 * 에러 경로: kstrtoul() 실패(err != 0, 숫자가 아닌 입력) 또는 UINT_MAX 초과 시
 *   -EINVAL을 반환하고 *var는 건드리지 않는다.
 *
 * 호출 체인:
 *   각 queue_*_store() → [queue_var_store] → kstrtoul
 */
static ssize_t
queue_var_store(unsigned long *var, const char *page, size_t count)
{
	/* 파싱 결과를 담을 커널 표준 오류 코드 변수. */
	int err;
	/* kstrtoul()이 채워 넣을 파싱된 값 - 아직 UINT_MAX 범위 검증 전 임시 저장소. */
	unsigned long v;

	/* page 문자열을 10진수로 해석해 v에 저장 - 사용자가 echo로 쓴 십진 텍스트를 정수로 변환. */
	err = kstrtoul(page, 10, &v);
	/*
	 * 파싱 자체가 실패했거나(err != 0, 숫자가 아닌 입력) v가 unsigned int 범위를 넘으면
	 *   하위 필드(대부분 unsigned int)에 안전하게 저장할 수 없다고 보고 거부.
	 */
	if (err || v > UINT_MAX)
		/* 잘못된 입력 - 사용자에게 -EINVAL(Invalid argument)을 돌려준다. */
		return -EINVAL;

	/* 검증을 통과한 값을 호출자가 넘긴 출력 포인터에 반영. */
	*var = v;

	/* sysfs write(2) 규약: 처리한 전체 바이트 수(count)를 그대로 반환해야 성공으로 간주됨. */
	return count;
}

/*
 * [한국어]
 * queue_requests_show - /sys/block/<disk>/queue/nr_requests 읽기
 *
 * @disk: 조회 대상 gendisk. disk->queue가 실제 request_queue를 가리킨다.
 * @page: 결과 문자열을 담을 sysfs 출력 버퍼.
 * @return: queue_var_show()가 기록한 바이트 수.
 *
 * nr_requests는 이 큐가 동시에 유지할 수 있는 소프트웨어 request 슬롯 수이며,
 * NVMe 관점에서는 SQ(Submission Queue)에 대응하는 blk-mq tag 풀의 논리적
 * 상한이다. q->elevator가 스케줄러 자체 tag pool(elevator_tags)을 쓸 때도
 * q->nr_requests 필드 자체는 항상 최신 값으로 유지되므로 이 값만 읽으면 된다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * 호출자: queue_requests_entry.show를 통해 queue_attr_show()가 호출.
 * 피호출자: queue_var_show().
 * 동시성: q->elevator_lock으로 nr_requests 필드를 보호 - queue_requests_store()나
 *   엘리베이터 전환과 동시에 읽어도 일관된 값을 보장한다.
 *
 * 호출 체인:
 *   queue_attr_show → [queue_requests_show] → queue_var_show
 */
static ssize_t queue_requests_show(struct gendisk *disk, char *page)
{
	/* queue_var_show()의 반환값(기록 바이트 수)을 담을 변수. */
	ssize_t ret;

	/*
	 * nr_requests 필드는 elevator_lock으로 보호됨 - queue_requests_store()의 동시 갱신이나
	 *   엘리베이터 전환 도중 읽어도 값이 찢기지(tearing) 않도록 잠근다.
	 */
	mutex_lock(&disk->queue->elevator_lock);
	/* 보호 구간 안에서 현재 nr_requests 값을 문자열로 변환. */
	ret = queue_var_show(disk->queue->nr_requests, page);
	/* 락 해제 - 임계구역은 필드 읽기 한 줄뿐이므로 짧게 유지. */
	mutex_unlock(&disk->queue->elevator_lock);
	/* 변환된 문자열의 바이트 수를 sysfs read(2) 결과로 반환. */
	return ret;
}

/*
 * [한국어]
 * queue_requests_store - /sys/block/<disk>/queue/nr_requests 쓰기: NVMe SQ 소프트웨어
 *   큐 깊이(nr_requests) 변경
 *
 * @disk: 대상 gendisk.
 * @page: 사용자가 write(2)로 넘긴 새 nr_requests 값(십진 문자열).
 * @count: page의 바이트 길이.
 * @return: 성공 시 count, 실패 시 음수 errno(-EINVAL/-EBUSY/-ENOMEM 등).
 *
 * nr_requests를 바꾸면 blk-mq tag_set의 tag 풀 크기 자체가 바뀌어야 하므로,
 * 단순 필드 대입이 아니라 (1) 새 값 검증, (2) 필요 시 스케줄러 tag 풀
 * (elevator_tags) 사전 할당, (3) 큐 freeze, (4) blk_mq_update_nr_requests()로
 * 실제 tag 풀 교체, (5) unfreeze 순서를 지켜야 한다. 이 값은 NVMe
 * nvme_queue_rq()가 blk_mq_get_request()로 확보할 수 있는 tag(=NVMe CID) 수의
 * 상한과 직결된다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트. set->update_nr_hwq_lock을
 *   trylock으로 잡아 nr_hw_queues 변경(하드웨어 큐 개수 재조정)이나 다른
 *   nr_requests 변경과의 동시 실행을 막는다. del_gendisk()의 kobject_del()이
 *   쥐는 kn->active와 update_nr_hwq_lock 사이에 역순 잠금 의존성이 생길 수
 *   있어(원본 영어 주석 참고) 반드시 down_write_trylock()을 사용해 블로킹
 *   데드락을 피한다.
 * 호출자: queue_requests_entry.store를 통해 queue_attr_store()가 호출.
 * 피호출자: queue_var_store(), blk_mq_alloc_sched_tags(), blk_mq_freeze_queue(),
 *   blk_mq_update_nr_requests(), blk_mq_unfreeze_queue(), blk_mq_free_sched_tags().
 * 에러 경로: update_nr_hwq_lock 획득 실패 시 -EBUSY. nr_requests가 reserved_tags
 *   이하이거나 스케줄러/비스케줄러 한도(MAX_SCHED_RQ, set->queue_depth)를
 *   초과하면 -EINVAL. elevator_tags 사전 할당 실패 시 -ENOMEM.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_requests_store] → blk_mq_freeze_queue →
 *     blk_mq_update_nr_requests → blk_mq_unfreeze_queue
 */
static ssize_t
queue_requests_store(struct gendisk *disk, const char *page, size_t count)
{
	/* 이후 반복 참조할 request_queue 포인터를 지역 변수로 캐시. */
	struct request_queue *q = disk->queue;
	/* NVMe tag_set - queue_depth/reserved_tags 등 하드웨어 tag 풀 한도를 담고 있다. */
	struct blk_mq_tag_set *set = q->tag_set;
	/*
	 * 스케줄러(mq-deadline/kyber/bfq)가 새로 필요로 할 수 있는 shadow tag 풀 포인터 -
	 *   기본값 NULL은 '사전 할당 불필요'를 의미.
	 */
	struct elevator_tags *et = NULL;
	/* blk_mq_freeze_queue()가 반환하는 GFP 플래그 스냅샷 - unfreeze 시 그대로 복원해야 함. */
	unsigned int memflags;
	/* 사용자가 요청한 새 nr_requests 값(파싱 전). */
	unsigned long nr;
	/* 이 함수의 최종 반환값(성공 시 count, 실패 시 음수 errno). */
	int ret;

	/* 사용자 입력 문자열을 nr에 파싱. */
	ret = queue_var_store(&nr, page, count);
	/* 파싱 자체가 실패했으면(잘못된 숫자 등) 더 진행하지 않고 즉시 에러 반환. */
	if (ret < 0)
		/* queue_var_store()가 이미 -EINVAL 등을 설정해 두었으므로 그대로 전달. */
		return ret;

	/*
	 * Serialize updating nr_requests with concurrent queue_requests_store()
	 * and switching elevator.
	 *
	 * Use trylock to avoid circular lock dependency with kernfs active
	 * reference during concurrent disk deletion:
	 *   update_nr_hwq_lock -> kn->active (via del_gendisk -> kobject_del)
	 *   kn->active -> update_nr_hwq_lock (via this sysfs write path)
	 */
	/*
	 * trylock 사용 이유(위 원본 영어 주석): del_gendisk -> kobject_del이 쥐는 kn->active와
	 *   이 sysfs write 경로가 쥐는 update_nr_hwq_lock 사이에 역순 의존이 생길 수 있어,
	 *   블로킹 write_lock을 쓰면 데드락 가능성이 있다 - trylock 실패 시 그냥 -EBUSY로 포기.
	 */
	if (!down_write_trylock(&set->update_nr_hwq_lock))
		/*
		 * 락을 즉시 얻지 못하면(동시에 nr_hw_queues 변경 등이 진행 중) 사용자에게 -EBUSY 반환 -
		 *   재시도는 사용자공간(예: udev) 몫.
		 */
		return -EBUSY;

	/* 이미 같은 값이면 tag 풀을 건드릴 필요가 없다 - no-op 최적화. */
	if (nr == q->nr_requests)
		/* unlock 레이블로 점프해 락만 풀고 성공(ret==0)으로 반환. */
		goto unlock;

	/*
	 * NVMe를 포함한 모든 큐에 대해 blk-mq가 최소로 보장하는 request 슬롯 수(BLKDEV_MIN_RQ)
	 *   미만은 허용하지 않음 - 너무 얕은 큐는 병합/재시도 여유가 없어 성능이 급락한다.
	 */
	if (nr < BLKDEV_MIN_RQ)
	/* 요청값을 최소값으로 올림(clamp) - 사용자가 0이나 1처럼 비현실적인 값을 줘도 안전하게 보정. */
		nr = BLKDEV_MIN_RQ;

	/*
	 * 원본 영어 주석: 엘리베이터 전환은 update_nr_hwq_lock으로 보호된다 - elevator sysfs
	 *   속성(scheduler 파일)에서는 read lock을, nr_hw_queues 갱신 경로에서는 write lock을
	 *   잡으므로, 지금 이 write lock을 쥔 상태에서는 q->elevator를 안전하게 읽을 수 있다는
	 *   것이 아래 조건문(102행)이 q->elevator를 잠금 없이 직접 참조하는 근거다.
	 */
	/*
	 * Switching elevator is protected by update_nr_hwq_lock:
	 *  - read lock is held from elevator sysfs attribute;
	 *  - write lock is held from updating nr_hw_queues;
	 * Hence it's safe to access q->elevator here with write lock held.
	 */
	/*
	 * nr_requests의 유효 범위 검증: (1) set->reserved_tags(내부 예약 tag) 이하이면 실사용
	 *   가능한 tag가 없어 무의미, (2) 스케줄러가 붙어 있으면 스케줄러 shadow tag pool의
	 *   상한인 MAX_SCHED_RQ 초과 금지, (3) 스케줄러가 없으면(none) 하드웨어 tag_set의
	 *   queue_depth(NVMe가 보고한 실제 SQ 슬롯 수) 초과 금지.
	 */
	if (nr <= set->reserved_tags ||
	    (q->elevator && nr > MAX_SCHED_RQ) ||
	    (!q->elevator && nr > set->queue_depth)) {
		/* 위 세 조건 중 하나라도 걸리면 잘못된 요청. */
		ret = -EINVAL;
		/* unlock 레이블로 점프 - ret=-EINVAL 상태로 락만 해제 후 반환. */
		goto unlock;
	}

	/*
	 * tag 풀이 hctx(하드웨어 큐)별로 분리된 스케줄러(shared_tags가 아님)에서, 요청한 nr가
	 *   현재 스케줄러 tag 풀의 nr_requests보다 크면(즉 tag 풀을 늘려야 하면) 아래에서
	 *   새 elevator_tags를 미리 할당한다.
	 */
	if (!blk_mq_is_shared_tags(set->flags) && q->elevator &&
	    nr > q->elevator->et->nr_requests) {
		/*
		 * Tags will grow, allocate memory before freezing queue to
		 * prevent deadlock.
		 */
		/*
		 * freeze 전에 미리 새 스케줄러 tag 풀을 할당 - freeze된 상태(메모리 회수 경로 제한)에서
		 *   할당을 시도하면 데드락 위험이 있다는 것이 위 원본 영어 주석의 요지.
		 */
		et = blk_mq_alloc_sched_tags(set, q->nr_hw_queues, nr);
		/* 할당 실패 시 처리. */
		if (!et) {
			/* 메모리 부족을 사용자에게 알림. */
			ret = -ENOMEM;
			/* unlock 레이블로 점프 - 아직 아무 상태도 바꾸지 않았으므로 단순히 락만 풀면 됨. */
			goto unlock;
		}
	}

	/*
	 * 큐를 freeze - 새 request가 SQ에 들어오지 못하게 막고, 이미 처리 중인 request가
	 *   모두 drain될 때까지 대기(내부적으로 blocking) - tag 풀 교체 중 stale tag 참조 방지.
	 */
	memflags = blk_mq_freeze_queue(q);
	/* elevator_lock 획득 - q->elevator 필드와 스케줄러 내부 상태를 일관되게 갱신하기 위함. */
	mutex_lock(&q->elevator_lock);
	/*
	 * 실제 nr_requests 갱신 및 (필요 시) 스케줄러 tag 풀 교체 수행 - 반환값은 이제 불필요해진
	 *   '이전' elevator_tags(있다면) 포인터이며, 아래에서 해제 대상이 된다.
	 */
	et = blk_mq_update_nr_requests(q, et, nr);
	/* elevator_lock 해제. */
	mutex_unlock(&q->elevator_lock);
	/* freeze 해제 - NVMe SQ에 새 request가 다시 들어갈 수 있게 됨. */
	blk_mq_unfreeze_queue(q, memflags);

	/*
	 * blk_mq_update_nr_requests()가 더 이상 쓰이지 않는 elevator_tags를 반환했다면(예:
	 *   스케줄러가 shared_tags로 전환되었거나 축소된 경우) 해제 대상이 있다는 뜻.
	 */
	if (et)
		/* 이전 tag 풀 메모리 해제. */
		blk_mq_free_sched_tags(et, set);

/* unlock: 성공/실패를 불문하고 update_nr_hwq_lock 해제 후 ret을 반환하는 공통 종료 지점. */
unlock:
	/* write lock 해제 - 다른 sysfs write나 nr_hw_queues 변경이 다시 진행될 수 있게 함. */
	up_write(&set->update_nr_hwq_lock);
	/*
	 * ret을 그대로 반환 - queue_var_store()가 남긴 count(성공) 값이 EINVAL 등으로
	 *   갱신되지 않았다면 그대로 사용자에게 처리 바이트 수로 보고된다.
	 */
	return ret;
}

/*
 * [한국어]
 * queue_async_depth_show - /sys/block/<disk>/queue/async_depth 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 sysfs 출력 버퍼.
 * @return: queue_var_show()의 반환값.
 *
 * async_depth는 I/O 스케줄러(mq-deadline 등)가 비동기(non-sync) write에
 * 적용하는 소프트 큐 깊이 상한으로, 동기 read/write에 SQ 슬롯을 더 많이
 * 남겨두기 위한 튜닝 값이다(추정). q->elevator_lock으로 보호된다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * 호출자: queue_async_depth_entry.show를 통해 queue_attr_show().
 * 피호출자: queue_var_show().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_async_depth_show] → queue_var_show
 */
static ssize_t queue_async_depth_show(struct gendisk *disk, char *page)
{
	/*
	 * guard(mutex)(...): C23 style cleanup 속성 기반 락 - 함수를 벗어나는 모든 경로(return
	 *   포함)에서 자동으로 elevator_lock이 해제된다. async_depth가 elevator_lock으로
	 *   보호되는 이유는 스케줄러 전환과 동시에 읽을 때 값이 찢기지 않도록 하기 위함.
	 */
	guard(mutex)(&disk->queue->elevator_lock);

	/* 보호 구간 안에서 현재 async_depth 값을 문자열로 변환해 반환. */
	return queue_var_show(disk->queue->async_depth, page);
}

/*
 * [한국어]
 * queue_async_depth_store - /sys/block/<disk>/queue/async_depth 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 async_depth 값(십진 문자열).
 * @count: page 길이.
 * @return: 성공 시 ret(=queue_var_store가 남긴 count 또는 0), 실패 시 -EINVAL.
 *
 * blk-mq 큐에서만 의미가 있는 스케줄러 파라미터이므로 non-mq 큐는 즉시
 * 거부한다. 새 값은 현재 nr_requests를 넘지 않도록 clamp되며, 스케줄러의
 * depth_updated() 콜백(mq-deadline 등)을 호출해 내부 상태를 재계산시킨다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트. blk_mq_freeze_queue()로
 *   NVMe SQ에 새 request가 들어오지 못하게 막은 뒤 elevator_lock 하에 갱신한다.
 * 호출자: queue_async_depth_entry.store를 통해 queue_attr_store().
 * 피호출자: queue_var_store(), blk_mq_freeze_queue(), blk_mq_unfreeze_queue(),
 *   q->elevator->type->ops.depth_updated().
 * 에러 경로: non-mq 큐, 파싱 실패, nr==0, 스케줄러 미부착 시 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_async_depth_store] → blk_mq_freeze_queue →
 *     (elevator->type->ops.depth_updated) → blk_mq_unfreeze_queue
 */
static ssize_t
queue_async_depth_store(struct gendisk *disk, const char *page, size_t count)
{
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;
	/* blk_mq_freeze_queue()가 반환하는 GFP 플래그 스냅샷. */
	unsigned int memflags;
	/* 사용자가 요청한 새 async_depth 값. */
	unsigned long nr;
	/* 반환값(성공 시 count류, 실패 시 -EINVAL). */
	int ret;

	/* async_depth는 blk-mq 스케줄러 전용 개념이므로 legacy(bio 기반) 큐에는 의미가 없다. */
	if (!queue_is_mq(q))
		/* 지원하지 않는 큐 타입 - 즉시 거부. */
		return -EINVAL;

	/* 입력 문자열을 nr로 파싱. */
	ret = queue_var_store(&nr, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* queue_var_store가 설정한 에러 코드를 그대로 전달. */
		return ret;

	/* async_depth=0은 '깊이 없음'이라는 의미가 불명확해 허용하지 않음. */
	if (nr == 0)
		/* 잘못된 값 - 거부. */
		return -EINVAL;

	/* 큐 freeze: 스케줄러 depth 갱신 도중 NVMe SQ에 request가 새로 들어오지 않도록 함. */
	memflags = blk_mq_freeze_queue(q);
	/*
	 * scoped_guard(mutex, ...): 블록을 벗어나면 자동으로 elevator_lock을 해제하는 cleanup
	 *   기반 락 - q->elevator를 안전하게 참조/갱신하기 위해 필요.
	 */
	scoped_guard(mutex, &q->elevator_lock) {
	/* 스케줄러가 붙어 있어야만 async_depth 개념이 유효하다. */
		if (q->elevator) {
		/* 요청한 nr가 현재 nr_requests(전체 소프트웨어 큐 깊이)를 넘지 않도록 clamp. */
			q->async_depth = min(q->nr_requests, nr);
		/* 스케줄러가 depth 변경 시 내부 상태 재계산이 필요하면(콜백이 등록되어 있으면). */
			if (q->elevator->type->ops.depth_updated)
			/* mq-deadline 등 스케줄러의 depth_updated 콜백 호출 - 예: 비동기 요청 큐 크기 재조정. */
				q->elevator->type->ops.depth_updated(q);
	/* 스케줄러가 없는(none) 큐에는 async_depth 개념이 없음. */
		} else {
		/*
		 * 스케줄러 미부착 큐에 대한 요청이므로 -EINVAL로 표시 - scoped_guard 블록을
		 *   벗어나며 이 ret 값이 그대로 함수 반환값이 된다.
		 */
			ret = -EINVAL;
		/* 스케줄러 미부착 - 잘못된 요청으로 처리. */
		}
	}
	/* freeze 해제 - NVMe I/O 재개. */
	blk_mq_unfreeze_queue(q, memflags);

	/* 성공/실패 결과를 그대로 반환. */
	return ret;
}

/*
 * [한국어]
 * queue_ra_show - /sys/block/<disk>/queue/read_ahead_kb 읽기
 *
 * @disk: 조회 대상 gendisk. disk->bdi가 실제 readahead 페이지 수를 보관한다.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: queue_var_show()의 반환값.
 *
 * read_ahead_kb는 VFS/페이지 캐시가 순차 읽기를 감지했을 때 미리 읽어들이는
 * 바이트 수(KB 단위)를 제어한다. NVMe SSD는 회전식 디스크와 달리 탐색
 * 비용이 없어 readahead의 이득이 상대적으로 작을 수 있지만, 큰 순차 읽기의
 * SQ 오버헤드를 줄이는 데는 여전히 유효하다(추정).
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트. limits_lock으로 보호되는
 *   이유는 ra_pages가 보통 queue_limits_commit_update()에서 함께 계산되기
 *   때문(아래 queue_ra_store 원본 영어 주석 참고).
 * 호출자: queue_ra_entry.show를 통해 queue_attr_show()가 아니라(주의: queue_ra
 *   는 show/store 쌍을 쓰는 QUEUE_RW_ENTRY이지만 내부에서 limits_lock을 직접
 *   잡는다) queue_attr_show()가 entry->show를 그대로 호출.
 * 피호출자: queue_var_show().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_ra_show] → queue_var_show
 */
static ssize_t queue_ra_show(struct gendisk *disk, char *page)
{
	/* queue_var_show()의 반환값을 담을 변수. */
	ssize_t ret;

	/* ra_pages는 queue_limits 계산과 연동되므로 limits_lock으로 읽기를 보호. */
	mutex_lock(&disk->queue->limits_lock);
	/*
	 * ra_pages(4KB 페이지 단위)를 KB 단위로 변환 - PAGE_SHIFT는 페이지 크기의 log2,
	 *   PAGE_SHIFT-10만큼 왼쪽 시프트하면 '페이지 수 -> KB' 변환이 된다(PAGE_SHIFT>=10 가정).
	 */
	ret = queue_var_show(disk->bdi->ra_pages << (PAGE_SHIFT - 10), page);
	/* 락 해제. */
	mutex_unlock(&disk->queue->limits_lock);

	/* 변환 결과를 반환. */
	return ret;
}

/*
 * [한국어]
 * queue_ra_store - /sys/block/<disk>/queue/read_ahead_kb 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 read_ahead_kb 값(십진 문자열).
 * @count: page 길이.
 * @return: 성공 시 queue_var_store()가 반환한 count, 실패 시 -EINVAL.
 *
 * bdi->ra_pages는 보통 queue_limits_commit_update() -> blk_apply_bdi_limits()가
 * io_opt(NVMe가 보고하는 최적 I/O 크기) 등으로부터 자동 계산하지만, 사용자가
 * 이 sysfs 파일로 직접 덮어쓸 수도 있다. 두 갱신 경로가 경합하지 않도록
 * limits_lock을 공유하고, bdi->ra_pages 자체는 다른 CPU가 락 없이 읽을 수
 * 있으므로 WRITE_ONCE()로 한 번에 기록해 찢어진 값(torn write)을 방지한다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * 호출자: queue_ra_entry.store를 통해 queue_attr_store().
 * 피호출자: queue_var_store(), WRITE_ONCE().
 * 에러 경로: 파싱 실패 시 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_ra_store] → queue_var_store
 */
static ssize_t
queue_ra_store(struct gendisk *disk, const char *page, size_t count)
{
	/* 파싱된 KB 단위 read-ahead 값을 담을 변수. */
	unsigned long ra_kb;
	/* 반환값(성공 시 count, 실패 시 -EINVAL). */
	ssize_t ret;
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;

	/* 입력 문자열을 ra_kb로 파싱. */
	ret = queue_var_store(&ra_kb, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* queue_var_store가 설정한 에러 코드를 그대로 전달. */
		return ret;
	/*
	 * The ->ra_pages change below is protected by ->limits_lock because it
	 * is usually calculated from the queue limits by
	 * queue_limits_commit_update().
	 *
	 * bdi->ra_pages reads are not serialized against bdi->ra_pages writes.
	 * Use WRITE_ONCE() to write bdi->ra_pages once.
	 */
	/* queue_limits_commit_update()와 동일한 limits_lock을 잡아 계산값 덮어쓰기 경쟁을 방지. */
	mutex_lock(&q->limits_lock);
	/*
	 * KB -> 페이지 수 변환(오른쪽 시프트로 PAGE_SHIFT-10만큼 나눔) 후 WRITE_ONCE로 기록 -
	 *   다른 CPU가 락 없이 ra_pages를 읽을 수 있으므로 단일 원자적 저장을 보장해야 함.
	 */
	WRITE_ONCE(disk->bdi->ra_pages, ra_kb >> (PAGE_SHIFT - 10));
	/* 락 해제. */
	mutex_unlock(&q->limits_lock);

	/* 결과 반환. */
	return ret;
}

/*
 * [한국어]
 * QUEUE_SYSFS_LIMIT_SHOW(_field) - queue_limits._field 값을 그대로 출력하는
 *   queue_<field>_show() 함수를 찍어내는 코드 생성 매크로
 *
 * 아래 19개의 매크로 호출(QUEUE_SYSFS_LIMIT_SHOW(max_segments) 등)이 각각
 * "static ssize_t queue_max_segments_show(...)" 형태의 함수 정의로 전개된다.
 * 모든 필드가 unsigned int/unsigned short 등 정수형이고 단위 변환이 필요
 * 없는 경우에만 이 매크로를 쓰며, 섹터->바이트/KB 변환이 필요한 필드는
 * 아래의 QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES/KB를 대신 사용한다.
 * 생성된 함수들은 모두 QUEUE_LIM_RO_ENTRY의 .show_limit으로 등록되어
 * queue_attr_show()가 q->limits_lock을 잡은 채로 호출한다.
 *
 * 호출 체인(생성된 각 함수 공통):
 *   queue_attr_show(limits_lock 보유) → [queue_<field>_show] → queue_var_show
 */
#define QUEUE_SYSFS_LIMIT_SHOW(_field)					\
static ssize_t queue_##_field##_show(struct gendisk *disk, char *page)	\
{									\
	return queue_var_show(disk->queue->limits._field, page);	\
}

/*
 * max_segments: bio가 나뉠 수 있는 최대 세그먼트(불연속 메모리 조각) 수 -
 *   NVMe PRP list/SGL 엔트리 상한과 연결(추정).
 */
QUEUE_SYSFS_LIMIT_SHOW(max_segments)
/* max_discard_segments: Deallocate(Trim) 명령 하나에 포함 가능한 range 세그먼트 수 상한. */
QUEUE_SYSFS_LIMIT_SHOW(max_discard_segments)
/* max_integrity_segments: DIF/DIX 보호정보(PI)를 위한 별도 세그먼트 수 상한. */
QUEUE_SYSFS_LIMIT_SHOW(max_integrity_segments)
/* max_segment_size: 세그먼트 하나(연속 물리 메모리 조각)의 최대 바이트 크기. */
QUEUE_SYSFS_LIMIT_SHOW(max_segment_size)
/* max_write_streams: FDP(Flexible Data Placement) 등 다중 쓰기 스트림 지원 개수(추정). */
QUEUE_SYSFS_LIMIT_SHOW(max_write_streams)
/* write_stream_granularity: 쓰기 스트림에 요구되는 정렬/크기 단위(추정). */
QUEUE_SYSFS_LIMIT_SHOW(write_stream_granularity)
/* logical_block_size: NVMe LBA(Logical Block Address) 데이터 크기 - 보통 512B/4096B. */
QUEUE_SYSFS_LIMIT_SHOW(logical_block_size)
/* physical_block_size: 실제 NAND/미디어 쓰기 단위 - 일반적으로 4096B. */
QUEUE_SYSFS_LIMIT_SHOW(physical_block_size)
/* chunk_sectors: RAID stripe나 NVMe 네임스페이스 최적 청크 경계(섹터 단위). */
QUEUE_SYSFS_LIMIT_SHOW(chunk_sectors)
/*
 * io_min: 성능 저하 없이 수행 가능한 최소 I/O 크기(바이트, 섹터 단위 필드지만
 *   queue_var_show가 그대로 출력 - 단위는 물리 블록 크기와 동일 스케일로 취급됨).
 */
QUEUE_SYSFS_LIMIT_SHOW(io_min)
/* io_opt: NVMe 컨트롤러가 권장하는 최적 I/O 크기 힌트. */
QUEUE_SYSFS_LIMIT_SHOW(io_opt)
/* discard_granularity: Deallocate(Trim) 명령이 요구하는 정렬 단위. */
QUEUE_SYSFS_LIMIT_SHOW(discard_granularity)
/* zone_write_granularity: ZNS(Zoned Namespace)에서 zone 내부 쓰기 정렬 단위. */
QUEUE_SYSFS_LIMIT_SHOW(zone_write_granularity)
/* virt_boundary_mask: PRP/SGL 세그먼트가 걸치면 안 되는 가상 경계 마스크. */
QUEUE_SYSFS_LIMIT_SHOW(virt_boundary_mask)
/* dma_alignment: DMA 전송 시작 주소에 요구되는 정렬(바이트 단위, 마스크 형태). */
QUEUE_SYSFS_LIMIT_SHOW(dma_alignment)
/* max_open_zones: ZNS에서 동시에 Open 상태로 유지 가능한 zone 개수 상한. */
QUEUE_SYSFS_LIMIT_SHOW(max_open_zones)
/* max_active_zones: ZNS에서 동시에 Active 상태로 유지 가능한 zone 개수 상한. */
QUEUE_SYSFS_LIMIT_SHOW(max_active_zones)
/*
 * atomic_write_unit_min: NVMe FAW(원자적 쓰기) 최소 보장 단위(섹터 등가 필드지만
 *   이 매크로로는 원시 값 그대로 노출).
 */
QUEUE_SYSFS_LIMIT_SHOW(atomic_write_unit_min)
/* atomic_write_unit_max: NVMe FAW 최대 보장 단위. */
QUEUE_SYSFS_LIMIT_SHOW(atomic_write_unit_max)

/*
 * [한국어]
 * QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(_field) - queue_limits._field(섹터
 *   단위, 512B)를 바이트 단위로 환산해 출력하는 함수를 찍어내는 매크로
 *
 * SECTOR_SHIFT(=9)만큼 왼쪽 시프트해 "섹터 수 x 512"를 계산한다. sysfs
 * 관례상 discard/write-zeroes/atomic-write/zone-append 관련 한도는 바이트
 * 단위 문자열로 노출하는데, 매우 큰 장치에서 값이 unsigned int 범위를 넘을
 * 수 있어 unsigned long long으로 캐스팅한 뒤 "%llu" 포맷을 쓴다.
 *
 * 호출 체인(생성된 각 함수 공통):
 *   queue_attr_show(limits_lock 보유) → [queue_<field>_show] → sysfs_emit
 */
#define QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(_field)			\
static ssize_t queue_##_field##_show(struct gendisk *disk, char *page)	\
{									\
	return sysfs_emit(page, "%llu\n",				\
		(unsigned long long)disk->queue->limits._field <<	\
			SECTOR_SHIFT);					\
}

/* max_discard_sectors: 사용자가 sysfs로 설정 가능한(HW 한도 이내) 최대 Deallocate 범위. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_discard_sectors)
/* max_hw_discard_sectors: NVMe DSM(Dataset Management)/Deallocate 명령의 HW 최대 범위. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_hw_discard_sectors)
/* max_write_zeroes_sectors: NVMe Write Zeroes 명령의 최대 범위. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_write_zeroes_sectors)
/* max_hw_wzeroes_unmap_sectors: Write Zeroes와 동시에 unmap(할당 해제) 가능한 HW 최대 범위. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_hw_wzeroes_unmap_sectors)
/* max_wzeroes_unmap_sectors: 사용자가 sysfs로 설정 가능한 Write Zeroes unmap 범위. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_wzeroes_unmap_sectors)
/* atomic_write_max_sectors: NVMe FAW(원자적 쓰기) 최대 보장 범위. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(atomic_write_max_sectors)
/* atomic_write_boundary_sectors: 원자적 쓰기가 걸치면 안 되는 경계 간격. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(atomic_write_boundary_sectors)
/* max_zone_append_sectors: ZNS Zone Append 명령의 최대 전송 범위. */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_zone_append_sectors)

/*
 * [한국어]
 * QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_KB(_field) - queue_limits._field(섹터
 *   단위)를 KB 단위(오른쪽으로 1비트 시프트, 즉 /2)로 환산해 출력하는 매크로
 *
 * 섹터가 512B이므로 섹터 수를 2로 나누면 KB 수가 된다(512B * N섹터 / 1024 =
 * N/2 KB). max_sectors_kb/max_hw_sectors_kb는 역사적으로 바이트가 아닌
 * KB 단위 sysfs 관례를 따르는 대표적인 필드로, NVMe 관점에서는 한 I/O
 * 명령이 준비할 수 있는 PRP/SGL 리스트 길이의 실질적 상한이다.
 *
 * 호출 체인(생성된 각 함수 공통):
 *   queue_attr_show(limits_lock 보유) → [queue_<field>_show] → queue_var_show
 */
#define QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_KB(_field)			\
static ssize_t queue_##_field##_show(struct gendisk *disk, char *page)	\
{									\
	return queue_var_show(disk->queue->limits._field >> 1, page);	\
}

/*
 * max_sectors: 사용자/커널이 실제로 적용 중인 한 요청당 최대 전송 크기(KB) -
 *   NVMe PRP/SGL 준비 및 bio splitting 임계값에 직결.
 */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_KB(max_sectors)
/* max_hw_sectors: NVMe MDTS(Maximum Data Transfer Size)에서 유도된 하드웨어 최대치(KB). */
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_KB(max_hw_sectors)

/*
 * [한국어]
 * QUEUE_SYSFS_SHOW_CONST(_name, _val) - 항상 고정 상수 _val만 출력하는
 *   queue_<name>_show() 함수를 찍어내는 매크로 (하위 호환용 deprecated 필드)
 *
 * discard_zeroes_data/write_same_max/poll_delay는 과거 커널에서 실제 동작을
 * 제어했으나 현재는 의미가 사라져, 옛 사용자공간 도구가 이 파일을 읽어도
 * 깨지지 않도록 고정값(0 또는 -1)만 반환하는 호환성 스텁이다.
 *
 * 호출 체인(생성된 각 함수 공통):
 *   queue_attr_show → [queue_<name>_show] → sysfs_emit
 */
#define QUEUE_SYSFS_SHOW_CONST(_name, _val)				\
static ssize_t queue_##_name##_show(struct gendisk *disk, char *page)	\
{									\
	return sysfs_emit(page, "%d\n", _val);				\
}

/* 이하 3개는 더 이상 실질적 의미가 없는 deprecated sysfs 파일들의 고정 응답 정의. */
/* deprecated fields */
/* discard_zeroes_data: 과거 'discard 후 읽으면 0이 보장되는가' 플래그 - 현재 항상 0(비보장). */
QUEUE_SYSFS_SHOW_CONST(discard_zeroes_data, 0)
/* write_same_max: 과거 WRITE SAME(SCSI) 최대 크기 - 현재 항상 0(미지원 표시). */
QUEUE_SYSFS_SHOW_CONST(write_same_max, 0)
/* poll_delay: 과거 폴링 지연시간(us) 설정값 - 현재 항상 -1(미사용 표시). */
QUEUE_SYSFS_SHOW_CONST(poll_delay, -1)

/*
 * [한국어]
 * queue_max_discard_sectors_store - /sys/block/<disk>/queue/discard_max_bytes 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 discard 최대 바이트 값(십진 문자열).
 * @count: page 길이.
 * @lim: queue_limits_start_update()로 만든 임시 사본 - 이 함수는 여기에만 값을
 *   쓰고, 실제 반영은 호출자(queue_attr_store)의 commit 단계에서 이뤄진다.
 * @return: 성공 시 0, 실패 시 -EINVAL.
 *
 * 사용자는 discard_max_hw_bytes(하드웨어 최대치)를 넘지 않는 범위에서 실제
 * 적용할 max_user_discard_sectors를 줄일 수 있다. NVMe Deallocate(Trim)
 * 명령이 한 번에 처리할 range 크기를 제한하는 용도(추정).
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트, q->limits_lock 보유 상태로
 *   호출됨(queue_attr_store가 queue_limits_start_update()로 미리 잠금).
 * 호출자: queue_max_discard_sectors_entry.store_limit을 통해 queue_attr_store().
 * 피호출자: queue_var_store().
 * 에러 경로: 파싱 실패, discard_granularity 배수가 아님, UINT_MAX 초과 시 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store(limits_lock 보유) → [queue_max_discard_sectors_store]
 */
static int queue_max_discard_sectors_store(struct gendisk *disk,
		const char *page, size_t count, struct queue_limits *lim)
{
	/* 파싱된 사용자 지정 최대 discard 바이트 값을 담을 변수. */
	unsigned long max_discard_bytes;
	/* queue_var_store()의 반환값. */
	ssize_t ret;

	/* 입력 문자열을 max_discard_bytes로 파싱. */
	ret = queue_var_store(&max_discard_bytes, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/*
	 * discard_granularity(정렬 단위)의 배수가 아니면 NVMe Deallocate range 계산에 맞지
	 *   않으므로 거부 - '- 1'로 하위 비트 마스크를 만들어 정렬 여부를 검사(2의 거듭제곱 가정).
	 */
	if (max_discard_bytes & (disk->queue->limits.discard_granularity - 1))
		/* 정렬 위반 - 잘못된 값. */
		return -EINVAL;

	/* 섹터로 환산했을 때 unsigned int(UINT_MAX) 범위를 넘으면 하위 필드에 저장 불가. */
	if ((max_discard_bytes >> SECTOR_SHIFT) > UINT_MAX)
		/* 범위 초과 - 거부. */
		return -EINVAL;

	/*
	 * 검증을 통과한 값을 섹터 단위로 변환해 임시 limits 사본에 기록 - 아직 q->limits에는
	 *   반영되지 않은 상태(호출자가 commit_update_frozen을 호출해야 실제 반영).
	 */
	lim->max_user_discard_sectors = max_discard_bytes >> SECTOR_SHIFT;
	/* store_limit 콜백 규약: 성공 시 0 반환(count 처리 여부는 호출자가 판단). */
	return 0;
}

/*
 * [한국어]
 * queue_max_wzeroes_unmap_sectors_store - /sys/.../write_zeroes_unmap_max_bytes 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 write-zeroes-unmap 최대 바이트 값(십진 문자열) - 0(비활성) 또는
 *   하드웨어 최대치(max_hw_wzeroes_unmap_sectors)만 허용되는 이진 스위치.
 * @count: page 길이.
 * @lim: 임시 queue_limits 사본.
 * @return: 성공 시 0, 실패 시 -EINVAL.
 *
 * 다른 discard 계열과 달리 임의 값이 아니라 "0 아니면 하드웨어 최대치"만
 * 허용한다 - NVMe Write Zeroes 명령에 Deallocate 비트를 함께 세팅할지
 * 여부를 사실상 on/off로만 제어한다는 뜻(추정).
 * 실행 컨텍스트: sysfs write(2), q->limits_lock 보유 상태.
 * 호출자: queue_max_wzeroes_unmap_sectors_entry.store_limit을 통해 queue_attr_store().
 * 피호출자: queue_var_store().
 * 에러 경로: 파싱 실패, 0도 아니고 하드웨어 최대치도 아닌 값이면 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store(limits_lock 보유) → [queue_max_wzeroes_unmap_sectors_store]
 */
static int queue_max_wzeroes_unmap_sectors_store(struct gendisk *disk,
		const char *page, size_t count, struct queue_limits *lim)
{
	/* 사용자 입력값(max_zeroes_bytes)과 하드웨어 최대치를 바이트로 환산한 값(max_hw_zeroes_bytes). */
	unsigned long max_zeroes_bytes, max_hw_zeroes_bytes;
	/* queue_var_store()의 반환값. */
	ssize_t ret;

	/* 입력 문자열을 max_zeroes_bytes로 파싱. */
	ret = queue_var_store(&max_zeroes_bytes, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/* 임시 사본(lim)의 하드웨어 최대치(섹터)를 바이트로 환산해 비교 기준을 준비. */
	max_hw_zeroes_bytes = lim->max_hw_wzeroes_unmap_sectors << SECTOR_SHIFT;
	/* 0(끄기)도 아니고 하드웨어 최대치(켜기)도 아닌 임의값은 허용하지 않는 이진 스위치 검증. */
	if (max_zeroes_bytes != 0 && max_zeroes_bytes != max_hw_zeroes_bytes)
		/* 허용되지 않는 값 - 거부. */
		return -EINVAL;

	/* 검증된 바이트 값을 섹터로 환산해 사용자 지정 write-zeroes-unmap 상한에 기록. */
	lim->max_user_wzeroes_unmap_sectors = max_zeroes_bytes >> SECTOR_SHIFT;
	/* 성공(0) 반환. */
	return 0;
}

/*
 * [한국어]
 * queue_max_sectors_store - /sys/block/<disk>/queue/max_sectors_kb 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 max_sectors_kb 값(십진 문자열).
 * @count: page 길이.
 * @lim: 임시 queue_limits 사본.
 * @return: 성공 시 0, 실패 시 -EINVAL.
 *
 * max_sectors_kb는 NVMe가 한 request/명령으로 전송 가능한 최대 크기를
 * 사용자공간에서 하향 조정하는 통로다 - MDTS(하드웨어 상한)보다 작은 값으로
 * 설정하면 PRP/SGL 리스트를 더 짧게 준비해도 되지만, 큰 순차 I/O를 더 잘게
 * 쪼개야 하므로 처리량과 지연시간의 트레이드오프가 생긴다(추정). 값 자체의
 * 상한 검증(하드웨어 max_hw_sectors 초과 금지 등)은 이 함수가 아니라 이후
 * blk_validate_limits()(commit 단계)에서 수행된다.
 * 실행 컨텍스트: sysfs write(2), q->limits_lock 보유 상태.
 * 호출자: queue_max_sectors_entry.store_limit을 통해 queue_attr_store().
 * 피호출자: queue_var_store().
 * 에러 경로: 파싱 실패 시 -EINVAL(상한 초과 등은 이후 commit 단계에서 별도 검증).
 *
 * 호출 체인:
 *   queue_attr_store(limits_lock 보유) → [queue_max_sectors_store] →
 *     (이후 commit 단계에서 blk_validate_limits가 재검증)
 */
static int
queue_max_sectors_store(struct gendisk *disk, const char *page, size_t count,
		struct queue_limits *lim)
{
	/* 파싱된 KB 단위 최대 전송 크기. */
	unsigned long max_sectors_kb;
	/* queue_var_store()의 반환값. */
	ssize_t ret;

	/* 입력 문자열을 max_sectors_kb로 파싱. */
	ret = queue_var_store(&max_sectors_kb, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/*
	 * KB를 섹터(512B) 단위로 변환(왼쪽으로 1비트 시프트, 즉 *2)해 사용자 지정 최대치에 기록 -
	 *   최종 유효성 검증은 이후 commit 경로의 blk_validate_limits()가 담당.
	 */
	lim->max_user_sectors = max_sectors_kb << 1;
	/* 성공(0) 반환. */
	return 0;
}

/*
 * [한국어]
 * queue_feature_store - queue_limits.features 비트마스크의 단일 비트를 설정/해제
 *   하는 공용 헬퍼
 *
 * @disk: 대상 gendisk(이 함수에서는 직접 사용하지 않지만 다른 store_limit
 *   콜백들과 시그니처를 맞추기 위해 유지됨).
 * @page: "0" 또는 "1"(그 외 0이 아닌 값도 참으로 취급) 형태의 입력.
 * @count: page 길이.
 * @lim: 임시 queue_limits 사본 - features 필드를 여기에 갱신.
 * @feature: 설정/해제할 단일 blk_features_t 비트(BLK_FEAT_ROTATIONAL 등).
 * @return: 성공 시 0, 실패 시 -EINVAL.
 *
 * QUEUE_SYSFS_FEATURE 매크로(아래)가 생성하는 각 <name>_store()가 이 헬퍼에
 * 위임하는 방식으로, on/off 스위치형 feature 플래그(회전형 여부, add_random,
 * iostats, stable_writes 등)를 공통 로직 한 곳에서 처리한다.
 * 실행 컨텍스트: sysfs write(2), q->limits_lock 보유 상태.
 * 호출자: QUEUE_SYSFS_FEATURE가 생성한 queue_<name>_store() 함수들.
 * 피호출자: queue_var_store().
 * 에러 경로: 파싱 실패 시 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store(limits_lock 보유) → queue_<name>_store → [queue_feature_store]
 */
static ssize_t queue_feature_store(struct gendisk *disk, const char *page,
		size_t count, struct queue_limits *lim, blk_features_t feature)
{
	/* 파싱된 0/1(또는 그 외 값) 입력. */
	unsigned long val;
	/* queue_var_store()의 반환값. */
	ssize_t ret;

	/* 입력 문자열을 val로 파싱. */
	ret = queue_var_store(&val, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/* 0이 아니면(참이면) 해당 feature 비트를 켠다. */
	if (val)
		/* OR로 비트 설정. */
		lim->features |= feature;
	/* 0이면(거짓이면) 해당 feature 비트를 끈다. */
	else
		/* AND-NOT으로 비트 해제. */
		lim->features &= ~feature;
	/* 성공(0) 반환. */
	return 0;
}

/*
 * [한국어]
 * QUEUE_SYSFS_FEATURE(_name, _feature) - queue_limits.features의 한 비트를
 *   읽고 쓰는 show/store_limit 쌍을 함께 찍어내는 매크로
 *
 * show는 단순히 해당 비트가 켜져 있는지(!!)를 0/1로 출력하고, store는
 * 위 queue_feature_store()에 위임한다. QUEUE_LIM_RW_ENTRY와 짝을 이뤄
 * 읽기/쓰기 모두 가능한 feature 스위치를 만든다.
 *
 * 호출 체인(생성된 각 함수 공통):
 *   show:  queue_attr_show(limits_lock 보유) → [queue_<name>_show] → sysfs_emit
 *   store: queue_attr_store(limits_lock 보유) → [queue_<name>_store] →
 *            queue_feature_store
 */
#define QUEUE_SYSFS_FEATURE(_name, _feature)				\
static ssize_t queue_##_name##_show(struct gendisk *disk, char *page)	\
{									\
	return sysfs_emit(page, "%u\n",					\
		!!(disk->queue->limits.features & _feature));		\
}									\
static int queue_##_name##_store(struct gendisk *disk,			\
		const char *page, size_t count, struct queue_limits *lim) \
{									\
	return queue_feature_store(disk, page, count, lim, _feature);	\
}

/* rotational: BLK_FEAT_ROTATIONAL - 회전식(HDD류) 장치 여부. NVMe SSD는 보통 0(비회전). */
QUEUE_SYSFS_FEATURE(rotational, BLK_FEAT_ROTATIONAL)
/*
 * add_random: BLK_FEAT_ADD_RANDOM - 이 장치의 I/O 완료 타이밍을 커널 엔트로피 풀에
 *   기여할지 여부. NVMe는 타이밍이 예측 가능해 보통 끄는 것이 권장됨(추정).
 */
QUEUE_SYSFS_FEATURE(add_random, BLK_FEAT_ADD_RANDOM)
/* iostats: BLK_FEAT_IO_STAT - /proc/diskstats 등 I/O 통계 수집 활성화 여부. */
QUEUE_SYSFS_FEATURE(iostats, BLK_FEAT_IO_STAT)
/*
 * stable_writes: BLK_FEAT_STABLE_WRITES - 전송 중인 페이지 내용이 변경되지 않음을
 *   보장해야 하는지(DIF/DIX, 압축 등에서 필요) 여부; 세미콜론이 붙어 있으나 매크로
 *   전개 결과(선언들) 뒤에 오는 빈 문장으로 컴파일에 영향 없음.
 */
QUEUE_SYSFS_FEATURE(stable_writes, BLK_FEAT_STABLE_WRITES);

/*
 * [한국어]
 * QUEUE_SYSFS_FEATURE_SHOW(_name, _feature) - features 비트를 읽기 전용으로만
 *   노출하는 show 함수를 찍어내는 매크로(store 없음)
 *
 * 위 QUEUE_SYSFS_FEATURE와 달리 store를 생성하지 않아 QUEUE_LIM_RO_ENTRY와
 * 짝을 이룬다 - fua/dax처럼 드라이버가 결정하고 사용자가 바꿀 수 없는
 * feature 비트에 사용된다.
 *
 * 호출 체인(생성된 각 함수 공통):
 *   queue_attr_show(limits_lock 보유) → [queue_<name>_show] → sysfs_emit
 */
#define QUEUE_SYSFS_FEATURE_SHOW(_name, _feature)			\
static ssize_t queue_##_name##_show(struct gendisk *disk, char *page)	\
{									\
	return sysfs_emit(page, "%u\n",					\
		!!(disk->queue->limits.features & _feature));		\
}

/*
 * fua: BLK_FEAT_FUA - NVMe FUA(Force Unit Access) 비트를 이 큐가 지원하는지 여부
 *   (읽기 전용 - 드라이버가 하드웨어 capability로 결정).
 */
QUEUE_SYSFS_FEATURE_SHOW(fua, BLK_FEAT_FUA);
/* dax: BLK_FEAT_DAX - Direct Access(페이지 캐시 우회 memory-mapped I/O) 지원 여부. */
QUEUE_SYSFS_FEATURE_SHOW(dax, BLK_FEAT_DAX);

/*
 * [한국어]
 * queue_poll_show - /sys/block/<disk>/queue/io_poll 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: sysfs_emit()의 반환값.
 *
 * blk-mq 큐라면 blk_mq_can_poll()(하드웨어 poll queue 존재 여부 등을 종합
 * 판단)을 쓰고, 그렇지 않은(legacy bio 기반) 큐라면 queue_limits의
 * BLK_FEAT_POLL 비트만 본다. NVMe에서 polling은 인터럽트 없이 CQ를
 * 직접 스핀 대기로 확인하는 완료 모델로, 매우 짧은 지연시간이 필요한
 * 워크로드에서 사용된다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * 호출자: queue_poll_entry.show를 통해 queue_attr_show().
 * 피호출자: queue_is_mq(), blk_mq_can_poll(), sysfs_emit().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_poll_show] → blk_mq_can_poll / sysfs_emit
 */
static ssize_t queue_poll_show(struct gendisk *disk, char *page)
{
	/* blk-mq(멀티큐) 기반 큐인지 확인 - NVMe는 항상 이 경로. */
	if (queue_is_mq(disk->queue))
		/* blk_mq_can_poll(): 하드웨어 poll queue 존재 및 관련 조건을 종합 판단한 결과를 출력. */
		return sysfs_emit(page, "%u\n", blk_mq_can_poll(disk->queue));

	/* legacy(bio 기반) 큐는 queue_limits의 BLK_FEAT_POLL 비트만으로 판단. */
	return sysfs_emit(page, "%u\n",
			!!(disk->queue->limits.features & BLK_FEAT_POLL));
}

/*
 * [한국어]
 * queue_zoned_show - /sys/block/<disk>/queue/zoned 읽기
 *   (아래 QUEUE_LIM_RO_ENTRY(queue_zoned, "zoned")가 이 함수를 .show_limit으로
 *   등록하므로 호출 시 limits_lock이 보유된 상태다)
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: sysfs_emit()의 반환값 - "host-managed" 또는 "none".
 *
 * NVMe ZNS(Zoned Namespace) 장치는 "host-managed"로, 일반 NVMe 네임스페이스는
 * "none"으로 보고된다. SCSI ZBC의 "host-aware"는 블록 레이어가 더 이상
 * 구분하지 않으므로(구버전 대비 단순화) 이 파일에는 두 값만 존재한다.
 * 실행 컨텍스트: sysfs read(2), limits_lock 보유 상태.
 * 호출자: queue_zoned_entry.show_limit을 통해 queue_attr_show().
 * 피호출자: blk_queue_is_zoned(), sysfs_emit().
 *
 * 호출 체인:
 *   queue_attr_show(limits_lock 보유) → [queue_zoned_show] → sysfs_emit
 */
static ssize_t queue_zoned_show(struct gendisk *disk, char *page)
{
	/* ZNS 등 zoned 장치인지 판별. */
	if (blk_queue_is_zoned(disk->queue))
		/* zoned 장치 - "host-managed" 보고. */
		return sysfs_emit(page, "host-managed\n");
	/* zoned이 아니면 "none" 보고. */
	return sysfs_emit(page, "none\n");
}

/*
 * [한국어]
 * queue_nr_zones_show - /sys/block/<disk>/queue/nr_zones 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: queue_var_show()의 반환값.
 *
 * NVMe ZNS 네임스페이스의 전체 zone 개수를 보고한다 - Identify Namespace
 * (CNS 05h, ZNS Command Set) 등에서 계산된 값을 disk 메타데이터에서 조회.
 * 실행 컨텍스트: sysfs read(2), QUEUE_RO_ENTRY로 등록되어 별도 락 없이 호출됨.
 * 호출자: queue_nr_zones_entry.show를 통해 queue_attr_show().
 * 피호출자: disk_nr_zones(), queue_var_show().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_nr_zones_show] → disk_nr_zones → queue_var_show
 */
static ssize_t queue_nr_zones_show(struct gendisk *disk, char *page)
{
	/* disk_nr_zones(): gendisk에 저장된 zone 개수(zoned이 아니면 0)를 조회해 출력. */
	return queue_var_show(disk_nr_zones(disk), page);
}

/*
 * [한국어]
 * queue_zoned_qd1_writes_show - /sys/block/<disk>/queue/zoned_qd1_writes 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: queue_var_show()의 반환값(0 또는 1).
 *
 * QUEUE_FLAG_ZONED_QD1_WRITES 큐 플래그의 현재 상태를 보고한다. 이 플래그가
 * 켜지면 zone에 대한 쓰기를 큐 깊이(QD) 1로 강제해, 여러 쓰기가 동시에
 * 재정렬되어 ZNS의 "zone 내부는 순차 쓰기만 허용"이라는 하드웨어 규칙을
 * 어기는 사고를 방지한다(추정).
 * 실행 컨텍스트: sysfs read(2), QUEUE_RW_ENTRY로 등록되어 별도 락 없이 호출됨
 *   (queue_flags는 비트 단위 원자적 접근이 보장되는 test_bit류 API로 다뤄짐, 추정).
 * 호출자: queue_zoned_qd1_writes_entry.show를 통해 queue_attr_show().
 * 피호출자: blk_queue_zoned_qd1_writes(), queue_var_show().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_zoned_qd1_writes_show] → queue_var_show
 */
static ssize_t queue_zoned_qd1_writes_show(struct gendisk *disk, char *page)
{
	/*
	 * blk_queue_zoned_qd1_writes(): QUEUE_FLAG_ZONED_QD1_WRITES 플래그 상태를 bool로
	 *   변환(!!)해 0/1로 출력.
	 */
	return queue_var_show(!!blk_queue_zoned_qd1_writes(disk->queue),
			      page);
}

/*
 * [한국어]
 * queue_zoned_qd1_writes_store - /sys/block/<disk>/queue/zoned_qd1_writes 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 값(0 또는 0이 아닌 값 - QD1 강제 여부).
 * @count: page 길이.
 * @return: 성공 시 count, 실패 시 -EINVAL.
 *
 * 이 플래그를 바꾸는 동안 진행 중인 zone 쓰기와 새 정책이 뒤섞이면 안 되므로
 * freeze(신규 진입 차단) 후 quiesce(기존 요청 배출/일시정지)까지 수행한 뒤
 * 플래그를 갱신하고 다시 unquiesce/unfreeze한다 - 일반 큐 플래그 변경보다
 * 한 단계 더 강한 동기화를 적용하는 셈이다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * 호출자: queue_zoned_qd1_writes_entry.store를 통해 queue_attr_store().
 * 피호출자: queue_var_store(), blk_mq_freeze_queue(), blk_mq_quiesce_queue(),
 *   blk_queue_flag_set()/clear(), blk_mq_unquiesce_queue(), blk_mq_unfreeze_queue().
 * 에러 경로: 파싱 실패 시 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_zoned_qd1_writes_store] → blk_mq_freeze_queue →
 *     blk_mq_quiesce_queue → (flag 갱신) → blk_mq_unquiesce_queue →
 *     blk_mq_unfreeze_queue
 */
static ssize_t queue_zoned_qd1_writes_store(struct gendisk *disk,
					    const char *page, size_t count)
{
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;
	/* 파싱된 0/1(또는 그 외) 입력값. */
	unsigned long qd1_writes;
	/* blk_mq_freeze_queue()가 반환하는 GFP 플래그 스냅샷. */
	unsigned int memflags;
	/* 반환값(성공 시 count, 실패 시 -EINVAL). */
	ssize_t ret;

	/* 입력 문자열을 qd1_writes로 파싱. */
	ret = queue_var_store(&qd1_writes, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/* 큐 freeze: 새 zone 쓰기가 SQ에 들어오지 못하게 차단. */
	memflags = blk_mq_freeze_queue(q);
	/*
	 * quiesce: 이미 디스패치된(진행 중인) 요청까지 조용히 배출/정지시켜 플래그 전환
	 *   시점에 QD1 정책과 기존 요청이 섞이지 않도록 함.
	 */
	blk_mq_quiesce_queue(q);
	/* 0이 아니면(참이면) QD1 강제 정책을 켠다. */
	if (qd1_writes)
		/* 플래그 설정. */
		blk_queue_flag_set(QUEUE_FLAG_ZONED_QD1_WRITES, q);
	/* 0이면 QD1 강제 정책을 끈다. */
	else
		/* 플래그 해제. */
		blk_queue_flag_clear(QUEUE_FLAG_ZONED_QD1_WRITES, q);
	/* quiesce 해제 - 큐가 다시 요청을 디스패치할 수 있게 됨. */
	blk_mq_unquiesce_queue(q);
	/* freeze 해제 - NVMe I/O 재개. */
	blk_mq_unfreeze_queue(q, memflags);

	/* 성공 시 처리 바이트 수(count) 반환. */
	return count;
}

/*
 * [한국어]
 * queue_iostats_passthrough_show - /sys/block/<disk>/queue/iostats_passthrough 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: queue_var_show()의 반환값(0 또는 1).
 *
 * NVMe passthrough(관리자 명령을 직접 전송하는 ioctl 등) I/O도 일반 I/O
 * 통계(/proc/diskstats 등)에 포함시킬지 여부를 보고한다(추정).
 * 실행 컨텍스트: sysfs read(2), QUEUE_LIM_RO_ENTRY로 등록되어 limits_lock
 *   보유 상태로 호출됨.
 * 호출자: queue_iostats_passthrough_entry.show_limit을 통해 queue_attr_show().
 * 피호출자: blk_queue_passthrough_stat(), queue_var_show().
 *
 * 호출 체인:
 *   queue_attr_show(limits_lock 보유) → [queue_iostats_passthrough_show] →
 *     queue_var_show
 */
static ssize_t queue_iostats_passthrough_show(struct gendisk *disk, char *page)
{
	/* blk_queue_passthrough_stat(): BLK_FLAG_IOSTATS_PASSTHROUGH 플래그 상태를 0/1로 출력. */
	return queue_var_show(!!blk_queue_passthrough_stat(disk->queue), page);
}

/*
 * [한국어]
 * queue_iostats_passthrough_store - /sys/block/<disk>/queue/iostats_passthrough 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 값(0 또는 1).
 * @count: page 길이.
 * @lim: 임시 queue_limits 사본.
 * @return: 성공 시 0, 실패 시 -EINVAL.
 *
 * queue_feature_store()와 유사한 형태지만 대상 필드가 features가 아니라
 * lim->flags(BLK_FLAG_IOSTATS_PASSTHROUGH)라는 점이 다르다.
 * 실행 컨텍스트: sysfs write(2), q->limits_lock 보유 상태.
 * 호출자: queue_iostats_passthrough_entry.store_limit을 통해 queue_attr_store().
 * 피호출자: queue_var_store().
 * 에러 경로: 파싱 실패 시 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store(limits_lock 보유) → [queue_iostats_passthrough_store]
 */
static int queue_iostats_passthrough_store(struct gendisk *disk,
		const char *page, size_t count, struct queue_limits *lim)
{
	/* 파싱된 0/1 입력값. */
	unsigned long ios;
	/* queue_var_store()의 반환값. */
	ssize_t ret;

	/* 입력 문자열을 ios로 파싱. */
	ret = queue_var_store(&ios, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/* 0이 아니면(참이면) passthrough I/O 통계 수집을 켠다. */
	if (ios)
		/* 플래그 설정. */
		lim->flags |= BLK_FLAG_IOSTATS_PASSTHROUGH;
	/* 0이면 끈다. */
	else
		/* 플래그 해제. */
		lim->flags &= ~BLK_FLAG_IOSTATS_PASSTHROUGH;
	/* 성공(0) 반환. */
	return 0;
}

/*
 * [한국어]
 * queue_nomerges_show - /sys/block/<disk>/queue/nomerges 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: queue_var_show()의 반환값(0/1/2).
 *
 * QUEUE_FLAG_NOMERGES와 QUEUE_FLAG_NOXMERGES 두 비트를 조합해 0(모든 병합
 * 허용)/1(간단한 일방향 병합만 금지)/2(모든 병합 금지) 세 상태로 인코딩한다.
 * NVMe에서 병합을 억제하면 SQ 엔트리당 더 단순한 단일 bio 구조를 유지할 수
 * 있지만 대신 더 많은 명령을 제출해야 한다(추정).
 * 실행 컨텍스트: sysfs read(2), QUEUE_RW_ENTRY로 등록되어 별도 락 없이 호출됨.
 * 호출자: queue_nomerges_entry.show를 통해 queue_attr_show().
 * 피호출자: blk_queue_nomerges(), blk_queue_noxmerges(), queue_var_show().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_nomerges_show] → queue_var_show
 */
static ssize_t queue_nomerges_show(struct gendisk *disk, char *page)
{
	/*
	 * nomerges(전체 병합 금지) 여부를 상위 비트(<<1)로, noxmerges(교차 병합만 금지)
	 *   여부를 하위 비트로 조합 - 결과값 0/1/2가 store에서 그대로 재해석된다.
	 */
	return queue_var_show((blk_queue_nomerges(disk->queue) << 1) |
	/* OR로 두 비트를 합쳐 queue_var_show에 전달. */
			       blk_queue_noxmerges(disk->queue), page);
}

/*
 * [한국어]
 * queue_nomerges_store - /sys/block/<disk>/queue/nomerges 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 값(0/1/2).
 * @count: page 길이.
 * @return: queue_var_store()의 반환값 그대로(성공 시 count, 실패 시 -EINVAL).
 *
 * 값 2는 모든 병합(단순/복잡 모두)을 금지하는 QUEUE_FLAG_NOMERGES를, 값
 * 1(0이 아닌 다른 값 포함)은 상대적으로 가벼운 QUEUE_FLAG_NOXMERGES만
 * 설정한다 - 항상 먼저 두 플래그를 모두 지운 뒤 필요한 플래그만 다시 켜는
 * "지우고 다시 설정" 패턴을 쓴다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트. 큐 플래그는 원자적
 *   비트연산(blk_queue_flag_*)으로 개별 접근되므로 별도 락이 필요 없다.
 * 호출자: queue_nomerges_entry.store를 통해 queue_attr_store().
 * 피호출자: queue_var_store(), blk_queue_flag_clear()/set().
 * 에러 경로: 파싱 실패 시 -EINVAL(플래그 자체는 항상 유효한 조합으로 정리됨).
 *
 * 호출 체인:
 *   queue_attr_store → [queue_nomerges_store] → queue_var_store
 */
static ssize_t queue_nomerges_store(struct gendisk *disk, const char *page,
				    size_t count)
{
	/* 파싱된 nomerges 값(0/1/2)을 담을 변수. */
	unsigned long nm;
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;
	/* 입력 문자열을 nm으로 파싱하며 동시에 반환값을 초기화. */
	ssize_t ret = queue_var_store(&nm, page, count);

	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/*
	 * 먼저 두 병합 억제 플래그를 모두 지워 알려진 상태에서 시작 - 이전 설정이 남아
	 *   혼재되는 것을 방지.
	 */
	blk_queue_flag_clear(QUEUE_FLAG_NOMERGES, q);
	/* NOXMERGES 플래그도 함께 초기화. */
	blk_queue_flag_clear(QUEUE_FLAG_NOXMERGES, q);
	/* 값이 2면 모든 병합(단순+복잡) 금지. */
	if (nm == 2)
		/* NOMERGES 플래그 설정. */
		blk_queue_flag_set(QUEUE_FLAG_NOMERGES, q);
	/* 0이 아닌 다른 값(사실상 1)이면 가벼운 교차 병합만 금지. */
	else if (nm)
		/* NOXMERGES 플래그 설정. */
		blk_queue_flag_set(QUEUE_FLAG_NOXMERGES, q);

	/* queue_var_store()가 남긴 처리 바이트 수(또는 에러)를 그대로 반환. */
	return ret;
}

/*
 * [한국어]
 * queue_rq_affinity_show - /sys/block/<disk>/queue/rq_affinity 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼.
 * @return: queue_var_show()의 반환값(0/1/2).
 *
 * QUEUE_FLAG_SAME_COMP(완료를 요청 CPU에서 처리하도록 시도)와
 * QUEUE_FLAG_SAME_FORCE(그 정책을 강제)의 조합을 인코딩한다. NVMe 관점에서는
 * 인터럽트(CQ 처리)가 어느 CPU에서 실행될지를 요청 제출 CPU와 맞출지 여부를
 * 나타내는 캐시 지역성/지연시간 튜닝 값이다.
 * 실행 컨텍스트: sysfs read(2), QUEUE_RW_ENTRY로 등록되어 별도 락 없이 호출됨
 *   (test_bit()로 개별 원자적 접근).
 * 호출자: queue_rq_affinity_entry.show를 통해 queue_attr_show().
 * 피호출자: test_bit(), queue_var_show().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_rq_affinity_show] → queue_var_show
 */
static ssize_t queue_rq_affinity_show(struct gendisk *disk, char *page)
{
	/* 완료 처리를 요청 제출 CPU와 같게 시도하는 정책(SAME_COMP)이 켜져 있는지 확인. */
	bool set = test_bit(QUEUE_FLAG_SAME_COMP, &disk->queue->queue_flags);
	/* 그 정책을 강제(FORCE, 다른 CPU로의 완료도 강제로 재배치)하는지 확인. */
	bool force = test_bit(QUEUE_FLAG_SAME_FORCE, &disk->queue->queue_flags);

	/*
	 * set << force: force가 1이면 값 2(강제), force가 0이고 set이 1이면 값 1(권장),
	 *   둘 다 0이면 0(정책 없음)으로 인코딩되어 출력.
	 */
	return queue_var_show(set << force, page);
}

/*
 * [한국어]
 * queue_rq_affinity_store - /sys/block/<disk>/queue/rq_affinity 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 값(0/1/2).
 * @count: page 길이.
 * @return: 성공 시 queue_var_store()가 남긴 count, 실패 시 -EINVAL. CONFIG_SMP가
 *   꺼진(단일 CPU) 빌드에서는 항상 -EINVAL(이 개념 자체가 무의미하므로).
 *
 * 완료 인터럽트(NVMe CQ 처리)를 요청 제출 CPU와 맞출지 결정하는 정책을
 * 갱신한다. 두 플래그(SAME_COMP/SAME_FORCE)를 원자적 비트연산으로 개별
 * 설정하므로 두 연산 사이에 잠깐 불일치가 있을 수 있으나(원본 영어 주석
 * 참고), 어차피 각 플래그는 test_bit()으로 개별 접근되어 harmless하다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트. CONFIG_SMP 빌드에서만
 *   실제 동작하며, UP(단일 CPU) 빌드에서는 이 개념이 무의미해 항상 실패.
 * 호출자: queue_rq_affinity_entry.store를 통해 queue_attr_store().
 * 피호출자: queue_var_store(), blk_queue_flag_set()/clear().
 * 에러 경로: 파싱 실패 시 -EINVAL. UP 빌드에서는 ret 초기값 -EINVAL이 그대로 반환.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_rq_affinity_store] → blk_queue_flag_set/clear
 */
static ssize_t
queue_rq_affinity_store(struct gendisk *disk, const char *page, size_t count)
{
	/*
	 * 기본값 -EINVAL - CONFIG_SMP가 꺼져 있으면 아래 블록이 통째로 컴파일 제외되어
	 *   이 값이 그대로 반환된다(단일 CPU에서는 affinity 개념이 무의미하므로 실패 처리).
	 */
	ssize_t ret = -EINVAL;
/*
 * CONFIG_SMP(멀티프로세서 빌드)에서만 실제 로직 컴파일 - 단일 CPU 커널은
 *   아래 전체를 건너뛰고 위에서 설정한 -EINVAL을 반환.
 */
#ifdef CONFIG_SMP
	/* 이후 반복 참조할 request_queue 캐시(SMP 빌드에서만 존재하는 지역 변수). */
	struct request_queue *q = disk->queue;
	/* 파싱된 값(0/1/2)을 담을 변수. */
	unsigned long val;

	/* 입력 문자열을 val로 파싱. */
	ret = queue_var_store(&val, page, count);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;

	/*
	 * 원본 영어 주석: 아래에서 두 큐 플래그를 각각 원자적 비트연산으로 갱신하는데,
	 *   두 번의 갱신 자체는 하나의 원자적 트랜잭션이 아니지만 각 플래그가 test_bit()으로
	 *   개별 접근되는 것을 감안하면 무해(harmless)하다 - 그래서 이 함수는 별도 락을
	 *   잡지 않고 flag_set/flag_clear만 순서대로 호출한다.
	 */
	/*
	 * Here we update two queue flags each using atomic bitops, although
	 * updating two flags isn't atomic it should be harmless as those flags
	 * are accessed individually using atomic test_bit operation. So we
	 * don't grab any lock while updating these flags.
	 */
	/*
	 * 값 2: 완료 처리를 요청 CPU와 반드시 일치시키도록 강제(SAME_COMP+SAME_FORCE 모두 설정) -
	 *   NVMe CQ 인터럽트 핸들러가 제출 CPU와 같은 코어에서 실행되도록 강제(추정).
	 */
	if (val == 2) {
		/* 완료-요청 CPU 일치 정책 켬. */
		blk_queue_flag_set(QUEUE_FLAG_SAME_COMP, q);
		/* 강제 옵션도 켬. */
		blk_queue_flag_set(QUEUE_FLAG_SAME_FORCE, q);
	/* 값 1: 완료-요청 CPU 일치를 '권장'만 하고 강제하지는 않음. */
	} else if (val == 1) {
		/* 일치 정책 켬. */
		blk_queue_flag_set(QUEUE_FLAG_SAME_COMP, q);
		/* 강제 옵션은 끔 - 스케줄러 판단에 여지를 둠. */
		blk_queue_flag_clear(QUEUE_FLAG_SAME_FORCE, q);
	/* 값 0: 어떤 affinity 정책도 적용하지 않음(기본 스케줄링에 맡김). */
	} else if (val == 0) {
		/* 일치 정책 끔. */
		blk_queue_flag_clear(QUEUE_FLAG_SAME_COMP, q);
		/* 강제 옵션도 끔. */
		blk_queue_flag_clear(QUEUE_FLAG_SAME_FORCE, q);
	}
/* CONFIG_SMP 블록 종료. */
#endif
	/* SMP 빌드면 위에서 설정된 ret(queue_var_store 결과), UP 빌드면 초기값 -EINVAL을 반환. */
	return ret;
}

/*
 * [한국어]
 * queue_poll_delay_store - /sys/block/<disk>/queue/io_poll_delay 쓰기 (호환용 no-op)
 *
 * @disk: 대상 gendisk(사용하지 않음).
 * @page: 사용자 입력(내용을 검사하지 않음).
 * @count: page 길이.
 * @return: 항상 count(써준 값을 그대로 받아들인 것처럼 응답).
 *
 * poll_delay 개념 자체가 폐기되어(위 queue_poll_delay_show가 항상 -1을
 * 반환하는 것과 짝) 쓰기도 아무 효과 없이 성공한 것처럼만 응답하는
 * 하위 호환 스텁이다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * 호출자: queue_poll_delay_entry.store를 통해 queue_attr_store().
 * 피호출자: 없음.
 * 에러 경로: 없음(항상 성공).
 *
 * 호출 체인:
 *   queue_attr_store → [queue_poll_delay_store] → (즉시 반환)
 */
static ssize_t queue_poll_delay_store(struct gendisk *disk, const char *page,
				size_t count)
{
	/* 아무 처리도 하지 않고 count를 그대로 반환해 '쓰기 성공'으로만 흉내낸다. */
	return count;
}

/*
 * [한국어]
 * queue_poll_store - /sys/block/<disk>/queue/io_poll 쓰기 (실효 없는 안내용)
 *
 * @disk: 대상 gendisk.
 * @page: 사용자 입력(내용을 검사하지 않음).
 * @count: page 길이.
 * @return: 큐가 poll을 지원하면 count(성공 흉내), 지원하지 않으면 -EINVAL.
 *
 * 실제 polling 활성화 여부는 드라이버(예: NVMe의 poll queue 개수 모듈
 * 파라미터)가 결정하며, 이 sysfs 파일을 통한 값 변경은 아무 효과가 없다 -
 * 다만 사용자가 실수로 값을 쓰면 안내 메시지를 남긴다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * 호출자: queue_poll_entry.store를 통해 queue_attr_store().
 * 피호출자: pr_info_ratelimited().
 * 에러 경로: BLK_FEAT_POLL 미지원 큐에서는 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_poll_store] → pr_info_ratelimited
 */
static ssize_t queue_poll_store(struct gendisk *disk, const char *page,
				size_t count)
{
	/* 기본 반환값을 count(성공 흉내)로 미리 설정. */
	ssize_t ret = count;
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;

	/* 이 큐가 애초에 polling을 지원하지 않으면 값을 쓰는 것 자체가 무의미. */
	if (!(q->limits.features & BLK_FEAT_POLL)) {
		/* 미지원 큐 - 에러로 표시. */
		ret = -EINVAL;
		/* out 레이블로 점프. */
		goto out;
	}

	/* polling 관련 sysfs 쓰기는 무시된다는 사실을 관리자에게 안내(rate limited로 로그 폭주 방지). */
	pr_info_ratelimited("writes to the poll attribute are ignored.\n");
	/* 드라이버별 모듈 파라미터를 대신 쓰라는 안내 메시지. */
	pr_info_ratelimited("please use driver specific parameters instead.\n");
/* out: 정상/에러 두 경로가 합류하는 지점. */
out:
	/* ret(성공 시 count, 실패 시 -EINVAL)을 반환. */
	return ret;
}

/*
 * [한국어]
 * queue_io_timeout_show - /sys/block/<disk>/queue/io_timeout 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼(밀리초 단위 정수 문자열).
 * @return: sysfs_emit()의 반환값.
 *
 * q->rq_timeout(jiffies 단위)을 밀리초로 환산해 보고한다. NVMe 명령이 이
 * 시간 안에 완료되지 않으면 blk-mq 워치독 타이머가 nvme_timeout()을 호출해
 * abort/컨트롤러 reset 복구 경로로 들어간다.
 * 실행 컨텍스트: sysfs read(2), QUEUE_RW_ENTRY로 등록되어 별도 락 없이 호출됨
 *   (rq_timeout은 READ_ONCE로 읽어 다른 CPU의 WRITE_ONCE 갱신과 데이터 경합 방지).
 * 호출자: queue_io_timeout_entry.show를 통해 queue_attr_show().
 * 피호출자: READ_ONCE(), jiffies_to_msecs(), sysfs_emit().
 *
 * 호출 체인:
 *   queue_attr_show → [queue_io_timeout_show] → sysfs_emit
 */
static ssize_t queue_io_timeout_show(struct gendisk *disk, char *page)
{
	/* sysfs_emit "%u\n" 포맷 시작. */
	return sysfs_emit(page, "%u\n",
	/*
	 * READ_ONCE로 rq_timeout(jiffies)을 읽어 다른 CPU의 동시 쓰기와의 데이터 경합을
	 *   피하고, jiffies_to_msecs()로 밀리초 단위로 환산해 출력.
	 */
			jiffies_to_msecs(READ_ONCE(disk->queue->rq_timeout)));
}

/*
 * [한국어]
 * queue_io_timeout_store - /sys/block/<disk>/queue/io_timeout 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 타임아웃 값(밀리초 단위 십진 문자열).
 * @count: page 길이.
 * @return: 성공 시 count, 실패 시 -EINVAL.
 *
 * 사용자가 밀리초 단위로 지정한 값을 jiffies로 환산해 blk_queue_rq_timeout()
 * (block/blk-settings.c)에 위임한다. 0ms는 "타임아웃 없음"으로 오인될 수
 * 있어 명시적으로 거부한다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * 호출자: queue_io_timeout_entry.store를 통해 queue_attr_store().
 * 피호출자: kstrtou32(), msecs_to_jiffies(), blk_queue_rq_timeout().
 * 에러 경로: 파싱 실패 또는 val==0이면 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_io_timeout_store] → blk_queue_rq_timeout
 */
static ssize_t queue_io_timeout_store(struct gendisk *disk, const char *page,
				  size_t count)
{
	/* 파싱된 밀리초 단위 타임아웃 값. */
	unsigned int val;
	/* kstrtou32()의 반환 에러 코드. */
	int err;
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;

	/* 10진 문자열을 32비트 부호 없는 정수(val)로 파싱. */
	err = kstrtou32(page, 10, &val);
	/* 파싱 실패 또는 0ms(타임아웃 비활성으로 오인될 수 있는 값)는 거부. */
	if (err || val == 0)
		/* 잘못된 값 - 에러 반환. */
		return -EINVAL;

	/*
	 * 밀리초를 jiffies로 환산해 큐 전체 타임아웃을 갱신 - 이후 NVMe 명령이 이 시간을
	 *   넘기면 nvme_timeout → nvme_abort_req → doorbell 경로로 복구가 시작된다(추정).
	 */
	blk_queue_rq_timeout(q, msecs_to_jiffies(val));

	/* 성공 시 처리 바이트 수(count) 반환. */
	return count;
}

/*
 * [한국어]
 * queue_wc_show - /sys/block/<disk>/queue/write_cache 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼("write back\n" 또는 "write through\n").
 * @return: sysfs_emit()의 반환값.
 *
 * NVMe VWC(Volatile Write Cache) feature 비트를 사람이 읽기 쉬운 문자열로
 * 변환한다. write back이면 컨트롤러가 캐시에 쓴 뒤 즉시 완료를 보고할 수
 * 있고(FLUSH/FUA로 영속성을 보장해야 함), write through면 매 쓰기가 곧바로
 * 비휘발성 매체까지 도달함을 의미한다(추정).
 * 실행 컨텍스트: sysfs read(2), QUEUE_LIM_RW_ENTRY로 등록되어 limits_lock 보유 상태.
 * 호출자: queue_wc_entry.show_limit을 통해 queue_attr_show().
 * 피호출자: blk_queue_write_cache(), sysfs_emit().
 *
 * 호출 체인:
 *   queue_attr_show(limits_lock 보유) → [queue_wc_show] → sysfs_emit
 */
static ssize_t queue_wc_show(struct gendisk *disk, char *page)
{
	/*
	 * blk_queue_write_cache(): BLK_FLAG_WRITE_CACHE_DISABLED 비트가 꺼져 있으면(캐시
	 *   활성) 참 - NVMe VWC가 켜져 있다는 뜻.
	 */
	if (blk_queue_write_cache(disk->queue))
		/* write back(캐시 활성) 응답. */
		return sysfs_emit(page, "write back\n");
	/* 캐시가 비활성화되어 있으면 write through 응답. */
	return sysfs_emit(page, "write through\n");
}

/*
 * [한국어]
 * queue_wc_store - /sys/block/<disk>/queue/write_cache 쓰기
 *
 * @disk: 대상 gendisk(이 함수에서는 직접 사용하지 않음).
 * @page: "write back", "write through", "none" 중 하나의 문자열.
 * @count: page 길이.
 * @lim: 임시 queue_limits 사본 - flags 필드를 갱신.
 * @return: 성공 시 0, 실패 시 -EINVAL.
 *
 * "write back"만 캐시를 켜는 것으로 처리하고, "write through"/"none"은
 * 둘 다 캐시를 끄는 것으로 동일하게 처리한다(별칭). 그 외 문자열은 거부.
 * NVMe에서 캐시를 끄면(BLK_FLAG_WRITE_CACHE_DISABLED 설정) FLUSH/FUA 처리
 * 로직이 더 이상 컨트롤러 VWC 플러시를 요구하지 않게 될 수 있다(추정).
 * 실행 컨텍스트: sysfs write(2), q->limits_lock 보유 상태.
 * 호출자: queue_wc_entry.store_limit을 통해 queue_attr_store().
 * 피호출자: strncmp().
 * 에러 경로: 세 문자열 중 어느 것도 매치하지 않으면 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_store(limits_lock 보유) → [queue_wc_store]
 */
static int queue_wc_store(struct gendisk *disk, const char *page,
		size_t count, struct queue_limits *lim)
{
	/* 캐시를 비활성화할지 여부를 담을 bool. */
	bool disable;

	/* "write back"(정확히 10글자까지 비교) - 캐시를 켜는 것으로 처리. */
	if (!strncmp(page, "write back", 10)) {
		/* disable=false: 캐시 활성 상태로 설정 예정. */
		disable = false;
	/* "write through"(13글자) 또는 */
	} else if (!strncmp(page, "write through", 13) ||
		/* "none"(4글자) - 둘 다 캐시를 끄는 별칭으로 처리. */
		   !strncmp(page, "none", 4)) {
		/* disable=true: 캐시 비활성 상태로 설정 예정. */
		disable = true;
	/* 위 세 문자열 어느 것과도 일치하지 않는 입력. */
	} else {
		/* 알 수 없는 값 - 거부. */
		return -EINVAL;
	}

	/* disable이 참이면 캐시 비활성화 플래그를 켠다. */
	if (disable)
		/* BLK_FLAG_WRITE_CACHE_DISABLED 설정 - flush/FUA 처리 로직에 영향. */
		lim->flags |= BLK_FLAG_WRITE_CACHE_DISABLED;
	/* disable이 거짓이면 캐시를 활성 상태로 유지. */
	else
		/* 플래그 해제. */
		lim->flags &= ~BLK_FLAG_WRITE_CACHE_DISABLED;
	/* 성공(0) 반환. */
	return 0;
}

/*
 * [한국어]
 * QUEUE_RO_ENTRY(_prefix, _name) - 읽기 전용, 자체 락 기반(show만 있는) 정적
 *   struct queue_sysfs_entry를 찍어내는 매크로
 *
 * <_prefix>_entry라는 이름의 정적 상수 구조체를 만들며, sysfs 파일명은
 * _name, 모드는 0444(root 포함 모두 읽기 전용), .show는 <_prefix>_show로
 * 연결한다. .store/.show_limit/.store_limit은 이 매크로가 만드는 초기화식에
 * 나타나지 않으므로 C 구조체 초기화 규칙에 따라 자동으로 NULL이 된다.
 *
 * 호출 체인: 이 매크로가 만드는 entry는 queue_attrs[]/blk_mq_queue_attrs[]
 *   배열에 담겨 attribute_group을 통해 kobject core에 등록된다.
 */
#define QUEUE_RO_ENTRY(_prefix, _name)				\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr	= { .name = _name, .mode = 0444 },		\
	.show	= _prefix##_show,				\
};

/*
 * [한국어]
 * QUEUE_RW_ENTRY(_prefix, _name) - 읽기/쓰기, 자체 락 기반(show+store) 정적
 *   struct queue_sysfs_entry를 찍어내는 매크로
 *
 * QUEUE_RO_ENTRY와 동일하되 모드가 0644(쓰기 권한 추가)이고 .store까지
 * 연결한다. nr_requests, async_depth, read_ahead_kb, scheduler 등 자체
 * 락(elevator_lock 등)으로 보호되는 속성에 쓰인다.
 *
 * 호출 체인: 이 매크로가 만드는 entry는 queue_attrs[]/blk_mq_queue_attrs[]
 *   배열에 담겨 attribute_group을 통해 kobject core에 등록된다.
 */
#define QUEUE_RW_ENTRY(_prefix, _name)				\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr	= { .name = _name, .mode = 0644 },		\
	.show	= _prefix##_show,				\
	.store	= _prefix##_store,				\
};

/*
 * [한국어]
 * QUEUE_LIM_RO_ENTRY(_prefix, _name) - 읽기 전용, queue_limits 기반(show_limit만
 *   있는) 정적 struct queue_sysfs_entry를 찍어내는 매크로
 *
 * QUEUE_RO_ENTRY와 달리 .show 대신 .show_limit을 연결한다 - queue_attr_show()가
 * 이 필드가 채워져 있으면 disk->queue->limits_lock을 직접 잡아준다는 차이가 있다.
 *
 * 호출 체인: 이 매크로가 만드는 entry는 queue_attrs[] 배열에 담겨 등록된다.
 */
#define QUEUE_LIM_RO_ENTRY(_prefix, _name)			\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr		= { .name = _name, .mode = 0444 },	\
	.show_limit	= _prefix##_show,			\
}

/*
 * [한국어]
 * QUEUE_LIM_RW_ENTRY(_prefix, _name) - 읽기/쓰기, queue_limits 기반(show_limit+
 *   store_limit) 정적 struct queue_sysfs_entry를 찍어내는 매크로
 *
 * .show_limit과 .store_limit을 모두 연결하며 모드는 0644. max_sectors_kb,
 * write_cache, iostats_passthrough 등 queue_limits 필드에 직접 대응하는
 * 읽기/쓰기 속성에 쓰인다.
 *
 * 호출 체인: 이 매크로가 만드는 entry는 queue_attrs[] 배열에 담겨 등록된다.
 */
#define QUEUE_LIM_RW_ENTRY(_prefix, _name)			\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr		= { .name = _name, .mode = 0644 },	\
	.show_limit	= _prefix##_show,			\
	.store_limit	= _prefix##_store,			\
}

/*
 * [한국어]
 * 이하 QUEUE_RO/RW_ENTRY, QUEUE_LIM_RO/RW_ENTRY 매크로 호출들은 각각 하나의
 * /sys/block/<disk>/queue/<name> sysfs 파일을 대표하는 정적 queue_sysfs_entry를
 * 생성한다. 실제로 어떤 파일이 사용자에게 보이는지는 아래 queue_attrs[]/
 * blk_mq_queue_attrs[] 배열과 queue_attr_visible()/blk_mq_queue_attr_visible()의
 * 가시성 판정에 달려 있다 - 여기서는 엔트리 자체의 정의만 이뤄진다.
 */
/* nr_requests: NVMe SQ 소프트웨어 큐 깊이(elevator_lock 보호, RW). */
QUEUE_RW_ENTRY(queue_requests, "nr_requests");
/* async_depth: 비동기 write에 적용되는 소프트 큐 깊이(elevator_lock 보호, RW). */
QUEUE_RW_ENTRY(queue_async_depth, "async_depth");
/* read_ahead_kb: readahead 크기 KB(limits_lock 보호, RW). */
QUEUE_RW_ENTRY(queue_ra, "read_ahead_kb");
/* max_sectors_kb: 사용자 지정 최대 전송 크기 KB(queue_limits 기반, RW). */
QUEUE_LIM_RW_ENTRY(queue_max_sectors, "max_sectors_kb");
/* max_hw_sectors_kb: NVMe MDTS 기반 하드웨어 최대 전송 크기 KB(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_hw_sectors, "max_hw_sectors_kb");
/* max_segments: 최대 세그먼트 수(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_segments, "max_segments");
/* max_integrity_segments: DIF/DIX 보호정보 세그먼트 수 상한(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_integrity_segments, "max_integrity_segments");
/* max_segment_size: 세그먼트 하나의 최대 바이트(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_segment_size, "max_segment_size");
/* max_write_streams: 다중 쓰기 스트림 지원 개수(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_write_streams, "max_write_streams");
/* write_stream_granularity: 쓰기 스트림 정렬 단위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_write_stream_granularity, "write_stream_granularity");
/* scheduler: 현재/가능한 I/O 스케줄러 목록(elv_iosched_show/store, elevator_lock 보호, RW). */
QUEUE_RW_ENTRY(elv_iosched, "scheduler");

/* logical_block_size: NVMe LBA 데이터 크기(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_logical_block_size, "logical_block_size");
/* physical_block_size: 실제 미디어 쓰기 단위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_physical_block_size, "physical_block_size");
/* chunk_sectors: 최적 청크 경계(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_chunk_sectors, "chunk_sectors");
/* minimum_io_size: 최소 I/O 크기(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_io_min, "minimum_io_size");
/* optimal_io_size: 최적 I/O 크기 힌트(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_io_opt, "optimal_io_size");

/* max_discard_segments: Deallocate 세그먼트 수 상한(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_discard_segments, "max_discard_segments");
/* discard_granularity: Deallocate 정렬 단위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_discard_granularity, "discard_granularity");
/* discard_max_hw_bytes: 하드웨어 Deallocate 최대 범위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_hw_discard_sectors, "discard_max_hw_bytes");
/* discard_max_bytes: 사용자 지정 Deallocate 최대 범위(queue_limits 기반, RW). */
QUEUE_LIM_RW_ENTRY(queue_max_discard_sectors, "discard_max_bytes");
/* discard_zeroes_data: deprecated 고정값 0(자체 락 없음, RO). */
QUEUE_RO_ENTRY(queue_discard_zeroes_data, "discard_zeroes_data");

/* atomic_write_max_bytes: NVMe FAW 최대 보장 범위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_atomic_write_max_sectors, "atomic_write_max_bytes");
/*
 * atomic_write_boundary_bytes: 원자적 쓰기 경계 간격(queue_limits 기반, RO) -
 *   이름이 길어 다음 줄로 개행된 매크로 호출.
 */
QUEUE_LIM_RO_ENTRY(queue_atomic_write_boundary_sectors,
/* 매크로 인자(문자열 리터럴) 연속. */
		"atomic_write_boundary_bytes");
/* atomic_write_unit_max_bytes: NVMe FAW 최대 보장 단위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_atomic_write_unit_max, "atomic_write_unit_max_bytes");
/* atomic_write_unit_min_bytes: NVMe FAW 최소 보장 단위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_atomic_write_unit_min, "atomic_write_unit_min_bytes");

/* write_same_max_bytes: deprecated 고정값 0(자체 락 없음, RO). */
QUEUE_RO_ENTRY(queue_write_same_max, "write_same_max_bytes");
/* write_zeroes_max_bytes: NVMe Write Zeroes 최대 범위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_write_zeroes_sectors, "write_zeroes_max_bytes");
/*
 * write_zeroes_unmap_max_hw_bytes: Write Zeroes+unmap 하드웨어 최대 범위
 *   (queue_limits 기반, RO) - 이름이 길어 다음 줄로 개행된 매크로 호출.
 */
QUEUE_LIM_RO_ENTRY(queue_max_hw_wzeroes_unmap_sectors,
/* 매크로 인자(문자열 리터럴) 연속. */
		"write_zeroes_unmap_max_hw_bytes");
/*
 * write_zeroes_unmap_max_bytes: 사용자 지정 Write Zeroes+unmap 최대 범위
 *   (queue_limits 기반, RW) - 이름이 길어 다음 줄로 개행된 매크로 호출.
 */
QUEUE_LIM_RW_ENTRY(queue_max_wzeroes_unmap_sectors,
/* 매크로 인자(문자열 리터럴) 연속. */
		"write_zeroes_unmap_max_bytes");
/* zone_append_max_bytes: ZNS Zone Append 최대 전송 범위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_zone_append_sectors, "zone_append_max_bytes");
/* zone_write_granularity: ZNS zone 내부 쓰기 정렬 단위(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_zone_write_granularity, "zone_write_granularity");

/* zoned: "host-managed"/"none" 문자열(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_zoned, "zoned");
/* zoned_qd1_writes: ZNS QD1 쓰기 강제 플래그(자체 freeze/quiesce 처리, RW). */
QUEUE_RW_ENTRY(queue_zoned_qd1_writes, "zoned_qd1_writes");
/* nr_zones: 전체 zone 개수(자체 락 없음, RO). */
QUEUE_RO_ENTRY(queue_nr_zones, "nr_zones");
/* max_open_zones: 동시 Open 가능 zone 수 상한(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_open_zones, "max_open_zones");
/* max_active_zones: 동시 Active 가능 zone 수 상한(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_max_active_zones, "max_active_zones");

/* nomerges: bio/request 병합 정책 0/1/2(자체 락 없음, RW). */
QUEUE_RW_ENTRY(queue_nomerges, "nomerges");
/* iostats_passthrough: passthrough I/O 통계 포함 여부(queue_limits 기반, RW). */
QUEUE_LIM_RW_ENTRY(queue_iostats_passthrough, "iostats_passthrough");
/* rq_affinity: 완료-요청 CPU 친화도 정책 0/1/2(원자적 비트, RW). */
QUEUE_RW_ENTRY(queue_rq_affinity, "rq_affinity");
/* io_poll: polling 지원 여부 조회 + 실효 없는 쓰기(RW). */
QUEUE_RW_ENTRY(queue_poll, "io_poll");
/* io_poll_delay: deprecated, 항상 -1 조회 + no-op 쓰기(RW). */
QUEUE_RW_ENTRY(queue_poll_delay, "io_poll_delay");
/* write_cache: NVMe VWC 활성화 상태(queue_limits 기반, RW). */
QUEUE_LIM_RW_ENTRY(queue_wc, "write_cache");
/* fua: NVMe FUA 지원 여부(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_fua, "fua");
/* dax: Direct Access 지원 여부(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_dax, "dax");
/* io_timeout: 명령 타임아웃(ms)(READ_ONCE/WRITE_ONCE 기반, RW). */
QUEUE_RW_ENTRY(queue_io_timeout, "io_timeout");
/* virt_boundary_mask: PRP/SGL 가상 경계 마스크(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_virt_boundary_mask, "virt_boundary_mask");
/* dma_alignment: DMA 정렬 마스크(queue_limits 기반, RO). */
QUEUE_LIM_RO_ENTRY(queue_dma_alignment, "dma_alignment");

/*
 * legacy alias: 과거 hw_sector_size라는 이름으로 logical_block_size를 읽던 사용자공간
 *   도구와의 호환을 위해 동일한 show 콜백을 가리키는 별도 이름의 엔트리를 정의.
 */
/* legacy alias for logical_block_size: */
/*
 * 매크로를 쓰지 않고 직접 struct queue_sysfs_entry를 초기화 - 이름만 다르고 나머지는
 *   QUEUE_LIM_RO_ENTRY(queue_logical_block_size, ...)와 동등.
 */
static const struct queue_sysfs_entry queue_hw_sector_size_entry = {
/* .attr = { .name = "hw_sector_size", .mode = 0444 }: 레거시 파일명, 읽기 전용. */
	.attr		= {.name = "hw_sector_size", .mode = 0444 },
/*
 * .show_limit = queue_logical_block_size_show: logical_block_size와 완전히 동일한
 *   콜백을 재사용 - 별도 함수 정의 없이 이름만 별칭으로 추가.
 */
	.show_limit	= queue_logical_block_size_show,
/* 구조체 초기화 종료. */
};

/*
 * rotational: BLK_FEAT_ROTATIONAL 비트(queue_limits 기반, RW - 가상 장치 등이
 *   회전형 여부를 재정의할 수 있게 허용, 추정).
 */
QUEUE_LIM_RW_ENTRY(queue_rotational, "rotational");
/* iostats: BLK_FEAT_IO_STAT 비트(queue_limits 기반, RW). */
QUEUE_LIM_RW_ENTRY(queue_iostats, "iostats");
/* add_random: BLK_FEAT_ADD_RANDOM 비트(queue_limits 기반, RW). */
QUEUE_LIM_RW_ENTRY(queue_add_random, "add_random");
/* stable_writes: BLK_FEAT_STABLE_WRITES 비트(queue_limits 기반, RW). */
QUEUE_LIM_RW_ENTRY(queue_stable_writes, "stable_writes");

/*
 * CONFIG_BLK_WBT(Writeback Throttle 서브시스템)가 활성화된 빌드에서만 wbt_lat_usec
 *   sysfs 파일과 관련 헬퍼를 컴파일 - WBT는 쓰기 지연시간을 관찰해 백그라운드
 *   writeback을 스스로 조절하는 QoS 메커니즘이다.
 */
#ifdef CONFIG_BLK_WBT
/*
 * [한국어]
 * queue_var_store64 - 부호 있는 64비트(s64) 값을 파싱하는 wbt 전용 입력 헬퍼
 *
 * @var: 파싱 결과를 저장할 출력 포인터.
 * @page: 사용자 입력 문자열(부호를 포함할 수 있는 십진수, 예: "-1").
 * @return: 성공 시 0, 실패 시 kstrtos64()가 반환한 음수 errno.
 *
 * wbt_lat_usec는 -1(WBT 비활성화 요청)이라는 특수값을 허용해야 하므로,
 * 다른 속성들이 쓰는 unsigned 전용 queue_var_store() 대신 부호 있는 별도
 * 파서를 둔다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * 호출자: queue_wb_lat_store().
 * 피호출자: kstrtos64().
 * 에러 경로: kstrtos64() 실패 시 해당 errno를 그대로 반환.
 *
 * 호출 체인:
 *   queue_wb_lat_store → [queue_var_store64] → kstrtos64
 */
static ssize_t queue_var_store64(s64 *var, const char *page)
{
	/* kstrtos64()의 반환 에러 코드를 담을 변수. */
	int err;
	/* 파싱된 부호 있는 64비트 값을 담을 변수. */
	s64 v;

	/* 10진 문자열(부호 포함 가능)을 s64로 파싱. */
	err = kstrtos64(page, 10, &v);
	/* 파싱이 실패했으면(음수 errno) */
	if (err < 0)
		/* 해당 에러 코드를 그대로 반환. */
		return err;

	/* 파싱된 값을 출력 포인터에 반영. */
	*var = v;
	/* 성공(0) 반환. */
	return 0;
}

/*
 * [한국어]
 * queue_wb_lat_show - /sys/block/<disk>/queue/wbt_lat_usec 읽기
 *
 * @disk: 조회 대상 gendisk.
 * @page: 결과 문자열을 담을 출력 버퍼(마이크로초 단위 정수, 비활성 시 "0").
 * @return: 성공 시 sysfs_emit() 바이트 수, WBT 미설치 시 -EINVAL.
 *
 * WBT가 이 큐에 rq_qos 플러그인으로 설치되어 있어야 값을 읽을 수 있다 -
 * NVMe 큐라도 WBT가 disable된 상태(wbt_disabled())라면 "0"을 반환해
 * "지연시간 목표 없음"을 표현한다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트. disk->rqos_state_mutex로
 *   rq_qos 체인 자체가 등록/해제되는 것과의 경합을 막는다.
 * 호출자: queue_wb_lat_entry.show를 통해 queue_attr_show().
 * 피호출자: wbt_rq_qos(), wbt_disabled(), wbt_get_min_lat(), sysfs_emit(), div_u64().
 * 에러 경로: wbt_rq_qos(q)가 NULL(WBT 미설치)이면 -EINVAL.
 *
 * 호출 체인:
 *   queue_attr_show → [queue_wb_lat_show] → wbt_get_min_lat
 */
static ssize_t queue_wb_lat_show(struct gendisk *disk, char *page)
{
	/* 반환값을 담을 변수. */
	ssize_t ret;
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;

	/* rqos_state_mutex: rq_qos 플러그인 체인(WBT 포함)의 등록/해제와 경합하지 않도록 보호. */
	mutex_lock(&disk->rqos_state_mutex);
	/* 이 큐에 WBT rq_qos 인스턴스가 아예 설치되어 있지 않은 경우. */
	if (!wbt_rq_qos(q)) {
		/* WBT 미설치 - 조회 불가 에러. */
		ret = -EINVAL;
		/* out 레이블로 점프해 락 해제 후 반환. */
		goto out;
	}

	/* WBT는 설치되어 있으나 현재 비활성화(지연시간 목표 미설정) 상태인 경우. */
	if (wbt_disabled(q)) {
		/* "0\n"으로 '목표 없음'을 표현. */
		ret = sysfs_emit(page, "0\n");
		/* out 레이블로 점프. */
		goto out;
	}

	/*
	 * wbt_get_min_lat()이 나노초 단위로 반환하는 값을 1000으로 나눠 마이크로초로
	 *   환산 후 "%llu\n" 포맷으로 출력.
	 */
	ret = sysfs_emit(page, "%llu\n", div_u64(wbt_get_min_lat(q), 1000));
/* out: 세 경로(미설치/비활성/정상)가 합류하는 지점. */
out:
	/* rqos_state_mutex 해제. */
	mutex_unlock(&disk->rqos_state_mutex);
	/* 결과 반환. */
	return ret;
}

/*
 * [한국어]
 * queue_wb_lat_store - /sys/block/<disk>/queue/wbt_lat_usec 쓰기
 *
 * @disk: 대상 gendisk.
 * @page: 새 지연시간 목표(마이크로초, -1이면 WBT를 완전히 비활성화하라는 특수값).
 * @count: page 길이.
 * @return: 성공 시 count, 실패 시 음수 errno.
 *
 * 실제 rq_qos 체인에 WBT를 설치/조정하는 로직은 wbt_set_lat()
 * (block/blk-wbt.c)에 위임한다. -1보다 작은 값(-2 이하)은 의미가 없으므로
 * 이 함수 단계에서 미리 거른다.
 * 실행 컨텍스트: sysfs write(2) 프로세스 컨텍스트.
 * 호출자: queue_wb_lat_entry.store를 통해 queue_attr_store().
 * 피호출자: queue_var_store64(), wbt_set_lat().
 * 에러 경로: 파싱 실패 또는 val < -1이면 -EINVAL, wbt_set_lat() 실패 시 그 값.
 *
 * 호출 체인:
 *   queue_attr_store → [queue_wb_lat_store] → wbt_set_lat
 */
static ssize_t queue_wb_lat_store(struct gendisk *disk, const char *page,
				  size_t count)
{
	/* 반환값을 담을 변수. */
	ssize_t ret;
	/* 파싱된 부호 있는 마이크로초 값. */
	s64 val;

	/* 입력 문자열을 val로 파싱(부호 있는 64비트, -1 특수값 허용). */
	ret = queue_var_store64(&val, page);
	/* 파싱 실패 시 조기 반환. */
	if (ret < 0)
		/* 에러 코드 전달. */
		return ret;
	/* -1(비활성화 요청)보다 작은 값은 의미가 없으므로 거부. */
	if (val < -1)
		/* 잘못된 값 - 거부. */
		return -EINVAL;

	/*
	 * 실제 WBT 설치/파라미터 조정은 wbt_set_lat()에 위임 - rq_qos 체인 삽입/제거,
	 *   disk->rqos_state_mutex 획득 등은 그 함수 내부에서 처리(추정).
	 */
	ret = wbt_set_lat(disk, val);
	/* 실패 시 wbt_set_lat()의 에러코드, 성공 시 count를 반환. */
	return ret ? ret : count;
}

/* wbt_lat_usec sysfs 파일(자체 락 disk->rqos_state_mutex 사용, RW) 엔트리 정의. */
QUEUE_RW_ENTRY(queue_wb_lat, "wbt_lat_usec");
/* CONFIG_BLK_WBT 블록 종료. */
#endif

/*
 * [한국어]
 * queue_attrs[] - bio 기반(legacy)과 blk-mq 기반 큐가 공통으로 갖는 sysfs
 *   속성들의 NULL 종료 포인터 배열
 *
 * kobj_type.default_groups(blk_queue_ktype)에 담긴 attribute_group 중 하나인
 * queue_attr_group.attrs_const로 지정되며, 실제 파일 생성 여부는
 * queue_attr_visible()이 개별 attr마다 한 번 더 걸러낸다. 배열은 크게
 * "limits_lock으로 보호되는 속성"과 "락이 필요 없는 속성" 두 그룹으로
 * 주석(원본 영어 주석)으로 나뉘어 있으며, 이는 각 entry의 .show_limit
 * 유무와 대응한다.
 */
/* Common attributes for bio-based and request-based queues. */
static const struct attribute *const queue_attrs[] = {
/*
 * 이하 항목들은 q->limits_lock으로 보호되는 queue_limits 기반 속성 그룹
 *   (원본 영어 주석 - QUEUE_LIM_RO/RW_ENTRY로 만들어진 .show_limit 보유 엔트리).
 */
	/*
	 * Attributes which are protected with q->limits_lock.
	 */
/* max_hw_sectors_kb 엔트리 주소. */
	&queue_max_hw_sectors_entry.attr,
/* max_sectors_kb 엔트리 주소. */
	&queue_max_sectors_entry.attr,
/* max_segments 엔트리 주소. */
	&queue_max_segments_entry.attr,
/* max_discard_segments 엔트리 주소. */
	&queue_max_discard_segments_entry.attr,
/* max_integrity_segments 엔트리 주소. */
	&queue_max_integrity_segments_entry.attr,
/* max_segment_size 엔트리 주소. */
	&queue_max_segment_size_entry.attr,
/* max_write_streams 엔트리 주소. */
	&queue_max_write_streams_entry.attr,
/* write_stream_granularity 엔트리 주소. */
	&queue_write_stream_granularity_entry.attr,
/* hw_sector_size(legacy alias) 엔트리 주소. */
	&queue_hw_sector_size_entry.attr,
/* logical_block_size 엔트리 주소. */
	&queue_logical_block_size_entry.attr,
/* physical_block_size 엔트리 주소. */
	&queue_physical_block_size_entry.attr,
/* chunk_sectors 엔트리 주소. */
	&queue_chunk_sectors_entry.attr,
/* minimum_io_size 엔트리 주소. */
	&queue_io_min_entry.attr,
/* optimal_io_size 엔트리 주소. */
	&queue_io_opt_entry.attr,
/* discard_granularity 엔트리 주소. */
	&queue_discard_granularity_entry.attr,
/* discard_max_bytes 엔트리 주소. */
	&queue_max_discard_sectors_entry.attr,
/* discard_max_hw_bytes 엔트리 주소. */
	&queue_max_hw_discard_sectors_entry.attr,
/* atomic_write_max_bytes 엔트리 주소. */
	&queue_atomic_write_max_sectors_entry.attr,
/* atomic_write_boundary_bytes 엔트리 주소. */
	&queue_atomic_write_boundary_sectors_entry.attr,
/* atomic_write_unit_min_bytes 엔트리 주소. */
	&queue_atomic_write_unit_min_entry.attr,
/* atomic_write_unit_max_bytes 엔트리 주소. */
	&queue_atomic_write_unit_max_entry.attr,
/* write_zeroes_max_bytes 엔트리 주소. */
	&queue_max_write_zeroes_sectors_entry.attr,
/* write_zeroes_unmap_max_hw_bytes 엔트리 주소. */
	&queue_max_hw_wzeroes_unmap_sectors_entry.attr,
/* write_zeroes_unmap_max_bytes 엔트리 주소. */
	&queue_max_wzeroes_unmap_sectors_entry.attr,
/* zone_append_max_bytes 엔트리 주소. */
	&queue_max_zone_append_sectors_entry.attr,
/* zone_write_granularity 엔트리 주소. */
	&queue_zone_write_granularity_entry.attr,
/* rotational 엔트리 주소. */
	&queue_rotational_entry.attr,
/* zoned 엔트리 주소. */
	&queue_zoned_entry.attr,
/* max_open_zones 엔트리 주소. */
	&queue_max_open_zones_entry.attr,
/* max_active_zones 엔트리 주소. */
	&queue_max_active_zones_entry.attr,
/* iostats_passthrough 엔트리 주소. */
	&queue_iostats_passthrough_entry.attr,
/* iostats 엔트리 주소. */
	&queue_iostats_entry.attr,
/* stable_writes 엔트리 주소. */
	&queue_stable_writes_entry.attr,
/* add_random 엔트리 주소. */
	&queue_add_random_entry.attr,
/* write_cache 엔트리 주소. */
	&queue_wc_entry.attr,
/* fua 엔트리 주소. */
	&queue_fua_entry.attr,
/* dax 엔트리 주소. */
	&queue_dax_entry.attr,
/* virt_boundary_mask 엔트리 주소. */
	&queue_virt_boundary_mask_entry.attr,
/* dma_alignment 엔트리 주소. */
	&queue_dma_alignment_entry.attr,
/*
 * read_ahead_kb 엔트리 주소(자체 락은 elevator_lock이 아닌 limits_lock을 씀에도
 *   .show/.store 형태 - queue_ra_show/store가 내부에서 직접 limits_lock을 잡는다).
 */
	&queue_ra_entry.attr,

/*
 * 이하 항목들은 별도 락이 필요 없는(또는 원자적 비트 연산만 쓰는) 속성 그룹
 *   (원본 영어 주석).
 */
	/*
	 * Attributes which don't require locking.
	 */
/* discard_zeroes_data(deprecated) 엔트리 주소. */
	&queue_discard_zeroes_data_entry.attr,
/* write_same_max(deprecated) 엔트리 주소. */
	&queue_write_same_max_entry.attr,
/* nr_zones 엔트리 주소. */
	&queue_nr_zones_entry.attr,
/* nomerges 엔트리 주소. */
	&queue_nomerges_entry.attr,
/* io_poll 엔트리 주소. */
	&queue_poll_entry.attr,
/* io_poll_delay(deprecated) 엔트리 주소. */
	&queue_poll_delay_entry.attr,
/* zoned_qd1_writes 엔트리 주소(freeze/quiesce로 자체 동기화, 별도 상시 락 불필요). */
	&queue_zoned_qd1_writes_entry.attr,

/* 배열 종결 NULL 센티널 - kobject core가 순회를 멈추는 기준. */
	NULL,
};

/*
 * [한국어]
 * blk_mq_queue_attrs[] - blk-mq(요청 기반) 큐에서만 의미가 있는 sysfs 속성들의
 *   NULL 종료 배열
 *
 * bio 기반 legacy 큐는 elevator/tag_set 개념이 없어 nr_requests, scheduler,
 * async_depth, io_timeout 같은 속성이 무의미하므로 별도 attribute_group
 * (blk_mq_queue_attr_group)으로 분리하고, blk_mq_queue_attr_visible()이
 * queue_is_mq() 검사로 legacy 큐에서는 이 그룹 전체를 숨긴다.
 */
/* Request-based queue attributes that are not relevant for bio-based queues. */
static const struct attribute *const blk_mq_queue_attrs[] = {
/*
 * q->sysfs_lock 외의 형태(elevator_lock/update_nr_hwq_lock 등)로 보호되는 그룹
 *   (원본 영어 주석).
 */
	/*
	 * Attributes which require some form of locking other than
	 * q->sysfs_lock.
	 */
/* scheduler 엔트리 주소(elevator_lock 보호). */
	&elv_iosched_entry.attr,
/* nr_requests 엔트리 주소(elevator_lock + update_nr_hwq_lock 보호). */
	&queue_requests_entry.attr,
/* async_depth 엔트리 주소(elevator_lock 보호). */
	&queue_async_depth_entry.attr,
/* CONFIG_BLK_WBT 빌드에서만 wbt_lat_usec 엔트리 포함. */
#ifdef CONFIG_BLK_WBT
/* wbt_lat_usec 엔트리 주소(disk->rqos_state_mutex 보호). */
	&queue_wb_lat_entry.attr,
/* CONFIG_BLK_WBT 조건부 포함 종료. */
#endif
/* 락이 필요 없는(원자적 비트/READ_ONCE 기반) 그룹(원본 영어 주석). */
	/*
	 * Attributes which don't require locking.
	 */
/* rq_affinity 엔트리 주소(원자적 비트 연산). */
	&queue_rq_affinity_entry.attr,
/* io_timeout 엔트리 주소(READ_ONCE/WRITE_ONCE 기반). */
	&queue_io_timeout_entry.attr,

/* 배열 종결 NULL 센티널. */
	NULL,
};

/*
 * [한국어]
 * queue_attr_visible - queue_attr_group에 속한 각 attribute의 실제 노출 여부 판정
 *
 * @kobj: 이 attribute_group이 속한 kobject - disk->queue_kobj 주소로부터
 *   container_of()로 상위 gendisk를 복원할 수 있다.
 * @attr: 현재 검사 중인 struct attribute(queue_attrs[] 배열의 한 원소).
 * @n: 배열 내 인덱스(이 함수에서는 사용하지 않음 - is_visible 콜백 표준 시그니처의
 *   일부일 뿐).
 * @return: 노출해도 되면 attr->mode(파일 권한 그대로), 숨겨야 하면 0(파일을
 *   아예 만들지 않음을 의미하는 sysfs 관례).
 *
 * ZNS 전용 속성(max_open_zones/max_active_zones/zoned_qd1_writes)은 이
 * 장치가 zoned가 아니면 사용자에게 혼란을 주므로 애초에 파일 자체를 만들지
 * 않는다.
 * 실행 컨텍스트: sysfs 디렉터리 population 시점(kobject_add()/kobject_uevent()
 *   경로)에 kernfs가 각 attribute마다 한 번씩 호출.
 * 호출자: kobject/sysfs core(populate_dir() 계열)가 attribute_group.is_visible_const로
 *   등록된 이 함수를 호출.
 * 피호출자: container_of(), blk_queue_is_zoned().
 * 에러 경로: 없음(단순 가시성 판정, 실패 개념이 없음).
 *
 * 호출 체인:
 *   sysfs populate_dir → [queue_attr_visible] → blk_queue_is_zoned
 */
static umode_t queue_attr_visible(struct kobject *kobj, const struct attribute *attr,
				int n)
{
	/* kobj(disk->queue_kobj)로부터 컨테이너인 gendisk를 역산. */
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);
	/* gendisk에서 request_queue를 꺼내 zoned 여부 판정에 사용. */
	struct request_queue *q = disk->queue;

	/*
	 * 현재 attr이 zoned 전용 3개 속성(max_open_zones/max_active_zones/
	 *   zoned_qd1_writes) 중 하나이면서
	 */
	if ((attr == &queue_max_open_zones_entry.attr ||
		/* (계속) max_active_zones인 경우 또는 */
	     attr == &queue_max_active_zones_entry.attr ||
		/* (계속) zoned_qd1_writes인 경우이고 */
	     attr == &queue_zoned_qd1_writes_entry.attr) &&
	/* 이 큐가 zoned 장치가 아니면(host-managed ZNS가 아니면) */
	    !blk_queue_is_zoned(q))
		/* 0을 반환해 이 attribute 파일을 아예 만들지 않는다. */
		return 0;

	/* 그 외에는 attribute가 원래 갖고 있던 모드(0444/0644)를 그대로 반환해 정상 노출. */
	return attr->mode;
}

/*
 * [한국어]
 * blk_mq_queue_attr_visible - blk_mq_queue_attr_group에 속한 각 attribute의
 *   실제 노출 여부 판정
 *
 * @kobj: disk->queue_kobj.
 * @attr: 현재 검사 중인 struct attribute(blk_mq_queue_attrs[] 배열의 한 원소).
 * @n: 배열 내 인덱스(사용하지 않음).
 * @return: 노출 시 attr->mode, 숨김 시 0.
 *
 * legacy(bio 기반) 큐에는 blk-mq 전용 속성 전체를 숨기고, blk-mq 큐라도
 * 드라이버가 타임아웃 콜백(q->mq_ops->timeout)을 등록하지 않았다면
 * io_timeout 파일만 별도로 숨긴다 - NVMe는 항상 타임아웃 콜백을 등록하므로
 * (nvme_timeout) 이 조건에 걸리지 않는다(추정).
 * 실행 컨텍스트: sysfs 디렉터리 population 시점.
 * 호출자: kobject/sysfs core가 attribute_group.is_visible_const로 호출.
 * 피호출자: container_of(), queue_is_mq().
 *
 * 호출 체인:
 *   sysfs populate_dir → [blk_mq_queue_attr_visible] → queue_is_mq
 */
static umode_t blk_mq_queue_attr_visible(struct kobject *kobj,
					 const struct attribute *attr, int n)
{
	/* kobj로부터 gendisk 역산. */
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);
	/* request_queue 포인터 확보. */
	struct request_queue *q = disk->queue;

	/* blk-mq 기반이 아닌(legacy bio 기반) 큐라면 */
	if (!queue_is_mq(q))
		/* 이 그룹 전체(nr_requests/scheduler/io_timeout 등)를 숨긴다. */
		return 0;

	/* io_timeout 속성인데 드라이버가 타임아웃 콜백을 등록하지 않은 경우 */
	if (attr == &queue_io_timeout_entry.attr && !q->mq_ops->timeout)
		/* 이 속성만 개별적으로 숨긴다 - 타임아웃 개념이 없는 드라이버에서 혼란 방지. */
		return 0;

	/* 그 외에는 원래 모드를 그대로 반환. */
	return attr->mode;
}

/*
 * [한국어]
 * queue_attr_group - 공통(bio+mq) queue 속성들의 attribute_group
 *
 * .attrs_const에 queue_attrs[] 배열을, .is_visible_const에 queue_attr_visible()을
 * 연결한다 - kobj_type.default_groups 배열(blk_queue_ktype)의 첫 번째 원소로
 * 등록되어, disk->queue_kobj가 kobject_add()될 때 자동으로 이 그룹의 모든
 * 가시 attribute에 대한 sysfs 파일이 생성된다.
 */
static const struct attribute_group queue_attr_group = {
	/* 이 그룹이 대표하는 attribute 배열(NULL 종료) 연결. */
	.attrs_const = queue_attrs,
	/* 가시성 판정 콜백 연결 - attribute마다 개별적으로 파일 생성 여부를 결정. */
	.is_visible_const = queue_attr_visible,
};

/*
 * [한국어]
 * blk_mq_queue_attr_group - blk-mq 전용 queue 속성들의 attribute_group
 *
 * .attrs_const에 blk_mq_queue_attrs[]를, .is_visible_const에
 * blk_mq_queue_attr_visible()을 연결한다 - kobj_type.default_groups의 두
 * 번째 원소로 등록되어, legacy 큐에서는 이 그룹의 모든 파일이 숨겨진다.
 */
static const struct attribute_group blk_mq_queue_attr_group = {
	/* 이 그룹이 대표하는 attribute 배열 연결. */
	.attrs_const = blk_mq_queue_attrs,
	/* 가시성 판정 콜백 연결. */
	.is_visible_const = blk_mq_queue_attr_visible,
};

/*
 * to_queue(atr): struct attribute 포인터를 그것을 포함하는
 *   struct queue_sysfs_entry 포인터로 되돌리는 container_of_const 매크로 -
 *   attr은 항상 entry.attr 필드의 주소이므로 오프셋 계산으로 상위 구조체를 복원한다.
 *   _const 버전을 쓰는 이유는 입력이 const struct attribute *일 수 있어도
 *   안전하게(top-level const 유지) 캐스팅하기 위함.
 */
#define to_queue(atr) container_of_const((atr), struct queue_sysfs_entry, attr)

/*
 * [한국어]
 * queue_attr_show - kobj_type.sysfs_ops.show의 실제 구현체: sysfs read(2)의 진입점
 *
 * @kobj: 읽으려는 파일이 속한 kobject(disk->queue_kobj).
 * @attr: 읽으려는 struct attribute(어떤 queue_sysfs_entry의 .attr인지는
 *   to_queue()로 역산).
 * @page: 결과를 기록할 PAGE_SIZE 버퍼(sysfs read(2) 규약).
 * @return: 콜백이 기록한 바이트 수, show/show_limit이 모두 없으면 -EIO.
 *
 * entry가 .show_limit을 갖고 있으면(queue_limits 기반 속성) 여기서
 * disk->queue->limits_lock을 직접 잡고 콜백을 호출한 뒤 풀어준다 - 개별
 * show_limit 콜백들은 그래서 락을 신경 쓸 필요가 없다. .show만 있으면
 * (자체 락 기반 속성) 락 없이 그대로 위임하며, 콜백 내부가 스스로 락을 관리한다.
 * 실행 컨텍스트: 사용자공간 read(2) -> VFS -> kernfs가 호출하는 프로세스 컨텍스트.
 * 호출자: kernfs_ops.seq_show 등 sysfs 코어(kobj_type.sysfs_ops.show로 등록됨).
 * 피호출자: to_queue(), container_of(), entry->show_limit() 또는 entry->show().
 * 에러 경로: show/show_limit이 모두 NULL이면(정의 오류 상황) -EIO.
 *
 * 호출 체인:
 *   sysfs read(2) → kernfs → [queue_attr_show] → entry->show_limit / entry->show
 */
static ssize_t
queue_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	/* attr(struct attribute*)로부터 상위 queue_sysfs_entry를 복원. */
	struct queue_sysfs_entry *entry = to_queue(attr);
	/*
	 * kobj(disk->queue_kobj)로부터 상위 gendisk를 복원 - show 콜백들이 disk를 인자로
	 *   받으므로 여기서 미리 변환해 둔다.
	 */
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);

	/* 두 콜백이 모두 없으면(entry 정의 오류) 읽을 방법이 없다는 뜻. */
	if (!entry->show && !entry->show_limit)
		/* -EIO(입출력 오류)로 보고. */
		return -EIO;

	/* queue_limits 기반 속성이면(show_limit이 있으면) */
	if (entry->show_limit) {
		/* 콜백 반환값을 담을 변수. */
		ssize_t res;

		/*
		 * limits_lock 획득 - queue_limits 필드를 읽는 동안 동시 갱신(store_limit)과의
		 *   경합을 막는다.
		 */
		mutex_lock(&disk->queue->limits_lock);
		/* 실제 show_limit 콜백 호출 - 콜백 내부는 이미 락이 걸려 있다고 가정하고 동작. */
		res = entry->show_limit(disk, page);
		/* 락 해제. */
		mutex_unlock(&disk->queue->limits_lock);
		/* 결과 반환. */
		return res;
	}

	/*
	 * 자체 락 기반 속성이면 락 없이 그대로 show 콜백에 위임 - 콜백이 스스로 필요한
	 *   락(elevator_lock 등)을 잡는다.
	 */
	return entry->show(disk, page);
}

/*
 * [한국어]
 * queue_attr_store - kobj_type.sysfs_ops.store의 실제 구현체: sysfs write(2)의 진입점
 *
 * @kobj: 쓰려는 파일이 속한 kobject.
 * @attr: 쓰려는 struct attribute.
 * @page: 사용자 입력 버퍼(NUL 종료 문자열).
 * @length: page의 바이트 길이.
 * @return: 처리한 바이트 수(성공 시 length), store/store_limit이 모두 없으면
 *   -EIO, 콜백이 실패를 반환하면 그 음수 errno.
 *
 * store_limit 경로는 queue_limits_start_update()로 q->limits_lock을 잡고
 * 현재 limits의 스택 사본(lim)을 얻은 뒤, 콜백이 그 사본만 수정하게 한다.
 * 콜백이 실패(res<0)하면 queue_limits_cancel_update()로 그냥 락만 풀고
 * 사본을 버리며, 성공하면 queue_limits_commit_update_frozen()이 큐를
 * freeze한 뒤 사본을 q->limits에 원자적으로 반영(및 unlock)한다. store만
 * 있는 경로는 콜백에 그대로 위임하며, 콜백이 자체 락/freeze를 처리한다.
 * 실행 컨텍스트: 사용자공간 write(2) -> VFS -> kernfs가 호출하는 프로세스 컨텍스트.
 * 호출자: kobj_type.sysfs_ops.store로 등록된 sysfs 코어.
 * 피호출자: to_queue(), container_of(), queue_limits_start_update(),
 *   entry->store_limit(), queue_limits_cancel_update(),
 *   queue_limits_commit_update_frozen(), entry->store().
 * 에러 경로: store/store_limit이 모두 NULL이면 -EIO. store_limit 콜백 실패
 *   시 그 값. commit 단계 실패 시(예: blk_validate_limits 거부) 그 값.
 *
 * 호출 체인:
 *   sysfs write(2) → kernfs → [queue_attr_store] → entry->store_limit
 *     → queue_limits_commit_update_frozen  (또는 entry->store)
 */
static ssize_t
queue_attr_store(struct kobject *kobj, struct attribute *attr,
		    const char *page, size_t length)
{
	/* attr로부터 상위 queue_sysfs_entry 복원. */
	struct queue_sysfs_entry *entry = to_queue(attr);
	/* kobj로부터 상위 gendisk 복원. */
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;

	/* 두 콜백이 모두 없으면(읽기 전용 entry에 쓰기를 시도한 경우) 쓸 방법이 없다. */
	if (!entry->store_limit && !entry->store)
		/* -EIO로 보고. */
		return -EIO;

	/* queue_limits 기반 속성이면(store_limit이 있으면) */
	if (entry->store_limit) {
		/* 콜백/커밋 결과를 담을 변수. */
		ssize_t res;

		/*
		 * limits_lock을 잡고 현재 q->limits의 스택 사본을 얻는다 - 이 사본(lim)만
		 *   수정하고 원본은 커밋 전까지 그대로 유지되어, 검증 실패 시 롤백이 간단하다.
		 */
		struct queue_limits lim = queue_limits_start_update(q);

		/* 실제 store_limit 콜백 호출 - lim 사본을 검증/수정. */
		res = entry->store_limit(disk, page, length, &lim);
		/* 콜백이 실패를 보고한 경우(값 검증 실패 등). */
		if (res < 0) {
			/* 락만 풀고 사본을 버림 - q->limits는 전혀 건드리지 않은 상태로 유지. */
			queue_limits_cancel_update(q);
			/* 콜백의 에러 코드를 그대로 반환. */
			return res;
		}

		/*
		 * 검증된 사본을 실제로 커밋 - 내부에서 freeze -> blk_validate_limits 재검증
		 *   -> q->limits 교체 -> unfreeze -> limits_lock 해제까지 모두 수행.
		 */
		res = queue_limits_commit_update_frozen(q, &lim);
		/* 커밋 자체가 실패(예: 필드 간 정합성 위반)할 수도 있다. */
		if (res)
			/* 커밋 실패 에러 코드 반환. */
			return res;
		/* 성공 - 사용자에게 전체 입력 길이를 처리했다고 보고. */
		return length;
	}

	/* 자체 락 기반 속성이면 락 없이 그대로 store 콜백에 위임. */
	return entry->store(disk, page, length);
}

/*
 * [한국어]
 * queue_sysfs_ops - blk_queue_ktype이 사용하는 sysfs_ops 테이블
 *
 * kobject core가 이 kobj_type에 속한 kobject(disk->queue_kobj)의 read(2)/
 * write(2)를 처리할 때 항상 이 두 함수(queue_attr_show/queue_attr_store)를
 * 거치도록 만드는 진입점 테이블이다.
 */
static const struct sysfs_ops queue_sysfs_ops = {
	/* read(2) 진입점 연결. */
	.show	= queue_attr_show,
	/* write(2) 진입점 연결. */
	.store	= queue_attr_store,
};

/*
 * [한국어]
 * blk_queue_attr_groups[] - blk_queue_ktype.default_groups에 연결되는 NULL 종료
 *   attribute_group 포인터 배열
 *
 * kobject_add()가 disk->queue_kobj를 등록할 때 이 배열의 각 그룹을 순회하며
 * sysfs 디렉터리에 파일들을 생성한다 - 순서상 공통 속성(queue_attr_group)이
 * 먼저, blk-mq 전용 속성(blk_mq_queue_attr_group)이 나중에 채워진다.
 */
static const struct attribute_group *blk_queue_attr_groups[] = {
	/* 공통(bio+mq) 속성 그룹. */
	&queue_attr_group,
	/* blk-mq 전용 속성 그룹. */
	&blk_mq_queue_attr_group,
	/* 배열 종결 NULL 센티널. */
	NULL
};

/*
 * [한국어]
 * blk_queue_release - disk->queue_kobj의 참조 카운트가 0이 되었을 때 kobject
 *   core가 호출하는 해제 콜백
 *
 * @kobj: 참조 카운트가 소진된 kobject(disk->queue_kobj).
 * @return: 없음(void).
 *
 * request_queue 자체의 메모리는 gendisk(부모 kobject 트리)가 소유하고
 * 별도로 해제되므로, 이 콜백은 kobject 프레임워크의 요구(모든 kobj_type은
 * .release를 가져야 함)를 만족시키기 위한 빈 구현이다.
 * 실행 컨텍스트: 마지막 kobject_put() 호출 컨텍스트(보통 del_gendisk 경로).
 * 호출자: kobject_release() (kobject core, 참조 카운트가 0이 될 때).
 * 피호출자: 없음.
 *
 * 호출 체인:
 *   kobject_put (참조 0 도달) → kobject_release → [blk_queue_release] (no-op)
 */
static void blk_queue_release(struct kobject *kobj)
{
	/*
	 * 원본 영어 주석: 모든 데이터는 부모 gendisk가 소유하므로 여기서 할 일이 없음 -
	 *   request_queue 메모리 해제는 gendisk 해제 경로가 책임진다.
	 */
	/* nothing to do here, all data is associated with the parent gendisk */
}

/*
 * [한국어]
 * blk_queue_ktype - disk->queue_kobj의 kobj_type(핸들러 테이블)
 *
 * kobject core가 disk->queue_kobj를 다룰 때 참조하는 3요소:
 * default_groups(등록 시 자동 생성할 attribute_group들), sysfs_ops(read/write
 * 진입점), release(참조 소진 시 정리 콜백). 이 구조체 하나가 사실상 이
 * 파일의 "정체성"이며, add_disk() -> blk_register_queue()에서
 * kobject_init_and_add() 계열(kobject_add 이전 어딘가에서 kobj->ktype이
 * 이 값으로 설정됨, 추정)을 통해 disk->queue_kobj에 연결된다.
 */
const struct kobj_type blk_queue_ktype = {
	/* 등록 시 자동으로 채워질 attribute_group 배열 연결. */
	.default_groups = blk_queue_attr_groups,
	/* read/write 진입점 테이블 연결. */
	.sysfs_ops	= &queue_sysfs_ops,
	/* 참조 소진 시 호출될 해제 콜백 연결. */
	.release	= blk_queue_release,
};

/*
 * [한국어]
 * blk_debugfs_remove - request_queue의 debugfs 트리(/sys/kernel/debug/block/<disk>)
 *   전체를 제거
 *
 * @disk: 대상 gendisk.
 * @return: 없음(void).
 *
 * blk_register_queue()가 만든 debugfs_dir와 그 하위(sched_debugfs_dir,
 * rqos_debugfs_dir 등 - blk_mq_debugfs_register()가 채웠던 하위 디렉터리들)
 * 를 debugfs_remove_recursive() 한 번으로 통째로 제거하고, blktrace 세션도
 * 함께 종료한다. blk_register_queue()의 실패 경로와 blk_unregister_queue()
 * 양쪽에서 재사용되는 공용 정리 함수다.
 * 실행 컨텍스트: 프로세스 컨텍스트(등록 실패 롤백 또는 del_gendisk 경로).
 *   blk_debugfs_lock_nomemsave()/unlock_nomemrestore()로 debugfs_mutex를
 *   잡아 다른 debugfs 등록/조회와의 경합을 막는다(NOIO 전환은 호출자 책임인
 *   _nomemsave 버전을 쓰는 이유는 이미 락 스코프가 더 큰 상위 호출자가
 *   NOIO 전환까지 관리하고 있을 수 있기 때문, 추정).
 * 호출자: blk_register_queue()의 에러 롤백 경로, blk_unregister_queue().
 * 피호출자: blk_debugfs_lock_nomemsave(), blk_trace_shutdown(),
 *   debugfs_remove_recursive(), blk_debugfs_unlock_nomemrestore().
 *
 * 호출 체인:
 *   blk_register_queue(실패 롤백) / blk_unregister_queue →
 *     [blk_debugfs_remove] → debugfs_remove_recursive
 */
static void blk_debugfs_remove(struct gendisk *disk)
{
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;

	/*
	 * debugfs_mutex 획득(NOIO 전환은 호출자 책임인 버전) - 등록/해제 도중 다른
	 *   debugfs 조작과 경합하지 않도록 보호.
	 */
	blk_debugfs_lock_nomemsave(q);
	/*
	 * 이 큐에 연결된 blktrace 세션이 있다면 종료 - debugfs 삭제 전에 트레이서가
	 *   더 이상 이 큐를 참조하지 않도록 정리.
	 */
	blk_trace_shutdown(q);
	/* debugfs_dir 이하 모든 하위 파일/디렉터리(hctx별 상태, sched, rqos 등)를 재귀 제거. */
	debugfs_remove_recursive(q->debugfs_dir);
	/* 루트 디렉터리 포인터 정리 - 이미 해제된 포인터를 실수로 재사용하는 것을 방지. */
	q->debugfs_dir = NULL;
	/* 스케줄러 debugfs 하위 디렉터리 참조 정리(부모가 사라졌으므로 함께 무효화). */
	q->sched_debugfs_dir = NULL;
	/* rq_qos(WBT 등) debugfs 하위 디렉터리 참조 정리. */
	q->rqos_debugfs_dir = NULL;
	/* debugfs_mutex 해제. */
	blk_debugfs_unlock_nomemrestore(q);
}

/*
 * [한국어]
 * blk_register_queue - add_disk() 경로에서 request_queue를 sysfs/debugfs에
 *   등록하고 I/O를 받을 준비를 완전히 마친다
 *
 * @disk: sysfs에 등록할 request_queue를 보유한 gendisk.
 * @return: 성공 시 0, 실패 시 각 단계에서 발생한 음수 errno.
 *
 * add_disk()가 호출하는 이 함수는 disk->queue_kobj를 "/sys/block/<disk>/queue"
 * 로 kobject_add()하는 것을 시작으로, blk-mq sysfs(mq/ 하위), debugfs 트리,
 * 독립 접근 범위(independent access ranges), blk-crypto sysfs, 기본
 * 스케줄러(elevator_set_default) 설정, WBT 기본 활성화까지 NVMe 큐 하나가
 * 사용자공간에 완전히 노출되기 위해 필요한 모든 단계를 순서대로 수행한다.
 * 중간 어느 단계든 실패하면 goto로 이미 등록된 것들을 역순으로 롤백한다.
 * 마지막으로 QUEUE_FLAG_INIT_DONE을 세우고 percpu_ref를 percpu 모드로
 * 전환해, 이후 SCSI/NVMe probe 실패로 인한 잦은 큐 생성/파괴가 빠르게
 * 처리되도록 만든다(원본 영어 주석 - percpu 모드 전이 자체가 상대적으로
 * 비용이 크므로 존재가 확정된 큐에서만 수행).
 * 실행 컨텍스트: 드라이버 probe -> add_disk()를 호출하는 프로세스 컨텍스트.
 * 호출자: block/genhd.c의 add_disk() (예: NVMe 네임스페이스 스캔 후 디스크 등록).
 * 피호출자: kobject_add(), blk_mq_sysfs_register(), debugfs_create_dir(),
 *   blk_mq_debugfs_register(), disk_register_independent_access_ranges(),
 *   blk_crypto_sysfs_register(), elevator_set_default(), wbt_init_enable_default(),
 *   kobject_uevent(), percpu_ref_switch_to_percpu().
 * 에러 경로: kobject_add() 실패 시 즉시 반환. 이후 각 단계 실패는
 *   out_debugfs_remove/out_unregister_ia_ranges/out_del_queue_kobj 레이블로
 *   점프해 이미 등록된 리소스를 역순으로 정리한 뒤 에러를 반환한다.
 *
 * 호출 체인:
 *   add_disk → [blk_register_queue] → blk_mq_sysfs_register →
 *     blk_mq_debugfs_register → disk_register_independent_access_ranges →
 *     blk_crypto_sysfs_register → elevator_set_default → wbt_init_enable_default
 */
/**
 * blk_register_queue - register a block layer queue with sysfs
 * @disk: Disk of which the request queue should be registered with sysfs.
 */
int blk_register_queue(struct gendisk *disk)
{
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;
	/* blk_debugfs_lock()이 반환하는 GFP 플래그 스냅샷. */
	unsigned int memflags;
	/* 각 단계의 반환값을 누적할 변수. */
	int ret;

	/*
	 * disk->queue_kobj를 disk_to_dev(disk)->kobj(디바이스 모델의 부모 kobject)
	 *   아래 "queue"라는 이름의 디렉터리로 등록 - 이 시점에 blk_queue_ktype이 정의한
	 *   default_groups(queue_attr_group 등)의 파일들이 생성되기 시작한다.
	 */
	ret = kobject_add(&disk->queue_kobj, &disk_to_dev(disk)->kobj, "queue");
	/* kobject 등록 자체가 실패하면(예: 이름 충돌, 메모리 부족) */
	if (ret < 0)
		/* 더 이상 아무 리소스도 등록하지 않았으므로 즉시 반환. */
		return ret;

	/* blk-mq(요청 기반) 큐인 경우에만 추가로 mq/ 하위 sysfs를 등록 - NVMe는 항상 이 경로. */
	if (queue_is_mq(q)) {
		/* hctx(하드웨어 디스패치 큐)별 sysfs 속성 트리 등록. */
		ret = blk_mq_sysfs_register(disk);
		/* 실패 시 */
		if (ret)
			/* queue_kobj 자체는 이미 등록되었으므로 그것만 롤백하는 out_del_queue_kobj로 이동. */
			goto out_del_queue_kobj;
	}
	/*
	 * sysfs_lock 획득 - 이후 elevator/debugfs/crypto 등록 단계 동안 동시
	 *   elv_iosched_store()(스케줄러 전환) 등의 sysfs 쓰기와 경합하지 않도록 보호.
	 */
	mutex_lock(&q->sysfs_lock);

	/*
	 * debugfs_mutex 획득(NOIO 전환 포함 - 등록 경로는 메모리 회수 재진입에 더
	 *   민감할 수 있어 blk_debugfs_remove와 달리 memflags를 저장하는 전체 버전을 사용).
	 */
	memflags = blk_debugfs_lock(q);
	/*
	 * /sys/kernel/debug/block/<disk_name> 디렉터리 생성 - blk_debugfs_root 아래에
	 *   이 큐 전용 debugfs 트리의 루트가 만들어진다.
	 */
	q->debugfs_dir = debugfs_create_dir(disk->disk_name, blk_debugfs_root);
	/* blk-mq 큐라면 */
	if (queue_is_mq(q))
		/* hctx/tags/dispatch 등 blk-mq 세부 상태 debugfs 파일들을 등록. */
		blk_mq_debugfs_register(q);
	/* debugfs_mutex 해제(등록 시 저장한 memflags로 NOIO 전환도 함께 복원). */
	blk_debugfs_unlock(q, memflags);

	/*
	 * For blk-mq rotational zoned devices, default to using QD=1
	 * writes. For non-mq rotational zoned devices, the device driver can
	 * set an appropriate default.
	 */
	/*
	 * blk-mq 기반이면서 회전형(rotational) zoned 장치인 경우 - HDD 기반 ZNS류
	 *   하이브리드 장치를 가리키는 것으로 보임(NVMe SSD는 대부분 비회전형이라 이
	 *   분기에 해당하지 않음, 추정).
	 */
	if (queue_is_mq(q) && blk_queue_rot(q) && blk_queue_is_zoned(q))
		/*
		 * 안전을 위해 zone 쓰기를 기본적으로 QD=1로 강제 - 비-mq(legacy) 회전형 zoned
		 *   장치는 드라이버가 스스로 적절한 기본값을 정하도록 맡긴다(원본 영어 주석).
		 */
		blk_queue_flag_set(QUEUE_FLAG_ZONED_QD1_WRITES, q);

	/*
	 * 독립 접근 범위(Independent Access Ranges, 예: 멀티 액추에이터 HDD나 특정
	 *   네임스페이스 파티셔닝) sysfs 트리 등록 - block/blk-ia-ranges.c 구현.
	 */
	ret = disk_register_independent_access_ranges(disk);
	/* 등록 실패 시 */
	if (ret)
		/* debugfs까지 롤백해야 하는 out_debugfs_remove로 이동. */
		goto out_debugfs_remove;

	/*
	 * blk-crypto(인라인 암호화, 하드웨어 wrapped-key 등) sysfs 속성 등록 -
	 *   drivers/nvme 등에서 crypto_profile을 설정한 경우에만 실질적 파일이 생김.
	 */
	ret = blk_crypto_sysfs_register(disk);
	/* 등록 실패 시 */
	if (ret)
		/* IA ranges까지 롤백해야 하는 out_unregister_ia_ranges로 이동. */
		goto out_unregister_ia_ranges;

	/* blk-mq 큐인 경우 */
	if (queue_is_mq(q))
		/*
		 * 기본 I/O 스케줄러 연결(none 또는 드라이버/커널 설정에 따른 mq-deadline 등) -
		 *   NVMe는 보통 대기시간이 매우 낮아 none이 기본으로 선택되는 경우가 많다(추정).
		 */
		elevator_set_default(q);

	/*
	 * 이 시점부터 이 큐가 완전히 등록되었음을 나타내는 플래그 설정 - 이후
	 *   blk_queue_registered()가 참을 반환하게 된다.
	 */
	blk_queue_flag_set(QUEUE_FLAG_REGISTERED, q);
	/*
	 * WBT(Writeback Throttle)의 기본 설치 조건이 충족되면(예: 회전형이 아니고,
	 *   스케줄러가 있고 등) 여기서 자동으로 rq_qos 체인에 WBT를 삽입.
	 */
	wbt_init_enable_default(disk);

	/*
	 * 원본 영어 주석: 이제 모든 준비가 끝났으니 KOBJ_ADD uevent를 내보낸다 -
	 *   udev 등 사용자공간이 이 시점부터 새 큐의 sysfs 트리가 완전하다고 가정할 수 있다.
	 */
	/* Now everything is ready and send out KOBJ_ADD uevent */
	/* disk->queue_kobj에 대한 KOBJ_ADD uevent 전송. */
	kobject_uevent(&disk->queue_kobj, KOBJ_ADD);
	/* 엘리베이터(스케줄러)가 실제로 붙어 있다면 */
	if (q->elevator)
		/*
		 * 그 kobj에 대해서도 별도 KOBJ_ADD uevent 전송 - /sys/block/<disk>/queue/iosched/
		 *   트리가 완성되었음을 알림.
		 */
		kobject_uevent(&q->elevator->kobj, KOBJ_ADD);
	/* sysfs_lock 해제 - 등록 단계 동안의 보호 구간 종료. */
	mutex_unlock(&q->sysfs_lock);

	/*
	 * SCSI probing may synchronously create and destroy a lot of
	 * request_queues for non-existent devices.  Shutting down a fully
	 * functional queue takes measureable wallclock time as RCU grace
	 * periods are involved.  To avoid excessive latency in these
	 * cases, a request_queue starts out in a degraded mode which is
	 * faster to shut down and is made fully functional here as
	 * request_queues for non-existent devices never get registered.
	 */
	/*
	 * 초기화 완료 플래그 설정 - 이 시점부터 이 큐가 '완전히 기능하는' 상태로 간주된다
	 *   (원본 영어 주석: SCSI 프로빙이 존재하지 않는 장치에 대해 큐를 빠르게
	 *   만들었다 부수는 경우가 많아, 그런 큐는 이 지점까지 도달하지 않고 degraded
	 *   상태로 남아 더 빠르게 정리될 수 있다).
	 */
	blk_queue_flag_set(QUEUE_FLAG_INIT_DONE, q);
	/*
	 * percpu_ref(q->q_usage_counter, 사용 중 참조 카운터)를 percpu 모드로 전환 -
	 *   percpu 카운터는 생성 비용이 있는 대신 핫 패스(각 I/O의 get/put)가 훨씬
	 *   빠르므로, 실제로 살아남을 큐에서만 이 전환을 수행한다.
	 */
	percpu_ref_switch_to_percpu(&q->q_usage_counter);

	/* 정상 경로 - 여기까지 오면 ret==0(kobject_add 성공 이후 모든 단계 성공). */
	return ret;

/*
 * out_unregister_ia_ranges: blk_crypto_sysfs_register() 실패 시 도달 -
 *   이미 등록된 IA ranges를 역순으로 해제한다.
 */
out_unregister_ia_ranges:
	/* IA ranges sysfs 등록 해제. */
	disk_unregister_independent_access_ranges(disk);
/*
 * out_debugfs_remove: disk_register_independent_access_ranges() 실패 시에도
 *   여기로 흘러 들어와(fall-through) debugfs를 정리한다.
 */
out_debugfs_remove:
	/* debugfs 트리 제거(blktrace 종료 포함). */
	blk_debugfs_remove(disk);
	/* sysfs_lock 해제 - 등록 실패 경로에서도 반드시 잠금 해제가 이뤄지도록 보장. */
	mutex_unlock(&q->sysfs_lock);
	/* blk-mq 큐였다면 */
	if (queue_is_mq(q))
		/* mq/ 하위 sysfs 등록도 해제. */
		blk_mq_sysfs_unregister(disk);
/*
 * out_del_queue_kobj: kobject_add() 이후 첫 단계(blk_mq_sysfs_register) 실패
 *   시 최초 진입하는 가장 바깥쪽 롤백 지점.
 */
out_del_queue_kobj:
	/* queue_kobj 자체를 제거 - kobject_add()로 만든 sysfs 디렉터리를 삭제. */
	kobject_del(&disk->queue_kobj);
	/* 누적된 에러 코드를 그대로 반환. */
	return ret;
}

/*
 * [한국어]
 * blk_unregister_queue - blk_register_queue()의 짝 함수: del_gendisk() 경로에서
 *   sysfs/debugfs 인터페이스를 등록의 역순으로 제거
 *
 * @disk: sysfs에서 제거할 request_queue를 보유한 gendisk.
 * @return: 없음(void).
 *
 * 원본 영어 주석대로 이 함수는 반드시 blk_register_queue()가 끝난 뒤에만
 * 호출되어야 한다. QUEUE_FLAG_REGISTERED를 먼저 지워 elv_iosched_store()
 * 같은 동시 sysfs 쓰기가 더 이상 유효한 상태 변경을 시도하지 못하게 막은
 * 뒤, mq sysfs -> crypto sysfs -> IA ranges -> queue_kobj -> elevator ->
 * debugfs 순으로 (등록의 대략적인 역순으로) 정리한다.
 * 실행 컨텍스트: 드라이버 제거/모듈 언로드 -> del_gendisk()를 호출하는
 *   프로세스 컨텍스트.
 * 호출자: block/genhd.c의 del_gendisk().
 * 피호출자: blk_queue_registered(), blk_mq_sysfs_unregister(),
 *   blk_crypto_sysfs_unregister(), disk_unregister_independent_access_ranges(),
 *   kobject_uevent(), kobject_del(), elevator_set_none(), blk_debugfs_remove().
 * 에러 경로: q가 NULL이면 WARN_ON 후 즉시 반환(호출자 버그 방지용 방어 코드).
 *   등록되지 않은 큐(blk_register_queue 실패 등)면 조용히 조기 반환.
 *
 * 호출 체인:
 *   del_gendisk → [blk_unregister_queue] → blk_mq_sysfs_unregister →
 *     blk_crypto_sysfs_unregister → disk_unregister_independent_access_ranges →
 *     kobject_del → elevator_set_none → blk_debugfs_remove
 */
/**
 * blk_unregister_queue - counterpart of blk_register_queue()
 * @disk: Disk of which the request queue should be unregistered from sysfs.
 *
 * Note: the caller is responsible for guaranteeing that this function is called
 * after blk_register_queue() has finished.
 */
void blk_unregister_queue(struct gendisk *disk)
{
	/* 이후 반복 참조할 request_queue 캐시. */
	struct request_queue *q = disk->queue;

	/*
	 * disk->queue가 NULL인 것은 호출자의 버그(등록되지 않은/이미 해제된 disk) -
	 *   커널 경고를 남긴다.
	 */
	if (WARN_ON(!q))
		/* 더 진행할 수 없으므로 즉시 반환. */
		return;

	/* Return early if disk->queue was never registered. */
	/*
	 * blk_register_queue()가 성공적으로 끝나지 않았던 큐(QUEUE_FLAG_REGISTERED
	 *   미설정)라면
	 */
	if (!blk_queue_registered(q))
		/*
		 * 아무것도 등록되지 않았으므로 조용히 반환 - 원본 영어 주석: 한 번도 등록되지
		 *   않은 disk->queue에 대한 조기 반환.
		 */
		return;

	/*
	 * Since sysfs_remove_dir() prevents adding new directory entries
	 * before removal of existing entries starts, protect against
	 * concurrent elv_iosched_store() calls.
	 */
	/*
	 * sysfs_lock 획득 - REGISTERED 플래그를 지우는 구간 동안 다른 sysfs 조작과의
	 *   경합을 막는다.
	 */
	mutex_lock(&q->sysfs_lock);
	/*
	 * REGISTERED 플래그 해제 - 원본 영어 주석: sysfs_remove_dir()는 제거 시작 전에
	 *   새 디렉터리 엔트리 추가를 막으므로, 이 플래그로 concurrent
	 *   elv_iosched_store()(스케줄러 전환 sysfs 쓰기)를 미리 차단해 둔다.
	 */
	blk_queue_flag_clear(QUEUE_FLAG_REGISTERED, q);
	/* sysfs_lock 해제. */
	mutex_unlock(&q->sysfs_lock);

	/*
	 * Remove the sysfs attributes before unregistering the queue data
	 * structures that can be modified through sysfs.
	 */
	/* blk-mq 큐였다면 */
	if (queue_is_mq(q))
		/*
		 * mq/ 하위 sysfs 등록을 해제 - 원본 영어 주석: sysfs를 통해 수정 가능한 큐
		 *   자료구조들을 등록 해제하기 전에 먼저 sysfs attribute들부터 제거해야 한다.
		 */
		blk_mq_sysfs_unregister(disk);
	/* blk-crypto sysfs 속성 해제. */
	blk_crypto_sysfs_unregister(disk);

	/* sysfs_lock 재획득 - IA ranges 해제 구간 보호. */
	mutex_lock(&q->sysfs_lock);
	/* 독립 접근 범위 sysfs 트리 해제. */
	disk_unregister_independent_access_ranges(disk);
	/* sysfs_lock 해제. */
	mutex_unlock(&q->sysfs_lock);

	/* Now that we've deleted all child objects, we can delete the queue. */
	/*
	 * disk->queue_kobj에 KOBJ_REMOVE uevent 전송 - 사용자공간에 제거를 통지
	 *   (원본 영어 주석: 모든 자식 객체를 지웠으니 이제 queue 자체를 지울 수 있다).
	 */
	kobject_uevent(&disk->queue_kobj, KOBJ_REMOVE);
	/* queue_kobj 삭제 - /sys/block/<disk>/queue 디렉터리 자체가 사라진다. */
	kobject_del(&disk->queue_kobj);

	if (queue_is_mq(q))
	/* blk-mq 큐였다면 */
		/*
		 * 엘리베이터를 none으로 전환해 스케줄러 내부 상태(elevator_tags 등)를 정리 -
		 *   큐가 완전히 사라지기 전에 스케줄러 리소스를 명시적으로 반납.
		 */
		elevator_set_none(q);

	/*
	 * debugfs 트리 제거(blktrace 종료 포함) - 등록의 첫 단계였던 debugfs 생성과
	 *   대응되는 마지막 정리 단계.
	 */
	blk_debugfs_remove(disk);
}
