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
 * pMBR이 아예 없거나 깨진 디스크(다른 크기 디스크로 dd 복사한 이미지 등)에서도
 * GPT 헤더만 보고 파티션을 잡고 싶을 때 쓰는 명시적 탈출구다.
 */
static int force_gpt; /* [한국어] 0이면 pMBR 검사를 통과한 디스크만 GPT로 스캔하고, 1이면 pMBR 판정과 무관하게 primary/alternate GPT를 읽어 본다. 부트 파라미터 파싱 때 한 번 쓰이고 이후에는 읽기 전용이라 별도 락이 필요 없다. */

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
	force_gpt = 1;	/* PMBR 검사 강제 통과 */ /* [한국어] 이 플래그가 서면 find_valid_gpt()는 is_pmbr_valid()가 0을 돌려줘도 중단하지 않고, primary GPT까지 깨진 경우에는 fops->alternative_gpt_sector 힌트 위치까지 추가로 시도한다. */
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
 * 디스크에서 읽어온 바이트열을 그대로 대상으로 계산하며, 그 결과가 헤더에
 * 기록된 CRC 필드와 일치해야만 그 헤더/엔트리 배열을 신뢰할 수 있다.
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
	return (crc32(~0L, buf, len) ^ ~0L); /* [한국어] 시드를 ~0으로 주고 결과를 다시 ~0과 XOR한다 - lib/crc32.c의 crc32()는 시드/최종 반전을 호출자에게 맡기므로, UEFI 사양이 규정한 CRC32(초기값 0xFFFFFFFF, 최종 보수) 형태를 맞추려면 두 반전을 여기서 직접 해줘야 한다. */
}

/**
 * last_lba() - 디바이스의 마지막 논리 블록 번호 반환
 * @disk: 블록 디바이스(gendisk). state->disk를 통해 전달되며, 디스크의
 *        전체 용량(part0)과 큐의 논리 블록 크기를 담고 있다.
 * @return: 디스크의 마지막 유효 LBA(0-base 인덱스). 디스크가 비어 있거나
 *          크기가 0이면 이론상 언더플로가 발생할 수 있으나(bdev_nr_bytes가
 *          0인 극단 케이스), 실제 등록된 gendisk에서는 발생하지 않는다.
 *
 * 디스크 전체 바이트 수를 논리 블록 크기(queue_logical_block_size)로 나눠
 * 마지막 LBA를 얻는다. GPT가 말하는 LBA는 512바이트가 아니라 "논리 블록"
 * 단위이므로, 512바이트 커널 섹터 개수(get_capacity())가 아니라 반드시 이
 * 값으로 범위를 검증해야 한다. first/last_usable_lba와 각 PTE의
 * starting/ending_lba 상한이 모두 이 값이다.
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
	/* [한국어] 바이트 단위 용량을 논리 블록 크기로 나눠 블록 개수를 얻고 1을
	 * 빼서 0-base 마지막 LBA로 바꾼다. 64비트 나눗셈을 div_u64()로 하는 이유는
	 * 32비트 아키텍처에서 '/' 연산이 컴파일러 런타임 헬퍼를 요구해 커널에서
	 * 링크 에러가 나기 때문이다.
	 */
	return div_u64(bdev_nr_bytes(disk->part0), /* [한국어] part0는 디스크 전체를 나타내는 block_device이므로 그 크기가 곧 디스크 총 바이트 수다. */
		       queue_logical_block_size(disk->queue)) - 1ULL; /* [한국어] 논리 블록 크기는 512가 아닐 수 있다(4Kn 포맷이면 4096). GPT LBA가 이 단위이므로 여기서 나눠야 하며, -1은 "개수 -> 마지막 인덱스" 변환이다. */
}

/**
 * pmbr_part_valid() - 한 개의 PMBR 파티션 레코드가 GPT 보호 파티션인지 검사
 * @part: 검사할 legacy_mbr.partition_record[] 배열의 원소 하나(MBR 파티션
 *        레코드). is_pmbr_valid()가 4개 레코드를 순회하며 각각 이 함수로
 *        전달한다.
 * @return: GPT_MBR_PROTECTIVE(이 레코드가 GPT 보호 엔트리로 확인됨) 또는
 *          0(이 레코드는 GPT 보호 엔트리가 아님 - 레거시 파티션이거나 미사용).
 *
 * 디스크 첫 512바이트(LBA 0)에는 레거시 MBR이 놓이는데, GPT 디스크는 그
 * 자리에 "이 디스크 전체가 이미 쓰이고 있다"고 거짓말하는 더미 파티션 하나만
 * 담은 보호(protective) MBR을 둔다. GPT를 모르는 옛 도구가 디스크를 빈 것으로
 * 보고 덮어쓰는 사고를 막기 위해서다. os_type이 0xEE이고 starting_lba가 1이면
 * 그 더미 파티션으로 인식한다.
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
	if (le32_to_cpu(part->starting_lba) != GPT_PRIMARY_PARTITION_TABLE_LBA) /* [한국어] UEFI 사양은 보호 파티션의 starting_lba를 1(=primary GPT 헤더가 놓인 LBA)로 못박는다. 온디스크 값은 리틀엔디안 고정이므로 빅엔디안 호스트에서도 맞게 비교하려면 le32_to_cpu()가 반드시 필요하다. */
		goto invalid; /* [한국어] os_type은 0xEE였지만 starting_lba가 1이 아니면 GPT 보호 엔트리 스펙에 어긋나므로 무효 처리 */

	return GPT_MBR_PROTECTIVE; /* [한국어] 두 조건을 모두 만족: 이 레코드는 GPT를 보호하는 더미 파티션임이 확정 */
invalid:
	return 0; /* [한국어] GPT 보호 엔트리가 아님을 호출자(is_pmbr_valid)에 알림 */
}

/*
 * [한국어]
 * is_pmbr_valid() - LBA 0의 보호 MBR(pMBR) 또는 하이브리드 MBR 검사
 * @mbr: read_lba(state, 0, ...)로 미리 읽어둔 LBA 0 버퍼를 legacy_mbr로
 *       캐스팅한 포인터(find_valid_gpt()가 kzalloc으로 할당 후 채움).
 * @total_sectors: get_capacity(state->disk)로 얻은 디스크 전체 512바이트
 *       섹터 수(논리 블록 크기가 아니라 커널 표준 섹터 단위).
 * @return: 0(무효한 MBR - GPT 없음), GPT_MBR_PROTECTIVE(순수 보호용 MBR),
 *          GPT_MBR_HYBRID(레거시 파티션이 공존하는 하이브리드 MBR).
 *
 * GPT 디스크는 첫 섹터에 레거시 MBR 대신 0xEE 타입 하나만 담긴 보호 MBR을
 * 둔다. 이 함수는 find_valid_gpt()가 GPT 헤더를 읽기 전에 통과해야 하는
 * 관문으로, 여기서 0이 나오면(그리고 force_gpt가 꺼져 있으면) GPT 스캔
 * 자체를 시작하지 않는다.
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
	if (!mbr || le16_to_cpu(mbr->signature) != MSDOS_MBR_SIGNATURE) /* [한국어] LBA 0의 마지막 2바이트(오프셋 510)에 있는 0xAA55 부트 시그니처 확인. 디스크에는 55 AA 순으로 기록되므로 le16_to_cpu()로 호스트 바이트 순서로 바꾼 뒤 비교해야 한다. 이 검사가 없으면 아무 쓰레기 섹터나 파티션 테이블로 해석하게 된다. */
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
	/* [한국어] 보호 엔트리를 찾았어도 나머지 3개 슬롯을 다시 훑는다. 순수
	 * 보호 MBR이라면 나머지는 전부 os_type 0x00(미사용)이어야 하고, 실제
	 * 파일시스템을 가리키는 레거시 엔트리가 하나라도 남아 있으면 이는 같은
	 * 디스크를 MBR로도 GPT로도 부팅시키려고 만든 하이브리드 MBR이다. */
	for (i = 0; i < 4; i++) /* [한국어] 조기 탈출 없이 4개를 모두 보는 이유: 하이브리드 판정은 "하나라도 있으면"이므로 마지막 슬롯까지 확인해야 한다. */
		if ((mbr->partition_record[i].os_type != /* [한국어] os_type은 1바이트라 엔디안 변환이 필요 없다. 0xEE 슬롯 자신은 하이브리드 근거가 못 되므로 제외하고, */
			EFI_PMBR_OSTYPE_EFI_GPT) &&
		    (mbr->partition_record[i].os_type != 0x00)) /* [한국어] 미사용(0x00)도 제외한다. 남는 것은 실제 레거시 파티션 타입뿐이다. */
			ret = GPT_MBR_HYBRID; /* [한국어] 반환값을 HYBRID로 승격. find_valid_gpt()는 PROTECTIVE와 HYBRID를 모두 "0이 아님"으로만 취급해 GPT 스캔을 진행하므로, 이 구분은 사실상 아래 size_in_lba 관용 검사를 건너뛰는 용도다(하이브리드 MBR은 디스크 크기 규칙을 지키지 않는다). */

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
		sz = le32_to_cpu(mbr->partition_record[part].size_in_lba); /* [한국어] 보호 파티션이 주장하는 크기(리틀엔디안 32비트). MBR 필드가 32비트뿐이라 표현 가능한 최대가 2TiB이고, 그래서 그 이상 디스크에서는 애초에 정확한 값을 담을 수 없다. */
		/* [한국어] 기대값은 두 가지뿐이다: 디스크 전체(LBA 0을 제외한
		 * total_sectors-1) 또는 32비트 포화값 0xFFFFFFFF. 둘 다 아니어도
		 * 실패로 처리하지 않고 진단만 남기는데, 작은 디스크 이미지를 큰
		 * 디스크에 dd로 복사한 흔한 상황을 부팅 불능으로 만들지 않기 위한
		 * 의도적 관용이다. */
		if (sz != (uint32_t) total_sectors - 1 && sz != 0xFFFFFFFF)
			/* [한국어] 기대값 쪽은 min()으로 32비트 상한에 맞춰 찍는다.
			 * 2TiB를 넘는 디스크에서 total_sectors-1을 그대로 %u로
			 * 출력하면 잘린 값이 나와 오히려 혼란스럽기 때문이다. */
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
 * 단위 변환이 이 함수의 핵심이다. GPT가 말하는 LBA는 디바이스의 논리 블록
 * (queue_logical_block_size, 512일 수도 4096일 수도 있다) 단위인 반면,
 * 블록 계층의 read_part_sector()는 항상 512바이트 고정 섹터 번호를 받는다.
 * 그래서 lba에 (논리 블록 크기 / 512)를 곱해 커널 섹터 번호 n으로 바꾼다.
 * 4Kn 디스크라면 GPT LBA 1이 커널 섹터 8이 되는 식이다.
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
	size_t totalreadcount = 0; /* [한국어] 지금까지 buffer에 채운 바이트 수. 중간에 I/O가 실패해도 이 값을 그대로 반환하므로, 호출자는 반환값 < count 여부로 부분 실패를 감지한다. */
	/* [한국어] GPT LBA(논리 블록 단위) -> 커널 섹터(항상 512B) 번호 변환.
	 * 이 곱셈을 빠뜨리면 4Kn 디스크에서 엉뚱한 위치를 읽게 된다. */
	sector_t n = lba * /* [한국어] 논리 블록 크기가 512면 계수는 1이라 무변환, 4096이면 8배가 된다. */
		(queue_logical_block_size(state->disk->queue) / 512);

	if (!buffer || lba > last_lba(state->disk)) /* [한국어] 디스크 끝을 넘는 LBA 요청을 여기서 막는다. GPT 헤더의 alternate_lba/partition_entry_lba는 디스크가 손상되면 임의의 큰 값일 수 있으므로, 이 상한 검사가 없으면 조작된 헤더 하나로 디스크 범위 밖 읽기를 유발할 수 있다. */
                return 0; /* [한국어] 인자 검증 실패: 아무 것도 읽지 않았으므로 0 반환, 호출자는 count와 비교해 실패로 판정 */

	while (count) { /* [한국어] 남은 요청 바이트가 0이 될 때까지 한 섹터씩 진행. 페이지 캐시 헬퍼가 섹터 단위 인터페이스라 한 번에 큰 덩어리를 받을 수 없다. */
		int copied = 512; /* [한국어] 한 회차에 옮길 최대 바이트 = 커널 섹터 크기. read_part_sector()가 돌려주는 포인터가 512바이트 경계 기준이므로 그 이상을 읽으면 인접 섹터 데이터를 침범한다. */
		Sector sect; /* [한국어] read_part_sector()가 내부적으로 페이지 캐시 페이지를 가리키도록 채워주는 핸들 - put_dev_sector()로 반드시 짝 맞춰 반납해야 페이지 참조 카운트가 새지 않는다 */
		/* [한국어] 페이지 캐시를 경유하는 동기 읽기. 이 스캔은 프로세스
		 * 컨텍스트에서만 돌기 때문에 여기서 블로킹되어도 문제가 없다. */
		unsigned char *data = read_part_sector(state, n++, &sect); /* [한국어] n을 후위 증가시켜 다음 회차가 자동으로 다음 섹터를 읽게 한다. */
		if (!data) /* [한국어] I/O 실패나 메모리 부족. 여기서 에러를 위로 던지지 않고 break만 하는 이유는, 부분적으로 읽힌 양(totalreadcount)을 호출자에게 그대로 알려 판단을 맡기기 위해서다. */
			break;
		if (copied > count) /* [한국어] 마지막 회차에서 남은 요청이 512바이트 미만인 경우. 이 절삭이 없으면 호출자 버퍼 뒤로 최대 511바이트를 넘겨 쓴다. */
			copied = count; /* [한국어] 남은 요청량으로 줄여 호출자 버퍼 밖을 건드리지 않게 한다. */
		memcpy(buffer, data, copied); /* [한국어] 페이지 캐시 페이지의 내용을 호출자 버퍼로 복사. 캐시 페이지는 put_dev_sector() 이후 언제든 회수될 수 있으므로 포인터를 들고 있지 않고 즉시 복사한다. */
		put_dev_sector(sect); /* [한국어] 읽기 참조 반납. 루프 안에서 매회 반납하지 않으면 큰 PTE 배열을 읽는 동안 페이지 참조가 계속 쌓인다. */
		buffer += copied; /* [한국어] 목적지 커서 전진 */
		totalreadcount +=copied; /* [한국어] 반환할 누적량 갱신 */
		count -= copied; /* [한국어] 남은 요청량 감소. 0이 되면 while 조건이 거짓이 되어 정상 종료. */
	}
	return totalreadcount; /* [한국어] 요청 전량을 읽었으면 count와 같고, 중간 실패면 그보다 작다. 호출자들은 예외 없이 "< count"로 실패를 판정한다. */
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
 * 표준 레이아웃에서 PTE 배열은 primary 헤더 바로 뒤(LBA 2)부터 연속 배치되고
 * 기본값은 128개 x 128바이트 = 16KiB지만, 두 값 모두 헤더가 자칭하는 값이므로
 * 여기서는 곱셈 결과를 그대로 할당 크기로 쓴다. 그래서 호출자 is_gpt_valid()가
 * 이 함수를 부르기 전에 헤더 CRC를 먼저 검증해 값의 신뢰성을 확보해야 한다.
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
	size_t count; /* [한국어] PTE 배열 전체 바이트 수. 엔트리 개수 x 엔트리 크기로 계산하며, 할당 크기이자 read_lba() 요청 길이로 함께 쓰인다. */
	gpt_entry *pte; /* [한국어] kmalloc으로 새로 할당해 디스크에서 읽어들일 PTE 배열의 시작 포인터. 성공 시 이 값을 그대로 반환, 실패 시 NULL로 재설정 후 반환 */

	if (!gpt) /* [한국어] 헤더 읽기가 이미 실패한 경우. 호출자가 검사를 생략해도 여기서 걸러지도록 방어적으로 둔다. */
		return NULL; /* [한국어] 이 함수의 모든 실패는 NULL 하나로만 표현된다 - 호출자 is_gpt_valid()가 원인을 구분할 필요가 없기 때문이다. */

	/* [한국어] 두 필드 모두 온디스크 리틀엔디안 32비트다. 곱하기 전에 한쪽을
	 * (size_t)로 캐스팅하는 것이 핵심으로, 32비트 x 32비트를 32비트 안에서
	 * 곱하면 오버플로가 나 실제보다 작은 버퍼를 할당한 뒤 read_lba()가 그
	 * 뒤를 덮어쓰게 된다. 64비트 size_t로 승격시켜 그 경로를 막는다. */
	count = (size_t)le32_to_cpu(gpt->num_partition_entries) * /* [한국어] 엔트리 개수(사양 권장 최소 128) */
                le32_to_cpu(gpt->sizeof_partition_entry); /* [한국어] 엔트리 하나의 바이트 크기(사양상 128의 배수). 호출자가 헤더 CRC를 이미 통과시켰고, is_gpt_valid()가 이 곱을 디스크 크기와 다시 대조한다. */
	if (!count) /* [한국어] 둘 중 하나가 0이면 읽을 것이 없다. kmalloc(0)은 NULL이 아닌 특수 포인터를 돌려주므로, 그것을 유효한 PTE 배열로 오인하지 않도록 여기서 미리 잘라낸다. */
		return NULL; /* [한국어] 엔트리가 없는 GPT는 파티션도 없다는 뜻이므로 실패로 처리한다. */
	pte = kmalloc(count, GFP_KERNEL); /* [한국어] GFP_KERNEL - 이 경로는 프로세스 컨텍스트에서만 실행되므로 슬립 가능한 할당을 써도 된다. */
	if (!pte) /* [한국어] 메모리 부족. 파티션 없이 디스크 전체만 노출되는 결과가 되지만, 스캔 실패는 시스템을 멈추는 오류가 아니므로 NULL 반환으로 끝낸다. */
		return NULL;

	/* [한국어] partition_entry_lba는 64비트라 le64_to_cpu()로 변환한다. 값이
	 * 디스크 끝을 넘더라도 read_lba()의 last_lba() 상한 검사가 0을 돌려주므로
	 * 아래 "< count" 조건에서 실패로 걸러진다. */
	if (read_lba(state, le64_to_cpu(gpt->partition_entry_lba), /* [한국어] 표준 배치라면 LBA 2 */
			(u8 *) pte, count) < count) {
		kfree(pte); /* [한국어] 부분만 읽힌 배열은 CRC 검증을 어차피 통과하지 못하므로 즉시 버린다. 여기서 해제해 두어야 호출자가 실패 시 정리 책임을 지지 않아도 된다. */
                pte=NULL; /* [한국어] 바로 다음 줄에서 NULL을 반환하므로 기능상 불필요하지만, 해제한 포인터를 지역 변수에 남기지 않는 관습을 따른 것이다. */
		return NULL; /* [한국어] 부분 읽기는 실패로 취급한다. 절반만 읽힌 배열을 넘기면 뒤쪽 엔트리가 초기화되지 않은 힙 내용이 되어, CRC 검증 이전에 이미 위험하다. */
	}
	return pte; /* [한국어] count 바이트를 온전히 채운 배열. 해제 책임은 호출자(is_gpt_valid()의 fail 경로 또는 efi_partition())에게 넘어간다. */
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
 * LBA 1(primary) 또는 마지막 LBA(alternate)에서 GPT 헤더를 읽는다. 할당/읽기
 * 단위를 sizeof(gpt_header)(92바이트)가 아니라 논리 블록 크기로 잡는 이유는,
 * read_lba()가 섹터 단위로만 복사하고 UEFI 사양도 헤더가 자신의 LBA 한 블록을
 * 통째로 차지한다고 규정하기 때문이다. 헤더 뒤 여분 바이트는 사용하지 않는다.
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
	unsigned ssz = queue_logical_block_size(state->disk->queue); /* [한국어] 논리 블록 크기 = 헤더 하나가 차지하는 온디스크 공간. 할당 크기와 읽기 길이를 같은 값으로 맞춰야 아래 "< ssz" 부족 판정이 성립한다. */

	gpt = kmalloc(ssz, GFP_KERNEL); /* [한국어] 헤더 구조체(92바이트)보다 크게 잡는다. 92바이트만 잡으면 read_lba()가 512바이트를 복사하며 힙을 넘어 쓴다. */
	if (!gpt) /* [한국어] 할당 실패 - 검증 없이 곧장 포기한다. */
		return NULL; /* [한국어] 아직 아무것도 할당·획득하지 않았으므로 정리할 것이 없다. */

	/* [한국어] 호출자가 넘긴 lba가 곧 primary(1) / alternate(마지막 LBA) /
	 * 드라이버 힌트 위치를 구분하는 유일한 인자다. 이 함수는 어느 쪽인지
	 * 신경 쓰지 않고 바이트만 읽어오며, 시그니처와 CRC 판정은 전적으로
	 * 호출자 is_gpt_valid()가 맡는다. */
	if (read_lba(state, lba, (u8 *) gpt, ssz) < ssz) { /* [한국어] 한 블록을 다 못 읽었다면 그 위치에 헤더가 없거나 매체 오류다. */
		kfree(gpt); /* [한국어] 실패 시 이 함수가 직접 해제해, 호출자가 NULL만 확인하면 되도록 만든다. */
                gpt=NULL;
		return NULL; /* [한국어] "이 LBA에는 헤더가 없다"는 신호. 호출자는 이를 곧바로 폴백 판단에 쓴다. */
	}

	return gpt; /* [한국어] 아직 검증되지 않은 날 것의 한 블록. 시그니처/CRC 판정은 전적으로 is_gpt_valid()의 몫이다. */
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
 * 디스크에서 읽어온 GPT 헤더의 서명, 헤더 크기, my_lba, first/last_usable_lba,
 * 헤더 CRC32, PTE 배열 CRC32를 검증한다. 헤더가 손상되면 alternate GPT로
 * fallback하는 근거가 된다. 이 함수가 0을 돌려주는 것은 "이 위치에 신뢰할 수
 * 있는 GPT가 없다"는 뜻일 뿐, 디스크 자체의 오류를 뜻하지는 않는다.
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
	u64 lastlba, pt_size; /* [한국어] lastlba는 디스크 마지막 LBA(범위 검사의 상한), pt_size는 PTE 배열 전체 바이트 수(할당 폭주 방지 검사와 CRC 계산 범위로 함께 쓰인다). */

	if (!ptes) /* [한국어] 출력 인자 방어. gpt는 NULL 검사를 하지 않는데, 이 함수의 유일한 호출자 find_valid_gpt()가 항상 지역 변수의 주소를 넘기기 때문이다. */
		return 0; /* [한국어] 호출자 프로그래밍 오류 방어: ptes가 NULL이면 이후 *ptes 대입이 크래시를 유발하므로 아무 것도 하지 않고 즉시 실패 반환 */
	if (!(*gpt = alloc_read_gpt_header(state, lba))) /* [한국어] 헤더 한 블록 읽기. 대입과 검사를 한 문장에 합쳐 두어, 실패 시 *gpt가 NULL로 남아 호출자가 그대로 판정할 수 있다. */
		return 0; /* [한국어] 헤더 자체를 읽지 못했으므로 이후 검증 단계 진입 불가, *gpt는 이미 alloc_read_gpt_header 내부에서 NULL 처리됨 */

	/* Check the GUID Partition Table signature */
	/* gpt_header.signature: "EFI PART" 시그니처(0x5452415020494645) 확인 */
	if (le64_to_cpu((*gpt)->signature) != GPT_HEADER_SIGNATURE) { /* [한국어] 오프셋 0x00의 8바이트 시그니처. 디스크에는 ASCII "EFI PART"(45 46 49 20 50 41 52 54)가 그 순서로 적혀 있고, 이를 리틀엔디안 64비트 정수로 읽으면 0x5452415020494645가 된다. le64_to_cpu()가 없으면 빅엔디안 호스트에서 정상 디스크를 전부 거부하게 된다. */
		pr_debug("GUID Partition Table Header signature is wrong:"
			 "%lld != %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->signature),
			 (unsigned long long)GPT_HEADER_SIGNATURE);
		goto fail; /* [한국어] 시그니처 불일치 - 이 LBA에는 GPT 헤더가 없거나 완전히 다른 데이터임, 더 검증할 필요 없이 즉시 실패 처리로 */
	}

	/* Check the GUID Partition Table header size is too big */
	/* [한국어] 상한 검사. header_size는 아래에서 CRC 계산 길이로 그대로
	 * 쓰이는데, alloc_read_gpt_header()가 잡아 둔 버퍼는 논리 블록 하나
	 * 크기뿐이다. 손상된 디스크가 header_size에 큰 값을 적어 두면 그 길이만큼
	 * 힙 바깥을 읽으며 CRC를 계산하게 되므로, 계산 전에 반드시 막아야 한다. */
	if (le32_to_cpu((*gpt)->header_size) > /* [한국어] 오프셋 0x0C, 리틀엔디안 32비트 */
			queue_logical_block_size(state->disk->queue)) {
		/* [한국어] pr_err가 아니라 pr_debug인 이유: GPT가 아닌 디스크를
		 * 스캔하다 여기 걸리는 것은 정상적인 일상 동작이라, 기본 로그
		 * 레벨에서는 조용해야 한다. */
		pr_debug("GUID Partition Table Header size is too large: %u > %u\n",
			le32_to_cpu((*gpt)->header_size),
			queue_logical_block_size(state->disk->queue));
		goto fail; /* [한국어] header_size가 한 논리 블록을 넘으면 alloc_read_gpt_header()가 읽어온 버퍼 범위를 벗어난 접근이 될 수 있으므로 실패 처리 */
	}

	/* Check the GUID Partition Table header size is too small */
	/* gpt_header.header_size: gpt_header 구조체 최소 크기보다 작으면 invalid */
	if (le32_to_cpu((*gpt)->header_size) < sizeof(gpt_header)) { /* [한국어] 하한 검사. 아래 검증들이 partition_entry_array_crc32(오프셋 0x58)까지 읽는데, header_size가 그보다 짧으면 헤더가 보증하지 않는(=CRC 범위 밖) 쓰레기 바이트를 필드로 신뢰하게 된다. */
		pr_debug("GUID Partition Table Header size is too small: %u < %zu\n",
			le32_to_cpu((*gpt)->header_size),
			sizeof(gpt_header));
		goto fail; /* [한국어] header_size가 gpt_header 구조체보다 작으면 필수 필드(예: partition_entry_array_crc32) 자체가 잘려 있다는 뜻이므로 실패 처리 */
	}

	/* Check the GUID Partition Table CRC */
	/* gpt_header.header_crc32: 헤더 CRC32 계산 후 비교 (계산 시 crc 필드는 0으로 둠) */
	origcrc = le32_to_cpu((*gpt)->header_crc32); /* [한국어] 오프셋 0x10에 저장된 기대값을 호스트 바이트 순서로 빼 둔다. 곧바로 필드를 0으로 덮을 것이므로 먼저 읽어야 한다. */
	(*gpt)->header_crc32 = 0; /* [한국어] UEFI 사양은 "CRC 필드 자신을 0으로 둔 상태의 헤더"에 대해 CRC를 계산하라고 규정한다. 자기 자신을 포함해 계산할 수는 없으므로 생기는 필연적 규칙이며, 인메모리 사본만 건드리고 디스크는 쓰지 않는다. */
	crc = efi_crc32((const unsigned char *) (*gpt), le32_to_cpu((*gpt)->header_size)); /* [한국어] CRC 범위는 헤더 맨 앞부터 header_size까지이며, 그 뒤 블록 잔여 바이트는 포함하지 않는다. 바로 위에서 상·하한을 검사해 둔 덕분에 이 길이는 항상 버퍼 안이다. */

	if (crc != origcrc) { /* [한국어] 매체 손상이나 쓰기 도중 전원 차단으로 헤더가 반만 갱신된 경우. 이 검사가 통과해야 비로소 my_lba 이하 필드를 신뢰할 수 있다. */
		pr_debug("GUID Partition Table Header CRC is wrong: %x != %x\n",
			 crc, origcrc);
		goto fail; /* [한국어] 헤더 CRC32가 어긋남 - 이 헤더가 (일부라도) 손상되었다는 강한 신호이므로 신뢰할 수 없어 실패 처리. find_valid_gpt()가 이후 alternate GPT로 폴백을 시도하는 근거가 된다 */
	}
	(*gpt)->header_crc32 = cpu_to_le32(origcrc); /* [한국어] 검증을 위해 잠시 0으로 만든 필드를 원래 값으로 되돌려, 이후 이 사본을 보는 코드가 온디스크 내용과 동일한 헤더를 보게 한다. origcrc는 이미 호스트 순서이므로 cpu_to_le32()로 다시 리틀엔디안으로 되돌린다. */

	/* Check that the my_lba entry points to the LBA that contains
	 * the GUID Partition Table */
	/* gpt_header.my_lba: 현재 읽은 LBA와 일치해야 함 (primary/alternate 구분) */
	if (le64_to_cpu((*gpt)->my_lba) != lba) { /* [한국어] 오프셋 0x18. 헤더가 자칭하는 위치와 실제로 읽어온 위치가 같아야 한다. CRC는 헤더 내용의 무결성만 보증할 뿐 "이 헤더가 여기 있어야 할 헤더인가"는 말해 주지 않으므로, 백업 헤더를 primary 자리에 복사해 둔 디스크를 걸러내려면 이 검사가 필요하다. */
		pr_debug("GPT my_lba incorrect: %lld != %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->my_lba),
			 (unsigned long long)lba);
		goto fail; /* [한국어] my_lba가 실제로 읽은 위치와 다르다는 것은 헤더 내용이 잘못됐거나 primary/alternate가 뒤바뀌어 해석되었다는 뜻이므로 실패 처리 */
	}

	/* Check the first_usable_lba and last_usable_lba are
	 * within the disk.
	 */
	/* [한국어] 여기서부터는 "헤더가 손상되지 않았다"가 아니라 "헤더가 이
	 * 디스크와 앞뒤가 맞는가"를 본다. usable 범위는 나중에 각 파티션의
	 * starting/ending_lba를 검증하는 기준이 되므로, 그 기준부터 디스크
	 * 범위 안에 있어야 한다. */
	lastlba = last_lba(state->disk); /* [한국어] 실제 디스크의 마지막 LBA. 헤더가 주장하는 값이 아니라 장치가 보고한 용량에서 계산한 값이라, 위조할 수 없는 유일한 기준점이다. */
	if (le64_to_cpu((*gpt)->first_usable_lba) > lastlba) { /* [한국어] 오프셋 0x28 */
		pr_debug("GPT: first_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->first_usable_lba),
			 (unsigned long long)lastlba);
		goto fail; /* [한국어] first_usable_lba가 디스크 끝을 넘는다는 것은 헤더 값 자체가 조작/손상되었다는 뜻 - 실패 처리 */
	}
	if (le64_to_cpu((*gpt)->last_usable_lba) > lastlba) { /* [한국어] 오프셋 0x30 */
		/* [한국어] 디스크를 줄여서 복제했을 때 전형적으로 걸리는 조건이다. */
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->last_usable_lba),
			 (unsigned long long)lastlba);
		goto fail; /* [한국어] last_usable_lba도 동일하게 디스크 끝을 넘으면 손상된 헤더로 간주해 실패 처리 */
	}
	if (le64_to_cpu((*gpt)->last_usable_lba) < le64_to_cpu((*gpt)->first_usable_lba)) { /* [한국어] 두 값이 각각 디스크 안에 있어도 순서가 뒤집혀 있으면 usable 구간의 길이가 음수가 된다. 개별 상한 검사만으로는 잡히지 않는 조합이라 따로 본다. */
		/* [한국어] 출력 형식의 ">"는 실제 조건("<")과 반대인데, 이는 위
		 * 메시지에서 복사해 온 문구 그대로다. 진단 문자열이라 동작에는
		 * 영향이 없다. */
		pr_debug("GPT: last_usable_lba incorrect: %lld > %lld\n",
			 (unsigned long long)le64_to_cpu((*gpt)->last_usable_lba),
			 (unsigned long long)le64_to_cpu((*gpt)->first_usable_lba));
		goto fail; /* [한국어] usable 범위의 끝이 시작보다 앞서는 것은 논리적으로 불가능한 상태이므로 실패 처리 */
	}
	/* Check that sizeof_partition_entry has the correct value */
	/* gpt_header.sizeof_partition_entry: 커널의 sizeof(gpt_entry)와 일치해야 함 */
	if (le32_to_cpu((*gpt)->sizeof_partition_entry) != sizeof(gpt_entry)) { /* [한국어] 오프셋 0x54. 사양은 128의 배수면 무엇이든 허용하지만, 이 파서는 배열을 gpt_entry[]로 그냥 인덱싱하므로 커널 구조체 크기(128바이트)와 정확히 같을 때만 진행한다. 다르면 두 번째 엔트리부터 전부 어긋난 위치를 읽게 된다. */
		pr_debug("GUID Partition Entry Size check failed.\n");
		goto fail; /* [한국어] 디스크의 PTE 엔트리 크기가 커널이 이해하는 gpt_entry 레이아웃과 다르면 이후 배열 인덱싱이 전부 어긋나므로 실패 처리 */
	}

	/* Sanity check partition table size */
	/* [한국어] 두 32비트 값의 곱을 (u64)로 승격해 계산한다. 32비트 안에서
	 * 곱하면 오버플로된 작은 값이 나와 아래 상한 검사를 그냥 통과해 버린다. */
	pt_size = (u64)le32_to_cpu((*gpt)->num_partition_entries) * /* [한국어] 오프셋 0x50: 엔트리 개수 */
		le32_to_cpu((*gpt)->sizeof_partition_entry); /* [한국어] 오프셋 0x54: 엔트리 크기(바로 위에서 128로 확정됨) */
	if (pt_size > KMALLOC_MAX_SIZE) { /* [한국어] 여기가 없으면 num_partition_entries에 0xFFFFFFFF를 적은 디스크 하나로 수백 GB 규모의 kmalloc을 유발할 수 있다. 실패할 할당을 시도조차 하지 않고 컷하는 것이 요점이다. */
		/* [한국어] pt_size는 u64라 %llu 앞에 캐스팅이 필요하고,
		 * KMALLOC_MAX_SIZE는 (1UL << ...) 형태의 unsigned long이라 %lu로 받는다. */
		pr_debug("GUID Partition Table is too large: %llu > %lu bytes\n",
			 (unsigned long long)pt_size, KMALLOC_MAX_SIZE);
		goto fail; /* [한국어] kmalloc이 애초에 실패할 것이 뻔한 크기이므로 시도조차 하지 않고 실패 처리 - 손상되거나 악의적으로 조작된 num_partition_entries로부터 커널을 보호 */
	}

	if (!(*ptes = alloc_read_gpt_entries(state, *gpt))) /* [한국어] 헤더 검증이 모두 끝난 뒤에야 PTE 배열을 읽는다. 순서가 중요한 이유는 배열의 위치·개수·크기를 모두 헤더 필드에서 가져오기 때문이다. */
		goto fail; /* [한국어] 이 시점에서 *gpt는 이미 CRC까지 검증된 상태지만, PTE 배열을 확보하지 못하면 헤더만으로는 쓸모가 없으므로 헤더까지 함께 실패 처리 */

	/* Check the GUID Partition Entry Array CRC */
	/* gpt_header.partition_entry_array_crc32: PTE 배열 전체 CRC32 검증 */
	crc = efi_crc32((const unsigned char *) (*ptes), pt_size); /* [한국어] 헤더 CRC와 달리 배열은 통째로(사용 중이든 빈 엔트리든 전부) 계산 대상이다. 그래서 파티션 하나만 바꿔도 이 값이 달라진다. */

	if (crc != le32_to_cpu((*gpt)->partition_entry_array_crc32)) { /* [한국어] 오프셋 0x58. 헤더와 배열은 서로 다른 LBA에 있으므로, 둘 중 한쪽만 기록된 채 전원이 끊긴 상황을 이 교차 CRC가 잡아낸다. */
		pr_debug("GUID Partition Entry Array CRC check failed.\n");
		goto fail_ptes; /* [한국어] 헤더는 유효했지만 PTE 배열 자체가 손상된 경우 - fail_ptes로 점프해 *ptes부터 해제한 뒤 아래로 흘러 *gpt도 함께 해제(폴스루) */
	}

	/* We're done, all's well */
	/* [한국어] 여기까지 왔다면 시그니처, 헤더 크기, 헤더 CRC, 자기 위치,
	 * usable 범위, 엔트리 크기, 배열 크기, 배열 CRC가 모두 통과한 것이다. */
	return 1; /* [한국어] *gpt와 *ptes의 해제 책임이 호출자 find_valid_gpt()로 넘어간다. */

 fail_ptes: /* [한국어] PTE 배열까지는 할당됐으나 그 내용이 무효로 판명된 경우의 진입점 */
	kfree(*ptes); /* [한국어] 배열만 무효인 경우의 진입점. 아래 fail로 그대로 흘러내려(폴스루) 헤더까지 함께 해제한다 - 헤더만 남겨 봐야 파티션을 읽을 수 없기 때문이다. */
	*ptes = NULL; /* [한국어] 해제된 포인터를 NULL로 명시해 use-after-free/이중 해제 방지 */
 fail: /* [한국어] 헤더 자체가 무효였거나(위 단계들) PTE까지 해제된 뒤 도달하는 공통 실패 진입점(fail_ptes에서 폴스루) */
	kfree(*gpt); /* [한국어] 이 함수는 실패 시 자신이 할당한 것을 전부 되돌린다. 덕분에 find_valid_gpt()는 primary가 실패하면 별도 정리 없이 곧바로 alternate 시도로 넘어갈 수 있다. */
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
 * 헤더가 정한 usable LBA 범위를 벗어나는 파티션은 무시한다. 또한 unused
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
	/* [한국어] 세 조건을 OR로 묶어 "쓰면 안 되는 엔트리"를 한 번에 거른다.
	 * 배열은 항상 num_partition_entries개가 통째로 존재하고 빈 슬롯이
	 * 중간에 섞일 수 있으므로, 개수가 아니라 엔트리별 판정이 필요하다. */
	if ((!efi_guidcmp(pte->partition_type_guid, NULL_GUID)) || /* [한국어] 오프셋 0x00. 타입 GUID가 전부 0이면 빈 슬롯이라는 사양상의 약속이다. efi_guidcmp()는 같을 때 0을 돌려주므로 여기서는 부정(!)이 곧 "빈 슬롯"을 뜻한다. */
	    le64_to_cpu(pte->starting_lba) > lastlba         || /* [한국어] 오프셋 0x20. 이 상한이 없으면 조작된 엔트리가 디스크 밖을 가리키는 파티션 디바이스를 만들어, 이후 그 파티션에 대한 I/O가 전부 범위 밖 요청이 된다. */
	    le64_to_cpu(pte->ending_lba)   > lastlba) /* [한국어] 오프셋 0x28. 끝 LBA도 같은 이유로 검사한다. 다만 start > end 역전은 여기서 걸러지지 않고, 호출자 efi_partition()의 size 계산에서 처리된다. */
		return 0; /* [한국어] 빈 슬롯이거나 범위를 벗어남 - 호출자는 이 엔트리를 등록하지 않고 건너뛴다. */
	return 1; /* [한국어] 사용 중이며 디스크 범위 안 - put_partition() 대상. */
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
 * 디스크에 이중으로 기록된 primary/alternate GPT 헤더의 my_lba, alternate_lba,
 * first/last_usable_lba, disk_guid, num_partition_entries, sizeof_partition_entry,
 * partition_entry_array_crc32가 서로 일치하는지 확인한다. 불일치 시 경고를
 * 출력하고 사용자에게 GNU parted로 수정할 것을 권고한다. 각 헤더는 자기
 * CRC로 자신의 무결성만 보증하므로, "둘 다 개별적으로는 멀쩡한데 내용이
 * 서로 다른" 상태(디스크를 키운 뒤 한쪽만 갱신한 경우가 대표적)는 오직 이
 * 교차 비교로만 드러난다.
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
	/* [한국어] 검사 1: 두 헤더의 상호 참조(정방향). primary가 "나는 여기
	 * 있다"고 적은 my_lba와, alternate가 "상대 헤더는 저기 있다"고 적은
	 * alternate_lba는 같은 값이어야 한다. 어긋나면 두 헤더는 애초에 서로를
	 * 짝으로 알고 있지 않다는 뜻이다(다른 디스크에서 복사해 온 이미지 등). */
	if (le64_to_cpu(pgpt->my_lba) != le64_to_cpu(agpt->alternate_lba)) {
		pr_warn("GPT:Primary header LBA != Alt. header alternate_lba\n");
		/* [한국어] u64는 64비트 아키텍처에서 unsigned long, 32비트에서는
		 * unsigned long long으로 정의되어 %lld와 타입이 어긋난다. 그래서
		 * 커널 코드는 이런 값 출력마다 (unsigned long long) 캐스팅을
		 * 명시해 아키텍처별 포맷 경고를 없앤다. */
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->my_lba),
                       (unsigned long long)le64_to_cpu(agpt->alternate_lba));
		error_found++; /* [한국어] 상호 참조(정방향) 불일치 1건 기록 */
	}
	/* [한국어] 검사 2: 같은 상호 참조를 반대 방향으로도 본다. 한쪽 방향만
	 * 보면 "primary만 새로 쓰고 alternate는 옛것" 같은 반쪽 갱신을 놓친다. */
	if (le64_to_cpu(pgpt->alternate_lba) != le64_to_cpu(agpt->my_lba)) {
		pr_warn("GPT:Primary header alternate_lba != Alt. header my_lba\n");
		/* [한국어] 어느 쪽이 옛 값인지 사람이 판단할 수 있도록 실제 두 값을
		 * 함께 찍는다. 커널은 여기서 스스로 고치려 들지 않는다. */
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->alternate_lba),
                       (unsigned long long)le64_to_cpu(agpt->my_lba));
		error_found++; /* [한국어] 상호 참조(역방향) 불일치 1건 기록 */
	}
	/* [한국어] 검사 3: 파티션이 놓일 수 있는 영역의 시작점. 두 헤더가 서로
	 * 다른 first_usable_lba를 주장하면, 어느 헤더를 믿느냐에 따라 PTE 배열
	 * 영역과 파티션 영역의 경계가 달라져 메타데이터를 덮어쓸 수 있다. */
	if (le64_to_cpu(pgpt->first_usable_lba) !=
            le64_to_cpu(agpt->first_usable_lba)) {
		pr_warn("GPT:first_usable_lbas don't match.\n");
		/* [한국어] 값 자체를 남겨야 어느 쪽이 백업 PTE 영역을 잘못 잡고
		 * 있는지 추적할 수 있다. */
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->first_usable_lba),
                       (unsigned long long)le64_to_cpu(agpt->first_usable_lba));
		error_found++; /* [한국어] usable 영역 시작 불일치 1건 기록 */
	}
	/* [한국어] 검사 4: usable 영역의 끝. 디스크(또는 가상 디스크 이미지)를
	 * 키운 뒤 alternate GPT만 새 끝으로 옮기고 primary를 갱신하지 않은
	 * 상황에서 가장 흔하게 걸리는 항목이다. */
	if (le64_to_cpu(pgpt->last_usable_lba) !=
            le64_to_cpu(agpt->last_usable_lba)) {
		pr_warn("GPT:last_usable_lbas don't match.\n");
		/* [한국어] 두 값의 차이가 곧 "인식하지 못하고 버려지는 뒤쪽 용량"의
		 * 크기이므로 그대로 출력해 준다. */
		pr_warn("GPT:%lld != %lld\n",
		       (unsigned long long)le64_to_cpu(pgpt->last_usable_lba),
                       (unsigned long long)le64_to_cpu(agpt->last_usable_lba));
		error_found++; /* [한국어] usable 영역 끝 불일치 1건 기록 */
	}
	/* [한국어] 검사 5: 디스크 GUID. 이 값이 다르면 두 헤더는 서로 다른
	 * 디스크의 것이다. GUID는 정수가 아니라 바이트 배열이므로 le*_to_cpu를
	 * 쓰지 않고 efi_guidcmp()(memcmp 계열)로 원시 바이트를 그대로 비교한다.
	 * 값 자체는 128비트라 출력하지 않고 불일치 사실만 알린다. */
	if (efi_guidcmp(pgpt->disk_guid, agpt->disk_guid)) {
		pr_warn("GPT:disk_guids don't match.\n");
		error_found++; /* [한국어] 디스크 GUID 불일치 1건 기록 */
	}
	/* [한국어] 검사 6: PTE 배열의 엔트리 개수. 두 배열의 길이가 다르면
	 * partition_entry_array_crc32의 계산 범위 자체가 달라지므로, 백업으로
	 * 폴백했을 때 인식되는 파티션 개수가 바뀔 수 있다. */
	if (le32_to_cpu(pgpt->num_partition_entries) !=
            le32_to_cpu(agpt->num_partition_entries)) {
		/* [한국어] 32비트 값이라 캐스팅 없이 %x로 바로 찍는다. 개수를 16진수로
		 * 보이는 이유는 흔한 값 128이 0x80처럼 눈에 익은 형태로 드러나기
		 * 때문이다. */
		pr_warn("GPT:num_partition_entries don't match: "
		       "0x%x != 0x%x\n",
		       le32_to_cpu(pgpt->num_partition_entries),
		       le32_to_cpu(agpt->num_partition_entries));
		error_found++; /* [한국어] 엔트리 개수 불일치 1건 기록 */
	}
	/* [한국어] 검사 7: 엔트리 하나의 크기. is_gpt_valid()는 채택한 헤더의
	 * 값만 sizeof(gpt_entry)와 대조하므로, 두 헤더가 서로 다른 크기를
	 * 주장하는 상황은 여기서만 드러난다. */
	if (le32_to_cpu(pgpt->sizeof_partition_entry) !=
            le32_to_cpu(agpt->sizeof_partition_entry)) {
		/* [한국어] 문자열 리터럴을 두 줄로 쪼갠 것은 한 줄 80칸 제한을 지키기
		 * 위한 것으로, C의 인접 문자열 연결로 하나의 포맷 문자열이 된다. */
		pr_warn("GPT:sizeof_partition_entry values don't match: "
		       "0x%x != 0x%x\n",
                       le32_to_cpu(pgpt->sizeof_partition_entry),
		       le32_to_cpu(agpt->sizeof_partition_entry));
		error_found++; /* [한국어] 엔트리 크기 불일치 1건 기록 */
	}
	/* [한국어] 검사 8: PTE 배열의 CRC32. 앞의 개수/크기가 같은데 이 값이
	 * 다르다면 두 배열의 내용 자체가 다르다는 뜻, 즉 파티션 구성이 한쪽에만
	 * 반영되어 있다는 결정적 증거다. 여기서 CRC를 다시 계산하지는 않고
	 * 헤더에 적힌 값끼리만 비교한다(각 배열의 실제 CRC 검증은 이미
	 * is_gpt_valid()가 마쳤다). */
	if (le32_to_cpu(pgpt->partition_entry_array_crc32) !=
            le32_to_cpu(agpt->partition_entry_array_crc32)) {
		/* [한국어] 두 CRC 값을 그대로 보여 준다. 사용자는 이 값이 다르다는
		 * 사실만으로 "백업 GPT가 오래된 구성을 담고 있다"고 판단할 수 있다. */
		pr_warn("GPT:partition_entry_array_crc32 values don't match: "
		       "0x%x != 0x%x\n",
                       le32_to_cpu(pgpt->partition_entry_array_crc32),
		       le32_to_cpu(agpt->partition_entry_array_crc32));
		error_found++; /* [한국어] PTE 배열 내용 불일치 1건 기록 */
	}
	/* [한국어] 검사 9: 두 헤더끼리가 아니라 실제 디스크 크기와 대조한다.
	 * UEFI 사양상 백업 헤더는 반드시 마지막 LBA에 있어야 하므로, primary가
	 * 가리키는 위치가 lastlba가 아니면 디스크가 커졌거나 잘렸다는 뜻이다. */
	if (le64_to_cpu(pgpt->alternate_lba) != lastlba) {
		pr_warn("GPT:Primary header thinks Alt. header is not at the end of the disk.\n");
		/* [한국어] primary가 믿는 위치와 실제 디스크 끝을 나란히 출력한다. */
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(pgpt->alternate_lba),
			(unsigned long long)lastlba);
		error_found++; /* [한국어] primary가 가리키는 백업 위치와 디스크 끝 불일치 1건 기록 */
	}

	/* [한국어] 검사 10: 백업 헤더 자신이 주장하는 위치도 디스크 끝인지 본다.
	 * 검사 9와 달리 이쪽은 agpt를 실제로 그 위치에서 읽어왔는지와 무관하게
	 * (드라이버가 준 alternative_gpt_sector 힌트에서 읽었을 수도 있다)
	 * 사양 위반 여부를 알려 준다. */
	if (le64_to_cpu(agpt->my_lba) != lastlba) {
		pr_warn("GPT:Alternate GPT header not at the end of the disk.\n");
		/* [한국어] 백업 헤더가 자칭하는 위치와 실제 디스크 끝을 나란히 출력. */
		pr_warn("GPT:%lld != %lld\n",
			(unsigned long long)le64_to_cpu(agpt->my_lba),
			(unsigned long long)lastlba);
		error_found++; /* [한국어] 백업 헤더 위치가 디스크 끝이 아님 1건 기록 */
	}

	/* [한국어] 개별 경고는 이미 다 찍었으므로 여기서는 조치 방법만 한 줄로
	 * 안내한다. 커널이 스스로 헤더를 고쳐 쓰지 않는 이유는 파티션 스캔이
	 * 읽기 전용 동작이어야 하기 때문이다 - 잘못 추측해 덮어쓰면 아직
	 * 살아 있는 사본까지 잃는다. */
	if (error_found)
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
 * 이 파일 안에서 efi_partition()만이 이 함수를 호출한다(static 함수).
 * 먼저 LBA 0의 protective MBR을 검사하고, primary GPT(LBA 1)와 alternate GPT
 * (마지막 LBA)를 읽어 유효한 쪽을 선택한다. 파티션 테이블을 찾지 못하면 이
 * 디스크는 파티션 디바이스 없이 디스크 전체 노드 하나로만
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
	sector_t total_sectors = get_capacity(state->disk); /* [한국어] 디스크 용량을 512바이트 커널 섹터 개수로 표현한 값. is_pmbr_valid()에 넘기는데, MBR의 size_in_lba가 512바이트 단위이기 때문이다 - 논리 블록 단위인 lastlba와 혼동하면 안 된다. */
	u64 lastlba; /* [한국어] 논리 블록 단위의 마지막 LBA. 백업 GPT의 위치이자 is_gpt_valid()에 넘길 범위 상한이다. */

	if (!ptes) /* [한국어] 호출자 프로그래밍 오류 방어: 출력 인자가 없으면 아무 것도 채울 수 없으므로 즉시 실패 */
		return 0; /* [한국어] 아직 아무 자원도 할당하지 않았으므로 정리 없이 바로 반환 가능 */

	lastlba = last_lba(state->disk); /* [한국어] 사양상 백업 GPT가 놓이는 자리. 아래에서 alternate 탐색 위치이자 compare_gpts()의 대조 기준으로 쓰인다. */
	/* [한국어] 기본 경로에서는 pMBR 관문을 먼저 통과해야 한다. GPT가 아닌
	 * 디스크에서 LBA 1을 GPT 헤더로 해석하려는 헛수고와, 그로 인한 오탐을
	 * 막기 위해서다. force_gpt가 켜져 있으면 이 관문을 통째로 건너뛴다. */
        if (!force_gpt) {
		/* This will be added to the EFI Spec. per Intel after v1.02. */
		legacymbr = kzalloc_obj(*legacymbr); /* [한국어] kzalloc(0으로 초기화)을 쓰는 이유: read_lba()가 실패해도 버퍼에 이전 힙 내용이 남지 않아, is_pmbr_valid()의 시그니처 검사가 확실히 실패하도록 만들기 위해서다. */
		if (!legacymbr) /* [한국어] 할당 실패 */
			goto fail; /* [한국어] 메모리 부족으로 pMBR 검사조차 시작할 수 없음 - pgpt/agpt 등은 아직 NULL이므로 fail 라벨에서 kfree(NULL)로 안전하게 정리됨 */

		/* [한국어] LBA 0의 첫 512바이트를 읽어 legacy/protective MBR로 해석한다. */
		read_lba(state, 0, (u8 *)legacymbr, sizeof(*legacymbr)); /* [한국어] 반환값을 일부러 무시한다. 위에서 0으로 초기화해 두었으므로, 읽기가 실패하면 시그니처가 0이 되어 어차피 is_pmbr_valid()에서 걸러진다. */
		good_pmbr = is_pmbr_valid(legacymbr, total_sectors); /* [한국어] 0 / PROTECTIVE / HYBRID 중 하나를 받는다. 아래에서는 0인지 여부만 보고, 종류는 pr_debug 문구를 고르는 데만 쓴다. */
		kfree(legacymbr); /* [한국어] 판정이 끝났으므로 즉시 반납한다. 이후 경로가 길어 여기서 놓치면 실패 경로마다 해제 코드를 중복해야 한다. */

		if (!good_pmbr) /* [한국어] pMBR이 없으면 GPT 디스크가 아니다. 여기서 실패를 돌려주면 core.c의 프로버 목록이 다음 후보(msdos 등)로 넘어가, 레거시 MBR로 다시 해석하게 된다. */
			goto fail; /* [한국어] GPT의 필수 전제 조건(유효한 protective/hybrid MBR)이 없으므로 GPT일 리가 없다고 판정 - primary/alternate GPT는 시도조차 하지 않음 */

		pr_debug("Device has a %s MBR\n",
			 good_pmbr == GPT_MBR_PROTECTIVE ?
						"protective" : "hybrid");
	}

	/* [한국어] 1순위: LBA 1의 primary GPT. 성공하면 pgpt/pptes가 채워진다. */
	good_pgpt = is_gpt_valid(state, GPT_PRIMARY_PARTITION_TABLE_LBA,
				 &pgpt, &pptes);
	/* [한국어] primary가 유효할 때만 백업도 읽어 본다. 이때 위치는 lastlba가
	 * 아니라 primary가 스스로 적어 둔 alternate_lba를 따르는데, 두 값이
	 * 어긋난 상황(디스크 크기 변경 등)은 아래 compare_gpts()가 경고로 알린다. */
        if (good_pgpt)
		good_agpt = is_gpt_valid(state, /* [한국어] 백업 헤더는 여기서 검증만 하고, 실제 파티션 정보는 primary 쪽을 쓴다. */
					 le64_to_cpu(pgpt->alternate_lba),
					 &agpt, &aptes);
	/* [한국어] 2순위: primary가 깨졌을 때의 폴백. 사양이 정한 자리(마지막
	 * LBA)에서 백업 헤더를 직접 찾는다. force_gpt 조건이 붙어 있는 이유는,
	 * 이 지점에 오는 경우 pMBR 검사도 이미 건너뛴 상태여서 GPT가 아닌
	 * 디스크의 마지막 섹터를 헤더로 오인할 위험이 있기 때문이다. */
        if (!good_agpt && force_gpt)
                good_agpt = is_gpt_valid(state, lastlba, &agpt, &aptes);

	/* [한국어] 3순위: 드라이버가 알려 주는 위치. 장치가 보고하는 용량과
	 * 실제 백업 GPT 위치가 어긋나는 경우(호스트가 보는 크기와 다른 크기로
	 * 파티셔닝된 디스크 등) 드라이버가 콜백으로 올바른 섹터를 알려 줄 수 있다. */
	if (!good_agpt && force_gpt && fops->alternative_gpt_sector) {
		sector_t agpt_sector; /* [한국어] 드라이버가 채워 줄 백업 GPT 섹터 번호 */
		int err; /* [한국어] 콜백 성공 여부(0이면 agpt_sector가 유효) */

		err = fops->alternative_gpt_sector(disk, &agpt_sector); /* [한국어] 드라이버별 힌트 조회. 콜백은 선택 사항이라 위에서 존재 여부를 먼저 확인했다. */
		/* [한국어] 힌트를 얻은 경우에만 시도한다. 실패했다면 agpt_sector는
		 * 초기화되지 않은 스택 값이므로 절대 사용해서는 안 된다. 힌트
		 * 위치라 해도 검증을 건너뛰지는 않는다. */
		if (!err)
			good_agpt = is_gpt_valid(state, agpt_sector,
						 &agpt, &aptes);
	}

        /* The obviously unsuccessful case */
        if (!good_pgpt && !good_agpt) /* [한국어] 이중화의 두 사본이 모두 무너진 경우 */
                goto fail; /* [한국어] 위에서 시도한 모든 경로(primary, primary가 가리키는 alternate, force_gpt의 마지막 LBA, 드라이버 힌트)가 전부 실패 - 더 이상 시도할 위치가 없으므로 최종 포기 */

	/* primary/alternate GPT 헤더 상호 비교 */
        compare_gpts(pgpt, agpt, lastlba); /* [한국어] 진단 전용 호출. 반환값이 없고 스캔 결과에도 영향을 주지 않으므로, 여기서 어떤 경고가 나와도 아래 채택 로직은 그대로 진행된다. */

        /* The good cases */
        if (good_pgpt) { /* primary GPT 우선 사용 */
                *gpt  = pgpt; /* primary GPT 헤더 결과 반환 */
                *ptes = pptes; /* primary PTE 배열 결과 반환 */
                kfree(agpt); /* alternate GPT 헤더 메모리 정리 */
                kfree(aptes); /* alternate PTE 배열 메모리 정리 */
		if (!good_agpt) /* [한국어] primary만으로도 동작에는 지장이 없지만, 백업이 깨진 상태를 방치하면 다음 사고 때 복구 수단이 없으므로 사용자에게 알린다. */
                        pr_warn("Alternate GPT is invalid, using primary GPT.\n");
                return 1; /* [한국어] primary가 유효하므로 이를 채택해 성공 반환 - alternate 손상 여부와 무관하게 primary만으로 충분 (단, 위 pr_warn으로 사용자에게는 알림) */
        }
        else if (good_agpt) { /* primary 손상 시 alternate GPT 사용 */
                *gpt  = agpt; /* alternate GPT 헤더 결과 반환 */
                *ptes = aptes; /* alternate PTE 배열 결과 반환 */
                kfree(pgpt); /* primary GPT 헤더 메모리 정리 */
                kfree(pptes); /* primary PTE 배열 메모리 정리 */
		/* [한국어] 여기는 조용히 넘어가지 않고 항상 경고한다. 백업으로
		 * 동작 중이라는 사실은 사용자가 반드시 알아야 할 상태이기 때문이다. */
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
 * GPT 파티션 이름(partition_name)은 UTF-16LE 36코드 유닛으로 저장된다.
 * 커널은 이를 파티션의 볼륨 레이블(/sys/.../partition 관련 속성)로 노출한다.
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
 * core.c의 check_partition()이 check_part[] 순서대로 프로버를 호출하다가
 * 이 함수에 닿는다. 배열에서 이 함수는 msdos_partition()보다 "앞"에 있는데
 * (core.c의 "this must come before msdos" 주석 참조), GPT 디스크가 호환성을
 * 위해 보호 MBR도 함께 갖고 있어서 msdos가 먼저 매칭되면 GPT를 통째로
 * 놓치기 때문이다. find_valid_gpt()로 헤더와 PTE를 얻은 뒤, 각 파티션의
 * starting_lba/ending_lba를 커널 섹터 단위로 환산해 put_partition()으로
 * state->parts[]에 적재한다. 실제 block_device 생성과 /dev 노드 노출은
 * 이 함수가 1을 반환한 뒤 core.c의 blk_add_partitions()가 수행한다.
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
	/* [한국어] 논리 블록 -> 커널 섹터 환산 계수. GPT의 LBA는 논리 블록
	 * 단위인데 put_partition()은 512바이트 섹터 단위를 받으므로, 아래에서
	 * start/size에 이 값을 곱해 넘긴다. 512바이트 디스크면 1이라 무변환. */
	unsigned ssz = queue_logical_block_size(state->disk->queue) / 512; /* [한국어] 4Kn 디스크면 8이 된다. 이 곱셈을 빠뜨리면 파티션 시작 위치가 실제의 1/8 지점으로 잡혀 엉뚱한 영역이 노출된다. */

	/* find_valid_gpt()가 실패하면 GPT 파티션이 아님 (msdos_partition()이 처리할 수도 있음) */
	if (!find_valid_gpt(state, &gpt, &ptes) || !gpt || !ptes) { /* [한국어] 반환값 외에 두 포인터도 함께 확인하는 이중 방어. find_valid_gpt()는 성공 시 반드시 둘 다 채우지만, 여기서 한 번 더 보고 들어가야 아래 루프가 NULL 역참조 걱정 없이 돌 수 있다. */
		kfree(gpt); /* [한국어] 실패 경로에서는 이미 NULL이지만, kfree(NULL)이 안전하므로 조건 없이 호출해 코드를 단순화한다. */
		kfree(ptes); /* [한국어] 위와 동일 */
		return 0; /* [한국어] "이 디스크는 GPT가 아니다"라는 정상 판정 - check.c가 msdos_partition() 등 다른 포맷 프로버로 계속 진행 */
	}

	pr_debug("GUID Partition Table is valid!  Yea!\n");

	/* gpt_header.num_partition_entries만큼 파티션 엔트리 순회 */
	/* [한국어] 배열 인덱스 i가 그대로 파티션 번호 i+1이 된다. 종료 조건이
	 * 두 개인데, 앞은 헤더가 자칭하는 엔트리 개수이고 뒤의 state->limit는
	 * 커널이 이 디스크에 만들 수 있는 파티션 수 상한이다. 후자가 없으면
	 * 큰 num_partition_entries를 적은 디스크가 parts[] 배열 밖을 넘어
	 * 쓰게 된다(-1은 parts[0]이 디스크 전체용으로 예약되어 있기 때문). */
	for (i = 0; i < le32_to_cpu(gpt->num_partition_entries) && i < state->limit-1; i++) {
		struct partition_meta_info *info; /* [한국어] UUID와 볼륨 라벨을 담을 parts[] 슬롯의 메타정보 영역 포인터 */
		unsigned label_max; /* [한국어] 볼륨 라벨로 복사할 최대 문자 수(양쪽 버퍼 중 작은 쪽) */
		u64 start = le64_to_cpu(ptes[i].starting_lba); /* [한국어] 오프셋 0x20. 온디스크 리틀엔디안 64비트를 호스트 순서로. */
		u64 size = le64_to_cpu(ptes[i].ending_lba) - /* [한국어] 오프셋 0x28. ending_lba는 마지막 블록을 "포함"하는 값이라(반열림 구간이 아님) */
			   le64_to_cpu(ptes[i].starting_lba) + 1ULL; /* [한국어] 크기를 얻으려면 +1이 필요하다. 이 +1을 빼먹으면 모든 파티션이 한 블록씩 짧아진다. */

		/* [한국어] 유효성 검사를 크기 계산 뒤에 두어도 순서상 무해하다(무효면
		 * 계산 결과를 쓰지 않고 건너뛴다). 다만 start > end인 엔트리는
		 * is_pte_valid()가 걸러 주지 않아 size가 언더플로된 거대한 값이 될 수
		 * 있다. put_partition() 자체는 슬롯 번호만 보고 값을 그대로 저장하므로,
		 * 그런 항목은 나중에 core.c의 blk_add_partition()이 디스크 용량(EOD)과
		 * 대조해 경고를 남기며 걸러낸다. */
		if (!is_pte_valid(&ptes[i], last_lba(state->disk)))
			continue; /* [한국어] 이 인덱스는 파티션 번호(i+1)만 건너뛰고 다음 엔트리로 - state->parts[i+1]은 채워지지 않은 채로 남는다(파티션 번호에 구멍이 생길 수 있음, GPT 스펙상 정상) */

		/* [한국어] 검출 결과를 state->parts[i+1]에 적재한다. 실제
		 * block_device 생성은 이 함수가 1을 반환한 뒤 core.c의
		 * blk_add_partitions()가 담당한다. */
		put_partition(state, i+1, start * ssz, size * ssz); /* [한국어] 여기서 논리 블록을 512바이트 섹터로 환산해 넘긴다. put_partition()(check.h)은 슬롯 번호가 state->limit 미만인지만 확인하고 값을 그대로 저장할 뿐, 디스크 용량 검사는 하지 않는다. */

		/* If this is a RAID volume, tell md */
		/* gpt_entry.partition_type_guid: Linux RAID GUID 확인 */
		if (!efi_guidcmp(ptes[i].partition_type_guid, PARTITION_LINUX_RAID_GUID)) /* [한국어] 오프셋 0x00의 타입 GUID가 Linux RAID(a19d880f-05fc-4d3b-a006-743f0f84911e)인지 본다. efi_guidcmp()는 일치할 때 0이므로 조건에 부정이 붙는다. */
			state->parts[i + 1].flags = ADDPART_FLAG_RAID; /* [한국어] core.c의 blk_add_partition()이 이 플래그를 보고 md_autodetect_dev()를 불러, 부팅 시 RAID 어레이 자동 조립 대상으로 등록한다. */

		/* parsed_partitions.parts[]: 파티션 메타정보 저장 */
		info = &state->parts[i + 1].info; /* [한국어] put_partition()이 이미 자리를 잡아 둔 슬롯의 메타정보 영역 */
		/* [한국어] 타입 GUID(파티션 용도)와 달리 unique GUID는 파티션마다
		 * 고유한 식별자다. 문자열로 변환해 두면 udev가 이를 읽어
		 * /dev/disk/by-partuuid/ 심볼릭 링크를 만든다. */
		efi_guid_to_str(&ptes[i].unique_partition_guid, info->uuid); /* [한국어] 오프셋 0x10. GUID의 앞 세 필드는 리틀엔디안 정수, 뒤 두 필드는 바이트 배열이라는 혼합 규칙을 efi_guid_to_str()이 처리한다. */

		/* Naively convert UTF16-LE to 7 bits. */
		/* gpt_entry.partition_name: UTF-16LE 볼륨 이름을 7비트 ASCII로 변환 */
		/* [한국어] 두 버퍼 중 작은 쪽에 맞춘다. -1은 utf16_le_to_7bit()가
		 * out[size]에 널 종료를 쓰기 때문에 남겨 두는 자리다. 이 계산을
		 * 틀리면 volname 배열 바로 뒤를 1바이트 덮어쓴다. */
		label_max = min(ARRAY_SIZE(info->volname) - 1,
				ARRAY_SIZE(ptes[i].partition_name));
		utf16_le_to_7bit(ptes[i].partition_name, label_max, info->volname); /* [한국어] 오프셋 0x38의 UTF-16LE 36코드 유닛 이름을 7비트로 눌러 담는다. partition_name은 널 종료가 보장되지 않으므로 길이를 명시해 넘긴다. */
		state->parts[i + 1].has_info = true; /* [한국어] uuid/volname을 채웠다는 표시. core.c는 이 플래그가 서 있을 때만 메타정보를 block_device로 복사한다. */
	}
	kfree(ptes); /* [한국어] 필요한 값은 모두 state->parts[]로 복사했으므로 원본 배열은 여기서 반납한다. */
	kfree(gpt); /* [한국어] 헤더도 마찬가지. 이 함수가 반환한 뒤에는 GPT 원본이 메모리에 남지 않는다. */
	seq_buf_puts(&state->pp_buf, "\n"); /* [한국어] /proc/partitions 등에 출력되는 파티션 요약 문자열 버퍼(pp_buf)에 개행 추가 - 다른 포맷 프로버들과 동일한 규약(각 프로버가 자신의 출력 끝에 개행을 남김) */
	return 1; /* [한국어] GPT 파티션을 성공적으로 찾아 전부 등록했음을 check.c에 알림 - 이후 다른 포맷 프로버는 시도되지 않는다 */
}

/* [한국어] 이 파일 전체를 관통하는 요점 정리
 * - 파티션 파서는 장치 종류(NVMe/SATA/virtio/loop 등)와 무관하다. 이 파일이
 *   보는 것은 오직 "논리 블록 크기"와 "총 용량", 그리고 디스크에 적힌
 *   바이트뿐이며, 실제 읽기는 read_part_sector()가 페이지 캐시를 통해
 *   처리하므로 어느 드라이버가 아래에 있는지는 알 필요도 없다.
 * - 단위가 두 종류라는 점이 반복해서 등장한다. GPT의 LBA는 논리 블록 단위,
 *   블록 계층의 섹터는 항상 512바이트다. read_lba()에서 한 번(입력 방향),
 *   efi_partition()의 ssz에서 한 번(출력 방향) 환산이 일어난다.
 * - 온디스크 표현은 전부 리틀엔디안이라 모든 다바이트 필드 접근에
 *   le16/le32/le64_to_cpu()가 붙는다. 예외는 GUID로, 바이트 배열이라
 *   efi_guidcmp()가 원시 바이트를 그대로 비교한다.
 * - 신뢰의 순서가 곧 코드의 순서다: pMBR 관문 -> 헤더 시그니처 -> 헤더 크기
 *   상·하한 -> 헤더 CRC -> my_lba -> usable 범위 -> 엔트리 크기 -> 배열
 *   크기 -> 배열 CRC. 앞 단계를 통과하기 전의 필드 값은 믿지 않는다.
 * - 이중화 복구: primary가 깨지면 백업 헤더로 폴백하고, 둘 다 살아 있으면
 *   compare_gpts()가 교차 비교해 경고만 남긴다. 커널은 어느 경우에도 디스크를
 *   고쳐 쓰지 않는다(파티션 스캔은 읽기 전용이어야 한다).
 * - 프로버 순서상 efi_partition()은 msdos_partition()보다 "먼저" 호출된다
 *   (block/partitions/core.c의 check_part[] 참조). GPT 디스크는 호환성을 위해
 *   보호 MBR도 함께 갖고 있어, msdos가 먼저 매칭되면 GPT를 놓치기 때문이다.
 */
