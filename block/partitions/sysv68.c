// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/sysv68.c
 *
 *  Copyright (C) 2007 Philippe De Muyter <phdm@macqel.be>
 *
 *  ===========================================================================
 *  NVMe 관점 파일 개요
 *  ---------------------------------------------------------------------------
 *  이 파일은 Motorola System V/68 포맷의 디스크 파티션 테이블을 해석한다.
 *  블록 레이어의 파티션 탐색 단계에서 동작하며, NVMe SSD의 논리 블록 주소
 *  (LBA) 공간을 슬라이스(slice) 단위로 나누는 메타데이터를 분석하는 역할을
 *  한다. 사용자가 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *  nvme_queue_rq -> nvme_submit_cmd(doorbell) 순으로 NVMe 명령을 생성하기
 *  전, 여기서 파티션 오프셋/크기 정보가 parsed_partitions 구조체에 채워진다.
 *  ===========================================================================
 */

#include "check.h"		/* check.c의 read_part_sector() / put_partition() 사용:
				 * rescan_partitions -> check_partition -> sysv68_partition
				 * -> read_part_sector -> submit_bio -> blk_mq_submit_bio
				 * -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd
				 * -> doorbell(CID, SQ tail doorbell) (추정) */

/*
 *	Volume ID structure: on first 256-bytes sector of disk
 *
 *	NVMe 관점: LBA 0에 위치한 볼륨 서명 블록으로, NVMe Identify Namespace에서
 *	얻은 lba_count 범위 내 첫 번째 섹터를 read_part_sector()로 읽어온다.
 *	vid_mac 필드가 "MOTOROLA" 문자열과 일치하는지 확인함으로써, 이 NVMe
 *	namespace가 sysV68 레이아웃을 사용하는지 판별한다.
 */

struct volumeid {
	u8	vid_unused[248];	/* LBA 0의 첫 248바이트는 사용하지 않음 (추정).
					 * NVMe Read 명령으로 512B 또는 4KB 논리 블록을
					 * 읽어오면 상위 248B는 무시되고, 하위
					 * 256B 내 vid_mac[8]만 서명 판별에 사용됨 */
	u8	vid_mac[8];	/* ASCII string "MOTOROLA", 볼륨 서명.
				 * NVMe CQE status = SUCCESS일 때만 신뢰 가능 */
};

/*
 *	config block: second 256-bytes sector on disk
 *
 *	NVMe 관점: LBA 1에 해당하는 설정 블록으로, 파티션 슬라이스 테이블의
 *	시작 블록 번호(ios_slcblk)와 항목 수(ios_slccnt)를 담고 있다. NVMe
 *	namespace의 논리 블록 크기가 512바이트 또는 4KB이더라도, 이 파일은
 *	고전적인 256바이트 섹터 번호를 그대로 LBA로 사용하는 것으로 보인다
 *	(추정).
 */

struct dkconfig {
	u8	ios_unused0[128];	/* 예약 영역, NVMe 동작과 무관함.
					 * offset 0~127: NVMe PRP/SGL에 매핑된
					 * 버퍼의 이 영역은 파싱하지 않음 */
	__be32	ios_slcblk;	/* Slice table block number, NVMe LBA로 해석됨.
				 * be32_to_cpu() 후 read_part_sector(state, i)의
				 * 두 번째 NVMe Read LBA 인자가 됨 */
	__be16	ios_slccnt;	/* Number of entries in slice table.
				 * be16_to_cpu() 후 슬라이스 루프 상한이 되며,
				 * 각 항목마다 별도의 put_partition() 등록이
				 * 발생 -> NVMe namespace 위 block device 수 증가 */
	u8	ios_unused1[122];	/* 예약 영역 */
};

/*
 *	combined volumeid and dkconfig block
 *
 *	NVMe 관점: read_part_sector(state, 0) 호출로 NVMe namespace의 LBA 0를
 *	한 번에 읽어 volumeid와 dkconfig를 동시에 파싱한다. 이렇게 함으로써
 *	doorbell을 통한 추가 NVMe 명령 왕복을 줄이고, 파티션 검출 지연(latency)을
 *	최소화할 수 있다.
 */

struct dkblk0 {
	struct volumeid dk_vid;	/* offset 0, LBA 0 첫 256B: "MOTOROLA" 서명 */
	struct dkconfig dk_ios;	/* offset 256, LBA 0 두 번째 256B: 슬라이스
				 * 테이블 위치/개수 메타데이터. NVMe namespace
				 * LBA 크기가 512B면 LBA 0 한 번의 Read로
				 * dkblk0 전체를 획득, 4KB면 LBA 0의 첫 512B만
				 * 사용하고 나머지 3.5KB는 무시됨 (추정) */
};

/*
 *	Slice Table Structure
 *
 *	NVMe 관점: 각 슬라이스는 NVMe namespace 내 연속적인 LBA 범위를 표현한다.
 *	nblocks는 해당 슬라이스의 논리 블록 개수, blkoff는 namespace 시작으로부터
 *	의 LBA 오프셋이다. 이 정보는 이후 put_partition()을 통해
 *	parsed_partitions->parts[] 배열로 전달되며, submit_bio ->
 *	blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq 경로에서 bio의
 *	시작 LBA에 파티션 기본 오프셋이 더해지는 데 사용된다.
 */

struct slice {
	__be32	nblocks;		/* slice size (in blocks).
					 * be32_to_cpu() 후 해당 파티션의 섹터 수가
					 * 되며, NVMe namespace capacity보다 큰 값이
					 * 들어오면 잘못된 LBA 범위로 이어질 수 있음 */
	__be32	blkoff;			/* block offset of slice.
					 * be32_to_cpu() 후 NVMe namespace 시작 LBA로부터
					 * 파티션의 시작 섹터 오프셋이 됨. NVMe Read/Write
					 * 명령의 SLBA 필드 계산 시 이 값이 기본
					 * 오프셋으로 더해짐 (추정) */
};


/*
 * sysv68_partition - Motorola System V/68 파티션 테이블 탐색
 * @state: blk layer의 파티션 파싱 상태를 담은 구조체
 *
 * 목적:
 *   NVMe namespace 상에서 sysV68 포맷의 슬라이스 테이블을 읽고,
 *   각 슬라이스를 커널 파티션 정보로 등록한다.
 *
 * 호출 경로 (NVMe 입출력 시 연결):
 *   rescan_partitions -> check_partition -> sysv68_partition
 *   이후 입출력 경로:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell, CID, SQ/CQ)
 *
 * NVMe 연결점:
 *   - read_part_sector() 낭부에서 NVMe namespace의 LBA를 읽는
 *     NVMe read 명령이 생성됨 (추정)
 *   - put_partition()으로 채워진 state->parts[] 정보가 이후 NVMe I/O
 *     경로에서 bio 시작 섹터를 변환하는 기준으로 사용됨
 *
 * 반환값:
 *   1  : sysV68 파티션 탐색 성공
 *   0  : sysV68 포맷이 아님
 *   -1 : 섹터 읽기 실패
 */
int sysv68_partition(struct parsed_partitions *state)
{
	int i, slices;			/* i: 임시 LBA/루프 인덱스, slices: 등록할
					 * 슬라이스 개수. 둘 다 NVMe namespace의
					 * LBA 범위를 다루므로 오버플로우 시 잘못된
					 * Read 명령을 유발할 수 있음 */
	int slot = 1;			/* 파티션 슬롯 번호, 1번부터 시작.
					 * slot 0은 전체 NVMe namespace를 나타내는
					 * placeholder로 예약됨 (추정) */
	Sector sect;			/* read_part_sector()가 반환한 Sector 버퍼.
					 * 낭부적으로는 NVMe Read의 DMA 버퍼를
					 * 감싸는 구조체 (추정) */
	unsigned char *data;		/* Sector 버퍼의 커널 가상 주소.
					 * NVMe PRP/SGL로부터 매핑된 메모리 영역을
					 * CPU가 해석하기 위한 포인터 */
	struct dkblk0 *b;		/* LBA 0 데이터를 dkblk0 구조체로 해석 */
	struct slice *slice;		/* 슬라이스 테이블 항목 배열의 포인터 */

	/* NVMe namespace의 LBA 0를 읽어 볼륨 ID와 설정 블록을 가져옴.
	 * 이 호출은 아래 경로를 따라 NVMe Read 명령으로 전환됨 (추정):
	 * read_part_sector -> submit_bio(BIO_OP_READ, sector=0)
	 * -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
	 * -> nvme_setup_rw_args -> nvme_submit_cmd(doorbell) -> CQE */
	data = read_part_sector(state, 0, &sect);
	if (!data)
		return -1;		/* NVMe read 명령 실패 또는 메모리 부족 (추정).
					 * CQE status != SUCCESS이거나, DMA 버퍼
					 * 할당(PCIe BAR/SGL 메모리) 실패 시 이
					 * 분기로 진입 -> 파티션 스캔 중단 */

	b = (struct dkblk0 *)data;	/* LBA 0 버퍼를 dkblk0 레이아웃으로 캐스팅.
					 * NVMe에서 읽은 원시 바이트 스트림을 구조체
					 * 필드 오프셋(0: vid, 256: config)에 맞춰
					 * 해석함 */
	/* "MOTOROLA" 서명 불일치 시 NVMe namespace는 sysV68 포맷이 아님 */
	if (memcmp(b->dk_vid.vid_mac, "MOTOROLA", sizeof(b->dk_vid.vid_mac))) {
		put_dev_sector(sect);	/* 서명 불일치로 인한 버퍼 해제:
					 * NVMe DMA 버퍼를 즉시 반납하여 메모리
					 * 및 PRP/SGL 자원을 회수함 */
		return 0;		/* 0 반환: check_partition()이 다음
					 * 파티션 포맷 탐색기로 넘어감. NVMe 명령
					 * 완료(CQE)는 정상이나 포맷 미일치 */
	}
	/* 빅엔디안 필드를 CPU 바이트오더로 변환 */
	slices = be16_to_cpu(b->dk_ios.ios_slccnt);
					/* ios_slccnt: 슬라이스 테이블 총 항목 수.
					 * 이 값이 클수록 for 루프에서 검사할 항목과
					 * 잠재적 put_partition() 등록 횟수, 따라서
					 * NVMe namespace 위 block device 개수가
					 * 증가함 */
	i = be32_to_cpu(b->dk_ios.ios_slcblk);
					/* ios_slcblk: 슬라이스 테이블이 위치한
					 * NVMe LBA. be32_to_cpu() 후 read_part_sector()
					 * 의 sector 인자로 직접 사용 -> 두 번째
					 * NVMe Read 명령의 시작 SLBA가 됨 */
	put_dev_sector(sect);		/* LBA 0용 Sector 버퍼 해제.
					 * 첫 번째 NVMe Read의 DMA 버퍼를 반납하여
					 * PRP/SGL 자원을 회수하고, 다음 Read 전에
					 * 메모리 압박을 줄임 */

	/* ios_slcblk가 가리키는 LBA에서 슬라이스 테이블을 읽어옴 */
	data = read_part_sector(state, i, &sect);
					/* 두 번째 NVMe Read: 슬라이스 테이블 LBA.
					 * i가 0이면 LBA 0를 다시 읽게 되어 중복
					 * NVMe 명령 발생. i가 namespace capacity를
					 * 벗어나면 CQE error(예: LBA Out of Range)
					 * 가능성 있음 (추정) */
	if (!data)
		return -1;		/* NVMe read 명령 실패 (추정).
					 * 두 번째 Read의 CQE status가 실패이거나
					 * DMA 버퍼 할당 실패 시 파티션 스캔 중단 */

	slices -= 1; /* last slice is the whole disk */
					/* 마지막 슬라이스는 전체 NVMe namespace를
					 * 나타낸다고 가정하여 제외. 이로써 등록되는
					 * block device 수를 1 감소시키고, 잘못된
					 * 전체 디스크 중복 등록을 방지함 (추정) */
	/* /proc/partitions 등에 출력될 포맷 문자열 생성 */
	seq_buf_printf(&state->pp_buf, "sysV68: %s(s%u)", state->name, slices);
	slice = (struct slice *)data;	/* 슬라이스 테이블 버퍼를 struct slice
					 * 배열로 해석. NVMe에서 읽은 연속된
					 * 메모리를 {nblocks, blkoff} 쌍의 배열로
					 * 매핑: offset 0~3=nblocks, 4~7=blkoff */
	/* 각 슬라이스를 커널 파티션으로 등록 */
	for (i = 0; i < slices; i++, slice++) {
					/* 슬라이스 항목별 반복. 각 항목은 NVMe
					 * namespace 내 하나의 LBA 구간을 정의하며,
					 * 비어 있지 않으면 별도의 block device가
					 * 생성됨 -> NVMe I/O 큐/namespace 참조 증가 */
		if (slot == state->limit)
			break;		/* 커널 파티션 슬롯 상한 도달.
					 * 더 이상 block device를 등록할 수 없으므로
					 * 추가 NVMe Read나 put_partition() 없이
					 * 루프 탈출. state->limit은 디스크당 최대
					 * 파티션 수로, NVMe namespace당 큐 용량과
					 * 관련될 수 있음 (추정) */
		if (be32_to_cpu(slice->nblocks)) {
					/* nblocks != 0인 항목만 실제 파티션으로
					 * 취급. 0이면 할당되지 않은 슬라이스로
					 * 보고 NVMe block device 등록을 건 넘김 */
			/* 비어 있지 않은 슬라이스만 state->parts[slot]에 기록 */
			put_partition(state, slot,
				be32_to_cpu(slice->blkoff),
				be32_to_cpu(slice->nblocks));
					/* 파티션 등록: (slot, start, size)를
					 * parsed_partitions에 기록. 이후
					 * submit_bio -> blk_mq_submit_bio에서
					 * bio의 시작 섹터에 blkoff가 더해져
					 * NVMe Read/Write의 SLBA로 사용됨 (추정) */
			seq_buf_printf(&state->pp_buf, "(s%u)", i);
					/* /proc/partitions 출력용 마커. 등록된
					 * 슬라이스마다 (sN) 형태로 추가되어 NVMe
					 * namespace 위에서 몇 개의 block device가
					 * 활성화되었는지 표시 */
		}
		slot++;			/* 다음 파티션 슬롯으로 이동.
					 * slot 번호가 곧 block device의 마이너
					 * 번호/파티션 인덱스가 되어 NVMe namespace
					 * 위에서 파티션을 구분함 (추정) */
	}
	seq_buf_puts(&state->pp_buf, "\n");
	put_dev_sector(sect);		/* 슬라이스 테이블 Sector 버퍼 해제.
					 * 두 번째 NVMe Read의 DMA 버퍼를 반납.
					 * 파티션 메타데이터는 이미 state->parts[]에
					 * 복사되었으므로 버퍼는 해제 가능 */
	return 1;			/* sysV68 파티션 탐색 성공.
					 * 이후 블록 레이어가 등록된 파티션들을
					 * NVMe namespace 위의 독립 block device로
					 * 노출하고, I/O 요청 시 앞서 설명한
					 * submit_bio -> ... -> nvme_submit_cmd 경로를
					 * 통해 NVMe 명령을 발행함 */
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 NVMe namespace의 LBA 공간을 sysV68 슬라이스 단위로 분할하는
 *   메타데이터를 파싱하며, rescan_partitions -> check_partition 경로에서
 *   실행된다.
 * - read_part_sector()를 통해 NVMe namespace의 LBA 0와 슬라이스 테이블을
 *   읽어오며, 이는 NVMe read 명령의 생성으로 이어진다 (추정).
 * - put_partition()으로 등록된 오프셋/크기는 이후 submit_bio ->
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell, CID) 과정에서 bio LBA 변환의 기준으로 쓰인다.
 * - 파일 상단의 #include "check.h"는 blk/partitions/check.c에 정의된 공용
 *   파티션 검사 유틸리티를 사용함을 의미하며, read_part_sector()와
 *   put_partition() 등이 그곳에 있다.
 * - NVMe PRP/SGL 주소 변환은 이 파일에서 직접 다루지 않고, 상위 블록 레이어
 *   및 nvme_queue_rq에서 처리된다.
 * - 본 포맷은 DOS/MBR의 연장 파티션(extended partition) 체인이 없는 평면
 *   슬라이스 테이블 구조이므로, 재귀적인 partition scan으로 인한 추가
 *   NVMe 명령 발행은 발생하지 않는다.
 */
