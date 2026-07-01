/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  fs/partitions/mac.h
 */

/*
 * Apple Mac 파티션 맵(APM)의 on-disk 레이아웃을 정의하는 헤더.
 * NVMe SSD의 namespace가 커널에 의해 인식될 때, LBA 0과 후속 블록에서
 * 이 구조체를 읽어 파티션 테이블을 파싱한다.
 *
 * 전형적 경로:
 *   nvme_scan_ns -> nvme_revalidate_disk -> rescan_partitions ->
 *   check_partition -> mac_partition
 * I/O 제출 경로:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell/CID/SQ)
 *
 * 이 파일은 block/partitions/check.c 및 block/partitions/mac.c와 논리적으로
 * 연결되며, 파싱된 파티션은 NVMe PRP/SGL의 SLBA 계산에 사용된다.
 */

/* APM 항목의 서명(리틀엔디안 'PM'). NVMe read 명령으로 LBA 0을 읽은 후
 * 이 값이 맞는지 확인하여 Mac 파티션 맵인지 판별한다. */
#define MAC_PARTITION_MAGIC	0x504d	/* offset 0x0000 in APM entry (추정)
					 * -> read_part_sector(0)이 NVMe Read(CID)
					 *    명령을 제출하고, CQE success 후
					 *    buffers[0..1]과 비교됨 */

/* type field value for A/UX or other Unix partitions */
#define APPLE_AUX_TYPE	"Apple_UNIX_SVR2"	/* NVMe: Unix 데이터 파티션임을
						 * 나타내는 단순 문자열 식별자.
						 * NVMe I/O 동작에는 영향 없음 */

/*
 * mac_partition: Apple Partition Map(APM)의 한 항목
 *
 * NVMe namespace는 LBA(Logical Block Address) 단위로 주소를 표현하며,
 * 이 구조체의 start_block, block_count 등은 NVMe namespace의 논리 블록
 * 크기(block_size)와 결합되어 실제 바이트 오프셋과 크기로 해석된다.
 *
 * 호출 맥락 (추정):
 *   rescan_partitions -> mac_partition() -> add_partition()
 *   이후 I/O:
 *   submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell/CID/SQ)
 */
struct mac_partition {
	__be16	signature;	/* expected to be MAC_PARTITION_MAGIC */
				/* NVMe: LBA 0 또는 파티션 맵 항목 읽기 시
				 * doorbell을 통한 첫 CID 명령 완료 후
				 * 버퍼에서 확인되는 매직 넘버 (추정) */
				/* offset 0x00, size 2: APM entry 매직 식별자.
				 *   read_part_sector() -> submit_bio -> nvme_queue_rq
				 *   -> nvme_submit_cmd(CDW10=SLBA) -> CQE DNR/SC 비교
				 *   -> 이 값이 0x504d가 아니면 파티션 탐색 중단 (추정) */
	__be16	res1;		/* NVMe: 예약 필드, NVMe 컨트롤러는 무시함 */
				/* offset 0x02, size 2: APM 항목 내 예약 영역.
				 *   NVMe PRP/SGL 전송 시에도 controller가 해석하지
				 *   않으며, driver는 이 영역을 0으로 기대할 수 있음 */
	__be32	map_count;	/* # blocks in partition map */
				/* NVMe: 파티션 맵 자체가 차지하는 블록 수.
				 * I/O 스케줄러가 bio를 제출하기 전에
				 * partition table 영역 접근 여부를 판단할 때
				 * 참고할 수 있음 (추정) */
				/* offset 0x04, size 4: APM 전체가 차지하는 블록 수.
				 *   mac_partition() 루프의 상한값으로 사용되며,
				 *   각 반복은 read_part_sector(i)를 통해 NVMe Read
				 *   명령 1회를 추가 제출함 (추정) */
	__be32	start_block;	/* absolute starting block # of partition */
				/* NVMe: 해당 파티션의 namespace 기준 절대
				 * 시작 LBA. blk_partition_remap()에서 bio의
				 * bi_sector에 더해져 NVMe PRP/SGL의 시작
				 * 주소(SLBA)로 변환됨 */
				/* offset 0x08, size 4: 파티션의 절대 시작 LBA.
				 *   -> add_partition() 시 gendisk.parts[].start_sect
				 *      에 저장됨
				 *   -> 이후 submit_bio: bi_sector + start_sect를
				 *      NVMe 명령 CDW10/CDW11(SLBA)로 변환 */
	__be32	block_count;	/* number of blocks in partition */
				/* NVMe: 파티션의 총 블록 수. NVMe Identify
				 * Namespace의 NSZE/NCAP과 비교하여 범위를
				 * 검증하는 데 사용될 수 있음 */
				/* offset 0x0c, size 4: 파티션 크기(블록 수).
				 *   -> start_block + block_count 가 NSZE 이내인지
				 *      확인; 초과 시 NVMe Read/Write가
				 *      LBA Out of Range(CQE SC=0x80) 발생 가능 (추정) */
	char	name[32];	/* partition name */
				/* NVMe: OS가 파티션을 식별하는 레이블.
				 * NVMe controller는 이 필드를 직접 해석하지
				 * 않고 블록 계층에서만 사용함 */
				/* offset 0x10, size 32: 파티션 레이블 문자열.
				 *   NVMe 명령의 데이터 버퍼에서 복사되며,
				 *   gendisk 파티션 정보에 표시됨 */
	char	type[32];	/* string type description */
				/* NVMe: 파일 시스템/용도 식별자. 예를 들어
				 * APPLE_AUX_TYPE은 Unix 데이터 파티션임을
				 * 나타낼 수 있으며, NVMe I/O 경로에는 영향을
				 * 주지 않음 */
				/* offset 0x30, size 32: 파티션 타입 문자열.
				 *   mac.c에서 Apple_UNIX_SVR2 등과 비교하여
				 *   put_partition() 호출 여부를 결정함 (추정) */
	__be32	data_start;	/* rel block # of first data block */
				/* NVMe: 파티션 내에서 실제 데이터 영역의
				 * 상대 시작 LBA. bio remap 후 NVMe 명령의
				 * SLBA(Starting LBA) 계산에 간접 참여함 */
				/* offset 0x50, size 4: 데이터 영역 상대 시작.
				 *   -> 실제 I/O SLBA = start_block + data_start +
				 *      bio.bi_sector (추정) */
	__be32	data_count;	/* number of data blocks */
				/* NVMe: 실제 데이터 블록 수. 읽기/쓰기
				 * 범위가 이 영역을 벗어나지 않도록 검증할
				 * 수 있음 (추정) */
				/* offset 0x54, size 4: 데이터 블록 수.
				 *   -> NVMe Write Zeroes/Trim 범위 산정 시
				 *      참고 가능 (추정) */
	__be32	status;		/* partition status bits */
				/* NVMe: 부팅 가능 여부 등 상태 플래그.
				 * NVMe controller는 직접 사용하지 않고,
				 * 블록 계층에서 파티션 속성으로 해석함 */
				/* offset 0x58, size 4: 상태/속성 비트 필드.
				 *   -> MAC_STATUS_BOOTABLE(비트3) 등이 설정되면
				 *      커널이 부팅 파티션으로 표시함 */
	__be32	boot_start;	/* NVMe: 부트스트랩 코드의 상대 시작 LBA
				 * (bootable 파티션인 경우) */
				/* offset 0x5c, size 4: 부트 코드 상대 시작.
				 *   -> NVMe 부팅 파티션(Boot Partition)과는 별개이며,
				 *      일반 Read 명령으로 로드 가능함 (추정) */
	__be32	boot_size;	/* NVMe: 부트스트랩 코드 블록 수 */
				/* offset 0x60, size 4: 부트 코드 블록 수.
				 *   -> NVMe Read(CDW12=Number of LBs) 산정 시
				 *      사용될 수 있음 (추정) */
	__be32	boot_load;	/* NVMe: 부트 로드 주소 (NVMe I/O 경로와 무관) */
				/* offset 0x64, size 4: 부트 로드 메모리 주소 */
	__be32	boot_load2;	/* NVMe: 부트 로드 주소 확장 (추정) */
				/* offset 0x68, size 4: 부트 로드 주소 확장 */
	__be32	boot_entry;	/* NVMe: 부트 진입점 (NVMe I/O 경로와 무관) */
				/* offset 0x6c, size 4: 부트 진입 메모리 주소 */
	__be32	boot_entry2;	/* NVMe: 부트 진입점 확장 (추정) */
				/* offset 0x70, size 4: 부트 진입점 확장 */
	__be32	boot_cksum;	/* NVMe: 부트 코드 무결성 체크섬.
				 * NVMe 데이터 경로와는 무관하며 펌웨어
				 * 부팅 시 사용될 수 있음 (추정) */
				/* offset 0x74, size 4: 부트 체크섬.
				 *   -> NVMe CQE status와 별개로 SW가 검증하며,
				 *      mismatch 시 partition scan은 계속되지만
				 *      부팅은 실패할 수 있음 (추정) */
	char	processor[16];	/* identifies ISA of boot */
				/* NVMe: 부트 프로세서 ISA 식별. NVMe SQ/CQ
				 * I/O 명령 경로와는 직접 관련 없음 */
				/* offset 0x78, size 16: 프로세서 ISA 문자열.
				 *   NVMe namespace LBA 레이아웃과 무관함 */
	/* there is more stuff after this that we don't need */
				/* NVMe: 파티션 항목 이후 필드는 현재
				 * 블록 장치 운용 및 NVMe I/O remap에
				 * 불필요하므로 무시함 */
				/* -> sizeof(struct mac_partition)은 최소 0x88(136B)
				 *    이며, NVMe read는 보통 512B 섹터 단위이므로
				 *    하나의 APM entry는 단일 Read 명령 버퍼에
				 *    여러 개 포함될 수 있음 (추정) */
};

#define MAC_STATUS_BOOTABLE	8	/* partition is bootable */
					/* NVMe: 부팅 가능 파티션 표시.
					 * NVMe controller의 부팅 파티션
					 * 기능(Boot Partition)과는 별개임 (추정) */
					/* -> status 필드의 비트 3; 설정 시
					 *    gendisk 파티션 플래그에 반영되어
					 *    NVMe 위 블록 장치에서 부팅 가능으로 표시됨 */

/* Driver descriptor의 서명(리틀엔디안 'ER'). LBA 0에서 검증됨 */
#define MAC_DRIVER_MAGIC	0x4552	/* offset 0x0000 in LBA 0 buffer (추정)
					 * -> read_part_sector(0) 결과의 첫 2바이트;
					 *    매칭되지 않으면 mac_partition()이
					 *    -EINVAL 반환 후 partition scan 종료 (추정) */

/*
 * mac_driver_desc: LBA 0에 위치한 드라이버 서술자
 *
 * NVMe namespace의 첫 번째 논리 블록(LBA 0)에서 발견되며, block_size는
 * NVMe Format 명령으로 설정된 LBA 데이터 크기(보통 512B 또는 4KiB)와
 * 일치해야 정상적으로 파싱된다. block_count는 namespace 총 용량을
 * 블록 수로 표현한 값과 의미상 동일하다.
 *
 * 호출 맥락 (추정):
 *   nvme_scan_ns -> ... -> rescan_partitions -> read_lba(0) ->
 *   mac_driver_desc 파싱
 */
/* Driver descriptor structure, in block 0 */
struct mac_driver_desc {
	__be16	signature;	/* expected to be MAC_DRIVER_MAGIC */
				/* NVMe: LBA 0 첫 2바이트; 정상적인 APM 디스크
				 * 인지를 NVMe read 명령 완료 후 판별하는
				 * 데 사용됨 */
				/* offset 0x00, size 2: LBA 0 매직 식별자.
				 *   -> read_part_sector(0)이 NVMe Read(CDW10=0)
				 *      명령 1회 제출; CQE success 후 버퍼[0..1]
				 *      과 0x45 0x52 비교 (추정) */
	__be16	block_size;	/* NVMe: 한 논리 블록의 바이트 수. NVMe
				 * Identify Namespace에서 보고된 LBAF의
				 * LBA Data Size와 대응됨 */
				/* offset 0x02, size 2: 논리 블록 크기(바이트).
				 *   -> queue_limits.logical_block_size와 비교;
				 *    불일치 시 sector<->LBA 변환이 어긋나
				 *    NVMe PRP/SGL 정렬 오류 발생 가능 (추정) */
	__be32	block_count;	/* NVMe: namespace의 총 블록 수. Identify
				 * Namespace의 NSZE와 의미상 동일함 */
				/* offset 0x04, size 4: 총 블록 수.
				 *   -> capacity = block_size * block_count로
				 *      NVMe namespace 총용량과 대응;
				 *      set_capacity() 호출 시 사용됨 (추정) */
    /* ... more stuff */
				/* NVMe: 드라이버 서술자의 나머지 필드는
				 * 현재 NVMe 블록 장치 인식 및 파티션
				 * 파싱에 불필요함 */
				/* -> sizeof(struct mac_driver_desc)은 최소 8B;
				 *    LBA 0의 나머지 504B(512B sector 기준)는
				 *    APM 파티션 맵 항목 또는 예약 영역임 (추정) */
};

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 * - APM 파티션 맵은 NVMe namespace의 LBA 0 및 후속 LBA에 저장되며, 커널이
 *   namespace를 프로브할 때 rescan_partitions -> mac_partition 경로로
 *   파싱된다.
 * - mac_partition.start_block은 bio가 partition remap을 거쳐 NVMe 명령의
 *   SLBA로 변환되는 출발점이 된다.
 * - mac_driver_desc.block_size는 NVMe LBA 데이터 크기와 일치해야 하며,
 *   그렇지 않으면 partition table 해석이 어긋나 I/O 실패로 이어질 수 있다.
 * - 이 파일은 block/partitions/check.c, block/partitions/mac.c 등과 논리적으로
 *   연결되며, NVMe 드라이버(drivers/nvme/host/core.c)는 파싱된 partition
 *   정보를 바탕으로 gendisk를 재구성한다.
 * - NVMe SQ/CQ, doorbell, CID, PRP/SGL 등의 실제 명령 제어는 본 파티션
 *   파싱 코드와는 직접 상호작용하지 않는다. (추정)
 * ============================================================================
 */
