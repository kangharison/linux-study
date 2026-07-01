// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1991-1998  Linus Torvalds
 * Re-organised Feb 1998 Russell King
 * Copyright (C) 2020 Christoph Hellwig
 */
/*
 * NVMe 관점 파일 개요
 *
 * 이 파일은 커널 범용 블록 레이어의 파티션(partition) 하위 시스템 핵심이다.
 * NVMe SSD가 노출하는 namespace 하나는 gendisk로 표현되며, 이 파일은
 * 해당 디스크의 파티션 테이블을 읽어 각 파티션을 독립적인 block_device로
 * 생성/삭제/크기 조정한다.
 * 파일 시스템이 파티션에 대해 I/O를 시작하면, 블록 레이어는
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe SQ/CQ에
 * CID를 할당하여 LBA 요청을 변환한다.
 * 따라서 이 파일은 NVMe namespace의 가상 주소 공간을 사용자에게 보이는
 * /dev/nvmeXnYpZ 장치로 분할하는 관문(gateway) 역할을 한다.
 * 본 파일은 block/gendisk.c, block/block_dev.c 등에서 디스크가
 * 등록된 뒤 호출되는 partition scan 단계를 담당한다.
 */
#include <linux/fs.h> /* NVMe namespace bdev를 위한 fs 헤더 */
#include <linux/major.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/ctype.h>
#include <linux/vmalloc.h>
#include <linux/raid/detect.h>
#include "check.h" /* struct parsed_partitions 및 파티션 프로버 인터페이스 */

/*
 * NVMe namespace의 LBA 0 또는 0xdc0 등에 위치한 파티션 테이블 포맷을
 * 하나씩 시도한다. 각 프로버는 page cache를 통해 NVMe READ 명령으로
 * 섹터를 읽는다 (추정).
 */
static int (*const check_part[])(struct parsed_partitions *) = {
	/*
	 * Probe partition formats with tables at disk address 0
	 * that also have an ADFS boot block at 0xdc0.
	 */
#ifdef CONFIG_ACORN_PARTITION_ICS
	adfspart_check_ICS, /* LBA 0/0xdc0의 ICS 메타데이터를 NVMe READ로 검증 */
#endif
#ifdef CONFIG_ACORN_PARTITION_POWERTEC
	adfspart_check_POWERTEC,
#endif
#ifdef CONFIG_ACORN_PARTITION_EESOX
	adfspart_check_EESOX,
#endif

	/*
	 * Now move on to formats that only have partition info at
	 * disk address 0xdc0.  Since these may also have stale
	 * PC/BIOS partition tables, they need to come before
	 * the msdos entry.
	 */
#ifdef CONFIG_ACORN_PARTITION_CUMANA
	adfspart_check_CUMANA,
#endif
#ifdef CONFIG_ACORN_PARTITION_ADFS
	adfspart_check_ADFS,
#endif

#ifdef CONFIG_CMDLINE_PARTITION
	cmdline_partition, /* 커널 파라미터로 주어진 파티션을 NVMe namespace에 적용 */
#endif
#ifdef CONFIG_OF_PARTITION
	of_partition,		/* cmdline have priority to OF */
#endif
#ifdef CONFIG_EFI_PARTITION
	efi_partition,		/* this must come before msdos */
	/*
	 * GPT 헤더는 LBA 1, 파티션 항목 배열은 그 뒤에 위치하며,
	 * EFI 프로버는 read_part_sector()로 NVMe namespace의 첫 번째 LBA
	 * 다음 영역부터 Protective MBR과 GPT 헤더/엔트리를 순차 READ한다.
	 */
#endif
#ifdef CONFIG_SGI_PARTITION
	sgi_partition,
#endif
#ifdef CONFIG_LDM_PARTITION
	ldm_partition,		/* this must come before msdos */
#endif
#ifdef CONFIG_MSDOS_PARTITION
	msdos_partition,
	/*
	 * MBR 프로버: LBA 0의 512B 부트 섹터에서 0x55AA 시그니처를 확인하고,
	 * 4개의 primary partition entry를 순회하며, 필요시 extended partition
	 * 체인을 따라 NVMe READ 명령을 추가로 발생시킨다.
	 */
#endif
#ifdef CONFIG_OSF_PARTITION
	osf_partition,
#endif
#ifdef CONFIG_SUN_PARTITION
	sun_partition,
#endif
#ifdef CONFIG_AMIGA_PARTITION
	amiga_partition,
#endif
#ifdef CONFIG_ATARI_PARTITION
	atari_partition,
#endif
#ifdef CONFIG_MAC_PARTITION
	mac_partition,
#endif
#ifdef CONFIG_ULTRIX_PARTITION
	ultrix_partition,
#endif
#ifdef CONFIG_IBM_PARTITION
	ibm_partition,
#endif
#ifdef CONFIG_KARMA_PARTITION
	karma_partition,
#endif
#ifdef CONFIG_SYSV68_PARTITION
	sysv68_partition,
#endif
	NULL /* 더 이상 시도할 포맷 프로버가 없으면 순회 종료 */
};

/*
 * struct parsed_partitions (include/linux/check.h에 정의):
 *  - pp_buf: 파티션 스캔 중 출력할 디버그/정보 버퍼. NVMe namespace
 *    이름과 함께 커널 로그에 기록된다.
 *  - parts[]: 탐지된 각 파티션의 시작 섹터(from), 크기(size), 플래그,
 *    메타 정보. NVMe LBA 범위로 변환될 후보들이다.
 *  - limit: 최대 파티션 수 (DISK_MAX_PARTS). NVMe namespace도 동일 제한.
 *  - disk: 스캔 대상 gendisk (NVMe namespace 디스크).
 *  - name: 디스크/파티션 접두사 (예: nvme0n1p1).
 *  - access_beyond_eod: 디스크 끝(EOD)을 넘어 읽었는지 표시.
 *    NVMe namespace 용량보다 큰 LBA 읽기 시 플래그가 설정된다.
 */
/*
 * allocate_partitions() - 파티션 스캔 상태를 할당한다.
 *
 * 목적: NVMe namespace 디스크(hd)에 대한 파티션 테이블 파싱 작업에
 * 필요한 parsed_partitions 구조체와 parts 배열을 할당한다.
 *
 * 호출 경로: check_partition() -> allocate_partitions()
 * 이후 blk_add_partitions() -> blk_add_partition() -> add_partition()
 * 순으로 파티션 block_device가 생성된다.
 *
 * NVMe 연결 지점: NVMe 드라이버가 등록한 gendisk의 part_tbl에
 * 추가될 후보 항목들을 담는 컨테이너를 준비한다.
 */
static struct parsed_partitions *allocate_partitions(struct gendisk *hd)
{
	struct parsed_partitions *state;
	int nr = DISK_MAX_PARTS; /* NVMe namespace 당 최대 파티션 수 한도 */

	state = kzalloc_obj(*state); /* 파티션 스캔 상태 메모리 할당; 실패 시 스캔 중단 */
	if (!state)
		return NULL; /* 메모리 부족으로 NVMe namespace 파티션 스캔 시작 불가 */

	state->parts = vzalloc(array_size(nr, sizeof(state->parts[0])));
	if (!state->parts) {
		kfree(state); /* parts 배열 할당 실패 시 state도 해제하고 스캔 포기 */
		return NULL;
	}

	state->limit = nr; /* parts[] 배열 인덱스 상한 설정 */

	return state;
}

/*
 * free_partitions() - 파티션 스캔 상태를 해제한다.
 *
 * 목적: 파티션 탐지가 끝난 후 parts 배열과 parsed_partitions 구조체를
 * 해제하여 NVMe namespace에 할당된 메모리를 회수한다.
 */
static void free_partitions(struct parsed_partitions *state)
{
	vfree(state->parts); /* 각 파티션 후보 정보를 담은 가상 메모리 반납 */
	kfree(state); /* 스캔 상태 구조체 반납 */
}

/*
 * check_partition() - 디스크에 기록된 파티션 테이블을 프로브한다.
 *
 * 목적: check_part[]에 등록된 각 포맷별 프로버를 순회하며
 * LBA 0 및 기타 파티션 메타데이터 영역을 읽고 파티션 정보를 파싱한다.
 *
 * 호출 경로: blk_add_partitions() -> check_partition()
 * 이후 blk_add_partition() -> add_partition()을 통해
 * /dev/nvmeXnYpZ 장치가 생성됨.
 *
 * NVMe 연결 지점: NVMe namespace의 LBA 0은 MBR/GPT 헤더가 위치하는
 * 영역이다 (추정). read_part_sector()가 page cache를 통해 해당 LBA를
 * 읽으며, 필요 시 NVMe I/O 경로인
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell)를 통해 실제 플래시의
 * 첫 번째 논리 블록에 접근한다.
 */
static struct parsed_partitions *check_partition(struct gendisk *hd)
{
	struct parsed_partitions *state;
	int i, res, err;

	/* 먼저 파티션 스캔 상태(parsed_partitions)를 할당한다. */
	state = allocate_partitions(hd);
	if (!state)
		return NULL; /* 메모리 할당 실패: NVMe namespace 파티션 인식 포기 */
	/* 파티션 스캔 로그를 담을 4 KiB 페이지를 할당한다. */
	state->pp_buf.buffer = (char *)__get_free_page(GFP_KERNEL);
	if (!state->pp_buf.buffer) {
		free_partitions(state); /* 로그 버퍼 할당 실패 시 상태 해제 */
		return NULL;
	}
	seq_buf_init(&state->pp_buf, state->pp_buf.buffer, PAGE_SIZE);

	state->disk = hd; /* 스캔 대상 gendisk 참조 저장 (NVMe namespace 디스크) */
	/* 스캔 대상을 NVMe namespace gendisk로 기록한다. */
	strscpy(state->name, hd->disk_name);
	/* NVMe namespace 이름으로 파티션 스캔 로그를 시작한다. */
	seq_buf_printf(&state->pp_buf, " %s:", state->name);
	/* 이름이 숫자로 끝나면(예: nvme0n1) 파티션 접미사 'p'를 붙인다. */
	if (isdigit(state->name[strlen(state->name)-1]))
		sprintf(state->name, "p");

	i = res = err = 0;
	/* 등록된 파티션 포맷 프로버(MBR/GPT 등)를 순회할 준비. */
	while (!res && check_part[i]) {
	/* 이전 프로버가 남긴 후보 상태를 초기화한다. */
		memset(state->parts, 0, state->limit * sizeof(state->parts[0]));
	/* 각 포맷 프로버가 NVMe namespace의 LBA를 읽어 파티션을 파싱한다. */
		res = check_part[i++](state);
	/* I/O 오류 발생 시 기록하되 다른 포맷 프로버를 계속 시도한다. */
		if (res < 0) {
			/*
			 * We have hit an I/O error which we don't report now.
			 * But record it, and let the others do their job.
			 */
			err = res; /* NVMe CQE error 또는 page cache READ 실패를 기록 (추정) */
			res = 0; /* 다음 포맷 프로버가 성공할 가능성을 열어둠 */
		}

	}
	/* 파티션 테이블 인식에 성공하면 결과를 출력하고 반환한다. */
	if (res > 0) {
		printk(KERN_INFO "%s", seq_buf_str(&state->pp_buf));

		free_page((unsigned long)state->pp_buf.buffer);
		return state;
	}
	/* NVMe namespace 용량보다 큰 LBA에 접근했다면 -ENOSPC로 기록한다. */
	if (state->access_beyond_eod)
		err = -ENOSPC;
	/*
	 * The partition is unrecognized. So report I/O errors if there were any
	 */
	/* 인식할 수 없는 테이블이고 I/O 오류가 있으면 보고한다. */
	if (err)
		res = err;
	if (res) {
		seq_buf_puts(&state->pp_buf,
			     " unable to read partition table\n");
		printk(KERN_INFO "%s", seq_buf_str(&state->pp_buf));
	}

	/* 파티션 테이블 인식 실패 시 로그 버퍼를 해제한다. */
	free_page((unsigned long)state->pp_buf.buffer);
	free_partitions(state);
	return ERR_PTR(res);
}

/*
 * 아래 part_xxx_show() 함수들은 /sys/block/nvmeXnY/nvmeXnYpZ/ 아래
 * 파티션 번호, 시작 섹터, 크기, 읽기 전용 등의 속성을 sysfs로 노출한다.
 */
static ssize_t part_partition_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", bdev_partno(dev_to_bdev(dev)));
	/* 예: /sys/block/nvme0n1/nvme0n1p1/partition 에 1 출력 */
}

static ssize_t part_start_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n", dev_to_bdev(dev)->bd_start_sect);
	/* 파티션의 NVMe namespace LBA 기준 시작 섹터를 사용자 공간에 노출 */
}

static ssize_t part_ro_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", bdev_read_only(dev_to_bdev(dev)));
	/* 읽기 전용 파티션 여부: NVMe WRITE 명령 차단 여부와 직결됨 */
}

static ssize_t part_alignment_offset_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", bdev_alignment_offset(dev_to_bdev(dev)));
	/* NVMe namespace 논리 블록 정렬 오프셋 정보 노출 */
}

static ssize_t part_discard_alignment_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", bdev_discard_alignment(dev_to_bdev(dev)));
	/* discard(TRIM/Deallocate) 정렬 정보: NVMe Deallocate 명령에 영향 */
}

	/* block_device 파티션의 sysfs 속성 정의. */
static DEVICE_ATTR(partition, 0444, part_partition_show, NULL);
static DEVICE_ATTR(start, 0444, part_start_show, NULL);
static DEVICE_ATTR(size, 0444, part_size_show, NULL);
static DEVICE_ATTR(ro, 0444, part_ro_show, NULL);
static DEVICE_ATTR(alignment_offset, 0444, part_alignment_offset_show, NULL);
static DEVICE_ATTR(discard_alignment, 0444, part_discard_alignment_show, NULL);
static DEVICE_ATTR(stat, 0444, part_stat_show, NULL);
static DEVICE_ATTR(inflight, 0444, part_inflight_show, NULL);
#ifdef CONFIG_FAIL_MAKE_REQUEST
static struct device_attribute dev_attr_fail =
	__ATTR(make-it-fail, 0644, part_fail_show, part_fail_store);
#endif

	/* 위 속성들의 배열. */
static struct attribute *part_attrs[] = {
	&dev_attr_partition.attr,
	&dev_attr_start.attr,
	&dev_attr_size.attr,
	&dev_attr_ro.attr,
	&dev_attr_alignment_offset.attr,
	&dev_attr_discard_alignment.attr,
	&dev_attr_stat.attr,
	&dev_attr_inflight.attr,
#ifdef CONFIG_FAIL_MAKE_REQUEST
	&dev_attr_fail.attr,
#endif
	NULL
};

/*
 * struct attribute_group part_attr_group:
 *  - .attrs: 파티션 관련 sysfs 파일 목록.
 * NVMe 관점: /sys/block/nvmeXnY/nvmeXnYpZ/ 아래에서 시작 섹터, 크기 등을
 * 사용자 공간에 노출한다.
 */
static const struct attribute_group part_attr_group = {
	.attrs = part_attrs,
};

	/* 파티션 속성 그룹 목록 (blk_trace 포함 가능). */
static const struct attribute_group *part_attr_groups[] = {
	&part_attr_group,
#ifdef CONFIG_BLK_DEV_IO_TRACE
	&blk_trace_attr_group,
#endif
	NULL
};

/*
 * part_release() - 파티션 block_device의 최종 해제 핸들러.
 *
 * 목적: sysfs 장치 모델에서 파티션이 제거될 때 disk 참조 카운트를 감소시키고
 * bdev를 해제한다.
 *
 * NVMe 연결 지점: /dev/nvmeXnYpZ 장치가 완전히 소멸될 때 호출된다.
 */
static void part_release(struct device *dev)
{
	put_disk(dev_to_bdev(dev)->bd_disk); /* NVMe namespace gendisk 참조 해제 */
	bdev_drop(dev_to_bdev(dev)); /* 파티션 block_device 메모리 반납 */
}

/*
 * part_uevent() - 파티션 추가/변경 시 uevent를 생성한다.
 *
 * 목적: 사용자 공간(udev 등)에 PARTN/PARTNAME/PARTUUID 환경 변수를
 * 전달하여 /dev/nvmeXnYpZ 노드를 올바르게 설정한다.
 */
static int part_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct block_device *part = dev_to_bdev(dev);

	add_uevent_var(env, "PARTN=%u", bdev_partno(part));
	if (part->bd_meta_info && part->bd_meta_info->volname[0])
		add_uevent_var(env, "PARTNAME=%s", part->bd_meta_info->volname);
		/* GPT 파티션 이름을 uevent로 전달: NVMe media에서 읽은 메타데이터 */
	if (part->bd_meta_info && part->bd_meta_info->uuid[0])
		add_uevent_var(env, "PARTUUID=%s", part->bd_meta_info->uuid);
		/* GPT PARTUUID 전달: NVMe namespace LBA에서 파싱한 고유 식별자 */
	return 0;
}

/*
 * struct device_type part_type:
 *  - .name: sysfs에서 "partition" 노드 이름.
 *  - .groups: 파티션별 sysfs 속성들.
 *  - .release: block_device 해제 시 disk/bdev 참조 카운트 감소.
 *  - .uevent: 파티션 추가/변경 시 PARTN/PARTNAME/PARTUUID 전달.
 * NVMe 관점: 각 파티션이 독립 sysfs 엔트리를 갖게 되어
 * 사용자 공간이 /sys/block/nvmeXnY/nvmeXnYpZ로 파티션 정보를 확인한다.
 */
const struct device_type part_type = {
	.name		= "partition",
	.groups		= part_attr_groups,
	.release	= part_release,
	.uevent		= part_uevent,
};

/*
 * drop_partition() - 기존 파티션 block_device를 제거한다.
 *
 * 목적: 디스크의 part_tbl에서 파티션 항목을 지우고 장치를 삭제한다.
 *
 * 호출 경로: bdev_disk_changed() 또는 bdev_del_partition() ->
 *           drop_partition()
 *
 * NVMe 연결 지점: 파티션이 제거되면 해당 /dev/nvmeXnYpZ에 대한
 * 열린 file/block_device는 더 이상 lookup되지 않으며, 이후
 * NVMe namespace로 가는 I/O는 파티션 없이 전체 디스크(gendisk)로
 * 직접 흘러간다.
 */
void drop_partition(struct block_device *part)
{
	lockdep_assert_held(&part->bd_disk->open_mutex);

	/* gendisk->part_tbl에서 해당 partno 항목을 제거한다. */
	xa_erase(&part->bd_disk->part_tbl, bdev_partno(part));
	kobject_put(part->bd_holder_dir); /* /sys/.../holders kobject 참조 해제 */

	device_del(&part->bd_device); /* sysfs와 devtmpfs에서 nvmeXnYpZ 노드 제거 */
	put_device(&part->bd_device); /* device 구조체 최종 해제 대기 */
}

static ssize_t whole_disk_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return 0;
}
static const DEVICE_ATTR(whole_disk, 0444, whole_disk_show, NULL);

/*
 * Must be called either with open_mutex held, before a disk can be opened or
 * after all disk users are gone.
 */
/*
 * struct block_device (NVMe 관련 필드):
 *  - bd_start_sect: 파티션의 NVMe namespace LBA 기준 시작 오프셋.
 *  - bd_disk: 소속 gendisk (NVMe namespace 디스크).
 *  - bd_device: sysfs/device 모델을 위한 장치 엔트리.
 *  - bd_holder_dir: /sys/.../holders 하위 디렉터리.
 *  - bd_meta_info: GPT 파티션 이름/UUID 등 메타 정보.
 */
/*
 * add_partition() - gendisk 위에 새 block_device 파티션을 생성한다.
 *
 * 목적: 파티션 번호(partno), 시작 섹터(start), 길이(len)를 기준으로
 * NVMe namespace 안의 LBA 범위를 독립적인 block_device로 노출한다.
 *
 * 호출 경로: blk_add_partition() -> add_partition()
 *           bdev_add_partition() -> add_partition()
 *
 * NVMe 연결 지점: NVMe 드라이버가 생성한 namespace용 gendisk에
 * bd_start_sect 오프셋을 가진 파티션 bdev를 추가한다.
 * 이후 파일 시스템의 submit_bio는 bdev의 bd_start_sect를 더해
 * NVMe namespace의 실제 LBA로 변환한다.
 */
static struct block_device *add_partition(struct gendisk *disk, int partno,
				sector_t start, sector_t len, int flags,
				struct partition_meta_info *info)
{
	dev_t devt = MKDEV(0, 0);
	struct device *ddev = disk_to_dev(disk); /* NVMe namespace의 device 객체 */
	struct device *pdev;
	struct block_device *bdev;
	const char *dname;
	int err;

	/* open_mutex 보유 확인: part_tbl과 bdev 상태 일관성 유지. */
	lockdep_assert_held(&disk->open_mutex);

	/* NVMe namespace도 DISK_MAX_PARTS 최대 파티션 수 제한을 따른다. */
	if (partno >= DISK_MAX_PARTS)
		return ERR_PTR(-EINVAL);

	/*
	 * Partitions are not supported on zoned block devices that are used as
	 * such.
	 */
	/* Host managed zoned NVMe namespace(ZNSSD)는 파티션을 지원하지 않는다. */
	if (bdev_is_zoned(disk->part0)) {
		pr_warn("%s: partitions not supported on host managed zoned block device\n",
			disk->disk_name);
		return ERR_PTR(-ENXIO);
	}

	/* 동일 partno가 이미 존재하는지 확인한다. */
	if (xa_load(&disk->part_tbl, partno))
		return ERR_PTR(-EBUSY); /* 이미 /dev/nvmeXnYpN 형태 bdev가 등록됨 */

	/* ensure we always have a reference to the whole disk */
	/* 파티션이 살아있는 동안 전체 디스크 참조를 유지한다. */
	get_device(disk_to_dev(disk));

	err = -ENOMEM;
	/* 파티션 전용 block_device를 할당한다. */
	bdev = bdev_alloc(disk, partno);
	if (!bdev)
		goto out_put_disk; /* bdev 할당 실패: NVMe 파티션 등록 포기 */

	/* NVMe namespace LBA 기준 파티션 시작 오프셋을 설정한다. */
	bdev->bd_start_sect = start;
	/* 파티션 길이를 NVMe sector(보통 512B/4KiB) 단위로 설정한다. */
	bdev_set_nr_sectors(bdev, len);

	pdev = &bdev->bd_device;
	dname = dev_name(ddev);
	/* nvme0n1p1 또는 nvme0n1 형태의 장치 이름을 결정한다. */
	if (isdigit(dname[strlen(dname) - 1]))
		dev_set_name(pdev, "%sp%d", dname, partno);
	else
		dev_set_name(pdev, "%s%d", dname, partno);

	device_initialize(pdev);
	pdev->class = &block_class;
	pdev->type = &part_type;
	pdev->parent = ddev; /* /sys/block/nvmeXnY 아래 nvmeXnYpZ 노드 배치 */

	/* in consecutive minor range? */
	/* 연속 minor 범위에 들어가면 사용, 아니면 동적 확장 minor를 할당한다. */
	if (bdev_partno(bdev) < disk->minors) {
		devt = MKDEV(disk->major, disk->first_minor + bdev_partno(bdev));
	} else {
		err = blk_alloc_ext_minor();
		if (err < 0)
			goto out_put; /* 확장 minor 부족 시 파티션 등록 롤백 */
		devt = MKDEV(BLOCK_EXT_MAJOR, err);
	}
	pdev->devt = devt;

	/* GPT 파티션 이름/UUID 등 메타정보를 복사한다. */
	if (info) {
		err = -ENOMEM;
		bdev->bd_meta_info = kmemdup(info, sizeof(*info), GFP_KERNEL);
		if (!bdev->bd_meta_info)
			goto out_put; /* 메타정보 복사 실패 시 롤백 */
	}

	/* delay uevent until 'holders' subdir is created */
	/* holders 하위 디렉터리 생성 전 uevent를 억제한다. */
	dev_set_uevent_suppress(pdev, 1);
	err = device_add(pdev);
	if (err)
		goto out_put; /* sysfs 등록 실패: /dev/nvmeXnYpZ 노드 생성 불가 */

	err = -ENOMEM;
	/* /sys/block/nvmeXnYpZ/holders 디렉터리를 생성한다. */
	bdev->bd_holder_dir = kobject_create_and_add("holders", &pdev->kobj);
	if (!bdev->bd_holder_dir)
		goto out_del; /* holders 디렉터리 생성 실패 시 롤백 */

	dev_set_uevent_suppress(pdev, 0);
	/* WHOLEDISK 플래그 속성을 sysfs에 추가한다. */
	if (flags & ADDPART_FLAG_WHOLEDISK) {
		err = device_create_file(pdev, &dev_attr_whole_disk);
		if (err)
			goto out_del;
	}

	/* 읽기 전용 파티션 플래그를 설정한다. */
	if (flags & ADDPART_FLAG_READONLY)
		bdev_set_flag(bdev, BD_READ_ONLY);

	/* everything is up and running, commence */
	/* gendisk->part_tbl에 파티션을 등록한다. */
	err = xa_insert(&disk->part_tbl, partno, bdev, GFP_KERNEL);
	if (err)
		goto out_del; /* part_tbl 삽입 실패(예: race) 시 롤백 */
	/* blockdev inode 해시에 등록하여 /dev/nvmeXnYpZ lookup이 가능하게 한다. */
	bdev_add(bdev, devt);

	/* suppress uevent if the disk suppresses it */
	/* 사용자 공간에 nvmeXnYpZ 추가 uevent(KOBJ_ADD)를 전달한다. */
	if (!dev_get_uevent_suppress(ddev))
		kobject_uevent(&pdev->kobj, KOBJ_ADD);
	return bdev; /* /dev/nvmeXnYpZ 형태의 block_device 생성 완료 */

out_del:
	kobject_put(bdev->bd_holder_dir);
	device_del(pdev);
out_put:
	put_device(pdev);
	return ERR_PTR(err);
out_put_disk:
	put_disk(disk);
	return ERR_PTR(err);
}

/*
 * partition_overlaps() - 기존 파티션과 LBA 범위가 겹치는지 검사한다.
 *
 * 목적: 새로 추가/크기 조정할 파티션의 [start, start+length)가
 * 기존 파티션의 [bd_start_sect, bd_start_sect+nr_sectors)와
 * NVMe LBA 공간상 겹치는지 확인한다.
 *
 * 호출 경로: bdev_add_partition()/bdev_resize_partition() ->
 *           partition_overlaps()
 */
static bool partition_overlaps(struct gendisk *disk, sector_t start,
		sector_t length, int skip_partno)
{
	struct block_device *part;
	bool overlap = false;
	unsigned long idx;

	rcu_read_lock();
	/* RCU 보호 아래 gendisk->part_tbl의 기존 파티션들을 순회한다. */
	xa_for_each_start(&disk->part_tbl, idx, part, 1) {
		if (bdev_partno(part) != skip_partno &&
		/* 두 LBA 구간이 겹치면 overlap=true로 설정한다. */
		    start < part->bd_start_sect + bdev_nr_sectors(part) &&
		    start + length > part->bd_start_sect) {
			overlap = true;
			break; /* NVMe LBA 충돌 발견: 추가/크기조정 거부 */
		}
	}
	rcu_read_unlock();

	return overlap;
}

/*
 * bdev_add_partition() - 사용자/커널이 요청한 파티션을 추가한다.
 *
 * 목적: block layer ioctl/userspace 요청에 따라 NVMe namespace 디스크에
 * 파티션을 추가한다.
 *
 * 호출 경로: ioctl(BLKPG_ADD_PARTITION) 등 ->
 *           bdev_add_partition() -> add_partition()
 */
int bdev_add_partition(struct gendisk *disk, int partno, sector_t start,
		sector_t length)
{
	struct block_device *part;
	int ret;

	mutex_lock(&disk->open_mutex);
	/* NVMe namespace가 아직 살아있지 않으면 오류 반환. */
	if (!disk_live(disk)) {
		ret = -ENXIO;
		goto out;
	}

	/* 해당 디스크는 파티션을 허용하지 않는다. */
	if (disk->flags & GENHD_FL_NO_PART) {
		ret = -EINVAL;
		goto out;
	}

	/* 추가할 LBA 범위가 기존 파티션과 겹치는지 검사한다. */
	if (partition_overlaps(disk, start, length, -1)) {
		ret = -EBUSY;
		goto out;
	}

	part = add_partition(disk, partno, start, length,
			ADDPART_FLAG_NONE, NULL);
	ret = PTR_ERR_OR_ZERO(part); /* 성공 시 0, 실패 시 음수 errno */
out:
	mutex_unlock(&disk->open_mutex);
	return ret;
}

/*
 * bdev_del_partition() - 지정한 파티션을 삭제한다.
 *
 * 목적: 파티션 block_device를 part_tbl에서 제거하고 sysfs 노드를 삭제한다.
 *
 * 호출 경로: ioctl(BLKPG_DEL_PARTITION) -> bdev_del_partition()
 *           -> drop_partition()
 */
int bdev_del_partition(struct gendisk *disk, int partno)
{
	struct block_device *part = NULL;
	int ret = -ENXIO;

	mutex_lock(&disk->open_mutex);
	part = xa_load(&disk->part_tbl, partno);
	if (!part)
		goto out_unlock; /* 해당 번호의 NVMe 파티션이 존재하지 않음 */

	ret = -EBUSY;
	/* 열린 파일/블록 장치가 있으면(NVMe I/O 경로 사용 중) 삭제 실패. */
	if (atomic_read(&part->bd_openers))
		goto out_unlock;

	/*
	 * We verified that @part->bd_openers is zero above and so
	 * @part->bd_holder{_ops} can't be set. And since we hold
	 * @disk->open_mutex the device can't be claimed by anyone.
	 *
	 * So no need to call @part->bd_holder_ops->mark_dead() here.
	 * Just delete the partition and invalidate it.
	 */

	/* inode lookup 해시에서 제거하여 새로운 open을 막는다. */
	bdev_unhash(part);
	/* page cache를 무효화한다. */
	invalidate_bdev(part);
	/* part_tbl과 sysfs에서 파티션을 제거한다. */
	drop_partition(part);
	ret = 0;
out_unlock:
	mutex_unlock(&disk->open_mutex);
	return ret;
}

/*
 * bdev_resize_partition() - 파티션 크기를 조정한다.
 *
 * 목적: 시작 섹터는 유지하면서 파티션 길이만 변경하여 NVMe namespace의
 * 새로운 LBA 범위를 반영한다.
 *
 * 호출 경로: ioctl(BLKPG_RESIZE_PARTITION) -> bdev_resize_partition()
 */
int bdev_resize_partition(struct gendisk *disk, int partno, sector_t start,
		sector_t length)
{
	struct block_device *part = NULL;
	int ret = -ENXIO;

	mutex_lock(&disk->open_mutex);
	part = xa_load(&disk->part_tbl, partno);
	if (!part)
		goto out_unlock; /* 해당 NVMe 파티션 없음 */

	ret = -EINVAL;
	/* NVMe namespace 기준 시작 LBA는 변경할 수 없다. */
	if (start != part->bd_start_sect)
		goto out_unlock;

	ret = -EBUSY;
	/* 새로운 끝이 다른 파티션과 겹치는지 확인한다. */
	if (partition_overlaps(disk, start, length, partno))
		goto out_unlock;

	/* 새 길이를 반영하여 파티션의 NVMe LBA 범위를 조정한다. */
	bdev_set_nr_sectors(part, length);

	ret = 0;
out_unlock:
	mutex_unlock(&disk->open_mutex);
	return ret;
}

/*
 * disk_unlock_native_capacity() - 디스크의 숨겨진 native 용량을 해제한다.
 *
 * 목적: 일부 가상/보호된 NVMe namespace(또는 호환 장치)에서
 * 실제 플래시 용량보다 작게 보고된 경우 unlock 콜백을 통해 전체 용량을
 * 노출할 수 있다 (추정).
 *
 * NVMe 연결 지점: NVMe namespace resize 이벤트나 가상 용량 보호
 * 메커니즘에서 호출될 수 있다 (추정).
 */
static bool disk_unlock_native_capacity(struct gendisk *disk)
{
	if (!disk->fops->unlock_native_capacity ||
	    test_and_set_bit(GD_NATIVE_CAPACITY, &disk->state)) {
		printk(KERN_CONT "truncated\n");
		return false; /* unlock 콜백이 없거나 이미 unlock 시도됨 */
	}

	printk(KERN_CONT "enabling native capacity\n");
	disk->fops->unlock_native_capacity(disk); /* NVMe 용량 제한 해제 콜백 */
	return true; /* unlock 후 파티션 스캔을 재시도해야 함 */
}

/*
 * blk_add_partition() - parsed_partitions에서 파싱된 한 파티션을 추가한다.
 *
 * 목적: 파티션 테이블에서 읽은 from/size가 NVMe namespace 용량 내에
 * 들어오는지 확인하고, add_partition()을 호출한다.
 *
 * 호출 경로: blk_add_partitions() -> blk_add_partition() -> add_partition()
 */
static bool blk_add_partition(struct gendisk *disk,
		struct parsed_partitions *state, int p)
{
	sector_t size = state->parts[p].size; /* NVMe LBA 개수 단위 파티션 크기 */
	sector_t from = state->parts[p].from; /* NVMe namespace 기준 시작 LBA */
	struct block_device *part;

	if (!size)
		return true; /* 비어 있는 엔트리: 사용하지 않는 파티션 슬롯 */

	/* 파티션 시작이 NVMe namespace EOD를 넘어섰는지 확인한다. */
	if (from >= get_capacity(disk)) {
		printk(KERN_WARNING
		       "%s: p%d start %llu is beyond EOD, ",
		       disk->disk_name, p, (unsigned long long) from);
		if (disk_unlock_native_capacity(disk))
			return false; /* native 용량 해제 후 재스캔 필요 */
		return true;
	}

	/* 파티션 끝이 EOD를 넘으면 용량을 잘라내거나 native 용량을 해제한다. */
	if (from + size > get_capacity(disk)) {
		printk(KERN_WARNING
		       "%s: p%d size %llu extends beyond EOD, ",
		       disk->disk_name, p, (unsigned long long) size);

		if (disk_unlock_native_capacity(disk))
			return false; /* native 용량 해제 후 재스캔 필요 */

		/*
		 * We can not ignore partitions of broken tables created by for
		 * example camera firmware, but we limit them to the end of the
		 * disk to avoid creating invalid block devices.
		 */
	/* 깨진 파티션 테이블은 EOD까지로 제한하여 유효한 bdev를 만든다. */
		size = get_capacity(disk) - from;
	}

	/* 검증된 NVMe LBA 범위로 파티션 block_device를 생성한다. */
	part = add_partition(disk, p, from, size, state->parts[p].flags,
			     &state->parts[p].info);
	if (IS_ERR(part)) {
		if (PTR_ERR(part) != -ENXIO) {
			printk(KERN_ERR " %s: p%d could not be added: %pe\n",
			       disk->disk_name, p, part);
		}
		return true; /* 단일 파티션 추가 실패필도 나머지 스캔 계속 */
	}

	/* RAID 파티션이면 md 드라이버에 자동 탐지를 요청한다. */
	if (IS_BUILTIN(CONFIG_BLK_DEV_MD) &&
	    (state->parts[p].flags & ADDPART_FLAG_RAID))
		md_autodetect_dev(part->bd_dev);

	return true;
}

/*
 * blk_add_partitions() - 디스크 전체에 대해 파티션 테이블을 스캔하고 등록한다.
 *
 * 목적: check_partition()으로 파싱한 결과를 순회하며
 * NVMe namespace에 해당하는 파티션 block_device들을 생성한다.
 *
 * 호출 경로: bdev_disk_changed() -> blk_add_partitions()
 *           -> check_partition() -> blk_add_partition() -> add_partition()
 */
static int blk_add_partitions(struct gendisk *disk)
{
	struct parsed_partitions *state;
	int ret = -EAGAIN, p;

	/* 디스크에 파티션 스캔 플래그가 없으면 아무 것도 하지 않는다. */
	if (!disk_has_partscan(disk))
		return 0;

	/* LBA 0 등 파티션 메타데이터 영역을 읽어 파싱한다. */
	state = check_partition(disk);
	if (!state)
		return 0; /* 메모리 부족 등으로 스캔 포기, 이미 printk 가능 */
	if (IS_ERR(state)) {
		/*
		 * I/O error reading the partition table.  If we tried to read
		 * beyond EOD, retry after unlocking the native capacity.
		 */
	/* 파티션 테이블이 NVMe namespace 끝을 넘어 있으면 native 용량 해제 시도. */
		if (PTR_ERR(state) == -ENOSPC) {
			printk(KERN_WARNING "%s: partition table beyond EOD, ",
			       disk->disk_name);
			if (disk_unlock_native_capacity(disk))
				return -EAGAIN; /* unlock 성공 시 bdev_disk_changed 재스캔 */
		}
		return -EIO; /* NVMe READ CQE error 또는 page cache I/O 실패로 인식 불가 */
	}

	/*
	 * Partitions are not supported on host managed zoned block devices.
	 */
	/* Host managed zoned NVMe namespace는 파티션 테이블을 무시한다. */
	if (bdev_is_zoned(disk->part0)) {
		pr_warn("%s: ignoring partition table on host managed zoned block device\n",
			disk->disk_name);
		ret = 0;
		goto out_free_state;
	}

	/*
	 * If we read beyond EOD, try unlocking native capacity even if the
	 * partition table was successfully read as we could be missing some
	 * partitions.
	 */
	/* 일부 파티션이 EOD 너머에 있으면 native 용량 해제 후 재시도 가능. */
	if (state->access_beyond_eod) {
		printk(KERN_WARNING
		       "%s: partition table partially beyond EOD, ",
		       disk->disk_name);
		if (disk_unlock_native_capacity(disk))
			goto out_free_state; /* unlock 성공 시 상위 루프가 재스캔 */
	}

	/* tell userspace that the media / partition table may have changed */
	/* 파티션 테이블 변경을 사용자 공간에 알린다. */
	kobject_uevent(&disk_to_dev(disk)->kobj, KOBJ_CHANGE);

	/* parts[1..limit) 순회; part0은 전체 NVMe namespace 자체. */
	for (p = 1; p < state->limit; p++)
		if (!blk_add_partition(disk, state, p))
			goto out_free_state; /* native 용량 해제로 인해 재스캔 필요 */

	ret = 0;
out_free_state:
	free_partitions(state);
	return ret;
}

/*
 * bdev_disk_changed() - 미디어/디스크 상태 변경 시 파티션 테이블을 재스캔한다.
 *
 * 목적: NVMe namespace의 용량이나 미디어가 바뀌었을 때
 * 기존 파티션을 모두 무효화하고 새로운 파티션 테이블을 읽어 재등록한다.
 *
 * 호출 경로: NVMe namespace AEN/resize 이벤트 등 (추정) ->
 *           bdev_disk_changed() -> blk_add_partitions()
 *
 * NVMe 연결 지점: NVMe namespace 크기 변경(ns resize) 시 이 함수가
 * 호출되어 파티션 오프셋/용량을 재평가한다 (추정).
 */
int bdev_disk_changed(struct gendisk *disk, bool invalidate)
{
	struct block_device *part;
	unsigned long idx;
	int ret = 0;

	/* open_mutex 보유 확인: part_tbl과 bdev 상태 일관성 유지. */
	lockdep_assert_held(&disk->open_mutex);

	/* NVMe namespace가 살아있지 않으면 오류 반환. */
	if (!disk_live(disk))
		return -ENXIO;

rescan:
	/* 파티션이 아직 열여 있으면 재스캔 불가. */
	if (disk->open_partitions)
		return -EBUSY;
	/* page cache의 더티 데이터를 동기화한다. */
	sync_blockdev(disk->part0);
	/* page cache를 무효화하여 오래된 NVMe namespace 데이터를 제거한다. */
	invalidate_bdev(disk->part0);

	/* 기존 파티션 block_device들을 모두 제거한다. */
	xa_for_each_start(&disk->part_tbl, idx, part, 1) {
		/*
		 * Remove the block device from the inode hash, so that
		 * it cannot be looked up any more even when openers
		 * still hold references.
		 */
	/* inode lookup 해시에서 제거하여 새로운 open 차단. */
		bdev_unhash(part);

		/*
		 * If @disk->open_partitions isn't elevated but there's
		 * still an active holder of that block device things
		 * are broken.
		 */
		/* 열린 오프너가 있으면 안 됨 (NVMe I/O 완료 가정). */
		WARN_ON_ONCE(atomic_read(&part->bd_openers));
		/* 각 파티션의 page cache도 무효화한다. */
		invalidate_bdev(part);
		/* part_tbl과 sysfs에서 파티션을 제거한다. */
		drop_partition(part);
	}
	/* partition scan 필요 플래그를 해제한다. */
	clear_bit(GD_NEED_PART_SCAN, &disk->state);

	/*
	 * Historically we only set the capacity to zero for devices that
	 * support partitions (independ of actually having partitions created).
	 * Doing that is rather inconsistent, but changing it broke legacy
	 * udisks polling for legacy ide-cdrom devices.  Use the crude check
	 * below to get the sane behavior for most device while not breaking
	 * userspace for this particular setup.
	 */
	/* 미디어 제거 시 용량을 0으로 설정한다. */
	if (invalidate) {
		if (!(disk->flags & GENHD_FL_NO_PART) ||
		    !(disk->flags & GENHD_FL_REMOVABLE))
			set_capacity(disk, 0);
	}

	/* 용량이 남아 있으면 파티션을 재등록한다. */
	if (get_capacity(disk)) {
	/* 파티션 테이블을 다시 읽고 /dev/nvmeXnYpZ를 생성한다. */
		ret = blk_add_partitions(disk);
	/* native 용량 해제 후 다시 스캔한다. */
		if (ret == -EAGAIN)
			goto rescan;
	/* 용량이 0이면 미디어 변경/제거 uevent를 전달한다. */
	} else if (invalidate) {
		/*
		 * Tell userspace that the media / partition table may have
		 * changed.
		 */
		kobject_uevent(&disk_to_dev(disk)->kobj, KOBJ_CHANGE);
	}

	return ret;
}
/*
 * Only exported for loop and dasd for historic reasons.  Don't use in new
 * code!
 */
EXPORT_SYMBOL_GPL(bdev_disk_changed);

/*
 * read_part_sector() - 파티션 테이블이 있는 단일 섹터를 읽는다.
 *
 * 목적: NVMe namespace의 LBA n에 해당하는 섹터(512B/4KiB)를
 * page cache를 통해 읽어 파티션 포맷 프로버에 전달한다.
 *
 * 호출 경로: 각 포맷별 check_xxx() -> read_part_sector()
 *
 * NVMe 연결 지점: read_mapping_folio()는 필요 시
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe READ 명령을
 * 발생시켜 LBA n을 플래시에서 가져온다.
 */
void *read_part_sector(struct parsed_partitions *state, sector_t n, Sector *p)
{
	struct address_space *mapping = state->disk->part0->bd_mapping;
	/* NVMe namespace 전체 디스크에 대한 page cache 매핑 */
	struct folio *folio;

	/* 요청한 LBA가 NVMe namespace 용량을 넘어서면 플래그를 설정한다. */
	if (n >= get_capacity(state->disk)) {
		state->access_beyond_eod = true;
		goto out; /* EOD 외부 접근: NVMe READ 명령 발생 없이 실패 처리 */
	}

	/* LBA n을 page cache folio로 매핑; 캐시 미스 시 NVMe READ가 발생한다. */
	folio = read_mapping_folio(mapping, n >> PAGE_SECTORS_SHIFT, NULL);
	if (IS_ERR(folio))
		goto out; /* NVMe READ CQE error 또는 메모리 할당 실패로 읽기 실패 */

	p->v = folio; /* 읽은 folio를 호출자가 나중에 put_sector()로 해제 */
	/* folio 내에서 sector 오프셋에 해당하는 커널 주소를 반환한다. */
	return folio_address(folio) + offset_in_folio(folio, n * SECTOR_SIZE);
	/*
	 * n * SECTOR_SIZE: NVMe LBA n을 바이트 오프셋으로 변환
	 * (sector_t) * 512B: NVMe namespace의 논리 블록 크기가 512B일 때 직접 매핑
	 */
out:
	/* 읽기 실패 또는 EOD 초과 시 NULL을 반환한다. */
	p->v = NULL;
	return NULL;
}
/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 NVMe namespace(gendisk) 위에 /dev/nvmeXnYpZ 파티션
 *   block_device를 생성/삭제/재스캔하는 partition layer의 핵심이다.
 * - 파티션은 bd_start_sect 오프셋을 가지며, 실제 NVMe I/O는
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서
 *   해당 오프셋이 더해진 LBA로 변환된다.
 * - 파티션 테이블 읽기(read_part_sector)는 page cache를 통해
 *   NVMe namespace의 LBA 0(GPT/MBR) 등을 읽는 첫 I/O를 유발한다.
 * - 파티션 추가/삭제/크기조정은 gendisk->part_tbl과 open_mutex를
 *   기준으로 동기화되며, 열린 NVMe I/O 경로가 있으면 삭제가
 *   거부된다.
 * - host managed zoned NVMe namespace에는 파티션을 지원하지 않는다.
 */
