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
 * [한국어 설명] Windows LDM(Logical Disk Manager, 동적 디스크) 온디스크 자료구조 정의 (ldm.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Windows가 "동적 디스크(Dynamic Disk)"에 사용하는 LDM 데이터베이스의
 * 온디스크(on-disk) 레이아웃을 리눅스 커널이 이해할 수 있는 C 자료구조로 옮겨
 * 담기 위한 매크로 상수와 in-memory 구조체 정의만 모아 놓은 파일이다. LDM은
 * MS-DOS/MBR 파티션 테이블의 한계(디스크 하나에 종속된 파티션만 표현 가능)를
 * 넘어서기 위해 Windows 2000부터 도입된 방식으로, 여러 물리 디스크에 걸친
 * 스팬/스트라이프/미러/RAID5 볼륨을 하나의 논리 볼륨으로 관리한다. 이 헤더가
 * 정의하는 매직 넘버(MAGIC_*), VBLK 타입 코드(VBLK_*), 플래그(VBLK_FLAG_*),
 * 고정 크기 상수(VBLK_SIZE_*), 섹터 오프셋(OFF_*)은 모두 실제 파서인 ldm.c가
 * 디스크에서 읽은 원시 바이트를 해석할 때 사용하는 "스펙 상수"이며, struct
 * frag/privhead/tocblock/vmdb/vblk_시리즈/vblk/ldmdb는 그 해석 결과를 담는 in-memory
 * 표현이다. 이 파일 자체는 어떤 로직도 담지 않는 순수 자료구조 정의 헤더이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널이 블록 디바이스를 등록(add_disk())하거나 재스캔(BLKRRPART)할 때
 * block/partitions/core.c의 check_partition()이 check_part[] 배열에 등록된
 * 파티션 스킴 프로버들을 순서대로 호출한다. 이 배열에서 ldm_partition()은
 * "msdos_partition보다 먼저" 오도록 배치되는데, 이는 LDM 데이터베이스 자체가
 * MBR 파티션 테이블 안에 타입 0x42(LDM_PARTITION, 이 파일이 정의)인 엔트리로
 * 중첩되어 있어서, 일반 MBR 파서가 그 파티션을 평범한 데이터 파티션으로 오인
 * 등록하기 전에 LDM 프로버가 먼저 가로채야 하기 때문이다. ldm_partition()의
 * 실제 구현(ldm.c)은 이 헤더가 정의한 구조체들을 이용해 PRIVHEAD -> TOCBLOCK
 * -> VMDB -> VBLK 순서로 데이터베이스를 파싱하는 상태 머신을 구성한다. 실행
 * 컨텍스트는 블록 디바이스 초기화/스캔 경로(프로세스 컨텍스트)이며 인터럽트
 * 컨텍스트에서는 쓰이지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더는 struct parsed_partitions를 전방 선언(forward declaration)만 하고
 * 실제 정의는 block/partitions/check.h에 있다 (check.h가 read_part_sector(),
 * put_partition() 등 파티션 프레임워크 공용 API도 함께 선언한다). ldm.c는
 * 이 헤더와 check.h를 모두 include하여, check.h의 read_part_sector()로 디스크
 * 섹터를 읽고 이 헤더의 구조체로 캐스팅/파싱한 뒤 check.h의 put_partition()으로
 * 결과를 커널에 등록한다. 주의할 점: 이 헤더는 uuid_t 타입(struct privhead와
 * struct vblk_disk가 사용)을 쓰면서도 정작 <linux/uuid.h>는 include하지 않는다
 * -- 유일한 사용자인 ldm.c가 자신의 include 목록에서 이 헤더보다 먼저
 * <linux/uuid.h>를 include하기 때문에 우연히 컴파일되는 구조이며, 만약 다른
 * 파일이 uuid.h 없이 이 헤더만 include한다면 컴파일에 실패할 수 있는 암묵적
 * 의존 관계다. 데이터 흐름은 "디스크 원시 섹터(BE 바이트) -> get_unaligned_beNN()
 * 변환 -> 이 헤더의 구조체 필드(CPU 네이티브 정수) -> struct list_head로 연결된
 * ldmdb 캐시 -> put_partition()이 참조하는 절대 LBA/크기"의 순서로 진행된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct privhead: 디스크 전체에 대한 프라이빗 헤더(PRIVHEAD) 1부. 논리 디스크
 *   영역과 LDM 설정 데이터베이스 영역의 시작 LBA/크기, 그리고 이 물리 디스크의
 *   GUID(disk_id)를 담는다. 디스크당 1부이며 3중 백업(OFF_PRIV1/2/3)으로 보호된다.
 * - struct tocblock: 데이터베이스 내 "config"/"log" 두 비트맵 영역의 위치를 담는
 *   목차(TOC). 4중 백업(OFF_TOCB1~4)으로 보호된다.
 * - struct vmdb: VBLK 레코드 테이블의 메타데이터(레코드 크기, 시작 오프셋, 마지막
 *   시퀀스 번호)를 담는 데이터베이스 헤더.
 * - struct vblk / struct vblk_head: 파싱된 VBLK(Virtual Block, 데이터베이스를
 *   구성하는 개별 레코드) 하나의 범용 표현. type 필드에 따라 union vblk 안의
 *   comp/dgrp/disk/part/volu 중 정확히 하나만 유효하다. struct vblk_head는
 *   헤더에는 정의돼 있으나 ldm.c 어디에서도 실제로 사용되지 않는 죽은(dead)
 *   구조체로 보인다 (실제 파편(fragment) 헤더는 struct frag가 담당한다).
 * - struct frag: 512바이트 섹터 하나에 다 담기지 않는 VBLK가 여러 레코드로
 *   조각났을 때, 조각들을 모아 재조립하기 위한 임시 버퍼.
 * - struct ldmdb: 파싱이 끝난 전체 데이터베이스의 캐시. privhead/tocblock/vmdb
 *   각 1부와, VBLK 타입별 5개의 연결 리스트(v_dgrp/v_disk/v_volu/v_comp/v_part)를
 *   담는다. 다만 ldm.c의 최종 파티션 생성 로직은 이 중 v_disk(자기 자신의 디스크
 *   객체 찾기)와 v_part(파티션 목록)만 순회하며, v_dgrp/v_comp/v_volu는 파싱
 *   및 검증만 되고 실제로 순회(list_for_each)되지는 않는다 -- 즉 RAID/스팬
 *   볼륨의 토폴로지 정보 자체는 읽지만, 리눅스는 그것을 재구성하지 않고 물리
 *   디스크에 속한 파티션들을 있는 그대로 노출한다.
 * - 이름이 겹치는 필드 주의: "disk_id"라는 이름이 이 파일 안에서 최소 4가지
 *   서로 다른 의미로 쓰인다 -- struct privhead.disk_id(uuid_t, 물리 디스크
 *   GUID), struct vblk_disk.disk_id(uuid_t, 역시 물리 디스크 GUID, privhead와
 *   매칭됨), struct vblk_part.disk_id(u64, 물리 디스크가 아니라 그 디스크를
 *   나타내는 vblk 레코드의 obj_id를 가리키는 참조), struct vblk_dgrp.disk_id[64]
 *   (문자열, 디스크 그룹 식별 문자열). 코드를 읽을 때 반드시 어느 구조체의
 *   disk_id인지 구분해야 한다.
 */

#ifndef _FS_PT_LDM_H_
#define _FS_PT_LDM_H_
/* [한국어] 이 헤더가 여러 번 include 되어도 구조체/매크로가 중복 정의되지 않도록 막는 표준 include guard. ldm.c가 유일한 사용자이므로 실질적 충돌 위험은 낮지만 관례상 항상 붙인다. */

#include <linux/types.h>
/* [한국어] u8/u16/u32/u64: 이 헤더의 모든 온디스크 구조체 필드가 사용하는 고정폭 정수 타입. LDM 메타데이터는 필드 폭이 스펙에 정확히 고정돼 있어 int/long 같은 가변폭 타입 대신 이들을 쓴다. */
#include <linux/list.h>
/* [한국어] struct list_head: struct frag와 struct vblk가 각각 조각(fragment) 리스트와 타입별 VBLK 리스트(ldmdb.v_dgrp 등)에 연결되기 위해 필요한 커널 표준 이중 연결 리스트 노드. */
#include <linux/fs.h>
/* [한국어] 이 헤더 자체는 block_device/gendisk류의 타입을 직접 쓰지 않지만, 파티션 서브시스템의 관례상 fs.h를 포함해 blkdev 관련 선언이 이 헤더를 include하는 쪽에서 곧바로 보이도록 해 둔 것으로 보인다(추정). */
#include <linux/unaligned.h>
/* [한국어] get_unaligned_be16/32/64(): LDM 온디스크 필드는 임의 바이트 오프셋에 위치해 정렬이 보장되지 않으므로, 정렬되지 않은 메모리에서 안전하게 값을 읽는 이 매크로들이 필요하다. 정작 이 매크로들을 실제로 호출하는 곳은 ldm.c인데, ldm.c는 <linux/unaligned.h>를 직접 include하지 않고 이 헤더를 통해 전이적으로(transitively) 얻어 쓴다 -- 이 헤더가 include 순서상 반드시 먼저 읽혀야 하는 이유 중 하나다. */
#include <asm/byteorder.h>
/* [한국어] le16/le64 등 리틀엔디안 변환 매크로. 다만 ldm.c의 실제 파싱 코드는 거의 전부 get_unaligned_beNN()(빅엔디안)을 사용한다 -- LDM 온디스크 정수 필드가 빅엔디안으로 저장되기 때문. Windows/x86 생태계에서 나온 포맷치고는 이례적인 선택으로, VMDB/VBLK 스펙을 그대로 반영한 결과다. */

struct parsed_partitions;
/* [한국어] struct parsed_partitions의 실제 정의는 block/partitions/check.h에 있다. 이 헤더 자체는 이 구조체를 필드나 프로토타입에 실제로 사용하지 않으므로(현재 ldm_partition()의 프로토타입도 check.h 쪽에 선언돼 있음), 이 전방 선언은 향후 확장이나 과거 버전과의 호환을 위해 남아있는 것으로 보인다(추정) -- 삭제해도 이 헤더 단독으로는 컴파일에 영향이 없어 보인다. */

/* Magic numbers in CPU format. */
/* [한국어] 아래 매직 값들은 "CPU format"이라는 원문 주석대로 이미 호스트 네이티브 정수로 적혀 있다. 디스크에는 이 값들이 빅엔디안 바이트로 저장돼 있으므로, ldm.c는 항상 get_unaligned_beNN()으로 읽은 뒤에 비교한다 -- 즉 이 매크로들 자체는 리틀엔디안/빅엔디안 변환과 무관하며, 비교 시점에 이미 변환이 끝난 값과 맞대어진다. */
#define MAGIC_VMDB	0x564D4442		/* VMDB */
/* [한국어] 'V' 'M' 'D' 'B'의 ASCII 코드를 빅엔디안 32비트로 묶은 값. ldm_parse_vmdb()가 VMDB 섹터 맨 앞 4바이트를 이 값과 비교해 데이터베이스 헤더가 맞는지 검증한다. 불일치 시 파싱 전체를 중단시키는 1차 방어선이다. */
#define MAGIC_VBLK	0x56424C4B		/* VBLK */
/* [한국어] 'V' 'B' 'L' 'K'의 ASCII 코드. ldm_get_vblks()가 VBLK 테이블을 순회하며 각 레코드 슬롯 맨 앞에서 이 시그니처를 확인한다 -- 불일치하면 데이터베이스가 손상됐다고 보고 즉시 중단한다. */
#define MAGIC_PRIVHEAD	0x5052495648454144ULL	/* PRIVHEAD */
/* [한국어] 8바이트 문자열 "PRIVHEAD"를 빅엔디안 64비트 정수로 묶은 값. ldm_parse_privhead()가 PRIVHEAD 섹터(OFF_PRIV1/2/3 중 하나) 맨 앞 8바이트와 비교한다. ULL 접미사는 이 상수가 64비트 부호없는 리터럴임을 명시해 32비트 환경에서도 오버플로 없이 비교되도록 한다. */
#define MAGIC_TOCBLOCK	0x544F43424C4F434BULL	/* TOCBLOCK */
/* [한국어] 8바이트 문자열 "TOCBLOCK"을 빅엔디안 64비트 정수로 묶은 값. ldm_parse_tocblock()이 TOCBLOCK 섹터(OFF_TOCB1~4 중 하나) 맨 앞 8바이트와 비교한다. */

/* The defined vblk types. */
/* [한국어] 이 값들은 각 VBLK 레코드의 buf[0x13] 위치(struct vblk.type)에 저장되는 1바이트 타입 코드다. 숫자 뒤의 아스키 문자는 실제로 이 타입 코드 자체가 사람이 읽을 수 있는 문자('2'~'5', 'D','E','Q')로 정의돼 있음을 보여준다. ldm_parse_vblk()의 switch문과 ldm_ldmdb_add()의 switch문이 각각 파싱 함수 선택과 리스트(v_dgrp/v_disk/v_volu/v_comp/v_part) 분류에 이 값을 사용한다. */
#define VBLK_VOL5		0x51		/* Volume,     version 5 */
/* [한국어] 'Q'(0x51). Volume VBLK, 버전 5 -- 논리 볼륨 하나(단일 파티션 볼륨이거나, 여러 component/partition을 묶은 스팬/RAID 볼륨의 상위 개념)를 표현. ldm_parse_vol5()가 처리하고 ldb->v_volu 리스트에 들어간다. */
#define VBLK_CMP3		0x32		/* Component,  version 3 */
/* [한국어] '2'(0x32). Component VBLK, 버전 3 -- 볼륨을 구성하는 스트라이프/기본/RAID 구성 단위 하나. ldm_parse_cmp3()가 처리하고 ldb->v_comp 리스트에 들어간다. */
#define VBLK_PRT3		0x33		/* Partition,  version 3 */
/* [한국어] '3'(0x33). Partition VBLK, 버전 3 -- 실제로 리눅스 파티션으로 변환되는 유일한 타입. ldm_parse_prt3()가 처리하고 ldb->v_part 리스트에 시작 섹터 순으로 정렬 삽입된다. */
#define VBLK_DSK3		0x34		/* Disk,       version 3 */
/* [한국어] '4'(0x34). Disk VBLK, 구버전(버전 3) -- 물리 디스크 하나를 나타내며 alt_name(문자열)을 함께 담는다. ldm_parse_dsk3()가 처리하고 ldb->v_disk 리스트에 들어간다. */
#define VBLK_DSK4		0x44		/* Disk,       version 4 */
/* [한국어] 'D'(0x44). Disk VBLK, 신버전(버전 4) -- 버전 3과 달리 alt_name을 채우지 않고 GUID(disk_id)만 import_uuid()로 채운다. ldm_parse_dsk4()가 처리한다. */
#define VBLK_DGR3		0x35		/* Disk Group, version 3 */
/* [한국어] '5'(0x35). Disk Group VBLK, 버전 3 -- 여러 디스크를 묶는 디스크 그룹의 식별 문자열을 담는다. ldm_parse_dgr3()가 처리하고 ldb->v_dgrp 리스트에 들어가지만, 이 리스트는 이후 어디서도 순회되지 않는다(파싱만 되고 실질적으로 사용되지 않음). */
#define VBLK_DGR4		0x45		/* Disk Group, version 4 */
/* [한국어] 'E'(0x45). Disk Group VBLK, 버전 4. ldm_parse_dgr4()가 처리하되, 이 함수는 파싱한 이름을 지역 변수 buf[64]에만 저장하고 vb 구조체 어디에도 반영하지 않는다 -- 사실상 유효성 검증 (길이 체크) 목적 외에는 파싱 결과를 버리는 셈이다. */

/* vblk flags indicating extra information will be present */
/* [한국어] 아래 플래그들은 모두 각 VBLK 레코드의 buf[0x12](struct vblk.flags)에 저장되는 비트마스크다. 중요한 함정: 여러 플래그가 같은 비트값(예: 0x08)을 공유한다 -- 그 의미는 현재 파싱 중인 VBLK의 type(PART/DGR3/DGR4/VOLU)에 따라 달라진다. 즉 이 비트들은 전역적으로 유일한 의미를 갖는 것이 아니라, 각 파싱 함수(ldm_parse_prt3/dgr3/dgr4/vol5)마다 문맥적으로 재해석된다. */
#define	VBLK_FLAG_COMP_STRIPE	0x10
/* [한국어] 0x10. Component VBLK 전용: 이 비트가 켜져 있으면 stripe(스트라이프) 레이아웃에 필요한 추가 필드(스트라이프 크기 등)가 뒤에 더 존재한다. ldm_parse_cmp3()가 buffer[0x12] & VBLK_FLAG_COMP_STRIPE로 검사한다. */
#define	VBLK_FLAG_PART_INDEX	0x08
/* [한국어] 0x08. Partition VBLK 전용: 이 비트가 켜져 있으면 파티션 인덱스(partnum) 확장 필드가 존재한다. ldm_parse_prt3()가 검사하며(buffer[0x12] 및 이후 vb->flags 두 곳에서 같은 값을 다시 검사), 꺼져 있으면 part->partnum은 0으로 고정된다. */
#define	VBLK_FLAG_DGR3_IDS	0x08
/* [한국어] 0x08. Disk Group VBLK 버전 3 전용: PART_INDEX와 값은 같지만 의미가 다르다 -- 이 비트가 켜지면 확장 ID 필드 두 개(r_id1/r_id2)가 더 존재함을 뜻한다. ldm_parse_dgr3()가 검사한다. */
#define	VBLK_FLAG_DGR4_IDS	0x08
/* [한국어] 0x08. Disk Group VBLK 버전 4 전용: 역시 값은 0x08로 동일하지만 v4 레이아웃 기준으로 재해석된다. ldm_parse_dgr4()가 검사한다. */
#define	VBLK_FLAG_VOLU_ID1	0x08
/* [한국어] 0x08. Volume VBLK 전용: 값은 여전히 0x08이지만 이번엔 "확장 ID1 필드 존재"를 의미한다. ldm_parse_vol5()가 검사한다. */
#define	VBLK_FLAG_VOLU_ID2	0x20
/* [한국어] 0x20. Volume VBLK 전용: 확장 ID2 필드가 존재함을 의미. ldm_parse_vol5()가 검사한다. */
#define	VBLK_FLAG_VOLU_SIZE	0x80
/* [한국어] 0x80. Volume VBLK 전용: 두 번째 크기(size2) 확장 필드가 존재함을 의미. ldm_parse_vol5()가 검사한다. */
#define	VBLK_FLAG_VOLU_DRIVE	0x02
/* [한국어] 0x02. Volume VBLK 전용: 드라이브 문자 힌트(drive_hint) 확장 필드가 존재함을 의미. ldm_parse_vol5()가 검사하며, 꺼져 있으면 volu->drive_hint는 채워지지 않은 채로 남는다(vb는 kmalloc으로만 할당되고 kzalloc/memset이 없으므로 초기화되지 않은 커널 메모리 그대로 남을 수 있다). */

/* size of a vblk's static parts */
/* [한국어] 아래 상수들은 각 VBLK 타입의 "고정 폭 부분"의 합산 바이트 수다. 각 ldm_parse_*() 함수는 가변폭 필드 오프셋들을 ldm_relative()로 누적 계산한 뒤(len 변수), 여기에 해당 VBLK_SIZE_* 를 더해 레코드에 기록된 전체 길이(buffer + 0x14의 빅엔디안 32비트 값)와 정확히 일치하는지 검증한다. 즉 이 상수들은 무결성 검사의 기준값이지, 직접 오프셋 계산에 쓰이는 값은 아니다. */
#define VBLK_SIZE_HEAD		16
/* [한국어] 16바이트. 모든 VBLK가 공유하는 공통 헤더 크기 -- 매직(4B, 여기서는 검사되지 않고 레코드 재조립 시 그대로 복사됨) + 시퀀스(4B) + 그룹ID(4B) + REC(2B) + NUM(2B). ldm_frag_add()가 조각의 첫 16바이트를 건너뛰어 실제 페이로드 시작 위치를 계산할 때 사용한다. */
#define VBLK_SIZE_CMP3		22		/* Name and version */
/* [한국어] 22바이트. Component v3 타입 전용 고정부 크기(원문 주석: "Name and version"). ldm_parse_cmp3()가 가변폭 필드 총합(len)에 이 값을 더해 buffer+0x14의 선언 길이와 비교한다. */
#define VBLK_SIZE_DGR3		12
/* [한국어] 12바이트. Disk Group v3 타입 전용 고정부 크기. ldm_parse_dgr3()가 사용. */
#define VBLK_SIZE_DGR4		44
/* [한국어] 44바이트. Disk Group v4 타입 전용 고정부 크기. ldm_parse_dgr4()가 사용. */
#define VBLK_SIZE_DSK3		12
/* [한국어] 12바이트. Disk v3 타입 전용 고정부 크기. ldm_parse_dsk3()가 사용. */
#define VBLK_SIZE_DSK4		45
/* [한국어] 45바이트. Disk v4 타입 전용 고정부 크기. ldm_parse_dsk4()가 사용. */
#define VBLK_SIZE_PRT3		28
/* [한국어] 28바이트. Partition v3 타입 전용 고정부 크기. ldm_parse_prt3()가 사용하며, 이 타입만 유일하게 '>' 비교(len > 선언길이)를 쓰고 나머지 타입들은 '!=' 비교를 쓴다는 점이 특이하다 -- Partition/Volume 파서만 초과분을 허용하는 완화된 검사를 하는 셈이다. */
#define VBLK_SIZE_VOL5		58
/* [한국어] 58바이트. Volume v5 타입 전용 고정부 크기. ldm_parse_vol5()가 사용하며 prt3와 마찬가지로 '>' 비교를 사용한다. */

/* component types */
/* [한국어] struct vblk_comp.type에 저장되는 컴포넌트 종류. Windows LDM의 볼륨 구성 방식(스팬/ 스트라이프/RAID)을 나타내지만, 리눅스 파서는 이 값을 읽어 검증만 할 뿐 ldb->v_comp 리스트 자체를 순회하지 않으므로 실제 파티션 생성 로직에는 영향을 주지 않는다. */
#define COMP_STRIPE		0x01		/* Stripe-set */
/* [한국어] 0x01. Stripe-set(스트라이프 집합) -- 여러 물리 디스크에 라운드로빈으로 데이터를 분산 저장하는 구성. 이때만 struct vblk_comp.chunksize가 의미를 가진다(VBLK_FLAG_COMP_STRIPE). */
#define COMP_BASIC		0x02		/* Basic disk */
/* [한국어] 0x02. Basic disk(기본 디스크) -- 단일 디스크에 그대로 매핑되는 가장 단순한 구성. */
#define COMP_RAID		0x03		/* Raid-set */
/* [한국어] 0x03. Raid-set(RAID5 등 중복/패리티 구성) -- 정확한 서브타입까지는 이 파일만으로는 확인할 수 없다(추정: RAID5로 알려져 있음). */

/* Other constants. */
#define LDM_DB_SIZE		2048		/* Size in sectors (= 1MiB). */
/* [한국어] LDM 설정 데이터베이스(PRIVHEAD/TOCBLOCK/VMDB/VBLK 전체)의 고정 크기: 2048섹터 x 512바이트 = 1MiB. ldm_parse_privhead()는 ph->config_size가 이 값과 다르면 에러가 아니라 정보성 로그(ldm_info)만 남기고 계속 진행한다 -- 즉 강제 규칙이 아니라 경험적 기본값 검증이다. */

#define OFF_PRIV1		6		/* Offset of the first privhead
						   relative to the start of the
						   device in sectors */
/* [한국어] 6. 디스크 시작(섹터 0)으로부터 첫 번째(주) PRIVHEAD까지의 상대 섹터 오프셋. ldm_validate_privheads()가 이 값으로 세 PRIVHEAD 사본(주 + 백업 2개) 중 첫 번째를 읽는다. 이 값은 항상 디스크 절대 섹터 0 기준이며, 다른 OFF_* 상수들과 달리 config_start를 더하지 않는다. */

/* Offsets to structures within the LDM Database in sectors. */
#define OFF_PRIV2		1856		/* Backup private headers. */
/* [한국어] 1856. 두 번째 PRIVHEAD 백업의 상대 오프셋. ldm_validate_privheads()의 off[] 배열 두 번째 원소로 쓰이며, 코드 주석대로 config_start(=ph[0]->config_start, 첫 루프 반복에서 0으로 리셋됨) 기준 상대값이다. */
#define OFF_PRIV3		2047
/* [한국어] 2047. 세 번째(마지막) PRIVHEAD 백업의 상대 오프셋 -- 데이터베이스의 맨 마지막 섹터. 홀수 크기 디스크에서는 이 세 번째 백업 읽기가 실패할 수 있어, ldm_validate_privheads()는 이 실패만은 치명적 오류로 취급하지 않고 무시한다(원문 FIXME 주석 참고). */

#define OFF_TOCB1		1		/* Tables of contents. */
/* [한국어] 1. 첫 번째 TOCBLOCK(목차)의 config_start 기준 상대 섹터 오프셋. ldm_validate_tocblocks()가 base(=ph->config_start)에 이 값을 더해 읽는다. */
#define OFF_TOCB2		2
/* [한국어] 2. 두 번째 TOCBLOCK 백업의 상대 오프셋. */
#define OFF_TOCB3		2045
/* [한국어] 2045. 세 번째 TOCBLOCK 백업의 상대 오프셋. */
#define OFF_TOCB4		2046
/* [한국어] 2046. 네 번째(마지막) TOCBLOCK 백업의 상대 오프셋. 네 사본 중 하나라도 유효하면 충분하다는 완화된 정책을 ldm_validate_tocblocks()가 적용한다(Windows Vista에서는 4개가 모두 존재하지 않는 경우가 있기 때문). */

#define OFF_VMDB		17		/* List of partitions. */
/* [한국어] 17. VMDB(및 그 뒤에 이어지는 VBLK 테이블)의 config_start 기준 상대 섹터 오프셋. TOCBLOCK의 첫 번째 비트맵("config")이 가리키는 영역과 사실상 같은 위치를 하드코딩한 값이다. */

#define LDM_PARTITION		0x42		/* Formerly SFS (Landis). */
/* [한국어] 0x42. MBR 파티션 엔트리의 sys_ind(파티션 타입 바이트)에 쓰이던 값으로, 예전에는 "SFS(Landis)" 파일시스템 식별자였으나 Windows가 동적 디스크 표시 용도로 재사용했다. ldm_validate_partition_table()이 MBR의 4개 파티션 엔트리 중 이 타입을 가진 것이 있는지로 "이 디스크가 동적 디스크일 가능성이 있다"는 최초의 약한 판단을 내린다. */

#define TOC_BITMAP1		"config"	/* Names of the two defined */
/* [한국어] "config". TOCBLOCK의 첫 번째 비트맵 이름으로 반드시 이 문자열이어야 한다. ldm_parse_tocblock()이 strncmp()으로 검증하며, 이 비트맵이 바로 VMDB+VBLK 테이블 영역이다. */
#define TOC_BITMAP2		"log"		/* bitmaps in the TOCBLOCK. */
/* [한국어] "log". TOCBLOCK의 두 번째 비트맵 이름으로 반드시 이 문자열이어야 한다. 이 비트맵은 트랜잭션 로그 영역으로 추정되며, 리눅스 파서는 이름 검증만 할 뿐 로그 내용 자체는 읽지 않는다. */

struct frag {				/* VBLK Fragment handling */
/* [한국어]
 * struct frag - 여러 섹터에 걸쳐 조각난(fragmented) VBLK를 재조립하기 위한 임시 버퍼.
 * VBLK 하나의 크기(vm.vblk_size)가 512바이트 섹터 하나보다 커서 여러 개의 512B 레코드 슬롯에
 * 나뉘어 저장된 경우, ldm_get_vblks()가 각 슬롯을 순서 없이 만나는 대로 ldm_frag_add()에 넘기고,
 * 이 구조체가 group ID로 같은 VBLK에 속한 조각들을 한데 모은다. 모든 조각이 모이면
 * ldm_frag_commit()이 재조립된 data[]를 ldm_ldmdb_add()에 넘겨 최종 파싱한다. ldm_get_vblks()
 * 함수 스코프의 지역 리스트(frags)에만 존재하며 사용 후 ldm_frag_free()로 전부 해제된다. */
	/* [한국어] 이 프래그먼트 버퍼를 ldm_get_vblks()의 지역 변수 frags 리스트에 연결하는 노드.
	 * 설정자: ldm_frag_add()가 신규 그룹 생성 시 list_add_tail()로 연결.
	 * 읽는 자: ldm_frag_add()/ldm_frag_commit()/ldm_frag_free()가 list_for_each(_safe)로 순회.
	 * 동기화: ldm_get_vblks() 단일 호출 스코프 안에서만 쓰이므로 락 불필요(단일 스레드 파싱). */
	struct list_head list;
	/* [한국어] 이 프래그먼트 그룹이 속한 VBLK의 그룹 ID(온디스크 data+0x08의 빅엔디안 32비트 값).
	 * 설정자: ldm_frag_add()가 새 struct frag를 만들 때 이번 레코드의 group 값으로 초기화.
	 * 읽는 자: ldm_frag_add()가 다음 레코드를 받을 때 list_for_each로 기존 그룹들과 비교(f->group
	 *   == group)해 같은 그룹인지 찾는 키로 사용.
	 * 값 범위: 임의의 32비트 값(디스크가 부여한 값이므로 이 파일만으로는 유일성 보장 여부 불명,
	 *   충돌 시 서로 다른 VBLK가 하나로 잘못 합쳐질 위험을 원문 코드는 방지하지 않는다 - 추정). */
	u32		group;
	/* [한국어] 이 그룹을 구성하는 전체 조각(레코드) 개수(온디스크 data+0x0E의 빅엔디안 16비트 값,
	 * 원문 변수명 num). 1~4 범위로 ldm_frag_add()가 강제 검증한다.
	 * 설정자: 그룹 최초 생성 시 한 번만 설정.
	 * 읽는 자: found 이후 매 조각마다 rec >= f->num 범위 검사, 그리고 f->map 초기화(0xFF << num)와
	 *   완성 판정(f->map == 0xFF)의 기준이 된다. */
	u8		num;		/* Total number of records */
	/* [한국어] 그룹이 처음 만들어질 때(rec==0이 아니어도) 마주친 첫 조각의 레코드 번호를 그대로
	 * 기록해 둔 필드(원문 주석: "This is record number n").
	 * 설정자: ldm_frag_add()가 새 그룹 생성 시 한 번 f->rec = rec;로 설정.
	 * 읽는 자: 이 파일 전체에서 f->rec를 다시 읽는 코드는 존재하지 않는다 - 사실상 write-only인
	 *   죽은(vestigial) 필드로 보인다(검증: grep 결과 대입 1회, 참조 0회). */
	u8		rec;		/* This is record number n */
	/* [한국어] num개의 조각 중 지금까지 도착한 조각들을 표시하는 비트맵(원문 주석: "Which
	 * portions are in use").
	 * 설정자: 그룹 생성 시 f->map = 0xFF << num;으로 초기화(하위 num비트만 0, 나머지는 1) -
	 *   이렇게 하면 하위 num비트가 모두 OR로 채워졌을 때만 정확히 0xFF가 되도록 설계됨. 이후
	 *   조각이 도착할 때마다 f->map |= (1 << rec);로 해당 비트를 세팅. 중복 조각을 만나면
	 *   f->map &= 0x7F;로 최상위 비트를 강제로 꺼서 이후 다시는 0xFF가 될 수 없게(영구 불량
	 *   그룹으로) 만든다.
	 * 읽는 자: ldm_frag_commit()이 f->map != 0xFF 검사로 완성 여부를 판정.
	 * 값 범위: 0x00~0xFF, 8비트 중 하위 num(1~4)비트만 의미 있는 슬롯 표시로 사용. */
	u8		map;		/* Which portions are in use */
	/* [한국어] 재조립된 VBLK 원본 바이트를 담는 가변 길이 배열(flexible array member).
	 * 레이아웃: [0..VBLK_SIZE_HEAD) = rec==0인 조각이 통째로 복사한 공통 헤더 16바이트,
	 *   [VBLK_SIZE_HEAD + rec*payload_size ..) = 각 조각의 헤더 이후 페이로드.
	 * 설정자: ldm_frag_add()가 kmalloc(sizeof(*f) + size*num, ...)으로 이 배열까지 포함해 한
	 *   번에 할당한 뒤 memcpy()로 채운다.
	 * 읽는 자: ldm_frag_commit()이 f->num * ldb->vm.vblk_size 크기로 ldm_ldmdb_add()에 넘겨
	 *   최종 파싱.
	 * 동기화: 별도 락 없음(단일 파싱 스레드에서만 접근). */
	u8		data[];
};

/* In memory LDM database structures. */
/* [한국어] 여기서부터 정의되는 구조체들은 모두 디스크에서 읽은 바이트를 파싱한 "결과"를 담는
 * in-memory 표현이며, 온디스크 레이아웃과 1:1로 대응하지 않는다(온디스크 오프셋은 ldm.c의
 * 0x.. 매직 넘버 상수들이 직접 표현한다). 즉 이 구조체들의 필드 순서/크기는 C 컴파일러의
 * 자연스러운 정렬을 따르며, __packed 등을 쓰지 않는다. */

struct privhead {			/* Offsets and sizes are in sectors. */
/* [한국어]
 * struct privhead - PRIVHEAD(Private Header) 섹터 하나를 파싱한 in-memory 표현.
 * 물리 디스크마다 정확히 1부 존재하며(디스크 그룹 전체가 아니라 개별 디스크 단위), 디스크 절대
 * 섹터 OFF_PRIV1(=6)에 주 사본이, OFF_PRIV2/OFF_PRIV3(config_start 기준 상대 오프셋)에 두
 * 백업 사본이 존재한다. ldm_validate_privheads()가 세 사본을 모두 읽어 ldm_compare_privheads()로
 * 비교한 뒤 대표값(ph1, 곧 ldb->ph)만 남긴다. 원문 주석대로 여기 담기는 오프셋/크기는 모두
 * "섹터 단위"다. */
	/* [한국어] PRIVHEAD 포맷의 주 버전 번호. 온디스크 data+0x000C(빅엔디안 16비트).
	 * 설정자: ldm_parse_privhead().
	 * 읽는 자: 같은 함수 내에서 ver_minor와 함께 2.11(Win2000/XP) 또는 2.12(Vista)인지 검사;
	 *   ldm_compare_privheads()가 사본 간 일치 여부 비교에도 사용.
	 * 값 범위: 실질적으로 2만 관찰됨(그 외 값이면 파싱 실패로 처리). */
	u16	ver_major;
	/* [한국어] PRIVHEAD 포맷의 부 버전 번호. 온디스크 data+0x000E(빅엔디안 16비트).
	 * 설정자: ldm_parse_privhead().
	 * 읽는 자: ver_major와 조합해 11(2000/XP) 또는 12(Vista) 여부 판정, is_vista 플래그 결정.
	 * 값 범위: 11 또는 12만 유효, 그 외는 에러 처리 후 파싱 실패. */
	u16	ver_minor;
	/* [한국어] "논리 디스크"(사용자에게 보이는 데이터 영역) 시작 LBA. 온디스크 data+0x011B
	 * (빅엔디안 64비트), 디스크 절대 섹터 기준.
	 * 설정자: ldm_parse_privhead().
	 * 읽는 자: ldm_validate_privheads()가 config_start와 겹치지 않는지 범위 검사;
	 *   ldm_create_data_partitions()가 이 값 + 각 파티션의 상대 start를 더해 put_partition()에
	 *   넘길 절대 LBA를 계산 -- 즉 리눅스에 최종 등록되는 파티션 위치의 기준점이다. */
	u64	logical_disk_start;
	/* [한국어] 논리 디스크 영역의 크기(섹터 수). 온디스크 data+0x0123(빅엔디안 64비트).
	 * 설정자: ldm_parse_privhead().
	 * 읽는 자: ldm_validate_privheads()가 logical_disk_start + logical_disk_size가
	 *   config_start를 넘지 않는지 검사(디스크 영역과 DB 영역이 겹치면 손상으로 간주). */
	u64	logical_disk_size;
	/* [한국어] LDM 설정 데이터베이스(TOCBLOCK+VMDB+VBLK 전체) 시작 LBA. 온디스크 data+0x012B
	 * (빅엔디안 64비트), 디스크 절대 섹터 기준.
	 * 설정자: ldm_parse_privhead().
	 * 읽는 자: ldm_partition()이 이 값을 지역 변수 base로 받아 이후 모든 OFF_TOCB* 및 OFF_VMDB
	 *   상대 오프셋 계산의 기준점으로 사용; ldm_validate_privheads()가 num_sects(디스크 전체
	 *   용량) 범위를 넘지 않는지도 검사. */
	u64	config_start;
	/* [한국어] LDM 설정 데이터베이스 크기(섹터 수). 온디스크 data+0x0133(빅엔디안 64비트).
	 * 설정자: ldm_parse_privhead().
	 * 읽는 자: LDM_DB_SIZE(2048)와 다르면 정보성 경고만 남김; ldm_validate_privheads()가
	 *   config_start+config_size가 디스크 용량을 넘지 않는지 검사;
	 *   ldm_validate_tocblocks()가 TOCBLOCK의 비트맵 범위가 이 값 이내인지 검사하는 기준이
	 *   되기도 한다. */
	u64	config_size;
	/* [한국어] 이 물리 디스크의 128비트 GUID. 온디스크 data+0x0030에서 uuid_parse()로 파싱.
	 * 설정자: ldm_parse_privhead().
	 * 읽는 자: ldm_compare_privheads()가 uuid_equal()로 세 PRIVHEAD 사본 간 일치 검증;
	 *   ldm_get_disk_objid()가 v_disk 리스트를 순회하며 각 vblk_disk.disk_id와 uuid_equal()로
	 *   비교해 "현재 이 물리 디스크에 해당하는 Disk VBLK"를 찾는 매칭 키로 사용한다. */
	uuid_t	disk_id;
};

struct tocblock {			/* We have exactly two bitmaps. */
/* [한국어]
 * struct tocblock - TOCBLOCK(Table Of Contents) 섹터 하나를 파싱한 in-memory 표현.
 * 데이터베이스(config_start 기준) 안에 정확히 두 개의 비트맵("config"와 "log")이 있다는
 * 전제(원문 주석: "We have exactly two bitmaps") 하에 그 위치/크기만 담는다. 최대 4개 사본
 * (OFF_TOCB1~4)이 존재할 수 있으며 ldm_validate_tocblocks()가 최소 1개 이상 유효하면 통과시키고
 * 서로 다른 사본끼리는 ldm_compare_tocblocks()로 비교한다. */
	/* [한국어] 첫 번째 비트맵의 이름 문자열(널 패딩, strscpy_pad()로 복사). 온디스크 data+0x24.
	 * 설정자: ldm_parse_tocblock().
	 * 읽는 자: 같은 함수에서 TOC_BITMAP1("config")과 strncmp()로 일치하는지 검증 -- 다르면
	 *   파싱 실패로 처리.
	 * 값 범위: 정상적인 디스크라면 항상 "config". */
	u8	bitmap1_name[16];
	/* [한국어] 첫 번째("config") 비트맵의 시작 오프셋(섹터, config_start 기준 상대값).
	 * 온디스크 data+0x2E(빅엔디안 64비트).
	 * 설정자: ldm_parse_tocblock().
	 * 읽는 자: 이 필드 자체를 직접 오프셋 계산에 쓰는 코드는 없고(OFF_VMDB가 대신 하드코딩됨),
	 *   ldm_validate_tocblocks()가 bitmap1_start+bitmap1_size가 ph->config_size 이내인지
	 *   범위 검사하는 데만 쓰인다. */
	u64	bitmap1_start;
	/* [한국어] 첫 번째("config") 비트맵의 크기(섹터). 온디스크 data+0x36(빅엔디안 64비트).
	 * 설정자: ldm_parse_tocblock().
	 * 읽는 자: ldm_validate_tocblocks()의 범위 검사;
	 *   ldm_validate_vmdb()가 (vblk_size*last_vblk_seq) <= (bitmap1_size << 9)(섹터->바이트
	 *   변환)로 VBLK 테이블 전체가 이 비트맵 크기를 넘지 않는지 검사. */
	u64	bitmap1_size;
	/* [한국어] 두 번째 비트맵의 이름 문자열. 온디스크 data+0x46.
	 * 설정자: ldm_parse_tocblock().
	 * 읽는 자: TOC_BITMAP2("log")와 일치 검증. */
	u8	bitmap2_name[16];
	/* [한국어] 두 번째("log") 비트맵의 시작 오프셋(섹터). 온디스크 data+0x50(빅엔디안 64비트).
	 * 설정자: ldm_parse_tocblock().
	 * 읽는 자: ldm_validate_tocblocks()의 범위 검사 외에는 사용되지 않는다(로그 영역 자체를
	 *   읽지는 않음). */
	u64	bitmap2_start;
	/* [한국어] 두 번째("log") 비트맵의 크기(섹터). 온디스크 data+0x58(빅엔디안 64비트).
	 * 설정자: ldm_parse_tocblock().
	 * 읽는 자: ldm_validate_tocblocks()의 범위 검사. */
	u64	bitmap2_size;
};

struct vmdb {				/* VMDB: The database header */
/* [한국어]
 * struct vmdb - VMDB(Virtual [Machine? Media?] DataBase, 원문 주석: "The database header")
 * 섹터 하나를 파싱한 in-memory 표현. config_start + OFF_VMDB(=17) 위치에 정확히 1부 존재하며
 * (백업 없음), 그 뒤로 VBLK 레코드들이 이어지는 테이블 전체의 메타데이터(레코드 크기/시작
 * 오프셋/총 개수)를 담는다. */
	/* [한국어] VMDB 포맷 주 버전. 온디스크 data+0x12(빅엔디안 16비트).
	 * 설정자: ldm_parse_vmdb().
	 * 읽는 자: 같은 함수에서 ver_minor와 함께 정확히 4.10인지 검사(privhead와 달리 단일 버전만
	 *   허용, 다르면 즉시 실패). */
	u16	ver_major;
	/* [한국어] VMDB 포맷 부 버전. 온디스크 data+0x14(빅엔디안 16비트).
	 * 설정자: ldm_parse_vmdb().
	 * 읽는 자: ver_major와 조합해 4.10 검사. */
	u16	ver_minor;
	/* [한국어] VBLK 레코드 하나의 온디스크 크기(바이트). 온디스크 data+0x08(빅엔디안 32비트).
	 * 설정자: ldm_parse_vmdb() (0이면 즉시 에러 처리).
	 * 읽는 자: ldm_get_vblks()가 perbuf = 512/size(섹터당 VBLK 개수)를 계산하는 분모, 그리고
	 *   finish = (size*last_vblk_seq)>>9(스캔할 총 섹터 수)의 승수로 사용;
	 *   ldm_frag_commit()이 재조립 길이(f->num*vblk_size) 계산에도 사용. */
	u32	vblk_size;
	/* [한국어] VBLK 테이블이 VMDB 헤더로부터 시작하는 바이트 오프셋. 온디스크 data+0x0C
	 * (빅엔디안 32비트).
	 * 설정자: ldm_parse_vmdb() (512가 아니면 정보성 로그만 남김).
	 * 읽는 자: ldm_get_vblks()가 skip = vblk_offset >> 9(바이트->섹터 변환)로 스캔 시작 섹터를
	 *   결정. */
	u32	vblk_offset;
	/* [한국어] 데이터베이스에 기록된 마지막 유효 VBLK의 시퀀스 번호. 온디스크 data+0x04
	 * (빅엔디안 32비트).
	 * 설정자: ldm_parse_vmdb().
	 * 읽는 자: ldm_validate_vmdb()의 크기 범위 검사; ldm_get_vblks()가 finish(스캔 종료 섹터)
	 *   계산에 사용해 VBLK 테이블 전체를 몇 섹터까지 읽어야 하는지 결정한다. */
	u32	last_vblk_seq;
};

struct vblk_comp {			/* VBLK Component */
/* [한국어]
 * struct vblk_comp - VBLK Component(컴포넌트) 레코드의 타입별 페이로드.
 * 하나의 Volume을 구성하는 스트라이프/기본/RAID 구성 단위 하나를 표현하며, 실제 파티션 위치
 * 정보는 담지 않고(그건 vblk_part의 몫) 상위 Volume과의 관계(parent_id)와 구성 방식만 담는다.
 * ldm_parse_cmp3()가 채우며 ldb->v_comp 리스트에 들어가지만, 이 리스트는 이후 순회되지 않아
 * 실질적인 파티션 생성에는 관여하지 않는다. */
	/* [한국어] 컴포넌트 상태 이름 문자열(예: "ACTIVE", 길이-접두 문자열을 ldm_get_vstr()로
	 * 복사). 온디스크 buffer + 0x18 + r_name.
	 * 설정자: ldm_parse_cmp3().
	 * 읽는 자: 현재 ldm.c 안에서 이 필드를 다시 읽는 코드는 없다(파싱해서 저장만 함, 로그/
	 *   파티션 생성 어디에도 사용되지 않음). */
	u8	state[16];
	/* [한국어] 이 컴포넌트가 속한 상위 Volume VBLK의 obj_id(가변폭 정수, ldm_get_vnum()으로
	 * 읽음). 온디스크 buffer + 0x2D + r_child.
	 * 설정자: ldm_parse_cmp3().
	 * 읽는 자: ldb->v_comp가 순회되지 않으므로 이 파일 내에서 실질적으로 소비되지 않는다
	 *   (Volume<->Component<->Partition 계층 재구성은 파싱만 되고 사용되지 않음). */
	u64	parent_id;
	/* [한국어] 컴포넌트 타입(COMP_STRIPE/COMP_BASIC/COMP_RAID). 온디스크 buffer +
	 * 0x18 + r_vstate 위치의 1바이트.
	 * 설정자: ldm_parse_cmp3().
	 * 값 범위: 0x01/0x02/0x03(정의된 상수 외 값도 그대로 저장은 됨, 별도 검증 없음). */
	u8	type;
	/* [한국어] 이 컴포넌트에 속한 자식(파티션) 개수. 온디스크 buffer + 0x1D + r_vstate
	 * 위치에서 ldm_get_vnum()(가변폭 정수)으로 읽되 u8 필드에 저장 - 8비트를 넘는 값이면
	 * 잘려서(truncate) 저장될 수 있음에 주의(원문 코드가 범위 검사를 하지 않음).
	 * 설정자: ldm_parse_cmp3(). */
	u8	children;
	/* [한국어] 스트라이프 청크 크기. COMP_STRIPE 타입이고 VBLK_FLAG_COMP_STRIPE 비트가 켜져
	 * 있을 때만 buffer + r_parent + 0x2E에서 ldm_get_vnum()으로 읽고, 아니면 0으로 고정.
	 * 설정자: ldm_parse_cmp3(). */
	u16	chunksize;
};

struct vblk_dgrp {			/* VBLK Disk Group */
/* [한국어]
 * struct vblk_dgrp - VBLK Disk Group(디스크 그룹) 레코드의 타입별 페이로드.
 * 필드가 문자열 하나뿐일 만큼 단순하며, 여러 디스크를 하나의 그룹(예: 하나의 RAID 세트를
 * 구성하는 디스크들)으로 묶는 식별자 역할만 한다. ldm_parse_dgr3()가 채우고, ldm_parse_dgr4()는
 * 이 구조체를 아예 쓰지 않고 지역 변수에만 파싱 결과를 버린다(v4는 그룹 문자열을 저장하지 않음). */
	/* [한국어] 디스크 그룹 식별 문자열(길이-접두 문자열, ldm_get_vstr()로 복사). 이름은
	 * "disk_id"지만 다른 구조체들의 uuid_t disk_id와 달리 여기서는 평문 문자열이다 - 이 파일
	 * 안에서 서로 다른 4가지 disk_id 의미 중 하나(문자열)임에 주의.
	 * 설정자: ldm_parse_dgr3()만 채움(v4는 사용 안 함).
	 * 읽는 자: ldb->v_dgrp 리스트가 이후 순회되지 않으므로 이 파일 내에서는 실질적으로 소비되지
	 *   않는다. */
	u8	disk_id[64];
};

struct vblk_disk {			/* VBLK Disk */
/* [한국어]
 * struct vblk_disk - VBLK Disk(물리 디스크) 레코드의 타입별 페이로드.
 * 이 물리 디스크 그 자체를 나타내는 레코드로, GUID를 통해 struct privhead.disk_id와 매칭되어
 * "이 gendisk가 곧 이 Disk VBLK다"라는 관계를 확립한다(ldm_get_disk_objid()). ldm_parse_dsk3()
 * (v3)와 ldm_parse_dsk4()(v4)가 각각 채우되 채우는 필드가 서로 다르다는 점에 주의. */
	/* [한국어] 물리 디스크의 128비트 GUID.
	 * 설정자: ldm_parse_dsk3()는 uuid_parse(buffer+0x19+r_name, ...)로, ldm_parse_dsk4()는
	 *   import_uuid(buffer+0x18+r_name, ...)로 채운다(오프셋과 파싱 API가 v3/v4 서로 다름).
	 * 읽는 자: ldm_get_disk_objid()가 ldb->ph.disk_id(현재 물리 디스크의 GUID)와 uuid_equal()로
	 *   비교해 일치하는 Disk VBLK를 찾는다 - 이것이 곧 "이 gendisk가 데이터베이스 안의 어느
	 *   디스크에 해당하는가"를 결정하는 유일한 매칭 지점이다. */
	uuid_t	disk_id;
	/* [한국어] 디스크의 대체 이름 문자열(길이-접두, ldm_get_vstr()로 복사).
	 * 설정자: ldm_parse_dsk3()만 채움(온디스크 buffer+0x18+r_diskid). ldm_parse_dsk4()는 이
	 *   필드를 전혀 건드리지 않는다.
	 * 읽는 자: 이 파일 내에서 다시 읽히지 않는다(로그/파티션 생성에 미사용). vb는 kmalloc_obj()로만
	 *   할당되고 kzalloc/memset이 없으므로, v4 Disk VBLK의 경우 이 필드는 초기화되지 않은 커널
	 *   메모리 그대로 남는다(추정: 실사용되지 않으니 실해는 없어 보이나, 필드 미초기화 자체는
	 *   코드 검토 관점에서 주목할 만하다). */
	u8	alt_name[128];
};

struct vblk_part {			/* VBLK Partition */
/* [한국어]
 * struct vblk_part - VBLK Partition(파티션) 레코드의 타입별 페이로드.
 * 이 파일 전체에서 실제로 리눅스 파티션 테이블에 반영되는 유일한 VBLK 타입이다.
 * ldm_parse_prt3()가 채우고, ldm_create_data_partitions()가 이 구조체의 start/size를 그대로
 * put_partition()에 넘긴다. */
	/* [한국어] 이 파티션의 시작 위치 -- "논리 디스크"(privhead.logical_disk_start) 시작을
	 * 0으로 하는 상대 섹터 오프셋. 온디스크 buffer + 0x24 + r_name(고정폭 빅엔디안 64비트,
	 * 다른 필드들과 달리 가변폭 인코딩이 아니다).
	 * 설정자: ldm_parse_prt3().
	 * 읽는 자: ldm_create_data_partitions()가 ldb->ph.logical_disk_start + part->start로
	 *   절대 LBA를 계산해 put_partition()에 전달 -- 리눅스 파티션 디바이스의 실제 시작 위치가
	 *   되는 값. */
	u64	start;
	/* [한국어] 이 파티션의 크기(섹터 수, 원문 주석: "start, size and vol_off in sectors").
	 * 온디스크 buffer + 0x34 + r_name에서 ldm_get_vnum()(가변폭 정수)으로 읽는다.
	 * 설정자: ldm_parse_prt3().
	 * 읽는 자: ldm_create_data_partitions()가 put_partition()의 size 인자로 그대로 전달. */
	u64	size;			/* start, size and vol_off in sectors */
	/* [한국어] 이 파티션이 속한 Volume의 논리 주소 공간 안에서의 오프셋(스팬/RAID 볼륨처럼
	 * 여러 파티션이 하나의 논리 볼륨을 구성할 때, Windows가 재조립 순서를 아는 데 필요).
	 * 온디스크 buffer + 0x2C + r_name(고정폭 빅엔디안 64비트).
	 * 설정자: ldm_parse_prt3().
	 * 읽는 자: 이 파일 내에서 다시 읽히지 않는다 - 리눅스는 Volume 재조립을 하지 않고 각
	 *   파티션을 물리 디스크 위 위치 그대로 노출하므로 이 필드는 파싱만 되고 사용되지 않는다. */
	u64	volume_offset;
	/* [한국어] 이 파티션을 소유하는 Component VBLK의 obj_id(가변폭 정수).
	 * 온디스크 buffer + 0x34 + r_size.
	 * 설정자: ldm_parse_prt3().
	 * 읽는 자: ldb->v_comp가 순회되지 않으므로 이 필드로 Component를 실제로 역참조하는 코드는
	 *   없다(파싱만 되고 미사용). */
	u64	parent_id;
	/* [한국어] 이 파티션이 속한 물리 디스크를 가리키는 참조 -- 주의: uuid_t GUID가 아니라
	 * struct vblk.obj_id(데이터베이스 내부 로컬 식별자)를 가리키는 u64 값이다(가변폭 정수,
	 * 온디스크 buffer + 0x34 + r_parent).
	 * 설정자: ldm_parse_prt3().
	 * 읽는 자: ldm_create_data_partitions()가 disk->obj_id(ldm_get_disk_objid()가 반환한 struct
	 *   vblk의 obj_id)와 이 필드를 비교(part->disk_id != disk->obj_id면 건너뜀)해, 데이터베이스
	 *   전체에 섞여 있는 여러 디스크의 파티션들 중 "현재 이 물리 디스크에 속한 파티션"만
	 *   필터링한다 - 즉 이 필드가 필터링의 핵심 키다. */
	u64	disk_id;
	/* [한국어] 파티션 인덱스(확장 필드). VBLK_FLAG_PART_INDEX 비트가 켜져 있을 때만
	 * buffer[0x35+r_diskid]에서 1바이트로 읽고, 꺼져 있으면 0으로 고정.
	 * 설정자: ldm_parse_prt3().
	 * 읽는 자: 이 파일 내에서 다시 읽히지 않는다(로그/파티션 생성 순번은 ldm_create_data_
	 *   partitions()의 지역 변수 part_num이 별도로 관리하며 이 필드를 참조하지 않는다). */
	u8	partnum;
};

struct vblk_volu {			/* VBLK Volume */
/* [한국어]
 * struct vblk_volu - VBLK Volume(논리 볼륨) 레코드의 타입별 페이로드.
 * 사용자가 Windows에서 보는 "드라이브"에 해당하는 최상위 논리 개념으로, 하나 이상의
 * Component(및 그 아래 Partition)를 아우른다. ldm_parse_vol5()가 채우고 ldb->v_volu에
 * 들어가지만, 이 리스트 역시 이후 순회되지 않아 실제 파티션 생성에는 관여하지 않는다. */
	/* [한국어] 볼륨 종류를 나타내는 문자열(길이-접두, 예: "gen"과 같은 값이 알려져 있으나
	 * 전체 열거값은 이 파일만으로는 확인 불가 - 추정). 온디스크 buffer+0x18+r_name.
	 * 설정자: ldm_parse_vol5(). */
	u8	volume_type[16];
	/* [한국어] 볼륨 상태 문자열(예: "ACTIVE"). 다른 문자열 필드들과 달리 ldm_get_vstr()이
	 * 아니라 memcpy()로 고정 16바이트를 그대로 복사한다(길이 접두 규칙을 따르지 않음) -
	 * 온디스크 buffer + 0x18 + r_disable_drive_letter.
	 * 설정자: ldm_parse_vol5(). */
	u8	volume_state[16];
	/* [한국어] 볼륨 GUID(16바이트). 다른 disk_id류 필드들과 달리 uuid_parse()/import_uuid()가
	 * 아니라 memcpy()로 원시 바이트만 복사한다 - uuid_t로 파싱되지 않은 raw 바이트 배열이라는
	 * 점에 주의. 온디스크 buffer + 0x42 + r_size.
	 * 설정자: ldm_parse_vol5(). */
	u8	guid[16];
	/* [한국어] 드라이브 문자 힌트(예: Windows에서 보여줄 드라이브 문자, 길이-접두 문자열).
	 * VBLK_FLAG_VOLU_DRIVE 비트가 켜져 있을 때만 buffer + 0x52 + r_size에서 채워지고,
	 * 꺼져 있으면 초기화되지 않은 채로 남는다(vb가 kmalloc_obj()로만 할당되어 zero-fill 없음).
	 * 설정자: ldm_parse_vol5() (조건부). */
	u8	drive_hint[4];
	/* [한국어] 볼륨 전체 크기(섹터, 가변폭 정수). 온디스크 buffer + 0x3D + r_child.
	 * 설정자: ldm_parse_vol5().
	 * 읽는 자: 이 파일 내에서 다시 읽히지 않는다 - 실제 파티션 크기는 vblk_part.size가 개별로
	 *   담당하므로 볼륨 합산 크기는 파싱만 되고 사용되지 않는다. */
	u64	size;
	/* [한국어] 파티션 타입 바이트(MBR sys_ind와 유사한 의미로 추정). 온디스크
	 * buffer[0x41 + r_size].
	 * 설정자: ldm_parse_vol5().
	 * 읽는 자: 이 파일 내에서 다시 읽히지 않는다. */
	u8	partition_type;
};

struct vblk_head {			/* VBLK standard header */
/* [한국어]
 * struct vblk_head - "VBLK standard header"라는 원문 주석이 붙어 있으나, 이 구조체 타입을
 * 실제로 선언/사용하는 코드가 ldm.c 어디에도 없다(grep 결과 정의 지점 외 참조 0건) - 죽은
 * (dead/vestigial) 구조체로 보인다. 실제로 조각 헤더 역할을 하는 것은 struct frag의 group/
 * num/rec 필드다. 필드 의미는 이름으로 유추한 내용을 아래에 적되, 실사용처가 없으므로
 * "현재는 쓰이지 않는다"는 점이 가장 중요한 사실이다. */
	/* [한국어] (추정) VBLK가 속한 그룹 ID. struct frag.group과 대응할 것으로 보이나 실사용 없음. */
	u32 group;
	/* [한국어] (추정) 조각 레코드 번호. struct frag.rec과 대응할 것으로 보이나 실사용 없음. */
	u16 rec;
	/* [한국어] (추정) 전체 조각 개수. struct frag.num과 대응할 것으로 보이나 실사용 없음. */
	u16 nrec;
};

struct vblk {				/* Generalised VBLK */
/* [한국어]
 * struct vblk - 파싱이 끝난 VBLK 레코드 하나의 범용(generalised) in-memory 표현.
 * ldm_parse_vblk()가 공통 헤더(name/obj_id/flags/type)를 채운 뒤 type에 따라
 * ldm_parse_{cmp3,dgr3,dgr4,dsk3,dsk4,prt3,vol5}() 중 하나를 호출해 union vblk 멤버를 채운다.
 * kmalloc_obj()로만 할당되고 별도 zero-fill이 없으므로, type이 요구하지 않는 필드/멤버는
 * 초기화되지 않은 채로 남을 수 있다는 점에 유의해야 한다(예: sequence 필드는 이 파일 어디에서도
 * 대입되지 않는 죽은 필드다 - grep 검증 완료). */
	/* [한국어] 이 VBLK의 이름 문자열(길이-접두, ldm_get_vstr()로 buf+0x18+r_objid에서 복사) -
	 * 디스크 이름, 볼륨 이름 등 type에 따라 의미가 달라진다.
	 * 설정자: ldm_parse_vblk() (모든 타입 공통).
	 * 읽는 자: 이 필드를 직접 로그/파티션 생성에 다시 읽는 코드는 확인되지 않는다(파싱만 됨). */
	u8	name[64];
	/* [한국어] 이 VBLK의 데이터베이스 내부 로컬 객체 ID(가변폭 정수, buf+0x18에서
	 * ldm_get_vnum()으로 읽음) - GUID가 아니라 VMDB 안에서만 의미 있는 참조용 정수다.
	 * 설정자: ldm_parse_vblk() (모든 타입 공통).
	 * 읽는 자: ldm_get_disk_objid()가 매칭에 성공한 Disk VBLK의 obj_id를 disk->obj_id로 반환;
	 *   ldm_create_data_partitions()가 vblk_part.disk_id(참조값)와 이 obj_id를 비교해 파티션
	 *   필터링에 사용 - 이 필드가 여러 VBLK 간 상호 참조의 핵심 키다. */
	u64	obj_id;
	/* [한국어] (죽은 필드) VBLK 시퀀스 번호로 추정되나, 이 파일 어디에서도 대입되는 코드가
	 * 없다(grep "->sequence"/".sequence" 결과 0건). vb가 kmalloc_obj()로만 할당되어
	 * zero-fill도 없으므로, 이 필드는 항상 초기화되지 않은 커널 메모리 값을 담은 채로 남는다
	 * (추정: 실사용되지 않으므로 기능적 위험은 없어 보이나 코드 위생 관점에서 주목할 지점). */
	u32	sequence;
	/* [한국어] 이 VBLK의 원문 플래그 바이트(buf[0x12]를 그대로 복사) - 어떤 선택적/가변
	 * 필드가 존재하는지를 나타내는 VBLK_FLAG_* 비트마스크. 비트의 실제 의미는 type에 따라
	 * 달라진다(예: 0x08은 PART_INDEX일 수도, DGR3_IDS일 수도 있음).
	 * 설정자: ldm_parse_vblk() (모든 타입 공통, vb->flags = buf[0x12]).
	 * 읽는 자: ldm_parse_prt3()가 partnum 결정에 vb->flags를 재확인(다른 파서들은 원본
	 *   buffer[0x12]를 직접 재확인, 값은 동일하지만 접근 경로가 함수마다 다르다). */
	u8	flags;
	/* [한국어] VBLK 타입 코드(VBLK_VOL5/CMP3/PRT3/DSK3/DSK4/DGR3/DGR4 중 하나, buf[0x13]).
	 * 설정자: ldm_parse_vblk().
	 * 읽는 자: 같은 함수의 switch문이 어느 ldm_parse_*() 헬퍼를 호출할지 결정; ldm_ldmdb_add()의
	 *   switch문이 어느 리스트(v_dgrp/v_disk/v_volu/v_comp/v_part)에 연결할지 결정하는 두 곳의
	 *   핵심 디스패치 키. */
	u8	type;
	/* [한국어] 아래 각주(1)~(5) 참고 - type에 따라 정확히 한 멤버만 유효한 공용체(union). */
	union {
		struct vblk_comp comp;
		struct vblk_dgrp dgrp;
		struct vblk_disk disk;
		struct vblk_part part;
		struct vblk_volu volu;
	/* [한국어] 위 union의 인스턴스 vblk - struct vblk 안에 실제로 필드가 아니라 "vblk"라는
	 * 이름의 익명 union 인스턴스로 내장된다(즉 접근 시 outer->vblk.comp처럼 씀).
	 * 설정자: ldm_parse_{cmp3,dgr3,dgr4,dsk3,dsk4,prt3,vol5}() 중 outer->type에 대응하는 하나만
	 *   해당 멤버를 채운다.
	 * 읽는 자: 접근하는 쪽은 반드시 먼저 outer->type을 확인한 뒤 대응 멤버만 읽어야 하며,
	 *   그렇지 않으면 다른 타입의 바이트를 잘못된 구조체로 재해석하게 된다(C union의 일반적
	 *   위험). */
	} vblk;
	/* [한국어] 이 VBLK 객체를 ldmdb의 다섯 리스트(v_dgrp/v_disk/v_volu/v_comp/v_part) 중
	 * 정확히 하나에 연결하는 노드, 또는 재조립 이전(struct frag.data 안)에는 아직 리스트에
	 * 연결되지 않은 상태.
	 * 설정자: ldm_ldmdb_add()가 type에 따라 list_add() 또는(파티션의 경우) 시작 섹터 순
	 *   정렬 삽입(list_add_tail())으로 연결.
	 * 읽는 자: ldm_get_disk_objid()/ldm_create_data_partitions()가 list_for_each()로 순회;
	 *   ldm_free_vblks()가 list_for_each_safe()로 순회하며 각 vblk를 kfree(). */
	struct list_head list;
};

struct ldmdb {				/* Cache of the database */
/* [한국어]
 * struct ldmdb - "Cache of the database"라는 원문 주석대로, PRIVHEAD/TOCBLOCK/VMDB 각 1부와
 * 전체 VBLK 레코드를 타입별로 분류한 다섯 리스트를 모두 모은 최상위 캐시 구조체.
 * ldm_partition()이 kmalloc_obj()로 스택이 아닌 힙에 하나 할당해 파싱 전 과정 동안 들고 있다가,
 * ldm_create_data_partitions()까지 끝나면 리스트들을 ldm_free_vblks()로, 자기 자신은
 * kfree(ldb)로 해제한다. */
	/* [한국어] 이 물리 디스크의 검증된 PRIVHEAD(주 사본, 세 사본을 비교해 대표로 채택된 값).
	 * 설정자: ldm_validate_privheads()가 &ldb->ph를 ph1으로 받아 직접 채움.
	 * 읽는 자: ldm_partition()이 base(=ph.config_start) 계산; ldm_get_disk_objid()가 ph.disk_id로
	 *   현재 디스크 매칭; ldm_create_data_partitions()가 ph.logical_disk_start로 절대 LBA 계산. */
	struct privhead ph;
	/* [한국어] 검증된 TOCBLOCK(여러 사본 중 대표로 채택된 값).
	 * 설정자: ldm_validate_tocblocks()가 &ldb->toc를 tb[0]으로 받아 채움.
	 * 읽는 자: ldm_validate_vmdb()가 toc.bitmap1_size로 VBLK 테이블 크기 범위 검사. */
	struct tocblock toc;
	/* [한국어] 검증된 VMDB 헤더.
	 * 설정자: ldm_validate_vmdb()가 &ldb->vm을 vm으로 받아 채움.
	 * 읽는 자: ldm_get_vblks()가 vm.vblk_size/vblk_offset/last_vblk_seq로 VBLK 테이블 스캔 범위
	 *   계산; ldm_frag_commit()이 vm.vblk_size로 재조립 길이 계산. */
	struct vmdb     vm;
	/* [한국어] Disk Group VBLK 리스트. 설정자: ldm_ldmdb_add()(VBLK_DGR3/DGR4). 초기화:
	 * ldm_partition()의 INIT_LIST_HEAD(&ldb->v_dgrp). 읽는 자: ldm_free_vblks()만 순회하며
	 * 해제한다 - 파티션 생성 로직에서는 순회되지 않는다(파싱만 되고 미사용). */
	struct list_head v_dgrp;
	/* [한국어] Disk VBLK 리스트. 설정자: ldm_ldmdb_add()(VBLK_DSK3/DSK4). 읽는 자:
	 * ldm_get_disk_objid()가 list_for_each()로 순회하며 disk_id(GUID)가 ph.disk_id와 일치하는
	 * 항목을 찾는다 - 다섯 리스트 중 v_part와 함께 실제로 순회/소비되는 두 리스트 중 하나. */
	struct list_head v_disk;
	/* [한국어] Volume VBLK 리스트. 설정자: ldm_ldmdb_add()(VBLK_VOL5). 읽는 자: v_dgrp와
	 * 마찬가지로 ldm_free_vblks() 외에는 순회되지 않는다(파싱만 되고 미사용). */
	struct list_head v_volu;
	/* [한국어] Component VBLK 리스트. 설정자: ldm_ldmdb_add()(VBLK_CMP3). 읽는 자: 역시
	 * ldm_free_vblks() 외에는 순회되지 않는다(파싱만 되고 미사용). */
	struct list_head v_comp;
	/* [한국어] Partition VBLK 리스트, 같은 disk_id를 가진 항목들끼리 시작 섹터(start) 오름차순
	 * 으로 정렬 삽입된다(ldm_ldmdb_add()의 VBLK_PRT3 케이스). 읽는 자: ldm_create_data_
	 * partitions()가 list_for_each()로 순회하며 disk_id가 일치하는 항목만 골라 put_partition()
	 * 호출 - v_disk와 함께 실제로 리눅스 파티션 결과에 반영되는 유일한 리스트. */
	struct list_head v_part;
};

/* [한국어] include guard 종료. */
#endif /* _FS_PT_LDM_H_ */

