// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/msdos.c
 *
 *  Code extracted from drivers/block/genhd.c
 *  Copyright (C) 1991-1998  Linus Torvalds
 *
 *  Thanks to Branko Lankester, lankeste@fwi.uva.nl, who found a bug
 *  in the early extended-partition checks and added DM partitions
 *
 *  Support for DiskManager v6.0x added by Mark Lord,
 *  with information provided by OnTrack.  This now works for linux fdisk
 *  and LILO, as well as loadlin and bootln.  Note that disks other than
 *  /dev/hda *must* have a "DOS" type 0x51 partition in the first slot (hda1).
 *
 *  More flexible handling of extended partitions - aeb, 950831
 *
 *  Check partition table on IDE disks for common CHS translations
 *
 *  Re-organised Feb 1998 Russell King
 *
 *  BSD disklabel support by Yossi Gottlieb <yogo@math.tau.ac.il>
 *  updated by Marc Espie <Marc.Espie@openbsd.org>
 *
 *  Unixware slices support by Andrzej Krzysztofowicz <ankry@mif.pg.gda.pl>
 *  and Krzysztof G. Baranowski <kgb@knm.org.pl>
 */

/*
 * ============================================================================
 * NVMe(SSD) 관점 파일 요약
 * ============================================================================
 * 이 파일은 디스크의 첫 번째 섹터(LBA 0)와 확장 파티션 체인을 해석하여
 * MS-DOS(MBR) 스타일 파티션 테이블을 파싱한다. NVMe SSD 입장에서는
 * nvme_scan_ns()가 네임스페이스를 등록한 뒤 rescan_partitions() ->
 * check_partition() -> msdos_partition() 순서로 호출되며, 이 시점에서
 * 읽는 LBA는 아직 사용자 I/O가 아닌 커널의 디스크 탐색 단계다.
 * 결정된 파티션 경계(start, size)는 나중에 submit_bio ->
 * blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 경로에서 NVMe CID/SQ/CQ/PRP/SGL 단위로
 * 변환될 LBA 범위가 된다. 논리적으로는 block/partitions/check.c의
 * read_part_sector()/put_partition()과 block/genhd.c의 디스크 등록
 * 흐름을 따른다.
 * ============================================================================
 */
#include <linux/msdos_fs.h>
#include <linux/msdos_partition.h>

#include "check.h"
#include "efi.h"

/*
 * Many architectures don't like unaligned accesses, while
 * the nr_sects and start_sect partition table entries are
 * at a 2 (mod 4) address.
 */
#include <linux/unaligned.h>

/*
 * msdos_partition 구조체( include/linux/msdos_partition.h 참고 )의
 * unaligned 32비트 LE 필드 nr_sects를 sector_t로 변환한다.
 * NVMe 관점: PRP/SGL에 담길 LBA 카운트 계산 전, 파티션 크기를
 * sector(512B) 단위로 얻는 지점이다.
 */
static inline sector_t nr_sects(struct msdos_partition *p)
{
	/* msdos_partition.nr_sects(오프셋 12, LE32)를 sector_t로 읽는다. 이 값은 NVMe namespace LBA count로 환산되기 전 512B 섹터 단위 크기다. */
	return (sector_t)get_unaligned_le32(&p->nr_sects);
}

/*
 * msdos_partition::start_sect를 sector_t로 변환한다.
 * NVMe 관점: 이 값이 NVMe Read/Write 명령의 SLBA(Starting LBA)로
 * 전환될 수 있는 파티션 오프셋이다.
 */
static inline sector_t start_sect(struct msdos_partition *p)
{
	/* msdos_partition.start_sect(오프셋 8, LE32)를 sector_t로 읽는다. NVMe Read/Write 명령의 SLBA(Starting LBA) 후보가 된다. */
	return (sector_t)get_unaligned_le32(&p->start_sect);
}

/*
 * 주어진 파티션 항목이 DOS/Windows/Linux 확장 파티션인지 판별한다.
 * NVMe 관점: 확장 파티션은 실제 데이터를 담지 않고 다음 EBR(Extended
 * Boot Record) LBA를 가리키는 링크 노드이므로, PRP List로 읽어야
 * 할 중첩된 메타데이터 영역이다.
 */
static inline int is_extended_partition(struct msdos_partition *p)
{
	return (p->sys_ind == DOS_EXTENDED_PARTITION ||
		p->sys_ind == WIN98_EXTENDED_PARTITION ||
		p->sys_ind == LINUX_EXTENDED_PARTITION);
}

#define MSDOS_LABEL_MAGIC1	0x55
#define MSDOS_LABEL_MAGIC2	0xAA

/*
 * 부트 섹터 마지막 2바이트(오프셋 510, 511)가 0x55 0xAA인지 확인한다.
 * NVMe 관점: LBA 0을 read_part_sector -> submit_bio -> ... ->
 * nvme_submit_cmd(doorbell)로 읽은 뒤 버퍼 +510 에서 시그니처를
 * 검사하여 MBR/FAT 여부를 판단한다.
 */
static inline int
msdos_magic_present(unsigned char *p)
{
	return (p[0] == MSDOS_LABEL_MAGIC1 && p[1] == MSDOS_LABEL_MAGIC2);
}

/* Value is EBCDIC 'IBMA' */
#define AIX_LABEL_MAGIC1	0xC9
#define AIX_LABEL_MAGIC2	0xC2
#define AIX_LABEL_MAGIC3	0xD4
#define AIX_LABEL_MAGIC4	0xC1

/*
 * AIX 디스크 라벨이 존재하는지 검사한다.
 * 호출 경로: msdos_partition()에서 먼저 호출되어 MSDOS 55aa가 없는
 * AIX 디스크를 식별한다.
 * NVMe 관점: LBA 0과 LBA 7을 read_part_sector로 읽으며, 각 읽기는
 * blk-mq 레이어를 거쳐 nvme_queue_rq -> nvme_submit_cmd(doorbell)로
 * 매핑된다. AIX라면 이후 aix_partition()로 넘어간다(해당 파일 참고).
 */
static int aix_magic_present(struct parsed_partitions *state, unsigned char *p)
{
	struct msdos_partition *pt = (struct msdos_partition *) (p + 0x1be);
	Sector sect;
	unsigned char *d;
	int slot, ret = 0;

	if (!(p[0] == AIX_LABEL_MAGIC1 &&
		p[1] == AIX_LABEL_MAGIC2 &&
		p[2] == AIX_LABEL_MAGIC3 &&
		p[3] == AIX_LABEL_MAGIC4))
		return 0;

	/*
	 * Assume the partition table is valid if Linux partitions exists.
	 * Note that old Solaris/x86 partitions use the same indicator as
	 * Linux swap partitions, so we consider that a Linux partition as
	 * well.
	 */
	/* 4개의 기본 파티션 항목을 순회한다. 각 항목은 NVMe namespace의 LBA 공간에 대한 16바이트 서술자이며, 이 루프는 on-disk 메타데이터를 CPU 구조체로 해석한다. */
	for (slot = 1; slot <= 4; slot++, pt++) {
		if (pt->sys_ind == SOLARIS_X86_PARTITION ||
		    pt->sys_ind == LINUX_RAID_PARTITION ||
		    pt->sys_ind == LINUX_DATA_PARTITION ||
		    pt->sys_ind == LINUX_LVM_PARTITION ||
		    is_extended_partition(pt))
			return 0;
	}
	/* (추정) LBA 7에 위치한 LVM 시그니처를 추가로 확인한다. */
	/* LBA 7을 read_part_sector()로 읽는다. 이 읽기 역시 blk-mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe Read 명령이 된다. */
	d = read_part_sector(state, 7, &sect);
	if (d) {
		/* LVM 시그니처('_LVM')가 있으면 AIX LVM 디스크로 판정한다. */
		if (d[0] == '_' && d[1] == 'L' && d[2] == 'V' && d[3] == 'M')
			ret = 1;
		/* 섹터 버퍼를 해제하여 read_part_sector에서 할당한 NVMe Read용 메모리를 반납한다. */
		put_dev_sector(sect);
	}
	return ret;
}

/*
 * 파티션 슬롯에 UUID 형식의 디스크 시그니처 기반 정보를 기록한다.
 * 호출 경로: msdos_partition() -> put_partition() 이후 또는
 * parse_extended() 내부에서 호출됨.
 * NVMe 관점: NVMe namespace의 UUID와는 별개로, MBR의 4바이트
 * 디스크 시그니처(disksig)만 사용하여 /dev/disk/by-uuid 등에 노출될
 * 정보를 채운다.
 */
static void set_info(struct parsed_partitions *state, int slot,
		     u32 disksig)
{
	struct partition_meta_info *info = &state->parts[slot].info;

	/* 4바이트 disksig와 slot 번호를 이용해 UUID 형식 문자열을 만든다. NVMe namespace UUID와는 별개로 MBR 시그니처만 사용한다. */
	snprintf(info->uuid, sizeof(info->uuid), "%08x-%02x", disksig,
		 slot);
	/* 볼륨명은 비워 둔다. */
	info->volname[0] = 0;
	/* 파티션 메타 정보가 채워졌음을 표시하여 block/genhd.c의 디스크 등록 단계에서 노출된다. */
	state->parts[slot].has_info = true;
}

/*
 * Create devices for each logical partition in an extended partition.
 * The logical partitions form a linked list, with each entry being
 * a partition table with two entries.  The first entry
 * is the real data partition (with a start relative to the partition
 * table start).  The second is a pointer to the next logical partition
 * (with a start relative to the entire extended partition).
 * We do not create a Linux partition for the partition tables, but
 * only for the actual data partitions.
 */

/*
 * 확장 파티션 내의 논리 파티션 링크드 리스트를 따라 파싱한다.
 * 호출 경로: msdos_partition() -> parse_extended()
 * NVMe 연결점:
 *   - EBR(Extended Boot Record) 섹터를 read_part_sector()로 반복 읽는다.
 *     read_part_sector -> submit_bio -> blk_mq_submit_bio ->
 *     blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   - each EBR describes one logical partition's start/size in sectors.
 *   - 검증된 (next, size) 쌍은 put_partition()으로 등록되어 이후
 *     사용자 I/O에서 NVMe Read/Write CID의 SLBA/NB 단위로 사용된다.
 */
static void parse_extended(struct parsed_partitions *state,
			   sector_t first_sector, sector_t first_size,
			   u32 disksig)
{
	struct msdos_partition *p;
	Sector sect;
	unsigned char *data;
	sector_t this_sector, this_size;
	sector_t sector_size;
	int loopct = 0;		/* number of links followed
				   without finding a data partition */
	int i;

	/*
	 * NVMe 네임스페이스의 논리 블록 크기(일반적으로 512B)를 512로 나누어
	 * MBR의 512B-sector 기준 값을 NVMe LBA 단위로 환산한다.
	 */
	sector_size = queue_logical_block_size(state->disk->queue) / 512;
	/* this_sector는 현재 탐색 중인 EBR의 NVMe LBA 절대 주소다. */
	this_sector = first_sector;
	/* this_size는 현재 확장 파티션 영역의 NVMe LBA 길이다. */
	this_size = first_size;

	while (1) {
		/* 무한 루프 방지: 논리 파티션 체인이 100개를 초과하면 중단. */
		if (++loopct > 100)
			return;
		/* 등록 가능한 파티션 슬롯(state->limit)을 모두 소진하면 탐색을 중단한다. 이는 NVMe namespace 위에 생성될 block device 개수의 상한이다. */
		if (state->next == state->limit)
			return;
		/* 현재 EBR 섹터를 읽는다: NVMe Admin/IO 큐를 통해 Read 수행. */
		data = read_part_sector(state, this_sector, &sect);
		/* 메모리 할당 실패나 I/O 에러 시 data가 NULL이다. 이때는 파티션 스캔을 중단하며, NVMe CQE 에러(추정)가 발생한 경우를 포함한다. */
		if (!data)
			return;

		/* EBR 섹터의 마지막 2바이트(오프셋 510)에서 0x55AA 시그니처를 검증한다. 잘못된 값이면 체인 탐색을 종료한다. */
		if (!msdos_magic_present(data + 510))
			goto done;

		/* EBR 내의 파티션 테이블은 오프셋 0x1be에 위치한다. */
		p = (struct msdos_partition *) (data + 0x1be);

		/*
		 * Usually, the first entry is the real data partition,
		 * the 2nd entry is the next extended partition, or empty,
		 * and the 3rd and 4th entries are unused.
		 * However, DRDOS sometimes has the extended partition as
		 * the first entry (when the data partition is empty),
		 * and OS/2 seems to use all four entries.
		 */

		/*
		 * First process the data partition(s)
		 */
		for (i = 0; i < 4; i++, p++) {
			sector_t offs, size, next;

			/* 항목 크기가 0이거나 확장 파티션이면 실제 데이터 파티션이 아니므로 걸러낸다. */
			if (!nr_sects(p) || is_extended_partition(p))
				continue;

			/* Check the 3rd and 4th entries -
			   these sometimes contain random garbage */
			/*
			 * 3, 4번 항목은 쓰레기 값이 들어 있을 수 있으므로
			 * first_sector/first_size 범위를 벗어나면 버린다.
			 * NVMe 관점: 잘못된 LBA 범위가 NVMe 컨트롤러로 날아가
			 * CQE 에러를 유발하지 않도록 막는 가드 역할이다.
			 */
			/* 항목의 start_sect는 현재 EBR 기준 상대 섹터이므로 sector_size를 곱해 NVMe LBA로 환산한다. */
			offs = start_sect(p)*sector_size;
			/* 항목의 nr_sects 역시 512B 섹터 수이므로 sector_size를 곱해 NVMe LBA 개수로 환산한다. */
			size = nr_sects(p)*sector_size;
			/* this_sector(현재 EBR의 절대 LBA)에 상대 오프셋을 더해 논리 파티션의 절대 NVMe LBA를 구한다. */
			next = this_sector + offs;
			if (i >= 2) {
				if (offs + size > this_size)
					continue;
				if (next < first_sector)
					continue;
				if (next + size > first_sector + first_size)
					continue;
			}

			/* 유효한 논리 파티션을 등록한다. */
			put_partition(state, state->next, next, size);
			/* MBR 디스크 시그니처 기반의 파티션 메타 정보를 채운다. */
			set_info(state, state->next, disksig);
			/* Linux RAID 파티션이면 ADDPART_FLAG_RAID 플래그를 설정한다. */
			if (p->sys_ind == LINUX_RAID_PARTITION)
				state->parts[state->next].flags = ADDPART_FLAG_RAID;
			loopct = 0;
			/* 파티션 슬롯 상한에 도달하면 EBR 체인 탐색을 종료한다. */
			if (++state->next == state->limit)
				goto done;
		}
		/*
		 * Next, process the (first) extended partition, if present.
		 * (So far, there seems to be no reason to make
		 *  parse_extended()  recursive and allow a tree
		 *  of extended partitions.)
		 * It should be a link to the next logical partition.
		 */
		p -= 4;
		/* 4개 항목을 다시 순회하며 확장 파티션 링크를 찾는다. 찾으면 다음 EBR로 이동하여 추가 NVMe Read를 수행한다. */
		for (i = 0; i < 4; i++, p++)
			if (nr_sects(p) && is_extended_partition(p))
				break;
		if (i == 4)
			goto done;	 /* nothing left to do */

		/* 다음 EBR로 이동: start_sect는 확장 파티션 전체 기준 상대값이다. */
		this_sector = first_sector + start_sect(p) * sector_size;
		/* 다음 확장 영역의 NVMe LBA 길이를 갱신한다. */
		this_size = nr_sects(p) * sector_size;
		/* 현재 EBR 버퍼를 해제하고 다음 루프에서 새 NVMe Read를 준비한다. */
		put_dev_sector(sect);
	}
done:
	/* 루프 종료 시점에 마지막 EBR 버퍼를 해제한다. */
	put_dev_sector(sect);
}

#define SOLARIS_X86_NUMSLICE	16
#define SOLARIS_X86_VTOC_SANE	(0x600DDEEEUL)

/*
 * Solaris x86 VTOC(Virtual Table of Contents) 내의 한 슬라이스 항목.
 * NVMe 관점: 이 슬라이스의 s_start/s_size는 나중에 NVMe Read/Write
 * PRP List를 구성할 때 SLBA와 length로 변환된다.
 */
struct solaris_x86_slice {
	__le16 s_tag;		/* ID tag of partition */
	__le16 s_flag;		/* permission flags */
	/* 슬라이스 시작 섹터: NVMe Read/Write 명령에서 사용할 SLBA 후보이다. */
	__le32 s_start;		/* start sector no of partition */
	/* 슬라이스 블록 수: PRP/SGL에 담길 LBA 개수 후보이다. */
	__le32 s_size;		/* # of blocks in partition */
};

/*
 * Solaris VTOC 메타데이터 구조체.
 * NVMe 관점: VTOC의 v_slice[] 배열은 NVMe namespace LBA 공간을
 * 여러 슬라이스로 나눈 서브 파티션 정보이며, v_sanity/v_version은
 * 컨트롤러에 Read 한 메타데이터의 무결성을 확인하는 마법값/버전이다.
 */
struct solaris_x86_vtoc {
	unsigned int v_bootinfo[3];	/* info needed by mboot */
	/* VTOC 무결성 마법값: NVMe에서 읽은 메타데이터가 Solaris VTOC인지 검증한다. */
	__le32 v_sanity;		/* to verify vtoc sanity */
	/* VTOC 레이아웃 버전: 호환 가능한 구조체 해석을 보장한다. */
	__le32 v_version;		/* layout version */
	char	v_volume[8];		/* volume name */
	/* 섹터 크기(바이트): NVMe namespace LBA 크기와 일치해야 한다(추정). */
	__le16	v_sectorsz;		/* sector size in bytes */
	/* 슬라이스 개수: 뒤따를 v_slice[] 항목 수를 결정한다. */
	__le16	v_nparts;		/* number of partitions */
	unsigned int v_reserved[10];	/* free space */
	struct solaris_x86_slice
		/* 슬라이스 헤더 배열: 각 항목은 NVMe namespace LBA 공간의 한 조각을 기술한다. */
		v_slice[SOLARIS_X86_NUMSLICE]; /* slice headers */
	unsigned int timestamp[SOLARIS_X86_NUMSLICE]; /* timestamp */
	char	v_asciilabel[128];	/* for compatibility */
};

/* james@bpgc.com: Solaris has a nasty indicator: 0x82 which also
   indicates linux swap.  Be careful before believing this is Solaris. */

/*
 * Solaris x86 VTOC 슬라이스 테이블을 파싱하여 파티션을 등록한다.
 * 호출 경로: msdos_partition() 2nd pass -> parse_solaris_x86()
 * NVMe 연결점:
 *   - offset+1 섹터를 read_part_sector로 읽는다.
 *   - v_sanity/v_version 검증 후 v_slice[]의 s_start/s_size를
 *     put_partition()으로 등록한다.
 *   - 이 LBA 범위는 이후 submit_bio -> blk_mq_submit_bio -> ... ->
 *     nvme_submit_cmd(doorbell) 경로에서 NVMe I/O로 변환된다.
 */
static void parse_solaris_x86(struct parsed_partitions *state,
			      sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_SOLARIS_X86_PARTITION
	Sector sect;
	struct solaris_x86_vtoc *v;
	int i;
	short max_nparts;

	/* VTOC는 해당 MS-DOS 파티션의 offset + 1 섹터에 위치한다. */
	v = read_part_sector(state, offset + 1, &sect);
	/* 읽기 실패/메모리 부족 시 VTOC 파싱을 중단한다. */
	if (!v)
		return;
	/* VTOC 무결성 마법값 검사. */
	if (le32_to_cpu(v->v_sanity) != SOLARIS_X86_VTOC_SANE) {
		/* VTOC 섹터 버퍼를 해제한다. */
		put_dev_sector(sect);
		return;
	}
	seq_buf_printf(&state->pp_buf, " %s%d: <solaris:", state->name, origin);
	/* 버전 1만 처리한다. */
	if (le32_to_cpu(v->v_version) != 1) {
		seq_buf_printf(&state->pp_buf,
			       "  cannot handle version %d vtoc>\n",
			       le32_to_cpu(v->v_version));
		/* disklabel 버퍼를 해제한다. */
		put_dev_sector(sect);
		return;
	}
	/* Ensure we can handle previous case of VTOC with 8 entries gracefully */
	max_nparts = le16_to_cpu(v->v_nparts) > 8 ? SOLARIS_X86_NUMSLICE : 8;
	/* 각 슬라이스 항목을 순회하며 유효한 크기를 가진 항목을 등록한다. */
	for (i = 0; i < max_nparts && state->next < state->limit; i++) {
		struct solaris_x86_slice *s = &v->v_slice[i];

		/* 크기가 0이면 미사용 슬라이스이므로 걸러낸다. */
		if (s->s_size == 0)
			continue;
		seq_buf_printf(&state->pp_buf, " [s%d]", i);
		/* solaris partitions are relative to current MS-DOS
		 * one; must add the offset of the current partition */
		/*
		 * Solaris 슬라이스의 s_start는 현재 MS-DOS 파티션 기준
		 * 상대 섹터이므로 NVMe LBA 절대 주소로 환산하기 위해
		 * offset을 더한다.
		 */
		put_partition(state, state->next++,
				 le32_to_cpu(s->s_start)+offset,
				 le32_to_cpu(s->s_size));
	}
	/* Unixware 메타데이터 버퍼를 해제한다. */
	put_dev_sector(sect);
	seq_buf_puts(&state->pp_buf, " >\n");
#endif
}

/* check against BSD src/sys/sys/disklabel.h for consistency */
#define BSD_DISKMAGIC	(0x82564557UL)	/* The disk magic number */
#define BSD_MAXPARTITIONS	16
#define OPENBSD_MAXPARTITIONS	16
#define BSD_FS_UNUSED		0 /* disklabel unused partition entry ID */

/*
 * BSD disklabel 구조체.
 * NVMe 관점: d_secsize는 NVMe LBA 크기(보통 512B)와 일치해야 하며,
 * d_npartitions/d_partitions[]는 namespace LBA 공간을 나눈
 * BSD 파티션 정보를 담는다. d_magic은 NVMe에서 읽어온 메타데이터의
 * 유효성을 확인하는 마법값이다.
 */
struct bsd_disklabel {
	/* BSD disklabel 마법값: NVMe에서 읽은 메타데이터의 유효성을 확인한다. */
	__le32	d_magic;		/* the magic number */
	__s16	d_type;			/* drive type */
	__s16	d_subtype;		/* controller/d_type specific */
	char	d_typename[16];		/* type name, e.g. "eagle" */
	char	d_packname[16];		/* pack identifier */
	/* 섹터 크기(바이트): NVMe LBA 크기와 일치해야 한다(추정). */
	__u32	d_secsize;		/* # of bytes per sector */
	__u32	d_nsectors;		/* # of data sectors per track */
	__u32	d_ntracks;		/* # of tracks per cylinder */
	__u32	d_ncylinders;		/* # of data cylinders per unit */
	__u32	d_secpercyl;		/* # of data sectors per cylinder */
	__u32	d_secperunit;		/* # of data sectors per unit */
	__u16	d_sparespertrack;	/* # of spare sectors per track */
	__u16	d_sparespercyl;		/* # of spare sectors per cylinder */
	__u32	d_acylinders;		/* # of alt. cylinders per unit */
	__u16	d_rpm;			/* rotational speed */
	__u16	d_interleave;		/* hardware sector interleave */
	__u16	d_trackskew;		/* sector 0 skew, per track */
	__u16	d_cylskew;		/* sector 0 skew, per cylinder */
	__u32	d_headswitch;		/* head switch time, usec */
	__u32	d_trkseek;		/* track-to-track seek, usec */
	__u32	d_flags;		/* generic flags */
#define NDDATA 5
	__u32	d_drivedata[NDDATA];	/* drive-type specific information */
#define NSPARE 5
	__u32	d_spare[NSPARE];	/* reserved for future use */
	__le32	d_magic2;		/* the magic number (again) */
	__le16	d_checksum;		/* xor of data incl. partitions */

			/* filesystem and partition information: */
	/* 파티션 테이블 항목 개수: d_partitions[] 순회 범위를 결정한다. */
	__le16	d_npartitions;		/* number of partitions in following */
	__le32	d_bbsize;		/* size of boot area at sn0, bytes */
	__le32	d_sbsize;		/* max size of fs superblock, bytes */
	/* BSD 파티션 항목: p_offset은 NVMe LBA SLBA, p_size는 LBA 개수가 된다. */
	struct	bsd_partition {		/* the partition table */
		__le32	p_size;		/* number of sectors in partition */
		/* 파티션 시작 섹터: NVMe Read/Write의 SLBA 후보이다. */
		__le32	p_offset;	/* starting sector */
		__le32	p_fsize;	/* filesystem basic fragment size */
		/* 파일시스템 타입: BSD_FS_UNUSED면 NVMe I/O 범위로 등록하지 않는다. */
		__u8	p_fstype;	/* filesystem type, see below */
		__u8	p_frag;		/* filesystem fragments per block */
		__le16	p_cpg;		/* filesystem cylinders per group */
	} d_partitions[BSD_MAXPARTITIONS];	/* actually may be more */
};

#if defined(CONFIG_BSD_DISKLABEL)
/*
 * Create devices for BSD partitions listed in a disklabel, under a
 * dos-like partition. See parse_extended() for more information.
 */

/*
 * BSD disklabel에 기술된 파티션들을 등록한다.
 * 호출 경로: msdos_partition() 2nd pass -> parse_freebsd() /
 * parse_netbsd() / parse_openbsd() -> parse_bsd()
 * NVMe 연결점:
 *   - offset+1 섹터를 read_part_sector()로 읽어 disklabel을 획득.
 *   - d_magic 검증 후 d_partitions[]의 p_offset/p_size를
 *     put_partition()으로 등록. FreeBSD는 상대 오프셋 변환이 필요.
 *   - 등록된 LBA 범위가 이후 submit_bio -> ... -> nvme_queue_rq ->
 *     nvme_submit_cmd(doorbell)로 전달된다.
 */
static void parse_bsd(struct parsed_partitions *state,
		      sector_t offset, sector_t size, int origin, char *flavour,
		      int max_partitions)
{
	Sector sect;
	struct bsd_disklabel *l;
	struct bsd_partition *p;

	/* offset + 1 LBA를 read_part_sector()로 읽어 BSD disklabel을 획득한다. 이 역시 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로 NVMe Read CID가 제출된다. */
	l = read_part_sector(state, offset + 1, &sect);
	/* 메모리 할당/Read 실패 시 서브 파티션 스캔을 중단한다. */
	if (!l)
		return;
	/* BSD disklabel 마법값 검증. */
	if (le32_to_cpu(l->d_magic) != BSD_DISKMAGIC) {
		/* Minix 파티션 첫 섹터 버퍼를 해제한다. */
		put_dev_sector(sect);
		return;
	}

	seq_buf_printf(&state->pp_buf, " %s%d: <%s:", state->name, origin, flavour);

	/* 디스크레이블에 기술된 실제 파티션 수로 순회 상한을 조정한다. */
	if (le16_to_cpu(l->d_npartitions) < max_partitions)
		max_partitions = le16_to_cpu(l->d_npartitions);
	/* d_partitions[]를 순회하며 각 항목을 NVMe namespace LBA 범위로 평가한다. */
	for (p = l->d_partitions; p - l->d_partitions < max_partitions; p++) {
		sector_t bsd_start, bsd_size;

		if (state->next == state->limit)
			break;
		/* 사용하지 않는 BSD 파티션 항목은 걸러낸다. */
		if (p->p_fstype == BSD_FS_UNUSED)
			continue;
		/* p_offset을 sector_t로 읽어 NVMe LBA 시작 주소 후보를 얻는다. */
		bsd_start = le32_to_cpu(p->p_offset);
		/* p_size를 sector_t로 읽어 NVMe LBA 개수 후보를 얻는다. */
		bsd_size = le32_to_cpu(p->p_size);
		/* FreeBSD has relative offset if C partition offset is zero */
		/* FreeBSD C 파티션 오프셋이 0이면 bsd_start가 상대값이므로
		 * NVMe LBA 절대 주소로 바꾸기 위해 offset을 더한다. */
		if (memcmp(flavour, "bsd\0", 4) == 0 &&
		    le32_to_cpu(l->d_partitions[2].p_offset) == 0)
			bsd_start += offset;
		/* 부모 파티션 전체를 덮는 항목은 이미 등록된 영역이므로 중복 등록을 피한다. */
		if (offset == bsd_start && size == bsd_size)
			/* full parent partition, we have it already */
			continue;
		/* 부모 파티션 범위를 벗어나는 서브 파티션은 잘못된 메타데이터로 보고 무시한다. 범위 초과는 NVMe CQE 에러를 유발할 수 있다(추정). */
		if (offset > bsd_start || offset+size < bsd_start+bsd_size) {
			seq_buf_puts(&state->pp_buf, "bad subpartition - ignored\n");
			continue;
		}
		/* BSD 파티션의 LBA 범위를 등록한다. */
		put_partition(state, state->next++, bsd_start, bsd_size);
	}
	/* LBA 0 버퍼를 해제하여 NVMe Read에 사용된 메모리를 반납한다. */
	put_dev_sector(sect);
	if (le16_to_cpu(l->d_npartitions) > max_partitions)
		seq_buf_printf(&state->pp_buf, " (ignored %d more)",
			       le16_to_cpu(l->d_npartitions) - max_partitions);
	seq_buf_puts(&state->pp_buf, " >\n");
}
#endif

static void parse_freebsd(struct parsed_partitions *state,
			  sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_BSD_DISKLABEL
	/* FreeBSD disklabel 파서를 호출한다. */
	parse_bsd(state, offset, size, origin, "bsd", BSD_MAXPARTITIONS);
#endif
}

static void parse_netbsd(struct parsed_partitions *state,
			 sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_BSD_DISKLABEL
	/* NetBSD disklabel 파서를 호출한다. */
	parse_bsd(state, offset, size, origin, "netbsd", BSD_MAXPARTITIONS);
#endif
}

static void parse_openbsd(struct parsed_partitions *state,
			  sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_BSD_DISKLABEL
	/* OpenBSD disklabel 파서를 호출한다. */
	parse_bsd(state, offset, size, origin, "openbsd",
		  OPENBSD_MAXPARTITIONS);
#endif
}

#define UNIXWARE_DISKMAGIC     (0xCA5E600DUL)	/* The disk magic number */
#define UNIXWARE_DISKMAGIC2    (0x600DDEEEUL)	/* The slice table magic nr */
#define UNIXWARE_NUMSLICE      16
#define UNIXWARE_FS_UNUSED     0		/* Unused slice entry ID */

/*
 * Unixware slice 항목.
 * NVMe 관점: start_sect/nr_sects는 NVMe Read/Write 명령에서 사용할
 * 슬라이스의 SLBA와 길이 정보가 된다.
 */
struct unixware_slice {
	__le16   s_label;	/* label */
	__le16   s_flags;	/* permission flags */
	/* 슬라이스 시작 섹터: NVMe Read/Write의 SLBA 후보이다. */
	__le32   start_sect;	/* starting sector */
	/* 슬라이스 섹터 수: NVMe PRP/SGL에 담길 LBA 개수 후보이다. */
	__le32   nr_sects;	/* number of sectors in slice */
};

/*
 * Unixware disklabel / VTOC 구조체.
 * NVMe 관점: d_magic/vtoc.v_magic은 NVMe에서 읽어온 메타데이터의
 * 유효성을 판별하는 마법값이며, v_slice[]는 namespace LBA 공간을
 * Unixware 슬라이스로 분할한 정보를 담는다.
 */
struct unixware_disklabel {
	__le32	d_type;			/* drive type */
	/* Unixware disklabel 마법값: NVMe 메타데이터 유효성 검증에 사용된다. */
	__le32	d_magic;		/* the magic number */
	__le32	d_version;		/* version number */
	char	d_serial[12];		/* serial number of the device */
	__le32	d_ncylinders;		/* # of data cylinders per device */
	__le32	d_ntracks;		/* # of tracks per cylinder */
	__le32	d_nsectors;		/* # of data sectors per track */
	/* 섹터 크기(바이트): NVMe LBA 크기와 일치해야 한다(추정). */
	__le32	d_secsize;		/* # of bytes per sector */
	__le32	d_part_start;		/* # of first sector of this partition*/
	__le32	d_unknown1[12];		/* ? */
	__le32	d_alt_tbl;		/* byte offset of alternate table */
	__le32	d_alt_len;		/* byte length of alternate table */
	__le32	d_phys_cyl;		/* # of physical cylinders per device */
	__le32	d_phys_trk;		/* # of physical tracks per cylinder */
	__le32	d_phys_sec;		/* # of physical sectors per track */
	__le32	d_phys_bytes;		/* # of physical bytes per sector */
	__le32	d_unknown2;		/* ? */
	__le32	d_unknown3;		/* ? */
	__le32	d_pad[8];		/* pad */

	struct unixware_vtoc {
		/* VTOC 마법값: NVMe에서 읽은 VTOC 메타데이터의 sanity 검사. */
		__le32	v_magic;		/* the magic number */
		__le32	v_version;		/* version number */
		char	v_name[8];		/* volume name */
		/* 슬라이스 개수: v_slice[] 순회 범위를 결정한다. */
		__le16	v_nslices;		/* # of slices */
		__le16	v_unknown1;		/* ? */
		__le32	v_reserved[10];		/* reserved */
		struct unixware_slice
			/* 슬라이스 헤더 배열: 각 항목은 NVMe namespace LBA 공간의 한 조각이다. */
			v_slice[UNIXWARE_NUMSLICE];	/* slice headers */
	} vtoc;
};  /* 408 */

/*
 * Create devices for Unixware partitions listed in a disklabel, under a
 * dos-like partition. See parse_extended() for more information.
 */

/*
 * Unixware VTOC 슬라이스를 파싱하여 파티션을 등록한다.
 * 호출 경로: msdos_partition() 2nd pass -> parse_unixware()
 * NVMe 연결점:
 *   - offset + 29 섹터에서 disklabel을 read_part_sector()로 읽는다.
 *   - d_magic/vtoc.v_magic 검증 후 v_slice[1..]의 start_sect/nr_sects를
 *     put_partition()으로 등록한다.
 *   - 이 LBA 범위는 이후 submit_bio -> blk_mq_submit_bio ->
 *     blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *     경로에서 NVMe I/O로 변환된다.
 */
static void parse_unixware(struct parsed_partitions *state,
			   sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_UNIXWARE_DISKLABEL
	Sector sect;
	struct unixware_disklabel *l;
	struct unixware_slice *p;

	/* Unixware disklabel/VTOC는 offset + 29 LBA에 위치한다. read_part_sector()는 이를 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로 NVMe Read 명령으로 만든다. */
	l = read_part_sector(state, offset + 29, &sect);
	/* Read 실패/메모리 부족 시 스캔을 중단한다. */
	if (!l)
		return;
	/* disklabel 및 VTOC 마법값 동시 검증. */
	if (le32_to_cpu(l->d_magic) != UNIXWARE_DISKMAGIC ||
	    le32_to_cpu(l->vtoc.v_magic) != UNIXWARE_DISKMAGIC2) {
		put_dev_sector(sect);
		return;
	}
	seq_buf_printf(&state->pp_buf, " %s%d: <unixware:", state->name, origin);
	p = &l->vtoc.v_slice[1];
	/* I omit the 0th slice as it is the same as whole disk. */
	while (p - &l->vtoc.v_slice[0] < UNIXWARE_NUMSLICE) {
		if (state->next == state->limit)
			break;

		if (p->s_label != UNIXWARE_FS_UNUSED)
			/* 0번 슬라이스는 전체 디스크이므로 제외한 슬라이스를 등록한다. start_sect는 NVMe LBA SLBA, nr_sects는 LBA 개수로 사용된다. */
			put_partition(state, state->next++,
				      le32_to_cpu(p->start_sect),
				      le32_to_cpu(p->nr_sects));
		p++;
	}
	put_dev_sector(sect);
	seq_buf_puts(&state->pp_buf, " >\n");
#endif
}

#define MINIX_NR_SUBPARTITIONS  4

/*
 * Minix 2.0.0/2.0.2 subpartition support.
 * Anand Krishnamurthy <anandk@wiproge.med.ge.com>
 * Rajeev V. Pillai    <rajeevvp@yahoo.com>
 */

/*
 * Minix 파티션의 보조 파티션 테이블을 파싱한다.
 * 호출 경로: msdos_partition() 2nd pass -> parse_minix()
 * NVMe 연결점:
 *   - Minix 파티션의 첫 섹터(offset)를 read_part_sector()로 읽는다.
 *   - 서브 파티션 항목의 start_sect/nr_sects를 put_partition()으로
 *     등록하여 NVMe I/O에서 사용할 LBA 범위를 만든다.
 */
static void parse_minix(struct parsed_partitions *state,
			sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_MINIX_SUBPARTITION
	Sector sect;
	unsigned char *data;
	struct msdos_partition *p;
	int i;

	/* Minix 파티션의 첫 번째 섹터(offset LBA)를 read_part_sector()로 읽는다. read_part_sector -> submit_bio -> ... -> nvme_submit_cmd(doorbell)로 NVMe Read CID가 제출된다. */
	data = read_part_sector(state, offset, &sect);
	/* 메모리 부족/Read 실패 시 서브 파티션 탐색을 중단한다. */
	if (!data)
		return;

	/* Minix 파티션 첫 섹터의 파티션 테이블은 오프셋 0x1be에 위치한다. */
	p = (struct msdos_partition *)(data + 0x1be);

	/* The first sector of a Minix partition can have either
	 * a secondary MBR describing its subpartitions, or
	 * the normal boot sector. */
	/* Minix 파티션 첫 섹터에 부트 시그니처 0x55AA와 MINIX_PARTITION
	 * 타입이 모두 있어야 서브 파티션 테이블로 인정한다. */
	if (msdos_magic_present(data + 510) &&
	    p->sys_ind == MINIX_PARTITION) { /* subpartition table present */
		seq_buf_printf(&state->pp_buf, " %s%d: <minix:", state->name, origin);
		/* MINIX_NR_SUBPARTITIONS(4)개의 서브 파티션 항목을 순회한다. */
		for (i = 0; i < MINIX_NR_SUBPARTITIONS; i++, p++) {
			if (state->next == state->limit)
				break;
			/* add each partition in use */
			/* 사용 중인 Minix 서브 파티션의 LBA 범위를 등록한다. */
			if (p->sys_ind == MINIX_PARTITION)
				/* put_partition()으로 Minix 서브 파티션을 NVMe namespace 위의 block device로 등록한다. */
				put_partition(state, state->next++,
					      start_sect(p), nr_sects(p));
		}
		seq_buf_puts(&state->pp_buf, " >\n");
	}
	put_dev_sector(sect);
#endif /* CONFIG_MINIX_SUBPARTITION */
}

/*
 * MS-DOS 파티션 타입(sys_ind)별 2nd pass 파서 테이블.
 * NVMe 관점: 1st pass에서 기본/확장 파티션을 등록한 뒤, 해당
 * 파티션 내의 서브 레이블(Solaris/BSD/Unixware/Minix)을 추가로
 * 스캔할 때 사용된다. 각 parse 함수는 read_part_sector를 통해
 * NVMe namespace의 메타데이터 섹터를 읽는다.
 */
static struct {
	unsigned char id;
	void (*parse)(struct parsed_partitions *, sector_t, sector_t, int);
} subtypes[] = {
	/* FREEBSD_PARTITION: FreeBSD disklabel이면 offset+1 LBA를 추가 NVMe Read한다. */
	{FREEBSD_PARTITION, parse_freebsd},
	/* NETBSD_PARTITION: NetBSD disklabel 추가 스캔. */
	{NETBSD_PARTITION, parse_netbsd},
	/* OPENBSD_PARTITION: OpenBSD disklabel 추가 스캔. */
	{OPENBSD_PARTITION, parse_openbsd},
	/* MINIX_PARTITION: Minix 서브 파티션 추가 스캔. */
	{MINIX_PARTITION, parse_minix},
	/* UNIXWARE_PARTITION: Unixware VTOC 추가 스캔. */
	{UNIXWARE_PARTITION, parse_unixware},
	/* SOLARIS_X86_PARTITION: Solaris VTOC 추가 스캔. */
	{SOLARIS_X86_PARTITION, parse_solaris_x86},
	/* NEW_SOLARIS_X86_PARTITION: 새 Solaris VTOC 추가 스캔. */
	{NEW_SOLARIS_X86_PARTITION, parse_solaris_x86},
	{0, NULL},
};

/*
 * MS-DOS(MBR) 파티션 테이블의 최상위 진입점이다.
 * 호출 경로:
 *   rescan_partitions() -> check_partition() -> msdos_partition()
 *   (NVMe namespace 초기화 시 nvme_scan_ns() -> nvme_revalidate_disk()
 *    -> rescan_partitions() 순으로 호출될 수 있음 (추정))
 * NVMe 연결점:
 *   - LBA 0을 read_part_sector()로 읽어 MBR을 획득한다.
 *   - 1st pass에서 기본 파티션과 확장 파티션 체인을 파싱하고,
 *     put_partition()으로 등록한다.
 *   - 2nd pass에서 BSD/Solaris/Unixware/Minix 등 서브 레이블을
 *     추가로 파싱한다.
 *   - 등록된 모든 LBA 범위는 이후 사용자 submit_bio ->
 *     blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *     nvme_submit_cmd(doorbell) 경로에서 NVMe CID/SQ/CQ/PRP/SGL
 *     단위로 변환된다.
 */
int msdos_partition(struct parsed_partitions *state)
{
	sector_t sector_size;
	Sector sect;
	unsigned char *data;
	struct msdos_partition *p;
	struct fat_boot_sector *fb;
	int slot;
	u32 disksig;

	/* NVMe namespace의 논리 블록 크기를 512B 기준 sector_size로 환산. */
	sector_size = queue_logical_block_size(state->disk->queue) / 512;
	/* LBA 0(MBR/부트 섹터)를 읽는다. */
	/* LBA 0(MBR/부트 섹터)를 read_part_sector()로 읽는다. read_part_sector -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 순서로 NVMe Admin/IO 큐에 Read CID가 제출된다. */
	data = read_part_sector(state, 0, &sect);
	/* 메모리 할당 실패나 NVMe Read CQE 에러(추정) 시 -1을 반환하여 파티션 스캔을 중단한다. */
	if (!data)
		return -1;

	/*
	 * Note order! (some AIX disks, e.g. unbootable kind,
	 * have no MSDOS 55aa)
	 */
	/* AIX 우선 검사: 일부 AIX 디스크는 55aa 시그니처가 없다. */
	/* LBA 0의 상위 4바이트가 AIX 라벨이면 AIX 파서로 넘긴다. 이 검사 역시 read_part_sector로 얻은 NVMe 버퍼를 대상으로 한다. */
	if (aix_magic_present(state, data)) {
		put_dev_sector(sect);
#ifdef CONFIG_AIX_PARTITION
		return aix_partition(state);
#else
		seq_buf_puts(&state->pp_buf, " [AIX]");
		return 0;
#endif
	}

	/* 0x55AA 시그니처가 없으면 MBR이 아니다. */
	/* LBA 0의 마지막 2바이트(오프셋 510)에서 0x55AA 시그니처를 검증한다. 시그니처가 없으면 MBR이 아닌 다른 포맷이다. */
	if (!msdos_magic_present(data + 510)) {
		put_dev_sector(sect);
		return 0;
	}

	/*
	 * Now that the 55aa signature is present, this is probably
	 * either the boot sector of a FAT filesystem or a DOS-type
	 * partition table. Reject this in case the boot indicator
	 * is not 0 or 0x80.
	 */
	/* MBR 파티션 테이블은 LBA 0 내 오프셋 0x1be(446)부터 64바이트(4개 항목)로 배치된다. 이 오프셋은 NVMe에서 읽은 512B 메타데이터의 고정 필드 위치다. */
	p = (struct msdos_partition *) (data + 0x1be);
	/* 4개의 기본 파티션 항목을 순회하며 boot_ind 필드가 0x00 또는 0x80인지 확인한다. 잘못된 값은 파티션 테이블이 아닌 가능성을 시사한다. */
	for (slot = 1; slot <= 4; slot++, p++) {
		/* boot_ind가 0 또는 0x80이 아니면 유효한 MBR 파티션 항목이 아니다. */
		if (p->boot_ind != 0 && p->boot_ind != 0x80) {
			/*
			 * Even without a valid boot indicator value
			 * its still possible this is valid FAT filesystem
			 * without a partition table.
			 */
			fb = (struct fat_boot_sector *) data;
			/* 1번 슬롯에서 예외적으로 FAT 부트 섹터로 보이면 파티션 테이블 없이 전체 디스크를 FAT로 처리한다. */
			if (slot == 1 && fb->reserved && fb->fats
				&& fat_valid_media(fb->media)) {
				seq_buf_puts(&state->pp_buf, "\n");
				put_dev_sector(sect);
				return 1;
			} else {
				put_dev_sector(sect);
				return 0;
			}
		}
	}

#ifdef CONFIG_EFI_PARTITION
	/* 파티션 테이블 포인터를 0x1be로 재설정한다. */
	p = (struct msdos_partition *) (data + 0x1be);
	/* EFI GPT 보호 파티션(0xEE)이 있는지 4개 항목을 다시 확인한다. */
	for (slot = 1 ; slot <= 4 ; slot++, p++) {
		/* If this is an EFI GPT disk, msdos should ignore it. */
		/* EFI GPT 보호 파티션(0xEE)이 있으면 MBR 파서가 아닌
		 * efi.c의 GPT 파서가 처리해야 하므로 종료한다. */
		if (p->sys_ind == EFI_PMBR_OSTYPE_EFI_GPT) {
			put_dev_sector(sect);
			return 0;
		}
	}
#endif
	p = (struct msdos_partition *) (data + 0x1be);

	/* MBR 오프셋 0x1b8에 있는 4바이트 디스크 시그니처를 읽는다. */
	/* MBR 오프셋 0x1b8(440)에서 4바이트 디스크 시그니처(disksig)를 읽는다. 이는 NVMe에서 읽은 on-disk 메타데이터의 고정 오프셋 값이다. */
	disksig = le32_to_cpup((__le32 *)(data + 0x1b8));

	/*
	 * Look for partitions in two passes:
	 * First find the primary and DOS-type extended partitions.
	 * On the second pass look inside *BSD, Unixware and Solaris partitions.
	 */

	/* 논리 파티션 번호는 5번부터 시작한다. */
	/* 논리 파티션 번호를 5번부터 시작한다. 1~4번은 기본 파티션 슬롯이다. */
	state->next = 5;
	/* 1st pass: 4개의 기본 파티션 항목을 순회하며 NVMe namespace LBA 범위를 추출한다. */
	for (slot = 1 ; slot <= 4 ; slot++, p++) {
		/* start_sect(p)는 MBR 512B 섹터 기준 상대 오프셋이므로 sector_size를 곱해 NVMe LBA 절대 주소로 환산한다. */
		sector_t start = start_sect(p)*sector_size;
		/* nr_sects(p)를 sector_size로 곱해 NVMe LBA 개수로 환산한다. */
		sector_t size = nr_sects(p)*sector_size;

		/* 크기가 0이면 비어 있는 파티션 항목이다. */
		if (!size)
			continue;
		/* 확장 파티션이면 EBR(Extended Boot Record) 체인을 따라 논리 파티션을 파싱한다. */
		if (is_extended_partition(p)) {
			/*
			 * prevent someone doing mkfs or mkswap on an
			 * extended partition, but leave room for LILO
			 * FIXME: this uses one logical sector for > 512b
			 * sector, although it may not be enough/proper.
			 */
			sector_t n = 2;

			/* 확장 파티션 자체는 2섹터 보호 영역만 등록하여 사용자가 실수로 mkfs하지 못하게 한다. sector_size 이상을 보장한다. */
			n = min(size, max(sector_size, n));
			/* 확장 파티션 자체는 2섹터짜리 보호용 파티션으로 등록. */
			/* put_partition()으로 확장 파티션 보호 영역을 등록한다. 이는 NVMe namespace 위의 block device로 노출되지 않는 보조 항목이다. */
			put_partition(state, slot, start, n);

			seq_buf_puts(&state->pp_buf, " <");
			/* 확장 파티션 체인을 따라 논리 파티션 파싱. */
			/* parse_extended()가 EBR 체인을 따라 반복적으로 read_part_sector -> NVMe Read를 수행하며 논리 파티션을 찾는다. 체인 길이에 따라 NVMe 명령 제출 횟수가 증가한다. */
			parse_extended(state, start, size, disksig);
			seq_buf_puts(&state->pp_buf, " >");
			continue;
		}
		/* 기본 파티션 등록: start/size는 NVMe LBA 단위. */
		/* put_partition()으로 기본 파티션을 등록한다. 등록된 (start, size)는 NVMe namespace의 LBA 범위이며, 이후 사용자 submit_bio -> ... -> nvme_submit_cmd(doorbell) 경로에서 NVMe CID의 SLBA/NB로 변환된다. */
		put_partition(state, slot, start, size);
		/* MBR disksig 기반 UUID 메타 정보를 설정한다. */
		set_info(state, slot, disksig);
		/* Linux RAID 파티션이면 ADDPART_FLAG_RAID 플래그를 설정한다. */
		if (p->sys_ind == LINUX_RAID_PARTITION)
			state->parts[slot].flags = ADDPART_FLAG_RAID;
		if (p->sys_ind == DM6_PARTITION)
			/* DiskManager 파티션 표시. */
			seq_buf_puts(&state->pp_buf, "[DM]");
		if (p->sys_ind == EZD_PARTITION)
			/* EZD 파티션 표시. */
			seq_buf_puts(&state->pp_buf, "[EZD]");
	}

	seq_buf_puts(&state->pp_buf, "\n");

	/* second pass - output for each on a separate line */
	/* 2nd pass를 위해 파티션 테이블 포인터를 다시 0x1be로 설정한다. */
	p = (struct msdos_partition *) (0x1be + data);
	/* 2nd pass: 4개 기본 파티션 항목을 다시 순회하며 BSD/Solaris/Unixware/Minix 등 서브 레이블을 추가 스캔한다. */
	for (slot = 1 ; slot <= 4 ; slot++, p++) {
		unsigned char id = p->sys_ind;
		int n;

		/* 크기가 0인 항목은 서브 레이블이 없으므로 걸러낸다. */
		if (!nr_sects(p))
			continue;

		/* subtypes[] 테이블에서 현재 파티션 타입에 해당하는 파서를 검색한다. */
		for (n = 0; subtypes[n].parse && id != subtypes[n].id; n++)
			;

		if (!subtypes[n].parse)
			continue;
		/* 서브 타입 파서 호출: 해당 파티션 내의 추가 레이블 스캔. */
		/* 서브 타입 파서를 호출한다. 파서 낶부에서 read_part_sector -> submit_bio -> ... -> nvme_submit_cmd(doorbell)로 추가 NVMe Read 명령이 제출된다. */
		subtypes[n].parse(state, start_sect(p) * sector_size,
				  /* 파서에 파티션의 NVMe LBA 시작 주소와 크기를 전달한다. */
				  nr_sects(p) * sector_size, slot);
	}
	put_dev_sector(sect);
	return 1;
}

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 * - 이 파일은 NVMe namespace가 커널에 등록된 직후, 사용자 I/O 시작 전에
 *   rescan_partitions() -> check_partition() -> msdos_partition() 경로로
 *   실행되어 MBR 파티션 경계를 결정한다.
 * - 모든 섹터 읽기는 read_part_sector()를 통해 submit_bio ->
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 순서로 NVMe Admin/IO 큐로 매핑된다.
 * - 파티션의 start_sect/nr_sects는 queue_logical_block_size()로 환산되어
 *   NVMe LBA 단위가 되며, 이후 사용자 bio가 PRP/SGL과 CID/SQ/CQ 단위로
 *   변환될 때 기준 범위가 된다.
 * - block/partitions/check.c의 read_part_sector()/put_partition() 및
 *   block/genhd.c의 디스크 등록 흐름과 논리적으로 연결된다.
 * ============================================================================
 */
