// SPDX-License-Identifier: GPL-2.0
/*
 *  gendisk handling
 *
 * Portions Copyright (C) 2020 Christoph Hellwig
 */

/*
 * ============================================================================
 * NVMe SSD 관점 gendisk 관리 파일
 *
 * 이 파일은 struct gendisk의 생명주기(할당/등록/삭제)와 블록 장치 번호(major/minor),
 * 파티션 스캔, sysfs/proc 인터페이스를 담당한다. NVMe SSD 입장에서는 상위 VFS/파일
 * 시스템의 bio가 request_queue를 거쳐 gendisk 단위로 라우팅되며, 이 파일은 해당
 * 디스크 객체가 블록 클래스에 노출되고 제거되는 관문(gate) 역할을 한다.
 * ============================================================================
 */
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/major.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/log2.h>
#include <linux/pm_runtime.h>
#include <linux/badblocks.h>
#include <linux/part_stat.h>
#include <linux/blktrace_api.h>

#include "blk-throttle.h"
#include "blk.h"
#include "blk-mq-sched.h"
#include "blk-rq-qos.h"
#include "blk-cgroup.h"

static struct kobject *block_depr;

/*
 * Unique, monotonically increasing sequential number associated with block
 * devices instances (i.e. incremented each time a device is attached).
 * Associating uevents with block devices in userspace is difficult and racy:
 * the uevent netlink socket is lossy, and on slow and overloaded systems has
 * a very high latency.
 * Block devices do not have exclusive owners in userspace, any process can set
 * one up (e.g. loop devices). Moreover, device names can be reused (e.g. loop0
 * can be reused again and again).
 * A userspace process setting up a block device and watching for its events
 * cannot thus reliably tell whether an event relates to the device it just set
 * up or another earlier instance with the same name.
 * This sequential number allows userspace processes to solve this problem, and
 * uniquely associate an uevent to the lifetime to a device.
 */
static atomic64_t diskseq;

/* for extended dynamic devt allocation, currently only one major is used */
#define NR_EXT_DEVT		(1 << MINORBITS)
static DEFINE_IDA(ext_devt_ida);

/*
 * set_capacity - gendisk의 논리 용량을 갱신
 * @disk: 대상 gendisk (NVMe 네임스페이스에 대응)
 * @sectors: 새로운 섹터 수
 *
 * NVMe 네임스페이스의 사용 가능한 논리 블록 수(LBA count)가 결정되면 이 함수를
 * 통해 상위 레이어에 용량을 알린다. 이 값은 파일시스템이 bio를 생성할 때
 * 최종 LBA 범위를 검증하는 기준이 된다.
 * 호출 경로(추정): nvme_update_disk_info -> set_capacity
 */
void set_capacity(struct gendisk *disk, sector_t sectors)
{
	if (sectors > BLK_DEV_MAX_SECTORS) {
		pr_warn_once("%s: truncate capacity from %lld to %lld\n",
				disk->disk_name, sectors,
				BLK_DEV_MAX_SECTORS);
		sectors = BLK_DEV_MAX_SECTORS;
	}

	bdev_set_nr_sectors(disk->part0, sectors);
}
EXPORT_SYMBOL(set_capacity);

/*
 * set_capacity_and_notify - 용량 변경 및 uevent 통지
 * @disk: 대상 gendisk
 * @size: 새 섹터 수
 *
 * NVMe 네임스페이스가 동적 재조정(namespace resize)되었을 때 호출된다.
 * 용량이 0에서/으로 바뀌지 않는 한 "RESIZE=1" uevent를 발생시켜 udev가
 * 파티션 테이블을 다시 읽도록 유도한다.
 */
/*
 * Set disk capacity and notify if the size is not currently zero and will not
 * be set to zero.  Returns true if a uevent was sent, otherwise false.
 */
bool set_capacity_and_notify(struct gendisk *disk, sector_t size)
{
	sector_t capacity = get_capacity(disk);
	char *envp[] = { "RESIZE=1", NULL };

	set_capacity(disk, size);

	/*
	 * Only print a message and send a uevent if the gendisk is user visible
	 * and alive.  This avoids spamming the log and udev when setting the
	 * initial capacity during probing.
	 */
	if (size == capacity ||
	    !disk_live(disk) ||
	    (disk->flags & GENHD_FL_HIDDEN))
		return false;

	pr_info_ratelimited("%s: detected capacity change from %lld to %lld\n",
		disk->disk_name, capacity, size);

	/*
	 * Historically we did not send a uevent for changes to/from an empty
	 * device.
	 */
	if (!capacity || !size)
		return false;
	kobject_uevent_env(&disk_to_dev(disk)->kobj, KOBJ_CHANGE, envp);
	return true;
}
EXPORT_SYMBOL_GPL(set_capacity_and_notify);

static void part_stat_read_all(struct block_device *part,
		struct disk_stats *stat)
{
	int cpu;

	memset(stat, 0, sizeof(struct disk_stats));
	for_each_possible_cpu(cpu) {
		struct disk_stats *ptr = per_cpu_ptr(part->bd_stats, cpu);
		int group;

		for (group = 0; group < NR_STAT_GROUPS; group++) {
			stat->nsecs[group] += ptr->nsecs[group];
			stat->sectors[group] += ptr->sectors[group];
			stat->ios[group] += ptr->ios[group];
			stat->merges[group] += ptr->merges[group];
		}

		stat->io_ticks += ptr->io_ticks;
	}
}

static void bdev_count_inflight_rw(struct block_device *part,
		unsigned int inflight[2], bool mq_driver)
{
	int write = 0;
	int read = 0;
	int cpu;

	if (mq_driver) {
		blk_mq_in_driver_rw(part, inflight); /* NVMe: SQ/CQ에 아직 완료되지 않은 CID 기반 요청 집계 */
		return;
	}

	for_each_possible_cpu(cpu) {
		read += part_stat_local_read_cpu(part, in_flight[READ], cpu);
		write += part_stat_local_read_cpu(part, in_flight[WRITE], cpu);
	}

	/*
	 * While iterating all CPUs, some IOs may be issued from a CPU already
	 * traversed and complete on a CPU that has not yet been traversed,
	 * causing the inflight number to be negative.
	 */
	inflight[READ] = read > 0 ? read : 0;
	inflight[WRITE] = write > 0 ? write : 0;
}

/*
 * bdev_count_inflight - 디바이스별 진행 중인 I/O 개수 반환
 * @part: 대상 block_device
 *
 * NVMe 큐에서 아직 완료되지 않은 요청 수를 집계한다.
 * rq-based 경로: blk_mq_start_request -> blk_account_io_start로 시작된 I/O
 * bio-based 경로: bdev_start_io_acct로 시작된 I/O
 */
/**
 * bdev_count_inflight - get the number of inflight IOs for a block device.
 *
 * @part: the block device.
 *
 * Inflight here means started IO accounting, from bdev_start_io_acct() for
 * bio-based block device, and from blk_account_io_start() for rq-based block
 * device.
 */
unsigned int bdev_count_inflight(struct block_device *part)
{
	unsigned int inflight[2] = {0};

	bdev_count_inflight_rw(part, inflight, false);

	return inflight[READ] + inflight[WRITE];
}
EXPORT_SYMBOL_GPL(bdev_count_inflight);

/*
 * Can be deleted altogether. Later.
 *
 */
#define BLKDEV_MAJOR_HASH_SIZE 255
/*
 * NVMe 연결점: major 번호는 /dev/nvmeXnY 같은 장치 노드 생성의 첫 단계이다.
 * NVMe 드라이버(drivers/nvme/host)는 이 해시를 통해 자신의 major를 등록하고,
 * 사용자 공간이 해당 major의 minor를 open할 때 디스크를 찾을 수 있게 한다.
 */
static struct blk_major_name {
	struct blk_major_name *next; /* 해시 체인의 다음 엔트리 */
	int major;                   /* /dev/nvmeXnY의 MAJOR 번호; NVMe 드라이버 등록 시 결정 */
	char name[16];               /* 드라이버 이름, 예: "nvme" */
#ifdef CONFIG_BLOCK_LEGACY_AUTOLOAD
	void (*probe)(dev_t devt);   /* 레거시 자동 로드 콜백(추정: 현대 NVMe 드라이버에서는 거의 사용 안 함) */
#endif
} *major_names[BLKDEV_MAJOR_HASH_SIZE]; /* major 번호 -> 드라이버 매핑 해시 테이블 */
static DEFINE_MUTEX(major_names_lock);
static DEFINE_SPINLOCK(major_names_spinlock);

/* index in the above - for now: assume no multimajor ranges */
static inline int major_to_index(unsigned major)
{
	return major % BLKDEV_MAJOR_HASH_SIZE;
}

#ifdef CONFIG_PROC_FS
void blkdev_show(struct seq_file *seqf, off_t offset)
{
	struct blk_major_name *dp;

	spin_lock(&major_names_spinlock);
	for (dp = major_names[major_to_index(offset)]; dp; dp = dp->next)
		if (dp->major == offset)
			seq_printf(seqf, "%3d %s\n", dp->major, dp->name);
	spin_unlock(&major_names_spinlock);
}
#endif /* CONFIG_PROC_FS */

/*
 * __register_blkdev - 새로운 블록 장치 major 번호 등록
 * @major: 요청 major 번호 (0이면 동적 할당)
 * @name: 장치 이름 (예: "nvme")
 * @probe: 레거시 자동 탐색 콜백
 *
 * NVMe 호스트 드라이버가 초기화될 때 자신의 major 번호를 시스템에 등록한다.
 * 등록된 major는 /dev/nvmeXnY 장치 노드 생성의 근거가 되며, 이후
 * add_disk()에서 gendisk의 major/minor가 설정된다.
 */
/**
 * __register_blkdev - register a new block device
 *
 * @major: the requested major device number [1..BLKDEV_MAJOR_MAX-1]. If
 *         @major = 0, try to allocate any unused major number.
 * @name: the name of the new block device as a zero terminated string
 * @probe: pre-devtmpfs / pre-udev callback used to create disks when their
 *	   pre-created device node is accessed. When a probe call uses
 *	   add_disk() and it fails the driver must cleanup resources. This
 *	   interface may soon be removed.
 *
 * The @name must be unique within the system.
 *
 * The return value depends on the @major input parameter:
 *
 *  - if a major device number was requested in range [1..BLKDEV_MAJOR_MAX-1]
 *    then the function returns zero on success, or a negative error code
 *  - if any unused major number was requested with @major = 0 parameter
 *    then the return value is the allocated major number in range
 *    [1..BLKDEV_MAJOR_MAX-1] or a negative error code otherwise
 *
 * See Documentation/admin-guide/devices.txt for the list of allocated
 * major numbers.
 *
 * Use register_blkdev instead for any new code.
 */
int __register_blkdev(unsigned int major, const char *name,
		void (*probe)(dev_t devt))
{
	struct blk_major_name **n, *p;
	int index, ret = 0;

	mutex_lock(&major_names_lock);

	/* temporary */
	if (major == 0) {
		for (index = ARRAY_SIZE(major_names)-1; index > 0; index--) {
			if (major_names[index] == NULL)
				break;
		}

		if (index == 0) {
			printk("%s: failed to get major for %s\n",
			       __func__, name);
			ret = -EBUSY;
			goto out;
		}
		major = index;
		ret = major;
	}

	if (major >= BLKDEV_MAJOR_MAX) {
		pr_err("%s: major requested (%u) is greater than the maximum (%u) for %s\n",
		       __func__, major, BLKDEV_MAJOR_MAX-1, name);

		ret = -EINVAL;
		goto out;
	}

	p = kmalloc_obj(struct blk_major_name);
	if (p == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	p->major = major;
#ifdef CONFIG_BLOCK_LEGACY_AUTOLOAD
	p->probe = probe;
#endif
	strscpy(p->name, name, sizeof(p->name));
	p->next = NULL;
	index = major_to_index(major);

	spin_lock(&major_names_spinlock);
	for (n = &major_names[index]; *n; n = &(*n)->next) {
		if ((*n)->major == major)
			break;
	}
	if (!*n)
		*n = p;
	else
		ret = -EBUSY;
	spin_unlock(&major_names_spinlock);

	if (ret < 0) {
		printk("register_blkdev: cannot get major %u for %s\n",
		       major, name);
		kfree(p);
	}
out:
	mutex_unlock(&major_names_lock);
	return ret;
}
EXPORT_SYMBOL(__register_blkdev);

/*
 * unregister_blkdev - 블록 장치 major 번호 등록 해제
 * @major: 등록 해제할 major 번호
 * @name: 등록 시 사용한 이름
 *
 * NVMe 드라이버가 unload되거나 컨트롤러가 제거될 때 major를 해제한다.
 */
void unregister_blkdev(unsigned int major, const char *name)
{
	struct blk_major_name **n;
	struct blk_major_name *p = NULL;
	int index = major_to_index(major);

	mutex_lock(&major_names_lock);
	spin_lock(&major_names_spinlock);
	for (n = &major_names[index]; *n; n = &(*n)->next)
		if ((*n)->major == major)
			break;
	if (!*n || strcmp((*n)->name, name)) {
		WARN_ON(1);
	} else {
		p = *n;
		*n = p->next;
	}
	spin_unlock(&major_names_spinlock);
	mutex_unlock(&major_names_lock);
	kfree(p);
}

EXPORT_SYMBOL(unregister_blkdev);

int blk_alloc_ext_minor(void)
{
	int idx;

	idx = ida_alloc_range(&ext_devt_ida, 0, NR_EXT_DEVT - 1, GFP_KERNEL);
	if (idx == -ENOSPC)
		return -EBUSY;
	return idx;
}

void blk_free_ext_minor(unsigned int minor)
{
	ida_free(&ext_devt_ida, minor);
}

void disk_uevent(struct gendisk *disk, enum kobject_action action)
{
	struct block_device *part;
	unsigned long idx;

	rcu_read_lock();
	xa_for_each(&disk->part_tbl, idx, part) {
		if (bdev_is_partition(part) && !bdev_nr_sectors(part))
			continue;
		if (!kobject_get_unless_zero(&part->bd_device.kobj))
			continue;

		rcu_read_unlock();
		kobject_uevent(bdev_kobj(part), action);
		put_device(&part->bd_device);
		rcu_read_lock();
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(disk_uevent);

/*
 * disk_scan_partitions - gendisk에 연결된 파티션 테이블 스캔
 * @disk: 대상 gendisk
 * @mode: 블록 장치 open 모드
 *
 * NVMe 네임스페이스가 등록된 후 사용자 공간에 노출되기 전에 파티션 정보를
 * 읽어들인다. GPT/MBR 파티션 테이블을 해석하여 disk->part_tbl에 추가한다.
 * 호출 경로: add_disk_final -> disk_scan_partitions
 */
int disk_scan_partitions(struct gendisk *disk, blk_mode_t mode)
{
	struct file *file;
	int ret = 0;

	if (!disk_has_partscan(disk))
		return -EINVAL;
	if (disk->open_partitions)
		return -EBUSY;

	/*
	 * If the device is opened exclusively by current thread already, it's
	 * safe to scan partitons, otherwise, use bd_prepare_to_claim() to
	 * synchronize with other exclusive openers and other partition
	 * scanners.
	 */
	if (!(mode & BLK_OPEN_EXCL)) {
		ret = bd_prepare_to_claim(disk->part0, disk_scan_partitions,
					  NULL);
		if (ret)
			return ret;
	}

	set_bit(GD_NEED_PART_SCAN, &disk->state);
	file = bdev_file_open_by_dev(disk_devt(disk), mode & ~BLK_OPEN_EXCL,
				     NULL, NULL);
	if (IS_ERR(file))
		ret = PTR_ERR(file);
	else
		fput(file);

	/*
	 * If blkdev_get_by_dev() failed early, GD_NEED_PART_SCAN is still set,
	 * and this will cause that re-assemble partitioned raid device will
	 * creat partition for underlying disk.
	 */
	clear_bit(GD_NEED_PART_SCAN, &disk->state);
	if (!(mode & BLK_OPEN_EXCL))
		bd_abort_claiming(disk->part0, disk_scan_partitions);
	return ret;
}

/*
 * add_disk_final - gendisk 등록의 마무리 단계
 * @disk: 등록할 gendisk
 *
 * sysfs/proc 노출, uevent 발생, 파티션 스캔, queue limits 적용을 수행한다.
 * NVMe 관점에서는 이 단계까지 완료되어야 /dev/nvmeXnY 노드가 생성되고
 * 상위 파일시스템이 I/O를 발행할 수 있다.
 * 호출 경로: add_disk_fwnode -> add_disk_final
 */
static void add_disk_final(struct gendisk *disk)
{
	struct device *ddev = disk_to_dev(disk);

	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		/* Make sure the first partition scan will be proceed */
		if (get_capacity(disk) && disk_has_partscan(disk))
			set_bit(GD_NEED_PART_SCAN, &disk->state);

		bdev_add(disk->part0, ddev->devt);
		if (get_capacity(disk))
			disk_scan_partitions(disk, BLK_OPEN_READ);

		/*
		 * Announce the disk and partitions after all partitions are
		 * created. (for hidden disks uevents remain suppressed forever)
		 */
		dev_set_uevent_suppress(ddev, 0);
		disk_uevent(disk, KOBJ_ADD);
	}

	blk_apply_bdi_limits(disk->bdi, &disk->queue->limits);
	disk_add_events(disk);
	set_bit(GD_ADDED, &disk->state);
}

/*
 * __add_disk - gendisk를 커널 장치 계층에 등록
 * @parent: 부모 장치 (NVMe 컨트롤러의 device)
 * @disk: 등록할 gendisk
 * @groups: 추가 sysfs 속성 그룹
 * @fwnode: 연결할 firmware node
 *
 * blk-mq 기반 NVMe 드라이버는 disk->fops->submit_bio를 제공하지 않는다.
 * 대신 request_queue의 tag_set을 통해 blk_mq_submit_bio 경로가 활성화된다.
 * 이 함수는 major/minor 할당, device_add, queue 등록, bdi 등록을 순차 수행한다.
 * 호출 경로: add_disk_fwnode -> __add_disk -> blk_register_queue
 */
static int __add_disk(struct device *parent, struct gendisk *disk,
		      const struct attribute_group **groups,
		      struct fwnode_handle *fwnode)

{
	struct device *ddev = disk_to_dev(disk);
	int ret;

	if (WARN_ON_ONCE(bdev_nr_sectors(disk->part0) > BLK_DEV_MAX_SECTORS))
		return -EINVAL;

	if (queue_is_mq(disk->queue)) { /* NVMe는 blk-mq 경로를 사용하므로 submit_bio 미제공 */
		/*
		 * ->submit_bio and ->poll_bio are bypassed for blk-mq drivers.
		 */
		if (disk->fops->submit_bio || disk->fops->poll_bio)
			return -EINVAL;
	} else {
		if (!disk->fops->submit_bio)
			return -EINVAL;
		bdev_set_flag(disk->part0, BD_HAS_SUBMIT_BIO);
	}

	/*
	 * If the driver provides an explicit major number it also must provide
	 * the number of minors numbers supported, and those will be used to
	 * setup the gendisk.
	 * Otherwise just allocate the device numbers for both the whole device
	 * and all partitions from the extended dev_t space.
	 */
	ret = -EINVAL;
	if (disk->major) {
		if (WARN_ON(!disk->minors))
			goto out;

		if (disk->minors > DISK_MAX_PARTS) {
			pr_err("block: can't allocate more than %d partitions\n",
				DISK_MAX_PARTS);
			disk->minors = DISK_MAX_PARTS;
		}
		if (disk->first_minor > MINORMASK ||
		    disk->minors > MINORMASK + 1 ||
		    disk->first_minor + disk->minors > MINORMASK + 1)
			goto out;
	} else {
		if (WARN_ON(disk->minors))
			goto out;

		ret = blk_alloc_ext_minor();
		if (ret < 0)
			goto out;
		disk->major = BLOCK_EXT_MAJOR;
		disk->first_minor = ret;
	}

	/* delay uevents, until we scanned partition table */
	dev_set_uevent_suppress(ddev, 1);

	ddev->parent = parent;
	ddev->groups = groups;
	dev_set_name(ddev, "%s", disk->disk_name);
	if (fwnode)
		device_set_node(ddev, fwnode);
	if (!(disk->flags & GENHD_FL_HIDDEN))
		ddev->devt = MKDEV(disk->major, disk->first_minor);
	ret = device_add(ddev);
	if (ret)
		goto out_free_ext_minor;

	ret = disk_alloc_events(disk);
	if (ret)
		goto out_device_del;

	ret = sysfs_create_link(block_depr, &ddev->kobj,
				kobject_name(&ddev->kobj));
	if (ret)
		goto out_device_del;

	/*
	 * avoid probable deadlock caused by allocating memory with
	 * GFP_KERNEL in runtime_resume callback of its all ancestor
	 * devices
	 */
	pm_runtime_set_memalloc_noio(ddev, true);

	disk->part0->bd_holder_dir = /* NVMe 디스크의 holder 디렉터리(예: dm/lvm 마운트 정보) */
		kobject_create_and_add("holders", &ddev->kobj);
	if (!disk->part0->bd_holder_dir) {
		ret = -ENOMEM;
		goto out_del_block_link;
	}
	disk->slave_dir = kobject_create_and_add("slaves", &ddev->kobj);
	if (!disk->slave_dir) {
		ret = -ENOMEM;
		goto out_put_holder_dir;
	}

	ret = blk_register_queue(disk); /* NVMe request_queue를 sysfs /sys/block/nvmeXnY/queue에 등록 */
	if (ret)
		goto out_put_slave_dir;

	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		ret = bdi_register(disk->bdi, "%u:%u", /* NVMe 네임스페이스의 writeback 인프라 등록 */
				   disk->major, disk->first_minor);
		if (ret)
			goto out_unregister_queue;
		bdi_set_owner(disk->bdi, ddev);
		ret = sysfs_create_link(&ddev->kobj,
					&disk->bdi->dev->kobj, "bdi");
		if (ret)
			goto out_unregister_bdi;
	} else {
		/*
		 * Even if the block_device for a hidden gendisk is not
		 * registered, it needs to have a valid bd_dev so that the
		 * freeing of the dynamic major works.
		 */
		disk->part0->bd_dev = MKDEV(disk->major, disk->first_minor);
	}
	return 0;

out_unregister_bdi:
	if (!(disk->flags & GENHD_FL_HIDDEN))
		bdi_unregister(disk->bdi);
out_unregister_queue:
	blk_unregister_queue(disk);
	rq_qos_exit(disk->queue);
out_put_slave_dir:
	kobject_put(disk->slave_dir);
	disk->slave_dir = NULL;
out_put_holder_dir:
	kobject_put(disk->part0->bd_holder_dir);
out_del_block_link:
	sysfs_remove_link(block_depr, dev_name(ddev));
	pm_runtime_set_memalloc_noio(ddev, false);
out_device_del:
	device_del(ddev);
out_free_ext_minor:
	if (disk->major == BLOCK_EXT_MAJOR)
		blk_free_ext_minor(disk->first_minor);
out:
	return ret;
}

/*
 * add_disk_fwnode - firmware node를 가진 gendisk 등록
 * @parent: 부모 장치
 * @disk: 등록할 gendisk
 * @groups: 추가 sysfs 속성 그룹
 * @fwnode: firmware node
 *
 * blk-mq 드라이버의 경우 tag_set->update_nr_hwq_lock을 read 잡은 상태에서
 * __add_disk을 호출한다. NVMe 드라이버가 동적으로 hw queue 개수를 변경할 때
 * 이 락이 등록 경로와 동기화한다. add_disk_final은 락 밖에서 호출된다.
 */
/**
 * add_disk_fwnode - add disk information to kernel list with fwnode
 * @parent: parent device for the disk
 * @disk: per-device partitioning information
 * @groups: Additional per-device sysfs groups
 * @fwnode: attached disk fwnode
 *
 * This function registers the partitioning information in @disk
 * with the kernel. Also attach a fwnode to the disk device.
 */
int __must_check add_disk_fwnode(struct device *parent, struct gendisk *disk,
				 const struct attribute_group **groups,
				 struct fwnode_handle *fwnode)
{
	struct blk_mq_tag_set *set;
	unsigned int memflags;
	int ret;

	if (queue_is_mq(disk->queue)) {
		set = disk->queue->tag_set; /* NVMe blk_mq_tag_set, nr_hw_queues 변경과 동기화 */
		memflags = memalloc_noio_save();
		down_read(&set->update_nr_hwq_lock); /* nvme_update_nr_queues 등과 경쟁 방지 */
		ret = __add_disk(parent, disk, groups, fwnode);
		up_read(&set->update_nr_hwq_lock);
		memalloc_noio_restore(memflags);
	} else {
		ret = __add_disk(parent, disk, groups, fwnode);
	}

	/*
	 * add_disk_final() needn't to read `nr_hw_queues`, so move it out
	 * of read lock `set->update_nr_hwq_lock` for avoiding unnecessary
	 * lock dependency on `disk->open_mutex` from scanning partition.
	 */
	if (!ret)
		add_disk_final(disk);
	return ret;
}
EXPORT_SYMBOL_GPL(add_disk_fwnode);

/*
 * device_add_disk - gendisk를 커널에 등록 (레거시 진입점)
 * @parent: 부모 장치
 * @disk: 등록할 gendisk
 * @groups: 추가 sysfs 속성 그룹
 *
 * NVMe 드라이버가 nvme_alloc_ns_disk 또는 유사 경로에서 이 함수를 호출하여
 * 네임스페이스를 블록 서브시스템에 노출시킨다.
 * 호출 경로(추정): nvme_revalidate_disk -> device_add_disk
 */
/**
 * device_add_disk - add disk information to kernel list
 * @parent: parent device for the disk
 * @disk: per-device partitioning information
 * @groups: Additional per-device sysfs groups
 *
 * This function registers the partitioning information in @disk
 * with the kernel.
 */
int __must_check device_add_disk(struct device *parent, struct gendisk *disk,
				 const struct attribute_group **groups)
{
	return add_disk_fwnode(parent, disk, groups, NULL);
}
EXPORT_SYMBOL(device_add_disk);

static void blk_report_disk_dead(struct gendisk *disk, bool surprise)
{
	struct block_device *bdev;
	unsigned long idx;

	/*
	 * On surprise disk removal, bdev_mark_dead() may call into file
	 * systems below. Make it clear that we're expecting to not hold
	 * disk->open_mutex.
	 */
	lockdep_assert_not_held(&disk->open_mutex);

	rcu_read_lock();
	xa_for_each(&disk->part_tbl, idx, bdev) {
		if (!kobject_get_unless_zero(&bdev->bd_device.kobj))
			continue;
		rcu_read_unlock();

		bdev_mark_dead(bdev, surprise);

		put_device(&bdev->bd_device);
		rcu_read_lock();
	}
	rcu_read_unlock();
}

static bool __blk_mark_disk_dead(struct gendisk *disk)
{
	/*
	 * Fail any new I/O.
	 */
	if (test_and_set_bit(GD_DEAD, &disk->state))
		return false;

	if (test_bit(GD_OWNS_QUEUE, &disk->state))
		blk_queue_flag_set(QUEUE_FLAG_DYING, disk->queue);

	/*
	 * Stop buffered writers from dirtying pages that can't be written out.
	 */
	set_capacity(disk, 0);

	/*
	 * Prevent new I/O from crossing bio_queue_enter().
	 */
	return blk_queue_start_drain(disk->queue);
}

/*
 * blk_mark_disk_dead - 디스크를 죽은 상태로 표시하고 새 I/O 차단
 * @disk: 대상 gendisk
 *
 * NVMe 컨트롤러가 갑작스럽게 제거되거나 치명적 오류 발생 시 호출된다.
 * GD_DEAD 플래그를 설정하고 QUEUE_FLAG_DYING를 request_queue에 설정하여
 * bio_queue_enter() 이후의 새 I/O를 차단한다. 기존 진행 중인 I/O는
 * nvme_timeout/nvme_cancel_request 등을 통해 하위에서 완료 처리된다.
 */
/**
 * blk_mark_disk_dead - mark a disk as dead
 * @disk: disk to mark as dead
 *
 * Mark as disk as dead (e.g. surprise removed) and don't accept any new I/O
 * to this disk.
 */
void blk_mark_disk_dead(struct gendisk *disk)
{
	__blk_mark_disk_dead(disk);
	blk_report_disk_dead(disk, true);
}
EXPORT_SYMBOL_GPL(blk_mark_disk_dead);

/*
 * __del_gendisk - gendisk와 관련 자원을 제거
 * @disk: 제거할 gendisk
 *
 * NVMe 네임스페이스가 제거될 때 파티션을 drop하고, queue를 drain/freeze한 뒤
 * request_queue를 unregister한다. blk_mq_freeze_queue_wait으로 모든 진행 중인
 * I/O가 완료될 때까지 대기한다. 이 함수는 del_gendisk의 실제 구현체이다.
 * 호출 경로: del_gendisk -> __del_gendisk -> blk_unregister_queue
 */
static void __del_gendisk(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	struct block_device *part;
	unsigned long idx;
	bool start_drain;

	might_sleep();

	if (WARN_ON_ONCE(!disk_live(disk) && !(disk->flags & GENHD_FL_HIDDEN)))
		return;

	disk_del_events(disk);

	/*
	 * Prevent new openers by unlinked the bdev inode.
	 */
	mutex_lock(&disk->open_mutex);
	xa_for_each(&disk->part_tbl, idx, part)
		bdev_unhash(part);
	mutex_unlock(&disk->open_mutex);

	/*
	 * Tell the file system to write back all dirty data and shut down if
	 * it hasn't been notified earlier.
	 */
	if (!test_bit(GD_DEAD, &disk->state))
		blk_report_disk_dead(disk, false);

	/*
	 * Drop all partitions now that the disk is marked dead.
	 */
	mutex_lock(&disk->open_mutex);
	start_drain = __blk_mark_disk_dead(disk);
	if (start_drain)
		blk_freeze_acquire_lock(q);
	xa_for_each_start(&disk->part_tbl, idx, part, 1)
		drop_partition(part);
	mutex_unlock(&disk->open_mutex);

	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		sysfs_remove_link(&disk_to_dev(disk)->kobj, "bdi");

		/*
		 * Unregister bdi before releasing device numbers (as they can
		 * get reused and we'd get clashes in sysfs).
		 */
		bdi_unregister(disk->bdi);
	}

	blk_unregister_queue(disk);

	kobject_put(disk->part0->bd_holder_dir);
	kobject_put(disk->slave_dir);
	disk->slave_dir = NULL;

	part_stat_set_all(disk->part0, 0);
	disk->part0->bd_stamp = 0;
	sysfs_remove_link(block_depr, dev_name(disk_to_dev(disk)));
	pm_runtime_set_memalloc_noio(disk_to_dev(disk), false);
	device_del(disk_to_dev(disk));

	blk_mq_freeze_queue_wait(q);

	blk_throtl_cancel_bios(disk);

	blk_sync_queue(q);
	blk_flush_integrity();

	if (queue_is_mq(q)) /* NVMe: blk_mq_cancel_work_sync로 태그 재활용 지연 작업 취소 */
		blk_mq_cancel_work_sync(q); /* NVMe timeout/work 재스케줄링 정리 */

	rq_qos_exit(q);

	/*
	 * If the disk does not own the queue, allow using passthrough requests
	 * again.  Else leave the queue frozen to fail all I/O.
	 */
	if (!test_bit(GD_OWNS_QUEUE, &disk->state))
		__blk_mq_unfreeze_queue(q, true);
	else if (queue_is_mq(q))
		blk_mq_exit_queue(q);

	if (start_drain)
		blk_unfreeze_release_lock(q);
}

static void disable_elv_switch(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;
	WARN_ON_ONCE(!queue_is_mq(q));

	down_write(&set->update_nr_hwq_lock);
	blk_queue_flag_set(QUEUE_FLAG_NO_ELV_SWITCH, q);
	up_write(&set->update_nr_hwq_lock);
}

/*
 * del_gendisk - gendisk 제거의 공개 인터페이스
 * @disk: 제거할 gendisk
 *
 * blk-mq 기반 NVMe 드라이버는 tag_set의 update_nr_hwq_lock 아래 __del_gendisk을
 * 호출한다. 이는 nvme_update_nr_queues 등에서 hw queue 수가 변경되는 동안
 * gendisk 제거와 경쟁하지 않도록 한다.
 */
/**
 * del_gendisk - remove the gendisk
 * @disk: the struct gendisk to remove
 *
 * Removes the gendisk and all its associated resources. This deletes the
 * partitions associated with the gendisk, and unregisters the associated
 * request_queue.
 *
 * This is the counter to the respective device_add_disk() call.
 *
 * The final removal of the struct gendisk happens when its refcount reaches 0
 * with put_disk(), which should be called after del_gendisk(), if
 * device_add_disk() was used.
 *
 * Drivers exist which depend on the release of the gendisk to be synchronous,
 * it should not be deferred.
 *
 * Context: can sleep
 */
void del_gendisk(struct gendisk *disk)
{
	struct blk_mq_tag_set *set;
	unsigned int memflags;

	if (!queue_is_mq(disk->queue)) { /* NVMe는 항상 blk-mq이므로 else 분기만 사용 */
		__del_gendisk(disk);
	} else {
		set = disk->queue->tag_set;

		disable_elv_switch(disk->queue); /* 삭제 중 I/O 스케줄러 교체 방지(추정) */

		memflags = memalloc_noio_save();
		down_read(&set->update_nr_hwq_lock);
		__del_gendisk(disk);
		up_read(&set->update_nr_hwq_lock);
		memalloc_noio_restore(memflags);
	}
}
EXPORT_SYMBOL(del_gendisk);

/*
 * invalidate_disk - 디스크의 버퍼/페이지 캐시를 무효화
 * @disk: 대상 gendisk
 *
 * NVMe 네임스페이스를 재사용하기 전에 기존 캐시와 용량 정보를 초기화한다.
 */
/**
 * invalidate_disk - invalidate the disk
 * @disk: the struct gendisk to invalidate
 *
 * A helper to invalidates the disk. It will clean the disk's associated
 * buffer/page caches and reset its internal states so that the disk
 * can be reused by the drivers.
 *
 * Context: can sleep
 */
void invalidate_disk(struct gendisk *disk)
{
	struct block_device *bdev = disk->part0;

	invalidate_bdev(bdev);
	bdev->bd_mapping->wb_err = 0;
	set_capacity(disk, 0);
}
EXPORT_SYMBOL(invalidate_disk);

/* sysfs access to bad-blocks list. */
static ssize_t disk_badblocks_show(struct device *dev,
					struct device_attribute *attr,
					char *page)
{
	struct gendisk *disk = dev_to_disk(dev);

	if (!disk->bb)
		return sysfs_emit(page, "\n");

	return badblocks_show(disk->bb, page, 0);
}

static ssize_t disk_badblocks_store(struct device *dev,
					struct device_attribute *attr,
					const char *page, size_t len)
{
	struct gendisk *disk = dev_to_disk(dev);

	if (!disk->bb)
		return -ENXIO;

	return badblocks_store(disk->bb, page, len, 0);
}

#ifdef CONFIG_BLOCK_LEGACY_AUTOLOAD
static bool blk_probe_dev(dev_t devt)
{
	unsigned int major = MAJOR(devt);
	struct blk_major_name **n;

	mutex_lock(&major_names_lock);
	for (n = &major_names[major_to_index(major)]; *n; n = &(*n)->next) {
		if ((*n)->major == major && (*n)->probe) {
			(*n)->probe(devt);
			mutex_unlock(&major_names_lock);
			return true;
		}
	}
	mutex_unlock(&major_names_lock);
	return false;
}

void blk_request_module(dev_t devt)
{
	int error;

	if (blk_probe_dev(devt))
		return;

	error = request_module("block-major-%d-%d", MAJOR(devt), MINOR(devt));
	/* Make old-style 2.4 aliases work */
	if (error > 0)
		error = request_module("block-major-%d", MAJOR(devt));
	if (!error)
		blk_probe_dev(devt);
}
#endif /* CONFIG_BLOCK_LEGACY_AUTOLOAD */

#ifdef CONFIG_PROC_FS
/* iterator */
static void *disk_seqf_start(struct seq_file *seqf, loff_t *pos)
{
	loff_t skip = *pos;
	struct class_dev_iter *iter;
	struct device *dev;

	iter = kmalloc_obj(*iter);
	if (!iter)
		return ERR_PTR(-ENOMEM);

	seqf->private = iter;
	class_dev_iter_init(iter, &block_class, NULL, &disk_type);
	do {
		dev = class_dev_iter_next(iter);
		if (!dev)
			return NULL;
	} while (skip--);

	return dev_to_disk(dev);
}

static void *disk_seqf_next(struct seq_file *seqf, void *v, loff_t *pos)
{
	struct device *dev;

	(*pos)++;
	dev = class_dev_iter_next(seqf->private);
	if (dev)
		return dev_to_disk(dev);

	return NULL;
}

static void disk_seqf_stop(struct seq_file *seqf, void *v)
{
	struct class_dev_iter *iter = seqf->private;

	/* stop is called even after start failed :-( */
	if (iter) {
		class_dev_iter_exit(iter);
		kfree(iter);
		seqf->private = NULL;
	}
}

static void *show_partition_start(struct seq_file *seqf, loff_t *pos)
{
	void *p;

	p = disk_seqf_start(seqf, pos);
	if (!IS_ERR_OR_NULL(p) && !*pos)
		seq_puts(seqf, "major minor  #blocks  name\n\n");
	return p;
}

static int show_partition(struct seq_file *seqf, void *v)
{
	struct gendisk *sgp = v;
	struct block_device *part;
	unsigned long idx;

	if (!get_capacity(sgp) || (sgp->flags & GENHD_FL_HIDDEN))
		return 0;

	rcu_read_lock();
	xa_for_each(&sgp->part_tbl, idx, part) {
		if (!bdev_nr_sectors(part))
			continue;
		seq_printf(seqf, "%4d  %7d %10llu %pg\n",
			   MAJOR(part->bd_dev), MINOR(part->bd_dev),
			   bdev_nr_sectors(part) >> 1, part);
	}
	rcu_read_unlock();
	return 0;
}

static const struct seq_operations partitions_op = {
	.start	= show_partition_start,
	.next	= disk_seqf_next,
	.stop	= disk_seqf_stop,
	.show	= show_partition
};
#endif

/*
 * genhd_device_init - 블록 장치 클래스 초기화
 *
 * 부팅 시 block_class를 등록하고 /sys/block 디렉터리를 만든다.
 * 이 초기화 이후에 NVMe 드라이버가 로드되어 gendisk를 등록할 수 있다.
 */
static int __init genhd_device_init(void)
{
	int error;

	error = class_register(&block_class);
	if (unlikely(error))
		return error;
	blk_dev_init();

	register_blkdev(BLOCK_EXT_MAJOR, "blkext");

	/* create top-level block dir */
	block_depr = kobject_create_and_add("block", NULL);
	return 0;
}

subsys_initcall(genhd_device_init);

static ssize_t disk_range_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%d\n", disk->minors);
}

static ssize_t disk_ext_range_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%d\n",
		(disk->flags & GENHD_FL_NO_PART) ? 1 : DISK_MAX_PARTS);
}

static ssize_t disk_removable_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%d\n",
		       (disk->flags & GENHD_FL_REMOVABLE ? 1 : 0));
}

static ssize_t disk_hidden_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%d\n",
		       (disk->flags & GENHD_FL_HIDDEN ? 1 : 0));
}

static ssize_t disk_ro_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%d\n", get_disk_ro(disk) ? 1 : 0);
}

ssize_t part_size_show(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", bdev_nr_sectors(dev_to_bdev(dev)));
}

/*
 * part_stat_show - sysfs를 통한 파티션 I/O 통계 출력
 * @dev: 장치
 * @attr: 속성
 * @buf: 출력 버퍼
 *
 * /sys/block/nvmeXnY/stat 등에서 읽히는 값을 생성한다.
 * read/write/discard/flush 별 ios, merges, sectors, nsecs를 집계하며,
 * NVMe 큐의 doorbell 왕복 지연, PRP/SGL 준비 시간 등이 이 통계에 반영된다.
 */
ssize_t part_stat_show(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	struct block_device *bdev = dev_to_bdev(dev);
	struct disk_stats stat;
	unsigned int inflight;

	inflight = bdev_count_inflight(bdev);
	if (inflight) {
		part_stat_lock();
		update_io_ticks(bdev, jiffies, true);
		part_stat_unlock();
	}
	part_stat_read_all(bdev, &stat);
	return sysfs_emit(buf,
		"%8lu %8lu %8llu %8u "
		"%8lu %8lu %8llu %8u "
		"%8u %8u %8u "
		"%8lu %8lu %8llu %8u "
		"%8lu %8u"
		"\n",
		stat.ios[STAT_READ],
		stat.merges[STAT_READ],
		(unsigned long long)stat.sectors[STAT_READ],
		(unsigned int)div_u64(stat.nsecs[STAT_READ], NSEC_PER_MSEC),
		stat.ios[STAT_WRITE],
		stat.merges[STAT_WRITE],
		(unsigned long long)stat.sectors[STAT_WRITE],
		(unsigned int)div_u64(stat.nsecs[STAT_WRITE], NSEC_PER_MSEC),
		inflight,
		jiffies_to_msecs(stat.io_ticks),
		(unsigned int)div_u64(stat.nsecs[STAT_READ] +
				      stat.nsecs[STAT_WRITE] +
				      stat.nsecs[STAT_DISCARD] +
				      stat.nsecs[STAT_FLUSH],
						NSEC_PER_MSEC),
		stat.ios[STAT_DISCARD],
		stat.merges[STAT_DISCARD],
		(unsigned long long)stat.sectors[STAT_DISCARD],
		(unsigned int)div_u64(stat.nsecs[STAT_DISCARD], NSEC_PER_MSEC),
		stat.ios[STAT_FLUSH],
		(unsigned int)div_u64(stat.nsecs[STAT_FLUSH], NSEC_PER_MSEC));
}

/*
 * part_inflight_show - sysfs를 통한 진행 중 I/O 개수 출력
 * @dev: 장치
 * @attr: 속성
 * @buf: 출력 버퍼
 *
 * /sys/block/nvmeXnY/inflight에서 확인되는, NVMe SQ에 submit되어 CID를
 * 할당받았으나 아직 CQ에서 완료되지 않은 요청 수를 read/write별로 반환한다.
 */
/*
 * Show the number of IOs issued to driver.
 * For bio-based device, started from bdev_start_io_acct();
 * For rq-based device, started from blk_mq_start_request();
 */
ssize_t part_inflight_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct block_device *bdev = dev_to_bdev(dev);
	struct request_queue *q = bdev_get_queue(bdev);
	unsigned int inflight[2] = {0};

	bdev_count_inflight_rw(bdev, inflight, queue_is_mq(q));

	return sysfs_emit(buf, "%8u %8u\n", inflight[READ], inflight[WRITE]);
}

static ssize_t disk_capability_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	dev_warn_once(dev, "the capability attribute has been deprecated.\n");
	return sysfs_emit(buf, "0\n");
}

static ssize_t disk_alignment_offset_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%d\n", bdev_alignment_offset(disk->part0));
}

static ssize_t disk_discard_alignment_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%d\n", bdev_alignment_offset(disk->part0));
}

static ssize_t diskseq_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);

	return sysfs_emit(buf, "%llu\n", disk->diskseq);
}

static ssize_t partscan_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", disk_has_partscan(dev_to_disk(dev)));
}

static DEVICE_ATTR(range, 0444, disk_range_show, NULL);
static DEVICE_ATTR(ext_range, 0444, disk_ext_range_show, NULL);
static DEVICE_ATTR(removable, 0444, disk_removable_show, NULL);
static DEVICE_ATTR(hidden, 0444, disk_hidden_show, NULL);
static DEVICE_ATTR(ro, 0444, disk_ro_show, NULL);
static DEVICE_ATTR(size, 0444, part_size_show, NULL);
static DEVICE_ATTR(alignment_offset, 0444, disk_alignment_offset_show, NULL);
static DEVICE_ATTR(discard_alignment, 0444, disk_discard_alignment_show, NULL);
static DEVICE_ATTR(capability, 0444, disk_capability_show, NULL);
static DEVICE_ATTR(stat, 0444, part_stat_show, NULL);
static DEVICE_ATTR(inflight, 0444, part_inflight_show, NULL);
static DEVICE_ATTR(badblocks, 0644, disk_badblocks_show, disk_badblocks_store);
static DEVICE_ATTR(diskseq, 0444, diskseq_show, NULL);
static DEVICE_ATTR(partscan, 0444, partscan_show, NULL);

#ifdef CONFIG_FAIL_MAKE_REQUEST
ssize_t part_fail_show(struct device *dev,
		       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
		       bdev_test_flag(dev_to_bdev(dev), BD_MAKE_IT_FAIL));
}

ssize_t part_fail_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int i;

	if (count > 0 && sscanf(buf, "%d", &i) > 0) {
		if (i)
			bdev_set_flag(dev_to_bdev(dev), BD_MAKE_IT_FAIL);
		else
			bdev_clear_flag(dev_to_bdev(dev), BD_MAKE_IT_FAIL);
	}
	return count;
}

static struct device_attribute dev_attr_fail =
	__ATTR(make-it-fail, 0644, part_fail_show, part_fail_store);
#endif /* CONFIG_FAIL_MAKE_REQUEST */

#ifdef CONFIG_FAIL_IO_TIMEOUT
static struct device_attribute dev_attr_fail_timeout =
	__ATTR(io-timeout-fail, 0644, part_timeout_show, part_timeout_store);
#endif

static struct attribute *disk_attrs[] = {
	&dev_attr_range.attr,
	&dev_attr_ext_range.attr,
	&dev_attr_removable.attr,
	&dev_attr_hidden.attr,
	&dev_attr_ro.attr,
	&dev_attr_size.attr,
	&dev_attr_alignment_offset.attr,
	&dev_attr_discard_alignment.attr,
	&dev_attr_capability.attr,
	&dev_attr_stat.attr,
	&dev_attr_inflight.attr,
	&dev_attr_badblocks.attr,
	&dev_attr_events.attr,
	&dev_attr_events_async.attr,
	&dev_attr_events_poll_msecs.attr,
	&dev_attr_diskseq.attr,
	&dev_attr_partscan.attr,
#ifdef CONFIG_FAIL_MAKE_REQUEST
	&dev_attr_fail.attr,
#endif
#ifdef CONFIG_FAIL_IO_TIMEOUT
	&dev_attr_fail_timeout.attr,
#endif
	NULL
};

static umode_t disk_visible(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, typeof(*dev), kobj);
	struct gendisk *disk = dev_to_disk(dev);

	if (a == &dev_attr_badblocks.attr && !disk->bb)
		return 0;
	return a->mode;
}

static struct attribute_group disk_attr_group = {
	.attrs = disk_attrs,
	.is_visible = disk_visible,
};

static const struct attribute_group *disk_attr_groups[] = {
	&disk_attr_group,
#ifdef CONFIG_BLK_DEV_IO_TRACE
	&blk_trace_attr_group,
#endif
#ifdef CONFIG_BLK_DEV_INTEGRITY
	&blk_integrity_attr_group,
#endif
	NULL
};

/*
 * disk_release - gendisk의 모든 자원을 해제
 * @dev: 디스크를 나타내는 장치
 *
 * gendisk의 refcount가 0이 되면 호출된다. blk-mq 기반 NVMe 드라이버는
 * request_queue의 refcount도 여기서 0으로 만들어 queue를 해제한다.
 * tag_set은 put_disk 이전에 해제되어야 한다(드라이버 문서 참조).
 */
/**
 * disk_release - releases all allocated resources of the gendisk
 * @dev: the device representing this disk
 *
 * This function releases all allocated resources of the gendisk.
 *
 * Drivers which used device_add_disk() have a gendisk with a request_queue
 * assigned. Since the request_queue sits on top of the gendisk for these
 * drivers we also call blk_put_queue() for them, and we expect the
 * request_queue refcount to reach 0 at this point, and so the request_queue
 * will also be freed prior to the disk.
 *
 * Context: can sleep
 */
static void disk_release(struct device *dev)
{
	struct gendisk *disk = dev_to_disk(dev);

	might_sleep();
	WARN_ON_ONCE(disk_live(disk));

	blk_trace_remove(disk->queue);

	/*
	 * To undo the all initialization from blk_mq_init_allocated_queue in
	 * case of a probe failure where add_disk is never called we have to
	 * call blk_mq_exit_queue here. We can't do this for the more common
	 * teardown case (yet) as the tagset can be gone by the time the disk
	 * is released once it was added.
	 */
	if (queue_is_mq(disk->queue) && /* NVMe probe 실패 시 add_disk 전 queue 정리 */
	    test_bit(GD_OWNS_QUEUE, &disk->state) &&
	    !test_bit(GD_ADDED, &disk->state))
		blk_mq_exit_queue(disk->queue);

	blkcg_exit_disk(disk);

	bioset_exit(&disk->bio_split);

	disk_release_events(disk);
	kfree(disk->random);
	disk_free_zone_resources(disk);
	xa_destroy(&disk->part_tbl);

	kobject_put(&disk->queue_kobj);
	disk->queue->disk = NULL;
	blk_put_queue(disk->queue);

	if (test_bit(GD_ADDED, &disk->state) && disk->fops->free_disk)
		disk->fops->free_disk(disk);

	bdev_drop(disk->part0);	/* frees the disk */
}

static int block_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct gendisk *disk = dev_to_disk(dev);

	return add_uevent_var(env, "DISKSEQ=%llu", disk->diskseq);
}

const struct class block_class = {
	.name		= "block",
	.dev_uevent	= block_uevent,
};

static char *block_devnode(const struct device *dev, umode_t *mode,
			   kuid_t *uid, kgid_t *gid)
{
	struct gendisk *disk = dev_to_disk(dev);

	if (disk->fops->devnode)
		return disk->fops->devnode(disk, mode);
	return NULL;
}

const struct device_type disk_type = {
	.name		= "disk",
	.groups		= disk_attr_groups,
	.release	= disk_release,
	.devnode	= block_devnode,
};

#ifdef CONFIG_PROC_FS
/*
 * aggregate disk stat collector.  Uses the same stats that the sysfs
 * entries do, above, but makes them available through one seq_file.
 *
 * The output looks suspiciously like /proc/partitions with a bunch of
 * extra fields.
 */
static int diskstats_show(struct seq_file *seqf, void *v)
{
	struct gendisk *gp = v;
	struct block_device *hd;
	unsigned int inflight;
	struct disk_stats stat;
	unsigned long idx;

	/*
	if (&disk_to_dev(gp)->kobj.entry == block_class.devices.next)
		seq_puts(seqf,	"major minor name"
				"     rio rmerge rsect ruse wio wmerge "
				"wsect wuse running use aveq"
				"\n\n");
	*/

	rcu_read_lock();
	xa_for_each(&gp->part_tbl, idx, hd) {
		if (bdev_is_partition(hd) && !bdev_nr_sectors(hd))
			continue;

		inflight = bdev_count_inflight(hd);
		if (inflight) {
			part_stat_lock();
			update_io_ticks(hd, jiffies, true);
			part_stat_unlock();
		}
		part_stat_read_all(hd, &stat);
		seq_put_decimal_ull_width(seqf, "",  MAJOR(hd->bd_dev), 4);
		seq_put_decimal_ull_width(seqf, " ", MINOR(hd->bd_dev), 7);
		seq_printf(seqf, " %pg", hd);
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_READ]);
		seq_put_decimal_ull(seqf, " ", stat.merges[STAT_READ]);
		seq_put_decimal_ull(seqf, " ", stat.sectors[STAT_READ]);
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_READ],
								     NSEC_PER_MSEC));
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_WRITE]);
		seq_put_decimal_ull(seqf, " ", stat.merges[STAT_WRITE]);
		seq_put_decimal_ull(seqf, " ", stat.sectors[STAT_WRITE]);
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_WRITE],
								     NSEC_PER_MSEC));
		seq_put_decimal_ull(seqf, " ", inflight);
		seq_put_decimal_ull(seqf, " ", jiffies_to_msecs(stat.io_ticks));
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_READ] +
								     stat.nsecs[STAT_WRITE] +
								     stat.nsecs[STAT_DISCARD] +
								     stat.nsecs[STAT_FLUSH],
								     NSEC_PER_MSEC));
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_DISCARD]);
		seq_put_decimal_ull(seqf, " ", stat.merges[STAT_DISCARD]);
		seq_put_decimal_ull(seqf, " ", stat.sectors[STAT_DISCARD]);
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_DISCARD],
								     NSEC_PER_MSEC));
		seq_put_decimal_ull(seqf, " ", stat.ios[STAT_FLUSH]);
		seq_put_decimal_ull(seqf, " ", (unsigned int)div_u64(stat.nsecs[STAT_FLUSH],
								     NSEC_PER_MSEC));
		seq_putc(seqf, '\n');
	}
	rcu_read_unlock();

	return 0;
}

static const struct seq_operations diskstats_op = {
	.start	= disk_seqf_start,
	.next	= disk_seqf_next,
	.stop	= disk_seqf_stop,
	.show	= diskstats_show
};

static int __init proc_genhd_init(void)
{
	proc_create_seq("diskstats", 0, NULL, &diskstats_op);
	proc_create_seq("partitions", 0, NULL, &partitions_op);
	return 0;
}
module_init(proc_genhd_init);
#endif /* CONFIG_PROC_FS */

dev_t part_devt(struct gendisk *disk, u8 partno)
{
	struct block_device *part;
	dev_t devt = 0;

	rcu_read_lock();
	part = xa_load(&disk->part_tbl, partno);
	if (part)
		devt = part->bd_dev;
	rcu_read_unlock();

	return devt;
}

/*
 * __alloc_disk_node - 지정 노드에 gendisk 할당
 * @q: 연결할 request_queue (NVMe의 blk_mq_tag_set에서 파생)
 * @node_id: 할당할 NUMA 노드
 * @lkclass: lockdep 클래스
 *
 * NVMe 드라이버가 네임스페이스당 하나의 gendisk를 생성할 때 사용한다.
 * disk->queue에 NVMe request_queue를 연결하고, part0 block_device를 생성하며,
 * cgroup, zone, random seed 등의 하위 자원을 초기화한다.
 */
struct gendisk *__alloc_disk_node(struct request_queue *q, int node_id,
		struct lock_class_key *lkclass)
{
	struct gendisk *disk;

	disk = kzalloc_node(sizeof(struct gendisk), GFP_KERNEL, node_id);
	if (!disk)
		return NULL;

	if (bioset_init(&disk->bio_split, BIO_POOL_SIZE, 0, 0))
		goto out_free_disk;

	disk->bdi = bdi_alloc(node_id);
	if (!disk->bdi)
		goto out_free_bioset;

	/* bdev_alloc() might need the queue, set before the first call */
	disk->queue = q;

	disk->part0 = bdev_alloc(disk, 0);
	if (!disk->part0)
		goto out_free_bdi;

	disk->node_id = node_id;
	mutex_init(&disk->open_mutex);
	xa_init(&disk->part_tbl);
	if (xa_insert(&disk->part_tbl, 0, disk->part0, GFP_KERNEL))
		goto out_destroy_part_tbl;

	if (blkcg_init_disk(disk))
		goto out_erase_part0;

	disk_init_zone_resources(disk);
	rand_initialize_disk(disk);
	disk_to_dev(disk)->class = &block_class;
	disk_to_dev(disk)->type = &disk_type;
	device_initialize(disk_to_dev(disk));
	inc_diskseq(disk);
	q->disk = disk; /* NVMe request_queue가 이 gendisk를 역참조할 수 있도록 연결 */
	lockdep_init_map(&disk->lockdep_map, "(bio completion)", lkclass, 0);
#ifdef CONFIG_BLOCK_HOLDER_DEPRECATED
	INIT_LIST_HEAD(&disk->slave_bdevs);
#endif
	mutex_init(&disk->rqos_state_mutex);
	kobject_init(&disk->queue_kobj, &blk_queue_ktype);
	return disk;

out_erase_part0:
	xa_erase(&disk->part_tbl, 0);
out_destroy_part_tbl:
	xa_destroy(&disk->part_tbl);
	disk->part0->bd_disk = NULL;
	bdev_drop(disk->part0);
out_free_bdi:
	bdi_put(disk->bdi);
out_free_bioset:
	bioset_exit(&disk->bio_split);
out_free_disk:
	kfree(disk);
	return NULL;
}

/*
 * __blk_alloc_disk - queue limits를 포함한 gendisk 할당
 * @lim: queue limits (NVMe의 물리적 섹터 크기, 최대 세그먼트 수 등)
 * @node: NUMA 노드
 * @lkclass: lockdep 클래스
 *
 * NVMe 드라이버가 자체 queue를 소유하는 단순 디스크를 만들 때 사용한다.
 * GD_OWNS_QUEUE 플래그를 설정하여 queue 생명주기를 gendisk가 관리함을 표시한다.
 */
struct gendisk *__blk_alloc_disk(struct queue_limits *lim, int node,
		struct lock_class_key *lkclass)
{
	struct queue_limits default_lim = { };
	struct request_queue *q;
	struct gendisk *disk;

	q = blk_alloc_queue(lim ? lim : &default_lim, node);
	if (IS_ERR(q))
		return ERR_CAST(q);

	disk = __alloc_disk_node(q, node, lkclass);
	if (!disk) {
		blk_put_queue(q);
		return ERR_PTR(-ENOMEM);
	}
	set_bit(GD_OWNS_QUEUE, &disk->state);
	return disk;
}
EXPORT_SYMBOL(__blk_alloc_disk);

/*
 * put_disk - gendisk의 참조 카운트 감소
 * @disk: 대상 gendisk
 *
 * NVMe 드라이버가 네임스페이스 제거 시 호출한다. 마지막 참조가 해제되면
 * disk_release -> blk_put_queue로 이어져 request_queue가 해제된다.
 * 프로브 실패 시에는 add_disk() 호출 전에 put_disk을 호출해야 tag_set이
 * 아직 유효한 상태에서 queue 정리가 가능하다.
 */
/**
 * put_disk - decrements the gendisk refcount
 * @disk: the struct gendisk to decrement the refcount for
 *
 * This decrements the refcount for the struct gendisk. When this reaches 0
 * we'll have disk_release() called.
 *
 * Note: for blk-mq disk put_disk must be called before freeing the tag_set
 * when handling probe errors (that is before add_disk() is called).
 *
 * Context: Any context, but the last reference must not be dropped from
 *          atomic context.
 */
void put_disk(struct gendisk *disk)
{
	if (disk)
		put_device(disk_to_dev(disk));
}
EXPORT_SYMBOL(put_disk);

static void set_disk_ro_uevent(struct gendisk *gd, int ro)
{
	char event[] = "DISK_RO=1";
	char *envp[] = { event, NULL };

	if (!ro)
		event[8] = '0';
	kobject_uevent_env(&disk_to_dev(gd)->kobj, KOBJ_CHANGE, envp);
}

/*
 * set_disk_ro - gendisk의 읽기 전용 상태 설정
 * @disk: 대상 gendisk
 * @read_only: true면 읽기 전용, false면 읽기/쓰기
 *
 * NVMe 네임스페이스가 write-protected 상태이거나 읽기 전용 모드로 노출되어야
 * 할 때 호출한다. 상태 변경 시 "DISK_RO=1/0" uevent를 발생시킨다.
 */
/**
 * set_disk_ro - set a gendisk read-only
 * @disk:	gendisk to operate on
 * @read_only:	%true to set the disk read-only, %false set the disk read/write
 *
 * This function is used to indicate whether a given disk device should have its
 * read-only flag set. set_disk_ro() is typically used by device drivers to
 * indicate whether the underlying physical device is write-protected.
 */
void set_disk_ro(struct gendisk *disk, bool read_only)
{
	if (read_only) {
		if (test_and_set_bit(GD_READ_ONLY, &disk->state))
			return;
	} else {
		if (!test_and_clear_bit(GD_READ_ONLY, &disk->state))
			return;
	}
	set_disk_ro_uevent(disk, read_only);
}
EXPORT_SYMBOL(set_disk_ro);

void inc_diskseq(struct gendisk *disk)
{
	disk->diskseq = atomic64_inc_return(&diskseq);
}

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ----------------------------------------------------------------------------
 * - 이 파일은 gendisk 생명주기를 관리하며, NVMe 네임스페이스를 블록 서브시스템에
 *   노출/제거하는 관문이다. 실제 I/O 처리는 request_queue -> blk-mq -> NVMe
 *   드라이버가 담당한다.
 *
 * - add_disk_fwnode/device_add_disk -> __add_disk -> blk_register_queue 경로를
 *   통해 NVMe 디스크가 /dev, /sys/block, /proc/diskstats에 등록되고, 이후
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd
 *   (doorbell)로 I/O가 전달된다.
 *
 * - del_gendisk/__del_gendisk은 NVMe 컨트롤러/네임스페이스 제거 시 queue를
 *   freeze/drain하고 모든 파티션과 sysfs 링크를 정리한다.
 *
 * - blk_mark_disk_dead은 갑작스러운 NVMe 제거(hot-unplug) 시 새 I/O 진입을
 *   차단하며, 하위 NVMe 레이어에서 진행 중인 CID의 완료/취소를 유도한다.
 *
 * - 이 파일은 block/bdev.c(block_device 관리), block/blk-mq.c(다중 큐 I/O),
 *   drivers/nvme/host/core.c(네임스페이스 생명주기)와 글로벌하게 연결된다.
 * ============================================================================
 */
