// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/ultrix.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Re-organised Jul 1999 Russell King
 *
 *  ===========================================================================
 *  NVMe SSD 관점 파일 요약
 *  ===========================================================================
 *  이 파일은 DEC Ultrix 디스크 레이블(파티션 테이블)을 파싱하는 블록 계층의
 *  파티션 검출기이다. NVMe 입출력이 실제로 흐르는 hot path는 아니지만,
 *  NVMe namespace(nvme0n1 등)가 커널에 등록될 때 gendisk 초기화 단계에서
 *  add_disk -> bdev_disk_changed -> read_partitions -> [ultrix_partition]
 *  경로로 호출되어 namespace 전체 영역을 어떻게 여러 파티션으로 나눌지 결정한다.
 *  파싱 결과는 struct parsed_partitions에 저장되고, 이후 blk_mq(blk-mq)가
 *  submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *  nvme_submit_cmd(doorbell) 경로로 NVMe I/O를 전달할 때 파티션 오프셋/크기를
 *  기반으로 bio를 올바른 LBA로 재매핑하는 데 사용된다.
 *  ===========================================================================
 */

#include "check.h"

/*
 * ultrix_partition - Ultrix 파티션 테이블을 디스크에서 읽어 등록한다.
 *
 * 목적:
 *   NVMe namespace 혹은 기타 블록 장치의 특정 섹터에 기록된 Ultrix 디스크
 *   레이블을 검사하고, 유효하면 8개까지의 파티션 엔트리를 blk_mq와 NVMe
 *   드라이버가 사용할 수 있도록 block layer에 등록한다.
 *
 * 호출 경로 (추정):
 *   add_disk -> bdev_disk_changed -> read_partitions -> check_partition ->
 *   ultrix_partition
 *
 * NVMe 연결 지점:
 *   - read_part_sector() 내부에서 bdev/bio 기반 읽기가 발생하고, bio는
 *     submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *     nvme_submit_cmd(doorbell) 경로를 따라 NVMe Admin/IO CQ/SQ 및 doorbell로
 *     변환된다.
 *   - put_partition() 결과가 저장된 state는 이후 NVMe I/O가 파티션 단위로
 *     submit될 때 start_sect, nr_sects 등을 통해 LBA 변환에 사용된다.
 */
int ultrix_partition(struct parsed_partitions *state)
{
	int i;				/* 파티션 엔트리 8개 순회용 인덱스; 각 i마다 NVMe namespace LBA 공간 내 한 파티션 슬롯 검사 */
	Sector sect;			/* read_part_sector()가 내부적으로 bio 읽기 완료 후 반환하는 섹터 버퍼 핸들; NVMe CQE 완료 시점까지 참조 유지(추정) */
	unsigned char *data;		/* NVMe Read 명령으로 namespace에서 읽은 원시 LBA 데이터의 커널 가상 주소; 이후 struct ultrix_disklabel 캐스팅 대상 */

	/*
	 * struct ultrix_disklabel - Ultrix 형식의 파티션 레이블 구조체.
	 *
	 * 이 구조체는 디스크(또는 NVMe namespace)의 특정 고정 위치에 기록된
	 * 메타데이터를 표현한다. NVMe namespace는 기본적으로 LBA 0부터 연속적인
	 * 섹터 공간이므로, 이 레이블은 namespace 내 특정 LBA에서 읽힌다.
	 *
	 * 필드별 NVMe 행위 연관성:
	 *   pt_magic   - 레이블 식별 매직 넘버. NVMe Admin Identify/Read
	 *                명령으로 읽은 원시 LBA 데이터에서 이 값이 있어야
	 *                파티션 정보로 인식된다.
	 *   pt_valid   - 드라이버가 "현재 유효함"으로 설정하는 플래그. NVMe
	 *                입장에서는 namespace 콘텐츠의 일부이며, 1일 때만
	 *                뒤이은 파티션 등록을 수행한다.
	 *   pt_part[]  - 8개 파티션 슬롯. pi_blkoff는 NVMe LBA 공간에서의
	 *                시작 섹터이고, pi_nblocks는 해당 파티션 길이(섹터
	 *                수)이다. blk_mq가 bio를 처리할 때 파티션의
	 *                start_sect를 더해 실제 NVMe CID/PRP/SGL 요청 LBA를
	 *                계산하게 된다.
	 */
	struct ultrix_disklabel {
		s32	pt_magic;	/* magic no. indicating part. info exits */
					/* NVMe namespace LBA 데이터 오프셋 0x0에 해당(추정); 매직 매칭 실패 시 NVMe CQE 상태와 무관하게 파티션 등록 불가 */
		s32	pt_valid;	/* set by driver if pt is current */
					/* 오프셋 0x4(추정); 1일 때만 NVMe namespace LBA를 파티션 단위로 분할하여 block device 등록 진행 */
		struct  pt_info {
			s32		pi_nblocks; /* no. of sectors */
						/* 파티션 크기(섹터 수); blk_mq가 NVMe Read/Write 명령의 NLB(Number of Logical Blocks) 산출 시 사용하는 범위 제한값 */
			u32		pi_blkoff;  /* block offset for start */
						/* 파티션 시작 LBA; submit_bio -> nvme_queue_rq 경로에서 파티션 start_sect로 더해져 NVMe CID의 SLBA 필드 계산에 기여 */
		} pt_part[8];			/* 8개 파티션 슬롯; 각 슬롯은 NVMe namespace 연속 LBA 영역 하나를 기술하며, bio가 흐를 때마다 해당 슬롯의 오프셋/크기로 LBA remap 수행 */
	} *label;				/* NVMe에서 읽은 원시 data를 Ultrix 레이블 구조체로 해석할 포인터; 필드 오프셋은 디스크 바이트 순서 그대로 적용 */

#define PT_MAGIC	0x032957	/* Partition magic number */
					/* Ultrix 레이블 식별용 매직; NVMe CQE로 반환된 버퍼에서 이 값이 정확히 위치해야 이 namespace가 Ultrix 포맷으로 분류됨 */
#define PT_VALID	1		/* Indicates if struct is valid */
					/* 레이블 유효 플래그; 매직과 함께 일치해야 NVMe namespace 위에 block partition device 등록 허용 */

	/*
	 * Ultrix 레이블은 디스크 끝에서 16384바이트 지점 근처에 위치한다.
	 * (16384 - sizeof(*label))/512 로 sector 번호를 계산하고,
	 * read_part_sector()를 통해 해당 섹터를 읽는다.
	 *
	 * NVMe 경로 (추정):
	 *   read_part_sector -> bdev_read_part -> submit_bio ->
	 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
	 *   nvme_submit_cmd(doorbell)
	 */
	data = read_part_sector(state, (16384 - sizeof(*label))/512, &sect);
					/* (16384 - sizeof(*label))/512: Ultrix 레이블이 저장된 NVMe namespace LBA; 512바이트 섹터 단위로 정렬되어 NVMe Read SLBA 계산 */
	if (!data)
		return -1;	/* 섹터 읽기 또는 메모리 할당 실패 시 파티션 검출 중단 */
				/* read_part_sector 내부 bio 할당/제출 실패 또는 NVMe 명령 CQE 오류 시 파티션 스캔 abort */

	/*
	 * 읽어온 섹터 끝 부분에서 Ultrix 레이블 크기만큼 뒤로 이동해
	 * struct ultrix_disklabel 포인터를 얻는다. NVMe namespace에서
	 * 읽은 원시 LBA 버퍼 안에서 오프셋을 조정하는 것이다.
	 */
	label = (struct ultrix_disklabel *)(data + 512 - sizeof(*label));
					/* data + 512 - sizeof(*label): 512바이트 섹터 내 레이블 끝 정렬 위치; NVMe Read로 가져온 단일 LBA 버퍼 내 오프셋 조정 */

	/*
	 * 매직 넘버와 valid 플래그가 모두 일치해야 파티션 정보로 인정한다.
	 * 일치하지 않으면 단순히 원시 namespace의 데이터일 뿐이며,
	 * 파티션으로 등록하지 않는다.
	 */
	if (label->pt_magic == PT_MAGIC && label->pt_valid == PT_VALID) {
					/* 매직/valid 일치: NVMe namespace 상 Ultrix 메타데이터 확인; CQE 성공 이후에도 소프트웨어 식별 단계 */
		for (i=0; i<8; i++)	/* 8개 파티션 슬롯 순회; 각 i마다 NVMe namespace 내 하나의 LBA 영역이 block partition으로 등록될 후보 */
			/*
			 * pi_nblocks가 0이면 미사용 파티션 슬롯이다.
			 * 0이 아닌 슬롯만 block layer의 파티션 테이블에 등록한다.
			 * 이 정보는 이후 submit_bio -> blk_mq_submit_bio ->
			 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd
			 * 에서 bio의 bdev/partno를 실제 NVMe LBA로 변환할 때
			 * 사용된다.
			 */
			if (label->pt_part[i].pi_nblocks)
					/* pi_nblocks != 0인 슬롯만 유효 파티션; NVMe I/O는 이 범위를 벗어나지 않도록 blk_mq에서 제한 */
				put_partition(state, i+1, 
					      label->pt_part[i].pi_blkoff,
					      label->pt_part[i].pi_nblocks);
					/* state에 파티션 i+1 등록: 파티션 번호 -> NVMe namespace LBA offset(pi_blkoff) + 길이(pi_nblocks) 매핑 생성 */
		put_dev_sector(sect);	/* read_part_sector로 얻은 섹터 버퍼 해제; NVMe Read에 사용된 bio/페이지 참조 카운트 정리(추정) */
		seq_buf_puts(&state->pp_buf, "\n");
					/* /proc/partitions 등에 출력될 문자열 버퍼에 개행 추가; NVMe namespace 파티션 정보 노출 포맷 구성 */
		return 1;	/* Ultrix 파티션 레이블 발견 (0~8개 파티션 등록) */
				/* 1 반환: read_partitions에 Ultrix 파티션 존재 보고, 이후 다른 파티션 검출기는 일반적으로 건너뜀(추정) */
	} else {
		put_dev_sector(sect);	/* 매직/valid 불일치 시에도 읽어온 섹터 버퍼 해제; NVMe Read 완료 후 리소스 정리 */
		return 0;	/* Ultrix 파티션 레이블이 아님 */
				/* 0 반환: 이 NVMe namespace는 Ultrix 레이블이 아님을 read_partitions에 알림; 다음 파티션 검출기로 폴백 */
	}
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 NVMe I/O hot path가 아니라 namespace 등록 시점의 파티션
 *   검출 코드이며, read_partitions -> check_partition -> ultrix_partition
 *   경로(추정)로 호출된다.
 *
 * - read_part_sector()에서 발생하는 읽기 bio는 submit_bio ->
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell)을 거쳐 NVMe Admin/IO SQ, CQ, CID, PRP/SGL
 *   형태의 명령으로 변환될 수 있다.
 *
 * - put_partition()으로 기록된 pi_blkoff/pi_nblocks는 이후 blk_mq가
 *   파티션 단위 bio를 처리할 때 NVMe namespace LBA로의 오프셋 변환에
 *   사용된다.
 *
 * - struct ultrix_disklabel의 pt_magic/pt_valid는 디스크(또는 NVMe
 *   namespace) 원시 데이터 중 Ultrix 메타데이터를 식별하는 표식일 뿐,
 *   NVMe 컨트롤러/도어벨 동작과는 직접 관련이 없다.
 *
 * - 이 파일은 block/partitions/msdos.c, block/partitions/efi.c 등 다른
 *   파티션 검출기와 함께 read_partitions()에 의해 순차적으로 호출될 수
 *   있으며, NVMe namespace가 어떤 파티션 체계를 사용하는지에 따라
 *   해당 parser만이 파티션을 등록하게 된다.
 */
