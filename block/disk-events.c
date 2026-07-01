// SPDX-License-Identifier: GPL-2.0
/*
 * Disk events - monitor disk events like media change and eject request.
 *
 * 파일 역할 요약:
 *   이 파일은 Linux block layer의 일부로, 디스크(블록 장치)에서 발생하는
 *   이벤트(media change, eject request)를 감지하고 사용자공간으로 uevent를
 *   전달하는 polling 기반 프레임워크를 제공합니다. NVMe SSD 관점에서는
 *   고정형 미디어를 가진 장치가 대부분이므로 DISK_EVENT_MEDIA_CHANGE는
 *   거의 사용되지 않지만, block layer 전반에서 이벤트 폴리싱 메커니즘을
 *   표준화하는 핵심 지점입니다. NVMe 드라이버는 blk_mq_run_hw_queue ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 I/O를 처리하는
 *   가운데, 본 파일은 sysfs(events_poll_msecs) 및 delayed work를 통해
 *   장치별 check_events 콜백을 주기적으로 호출합니다.
 *
 *   상위/하위 연결:
 *   - block/genhd.c: gendisk 및 디바이스 등록과 연결됨
 *   - block/blk-mq.c: NVMe SGL/PRP I/O 경로의 상위 layer
 *   - drivers/nvme/host/core.c: check_events 등 disk_operations 콜백 구현
 */
#include <linux/export.h>
#include <linux/moduleparam.h>
#include <linux/blkdev.h>
#include "blk.h"

/*
 * struct disk_events: 특정 gendisk에 대한 이벤트 상태를 추적.
 *
 * NVMe SSD 관점:
 *   - node: 전역 disk_events 리스트에 연결, 시스템 전체 폴 간격 변경 시
 *           list_for_each_entry로 순회(disk_events_set_dfl_poll_msecs)
 *   - disk: 연결된 gendisk; NVMe의 경우 nvme_alloc_ns() 등에서 초기화
 *   - lock: pending/clearing/block 갱신 시 보호; blk_mq 컨텍스트와 경쟁
 *   - block_mutex: block 카운트 조정 직렬화; suspend/resume 시 사용
 *   - block: 이벤트 폴 차단 깊이(block 중이면 dwork 미실행)
 *   - pending: 아직 처리되지 않은 이벤트 비트맵
 *   - clearing: 드라이버 check_events()에 넘겨 클리어 요청할 이벤트
 *   - poll_msecs: 폴 주기; -1이면 시스템 기본값 사용
 *   - dwork: 지연 work; system_freezable_power_efficient_wq에서 실행
 */
struct disk_events {
	struct list_head	node;		/* all disk_event's */
	struct gendisk		*disk;		/* the associated disk */
	spinlock_t		lock;

	struct mutex		block_mutex;	/* protects blocking */
	int			block;		/* event blocking depth */
	unsigned int		pending;	/* events already sent out */
	unsigned int		clearing;	/* events being cleared */

	long			poll_msecs;	/* interval, -1 for default */
	struct delayed_work	dwork;
};

static const char *disk_events_strs[] = {
	[ilog2(DISK_EVENT_MEDIA_CHANGE)]	= "media_change",
	[ilog2(DISK_EVENT_EJECT_REQUEST)]	= "eject_request",
};

static char *disk_uevents[] = {
	[ilog2(DISK_EVENT_MEDIA_CHANGE)]	= "DISK_MEDIA_CHANGE=1",
	[ilog2(DISK_EVENT_EJECT_REQUEST)]	= "DISK_EJECT_REQUEST=1",
};

/* list of all disk_events */
static DEFINE_MUTEX(disk_events_mutex);
static LIST_HEAD(disk_events);

/* disable in-kernel polling by default */
static unsigned long disk_events_dfl_poll_msecs;

/*
 * disk_events_poll_jiffies: per-disk 폴 주기를 jiffies로 환산.
 *
 * 호출 경로:
 *   disk_add_events -> __disk_unblock_events -> disk_events_poll_jiffies
 *   disk_events_workfn -> disk_check_events -> disk_events_poll_jiffies
 *
 * NVMe 관련:
 *   NVMe 장치는 보통 DISK_EVENT_FLAG_POLL 플래그 없이 동작하므로
 *   기본적으로 polling은 비활성화됨. events_poll_msecs sysfs를 통해
 *   강제로 활성화하면 이 함수가 주기를 결정.
 */
static unsigned long disk_events_poll_jiffies(struct gendisk *disk)
{
	struct disk_events *ev = disk->ev;
	long intv_msecs = 0;

	/*
	 * If device-specific poll interval is set, always use it.  If
	 * the default is being used, poll if the POLL flag is set.
	 */
	if (ev->poll_msecs >= 0)
		intv_msecs = ev->poll_msecs;
	else if (disk->event_flags & DISK_EVENT_FLAG_POLL)
		intv_msecs = disk_events_dfl_poll_msecs;

	return msecs_to_jiffies(intv_msecs);
}

/**
 * disk_block_events - block and flush disk event checking
 * @disk: disk to block events for
 *
 * On return from this function, it is guaranteed that event checking
 * isn't in progress and won't happen until unblocked by
 * disk_unblock_events().  Events blocking is counted and the actual
 * unblocking happens after the matching number of unblocks are done.
 *
 * Note that this intentionally does not block event checking from
 * disk_clear_events().
 *
 * CONTEXT:
 * Might sleep.
 */
/*
 * disk_block_events: 이벤트 polling을 차단하고 현재 진행 중인 dwork를
 * 취소(flush)합니다.
 *
 * 호출 경로 (추정):
 *   사용자 ioctl/suspend -> disk_block_events(this)
 *   -> cancel_delayed_work_sync(&disk->ev->dwork)
 *
 * NVMe 연결:
 *   NVMe 장치의 namespace가 제거되거나 시스템이 suspend될 때 이벤트
 *   검출을 중지하여 race를 방지. 이는 I/O SQ/CID 처리와 독립적이나,
 *   전원 상태 전환 시 doorbell 및 CQ 소비와의 일관성을 위해 동기화
 *   지점으로 활용될 수 있음.
 */
void disk_block_events(struct gendisk *disk)
{
	struct disk_events *ev = disk->ev;
	unsigned long flags;
	bool cancel;

	if (!ev)
		return;

	/*
	 * Outer mutex ensures that the first blocker completes canceling
	 * the event work before further blockers are allowed to finish.
	 */
	mutex_lock(&ev->block_mutex);

	spin_lock_irqsave(&ev->lock, flags);
	cancel = !ev->block++;
	spin_unlock_irqrestore(&ev->lock, flags);

	if (cancel)
		cancel_delayed_work_sync(&disk->ev->dwork);

	mutex_unlock(&ev->block_mutex);
}

/*
 * __disk_unblock_events: block 카운트를 감소시키고 polling을 재개.
 *
 * 호출 경로:
 *   disk_unblock_events(this) -> __disk_unblock_events(check_now=false)
 *   disk_add_events(this) -> __disk_unblock_events(check_now=true)
 *   disk_clear_events(this) -> __disk_unblock_events(check_now=ev->clearing?)
 *
 * NVMe 연결:
 *   check_now가 true면 즉시 disk_events_workfn -> disk_check_events가
 *   실행되어 check_events 콜백을 타고 NVMe 관련 이벤트(만약 구현돼
 *   있다면)를 스캔. false면 poll_msecs 간격으로 지연 예약.
 */
static void __disk_unblock_events(struct gendisk *disk, bool check_now)
{
	struct disk_events *ev = disk->ev;
	unsigned long intv;
	unsigned long flags;

	spin_lock_irqsave(&ev->lock, flags);

	if (WARN_ON_ONCE(ev->block <= 0))
		goto out_unlock;

	if (--ev->block)
		goto out_unlock;

	intv = disk_events_poll_jiffies(disk);
	if (check_now)
		queue_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, 0);
	else if (intv)
		queue_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, intv);
out_unlock:
	spin_unlock_irqrestore(&ev->lock, flags);
}

/**
 * disk_unblock_events - unblock disk event checking
 * @disk: disk to unblock events for
 *
 * Undo disk_block_events().  When the block count reaches zero, it
 * starts events polling if configured.
 *
 * CONTEXT:
 * Don't care.  Safe to call from irq context.
 */
void disk_unblock_events(struct gendisk *disk)
{
	if (disk->ev)
		__disk_unblock_events(disk, false);
}

/**
 * disk_flush_events - schedule immediate event checking and flushing
 * @disk: disk to check and flush events for
 * @mask: events to flush
 *
 * Schedule immediate event checking on @disk if not blocked.  Events in
 * @mask are scheduled to be cleared from the driver.  Note that this
 * doesn't clear the events from @disk->ev.
 *
 * CONTEXT:
 * If @mask is non-zero must be called with disk->open_mutex held.
 */
/*
 * disk_flush_events: 이벤트를 즉시 확인/클리어하도록 dwork를 예약.
 *
 * 호출 경로:
 *   sysfs events_dfl_poll_msecs 쓰기 -> disk_events_set_dfl_poll_msecs
 *   -> disk_flush_events(this) -> mod_delayed_work(..., 0)
 *
 * NVMe 연결:
 *   polling 간격 변경 시 NVMe를 포함한 모든 block 장치에 대해 즉시
 *   한 번의 이벤트 체크를 유발. NVMe의 경우 check_events가 비어 있거나
 *   NOP일 가능성이 높음(미디어 교체가 없는 고정형 SSD).
 */
void disk_flush_events(struct gendisk *disk, unsigned int mask)
{
	struct disk_events *ev = disk->ev;

	if (!ev)
		return;

	spin_lock_irq(&ev->lock);
	ev->clearing |= mask;
	if (!ev->block)
		mod_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, 0);
	spin_unlock_irq(&ev->lock);
}

/*
 * Tell userland about new events.  Only the events listed in @disk->events are
 * reported, and only if DISK_EVENT_FLAG_UEVENT is set.  Otherwise, events are
 * processed internally but never get reported to userland.
 */
static void disk_event_uevent(struct gendisk *disk, unsigned int events)
{
	char *envp[ARRAY_SIZE(disk_uevents) + 1] = { };
	int nr_events = 0, i;

	for (i = 0; i < ARRAY_SIZE(disk_uevents); i++)
		if (events & disk->events & (1 << i))
			envp[nr_events++] = disk_uevents[i];

	if (nr_events)
		kobject_uevent_env(&disk_to_dev(disk)->kobj, KOBJ_CHANGE, envp);
}

/*
 * disk_check_events: 실제로 디스크 이벤트를 체크하고 다음 폴을 예약.
 *
 * 호출 경로:
 *   disk_events_workfn -> disk_check_events -> disk->fops->check_events
 *   disk_clear_events -> disk_check_events -> disk->fops->check_events
 *
 * NVMe 연결:
 *   check_events 콜백은 장치별로 구현됨. NVMe SSD의 경우 이 콜백은
 *   보통 NULL이거나 미디어 변경이 없음을 단순 반환. 만약 NVMe 미래
 *   확장에서 asynchronous event notification(AEN)을 polling으로
 *   에뮬레이트한다면 이 지점이 NVMe controller status/async event
 *   log page를 읽는 진입점이 될 수 있음(추정).
 */
static void disk_check_events(struct disk_events *ev,
			      unsigned int *clearing_ptr)
{
	struct gendisk *disk = ev->disk;
	unsigned int clearing = *clearing_ptr;
	unsigned int events;
	unsigned long intv;

	/* check events */
	events = disk->fops->check_events(disk, clearing);

	/* accumulate pending events and schedule next poll if necessary */
	spin_lock_irq(&ev->lock);

	events &= ~ev->pending; /* 이미 보고된 이벤트는 중복 제거 */
	ev->pending |= events;
	*clearing_ptr &= ~clearing; /* 드라이버에 클리어 요청 완료됨 */

	intv = disk_events_poll_jiffies(disk);
	if (!ev->block && intv)
		queue_delayed_work(system_freezable_power_efficient_wq,
				&ev->dwork, intv);

	spin_unlock_irq(&ev->lock);

	if (events & DISK_EVENT_MEDIA_CHANGE)
		inc_diskseq(disk);

	if (disk->event_flags & DISK_EVENT_FLAG_UEVENT)
		disk_event_uevent(disk, events);
}

/**
 * disk_clear_events - synchronously check, clear and return pending events
 * @disk: disk to fetch and clear events from
 * @mask: mask of events to be fetched and cleared
 *
 * Disk events are synchronously checked and pending events in @mask
 * are cleared and returned.  This ignores the block count.
 *
 * CONTEXT:
 * Might sleep.
 */
/*
 * disk_clear_events: 동기적으로 이벤트를 확인, 클리어, 반환.
 *
 * 호출 경로:
 *   disk_check_media_change -> disk_clear_events -> disk_block_events
 *   -> disk_check_events -> __disk_unblock_events
 *
 * NVMe 연결:
 *   NVMe SSD는 미디어 변경이 일반적이지 않아 실제로 이 경로를
 *   거의 타지 않음. 다만 NVMe 엔드포인트가 removable 미디어를
 *   에뮬레이트하는 경우(예: NVMe enclosure의 NVMe-MI) 여기서
 *   partition rescan 트리거가 가능.
 */
static unsigned int disk_clear_events(struct gendisk *disk, unsigned int mask)
{
	struct disk_events *ev = disk->ev;
	unsigned int pending;
	unsigned int clearing = mask;

	if (!ev)
		return 0;

	disk_block_events(disk);

	/*
	 * store the union of mask and ev->clearing on the stack so that the
	 * race with disk_flush_events does not cause ambiguity (ev->clearing
	 * can still be modified even if events are blocked).
	 */
	spin_lock_irq(&ev->lock);
	clearing |= ev->clearing;
	ev->clearing = 0;
	spin_unlock_irq(&ev->lock);

	disk_check_events(ev, &clearing);
	/*
	 * if ev->clearing is not 0, the disk_flush_events got called in the
	 * middle of this function, so we want to run the workfn without delay.
	 */
	__disk_unblock_events(disk, ev->clearing ? true : false);

	/* then, fetch and clear pending events */
	spin_lock_irq(&ev->lock);
	pending = ev->pending & mask;
	ev->pending &= ~mask;
	spin_unlock_irq(&ev->lock);
	WARN_ON_ONCE(clearing & mask);

	return pending;
}

/**
 * disk_check_media_change - check if a removable media has been changed
 * @disk: gendisk to check
 *
 * Returns %true and marks the disk for a partition rescan whether a removable
 * media has been changed, and %false if the media did not change.
 */
/*
 * disk_check_media_change: removable 미디어 변경 여부 확인.
 *
 * 호출 경로 (추정):
 *   block device open/read -> ... -> disk_check_media_change
 *
 * NVMe 연결:
 *   NVMe SSD는 non-removable이므로 보통 false를 반환. NVMe over Fabrics
 *   또는 특수 컨트롤러에서 namespace attribute가 변경되는 경우에만
 *   GD_NEED_PART_SCAN이 설정될 수 있음(추정).
 */
bool disk_check_media_change(struct gendisk *disk)
{
	unsigned int events;

	events = disk_clear_events(disk, DISK_EVENT_MEDIA_CHANGE |
				   DISK_EVENT_EJECT_REQUEST);
	if (events & DISK_EVENT_MEDIA_CHANGE) {
		set_bit(GD_NEED_PART_SCAN, &disk->state);
		return true;
	}
	return false;
}
EXPORT_SYMBOL(disk_check_media_change);

/**
 * disk_force_media_change - force a media change event
 * @disk: the disk which will raise the event
 *
 * Should be called when the media changes for @disk.  Generates a uevent
 * and attempts to free all dentries and inodes and invalidates all block
 * device page cache entries in that case.
 *
 * Callers that need a partition re-scan should arrange for one explicitly.
 */
/*
 * disk_force_media_change: 강제로 media change 이벤트 발생.
 *
 * NVMe 연결:
 *   NVMe namespace가 hot-removed되거나 컨트롤러가 reset될 때 상위
 *   layer에서 호출될 수 있음. bdev_mark_dead()를 통해 block device
 *   cache를 무효화하며, 이는 NVMe I/O 경로에서 blk_mq_complete_request
 *   -> nvme_process_cq 로의 완료와 독립적으로 캐시 일관성을 유지하기
 *   위한 지점(추정).
 */
void disk_force_media_change(struct gendisk *disk)
{
	disk_event_uevent(disk, DISK_EVENT_MEDIA_CHANGE);
	inc_diskseq(disk);
	bdev_mark_dead(disk->part0, true);
}
EXPORT_SYMBOL_GPL(disk_force_media_change);

/*
 * Separate this part out so that a different pointer for clearing_ptr can be
 * passed in for disk_clear_events.
 */
/*
 * disk_events_workfn: delayed work handler. polling 주기마다 실행.
 *
 * 호출 경로:
 *   system_freezable_power_efficient_wq -> disk_events_workfn
 *   -> disk_check_events -> disk->fops->check_events
 */
static void disk_events_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct disk_events *ev = container_of(dwork, struct disk_events, dwork);

	disk_check_events(ev, &ev->clearing);
}

/*
 * A disk events enabled device has the following sysfs nodes under
 * its /sys/block/X/ directory.
 *
 * events		: list of all supported events
 * events_async		: list of events which can be detected w/o polling
 *			  (always empty, only for backwards compatibility)
 * events_poll_msecs	: polling interval, 0: disable, -1: system default
 */
static ssize_t __disk_events_show(unsigned int events, char *buf)
{
	const char *delim = "";
	ssize_t pos = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(disk_events_strs); i++)
		if (events & (1 << i)) {
			pos += sprintf(buf + pos, "%s%s",
				       delim, disk_events_strs[i]);
			delim = " ";
		}
	if (pos)
		pos += sprintf(buf + pos, "\n");
	return pos;
}

static ssize_t disk_events_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	if (!(disk->event_flags & DISK_EVENT_FLAG_UEVENT))
		return 0;
	return __disk_events_show(disk->events, buf);
}

static ssize_t disk_events_async_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t disk_events_poll_msecs_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	if (!disk->ev)
		return sprintf(buf, "-1\n");
	return sprintf(buf, "%ld\n", disk->ev->poll_msecs);
}

static ssize_t disk_events_poll_msecs_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct gendisk *disk = dev_to_disk(dev);
	long intv;

	if (!count || !sscanf(buf, "%ld", &intv))
		return -EINVAL;

	if (intv < 0 && intv != -1)
		return -EINVAL;

	if (!disk->ev)
		return -ENODEV;

	/* poll 주기 갱신: 기존 dwork 취소 후 새 주기로 재개 */
	disk_block_events(disk);
	disk->ev->poll_msecs = intv;
	__disk_unblock_events(disk, true);
	return count;
}

DEVICE_ATTR(events, 0444, disk_events_show, NULL);
DEVICE_ATTR(events_async, 0444, disk_events_async_show, NULL);
DEVICE_ATTR(events_poll_msecs, 0644, disk_events_poll_msecs_show,
	    disk_events_poll_msecs_store);

/*
 * The default polling interval can be specified by the kernel
 * parameter block.events_dfl_poll_msecs which defaults to 0
 * (disable).  This can also be modified runtime by writing to
 * /sys/module/block/parameters/events_dfl_poll_msecs.
 */
static int disk_events_set_dfl_poll_msecs(const char *val,
					  const struct kernel_param *kp)
{
	struct disk_events *ev;
	int ret;

	ret = param_set_ulong(val, kp);
	if (ret < 0)
		return ret;

	mutex_lock(&disk_events_mutex);
	list_for_each_entry(ev, &disk_events, node)
		disk_flush_events(ev->disk, 0);
	mutex_unlock(&disk_events_mutex);
	return 0;
}

static const struct kernel_param_ops disk_events_dfl_poll_msecs_param_ops = {
	.set	= disk_events_set_dfl_poll_msecs,
	.get	= param_get_ulong,
};

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX	"block."

module_param_cb(events_dfl_poll_msecs, &disk_events_dfl_poll_msecs_param_ops,
		&disk_events_dfl_poll_msecs, 0644);

/*
 * disk_{alloc|add|del|release}_events - initialize and destroy disk_events.
 */
/*
 * disk_alloc_events: gendisk에 disk_events 구조체 할당 및 초기화.
 *
 * 호출 경로 (추정):
 *   add_disk -> disk_alloc_events(this)
 *
 * NVMe 연결:
 *   NVMe namespace가 등록될 때 gendisk->ev가 설정됨. check_events
 *   콜백이나 disk->events가 없으면 이벤트 프레임워크를 사용하지
 *   않음. NVMe SSD는 일반적으로 이벤트를 사용하지 않으므로 이
 *   함수가 0을 반환하며 ev는 NULL일 수 있음.
 */
int disk_alloc_events(struct gendisk *disk)
{
	struct disk_events *ev;

	if (!disk->fops->check_events || !disk->events)
		return 0;

	ev = kzalloc_obj(*ev);
	if (!ev) {
		pr_warn("%s: failed to initialize events\n", disk->disk_name);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&ev->node);
	ev->disk = disk;
	spin_lock_init(&ev->lock);
	mutex_init(&ev->block_mutex);
	ev->block = 1; /* 초기에는 차단 상태, disk_add_events에서 해제 */
	ev->poll_msecs = -1; /* -1: 시스템 기본값 사용 */
	INIT_DELAYED_WORK(&ev->dwork, disk_events_workfn);

	disk->ev = ev;
	return 0;
}

/*
 * disk_add_events: 전역 disk_events 리스트에 등록하고 polling 시작.
 *
 * 호출 경로:
 *   add_disk -> disk_add_events -> __disk_unblock_events(disk, true)
 *   -> queue_delayed_work(..., 0)
 */
void disk_add_events(struct gendisk *disk)
{
	if (!disk->ev)
		return;

	mutex_lock(&disk_events_mutex);
	list_add_tail(&disk->ev->node, &disk_events);
	mutex_unlock(&disk_events_mutex);

	/*
	 * Block count is initialized to 1 and the following initial
	 * unblock kicks it into action.
	 */
	__disk_unblock_events(disk, true);
}

/*
 * disk_del_events: 리스트에서 제거하고 polling 중지.
 *
 * 호출 경로:
 *   del_gendisk -> disk_del_events -> disk_block_events
 */
void disk_del_events(struct gendisk *disk)
{
	if (disk->ev) {
		disk_block_events(disk);

		mutex_lock(&disk_events_mutex);
		list_del_init(&disk->ev->node);
		mutex_unlock(&disk_events_mutex);
	}
}

void disk_release_events(struct gendisk *disk)
{
	/* the block count should be 1 from disk_del_events() */
	WARN_ON_ONCE(disk->ev && disk->ev->block != 1);
	kfree(disk->ev);
}

/* NVMe 관점 핵심 요약
 * - 이 파일은 block layer의 이벤트 폴리싱 인프라로, NVMe I/O 경로
 *   (blk_mq_run_hw_queue -> nvme_queue_rq -> nvme_submit_cmd)와는
 *   별개의 제어 경로에서 동작합니다.
 * - NVMe SSD는 고정형 미디어이므로 DISK_EVENT_MEDIA_CHANGE/EJECT_REQUEST
 *   처리가 거의 발생하지 않으며, check_events 콜백도 종종 NULL입니다.
 * - sysfs(events_poll_msecs) 및 kernel module parameter를 통해 polling
 *   주기를 조절할 수 있고, 변경 시 disk_flush_events -> disk_events_workfn
 *   -> disk->fops->check_events 경로로 전파됩니다.
 * - disk_force_media_change는 NVMe namespace hot-remove/reset 시 상위
 *   layer가 block device cache 일관성을 맞추기 위해 호출할 수 있는
 *   지점입니다(추정).
 * - 논리적 선행 파일: block/genhd.c(gendisk 초기화), block/blk-mq.c(I/O
 *   큐 처리); 후속/연계: drivers/nvme/host/core.c(check_events 구현).
 */
