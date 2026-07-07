/* SPDX-License-Identifier: GPL-2.0-or-later */
/************************************************************
 * EFI GUID Partition Table
 * Per Intel EFI Specification v1.02
 * http://developer.intel.com/technology/efi/efi.htm
 *
 * By Matt Domsch <Matt_Domsch@dell.com>  Fri Sep 22 22:15:56 CDT 2000  
 *   Copyright 2000,2001 Dell Inc.
 ************************************************************/


/*
 * [한국어 설명] EFI GUID Partition Table(GPT) 온디스크 자료구조 정의 (efi.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 UEFI(Unified Extensible Firmware Interface) 스펙이 정의하는
 * GPT(GUID Partition Table)와, GPT 앞에 위치하는 보호용/하이브리드 MBR의
 * "온디스크 레이아웃"을 그대로 반영하는 C 구조체와 매크로를 정의한다.
 * 여기서 "온디스크 레이아웃"이라 함은 디스크에 실제로 기록된 바이트 배치
 * 순서를 그대로 따르는 구조체(__packed 지정, 리틀엔디언 필드 타입 __leNN
 * 사용)를 의미하며, 컴파일러가 필드 사이에 패딩을 넣거나 엔디언을 바꾸면
 * 디스크의 실제 바이트와 구조체 필드가 어긋나 버린다. 이 파일 자체는 어떤
 * 파싱 로직도 담지 않는 순수 자료구조 정의 헤더이며, 실제 읽기/검증/등록
 * 로직은 전부 block/partitions/efi.c에 있다. 이 파일이 없으면 efi.c는
 * GPT 헤더/엔트리의 바이트 오프셋을 매번 매직 넘버로 직접 계산해야 하므로,
 * 이 헤더는 "GPT 스펙과 파싱 코드 사이의 계약(contract)" 역할을 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * block/partitions/efi.c가 디스크의 LBA(Logical Block Address, 논리 블록
 * 주소) 0/1/마지막 LBA를 읽어 각각 legacy_mbr, gpt_header, gpt_entry 배열
 * 포인터로 캐스팅(직접 대입이 아니라 read_lba()가 채운 커널 버퍼를 이
 * 구조체 포인터로 해석)하는데, 이때 이 파일의 타입 정의가 그 해석 기준이
 * 된다. 실행 컨텍스트는 efi.c와 동일하게 디스크 등록/재스캔 시점의 커널
 * 프로세스 컨텍스트이며, 이 헤더 자신은 코드가 없으므로 별도의 실행
 * 컨텍스트를 갖지 않는다(순수 컴파일 타임 타입 정의). 이 헤더가 정의하는
 * 상수(GPT_HEADER_SIGNATURE, EFI_PMBR_OSTYPE_EFI_GPT 등)는 efi.c의 여러
 * 분기 조건에서 "이 디스크가 GPT인가?", "이 헤더가 유효한가?"를 판정하는
 * 리터럴 기준값으로 직접 쓰인다.
 *
 * === 타 모듈과의 연결 ===
 * - block/partitions/efi.c: 이 헤더의 모든 타입/매크로를 사용하는 유일한
 *   구현 파일. is_gpt_valid()가 gpt_header의 시그니처/CRC32/LBA 필드를
 *   검증하고, efi_partition()이 gpt_entry 배열을 순회하며 파티션을 등록한다.
 * - linux/efi.h: efi_guid_t 타입과 EFI_GUID()/efi_guidcmp()/NULL_GUID 등
 *   GUID 비교 헬퍼를 제공한다. 이 파일이 정의하는 PARTITION_*_GUID 매크로는
 *   모두 EFI_GUID() 매크로로 빌드된 상수이며, efi.c가 efi_guidcmp()로
 *   gpt_entry.partition_type_guid와 비교해 파티션 용도를 식별한다.
 * - block/partitions/check.h: struct parsed_partitions를 정의하며,
 *   efi.c가 이 헤더의 구조체로 읽어들인 데이터를 그 안의 parts[] 배열에
 *   최종 반영한다(이 파일 자체는 check.h를 직접 참조하지 않는다).
 * - 데이터 흐름: 디스크 바이트(리틀엔디언, __packed 레이아웃) -> efi.c의
 *   read_lba()/alloc_read_gpt_header()/alloc_read_gpt_entries()가 그대로
 *   커널 버퍼에 복사 -> 그 버퍼를 이 파일의 legacy_mbr, gpt_header,
 *   gpt_entry 포인터로 캐스팅해 필드 단위로 해석 -> le16_to_cpu()/
 *   le32_to_cpu()/le64_to_cpu()로 CPU 네이티브 엔디언으로 변환 후 사용.
 *
 * === 주요 함수/구조체 요약 ===
 * (이 파일은 헤더 전용이며 함수 정의를 포함하지 않는다. 아래는 핵심 구조체.)
 * - gpt_header: LBA 1(primary)과 마지막 LBA(alternate/backup)에 각각 하나씩
 *   존재하는 GPT 헤더 본체. 시그니처, 리비전, CRC32, my_lba/alternate_lba,
 *   first/last_usable_lba, 파티션 엔트리 배열의 위치/개수/크기를 담는다.
 * - gpt_entry: 파티션 엔트리 배열(PTE, Partition Table Entry array)의
 *   원소 하나. 파티션 타입 GUID, 고유 GUID, 시작/끝 LBA, 속성 비트,
 *   UTF-16LE 파티션 이름을 담는다.
 * - gpt_entry_attributes: gpt_entry.attributes 필드의 64비트 비트필드
 *   해석. bit0(필수 파티션), bit1-47(예약), bit48-63(타입별 특수 속성).
 * - gpt_mbr_record / legacy_mbr: GPT보다 앞서 존재하던 레거시 MBR의
 *   파티션 레코드와 LBA 0 전체 레이아웃. GPT는 이 레거시 포맷과의 호환을
 *   위해 LBA 0에 "보호용(protective)" 또는 "하이브리드(hybrid)" MBR을
 *   함께 둔다.
 */
#ifndef FS_PART_EFI_H_INCLUDED  /* [한국어] 헤더 중복 포함(include) 방지 시작: 이 매크로가 이미 정의돼 있으면 아래 전체를 건너뛴다. */
#define FS_PART_EFI_H_INCLUDED  /* [한국어] 위 #ifndef의 짝. 이 토큰이 정의되었다는 사실 자체가 "이 헤더가 이미 한 번 포함됨"의 표시가 된다. */

/*
 * [한국어] 아래 include들은 이 헤더가 정의하는 온디스크 구조체/매크로가
 * 의존하는 기반 타입과 유틸리티를 가져온다. 각 include가 없으면 어떤
 * 컴파일 에러가 나는지까지 함께 적어 둔다.
 */
#include <linux/types.h>  /* [한국어] __le16/__le32/__le64, u8/u64 등 고정폭 정수 타입 정의. 이 타입들이 없으면 gpt_header 등의 필드 폭이 아키텍처마다 달라져 __packed 레이아웃이 깨진다. */
#include <linux/fs.h>  /* [한국어] struct block_device_operations, sector_t 등 블록 계층 타입. efi.c의 find_valid_gpt()가 disk->fops->alternative_gpt_sector를 호출할 때 이 타입 정의가 필요하다. */
#include <linux/kernel.h>  /* [한국어] pr_debug()/pr_warn() 등 커널 로그 매크로와 container_of, ARRAY_SIZE 등 기본 매크로. efi.c 전역에서 진단 로그 출력에 사용된다. */
#include <linux/major.h>  /* [한국어] 블록 디바이스 major 번호 상수 정의. 이 헤더 자체는 major 번호를 직접 쓰지 않지만 관례적으로 fs 관련 헤더들이 함께 include한다. */
#include <linux/string.h>  /* [한국어] memcmp/memcpy/strlen 등 문자열·메모리 유틸리티. gpt_entry.partition_name 처리, GUID 비교 등에 간접적으로 쓰인다. */
#include <linux/efi.h>  /* [한국어] efi_guid_t 타입, EFI_GUID() 매크로, efi_guidcmp(), NULL_GUID 정의. 이 파일의 모든 PARTITION_*_GUID 매크로와 disk_guid/partition_type_guid/unique_partition_guid 필드의 타입 기반이다. */
#include <linux/compiler.h>  /* [한국어] __packed 등 컴파일러 속성 매크로. 이 파일의 모든 구조체(gpt_header, gpt_entry, legacy_mbr 등)에 __packed를 적용해 컴파일러가 필드 사이에 패딩을 넣지 못하게 강제하는 근거. */


/*
 * [한국어] 레거시 MBR(Master Boot Record) 및 보호용 MBR 관련 매직 넘버.
 * GPT 디스크도 LBA 0에는 반드시 이 상수들로 식별 가능한 MBR 유사 구조를
 * 두어야 한다(하위 호환을 위해). efi.c의 is_pmbr_valid()/pmbr_part_valid()가
 * 이 상수들과 legacy_mbr/gpt_mbr_record 필드를 비교해 GPT 존재 여부와
 * protective/hybrid 여부를 판정한다.
 */
#define MSDOS_MBR_SIGNATURE 0xaa55  /* [한국어] MBR 유효성의 마지막 표식. legacy_mbr.signature(오프셋 510, 2바이트)가 이 값과 일치해야 "이 LBA 0이 MBR로서 유효하다"고 인정된다. 값 자체는 x86 리얼모드 시절부터 내려온 역사적 매직 넘버(0x55AA를 리틀엔디언으로 저장하면 바이트열은 0x55,0xAA가 되고, 이를 16비트 정수로 읽으면 0xAA55)이다. */
#define EFI_PMBR_OSTYPE_EFI 0xEF  /* [한국어] 레거시 MBR 파티션 타입 바이트 중 "EFI 시스템 파티션"을 나타내는 값(0xEF). GPT를 쓰지 않는 순수 레거시 MBR 디스크에서 ESP(EFI System Partition)를 표시할 때 쓰이며, 이 파일의 코드에서는 참고용 상수로만 정의되고 efi.c의 판정 로직에서는 EFI_PMBR_OSTYPE_EFI_GPT(0xEE) 쪽이 실제로 비교된다. */
#define EFI_PMBR_OSTYPE_EFI_GPT 0xEE  /* [한국어] 보호용 MBR(protective MBR)의 os_type 값(0xEE). efi.c의 pmbr_part_valid()가 gpt_mbr_record.os_type과 이 값을 비교하여 "이 MBR 파티션 레코드는 GPT를 보호하기 위한 더미 엔트리"임을 판정한다. 레거시(비 GPT 인식) 도구가 이 디스크를 통째로 하나의 큰 파티션으로 오인해 실수로 덮어쓰는 것을 막는 것이 protective MBR의 목적이다. */


/*
 * [한국어] is_pmbr_valid()/pmbr_part_valid()의 반환값이자 find_valid_gpt()가
 * good_pmbr 플래그에 저장하는 값. 단순 bool이 아니라 "보호용인지 하이브리드인지"를
 * 구분해서 로그 메시지(pr_debug("Device has a %s MBR", ...))에 반영한다.
 */
#define GPT_MBR_PROTECTIVE  1  /* [한국어] LBA 0 전체가 단일 GPT 보호 엔트리 하나로만 채워진 "순수 보호용 MBR". 레거시 파티션은 존재하지 않고 GPT만 사용하는 표준적인 경우다. */
#define GPT_MBR_HYBRID      2  /* [한국어] GPT 보호 엔트리 외에 추가로 레거시(non-EFI, non-empty) 파티션 레코드가 함께 존재하는 "하이브리드 MBR". 일부 부트로더(GRUB, rEFIt 등)가 레거시 BIOS 부팅을 지원하기 위해 만들며, is_pmbr_valid()가 4개 레코드를 모두 훑어 os_type이 0xEE도 0x00(미사용)도 아닌 것을 발견하면 이 값으로 승격시킨다. */


/*
 * [한국어] GPT 헤더 자신을 식별하는 시그니처/리비전/기본 위치 상수.
 * is_gpt_valid()가 gpt_header.signature/revision(리비전은 현재 코드에서
 * 직접 비교하지는 않지만 상수로 노출)과 my_lba를 검증할 때 기준값으로 쓰인다.
 */
#define GPT_HEADER_SIGNATURE 0x5452415020494645ULL  /* [한국어] ASCII 문자열 "EFI PART"를 8바이트 리틀엔디언 64비트 정수로 인코딩한 값. 'E'(0x45)가 최하위 바이트, 'T'(0x54)가 최상위 바이트가 되도록 배치되어 있다(온디스크에는 "EFI PART" 그대로, 메모리의 __le64 정수로 읽으면 이 16진수 값). is_gpt_valid()가 le64_to_cpu(gpt->signature)와 이 값을 비교하는 것이 GPT 판별의 첫 관문이다. */
#define GPT_HEADER_REVISION_V1 0x00010000  /* [한국어] UEFI 스펙 GPT 헤더 리비전 1.0을 나타내는 값(상위 16비트=메이저 1, 하위 16비트=마이너 0). 이 헤더 파일은 상수로만 정의하며, 실제 efi.c의 is_gpt_valid()는 현재 이 값을 직접 비교하지 않고 header_size/CRC 등으로 검증을 대체한다(과거 버전과의 호환을 위해 리비전 자체는 느슨하게 다룸). */
#define GPT_PRIMARY_PARTITION_TABLE_LBA 1  /* [한국어] primary GPT 헤더가 위치하는 표준 LBA 번호(=1). find_valid_gpt()가 is_gpt_valid(state, GPT_PRIMARY_PARTITION_TABLE_LBA, ...)를 호출할 때, pmbr_part_valid()가 protective MBR의 starting_lba와 비교할 때 모두 이 상수를 기준으로 삼는다. */


/*
 * [한국어] 표준 파티션 타입 GUID 매크로 모음.
 * EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved,
 *          clock_seq_low, node0..node5) 형태로, RFC 4122 GUID의 표준 필드
 * 배치를 그대로 사용한다. gpt_entry.partition_type_guid 필드에는 매 파티션마다
 * 이 값들 중 하나(혹은 목록에 없는 제3자 GUID)가 기록되며, efi.c의
 * efi_partition()이 efi_guidcmp()로 PARTITION_LINUX_RAID_GUID와만 특별
 * 비교(md RAID 자동 인식 플래그)를 수행하고, 그 외 GUID는 별도 처리 없이
 * 그대로 파티션으로 등록한다(타입을 몰라도 GPT는 파티션을 인식할 수 있다는
 * 것이 MBR 대비 GPT의 장점 중 하나). 값의 출처는 UEFI 스펙 부록과 각 OS
 * 벤더(Microsoft, Linux 커뮤니티)가 등록한 공개 GUID이다.
 */
/* [한국어] EFI 시스템 파티션(ESP, EFI System Partition) 타입 GUID: 부트로더(GRUB, systemd-boot 등)와 UEFI 펌웨어가 부팅 시 마운트하는 FAT 파일시스템 파티션을 식별한다. */
#define PARTITION_SYSTEM_GUID \
    EFI_GUID( 0xC12A7328, 0xF81F, 0x11d2, \
              0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B)   /* [한국어] EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved, clock_seq_low, node[0..5]) 매크로 호출: RFC4122 GUID의 시간 기반 필드 3개 + clock_seq 2개 + 6바이트 node 필드로 128비트 GUID를 리틀엔디언으로 조립한다. */
/* [한국어] 레거시 MBR 파티션을 GPT 안에 표현할 때 쓰는 타입 GUID: 순수 GPT가 아니라 레거시 MBR 파티션 테이블 전체를 하나의 GPT 엔트리로 감싼 특수한 경우(하이브리드 변환 도구가 생성)에 등장한다. */
#define LEGACY_MBR_PARTITION_GUID \
    EFI_GUID( 0x024DEE41, 0x33E7, 0x11d3, \
              0x9D, 0x69, 0x00, 0x08, 0xC7, 0x81, 0xF3, 0x9F)  /* [한국어] EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved, clock_seq_low, node[0..5]) 매크로 호출: RFC4122 GUID의 시간 기반 필드 3개 + clock_seq 2개 + 6바이트 node 필드로 128비트 GUID를 리틀엔디언으로 조립한다. */
/* [한국어] Microsoft가 예약해 둔 파티션 타입 GUID(MSR, Microsoft Reserved Partition): Windows가 부트/파티셔닝 목적으로 예약하며 사용자 데이터는 담지 않는다. */
#define PARTITION_MSFT_RESERVED_GUID \
    EFI_GUID( 0xE3C9E316, 0x0B5C, 0x4DB8, \
              0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE)  /* [한국어] EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved, clock_seq_low, node[0..5]) 매크로 호출: RFC4122 GUID의 시간 기반 필드 3개 + clock_seq 2개 + 6바이트 node 필드로 128비트 GUID를 리틀엔디언으로 조립한다. */
/* [한국어] 범용 데이터 파티션 타입 GUID(Basic Data Partition): Windows NTFS/FAT뿐 아니라 다수의 범용 파일시스템 파티션이 이 타입을 사용한다. Linux에서도 별도 타입 GUID 없이 이 값을 쓰는 경우가 있다. */
#define PARTITION_BASIC_DATA_GUID \
    EFI_GUID( 0xEBD0A0A2, 0xB9E5, 0x4433, \
              0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7)  /* [한국어] EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved, clock_seq_low, node[0..5]) 매크로 호출: RFC4122 GUID의 시간 기반 필드 3개 + clock_seq 2개 + 6바이트 node 필드로 128비트 GUID를 리틀엔디언으로 조립한다. */
/* [한국어] Linux RAID 멤버 디바이스를 나타내는 타입 GUID: efi.c의 efi_partition()이 이 GUID와 일치하면 state->parts[i+1].flags에 ADDPART_FLAG_RAID를 설정해, md(멀티 디스크) 드라이버가 자동으로 RAID 구성원임을 인식하도록 힌트를 남긴다. */
#define PARTITION_LINUX_RAID_GUID \
    EFI_GUID( 0xa19d880f, 0x05fc, 0x4d3b, \
              0xa0, 0x06, 0x74, 0x3f, 0x0f, 0x84, 0x91, 0x1e)  /* [한국어] EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved, clock_seq_low, node[0..5]) 매크로 호출: RFC4122 GUID의 시간 기반 필드 3개 + clock_seq 2개 + 6바이트 node 필드로 128비트 GUID를 리틀엔디언으로 조립한다. */
/* [한국어] Linux SWAP 파티션 타입 GUID: mkswap/swapon 등 스왑 관리 도구와 배포판 설치기가 스왑 영역 식별에 사용한다. */
#define PARTITION_LINUX_SWAP_GUID \
    EFI_GUID( 0x0657fd6d, 0xa4ab, 0x43c4, \
              0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f)  /* [한국어] EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved, clock_seq_low, node[0..5]) 매크로 호출: RFC4122 GUID의 시간 기반 필드 3개 + clock_seq 2개 + 6바이트 node 필드로 128비트 GUID를 리틀엔디언으로 조립한다. */
/* [한국어] Linux LVM(Logical Volume Manager) 물리 볼륨(PV) 타입 GUID: pvcreate로 초기화된 LVM PV가 이 타입으로 표시되어, vgscan 등 LVM 도구가 물리 볼륨 후보를 찾는 근거가 된다. */
#define PARTITION_LINUX_LVM_GUID \
    EFI_GUID( 0xe6d6d379, 0xf507, 0x44c2, \
              0xa2, 0x3c, 0x23, 0x8f, 0x2a, 0x3d, 0xf9, 0x28)  /* [한국어] EFI_GUID(time_low, time_mid, time_hi_and_version, clock_seq_hi_and_reserved, clock_seq_low, node[0..5]) 매크로 호출: RFC4122 GUID의 시간 기반 필드 3개 + clock_seq 2개 + 6바이트 node 필드로 128비트 GUID를 리틀엔디언으로 조립한다. */


/*
 * [한국어 구조체 설명] struct gpt_header (typedef 이름: gpt_header)
 *
 * UEFI 스펙이 정의하는 GPT 헤더 본체를 바이트 단위 그대로 표현하는 온디스크
 * 구조체. primary GPT 헤더는 LBA 1에, alternate(backup) GPT 헤더는 디스크의
 * 마지막 LBA에 각각 하나씩 존재하며, 두 헤더는 my_lba/alternate_lba로 서로를
 * 가리킨다. 이 구조체는 block/partitions/efi.c의 alloc_read_gpt_header()가
 * 디스크에서 읽은 논리 블록 하나(queue_logical_block_size 바이트)를 그대로
 * 이 타입 포인터로 캐스팅해 사용하며, is_gpt_valid()가 시그니처/크기/CRC32/
 * LBA 범위를 검증하고, compare_gpts()가 primary와 alternate 두 헤더의 필드가
 * 서로 정합적인지 교차 검사한다. sizeof(gpt_header)는 92바이트이며(구조체
 * 정의상 14개 필드의 합), 실제 UEFI 스펙 헤더 크기(header_size 필드로 기록된
 * 값, 통상 92)와 반드시 일치할 필요는 없고 header_size >= sizeof(gpt_header)
 * 이며 header_size <= 논리 블록 크기이면 유효하다(나머지는 예약 영역).
 */
typedef struct _gpt_header {
	/* [한국어] signature - GPT 헤더의 첫 8바이트, ASCII "EFI PART"를 리틀엔디언
	 * 64비트로 인코딩한 매직 넘버(GPT_HEADER_SIGNATURE, 0x5452415020494645ULL).
	 * 설정자: 디스크 파티셔닝 도구(parted, gdisk 등)가 GPT를 생성할 때 기록.
	 * 읽는 자: efi.c의 is_gpt_valid()가 le64_to_cpu(gpt->signature)와
	 *   GPT_HEADER_SIGNATURE를 비교하는 것이 GPT 판별의 첫 번째 관문이다.
	 * 값 범위: 반드시 GPT_HEADER_SIGNATURE와 정확히 일치해야 하며, 다르면
	 *   is_gpt_valid()가 즉시 fail 라벨로 점프해 이 헤더를 폐기한다.
	 * 동기화: 읽기 전용으로만 다뤄지며(파싱 시점에 커널이 값을 변경하지
	 *   않음) 별도 락이 필요 없다. */
	__le64 signature;
	/* [한국어] revision - GPT 스펙 리비전 번호(GPT_HEADER_REVISION_V1 =
	 * 0x00010000, 상위 16비트 메이저=1, 하위 16비트 마이너=0).
	 * 설정자: 파티셔닝 도구가 GPT 생성 시 자신이 준수하는 스펙 버전을 기록.
	 * 읽는 자: 이 파일/efi.c 어디에서도 현재는 직접 비교하지 않는다(과거
	 *   버전 호환을 위해 관대하게 처리, header_size/CRC로 유효성을 대체 검증).
	 * 값 범위: 사실상 항상 0x00010000(v1.0)이며, 다른 값이라도 파싱은
	 *   진행된다(추정: 향후 상위 리비전 등장 시 이 필드로 분기 가능).
	 * 동기화: 읽기 전용. */
	__le32 revision;
	/* [한국어] header_size - 이 GPT 헤더 구조체의 유효 바이트 수(통상 92).
	 * 설정자: 파티셔닝 도구. UEFI 스펙상 92바이트 고정이나 필드는 확장
	 *   가능성을 위해 별도로 기록된다.
	 * 읽는 자: is_gpt_valid()가 두 번 검사한다 - (1) queue_logical_block_size
	 *   보다 크면 invalid(헤더가 한 논리 블록을 넘어설 수 없음), (2)
	 *   sizeof(gpt_header)=92보다 작으면 invalid(필수 필드가 잘림).
	 *   또한 efi_crc32() 계산 범위(0..header_size-1)를 결정하는 길이로도 쓰인다.
	 * 값 범위: sizeof(gpt_header) <= header_size <= 논리 블록 크기.
	 * 동기화: 읽기 전용(단, CRC 계산 중 header_crc32 필드는 아래처럼 임시로
	 *   0으로 바뀌었다가 복원되므로 이 필드 자체가 아니라 형제 필드가 임시
	 *   변경되는 것에 유의). */
	__le32 header_size;
	/* [한국어] header_crc32 - 헤더 자신(오프셋 0..header_size-1)에 대한
	 * CRC32 체크섬(efi_crc32() 즉 표준 CRC32에 ~0 시드/최종 ~0 XOR을 적용한
	 * EFI 변형 CRC32). CRC 계산 시에는 관례상 이 필드 자체를 0으로 간주한다.
	 * 설정자: 파티셔닝 도구가 헤더 생성 후 이 필드를 0으로 채우고 계산한
	 *   CRC32 값을 여기에 기록.
	 * 읽는 자: is_gpt_valid()가 origcrc = 기존 값을 저장 -> 필드를 0으로
	 *   세팅 -> efi_crc32()로 재계산 -> origcrc와 비교 -> 필드를 원래 값으로
	 *   복원(cpu_to_le32(origcrc))하는 4단계 절차를 거친다.
	 * 값 범위: 32비트 전체 범위, 실제로는 계산값과 정확히 일치해야 함.
	 * 동기화: 검증 도중 이 필드가 일시적으로 0으로 바뀌었다가 복원되므로,
	 *   이 헤더 버퍼는 단일 스레드(파티션 스캔 컨텍스트)에서만 다뤄져야
	 *   하며 다른 스레드가 동시에 읽으면 일관성이 깨질 수 있다(실제로는
	 *   kmalloc으로 막 할당된 로컬 버퍼이므로 공유되지 않아 문제 없음). */
	__le32 header_crc32;
	/* [한국어] reserved1 - UEFI 스펙상 항상 0이어야 하는 예약 필드.
	 * 설정자: 파티셔닝 도구가 0으로 기록(스펙 준수 시).
	 * 읽는 자: 현재 efi.c는 이 필드를 명시적으로 검사하지 않는다(header_crc32
	 *   검증 범위에는 포함되므로 값이 오염되면 간접적으로 CRC 불일치로 잡힘).
	 * 값 범위: 0 고정(스펙), 향후 확장을 위해 예약됨.
	 * 동기화: 읽기 전용. */
	__le32 reserved1;
	/* [한국어] my_lba - 이 헤더 구조체 자신이 기록되어 있는 LBA. primary
	 * 헤더라면 보통 1, alternate(backup) 헤더라면 디스크의 마지막 LBA.
	 * 설정자: 파티셔닝 도구가 헤더를 쓰는 위치에 맞춰 기록.
	 * 읽는 자: is_gpt_valid()가 lba 인자(자신이 이 헤더를 읽어온 LBA)와
	 *   my_lba가 일치하는지 검사한다 - 이 검사가 primary/alternate 헤더가
	 *   서로 뒤바뀌어 해석되는 것을 막는다. compare_gpts()도 primary.my_lba
	 *   == alternate.alternate_lba 인지 교차 검증한다.
	 * 값 범위: 0 <= my_lba <= last_lba(디스크 끝 LBA).
	 * 동기화: 읽기 전용. */
	__le64 my_lba;
	/* [한국어] alternate_lba - 반대편(backup) 헤더가 위치한 LBA. primary
	 * 헤더에서는 마지막 LBA를, alternate 헤더에서는 1(primary 위치)을 가리킨다.
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: find_valid_gpt()가 primary가 유효할 때
	 *   is_gpt_valid(state, le64_to_cpu(pgpt->alternate_lba), ...)로 alternate
	 *   GPT를 자동으로 찾아가는 데 이 필드를 사용한다(GPT 이중화 복구의
	 *   핵심 연결 고리). compare_gpts()도 primary.alternate_lba ==
	 *   alternate.my_lba, primary.alternate_lba == lastlba를 검사한다.
	 * 값 범위: 0 <= alternate_lba <= last_lba.
	 * 동기화: 읽기 전용. */
	__le64 alternate_lba;
	/* [한국어] first_usable_lba - 파티션 엔트리들이 사용할 수 있는 첫
	 * LBA(이보다 앞쪽은 primary 헤더/PTE 배열 영역이라 파티션이 침범할 수 없음).
	 * 설정자: 파티셔닝 도구가 primary PTE 배열이 끝나는 지점 다음으로 계산.
	 * 읽는 자: is_gpt_valid()가 lastlba(디스크 끝)를 넘지 않는지만 검사하고,
	 *   is_pte_valid()는 이 필드를 직접 쓰지 않는 대신 lastlba만으로 각
	 *   파티션 엔트리의 LBA 범위를 검사한다(이 필드는 주로 파티셔닝 도구가
	 *   새 파티션을 만들 때의 하한 힌트로 쓰인다).
	 * 값 범위: 0 <= first_usable_lba <= last_usable_lba <= last_lba.
	 * 동기화: 읽기 전용. */
	__le64 first_usable_lba;
	/* [한국어] last_usable_lba - 파티션 엔트리들이 사용할 수 있는 마지막
	 * LBA(이보다 뒤쪽은 alternate PTE 배열/헤더 영역이므로 파티션이 넘어갈
	 * 수 없음).
	 * 설정자: 파티셔닝 도구가 alternate PTE 배열이 시작되는 지점 이전으로 계산.
	 * 읽는 자: is_gpt_valid()가 lastlba를 넘지 않는지, first_usable_lba보다
	 *   작지 않은지(역전 방지) 검사한다.
	 * 값 범위: first_usable_lba <= last_usable_lba <= last_lba(디스크의
	 *   실제 마지막 LBA, last_lba()로 계산된 값).
	 * 동기화: 읽기 전용. */
	__le64 last_usable_lba;
	/* [한국어] disk_guid - 이 디스크 전체를 식별하는 128비트 GUID(efi_guid_t).
	 * 설정자: 파티셔닝 도구가 GPT 생성 시 무작위로 새로 생성.
	 * 읽는 자: compare_gpts()가 primary와 alternate의 disk_guid가
	 *   efi_guidcmp()로 일치하는지 검사한다(불일치 시 경고만 출력).
	 * 값 범위: RFC4122 GUID 전체 공간, 사실상 유일값으로 취급.
	 * 동기화: 읽기 전용. */
	efi_guid_t disk_guid;
	/* [한국어] partition_entry_lba - 파티션 엔트리 배열(PTE 배열)이 시작하는
	 * LBA. primary 헤더에서는 보통 LBA 2(헤더 바로 다음), alternate
	 * 헤더에서는 alternate PTE 배열 시작 위치(보통 last_usable_lba 다음).
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: alloc_read_gpt_entries()가 read_lba(state,
	 *   le64_to_cpu(gpt->partition_entry_lba), ...)로 이 위치부터 PTE 배열
	 *   전체를 읽어온다.
	 * 값 범위: 0 <= partition_entry_lba <= last_lba, 통상 first_usable_lba
	 *   보다 작은 영역(primary 헤더 기준).
	 * 동기화: 읽기 전용. */
	__le64 partition_entry_lba;
	/* [한국어] num_partition_entries - PTE 배열의 원소(파티션 엔트리) 개수.
	 * UEFI 스펙 관례상 128개가 표준이지만 스펙상 가변 가능하다.
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: alloc_read_gpt_entries()가 num_partition_entries *
	 *   sizeof_partition_entry로 PTE 배열 전체 바이트 크기를 계산해
	 *   kmalloc()하고, efi_partition()이 이 개수만큼(단, state->limit-1을
	 *   넘지 않는 범위에서) 루프를 돌며 각 엔트리를 파티션으로 등록한다.
	 * 값 범위: 0 이상. is_gpt_valid()는 num_partition_entries *
	 *   sizeof_partition_entry(=pt_size)가 KMALLOC_MAX_SIZE를 넘으면
	 *   invalid로 거부한다(악의적/손상된 값으로부터 커널 메모리 보호).
	 * 동기화: 읽기 전용. */
	__le32 num_partition_entries;
	/* [한국어] sizeof_partition_entry - PTE 배열 원소 하나(gpt_entry)의
	 * 바이트 크기. UEFI 스펙 표준값은 128바이트.
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: is_gpt_valid()가 이 값이 커널의 sizeof(gpt_entry)와 정확히
	 *   일치하는지 검사한다(불일치 시 커널이 이해하는 레이아웃과 디스크
	 *   레이아웃이 어긋나므로 invalid 처리). 일치할 때만
	 *   alloc_read_gpt_entries()의 크기 계산에 num_partition_entries와
	 *   곱해 사용된다.
	 * 값 범위: 반드시 sizeof(gpt_entry)와 같아야 함(현재 커널은 128 고정
	 *   레이아웃만 지원, 다른 값은 전부 거부).
	 * 동기화: 읽기 전용. */
	__le32 sizeof_partition_entry;
	/* [한국어] partition_entry_array_crc32 - PTE 배열 전체(모든
	 * num_partition_entries개 엔트리, 총 pt_size 바이트)에 대한 CRC32
	 * 체크섬.
	 * 설정자: 파티셔닝 도구가 PTE 배열 생성 후 계산해 헤더에 기록.
	 * 읽는 자: is_gpt_valid()가 alloc_read_gpt_entries()로 PTE 배열을 읽은
	 *   뒤 efi_crc32()로 재계산한 값과 이 필드를 비교한다. compare_gpts()도
	 *   primary/alternate 두 헤더의 이 필드가 일치하는지 교차 검사한다.
	 * 값 범위: 32비트 전체, 계산값과 정확히 일치해야 함. 불일치 시
	 *   is_gpt_valid()는 fail_ptes 라벨로 점프해 방금 읽은 PTE 배열까지
	 *   함께 폐기한다(헤더는 CRC가 맞았어도 PTE가 손상되었으면 전체 무효).
	 * 동기화: 읽기 전용. */
	__le32 partition_entry_array_crc32;

	/* The rest of the logical block is reserved by UEFI and must be zero.
	 * EFI standard handles this by:
	 *
	 * uint8_t		reserved2[ BlockSize - 92 ];
	 */
} __packed gpt_header;  /* [한국어] __packed: 컴파일러가 필드 사이에 정렬 패딩을 넣지 못하게 강제한다. 이 구조체가 디스크에서 그대로 읽어온 92바이트 버퍼 위에 캐스팅되므로, 패딩이 하나라도 끼면 그 뒤 모든 필드의 오프셋이 어긋나 잘못된 값을 읽게 된다. */


/*
 * [한국어 구조체 설명] struct gpt_entry_attributes (typedef 이름:
 * gpt_entry_attributes)
 *
 * gpt_entry.attributes 필드의 64비트 값을 UEFI 스펙이 정의한 비트 레이아웃으로
 * 해석하기 위한 비트필드. C 비트필드 순서는 컴파일러/아키텍처 종속적이지만,
 * 리눅스 커널 빌드 대상(리틀엔디언 x86/arm64 등)에서는 이 선언 순서가 LSB부터
 * 채워지는 것으로 일관되게 해석된다. 이 구조체 자체는 별도로 디스크에서
 * 읽히지 않고, gpt_entry.attributes 필드가 이 타입으로 선언되어 있어 그
 * 8바이트가 자동으로 이렇게 해석된다.
 */
typedef struct _gpt_entry_attributes {
	/* [한국어] required_to_function:1 - bit0. UEFI 스펙의
	 * GPT_REQUIRED_TO_FUNCTION 비트. 1이면 "플랫폼이 정상 동작하는 데
	 * 이 파티션이 필수적"이라는 의미로, 펌웨어/OS 설치기가 이 파티션을
	 * 삭제/이동시키지 않도록 하는 힌트다.
	 * 설정자: 파티셔닝 도구/OEM 이미지 빌더.
	 * 읽는 자: 현재 이 커널 파일(efi.c)은 이 비트를 직접 검사하지 않는다
	 *   (추정: 파티션 삭제/포맷 보호는 사용자 공간 도구의 책임으로 위임).
	 * 값 범위: 0 또는 1.
	 * 동기화: 읽기 전용. */
	u64 required_to_function:1;
	/* [한국어] reserved:47 - bit1..bit47. UEFI 스펙이 향후 확장을 위해
	 * 예약한 비트들.
	 * 설정자: 파티셔닝 도구가 0으로 채우는 것이 일반적(스펙 준수 시).
	 * 읽는 자: 현재 커널 코드는 무시한다.
	 * 값 범위: 스펙상 0이어야 하나, 강제 검증은 하지 않는다.
	 * 동기화: 읽기 전용. */
	u64 reserved:47;
	/* [한국어] type_guid_specific:16 - bit48..bit63. 파티션 타입
	 * GUID(gpt_entry.partition_type_guid)마다 별도로 의미가 정의되는
	 * 상위 16비트. 예를 들어 Microsoft 기본 데이터 파티션 타입에서는
	 * bit60=읽기 전용, bit61=섀도우 카피, bit62=숨김, bit63=자동 마운트
	 * 금지 등으로 쓰인다(타입 GUID별 스펙 문서 참조).
	 * 설정자: 파티셔닝 도구/OS(타입별 규약에 따라).
	 * 읽는 자: 현재 커널 코드(efi.c)는 이 필드를 직접 해석하지 않는다
	 *   (추정: 사용자 공간 도구나 특정 타입 GUID 전용 처리기가 있다면
	 *   그쪽에서 해석).
	 * 값 범위: 0..0xFFFF, 해석은 partition_type_guid에 종속적.
	 * 동기화: 읽기 전용. */
        u64 type_guid_specific:16;
} __packed gpt_entry_attributes;  /* [한국어] __packed: 8바이트(64비트) 크기를 그대로 유지시켜 gpt_entry.attributes 오프셋 계산이 어긋나지 않도록 한다. */


/*
 * [한국어 구조체 설명] struct gpt_entry (typedef 이름: gpt_entry)
 *
 * PTE(Partition Table Entry) 배열의 원소 하나로, GPT가 관리하는 파티션 한
 * 개를 표현하는 128바이트(sizeof_partition_entry로 검증됨) 고정 크기
 * 레코드다. is_gpt_valid()가 읽어들인 gpt_entry 배열은 alloc_read_gpt_entries()
 * 가 반환한 연속 메모리 블록이며, efi_partition()이 이 배열을 순회하면서
 * is_pte_valid()로 각 엔트리의 유효성을 검사한 뒤 put_partition()으로 커널
 * 파티션 테이블에 등록한다.
 */
typedef struct _gpt_entry {
	/* [한국어] partition_type_guid - 이 파티션의 "용도/종류"를 나타내는
	 * 128비트 GUID. 이 헤더 상단에 정의된 PARTITION_SYSTEM_GUID,
	 * PARTITION_LINUX_RAID_GUID 등의 상수와 비교된다.
	 * 설정자: 파티셔닝 도구가 사용자가 선택한 파티션 타입에 따라 기록.
	 * 읽는 자: is_pte_valid()가 efi_guidcmp(pte->partition_type_guid,
	 *   NULL_GUID)로 "미사용 엔트리인가"만 검사하고, efi_partition()이
	 *   PARTITION_LINUX_RAID_GUID와 비교해 ADDPART_FLAG_RAID를 설정한다.
	 *   그 외 타입 GUID는 커널이 특별 취급하지 않고 그대로 파티션으로 등록한다.
	 * 값 범위: NULL_GUID(00000000-0000-0000-0000-000000000000, 미사용)
	 *   또는 임의의 128비트 GUID.
	 * 동기화: 읽기 전용. */
	efi_guid_t partition_type_guid;
	/* [한국어] unique_partition_guid - 이 파티션 인스턴스 자체를 식별하는
	 * 고유 GUID(같은 타입의 파티션이 여러 개 있어도 서로 다른 값을 가짐).
	 * 설정자: 파티셔닝 도구가 파티션 생성 시 무작위로 새로 생성.
	 * 읽는 자: efi_partition()이 efi_guid_to_str()로 문자열 UUID로 변환해
	 *   state->parts[i+1].info.uuid에 저장한다 - 이는 사용자 공간에서
	 *   /dev/disk/by-partuuid/<uuid> 심볼릭 링크를 만드는 근거가 된다(udev).
	 * 값 범위: 사실상 유일값으로 취급되는 128비트 GUID.
	 * 동기화: 읽기 전용. */
	efi_guid_t unique_partition_guid;
	/* [한국어] starting_lba - 이 파티션이 시작하는 LBA(포함).
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: is_pte_valid()가 lastlba를 넘지 않는지 검사하고,
	 *   efi_partition()이 start = le64_to_cpu(ptes[i].starting_lba)로 읽어
	 *   put_partition(state, i+1, start*ssz, size*ssz)의 시작 섹터 인자로
	 *   사용한다(ssz = 논리 블록 크기/512, GPT LBA를 커널 512바이트 섹터
	 *   단위로 환산하는 계수).
	 * 값 범위: first_usable_lba(헤더) <= starting_lba <= ending_lba <=
	 *   last_usable_lba(헤더) 이어야 스펙에 부합하나, is_pte_valid()는
	 *   lastlba(디스크 끝)만 검사하고 usable 범위는 별도로 강제하지 않는다.
	 * 동기화: 읽기 전용. */
	__le64 starting_lba;
	/* [한국어] ending_lba - 이 파티션이 끝나는 LBA(포함, exclusive가 아님에
	 * 주의).
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: is_pte_valid()가 lastlba를 넘지 않는지 검사하고,
	 *   efi_partition()이 size = ending_lba - starting_lba + 1(포함 범위이므로
	 *   +1)로 파티션 크기를 계산한다.
	 * 값 범위: starting_lba <= ending_lba <= lastlba.
	 * 동기화: 읽기 전용. */
	__le64 ending_lba;
	/* [한국어] attributes - 위에서 정의한 gpt_entry_attributes 비트필드.
	 * required_to_function, type_guid_specific 등 파티션별 속성 플래그.
	 * 설정자: 파티셔닝 도구/OS.
	 * 읽는 자: 현재 efi.c는 이 필드를 직접 검사하지 않는다(추정: 필요 시
	 *   향후 확장 지점).
	 * 값 범위: gpt_entry_attributes 참고.
	 * 동기화: 읽기 전용. */
	gpt_entry_attributes attributes;
	/* [한국어] partition_name[72/sizeof(__le16)] - UTF-16LE로 인코딩된
	 * 사람이 읽을 수 있는 파티션 이름(총 72바이트 = 36개 __le16 코드 유닛,
	 * 36 UTF-16 문자, null 종료 보장 없음).
	 * 설정자: 파티셔닝 도구가 사용자 지정 레이블을 UTF-16LE로 인코딩해 기록.
	 * 읽는 자: efi_partition()이 utf16_le_to_7bit()로 상위 비트를 잘라낸
	 *   "나이브한" 7비트 ASCII로 변환해 state->parts[i+1].info.volname에
	 *   저장한다(label_max = min(volname 배열 크기-1, 36)만큼만 변환) -
	 *   이는 /dev/disk/by-label/<name> 심볼릭 링크 생성 근거가 된다(udev).
	 * 값 범위: 임의의 UTF-16LE 문자열(정식 유니코드 변환 없이 코드 유닛의
	 *   하위 7비트만 취하므로, ASCII 범위를 벗어난 문자는 손실/오염될 수 있음).
	 * 동기화: 읽기 전용. */
	__le16 partition_name[72/sizeof(__le16)];
} __packed gpt_entry;  /* [한국어] __packed: 128바이트 고정 크기를 강제해, PTE 배열에서 gpt_entry 포인터를 ++ 하거나 인덱싱할 때 정확히 sizeof_partition_entry(128) 바이트씩 이동하도록 보장한다. */


/*
 * [한국어 구조체 설명] struct gpt_mbr_record (typedef 이름: gpt_mbr_record)
 *
 * 레거시 MBR 파티션 테이블의 16바이트 파티션 레코드 하나를 표현한다.
 * legacy_mbr.partition_record[4] 배열의 원소 타입이며, GPT 보호용/하이브리드
 * MBR에서는 이 중 최소 하나가 os_type=0xEE(EFI_PMBR_OSTYPE_EFI_GPT)로 설정되어
 * "이 디스크는 GPT를 사용한다"는 것을 레거시(비GPT 인식) 도구에게 알리는
 * 역할을 한다. CHS(Cylinder-Head-Sector, 실린더-헤드-섹터)로 시작하는 필드들은
 * BIOS 시절의 레거시 주소 지정 방식으로, GPT/EFI는 전적으로 LBA만 사용하므로
 * 이 필드들은 사실상 무시되고 관례적인 고정값으로 채워진다.
 */
typedef struct _gpt_mbr_record {
	/* [한국어] boot_indicator - 부팅 가능 표시(0x80=bootable, 0x00=non-bootable).
	 * EFI/GPT 파싱에서는 사용하지 않음(unused by EFI).
	 * 설정자: 파티셔닝 도구, protective MBR에서는 관례상 0x00.
	 * 읽는 자: 이 커널 파일 어디에서도 검사하지 않는다.
	 * 값 범위: 0x00 또는 0x80(레거시 BIOS 관례), GPT 판정에는 무관.
	 * 동기화: 읽기 전용. */
	u8	boot_indicator; /* unused by EFI, set to 0x80 for bootable */
	/* [한국어] start_head - CHS 시작 주소의 head 필드. EFI에서는 사용하지
	 * 않음(unused by EFI, pt start in CHS).
	 * 설정자: 파티셔닝 도구(레거시 호환 목적으로만 채움).
	 * 읽는 자: 이 커널 파일에서 검사하지 않는다.
	 * 값 범위: 0..255(레거시 CHS 인코딩), GPT 판정과 무관.
	 * 동기화: 읽기 전용. */
	u8	start_head;     /* unused by EFI, pt start in CHS */
	/* [한국어] start_sector - CHS 시작 주소의 sector 필드(레거시 인코딩에서
	 * 상위 실린더 비트와 섞여 저장되는 경우가 있음). EFI에서는 사용하지
	 * 않음(unused by EFI, pt start in CHS).
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: 이 커널 파일에서 검사하지 않는다.
	 * 값 범위: 레거시 CHS 인코딩, GPT 판정과 무관.
	 * 동기화: 읽기 전용. */
	u8	start_sector;   /* unused by EFI, pt start in CHS */
	/* [한국어] start_track - CHS 시작 주소의 track(실린더) 필드.
	 * 설정자: 파티셔닝 도구, protective MBR에서는 관례적 고정값(0x00 또는
	 * 0x02 등, 도구마다 상이).
	 * 읽는 자: 이 커널 파일에서 검사하지 않는다.
	 * 값 범위: 레거시 CHS 인코딩, GPT 판정과 무관.
	 * 동기화: 읽기 전용. */
	u8	start_track;
	/* [한국어] os_type - 파티션 타입 바이트(EFI and legacy non-EFI OS types).
	 * GPT 판별의 핵심 필드.
	 * 설정자: 파티셔닝 도구. protective/hybrid MBR 생성 도구는 최소 한
	 *   레코드에 EFI_PMBR_OSTYPE_EFI_GPT(0xEE)를 기록.
	 * 읽는 자: pmbr_part_valid()가 EFI_PMBR_OSTYPE_EFI_GPT(0xEE)와 비교해
	 *   GPT_MBR_PROTECTIVE 여부를 판정하고, is_pmbr_valid()가 나머지
	 *   레코드들의 os_type이 0xEE도 0x00도 아니면 GPT_MBR_HYBRID로 승격시킨다.
	 * 값 범위: 0xEE(GPT protective), 0xEF(레거시 ESP 마커, 이 파일 상단
	 *   EFI_PMBR_OSTYPE_EFI), 0x00(미사용), 그 외 레거시 파티션 타입 바이트.
	 * 동기화: 읽기 전용. */
	u8	os_type;        /* EFI and legacy non-EFI OS types */
	/* [한국어] end_head - CHS 종료 주소의 head 필드. EFI에서는 사용하지
	 * 않음(unused by EFI, pt end in CHS).
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: 이 커널 파일에서 검사하지 않는다.
	 * 값 범위: 레거시 CHS 인코딩, GPT 판정과 무관.
	 * 동기화: 읽기 전용. */
	u8	end_head;       /* unused by EFI, pt end in CHS */
	/* [한국어] end_sector - CHS 종료 주소의 sector 필드. EFI에서는 사용하지
	 * 않음(unused by EFI, pt end in CHS).
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: 이 커널 파일에서 검사하지 않는다.
	 * 값 범위: 레거시 CHS 인코딩, GPT 판정과 무관.
	 * 동기화: 읽기 전용. */
	u8	end_sector;     /* unused by EFI, pt end in CHS */
	/* [한국어] end_track - CHS 종료 주소의 track(실린더) 필드. protective
	 * MBR에서는 관례적으로 디스크 전체 범위를 CHS로 표현하려는 고정값이
	 * 채워지는 경우가 많다(도구마다 상이).
	 * 설정자: 파티셔닝 도구.
	 * 읽는 자: 이 커널 파일에서 검사하지 않는다.
	 * 값 범위: 레거시 CHS 인코딩, GPT 판정과 무관.
	 * 동기화: 읽기 전용. */
	u8	end_track;      /* unused by EFI, pt end in CHS */
	/* [한국어] starting_lba - 이 MBR 파티션 레코드가 시작하는 32비트 LBA.
	 * EFI가 실제로 사용하는 필드(used by EFI - start addr of the on disk pt).
	 * 설정자: 파티셔닝 도구. protective MBR의 GPT 엔트리에서는 항상 1(=
	 *   GPT_PRIMARY_PARTITION_TABLE_LBA, primary GPT 헤더 위치).
	 * 읽는 자: pmbr_part_valid()가 le32_to_cpu(part->starting_lba) ==
	 *   GPT_PRIMARY_PARTITION_TABLE_LBA(=1)인지 검사한다 - 이것이 두 번째
	 *   GPT 판별 조건(os_type=0xEE 이면서 starting_lba=1).
	 * 값 범위: GPT protective 엔트리는 1 고정, 그 외 레거시 엔트리는 임의의
	 *   32비트 LBA.
	 * 동기화: 읽기 전용. */
	__le32	starting_lba;   /* used by EFI - start addr of the on disk pt */
	/* [한국어] size_in_lba - 이 MBR 파티션 레코드의 크기(LBA 개수, 32비트).
	 * EFI가 실제로 사용하는 필드(used by EFI - size of pt in LBA).
	 * 설정자: 파티셔닝 도구. protective MBR에서는 디스크 전체 크기 - 1 또는,
	 *   32비트로 표현 불가능할 만큼 큰 디스크라면 0xFFFFFFFF(포화 값).
	 * 읽는 자: is_pmbr_valid()가 GPT_MBR_PROTECTIVE로 판정된 레코드에 대해
	 *   sz = le32_to_cpu(part->size_in_lba)를 total_sectors-1과 비교하고,
	 *   다르면서 0xFFFFFFFF도 아니면 단순 경고(pr_debug)만 남긴다(치명적
	 *   오류로 취급하지 않음 - 작은 디스크 이미지를 큰 디스크에 dd로 복사한
	 *   경우 등을 지원하기 위한 관용적 처리).
	 * 값 범위: 0 .. 0xFFFFFFFF(2^32-1, 32비트 LBA로 표현 가능한 약 2TiB
	 *   한계에 해당하는 포화값 포함).
	 * 동기화: 읽기 전용. */
	__le32	size_in_lba;    /* used by EFI - size of pt in LBA */
} __packed gpt_mbr_record;  /* [한국어] __packed: 16바이트 고정 크기를 강제해, legacy_mbr.partition_record[4] 배열이 LBA 0 버퍼의 오프셋 446부터 16바이트씩 연속 배치된 실제 레이아웃과 정확히 대응하게 한다. */



/*
 * [한국어 구조체 설명] struct legacy_mbr (typedef 이름: legacy_mbr)
 *
 * 디스크의 LBA 0(첫 512바이트) 전체 레이아웃을 표현하는 구조체. GPT
 * 디스크에서는 이 영역이 "보호용(protective)" 또는 "하이브리드(hybrid)"
 * MBR로 채워지며, efi.c의 find_valid_gpt()가 legacy_mbr 크기만큼 kzalloc으로
 * 버퍼를 할당하고 read_lba(state, 0, ...)로 LBA 0을 읽어들인 뒤,
 * is_pmbr_valid()로 이 구조체의 필드들을 검사한다. sizeof(legacy_mbr)는
 * 440+4+2+4*16+2 = 512바이트로, 표준 섹터 크기와 정확히 일치한다.
 */
typedef struct _legacy_mbr {
	/* [한국어] boot_code[440] - x86 리얼모드 부트 코드 영역(레거시 BIOS가
	 * LBA 0을 메모리 0x7C00에 로드해 그대로 실행하던 시절의 기계어 코드).
	 * 설정자: 부트로더 설치 도구(GRUB의 grub-install 등)가 1단계 부트
	 *   스테이지 코드를 기록. protective MBR만 있는 순수 UEFI 부팅
	 *   디스크에서는 이 영역이 대부분 0으로 채워지거나, 하이브리드 부팅을
	 *   지원하는 경우 실제 BIOS 부트 코드가 들어간다.
	 * 읽는 자: 이 커널 파일은 이 필드를 파싱하지 않는다(순수 데이터 통과
	 *   영역). 레거시 BIOS 펌웨어만 이 영역을 실행한다.
	 * 값 범위: 임의의 바이트열(x86 기계어 또는 0).
	 * 동기화: 읽기 전용. */
	u8 boot_code[440];
	/* [한국어] unique_mbr_signature - 오프셋 440의 4바이트 "디스크 서명"
	 * (Windows가 NT 드라이브 일련번호로 쓰는 필드, EFI/GPT 스펙 필수 항목은
	 * 아니지만 실제 디스크에는 대부분 존재).
	 * 설정자: OS 설치기(Windows 설치기 등)나 파티셔닝 도구가 무작위/증가
	 *   값으로 기록.
	 * 읽는 자: 이 커널 파일은 이 필드를 검사하지 않는다(추정: 필요 시
	 *   /dev/disk/by-id 계열 식별자 생성에 참고될 수 있으나 현재 GPT
	 *   경로에서는 미사용).
	 * 값 범위: 임의의 32비트 값.
	 * 동기화: 읽기 전용. */
	__le32 unique_mbr_signature;
	/* [한국어] unknown - 오프셋 444의 예약/미사용 2바이트(partition_record
	 * 직전 패딩 영역, 역사적으로 "Copy protected" 플래그 등으로 쓰인 적이
	 * 있으나 GPT/EFI 스펙과는 무관).
	 * 설정자: 도구별로 상이(대개 0).
	 * 읽는 자: 이 커널 파일은 검사하지 않는다.
	 * 값 범위: 임의의 16비트 값, GPT 판정에 영향 없음.
	 * 동기화: 읽기 전용. */
	__le16 unknown;
	/* [한국어] partition_record[4] - 오프셋 446부터 16바이트씩 4개, 총
	 * 64바이트의 레거시 MBR 파티션 레코드 배열(gpt_mbr_record 타입).
	 * 설정자: 파티셔닝 도구. GPT protective MBR에서는 최소 하나의
	 *   레코드가 os_type=0xEE, starting_lba=1로 설정됨.
	 * 읽는 자: is_pmbr_valid()가 4개 레코드를 순회하며 각각
	 *   pmbr_part_valid()로 GPT protective 여부를 검사하고, protective
	 *   레코드를 찾은 뒤에는 나머지 레코드들의 os_type을 검사해
	 *   hybrid MBR 여부를 판정한다.
	 * 값 범위: 각 원소는 gpt_mbr_record 구조체 참고.
	 * 동기화: 읽기 전용. */
	gpt_mbr_record partition_record[4];
	/* [한국어] signature - 오프셋 510의 2바이트 MBR 유효성 시그니처.
	 * MSDOS_MBR_SIGNATURE(0xAA55)와 일치해야 이 LBA 0이 "유효한 MBR로
	 * 해석 가능하다"고 인정된다(BIOS가 부팅 가능 디스크를 판별하던
	 * 전통적 방식과 동일).
	 * 설정자: 모든 MBR/GPT 파티셔닝 도구가 필수로 기록.
	 * 읽는 자: is_pmbr_valid()가 가장 먼저 검사하는 필드
	 *   (!mbr || le16_to_cpu(mbr->signature) != MSDOS_MBR_SIGNATURE 이면
	 *   즉시 invalid) - 이 검사를 통과하지 못하면 partition_record 배열
	 *   자체를 들여다보지 않는다.
	 * 값 범위: 반드시 0xAA55.
	 * 동기화: 읽기 전용. */
	__le16 signature;
} __packed legacy_mbr;  /* [한국어] __packed: boot_code(440)+unique_mbr_signature(4)+unknown(2)+partition_record(64)+signature(2) = 512바이트가 정확히 유지되어야 LBA 0 버퍼(논리 블록 크기가 512인 디바이스 기준)와 오프셋이 어긋나지 않는다. */


/*
 * [한국어] 이 헤더는 함수 정의를 포함하지 않는 순수 자료구조 헤더이므로,
 * "주요 함수 요약"에 해당하는 내용은 상단 4섹션 블록의 "주요 함수/구조체
 * 요약"에 구조체 중심으로 기술했다. GPT 파싱의 실제 진입점
 * efi_partition()과 그 호출 체인(check_partition() -> efi_partition())은
 * block/partitions/efi.c 상단 블록 주석을 참고할 것.
 */
#endif  /* [한국어] FS_PART_EFI_H_INCLUDED: 파일 맨 앞 #ifndef과 짝을 이루는 include guard 종료. */
