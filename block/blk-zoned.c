// SPDX-License-Identifier: GPL-2.0
/*
 * Zoned block device handling
 *
 * Copyright (c) 2015, Hannes Reinecke
 * Copyright (c) 2015, SUSE Linux GmbH
 *
 * Copyright (c) 2016, Damien Le Moal
 * Copyright (c) 2016, Western Digital
 * Copyright (c) 2024, Western Digital Corporation or its affiliates.
 */
/*
 * [한국어 설명] Zoned Block Device(ZBD) 관리 - zone report/관리 명령/zone write plug (blk-zoned.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 zone 단위로 순차 쓰기(sequential write)만 허용하는 zoned block
 * device(ZBD) — NVMe ZNS(Zoned Namespace), SMR(Shingled Magnetic Recording)
 * HDD 등 — 를 블록 계층에서 공통으로 다루는 SW(소프트웨어) 계층이다. zone
 * 정보 조회(blkdev_report_zones, Report Zones 커맨드), zone 관리 명령
 * (REQ_OP_ZONE_RESET/OPEN/CLOSE/FINISH/RESET_ALL을 통한 Zone Management
 * Send 커맨드), 그리고 이 파일의 핵심 개념인 zone write plug(struct
 * blk_zone_wplug)를 구현한다. zone write plug는 zone마다 하나씩 존재하는
 * per-zone 큐로, 동시에 여러 write BIO가 도착해도 write pointer(WP) 순서를
 * 어기지 않도록 BIO를 직렬화(plug)하고, zone append 커맨드를 native로
 * 지원하지 않는 디바이스에서는 zone append를 일반 write로 에뮬레이션하는
 * 역할도 겸한다. 즉 "디바이스가 반드시 순차적으로만 써야 하는" 제약을
 * host(커널) 쪽에서 미리 강제함으로써, 잘못된 순서의 쓰기가 NVMe SQ(제출
 * 큐)/CID(커맨드 식별자)를 낭비하며 디바이스까지 도달해 실패하는 것을
 * 방지한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 대부분 커널 블록 계층(호스트 커널 스레드/워크큐/제출자
 * 컨텍스트)이며, GPU나 유저스페이스 코드는 포함하지 않는다. 데이터(쓰기)
 * 경로 관점에서는: 사용자/파일시스템이 write 또는 REQ_OP_ZONE_APPEND BIO를
 * 제출 -> submit_bio_noacct() -> blk_zone_plug_bio()(이 파일)가 zone write
 * plug에 태워 순서를 강제 -> plug가 풀리면(즉시 또는 workqueue/kthread를
 * 통해 지연) 원래의 제출 경로로 복귀 -> blk_mq_submit_bio() ->
 * (NVMe라면) nvme_queue_rq() -> nvme_submit_cmd()가 SQ에 커맨드를 채우고
 * 도어벨(doorbell)을 울린다. 관리 명령 경로는: ioctl(BLKRESETZONE 등)이나
 * blkdev_zone_mgmt() 호출자가 REQ_OP_ZONE_* BIO를 만들어
 * submit_bio_wait()로 동기 제출 -> 완료 시 blk_zone_mgmt_bio_endio()(이
 * 파일)가 캐시된 WP/zone condition을 갱신한다. zone 정보 조회 경로는:
 * blkdev_report_zones()/blkdev_report_zones_cached()가
 * disk->fops->report_zones (예: NVMe의 nvme_report_zones)를 호출하거나,
 * 커널이 캐시한 zone condition/WP만으로 응답한다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 block/blk-mq.c(요청 기반 제출/완료 경로), block/blk-mq.h,
 * block/blk.h(gendisk/request_queue 내부 정의), block/blk-mq-sched.h,
 * block/blk-mq-debugfs.h(디버그 파일시스템 노출)에 의존한다. 반대로 이
 * 파일이 노출하는 blk_zone_plug_bio(), blk_zone_write_plug_init_request(),
 * blk_zone_write_plug_bio_endio(), blk_zone_write_plug_finish_request(),
 * blk_zone_mgmt_bio_endio(), blk_zone_append_update_request_bio() 등은
 * block/blk-mq.c와 block/bio.c의 BIO/요청 제출·병합·완료 경로에서 호출되어
 * 이 파일에 의존한다. NVMe ZNS 드라이버(drivers/nvme/host/zns.c)는
 * disk->fops->report_zones를 구현해 이 파일의 Report Zones 요청에 응답하고,
 * 이 파일이 관리하는 WP 규칙(REQ_OP_ZONE_APPEND/WRITE의 정렬 요구사항)을
 * 준수하는 커맨드만 받게 된다. 데이터 흐름 관점에서 핵심 공유 자료구조는
 * gendisk에 매달린 zone_wplugs_hash(zone 번호로 조회하는 plug 해시
 * 테이블), zones_cond(RCU로 보호되는 zone condition 캐시 배열),
 * zone_wplugs_pool/zone_wplugs_wq/zone_wplugs_worker(plug 메모리 풀,
 * 비동기 제출용 workqueue, 회전형 매체를 위한 단일 워커 스레드)이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blk_zone_plug_bio(): zone-aware BIO 제출의 총진입점. write/zone
 *   append/write-zeroes는 plug 경로로, zone 관리 명령은 별도 경로로 분기.
 * - blk_zone_wplug_handle_write(): 실제 zone write plug 상태 기계의 핵심 —
 *   plug 할당/검색, WP 정렬 검증, 즉시 제출 또는 대기열 적재를 결정.
 * - disk_zone_wplug_submit_bio(): plug에 쌓인 BIO를 하나씩 꺼내 WP 규칙을
 *   재검증한 뒤 실제 제출 경로(blk_mq_submit_bio 등)로 넘긴다.
 * - blkdev_report_zones()/blkdev_report_zones_cached(): 디바이스 Report
 *   Zones 커맨드를 직접 호출하거나 커널 캐시로 대체 응답한다.
 * - blkdev_zone_mgmt()/blk_revalidate_disk_zones(): zone 관리 명령 실행과
 *   probe/재검증 시 zone 레이아웃 전체 검증 및 자원(해시/풀/워커) 초기화.
 * - struct blk_zone_wplug: zone 하나당 하나씩 존재하는 write plug. 대기
 *   BIO 목록, WP 오프셋, zone condition, 참조 카운트, 동기화 락을 가진다.
 * - struct blk_report_zones_args / struct blk_revalidate_zone_args: 각각
 *   Report Zones 콜백 전달용, 그리고 재검증(revalidate) 진행 상태 전달용
 *   임시 인자 구조체.
 */

#include <linux/kernel.h>
/* [한국어] pr_warn/ARRAY_SIZE 등 커널 기본 매크로 - zone 오류 로깅과 배열 크기 계산에 사용 */
#include <linux/blkdev.h>
/* [한국어] struct gendisk, struct block_device, bdev_* 헬퍼 등 블록 계층 핵심 타입 - 이 파일 전체가 이 헤더의 zone 관련 선언(blk_zone_wplug 등은 blk.h)에 의존 */
#include <linux/blk-mq.h>
/* [한국어] blk_mq_submit_bio/blk_mq_freeze_queue 등 blk-mq(멀티큐 블록 계층) API - request 기반 NVMe 장치의 제출/freeze에 사용 */
#include <linux/spinlock.h>
/* [한국어] spinlock_t, spin_lock_irqsave 등 - zone write plug 및 해시 테이블의 동시 접근 직렬화에 사용 */
#include <linux/refcount.h>
/* [한국어] refcount_t, refcount_inc/dec_and_test - zone write plug 생명주기(참조 카운트) 관리에 사용 */
#include <linux/mempool.h>
/* [한국어] mempool_alloc/mempool_create_kmalloc_pool - zone write plug를 GFP_NOIO 상황에서도 안전하게 할당하기 위한 bounded 메모리 풀 */
#include <linux/kthread.h>
/* [한국어] kthread_create/kthread_stop - 회전형(qd1 writes) 매체를 위한 zone write plug 전용 커널 스레드 생성/종료 */
#include <linux/freezer.h>
/* [한국어] set_freezable/try_to_freeze - zone plug 워커 스레드가 시스템 suspend/freeze 대상에 포함되도록 등록 */

#include <trace/events/block.h>
/* [한국어] trace_blkdev_zone_mgmt 등 블록 계층 트레이스포인트 정의 - ftrace/perf로 zone 관리 명령과 plug 이벤트를 관찰하기 위함 */

#include "blk.h"
/* [한국어] 블록 계층 내부(비공개) 선언 - disk_zone_no, bdev_offset_from_zone_start 등 zone 산술 헬퍼와 gendisk 내부 필드 접근에 필요 */
#include "blk-mq-sched.h"
/* [한국어] I/O 스케줄러(mq-deadline 등) 연동 선언 - blk_mq_sched 훅과의 상호작용(요청 병합 등)에 필요 */
#include "blk-mq-debugfs.h"
/* [한국어] blk-mq debugfs 인프라 선언 - 이 파일 하단의 queue_zone_wplugs_show()가 CONFIG_BLK_DEBUG_FS 하에서 사용 */

#define ZONE_COND_NAME(name) [BLK_ZONE_COND_##name] = #name
/* [한국어] BLK_ZONE_COND_FOO -> 문자열 리터럴 "FOO" 로 변환하는 매크로.
 * 지정 배열 초기화 문법([인덱스] = 값)을 이용해 enum 값을 그대로 배열
 * 인덱스로 사용하므로, enum 정의 순서가 바뀌어도 매핑이 깨지지 않는다.
 * 아래 zone_cond_name[] 초기화에서만 사용되고 바로 #undef 된다. */
static const char *const zone_cond_name[] = {
/* [한국어] enum blk_zone_cond 값 -> 사람이 읽을 수 있는 문자열 매핑 테이블.
 * blk_zone_cond_str()이 dmesg/트레이스 출력에 사용한다. ZNS(Zoned
 * Namespace) Report Zones 응답의 Zone State 필드 값과 1:1 대응. */
	ZONE_COND_NAME(NOT_WP), // conventional zone (WP 개념 없음)
	ZONE_COND_NAME(EMPTY), // WP가 zone 시작에 위치 (아직 쓰기 없음)
	ZONE_COND_NAME(IMP_OPEN), // implicit open: write에 의해 암묵적으로 open된 상태
	ZONE_COND_NAME(EXP_OPEN), // explicit open: REQ_OP_ZONE_OPEN에 의해 명시적으로 open된 상태
	ZONE_COND_NAME(CLOSED), // open이었다가 close된 상태 (WP는 유지)
	ZONE_COND_NAME(READONLY), // 디바이스 정책상 읽기 전용으로 전환된 zone
	ZONE_COND_NAME(FULL), // WP가 zone capacity에 도달해 더 이상 쓸 수 없음
	ZONE_COND_NAME(OFFLINE), // 디바이스가 오프라인으로 표시한 zone (접근 불가)
	ZONE_COND_NAME(ACTIVE), // cached report에서 open 계열(imp/exp open, closed)을 뭉뚱그린 상태
};
#undef ZONE_COND_NAME
/* [한국어] 배열 초기화 종료 후 매크로를 해제 - 다른 파일/뒤 코드에서 동일 이름 매크로와 충돌하지 않도록 네임스페이스 정리 */

/*
 * [한국어]
 * NVMe ZNS 관점에서 struct blk_zone_wplug 는 하나의 zone 단위로
 * write pointer(WP)와 zone state를 SW 층에서 캐시/직렬화하는 객체다.
 * NVMe 컨트롤러는 Zone Identifier(ZID)별로 WP를 유지하지만, 이 구조체는
 * 커널 내에서 동일 zone에 대한 다중 BIO 흐름을 순차 쓰기 규칙에 맞게
 * 정렬하고, zone append 에뮬레이션 시 WP 위치를 결정한다. 아래 각 필드
 * 주석에서 설정자/읽는 자/값 범위/동기화 방식을 상세히 설명한다.
 */
/*
 * Per-zone write plug.
 * @node: hlist_node structure for managing the plug using a hash table.
 * @entry: list_head structure for listing the plug in the disk list of active
 *         zone write plugs.
 * @bio_list: The list of BIOs that are currently plugged.
 * @bio_work: Work struct to handle issuing of plugged BIOs
 * @rcu_head: RCU head to free zone write plugs with an RCU grace period.
 * @disk: The gendisk the plug belongs to.
 * @lock: Spinlock to atomically manipulate the plug.
 * @ref: Zone write plug reference counter. A zone write plug reference is
 *       always at least 1 when the plug is hashed in the disk plug hash table.
 *       The reference is incremented whenever a new BIO needing plugging is
 *       submitted and when a function needs to manipulate a plug. The
 *       reference count is decremented whenever a plugged BIO completes and
 *       when a function that referenced the plug returns. The initial
 *       reference is dropped whenever the zone of the zone write plug is reset,
 *       finished and when the zone becomes full (last write BIO to the zone
 *       completes).
 * @flags: Flags indicating the plug state.
 * @zone_no: The number of the zone the plug is managing.
 * @wp_offset: The zone write pointer location relative to the start of the zone
 *             as a number of 512B sectors.
 * @cond: Condition of the zone
 */
struct blk_zone_wplug {
	struct hlist_node	node;
	/* [한국어] 이 zone plug를 disk->zone_wplugs_hash[] 해시 체인에 연결하는 노드.
	 * 설정자: disk_insert_zone_wplug()가 hlist_add_head_rcu()로 삽입.
	 * 읽는 자: disk_get_hashed_zone_wplug()가 hlist_for_each_entry_rcu()로
	 * ZID(zone_no)가 일치하는 plug를 RCU read-side에서 lock-free 탐색.
	 * 값 범위: 해시에 없을 때는 hlist_del_init_rcu()로 초기화된 상태(빈 노드).
	 * 동기화: 삽입/삭제는 disk->zone_wplugs_hash_lock으로 보호되고, 탐색은
	 * RCU read lock만으로 안전 — writer가 unlink해도 grace period 동안
	 * reader가 계속 순회할 수 있다(disk_free_zone_wplug_rcu에서 최종 free). */

	struct list_head	entry;
	/* [한국어] 회전형(SMR HDD 등) qd1(queue depth 1) write 장치를 위한 전역
	 * active plug 리스트(disk->zone_wplugs_list) 노드.
	 * 설정자: disk_zone_wplug_add_bio()가 blk_queue_zoned_qd1_writes()인
	 * 장치에서 list_add_tail()로 추가; 비어있음은 INIT_LIST_HEAD()로 표시.
	 * 읽는 자: disk_zone_wplugs_worker() 단일 커널 스레드가
	 * disk_get_zone_wplugs_work()로 순서대로 꺼내 zone별 BIO를 처리.
	 * 값 범위: 리스트에 없을 때는 list_empty()가 참인 상태로 유지.
	 * 동기화: disk->zone_wplugs_list_lock spinlock으로 보호되며, NVMe ZNS
	 * SSD처럼 여러 zone을 병렬 처리할 수 있는 장치는 이 리스트를 쓰지 않고
	 * per-plug bio_work(workqueue)를 사용한다. */

	struct bio_list		bio_list;
	/* [한국어] 아직 디바이스로 제출하지 않고 WP 순서를 기다리는 BIO 연결
	 * 리스트(선입선출).
	 * 설정자: disk_zone_wplug_add_bio()가 bio_list_add()로 tail에 추가;
	 * disk_zone_wplug_submit_bio()/disk_zone_wplug_abort()가
	 * bio_list_pop()으로 제거.
	 * 읽는 자: blk_zone_write_plug_init_request()가 bio_list_peek()으로
	 * 다음 BIO를 미리 확인해 진행 중인 request에 back merge를 시도한다.
	 * 값 범위: 비어 있으면 bio_list_empty()가 참 — 이 경우 PLUGGED 플래그를
	 * 클리어할 수 있다.
	 * 동기화: 반드시 zwplug->lock을 쥔 상태에서만 조작 — 여러 제출자가
	 * 동시에 같은 zone에 write할 때 순서가 꼬이지 않게 하는 핵심 자료구조. */

	struct work_struct	bio_work;
	/* [한국어] plug가 풀릴 때 다음 BIO를 비동기로 제출하기 위한 workqueue
	 * 작업 항목. blk_zone_wplug_bio_work()가 실제 콜백 함수다.
	 * 설정자: disk_get_or_alloc_zone_wplug()가 plug 최초 할당 시
	 * INIT_WORK()로 초기화.
	 * 읽는 자: disk_zone_wplug_schedule_work()가 disk->zone_wplugs_wq에
	 * queue_work()로 스케줄; 실행되면 disk_zone_wplug_submit_bio() ->
	 * blk_mq_submit_bio() -> nvme_queue_rq()로 이어져 실제 NVMe SQ에
	 * 커맨드가 들어간다.
	 * 값 범위: process context에서만 실행되는 콜백 — 인터럽트 컨텍스트가
	 * 아니므로 GFP_KERNEL/mutex 등을 안전하게 쓸 수 있다.
	 * 동기화: workqueue 코어가 중복 스케줄을 막아주며, 스케줄 시 plug
	 * refcount를 미리 증가시켜 work 실행 전에 plug가 해제되지 않게 한다. */

	struct rcu_head		rcu_head;
	/* [한국어] RCU grace period 경과 후 plug 메모리를 mempool에 반환하기
	 * 위한 콜백 헤더.
	 * 설정자: disk_free_zone_wplug()가 call_rcu()에 등록.
	 * 읽는 자: RCU 코어가 grace period 종료 후 disk_free_zone_wplug_rcu()를
	 * 호출 — 이 함수가 container_of()로 plug 포인터를 복원해 mempool_free().
	 * 값 범위: DEAD 플래그가 설정되고 마지막 참조가 해제된 이후에만 유효한
	 * 콜백 — 그 전에는 이 필드가 아직 쓰이지 않는다.
	 * 동기화: RCU 자체가 동기화 메커니즘 — 해시에서 unlink된 후에도 이미
	 * RCU read-side에 있던 reader는 안전하게 계속 접근 가능하다. */

	struct gendisk		*disk;
	/* [한국어] 이 plug가 속한 NVMe namespace(또는 SMR 디스크)의 gendisk
	 * 역참조 포인터.
	 * 설정자: disk_get_or_alloc_zone_wplug()가 할당 시 1회 설정 후 불변.
	 * 읽는 자: 거의 모든 plug 조작 함수(disk_zone_wplug_abort,
	 * disk_zone_wplug_schedule_work 등)가 disk->queue, disk->zone_capacity
	 * 등 disk 전역 상태에 접근하기 위해 사용.
	 * 값 범위: 유효한 gendisk 포인터 — NULL 불가.
	 * 동기화: plug 생존 기간 내내 불변이므로 별도 락 없이 읽기만 하면 안전. */

	spinlock_t		lock;
	/* [한국어] 이 zone plug 하나의 상태(flags/wp_offset/cond/bio_list)를
	 * 원자적으로 조작하기 위한 zone 단위 spinlock.
	 * 설정자: disk_get_or_alloc_zone_wplug()가 spin_lock_init()으로 초기화.
	 * 읽는 자/쓰는 자: blk_zone_wplug_handle_write(),
	 * disk_zone_wplug_submit_bio() 등 이 파일의 거의 모든 함수가
	 * spin_lock_irqsave(&zwplug->lock, flags)로 임계구역을 보호.
	 * 값 범위: 잠금/해제 상태만 존재.
	 * 동기화: 동일 zone에 대한 여러 CPU의 동시 write 요청이 WP를 놓고
	 * 경쟁하는 것을 막는 이 구조체의 핵심 동기화 지점. 인터럽트 컨텍스트
	 * (BIO 완료 콜백)에서도 잡히므로 irqsave 계열만 사용해야 한다. */

	refcount_t		ref;
	/* [한국어] zone plug의 생명주기를 관리하는 참조 카운터.
	 * 설정자: disk_get_or_alloc_zone_wplug()가 refcount_set(2)로 초기화
	 * (해시 테이블 자신의 몫 1 + 현재 호출자 몫 1); 이후
	 * disk_get_hashed_zone_wplug()의 refcount_inc_not_zero(),
	 * disk_zone_wplug_schedule_work()/disk_zone_wplug_add_bio()의
	 * refcount_inc()로 증가.
	 * 읽는 자: disk_put_zone_wplug()가 refcount_dec_and_test()로 감소시켜
	 * 0이 되면 disk_free_zone_wplug()를 호출.
	 * 값 범위: 해시에 있는 동안 최소 1 이상; DEAD 플래그가 설정되며
	 * hash상의 몫이 반환되면 이후로는 활성 사용자 수만 반영.
	 * 동기화: atomic 연산 자체가 lock-free 동기화 수단 — RCU read-side에서
	 * 찾은 plug가 이미 해제 중인지(0으로 감) 판별하는 데도 쓰인다. */

	unsigned int		flags;
	/* [한국어] BLK_ZONE_WPLUG_PLUGGED/NEED_WP_UPDATE/DEAD 비트 플래그 조합.
	 * 설정자/읽는 자: 이 파일 전역의 plug 상태 기계 함수들
	 * (blk_zone_wplug_handle_write, disk_zone_wplug_set_wp_offset,
	 * disk_mark_zone_wplug_dead 등)이 zwplug->lock을 쥔 채로 비트 연산.
	 * 값 범위: 0(플래그 없음)부터 세 비트 조합까지 — 자세한 의미는 아래
	 * BLK_ZONE_WPLUG_* 매크로 정의부 주석 참고.
	 * 동기화: 반드시 zwplug->lock 보유 상태에서만 읽고 써야 하는 필드 —
	 * lockdep_assert_held(&zwplug->lock)로 다수의 함수에서 강제된다. */

	unsigned int		zone_no;
	/* [한국어] 이 plug가 관리하는 zone의 번호(NVMe ZNS의 Zone Identifier와
	 * 대응, 0부터 시작하는 순번).
	 * 설정자: disk_get_or_alloc_zone_wplug()가 disk_zone_no()로 sector
	 * 로부터 계산해 할당 시 1회 설정 후 불변.
	 * 읽는 자: 해시 버킷 탐색 시 동일 zone인지 비교(disk_insert_zone_wplug,
	 * disk_get_hashed_zone_wplug)하고, trace 이벤트와 debugfs 출력에도 사용.
	 * 값 범위: 0 이상 disk->nr_zones 미만.
	 * 동기화: 불변 필드라 락 없이 읽어도 안전. */

	unsigned int		wp_offset;
	/* [한국어] zone 시작 sector로부터 현재 write pointer(WP)까지의 거리를
	 * 512바이트 섹터 단위로 나타낸 값 — NVMe ZNS Zone Descriptor의 wp
	 * 필드를 zone 시작 기준 상대값으로 SW에서 미러링한 것.
	 * 설정자: blk_zone_wplug_prepare_bio()/blk_zone_write_plug_bio_merged()
	 * 등이 BIO 크기만큼 전진(+=)시키고, disk_zone_wplug_set_wp_offset()이
	 * reset/finish/report 결과에 맞춰 절대값으로 재설정.
	 * 읽는 자: disk_zone_wplug_is_full()이 zone_capacity와 비교해 FULL
	 * 여부 판정, blkdev_get_zone_info()가 실제 WP(sector 단위)를 사용자에
	 * 보고할 때 zone 시작 sector에 더함.
	 * 값 범위: 0(EMPTY) ~ zone_capacity/last_zone_capacity(FULL).
	 * 동기화: zwplug->lock 보유 상태에서만 갱신 — host 측이 디바이스보다
	 * 먼저 WP를 예측(mirror)하므로 doorbell 전에 다음 write의 정렬 여부를
	 * 판단할 수 있다. */

	enum blk_zone_cond	cond;
	/* [한국어] 캐시된 zone condition(EMPTY/IMP_OPEN/EXP_OPEN/CLOSED/
	 * FULL/ACTIVE 등) — NVMe ZNS Report Zones 응답의 Zone State 필드를
	 * 미러링한 값.
	 * 설정자: disk_zone_wplug_update_cond()가 wp_offset 변화에 따라
	 * EMPTY/FULL/ACTIVE로 갱신; disk_insert_zone_wplug()가 최초 삽입 시
	 * zones_cond 배열 값이나 ACTIVE로 초기화.
	 * 읽는 자: blkdev_get_zone_info()가 zone 정보 조회 시 그대로 노출;
	 * disk_free_zone_wplug()가 plug 소멸 시 disk->zones_cond 배열로
	 * 값을 되돌려 캐시 일관성을 유지.
	 * 값 범위: enum blk_zone_cond의 zone_cond_name[]에 매핑되는 값들.
	 * 동기화: zwplug->lock 보유 상태에서만 갱신. */
};

/*
 * [한국어]
 * disk_need_zone_resources - 이 디스크가 zone write plug 자원을 필요로 하는가
 *
 * @disk: 검사할 gendisk.
 * @return: true면 zone write plug 해시/mempool/workqueue 등을 할당해야 함,
 *          false면 zone write plug 없이도 순차 쓰기를 보장할 수 있는 장치.
 *
 * blk-mq(멀티큐 블록 계층) 기반 request 방식 zoned 디바이스는 여러 CPU/
 * 컨텍스트에서 동시에 요청을 만들 수 있어 블록 계층이 zone write plug로
 * write BIO를 자동 직렬화해야 한다. 반면 BIO 기반 드라이버(예: device
 * mapper의 dm-zoned)는 대개 자기 자신의 zone 순서 관리 로직을 이미 갖추고
 * 있으므로 plug가 필요 없다 — 단, 그런 드라이버라도 zone append 커맨드를
 * 자체적으로 지원하지 못해 커널이 에뮬레이션(queue_emulates_zone_append)
 * 해줘야 한다면 plug가 필요하다.
 * 실행 컨텍스트: disk 초기화/재검증 경로에서 호출되는 단순 조회 함수 —
 * 동시성/재진입 문제 없음(부작용 없는 순수 판정).
 * 호출자: disk_revalidate_zone_resources().
 * 피호출자: queue_is_mq(), queue_emulates_zone_append() (둘 다 queue
 * flag/limits를 읽기만 함).
 *
 * 호출 체인:
 *   disk_revalidate_zone_resources() → [disk_need_zone_resources]
 */
static inline bool disk_need_zone_resources(struct gendisk *disk)
{
	/*
	 * All request-based zoned devices need zone resources so that the
	 * block layer can automatically handle write BIO plugging. BIO-based
	 * device drivers (e.g. DM devices) are normally responsible for
	 * handling zone write ordering and do not need zone resources, unless
	 * the driver requires zone append emulation.
	 */
	return queue_is_mq(disk->queue) ||
		queue_emulates_zone_append(disk->queue); // zone append emulation path: DM이나 SW ZNS emulation에서 사용
	// request 기반 NVMe ZNS 또는 zone append 에뮬레이션 필요 시 plug 사용
}

/*
 * [한국어]
 * disk_zone_wplugs_hash_size - zone write plug 해시 테이블의 버킷 개수
 *
 * @disk: 대상 gendisk.
 * @return: disk->zone_wplugs_hash_bits 비트 수에 대응하는 2의 거듭제곱 개수
 *          (해시 버킷 배열의 원소 수).
 *
 * disk->zone_wplugs_hash는 gendisk마다 한 번 disk_alloc_zone_resources()에서
 * 크기가 결정되는 hlist_head 배열이며, 이 함수는 그 크기를 비트 시프트로
 * 계산해 여러 호출자가 매번 동일한 공식을 반복하지 않도록 한다.
 * 실행 컨텍스트: 어떤 컨텍스트에서도 호출 가능한 순수 계산 — 락 불필요.
 * 호출자: disk_zone_wplugs_worker(), disk_alloc_zone_resources(),
 * disk_destroy_zone_wplugs_hash_table(), blk_zone_reset_all_bio_endio(),
 * queue_zone_wplugs_show() 등 해시 테이블 전체를 순회하는 모든 함수.
 * 피호출자: 없음(단순 산술).
 *
 * 호출 체인:
 *   (해시 전체 순회가 필요한 다수 함수) → [disk_zone_wplugs_hash_size]
 */
static inline unsigned int disk_zone_wplugs_hash_size(struct gendisk *disk)
{
	return 1U << disk->zone_wplugs_hash_bits; // ZID -> zone plug 빠른 lookup을 위한 hash bucket 수
}

/*
 * Zone write plug flags bits:
 *  - BLK_ZONE_WPLUG_PLUGGED: Indicates that the zone write plug is plugged,
 *    that is, that write BIOs are being throttled due to a write BIO already
 *    being executed or the zone write plug bio list is not empty.
 *  - BLK_ZONE_WPLUG_NEED_WP_UPDATE: Indicates that we lost track of a zone
 *    write pointer offset and need to update it.
 *  - BLK_ZONE_WPLUG_DEAD: Indicates that the zone write plug will be
 *    removed from the disk hash table of zone write plugs when the last
 *    reference on the zone write plug is dropped. If set, this flag also
 *    indicates that the initial extra reference on the zone write plug was
 *    dropped, meaning that the reference count indicates the current number of
 *    active users (code context or BIOs and requests in flight). This flag is
 *    set when a zone is reset, finished or becomes full.
 */
#define BLK_ZONE_WPLUG_PLUGGED		(1U << 0) // 해당 zone에 진행/대기 중인 write가 있음 -> NVMe WP 순서 직렬화
#define BLK_ZONE_WPLUG_NEED_WP_UPDATE	(1U << 1) // write 오류 등으로 WP를 잃음; 다음 Report Zones로 복구 필요
#define BLK_ZONE_WPLUG_DEAD		(1U << 2) // zone이 full/reset/finish 상태; 추가 write/CID 할당 차단
/*
 * 플래그 NVMe 의미:
 *   PLUGGED:          해당 zone에 아직 처리 중인 write가 있어 다음 BIO가
 *                     NVMe SQ 진입을 대기함.
 *   NEED_WP_UPDATE:   write 오류 등으로 WP를 잃어버림. NVMe Report Zones
 *                     또는 reset/finish로 복구해야 한다.
 *   DEAD:             zone이 full/empty/reset/finish 상태가 되어 plug가
 *                     곧 해제됨. 새로운 write는 실패한다.
 */

/**
 * blk_zone_cond_str - Return a zone condition name string
 * @zone_cond: a zone condition BLK_ZONE_COND_name
 *
 * Convert a BLK_ZONE_COND_name zone condition into the string "name". Useful
 * for the debugging and tracing zone conditions. For an invalid zone
 * conditions, the string "UNKNOWN" is returned.
 */
/*
 * [한국어]
 * blk_zone_cond_str - zone condition 값을 사람이 읽을 수 있는 문자열로 변환
 *
 * @zone_cond: 변환할 BLK_ZONE_COND_* 값(NVMe ZNS Report Zones 응답의
 *             Zone State 필드에 대응).
 * @return: "EMPTY"/"IMP_OPEN"/"FULL" 등 zone_cond_name[] 테이블의 문자열
 *          리터럴. 범위를 벗어나거나 테이블에 이름이 없으면 "UNKNOWN".
 *
 * dmesg 경고 로그나 ftrace 트레이스포인트에 숫자 대신 사람이 읽을 수 있는
 * 이름을 넣기 위한 순수 조회 함수. 배열 인덱스 범위와 NULL 항목을 모두
 * 검사해 잘못된 값이 들어와도 커널을 오염시키지 않고 안전하게 "UNKNOWN"을
 * 반환한다. 실행 컨텍스트에 제약이 없다(부작용 없음, 재진입 안전 —
 * 반환하는 static 문자열은 읽기 전용 룩업 테이블일 뿐 갱신되는 상태가
 * 아니므로 동시 호출도 안전).
 * 호출자: queue_zone_wplug_show()(debugfs), trace 이벤트 포맷터, 그 외 zone
 * condition을 로그에 남기는 모든 코드(EXPORT_SYMBOL_GPL로 드라이버에도 공개).
 * 피호출자: ARRAY_SIZE()(컴파일 타임 배열 크기 계산 매크로)만 사용.
 * 에러 경로: 별도 에러 반환 없이 "UNKNOWN" 문자열로 대체.
 *
 * 호출 체인:
 *   (트레이스/디버그 출력 각지) → [blk_zone_cond_str]
 */
const char *blk_zone_cond_str(enum blk_zone_cond zone_cond)
{
	static const char *zone_cond_str = "UNKNOWN"; // 기본 unknown 문자열; ZNS state 값 범위 검증 후 덮어씀

	if (zone_cond < ARRAY_SIZE(zone_cond_name) && zone_cond_name[zone_cond]) // ZNS Zone State 값의 범위 및 NULL 검증
		zone_cond_str = zone_cond_name[zone_cond]; // dmesg/trace에서 ZNS zone condition 문자열로 변환

	return zone_cond_str;
}
EXPORT_SYMBOL_GPL(blk_zone_cond_str);

/*
 * [한국어]
 * blk_zone_set_cond - zone condition 캐시 배열의 한 원소를 갱신
 *
 * @zones_cond: 갱신할 zone condition 캐시 배열(zone마다 1바이트, 아직 없으면
 *              NULL일 수 있음).
 * @zno: 갱신할 zone의 번호(배열 인덱스).
 * @cond: 새로 반영할 condition 값.
 * @return: 없음(void).
 *
 * zones_cond 배열은 메모리를 아끼기 위해 zone 하나에 1바이트만 쓰므로,
 * IMP_OPEN/EXP_OPEN/CLOSED처럼 "열려 있다"는 의미로 묶을 수 있는 상태들을
 * 모두 ACTIVE 하나로 축소해 저장한다(cached report 시 다시 open 계열로
 * 세분화하지 않고 뭉뚱그려 응답). 반면 conventional(NOT_WP)/EMPTY/FULL/
 * OFFLINE/READONLY는 그대로 저장해 정확히 구분한다.
 * 실행 컨텍스트: 호출자가 이미 disk->zone_wplugs_hash_lock을 쥐고 있거나
 * RCU read-side인 경우가 대부분 — 이 함수 자체는 배열 인덱스 하나만
 * 건드리는 짧은 임계구역이라 재진입에 안전(락은 호출자 책임).
 * 호출자: disk_zone_set_cond(), disk_insert_zone_wplug(),
 * disk_free_zone_wplug(), blk_revalidate_zone_cond().
 * 피호출자: 없음(단순 배열 대입).
 * 에러 경로: zones_cond가 NULL이면(재검증 초기 등 아직 배열이 없는 시점)
 * 아무 것도 하지 않고 조용히 반환.
 *
 * 호출 체인:
 *   disk_zone_set_cond() / disk_insert_zone_wplug() / disk_free_zone_wplug()
 *   / blk_revalidate_zone_cond() → [blk_zone_set_cond]
 */
static void blk_zone_set_cond(u8 *zones_cond, unsigned int zno,
			      enum blk_zone_cond cond)
{
	if (!zones_cond) // zones_cond 배열이 아직 없으면 아무것도 하지 않음 (초기화 경로)
		return;

	switch (cond) {
	case BLK_ZONE_COND_IMP_OPEN:
	case BLK_ZONE_COND_EXP_OPEN:
	case BLK_ZONE_COND_CLOSED:
		zones_cond[zno] = BLK_ZONE_COND_ACTIVE; // implicit/explicit open/closed 상태를 active로 축소하여 cached report 단순화
		return;
	case BLK_ZONE_COND_NOT_WP:
	case BLK_ZONE_COND_EMPTY:
	case BLK_ZONE_COND_FULL:
	case BLK_ZONE_COND_OFFLINE:
	case BLK_ZONE_COND_READONLY:
	default:
		zones_cond[zno] = cond; // conventional/empty/full/offline/readonly는 그대로 유지
		return;
	}
}

/*
 * [한국어]
 * disk_zone_set_cond - sector가 속한 zone의 condition 캐시를 갱신
 *
 * @disk: 대상 gendisk.
 * @sector: condition을 갱신할 zone에 속하는 임의의 sector(보통 zone 시작
 *          sector).
 * @cond: 새로 반영할 condition.
 * @return: 없음(void).
 *
 * zone write plug가 없는 zone(zone plug를 만들 필요가 없는 conventional
 * zone이거나, 이미 FULL/EMPTY라 plug가 해제된 zone)의 condition은 이 함수를
 * 거쳐 disk->zones_cond 캐시 배열에 직접 반영된다. conventional/readonly/
 * offline zone은 상태가 절대 바뀌지 않는 디바이스 특성이므로 갱신을 건너뛴다.
 * 실행 컨텍스트: BIO 완료 콜백(인터럽트 또는 softirq 문맥일 수 있음) 등
 * 다양한 컨텍스트에서 호출되므로 RCU read lock + zone_wplugs_hash_lock으로
 * zones_cond 포인터 교체(재검증 중 revalidate)와 안전하게 경쟁한다.
 * 호출자: blk_zone_reset_bio_endio(), blk_zone_reset_all_bio_endio(),
 * blk_zone_finish_bio_endio() — 모두 zone 관리 명령 완료 후 캐시 동기화.
 * 피호출자: disk_zone_no()(sector→ZID 변환), blk_zone_set_cond()(실제 대입).
 * 에러 경로: zones_cond가 아직 없으면(재검증 미완료) 아무 것도 하지 않음.
 *
 * 호출 체인:
 *   blk_zone_reset_bio_endio() / blk_zone_reset_all_bio_endio() /
 *   blk_zone_finish_bio_endio() → [disk_zone_set_cond] → blk_zone_set_cond()
 */
static void disk_zone_set_cond(struct gendisk *disk, sector_t sector,
			       enum blk_zone_cond cond)
{
	u8 *zones_cond;

	rcu_read_lock(); // zones_cond 포인터가 revalidate 중 바뀌는 것을 보호
	zones_cond = rcu_dereference(disk->zones_cond); // RCU read-side에서 zone condition cache 포인터 snapshot 획득
	if (zones_cond) {
		unsigned int zno = disk_zone_no(disk, sector); // sector로부터 NVMe ZNS Zone Identifier(ZID) 계산

		/*
		 * The condition of a conventional, readonly and offline zones
		 * never changes, so do nothing if the target zone is in one of
		 * these conditions.
		 */
		switch (zones_cond[zno]) { // conventional/readonly/offline zone의 cond는 변경 불가
		case BLK_ZONE_COND_NOT_WP:
		// conventional/readonly/offline zone의 cond는 변하지 않으므로 무시
		case BLK_ZONE_COND_READONLY:
		case BLK_ZONE_COND_OFFLINE:
			break;
		default:
			blk_zone_set_cond(zones_cond, zno, cond); // 커널 캐시의 ZNS zone state 갱신
			break;
		}
	}
	rcu_read_unlock();
}

/**
 * bdev_zone_is_seq - check if a sector belongs to a sequential write zone
 * @bdev:       block device to check
 * @sector:     sector number
 *
 * Check if @sector on @bdev is contained in a sequential write required zone.
 */
/*
 * [한국어]
 * bdev_zone_is_seq (한국어 보강) - sector가 sequential write required zone에
 * 속하는지 판정
 *
 * @bdev: 검사할 block_device.
 * @sector: 검사할 sector 번호.
 * @return: true면 순차 쓰기 필수 zone(WP 규칙 적용 대상), false면
 *          conventional zone이거나 zoned 디바이스가 아님.
 *
 * NVMe ZNS에서 conventional zone이 아닌 zone은 모두 "sequential write
 * required"로 간주되어 임의 위치 쓰기가 허용되지 않는다. 이 함수는
 * zones_cond 캐시에서 해당 zone의 condition이 BLK_ZONE_COND_NOT_WP인지만
 * 확인하는 빠른 경로 — WP 위치 자체는 보지 않고 zone "종류"만 구분한다.
 * 실행 컨텍스트: BIO 제출 경로(프로세스 컨텍스트)에서 호출되며
 * RCU read-side로 zones_cond 포인터를 안전하게 스냅샷한다(재검증 중 배열이
 * 교체돼도 use-after-free 없음).
 * 호출자: blk_zone_wplug_handle_write() — conventional zone은 plug 없이
 * 바로 통과시키기 위해 최우선으로 확인. blk_zone_wplug_handle_zone_mgmt()도
 * reset/finish가 conventional zone에 오지 않았는지 검증.
 * 피호출자: disk_zone_no()(sector→ZID 변환), rcu_dereference().
 * 에러 경로: bdev_is_zoned()가 거짓이거나 zones_cond/ZID 범위가 유효하지
 * 않으면 false를 반환(zoned가 아니면 sequential 개념 자체가 없음).
 *
 * 호출 체인:
 *   blk_zone_wplug_handle_write() / blk_zone_wplug_handle_zone_mgmt()
 *   → [bdev_zone_is_seq]
 */
bool bdev_zone_is_seq(struct block_device *bdev, sector_t sector)
{
	struct gendisk *disk = bdev->bd_disk; // NVMe namespace를 나타내는 gendisk 획득
	unsigned int zno = disk_zone_no(disk, sector); // 대상 sector가 속한 ZID 계산
	bool is_seq = false;
	u8 *zones_cond;

	if (!bdev_is_zoned(bdev)) // zoned 디바이스가 아니면 ZNS 판정 없이 false 반환
		return false;

	rcu_read_lock(); // zones_cond 교체(revalidate/format)와의 RCU 동기화
	zones_cond = rcu_dereference(disk->zones_cond); // RCU pointer로 zone condition cache 접근
	if (zones_cond && zno < disk->nr_zones) // ZID가 전체 zone 수 이내인지 검증
		is_seq = zones_cond[zno] != BLK_ZONE_COND_NOT_WP; // NOT_WP가 아니면 sequential write required zone (ZNS)
	rcu_read_unlock();

	return is_seq;
}
EXPORT_SYMBOL_GPL(bdev_zone_is_seq);

/*
 * struct blk_report_zones_args:
 *   NVMe ZNS Report Zones command가 완료된 후 각 zone descriptor를
 *   상위 층(사용자 공간 ioctl, 재검증, 캐시 갱신 등)으로 전달할 때
 *   사용하는 콜백 인자.
 *   cb:   zone descriptor 하나를 처리할 콜백.
 *   data: 사용자 콜백 데이터.
 *   report_active: cached report fallback 시 implicit/explicit open/closed
 *                  를 active로 축소하여 단순화.
 */
/*
 * Zone report arguments for block device drivers report_zones operation.
 * @cb: report_zones_cb callback for each reported zone.
 * @data: Private data passed to report_zones_cb.
 */
struct blk_report_zones_args {
	report_zones_cb cb;
	/* [한국어] Report Zones 결과로 얻은 zone descriptor 하나마다 호출되는
	 * 콜백 함수 포인터 (typedef 정의는 blkdev.h).
	 * 설정자: blkdev_report_zones()/blkdev_report_zones_cached()가 사용자가
	 * 넘긴 콜백을 그대로 저장; blk_revalidate_disk_zones()는 내부용
	 * blk_revalidate_zone_cb()를 지정.
	 * 읽는 자: disk_report_zone()이 args->cb(zone, idx, args->data) 형태로
	 * 호출 — 디바이스 드라이버(예: nvme_report_zones)가 zone 하나를 받을
	 * 때마다 이 콜백을 통해 상위 계층으로 전달.
	 * 값 범위: NULL이면 disk_report_zone()이 콜백 호출을 생략(캐시 갱신만
	 * 수행).
	 * 동기화: 별도 락 없음 — args 구조체 자체가 단일 호출 스택 프레임에
	 * 스코프되어 동시 접근이 없다. */

	void		*data;
	/* [한국어] cb 콜백에 그대로 전달되는 사용자 정의 컨텍스트 포인터.
	 * 설정자: 호출자가 상황에 맞게 설정 — 예: blkdev_copy_zone_to_user()용
	 * struct zone_report_args*, blk_revalidate_zone_cb()용
	 * struct blk_revalidate_zone_args*.
	 * 읽는 자: disk_report_zone()이 args->cb() 호출 시 3번째 인자로 그대로
	 * 전달 — 콜백이 실제 타입으로 캐스팅해 사용.
	 * 값 범위: 콜백 구현에 따라 의미가 다른 불투명(opaque) 포인터.
	 * 동기화: 해당 없음(단일 호출 컨텍스트). */

	bool		report_active;
	/* [한국어] cached report가 실패해 실제 Report Zones로 fallback할 때,
	 * IMP_OPEN/EXP_OPEN/CLOSED 조건을 ACTIVE로 뭉뚱그릴지 여부.
	 * 설정자: blkdev_report_zone_fallback(), blkdev_report_zones_cached()의
	 * fallback 경로가 true로 설정; blkdev_report_zones()(일반 경로)는
	 * 기본값 false(초기화 리스트에서 생략) 유지.
	 * 읽는 자: disk_report_zone()이 true일 때 open/closed 계열 조건을
	 * BLK_ZONE_COND_ACTIVE로 축소해 zone plug의 캐시된 condition과
	 * 일관되게 맞춘다(캐시는 open/closed를 세분화하지 않음).
	 * 값 범위: true/false.
	 * 동기화: 해당 없음(단일 호출 컨텍스트). */
};

/*
 * [한국어]
 * blkdev_do_report_zones - Report Zones 요청을 드라이버 report_zones op로 전달
 *
 * @bdev: 대상 block_device.
 * @sector: report를 시작할 sector.
 * @nr_zones: 요청할 최대 zone 개수.
 * @args: 콜백/데이터/report_active 플래그를 담은 인자 구조체.
 * @return: 실제 report된 zone 개수(0 이상), 또는 -EOPNOTSUPP(zoned가 아니거나
 *          드라이버가 report_zones op를 구현하지 않음).
 *
 * blkdev_report_zones()와 blkdev_report_zone_fallback() 두 상위 함수가
 * 공유하는 공통 검증 + 디스패치 헬퍼. disk->fops->report_zones는 NVMe라면
 * nvme_report_zones()로 연결되어 실제로 NVMe ZNS Report Zones 커맨드를
 * SQ에 태우고 CQ 완료를 기다린 뒤 zone descriptor들을 args->cb 콜백으로
 * 하나씩 넘긴다. 이 함수 자체는 그 호출 전 범위 검증만 담당한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl/재검증 경로) — 호출자가
 * memalloc_noXX_save/restore로 메모리 할당 컨텍스트를 제어해야 한다.
 * 호출자: blkdev_report_zones(), blkdev_report_zone_fallback().
 * 피호출자: disk->fops->report_zones(드라이버 콜백, 예: nvme_report_zones).
 * 에러 경로: zoned 디바이스가 아니거나 report_zones op가 없으면
 * -EOPNOTSUPP(WARN_ON_ONCE로 커널 버그 여부도 함께 표시); 범위가 비었거나
 * capacity를 벗어나면 0을 반환(에러는 아님, report할 것이 없다는 의미).
 *
 * 호출 체인:
 *   blkdev_report_zones() / blkdev_report_zone_fallback() →
 *   [blkdev_do_report_zones] → disk->fops->report_zones() → nvme_report_zones()
 */
static int blkdev_do_report_zones(struct block_device *bdev, sector_t sector,
				  unsigned int nr_zones,
				  struct blk_report_zones_args *args)
{
	struct gendisk *disk = bdev->bd_disk;

	if (!bdev_is_zoned(bdev) || WARN_ON_ONCE(!disk->fops->report_zones)) // NVMe ZNS 드라이버는 report_zones op를 구현해야 함
	// NVMe ZNS는 report_zones op를 구현해야 함
		return -EOPNOTSUPP;

	if (!nr_zones || sector >= get_capacity(disk)) // 요청 범위가 비었거나 용량 초과시 NVMe command 불필요
		return 0;

	return disk->fops->report_zones(disk, sector, nr_zones, args); // nvme_report_zones() 호출 -> SQ/CID 할당 및 CQ 수신
}

/**
 * blkdev_report_zones - Get zones information
 * @bdev:	Target block device
 * @sector:	Sector from which to report zones
 * @nr_zones:	Maximum number of zones to report
 * @cb:		Callback function called for each reported zone
 * @data:	Private data for the callback
 *
 * Description:
 *    Get zone information starting from the zone containing @sector for at most
 *    @nr_zones, and call @cb for each zone reported by the device.
 *    To report all zones in a device starting from @sector, the BLK_ALL_ZONES
 *    constant can be passed to @nr_zones.
 *    Returns the number of zones reported by the device, or a negative errno
 *    value in case of failure.
 *
 *    Note: The caller must use memalloc_noXX_save/restore() calls to control
 *    memory allocations done within this function.
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * blkdev_report_zones - Report Zones 정보 조회의 공개 진입점
 *
 * @bdev: 대상 block_device.
 * @sector: report를 시작할 sector가 포함된 zone부터 조회.
 * @nr_zones: 최대 조회 zone 개수(BLK_ALL_ZONES를 넘기면 전체).
 * @cb: zone마다 호출될 콜백.
 * @data: 콜백에 그대로 전달되는 사용자 컨텍스트.
 * @return: 실제 report된 zone 개수, 또는 음수 errno.
 *
 * 사용자/상위 계층(파일시스템, ioctl 등)이 항상 실제 디바이스에 Report
 * Zones 커맨드를 내려 최신 정보를 얻고 싶을 때 쓰는 경로 — 캐시를 쓰는
 * blkdev_report_zones_cached()와 대비된다. cb/data를 blk_report_zones_args로
 * 감싸 blkdev_do_report_zones()에 위임한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 memalloc_noXX_save/restore로
 * 메모리 할당 GFP 컨텍스트를 제어해야 함(커널 문서 명시).
 * 호출자: blkdev_report_zones_ioctl()(BLKREPORTZONE), 그 외 드라이버/상위
 * 계층에서 직접 조회할 때(EXPORT_SYMBOL_GPL로 모듈에도 공개).
 * 피호출자: blkdev_do_report_zones() → disk->fops->report_zones →
 * nvme_report_zones() → SQ/CID 할당 및 CQ 수신.
 * 에러 경로: blkdev_do_report_zones()가 반환하는 음수 errno를 그대로 전파.
 *
 * 호출 체인:
 *   blkdev_report_zones_ioctl() → [blkdev_report_zones] →
 *   blkdev_do_report_zones() → nvme_report_zones() → SQ/CID
 */
int blkdev_report_zones(struct block_device *bdev, sector_t sector,
			unsigned int nr_zones, report_zones_cb cb, void *data)
{
	struct blk_report_zones_args args = { // 사용자 콜백을 report_zones 인자에 연결
		.cb = cb, // private data; NVMe ZNS zone descriptor가 전달될 context
		.data = data,
	};

	return blkdev_do_report_zones(bdev, sector, nr_zones, &args); // blkdev_do_report_zones() 경유로 NVMe command 발행
}
EXPORT_SYMBOL_GPL(blkdev_report_zones);

/*
 * [한국어]
 * blkdev_zone_reset_all - 디스크 전체 zone을 한 번에 Reset
 *
 * @bdev: 대상 block_device.
 * @return: submit_bio_wait()의 결과(0=성공, 음수=errno).
 *
 * REQ_OP_ZONE_RESET_ALL 하나만 실은 BIO를 스택에 만들어 동기 제출한다.
 * NVMe ZNS는 "Reset All"이 별도의 Zone Management Send 하위 명령으로
 * 존재하므로, 전체 디스크를 reset할 때는 zone 개수만큼 개별 Reset 명령을
 * 반복하지 않고 이 명령 하나로 끝낼 수 있어 SQ/CID 낭비를 크게 줄인다.
 * 실행 컨텍스트: 프로세스 컨텍스트 — bio_init()으로 스택 BIO를 초기화하고
 * submit_bio_wait()로 완료까지 블로킹 대기(재진입 문제 없음, 짧은 생명주기).
 * 호출자: blkdev_zone_mgmt() — sector==0 && nr_sectors==capacity인
 * REQ_OP_ZONE_RESET 요청을 최적화하는 특수 경로로만 호출.
 * 피호출자: bio_init(), submit_bio_wait() → blk_mq_submit_bio() →
 * nvme_queue_rq() → nvme_submit_cmd(doorbell).
 * 에러 경로: submit_bio_wait()가 반환하는 값을 그대로 호출자에 전달.
 *
 * 호출 체인:
 *   blkdev_zone_mgmt() → [blkdev_zone_reset_all] → submit_bio_wait() →
 *   nvme_queue_rq()
 */
static int blkdev_zone_reset_all(struct block_device *bdev)
{
	struct bio bio;

	bio_init(&bio, bdev, NULL, 0, REQ_OP_ZONE_RESET_ALL | REQ_SYNC); // REQ_OP_ZONE_RESET_ALL을 태운 단일 BIO 생성
	trace_blkdev_zone_mgmt(&bio, 0);
	return submit_bio_wait(&bio); // NVMe Reset All command가 완료될 때까지 동기 대기
}

/**
 * blkdev_zone_mgmt - Execute a zone management operation on a range of zones
 * @bdev:	Target block device
 * @op:		Operation to be performed on the zones
 * @sector:	Start sector of the first zone to operate on
 * @nr_sectors:	Number of sectors, should be at least the length of one zone and
 *		must be zone size aligned.
 *
 * Description:
 *    Perform the specified operation on the range of zones specified by
 *    @sector..@sector+@nr_sectors. Specifying the entire disk sector range
 *    is valid, but the specified range should not contain conventional zones.
 *    The operation to execute on each zone can be a zone reset, open, close
 *    or finish request.
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * blkdev_zone_mgmt - zone 범위에 대해 zone 관리 명령을 실행
 *
 * @bdev: 대상 block_device.
 * @op: 수행할 연산(REQ_OP_ZONE_RESET/OPEN/CLOSE/FINISH 중 하나).
 * @sector: 대상 zone 범위의 시작 sector(zone 경계에 정렬돼야 함).
 * @nr_sectors: 대상 범위의 길이(zone 크기의 배수여야 하며, 마지막 zone까지
 *              포함하는 경우는 예외적으로 남은 길이 허용).
 * @return: 0(성공) 또는 음수 errno(-EOPNOTSUPP/-EPERM/-EINVAL 등).
 *
 * NVMe ZNS Zone Management Send 커맨드의 Reset/Open/Close/Finish 하위
 * 기능을 [sector, sector+nr_sectors) 범위의 모든 zone에 대해 수행한다.
 * zone 경계/정렬/범위를 모두 검증한 뒤, 전체 디스크에 대한 Reset이면
 * blkdev_zone_reset_all()로 최적화하고, 그 외에는 zone마다 BIO를 만들어
 * blk_next_bio()로 체인처럼 연결한 후 submit_bio_wait()로 동기 제출한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 — zone 수가 많으면 루프가 길어질 수
 * 있어 cond_resched()로 협조적 스케줄링을 명시적으로 양보한다.
 * 호출자: blkdev_zone_mgmt_ioctl()(BLKRESETZONE 등)과 blkdev_reset_zone().
 * 피호출자: blkdev_zone_reset_all(), blk_next_bio(), submit_bio_wait() →
 * blk_mq_submit_bio() → nvme_queue_rq() → nvme_submit_cmd(doorbell, SQ,
 * CID) — 이 파일이 강제한 zone 정렬 규칙을 지킨 커맨드만 여기까지 도달.
 * 에러 경로: read-only/zoned 아님/범위 초과/정렬 오류는 BIO를 만들기 전에
 * 조기 반환; 제출 후 실패는 submit_bio_wait()의 반환값을 그대로 전달.
 *
 * 호출 체인:
 *   blkdev_zone_mgmt_ioctl() / blkdev_reset_zone() → [blkdev_zone_mgmt] →
 *   blkdev_zone_reset_all() 또는 submit_bio_wait() → nvme_queue_rq()
 */
int blkdev_zone_mgmt(struct block_device *bdev, enum req_op op,
		     sector_t sector, sector_t nr_sectors)
{
	sector_t zone_sectors = bdev_zone_sectors(bdev); // NVMe ZNS zone size (queue limits.chunk_sectors)
	sector_t capacity = bdev_nr_sectors(bdev); // NVMe namespace capacity
	sector_t end_sector = sector + nr_sectors; // 작업할 zone 범위의 exclusive 끝 sector
	struct bio *bio = NULL;
	int ret = 0;

	if (!bdev_is_zoned(bdev)) // ZNS가 아닌 블록 장치는 zone management 불가
		return -EOPNOTSUPP;

	if (bdev_read_only(bdev)) // read-only namespace이면 NVMe command 발행 전 차단
		return -EPERM;

	if (!op_is_zone_mgmt(op)) // zone mgmt op인지 검증 (reset/open/close/finish)
		return -EOPNOTSUPP;

	if (end_sector <= sector || end_sector > capacity) // NVMe namespace capacity 범위 검증
		/* Out of range */
		return -EINVAL;

	/* Check alignment (handle eventual smaller last zone) */
	if (!bdev_is_zone_start(bdev, sector)) // zone 시작 경계 정렬 검증; 잘못된 ZID 명령 방지
		return -EINVAL;

	if (!bdev_is_zone_start(bdev, nr_sectors) && end_sector != capacity) // 마지막 zone을 제외한 크기는 zone size 배수여야 함
		return -EINVAL;

	/*
	 * In the case of a zone reset operation over all zones, use
	 * REQ_OP_ZONE_RESET_ALL.
	 */
	if (op == REQ_OP_ZONE_RESET && sector == 0 && nr_sectors == capacity) // 전체 디스크 reset 시 NVMe Reset All command로 최적화
		return blkdev_zone_reset_all(bdev);
		// 전체 zone reset은 NVMe Reset All command로 매핑

	while (sector < end_sector) {
		bio = blk_next_bio(bio, bdev, 0, op | REQ_SYNC, GFP_KERNEL); // zone 단위로 BIO 생성 -> zone마다 NVMe SQ entry 소비
		bio->bi_iter.bi_sector = sector; // 각 zone의 시작 sector 설정 (ZNS Zone Identifier 기반)
		sector += zone_sectors; // 다음 zone으로 이동; NVMe Zone Management Send 범위 축적

		/* This may take a while, so be nice to others */
		cond_resched(); // 오랜 zone loop 중 선점 양보; NVMe ISR/completion 처리 허용
	}

	trace_blkdev_zone_mgmt(bio, nr_sectors);
	ret = submit_bio_wait(bio); // 모든 zone management BIO가 NVMe 완료될 때까지 대기
	bio_put(bio); // BIO 참조 해제; NVMe completion 후 메모리 반환

	return ret;
}
EXPORT_SYMBOL_GPL(blkdev_zone_mgmt);

/*
 * [한국어]
 * struct zone_report_args:
 *   BLKREPORTZONE/BLKREPORTZONEV2 ioctl 처리 중 blkdev_copy_zone_to_user()
 *   콜백에 전달되는 사용자 버퍼 컨텍스트. blk_report_zones_args.data로
 *   보관되어 disk_report_zone()이 zone 하나를 얻을 때마다 넘겨준다. */
struct zone_report_args {
	struct blk_zone __user *zones;
	/* [한국어] 사용자 공간(ioctl 호출자)이 제공한 blk_zone 배열의 시작
	 * 주소 — copy_to_user() 대상이 되는 __user 포인터.
	 * 설정자: blkdev_report_zones_ioctl()이 ioctl 인자 바로 뒤(zone
	 * report 헤더 다음)의 사용자 버퍼 주소를 계산해 대입.
	 * 읽는 자: blkdev_copy_zone_to_user()가 args->zones[idx]에
	 * copy_to_user()로 zone descriptor 하나를 기록.
	 * 값 범위: 유효한 사용자 공간 포인터 — 커널이 직접 역참조하면 안 되고
	 * 반드시 copy_to_user()를 통해서만 접근해야 한다.
	 * 동기화: 해당 없음 — 단일 ioctl 호출 스택 내에서만 유효한 임시
	 * 컨텍스트. */
};

/*
 * [한국어]
 * blkdev_copy_zone_to_user - report_zones 콜백: zone descriptor를 사용자에게 복사
 *
 * @zone: 커널이 채운 zone descriptor(스택 또는 드라이버 버퍼).
 * @idx: 이번 report 요청 내에서 이 zone의 순번(0부터 시작).
 * @data: struct zone_report_args* 로 캐스팅되는 사용자 버퍼 컨텍스트.
 * @return: 0(성공) 또는 -EFAULT(사용자 버퍼 접근 실패).
 *
 * blkdev_report_zones()/blkdev_report_zones_cached()가 report_zones_cb
 * 타입 콜백으로 등록하는 함수 — zone 하나가 확인될 때마다 커널 공간의
 * blk_zone 구조체를 BLKREPORTZONE(V2) ioctl 호출자의 사용자 버퍼 idx번째
 * 슬롯에 복사한다.
 * 실행 컨텍스트: ioctl 처리 중인 프로세스 컨텍스트 — copy_to_user()가
 * 페이지 폴트를 유발할 수 있어 인터럽트 컨텍스트에서는 호출 불가.
 * 호출자: disk_report_zone()이 args->cb(zone, idx, args->data) 형태로 호출.
 * 피호출자: copy_to_user().
 * 에러 경로: copy_to_user() 실패(사용자 페이지 접근 불가) 시 -EFAULT를
 * 반환 — 이는 report_zones 루프를 즉시 중단시키는 신호로 전파된다.
 *
 * 호출 체인:
 *   disk_report_zone() → [blkdev_copy_zone_to_user] → copy_to_user()
 */
static int blkdev_copy_zone_to_user(struct blk_zone *zone, unsigned int idx,
				    void *data)
{
	struct zone_report_args *args = data;

	if (copy_to_user(&args->zones[idx], zone, sizeof(struct blk_zone))) // 커널의 ZNS zone descriptor를 사용자 공간 ioctl 버퍼로 복사
		return -EFAULT;
	return 0;
}

/*
 * Mask of valid input flags for BLKREPORTZONEV2 ioctl.
 */
#define BLK_ZONE_REPV2_INPUT_FLAGS	BLK_ZONE_REP_CACHED
/* [한국어] BLKREPORTZONEV2 ioctl의 blk_zone_report.flags에서 사용자가 지정할
 * 수 있는 유일한 입력 비트 - BLK_ZONE_REP_CACHED(캐시된 report 요청).
 * 그 외 비트가 설정되면 blkdev_report_zones_ioctl()이 -EINVAL로 거부한다. */

/*
 * BLKREPORTZONE and BLKREPORTZONEV2 ioctl processing.
 * Called from blkdev_ioctl.
 */
/*
 * [한국어]
 * blkdev_report_zones_ioctl - BLKREPORTZONE/BLKREPORTZONEV2 ioctl 처리
 *
 * @bdev: 대상 block_device.
 * @cmd: BLKREPORTZONE 또는 BLKREPORTZONEV2.
 * @arg: 사용자 공간의 struct blk_zone_report(+ zone 배열)에 대한 포인터.
 * @return: 0(성공) 또는 음수 errno(-EINVAL/-ENOTTY/-EFAULT 등).
 *
 * 사용자가 BLKREPORTZONE(레거시)/BLKREPORTZONEV2(cached report 지원)
 * ioctl로 zone 정보를 요청할 때 처리하는 진입점. V2는 rep.flags에
 * BLK_ZONE_REP_CACHED가 설정되면 blkdev_report_zones_cached()로, 그렇지
 * 않거나 V1이면 blkdev_report_zones()로 실제(비캐시) Report Zones를
 * 수행한다. cached 경로는 커널이 이미 알고 있는 zone condition/WP만으로
 * 응답해 불필요한 NVMe 커맨드 왕복을 줄인다.
 * 실행 컨텍스트: ioctl 시스템 콜 처리 중인 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl().
 * 피호출자: blkdev_report_zones(), blkdev_report_zones_cached(),
 * blkdev_copy_zone_to_user()(각 zone을 사용자 버퍼에 복사).
 * 에러 경로: 인자 NULL/zoned 아님/copy_from_user 실패/nr_zones==0/잘못된
 * V2 flags는 조기에 -EINVAL/-ENOTTY/-EFAULT로 반환; report 자체 실패는
 * ret<0 분기에서 그대로 상위로 전파.
 *
 * 호출 체인:
 *   blkdev_ioctl() → [blkdev_report_zones_ioctl] →
 *   blkdev_report_zones() / blkdev_report_zones_cached() →
 *   disk->fops->report_zones() → nvme_report_zones()
 */
int blkdev_report_zones_ioctl(struct block_device *bdev, unsigned int cmd,
		unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct zone_report_args args;
	struct blk_zone_report rep;
	int ret;

	if (!argp) // ioctl 인자 포인터 NULL 검증
		return -EINVAL;

	if (!bdev_is_zoned(bdev)) // ZNS 디바이스가 아니면 ioctl 미지원
		return -ENOTTY;

	if (copy_from_user(&rep, argp, sizeof(struct blk_zone_report))) // 사용자로부터 blk_zone_report 구조체 복사
		return -EFAULT;

	if (!rep.nr_zones) // 요청 zone 수가 0이면 의미 없음
		return -EINVAL;

	args.zones = argp + sizeof(struct blk_zone_report);

	switch (cmd) {
	case BLKREPORTZONE:
		ret = blkdev_report_zones(bdev, rep.sector, rep.nr_zones, // uncached path: NVMe Report Zones command 발행
					  blkdev_copy_zone_to_user, &args);
		break;
	case BLKREPORTZONEV2:
		if (rep.flags & ~BLK_ZONE_REPV2_INPUT_FLAGS) // 허용되지 않은 report flags 거부
			return -EINVAL;
		ret = blkdev_report_zones_cached(bdev, rep.sector, rep.nr_zones, // cached path: 커널 캐시로 NVMe command 왕복 회피
					 blkdev_copy_zone_to_user, &args);
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0) // NVMe command 실패 시 즉시 사용자에게 전파
		return ret;

	rep.nr_zones = ret; // 실제 report된 zone descriptor 개수 기록
	rep.flags = BLK_ZONE_REP_CAPACITY;
	if (copy_to_user(argp, &rep, sizeof(struct blk_zone_report))) // 사용자 공간에 결과 헤더 복사
		return -EFAULT;
	return 0;
}

/*
 * [한국어]
 * blkdev_reset_zone - BLKRESETZONE 처리: page cache 무효화 후 Reset 실행
 *
 * @bdev: 대상 block_device.
 * @mode: 파일 오픈 모드(권한/트렁케이트 검사에 사용).
 * @zrange: 초기화할 sector 범위.
 * @return: 0(성공) 또는 음수 errno.
 *
 * zone reset은 WP를 zone 시작으로 되돌려 해당 구간의 기존 데이터를 모두
 * 무효화하는 파괴적 연산이므로, 실제 Zone Management Send(Reset)를
 * 내리기 전에 (1) inode 락으로 파일시스템 쓰기와 직렬화하고 (2)
 * filemap_invalidate_lock + truncate_bdev_range()로 해당 구간의 page
 * cache를 미리 비워 stale 데이터를 사용자가 재사용하지 않게 한다.
 * 실행 컨텍스트: ioctl 프로세스 컨텍스트 — inode_lock/filemap_invalidate_lock은
 * 블로킹 뮤텍스이므로 인터럽트 컨텍스트에서 호출 불가.
 * 호출자: blkdev_zone_mgmt_ioctl()의 BLKRESETZONE 분기.
 * 피호출자: truncate_bdev_range(), blkdev_zone_mgmt()(REQ_OP_ZONE_RESET).
 * 에러 경로: 범위 검증 실패나 truncate 실패는 blkdev_zone_mgmt() 호출 전
 * out_unlock으로 점프해 락만 정리하고 에러를 반환.
 *
 * 호출 체인:
 *   blkdev_zone_mgmt_ioctl() → [blkdev_reset_zone] →
 *   truncate_bdev_range(), blkdev_zone_mgmt()
 */
static int blkdev_reset_zone(struct block_device *bdev, blk_mode_t mode,
			     struct blk_zone_range *zrange)
{
	loff_t start, end;
	int ret = -EINVAL;

	inode_lock(bdev->bd_mapping->host); // 파일시스템 쓰기와 reset 동작 직렬화
	filemap_invalidate_lock(bdev->bd_mapping); // reset 구간의 page cache를 무효화
	if (zrange->sector + zrange->nr_sectors <= zrange->sector || // 오버플로 또는 0 길이 검사
	    zrange->sector + zrange->nr_sectors > get_capacity(bdev->bd_disk)) // capacity 초과 검사
		/* Out of range */
		// [한국어] 범위를 벗어난 reset 요청 - NVMe command 발행 전 조기 차단하고 lock만 정리
		goto out_unlock;

	start = zrange->sector << SECTOR_SHIFT; // sector -> byte offset 변환 (truncate 범위 시작)
	end = ((zrange->sector + zrange->nr_sectors) << SECTOR_SHIFT) - 1; // truncate 범위의 마지막 byte(inclusive)

	ret = truncate_bdev_range(bdev, mode, start, end); // reset할 구간의 캐시된 데이터 truncate
	if (ret) // truncate 실패(예: 매핑된 페이지 잠금 등) 시 NVMe reset을 시도하지 않고 즉시 실패
		goto out_unlock;

	ret = blkdev_zone_mgmt(bdev, REQ_OP_ZONE_RESET, zrange->sector, // NVMe Zone Management Send(Reset) 실행
			       zrange->nr_sectors);
out_unlock:
	filemap_invalidate_unlock(bdev->bd_mapping);
	inode_unlock(bdev->bd_mapping->host);
	return ret;
}

/*
 * BLKRESETZONE, BLKOPENZONE, BLKCLOSEZONE and BLKFINISHZONE ioctl processing.
 * Called from blkdev_ioctl.
 */
/*
 * [한국어]
 * blkdev_zone_mgmt_ioctl - BLKRESETZONE/OPENZONE/CLOSEZONE/FINISHZONE 처리
 *
 * @bdev: 대상 block_device.
 * @mode: 파일 오픈 모드 — 쓰기 권한(BLK_OPEN_WRITE) 검사에 사용.
 * @cmd: BLKRESETZONE/BLKOPENZONE/BLKCLOSEZONE/BLKFINISHZONE 중 하나.
 * @arg: 사용자 공간의 struct blk_zone_range에 대한 포인터.
 * @return: 0(성공) 또는 음수 errno(-EINVAL/-ENOTTY/-EBADF/-EFAULT 등).
 *
 * 네 가지 zone 관리 ioctl의 공통 진입점. RESETZONE만 page cache 무효화가
 * 필요해 blkdev_reset_zone()으로 별도 위임하고, 나머지 세 개는 대응하는
 * REQ_OP_ZONE_* 값을 골라 blkdev_zone_mgmt()를 직접 호출한다. 각각 NVMe
 * ZNS Zone Management Send 커맨드의 Reset/Open/Close/Finish 하위 기능에
 * 대응한다.
 * 실행 컨텍스트: ioctl 시스템 콜 처리 중인 프로세스 컨텍스트.
 * 호출자: blkdev_ioctl().
 * 피호출자: blkdev_reset_zone() 또는 blkdev_zone_mgmt().
 * 에러 경로: 인자 NULL, zoned 아님, 쓰기 권한 없음, copy_from_user 실패,
 * 알 수 없는 cmd는 각각 -EINVAL/-ENOTTY/-EBADF/-EFAULT/-ENOTTY로 조기 반환.
 *
 * 호출 체인:
 *   blkdev_ioctl() → [blkdev_zone_mgmt_ioctl] → blkdev_reset_zone() /
 *   blkdev_zone_mgmt() → nvme_queue_rq()
 */
int blkdev_zone_mgmt_ioctl(struct block_device *bdev, blk_mode_t mode,
			   unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct blk_zone_range zrange;
	enum req_op op;

	if (!argp)
		return -EINVAL;

	if (!bdev_is_zoned(bdev)) // ZNS 디바이스인지 재확인
		return -ENOTTY;

	if (!(mode & BLK_OPEN_WRITE)) // 쓰기 권한 없으면 NVMe command 발행 전 차단
		return -EBADF;

	if (copy_from_user(&zrange, argp, sizeof(struct blk_zone_range))) // 사용자로부터 zone 범위 복사
		return -EFAULT;

	switch (cmd) {
	case BLKRESETZONE:
		return blkdev_reset_zone(bdev, mode, &zrange); // Reset 후 NVMe ZNS zone WP를 0으로 되돌림
	case BLKOPENZONE:
		op = REQ_OP_ZONE_OPEN; // NVMe Zone Management Send(Open)에 대응
		break;
	case BLKCLOSEZONE:
		op = REQ_OP_ZONE_CLOSE; // NVMe Zone Management Send(Close)에 대응
		break;
	case BLKFINISHZONE:
		op = REQ_OP_ZONE_FINISH; // NVMe Zone Management Send(Finish)에 대응
		break;
	default:
		return -ENOTTY;
	}

	return blkdev_zone_mgmt(bdev, op, zrange.sector, zrange.nr_sectors); // 지정 범위의 NVMe Zone Management Send command 발행
}

/*
 * [한국어]
 * disk_zone_is_last - 이 zone이 디스크의 마지막 zone인가
 *
 * @disk: 대상 gendisk.
 * @zone: 검사할 zone descriptor(start/len 필드 사용).
 * @return: true면 마지막 zone(capacity가 다른 zone과 다를 수 있음).
 *
 * zone 크기가 capacity를 정확히 나누어 떨어지지 않는 디바이스에서는
 * 마지막 zone만 더 작은 usable capacity(last_zone_capacity)를 가질 수
 * 있다. 이 함수는 zone 시작+길이가 디스크 전체 capacity 이상인지만 보고
 * "이 zone 다음에 더 이상 zone이 없다"를 판정하는 순수 산술 헬퍼.
 * 실행 컨텍스트: 어디서든 호출 가능한 부작용 없는 계산.
 * 호출자: disk_zone_wplug_is_full(), blkdev_get_zone_info(),
 * blk_revalidate_conv_zone(), blk_revalidate_seq_zone().
 * 피호출자: get_capacity()(gendisk의 전체 섹터 수 조회).
 *
 * 호출 체인:
 *   disk_zone_wplug_is_full() 등 → [disk_zone_is_last] → get_capacity()
 */
static bool disk_zone_is_last(struct gendisk *disk, struct blk_zone *zone)
{
	return zone->start + zone->len >= get_capacity(disk); // 마지막 ZNS zone 판별; last zone capacity 적용에 사용
}

/*
 * [한국어]
 * disk_zone_wplug_is_full - zone write plug의 WP가 zone capacity에 도달했는가
 *
 * @disk: 대상 gendisk.
 * @zwplug: 검사할 zone write plug.
 * @return: true면 FULL(더 이상 write 불가), false면 아직 여유 있음.
 *
 * NVMe ZNS FULL 조건(WP == zone capacity)을 host 측에서 미리 판정하기
 * 위한 핵심 술어(predicate) — 이 판정이 doorbell 전에 이뤄지므로 FULL
 * zone에 대한 쓸모없는 write가 SQ/CID를 낭비하며 디바이스까지 가지 않게
 * 막을 수 있다. 마지막 zone은 zone_capacity 대신 last_zone_capacity를
 * 기준으로 삼는다(둘이 다를 수 있으므로).
 * 실행 컨텍스트: 반드시 zwplug->lock을 쥔 상태에서 호출(wp_offset이 다른
 * 스레드에 의해 동시에 바뀌지 않도록).
 * 호출자: blk_zone_wplug_prepare_bio(), blk_zone_write_plug_init_request(),
 * disk_zone_wplug_set_wp_offset(), disk_zone_wplug_unplug_bio() 등 WP
 * 갱신/검증 경로 전반.
 * 피호출자: disk_zone_is_last().
 *
 * 호출 체인:
 *   blk_zone_wplug_prepare_bio() 등 → [disk_zone_wplug_is_full] →
 *   disk_zone_is_last()
 */
static bool disk_zone_wplug_is_full(struct gendisk *disk,
				    struct blk_zone_wplug *zwplug)
{
	if (zwplug->zone_no < disk->nr_zones - 1) // 마지막 zone이 아니면 일반 zone_capacity로 full 판정
		return zwplug->wp_offset >= disk->zone_capacity; // WP가 zone capacity에 도달하면 ZNS FULL 상태
	return zwplug->wp_offset >= disk->last_zone_capacity; // 마지막 zone은 축소된 capacity로 full 판정
}

/*
 * [한국어]
 * disk_insert_zone_wplug - 새 zone write plug를 disk 해시 테이블에 삽입
 *
 * @disk: 대상 gendisk.
 * @zwplug: 삽입할(미리 할당된) zone write plug.
 * @return: true면 삽입 성공, false면 이미 동일 zone에 대한 plug가 존재해서
 *          실패(호출자가 재시도해야 함).
 *
 * 여러 CPU/컨텍스트가 동시에 같은 zone에 처음 write를 시도할 수 있으므로,
 * 삽입 전에 반드시 해시 버킷을 다시 훑어 경쟁자가 먼저 넣었는지 확인한다.
 * 성공 시 zone의 초기 condition을 zones_cond 캐시(있다면)에서 복사하거나,
 * 아직 재검증이 처음이라 캐시가 없다면 ACTIVE로 간주해 채운다.
 * 실행 컨텍스트: disk->zone_wplugs_hash_lock을 직접 획득/해제 — 인터럽트
 * 컨텍스트에서도 호출될 수 있어 spin_lock_irqsave 계열 사용.
 * 호출자: disk_get_or_alloc_zone_wplug()가 새로 할당한 plug를 게시할 때.
 * 피호출자: hash_32(), rcu_dereference_check(), hlist_add_head_rcu(),
 * atomic_inc().
 * 에러 경로: 중복 삽입 감지 시 락만 풀고 false 반환 — 호출자
 * (disk_get_or_alloc_zone_wplug)는 goto again으로 기존 plug를 재조회.
 *
 * 호출 체인:
 *   disk_get_or_alloc_zone_wplug() → [disk_insert_zone_wplug]
 */
static bool disk_insert_zone_wplug(struct gendisk *disk,
				   struct blk_zone_wplug *zwplug)
{
	struct blk_zone_wplug *zwplg;
	unsigned long flags;
	u8 *zones_cond;
	unsigned int idx =
		hash_32(zwplug->zone_no, disk->zone_wplugs_hash_bits); // ZID를 hash bucket index로 매핑

	/*
	 * Add the new zone write plug to the hash table, but carefully as we
	 * are racing with other submission context, so we may already have a
	 * zone write plug for the same zone.
	 */
	spin_lock_irqsave(&disk->zone_wplugs_hash_lock, flags); // 동시 제출 경쟁 방지를 위한 hash table lock
	hlist_for_each_entry_rcu(zwplg, &disk->zone_wplugs_hash[idx], node) { // RCU read-side와 안전하게 동일 ZID plug 존재 여부 탐색
		if (zwplg->zone_no == zwplug->zone_no) { // 이미 동일 ZID에 plug가 있으면 삽입 실패
		// 동일 ZID plug가 이미 있으면 삽입 실패
			spin_unlock_irqrestore(&disk->zone_wplugs_hash_lock, // hash lock 해제 후 재시도 또는 실패
					       flags);
			return false;
		}
	}

	/*
	 * Set the zone condition: if we do not yet have a zones_cond array
	 * attached to the disk, then this is a zone write plug insert from the
	 * first call to blk_revalidate_disk_zones(), in which case the zone is
	 * necessarilly in the active condition.
	 */
	zones_cond = rcu_dereference_check(disk->zones_cond, // hash lock을 보유한 상태에서 zones_cond 포인터 접근
				lockdep_is_held(&disk->zone_wplugs_hash_lock));
	if (zones_cond) // 재검증 완료 후라면 zones_cond에서 초기 condition 복사
		zwplug->cond = zones_cond[zwplug->zone_no]; // ZNS Report Zones로 얻은 zone state를 plug에 반영
	else
		// 첫 재검증이라 zones_cond 없음 -> ACTIVE로 처리
		zwplug->cond = BLK_ZONE_COND_ACTIVE;

	hlist_add_head_rcu(&zwplug->node, &disk->zone_wplugs_hash[idx]); // 새 plug를 RCU read-side에 publish
	atomic_inc(&disk->nr_zone_wplugs); // 활성 zone plug 개수 증가; 빠른 early-exit에 사용
	spin_unlock_irqrestore(&disk->zone_wplugs_hash_lock, flags); // hash table 갱신 완료 후 unlock

	return true;
}

/*
 * [한국어]
 * disk_get_hashed_zone_wplug - 해시 테이블에서 sector가 속한 zone의 plug 조회
 *
 * @disk: 대상 gendisk.
 * @sector: 조회할 zone에 속하는 임의의 sector.
 * @return: 참조 카운트가 증가된 plug 포인터, 또는 NULL(존재하지 않음/막
 *          해제 중이라 참조를 얻지 못함).
 *
 * RCU read-side lock만으로 lock-free하게 해시 버킷 충돌 체인을 순회하는
 * 조회 함수 — refcount_inc_not_zero()로 "찾았지만 이미 참조 카운트가
 * 0으로 가는 중(곧 free될 plug)"인 경쟁 상황을 안전하게 회피한다(0에서
 * 증가시키면 use-after-free가 될 수 있으므로 반드시 not_zero 버전 사용).
 * 실행 컨텍스트: 어떤 컨텍스트에서도 호출 가능(RCU read-side는 인터럽트
 * 컨텍스트에서도 안전) — 다만 반환된 참조는 호출자가 반드시
 * disk_put_zone_wplug()로 해제해야 한다.
 * 호출자: disk_get_zone_wplug()(nr_zone_wplugs가 0이 아닐 때만 호출).
 * 피호출자: disk_zone_no(), hash_32(), rcu_read_lock/unlock(),
 * refcount_inc_not_zero().
 *
 * 호출 체인:
 *   disk_get_zone_wplug() → [disk_get_hashed_zone_wplug]
 */
static struct blk_zone_wplug *disk_get_hashed_zone_wplug(struct gendisk *disk,
							 sector_t sector)
{
	unsigned int zno = disk_zone_no(disk, sector); // 대상 sector의 ZID
	unsigned int idx = hash_32(zno, disk->zone_wplugs_hash_bits); // ZID로 hash bucket 선택
	struct blk_zone_wplug *zwplug;

	rcu_read_lock(); // lock-free hash lookup을 위한 RCU read lock

	hlist_for_each_entry_rcu(zwplug, &disk->zone_wplugs_hash[idx], node) { // 해당 bucket의 충돌 체인 탐색
		if (zwplug->zone_no == zno && // 동일 ZID이면서 아직 해제되지 않은 plug 참조 획득
		    refcount_inc_not_zero(&zwplug->ref)) {
			rcu_read_unlock(); // plug를 찾으면 RCU read unlock
			return zwplug;
		}
	}

	rcu_read_unlock(); // plug를 찾지 못하면 RCU read unlock

	return NULL;
}

/*
 * [한국어]
 * disk_get_zone_wplug - sector가 속한 zone의 plug 조회(빠른 경로 포함)
 *
 * @disk: 대상 gendisk.
 * @sector: 조회할 zone에 속하는 임의의 sector.
 * @return: 참조 카운트가 증가된 plug, 또는 NULL.
 *
 * disk_get_hashed_zone_wplug()의 얇은 래퍼 — 활성 plug가 하나도 없는
 * (atomic_read(nr_zone_wplugs)==0) 흔한 경우에 해시 탐색 자체를 생략해
 * FULL/EMPTY 상태의 zone에 반복 접근하는 hot path의 오버헤드를 줄인다.
 * 실행 컨텍스트: 상위 함수와 동일 — 어떤 컨텍스트에서도 호출 가능.
 * 호출자: 이 파일의 거의 모든 plug 조회 지점(blk_zone_wplug_handle_write,
 * blkdev_get_zone_info, blk_zone_reset_bio_endio 등 다수).
 * 피호출자: atomic_read(), disk_get_hashed_zone_wplug().
 *
 * 호출 체인:
 *   (다수의 plug 조회 지점) → [disk_get_zone_wplug] →
 *   disk_get_hashed_zone_wplug()
 */
static inline struct blk_zone_wplug *disk_get_zone_wplug(struct gendisk *disk,
							 sector_t sector)
{
	if (!atomic_read(&disk->nr_zone_wplugs)) // 활성 plug가 없으면 lookup 생략
		return NULL;

	return disk_get_hashed_zone_wplug(disk, sector); // hash table에서 ZID에 해당하는 plug 검색
}

/*
 * [한국어]
 * disk_free_zone_wplug_rcu - RCU 콜백: grace period 후 plug 메모리 반환
 *
 * @rcu_head: struct blk_zone_wplug.rcu_head 필드(call_rcu 등록 시 사용).
 * @return: 없음(void, RCU 콜백 시그니처 고정).
 *
 * container_of()로 rcu_head 포인터에서 감싸고 있는 blk_zone_wplug 전체
 * 포인터를 복원한 뒤 mempool_free()로 되돌린다. RCU 콜백이므로 이 시점에는
 * 이미 해시에서 unlink되었고 모든 RCU read-side critical section도
 * 지나갔음이 보장된다 — 즉 어떤 reader도 더 이상 이 plug를 참조하지 않음.
 * 실행 컨텍스트: softirq 컨텍스트(RCU 콜백은 보통 softirq에서 실행) —
 * 블로킹 불가, mempool_free()는 블로킹하지 않는 짧은 연산만 수행.
 * 호출자: RCU 서브시스템이 call_rcu()에 등록된 콜백으로 자동 호출.
 * 피호출자: container_of(), mempool_free().
 *
 * 호출 체인:
 *   disk_free_zone_wplug() → call_rcu() → (RCU grace period) →
 *   [disk_free_zone_wplug_rcu]
 */
static void disk_free_zone_wplug_rcu(struct rcu_head *rcu_head)
{
	struct blk_zone_wplug *zwplug =
		container_of(rcu_head, struct blk_zone_wplug, rcu_head);

	mempool_free(zwplug, zwplug->disk->zone_wplugs_pool); // RCU grace period 종료 후 plug를 bounded mempool에 반환
}

/*
 * [한국어]
 * disk_free_zone_wplug - zone write plug를 해시에서 제거하고 해제 예약
 *
 * @zwplug: 마지막 참조가 끊긴 zone write plug(DEAD 플래그가 이미 설정돼
 *          있어야 함).
 * @return: 없음(void).
 *
 * disk_put_zone_wplug()가 참조 카운트를 0으로 만들었을 때만 호출되는 최종
 * 해제 경로. 먼저 plug의 마지막 zone condition을 disk->zones_cond 캐시
 * 배열로 되돌려 써서(plug가 없어져도 zone 상태 정보가 유실되지 않게)
 * 캐시 일관성을 유지한 뒤, 해시 체인에서 RCU-safe하게 unlink하고, 실제
 * 메모리 반환은 call_rcu()로 지연시켜 아직 진행 중일 수 있는 RCU
 * read-side 탐색(disk_get_hashed_zone_wplug)과 충돌하지 않게 한다.
 * 실행 컨텍스트: 여러 컨텍스트에서 호출될 수 있어(BIO 완료 인터럽트 포함)
 * disk->zone_wplugs_hash_lock을 irqsave 계열로 잡는다.
 * 호출자: disk_put_zone_wplug()(refcount_dec_and_test 참일 때만).
 * 피호출자: blk_zone_set_cond(), rcu_dereference_check(),
 * hlist_del_init_rcu(), atomic_dec(), call_rcu().
 * 불변조건: WARN_ON_ONCE 세 개로 DEAD 플래그 설정/PLUGGED 미설정/대기
 * BIO 없음을 검증 — 위반 시 커널 버그를 dmesg에 남기되 계속 진행(디버깅
 * 목적의 방어적 점검).
 *
 * 호출 체인:
 *   disk_put_zone_wplug() → [disk_free_zone_wplug] → call_rcu() →
 *   disk_free_zone_wplug_rcu()
 */
static void disk_free_zone_wplug(struct blk_zone_wplug *zwplug)
{
	struct gendisk *disk = zwplug->disk;
	unsigned long flags;

	WARN_ON_ONCE(!(zwplug->flags & BLK_ZONE_WPLUG_DEAD)); // DEAD flag가 설정된 plug만 해제 가능
	WARN_ON_ONCE(zwplug->flags & BLK_ZONE_WPLUG_PLUGGED); // PLUGGED 상태에서 해제하면 대기 BIO 유실 위험
	WARN_ON_ONCE(!bio_list_empty(&zwplug->bio_list)); // 해제 전 plug list에 남아있는 BIO가 없어야 함

	spin_lock_irqsave(&disk->zone_wplugs_hash_lock, flags); // zones_cond 및 hash list 갱신 직렬화
	blk_zone_set_cond(rcu_dereference_check(disk->zones_cond,
				lockdep_is_held(&disk->zone_wplugs_hash_lock)),
			  zwplug->zone_no, zwplug->cond); // plug의 최종 condition을 커널 캐시에 기록
	hlist_del_init_rcu(&zwplug->node); // hash chain에서 제거; RCU reader는 grace period 동안 계속 볼 수 있음
	atomic_dec(&disk->nr_zone_wplugs); // 활성 plug 개수 감소
	spin_unlock_irqrestore(&disk->zone_wplugs_hash_lock, flags);

	call_rcu(&zwplug->rcu_head, disk_free_zone_wplug_rcu);
}

/*
 * [한국어]
 * disk_put_zone_wplug - zone write plug 참조 카운트를 하나 감소
 *
 * @zwplug: 참조를 반환할 plug.
 * @return: 없음(void).
 *
 * refcount_t 기반 생명주기 관리의 "release" 짝 — 이 파일 전체에서 plug를
 * 획득(disk_get_zone_wplug, disk_get_or_alloc_zone_wplug, refcount_inc
 * 계열)할 때마다 반드시 짝을 이루어 호출해야 하는 함수. 카운트가 0이
 * 되면(마지막 참조) disk_free_zone_wplug()로 실제 해제 절차를 시작한다.
 * 실행 컨텍스트: 어떤 컨텍스트에서도 호출 가능 — refcount_dec_and_test()
 * 자체가 원자적이므로 별도 락 불필요(단, 0이 되는 순간 이후의 후속 처리는
 * disk_free_zone_wplug() 내부에서 별도로 락을 잡음).
 * 호출자: 이 파일의 사실상 모든 plug 사용자(blkdev_get_zone_info,
 * blk_zone_write_plug_bio_endio, disk_zone_wplug_bio_work 등 수십 곳).
 * 피호출자: refcount_dec_and_test(), disk_free_zone_wplug().
 *
 * 호출 체인:
 *   (plug를 획득했던 모든 함수) → [disk_put_zone_wplug] →
 *   disk_free_zone_wplug()(카운트가 0이 될 때만)
 */
static inline void disk_put_zone_wplug(struct blk_zone_wplug *zwplug)
{
	if (refcount_dec_and_test(&zwplug->ref)) // 마지막 참조 해제 시 RCU deferred free 시작
		disk_free_zone_wplug(zwplug);
}

/*
 * Flag the zone write plug as dead and drop the initial reference we got when
 * the zone write plug was added to the hash table. The zone write plug will be
 * unhashed when its last reference is dropped.
 */
/*
 * [한국어]
 * disk_mark_zone_wplug_dead - plug를 DEAD로 표시하고 초기(hash) 참조를 반환
 *
 * @zwplug: DEAD로 표시할 plug(zwplug->lock을 이미 쥐고 있어야 함).
 * @return: 없음(void).
 *
 * plug가 해시 테이블에 들어갈 때 disk_get_or_alloc_zone_wplug()가 잡아둔
 * "초기 여분 참조"를 이 시점에 반환한다. DEAD 플래그가 서면 이후 이
 * zone에 대한 새 write는 모두 실패 처리되며(blk_zone_wplug_handle_write의
 * DEAD 검사), 활성 사용자(BIO/request)가 모두 빠지면 참조 카운트가 0이
 * 되어 실제로 해시에서 제거된다. 이미 DEAD인 plug에 대한 중복 호출은
 * 참조를 두 번 반환하지 않도록 플래그 검사로 막는다(멱등성 보장).
 * 실행 컨텍스트: zwplug->lock 보유 상태 — lockdep_assert_held로 강제.
 * 호출자: disk_zone_wplug_set_wp_offset()(WP가 0/full이 됨),
 * blk_zone_wplug_handle_native_zone_append(), disk_zone_wplug_unplug_bio(),
 * disk_destroy_zone_wplugs_hash_table().
 * 피호출자: disk_put_zone_wplug().
 *
 * 호출 체인:
 *   disk_zone_wplug_set_wp_offset() 등 → [disk_mark_zone_wplug_dead] →
 *   disk_put_zone_wplug() → disk_free_zone_wplug()
 */
static void disk_mark_zone_wplug_dead(struct blk_zone_wplug *zwplug)
{
	lockdep_assert_held(&zwplug->lock);

	if (!(zwplug->flags & BLK_ZONE_WPLUG_DEAD)) {
		zwplug->flags |= BLK_ZONE_WPLUG_DEAD; // zone이 full/empty/reset/finish 상태임을 표시
		disk_put_zone_wplug(zwplug); // hash table에 들어갈 때 획득한 초기 reference 해제
		// DEAD 설정 후 초기 ref 해제로 최종 제거 가능
	}
}

static bool disk_zone_wplug_submit_bio(struct gendisk *disk,
				       struct blk_zone_wplug *zwplug);

/*
 * [한국어]
 * blk_zone_wplug_bio_work - workqueue 콜백: plug의 다음 BIO를 제출
 *
 * @work: struct blk_zone_wplug.bio_work 필드(queue_work 등록 시 사용).
 * @return: 없음(void, workqueue 콜백 시그니처 고정).
 *
 * disk_zone_wplug_schedule_work()가 disk->zone_wplugs_wq에 스케줄한 작업
 * 항목의 실제 실행 함수. container_of()로 plug 포인터를 복원해
 * disk_zone_wplug_submit_bio()를 호출 — 이 함수 하나가 실제로 plug list의
 * BIO를 blk_mq_submit_bio()/submit_bio 경로로 되돌려 NVMe SQ까지 도달하게
 * 만드는 지점이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(workqueue 워커 스레드) — BIO 제출이
 * 블로킹할 수 있는 경로(메모리 할당 등)를 안전하게 수행할 수 있다.
 * 호출자: workqueue 코어가 disk_zone_wplug_schedule_work()의 queue_work()
 * 요청에 따라 비동기로 호출.
 * 피호출자: container_of(), disk_zone_wplug_submit_bio(),
 * disk_put_zone_wplug().
 * 참조 카운트: disk_zone_wplug_schedule_work()가 스케줄 시 미리 잡아둔
 * 참조를 여기서 반환 — work가 실행되는 동안 plug가 해제되지 않도록 보장.
 *
 * 호출 체인:
 *   disk_zone_wplug_schedule_work() → queue_work() →
 *   [blk_zone_wplug_bio_work] → disk_zone_wplug_submit_bio() →
 *   blk_mq_submit_bio() → nvme_queue_rq()
 */
static void blk_zone_wplug_bio_work(struct work_struct *work)
{
	struct blk_zone_wplug *zwplug =
		container_of(work, struct blk_zone_wplug, bio_work);

	disk_zone_wplug_submit_bio(zwplug->disk, zwplug); // workqueue에서 plug list의 다음 BIO를 제출 경로로 복귀

	/* Drop the reference we took in disk_zone_wplug_schedule_work(). */
	disk_put_zone_wplug(zwplug); // work 스케줄링 시 증가시켰던 reference 해제
}

/*
 * Get a zone write plug for the zone containing @sector.
 * If the plug does not exist, it is allocated and inserted in the disk hash
 * table.
 */
/*
 * [한국어] (영어 요약 보강)
 * disk_get_or_alloc_zone_wplug - zone plug를 조회하고 없으면 새로 할당
 *
 * @disk: 대상 gendisk.
 * @sector: plug를 찾을 zone에 속하는 sector.
 * @gfp_mask: 할당 시 사용할 GFP 플래그(REQ_NOWAIT BIO면 GFP_NOWAIT, 아니면
 *            GFP_NOIO — 호출자가 결정).
 * @return: 참조 카운트가 증가된 plug 포인터, 또는 NULL(메모리 부족으로
 *          mempool_alloc 실패).
 *
 * 해당 zone에 write가 "처음" 도착했을 때 zone write plug 자료구조를 새로
 * 만드는 지점 — 이후 이 zone에 대한 모든 WP 추적/직렬화가 여기서 만든
 * plug를 거친다. mempool에서 할당한 뒤 ref=2(해시 테이블 자신의 몫 +
 * 현재 호출자 몫)로 초기화하고, 다른 CPU가 동시에 같은 zone에 대한 plug를
 * 먼저 넣었다면(disk_insert_zone_wplug 실패) 방금 할당한 것을 버리고
 * 처음부터 재시도(goto again)한다.
 * 실행 컨텍스트: 프로세스 컨텍스트에서 주로 호출되지만 gfp_mask에 따라
 * non-blocking(REQ_NOWAIT) 경로도 지원.
 * 호출자: blk_zone_wplug_handle_write()(write BIO 최초 도착),
 * blk_revalidate_seq_zone()(재검증 중 WP가 중간인 zone 미리 생성).
 * 피호출자: disk_get_zone_wplug(), mempool_alloc(),
 * bdev_offset_from_zone_start(), disk_insert_zone_wplug(), mempool_free().
 * 에러 경로: mempool_alloc() 실패 시 NULL 반환 — 호출자가 각각
 * bio_wouldblock_error/bio_io_error 또는 -ENOMEM으로 처리.
 *
 * 호출 체인:
 *   blk_zone_wplug_handle_write() / blk_revalidate_seq_zone() →
 *   [disk_get_or_alloc_zone_wplug] → disk_insert_zone_wplug()
 */
static struct blk_zone_wplug *disk_get_or_alloc_zone_wplug(struct gendisk *disk,
					sector_t sector, gfp_t gfp_mask)
{
	unsigned int zno = disk_zone_no(disk, sector); // BIO가 속한 ZNS zone의 ZID
	struct blk_zone_wplug *zwplug;

again:
	zwplug = disk_get_zone_wplug(disk, sector); // 기존 plug가 있으면 재사용
	if (zwplug)
		return zwplug;

	/*
	 * Allocate and initialize a zone write plug with an extra reference
	 * so that it is not freed when the zone write plug becomes idle without
	 * the zone being full.
	 */
	zwplug = mempool_alloc(disk->zone_wplugs_pool, gfp_mask); // max_open/active_zones 기반 bounded pool에서 plug 할당 (GFP_NOIO)
	if (!zwplug)
		return NULL;

	INIT_HLIST_NODE(&zwplug->node); // hash chain 초기화
	refcount_set(&zwplug->ref, 2); // ref=2: hash table + 현재 호출자; NVMe request ref 모델과 유사
	// 초기 ref 2: hash table + 현재 사용자
	spin_lock_init(&zwplug->lock); // zone 단위 spinlock 초기화; SQ doorbell 직렬화 기반
	zwplug->flags = 0; // PLUGGED/NEED_WP_UPDATE/DEAD bit 초기화
	zwplug->zone_no = zno; // plug가 관리하는 ZID 설정
	zwplug->wp_offset = bdev_offset_from_zone_start(disk->part0, sector); // 현재 BIO sector로부터 zone 내 WP offset 초기화
	// 현재 sector로부터 zone 시작까지 offset -> WP 초기 위치
	bio_list_init(&zwplug->bio_list); // 순차 쓰기 대기 BIO list 초기화
	INIT_WORK(&zwplug->bio_work, blk_zone_wplug_bio_work); // plug 풀림 시 비동기 제출할 work_struct 초기화
	INIT_LIST_HEAD(&zwplug->entry); // qd1 worker list node 초기화
	zwplug->disk = disk; // NVMe namespace gendisk 역참조 설정

	/*
	 * Insert the new zone write plug in the hash table. This can fail only
	 * if another context already inserted a plug. Retry from the beginning
	 * in such case.
	 */
	if (!disk_insert_zone_wplug(disk, zwplug)) { // 다른 CPU가 먼저 삽입했으면 재시도
		mempool_free(zwplug, disk->zone_wplugs_pool); // 경쟁 실패 시 할당한 plug를 pool에 즉시 반환
		goto again;
	}

	return zwplug;
}

/*
 * [한국어]
 * blk_zone_wplug_bio_io_error - 대기 중이던 BIO를 I/O 에러로 완료 처리
 *
 * @zwplug: 이 BIO가 속했던 zone write plug(참조 하나를 반환하게 됨).
 * @bio: 실패시킬 BIO.
 * @return: 없음(void).
 *
 * WP 정렬 위반, DEAD zone에 대한 write, 메모리 부족 등으로 plug 단계에서
 * 이미 실패가 확정된 BIO를 NVMe SQ에 아예 올리지 않고 즉시 완료 처리하는
 * 공통 헬퍼. bio_io_error()를 부르기 전에 BIO_ZONE_WRITE_PLUGGING 플래그를
 * 지워 이후 completion 경로(blk_zone_write_plug_bio_endio)가 이 BIO를 다시
 * plug 관련 처리 대상으로 착각하지 않게 한다.
 * 실행 컨텍스트: zwplug->lock을 놓은 상태에서 호출되어야 함(bio_io_error가
 * 다시 이 파일의 완료 콜백을 재귀 호출할 수 있어 self-deadlock 위험).
 * 호출자: disk_zone_wplug_abort(), disk_zone_wplug_submit_bio()(prepare_bio
 * 검증 실패 시).
 * 피호출자: bio_clear_flag(), bio_io_error(), disk_put_zone_wplug(),
 * blk_queue_exit().
 * 참조/카운터 해제: disk_zone_wplug_add_bio()가 plug에 넣을 때 잡았던
 * q_usage_counter 참조(blk_queue_exit)와 plug 참조(disk_put_zone_wplug)를
 * 여기서 함께 반환 — 두 자원 모두 "이 BIO가 언젠가는 완료된다"는 전제로
 * 잡혔던 것이므로 실패 완료 시에도 반드시 반환해야 함.
 *
 * 호출 체인:
 *   disk_zone_wplug_abort() / disk_zone_wplug_submit_bio() →
 *   [blk_zone_wplug_bio_io_error] → bio_io_error(), blk_queue_exit()
 */
static inline void blk_zone_wplug_bio_io_error(struct blk_zone_wplug *zwplug,
					       struct bio *bio)
{
	struct request_queue *q = zwplug->disk->queue;

	bio_clear_flag(bio, BIO_ZONE_WRITE_PLUGGING); // BIO가 더 이상 zone plug 제어 대상이 아님을 표시
	bio_io_error(bio); // NVMe command 없이 BIO 완료를 I/O error로 처리
	disk_put_zone_wplug(zwplug); // BIO가 소유하던 plug reference 해제
	// BIO 추가 시 증가시킨 q_usage_counter 해제
	/* Drop the reference taken by disk_zone_wplug_add_bio(). */
	blk_queue_exit(q); // plug 시점에 증가시킨 q_usage_counter 해제; NVMe queue exit과 동일
}

/*
 * Abort (fail) all plugged BIOs of a zone write plug.
 */
/*
 * [한국어]
 * disk_zone_wplug_abort - plug에 대기 중인 모든 BIO를 실패 처리
 *
 * @zwplug: 대상 zone write plug(zwplug->lock 보유 상태여야 함).
 * @return: 없음(void).
 *
 * zone reset/finish, write 오류로 인한 WP 유실, native zone append와
 * 일반 write 혼용 감지 등 "이 zone의 WP 순서를 더 이상 신뢰할 수 없는"
 * 상황에서 대기열의 BIO를 전부 실패시키는 함수. bio_list_pop()으로
 * 하나씩 꺼내 blk_zone_wplug_bio_io_error()로 완료 처리하므로 NVMe
 * SQ/CID는 전혀 소비되지 않는다. 회전형(qd1) 장치라면 전역 active plug
 * 리스트에서도 제거한다.
 * 실행 컨텍스트: zwplug->lock 보유 — lockdep_assert_held로 강제.
 * 호출자: disk_zone_wplug_set_wp_offset(), blk_zone_wplug_handle_native_zone_append().
 * 피호출자: bio_list_empty/pop(), pr_warn_ratelimited(),
 * blk_zone_wplug_bio_io_error(), blk_queue_zoned_qd1_writes(),
 * list_del_init(), disk_put_zone_wplug().
 * 에러 경로: 이 함수 자체가 에러 처리 경로 — 대기 BIO가 없으면 아무 일도
 *하지 않고 조기 반환.
 *
 * 호출 체인:
 *   disk_zone_wplug_set_wp_offset() /
 *   blk_zone_wplug_handle_native_zone_append() → [disk_zone_wplug_abort] →
 *   blk_zone_wplug_bio_io_error()
 */
static void disk_zone_wplug_abort(struct blk_zone_wplug *zwplug)
{
	struct gendisk *disk = zwplug->disk;
	struct bio *bio;

	lockdep_assert_held(&zwplug->lock);

	if (bio_list_empty(&zwplug->bio_list)) // abort할 대기 BIO가 없으면 즉시 반환
		return;

	pr_warn_ratelimited("%s: zone %u: Aborting plugged BIOs\n", // plug된 BIO들을 강제 실패시킴을 기록
			    zwplug->disk->disk_name, zwplug->zone_no);
	while ((bio = bio_list_pop(&zwplug->bio_list))) // NVMe SQ/CID를 소비하지 않고 plug list의 BIO들을 모두 실패 처리
		blk_zone_wplug_bio_io_error(zwplug, bio);

	zwplug->flags &= ~BLK_ZONE_WPLUG_PLUGGED; // 더 이상 대기 BIO가 없으면 PLUGGED 상태 클리어

	/*
	 * If we are using the per disk zone write plugs worker thread, remove
	 * the zone write plug from the work list and drop the reference we
	 * took when the zone write plug was added to that list.
	 */
	if (blk_queue_zoned_qd1_writes(disk->queue)) { // 회전형(qd1 writes) 장치용 worker list 사용 여부
		spin_lock(&disk->zone_wplugs_list_lock); // qd1 worker list 동기화
		if (!list_empty(&zwplug->entry)) {
			list_del_init(&zwplug->entry); // active plug list에서 제거
			disk_put_zone_wplug(zwplug); // worker list에 추가할 때 획득한 reference 해제
		}
		spin_unlock(&disk->zone_wplugs_list_lock);
	}
}

/*
 * Update a zone write plug condition based on the write pointer offset.
 */
/*
 * [한국어]
 * disk_zone_wplug_update_cond - wp_offset 값으로부터 zone condition 재계산
 *
 * @disk: 대상 gendisk.
 * @zwplug: 갱신할 zone write plug(zwplug->lock 보유 상태여야 함).
 * @return: 없음(void).
 *
 * wp_offset이 바뀔 때마다(BIO 병합/제출/완료 등) zwplug->cond를 EMPTY(WP==0)
 * /FULL(WP==capacity)/ACTIVE(그 사이) 세 가지로 재분류하는 작은 상태
 * 기계. IMP_OPEN/EXP_OPEN/CLOSED처럼 더 세분화된 조건은 캐시 목적상 여기서
 * 만들지 않고 ACTIVE로 뭉뚱그린다(zones_cond 배열과 동일한 단순화 정책).
 * 실행 컨텍스트: zwplug->lock 보유 — lockdep_assert_held로 강제.
 * 호출자: blk_zone_write_plug_bio_merged(), blk_zone_wplug_prepare_bio(),
 * blk_zone_write_plug_init_request(), disk_zone_wplug_set_wp_offset().
 * 피호출자: disk_zone_wplug_is_full().
 *
 * 호출 체인:
 *   (WP를 바꾸는 다수 함수) → [disk_zone_wplug_update_cond] →
 *   disk_zone_wplug_is_full()
 */
static void disk_zone_wplug_update_cond(struct gendisk *disk,
					struct blk_zone_wplug *zwplug)
{
	lockdep_assert_held(&zwplug->lock);

	if (disk_zone_wplug_is_full(disk, zwplug)) // WP가 zone capacity에 도달했는지 검사
		zwplug->cond = BLK_ZONE_COND_FULL; // ZNS FULL 상태로 전이
	else if (!zwplug->wp_offset) // WP offset이 0인지 검사
		zwplug->cond = BLK_ZONE_COND_EMPTY; // ZNS EMPTY 상태로 전이
	else
		zwplug->cond = BLK_ZONE_COND_ACTIVE; // 그 외에는 ZNS ACTIVE 상태로 유지
}

/*
 * Set a zone write plug write pointer offset to the specified value.
 * This aborts all plugged BIOs, which is fine as this function is called for
 * a zone reset operation, a zone finish operation or if the zone needs a wp
 * update from a report zone after a write error.
 */
/*
 * [한국어] (영어 요약 보강)
 * disk_zone_wplug_set_wp_offset - WP offset을 절대값으로 강제 재설정
 *
 * @disk: 대상 gendisk.
 * @zwplug: 갱신할 plug(zwplug->lock 보유 상태여야 함).
 * @wp_offset: 새로 설정할 WP offset(512B sector 단위, zone 시작 기준).
 * @return: 없음(void).
 *
 * zone reset(WP를 0으로)/finish(WP를 zone 크기로)가 완료됐거나, write
 * 오류로 WP를 잃어버려 Report Zones 결과로 다시 동기화하는 경우에
 * 호출된다. 대기 중이던 BIO들은 이 시점 이후로는 위치를 다시 신뢰할 수
 * 없으므로 disk_zone_wplug_abort()로 모두 실패 처리하며(reset/finish
 * 도중 진행 중이던 write는 어차피 실패할 운명이므로 문제 없음), WP가
 * 0(EMPTY)이거나 zone capacity(FULL)에 도달하면 이 plug를 더 이상 쓸 일이
 * 없으므로 DEAD로 표시해 자원을 회수한다.
 * 실행 컨텍스트: zwplug->lock 보유 — lockdep_assert_held로 강제.
 * 호출자: blk_zone_reset_bio_endio(), blk_zone_reset_all_bio_endio(),
 * blk_zone_finish_bio_endio(), disk_zone_wplug_sync_wp_offset().
 * 피호출자: disk_zone_wplug_update_cond(), disk_zone_wplug_abort(),
 * disk_zone_wplug_is_full(), disk_mark_zone_wplug_dead().
 *
 * 호출 체인:
 *   blk_zone_reset_bio_endio() 등 → [disk_zone_wplug_set_wp_offset] →
 *   disk_zone_wplug_abort(), disk_mark_zone_wplug_dead()
 */
static void disk_zone_wplug_set_wp_offset(struct gendisk *disk,
					  struct blk_zone_wplug *zwplug,
					  unsigned int wp_offset)
{
	lockdep_assert_held(&zwplug->lock);

	/* Update the zone write pointer and abort all plugged BIOs. */
	zwplug->flags &= ~BLK_ZONE_WPLUG_NEED_WP_UPDATE; // WP가 동기화됨; NEED_WP_UPDATE 복구 flag 클리어
	zwplug->wp_offset = wp_offset; // ZNS write pointer offset을 강제로 재설정
	// WP 갱신 및 NEED_WP_UPDATE 클리어
	disk_zone_wplug_update_cond(disk, zwplug); // 새 WP에 따라 EMPTY/FULL/ACTIVE 상태 갱신

	disk_zone_wplug_abort(zwplug); // reset/finish/report 후에는 대기 BIO들이 무효화되므로 abort
	if (!zwplug->wp_offset || disk_zone_wplug_is_full(disk, zwplug)) // WP가 0이면 EMPTY, full이면 더 이상의 write 불가
		disk_mark_zone_wplug_dead(zwplug); // zone plug를 DEAD로 표시하여 추가 CID 할당 차단
}

/*
 * [한국어]
 * blk_zone_wp_offset - Report Zones로 얻은 zone descriptor에서 WP offset 산출
 *
 * @zone: 디바이스가 보고한 zone descriptor(wp/start/cond 필드 사용).
 * @return: zone 시작 기준 상대 WP offset(512B sector 단위), 또는 UINT_MAX
 *          (conventional/full/offline/readonly처럼 유효한 WP가 없는 zone).
 *
 * NVMe ZNS Report Zones 응답의 절대 WP(zone->wp, 디스크 전체 기준 sector)를
 * zone write plug가 쓰는 상대 offset(zone->wp - zone->start) 표현으로
 * 변환한다. open/active/closed 계열만 실제 WP 위치가 의미 있고, EMPTY는
 * 정의상 0, 그 외(FULL/conventional/offline/readonly)는 "WP 개념이 없음"을
 * UINT_MAX라는 sentinel 값으로 표현한다.
 * 실행 컨텍스트: 순수 계산 함수 — 어디서든 호출 가능, 부작용 없음.
 * 호출자: disk_zone_wplug_sync_wp_offset().
 * 피호출자: 없음(단순 산술/switch).
 *
 * 호출 체인:
 *   disk_zone_wplug_sync_wp_offset() → [blk_zone_wp_offset]
 */
static unsigned int blk_zone_wp_offset(struct blk_zone *zone)
{
	switch (zone->cond) {
	case BLK_ZONE_COND_IMP_OPEN:
	case BLK_ZONE_COND_EXP_OPEN:
	case BLK_ZONE_COND_CLOSED:
	case BLK_ZONE_COND_ACTIVE:
		return zone->wp - zone->start; // open/active zone의 WP는 zone 시작 + wp_offset
		// ZNS active/open zone의 WP는 start + wp_offset
	case BLK_ZONE_COND_EMPTY:
		return 0; // EMPTY zone의 WP는 zone 시작과 동일
	case BLK_ZONE_COND_FULL:
	case BLK_ZONE_COND_NOT_WP:
	case BLK_ZONE_COND_OFFLINE:
	case BLK_ZONE_COND_READONLY:
	default:
		/*
		 * Conventional, full, offline and read-only zones do not have
		 * a valid write pointer.
		 */
		return UINT_MAX; // conventional/full/offline/readonly zone은 유효 WP 없음
	}
}

/*
 * [한국어]
 * disk_zone_wplug_sync_wp_offset - Report Zones 결과로 plug의 WP를 동기화
 *
 * @disk: 대상 gendisk.
 * @zone: 디바이스가 보고한 zone descriptor.
 * @return: 이 zone에 대해 계산된 WP offset(blk_zone_wp_offset 결과) — plug가
 *          없어도 항상 값을 반환(호출자가 그대로 활용 가능).
 *
 * write 오류 등으로 zwplug->flags에 BLK_ZONE_WPLUG_NEED_WP_UPDATE가 설정된
 * plug만 실제로 갱신한다 — WP를 신뢰할 수 있는 상태의 plug는 Report
 * Zones가 오래된 스냅샷을 가져와도 덮어쓰지 않는다(host 측이 이미 더
 * 최신 정보를 갖고 있을 수 있으므로). 갱신이 필요하면
 * disk_zone_wplug_set_wp_offset()으로 위임해 대기 BIO abort까지 함께
 * 처리한다.
 * 실행 컨텍스트: 어떤 컨텍스트에서도 호출 가능 — plug 자체의 lock만
 * 잡는다(disk_get_zone_wplug/disk_put_zone_wplug로 참조 관리).
 * 호출자: disk_report_zone(), blk_revalidate_seq_zone().
 * 피호출자: blk_zone_wp_offset(), disk_get_zone_wplug(),
 * disk_zone_wplug_set_wp_offset(), disk_put_zone_wplug().
 *
 * 호출 체인:
 *   disk_report_zone() / blk_revalidate_seq_zone() →
 *   [disk_zone_wplug_sync_wp_offset] → disk_zone_wplug_set_wp_offset()
 */
static unsigned int disk_zone_wplug_sync_wp_offset(struct gendisk *disk,
						   struct blk_zone *zone)
{
	struct blk_zone_wplug *zwplug;
	unsigned int wp_offset = blk_zone_wp_offset(zone); // ZNS zone descriptor로부터 512B sector 단위 wp_offset 산출

	zwplug = disk_get_zone_wplug(disk, zone->start); // 해당 zone의 plug가 있으면 WP 동기화
	if (zwplug) {
		unsigned long flags;

		spin_lock_irqsave(&zwplug->lock, flags); // plug의 WP와 cond를 직렬화하여 갱신
		if (zwplug->flags & BLK_ZONE_WPLUG_NEED_WP_UPDATE) // write 오류 등으로 WP 불확실할 때만 동기화
			disk_zone_wplug_set_wp_offset(disk, zwplug, wp_offset); // Report Zones 결과로 WP를 갱신하고 대기 BIO abort
		spin_unlock_irqrestore(&zwplug->lock, flags);
		disk_put_zone_wplug(zwplug); // 조회용 reference 해제
	}

	return wp_offset;
}

/**
 * disk_report_zone - Report one zone
 * @disk:	Target disk
 * @zone:	The zone to report
 * @idx:	The index of the zone in the overall zone report
 * @args:	report zones callback and data
 *
 * Description:
 *    Helper function for block device drivers to report one zone of a zone
 *    report initiated with blkdev_report_zones(). The zone being reported is
 *    specified by @zone and used to update, if necessary, the zone write plug
 *    information for the zone. If @args specifies a user callback function,
 *    this callback is executed.
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * disk_report_zone - report_zones 드라이버 콜백을 위한 zone 하나 처리 헬퍼
 *
 * @disk: 대상 gendisk.
 * @zone: 드라이버가 채운 zone descriptor(필요 시 이 함수가 cond를 수정).
 * @idx: 이번 report 요청 내에서 이 zone의 순번.
 * @args: 상위 계층이 blkdev_do_report_zones()에 넘긴 콜백/데이터/
 *        report_active 플래그(NULL 가능 — 드라이버 내부 전용 report도 지원).
 * @return: args->cb 콜백의 반환값, 또는 콜백이 없으면 0.
 *
 * NVMe(nvme_report_zones) 등 저수준 드라이버가 Report Zones 커맨드로 zone
 * 하나를 확인할 때마다 호출하도록 설계된 공용 헬퍼(EXPORT_SYMBOL_GPL로
 * 드라이버에 공개). report_active가 설정되어 있으면(cached report의
 * fallback 상황) IMP_OPEN/EXP_OPEN/CLOSED를 ACTIVE로 축소해 캐시와 일관된
 * 표현을 유지하고, zone plug 해시가 있다면 disk_zone_wplug_sync_wp_offset()
 * 으로 host 측 WP 캐시도 최신 정보로 맞춘다. 마지막으로 사용자가 지정한
 * 콜백이 있으면 호출해 zone 정보를 상위로 전달한다.
 * 실행 컨텍스트: 드라이버의 report_zones 구현이 호출하는 컨텍스트를
 * 그대로 물려받음 — 대개 프로세스 컨텍스트.
 * 호출자: NVMe 드라이버 등 disk->fops->report_zones 구현체
 * (drivers/nvme/host/zns.c의 nvme_report_zones 등).
 * 피호출자: disk_zone_wplug_sync_wp_offset(), args->cb().
 * 에러 경로: 콜백이 음수를 반환하면 그 값을 그대로 전달해 드라이버 쪽
 * report 루프가 중단되도록 함.
 *
 * 호출 체인:
 *   nvme_report_zones() → [disk_report_zone] →
 *   disk_zone_wplug_sync_wp_offset(), args->cb()
 */
int disk_report_zone(struct gendisk *disk, struct blk_zone *zone,
		     unsigned int idx, struct blk_report_zones_args *args)
{
	if (args && args->report_active) { // cached report fallback 시 open/closed/imp_open을 active로 축소
		/*
		 * If we come here, then this is a report zones as a fallback
		 * for a cached report. So collapse the implicit open, explicit
		 * open and closed conditions into the active zone condition.
		 */
		switch (zone->cond) {
		case BLK_ZONE_COND_IMP_OPEN:
		case BLK_ZONE_COND_EXP_OPEN:
		case BLK_ZONE_COND_CLOSED:
			zone->cond = BLK_ZONE_COND_ACTIVE;
			break;
		default:
			break;
		}
	}

	if (disk->zone_wplugs_hash) // zone plug hash가 초기화된 경우에만 WP 동기화
		disk_zone_wplug_sync_wp_offset(disk, zone); // Report Zones로 얻은 WP를 plug에 반영

	if (args && args->cb) // 사용자 콜백이 있으면 zone descriptor 전달
		return args->cb(zone, idx, args->data); // NVMe ZNS zone 정보를 상위 계층으로 전달

	return 0;
}
EXPORT_SYMBOL_GPL(disk_report_zone);

/*
 * [한국어]
 * blkdev_report_zone_cb - report_zones 콜백: zone descriptor를 커널 버퍼로 복사
 *
 * @zone: 드라이버가 채운 zone descriptor.
 * @idx: report 요청 내 순번(fallback은 항상 1개만 요청하므로 사실상 0).
 * @data: 결과를 받을 struct blk_zone* 포인터(사용자 공간이 아닌 커널 공간).
 * @return: 항상 0(실패할 일이 없는 단순 memcpy).
 *
 * blkdev_report_zone_fallback()이 blk_report_zones_args.cb로 등록하는
 * 콜백 — blkdev_copy_zone_to_user()와 달리 사용자 공간이 아니라 커널
 * 스택/호출자가 제공한 blk_zone 구조체에 그대로 복사한다(캐시가 실패해
 * 커널 내부에서 정확한 정보를 다시 얻어야 할 때 사용).
 * 실행 컨텍스트: 어떤 컨텍스트에서도 안전(단순 memcpy, 블로킹 없음).
 * 호출자: disk_report_zone()이 args->cb()로 호출.
 * 피호출자: memcpy().
 *
 * 호출 체인:
 *   disk_report_zone() → [blkdev_report_zone_cb]
 */
static int blkdev_report_zone_cb(struct blk_zone *zone, unsigned int idx,
				 void *data)
{
	memcpy(data, zone, sizeof(struct blk_zone));
	return 0;
}

/*
 * [한국어]
 * blkdev_report_zone_fallback - 캐시를 못 쓸 때 zone 하나를 실제로 재조회
 *
 * @bdev: 대상 block_device.
 * @sector: 조회할 zone의 시작 sector.
 * @zone: 결과를 받을 blk_zone 구조체(호출자 소유).
 * @return: 0(성공) 또는 음수 errno(-EIO/-EOPNOTSUPP 등).
 *
 * blkdev_get_zone_info()의 cached 경로가 신뢰할 수 없다고 판단했을 때
 * (blkdev_has_cached_report_zones()==false, 또는 WP가 NEED_WP_UPDATE
 * 상태) 실제로 디바이스에 Report Zones 커맨드 하나를 내려 정확한 zone
 * 정보를 얻는다. report_active=true를 지정해 disk_report_zone()이
 * open/closed 계열을 ACTIVE로 축소하도록 한다(캐시와 표현을 통일).
 * 실행 컨텍스트: blkdev_do_report_zones()를 그대로 호출하므로 프로세스
 * 컨텍스트, memalloc_noXX 제약도 동일하게 적용.
 * 호출자: blkdev_get_zone_info().
 * 피호출자: blkdev_do_report_zones() → disk->fops->report_zones →
 * nvme_report_zones().
 * 에러 경로: report_zones 호출 자체가 실패하면 그 음수 errno를 그대로
 * 반환하고, 성공했지만 zone을 하나도 못 받았으면(error==0) -EIO로 변환
 * (디바이스가 요청 범위에 대해 아무것도 보고하지 않은 비정상 상황).
 *
 * 호출 체인:
 *   blkdev_get_zone_info() → [blkdev_report_zone_fallback] →
 *   blkdev_do_report_zones() → nvme_report_zones()
 */
static int blkdev_report_zone_fallback(struct block_device *bdev,
				       sector_t sector, struct blk_zone *zone)
{
	struct blk_report_zones_args args = {
		.cb = blkdev_report_zone_cb,
		.data = zone,
		.report_active = true,
	};
	int error;

	error = blkdev_do_report_zones(bdev, sector, 1, &args); // NVMe Report Zones를 한 zone만큼 fallback 실행
	if (error < 0) // NVMe command 실패 시 오류 반환
		return error;
	if (error == 0) // 디바이스가 zone을 report하지 않으면 I/O error
		return -EIO;
	return 0;
}

/*
 * For devices that natively support zone append operations, we do not use zone
 * write plugging for zone append writes, which makes the zone condition
 * tracking invalid once zone append was used.  In that case fall back to a
 * regular report zones to get correct information.
 */
/*
 * [한국어] (영어 요약 보강)
 * blkdev_has_cached_report_zones - 이 디바이스에서 cached report를 신뢰할 수 있나
 *
 * @bdev: 대상 block_device.
 * @return: true면 커널 캐시(zones_cond+zone plug)만으로 응답 가능,
 *          false면 항상 실제 Report Zones 커맨드로 확인해야 함.
 *
 * native zone append를 지원하는 디바이스는 append 커맨드가 zone write
 * plug를 거치지 않고 바로 SQ에 들어가므로(blk_zone_wplug_handle_native_zone_append
 * 참고), plug가 WP를 계속 추적하지 못해 캐시가 부정확해질 수 있다. 이
 * 함수는 (1) 애초에 zone 자원(plug)을 쓰는 디바이스인지, (2) zone
 * append를 에뮬레이션하는 디바이스이거나 아직 한 번도 native zone append
 * 를 쓴 적이 없는지(GD_ZONE_APPEND_USED)를 함께 확인해 캐시 신뢰 여부를
 * 결정한다.
 * 실행 컨텍스트: 순수 조회 — 어디서든 호출 가능.
 * 호출자: blkdev_get_zone_info(), blkdev_report_zones_cached().
 * 피호출자: disk_need_zone_resources(), bdev_emulates_zone_append(),
 * test_bit().
 *
 * 호출 체인:
 *   blkdev_get_zone_info() / blkdev_report_zones_cached() →
 *   [blkdev_has_cached_report_zones]
 */
static inline bool blkdev_has_cached_report_zones(struct block_device *bdev)
{
	return disk_need_zone_resources(bdev->bd_disk) && // request-based NVMe 장치 또는 zone append emulation 필요 여부
		(bdev_emulates_zone_append(bdev) || // SW zone append emulation 경로(DM 등)
		 !test_bit(GD_ZONE_APPEND_USED, &bdev->bd_disk->state)); // native zone append 사용 시 cached WP가 신뢰할 수 없음
}

/**
 * blkdev_get_zone_info - Get a single zone information from cached data
 * @bdev:   Target block device
 * @sector: Sector contained by the target zone
 * @zone:   zone structure to return the zone information
 *
 * Description:
 *    Get the zone information for the zone containing @sector using the zone
 *    write plug of the target zone, if one exist, or the disk zone condition
 *    array otherwise. The zone condition may be reported as being
 *    the BLK_ZONE_COND_ACTIVE condition for a zone that is in the implicit
 *    open, explicit open or closed condition.
 *
 *    Returns 0 on success and a negative error code on failure.
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * blkdev_get_zone_info - 캐시된 정보로 zone 하나의 정보를 구성
 *
 * @bdev: 대상 block_device.
 * @sector: 조회할 zone에 속하는 sector.
 * @zone: 결과를 채울 blk_zone 구조체(호출 전 caller가 소유, 이 함수가
 *        memset으로 초기화 후 채움).
 * @return: 0(성공) 또는 음수 errno.
 *
 * blkdev_report_zones_cached()가 zone마다 호출하는 핵심 로직 — 실제
 * Report Zones 커맨드를 매번 내리는 대신, disk->zones_cond 캐시 배열과
 * (있다면) 해당 zone의 zone write plug를 이용해 zone 정보를 재구성한다.
 * conventional zone은 캐시된 condition만으로 즉시 답할 수 있고,
 * sequential zone은 plug가 없으면 EMPTY/FULL 둘 중 하나로 확정(plug가
 * 없다는 것 자체가 "쓰기 진행 중이 아님"을 의미), plug가 있으면 그
 * wp_offset/cond를 그대로 사용한다. 단, plug의 NEED_WP_UPDATE 플래그가
 * 서 있으면(write 오류로 WP 불확실) 캐시를 포기하고
 * blkdev_report_zone_fallback()으로 실제 재조회한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 — RCU read-side로 zones_cond를
 * 스냅샷하고, plug가 있으면 zwplug->lock으로 WP/cond를 안전하게 읽는다.
 * 호출자: blkdev_report_zones_cached().
 * 피호출자: blkdev_has_cached_report_zones(), disk_zone_is_last(),
 * disk_get_zone_wplug(), blkdev_report_zone_fallback(),
 * disk_put_zone_wplug().
 * 에러 경로: zoned 아님/capacity 초과는 조기 -EOPNOTSUPP/-EINVAL; NEED_WP_UPDATE
 * 상태는 fallback 결과(0 또는 음수 errno)를 그대로 반환.
 *
 * 호출 체인:
 *   blkdev_report_zones_cached() → [blkdev_get_zone_info] →
 *   (fallback) blkdev_report_zone_fallback() → nvme_report_zones()
 */
int blkdev_get_zone_info(struct block_device *bdev, sector_t sector,
			 struct blk_zone *zone)
{
	struct gendisk *disk = bdev->bd_disk;
	sector_t zone_sectors = bdev_zone_sectors(bdev); // ZNS zone 크기(chunk_sectors)로 zone 범위 계산
	struct blk_zone_wplug *zwplug;
	unsigned long flags;
	u8 *zones_cond;

	if (!bdev_is_zoned(bdev)) // ZNS가 아니면 zone info 조회 불가
		return -EOPNOTSUPP;

	if (sector >= get_capacity(disk)) // NVMe namespace capacity 초과 검증
		return -EINVAL;

	memset(zone, 0, sizeof(*zone)); // 반환할 zone descriptor 초기화
	sector = bdev_zone_start(bdev, sector); // sector를 zone 시작으로 정렬

	if (!blkdev_has_cached_report_zones(bdev)) // cached report를 사용할 수 없으면 NVMe command로 fallback
		return blkdev_report_zone_fallback(bdev, sector, zone); // NVMe Report Zones를 한 zone 조회

	rcu_read_lock(); // zones_cond read-side RCU lock
	zones_cond = rcu_dereference(disk->zones_cond); // zone condition cache 포인터 snapshot
	if (!disk->zone_wplugs_hash || !zones_cond) { // plug hash나 condition cache가 준비되지 않았으면 fallback
		rcu_read_unlock();
		return blkdev_report_zone_fallback(bdev, sector, zone);
	}
	zone->cond = zones_cond[disk_zone_no(disk, sector)]; // cache에서 ZNS zone condition 읽기
	rcu_read_unlock();

	zone->start = sector; // zone 시작 sector 설정
	zone->len = zone_sectors; // zone 길이 설정

	/*
	 * If this is a conventional zone, we do not have a zone write plug and
	 * can report the zone immediately.
	 */
	if (zone->cond == BLK_ZONE_COND_NOT_WP) { // conventional zone은 WP 개념 없음
		zone->type = BLK_ZONE_TYPE_CONVENTIONAL; // conventional zone type 지정
		zone->capacity = zone_sectors; // conventional zone은 전체 zone size 사용 가능
		// conventional zone은 WP 개념 없음 -> ULLONG_MAX
		zone->wp = ULLONG_MAX; // WP가 의미 없으므로 ULLONG_MAX 반환
		return 0;
	}

	/*
	 * This is a sequential write required zone. If the zone is read-only or
	 * offline, only set the zone write pointer to an invalid value and
	 * report the zone.
	 */
	zone->type = BLK_ZONE_TYPE_SEQWRITE_REQ; // sequential write required zone type 지정
	if (disk_zone_is_last(disk, zone)) // 마지막 zone인지 확인
		zone->capacity = disk->last_zone_capacity; // 마지막 zone은 축소된 capacity 사용
	else
		zone->capacity = disk->zone_capacity; // 일반 zone은 zone_capacity 사용
		// ZNS sequential write required zone 설정

	if (zone->cond == BLK_ZONE_COND_READONLY || // readonly/offline zone은 쓰기 불가
	    zone->cond == BLK_ZONE_COND_OFFLINE) {
		zone->wp = ULLONG_MAX; // 유효하지 않은 WP 표시
		return 0;
	}

	/*
	 * If the zone does not have a zone write plug, it is either full or
	 * empty, as we otherwise would have a zone write plug for it. In this
	 * case, set the write pointer accordingly and report the zone.
	 * Otherwise, if we have a zone write plug, use it.
	 */
	zwplug = disk_get_zone_wplug(disk, sector); // 해당 zone의 plug 조회; 없으면 empty/full로 간주
	if (!zwplug) {
		if (zone->cond == BLK_ZONE_COND_FULL) // plug가 없고 FULL이면 WP를 ULLONG_MAX로
			zone->wp = ULLONG_MAX; // FULL zone은 추가 쓰기 불가
		else
			zone->wp = sector; // EMPTY이면 WP는 zone 시작
		return 0;
		// plug 없으면 FULL 또는 EMPTY
	}

	spin_lock_irqsave(&zwplug->lock, flags); // plug의 WP 상태를 직렬화
	if (zwplug->flags & BLK_ZONE_WPLUG_NEED_WP_UPDATE) { // WP 불확실 시 cached report를 신뢰할 수 없음
		spin_unlock_irqrestore(&zwplug->lock, flags);
		disk_put_zone_wplug(zwplug); // fallback 전 plug 조회 reference 해제
// NVMe Report Zones로 정확한 WP 재확인
		// WP 불확실 시 NVMe Report Zones로 fallback
		return blkdev_report_zone_fallback(bdev, sector, zone);
	}
	zone->cond = zwplug->cond; // plug의 condition을 zone descriptor에 반영
	zone->wp = sector + zwplug->wp_offset; // plug wp_offset으로부터 ZNS write pointer 계산
	spin_unlock_irqrestore(&zwplug->lock, flags);

	disk_put_zone_wplug(zwplug); // 조회용 reference 해제

	return 0;
}
EXPORT_SYMBOL_GPL(blkdev_get_zone_info);

/**
 * blkdev_report_zones_cached - Get cached zones information
 * @bdev:     Target block device
 * @sector:   Sector from which to report zones
 * @nr_zones: Maximum number of zones to report
 * @cb:       Callback function called for each reported zone
 * @data:     Private data for the callback function
 *
 * Description:
 *    Similar to blkdev_report_zones() but instead of calling into the low level
 *    device driver to get the zone report from the device, use
 *    blkdev_get_zone_info() to generate the report from the disk zone write
 *    plugs and zones condition array. Since calling this function without a
 *    callback does not make sense, @cb must be specified.
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * blkdev_report_zones_cached - 캐시된 정보로 zone report를 생성
 *
 * @bdev: 대상 block_device.
 * @sector: report를 시작할 sector가 포함된 zone부터.
 * @nr_zones: 최대 조회 zone 개수.
 * @cb: zone마다 호출될 콜백(필수 — NULL이면 의미가 없으므로 -EOPNOTSUPP).
 * @data: 콜백에 전달될 컨텍스트.
 * @return: 실제 report한 zone 개수, 또는 음수 errno.
 *
 * blkdev_report_zones()와 기능은 같지만 실제 디바이스에 커맨드를 내리지
 * 않고 blkdev_get_zone_info()로 zone마다 커널 캐시(zones_cond + zone
 * write plug)에서 정보를 구성한다는 점이 다르다. 캐시를 아예 신뢰할 수
 * 없는 디바이스(blkdev_has_cached_report_zones()==false)라면 곧바로
 * blkdev_do_report_zones()로 fallback해 실제 Report Zones를 수행한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 처리) — 각 zone 조회 사이에
 * 블로킹은 없지만 zone 수가 많으면 루프가 길어질 수 있다.
 * 호출자: blkdev_report_zones_ioctl()(BLKREPORTZONEV2, BLK_ZONE_REP_CACHED
 * 플래그 지정 시).
 * 피호출자: blkdev_has_cached_report_zones(), blkdev_do_report_zones(),
 * blkdev_get_zone_info(), cb().
 * 에러 경로: cb가 없거나 zoned가 아니면 -EOPNOTSUPP; 루프 중
 * blkdev_get_zone_info()나 cb()가 실패하면 그 값을 즉시 반환(부분 결과는
 * 버려짐 — ioctl 계층에서 재시도 책임).
 *
 * 호출 체인:
 *   blkdev_report_zones_ioctl() → [blkdev_report_zones_cached] →
 *   blkdev_get_zone_info() / blkdev_do_report_zones()
 */
int blkdev_report_zones_cached(struct block_device *bdev, sector_t sector,
			unsigned int nr_zones, report_zones_cb cb, void *data)
{
	struct gendisk *disk = bdev->bd_disk;
	sector_t capacity = get_capacity(disk); // NVMe namespace capacity
	sector_t zone_sectors = bdev_zone_sectors(bdev); // ZNS zone size
	unsigned int idx = 0;
	struct blk_zone zone;
	int ret;

	if (!cb || !bdev_is_zoned(bdev) || // callback 필수; ZNS zone descriptor를 사용자에게 전달
	    WARN_ON_ONCE(!disk->fops->report_zones))
		return -EOPNOTSUPP;

	if (!nr_zones || sector >= capacity) // report할 zone이 없거나 용량 초과시 no-op
		return 0;

	if (!blkdev_has_cached_report_zones(bdev)) { // cached path 사용 불가 시 NVMe command로 fallback
		struct blk_report_zones_args args = {
			.cb = cb,
			.data = data,
			.report_active = true,
		};

		return blkdev_do_report_zones(bdev, sector, nr_zones, &args); // report_zones op (nvme_report_zones) 호출
	}

	for (sector = bdev_zone_start(bdev, sector); // zone 단위로 순회; 캐시 유효하면 NVMe SQ/CID 사용 안 함
	     sector < capacity && idx < nr_zones;
	     sector += zone_sectors, idx++) {
		ret = blkdev_get_zone_info(bdev, sector, &zone); // 커널 캐시에서 zone 정보 조회
		if (ret) // 캐시 조회 실패(fallback 포함) 시 루프를 중단하고 에러 전파
			return ret;

		ret = cb(&zone, idx, data); // 조회된 ZNS zone descriptor를 사용자 콜백으로 전달
		if (ret) // 콜백이 실패(예: copy_to_user 실패)를 알리면 나머지 zone은 조회하지 않고 중단
			return ret;
	}

	return idx;
}
EXPORT_SYMBOL_GPL(blkdev_report_zones_cached);

/*
 * [한국어]
 * blk_zone_reset_bio_endio - REQ_OP_ZONE_RESET 완료 처리
 *
 * @bio: 완료된 Reset BIO(bi_status가 이미 BLK_STS_OK로 확인된 상태).
 * @return: 없음(void).
 *
 * NVMe ZNS Zone Management Send(Reset) 커맨드가 성공적으로 완료된 뒤
 * 호출되어 host 측 WP 캐시를 실제 디바이스 상태(WP==zone 시작, 즉 0)와
 * 맞춘다. plug가 있으면 disk_zone_wplug_set_wp_offset(0)으로 WP를
 * 되돌리는데, 이 과정에서 혹시 남아있는 대기 BIO들은 모두 abort된다 —
 * reset과 동시에 진행 중이던 write는 사용자 책임(동기화 없이 섞어 쓴
 * 경우)이므로 실패해도 문제없다는 문서화된 전제. plug가 아예 없다면(이미
 * EMPTY/FULL로 소멸된 상태) zones_cond 캐시만 EMPTY로 직접 갱신한다.
 * 실행 컨텍스트: BIO 완료 콜백 컨텍스트(인터럽트/softirq일 수 있음) —
 * zwplug->lock을 irqsave 계열로 잡는다.
 * 호출자: blk_zone_mgmt_bio_endio()의 REQ_OP_ZONE_RESET 분기.
 * 피호출자: disk_get_zone_wplug(), disk_zone_wplug_set_wp_offset(),
 * disk_put_zone_wplug(), disk_zone_set_cond().
 *
 * 호출 체인:
 *   nvme_complete_rq() → bio_endio() → blk_zone_mgmt_bio_endio() →
 *   [blk_zone_reset_bio_endio] → disk_zone_wplug_set_wp_offset()
 */
static void blk_zone_reset_bio_endio(struct bio *bio)
{
	struct gendisk *disk = bio->bi_bdev->bd_disk; // 완료된 BIO의 NVMe namespace gendisk
	sector_t sector = bio->bi_iter.bi_sector; // 완료된 BIO의 시작 sector로 ZID 식별
	struct blk_zone_wplug *zwplug;

	/*
	 * If we have a zone write plug, set its write pointer offset to 0.
	 * This will abort all BIOs plugged for the target zone. It is fine as
	 * resetting zones while writes are still in-flight will result in the
	 * writes failing anyway.
	 */
	zwplug = disk_get_zone_wplug(disk, sector); // 해당 ZID의 plug 조회
	if (zwplug) {
		unsigned long flags;

		spin_lock_irqsave(&zwplug->lock, flags);
		disk_zone_wplug_set_wp_offset(disk, zwplug, 0); // Reset 완료 후 WP를 0으로 동기화
		spin_unlock_irqrestore(&zwplug->lock, flags);
		disk_put_zone_wplug(zwplug);
	} else {
		disk_zone_set_cond(disk, sector, BLK_ZONE_COND_EMPTY); // plug가 없으면 zones_cond를 EMPTY로 갱신
	}
}

/*
 * [한국어]
 * blk_zone_reset_all_bio_endio - REQ_OP_ZONE_RESET_ALL 완료 처리
 *
 * @bio: 완료된 Reset All BIO.
 * @return: 없음(void).
 *
 * 디스크 전체 zone을 한 번에 초기화하는 NVMe Reset All 커맨드가 끝난 뒤,
 * 존재하는 모든 zone write plug를 순회하며 WP를 0으로 되돌리고(활성 plug가
 * 하나도 없으면 이 순회 자체를 건너뛰는 빠른 경로 포함), 이어서 전체
 * capacity를 zone 크기 단위로 순회하며 zones_cond 캐시 배열도 모두 EMPTY로
 * 표시한다. 마지막으로 native zone append 사용 이력(GD_ZONE_APPEND_USED)도
 * 초기화 — reset 이후에는 다시 처음부터 append 안전성을 재확인해야 하므로.
 * 실행 컨텍스트: BIO 완료 콜백 — 해시 전체를 순회하는 동안 RCU read
 * lock을 유지하고, plug 개별 갱신은 zwplug->lock으로 보호.
 * 호출자: blk_zone_mgmt_bio_endio()의 REQ_OP_ZONE_RESET_ALL 분기.
 * 피호출자: disk_zone_wplugs_hash_size(), disk_zone_wplug_set_wp_offset(),
 * disk_zone_set_cond(), clear_bit().
 *
 * 호출 체인:
 *   nvme_complete_rq() → bio_endio() → blk_zone_mgmt_bio_endio() →
 *   [blk_zone_reset_all_bio_endio] → disk_zone_wplug_set_wp_offset()
 */
static void blk_zone_reset_all_bio_endio(struct bio *bio)
{
	struct gendisk *disk = bio->bi_bdev->bd_disk; // Reset All 완료된 BIO의 gendisk
	sector_t capacity = get_capacity(disk); // NVMe namespace capacity
	struct blk_zone_wplug *zwplug;
	unsigned long flags;
	sector_t sector;
	unsigned int i;

	if (atomic_read(&disk->nr_zone_wplugs)) { // 활성 plug가 있을 때만 전체 순회
		/* Update the condition of all zone write plugs. */
		rcu_read_lock();
		for (i = 0; i < disk_zone_wplugs_hash_size(disk); i++) { // 모든 hash bucket을 순회하며 plug 상태 갱신
			hlist_for_each_entry_rcu(zwplug, // 각 bucket의 충돌 체인을 RCU-safe하게 순회
						 &disk->zone_wplugs_hash[i],
						 node) {
				spin_lock_irqsave(&zwplug->lock, flags); // plug별 WP 갱신 직렬화
				disk_zone_wplug_set_wp_offset(disk, zwplug, 0); // Reset All로 모든 plug의 WP를 0으로 초기화
				spin_unlock_irqrestore(&zwplug->lock, flags);
			}
		}
		rcu_read_unlock();
	}

	/* Update the cached zone conditions. */
	for (sector = 0; sector < capacity; // capacity 전체를 순회하며 zones_cond 초기화
	     sector += bdev_zone_sectors(bio->bi_bdev))
		disk_zone_set_cond(disk, sector, BLK_ZONE_COND_EMPTY); // 모든 zone을 EMPTY로 표시
	clear_bit(GD_ZONE_APPEND_USED, &disk->state); // Reset All 후 native zone append 사용 기록 클리어
	// reset all 후 zone append 사용 기록 클리어
}

/*
 * [한국어]
 * blk_zone_finish_bio_endio - REQ_OP_ZONE_FINISH 완료 처리
 *
 * @bio: 완료된 Finish BIO.
 * @return: 없음(void).
 *
 * NVMe ZNS Zone Management Send(Finish) 커맨드가 완료되면 해당 zone은
 * 강제로 FULL 상태가 된다(더 이상 쓸 수 없음). plug가 있으면
 * disk_zone_wplug_set_wp_offset()에 zone 전체 크기를 넘겨 WP를 끝까지
 * 채운 것으로 표시하고(그 결과 disk_zone_wplug_is_full()이 true가 되어
 * DEAD로 표시됨), plug가 없다면 zones_cond 캐시만 FULL로 직접 갱신한다.
 * 실행 컨텍스트: BIO 완료 콜백 컨텍스트 — zwplug->lock을 irqsave로 보호.
 * 호출자: blk_zone_mgmt_bio_endio()의 REQ_OP_ZONE_FINISH 분기.
 * 피호출자: disk_get_zone_wplug(), disk_zone_wplug_set_wp_offset(),
 * disk_put_zone_wplug(), disk_zone_set_cond(), bdev_zone_sectors().
 *
 * 호출 체인:
 *   nvme_complete_rq() → bio_endio() → blk_zone_mgmt_bio_endio() →
 *   [blk_zone_finish_bio_endio] → disk_zone_wplug_set_wp_offset()
 */
static void blk_zone_finish_bio_endio(struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev; // Finish 완료된 BIO의 block_device
	struct gendisk *disk = bdev->bd_disk;
	sector_t sector = bio->bi_iter.bi_sector; // 완료된 BIO의 시작 sector
	struct blk_zone_wplug *zwplug;

	/*
	 * If we have a zone write plug, set its write pointer offset to the
	 * zone size. This will abort all BIOs plugged for the target zone. It
	 * is fine as resetting zones while writes are still in-flight will
	 * result in the writes failing anyway.
	 */
	zwplug = disk_get_zone_wplug(disk, sector); // 해당 ZID의 plug 조회
	if (zwplug) {
		unsigned long flags;

		spin_lock_irqsave(&zwplug->lock, flags);
		disk_zone_wplug_set_wp_offset(disk, zwplug, // Finish 완료 후 WP를 zone 크기로 설정 (FULL)
					      bdev_zone_sectors(bdev));
		spin_unlock_irqrestore(&zwplug->lock, flags);
		disk_put_zone_wplug(zwplug);
	} else {
		disk_zone_set_cond(disk, sector, BLK_ZONE_COND_FULL); // plug가 없으면 zones_cond를 FULL로 갱신
	}
}

/*
 * [한국어]
 * blk_zone_mgmt_bio_endio - zone 관리 명령 BIO의 공용 완료 콜백
 *
 * @bio: 완료된 REQ_OP_ZONE_RESET/RESET_ALL/OPEN/CLOSE/FINISH BIO.
 * @return: 없음(void).
 *
 * NVMe ZNS Zone Management Send 커맨드가 완료될 때마다 블록 계층
 * completion 경로에서 호출되어 host 측 WP/condition 캐시를 실제 디바이스
 * 상태와 맞추는 총 진입점. 커맨드가 실패했다면(bi_status != OK) 캐시를
 * 건드리지 않고 그대로 반환 — 실패한 관리 명령은 디바이스 상태를
 * 바꾸지 않았다고 가정. REQ_OP_ZONE_OPEN/CLOSE는 이 파일이 host 측
 * WP/condition을 따로 추적할 필요가 없어(open/close는 WP를 바꾸지 않음)
 * default 분기로 떨어져 아무 것도 하지 않는다.
 * 실행 컨텍스트: BIO 완료 콜백(인터럽트 또는 softirq 컨텍스트 가능).
 * 호출자: bio_endio() — REQ_OP_ZONE_* 계열 BIO의 bi_end_io로 등록된 경로.
 * 피호출자: blk_zone_reset_bio_endio(), blk_zone_reset_all_bio_endio(),
 * blk_zone_finish_bio_endio().
 *
 * 호출 체인:
 *   nvme_complete_rq() → bio_endio() → [blk_zone_mgmt_bio_endio] →
 *   blk_zone_reset_bio_endio() / blk_zone_reset_all_bio_endio() /
 *   blk_zone_finish_bio_endio()
 */
void blk_zone_mgmt_bio_endio(struct bio *bio)
{
	/* If the BIO failed, we have nothing to do. */
	if (bio->bi_status != BLK_STS_OK) // NVMe Zone Management Send 실패 시 WP 캐시를 갱신하지 않음
		return;

	switch (bio_op(bio)) {
	case REQ_OP_ZONE_RESET: // Reset 완료 후 WP 캐시 동기화
		blk_zone_reset_bio_endio(bio);
		return;
	case REQ_OP_ZONE_RESET_ALL: // Reset All 완료 후 전체 WP/condition 초기화
		blk_zone_reset_all_bio_endio(bio);
		return;
	case REQ_OP_ZONE_FINISH: // Finish 완료 후 zone을 FULL로 표시
		blk_zone_finish_bio_endio(bio);
		return;
	default:
		return;
	}
}

/*
 * [한국어] (영어 요약 보강 — 주의: 원문에 섞여 있던 무관한 주석 조각 정리)
 * disk_zone_wplug_schedule_work - plug의 다음 BIO 제출을 workqueue에 예약
 *
 * @disk: 대상 gendisk.
 * @zwplug: work를 스케줄할 plug(zwplug->lock 보유 상태여야 함).
 * @return: 없음(void).
 *
 * NVMe ZNS SSD처럼 여러 zone을 병렬로 다룰 수 있는 장치를 위한 경로 —
 * per-plug work_struct(bio_work)를 disk->zone_wplugs_wq에 큐잉해 process
 * context에서 비동기로 다음 BIO를 제출하게 한다(회전형 qd1 장치는 별도의
 * 전역 워커 스레드를 쓰므로 이 경로를 타지 않는다 — WARN_ON_ONCE로 확인).
 * work item 자체가 plug 구조체에 내장되어 있으므로, work가 실행되기 전에
 * plug가 해제되지 않도록 스케줄 전에 미리 참조를 하나 증가시킨다 — 이
 * 참조는 blk_zone_wplug_bio_work()가 실행 후 반환하거나, 이미 큐에 있어
 * 중복 스케줄이 무시된 경우 이 함수 자신이 즉시 반환한다.
 * 실행 컨텍스트: zwplug->lock 보유 — lockdep_assert_held로 강제.
 * 호출자: blk_zone_wplug_handle_write()(새로 PLUGGED 상태가 됐을 때),
 * disk_zone_wplug_unplug_bio()(완료 후 남은 BIO가 있을 때).
 * 피호출자: refcount_inc(), queue_work(), disk_put_zone_wplug().
 *
 * 호출 체인:
 *   blk_zone_wplug_handle_write() / disk_zone_wplug_unplug_bio() →
 *   [disk_zone_wplug_schedule_work] → queue_work() →
 *   blk_zone_wplug_bio_work()
 */
static void disk_zone_wplug_schedule_work(struct gendisk *disk,
					  struct blk_zone_wplug *zwplug)
{
	lockdep_assert_held(&zwplug->lock);

	/*
	 * Schedule the submission of the next plugged BIO. Taking a reference
	 * to the zone write plug is required as the bio_work belongs to the
	 * plug, and thus we must ensure that the write plug does not go away
	 * while the work is being scheduled but has not run yet.
	 * blk_zone_wplug_bio_work() will release the reference we take here,
	 * and we also drop this reference if the work is already scheduled.
	 */
	WARN_ON_ONCE(!(zwplug->flags & BLK_ZONE_WPLUG_PLUGGED));
	WARN_ON_ONCE(blk_queue_zoned_qd1_writes(disk->queue));
	refcount_inc(&zwplug->ref); // work 실행 전 plug가 해제되지 않도록 reference 추가
	if (!queue_work(disk->zone_wplugs_wq, &zwplug->bio_work)) // process context workqueue에 제출; NVMe ISR과는 별개 스케줄링
		disk_put_zone_wplug(zwplug); // 이미 큐에 있으면 즉시 reference 해제
}

/*
 * [한국어] (영어 요약 보강)
 * disk_zone_wplug_add_bio - BIO를 zone write plug의 대기열에 적재
 *
 * @disk: 대상 gendisk.
 * @zwplug: BIO를 추가할 plug(zwplug->lock 보유 상태여야 함).
 * @bio: 대기열에 넣을 BIO.
 * @nr_segs: 분할(split) 후 확정된 물리 세그먼트 수 — 나중에 back merge
 *           시 재사용하기 위해 BIO의 poll cookie 필드에 저장해 둔다.
 * @return: 없음(void).
 *
 * 이 zone에 대해 이미 다른 write가 진행 중이거나(PLUGGED) 회전형(qd1)
 * 장치라서 즉시 제출할 수 없는 BIO를 plug의 bio_list 꼬리에 추가한다.
 * 이때 이 BIO가 나중에 blk-mq 제출 경로를 다시 탈 때 재사용할
 * q_usage_counter 참조를 미리 잡아두고(제출 시점에 다시 얻지 않아도 되게),
 * polled completion은 plug된 BIO에는 의미가 없으므로 미리 꺼둔다. 순서는
 * 항상 tail 추가(FIFO) — 블록 계층이 분할된 BIO를 원래 순서대로 넘겨주고
 * 사용자도 순차적으로 write를 발행한다는 전제 위에서 이 방식만으로 WP
 * 순서가 보존된다. 회전형(qd1) 장치라면 전역 active plug 리스트에도 함께
 * 등록한다.
 * 실행 컨텍스트: zwplug->lock 보유 — lockdep는 아니지만 호출자 쪽에서
 * 항상 락을 쥔 채로 부른다.
 * 호출자: blk_zone_wplug_handle_write()의 queue_bio 경로.
 * 피호출자: percpu_ref_get(), bio_clear_polled(), bio_list_add(),
 * blk_queue_zoned_qd1_writes(), list_add_tail(), refcount_inc().
 *
 * 호출 체인:
 *   blk_zone_wplug_handle_write() → [disk_zone_wplug_add_bio]
 */
static inline void disk_zone_wplug_add_bio(struct gendisk *disk,
				struct blk_zone_wplug *zwplug,
				struct bio *bio, unsigned int nr_segs)
{
	/*
	 * Grab an extra reference on the BIO request queue usage counter.
	 * This reference will be reused to submit a request for the BIO for
	 * blk-mq devices and dropped when the BIO is failed and after
	 * it is issued in the case of BIO-based devices.
	 */
	percpu_ref_get(&bio->bi_bdev->bd_disk->queue->q_usage_counter); // BIO 제출 시 blk-mq/NVMe queue 사용을 위한 q_usage_counter 획득

	/*
	 * The BIO is being plugged and thus will have to wait for the on-going
	 * write and for all other writes already plugged. So polling makes
	 * no sense.
	 */
	bio_clear_polled(bio); // plug된 BIO는 polled CQ completion을 사용할 수 없음

	/*
	 * Reuse the poll cookie field to store the number of segments when
	 * split to the hardware limits.
	 */
	bio->__bi_nr_segments = nr_segs; // BIO segment 수를 poll cookie 필드에 저장; 이후 request PRP/SGL 구성에 활용 (추정)

	/*
	 * We always receive BIOs after they are split and ready to be issued.
	 * The block layer passes the parts of a split BIO in order, and the
	 * user must also issue write sequentially. So simply add the new BIO
	 * at the tail of the list to preserve the sequential write order.
	 */
	bio_list_add(&zwplug->bio_list, bio); // BIO를 tail에 추가하여 ZNS WP 순서 유지
	trace_disk_zone_wplug_add_bio(zwplug->disk->queue, zwplug->zone_no,
				      bio->bi_iter.bi_sector, bio_sectors(bio));

	/*
	 * If we are using the disk zone write plugs worker instead of the per
	 * zone write plug BIO work, add the zone write plug to the work list
	 * if it is not already there. Make sure to also get an extra reference
	 * on the zone write plug so that it does not go away until it is
	 * removed from the work list.
	 */
	if (blk_queue_zoned_qd1_writes(disk->queue)) { // 회전형(qd1 writes) 장치는 전역 worker list 사용
		spin_lock(&disk->zone_wplugs_list_lock); // active plug list 보호 spinlock
		if (list_empty(&zwplug->entry)) {
			list_add_tail(&zwplug->entry, &disk->zone_wplugs_list); // qd1 worker가 처리할 plug list에 추가
			refcount_inc(&zwplug->ref); // worker list에 남아있는 동안 plug 유지
		}
		spin_unlock(&disk->zone_wplugs_list_lock);
	}
}

/*
 * Called from bio_attempt_back_merge() when a BIO was merged with a request.
 */
/*
 * [한국어] (영어 요약 보강)
 * blk_zone_write_plug_bio_merged - back merge된 BIO만큼 WP를 전진
 *
 * @bio: 기존 request 뒤에 병합(back merge)된 BIO.
 * @return: 없음(void).
 *
 * 이미 zone write plug를 거쳐 진행 중인 request에 새 BIO가 back merge로
 * 흡수될 때 호출되어, 그 BIO 크기만큼 zwplug->wp_offset을 미리 전진시킨다
 * — 병합되면 별도의 BIO/request로 SQ/CID를 소비하지 않고 기존 request에
 * 얹혀 나가므로, host 측 WP 예측치도 병합된 크기만큼만 앞당기면 된다.
 * 단, 이미 BIO_ZONE_WRITE_PLUGGING이 설정된 BIO(plug를 거쳐 이미 대기
 * 중이던 BIO가 blk_zone_write_plug_init_request()의 back merge 대상이 된
 * 경우)는 그 경로에서 이미 WP 갱신을 책임지므로 여기서는 중복 처리를
 * 피하기 위해 조기 반환한다.
 * 실행 컨텍스트: BIO 병합이 일어나는 제출 경로(대개 프로세스 컨텍스트) —
 * zwplug->lock으로 wp_offset 갱신을 보호.
 * 호출자: bio_attempt_back_merge() — blk-mq의 request 병합 로직.
 * 피호출자: bio_flagged(), bio_set_flag(), disk_get_zone_wplug(),
 * disk_zone_wplug_update_cond().
 * 참조 카운트 주의: disk_get_zone_wplug()로 얻은 참조를 이 함수는 일부러
 * disk_put_zone_wplug()로 반환하지 않는다 — BIO에 BIO_ZONE_WRITE_PLUGGING
 * 플래그를 세팅함으로써 이 BIO가 이제부터 "plug를 거친 BIO"와 동일하게
 * 취급되기 때문에, 그 참조의 소유권이 이 함수 실행 스택에서 BIO 자신으로
 * 넘어간 것으로 봐야 한다. 이 참조는 나중에 완료 시점에
 * blk_zone_write_plug_bio_endio()의 "Drop the reference we took when the
 * BIO was issued" 지점에서 반환된다.
 *
 * 호출 체인:
 *   bio_attempt_back_merge() → [blk_zone_write_plug_bio_merged] →
 *   disk_zone_wplug_update_cond() (참조는 blk_zone_write_plug_bio_endio()에서
 *   최종 반환)
 */
void blk_zone_write_plug_bio_merged(struct bio *bio)
{
	struct gendisk *disk = bio->bi_bdev->bd_disk; // BIO가 속한 NVMe namespace gendisk
	struct blk_zone_wplug *zwplug;
	unsigned long flags;

	/*
	 * If the BIO was already plugged, then we were called through
	 * blk_zone_write_plug_init_request() -> blk_attempt_bio_merge().
	 * For this case, we already hold a reference on the zone write plug for
	 * the BIO and blk_zone_write_plug_init_request() will handle the
	 * zone write pointer offset update.
	 */
	if (bio_flagged(bio, BIO_ZONE_WRITE_PLUGGING)) // 이미 plug된 BIO는 초기화 경로에서 처리하므로 중복 방지
		return;

	bio_set_flag(bio, BIO_ZONE_WRITE_PLUGGING); // merge 과정에서 plug가 추적 중임을 표시

	/*
	 * Get a reference on the zone write plug of the target zone and advance
	 * the zone write pointer offset. Given that this is a merge, we already
	 * have at least one request and one BIO referencing the zone write
	 * plug. So this should not fail.
	 */
	zwplug = disk_get_zone_wplug(disk, bio->bi_iter.bi_sector); // merge 대상 zone의 plug 획득
	if (WARN_ON_ONCE(!zwplug))
		return;

	spin_lock_irqsave(&zwplug->lock, flags);
	zwplug->wp_offset += bio_sectors(bio); // 병합된 BIO만큼 WP 전진 -> NVMe command 수 감소
	disk_zone_wplug_update_cond(disk, zwplug); // WP 변화에 따른 EMPTY/FULL/ACTIVE 상태 갱신
	spin_unlock_irqrestore(&zwplug->lock, flags);
}

/*
 * Attempt to merge plugged BIOs with a newly prepared request for a BIO that
 * already went through zone write plugging (either a new BIO or one that was
 * unplugged).
 */
/*
 * [한국어] (영어 요약 보강)
 * blk_zone_write_plug_init_request - 새 request에 plug 대기 BIO를 최대한 병합
 *
 * @req: 방금 zone write plug를 통과한 BIO로 초기화된 새 request.
 * @return: 없음(void).
 *
 * zone write plug를 거친 BIO 하나로 request가 막 만들어졌을 때, 같은
 * zone의 plug 대기열에 남아 있는 다음 BIO들을 이 request 뒤에 최대한
 * back merge시켜 request 하나에 더 많은 데이터를 실어 NVMe SQ/CID
 * 사용을 절약한다. request의 rq_flags에 RQF_ZONE_WRITE_PLUGGING을 세워
 * 완료 시 blk_zone_write_plug_finish_request()가 이 plug의 참조를
 * 반환해야 함을 표시한다. queue가 merge를 금지(nomerge)하면 이 최적화를
 * 건너뛴다. merge 루프는 zone이 FULL이 되거나, 다음 대기 BIO가 request
 * tail과 연속되지 않거나(sector 불일치/merge 조건 불충족), 실제
 * bio_attempt_back_merge()가 실패할 때까지 반복한다.
 * 실행 컨텍스트: request 준비 경로(프로세스 컨텍스트) — zwplug->lock으로
 * BIO list/WP 갱신을 보호.
 * 호출자: blk_mq_make_request() 계열의 request 초기화 경로 — 새 zone
 * write plug 경유 request가 만들어질 때마다 호출.
 * 피호출자: disk_get_zone_wplug(), blk_queue_nomerges(),
 * disk_zone_wplug_is_full(), bio_list_peek/pop(), blk_rq_merge_ok(),
 * bio_attempt_back_merge(), blk_queue_exit(), disk_zone_wplug_update_cond().
 * 에러 경로: zwplug를 못 찾으면(WARN_ON_ONCE) 아무 것도 하지 않고 반환 —
 * 이는 로직상 있을 수 없는 상황(버그 표시 목적).
 *
 * 호출 체인:
 *   blk_mq_make_request() → [blk_zone_write_plug_init_request] →
 *   bio_attempt_back_merge() → blk_zone_write_plug_bio_merged()
 */
void blk_zone_write_plug_init_request(struct request *req)
{
	sector_t req_back_sector = blk_rq_pos(req) + blk_rq_sectors(req); // request tail sector; back merge 연속성 판단
	struct request_queue *q = req->q; // request_queue (NVMe namespace queue) 참조
	struct gendisk *disk = q->disk;
	struct blk_zone_wplug *zwplug =
		disk_get_zone_wplug(disk, blk_rq_pos(req)); // request 위치의 ZID에 해당하는 plug 획득
	unsigned long flags;
	struct bio *bio;

	if (WARN_ON_ONCE(!zwplug)) // request가 반드시 zone plug를 가져야 함
		return;

	/*
	 * Indicate that completion of this request needs to be handled with
	 * blk_zone_write_plug_finish_request(), which will drop the reference
	 * on the zone write plug we took above on entry to this function.
	 */
	req->rq_flags |= RQF_ZONE_WRITE_PLUGGING; // 완료 시 plug reference 해제를 요청에 표시

	if (blk_queue_nomerges(q)) // NVMe queue limits가 merge를 금지한 경우 조기 반환
		return;

	/*
	 * Walk through the list of plugged BIOs to check if they can be merged
	 * into the back of the request.
	 */
	spin_lock_irqsave(&zwplug->lock, flags); // plug의 BIO list와 WP 직렬화
	while (!disk_zone_wplug_is_full(disk, zwplug)) { // zone이 full이 아닐 때까지 plug list의 BIO를 request에 병합
		bio = bio_list_peek(&zwplug->bio_list); // 가장 오래된 대기 BIO 확인
		if (!bio)
			break;

		if (bio->bi_iter.bi_sector != req_back_sector || // BIO 시작 sector가 request tail과 연속되는지 검사
		    !blk_rq_merge_ok(req, bio)) // (추정) queue limits/integrity/crypto/discard 호환성 검사 (NVMe command 구성 조건)
			break;

		WARN_ON_ONCE(bio_op(bio) != REQ_OP_WRITE_ZEROES && // merge된 BIO는 segment 정보를 가지고 있어야 함
			     !bio->__bi_nr_segments);

		bio_list_pop(&zwplug->bio_list); // 순서대로 BIO를 꺼내 back merge 시도
		if (bio_attempt_back_merge(req, bio, bio->__bi_nr_segments) != // (추정) request bio list 확장; 이후 NVMe PRP/SGL 항목으로 변환
		    BIO_MERGE_OK) {
			bio_list_add_head(&zwplug->bio_list, bio);
			break;
		}

		/* Drop the reference taken by disk_zone_wplug_add_bio(). */
		blk_queue_exit(q); // plug 시점에 잡았던 q_usage_counter 해제; NVMe queue exit
		zwplug->wp_offset += bio_sectors(bio); // 병합된 BIO 크기만큼 WP 전진
		disk_zone_wplug_update_cond(disk, zwplug);

		req_back_sector += bio_sectors(bio); // request tail sector 갱신; 다음 merge 연속성 확인
	}
	spin_unlock_irqrestore(&zwplug->lock, flags);
}

/*
 * Check and prepare a BIO for submission by incrementing the write pointer
 * offset of its zone write plug and changing zone append operations into
 * regular write when zone append emulation is needed.
 */
/*
 * [한국어] (영어 요약 보강)
 * blk_zone_wplug_prepare_bio - BIO를 실제 제출 직전 형태로 확정
 *
 * @zwplug: 이 BIO가 속한 zone write plug(zwplug->lock 보유 상태여야 함).
 * @bio: 제출을 준비할 BIO(zone append라면 이 함수가 opcode/sector를
 *       변형시킬 수 있음).
 * @return: true면 이 BIO를 그대로 제출해도 됨(WP도 이미 전진시킴),
 *          false면 WP가 불확실하거나 zone이 FULL이거나 비정렬 write라
 *          제출하면 안 됨(호출자가 실패 처리해야 함).
 *
 * WP 검증과 zone append 에뮬레이션을 한 곳에서 처리하는 이 파일의 핵심
 * 게이트 함수. (1) NEED_WP_UPDATE가 서 있으면(직전 write 오류로 WP를
 * 신뢰 못 함) 즉시 실패 — 사용자가 report/reset/finish로 복구하기 전까지
 * 이 zone에 대한 새 write는 받아들이지 않는다. (2) FULL zone에 대한
 * write도 실패 처리 — 계속 진행하면 wp_offset이 zone 경계를 넘어
 * 오버플로될 위험. (3) zone append이면서 에뮬레이션이 필요한 디바이스라면
 * REQ_OP_WRITE + REQ_NOMERGE로 opcode를 바꾸고 현재 WP를 sector에 더해
 * "append가 실제로 쓰는 절대 위치"를 host가 미리 계산해 넣는다(디바이스가
 * native append를 못 하니 host가 흉내). (4) 일반 write라면 BIO 시작
 * sector가 현재 WP와 정확히 일치하는지 검사 — 어긋나면 ZNS 순차 쓰기
 * 규칙 위반이므로 즉시 실패. 마지막으로 통과한 BIO 크기만큼 WP를 미리
 * 전진시켜 host의 WP 미러가 디바이스보다 앞서가게 한다(doorbell 전에
 * 이미 예측 완료).
 * 실행 컨텍스트: zwplug->lock 보유 — lockdep_assert_held로 강제.
 * 호출자: blk_zone_wplug_handle_write()(즉시 제출 경로),
 * disk_zone_wplug_submit_bio()(대기열에서 꺼낸 BIO 제출 전 재검증).
 * 피호출자: disk_zone_wplug_is_full(), bio_offset_from_zone_start(),
 * disk_zone_wplug_update_cond().
 *
 * 호출 체인:
 *   blk_zone_wplug_handle_write() / disk_zone_wplug_submit_bio() →
 *   [blk_zone_wplug_prepare_bio] → disk_zone_wplug_update_cond()
 */
static bool blk_zone_wplug_prepare_bio(struct blk_zone_wplug *zwplug,
				       struct bio *bio)
{
	struct gendisk *disk = bio->bi_bdev->bd_disk; // BIO의 NVMe namespace gendisk

	lockdep_assert_held(&zwplug->lock);

	/*
	 * If we lost track of the zone write pointer due to a write error,
	 * the user must either execute a report zones, reset the zone or finish
	 * the to recover a reliable write pointer position. Fail BIOs if the
	 * user did not do that as we cannot handle emulated zone append
	 * otherwise.
	 */
	if (zwplug->flags & BLK_ZONE_WPLUG_NEED_WP_UPDATE) // WP를 잃어버린 상태면 CID/SQ 소비 전 빠르게 실패
		return false;

	/*
	 * Check that the user is not attempting to write to a full zone.
	 * We know such BIO will fail, and that would potentially overflow our
	 * write pointer offset beyond the end of the zone.
	 */
	if (disk_zone_wplug_is_full(disk, zwplug)) // FULL zone 쓰기는 ZNS에서 무조건 실패; doorbell 차단
		return false;

	if (bio_op(bio) == REQ_OP_ZONE_APPEND) {
		/*
		 * Use a regular write starting at the current write pointer.
		 * Similarly to native zone append operations, do not allow
		 * merging.
		 */
		bio->bi_opf &= ~REQ_OP_MASK; // zone append를 REQ_OP_WRITE로 변환
		bio->bi_opf |= REQ_OP_WRITE | REQ_NOMERGE; // NVMe command merge 방지 (REQ_NOMERGE); ZNS atomic append 보장
		bio->bi_iter.bi_sector += zwplug->wp_offset; // zone 내 상대 sector에 WP offset을 더해 NVMe SLBA 계산
		// zone 상대 sector에 WP offset 더해 절대 WP 주소 계산

		/*
		 * Remember that this BIO is in fact a zone append operation
		 * so that we can restore its operation code on completion.
		 */
		bio_set_flag(bio, BIO_EMULATES_ZONE_APPEND); // 완료 시 op 코드를 ZONE_APPEND로 복원하기 위한 표시
		// 완료 시 op 코드를 ZONE_APPEND로 복원하기 위해 표시
	} else {
		/*
		 * Check for non-sequential writes early as we know that BIOs
		 * with a start sector not unaligned to the zone write pointer
		 * will fail.
		 */
		if (bio_offset_from_zone_start(bio) != zwplug->wp_offset) // WP와 정렬되지 않은 쓰기는 ZNS 위반; NVMe command 발행 전 차단
		// 비순차 쓰기 사전 차단
			return false;
	}

	/* Advance the zone write pointer offset. */
	zwplug->wp_offset += bio_sectors(bio); // BIO 크기만큼 WP 전진; doorbell보다 먼저 host 측 mirror 갱신
	disk_zone_wplug_update_cond(disk, zwplug);

	return true;
}

/*
 * [한국어]
 * blk_zone_wplug_handle_write - write/append/write-zeroes BIO의 zone plug 처리
 *
 * @bio: 처리할 write/zone-append/write-zeroes BIO.
 * @nr_segs: 분할 후 확정된 물리 세그먼트 수(plug 대기 시 나중 병합에
 *           사용하기 위해 보관됨).
 * @return: true면 이 함수가 BIO의 최종 처리(완료 또는 대기열 적재)를
 *          책임졌으므로 호출자가 더 이상 처리하지 않아도 됨, false면
 *          conventional zone이라 plug 없이 원래 제출 경로를 계속 타야 함.
 *
 * zone write plug 상태 기계의 심장부. (1) BIO가 zone 경계를 걸치면(블록
 * 계층이 분할을 실패한 버그 상황) 즉시 에러 처리. (2) conventional
 * zone이면 WP 규칙이 없으므로 plug 없이 false를 반환해 원래 경로로
 * 넘긴다(단, zone append는 conventional에 허용되지 않으므로 별도 처리).
 * (3) sequential zone이면 disk_get_or_alloc_zone_wplug()로 plug를
 * 얻는다. (4) plug가 DEAD(zone이 이미 full/reset 등으로 못 쓰는 상태)면
 * 즉시 실패. (5) REQ_NOWAIT BIO나 qd1 회전형 장치, 혹은 이미 다른 write가
 * 진행 중(PLUGGED)이면 queue_bio 경로로 대기열에 넣는다. (6) 그렇지 않고
 * 지금 당장 제출 가능하면 blk_zone_wplug_prepare_bio()로 WP 검증/조정 후
 * PLUGGED를 세우고 false를 반환해 "지금 여기서 바로 제출해도 된다"는
 * 신호를 준다(즉, 호출자인 blk_zone_plug_bio()가 계속 진행). PLUGGED가
 * 새로 켜진 대기 경로라면 회전형 워커를 깨우거나 workqueue에 예약한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(BIO 제출 경로) — zwplug->lock으로
 * 상태를 보호.
 * 호출자: blk_zone_plug_bio()(write/write-zeroes/에뮬레이션 zone append
 * 분기).
 * 피호출자: bio_straddles_zones(), bdev_zone_is_seq(),
 * disk_get_or_alloc_zone_wplug(), disk_zone_wplug_add_bio(),
 * blk_zone_wplug_prepare_bio(), disk_zone_wplug_schedule_work(),
 * wake_up_process().
 * 에러 경로: 경계 위반/할당 실패/DEAD plug/WP 검증 실패는 각각
 * bio_io_error() 또는 bio_wouldblock_error()로 즉시 완료 처리하고
 * true 반환 — 이 모든 경로에서 NVMe SQ/CID는 전혀 소비되지 않는다.
 *
 * 호출 체인:
 *   blk_zone_plug_bio() → [blk_zone_wplug_handle_write] →
 *   disk_zone_wplug_add_bio() 또는 (즉시 제출) blk_mq_submit_bio() →
 *   nvme_queue_rq() → nvme_submit_cmd(doorbell, SQ, CID)
 */
static bool blk_zone_wplug_handle_write(struct bio *bio, unsigned int nr_segs)
{
	struct gendisk *disk = bio->bi_bdev->bd_disk; // BIO의 NVMe namespace gendisk
	sector_t sector = bio->bi_iter.bi_sector; // BIO 시작 sector (ZNS zone 내 위치)
	struct blk_zone_wplug *zwplug;
	gfp_t gfp_mask = GFP_NOIO; // NOIO 기본값; REQ_NOWAIT 요청시 NOWAIT로 전환
	unsigned long flags;

	/*
	 * BIOs must be fully contained within a zone so that we use the correct
	 * zone write plug for the entire BIO. For blk-mq devices, the block
	 * layer should already have done any splitting required to ensure this
	 * and this BIO should thus not be straddling zone boundaries. For
	 * BIO-based devices, it is the responsibility of the driver to split
	 * the bio before submitting it.
	 */
	if (WARN_ON_ONCE(bio_straddles_zones(bio))) { // ZNS는 단일 command가 zone 경계를 넘을 수 없음
	// NVMe ZNS는 zone 경계를 넘는 BIO를 허용하지 않음
		bio_io_error(bio); // 잘못된 BIO를 NVMe SQ에 넣기 전 즉시 I/O error
		return true;
	}

	/* Conventional zones do not need write plugging. */
	if (!bdev_zone_is_seq(bio->bi_bdev, sector)) { // conventional zone은 WP serialization 없이 통과
	// conventional zone은 WP 제약 없이 바로 제출
		/* Zone append to conventional zones is not allowed. */
		if (bio_op(bio) == REQ_OP_ZONE_APPEND) { // conventional zone에 zone append는 ZNS에서 금지
			bio_io_error(bio);
			return true;
		}
		return false;
	}

	if (bio->bi_opf & REQ_NOWAIT) // REQ_NOWAIT BIO는 blocking allocation을 피해야 함
		gfp_mask = GFP_NOWAIT; // plug 할당도 non-blocking으로 시도

	zwplug = disk_get_or_alloc_zone_wplug(disk, sector, gfp_mask); // 해당 ZID에 대한 plug 검색 또는 새로 할당
	if (!zwplug) {
		if (bio->bi_opf & REQ_NOWAIT) // REQ_NOWAIT이고 할당 실패 시 -EAGAIN
			bio_wouldblock_error(bio); // 호출자에게 non-blocking retry 가능을 알림
		else
			bio_io_error(bio); // 일반적인 할당 실패는 I/O error
		return true;
	}

	spin_lock_irqsave(&zwplug->lock, flags); // plug 상태와 WP 직렬화

	/*
	 * If we got a zone write plug marked as dead, then the user is issuing
	 * writes to a full zone, or without synchronizing with zone reset or
	 * zone finish operations. In such case, fail the BIO to signal this
	 * invalid usage.
	 */
	if (zwplug->flags & BLK_ZONE_WPLUG_DEAD) { // DEAD zone은 추가 NVMe CID 할당을 허용하지 않음
		spin_unlock_irqrestore(&zwplug->lock, flags);
		disk_put_zone_wplug(zwplug);
		bio_io_error(bio);
		return true;
	}

	/* Indicate that this BIO is being handled using zone write plugging. */
	bio_set_flag(bio, BIO_ZONE_WRITE_PLUGGING); // 이 BIO가 zone plug 관리 대상임을 표시

	/*
	 * Add REQ_NOWAIT BIOs to the plug list to ensure that we will not see a
	 * BLK_STS_AGAIN failure if we let the caller submit the BIO.
	 */
	if (bio->bi_opf & REQ_NOWAIT) { // REQ_NOWAIT BIO는 직접 제출하지 않고 queue에 넣음
		bio->bi_opf &= ~REQ_NOWAIT; // plug 남 나이므로 NOWAIT flag 제거
		goto queue_bio;
		// NOWAIT BIO는 queue에 넣어 later submit
	}

	/*
	 * For rotational devices, we will use the gendisk zone write plugs
	 * work instead of the per zone write plug BIO work, so queue the BIO.
	 */
	if (blk_queue_zoned_qd1_writes(disk->queue)) // 회전형 장치(qd1 writes)는 별도 worker 사용
		goto queue_bio;
		// 회전형(qd1) 장치는 전용 worker queue 사용

	/* If the zone is already plugged, add the BIO to the BIO plug list. */
	if (zwplug->flags & BLK_ZONE_WPLUG_PLUGGED) // 이미 다른 write가 진행 중이면 BIO를 queue
		goto queue_bio;

	if (!blk_zone_wplug_prepare_bio(zwplug, bio)) { // WP/full/정렬 조건 미충족 시 doorbell 전 실패
		spin_unlock_irqrestore(&zwplug->lock, flags);
		bio_io_error(bio);
		// WP/full/정렬 오류 시 doorbell 전 빠른 실패
		return true;
	}

	/* Otherwise, plug and let the caller submit the BIO. */
	zwplug->flags |= BLK_ZONE_WPLUG_PLUGGED; // 현재 zone에 진행 중인 write가 있음을 표시

	spin_unlock_irqrestore(&zwplug->lock, flags);

	return false;

queue_bio:
	disk_zone_wplug_add_bio(disk, zwplug, bio, nr_segs); // 대기 BIO를 plug list에 추가

	if (!(zwplug->flags & BLK_ZONE_WPLUG_PLUGGED)) { // plug가 새로 활성화된 경우에만 깨움
		zwplug->flags |= BLK_ZONE_WPLUG_PLUGGED;
		if (blk_queue_zoned_qd1_writes(disk->queue)) // qd1 worker에게 새 work가 있음을 알림
			wake_up_process(disk->zone_wplugs_worker);
		else
			disk_zone_wplug_schedule_work(disk, zwplug); // per-zone workqueue에 제출하여 순차적으로 제출
	}

	spin_unlock_irqrestore(&zwplug->lock, flags);

	return true;
}

/*
 * [한국어]
 * blk_zone_wplug_handle_native_zone_append - native zone append BIO 처리
 *
 * @bio: REQ_OP_ZONE_APPEND이며 디바이스가 native로 지원하는 BIO(에뮬레이션
 *       불필요 — bdev_emulates_zone_append()==false인 경우에만 호출됨).
 * @return: 없음(void, blk_zone_plug_bio()가 이 함수 호출 후 항상 false를
 *          반환해 원래 제출 경로를 계속 타게 함).
 *
 * NVMe ZNS 컨트롤러가 zone append를 네이티브로 지원하면 host가 WP를
 * 예측할 필요 없이(컨트롤러가 알아서 현재 WP 위치에 기록하고 실제 쓴
 * 위치를 완료 시 알려줌) plug 없이 곧바로 제출 경로로 보낸다. 다만 같은
 * zone에 "과거 일반 write로 생성된" plug가 남아 있을 수 있으므로(사용자가
 * append와 일반 write를 섞어 쓴 이력), 그 plug를 찾아 제거해 두지 않으면
 * 이후 계속 append만 쓰는 zone에서 plug가 해시 테이블에 영원히 남는
 * 메모리 누수가 생긴다. 만약 그 plug의 대기열에 아직 완료되지 않은 일반
 * write BIO가 남아 있다면, native zone append와 일반 write 사이에는 순서
 * 보장이 전혀 없으므로(둘 다 동시에 디바이스로 갈 수 있음) 그 대기
 * BIO들은 정상적으로 실행돼도 결과를 신뢰할 수 없어 disk_zone_wplug_abort()
 * 로 강제 실패시킨다. 첫 native append 사용 시 GD_ZONE_APPEND_USED
 * 비트를 세워 cached report가 더 이상 이 zone의 WP를 신뢰하지 않게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(BIO 제출 경로).
 * 호출자: blk_zone_plug_bio()의 REQ_OP_ZONE_APPEND 분기(에뮬레이션
 * 불필요 시).
 * 피호출자: test_bit/set_bit(GD_ZONE_APPEND_USED), disk_get_zone_wplug(),
 * disk_zone_wplug_abort(), disk_mark_zone_wplug_dead(),
 * disk_put_zone_wplug().
 *
 * 호출 체인:
 *   blk_zone_plug_bio() → [blk_zone_wplug_handle_native_zone_append] →
 *   disk_mark_zone_wplug_dead()
 */
static void blk_zone_wplug_handle_native_zone_append(struct bio *bio)
{
	struct gendisk *disk = bio->bi_bdev->bd_disk; // BIO의 NVMe namespace gendisk
	struct blk_zone_wplug *zwplug;
	unsigned long flags;

	if (!test_bit(GD_ZONE_APPEND_USED, &disk->state)) // native zone append 첫 사용 시 플래그 설정
		set_bit(GD_ZONE_APPEND_USED, &disk->state); // cached report가 WP를 신뢰할 수 없게 됨을 표시
	// native zone append 사용 기록 -> cached report 무효화

	/*
	 * We have native support for zone append operations, so we are not
	 * going to handle @bio through plugging. However, we may already have a
	 * zone write plug for the target zone if that zone was previously
	 * partially written using regular writes. In such case, we risk leaving
	 * the plug in the disk hash table if the zone is fully written using
	 * zone append operations. Avoid this by removing the zone write plug.
	 */
	zwplug = disk_get_zone_wplug(disk, bio->bi_iter.bi_sector); // 이전 일반 write로 생성된 plug가 있는지 확인
	if (likely(!zwplug)) // plug가 없으면 그대로 native append 진행
		return;

	spin_lock_irqsave(&zwplug->lock, flags); // plug 제거 전 마지막 상태 확인

	/*
	 * We are about to remove the zone write plug. But if the user
	 * (mistakenly) has issued regular writes together with native zone
	 * append, we must aborts the writes as otherwise the plugged BIOs would
	 * not be executed by the plug BIO work as disk_get_zone_wplug() will
	 * return NULL after the plug is removed. Aborting the plugged write
	 * BIOs is consistent with the fact that these writes will most likely
	 * fail anyway as there is no ordering guarantees between zone append
	 * operations and regular write operations.
	 */
	if (!bio_list_empty(&zwplug->bio_list)) { // native append와 일반 write 혼합 시 대기 BIO들은 순서 보장 불가
		pr_warn_ratelimited("%s: zone %u: Invalid mix of zone append and regular writes\n",
				    disk->disk_name, zwplug->zone_no);
		disk_zone_wplug_abort(zwplug); // 혼합 사용 시 plug된 일반 write BIO들을 강제 실패
		// native append와 일반 write 혼합 시 plug abort
	}
	disk_mark_zone_wplug_dead(zwplug); // plug를 제거하여 메모리 누수 방지
	spin_unlock_irqrestore(&zwplug->lock, flags);

	disk_put_zone_wplug(zwplug);
}

/*
 * [한국어]
 * blk_zone_wplug_handle_zone_mgmt - zone 관리 명령 BIO의 사전 검증
 *
 * @bio: REQ_OP_ZONE_RESET/RESET_ALL/FINISH 중 하나인 BIO.
 * @return: true면 이미 에러 처리 완료(호출자가 더 볼 필요 없음), false면
 *          검증 통과 — 원래 제출 경로(plug 없이)를 계속 진행해야 함.
 *
 * zone 관리 명령은 WP 직렬화가 필요 없으므로(디바이스 자체가 원자적으로
 * 처리) zone write plug를 거치지 않지만, 제출 전에 최소한의 안전성 검사는
 * 필요하다: RESET_ALL이 아닌 reset/finish는 conventional zone에는 적용될
 * 수 없으므로 그런 경우 BIO를 즉시 실패시킨다. 또한 REQ_NOWAIT가 설정된
 * zone 관리 BIO는 사실상 의미가 없는데(호출자 대부분이 blocking으로
 * 발행) BLK_STS_AGAIN 실패를 방지하기 위해 경고를 남기고 그 플래그를
 * 강제로 지운다.
 * 실행 컨텍스트: 프로세스 컨텍스트(BIO 제출 경로).
 * 호출자: blk_zone_plug_bio()의 REQ_OP_ZONE_RESET/FINISH/RESET_ALL 분기.
 * 피호출자: bdev_zone_is_seq(), bio_io_error().
 *
 * 호출 체인:
 *   blk_zone_plug_bio() → [blk_zone_wplug_handle_zone_mgmt]
 */
static bool blk_zone_wplug_handle_zone_mgmt(struct bio *bio)
{
	if (bio_op(bio) != REQ_OP_ZONE_RESET_ALL && // RESET_ALL은 conventional zone에도 적용 가능
	    !bdev_zone_is_seq(bio->bi_bdev, bio->bi_iter.bi_sector)) { // reset/finish는 sequential zone에서만 유효
		/*
		 * Zone reset and zone finish operations do not apply to
		 * conventional zones.
		 */
		bio_io_error(bio); // 잘못된 zone에 대한 NVMe command 발행 전 차단
		return true;
	}

	/*
	 * No-wait zone management BIOs do not make much sense as the callers
	 * issue these as blocking operations in most cases. To avoid issues
	 * with the BIO execution potentially failing with BLK_STS_AGAIN, warn
	 * about REQ_NOWAIT being set and ignore that flag.
	 */
	if (WARN_ON_ONCE(bio->bi_opf & REQ_NOWAIT)) // zone management는 보통 blocking operation
		bio->bi_opf &= ~REQ_NOWAIT; // NOWAIT flag를 무시하고 동기식으로 처리
	// zone mgmt은 blocking이므로 NOWAIT 무시

	return false;
}

/**
 * blk_zone_plug_bio - Handle a zone write BIO with zone write plugging
 * @bio: The BIO being submitted
 * @nr_segs: The number of physical segments of @bio
 *
 * Handle write, write zeroes and zone append operations requiring emulation
 * using zone write plugging.
 *
 * Return true whenever @bio execution needs to be delayed through the zone
 * write plug. Otherwise, return false to let the submission path process
 * @bio normally.
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * blk_zone_plug_bio - zone write plugging이 필요한 BIO를 처리
 *
 * @bio: 제출되는 BIO(zoned 디바이스 대상).
 * @nr_segs: 분할 후 확정된 @bio의 물리 세그먼트 수.
 * @return: true면 zone write plug를 거쳐 실행이 지연됨(이 함수가 이미
 *          대기열 적재/에러 처리를 완료) — 호출자는 더 이상 이 BIO를
 *          처리하지 않아야 함. false면 제출 경로가 @bio를 평소처럼 계속
 *          처리해야 함(plug를 거치지 않거나 즉시 제출 가능한 경우 포함).
 *
 * 이 파일 전체의 유일한 공개 진입점 — zoned 디바이스로 향하는 모든 BIO는
 * submit_bio_noacct()에서 이 함수를 거친다. REQ_OP_WRITE/WRITE_ZEROES는
 * 항상 blk_zone_wplug_handle_write()로 plug 경로를 태운다(REQ_FUA/
 * REQ_PREFLUSH가 섞여도 flush 기계는 request 레벨, 즉 plug보다 아래에서
 * 동작하므로 문제 없음). REQ_OP_ZONE_APPEND는 디바이스가 native로
 * 지원하면 plug 없이 바로 보내고(blk_zone_wplug_handle_native_zone_append),
 * 에뮬레이션이 필요하면 write와 동일하게 plug 경로(fallthrough)를 탄다.
 * REQ_OP_ZONE_RESET/FINISH/RESET_ALL은 WP 직렬화가 필요 없으므로
 * blk_zone_wplug_handle_zone_mgmt()로 최소 검증만 거친다.
 * 실행 컨텍스트: 프로세스 컨텍스트(BIO 제출 경로) — 이 함수 자체는 락을
 * 잡지 않고 하위 핸들러에 위임.
 * 호출자: submit_bio_noacct().
 * 피호출자: blk_zone_wplug_handle_native_zone_append(),
 * blk_zone_wplug_handle_write(), blk_zone_wplug_handle_zone_mgmt().
 * 에러 경로: zone_wplugs_hash가 초기화 안 된 상태(WARN_ON_ONCE)는 버그로
 * 간주해 false 반환 — 이후 실제 처리는 하위 핸들러들이 각자의 에러
 * 경로에서 bio_io_error()/bio_wouldblock_error()로 완료.
 *
 * 호출 체인:
 *   submit_bio_noacct() → [blk_zone_plug_bio] →
 *   blk_zone_wplug_handle_write() → blk_mq_submit_bio() → nvme_queue_rq()
 *   → nvme_submit_cmd(doorbell, SQ, CID)
 */
bool blk_zone_plug_bio(struct bio *bio, unsigned int nr_segs)
{
	struct block_device *bdev = bio->bi_bdev; // BIO가 접근하는 block_device

	if (WARN_ON_ONCE(!bdev->bd_disk->zone_wplugs_hash)) // plug hash가 초기화되지 않은 zoned 장치는 버그
		return false;

	/*
	 * Regular writes and write zeroes need to be handled through the target
	 * zone write plug. This includes writes with REQ_FUA | REQ_PREFLUSH
	 * which may need to go through the flush machinery depending on the
	 * target device capabilities. Plugging such writes is fine as the flush
	 * machinery operates at the request level, below the plug, and
	 * completion of the flush sequence will go through the regular BIO
	 * completion, which will handle zone write plugging.
	 * Zone append operations for devices that requested emulation must
	 * also be plugged so that these BIOs can be changed into regular
	 * write BIOs.
	 * Zone reset, reset all and finish commands need special treatment
	 * to correctly track the write pointer offset of zones. These commands
	 * are not plugged as we do not need serialization with write
	 * operations. It is the responsibility of the user to not issue reset
	 * and finish commands when write operations are in flight.
	 */
	switch (bio_op(bio)) {
	case REQ_OP_ZONE_APPEND:
		if (!bdev_emulates_zone_append(bdev)) { // NVMe ZNS가 native zone append를 지원하는 경우
			blk_zone_wplug_handle_native_zone_append(bio); // plug 없이 직접 NVMe SQ로 제출
			return false;
		}
		fallthrough; // emulation이 필요하면 write/zone-append 동일 경로로 처리
	case REQ_OP_WRITE:
	case REQ_OP_WRITE_ZEROES:
		return blk_zone_wplug_handle_write(bio, nr_segs); // write/write-zeroes는 WP serialization을 거침
	case REQ_OP_ZONE_RESET: // NVMe Zone Management Send(Reset) 명령
	case REQ_OP_ZONE_FINISH: // NVMe Zone Management Send(Finish) 명령
	case REQ_OP_ZONE_RESET_ALL: // NVMe Zone Management Send(Reset All) 명령
		return blk_zone_wplug_handle_zone_mgmt(bio); // zone management 명령은 plug 없이 제출
	default:
		return false;
	}

	return false;
}
EXPORT_SYMBOL_GPL(blk_zone_plug_bio);

/*
 * [한국어]
 * disk_zone_wplug_unplug_bio - BIO/request 완료 후 다음 대기 BIO를 깨움
 *
 * @disk: 대상 gendisk.
 * @zwplug: 완료 처리 대상 plug.
 * @return: 없음(void).
 *
 * 하나의 plug된 BIO(또는 request)가 완료될 때마다 호출되어 "다음
 * 차례"를 진행시키는 함수. 대기열이 비었으면 PLUGGED를 내려 이후 새
 * write가 즉시 제출될 수 있게 하고, 회전형(qd1) 장치라면 전용 워커
 * 스레드에게 completion을 신호해 다음 BIO를 처리하게 하며, 그 외
 * (NVMe SSD 등 병렬 처리 가능 장치)라면 남은 BIO가 있을 때
 * workqueue에 다음 제출을 예약한다. 마지막으로 WP가 0(EMPTY, 흔치
 * 않지만 이론상 가능)이거나 FULL이면 plug를 DEAD로 표시해 자원을
 * 회수한다.
 * 실행 컨텍스트: BIO/request 완료 콜백 컨텍스트(인터럽트 가능) —
 * zwplug->lock을 irqsave로 보호.
 * 호출자: blk_zone_write_plug_bio_endio()(BIO 기반 드라이버 경로),
 * blk_zone_write_plug_finish_request()(request 기반 blk-mq 경로).
 * 피호출자: blk_queue_zoned_qd1_writes(), complete(),
 * disk_zone_wplug_schedule_work(), disk_zone_wplug_is_full(),
 * disk_mark_zone_wplug_dead().
 *
 * 호출 체인:
 *   blk_zone_write_plug_bio_endio() / blk_zone_write_plug_finish_request()
 *   → [disk_zone_wplug_unplug_bio] → disk_zone_wplug_schedule_work()
 */
static void disk_zone_wplug_unplug_bio(struct gendisk *disk,
				       struct blk_zone_wplug *zwplug)
{
	unsigned long flags;

	spin_lock_irqsave(&zwplug->lock, flags); // plug 상태와 BIO list 직렬화

	/*
	 * For rotational devices, signal the BIO completion to the zone write
	 * plug work. Otherwise, schedule submission of the next plugged BIO
	 * if we have one.
	 */
	if (bio_list_empty(&zwplug->bio_list)) // 더 이상 대기 BIO가 없으면 PLUGGED 해제
		zwplug->flags &= ~BLK_ZONE_WPLUG_PLUGGED; // plug 해제로 다음 write가 NVMe SQ로 진입 가능

	if (blk_queue_zoned_qd1_writes(disk->queue)) // qd1 회전형 장치용 completion 신호
		complete(&disk->zone_wplugs_worker_bio_done); // BIO 완료를 worker에게 알림
	else if (!bio_list_empty(&zwplug->bio_list)) // NVMe/SSD 경로에서는 남은 BIO를 workqueue로 재스케줄
		disk_zone_wplug_schedule_work(disk, zwplug); // 다음 BIO 제출을 workqueue에 예약

	if (!zwplug->wp_offset || disk_zone_wplug_is_full(disk, zwplug)) // WP가 경계에 도달하면 zone을 더 이상 사용하지 않음
		disk_mark_zone_wplug_dead(zwplug); // DEAD 표시로 추가 CID 할당 차단

	spin_unlock_irqrestore(&zwplug->lock, flags);
}

/*
 * [한국어] (영어 요약 보강)
 * blk_zone_append_update_request_bio - request가 실제로 쓴 LBA를 BIO로 복사
 *
 * @rq: 완료 처리 중인 request(zone append 커맨드로 실제 디바이스에 쓰인
 *      절대 sector가 rq->__sector에 담겨 있음).
 * @bio: 이 request에 속한 BIO(제출 시점의 sector를 아직 갖고 있음).
 * @return: 없음(void).
 *
 * NVMe ZNS zone append는 "어디에 쓸지"를 host가 지정하지 않고 컨트롤러가
 * 알아서 현재 WP 위치에 쓴 뒤 "실제로 어디에 썼는지"를 완료 시 알려주는
 * 커맨드다. 이 함수는 그 실제 LBA(rq->__sector)를 BIO의 bi_iter.bi_sector
 * 에 되돌려 써서, BIO 제출자가 결과를 조회했을 때 실제 쓰인 위치를 알 수
 * 있게 한다. 동시에, plug된 zone write(에뮬레이션된 zone append 포함)의
 * 경우 이후 blk_zone_write_plug_bio_endio()가 이 BIO가 속한 plug를
 * 다시 찾아야 하므로, 그 조회에 필요한 "원래(zone 상대) sector"를
 * 유지해야 하는 문제와 균형을 맞춰 설계되어 있다(호출 순서상 이 함수가
 * 항상 zone write plug 조회보다 먼저 실행되지 않도록 호출자가 보장).
 * 실행 컨텍스트: request 완료 경로(인터럽트/softirq 가능).
 * 호출자: blk-mq의 zone append 완료 처리 경로(예: NVMe zone append 완료
 * 시 request 완료 함수에서).
 * 피호출자: 없음(단순 대입 + 트레이스 이벤트).
 *
 * 호출 체인:
 *   (zone append 완료 처리 경로) → [blk_zone_append_update_request_bio]
 */
void blk_zone_append_update_request_bio(struct request *rq, struct bio *bio)
{
	/*
	 * For zone append requests, the request sector indicates the location
	 * at which the BIO data was written. Return this value to the BIO
	 * issuer through the BIO iter sector.
	 * For plugged zone writes, which include emulated zone append, we need
	 * the original BIO sector so that blk_zone_write_plug_bio_endio() can
	 * lookup the zone write plug.
	 */
	bio->bi_iter.bi_sector = rq->__sector; // NVMe ZNS zone append가 실제 기록한 LBA를 BIO issuer에게 반환
	trace_blk_zone_append_update_request_bio(rq);
}

/*
 * [한국어]
 * blk_zone_write_plug_bio_endio - plug를 거친 BIO의 공용 완료 콜백
 *
 * @bio: 완료된 BIO(zone write plug를 거쳐 제출됐던 것).
 * @return: 없음(void).
 *
 * zone write plug 경로로 나간 모든 BIO(일반 write, write-zeroes, 에뮬레이션
 * zone append)가 공통으로 거치는 완료 처리. BIO_ZONE_WRITE_PLUGGING
 * 플래그를 지워 재진입을 막고, 에뮬레이션된 zone append였다면 opcode를
 * 원래의 REQ_OP_ZONE_APPEND로 되돌린다(호출자에게는 append처럼 보이게).
 * BIO가 실패했다면(bi_status != OK) 이후 WP를 더 이상 신뢰할 수 없으므로
 * plug에 남은 대기 BIO들을 모두 abort하고 NEED_WP_UPDATE를 세워 다음
 * Report Zones/reset/finish 때 정확한 WP로 복구되게 한다. BIO 기반
 * 드라이버(BD_HAS_SUBMIT_BIO)는 request 완료 콜백
 * (blk_zone_write_plug_finish_request)을 거치지 않으므로, 이 함수
 * 자신이 disk_zone_wplug_unplug_bio()를 직접 호출해 다음 대기 BIO를
 * 깨운다 — request 기반(blk-mq) 장치는 그 역할을 finish_request가
 * 대신하므로 여기서는 건너뛴다.
 * 실행 컨텍스트: BIO 완료 콜백(인터럽트/softirq 가능) — 필요한 구간만
 * zwplug->lock으로 보호.
 * 호출자: bio_endio() — plug된 BIO의 bi_end_io로 등록된 경로.
 * 피호출자: disk_get_zone_wplug(), bio_clear_flag(), bio_flagged(),
 * disk_zone_wplug_abort(), disk_put_zone_wplug(), bdev_test_flag(),
 * disk_zone_wplug_unplug_bio().
 * 참조 카운트: 이 함수에서 disk_put_zone_wplug()를 두 번 호출 —
 * (1) BIO 제출 시 잡았던 참조, (2) 이 함수 진입 시
 * disk_get_zone_wplug()로 새로 얻은 참조. 둘 다 반드시 반환해야 leak이
 * 없다.
 *
 * 호출 체인:
 *   bio_endio() → [blk_zone_write_plug_bio_endio] →
 *   disk_zone_wplug_unplug_bio() → disk_zone_wplug_schedule_work()
 */
void blk_zone_write_plug_bio_endio(struct bio *bio)
{
	struct gendisk *disk = bio->bi_bdev->bd_disk; // 완료된 BIO의 NVMe namespace gendisk
	struct blk_zone_wplug *zwplug = // BIO sector로 ZID에 해당하는 plug 조회
		disk_get_zone_wplug(disk, bio->bi_iter.bi_sector);
	unsigned long flags;

	if (WARN_ON_ONCE(!zwplug))
		return; // plug를 찾지 못하면 상태 동기화 불가

	/* Make sure we do not see this BIO again by clearing the plug flag. */
	bio_clear_flag(bio, BIO_ZONE_WRITE_PLUGGING); // 이 BIO의 plug 처리 완료 표시

	/*
	 * If this is a regular write emulating a zone append operation,
	 * restore the original operation code.
	 */
	if (bio_flagged(bio, BIO_EMULATES_ZONE_APPEND)) { // emulation으로 WRITE로 변환되었던 BIO 복원
		bio->bi_opf &= ~REQ_OP_MASK; // op 코드를 다시 ZONE_APPEND로 설정
		bio->bi_opf |= REQ_OP_ZONE_APPEND;
		bio_clear_flag(bio, BIO_EMULATES_ZONE_APPEND); // emulation 표시 제거
	}

	/*
	 * If the BIO failed, abort all plugged BIOs and mark the plug as
	 * needing a write pointer update.
	 */
	if (bio->bi_status != BLK_STS_OK) { // NVMe command 실패 시 plug 상태 복구 필요
		spin_lock_irqsave(&zwplug->lock, flags); // plug와 BIO list 직렬화
		disk_zone_wplug_abort(zwplug); // 실패한 write 이후 대기 BIO들을 모두 abort
		zwplug->flags |= BLK_ZONE_WPLUG_NEED_WP_UPDATE; // WP 불확실 상태 표시; 다음 Report Zones로 동기화
		spin_unlock_irqrestore(&zwplug->lock, flags);
	}

	/* Drop the reference we took when the BIO was issued. */
	disk_put_zone_wplug(zwplug); // BIO 제출 시 획득했던 plug reference 해제

	/*
	 * For BIO-based devices, blk_zone_write_plug_finish_request()
	 * is not called. So we need to schedule execution of the next
	 * plugged BIO here.
	 */
	if (bdev_test_flag(bio->bi_bdev, BD_HAS_SUBMIT_BIO)) // BIO-based driver 경로에서는 request 완료 callback이 없음
		disk_zone_wplug_unplug_bio(disk, zwplug); // BIO-based 경로에서도 다음 BIO가 깨어나도록 함

	/* Drop the reference we took when entering this function. */
	disk_put_zone_wplug(zwplug); // 함수 진입 시 획득한 plug reference 해제
}

/*
 * [한국어]
 * blk_zone_write_plug_finish_request - request 완료 시 zone plug 마무리
 *
 * @req: 완료된 request(RQF_ZONE_WRITE_PLUGGING이 세팅돼 있어야 함 —
 *       blk_zone_write_plug_init_request()가 표시).
 * @return: 없음(void).
 *
 * request 기반(blk-mq) 장치에서 zone write plug를 거친 request가 완료될
 * 때 호출되는 짝 함수. blk_zone_write_plug_init_request()가 잡았던 plug
 * 참조를 반환하고, disk_zone_wplug_unplug_bio()를 호출해 다음 대기 BIO를
 * 깨운다 — BIO 기반 드라이버는 이 역할을 blk_zone_write_plug_bio_endio()
 * 자신이 대신하므로 이 함수는 request 기반 경로에서만 쓰인다.
 * 실행 컨텍스트: request 완료 콜백(인터럽트/softirq 가능).
 * 호출자: blk_mq_end_request() 계열의 request 완료 경로(예: NVMe라면
 * nvme_complete_rq() → blk_mq_end_request()).
 * 피호출자: disk_get_zone_wplug(), disk_put_zone_wplug(),
 * disk_zone_wplug_unplug_bio().
 * 참조 카운트: disk_put_zone_wplug()를 두 번 호출 — (1)
 * blk_zone_write_plug_init_request()가 잡았던 참조, (2) 이 함수 진입
 * 시 disk_get_zone_wplug()로 새로 얻은 참조.
 *
 * 호출 체인:
 *   nvme_complete_rq() → blk_mq_end_request() →
 *   [blk_zone_write_plug_finish_request] → disk_zone_wplug_unplug_bio()
 */
void blk_zone_write_plug_finish_request(struct request *req)
{
	struct gendisk *disk = req->q->disk; // request의 NVMe namespace gendisk
	struct blk_zone_wplug *zwplug;

	zwplug = disk_get_zone_wplug(disk, req->__sector); // request sector로 ZID에 해당하는 plug 조회
	if (WARN_ON_ONCE(!zwplug))
		return;

	req->rq_flags &= ~RQF_ZONE_WRITE_PLUGGING; // request의 zone plug 처리 완료 표시 제거

	/*
	 * Drop the reference we took when the request was initialized in
	 * blk_zone_write_plug_init_request().
	 */
	disk_put_zone_wplug(zwplug); // init_request()에서 획득한 plug reference 해제

	disk_zone_wplug_unplug_bio(disk, zwplug); // 다음 대기 BIO 제출 또는 plug 종료

	/* Drop the reference we took when entering this function. */
	disk_put_zone_wplug(zwplug); // 함수 진입 시 획득한 plug reference 해제
}

/*
 * [한국어]
 * disk_zone_wplug_submit_bio - plug 대기열의 다음 BIO를 검증 후 실제 제출
 *
 * @disk: 대상 gendisk.
 * @zwplug: 대기열에서 BIO를 꺼낼 plug.
 * @return: true면 BIO 하나를 성공적으로 제출(또는 검증 실패로 즉시 완료
 *          처리 후 다음 BIO로 재시도)했음 — 회전형(qd1) 워커는 이 반환값을
 *          루프 조건으로 사용, false면 대기열이 비어 더 이상 할 일이 없음
 *          (PLUGGED도 함께 해제됨).
 *
 * disk_zone_wplug_schedule_work()로 예약된 workqueue 작업이나 회전형
 * qd1 워커 스레드가 실제로 "다음 BIO를 디바이스에 내보내는" 지점.
 * bio_list_pop()으로 대기열 머리를 꺼낸 뒤 blk_zone_wplug_prepare_bio()
 * 로 WP를 재검증한다(대기하는 동안 다른 이유로 상태가 바뀌었을 수 있어
 * 재검증이 필요) — 검증에 실패하면 blk_zone_wplug_bio_io_error()로
 * 완료 처리하고 다음 BIO로 재시도(goto again)한다. 검증을 통과하면 실제
 * 제출 경로를 탄다: BIO 기반 드라이버(BD_HAS_SUBMIT_BIO)는
 * disk->fops->submit_bio()를 직접 호출하고 blk_queue_exit()로
 * q_usage_counter를 해제하며, request 기반(blk-mq) 장치는
 * blk_mq_submit_bio()에게 넘겨 그 안에서 미리 잡아둔 q_usage_counter
 * 참조를 재사용한다(중복 획득/해제 없이 효율적으로 이어짐). 회전형
 * (qd1) 장치라면 제출 전에 completion을 재초기화해 다음 BIO 완료를
 * 기다릴 준비를 한다.
 * 실행 컨텍스트: workqueue 워커 또는 zone plug 전용 커널 스레드
 * (프로세스 컨텍스트) — BIO 제출이 블로킹할 수 있는 경로를 안전하게
 * 수행.
 * 호출자: disk_zone_wplugs_worker()(회전형 qd1 루프),
 * blk_zone_wplug_bio_work()(workqueue 콜백).
 * 피호출자: bio_list_pop(), blk_zone_wplug_prepare_bio(),
 * blk_zone_wplug_bio_io_error(), reinit_completion(),
 * bdev_test_flag(), disk->fops->submit_bio(), blk_queue_exit(),
 * blk_mq_submit_bio().
 *
 * 호출 체인:
 *   disk_zone_wplugs_worker() / blk_zone_wplug_bio_work() →
 *   [disk_zone_wplug_submit_bio] → blk_mq_submit_bio() → nvme_queue_rq()
 *   → nvme_submit_cmd(doorbell, SQ, CID)
 */
static bool disk_zone_wplug_submit_bio(struct gendisk *disk,
				       struct blk_zone_wplug *zwplug)
{
	struct block_device *bdev; // 제출할 BIO의 block_device
	unsigned long flags;
	struct bio *bio;
	bool prepared;

	/*
	 * Submit the next plugged BIO. If we do not have any, clear
	 * the plugged flag.
	 */
again:
	spin_lock_irqsave(&zwplug->lock, flags); // plug와 BIO list 직렬화
	bio = bio_list_pop(&zwplug->bio_list); // FIFO 순서로 다음 BIO를 꺼냄
	if (!bio) {
		zwplug->flags &= ~BLK_ZONE_WPLUG_PLUGGED; // 대기 BIO가 없으면 PLUGGED 상태 클리어
		spin_unlock_irqrestore(&zwplug->lock, flags);
		return false;
	}

	trace_blk_zone_wplug_bio(zwplug->disk->queue, zwplug->zone_no,
				 bio->bi_iter.bi_sector, bio_sectors(bio));

	prepared = blk_zone_wplug_prepare_bio(zwplug, bio); // WP/full/정렬 검증; 실패하면 abort
	spin_unlock_irqrestore(&zwplug->lock, flags);

	if (!prepared) {
		blk_zone_wplug_bio_io_error(zwplug, bio); // 검증 실패 시 NVMe command 없이 BIO error
		goto again;
	}

	/*
	 * blk-mq devices will reuse the extra reference on the request queue
	 * usage counter we took when the BIO was plugged, but the submission
	 * path for BIO-based devices will not do that. So drop this extra
	 * reference here.
	 */
	if (blk_queue_zoned_qd1_writes(disk->queue)) // qd1 회전형 장치에서는 completion을 기다림
		reinit_completion(&disk->zone_wplugs_worker_bio_done); // BIO 완료 대기용 completion 재초기화
		// qd1 worker가 BIO 완료를 기다릴 수 있도록 completion 재초기화
	bdev = bio->bi_bdev; // BIO의 block_device 참조
	if (bdev_test_flag(bdev, BD_HAS_SUBMIT_BIO)) { // BIO-based driver (예: DM)인지 확인
		bdev->bd_disk->fops->submit_bio(bio); // BIO-based driver 경로로 제출
		blk_queue_exit(bdev->bd_disk->queue); // BIO-based driver 완료 후 q_usage_counter 해제
	} else {
		blk_mq_submit_bio(bio); // blk-mq 경로 -> nvme_queue_rq -> nvme_submit_cmd(doorbell, SQ, CID)
		// blk-mq 경로: NVMe SQ/CID 할당으로 이어짐
	}

	return true;
}

/*
 * [한국어]
 * disk_get_zone_wplugs_work - 회전형(qd1) 워커가 처리할 다음 plug를 꺼냄
 *
 * @disk: 대상 gendisk.
 * @return: active plug 리스트의 첫 항목(참조는 이미 disk_zone_wplug_add_bio
 *          에서 리스트에 넣을 때 잡아둔 것을 그대로 승계), 또는 NULL(리스트가
 *          비어 처리할 것이 없음).
 *
 * disk->zone_wplugs_list는 FIFO로 취급되어, 먼저 write가 도착한 zone부터
 * 순서대로 처리한다(회전형 매체에서 임의의 zone을 오가며 seek하지 않고
 * 순차적으로 처리하게 하려는 의도). 꺼낸 항목은 즉시 리스트에서
 * list_del_init()으로 제거해 다른 컨텍스트가 중복으로 가져가지 않게 한다.
 * 실행 컨텍스트: disk_zone_wplugs_worker() 단일 커널 스레드에서만 호출됨
 * (호출자가 유일하므로 사실상 경쟁자는 disk_zone_wplug_add_bio()의 삽입
 * 뿐) — zone_wplugs_list_lock spinlock으로 리스트 조작을 보호.
 * 호출자: disk_zone_wplugs_worker().
 * 피호출자: list_first_entry_or_null(), list_del_init().
 *
 * 호출 체인:
 *   disk_zone_wplugs_worker() → [disk_get_zone_wplugs_work]
 */
static struct blk_zone_wplug *disk_get_zone_wplugs_work(struct gendisk *disk)
{
	struct blk_zone_wplug *zwplug;

	spin_lock_irq(&disk->zone_wplugs_list_lock); // active plug list 보호
	zwplug = list_first_entry_or_null(&disk->zone_wplugs_list, // 처리할 다음 zone plug 선택
					  struct blk_zone_wplug, entry);
	if (zwplug)
		list_del_init(&zwplug->entry); // worker가 가져간 plug를 list에서 제거
	spin_unlock_irq(&disk->zone_wplugs_list_lock);

	return zwplug;
}

/*
 * [한국어] (영어 요약 보강)
 * disk_zone_wplugs_worker - 회전형(qd1 writes) 매체 전용 zone plug 처리 스레드
 *
 * @data: struct gendisk* 포인터(kthread_create의 data 인자로 전달됨).
 * @return: 0(kthread_stop()에 의해 정상 종료).
 *
 * SMR HDD처럼 큐 깊이 1(qd1)로 zone을 순차 처리해야 하는 회전형 매체를
 * 위한 전용 커널 스레드. NVMe ZNS SSD는 여러 zone에 병렬로 write할 수
 * 있어 per-plug workqueue(blk_zone_wplug_bio_work)로 충분하지만, 회전형
 * 매체는 헤드 이동 비용 때문에 한 번에 zone 하나씩만 순차 처리하는 것이
 * 유리해 이 단일 스레드가 active plug 리스트를 순서대로 소진한다. plug
 * 하나를 얻으면 그 plug의 BIO가 모두 나갈 때까지
 * disk_zone_wplug_submit_bio()를 반복 호출하고, 매번 이전 BIO의 완료를
 * blk_wait_io()로 기다린 뒤에야 다음 BIO를 내보낸다(진짜 큐 깊이 1 방식).
 * 할 일이 없으면 TASK_INTERRUPTIBLE로 잠들고, freezer(시스템 절전)와
 * kthread_stop() 요청에 응답한다.
 * 실행 컨텍스트: 독립된 커널 스레드(diskname_zwplugs_worker) — 전체 함수
 * 실행 동안 memalloc_noio_save()로 GFP_NOIO를 강제해 스토리지 재진입
 * 데드락을 피한다.
 * 호출자: kthread가 disk_alloc_zone_resources()의 kthread_create() +
 * wake_up_process()로 시작.
 * 피호출자: disk_get_zone_wplugs_work(), disk_zone_wplug_submit_bio(),
 * blk_wait_io(), disk_put_zone_wplug(), try_to_freeze(),
 * kthread_should_stop().
 *
 * 호출 체인:
 *   disk_alloc_zone_resources() → kthread_create() →
 *   [disk_zone_wplugs_worker] → disk_zone_wplug_submit_bio()
 */
static int disk_zone_wplugs_worker(void *data)
{
	struct gendisk *disk = data;
	struct blk_zone_wplug *zwplug;
	unsigned int noio_flag;

	noio_flag = memalloc_noio_save(); // memory allocation을 NOIO 모드로 제한
	set_user_nice(current, MIN_NICE); // worker 스레드 우선순위 최고로 설정
	set_freezable();

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE | TASK_FREEZABLE); // 작업이 있을 때까지 interruptible sleep

		zwplug = disk_get_zone_wplugs_work(disk); // 처리할 zone plug가 있는지 확인
		if (zwplug) {
			/*
			 * Process all BIOs of this zone write plug and then
			 * drop the reference we took when adding the zone write
			 * plug to the active list.
			 */
			set_current_state(TASK_RUNNING);
			while (disk_zone_wplug_submit_bio(disk, zwplug)) // 해당 zone의 plug list를 모두 소진할 때까지 제출
				blk_wait_io(&disk->zone_wplugs_worker_bio_done); // BIO 하나 완료될 때까지 대기 (회전형 qd1 모델)
			disk_put_zone_wplug(zwplug); // worker list에서 제거할 때 획득한 reference 해제
			continue;
		}

		/*
		 * Only sleep if nothing sets the state to running. Else check
		 * for zone write plugs work again as a newly submitted BIO
		 * might have added a zone write plug to the work list.
		 */
		if (get_current_state() == TASK_RUNNING) {
			try_to_freeze();
		} else {
			if (kthread_should_stop()) {
				set_current_state(TASK_RUNNING);
				break;
			}
			schedule();
		}
	}

	WARN_ON_ONCE(!list_empty(&disk->zone_wplugs_list));
	memalloc_noio_restore(noio_flag);

	return 0;
}

/*
 * [한국어]
 * disk_init_zone_resources - gendisk의 zone 관련 락/리스트/completion 초기화
 *
 * @disk: 초기화할 gendisk(zoned 여부와 무관하게 gendisk 생성 시 항상
 *        호출됨 — 이후 실제 zone 자원은 필요할 때 disk_alloc_zone_resources
 *        가 별도로 할당).
 * @return: 없음(void).
 *
 * hash table/mempool/workqueue처럼 나중에 gfp 할당이 필요한 무거운 자원은
 * disk_alloc_zone_resources()로 미루고, 이 함수는 gendisk 생성 시점에
 * 항상 안전하게 초기화할 수 있는 락/리스트/completion만 준비한다(zoned가
 * 아닌 디스크라도 이 필드들 자체는 유효한 상태여야 하므로).
 * 실행 컨텍스트: gendisk 생성/초기화 경로(프로세스 컨텍스트) — 동시
 * 접근이 없는 시점.
 * 호출자: gendisk 할당 경로(__alloc_disk_node 등, 이 파일 밖).
 * 피호출자: spin_lock_init(), INIT_LIST_HEAD(), init_completion().
 *
 * 호출 체인:
 *   (gendisk 할당 경로) → [disk_init_zone_resources]
 */
void disk_init_zone_resources(struct gendisk *disk)
{
	spin_lock_init(&disk->zone_wplugs_hash_lock); // zone plug hash table lock 초기화
	spin_lock_init(&disk->zone_wplugs_list_lock); // qd1 worker active list lock 초기화
	INIT_LIST_HEAD(&disk->zone_wplugs_list); // active zone plug list 초기화
	init_completion(&disk->zone_wplugs_worker_bio_done); // qd1 worker BIO 완료 대기 completion 초기화
}

/*
 * For the size of a disk zone write plug hash table, use the size of the
 * zone write plug mempool, which is the maximum of the disk open zones and
 * active zones limits. But do not exceed 4KB (512 hlist head entries), that is,
 * 9 bits. For a disk that has no limits, mempool size defaults to 128.
 */
#define BLK_ZONE_WPLUG_MAX_HASH_BITS		9
/* [한국어] zone write plug 해시 테이블 크기의 상한 비트 수(2^9=512 버킷,
 * hlist_head 포인터 크기 기준 약 4KB) - open/active zone 한도가 아주 큰
 * 디바이스라도 해시 테이블 메모리가 과도하게 커지지 않도록 제한한다. */
#define BLK_ZONE_WPLUG_DEFAULT_POOL_SIZE	128
/* [한국어] 디바이스가 max_open_zones/max_active_zones 한도를 전혀 광고하지
 * 않을 때 사용하는 기본 zone write plug mempool 크기 - 동시에 활성화될
 * 것으로 가정하는 zone 개수의 보수적인 기본값. */

/*
 * [한국어] (영어 요약 보강)
 * disk_alloc_zone_resources - zone plug 해시/mempool/workqueue/워커 스레드 할당
 *
 * @disk: 대상 gendisk.
 * @pool_size: mempool과 workqueue 동시성 한도로 쓸 크기(open/active zone
 *             한도 중 큰 값, 또는 기본값).
 * @return: 0(성공) 또는 -ENOMEM(단계별 할당 실패), kthread_create() 실패
 *          시 그 PTR_ERR 값.
 *
 * disk_revalidate_zone_resources()가 이 디바이스는 request 기반(blk-mq)
 * 이거나 zone append 에뮬레이션이 필요하다고 판단했을 때만 호출되는
 * 무거운 자원 할당 경로. 순서대로 (1) 해시 버킷 수를 pool_size 기반으로
 * 계산(9비트 상한), (2) 해시 테이블 배열 할당 및 초기화, (3) plug
 * mempool 생성, (4) per-disk workqueue 생성(WQ_MEM_RECLAIM으로 메모리
 * 회수 경로에서도 진행 보장, WQ_HIGHPRI로 I/O 지연 최소화), (5) 회전형
 * (qd1) 매체를 위한 워커 스레드 생성 후 즉시 실행. 각 단계는 goto 라벨로
 * 이전 단계까지의 자원만 정확히 되돌리는 표준 커널 에러 처리 패턴을
 * 따른다.
 * 실행 컨텍스트: 프로세스 컨텍스트, GFP_NOIO 하에서 호출됨(호출자인
 * blk_revalidate_disk_zones()가 memalloc_noio_save 적용).
 * 호출자: disk_revalidate_zone_resources().
 * 피호출자: ilog2(), kzalloc_objs(), mempool_create_kmalloc_pool(),
 * alloc_workqueue(), kthread_create(), wake_up_process().
 * 에러 경로: 각 단계 실패 시 destroy_wq/destroy_pool/free_hash 라벨로
 * 순서대로 되돌아가며 그 이전 단계까지 할당한 자원만 해제.
 *
 * 호출 체인:
 *   disk_revalidate_zone_resources() → [disk_alloc_zone_resources] →
 *   kthread_create() → disk_zone_wplugs_worker()
 */
static int disk_alloc_zone_resources(struct gendisk *disk,
				     unsigned int pool_size)
{
	unsigned int i; // hash bucket 초기화 loop index
	int ret = -ENOMEM; // 자원 할당 실패 시 반환값
	atomic_set(&disk->nr_zone_wplugs, 0); // 활성 zone plug 개수 0으로 초기화
	disk->zone_wplugs_hash_bits =
		min(ilog2(pool_size) + 1, BLK_ZONE_WPLUG_MAX_HASH_BITS); // hash bucket 수를 512개(9 bits)로 제한

	disk->zone_wplugs_hash =
		kzalloc_objs(struct hlist_head,
			     disk_zone_wplugs_hash_size(disk)); // zone plug hash table 메모리 할당
	if (!disk->zone_wplugs_hash) // 해시 배열 할당 실패 - 아직 아무 자원도 안 만들었으니 바로 반환
		return -ENOMEM;

	for (i = 0; i < disk_zone_wplugs_hash_size(disk); i++) // 모든 hash bucket head 초기화
		INIT_HLIST_HEAD(&disk->zone_wplugs_hash[i]);

	disk->zone_wplugs_pool = mempool_create_kmalloc_pool(pool_size,
						sizeof(struct blk_zone_wplug)); // max_open/active_zones 기반 plug mempool 생성
	if (!disk->zone_wplugs_pool) // mempool 생성 실패 - 방금 만든 해시 테이블을 해제해야 함
		goto free_hash;

	disk->zone_wplugs_wq =
		alloc_workqueue("%s_zwplugs", WQ_MEM_RECLAIM | WQ_HIGHPRI,
				pool_size, disk->disk_name); // per-disk 고우선순위 workqueue 할당
	if (!disk->zone_wplugs_wq) // workqueue 생성 실패 - 해시+mempool을 함께 해제해야 함
		goto destroy_pool;

	disk->zone_wplugs_worker =
		kthread_create(disk_zone_wplugs_worker, disk,
			       "%s_zwplugs_worker", disk->disk_name); // 회전형(qd1 writes) 장치용 커널 스레드 생성
	if (IS_ERR(disk->zone_wplugs_worker)) { // 스레드 생성 실패 - 에러 코드를 보존하고 지금까지의 자원을 모두 해제
		ret = PTR_ERR(disk->zone_wplugs_worker); // kthread_create 실패 원인 코드를 반환값으로 보존
		disk->zone_wplugs_worker = NULL; // 실패한 포인터를 NULL로 정리 (dangling 방지)
		goto destroy_wq;
	}
	wake_up_process(disk->zone_wplugs_worker); // zone plug worker 실행 시작

	return 0;

destroy_wq:
	destroy_workqueue(disk->zone_wplugs_wq); // workqueue 해제
	disk->zone_wplugs_wq = NULL; // workqueue 포인터 NULL화
destroy_pool:
	mempool_destroy(disk->zone_wplugs_pool); // mempool 해제
	disk->zone_wplugs_pool = NULL; // mempool 포인터 NULL화
free_hash:
	kfree(disk->zone_wplugs_hash); // hash table 메모리 해제
	disk->zone_wplugs_hash = NULL; // hash table 포인터 NULL화
	disk->zone_wplugs_hash_bits = 0; // hash bits 0으로 초기화
	return ret;
}

/*
 * [한국어]
 * disk_destroy_zone_wplugs_hash_table - 모든 zone plug와 해시 테이블 정리
 *
 * @disk: 대상 gendisk.
 * @return: 없음(void).
 *
 * 재검증 실패나 디스크 제거 시 남아 있는 모든 zone write plug를 강제로
 * DEAD 처리해 정리하는 함수. 각 버킷을 순회하며 첫 항목을
 * disk_mark_zone_wplug_dead()로 DEAD 표시(초기 참조 반환 유발)하는
 * 작업을 버킷이 빌 때까지 반복 — 아직 다른 활성 참조(진행 중인 BIO 등)가
 * 남아 있는 plug라도 DEAD 표시 자체는 즉시 hash unlink를 유발하므로
 * 버킷에서는 사라지지만 실제 free는 그 활성 참조들이 모두 반환된 뒤에야
 * 일어난다(RCU 지연 free). 모든 버킷을 비운 뒤 rcu_barrier()로 진행 중인
 * 모든 RCU 콜백(disk_free_zone_wplug_rcu)이 끝나기를 기다린 다음에야
 * mempool을 안전하게 파괴한다(그렇지 않으면 아직 반환 안 된 plug 메모리가
 * mempool 밖에서 쓰이는 use-after-free 위험).
 * 실행 컨텍스트: 프로세스 컨텍스트 — rcu_barrier()가 블로킹하므로
 * 인터럽트 컨텍스트에서 호출 불가.
 * 호출자: disk_free_zone_resources().
 * 피호출자: disk_zone_wplugs_hash_size(), disk_mark_zone_wplug_dead(),
 * rcu_barrier(), mempool_destroy().
 *
 * 호출 체인:
 *   disk_free_zone_resources() → [disk_destroy_zone_wplugs_hash_table] →
 *   disk_mark_zone_wplug_dead(), rcu_barrier()
 */
static void disk_destroy_zone_wplugs_hash_table(struct gendisk *disk)
{
	struct blk_zone_wplug *zwplug;
	unsigned int i;

	if (!disk->zone_wplugs_hash) // hash table이 없으면 할당된 것도 없음
		return;

	/* Free all the zone write plugs we have. */
	for (i = 0; i < disk_zone_wplugs_hash_size(disk); i++) { // 모든 hash bucket을 순회하며 plug 정리
		while (!hlist_empty(&disk->zone_wplugs_hash[i])) { // bucket에 남은 plug가 있을 때까지 반복
			zwplug = hlist_entry(disk->zone_wplugs_hash[i].first, // bucket의 첫 번째 plug entry 획득
					     struct blk_zone_wplug, node);
			spin_lock_irq(&zwplug->lock); // plug 상태 변경 직렬화
			disk_mark_zone_wplug_dead(zwplug); // DEAD 표시 및 hash ref 해제
			spin_unlock_irq(&zwplug->lock);
		}
	}

	WARN_ON_ONCE(atomic_read(&disk->nr_zone_wplugs)); // 모든 plug가 정리되었는지 검증
	kfree(disk->zone_wplugs_hash); // hash table 메모리 해제
	disk->zone_wplugs_hash = NULL;
	disk->zone_wplugs_hash_bits = 0;

	/*
	 * Wait for the zone write plugs to be RCU-freed before destroying the
	 * mempool.
	 */
	rcu_barrier(); // RCU read-side가 모두 종료될 때까지 대기
	mempool_destroy(disk->zone_wplugs_pool); // plug mempool 해제
	disk->zone_wplugs_pool = NULL;
}

/*
 * [한국어]
 * disk_set_zones_cond_array - zones_cond 캐시 배열을 RCU로 교체
 *
 * @disk: 대상 gendisk.
 * @zones_cond: 새로 설정할 배열(NULL이면 캐시를 완전히 비움 — 자원 해제
 *              시 사용).
 * @return: 없음(void).
 *
 * 재검증이 끝나 새로 만든 zone condition 캐시 배열을 disk->zones_cond에
 * 게시하는 유일한 지점 — rcu_replace_pointer()로 이전 포인터를 원자적으로
 * 교체하고, 이전 배열은 즉시 free하지 않고 kfree_rcu_mightsleep()으로
 * RCU grace period 이후 해제되도록 예약한다(진행 중인 RCU read-side
 * 사용자가 여전히 이전 배열을 참조 중일 수 있으므로).
 * 실행 컨텍스트: 프로세스 컨텍스트 — disk->zone_wplugs_hash_lock으로
 * 포인터 교체를 직렬화.
 * 호출자: disk_update_zone_resources()(재검증 성공 시 새 배열 게시),
 * disk_free_zone_resources()(NULL로 교체해 캐시 제거).
 * 피호출자: rcu_replace_pointer(), kfree_rcu_mightsleep().
 *
 * 호출 체인:
 *   disk_update_zone_resources() / disk_free_zone_resources() →
 *   [disk_set_zones_cond_array]
 */
static void disk_set_zones_cond_array(struct gendisk *disk, u8 *zones_cond)
{
	unsigned long flags;

	spin_lock_irqsave(&disk->zone_wplugs_hash_lock, flags); // zones_cond 포인터 교체 직렬화
	zones_cond = rcu_replace_pointer(disk->zones_cond, zones_cond, // RCU로 새로운 zone condition cache publish
				lockdep_is_held(&disk->zone_wplugs_hash_lock));
	spin_unlock_irqrestore(&disk->zone_wplugs_hash_lock, flags);

	kfree_rcu_mightsleep(zones_cond); // 오래된 zones_cond 배열을 RCU grace period 후 해제
}

/*
 * [한국어] (영어 요약 보강)
 * disk_free_zone_resources - zone write plug 관련 자원을 전부 해제
 *
 * @disk: 대상 gendisk.
 * @return: 없음(void).
 *
 * disk_alloc_zone_resources()가 할당했던 모든 자원(워커 스레드, workqueue,
 * plug 해시/mempool, zones_cond 캐시)과 disk 자체의 zone 관련 카운터를
 * 원래 상태로 되돌리는 총 정리 함수 — 디바이스 제거, 재검증 실패, zone
 * 자원이 더 이상 필요 없어진 경우(예: 재검증 결과 zoned가 아니게 됨) 등에
 * 호출된다. 워커 스레드를 먼저 멈춰(kthread_stop) 더 이상 새로운 plug
 * 접근이 없음을 보장한 뒤, workqueue를 파괴하고, 해시 테이블과 mempool을
 * disk_destroy_zone_wplugs_hash_table()로 정리하고, 마지막으로 zones_cond
 * 캐시와 zone_capacity/last_zone_capacity/nr_zones를 초기화한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 — kthread_stop()과
 * disk_destroy_zone_wplugs_hash_table() 내부의 rcu_barrier()가 모두
 * 블로킹할 수 있음.
 * 호출자: blk_revalidate_disk_zones()(자원 준비/report 실패 시 정리
 * 경로), 디바이스 제거 경로(gendisk 해제).
 * 피호출자: kthread_stop(), destroy_workqueue(),
 * disk_destroy_zone_wplugs_hash_table(), disk_set_zones_cond_array().
 *
 * 호출 체인:
 *   blk_revalidate_disk_zones() (free_resources 경로) →
 *   [disk_free_zone_resources] → disk_destroy_zone_wplugs_hash_table()
 */
void disk_free_zone_resources(struct gendisk *disk)
{
	if (disk->zone_wplugs_worker) // qd1 worker 커널 스레드 종료
		kthread_stop(disk->zone_wplugs_worker);
	WARN_ON_ONCE(!list_empty(&disk->zone_wplugs_list)); // 남은 active plug가 없어야 함

	if (disk->zone_wplugs_wq) {
		destroy_workqueue(disk->zone_wplugs_wq); // per-disk workqueue 제거
		disk->zone_wplugs_wq = NULL;
	}

	disk_destroy_zone_wplugs_hash_table(disk); // 모든 zone plug 및 hash table 제거

	disk_set_zones_cond_array(disk, NULL); // zones_cond 포인터 NULL화
	disk->zone_capacity = 0; // zone_capacity 초기화
	disk->last_zone_capacity = 0; // last_zone_capacity 초기화
	disk->nr_zones = 0; // nr_zones 초기화
}

/*
 * [한국어]
 * struct blk_revalidate_zone_args:
 *   zoned 디스크 probe/재검증(revalidate) 시 blk_revalidate_disk_zones()가
 *   disk->fops->report_zones(nvme_report_zones)로부터 순차적으로 zone
 *   descriptor를 받으며 그 결과를 누적하는 임시(stack) 구조체. Report
 *   Zones 콜백(blk_revalidate_zone_cb)이 이 구조체를 채우고, 완료 후
 *   disk_update_zone_resources()가 이 값들로 disk 필드와 queue limits를
 *   커밋한다. */
struct blk_revalidate_zone_args {
	struct gendisk	*disk;
	/* [한국어] 재검증 대상 gendisk에 대한 역참조.
	 * 설정자: disk_revalidate_zone_resources()가 함수 진입 시 1회 설정.
	 * 읽는 자: blk_revalidate_conv_zone/seq_zone/zone_cb 등 콜백 계열
	 * 함수들이 disk->disk_name(경고 로그), disk_zone_is_last() 등에 사용.
	 * 값 범위: 유효한 gendisk 포인터, NULL 불가.
	 * 동기화: 재검증은 단일 호출 스레드에서 순차 진행되므로 락 불필요. */

	u8		*zones_cond;
	/* [한국어] 새로 구성 중인 zone condition 캐시 배열(zone마다 1바이트) —
	 * 완료되면 disk->zones_cond로 RCU publish될 후보.
	 * 설정자: disk_revalidate_zone_resources()가 kzalloc()으로 nr_zones
	 * 바이트 할당; blk_revalidate_zone_cond()가 각 zone을 검증하며
	 * blk_zone_set_cond()로 채움.
	 * 읽는 자: disk_update_zone_resources()가 성공 시
	 * disk_set_zones_cond_array()로 disk->zones_cond에 RCU 교체.
	 * 값 범위: 재검증 실패 시 NULL로 설정된 뒤 kfree()되어 해제됨(중복
	 * 해제 방지를 위해 소유권 이전 후 NULL 대입).
	 * 동기화: 아직 disk에 게시되지 않은 동안은 이 함수 호출 스레드만
	 * 접근 — RCU publish 이전이므로 별도 락 불필요. */

	unsigned int	nr_zones;
	/* [한국어] Report Zones로 확인한 디스크 전체 zone 개수 (capacity를
	 * chunk_sectors로 나눈 값).
	 * 설정자: disk_revalidate_zone_resources()가
	 * DIV_ROUND_UP_ULL(capacity, chunk_sectors)로 계산.
	 * 읽는 자: blk_revalidate_zone_cond()가 마지막 zone 판정에,
	 * disk_update_zone_resources()가 disk->nr_zones 갱신에 사용.
	 * 값 범위: 1 이상 — 0이면 애초에 zoned 디바이스로 볼 수 없음.
	 * 동기화: 해당 없음(단일 스레드 임시값). */

	unsigned int	nr_conv_zones;
	/* [한국어] 지금까지 확인된 conventional(비순차) zone의 개수 누적값.
	 * 설정자: blk_revalidate_conv_zone()이 conventional zone을 볼 때마다
	 * ++ 로 증가.
	 * 읽는 자: disk_update_zone_resources()가 nr_zones와 비교해 유효성을
	 * 검증(conventional이 전체 이상이면 -ENODEV)하고, sequential zone 수
	 * (nr_zones - nr_conv_zones) 계산에 사용.
	 * 값 범위: 0 이상 nr_zones 미만.
	 * 동기화: 해당 없음(단일 스레드 누적값). */

	unsigned int	zone_capacity;
	/* [한국어] sequential zone들의 표준 capacity(ZNS의 ZCAP, zone 내 실제
	 * 쓰기 가능 용량 — zone 길이보다 작거나 같음).
	 * 설정자: blk_revalidate_seq_zone()이 첫 sequential zone에서 기록하고,
	 * 이후 zone들과 비교해 일관성을 검증.
	 * 읽는 자: disk_update_zone_resources()가 disk->zone_capacity로 커밋.
	 * 값 범위: 0(아직 미설정)이거나 zone 길이 이하의 양수 — 마지막 zone은
	 * 예외적으로 다를 수 있어 last_zone_capacity로 별도 관리.
	 * 동기화: 해당 없음(단일 스레드 임시값). */

	unsigned int	last_zone_capacity;
	/* [한국어] 디스크의 마지막 zone(capacity가 나머지와 다를 수 있음)의
	 * capacity.
	 * 설정자: blk_revalidate_conv_zone()/blk_revalidate_seq_zone()이
	 * disk_zone_is_last()로 마지막 zone을 식별했을 때 기록.
	 * 읽는 자: disk_update_zone_resources()가 disk->last_zone_capacity로
	 * 커밋; disk_zone_wplug_is_full()이 마지막 zone의 FULL 판정 기준으로
	 * 사용.
	 * 값 범위: 0(아직 마지막 zone을 못 봄)이거나 양수.
	 * 동기화: 해당 없음(단일 스레드 임시값). */

	sector_t	sector;
	/* [한국어] 지금까지 검증을 마친 다음에 이어져야 할 예상 zone 시작
	 * sector — Report Zones 응답에 zone 사이 간격(gap)이나 순서 이상이
	 * 없는지 검증하는 커서(cursor) 역할.
	 * 설정자: blk_revalidate_zone_cb()가 매 zone 검증 성공 시
	 * += zone->len으로 전진; 초기값은 0(구조체 { } 초기화).
	 * 읽는 자: blk_revalidate_zone_cb()가 다음 zone->start와 비교해 gap을
	 * 검출하고, blk_revalidate_disk_zones()가 최종적으로 capacity와
	 * 일치하는지 검사(불일치 시 zone 누락으로 판단).
	 * 값 범위: 0부터 disk capacity까지 단조 증가.
	 * 동기화: 해당 없음(단일 스레드 누적값). */
};

/*
 * [한국어]
 * disk_revalidate_zone_resources - zone 재검증에 필요한 자원을 준비
 *
 * @disk: 재검증 대상 gendisk. queue->limits.chunk_sectors 등 이미
 *        드라이버가 설정해 둔 zone 관련 limits를 읽어들인다.
 * @args: 채워 넣을 blk_revalidate_zone_args. nr_zones/zones_cond가
 *        이 함수에서 계산/할당된다.
 * @return: 0(성공, 또는 zone 자원이 애초에 필요 없는 디바이스),
 *          -ENOMEM(zones_cond 캐시 배열 또는 zone plug 자원 할당 실패).
 *
 * NVMe ZNS namespace가 포맷되거나 리사이즈된 후, 드라이버가
 * blk_revalidate_disk_zones()를 호출하면 그 초입에서 이 함수가 실행되어
 * (1) zone 개수를 계산하고 (2) zone condition 캐시 배열을 새로 할당하고
 * (3) request 기반(blk-mq) 장치라면 zone write plug 해시/mempool/workqueue/
 * 워커까지 준비한다. pool 크기는 디바이스가 광고하는 max_open_zones /
 * max_active_zones 한도 중 큰 값을 쓰며, 한도가 없으면 기본값
 * BLK_ZONE_WPLUG_DEFAULT_POOL_SIZE(128)와 zone 개수 중 작은 값을 쓴다.
 * 프로세스 컨텍스트에서 GFP_NOIO 하에 실행되며 동시 재진입은 상정하지
 * 않는다(드라이버가 단일 스레드에서 순차 호출).
 * 호출자: blk_revalidate_disk_zones().
 * 피호출자: disk_need_zone_resources(), disk_alloc_zone_resources().
 * 에러 시 blk_revalidate_disk_zones()가 free_resources 경로로 정리한다.
 *
 * 호출 체인:
 *   blk_revalidate_disk_zones() → [disk_revalidate_zone_resources] →
 *   disk_alloc_zone_resources()
 */
static int disk_revalidate_zone_resources(struct gendisk *disk,
				struct blk_revalidate_zone_args *args)
{
	struct queue_limits *lim = &disk->queue->limits; // queue limits (chunk_sectors, max_open/active_zones)
	unsigned int pool_size;
	int ret = 0; // 재검증 결과 코드

	args->disk = disk; // 이후 콜백들이 disk_name 등 disk 전역 상태에 접근할 수 있도록 역참조 저장
	args->nr_zones =
		DIV_ROUND_UP_ULL(get_capacity(disk), lim->chunk_sectors); // capacity / chunk_sectors로 ZNS 총 zone 수 계산
	// ZNS namespace 총 zone 수 = capacity / chunk_sectors

	/* Cached zone conditions: 1 byte per zone */
	args->zones_cond = kzalloc(args->nr_zones, GFP_NOIO); // zone마다 1바이트 condition cache 할당 (ZNS zone state 배열)
	if (!args->zones_cond)
		return -ENOMEM;

	if (!disk_need_zone_resources(disk)) // BIO 기반이며 zone append 에뮬레이션도 불필요하면 plug 자원 자체가 필요 없음
		return 0; // zones_cond 캐시만으로 충분 - 여기서 조기 반환

	/*
	 * If the device has no limit on the maximum number of open and active
	 * zones, use BLK_ZONE_WPLUG_DEFAULT_POOL_SIZE.
	 */
	pool_size = max(lim->max_open_zones, lim->max_active_zones); // open/active zone 한도를 plug pool 크기로 산정
	if (!pool_size) // 디바이스가 한도를 광고하지 않은 경우
		pool_size =
		// open/active zone 한도를 반영한 plug pool 크기
			min(BLK_ZONE_WPLUG_DEFAULT_POOL_SIZE, args->nr_zones); // 기본 128 또는 seq zone 수 중 작은 값 사용

	if (!disk->zone_wplugs_hash) { // plug hash가 아직 없을 때만 자원 할당
		ret = disk_alloc_zone_resources(disk, pool_size); // hash/mempool/workqueue/worker 생성
		if (ret)
			kfree(args->zones_cond); // 자원 할당 실패 시 condition cache 해제
	}

	return ret;
}

/*
 * Update the disk zone resources information and device queue limits.
 * The disk queue is frozen when this is executed.
 */
/*
 * [한국어] (영어 요약 보강)
 * disk_update_zone_resources - 재검증 결과를 disk/queue limits에 커밋
 *
 * @disk: 대상 gendisk.
 * @args: blk_revalidate_zone_cb()가 모든 zone을 검증하며 채운 결과.
 * @return: 0(성공) 또는 음수 errno(-ENODEV 등).
 *
 * blk_revalidate_disk_zones()가 report_zones로 모든 zone을 확인하고 이상
 * 없음을 검증한 뒤 마지막으로 호출하는 커밋 단계. disk->nr_zones/
 * zone_capacity/last_zone_capacity를 갱신하고, 새 zones_cond 배열을
 * disk_set_zones_cond_array()로 RCU 게시한다. max_open_zones/
 * max_active_zones가 sequential zone 총수 이상이면 "사실상 무제한"으로
 * 간주해 0으로 정규화하고, plug pool이 있다면 그 한도에 맞춰
 * mempool_resize()로 크기를 조정한다(진짜 무제한 디바이스는 mempool
 * 크기를 사용자에게 성능 힌트로 노출하기 위해 max_open_zones를 pool
 * 크기로 다시 채움). queue_limits_commit_update()로 새 limits를 큐에
 * 원자적으로 반영한다.
 * 실행 컨텍스트: 호출 시점에 이미 blk_mq_freeze_queue()로 큐가 얼어 있어
 * (freeze) 진행 중인 I/O 제출과 경쟁하지 않는다.
 * 호출자: blk_revalidate_disk_zones().
 * 피호출자: queue_limits_start_update/commit_update/cancel_update(),
 * blk_mq_freeze_queue/unfreeze_queue(), disk_set_zones_cond_array(),
 * mempool_resize(), disk_free_zone_resources()(commit 실패 시).
 * 에러 경로: conventional zone 수가 전체 이상이면 -ENODEV로 취소하고
 * unfreeze; queue_limits_commit_update() 실패 시
 * disk_free_zone_resources()로 모든 자원을 되돌린 뒤 unfreeze.
 *
 * 호출 체인:
 *   blk_revalidate_disk_zones() → [disk_update_zone_resources] →
 *   queue_limits_commit_update()
 */
static int disk_update_zone_resources(struct gendisk *disk,
				      struct blk_revalidate_zone_args *args)
{
	struct request_queue *q = disk->queue; // gendisk의 request_queue (NVMe namespace queue)
	unsigned int nr_seq_zones; // sequential zone 개수
	unsigned int pool_size, memflags; // mempool resize 및 queue freeze 보관용
	struct queue_limits lim;
	int ret = 0; // 갱신할 queue_limits 사본

	lim = queue_limits_start_update(q); // atomic queue limits 갱신 시작

	memflags = blk_mq_freeze_queue(q); // NVMe command 제출과의 경쟁을 막기 위해 queue freeze

	disk->nr_zones = args->nr_zones; // disk의 전체 zone 수 설정
	if (args->nr_conv_zones >= disk->nr_zones) { // conventional zone 수가 전체 zone 수 이상이면 비정상
		queue_limits_cancel_update(q);
		pr_warn("%s: Invalid number of conventional zones %u / %u\n",
			disk->disk_name, args->nr_conv_zones, disk->nr_zones);
		ret = -ENODEV;
		goto unfreeze;
	}

	disk->zone_capacity = args->zone_capacity; // 일반 ZNS zone capacity 설정
	disk->last_zone_capacity = args->last_zone_capacity; // 마지막 ZNS zone capacity 설정
	disk_set_zones_cond_array(disk, args->zones_cond); // 새 zone condition cache publish
	args->zones_cond = NULL;

	/*
	 * Some devices can advertise zone resource limits that are larger than
	 * the number of sequential zones of the zoned block device, e.g. a
	 * small ZNS namespace. For such case, assume that the zoned device has
	 * no zone resource limits.
	 */
	nr_seq_zones = disk->nr_zones - args->nr_conv_zones; // sequential zone 수 계산
	if (lim.max_open_zones >= nr_seq_zones) // max_open_zones가 sequential zone 수 이상이면 무제한 간주
		lim.max_open_zones = 0; // 무제한으로 설정
	if (lim.max_active_zones >= nr_seq_zones) // max_active_zones가 sequential zone 수 이상이면 무제한 간주
	// open/active zone 한도가 sequential zone 수 이상이면 무제한 간주
		lim.max_active_zones = 0; // 무제한으로 설정

	if (!disk->zone_wplugs_pool) // plug pool이 없으면 limits만 갱신
		goto commit;

	/*
	 * If the device has no limit on the maximum number of open and active
	 * zones, set its max open zone limit to the mempool size to indicate
	 * to the user that there is a potential performance impact due to
	 * dynamic zone write plug allocation when simultaneously writing to
	 * more zones than the size of the mempool.
	 */
	pool_size = max(lim.max_open_zones, lim.max_active_zones); // open/active zone 한도로 pool 크기 재계산
	if (!pool_size) // 한도가 없으면
		pool_size = min(BLK_ZONE_WPLUG_DEFAULT_POOL_SIZE, nr_seq_zones); // 기본값 또는 sequential zone 수 중 작은 값
		// 한도 없으면 기본 128 또는 seq zone 수 중 작은 값

	mempool_resize(disk->zone_wplugs_pool, pool_size); // mempool 크기를 새로운 한도에 맞게 조정

	if (!lim.max_open_zones && !lim.max_active_zones) { // open/active 한도가 모두 없는 경우
		if (pool_size < nr_seq_zones) // pool 크기가 sequential zone 수보다 작으면
			lim.max_open_zones = pool_size; // max_open_zones를 pool 크기로 제한
		else
			lim.max_open_zones = 0; // 그렇지 않으면 한도 없음
	}

commit:
	ret = queue_limits_commit_update(q, &lim); // 새 queue limits를 blk-mq 및 NVMe driver에 반영

unfreeze:
	if (ret) // limits 갱신 실패 시 모든 zone 자원 해제
		disk_free_zone_resources(disk);

	blk_mq_unfreeze_queue(q, memflags); // queue freeze 해제; NVMe I/O 재개

	return ret;
}

/*
 * [한국어]
 * blk_revalidate_zone_cond - zone condition이 zone type과 일관되는지 검증
 *
 * @zone: 검증할 zone descriptor.
 * @idx: 검증 통과 시 zones_cond 배열에 기록할 인덱스.
 * @args: 진행 중인 재검증 컨텍스트(disk, zones_cond 배열 포함).
 * @return: 0(검증 통과, zones_cond[idx]에 기록 완료) 또는 -ENODEV(조건과
 *          타입 불일치, 또는 정의되지 않은 condition 값).
 *
 * NVMe ZNS Report Zones 응답의 Zone State가 Zone Type과 모순되지 않는지
 * 확인한다 — conventional zone은 반드시 NOT_WP여야 하고, sequential
 * zone은 그 외 정의된 condition(IMP_OPEN/EXP_OPEN/CLOSED/EMPTY/FULL/
 * OFFLINE/READONLY) 중 하나여야 한다. 정의되지 않은 값이 오면 디바이스
 * 펌웨어 버그 가능성으로 보고 재검증 자체를 실패시킨다.
 * 실행 컨텍스트: blk_revalidate_disk_zones() 진행 중인 단일 스레드.
 * 호출자: blk_revalidate_zone_cb().
 * 피호출자: blk_zone_set_cond().
 * 에러 경로: 불일치/미정의 값은 pr_warn()으로 사용자에게 알리고 -ENODEV —
 * 이는 blk_revalidate_zone_cb()를 거쳐 최종적으로 report_zones 루프
 * 전체를 중단시킨다.
 *
 * 호출 체인:
 *   blk_revalidate_zone_cb() → [blk_revalidate_zone_cond] →
 *   blk_zone_set_cond()
 */
static int blk_revalidate_zone_cond(struct blk_zone *zone, unsigned int idx,
				    struct blk_revalidate_zone_args *args)
{
	enum blk_zone_cond cond = zone->cond;

	/* Check that the zone condition is consistent with the zone type. */
	switch (cond) {
	case BLK_ZONE_COND_NOT_WP:
		if (zone->type != BLK_ZONE_TYPE_CONVENTIONAL) // conventional zone은 반드시 NOT_WP 조건이어야 함
			goto invalid_condition;
		break;
	case BLK_ZONE_COND_IMP_OPEN:
	case BLK_ZONE_COND_EXP_OPEN:
	case BLK_ZONE_COND_CLOSED:
	case BLK_ZONE_COND_EMPTY:
	case BLK_ZONE_COND_FULL:
	case BLK_ZONE_COND_OFFLINE:
	case BLK_ZONE_COND_READONLY:
		if (zone->type != BLK_ZONE_TYPE_SEQWRITE_REQ) // sequential zone은 active/open/closed/empty/full/offline/readonly 조건이어야 함
			goto invalid_condition;
		break;
	default:
		pr_warn("%s: Invalid zone condition 0x%X\n",
			args->disk->disk_name, cond);
		return -ENODEV;
	}

	blk_zone_set_cond(args->zones_cond, idx, cond); // 검증된 condition을 zones_cond cache에 기록

	return 0;

invalid_condition:
	pr_warn("%s: Invalid zone condition 0x%x for type 0x%x\n",
		args->disk->disk_name, cond, zone->type);

	return -ENODEV;
}

/*
 * [한국어]
 * blk_revalidate_conv_zone - conventional zone 하나를 검증하고 집계
 *
 * @zone: 검증할 conventional zone descriptor.
 * @idx: (사용하지 않음 — blk_revalidate_zone_cb의 콜백 시그니처를 맞추기
 *       위해서만 존재).
 * @args: 진행 중인 재검증 컨텍스트(nr_conv_zones/last_zone_capacity 누적).
 * @return: 0(검증 통과) 또는 -ENODEV(capacity != len).
 *
 * conventional zone은 WP 개념이 없어 zone 전체가 곧 capacity여야 한다
 * (capacity가 len보다 작을 이유가 없음) — 이 불변조건을 검증하고, 마지막
 * zone이면 args->last_zone_capacity를 기록하며, conventional zone
 * 개수를 args->nr_conv_zones에 누적한다(이후 sequential zone 수 계산에
 * 사용).
 * 실행 컨텍스트: blk_revalidate_disk_zones() 진행 중인 단일 스레드.
 * 호출자: blk_revalidate_zone_cb()의 BLK_ZONE_TYPE_CONVENTIONAL 분기.
 * 피호출자: disk_zone_is_last().
 *
 * 호출 체인:
 *   blk_revalidate_zone_cb() → [blk_revalidate_conv_zone]
 */
static int blk_revalidate_conv_zone(struct blk_zone *zone, unsigned int idx,
				    struct blk_revalidate_zone_args *args)
{
	struct gendisk *disk = args->disk;

	if (zone->capacity != zone->len) { // conventional zone은 capacity == len이어야 함
		pr_warn("%s: Invalid conventional zone capacity\n",
			disk->disk_name);
		return -ENODEV;
	}

	if (disk_zone_is_last(disk, zone)) // 마지막 conventional zone 처리
		args->last_zone_capacity = zone->capacity; // 마지막 zone capacity 기록

	args->nr_conv_zones++; // conventional zone 개수 증가

	return 0;
}

/*
 * [한국어] (영어 요약 보강)
 * blk_revalidate_seq_zone - sequential zone 하나를 검증하고 필요 시 plug 생성
 *
 * @zone: 검증할 sequential zone descriptor.
 * @idx: (사용하지 않음 — 콜백 시그니처 통일용).
 * @args: 진행 중인 재검증 컨텍스트(zone_capacity 표준값, disk 등).
 * @return: 0(검증 통과, plug 생성도 성공) 또는 -ENODEV(capacity 불일치),
 *          -ENOMEM(WP 추적용 plug 할당 실패).
 *
 * 모든 sequential zone은 마지막 zone을 제외하면 동일한 capacity를 가져야
 * 한다는 ZNS 규칙을 검증하고(첫 zone에서 표준값을 기록, 이후 zone들과
 * 비교), zone append 에뮬레이션이 필요한 디바이스라면 이미 부분적으로
 * 쓰인(WP가 0도 아니고 zone capacity도 아닌) zone에 대해 미리
 * disk_get_or_alloc_zone_wplug()로 plug를 만들어 둔다 — 재검증 이후 첫
 * write/append가 도착하기 전에 이미 WP를 정확히 추적할 준비를 해 두는
 * 것. plug hash 자체가 없는 디바이스(zone 자원이 필요 없는 BIO 기반
 * 드라이버)는 이 단계를 건너뛴다.
 * 실행 컨텍스트: blk_revalidate_disk_zones() 진행 중인 단일 스레드,
 * GFP_NOIO 하에서 할당.
 * 호출자: blk_revalidate_zone_cb()의 BLK_ZONE_TYPE_SEQWRITE_REQ 분기.
 * 피호출자: disk_zone_is_last(), disk_zone_wplug_sync_wp_offset(),
 * disk_get_or_alloc_zone_wplug(), disk_put_zone_wplug().
 *
 * 호출 체인:
 *   blk_revalidate_zone_cb() → [blk_revalidate_seq_zone] →
 *   disk_get_or_alloc_zone_wplug()
 */
static int blk_revalidate_seq_zone(struct blk_zone *zone, unsigned int idx,
				   struct blk_revalidate_zone_args *args)
{
	struct gendisk *disk = args->disk;
	struct blk_zone_wplug *zwplug;
	unsigned int wp_offset;

	/*
	 * Remember the capacity of the first sequential zone and check
	 * if it is constant for all zones, ignoring the last zone as it can be
	 * smaller.
	 */
	if (!args->zone_capacity) // 첫 sequential zone의 capacity를 표준으로 삼음
		args->zone_capacity = zone->capacity;
	if (disk_zone_is_last(disk, zone)) { // 마지막 zone은 더 작을 수 있음
		args->last_zone_capacity = zone->capacity;
	} else if (zone->capacity != args->zone_capacity) { // 가변 zone capacity는 ZNS 규격 위반
		pr_warn("%s: Invalid variable zone capacity\n",
			disk->disk_name);
		return -ENODEV;
	}

	/*
	 * If the device needs zone append emulation, we need to track the
	 * write pointer of all zones that are not empty nor full. So make sure
	 * we have a zone write plug for such zone if the device has a zone
	 * write plug hash table.
	 */
	if (!disk->zone_wplugs_hash) // plug hash가 없으면 WP 추적 불필요
		return 0;

	wp_offset = disk_zone_wplug_sync_wp_offset(disk, zone); // Report Zones 결과로 plug WP 동기화
	if (!wp_offset || wp_offset >= zone->capacity) // EMPTY나 FULL이면 plug 생성 불필요
		return 0;

	zwplug = disk_get_or_alloc_zone_wplug(disk, zone->wp, GFP_NOIO); // 중간 WP를 가진 zone에 plug 생성
	if (!zwplug) // plug 할당 실패 시 메모리 부족
		return -ENOMEM;
	disk_put_zone_wplug(zwplug); // 생성용 reference 해제

	return 0;
}

/*
 * Helper function to check the validity of zones of a zoned block device.
 */
/*
 * [한국어]
 * blk_revalidate_zone_cb - report_zones 콜백: zone 하나씩 레이아웃 검증
 *
 * @zone: 디바이스가 보고한 zone descriptor.
 * @idx: report 요청 내 순번(zones_cond 배열 인덱스로도 사용).
 * @data: struct blk_revalidate_zone_args* 로 캐스팅되는 진행 상태.
 * @return: 0(이 zone까지 검증 통과) 또는 -ENODEV(레이아웃/타입/조건 이상).
 *
 * blk_revalidate_disk_zones()가 disk->fops->report_zones에 등록하는
 * 콜백 — Report Zones 응답으로 zone descriptor가 하나씩 올 때마다 이
 * 함수가 호출되어 (1) zone 사이에 gap이 없는지(args->sector와
 * zone->start 비교), (2) zone 시작/길이가 capacity 범위 안인지, (3)
 * 마지막 zone을 제외한 모든 zone 크기가 동일한지, (4) capacity가
 * 0보다 크고 zone 길이 이하인지를 검사한 뒤, condition 일관성
 * (blk_revalidate_zone_cond)과 타입별 세부 검증(conventional은
 * blk_revalidate_conv_zone, sequential은 blk_revalidate_seq_zone)으로
 * 위임한다. 성공하면 args->sector를 다음 예상 zone 시작 위치로 전진시켜
 * 다음 콜백 호출에서 gap 검사를 이어갈 수 있게 한다.
 * 실행 컨텍스트: blk_revalidate_disk_zones()가 report_zones를 호출하는
 * 동안 그 드라이버 콜백 스택 안에서 실행(대개 프로세스 컨텍스트).
 * 호출자: disk->fops->report_zones 구현체(예: nvme_report_zones)가 zone
 * 하나를 확인할 때마다 args->cb()로 호출.
 * 피호출자: disk_zone_is_last(), blk_revalidate_zone_cond(),
 * blk_revalidate_conv_zone(), blk_revalidate_seq_zone().
 * 에러 경로: 어느 검증이든 실패하면 pr_warn()으로 원인을 남기고 -ENODEV —
 * 드라이버의 report_zones 루프가 이 값을 보고 즉시 중단, 결과적으로
 * blk_revalidate_disk_zones()가 free_resources 경로로 진입.
 *
 * 호출 체인:
 *   nvme_report_zones() → [blk_revalidate_zone_cb] →
 *   blk_revalidate_conv_zone() / blk_revalidate_seq_zone()
 */
static int blk_revalidate_zone_cb(struct blk_zone *zone, unsigned int idx,
				  void *data)
{
	struct blk_revalidate_zone_args *args = data;
	struct gendisk *disk = args->disk;
	sector_t zone_sectors = disk->queue->limits.chunk_sectors;
	int ret;

	/* Check for bad zones and holes in the zone report */
	if (zone->start != args->sector) { // ZNS zone layout은 연속적이어야 함 (gap 불가)
		pr_warn("%s: Zone gap at sectors %llu..%llu\n",
			disk->disk_name, args->sector, zone->start);
		return -ENODEV;
	}

	if (zone->start >= get_capacity(disk) || !zone->len) { // zone 시작/길이가 capacity 범위를 벗어나면 비정상
		pr_warn("%s: Invalid zone start %llu, length %llu\n",
			disk->disk_name, zone->start, zone->len);
		return -ENODEV;
	}

	/*
	 * All zones must have the same size, with the exception on an eventual
	 * smaller last zone.
	 */
	if (!disk_zone_is_last(disk, zone)) { // 마지막 zone을 제외한 모든 zone은 동일 크기
		if (zone->len != zone_sectors) { // zone size 불일치 시 ZNS namespace 무효
			pr_warn("%s: Invalid zoned device with non constant zone size\n",
				disk->disk_name);
			return -ENODEV;
		}
	} else if (zone->len > zone_sectors) { // 마지막 zone은 표준 크기를 초과할 수 없음
		pr_warn("%s: Invalid zoned device with larger last zone size\n",
			disk->disk_name);
		return -ENODEV;
	}

	if (!zone->capacity || zone->capacity > zone->len) { // zone capacity는 0보다 크고 zone len 이하여야 함
		pr_warn("%s: Invalid zone capacity\n",
			disk->disk_name);
		return -ENODEV;
	}

	/* Check zone condition */
	ret = blk_revalidate_zone_cond(zone, idx, args); // zone condition 일관성 검증
	if (ret)
		return ret;

	/* Check zone type */
	switch (zone->type) {
	case BLK_ZONE_TYPE_CONVENTIONAL:
		ret = blk_revalidate_conv_zone(zone, idx, args); // conventional zone 처리
		break;
	case BLK_ZONE_TYPE_SEQWRITE_REQ:
		ret = blk_revalidate_seq_zone(zone, idx, args); // sequential zone 처리 및 WP 추적 plug 생성
		break;
	case BLK_ZONE_TYPE_SEQWRITE_PREF:
	default:
		pr_warn("%s: Invalid zone type 0x%x at sectors %llu\n",
			disk->disk_name, (int)zone->type, zone->start);
		ret = -ENODEV;
	}

	if (!ret) // 검증 성공 시 다음 예상 zone 시작 위치로 이동
		args->sector += zone->len; // 다음 zone 시작 sector 갱신

	return ret;
}

/**
 * blk_revalidate_disk_zones - (re)allocate and initialize zone write plugs
 * @disk:	Target disk
 *
 * Helper function for low-level device drivers to check, (re) allocate and
 * initialize resources used for managing zoned disks. This function should
 * normally be called by blk-mq based drivers when a zoned gendisk is probed
 * and when the zone configuration of the gendisk changes (e.g. after a format).
 * Before calling this function, the device driver must already have set the
 * device zone size (chunk_sector limit) and the max zone append limit.
 * BIO based drivers can also use this function as long as the device queue
 * can be safely frozen.
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * blk_revalidate_disk_zones - zone 자원을 (재)할당/초기화
 *
 * @disk: 대상 gendisk(zone size/max zone append limit이 드라이버에 의해
 *        이미 설정돼 있어야 함).
 * @return: 0(성공) 또는 음수 errno(-EIO/-ENODEV/-ENOMEM 등).
 *
 * zoned gendisk가 probe되거나 zone 구성이 바뀔 때마다(예: 포맷 후) 저수준
 * 드라이버가 호출하는 최상위 재검증 함수. 전체 흐름: (1) queue가
 * zoned인지, capacity/zone_sectors가 유효한지 확인, (2)
 * disk_revalidate_zone_resources()로 zones_cond 배열과(필요하면) zone
 * plug 자원을 준비, (3) disk->fops->report_zones(0, UINT_MAX, ...)로
 * 디바이스 전체 zone을 한 번에 report 요청 — 콜백
 * blk_revalidate_zone_cb()가 zone마다 레이아웃/타입/조건을 검증하고
 * 필요하면 plug까지 생성, (4) report된 zone들이 capacity 전체를 빈틈없이
 * 커버했는지 최종 확인, (5) disk_update_zone_resources()로 검증 결과를
 * disk 필드와 queue limits에 커밋. 각 단계 GFP 컨텍스트는
 * memalloc_noio_save/restore로 GFP_NOIO를 강제한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 — BIO 기반 드라이버도 큐를 안전하게
 * freeze할 수만 있으면 사용 가능(커널 문서 명시).
 * 호출자: 드라이버의 probe/재검증 경로(NVMe라면 nvme_revalidate_zones
 * 계열).
 * 피호출자: disk_revalidate_zone_resources(), disk->fops->report_zones()
 * → nvme_report_zones(), disk_update_zone_resources(),
 * disk_free_zone_resources()(실패 시 정리).
 * 에러 경로: zoned 큐가 아니거나 capacity/zone_sectors가 무효하면 조기
 * 반환; report_zones 실패나 zone 누락, limits 커밋 실패는 모두
 * free_resources 라벨로 모여 disk_free_zone_resources()로 자원을
 * 되돌린 뒤 에러를 반환.
 *
 * 호출 체인:
 *   nvme_revalidate_zones() 등 드라이버 → [blk_revalidate_disk_zones] →
 *   disk->fops->report_zones() → nvme_report_zones() →
 *   blk_revalidate_zone_cb() → disk_update_zone_resources()
 */
int blk_revalidate_disk_zones(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; // gendisk의 request_queue
	sector_t zone_sectors = q->limits.chunk_sectors; // ZNS zone size
	sector_t capacity = get_capacity(disk); // NVMe namespace capacity
	struct blk_revalidate_zone_args args = { }; // 모든 필드 0/NULL로 초기화 - sector 커서와 zones_cond 등은 콜백이 채워나감
	unsigned int memflags, noio_flag;
	struct blk_report_zones_args rep_args = {
		.cb = blk_revalidate_zone_cb, // report_zones가 zone마다 부를 검증 콜백 등록
		.data = &args, // 콜백에 진행 상태(args)를 전달하기 위한 연결
	};
	int ret = -ENOMEM;

	if (WARN_ON_ONCE(!blk_queue_is_zoned(q))) // zoned queue가 아니면 zone 자원 할당 불가
		return -EIO;

	if (!capacity) // 용량이 0이면 ZNS namespace로 취급 불가
		return -ENODEV;

	/*
	 * Checks that the device driver indicated a valid zone size and that
	 * the max zone append limit is set.
	 */
	if (!zone_sectors || !is_power_of_2(zone_sectors)) { // ZNS는 zone size가 2의 거듭제곱이어야 함
		pr_warn("%s: Invalid non power of two zone size (%llu)\n",
			disk->disk_name, zone_sectors);
		return -ENODEV;
		// NVMe ZNS는 zone size가 2의 거듭제곱이어야 함
	}

	/*
	 * Ensure that all memory allocations in this context are done as if
	 * GFP_NOIO was specified.
	 */
	noio_flag = memalloc_noio_save(); // 재검증 중 모든 메모리 할당을 NOIO로 처리
	ret = disk_revalidate_zone_resources(disk, &args); // zone plug/hash/mempool 자원 준비
	if (ret) {
		memalloc_noio_restore(noio_flag);
		return ret;
	}

	ret = disk->fops->report_zones(disk, 0, UINT_MAX, &rep_args); // 전체 zone descriptor를 NVMe Report Zones로 획득
	// NVMe ZNS Report Zones로 전체 zone descriptor 획득
	if (!ret) { // 디바이스가 zone을 report하지 않으면 비정상
		pr_warn("%s: No zones reported\n", disk->disk_name);
		ret = -ENODEV;
	}
	memalloc_noio_restore(noio_flag);

	if (ret <= 0) // report_zones 실패 시 자원 해제
		goto free_resources;

	/*
	 * If zones where reported, make sure that the entire disk capacity
	 * has been checked.
	 */
	if (args.sector != capacity) { // report된 zone들이 전체 capacity를 커버해야 함
		pr_warn("%s: Missing zones from sector %llu\n",
			disk->disk_name, args.sector);
		ret = -ENODEV;
		goto free_resources;
		// report된 zone이 전체 capacity를 커버해야 유효
	}

	ret = disk_update_zone_resources(disk, &args); // disk 구조체와 queue limits를 갱신
	if (ret)
		goto free_resources;

	return 0;

free_resources:
	pr_warn("%s: failed to revalidate zones\n", disk->disk_name);

	kfree(args.zones_cond); // 할당된 zones_cond 메모리 해제
	memflags = blk_mq_freeze_queue(q); // 자원 해제 중 queue freeze
	disk_free_zone_resources(disk); // 모든 zone 자원 해제
	blk_mq_unfreeze_queue(q, memflags); // queue freeze 해제

	return ret;
}
EXPORT_SYMBOL_GPL(blk_revalidate_disk_zones);

/**
 * blk_zone_issue_zeroout - zero-fill a block range in a zone
 * @bdev:	blockdev to write
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *  Zero-fill a block range in a zone (@sector must be equal to the zone write
 *  pointer), handling potential errors due to the (initially unknown) lack of
 *  hardware offload (See blkdev_issue_zeroout()).
 */
/*
 * [한국어] (영어 kerneldoc 보강)
 * blk_zone_issue_zeroout - zone 내 범위를 0으로 채움(WP 어긋남 자동 복구 포함)
 *
 * @bdev: 대상 block_device.
 * @sector: zero-fill을 시작할 sector(zone의 현재 WP와 일치해야 함 —
 *          sequential write 규칙).
 * @nr_sects: zero-fill할 섹터 수.
 * @gfp_mask: bio_alloc 등에 사용할 메모리 할당 플래그.
 * @return: 0(성공) 또는 음수 errno.
 *
 * blkdev_issue_zeroout()의 zone-aware 래퍼. 먼저 하드웨어 오프로드
 * zero-fill(WRITE ZEROES 등)을 BLKDEV_ZERO_NOFALLBACK로 시도해 순수하게
 * 오프로드만 사용하고, 디바이스가 이를 지원하지 않으면(-EOPNOTSUPP) 일반
 * write로 폴백해야 한다. 문제는 실패한 그 시도조차 이미 host 측 WP 미러
 * (zone write plug의 wp_offset)를 전진시켜 버렸을 수 있다는 점 —
 * 그래서 실패를 감지하면 disk->fops->report_zones()로 zone 하나를 다시
 * 조회해(NULL 콜백으로 호출 — disk_report_zone이 내부적으로 WP 캐시만
 * 동기화하고 사용자 콜백은 생략) 정확한 WP로 되돌린 뒤, 이번에는
 * BLKDEV_ZERO_NOFALLBACK 없이 재시도해 일반 zero-page write로 강제
 * 폴백시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트 — blkdev_issue_zeroout() 자체가
 * BIO 제출/완료 대기를 포함해 블로킹.
 * 호출자: blkdev_issue_zeroout()(zoned 디바이스 감지 시 이 함수로 위임하는
 * 상위 계층), 파일시스템의 zone 초기화 경로.
 * 피호출자: blkdev_issue_zeroout(), disk->fops->report_zones()(WP 재동기화
 * 전용 호출).
 * 에러 경로: zoned가 아니면 -EIO(버그 상황); report_zones가 정확히 1개를
 * 반환하지 않으면 그 값(음수) 또는 -EIO로 변환.
 *
 * 호출 체인:
 *   blkdev_issue_zeroout() → [blk_zone_issue_zeroout] →
 *   (실패 시) disk->fops->report_zones() → blkdev_issue_zeroout()(재시도)
 */
int blk_zone_issue_zeroout(struct block_device *bdev, sector_t sector,
			   sector_t nr_sects, gfp_t gfp_mask)
{
	struct gendisk *disk = bdev->bd_disk; // target block_device
	int ret;

	if (WARN_ON_ONCE(!bdev_is_zoned(bdev))) // zoned 장치가 아니면 zeroout 의미 없음
		return -EIO;

	ret = blkdev_issue_zeroout(bdev, sector, nr_sects, gfp_mask, // hardware offload zeroout 시도
				   BLKDEV_ZERO_NOFALLBACK);
	if (ret != -EOPNOTSUPP) // hardware offload 성공/다른 오류는 즉시 반환
		return ret;

	/*
	 * The failed call to blkdev_issue_zeroout() advanced the zone write
	 * pointer. Undo this using a report zone to update the zone write
	 * pointer to the correct current value.
	 */
	ret = disk->fops->report_zones(disk, sector, 1, NULL); // zeroout 실패로 WP가 어긋났을 때 Report Zones로 재확인
	if (ret != 1)
		return ret < 0 ? ret : -EIO; // Report Zones 결과가 비정상이면 오류 반환
	// zeroout 실패로 WP 어긋남 -> Report Zones로 재확인

	/*
	 * Retry without BLKDEV_ZERO_NOFALLBACK to force the fallback to a
	 * regular write with zero-pages.
	 */
	return blkdev_issue_zeroout(bdev, sector, nr_sects, gfp_mask, 0); // zero-page write로 fallback 시도
}
EXPORT_SYMBOL_GPL(blk_zone_issue_zeroout);

#ifdef CONFIG_BLK_DEBUG_FS
/* [한국어] CONFIG_BLK_DEBUG_FS(블록 계층 debugfs 인터페이스)가 켜진
 * 빌드에서만 아래 두 함수를 포함 - /sys/kernel/debug/block/<dev>/zone_wplugs
 * 같은 진단 파일에서 zone write plug 내부 상태를 노출하기 위함이며,
 * 프로덕션 커널에서는 불필요한 코드를 배제하기 위해 조건부 컴파일한다. */

/*
 * [한국어]
 * queue_zone_wplug_show - zone write plug 하나의 상태를 debugfs로 출력
 *
 * @zwplug: 출력할 plug.
 * @m: seq_file 출력 대상(debugfs read 시스템 콜의 버퍼).
 * @return: 없음(void).
 *
 * zwplug->lock을 짧게 잡고 필요한 필드들을 로컬 변수로 스냅샷한 다음
 * (락을 오래 들고 seq_printf처럼 블로킹 가능한 호출을 하지 않기 위한
 * 표준 패턴), 락 해제 후 사람이 읽기 좋은 한 줄로 포맷해 출력한다.
 * 실행 컨텍스트: debugfs 파일 read 시스템 콜 처리 중인 프로세스 컨텍스트.
 * 호출자: queue_zone_wplugs_show()가 해시의 모든 plug에 대해 반복 호출.
 * 피호출자: refcount_read(), bio_list_size(), blk_zone_cond_str(),
 * seq_printf().
 *
 * 호출 체인:
 *   queue_zone_wplugs_show() → [queue_zone_wplug_show] →
 *   blk_zone_cond_str(), seq_printf()
 */
static void queue_zone_wplug_show(struct blk_zone_wplug *zwplug,
				  struct seq_file *m)
{
	unsigned int zwp_wp_offset, zwp_flags;
	unsigned int zwp_zone_no, zwp_ref;
	unsigned int zwp_bio_list_size;
	enum blk_zone_cond zwp_cond;
	unsigned long flags;

	spin_lock_irqsave(&zwplug->lock, flags); // 아래 필드들을 일관된 스냅샷으로 읽기 위해 잠금
	zwp_zone_no = zwplug->zone_no; // debugfs 출력용 ZID(zone 번호) 스냅샷
	zwp_flags = zwplug->flags; // debugfs 출력용 plug flags(PLUGGED/NEED_WP_UPDATE/DEAD) 스냅샷
	zwp_ref = refcount_read(&zwplug->ref); // debugfs 출력용 reference count 스냅샷
	zwp_cond = zwplug->cond; // debugfs 출력용 zone condition 스냅샷
	zwp_wp_offset = zwplug->wp_offset; // debugfs 출력용 WP offset 스냅샷
	zwp_bio_list_size = bio_list_size(&zwplug->bio_list); // debugfs 출력용 대기 중인 BIO 개수 스냅샷
	spin_unlock_irqrestore(&zwplug->lock, flags); // 스냅샷 완료 후 즉시 잠금 해제 - seq_printf는 락 밖에서 수행

	seq_printf(m,
		"Zone no: %u, flags: 0x%x, ref: %u, cond: %s, wp ofst: %u, pending BIO: %u\n",
		zwp_zone_no, zwp_flags, zwp_ref, blk_zone_cond_str(zwp_cond),
		zwp_wp_offset, zwp_bio_list_size);
}

/*
 * [한국어]
 * queue_zone_wplugs_show - 이 큐(디스크)의 모든 zone write plug를 debugfs로 출력
 *
 * @data: struct request_queue* 포인터(debugfs 인프라가 등록 시 전달).
 * @m: seq_file 출력 대상.
 * @return: 항상 0(seq_file show 콜백 시그니처 — 실패 없음).
 *
 * debugfs의 blk-mq-debugfs 인프라가 특정 파일을 read할 때 호출하는
 * show 콜백. plug 해시의 모든 버킷을 RCU read-side로 순회하며 각 plug를
 * queue_zone_wplug_show()로 한 줄씩 출력한다 — 활성 plug가 하나도 없는
 * (해시 자체가 없는) zoned 아닌 디스크나 아직 초기화 전인 디스크는 즉시
 * 빈 결과를 반환.
 * 실행 컨텍스트: debugfs 파일 read 시스템 콜 처리 중인 프로세스 컨텍스트 —
 * RCU read lock으로 해시를 lock-free 순회.
 * 호출자: blk-mq-debugfs 인프라(디렉토리 엔트리 등록을 통해 파일 read
 * 시 자동 호출).
 * 피호출자: disk_zone_wplugs_hash_size(), queue_zone_wplug_show().
 *
 * 호출 체인:
 *   (debugfs read syscall) → blk-mq-debugfs 인프라 →
 *   [queue_zone_wplugs_show] → queue_zone_wplug_show()
 */
int queue_zone_wplugs_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct gendisk *disk = q->disk;
	struct blk_zone_wplug *zwplug;
	unsigned int i;

	if (!disk->zone_wplugs_hash) // plug hash가 없으면 debugfs에 아무것도 출력하지 않음
		return 0;

	rcu_read_lock(); // RCU read-side로 plug hash 순회
	for (i = 0; i < disk_zone_wplugs_hash_size(disk); i++) // 모든 hash bucket 순회
		hlist_for_each_entry_rcu(zwplug, &disk->zone_wplugs_hash[i], // 각 bucket의 충돌 체인 순회
					 node)
			queue_zone_wplug_show(zwplug, m); // debugfs에 plug 상태 출력
	rcu_read_unlock();

	return 0;
}

#endif

/*
 * NVMe 관점 핵심 요약
 * ===================
 * - 이 파일은 NVMe ZNS SSD의 zone state(WP, open/closed/full)를 커널이
 *   캐시/에뮬레이션하여 잘못된 쓰기가 SQ/CID를 소비하기 전에 차단한다.
 * - write/zone-append BIO는 blk_zone_plug_bio -> blk_zone_wplug_handle_write
 *   를 거쳐 WP 정렬을 검증한 뒤 blk_mq_submit_bio -> nvme_queue_rq 로
 *   NVMe doorbell/SQ/CID 에 도달한다.
 * - zone reset/open/close/finish 명령은 NVMe Zone Management Send에 대응하
 *   며, 완료 후 plug WP와 zones_cond를 동기화해 플래시 상태와 일치시킨다.
 * - block/blk-mq.c의 request 할당/완료 경로와 긴밀히 연결되며,
 *   drivers/nvme/host/zns.c는 이 파일이 준수시킨 REQ_OP_ZONE_* 규칙을
 *   받아 실제 PRP/SGL 및 CID 할당을 수행한다.
 */
