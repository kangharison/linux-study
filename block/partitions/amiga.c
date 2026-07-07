// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/amiga.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 */

/*
 * [한국어 설명] Amiga RDB(Rigid Disk Block) 파티션 테이블 파서 (amiga.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Commodore Amiga 컴퓨터가 사용하던 RDB(Rigid Disk Block, 강성 디스크
 * 블록) 파티션 포맷을 리눅스 블록 계층이 인식할 수 있도록 파싱한다. 디스크
 * 맨 앞부분(최대 RDB_ALLOCATION_LIMIT개 섹터)을 섹터 단위로 순회하며 "RDSK"
 * 매직 시그니처를 가진 RigidDiskBlock을 찾고, 빅엔디안 32비트 정수합
 * 체크섬으로 그 블록의 무결성을 검증한다. RDB를 찾으면 그 안의
 * rdb_PartitionList 포인터를 시작으로 "PART" 매직을 가진 PartitionBlock
 * 연결 리스트(pb_Next 체인)를 순회하면서, 각 파티션의 CHS(Cylinder/Head/
 * Sector, 실린더/헤드/섹터) 좌표를 리눅스가 이해하는 (start_sect, nr_sects)
 * 512바이트 섹터 좌표 쌍으로 환산해 커널에 등록한다. 이 파서가 없으면
 * Amiga RDB로 포맷된 디스크(또는 그 이미지)를 리눅스에서 파티션 단위로
 * 인식하거나 마운트할 수 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 블록 계층은 새 디스크(gendisk)가 등록될 때
 * device_add_disk() -> bdev_disk_changed() -> blk_add_partitions() ->
 * rescan_partitions() -> check_partition()의 흐름으로 여러 파티션 테이블
 * 형식을 순차적으로 시도한다(block/partitions/core.c). check_partition()은
 * block/partitions/check.h에 나열된 각 형식별 <형식>_partition() 함수를
 * 하나씩 호출해 보고, 0이 아닌 값을 반환하는 첫 함수가 그 디스크의 담당
 * 형식으로 인정된다. amiga_partition()은 그 후보 중 하나이며, msdos/efi/
 * mac 등 다른 형식이 먼저 인식하지 못했을 때만 실질적으로 파티션을
 * 발견하게 된다. 이 파일 자체는 하드웨어에서 직접 섹터를 읽지 않고,
 * check.h가 제공하는 read_part_sector()를 통해 블록 계층의 동기 섹터 읽기
 * 경로(내부적으로 bio 제출과 블록 드라이버의 요청 처리를 거침)를 간접
 * 이용할 뿐이다. 실행 컨텍스트는 파티션 스캔을 수행하는 프로세스
 * 컨텍스트(디스크 프로브 또는 BLKRRPART ioctl 처리 중)이며, 단일 스레드
 * 동기 실행이라 락이나 원자적 연산이 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: block/partitions/check.h(parsed_partitions 상태 구조체와
 * read_part_sector()/put_dev_sector()/put_partition() 헬퍼 선언),
 * <linux/affs_hardblocks.h>(RigidDiskBlock/PartitionBlock on-disk 레이아웃
 * 정의와 IDNAME_RIGIDDISK/IDNAME_PARTITION/RDB_ALLOCATION_LIMIT 상수),
 * <linux/overflow.h>(check_mul_overflow()/check_add_overflow() 오버플로우
 * 검사 매크로). 이 파일에 의존하는 모듈: block/partitions/core.c의
 * check_partition()이 함수 포인터 테이블을 통해 amiga_partition()을
 * 호출한다. 데이터 흐름은 디스크(또는 디스크 이미지) -> read_part_sector()가
 * 채우는 Sector/data 버퍼 -> RigidDiskBlock/PartitionBlock으로의 캐스팅
 * -> checksum_block()을 통한 무결성 검증 -> pb_Environment(DosEnvVec)에서
 * CHS 필드 추출 및 오버플로우 검사 -> start_sect/nr_sects 산출 ->
 * put_partition()을 통한 parsed_partitions->parts[] 등록 -> 이후 core.c가
 * gendisk의 파티션 테이블에 반영하는 순서로 흐른다. 공유 자료구조는
 * struct parsed_partitions(state)이며, 이 구조체를 통해 디스크 이름,
 * pp_buf(사용자에게 보일 요약 문자열), parts[] 배열을 다른 파티션
 * 파서들과 동일한 방식으로 갱신한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - checksum_block(): 빅엔디안 32비트 워드 배열의 합을 계산하는 헬퍼.
 *   RDB와 PartitionBlock 양쪽의 무결성 검증에 공용으로 쓰인다.
 * - amiga_partition(): 이 파일의 유일한 공개 함수이자 진입점. RDB 탐색,
 *   체크섬 검증(Windows 훼손 우회 포함), PartitionBlock 연결 리스트 순회,
 *   CHS->섹터 환산, 각 단계의 오버플로우 검사, put_partition() 등록까지
 *   전 과정을 담당한다.
 * - struct RigidDiskBlock, struct PartitionBlock (<linux/affs_hardblocks.h>
 *   정의, 이 파일에서는 데이터 해석 대상으로만 사용): 각각 디스크 전체
 *   기하구조/파티션 리스트 헤드, 개별 파티션의 CHS 범위와 dostype을 담는
 *   on-disk 빅엔디안 구조체이다.
 * - NR_HD/NR_SECT/LO_CYL/HI_CYL: PartitionBlock.pb_Environment[] 배열(일명
 *   DosEnvVec, AmigaDOS 환경 벡터)에서 헤드 수/트랙당 섹터 수/시작
 *   실린더/끝 실린더가 위치한 인덱스를 나타내는 매직 오프셋 상수이다.
 */

#define pr_fmt(fmt) fmt	/* [한국어] 이 파일의 pr_err()/pr_warn() 메시지에 별도 접두어를 붙이지 않음 -- 포맷 문자열(fmt)을 그대로 사용해 "Dev %s: ..." 형태로 출력되게 함 */

#include <linux/types.h>	/* [한국어] u32/u64/__be32 등 커널 공용 고정폭 정수 타입 -- RDB/PartitionBlock 필드의 크기를 정확히 표현하기 위해 필요 */
#include <linux/mm_types.h>	/* [한국어] 메모리 관리 관련 타입 정의 제공 -- check.h의 Sector(folio 기반) 래퍼가 참조하는 하위 타입을 위해 포함되는 것으로 추정 */
#include <linux/overflow.h>	/* [한국어] check_mul_overflow()/check_add_overflow() 매크로 정의 -- CHS 좌표를 곱하고 더해 섹터 수를 구하는 과정에서 32/64비트 오버플로우를 검사하기 위해 사용 */
#include <linux/affs_hardblocks.h>	/* [한국어] RigidDiskBlock/PartitionBlock on-disk 구조체 정의와 IDNAME_RIGIDDISK ("RDSK")/IDNAME_PARTITION("PART") 매직, RDB_ALLOCATION_LIMIT 상수 제공 */

#include "check.h"	/* [한국어] 파티션 스캔 프레임워크 헤더 -- parsed_partitions, Sector, read_part_sector()/put_dev_sector()/put_partition() 선언을 가져옴 */

/* magic offsets in partition DosEnvVec */ /* [한국어] AmigaDOS는 PartitionBlock.pb_Environment[] 배열(DosEnvVec, 환경 벡터)에 디스크 기하구조/파일시스템 파라미터를 실어 보낸다. 아래 4개 상수는 그 배열에서 헤드 수/트랙당 섹터 수/시작-끝 실린더가 위치하는 인덱스이다. */
#define NR_HD	3	/* [한국어] DosEnvVec[3]: 헤드(head) 수 -- CHS 좌표 중 H, nr_hd로 읽힘 */
#define NR_SECT	5	/* [한국어] DosEnvVec[5]: 트랙당 섹터 수 -- CHS 좌표 중 S, nr_sect로 읽힘 */
#define LO_CYL	9	/* [한국어] DosEnvVec[9]: 파티션 시작 실린더(Low Cylinder) -- start_sect 계산의 기준 */
#define HI_CYL	10	/* [한국어] DosEnvVec[10]: 파티션 끝 실린더(High Cylinder) -- nr_sects 계산의 기준 */

/*
 * [한국어]
 * checksum_block - RDB/PartitionBlock 블록의 빅엔디안 32비트 워드 체크섬 계산
 *
 * @m: 체크섬을 계산할 빅엔디안(__be32) 워드 배열의 시작 주소. RigidDiskBlock
 *     또는 PartitionBlock을 담고 있는 섹터 버퍼(data/pb)를 그대로 캐스팅한
 *     포인터가 전달된다.
 * @size: 합산할 32비트 워드(4바이트) 개수. 호출자가 rdb_SummedLongs/
 *        pb_SummedLongs 필드의 하위 7비트(& 0x7F)로 넘겨주며, 이 값은
 *        블록 하나(512바이트=128워드)를 넘지 않는 범위로 제한된다.
 * @return: 모든 워드를 CPU 엔디안으로 변환해 누적한 u32 합. AmigaDOS의
 *          체크섬 규약은 "블록 전체(체크섬 필드 자신 포함)의 32비트 워드
 *          합이 0이면 무결"이므로, 호출자는 이 반환값이 0인지만 비교한다.
 *
 * AmigaDOS의 RDB/PartitionBlock은 별도의 CRC가 아니라 CPU 정수 산술의
 * 2의 보수 wraparound를 이용하는 자기보정형 체크섬을 쓴다. 블록을 기록할
 * 때 rdb_ChkSum/pb_ChkSum 필드에 "블록 전체의 워드 합이 0이 되도록"
 * 미리 계산한 보정값을 넣어 두므로, 읽는 쪽은 체크섬 필드까지 포함해
 * 전부 더하기만 하면 검증이 끝난다. u32는 부호 없는 32비트 산술이라
 * 오버플로우가 나도 mod 2^32로 자동 wraparound되므로 별도의 캐리 처리가
 * 필요 없다.
 * 실행 컨텍스트: amiga_partition()과 동일한 단일 스레드 파티션 스캔
 * 컨텍스트에서 동기적으로 실행되며, 전역 상태를 건드리지 않는 순수
 * 함수라 재진입/락이 필요 없다.
 * 호출자: amiga_partition()이 RDB 검증 시 최대 두 번(원본 1회, 0xdc~0xdf
 * 워드를 0으로 만든 우회본 1회), PartitionBlock 검증 시 한 번, 총 세
 * 지점에서 호출한다.
 * 피호출자: be32_to_cpu() 뿐이다(엔디안 변환 인라인 함수/매크로).
 * 에러 처리: 이 함수 자체는 실패를 표현하지 않는다(항상 정상적으로 합을
 * 반환). 무결성 판단(합이 0인지 비교)과 실패 시 에러 로그 출력은 모두
 * 호출자의 몫이다.
 *
 * 호출 체인:
 *   amiga_partition() → [checksum_block] → be32_to_cpu()
 */
static __inline__ u32	/* [한국어] 반환 타입 u32 -- 워드 합계, 오버플로우는 자연스러운 mod 2^32 wraparound로 흡수 */
checksum_block(__be32 *m, int size)	/* [한국어] m: 빅엔디안 워드 배열 시작 주소, size: 합산할 워드(4바이트) 개수 */
{
	u32 sum = 0;	/* [한국어] 누적 합의 초기값 0 -- u32 wraparound 산술이라 별도 오버플로우 방지 로직 불필요 */

	while (size--)	/* [한국어] size가 0이 될 때까지(size--는 후위 감소 후 0이면 false) 워드 개수만큼 반복 */
		sum += be32_to_cpu(*m++);	/* [한국어] 빅엔디안 워드를 CPU 엔디안으로 변환해 sum에 누적하고, 포인터 m을 다음 워드로 전진(m++) */
	return sum;	/* [한국어] 전체 워드 합 반환 -- 호출자는 이 값이 0인지 비교해 체크섬 통과 여부를 판단 */
}

/*
 * [한국어]
 * amiga_partition - Amiga RDB 파티션 테이블을 탐색해 커널에 파티션 등록
 *
 * @state: 파티션 스캔 상태를 담는 구조체(block/partitions/check.h). state->disk는
 *         현재 스캔 중인 gendisk, state->parts[]는 발견된 파티션이 기록될
 *         배열, state->pp_buf는 사용자에게 보여줄 요약 문자열 버퍼이다.
 *         호출자(core.c의 check_partition())가 이미 할당·초기화해 넘겨준다.
 * @return: 1  - RDB를 찾았고 하나 이상의 유효한 파티션을 등록한 경우.
 *          0  - RDB_ALLOCATION_LIMIT 섹터 안에서 유효한 RDB를 전혀 찾지
 *               못한 경우(이 디스크는 Amiga RDB 형식이 아님).
 *          -1 - 섹터 읽기(read_part_sector)가 실패한 경우(I/O 오류 또는
 *               버퍼 할당 실패). 이 값은 그 이전에 이미 파티션을 등록해
 *               res=1이 되어 있었더라도 덮어쓴다(I/O 오류 보고가 우선).
 *
 * 디스크 초반 RDB_ALLOCATION_LIMIT(16)개 섹터를 순차적으로 읽으며 "RDSK"
 * 매직으로 시작하는 RigidDiskBlock을 찾는다. 찾으면 체크섬으로 무결성을
 * 검증하고(Windows가 특정 오프셋을 훼손했을 가능성까지 우회 검증), RDB의
 * rdb_BlockBytes로부터 "512바이트 표준 블록 몇 개가 RDB의 논리 블록
 * 하나인지"(blksize)를 계산한다. 그다음 rdb_PartitionList가 가리키는
 * PartitionBlock 연결 리스트(pb_Next로 체이닝, 최대 16개)를 순회하면서
 * 각 파티션의 "PART" 매직/체크섬을 검증하고, pb_Environment(DosEnvVec)에서
 * 헤드 수/트랙당 섹터 수/시작-끝 실린더를 읽어 CHS 좌표를
 * (start_sect, nr_sects) 512바이트 섹터 좌표로 환산한다. 각 단계마다
 * 32비트 곱셈/덧셈 오버플로우를 check_mul_overflow()/check_add_overflow()로
 * 검사해, 손상되었거나 비정상적으로 큰 값이 들어와도 정수 오버플로우로
 * 이어지지 않도록 방어한다. 유효한 파티션은 put_partition()으로 등록하고,
 * dostype 4글자를 pp_buf에 부가 정보로 출력한다.
 *
 * 실행 컨텍스트: 파티션 스캔은 단일 프로세스 컨텍스트(디스크 프로브 또는
 * BLKRRPART ioctl 처리 스레드)에서 동기적으로 실행되며, 이 함수 내부에서
 * 별도의 락이나 원자적 연산은 쓰지 않는다(state는 스캔 기간 동안 다른
 * 스레드와 공유되지 않는다고 가정).
 * 호출자: block/partitions/core.c의 check_partition()이 다른 파티션 형식
 * 파서들과 함께 순서대로 시도한다.
 * 피호출자: read_part_sector()/put_dev_sector()(섹터 I/O), checksum_block()
 * (체크섬), check_mul_overflow()/check_add_overflow()(오버플로우 검사),
 * put_partition()(등록), pr_err()/pr_warn()/seq_buf_printf()(로그/출력).
 * 에러 경로: read_part_sector()가 NULL을 반환하면 즉시 res=-1로 표시하고
 * rdb_done 레이블로 점프해 함수를 종료한다. 그 외 개별 파티션 단위의
 * 오버플로우나 체크섬 실패는 continue로 해당 파티션만 건너뛰고 스캔을
 * 이어간다(치명적 오류로 취급하지 않음).
 *
 * 호출 체인:
 *   check_partition() → [amiga_partition] → read_part_sector()/checksum_block()/put_partition()
 */
int amiga_partition(struct parsed_partitions *state)	/* [한국어] state->disk는 이 함수가 검사 중인 gendisk(디스크) 객체를 가리킴 */
{
	Sector sect;	/* [한국어] read_part_sector()가 채워주는 섹터 버퍼 래퍼 -- 매 반복 put_dev_sector(sect)로 반드시 해제 */
	unsigned char *data;	/* [한국어] 현재 읽어온 섹터의 원시 바이트 포인터 -- RDB 또는 PartitionBlock으로 캐스팅해 해석 */
	struct RigidDiskBlock *rdb;	/* [한국어] 찾아낸 RDB를 가리키는 포인터 -- 탐색 루프를 빠져나온 뒤에도 blksize/rdb_CylBlocks 등을 참조하려 유지 */
	struct PartitionBlock *pb;	/* [한국어] PartitionList 순회 중 현재 파티션 엔트리를 가리키는 포인터 */
	u64 start_sect, nr_sects;	/* [한국어] 최종 산출되는 파티션의 시작 섹터와 섹터 수(512바이트 단위) -- put_partition()에 그대로 전달됨 */
	sector_t blk, end_sect;	/* [한국어] blk: 현재 읽으려는 섹터 번호(RDB 탐색 시엔 원시 LBA, PartitionList 순회 시엔 blksize 배율 적용 후 LBA); end_sect: start_sect+nr_sects 오버플로우 검사용 임시값 */
	u32 cylblk;		/* rdb_CylBlocks = nr_heads*sect_per_track */ /* [한국어] 실린더당 총 블록 수(헤드수×트랙당섹터수) -- 이후 512바이트 단위로 재정규화되어 start_sect/nr_sects 계산의 곱셈 인자로 쓰임 */
	u32 nr_hd, nr_sect, lo_cyl, hi_cyl;	/* [한국어] pb_Environment(DosEnvVec)에서 읽어올 헤드 수/트랙당 섹터 수/시작-끝 실린더 임시 변수 */
	int part, res = 0;	/* [한국어] part: 1부터 시작하는 파티션 순번(최대 16); res: 함수 반환값(0=RDB 미발견,1=성공,-1=I/O오류)으로 초기값 0 */
	unsigned int blksize = 1;	/* Multiplier for disk block size */ /* [한국어] RDB의 논리 블록 하나가 512바이트 표준 블록 몇 개에 해당하는지 나타내는 배율 -- 기본값 1(=512바이트)로 초기화 후 rdb_BlockBytes로 갱신 */
	int slot = 1;	/* [한국어] put_partition()에 넘길 파티션 슬롯 번호 -- 등록할 때마다 1씩 증가(슬롯 0은 디스크 전체 의미이므로 1부터 시작) */

	/* [한국어] RDB 탐색 루프: blk=0부터 시작해 섹터를 하나씩 순차적으로 읽으며
	 * "RDSK" 매직을 가진 RigidDiskBlock을 찾는다. 매 반복 종료 시(continue 포함)
	 * put_dev_sector(sect)로 직전에 읽은 섹터를 반드시 해제한다. AmigaDOS 스펙상
	 * RDB는 디스크 첫 RDB_ALLOCATION_LIMIT(16)개 섹터 어딘가에 위치할 수 있다
	 * (항상 0번 섹터는 아님). */
	for (blk = 0; ; blk++, put_dev_sector(sect)) {	/* [한국어] blk를 0부터 증가시키며 섹터 단위로 순회 -- 이전 반복에서 읽은 sect는 다음 반복 진입 전에 해제 */
		if (blk == RDB_ALLOCATION_LIMIT)		/* [한국어] AmigaDOS 스펙이 정한 탐색 한계(16섹터)에 도달 -- 더 이상 RDB가 없다고 판단 */
			goto rdb_done;			/* [한국어] res=0(초기값) 그대로 유지한 채 함수 종료 경로로 이동 -- 이 디스크는 Amiga RDB가 아님을 의미 */
		data = read_part_sector(state, blk, &sect);		/* [한국어] blk번째 섹터를 동기적으로 읽어와 data에 원시 바이트 포인터를 저장, sect에는 버퍼 소유권 보관 */
		if (!data) {		/* [한국어] 섹터 읽기 실패(디스크 I/O 오류 또는 버퍼 할당 실패) 판단 */
			pr_err("Dev %s: unable to read RDB block %llu\n",			/* [한국어] 커널 로그로 읽기 실패를 알림 -- 포맷 문자열은 다음 줄의 인자와 함께 완성됨 */
			       state->disk->disk_name, blk);			/* [한국어] 로그 인자: 실패한 디스크 이름과 섹터 번호(blk) */
			res = -1;			/* [한국어] 반환값을 I/O 오류로 표시 */
			goto rdb_done;			/* [한국어] 추가 섹터 읽기를 시도하지 않고 즉시 함수 종료 경로로 이동 */
		}
		if (*(__be32 *)data != cpu_to_be32(IDNAME_RIGIDDISK))		/* [한국어] 섹터 첫 4바이트가 빅엔디안 매직 "RDSK"(IDNAME_RIGIDDISK=0x5244534B)와 다른지 확인 */
			continue;			/* [한국어] RDB가 아닌 섹터이므로 건너뜀 -- for문 증가부(put_dev_sector)가 실행되어 이번에 읽은 sect가 해제된 후 다음 blk로 진행 */

		rdb = (struct RigidDiskBlock *)data;		/* [한국어] 매직이 일치했으므로 원시 바이트를 RigidDiskBlock 구조체로 재해석(캐스팅) */
		if (checksum_block((__be32 *)data, be32_to_cpu(rdb->rdb_SummedLongs) & 0x7F) == 0)		/* [한국어] rdb_SummedLongs 하위 7비트(최대 128워드)만큼 체크섬 계산 -- 0이면 이 블록이 손상되지 않았다는 뜻 */
			break;			/* [한국어] 유효한 RDB를 확정하고 탐색 루프 종료 -- 이후 rdb/sect가 그대로 사용됨(아직 put_dev_sector 호출 안 됨) */
		/* Try again with 0xdc..0xdf zeroed, Windows might have
		 * trashed it.
		 */
		/* [한국어] 일부 Windows 파티션 도구가 이 오프셋(0xdc~0xdf, RDB 내부 예약/미사용
		 * 워드로 추정)에 값을 잘못 기록해 체크섬이 깨지는 사례가 있었다고 알려짐. 그
		 * 워드를 메모리 상에서만 0으로 지운 뒤 체크섬을 다시 계산해, 실제로는 유효한
		 * RDB를 "손상"으로 오판하지 않도록 구제한다. */
		*(__be32 *)(data+0xdc) = 0;		/* [한국어] 섹터 버퍼(data, 메모리 상의 사본) 오프셋 0xdc의 4바이트를 0으로 덮어씀 -- 디스크에는 쓰지 않으므로 원본 미디어 데이터는 변경되지 않음 */
		if (checksum_block((__be32 *)data,		/* [한국어] 0xdc 워드를 0으로 만든 상태로 체크섬 재계산 시작 */
				be32_to_cpu(rdb->rdb_SummedLongs) & 0x7F)==0) {			/* [한국어] 이전과 동일한 워드 개수(rdb_SummedLongs&0x7F)로 재검증 -- 0이면 훼손된 워드를 무시하고도 무결성 확인 성공 */
			pr_err("Trashed word at 0xd0 in block %llu ignored in checksum calculation\n",			/* [한국어] 우회 검증으로 통과했음을 사용자에게 경고 로그로 알림(0xd0 워드가 손상되었을 가능성을 명시) */
			       blk);			       /* [한국어] 로그 인자로 현재 블록 번호(blk) 전달 */
			break;			/* [한국어] 우회 검증으로 확정된 RDB로 인정하고 탐색 루프 종료 */
		}		/* [한국어] 우회 체크섬 검증 분기(0xdc 워드 무시) 종료 */

		pr_err("Dev %s: RDB in block %llu has bad checksum\n",		/* [한국어] 두 번의 체크섬 시도(원본, 0xdc 워드 제거본) 모두 실패 -- 진짜로 손상된 RDB이거나 RDB가 아닌데 우연히 매직만 일치한 섹터로 판단하고 경고 로그 출력 */
		       state->disk->disk_name, blk);		       /* [한국어] 로그 인자: 디스크 이름과 블록 번호; 이후 break/continue 없이 for문 몸체 끝까지 도달해 blk++ 후 다음 섹터 탐색으로 자연스럽게 이어짐 */
	}	/* [한국어] RDB 탐색 for 루프 종료 -- 정상적으로 이 지점을 지나 아래로 내려가는 유일한 경로는 체크섬 검증에 성공했을 때 실행되는 break(유효 RDB 발견 또는 Windows 훼손 우회 인정)뿐이며, 그 외에는 rdb_done으로 조기 종료되거나 반복이 지속됨 */

	/* blksize is blocks per 512 byte standard block */ /* [한국어] RDB가 정의하는 논리 블록 크기(rdb_BlockBytes, 바이트 단위)가 512바이트 표준 섹터 몇 개에 해당하는지 계산해 blksize에 저장 -- 이후 모든 파티션 좌표를 512바이트 단위로 통일하기 위한 배율 */
	blksize = be32_to_cpu( rdb->rdb_BlockBytes ) / 512;	/* [한국어] rdb_BlockBytes(빅엔디안, 바이트 단위)를 CPU 엔디안으로 변환 후 512로 나눔 -- 예: BlockBytes=512면 blksize=1, 4096이면 blksize=8 */

	/* Be more informative */ /* [한국어] 사용자에게 보여줄 파티션 요약 문자열에 RDB를 찾았다는 표시와 실제 블록 바이트 수를 덧붙이기 위한 준비 */
	seq_buf_printf(&state->pp_buf, " RDSK (%d)", blksize * 512);	/* [한국어] pp_buf(디스크명 옆에 출력되는 문자열 버퍼)에 " RDSK (블록바이트수)" 형태로 기록 */
	blk = be32_to_cpu(rdb->rdb_PartitionList);	/* [한국어] RDB 필드(rdb_PartitionList)를 읽어 첫 PartitionBlock의 원시 LBA(아직 blksize 배율 미적용)를 blk에 저장 */
	put_dev_sector(sect);	/* [한국어] RDB가 담긴 섹터 버퍼는 더 이상 필요 없으므로 명시적으로 해제 -- RDB 탐색 for문은 이미 break로 빠져나와 있어 그 for문 증가부의 자동 해제는 이미 끝났고 이번은 별도의 수동 해제 */
	/* [한국어] PartitionBlock 연결 리스트 순회 루프: rdb_PartitionList가 가리키는 첫
	 * 엔트리부터 pb_Next 체인을 따라가며 최대 16개까지 파티션을 처리한다. blk를
	 * (s32)로 캐스팅해 부호 있는 정수로 비교하는 이유는, pb_Next가 0이면 "다음
	 * 없음"이고 AmigaDOS 스펙상 링크 종료를 나타내는 값이 상위 비트를 포함할 수
	 * 있어 부호 있는 비교로 리스트 종료를 안전하게 감지하기 위함이다. 매 반복
	 * 종료 시 put_dev_sector(sect)로 직전 파티션 섹터를 해제한다. */
	for (part = 1; (s32) blk>0 && part<=16; part++, put_dev_sector(sect)) {	/* [한국어] part=1부터 최대 16개까지 순회 -- blk가 0 이하가 되면(pb_Next 체인 종료) 루프 탈출 */
		/* Read in terms partition table understands */ /* [한국어] PartitionBlock의 pb_Next는 "RDB 논리 블록" 단위이므로, 실제 512바이트 섹터 LBA로 쓰려면 blksize를 곱해 환산해야 함 */
		if (check_mul_overflow(blk, (sector_t) blksize, &blk)) {		/* [한국어] blk(논리 블록 번호) × blksize(512B 배율)를 계산해 다시 blk에 저장 -- sector_t 범위를 넘는 오버플로우가 나면 true 반환 */
			pr_err("Dev %s: overflow calculating partition block %llu! Skipping partitions %u and beyond\n",			/* [한국어] 오버플로우 발생을 경고 -- 이 파티션 및 이후 체인 전체를 포기한다는 의미 */
				state->disk->disk_name, blk, part);				/* [한국어] 로그 인자: 디스크 이름, (오버플로우로 값이 불확실해진) blk, 현재 파티션 번호 part */
			break;			/* [한국어] PartitionList 순회를 완전히 중단 -- 오버플로우 이후의 체인은 신뢰할 수 없으므로 더 이상 탐색하지 않음 */
		}
		data = read_part_sector(state, blk, &sect);		/* [한국어] 환산된 512바이트 LBA(blk)에서 PartitionBlock이 담긴 섹터를 읽어옴 */
		if (!data) {		/* [한국어] 섹터 읽기 실패 판단(RDB 탐색 때와 동일한 오류 처리 패턴) */
			pr_err("Dev %s: unable to read partition block %llu\n",			/* [한국어] 파티션 블록 읽기 실패를 커널 로그로 알림 */
			       state->disk->disk_name, blk);			       /* [한국어] 로그 인자: 디스크 이름과 실패한 섹터 번호 */
			res = -1;			/* [한국어] 반환값을 I/O 오류로 표시 */
			goto rdb_done;			/* [한국어] 이미 등록된 파티션이 있어도(res가 1이었더라도) 즉시 함수를 종료 -- I/O 오류(res=-1)가 이전의 성공 표시를 덮어씀 */
		}
		pb  = (struct PartitionBlock *)data;		/* [한국어] 읽어온 원시 바이트를 PartitionBlock 구조체로 재해석 */
		blk = be32_to_cpu(pb->pb_Next);		/* [한국어] 체인의 다음 파티션 블록 번호(RDB 논리 블록 단위, 아직 blksize 미적용)를 미리 읽어둠 -- 다음 반복에서 blksize를 곱해 512바이트 LBA로 환산할 때(check_mul_overflow) 입력이 됨 */
		if (pb->pb_ID != cpu_to_be32(IDNAME_PARTITION))		/* [한국어] 매직 시그니처가 "PART"(IDNAME_PARTITION=0x50415254)와 다른지 확인 -- pb_ID는 이미 __be32이므로 상수 쪽을 cpu_to_be32로 변환해 비교 */
			continue;			/* [한국어] PartitionBlock이 아닌 엔트리이므로 건너뜀 -- blk는 이미 pb_Next로 갱신되어 있어 다음 반복은 체인을 계속 따라감 */
		if (checksum_block((__be32 *)pb, be32_to_cpu(pb->pb_SummedLongs) & 0x7F) != 0 )		/* [한국어] pb_SummedLongs 하위 7비트만큼 체크섬 계산 -- 0이 아니면(!=0) 손상된 것으로 판단 (RDB와 달리 PartitionBlock에는 Windows 우회 재검증 로직이 없음) */
			continue;			/* [한국어] 체크섬 불일치 파티션은 등록하지 않고 건너뜀 -- blk는 이미 다음 체인 값으로 갱신됨 */

		/* RDB gives us more than enough rope to hang ourselves with,
		 * many times over (2^128 bytes if all fields max out).
		 * Some careful checks are in order, so check for potential
		 * overflows.
		 * We are multiplying four 32 bit numbers to one sector_t!
		 */
		/* [한국어] 이 뒤로는 nr_hd × nr_sect × (hi_cyl-lo_cyl+1) × blksize 라는 최대
		 * 4개의 32비트 값을 연속으로 곱해 하나의 sector_t에 담아야 한다. 모든 필드가
		 * 극단값이면 이론상 2^128바이트까지 표현하려는 셈이 되므로, 각 곱셈/덧셈
		 * 단계마다 check_mul_overflow()/check_add_overflow()로 오버플로우를 검사하지
		 * 않으면 손상되었거나 터무니없이 큰 값이 조용히 잘못된 파티션 범위로
		 * 등록될 위험이 있다. */

		nr_hd   = be32_to_cpu(pb->pb_Environment[NR_HD]);		/* [한국어] DosEnvVec[3](NR_HD 인덱스)에서 헤드 수를 읽음 */
		nr_sect = be32_to_cpu(pb->pb_Environment[NR_SECT]);		/* [한국어] DosEnvVec[5](NR_SECT 인덱스)에서 트랙당 섹터 수를 읽음 */

		/* CylBlocks is total number of blocks per cylinder */ /* [한국어] 실린더 하나에 들어가는 총 블록 수 = 헤드 수 × 트랙당 섹터 수(디스크 기하구조 상 원통 하나를 채우는 블록 개수) */
		if (check_mul_overflow(nr_hd, nr_sect, &cylblk)) {		/* [한국어] nr_hd × nr_sect를 계산해 cylblk(u32)에 저장 -- 둘 다 pb_Environment에서 읽은 신뢰할 수 없는 32비트 값이므로 곱셈 오버플로우를 검사 */
			pr_err("Dev %s: heads*sects %u overflows u32, skipping partition!\n",			/* [한국어] 오버플로우로 이 파티션 하나만 포기한다는 경고(PartitionList 전체 탐색은 계속됨) */
				state->disk->disk_name, cylblk);				/* [한국어] 로그 인자: 디스크 이름과 오버플로우로 인해 신뢰할 수 없어진 cylblk 값 */
			continue;			/* [한국어] 이 파티션 등록을 포기하고 PartitionList의 다음 엔트리로 이동(blk는 이미 pb_Next로 갱신됨) */
		}

		/* check for consistency with RDB defined CylBlocks */ /* [한국어] 파티션이 스스로 계산한 실린더당 블록 수(cylblk)가 RDB 전체가 선언한 rdb_CylBlocks와 맞는지 교차 검증 -- 불일치해도 등록을 막지 않고 경고만 남김 */
		if (cylblk > be32_to_cpu(rdb->rdb_CylBlocks)) {		/* [한국어] 파티션의 실린더당 블록 수가 RDB 전체 값보다 큰 비정상 상황 감지 */
			pr_warn("Dev %s: cylblk %u > rdb_CylBlocks %u!\n",			/* [한국어] continue 없이 경고만 출력하고 아래 로직을 계속 진행함 -- 등록 자체를 막지는 않음 */
				state->disk->disk_name, cylblk,				/* [한국어] 로그 인자: 디스크 이름과 파티션이 계산한 cylblk */
				be32_to_cpu(rdb->rdb_CylBlocks));				/* [한국어] 로그 인자: RDB가 선언한 실린더당 블록 수(빅엔디안 필드를 CPU 엔디안으로 변환) */
		}

		/* RDB allows for variable logical block size -
		 * normalize to 512 byte blocks and check result.
		 */ /* [한국어] RDB의 논리 블록 크기는 디스크마다 다를 수 있으므로(rdb_BlockBytes), cylblk(RDB 논리 블록 단위)를 blksize 배율로 곱해 512바이트 표준 섹터 단위로 재정규화한다. 이 결과가 이후 start_sect/nr_sects 계산의 곱셈 인자가 된다. */

		if (check_mul_overflow(cylblk, blksize, &cylblk)) {		/* [한국어] cylblk(실린더당 RDB 블록 수) × blksize(512B 배율)을 계산해 다시 cylblk에 저장(512바이트 단위 실린더 크기로 갱신) -- 오버플로우 검사 */
			pr_err("Dev %s: partition %u bytes per cyl. overflows u32, skipping partition!\n",			/* [한국어] 512바이트 단위로 정규화한 실린더 크기가 32비트를 넘어 이 파티션을 계산할 수 없음을 경고 */
				state->disk->disk_name, part);				/* [한국어] 로그 인자: 디스크 이름과 현재 파티션 번호 */
			continue;			/* [한국어] 이 파티션은 등록하지 않고 다음 체인 엔트리로 이동 */
		}

		/* Calculate partition start and end. Limit of 32 bit on cylblk
		 * guarantees no overflow occurs if LBD support is enabled.
		 */ /* [한국어] 여기서부터 파티션의 시작/끝 섹터를 계산한다. cylblk가 이미 32비트 범위 안으로 검증되었으므로(위에서 오버플로우 검사 통과), lo_cyl/hi_cyl·cylblk 모두 32비트 값을 u64로 승격해 곱하면 LBD(Large Block Device, 64비트 섹터 번호 지원) 활성화 시 오버플로우가 발생하지 않음이 보장된다. */

		lo_cyl = be32_to_cpu(pb->pb_Environment[LO_CYL]);		/* [한국어] DosEnvVec[9](LO_CYL 인덱스)에서 파티션의 시작 실린더 번호를 읽음 */
		start_sect = ((u64) lo_cyl * cylblk);		/* [한국어] 시작 실린더 × (512바이트 정규화된) 실린더당 블록 수 = 파티션 시작 섹터(512바이트 단위); lo_cyl을 u64로 캐스팅해 64비트 곱셈을 강제함으로써 32비트 산술 오버플로우를 회피 */

		hi_cyl = be32_to_cpu(pb->pb_Environment[HI_CYL]);		/* [한국어] DosEnvVec[10](HI_CYL 인덱스)에서 파티션의 끝 실린더 번호를 읽음 */
		nr_sects = (((u64) hi_cyl - lo_cyl + 1) * cylblk);		/* [한국어] (끝 실린더 - 시작 실린더 + 1) = 파티션이 차지하는 실린더 개수, 여기에 실린더당 블록 수를 곱해 전체 섹터 수 산출; hi_cyl을 u64로 캐스팅해 뺄셈·곱셈 모두 64비트로 수행 */

		if (!nr_sects)		/* [한국어] 산출된 섹터 수가 0인 경우(예: hi_cyl < lo_cyl로 언더플로우가 나거나 실제로 크기가 없는 파티션) 감지 */
			continue;			/* [한국어] 크기가 0인 파티션은 등록할 의미가 없으므로 건너뜀 */

		/* Warn user if partition end overflows u32 (AmigaDOS limit) */ /* [한국어] AmigaDOS 자체는 32비트 섹터 번호까지만 이해하므로, 리눅스가 64비트로 이 파티션을 처리하더라도 원래 포맷의 한계를 넘었음을 사용자에게 알려줌(등록 자체는 막지 않음) */

		if ((start_sect + nr_sects) > UINT_MAX) {		/* [한국어] 시작+크기가 32비트 부호없는 정수 최대값(UINT_MAX)을 넘는지 확인 -- start_sect/nr_sects가 u64이므로 이 덧셈 자체는 오버플로우하지 않고 안전하게 비교 가능 */
			pr_warn("Dev %s: partition %u (%llu-%llu) needs 64 bit device support!\n",			/* [한국어] 이 파티션을 다루려면 64비트(LBD) 디바이스 지원이 필요하다는 경고만 출력하고 등록은 계속 진행 */
				state->disk->disk_name, part,				/* [한국어] 로그 인자: 디스크 이름과 파티션 번호 */
				start_sect, start_sect + nr_sects);				/* [한국어] 로그 인자: 시작 섹터와 끝 섹터(경계) 값 */
		}

		if (check_add_overflow(start_sect, nr_sects, &end_sect)) {		/* [한국어] start_sect+nr_sects를 end_sect(sector_t)에 저장 -- sector_t가 32비트인 환경(LBD 미지원 빌드)에서는 64비트 값이 잘려 오버플로우가 발생할 수 있어 검사 */
			pr_err("Dev %s: partition %u (%llu-%llu) needs LBD device support, skipping partition!\n",			/* [한국어] sector_t 폭이 부족해(LBD 미지원) 이 파티션을 표현할 수 없음을 알리고 등록을 포기 */
				state->disk->disk_name, part,				/* [한국어] 로그 인자: 디스크 이름과 파티션 번호 */
				start_sect, end_sect);				/* [한국어] 로그 인자: 시작 섹터와 (오버플로우로 잘렸을 수 있는) end_sect 값 */
			continue;			/* [한국어] sector_t로 표현 불가능한 파티션은 등록하지 않고 다음 체인 엔트리로 이동 */
		}

		/* Tell Kernel about it */ /* [한국어] 지금까지의 모든 검증을 통과했으므로 이 파티션을 실제로 커널의 파티션 테이블에 등록하는 단계 */

		put_partition(state,slot++,start_sect,nr_sects);		/* [한국어] state->parts[slot]에 (start_sect, nr_sects)를 기록하고 pp_buf에 "이름+번호"를 추가; slot은 후위 증가(++)로 사용 후 1 증가해 다음 파티션에 대비 */
		{		/* [한국어] 블록 스코프 시작 -- dostype 문자열을 pp_buf에 추가 출력하기 위한 지역 변수(dostype, dt)의 생명주기를 이 스코프로 제한 */
			/* Be even more informative to aid mounting */			 /* [한국어] 파일시스템 종류(dostype)를 함께 보여주면 사용자가 어떤 파일시스템으로 마운트해야 할지 판단하는 데 도움이 됨 */
			char dostype[4];			/* [한국어] Amiga의 4바이트 파일시스템 식별자(예: "DOS\0", "DOS\1" 등)를 담을 로컬 버퍼 */

			__be32 *dt = (__be32 *)dostype;			/* [한국어] 4바이트 문자 배열을 하나의 빅엔디안 32비트 워드로 다루기 위한 별칭 포인터 */
			*dt = pb->pb_Environment[16];			/* [한국어] DosEnvVec[16]에 저장된 dostype 워드(이미 __be32)를 dostype 배열에 그대로 복사 -- 엔디안 변환 없이 원시 빅엔디안 바이트 순서 그대로 저장됨에 유의 */
			if (dostype[3] < ' ')			/* [한국어] dostype 4번째 바이트가 출력 가능한 아스키(스페이스=0x20) 미만인 제어 문자인지 확인 -- AmigaDOS는 이 바이트에 파일시스템 버전 번호를 함께 인코딩하는 관례가 있어(예: DOS\1, DOS\2) 화면 표시용으로 별도 처리 필요 */
				seq_buf_printf(&state->pp_buf,				/* [한국어] 제어 문자인 경우: 그대로 출력하면 터미널이 깨지므로 "^문자" 형태(캐럿 표기)로 안전하게 표시 */
					       " (%c%c%c^%c)",					 /* [한국어] 포맷: 앞 3바이트는 그대로, 마지막 바이트는 캐럿(^) 뒤에 가시 문자로 변환해 출력 */
					       dostype[0], dostype[1],					 /* [한국어] dostype 첫 두 바이트를 %c 인자로 전달 */
					       dostype[2],					 /* [한국어] dostype 세 번째 바이트를 %c 인자로 전달 */
					       dostype[3] + '@');					 /* [한국어] 제어 문자(0~0x1F)에 '@'(0x40)를 더해 캐럿 표기 관례(^@=NUL, ^A=0x01 등)에 맞는 가시 문자로 변환 */
			else			/* [한국어] dostype[3]이 출력 가능한 일반 문자인 경우 */
				seq_buf_printf(&state->pp_buf,				/* [한국어] 4바이트를 그대로 문자로 출력 */
					       " (%c%c%c%c)",					 /* [한국어] 포맷: 4바이트 모두 %c로 그대로 표시(예: " (DOS1)") */
					       dostype[0], dostype[1],					 /* [한국어] dostype 첫 두 바이트 전달 */
					       dostype[2], dostype[3]);					 /* [한국어] dostype 나머지 두 바이트 전달 */
			seq_buf_printf(&state->pp_buf, "(res %d spb %d)",			/* [한국어] 추가로 예약 블록 수(res)와 블록당 섹터 관련 파라미터(spb)를 부가 정보로 출력 준비 */
				       be32_to_cpu(pb->pb_Environment[6]),				       /* [한국어] DosEnvVec[6]: 예약 블록 수(res) -- 부트블록 등 파일시스템이 예약하는 블록 개수(추정) */
				       be32_to_cpu(pb->pb_Environment[4]));				       /* [한국어] DosEnvVec[4]: 블록당 섹터 수(spb, sectors per block) -- 파일시스템 레벨 블록 크기 관련 파라미터(추정) */
		}		/* [한국어] dostype 출력용 블록 스코프 종료 -- dostype/dt 지역 변수의 생명주기가 여기서 끝남 */
		res = 1;		/* [한국어] 하나 이상의 파티션을 성공적으로 등록했음을 표시 -- 이후 다른 파티션이 실패하더라도(체크섬/오버플로우로 continue) 이 값은 유지되며, 오직 read_part_sector 실패(res=-1) 경로만 이 값을 덮어씀 */
	}	/* [한국어] PartitionList 순회 for 루프 종료 -- 다음 반복 진입 조건((s32)blk>0 && part<=16) 재평가로 이어짐 */
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 파티션 요약 문자열의 끝에 줄바꿈 추가로 출력 마무리 -- RDB를 못 찾은 경로(goto rdb_done)에서는 이 줄을 거치지 않음에 유의 */

rdb_done: /* [한국어] 함수의 단일 종료 레이블 -- RDB 탐색 실패/성공, 각 read 실패, 정상 종료 모든 경로가 이곳으로 모여 res를 반환 */
	return res;	/* [한국어] 최종 반환값: 1=파티션 하나 이상 등록 성공, 0=RDB 자체를 찾지 못함(다른 파티션 형식 파서가 이어서 시도), -1=섹터 읽기 실패(I/O 오류) */
} /* [한국어] amiga_partition() 함수 종료 */
