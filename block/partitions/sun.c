// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/sun.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 *
 *  ===================================================================
 *  NVMe 관점 파일 상단 요약
 *  ===================================================================
 *  본 파일은 SPARC/SUN 마이크로시스템즈의 디스크 레이블(Sun disklabel)을
 *  파싱하여 커널의 generic block layer가 인식할 수 있는 partition 테이블로
 *  변환하는 역할을 수행합니다. NVMe SSD 관점에서 볼 때, 이 파일은
 *  사용자가 submit_bio를 통해 본격적인 I/O를 시작하기 전에, NVMe namespace
 *  (nvme_ns)에 매핑된 block device (gendisk)의 경계를 분할(partition) 단위로
 *  초기화하는 선행 단계입니다. 파티션 정보는 blkpg/partition scan 경로를
 *  거쳐 gendisk에 등록되며, 이후 bio -> blk_mq_submit_bio ->
 *  blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로
 *  이어지는 NVMe SQ/CQ 제출 경로에서 LBA 범위 검증의 기준으로 사용됩니다.
 *  (참고: 본 파일의 파싱 흐름은 block/partitions/msdos.c,
 *   block/partitions/efi.c 등과 동일한 check.c 상위 프레임워크 아래에
 *   위치하며, partition table 타입만 다를 뿐 NVMe namespace 초기화 단계에서
 *   수행하는 기능은 유사합니다.)
 *  ===================================================================
 */

#include "check.h"	// partition scan 프레임워크 헤더; NVMe namespace 초기화 시 check_partition()을 통해 이 파서를 호출

#define SUN_LABEL_MAGIC          0xDABE	// NVMe namespace LBA 0에서 Sun disklabel임을 식별하는 매직값 (추정: 컨트롤러 DMA 완료 후 CPU가 메모리 매핑된 버퍼에서 이 값을 검사)
#define SUN_VTOC_SANITY          0x600DDEEE	// VTOC 블록의 sanity 마법값; NVMe Read로 읽은 메타데이터가 Sun VTOC 레이아웃인지 상위에서 검증

enum {
	SUN_WHOLE_DISK = 5,	// 전체 NVMe namespace를 나타내는 파티션 id; whole-disk 플래그로 매핑
	LINUX_RAID_PARTITION = 0xfd,	/* autodetect RAID partition */	// Linux RAID 자동검출 파티션 id; NVMe 명령에는 영향 없고 상위 md 계층에서 활용
};

/*
 * sun_partition - Sun disklabel 기반 파티션 테이블 파싱
 *
 * 목적:
 *   NVMe namespace에 연결된 block device의 첫 번째 섹터(sector 0)를 읽어
 *   Sun disklabel/VTOC 구조체를 해석하고, partition 엔트리를 gendisk에
 *   등록합니다. 이 함수는 partition scan 단계에서 호출되며, 사용자 I/O가
 *   NVMe SQ/CQ를 통해 전달되기 전에 LBA 범위를 분할하는 사전 작업입니다.
 *
 * 호출 경로 (NVMe 연결점):
 *   nvme_scan_ns -> nvme_alloc_ns -> add_disk -> device_add_disk ->
 *   bdev_disk_changed -> blkpg_do_ioctl/scanf_partitions ->
 *   check_partition -> sun_partition
 *
 *   이후 bio 제출 경로:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell), CID 할당, PRP/SGL 구성
 *
 * 반환값:
 *   1  - 파티션 발견 및 등록 성공
 *   0  - Sun disklabel 아님 또는 체크섬/무결성 실패
 *   -1 - 첫 섹터 읽기 실패
 */
int sun_partition(struct parsed_partitions *state)
{
	int i;	// partition 엔트리 순회 인덱스; 각 i는 NVMe namespace의 서브 LBA 범위에 대응
	__be16 csum;	// 레이블 XOR 체크섬 누산기; NVMe DMA 읽기 후 CPU endian 변환 없이 빅엔디안 워드 단위로 XOR
	int slot = 1;	// gendisk->part[]에 등록할 minor 슬롯; slot 0은 전체 NVMe namespace이므로 1부터 시작
	__be16 *ush;	// 체크섬 계산용 16비트 빅엔디안 포인터; 레이블 끝(offset)부터 역순으로 스캔
	Sector sect;	// read_part_sector가 NVMe Read 완료 후 반환하는 페이지 크기 버퍼 핸들; LBA 0 DMA 결과 보관
	/*
	 * struct sun_disklabel - Sun 디스크 레이블 전체 구조체
	 *
	 * NVMe와의 연결:
	 *   이 구조체는 NVMe namespace의 LBA 0에 기록된 레이블입니다.
	 *   NVMe 컨트롤러는 낮은 수준에서 PRP/SGL로 LBA 0을 DMA로 읽어오며,
	 *   본 구조체는 그 바이트 배열을 CPU endian으로 해석하는 템플릿입니다.
	 */
	struct sun_disklabel {
		/*
		 * info[128]: 사람이 읽을 수 있는 텍스트 문자열
		 * NVMe 관점: namespace에 대한 식별용 주석이며, CID나 doorbell
		 * 제출과는 무관합니다.
		 */
		unsigned char info[128];	/* Informative text string */
		/*
		 * vtoc: Volume Table of Contents
		 * NVMe 관점: namespace의 논리적 볼륨 정보입니다. 파티션 수(nparts)
		 * 및 각 파티션 타입(id)을 결정하며, 이 정보는 이후 bio를
		 * 특정 partition minor에 매핑할 때 사용됩니다.
		 */
		struct sun_vtoc {
		    /*
		     * version: VTOC 레이아웃 버전
		     * NVMe 관점: 레이블 해석 규칙의 버전이며, namespace 포맷과
		     * 직접 매핑되지는 않습니다.
		     */
		    __be32 version;	/* Layout version */
		    /*
		     * volume[8]: 볼륨 이름
		     * NVMe 관점: gendisk의 disk_name과는 별개의 Sun 고유 이름입니다.
		     */
		    char   volume[8];	/* Volume name */
		    /*
		     * nparts: 파티션 개수 (최대 8)
		     * NVMe 관점: namespace가 나뉘는 논리 단위 수를 결정하며,
		     * gendisk의 partition 배열 크기와 직접 관련이 있습니다.
		     */
		    __be16 nparts;	/* Number of partitions */
		    /*
		     * infos[8]: 각 파티션의 id/flags
		     * NVMe 관점: RAID 전체 디스크 여부 등이 결정되며,
		     * 이 플래그는 bio를 NVMe queue_rq로 별낼 때 추가
		     * 메타데이터(ADDPART_FLAG_*)로 전달됩니다.
		     */
		    struct sun_info {		/* Partition hdrs, sec 2 */
			__be16 id;
			__be16 flags;
		    } infos[8];
		    /*
		     * padding: 구조체 정렬용 패딩
		     * NVMe 관점: DMA 읽기 시 메모리 정렬과 무관한 on-disk
		     * 레이아웃 일부입니다.
		     */
		    __be16 padding;	/* Alignment padding */
		    /*
		     * bootinfo[3]: mboot 부팅 정보
		     * NVMe 관점: OS 로더가 NVMe namespace로부터 부팅에 필요한
		     * 추가 LBA/메타데이터를 기술합니다.
		     */
		    __be32 bootinfo[3];	/* Info needed by mboot */
		    /*
		     * sanity: VTOC 무결성 검증용 마법값
		     * NVMe 관점: namespace에 쓰인 데이터의 일관성을 상위에서
		     * 간접 검증하며, NVMe 자체의 end-to-end 데이터 보호는 아닙니다.
		     */
		    __be32 sanity;	/* To verify vtoc sanity */
		    /*
		     * reserved[10]: 향후 확보 영역
		     */
		    __be32 reserved[10];	/* Free space */
		    /*
		     * timestamp[8]: 파티션별 타임스탬프
		     * NVMe 관점: 파티션 메타데이터의 생성/변경 시각이며, NVMe
		     * 명령 제출과는 무관합니다.
		     */
		    __be32 timestamp[8];	/* Partition timestamp */
		} vtoc;
		/*
		 * write_reinstruct / read_reinstruct:
		 * 쓰기/읽기 시 스킵할 섹터 수 (오래된 디스크 특성)
		 * NVMe 관점: NVMe SSD에는 불필요한 회전 지연/물리적 헤드
		 * 이동 개념이 없으므로, 이 값은 0으로 무시되는 경우가 많습니다.
		 */
		__be32 write_reinstruct;	/* sectors to skip, writes */
		__be32 read_reinstruct;	/* sectors to skip, reads */
		unsigned char spare[148];	/* Padding */	// Sun 레이블 내 예약 영역; NVMe Read로 읽은 512B 레이블 중 사용되지 않는 on-disk 바이트(offset 280~427)
		/*
		 * rspeed: 디스크 회전 속도
		 * NVMe 관점: NAND flash 기반 NVMe에서는 의미 없는 레거시 필드
		 */
		__be16 rspeed;	/* Disk rotational speed */
		/*
		 * pcylcount, sparecyl, ncyl, nacyl, ntrks, nsect:
		 * CHS(실린더/헤드/섹터) 형태의 기하학적 정보
		 * NVMe 관점: NVMe는 LBA 기반이므로 CHS 값은 레이블 호환성을
		 * 위해서만 사용되며, 실제 NVMe Read/Write 명령에는 LBA를
		 * 직접 사용합니다.
		 */
		__be16 pcylcount;	/* Physical cylinder count */
		__be16 sparecyl;	/* extra sects per cylinder */
		__be16 obs1;		/* gap1 */
		__be16 obs2;		/* gap2 */
		__be16 ilfact;		/* Interleave factor */
		__be16 ncyl;		/* Data cylinder count */
		__be16 nacyl;		/* Alt. cylinder count */
		__be16 ntrks;		/* Tracks per cylinder */
		__be16 nsect;		/* Sectors per track */
		__be16 obs3;		/* bhead - Label head offset */
		__be16 obs4;		/* ppart - Physical Partition */
		/*
		 * partitions[8]: 실제 파티션 엔트리 배열
		 *
		 * NVMe 관점: 각 엔트리의 start_cylinder*spc 와 num_sectors가
		 * NVMe namespace 상의 시작 LBA와 섹터 수로 변환되어,
		 * gendisk->part[]에 등록됩니다. 이후 bio가 partition minor를
		 * 통해 들어오면, bio.bi_iter.bi_sector가 해당 파티션의
		 * 시작 LBA에 상대적으로 오프셋되어 NVMe PRP/SGL에 전달됩니다.
		 */
		struct sun_partition {
			__be32 start_cylinder;
			__be32 num_sectors;
		} partitions[8];
		/*
		 * magic: Sun 레이블 식별 마법값 (SUN_LABEL_MAGIC = 0xDABE)
		 * NVMe 관점: namespace LBA 0의 콘텐츠가 Sun disklabel인지
		 * 판별하는 첫 번째 힌트입니다.
		 */
		__be16 magic;		/* Magic number */	// offset 508 (0x1FC): LBA 0 마지막 4B 중 첫 2B; Sun 포맷 식별
		/*
		 * csum: 레이블 전체에 대한 XOR 체크섬
		 * NVMe 관점: NVMe 자체 데이터 무결성(DIF/DIX)과는 별개이며,
		 * Sun 레이블 수준의 단순 무결성 검증입니다.
		 */
		__be16 csum;		/* Label xor'd checksum */	// offset 510 (0x1FE): 512B 레이블 마지막 2B; 레이블 전체 XOR 결과가 이 위치에 기록
	} * label;
	/*
	 * p: 현재 파싱 중인 sun_partition 엔트리를 가리킵니다.
	 * NVMe 관점: 각 엔트리는 NVMe namespace의 서브 영역(sub-range)에
	 * 해당합니다.
	 */
	struct sun_partition *p;
	/*
	 * spc: 섹터 per 실린더 (ntrks * nsect)
	 * NVMe 관점: CHS를 LBA로 변환하기 위한 보조 변수입니다.
	 */
	unsigned long spc;
	int use_vtoc;	// VTOC 메타데이터 사용 가능 여부; nparts 및 파티션 id/flags 해석 스위치
	int nparts;	// 실제 스캔할 파티션 엔트리 수; NVMe namespace 분할 granularity 결정

	/*
	 * LBA 0을 읽어 Sun disklabel을 메모리로 가져옵니다.
	 * NVMe 연결: 실제 read_part_sector 낮은 수준에서는
	 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
	 * nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로를 통해
	 * NVMe 컨트롤러가 CID를 할당하고, PRP/SGL을 구성하여
	 * namespace로부터 LBA 0을 DMA로 읽어옵니다.
	 */
	label = read_part_sector(state, 0, &sect);	// sector 0 요청 -> submit_bio -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_submit_cmd (SLBA=0, 1 sector) (추정)
	if (!label)	// read_part_sector 낸부 메모리 할당 실패 또는 submit_bio/wait_for_completion 오류 시 NULL; NVMe CQE 오류/타임아웃 가능성
		return -1;	// 첫 섹터 읽기 실패; 상위 check_partition은 이 namespace를 파티션 없이 처리하거나 다음 파서로 진행

	p = label->partitions;	// LBA 0에 DMA로 읽어온 메모리에서 partition 엔트리 배열의 첫 번째 항목 주소 획득
	/*
	 * magic 필드가 Sun 레이블이 맞는지 확인합니다.
	 * NVMe 관점: namespace의 LBA 0에 Sun 포맷이 기록되어 있지 않으면
	 * 파티션 등록을 중단하고 다음 파서(예: efi, msdos)로 넘어갑니다.
	 */
	if (be16_to_cpu(label->magic) != SUN_LABEL_MAGIC) {	// LBA 0 offset 508의 매직 불일치; NVMe namespace가 Sun 포맷이 아님
		put_dev_sector(sect);	// 매직값 불일치 시 NVMe Read로 할당된 버퍼 해제 -> 다음 파서(msdos/efi)가 동일 namespace를 재탐색 가능
		return 0;
	}
	/* Look at the checksum */
	/*
	 * 레이블 끝부터 시작하여 16비트 단위로 XOR하여 체크섬을 검증합니다.
	 * NVMe 관점: namespace에서 읽은 메타데이터가 손상되지 않았는지
	 * 상위에서 검증하며, 체크섬이 틀리면 파티션 정보를 신뢰하지 않습니다.
	 */
	ush = ((__be16 *) (label+1)) - 1;	// struct sun_disklabel 바로 다음(512B 끝)을 16비트 단위로 가리킴; Sun 레이블 전체를 워드 단위 XOR
	for (csum = 0; ush >= ((__be16 *) label);)	// 레이블 끝(offset 510)에서 시작(offset 0)까지 역순 순회; NVMe로 읽은 512B 메타데이터 전체를 커버
		csum ^= *ush--;	// 빅엔디안 워드 단위 XOR; endian 변환 없이 원시 바이트 값으로 체크섬 누적
	if (csum) {	// XOR 결과가 0이 아니면 레이블 손상; (추정) NVMe CQE Status = Successful이어도 미디어/전송 손상 가능성 있음
		printk("Dev %s Sun disklabel: Csum bad, label corrupted\n",
		       state->disk->disk_name);	// 체크섬 오류 로그; 이 시점에서 NVMe namespace는 여전히 블록 장치로 존재하나 partition 등록 중단
		put_dev_sector(sect);	// 체크섬 실패 시 버퍼 해제; 이후 파서가 동일 namespace에 다른 레이블 포맷을 시도할 수 있음
		return 0;
	}

	/*
	 * VTOC 테이블을 사용할 수 있는지 sanity/version/nparts 조건을 검사합니다.
	 * NVMe 관점: VTOC이 유효하면 nparts 및 파티션 id/flags를 활용하여
	 * RAID/whole-disk 플래그를 gendisk partition에 반영합니다.
	 */
	/* Check to see if we can use the VTOC table */
	use_vtoc = ((be32_to_cpu(label->vtoc.sanity) == SUN_VTOC_SANITY) &&
		    (be32_to_cpu(label->vtoc.version) == 1) &&
		    (be16_to_cpu(label->vtoc.nparts) <= 8));

	/*
	 * VTOC이 검증되지 않았다면 최대 8개의 파티션 엔트리를 사용합니다.
	 * NVMe 관점: partition minor 최대 개수가 NVMe namespace의
	 * gendisk capacity와는 독립적으로 결정됩니다.
	 */
	/* Use 8 partition entries if not specified in validated VTOC */
	nparts = (use_vtoc) ? be16_to_cpu(label->vtoc.nparts) : 8;	// VTOC이 유효하면 nparts만큼만 NVMe namespace LBA 공간을 분할, 아니면 최대 8개 슬롯 모두 스캔

	/*
	 * 구형 Linux-Sun 파티션과의 하위 호환성을 위해,
	 * sanity/version/nparts가 모두 0이어도 VTOC을 사용하도록 합니다.
	 * NVMe 관점: 레거시 포맷의 namespace도 인식할 수 있도록
	 * 파서의 관용성을 확보합니다.
	 */
	/*
	 * So that old Linux-Sun partitions continue to work,
	 * alow the VTOC to be used under the additional condition ...
	 */
	use_vtoc = use_vtoc || !(label->vtoc.sanity ||
				 label->vtoc.version || label->vtoc.nparts);	// 세 필드가 모두 0이면 레거시 VTOC로 간주; NVMe namespace 상에 구형 레이블이 있을 경우에도 파티션 인식
	/*
	 * spc를 계산합니다: 실린더당 섹터 수 = 헤드 수 * 트랙당 섹터 수.
	 * NVMe 관점: CHS 기반 Sun 레이블을 LBA 기반 NVMe namespace에
	 * 매핑할 때 필요한 변환 계수입니다.
	 */
	spc = be16_to_cpu(label->ntrks) * be16_to_cpu(label->nsect);	// CHS -> LBA 변환 계수; ntrks/nsect가 0이면 spc=0으로 이후 st_sector=0이 될 수 있음 (추정: 이런 경우 partition 시작이 namespace 시작과 겹칠 위험)
	/*
	 * nparts만큼 파티션을 순회하며 gendisk에 등록합니다.
	 * NVMe 관점: 각 파티션은 NVMe namespace LBA 공간의 연속된
	 * 서브 범위를 나타낼, 이후 bio -> nvme_queue_rq 경로에서
	 * partition 시작 LBA를 기준으로 한 상대 sector가 NVMe 명령의
	 * SLBA(Starting LBA) 필드로 변환됩니다.
	 *
	 * (참고) Sun disklabel은 MBR-style extended partition chain이 없으므로
	 * 파티션 스캔 중 namespace로 제출되는 NVMe Read 명령은 LBA 0 한 번뿐입니다.
	 */
	for (i = 0; i < nparts; i++, p++) {	// nparts번 반복; 각 반복은 NVMe namespace 위에 독립 block device(minor)를 하나 등록할 기회
		unsigned long st_sector;	// 현재 파티션의 NVMe namespace 전역 시작 섹터(LBA); bio 제출 시 part 베이스에 더해져 SLBA로 사용
		unsigned int num_sectors;	// 현재 파티션의 섹터 수; NVMe Read/Write 명령의 Length/NLB 계산에 간접적으로 활용

		/*
		 * CHS 시작 실린더를 LBA 섹터 번호로 변환합니다.
		 * NVMe 관점: partition의 시작 위치가 namespace 전역 LBA로
		 * 환산되어 gendisk->part[]에 저장됩니다.
		 */
		st_sector = be32_to_cpu(p->start_cylinder) * spc;	// start_cylinder * spc = partition 시작 LBA; CHS 기호를 NVMe LBA로 변환
		num_sectors = be32_to_cpu(p->num_sectors);	// 빅엔디안 num_sectors를 CPU endian으로 변환; 0이면 미사용(빈) 파티션 엔트리
		if (num_sectors) {	// num_sectors가 0이 아닌 경우만 실제 NVMe namespace의 유효 LBA 범위로 간주하여 등록
			/*
			 * 파티션을 parsed_partitions state에 등록합니다.
			 * NVMe 관점: 이 등록 정보는 bio 제출 시
			 * blk_mq_submit_bio에서 partition minor에 해당하는
			 * gendisk part를 찾아 bio의 시작 sector를 보정하는 데
			 * 사용됩니다.
			 */
			put_partition(state, slot, st_sector, num_sectors);	// gendisk->part[slot] 생성; 이 minor를 통해 들어온 bio는 st_sector를 베이스로 NVMe SLBA 계산
			state->parts[slot].flags = 0;	// 현재 슬롯 플래그 초기화; 이후 ADDPART_FLAG_RAID/WHOLEDISK 여부가 NVMe CID/SQ/CQ에는 반영되지 않음
			/*
			 * VTOC의 파티션 id에 따라 RAID 또는 whole-disk 플래그를
			 * 설정합니다.
			 * NVMe 관점: 이 플래그는 NVMe 명령 자체에는 영향을 주지
			 * 않으나, 상위 블록 계층(예: md RAID autodetect)에서
			 * namespace를 처리하는 방식을 결정합니다.
			 */
			if (use_vtoc) {
				if (be16_to_cpu(label->vtoc.infos[i].id) == LINUX_RAID_PARTITION)
					state->parts[slot].flags |= ADDPART_FLAG_RAID;	// 상위 md autodetect 힌트; NVMe queue_rq에서는 무시
				else if (be16_to_cpu(label->vtoc.infos[i].id) == SUN_WHOLE_DISK)
					state->parts[slot].flags |= ADDPART_FLAG_WHOLEDISK;	// 전체 namespace를 나타내는 파티션으로 표시
			}
		}
		slot++;	// 다음 minor 슬롯 이동; 각 슬롯은 NVMe namespace 위의 독립 block device 후보가 됨
	}
	seq_buf_puts(&state->pp_buf, "\n");	// /proc/partitions 등에 출력할 파티션 정보 버퍼에 개행 추가; NVMe namespace 분할 결과 기록
	put_dev_sector(sect);	// 모든 파티션 등록 완료 후 NVMe Read 버퍼 해제; 이제 gendisk->part[]만으로 I/O 경로가 동작
	return 1;	// Sun disklabel 파싱 성공; 상위 check_partition은 이 namespace에 대해 추가 파서 시도를 중단
}

/*
 * NVMe 관점 핵심 요약
 *
 * - 본 파일은 NVMe namespace의 LBA 0에 기록된 Sun disklabel을 파싱하여
 *   gendisk partition 테이블을 구성하는 선행 단계입니다.
 *
 * - 파티션의 시작/크기 정보는 CHS 기반이지만, NVMe I/O 경로에서는
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 과정에서 LBA 기반으로
 *   변환되어 PRP/SGL에 사용됩니다.
 *
 * - VTOC의 sanity, version, nparts 검증과 XOR 체크섬은 Sun 레이블 수준의
 *   무결성 검사이며, NVMe 자체의 DIF/DIX 데이터 보호 메커니즘과는
 *   독립적으로 동작합니다.
 *
 * - RAID/whole-disk 플래그는 NVMe CID/SQ/CQ 명령에 직접 반영되지 않고,
 *   상위 블록/RAID 계층에서 namespace를 인식하는 메타데이터로 활용됩니다.
 *
 * - (추정) Sun disklabel을 사용하는 레거시 SPARC 시스템에서 NVMe를
 *   부팅 디스크로 사용할 경우, mboot가 bootinfo[]에 기술된 LBA를
 *   NVMe Read 명령으로 읽어 커널 이미지를 적재할 수 있습니다.
 */
