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
 * NVMe 관점 파일 개요 (5-10줄 요약)
 * -------------------------------
 * 이 헤더는 EFI GUID Partition Table(GPT)의 온디스크 레이아웃을 정의한다.
 * NVMe SSD의 namespace가 블록 장치로 등록되면, 커널은 LBA 0(보호용 MBR)과
 * LBA 1(GPT 헤더)을 읽어 파티션 테이블을 파싱하는데, 이때 이 헤더의 구조체와
 * 매크로가 사용된다. NVMe 입출력 경로는 submit_bio -> blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)를 거쳐
 * namespace의 특정 LBA에 대한 Read/Write 명령(NVM Read/Write)을 발행하고,
 * 파티션 스캔 단계에서의 LBA 읽기 역시 이 경로를 통해 처리된다. 따라서 이
 * 파일은 NVMe I/O 스택의 최상단(block layer 파티션 관리)과 하드웨어 명령
 * 경로 사이를 잇는 데이터 구조 역할을 한다. 실제 파싱 로직은
 * block/partitions/efi.c에 있으며, 이 파일은 해당 파일과
 * block/partitions/check.c, block/genhd.c 등에서 참조된다.
 */

#ifndef FS_PART_EFI_H_INCLUDED /* 중복 포함 방지: NVMe 빌드 시 include 순서에 영향 없음 */
#define FS_PART_EFI_H_INCLUDED /* 헤더 guard 정의 */

#include <linux/types.h>   /* __le64/__le32/__le16/u8 등: NVMe PRP/SGL DMA 버퍼 타입 기반 */
#include <linux/fs.h>      /* block_device/partition 관련 타입: NVMe namespace 위 파티션 등록 시 사용 */
#include <linux/kernel.h>  /* KERN_* 로그/기본 매크로: GPT 파싱 실패 시 NVMe namespace 탐색 로그 출력 (추정) */
#include <linux/major.h>   /* 블록 major 번호 정의: NVMe 블록 장치(major 259 등) 파티션 번호 할당 근거 */
#include <linux/string.h>  /* memcmp/memcpy 등: GPT 헤더/항목 DMA 버퍼 비교/복사 시 사용 */
#include <linux/efi.h>     /* efi_guid_t: GPT GUID 타입 정의, NVMe namespace GUID 비교의 기반 */
#include <linux/compiler.h> /* __packed/__aligned 등: 온디스크 GPT 레이아웃이 NVMe media에서 그대로 매핑되도록 보장 */

/* 보호용/하이브리드 MBR 관련 상수: NVMe namespace의 LBA 0에서 확인된다. */
#define MSDOS_MBR_SIGNATURE 0xaa55 /* MBR 시그니처(LBA 0 끝 2바이트): read_part_sector(LBA0) -> submit_bio -> nvme_queue_rq -> nvme_submit_cmd(Read, SLBA=0) 수행 후 CQE 완료 시 버퍼 offset 510 확인 */
#define EFI_PMBR_OSTYPE_EFI 0xEF   /* EFI 시스템 파티션 MBR os_type: LBA 0 partition_record[].os_type 비교 값 */
#define EFI_PMBR_OSTYPE_EFI_GPT 0xEE /* GPT 보호용 MBR os_type: LBA 0 읽기 후 이 값이면 GPT 존재 추정, efi_partition() 진입 유도 */

#define GPT_MBR_PROTECTIVE  1 /* 보호용 MBR(LBA 0만 GPT 보호): NVMe namespace 전체를 GPT로 인식 */
#define GPT_MBR_HYBRID      2 /* 하이브리드 MBR: 레거시 MBR 파티션과 GPT 공존, NVMe LBA 0/1 모두 탐색 (추정) */

/*
 * GPT 헤더 시그니처 "EFI PART"를 little-endian 64비트로 인코딩한 값.
 * NVMe Read 명령으로 LBA 1을 읽었을 때 이 값으로 GPT 존재 여부를 판별한다.
 */
#define GPT_HEADER_SIGNATURE 0x5452415020494645ULL /* "EFI PART" LE: read_part_sector(LBA1) -> nvme_submit_cmd(Read, SLBA=1) -> CQE -> ((__le64*)buf)[0] 비교 */
#define GPT_HEADER_REVISION_V1 0x00010000          /* GPT revision 1.0: 헤더 유효성 검증 시 signature 다음 필드로 확인 */
#define GPT_PRIMARY_PARTITION_TABLE_LBA 1          /* GPT 헤더는 일반적으로 LBA 1에 위치: NVMe Read SLBA=1 의미 */

/*
 * 주요 파티션 형식 GUID 매크로.
 * NVMe namespace에 생성된 파티션의 partition_type_guid 필드와 비교되어
 * 해당 파티션의 용도(시스템, MSFT 예약, Linux RAID/LVM/SWAP 등)를 식별한다.
 */
#define PARTITION_SYSTEM_GUID \
    EFI_GUID( 0xC12A7328, 0xF81F, 0x11d2, /* EFI 시스템 파티션: NVMe 부팅 firmware가 읽는 ESP, SLBA=starting_lba */ \
              0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B) 
#define LEGACY_MBR_PARTITION_GUID \
    EFI_GUID( 0x024DEE41, 0x33E7, 0x11d3, /* 레거시 MBR GUID: NVMe namespace에 레거시 파티션 테이블이 내장된 경우 식별 (추정) */ \
              0x9D, 0x69, 0x00, 0x08, 0xC7, 0x81, 0xF3, 0x9F)
#define PARTITION_MSFT_RESERVED_GUID \
    EFI_GUID( 0xE3C9E316, 0x0B5C, 0x4DB8, /* MSFT 예약 파티션: NVMe namespace LBA 범위 중 Windows가 예약한 영역 */ \
              0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE)
#define PARTITION_BASIC_DATA_GUID \
    EFI_GUID( 0xEBD0A0A2, 0xB9E5, 0x4433, /* 일반 데이터 파티션: NVMe namespace 위에서 파일시스템 BIO가 SLBA=starting_lba 기준으로 매핑 */ \
              0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7)
#define PARTITION_LINUX_RAID_GUID \
    EFI_GUID( 0xa19d880f, 0x05fc, 0x4d3b, /* Linux RAID 멤버: NVMe namespace 파티션이 md RAID 구성원으로 등록됨 */ \
              0xa0, 0x06, 0x74, 0x3f, 0x0f, 0x84, 0x91, 0x1e)
#define PARTITION_LINUX_SWAP_GUID \
    EFI_GUID( 0x0657fd6d, 0xa4ab, 0x43c4, /* Linux SWAP 파티션: NVMe namespace의 해당 LBA 범위가 스왑 영역으로 사용 */ \
              0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f)
#define PARTITION_LINUX_LVM_GUID \
    EFI_GUID( 0xe6d6d379, 0xf507, 0x44c2, /* Linux LVM PV: NVMe namespace 파티션을 LVM physical volume로 사용 */ \
              0xa2, 0x3c, 0x23, 0x8f, 0x2a, 0x3d, 0xf9, 0x28)

/*
 * struct gpt_header - GPT 헤더 (일반적으로 LBA 1에 위치)
 *
 * NVMe 동작 연결:
 *  - 이 헤더를 읽기 위해 커널은 namespace에 Read 명령을 발행한다.
 *    submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *    nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를 통해 처리된다.
 *  - header_crc32, partition_entry_array_crc32를 검증하여 DMA 완료 후
 *    PRP/SGL로 전달받은 데이터의 무결성을 소프트웨어적으로 확인한다.
 *  - my_lba, alternate_lba, first_usable_lba, last_usable_lba는 NVMe
 *    namespace의 논리 블록 주소(LBA) 단위이며, namespace의 LBAF 포맷에
 *    따라 실제 바이트 오프셋으로 변환된다.
 */
typedef struct _gpt_header {
	__le64 signature; /* offset 0, "EFI PART" 시그니처: NVMe LBA 1 읽기 후 먼저 확인 */
	__le32 revision; /* offset 8, GPT 사양 revision: header_size와 함께 파싱 호환성 검증 */
	__le32 header_size; /* offset 12, 헤더 자체의 유효 바이트 수: CRC32 계산 범위 결정, NVMe DMA 버퍼 중 이 길이만큼만 유효 */
	__le32 header_crc32; /* offset 16, 헤더 CRC32: DMA 완료 후 PRP/SGL 버퍼 0..header_size-1 계산값과 비교, 불일치 시 NVMe media/metadata 손상으로 간주 */
	__le32 reserved1; /* offset 20, 예약: 반드시 0, NVMe namespace LBA 1 내 zero-filled 영역 */
	__le64 my_lba; /* offset 24, 이 헤더가 위치한 LBA (주로 1): NVMe Read SLBA 검증, primary/backup 구분 */
	__le64 alternate_lba; /* offset 32, 백업 GPT 헤더 LBA: NVMe 장애 복구/미디어 손상 시 alternate 위치에서 read_part_sector() 재시도 (추정) */
	__le64 first_usable_lba; /* offset 40, 사용자 데이터가 시작 가능한 첫 LBA: 파티션 starting_lba 최소값 제한, NVMe namespace 논리 주소 */
	__le64 last_usable_lba; /* offset 48, 사용자 데이터가 끝나는 마지막 LBA: 파티션 ending_lba 최대값 제한, namespace capacity 내 경계 */
	efi_guid_t disk_guid; /* offset 56, 전체 NVMe namespace를 식별하는 GUID: namespace 단위 고유 ID로 매핑 가능 (추정) */
	__le64 partition_entry_lba; /* offset 72, 파티션 항목 배열이 시작하는 LBA: read_part_sector() 반복 호출 시 SLBA 기준, num_partition_entries * sizeof_partition_entry 만큼 NVMe Read 발생 */
	__le32 num_partition_entries; /* offset 80, 파티션 항목 개수: partition_entry_lba 시작으로부터 루프 반복 횟수, NVMe command submission volume 직접 관련 */
	__le32 sizeof_partition_entry; /* offset 84, 각 파티션 항목의 바이트 크기(보통 128): LBA 경계 정렬 계산 및 gpt_entry 포인터 증가 폭 */
	__le32 partition_entry_array_crc32; /* offset 88, 파티션 항목 배열 전체 CRC32: DMA 완료 후 gpt_entry[] 버퍼 무결성 검증 */

	/* The rest of the logical block is reserved by UEFI and must be zero.
	 * EFI standard handles this by:
	 *
	 * uint8_t		reserved2[ BlockSize - 92 ];
	 */
} __packed gpt_header; /* __packed: NVMe media에서 LBA 1을 DMA로 읽은 버퍼와 1:1 매핑 보장, 컴파일러 패딩 배제 */

/*
 * struct gpt_entry_attributes - GPT 파티션 항목의 속성 비트필드
 *
 * NVMe 동작 연결:
 *  - required_to_function 비트가 설정된 파티션은 namespace 기능에 필수적이므로
	*    firmware 업데이트나 NVMe Format 등에서 손상되지 않도록 주의해야 한다.
	*  - type_guid_specific 영역은 파일시스템/RAID/LVM 등 파티션 형식별로 추가
	*    의미를 가지며, NVMe I/O 스케줄러나 I/O hint에 참조될 수 있다 (추정).
	*/
typedef struct _gpt_entry_attributes {
	u64 required_to_function:1; /* bit0, 1이면 해당 파티션이 namespace 동작에 필수: nvme_format_nvm 시 보호/경고 대상 (추정) */
	u64 reserved:47; /* bit1..47, UEFI 예약 비트: NVMe 파티션 스캔 시 무시, 향후 확장 대비 */
        u64 type_guid_specific:16; /* bit48..63, 파티션 형식 GUID별 특수 속성: NVMe I/O hint/우선순위 설정 참조 가능 (추정) */
} __packed gpt_entry_attributes; /* __packed: gpt_entry 내 8바이트 경계로 NVMe media 버퍼와 정확히 일치 */

/*
 * struct gpt_entry - 단일 GPT 파티션 항목
 *
 * NVMe 동작 연결:
 *  - partition_type_guid를 통해 NVMe namespace 위의 파티션 용도를 식별하고,
 *    block/partitions/efi.c는 이를 바탕으로 block_device 파티션을 등록한다.
 *  - starting_lba, ending_lba는 NVMe Read/Write 명령의 SLBA 필드로 직접
 *    변환되어, 파일시스템의 BIO가 nvme_queue_rq에서 PRP/SGL 항목으로
 *    매핑될 때 기준 주소가 된다.
 *  - attributes.required_to_function이 설정된 파티션에 대해서는
 *    nvme_format_nvm 등 파괴적 명령 발행 시 경고/보호 로직이 추가될 수
 *    있다 (추정).
 */
typedef struct _gpt_entry {
	efi_guid_t partition_type_guid; /* offset 0, 파티션 형식 GUID: NVMe 상 파티션 종류 식별 -> put_partition() 전 type_guid 비교로 무시/등록 결정 */
	efi_guid_t unique_partition_guid; /* offset 16, 파티션 고유 GUID: NVMe namespace 내 파티션별 UUID, /dev/disk/by-partuuid 심볼릭 링크 생성 근거 */
	__le64 starting_lba; /* offset 32, NVMe Read/Write SLBA의 기준 시작 LBA: 파일시스템 bio sector + starting_lba -> nvme_queue_rq SLBA */
	__le64 ending_lba; /* offset 40, NVMe Read/Write SLBA의 기준 끝 LBA: 파티션 크기 계산(ending - starting + 1), 경계 초과 BIO는 NVMe CQE LBA Out of Range 가능 (추정) */
	gpt_entry_attributes attributes; /* offset 48, 파티션 속성: required_to_function 등 NVMe namespace 보호 힌트 */
	__le16 partition_name[72/sizeof(__le16)]; /* offset 56, UTF-16LE 파티션 이름: /dev/disk/by-partlabel 생성 시 사용, NVMe I/O 경로와는 무관 */
} __packed gpt_entry; /* __packed: NVMe Read로 가져온 gpt_entry 배열 버퍼와 메모리 레이아웃 일치 */

/*
 * struct gpt_mbr_record - 보호용/하이브리드 MBR 내의 파티션 레코드
 *
 * NVMe 동작 연결:
 *  - NVMe namespace의 LBA 0은 레거시 BIOS/부트로더와의 호환을 위해 보호용
 *    MBR을 포함할 수 있다.
 *  - os_type이 EFI_PMBR_OSTYPE_EFI_GPT(0xEE)이면 이 디스크에 GPT가 있음을
 *    나타낸다. NVMe 부팅 시 firmware/부트로더가 LBA 0을 먼저 읽어 GPT
 *    존재 여부를 판단할 수 있다.
 *  - starting_lba, size_in_lba는 32비트로, 보호용 MBR 범위를 표현한다.
 */
typedef struct _gpt_mbr_record {
	u8	boot_indicator; /* offset 0, unused by EFI, set to 0x80 for bootable: NVMe 부팅 시 firmware/부트로더가 부팅 가능 파티션 식별 (추정) */
	u8	start_head;     /* offset 1, unused by EFI, pt start in CHS: 레거시 CHS 주소, NVMe namespace는 LBA만 사용하므로 GPT 파싱 시 무시 */
	u8	start_sector;   /* offset 2, unused by EFI, pt start in CHS: CHS sector 번호, NVMe LBA layout과 직접 매핑되지 않음 */
	u8	start_track;    /* offset 3, CHS track: GPT 보호용 MBR에서는 0x00/0x02 등 고정값 사용 */
	u8	os_type;        /* offset 4, EFI and legacy non-EFI OS types: LBA 0 partition_record[0].os_type == 0xEE이면 GPT 존재, efi_partition() 호출 분기 */
	u8	end_head;       /* offset 5, unused by EFI, pt end in CHS: CHS 변환 종료 head, NVMe LBA와 무관 */
	u8	end_sector;     /* offset 6, unused by EFI, pt end in CHS: CHS 변환 종료 sector, NVMe LBA와 무관 */
	u8	end_track;      /* offset 7, CHS track: 보호용 MBR에서는 전체 디스크 LBA 범위를 CHS로 채워넣음 (추정) */
	__le32	starting_lba;   /* offset 8, used by EFI - start addr of the on disk pt: 32비트 LBA, 보호용 MBR에서 GPT 시작 위치(주로 1) */
	__le32	size_in_lba;    /* offset 12, used by EFI - size of pt in LBA: 32비트 LBA 개수, 보호용 MBR에서 namespace 끝까지 또는 0xFFFFFFFF */
} __packed gpt_mbr_record; /* __packed: legacy_mbr.partition_record[4] 배열이 NVMe LBA 0 버퍼 offset 446부터 연속 배치 */


/*
 * struct legacy_mbr - LBA 0에 위치하는 레거시 MBR 레이아웃
 *
 * NVMe 동작 연결:
 *  - NVMe namespace가 처음 탐색될 때 LBA 0을 읽어 signature(0xAA55)와
 *    partition_record[0].os_type을 확인한다.
 *  - os_type이 0xEE이면 GPT 보호용 MBR로 인식하고, efi_partition()이
 *    block/partitions/efi.c에서 호출되어 LBA 1 이후의 GPT를 파싱한다.
 *  - boot_code는 레거시 BIOS 부트 코드 영역으로, NVMe 부팅 시 firmware가
 *    해석할 수 있다 (추정).
 */
typedef struct _legacy_mbr {
	u8 boot_code[440]; /* offset 0, 부트 코드: 레거시 BIOS용, NVMe 부팅 시 firmware가 LBA 0을 fetch/execute 가능 (추정) */
	__le32 unique_mbr_signature; /* offset 440, MBR 고유 시그니처: NVMe namespace별 식별자, /dev/disk/by-id 일부 생성 근거 (추정) */
	__le16 unknown; /* offset 444, 미사용/예약: LBA 0 내 2바이트, partition_record 직전 */
	gpt_mbr_record partition_record[4]; /* offset 446, 최대 4개의 MBR 파티션 레코드: [0].os_type 검증이 GPT 스캔 진입 조건 */
	__le16 signature; /* offset 510, MBR 시그니처 0xAA55: LBA 0이 유효한 MBR임을 표시, read_part_sector(LBA0) -> nvme_submit_cmd(Read,SLBA=0) -> CQE -> buf[510..511] 확인 */
} __packed legacy_mbr; /* __packed: NVMe namespace logical block size(512/4096B) 버퍼와 1:1 매핑, MBR 레이아웃 그대로 해석 */

/*
 * 참고: 이 파일은 헤더 파일로, 함수 정의는 포함하지 않는다.
 * 따라서 "key functions"에 대한 블록 주석은 본 파일에 해당하는 코드가 없음.
 * GPT 파싱의 실제 진입점은 block/partitions/efi.c 내의 efi_partition()이며,
 * 이 함수는 block/partitions/check.c -> rescan_partitions() 경로에서
 * NVMe namespace 블록 장치 초기화 시 호출된다 (추정).
 */

/* NVMe 관점 핵심 요약 */
/*
 *  - GPT 헤더/항목 구조는 NVMe namespace의 LBA 단위 주소 체계 위에 정의되며,
 *    파일시스템 BIO가 nvme_queue_rq에서 NVMe Read/Write 명령으로 변환될 때
 *    SLBA, PRP/SGL 계산의 기준 데이터가 된다.
 *  - NVMe SSD가 블록 장치로 등록되면 block/partitions/check.c를 통해
 *    rescan_partitions() -> efi_partition() 호출 경로가 실행되고, 이 때
 *    submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *    nvme_submit_cmd(doorbell) 경로로 LBA 0/1 및 파티션 항목 배열을 읽는다.
 *  - gpt_header의 header_crc32와 partition_entry_array_crc32는 DMA 완료 후
 *    PRP/SGL 버퍼 내용에 대한 소프트웨어 무결성 검증에 사용된다.
 *  - gpt_entry.starting_lba/ending_lba는 NVMe namespace 내에서 해당 파티션의
 *    물리적 LBA 범위를 정의하며, 파티션 경계를 벗어난 BIO 요청은 NVMe
 *    컨트롤러에서 LBA Out of Range 에러로 거부될 수 있다 (추정).
 *  - 보호용 MBR(legacy_mbr)은 레거시 BIOS/부트로더와의 호환을 위해 NVMe
 *    namespace의 LBA 0에 유지되며, os_type == 0xEE일 때 GPT 존재를
 *    알리는 마커 역할을 한다.
 */

#endif /* FS_PART_EFI_H_INCLUDED: NVMe GPT 헤더 파싱 종료, 이후 다른 컴파일 유닛에서 재사용 가능 */
