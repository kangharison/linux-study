// SPDX-License-Identifier: GPL-2.0-only
/*
 * block/holder.c - 블록 장치 holder/slave 관계 및 sysfs 심볼릭 링크 관리
 *
 * 이 파일은 상위 블록 장치(holder, 예: dm, md, multipath)와 하위 블록 장치
 * (slave, 예: nvme0n1 파티션 또는 namespace) 간의 sysfs 토폴로지 링크를
 * 생성/제거한다. NVMe SSD 관점에서는 nvme0n1 같은 namespace가 device-mapper
 * 타겟의 slave로 등록되거나, NVMe multipath 하위 경로로 연결될 때 이 코드가
 * /sys/block/<holder>/slaves/ 및 /sys/block/<slave>/holders/ 링크를 통해
 * 디스크 계층 구조를 노출한다. 실제 I/O 경로는 아니지만, 입출력 스택
 * (bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *  nvme_submit_cmd(doorbell)) 위의 장치 트리를 표현하므로 NVMe 장치 관리와
 * 직접적으로 연결된다.
 *
 * 연관 파일: block/genhd.c(gendisk/bdev 생명주기),
 *           block/partitions/*.c(파티션 holder 관계)
 */
#include <linux/blkdev.h>
#include <linux/slab.h>

/* NVMe namespace 등을 slave로 포함하는 holder 디스크 엔트리 */
struct bd_holder_disk {
	struct list_head	list;		/* disk->slave_bdevs 리스트 연결; NVMe namespace가 여러 holder에 동시 속할 수 있음을 표현 */
	struct kobject		*holder_dir;	/* slave bdev의 holders/ 디렉터리 kobject; NVMe 예시: /sys/block/nvme0n1/holders/<holder> */
	int			refcnt;		/* 동일 holder-slave 쌍에 대한 중복 연결 참조 카운트 */
};

static DEFINE_MUTEX(blk_holder_mutex);

/*
 * bd_find_holder_disk: bdev와 disk 사이의 기존 holder 관계를 검색한다.
 *
 * NVMe 맥락: 동일한 NVMe namespace bdev가 이미 해당 holder disk에 등록되어
 * 있는지 확인할 때 사용된다. (추정) dm-multipath 경로 재설정 등에서 중복
 * 등록을 방지하기 위해 호출될 수 있다.
 */
static struct bd_holder_disk *bd_find_holder_disk(struct block_device *bdev,
						  struct gendisk *disk)
{
	struct bd_holder_disk *holder;

	list_for_each_entry(holder, &disk->slave_bdevs, list)
		if (holder->holder_dir == bdev->bd_holder_dir)
			return holder;
	return NULL;
}

static int add_symlink(struct kobject *from, struct kobject *to)
{
	return sysfs_create_link(from, to, kobject_name(to));
}

static void del_symlink(struct kobject *from, struct kobject *to)
{
	sysfs_remove_link(from, kobject_name(to));
}

/**
 * bd_link_disk_holder - create symlinks between holding disk and slave bdev
 * @bdev: the claimed slave bdev
 * @disk: the holding disk
 *
 * DON'T USE THIS UNLESS YOU'RE ALREADY USING IT.
 *
 * This functions creates the following sysfs symlinks.
 *
 * - from "slaves" directory of the holder @disk to the claimed @bdev
 * - from "holders" directory of the @bdev to the holder @disk
 *
 * For example, if /dev/dm-0 maps to /dev/sda and disk for dm-0 is
 * passed to bd_link_disk_holder(), then:
 *
 *   /sys/block/dm-0/slaves/sda --> /sys/block/sda
 *   /sys/block/sda/holders/dm-0 --> /sys/block/dm-0
 *
 * The caller must have claimed @bdev before calling this function and
 * ensure that both @bdev and @disk are valid during the creation and
 * lifetime of these symlinks.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * bd_link_disk_holder 목적: 상위 디스크(@disk)와 하위 bdev(@bdev) 사이에
 * sysfs 심볼릭 링크를 생성하여 디스크 계층 구조를 노출한다.
 *
 * NVMe 연결점:
 *  - NVMe namespace bdev(예: nvme0n1)가 dm-multipath, dm-linear, md 등의
 *    slave로 등록될 때 이 함수가 호출된다.
 *  - 생성되는 링크 예시:
 *      /sys/block/dm-0/slaves/nvme0n1 -> /sys/block/nvme0n1
 *      /sys/block/nvme0n1/holders/dm-0 -> /sys/block/dm-0
 *  - (추정) NVMe multipath 경로 추가 시 새 경로의 namespace bdev가 이 함수를
 *    통해 holder 관계를 맺을 수 있다.
 *
 * 호출 경로 (추정):
 *   dm_table_add_target -> dm_get_device -> bd_link_disk_holder
 *   또는 md_add_new_disk -> bind_rdev_to_array -> bd_link_disk_holder
 */
int bd_link_disk_holder(struct block_device *bdev, struct gendisk *disk)
{
	struct bd_holder_disk *holder;
	int ret = 0;

	if (WARN_ON_ONCE(!disk->slave_dir))
		return -EINVAL;

	if (bdev->bd_disk == disk)
		return -EINVAL;

	/*
	 * del_gendisk drops the initial reference to bd_holder_dir, so we
	 * need to keep our own here to allow for cleanup past that point.
	 */
	mutex_lock(&bdev->bd_disk->open_mutex);
	if (!disk_live(bdev->bd_disk)) {	/* NVMe namespace가 아직 live 상태인지 확인; 제거 중인 장치는 slave로 등록 불가 */
		mutex_unlock(&bdev->bd_disk->open_mutex);
		return -ENODEV;
	}
	kobject_get(bdev->bd_holder_dir);	/* sysfs 링크 생명주기를 위해 kobject 참조 획득 */
	mutex_unlock(&bdev->bd_disk->open_mutex);

	mutex_lock(&blk_holder_mutex);
	WARN_ON_ONCE(!bdev->bd_holder);		/* bdev가 먼저 claim되어야 함; NVMe 관점에서 nvme_ns_head 또는 bdev holder 설정 후에만 연결 가능 */

	holder = bd_find_holder_disk(bdev, disk);
	if (holder) {
		kobject_put(bdev->bd_holder_dir);	/* 기존 관계가 있으면 중복 kobject_get 롤백 */
		holder->refcnt++;			/* 동일 holder-slave 쌍에 대한 중복 참조 카운트 증가 */
		goto out_unlock;
	}

	holder = kzalloc_obj(*holder);
	if (!holder) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	INIT_LIST_HEAD(&holder->list);
	holder->refcnt = 1;
	holder->holder_dir = bdev->bd_holder_dir;

	ret = add_symlink(disk->slave_dir, bdev_kobj(bdev));	/* /sys/block/<holder>/slaves/<slave> 링크 생성 */
	if (ret)
		goto out_free_holder;
	ret = add_symlink(bdev->bd_holder_dir, &disk_to_dev(disk)->kobj);	/* /sys/block/<slave>/holders/<holder> 링크 생성 */
	if (ret)
		goto out_del_symlink;
	list_add(&holder->list, &disk->slave_bdevs);		/* disk->slave_bdevs 리스트에 NVMe slave 등록 */

	mutex_unlock(&blk_holder_mutex);
	return 0;

out_del_symlink:
	del_symlink(disk->slave_dir, bdev_kobj(bdev));
out_free_holder:
	kfree(holder);
out_unlock:
	mutex_unlock(&blk_holder_mutex);
	if (ret)
		kobject_put(bdev->bd_holder_dir);
	return ret;
}
EXPORT_SYMBOL_GPL(bd_link_disk_holder);

/**
 * bd_unlink_disk_holder - destroy symlinks created by bd_link_disk_holder()
 * @bdev: the calimed slave bdev
 * @disk: the holding disk
 *
 * DON'T USE THIS UNLESS YOU'RE ALREADY USING IT.
 *
 * CONTEXT:
 * Might sleep.
 */
/*
 * bd_unlink_disk_holder 목적: bd_link_disk_holder()가 생성한 sysfs 링크를
 * 제거하고 holder 관계를 해제한다.
 *
 * NVMe 연결점:
 *  - NVMe namespace가 device-mapper 테이블에서 제거되거나 md 배열에서
 *    빠질 때 호출된다.
 *  - (추정) NVMe multipath 경로가 실패/제거되어 더 이상 해당 namespace가
 *    활성 경로로 사용되지 않을 때 이 함수로 링크가 정리된다.
 *  - refcnt가 0이 될 때만 링크와 구조체를 해제하므로, 동일한 NVMe slave를
 *    여러 테이블이 참조 중일 때 안전하게 정리된다.
 *
 * 호출 경로 (추정):
 *   dm_put_device -> bd_unlink_disk_holder
 *   또는 md_kick_rdev_from_array -> bd_unlink_disk_holder
 */
void bd_unlink_disk_holder(struct block_device *bdev, struct gendisk *disk)
{
	struct bd_holder_disk *holder;

	if (WARN_ON_ONCE(!disk->slave_dir))
		return;

	mutex_lock(&blk_holder_mutex);
	holder = bd_find_holder_disk(bdev, disk);
	if (!WARN_ON_ONCE(holder == NULL) && !--holder->refcnt) {	/* refcnt가 0일 때만 실제 제거 */
		del_symlink(disk->slave_dir, bdev_kobj(bdev));		/* /sys/block/<holder>/slaves/<slave> 링크 제거 */
		del_symlink(holder->holder_dir, &disk_to_dev(disk)->kobj);	/* /sys/block/<slave>/holders/<holder> 링크 제거 */
		kobject_put(holder->holder_dir);			/* bd_holder_dir에 대한 kobject 참조 해제 */
		list_del_init(&holder->list);			/* disk->slave_bdevs 리스트에서 NVMe slave 제거 */
		kfree(holder);
	}
	mutex_unlock(&blk_holder_mutex);
}
EXPORT_SYMBOL_GPL(bd_unlink_disk_holder);

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 실제 NVMe I/O 경로(bio -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell))가
 *   아니라, 상위 장치와 NVMe namespace 간의 sysfs 토폴로지 링크를 관리한다.
 * - NVMe namespace bdev(예: nvme0n1)는 dm-multipath, md, dm-linear 등의
 *   slave로 등록될 수 있으며, 이 파일이 /sys/block 아래 계층 링크를 생성한다.
 * - bd_link_disk_holder / bd_unlink_disk_holder는 device-mapper나 md 등에서
 *   NVMe slave를 attach/detach할 때 호출되며, refcnt로 중복 연결을 안전하게
 *   처리한다.
 * - (추정) NVMe multipath 경로 추가/제거 시에도 이 파일의 링크 생성/제거가
 *   /sys 에 즉시 반영되어 관리 도구에서 현재 활성 경로를 확인할 수 있다.
 * - block/genhd.c, block/partitions/ 등과 연계되어 gendisk 및 bdev의
 *   생명주기, sysfs 표현 전체를 완성한다.
 */
