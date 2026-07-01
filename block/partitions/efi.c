// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * NVMe 관점 파일 요약:
 * 이 파일은 블록 계층의 파티션 검출 단계에서 EFI GUID Partition Table(GPT)을
 * 파싱한다. NVMe SSD가 부팅 디바이스나 데이터 디바이스로 인식되면, 커널은
 * 먼저 이 코드를 통해 디스크의 보호 MBR과 primary/alternate GPT 헤더를 읽고
 * 파티션 엔트리를 검증한다. 성공하면 각 파티션의 start/size가 blk_mq 구조로
 * 전달되어 이후 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 NVMe I/O 명령(SQ/CQ)이
 * 만들어진다. 즉, 이 파일은 NVMe 컨트롤러 드라이버 이전에 위치하는
 * 사용자 데이터 영역 분할의 관문이다.
 */

/************************************************************
 * EFI GUID Partition Table handling
 *
 * http://www.uefi.org/specs/
 * http://www.intel.com/technology/efi/
 *
 * efi.[ch] by Matt Domsch <Matt_Domsch@dell.com>
 *   Copyright 2000,2001,2002,2004 Dell Inc.
 *
 * TODO:
 *
 * Changelog:
 * Mon August 5th, 2013 Davidlohr Bueso <davidlohr@hp.com>
 * - detect hybrid MBRs, tighter pMBR checking & cleanups.
 *
 * Mon Nov 09 2004 Matt Domsch <Matt_Domsch@dell.com>
 * - test for valid PMBR and valid PGPT before ever reading
 *   AGPT, allow override with 'gpt' kernel command line option.
 * - check for first/last_usable_lba outside of size of disk
 *
 * Tue  Mar 26 2002 Matt Domsch <Matt_Domsch@dell.com>
 * - Ported to 2.5.7-pre1 and 2.5.7-dj2
 * - Applied patch to avoid fault in alternate header handling
 * - cleaned up find_valid_gpt
 * - On-disk structure and copy in memory is *always* LE now - 
 *   swab fields as needed
 * - remove print_gpt_header()
 * - only use first max_p partition entries, to keep the kernel minor number
 *   and partition numbers tied.
 *
 * Mon  Feb 04 2002 Matt Domsch <Matt_Domsch@dell.com>
 * - Removed __PRIPTR_PREFIX - not being used
 *
 * Mon  Jan 14 2002 Matt Domsch <Matt_Domsch@dell.com>
 * - Ported to 2.5.2-pre11 + library crc32 patch Linus applied
 *
 * Thu Dec 6 2001 Matt Domsch <Matt_Domsch@dell.com>
 * - Added compare_gpts().
 * - moved le_efi_guid_to_cpus() back into this file.  GPT is the only
 *   thing that keeps EFI GUIDs on disk.
 * - Changed gpt structure names and members to be simpler and more Linux-like.
 * 
 * Wed Oct 17 2001 Matt Domsch <Matt_Domsch@dell.com>
 * - Removed CONFIG_DEVFS_VOLUMES_UUID code entirely per Martin Wilck
 *
 * Wed Oct 10 2001 Matt Domsch <Matt_Domsch@dell.com>
 * - Changed function comments to DocBook style per Andreas Dilger suggestion.
 *
 * Mon Oct 08 2001 Matt Domsch <Matt_Domsch@dell.com>
 * - Change read_lba() to use the page cache per Al Viro's work.
 * - print u64s properly on all architectures
 * - fixed debug_printk(), now Dprintk()
 *
 * Mon Oct 01 2001 Matt Domsch <Matt_Domsch@dell.com>
 * - Style cleanups
 * - made most functions static
 * - Endianness addition
 * - remove test for second alternate header, as it's not per spec,
 *   and is unnecessary.  There's now a method to read/write the last
 *   sector of an odd-sized disk from user space.  No tools have ever
 *   been released which used this code, so it's effectively dead.
 * - Per Asit Mallick of Intel, added a test for a valid PMBR.
 * - Added kernel command line option 'gpt' to override valid PMBR test.
 *
 * Wed Jun  6 2001 Martin Wilck <Martin.Wilck@Fujitsu-Siemens.com>
 * - added devfs volume UUID support (/dev/volumes/uuids) for
 *   mounting file systems by the partition GUID. 
 *
 * Tue Dec  5 2000 Matt Domsch <Matt_Domsch@dell.com>
 * - Moved crc32() to linux/lib, added efi_crc32().
 *
 * Thu Nov 30 2000 Matt Domsch <Matt_Domsch@dell.com>
 * - Replaced Intel's CRC32 function with an equivalent
 *   non-license-restricted version.
 *
 * Wed Oct 25 2000 Matt Domsch <Matt_Domsch@dell.com>
 * - Fixed the last_lba() call to return the proper last block
 *
 * Thu Oct 12 2000 Matt Domsch <Matt_Domsch@dell.com>
 * - Thanks to Andries Brouwer for his debugging assistance.
 * - Code works, detects all the partitions.
 *
 ************************************************************/
#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/ctype.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include "check.h"
#include "efi.h"

/* 커널 커맨드라인 'gpt' 옵션으로 PMBR(보호 MBR) 검사를 우회할 수 있다.
 * 파티션 테이블 재읽기는 init 이후에도 발생하므로 __initdata가 아니다.
 * NVMe SSD가 가상 이미지나 비표준 레이아웃을 가진 경우 이 옵션이 필요할 수 있다. (추정)
 */
static int force_gpt; /* PMBR 우회 플래그: NVMe 비표준/가상 이미지 대응 (추정) */

/* gpt 커맨드라인 인자 파싱 */
static int __init
force_gpt_fn(char *str)
{
	force_gpt = 1;	/* PMBR 검사 강제 통과 */ /* 'gpt' 옵션 시 PMBR 없이 GPT 스캔, NVMe 가상 디스크 대응 (추정) */
	return 1;
}
__setup("gpt", force_gpt_fn);


/**
 * efi_crc32() - EFI 버전의 crc32 함수
 * @buf: crc32를 계산할 버퍼
 * @len: 버퍼 길이
 *
 * GPT 헤더와 파티션 엔트리 배열의 무결성을 검증하는 CRC32를 계산한다.
 * NVMe SSD는 PRP/SGL로 전달된 원본 데이터와 무관하게, 디스크에 기록된
 * GPT 메타데이터의 CRC가 정확해야 이 헤더를 신뢰할 수 있다.
 * Ethernet 다항식에 ~0 시드, 최종 ~0 XOR 방식을 사용한다.
 */
/**
 * efi_crc32() - EFI version of crc32 function
 * @buf: buffer to calculate crc32 of
 * @len: length of buf
 *
 * Description: Returns EFI-style CRC32 value for @buf
 * 
 * This function uses the little endian Ethernet polynomial
 * but seeds the function with ~0, and xor's with ~0 at the end.
 * Note, the EFI Specification, v1.02, has a reference to
 * Dr. Dobbs Journal, May 1994 (actually it's in May 1992).
 */
static inline u32
efi_crc32(const void *buf, unsigned long len)
{
	return (crc32(~0L, buf, len) ^ ~0L); /* buf는 read_lba -> read_part_sector -> NVMe Read로 채워진 GPT 메타데이터 */
}

/**
 * last_lba() - 디바이스의 마지막 논리 블록 번호 반환
 * @disk: 블록 디바이스 (gendisk)
 *
 * NVMe namespace의 전체 용량에서 논리 블록 크기(queue_logical_block_size)로
 * 나누어 최종 LBA를 계산한다. NVMe Identify Namespace의 NN(총 namespace 크기)와
 * 유사한 개념이며, GPT first/last_usable_lba 검증의 기준점이 된다.
 * Returns: 마지막 LBA, 오류 시 0.
 */
/**
 * last_lba(): return number of last logical block of device
 * @disk: block device
 * 
 * Description: Returns last LBA value on success, 0 on error.
 * This is stored (by sd and ide-geometry) in
 *  the part[0] entry for this disk, and is the number of
 *  physical sectors available on the disk.
 */
static u64 last_lba(struct gendisk *disk)
{
	/* bdev_nr_bytes: NVMe namespace 전체 바이트 수,
	 * queue_logical_block_size: NVMe 포맷된 LBA 크기(보통 512 or 4096)
	 */
	return div_u64(bdev_nr_bytes(disk->part0), /* bdev_nr_bytes: NVMe namespace 전체 용량(바이트) */
		       queue_logical_block_size(disk->queue)) - 1ULL; /* queue_logical_block_size: NVMe Format LBA size, Identify Namespace FORMAT -> lbads와 대응 (추정) */
}

/**
 * pmbr_part_valid() - 한 개의 PMBR 파티션 레코드가 GPT 보호 파티션인지 검사
 * @part: MBR 파티션 레코드
 *
 * NVMe SSD의 첫 512바이트(LBA 0)는 legacy MBR 또는 protective MBR을 담는다.
 * os_type이 0xEE이고 starting_lba가 1이면 GPT protective MBR로 인식한다.
 */
static inline int pmbr_part_valid(gpt_mbr_record *part)
{
	if (part->os_type != EFI_PMBR_OSTYPE_EFI_GPT) /* MBR partition_record[4] 중 os_type 0xEE가 GPT protective partition */
		goto invalid;

	/* GPT protective MBR의 starting_lba는 GPT Partition Header가 위치한 LBA 1을 가리킨다. */
	/* set to 0x00000001 (i.e., the LBA of the GPT Partition Header) */
	if (le32_to_cpu(part->starting_lba) != GPT_PRIMARY_PARTITION_TABLE_LBA) /* starting_lba=1이면 LBA 1에 GPT 헤더가 있음을 의미, NVMe Read SLBA=1 준비 */
		goto invalid;

	return GPT_MBR_PROTECTIVE;
invalid:
	return 0;
}

/*
 * is_pmbr_valid(): NVMe LBA 0의 보호 MBR(pMBR) 또는 하이브리드 MBR 검사
 * NVMe SSD는 첫 섹터에 legacy MBR 대신 protective MBR(0xEE)을 배치해
 * GPT 레이아웃임을 알린다. 이 함수는 check.c -> efi_partition() 호출 전
 * (또는 그 내부에서) GPT 존재 여부를 판단하는 관문 역할을 한다.
 */
/**
 * is_pmbr_valid(): test Protective MBR for validity
 * @mbr: pointer to a legacy mbr structure
 * @total_sectors: amount of sectors in the device
 *
 * Description: Checks for a valid protective or hybrid
 * master boot record (MBR). The validity of a pMBR depends
 * on all of the following properties:
 *  1) MSDOS signature is in the last two bytes of the MBR
 *  2) One partition of type 0xEE is found
 *
 * In addition, a hybrid MBR will have up to three additional
 * primary partitions, which point to the same space that's
 * marked out by up to three GPT partitions.
 *
 * Returns 0 upon invalid MBR, or GPT_MBR_PROTECTIVE or
 * GPT_MBR_HYBRID depending on the device layout.
 */
static int is_pmbr_valid(legacy_mbr *mbr, sector_t total_sectors)
{
	uint32_t sz = 0; /* pMBR size_in_lba 캐시, CHS 32비트 LBA 한계(2TiB)와 관련 */
	int i, part = 0, ret = 0; /* invalid by default */ /* 4개 primary partition record 순회, GPT 보호 파티션 인덱스 기록 */

	/* legacy_mbr.signature: MBR 시그니처 0xAA55 확인 */
	if (!mbr || le16_to_cpu(mbr->signature) != MSDOS_MBR_SIGNATURE) /* LBA 0 마지막 2바이트 0xAA55 확인, NVMe CQE success 후 데이터 무결성의 첫 관문 */
		goto done;

	/* 4개 primary 파티션 중 GPT 보호 파티션(0xEE) 탐색 */
	for (i = 0; i < 4; i++) { /* MBR primary partition record 4개 순회, 각 record는 16바이트 (boot|chs_start|os_type|chs_end|starting_lba|size_in_lba) */
		ret = pmbr_part_valid(&mbr->partition_record[i]); /* partition_record[i] 필드 오프셋: boot(1) | chs_start(3) | os_type(1) | chs_end(3) | starting_lba(4) | size_in_lba(4) */
		if (ret == GPT_MBR_PROTECTIVE) { /* protective MBR 발견 시 hybrid MBR 여부 추가 검사 */
			part = i;
			/*
			 * Ok, we at least know that there's a protective MBR,
			 * now check if there are other partition types for
			 * hybrid MBR.
			 */
			goto check_hybrid;
		}
	}

	if (ret != GPT_MBR_PROTECTIVE)
		goto done;
check_hybrid:
	/* 다른 non-EFI/non-empty 파티션이 있으면 hybrid MBR로 간주 */
	for (i = 0; i < 4; i++) /* GPT 외 추가 레거시 파티션 존재 시 hybrid MBR로 판정 */
		if ((mbr->partition_record[i].os_type !=
			EFI_PMBR_OSTYPE_EFI_GPT) &&
		    (mbr->partition_record[i].os_type != 0x00))
			ret = GPT_MBR_HYBRID;

	/*
	 * Protective MBRs take up the lesser of the whole disk
	 * or 2 TiB (32bit LBA), ignoring the rest of the disk.
	 * Some partitioning programs, nonetheless, choose to set
	 * the size to the maximum 32-bit limitation, disregarding
	 * the disk size.
	 *
	 * Hybrid MBRs do not necessarily comply with this.
	 *
	 * Consider a bad value here to be a warning to support dd'ing
	 * an image from a smaller disk to a larger disk.
	 */
	if (ret == GPT_MBR_PROTECTIVE) {
		sz = le32_to_cpu(mbr->partition_record[part].size_in_lba); /* CHS 32비트 LBA 한계(2TiB) 반영, size_in_lba가 전체 디스크 또는 0xFFFFFFFF 허용 */
		if (sz != (uint32_t) total_sectors - 1 && sz != 0xFFFFFFFF) /* total_sectors는 get_capacity() 값, NVMe namespace 총 512바이트 섹터 수 */
			pr_debug("GPT: mbr size in lba (%u) different than whole disk (%u).\n",
				 sz, (uint32_t)min(total_sectors - 1, 0xFFFFFFFF));
	}
done:
	return ret;
}

/**
 * read_lba() - 지정한 LBA부터 디스크 바이트를 읽음
 * @state: 파싱 중인 디스크 상태
 * @lba: GPT 관점의 512바이트 기반 논리 블록 주소
 * @buffer: 대상 버퍼
 * @count: 읽을 바이트 수
 *
 * GPT의 모든 LBA는 512바이트 단위이지만, NVMe SSD는 queue_logical_block_size가
 * 512/4096 등으로 포맷될 수 있다. 따라서 lba에 (lblk_size/512)를 곱해
 * read_part_sector()용 커널 섹터 번호 n으로 변환한다.
 * 이 요청은 이후 submit_bio -> blk_mq_submit_bio -> ... -> nvme_queue_rq 경로로
 * NVMe Read 명령(CID 할당, PRP/SGL 생성, doorbell 갱신)으로 변환된다. (추정)
 * Returns: 성공 시 읽은 바이트 수, 실패 시 0.
 */
/**
 * read_lba(): Read bytes from disk, starting at given LBA
 * @state: disk parsed partitions
 * @lba: the Logical Block Address of the partition table
 * @buffer: destination buffer
 * @count: bytes to read
 *
 * Description: Reads @count bytes from @state->disk into @buffer.
 * Returns number of bytes read on success, 0 on error.
 */
static size_t read_lba(struct parsed_partitions *state,
		       u64 lba, u8 *buffer, size_t count)
{
	size_t totalreadcount = 0; /* read_part_sector 호출 누적 바이트 카운트 */
	/* GPT LBA(512B) -> 커널 섹터(512B) 번호: NVMe 4K 포맷 시 8배 차이 */
	sector_t n = lba * /* GPT LBA(512B)를 커널 섹터 n으로 변환: NVMe 4K 포맷 시 LBA 1 == 섹터 8 */
		(queue_logical_block_size(state->disk->queue) / 512);

	if (!buffer || lba > last_lba(state->disk)) /* 버퍼 NULL 또는 NVMe namespace 끝 초과 시 조기 리턴 */
                return 0;

	while (count) { /* 512바이트씩 read_part_sector 호출 -> NVMe Read 명령이 count/512 만큼 반복 제출 (추정) */
		int copied = 512; /* GPT 메타데이터는 512바이트 단위로 처리, NVMe PRP/SGL entry 단위(4K 정렬)와 다를 수 있음 */
		Sector sect;
		/* read_part_sector -> ... -> NVMe Read 명령으로 512바이트 단위 읽기 (추정) */
		unsigned char *data = read_part_sector(state, n++, &sect); /* read_part_sector -> bdev_read_sector -> submit_bio_wait -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(SQ doorbell) (추정) */
		if (!data) /* NVMe CQE 오류, IO scheduler timeout, 또는 메모리 할당 실패 시 루프 탈출, 스캔 중단 가능 */
			break;
		if (copied > count) /* 마지막 512바이트 미만 조각 처리 */
			copied = count;
		memcpy(buffer, data, copied); /* read_part_sector가 반환한 섹터 버퍼를 GPT 파싱 버퍼로 복사 */
		put_dev_sector(sect); /* 섹터 버퍼 해제, NVMe Read 완료 자원 정리 */
		buffer += copied; /* 다음 GPT 메타데이터 오프셋 이동 */
		totalreadcount +=copied; /* 읽은 바이트 누적, 반환값으로 사용 */
		count -= copied; /* 남은 바이트 감소, 0이면 while 종료 */
	}
	return totalreadcount;
}

/**
 * alloc_read_gpt_entries() - 디스크에서 GPT 파티션 엔트리(PTE) 배열 읽기
 * @state: 파싱 중인 디스크 상태
 * @gpt: 검증된 GPT 헤더
 *
 * gpt->num_partition_entries와 gpt->sizeof_partition_entry를 곱해 PTE 배열
 * 전체 크기를 결정하고, gpt->partition_entry_lba부터 read_lba()로 읽어온다.
 * NVMe SSD에서 PTE는 일반적으로 LBA 2부터 연속 배치되며, 각 엔트리는
 * starting_lba/ending_lba를 포함해 NVMe Read/Write의 SLBA 입력값과 직결된다.
 */
/**
 * alloc_read_gpt_entries(): reads partition entries from disk
 * @state: disk parsed partitions
 * @gpt: GPT header
 * 
 * Description: Returns ptes on success,  NULL on error.
 * Allocates space for PTEs based on information found in @gpt.
 * Notes: remember to free pte when you're done!
 */
static gpt_entry *alloc_read_gpt_entries(struct parsed_partitions *state,
					 gpt_header *gpt)
{
	size_t count; /* PTE 배열 kmalloc 할당 크기 */
	gpt_entry *pte;

	if (!gpt) /* NULL 헤더 방어, NVMe 메타데이터 없음 */
		return NULL;

	/* gpt_header.num_partition_entries * sizeof_partition_entry = PTE 배열 총 바이트 */
	count = (size_t)le32_to_cpu(gpt->num_partition_entries) * /* num_partition_entries * sizeof_partition_entry = PTE 배열 전체 바이트 */
                le32_to_cpu(gpt->sizeof_partition_entry);
	if (!count) /* 0개 엔트리 시 NVMe 파티션 정보 없음으로 처리 */
		return NULL;
	pte = kmalloc(count, GFP_KERNEL); /* PTE 배열 메모리 할당, 실패 시 GPT 스캔 중단 */
	if (!pte) /* 메모리 부족: NVMe 파티션 메타데이터 파싱 불가, 블록 레이블 없이 namespace 전체 사용 가능 */
		return NULL;

	/* gpt->partition_entry_lba: PTE 배열 시작 LBA (일반적으로 LBA 2) */
	if (read_lba(state, le64_to_cpu(gpt->partition_entry_lba), /* partition_entry_lba(보통 LBA 2)부터 PTE 배열 연속 읽기 -> 다수 NVMe Read 제출 */
			(u8 *) pte, count) < count) {
		kfree(pte); /* read_lba 실패(예: NVMe CQE error) 시 pte 정리 */
                pte=NULL;
		return NULL;
	}
	return pte;
}

/**
 * alloc_read_gpt_header() - GPT 헤더 할당 및 디스크에서 읽기
 * @state: 파싱 중인 디스크 상태
 * @lba: GPT 헤더가 위치한 LBA (primary=1, alternate=lastlba)
 *
 * NVMe SSD의 LBA 1(primary) 또는 마지막 LBA(alternate)에서 GPT 헤더를 읽는다.
 * 할당 크기는 queue_logical_block_size이며, 헤더는 항상 하나의 논리 블록에
 * 들어간다고 가정한다. (추정)
 */
/**
 * alloc_read_gpt_header(): Allocates GPT header, reads into it from disk
 * @state: disk parsed partitions
 * @lba: the Logical Block Address of the partition table
 * 
 * Description: returns GPT header on success, NULL on error.   Allocates
 * and fills a GPT header starting at @ from @state->disk.
 * Note: remember to free gpt when finished with it.
 */
static gpt_header *alloc_read_gpt_header(struct parsed_partitions *state,
					 u64 lba)
{
	gpt_header *gpt;
	unsigned ssz = queue_logical_block_size(state->disk->queue); /* NVMe namespace 논리 블록 크기(512 or 4096) = 헤더 할당/읽기 단위 */

	gpt = kmalloc(ssz, GFP_KERNEL); /* 하나의 논리 블록 크기만큼 GPT 헤더 버퍼 할당 */
	if (!gpt) /* 헤더 버퍼 할당 실패 시 NVMe GPT 스캔 불가 */
		return NULL;

	/* primary GPT(LBA 1) 또는 alternate GPT(마지막 LBA)에서 헤더 읽기 */
	if (read_lba(state, lba, (u8 *) gpt, ssz) < ssz) { /* primary(LBA 1) 또는 alternate(마지막 LBA)에서 헤더 읽기, NVMe Read 실패 시 fallback */
		kfree(gpt); /* 실패 시 헤더 메모리 정리 */
                gpt=NULL;
		return NULL;
	}

	return gpt;
}

/**
 * is_gpt_valid() - 하나의 GPT 헤더와 PTE 배열의 유효성 검사
 * @state: 파싱 중인 디스크 상태
 * @lba: 검사할 GPT 헤더의 LBA
 * @gpt: 반환용 GPT 헤더 이중 포인터
 * @ptes: 반환용 PTE 배열 이중 포인터
 *
 * NVMe SSD에 기록된 GPT 헤더의 서명, 헤더 크기, my_lba, first/last_usable_lba,
 * 헤더 CRC32, PTE 배열 CRC32를 검증한다. 헤더가 손상되면 alternate GPT로
 * fallback하는 근거가 되며, 이는 NVMe namespace의 메타데이터 신뢰성과 직결된다.
 */
/**
 * is_gpt_valid() - tests one GPT header and PTEs for validity
 * @state: disk parsed partitions
 * @lba: logical block address of the GPT header to test
 * @gpt: GPT header ptr, filled on return.
 * @ptes: PTEs ptr, filled on return.
 *
 * Description: returns 1 if valid,  0 on error.
 * If valid, returns pointers to newly allocated GPT header and PTEs.
 */
static int is_gpt_valid(struct parsed_partitions *state, u64 lba,
			gpt_header **gpt, gpt_entry **ptes)
{
	u32 crc, origcrc; /* 헤더 CRC32 저장 */
	u64 lastlba, pt_size; /* NVMe namespace 경계 및 PTE 배열 크기 */

	if (!ptes) /* ptes 출력 인자 NULL 방어 */
		return 0;
	if (!(*gpt = alloc_read_gpt_header(state, lba))) /* NVMe LBA에서 GPT 헤더 읽기 실패, CQE error 또는 메모리 부족 가능 */
		return 0;

	/* Check the GUID Partition Table signature */
	/* gpt_header.signature: "EFI PART" 시그니처(0x5452415020494645) 확인 */
	if (le64_to_cpu((*gpt)->signature) != GPT_HEADER_SIGNATURE) { /* 오프셋 0x00: signature "EFI PART"(0x5452415020494645) 확인, 잘못된 NVMe LBA 1 내용 */
		pr_debug("GUID Partition Table Header signature is wrong:"
			 "%lld != %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->signature),
			 (unsigned long long)GPT_HEADER_SIGNATURE);
		goto fail;
	}

	/* Check the GUID Partition Table header size is too big */
	/* gpt_header.header_size: NVMe 논리 블록 크기를 초과하면 invalid */
	if (le32_to_cpu((*gpt)->header_size) > /* 오프셋 0x0C: header_size가 NVMe 논리 블록보다 큼, invalid */
			queue_logical_block_size(state->disk->queue)) {
		pr_debug("GUID Partition Table Header size is too large: %u > %u\n",
			le32_to_cpu((*gpt)->header_size),
			queue_logical_block_size(state->disk->queue));
		goto fail;
	}

	/* Check the GUID Partition Table header size is too small */
	/* gpt_header.header_size: gpt_header 구조체 최소 크기보다 작으면 invalid */
	if (le32_to_cpu((*gpt)->header_size) < sizeof(gpt_header)) { /* 오프셋 0x0C: header_size가 gpt_header 구조체 최소 크기보다 작음, invalid */
		pr_debug("GUID Partition Table Header size is too small: %u < %zu\n",
			le32_to_cpu((*gpt)->header_size),
			sizeof(gpt_header));
		goto fail;
	}

	/* Check the GUID Partition Table CRC */
	/* gpt_header.header_crc32: 헤더 CRC32 계산 후 비교 (계산 시 crc 필드는 0으로 둠) */
	origcrc = le32_to_cpu((*gpt)->header_crc32); /* 오프셋 0x10: 저장된 header_crc32 읽기 */
	(*gpt)->header_crc32 = 0; /* CRC 계산 시 header_crc32 필드 자신은 0으로 간주 (UEFI spec) */
	crc = efi_crc32((const unsigned char *) (*gpt), le32_to_cpu((*gpt)->header_size)); /* 헤더 전체(0 ~ header_size) CRC32 재계산, NVMe에서 읽은 원본 데이터 사용 */

	if (crc != origcrc) { /* CRC 불일치: NVMe media 손상 또는 전송 오류 (CQE status는 success였으나 데이터 무결성 깨짐) */
		pr_debug("GUID Partition Table Header CRC is wrong: %x != %x\n",
			 crc, origcrc);
		goto fail;
	}
	(*gpt)->header_crc32 = cpu_to_le32(origcrc); /* CRC 필드 원복 (fail path에서 kfree 전 복원, 디버깅용) */

	/* Check that the my_lba entry points to the LBA that contains
	 * the GUID Partition Table */
	/* gpt_header.my_lba: 현재 읽은 LBA와 일치해야 함 (primary/alternate 구분) */
	if (le64_to_cpu((*gpt)->my_lba) != lba) { /* 오프셋 0x18: my_lba는 현재 읽은 LBA와 일치해야 함 (primary vs alternate 구분) */
		pr_debug("GPT my_lba incorrect: %lld != %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->my_lba),
			 (unsigned long long)lba);
		goto fail;
	}

	/* Check the first_usable_lba and last_usable_lba are
	 * within the disk.
	 */
	/* gpt_header.first_usable_lba/last_usable_lba: NVMe namespace 경계 내에 있어야 함 */
	lastlba = last_lba(state->disk); /* NVMe namespace 마지막 LBA 재계산 */
	if (le64_to_cpu((*gpt)->first_usable_lba) > lastlba) { /* 오프셋 0x28: first_usable_lba가 NVMe namespace 범위 초과 */
		pr_debug("GPT: first_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->first_usable_lba),
			 (unsigned long long)lastlba);
		goto fail;
	}
	if (le64_to_cpu((*gpt)->last_usable_lba) > lastlba) { /* 오프셋 0x30: last_usable_lba가 NVMe namespace 범위 초과 */
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->last_usable_lba),
			 (unsigned long long)lastlba);
		goto fail;
	}
	if (le64_to_cpu((*gpt)->last_usable_lba) < le64_to_cpu((*gpt)->first_usable_lba)) { /* usable LBA 범위 역전, NVMe namespace 레이아웃 오류 */
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->last_usable_lba),
			 (unsigned long long)le64_to_cpu((*gpt)->first_usable_lba));
		goto fail;
	}
	/* Check that sizeof_partition_entry has the correct value */
	/* gpt_header.sizeof_partition_entry: 커널의 sizeof(gpt_entry)와 일치해야 함 */
	if (le32_to_cpu((*gpt)->sizeof_partition_entry) != sizeof(gpt_entry)) { /* 오프셋 0x4C: PTE 크기가 커널 gpt_entry(128바이트)와 불일치 */
		pr_debug("GUID Partition Entry Size check failed.\n");
		goto fail;
	}

	/* Sanity check partition table size */
	/* gpt_header.num_partition_entries * sizeof_partition_entry: kmalloc 최대 크기 초과 금지 */
	pt_size = (u64)le32_to_cpu((*gpt)->num_partition_entries) * /* 오프셋 0x50/0x54: PTE 배열 총 크기 = num_partition_entries * sizeof_partition_entry */
		le32_to_cpu((*gpt)->sizeof_partition_entry);
	if (pt_size > KMALLOC_MAX_SIZE) { /* PTE 배열이 커널 슬랩 한계 초과, 스캔 중단 (악의적/손상된 GPT) */
		pr_debug("GUID Partition Table is too large: %llu > %lu bytes\n",
			 (unsigned long long)pt_size, KMALLOC_MAX_SIZE);
		goto fail;
	}

	if (!(*ptes = alloc_read_gpt_entries(state, *gpt))) /* PTE 배열 읽기 실패: NVMe LBA 연속 읽기 또는 메모리 할당 실패 */
		goto fail;

	/* Check the GUID Partition Entry Array CRC */
	/* gpt_header.partition_entry_array_crc32: PTE 배열 전체 CRC32 검증 */
	crc = efi_crc32((const unsigned char *) (*ptes), pt_size); /* PTE 배열 전체 CRC32 계산, NVMe로부터 읽은 모든 파티션 엔트리 대상 */

	if (crc != le32_to_cpu((*gpt)->partition_entry_array_crc32)) { /* 오프셋 0x58: PTE CRC32 불일치, NVMe media 손상 가능 */
		pr_debug("GUID Partition Entry Array CRC check failed.\n");
		goto fail_ptes;
	}

	/* We're done, all's well */
	return 1;

 fail_ptes:
	kfree(*ptes); /* 손상된 PTE 배열 해제 */
	*ptes = NULL;
 fail:
	kfree(*gpt); /* 손상된 헤더 해제, GPT 스캔 실패 */
	*gpt = NULL;
	return 0;
}

/**
 * is_pte_valid() - 단일 GPT 파티션 엔트리(PTE) 유효성 검사
 * @pte: 검사할 PTE
 * @lastlba: 디스크의 마지막 LBA
 *
 * NVMe SSD의 usable LBA 범위를 벗어나는 파티션은 무시한다. 또한 unused
 * partition_type_guid(NULL_GUID) 파티션은 커널에 등록하지 않는다.
 */
/**
 * is_pte_valid() - tests one PTE for validity
 * @pte:pte to check
 * @lastlba: last lba of the disk
 *
 * Description: returns 1 if valid,  0 on error.
 */
static inline int
is_pte_valid(const gpt_entry *pte, const u64 lastlba)
{
	/* gpt_entry.partition_type_guid: NULL_GUID이면 미사용 엔트리 */
	/* gpt_entry.starting_lba/ending_lba: NVMe namespace 경계 초과 검사 */
	if ((!efi_guidcmp(pte->partition_type_guid, NULL_GUID)) || /* 오프셋 0x00: partition_type_guid가 00000000-0000-0000-0000-000000000000이면 미사용 엔트리 */
	    le64_to_cpu(pte->starting_lba) > lastlba         || /* 오프셋 0x20: starting_lba가 NVMe namespace 끝 초과 */
	    le64_to_cpu(pte->ending_lba)   > lastlba) /* 오프셋 0x28: ending_lba가 NVMe namespace 끝 초과 */
		return 0;
	return 1;
}

/**
 * compare_gpts() - primary와 alternate GPT 헤더 비교
 * @pgpt: primary GPT 헤더
 * @agpt: alternate GPT 헤더
 * @lastlba: 디스크의 마지막 LBA
 *
 * NVMe SSD에 기록된 primary/alternate GPT 헤더의 my_lba, alternate_lba,
 * first/last_usable_lba, disk_guid, num_partition_entries, sizeof_partition_entry,
 * partition_entry_array_crc32가 일치하는지 확인한다. 불일치 시 경고를 출력하고
 * 사용자에게 GNU parted로 수정할 것을 권고한다.
 */
/**
 * compare_gpts() - Search disk for valid GPT headers and PTEs
 * @pgpt: primary GPT header
 * @agpt: alternate GPT header
 * @lastlba: last LBA number
 *
 * Description: Returns nothing.  Sanity checks pgpt and agpt fields
 * and prints warnings on discrepancies.
 * 
 */
static void
compare_gpts(gpt_header *pgpt, gpt_header *agpt, u64 lastlba)
{
	int error_found = 0;
	if (!pgpt || !agpt)
		return;
	/* gpt_header.my_lba vs alternate_lba: primary의 my_lba는 alternate의 alternate_lba와 같아야 함 */
	if (le64_to_cpu(pgpt->my_lba) != le64_to_cpu(agpt->alternate_lba)) { /* 오프셋 0x18/0x20: primary.my_lba == alternate.alternate_lba 확인 */
		pr_warn("GPT:Primary header LBA != Alt. header alternate_lba\n");
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->my_lba),
                       (unsigned long long)le64_to_cpu(agpt->alternate_lba));
		error_found++;
	}
	if (le64_to_cpu(pgpt->alternate_lba) != le64_to_cpu(agpt->my_lba)) { /* 오프셋 0x20/0x18: primary.alternate_lba == alternate.my_lba 확인 */
		pr_warn("GPT:Primary header alternate_lba != Alt. header my_lba\n");
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->alternate_lba),
                       (unsigned long long)le64_to_cpu(agpt->my_lba));
		error_found++;
	}
	if (le64_to_cpu(pgpt->first_usable_lba) != /* 오프셋 0x28: NVMe usable 영역 시작 일치 */
            le64_to_cpu(agpt->first_usable_lba)) {
		pr_warn("GPT:first_usable_lbas don't match.\n");
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->first_usable_lba),
                       (unsigned long long)le64_to_cpu(agpt->first_usable_lba));
		error_found++;
	}
	if (le64_to_cpu(pgpt->last_usable_lba) != /* 오프셋 0x30: NVMe usable 영역 끝 일치 */
            le64_to_cpu(agpt->last_usable_lba)) {
		pr_warn("GPT:last_usable_lbas don't match.\n");
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->last_usable_lba),
                       (unsigned long long)le64_to_cpu(agpt->last_usable_lba));
		error_found++;
	}
	/* gpt_header.disk_guid: primary와 alternate의 디스크 GUID 일치해야 함 */
	if (efi_guidcmp(pgpt->disk_guid, agpt->disk_guid)) { /* 오프셋 0x38: 디스크 GUID 일치, NVMe namespace 식별자와 연결 (추정) */
		pr_warn("GPT:disk_guids don't match.\n");
		error_found++;
	}
	if (le32_to_cpu(pgpt->num_partition_entries) != /* 오프셋 0x50: 파티션 개수 일치 */
            le32_to_cpu(agpt->num_partition_entries)) {
		pr_warn("GPT:num_partition_entries don't match: "
		       "0x%x != 0x%x\n",
		       le32_to_cpu(pgpt->num_partition_entries),
		       le32_to_cpu(agpt->num_partition_entries));
		error_found++;
	}
	if (le32_to_cpu(pgpt->sizeof_partition_entry) != /* 오프셋 0x54: PTE 크기 일치 */
            le32_to_cpu(agpt->sizeof_partition_entry)) {
		pr_warn("GPT:sizeof_partition_entry values don't match: "
		       "0x%x != 0x%x\n",
                       le32_to_cpu(pgpt->sizeof_partition_entry),
		       le32_to_cpu(agpt->sizeof_partition_entry));
		error_found++;
	}
	if (le32_to_cpu(pgpt->partition_entry_array_crc32) != /* 오프셋 0x58: PTE CRC 일치, NVMe media 두 위치 데이터 비교 */
            le32_to_cpu(agpt->partition_entry_array_crc32)) {
		pr_warn("GPT:partition_entry_array_crc32 values don't match: "
		       "0x%x != 0x%x\n",
                       le32_to_cpu(pgpt->partition_entry_array_crc32),
		       le32_to_cpu(agpt->partition_entry_array_crc32));
		error_found++;
	}
	/* gpt_header.alternate_lba: primary는 alternate가 마지막 LBA라고 가리켜야 함 */
	if (le64_to_cpu(pgpt->alternate_lba) != lastlba) { /* primary의 alternate_lba는 NVMe 마지막 LBA를 가리켜야 함 */
		pr_warn("GPT:Primary header thinks Alt. header is not at the end of the disk.\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(pgpt->alternate_lba),
			(unsigned long long)lastlba);
		error_found++;
	}

	/* gpt_header.my_lba: alternate 헤더는 마지막 LBA에 위치해야 함 */
	if (le64_to_cpu(agpt->my_lba) != lastlba) { /* alternate 헤더는 NVMe 마지막 LBA에 위치해야 함 */
		pr_warn("GPT:Alternate GPT header not at the end of the disk.\n");
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(agpt->my_lba),
			(unsigned long long)lastlba);
		error_found++;
	}

	if (error_found)
		pr_warn("GPT: Use GNU Parted to correct GPT errors.\n");
	return;
}

/**
 * find_valid_gpt() - 디스크에서 유효한 GPT 헤더와 PTE 검색
 * @state: 파싱 중인 디스크 상태
 * @gpt: 반환용 GPT 헤더 이중 포인터
 * @ptes: 반환용 PTE 배열 이중 포인터
 *
 * check.c -> efi_partition() 내부에서 호출된다. (추정)
 * 먼저 LBA 0의 protective MBR을 검사하고, primary GPT(LBA 1)와 alternate GPT
 * (마지막 LBA)를 읽어 유효한 쪽을 선택한다. NVMe SSD에서 파티션 테이블을 찾지
 * 못하면 이 디스크는 NVMe I/O 요청 시에도 블록 레이블 없이 전체 namespace로
 * 다뤄지게 된다.
 */
/**
 * find_valid_gpt() - Search disk for valid GPT headers and PTEs
 * @state: disk parsed partitions
 * @gpt: GPT header ptr, filled on return.
 * @ptes: PTEs ptr, filled on return.
 *
 * Description: Returns 1 if valid, 0 on error.
 * If valid, returns pointers to newly allocated GPT header and PTEs.
 * Validity depends on PMBR being valid (or being overridden by the
 * 'gpt' kernel command line option) and finding either the Primary
 * GPT header and PTEs valid, or the Alternate GPT header and PTEs
 * valid.  If the Primary GPT header is not valid, the Alternate GPT header
 * is not checked unless the 'gpt' kernel command line option is passed.
 * This protects against devices which misreport their size, and forces
 * the user to decide to use the Alternate GPT.
 */
static int find_valid_gpt(struct parsed_partitions *state, gpt_header **gpt,
			  gpt_entry **ptes)
{
	int good_pgpt = 0, good_agpt = 0, good_pmbr = 0; /* primary/alternate/pMBR 유효성 플래그 */
	gpt_header *pgpt = NULL, *agpt = NULL; /* primary/alternate GPT 헤더 포인터 */
	gpt_entry *pptes = NULL, *aptes = NULL; /* primary/alternate PTE 배열 포인터 */
	legacy_mbr *legacymbr;
	struct gendisk *disk = state->disk;
	const struct block_device_operations *fops = disk->fops;
	sector_t total_sectors = get_capacity(state->disk); /* NVMe namespace 총 512바이트 섹터 수 (queue 논리 블록 크기와 무관) */
	u64 lastlba; /* NVMe 논리 블록 기준 마지막 LBA */

	if (!ptes)
		return 0;

	lastlba = last_lba(state->disk); /* alternate GPT 위치 및 범위 검증 기준 */
	/* force_gpt 미설정 시 LBA 0의 protective MBR부터 검사 */
        if (!force_gpt) { /* 기본 동작: LBA 0 pMBR부터 확인, NVMe 보호 MBR 없으면 GPT 스캔 중단 */
		/* This will be added to the EFI Spec. per Intel after v1.02. */
		legacymbr = kzalloc_obj(*legacymbr); /* legacy_mbr 구조체 크기만큼 0으로 할당 (GPT protective MBR용) */
		if (!legacymbr) /* LBA 0 읽기 버퍼 할당 실패, NVMe 파티션 스캔 전체 중단 */
			goto fail;

		/* LBA 0: legacy/protective MBR 읽기 (NVMe 첫 512바이트) */
		read_lba(state, 0, (u8 *)legacymbr, sizeof(*legacymbr)); /* LBA 0: NVMe Read로 512바이트 pMBR/legacy MBR 읽기, 실패 반환값 무시(이후 is_pmbr_valid에서 signature로 거름) */
		good_pmbr = is_pmbr_valid(legacymbr, total_sectors); /* pMBR 시그니처/보호 파티션 검증, hybrid MBR 반환 가능 */
		kfree(legacymbr); /* MBR 버퍼 해제 */

		if (!good_pmbr) /* pMBR invalid 시 GPT 스캔 중단, msdos_partition()이 legacy MBR 처리 */
			goto fail;

		pr_debug("Device has a %s MBR\n",
			 good_pmbr == GPT_MBR_PROTECTIVE ?
						"protective" : "hybrid");
	}

	/* LBA 1: primary GPT 헤더/엔트리 읽기 및 검증 */
	good_pgpt = is_gpt_valid(state, GPT_PRIMARY_PARTITION_TABLE_LBA, /* LBA 1: primary GPT 헤더+PTE 읽기 및 검증, NVMe Read 1회 + PTE 연속 읽기 */
				 &pgpt, &pptes);
	/* primary가 유효하면 primary가 가리키는 alternate LBA에서 alternate GPT 검증 */
        if (good_pgpt) /* primary 유효할 때만 alternate 자동 검증, fallback 최소화 */
		good_agpt = is_gpt_valid(state, /* primary 헤더의 alternate_lba 필드로 alternate GPT 읽기 (보통 NVMe 마지막 LBA) */
					 le64_to_cpu(pgpt->alternate_lba),
					 &agpt, &aptes);
	/* primary 없고 force_gpt 시 마지막 LBA에서 alternate GPT 직접 검증 */
        if (!good_agpt && force_gpt) /* primary 손상 + force_gpt 시 마지막 LBA에서 alternate 직접 탐색 */
                good_agpt = is_gpt_valid(state, lastlba, &agpt, &aptes); /* NVMe namespace 마지막 LBA에서 alternate GPT 읽기 */

	/* block_device_operations.alternative_gpt_sector가 제공하면 사용 (일부 NVMe/SCSI 장치) */
	if (!good_agpt && force_gpt && fops->alternative_gpt_sector) { /* 일부 NVMe/SCSI 드라이버가 alternative_gpt_sector 콜백 제공 (추정) */
		sector_t agpt_sector;
		int err;

		err = fops->alternative_gpt_sector(disk, &agpt_sector); /* 드라이버별 alternate GPT 섹터 힌트 획득 */
		if (!err) /* 힌트 획득 성공 시에만 alternate GPT 재검증 */
			good_agpt = is_gpt_valid(state, agpt_sector, /* alternative_gpt_sector 힌트 위치에서 NVMe Read로 GPT 검증 */
						 &agpt, &aptes);
	}

        /* The obviously unsuccessful case */
        if (!good_pgpt && !good_agpt) /* primary/alternate 모두 invalid: NVMe GPT 메타데이터 손상, 스캔 실패 */
                goto fail;

	/* primary/alternate GPT 헤더 상호 비교 */
        compare_gpts(pgpt, agpt, lastlba); /* primary/alternate 헤더 상호 검증, 불일치 시 경고 (NVMe media 데이터 일관성 문제) */

        /* The good cases */
        if (good_pgpt) { /* primary GPT 우선 사용 */
                *gpt  = pgpt; /* primary GPT 헤더 결과 반환 */
                *ptes = pptes; /* primary PTE 배열 결과 반환 */
                kfree(agpt); /* alternate GPT 헤더 메모리 정리 */
                kfree(aptes); /* alternate PTE 배열 메모리 정리 */
		if (!good_agpt) /* alternate 손상 경고, NVMe media 백업 헤더 손상 가능 */
                        pr_warn("Alternate GPT is invalid, using primary GPT.\n");
                return 1;
        }
        else if (good_agpt) { /* primary 손상 시 alternate GPT 사용 */
                *gpt  = agpt; /* alternate GPT 헤더 결과 반환 */
                *ptes = aptes; /* alternate PTE 배열 결과 반환 */
                kfree(pgpt); /* primary GPT 헤더 메모리 정리 */
                kfree(pptes); /* primary PTE 배열 메모리 정리 */
		pr_warn("Primary GPT is invalid, using alternate GPT.\n");
                return 1;
        }

 fail:
        kfree(pgpt);
        kfree(agpt);
        kfree(pptes);
        kfree(aptes);
        *gpt = NULL;
        *ptes = NULL;
        return 0;
}

/**
 * utf16_le_to_7bit() - UTF-16LE 문자열을 7비트 ASCII로 단순 변환
 * @in: 입력 UTF-16LE 문자열
 * @size: 입력 문자열 크기
 * @out: 출력 버퍼 (size+1 문자 저장 가능)
 *
 * GPT 파티션 이름(partition_name)은 UTF-16LE로 저장되어 있다. NVMe SSD는 이
 * 이름과 무관하게 I/O를 처리하지만, 커널은 이를 볼륨 레이블로 노출한다.
 */
/**
 * utf16_le_to_7bit(): Naively converts a UTF-16LE string to 7-bit ASCII characters
 * @in: input UTF-16LE string
 * @size: size of the input string
 * @out: output string ptr, should be capable to store @size+1 characters
 *
 * Description: Converts @size UTF16-LE symbols from @in string to 7-bit
 * ASCII characters and stores them to @out. Adds trailing zero to @out array.
 */
static void utf16_le_to_7bit(const __le16 *in, unsigned int size, u8 *out)
{
	unsigned int i = 0;

	out[size] = 0;

	while (i < size) {
		u8 c = le16_to_cpu(in[i]) & 0x7f;

		if (c && !isprint(c))
			c = '!';
		out[i] = c;
		i++;
	}
}

/**
 * efi_partition() - GPT 파티션 스캔 및 커널 파티션 등록
 * @state: 파싱 중인 디스크 상태
 *
 * check.c에서 파티션 테이블 탐색기로 호출된다. (msdos_partition()이 LBA 0의
 * legacy MBR을 먼저 처리한 뒤, 이 함수가 protective MBR + GPT를 처리한다.)
 * find_valid_gpt()로 헤더와 PTE를 얻은 뒤, 각 파티션의 starting_lba와
 * ending_lba를 이용해 put_partition()으로 blk_mq가 인식할 수 있는 파티션
 * 범위를 등록한다. 이후 NVMe I/O는 submit_bio -> blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를
 * 거쳐 각 파티션의 SLBA에 맞는 SQ/CQ 명령으로 변환된다.
 * Returns: -1(읽기 실패), 0(해당 없음), 1(성공).
 */
/**
 * efi_partition - scan for GPT partitions
 * @state: disk parsed partitions
 *
 * Description: called from check.c, if the disk contains GPT
 * partitions, sets up partition entries in the kernel.
 *
 * If the first block on the disk is a legacy MBR,
 * it will get handled by msdos_partition().
 * If it's a Protective MBR, we'll handle it here.
 *
 * We do not create a Linux partition for GPT, but
 * only for the actual data partitions.
 * Returns:
 * -1 if unable to read the partition table
 *  0 if this isn't our partition table
 *  1 if successful
 *
 */
int efi_partition(struct parsed_partitions *state)
{
	gpt_header *gpt = NULL; /* find_valid_gpt에서 할당된 primary/alternate 헤더 */
	gpt_entry *ptes = NULL; /* find_valid_gpt에서 할당된 PTE 배열 */
	u32 i; /* PTE 배열 인덱스 */
	/* 커널 섹터(512B) 기준으로 변환할 때 사용하는 계수 (NVMe 4K 시 8) */
	unsigned ssz = queue_logical_block_size(state->disk->queue) / 512; /* NVMe 논리 블록당 512바이트 섹터 수: 4K 포맷 시 8, put_partition에 커널 섹터 단위로 변환 */

	/* find_valid_gpt()가 실패하면 GPT 파티션이 아님 (msdos_partition()이 처리할 수도 있음) */
	if (!find_valid_gpt(state, &gpt, &ptes) || !gpt || !ptes) { /* GPT 메타데이터 읽기/검증 실패, NVMe namespace 전체로 사용하거나 msdos_partition fallback */
		kfree(gpt); /* 실패 시 GPT 헤더 메모리 정리 */
		kfree(ptes); /* 실패 시 PTE 메모리 정리, GPT 파티션 없음 반환(0) */
		return 0;
	}

	pr_debug("GUID Partition Table is valid!  Yea!\n");

	/* gpt_header.num_partition_entries만큼 파티션 엔트리 순회 */
	for (i = 0; i < le32_to_cpu(gpt->num_partition_entries) && i < state->limit-1; i++) { /* num_partition_entries(보통 128)만큼 순회, state->limit은 커널 파티션 개수 한계; GPT는 MBR extended partition chain이 없어 재귀적 NVMe Read가 불필요 */
		struct partition_meta_info *info; /* 파티션 메타정보 저장 포인터 */
		unsigned label_max; /* 볼륨 이름 최대 길이 */
		/* gpt_entry.starting_lba/ending_lba: NVMe Read/Write의 SLBA 범위 */
		u64 start = le64_to_cpu(ptes[i].starting_lba); /* 오프셋 0x20: 파티션 시작 LBA, NVMe Read/Write SLBA 계산 기준 */
		u64 size = le64_to_cpu(ptes[i].ending_lba) - /* 오프셋 0x28: 파티션 크기(LBA 개수), ending_lba 포함이므로 +1 */
			   le64_to_cpu(ptes[i].starting_lba) + 1ULL;

		if (!is_pte_valid(&ptes[i], last_lba(state->disk))) /* NVMe namespace 범위 밖 또는 unused GUID 파티션 스킵 */
			continue;

		/* start/size를 커널 섹터 단위로 변환하여 blk_mq 파티션 등록 */
		put_partition(state, i+1, start * ssz, size * ssz); /* blk_mq 파티션 등록: partno=i+1, start/size를 커널 섹터 단위로 변환 -> gendisk partition table에 추가 -> /dev/nvmeXnYpZ 생성 (추정) */

		/* If this is a RAID volume, tell md */
		/* gpt_entry.partition_type_guid: Linux RAID GUID 확인 */
		if (!efi_guidcmp(ptes[i].partition_type_guid, PARTITION_LINUX_RAID_GUID)) /* 오프셋 0x00: Linux RAID GUID면 md 스캔 표시, NVMe 위에 md/raid 구축 시 사용 */
			state->parts[i + 1].flags = ADDPART_FLAG_RAID; /* RAID 파티션 플래그 설정 */

		/* parsed_partitions.parts[]: 파티션 메타정보 저장 */
		info = &state->parts[i + 1].info; /* 파티션 메타정보 포인터 획득 */
		/* gpt_entry.unique_partition_guid: 파티션 UUID로 노출 */
		efi_guid_to_str(&ptes[i].unique_partition_guid, info->uuid); /* 오프셋 0x10: 파티션 unique GUID를 문자열 UUID로 변환, /dev/disk/by-partuuid 심볼릭 링크 생성 기반 (추정) */

		/* Naively convert UTF16-LE to 7 bits. */
		/* gpt_entry.partition_name: UTF-16LE 볼륨 이름을 7비트 ASCII로 변환 */
		label_max = min(ARRAY_SIZE(info->volname) - 1, /* 볼륨 이름 최대 길이 제한 */
				ARRAY_SIZE(ptes[i].partition_name));
		utf16_le_to_7bit(ptes[i].partition_name, label_max, info->volname); /* 오프셋 0x38: UTF-16LE partition_name을 volname으로 변환, /dev/disk/by-label 노출 기반 (추정) */
		state->parts[i + 1].has_info = true; /* 메타정보 유효 표시 */
	}
	kfree(ptes); /* 파티션 등록 완료 후 PTE 메모리 해제 */
	kfree(gpt); /* 파티션 등록 완료 후 GPT 헤더 메모리 해제 */
	seq_buf_puts(&state->pp_buf, "\n");
	return 1;
}

/* NVMe 관점 핵심 요약
 * - 이 파일은 NVMe SSD namespace의 LBA 공간을 GPT 파티션으로 분할하고,
 *   각 파티션의 start/size를 blk_mq 구조에 등록하는 관문이다.
 * - read_lba()는 GPT LBA(512바이트 기반)를 커널 섹터 번호로 변환한 뒤
 *   read_part_sector()를 통해 NVMe Read 명령으로 디스크를 읽는다. (추정)
 * - find_valid_gpt()에서 primary/alternate GPT 헤더와 PTE의 CRC32를 검증하며,
 *   NVMe 메타데이터 무결성 문제를 조기에 감지한다.
 * - efi_partition()은 check.c -> msdos_partition() 다음에 호출되어,
 *   protective MBR을 가진 NVMe 디스크의 GPT 파티션을 최종 등록한다.
 * - 등록된 파티션은 이후 submit_bio -> ... -> nvme_queue_rq 경로에서
 *   NVMe SQ/CQ 명령의 SLBA 기준으로 변환되어 doorbell을 통해 SSD로 전달된다.
 */
