// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ldm - Part of the Linux-NTFS project.
 *
 * Copyright (C) 2001,2002 Richard Russon <ldm@flatcap.org>
 * Copyright (c) 2001-2007 Anton Altaparmakov
 * Copyright (C) 2001,2002 Jakob Kemi <jakob.kemi@telia.com>
 *
 * Documentation is available at http://www.linux-ntfs.org/doku.php?id=downloads 
 */

/*
 * 파일 상단 요약 (NVMe 관점)
 * --------------------------
 * block/partitions/ldm.h
 *
 * Windows Logical Disk Manager(LDM, 동적 디스크) 파티션 메타데이터를
 * 파싱하기 위한 자료구조와 매직 넘버/상수를 정의한 헤더 파일이다.
 * NVMe SSD 관점에서는 상위 계층의 submit_bio 호출이 blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로
 * 전달되기 전, 먼저 이 헤더에 정의된 구조체들을 이용해 NVMe namespace
 * 내 LBA 영역을 파티션 단위로 분할하는 역할을 수행한다.
 * 실제 파싱 루틴은 ldm.c에 있으며, 본 헤더는 커널의 block/partitions
 * 하위 시스템과 NVMe 드라이버 사이의 분할 정보 인터페이스를 제공한다.
 */

#ifndef _FS_PT_LDM_H_
#define _FS_PT_LDM_H_

#include <linux/types.h>   /* u8/u16/u32/u64: NVMe PRP/SGL entry 크기와 동일한 정수 타입 */
#include <linux/list.h>    /* struct list_head: VBLK fragment 연결 리스트, NVMe 완료 후 재조립에 사용 */
#include <linux/fs.h>      /* block_device 관련 정의; NVMe namespace 위에 생성되는 파티션 장치의 기반 */
#include <linux/unaligned.h> /* 디스크에서 읽은 NVMe 메타데이터는 정렬되지 않을 수 있어 get_unaligned 사용 */
#include <asm/byteorder.h> /* le16/le64 ↔ CPU 변환: NVMe media는 little-endian이 표준 (추정) */

struct parsed_partitions;

/* Magic numbers in CPU format. */
#define MAGIC_VMDB	0x564D4442		/* VMDB: NVMe로 읽은 LDM DB의 VMDB 서명; 일치하지 않으면 파싱 중단 -> CQE 실패와 유사한 오류 경로 */
#define MAGIC_VBLK	0x56424C4B		/* VBLK: 개별 VBLK 레코드 서명; 매 레코드마다 체크하여 잘못된 namespace 메타데이터 거름 */
#define MAGIC_PRIVHEAD	0x5052495648454144ULL	/* PRIVHEAD: LDM private header 매직; NVMe Read로 0번/백업 LBA 읽어 검증 */
#define MAGIC_TOCBLOCK	0x544F43424C4F434BULL	/* TOCBLOCK: 목차 블록 서명; LDM DB 진입 시 첫 번째로 확인하는 섹터 */

/* The defined vblk types. */
#define VBLK_VOL5		0x51		/* Volume,     version 5: NVMe namespace 위 논리 볼륨 */
#define VBLK_CMP3		0x32		/* Component,  version 3: stripe/raid/basic 구성 단위, NVMe I/O 분산 경로 결정 */
#define VBLK_PRT3		0x33		/* Partition,  version 3: NVMe namespace 내 LBA 범위 -> vblk_part */
#define VBLK_DSK3		0x34		/* Disk,       version 3: 물리 디스크 == NVMe namespace (구버전) */
#define VBLK_DSK4		0x44		/* Disk,       version 4: 물리 디스크 == NVMe namespace (신버전) */
#define VBLK_DGR3		0x35		/* Disk Group, version 3: 다중 NVMe namespace 그룹핑 */
#define VBLK_DGR4		0x45		/* Disk Group, version 4: 다중 NVMe namespace 그룹핑 */

/* vblk flags indicating extra information will be present */
#define	VBLK_FLAG_COMP_STRIPE	0x10	/* stripe 컴포넌트 정보 있음 -> NVMe bio가 여러 영역으로 분할될 수 있음 */
#define	VBLK_FLAG_PART_INDEX	0x08	/* 파티션 인덱스 확장 정보 있음 -> 파티션 번호 매핑 정밀화 */
#define	VBLK_FLAG_DGR3_IDS	0x08	/* DGR3 확장 ID 있음 */
#define	VBLK_FLAG_DGR4_IDS	0x08	/* DGR4 확장 ID 있음 */
#define	VBLK_FLAG_VOLU_ID1	0x08	/* 볼륨 확장 ID1 있음 */
#define	VBLK_FLAG_VOLU_ID2	0x20	/* 볼륨 확장 ID2 있음 */
#define	VBLK_FLAG_VOLU_SIZE	0x80	/* 볼륨 크기 확장 필드 있음 -> NVMe 가용 LBA 범위 재계산 */
#define	VBLK_FLAG_VOLU_DRIVE	0x02	/* 드라이브 힌트 확장 정보 있음 */

/* size of a vblk's static parts */
#define VBLK_SIZE_HEAD		16	/* VBLK 공통 헤더 크기; NVMe 메타데이터 읽기 시 record 경계 파악 기준 */
#define VBLK_SIZE_CMP3		22	/* Name and version; vblk_comp 고정 크기, stripe bio 분할 해석 기준 */
#define VBLK_SIZE_DGR3		12	/* Disk Group v3 고정 크기 */
#define VBLK_SIZE_DGR4		44	/* Disk Group v4 고정 크기 */
#define VBLK_SIZE_DSK3		12	/* Disk v3 고정 크기 */
#define VBLK_SIZE_DSK4		45	/* Disk v4 고정 크기 */
#define VBLK_SIZE_PRT3		28	/* Partition v3 고정 크기; start/size offset 계산에 직접 사용 */
#define VBLK_SIZE_VOL5		58	/* Volume v5 고정 크기 */

/* component types */
#define COMP_STRIPE		0x01		/* Stripe-set: NVMe bio를 번갈아가며 여러 namespace LBA에 분산 */
#define COMP_BASIC		0x02		/* Basic disk: 단일 NVMe namespace LBA 범위 직접 매핑 */
#define COMP_RAID		0x03		/* Raid-set: 중복/패리티용 NVMe I/O 추가 발생 가능 (추정) */

/* Other constants. */
#define LDM_DB_SIZE		2048		/* Size in sectors (= 1MiB). */
						/* LDM DB 전체 크기; NVMe Read 명령으로 2048개 섹터(보통 512B)를 읽어 메모리 적재 */

#define OFF_PRIV1		6		/* Offset of the first privhead
					   relative to the start of the
					   device in sectors */
						/* 첫 번째 PRIVHEAD가 NVMe namespace 0번 LBA로부터 6섹터 떨어진 위치; read_part_sector(state, 6, ...) -> NVMe Read LBA=6 */

/* Offsets to structures within the LDM Database in sectors. */
#define OFF_PRIV2		1856		/* Backup private headers. */
						/* PRIVHEAD 백업 #2: NVMe namespace LBA 1856; 메타데이터 손상 시 fallback 읽기 (추정) */
#define OFF_PRIV3		2047		/* PRIVHEAD 백업 #3: NVMe namespace LBA 2047; LDM DB 마지막 섹터 */

#define OFF_TOCB1		1		/* Tables of contents. */
						/* TOCBLOCK #1: LBA 1; NVMe Read LBA=1로 DB 낸부 목차 로드 */
#define OFF_TOCB2		2		/* TOCBLOCK #2: LBA 2; TOC 중복 복사본 */
#define OFF_TOCB3		2045		/* TOCBLOCK #3: LBA 2045; DB 후방 중복 */
#define OFF_TOCB4		2046		/* TOCBLOCK #4: LBA 2046; DB 후방 중복 */

#define OFF_VMDB		17		/* List of partitions. */
						/* VMDB 시작 LBA; 여기서부터 VBLK 레코드 테이블을 순회하며 NVMe 파티션 정보 수집 */

#define LDM_PARTITION		0x42		/* Formerly SFS (Landis). */
						/* MBR-style 파티션 타입; block layer가 NVMe namespace 위 파티션 장치 등록 시 사용 */

#define TOC_BITMAP1		"config"	/* Names of the two defined */
#define TOC_BITMAP2		"log"		/* bitmaps in the TOCBLOCK. */
						/* config/log 비트맵 이름; NVMe로부터 TOC 블록 읽어 문자열 비교 후 해당 비트맵 LBA 추출 */

/*
 * VBLK Fragment handling
 *
 * LDM 데이터베이스는 여러 개의 VBLK 레코드 조각(fragment)로 구성되며,
 * NVMe SSD에서 bio 단위로 읽어들인 메타데이터 페이지들을 재조립할 때
 * 사용된다. submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 읽기 명령이
 * 전달된 후, 완료 인터럽트(CQ entry, CID 기준)에서 이 frag 리스트를
 * 순회하며 조각들을 연결한다.
 */
struct frag {				/* VBLK Fragment handling */
	struct list_head list;		/* 조각들을 연결하는 리스트; NVMe 메타데이터 읽기 완료 후 ldm.c가 순회 */
	u32		group;		/* 동일 VBLK를 구성하는 조각 그룹 ID; CID/PRP 단위가 아닌 논리적 조각 단위 */
	u8		num;		/* Total number of records */
					/* 이 VBLK를 구성하는 총 조각 수; 누락 조각 있으면 메타데이터 불완전 -> 파싱 실패 */
	u8		rec;		/* This is record number n */
					/* 현재 조각 번호; 1-based 또는 0-based (추정); 순서대로 재조립 */
	u8		map;		/* Which portions are in use */
					/* 조각 내 유효 비트맵; NVMe에서 읽은 raw page에서 실제 VBLK 데이터 영역 표시 */
	u8		data[];		/* Flexible array: NVMe로 읽은 메타데이터 바이트를 복사 저장; kzalloc 실패 시 파싱 중단 */
};

/* In memory LDM database structures. */

/*
 * privhead - LDM private header (PRIVHEAD)
 *
 * NVMe namespace 전체에서 LDM 메타데이터 영역의 위치와 크기를
 * 설명한다. 모든 offset/size는 섹터(보통 512B, NVMe LBA 단위와
 * 동일한 단위로 해석) 단위이다.
 *
 * logical_disk_start: NVMe namespace 내 실제 사용자 데이터 영역의
 *     시작 LBA. 이 offset 이전은 LDM 프라이빗 메타데이터 영역이다.
 * logical_disk_size:  논리 디스크 크기로, NVMe namespace 크기에서
 *     메타데이터 영역을 제외한 사용자 가용 LBA 범위와 대응된다.
 * config_start:       LDM 설정(config) 메타데이터가 시작하는 LBA.
 *     NVMe 읽기 명령(PRP/SGL 형식)으로 이 offset을 요청한다.
 * config_size:        LDM 설정 메타데이터의 섹터 수.
 * disk_id:            해당 NVMe namespace를 식별하는 GUID.
 */
struct privhead {			/* Offsets and sizes are in sectors. */
	u16	ver_major;		/* 메이저 버전; NVMe namespace가 지원하는 LDM 형식 호환성 체크 */
	u16	ver_minor;		/* 마이너 버전 */
	u64	logical_disk_start;	/* 사용자 데이터 시작 LBA; NVMe namespace 0번보다 크면 앞부분은 LDM metadata */
	u64	logical_disk_size;	/* 사용자 가용 섹터 수; block layer가 노출하는 용량의 상한 */
	u64	config_start;		/* config 영역 시작 LBA; read_part_sector -> NVMe Read PRP/SGL에 사용 */
	u64	config_size;		/* config 영역 섹터 수; NVMe 명령 nlb 계산 시 사용 */
	uuid_t	disk_id;		/* NVMe namespace GUID; multi-namespace 구성에서 물리적 대상 식별 */
};

/*
 * tocblock - LDM Table of Contents Block (TOCBLOCK)
 *
 * LDM 데이터베이스 낸부의 비트맵(config/log) 위치를 기술한다. NVMe
 * 입장에서는 namespace의 특정 LBA 범위를 읽어 비트맵을 로드하며,
 * 이 비트맵은 이후 VBLK 조각들이 어느 LBA에 저장되어 있는지를
 * 추적하는 데 사용된다 (추정).
 */
struct tocblock {			/* We have exactly two bitmaps. */
	u8	bitmap1_name[16];	/* "config" 이름; NVMe로 읽은 TOC 블록에서 문자열 비교 */
	u64	bitmap1_start;		/* config 비트맵 시작 LBA */
					/* config 비트맵이 위치한 NVMe namespace LBA; read_part_sector()로 읽음 */
	u64	bitmap1_size;		/* config 비트맵 섹터 수 */
					/* 읽을 섹터 수; NVMe Read 명령의 nlb 또는 SGL 길이 계산에 사용 */
	u8	bitmap2_name[16];	/* "log" 이름 */
	u64	bitmap2_start;		/* log 비트맵 시작 LBA */
					/* log 비트맵이 위치한 NVMe namespace LBA */
	u64	bitmap2_size;		/* log 비트맵 섹터 수 */
					/* log 비트맵 크기; NVMe 메타데이터 읽기 범위 산정 */
};

/*
 * vmdb - LDM VMDB (Volume Manager Database header)
 *
 * VBLK(Virtual Block) 레코드 테이블의 전체적인 레이아웃을 기술한다.
 * NVMe 파티션 파서는 vblk_size, vblk_offset을 이용해 namespace에서
 * 연속된 VBLK 레코드들을 읽어오며, 각 레코드는 이후 struct vblk로
 * 해석되어 NVMe LBA -> 파티션 매핑 정보를 생성한다.
 */
struct vmdb {				/* VMDB: The database header */
	u16	ver_major;		/* VMDB 메이저 버전 */
	u16	ver_minor;		/* VMDB 마이너 버전 */
	u32	vblk_size;		/* 개별 VBLK 레코드 크기 (바이트) */
					/* NVMe에서 한 번에 읽을 VBLK record 단위; PRP/SGL 정렬 및 버퍼 크기 결정 */
	u32	vblk_offset;		/* 첫 번째 VBLK까지의 오프셋 (추정) */
					/* VMDB로부터 첫 VBLK까지의 바이트 오프셋; namespace LBA + 이 값으로 실제 데이터 위치 */
	u32	last_vblk_seq;		/* 마지막 VBLK 시퀀스 번호 */
					/* VBLK 조각 재조립 시 누락 여부 확인; 전체 메타데이터가 NVMe로부터 완전히 읽혔는지 판단 */
};

/*
 * vblk_comp - VBLK Component
 *
 * LDM 볼륨의 구성 단위(Component) 정보. stripe/raid/basic 타입에
 * 따라 NVMe로 날아가는 bio가 여러 물리 영역으로 분산될 수 있다.
 * chunksize는 stripe 단위로, NVMe PRP/SGL 기반 분할 I/O의
 * 정렬/단위를 결정하는 데 참조된다 (추정).
 */
struct vblk_comp {			/* VBLK Component */
	u8	state[16];		/* 컴포넌트 상태; 손상 시 NVMe I/O 경로에서 -EIO 반환 가능 */
	u64	parent_id;		/* 상위 볼륨 ID; NVMe namespace -> 볼륨 -> 파티션 트리 구성 */
	u8	type;			/* COMP_STRIPE/COMP_BASIC/COMP_RAID */
					/* COMP_STRIPE면 단일 bio를 chunksize 단위로 여러 NVMe 영역에 분할 */
	u8	children;		/* 하위 구성원 수; stripe_width == children; NVMe queue depth 분산 척도 (추정) */
	u16	chunksize;		/* stripe 크기 (섹터 단위, 추정) */
					/* NVMe PRP/SGL boundary 정렬 기준; chunksize * 512 byte 단위로 I/O 분할 */
};

/*
 * vblk_dgrp - VBLK Disk Group
 *
 * 동적 디스크 그룹 식별자. NVMe namespace 하나가 특정 disk group에
 * 속함을 나타낸다. multi-namespace NVMe SSD 환경에서 여러 namespace가
 * 하나의 disk group을 구성할 수 있다 (추정).
 */
struct vblk_dgrp {			/* VBLK Disk Group */
	u8	disk_id[64];		/* disk group GUID; NVMe SSD 여러 namespace를 논리 그룹으로 묶는 식별자 */
};

/*
 * vblk_disk - VBLK Disk
 *
 * 개별 물리 디스크(여기서는 NVMe namespace)를 표현한다. disk_id는
 * privhead의 disk_id와 연결되며, alt_name은 사용자에게 노출되는
 * 디스크 이름이다. NVMe CQ/SQ 상에서는 namespace ID(nsid)와 결합해
 * 물리적 대상을 식별한다 (추정).
 */
struct vblk_disk {			/* VBLK Disk */
	uuid_t	disk_id;		/* NVMe namespace GUID; privhead.disk_id와 일치해야 올바른 물리 디스크 */
	u8	alt_name[128];		/* 사용자 노출 디스크 이름; /sys/block/nvme*n*p* 레이블링에 간접 사용 (추정) */
};

/*
 * vblk_part - VBLK Partition
 *
 * NVMe namespace 낸부의 하나의 파티션을 기술한다. start, size,
 * volume_offset은 모두 섹터(LBA) 단위이며, 이 필드들이 최종적으로
 * block layer의 parsed_partitions에 반영되어 NVMe I/O 경로에서
 * 파일시스템 bio의 시작 LBA를 결정한다.
 *
 * start:          파티션의 NVMe namespace 상 시작 LBA.
 * size:           파티션 크기 (섹터 수).
 * volume_offset:  상위 볼륨 내에서의 오프셋 (섹터 수, 추정).
 * parent_id:      상위 component 객체 ID.
 * disk_id:        소속 물리 디스크(NVMe namespace) ID.
 * partnum:        파티션 번호.
 */
struct vblk_part {			/* VBLK Partition */
	u64	start;			/* 파티션 시작 LBA; put_partition 시 block layer가 nvme*n*pN 시작 LBA로 등록 */
	u64	size;			/* start, size and vol_off in sectors */
					/* 파티션 섹터 수; NVMe Read/Write 명령의 최대 LBA 범위 제한 */
	u64	volume_offset;		/* 볼륨 내 오프셋 (섹터); 논리 볼륨이 여러 파티션으로 구성될 때 LBA 변환에 사용 */
	u64	parent_id;		/* 상위 component ID; stripe/raid 시 NVMe I/O 분산 결정 */
	u64	disk_id;		/* 소속 NVMe namespace GUID; 잘못된 namespace면 I/O 거부 */
	u8	partnum;		/* 파티션 번호; /dev/nvme*n*pN 의 minor 번호 매핑에 참조 (추정) */
};

/*
 * vblk_volu - VBLK Volume
 *
 * LDM 논리 볼륨 정보. NVMe 입장에서는 여러 파티션(vblk_part)이
 * 모여 구성되는 상위 논리 단위이며, 파일시스템이 마운트하는
 * 최종 블록 장치에 대응한다. partition_type은 MBR 파티션 타입으로,
 * 이 값이 NVMe I/O 경로 자체에 직접 영향을 주지는 않으나
 * 상위 블록 계층에서 파일시스템 식별에 사용된다.
 */
struct vblk_volu {			/* VBLK Volume */
	u8	volume_type[16];	/* 볼륨 타입; NVMe I/O 경로에는 영향 없음, 상위 식별용 */
	u8	volume_state[16];	/* 볼륨 상태; 실패/경고 상태에서 NVMe I/O -EIO 가능 */
	u8	guid[16];		/* 볼륨 GUID; 마운트 지점 식별 */
	u8	drive_hint[4];		/* 드라이브 힌트 */
	u64	size;			/* 볼륨 총 크기 (섹터 수, 추정) */
					/* 모든 구성 파티션 size 합과 일치해야 함; NVMe namespace LBA 범위 초과 시 오류 */
	u8	partition_type;		/* MBR 파티션 타입; block layer가 파티션 장치 등록/노출 시 참조 */
};

/*
 * vblk_head - VBLK standard header
 *
 * 모든 VBLK 레코드 앞에 위치하는 공통 헤더. group은 동일한 VBLK를
 * 구성하는 조각들을 묶는 식별자이며, rec/nrec은 현재 조각 번호와
 * 총 조각 수를 나타낸다. NVMe로부터 메타데이터 페이지를 읽어온 후
 * ldm.c의 파서가 이 헤더를 보고 frag 리스트에 조각을 연결한다.
 */
struct vblk_head {			/* VBLK standard header */
	u32 group;			/* VBLK 조각 그룹 ID; 동일 group끼리 재조립 */
	u16 rec;			/* 현재 조각 번호; nrec와 비교해 누락 검사 */
	u16 nrec;			/* 총 조각 수; NVMe로부터 모두 수신됐는지 확인 */
};

/*
 * vblk - Generalised VBLK
 *
 * LDM 데이터베이스의 범용 레코드. name은 디스플레이 이름,
 * obj_id는 객체 식별자, sequence는 레코드 순서, flags/type은
 * VBLK 플래그와 타입(VBLK_VOL5 등)이다. union 낸부의 구조체는
 * type 필드에 따라 선택적으로 해석된다.
 *
 * NVMe 스택과의 연결: 이 구조체들이 파싱 완료되면
 * block/partitions/check.c 등에서 parsed_partitions를 채우고,
 * 이후 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를 통해
 * 실제 NVMe namespace의 올바른 LBA로 I/O가 전달된다.
 */
struct vblk {				/* Generalised VBLK */
	u8	name[64];		/* 디스플레이 이름; /proc/partitions 등에 노출 */
	u64	obj_id;			/* 객체 ID; VBLK 간 참조 및 트리 구성 키 */
	u32	sequence;		/* 레코드 순서; VBLK 테이블 순회 시 정렬 기준 */
	u8	flags;			/* VBLK_FLAG_*; 추가 필드 존재 여부 -> 파싱 오프셋 결정 */
	u8	type;			/* VBLK_VOL5, VBLK_PRT3 등 */
					/* union의 어떤 구조체를 사용할지 결정; 잘못 파싱 시 NVMe LBA 매핑 오류 */
	union {
		struct vblk_comp comp;	/* COMP_STRIPE/BASIC/RAID: NVMe I/O 분산/직접 매핑 */
		struct vblk_dgrp dgrp;	/* Disk Group: 다중 NVMe namespace 묶음 */
		struct vblk_disk disk;	/* Disk: 단일 NVMe namespace */
		struct vblk_part part;	/* Partition: NVMe namespace 내 LBA 범위 */
		struct vblk_volu volu;	/* Volume: 상위 논리 볼륨 */
	} vblk;
	struct list_head list;	/* 타입별 ldmdb 리스트 연결; 파티션 순회 및 NVMe I/O 매핑 조회용 */
};

/*
 * ldmdb - Cache of the database
 *
 * LDM 메타데이터를 파싱한 결과를 커널 메모리에 캐시한다. ph, toc, vm은
 * 각각 PRIVHEAD/TOCBLOCK/VMDB의 인메모리 복사본이며, v_* 리스트들은
 * 타입별로 정렬된 VBLK 객체들을 연결한다. NVMe 입장에서는 namespace
 * 단위로 이 구조체가 한 번 구축되면, 이후 bio 처리 시 별도의
 * 메타데이터 재읽기 없이 LBA->파티션 매핑을 빠르게 조회할 수 있다.
 */
struct ldmdb {				/* Cache of the database */
	struct privhead ph;		/* PRIVHEAD 인메모리 복사; NVMe namespace LBA 레이아웃 캐시 */
	struct tocblock toc;		/* TOCBLOCK 인메모리 복사; config/log 비트맵 LBA 캐시 */
	struct vmdb     vm;		/* VMDB 인메모리 복사; VBLK record 단위/순회 정보 캐시 */
	struct list_head v_dgrp;	/* disk group 리스트; NVMe namespace 그룹핑 정보 */
	struct list_head v_disk;	/* disk 리스트; 소속 NVMe namespace 목록 */
	struct list_head v_volu;	/* volume 리스트; 마운트 대상 블록 장치 목록 */
	struct list_head v_comp;	/* component 리스트; stripe/raid/basic I/O 분산 정책 */
	struct list_head v_part;	/* partition 리스트; NVMe namespace LBA -> 파티션 매핑의 최종 소스 */
};

/*
 * NVMe 관점 핵심 요약
 * --------------------
 *  * 본 헤더는 Windows LDM(동적 디스크) 파티션 메타데이터를 파싱하기
 *    위한 자료구조를 정의하며, NVMe namespace의 LBA 공간을 논리
 *    볼륨/파티션으로 분할하는 상위 block/partitions 계층의 일부이다.
 *  * 실제 파싱 및 매핑 루틴은 ldm.c에 구현되어 있으며, 그 결과는
 *    block/partitions/check.c 등을 거쳐 parsed_partitions에 반영된다.
 *  * 파티션 정보가 확정된 후, 파일시스템의 submit_bio는
 *    blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *    nvme_submit_cmd(doorbell) 경로를 따라 NVMe SQ/CQ, CID, PRP/SGL
 *    기반의 실제 플래시 읽기/쓰기 명령으로 변환된다.
 *  * struct vblk_part의 start/size 필드가 NVMe I/O에서 최종적으로
 *    사용되는 namespace 상 LBA 범위를 결정한다.
 *  * LDM은 stripe/raid component를 지원하므로, 단일 bio가 NVMe
 *    PRP/SGL을 통해 여러 namespace 영역으로 분산될 수 있다 (추정).
 */

#endif /* _FS_PT_LDM_H_ */
