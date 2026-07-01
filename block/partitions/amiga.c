// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/amiga.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 *
 *  ===========================================================================
 *  NVMe SSD 관점 파일 요약 (5-10줄)
 *  ===========================================================================
 *  이 파일은 Amiga RDB(RigidDiskBlock) 형식의 파티션 테이블을 파싱하여
 *  커널의 generic block layer에 등록하는 역할을 수행합니다.
 *  NVMe 스택에서 볼 때, 이 코드는 nvme块设备 -> gendisk -> partition scan
 *  경로의 최상위 탐색 단계에 위치하며, 실제 NVMe I/O 경로
 *  (submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)) 와는 직접적으로 연결되지
 *  않습니다. 다만, 파티션 경계(start_sect, nr_sects)가 결정되면 이후
 *  NVMe 드라이버가 CID, SQ, CQ, PRP/SGL 기반으로 해당 LBA 범위로의
 *  read/write 명령을 발행할 때 사용됩니다.
 *  ===========================================================================
 */

#define pr_fmt(fmt) fmt	/* dmesg 출력 포맷; NVMe 드라이버 오류 메시지와 동일한 커널 로그 채널 사용 */

#include <linux/types.h>	/* u32/u64 등 기본 타입; NVMe 명령 구조체(CDW, PRP)와 동일한 크기 정의 재사용 */
#include <linux/mm_types.h>	/* 메모리 관리 타입; read_part_sector가 할당한 DMA 가능 버퍼의 page 상태 참조 시 사용 (추정) */
#include <linux/overflow.h>	/* 산술 오버플로우 검사; LBA 범위가 NVMe Namespace 경계를 넘지 않도록 보호 */
#include <linux/affs_hardblocks.h>	/* RDB/PartitionBlock 구조체 정의; NVMe로부터 읽은 on-disk 메타데이터 레이아웃 */

#include "check.h"	/* partition scan 프레임워크 헤더; rescan_partitions -> check_partition -> amiga_partition 연결 */

/* magic offsets in partition DosEnvVec */
/* Amiga 파티션 환경 벡터(DosEnvVec) 내 오프셋 정의.
 * NVMe 입장에서는 이 값들이 결국 nvme Namespace LBA로 변환되기 전의
 * CHS 스타일 좌표일 뿐이며, 실제 플래시 접근은 NVMe 컨트롤러가
 * LBA -> NAND 주소로 매핑합니다.
 */
#define NR_HD	3	/* DosEnvVec[3]: 헤드 수; CHS -> LBA 변환의 한 요소 (NVMe는 CHS를 직접 사용하지 않음) */
#define NR_SECT	5	/* DosEnvVec[5]: 트랙당 섹터 수; NVMe Namespace의 LBAF와 별개 */
#define LO_CYL	9	/* DosEnvVec[9]: 시작 실린더; 최종적으로 start_sect로 환산되어 NVMe SLBA 후보가 됨 */
#define HI_CYL	10	/* DosEnvVec[10]: 종료 실린더; nr_sects 산출에 사용되어 NVMe NLB 후보가 됨 */

/* RDB 블록의 체크섬을 계산.
 * NVMe 관점: 이 체크섬 검증은 파티션 탐색 단계에서 CPU 메모리 내에서
 * 이루어지며, NVMe의 End-to-End Data Protection(CRC 등)과는 별개입니다.
 * I/O 경로상에서는 read_part_sector -> submit_bio -> ... -> nvme_queue_rq
 * (CID 할당, PRP/SGL 구성)를 통해 이 블록이 DMA로 전달되었습니다.
 */
static __inline__ u32	/* u32 누적합; NVMe CQE status와 무관하게 CPU에서 계산 */
checksum_block(__be32 *m, int size)	/* m: NVMe DMA 버퍼 내 빅엔디안 워드 배열, size: 워드 개수 */
{
	u32 sum = 0;	/* CPU 메모리 상 누적값; NVMe PRP/SGL로 수신된 데이터의 word 단위 합 */

	while (size--)	/* RDB/PartitionBlock 전체 워드를 순회; NVMe 미디어에서 읽은 바이트 그대로 해석 */
		sum += be32_to_cpu(*m++);	/* 빅엔디안 워드를 CPU endian으로 변환 후 누적; checksum==0이면 무결성 통과 */
	return sum;	/* 0이면 정상; 비정상일 경우 이 LBA는 유효한 Amiga 메타데이터로 취급하지 않음 */
}

/* Amiga 파티션 테이블 탐색 및 커널 등록의 핵심 함수.
 *
 * 목적:
 *   디스크의 RDB를 순회하면서 AmigaDOS 파티션 엔트리를 찾아내고,
 *   각 파티션의 시작 섹터(start_sect)와 섹터 수(nr_sects)를 산출하여
 *   put_partition()로 커널에 등록합니다.
 *
 * 호출 경로 (추정):
 *   device_add_disk -> bdev_disk_changed -> blkdev_parts_changed
 *   -> rescan_partitions -> check_partition -> amiga_partition
 *
 * NVMe 연결 지점:
 *   - 파라미터 state->disk 는 nvme에 의해 할당된 gendisk/disk_object와
 *     연결되어 있습니다.
 *   - 이 함수가 반환한 후, NVMe 드라이버는 해당 파티션의 bio 요청을
 *     blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq 경로로
 *     받아, CID를 부여하고 doorbell을 울려 SQ에 명령을 배치합니다.
 *   - NVMe Namespace의 LBA 단위(typically 512B or 4KB)와 blksize 보정이
 *     맞지 않으면 이후 I/O 정렬 문제로 이어질 수 있습니다.
 */
int amiga_partition(struct parsed_partitions *state)	/* state: partition scan 상태; state->disk는 NVMe가 등록한 gendisk 객체 */
{
	Sector sect;	/* read_part_sector가 반환하는 단일 512B 섹터 버퍼; NVMe PRP/SGL DMA 완료 후 커널 메모리 참조 */
	unsigned char *data;	/* NVMe 미디어에서 읽어온 메타데이터에 대한 커널 가상 주소 */
	struct RigidDiskBlock *rdb;	/* NVMe LBA 0 근처에서 탐색하는 RDB 레이아웃 (on-disk struct) */
	struct PartitionBlock *pb;	/* PartitionList 체인의 각 엔트리; NVMe read 결과를 구조체로 해석 */
	u64 start_sect, nr_sects;	/* 512B 정규화된 파티션 시작/섹터 수; NVMe Read/Write 명령의 SLBA/NLB 산출 기초 */
	sector_t blk, end_sect;	/* blk: 탐색 중인 NVMe Namespace 상의 512B LBA; end_sect: start_sect + nr_sects */
	u32 cylblk;		/* rdb_CylBlocks = nr_heads*sect_per_track; 512B 정규화 후 NVMe LBA 계산에 사용 */
	u32 nr_hd, nr_sect, lo_cyl, hi_cyl;	/* Amiga CHS 파라미터; NVMe 컨트롤러는 CHS를 모르고 LBA로만 접근 */
	int part, res = 0;	/* part: 1~16 파티션 인덱스; res: 0=미발견, 1=등록성공, -1=I/O오류(NVMe read 실패 가능) */
	unsigned int blksize = 1;	/* Multiplier for disk block size; RDB 블록당 512B 배수, NVMe LBAF와 정합 필요 */
	int slot = 1;	/* put_partition에 전달할 minor 슬롯; NVMe namespace 위의 파티션 번호 */

	/* RDB 할당 한계(RDB_ALLOCATION_LIMIT)까지 블록을 순회.
	 * NVMe 관점: 각 read_part_sector 호출은 bio/submit_bio를 통해
	 * NVMe 컨트롤러의 Admin 또는 I/O SQ에 read 명령(CID 할당 후
	 * doorbell)을 발행하고, CQ 인터럽트를 기다리는 과정입니다.
	 */
	for (blk = 0; ; blk++, put_dev_sector(sect)) {	/* blk를 512B LBA로 증가시키며 이전 섹터 버퍼 해제 */
		if (blk == RDB_ALLOCATION_LIMIT)	/* NVMe Namespace 전면 탐색 한계 도달; RDB 미발견으로 종료 */
			goto rdb_done;	/* 탐색 종료; res는 0으로 유지됨 */
		data = read_part_sector(state, blk, &sect);	/* blk LBA를 NVMe Read 명령으로 읽어 sect 버퍼 채움 */
		if (!data) {	/* NVMe Read 명령 실패 또는 메모리 할당 실패; CQE error/status 불량 가능 */
			pr_err("Dev %s: unable to read RDB block %llu\n",
			       state->disk->disk_name, blk);
			res = -1;	/* I/O 오류 표시; 상위 rescan_partitions가 탐색 중단 또는 재시도 판단 */
			goto rdb_done;	/* 더 이상의 NVMe 명령 발행 중단 */
		}
		/* RDB 매직 시그니처(IDNAME_RIGIDDISK) 확인.
		 * NVMe 관점: 맞지 않으면 다음 LBA로 넘어가는 단순 탐색 단계로,
		 * 이 시점까지 NVMe 에러는 아닙니다.
		 */
		if (*(__be32 *)data != cpu_to_be32(IDNAME_RIGIDDISK))	/* LBA의 첫 4바이트가 'RDSK'인지 확인; 아니면 다음 LBA */
			continue;	/* 현재 LBA는 Amiga RDB가 아님; put_dev_sector로 버퍼 해제 후 다음 LBA 탐색 */

		rdb = (struct RigidDiskBlock *)data;	/* NVMe로부터 DMA 완료된 메모리를 RDB 구조체로 캐스팅 */
		/* RDB 체크섬 검증. rdb_SummedLongs 하위 7비트만 사용.
		 * (추정) 상위 비트는 체크섬에 포함하지 않는 레거시 정책입니다.
		 */
		if (checksum_block((__be32 *)data, be32_to_cpu(rdb->rdb_SummedLongs) & 0x7F) == 0)	/* 0이면 무결성 통과 */
			break;	/* 유효한 RDB 발견; PartitionList 탐색 단계로 진출 */
		/* Try again with 0xdc..0xdf zeroed, Windows might have
		 * trashed it.
		 */
		/* Windows가 해당 워드를 훼손했을 가능성을 대비한 우회 검증.
		 * NVMe 관점: 플래시 상의 데이터 무결성은 NVMe Protection
		 * Information(PI)로 보호받을 수 있으나, 이 영역은 사용자가
		 * 직접 수정한 파티션 메타데이터이므로 PI와는 무관합니다.
		 */
		*(__be32 *)(data+0xdc) = 0;	/* 메모리 내 수정(0xdc 오프셋 4바이트 0으로 덮어씀); NVMe 쓰기 아님 */
		if (checksum_block((__be32 *)data,
				be32_to_cpu(rdb->rdb_SummedLongs) & 0x7F)==0) {	/* 우회 체크섬 재검증 */
			pr_err("Trashed word at 0xd0 in block %llu ignored in checksum calculation\n",
			       blk);
			break;	/* 훼손 워드 무시 후 유효 RDB로 인정; PartitionList 탐색 진출 */
		}

		pr_err("Dev %s: RDB in block %llu has bad checksum\n",
		       state->disk->disk_name, blk);	/* 체크섬 최종 불일치; 이 LBA의 RDB는 손상됨 */
	}

	/* blksize is blocks per 512 byte standard block */
	/* blksize: RDB가 사용하는 논리 블록이 512바이트 표준 블록 몇 개인지.
	 * NVMe 관점: Namespace의 LBAF 포맷과 다를 수 있으며, 이후
	 * start_sect/nr_sects가 512바이트 단위로 정규화되어야
	 * NVMe Read/Write 명령의 SLBA 필드와 호환됩니다.
	 */
	blksize = be32_to_cpu( rdb->rdb_BlockBytes ) / 512;	/* rdb_BlockBytes / 512 = NVMe 512B LBA 단위 승수 */

	/* Be more informative */
	seq_buf_printf(&state->pp_buf, " RDSK (%d)", blksize * 512);	/* /proc/partitions 등에 표시될 디스크 라벨; NVMe 장치 이름 옆 노출 */
	blk = be32_to_cpu(rdb->rdb_PartitionList);	/* RDB가 가리키는 첫 PartitionBlock의 LBA (아직 blksize 미변환) */
	put_dev_sector(sect);	/* RDB 섹터 버퍼 해제; 이후 PartitionList 체인 탐색 시작, 추가 NVMe Read 예정 */
	/* PartitionList를 따라 최대 16개 파티션 탐색.
	 * NVMe 관점: 파티션당 최소 한 번의 read_part_sector -> submit_bio
	 * -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
	 * -> nvme_submit_cmd(doorbell)가 발생할 수 있습니다.
	 */
	for (part = 1; (s32) blk>0 && part<=16; part++, put_dev_sector(sect)) {	/* pb_Next 체인이 0이거나 16개 슬롯 초과 시 종료; 매 반복 at least 1 NVMe read */
		/* Read in terms partition table understands */
		/* blksize 곱으로 NVMe LBA(512B 기준) 좌표로 변환.
		 * 오버플로우 시 이 파티션 이후는 모두 스킵.
		 * (추정) Amiga RDB는 32비트 좌표를 주로 사용하므로
		 * LBD(Large Block Device)가 없는 환경에서 문제가 될 수 있습니다.
		 */
		if (check_mul_overflow(blk, (sector_t) blksize, &blk)) {	/* PartitionList LBA * blksize = 512B 기준 NVMe LBA; overflow면 안전하지 않음 */
			pr_err("Dev %s: overflow calculating partition block %llu! Skipping partitions %u and beyond\n",
				state->disk->disk_name, blk, part);
			break;	/* 이후 파티션은 더 이상 정확한 LBA를 계산할 수 없으므로 NVMe 명령 발행 중단 */
		}
		data = read_part_sector(state, blk, &sect);	/* PartitionBlock 읽기: submit_bio -> blk_mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell) */
		if (!data) {	/* NVMe Read 명령 실패 또는 메모리 할당 실패; 이 PartitionBlock 탐색 불가 */
			pr_err("Dev %s: unable to read partition block %llu\n",
			       state->disk->disk_name, blk);
			res = -1;	/* I/O 오류 기록; 상위가 탐색 중단/재시도 */
			goto rdb_done;	/* 추가 PartitionList 체인 탐색 중단 */
		}
		pb  = (struct PartitionBlock *)data;	/* PartitionBlock 구조체로 캐스팅; NVMe로부터 DMA 완료된 메타데이터 */
		blk = be32_to_cpu(pb->pb_Next);	/* 체인의 다음 PartitionBlock LBA; 0이면 체인 종료, 아니면 추가 NVMe Read */
		if (pb->pb_ID != cpu_to_be32(IDNAME_PARTITION))	/* PARTITION 매직 시그니처 확인; 불일치 시 손상되거나 다른 블록 */
			continue;	/* 현재 엔트리 무시; put_dev_sector로 버퍼 해제 후 다음 체인 진행 */
		if (checksum_block((__be32 *)pb, be32_to_cpu(pb->pb_SummedLongs) & 0x7F) != 0 )	/* PartitionBlock 체크섬 검증; 실패 시 손상된 메타데이터 */
			continue;	/* 체크섬 불일치 엔트리 무시; NVMe 미디어 또는 사용자 수정으로 인한 손상 가능 */

		/* RDB gives us more than enough rope to hang ourselves with,
		 * many times over (2^128 bytes if all fields max out).
		 * Some careful checks are in order, so check for potential
		 * overflows.
		 * We are multiplying four 32 bit numbers to one sector_t!
		 */

		nr_hd   = be32_to_cpu(pb->pb_Environment[NR_HD]);	/* DosEnvVec[3]: 헤드 수; CHS -> LBA 변환용 */
		nr_sect = be32_to_cpu(pb->pb_Environment[NR_SECT]);	/* DosEnvVec[5]: 실린더당 섹터 수; NVMe는 직접 사용하지 않음 */

		/* CylBlocks is total number of blocks per cylinder */
		if (check_mul_overflow(nr_hd, nr_sect, &cylblk)) {	/* heads * sects = cylblk; CHS 기반 cylinder 크기 산출 */
			pr_err("Dev %s: heads*sects %u overflows u32, skipping partition!\n",
				state->disk->disk_name, cylblk);
			continue;	/* overflow 파티션 스킵; 잘못된 CHS는 NVMe LBA 계산에 치명적 */
		}

		/* check for consistency with RDB defined CylBlocks */
		if (cylblk > be32_to_cpu(rdb->rdb_CylBlocks)) {	/* RDB의 CylBlocks와 비교; 불일치하면 잘못된 파티션 파라미터(추정) */
			pr_warn("Dev %s: cylblk %u > rdb_CylBlocks %u!\n",
				state->disk->disk_name, cylblk,
				be32_to_cpu(rdb->rdb_CylBlocks));
		}

		/* RDB allows for variable logical block size -
		 * normalize to 512 byte blocks and check result.
		 */

		if (check_mul_overflow(cylblk, blksize, &cylblk)) {	/* cylinder 블록 수 * 512B 승수 = 512B 기준 cylinder당 블록 수 */
			pr_err("Dev %s: partition %u bytes per cyl. overflows u32, skipping partition!\n",
				state->disk->disk_name, part);
			continue;	/* 512B 정규화 불가능; NVMe SLBA 계산 불가 */
		}

		/* Calculate partition start and end. Limit of 32 bit on cylblk
		 * guarantees no overflow occurs if LBD support is enabled.
		 */

		lo_cyl = be32_to_cpu(pb->pb_Environment[LO_CYL]);	/* DosEnvVec[9]: 시작 실린더 */
		start_sect = ((u64) lo_cyl * cylblk);	/* CHS 시작 -> 512B 정규화 LBA (NVMe SLBA 후보) */

		hi_cyl = be32_to_cpu(pb->pb_Environment[HI_CYL]);	/* DosEnvVec[10]: 종료 실린더 */
		nr_sects = (((u64) hi_cyl - lo_cyl + 1) * cylblk);	/* CHS 크기 -> 512B 정규화 섹터 수 (NVMe NLB 후보) */

		if (!nr_sects)	/* 크기 0 파티션은 등록 의미 없음; NVMe I/O도 발생하지 않음 */
			continue;

		/* Warn user if partition end overflows u32 (AmigaDOS limit) */

		if ((start_sect + nr_sects) > UINT_MAX) {	/* AmigaDOS 32비트 한계 경고; 64비트 NVMe Namespace에서는 계속 등록 가능 */
			pr_warn("Dev %s: partition %u (%llu-%llu) needs 64 bit device support!\n",
				state->disk->disk_name, part,
				start_sect, start_sect + nr_sects);
		}

		if (check_add_overflow(start_sect, nr_sects, &end_sect)) {	/* start+size overflow 검사; LBD 미지원 시 파티션 스킵 */
			pr_err("Dev %s: partition %u (%llu-%llu) needs LBD device support, skipping partition!\n",
				state->disk->disk_name, part,
				start_sect, end_sect);
			continue;	/* LBD 없이는 NVMe Namespace 끝을 넘는 I/O를 안전히 처리할 수 없음 */
		}

		/* Tell Kernel about it */
		/* 커널에 파티션 등록. 등록된 (start_sect, nr_sects)는 이후
		 * NVMe I/O 경로에서 bio의 bi_sector 보정(partition start
		 * offset 더하기)에 사용되며, 최종적으로 nvme_queue_rq에서
		 * PRP/SGL에 매핑될 NVMe 명령의 Starting LBA(SLBA)가 됩니다.
		 */
		put_partition(state,slot++,start_sect,nr_sects);	/* state->disk(→NVMe gendisk)에 slot번 파티션 등록; bio는 partition start offset 보정 후 NVMe SLBA로 변환 */
		{
			/* Be even more informative to aid mounting */
			char dostype[4];	/* 4바이트 Amiga dostype; 파일시스템 식별용, NVMe I/O 경로와는 무관 */

			__be32 *dt = (__be32 *)dostype;	/* 4바이트 dostype을 빅엔디안 워드로 해석 (Amiga on-disk format) */
			*dt = pb->pb_Environment[16];	/* PartitionBlock 환경 벡터의 dostype 필드 복사 */
			if (dostype[3] < ' ')	/* 제어 문자 처리 분기; 사용자가 lsblk 등에서 NVMe 파티션 식별 정보로 확인 */
				seq_buf_printf(&state->pp_buf,
					       " (%c%c%c^%c)",
					       dostype[0], dostype[1],
					       dostype[2],
					       dostype[3] + '@');
			else
				seq_buf_printf(&state->pp_buf,
					       " (%c%c%c%c)",
					       dostype[0], dostype[1],
					       dostype[2], dostype[3]);
			seq_buf_printf(&state->pp_buf, "(res %d spb %d)",
				       be32_to_cpu(pb->pb_Environment[6]),
				       be32_to_cpu(pb->pb_Environment[4]));	/* 추가 Amiga 메타데이터; NVMe I/O 정렬에는 직접 영향 없음 */
		}
		res = 1;	/* 최소 하나의 파티션을 성공적으로 등록 */
	}
	seq_buf_puts(&state->pp_buf, "\n");	/* 파티션 정보 문자열 종료 */

rdb_done:
	return res;	/* 1: 파티션 발견, 0: Amiga RDB 없음/발견 안 됨, -1: I/O 오류 (NVMe read 실패 가능) */
}

/* ==========================================================================
 * NVMe 관점 핵심 요약
 * ==========================================================================
 * - 이 파일은 Amiga RDB 파티션 테이블 파싱 계층으로, NVMe I/O hot path
 *   (submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *    nvme_queue_rq -> nvme_submit_cmd(doorbell)) 에 직접 속하지 않습니다.
 * - 다만 put_partition()으로 등록된 (start_sect, nr_sects)는 이후
 *   NVMe 명령의 SLBA 범위를 결정하므로, NVMe Namespace의 LBA 단위와
 *   정확히 정합해야 합니다.
 * - checksum_block의 체크섬은 사용자 영역 파티션 메타데이터 무결성을
 *   검사하는 것이며, NVMe End-to-End Data Protection(PI)이나 PRP/SGL
 *   기반 DMA 무결성과는 목적이 다릅니다.
 * - blksize(512B 단위 승수)는 NVMe LBAF 포맷과 다를 수 있으므로
 *   bio/bdev 레벨에서 추가 섹터 변환이 발생할 수 있습니다.
 * - 논리적으로 이 파일은 block/partitions/check.c 등 파티션 탐색
 *   프레임워크 뒤에 위치하며, block/blk-mq.c, drivers/nvme/host/core.c
 *   뒤의 실제 NVMe I/O 처리와 연결됩니다.
 * ==========================================================================
 */
