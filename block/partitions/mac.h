/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  fs/partitions/mac.h
 */

/*
 * [한국어 설명] Apple Partition Map(APM, 구형 68k/PowerPC Mac OS 파티션 스킴)의
 * 온디스크 자료구조 정의 헤더 (mac.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 block/partitions/mac.c가 파싱하는 두 가지 온디스크 구조체
 * (struct mac_partition, struct mac_driver_desc)와 관련 매직 넘버/플래그
 * 매크로를 정의한다. 두 구조체 모두 디스크(블록 장치) 위에 빅엔디안으로
 * 저장된 값을 그대로 매핑하기 위한 것이며, 필드 타입이 모두 __beN(빅엔디안
 * N비트) 계열로 선언되어 있다. 이 헤더 자체는 실행 코드를 담고 있지 않고,
 * mac.c가 be16_to_cpu()/be32_to_cpu()로 변환해 사용할 "온디스크 포맷 계약"
 * 역할만 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * mac.c의 mac_partition()이 read_part_sector()로 읽어온 원시 섹터 버퍼를
 * 이 헤더가 정의한 구조체 포인터로 캐스팅해서 해석한다. 즉 이 헤더는
 * "디스크 바이트 배열 -> 커널 내부 구조체" 변환의 타입 계약에 해당하며,
 * 실제 실행 흐름상으로는 rescan_partitions() -> check_partition() ->
 * mac_partition() 호출 체인 중 mac_partition() 내부에서만 사용된다.
 * 이 헤더가 정의하는 자료구조는 커널 프로세스 컨텍스트(디스크 스캔을
 * 수행하는 프로세스, 예: 모듈 로드/udev 스레드)에서만 해석되며, 별도의
 * 실행 컨텍스트(인터럽트, GPU 등)에서는 쓰이지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * mac.c가 이 헤더를 include(#include "mac.h")하여 구조체/매크로 정의를
 * 가져다 쓴다. 반대로 이 헤더는 mac.c나 check.h에 의존하지 않는 순수
 * 자료구조 정의 파일이다. 데이터 흐름 관점에서는 "블록 장치의 물리
 * 섹터(빅엔디안 바이트) -> read_part_sector()가 채운 버퍼 -> 이 헤더의
 * 구조체로 캐스팅 -> be16_to_cpu()/be32_to_cpu() 변환 -> parsed_partitions"
 * 순서로 데이터가 흐른다. struct mac_driver_desc는 항상 디스크의 0번
 * 블록에서, struct mac_partition은 그 이후(secsize/512번째 블록부터)
 * 반복 등장하는 파티션 맵 엔트리에서 읽힌다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct mac_driver_desc: 0번 블록에 위치하는 드라이버 서술자.
 *   매직 넘버(signature)와 논리 블록 크기(block_size)를 담고 있으며,
 *   mac_partition()이 이 블록 크기를 기준으로 이후 파티션 맵 엔트리의
 *   위치를 계산한다.
 * - struct mac_partition: 파티션 맵의 엔트리 하나. 파티션의 절대 시작
 *   블록(start_block), 크기(block_count), 이름(name), 타입 문자열(type),
 *   부팅 관련 필드(boot_*, status, processor) 등을 담는다.
 * - MAC_PARTITION_MAGIC / MAC_DRIVER_MAGIC: 각각 파티션 엔트리와 드라이버
 *   서술자가 유효한지 확인하는 매직 넘버("PM", "ER"의 ASCII 코드에서 유래).
 * - MAC_STATUS_BOOTABLE: mac_partition.status 필드의 부팅 가능 비트.
 * - APPLE_AUX_TYPE: A/UX(Apple Unix) 파티션을 식별하는 타입 문자열 상수.
 */

/*
 * [한국어]
 * MAC_PARTITION_MAGIC - Apple Partition Map 엔트리의 매직 넘버("PM").
 *
 * 설정자: 이 매크로 자체는 컴파일 타임 상수라 "설정"되지 않는다. 온디스크
 *         값은 파티션 맵을 최초로 기록한 Mac OS 유틸리티(pdisk 등)가 기록.
 * 읽는 자: mac.c의 mac_partition()이 be16_to_cpu(part->signature)와 비교하여
 *          해당 섹터가 유효한 APM 엔트리인지 판별한다. 불일치 시 파티션
 *          스캔을 중단(스캔 시작 시 0 리턴, 루프 중이면 break)한다.
 * 값 범위: 항상 0x504d 고정. ASCII로 'P'(0x50) 'M'(0x4d)에 해당하며,
 *          디스크에는 빅엔디안 2바이트로 기록되어 있다.
 * 동기화: 컴파일 타임 상수이므로 동기화 대상이 아니다.
 */
#define MAC_PARTITION_MAGIC	0x504d

/* type field value for A/UX or other Unix partitions */
/*
 * [한국어]
 * APPLE_AUX_TYPE - mac_partition.type 필드와 비교되는 문자열 상수.
 *
 * 설정자: 컴파일 타임 문자열 리터럴이므로 런타임 설정자는 없다.
 * 읽는 자: mac.c의 mac_partition()이 CONFIG_PPC_PMAC 블록에서
 *          strcasecmp(part->type, "Apple_UNIX_SVR2")로 직접 비교한다
 *          (이 매크로가 아니라 문자열 리터럴로 재입력되어 있음에 유의).
 *          A/UX(Apple Unix)나 Linux 데이터 파티션을 찾아 PowerMac 부팅
 *          루트 후보의 적합도 점수를 매기는 데 쓰인다.
 * 값 범위: "Apple_UNIX_SVR2" 고정 문자열(널 종료, mac_partition.type[32]와
 *          strcasecmp()로 대소문자 구분 없이 비교됨).
 * 동기화: 컴파일 타임 상수, 동기화 불필요.
 */
#define APPLE_AUX_TYPE	"Apple_UNIX_SVR2"

/*
 * [한국어]
 * struct mac_partition - Apple Partition Map(APM)의 파티션 엔트리 1개.
 *
 * 디스크 위에 빅엔디안으로 직렬화된 온디스크 포맷이며, mac_partition()이
 * read_part_sector()로 읽어온 버퍼를 이 구조체 포인터로 캐스팅해서 해석한다
 * (struct mac_partition *part = (struct mac_partition *)(data + offset)).
 * 파티션 맵은 mac_driver_desc.block_size(secsize) 단위로 연속 배치되며,
 * 첫 엔트리(map_count 필드를 가진 엔트리)부터 시작해 map_count개만큼
 * 반복된다. 모든 멀티바이트 필드는 __beN(빅엔디안) 타입으로 선언되어
 * 있으므로 반드시 be16_to_cpu()/be32_to_cpu()로 변환한 뒤 사용해야 한다.
 */
struct mac_partition {
	__be16	signature;	/* expected to be MAC_PARTITION_MAGIC */
	/* [한국어] APM 엔트리의 매직 넘버 필드 (오프셋 0x00, 크기 2바이트, 빅엔디안).
	 * 설정자: 온디스크 값이므로 커널이 설정하지 않는다 - 디스크에 이미
	 *         기록된 값을 read_part_sector()가 그대로 읽어올 뿐이다.
	 * 읽는 자: mac_partition()이 be16_to_cpu(part->signature)로 변환한 뒤
	 *          MAC_PARTITION_MAGIC과 비교한다. 최초 파티션 맵 엔트리를 찾을
	 *          때(datasize/512 블록)와, 이후 슬롯을 순회할 때(pos/512 블록)
	 *          모두 이 필드를 검사해 파티션 맵의 끝(비-APM 데이터)을 감지한다.
	 * 값 범위: 유효한 엔트리라면 항상 MAC_PARTITION_MAGIC(0x504d)와 같아야 함.
	 * 동기화: 별도 락 없음 - read_part_sector()가 반환한 버퍼는 단일 스캔
	 *         호출(mac_partition 1회 실행) 동안만 유효하며 put_dev_sector()로
	 *         즉시 해제되므로 동시 접근이 발생하지 않는다. */
	__be16	res1;
	/* [한국어] APM 엔트리의 예약(reserved) 필드 (오프셋 0x02, 크기 2바이트).
	 * 설정자: 온디스크 값 그대로이며 커널이 쓰지 않는다.
	 * 읽는 자: mac.c의 어떤 함수도 이 필드를 읽지 않는다 - signature와
	 *          map_count 사이의 패딩을 구조체 레이아웃상 맞추기 위해서만
	 *          존재하는 것으로 추정된다 (추정).
	 * 값 범위: 규격상 사용되지 않는 영역으로 임의의 값이 들어있을 수 있다.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	map_count;	/* # blocks in partition map */
	/* [한국어] 파티션 맵 전체가 차지하는 엔트리(블록) 개수 (오프셋 0x04, 4바이트).
	 * 설정자: 온디스크 값 - 파티션 맵을 최초 생성한 도구(pdisk 등)가 기록.
	 * 읽는 자: mac_partition()이 be32_to_cpu(part->map_count)로 읽어
	 *          blocks_in_map에 저장하고, 이후 for (slot = 1; slot <=
	 *          blocks_in_map; ++slot) 루프의 상한으로 사용한다. 이 값이
	 *          parsed_partitions.limit을 넘으면 limit-1로 잘라낸다.
	 * 값 범위: 0 이상. mac_partition()에서 DISK_MAX_PARTS 미만이어야 유효한
	 *          것으로 간주되며(blocks_in_map < 0 || >= DISK_MAX_PARTS 검사),
	 *          그 이상이거나 음수면 손상된 것으로 보고 스캔을 중단한다.
	 * 동기화: 읽기 전용 스캔이므로 별도 동기화 불필요. */
	__be32	start_block;	/* absolute starting block # of partition */
	/* [한국어] 디스크 전체를 기준으로 한 파티션의 절대 시작 블록 번호
	 * (오프셋 0x08, 4바이트). 단위는 mac_driver_desc.block_size(secsize)이며
	 * 512바이트가 아닐 수 있다.
	 * 설정자: 온디스크 값 - 파티션 생성 도구가 기록.
	 * 읽는 자: mac_partition()이 be32_to_cpu(part->start_block) *
	 *          (secsize/512) 연산으로 secsize 단위 블록 번호를 커널
	 *          표준 512바이트 섹터 단위로 환산한 뒤 put_partition()의
	 *          시작 섹터 인자로 전달한다.
	 * 값 범위: 0 이상, 디스크 총 블록 수 이내 (커널이 별도 상한 검증을
	 *          하지는 않는다).
	 * 동기화: 읽기 전용, 별도 동기화 불필요. */
	__be32	block_count;	/* number of blocks in partition */
	/* [한국어] 파티션의 크기를 블록 수로 표현 (오프셋 0x0c, 4바이트).
	 * 단위는 start_block과 마찬가지로 secsize(mac_driver_desc.block_size).
	 * 설정자: 온디스크 값 - 파티션 생성 도구가 기록.
	 * 읽는 자: mac_partition()이 be32_to_cpu(part->block_count) *
	 *          (secsize/512)로 환산해 put_partition()의 크기(512바이트
	 *          섹터 개수) 인자로 전달한다. 이 값이 parsed_partitions의
	 *          섹터 배열에 그대로 기록되어 이후 블록 계층의 파티션
	 *          경계(bio 리매핑 범위) 판단에 쓰인다.
	 * 값 범위: 0 이상.
	 * 동기화: 읽기 전용, 별도 동기화 불필요. */
	char	name[32];	/* partition name */
	/* [한국어] 파티션 이름 문자열, 최대 32바이트, 널 종료 보장 없음
	 * (오프셋 0x10). 예: "Apple_HFS", "untitled" 등 사용자가 붙인 레이블.
	 * 설정자: 온디스크 값 - 사용자가 파티션 생성 시 지정.
	 * 읽는 자: mac_partition()이 strnlen()/strncmp()/strncasecmp()로
	 *          "/"(루트) 또는 "root"/"swap" 부분 문자열을 검사해 PowerMac
	 *          부팅 루트 후보의 적합도(goodness) 점수에 반영한다.
	 * 값 범위: 임의의 ASCII 문자열. 끝에 공백이 패딩되어 있을 수 있어
	 *          CONFIG_PPC_PMAC 경로에서는 mac_fix_string()으로 우측
	 *          공백을 제거한 뒤 비교한다.
	 * 동기화: 읽기 전용, 별도 동기화 불필요. */
	char	type[32];	/* string type description */
	/* [한국어] 파티션 타입을 나타내는 문자열, 최대 32바이트 (오프셋 0x30).
	 * 예: "Apple_UNIX_SVR2", "Linux_RAID", "Linux_swap", "Apple_HFS" 등.
	 * 설정자: 온디스크 값 - 파티션 생성 도구가 파티션 종류에 맞춰 기록.
	 * 읽는 자: mac_partition()이 strncasecmp(part->type, "Linux_RAID", 10)로
	 *          RAID 파티션 여부를 판별해 ADDPART_FLAG_RAID 플래그를 설정하고,
	 *          CONFIG_PPC_PMAC 경로에서는 "Apple_UNIX_SVR2"/"Linux"* 문자열과
	 *          비교해 부팅 루트 후보 적합도를 계산한다.
	 * 값 범위: 임의의 ASCII 문자열.
	 * 동기화: 읽기 전용, 별도 동기화 불필요. */
	__be32	data_start;	/* rel block # of first data block */
	/* [한국어] 파티션 내부에서 실제 데이터 영역이 시작하는 상대 블록 번호
	 * (오프셋 0x50, 4바이트). start_block으로부터의 오프셋이다.
	 * 설정자: 온디스크 값.
	 * 읽는 자: mac.c의 어떤 함수도 현재 이 필드를 읽지 않는다 - 파티션
	 *          전체(start_block~start_block+block_count)를 그대로
	 *          커널 파티션으로 등록하기 때문으로 추정된다 (추정).
	 * 값 범위: 0 이상.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	data_count;	/* number of data blocks */
	/* [한국어] 실제 데이터 블록 수 (오프셋 0x54, 4바이트).
	 * 설정자: 온디스크 값.
	 * 읽는 자: mac.c에서 현재 참조하지 않는다 (추정, data_start와 동일한
	 *          이유).
	 * 값 범위: 0 이상.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	status;		/* partition status bits */
	/* [한국어] 파티션 상태 비트 필드 (오프셋 0x58, 4바이트).
	 * 설정자: 온디스크 값 - 파티션 생성 도구가 부팅 가능 여부 등을 기록.
	 * 읽는 자: mac_partition()이 CONFIG_PPC_PMAC 경로에서
	 *          be32_to_cpu(part->status) & MAC_STATUS_BOOTABLE로 부팅
	 *          가능 비트를 검사하고, processor 필드가 "powerpc"와 일치하면
	 *          부팅 루트 후보 적합도를 1 증가시킨다.
	 * 값 범위: 비트마스크. 이 파일에서 의미가 정의된 비트는
	 *          MAC_STATUS_BOOTABLE(비트 3, 값 8)뿐이다.
	 * 동기화: 읽기 전용, 별도 동기화 불필요. */
	__be32	boot_start;
	/* [한국어] 부트스트랩 코드가 위치한 상대 블록 번호 (오프셋 0x5c, 4바이트).
	 * 설정자: 온디스크 값 - 부팅 가능 파티션에만 의미가 있다.
	 * 읽는 자: mac.c에서 현재 참조하지 않는다 - 실제 부트 코드 로드는
	 *          이 파일의 책임이 아니라 펌웨어/부트로더의 몫으로 추정된다
	 *          (추정).
	 * 값 범위: 0 이상.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	boot_size;
	/* [한국어] 부트스트랩 코드의 크기(블록 수) (오프셋 0x60, 4바이트).
	 * 설정자: 온디스크 값.
	 * 읽는 자: mac.c에서 현재 참조하지 않는다 (추정, boot_start와 동일한
	 *          이유).
	 * 값 범위: 0 이상.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	boot_load;
	/* [한국어] 부트 코드를 메모리에 적재할 주소 (오프셋 0x64, 4바이트).
	 * 설정자: 온디스크 값.
	 * 읽는 자: mac.c에서 참조하지 않는다 (추정).
	 * 값 범위: 아키텍처/펌웨어에 의미가 정의된 메모리 주소 값.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	boot_load2;
	/* [한국어] boot_load의 확장(상위 비트 등으로 추정) 필드
	 * (오프셋 0x68, 4바이트). 오래된 Mac OS 툴체인의 32비트 주소 공간
	 * 확장 목적으로 추정된다 (추정).
	 * 설정자: 온디스크 값.
	 * 읽는 자: mac.c에서 참조하지 않는다 (추정).
	 * 값 범위: boot_load와 결합되어 해석되는 것으로 추정 (추정).
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	boot_entry;
	/* [한국어] 부트 코드의 진입점(entry point) 주소 (오프셋 0x6c, 4바이트).
	 * 설정자: 온디스크 값.
	 * 읽는 자: mac.c에서 참조하지 않는다 (추정).
	 * 값 범위: 메모리 주소 값.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	boot_entry2;
	/* [한국어] boot_entry의 확장 필드로 추정 (오프셋 0x70, 4바이트) (추정).
	 * 설정자: 온디스크 값.
	 * 읽는 자: mac.c에서 참조하지 않는다 (추정).
	 * 값 범위: boot_entry와 결합되어 해석되는 것으로 추정 (추정).
	 * 동기화: 사용되지 않으므로 해당 없음. */
	__be32	boot_cksum;
	/* [한국어] 부트 코드의 체크섬 (오프셋 0x74, 4바이트). 펌웨어가 부트
	 * 코드를 로드하기 전 무결성을 검증하는 데 쓰였을 것으로 추정된다.
	 * 설정자: 온디스크 값 - 파티션/부트 코드 생성 도구가 계산해 기록.
	 * 읽는 자: mac.c에서 참조하지 않는다 (추정, 검증은 펌웨어의 몫으로
	 *          추정).
	 * 값 범위: 체크섬 알고리즘에 따른 임의의 32비트 값.
	 * 동기화: 사용되지 않으므로 해당 없음. */
	char	processor[16];	/* identifies ISA of boot */
	/* [한국어] 부트 코드가 대상으로 하는 프로세서 ISA(명령어 집합) 이름
	 * 문자열, 최대 16바이트 (오프셋 0x78). 예: "powerpc", "68k" 등.
	 * 설정자: 온디스크 값 - 파티션 생성 도구가 기록.
	 * 읽는 자: mac_partition()이 CONFIG_PPC_PMAC 경로에서
	 *          mac_fix_string(part->processor, 16)으로 우측 공백을 제거한
	 *          뒤 strcasecmp(part->processor, "powerpc")로 비교, 부팅
	 *          적합도 점수에 반영한다.
	 * 값 범위: 임의의 ASCII 문자열, 우측 공백 패딩 가능.
	 * 동기화: 읽기 전용, 별도 동기화 불필요. */
	/* there is more stuff after this that we don't need */
	/* [한국어] 이 시점 이후에도 APM 규격상 필드가 더 있으나(예약 영역 등),
	 * 리눅스 파티션 인식에는 필요하지 않아 이 구조체에 포함하지 않았다.
	 * 이 구조체 크기(sizeof(struct mac_partition))가 실제 온디스크 엔트리
	 * 크기보다 작을 수 있으므로, 다음 엔트리로 이동할 때는 이 구조체
	 * 크기가 아니라 mac_driver_desc.block_size(secsize)를 기준으로
	 * 오프셋을 계산해야 한다 - 실제로 mac_partition()의 "pos = slot *
	 * secsize" 계산이 바로 이 점을 반영한 것이다 (추정). */
};

/*
 * [한국어]
 * MAC_STATUS_BOOTABLE - mac_partition.status 필드에서 "부팅 가능"을
 * 나타내는 비트 마스크(값 8, 비트 인덱스 3).
 *
 * 설정자: 상수 매크로. 온디스크 status 값 자체는 파티션 생성 도구가 설정.
 * 읽는 자: mac_partition()이 be32_to_cpu(part->status) & MAC_STATUS_BOOTABLE
 *          연산으로 비트가 켜져 있는지 검사해 PowerMac 부팅 루트 후보
 *          적합도 점수 계산에 사용한다.
 * 값 범위: 8(0b1000) 고정.
 * 동기화: 컴파일 타임 상수, 동기화 불필요.
 */
#define MAC_STATUS_BOOTABLE	8	/* partition is bootable */

/*
 * [한국어]
 * MAC_DRIVER_MAGIC - mac_driver_desc.signature의 기대값("ER").
 *
 * 설정자: 상수. 온디스크 값은 디스크를 초기화한 Mac OS 드라이버 설치
 *         유틸리티가 기록한다.
 * 읽는 자: mac_partition()이 0번 블록을 읽은 뒤
 *          be16_to_cpu(md->signature) != MAC_DRIVER_MAGIC 비교로 이
 *          디스크가 애초에 Mac 포맷인지를 가장 먼저 판별한다. 불일치 시
 *          즉시 0을 반환해 "Mac 디스크 아님"으로 처리한다.
 * 값 범위: 0x4552 고정 (ASCII 'E'(0x45) 'R'(0x52)의 빅엔디안 조합).
 * 동기화: 컴파일 타임 상수, 동기화 불필요.
 */
#define MAC_DRIVER_MAGIC	0x4552

/* Driver descriptor structure, in block 0 */
/*
 * [한국어]
 * struct mac_driver_desc - 디스크 0번 블록에 위치하는 드라이버 서술자.
 *
 * mac_partition()이 가장 먼저 read_part_sector(state, 0, &sect)로 읽어
 * 이 구조체로 캐스팅하는 대상이다. signature로 Mac 포맷 여부를 확인하고,
 * block_size(secsize)로 이후 파티션 맵 엔트리들의 섹터 오프셋 계산 단위를
 * 결정한다. 이 구조체 이후에도 드라이버 파티션 목록 등 추가 필드가 더
 * 있으나(아래 block_count 필드 주석 참고), 리눅스 파티션 파싱에는
 * 불필요하여 정의하지 않았다.
 */
struct mac_driver_desc {
	__be16	signature;	/* expected to be MAC_DRIVER_MAGIC */
	/* [한국어] 0번 블록의 매직 넘버 필드 (오프셋 0x00, 2바이트, 빅엔디안).
	 * 설정자: 온디스크 값 - Mac OS 드라이버 설치 유틸리티가 기록.
	 * 읽는 자: mac_partition()이 be16_to_cpu(md->signature)로 변환 후
	 *          MAC_DRIVER_MAGIC과 비교. 불일치 시 put_dev_sector(sect) 후
	 *          0을 반환하여 스캔을 조기 종료한다.
	 * 값 범위: 유효한 디스크라면 MAC_DRIVER_MAGIC(0x4552)와 같아야 한다.
	 * 동기화: 읽기 전용, 별도 동기화 불필요 - read_part_sector()가 반환한
	 *         버퍼는 이 검사 직후 put_dev_sector()로 해제된다. */
	__be16	block_size;
	/* [한국어] 디스크의 논리 블록 크기(바이트 단위) (오프셋 0x02, 2바이트).
	 * 설정자: 온디스크 값 - 디스크 포맷 시 결정된 물리/논리 블록 크기.
	 * 읽는 자: mac_partition()이 secsize = be16_to_cpu(md->block_size)로
	 *          저장한 뒤, secsize 단위로 파티션 맵 엔트리 위치(pos = slot
	 *          * secsize)를 계산하고, 최종적으로 (secsize/512) 배수를
	 *          곱해 커널 표준 512바이트 섹터 단위로 환산하는 데 쓰인다.
	 * 값 범위: 통상 512의 배수이며, mac_partition()은 is_power_of_2()로
	 *          2의 거듭제곱인지만 검증한다(그렇지 않으면 -1 반환).
	 * 동기화: 읽기 전용, 별도 동기화 불필요. */
	__be32	block_count;
	/* [한국어] 디스크 전체의 총 블록 수 (오프셋 0x04, 4바이트).
	 * 설정자: 온디스크 값 - 디스크 포맷 시 기록.
	 * 읽는 자: mac.c의 어떤 함수도 현재 이 필드를 읽지 않는다 - 디스크
	 *          전체 크기는 이미 블록 계층(gendisk)이 알고 있으므로
	 *          파티션 파서 입장에서는 검증용으로만 쓰일 수 있으나 실제
	 *          코드에서는 참조되지 않는다 (추정).
	 * 값 범위: 0 이상, 디스크 용량 / block_size.
	 * 동기화: 사용되지 않으므로 해당 없음. */
    /* ... more stuff */
	/* [한국어] 0번 블록에는 이 구조체 이후에도 Mac OS 드라이버 파티션
	 * 테이블 등 추가 정보가 더 있으나(APM 스펙 참고), 리눅스 파티션
	 * 인식에는 필요하지 않아 정의를 생략했다. 실제로 mac_partition()은
	 * 이 구조체를 읽은 직후 put_dev_sector()로 버퍼를 해제하고, 파티션
	 * 맵 엔트리는 datasize = round_down(secsize, 512) 계산을 거쳐 별도로
	 * 다시 읽어온다 (추정). */
};

