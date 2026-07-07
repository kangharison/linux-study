// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * [한국어 설명] EFI GUID Partition Table(GPT) 파서 (efi.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 MBR(Master Boot Record, 32비트 LBA·4개 primary 파티션 한계를
 * 가진 구식 파티션 스킴)을 대체하는 EFI/UEFI 표준 GPT를 파싱해, 검출한
 * 파티션들을 커널의 parsed_partitions 상태에 등록하는 역할을 한다. GPT는
 * 디스크 맨 앞(LBA 1)에 primary 헤더를, 디스크 맨 끝 LBA에 backup(alternate)
 * 헤더를 이중으로 유지하고 각 헤더 자신과 파티션 엔트리 배열(PTE, Partition
 * Table Entry array)에 대해 CRC32 체크섬을 기록해 둔다. 이 파일이 없으면
 * 커널은 GPT로 분할된 디스크 - 오늘날 대부분의 NVMe SSD, SATA/SAS 디스크,
 * 가상 디스크 이미지가 여기 해당한다 - 의 개별 파티션을 전혀 인식하지 못하고
 * 디스크 전체를 파티션 없는 단일 블록 디바이스로만 다루게 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * gendisk가 등록되거나(add_disk()) 재스캔 ioctl(BLKRRPART 등)이 발생하면
 * block/partitions/core.c의 static 함수 check_partition()이 check.h가
 * 선언한 포맷별 프로버 배열을 순서대로 함수 포인터로 호출하는데, 이 파일의
 * efi_partition()이 그 프로버 중 하나로 등록되어 있다. 즉 호출 체인은
 * bdev_disk_changed() -> blk_add_partitions() -> check_partition() ->
 * [efi_partition()] 이다. efi_partition()이 1을 반환하면 GPT로 확정되고,
 * 0을 반환하면 다음 프로버(msdos_partition 등)로 넘어간다. 이 코드는 디스크
 * 마운트/파일시스템 I/O가 시작되기 이전, 디스크 등록/스캔 시점에 단 한
 * 번(혹은 재스캔 시마다) 프로세스 컨텍스트에서 동기적으로 실행되며, 인터럽트
 * 컨텍스트나 GPU 커널과는 무관하다. 파티션 스캔이 끝나면 이 파일이 채워
 * 넣은 parts[] 정보를 blk_add_partitions()가 읽어 실제 block_device(예:
 * /dev/nvme0n1p1, /dev/sda1)를 생성한다.
 *
 * === 타 모듈과의 연결 ===
 * - check.h/check.c: struct parsed_partitions(스캔 세션 상태), 섹터를
 *   안전하게 읽고 반납하는 read_part_sector()/put_dev_sector(), 검출한
 *   파티션 하나를 상태에 기록하는 put_partition()을 제공한다. 이 파일의
 *   모든 디스크 접근과 파티션 등록은 이 공용 API를 통해서만 이뤄진다.
 * - efi.h: gpt_header, gpt_entry, legacy_mbr, gpt_mbr_record 등 GPT/MBR
 *   온디스크 레이아웃 구조체와 GPT_HEADER_SIGNATURE, GPT_PRIMARY_PARTITION_
 *   TABLE_LBA, EFI_PMBR_OSTYPE_EFI_GPT, GPT_MBR_PROTECTIVE/HYBRID 등 매직
 *   상수를 정의한다. 이 파일의 모든 오프셋/필드 접근은 그 정의를 그대로
 *   사용하며, 각 구조체 필드의 설정자/읽는 자/값 범위는 efi.h 자체의
 *   필드별 주석에 상세히 기술되어 있다.
 * - lib/crc32.c(linux/crc32.h): crc32()로 GPT 헤더와 PTE 배열의 CRC32
 *   무결성 검증에 쓰이는 CRC 다항식 연산을 제공한다.
 * - 데이터 흐름: 디스크(LBA 0의 protective/hybrid MBR, LBA 1의 primary GPT,
 *   마지막 LBA의 alternate GPT) -> read_lba()가 read_part_sector()를 통해
 *   섹터를 페이지 캐시(또는 하위 블록 드라이버의 submit_bio 경로)로 읽음
 *   -> 커널 메모리의 gpt_header/gpt_entry 배열 -> is_gpt_valid()/
 *   compare_gpts()가 CRC32와 LBA 범위를 검증 -> efi_partition()이
 *   put_partition()으로 parsed_partitions.parts[]에 파티션 시작/크기를
 *   적재 -> 호출자(check.c)가 이를 실제 파티션 block_device로 반영한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - efi_partition(): 이 파일의 유일한 외부 진입점. find_valid_gpt()로
 *   유효한 GPT 헤더+PTE를 얻고, 각 파티션 엔트리를 순회하며
 *   put_partition()으로 커널에 등록한다.
 * - find_valid_gpt(): protective MBR 검사 -> primary GPT 검증 -> 실패 시
 *   alternate(backup) GPT로 폴백하는, GPT 이중화 복구 로직의 핵심 함수.
 * - is_gpt_valid(): 헤더 하나(primary 또는 alternate)와 그 PTE 배열에 대해
 *   시그니처/헤더 크기/my_lba/usable LBA 범위/헤더 CRC32/PTE 배열 CRC32를
 *   모두 검사하는, 이 파일에서 가장 핵심적인 무결성 검증 함수.
 * - compare_gpts(): 유효하다고 판정된 primary/alternate 헤더 두 개가 서로
 *   내용상 일치하는지 교차 검증하고, 불일치 시 경고만 출력한다(치명적
 *   오류로 취급해 스캔을 중단시키지는 않음).
 * - alloc_read_gpt_header()/alloc_read_gpt_entries()/read_lba(): GPT
 *   헤더 한 블록, PTE 배열 전체, 임의 바이트 범위를 각각 디스크에서 읽어오는
 *   하위 I/O 계층 함수들.
 * - is_pmbr_valid()/pmbr_part_valid(): LBA 0의 protective/hybrid MBR이
 *   유효한지 검사(GPT 앞에 반드시 존재해야 하는 관문).
 * - is_pte_valid(): 개별 PTE(파티션 엔트리) 하나가 사용 중이며 usable LBA
 *   범위 내에 있는지 검사.
 * - utf16_le_to_7bit(): GPT 파티션 이름(partition_name, UTF-16LE 저장)을
 *   커널이 다루는 7비트 ASCII 볼륨 라벨 문자열로 단순 변환.
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

/**
 * force_gpt_fn() - 커널 커맨드라인 'gpt' 옵션 파서
 * @str: __setup()이 전달하는 옵션 뒤의 잔여 문자열(이 옵션은 인자를 받지
 *       않으므로 실제로는 사용하지 않음).
 * @return: 항상 1(=이 옵션을 성공적으로 소비했음을 커널 부트 파라미터
 *          파서에 알림). 0을 반환하면 커널이 이 옵션을 처리되지 않은
 *          것으로 간주해 "Unknown kernel command line" 경고를 낸다.
 *
 * 커맨드라인에 'gpt'가 있으면 force_gpt 플래그를 1로 세팅해, 이후
 * find_valid_gpt()가 protective MBR 검사를 실패하더라도 alternate(backup)
 * GPT를 강제로 탐색하도록 만든다. 이는 손상되었거나 크기를 잘못 보고하는
 * 디바이스에서 alternate GPT라도 사용하고 싶을 때 사용자가 명시적으로
 * 선택하는 탈출구다.
 * 실행 컨텍스트: 커널 부트 파라미터 파싱 단계(__setup 매크로가 등록한
 * 콜백)에서 단 한 번 호출되며, 이후에는 재호출되지 않으므로 동시성 문제가
 * 없다.
 * 호출자: 커널 부트 파라미터 파서(do_early_param() 등, __setup 매크로가
 * 생성한 테이블을 통해 매칭된 콜백으로 호출).
 * 피호출자: 없음(전역 변수 force_gpt만 대입).
 * 에러 경로: 없음(항상 성공).
 *
 * 호출 체인:
 *   커널 부트 파라미터 파서(do_early_param) → [force_gpt_fn()] → (없음, force_gpt 전역 변수만 설정)
 */
static int __init
force_gpt_fn(char *str)
{
	force_gpt = 1;	/* PMBR 검사 강제 통과 */ /* 'gpt' 옵션 시 PMBR 없이 GPT 스캔, NVMe 가상 디스크 대응 (추정) */
	return 1; /* [한국어] __setup 콜백 규약: 1을 반환해 이 커맨드라인 토큰을 정상 소비했음을 알린다. */
}
__setup("gpt", force_gpt_fn); /* [한국어] "gpt" 토큰을 부트 파라미터로 등록: 커널 커맨드라인에 "gpt"가 있으면 force_gpt_fn()을 호출하도록 부트 파라미터 테이블에 등재한다. */


/**
 * efi_crc32() - EFI 버전의 crc32 함수
 * @buf: crc32를 계산할 버퍼. is_gpt_valid()가 호출할 때는 gpt_header 또는
 *       gpt_entry 배열이 위치한 커널 메모리를 가리킨다.
 * @len: 버퍼 길이(바이트). 헤더 CRC 계산 시에는 header_size, PTE 배열 CRC
 *       계산 시에는 num_partition_entries * sizeof_partition_entry.
 * @return: EFI 스타일 CRC32 값(표준 CRC32 결과에 대해 추가 XOR ~0을 적용한
 *          최종 값). 호출자는 이 값을 디스크에 기록된 CRC 필드와 그대로
 *          비교한다.
 *
 * GPT 헤더와 파티션 엔트리 배열의 무결성을 검증하는 CRC32를 계산한다.
 * NVMe SSD는 PRP/SGL로 전달된 원본 데이터와 무관하게, 디스크에 기록된
 * GPT 메타데이터의 CRC가 정확해야 이 헤더를 신뢰할 수 있다.
 * Ethernet 다항식에 ~0 시드, 최종 ~0 XOR 방식을 사용한다.
 * 실행 컨텍스트: is_gpt_valid() 호출 도중, 디스크 스캔 프로세스 컨텍스트에서
 * 동기적으로 실행되며 별도 동기화가 필요 없다(순수 계산 함수, 부작용 없음).
 * 에러 경로: 없음(항상 값을 계산해 반환, 실패 개념이 없음). CRC 불일치
 * 판정은 호출자인 is_gpt_valid()가 담당한다.
 *
 * 호출 체인:
 *   is_gpt_valid() → [efi_crc32()] → crc32() (lib/crc32.c, 표준 CRC32 테이블 기반 구현)
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
 * @disk: 블록 디바이스(gendisk). state->disk를 통해 전달되며, 디스크의
 *        전체 용량(part0)과 큐의 논리 블록 크기를 담고 있다.
 * @return: 디스크의 마지막 유효 LBA(0-base 인덱스). 디스크가 비어 있거나
 *          크기가 0이면 이론상 언더플로가 발생할 수 있으나(bdev_nr_bytes가
 *          0인 극단 케이스), 실제 등록된 gendisk에서는 발생하지 않는다.
 *
 * NVMe namespace의 전체 용량에서 논리 블록 크기(queue_logical_block_size)로
 * 나누어 최종 LBA를 계산한다. NVMe Identify Namespace의 NN(총 namespace 크기)와
 * 유사한 개념이며, GPT first/last_usable_lba 검증의 기준점이 된다.
 * Returns: 마지막 LBA, 오류 시 0.
 * 실행 컨텍스트: 디스크 파티션 스캔 프로세스 컨텍스트, 부작용 없는 순수
 * 계산 함수라 재진입/동시 호출에도 안전하다.
 * 호출자: read_lba()(요청 LBA가 디스크 범위 내인지), is_gpt_valid()
 * (first/last_usable_lba 상한 검증), find_valid_gpt()(alternate GPT
 * 위치 계산), efi_partition()(is_pte_valid() 호출 시 상한 전달).
 * 피호출자: bdev_nr_bytes(), queue_logical_block_size(), div_u64().
 *
 * 호출 체인:
 *   read_lba()/is_gpt_valid()/find_valid_gpt()/efi_partition() → [last_lba()] → bdev_nr_bytes(), queue_logical_block_size(), div_u64()
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
 * @part: 검사할 legacy_mbr.partition_record[] 배열의 원소 하나(MBR 파티션
 *        레코드). is_pmbr_valid()가 4개 레코드를 순회하며 각각 이 함수로
 *        전달한다.
 * @return: GPT_MBR_PROTECTIVE(이 레코드가 GPT 보호 엔트리로 확인됨) 또는
 *          0(이 레코드는 GPT 보호 엔트리가 아님 - 레거시 파티션이거나 미사용).
 *
 * NVMe SSD의 첫 512바이트(LBA 0)는 legacy MBR 또는 protective MBR을 담는다.
 * os_type이 0xEE이고 starting_lba가 1이면 GPT protective MBR로 인식한다.
 * 두 조건(os_type, starting_lba) 중 하나라도 어긋나면 이 레코드는 GPT와
 * 무관한 레거시 파티션 레코드로 간주하고 invalid 라벨로 점프한다.
 * 실행 컨텍스트: is_pmbr_valid()의 순회 루프 내부, 디스크 스캔 프로세스
 * 컨텍스트에서 동기 실행되며 부작용이 없어 재진입에 안전하다.
 * 에러 경로: 없음(단순 판정 함수, 실패해도 예외 없이 0을 반환).
 *
 * 호출 체인:
 *   is_pmbr_valid() → [pmbr_part_valid()] → (없음, 필드 비교만 수행)
 */
static inline int pmbr_part_valid(gpt_mbr_record *part)
{
	if (part->os_type != EFI_PMBR_OSTYPE_EFI_GPT) /* MBR partition_record[4] 중 os_type 0xEE가 GPT protective partition */
		goto invalid; /* [한국어] os_type이 0xEE가 아니면 이 레코드는 GPT 보호 엔트리가 아니므로 즉시 무효 판정 */

	/* GPT protective MBR의 starting_lba는 GPT Partition Header가 위치한 LBA 1을 가리킨다. */
	/* set to 0x00000001 (i.e., the LBA of the GPT Partition Header) */
	if (le32_to_cpu(part->starting_lba) != GPT_PRIMARY_PARTITION_TABLE_LBA) /* starting_lba=1이면 LBA 1에 GPT 헤더가 있음을 의미, NVMe Read SLBA=1 준비 */
		goto invalid; /* [한국어] os_type은 0xEE였지만 starting_lba가 1이 아니면 GPT 보호 엔트리 스펙에 어긋나므로 무효 처리 */

	return GPT_MBR_PROTECTIVE; /* [한국어] 두 조건을 모두 만족: 이 레코드는 GPT를 보호하는 더미 파티션임이 확정 */
invalid:
	return 0; /* [한국어] GPT 보호 엔트리가 아님을 호출자(is_pmbr_valid)에 알림 */
}

/*
 * [한국어]
 * is_pmbr_valid() - NVMe LBA 0의 보호 MBR(pMBR) 또는 하이브리드 MBR 검사
 * @mbr: read_lba(state, 0, ...)로 미리 읽어둔 LBA 0 버퍼를 legacy_mbr로
 *       캐스팅한 포인터(find_valid_gpt()가 kzalloc으로 할당 후 채움).
 * @total_sectors: get_capacity(state->disk)로 얻은 디스크 전체 512바이트
 *       섹터 수(논리 블록 크기가 아니라 커널 표준 섹터 단위).
 * @return: 0(무효한 MBR - GPT 없음), GPT_MBR_PROTECTIVE(순수 보호용 MBR),
 *          GPT_MBR_HYBRID(레거시 파티션이 공존하는 하이브리드 MBR).
 *
 * NVMe SSD는 첫 섹터에 legacy MBR 대신 protective MBR(0xEE)을 배치해
 * GPT 레이아웃임을 알린다. 이 함수는 check.c -> efi_partition() 호출 전
 * (또는 그 내부에서) GPT 존재 여부를 판단하는 관문 역할을 한다.
 * 동작 단계: (1) MSDOS_MBR_SIGNATURE(0xAA55) 확인 (2) 4개
 * partition_record를 순회하며 pmbr_part_valid()로 GPT 보호 엔트리 탐색
 * (3) 보호 엔트리를 찾으면 나머지 레코드로 하이브리드 여부 판정 (4)
 * protective MBR인 경우 size_in_lba가 디스크 실제 크기와 (대략) 일치하는지
 * 관용적으로 확인(불일치는 경고만, 실패로 취급하지 않음).
 * 실행 컨텍스트: find_valid_gpt() 내부, 디스크 스캔 프로세스 컨텍스트.
 * 에러 경로: !mbr이거나 시그니처 불일치, 혹은 protective 레코드를 전혀
 * 찾지 못하면 done 라벨로 점프해 ret=0(무효)을 반환한다.
 *
 * 호출 체인:
 *   find_valid_gpt() → [is_pmbr_valid()] → pmbr_part_valid()
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
		goto done; /* [한국어] mbr 포인터가 NULL이거나(할당 실패) 시그니처 불일치 시 ret=0(초기값) 그대로 즉시 반환 - 4개 레코드는 들여다보지도 않는다 */

	/* 4개 primary 파티션 중 GPT 보호 파티션(0xEE) 탐색 */
	for (i = 0; i < 4; i++) { /* MBR primary partition record 4개 순회, 각 record는 16바이트 (boot|chs_start|os_type|chs_end|starting_lba|size_in_lba) */
		ret = pmbr_part_valid(&mbr->partition_record[i]); /* partition_record[i] 필드 오프셋: boot(1) | chs_start(3) | os_type(1) | chs_end(3) | starting_lba(4) | size_in_lba(4) */
		if (ret == GPT_MBR_PROTECTIVE) { /* protective MBR 발견 시 hybrid MBR 여부 추가 검사 */
			part = i; /* [한국어] 보호 엔트리를 찾은 레코드의 인덱스를 기억해 둠 - 뒤에서 size_in_lba 검사 시 mbr->partition_record[part]로 재사용 */
			/*
			 * Ok, we at least know that there's a protective MBR,
			 * now check if there are other partition types for
			 * hybrid MBR.
			 */
			goto check_hybrid;
		}
	}

	if (ret != GPT_MBR_PROTECTIVE) /* [한국어] 4개 레코드를 모두 순회했지만 GPT 보호 엔트리(0xEE)를 하나도 찾지 못함 - ret은 여전히 초기값 0이므로 즉시 무효 판정 */
		goto done; /* [한국어] protective MBR 자체가 없으므로 hybrid 여부 검사도 건너뛰고 done으로: 이 디스크는 순수 레거시 MBR이거나 파티션이 없는 것으로 처리 */
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
done: /* [한국어] 성공/실패 공통 반환 지점: ret에는 0(무효), GPT_MBR_PROTECTIVE, GPT_MBR_HYBRID 중 하나가 담겨 있다 */
	return ret; /* [한국어] find_valid_gpt()의 good_pmbr에 대입되어, 0이면 GPT 스캔 전체를 중단(fail)시키는 기준이 된다 */
}

/**
 * read_lba() - 지정한 LBA부터 디스크 바이트를 읽음
 * @state: 파싱 중인 디스크 상태(struct parsed_partitions). state->disk를
 *         통해 대상 gendisk와 큐 정보에 접근한다.
 * @lba: GPT 관점의 512바이트 기반 논리 블록 주소(GPT 스펙은 항상 512바이트
 *       LBA를 기준으로 하며, 디스크의 실제 논리 블록 크기와는 별개 개념).
 * @buffer: 읽은 바이트를 채워 넣을 대상 버퍼(호출자가 미리 할당).
 * @count: 읽을 바이트 수.
 * @return: 실제로 읽은 바이트 수(성공 시 count와 같음). 버퍼가 NULL이거나
 *          lba가 디스크 범위를 벗어나면 0. 디스크 읽기 도중 실패하면
 *          그때까지 누적된(count보다 작은) 바이트 수를 반환하며, 호출자는
 *          "< count"로 부족분을 판정한다.
 *
 * GPT의 모든 LBA는 512바이트 단위이지만, NVMe SSD는 queue_logical_block_size가
 * 512/4096 등으로 포맷될 수 있다. 따라서 lba에 (lblk_size/512)를 곱해
 * read_part_sector()용 커널 섹터 번호 n으로 변환한다.
 * 이 요청은 이후 submit_bio -> blk_mq_submit_bio -> ... -> nvme_queue_rq 경로로
 * NVMe Read 명령(CID 할당, PRP/SGL 생성, doorbell 갱신)으로 변환된다. (추정)
 * Returns: 성공 시 읽은 바이트 수, 실패 시 0.
 * 동작 단계: (1) GPT LBA를 커널 섹터 번호로 환산 (2) count가 남아있는 동안
 * 512바이트씩 read_part_sector()로 섹터를 읽어 buffer에 memcpy (3) 섹터를
 * put_dev_sector()로 반납 (4) 다음 섹터로 n을 증가시키며 반복.
 * 실행 컨텍스트: 디스크 스캔 프로세스 컨텍스트. read_part_sector()가 내부적으로
 * 블록 계층 I/O(동기 읽기)를 수행하므로 이 함수 호출 동안 블로킹될 수 있다.
 * 에러 경로: read_part_sector()가 NULL을 반환하면(I/O 실패) 루프를 즉시
 * break하고 그때까지의 totalreadcount만 반환 - 호출자가 count와 비교해
 * 실패를 감지한다.
 *
 * 호출 체인:
 *   alloc_read_gpt_header()/alloc_read_gpt_entries()/find_valid_gpt() → [read_lba()] → last_lba(), read_part_sector(), put_dev_sector()
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
                return 0; /* [한국어] 인자 검증 실패: 아무 것도 읽지 않았으므로 0 반환, 호출자는 count와 비교해 실패로 판정 */

	while (count) { /* 512바이트씩 read_part_sector 호출 -> NVMe Read 명령이 count/512 만큼 반복 제출 (추정) */
		int copied = 512; /* GPT 메타데이터는 512바이트 단위로 처리, NVMe PRP/SGL entry 단위(4K 정렬)와 다를 수 있음 */
		Sector sect; /* [한국어] read_part_sector()가 내부적으로 페이지 캐시 페이지를 가리키도록 채워주는 핸들 - put_dev_sector()로 반드시 짝 맞춰 반납해야 페이지 참조 카운트가 새지 않는다 */
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
 * @gpt: 아직 CRC/범위 검증 전이지만 header_size/시그니처는 확인된 GPT
 *       헤더(alloc_read_gpt_header()가 읽어온 결과). num_partition_entries와
 *       sizeof_partition_entry, partition_entry_lba 필드를 읽어 PTE 배열의
 *       위치와 크기를 결정하는 데 사용한다.
 * @return: 새로 kmalloc()된 PTE 배열 포인터(성공), 또는 NULL(gpt가 NULL,
 *          엔트리 개수가 0, 메모리 할당 실패, read_lba() 읽기 실패 중 하나).
 *          호출자(is_gpt_valid())가 실패 시 별도 정리 없이 그대로 fail
 *          경로로 넘어가도록, 이 함수 자신이 실패 시 이미 할당한 메모리를
 *          모두 해제한 뒤 NULL을 반환한다(메모리 누수 없음).
 *
 * gpt->num_partition_entries와 gpt->sizeof_partition_entry를 곱해 PTE 배열
 * 전체 크기를 결정하고, gpt->partition_entry_lba부터 read_lba()로 읽어온다.
 * NVMe SSD에서 PTE는 일반적으로 LBA 2부터 연속 배치되며, 각 엔트리는
 * starting_lba/ending_lba를 포함해 NVMe Read/Write의 SLBA 입력값과 직결된다.
 * 실행 컨텍스트: is_gpt_valid() 내부에서, 헤더 CRC 검증 성공 직후 호출된다.
 * 에러 경로: gpt==NULL(즉시 NULL 반환), count==0(엔트리 없음, NULL 반환),
 * kmalloc 실패(NULL 반환), read_lba()가 count보다 적게 읽으면 방금 할당한
 * pte를 kfree하고 NULL 반환.
 *
 * 호출 체인:
 *   is_gpt_valid() → [alloc_read_gpt_entries()] → read_lba(), kmalloc(), kfree()
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
	gpt_entry *pte; /* [한국어] kmalloc으로 새로 할당해 디스크에서 읽어들일 PTE 배열의 시작 포인터. 성공 시 이 값을 그대로 반환, 실패 시 NULL로 재설정 후 반환 */

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
 * @return: 새로 kmalloc()된 gpt_header 포인터(성공, 논리 블록 크기만큼
 *          할당되어 헤더 이후 여분 바이트도 포함), 또는 NULL(메모리 할당
 *          실패 또는 read_lba()가 요청 크기만큼 읽지 못함). 아직 시그니처/
 *          CRC 등은 검증되지 않은 "날 것" 상태이며, 검증은 호출자
 *          is_gpt_valid()의 몫이다.
 *
 * NVMe SSD의 LBA 1(primary) 또는 마지막 LBA(alternate)에서 GPT 헤더를 읽는다.
 * 할당 크기는 queue_logical_block_size이며, 헤더는 항상 하나의 논리 블록에
 * 들어간다고 가정한다. (추정)
 * 에러 경로: kmalloc 실패 시 즉시 NULL, read_lba()가 ssz보다 적게 읽으면
 * 방금 할당한 gpt를 kfree하고 NULL 반환(메모리 누수 방지).
 *
 * 호출 체인:
 *   is_gpt_valid() → [alloc_read_gpt_header()] → queue_logical_block_size(), kmalloc(), read_lba(), kfree()
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
	gpt_header *gpt; /* [한국어] 이 함수가 새로 할당해 디스크 내용을 채워 넣을 GPT 헤더 버퍼 포인터. 성공 시 그대로 반환, 실패 시 NULL로 재설정 */
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
 * @lba: 검사할 GPT 헤더의 LBA(primary=1, alternate=lastlba, 혹은 드라이버가
 *       제공하는 alternative_gpt_sector 값).
 * @gpt: 반환용 GPT 헤더 이중 포인터. 성공 시 새로 할당된 gpt_header를
 *       가리키도록 채워지고, 실패 시 NULL로 설정된다(호출자가 이미
 *       가리키던 값은 여기서 참조되지 않고 덮어써진다).
 * @ptes: 반환용 PTE 배열 이중 포인터. 성공 시 새로 할당된 gpt_entry 배열을
 *        가리키도록 채워지고, 실패 시 NULL로 설정된다.
 * @return: 1(헤더와 PTE 배열 모두 유효), 0(둘 중 하나라도 무효 - 이 경우
 *          *gpt/*ptes는 반드시 NULL로 정리되어 호출자가 이중 해제를 하지
 *          않아도 안전하다).
 *
 * NVMe SSD에 기록된 GPT 헤더의 서명, 헤더 크기, my_lba, first/last_usable_lba,
 * 헤더 CRC32, PTE 배열 CRC32를 검증한다. 헤더가 손상되면 alternate GPT로
 * fallback하는 근거가 되며, 이는 NVMe namespace의 메타데이터 신뢰성과 직결된다.
 * 동작 단계: (1) alloc_read_gpt_header()로 헤더 읽기 (2) signature 검사
 * (3) header_size 상한/하한 검사 (4) header_crc32 재계산 비교(계산 중
 * 필드를 0으로 뒀다가 원복) (5) my_lba 일치 검사 (6) first/last_usable_lba가
 * 디스크 범위 내인지, 역전되지 않았는지 검사 (7) sizeof_partition_entry가
 * 커널의 sizeof(gpt_entry)와 일치하는지 검사 (8) PTE 배열 총 크기가
 * KMALLOC_MAX_SIZE를 넘지 않는지 검사 (9) alloc_read_gpt_entries()로 PTE
 * 배열 읽기 (10) PTE 배열 CRC32 재계산 비교. 10단계 중 하나라도 실패하면
 * 그 즉시 해당 fail 라벨로 점프해 이미 할당한 자원을 정리하고 0을 반환한다.
 * 실행 컨텍스트: find_valid_gpt() 내부, 디스크 스캔 프로세스 컨텍스트에서
 * primary/alternate 각각에 대해 최대 여러 번 호출될 수 있다.
 * 에러 경로: 두 개의 실패 라벨(fail_ptes, fail)로 구성 - PTE CRC만 실패하면
 * fail_ptes(*ptes까지 해제 후 fail로 흘러 *gpt도 해제), 그 이전 단계
 * 실패는 곧바로 fail(*gpt만 해제, *ptes는 아직 할당되지 않았으므로 대상 없음).
 *
 * 호출 체인:
 *   find_valid_gpt() → [is_gpt_valid()] → alloc_read_gpt_header(), efi_crc32(), last_lba(), alloc_read_gpt_entries()
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
		return 0; /* [한국어] 호출자 프로그래밍 오류 방어: ptes가 NULL이면 이후 *ptes 대입이 크래시를 유발하므로 아무 것도 하지 않고 즉시 실패 반환 */
	if (!(*gpt = alloc_read_gpt_header(state, lba))) /* NVMe LBA에서 GPT 헤더 읽기 실패, CQE error 또는 메모리 부족 가능 */
		return 0; /* [한국어] 헤더 자체를 읽지 못했으므로 이후 검증 단계 진입 불가, *gpt는 이미 alloc_read_gpt_header 내부에서 NULL 처리됨 */

	/* Check the GUID Partition Table signature */
	/* gpt_header.signature: "EFI PART" 시그니처(0x5452415020494645) 확인 */
	if (le64_to_cpu((*gpt)->signature) != GPT_HEADER_SIGNATURE) { /* 오프셋 0x00: signature "EFI PART"(0x5452415020494645) 확인, 잘못된 NVMe LBA 1 내용 */
		pr_debug("GUID Partition Table Header signature is wrong:"
			 "%lld != %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->signature),
			 (unsigned long long)GPT_HEADER_SIGNATURE);
		goto fail; /* [한국어] 시그니처 불일치 - 이 LBA에는 GPT 헤더가 없거나 완전히 다른 데이터임, 더 검증할 필요 없이 즉시 실패 처리로 */
	}

	/* Check the GUID Partition Table header size is too big */
	/* gpt_header.header_size: NVMe 논리 블록 크기를 초과하면 invalid */
	if (le32_to_cpu((*gpt)->header_size) > /* 오프셋 0x0C: header_size가 NVMe 논리 블록보다 큼, invalid */
			queue_logical_block_size(state->disk->queue)) {
		pr_debug("GUID Partition Table Header size is too large: %u > %u\n",
			le32_to_cpu((*gpt)->header_size),
			queue_logical_block_size(state->disk->queue));
		goto fail; /* [한국어] header_size가 한 논리 블록을 넘으면 alloc_read_gpt_header()가 읽어온 버퍼 범위를 벗어난 접근이 될 수 있으므로 실패 처리 */
	}

	/* Check the GUID Partition Table header size is too small */
	/* gpt_header.header_size: gpt_header 구조체 최소 크기보다 작으면 invalid */
	if (le32_to_cpu((*gpt)->header_size) < sizeof(gpt_header)) { /* 오프셋 0x0C: header_size가 gpt_header 구조체 최소 크기보다 작음, invalid */
		pr_debug("GUID Partition Table Header size is too small: %u < %zu\n",
			le32_to_cpu((*gpt)->header_size),
			sizeof(gpt_header));
		goto fail; /* [한국어] header_size가 gpt_header 구조체보다 작으면 필수 필드(예: partition_entry_array_crc32) 자체가 잘려 있다는 뜻이므로 실패 처리 */
	}

	/* Check the GUID Partition Table CRC */
	/* gpt_header.header_crc32: 헤더 CRC32 계산 후 비교 (계산 시 crc 필드는 0으로 둠) */
	origcrc = le32_to_cpu((*gpt)->header_crc32); /* 오프셋 0x10: 저장된 header_crc32 읽기 */
	(*gpt)->header_crc32 = 0; /* CRC 계산 시 header_crc32 필드 자신은 0으로 간주 (UEFI spec) */
	crc = efi_crc32((const unsigned char *) (*gpt), le32_to_cpu((*gpt)->header_size)); /* 헤더 전체(0 ~ header_size) CRC32 재계산, NVMe에서 읽은 원본 데이터 사용 */

	if (crc != origcrc) { /* CRC 불일치: NVMe media 손상 또는 전송 오류 (CQE status는 success였으나 데이터 무결성 깨짐) */
		pr_debug("GUID Partition Table Header CRC is wrong: %x != %x\n",
			 crc, origcrc);
		goto fail; /* [한국어] 헤더 CRC32가 어긋남 - 이 헤더가 (일부라도) 손상되었다는 강한 신호이므로 신뢰할 수 없어 실패 처리. find_valid_gpt()가 이후 alternate GPT로 폴백을 시도하는 근거가 된다 */
	}
	(*gpt)->header_crc32 = cpu_to_le32(origcrc); /* CRC 필드 원복 (fail path에서 kfree 전 복원, 디버깅용) */

	/* Check that the my_lba entry points to the LBA that contains
	 * the GUID Partition Table */
	/* gpt_header.my_lba: 현재 읽은 LBA와 일치해야 함 (primary/alternate 구분) */
	if (le64_to_cpu((*gpt)->my_lba) != lba) { /* 오프셋 0x18: my_lba는 현재 읽은 LBA와 일치해야 함 (primary vs alternate 구분) */
		pr_debug("GPT my_lba incorrect: %lld != %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->my_lba),
			 (unsigned long long)lba);
		goto fail; /* [한국어] my_lba가 실제로 읽은 위치와 다르다는 것은 헤더 내용이 잘못됐거나 primary/alternate가 뒤바뀌어 해석되었다는 뜻이므로 실패 처리 */
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
		goto fail; /* [한국어] first_usable_lba가 디스크 끝을 넘는다는 것은 헤더 값 자체가 조작/손상되었다는 뜻 - 실패 처리 */
	}
	if (le64_to_cpu((*gpt)->last_usable_lba) > lastlba) { /* 오프셋 0x30: last_usable_lba가 NVMe namespace 범위 초과 */
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->last_usable_lba),
			 (unsigned long long)lastlba);
		goto fail; /* [한국어] last_usable_lba도 동일하게 디스크 끝을 넘으면 손상된 헤더로 간주해 실패 처리 */
	}
	if (le64_to_cpu((*gpt)->last_usable_lba) < le64_to_cpu((*gpt)->first_usable_lba)) { /* usable LBA 범위 역전, NVMe namespace 레이아웃 오류 */
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->last_usable_lba),
			 (unsigned long long)le64_to_cpu((*gpt)->first_usable_lba));
		goto fail; /* [한국어] usable 범위의 끝이 시작보다 앞서는 것은 논리적으로 불가능한 상태이므로 실패 처리 */
	}
	/* Check that sizeof_partition_entry has the correct value */
	/* gpt_header.sizeof_partition_entry: 커널의 sizeof(gpt_entry)와 일치해야 함 */
	if (le32_to_cpu((*gpt)->sizeof_partition_entry) != sizeof(gpt_entry)) { /* 오프셋 0x4C: PTE 크기가 커널 gpt_entry(128바이트)와 불일치 */
		pr_debug("GUID Partition Entry Size check failed.\n");
		goto fail; /* [한국어] 디스크의 PTE 엔트리 크기가 커널이 이해하는 gpt_entry 레이아웃과 다르면 이후 배열 인덱싱이 전부 어긋나므로 실패 처리 */
	}

	/* Sanity check partition table size */
	/* gpt_header.num_partition_entries * sizeof_partition_entry: kmalloc 최대 크기 초과 금지 */
	pt_size = (u64)le32_to_cpu((*gpt)->num_partition_entries) * /* 오프셋 0x50/0x54: PTE 배열 총 크기 = num_partition_entries * sizeof_partition_entry */
		le32_to_cpu((*gpt)->sizeof_partition_entry);
	if (pt_size > KMALLOC_MAX_SIZE) { /* PTE 배열이 커널 슬랩 한계 초과, 스캔 중단 (악의적/손상된 GPT) */
		pr_debug("GUID Partition Table is too large: %llu > %lu bytes\n",
			 (unsigned long long)pt_size, KMALLOC_MAX_SIZE);
		goto fail; /* [한국어] kmalloc이 애초에 실패할 것이 뻔한 크기이므로 시도조차 하지 않고 실패 처리 - 손상되거나 악의적으로 조작된 num_partition_entries로부터 커널을 보호 */
	}

	if (!(*ptes = alloc_read_gpt_entries(state, *gpt))) /* PTE 배열 읽기 실패: NVMe LBA 연속 읽기 또는 메모리 할당 실패 */
		goto fail; /* [한국어] 이 시점에서 *gpt는 이미 CRC까지 검증된 상태지만, PTE 배열을 확보하지 못하면 헤더만으로는 쓸모가 없으므로 헤더까지 함께 실패 처리 */

	/* Check the GUID Partition Entry Array CRC */
	/* gpt_header.partition_entry_array_crc32: PTE 배열 전체 CRC32 검증 */
	crc = efi_crc32((const unsigned char *) (*ptes), pt_size); /* PTE 배열 전체 CRC32 계산, NVMe로부터 읽은 모든 파티션 엔트리 대상 */

	if (crc != le32_to_cpu((*gpt)->partition_entry_array_crc32)) { /* 오프셋 0x58: PTE CRC32 불일치, NVMe media 손상 가능 */
		pr_debug("GUID Partition Entry Array CRC check failed.\n");
		goto fail_ptes; /* [한국어] 헤더는 유효했지만 PTE 배열 자체가 손상된 경우 - fail_ptes로 점프해 *ptes부터 해제한 뒤 아래로 흘러 *gpt도 함께 해제(폴스루) */
	}

	/* We're done, all's well */
	return 1; /* [한국어] 10단계 검증을 모두 통과 - *gpt와 *ptes가 호출자(find_valid_gpt)에게 유효한 상태로 전달된다 */

 fail_ptes: /* [한국어] PTE 배열까지는 할당됐으나 그 내용이 무효로 판명된 경우의 진입점 */
	kfree(*ptes); /* 손상된 PTE 배열 해제 */
	*ptes = NULL; /* [한국어] 해제된 포인터를 NULL로 명시해 use-after-free/이중 해제 방지 */
 fail: /* [한국어] 헤더 자체가 무효였거나(위 단계들) PTE까지 해제된 뒤 도달하는 공통 실패 진입점(fail_ptes에서 폴스루) */
	kfree(*gpt); /* 손상된 헤더 해제, GPT 스캔 실패 */
	*gpt = NULL; /* [한국어] 해제된 포인터를 NULL로 명시 - 호출자가 실수로 역참조/재해제하지 않도록 보장 */
	return 0; /* [한국어] 이 lba의 헤더는 무효함을 호출자(find_valid_gpt)에 알림 - 다른 lba(alternate 등)로 재시도할지는 호출자가 결정 */
}

/**
 * is_pte_valid() - 단일 GPT 파티션 엔트리(PTE) 유효성 검사
 * @pte: 검사할 PTE(gpt_entry 배열의 원소 하나에 대한 포인터).
 * @lastlba: 디스크의 마지막 LBA(last_lba()로 계산된 값). starting_lba/
 *           ending_lba가 이 값을 넘는지 검사하는 상한 기준.
 * @return: 1(유효한 파티션 - 등록 대상), 0(무효 - 미사용 엔트리이거나 LBA
 *          범위가 디스크를 벗어남, efi_partition()이 이 엔트리를 건너뜀).
 *
 * NVMe SSD의 usable LBA 범위를 벗어나는 파티션은 무시한다. 또한 unused
 * partition_type_guid(NULL_GUID) 파티션은 커널에 등록하지 않는다.
 * 실행 컨텍스트: efi_partition()의 PTE 순회 루프 내부, 매 엔트리마다 호출.
 * 에러 경로: 없음(단순 판정 함수). 0을 반환받은 호출자가 continue로
 * 해당 엔트리를 건너뛰는 것으로 "에러 처리"를 대신한다.
 *
 * 호출 체인:
 *   efi_partition() → [is_pte_valid()] → efi_guidcmp()
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
 * @pgpt: primary GPT 헤더(NULL 가능 - find_valid_gpt()에서 primary가 무효면
 *        NULL로 전달됨).
 * @agpt: alternate GPT 헤더(NULL 가능, 위와 동일한 이유).
 * @lastlba: 디스크의 마지막 LBA(last_lba() 결과). primary의 alternate_lba와
 *           alternate의 my_lba가 실제로 이 값과 일치하는지 검사하는 기준.
 * @return: 없음(void). 검사 결과는 오직 pr_warn() 로그로만 보고되며, 반환값
 *          이나 출력 인자로 호출자에게 전달되지 않는다 - 즉 이 함수는 GPT
 *          스캔의 성공/실패를 좌우하지 않고 순수하게 진단 목적이다.
 *
 * NVMe SSD에 기록된 primary/alternate GPT 헤더의 my_lba, alternate_lba,
 * first/last_usable_lba, disk_guid, num_partition_entries, sizeof_partition_entry,
 * partition_entry_array_crc32가 일치하는지 확인한다. 불일치 시 경고를 출력하고
 * 사용자에게 GNU parted로 수정할 것을 권고한다.
 * 동작 원리: 두 헤더가 모두 유효(is_gpt_valid() 통과)하더라도 서로 다른
 * 디스크 이미지를 이어붙였거나 부분적으로만 갱신된 경우 내용이 어긋날 수
 * 있으므로, 이 함수가 8가지 필드를 하나씩 비교해 불일치 개수(error_found)를
 * 세고 마지막에 종합 경고 한 줄을 남긴다. 어떤 필드가 불일치해도 함수
 * 실행은 계속되며(치명적 오류로 취급하지 않음) 스캔 자체를 중단시키지 않는다.
 * 실행 컨텍스트: find_valid_gpt() 내부, 두 헤더 모두 확보된 이후 한 번 호출.
 * 에러 경로: pgpt 또는 agpt가 NULL이면(둘 중 하나만 유효했던 경우) 비교할
 * 대상이 없으므로 아무 것도 하지 않고 즉시 반환.
 *
 * 호출 체인:
 *   find_valid_gpt() → [compare_gpts()] → efi_guidcmp(), pr_warn()
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
	int error_found = 0; /* [한국어] 발견된 불일치 필드 개수 누적 - 0이면 마지막에 GNU Parted 권고 메시지를 생략 */
	if (!pgpt || !agpt) /* [한국어] 둘 중 하나라도 확보되지 않았으면(단일 헤더만 유효했던 경우) 비교 자체가 불가능 */
		return; /* [한국어] 비교 대상 부재로 조기 종료 - 이 경우도 에러가 아니라 정상 흐름(단일 GPT만 유효한 디스크) */
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

	if (error_found) /* [한국어] 위 8가지 검사 중 하나라도 불일치가 있었다면(error_found > 0) 종합 안내 메시지 출력 */
		pr_warn("GPT: Use GNU Parted to correct GPT errors.\n");
	return; /* [한국어] void 함수의 명시적 종료 - 스캔 성공/실패 여부에는 영향 없음(진단 전용) */
}

/**
 * find_valid_gpt() - 디스크에서 유효한 GPT 헤더와 PTE 검색
 * @state: 파싱 중인 디스크 상태
 * @gpt: 반환용 GPT 헤더 이중 포인터. 성공 시 채택된(primary 또는 alternate)
 *       헤더를 가리키며, 채택되지 않은 쪽은 이 함수 내부에서 이미 kfree됨.
 * @ptes: 반환용 PTE 배열 이중 포인터. gpt와 동일한 원칙으로 채택된 쪽만 남는다.
 * @return: 1(유효한 GPT를 찾음 - primary 또는 alternate 중 하나), 0(둘 다
 *          무효 - pMBR이 없거나 primary/alternate 모두 CRC/범위 검증 실패).
 *
 * check.c -> efi_partition() 내부에서 호출된다. (추정)
 * 먼저 LBA 0의 protective MBR을 검사하고, primary GPT(LBA 1)와 alternate GPT
 * (마지막 LBA)를 읽어 유효한 쪽을 선택한다. NVMe SSD에서 파티션 테이블을 찾지
 * 못하면 이 디스크는 NVMe I/O 요청 시에도 블록 레이블 없이 전체 namespace로
 * 다뤄지게 된다.
 * 동작 단계(GPT 이중화 복구의 핵심 로직): (1) force_gpt가 꺼져 있으면 LBA 0의
 * protective/hybrid MBR을 읽어 is_pmbr_valid()로 검사, 무효면 즉시 fail
 * (2) primary GPT(LBA 1)를 is_gpt_valid()로 검증 (3) primary가 유효하면
 * primary가 가리키는 alternate_lba 위치에서 alternate GPT도 자동 검증
 * (4) primary가 무효하고 force_gpt가 설정된 경우에만 디스크 마지막 LBA에서
 * alternate GPT를 직접 검증(사용자가 명시적으로 위험을 감수하겠다고 선택한
 * 경우로 제한 - 크기를 잘못 보고하는 손상 디바이스에 대한 안전장치)
 * (5) 그래도 실패하면 드라이버가 제공하는 alternative_gpt_sector 훅으로
 * 마지막 시도 (6) 최종적으로 하나라도 유효하면 compare_gpts()로 상호
 * 정합성을 점검(경고만, 치명적이지 않음)하고 primary를 우선 채택, 없으면
 * alternate를 채택.
 * 실행 컨텍스트: efi_partition() 내부, 디스크 스캔 프로세스 컨텍스트에서
 * 한 번 호출된다.
 * 에러 경로: 각 단계 실패는 개별 pgpt/agpt/pptes/aptes를 즉시 정리하지
 * 않고 끝까지 들고 있다가, 최종 fail 라벨에서 네 포인터를 한꺼번에
 * kfree()한다(부분 실패 시에도 안전하게 처리 - kfree(NULL)은 안전).
 *
 * 호출 체인:
 *   efi_partition() → [find_valid_gpt()] → last_lba(), is_pmbr_valid(), is_gpt_valid(), compare_gpts(), fops->alternative_gpt_sector()
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
	legacy_mbr *legacymbr; /* [한국어] LBA 0을 읽어들일 protective/hybrid MBR 버퍼 - force_gpt가 꺼져 있을 때만 할당/사용 */
	struct gendisk *disk = state->disk; /* [한국어] state가 가리키는 gendisk를 지역 변수로 캐시 - 이후 disk->fops, get_capacity(), last_lba() 등에서 반복 참조 */
	const struct block_device_operations *fops = disk->fops; /* [한국어] 드라이버별 블록 디바이스 연산 테이블 - alternative_gpt_sector 콜백이 있는지 확인하는 데 사용 */
	sector_t total_sectors = get_capacity(state->disk); /* NVMe namespace 총 512바이트 섹터 수 (queue 논리 블록 크기와 무관) */
	u64 lastlba; /* NVMe 논리 블록 기준 마지막 LBA */

	if (!ptes) /* [한국어] 호출자 프로그래밍 오류 방어: 출력 인자가 없으면 아무 것도 채울 수 없으므로 즉시 실패 */
		return 0; /* [한국어] 아직 아무 자원도 할당하지 않았으므로 정리 없이 바로 반환 가능 */

	lastlba = last_lba(state->disk); /* alternate GPT 위치 및 범위 검증 기준 */
	/* force_gpt 미설정 시 LBA 0의 protective MBR부터 검사 */
        if (!force_gpt) { /* 기본 동작: LBA 0 pMBR부터 확인, NVMe 보호 MBR 없으면 GPT 스캔 중단 */
		/* This will be added to the EFI Spec. per Intel after v1.02. */
		legacymbr = kzalloc_obj(*legacymbr); /* legacy_mbr 구조체 크기만큼 0으로 할당 (GPT protective MBR용) */
		if (!legacymbr) /* LBA 0 읽기 버퍼 할당 실패, NVMe 파티션 스캔 전체 중단 */
			goto fail; /* [한국어] 메모리 부족으로 pMBR 검사조차 시작할 수 없음 - pgpt/agpt 등은 아직 NULL이므로 fail 라벨에서 kfree(NULL)로 안전하게 정리됨 */

		/* LBA 0: legacy/protective MBR 읽기 (NVMe 첫 512바이트) */
		read_lba(state, 0, (u8 *)legacymbr, sizeof(*legacymbr)); /* LBA 0: NVMe Read로 512바이트 pMBR/legacy MBR 읽기, 실패 반환값 무시(이후 is_pmbr_valid에서 signature로 거름) */
		good_pmbr = is_pmbr_valid(legacymbr, total_sectors); /* pMBR 시그니처/보호 파티션 검증, hybrid MBR 반환 가능 */
		kfree(legacymbr); /* MBR 버퍼 해제 */

		if (!good_pmbr) /* pMBR invalid 시 GPT 스캔 중단, msdos_partition()이 legacy MBR 처리 */
			goto fail; /* [한국어] GPT의 필수 전제 조건(유효한 protective/hybrid MBR)이 없으므로 GPT일 리가 없다고 판정 - primary/alternate GPT는 시도조차 하지 않음 */

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
                goto fail; /* [한국어] 위에서 시도한 모든 경로(primary, primary가 가리키는 alternate, force_gpt의 마지막 LBA, 드라이버 힌트)가 전부 실패 - 더 이상 시도할 위치가 없으므로 최종 포기 */

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
                return 1; /* [한국어] primary가 유효하므로 이를 채택해 성공 반환 - alternate 손상 여부와 무관하게 primary만으로 충분 (단, 위 pr_warn으로 사용자에게는 알림) */
        }
        else if (good_agpt) { /* primary 손상 시 alternate GPT 사용 */
                *gpt  = agpt; /* alternate GPT 헤더 결과 반환 */
                *ptes = aptes; /* alternate PTE 배열 결과 반환 */
                kfree(pgpt); /* primary GPT 헤더 메모리 정리 */
                kfree(pptes); /* primary PTE 배열 메모리 정리 */
		pr_warn("Primary GPT is invalid, using alternate GPT.\n");
                return 1; /* [한국어] GPT 이중화 복구의 핵심 순간: primary가 무효였지만 alternate(backup)가 유효하므로 이를 채택해 성공 반환 - 이것이 GPT가 MBR 대비 갖는 핵심 내결함성(fault tolerance) */
        }

 fail: /* [한국어] primary/alternate 어느 쪽도 유효하지 않았던 최종 실패 경로 - 지금까지 부분적으로 할당됐을 수 있는 네 포인터를 모두 정리 */
        kfree(pgpt); /* [한국어] primary 헤더가 NULL이면 kfree(NULL)은 안전하게 아무 일도 하지 않음 */
        kfree(agpt); /* [한국어] alternate 헤더도 동일하게 안전 해제 */
        kfree(pptes); /* [한국어] primary PTE 배열 해제 */
        kfree(aptes); /* [한국어] alternate PTE 배열 해제 */
        *gpt = NULL; /* [한국어] 호출자(efi_partition)가 이 포인터를 그대로 kfree/역참조하지 않도록 명시적으로 NULL 확정 */
        *ptes = NULL; /* [한국어] 위와 동일한 이유로 PTE 출력 포인터도 NULL 확정 */
        return 0; /* [한국어] GPT를 찾지 못했음을 efi_partition()에 알림 - efi_partition()은 0을 그대로 자신의 반환값으로 전달(GPT 아님) */
}

/**
 * utf16_le_to_7bit() - UTF-16LE 문자열을 7비트 ASCII로 단순 변환
 * @in: 입력 UTF-16LE 문자열(gpt_entry.partition_name 필드, 리틀엔디언
 *      __le16 코드 유닛 배열). null 종료가 보장되지 않으므로 @size로
 *      길이를 별도로 받는다.
 * @size: 입력 문자열의 __le16 코드 유닛 개수(efi_partition()이
 *        min(volname 배열 크기-1, ARRAY_SIZE(partition_name))로 계산해 전달).
 * @out: 출력 버퍼(u8, ASCII). 최소 @size+1바이트가 있어야 하며(마지막 1바이트는
 *       널 종료용), efi_partition()에서는 state->parts[i+1].info.volname을 전달한다.
 * @return: 없음(void). 결과는 @out 버퍼에 직접 기록된다.
 *
 * GPT 파티션 이름(partition_name)은 UTF-16LE로 저장되어 있다. NVMe SSD는 이
 * 이름과 무관하게 I/O를 처리하지만, 커널은 이를 볼륨 레이블로 노출한다.
 * "나이브(naive)"하다는 것은 정식 유니코드 정규화나 다국어 변환 없이, 각
 * UTF-16 코드 유닛의 하위 7비트만 취해 ASCII 문자로 취급한다는 뜻이다 -
 * ASCII 범위를 벗어나는 문자(한글, 한자 등)는 원래 값과 무관한 엉뚱한
 * 7비트 값으로 잘리거나 아래 로직에 의해 '!'로 치환될 수 있다.
 * 동작 단계: (1) 출력 버퍼의 @size번째 바이트(마지막 다음 자리)를 0으로
 * 채워 널 종료 보장 (2) 각 입력 코드 유닛을 리틀엔디언 → CPU 네이티브로
 * 변환 후 하위 7비트만 추출 (3) 그 값이 0이 아니면서 출력 불가능한
 * 제어문자이면 '!'로 대체(터미널/로그에 안전하게 표시되도록) (4) 결과를
 * 출력 버퍼에 순서대로 기록.
 * 실행 컨텍스트: efi_partition()의 PTE 순회 루프 내부, 디스크 스캔 프로세스
 * 컨텍스트에서 파티션마다 한 번씩 호출.
 * 에러 경로: 없음(입력값이 무엇이든 항상 완료됨, 실패 개념이 없는 순수
 * 변환 함수).
 *
 * 호출 체인:
 *   efi_partition() → [utf16_le_to_7bit()] → le16_to_cpu(), isprint()
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
	unsigned int i = 0; /* [한국어] 입력/출력 배열을 함께 순회하는 인덱스 */

	out[size] = 0; /* [한국어] 출력 버퍼의 size번째(마지막 유효 문자 다음) 바이트를 0으로 채워 C 문자열 널 종료를 보장 - 입력 UTF-16LE에는 null 종료가 없을 수 있으므로 반드시 필요 */

	while (i < size) { /* [한국어] size개의 UTF-16LE 코드 유닛을 처음부터 끝까지 순회 */
		u8 c = le16_to_cpu(in[i]) & 0x7f; /* [한국어] 리틀엔디언 코드 유닛을 CPU 네이티브 정수로 변환 후 하위 7비트만 추출 - ASCII 범위(0x00~0x7F)로 강제 축소하는 "나이브" 변환의 핵심 */

		if (c && !isprint(c)) /* [한국어] 널 문자(c==0)는 그대로 두되, 널이 아니면서 출력 불가능한 제어 문자이면 대체 처리 */
			c = '!'; /* [한국어] 사람이 읽기 안전한 대체 문자로 치환 - 레이블에 원시 제어 문자가 섞여 터미널/로그를 오염시키는 것을 방지 */
		out[i] = c; /* [한국어] 변환/치환된 7비트 문자를 출력 버퍼의 동일 인덱스에 기록 */
		i++; /* [한국어] 다음 코드 유닛으로 진행 */
	}
}

/**
 * efi_partition() - GPT 파티션 스캔 및 커널 파티션 등록
 * @state: 파싱 중인 디스크 상태. 이 함수가 검출한 각 파티션이
 *         state->parts[]에 채워진다(put_partition()을 통해).
 * @return: 아래 함수 본문의 실제 동작 - 0(GPT가 아니거나 읽기/검증 실패,
 *          check.c가 다음 파티션 포맷 프로버로 넘어감) 또는 1(GPT 파티션을
 *          성공적으로 찾아 전부 등록함). 참고: 바로 아래 원본 커널 문서
 *          주석은 "-1 if unable to read the partition table"도 언급하지만,
 *          실제 이 함수 본문은 -1을 반환하는 경로가 없다(find_valid_gpt()
 *          실패 시에도 0을 반환) - 오래된 문서 주석이 구현 변경을 따라가지
 *          못한 사례로 보인다(추정).
 *
 * check.c에서 파티션 테이블 탐색기로 호출된다. (msdos_partition()이 LBA 0의
 * legacy MBR을 먼저 처리한 뒤, 이 함수가 protective MBR + GPT를 처리한다.)
 * find_valid_gpt()로 헤더와 PTE를 얻은 뒤, 각 파티션의 starting_lba와
 * ending_lba를 이용해 put_partition()으로 blk_mq가 인식할 수 있는 파티션
 * 범위를 등록한다. 이후 NVMe I/O는 submit_bio -> blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를
 * 거쳐 각 파티션의 SLBA에 맞는 SQ/CQ 명령으로 변환된다.
 * Returns: -1(읽기 실패), 0(해당 없음), 1(성공).
 * 동작 단계: (1) find_valid_gpt()로 유효한 GPT 헤더+PTE 확보, 실패 시
 * 자원 정리 후 0 반환 (2) num_partition_entries(및 state->limit-1) 만큼
 * PTE 배열 순회 (3) is_pte_valid()로 각 엔트리 검사, 무효면 건너뜀
 * (4) put_partition()으로 커널 파티션 테이블에 시작/크기(커널 섹터 단위로
 * 환산) 등록 (5) Linux RAID GUID면 ADDPART_FLAG_RAID 설정 (6) unique GUID를
 * 문자열 UUID로, UTF-16LE 이름을 7비트 ASCII 볼륨명으로 각각 변환해 저장
 * (7) 순회 후 GPT 헤더/PTE 메모리 해제, 성공(1) 반환.
 * 실행 컨텍스트: 디스크 등록/재스캔 시점의 커널 프로세스 컨텍스트, 이
 * 파일의 유일한 외부 진입점(비-static)이다.
 * 에러 경로: find_valid_gpt() 실패 시 0 반환(치명적 에러가 아니라 "이
 * 디스크는 GPT가 아니다"라는 정상적 판정 - check.c가 다음 포맷으로 계속
 * 시도함).
 *
 * 호출 체인:
 *   check.c(check_partition()) → [efi_partition()] → find_valid_gpt(), is_pte_valid(), put_partition(), efi_guid_to_str(), utf16_le_to_7bit()
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
		return 0; /* [한국어] "이 디스크는 GPT가 아니다"라는 정상 판정 - check.c가 msdos_partition() 등 다른 포맷 프로버로 계속 진행 */
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
			continue; /* [한국어] 이 인덱스는 파티션 번호(i+1)만 건너뛰고 다음 엔트리로 - state->parts[i+1]은 채워지지 않은 채로 남는다(파티션 번호에 구멍이 생길 수 있음, GPT 스펙상 정상) */

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
	seq_buf_puts(&state->pp_buf, "\n"); /* [한국어] /proc/partitions 등에 출력되는 파티션 요약 문자열 버퍼(pp_buf)에 개행 추가 - 다른 포맷 프로버들과 동일한 규약(각 프로버가 자신의 출력 끝에 개행을 남김) */
	return 1; /* [한국어] GPT 파티션을 성공적으로 찾아 전부 등록했음을 check.c에 알림 - 이후 다른 포맷 프로버는 시도되지 않는다 */
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
