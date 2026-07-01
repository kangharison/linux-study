// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/osf.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 */

/*
 * 파일 상단 요약 (NVMe 관점)
 * ------------------------
 * 이 파일은 OSF/Tru64 디스크 레이블(DEC BSD 4.x 계열)을 파싱하여
 * 블록 장치 위의 파티션 정보를 등록한다.
 *
 * NVMe 스택에서의 위치:
 *   nvme_probe() -> nvme_alloc_ns() -> nvme_revalidate_disk() ->
 *   add_disk() -> rescan_partitions() -> check_partition() -> osf_partition()
 *
 * NVMe SSD는 CHS/헤드/섹터 같은 레거시 기하 정보 대신 LBA(논리 블록 주소)
 * 기반으로 동작하지만, 부팅 및 레거시 호환성을 위해 디스크의 첫 섹터(LBA 0)
 * 에 기록된 파티션 테이블 형식은 여전히 파싱되어야 한다.
 *
 * 이 파일은 block/partitions/check.c 의 check_partition 테이블에 의해
 * 호출되는 다수의 파티션 검출기 중 하나이며, msdos.c, gpt.c, sgi.c 등과
 * 나란히 위치한다.
 */

#include "check.h"		/* check.c 의 파티션 탐지기 테이블과 struct parsed_partitions 선언; rescan_partitions -> check_partition -> osf_partition 연결 지점 */

#define MAX_OSF_PARTITIONS 18	/* NVMe namespace 당 최대 OSF 파티션 항목 수; 실제 /dev/nvme0n1pX 생성 한계는 state->limit이 결정 */
#define DISKLABELMAGIC (0x82564557UL)	/* OSF/Tru64 디스크 레이블 매직; NVMe로부터 읽은 LBA 0 데이터와 비교되는 시그니처 */

/*
 * osf_partition() - OSF/Tru64 디스크 레이블을 탐지하고 파티션을 등록
 *
 * 목적:
 *   NVMe namespace 또는 기타 블록 장치의 LBA 0번 섹터를 읽어
 *   OSF 디스크 레이블 매직(DISKLABELMAGIC)이 존재하면 파티션 항목을
 *   parsed_partitions 구조체에 추가한다.
 *
 * NVMe 스택에서의 호출 경로 (추정):
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   (단, 이 함수 자체는 디스크 발견(discovery) 시점의 동기적 IO 경로이며
 *    위 경로는 add_disk/rescan_partitions 날짐에 의한 낮은 수준 IO를 의미)
 *
 *   사용자가 rescan_partitions를 요청했을 때의 상위 경로:
 *   add_disk() / device_add_disk() -> bdev_disk_changed() ->
 *   rescan_partitions() -> check_partition() -> osf_partition()
 *
 * 연결 지점:
 *   - read_part_sector(): check.c 의 helper, LBA 0을 읽어옴
 *   - put_partition():    발견된 각 파티션을 blkdev 영역에 등록
 *   - put_dev_sector():   read_part_sector로 얻은 sector 버퍼 해제
 */
int osf_partition(struct parsed_partitions *state)
{
	int i;				/* d_partitions[] 순회 인덱스; 한 항목씩 NVMe namespace LBA 영역을 blkdev에 등록 */
	int slot = 1;			/* /dev/nvme0n1p1 부터 파티션 번호 부여; state->limit을 초과하면 중단 */
	unsigned int npartitions;
	Sector sect;			/* read_part_sector가 반환한 임시 버퍼 (NVMe PRP/SGL DMA 완료 후 커널 매핑 영역) */
	unsigned char *data;		/* read_part_sector()로 얻은 LBA 0 데이터의 커널 가상 주소 */

	/*
	 * struct disklabel - OSF/Tru64 디스크 레이블
	 *
	 * 이 구조체는 CHS 기반 디스크들을 위해 설계되었으며, NVMe SSD에서는
	 * 기하/물리적 헤드/섹터 값이 실제 플래시 동작과 무관하다.
	 * NVMe namespace의 Identify Controller/Identify Namespace 대신
	 * 디스크의 첫 섹터에 저장된 레거시 메타데이터를 해석하는 구조.
	 */
	struct disklabel {
		__le32 d_magic;		/* LBA 0 + 0x00(레이블 기준): 디스크 레이블 매직; NVMe Read CQE 데이터의 첫 워드로 사용 */
		__le16 d_type,d_subtype;	/* +0x04: 레이블 타입/서브타입; NVMe IO 명령과 직접 무관 */
		u8 d_typename[16];	/* +0x08: 레이블 이름 문자열; NVMe namespace 식별명과는 별개 */
		u8 d_packname[16];	/* +0x18: 패키지 이름; 플래시 상 물리 패키지와 무관 (추정) */
		__le32 d_secsize;	/* +0x28: 레거시 섹터 크기(바이트); NVMe는 ns->lba_shift로 실제 LBA 크기 결정하므로 단순 참고값 */
		__le32 d_nsectors;	/* +0x2C: 트랙 당 섹터 수; NVMe 플래시에서는 CHS 에뮬레이션용 의미 없음 (추정) */
		__le32 d_ntracks;	/* +0x30: 헤드(트랙) 수; NVMe LBA 주소 공간과 무관 */
		__le32 d_ncylinders;	/* +0x34: 실린더 수; NVMe 컨트롤러가 인식하지 않음 */
		__le32 d_secpercyl;	/* +0x38: 실린더 당 섹터 수; CHS -> LBA 변환 시 사용되나 NVMe NS에서는 무시 (추정) */
		__le32 d_secprtunit;	/* +0x3C: 단위당 섹터 수; NVMe PRP/SGL 전송 단위와 직접 매핑되지 않음 */
		__le16 d_sparespertrack;	/* +0x40: 트랙 당 예비 섹터 수; NVMe 플래시의 낸드 예비 블록과는 별개 */
		__le16 d_sparespercyl;	/* +0x42: 실린더 당 예비 섹터 수; NVMe e2e protection/예비 영역과 무관 */
		__le32 d_acylinders;	/* +0x44: 대체 실린더 수; NVMe remap 영역과 직접 대응하지 않음 (추정) */
		__le16 d_rpm, d_interleave, d_trackskew, d_cylskew;	/* +0x48: 레거시 물리 디스크 파라미터; NVMe SSD에서는 더미값 */
		__le32 d_headswitch, d_trkseek, d_flags;	/* +0x50~0x58: 헤드 스위치/seek 시간 및 플래그; NVMe latency 경로와 무관 */
		__le32 d_drivedata[5];	/* +0x5C: 드라이브 고유 데이터; NVMe Identify Controller의 vendor specific 영역과는 별개 */
		__le32 d_spare[5];	/* +0x70: 예약 필드; NVMe namespace 메타데이터로 해석 불가 (추정) */
		__le32 d_magic2;	/* +0x84: d_magic과 동일한 보조 매직; NVMe 관점에서 LBA 0 데이터 무결성 재확인 */
		__le16 d_checksum;	/* +0x88: 레이블 체크섬; 본 코드에서는 검증하지 않으나 NVMe CQE 데이터가 E2E 보호 없을 때 중요 (추정) */
		__le16 d_npartitions;	/* +0x8A: 등록할 파티션 수; MAX_OSF_PARTITIONS(18) 초과 시 거부 */
		__le32 d_bbsize, d_sbsize;	/* +0x8C: 부트블록/슈퍼블록 크기; NVMe namespace의 블록 크기와는 별개 */

		/*
		 * struct d_partition - 개별 OSF 파티션 항목
		 *
		 * NVMe namespace의 LBA 공간에서 offset/size 쌍으로 해석되며,
		 * NVMe PRP/SGL에서 최종적으로 요청되는 시작 LBA와 길이의 기반이 된다.
		 */
		struct d_partition {
			__le32 p_size;		/* +0x00: 파티션 크기(섹터 단위); NVMe Read/Write 명령의 NLB(Number of Logical Blocks) 상한 */
			__le32 p_offset;	/* +0x04: 파티션 시작 LBA; NVMe 명령의 SLBA(Starting LBA) 산출 기준 */
			__le32 p_fsize;		/* +0x08: 파일시스템 조각 크기; NVMe LBA/블록 정렬 검사와 간접 관련 (추정) */
			u8  p_fstype;		/* +0x0C: 파일시스템 유형; NVMe IO 경로에서는 무시되고 VFS/mount 계층에서 사용 */
			u8  p_frag;		/* +0x0D: 조각 단위; NVMe PRP/SGL 정렬과 직접 매핑되지 않음 */
			__le16 p_cpg;		/* +0x0E: 실린더 당 그룹 수; NVMe LBA 배치와 무관 */
		} d_partitions[MAX_OSF_PARTITIONS];	/* +0x94: 최대 18개 파티션 항목 배열; 각 항목은 NVMe namespace LBA 범위 */
	} * label;
	struct d_partition * partition;

	/*
	 * LBA 0 읽기: state->bdev의 단일 섹터(보통 512B)를 NVMe로 요청.
	 * read_part_sector() 내부:
	 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
	 *   nvme_queue_rq -> nvme_setup_rw_sq -> PRP/SGL 설정 ->
	 *   nvme_submit_cmd(doorbell) -> CQE 완료 -> 데이터 매핑
	 */
	data = read_part_sector(state, 0, &sect);
	if (!data)
		/*
		 * DMA 매핑 실패, 범위 밖 LBA, 또는 NVMe CQE error 상태(예: SC=SGL/PRP 오류)
		 * 등으로 읽기에 실패하면 파티션 스캔을 중단하고 -1 반환.
		 */
		return -1;

	/*
	 * OSF 레이블은 LBA 0 시작이 아닌 offset 64 바이트 위치에 위치 (추정).
	 * 즉, NVMe로부터 읽은 512B 섹터 내 0x40 오프셋에서 disklabel이 시작.
	 */
	label = (struct disklabel *) (data+64);
	partition = label->d_partitions;	/* 첫 번째 d_partition 항목; 각 항목은 NVMe namespace LBA 영역 */

	/*
	 * 매직 넘버 불일치 시 NVMe namespace에 OSF 레이블이 없는 것으로 판단.
	 * 이는 NVMe CQE로 수신한 메타데이터가 예상 포맷이 아님을 의미하며,
	 * 다른 파티션 검출기(msdos.c, gpt.c 등)가 이어서 시도한다.
	 */
	if (le32_to_cpu(label->d_magic) != DISKLABELMAGIC) {
		put_dev_sector(sect);		/* PRP/SGL DMA 버퍼 해제; NVMe IO 리소스 정리 (추정) */
		return 0;
	}
	/*
	 * 보조 매직도 검사; 단일 매직 손상으로 인한 오판 방지.
	 * 두 매직이 모두 일치해야 NVMe LBA 0에 유효한 OSF 레이블이 존재한다고 판단.
	 */
	if (le32_to_cpu(label->d_magic2) != DISKLABELMAGIC) {
		put_dev_sector(sect);
		return 0;
	}

	/*
	 * 파티션 수가 구조체 한계를 초과하면 레이블을 신뢰하지 않음.
	 * NVMe CQE로 받은 메타데이터가 손상되었거나 잘못된 포맷일 가능성.
	 */
	npartitions = le16_to_cpu(label->d_npartitions);
	if (npartitions > MAX_OSF_PARTITIONS) {
		put_dev_sector(sect);
		return 0;
	}

	/*
	 * 각 d_partition 항목을 순회하며 put_partition()으로 등록.
	 * 등록된 파티션은 이후 submit_bio -> blk_mq_submit_bio 경로에서
	 * 요청된 LBA가 어느 nvme namespace 영역에 속하는지 파악하는 기준으로 사용.
	 */
	for (i = 0 ; i < npartitions; i++, partition++) {
		if (slot == state->limit)
		        break;			/* 커널이 허용하는 최대 파티션 슬롯 도달; 추가 /dev/nvme0n1pX 생성 불가 */
		if (le32_to_cpu(partition->p_size))
			/*
			 * p_size != 0인 항목만 등록.
			 * put_partition() -> add_partition() -> device_add() 순으로
			 * /dev/nvme0n1pX 블록 장치가 생성되며, 이 장치의 submit_bio는
			 * 최종적으로 nvme_ns_head 또는 nvme_ns의 request_queue로 연결.
			 */
			put_partition(state, slot,
				le32_to_cpu(partition->p_offset),	/* NVMe namespace 내 시작 LBA; 향후 nvme_setup_rw_sq의 SLBA로 변환 */
				le32_to_cpu(partition->p_size));	/* 파티션 크기(섹터); NVMe Read/Write NLB 상한 및 blkdev 크기 결정 */
		slot++;				/* 다음 파티션 슬롯 번호; /dev/nvme0n1p2, p3, ... 순 */
	}
	seq_buf_puts(&state->pp_buf, "\n");		/* /proc/partitions 스타일 버퍼에 개행; NVMe namespace 파티션 목록 문자열 완성 */
	put_dev_sector(sect);				/* LBA 0 버퍼 해제; NVMe PRP/SGL DMA unmap 완료 (추정) */
	return 1;
}

/*
 * NVMe 관점 핵심 요약
 * ------------------
 * - osf_partition()은 NVMe namespace가 처음 노출될 때 add_disk() ->
 *   rescan_partitions() -> check_partition() 경로로 한 번 호출된다.
 * - 이 코드는 NVMe Identify/PRP/SGL/CID/SQ/CQ와 직접 상호작용하지 않으며,
 *   LBA 0의 레거시 파티션 테이블을 해석하는 블록 계층 파서(parser)다.
 * - 파싱 결과인 p_offset/p_size는 이후 NVMe Read/Write 명령의
 *   SLBA(Starting LBA)와 NLB(Number of Logical Blocks) 산출의 기초가 된다.
 * - struct disklabel의 CHS/헤드/섹터 필드는 NVMe 플래시 메모리의
 *   실제 물리 구조와 무관하며, 레거시 메타데이터 호환용으로만 존재한다 (추정).
 * - block/partitions/check.c 의 파티션 검출기 테이블에 의해 호출되며,
 *   msdos.c, gpt.c, sgi.c 등과 동일한 수준의 파티션 검출 인터페이스를 따른다.
 */
