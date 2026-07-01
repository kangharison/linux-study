/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NVMe 관점 파일 요약:
 *  - Atari AHDI/ICD/XGM 파티션 테이블의 on-disk 레이아웃을 정의하는 헤더.
 *  - NVMe SSD가 노출한 namespace(block device)에 대해 커널이 부팅/탐색 시
 *    파티션 정보를 해석할 때 사용된다.
 *  - 전체 호출 경로(추정): nvme_alloc_ns -> add_disk -> device_add_disk
 *    -> bdev_disk_changed -> rescan_partitions -> check_partition
 *    -> atari_partition (atari.c) -> struct rootsector / struct partition_info.
 *  - blk_mq, nvme_queue, doorbell, CID, SQ/CQ, PRP/SGL 등과는 직접 통신하지
 *    않고, NVMe 드라이버가 완료한 I/O 결과(섹터 데이터)를 파싱하는 단계다.
 */
/*
 * 추가 NVMe 밀착 주석:
 *  - read_part_sector(state, sector, &sect) -> submit_bio -> nvme_queue_rq
 *    -> doorbell -> NVMe READ(opcode 0x02) -> CQE -> DMA 버퍼 반환(추정).
 *  - atari_partition() 루프에서 part[4]/icdpart[8] 및 확장 파티션 체인을
 *    순회하며 put_partition() -> add_partition() 호출, NVMe namespace 위에
 *    하위 block device 등록(추정).
 *  - kmalloc()/read_part_sector() 메모리 또는 I/O 실패 시 partition scan이
 *    중단되고 add_disk()가 실패 처리된다(추정).
 *  - struct rootsector의 __packed 및 필드 오프셋은 NVMe media에서 읽은
 *    raw 512B LBA 0 섹터와 1:1 매핑되어야 한다.
 */

/*
 *  fs/partitions/atari.h
 *  Moved by Russell King from:
 *
 * linux/include/linux/atari_rootsec.h
 * definitions for Atari Rootsector layout
 * by Andreas Schwab (schwab@ls5.informatik.uni-dortmund.de)
 *
 * modified for ICD/Supra partitioning scheme restricted to at most 12
 * partitions
 * by Guenther Kelleter (guenther@pool.informatik.rwth-aachen.de)
 */

#include <linux/compiler.h>
/* __packed 제공: NVMe DMA로 받은 512B 섹터와 구조체 간 컴파일러 패딩 없이
 * 1:1 매핑되어 on-disk 레이아웃이 깨지지 않는다.
 */

/*
 * atari_partition() -> struct partition_info *pi 에서 읽는 단일 파티션 엔트리.
 * NVMe namespace의 LBA 0(또는 확장 파티션 내 섹터)에서 이 레코드를 읽어
 * 어떤 논리 영역이 활성/부팅 가능한지 판단한다.
 */
struct partition_info
{
  u8 flg;			/* bit 0: active; bit 7: bootable
				 * NVMe 입장에서는 단지 namespace LBA 범위의
				 * 메타데이터 플래그이며, doorbell/CID와 무관.
				 */
				/* NVMe 연결: offset 0; active/bootable 플래그는
				 * namespace LBA 범위 메타데이터이며, NVMe doorbell
				 * 이나 CID와는 무관하다.
				 */
  char id[3];			/* "GEM", "BGM", "XGM", or other
				 * 파일시스템/용도 식별자. NVMe SSD는 이 값을
				 * 해석하지 않고 커널 block layer가 해석.
				 */
				/* NVMe 연결: offset 1; "GEM"/"BGM"/"XGM" 등
				 * magic/signature는 block layer가 파티션 타입을
				 * 판별할 때 사용하고, NVMe SSD는 raw bytes만
				 * 전달한다(추정).
				 */
  __be32 st;			/* start of partition
				 * NVMe namespace 내 시작 LBA(512-byte sector 기준).
				 * read_part_sector()로 PRP/SGL을 통해 읽은 섹터 번호.
				 */
				/* NVMe 연결: offset 4; start LBA(512B sector).
				 * read_part_sector() -> submit_bio -> nvme_queue_rq
				 * 에서 이 LBA를 NVMe READ 명령의 SLBA로 변환(추정).
				 * Atari는 LBA 기반이므로 CHS 변환 없이 namespace
				 * LBA로 직접 매핑된다.
				 */
  __be32 siz;			/* length of partition
				 * 해당 파티션의 섹터 개수; NVMe READ/WRITE의
				 * lba range를 형성하는 기초 정보.
				 */
				/* NVMe 연결: offset 8; sector count.
				 * NVMe READ/WRITE range = [st, st+siz) 로 활용되며,
				 * 0이면 해당 엔트리를 무시하거나 예외 처리한다(추정).
				 */
};
				/* sizeof(struct partition_info) == 12 bytes;
				 * rootsector 내 연속 배열로 NVMe 섹터 오프셋을
				 * 고정시킨다.
				 */

/*
 * NVMe namespace LBA 0에 위치한 Atari rootsector 레이아웃.
 * atari.c 의 atari_partition()이 read_part_sector(state, 0, &sect)로
 * 이 전체 구조체를 읽어 파티션 테이블을 복원한다.
 */
struct rootsector
{
  char unused[0x156];		/* room for boot code
				 * NVMe가 전달한 첫 0x156 바이트는 파티션 파싱에
				 * 사용되지 않는 부트 코드/예약 영역.
				 */
				/* NVMe 연결: offset 0x000 ~ 0x155; boot code.
				 * read_part_sector(state, 0)로 읽힌 DMA 버퍼의
				 * 첫 0x156 bytes이며, 파티션 탐색에는 사용되지
				 * 않는다.
				 */
  struct partition_info icdpart[8];	/* info for ICD-partitions 5..12
					 * ICD/Supra 확장 포맷용 추가 엔트리.
					 * NVMe 입장에서는 추가 LBA range descriptor 집합.
					 */
					/* NVMe 연결: offset 0x156; 8개 추가
					 * partition_info. atari_partition() 루프에서
					 * part[4] 다음으로 검사되며, 유효한 엔트리는
					 * put_partition() -> NVMe namespace 위 block
					 * device 등록으로 이어진다(추정).
					 * 확장 파티션 체인이 존재하면 각 link마다
					 * 추가 read_part_sector() -> NVMe READ가
					 * 발생할 수 있다(추정).
					 */
  char unused2[0xc];		/* 패딩/예약; NVMe 섹터 데이터 내 위치 고정용 */
				/* NVMe 연결: offset 0x1B6; 12 bytes 패딩.
				 * __packed로 인해 컴파일러 패딩이 삽입되지 않아
				 * 뒤따르는 hd_siz/part[] 오프셋이 NVMe media의
				 * raw 512B와 일치한다.
				 */
  u32 hd_siz;			/* size of disk in blocks
				 * 디스크 전체 블록 수; NVMe namespace capacity와
				 * 대응되며 get_capacity()와 비교해 유효성 검사.
				 */
				/* NVMe 연결: offset 0x1C2; 전체 블록 수.
				 * NVMe Identify Namespace의 NSZE/NCAP과 대응하며,
				 * 커널이 get_capacity()로 비교해 유효성을 검사할
				 * 수 있다(추정).
				 */
  struct partition_info part[4];
				/* NVMe 연결: offset 0x1C6; 4개 primary
				 * partition_info. atari_partition() 루프에서
				 * 순회하며 각 st/siz를 검증한 뒤
				 * put_partition() -> add_partition()을 통해 NVMe
				 * namespace 위 독립 block device를 생성한다(추정).
				 */
  u32 bsl_st;			/* start of bad sector list
				 * Atari era의 결함 섹터 목록 시작; 현대 NVMe SSD는
				 * 낸드 불량 블록을 낸드플래시 컨트롤러가 재배치하지만
				 * (추정) 커널은 이 필드를 무시하고 파티션 경계만 계산.
				 */
				/* NVMe 연결: offset 0x1F6; bad sector list
				 * 시작 LBA. 현대 NVMe SSD는 낸드플래시 컨트롤러
				 * FTL이 재배치하므로 커널이 이 필드를 무시하고
				 * 파티션 경계만 계산한다(추정).
				 */
  u32 bsl_cnt;			/* length of bad sector list */
				/* NVMe 연결: offset 0x1FA; bad sector list
				 * 길이. 파티션 경계나 NVMe 명령 생성에는 영향을
				 * 주지 않는다.
				 */
  u16 checksum;			/* checksum for bootable disks
				 * 부팅 가능 디스크의 체크섬; NVMe stack에서는
				 * 사용하지 않음(추정).
				 */
				/* NVMe 연결: offset 0x1FE; bootable disk
				 * checksum. NVMe CQE status(DNR/SC)와는 별개이며,
				 * 커널이 SW로 검증/무시한다(추정).
				 */
} __packed;				/* on-disk 바이트 단위 패킹:
					 * NVMe READ 로 받은 512B 섹터와 정확히 맞추기
					 * 위해 패딩을 금지함.
					 */
					/* NVMe 연결: sizeof(struct rootsector)
					 * == 512 bytes. NVMe READ로 받은 LBA 0
					 * raw 섹터와 1:1 매핑되도록 __packed를
					 * 사용한다.
					 */

/* NVMe 관점 핵심 요약
 *  - 이 파일은 NVMe SSD namespace에 존재하는 레거시 Atari 파티션 레이아웃을
 *    해석하는 데이터 구조체 집합이다.
 *  - NVMe READ 명령으로 LBA 0(PRP/SGL 경유)을 읽고, 그 512B 데이터를
 *    struct rootsector로 캐스팅하여 파티션을 복원한다.
 *  - 호출 연결(추정): add_disk -> rescan_partitions -> check_partition
 *    -> atari_partition -> read_part_sector -> struct rootsector.
 *  - 이 헤더는 block/partitions/atari.c와 짝을 이루며, block/partitions/core.c
 *    의 파티션 탐색 루프 아래서 동작한다.
 *  - struct rootsector의 __packed 속성은 NVMe가 DMA로 전달한 raw 섹터와
 *    커널 구조체 간 1:1 매핑을 보장하기 위함이다.
 */
