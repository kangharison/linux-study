// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어 설명] MS-DOS(MBR, Master Boot Record) 파티션 테이블 파서 (msdos.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 디스크 첫 섹터(LBA 0)의 512바이트 부트섹터에 기록되는 전통적인
 * MS-DOS/MBR 파티션 테이블을 해석한다. MBR은 오프셋 0x1be(446)부터 16바이트
 * 짜리 파티션 엔트리 4개(기본 파티션, primary partition)를 담을 수 있고,
 * 그중 하나를 "확장 파티션(extended partition)"으로 지정하면 EBR(Extended
 * Boot Record) 체인을 통해 임의 개수의 논리 파티션(logical partition)을
 * 추가로 표현할 수 있다. 이 파일은 여기에 더해 각 기본 파티션 내부에 중첩될
 * 수 있는 BSD disklabel, Solaris x86 VTOC, Unixware VTOC, Minix 서브
 * 파티션, AIX LVM, DOS DriveManager/EZD 등 다양한 OS별 서브 레이아웃까지
 * 인식하는, block/partitions/ 서브시스템에서 가장 많은 변종을 다루는
 * 프로버(prober)다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * gendisk가 등록되거나(add_disk()) 재스캔 ioctl(BLKRRPART 등)이 발생하면
 * block/partitions/core.c의 bdev_disk_changed() -> blk_add_partitions() ->
 * check_partition()이 check.h가 선언한 20여 개 포맷별 프로버를 순서대로
 * 함수 포인터로 호출하며, 이 파일의 msdos_partition()이 그 후보 중 하나로
 * 등록되어 있다. 이 파일은 GPT(efi.c)와 상호 배타적으로 동작하도록 설계
 * 되어 있다 - msdos_partition()은 4개 기본 파티션 항목 중 하나라도
 * EFI_PMBR_OSTYPE_EFI_GPT(0xEE, efi.h 정의, GPT protective/hybrid MBR
 * 표지)를 발견하면 즉시 0을 반환해 자기 자신을 무효화하고 efi_partition()
 * 에게 실제 파싱을 양보한다(아래 msdos_partition()의 "#ifdef
 * CONFIG_EFI_PARTITION" 블록 참고 - efi.c와의 하이브리드/보호 관계는
 * efi.c 상단 주석과 is_pmbr_valid()/pmbr_part_valid()에 상세히 기술됨).
 * 이 코드는 디스크 마운트/파일시스템 I/O가 시작되기 이전, 디스크 등록/
 * 재스캔 시점에 단일 프로세스 컨텍스트에서 동기적으로 실행되며, 인터럽트
 * 컨텍스트나 GPU 커널과는 무관하다.
 *
 * === 타 모듈과의 연결 ===
 * - check.h/core.c: struct parsed_partitions(스캔 세션 상태), 섹터를
 *   안전하게 읽고 반납하는 read_part_sector()/put_dev_sector(), 검출한
 *   파티션 하나를 상태에 기록하는 put_partition()을 제공한다. 이 파일의
 *   모든 디스크 접근과 파티션 등록은 이 공용 계약을 통해서만 이뤄진다.
 * - efi.h/efi.c: EFI_PMBR_OSTYPE_EFI_GPT 매직을 공유해 GPT/MBR 하이브리드
 *   판별의 경계를 이룬다 - 이 파일은 GPT 신호를 보면 즉시 물러나고, GPT
 *   보호 MBR의 상세 검증 자체는 efi.c가 담당한다.
 * - include/linux/msdos_partition.h: 온디스크 struct msdos_partition(16
 *   바이트 엔트리) 레이아웃과 sys_ind(파티션 타입 바이트) enum 상수
 *   (DOS_EXTENDED_PARTITION, LINUX_DATA_PARTITION, FREEBSD_PARTITION 등)
 *   을 정의 - 이 파일 전체가 이 헤더의 상수/구조체에 의존한다.
 * - include/linux/msdos_fs.h: struct fat_boot_sector와 fat_valid_media()
 *   를 제공 - 파티션 테이블이 아예 없는 순수 FAT 부트섹터를 msdos_partition()
 *   이 폴백으로 인식할 때 쓰인다.
 * - 데이터 흐름: 디스크(LBA 0, 각 EBR, 각 서브 레이블 섹터) ->
 *   read_part_sector()가 페이지 캐시(또는 하위 블록 드라이버의 submit_bio
 *   경로)로 섹터를 읽음 -> nr_sects()/start_sect()가 비정렬(unaligned)
 *   32비트 LE 필드를 안전하게 추출 -> put_partition()으로
 *   parsed_partitions.parts[]에 적재 -> core.c가 이를 실제 block_device로
 *   반영한다.
 * - 공유 핵심 자료구조: struct parsed_partitions(check.h, 스캔 세션),
 *   struct msdos_partition(msdos_partition.h, 온디스크 16바이트 엔트리),
 *   그리고 이 파일 내부 전용의 struct solaris_x86_vtoc/bsd_disklabel/
 *   unixware_disklabel(서브 레이블 온디스크 구조체 - 다른 파일과 공유하지
 *   않는 이 파일만의 파싱 스크래치 타입).
 *
 * === 주요 함수/구조체 요약 ===
 * - msdos_partition(): 이 파일의 유일한 외부 진입점. LBA 0을 읽어 AIX/
 *   0x55AA/GPT 여부를 순서대로 배제한 뒤, 1st pass(기본 파티션 4개 +
 *   확장 파티션 체인)와 2nd pass(서브 레이블)를 수행한다.
 * - parse_extended(): 확장 파티션의 EBR 체인을 최대 100개까지 따라가며
 *   논리 파티션(5번부터 시작하는 파티션 번호)을 찾아 등록한다.
 * - parse_bsd()/parse_solaris_x86()/parse_unixware()/parse_minix(): 각각
 *   BSD disklabel, Solaris VTOC, Unixware VTOC, Minix 서브 파티션 테이블
 *   이라는 OS별 중첩 레이블을 파싱하는 2nd pass 서브 파서들.
 * - aix_magic_present(): MSDOS 0x55AA 시그니처가 없는 일부 AIX 디스크를
 *   'IBMA'(EBCDIC) 매직과 '_LVM' 문자열로 식별한다.
 * - nr_sects()/start_sect(): msdos_partition 엔트리의 비정렬 32비트 LE
 *   필드(nr_sects, start_sect)를 안전하게 읽는 헬퍼.
 * - subtypes[]: sys_ind(파티션 타입 바이트) -> 2nd pass 서브 파서 함수
 *   포인터 매핑 테이블(BSD 계열/Minix/Unixware/Solaris).
 */

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
/* [한국어] struct fat_boot_sector, fat_valid_media() 제공 - msdos_partition()이
 * 파티션 테이블 없이 부트섹터만 있는 순수 FAT 디스크를 폴백으로 인식할 때 사용(아래
 * msdos_partition()의 boot_ind 검사 실패 경로 참고). */
#include <linux/msdos_fs.h>
/* [한국어] 온디스크 struct msdos_partition(16바이트 엔트리) 레이아웃과 sys_ind
 * enum 상수(DOS_EXTENDED_PARTITION, LINUX_DATA_PARTITION, FREEBSD_PARTITION,
 * MINIX_PARTITION 등) 정의 - 이 파일 전체가 의존하는 가장 기본적인 헤더. */
#include <linux/msdos_partition.h>

/* [한국어] parsed_partitions, Sector, read_part_sector()/put_dev_sector()/
 * put_partition() 등 파티션 검출 인프라 공용 계약 - 이 파일의 모든 섹터 읽기와
 * 파티션 등록이 이 API를 경유한다(check.h 상단 블록 주석 참고). */
#include "check.h"
/* [한국어] EFI_PMBR_OSTYPE_EFI_GPT(0xEE) 등 GPT 관련 매직 상수 - msdos_partition()이
 * GPT protective/hybrid MBR을 만나면 스스로 물러나기 위해 이 상수로 판별한다
 * (efi.c와의 하이브리드/보호 관계, 상세 검증은 efi.c의 is_pmbr_valid() 참고). */
#include "efi.h"

/* [한국어] 아래 원본 주석 보강: msdos_partition 구조체는 __packed 속성으로 선언되어
 * 있어(1바이트 필드 8개 뒤에 4바이트 LE32 필드 2개가 옴) start_sect/nr_sects가
 * 4바이트 정렬 경계(주소가 4의 배수)에 있지 않다(오프셋 8, 12는 각각 2 mod 4). 일부
 * 아키텍처는 정렬되지 않은 다중 바이트 접근에서 버스 오류나 성능 저하를 겪으므로,
 * 아래 nr_sects()/start_sect() 헬퍼가 get_unaligned_le32()로 우회한다. */
/*
 * Many architectures don't like unaligned accesses, while
 * the nr_sects and start_sect partition table entries are
 * at a 2 (mod 4) address.
 */
/* [한국어] get_unaligned_le32() 제공 - 위 설명대로 msdos_partition의 비정렬 LE32
 * 필드(start_sect, nr_sects)를 안전하게 읽기 위해 필요. */
#include <linux/unaligned.h>

/*
 * [한국어]
 * nr_sects() - msdos_partition 엔트리의 파티션 길이(섹터 수) 필드를 안전하게 읽는다.
 *
 * @p: 대상 msdos_partition 온디스크 엔트리 포인터. MBR/EBR 512바이트 버퍼 내
 *     오프셋 0x1be + 16*n 위치에 있는 16바이트 엔트리 하나를 가리킨다.
 * @return: 이 엔트리가 나타내는 파티션의 길이(512바이트 논리 섹터 수, sector_t).
 *
 * msdos_partition.nr_sects 필드(엔트리 내 오프셋 12)는 리틀엔디안 32비트
 * 정수이지만 __packed 구조체 배치상 4바이트 정렬 경계에 있지 않다. 이 함수는
 * p->nr_sects를 직접 역참조하지 않고 get_unaligned_le32()로 바이트 단위
 * 안전하게 읽어 sector_t로 캐스팅한다.
 * 실행 컨텍스트: 파티션 스캔 프로세스 컨텍스트에서 호출되는 순수 계산
 * 함수이며 부작용이 없어 재진입에 안전하다.
 * 호출자: aix_magic_present(), parse_extended(), parse_minix(),
 * msdos_partition() 등 msdos_partition 엔트리의 길이가 필요한 모든 위치.
 * 피호출자: get_unaligned_le32()(linux/unaligned.h).
 * 에러 경로: 없음(항상 값을 반환). 길이 0은 "이 엔트리가 비어 있음"으로
 * 호출자가 해석한다.
 *
 * 호출 체인:
 *   <각 파티션 파서> -> [nr_sects()] -> get_unaligned_le32()
 */
static inline sector_t nr_sects(struct msdos_partition *p)
{
	/* [한국어] msdos_partition.nr_sects(엔트리 내 오프셋 12, LE32, 비정렬)를 안전하게
	 * 읽어 sector_t(섹터 단위 길이)로 반환한다. */
	return (sector_t)get_unaligned_le32(&p->nr_sects);
}

/*
 * [한국어]
 * start_sect() - msdos_partition 엔트리의 파티션 시작 섹터 필드를 안전하게 읽는다.
 *
 * @p: 대상 msdos_partition 온디스크 엔트리 포인터(nr_sects()와 동일한 성격).
 * @return: 이 엔트리가 나타내는 파티션의 시작 위치(512바이트 논리 섹터 단위,
 *          sector_t). 절대 LBA가 아니라, 문맥에 따라 "직전 EBR/MBR 기준
 *          상대 오프셋"일 수도, "디스크 절대 오프셋"일 수도 있다 - 어느
 *          쪽인지는 호출자(예: msdos_partition() 1st pass vs
 *          parse_extended())가 스스로 해석/환산한다.
 *
 * msdos_partition.start_sect 필드(엔트리 내 오프셋 8)도 nr_sects와
 * 마찬가지로 리틀엔디안 32비트이지만 4바이트 정렬 경계에 있지 않으므로
 * get_unaligned_le32()로 안전하게 읽는다.
 * 실행 컨텍스트: 순수 계산 함수, 파티션 스캔 프로세스 컨텍스트에서 호출.
 * 호출자: msdos_partition()(1st/2nd pass), parse_extended()(EBR 체인 내
 * 데이터/링크 파티션), parse_minix().
 * 피호출자: get_unaligned_le32().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   <각 파티션 파서> -> [start_sect()] -> get_unaligned_le32()
 */
static inline sector_t start_sect(struct msdos_partition *p)
{
	/* [한국어] msdos_partition.start_sect(엔트리 내 오프셋 8, LE32, 비정렬)를 안전하게
	 * 읽어 sector_t로 반환한다. */
	return (sector_t)get_unaligned_le32(&p->start_sect);
}

/*
 * [한국어]
 * is_extended_partition() - 이 엔트리가 확장 파티션(Extended Partition)인지 판별한다.
 *
 * @p: 검사할 msdos_partition 엔트리 포인터.
 * @return: 참(0이 아님)이면 확장 파티션, 0이면 일반 데이터 파티션(또는 빈 항목).
 *
 * DOS_EXTENDED_PARTITION(0x05, 전통 DOS 확장 파티션),
 * WIN98_EXTENDED_PARTITION(0x0f, LBA 확장 파티션), LINUX_EXTENDED_PARTITION
 * (0x85, 리눅스 fdisk가 만드는 확장 파티션) 세 sys_ind 값 중 하나이면
 * 확장 파티션으로 간주한다. 확장 파티션 자체는 실제 데이터를 담지 않고,
 * 그 start_sect가 다음 EBR(Extended Boot Record)을 가리키는 링크 노드
 * 역할만 한다 - 그래서 parse_extended()/msdos_partition()은 이 판별
 * 결과에 따라 "데이터 파티션으로 등록"할지 "EBR 체인을 계속 따라갈지"를
 * 분기한다.
 * 실행 컨텍스트: 순수 판별 함수, 파티션 스캔 프로세스 컨텍스트.
 * 호출자: aix_magic_present()(AIX 오판 방지 검사), parse_extended()(1st
 * pass 데이터 파티션 필터링과 2nd pass 링크 탐색 양쪽), msdos_partition()
 * (1st pass에서 데이터 vs 확장 분기).
 * 피호출자: 없음(필드 비교만 수행).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   aix_magic_present()/parse_extended()/msdos_partition() -> [is_extended_partition()]
 */
static inline int is_extended_partition(struct msdos_partition *p)
{
	/* [한국어] sys_ind가 DOS(0x05)/WIN98 LBA(0x0f)/Linux(0x85) 확장 파티션 타입 중
	 * 하나인지 OR로 검사한다 - 세 값 모두 "이 엔트리는 데이터가 아니라 확장 링크"라는
	 * 동일한 의미를 갖는다. */
	return (p->sys_ind == DOS_EXTENDED_PARTITION ||
		p->sys_ind == WIN98_EXTENDED_PARTITION ||
		p->sys_ind == LINUX_EXTENDED_PARTITION);
}

/* [한국어] MBR/EBR 부트섹터 마지막 2바이트(오프셋 510, 511)에 있어야 하는 부트
 * 시그니처 0x55, 0xAA(리틀엔디안으로 읽으면 0xAA55) - BIOS가 "부팅 가능한
 * 디스크/파티션 테이블"로 인식하는 조건과 동일하며, 이 파일은 이를 "MBR 파티션
 * 테이블이 존재함"의 관문으로 재사용한다. */
#define MSDOS_LABEL_MAGIC1	0x55
#define MSDOS_LABEL_MAGIC2	0xAA	/* [한국어] MBR 부트 시그니처의 둘째 바이트. 섹터 0 의 마지막 두 바이트가
					 * 0x55 0xAA 여야 MBR 로 인정한다. 두 바이트뿐이라 우연히 일치할 수도 있어,
					 * 이 검사는 필요조건일 뿐 충분조건이 아니다 */

/*
 * [한국어]
 * msdos_magic_present() - 0x55AA MBR 부트 시그니처 존재 여부를 검사한다.
 *
 * @p: 시그니처를 검사할 2바이트 버퍼의 시작 주소. 모든 호출자가 "섹터버퍼 +
 *     510"을 넘기므로, 실질적으로 512바이트 섹터의 마지막 2바이트(오프셋
 *     510, 511)를 가리킨다.
 * @return: p[0]==0x55 && p[1]==0xAA 이면 참(1), 아니면 0.
 *
 * IBM PC BIOS가 부팅 가능한 디스크/파티션 테이블로 인식하는 조건과 동일한
 * 매직 넘버를 검사하는 가장 기초적인 관문 함수다. MBR 부트섹터뿐 아니라
 * EBR(parse_extended), FAT 단독 부트섹터(parse_minix에서도 재사용) 판별에
 * 공통으로 쓰인다.
 * 실행 컨텍스트: 순수 판별 함수, 부작용 없음.
 * 호출자: msdos_partition()(LBA 0 검사), parse_extended()(각 EBR 섹터
 * 검사), parse_minix()(Minix 파티션 첫 섹터가 2차 MBR인지 검사).
 * 피호출자: 없음.
 * 에러 경로: 없음 - 실패(시그니처 불일치)는 0을 반환해 호출자가 "이 포맷이
 * 아님/체인 종료"로 처리하게 한다.
 *
 * 호출 체인:
 *   msdos_partition()/parse_extended()/parse_minix() -> [msdos_magic_present()]
 */
static inline int
msdos_magic_present(unsigned char *p)
{
	/* [한국어] 오프셋 510, 511이 각각 0x55, 0xAA인지 확인 - BIOS 부트 시그니처와 동일한
	 * 검사를 재사용한다. */
	return (p[0] == MSDOS_LABEL_MAGIC1 && p[1] == MSDOS_LABEL_MAGIC2);
}

/* Value is EBCDIC 'IBMA' */
/* [한국어] AIX 디스크 레이블의 첫 4바이트 매직 - 아스키가 아니라 EBCDIC 코드값으로
 * 표현한 'IBMA' 문자열이다(원본 주석 "Value is EBCDIC 'IBMA'" 참고). MSDOS 0x55AA
 * 시그니처가 없는 일부(특히 부팅 불가능한) AIX 디스크를 aix_magic_present()가 이
 * 4바이트로 식별한다. */
#define AIX_LABEL_MAGIC1	0xC9
#define AIX_LABEL_MAGIC2	0xC2	/* [한국어] AIX 디스크 레이블 시그니처의 둘째 바이트(전체는 0xC9C2D4C1).
					 * MBR 과 같은 자리를 쓰므로, AIX 디스크를 MBR 로 오인하지 않도록 먼저 검사한다 */
#define AIX_LABEL_MAGIC3	0xD4	/* [한국어] AIX 시그니처 셋째 바이트 */
#define AIX_LABEL_MAGIC4	0xC1
/*
 * [한국어]
 * aix_magic_present() - LBA 0이 AIX 디스크 레이블인지 판별한다.
 *
 * @state: 파티션 스캔 상태(parsed_partitions). LBA 7을 추가로 읽을 때
 *         read_part_sector(state, 7, ...)로 전달된다.
 * @p: read_part_sector(state, 0, ...)로 이미 읽어 둔 LBA 0 버퍼(512바이트)의
 *     시작 주소. msdos_partition()이 호출 전에 미리 읽어 넘겨준다.
 * @return: 1이면 이 디스크가 AIX(LVM) 디스크로 확정, 0이면 아님(일반 MBR로
 *          계속 처리하거나 다른 포맷으로 진행).
 *
 * MSDOS 0x55AA 시그니처가 없는 부팅 불가능한 일부 AIX 디스크를 식별하기
 * 위해, msdos_partition()이 0x55AA 검사보다 먼저 이 함수를 호출한다.
 * 동작 과정: (1) LBA 0의 첫 4바이트가 AIX_LABEL_MAGIC1..4('IBMA'의 EBCDIC
 * 코드)와 일치하는지 확인, 아니면 즉시 0. (2) 이미 리눅스 계열 파티션
 * (Solaris x86, Linux RAID/Data/LVM, 확장 파티션)이 4개 기본 파티션 슬롯
 * 중 하나에서라도 발견되면, 이 디스크는 AIX가 아니라 이미 유효한 리눅스
 * 파티션 테이블을 가진 것으로 보고 0을 반환한다(오판 방지 - 옛 Solaris/x86과
 * Linux swap이 같은 0x82 표시자를 공유하는 것처럼, AIX 매직 4바이트만으로
 * 확정하면 위험하기 때문). (3) 두 조건을 모두 통과하면 LBA 7을 추가로 읽어
 * '_LVM' 문자열이 있는지 확인하고, 있으면 최종적으로 AIX LVM 디스크로
 * 확정(ret=1)한다.
 * 실행 컨텍스트: msdos_partition() 진입 직후, 파티션 스캔 프로세스 컨텍스트
 * 에서 동기 실행. LBA 7 읽기는 캐시 미스 시 블로킹 I/O를 유발할 수 있다.
 * 호출자: msdos_partition()(가장 먼저 호출되는 포맷 판별 검사).
 * 피호출자: is_extended_partition(), read_part_sector(), put_dev_sector().
 * 에러 처리: LBA 7 읽기가 실패(NULL)하면 '_LVM' 문자열 검사를 건너뛰고
 * ret의 초기값 0(AIX 아님)을 그대로 반환한다 - I/O 오류를 별도로 상위에
 * 보고하지 않고 "AIX 아님"으로 단순화해서 처리한다.
 *
 * 호출 체인:
 *   msdos_partition() -> [aix_magic_present()] -> is_extended_partition(),
 *     read_part_sector(), put_dev_sector()
 */
static int aix_magic_present(struct parsed_partitions *state, unsigned char *p)
{
	/* [한국어] LBA 0 버퍼 내 오프셋 0x1be(446)부터 시작하는 파티션 테이블을 msdos_partition
	 * 배열로 캐스팅 - MBR 표준 레이아웃과 동일한 위치를 AIX 라벨 판별에도 재사용한다. */
	struct msdos_partition *pt = (struct msdos_partition *) (p + 0x1be);
	/* [한국어] LBA 7을 읽을 때 사용할 Sector 출력 파라미터(folio 참조 보관용) 선언. */
	Sector sect;
	/* [한국어] LBA 7 버퍼 내용을 가리킬 포인터 - '_LVM' 문자열 검사에 사용. */
	unsigned char *d;
	/* [한국어] slot은 4개 기본 파티션 순회 인덱스, ret은 최종 판정 결과(기본값 0=AIX 아님)로
	 * 초기화. */
	int slot, ret = 0;

	/* [한국어] LBA 0의 첫 4바이트가 AIX_LABEL_MAGIC1..4('IBMA' EBCDIC)와 정확히 일치해야
	 * 다음 단계로 진행 - 하나라도 다르면 AIX 라벨이 아니므로 즉시 0을 반환한다. */
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
	/* [한국어] 4개 기본 파티션 슬롯을 순회하며, 이미 알려진 Linux/Solaris x86 파티션
	 * 타입이나 확장 파티션이 하나라도 있으면 이 디스크는 AIX가 아니라 이미 유효한 MBR로
	 * 보고 즉시 0을 반환한다(원본 주석 참고 - Solaris x86과 Linux swap이 같은
	 * sys_ind=0x82를 공유하므로 이를 Linux로 간주해 안전 측으로 처리). */
	for (slot = 1; slot <= 4; slot++, pt++) {
		/* [한국어] 이 슬롯이 Solaris x86/Linux RAID/Linux Data/Linux LVM 타입이거나 확장
		 * 파티션이면 이미 유효한(비-AIX) 파티션 테이블로 판정한다. */
		if (pt->sys_ind == SOLARIS_X86_PARTITION ||
		    pt->sys_ind == LINUX_RAID_PARTITION ||
		    pt->sys_ind == LINUX_DATA_PARTITION ||
		    pt->sys_ind == LINUX_LVM_PARTITION ||
		    is_extended_partition(pt))
			/* [한국어] AIX 판정을 포기하고 즉시 0을 반환 - 4개 슬롯 검사를 중단한다. */
			return 0;
	}
	/* [한국어] LBA 7을 read_part_sector()로 읽는다 - AIX/LVM 디스크는 관례적으로 이 위치에
	 * '_LVM' 식별 문자열을 둔다. 캐시 미스 시 하위 블록 드라이버의 읽기 경로가 동작한다. */
	d = read_part_sector(state, 7, &sect);
	/* [한국어] 읽기에 성공한 경우에만(NULL이 아닐 때) 내용을 검사 - 실패 시 이 블록
	 * 전체를 건너뛰고 ret=0을 유지한다. */
	if (d) {
		/* [한국어] 버퍼의 첫 4바이트가 정확히 '_LVM' 문자열인지 확인 - AIX LVM(Logical
		 * Volume Manager) 디스크의 식별 표지. */
		if (d[0] == '_' && d[1] == 'L' && d[2] == 'V' && d[3] == 'M')
			/* [한국어] AIX LVM 디스크로 최종 확정. */
			ret = 1;
		/* [한국어] LBA 7 버퍼의 folio 참조를 반납한다. */
		put_dev_sector(sect);
	}
	/* [한국어] 판정 결과를 msdos_partition()에 반환 - 1이면 AIX 파티션 처리 경로
	 * (#ifdef CONFIG_AIX_PARTITION)로, 0이면 일반 MBR 파싱을 계속한다. */
	return ret;
}

/*
 * [한국어]
 * set_info() - MBR 디스크 시그니처를 이용해 파티션의 UUID 메타 정보를 채운다.
 *
 * @state: 대상 파티션 스캔 상태. state->parts[slot].info/has_info이 갱신
 *         대상이다.
 * @slot: 메타 정보를 채울 parts[] 슬롯 번호(파티션 번호).
 * @disksig: MBR 오프셋 0x1b8에서 읽은 4바이트 디스크 시그니처
 *           (msdos_partition()이 le32_to_cpup()으로 미리 읽어 전달).
 * @return: 없음(void).
 *
 * GPT(efi.c)가 128비트 진짜 GUID를 파티션마다 갖는 것과 달리, MBR은 디스크
 * 전체에 4바이트 시그니처 하나만 갖는다. 이 함수는 그 4바이트와 파티션
 * 번호(slot)를 조합해 "disksig-slot" 형태의 유사 UUID 문자열을 만들어
 * /dev/disk/by-uuid 등 사용자 공간 도구가 파티션을 안정적으로 지칭할 수
 * 있게 한다. 이는 실제 RFC 4122 UUID가 아니라 커널이 관례적으로 사용하는
 * 합성 식별자다.
 * 동작 과정: (1) snprintf()로 "%08x-%02x" 형식 문자열을 info->uuid에 기록
 * (2) volname은 MBR에 대응 개념이 없으므로 빈 문자열로 설정 (3) has_info를
 * true로 세팅해 core.c의 add_partition()이 이 메타 정보를 block_device에
 * 반영하도록 표시한다.
 * 실행 컨텍스트: 파티션 스캔 프로세스 컨텍스트, state->parts[slot]만
 * 국소적으로 갱신하므로 다른 슬롯과 간섭 없이 안전하다.
 * 호출자: msdos_partition()(1st pass 기본 파티션 등록 직후),
 * parse_extended()(EBR 체인의 논리 파티션 등록 직후).
 * 피호출자: snprintf().
 * 에러 경로: 없음(항상 성공, 버퍼 크기가 고정 상수라 오버플로 우려가 없다).
 *
 * 호출 체인:
 *   msdos_partition()/parse_extended() -> [set_info()] -> snprintf()
 */
static void set_info(struct parsed_partitions *state, int slot,
		     u32 disksig)
{
	/* [한국어] 갱신 대상 partition_meta_info 구조체(uuid/volname 문자열 필드)를 가리키는
	 * 포인터를 미리 얻어둔다. */
	struct partition_meta_info *info = &state->parts[slot].info;

	/* [한국어] disksig(4바이트)와 slot(파티션 번호)을 "%08x-%02x" 형식으로 합성해 유사
	 * UUID 문자열을 만든다(예: "12ab34cd-01"). */
	snprintf(info->uuid, sizeof(info->uuid), "%08x-%02x", disksig,
		 slot);
	/* [한국어] MBR에는 파티션 이름/볼륨 라벨 개념이 없으므로 빈 문자열로 둔다. */
	info->volname[0] = 0;
	/* [한국어] core.c의 add_partition()이 이 메타 정보를 block_device의 PARTUUID 등으로
	 * 노출하도록 플래그를 세운다. */
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
 * [한국어]
 * parse_extended() - 확장 파티션의 EBR(Extended Boot Record) 체인을 따라가며 논리 파티션을 등록한다.
 *
 * @state: 파티션 스캔 상태. 등록된 논리 파티션은 state->parts[state->next]에
 *         쌓이고 state->next가 순차적으로 증가한다.
 * @first_sector: 최상위 확장 파티션 자체의 시작 LBA(msdos_partition()의 1st
 *                pass에서 start_sect(p)*sector_size로 계산해 전달).
 * @first_size: 최상위 확장 파티션 전체의 길이(섹터 수). EBR 체인이 벗어나면
 *              안 되는 상한 범위로 쓰인다.
 * @disksig: msdos_partition()이 읽어 둔 MBR 디스크 시그니처. 각 논리
 *           파티션의 set_info()에 그대로 전달되어 UUID 메타 정보를 만든다.
 * @return: 없음(void). 에러/체인 종료는 조용히 return으로 빠져나온다.
 *
 * 확장 파티션은 그 자체로는 데이터를 담지 않고, 시작 지점에 있는 EBR(일반
 * MBR과 동일한 512바이트 포맷이지만 오프셋 0x1be의 4개 엔트리 중 보통 앞
 * 2개만 유효하게 씀)을 통해 "실제 데이터 파티션 1개 + 다음 EBR로의 링크"를
 * 연결 리스트 형태로 표현한다. 이 함수는 이 링크드 리스트를 순회하며 논리
 * 파티션(5번부터 시작하는 파티션 번호)을 계속 등록한다.
 * 동작 과정: this_sector/this_size로 "현재 살펴보는 EBR/확장 영역"을
 * 추적하면서 무한 루프를 돈다. 매 회전마다 (1) loopct가 100을 넘으면
 * 손상된 순환 체인으로 보고 중단 (2) state->next가 limit에 도달하면 중단
 * (3) this_sector의 EBR을 읽고 0x55AA 시그니처를 확인 (4) EBR 내 4개
 * 엔트리 중 데이터 파티션(확장 타입이 아니고 크기가 0이 아닌 것)을 찾아
 * put_partition()으로 등록 - 3, 4번 엔트리는 쓰레기 값일 수 있어
 * first_sector/first_size 범위 검증을 추가로 거친다 (5) 나머지 엔트리 중
 * 다음 확장 파티션(링크)을 찾아 this_sector/this_size를 갱신하고 루프를
 * 계속한다 - 못 찾으면(i==4) done으로 종료.
 * 실행 컨텍스트: msdos_partition() 호출 중 파티션 스캔 프로세스 컨텍스트,
 * EBR마다 read_part_sector()가 캐시 미스 시 블로킹 I/O를 유발할 수 있다.
 * 호출자: msdos_partition()(1st pass에서 확장 파티션을 발견했을 때).
 * 피호출자: read_part_sector(), put_dev_sector(), msdos_magic_present(),
 * nr_sects(), start_sect(), is_extended_partition(), put_partition(),
 * set_info(), queue_logical_block_size().
 * 에러 처리: 순환 체인(loopct>100), 슬롯 소진(next==limit), 읽기 실패
 * (!data), 시그니처 불일치(goto done) 네 가지 경로 모두 명시적 에러 코드
 * 없이 조용히 함수를 빠져나온다 - 이미 등록된 파티션까지 무효화하지는
 * 않고, "여기까지 찾은 것"으로 마무리한다.
 *
 * 호출 체인:
 *   msdos_partition() -> [parse_extended()] -> read_part_sector(),
 *     msdos_magic_present(), nr_sects(), start_sect(),
 *     is_extended_partition(), put_partition(), set_info(), put_dev_sector()
 */
static void parse_extended(struct parsed_partitions *state,
			   sector_t first_sector, sector_t first_size,
			   u32 disksig)
{
	/* [한국어] EBR 내 파티션 테이블 엔트리를 가리킬 포인터. */
	struct msdos_partition *p;
	/* [한국어] read_part_sector()가 채울 folio 참조 보관용. */
	Sector sect;
	/* [한국어] 현재 읽은 EBR 512바이트 버퍼의 시작 주소. */
	unsigned char *data;
	/* [한국어] 현재 순회 중인 EBR의 절대 LBA(this_sector)와 그 EBR이 속한 확장 영역의
	 * 길이(this_size). */
	sector_t this_sector, this_size;
	/* [한국어] 논리 블록 크기를 512로 나눈 배수 - MBR의 512B 섹터 단위 필드를 실제
	 * 디스크 LBA 단위로 환산하는 계수. */
	sector_t sector_size;
	/* [한국어] EBR 체인 순환/손상 방지 카운터 - 아래에서 100을 넘으면 강제 종료한다
	 * (원본 주석: 데이터 파티션을 찾지 못한 채 따라간 링크 수). */
	int loopct = 0;		/* number of links followed
				   without finding a data partition */
	/* [한국어] EBR의 4개 엔트리를 순회하는 인덱스. */
	int i;

	/* [한국어] 디스크의 논리 블록 크기를 조회해 512로 나눈다 - 논리 블록이 4096B인
	 * 디스크에서는 이 값이 8이 되어, 이후 모든 start_sect()*sector_size 계산이 실제
	 * LBA로 환산된다. */
	sector_size = queue_logical_block_size(state->disk->queue) / 512;
	/* [한국어] 순회 시작점을 확장 파티션 자신의 시작 LBA로 초기화. */
	this_sector = first_sector;
	/* [한국어] 순회 시작 시점의 유효 범위를 확장 파티션 전체 크기로 초기화. */
	this_size = first_size;

	/* [한국어] EBR 체인을 끝까지(또는 이상 상황까지) 순회하는 무한 루프 - 종료는 아래 각
	 * return/goto done으로만 이뤄진다. */
	while (1) {
		/* [한국어] 링크를 100회 초과 따라갔다면 순환 참조 등으로 손상된 체인으로 간주한다. */
		if (++loopct > 100)
			/* [한국어] 이미 등록된 파티션은 유지한 채 더 이상 진행하지 않고 함수를 종료한다. */
			return;
		/* [한국어] parts[] 배열의 다음 빈 슬롯이 상한(limit)에 도달했다면 더 등록할 공간이 없다. */
		if (state->next == state->limit)
			/* [한국어] 슬롯 소진으로 조용히 종료한다. */
			return;
		/* [한국어] 현재 EBR 섹터(this_sector)를 읽는다 - 캐시 미스 시 하위 블록 드라이버
		 * 읽기 경로가 동작한다. */
		data = read_part_sector(state, this_sector, &sect);
		/* [한국어] 메모리 확보 실패 또는 디스크 끝을 넘는 LBA 등으로 읽기에 실패한 경우. */
		if (!data)
			/* [한국어] 이 시점까지 찾은 파티션만 유지하고 체인 탐색을 중단한다. */
			return;

		/* [한국어] EBR도 일반 MBR과 동일하게 마지막 2바이트에 0x55AA 시그니처가 있어야
		 * 유효하다. */
		if (!msdos_magic_present(data + 510))
			/* [한국어] 시그니처가 없으면 손상된 EBR로 보고 이번 섹터의 folio만 반납한 뒤
			 * 종료한다(아래 done 레이블로 점프). */
			goto done;

		/* [한국어] EBR 내 파티션 테이블도 표준 MBR과 동일하게 오프셋 0x1be에 위치한다. */
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
		/* [한국어] EBR의 4개 엔트리를 순회 - 통상 0번=데이터, 1번=다음 링크이지만 DRDOS/
		 * OS2 등 예외가 있어 4개 모두 검사한다(위 원본 주석 참고). */
		for (i = 0; i < 4; i++, p++) {
			/* [한국어] 이번 엔트리의 상대 오프셋/크기와, this_sector 기준으로 환산한 절대
			 * LBA(next)를 담을 지역 변수. */
			sector_t offs, size, next;

			/* [한국어] 크기가 0(빈 엔트리)이거나 확장 파티션(다음 링크)이면 데이터
			 * 파티션이 아니므로 건너뛴다. */
			if (!nr_sects(p) || is_extended_partition(p))
				continue;

			/* Check the 3rd and 4th entries -
			   these sometimes contain random garbage */
			/* [한국어] 엔트리의 시작 오프셋은 this_sector(현재 EBR) 기준 상대값이므로
			 * sector_size를 곱해 실제 LBA 단위로 환산한다. */
			offs = start_sect(p)*sector_size;
			/* [한국어] 엔트리 길이도 동일하게 LBA 단위로 환산한다. */
			size = nr_sects(p)*sector_size;
			/* [한국어] 현재 EBR의 절대 LBA(this_sector)에 상대 오프셋을 더해 이 논리
			 * 파티션의 절대 시작 LBA를 구한다. */
			next = this_sector + offs;
			/* [한국어] 3, 4번째 엔트리(i=2,3)는 원본 주석대로 쓰레기 값이 들어 있는 경우가
			 * 흔하므로, 아래 세 조건으로 범위를 검증한다. 이 가드가 없으면 쓰레기
			 * 값이 그대로 파티션 시작/크기가 되어, 확장 영역 밖이나 디스크 밖을
			 * 가리키는 파티션이 등록된다. */
			if (i >= 2) {
				/* [한국어] 이 엔트리가 현재 확장 영역(this_size)을 벗어나면 무효. */
				if (offs + size > this_size)
					continue;
				/* [한국어] 계산된 절대 LBA가 최상위 확장 파티션의 시작보다 앞이면 무효. */
				if (next < first_sector)
					continue;
				/* [한국어] 최상위 확장 파티션의 끝을 넘어서면 무효. */
				if (next + size > first_sector + first_size)
					continue;
			}

			/* [한국어] 검증을 통과한 논리 파티션을 state->next 슬롯에 (절대 LBA, 길이)로
			 * 등록한다. */
			put_partition(state, state->next, next, size);
			/* [한국어] 같은 슬롯에 MBR disksig 기반 UUID 메타 정보를 채운다. */
			set_info(state, state->next, disksig);
			/* [한국어] 이 논리 파티션이 Linux RAID 멤버로 표시되어 있다면. */
			if (p->sys_ind == LINUX_RAID_PARTITION)
				/* [한국어] RAID 오토디텍트 힌트 플래그를 세팅한다. */
				state->parts[state->next].flags = ADDPART_FLAG_RAID;
			/* [한국어] 데이터 파티션을 찾았으므로 순환 카운터를 리셋한다(원본 주석: '데이터
			 * 파티션 없이 따라간 링크 수'이므로 찾으면 0으로). */
			loopct = 0;
			/* [한국어] 방금 등록으로 슬롯을 모두 소진했다면. */
			if (++state->next == state->limit)
				/* [한국어] 더 등록할 수 없으므로 done으로 이동해 현재 EBR 버퍼를 반납하고
				 * 종료한다. */
				goto done;
		}
		/*
		 * Next, process the (first) extended partition, if present.
		 * (So far, there seems to be no reason to make
		 *  parse_extended()  recursive and allow a tree
		 *  of extended partitions.)
		 * It should be a link to the next logical partition.
		 */
		/* [한국어] 위 루프에서 4번 전진한 p를 다시 엔트리 0번 위치로 되돌린다. */
		p -= 4;
		/* [한국어] 이번엔 데이터 파티션이 아니라 다음 EBR로의 링크(확장 파티션 타입 엔트리)를
		 * 찾는다. */
		for (i = 0; i < 4; i++, p++)
			/* [한국어] 크기가 0이 아니면서 확장 파티션 타입인 엔트리가 링크 후보. */
			if (nr_sects(p) && is_extended_partition(p))
				/* [한국어] 찾으면 즉시 루프를 벗어난다(원본 주석: 첫 번째 확장 파티션
				 * 링크만 처리, 트리 구조는 지원하지 않음). */
				break;
		/* [한국어] 4개 엔트리를 모두 순회했지만 링크를 찾지 못한 경우. */
		if (i == 4)
			/* [한국어] 더 이상 이어질 EBR이 없으므로 체인 탐색을 여기서 종료한다. */
			goto done;	 /* nothing left to do */

		/* [한국어] 다음 EBR의 절대 LBA를 계산한다 - 링크 엔트리의 start_sect는 최상위
		 * 확장 파티션(first_sector) 기준 상대값이다. */
		this_sector = first_sector + start_sect(p) * sector_size;
		/* [한국어] 다음 확장 영역의 유효 길이를 갱신한다 - 다음 루프 회전의 3/4번 엔트리
		 * 범위 검증에 쓰인다. */
		this_size = nr_sects(p) * sector_size;
		/* [한국어] 현재 EBR 버퍼의 folio 참조를 반납한다 - 다음 루프에서 새 EBR을 읽기 전에
		 * 정리. */
		put_dev_sector(sect);
	}
	/* [한국어] 정상/이상 종료 공통 지점 - 마지막으로 읽은 EBR 버퍼만 정리하고 함수를
	 * 마친다. */
done:
	/* [한국어] done으로 도달한 경우에도 마지막으로 읽은 EBR의 folio 참조를 반드시 반납한다. */
	put_dev_sector(sect);
}

/* [한국어] Solaris x86 VTOC(Virtual Table Of Contents) 관련 상수. SOLARIS_X86_NUMSLICE
 * (16)는 v_slice[] 배열의 최대 원소 수(슬라이스 = Solaris 용어의 파티션), 
 * SOLARIS_X86_VTOC_SANE(0x600DDEEE, "sane"과 발음이 비슷한 말장난 매직)는 VTOC가
 * 유효한지 확인하는 sanity 매직 넘버다. */
#define SOLARIS_X86_NUMSLICE	16
#define SOLARIS_X86_VTOC_SANE	(0x600DDEEEUL)	/* [한국어] Solaris x86 VTOC 가 유효함을 나타내는 매직값.
						 * 16진수를 읽으면 "GOOD DEEE" 로 보이는 의도적 배치다.
						 * 아래 UNIXWARE_DISKMAGIC2 와 값이 같은데, 두 포맷이 같은 조상에서
						 * 갈라져 나왔기 때문이다 — 그래서 값만으로는 구분되지 않고
						 * 파티션 타입 바이트로 먼저 갈라야 한다 */

/*
 * [한국어] Solaris x86 VTOC 슬라이스(슬라이스 = Solaris 용어의 파티션) 온디스크
 * 엔트리. v_slice[] 배열의 원소 타입으로, 각 슬라이스가 담당하는 LBA 범위와
 * 태그/권한 플래그를 기술한다. 이 구조체 자체는 파일시스템 접근 없이 순수하게
 * 온디스크 바이트 레이아웃만 표현하는 값 타입이다.
 */
struct solaris_x86_slice {
	__le16 s_tag;		/* ID tag of partition */
	/* [한국어] 슬라이스 용도 태그(파일시스템 종류: root/swap/usr 등을 나타내는 값).
	 * 설정자: Solaris 설치 도구가 디스크에 기록. 읽는 자: 이 파일은 값을 해석하지
	 * 않고 구조체 레이아웃 정합성 목적으로만 필드를 유지한다. 값 범위: Solaris VTOC
	 * 스펙의 태그 상수. 동기화: 읽기 전용 온디스크 값, 별도 동기화 불필요. */
	__le16 s_flag;		/* permission flags */
	/* [한국어] 슬라이스 접근 권한 플래그(읽기전용 등).
	 * 설정자: Solaris 설치 도구. 읽는 자: 이 파일은 사용하지 않고 필드만 보존한다.
	 * 값 범위: Solaris VTOC 스펙의 플래그 비트. 동기화: 온디스크 읽기 전용 값. */
	__le32 s_start;		/* start sector no of partition */
	/* [한국어] 슬라이스(파티션) 시작 섹터 번호.
	 * 설정자: Solaris 파티셔닝 도구가 디스크에 기록. 읽는 자: parse_solaris_x86()이
	 * le32_to_cpu(s->s_start)+offset으로 읽어 put_partition()의 시작 LBA 인자로
	 * 사용한다(offset은 이 슬라이스를 담은 상위 MS-DOS 파티션 기준 오프셋).
	 * 값 범위: 0 이상, 현재 MS-DOS 파티션 기준 상대 섹터. 동기화: 스캔 스레드
	 * 단독 접근. */
	__le32 s_size;		/* # of blocks in partition */
	/* [한국어] 슬라이스(파티션) 길이(블록 수).
	 * 설정자: Solaris 파티셔닝 도구. 읽는 자: parse_solaris_x86()이 0이면 미사용
	 * 슬라이스로 건너뛰고, 아니면 put_partition()의 길이 인자로 사용한다.
	 * 값 범위: 0(미사용)~디스크 크기 이하. 동기화: 스캔 스레드 단독 접근. */
};

/*
 * [한국어] Solaris x86 VTOC(Volume Table Of Contents) 메타데이터 구조체. 하나의
 * MS-DOS 파티션(sys_ind == SOLARIS_X86_PARTITION 또는 NEW_SOLARIS_X86_PARTITION)
 * 내부에 중첩되어, 그 파티션을 다시 최대 SOLARIS_X86_NUMSLICE(16)개의 슬라이스로
 * 세분하는 서브 레이블이다. v_sanity/v_version은 이 구조체가 진짜 Solaris VTOC인지
 * 검증하는 매직/버전 필드, v_slice[]가 실제 슬라이스 배열이다.
 */
struct solaris_x86_vtoc {
	unsigned int v_bootinfo[3];	/* info needed by mboot */
	/* [한국어] Solaris 부트로더(mboot)가 쓰는 부트 정보 3워드.
	 * 설정자: Solaris 설치 도구. 읽는 자: 이 파일은 사용하지 않고 레이아웃 정합성
	 * 목적으로만 유지한다. 동기화: 온디스크 읽기 전용 값. */
	__le32 v_sanity;		/* to verify vtoc sanity */
	/* [한국어] VTOC 무결성 sanity 매직 넘버.
	 * 설정자: Solaris 파티셔닝 도구가 SOLARIS_X86_VTOC_SANE 값으로 기록. 읽는 자:
	 * parse_solaris_x86()이 le32_to_cpu(v->v_sanity) != SOLARIS_X86_VTOC_SANE 이면
	 * 이 섹터를 Solaris VTOC가 아닌 것으로 보고 즉시 파싱을 중단한다. 값 범위:
	 * SOLARIS_X86_VTOC_SANE(0x600DDEEE)이어야 유효. 동기화: 온디스크 읽기 전용 값. */
	__le32 v_version;		/* layout version */
	/* [한국어] VTOC 레이아웃 버전 번호.
	 * 설정자: Solaris 파티셔닝 도구. 읽는 자: parse_solaris_x86()이 1이 아니면 처리할
	 * 수 없는 버전이라는 로그를 남기고 중단한다(이 파일은 버전 1만 지원). 값 범위:
	 * 정수 버전 번호, 이 파일이 인식하는 값은 1뿐. 동기화: 온디스크 읽기 전용 값. */
	char	v_volume[8];		/* volume name */
	/* [한국어] 볼륨 이름 문자열(최대 8바이트).
	 * 설정자: Solaris 설치 도구. 읽는 자: 이 파일은 사용하지 않는다. 동기화: 온디스크
	 * 읽기 전용 값. */
	__le16	v_sectorsz;		/* sector size in bytes */
	/* [한국어] 섹터 크기(바이트).
	 * 설정자: Solaris 파티셔닝 도구. 읽는 자: 이 파일은 사용하지 않고(디스크의 논리
	 * 블록 크기는 이미 queue_logical_block_size()로 별도 확인) 레이아웃 정합성
	 * 목적으로만 유지한다. 동기화: 온디스크 읽기 전용 값. */
	__le16	v_nparts;		/* number of partitions */
	/* [한국어] 이 VTOC에 기록된 슬라이스(파티션) 개수.
	 * 설정자: Solaris 파티셔닝 도구. 읽는 자: parse_solaris_x86()이
	 * le16_to_cpu(v->v_nparts)를 8과 비교해 max_nparts(순회 상한)를 정한다 - 8
	 * 초과면 SOLARIS_X86_NUMSLICE(16)까지, 아니면 구버전 호환을 위해 8로 제한한다.
	 * 값 범위: 0~SOLARIS_X86_NUMSLICE. 동기화: 온디스크 읽기 전용 값. */
	unsigned int v_reserved[10];	/* free space */
	/* [한국어] 예약 영역(향후 확장용).
	 * 설정자: 사용 안 함(0 또는 임의값). 읽는 자: 이 파일은 사용하지 않는다.
	 * 동기화: 온디스크 읽기 전용 값. */
	struct solaris_x86_slice
		/* [한국어] 실제 슬라이스 배열 - 최대 SOLARIS_X86_NUMSLICE(16)개.
		 * 설정자: Solaris 파티셔닝 도구. 읽는 자: parse_solaris_x86()이 max_nparts개까지
		 * 순회하며 s_size!=0인 항목을 put_partition()으로 등록한다. 값 범위: 각 원소는
		 * struct solaris_x86_slice 필드 설명 참고. 동기화: 온디스크 읽기 전용 값. */
		v_slice[SOLARIS_X86_NUMSLICE]; /* slice headers */
	unsigned int timestamp[SOLARIS_X86_NUMSLICE]; /* timestamp */
	/* [한국어] 슬라이스별 타임스탬프 배열.
	 * 설정자: Solaris 파티셔닝 도구. 읽는 자: 이 파일은 사용하지 않는다. 동기화:
	 * 온디스크 읽기 전용 값. */
	char	v_asciilabel[128];	/* for compatibility */
	/* [한국어] 구버전 호환용 ASCII 레이블 문자열(128바이트).
	 * 설정자: Solaris 파티셔닝 도구. 읽는 자: 이 파일은 사용하지 않는다. 동기화:
	 * 온디스크 읽기 전용 값. */
};

/* james@bpgc.com: Solaris has a nasty indicator: 0x82 which also
   indicates linux swap.  Be careful before believing this is Solaris. */

/*
 * [한국어]
 * parse_solaris_x86() - 하나의 MS-DOS 파티션 안에 중첩된 Solaris x86 VTOC를 파싱한다.
 *
 * @state: 파티션 스캔 상태.
 * @offset: 이 VTOC가 속한 MS-DOS 파티션의 시작 LBA(msdos_partition()의 2nd
 *          pass가 start_sect(p)*sector_size로 전달).
 * @size: 사용하지 않음(다른 서브 파서와 동일한 함수 포인터 시그니처를 맞추기
 *        위한 매개변수 - subtypes[] 테이블의 parse 콜백 타입 참고).
 * @origin: 이 VTOC를 담고 있는 부모 MS-DOS 파티션의 번호(1~4). 로그 문자열과
 *          put_partition()에는 직접 쓰이지 않고 seq_buf 로그 헤더에만 쓰인다.
 * @return: 없음(void). CONFIG_SOLARIS_X86_PARTITION 비활성 시 빈 함수.
 *
 * MS-DOS 파티션 타입이 SOLARIS_X86_PARTITION/NEW_SOLARIS_X86_PARTITION일 때
 * msdos_partition()의 2nd pass가 subtypes[] 테이블을 통해 이 함수를 호출해,
 * 그 파티션 내부를 다시 최대 16개의 Solaris 슬라이스로 세분한다.
 * 동작 과정: (1) offset+1 섹터에서 VTOC를 읽는다(관례상 VTOC는 파티션 시작
 * 다음 섹터에 위치) (2) v_sanity 매직을 검증, 불일치 시 중단 (3) 로그
 * 헤더를 출력 (4) v_version이 1이 아니면 처리할 수 없다는 로그만 남기고
 * 중단(이 파일은 버전 1만 지원) (5) v_nparts에 따라 순회 상한(max_nparts)을
 * 정하고, 각 슬라이스의 s_size가 0이 아니면 offset을 더해 절대 LBA로 환산한
 * 뒤 put_partition()으로 등록한다.
 * 실행 컨텍스트: msdos_partition() 2nd pass 호출 중, 파티션 스캔 프로세스
 * 컨텍스트.
 * 호출자: msdos_partition()(subtypes[] 테이블을 통해 함수 포인터로 호출).
 * 피호출자: read_part_sector(), put_dev_sector(), put_partition(),
 * seq_buf_printf(), seq_buf_puts().
 * 에러 처리: 읽기 실패(!v), sanity 불일치, 버전 불일치 세 경로 모두 조용히
 * return으로 종료 - 이미 등록된 (부모) 파티션에는 영향을 주지 않는다.
 *
 * 호출 체인:
 *   msdos_partition() -> subtypes[].parse -> [parse_solaris_x86()] ->
 *     read_part_sector(), put_partition(), put_dev_sector()
 */
static void parse_solaris_x86(struct parsed_partitions *state,
			      sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_SOLARIS_X86_PARTITION
	/* [한국어] read_part_sector()가 채울 folio 참조 보관용. */
	Sector sect;
	/* [한국어] 읽어들인 VTOC 버퍼를 가리킬 포인터. */
	struct solaris_x86_vtoc *v;
	/* [한국어] v_slice[] 순회 인덱스. */
	int i;
	/* [한국어] 실제로 순회할 슬라이스 개수 상한(구버전 8개 VTOC와 신버전 16개 VTOC
	 * 호환 목적). */
	short max_nparts;

	/* [한국어] VTOC는 해당 MS-DOS 파티션의 offset + 1 섹터에 위치한다(관례). */
	v = read_part_sector(state, offset + 1, &sect);
	/* [한국어] 읽기 실패/메모리 부족 시 VTOC 파싱을 중단한다. */
	if (!v)
		return;
	/* [한국어] VTOC 무결성 sanity 매직 값 검사 - 일치하지 않으면 Solaris VTOC가
	 * 아니다. */
	if (le32_to_cpu(v->v_sanity) != SOLARIS_X86_VTOC_SANE) {
		/* [한국어] VTOC 섹터 버퍼를 해제하고 중단한다. */
		put_dev_sector(sect);
		return;
	}
	/* [한국어] 로그 버퍼에 " <파티션번호>: <solaris:" 헤더를 기록해 이후 등록되는 슬라이스
	 * 목록을 이 헤더 아래에 이어붙일 준비를 한다. */
	seq_buf_printf(&state->pp_buf, " %s%d: <solaris:", state->name, origin);
	/* [한국어] 이 파일이 지원하는 VTOC 레이아웃 버전은 1뿐이다. */
	if (le32_to_cpu(v->v_version) != 1) {
		/* [한국어] 처리할 수 없는 버전임을 로그로 남긴다. */
		seq_buf_printf(&state->pp_buf,
			       "  cannot handle version %d vtoc>\n",
			       le32_to_cpu(v->v_version));
		/* [한국어] VTOC 섹터 버퍼를 해제한다. */
		put_dev_sector(sect);
		return;
	}
	/* Ensure we can handle previous case of VTOC with 8 entries gracefully */
	/* [한국어] v_nparts가 8을 넘으면 신버전(최대 16개)으로, 아니면 구버전 호환을 위해
	 * 8개로 순회 상한을 제한한다(원본 주석 참고). */
	max_nparts = le16_to_cpu(v->v_nparts) > 8 ? SOLARIS_X86_NUMSLICE : 8;
	/* [한국어] max_nparts개(또는 parts[] 슬롯이 먼저 소진될 때까지) 슬라이스를 순회한다. */
	for (i = 0; i < max_nparts && state->next < state->limit; i++) {
		/* [한국어] i번째 슬라이스 헤더를 가리키는 지역 포인터. */
		struct solaris_x86_slice *s = &v->v_slice[i];

		/* [한국어] 길이가 0이면 미사용 슬라이스이므로 건너뛴다. */
		if (s->s_size == 0)
			continue;
		/* [한국어] 로그에 " [sN]" 형태로 이 슬라이스의 인덱스를 남긴다. */
		seq_buf_printf(&state->pp_buf, " [s%d]", i);
		/* solaris partitions are relative to current MS-DOS
		 * one; must add the offset of the current partition */
		/* [한국어] Solaris 슬라이스의 s_start는 현재 MS-DOS 파티션 기준 상대 섹터이므로,
		 * 디스크 절대 LBA로 환산하기 위해 offset(부모 파티션의 절대 시작 LBA)을 더한다. */
		put_partition(state, state->next++,
				 le32_to_cpu(s->s_start)+offset,
				 le32_to_cpu(s->s_size));
	}
	/* [한국어] VTOC 섹터 버퍼를 해제한다. */
	put_dev_sector(sect);
	/* [한국어] 이 파티션의 로그 라인을 " >\n"으로 닫는다. */
	seq_buf_puts(&state->pp_buf, " >\n");
#endif
}

/* check against BSD src/sys/sys/disklabel.h for consistency */
/* [한국어] BSD 계열(FreeBSD/NetBSD/OpenBSD)이 공유하는 disklabel 매직/상수.
 * BSD_DISKMAGIC(0x82564557)는 disklabel 유효성 매직, BSD_MAXPARTITIONS/
 * OPENBSD_MAXPARTITIONS(둘 다 16)는 d_partitions[] 배열 크기, BSD_FS_UNUSED(0)는
 * 미사용 파티션 엔트리 표시값이다. */
#define BSD_DISKMAGIC	(0x82564557UL)	/* The disk magic number */
#define BSD_MAXPARTITIONS	16	/* [한국어] BSD 디스크 레이블 하나가 담을 수 있는 파티션(슬라이스) 최대 수.
					 * 파싱 루프의 상한이며, 레이블이 손상돼 더 큰 값을 담고 있어도
					 * 이 값으로 잘라 배열 밖 접근을 막는다 */
#define OPENBSD_MAXPARTITIONS	16	/* [한국어] OpenBSD 판. 값은 같지만 별도 상수로 둔 이유는 두 포맷이
						 * 서로 독립적으로 변할 수 있기 때문이다 */
#define BSD_FS_UNUSED		0 /* disklabel unused partition entry ID */
/*
 * [한국어] BSD disklabel 온디스크 구조체 - FreeBSD/NetBSD/OpenBSD가 공통으로 쓰는
 * 디스크 레이블 포맷이다(parse_bsd()가 flavour 문자열로 세 변종을 구분해 파싱).
 * 대부분의 필드(드라이브 기하구조: 실린더/헤드/섹터 수 등)는 이 파일의 파싱
 * 로직에서 실제로 사용되지 않고 온디스크 레이아웃을 정확히 맞추기 위한 자리
 * 채움이며, 실제로 쓰이는 것은 d_magic(유효성 검증)과 d_npartitions/
 * d_partitions[](서브 파티션 목록)뿐이다.
 */
struct bsd_disklabel {
	__le32	d_magic;		/* the magic number */
	/* [한국어] disklabel 유효성 매직 넘버.
	 * 설정자: BSD 파티셔닝 도구가 BSD_DISKMAGIC 값으로 기록. 읽는 자: parse_bsd()가
	 * le32_to_cpu(l->d_magic) != BSD_DISKMAGIC 이면 이 섹터를 BSD disklabel이 아닌
	 * 것으로 보고 즉시 중단한다. 값 범위: BSD_DISKMAGIC(0x82564557)이어야 유효.
	 * 동기화: 온디스크 읽기 전용 값. */
	__s16	d_type;			/* drive type */
	/* [한국어] 드라이브 타입 식별자(SCSI/IDE 등). 이 파일은 사용하지 않고 레이아웃
	 * 정합성 목적으로만 유지한다. */
	__s16	d_subtype;		/* controller/d_type specific */
	/* [한국어] 컨트롤러/d_type 특화 서브타입. 이 파일은 사용하지 않는다. */
	char	d_typename[16];		/* type name, e.g. "eagle" */
	/* [한국어] 드라이브 타입 이름 문자열(예: "eagle"). 이 파일은 사용하지 않는다. */
	char	d_packname[16];		/* pack identifier */
	/* [한국어] 팩(디스크 팩) 식별자 문자열. 이 파일은 사용하지 않는다. */
	__u32	d_secsize;		/* # of bytes per sector */
	/* [한국어] 섹터 크기(바이트). 이 파일은 사용하지 않고(디스크 논리 블록 크기는
	 * 별도로 queue_logical_block_size()로 확인) 레이아웃 정합성 목적으로만 유지한다. */
	__u32	d_nsectors;		/* # of data sectors per track */
	/* [한국어] 트랙당 데이터 섹터 수(CHS 기하구조). 이 파일은 사용하지 않는다. */
	__u32	d_ntracks;		/* # of tracks per cylinder */
	/* [한국어] 실린더당 트랙 수(CHS 기하구조). 이 파일은 사용하지 않는다. */
	__u32	d_ncylinders;		/* # of data cylinders per unit */
	/* [한국어] 유닛당 데이터 실린더 수. 이 파일은 사용하지 않는다. */
	__u32	d_secpercyl;		/* # of data sectors per cylinder */
	/* [한국어] 실린더당 데이터 섹터 수. 이 파일은 사용하지 않는다. */
	__u32	d_secperunit;		/* # of data sectors per unit */
	/* [한국어] 유닛(디스크) 전체 섹터 수. 이 파일은 사용하지 않는다. */
	__u16	d_sparespertrack;	/* # of spare sectors per track */
	/* [한국어] 트랙당 예비 섹터 수. 이 파일은 사용하지 않는다. */
	__u16	d_sparespercyl;		/* # of spare sectors per cylinder */
	/* [한국어] 실린더당 예비 섹터 수. 이 파일은 사용하지 않는다. */
	__u32	d_acylinders;		/* # of alt. cylinders per unit */
	/* [한국어] 유닛당 대체(alternate) 실린더 수. 이 파일은 사용하지 않는다. */
	__u16	d_rpm;			/* rotational speed */
	/* [한국어] 회전 속도(RPM, 회전 매체 전용). 이 파일은 사용하지 않는다. */
	__u16	d_interleave;		/* hardware sector interleave */
	/* [한국어] 하드웨어 섹터 인터리브. 이 파일은 사용하지 않는다. */
	__u16	d_trackskew;		/* sector 0 skew, per track */
	/* [한국어] 트랙당 섹터 0 스큐. 이 파일은 사용하지 않는다. */
	__u16	d_cylskew;		/* sector 0 skew, per cylinder */
	/* [한국어] 실린더당 섹터 0 스큐. 이 파일은 사용하지 않는다. */
	__u32	d_headswitch;		/* head switch time, usec */
	/* [한국어] 헤드 전환 시간(usec, 회전 매체 전용). 이 파일은 사용하지 않는다. */
	__u32	d_trkseek;		/* track-to-track seek, usec */
	/* [한국어] 트랙 간 탐색 시간(usec). 이 파일은 사용하지 않는다. */
	__u32	d_flags;		/* generic flags */
	/* [한국어] 일반 플래그 비트마스크(BSD 커널 정의). 이 파일은 사용하지 않는다. */
#define NDDATA 5
	/* [한국어] 아래 d_drivedata[] 배열의 크기 상수(5) - BSD 커널의 드라이브별 특화
	 * 데이터 슬롯 개수. */
	__u32	d_drivedata[NDDATA];	/* drive-type specific information */
	/* [한국어] 드라이브 타입별 특화 정보 5워드. 이 파일은 사용하지 않는다. */
#define NSPARE 5
	/* [한국어] 아래 d_spare[] 배열의 크기 상수(5) - 향후 확장을 위한 예비 슬롯 개수. */
	__u32	d_spare[NSPARE];	/* reserved for future use */
	/* [한국어] 예약 영역(향후 확장용) 5워드. 이 파일은 사용하지 않는다. */
	__le32	d_magic2;		/* the magic number (again) */
	/* [한국어] d_magic과 동일한 값을 구조체 끝부분에도 중복 기록한 두 번째 매직.
	 * 설정자: BSD 파티셔닝 도구. 읽는 자: 이 파일은 d_magic만 검증하고 d_magic2는
	 * 확인하지 않는다(레이아웃 정합성 목적으로만 유지). */
	__le16	d_checksum;		/* xor of data incl. partitions */
	/* [한국어] 파티션 정보를 포함한 데이터의 XOR 체크섬. 이 파일은 검증하지 않는다. */

			/* filesystem and partition information: */
	/* [한국어] 아래부터가 이 파일이 실제로 읽는 필드들(파일시스템/파티션 정보
	 * 섹션)이다. */
	__le16	d_npartitions;		/* number of partitions in following */
	/* [한국어] d_partitions[]에 유효하게 채워진 파티션 엔트리 개수.
	 * 설정자: BSD 파티셔닝 도구. 읽는 자: parse_bsd()가
	 * le16_to_cpu(l->d_npartitions)와 max_partitions(flavour별 상한)을 비교해 실제
	 * 순회할 엔트리 수를 이 값과 상한 중 작은 쪽으로 조정한다. 값 범위: 0~
	 * BSD_MAXPARTITIONS(16)/OPENBSD_MAXPARTITIONS(16). 동기화: 온디스크 읽기 전용
	 * 값. */
	__le32	d_bbsize;		/* size of boot area at sn0, bytes */
	/* [한국어] 부트 영역 크기(바이트, sn0 슬라이스 기준). 이 파일은 사용하지 않는다. */
	__le32	d_sbsize;		/* max size of fs superblock, bytes */
	/* [한국어] 파일시스템 슈퍼블록 최대 크기(바이트). 이 파일은 사용하지 않는다. */
	/* [한국어] 실제 BSD 파티션 테이블 - d_npartitions개(상한 max_partitions)만큼
	 * 유효한 struct bsd_partition 엔트리를 담는 배열이다. 아래 중첩 struct
	 * bsd_partition의 필드별 설명 참고. */
	struct	bsd_partition {		/* the partition table */
		__le32	p_size;		/* number of sectors in partition */
		/* [한국어] 이 BSD 파티션의 길이(섹터 수).
		 * 설정자: BSD 파티셔닝 도구. 읽는 자: parse_bsd()가 le32_to_cpu(p->p_size)로
		 * 읽어 bsd_size 지역 변수에 담고, 이후 부모 파티션 범위 검증과
		 * put_partition()의 길이 인자로 사용한다. 동기화: 온디스크 읽기 전용 값. */
		__le32	p_offset;	/* starting sector */
		/* [한국어] 이 BSD 파티션의 시작 섹터.
		 * 설정자: BSD 파티셔닝 도구. 읽는 자: parse_bsd()가 le32_to_cpu(p->p_offset)로
		 * 읽어 bsd_start에 담는다. FreeBSD의 경우 C 파티션(전체 디스크를 나타내는
		 * 슬라이스) 오프셋이 0이면 이 값이 절대가 아니라 상대 오프셋이라는 관례가
		 * 있어 parse_bsd()가 offset을 더해 보정한다. 동기화: 온디스크 읽기 전용 값. */
		__le32	p_fsize;	/* filesystem basic fragment size */
		/* [한국어] 파일시스템 기본 프래그먼트 크기. 이 파일은 사용하지 않는다. */
		__u8	p_fstype;	/* filesystem type, see below */
		/* [한국어] 파일시스템 타입 바이트.
		 * 설정자: BSD 파티셔닝 도구. 읽는 자: parse_bsd()가 BSD_FS_UNUSED(0)이면 이
		 * 엔트리를 미사용으로 보고 건너뛴다. 동기화: 온디스크 읽기 전용 값. */
		__u8	p_frag;		/* filesystem fragments per block */
		/* [한국어] 블록당 파일시스템 프래그먼트 수. 이 파일은 사용하지 않는다. */
		__le16	p_cpg;		/* filesystem cylinders per group */
		/* [한국어] 그룹당 파일시스템 실린더 수. 이 파일은 사용하지 않는다. */
	} d_partitions[BSD_MAXPARTITIONS];	/* actually may be more */
	/* [한국어] 위 struct bsd_partition 배열 필드 자체 - 이름 그대로 BSD_MAXPARTITIONS
	 * (16)개로 선언되어 있지만, 원본 주석대로 실제로는 더 많을 수 있다(disklabel이
	 * 가변 길이일 수 있음). parse_bsd()는 항상 max_partitions로 전달받은 상한과
	 * d_npartitions 중 작은 값까지만 순회해 이 배열 경계를 넘지 않도록 방어한다. */
};

#if defined(CONFIG_BSD_DISKLABEL)
/*
 * Create devices for BSD partitions listed in a disklabel, under a
 * dos-like partition. See parse_extended() for more information.
 */
/*
 * [한국어]
 * parse_bsd() - BSD 계열(FreeBSD/NetBSD/OpenBSD) disklabel의 파티션들을 등록한다.
 *
 * @state: 파티션 스캔 상태.
 * @offset: 이 disklabel이 속한 부모 MS-DOS 파티션의 시작 LBA.
 * @size: 부모 MS-DOS 파티션의 길이(섹터 수) - 부모 전체를 그대로 나타내는
 *        BSD 파티션(예: FreeBSD의 C 슬라이스)을 중복 등록하지 않기 위한 비교
 *        기준으로 쓰인다.
 * @origin: 부모 MS-DOS 파티션 번호(로그 헤더에만 사용).
 * @flavour: "bsd"/"netbsd"/"openbsd" 중 하나의 문자열 - 로그 태그 및 FreeBSD
 *           전용 상대 오프셋 보정 분기(memcmp(flavour, "bsd\0", 4))에 쓰인다.
 * @max_partitions: 순회할 최대 파티션 엔트리 수(BSD_MAXPARTITIONS 또는
 *                  OPENBSD_MAXPARTITIONS, 호출자가 종류별로 지정).
 * @return: 없음(void).
 *
 * parse_freebsd()/parse_netbsd()/parse_openbsd() 세 얇은 래퍼가 flavour
 * 문자열만 바꿔 이 함수를 호출하는 공용 구현이다(parse_extended()의 EBR과
 * 유사하게, 하나의 MS-DOS 파티션 내부에 BSD의 독자적인 서브 파티션 체계를
 * 얹는 구조).
 * 동작 과정: (1) offset+1 섹터에서 disklabel을 읽는다(관례상 disklabel은
 * 파티션 시작 다음 섹터) (2) d_magic 검증 (3) d_npartitions와
 * max_partitions 중 작은 값으로 순회 상한을 정한다 (4) d_partitions[]를
 * 순회하며 미사용(BSD_FS_UNUSED) 엔트리는 건너뛰고, FreeBSD의 상대 오프셋
 * 관례를 보정한 뒤, 부모 파티션 전체와 동일한 엔트리(이미 등록됨)는
 * 건너뛰고, 부모 범위를 벗어나는 엔트리는 손상된 것으로 보고 경고만
 * 남기고 건너뛴 뒤, 나머지를 put_partition()으로 등록한다.
 * 실행 컨텍스트: msdos_partition() 2nd pass 호출 중, 파티션 스캔 프로세스
 * 컨텍스트.
 * 호출자: parse_freebsd()/parse_netbsd()/parse_openbsd().
 * 피호출자: read_part_sector(), put_dev_sector(), put_partition(),
 * seq_buf_printf(), seq_buf_puts(), memcmp().
 * 에러 처리: 읽기 실패(!l)나 매직 불일치 시 조용히 return. 부모 범위를
 * 벗어나는 개별 서브 파티션은 전체 스캔을 중단시키지 않고 "bad
 * subpartition - ignored" 로그만 남긴다.
 *
 * 호출 체인:
 *   parse_freebsd()/parse_netbsd()/parse_openbsd() -> [parse_bsd()] ->
 *     read_part_sector(), put_partition(), put_dev_sector()
 */
static void parse_bsd(struct parsed_partitions *state,
		      sector_t offset, sector_t size, int origin, char *flavour,
		      int max_partitions)
{
	/* [한국어] read_part_sector()가 채울 folio 참조 보관용. */
	Sector sect;
	/* [한국어] 읽어들인 BSD disklabel 버퍼를 가리킬 포인터. */
	struct bsd_disklabel *l;
	/* [한국어] d_partitions[] 순회용 포인터. */
	struct bsd_partition *p;

	/* [한국어] offset + 1 LBA를 읽어 BSD disklabel을 획득한다 - 캐시 미스 시 하위
	 * 블록 드라이버 읽기 경로가 동작한다. */
	l = read_part_sector(state, offset + 1, &sect);
	/* [한국어] 메모리 할당/읽기 실패 시 서브 파티션 스캔을 중단한다. */
	if (!l)
		return;
	/* [한국어] BSD disklabel 매직 값 검증 - 일치하지 않으면 BSD disklabel이 아니다. */
	if (le32_to_cpu(l->d_magic) != BSD_DISKMAGIC) {
		/* [한국어] disklabel 버퍼를 해제하고 중단한다. */
		put_dev_sector(sect);
		return;
	}

	/* [한국어] 로그 버퍼에 " <파티션번호>: <flavour:" 헤더를 기록한다(flavour는
	 * "bsd"/"netbsd"/"openbsd"). */
	seq_buf_printf(&state->pp_buf, " %s%d: <%s:", state->name, origin, flavour);

	/* [한국어] disklabel에 기술된 실제 파티션 수(d_npartitions)가 flavour별 상한보다
	 * 작으면, 그 실제 값으로 순회 상한을 낮춘다(과도한 순회로 잘못된 메모리를 읽지
	 * 않도록). */
	if (le16_to_cpu(l->d_npartitions) < max_partitions)
		max_partitions = le16_to_cpu(l->d_npartitions);
	/* [한국어] d_partitions[]를 처음부터 max_partitions개까지 순회한다. */
	for (p = l->d_partitions; p - l->d_partitions < max_partitions; p++) {
		/* [한국어] 이번 엔트리의 시작/길이를 담을 지역 변수. */
		sector_t bsd_start, bsd_size;

		/* [한국어] parts[] 슬롯이 소진되었다면 더 등록할 수 없으므로 순회를 중단한다. */
		if (state->next == state->limit)
			break;
		/* [한국어] 미사용으로 표시된 엔트리는 건너뛴다. */
		if (p->p_fstype == BSD_FS_UNUSED)
			continue;
		/* [한국어] p_offset을 sector_t로 읽어 시작 위치 후보를 얻는다. */
		bsd_start = le32_to_cpu(p->p_offset);
		/* [한국어] p_size를 sector_t로 읽어 길이 후보를 얻는다. */
		bsd_size = le32_to_cpu(p->p_size);
		/* FreeBSD has relative offset if C partition offset is zero */
		/* [한국어] FreeBSD 관례: flavour가 "bsd"이고 C 파티션(인덱스 2, 전체 디스크를
		 * 나타내는 슬라이스)의 오프셋이 0이면, 이 disklabel의 모든 오프셋이 절대값이
		 * 아니라 부모 MS-DOS 파티션 기준 상대값이라는 뜻이므로 offset을 더해 절대
		 * LBA로 보정한다. */
		if (memcmp(flavour, "bsd\0", 4) == 0 &&
		    le32_to_cpu(l->d_partitions[2].p_offset) == 0)
			bsd_start += offset;
		/* [한국어] 이 엔트리가 부모 MS-DOS 파티션 전체와 동일한 범위라면, 이미 상위
		 * msdos_partition()이 등록한 것과 같으므로 중복 등록을 피한다. */
		if (offset == bsd_start && size == bsd_size)
			/* full parent partition, we have it already */
			continue;
		/* [한국어] 이 엔트리가 부모 파티션 범위를 벗어나면(시작이 앞서거나 끝이 넘으면)
		 * 손상된 서브 파티션 정보로 보고 경고 로그만 남긴 뒤 건너뛴다. */
		if (offset > bsd_start || offset+size < bsd_start+bsd_size) {
			seq_buf_puts(&state->pp_buf, "bad subpartition - ignored\n");
			continue;
		}
		/* [한국어] 검증을 통과한 BSD 파티션의 (시작, 길이)를 등록한다. */
		put_partition(state, state->next++, bsd_start, bsd_size);
	}
	/* [한국어] disklabel 버퍼를 해제한다. */
	put_dev_sector(sect);
	/* [한국어] d_npartitions가 실제 순회한 max_partitions보다 많았다면, 슬롯 부족
	 * 등으로 등록하지 못하고 무시한 엔트리 수를 로그에 남긴다. */
	if (le16_to_cpu(l->d_npartitions) > max_partitions)
		seq_buf_printf(&state->pp_buf, " (ignored %d more)",
			       le16_to_cpu(l->d_npartitions) - max_partitions);
	/* [한국어] 이 파티션의 로그 라인을 " >\n"으로 닫는다. */
	seq_buf_puts(&state->pp_buf, " >\n");
}
#endif

/*
 * [한국어]
 * parse_freebsd() - FreeBSD disklabel 파서로 진입하는 얇은 래퍼.
 *
 * @state/@offset/@size/@origin: parse_bsd()에 그대로 전달되는 공용 파라미터.
 * @return: 없음(void).
 *
 * flavour="bsd", max_partitions=BSD_MAXPARTITIONS(16)로 고정해 parse_bsd()를
 * 호출한다. subtypes[] 테이블에서 FREEBSD_PARTITION 타입에 대응되는 콜백으로
 * 등록되어 있다. CONFIG_BSD_DISKLABEL이 꺼져 있으면 아무 일도 하지 않는다.
 * 실행 컨텍스트: msdos_partition() 2nd pass 호출 중.
 * 호출자: msdos_partition()(subtypes[] 테이블을 통해 함수 포인터로).
 * 피호출자: parse_bsd().
 * 에러 처리: 없음(단순 위임).
 *
 * 호출 체인:
 *   msdos_partition() -> subtypes[].parse -> [parse_freebsd()] -> parse_bsd()
 */
static void parse_freebsd(struct parsed_partitions *state,
			  sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_BSD_DISKLABEL
	/* [한국어] flavour="bsd", 상한 BSD_MAXPARTITIONS(16)로 FreeBSD disklabel 파서를
	 * 호출한다. */
	parse_bsd(state, offset, size, origin, "bsd", BSD_MAXPARTITIONS);
#endif
}

/*
 * [한국어]
 * parse_netbsd() - NetBSD disklabel 파서로 진입하는 얇은 래퍼.
 *
 * @state/@offset/@size/@origin: parse_bsd()에 그대로 전달되는 공용 파라미터.
 * @return: 없음(void).
 *
 * flavour="netbsd", max_partitions=BSD_MAXPARTITIONS(16)로 고정해
 * parse_bsd()를 호출한다. subtypes[] 테이블에서 NETBSD_PARTITION 타입에
 * 대응되는 콜백으로 등록되어 있다.
 * 실행 컨텍스트: msdos_partition() 2nd pass 호출 중.
 * 호출자: msdos_partition()(subtypes[] 테이블을 통해).
 * 피호출자: parse_bsd().
 * 에러 처리: 없음(단순 위임).
 *
 * 호출 체인:
 *   msdos_partition() -> subtypes[].parse -> [parse_netbsd()] -> parse_bsd()
 */
static void parse_netbsd(struct parsed_partitions *state,
			 sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_BSD_DISKLABEL
	/* [한국어] flavour="netbsd", 상한 BSD_MAXPARTITIONS(16)로 NetBSD disklabel 파서를
	 * 호출한다. */
	parse_bsd(state, offset, size, origin, "netbsd", BSD_MAXPARTITIONS);
#endif
}

/*
 * [한국어]
 * parse_openbsd() - OpenBSD disklabel 파서로 진입하는 얇은 래퍼.
 *
 * @state/@offset/@size/@origin: parse_bsd()에 그대로 전달되는 공용 파라미터.
 * @return: 없음(void).
 *
 * flavour="openbsd", max_partitions=OPENBSD_MAXPARTITIONS(16)로 고정해
 * parse_bsd()를 호출한다. subtypes[] 테이블에서 OPENBSD_PARTITION 타입에
 * 대응되는 콜백으로 등록되어 있다.
 * 실행 컨텍스트: msdos_partition() 2nd pass 호출 중.
 * 호출자: msdos_partition()(subtypes[] 테이블을 통해).
 * 피호출자: parse_bsd().
 * 에러 처리: 없음(단순 위임).
 *
 * 호출 체인:
 *   msdos_partition() -> subtypes[].parse -> [parse_openbsd()] -> parse_bsd()
 */
static void parse_openbsd(struct parsed_partitions *state,
			  sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_BSD_DISKLABEL
	/* [한국어] flavour="openbsd", 상한 OPENBSD_MAXPARTITIONS(16)로 OpenBSD disklabel
	 * 파서를 호출한다. */
	parse_bsd(state, offset, size, origin, "openbsd",
		  OPENBSD_MAXPARTITIONS);
#endif
}

/* [한국어] Unixware(SCO/Univel Unixware) disklabel/VTOC 관련 매직·크기 상수.
 * UNIXWARE_DISKMAGIC(0xCA5E600D, "CASE COOD"에 대한 말장난)는 disklabel 매직,
 * UNIXWARE_DISKMAGIC2(0x600DDEEE, Solaris VTOC sanity 값과 동일한 패턴)는 그
 * 안에 중첩된 슬라이스 테이블(VTOC) 매직, UNIXWARE_NUMSLICE(16)는 슬라이스 배열
 * 크기, UNIXWARE_FS_UNUSED(0)는 미사용 슬라이스 표시값이다. */
#define UNIXWARE_DISKMAGIC     (0xCA5E600DUL)	/* The disk magic number */
#define UNIXWARE_DISKMAGIC2    (0x600DDEEEUL)	/* [한국어] UnixWare 슬라이스 테이블 매직.
						 * 위 SOLARIS_X86_VTOC_SANE 과 같은 값이다(공통 조상). 원본 주석: The slice table magic nr */
#define UNIXWARE_NUMSLICE      16	/* [한국어] UnixWare 슬라이스 최대 개수 — 파싱 루프 상한 */
#define UNIXWARE_FS_UNUSED     0		/* [한국어] 빈 슬라이스를 나타내는 파일시스템 타입 값(0).
						 * 파싱 시 이 값인 항목은 건너뛴다 — 테이블에는 자리가 있지만
						 * 실제 파티션은 아니라는 뜻이다. 원본 주석: Unused slice entry ID */

/*
 * [한국어] Unixware 슬라이스 온디스크 엔트리 - vtoc.v_slice[] 배열의 원소 타입.
 * 각 슬라이스가 담당하는 LBA 범위를 start_sect/nr_sects로 표현한다(이름이 이 파일
 * 최상단의 start_sect()/nr_sects() 헬퍼 함수와 같지만, 이 구조체의 필드는 함수가
 * 아니라 데이터 멤버이며 서로 별개다).
 */
struct unixware_slice {
	__le16   s_label;	/* label */
	/* [한국어] 슬라이스 레이블(용도 식별자). 이 파일은 사용하지 않고, UNIXWARE_FS_UNUSED
	 * 인지 여부만 아래 s_label 필드로 검사한다(사실상 이 필드가 그 s_label이다). */
	__le16   s_flags;	/* permission flags */
	/* [한국어] 슬라이스 접근 권한 플래그. 이 파일은 사용하지 않는다. */
	__le32   start_sect;	/* starting sector */
	/* [한국어] 슬라이스 시작 섹터.
	 * 설정자: Unixware 파티셔닝 도구. 읽는 자: parse_unixware()가
	 * le32_to_cpu(p->start_sect)로 읽어 put_partition()의 시작 LBA 인자로 그대로
	 * 사용한다(Solaris 슬라이스와 달리 offset을 더하는 보정이 없다 - 이미 절대값).
	 * 값 범위: 0 이상. 동기화: 온디스크 읽기 전용 값. */
	__le32   nr_sects;	/* number of sectors in slice */
	/* [한국어] 슬라이스 길이(섹터 수).
	 * 설정자: Unixware 파티셔닝 도구. 읽는 자: parse_unixware()가
	 * le32_to_cpu(p->nr_sects)로 읽어 put_partition()의 길이 인자로 사용한다. 동기화:
	 * 온디스크 읽기 전용 값. */
};

/*
 * [한국어] Unixware disklabel 온디스크 구조체 - 대부분의 기하구조 필드는 이 파일이
 * 사용하지 않는 자리채움이고, 실제로 쓰이는 것은 d_magic(1차 매직 검증)과 중첩된
 * struct unixware_vtoc(2차 매직 검증 + 실제 슬라이스 테이블)뿐이다.
 */
struct unixware_disklabel {
	__le32	d_type;			/* drive type */
	/* [한국어] 드라이브 타입 식별자. 이 파일은 사용하지 않는다. */
	__le32	d_magic;		/* the magic number */
	/* [한국어] disklabel 1차 유효성 매직.
	 * 설정자: Unixware 파티셔닝 도구가 UNIXWARE_DISKMAGIC 값으로 기록. 읽는 자:
	 * parse_unixware()가 le32_to_cpu(l->d_magic) != UNIXWARE_DISKMAGIC 이면 무효로
	 * 판정한다. 값 범위: UNIXWARE_DISKMAGIC(0xCA5E600D)이어야 유효. 동기화: 온디스크
	 * 읽기 전용 값. */
	__le32	d_version;		/* version number */
	/* [한국어] disklabel 버전 번호. 이 파일은 검증하지 않는다. */
	char	d_serial[12];		/* serial number of the device */
	/* [한국어] 장치 시리얼 번호 문자열. 이 파일은 사용하지 않는다. */
	__le32	d_ncylinders;		/* # of data cylinders per device */
	/* [한국어] 장치당 데이터 실린더 수(CHS 기하구조). 이 파일은 사용하지 않는다. */
	__le32	d_ntracks;		/* # of tracks per cylinder */
	/* [한국어] 실린더당 트랙 수. 이 파일은 사용하지 않는다. */
	__le32	d_nsectors;		/* # of data sectors per track */
	/* [한국어] 트랙당 데이터 섹터 수. 이 파일은 사용하지 않는다. */
	__le32	d_secsize;		/* # of bytes per sector */
	/* [한국어] 섹터 크기(바이트). 이 파일은 사용하지 않는다. */
	__le32	d_part_start;		/* # of first sector of this partition*/
	/* [한국어] 이 파티션의 첫 섹터 번호(참고용, 부모 MS-DOS 파티션 관점). 이 파일은
	 * 사용하지 않는다(대신 parse_unixware() 호출 인자 offset을 쓴다). */
	__le32	d_unknown1[12];		/* ? */
	/* [한국어] 원본 주석대로 용도 불명(레이아웃 자리채움). 이 파일은 사용하지 않는다. */
	__le32	d_alt_tbl;		/* byte offset of alternate table */
	/* [한국어] 대체(alternate) 테이블의 바이트 오프셋. 이 파일은 사용하지 않는다. */
	__le32	d_alt_len;		/* byte length of alternate table */
	/* [한국어] 대체 테이블의 바이트 길이. 이 파일은 사용하지 않는다. */
	__le32	d_phys_cyl;		/* # of physical cylinders per device */
	/* [한국어] 장치당 물리 실린더 수. 이 파일은 사용하지 않는다. */
	__le32	d_phys_trk;		/* # of physical tracks per cylinder */
	/* [한국어] 실린더당 물리 트랙 수. 이 파일은 사용하지 않는다. */
	__le32	d_phys_sec;		/* # of physical sectors per track */
	/* [한국어] 트랙당 물리 섹터 수. 이 파일은 사용하지 않는다. */
	__le32	d_phys_bytes;		/* # of physical bytes per sector */
	/* [한국어] 섹터당 물리 바이트 수. 이 파일은 사용하지 않는다. */
	__le32	d_unknown2;		/* ? */
	/* [한국어] 원본 주석대로 용도 불명. 이 파일은 사용하지 않는다. */
	__le32	d_unknown3;		/* ? */
	/* [한국어] 원본 주석대로 용도 불명. 이 파일은 사용하지 않는다. */
	__le32	d_pad[8];		/* pad */
	/* [한국어] 구조체 크기를 맞추기 위한 패딩 8워드. 이 파일은 사용하지 않는다. */

	/* [한국어] 이 disklabel에 중첩된 VTOC(Volume Table Of Contents) - 실제 슬라이스
	 * 테이블은 이 안의 v_slice[]에 있다. 아래 필드별 설명 참고. */
	struct unixware_vtoc {
		__le32	v_magic;		/* the magic number */
		/* [한국어] VTOC 2차 유효성 매직.
		 * 설정자: Unixware 파티셔닝 도구가 UNIXWARE_DISKMAGIC2 값으로 기록. 읽는 자:
		 * parse_unixware()가 d_magic과 함께 이 값도 UNIXWARE_DISKMAGIC2와 비교해 둘 다
		 * 맞아야 유효한 것으로 판정한다. 값 범위: UNIXWARE_DISKMAGIC2(0x600DDEEE)이어야
		 * 유효. 동기화: 온디스크 읽기 전용 값. */
		__le32	v_version;		/* version number */
		/* [한국어] VTOC 버전 번호. 이 파일은 검증하지 않는다. */
		char	v_name[8];		/* volume name */
		/* [한국어] 볼륨 이름 문자열(8바이트). 이 파일은 사용하지 않는다. */
		__le16	v_nslices;		/* # of slices */
		/* [한국어] 이 VTOC에 기록된 슬라이스 개수. 이 파일은 사용하지 않는다(대신
		 * UNIXWARE_NUMSLICE 고정 상한과 s_label 검사로 순회한다). */
		__le16	v_unknown1;		/* ? */
		/* [한국어] 원본 주석대로 용도 불명. 이 파일은 사용하지 않는다. */
		__le32	v_reserved[10];		/* reserved */
		/* [한국어] 예약 영역(향후 확장용). 이 파일은 사용하지 않는다. */
		struct unixware_slice
			/* [한국어] 실제 슬라이스 배열 - UNIXWARE_NUMSLICE(16)개 고정.
			 * 설정자: Unixware 파티셔닝 도구. 읽는 자: parse_unixware()가 인덱스 1부터
			 * (0번은 항상 전체 디스크와 동일하므로 건너뜀) 순회하며 s_label이
			 * UNIXWARE_FS_UNUSED가 아닌 항목을 put_partition()으로 등록한다. 값 범위:
			 * 각 원소는 struct unixware_slice 필드 설명 참고. 동기화: 온디스크 읽기
			 * 전용 값. */
			v_slice[UNIXWARE_NUMSLICE];	/* slice headers */
	} vtoc;
	/* [한국어] 위 struct unixware_vtoc 중첩 인스턴스 필드 자체.
	 * 설정자: Unixware 파티셔닝 도구가 disklabel 전체를 디스크에 기록할 때 함께
	 * 기록한다. 읽는 자: parse_unixware()가 l->vtoc.v_magic/v_slice[]로 접근한다.
	 * 동기화: 온디스크 읽기 전용 값, 스캔 스레드 단독 접근. */
};  /* 408 */

/*
 * Create devices for Unixware partitions listed in a disklabel, under a
 * dos-like partition. See parse_extended() for more information.
 */
/*
 * [한국어]
 * parse_unixware() - 하나의 MS-DOS 파티션 안에 중첩된 Unixware VTOC를 파싱한다.
 *
 * @state: 파티션 스캔 상태.
 * @offset: 이 disklabel이 속한 부모 MS-DOS 파티션의 시작 LBA.
 * @size: 사용하지 않음(다른 서브 파서와 함수 포인터 시그니처를 맞추기 위한
 *        매개변수).
 * @origin: 부모 MS-DOS 파티션 번호(로그 헤더에만 사용).
 * @return: 없음(void). CONFIG_UNIXWARE_DISKLABEL 비활성 시 빈 함수.
 *
 * MS-DOS 파티션 타입이 UNIXWARE_PARTITION일 때 msdos_partition()의 2nd
 * pass가 subtypes[] 테이블을 통해 이 함수를 호출한다.
 * 동작 과정: (1) offset + 29 섹터에서 disklabel을 읽는다(Unixware 관례상
 * disklabel/VTOC는 파티션 시작으로부터 29섹터 뒤에 위치) (2) d_magic과
 * vtoc.v_magic 두 매직을 모두 검증, 하나라도 불일치하면 중단 (3)
 * v_slice[1]부터(0번은 항상 전체 디스크와 동일하므로 건너뜀)
 * UNIXWARE_NUMSLICE(16)개까지 순회하며 s_label이 UNIXWARE_FS_UNUSED가
 * 아닌 슬라이스를 put_partition()으로 등록한다.
 * 실행 컨텍스트: msdos_partition() 2nd pass 호출 중, 파티션 스캔 프로세스
 * 컨텍스트.
 * 호출자: msdos_partition()(subtypes[] 테이블을 통해 함수 포인터로 호출).
 * 피호출자: read_part_sector(), put_dev_sector(), put_partition(),
 * seq_buf_printf(), seq_buf_puts().
 * 에러 처리: 읽기 실패(!l)나 두 매직 중 하나라도 불일치하면 조용히 return.
 *
 * 호출 체인:
 *   msdos_partition() -> subtypes[].parse -> [parse_unixware()] ->
 *     read_part_sector(), put_partition(), put_dev_sector()
 */
static void parse_unixware(struct parsed_partitions *state,
			   sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_UNIXWARE_DISKLABEL
	/* [한국어] read_part_sector()가 채울 folio 참조 보관용. */
	Sector sect;
	/* [한국어] 읽어들인 Unixware disklabel 버퍼를 가리킬 포인터. */
	struct unixware_disklabel *l;
	/* [한국어] v_slice[] 순회용 포인터. */
	struct unixware_slice *p;

	/* [한국어] Unixware disklabel/VTOC는 offset + 29 LBA에 위치한다(관례) - 캐시 미스
	 * 시 하위 블록 드라이버 읽기 경로가 동작한다. */
	l = read_part_sector(state, offset + 29, &sect);
	/* [한국어] 읽기 실패/메모리 부족 시 스캔을 중단한다. */
	if (!l)
		return;
	/* [한국어] disklabel 1차 매직(d_magic)과 VTOC 2차 매직(vtoc.v_magic)을 모두
	 * 검증한다 - 둘 중 하나라도 틀리면 Unixware 레이아웃이 아니다. */
	if (le32_to_cpu(l->d_magic) != UNIXWARE_DISKMAGIC ||
	    le32_to_cpu(l->vtoc.v_magic) != UNIXWARE_DISKMAGIC2) {
		/* [한국어] 매직 불일치로 disklabel 버퍼를 해제하고 중단한다. */
		put_dev_sector(sect);
		return;
	}
	/* [한국어] 로그 버퍼에 " <파티션번호>: <unixware:" 헤더를 기록한다. */
	seq_buf_printf(&state->pp_buf, " %s%d: <unixware:", state->name, origin);
	/* [한국어] 순회 시작 포인터를 슬라이스 인덱스 1로 설정한다(0번은 항상 전체
	 * 디스크와 동일하므로 등록 대상에서 제외). */
	p = &l->vtoc.v_slice[1];
	/* I omit the 0th slice as it is the same as whole disk. */
	/* [한국어] 인덱스 1부터 UNIXWARE_NUMSLICE(16) 미만까지 슬라이스를 순회한다. */
	while (p - &l->vtoc.v_slice[0] < UNIXWARE_NUMSLICE) {
		/* [한국어] parts[] 슬롯이 소진되었다면 더 등록할 수 없으므로 순회를 중단한다. */
		if (state->next == state->limit)
			break;

		/* [한국어] 미사용으로 표시되지 않은 슬라이스만 등록 대상이다. start_sect는 절대
		 * LBA, nr_sects는 길이(섹터 수)로 그대로 쓰인다. */
		if (p->s_label != UNIXWARE_FS_UNUSED)
			put_partition(state, state->next++,
				      le32_to_cpu(p->start_sect),
				      le32_to_cpu(p->nr_sects));
		/* [한국어] 다음 슬라이스 엔트리로 이동한다(등록 여부와 무관하게 항상 전진). */
		p++;
	}
	/* [한국어] disklabel/VTOC 버퍼를 해제한다. */
	put_dev_sector(sect);
	/* [한국어] 이 파티션의 로그 라인을 " >\n"으로 닫는다. */
	seq_buf_puts(&state->pp_buf, " >\n");
#endif
}

/* [한국어] Minix 파티션이 자신의 첫 섹터에 2차 파티션 테이블(서브 파티션)을 둘 때
 * 담을 수 있는 최대 서브 파티션 개수. */
#define MINIX_NR_SUBPARTITIONS  4

/*
 * Minix 2.0.0/2.0.2 subpartition support.
 * Anand Krishnamurthy <anandk@wiproge.med.ge.com>
 * Rajeev V. Pillai    <rajeevvp@yahoo.com>
 */
/*
 * [한국어]
 * parse_minix() - Minix 파티션 첫 섹터에 있을 수 있는 2차 파티션 테이블(서브 파티션)을 파싱한다.
 *
 * @state: 파티션 스캔 상태.
 * @offset: 이 Minix 파티션의 시작 LBA(msdos_partition()의 2nd pass가
 *          start_sect(p)*sector_size로 전달).
 * @size: 사용하지 않음(다른 서브 파서와 함수 포인터 시그니처를 맞추기 위한
 *        매개변수).
 * @origin: 부모 MS-DOS 파티션 번호(로그 헤더에만 사용).
 * @return: 없음(void). CONFIG_MINIX_SUBPARTITION 비활성 시 빈 함수.
 *
 * MS-DOS 파티션 타입이 MINIX_PARTITION일 때 msdos_partition()의 2nd
 * pass가 subtypes[] 테이블을 통해 이 함수를 호출한다. Minix 파티션의 첫
 * 섹터는 (원본 주석대로) 서브 파티션을 나열하는 2차 MBR이거나, 그냥 평범한
 * 부트섹터일 수 있어 두 경우를 시그니처+타입으로 구분해야 한다.
 * 동작 과정: (1) 파티션의 첫 섹터(offset)를 읽는다 (2) 0x55AA 시그니처와
 * 오프셋 0x1be 엔트리의 sys_ind가 MINIX_PARTITION인지 모두 확인해야만 2차
 * 테이블로 인정한다 (3) 인정되면 MINIX_NR_SUBPARTITIONS(4)개의 엔트리를
 * 순회하며 sys_ind가 MINIX_PARTITION인 것만 put_partition()으로 등록한다.
 * 실행 컨텍스트: msdos_partition() 2nd pass 호출 중, 파티션 스캔 프로세스
 * 컨텍스트.
 * 호출자: msdos_partition()(subtypes[] 테이블을 통해 함수 포인터로 호출).
 * 피호출자: read_part_sector(), put_dev_sector(), msdos_magic_present(),
 * put_partition(), start_sect(), nr_sects(), seq_buf_printf(),
 * seq_buf_puts().
 * 에러 처리: 읽기 실패(!data) 시 조용히 return. 시그니처/타입이 2차 테이블
 * 조건을 만족하지 않으면(if 실패) 아무것도 등록하지 않고 버퍼만 정리한다
 * (이 경우는 에러가 아니라 "평범한 부트섹터"라는 정상 케이스).
 *
 * 호출 체인:
 *   msdos_partition() -> subtypes[].parse -> [parse_minix()] ->
 *     read_part_sector(), msdos_magic_present(), put_partition(),
 *     put_dev_sector()
 */
static void parse_minix(struct parsed_partitions *state,
			sector_t offset, sector_t size, int origin)
{
#ifdef CONFIG_MINIX_SUBPARTITION
	/* [한국어] read_part_sector()가 채울 folio 참조 보관용. */
	Sector sect;
	/* [한국어] 읽어들인 Minix 파티션 첫 섹터 버퍼를 가리킬 포인터. */
	unsigned char *data;
	/* [한국어] 2차 파티션 테이블 엔트리를 가리킬 포인터. */
	struct msdos_partition *p;
	/* [한국어] MINIX_NR_SUBPARTITIONS(4)개 서브 파티션 순회 인덱스. */
	int i;

	/* [한국어] Minix 파티션의 첫 번째 섹터(offset LBA)를 읽는다 - 캐시 미스 시 하위
	 * 블록 드라이버 읽기 경로가 동작한다. */
	data = read_part_sector(state, offset, &sect);
	/* [한국어] 메모리 부족/읽기 실패 시 서브 파티션 탐색을 중단한다. */
	if (!data)
		return;

	/* [한국어] Minix 파티션 첫 섹터의 2차 파티션 테이블도 표준 MBR과 동일하게 오프셋
	 * 0x1be에 위치한다. */
	p = (struct msdos_partition *)(data + 0x1be);

	/* The first sector of a Minix partition can have either
	 * a secondary MBR describing its subpartitions, or
	 * the normal boot sector. */
	/* [한국어] 0x55AA 부트 시그니처와, 오프셋 0x1be의 엔트리가 MINIX_PARTITION 타입인
	 * 조건을 모두 만족해야만 이 섹터를 "서브 파티션 테이블 있음"으로 인정한다 - 둘 중
	 * 하나만으로는 오판 위험이 있다. */
	if (msdos_magic_present(data + 510) &&
	    p->sys_ind == MINIX_PARTITION) { /* subpartition table present */
		/* [한국어] 로그 버퍼에 " <파티션번호>: <minix:" 헤더를 기록한다. */
		seq_buf_printf(&state->pp_buf, " %s%d: <minix:", state->name, origin);
		/* [한국어] MINIX_NR_SUBPARTITIONS(4)개의 서브 파티션 엔트리를 순회한다. */
		for (i = 0; i < MINIX_NR_SUBPARTITIONS; i++, p++) {
			/* [한국어] parts[] 슬롯이 소진되었다면 더 등록할 수 없으므로 순회를 중단한다. */
			if (state->next == state->limit)
				break;
			/* add each partition in use */
			/* [한국어] sys_ind가 MINIX_PARTITION인(실제 사용 중인) 엔트리만 절대 LBA/길이로
			 * 등록한다. */
			if (p->sys_ind == MINIX_PARTITION)
				/* [한국어] start_sect(p)/nr_sects(p)는 이미 이 파일 상단 헬퍼로 비정렬 안전하게
				 * 읽은 절대(이 서브 테이블 기준) LBA/길이다. */
				put_partition(state, state->next++,
					      start_sect(p), nr_sects(p));
		}
		/* [한국어] 이 파티션의 로그 라인을 " >\n"으로 닫는다. */
		seq_buf_puts(&state->pp_buf, " >\n");
	}
	/* [한국어] 조건 만족 여부와 무관하게 항상 첫 섹터 버퍼를 해제한다. */
	put_dev_sector(sect);
#endif /* CONFIG_MINIX_SUBPARTITION */
}

/*
 * [한국어] MS-DOS 파티션 타입(sys_ind) 바이트 -> 2nd pass 서브 파서 함수 포인터
 * 매핑 테이블. msdos_partition()의 2nd pass가 각 기본 파티션의 sys_ind로 이 표를
 * 선형 검색해, 해당 타입에 대응하는 서브 레이블(BSD/Solaris/Unixware/Minix) 파서를
 * 찾아 호출한다. 마지막 {0, NULL} 원소는 검색 종료를 표시하는 sentinel이다.
 */
static struct {
	/* [한국어] 이 엔트리가 대응하는 MS-DOS 파티션 타입 바이트(sys_ind 값). */
	unsigned char id;
	/* [한국어] 해당 타입을 만났을 때 호출할 서브 파서 함수 포인터 - 모든 parse_*() 함수가
	 * 동일한 (state, offset, size, origin) 시그니처를 공유하므로 균일하게 저장 가능하다. */
	void (*parse)(struct parsed_partitions *, sector_t, sector_t, int);
} subtypes[] = {	/* [한국어] 파일 스코프 static 배열로 두어 초기화가 컴파일 타임에 끝나고, 새 서브 포맷을 지원할 때 이 표에 한 줄만 추가하면 되도록 한 구조다(디스패치 코드는 손대지 않는다). */
	/* [한국어] FreeBSD 파티션 타입 -> parse_freebsd(). */
	{FREEBSD_PARTITION, parse_freebsd},
	/* [한국어] NetBSD 파티션 타입 -> parse_netbsd(). */
	{NETBSD_PARTITION, parse_netbsd},
	/* [한국어] OpenBSD 파티션 타입 -> parse_openbsd(). */
	{OPENBSD_PARTITION, parse_openbsd},
	/* [한국어] Minix 파티션 타입 -> parse_minix(). */
	{MINIX_PARTITION, parse_minix},
	/* [한국어] Unixware 파티션 타입 -> parse_unixware(). */
	{UNIXWARE_PARTITION, parse_unixware},
	/* [한국어] 구버전 Solaris x86 파티션 타입 -> parse_solaris_x86(). */
	{SOLARIS_X86_PARTITION, parse_solaris_x86},
	/* [한국어] 신버전 Solaris x86 파티션 타입 -> 동일한 parse_solaris_x86() 재사용
	 * (VTOC 파싱 로직 자체는 구/신버전 타입 바이트 구분과 무관하게 동일). */
	{NEW_SOLARIS_X86_PARTITION, parse_solaris_x86},
	/* [한국어] 검색 종료 sentinel - id/parse 모두 0/NULL이면 이 이후로는 대응하는 서브
	 * 파서가 없다는 뜻이다(msdos_partition()의 for 루프 종료 조건). */
	{0, NULL},
};

/*
 * [한국어]
 * msdos_partition() - MS-DOS(MBR) 파티션 테이블의 최상위 진입점.
 *
 * @state: 파티션 스캔 상태(parsed_partitions). 이 함수와 이 함수가 호출하는
 *         모든 하위 파서가 이 상태를 통해 섹터를 읽고 파티션을 등록한다.
 * @return: 1(성공 - 이 디스크를 MBR 또는 파티션 테이블 없는 FAT로 확정)
 *          0(이 포맷이 아님 - GPT이거나, 시그니처 불일치, 또는 boot_ind
 *          이상으로 파티션 테이블을 신뢰할 수 없음) -1(LBA 0 읽기 자체가
 *          실패 - I/O 오류로 취급). 이 세 값은 check.h가 정의한 프로버 공용
 *          계약(1=성공/0=포맷 아님/음수=I/O 오류)을 그대로 따른다.
 *
 * check_partition()이 등록된 프로버들을 순서대로 시도할 때 호출하는, 이
 * 파일의 유일한 외부 진입점이다. MBR은 전 세계에서 가장 오래되고 여전히
 * 가장 흔한 파티션 스킴이므로, 이 함수는 단순 MBR뿐 아니라 GPT 배제,
 * AIX/FAT 폴백, 확장 파티션, 다양한 OS별 서브 레이블까지 모두 처리한다.
 * 동작 과정:
 *  1) LBA 0을 읽는다. 실패하면 -1(I/O 오류)로 즉시 반환.
 *  2) aix_magic_present()로 AIX 디스크 여부를 먼저 확인한다(일부 AIX
 *     디스크는 0x55AA가 없어 순서가 중요하다) - AIX면 CONFIG_AIX_PARTITION
 *     여부에 따라 aix_partition()에 위임하거나 0을 반환.
 *  3) 0x55AA 시그니처를 확인 - 없으면 MBR이 아니므로 0.
 *  4) 4개 기본 파티션 엔트리의 boot_ind가 모두 0 또는 0x80인지 검사 -
 *     아니면 손상된 테이블이거나(0), 파티션 테이블 없이 부트섹터만 있는
 *     FAT 파일시스템일 수 있어(fb->reserved && fb->fats &&
 *     fat_valid_media()) 그 경우 1을 반환해 디스크 전체를 파티션 없는
 *     FAT로 인정한다.
 *  5) CONFIG_EFI_PARTITION이면, 4개 엔트리 중 하나라도
 *     EFI_PMBR_OSTYPE_EFI_GPT(0xEE)이면 즉시 0을 반환해 GPT 파서
 *     (efi_partition())에게 양보한다(efi.c와의 상호 배타 관계).
 *  6) MBR 오프셋 0x1b8의 4바이트 disksig를 읽는다.
 *  7) 1st pass: 논리 파티션 번호를 5부터 시작(state->next=5)하고, 4개
 *     기본 파티션 엔트리를 순회하며 크기 0은 건너뛰고, 확장 파티션이면
 *     2섹터짜리 보호 엔트리만 등록한 뒤 parse_extended()로 EBR 체인을
 *     따라가며, 일반 데이터 파티션이면 put_partition()+set_info()로
 *     등록하고 RAID/DM6/EZD 특수 표시를 로그에 남긴다.
 *  8) 2nd pass: 다시 4개 기본 파티션을 순회하며 subtypes[] 테이블로 각
 *     sys_ind에 대응하는 서브 파서(BSD/Solaris/Unixware/Minix)를 찾아
 *     호출해 중첩 레이블을 추가로 파싱한다.
 *  9) LBA 0 버퍼를 반납하고 1을 반환한다.
 * 실행 컨텍스트: check_partition()의 호출 스택에서 단일 프로세스
 * 컨텍스트로 동기 실행되며, 하위 read_part_sector() 호출들이 캐시 미스
 * 시 블로킹 I/O를 유발할 수 있다.
 * 호출자: block/partitions/core.c의 check_partition()(check.h가 선언한
 * 함수 포인터 배열을 통해).
 * 피호출자: read_part_sector(), put_dev_sector(), aix_magic_present(),
 * msdos_magic_present(), is_extended_partition(), parse_extended(),
 * put_partition(), set_info(), start_sect(), nr_sects(),
 * queue_logical_block_size(), subtypes[].parse, seq_buf_puts().
 * 에러 처리: LBA 0 읽기 실패만 -1(I/O 오류)로 구분해 보고하고, 그 외
 * "이 포맷이 아님"에 해당하는 모든 경로(0x55AA 없음, boot_ind 이상,
 * GPT 발견)는 0으로 통일해 check_partition()이 다음 프로버로 넘어가게
 * 한다.
 *
 * 호출 체인:
 *   check_partition() -> [msdos_partition()] -> aix_magic_present(),
 *     msdos_magic_present(), parse_extended(), put_partition(),
 *     set_info(), subtypes[].parse(parse_bsd/parse_solaris_x86/
 *     parse_unixware/parse_minix 등)
 */
int msdos_partition(struct parsed_partitions *state)
{
	/* [한국어] 논리 블록 크기를 512로 나눈 배수 - 512B 단위 필드를 실제 LBA로 환산하는
	 * 계수(parse_extended()의 동일 이름 지역 변수와 같은 역할). */
	sector_t sector_size;
	/* [한국어] read_part_sector()가 채울 folio 참조 보관용(LBA 0 전용). */
	Sector sect;
	/* [한국어] LBA 0(MBR) 512바이트 버퍼의 시작 주소. */
	unsigned char *data;
	/* [한국어] MBR 파티션 테이블 엔트리(오프셋 0x1be)를 가리킬 포인터, 1st/2nd pass
	 * 순회에서 재사용된다. */
	struct msdos_partition *p;
	/* [한국어] boot_ind 검사 실패 시 순수 FAT 부트섹터 여부를 판별하기 위해 data를
	 * 재해석할 포인터. */
	struct fat_boot_sector *fb;
	/* [한국어] 4개 기본 파티션 슬롯(1~4) 순회 인덱스이자 파티션 번호로도 쓰인다. */
	int slot;
	/* [한국어] MBR 오프셋 0x1b8의 4바이트 디스크 시그니처 - set_info()에 전달되어 유사
	 * UUID를 만드는 재료가 된다. */
	u32 disksig;

	/* [한국어] 이 디스크의 논리 블록 크기를 512로 나눠 sector_size 계수를 구한다 -
	 * parse_extended()와 동일한 환산 기준을 1st/2nd pass 전체에서 공유한다. */
	sector_size = queue_logical_block_size(state->disk->queue) / 512;
	/* [한국어] LBA 0(MBR/부트섹터)을 읽는다 - 캐시 미스 시 하위 블록 드라이버의 읽기
	 * 경로가 동작한다. */
	data = read_part_sector(state, 0, &sect);
	/* [한국어] 메모리 할당 실패나 I/O 오류로 LBA 0을 읽지 못한 경우. */
	if (!data)
		/* [한국어] check.h 계약의 "음수=I/O 오류"에 해당 - check_partition()이 이 값을
		 * 기억해 뒀다가 모든 프로버 실패 시 최종 에러로 승격시킨다. */
		return -1;

	/*
	 * Note order! (some AIX disks, e.g. unbootable kind,
	 * have no MSDOS 55aa)
	 */
	/* [한국어] LBA 0의 상위 4바이트가 AIX 라벨 매직이면, 0x55AA 검사보다 먼저 AIX로
	 * 분기한다(원본 주석대로 일부 AIX 디스크는 0x55AA가 없어 순서가 중요하다). */
	if (aix_magic_present(state, data)) {
		/* [한국어] AIX 판정 시 LBA 0 버퍼는 더 이상 필요 없으므로 먼저 반납한다. */
		put_dev_sector(sect);
#ifdef CONFIG_AIX_PARTITION
		/* [한국어] AIX 전용 파서가 빌드에 포함되어 있으면 그쪽에 전체 파싱을 위임한다. */
		return aix_partition(state);
#else
		/* [한국어] AIX 파서가 빌드에서 빠져 있으면 파싱하지 못함을 로그로만 남기고,
		 * "포맷은 인식했으나 처리 불가"가 아니라 관례상 0(포맷 아님)으로 반환한다. */
		seq_buf_puts(&state->pp_buf, " [AIX]");
		return 0;
#endif
	}

	/* [한국어] AIX가 아니라면 이제 표준 MBR 시그니처 0x55AA를 확인한다. */
	if (!msdos_magic_present(data + 510)) {
		/* [한국어] 시그니처가 없으면 MBR이 아니므로 버퍼를 반납하고 0(포맷 아님)을 반환해
		 * 다음 프로버로 넘어가게 한다. */
		put_dev_sector(sect);
		return 0;
	}

	/*
	 * Now that the 55aa signature is present, this is probably
	 * either the boot sector of a FAT filesystem or a DOS-type
	 * partition table. Reject this in case the boot indicator
	 * is not 0 or 0x80.
	 */
	/* [한국어] MBR 파티션 테이블은 LBA 0 내 오프셋 0x1be(446)부터 16바이트씩 4개
	 * 엔트리로 배치된다 - 이 오프셋은 IBM PC 표준 MBR 레이아웃의 고정 위치다. */
	p = (struct msdos_partition *) (data + 0x1be);
	/* [한국어] 4개의 기본 파티션 엔트리를 순회하며 boot_ind가 유효한 값(0=비활성 또는
	 * 0x80=활성)인지 확인한다 - 하나라도 다른 값이면 파티션 테이블을 신뢰할 수 없다. */
	for (slot = 1; slot <= 4; slot++, p++) {
		/* [한국어] boot_ind가 0도 0x80도 아니면 유효한 MBR 엔트리로 보기 어렵다. */
		if (p->boot_ind != 0 && p->boot_ind != 0x80) {
			/*
			 * Even without a valid boot indicator value
			 * its still possible this is valid FAT filesystem
			 * without a partition table.
			 */
			/* [한국어] data를 struct fat_boot_sector로 재해석해 FAT 부트섹터 특유의 필드를
			 * 검사할 준비를 한다. */
			fb = (struct fat_boot_sector *) data;
			/* [한국어] 첫 번째 슬롯에서만(파티션 테이블이 아예 없는 디스크를 가정) reserved/
			 * fats 필드가 채워져 있고 media 바이트가 유효한 FAT 매체 코드인지 확인한다. */
			if (slot == 1 && fb->reserved && fb->fats
				&& fat_valid_media(fb->media)) {
				/* [한국어] 로그 라인을 개행으로 마무리한다(파티션 목록 없이 FAT 단독임을
				 * 표시). */
				seq_buf_puts(&state->pp_buf, "\n");
				/* [한국어] LBA 0 버퍼를 반납한다. */
				put_dev_sector(sect);
				/* [한국어] 파티션 테이블 없이 디스크 전체를 FAT로 확정 - check.h 계약의
				 * "1=성공"에 해당한다. */
				return 1;
			} else {
				/* [한국어] FAT 부트섹터 조건도 만족하지 않으면 이 boot_ind 이상만으로는
				 * 아무것도 확정할 수 없으므로 버퍼를 반납하고 0(포맷 아님)을 반환한다. */
				put_dev_sector(sect);
				return 0;
			}
		}
	}

#ifdef CONFIG_EFI_PARTITION
	/* [한국어] 파티션 테이블 포인터를 다시 엔트리 0번(오프셋 0x1be)으로 되돌린다. */
	p = (struct msdos_partition *) (data + 0x1be);
	/* [한국어] GPT 여부만 확인하기 위해 4개 엔트리를 한 번 더(boot_ind 검사와는 별개로)
	 * 순회한다. */
	for (slot = 1 ; slot <= 4 ; slot++, p++) {
		/* If this is an EFI GPT disk, msdos should ignore it. */
		/* [한국어] sys_ind가 EFI_PMBR_OSTYPE_EFI_GPT(0xEE)이면 이 디스크는 GPT protective/
		 * hybrid MBR이다 - efi.c의 efi_partition()이 진짜 파싱을 담당해야 하므로 이 파일은
		 * 스스로 물러난다(efi.c의 is_pmbr_valid()/pmbr_part_valid()가 대응 검증을 수행). */
		if (p->sys_ind == EFI_PMBR_OSTYPE_EFI_GPT) {
			/* [한국어] LBA 0 버퍼를 반납하고 0(포맷 아님)을 반환해 GPT 파서에게 양보한다. */
			put_dev_sector(sect);
			return 0;
		}
	}
#endif
	/* [한국어] GPT가 아님을 확인했으므로(또는 CONFIG_EFI_PARTITION이 꺼져 있으므로) 포인터를
	 * 다시 엔트리 0번으로 되돌려 본격적인 1st pass를 준비한다. */
	p = (struct msdos_partition *) (data + 0x1be);

	/* [한국어] MBR 오프셋 0x1b8(440)의 4바이트를 디스크 시그니처(disksig)로 읽는다 - 이후
	 * set_info()가 이 값과 슬롯 번호를 조합해 유사 UUID를 만드는 재료로 쓴다. */
	disksig = le32_to_cpup((__le32 *)(data + 0x1b8));

	/*
	 * Look for partitions in two passes:
	 * First find the primary and DOS-type extended partitions.
	 * On the second pass look inside *BSD, Unixware and Solaris partitions.
	 */

	/* [한국어] 논리 파티션(확장 파티션 내부에서 발견될 파티션) 번호는 5번부터
	 * 시작한다 - 1~4번은 항상 기본 파티션 슬롯 몫으로 예약되어 있다. */
	state->next = 5;
	/* [한국어] 1st pass: 4개의 기본 파티션 엔트리를 순회하며 기본/확장 파티션을 등록한다. */
	for (slot = 1 ; slot <= 4 ; slot++, p++) {
		/* [한국어] start_sect(p)는 MBR 512B 섹터 기준 상대 오프셋이므로 sector_size를 곱해
		 * 실제 디스크 LBA로 환산한다. */
		sector_t start = start_sect(p)*sector_size;
		/* [한국어] 길이도 동일하게 LBA 단위로 환산한다. */
		sector_t size = nr_sects(p)*sector_size;

		/* [한국어] 길이가 0이면 미사용 엔트리이므로 건너뛴다. */
		if (!size)
			continue;
		/* [한국어] 확장 파티션이면 실제 데이터가 아니라 EBR 체인의 진입점이므로 별도
		 * 처리한다. */
		if (is_extended_partition(p)) {
			/*
			 * prevent someone doing mkfs or mkswap on an
			 * extended partition, but leave room for LILO
			 * FIXME: this uses one logical sector for > 512b
			 * sector, although it may not be enough/proper.
			 */
			/* [한국어] 보호 영역 길이를 최소 2섹터로 잡을 지역 변수(FIXME 원본 주석대로
			 * 512B보다 큰 논리 섹터에서는 부족할 수 있음을 커널 개발자들도 인지하고 있다). */
			sector_t n = 2;

			/* [한국어] 확장 파티션 전체 크기(size)와 sector_size 중 큰 값을 하한으로 삼되,
			 * 확장 파티션 자체보다 커지지 않도록 size로 상한도 씌운다. */
			n = min(size, max(sector_size, n));
			/* [한국어] 확장 파티션 슬롯 자체에는 실제 데이터가 아니라 이 작은 보호용 크기(n)만
			 * 등록해, 사용자가 실수로 확장 파티션 전체에 mkfs/mkswap하는 것을 막는다(원본
			 * 주석 참고). */
			put_partition(state, slot, start, n);

			/* [한국어] 로그에 확장 파티션 내부 논리 파티션 목록을 감쌀 여는 괄호를 남긴다. */
			seq_buf_puts(&state->pp_buf, " <");
			/* [한국어] parse_extended()가 EBR 체인을 따라가며 실제 논리 파티션들을 등록한다 -
			 * 체인 길이에 비례해 추가 read_part_sector() 호출(과 필요 시 실제 디스크 I/O)이
			 * 발생한다. */
			parse_extended(state, start, size, disksig);
			/* [한국어] 로그의 닫는 괄호를 남긴다. */
			seq_buf_puts(&state->pp_buf, " >");
			/* [한국어] 확장 파티션 슬롯은 아래 일반 데이터 파티션 등록 코드를 거치지 않고 다음
			 * 슬롯으로 넘어간다. */
			continue;
		}
		/* [한국어] 일반 데이터 파티션을 (절대 LBA, 길이)로 등록한다. */
		put_partition(state, slot, start, size);
		/* [한국어] MBR disksig 기반 유사 UUID 메타 정보를 채운다. */
		set_info(state, slot, disksig);
		/* [한국어] 이 파티션이 Linux RAID 멤버로 표시되어 있다면. */
		if (p->sys_ind == LINUX_RAID_PARTITION)
			/* [한국어] RAID 오토디텍트 힌트 플래그를 세팅한다. */
			state->parts[slot].flags = ADDPART_FLAG_RAID;
		/* [한국어] DM6(DiskManager 6.0x) 타입이면. */
		if (p->sys_ind == DM6_PARTITION)
			/* [한국어] 로그에 "[DM]" 표시를 남겨 DiskManager 변환 파티션임을 알린다(원본
			 * 주석의 DiskManager v6.0x 지원 이력 참고). */
			seq_buf_puts(&state->pp_buf, "[DM]");
		/* [한국어] EZD(EZ-Drive) 타입이면. */
		if (p->sys_ind == EZD_PARTITION)
			/* [한국어] 로그에 "[EZD]" 표시를 남긴다. */
			seq_buf_puts(&state->pp_buf, "[EZD]");
	}

	/* [한국어] 1st pass 로그 라인을 개행으로 마무리한다. */
	seq_buf_puts(&state->pp_buf, "\n");

	/* second pass - output for each on a separate line */
	/* [한국어] 2nd pass를 위해 파티션 테이블 포인터를 다시 엔트리 0번(오프셋 0x1be)으로
	 * 되돌린다. */
	p = (struct msdos_partition *) (0x1be + data);
	/* [한국어] 2nd pass: 4개 기본 파티션을 다시 순회하며 BSD/Solaris/Unixware/Minix 등
	 * 서브 레이블을 추가로 스캔한다. */
	for (slot = 1 ; slot <= 4 ; slot++, p++) {
		/* [한국어] subtypes[] 테이블 검색 키로 쓸 이 엔트리의 sys_ind 값. */
		unsigned char id = p->sys_ind;
		/* [한국어] subtypes[] 배열 검색 인덱스. */
		int n;

		/* [한국어] 길이가 0인 엔트리는 서브 레이블도 있을 수 없으므로 건너뛴다. */
		if (!nr_sects(p))
			continue;

		/* [한국어] subtypes[] 테이블을 sentinel({0, NULL})을 만나거나 id가 일치할 때까지
		 * 선형 검색한다. */
		for (n = 0; subtypes[n].parse && id != subtypes[n].id; n++)
			;

		/* [한국어] 검색이 sentinel까지 도달했다면(parse==NULL) 이 타입에 대응하는 서브
		 * 파서가 없다는 뜻이므로 다음 슬롯으로 넘어간다. */
		if (!subtypes[n].parse)
			continue;
		/* [한국어] 대응하는 서브 파서를 호출한다 - 시작 LBA와 길이를 sector_size로 환산해
		 * 전달하며, 파서 내부에서 추가 read_part_sector() 호출(필요 시 실제 디스크 I/O)이
		 * 발생할 수 있다. */
		subtypes[n].parse(state, start_sect(p) * sector_size,
				  nr_sects(p) * sector_size, slot);
	}
	/* [한국어] LBA 0 버퍼를 최종적으로 반납한다. */
	put_dev_sector(sect);
	/* [한국어] MBR로 성공적으로 인식하고 파싱을 마쳤음을 check.h 계약대로 1로 보고한다. */
	return 1;
}
