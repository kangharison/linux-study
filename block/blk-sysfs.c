// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to sysfs handling
 */
/*
 * 파일 상단 요약 (NVMe SSD 관점)
 *
 * block/blk-sysfs.c는 request_queue의 sysfs/debugfs 등록과 속성 접근을 담당한다.
 * NVMe SSD에서는 /sys/block/nvme*/queue/*를 통해 nr_requests, max_sectors_kb,
 * io_timeout, scheduler, write_cache 등을 노출/제어한다.
 * 이 파일은 block/blk-mq.c, block/elevator.c 등에서 초기화된 request_queue와
 * blk_mq_tag_set 위에 사용자공간 인터페이스를 얹는 지점이며, I/O 경로
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 * -> nvme_submit_cmd(doorbell) 의 큐 특성을 런타임에 조율한다.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/blktrace_api.h>
#include <linux/debugfs.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-sched.h"
#include "blk-rq-qos.h"
#include "blk-wbt.h"
#include "blk-cgroup.h"
#include "blk-throttle.h"

/*
 * struct queue_sysfs_entry: /sys/block/<disk>/queue/<attr>에 대응되는
 * sysfs attribute 캡슐화 구조체. NVMe에서 이 구조체는 큐 특성을
 * 사용자공간에 노출하는 창구이며, 각 필드는 다음과 같은 NVMe 동작과 연결된다.
 *
 * @attr: attribute 이름/권한. e.g. "nr_requests", "io_timeout".
 * @show: 현재 큐 상태를 읽어온다. NVMe 관점에서는 SQ/CQ 깊이,
 *        write cache 상태, scheduler 설정 등을 반환.
 * @show_limit: queue_limits 기반 읽기. NVMe Identify(CNS 00h/05h)로
 *              결정된 capacity/limit 값을 sysfs로 노출할 때 사용 (추정).
 * @store: 큐 파라미터 쓰기. e.g. nr_requests 변경 시 blk_mq tag pool
 *         크기가 변하고, 이는 nvme_queue_rq에서 사용 가능한 request 수를
 *         제한한다.
 * @store_limit: queue_limits 갱신. NVMe에서 max_sectors_kb 등을
 *               재설정하면 PRP/SGL 준비에 영향을 준다 (추정).
 */
struct queue_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct gendisk *disk, char *page);
	ssize_t (*show_limit)(struct gendisk *disk, char *page);

	ssize_t (*store)(struct gendisk *disk, const char *page, size_t count);
	int (*store_limit)(struct gendisk *disk, const char *page,
			size_t count, struct queue_limits *lim);
};

/*
 * queue_var_show: unsigned long 값을 sysfs 페이지에 출력하는 헬퍼.
 * NVMe 큐 속성(nrq, timeout 등) 수치 표시에 재사용된다.
 */
static ssize_t
queue_var_show(unsigned long var, char *page)
{
	return sysfs_emit(page, "%lu\n", var);
}

/*
 * queue_var_store: 사용자가 쓴 십진 문자열을 unsigned long로 변환.
 * NVMe queue tunable 쓰기의 공통 입력 파싱 단계.
 */
static ssize_t
queue_var_store(unsigned long *var, const char *page, size_t count)
{
	int err;
	unsigned long v;

	err = kstrtoul(page, 10, &v);
	if (err || v > UINT_MAX)
		return -EINVAL;

	*var = v;

	return count;
}

/*
 * queue_requests_show: /sys/block/<disk>/queue/nr_requests 읽기.
 * 현재 request_queue의 software queue depth를 반환한다.
 */
static ssize_t queue_requests_show(struct gendisk *disk, char *page)
{
	ssize_t ret;

	mutex_lock(&disk->queue->elevator_lock);
	ret = queue_var_show(disk->queue->nr_requests, page);
	mutex_unlock(&disk->queue->elevator_lock);
	return ret;
}

/*
 * queue_requests_store: /sys/block/nvme*/queue/nr_requests 쓰기 핸들러.
 *
 * 목적: 큐당 동시 처리 가능한 request 수(nr_requests)를 조정하여
 *       NVMe Submission Queue(SQ)에 대한 소프트웨어 큐 깊이를 조절한다.
 *
 * 주요 호출 경로:
 *   sysfs write -> queue_requests_store -> blk_mq_update_nr_requests
 *   -> blk_mq_tag_set 재조정 -> 이후 I/O 경로:
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결점:
 *   - nr_requests는 NVMe 드라이버의 queue_depth/tag_depth와 직접
 *     연관되어 SQ에 삽입할 수 있는 CID(command id) 수를 제한한다.
 *   - blk_mq_freeze_queue/unfreeze_queue 사이에서 갱신되므로 SQ
 *     doorbell race를 피하면서 큐 깊이를 바꾼다 (추정).
 */
static ssize_t
queue_requests_store(struct gendisk *disk, const char *page, size_t count)
{
	struct request_queue *q = disk->queue;
	struct blk_mq_tag_set *set = q->tag_set;
	struct elevator_tags *et = NULL;
	unsigned int memflags;
	unsigned long nr;
	int ret;

	ret = queue_var_store(&nr, page, count);
	if (ret < 0)
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
	if (!down_write_trylock(&set->update_nr_hwq_lock))
		return -EBUSY;

	if (nr == q->nr_requests) /* 기존 값과 동일하면 아무것도 하지 않는다. */
		goto unlock;

	if (nr < BLKDEV_MIN_RQ) /* NVMe에서도 최소 request 개수 보장. */
		nr = BLKDEV_MIN_RQ;

	/*
	 * Switching elevator is protected by update_nr_hwq_lock:
	 *  - read lock is held from elevator sysfs attribute;
	 *  - write lock is held from updating nr_hw_queues;
	 * Hence it's safe to access q->elevator here with write lock held.
	 */
	/*
	 * NVMe 측면: nr_requests는 tag_set->queue_depth, reserved_tags,
	 * MAX_SCHED_RQ 제약을 받는다. 즉 NVMe SQ에 매핑되는 software queue
	 * depth의 상한이며, 이를 통해 nvme_queue_rq가 할당할 수 있는
	 * request/CID 수가 조절된다 (추정).
	 */
	if (nr <= set->reserved_tags ||
	    (q->elevator && nr > MAX_SCHED_RQ) ||
	    (!q->elevator && nr > set->queue_depth)) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!blk_mq_is_shared_tags(set->flags) && q->elevator && /* 스케줄러 tag pool 확장이 필요한 경우 사전 할당; NVMe SQ depth 확장과 연결. */
	    nr > q->elevator->et->nr_requests) {
		/*
		 * Tags will grow, allocate memory before freezing queue to
		 * prevent deadlock.
		 */
		et = blk_mq_alloc_sched_tags(set, q->nr_hw_queues, nr);
		if (!et) {
			ret = -ENOMEM;
			goto unlock;
		}
	}

	memflags = blk_mq_freeze_queue(q); /* 큐 동결: NVMe SQ doorbell 경쟁 없이 tag depth 갱신 (추정). */
	mutex_lock(&q->elevator_lock);
	et = blk_mq_update_nr_requests(q, et, nr); /* nr_requests 및 tag pool 갱신. 이후 nvme_queue_rq에서 사용. */
	mutex_unlock(&q->elevator_lock);
	blk_mq_unfreeze_queue(q, memflags);

	if (et) /* 임시 sched tag set 해제. */
		blk_mq_free_sched_tags(et, set);

unlock: /* lock 해제, NVMe queue depth 변경 완료. */
	up_write(&set->update_nr_hwq_lock);
	return ret;
}

static ssize_t queue_async_depth_show(struct gendisk *disk, char *page)
{
	guard(mutex)(&disk->queue->elevator_lock);

	return queue_var_show(disk->queue->async_depth, page);
}

/*
 * queue_async_depth_store: /sys/block/<disk>/queue/async_depth 쓰기 핸들러.
 *
 * 목적: 파일시스템/RAID 등 상위 계층이 사용하는 최대 비동기 I/O 깊이를
 *       제한한다. NVMe 관점에서는 큐당 outstanding request 수에 대한
 *       추가 소프트 상한을 설정한다 (추정).
 *
 * 호출 경로:
 *   sysfs write -> queue_async_depth_store -> elevator->type->ops.depth_updated
 *   -> 이후 I/O 경로: blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd.
 */
static ssize_t
queue_async_depth_store(struct gendisk *disk, const char *page, size_t count)
{
	struct request_queue *q = disk->queue;
	unsigned int memflags;
	unsigned long nr;
	int ret;

	if (!queue_is_mq(q)) /* NVMe는 blk-mq 기반만 해당. */
		return -EINVAL;

	ret = queue_var_store(&nr, page, count);
	if (ret < 0)
		return ret;

	if (nr == 0) /* async_depth=0은 허용되지 않음. */
		return -EINVAL;

	memflags = blk_mq_freeze_queue(q); /* queue 동결 후 async_depth 갱신. */
	scoped_guard(mutex, &q->elevator_lock) {
		if (q->elevator) {
			q->async_depth = min(q->nr_requests, nr); /* 실제 큐 깊이(q->nr_requests)를 초과하지 않도록 제한. */
			if (q->elevator->type->ops.depth_updated)
				q->elevator->type->ops.depth_updated(q); /* mq-deadline 등 스케줄러 depth 콜백 호출. */
		} else {
			ret = -EINVAL;
		}
	}
	blk_mq_unfreeze_queue(q, memflags); /* 동결 해제 후 NVMe I/O 재개. */

	return ret;
}

/*
 * queue_ra_show: /sys/block/<disk>/queue/read_ahead_kb 읽기.
 * NVMe 순차 읽기 성능에 영향을 주는 readahead 크기를 반환한다.
 */
static ssize_t queue_ra_show(struct gendisk *disk, char *page)
{
	ssize_t ret;

	mutex_lock(&disk->queue->limits_lock);
	ret = queue_var_show(disk->bdi->ra_pages << (PAGE_SHIFT - 10), page);
	mutex_unlock(&disk->queue->limits_lock);

	return ret;
}

/*
 * queue_ra_store: /sys/block/<disk>/queue/read_ahead_kb 쓰기 핸들러.
 * bdi->ra_pages를 갱신한다. NVMe sequential read 시 SSD 낸드 prefetch와
 * 상호작용할 수 있다 (추정).
 */
static ssize_t
queue_ra_store(struct gendisk *disk, const char *page, size_t count)
{
	unsigned long ra_kb;
	ssize_t ret;
	struct request_queue *q = disk->queue;

	ret = queue_var_store(&ra_kb, page, count);
	if (ret < 0)
		return ret;
	/*
	 * The ->ra_pages change below is protected by ->limits_lock because it
	 * is usually calculated from the queue limits by
	 * queue_limits_commit_update().
	 *
	 * bdi->ra_pages reads are not serialized against bdi->ra_pages writes.
	 * Use WRITE_ONCE() to write bdi->ra_pages once.
	 */
	mutex_lock(&q->limits_lock); /* bdi readahead 페이지 값 보호. */
	WRITE_ONCE(disk->bdi->ra_pages, ra_kb >> (PAGE_SHIFT - 10)); /* 단위 변환: KB -> PAGE 수; NVMe 순차 읽기 폭 조정 (추정). */
	mutex_unlock(&q->limits_lock);

	return ret;
}

#define QUEUE_SYSFS_LIMIT_SHOW(_field)					\
static ssize_t queue_##_field##_show(struct gendisk *disk, char *page)	\
{									\
	return queue_var_show(disk->queue->limits._field, page);	\
}

QUEUE_SYSFS_LIMIT_SHOW(max_segments)
QUEUE_SYSFS_LIMIT_SHOW(max_discard_segments)
QUEUE_SYSFS_LIMIT_SHOW(max_integrity_segments)
QUEUE_SYSFS_LIMIT_SHOW(max_segment_size)
QUEUE_SYSFS_LIMIT_SHOW(max_write_streams)
QUEUE_SYSFS_LIMIT_SHOW(write_stream_granularity)
QUEUE_SYSFS_LIMIT_SHOW(logical_block_size)
QUEUE_SYSFS_LIMIT_SHOW(physical_block_size)
QUEUE_SYSFS_LIMIT_SHOW(chunk_sectors)
QUEUE_SYSFS_LIMIT_SHOW(io_min)
QUEUE_SYSFS_LIMIT_SHOW(io_opt)
QUEUE_SYSFS_LIMIT_SHOW(discard_granularity)
QUEUE_SYSFS_LIMIT_SHOW(zone_write_granularity)
QUEUE_SYSFS_LIMIT_SHOW(virt_boundary_mask)
QUEUE_SYSFS_LIMIT_SHOW(dma_alignment)
QUEUE_SYSFS_LIMIT_SHOW(max_open_zones)
QUEUE_SYSFS_LIMIT_SHOW(max_active_zones)
QUEUE_SYSFS_LIMIT_SHOW(atomic_write_unit_min)
QUEUE_SYSFS_LIMIT_SHOW(atomic_write_unit_max)

#define QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(_field)			\
static ssize_t queue_##_field##_show(struct gendisk *disk, char *page)	\
{									\
	return sysfs_emit(page, "%llu\n",				\
		(unsigned long long)disk->queue->limits._field <<	\
			SECTOR_SHIFT);					\
}

QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_discard_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_hw_discard_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_write_zeroes_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_hw_wzeroes_unmap_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_wzeroes_unmap_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(atomic_write_max_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(atomic_write_boundary_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_BYTES(max_zone_append_sectors)

#define QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_KB(_field)			\
static ssize_t queue_##_field##_show(struct gendisk *disk, char *page)	\
{									\
	return queue_var_show(disk->queue->limits._field >> 1, page);	\
}

QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_KB(max_sectors)
QUEUE_SYSFS_LIMIT_SHOW_SECTORS_TO_KB(max_hw_sectors)

#define QUEUE_SYSFS_SHOW_CONST(_name, _val)				\
static ssize_t queue_##_name##_show(struct gendisk *disk, char *page)	\
{									\
	return sysfs_emit(page, "%d\n", _val);				\
}

/* deprecated fields */
QUEUE_SYSFS_SHOW_CONST(discard_zeroes_data, 0)
QUEUE_SYSFS_SHOW_CONST(write_same_max, 0)
QUEUE_SYSFS_SHOW_CONST(poll_delay, -1)

/*
 * queue_max_discard_sectors_store: /sys/.../discard_max_bytes 제한 쓰기.
 * NVMe Deallocate(Trim) 명령의 최대 범위를 사용자가 제한한다 (추정).
 */
static int queue_max_discard_sectors_store(struct gendisk *disk,
		const char *page, size_t count, struct queue_limits *lim)
{
	unsigned long max_discard_bytes;
	ssize_t ret;

	ret = queue_var_store(&max_discard_bytes, page, count);
	if (ret < 0)
		return ret;

	if (max_discard_bytes & (disk->queue->limits.discard_granularity - 1)) /* discard_granularity 미만 단위는 NVMe Deallocate 명령에 맞지 않음 (추정). */
		return -EINVAL;

	if ((max_discard_bytes >> SECTOR_SHIFT) > UINT_MAX) /* sector 단위가 UINT_MAX 초과 시 저장 불가. */
		return -EINVAL;

	lim->max_user_discard_sectors = max_discard_bytes >> SECTOR_SHIFT; /* 사용자 지정 최대 discard 범위 기록. */
	return 0;
}

/*
 * queue_max_wzeroes_unmap_sectors_store: write zeroes unmap 최대값 제한.
 * NVMe Write Zeroes/Deallocate 관련 limit을 제어한다 (추정).
 */
static int queue_max_wzeroes_unmap_sectors_store(struct gendisk *disk,
		const char *page, size_t count, struct queue_limits *lim)
{
	unsigned long max_zeroes_bytes, max_hw_zeroes_bytes;
	ssize_t ret;

	ret = queue_var_store(&max_zeroes_bytes, page, count);
	if (ret < 0)
		return ret;

	max_hw_zeroes_bytes = lim->max_hw_wzeroes_unmap_sectors << SECTOR_SHIFT; /* 하드웨어가 지원하는 write zeroes/unmap 크기 계산. */
	if (max_zeroes_bytes != 0 && max_zeroes_bytes != max_hw_zeroes_bytes) /* 0(제한 없음) 또는 하드웨어 최대값만 허용. */
		return -EINVAL;

	lim->max_user_wzeroes_unmap_sectors = max_zeroes_bytes >> SECTOR_SHIFT; /* NVMe Write Zeroes/Deallocate 범위 제한에 반영 (추정). */
	return 0;
}

/*
 * queue_max_sectors_store: /sys/.../max_sectors_kb 쓰기.
 * NVMe 한 명령당 최대 전송 sector 수를 제한하며, PRP/SGL 리스트
 * 길이 및 메모리 할당에 영향을 준다 (추정).
 */
static int
queue_max_sectors_store(struct gendisk *disk, const char *page, size_t count,
		struct queue_limits *lim)
{
	unsigned long max_sectors_kb;
	ssize_t ret;

	ret = queue_var_store(&max_sectors_kb, page, count);
	if (ret < 0)
		return ret;

	lim->max_user_sectors = max_sectors_kb << 1; /* KB -> sector(512B) 변환; NVMe PRP/SGL 최대 길이에 영향 (추정). */
	return 0;
}

/*
 * queue_feature_store: queue_limits.features의 비트를 설정/해제.
 * BLK_FEAT_FUA 같은 플래그는 NVMe FUA bit 사용 여부와 연결된다 (추정).
 */
static ssize_t queue_feature_store(struct gendisk *disk, const char *page,
		size_t count, struct queue_limits *lim, blk_features_t feature)
{
	unsigned long val;
	ssize_t ret;

	ret = queue_var_store(&val, page, count);
	if (ret < 0)
		return ret;

	if (val) /* feature bit 설정/해제; e.g. BLK_FEAT_FUA는 NVMe FUA bit 사용 여부와 연결 (추정). */
		lim->features |= feature;
	else
		lim->features &= ~feature;
	return 0;
}

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

QUEUE_SYSFS_FEATURE(rotational, BLK_FEAT_ROTATIONAL)
QUEUE_SYSFS_FEATURE(add_random, BLK_FEAT_ADD_RANDOM)
QUEUE_SYSFS_FEATURE(iostats, BLK_FEAT_IO_STAT)
QUEUE_SYSFS_FEATURE(stable_writes, BLK_FEAT_STABLE_WRITES);

#define QUEUE_SYSFS_FEATURE_SHOW(_name, _feature)			\
static ssize_t queue_##_name##_show(struct gendisk *disk, char *page)	\
{									\
	return sysfs_emit(page, "%u\n",					\
		!!(disk->queue->limits.features & _feature));		\
}

QUEUE_SYSFS_FEATURE_SHOW(fua, BLK_FEAT_FUA);
QUEUE_SYSFS_FEATURE_SHOW(dax, BLK_FEAT_DAX);

/*
 * queue_poll_show: polling 지원 여부를 sysfs에 노출.
 * NVMe 드라이버가 poll queue를 지원하는 경우 활성화로 표시된다.
 */
static ssize_t queue_poll_show(struct gendisk *disk, char *page)
{
	if (queue_is_mq(disk->queue))
		return sysfs_emit(page, "%u\n", blk_mq_can_poll(disk->queue));

	return sysfs_emit(page, "%u\n",
			!!(disk->queue->limits.features & BLK_FEAT_POLL));
}

static ssize_t queue_zoned_show(struct gendisk *disk, char *page)
{
	if (blk_queue_is_zoned(disk->queue))
		return sysfs_emit(page, "host-managed\n");
	return sysfs_emit(page, "none\n");
}

static ssize_t queue_nr_zones_show(struct gendisk *disk, char *page)
{
	return queue_var_show(disk_nr_zones(disk), page);
}

static ssize_t queue_zoned_qd1_writes_show(struct gendisk *disk, char *page)
{
	return queue_var_show(!!blk_queue_zoned_qd1_writes(disk->queue),
			      page);
}

/*
 * queue_zoned_qd1_writes_store: /sys/.../zoned_qd1_writes 쓰기.
 * NVMe ZNS(zoned namespace) 쓰기를 queue depth 1로 강제하여 zone
 * 순차 쓰기 규칙을 보장한다 (추정).
 */
static ssize_t queue_zoned_qd1_writes_store(struct gendisk *disk,
					    const char *page, size_t count)
{
	struct request_queue *q = disk->queue;
	unsigned long qd1_writes;
	unsigned int memflags;
	ssize_t ret;

	ret = queue_var_store(&qd1_writes, page, count);
	if (ret < 0)
		return ret;

	memflags = blk_mq_freeze_queue(q); /* 큐 동결: 새 I/O가 들어오지 않도록 함. */
	blk_mq_quiesce_queue(q); /* 진행 중 I/O가 완료될 때까지 대기. */
	if (qd1_writes) /* NVMe ZNS 쓰기를 QD=1으로 강제. */
		blk_queue_flag_set(QUEUE_FLAG_ZONED_QD1_WRITES, q);
	else
		blk_queue_flag_clear(QUEUE_FLAG_ZONED_QD1_WRITES, q);
	blk_mq_unquiesce_queue(q); /* NVMe I/O 처리 재개. */
	blk_mq_unfreeze_queue(q, memflags); /* 동결 해제. */

	return count;
}

static ssize_t queue_iostats_passthrough_show(struct gendisk *disk, char *page)
{
	return queue_var_show(!!blk_queue_passthrough_stat(disk->queue), page);
}

/*
 * queue_iostats_passthrough_store: passthrough I/O 통계 수집 설정.
 * NVMe admin/io passthrough 명령 통계 포함 가능 (추정).
 */
static int queue_iostats_passthrough_store(struct gendisk *disk,
		const char *page, size_t count, struct queue_limits *lim)
{
	unsigned long ios;
	ssize_t ret;

	ret = queue_var_store(&ios, page, count);
	if (ret < 0)
		return ret;

	if (ios) /* passthrough I/O 통계 수집 활성화; NVMe admin/io passthrough 명령 통계 포함 (추정). */
		lim->flags |= BLK_FLAG_IOSTATS_PASSTHROUGH;
	else
		lim->flags &= ~BLK_FLAG_IOSTATS_PASSTHROUGH;
	return 0;
}

/*
 * queue_nomerges_show: /sys/.../nomerges 현재값 읽기.
 * 0/1/2 값은 NVMe에서 bio/request 병합 정책을 나타낸다.
 */
static ssize_t queue_nomerges_show(struct gendisk *disk, char *page)
{
	return queue_var_show((blk_queue_nomerges(disk->queue) << 1) |
			       blk_queue_noxmerges(disk->queue), page);
}

/*
 * queue_nomerges_store: /sys/.../nomerges 쓰기.
 * NVMe에서는 큰 단일 I/O보다 병합을 억제하여 SQ entry당 bio 구조를
 * 단순화할 때 사용할 수 있다 (추정).
 */
static ssize_t queue_nomerges_store(struct gendisk *disk, const char *page,
				    size_t count)
{
	unsigned long nm;
	struct request_queue *q = disk->queue;
	ssize_t ret = queue_var_store(&nm, page, count);

	if (ret < 0)
		return ret;

	blk_queue_flag_clear(QUEUE_FLAG_NOMERGES, q); /* 기존 설정 초기화. */
	blk_queue_flag_clear(QUEUE_FLAG_NOXMERGES, q);
	if (nm == 2) /* 2: bio 병합 금지; NVMe SQ entry 크기 최적화를 위해 단일 bio 선호 시 사용. */
		blk_queue_flag_set(QUEUE_FLAG_NOMERGES, q);
	else if (nm) /* 1: 병합 시도조차 하지 않음. */
		blk_queue_flag_set(QUEUE_FLAG_NOXMERGES, q);

	return ret;
}

/*
 * queue_rq_affinity_show: /sys/.../rq_affinity 읽기.
 * NVMe CQ(interrupt) 처리 CPU와 request 제출 CPU의 일치 정책을 표시.
 */
static ssize_t queue_rq_affinity_show(struct gendisk *disk, char *page)
{
	bool set = test_bit(QUEUE_FLAG_SAME_COMP, &disk->queue->queue_flags); /* 완료 CPU와 제출 CPU 일치 여부. */
	bool force = test_bit(QUEUE_FLAG_SAME_FORCE, &disk->queue->queue_flags); /* 강제 일치 여부. */

	return queue_var_show(set << force, page);
}

/*
 * queue_rq_affinity_store: /sys/.../rq_affinity 쓰기.
 * NVMe CQ 핸들러가 실행될 CPU를 제출 CPU에 근접하게 배치하여
 * 캐시 효율과 latency를 조율한다 (추정).
 */
static ssize_t
queue_rq_affinity_store(struct gendisk *disk, const char *page, size_t count)
{
	ssize_t ret = -EINVAL;
#ifdef CONFIG_SMP
	struct request_queue *q = disk->queue;
	unsigned long val;

	ret = queue_var_store(&val, page, count);
	if (ret < 0)
		return ret;

	/*
	 * Here we update two queue flags each using atomic bitops, although
	 * updating two flags isn't atomic it should be harmless as those flags
	 * are accessed individually using atomic test_bit operation. So we
	 * don't grab any lock while updating these flags.
	 */
	if (val == 2) { /* 2: 완료 인터럽트를 요청 CPU와 동일하게; NVMe CQ affinity와 관련 (추정). */
		blk_queue_flag_set(QUEUE_FLAG_SAME_COMP, q);
		blk_queue_flag_set(QUEUE_FLAG_SAME_FORCE, q);
	} else if (val == 1) { /* 1: 완료 CPU 일치를 권장. */
		blk_queue_flag_set(QUEUE_FLAG_SAME_COMP, q);
		blk_queue_flag_clear(QUEUE_FLAG_SAME_FORCE, q);
	} else if (val == 0) { /* 0: affinity 정책 해제. */
		blk_queue_flag_clear(QUEUE_FLAG_SAME_COMP, q);
		blk_queue_flag_clear(QUEUE_FLAG_SAME_FORCE, q);
	}
#endif
	return ret;
}

static ssize_t queue_poll_delay_store(struct gendisk *disk, const char *page,
				size_t count)
{
	return count;
}

/*
 * queue_poll_store: /sys/.../io_poll 쓰기 핸들러.
 * NVMe polling 설정은 드라이버별 파라미터로 제어하므로, generic
 * sysfs 쓰기는 무시되고 안내 메시지만 출력된다.
 */
static ssize_t queue_poll_store(struct gendisk *disk, const char *page,
				size_t count)
{
	ssize_t ret = count;
	struct request_queue *q = disk->queue;

	if (!(q->limits.features & BLK_FEAT_POLL)) { /* NVMe polling 지원 시에만 의미 있음; 드라이버 파라미터로 대체 권장. */
		ret = -EINVAL;
		goto out;
	}

	pr_info_ratelimited("writes to the poll attribute are ignored.\n"); /* poll 속성 쓰기는 무시됨. */
	pr_info_ratelimited("please use driver specific parameters instead.\n");
out:
	return ret;
}

/*
 * queue_io_timeout_show: /sys/.../io_timeout 읽기.
 * NVMe 명령이 완료되지 않았을 때 abort/recovery가 시작되는 기한을
 * 밀리초 단위로 반환한다.
 */
static ssize_t queue_io_timeout_show(struct gendisk *disk, char *page)
{
	return sysfs_emit(page, "%u\n",
			jiffies_to_msecs(READ_ONCE(disk->queue->rq_timeout))); /* ms 단위로 변환하여 노출. */
}

/*
 * queue_io_timeout_store: /sys/.../io_timeout 쓰기.
 *
 * 목적: request_queue 전체의 timeout 값을 설정한다.
 *
 * NVMe 연결점:
 *   - 타임아웃이 발생하면 nvme_timeout (드라이버 등록)이 호출되어
 *     해당 CID를 abort하고 doorbell을 울린다 (추정).
 *   - 호출 경로: queue_io_timeout_store -> blk_queue_rq_timeout
 *     -> (I/O timeout) -> nvme_timeout -> nvme_abort_req -> doorbell.
 */
static ssize_t queue_io_timeout_store(struct gendisk *disk, const char *page,
				  size_t count)
{
	unsigned int val;
	int err;
	struct request_queue *q = disk->queue;

	err = kstrtou32(page, 10, &val);
	if (err || val == 0) /* 0 ms 타임아웃은 허용되지 않음. */
		return -EINVAL;

	blk_queue_rq_timeout(q, msecs_to_jiffies(val)); /* 타임아웃 갱신; NVMe 명령이 이 시간 초과 시 nvme_timeout -> abort CID -> doorbell 경로로 복구 (추정). */

	return count;
}

/*
 * queue_wc_show: /sys/.../write_cache 읽기.
 * NVMe Volatile Write Cache(VWC)의 활성화 상태를 반환한다 (추정).
 */
static ssize_t queue_wc_show(struct gendisk *disk, char *page)
{
	if (blk_queue_write_cache(disk->queue)) /* volatile write cache 활성화 상태. */
		return sysfs_emit(page, "write back\n");
	return sysfs_emit(page, "write through\n");
}

/*
 * queue_wc_store: /sys/.../write_cache 쓰기.
 * NVMe VWC(Volatile Write Cache) feature를 활성/비활성화한다 (추정).
 * write through로 설정하면 flush/FUA 동작에 영향을 준다.
 */
static int queue_wc_store(struct gendisk *disk, const char *page,
		size_t count, struct queue_limits *lim)
{
	bool disable;

	if (!strncmp(page, "write back", 10)) { /* write back: NVMe Volatile Write Cache(VWC) 사용 (추정). */
		disable = false;
	} else if (!strncmp(page, "write through", 13) || /* write through/none: 캐시 비활성화. */
		   !strncmp(page, "none", 4)) {
		disable = true;
	} else {
		return -EINVAL;
	}

	if (disable)
		lim->flags |= BLK_FLAG_WRITE_CACHE_DISABLED; /* 캐시 off 플래그 설정; flush/FUA 동작에 영향. */
	else
		lim->flags &= ~BLK_FLAG_WRITE_CACHE_DISABLED;
	return 0;
}

#define QUEUE_RO_ENTRY(_prefix, _name)				\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr	= { .name = _name, .mode = 0444 },		\
	.show	= _prefix##_show,				\
};

#define QUEUE_RW_ENTRY(_prefix, _name)				\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr	= { .name = _name, .mode = 0644 },		\
	.show	= _prefix##_show,				\
	.store	= _prefix##_store,				\
};

#define QUEUE_LIM_RO_ENTRY(_prefix, _name)			\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr		= { .name = _name, .mode = 0444 },	\
	.show_limit	= _prefix##_show,			\
}

#define QUEUE_LIM_RW_ENTRY(_prefix, _name)			\
static const struct queue_sysfs_entry _prefix##_entry = {	\
	.attr		= { .name = _name, .mode = 0644 },	\
	.show_limit	= _prefix##_show,			\
	.store_limit	= _prefix##_store,			\
}

QUEUE_RW_ENTRY(queue_requests, "nr_requests");
QUEUE_RW_ENTRY(queue_async_depth, "async_depth");
QUEUE_RW_ENTRY(queue_ra, "read_ahead_kb");
QUEUE_LIM_RW_ENTRY(queue_max_sectors, "max_sectors_kb");
QUEUE_LIM_RO_ENTRY(queue_max_hw_sectors, "max_hw_sectors_kb");
QUEUE_LIM_RO_ENTRY(queue_max_segments, "max_segments");
QUEUE_LIM_RO_ENTRY(queue_max_integrity_segments, "max_integrity_segments");
QUEUE_LIM_RO_ENTRY(queue_max_segment_size, "max_segment_size");
QUEUE_LIM_RO_ENTRY(queue_max_write_streams, "max_write_streams");
QUEUE_LIM_RO_ENTRY(queue_write_stream_granularity, "write_stream_granularity");
QUEUE_RW_ENTRY(elv_iosched, "scheduler");

QUEUE_LIM_RO_ENTRY(queue_logical_block_size, "logical_block_size");
QUEUE_LIM_RO_ENTRY(queue_physical_block_size, "physical_block_size");
QUEUE_LIM_RO_ENTRY(queue_chunk_sectors, "chunk_sectors");
QUEUE_LIM_RO_ENTRY(queue_io_min, "minimum_io_size");
QUEUE_LIM_RO_ENTRY(queue_io_opt, "optimal_io_size");

QUEUE_LIM_RO_ENTRY(queue_max_discard_segments, "max_discard_segments");
QUEUE_LIM_RO_ENTRY(queue_discard_granularity, "discard_granularity");
QUEUE_LIM_RO_ENTRY(queue_max_hw_discard_sectors, "discard_max_hw_bytes");
QUEUE_LIM_RW_ENTRY(queue_max_discard_sectors, "discard_max_bytes");
QUEUE_RO_ENTRY(queue_discard_zeroes_data, "discard_zeroes_data");

QUEUE_LIM_RO_ENTRY(queue_atomic_write_max_sectors, "atomic_write_max_bytes");
QUEUE_LIM_RO_ENTRY(queue_atomic_write_boundary_sectors,
		"atomic_write_boundary_bytes");
QUEUE_LIM_RO_ENTRY(queue_atomic_write_unit_max, "atomic_write_unit_max_bytes");
QUEUE_LIM_RO_ENTRY(queue_atomic_write_unit_min, "atomic_write_unit_min_bytes");

QUEUE_RO_ENTRY(queue_write_same_max, "write_same_max_bytes");
QUEUE_LIM_RO_ENTRY(queue_max_write_zeroes_sectors, "write_zeroes_max_bytes");
QUEUE_LIM_RO_ENTRY(queue_max_hw_wzeroes_unmap_sectors,
		"write_zeroes_unmap_max_hw_bytes");
QUEUE_LIM_RW_ENTRY(queue_max_wzeroes_unmap_sectors,
		"write_zeroes_unmap_max_bytes");
QUEUE_LIM_RO_ENTRY(queue_max_zone_append_sectors, "zone_append_max_bytes");
QUEUE_LIM_RO_ENTRY(queue_zone_write_granularity, "zone_write_granularity");

QUEUE_LIM_RO_ENTRY(queue_zoned, "zoned");
QUEUE_RW_ENTRY(queue_zoned_qd1_writes, "zoned_qd1_writes");
QUEUE_RO_ENTRY(queue_nr_zones, "nr_zones");
QUEUE_LIM_RO_ENTRY(queue_max_open_zones, "max_open_zones");
QUEUE_LIM_RO_ENTRY(queue_max_active_zones, "max_active_zones");

QUEUE_RW_ENTRY(queue_nomerges, "nomerges");
QUEUE_LIM_RW_ENTRY(queue_iostats_passthrough, "iostats_passthrough");
QUEUE_RW_ENTRY(queue_rq_affinity, "rq_affinity");
QUEUE_RW_ENTRY(queue_poll, "io_poll");
QUEUE_RW_ENTRY(queue_poll_delay, "io_poll_delay");
QUEUE_LIM_RW_ENTRY(queue_wc, "write_cache");
QUEUE_LIM_RO_ENTRY(queue_fua, "fua");
QUEUE_LIM_RO_ENTRY(queue_dax, "dax");
QUEUE_RW_ENTRY(queue_io_timeout, "io_timeout");
QUEUE_LIM_RO_ENTRY(queue_virt_boundary_mask, "virt_boundary_mask");
QUEUE_LIM_RO_ENTRY(queue_dma_alignment, "dma_alignment");

/* legacy alias for logical_block_size: */
static const struct queue_sysfs_entry queue_hw_sector_size_entry = {
	.attr		= {.name = "hw_sector_size", .mode = 0444 },
	.show_limit	= queue_logical_block_size_show,
};

QUEUE_LIM_RW_ENTRY(queue_rotational, "rotational");
QUEUE_LIM_RW_ENTRY(queue_iostats, "iostats");
QUEUE_LIM_RW_ENTRY(queue_add_random, "add_random");
QUEUE_LIM_RW_ENTRY(queue_stable_writes, "stable_writes");

#ifdef CONFIG_BLK_WBT
static ssize_t queue_var_store64(s64 *var, const char *page)
{
	int err;
	s64 v;

	err = kstrtos64(page, 10, &v);
	if (err < 0)
		return err;

	*var = v;
	return 0;
}

static ssize_t queue_wb_lat_show(struct gendisk *disk, char *page)
{
	ssize_t ret;
	struct request_queue *q = disk->queue;

	mutex_lock(&disk->rqos_state_mutex);
	if (!wbt_rq_qos(q)) {
		ret = -EINVAL;
		goto out;
	}

	if (wbt_disabled(q)) {
		ret = sysfs_emit(page, "0\n");
		goto out;
	}

	ret = sysfs_emit(page, "%llu\n", div_u64(wbt_get_min_lat(q), 1000));
out:
	mutex_unlock(&disk->rqos_state_mutex);
	return ret;
}

static ssize_t queue_wb_lat_store(struct gendisk *disk, const char *page,
				  size_t count)
{
	ssize_t ret;
	s64 val;

	ret = queue_var_store64(&val, page);
	if (ret < 0)
		return ret;
	if (val < -1)
		return -EINVAL;

	ret = wbt_set_lat(disk, val);
	return ret ? ret : count;
}

QUEUE_RW_ENTRY(queue_wb_lat, "wbt_lat_usec");
#endif

/* Common attributes for bio-based and request-based queues. */
static const struct attribute *const queue_attrs[] = {
	/*
	 * Attributes which are protected with q->limits_lock.
	 */
	&queue_max_hw_sectors_entry.attr,
	&queue_max_sectors_entry.attr,
	&queue_max_segments_entry.attr,
	&queue_max_discard_segments_entry.attr,
	&queue_max_integrity_segments_entry.attr,
	&queue_max_segment_size_entry.attr,
	&queue_max_write_streams_entry.attr,
	&queue_write_stream_granularity_entry.attr,
	&queue_hw_sector_size_entry.attr,
	&queue_logical_block_size_entry.attr,
	&queue_physical_block_size_entry.attr,
	&queue_chunk_sectors_entry.attr,
	&queue_io_min_entry.attr,
	&queue_io_opt_entry.attr,
	&queue_discard_granularity_entry.attr,
	&queue_max_discard_sectors_entry.attr,
	&queue_max_hw_discard_sectors_entry.attr,
	&queue_atomic_write_max_sectors_entry.attr,
	&queue_atomic_write_boundary_sectors_entry.attr,
	&queue_atomic_write_unit_min_entry.attr,
	&queue_atomic_write_unit_max_entry.attr,
	&queue_max_write_zeroes_sectors_entry.attr,
	&queue_max_hw_wzeroes_unmap_sectors_entry.attr,
	&queue_max_wzeroes_unmap_sectors_entry.attr,
	&queue_max_zone_append_sectors_entry.attr,
	&queue_zone_write_granularity_entry.attr,
	&queue_rotational_entry.attr,
	&queue_zoned_entry.attr,
	&queue_max_open_zones_entry.attr,
	&queue_max_active_zones_entry.attr,
	&queue_iostats_passthrough_entry.attr,
	&queue_iostats_entry.attr,
	&queue_stable_writes_entry.attr,
	&queue_add_random_entry.attr,
	&queue_wc_entry.attr,
	&queue_fua_entry.attr,
	&queue_dax_entry.attr,
	&queue_virt_boundary_mask_entry.attr,
	&queue_dma_alignment_entry.attr,
	&queue_ra_entry.attr,

	/*
	 * Attributes which don't require locking.
	 */
	&queue_discard_zeroes_data_entry.attr,
	&queue_write_same_max_entry.attr,
	&queue_nr_zones_entry.attr,
	&queue_nomerges_entry.attr,
	&queue_poll_entry.attr,
	&queue_poll_delay_entry.attr,
	&queue_zoned_qd1_writes_entry.attr,

	NULL,
};

/* Request-based queue attributes that are not relevant for bio-based queues. */
static const struct attribute *const blk_mq_queue_attrs[] = {
	/*
	 * Attributes which require some form of locking other than
	 * q->sysfs_lock.
	 */
	&elv_iosched_entry.attr,
	&queue_requests_entry.attr,
	&queue_async_depth_entry.attr,
#ifdef CONFIG_BLK_WBT
	&queue_wb_lat_entry.attr,
#endif
	/*
	 * Attributes which don't require locking.
	 */
	&queue_rq_affinity_entry.attr,
	&queue_io_timeout_entry.attr,

	NULL,
};

/*
 * queue_attr_visible: 일반 queue 속성의 sysfs 노출 여부 결정.
 * NVMe ZNS 장치가 아니면 zoned 관련 속성을 숨긴다.
 */
static umode_t queue_attr_visible(struct kobject *kobj, const struct attribute *attr,
				int n)
{
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);
	struct request_queue *q = disk->queue;

	if ((attr == &queue_max_open_zones_entry.attr ||
	     attr == &queue_max_active_zones_entry.attr ||
	     attr == &queue_zoned_qd1_writes_entry.attr) &&
	    !blk_queue_is_zoned(q)) /* zoned 디바이스가 아니면 관련 속성 숨김; NVMe ZNS 전용. */
		return 0;

	return attr->mode;
}

/*
 * blk_mq_queue_attr_visible: blk-mq 전용 속성의 sysfs 노출 여부 결정.
 * NVMe는 blk-mq 기반이므로 대부분의 속성이 노출된다.
 */
static umode_t blk_mq_queue_attr_visible(struct kobject *kobj,
					 const struct attribute *attr, int n)
{
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);
	struct request_queue *q = disk->queue;

	if (!queue_is_mq(q)) /* bio-based legacy 큐에는 blk-mq 속성 숨김. */
		return 0;

	if (attr == &queue_io_timeout_entry.attr && !q->mq_ops->timeout) /* 드라이버가 timeout 콜백을 등록하지 않으면 io_timeout 속성 숨김. */
		return 0;

	return attr->mode;
}

static const struct attribute_group queue_attr_group = {
	.attrs_const = queue_attrs,
	.is_visible_const = queue_attr_visible,
};

static const struct attribute_group blk_mq_queue_attr_group = {
	.attrs_const = blk_mq_queue_attrs,
	.is_visible_const = blk_mq_queue_attr_visible,
};

#define to_queue(atr) container_of_const((atr), struct queue_sysfs_entry, attr)

/*
 * queue_attr_show: sysfs "show" 콜백 분기.
 * NVMe queue 속성 읽기 요청을 show 또는 show_limit 핸들러로 라우팅.
 */
static ssize_t
queue_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct queue_sysfs_entry *entry = to_queue(attr);
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);

	if (!entry->show && !entry->show_limit) /* show/show_limit 중 하나는 반드시 있어야 함. */
		return -EIO;

	if (entry->show_limit) {
		ssize_t res;

		mutex_lock(&disk->queue->limits_lock); /* queue_limits 읽기 동기화. */
		res = entry->show_limit(disk, page);
		mutex_unlock(&disk->queue->limits_lock);
		return res;
	}

	return entry->show(disk, page);
}

/*
 * queue_attr_store: sysfs "store" 콜백 분기.
 * NVMe queue tunable 쓰기 요청을 store_limit(atomic limits update) 또는
 * store 핸들러로 라우팅한다.
 */
static ssize_t
queue_attr_store(struct kobject *kobj, struct attribute *attr,
		    const char *page, size_t length)
{
	struct queue_sysfs_entry *entry = to_queue(attr);
	struct gendisk *disk = container_of(kobj, struct gendisk, queue_kobj);
	struct request_queue *q = disk->queue;

	if (!entry->store_limit && !entry->store) /* store/store_limit 중 하나는 반드시 있어야 함. */
		return -EIO;

	if (entry->store_limit) {
		ssize_t res;

		struct queue_limits lim = queue_limits_start_update(q); /* queue_limits 변경 시작. */

		res = entry->store_limit(disk, page, length, &lim);
		if (res < 0) {
			queue_limits_cancel_update(q); /* 갱신 실패 시 롤백. */
			return res;
		}

		res = queue_limits_commit_update_frozen(q, &lim); /* limits_lock 보호 하에 실제 queue_limits에 반영. */
		if (res)
			return res;
		return length;
	}

	return entry->store(disk, page, length);
}

static const struct sysfs_ops queue_sysfs_ops = {
	.show	= queue_attr_show,
	.store	= queue_attr_store,
};

static const struct attribute_group *blk_queue_attr_groups[] = {
	&queue_attr_group,
	&blk_mq_queue_attr_group,
	NULL
};

/*
 * blk_queue_release: kobject 참조가 0이 되었을 때 호출.
 * request_queue 데이터는 parent gendisk가 관리하므로 아무것도 하지 않는다.
 */
static void blk_queue_release(struct kobject *kobj)
{
	/* nothing to do here, all data is associated with the parent gendisk */ /* gendisk가 lifecycle을 관리하므로 release는 비어 있음. */
}

/*
 * blk_queue_ktype: request_queue kobject의 타입.
 * .sysfs_ops를 통해 /sys/block/nvme*/queue/*의 읽기/쓰기가
 * queue_attr_show/store로 연결된다.
 */
const struct kobj_type blk_queue_ktype = {
	.default_groups = blk_queue_attr_groups,
	.sysfs_ops	= &queue_sysfs_ops,
	.release	= blk_queue_release,
};

/*
 * blk_debugfs_remove: /sys/kernel/debug/block/<disk> 트리 제거.
 * NVMe debugfs 인터페이스(hctx, SQ/CQ 상태 등)를 정리한다 (추정).
 */
static void blk_debugfs_remove(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;

	blk_debugfs_lock_nomemsave(q); /* debugfs 상태 보호. */
	blk_trace_shutdown(q); /* blktrace 종료. */
	debugfs_remove_recursive(q->debugfs_dir); /* /sys/kernel/debug/block/<disk> 제거. */
	q->debugfs_dir = NULL; /* 루트 디렉터리 참조 정리. */
	q->sched_debugfs_dir = NULL; /* scheduler debugfs 참조 정리. */
	q->rqos_debugfs_dir = NULL; /* rqos debugfs 참조 정리. */
	blk_debugfs_unlock_nomemrestore(q);
}

/**
 * blk_register_queue - register a block layer queue with sysfs
 * @disk: Disk of which the request queue should be registered with sysfs.
 */
/*
 * NVMe 관점: add_disk() 호출 시 nvme_alloc_ns_disk() 등에서 도달한다.
 * 목적: request_queue를 sysfs/debugfs에 등록하고, elevator 기본값과
 *       queue 사용 카운터를 초기화하여 NVMe I/O 처리를 가능하게 한다.
 *
 * 주요 호출 경로:
 *   nvme_alloc_ns_disk/add_disk -> blk_register_queue
 *   -> blk_mq_sysfs_register, blk_mq_debugfs_register
 *   -> elevator_set_default (none/mq-deadline)
 *   -> 이후: submit_bio -> blk_mq_submit_bio -> nvme_queue_rq
 *      -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결점:
 *   - queue_kobj 등록 후 /sys/block/nvme*/queue/* 노출.
 *   - blk_mq_debugfs_register로 hctx/SQ/CQ 상태 디버깅 인터페이스 제공.
 *   - QUEUE_FLAG_INIT_DONE + percpu ref 전환으로 NVMe SQ doorbell 등
 *     실제 I/O 진입이 허용된다 (추정).
 */
int blk_register_queue(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	unsigned int memflags;
	int ret;

	ret = kobject_add(&disk->queue_kobj, &disk_to_dev(disk)->kobj, "queue"); /* /sys/block/<disk>/queue 디렉터리 생성. */
	if (ret < 0)
		return ret;

	if (queue_is_mq(q)) {
		ret = blk_mq_sysfs_register(disk); /* /sys/block/<disk>/mq/ 하위 등록; NVMe hw queue별 속성. */
		if (ret)
			goto out_del_queue_kobj;
	}
	mutex_lock(&q->sysfs_lock);

	memflags = blk_debugfs_lock(q);
	q->debugfs_dir = debugfs_create_dir(disk->disk_name, blk_debugfs_root); /* /sys/kernel/debug/block/<disk> 생성. */
	if (queue_is_mq(q))
		blk_mq_debugfs_register(q); /* blk-mq hctx 디버그 파일 생성; NVMe SQ/CQ 상태 확인. */
	blk_debugfs_unlock(q, memflags);

	/*
	 * For blk-mq rotational zoned devices, default to using QD=1
	 * writes. For non-mq rotational zoned devices, the device driver can
	 * set an appropriate default.
	 */
	if (queue_is_mq(q) && blk_queue_rot(q) && blk_queue_is_zoned(q))
		blk_queue_flag_set(QUEUE_FLAG_ZONED_QD1_WRITES, q); /* 회전식 zoned NVMe는 기본 QD=1 쓰기. */

	ret = disk_register_independent_access_ranges(disk); /* 독립적 접근 범위(IA ranges) 등록; NVMe 멀티-논리 유닛 대응. */
	if (ret)
		goto out_debugfs_remove;

	ret = blk_crypto_sysfs_register(disk); /* blk-crypto sysfs 등록; NVMe inline encryption 지원 시 사용. */
	if (ret)
		goto out_unregister_ia_ranges;

	if (queue_is_mq(q))
		elevator_set_default(q); /* 기본 I/O 스케줄러 설정; NVMe에서는 보통 none 사용. */

	blk_queue_flag_set(QUEUE_FLAG_REGISTERED, q); /* 등록 완료 플래그. */
	wbt_init_enable_default(disk); /* writeback throttle 기본 활성화. */

	/* Now everything is ready and send out KOBJ_ADD uevent */
	kobject_uevent(&disk->queue_kobj, KOBJ_ADD); /* 사용자공간에 queue 등록 uevent 전송. */
	if (q->elevator)
		kobject_uevent(&q->elevator->kobj, KOBJ_ADD);
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
	blk_queue_flag_set(QUEUE_FLAG_INIT_DONE, q); /* 초기화 완료; 이 시점부터 NVMe 큐가 full functional. */
	percpu_ref_switch_to_percpu(&q->q_usage_counter); /* percpu reference로 전환하여 빠른 shutdown 가능성 유지. */

	return ret;

out_unregister_ia_ranges: /* 등록 실패 시 역순 정리 (IA ranges). */
	disk_unregister_independent_access_ranges(disk);
out_debugfs_remove:
	blk_debugfs_remove(disk);
	mutex_unlock(&q->sysfs_lock);
	if (queue_is_mq(q))
		blk_mq_sysfs_unregister(disk);
out_del_queue_kobj: /* 등록 실패 시 queue kobject 제거. */
	kobject_del(&disk->queue_kobj);
	return ret;
}

/**
 * blk_unregister_queue - counterpart of blk_register_queue()
 * @disk: Disk of which the request queue should be unregistered from sysfs.
 *
 * Note: the caller is responsible for guaranteeing that this function is called
 * after blk_register_queue() has finished.
 */
/*
 * NVMe 관점: del_gendisk() 경로에서 호출된다.
 * 목적: sysfs/debugfs 인터페이스를 역순으로 제거하고, NVMe I/O 경로를
 *       안전하게 종료하기 위한 전 단계를 수행한다.
 *
 * 주요 호출 경로:
 *   del_gendisk -> blk_unregister_queue -> blk_mq_sysfs_unregister
 *   -> elevator_set_none -> blk_debugfs_remove.
 *
 * NVMe 연결점:
 *   - QUEUE_FLAG_REGISTERED 해제로 sysfs 쓰기 경쟁 차단.
 *   - elevator_set_none으로 NVMe request 스케줄링 정리.
 *   - debugfs/crypto/IA ranges 제거로 NVMe queue 자원 해제 준비.
 */
void blk_unregister_queue(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;

	if (WARN_ON(!q))
		return;

	/* Return early if disk->queue was never registered. */
	if (!blk_queue_registered(q)) /* 등록되지 않은 큐는 조용히 리턴. */
		return;

	/*
	 * Since sysfs_remove_dir() prevents adding new directory entries
	 * before removal of existing entries starts, protect against
	 * concurrent elv_iosched_store() calls.
	 */
	mutex_lock(&q->sysfs_lock);
	blk_queue_flag_clear(QUEUE_FLAG_REGISTERED, q); /* sysfs 쓰기 경쟁 방지를 위해 등록 플래그 해제. */
	mutex_unlock(&q->sysfs_lock);

	/*
	 * Remove the sysfs attributes before unregistering the queue data
	 * structures that can be modified through sysfs.
	 */
	if (queue_is_mq(q))
		blk_mq_sysfs_unregister(disk); /* /sys/block/<disk>/mq 제거. */
	blk_crypto_sysfs_unregister(disk); /* crypto sysfs 제거. */

	mutex_lock(&q->sysfs_lock);
	disk_unregister_independent_access_ranges(disk); /* IA ranges 해제. */
	mutex_unlock(&q->sysfs_lock);

	/* Now that we've deleted all child objects, we can delete the queue. */
	kobject_uevent(&disk->queue_kobj, KOBJ_REMOVE); /* queue 제거 uevent 전송. */
	kobject_del(&disk->queue_kobj); /* /sys/block/<disk>/queue 삭제. */

	if (queue_is_mq(q))
		elevator_set_none(q); /* elevator 제거; NVMe I/O 경로 완전 종료 전 정리. */

	blk_debugfs_remove(disk); /* debugfs 제거. */
}
/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 request_queue의 sysfs/debugfs 인터페이스를 등록/해제하며,
 *   NVMe 드라이버가 생성한 queue를 사용자공간에 노출한다.
 * - nr_requests, async_depth, max_sectors_kb, io_timeout, scheduler,
 *   write_cache, zoned_qd1_writes 등은 NVMe SQ/CQ depth, CID 할당,
 *   PRP/SGL 준비, 타임아웃 복구, FUA/flush, ZNS 쓰기 직렬화와 연결된다.
 * - blk_register_queue는 add_disk() 경로에서, blk_unregister_queue는
 *   del_gendisk() 경로에서 호출되며, NVMe 생명주기와 밀접하게 연결된다.
 * - block/blk-mq.c, block/elevator.c, block/blk-zoned.c 등에서 설정된
 *   queue 특성을 이 파일이 사용자공간에 노출하는 지점이다.
 * - 대부분의 NVMe 연결 설명은 이 파일의 코드 흐름을 바탕으로 한 추정이다.
 */
