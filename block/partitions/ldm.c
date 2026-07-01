// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ldm - Support for Windows Logical Disk Manager (Dynamic Disks)
 *
 * Copyright (C) 2001,2002 Richard Russon <ldm@flatcap.org>
 * Copyright (c) 2001-2012 Anton Altaparmakov
 * Copyright (C) 2001,2002 Jakob Kemi <jakob.kemi@telia.com>
 *
 * Documentation is available at http://www.linux-ntfs.org/doku.php?id=downloads 
 */

/*
 * NVMe SSD 관점 파일 요약
 *
 * 이 파일은 Windows Logical Disk Manager(LDM, Dynamic Disk) 파티션 메타데이터를
 * 파싱하여 NVMe namespace가 노출하는 블록 장치를 논리적 파티션으로 나누는 역할을 한다.
 * 디스크 탐색(discovery) 단계에서 호출되며, read_part_sector()를 통해
 * NVMe namespace의 특정 LBA에서 512바이트 섹터를 읽어온다.
 * 읽기 요청은 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를 타고 NVMe SQ/CQ로 전달된다(추정).
 * 파서가 완료되면 put_partition()으로 gendisk에 파티션을 등록한다.
 */

#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/stringify.h>
#include <linux/kernel.h>
#include <linux/uuid.h>
#include <linux/msdos_partition.h>

#include "ldm.h"
#include "check.h"

/*
 * 주요 구조체와 NVMe 동작의 연관성
 *
 * struct privhead:
 *   - ver_major/minor: LDM 데이터베이스 버전. NVMe 입장에서는 메타데이터 형식 호환성을
 *     판단할 뿐 I/O 프로토콜에는 영향을 주지 않는다.
 *   - logical_disk_start/size: NVMe namespace 내에서 사용자 데이터 영역의 시작 LBA와
 *     크기(섹터 단위). NVMe Read/Write 명령의 SLBA와 직접 연결된다.
 *   - config_start/size: LDM 설정 데이터베이스가 위치한 NVMe LBA 범위. 이 영역에 대한
 *     read_part_sector() 요청이 NVMe PRP/SGL을 통해 DMA로 전달된다.
 *   - disk_id: 이 물리 디스크/GUID. NVMe namespace의 고유 식별자와는 별개이며
 *     LDM 그룹 내 디스크 매칭에 사용된다.
 *
 * struct tocblock:
 *   - bitmap{1,2}_start/size: VMDB/VBLK 레코드들을 담은 비트맵의 LBA 범위.
 *     NVMe SSD에서는 이 범위를 여러 개의 512바이트 Read 명령으로 읽게 된다.
 *
 * struct vmdb:
 *   - vblk_size: 각 VBLK 레코드 크기. 한 섹터(512B)에 몇 개의 VBLK가 들어가는지
 *     결정하고, NVMe Read 완료 후 버퍼를 얼마만큼씩 건너뛸지 계산한다.
 *   - vblk_offset: 데이터베이스 내 VBLK 영역 시작 오프셋(바이트). 섹터 단위로
 *     변환되어 NVMe LBA 오프셋에 더해진다.
 *   - last_vblk_seq: 마지막 VBLK 시퀀스 번호. 읽어야 할 전체 메타데이터 섹터 수를
 *     계산하는 데 사용되며, NVMe 명령 개수와 관련된다.
 *
 * struct ldmdb:
 *   - ph/toc/vm: 파싱된 PRIVHEAD/TOCBLOCK/VMDB 캐시.
 *   - v_dgrp/v_disk/v_volu/v_comp/v_part: VBLK 객체 연결 리스트. NVMe I/O 경로와는
 *     무관하지만, 이 리스트에서 추출한 파티션 정보가 최종적으로 gendisk에 반영된다.
 *
 * struct vblk / struct vblk_part:
 *   - vblk_part.start/size: NVMe namespace에서 해당 파티션의 시작 LBA와 길이.
 *   - volume_offset: 볼륨 내 오프셋. NVMe SSD가 실제로 접근할 LBA는
 *     logical_disk_start + start가 된다.
 *   - disk_id/parent_id: LDM 객체 간 관계. 현재 namespace와 일치하는 disk_id를
 *     찾아야만 이 NVMe 장치에 속한 파티션으로 등록한다.
 */

/*
 * ldm_debug/info/error/crit - Output an error message
 * @f:    A printf format string containing the message
 * @...:  Variables to substitute into @f
 *
 * ldm_debug() writes a DEBUG level message to the syslog but only if the
 * driver was compiled with debug enabled. Otherwise, the call turns into a NOP.
 */
#ifndef CONFIG_LDM_DEBUG
#define ldm_debug(...)	do {} while (0)
#else
#define ldm_debug(f, a...) _ldm_printk (KERN_DEBUG, __func__, f, ##a)
#endif

#define ldm_crit(f, a...)  _ldm_printk (KERN_CRIT,  __func__, f, ##a)
#define ldm_error(f, a...) _ldm_printk (KERN_ERR,   __func__, f, ##a)
#define ldm_info(f, a...)  _ldm_printk (KERN_INFO,  __func__, f, ##a)

static __printf(3, 4)
void _ldm_printk(const char *level, const char *function, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start (args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk("%s%s(): %pV\n", level, function, &vaf);

	va_end(args);
}

/*
 * ldm_parse_privhead()
 *   목적: NVMe namespace에서 읽어온 원시 PRIVHEAD 섹터를 파싱하여 struct privhead를 채운다.
 *   호출 경로: ldm_partition -> ldm_validate_privheads -> ldm_parse_privhead
 *   NVMe 연결: read_part_sector()로 전달된 512바이트 버퍼는 이미
 *              submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *              nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를 통해
 *              NVMe controller로부터 완료(CQ)된 데이터이다(추정).
 *              이 함수는 DMA로 받은 메타데이터의 magic, 버전, LBA 범위를 검증한다.
 *   주요 검사: MAGIC_PRIVHEAD, 버전 2.11/2.12, logical_disk_start/size 범위,
 *             config_size, disk_id UUID.
 */
/**
 * ldm_parse_privhead - Read the LDM Database PRIVHEAD structure
 * @data:  Raw database PRIVHEAD structure loaded from the device
 * @ph:    In-memory privhead structure in which to return parsed information
 *
 * This parses the LDM database PRIVHEAD structure supplied in @data and
 * sets up the in-memory privhead structure @ph with the obtained information.
 *
 * Return:  'true'   @ph contains the PRIVHEAD data
 *          'false'  @ph contents are undefined
 */
static bool ldm_parse_privhead(const u8 *data, struct privhead *ph)
{
	bool is_vista = false;
	// Windows 버전 식별 플래그; NVMe Read/Write 명령과는 무관

	BUG_ON(!data || !ph);
	// NVMe DMA로 수신한 메타데이터 버퍼/data가 NULL이면 파서 버그
	if (MAGIC_PRIVHEAD != get_unaligned_be64(data)) {
	// PRIVHEAD 시그니처 불일치는 NVMe media 메타데이터 손상 또는 CQE 오류를 의미(추정)
		ldm_error("Cannot find PRIVHEAD structure. LDM database is"
			" corrupt. Aborting.");
		return false;
	}
	ph->ver_major = get_unaligned_be16(data + 0x000C);
	// offset 0x000C: NVMe로부터 읽은 512B 섹터 내 메이저 버전
	ph->ver_minor = get_unaligned_be16(data + 0x000E);
	// offset 0x000E: LDM 데이터베이스 마이너 버전
	ph->logical_disk_start = get_unaligned_be64(data + 0x011B);
	// offset 0x011B: NVMe namespace LBA 공간에서 사용자 데이터 영역 시작(섹터 단위)
	ph->logical_disk_size = get_unaligned_be64(data + 0x0123);
	// offset 0x0123: 사용자 데이터 영역 크기(섹터); NVMe Read/Write SLBA 범위 결정
	ph->config_start = get_unaligned_be64(data + 0x012B);
	// offset 0x012B: LDM 메타데이터 영역 시작 LBA; 이후 read_part_sector의 기준
	ph->config_size = get_unaligned_be64(data + 0x0133);
	// offset 0x0133: LDM 메타데이터 영역 크기(섹터)
	/* Version 2.11 is Win2k/XP and version 2.12 is Vista. */
	if (ph->ver_major == 2 && ph->ver_minor == 12)
	// Vista(v2.12) 여부 판별; NVMe 프로토콜에는 영향 없음
		is_vista = true;
/* Windows 버전(2.11=2K/XP, 2.12=Vista)을 식별하며, NVMe SSD와는 무관한 LDM 메타데이터 형식 정보다. */
	if (!is_vista && (ph->ver_major != 2 || ph->ver_minor != 11)) {
	// 지원 버전이 아니면 NVMe media의 LDM 형식을 해석할 수 없으므로 중단
		ldm_error("Expected PRIVHEAD version 2.11 or 2.12, got %d.%d."
			" Aborting.", ph->ver_major, ph->ver_minor);
		return false;
	}
	ldm_debug("PRIVHEAD version %d.%d (Windows %s).", ph->ver_major,
			ph->ver_minor, is_vista ? "Vista" : "2000/XP");
	if (ph->config_size != LDM_DB_SIZE) {	/* 1 MiB in sectors. */
	// 표준 1MiB(2048섹터)와 다르면 NVMe namespace의 메타데이터 크기에 주의
		/* Warn the user and continue, carefully. */
		ldm_info("Database is normally %u bytes, it claims to "
			"be %llu bytes.", LDM_DB_SIZE,
			(unsigned long long)ph->config_size);
	}
/* config_size가 표준 1MiB(2048섹터)와 다르면 경고만 출력하고 계속 진행한다. */
	if ((ph->logical_disk_size == 0) || (ph->logical_disk_start +
			ph->logical_disk_size > ph->config_start)) {
			// logical_disk_start+size가 config_start를 넘으면 NVMe namespace LBA 매핑이 잘못됨
		ldm_error("PRIVHEAD disk size doesn't match real disk size");
		return false;
	}
/* logical_disk_start/size가 config_start를 넘어서면 잘못된 LBA 범위이므로 중단한다. */
	if (uuid_parse(data + 0x0030, &ph->disk_id)) {
	// offset 0x0030: 디스크 GUID; NVMe namespace UUID와는 별개(추정)
		ldm_error("PRIVHEAD contains an invalid GUID.");
		return false;
	}
	ldm_debug("Parsed PRIVHEAD successfully.");
	return true;
}

/*
 * ldm_parse_tocblock()
 *   목적: LDM 데이터베이스의 TOCBLOCK(Table of Contents)을 파싱하여
 *         bitmap1/2의 이름과 LBA 범위를 struct tocblock에 저장한다.
 *   호출 경로: ldm_partition -> ldm_validate_tocblocks -> ldm_parse_tocblock
 *   NVMe 연결: config_start 기준 오프셋(OFF_TOCB*)에 해당하는 NVMe LBA에서
 *              512바이트 섹터를 읽어온 뒤 검증한다.
 *   주요 검사: MAGIC_TOCBLOCK, bitmap1_name == "config", bitmap2_name == "log".
 */
/**
 * ldm_parse_tocblock - Read the LDM Database TOCBLOCK structure
 * @data:  Raw database TOCBLOCK structure loaded from the device
 * @toc:   In-memory toc structure in which to return parsed information
 *
 * This parses the LDM Database TOCBLOCK (table of contents) structure supplied
 * in @data and sets up the in-memory tocblock structure @toc with the obtained
 * information.
 *
 * N.B.  The *_start and *_size values returned in @toc are not range-checked.
 *
 * Return:  'true'   @toc contains the TOCBLOCK data
 *          'false'  @toc contents are undefined
 */
static bool ldm_parse_tocblock (const u8 *data, struct tocblock *toc)
{
	BUG_ON (!data || !toc);
	// NVMe로부터 읽은 TOCBLOCK DMA 버퍼 검증

	if (MAGIC_TOCBLOCK != get_unaligned_be64(data)) {
	// TOCBLOCK 시그니처 불일치는 NVMe media 손상 또는 CQE 오류(추정)
		ldm_crit ("Cannot find TOCBLOCK, database may be corrupt.");
		return false;
	}
	strscpy_pad(toc->bitmap1_name, data + 0x24, sizeof(toc->bitmap1_name));
	// offset 0x24: NVMe 섹터에서 bitmap1 이름 문자열 추출
	toc->bitmap1_start = get_unaligned_be64(data + 0x2E);
	// offset 0x2E: bitmap1 시작 LBA; NVMe namespace 내 VMDB/VBLK 위치 계산에 사용
	toc->bitmap1_size  = get_unaligned_be64(data + 0x36);
	// offset 0x36: bitmap1 크기(섹터); NVMe Read 범위 산정

	if (strncmp (toc->bitmap1_name, TOC_BITMAP1,
	// bitmap1 이름이 'config'가 아니면 NVMe에서 읽은 TOCBLOCK 손상
			sizeof (toc->bitmap1_name)) != 0) {
		ldm_crit ("TOCBLOCK's first bitmap is '%s', should be '%s'.",
				TOC_BITMAP1, toc->bitmap1_name);
		return false;
	}
	strscpy_pad(toc->bitmap2_name, data + 0x46, sizeof(toc->bitmap2_name));
	// offset 0x46: bitmap2 이름('log') 추출
	toc->bitmap2_start = get_unaligned_be64(data + 0x50);
	// offset 0x50: bitmap2 시작 LBA
	toc->bitmap2_size  = get_unaligned_be64(data + 0x58);
	// offset 0x58: bitmap2 크기(섹터)
	if (strncmp (toc->bitmap2_name, TOC_BITMAP2,
			sizeof (toc->bitmap2_name)) != 0) {
		ldm_crit ("TOCBLOCK's second bitmap is '%s', should be '%s'.",
				TOC_BITMAP2, toc->bitmap2_name);
		return false;
	}
	ldm_debug ("Parsed TOCBLOCK successfully.");
	return true;
}

/*
 * ldm_parse_vmdb()
 *   목적: LDM 데이터베이스 헤더(VMDB)를 파싱하여 VBLK 레코드 크기/오프셋/개수를 얻는다.
 *   호출 경로: ldm_partition -> ldm_validate_vmdb -> ldm_parse_vmdb
 *   NVMe 연결: VMDB가 위치한 NVMe LBA(OFF_VMDB)를 read_part_sector()로 읽고,
 *              vblk_size와 last_vblk_seq를 이용해 이후 읽을 NVMe Read 명령 수를 예측한다.
 *   주요 검사: MAGIC_VMDB, 버전 4.10, 트랜잭션 일관성(0x10 == 0x01),
 *             vblk_size != 0, vblk_size*last_vblk_seq <= bitmap1_size << 9.
 */
/**
 * ldm_parse_vmdb - Read the LDM Database VMDB structure
 * @data:  Raw database VMDB structure loaded from the device
 * @vm:    In-memory vmdb structure in which to return parsed information
 *
 * This parses the LDM Database VMDB structure supplied in @data and sets up
 * the in-memory vmdb structure @vm with the obtained information.
 *
 * N.B.  The *_start, *_size and *_seq values will be range-checked later.
 *
 * Return:  'true'   @vm contains VMDB info
 *          'false'  @vm contents are undefined
 */
static bool ldm_parse_vmdb (const u8 *data, struct vmdb *vm)
{
	BUG_ON (!data || !vm);
	// NVMe DMA 버퍼의 VMDB 헤더 검증

	if (MAGIC_VMDB != get_unaligned_be32(data)) {
	// VMDB 시그니처 불일치 시 NVMe media 손상 또는 CQE 오류(추정)
		ldm_crit ("Cannot find the VMDB, database may be corrupt.");
		return false;
	}

	vm->ver_major = get_unaligned_be16(data + 0x12);
	// offset 0x12: VMDB 메이저 버전
	vm->ver_minor = get_unaligned_be16(data + 0x14);
	// offset 0x14: VMDB 마이너 버전
	if ((vm->ver_major != 4) || (vm->ver_minor != 10)) {
		ldm_error ("Expected VMDB version %d.%d, got %d.%d. "
			"Aborting.", 4, 10, vm->ver_major, vm->ver_minor);
		return false;
	}

	vm->vblk_size     = get_unaligned_be32(data + 0x08);
	// offset 0x08: VBLK 레코드 크기; 512B NVMe 섹터당 VBLK 개수 계산
	if (vm->vblk_size == 0) {
	// VBLK 크기가 0이면 NVMe로부터 읽은 메타데이터가 손상됨
		ldm_error ("Illegal VBLK size");
		return false;
	}

	vm->vblk_offset   = get_unaligned_be32(data + 0x0C);
	// offset 0x0C: VBLK 영역 시작 바이트 오프셋; NVMe LBA 오프셋으로 변환
	vm->last_vblk_seq = get_unaligned_be32(data + 0x04);
	// offset 0x04: 마지막 VBLK 시퀀스; 읽을 NVMe 섹터 수 예측

	ldm_debug ("Parsed VMDB successfully.");
	return true;
}

/**
 * ldm_compare_privheads - Compare two privhead objects
 * @ph1:  First privhead
 * @ph2:  Second privhead
 *
 * This compares the two privhead structures @ph1 and @ph2.
 *
 * Return:  'true'   Identical
 *          'false'  Different
 */
static bool ldm_compare_privheads (const struct privhead *ph1,
				   const struct privhead *ph2)
{
	BUG_ON (!ph1 || !ph2);

	return ((ph1->ver_major          == ph2->ver_major)		&&
		(ph1->ver_minor          == ph2->ver_minor)		&&
		(ph1->logical_disk_start == ph2->logical_disk_start)	&&
		(ph1->logical_disk_size  == ph2->logical_disk_size)	&&
		(ph1->config_start       == ph2->config_start)		&&
		(ph1->config_size        == ph2->config_size)		&&
		uuid_equal(&ph1->disk_id, &ph2->disk_id));
		// PRIVHEAD의 LBA 범위와 UUID가 모두 일치해야 NVMe namespace 메타데이터를 신뢰
}

/**
 * ldm_compare_tocblocks - Compare two tocblock objects
 * @toc1:  First toc
 * @toc2:  Second toc
 *
 * This compares the two tocblock structures @toc1 and @toc2.
 *
 * Return:  'true'   Identical
 *          'false'  Different
 */
static bool ldm_compare_tocblocks (const struct tocblock *toc1,
				   const struct tocblock *toc2)
{
	BUG_ON (!toc1 || !toc2);

	return ((toc1->bitmap1_start == toc2->bitmap1_start)	&&
		(toc1->bitmap1_size  == toc2->bitmap1_size)	&&
		(toc1->bitmap2_start == toc2->bitmap2_start)	&&
		(toc1->bitmap2_size  == toc2->bitmap2_size)	&&
		!strncmp (toc1->bitmap1_name, toc2->bitmap1_name,
			sizeof (toc1->bitmap1_name))		&&
		!strncmp (toc1->bitmap2_name, toc2->bitmap2_name,
			sizeof (toc1->bitmap2_name)));
			// TOCBLOCK bitmap 범위가 일치해야 NVMe로 읽을 VBLK 위치가 확정
}

/*
 * ldm_validate_privheads()
 *   목적: 디스크에 기록된 3개의 PRIVHEAD(1차 + 2개 백업)를 읽고 서로 비교/검증한다.
 *   호출 경로: ldm_partition -> ldm_validate_privheads
 *   NVMe 연결: OFF_PRIV1/2/3 오프셋에 대해 read_part_sector()를 3회 호출한다.
 *              각 읽기는 NVMe namespace의 특정 LBA를 대상으로 하며,
 *              NVMe SQ에 CID가 할당된 Read 명령으로 변환된다(추정).
 *   주요 검사: 3개 PRIVHEAD 파싱, 용량 초과 여부, 데이터베이스와 사용자 영역 중첩,
 *             primary/backup PRIVHEAD 일치(backup 중 하나는 현재 무시).
 */
/**
 * ldm_validate_privheads - Compare the primary privhead with its backups
 * @state: Partition check state including device holding the LDM Database
 * @ph1:   Memory struct to fill with ph contents
 *
 * Read and compare all three privheads from disk.
 *
 * The privheads on disk show the size and location of the main disk area and
 * the configuration area (the database).  The values are range-checked against
 * @hd, which contains the real size of the disk.
 *
 * Return:  'true'   Success
 *          'false'  Error
 */
static bool ldm_validate_privheads(struct parsed_partitions *state,
				   struct privhead *ph1)
{
	static const int off[3] = { OFF_PRIV1, OFF_PRIV2, OFF_PRIV3 };
	// 3개 PRIVHEAD LBA 오프셋; read_part_sector 호출 3회 -> NVMe Read 3회(추정)
	struct privhead *ph[3] = { ph1 };
	Sector sect;
	u8 *data;
	bool result = false;
	long num_sects;
	int i;

	BUG_ON (!state || !ph1);

	ph[1] = kmalloc_obj(*ph[1]);
	// PRIVHEAD 백업1용 메모리; 할당 실패 시 파티션 스캔 중단
	ph[2] = kmalloc_obj(*ph[2]);
	// PRIVHEAD 백업2용 메모리; 할당 실패 시 파티션 스캔 중단
	if (!ph[1] || !ph[2]) {
		ldm_crit ("Out of memory.");
		goto out;
	}

	/* off[1 & 2] are relative to ph[0]->config_start */
	ph[0]->config_start = 0;
	// 백업 PRIVHEAD 읽을 때 상대 LBA 기준을 0으로 설정
/* 백업 PRIVHEAD 읽을 때 기준 오프셋을 0으로 만들어 상대 주소 계산을 단순화한다. */

	/* Read and parse privheads */
	for (i = 0; i < 3; i++) {
	// 3개 PRIVHEAD를 순회; 루프당 1회씩 NVMe SQ/CQ를 통한 512B Read 발생(추정)
		/* read_part_sector()는 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe LBA에서 섹터를 읽어온다(추정). */
		data = read_part_sector(state, ph[0]->config_start + off[i],
					&sect);
					// config_start+OFF_PRIV* LBA에서 NVMe 512B Read: read_part_sector -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)(추정)
		if (!data) {
		// NVMe CQE 실패/버퍼 없음 시 PRIVHEAD 검증 중단
			ldm_crit ("Disk read failed.");
			goto out;
		}
		result = ldm_parse_privhead (data, ph[i]);
		// DMA 버퍼를 PRIVHEAD 구조체로 파싱
		put_dev_sector (sect);
		// 섹터 버퍼 해제; NVMe PRP/SGL 버퍼 반납
		if (!result) {
			ldm_error ("Cannot find PRIVHEAD %d.", i+1); /* Log again */
			if (i < 2)
				goto out;	/* Already logged */
			else
				break;	/* FIXME ignore for now, 3rd PH can fail on odd-sized disks */
/* 3번째 PRIVHEAD가 홀수 크기 디스크에서 실패할 수 있어 일단 무시한다(추정). */
		}
	}

	num_sects = get_capacity(state->disk);
	// NVMe namespace의 총 섹터 수(LBA 개수)

	if ((ph[0]->config_start > num_sects) ||
	// config_start가 NVMe namespace 끝을 넘어서면 잘못된 LBA
	   ((ph[0]->config_start + ph[0]->config_size) > num_sects)) {
		ldm_crit ("Database extends beyond the end of the disk.");
		goto out;
	}

	if ((ph[0]->logical_disk_start > ph[0]->config_start) ||
	// logical_disk 영역과 config 영역이 중첩되면 NVMe LBA 충돌
	   ((ph[0]->logical_disk_start + ph[0]->logical_disk_size)
		    > ph[0]->config_start)) {
		ldm_crit ("Disk and database overlap.");
		goto out;
	}

	if (!ldm_compare_privheads (ph[0], ph[1])) {
	// 1차/백업 PRIVHEAD 불일치는 NVMe media 손상 또는 섹터 쓰기 완료 불일치(추정)
		ldm_crit ("Primary and backup PRIVHEADs don't match.");
		goto out;
	}
/* 1차 PRIVHEAD와 백업 비교; 불일치 시 LDM 메타데이터 손상 가능성이 있다. */
	/* FIXME ignore this for now
	if (!ldm_compare_privheads (ph[0], ph[2])) {
		ldm_crit ("Primary and backup PRIVHEADs don't match.");
		goto out;
	}*/
	ldm_debug ("Validated PRIVHEADs successfully.");
	result = true;
out:
	kfree (ph[1]);
	// 백업 PRIVHEAD 메모리 해제
	kfree (ph[2]);
	return result;
}

/*
 * ldm_validate_tocblocks()
 *   목적: 4개의 TOCBLOCK을 읽고 비교하여 데이터베이스 내 bitmap 위치를 검증한다.
 *   호출 경로: ldm_partition -> ldm_validate_tocblocks
 *   NVMe 연결: base + OFF_TOCB1/2/3/4 LBA에서 512바이트 메타데이터를 읽는다.
 *              Vista LDM v2.12에서는 4개가 모두 존재하지 않을 수 있어
 *              하나 이상 유효하면 진행한다.
 *   주요 검사: TOCBLOCK 파싱, bitmap 범위가 ph->config_size 이내인지,
 *             읽어들인 TOCBLOCK 간 일치 여부.
 */
/**
 * ldm_validate_tocblocks - Validate the table of contents and its backups
 * @state: Partition check state including device holding the LDM Database
 * @base:  Offset, into @state->disk, of the database
 * @ldb:   Cache of the database structures
 *
 * Find and compare the four tables of contents of the LDM Database stored on
 * @state->disk and return the parsed information into @toc1.
 *
 * The offsets and sizes of the configs are range-checked against a privhead.
 *
 * Return:  'true'   @toc1 contains validated TOCBLOCK info
 *          'false'  @toc1 contents are undefined
 */
static bool ldm_validate_tocblocks(struct parsed_partitions *state,
				   unsigned long base, struct ldmdb *ldb)
{
	static const int off[4] = { OFF_TOCB1, OFF_TOCB2, OFF_TOCB3, OFF_TOCB4};
	// 4개 TOCBLOCK LBA 오프셋; Vista에서는 일부 누락 가능
	struct tocblock *tb[4];
	struct privhead *ph;
	Sector sect;
	u8 *data;
	int i, nr_tbs;
	bool result = false;

	BUG_ON(!state || !ldb);
	ph = &ldb->ph;
	tb[0] = &ldb->toc;
	tb[1] = kmalloc_objs(*tb[1], 3);
	// TOCBLOCK 비교용 메모리 3개; 할당 실패 시 스캔 중단
	if (!tb[1]) {
		ldm_crit("Out of memory.");
		goto err;
	}
	tb[2] = (struct tocblock*)((u8*)tb[1] + sizeof(*tb[1]));
	tb[3] = (struct tocblock*)((u8*)tb[2] + sizeof(*tb[2]));
	/*
	 * Try to read and parse all four TOCBLOCKs.
	 *
	 * Windows Vista LDM v2.12 does not always have all four TOCBLOCKs so
	 * skip any that fail as long as we get at least one valid TOCBLOCK.
	 */
	for (nr_tbs = i = 0; i < 4; i++) {
	// 4개 TOCBLOCK 순회; 각 read_part_sector는 NVMe SQ/CQ 경유 512B Read(추정)
/* 4개 TOCBLOCK을 순회하며 하나라도 유효하면 진행(Vista에서는 4개 모두 없을 수 있음). */
		data = read_part_sector(state, base + off[i], &sect);
		// base+OFF_TOCB* LBA에서 NVMe 512B Read: read_part_sector -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)(추정)
		if (!data) {
		// NVMe Read 실패 시 해당 TOCBLOCK은 스킵
			ldm_error("Disk read failed for TOCBLOCK %d.", i);
			continue;
		}
		if (ldm_parse_tocblock(data, tb[nr_tbs]))
		// DMA 버퍼에서 TOCBLOCK 파싱 성공 시 카운트 증가
			nr_tbs++;
		put_dev_sector(sect);
	}
	if (!nr_tbs) {
	// 유효한 TOCBLOCK이 없으면 LDM 파싱 중단
		ldm_crit("Failed to find a valid TOCBLOCK.");
		goto err;
	}
	/* Range check the TOCBLOCK against a privhead. */
	if (((tb[0]->bitmap1_start + tb[0]->bitmap1_size) > ph->config_size) ||
	// bitmap 범위가 config_size(섹터)를 초과하면 NVMe LBA 범위 오류
			((tb[0]->bitmap2_start + tb[0]->bitmap2_size) >
			ph->config_size)) {
		ldm_crit("The bitmaps are out of range.  Giving up.");
		goto err;
	}
/* bitmap 범위가 config_size(전체 메타데이터 크기)를 초과하면 손상으로 간주한다. */
	/* Compare all loaded TOCBLOCKs. */
	for (i = 1; i < nr_tbs; i++) {
	// 읽어들인 TOCBLOCK들끼리 비교; 불일치는 NVMe media 손상 가능성
		if (!ldm_compare_tocblocks(tb[0], tb[i])) {
			ldm_crit("TOCBLOCKs 0 and %d do not match.", i);
			goto err;
		}
	}
	ldm_debug("Validated %d TOCBLOCKs successfully.", nr_tbs);
	result = true;
err:
	kfree(tb[1]);
	// TOCBLOCK 비교용 메모리 해제
	return result;
}

/*
 * ldm_validate_vmdb()
 *   목적: VMDB를 읽어 VBLK 파싱에 필요한 메타데이터 파라미터를 확보하고 일관성을 검사한다.
 *   호출 경로: ldm_partition -> ldm_validate_vmdb
 *   NVMe 연결: base + OFF_VMDB 위치의 섹터를 읽어 vblk_size, vblk_offset,
 *              last_vblk_seq를 얻고, 이 값으로 NVMe Read 루프 범위를 산정한다.
 *   주요 검사: VMDB 파싱, 트랜잭션 상태, VBLK 영역이 TOCBLOCK bitmap 크기를 초과하지 않는지.
 */
/**
 * ldm_validate_vmdb - Read the VMDB and validate it
 * @state: Partition check state including device holding the LDM Database
 * @base:  Offset, into @bdev, of the database
 * @ldb:   Cache of the database structures
 *
 * Find the vmdb of the LDM Database stored on @bdev and return the parsed
 * information in @ldb.
 *
 * Return:  'true'   @ldb contains validated VBDB info
 *          'false'  @ldb contents are undefined
 */
static bool ldm_validate_vmdb(struct parsed_partitions *state,
			      unsigned long base, struct ldmdb *ldb)
{
	Sector sect;
	u8 *data;
	bool result = false;
	struct vmdb *vm;
	struct tocblock *toc;

	BUG_ON (!state || !ldb);

	vm  = &ldb->vm;
	// VMDB 파싱 결과를 ldmdb에 저장
	toc = &ldb->toc;
	// TOCBLOCK의 bitmap 범위로 VMDB 크기 제한

	data = read_part_sector(state, base + OFF_VMDB, &sect);
	// VMDB 섹터(base+OFF_VMDB LBA)에 대한 NVMe 512B Read: read_part_sector -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)(추정)
	if (!data) {
	// NVMe CQE 실패 시 VMDB 검증 중단
		ldm_crit ("Disk read failed.");
		return false;
	}

	if (!ldm_parse_vmdb (data, vm))
	// DMA 버퍼에서 VMDB 파싱
		goto out;				/* Already logged */

	/* Are there uncommitted transactions? */
	if (get_unaligned_be16(data + 0x10) != 0x01) {
	// offset 0x10: 트랜잭션 일관성 플래그; 0x01 아니면 NVMe에 기록된 메타데이터가 불완전
		ldm_crit ("Database is not in a consistent state.  Aborting.");
		goto out;
	}
/* 0x10이 0x01이 아니면 커밋되지 않은 트랜잭션이 있어 일관성이 깨진 것으로 본다. */

	if (vm->vblk_offset != 512)
	// VBLK 시작 오프셋이 512B가 아니면 NVMe PRP/SGL 경계 정렬에 주의(추정)
		ldm_info ("VBLKs start at offset 0x%04x.", vm->vblk_offset);
/* VBLK 시작 오프셋이 512바이트가 아니면 NVMe 섹터 경계를 벗어날 수 있어 정보용 로그를 남긴다. */

	/*
	 * The last_vblkd_seq can be before the end of the vmdb, just make sure
	 * it is not out of bounds.
	 */
	if ((vm->vblk_size * vm->last_vblk_seq) > (toc->bitmap1_size << 9)) {
	// VBLK 전체 크기가 bitmap 크기를 초과하면 읽을 NVMe 섹터 범위 오류
		ldm_crit ("VMDB exceeds allowed size specified by TOCBLOCK.  "
				"Database is corrupt.  Aborting.");
		goto out;
/* VBLK 전체 크기가 TOCBLOCK bitmap 크기를 초과하면 데이터베이스가 손상됐다고 판단한다. */
	}

	result = true;
out:
	put_dev_sector (sect);
	return result;
}


/*
 * ldm_validate_partition_table()
 *   목적: 장치가 Windows Dynamic Disk인지를 느슨하게 판별하기 위해 MBR의
 *         파티션 타입 0x42(LDM_PARTITION) 존재 여부를 확인한다.
 *   호출 경로: ldm_partition -> ldm_validate_partition_table
 *   NVMe 연결: LBA 0에서 첫 번째 512바이트 섹터(MBR)를 read_part_sector()로 읽는다.
 *              이 요청은 NVMe controller의 Admin/IO SQ 중 IO queue를 통해
 *              처리될 수 있다(추정).
 *   반환: true이면 Dynamic Disk 가능성이 있으므로 LDM 파싱을 계속 진행한다.
 */
/**
 * ldm_validate_partition_table - Determine whether bdev might be a dynamic disk
 * @state: Partition check state including device holding the LDM Database
 *
 * This function provides a weak test to decide whether the device is a dynamic
 * disk or not.  It looks for an MS-DOS-style partition table containing at
 * least one partition of type 0x42 (formerly SFS, now used by Windows for
 * dynamic disks).
 *
 * N.B.  The only possible error can come from the read_part_sector and that is
 *       only likely to happen if the underlying device is strange.  If that IS
 *       the case we should return zero to let someone else try.
 *
 * Return:  'true'   @state->disk is a dynamic disk
 *          'false'  @state->disk is not a dynamic disk, or an error occurred
 */
static bool ldm_validate_partition_table(struct parsed_partitions *state)
{
	Sector sect;
	u8 *data;
	struct msdos_partition *p;
	int i;
	bool result = false;

	BUG_ON(!state);
	// 파티션 검색 상태 검증

	data = read_part_sector(state, 0, &sect);
	// LBA 0 MBR 섹터에 대한 NVMe 512B Read: read_part_sector -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)(추정)
	if (!data) {
	// NVMe MBR Read 실패 시 0 반환하여 다른 파서에 양보
		ldm_info ("Disk read failed.");
		return false;
	}

	if (*(__le16*) (data + 0x01FE) != cpu_to_le16 (MSDOS_LABEL_MAGIC))
	// offset 0x01FE: MBR 0xAA55 시그니처; 없으면 NVMe media에 MBR 없음
		goto out;
/* 0xAA55 MBR 시그니처가 없으면 Dynamic Disk일 가능성이 없다. */

	p = (struct msdos_partition *)(data + 0x01BE);
	// offset 0x01BE: 4개 기본 파티션 엔트리 시작; 각 항목의 start_lba는 NVMe LBA로 매핑(추정)
	for (i = 0; i < 4; i++, p++)
	// 4개 기본 파티션 엔트리 순회; CHS 필드는 NVMe LBA 경로와 무관
		if (p->sys_ind == LDM_PARTITION) {
		// 파티션 타입 0x42(LDM_PARTITION) 발견 시 Dynamic Disk로 판단, 추가 NVMe 메타데이터 Read 유발
			result = true;
			break;
		}
/* 4개의 기본 파티션 엔트리 중 하나라도 sys_ind == 0x42(LDM_PARTITION)이면 Dynamic Disk로 판단한다. */

	if (result)
		ldm_debug ("Found W2K dynamic disk partition type.");

out:
	put_dev_sector (sect);
	return result;
	// LDM은 MBR 확장 파티션 체인을 사용하지 않으므로 추가 NVMe Read 재귀는 없음; 대신 VBLK 영역 순회로 NVMe Read 명령량이 결정됨(추정)
}

/*
 * ldm_get_disk_objid()
 *   목적: LDM Disk Group 내에서 현재 NVMe namespace의 PRIVHEAD.disk_id와 일치하는
 *         Disk VBLK를 찾아 현재 물리 디스크 객체를 반환한다.
 *   호출 경로: ldm_create_data_partitions -> ldm_get_disk_objid
 *   NVMe 연결: NVMe와 직접 I/O하지 않으며, 이미 메모리에 캐시된 VBLK 리스트에서
 *              UUID 일치 여부로 디스크를 식별한다.
 */
/**
 * ldm_get_disk_objid - Search a linked list of vblk's for a given Disk Id
 * @ldb:  Cache of the database structures
 *
 * The LDM Database contains a list of all partitions on all dynamic disks.
 * The primary PRIVHEAD, at the beginning of the physical disk, tells us
 * the GUID of this disk.  This function searches for the GUID in a linked
 * list of vblk's.
 *
 * Return:  Pointer, A matching vblk was found
 *          NULL,    No match, or an error
 */
static struct vblk * ldm_get_disk_objid (const struct ldmdb *ldb)
{
	struct list_head *item;

	BUG_ON (!ldb);

	list_for_each (item, &ldb->v_disk) {
	// VBLK Disk 리스트 순회; PRIVHEAD.disk_id와 일치하는 항목으로 현재 NVMe namespace 식별
		struct vblk *v = list_entry (item, struct vblk, list);
		if (uuid_equal(&v->vblk.disk.disk_id, &ldb->ph.disk_id))
			return v;
/* PRIVHEAD의 disk_id와 일치하는 Disk VBLK를 찾으면 현재 NVMe 장치를 식별할 수 있다. */
	}

	return NULL;
}

/*
 * ldm_create_data_partitions()
 *   목적: LDM 데이터베이스에서 현재 NVMe namespace에 해당하는 disk_id를 가진
 *         파티션들만 골라 gendisk에 등록한다.
 *   호출 경로: ldm_partition -> ldm_create_data_partitions
 *   NVMe 연결: 등록된 각 파티션의 start/size는 NVMe namespace LBA를 기준으로 하며,
 *              이후 blk_mq가 해당 파티션의 bio를 처리할 때
 *              start 오프셋이 NVMe Read/Write 명령의 SLBA로 더해진다(추정).
 *   주요 동작: ldm_get_disk_objid()로 디스크 객체 검색,
 *             v_part 리스트를 순회하며 part->disk_id 일치 여부 확인,
 *             put_partition()으로 파티션 테이블에 추가.
 */
/**
 * ldm_create_data_partitions - Create data partitions for this device
 * @pp:   List of the partitions parsed so far
 * @ldb:  Cache of the database structures
 *
 * The database contains ALL the partitions for ALL disk groups, so we need to
 * filter out this specific disk. Using the disk's object id, we can find all
 * the partitions in the database that belong to this disk.
 *
 * Add each partition in our database, to the parsed_partitions structure.
 *
 * N.B.  This function creates the partitions in the order it finds partition
 *       objects in the linked list.
 *
 * Return:  'true'   Partition created
 *          'false'  Error, probably a range checking problem
 */
static bool ldm_create_data_partitions (struct parsed_partitions *pp,
					const struct ldmdb *ldb)
{
	struct list_head *item;
	struct vblk *vb;
	struct vblk *disk;
	struct vblk_part *part;
	int part_num = 1;
	// gendisk에 등록할 파티션 번호 시작; NVMe namespace 위에 블록 디바이스 생성

	BUG_ON (!pp || !ldb);

	disk = ldm_get_disk_objid (ldb);
	// 현재 NVMe namespace에 해당하는 Disk VBLK 검색
	if (!disk) {
	// 일치하는 disk_id가 없으면 이 NVMe 장치에 대한 파티션 등록 불가
		ldm_crit ("Can't find the ID of this disk in the database.");
		return false;
	}

	seq_buf_puts(&pp->pp_buf, " [LDM]");
	// LDM 파서가 인식됨을 dmesg 파티션 버퍼에 기록

	/* Create the data partitions */
	list_for_each (item, &ldb->v_part) {
	// LDM 파티션 VBLK 리스트 순회; 매칭 항목마다 NVMe namespace 위 block device 생성
		vb = list_entry (item, struct vblk, list);
		part = &vb->vblk.part;

		if (part->disk_id != disk->obj_id)
		// 다른 NVMe namespace/디스크의 파티션은 스킵
			continue;

		/* logical_disk_start + part->start를 시작 LBA로, part->size를 크기로 gendisk에 파티션을 등록한다. */
		put_partition (pp, part_num, ldb->ph.logical_disk_start +
				part->start, part->size);
				// gendisk 파티션 등록 -> user-space는 /dev/nvme*n*p* 형태로 접근(추정)
		part_num++;
		// 다음 파티션 번호 증가
	}

	seq_buf_puts(&pp->pp_buf, "\n");
	// 파티션 정보 출력 종료
	return true;
}


/**
 * ldm_relative - Calculate the next relative offset
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @base:    Size of the previous fixed width fields
 * @offset:  Cumulative size of the previous variable-width fields
 *
 * Because many of the VBLK fields are variable-width, it's necessary
 * to calculate each offset based on the previous one and the length
 * of the field it pointed to.
 *
 * Return:  -1 Error, the calculated offset exceeded the size of the buffer
 *           n OK, a range-checked offset into buffer
 */
static int ldm_relative(const u8 *buffer, int buflen, int base, int offset)
{

	base += offset;
	// VBLK 가변 필드의 다음 오프셋; NVMe DMA 버퍼 내 바이트 인덱스
/* 이전 고정 필드 너비(base)와 가변 필드 누적 크기(offset)를 더해 다음 필드 위치를 계산한다. */
	if (!buffer || offset < 0 || base > buflen) {
	// 오프셋이 버퍼를 벗어나면 NVMe로부터 읽은 메타데이터 손상
		if (!buffer)
			ldm_error("!buffer");
		if (offset < 0)
			ldm_error("offset (%d) < 0", offset);
		if (base > buflen)
			ldm_error("base (%d) > buflen (%d)", base, buflen);
		return -1;
	}
	if (base + buffer[base] >= buflen) {
	// 다음 가변 필드 길이 포함 시 버퍼 범위 초과
		ldm_error("base (%d) + buffer[base] (%d) >= buflen (%d)", base,
				buffer[base], buflen);
		return -1;
	}
/* 다음 가변 필드가 버퍼 범위를 벗어나면 파싱을 중단한다. */
	return buffer[base] + offset + 1;
}

/**
 * ldm_get_vnum - Convert a variable-width, big endian number, into cpu order
 * @block:  Pointer to the variable-width number to convert
 *
 * Large numbers in the LDM Database are often stored in a packed format.  Each
 * number is prefixed by a one byte width marker.  All numbers in the database
 * are stored in big-endian byte order.  This function reads one of these
 * numbers and returns the result
 *
 * N.B.  This function DOES NOT perform any range checking, though the most
 *       it will read is eight bytes.
 *
 * Return:  n A number
 *          0 Zero, or an error occurred
 */
static u64 ldm_get_vnum (const u8 *block)
{
	u64 tmp = 0;
	u8 length;

	BUG_ON (!block);

	length = *block++;
	// VNUM 길이 바이트; NVMe DMA 버퍼에서 첫 바이트 읽기
/* VNUM의 첫 바이트가 길이(width)를 나타낸며, 이후 length바이트를 big-endian으로 읽는다. */

	if (length && length <= 8)
		while (length--)
		// big-endian 바이트를 조립하여 VBLK 내 LBA/크기 값 복원
			tmp = (tmp << 8) | *block++;
	else
		ldm_error ("Illegal length %d.", length);

	return tmp;
}

/**
 * ldm_get_vstr - Read a length-prefixed string into a buffer
 * @block:   Pointer to the length marker
 * @buffer:  Location to copy string to
 * @buflen:  Size of the output buffer
 *
 * Many of the strings in the LDM Database are not NULL terminated.  Instead
 * they are prefixed by a one byte length marker.  This function copies one of
 * these strings into a buffer.
 *
 * N.B.  This function DOES NOT perform any range checking on the input.
 *       If the buffer is too small, the output will be truncated.
 *
 * Return:  0, Error and @buffer contents are undefined
 *          n, String length in characters (excluding NULL)
 *          buflen-1, String was truncated.
 */
static int ldm_get_vstr (const u8 *block, u8 *buffer, int buflen)
{
	int length;

	BUG_ON (!block || !buffer);

	length = block[0];
	if (length >= buflen) {
		ldm_error ("Truncating string %d -> %d.", length, buflen);
		length = buflen - 1;
	}
	memcpy (buffer, block + 1, length);
	buffer[length] = 0;
	return length;
}


/**
 * ldm_parse_cmp3 - Read a raw VBLK Component object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Component object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Component VBLK
 *          'false'  @vb contents are not defined
 */
static bool ldm_parse_cmp3 (const u8 *buffer, int buflen, struct vblk *vb)
{
	int r_objid, r_name, r_vstate, r_child, r_parent, r_stripe, r_cols, len;
	struct vblk_comp *comp;

	BUG_ON (!buffer || !vb);

	r_objid  = ldm_relative (buffer, buflen, 0x18, 0);
	r_name   = ldm_relative (buffer, buflen, 0x18, r_objid);
	r_vstate = ldm_relative (buffer, buflen, 0x18, r_name);
	r_child  = ldm_relative (buffer, buflen, 0x1D, r_vstate);
	r_parent = ldm_relative (buffer, buflen, 0x2D, r_child);
	// Component VBLK 가변 필드 오프셋; NVMe 512B 버퍼 파싱

	if (buffer[0x12] & VBLK_FLAG_COMP_STRIPE) {
	// stripe 플래그가 설정되면 RAID 청크 크기가 NVMe I/O 정렬에 영향(추정)
		r_stripe = ldm_relative (buffer, buflen, 0x2E, r_parent);
		r_cols   = ldm_relative (buffer, buflen, 0x2E, r_stripe);
		len = r_cols;
	} else {
		r_stripe = 0;
		len = r_parent;
	}
	if (len < 0)
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		return false;

	len += VBLK_SIZE_CMP3;
	if (len != get_unaligned_be32(buffer + 0x14))
		return false;

	comp = &vb->vblk.comp;
	ldm_get_vstr (buffer + 0x18 + r_name, comp->state,
		sizeof (comp->state));
	comp->type      = buffer[0x18 + r_vstate];
	comp->children  = ldm_get_vnum (buffer + 0x1D + r_vstate);
	comp->parent_id = ldm_get_vnum (buffer + 0x2D + r_child);
	comp->chunksize = r_stripe ? ldm_get_vnum (buffer+r_parent+0x2E) : 0;
	// comp->chunksize는 연속된 NVMe LBA 단위 stripe 크기(추정)

	return true;
}

/**
 * ldm_parse_dgr3 - Read a raw VBLK Disk Group object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk Group object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk Group VBLK
 *          'false'  @vb contents are not defined
 */
static int ldm_parse_dgr3 (const u8 *buffer, int buflen, struct vblk *vb)
{
	int r_objid, r_name, r_diskid, r_id1, r_id2, len;
	struct vblk_dgrp *dgrp;

	BUG_ON (!buffer || !vb);

	r_objid  = ldm_relative (buffer, buflen, 0x18, 0);
	r_name   = ldm_relative (buffer, buflen, 0x18, r_objid);
	r_diskid = ldm_relative (buffer, buflen, 0x18, r_name);

	if (buffer[0x12] & VBLK_FLAG_DGR3_IDS) {
	// VBLK 플래그에 따라 선택적 필드 존재; NVMe DMA 버퍼 내 오프셋 재계산
		r_id1 = ldm_relative (buffer, buflen, 0x24, r_diskid);
		r_id2 = ldm_relative (buffer, buflen, 0x24, r_id1);
		len = r_id2;
	} else
		len = r_diskid;
	if (len < 0)
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		return false;

	len += VBLK_SIZE_DGR3;
	if (len != get_unaligned_be32(buffer + 0x14))
		return false;

	dgrp = &vb->vblk.dgrp;
	ldm_get_vstr (buffer + 0x18 + r_name, dgrp->disk_id,
		sizeof (dgrp->disk_id));
	return true;
}

/**
 * ldm_parse_dgr4 - Read a raw VBLK Disk Group object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk Group object (version 4) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk Group VBLK
 *          'false'  @vb contents are not defined
 */
static bool ldm_parse_dgr4 (const u8 *buffer, int buflen, struct vblk *vb)
{
	char buf[64];
	int r_objid, r_name, r_id1, r_id2, len;

	BUG_ON (!buffer || !vb);

	r_objid  = ldm_relative (buffer, buflen, 0x18, 0);
	r_name   = ldm_relative (buffer, buflen, 0x18, r_objid);

	if (buffer[0x12] & VBLK_FLAG_DGR4_IDS) {
	// VBLK 플래그에 따라 선택적 필드 존재; NVMe DMA 버퍼 내 오프셋 재계산
		r_id1 = ldm_relative (buffer, buflen, 0x44, r_name);
		r_id2 = ldm_relative (buffer, buflen, 0x44, r_id1);
		len = r_id2;
	} else
		len = r_name;
	if (len < 0)
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		return false;

	len += VBLK_SIZE_DGR4;
	if (len != get_unaligned_be32(buffer + 0x14))
		return false;

	ldm_get_vstr (buffer + 0x18 + r_objid, buf, sizeof (buf));
	return true;
}

/**
 * ldm_parse_dsk3 - Read a raw VBLK Disk object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk VBLK
 *          'false'  @vb contents are not defined
 */
static bool ldm_parse_dsk3 (const u8 *buffer, int buflen, struct vblk *vb)
{
	int r_objid, r_name, r_diskid, r_altname, len;
	struct vblk_disk *disk;

	BUG_ON (!buffer || !vb);

	r_objid   = ldm_relative (buffer, buflen, 0x18, 0);
	r_name    = ldm_relative (buffer, buflen, 0x18, r_objid);
	r_diskid  = ldm_relative (buffer, buflen, 0x18, r_name);
	r_altname = ldm_relative (buffer, buflen, 0x18, r_diskid);
	// Disk VBLK 가변 필드; NVMe 메타데이터에서 디스크 UUID 위치 계산
	len = r_altname;
	if (len < 0)
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		return false;

	len += VBLK_SIZE_DSK3;
	if (len != get_unaligned_be32(buffer + 0x14))
		return false;

	disk = &vb->vblk.disk;
	ldm_get_vstr (buffer + 0x18 + r_diskid, disk->alt_name,
		sizeof (disk->alt_name));
	if (uuid_parse(buffer + 0x19 + r_name, &disk->disk_id))
	// Disk UUID 파싱; NVMe namespace UUID와 매칭 대상
		return false;

	return true;
}

/**
 * ldm_parse_dsk4 - Read a raw VBLK Disk object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk object (version 4) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk VBLK
 *          'false'  @vb contents are not defined
 */
static bool ldm_parse_dsk4 (const u8 *buffer, int buflen, struct vblk *vb)
{
	int r_objid, r_name, len;
	struct vblk_disk *disk;

	BUG_ON (!buffer || !vb);

	r_objid = ldm_relative (buffer, buflen, 0x18, 0);
	r_name  = ldm_relative (buffer, buflen, 0x18, r_objid);
	// Disk VBLK v4 가변 필드
	len     = r_name;
	if (len < 0)
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		return false;

	len += VBLK_SIZE_DSK4;
	if (len != get_unaligned_be32(buffer + 0x14))
		return false;

	disk = &vb->vblk.disk;
	import_uuid(&disk->disk_id, buffer + 0x18 + r_name);
	// v4 디스크 UUID 가져오기
	return true;
}

/**
 * ldm_parse_prt3 - Read a raw VBLK Partition object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Partition object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Partition VBLK
 *          'false'  @vb contents are not defined
 */
static bool ldm_parse_prt3(const u8 *buffer, int buflen, struct vblk *vb)
{
	int r_objid, r_name, r_size, r_parent, r_diskid, r_index, len;
	struct vblk_part *part;

	BUG_ON(!buffer || !vb);
	r_objid = ldm_relative(buffer, buflen, 0x18, 0);
	if (r_objid < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_objid %d < 0", r_objid);
		return false;
	}
	r_name = ldm_relative(buffer, buflen, 0x18, r_objid);
	if (r_name < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_name %d < 0", r_name);
		return false;
	}
	r_size = ldm_relative(buffer, buflen, 0x34, r_name);
	if (r_size < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_size %d < 0", r_size);
		return false;
	}
	r_parent = ldm_relative(buffer, buflen, 0x34, r_size);
	if (r_parent < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_parent %d < 0", r_parent);
		return false;
	}
	r_diskid = ldm_relative(buffer, buflen, 0x34, r_parent);
	if (r_diskid < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_diskid %d < 0", r_diskid);
		return false;
	}
	// Partition VBLK 가변 필드 오프셋; start/size가 NVMe LBA/섹터 수로 해석
	if (buffer[0x12] & VBLK_FLAG_PART_INDEX) {
	// partnum 인덱스 플래그
		r_index = ldm_relative(buffer, buflen, 0x34, r_diskid);
		if (r_index < 0) {
		// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
			ldm_error("r_index %d < 0", r_index);
			return false;
		}
		len = r_index;
	} else
		len = r_diskid;
	if (len < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("len %d < 0", len);
		return false;
	}
	len += VBLK_SIZE_PRT3;
	if (len > get_unaligned_be32(buffer + 0x14)) {
		ldm_error("len %d > BE32(buffer + 0x14) %d", len,
				get_unaligned_be32(buffer + 0x14));
		return false;
	}
	part = &vb->vblk.part;
	part->start = get_unaligned_be64(buffer + 0x24 + r_name);
	// offset 0x24+r_name: NVMe namespace 내 파티션 시작 LBA(섹터)
	part->volume_offset = get_unaligned_be64(buffer + 0x2C + r_name);
	// offset 0x2C+r_name: 볼륨 내 오프셋(섹터)
	part->size = ldm_get_vnum(buffer + 0x34 + r_name);
	// offset 0x34+r_name: 파티션 크기(섹터); NVMe Read/Write 범위
	part->parent_id = ldm_get_vnum(buffer + 0x34 + r_size);
	// 상위 Component 객체 ID
	part->disk_id = ldm_get_vnum(buffer + 0x34 + r_parent);
	// 소속 Disk 객체 ID; 현재 NVMe namespace와 매칭
	if (vb->flags & VBLK_FLAG_PART_INDEX)
		part->partnum = buffer[0x35 + r_diskid];
		// 파티션 번호; gendisk 등록 시 사용되지 않고 순차 번호 사용
	else
		part->partnum = 0;
	return true;
}

/**
 * ldm_parse_vol5 - Read a raw VBLK Volume object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Volume object (version 5) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Volume VBLK
 *          'false'  @vb contents are not defined
 */
static bool ldm_parse_vol5(const u8 *buffer, int buflen, struct vblk *vb)
{
	int r_objid, r_name, r_vtype, r_disable_drive_letter, r_child, r_size;
	int r_id1, r_id2, r_size2, r_drive, len;
	struct vblk_volu *volu;

	BUG_ON(!buffer || !vb);
	r_objid = ldm_relative(buffer, buflen, 0x18, 0);
	if (r_objid < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_objid %d < 0", r_objid);
		return false;
	}
	r_name = ldm_relative(buffer, buflen, 0x18, r_objid);
	if (r_name < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_name %d < 0", r_name);
		return false;
	}
	r_vtype = ldm_relative(buffer, buflen, 0x18, r_name);
	if (r_vtype < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_vtype %d < 0", r_vtype);
		return false;
	}
	r_disable_drive_letter = ldm_relative(buffer, buflen, 0x18, r_vtype);
	if (r_disable_drive_letter < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_disable_drive_letter %d < 0",
				r_disable_drive_letter);
		return false;
	}
	r_child = ldm_relative(buffer, buflen, 0x2D, r_disable_drive_letter);
	if (r_child < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_child %d < 0", r_child);
		return false;
	}
	r_size = ldm_relative(buffer, buflen, 0x3D, r_child);
	if (r_size < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("r_size %d < 0", r_size);
		return false;
	}
	if (buffer[0x12] & VBLK_FLAG_VOLU_ID1) {
	// VBLK 플래그에 따라 선택적 필드 존재; NVMe DMA 버퍼 내 오프셋 재계산
		r_id1 = ldm_relative(buffer, buflen, 0x52, r_size);
		if (r_id1 < 0) {
		// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
			ldm_error("r_id1 %d < 0", r_id1);
			return false;
		}
	} else
		r_id1 = r_size;
	if (buffer[0x12] & VBLK_FLAG_VOLU_ID2) {
	// VBLK 플래그에 따라 선택적 필드 존재; NVMe DMA 버퍼 내 오프셋 재계산
		r_id2 = ldm_relative(buffer, buflen, 0x52, r_id1);
		if (r_id2 < 0) {
		// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
			ldm_error("r_id2 %d < 0", r_id2);
			return false;
		}
	} else
		r_id2 = r_id1;
	if (buffer[0x12] & VBLK_FLAG_VOLU_SIZE) {
	// VBLK 플래그에 따라 선택적 필드 존재; NVMe DMA 버퍼 내 오프셋 재계산
		r_size2 = ldm_relative(buffer, buflen, 0x52, r_id2);
		if (r_size2 < 0) {
		// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
			ldm_error("r_size2 %d < 0", r_size2);
			return false;
		}
	} else
		r_size2 = r_id2;
	if (buffer[0x12] & VBLK_FLAG_VOLU_DRIVE) {
	// VBLK 플래그에 따라 선택적 필드 존재; NVMe DMA 버퍼 내 오프셋 재계산
		r_drive = ldm_relative(buffer, buflen, 0x52, r_size2);
		if (r_drive < 0) {
		// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
			ldm_error("r_drive %d < 0", r_drive);
			return false;
		}
	} else
		r_drive = r_size2;
		// Volume VBLK 가변 필드; 볼륨 크기는 NVMe LBA 범위 계산에 사용
	len = r_drive;
	if (len < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error("len %d < 0", len);
		return false;
	}
	len += VBLK_SIZE_VOL5;
	if (len > get_unaligned_be32(buffer + 0x14)) {
		ldm_error("len %d > BE32(buffer + 0x14) %d", len,
				get_unaligned_be32(buffer + 0x14));
		return false;
	}
	volu = &vb->vblk.volu;
	ldm_get_vstr(buffer + 0x18 + r_name, volu->volume_type,
			sizeof(volu->volume_type));
	memcpy(volu->volume_state, buffer + 0x18 + r_disable_drive_letter,
			sizeof(volu->volume_state));
	volu->size = ldm_get_vnum(buffer + 0x3D + r_child);
	// offset 0x3D+r_child: 볼륨 크기(섹터); NVMe namespace LBA 범위
	volu->partition_type = buffer[0x41 + r_size];
	memcpy(volu->guid, buffer + 0x42 + r_size, sizeof(volu->guid));
	if (buffer[0x12] & VBLK_FLAG_VOLU_DRIVE) {
	// VBLK 플래그에 따라 선택적 필드 존재; NVMe DMA 버퍼 내 오프셋 재계산
		ldm_get_vstr(buffer + 0x52 + r_size, volu->drive_hint,
				sizeof(volu->drive_hint));
	}
	return true;
}

/*
 * ldm_parse_vblk()
 *   목적: VBLK 공통 헤더를 읽고, 객체 타입(VBLK_*)에 따라 전용 파서로 위임한다.
 *   호출 경로: ldm_get_vblks -> ldm_ldmdb_add -> ldm_parse_vblk
 *   NVMe 연결: NVMe로부터 받은 512바이트 메타데이터 버퍼 안의 VBLK 레코드를 해석한다.
 *              vblk_size가 NVMe 섹터 크기(512B)로 나누어 떨어지지 않을 수 있으므로
 *              (추정) 버퍼 내 바이트 오프셋 계산이 중요하다.
 *   주요 동작: flags/type/obj_id/name 파싱, switch 문으로 하위 파서 호출,
 *             Component/Disk/DiskGroup/Partition/Volume 처리.
 */
/**
 * ldm_parse_vblk - Read a raw VBLK object into a vblk structure
 * @buf:  Block of data being worked on
 * @len:  Size of the block of data
 * @vb:   In-memory vblk in which to return information
 *
 * Read a raw VBLK object into a vblk structure.  This function just reads the
 * information common to all VBLK types, then delegates the rest of the work to
 * helper functions: ldm_parse_*.
 *
 * Return:  'true'   @vb contains a VBLK
 *          'false'  @vb contents are not defined
 */
static bool ldm_parse_vblk (const u8 *buf, int len, struct vblk *vb)
{
	bool result = false;
	int r_objid;

	BUG_ON (!buf || !vb);

	r_objid = ldm_relative (buf, len, 0x18, 0);
	// VBLK 공통 헤더에서 obj_id 필드 오프셋 계산
	if (r_objid < 0) {
	// 가변 필드 오프셋/길이가 음수이면 NVMe로부터 읽은 VBLK 메타데이터가 손상됨
		ldm_error ("VBLK header is corrupt.");
		return false;
	}

	vb->flags  = buf[0x12];
	// offset 0x12: VBLK 플래그; 파싱 경로 분기
	vb->type   = buf[0x13];
	// offset 0x13: VBLK 타입; Partition 타입이 NVMe LBA 정보 포함
	vb->obj_id = ldm_get_vnum (buf + 0x18);
	// offset 0x18: 객체 ID(VNUM)
	ldm_get_vstr (buf+0x18+r_objid, vb->name, sizeof (vb->name));
	// obj_id 뒤에 위치한 이름 문자열
/* VBLK 공통 헤더에서 flags, type, obj_id, name을 추출한 뒤 타입별 파서로 넘긴다. */

	switch (vb->type) {
	// VBLK 타입별 파서 분기
/* Component/Disk/DiskGroup/Partition/Volume 중 해당하는 파서를 선택한다. */
		case VBLK_CMP3:  result = ldm_parse_cmp3 (buf, len, vb); break;
		case VBLK_DSK3:  result = ldm_parse_dsk3 (buf, len, vb); break;
		case VBLK_DSK4:  result = ldm_parse_dsk4 (buf, len, vb); break;
		case VBLK_DGR3:  result = ldm_parse_dgr3 (buf, len, vb); break;
		case VBLK_DGR4:  result = ldm_parse_dgr4 (buf, len, vb); break;
		case VBLK_PRT3:  result = ldm_parse_prt3 (buf, len, vb); break;
		case VBLK_VOL5:  result = ldm_parse_vol5 (buf, len, vb); break;
	}

	if (result)
		ldm_debug ("Parsed VBLK 0x%llx (type: 0x%02x) ok.",
			 (unsigned long long) vb->obj_id, vb->type);
	else
		ldm_error ("Failed to parse VBLK 0x%llx (type: 0x%02x).",
			(unsigned long long) vb->obj_id, vb->type);

	return result;
}


/*
 * ldm_ldmdb_add()
 *   목적: 파싱된 VBLK를 ldmdb의 타입별 리스트에 분류하여 저장한다.
 *   호출 경로: ldm_get_vblks -> ldm_ldmdb_add
 *              (또는 ldm_frag_commit -> ldm_ldmdb_add)
 *   NVMe 연결: NVMe 메타데이터에서 추출한 파티션/볼륨/디스크 정보를
 *              메모리 캐시에 담아두는 단계로, NVMe I/O 프로토콜과는 직접 관련없으나
 *              이 정보가 이후 NVMe 데이터 경로의 gendisk 파티션 테이블로 반영된다.
 *   주요 동작: VBLK 타입별 리스트 추가, VBLK_PRT3인 경우 start 섹터 기준 정렬 삽입.
 */
/**
 * ldm_ldmdb_add - Adds a raw VBLK entry to the ldmdb database
 * @data:  Raw VBLK to add to the database
 * @len:   Size of the raw VBLK
 * @ldb:   Cache of the database structures
 *
 * The VBLKs are sorted into categories.  Partitions are also sorted by offset.
 *
 * N.B.  This function does not check the validity of the VBLKs.
 *
 * Return:  'true'   The VBLK was added
 *          'false'  An error occurred
 */
static bool ldm_ldmdb_add (u8 *data, int len, struct ldmdb *ldb)
{
	struct vblk *vb;
	struct list_head *item;

	BUG_ON (!data || !ldb);

	vb = kmalloc_obj(*vb);
	// VBLK 객체 메모리; 할당 실패 시 ldmdb 구축 중단
	if (!vb) {
		ldm_crit ("Out of memory.");
		return false;
	}

	if (!ldm_parse_vblk (data, len, vb)) {
	// NVMe DMA 버퍼에서 VBLK 파싱
		kfree(vb);
		// 파싱 실패한 VBLK 메모리 해제; NVMe namespace에는 등록되지 않음
		return false;			/* Already logged */
	}

	/* Put vblk into the correct list. */
	switch (vb->type) {
	// 파싱된 VBLK를 타입별 리스트로 분류
	case VBLK_DGR3:
	case VBLK_DGR4:
		list_add (&vb->list, &ldb->v_dgrp);
		// Disk Group VBLK를 캐시; NVMe I/O와는 무관한 LDM 그룹 메타데이터
		break;
	case VBLK_DSK3:
	case VBLK_DSK4:
		list_add (&vb->list, &ldb->v_disk);
		// Disk VBLK를 캐시; PRIVHEAD.disk_id와 매칭하여 현재 NVMe namespace 식별
		break;
	case VBLK_VOL5:
		list_add (&vb->list, &ldb->v_volu);
		// Volume VBLK를 캐시; 볼륨 크기는 NVMe LBA 범위 해석에 사용
		break;
	case VBLK_CMP3:
		list_add (&vb->list, &ldb->v_comp);
		// Component VBLK를 캐시; RAID stripe/chunksize가 NVMe I/O 정렬에 영향(추정)
		break;
	case VBLK_PRT3:
	// 파티션 VBLK는 start LBA 기준 정렬; 이후 NVMe namespace 파티션 등록 순서에 영향(추정)
		/* Sort by the partition's start sector. */
		list_for_each (item, &ldb->v_part) {
			struct vblk *v = list_entry (item, struct vblk, list);
			if ((v->vblk.part.disk_id == vb->vblk.part.disk_id) &&
			// 같은 NVMe disk 내에서 start LBA 오름차순 삽입
			    (v->vblk.part.start > vb->vblk.part.start)) {
				list_add_tail (&vb->list, &v->list);
				return true;
			}
		}
		list_add_tail (&vb->list, &ldb->v_part);
		// 파티션 VBLK를 start LBA 순으로 캐시; 최종 NVMe namespace 파티션 등록 순서(추정)
/* 파티션 VBLK는 start LBA 기준으로 정렬되어 삽입된다(추정: 이후 순차적 등록을 위함). */
		break;
	}
	return true;
}

/*
 * ldm_frag_add()
 *   목적: 하나의 VBLK가 여러 섹터에 걸쳐 단편화되어 있을 때 조각을 수집한다.
 *   호출 경로: ldm_get_vblks -> ldm_frag_add
 *   NVMe 연결: NVMe Read로 얻은 각 512바이트 섹터에서 VBLK_HEAD(group/rec/num)를
 *              보고 같은 group에 속하는 조각들을 하나의 버퍼에 모은다.
 *   주요 동작: group별 frag 할당, 중복 조각 검사, map 비트로 완성도 추적.
 */
/**
 * ldm_frag_add - Add a VBLK fragment to a list
 * @data:   Raw fragment to be added to the list
 * @size:   Size of the raw fragment
 * @frags:  Linked list of VBLK fragments
 *
 * Fragmented VBLKs may not be consecutive in the database, so they are placed
 * in a list so they can be pieced together later.
 *
 * Return:  'true'   Success, the VBLK was added to the list
 *          'false'  Error, a problem occurred
 */
static bool ldm_frag_add (const u8 *data, int size, struct list_head *frags)
{
	struct frag *f;
	struct list_head *item;
	int rec, num, group;

	BUG_ON (!data || !frags);

	if (size < 2 * VBLK_SIZE_HEAD) {
	// 조각 헤더 크기가 NVMe 512B 섹터보다 작아야 함
		ldm_error("Value of size is too small.");
		return false;
	}

	group = get_unaligned_be32(data + 0x08);
	// offset 0x08: VBLK 조각 그룹 ID; NVMe DMA 버퍼에서 추출
	rec   = get_unaligned_be16(data + 0x0C);
	// offset 0x0C: 현재 조각 번호
	num   = get_unaligned_be16(data + 0x0E);
	// offset 0x0E: 전체 조각 수; 1~4개(추정)
	if ((num < 1) || (num > 4)) {
	// 조각 수가 1~4 범위 밖이면 NVMe 메타데이터 손상
		ldm_error ("A VBLK claims to have %d parts.", num);
		return false;
	}
	if (rec >= num) {
	// 조각 번호가 전체 수 이상이면 잘못된 VBLK
		ldm_error("REC value (%d) exceeds NUM value (%d)", rec, num);
		return false;
	}

	list_for_each (item, frags) {
		f = list_entry (item, struct frag, list);
		if (f->group == group)
			goto found;
	}

	f = kmalloc (sizeof (*f) + size*num, GFP_KERNEL);
	// 단편화된 VBLK 재조립용 메모리; 할당 실패 시 중단
	if (!f) {
		ldm_crit ("Out of memory.");
		return false;
	}

	f->group = group;
	f->num   = num;
	f->rec   = rec;
	f->map   = 0xFF << num;
	// 완성도 비트맵 초기화; 하위 num비트가 0이면 미수신
/* map의 하위 num비트를 0으로 두고, 수신한 조각 인덱스를 1로 표시할 준비를 한다. */

	list_add_tail (&f->list, frags);
found:
	if (rec >= f->num) {
		ldm_error("REC value (%d) exceeds NUM value (%d)", rec, f->num);
		return false;
	}
	if (f->map & (1 << rec)) {
	// 중복 조각은 NVMe media 손상 또는 잘못된 CQE(추정)
		ldm_error ("Duplicate VBLK, part %d.", rec);
		f->map &= 0x7F;			/* Mark the group as broken */
		return false;
	}
/* 중복 조각이 수신되면 해당 그룹을 손상(0x7F)으로 표시하고 폐기한다. */
	f->map |= (1 << rec);
	// 수신한 조각 번호를 비트맵에 표시
	if (!rec)
		memcpy(f->data, data, VBLK_SIZE_HEAD);
	data += VBLK_SIZE_HEAD;
	size -= VBLK_SIZE_HEAD;
	memcpy(f->data + VBLK_SIZE_HEAD + rec * size, data, size);
	return true;
}

/**
 * ldm_frag_free - Free a linked list of VBLK fragments
 * @list:  Linked list of fragments
 *
 * Free a linked list of VBLK fragments
 *
 * Return:  none
 */
static void ldm_frag_free (struct list_head *list)
{
	struct list_head *item, *tmp;

	BUG_ON (!list);

	list_for_each_safe (item, tmp, list)
		kfree (list_entry (item, struct frag, list));
}

/*
 * ldm_frag_commit()
 *   목적: 단편화된 VBLK 조각들이 모두 모아졌는지 확인한 뒤 ldmdb에 추가한다.
 *   호출 경로: ldm_get_vblks -> ldm_frag_commit
 *   NVMe 연결: NVMe에서 읽은 여러 섹터에 흩어진 VBLK 조각들을 재조립하여
 *              하나의 완전한 VBLK 메타데이터를 만든다.
 *              재조립이 끝난 데이터는 다시 ldm_parse_vblk를 통해 해석된다.
 *   주요 동작: f->map == 0xFF 완성 여부 확인, 완성된 조각을 ldm_ldmdb_add()로 등록.
 */
/**
 * ldm_frag_commit - Validate fragmented VBLKs and add them to the database
 * @frags:  Linked list of VBLK fragments
 * @ldb:    Cache of the database structures
 *
 * Now that all the fragmented VBLKs have been collected, they must be added to
 * the database for later use.
 *
 * Return:  'true'   All the fragments we added successfully
 *          'false'  One or more of the fragments we invalid
 */
static bool ldm_frag_commit (struct list_head *frags, struct ldmdb *ldb)
{
	struct frag *f;
	struct list_head *item;

	BUG_ON (!frags || !ldb);

	list_for_each (item, frags) {
	// 수집된 VBLK 조각 리스트 순회
		f = list_entry (item, struct frag, list);

		if (f->map != 0xFF) {
		// 모든 조각이 수집되지 않으면 NVMe로부터 메타데이터 누락
			ldm_error ("VBLK group %d is incomplete (0x%02x).",
				f->group, f->map);
			return false;
		}
/* 모든 조각이 모이지 않으면(f->map != 0xFF) 불완전한 VBLK로 간주한다. */

		if (!ldm_ldmdb_add (f->data, f->num*ldb->vm.vblk_size, ldb))
		// 재조립된 VBLK를 ldmdb에 추가하여 파싱
			return false;		/* Already logged */
	}
	return true;
}

/*
 * ldm_get_vblks()
 *   목적: VMDB가 가리키는 VBLK 영역을 모두 읽어 메모리 내 ldmdb 리스트로 구성한다.
 *   호출 경로: ldm_partition -> ldm_get_vblks
 *   NVMe 연결: vblk_size, vblk_offset, last_vblk_seq를 바탕으로
 *              base + OFF_VMDB + s 의 NVMe LBA 범위를 순회하며
 *              read_part_sector()를 호출한다. 각 호출은 NVMe Read 명령으로
 *              매핑되며, 512바이트 단위의 PRP/SGL 버퍼로 완료된다(추정).
 *              단편화된 VBLK는 ldm_frag_add/ldm_frag_commit로 재조립한다.
 *   주요 동작: 섹터 단위 반복, 섹터당 perbuf 개 VBLK 처리, recs==1이면 즉시 추가,
 *             recs>1이면 frag 리스트에 저장, 마지막에 commit.
 */
/**
 * ldm_get_vblks - Read the on-disk database of VBLKs into memory
 * @state: Partition check state including device holding the LDM Database
 * @base:  Offset, into @state->disk, of the database
 * @ldb:   Cache of the database structures
 *
 * To use the information from the VBLKs, they need to be read from the disk,
 * unpacked and validated.  We cache them in @ldb according to their type.
 *
 * Return:  'true'   All the VBLKs were read successfully
 *          'false'  An error occurred
 */
static bool ldm_get_vblks(struct parsed_partitions *state, unsigned long base,
			  struct ldmdb *ldb)
{
	int size, perbuf, skip, finish, s, v, recs;
	u8 *data = NULL;
	Sector sect;
	bool result = false;
	LIST_HEAD (frags);

	BUG_ON(!state || !ldb);

	size   = ldb->vm.vblk_size;
	// VBLK 크기(바이트)
	perbuf = 512 / size;
	// 512B NVMe 섹터당 포함 가능한 VBLK 개수
	skip   = ldb->vm.vblk_offset >> 9;		/* Bytes to sectors */
	// 바이트 오프셋을 NVMe LBA 오프셋(섹터)로 변환
	finish = (size * ldb->vm.last_vblk_seq) >> 9;
	// 읽어야 할 총 NVMe 섹터 수
/* vblk_size로 512바이트 섹터당 VBLK 개수(perbuf)와 읽을 섹터 범위를 계산한다. */

	for (s = skip; s < finish; s++) {		/* For each sector */
	// skip부터 finish까지 NVMe LBA 순회; 반복당 1개 NVMe Read 명령(추정)
/* OFF_VMDB 기준으로 skip부터 finish까지 NVMe LBA를 순회한다. */
		data = read_part_sector(state, base + OFF_VMDB + s, &sect);
		// base+OFF_VMDB+s LBA에서 NVMe 512B Read: read_part_sector -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)(추정)
		if (!data) {
		// NVMe CQE 실패 시 VBLK 수집 중단
			ldm_crit ("Disk read failed.");
			goto out;
		}

		for (v = 0; v < perbuf; v++, data+=size) {  /* For each vblk */
		// 하나의 NVMe 섹터 버퍼 내에서 perbuf 개 VBLK 처리
/* 하나의 512바이트 NVMe 섹터 안에서 perbuf 개의 VBLK를 바이트 단위로 건너뛰며 처리한다. */
			if (MAGIC_VBLK != get_unaligned_be32(data)) {
			// VBLK 시그니처 불일치; NVMe media 손상
				ldm_error ("Expected to find a VBLK.");
				goto out;
			}

			recs = get_unaligned_be16(data + 0x0E);	/* Number of records */
			// offset 0x0E: VBLK 조각 수
/* VBLK가 몇 개의 조각으로 나뉘어 있는지 확인한다. */
			if (recs == 1) {
			// 단일 조각 VBLK는 즉시 ldmdb 추가
				if (!ldm_ldmdb_add (data, size, ldb))
					goto out;	/* Already logged */
			} else if (recs > 1) {
			// 다중 조각 VBLK는 후속 NVMe 섹터에서 나머지 조각 수집
				if (!ldm_frag_add (data, size, &frags))
					goto out;	/* Already logged */
			}
			/* else Record is not in use, ignore it. */
/* recs==0이면 사용되지 않는 레코드이므로 무시한다. */
		}
		put_dev_sector (sect);
		// 현재 NVMe 섹터 버퍼 해제
		data = NULL;
	}

	result = ldm_frag_commit (&frags, ldb);	/* Failures, already logged */
/* 단편화된 VBLK 조각들을 재조립하여 ldmdb 리스트에 반영한다. */
out:
	if (data)
		put_dev_sector (sect);
	ldm_frag_free (&frags);

	return result;
}

/**
 * ldm_free_vblks - Free a linked list of vblk's
 * @lh:  Head of a linked list of struct vblk
 *
 * Free a list of vblk's and free the memory used to maintain the list.
 *
 * Return:  none
 */
static void ldm_free_vblks (struct list_head *lh)
{
	struct list_head *item, *tmp;

	BUG_ON (!lh);

	list_for_each_safe (item, tmp, lh)
		kfree (list_entry (item, struct vblk, list));
}


/*
 * ldm_partition()
 *   목적: NVMe namespace 블록 장치가 Windows LDM Dynamic Disk인지 판별하고,
 *         맞다면 LDM 메타데이터를 해석하여 파티션을 생성한다.
 *   호출 경로: check_partition (block/partitions/check.c) -> ldm_partition (추정)
 *   NVMe 연결: 이 함수는 NVMe 드라이버가 namespace를 등록할 때
 *              디스커버리 단계에서 호출된다(추정).
 *              내부의 read_part_sector() 호출은
 *              submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *              nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를 거쳐
 *              NVMe SQ/CQ를 통해 LBA 단위 메타데이터를 읽어온다.
 *   주요 흐름: MBR 0x42 파티션 타입 확인 -> PRIVHEAD 검증 ->
 *             TOCBLOCK/VMDB 검증 -> VBLK 읽기 -> 데이터 파티션 등록.
 */
/**
 * ldm_partition - Find out whether a device is a dynamic disk and handle it
 * @state: Partition check state including device holding the LDM Database
 *
 * This determines whether the device @bdev is a dynamic disk and if so creates
 * the partitions necessary in the gendisk structure pointed to by @hd.
 *
 * We create a dummy device 1, which contains the LDM database, and then create
 * each partition described by the LDM database in sequence as devices 2+. For
 * example, if the device is hda, we would have: hda1: LDM database, hda2, hda3,
 * and so on: the actual data containing partitions.
 *
 * Return:  1 Success, @state->disk is a dynamic disk and we handled it
 *          0 Success, @state->disk is not a dynamic disk
 *         -1 An error occurred before enough information had been read
 *            Or @state->disk is a dynamic disk, but it may be corrupted
 */
int ldm_partition(struct parsed_partitions *state)
{
	struct ldmdb  *ldb;
	// LDM 데이터베이스 캐시 구조체
	unsigned long base;
	// LDM 데이터베이스 시작 LBA (config_start)
	int result = -1;

	BUG_ON(!state);

	/* Look for signs of a Dynamic Disk */
	if (!ldm_validate_partition_table(state))
	// MBR LDM 파티션 타입 확인; 없으면 Dynamic Disk 아니므로 0 반환
		return 0;
/* MBR에 LDM 파티션 타입이 없으면 Dynamic Disk가 아니며, 다른 파서가 처리할 수 있도록 0을 반환한다. */

	ldb = kmalloc_obj(*ldb);
	// LDM 파싱 캐시 메모리; 할당 실패 시 -1 반환
	if (!ldb) {
		ldm_crit ("Out of memory.");
		goto out;
	}

	/* Parse and check privheads. */
	if (!ldm_validate_privheads(state, &ldb->ph))
	// PRIVHEAD 3회 NVMe Read 및 검증
		goto out;		/* Already logged */
/* PRIVHEAD 읽기/검증에 실패하면 메타데이터 손상으로 -1을 반환한다. */

	/* All further references are relative to base (database start). */
	base = ldb->ph.config_start;
	// 이후 LBA 참조는 config_start 기준; NVMe namespace 내 메타데이터 시작

	/* Parse and check tocs and vmdb. */
	if (!ldm_validate_tocblocks(state, base, ldb) ||
	// TOCBLOCK 1~4회 NVMe Read 및 검증
	    !ldm_validate_vmdb(state, base, ldb))
	    // VMDB 1회 NVMe Read 및 검증
	    	goto out;		/* Already logged */
/* TOCBLOCK이나 VMDB 검증에 실패하면 더 이상 진행하지 않는다. */

	/* Initialize vblk lists in ldmdb struct */
	INIT_LIST_HEAD (&ldb->v_dgrp);
	INIT_LIST_HEAD (&ldb->v_disk);
	INIT_LIST_HEAD (&ldb->v_volu);
	INIT_LIST_HEAD (&ldb->v_comp);
	INIT_LIST_HEAD (&ldb->v_part);
	// VBLK 캐시 리스트 초기화

	if (!ldm_get_vblks(state, base, ldb)) {
	// VBLK 영역을 NVMe에서 반복 읽어 ldmdb 구성
		ldm_crit ("Failed to read the VBLKs from the database.");
		goto cleanup;
/* VBLK 읽기/파싱에 실패하면 할당된 리스트들을 정리한다. */
	}

	/* Finally, create the data partition devices. */
	if (ldm_create_data_partitions(state, ldb)) {
	// 현재 NVMe namespace에 속한 파티션을 gendisk에 등록
		ldm_debug ("Parsed LDM database successfully.");
		result = 1;
	}
/* 현재 NVMe namespace에 속한 파티션 등록에 성공하면 result=1을 반환한다. */
	/* else Already logged */

cleanup:
	ldm_free_vblks (&ldb->v_dgrp);
	// Disk Group VBLK 리스트 메모리 해제
	ldm_free_vblks (&ldb->v_disk);
	// Disk VBLK 리스트 메모리 해제
	ldm_free_vblks (&ldb->v_volu);
	// Volume VBLK 리스트 메모리 해제
	ldm_free_vblks (&ldb->v_comp);
	// Component VBLK 리스트 메모리 해제
	ldm_free_vblks (&ldb->v_part);
	// Partition VBLK 리스트 메모리 해제; NVMe 파티션 등록은 이미 완료됨
out:
	kfree (ldb);
	// LDM 파싱 캐시 해제; NVMe namespace 디스커버리 단계 종료
	return result;
}

/* NVMe 관점 핵심 요약 */
/*
 * - 이 파일은 NVMe namespace가 처음 인식될 때 디스커버리 단계에서 실행되는
 *   partition parser로, Windows LDM 메타데이터를 읽고 파티션을 등록한다.
 * - 모든 디스크 메타데이터 읽기는 read_part_sector()를 통해 이루어지며,
 *   이는 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe SQ/CQ를 통해
 *   처리된다(추정).
 * - struct privhead/vmdb/vblk_part의 start/size 필드는 NVMe namespace의
 *   LBA 주소 공간과 직접 연결되며, 파싱 결과가 gendisk 파티션 테이블에 반영된다.
 * - block/partitions/check.c의 check_partition()에서 파서들을 순회하듯
 *   호출되는 구조이며(추정), LDM 파서는 MBR 파티션 타입 0x42를 전제로 동작한다.
 * - NVMe SSD 입장에서는 512바이트 메타데이터 Read가 반복적으로 발생하므로,
 *   작은 임의 읽기(random read) 패턴이 될 수 있다(추정).
 */
