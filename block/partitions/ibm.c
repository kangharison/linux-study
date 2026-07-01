// SPDX-License-Identifier: GPL-2.0
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 *                  Volker Sameske <sameske@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2012
 */

/*
 * 파일 상단 요약:
 * block/partitions/ibm.c는 Linux block layer의 파티션 테이블 파서로,
 * IBM s390 DASD 디스크의 VOL1/LNX1/CMS1 레이블을 해석하여 파티션을 등록한다.
 * NVMe SSD 입장에서 볼 때, 이 파일은 submit_bio -> blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로 이어지는
 * 실제 I/O 경로보다 상위, 즉 블록 장치 초기화 시 파티션 정보를 결정하는
 * preprocessing 단계에 해당한다. 파티션 경계를 정확히 파악해야 NVMe 호스트가
 * 생성하는 Read/Write CID, PRP/SGL, 그리고 SQ/CQ 엔트리들이 올바른
 * 논리 섹터 범위를 대상으로 할 수 있다.
 */

#include <linux/buffer_head.h>
#include <linux/hdreg.h>
#include <linux/slab.h>
#include <asm/dasd.h>
#include <asm/ebcdic.h>
#include <linux/uaccess.h>
#include <asm/vtoc.h>
#include <linux/module.h>
#include <linux/dasd_mod.h>

#include "check.h"

/*
 * union label_t: DASD 볼륨 레이블의 세 가지 변종(VOL1, LNX1, CMS1)을 동일한
 * 메모리로 오버레이한다. NVMe SSD가 인식하는 것은 최종적으로 변환된 LBA 범위이며,
 * 이 union은 DASD 고유의 기하학적/EBCDIC 레이블을 범용 block layer가 다루는
 * sector_t 단위로 해석하기 위한 중간 버퍼 역할을 한다.
 */
union label_t {
	struct vtoc_volume_label_cdl vol;	/* on-disk VOL1 메타데이터가 NVMe media -> read_part_sector -> 이 필드 오프셋에 복사됨 (추정) */
	struct vtoc_volume_label_ldl lnx;	/* LNX1 변종: LDL 형식의 크기/버전 필드가 NVMe namespace LBA 0 근처에 위치 (추정) */
	struct vtoc_cms_label cms;		/* CMS1 변종: block_size, disk_offset, block_count 필드가 NVMe Read로 읽힌 버퍼 내 고정 오프셋에 배치 (추정) */
};

/*
 * cchh2blk:
 * 목적: DASD의 cylinder/head 값을 범용 블록 번호로 변환한다.
 * 호출 경로: ibm_partition -> find_vol1_partitions -> cchh2blk
 * NVMe 연결: 변환된 blk 값은 secperblk를 곱해 최종 LBA가 되며, 이 LBA가
 *            NVMe 명령의 SLBA(Starting LBA) 필드로 매핑된다(추정).
 *            이 함수 자체는 NVMe 드라이버의 doorbell/CID/SQ/CQ와는 직접
 *            상호작용하지 않는다.
 */
static sector_t cchh2blk(struct vtoc_cchh *ptr, struct hd_geometry *geo)
{
	sector_t cyl;
	__u16 head;

	/* decode cylinder and heads for large volumes */
	cyl = ptr->hh & 0xFFF0;		/* 상위 12비트 cylinder 정보를 분리: DASD CHS -> NVMe LBA 계산의 전 단계 (추정) */
	cyl <<= 12;			/* DASD 대용량 볼륨의 cylinder 비트 확장: 이 값은 곧 NVMe namespace 상에서의 블록 번호 기반 (추정) */
	cyl |= ptr->cc;			/* 하위 cylinder 비트 병합 -> 최종 실린더 번호, NVMe Read의 SLBA 산출에 간접 사용 (추정) */
	head = ptr->hh & 0x000F;	/* 하위 4비트 head 추출: DASD 기하학적 주소 -> block layer의 평면적 block 번호로 변환 (추정) */
	return cyl * geo->heads * geo->sectors +	/* cylinder 기반 블록 수: DASD 기하학 -> NVMe가 이해할 수 있는 연속 sector offset으로 변환 예정 (추정) */
	       head * geo->sectors;			/* head 기반 블록 수: 위 결과와 합쳐져 NVMe namespace 내 시작 LBA 후보가 됨 (추정) */
}

/*
 * cchhb2blk:
 * 목적: cylinder/head/block 값을 블록 번호로 변환한다.
 * 호출 경로: ibm_partition -> find_label/find_vol1_partitions -> cchhb2blk
 * NVMe 연결: 이 함수가 산출한 블록 번호는 이후 secperblk를 곱해 실제 sector
 *            단위 offset이 된다. NVMe Read/Write 명령은 이 offset을 바탕으로
 *            PRP/SGL 리스트를 구성하여 플래시 페이지에 접근한다(추정).
 */
static sector_t cchhb2blk(struct vtoc_cchhb *ptr, struct hd_geometry *geo)
{
	sector_t cyl;
	__u16 head;

	/* decode cylinder and heads for large volumes */
	cyl = ptr->hh & 0xFFF0;		/* cchh2blk과 동일한 cylinder 디코딩: DASD 고유 주소를 NVMe LBA로 변환하기 위한 중간값 (추정) */
	cyl <<= 12;			/* 대용량 cylinder 비트 확장: NVMe namespace는 이 값을 평면적 LBA로 최종 해석 (추정) */
	cyl |= ptr->cc;			/* 실린더 번호 완성: VTOC 위치 파악 -> NVMe Read 명령의 SLBA 계산 시작점 (추정) */
	head = ptr->hh & 0x000F;	/* head 분리: DASD CHS 기하학 -> block 번호 기반 offset으로 매핑 (추정) */
	return	cyl * geo->heads * geo->sectors +	/* cylinder*heads*sectors: DASD 3차원 주소를 1차원 block 번호로 펼침, NVMe LBA 산출의 기반 (추정) */
		head * geo->sectors +			/* head*sectors: head 평면 내 offset 추가 (추정) */
		ptr->b;					/* block 번호 추가: DASD record 번호 -> NVMe LBA 계산 전 마지막 DASD 단위 (추정) */
}

/* Volume Label Type/ID Length */
#define DASD_VOL_TYPE_LEN	4	/* 레이블 타입 문자열("VOL1" 등) 길이: NVMe media에서 읽은 첫 4바이트가 이 길이만큼 매직/시그니처로 사용됨 (추정) */
#define DASD_VOL_ID_LEN		6	/* 볼륨 ID 길이: 파티션 등록 후 block device 이름에 반영, NVMe CID 계산과는 무관 */

/* Volume Label Types */
#define DASD_VOLLBL_TYPE_VOL1 0
#define DASD_VOLLBL_TYPE_LNX1 1
#define DASD_VOLLBL_TYPE_CMS1 2

/*
 * struct dasd_vollabel: EBCDIC 문자열 형태의 레이블 식별자("VOL1" 등)를
 * 커널 낶에서 사용하는 정수 인덱스로 매핑한다. NVMe SSD의 namespace는 이러한
 * 문자열 레이블을 해석하지 않고, 파티션 파서가 등록한 최종 LBA 범위만을
 * 기준으로 명령을 처리한다.
 */
struct dasd_vollabel {
	char *type;	/* 매직 문자열 포인터: read_part_sector로 읽은 NVMe media 상 첫 4바이트와 비교 대상 (추정) */
	int idx;	/* 레이블 종류 인덱스: switch 분기 -> 파티션 경계 산출 방식 선택 -> NVMe SLBA/Length 계산 방식 결정 (추정) */
};

static struct dasd_vollabel dasd_vollabels[] = {
	[DASD_VOLLBL_TYPE_VOL1] = {
		.type = "VOL1",		/* NVMe media offset 0에 있는 4바이트 EBCDIC/ASCII 매직과 비교 (추정) */
		.idx = DASD_VOLLBL_TYPE_VOL1,	/* VOL1 선택 시 VTOC 체인 탐색 -> 다중 put_partition -> 여러 NVMe 하위 bdev 생성 (추정) */
	},
	[DASD_VOLLBL_TYPE_LNX1] = {
		.type = "LNX1",		/* LDL 형식 매직: 단일 파티션 등록 -> NVMe namespace 하위 단일 bdev (추정) */
		.idx = DASD_VOLLBL_TYPE_LNX1,
	},
	[DASD_VOLLBL_TYPE_CMS1] = {
		.type = "CMS1",		/* CMS 형식 매직: minidisk/일반 경우 분기 -> NVMe SLBA offset 결정 (추정) */
		.idx = DASD_VOLLBL_TYPE_CMS1,
	},
};

/*
 * get_label_by_type:
 * 목적: 4바이트 EBCDIC/ASCII 레이블 문자열을 dasd_vollabels 테이블에서
 *       검색해 정수 인덱스로 반환한다.
 * 호출 경로: find_label -> get_label_by_type; ibm_partition -> get_label_by_type
 * NVMe 연결: 레이블 식별 단계는 NVMe 컨트롤러의 Identify namespace/CNS와는
 *            독립적이다. 다만, 레이블 종류에 따라 파티션의 시작/끝 LBA가
 *            달라지므로 NVMe SQ에 기록되는 SLBA/Length에 영향을 준다.
 */
static int get_label_by_type(const char *type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dasd_vollabels); i++) {	/* 레이블 매직 후보 순회: 매칭 실패 시 -1 반환 -> NVMe 파티션 등록 경로 미진입 (추정) */
		if (!memcmp(type, dasd_vollabels[i].type, DASD_VOL_TYPE_LEN))	/* NVMe Read로 가져온 버퍼의 첫 4바이트(매직/시그니처)와 비교 (추정) */
			return dasd_vollabels[i].idx;	/* 매직 일치 -> 파서가 NVMe namespace LBA 배치를 어떻게 해석할지 결정 (추정) */
	}

	return -1;	/* 매직 불일치: CQE 개념은 아니나, NVMe 입장에서는 잘못된 포맷 namespace로 판단될 수 있음 (추정) */
}

/*
 * find_label:
 * 목적: 디스크 상에서 VOL1/LNX1/CMS1 레이블을 탐색하고, 레이블 섹터 번호와
 *       볼륨 이름을 반환한다.
 * 호출 경로: ibm_partition -> find_label
 * NVMe 연결: 이 함수가 호출하는 read_part_sector는 block layer의 일반
 *            bio/submit_bio 경로를 타며, NVMe namespace 위에서도
 *            submit_bio -> blk_mq_submit_bio -> ... -> nvme_queue_rq ->
 *            nvme_submit_cmd(doorbell)로 전달될 수 있다(추정).
 *            레이블 읽기는 부팅/장치 초기화 시 한 번 수행되며, 이후
 *            정상 I/O 경로는 NVMe queue pair(SQ/CQ)를 통해 이루어진다.
 */
static int find_label(struct parsed_partitions *state,
		      dasd_information2_t *info,
		      struct hd_geometry *geo,
		      int blocksize,
		      sector_t *labelsect,
		      char name[],
		      char type[],
		      union label_t *label)
{
	sector_t testsect[3];	/* 후보 섹터 번호 배열: 각 원소가 read_part_sector -> submit_bio -> NVMe Read CID 하나로 전환 가능 (추정) */
	int i, testcount;
	Sector sect;		/* read_part_sector가 반환하는 임시 버퍼 핸들: NVMe media 데이터가 DMA 완료 후 CPU가 접근하는 페이지 참조 (추정) */
	void *data;

	/* There a three places where we may find a valid label:
	 * - on an ECKD disk it's block 2
	 * - on an FBA disk it's block 1
	 * - on an CMS formatted FBA disk it is sector 1, even if the block size
	 *   is larger than 512 bytes (possible if the DIAG discipline is used)
	 * If we have a valid info structure, then we know exactly which case we
	 * have, otherwise we just search through all possebilities.
	 */
	if (info) {		/* DASD discipline이 제공하는 정보 있음: 탐색 횟수 1회 -> NVMe Read 명령 1회 감소 (추정) */
		if ((info->cu_type == 0x6310 && info->dev_type == 0x9336) ||	/* FBA/ diagnose 특수 장치 식별: NVMe와 직접 무관 (추정) */
		    (info->cu_type == 0x3880 && info->dev_type == 0x3370))	/* 3370 유형: 레이블 위치가 고정됨 (추정) */
			testsect[0] = info->label_block;	/* 블록 단위 그대로 -> read_part_sector의 인자는 sector 단위이므로 아래에서 보정 (추정) */
		else
			testsect[0] = info->label_block * (blocksize >> 9);	/* DASD 블록 -> 512바이트 sector 변환: NVMe Read의 SLBA 후보 (추정) */
		testcount = 1;	/* 단일 후보 탐색: NVMe command submission volume 1회 (추정) */
	} else {
		/* info가 없으면 FBA/ECKD/CMS 세 경우를 순회 탐색 (추정) */
		testsect[0] = 1;		/* FBA/CMSDIAG 섹터 1 후보: NVMe namespace LBA 1에 Read 명령 (추정) */
		testsect[1] = (blocksize >> 9);	/* ECKD 블록 2에 해당하는 sector: blocksize/512 (추정) */
		testsect[2] = 2 * (blocksize >> 9);	/* ECKD 블록 3에 해당하는 sector (추정) */
		testcount = 3;	/* 최대 3회 read_part_sector -> 최대 3회 NVMe Read command submission (추정) */
	}
	for (i = 0; i < testcount; ++i) {
		/* read_part_sector -> submit_bio -> blk_mq_submit_bio ->
		 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
		 * 로 이어질 수 있는 NVMe 경로의 최상위 진입점 (추정)
		 */
		data = read_part_sector(state, testsect[i], &sect);	/* NVMe namespace에서 testsect[i] 번호의 LBA를 읽어 레이블 후보 획득 (추정) */
		if (data == NULL)
			continue;	/* read_part_sector 실패/미디어 에러 시 다음 후보로 skip: NVMe CQE status SCT/SKIP 아님, block layer 재시도 후 실패 (추정) */
		memcpy(label, data, sizeof(*label));		/* NVMe media -> DMA 버퍼에서 label_t union으로 복사: on-disk 메타데이터의 고정 오프셋/크기로 해석 (추정) */
		memcpy(type, data, DASD_VOL_TYPE_LEN);	/* NVMe Read 버퍼 offset 0의 매직 4바이트 추출 (추정) */
		EBCASC(type, DASD_VOL_TYPE_LEN); /* EBCDIC -> ASCII 변환 */
		put_dev_sector(sect);	/* 임시 sector 버퍼 해제: NVMe Read 완료(CQE) 후 커널 버퍼 정리 (추정) */
		switch (get_label_by_type(type)) {	/* 매직/시그니처 검증: 일치하지 않으면 NVMe 파티션 등록 안 함 (추정) */
		case DASD_VOLLBL_TYPE_VOL1:
			memcpy(name, label->vol.volid, DASD_VOL_ID_LEN);	/* VOL1 구조체의 volid 필드 offset에서 볼륨명 추출: NVMe media 메타데이터 offset (추정) */
			EBCASC(name, DASD_VOL_ID_LEN);
			*labelsect = testsect[i];	/* 레이블이 위치한 sector 번호 저장 -> 이후 VTOC/파티션 offset 계산의 기준 (추정) */
			return 1;			/* 매직 검증 성공: NVMe namespace 하위 파티션 파싱 계속 진행 (추정) */
		case DASD_VOLLBL_TYPE_LNX1:
		case DASD_VOLLBL_TYPE_CMS1:
			memcpy(name, label->lnx.volid, DASD_VOL_ID_LEN);	/* LNX1/CMS1 구조체의 volid 필드 offset에서 볼륨명 추출 (추정) */
			EBCASC(name, DASD_VOL_ID_LEN);
			*labelsect = testsect[i];	/* NVMe LBA 기준 레이블 섹터 확정 (추정) */
			return 1;			/* 매직 검증 성공: 단일 파티션 파싱 진행 (추정) */
		default:
			break;	/* 시그니처 불일치: 다음 testsect 후보로, NVMe Read 추가 시도 (추정) */
		}
	}

	return 0;	/* 모든 후보 실패: NVMe namespace에 IBM DASD 파티션 없음으로 보고 (추정) */
}

/*
 * find_vol1_partitions:
 * 목적: VOL1 레이블이 가리키는 VTOC를 따라 format1/format8 레이블을 해석해
 *       다중 파티션을 등록한다.
 * 호출 경로: ibm_partition -> find_vol1_partitions
 * NVMe 연결: put_partition()으로 등록된 각 파티션의 (start, size)는 NVMe
 *            namespace 낶의 하위 블록 장치로 노출된다. 실제 NVMe Read/Write
 *            CID를 할당할 때, 이 start를 더해 NVMe 명령의 SLBA를 계산한다.
 */
static int find_vol1_partitions(struct parsed_partitions *state,
				struct hd_geometry *geo,
				int blocksize,
				char name[],
				union label_t *label)
{
	sector_t blk;		/* VTOC 내 현재 블록 번호: DASD 단위 -> secperblk 곱해서 NVMe LBA로 변환 예정 (추정) */
	int counter;		/* 등록한 파티션 개수: state->limit 초과 시 NVMe 하위 bdev 생성 중단 (추정) */
	Sector sect;
	unsigned char *data;	/* NVMe media에서 읽은 VTOC 레이블 바이트들: struct vtoc_format1_label offset 기준 해석 (추정) */
	loff_t offset, size;
	struct vtoc_format1_label f1;	/* VTOC format1/format8 레이드: DS1FMTID 등 필드가 NVMe media 상 고정 offset에 위치 (추정) */
	int secperblk;

	seq_buf_printf(&state->pp_buf, "VOL1/%8s:", name);	/* 파티션 정보 문자열 기록: NVMe 입장에서 debugging message 용도 (추정) */
	/*
	 * get start of VTOC from the disk label and then search for format1
	 * and format8 labels
	 */
	secperblk = blocksize >> 9; /* 512바이트 섹터 수: NVMe는 일반적으로 512 or 4096바이트 LBA 단위를 사용 (추정) */
	blk = cchhb2blk(&label->vol.vtoc, geo) + 1;	/* VOL1 레이블의 vtoc 필드 offset -> DASD CHS -> block 번호 +1: NVMe VTOC 시작 LBA 후보 (추정) */
	counter = 0;
	data = read_part_sector(state, blk * secperblk, &sect);	/* VTOC 첫 sector 읽기 -> submit_bio -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_submit_cmd (추정) */
	while (data != NULL) {	/* VTOC 레이블 체인 순회: 각 루프 반복마다 read_part_sector -> 추가 NVMe Read 명령 (추정) */
		memcpy(&f1, data, sizeof(struct vtoc_format1_label));	/* NVMe Read 버퍼의 VTOC 엔트리를 로컬 구조체로 복사: DS1FMTID 등 필드 offset 기준 (추정) */
		put_dev_sector(sect);	/* 현재 VTOC sector 버퍼 해제: 이전 NVMe Read의 CQE 처리 완료 후 정리 (추정) */
		/* skip FMT4 / FMT5 / FMT7 labels */
		if (f1.DS1FMTID == _ascebc['4']		/* VTOC 엔트리 타입 매직 '4': partition entry가 아니므로 NVMe 파티션 등록 skip (추정) */
		    || f1.DS1FMTID == _ascebc['5']	/* 매직 '5': NVMe SLBA/Length 계산 대상 아님 (추정) */
		    || f1.DS1FMTID == _ascebc['7']	/* 매직 '7': NVMe 파티션 등록 skip (추정) */
		    || f1.DS1FMTID == _ascebc['9']) {	/* 매직 '9': NVMe 파티션 등록 skip (추정) */
			blk++;	/* 다음 VTOC 블록: NVMe Read의 SLBA 1블록 증가 (추정) */
			data = read_part_sector(state, blk * secperblk, &sect);	/* 다음 VTOC 엔트리 읽기 -> 추가 NVMe Read CID (추정) */
			continue;
		}
		/* only FMT1 and 8 labels valid at this point */
		if (f1.DS1FMTID != _ascebc['1'] &&	/* 매직 '1' 아니면 partition entry가 아님 (추정) */
		    f1.DS1FMTID != _ascebc['8'])		/* 매직 '8' 아니면 VTOC 체인 끝으로 간주, NVMe 파티션 등록 종료 (추정) */
			break;
		/* OK, we got valid partition data */
		offset = cchh2blk(&f1.DS1EXT1.llimit, geo);	/* 파티션 하한 CHS -> block 번호: NVMe namespace 내 시작 LBA의 전 단계 (추정) */
		size  = cchh2blk(&f1.DS1EXT1.ulimit, geo) -	/* 파티션 상한 CHS -> block 번호 (추정) */
			offset + geo->sectors;	/* 상한-하한+sectors: DASD 파티션 크기(block 단위), NVMe Length 산출 전 단계 (추정) */
		offset *= secperblk; /* DASD 논리 블록 -> NVMe LBA로 해석 가능한 sector 단위 (추정) */
		size *= secperblk;	/* DASD block -> NVMe sector(LBA) 개수: NVMe Read/Write 명령의 Length(dword12) 후보 (추정) */
		if (counter >= state->limit)	/* 최대 파티션 개수 초과 시 중단: NVMe 하위 bdev 생성 한계 (추정) */
			break;
		put_partition(state, counter + 1, offset, size);	/* block device 등록 -> NVMe namespace 하위 bdev 생성 -> 이후 bio가 이 offset을 더해 SLBA 계산 (추정) */
		counter++;	/* NVMe 하위 bdev 하나 추가 완료 (추정) */
		blk++;		/* 다음 VTOC 엔트리 (추정) */
		data = read_part_sector(state, blk * secperblk, &sect);	/* VTOC 체인 다음 엔트리 -> 추가 NVMe Read command submission (추정) */
	}
	seq_buf_puts(&state->pp_buf, "\n");

	if (!data)	/* 마지막 read_part_sector가 NULL 반환: NVMe media 읽기 실패로 파티션 스캔 중단/에러 보고 (추정) */
		return -1;

	return 1;	/* VOL1 파티션 등록 성공: NVMe namespace 하위 다중 bdev 생성 완료 (추정) */
}

/*
 * find_lnx1_partitions:
 * 목적: LNX1 레이블이 서술하는 단일 파티션을 등록한다.
 * 호출 경로: ibm_partition -> find_lnx1_partitions
 * NVMe 연결: LDL 형식의 큰 볼륨 지원 여부에 따라 size가 달라진다. 이 size가
 *            NVMe namespace의 마지막 LBA를 넘지 않도록 제한하는 것은 상위
 *            block layer/gendisk의 책임이며, NVMe 컨트롤러는 유효한 SLBA +
 *            Length 조합만을 처리한다.
 */
static int find_lnx1_partitions(struct parsed_partitions *state,
				struct hd_geometry *geo,
				int blocksize,
				char name[],
				union label_t *label,
				sector_t labelsect,
				sector_t nr_sectors,
				dasd_information2_t *info)
{
	loff_t offset, geo_size, size;
	int secperblk;

	seq_buf_printf(&state->pp_buf, "LNX1/%8s:", name);	/* LNX1 단일 파티션 메시지: NVMe와 직접 무관 (추정) */
	secperblk = blocksize >> 9;	/* 512바이트 sector 개수: NVMe LBA 변환 계수 (추정) */
	if (label->lnx.ldl_version == 0xf2) {	/* LDL 대용량 버전: formatted_blocks 필드가 NVMe LBA 길이로 직접 변환 (추정) */
		size = label->lnx.formatted_blocks * secperblk;	/* NVMe namespace 내 파티션 길이( sector 수 ), Length dword12 후보 (추정) */
	} else {
		/*
		 * Formated w/o large volume support. If the sanity check
		 * 'size based on geo == size based on nr_sectors' is true, then
		 * we can safely assume that we know the formatted size of
		 * the disk, otherwise we need additional information
		 * that we can only get from a real DASD device.
		 */
		geo_size = geo->cylinders * geo->heads	/* DASD CHS 기반 총 sector 수 산출: NVMe namespace 용량과 교차 검증 (추정) */
			* geo->sectors * secperblk;
		size = nr_sectors;	/* block layer가 알고 있는 총 sector 수: NVMe namespace의 마지막 LBA 경계 (추정) */
		if (size != geo_size) {	/* geometry 기반 크기와 실제 sector 수 불일치: NVMe namespace 기하학/포맷 불일치 가능성 (추정) */
			if (!info) {
				seq_buf_puts(&state->pp_buf, "\n");
				return 1;	/* 추가 DASD 정보 없으면 단일 파티션 등록 포기, NVMe bdev 생성 안 함 (추정) */
			}
			if (!strcmp(info->type, "ECKD"))	/* ECKD인 경우 geometry 크기 제한 적용: NVMe LBA 범위 축소 가능 (추정) */
				if (geo_size < size)
					size = geo_size;	/* NVMe namespace length를 geometry 기반으로 제한: 잘못된 LBA 접근 방지 (추정) */
			/* else keep size based on nr_sectors */
		}
	}
	/* first and only partition starts in the first block after the label */
	offset = labelsect + secperblk;	/* 레이블 다음 block부터 파티션 시작: NVMe namespace 내 시작 LBA(SLBA) (추정) */
	put_partition(state, 1, offset, size - offset);	/* 단일 파티션 등록 -> NVMe namespace 하위 bdev 1개 생성 (추정) */
	seq_buf_puts(&state->pp_buf, "\n");
	return 1;	/* LNX1 파티션 등록 성공: NVMe bdev 1개 생성 완료 (추정) */
}

/*
 * find_cms1_partitions:
 * 목적: VM/CMS 형식의 CMS1 레이블을 해석해 단일 파티션을 등록한다.
 * 호출 경로: ibm_partition -> find_cms1_partitions
 * NVMe 연결: CMS minidisk 여부와 FBA DIAG 디스크플린 특수 케이스에 따라
 *            offset이 달라진다. 파티션 offset은 NVMe 명령의 SLBA 계산에
 *            직접 반영되므로, 레이블 파싱 오류는 NVMe의 잘못된 LBA 접근으로
 *            이어질 수 있다(추정).
 */
static int find_cms1_partitions(struct parsed_partitions *state,
				struct hd_geometry *geo,
				int blocksize,
				char name[],
				union label_t *label,
				sector_t labelsect)
{
	loff_t offset, size;
	int secperblk;

	/*
	 * VM style CMS1 labeled disk
	 */
	blocksize = label->cms.block_size;	/* NVMe media에서 읽은 CMS1 구조체의 block_size 필드: sector 변환 계수 재설정 (추정) */
	secperblk = blocksize >> 9;		/* block_size/512: NVMe LBA 계산에 사용 (추정) */
	if (label->cms.disk_offset != 0) {	/* CMS reserved minidisk: offset이 0이 아니면 NVMe SLBA에 추가 offset 반영 (추정) */
		seq_buf_printf(&state->pp_buf, "CMS1/%8s(MDSK):", name);
		/* disk is reserved minidisk */
		offset = label->cms.disk_offset * secperblk;	/* minidisk 시작 offset을 NVMe LBA 단위로 변환 (추정) */
		size = (label->cms.block_count - 1) * secperblk;	/* minidisk 크기를 NVMe sector 수로 변환, Length 후보 (추정) */
	} else {
		seq_buf_printf(&state->pp_buf, "CMS1/%8s:", name);
		/*
		 * Special case for FBA devices:
		 * If an FBA device is CMS formatted with blocksize > 512 byte
		 * and the DIAG discipline is used, then the CMS label is found
		 * in sector 1 instead of block 1. However, the partition is
		 * still supposed to start in block 2.
		 */
		if (labelsect == 1)	/* DIAG discipline 특수 케이스: 레이블이 LBA 1에 있음, 파티션은 block 2부터 (추정) */
			offset = 2 * secperblk;	/* block 2 시작 -> NVMe SLBA = 2*secperblk (추정) */
		else
			offset = labelsect + secperblk;	/* 일반: 레이블 sector 다음 block 시작 -> NVMe SLBA (추정) */
		size = label->cms.block_count * secperblk;	/* CMS block_count -> NVMe namespace sector 길이 (추정) */
	}

	put_partition(state, 1, offset, size-offset);	/* CMS 파티션 등록 -> NVMe namespace 하위 bdev 생성 (추정) */
	seq_buf_puts(&state->pp_buf, "\n");
	return 1;	/* CMS1 파티션 등록 성공: NVMe bdev 1개 생성 (추정) */
}


/*
 * ibm_partition:
 * 목적: block/partitions/check.c로부터 호출되어, IBM DASD 디스크의
 *       레이블을 읽고 파티션을 등록하는 메인 파서이다.
 * 호출 경로: check.c의 파티션 탐색 루프 -> ibm_partition
 * NVMe 연결: 이 함수는 NVMe SSD 초기화 시 blk_mq가 아닌 상위 block device
 *            초기화 경로에서 실행된다. 파서가 put_partition()으로 등록한
 *            파티션은 이후 NVMe namespace 하위의 독립적 bdev로 노출되며,
 *            파일시스템/응용계층의 bio가 해당 파티션을 거쳐
 *            submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *            nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로 전달될 때
 *            올바른 LBA 범위를 보장한다.
 */
int ibm_partition(struct parsed_partitions *state)
{
	int (*fn)(struct gendisk *disk, dasd_information2_t *info);	/* s390 DASD discipline 콜백 포인터: NVMe 경로와 무관 (추정) */
	struct gendisk *disk = state->disk;	/* 파싱 대상 gendisk: NVMe namespace를 나타내는 block device 상위 객체 (추정) */
	struct block_device *bdev = disk->part0;	/* NVMe namespace 전체를 나타내는 block_device (추정) */
	int blocksize, res;
	loff_t offset, size;
	sector_t nr_sectors;
	dasd_information2_t *info;	/* s390 DASD 전용 정보: NVMe와 직접 무관, 없으면 generic block layer 경로로 진행 (추정) */
	struct hd_geometry *geo;
	char type[DASD_VOL_TYPE_LEN + 1] = "";	/* 매직/시그니처 버퍼: read_part_sector로 읽은 NVMe media 첫 4바이트 저장 (추정) */
	char name[DASD_VOL_ID_LEN + 1] = "";	/* 볼륨명 버퍼: NVMe와 무관한 디스플레이 정보 (추정) */
	sector_t labelsect;	/* 레이블이 위치한 sector 번호: NVMe namespace LBA (추정) */
	union label_t *label;	/* NVMe media 메타데이터를 오버레이하는 버퍼 (추정) */

	res = 0;	/* 아직 파티션 미등록: NVMe 하위 bdev 0개 (추정) */
	if (!disk->fops->getgeo)
		goto out_exit;	/* geometry 획득 불가 -> CHS->LBA 변환 불가 -> NVMe 파티션 스캔 포기 (추정) */
	fn = symbol_get(dasd_biodasdinfo); /* s390 DASD discipline 전용 심볼 (NVMe와 무관) */
	blocksize = bdev_logical_block_size(bdev);	/* NVMe namespace의 논리 블록 크기(512/4096 등): LBA 계산 기준 (추정) */
	if (blocksize <= 0)
		goto out_symbol;	/* 유효하지 않은 블록 크기 -> NVMe LBA 변환 불가 -> 스캔 중단 (추정) */
	nr_sectors = bdev_nr_sectors(bdev);	/* NVMe namespace 총 sector 수: 마지막 LBA 경계, sanity check에 사용 (추정) */
	if (nr_sectors == 0)
		goto out_symbol;	/* 빈 장치 -> NVMe Read/Write 대상 없음, 파티션 등록 의미 없음 (추정) */
	info = kmalloc_obj(dasd_information2_t);
	if (info == NULL)
		goto out_symbol;	/* 메모리 부족 -> NVMe 파티션 스캔 중단, 상위 check.c가 다음 파서 시도 (추정) */
	geo = kmalloc_obj(struct hd_geometry);
	if (geo == NULL)
		goto out_nogeo;	/* 메모리 부족 -> geo 할당 실패, 파티션 스캔 중단 (추정) */
	label = kmalloc_obj(union label_t);
	if (label == NULL)
		goto out_nolab;	/* 메모리 부족 -> label 버퍼 없이 NVMe media 메타데이터 해석 불가 (추정) */
	/* set start if not filled by getgeo function e.g. virtblk */
	geo->start = get_start_sect(bdev);	/* bdev 시작 sector: NVMe namespace가 파티션/오프셋을 가진 경우 기준 LBA (추정) */
	if (disk->fops->getgeo(disk, geo))
		goto out_freeall;	/* geometry 획득 실패 -> CHS->LBA 변환 불가 -> NVMe 파티션 스캔 중단 (추정) */
	if (!fn || fn(disk, info)) {
		kfree(info);
		info = NULL;	/* DASD discipline 정보 없음: FBA/ECKD/CMS 후보 3회 탐색 -> NVMe Read 최대 3회 (추정) */
	}

	if (find_label(state, info, geo, blocksize, &labelsect, name, type, label)) {	/* 레이블 탐색 -> read_part_sector -> submit_bio -> ... -> nvme_submit_cmd (추정) */
		switch (get_label_by_type(type)) {	/* 매직/시그니처 분기 -> NVMe 파티션 등록 방식 결정 (추정) */
		case DASD_VOLLBL_TYPE_VOL1:
			res = find_vol1_partitions(state, geo, blocksize, name,		/* VOL1 -> VTOC 체인 탐색 -> 다중 put_partition -> 다중 NVMe 하위 bdev (추정) */
						   label);
			break;
		case DASD_VOLLBL_TYPE_LNX1:
			res = find_lnx1_partitions(state, geo, blocksize, name,	/* LNX1 -> 단일 파티션 -> NVMe 하위 bdev 1개 (추정) */
						   label, labelsect, nr_sectors,
						   info);
			break;
		case DASD_VOLLBL_TYPE_CMS1:
			res = find_cms1_partitions(state, geo, blocksize, name,	/* CMS1 -> 단일 파티션(minidisk 포함) -> NVMe 하위 bdev 1개 (추정) */
						   label, labelsect);
			break;
		}
	} else if (info) {
		/*
		 * ugly but needed for backward compatibility:
		 * If the block device is a DASD (i.e. BIODASDINFO2 works),
		 * then we claim it in any case, even though it has no valid
		 * label. If it has the LDL format, then we simply define a
		 * partition as if it had an LNX1 label.
		 */
		res = 1;	/* 레이블 없어도 DASD로 인식 -> NVMe와는 무관하지만 block layer에서 장치 점유 (추정) */
		if (info->format == DASD_FORMAT_LDL) {
			seq_buf_puts(&state->pp_buf, "(nonl)");
			size = nr_sectors;	/* 전체 NVMe namespace를 단일 파티션 길이로 사용 (추정) */
			offset = (info->label_block + 1) * (blocksize >> 9);	/* 레이블 다음 block -> NVMe SLBA (추정) */
			put_partition(state, 1, offset, size-offset);	/* LDL fallback 파티션 등록 -> NVMe 하위 bdev 1개 (추정) */
			seq_buf_puts(&state->pp_buf, "\n");
		}
	} else
		res = 0;	/* DASD도 아니고 레이블도 없음 -> NVMe namespace에 IBM 파티션 없음 (추정) */

out_freeall:
	kfree(label);	/* label 버퍼 해제: NVMe media 메타데이터 해석용 메모리 반환 (추정) */
out_nolab:
	kfree(geo);	/* geo 버퍼 해제 (추정) */
out_nogeo:
	kfree(info);	/* info 버퍼 해제 (추정) */
out_symbol:
	if (fn)
		symbol_put(dasd_biodasdinfo);	/* s390 심볼 참조 해제: NVMe와 무관 (추정) */
out_exit:
	return res;	/* 0: NVMe 파티션 미등록, 1: 성공, -1: 스캔 에러 (추정) */
}

/* NVMe 관점 핵심 요약:
 * - block/partitions/ibm.c는 NVMe SQ/CQ/doorbell보다 상위인 파티션 파싱
 *   단계로, DASD 레이블을 LBA 범위로 변환하여 NVMe namespace 하위의
 *   파티션 장치를 형성한다.
 * - read_part_sector는 초기화 시 bio 기반 경로를 타며, NVMe 위에서는
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell)로 전달될 수 있다(추정).
 * - put_partition()으로 등록된 (start, size)는 이후 NVMe Read/Write CID의
 *   SLBA 및 Length 계산에 직접 사용된다.
 * - 이 파일은 block/partitions/check.c의 파서 호출 루프에서 불리며, 다른
 *   파티션 포맷 파서(msdos.c, gpt.c 등)와 동일한 block layer 위치에서
 *   동작한다.
 * - 본 파일은 s390 DASD 전용이므로 NVMe 하드웨어가 직접 DASD 레이블을
 *   해석하지는 않으며, 다만 파서가 노출한 파티션 범위를 기준으로 NVMe
 *   명령을 처리한다(추정).
 */
