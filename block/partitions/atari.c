// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/atari.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 */
/*
 * ============================================================================
 * NVMe SSD 관점 파일 요약
 * ============================================================================
 * 이 파일은 리눅스 블록 계층(block layer)에서 Atari AHDI/XGM/ICD 파티션
 * 테이블을 파싱하는 파티션 검출기이다. NVMe 입출력이 실제로 흐르는 hot
 * path는 아니지만, NVMe namespace(nvme0n1 등)가 커널에 등록될 때
 * add_disk -> bdev_disk_changed -> read_partitions -> check_partition ->
 * atari_partition 경로로 호출되어 namespace 전체 LBA 공간을 여러 파티션으로
 * 나눈다. 파싱 결과는 struct parsed_partitions에 저장되고, 이후
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 경로로 NVMe I/O가 전달될 때 파티션 오프셋을
 * 기반으로 bio의 LBA를 재매핑하는 데 사용된다.
 * ============================================================================
 */

#include <linux/ctype.h>
#include "check.h"
#include "atari.h"

/* ++guenther: this should be settable by the user ("make config")?.
 */
#define ICD_PARTS

/*
 * VALID_PARTITION 매크로 - Atari partition_info 엔트리가 유효한지 검사한다.
 *
 * NVMe 관련 필드 해석:
 *   pi->flg & 1   - 활성(active) 플래그. NVMe 입장에서는 의미 없는 레거시
 *                   비트이며, 단지 커널이 해당 엔트리를 파티션으로 등록할지
 *                   여부를 결정한다(추정).
 *   pi->id[0..2]  - "GEM", "BGM", "XGM" 등 파티션 ID. NVMe namespace의
 *                   LBA 공간에 논리적 이름을 부여하는 메타데이터일 뿐, NVMe
 *                   명령어에는 직접 반영되지 않는다.
 *   pi->st        - big-endian으로 저장된 파티션 시작 LBA. NVMe Read/Write
 *                   명령의 SLBA(Start LBA) 계산 시 호스트 endian으로 변환해야
 *                   한다.
 *   pi->siz       - big-endian으로 저장된 파티션 크기(섹터 수). namespace
 *                   총 용량(hd_size)을 초과하면 안 된다.
 */
/* check if a partition entry looks valid -- Atari format is assumed if at
   least one of the primary entries is ok this way */
#define	VALID_PARTITION(pi,hdsiz)					     \
    (((pi)->flg & 1) &&							     \
     isalnum((pi)->id[0]) && isalnum((pi)->id[1]) && isalnum((pi)->id[2]) && \
     be32_to_cpu((pi)->st) <= (hdsiz) &&				     \
     be32_to_cpu((pi)->st) + be32_to_cpu((pi)->siz) <= (hdsiz))

/*
 * OK_id() - Atari ICD 파티션 ID가 커널이 인식하는 ID인지 확인한다.
 *
 * 목적:
 *   GEM, BGM, LNX, SWP, RAW ID 문자열 중 하나인지 검사하여 ICD/Supra
 *   파티션 탐색 시 등록할 항목인지 판별한다.
 *
 * NVMe 연결:
 *   이 ID는 NVMe Admin Identify Command가 반환하는 namespace 정보와는
 *   무관하며, 순수하게 Atari 레이블의 메타데이터를 해석하는 용도이다(추정).
 */
static inline int OK_id(char *s)
{
	return  memcmp (s, "GEM", 3) == 0 || memcmp (s, "BGM", 3) == 0 ||
		memcmp (s, "LNX", 3) == 0 || memcmp (s, "SWP", 3) == 0 ||
		memcmp (s, "RAW", 3) == 0 ;
}

/*
 * ----------------------------------------------------------------------------
 * atari_partition()
 * ----------------------------------------------------------------------------
 * 목적:
 *   NVMe namespace를 통해 노출된 block device의 LBA 0에서 Atari
 *   rootsector를 읽어 AHDI 기본 파티션, XGM 확장 파티션, ICD/Supra
 *   파티션을 파싱하고 struct parsed_partitions에 등록한다.
 *
 * 호출 경로 (추정):
 *   add_disk -> bdev_disk_changed -> read_partitions -> check_partition ->
 *   atari_partition
 *
 * NVMe 연결 지점:
 *   - state->disk->queue는 해당 NVMe namespace의 request_queue이다.
 *   - queue_logical_block_size()가 512B가 아니면 Atari 형식을 지원하지
 *     않고 즉시 반환한다.
 *   - read_part_sector() 낮부에서 bdev/bio 기반 읽기가 발생하고, bio는
 *     submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *     nvme_submit_cmd(doorbell) 경로를 따라 NVMe Read 명령(SQ/CQ, CID,
 *     PRP/SGL)으로 변환될 수 있다(추정).
 *   - put_partition()으로 저장된 state는 이후 NVMe I/O가 파티션 단위로
 *     submit될 때 start_sect, nr_sects를 통해 LBA 변환에 사용된다.
 * ----------------------------------------------------------------------------
 */
int atari_partition(struct parsed_partitions *state)
{
	Sector sect;				/* read_part_sector()가 반환하는 버퍼/섹터 핸들; NVMe media -> host buffer 매핑의 추상화(추정) */
	struct rootsector *rs;		/* LBA 0의 on-disk Atari rootsector 메타데이터; NVMe Read로 512B 단위로 채워진다(추정) */
	struct partition_info *pi;	/* rootsector/icdpart 내 개별 파티션 엔트리; 필드 offset은 NVMe에서 읽은 raw byte layout 그대로(추정) */
	u32 extensect;				/* XGM 확장 파티션 체인의 기준점(base LBA); 서브 파티션 SLBA 계산 시 절대/상대 변환에 사용 */
	u32 hd_size;				/* NVMe namespace 전체 섹터 수; Identify Namespace의 NSZE/NCAP과 동일한 의미(추정) */
	int slot;					/* parsed_partitions->parts[] 인덱스; 파티션 등록 시 /dev/nvme0n1p<slot> 번호로 매핑(추정) */
#ifdef ICD_PARTS
	int part_fmt = 0; /* 0:unknown, 1:AHDI, 2:ICD/Supra */	/* 탐색 형식 플래그; NVMe namespace LBA layout 해석 방식을 구분(추정) */
#endif

	/* NVMe namespace의 논리 블록 크기가 512B가 아니면 Atari 파티션은 의미 없음 */
	/*
	 * ATARI partition scheme supports 512 lba only.  If this is not
	 * the case, bail early to avoid miscalculating hd_size.
	 */
	if (queue_logical_block_size(state->disk->queue) != 512)
		return 0;

	/* LBA 0를 읽어 Atari rootsector 획득 (NVMe Read 명령 발생 지점) */
	rs = read_part_sector(state, 0, &sect);	/* submit_bio -> blk_mq_submit_bio -> ... -> nvme_submit_cmd(doorbell)로 LBA 0 Read 전달(추정) */
	if (!rs)					/* read_part_sector 낮부에서 bio allocation/queue_rq/CQE error 시 NULL 반환(추정) */
		return -1;

	/* Verify this is an Atari rootsector: */
	/* gendisk 용량(섹터 단위) = NVMe namespace 전체 LBA 수 */
	hd_size = get_capacity(state->disk);	/* NVMe Identify Namespace의 NSZE와 일치; 이후 st/siz 경계 검증의 기준 */
	/* rootsector->part[0..3] 중 하나라도 유효해야 Atari 디스크로 간주 */
	if (!VALID_PARTITION(&rs->part[0], hd_size) &&
	    !VALID_PARTITION(&rs->part[1], hd_size) &&
	    !VALID_PARTITION(&rs->part[2], hd_size) &&
	    !VALID_PARTITION(&rs->part[3], hd_size)) {
		/*
		 * if there's no valid primary partition, assume that no Atari
		 * format partition table (there's no reliable magic or the like
	         * :-()
		 */
		put_dev_sector(sect);		/* NVMe media에서 DMA로 받은 버퍼 해제; bio completion 리소스 정리(추정) */
		return 0;
	}

	/* 첫 번째 primary partition부터 순회 시작 */
	pi = &rs->part[0];				/* rootsector 시작 offset + partition_info[0] field offset에서 NVMe media 원본 데이터 위치(추정) */
	seq_buf_puts(&state->pp_buf, " AHDI");
	/* part[0..3]를 순회하며 파티션 등록; slot은 parsed_partitions 배열 인덱스 */
	for (slot = 1; pi < &rs->part[4] && slot < state->limit; slot++, pi++) {
		struct rootsector *xrs;
		Sector sect2;			/* 확장 파티션 체인에서 추가 NVMe Read의 버퍼 핸들; 각각 독립적 PRP/SGL 매핑(추정) */
		ulong partsect;			/* 현재 서브 파티션 테이블의 절대 LBA; 확장 체인 탐색 시마다 새 NVMe Read CID 할당(추정) */

		/* flg bit 0이 설정되지 않으면 비활성 파티션, skip */
		if ( !(pi->flg & 1) )
			continue;			/* 비활성 엔트리는 /dev/nvme0n1p<N>에 등록되지 않음; NVMe I/O 경로에는 영향 없음 */
		/* active partition */
		/* "XGM"이 아닌 일반 파티션은 바로 등록 */
		if (memcmp (pi->id, "XGM", 3) != 0) {
			/* we don't care about other id's */
			/* pi->st, pi->siz는 big-endian이므로 CPU endian 변환 후 등록 */
			put_partition (state, slot, be32_to_cpu(pi->st),
					be32_to_cpu(pi->siz));	/* parsed_partitions->parts[slot]에 {start_sect=pi->st, nr_sects=pi->siz} 저장; 이후 submit_bio 시 bio->bi_iter.bi_sector에 start_sect를 더해 NVMe SLBA 재계산(추정) */
			continue;
		}
		/* extension partition */
#ifdef ICD_PARTS
		part_fmt = 1; /* 확장 파티션 존재 시 AHDI 형식으로 표시 */	/* AHDI 형식으로 식별; NVMe namespace LBA 상에 XGM linked list가 존재함을 의미 */
#endif
		seq_buf_puts(&state->pp_buf, " XGM<");
		/* 확장 파티션의 시작 LBA; 이후 linked list로 연결된 서브 파티션 탐색 */
		partsect = extensect = be32_to_cpu(pi->st);	/* extensect는 상대 오프셋의 base; 모든 서브 테이블 SLBA는 extensect + next_relative(추정) */
		while (1) {
			/* 각 서브 파티션 테이블은 별도 섹터를 읽어야 함 (NVMe Read 추가 발생) */
			xrs = read_part_sector(state, partsect, &sect2);	/* 확장 체인의 매 노드마다 별도 NVMe Read 명령(SQ entry/CID) 생성; I/O submission volume 증가(추정) */
			if (!xrs) {
				printk (" block %ld read failed\n", partsect);		/* NVMe CQE status가 error이거나 queue가 reject한 경우(추정) */
				put_dev_sector(sect);							/* rootsector 버퍼 해제; 파티션 스캔 중단 */
				return -1;
			}

			/* ++roman: sanity check: bit 0 of flg field must be set */
			/* 서브 파티션도 flg bit 0이 설정되어야 유효 */
			if (!(xrs->part[0].flg & 1)) {
				printk( "\nFirst sub-partition in extended partition is not valid!\n" );
				put_dev_sector(sect2);							/* 유효하지 않은 서브 파티션 테이터 폐기; NVMe 버퍼 반납 */
				break;
			}

			/* 서브 파티션의 실제 시작은 partsect 기준 상대 오프셋 사용 */
			put_partition(state, slot,
				   partsect + be32_to_cpu(xrs->part[0].st),	/* 절대 SLBA = 서브 테이블 LBA + 엔트리 내 relative st; NVMe PRP SLBA 필드와 동일한 단위(추정) */
				   be32_to_cpu(xrs->part[0].siz));			/* 서브 파티션 크기; /proc/partitions의 SECTORS 열 및 NVMe 범위 검증에 사용(추정) */

			/* part[1]이 유효하지 않으면 linked list 종료 */
			if (!(xrs->part[1].flg & 1)) {
				/* end of linked partition list */
				put_dev_sector(sect2);
				break;
			}
			/* 다음 서브 파티션 테이블을 가리키는 항목도 "XGM"이어야 함 */
			if (memcmp( xrs->part[1].id, "XGM", 3 ) != 0) {
				printk("\nID of extended partition is not XGM!\n");
				put_dev_sector(sect2);
				break;
			}

			/* 다음 확장 파티션 테이블의 절대 LBA 계산 */
			partsect = be32_to_cpu(xrs->part[1].st) + extensect;	/* 다음 NVMe Read 대상 SLBA; extensect base + relative pointer(추정) */
			put_dev_sector(sect2);								/* 현재 서브 테이블 버퍼 해제; 다음 read_part_sector 호출 전 메모리 정리 */
			if (++slot == state->limit) {
				printk( "\nMaximum number of partitions reached!\n" );
				break;										/* parsed_partitions->limit 초과 시 NVMe namespace당 파티션 개수 제한 도달 */
			}
		}
		seq_buf_puts(&state->pp_buf, " >");
	}
#ifdef ICD_PARTS
	/* 확장 파티션이 없는 경우 ICD/Supra 형식 추가 탐색 */
	if ( part_fmt!=1 ) { /* no extended partitions -> test ICD-format */
		pi = &rs->icdpart[0];		/* rootsector의 ICD 영역 시작 offset; NVMe Read로 받은 512B 내 icdpart[0] 위치(추정) */
		/* ICD 파티션의 첫 항목 id가 GEM/BGM/RAW/LNX/SWP 중 하나여야 ICD로 인정 */
		/* sanity check: no ICD format if first partition invalid */
		if (OK_id(pi->id)) {
			seq_buf_puts(&state->pp_buf, " ICD<");
			for (; pi < &rs->icdpart[8] && slot < state->limit; slot++, pi++) {
				/* accept only GEM,BGM,RAW,LNX,SWP partitions */
				if (!((pi->flg & 1) && OK_id(pi->id)))
					continue;			/* 조건 불만족 시 /dev/nvme0n1p<N> 미등록; NVMe 입장에서 빈 LBA 영역으로 취급 */
				/* flg active && id가 허용 목록에 있을 때만 등록 */
				put_partition (state, slot,
						be32_to_cpu(pi->st),	/* ICD 파티션의 NVMe namespace 상대 시작 LBA; bio remap 시 더해짐(추정) */
						be32_to_cpu(pi->siz));	/* ICD 파티션 섹터 수; NVMe Read/Write 범위 검사 시 nr_sects 참조(추정) */
			}
			seq_buf_puts(&state->pp_buf, " >");
		}
	}
#endif
	put_dev_sector(sect);			/* rootsector DMA buffer 해제; NVMe media -> host 메모리 매핑 종료(추정) */

	seq_buf_puts(&state->pp_buf, "\n");

	return 1;
}

/* ============================================================================
 * NVMe 관점 핵심 요약
 * ----------------------------------------------------------------------------
 * 1. 이 파일은 NVMe I/O hot path가 아니라, 디스크 탐색 단계에서
 *    add_disk -> bdev_disk_changed -> read_partitions -> check_partition ->
 *    atari_partition 순으로 한 번 호출된다.
 * 2. read_part_sector() 호출 시 NVMe SSD로의 실제 Read는
 *    submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *    nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를 따라 NVMe SQ/CQ,
 *    CID, PRP/SGL 형태로 전달될 수 있다(추정).
 * 3. queue_logical_block_size()가 512B가 아닌 NVMe namespace에서는 Atari
 *    파티션 테이블을 지원하지 않고 즉시 반환한다.
 * 4. Atari 파티션의 st(시작), siz(크기) 필드는 big-endian이므로 NVMe가
 *    사용하는 LBA(호스트 endian)로 변환해야 한다.
 * 5. partition_info의 flg 필드는 활성(active) 여부를 나타낼 뿐, NVMe
 *    입장에서 특별한 의미는 없고 단순히 커널의 파티션 등록 여부를
 *    결정한다(추정).
 * ============================================================================
 */
