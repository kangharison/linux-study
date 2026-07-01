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
 * =============================================================================
 * NVMe SSD 관점 파일 개요
 * =============================================================================
 * 이 파일은 zoned block device(ZBD)를 위한 블록 계층의 SW 층 역할을 한다.
 * NVMe ZNS(Zoned Namespace) 컨트롤러가 수행하는 zone 관리(reset/open/close/finish)
 * 및 write pointer(WP) 추적을 커널이 대신/보조 수행한다.
 * 사용자의 write/zone-append BIO는 blk_mq_submit_bio -> blk_zone_plug_bio ->
 * blk_zone_wplug_handle_write 경로에서 plug/hash/순차 쓰기 제어를 거쳐
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 형태로 실제 SQ/CID 에 매핑된다.
 * block/blk-mq.c, block/blk-mq.h, block/blk.h 와 긴밀히 연결되며,
 * NVMe ZNS 드라이버(drivers/nvme/host/zns.c)는 이 파일이 세운 WP 규칙을
 * 준수하는 REQ_OP_ZONE_* 명령을 받게 된다.
 */

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/spinlock.h>
#include <linux/refcount.h>
#include <linux/mempool.h>
#include <linux/kthread.h>
#include <linux/freezer.h>

#include <trace/events/block.h>

#include "blk.h"
#include "blk-mq-sched.h"
#include "blk-mq-debugfs.h"

#define ZONE_COND_NAME(name) [BLK_ZONE_COND_##name] = #name
static const char *const zone_cond_name[] = {
	ZONE_COND_NAME(NOT_WP),
	ZONE_COND_NAME(EMPTY),
	ZONE_COND_NAME(IMP_OPEN),
	ZONE_COND_NAME(EXP_OPEN),
	ZONE_COND_NAME(CLOSED),
	ZONE_COND_NAME(READONLY),
	ZONE_COND_NAME(FULL),
	ZONE_COND_NAME(OFFLINE),
	ZONE_COND_NAME(ACTIVE),
};
#undef ZONE_COND_NAME

/*
 * NVMe ZNS 관점에서 struct blk_zone_wplug 는 하나의 zone 단위로
 * write pointer(WP)와 zone state를 SW 층에서 캐시/직렬화하는 객체다.
 * NVMe 컨트롤러는 Zone Identifier(ZID)별로 WP를 유지하지만, 이 구조체는
 * 커널 내에서 동일 zone에 대한 다중 BIO 흐름을 순차 쓰기 규칙에 맞게
 * 정렬하고, zone append 에뮬레이션 시 WP 위치를 결정한다.
 *
 * 필드별 NVMe 연결:
 *   node/entry: zone plug 해시/작업 리스트 연결. ZNS namespace 내
 *               수많은 zone 중 대상 zone을 O(1)~O(bucket)로 찾는다.
 *   bio_list:   아직 NVMe SQ에 넣지 않고 WP 순서를 기다리는 BIO들.
 *   bio_work:   plug 풀릴 때 다음 BIO를 깨워 submit_bio/bdev->submit_bio
 *               -> blk_mq_submit_bio -> nvme_queue_rq 로 보낸다.
 *   lock:       zone 단위 직렬화. NVMe ZNS도 zone 단위 직렬이지만
 *               host측에서 먼저 실패시켜 불필요한 CID/SQ 소비를 막는다.
 *   ref:        zone plug 생명주기. zone이 full/empty/dead가 될 때
 *               마지막 참조가 사라지면 해제된다.
 *   zone_no:    NVMe ZNS의 Zone Identifier(ZID)에 대응.
 *   wp_offset:  zone 시작부터 현재 WP까지 512B sector 수. ZNS의
 *               Zone Descriptor wp 필드를 SW에서 미러링한다.
 *   cond:       zone condition (empty/open/closed/full 등). ZNS Report
 *               Zones 응답의 Zone State를 캐시한다.
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
	struct hlist_node	node; // NVMe ZNS Zone Identifier(ZID) 기반 hash chain node; RCU read-side 탐색용
	struct list_head	entry; // active plug list node; qd1 worker가 순차적으로 zone을 처리할 때 사용
	struct bio_list		bio_list; // 해당 zone에 대해 NVMe SQ 진입을 대기하는 BIO list (WP 순서 유지)
	struct work_struct	bio_work; // plug 풀릴 때 workqueue에서 BIO를 blk_mq_submit_bio -> nvme_queue_rq로 전달
	struct rcu_head		rcu_head; // RCU grace period 후 mempool 반환; hash 탐색과 해제 병행
	struct gendisk		*disk; // request_queue/NVMe namespace gendisk 역참조
	spinlock_t		lock; // zone 단위 직렬화; 동일 ZID의 병렬 doorbell 방지
	refcount_t		ref; // 생명주기 reference; hash + 활성 BIO/request (NVMe request ref 유사)
	unsigned int		flags; // PLUGGED/NEED_WP_UPDATE/DEAD 상태 bit; NVMe queue state 전이 대응
	unsigned int		zone_no; // NVMe ZNS Zone Identifier(ZID)
	unsigned int		wp_offset; // ZNS Zone Descriptor의 write pointer를 512B sector 단위로 미러링
	enum blk_zone_cond	cond; // cached ZNS zone condition (empty/imp_open/exp_open/closed/full)
};

/*
 * disk_need_zone_resources:
 *   NVMe ZNS namespace는 보통 blk-mq 기반 요청 기반(request-based)이므로
 *   zone resource(plug/hash)가 필요하다. BIO 기반 디바이스(DM 등)는
 *   자체적으로 순차 쓰기를 관리하므로 plug를 사용하지 않는다.
 *   (단, zone append 에뮬레이션이 필요하면 예외)
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
 * blk_zone_cond_str:
 *   BLK_ZONE_COND_* 값을 문자열로 변환하여 ZNS zone state 디버깅/트레이싱에
 *   사용한다. (예: EMPTY, IMP_OPEN, EXP_OPEN, CLOSED, FULL 등)
 */
const char *blk_zone_cond_str(enum blk_zone_cond zone_cond)
{
	static const char *zone_cond_str = "UNKNOWN"; // 기본 unknown 문자열; ZNS state 값 범위 검증 후 덮어씀

	if (zone_cond < ARRAY_SIZE(zone_cond_name) && zone_cond_name[zone_cond]) // ZNS Zone State 값의 범위 및 NULL 검증
		zone_cond_str = zone_cond_name[zone_cond]; // dmesg/trace에서 ZNS zone condition 문자열로 변환

	return zone_cond_str;
}
EXPORT_SYMBOL_GPL(blk_zone_cond_str);

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
 * bdev_zone_is_seq:
 *   대상 sector가 sequential write required zone에 속하는지 확인.
 *   NVMe ZNS에서 conventional zone이 아닌 zone은 모두 sequential로
 *   간주되며, 임의 위치 쓰기를 허용하지 않는다.
 *   호출 경로: blk_zone_wplug_handle_write -> bdev_zone_is_seq
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
	report_zones_cb cb; // zone descriptor 하나마다 호출되는 콜백; NVMe Report Zones CQ 데이터 소비
	void		*data; // 사용자 콜백 private 데이터
	bool		report_active; // cached report fallback 시 open/closed를 active로 단순화
};

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
 * blkdev_report_zones:
 *   사용자나 상위 계층이 NVMe ZNS Report Zones command를 호출하는 진입점.
 *   disk->fops->report_zones (nvme_report_zones)를 호출해 device로부터
 *   zone descriptor list를 받아 콜백으로 전달한다.
 *   호출 경로: blkdev_report_zones_ioctl -> blkdev_report_zones
 *          -> disk->fops->report_zones -> nvme_report_zones -> SQ/CID
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
 * blkdev_zone_mgmt:
 *   NVMe ZNS Zone Management Send command를 여러 zone에 대해 수행.
 *   REQ_OP_ZONE_RESET/OPEN/CLOSE/FINISH를 각 zone 별 BIO로 만들어
 *   submit_bio_wait로 제출한다. 전체 디스크 reset은 RESET_ALL 최적화 사용.
 *   호출 경로: blkdev_zone_mgmt_ioctl -> blkdev_zone_mgmt
 *          -> submit_bio_wait -> blk_mq_submit_bio -> nvme_queue_rq
 *          -> nvme_submit_cmd(doorbell, SQ, CID)
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

struct zone_report_args {
	struct blk_zone __user *zones;
};

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

/*
 * BLKREPORTZONE and BLKREPORTZONEV2 ioctl processing.
 * Called from blkdev_ioctl.
 */
/*
 * blkdev_report_zones_ioctl:
 *   사용자가 BLKREPORTZONE/BLKREPORTZONEV2 ioctl로 NVMe ZNS Report Zones
 *   정보를 요청할 때 처리. cached 버전은 커널이 캐시한 zone condition을
 *   사용해 불필요한 NVMe command 왕복을 줄인다.
 *   호출 경로: blkdev_ioctl -> blkdev_report_zones_ioctl
 *          -> blkdev_report_zones[_cached] -> disk->fops->report_zones
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

static int blkdev_reset_zone(struct block_device *bdev, blk_mode_t mode,
			     struct blk_zone_range *zrange)
{
	loff_t start, end;
	int ret = -EINVAL;

	inode_lock(bdev->bd_mapping->host); // 파일시스템 쓰기와 reset 동작 직렬화
	filemap_invalidate_lock(bdev->bd_mapping); // reset 구간의 page cache를 무효화
	if (zrange->sector + zrange->nr_sectors <= zrange->sector ||
	    zrange->sector + zrange->nr_sectors > get_capacity(bdev->bd_disk))
		/* Out of range */
		goto out_unlock;

	start = zrange->sector << SECTOR_SHIFT;
	end = ((zrange->sector + zrange->nr_sectors) << SECTOR_SHIFT) - 1;

	ret = truncate_bdev_range(bdev, mode, start, end); // reset할 구간의 캐시된 데이터 truncate
	if (ret)
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
 * blkdev_zone_mgmt_ioctl:
 *   BLKRESETZONE/BLKOPENZONE/BLKCLOSEZONE/BLKFINISHZONE ioctl 처리.
 *   각 명령은 NVMe ZNS Zone Management Send의 Reset/Open/Close/Finish
 *   하위 명령에 대응한다.
 *   호출 경로: blkdev_ioctl -> blkdev_zone_mgmt_ioctl -> blkdev_zone_mgmt
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

static bool disk_zone_is_last(struct gendisk *disk, struct blk_zone *zone)
{
	return zone->start + zone->len >= get_capacity(disk); // 마지막 ZNS zone 판별; last zone capacity 적용에 사용
}

static bool disk_zone_wplug_is_full(struct gendisk *disk,
				    struct blk_zone_wplug *zwplug)
{
	if (zwplug->zone_no < disk->nr_zones - 1) // 마지막 zone이 아니면 일반 zone_capacity로 full 판정
		return zwplug->wp_offset >= disk->zone_capacity; // WP가 zone capacity에 도달하면 ZNS FULL 상태
	return zwplug->wp_offset >= disk->last_zone_capacity; // 마지막 zone은 축소된 capacity로 full 판정
}

/*
 * disk_insert_zone_wplug:
 *   새로 할당한 zone plug를 hash table에 삽입. 이미 동일 zone plug가
 *   있으면 삽입 실패. zone condition은 zones_cond 배열에서 가져오거나,
 *   재검증 초기에는 ACTIVE로 간주한다.
 *   NVMe 연결: ZNS namespace가 probe될 때 각 zone의 초기 state를 반영.
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

static inline struct blk_zone_wplug *disk_get_zone_wplug(struct gendisk *disk,
							 sector_t sector)
{
	if (!atomic_read(&disk->nr_zone_wplugs)) // 활성 plug가 없으면 lookup 생략
		return NULL;

	return disk_get_hashed_zone_wplug(disk, sector); // hash table에서 ZID에 해당하는 plug 검색
}

static void disk_free_zone_wplug_rcu(struct rcu_head *rcu_head)
{
	struct blk_zone_wplug *zwplug =
		container_of(rcu_head, struct blk_zone_wplug, rcu_head);

	mempool_free(zwplug, zwplug->disk->zone_wplugs_pool); // RCU grace period 종료 후 plug를 bounded mempool에 반환
}

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
 * disk_get_or_alloc_zone_wplug:
 *   대상 zone에 대한 plug를 검색하거나 새로 할당.
 *   ref를 2로 설정해 idle 시에도 유지되도록 하며, zone이 full/empty/reset/
 *   finish되면 DEAD 플래그와 함께 초기 ref를 해제한다.
 *   NVMe 연결: zone write/append BIO가 처음 도착하면 해당 ZNS zone에
 *   대응하는 plug가 생성되며, 이후 WP 추적이 시작된다.
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
 * disk_zone_wplug_set_wp_offset:
 *   zone reset/finish/report 후 write pointer offset을 강제로 갱신.
 *   기존에 plug된 BIO들은 모두 abort되며, WP가 0이나 zone 크기이면
 *   plug를 dead로 표시하여 이후 write가 실패하도록 한다.
 *   NVMe 연결: ZNS Zone Management Send(reset/finish) 완료 후 커널 WP
 *   캐시를 동기화해야 실제 플래시 상태와 불일치를 방지한다.
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
 * disk_zone_wplug_sync_wp_offset:
 *   Report Zones로 얻은 zone descriptor의 WP를 해당 zone plug에 반영.
 *   NEED_WP_UPDATE 플래그가 있을 때만 갱신한다.
 *   호출 경로: disk_report_zone -> disk_zone_wplug_sync_wp_offset
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
 * disk_report_zone:
 *   NVMe ZNS Report Zones 완료 후 각 zone descriptor를 상위로 전달하기 전
 *   커널 캐시(zones_cond, zone plug WP)를 동기화.
 *   report_active=true 이면 open/closed 상태를 active로 축소.
 *   호출 경로: disk->fops->report_zones 콜백 -> disk_report_zone
 *          -> disk_zone_wplug_sync_wp_offset, args->cb
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

static int blkdev_report_zone_cb(struct blk_zone *zone, unsigned int idx,
				 void *data)
{
	memcpy(data, zone, sizeof(struct blk_zone));
	return 0;
}

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
 * blkdev_has_cached_report_zones:
 *   native zone append를 지원하면 plug가 WP 추적에 참여하지 않으므로
 *   cached report가 유효하지 않을 수 있다. zone append 에뮬레이션을
 *   사용하거나 아직 zone append를 한 번도 사용하지 않은 경우에만 캐시 사용.
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
 * blkdev_get_zone_info:
 *   단일 zone 정보를 커널 캐시(zones_cond + zone plug)에서 조회.
 *   캐시를 신뢰할 수 없으면 NVMe Report Zones를 fallback으로 실행.
 *   반환된 blk_zone은 사용자 공간 ioctl 결과로 전달됨.
 *   호출 경로: blkdev_report_zones_cached -> blkdev_get_zone_info
 *          -> (fallback) blkdev_do_report_zones -> nvme_report_zones
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
 * blkdev_report_zones_cached:
 *   디바이스에 NVMe Report Zones command를 내리지 않고 커널 캐시만으로
 *   zone report를 생성. cached를 사용할 수 없으면 disk->fops->report_zones
 *   (nvme_report_zones)로 fallback.
 *   호출 경로: BLKREPORTZONEV2 ioctl -> blkdev_report_zones_cached
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
		if (ret)
			return ret;

		ret = cb(&zone, idx, data); // 조회된 ZNS zone descriptor를 사용자 콜백으로 전달
		if (ret)
			return ret;
	}

	return idx;
}
EXPORT_SYMBOL_GPL(blkdev_report_zones_cached);

/*
 * blk_zone_reset_bio_endio:
 *   NVMe ZNS Reset 명령 완료 후 해당 zone plug의 WP를 0으로 되돌리거나,
 *   plug가 없으면 zones_cond를 EMPTY로 갱신.
 *   호출 경로: blk_zone_mgmt_bio_endio -> blk_zone_reset_bio_endio
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
 * blk_zone_reset_all_bio_endio:
 *   NVMe ZNS Reset All 완료 후 모든 zone plug를 제거/초기화하고
 *   zones_cond를 EMPTY로, GD_ZONE_APPEND_USED 플래그를 클리어.
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
 * blk_zone_finish_bio_endio:
 *   NVMe ZNS Finish 명령 완료 후 해당 zone plug WP를 zone 크기로 설정하여
 *   FULL로 만들고, plug가 없으면 zones_cond를 FULL로 갱신.
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
 * blk_zone_mgmt_bio_endio:
 *   NVMe ZNS Zone Management Send 명령(BIO) 완료 시 커널 캐시를 갱신.
 *   명령 실패 시에는 아무것도 하지 않는다. reset/open/close/finish 별로
 *   plug 상태나 zones_cond를 갱신한다.
 *   호출 경로: nvme_complete_rq -> bio_endio -> blk_zone_mgmt_bio_endio
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
 * disk_zone_wplug_schedule_work:
 *   plug된 다음 BIO를 workqueue로 스케줄. work 항목은 plug 자체에
 *   속하므로 ref를 증가시켜 work 실행 전 해제를 방지.
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
		// zone append 에뮬레이션: WP 기반 일반 write로 변환
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
 * disk_zone_wplug_add_bio:
 *   BIO를 plug list에 추가. q_usage_counter를 증가시켜 제출 시
 *   blk-mq가 queue 사용을 재활용하고, BIO-based 드라이버는 완료 후
 *   blk_queue_exit로 해제한다. REQ_NOWAIT BIO는 nowait 플래그를 제거.
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
 * blk_zone_write_plug_bio_merged:
 *   bio_attempt_back_merge로 BIO가 기존 request에 병합된 경우
 *   zone plug의 wp_offset을 해당 BIO 크기만큼 전진. 이미 plug된 BIO
 *   라면 초기화 경로에서 처리되므로 여기서는 무시.
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
 * blk_zone_write_plug_init_request:
 *   새 request 준비 시 plug list의 BIO를 back merge로 끌어들여 NVMe SQ에
 *   넣을 request를 최대한 채운다. 이는 SQ entry(CID) 사용 효율을 높이고
 *   zone 내 연속 쓰기를 단일 I/O로 묶는다.
 *   호출 경로: blk_mq_make_request -> blk_zone_write_plug_init_request
 *          -> bio_attempt_back_merge -> blk_zone_write_plug_bio_merged
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
 * blk_zone_wplug_prepare_bio:
 *   plug를 풀어 실제 제출할 BIO의 위치와 동작을 확정.
 *   NEED_WP_UPDATE나 full zone이면 false를 반환해 BIO 실패.
 *   zone append 에뮬레이션 시 현재 WP에서 REQ_OP_WRITE + REQ_NOMERGE로
 *   변환하며, 일반 write는 반드시 WP에 정렬되어 있어야 한다.
 *   호출 경로: blk_zone_wplug_handle_write, disk_zone_wplug_submit_bio
 *          -> blk_zone_wplug_prepare_bio
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
 * blk_zone_wplug_handle_write:
 *   write/zone-append/write-zeroes BIO에 대한 zone plug 핵심 처리.
 *   conventional zone은 통과시키고, sequential zone은 plug 할당/검색 후
 *   WP 규칙에 따라 즉시 제출하거나 plug list에 대기시킨다.
 *   NVMe 연결: NVMe SQ에 들어가기 전 host 측에서 zone 순차 쓰기와
 *   open/closed/full 상태를 강제하며, 잘못된 쓰기는 doorbell 전달 전
 *   BLK_STS_IOERR로 빠르게 실패시킨다.
 *   호출 경로: blk_zone_plug_bio -> blk_zone_wplug_handle_write
 *          -> (제출) blk_mq_submit_bio -> nvme_queue_rq
 *          -> nvme_submit_cmd(doorbell, SQ, CID)
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
 * blk_zone_wplug_handle_native_zone_append:
 *   NVMe ZNS가 native zone append를 지원하면 plug를 사용하지 않고
 *   BIO를 직접 NVMe SQ로 보낸다. 만약 동일 zone에 이전 일반 쓰기로
 *   생성된 plug가 남아 있으면 제거하여 메모리 누수를 막는다.
 *   (추정): zone append와 일반 write를 섞으면 ZNS 컨트롤러의 WP
 *   순서 보장이 없어 plug된 일반 write는 abort 처리된다.
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
 * blk_zone_wplug_handle_zone_mgmt:
 *   zone reset/finish/reset_all BIO에 대해 conventional zone이 아닌지
 *   확인하고 REQ_NOWAIT 플래그를 무시. 실제 NVMe Zone Management Send
 *   명령은 제출 경로에서 처리되며, 이 함수는 plug와 무관하다.
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
 * blk_zone_plug_bio:
 *   zone-aware BIO 제출의 총진입점. write/zone append/write zeroes는
 *   plug를 거치고, zone management 명령은 특수 처리 후 제출한다.
 *   NVMe 연결: 이 함수를 통과한 BIO만이 이후 blk_mq_submit_bio ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)로 NVMe SQ/CID에 할당됨.
 *   호출 경로: submit_bio_noacct -> blk_zone_plug_bio
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
 * blk_zone_write_plug_bio_endio:
 *   plug를 통해 제출된 BIO 완료 시 호출. zone append 에뮬레이션 BIO의
 *   op 코드를 복원하고, 실패 시 plug된 나머지 BIO를 abort하고
 *   NEED_WP_UPDATE를 설정해 다음 Report Zones로 WP를 복구하도록 한다.
 *   호출 경로: bio_endio -> blk_zone_write_plug_bio_endio
 *          -> disk_zone_wplug_unplug_bio -> disk_zone_wplug_schedule_work
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
 * blk_zone_write_plug_finish_request:
 *   request 기반(blk-mq) 장치에서 request 완료 시 호출. request에
 *   연결된 zone plug를 찾아 ref를 해제하고 다음 plug된 BIO를 깨운다.
 *   호출 경로: nvme_complete_rq -> blk_mq_end_request ->
 *          blk_zone_write_plug_finish_request
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
 * disk_zone_wplug_submit_bio:
 *   plug list에서 다음 BIO를 꺼내 WP 규칙 검증 후 제출.
 *   blk-mq 장치는 q_usage_counter 추가 참조를 재사용하고, BIO-based
 *   장치는 submit_bio 후 blk_queue_exit로 해제한다.
 *   호출 경로: disk_zone_wplugs_worker, blk_zone_wplug_bio_work
 *          -> disk_zone_wplug_submit_bio
 *          -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_submit_cmd(...)
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
 * disk_zone_wplugs_worker:
 *   회전형(qd1 writes) 장치용 단일 커널 스레드. active plug list를
 *   순회하며 각 zone의 plug list를 소진할 때까지 BIO를 제출하고, 각 BIO
 *   완료를 기다린다. 이는 NVMe ZNS와 달리 순차적으로 zone을 처리해야
 *   하는 SMR HDD 등을 위한 경로다.
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
#define BLK_ZONE_WPLUG_DEFAULT_POOL_SIZE	128

/*
 * disk_alloc_zone_resources:
 *   zoned 디스크를 위한 zone plug 해시 테이블, mempool, workqueue,
 *   커널 스레드 할당. pool 크기는 max_open_zones/max_active_zones에
 *   기반하며, NVMe ZNS namespace의 zone 리소스 한도를 반영한다.
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
	if (!disk->zone_wplugs_hash)
		return -ENOMEM;

	for (i = 0; i < disk_zone_wplugs_hash_size(disk); i++) // 모든 hash bucket head 초기화
		INIT_HLIST_HEAD(&disk->zone_wplugs_hash[i]);

	disk->zone_wplugs_pool = mempool_create_kmalloc_pool(pool_size,
						sizeof(struct blk_zone_wplug)); // max_open/active_zones 기반 plug mempool 생성
	if (!disk->zone_wplugs_pool)
		goto free_hash;

	disk->zone_wplugs_wq =
		alloc_workqueue("%s_zwplugs", WQ_MEM_RECLAIM | WQ_HIGHPRI,
				pool_size, disk->disk_name); // per-disk 고우선순위 workqueue 할당
	if (!disk->zone_wplugs_wq)
		goto destroy_pool;

	disk->zone_wplugs_worker =
		kthread_create(disk_zone_wplugs_worker, disk,
			       "%s_zwplugs_worker", disk->disk_name); // 회전형(qd1 writes) 장치용 커널 스레드 생성
	if (IS_ERR(disk->zone_wplugs_worker)) {
		ret = PTR_ERR(disk->zone_wplugs_worker);
		disk->zone_wplugs_worker = NULL;
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
 * disk_free_zone_resources:
 *   zone plug 자원을 모두 해제. 모든 plug를 dead로 표시하고 RCU grace
 *   period 후 mempool을 파괴. 디스크 제거나 재검증 실패 시 호출.
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

struct blk_revalidate_zone_args {
	struct gendisk	*disk;
	u8		*zones_cond;
	unsigned int	nr_zones;
	unsigned int	nr_conv_zones;
	unsigned int	zone_capacity;
	unsigned int	last_zone_capacity;
	sector_t	sector;
};

/*
 * struct blk_revalidate_zone_args:
 *   zoned 디스크 재검증 시 Report Zones 결과를 수집하는 임시 구조체.
 *   zones_cond:   각 zone의 condition 캐시. ZNS Report Zones의
 *                 Zone State 배열.
 *   nr_zones:     ZNS namespace 총 zone 수 (capacity / zone size).
 *   nr_conv_zones: conventional zone 개수.
 *   zone_capacity/last_zone_capacity: ZNS zone의 가용 capacity(ZCAP).
 *   sector:       재검증 중 검증할 다음 sector.
 *
 * disk_revalidate_zone_resources:
 *   위 구조체를 초기화하고 필요하면 zone plug 자원을 할당.
 *   NVMe ZNS namespace 포맷/리사이즈 후 호출.
 */
static int disk_revalidate_zone_resources(struct gendisk *disk,
				struct blk_revalidate_zone_args *args)
{
	struct queue_limits *lim = &disk->queue->limits; // queue limits (chunk_sectors, max_open/active_zones)
	unsigned int pool_size;
	int ret = 0; // 재검증 결과 코드

	args->disk = disk;
	args->nr_zones =
		DIV_ROUND_UP_ULL(get_capacity(disk), lim->chunk_sectors); // capacity / chunk_sectors로 ZNS 총 zone 수 계산
	// ZNS namespace 총 zone 수 = capacity / chunk_sectors

	/* Cached zone conditions: 1 byte per zone */
	args->zones_cond = kzalloc(args->nr_zones, GFP_NOIO); // zone마다 1바이트 condition cache 할당 (ZNS zone state 배열)
	if (!args->zones_cond)
		return -ENOMEM;

	if (!disk_need_zone_resources(disk))
		return 0;

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
 * disk_update_zone_resources:
 *   report_zones 결과를 반영해 disk의 nr_zones, zone_capacity,
 *   zones_cond, queue limits를 갱신. max_open/active_zones가 sequential
 *   zone 수 이상이면 무제한으로 간주.
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
 * blk_revalidate_seq_zone:
 *   sequential zone의 capacity 일관성을 검증하고, zone append 에뮬레이션
 *   필요 시 WP가 중간에 있는 zone에 plug를 생성.
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
 * blk_revalidate_disk_zones:
 *   zoned 디스크 probe/재검증 시 전체 zone layout을 검증하고 자원을
 *   할당. disk->fops->report_zones (nvme_report_zones)로 모든 zone의
 *   descriptor를 받아 zones_cond와 plug WP를 초기화.
 *   NVMe 연결: NVMe ZNS namespace가 등록되면 이 함수가 ZNS의
 *   Report Zones 결과를 기반으로 커널 내 zone 자료구조를 구축.
 *   호출 경로: nvme_revalidate_disk -> blk_revalidate_disk_zones
 *          -> disk->fops->report_zones -> nvme_report_zones -> SQ/CID
 */
int blk_revalidate_disk_zones(struct gendisk *disk)
{
	struct request_queue *q = disk->queue; // gendisk의 request_queue
	sector_t zone_sectors = q->limits.chunk_sectors; // ZNS zone size
	sector_t capacity = get_capacity(disk); // NVMe namespace capacity
	struct blk_revalidate_zone_args args = { };
	unsigned int memflags, noio_flag;
	struct blk_report_zones_args rep_args = {
		.cb = blk_revalidate_zone_cb,
		.data = &args,
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
 * blk_zone_issue_zeroout:
 *   zone 내 특정 범위를 0으로 채운다. hardware offload가 없으면
 *   zero-page write로 fallback. zeroout 실패로 WP가 어긋났을 때는
 *   report_zones를 호출해 WP를 재동기화.
 *   호출 경로: blkdev_issue_zeroout -> blk_zone_issue_zeroout
 *          -> (fallback) disk->fops->report_zones
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
static void queue_zone_wplug_show(struct blk_zone_wplug *zwplug,
				  struct seq_file *m)
{
	unsigned int zwp_wp_offset, zwp_flags;
	unsigned int zwp_zone_no, zwp_ref;
	unsigned int zwp_bio_list_size;
	enum blk_zone_cond zwp_cond;
	unsigned long flags;

	spin_lock_irqsave(&zwplug->lock, flags);
	zwp_zone_no = zwplug->zone_no; // plug lock 획득 후 필드 읽기
	zwp_flags = zwplug->flags; // debugfs 출력용 ZID
	zwp_ref = refcount_read(&zwplug->ref); // debugfs 출력용 plug flags
	zwp_cond = zwplug->cond; // debugfs 출력용 reference count
	zwp_wp_offset = zwplug->wp_offset; // debugfs 출력용 zone condition
	zwp_bio_list_size = bio_list_size(&zwplug->bio_list); // debugfs 출력용 WP offset
	spin_unlock_irqrestore(&zwplug->lock, flags); // debugfs 출력용 pending BIO 개수

	seq_printf(m,
		"Zone no: %u, flags: 0x%x, ref: %u, cond: %s, wp ofst: %u, pending BIO: %u\n",
		zwp_zone_no, zwp_flags, zwp_ref, blk_zone_cond_str(zwp_cond),
		zwp_wp_offset, zwp_bio_list_size);
}

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
