// SPDX-License-Identifier: GPL-2.0-only
/*
 * Code for looking up block devices in the early boot code before mounting the
 * root file system.
 *
 * 이 파일은 부팅 초기에 루트 파일 시스템을 마운트하기 전에
 * 블록 장치 이름(PARTUUID, PARTLABEL, /dev/nvme*, major:minor 등)을
 * 실제 dev_t(device number)로 변환하는 역할을 한다.
 *
 * NVMe SSD 관점에서는 이른 시점에 호스트 커널이
 * "nvme0n1p2" 같은 장치를 찾아내어, 이 장치에 대한
 * blk_mq_hw_ctx -> nvme_queue -> SQ/CQ/doorbell 경로가 구성되기 전
 * 루트 장치를 결정하는 관문(gate)에 해당한다.
 *
 * blk_lookup_devt 등은 block_class 내의 gendisk 객체를 순회하며,
 * 이후 nvme 드라이버의 probe() -> nvme_alloc_ns() -> add_disk()로
 * 등록된 NVMe namespace의 메타데이터와 매칭시킨다.
 */
#include <linux/blkdev.h>
#include <linux/ctype.h>

/* NVMe namespace UUID 기반으로 장치를 식별할 때 사용하는 비교용 구조체.
 * 이 구조체는 block_class 내 gendisk->part_tbl의 파티션 메타정보와
 * 비교되며, NVMe의 Identify Namespace/Identify Controller에서
 * 얻어진 UUID와 매칭된다.
 */
struct uuidcmp {
	const char *uuid;	/* 비교 대상 UUID 문자열 (NVMe EUI64/NGUID/UUID 형식일 수 있음) */
	int len;		/* uuid의 길이. PARTUUID=.../PARTNROFF=... 형식에서 슬래시 기준으로 길이 계산 */
};

/**
 * match_dev_by_uuid - callback for finding a partition using its uuid
 * @dev:	device passed in by the caller
 * @data:	opaque pointer to the desired struct uuidcmp to match
 *
 * Returns 1 if the device matches, and 0 otherwise.
 *
 * 목적:
 *   block_class 순회 중 각 block_device(bdev)의 bd_meta_info->uuid를
 *   NVMe rootfs 파티션의 UUID와 비교한다.
 *
 * 호출 경로 (추정):
 *   early_lookup_bdev("PARTUUID=...") -> devt_from_partuuid ->
 *   class_find_device -> match_dev_by_uuid
 *
 * NVMe 연결점:
 *   NVMe SSD의 Identify Namespace 데이터(예: EUI-64, NGUID, UUID)가
 *   파티션 테이블(주로 GPT)의 UUID와 연결되어, 이 콜백을 통해
 *   루트 장치로 지정된 namespace를 찾아낸다.
 */
static int __init match_dev_by_uuid(struct device *dev, const void *data)
{
	struct block_device *bdev = dev_to_bdev(dev);
	const struct uuidcmp *cmp = data;

	/* bd_meta_info가 없으면 UUID 메타데이터가 없는 장치이므로 매칭 불가 */
	if (!bdev->bd_meta_info ||
	    strncasecmp(cmp->uuid, bdev->bd_meta_info->uuid, cmp->len))
		return 0;
	return 1;
}

/**
 * devt_from_partuuid - looks up the dev_t of a partition by its UUID
 * @uuid_str:	char array containing ascii UUID
 * @devt:	dev_t result
 *
 * The function will return the first partition which contains a matching
 * UUID value in its partition_meta_info struct.  This does not search
 * by filesystem UUIDs.
 *
 * If @uuid_str is followed by a "/PARTNROFF=%d", then the number will be
 * extracted and used as an offset from the partition identified by the UUID.
 *
 * Returns 0 on success or a negative error code on failure.
 *
 * 목적:
 *   "root=PARTUUID=..." 커널 인자를 해석하여 루트 블록 장치의
 *   major:minor(dev_t)를 얻는다.
 *
 * 호출 경로:
 *   early_lookup_bdev -> devt_from_partuuid ->
 *   class_find_device(&block_class, ..., &match_dev_by_uuid)
 *
 * NVMe 연결점:
 *   NVMe SSD는 보통 nvme0n1p1, nvme0n1p2처럼 namespace 단위로
 *   파티셔닝되며, 이 함수가 GPT UUID를 이용해 올바른 파티션을
 *   선택하게 된다. 이 선택된 bdev가 나중에 nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell)으로 I/O가 전달된다.
 */
static int __init devt_from_partuuid(const char *uuid_str, dev_t *devt)
{
	struct uuidcmp cmp;
	struct device *dev = NULL;
	int offset = 0;
	char *slash;

	cmp.uuid = uuid_str;

	slash = strchr(uuid_str, '/');
	/* Check for optional partition number offset attributes. */
	if (slash) {
		char c = 0;

		/* Explicitly fail on poor PARTUUID syntax. */
		if (sscanf(slash + 1, "PARTNROFF=%d%c", &offset, &c) != 1)
			goto out_invalid;
		cmp.len = slash - uuid_str;	/* 슬래시 앞부분만 UUID로 취급 */
	} else {
		cmp.len = strlen(uuid_str);	/* 전체 문자열이 UUID */
	}

	if (!cmp.len)
		goto out_invalid;

	/* block_class에 등록된 NVMe namespace/파티션 중 UUID가 일치하는 bdev 검색 */
	dev = class_find_device(&block_class, NULL, &cmp, &match_dev_by_uuid);
	if (!dev)
		return -ENODEV;

	if (offset) {
		/*
		 * Attempt to find the requested partition by adding an offset
		 * to the partition number found by UUID.
		 */
		*devt = part_devt(dev_to_disk(dev),
				  bdev_partno(dev_to_bdev(dev)) + offset);
	} else {
		*devt = dev->devt;	/* UUID로 직접 찾은 NVMe 파티션의 dev_t */
	}

	put_device(dev);
	return 0;

out_invalid:
	pr_err("VFS: PARTUUID= is invalid.\n"
	       "Expected PARTUUID=<valid-uuid-id>[/PARTNROFF=%%d]\n");
	return -EINVAL;
}

/**
 * match_dev_by_label - callback for finding a partition using its label
 * @dev:	device passed in by the caller
 * @data:	opaque pointer to the label to match
 *
 * Returns 1 if the device matches, and 0 otherwise.
 *
 * 목적:
 *   GPT 파티션 레이블(volname)과 "root=PARTLABEL=..." 인자를 비교하여
 *   NVMe namespace 내의 특정 파티션을 식별한다.
 *
 * 호출 경로:
 *   early_lookup_bdev("PARTLABEL=...") -> devt_from_partlabel ->
 *   class_find_device -> match_dev_by_label
 *
 * NVMe 연결점:
 *   NVMe SSD의 namespace 위에 생성된 GPT 파티션의 레이블을
 *   이용해 루트 장치를 지정할 수 있다. UUID 대신 사용자가
 *   가독성 높은 이름으로 NVMe 장치를 선택하는 경우다.
 */
static int __init match_dev_by_label(struct device *dev, const void *data)
{
	struct block_device *bdev = dev_to_bdev(dev);
	const char *label = data;

	if (!bdev->bd_meta_info || strcmp(label, bdev->bd_meta_info->volname))
		return 0;
	return 1;
}

/**
 * devt_from_partlabel - looks up dev_t by GPT partition label
 * @label:	PARTLABEL=... 에서 추출한 레이블 문자열
 * @devt:	찾은 장치의 dev_t 결과 버퍼
 *
 * 목적:
 *   레이블 기반으로 block_class에서 bdev를 검색한다.
 *
 * 호출 경로:
 *   early_lookup_bdev -> devt_from_partlabel -> class_find_device
 *
 * NVMe 연결점:
 *   NVMe namespace의 파티션 레이블을 통해 루트 장치를
 *   결정하며, 이후 해당 bdev에 대한 open -> blkdev_get_by_dev ->
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   경로로 I/O가 흘러간다.
 */
static int __init devt_from_partlabel(const char *label, dev_t *devt)
{
	struct device *dev;

	dev = class_find_device(&block_class, NULL, label, &match_dev_by_label);
	if (!dev)
		return -ENODEV;
	*devt = dev->devt;
	put_device(dev);
	return 0;
}

/**
 * blk_lookup_devt - block_class에서 disk 이름으로 dev_t를 검색
 * @name:	디스크 이름 (예: "nvme0n1")
 * @partno:	찾고자 하는 파티션 번호 (0이면 전체 디스크)
 *
 * 목적:
 *   /dev/nvme0n1 같은 장치 이름을 block_class 내의 gendisk와
 *   매칭하여 dev_t를 반환한다.
 *
 * 호출 경로:
 *   devt_from_devname -> blk_lookup_devt
 *
 * NVMe 연결점:
 *   NVMe 드라이버가 probe 중에 nvme_alloc_ns()를 통해 생성한
 *   gendisk(이름: nvmeXnY)가 block_class에 등록되어 있어야
 *   이 함수에서 검색할 수 있다. gendisk는 이후 blk_mq_ops를
 *   통해 NVMe submission queue(SQ)로 bio를 전달하는 핵심 객체다.
 */
static dev_t __init blk_lookup_devt(const char *name, int partno)
{
	dev_t devt = MKDEV(0, 0);
	struct class_dev_iter iter;
	struct device *dev;

	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);
	while ((dev = class_dev_iter_next(&iter))) {
		struct gendisk *disk = dev_to_disk(dev);

		if (strcmp(dev_name(dev), name))
			continue;

		if (partno < disk->minors) {
			/* We need to return the right devno, even
			 * if the partition doesn't exist yet.
			 */
			devt = MKDEV(MAJOR(dev->devt),
				     MINOR(dev->devt) + partno);
		} else {
			devt = part_devt(disk, partno);
			if (devt)
				break;
		}
	}
	class_dev_iter_exit(&iter);
	return devt;
}

/**
 * devt_from_devname - "/dev/..." 형식의 이름을 dev_t로 변환
 * @name:	"/dev/" 이후의 이름 (예: "nvme0n1p2")
 * @devt:	결과 dev_t 버퍼
 *
 * 목적:
 *   /dev/nvme0n1p2 같은 전통적인 장치 경로를 파싱하여
 *   block_class에서 해당 gendisk를 찾는다.
 *
 * 호출 경로:
 *   early_lookup_bdev("/dev/...") -> devt_from_devname ->
 *   blk_lookup_devt
 *
 * NVMe 연결점:
 *   NVMe 장치 이름은 보통 "nvme0n1", "nvme0n1p2" 형태다.
 *   이름 끝의 숫자가 파티션 번호로 해석되며, 이 파티션은
 *   NVMe namespace의 LBA 범위를 나눈 것이다.
 */
static int __init devt_from_devname(const char *name, dev_t *devt)
{
	int part;
	char s[32];
	char *p;

	if (strlen(name) > 31)
		return -EINVAL;
	strcpy(s, name);
	/* /dev/ 경로의 슬래시를 뱅(!)으로 변환: block_class의 dev_name과 형식 맞춤 */
	for (p = s; *p; p++) {
		if (*p == '/')
			*p = '!';
	}

	*devt = blk_lookup_devt(s, 0);
	if (*devt)
		return 0;

	/*
	 * Try non-existent, but valid partition, which may only exist after
	 * opening the device, like partitioned md devices.
	 */
	while (p > s && isdigit(p[-1]))
		p--;
	if (p == s || !*p || *p == '0')
		return -ENODEV;

	/* try disk name without <part number> */
	part = simple_strtoul(p, NULL, 10);
	*p = '\0';
	*devt = blk_lookup_devt(s, part);
	if (*devt)
		return 0;

	/* try disk name without p<part number> */
	if (p < s + 2 || !isdigit(p[-2]) || p[-1] != 'p')
		return -ENODEV;
	p[-1] = '\0';
	*devt = blk_lookup_devt(s, part);
	if (*devt)
		return 0;
	return -ENODEV;
}

/**
 * devt_from_devnum - major:minor 또는 16진수 형식의 dev_t 파싱
 * @name:	"major:minor" 또는 "b302" 같은 문자열
 * @devt:	결과 dev_t 버퍼
 *
 * 목적:
 *   /dev/nvme0n1p2 같은 이름 없이 직접 major:minor를 지정했을 때
 *   dev_t를 만든다.
 *
 * NVMe 연결점:
 *   NVMe 블록 장치의 major(예: 259 블록 장치)와 minor를
 *   직접 지정할 수 있다. 이 dev_t는 이후 bdev 캐시 조회나
 *   blkdev_open에서 NVMe namespace의 gendisk와 연결된다.
 */
static int __init devt_from_devnum(const char *name, dev_t *devt)
{
	unsigned maj, min, offset;
	char *p, dummy;

	if (sscanf(name, "%u:%u%c", &maj, &min, &dummy) == 2 ||
	    sscanf(name, "%u:%u:%u:%c", &maj, &min, &offset, &dummy) == 3) {
		*devt = MKDEV(maj, min);
		if (maj != MAJOR(*devt) || min != MINOR(*devt))
			return -EINVAL;
	} else {
		*devt = new_decode_dev(simple_strtoul(name, &p, 16));
		if (*p)
			return -EINVAL;
	}

	return 0;
}

/*
 *	Convert a name into device number.  We accept the following variants:
 *
 *	1) <hex_major><hex_minor> device number in hexadecimal represents itself
 *         no leading 0x, for example b302.
 *	3) /dev/<disk_name> represents the device number of disk
 *	4) /dev/<disk_name><decimal> represents the device number
 *         of partition - device number of disk plus the partition number
 *	5) /dev/<disk_name>p<decimal> - same as the above, that form is
 *	   used when disk name of partitioned disk ends on a digit.
 *	6) PARTUUID=00112233-4455-6677-8899-AABBCCDDEEFF representing the
 *	   unique id of a partition if the partition table provides it.
 *	   The UUID may be either an EFI/GPT UUID, or refer to an MSDOS
 *	   partition using the format SSSSSSSS-PP, where SSSSSSSS is a zero-
 *	   filled hex representation of the 32-bit "NT disk signature", and PP
 *	   is a zero-filled hex representation of the 1-based partition number.
 *	7) PARTUUID=<UUID>/PARTNROFF=<int> to select a partition in relation to
 *	   a partition with a known unique id.
 *	8) <major>:<minor> major and minor number of the device separated by
 *	   a colon.
 *	9) PARTLABEL=<name> with name being the GPT partition label.
 *	   MSDOS partitions do not support labels!
 *
 *	If name doesn't have fall into the categories above, we return (0,0).
 *	block_class is used to check if something is a disk name. If the disk
 *	name contains slashes, the device name has them replaced with
 *	bangs.
 */

/**
 * early_lookup_bdev - 부팅 초기에 루트 블록 장치 이름을 dev_t로 변환
 * @name:	커널 인자로 전달된 장치 이름 (예: "PARTUUID=...", "/dev/nvme0n1p2")
 * @devt:	변환된 dev_t 결과
 *
 * 목적:
 *   root= 커널 인자를 파싱하여 루트 블록 장치의 major:minor를 얻는다.
 *
 * 호출 경로 (추정):
 *   name_to_dev_t (init/do_mounts.c) -> early_lookup_bdev ->
 *   devt_from_partuuid | devt_from_partlabel | devt_from_devname | devt_from_devnum
 *
 * NVMe 연결점:
 *   NVMe SSD가 시스템의 루트 장치일 경우, 이 함수가
 *   커널이 NVMe 하드웨어를 드라이빙하기 전에도 루트 장치를
 *   식별할 수 있도록 한다. 이는 NVMe 드라이버가 이후
 *   PCI probe -> nvme_reset_work -> nvme_alloc_queue_pairs ->
 *   nvme_create_io_queues -> add_disk() 순으로 gendisk를
 *   등록하기 전에도 initramfs 마운트를 가능하게 한다.
 */
int __init early_lookup_bdev(const char *name, dev_t *devt)
{
	if (strncmp(name, "PARTUUID=", 9) == 0)
		return devt_from_partuuid(name + 9, devt);
	if (strncmp(name, "PARTLABEL=", 10) == 0)
		return devt_from_partlabel(name + 10, devt);
	if (strncmp(name, "/dev/", 5) == 0)
		return devt_from_devname(name + 5, devt);
	return devt_from_devnum(name, devt);
}

/**
 * bdevt_str - dev_t를 인쇄 가능한 문자열로 변환
 * @devt:	변환할 장치 번호
 * @buf:	결과 버퍼 (BDEVT_SIZE 바이트)
 *
 * 목적:
 *   printk_all_partitions에서 파티션 목록을 출력할 때
 *   major:minor를 16진수 문자열로 표현한다.
 *
 * NVMe 연결점:
 *   NVMe 블록 장치의 major:minor를 "259:0" 또는 "03a:00000"
 *   같은 형태로 출력하여, 루트 마운트 실패 시 디버깅에 사용된다.
 */
static char __init *bdevt_str(dev_t devt, char *buf)
{
	if (MAJOR(devt) <= 0xff && MINOR(devt) <= 0xff) {
		char tbuf[BDEVT_SIZE];
		snprintf(tbuf, BDEVT_SIZE, "%02x%02x", MAJOR(devt), MINOR(devt));
		snprintf(buf, BDEVT_SIZE, "%-9s", tbuf);
	} else
		snprintf(buf, BDEVT_SIZE, "%03x:%05x", MAJOR(devt), MINOR(devt));

	return buf;
}

/*
 * print a full list of all partitions - intended for places where the root
 * filesystem can't be mounted and thus to give the victim some idea of what
 * went wrong
 */

/**
 * printk_all_partitions - 등록된 모든 블록 파티션을 출력
 *
 * 목적:
 *   루트 파일 시스템 마운트에 실패했을 때, 현재 시스템에
 *   등록된 모든 블록 장치/파티션 목록을 출력하여 진단 정보를 제공한다.
 *
 * 호출 경로 (추정):
 *   mount_block_root 실패 시 -> printk_all_partitions
 *
 * NVMe 연결점:
 *   NVMe SSD가 루트 장치로 인식되지 않았을 때, 이 함수를 통해
 *   nvme0n1, nvme0n1p1, nvme0n1p2 등이 정상적으로 block_class에
 *   등록되었는지 확인할 수 있다. 장치가 보이지 않으면
 *   NVMe 컨트롤러 probe, queue pair 생성, doorbell 초기화,
 *   또는 namespace 스캔 단계에서 문제가 있을 수 있다 (추정).
 */
void __init printk_all_partitions(void)
{
	struct class_dev_iter iter;
	struct device *dev;

	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);
	while ((dev = class_dev_iter_next(&iter))) {
		struct gendisk *disk = dev_to_disk(dev);
		struct block_device *part;
		char devt_buf[BDEVT_SIZE];
		unsigned long idx;

		/*
		 * Don't show empty devices or things that have been
		 * suppressed
		 */
		if (get_capacity(disk) == 0 || (disk->flags & GENHD_FL_HIDDEN))
			continue;

		/*
		 * Note, unlike /proc/partitions, I am showing the numbers in
		 * hex - the same format as the root= option takes.
		 */
		rcu_read_lock();
		xa_for_each(&disk->part_tbl, idx, part) {
			if (!bdev_nr_sectors(part))
				continue;
			printk("%s%s %10llu %pg %s",
			       bdev_is_partition(part) ? "  " : "",
			       bdevt_str(part->bd_dev, devt_buf),
			       bdev_nr_sectors(part) >> 1, part,
			       part->bd_meta_info ?
					part->bd_meta_info->uuid : "");
			if (bdev_is_partition(part))
				printk("\n");
			else if (dev->parent && dev->parent->driver)
				printk(" driver: %s\n",
					dev->parent->driver->name);
			else
				printk(" (driver?)\n");
		}
		rcu_read_unlock();
	}
	class_dev_iter_exit(&iter);
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 커널이 NVMe SSD를 I/O 드라이빙하기 전,
 *   루트 장치 이름(PARTUUID, PARTLABEL, /dev/nvme0n1p2 등)을
 *   dev_t로 해석하는 초기 관문이다.
 *
 * - block_class에 등록된 gendisk(예: nvme0n1)는 NVMe 드라이버의
 *   add_disk() 이후에만 존재하므로, 이 함수들은 NVMe probe
 *   완료 후에야 성공적으로 장치를 찾을 수 있다.
 *
 * - UUID/레이블 매칭은 GPT 파티션 메타데이터(bd_meta_info)를
 *   사용하며, NVMe Identify Namespace의 UUID/NGUID/EUI64와
 *   개념적으로 연결된다.
 *
 * - 이른 시점의 장치 식별이 끝나면, 이후 일반 I/O 경로인
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) -> SQ/CQ를 통해 NVMe 명령이 전달된다.
 *
 * - 관련 블록 계층 파일: block/genhd.c(gendisk 등록/해제),
 *   block/partitions/(파티션 스캔), drivers/nvme/host/core.c 및
 *   pci.c(NVMe 컨트롤러/namespace 초기화)와 논리적으로 연결된다.
 */
