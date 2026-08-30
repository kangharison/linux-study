// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/mac.c
 *
 *  Code extracted from drivers/block/genhd.c
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 *
 * [한국어 설명] 구형 68k/PowerPC Mac OS의 Apple Partition Map(APM) 파티션
 * 테이블을 인식하는 파서 (mac.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 블록 장치의 0번 블록에서 mac_driver_desc(드라이버 서술자)를
 * 읽어 매직 넘버("ER" = MAC_DRIVER_MAGIC)와 논리 블록 크기를 확인한 뒤,
 * 이어지는 블록들에서 mac_partition 엔트리(파티션 맵)를 순서대로 읽어
 * 각 파티션의 시작 위치와 크기를 커널 표준 512바이트 섹터 단위로 환산해
 * parsed_partitions 구조체에 등록한다. CONFIG_PPC_PMAC(PowerPC 기반
 * PowerMac) 환경에서는 추가로 부팅에 가장 적합한 루트 파티션을 찾아
 * note_bootable_part()로 부팅 서브시스템에 알려주는 역할도 겸한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 장치가 커널에 등록되거나 재검사될 때(add_disk, ioctl BLKRRPART 등)
 * bdev_disk_changed() -> rescan_partitions() -> check_partition() 호출
 * 체인을 거치며, check.c의 check_partition()이 등록된 파티션 파서 배열
 * (check.c의 check_part[])을 순서대로 시도하다가 이 파일의
 * mac_partition()을 호출한다. mac_partition()은 섹터를 읽기 위해
 * check.h가 선언한 read_part_sector()를 호출하며, 이는 내부적으로 bio를
 * 구성해 블록 계층에 동기적으로 제출한다(구체적 제출 경로는 하위 드라이버에
 * 따라 다르며, 예를 들어 NVMe 장치라면 submit_bio -> blk_mq_submit_bio ->
 * blk_mq_get_request -> mq_ops->queue_rq (간접 호출; NVMe PCIe 면 nvme_queue_rq -> nvme_sq_copy_cmd -> nvme_write_sq_db) 순으로
 * 진행될 것으로 추정된다 - 추정). 실행 컨텍스트는 디스크 스캔을 수행하는
 * 커널 프로세스 컨텍스트이며, 인터럽트 컨텍스트에서는 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈: block/partitions/check.h가 선언하는 read_part_sector(),
 * put_dev_sector(), put_partition(), struct parsed_partitions를 사용해
 * 섹터 입출력과 파티션 등록을 수행한다. mac.h가 정의하는
 * struct mac_partition, struct mac_driver_desc, MAC_PARTITION_MAGIC/
 * MAC_DRIVER_MAGIC/MAC_STATUS_BOOTABLE/APPLE_AUX_TYPE에 전적으로
 * 의존한다. CONFIG_PPC_PMAC가 켜진 경우 arch/powerpc가 제공하는
 * <asm/machdep.h>의 note_bootable_part()를 호출해 부팅 서브시스템에
 * 결과를 전달한다. 데이터 흐름은 "블록 장치의 원시 섹터 바이트(빅엔디안)
 * -> read_part_sector()가 채운 커널 버퍼 -> be16_to_cpu()/be32_to_cpu()로
 * 호스트 엔디안 변환 -> parsed_partitions 구조체의 시작 섹터/크기 필드"
 * 순서로 이어지며, 이 정보는 이후 커널 파티션 디바이스(/dev/<disk>N 형태)
 * 생성에 쓰인다.
 *
 * === 주요 함수/구조체 요약 ===
 * - mac_partition(): 이 파일의 유일한 공개 진입점. 0번 블록을 읽어
 *   드라이버 서술자를 검증하고, 파티션 맵을 순회하며 각 파티션을
 *   parsed_partitions에 등록한다. 반환값 1/0/-1로 각각 "Mac 파티션 인식
 *   성공"/"Mac 디스크 아님"/"I/O 오류 또는 비정상 블록 크기"를 구분한다.
 * - mac_fix_string() (CONFIG_PPC_PMAC 전용 static 함수): 고정폭 문자열
 *   필드의 우측 공백을 제거해 strcasecmp()/strncmp() 비교가 정확히
 *   동작하도록 만드는 보조 함수.
 * - struct mac_partition (mac.h): 온디스크 파티션 엔트리 1개의 레이아웃.
 * - struct mac_driver_desc (mac.h): 0번 블록에 위치한 드라이버 서술자로,
 *   블록 크기(block_size)와 매직 넘버를 담는다.
 */

#include <linux/ctype.h> /* [한국어] isprint 등 — 파티션 이름 문자열을 다듬을 때 쓴다 */
#include "check.h" /* read_part_sector(), parsed_partitions, put_partition 선언; NVMe namespace 파싱 인프라 */
#include "mac.h" /* mac_partition(), mac_driver_desc, MAC_* 매직 정의; NVMe LBA 0/APM 엔트리 레이아웃 */

#ifdef CONFIG_PPC_PMAC /* [한국어] PowerMac 전용 코드 — 이 아키텍처에서만 부팅 파티션을 펌웨어에 보고한다 */
#include <asm/machdep.h>	/* [한국어] PowerMac 펌웨어 연동 헤더. 아래 note_bootable_part() 를 통해
				 * "Open Firmware 가 지정한 부팅 파티션이 어느 것인가"를 커널에 알린다.
				 * CONFIG_PPC_PMAC 에서만 필요하므로 #ifdef 안에 들어 있다 */
extern void note_bootable_part(dev_t dev, int part, int goodness); /* PowerMac용 루트 파티션 알림; NVMe controller 무관 */
#endif

/*
 * Code to understand MacOS partition tables.
 */

#ifdef CONFIG_PPC_PMAC
/*
 * [한국어]
 * mac_fix_string() - 고정폭 문자열 필드 우측의 공백 패딩을 제거한다.
 *
 * @stg: 우측 공백을 제거할 대상 버퍼. mac_partition()이 온디스크
 *       struct mac_partition의 processor[16]/name[32]/type[32] 필드
 *       포인터를 그대로 전달한다 (널 종료가 보장되지 않는 고정폭 버퍼).
 * @len: stg 버퍼의 전체 길이(바이트). 호출부에서 16 또는 32가 전달된다.
 * @return: 없음(void). stg 버퍼를 제자리(in-place)에서 수정한다.
 *
 * APM(Apple Partition Map) 온디스크 포맷은 name/type/processor 필드를
 * 고정 길이 문자 배열로 저장하며, 실제 문자열보다 짧을 경우 남는 공간을
 * 공백(' ')으로 채운다. 이 상태로 strcasecmp()/strncmp() 등을 사용하면
 * "root"와 "root            "(공백 패딩)가 다른 문자열로 취급되어 비교가
 * 실패하므로, 비교 전에 우측 공백을 널 문자('\0')로 덮어써 실질적인
 * 문자열 종료 지점을 만들어 주는 것이 이 함수의 목적이다.
 * 동작 과정: len-1번째(마지막) 바이트부터 역방향으로 훑으며 공백을
 * 만나는 동안 계속 '\0'으로 덮어쓰고, 공백이 아닌 문자를 만나거나
 * 인덱스가 0 미만이 되면 멈춘다. 즉 문자열 끝의 연속된 공백만 제거하고
 * 중간에 있는 공백은 건드리지 않는다.
 * 실행 컨텍스트: mac_partition() 호출 스레드 내에서 동기적으로 실행되는
 * 순수 CPU 연산이며, 별도의 잠금이나 재진입 문제는 없다(각 호출은 서로
 * 다른 온디스크 버퍼 영역을 대상으로 하며 공유 상태가 없다).
 * 호출자: mac_partition()이 CONFIG_PPC_PMAC 블록 안에서
 * part->processor/name/type 각각에 대해 한 번씩 호출한다.
 * 피호출자: 없음(단순 반복문, 외부 함수 호출 없음).
 * 에러 처리: 실패 개념이 없는 단순 문자열 가공 함수이므로 에러 경로가
 * 존재하지 않는다.
 *
 * 호출 체인:
 *   mac_partition() → [mac_fix_string()] → (반환값 없음, stg 버퍼 직접 수정)
 */
static inline void mac_fix_string(char *stg, int len)
{
	int i;

	/* [한국어] 파티션 이름의 우측 공백을 잘라낸다 — Mac 파티션 맵은 이름을 고정 길이
	 * 필드에 공백으로 채워 저장한다 */
	for (i = len - 1; i >= 0 && stg[i] == ' '; i--)
		stg[i] = 0;	/* 우측 끝 공백을 '\0'로 덮어써서 문자열 종료 (NVMe queue의 데이터 버퍼와 무관) */
}
#endif

/*
 * [한국어]
 * mac_partition() - Apple Partition Map(APM)을 인식해 파티션을 등록한다.
 *
 * @state: check_partition()이 넘겨주는 parsed_partitions 컨텍스트. 대상
 *         블록 장치(gendisk)와 파티션 등록 결과를 담을 배열(state->parts[]),
 *         현재까지 처리 가능한 파티션 개수 한도(state->limit), 파티션 스캔
 *         결과를 사람이 읽을 문자열로 누적하는 seq_buf(state->pp_buf) 등을
 *         포함한다.
 * @return: 1  - Mac 파티션 테이블을 정상 인식하고 파티션 등록을 완료한 경우.
 *          0  - 매직 넘버 불일치 등으로 이 디스크가 Mac 포맷이 아니라고
 *               판단한 경우(에러가 아니라 "이 파서는 해당 없음"을 의미).
 *          -1 - 섹터 읽기 실패, 또는 block_size가 2의 거듭제곱이 아니거나
 *               파티션 엔트리가 읽은 버퍼 범위를 벗어나는 등 비정상적인
 *               상황(진짜 오류)을 만난 경우.
 *
 * 이 함수는 이 파일의 유일한 공개 심볼로, block/partitions/check.c가 관리
 * 하는 파티션 파서 테이블에 등록되어 있다. 디스크(또는 파티션이 없는
 * 블록 장치)를 처음 스캔할 때 다른 파서들과 마찬가지로 한 번 시도되며,
 * 자신이 인식할 수 없는 포맷이면 조용히 0을 반환해 다음 파서에게 기회를
 * 넘겨준다.
 * 동작 과정:
 *   1) 0번 블록을 read_part_sector()로 읽어 struct mac_driver_desc로
 *      캐스팅하고, signature가 MAC_DRIVER_MAGIC("ER")인지 확인한다.
 *   2) block_size(secsize)를 얻어 2의 거듭제곱인지 검증한다(그렇지 않으면
 *      파티션 엔트리가 섹터 경계를 넘어 read_part_sector()로 정렬된 접근이
 *      불가능해지므로 -1을 반환).
 *   3) secsize를 512바이트 단위로 내림(datasize)한 뒤 해당 블록을 다시
 *      읽어 첫 번째 mac_partition 엔트리를 찾고, 매직 넘버(0x504d, "PM")와
 *      map_count(파티션 총 개수)를 확인한다.
 *   4) slot = 1부터 map_count(또는 state->limit-1로 제한된 값)까지 순회
 *      하며 각 슬롯 위치(slot * secsize)의 섹터를 읽어 파티션 엔트리를
 *      해석하고, put_partition()으로 시작 섹터/크기를 등록한다.
 *   5) type 필드가 "Linux_RAID"면 ADDPART_FLAG_RAID를 설정한다.
 *   6) CONFIG_PPC_PMAC가 활성화된 커널에서는 각 파티션의 부팅 적합도
 *      (goodness)를 계산해 가장 적합한 파티션을 찾고, 루프가 끝난 뒤
 *      note_bootable_part()로 부팅 서브시스템에 알려준다.
 * 실행 컨텍스트: 디스크 스캔을 수행하는 단일 커널 프로세스 컨텍스트에서
 * 동기적으로 실행되며, 인터럽트 컨텍스트에서 호출되지 않는다. 이 함수
 * 내에서 사용하는 Sector sect/버퍼는 매 반복마다 put_dev_sector()로
 * 해제되므로 장기 보유되는 락이나 공유 상태는 없다(재진입 시에도 각
 * 호출은 독립된 지역 변수만 사용).
 * 호출자: block/partitions/check.c의 check_partition()이
 * rescan_partitions() 흐름 안에서 다른 파티션 파서들과 함께 순서대로
 * 이 함수를 호출한다.
 * 피호출자: read_part_sector()/put_dev_sector()(섹터 I/O),
 * be16_to_cpu()/be32_to_cpu()(엔디안 변환), is_power_of_2()/round_down()
 * (블록 크기 검증), put_partition()(파티션 등록), seq_buf_puts()(로그
 * 문자열 누적), mac_fix_string()/strcasecmp()/strncasecmp()/strncmp()/
 * strnlen()(CONFIG_PPC_PMAC 부팅 후보 판정), note_bootable_part()
 * (CONFIG_PPC_PMAC 부팅 통보).
 * 에러 처리: read_part_sector()가 NULL을 반환하면 즉시 -1로 반환한다.
 * 매직 넘버가 맞지 않으면 이미 읽은 섹터를 put_dev_sector()로 해제한 뒤
 * 0을 반환한다. 파티션 엔트리가 읽은 버퍼 범위를 벗어나면(partoffset +
 * sizeof(*part) > datasize) 버퍼를 해제하고 -1을 반환한다. 파티션 맵
 * 순회 도중 매직 넘버가 깨지면(더 이상 유효한 엔트리가 없으면) 에러로
 * 취급하지 않고 break로 루프만 종료한다.
 *
 * 호출 체인:
 *   rescan_partitions() → check_partition() → [mac_partition()] →
 *     read_part_sector() / put_dev_sector() / put_partition() /
 *     mac_fix_string() / note_bootable_part()
 */
int mac_partition(struct parsed_partitions *state)
{
	Sector sect;		/* read_part_sector()가 반환한 512바이트 섹터 버퍼 (NVMe에서 PRP/SGL로 채워진 데이터의 호스트 사본) */
	unsigned char *data;	/* sect 내부의 실제 바이트 포인터, NVMe Read 완료 후 CPU가 해석하는 메모리 주소 */
	/* 파티션 맵 순회 인덱스(slot)와 총 엔트리 수; 각 slot마다 별도의 NVMe Read가 제출될 수 있음 */
	int slot, blocks_in_map;
	/* Mac 블록 크기(secsize)를 NVMe 512B LBA 단위로 환산하기 위한 변수들 */
	unsigned secsize, datasize, partoffset;
#ifdef CONFIG_PPC_PMAC
	int found_root = 0;		/* PowerMac 부팅에 적합한 루트 파티션 슬롯 (NVMe CID/SQ 상태와 무관) */
	int found_root_goodness = 0;	/* 루트 파티션 후보의 적합도 점수 */
#endif
	struct mac_partition *part;	/* NVMe에서 읽어온 파티션 엔트리를 해석할 때 사용하는 Apple 파티션 구조체 */
	struct mac_driver_desc *md;	/* 0번 블록에 위치한 Mac 드라이버 서술자 (NVMe LBA 0에서 읽음) */

	/* Get 0th block and look at the first partition map entry. */
	/* LBA 0을 NVMe에서 읽음: read_part_sector() -> submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_sq_copy_cmd/nvme_write_sq_db(Read, SLBA=0, doorbell) */
	md = read_part_sector(state, 0, &sect);
	/* [한국어] read_part_sector() 가 NULL 이면 그 섹터를 읽지 못했다는 뜻이다
		 * (메모리 부족이거나 장치가 에러를 냈거나 범위를 벗어났거나) */
	if (!md)
		return -1;	/* NVMe Read 명령 실패 또는 메모리 부족 (CID 완료 전/후 오류 가능) */
	/* NVMe Read CQE가 성공했어도 매직 넘버 검증이 필요; MAC_DRIVER_MAGIC(0x4552) 불일치 시 Mac 디스크 아님 */
	if (be16_to_cpu(md->signature) != MAC_DRIVER_MAGIC) {
		/* LBA 0 버퍼 해제: NVMe DMA 완료된 folio 반납 */
		put_dev_sector(sect);
		return 0;	/* LBA 0에 Mac 드라이버 서술자가 없음 -> NVMe 입장에서는 일반 블록 데이터 */
	}
	/* [한국어] Mac 파티션 맵이 스스로 기록해 둔 논리 블록 크기(빅엔디언 16비트).
	 * 장치가 보고하는 논리 블록 크기와는 별개의 값으로, 맵을 해석하는 데만 쓴다 */
	secsize = be16_to_cpu(md->block_size);	/* [한국어] 이 맵이 쓰는 블록 크기. 커널이 읽어 오는 512B 섹터와 다를 수 있어
						 * 아래에서 비율을 곱해 섹터 번호로 환산한다 */
	/* 드라이버 서술자 파싱 후 버퍼 해제; 다음 read_part_sector() 전 필수 */
	put_dev_sector(sect);

	/*
	 * If the "block size" is not a power of 2, things get weird - we might
	 * end up with a partition straddling a sector boundary, so we wouldn't
	 * be able to read a partition entry with read_part_sector().
	 * Real block sizes are probably (?) powers of two, so just require
	 * that.
	 */
	/* [한국어] 아래에서 secsize/512 로 섹터 번호를 환산하므로 2의 거듭제곱이어야
	 * 시프트로 정확히 나눠떨어진다. 손상된 맵이 이상한 값을 담고 있을 수 있어 검사한다 */
	if (!is_power_of_2(secsize))
		return -1;	/* [한국어] 2의 거듭제곱이 아니면 이 맵을 해석할 수 없다 — 손상으로 보고 포기한다 */
	/* secsize를 NVMe 섹터(512B) 단위로 내림; secsize<512이면 datasize=0이 되어 후속 읽기가 실패할 수 있음 */
	datasize = round_down(secsize, 512);	/* NVMe가 반환하는 512B 섹터 단위로 내림 */
	/* datasize/512 = NVMe Read의 SLBA; LBA 0 또는 1에서 첫 Apple Partition Map 엔트리를 읽음 */
	data = read_part_sector(state, datasize / 512, &sect);
	/* [한국어] 해당 섹터를 읽지 못했다 (에러이거나 메모리 부족) */
	if (!data)
		return -1;	/* NVMe Read 실패: SQ/CQ 완료 상태 비정상 또는 메모리 할당 실패 */
	/* secsize가 512B 배수가 아닐 때 섹터 내 APM 엔트리 시작 오프셋; 512B 배수면 0 */
	partoffset = secsize % 512;	/* secsize가 512보다 클 때 첫 번째 파티션 엔트리의 섹터 내 오프셋 */
	/* sizeof(*part)는 최소 184B; NVMe가 반환한 512B 버퍼 내에서 partoffset+sizeof(*part)가 벗어나면 손상 또는 잘못된 secsize */
	if (partoffset + sizeof(*part) > datasize) {
		/* 파티션 엔트리 범위 초과로 인한 조기 반환 전 버퍼 해제 */
		put_dev_sector(sect);
		return -1;	/* NVMe에서 읽은 버퍼 범위를 벗어나는 파티션 엔트리 (데이터 손상 또는 잘못된 secsize) */
	}
	/* NVMe DMA로 채워진 버퍼에서 partoffset만큼 건너 뛴 APM 엔트리 포인터 */
	part = (struct mac_partition *) (data + partoffset);
	/* LBA datasize/512의 CQE 데이터에서 MAC_PARTITION_MAGIC(0x504d) 검증; 불일치 시 Mac 포맷 아님 */
	if (be16_to_cpu(part->signature) != MAC_PARTITION_MAGIC) {
		/* APM 매직 불일치로 스캔 중단 전 버퍼 해제 */
		put_dev_sector(sect);
		return 0;	/* LBA 0 주변에 Apple Partition Map 시그니처가 없음 -> NVMe SSD는 Mac 포맷이 아님 */
	}
	/* map_count는 APM 엔트리 오프셋 4의 big-endian 32비트; NVMe LBA 0 또는 첫 엔트리에서 읽어낸 값 */
	blocks_in_map = be32_to_cpu(part->map_count);	/* Apple Partition Map의 총 엔트리 수; NVMe가 LBA 0에서 반환한 데이터의 하위 필드 */
	/* DISK_MAX_PARTS는 커널의 최대 파티션 수; NVMe namespace 용량과 무관하게 적용 */
	if (blocks_in_map < 0 || blocks_in_map >= DISK_MAX_PARTS) {
		/* 비정상적인 map_count로 스캔 중단 전 버퍼 해제 */
		put_dev_sector(sect);
		return 0;	/* 파티션 개수가 너무 많아 블록 계층이 관리할 수 없으므로 NVMe 입장에서는 단일 디스크로 취급 */
	}

	/* state->limit은 gendisk가 수용 가능한 최대 파티션 수; 초과 시 등록하지 않음 */
	if (blocks_in_map >= state->limit)
		blocks_in_map = state->limit - 1;	/* parsed_partitions 구조체의 한계를 NVMe LBA와 무관하게 맞춤 */

	/* 파티션 스캔 결과 문자열 버퍼에 " [mac]" 기록; 이후 dmesg 등에서 "nvme0n1: [mac] p1 p2 ..." 형태로 출력 */
	seq_buf_puts(&state->pp_buf, " [mac]");
	/* [한국어] APM 은 MBR 확장 파티션(EBR)처럼 연결 리스트를 따라가는 구조가 아니라
	 * 엔트리가 평면으로 나열된 구조라, blocks_in_map 만큼만 순회하면 끝난다 */
	for (slot = 1; slot <= blocks_in_map; ++slot) {
		/* 현재 파티션 엔트리의 Mac 논리 블록 오프셋; CHS 변환 없이 /512로 NVMe LBA로 직접 환산 */
		int pos = slot * secsize;	/* 현재 파티션 엔트리의 Mac 논리 블록 오프셋 */
		/* 이전 read_part_sector()로 얻은 folio 해제; NVMe DMA 버퍼 수명 종료 */
		put_dev_sector(sect);
		/* pos/512 = NVMe Read SLBA; APM은 CHS 대신 논리 블록 번호를 사용하므로 bio remap 시 NVMe SLBA로 직접 대응 */
		data = read_part_sector(state, pos/512, &sect);
		/* [한국어] 이 엔트리를 담은 섹터를 읽지 못했다 */
		if (!data)
			return -1;		/* [한국어] 읽기 실패 — 맵을 신뢰할 수 없으므로 파싱을 중단한다 */
		/* NVMe가 반환한 512B 버퍼 내 pos%512 위치의 APM 엔트리; 이 위치는 NVMe PRP/SGL 버퍼의 바이트 오프셋 */
		part = (struct mac_partition *) (data + pos%512);
		/* 예상된 APM 시그니처(0x504d)가 아니면 중단; NVMe에서 읽은 데이터의 나머지는 무시됨 */
		if (be16_to_cpu(part->signature) != MAC_PARTITION_MAGIC)
			/* APM 시그니처 깨짐: 이 지점 이후의 NVMe Read는 불필요하므로 루프 탈출 */
			break;			/* 예상된 Apple Partition Map 시그니처가 아니면 중단; NVMe 데이터는 여기까지만 유효 */
		/* [한국어] 파싱 결과를 state->parts[] 에 기록해 둔다. 실제 파티션 블록 디바이스는
		 * 파싱이 모두 끝난 뒤 상위(bdev_disk_changed 경로)가 만든다 — 이 파일은 해석만 한다 */
		put_partition(state, slot,
			/* start_block은 APM 엔트리 오프셋 8의 be32; Mac 블록 단위를 NVMe 512B LBA 단위로 환산 -> bio remap 후 SLBA에 더해짐 */
			be32_to_cpu(part->start_block) * (secsize/512),	/* NVMe LBA = Mac 시작 블록 * (Mac 블록 크기 / 512B) */
			/* block_count는 APM 엔트리 오프셋 12의 be32; 파티션 크기를 NVMe 섹터 수로 환산 -> gendisk 파티션 크기 결정 */
			be32_to_cpu(part->block_count) * (secsize/512));	/* 해당 파티션의 NVMe 상에서의 총 섹터 수 */

		/* part->type[0..31]은 APM 엔트리 오프셋 16에 위치; NVMe에서 읽은 메타데이터 기반 OS 수준 플래그 설정 */
		if (!strncasecmp(part->type, "Linux_RAID", 10))
			/* RAID 파티션 플래그 기록; NVMe SQ/CQ 명령 형식에는 영향 없음 */
			state->parts[slot].flags = ADDPART_FLAG_RAID;	/* 파티션 플래그 설정; NVMe SQ/CQ에는 영향 없음 */
#ifdef CONFIG_PPC_PMAC
		/*
		 * If this is the first bootable partition, tell the
		 * setup code, in case it wants to make this the root.
		 */
		/* PowerMac 플랫폼에서만 실행; NVMe 컨트롤러/펌웨어와 무관한 부팅 정책 */
		if (machine_is(powermac)) {
			int goodness = 0;

			/* part->processor[0..15] 필드 정리; APM 엔트리 오프셋 112 (추정) */
			mac_fix_string(part->processor, 16);
			/* part->name[0..31] 필드 정리; APM 엔트리 오프셋 48 */
			mac_fix_string(part->name, 32);
			/* part->type[0..31] 필드 정리; APM 엔트리 오프셋 16 */
			mac_fix_string(part->type, 32);					
		    
			/* part->status는 APM 엔트리 오프셋 44의 be32; MAC_STATUS_BOOTABLE 비트 검사 */
			if ((be32_to_cpu(part->status) & MAC_STATUS_BOOTABLE)
			    && strcasecmp(part->processor, "powerpc") == 0)
				/* PowerPC 프로세서 매칭 시 부팅 적합도 증가; NVMe I/O 경로와 무관 */
				goodness++;

			/* part->type은 APM 엔트리 오프셋 16의 32바이트 문자열; Apple_UNIX_SVR2/Linux 등 판별 */
			if (strcasecmp(part->type, "Apple_UNIX_SVR2") == 0
			    || (strncasecmp(part->type, "Linux", 5) == 0
			        && strcasecmp(part->type, "Linux_swap") != 0)) {
				int i, l;

				/* Unix/Linux 데이터 파티션 확인 시 부팅 적합도 증가 */
				goodness++;
				/* 파티션 이름 길이 측정; APM 엔트리 오프셋 48의 name[32] 필드 */
				l = strnlen(part->name, sizeof(part->name));
				/* 이름이 "/"이면 루트 파티션으로 간주; NVMe LBA remap과 무관 */
				if (strncmp(part->name, "/", sizeof(part->name)) == 0)
					goodness++;
				/* 이름에서 "root" 서브스트링 검색; 순전히 OS 부팅 정책 (추정) */
				for (i = 0; i <= l - 4; ++i) {
					/* "root" 서브스트링 발견 시 부팅 적합도 크게 증가 */
					if (strncasecmp(part->name + i, "root",
						     4) == 0) {
						/* 루트 후보 가중치 부여; NVMe I/O 경로와 무관 */
						goodness += 2;
						break;
					}
				}
				/* 이름이 "swap"이면 적합도 감소; NVMe page cache/SWAP I/O 경로와 무관 */
				if (strncasecmp(part->name, "swap", 4) == 0)
					/* 스왑 파티션은 루트 후보에서 제외 */
					goodness--;
			}

			/* 현재까지 가장 부팅에 적합한 파티션 후보 갱신 */
			if (goodness > found_root_goodness) {
				/* [한국어] 루트로 표시된 파티션의 슬롯 번호를 기록한다. 이 번호가 곧 장치 이름 끝의
			 * 파티션 번호(예: nvme0n1p2 의 2)가 된다 */
				found_root = slot;
				/* 현재 최고 부팅 적합도 점수 갱신 */
				found_root_goodness = goodness;
			}
		}
#endif /* CONFIG_PPC_PMAC */
	}
#ifdef CONFIG_PPC_PMAC
	/* found_root이 설정된 경우 PowerMac 부팅 코드에 통보; NVMe queue에는 영향 없음 */
	if (found_root_goodness)
		/* note_bootable_part는 PowerMac 고유; NVMe SQ/CQ doorbell과 무관 */
		note_bootable_part(state->disk->part0->bd_dev, found_root,
				   found_root_goodness);
#endif

	/* for 루프 종료 후 마지막 read_part_sector() folio 해제; 모든 NVMe Read 버퍼 반납 */
	put_dev_sector(sect);
	/* 파티션 목록 출력의 줄바꿈; NVMe I/O와 무관 */
	seq_buf_puts(&state->pp_buf, "\n");
	/* Mac 파티션 등록 완료; 이후 bio가 partition remap을 거쳐 submit_bio -> blk_mq_submit_bio -> nvme_queue_rq -> nvme_sq_copy_cmd/nvme_write_sq_db(SLBA = 파티션 시작 LBA + bi_sector)로 전달됨 */
	return 1;	/* Mac 파티션 등록 완료; 이후 blk_mq_submit_bio -> nvme_queue_rq 경로는 파티션 오프셋을 반영한 LBA로 변환됨 */
}
