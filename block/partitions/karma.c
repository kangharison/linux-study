// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/karma.c
 *  Rio Karma partition info.
 *
 *  Copyright (C) 2006 Bob Copeland (me@bobcopeland.com)
 *  based on osf.c
 *
 *  파일 개요 (NVMe 관점):
 *  이 파일은 Rio Karma 휴용 플레이어의 디스크 레이블(partition table)을 파싱하는
 *  block layer 파티션 탐지기(partition parser)이다. NVMe SSD 입장에서는, 상위
 *  파일시스템/IO 스택이 submit_bio를 통해 본 장치에 대한 bio를 생성하기 전,
 *  block/partitions/msdos.c, gpt.c 등과 함께 NVMe namespace(block device)의
 *  섹터 0을 읽어 파티션 경계를 먼저 인식하는 선행 단계에 해당한다.
 *  즉, nvme_queue_rq -> nvme_submit_cmd(doorbell)로 전달될 LBA가 어느
 *  파티션 안에 속하는지 결정하는 메타데이터 해석기 역할을 한다 (추정).
 */

#include "check.h"		/* block layer 파티션 탐지 공용 헤더: read_part_sector(), put_partition(), Sector 등의 NVMe 중간 호출 규약 정의 */
#include <linux/compiler.h>	/* __packed 등 컴파일러 속성; NVMe에서 읽은 raw 바이트 스트림을 padding 없이 구조체에 매핑할 때 사용 */

/*
 * KARMA_LABEL_MAGIC: Rio Karma 펌웨어가 섹터 0 끝부분에 기록하는 시그니처.
 * NVMe 관점에서 이 magic이 맞지 않으면, 이 namespace는 Karma 파티션
 * 레이아웃이 아니므로 상위 block layer가 다른 parser(msdos/gpt 등)로
 * 넘어가게 된다.
 */
#define KARMA_LABEL_MAGIC		0xAB56	/* NVMe CQE 완료 후 사용자 공간에 보이는 시그니처; 일치하지 않으면 NVMe namespace가 Karma 미디어로부터 읽은 것이 아님 */

/*
 * karma_partition - Rio Karma 파티션 테이블을 탐지하고 등록한다.
 *
 * 목적:
 *   NVMe namespace(또는 기타 block device)의 0번 섹터를 읽어, Rio Karma
 *   고유의 디스크 레이블인지 확인하고, 유효한 파티션을 parsed_partitions
 *   상태에 등록(put_partition)한다.
 *
 * 호출 경로 (NVMe SSD를 타겟으로 할 때):
 *   사용자/커널 마운트 시도 -> vfs -> blkdev_open -> rescan_partitions
 *   -> check_partition -> [block/partitions/msdos.c 등이 먼저 시도될 수 있음]
 *   -> karma_partition -> read_part_sector -> submit_bio
 *      -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *      -> nvme_submit_cmd(doorbell) -> NVMe Read 명령(CID 할당, PRP/SGL)
 *   완료 인터럽트(MSI-X) -> nvme_irq -> nvme_process_cq -> bio 완료
 *   -> 섹터 데이터가 반환됨.
 *
 * NVMe 연결점:
 *   - read_part_sector() 내부에서 최종적으로 NVMe namespace에 Read(0 LBA)
 *     명령이 날아간다.
 *   - 반환된 data는 NVMe DMA 버퍼이며, 그 안의 d_magic, d_partitions[]
 *     필드를 CPU 바이트오더로 변환해 해석한다.
 *   - 파티션 offset/size는 이후 상위 IO 경로에서 bio->bi_iter.bi_sector에
 *     더해져 NVMe 명령의 SLBA(Starting LBA)로 변환된다.
 */
int karma_partition(struct parsed_partitions *state)	/* state->bdev가 NVMe namespace block device를 가리킬 때 날라오는 partition parser 진입점 */
{
	int i;			/* d_partitions[2] 순회 인덱스; 각 항목은 NVMe namespace 내 연속 LBA 구간 후보 */
	int slot = 1;		/* block layer가 할당하는 파티션 번호; /dev/nvme0n1p1, p2 등으로 매핑됨 */
	Sector sect;		/* read_part_sector()가 반환하는 섹터 버퍼의 수명 관리 객체; NVMe DMA 완료 후 put_dev_sector()로 해제 */
	unsigned char *data;	/* NVMe Read(0) 결과로 받은 raw 512B(추정) 섹터 데이터 포인터; sector size 정렬 버퍼 (추정) */

	/*
	 * struct disklabel: Rio Karma가 섹터 0에 기록하는 레이아웃.
	 * NVMe 입장에서는 namespace의 첫 번째 논리 블록(LBA 0)에 위치하며,
	 * 이 구조체 필드들이 NVMe Read 명령으로부터 반환된 바이트 스트림을
	 * 어떻게 해석할지를 정의한다.
	 */
	struct disklabel {
		u8 d_reserved[270];		/* NVMe LBA 0 앞부분 270바이트(오프셋 0~269): 펌웨어/부트 관련 데이터로 보임 (추정); NVMe Read로 전송되나 파티션 판별에는 직접 사용 안 함 */
		struct d_partition {
			__le32 p_res;		/* 리틀엔디언 예약 필드; NVMe 명령에는 직접 쓰이지 않음 */
			u8 p_fstype;		/* 파일시스템 타입; 0x4d일 때만 block layer에 파티션으로 노출, 그 외는 NVMe namespace의 미사용 LBA 영역으로 간주 */
			u8 p_res2[3];		/* 정렬/예약용; NVMe PRP/SGL과 무관, 레이아웃 패딩 */
			__le32 p_offset;	/* 파티션 시작 섹터; bio->bi_sector에 더해져 NVMe Read/Write의 SLBA(Starting LBA)로 직접 변환 */
			__le32 p_size;		/* 파티션 크기(섹터 단위); NVMe namespace 총 용량(nsze) 내에서 p_offset+p_size 경계가 유효한지 간접 검증 */
		} d_partitions[2];		/* Karma는 최대 2개 파티션을 기록; NVMe 입장에서는 2개의 연속 LBA 구간 */
		u8 d_blank[208];		/* 오프셋 286~493 미사용/패딩 영역; NVMe Read 시에도 함께 전송됨, DMA 버퍼 내 불필요 payload */
		__le16 d_magic;			/* 오프셋 494~495의 0xAB56; NVMe CQE 성공 후 이 시그니처가 맞아야 Karma 레이아웃으로 인식됨 */
	} __packed *label;			/* __packed: 컴파일러가 padding을 삽입하지 못하게 해 NVMe 미디어의 raw 바이트 오프셋과 구조체 필드 오프셋을 1:1로 유지 */
	struct d_partition *p;		/* 순회용 포인터; d_partitions[0] 또는 [1] 가리킴 */

	/*
	 * 섹터 0 읽기: NVMe namespace의 LBA 0에 대한 Read 명령을 간접 호출.
	 * 실패 시(-1)는 NVMe 명령 실패 또는 메모리 부족을 의미할 수 있음.
	 */
	data = read_part_sector(state, 0, &sect);
				/* read_part_sector() 난 -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) -> NVMe Read(0 LBA) */
	if (!data)
		return -1;	/* 섹터 버퍼 할당 실패 또는 NVMe Read 명령/CQE 오류 시 파티션 스캔 중단; 상위 block layer가 -1을 받으면 이 parser 실패로 처리 */

	/*
	 * NVMe DMA 버퍼를 disklabel 구조체로 해석한 뒤, magic number 확인.
	 * le16_to_cpu(): x86 등 리틀엔디언 CPU에서는 no-op이지만, 빅엔디언
	 * 아키텍처에서도 NVMe 리틀엔디언 필드를 정확히 읽기 위해 필요.
	 */
	label = (struct disklabel *)data;
				/* NVMe로부터 DMA로 받은 LBA 0 버퍼를 struct disklabel로 재해석; 시작 오프셋 0, d_reserved[270] 뒤에 d_partitions[2] 위치 */
	if (le16_to_cpu(label->d_magic) != KARMA_LABEL_MAGIC) {
				/* magic 불일치 = NVMe CQE는 성공했으나 미디어 내용이 Karma signature가 아님; checksum 대신 signature 기반 검증 */
		put_dev_sector(sect);	/* NVMe DMA 버퍼/섹터 객체 해제; CQE 완료 후 할당 해제 */
		return 0;		/* Karma 레이블 아님; block layer가 다음 parser(msdos/gpt)로 넘어감 */
	}

	p = label->d_partitions;
				/* d_partitions[0] 시작 = LBA 0 + 270B 오프셋; NVMe에서 읽은 메타데이터의 고정 위치 */
	for (i = 0 ; i < 2; i++, p++) {
				/* Karma는 최대 2개 primary partition만 기술; msdos의 extended partition 체인은 없어 추가 NVMe 명령 재귀 탐색 없음 (추정) */
		if (slot == state->limit)
			break;		/* 파티션 슬롯 최대치 도달; 더 이상 /dev/nvme0n1pN 등록 불가, NVMe namespace queue 재스캔 시에도 무시됨 */

		/*
		 * p_fstype == 0x4d이고 크기가 0이 아닌 항목만 block device
		 * 파티션으로 등록. p_offset/p_size는 NVMe LBA 좌표계의
		 * 시작과 길이를 나타낸다.
		 */
		if (p->p_fstype == 0x4d && le32_to_cpu(p->p_size)) {
				/* p_offset/p_size는 NVMe namespace LBA 좌표계의 섹터 단위 값; 별도 CHS 변환 없이 1:1 LBA 매핑 */
			put_partition(state, slot, le32_to_cpu(p->p_offset),
				le32_to_cpu(p->p_size));
				/* block layer가 /dev/nvme0n1p[slot] 형태의 하위 block device를 생성; 이후 bio->bi_sector += p_offset되어 NVMe SLBA로 전달 */
		}
		slot++;		/* 다음 파티션 슬롯 이동; NVMe namespace 위에 생성될 block device 번호 증가 */
	}
	seq_buf_puts(&state->pp_buf, "\n");	/* /proc/partitions 등에 출력될 문자열 완료; NVMe namespace 기반 파티션 목록 노출 */
	put_dev_sector(sect);			/* NVMe로부터 받은 섹터 버퍼 반환; DMA 버퍼 수명 종료 */
	return 1;
					/* 파티션 탐지 성공; state에 파티션 정보 기록됨; 이 NVMe namespace는 Karma 레이아웃으로 확정 */
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 NVMe namespace의 LBA 0을 읽어 파티션 테이블을 해석하는
 *   block layer parser이다. (read_part_sector -> submit_bio ->
 *     blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *     nvme_submit_cmd(doorbell))
 * - 파티션 경계(p_offset, p_size)는 이후 상위 IO 요청의 bio->bi_sector
 *   계산에 직접 반영되어 NVMe 명령의 SLBA로 변환된다.
 * - KARMA_LABEL_MAGIC(0xAB56) 불일치 시 NVMe namespace는 Karma 장치로
 *   인식되지 않으며, block layer가 msdos/gpt 등 다른 parser로 대체 시도한다.
 * - le16_to_cpu/le32_to_cpu는 NVMe 프로토콜의 리틀엔디언 필드를 호스트
 *   CPU 바이트오더로 변환하여 다양한 아키텍처에서 올바르게 해석하도록
 *   보장한다.
 * - put_partition()으로 등록된 슬롯 번호는 /dev/nvme0n1p1, p2 식의
 *   사용자 공간 노드 이름과 연결된다 (추정).
 */
