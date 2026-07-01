// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 HUAWEI
 * Author: Cai Zhiyong <caizhiyong@huawei.com>
 *
 * Read block device partition table from the command line.
 * Typically used for fixed block (eMMC) embedded devices.
 * It has no MBR, so saves storage space. Bootloader can be easily accessed
 * by absolute address of data on the block device.
 * Users can easily change the partition.
 *
 * The format for the command line is just like mtdparts.
 *
 * For further information, see "Documentation/block/cmdline-partition.rst"
 *
 */

/*
 * NVMe 관점 파일 요약
 *   - 이 파일은 커널 커맨드라인("blkdevparts=")에서 블록 장치의 파티션 테이블을
 *     파싱하여 block layer에 등록한다.
 *   - NVMe SSD를 포함한 모든 block device에 적용 가능하며, 파티션이 나뉜 LBA
 *     범위는 이후 NVMe Read/Write 명령의 시작 LBA와 전송 크기로 변환된다.
 *   - 파티션 검색 흐름: rescan_partitions -> check_partition ->
 *     cmdline_partition -> ... (이 파일은 drivers/nvme/host 와 같은 NVMe
 *     드라이버보다 상위 generic partition layer에 위치한다)
 *   - I/O 경로 예시: submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *     nvme_queue_rq -> nvme_submit_cmd(doorbell).
 *   - cmdline partition은 MBR/GPT와 달리 NVMe media에서 sector 단위로
 *     partition table을 read_part_sector()로 읽어오지 않는다. 대신 부트로더나
 *     커널 파라미터로 전달된 문자열을 신뢰(trust)하여 파티션을 구성한다.
 */
#include <linux/blkdev.h>	/* block_device, gendisk, submit_bio 등 NVMe 상위 layer 공용 헤더 */
#include <linux/fs.h>		/* block layer가 파일시스템과 연결되는 구조체 포함 */
#include <linux/slab.h>		/* kzalloc_obj/kfree: 파티션 메타데이터 할당, 실패 시 scan 중단 */
#include "check.h"		/* parsed_partitions, put_partition 등 partition layer 인터페이스 */


/* partition flags */
#define PF_RDONLY                   0x01 /* Device is read only */
#define PF_POWERUP_LOCK             0x02 /* Always locked after reset */

/*
 * struct cmdline_subpart
 *   - 하나의 파티션(예: NVMe namespace 위의 'rootfs')을 표현한다.
 *   - name: 파티션 볼륨 이름. 파일시스템이나 /dev/disk/by-partlabel 등에서
 *           사용되며, NVMe namespace label과는 별개다.
 *   - from: 파티션 시작 오프셋(바이트 단위). block layer가 이를 512바이트
 *           섹터로 변환하면 NVMe Read/Write 명령의 시작 LBA가 된다.
 *   - size: 파티션 크기(바이트 단위). NVMe 명령에서 전송할 수 있는 최대
 *           바이트 수의 상한이 된다.
 *   - flags: PF_RDONLY가 설정되면 해당 범위로의 쓰기(Write, Flush 등)를
 *            block layer 수준에서 억제한다.
 *   - next_subpart: 동일한 block device(예: nvme0n1) 내의 다음 파티션을
 *                   연결하는 단방향 연결 리스트.
 */
struct cmdline_subpart {
	char name[BDEVNAME_SIZE]; /* partition name, such as 'rootfs' */
	sector_t from;		/* NVMe Read/Write 시작 LBA 계산의 기반이 되는 byte offset */
	sector_t size;		/* NVMe transfer length 상한(byte), LBA 범위 = size >> 9 */
	int flags;		/* PF_RDONLY 시 NVMe Write/Flush/Write Zeroes 차단 */
	struct cmdline_subpart *next_subpart;	/* 같은 namespace 내 partition chain */
};

/*
 * struct cmdline_parts
 *   - 하나의 block device(예: "nvme0n1")에 속한 모든 파티션 목록을 담는다.
 *   - name: 커맨드라인에서 지정한 block device 이름. NVMe namespace 장치명
 *           (nvme0n1 등)과 일치할 수 있다.
 *   - nr_subparts: 파티션 개수.
 *   - subpart: 첫 번째 cmdline_subpart. 이를 따라가면 namespace 전체 파티션
 *              구성을 알 수 있다.
 *   - next_parts: 다음 block device의 파티션 집합을 가리킨다.
 */
struct cmdline_parts {
	char name[BDEVNAME_SIZE]; /* block device, such as 'mmcblk0' */
	unsigned int nr_subparts;	/* NVMe namespace 위에 노출될 minor/pN 개수 */
	struct cmdline_subpart *subpart;	/* partition entry 연결 리스트의 head */
	struct cmdline_parts *next_parts;	/* 다음 block device의 partition set */
};

/*
 * parse_subpart
 *   - 하나의 파티션 정의(예: "4G@8G(rootfs)")를 파싱하여 cmdline_subpart를
 *     생성한다.
 *   - 호출 경로: cmdline_parts_parse -> parse_parts -> parse_subpart.
 *   - NVMe 연결: 파싱된 from/size가 add_part에서 512바이트 섹터로 변환되어
 *     NVMe Read/Write의 시작 LBA와 길이로 사용된다.
 */
static int parse_subpart(struct cmdline_subpart **subpart, char *partdef)
{
	int ret = 0;
	struct cmdline_subpart *new_subpart;

	*subpart = NULL;

	new_subpart = kzalloc_obj(struct cmdline_subpart);
	if (!new_subpart)	/* NVMe namespace metadata 할당 실패 -> partition scan 중단 */
		return -ENOMEM;

	if (*partdef == '-') {
		/* '-'는 나머지 디스크 용량을 모두 사용하겠다는 의미 (추정) */
		new_subpart->size = (sector_t)(~0ULL);	/* namespace 끝까지 확장, 이후 cmdline_parts_set에서 자름 */
		partdef++;
	} else {
		new_subpart->size = (sector_t)memparse(partdef, &partdef);
		/* PAGE_SIZE 미만은 의미 없는 파티션으로 간주한다 (추정) */
		if (new_subpart->size < (sector_t)PAGE_SIZE) {
			pr_warn("cmdline partition size is invalid.");
			ret = -EINVAL;
			goto fail;
		}
	}

	if (*partdef == '@') {
		partdef++;
		new_subpart->from = (sector_t)memparse(partdef, &partdef);
		/* @ 뒤의 offset이 NVMe namespace LBA 0 기준 byte offset이 됨 */
	} else {
		/* '@'가 없으면 이전 파티션 뒤부터 자동 배치 (추정) */
		new_subpart->from = (sector_t)(~0ULL);		/* marker: 이후 cmdline_parts_set에서 연산 */
	}

	if (*partdef == '(') {
		partdef++;
		char *next = strsep(&partdef, ")");

		if (!next) {
			pr_warn("cmdline partition format is invalid.");
			ret = -EINVAL;
			goto fail;
		}

		strscpy(new_subpart->name, next, sizeof(new_subpart->name));
		/* volname은 /dev/disk/by-partlabel -> nvme0n1pN 링크 생성 시 사용 */
	} else
		new_subpart->name[0] = '\0';

	new_subpart->flags = 0;

	if (!strncmp(partdef, "ro", 2)) {
		/* 읽기 전용: NVMe Write/Flush 등 쓰기 경로를 막는다 */
		new_subpart->flags |= PF_RDONLY;
		partdef += 2;
	}

	if (!strncmp(partdef, "lk", 2)) {
		new_subpart->flags |= PF_POWERUP_LOCK;
		partdef += 2;
	}

	*subpart = new_subpart;
	return 0;
fail:
	kfree(new_subpart);	/* 할당된 metadata를 해제하고 partition scan 실패 전파 */
	return ret;
}

/*
 * free_subpart
 *   - 하나의 block device에 속한 모든 cmdline_subpart를 해제한다.
 *   - parse_parts 실패 시나 cmdline_parts_free에서 호출된다.
 */
static void free_subpart(struct cmdline_parts *parts)
{
	struct cmdline_subpart *subpart;

	while (parts->subpart) {
		subpart = parts->subpart;
		parts->subpart = subpart->next_subpart;		/* chain을 따라 다음 partition metadata로 이동 */
		kfree(subpart);		/* NVMe와 무관한 in-memory metadata 해제 */
	}
}

/*
 * parse_parts
 *   - "device:part1,part2,..." 형태의 문자열을 파싱한다.
 *   - 호출 경로: cmdline_parts_parse -> parse_parts -> parse_subpart.
 *   - NVMe 연결: device 이름이 "nvme0n1"과 일치하면 이 장치의 namespace가
 *     지정한 파티션들로 분할된다.
 */
static int parse_parts(struct cmdline_parts **parts, char *bdevdef)
{
	int ret = -EINVAL;
	char *next;
	struct cmdline_subpart **next_subpart;
	struct cmdline_parts *newparts;

	*parts = NULL;

	newparts = kzalloc_obj(struct cmdline_parts);
	if (!newparts)		/* block device metadata 할당 실패 -> scan abort */
		return -ENOMEM;

	next = strsep(&bdevdef, ":");
	if (!next) {
		pr_warn("cmdline partition has no block device.");
		goto fail;
	}

	strscpy(newparts->name, next, sizeof(newparts->name));
	/* name이 "nvme0n1"이면 이 parsed partitions이 NVMe namespace에 적용됨 */
	newparts->nr_subparts = 0;

	next_subpart = &newparts->subpart;

	while ((next = strsep(&bdevdef, ","))) {
		ret = parse_subpart(next_subpart, next);
		if (ret)
			goto fail;

		newparts->nr_subparts++;		/* NVMe namespace 위의 minor/pN 카운트 증가 */
		next_subpart = &(*next_subpart)->next_subpart;
		/* extended partition chain과 유사하게 다음 entry를 연결 (추정) */
	}

	if (!newparts->subpart) {
		pr_warn("cmdline partition has no valid partition.");
		ret = -EINVAL;
		goto fail;
	}

	*parts = newparts;

	return 0;
fail:
	free_subpart(newparts);
	kfree(newparts);		/* 파싱 실패 시 NVMe에 등록되기 전에 정리 */
	return ret;
}

/*
 * cmdline_parts_free
 *   - 파싱된 모든 block device의 파티션 목록을 메모리에서 제거한다.
 */
static void cmdline_parts_free(struct cmdline_parts **parts)
{
	struct cmdline_parts *next_parts;

	while (*parts) {
		next_parts = (*parts)->next_parts;
		free_subpart(*parts);
		kfree(*parts);
		*parts = next_parts;
	}
}

/*
 * cmdline_parts_parse
 *   - "blkdevparts=" 전체 문자열을 ';' 단위로 분할하여 모든 장치 파티션을
 *     파싱한다.
 *   - 호출 경로: cmdline_partition -> cmdline_parts_parse -> parse_parts.
 *   - 부팅 시 한 번 수행되며, 이후 bdev_parts 전역 변수에 결과가 저장된다.
 */
static int cmdline_parts_parse(struct cmdline_parts **parts,
		const char *cmdline)
{
	int ret;
	char *buf;
	char *pbuf;
	char *next;
	struct cmdline_parts **next_parts;

	*parts = NULL;

	pbuf = buf = kstrdup(cmdline, GFP_KERNEL);
	if (!buf)		/* 커맨드라인 복사 실패: NVMe partition 등록 자체가 불가 */
		return -ENOMEM;

	next_parts = parts;

	while ((next = strsep(&pbuf, ";"))) {
		ret = parse_parts(next_parts, next);
		if (ret)
			goto fail;

		next_parts = &(*next_parts)->next_parts;
		/* 다음 block device(예: nvme1n1)의 partition set으로 이동 */
	}

	if (!*parts) {
		pr_warn("cmdline partition has no valid partition.");
		ret = -EINVAL;
		goto fail;
	}

	ret = 0;
done:
	kfree(buf);
	return ret;

fail:
	cmdline_parts_free(parts);
	goto done;
}

/*
 * cmdline_parts_find
 *   - 파싱 결과에서 주어진 block device 이름(예: "nvme0n1")에 해당하는
 *     cmdline_parts를 찾는다.
 *   - 호출 경로: cmdline_partition -> cmdline_parts_find.
 */
static struct cmdline_parts *cmdline_parts_find(struct cmdline_parts *parts,
						 const char *bdev)
{
	while (parts && strncmp(bdev, parts->name, sizeof(parts->name)))
		parts = parts->next_parts;	/* 연결 리스트를 순회하며 NVMe namespace명 매칭 */
	return parts;
}

static char *cmdline;		/* __setup("blkdevparts=")로 저장된 원본 커맨드라인 */
static struct cmdline_parts *bdev_parts;	/* 파싱된 모든 block device의 partition set 리스트 */

/*
 * add_part
 *   - 파싱된 하나의 파티션을 parsed_partitions 상태에 등록한다.
 *   - 호출 경로: cmdline_parts_set -> add_part.
 *   - NVMe 연결: put_partition이 성공하면 block device partition이 생성되어
 *     이후 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *     nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe 명령이
 *     전달된다.
 */
static int add_part(int slot, struct cmdline_subpart *subpart,
		struct parsed_partitions *state)
{
	struct partition_meta_info *info;

	if (slot >= state->limit)
		return 1;		/* 최대 파티션 개수 초과, NVMe namespace당 minor 한도 도달 */

	/* from/size(바이트)를 512바이트 섹터 단위로 변환하여 등록 */
	put_partition(state, slot, subpart->from >> 9,
		      subpart->size >> 9);
	/* put_partition -> add_partition -> bdget_disk: /dev/nvme0n1pN 생성 */
	/* >>9는 LBA 변환: byte offset/512 = NVMe PRP/SGL이 참조하는 시작 LBA */

	if (subpart->flags & PF_RDONLY)
		state->parts[slot].flags |= ADDPART_FLAG_READONLY;
	/* READONLY 시 NVMe Write/Flush/Write Zeroes/DSM Trim이 block layer에서 reject */

	info = &state->parts[slot].info;

	strscpy(info->volname, subpart->name, sizeof(info->volname));

	seq_buf_printf(&state->pp_buf, "(%s)", info->volname);

	state->parts[slot].has_info = true;

	return 0;
}

/*
 * cmdline_parts_set
 *   - 찾은 block device의 모든 파티션을 disk 크기에 맞춰 조정하고 등록한다.
 *   - '@' 생략 시 이전 파티션 끝에서부터 연속 배치한다.
 *   - NVMe 연결: 설정된 from/size 범위가 NVMe namespace 총용량을 초과하지
 *     않도록 자른다. 이 범위가 NVMe 명령의 유효 LBA 범위가 된다.
 */
static int cmdline_parts_set(struct cmdline_parts *parts, sector_t disk_size,
		struct parsed_partitions *state)
{
	sector_t from = 0;		/* namespace LBA 0에 대응하는 byte 커서 */
	struct cmdline_subpart *subpart;
	int slot = 1;			/* minor 1부터 시작, nvme0n1p1, p2, ... */

	for (subpart = parts->subpart; subpart;
	     subpart = subpart->next_subpart, slot++) {
		/* 파티션 entry를 순회: 각 iteration은 곧 NVMe namespace 위의 pN 하나 */
		if (subpart->from == (sector_t)(~0ULL))
			subpart->from = from;	/* 시작 주소 자동 계산 (추정) */
		else
			from = subpart->from;	/* 명시적 시작 주소로 커서 이동 */

		if (from >= disk_size)
			break;		/* NVMe namespace 총용량을 초과하는 파티션은 중단 */

		if (subpart->size > (disk_size - from))
			subpart->size = disk_size - from;
		/* namespace 끝을 넘는 size는 유효 LBA 범위로 클리핑 */

		from += subpart->size;

		if (add_part(slot, subpart, state))
			break;		/* minor/slot 한도 초과 시 partition chain 중단 */
	}

	return slot;
}

/*
 * cmdline_parts_setup
 *   - 커널 커맨드라인 "blkdevparts=..." 매개변수를 전역 cmdline 변수에
 *     저장한다.
 */
static int __init cmdline_parts_setup(char *s)
{
	cmdline = s;		/* 실제 파싱은 cmdline_partition에서 지연 수행 */
	return 1;
}
__setup("blkdevparts=", cmdline_parts_setup);

/*
 * has_overlaps
 *   - 두 섹터 범위가 겹치는지 검사한다.
 *   - NVMe 연결: 겹치는 파티션에 동시에 쓰기를 발행하면 block layer는
 *     순서를 보장하지만, NVMe controller 입장에서는 동일 LBA에 대한
 *     경쟁(race) 가능성이 있어 데이터 무결성 위험이 커진다.
 */
static bool has_overlaps(sector_t from, sector_t size,
			 sector_t from2, sector_t size2)
{
	sector_t end = from + size;		/* 첫 번째 파티션의 끝 LBA(byte) */
	sector_t end2 = from2 + size2;		/* 두 번째 파티션의 끝 LBA(byte) */

	if (from >= from2 && from < end2)
		return true;

	if (end > from2 && end <= end2)
		return true;

	if (from2 >= from && from2 < end)
		return true;

	if (end2 > from && end2 <= end)
		return true;

	return false;
}

static inline void overlaps_warns_header(void)
{
	pr_warn("Overlapping partitions are used in command line partitions.");
	pr_warn("Don't use filesystems on overlapping partitions:");
}

/*
 * cmdline_parts_verifier
 *   - 등록된 파티션들 간의 LBA 범위 중첩 여부를 검사하고 경고한다.
 *   - 호출 경로: cmdline_partition -> cmdline_parts_verifier.
 */
static void cmdline_parts_verifier(int slot, struct parsed_partitions *state)
{
	int i;
	bool header = true;

	for (; slot < state->limit && state->parts[slot].has_info; slot++) {
		/* 등록된 파티션 entry를 순회, 각각은 NVMe namespace 위의 pN */
		for (i = slot+1; i < state->limit && state->parts[i].has_info;
		     i++) {
			if (has_overlaps(state->parts[slot].from,
					 state->parts[slot].size,
					 state->parts[i].from,
					 state->parts[i].size)) {
				if (header) {
					header = false;
					overlaps_warns_header();
				}
				pr_warn("%s[%llu,%llu] overlaps with "
					"%s[%llu,%llu].",
					state->parts[slot].info.volname,
					(u64)state->parts[slot].from << 9,
					(u64)state->parts[slot].size << 9,
					state->parts[i].info.volname,
					(u64)state->parts[i].from << 9,
					(u64)state->parts[i].size << 9);
				/* <<9는 512-byte sector를 byte로 환산, NVMe LBA 경계 표시 */
			}
		}
	}
}

/*
 * Purpose: allocate cmdline partitions.
 * Returns:
 * -1 if unable to read the partition table
 *  0 if this isn't our partition table
 *  1 if successful
 *
 * cmdline_partition
 *   - block layer의 파티션 스캐너에서 호출되는 진입점이다.
 *   - 호출 경로: rescan_partitions -> check_partition ->
 *     cmdline_partition. check_partition 낸에서 msdos.c, gpt.c 등 다른
 *     파티션 타입도 함께 시도한다.
 *   - NVMe 연결: 이 함수가 반환한 파티션 정보는 NVMe namespace block device
 *     (nvme0n1 등)에 하위 파티션(nvme0n1p1 등)으로 노출되며, 해당
 *     파티션으로의 I/O는 nvme_queue_rq를 통해 NVMe submission queue에
 *     CID, PRP/SGL, doorbell과 함께 기록된다.
 */
int cmdline_partition(struct parsed_partitions *state)
{
	sector_t disk_size;
	struct cmdline_parts *parts;

	if (cmdline) {
		if (bdev_parts)
			cmdline_parts_free(&bdev_parts);
		/* 기존 파싱 결과를 초기화하고 새 커맨드라인으로 재파싱 */

		if (cmdline_parts_parse(&bdev_parts, cmdline)) {
			cmdline = NULL;
			return -1;	/* 파싱 실패: NVMe 파티션 등록 불가, scan abort */
		}
		cmdline = NULL;
	}

	if (!bdev_parts)
		return 0;	/* cmdline partition table이 없으면 다른 파티션 타입(msdos/gpt) 시도 */

	parts = cmdline_parts_find(bdev_parts, state->disk->disk_name);
	if (!parts)
		return 0;	/* 이 NVMe namespace에 대한 blkdevparts 정의가 없음 */

	/* get_capacity는 512바이트 섹터 수를 반환하므로 바이트로 변환 */
	disk_size = get_capacity(state->disk) << 9;
	/* disk_size = NVMe namespace 전체 byte 용량, FLBA range upper bound로 사용 */

	cmdline_parts_set(parts, disk_size, state);
	/* 파티션 등록: 각 put_partition -> /dev/nvme0n1pN 생성 */

	cmdline_parts_verifier(1, state);
	/* 중첩 검사: 동일 LBA에 대한 NVMe Write race 가능성 경고 */

	seq_buf_puts(&state->pp_buf, "\n");

	return 1;
}

/* NVMe 관점 핵심 요약
 * - 이 파일은 커맨드라인으로부터 블록 장치 파티션을 생성하는 generic layer에
 *   속하며, NVMe SSD에도 nvme0n1pN 형태로 적용될 수 있다.
 * - 파티션의 from/size는 512바이트 섹터 단위로 변환되어 NVMe Read/Write
 *   명령의 시작 LBA와 길이가 된다.
 * - PF_RDONLY 플래그는 NVMe 쓰기 명령(Write, Flush, Write Zeroes 등)이
 *   발행되기 전에 block layer에서 막는다.
 * - submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 파티션 I/O가 NVMe
 *   controller로 전달된다.
 * - 겹치는 파티션은 동일 LBA에 대한 동시 쓰기가 controller/firmware 수준에서
 *   race를 일으킬 수 있으므로 경고한다.
 * - cmdline partition은 read_part_sector()를 통해 NVMe media에서
 *   partition table을 읽지 않으므로 magic/signature/checksum 검증이나
 *   CHS 변환은 발생하지 않는다. 파티션 정보는 부트로더/커널 파라미터로
 *   전달된 문자열을 신뢰(trust)한다.
 */
