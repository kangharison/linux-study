// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/sgi.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  =====================================================================
 *  NVMe 관점 파일 개요
 *  =====================================================================
 *  이 파일은 SGI 디스크 레이블을 파싱하여 블록 장치 위의 파티션을
 *  generic block layer에 등록한다. NVMe SSD를 포함한 모든 블록 장치가
 *  add_disk()로 등록된 후, block/partitions/check.c의 check_partition()
 *  함수가 파티션 테이블 파서들을 순회하면서 sgi_partition()을 호출한다.
 *  따라서 NVMe 네임스페이스가 블록 장치로 노출된 뒤의 초기화 경로에서
 *  간접적으로 연결된다.
 *  =====================================================================
 */

#include "check.h"		/* partition scan 공통 인프라: read_part_sector(), put_dev_sector(),
				 * put_partition() 선언; NVMe namespace 블록 장치에 대한
				 * 파티션 발견(discovery) 경로를 구성한다. */

/* SGI 레이블 매직 넘버.
 * NVMe namespace는 blk_mq 경로를 통해 첫 섹터(LBA 0)를 읽어오며,
 * read_part_sector() 낶에서 submit_bio -> blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 순으로
 * CID/SQ/CQ/PRP 전송이 일어난다. 이 매직은 그 첫 4바이트와 비교된다.
 * (추정: NVMe SSD는 PRP/SGL로 DMA한 응답을 반환한다)
 */
#define SGI_LABEL_MAGIC 0x0be5a941	/* LBA 0 오프셋 0의 4바이트; NVMe Read 명령으로
					 * PRP entry(들)를 통해 수신한 raw big-endian 값과
					 * 비교되며, 매직 불일치 시 NVMe 하드웨어 오류가 아닌
					 * 포맷 불일치로 처리된다. */

enum {
	/* 자동 RAID 감지용 파티션 타입. NVMe 입장에서는 단순히 LBA 범위에
	 * 붙는 플래그일 뿐, RAID 자체는 md/block layer에서 처리한다. */
	LINUX_RAID_PARTITION = 0xfd,	/* autodetect RAID partition; 동일한 NVMe LBA 구간에
					 * ADDPART_FLAG_RAID를 설정하여 md가 스캔할 수
					 * 있도록 한다. */
};

/* SGI 디스크 레이블.
 * NVMe SSD에서도 namespace 크기와 무관하게 LBA 0에 이 구조체가 있으면
 * SGI 파티션으로 인식된다. 각 필드는 NVMe 논리 블록(LBA) 주소 공간이나
 * 메타데이터 체크섬과 연결된다.
 */
struct sgi_disklabel {
	/* SGI 레이블 매직. NVMe 입장에서는 PRP/SGL로 읽어온 첫 512B의 맨 앞 4바이트다. */
	__be32 magic_mushroom;		/* Big fat spliff... */
	/* ARCS 부트로더용 루트 파티션 번호. NVMe 부팅 시 파티션 선택에 사용될 수 있다. (추정) */
	__be16 root_part_num;		/* Root partition number */
	/* 스왑 파티션 번호. NVMe에서도 스왑 영역이 될 LBA 범위를 가리킨다. */
	__be16 swap_part_num;		/* Swap partition number */
	/* ARCS 부트 파일 이름. NVMe 행동과는 직접 연결되지 않는다. */
	s8 boot_file[16];		/* Name of boot file for ARCS */
	/* 사용되지 않는 SGI 고유 파라미터. NVMe namespace에는 영향 없음. */
	u8 _unused0[48];		/* Device parameter useless crapola.. */
	/* SGI 볼륨: NVMe namespace의 LBA 범위를 논리적 볼륨으로 묶은 단위. */
	struct sgi_volume {
		/* 볼륨 이름. NVMe 식별자와는 무관. */
		s8 name[8];		/* Name of volume */
		/* 볼륨 시작 LBA. NVMe namespace 내 논리 블록 번호로 해석된다. */
		__be32 block_num;		/* Logical block number */
		/* 볼륨 크기(바이트). NVMe PRP/SGL 전송 길이와는 직접 관련 없음. */
		__be32 num_bytes;		/* How big, in bytes */
	} volume[15];			/* SGI 볼륨 배열; on-disk metadata의 NVMe media layout
					 * 일부이나 Linux 파티션 등록에는 직접 사용되지
					 * 않는다. */
	/* SGI 파티션: 최종적으로 blk layer에 등록되는 NVMe LBA 구간. */
	struct sgi_partition {
		/* 파티션 크기(논리 블록 수). NVMe namespace의 LBA 개수다. */
		__be32 num_blocks;		/* Size in logical blocks */
		/* 파티션 시작 LBA. NVMe namespace 내 첫 번째 논리 블록이다. */
		__be32 first_block;	/* First logical block */
		/* 파티션 타입. LINUX_RAID_PARTITION 등이 NVMe 입장에서는 LBA 범위 플래그. */
		__be32 type;		/* Type of this partition */
	} partitions[16];		/* on-disk offset 약 0x168; 각 12바이트 엔트리는 NVMe
					 * namespace의 [first_block, first_block+num_blocks)
					 * LBA 범위를 표현한다. */
	/* 레이블 체크섬. NVMe로 읽어온 메타데이터 무결성을 검증한다. */
	__be32 csum;			/* Disk label checksum */
	/* 정렬용 패딩. NVMe 동작과 무관. */
	__be32 _unused1;			/* Padding */
};					/* 전체 크기 512바이트 == NVMe 논리 블록 1개;
					 * read_part_sector() 한 번의 NVMe Read(LBA 0)으로
					 * 전체 구조체를 얻는다. */

/*
 * sgi_partition()
 *   SGI 디스크 레이블을 읽고 파티션 엔트리를 parsed_partitions에 등록한다.
 *   NVMe namespace가 블록 장치로 노출되면 다음 경로를 통해 호출된다:
 *   add_disk() -> register_disk() -> bdev_disk_changed() ->
 *   rescan_partitions() -> check_partition() (block/partitions/check.c)
 *   -> sgi_partition()
 *   낶에서는 read_part_sector()가 submit_bio -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)를
 *   통해 LBA 0을 읽어오며, CID/SQ/CQ와 PRP/SGL이 실제 NVMe 플래시 읽기로
 *   변환된다. (추정)
 */
int sgi_partition(struct parsed_partitions *state)
{
	int i, csum;			/* i: 16개 파티션 엔트리 인덱스;
					 * csum: on-disk 체크섬 누적값 */
	__be32 magic;			/* LBA 0 오프셋 0의 raw big-endian 매직 워드;
					 * NVMe DMA 버퍼에서 직접 복사된다. */
	int slot = 1;			/* minor/partition slot 번호; 0은 전체 장치이므로
					 * 1부터 시작하여 /dev/nvmeXnYpZ 의 Z에 대응 */
	unsigned int start, blocks;	/* 파티션의 NVMe namespace LBA 시작/블록 수 */
	__be32 *ui, cs;			/* ui: 레이블 끝에서 시작하는 체크섬 순회 포인터;
					 * cs:  현재 word의 big-endian 값 */
	Sector sect;			/* read_part_sector()가 할당한 512B 섹터 버퍼;
					 * NVMe PRP1/PRP2로 DMA 완료 후 CPU 캐시 동기화
					 * 상태의 메모리 영역을 가리킨다. (추정) */
	struct sgi_disklabel *label;
	struct sgi_partition *p;

	/* LBA 0을 읽어 sgi_disklabel 후보를 얻는다. NVMe I/O 경로의 시작점. */
	label = read_part_sector(state, 0, &sect);
					/* read_part_sector(state, 0) ==
					 *  submit_bio(BLK_NSEC_TO_BIO, LBA 0) ->
					 *  blk_mq_submit_bio -> request allocation ->
					 *  nvme_queue_rq -> build PRP/SGL -> nvme_submit_cmd
					 *  (opcode NVME_CMD_READ, nsid, slba=0) ->
					 *  doorbell -> CQE 수신 후 버퍼 반환.
					 *  즉, 이 한 줄이 NVMe 플래시에서 첫 512B를 읽어오는
					 *  Read 명령을 발생시킨다. (추정) */
	if (!label)
		return -1; /* NVMe 읽기 실패 또는 메모리 부족 */
				/* !label은 submit_bio I/O error, GFP_NOIO 섹터 버퍼
				 * 할당 실패, 또는 read_dev_sector() 내부의 bio 처리 실패를
				 * 의미할 수 있다. 이때 파티션 스캔이 중단된다. */
	p = &label->partitions[0];	/* partition entry 배열 시작;
					 * 각 엔트리는 NVMe namespace 내 LBA 범위를
					 * 서술한다. */
	/* 첫 4바이트를 SGI 매직과 비교; 일치하지 않으면 다른 파티션 스킴으로 넘어간다. */
	magic = label->magic_mushroom;
	if(be32_to_cpu(magic) != SGI_LABEL_MAGIC) {
		put_dev_sector(sect);
		return 0;
	}				/* 매직 불일치: NVMe CQE status는 SUCCESS일 수
					 * 있으나 디스크에 SGI 레이블이 없음;
					 * check_partition()이 다음 파서(msdos/efi 등)로
					 * 넘어가도록 0을 반환한다. */
	/* 레이블 끝에서 시작하여 전체 워드를 big-endian으로 더해 체크섬을 계산한다. */
	ui = ((__be32 *) (label + 1)) - 1;
					/* label 끝(구조체 다음)에서 한 word 뺀 주소:
					 * csum 필드(오프셋 약 0x1f8)부터 역순으로 순회;
					 * NVMe로 읽어온 512B on-disk layout을 그대로 따라간다. */
	for(csum = 0; ui >= ((__be32 *) label);) {
		cs = *ui--;		/* NVMe DMA 버퍼에서 big-endian word를 한 개씩
					 * 뒤에서 읽어온다. */
		csum += be32_to_cpu(cs);		/* host byte order로 변환 후 누적;
						 * SGI 알고리즘은 전체 word 합이 0이
						 * 되어야 정상이다. */
	}
	/* 체크섬이 0이 아니면 NVMe로 읽은 메타데이터가 손상되었거나 비SGI 데이터다. */
	if(csum) {
		printk(KERN_WARNING "Dev %s SGI disklabel: csum bad, label corrupted\n",
		       state->disk->disk_name);
		put_dev_sector(sect);
		return 0;
	}				/* csum != 0: NVMe CQE 자체는 정상이어도
					 * logical block 데이터가 손상되었거나 잘못된
					 * 포맷; (추정) NVMe end-to-end data protection
					 * (DIF/DIX) 미적용 namespace라면 이런 손상을
					 * 블록 레이어에서만 검출할 수 있다. */
	/* All SGI disk labels have 16 partitions, disks under Linux only
	 * have 15 minor's.  Luckily there are always a few zero length
	 * partitions which we don't care about so we never overflow the
	 * current_minor.
	 */
	for(i = 0; i < 16; i++, p++) {
					/* 16개 파티션 엔트리 순회; SGI는 msdos처럼
					 * extended partition chain을 사용하지 않으므로
					 * 추가적인 NVMe Read 명령 없이 이미 읽은 LBA 0
					 * 메타데이터만으로 모든 엔트리를 파싱한다. */
		blocks = be32_to_cpu(p->num_blocks); /* NVMe namespace LBA 기준 파티션 크기 */
		start  = be32_to_cpu(p->first_block); /* NVMe namespace 내 시작 LBA */
		if (blocks) {
			/* generic block layer에 파티션 슬롯과 LBA 범위를 등록한다. */
			put_partition(state, slot, start, blocks);
					/* put_partition() -> add_partition() ->
					 *  bdev_add_partition() / device_add() 경로로
					 *  NVMe namespace 위에 /dev/nvmeXnYpZ 형태의
					 *  block device를 생성한다. 이후 해당 파티션에
					 *  대한 bio는 block layer에서 start LBA를 더해
					 *  NVMe queue로 전달된다. */
			/* RAID 파티션 타입이면 md가 이 LBA 범위를 검사할 수 있도록 플래그를 설정한다. */
			if (be32_to_cpu(p->type) == LINUX_RAID_PARTITION)
				state->parts[slot].flags = ADDPART_FLAG_RAID;
					/* 동일한 NVMe LBA 범위를 RAID candidate로
					 * 마킹; I/O 경로 자체는 NVMe를 그대로 경유한다. */
		}
		slot++;			/* 다음 minor/partition slot;
					 * blocks==0인 엔트리도 slot은 소모하지만
					 * put_partition()은 호출되지 않아 NVMe 상위
					 * block device 생성이 생략된다. */
	}
	/* 파티션 정보 문자열 끝에 개행을 추가한다. */
	seq_buf_puts(&state->pp_buf, "\n");
	put_dev_sector(sect); /* read_part_sector()에서 할당한 섹터를 해제한다. */
					/* 섹터 해제 후 NVMe 관련 bio/request도
					 * 이미 완료된 상태; partition scan 중간에
					 * 할당한 임시 버퍼만 정리한다. */
	return 1;
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 NVMe 컨트롤러/드라이버를 직접 다루지 않으며, NVMe namespace가
 *   블록 장치로 등록된 뒤에 실행되는 파티션 테이블 파서이다.
 * - read_part_sector()는 blk_mq/NVMe I/O 경로를 통해 LBA 0을 읽으며,
 *   submit_bio -> ... -> nvme_submit_cmd(doorbell) 과정에서 CID, SQ, CQ,
 *   PRP/SGL이 실제 플래시 읽기로 변환된다. (추정)
 * - 파티션 엔트리의 first_block/num_blocks는 NVMe namespace의 논리 블록
 *   주소(LBA) 공간을 사용자 공간에 노출하는 기준이다.
 * - 파싱 실패(매직 불일치/체크섬 오류)는 NVMe 하드웨어 오류보다는 레이블
 *   포맷 불일치 또는 메타데이터 손상을 의미할 가능성이 크다.
 * - block/partitions/check.c의 check_partition()과 msdos.c, efi.c 등 다른
 *   파서들이 함께 파티션 발견(partition discovery) 체인을 구성한다.
 */
