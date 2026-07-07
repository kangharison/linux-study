// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (c) 1996-2000 Russell King.
 *
 *  Scan ADFS partitions on hard disk drives.  Unfortunately, there
 *  isn't a standard for partitioning drives on Acorn machines, so
 *  every single manufacturer of SCSI and IDE cards created their own
 *  method.
 */
/*
 * [한국어 설명] Acorn(RISC OS) 계열 하드디스크의 서드파티 파티션 테이블 파서 (acorn.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 1990년대 Acorn Archimedes/RiscPC 등 RISC OS 컴퓨터에서 사용되던
 * 여러 서드파티 하드디스크 파티셔닝 방식을 인식하고, 리눅스 커널의 공통
 * 파티션 테이블(parsed_partitions)에 등록하는 파티션 스캐너다. 당시 ADFS
 * (Acorn Disc Filing System) 기반 디스크에는 표준 파티션 규격이 없었기
 * 때문에 Cumana, ICS, EESOX, PowerTec 등 SCSI/IDE 인터페이스 카드
 * 제조사마다 제각각의 온디스크(on-disk) 파티션 테이블 포맷을 고안했고,
 * 이 파일은 그 각각을 개별 함수로 구현해 커널 부팅/디스크 인식 시점에
 * 순서대로 시도한다. 각 서브 파서는 디스크의 특정 섹터(부트 블록, 섹터
 * 0/6/7 등)에서 매직 넘버나 체크섬을 검사해 해당 포맷인지 판별하고,
 * 일치하면 파티션의 시작 섹터와 길이를 계산해 put_partition()으로
 * 등록한다. RISCiX(Acorn의 UNIX 이식판)와 Acorn Linux용 "2차" 파티션
 * 체인 파서는 여러 포맷에서 공통으로 재사용되는 하위 루틴으로 분리돼
 * 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 block/partitions/ 디렉터리에 속하며, 블록 디바이스가 처음
 * 등록되거나 open()될 때 호출되는 파티션 테이블 프로브
 * (check_partition(), block/partitions/core.c)가 시도하는 여러 파티션
 * 스킴 후보 중 하나로 동작한다. 커널은 등록된 스킴 검사 함수를 순서대로
 * 호출하며, 이 파일이 제공하는 adfspart_check_CUMANA(),
 * adfspart_check_ADFS(), adfspart_check_ICS(), adfspart_check_POWERTEC(),
 * adfspart_check_EESOX() 각각이 하나의 후보로 등록된다(각각
 * CONFIG_ACORN_PARTITION_* 커널 설정 옵션으로 개별 활성화/비활성화됨).
 * 이 진입점 함수들은 파티션을 인식하면 1, 이 스킴이 아니면 0, 디스크
 * 읽기 자체가 실패하면 -1을 반환하여 호출자가 다음 후보 스킴으로 넘어갈지
 * 판단하게 한다. 실행 컨텍스트는 블록 디바이스 초기화/스캔 경로(프로세스
 * 컨텍스트)이며, 인터럽트 컨텍스트에서는 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 block/partitions/check.h가 선언하는 parsed_partitions,
 * Sector, read_part_sector(), put_dev_sector(), put_partition() 등
 * 파티션 프레임워크 공용 인터페이스에 의존한다. read_part_sector()는
 * 내부적으로 buffer_head 계층(include/linux/buffer_head.h)을 통해
 * 디스크의 512바이트 섹터 하나를 블록 디바이스에서 읽어오며, 이 요청은
 * 실제로는 블록 계층 submit_bio() 경로를 거쳐 하위 드라이버(SCSI/ATA
 * 등)까지 내려간다. include/linux/adfs_fs.h는 ADFS 디스크 레코드
 * (struct adfs_discrecord, uapi/linux/adfs_fs.h에 정의)와 부트 블록
 * 체크섬 검사 함수 adfs_checkbblk()를 제공하며, adfs_partition()이
 * ADFS 포맷 여부를 판별하는 데 사용한다. 데이터 흐름은 "디스크 섹터
 * 원시 바이트를 읽음 -> 각 포맷별 온디스크 구조체로 캐스팅 -> 파티션
 * 시작 섹터·길이 계산 -> put_partition()으로 parsed_partitions에 등록
 * -> 이후 커널이 /dev/sdXn 등의 파티션 디바이스 노드를 생성"하는
 * 순서로 진행된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - adfs_partition(): ADFS 부트 블록/디스크 레코드를 검사해 파티션
 *   하나를 등록하는 공통 헬퍼. CUMANA와 ADFS 스킴이 공유한다.
 * - riscix_partition() / linux_partition(): 각각 RISCiX, Acorn Linux
 *   방식의 "2차" 파티션 테이블(디스크의 첫 ADFS 파티션 뒤에 이어지는
 *   하위 파티션 목록)을 파싱한다. CUMANA와 ADFS 스킴이 공유한다.
 * - adfspart_check_CUMANA() / adfspart_check_ADFS(): ADFS 계열의 두
 *   변형(연결형 다중 드라이브 vs 단일 부트블록+비-ADFS 나머지 영역)에
 *   대한 진입점.
 * - adfspart_check_ICS() / adfspart_check_POWERTEC() /
 *   adfspart_check_EESOX(): 각각 ICS, PowerTec, EESOX 인터페이스 카드
 *   제조사의 독자 파티션 테이블 포맷에 대한 진입점.
 * - struct riscix_part/riscix_record, struct linux_part,
 *   struct ics_part, struct ptec_part, struct eesox_part: 각 포맷의
 *   온디스크 레이아웃을 그대로 반영하는 구조체들 -- 필드 순서와 크기가
 *   실제 디스크 바이트 레이아웃과 정확히 일치해야 하므로 임의로 재배치
 *   하거나 패딩을 추가해서는 안 된다.
 */
/* [한국어] read_part_sector()/put_dev_sector()가 사용하는 Sector 추상화와 buffer_head 기반 섹터 I/O 선언을 가져오기 위한 include. 이 파일 자체는 buffer_head 필드를 직접 만지지 않고, check.h가 감싼 헬퍼를 통해서만 접근한다. */
#include <linux/buffer_head.h>
/* [한국어] struct adfs_discrecord(ADFS 디스크 레코드 온디스크 레이아웃)와 adfs_checkbblk()(부트 블록 체크섬 검증)를 제공하는 헤더. adfs_partition()이 ADFS 포맷 여부를 판별하는 데 필요하다. */
#include <linux/adfs_fs.h>

/* [한국어] parsed_partitions, Sector, read_part_sector(), put_dev_sector(), put_partition() 등 이 파일이 사용하는 파티션 프레임워크 공용 선언. block/partitions/ 하위 모든 스킴 파서가 공유하는 헤더다. */
#include "check.h"

/*
 * Partition types. (Oh for reusability)
 */
/*
 * [한국어] 위 세 매크로는 ADFS 부트 블록의 파티션 타입 니블(4비트, data[0x1fc] & 15)이
 * 가리키는 "다음에 이어지는 비-ADFS 파티션"의 종류를 식별하는 값이다. RISC OS
 * 진영에서 통용되던 값을 그대로 커널에 가져와 쓰고 있으며, NVMe/SCSI 등 하위
 * 전송 계층과는 무관하게 순수히 이 파서 내부에서만 의미를 갖는 식별자다.
 */
/* [한국어] RISCiX(부트 로더가 MFM 디스크 컨트롤러로 접근하던 구세대) 파티션 타입 값 */
#define PARTITION_RISCIX_MFM	1
/* [한국어] RISCiX(SCSI 디스크 컨트롤러로 접근하는 신세대) 파티션 타입 값 */
#define PARTITION_RISCIX_SCSI	2
/* [한국어] Acorn 진영에서 배포되던 리눅스 이식판의 파티션 타입 값 */
#define PARTITION_LINUX		9

/* [한국어] CUMANA와 ADFS 두 스킴이 공통으로 쓰는 adfs_partition()/riscix_partition() 헬퍼를 해당 CONFIG가 켜졌을 때만 컴파일하기 위한 조건부 컴파일 게이트 */
#if defined(CONFIG_ACORN_PARTITION_CUMANA) || \
	defined(CONFIG_ACORN_PARTITION_ADFS)
/*
 * [한국어]
 * adfs_partition() - ADFS 부트 블록/디스크 레코드를 검증하고 파티션 1개를 등록
 *
 * @state: 파티션 등록 대상이 되는 parsed_partitions 컨텍스트. 호출자가
 *         block/partitions/core.c의 check_partition()에서 할당해 전달한다.
 * @name: 진단 문자열에 붙일 파티션 이름("ADFS", "CUMANA/ADFS" 등). NULL이면
 *        이름을 출력하지 않는다(체인의 두 번째 이후 호출에서 중복 출력 방지용).
 * @data: read_part_sector()로 이미 읽어둔 512바이트 섹터 원본 버퍼. 호출자가
 *        섹터 6(ADFS 부트 블록 위치)을 미리 읽어서 넘긴다.
 * @first_sector: 이 파티션이 시작하는 절대 섹터 번호(디스크 처음 기준).
 *                디스크의 첫 파티션이면 0, CUMANA 체인의 후속 파티션이면
 *                누적된 오프셋이 전달된다.
 * @slot: parsed_partitions->parts[] 배열에서 이 파티션을 등록할 인덱스.
 * @return: 부트 블록 검증에 성공하고 유효한 디스크 크기를 찾으면 디스크
 *          레코드 포인터(dr)를 반환한다. 부트 블록 체크섬이 틀리거나
 *          디스크 크기가 0이면 NULL을 반환해 "이 스킴이 아님"을 알린다.
 *
 * ADFS 부트 블록은 512바이트 섹터의 마지막 바이트에 1바이트 체크섬을 두고,
 * 오프셋 0x1c0부터 60바이트짜리 디스크 레코드(struct adfs_discrecord)를
 * 담는 고정 레이아웃을 갖는다. 이 함수는 그 체크섬을 검증한 뒤, 디스크
 * 레코드의 disc_size/disc_size_high 두 필드를 리틀엔디안 32비트 값으로
 * 합쳐 512바이트 섹터 단위의 파티션 크기를 계산하고, put_partition()으로
 * 커널 파티션 테이블에 등록한다. 반환된 dr 포인터는 heads/secspertrack
 * 같은 CHS(Cylinder-Head-Sector) 기하 정보를 담고 있어, 호출자
 * (adfspart_check_CUMANA())가 다음 파티션의 오프셋을 계산하는 데 재사용한다.
 * 실행 컨텍스트는 파티션 스캔 중인 단일 스레드이며 동시성 문제는 없다.
 *
 * 호출 체인:
 *   adfspart_check_CUMANA()/adfspart_check_ADFS() -> [adfs_partition] -> put_partition()
 */
static struct adfs_discrecord *
adfs_partition(struct parsed_partitions *state, char *name, char *data,
	       unsigned long first_sector, int slot)
{
	/* [한국어] 부트 블록에서 캐스팅해 낼 ADFS 디스크 레코드 포인터. 검증 실패 시 NULL로 남는다. */
	struct adfs_discrecord *dr;
	/* [한국어] 이 파티션의 512바이트 섹터 수. disc_size/disc_size_high로부터 계산되어 put_partition()에 그대로 전달된다. */
	unsigned int nr_sects;

	/* [한국어] 부트 블록 512바이트 체크섬 검증(adfs_checkbblk, adfs_fs.h) -- 순환 가산 체크섬이 섹터 마지막 바이트와 불일치하면 ADFS 포맷이 아니거나 손상된 것으로 간주 */
	if (adfs_checkbblk(data))
		/* [한국어] 체크섬 불일치 -> 이 섹터는 ADFS 부트 블록이 아님 -> 호출자에게 NULL을 돌려 다음 스킴/체인 종료를 알림 */
		return NULL;

	/* [한국어] ADFS_DR_OFFSET(0x1c0) 고정 오프셋에 위치하는 디스크 레코드를 데이터 버퍼 위에 그대로 캐스팅 -- 별도 복사 없이 온디스크 레이아웃을 구조체로 재해석 */
	dr = (struct adfs_discrecord *)(data + 0x1c0);

	/* [한국어] 64비트로 취급되는 disc_size(하위)와 disc_size_high(상위)가 둘 다 0이면 크기가 정의되지 않은 디스크 -- 유효한 ADFS 파티션이 아님 */
	if (dr->disc_size == 0 && dr->disc_size_high == 0)
		/* [한국어] 크기 0 -> 등록할 파티션이 없으므로 NULL 반환 */
		return NULL;

	/* [한국어] disc_size_high(64비트 크기의 상위 32비트, 바이트 단위)를 512바이트 섹터 단위로 옮기기 위해 <<23 시프트: 상위 32비트를 그대로 두면 바이트 값이므로, 32비트만큼 올리고(<<32) 다시 512로 나누면(>>9) 결과적으로 <<23이 됨 */
	nr_sects = (le32_to_cpu(dr->disc_size_high) << 23) |
		   /* [한국어] disc_size(하위 32비트, 바이트 단위)를 >>9(512로 나눔)해 섹터 수로 변환하고 위 상위 워드와 OR로 결합 */
		   (le32_to_cpu(dr->disc_size) >> 9);

	/* [한국어] 호출자가 진단 이름을 넘겼을 때만(체인의 첫 파티션에서만) 이름을 출력 -- name이 NULL인 후속 호출에서는 중복 출력을 피함 */
	if (name) {
		/* [한국어] pp_buf(진단용 seq_buf)에 대괄호로 감싼 이름( [name] 형태)을 덧붙임 -- /proc나 dmesg에 보이는 파티션 스캔 로그의 일부가 됨 */
		seq_buf_printf(&state->pp_buf, " [%s]", name);
	}
	/* [한국어] 계산된 first_sector/nr_sects를 parsed_partitions->parts[slot]에 기록 -- 이후 커널이 이 범위로 /dev/sdXN 등의 파티션 디바이스를 생성 */
	put_partition(state, slot, first_sector, nr_sects);
	/* [한국어] 디스크 레코드 포인터를 호출자에 반환 -- CUMANA 체인 워커가 heads/secspertrack로 다음 파티션 오프셋을 계산할 때 재사용 */
	return dr;
}
#endif
/* [한국어] adfs_partition()을 감쌌던 CONFIG_ACORN_PARTITION_CUMANA || CONFIG_ACORN_PARTITION_ADFS 게이트 종료 */

/* [한국어] RISCiX 관련 구조체(struct riscix_part/riscix_record)와 riscix_partition() 헬퍼는 RISCiX 파티션 지원이 커널 설정에서 켜졌을 때만 컴파일됨 */
#ifdef CONFIG_ACORN_PARTITION_RISCIX

/*
 * [한국어]
 * struct riscix_part - RISCiX 파티션 테이블의 엔트리 1개(온디스크 레이아웃)
 *
 * RISCiX(Acorn 하드웨어용 UNIX 이식판)가 사용하던 파티션 레코드로, 디스크의
 * 첫 섹터 이후에 최대 8개까지 이어지는 struct riscix_record.part[] 배열의
 * 원소 타입이다. 필드 순서/크기가 실제 디스크 바이트 레이아웃과 정확히
 * 일치해야 하므로 임의로 재배치할 수 없다.
 */
struct riscix_part {
	__le32	start;
	/* [한국어] 이 RISCiX 파티션이 시작하는 절대 섹터 번호(리틀엔디안 32비트).
	 * 설정자: RISCiX 포맷 시점에 RISC OS/RISCiX 도구가 디스크에 기록.
	 * 읽는 자: riscix_partition()이 le32_to_cpu()로 변환해 put_partition()의
	 *          from 인자로 전달 -- 커널 파티션 테이블의 시작 섹터가 된다.
	 * 값 범위: 0 ~ 디스크 전체 섹터 수. 유효성 검사는 이 파서가 하지 않고
	 *          디스크에 기록된 값을 그대로 신뢰한다.
	 * 동기화: 읽기 전용으로만 접근하는 온디스크 데이터이므로 커널 측 락은
	 *          필요 없다(단일 스캔 스레드에서만 참조). */
	__le32	length;
	/* [한국어] 이 파티션의 섹터 수(리틀엔디안 32비트).
	 * 설정자: RISCiX 포맷 도구.
	 * 읽는 자: riscix_partition()이 le32_to_cpu()로 변환해 put_partition()의
	 *          size 인자로 전달.
	 * 값 범위: 1 이상의 섹터 수. 0이면 사실상 빈 엔트리지만, 이 파서는 0
	 *          여부를 별도로 걸러내지 않고 one/name 필드로만 유효 엔트리를
	 *          가른다.
	 * 동기화: start와 동일 -- 읽기 전용, 락 불필요. */
	__le32	one;
	/* [한국어] 엔트리 활성화 플래그로 추정되는 필드(리틀엔디안 32비트).
	 * 설정자: RISCiX 포맷 도구가 사용 중인 엔트리에 0이 아닌 값을 기록.
	 * 읽는 자: riscix_partition()의 for 루프에서 "rr->part[part].one"이
	 *          참(0이 아님)인 엔트리만 유효한 파티션으로 취급.
	 * 값 범위: 0(미사용/빈 엔트리) 또는 0이 아닌 값(사용 중). 정확한 비트
	 *          의미는 RISCiX 커널 소스 없이는 단정할 수 없어 "추정"이다.
	 * 동기화: 읽기 전용, 락 불필요. */
	char	name[16];
	/* [한국어] 파티션 이름(최대 16바이트, NUL 종단 문자열로 추정).
	 * 설정자: RISCiX 포맷 도구가 사용자 지정 이름 또는 "All" 같은 예약어를 기록.
	 * 읽는 자: riscix_partition()이 memcmp()로 "All\0"과 비교해 이 이름을
	 *          가진 엔트리(전체 디스크를 가리키는 자기참조 엔트리로 추정)를
	 *          건너뛰고, 그 외에는 seq_buf_printf()로 진단 로그에 출력.
	 * 값 범위: 최대 15자 + NUL. 이 필드는 커널 파티션 이름(예: 파티션
	 *          레이블)으로 전달되지 않고 오직 스캔 로그 문자열용으로만 쓰인다.
	 * 동기화: 읽기 전용, 락 불필요. */
};

/*
 * [한국어]
 * struct riscix_record - RISCiX 파티션 테이블 섹터 전체(온디스크 레이아웃)
 *
 * read_part_sector()로 읽은 512바이트 섹터를 그대로 이 구조체로 캐스팅해
 * 사용한다. magic 필드로 포맷을 식별하고, 이후 최대 8개의 riscix_part
 * 엔트리를 담는다.
 */
struct riscix_record {
	__le32	magic;
	/* [한국어] RISCiX 포맷임을 식별하는 매직 넘버(리틀엔디안 32비트).
	 * 설정자: RISCiX 포맷 도구가 고정값 RISCIX_MAGIC(0x4a657320, ASCII로
	 *         "Jes " -- 포맷 제작자 서명으로 추정)을 기록.
	 * 읽는 자: riscix_partition()이 rr->magic == RISCIX_MAGIC 비교로 이
	 *          섹터가 RISCiX 테이블인지 판별. 불일치 시 첫 섹터 전체를 통째로
	 *          하나의 파티션으로만 등록하는 폴백 경로를 탄다.
	 * 값 범위: 0x4a657320 이외의 값이면 RISCiX 포맷이 아님으로 간주.
	 * 동기화: 읽기 전용, 락 불필요. */
#define RISCIX_MAGIC	cpu_to_le32(0x4a657320)
	/* [한국어] RISCIX_MAGIC 리터럴 정의 -- 0x4a657320을 리틀엔디안 __le32로 고정한 매직 넘버. 구조체 정의 내부에 매크로를 두어 magic 필드 바로 옆에서 값의 의미를 알아보기 쉽게 한 스타일(원본 코드) */
	__le32	date;
	/* [한국어] 포맷/기록 시각으로 추정되는 필드(리틀엔디안 32비트). 이 파서는
	 * 이 필드를 읽거나 검증에 사용하지 않는다 -- 온디스크 레이아웃 정렬을
	 * 맞추기 위해서만 선언돼 있다.
	 * 설정자/읽는 자: 이 파일에서는 사용되지 않음(RISCiX/RISC OS 측 도구가
	 *                 기록한 것으로 추정).
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
	struct riscix_part part[8];
	/* [한국어] 최대 8개의 RISCiX 파티션 엔트리 배열(struct riscix_part).
	 * 설정자: RISCiX 포맷 도구가 사용 중인 개수만큼 채우고 나머지는 0으로
	 *         남기는 것으로 추정(one 필드가 0인 엔트리는 미사용으로 처리됨).
	 * 읽는 자: riscix_partition()의 for (part = 0; part < 8; part++) 루프가
	 *          순서대로 순회하며 유효한 엔트리만 put_partition()으로 등록.
	 * 값 범위: 각 엔트리는 struct riscix_part 참조.
	 * 동기화: 읽기 전용, 락 불필요. */
};

/* [한국어] riscix_partition() 헬퍼도 adfs_partition()과 마찬가지로 CUMANA/ADFS 두 스킴이 공유하므로 동일한 조건부 컴파일 게이트로 감쌈 */
#if defined(CONFIG_ACORN_PARTITION_CUMANA) || \
	defined(CONFIG_ACORN_PARTITION_ADFS)
/*
 * [한국어]
 * riscix_partition() - RISCiX 형식의 "2차" 파티션 테이블을 파싱해 등록
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트.
 * @first_sect: RISCiX 테이블 섹터 자체(및 유효하지 않을 때는 전체 영역)가
 *              시작하는 절대 섹터 번호. adfspart_check_CUMANA()/
 *              adfspart_check_ADFS()가 ADFS 파티션 이후 비-ADFS 영역의
 *              시작 오프셋으로 계산해 전달한다.
 * @slot: 다음에 등록할 parsed_partitions->parts[] 인덱스. 함수 내부에서
 *        등록할 때마다 증가시킨 뒤 최종값을 반환한다.
 * @nr_sects: 이 RISCiX 영역이 차지하는 섹터 수(호출자가 계산한 값). magic
 *            불일치 시 이 값 전체를 하나의 파티션으로 폴백 등록하는 데 쓰인다.
 * @return: 갱신된 slot 값(다음에 등록할 인덱스). 첫 섹터 읽기 자체가
 *          실패하면 -1을 반환한다.
 *
 * ADFS 진영에서 RISCiX를 위한 비-ADFS 파티션을 만들 때 사용하던 포맷이다.
 * 먼저 first_sect 섹터를 읽어 RISCIX_MAGIC 시그니처를 검사하고, 일치하면
 * 부트 섹터 자리(최대 2섹터)를 슬롯 하나로 등록한 뒤, part[8] 배열을
 * 순회하며 "All"이 아니고 활성화된(one != 0) 엔트리마다 각각의 절대
 * start/length를 커널 파티션으로 등록한다. 시그니처가 일치하지 않으면
 * RISCiX 포맷이 아닌 것으로 보고 first_sect~nr_sects 전체를 통째로 하나의
 * 파티션으로 등록하는 보수적인 폴백을 취한다("우리는 다음 것을 어떻게
 * 찾는지 모른다"는 CUMANA 쪽 주석과 같은 맥락의 미완성 지원).
 * 실행 컨텍스트는 파티션 스캔 중인 단일 스레드이며 동시성 문제는 없다.
 *
 * 호출 체인:
 *   adfspart_check_CUMANA()/adfspart_check_ADFS() -> [riscix_partition] -> put_partition()
 */
static int riscix_partition(struct parsed_partitions *state,
			    unsigned long first_sect, int slot,
			    unsigned long nr_sects)
{
	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들(Sector 추상화) -- 사용 후 put_dev_sector()로 반드시 해제해야 함 */
	Sector sect;
	/* [한국어] 읽어들인 섹터를 RISCiX 테이블 레이아웃으로 재해석할 포인터 */
	struct riscix_record *rr;
	
	rr = read_part_sector(state, first_sect, &sect);	/* [한국어] first_sect 섹터 하나를 읽어 rr에 캐스팅 -- 실패하면 NULL */
	if (!rr)	/* [한국어] 섹터 읽기 실패(메모리 할당 실패 또는 I/O 오류) */
		return -1;	/* [한국어] 상위 호출자에게 치명적 오류를 알려 스캔을 중단시킴 */

	seq_buf_puts(&state->pp_buf, " [RISCiX]");	/* [한국어] 진단 로그에 이 파서가 RISCiX 스킴을 시도 중임을 표시 */


	if (rr->magic == RISCIX_MAGIC) {	/* [한국어] 매직 넘버 일치 -- 실제 RISCiX 파티션 테이블로 신뢰하고 세부 엔트리를 순회 */
		unsigned long size = nr_sects > 2 ? 2 : nr_sects;	/* [한국어] 부트 섹터 영역은 최대 2섹터로 제한(추정: RISCiX 부트 로더가 차지하는 고정 크기) */
		int part;	/* [한국어] part[8] 배열을 순회할 인덱스 */

		seq_buf_puts(&state->pp_buf, " <");	/* [한국어] 이후 등록되는 하위 파티션 이름들을 감쌀 여는 괄호를 로그에 출력 */

		put_partition(state, slot++, first_sect, size);	/* [한국어] RISCiX 부트 영역 자체를 첫 하위 슬롯으로 등록하고 slot 전진 */
		for (part = 0; part < 8; part++) {	/* [한국어] 고정 8개 엔트리 배열 전체 순회 */
			if (rr->part[part].one &&	/* [한국어] one 필드가 0이 아닌(활성화된) 엔트리인지 확인 */
			    memcmp(rr->part[part].name, "All\0", 4)) {
				put_partition(state, slot++,	/* [한국어] 활성 엔트리를 새 슬롯으로 등록하고 slot 전진 */
					le32_to_cpu(rr->part[part].start),
					le32_to_cpu(rr->part[part].length));
				seq_buf_printf(&state->pp_buf, "(%s)", rr->part[part].name);	/* [한국어] 등록한 엔트리 이름을 진단 로그에 괄호로 덧붙임 */
			}
		}

		seq_buf_puts(&state->pp_buf, " >\n");	/* [한국어] 하위 파티션 이름 나열을 닫는 괄호와 줄바꿈으로 로그 마무리 */
	} else {	/* [한국어] 매직 넘버 불일치 -- 이 섹터는 RISCiX 테이블이 아니므로 세부 엔트리를 해석하지 않는 보수적 폴백 경로 */
		put_partition(state, slot++, first_sect, nr_sects);	/* [한국어] 통째로 first_sect~nr_sects 범위 하나만 파티션으로 등록 */
	}

	put_dev_sector(sect);	/* [한국어] 읽어둔 섹터 버퍼 참조 해제 -- 이 시점 이후 rr 포인터는 더 이상 유효하지 않음 */
	return slot;	/* [한국어] 갱신된 slot(다음에 등록할 인덱스)을 호출자에 돌려줌 */
}
#endif
/* [한국어] riscix_partition()을 감쌌던 CUMANA||ADFS 게이트 종료 */
#endif
/* [한국어] struct riscix_part/riscix_record 및 riscix_partition()을 감쌌던 CONFIG_ACORN_PARTITION_RISCIX 게이트 종료 */

/* [한국어] Acorn Linux 네이티브(ext2 등) 파티션 엔트리를 표시하는 매직 넘버 -- 0xdeadbeef류의 커널 관행처럼 기억하기 쉬운 16진수 문자열을 고른 것으로 추정 */
#define LINUX_NATIVE_MAGIC 0xdeafa1de
/* [한국어] Acorn Linux 스왑 파티션 엔트리를 표시하는 매직 넘버 -- NATIVE_MAGIC과 마지막 니블만 달라 스왑임을 구분 */
#define LINUX_SWAP_MAGIC   0xdeafab1e

/*
 * [한국어]
 * struct linux_part - Acorn Linux "2차" 파티션 테이블의 엔트리 1개(온디스크 레이아웃)
 *
 * ADFS 파티션 뒤에 이어지는 리눅스 전용 영역에서, magic 값이 일치하는 동안
 * 배열처럼 연속해서 다음 엔트리로 진행하며 읽히는 구조체다(별도의 "엔트리
 * 개수" 필드 없이 magic 불일치로 끝을 판단).
 */
struct linux_part {
	__le32 magic;
	/* [한국어] 이 엔트리가 유효한지, 네이티브인지 스왑인지 식별하는 매직 넘버.
	 * 설정자: Acorn Linux용 파티셔닝 도구가 LINUX_NATIVE_MAGIC 또는
	 *         LINUX_SWAP_MAGIC 값을 기록.
	 * 읽는 자: linux_partition()의 while 루프 조건 -- 둘 중 하나와 일치하는
	 *          동안 체인을 계속 따라가고, 둘 다 아니면 순회를 종료.
	 * 값 범위: 0xdeafa1de(NATIVE), 0xdeafab1e(SWAP), 그 외 값은 "체인 끝".
	 * 동기화: 읽기 전용, 락 불필요. */
	__le32 start_sect;
	/* [한국어] 파티션의 상대 시작 섹터(리틀엔디안 32비트) -- first_sect(테이블이
	 * 시작하는 절대 섹터) 기준의 오프셋으로 해석된다.
	 * 설정자: Acorn Linux 파티셔닝 도구.
	 * 읽는 자: linux_partition()이 first_sect + le32_to_cpu(start_sect)로
	 *          절대 섹터를 계산해 put_partition()의 from 인자로 전달.
	 * 값 범위: 0 이상. first_sect에 더해도 디스크 범위를 넘지 않아야 하나
	 *          이 함수는 별도 범위 검증을 하지 않는다.
	 * 동기화: 읽기 전용, 락 불필요. */
	__le32 nr_sects;
	/* [한국어] 파티션 섹터 수(리틀엔디안 32비트).
	 * 설정자: Acorn Linux 파티셔닝 도구.
	 * 읽는 자: linux_partition()이 le32_to_cpu()로 변환해 put_partition()의
	 *          size 인자로 그대로 전달.
	 * 값 범위: 1 이상의 섹터 수.
	 * 동기화: 읽기 전용, 락 불필요. */
};

/* [한국어] linux_partition() 헬퍼도 CUMANA/ADFS 두 스킴이 공유하므로 동일한 조건부 컴파일 게이트로 감쌈 */
#if defined(CONFIG_ACORN_PARTITION_CUMANA) || \
	defined(CONFIG_ACORN_PARTITION_ADFS)
/*
 * [한국어]
 * linux_partition() - Acorn Linux 형식의 "2차" 파티션 체인을 파싱해 등록
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트.
 * @first_sect: 이 Linux 영역이 시작하는 절대 섹터 번호. ADFS 파티션 이후
 *              비-ADFS 영역의 시작 오프셋으로 호출자가 계산해 전달한다.
 * @slot: 다음에 등록할 parsed_partitions->parts[] 인덱스.
 * @nr_sects: 부트 섹터 영역 크기를 정하는 데만 쓰이는 이 영역의 섹터 수
 *            상한(실제 체인 순회는 magic 값으로 끝을 판단하지 이 값으로
 *            개수를 세지 않는다).
 * @return: 갱신된 slot 값. 파티션 엔트리 섹터 읽기가 실패하면 -1을 반환한다.
 *          (주의: 부트 섹터 등록은 이 실패 이전에 이미 이뤄져 slot이
 *          한 번 증가된 상태로 남을 수 있다 -- 원본 코드의 동작을 그대로
 *          따른다.)
 *
 * first_sect 위치를 부트 섹터 영역(최대 2섹터)으로 우선 등록한 뒤, 같은
 * 섹터를 다시 읽어 struct linux_part 배열로 재해석하고, magic 값이
 * LINUX_NATIVE_MAGIC 또는 LINUX_SWAP_MAGIC인 동안 포인터를 12바이트씩
 * 전진시키며 각 엔트리의 상대 시작/길이를 절대 섹터로 변환해 등록한다.
 * parsed_partitions->limit(커널 파티션 슬롯 상한)에 도달하면 더 이상
 * 등록하지 않고 순회를 중단한다. 실행 컨텍스트는 단일 스캔 스레드이며
 * 동시성 문제는 없다.
 *
 * 호출 체인:
 *   adfspart_check_CUMANA()/adfspart_check_ADFS() -> [linux_partition] -> put_partition()
 */
static int linux_partition(struct parsed_partitions *state,
			   unsigned long first_sect, int slot,
			   unsigned long nr_sects)
{
	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들 */
	Sector sect;
	/* [한국어] 읽어들인 섹터를 Acorn Linux 파티션 엔트리 배열로 재해석할 포인터 -- 순회하며 계속 전진시킴 */
	struct linux_part *linuxp;
	unsigned long size = nr_sects > 2 ? 2 : nr_sects;	/* [한국어] 부트 섹터 영역은 최대 2섹터로 제한(추정: 부트 로더 영역 관례) */

	seq_buf_puts(&state->pp_buf, " [Linux]");	/* [한국어] 진단 로그에 Linux 스킴을 시도 중임을 표시 */

	put_partition(state, slot++, first_sect, size);	/* [한국어] 부트 섹터 영역을 첫 슬롯으로 먼저 등록 -- 아래 루프의 개별 엔트리 등록보다 앞서 이뤄짐 */

	linuxp = read_part_sector(state, first_sect, &sect);	/* [한국어] 같은 first_sect 섹터를 다시 읽어 파티션 엔트리 배열의 시작으로 재해석 */
	if (!linuxp)	/* [한국어] 섹터 읽기 실패 */
		return -1;	/* [한국어] 치명적 오류를 알려 스캔 중단 -- 단, 위에서 이미 slot++로 부트 섹터는 등록된 뒤임 */

	seq_buf_puts(&state->pp_buf, " <");	/* [한국어] 이후 등록되는 엔트리들을 감쌀 여는 괄호를 로그에 출력 */
	while (linuxp->magic == cpu_to_le32(LINUX_NATIVE_MAGIC) ||	/* [한국어] 현재 엔트리가 네이티브 또는 스왑 매직 중 하나와 일치하는 동안 계속 순회 */
	       linuxp->magic == cpu_to_le32(LINUX_SWAP_MAGIC)) {
		if (slot == state->limit)	/* [한국어] 커널 파티션 테이블 슬롯이 가득 찼는지 확인 */
			break;	/* [한국어] 더 이상 등록할 슬롯이 없으므로 순회 중단 */
		put_partition(state, slot++, first_sect +	/* [한국어] 상대 시작 섹터를 first_sect에 더해 절대 섹터로 변환해 등록하고 slot 전진 */
				 le32_to_cpu(linuxp->start_sect),
				 le32_to_cpu(linuxp->nr_sects));
		linuxp ++;	/* [한국어] 다음 온디스크 엔트리로 포인터 전진(sizeof(struct linux_part) == 12바이트만큼 이동) */
	}
	seq_buf_puts(&state->pp_buf, " >");	/* [한국어] 엔트리 나열을 닫는 괄호로 로그 마무리(줄바꿈은 호출자가 필요 시 추가) */

	put_dev_sector(sect);	/* [한국어] 읽어둔 섹터 버퍼 참조 해제 -- 이 시점 이후 linuxp가 가리키던 메모리는 더 이상 유효하지 않음 */
	return slot;	/* [한국어] 갱신된 slot을 호출자에 돌려줌 */
}
#endif
/* [한국어] linux_partition()을 감쌌던 CUMANA||ADFS 게이트 종료 */

/* [한국어] Cumana 스타일 연결형 파티션 지원이 커널 설정에서 켜졌을 때만 이 진입점 함수를 컴파일 */
#ifdef CONFIG_ACORN_PARTITION_CUMANA
/*
 * [한국어]
 * adfspart_check_CUMANA() - Cumana 스타일 연결형 ADFS 파티션 체인을 탐색
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트. state->disk,
 *         state->limit, state->pp_buf를 모두 참조/갱신한다.
 * @return: 파티션을 하나도 찾지 못했으면(첫 시도부터 ADFS 부트 블록이
 *          아니면) 0, 하나 이상 찾았으면 1을 반환한다. 도중에 섹터
 *          읽기가 실패하면 -1을 반환해 스캔 자체를 실패로 처리한다.
 *
 * Cumana 카드의 파티션 방식은 "섹터 6에 ADFS 부트 블록이 있고, 그 안의
 * CHS(실린더/헤드/섹터) 정보로 다음 드라이브의 시작 위치를 계산해 계속
 * 이어붙이는" 연결 리스트 구조로 추정된다(원본 코드 주석에도 "다음
 * 파티션의 실린더 번호가 현재 파티션 기준 상대값인지 확신할 수 없다",
 * "Cumana가 어떤 ID를 썼는지도 불명확하다"는 미완성/미검증 경고가 달려
 * 있다). do-while 루프를 돌며 adfs_partition()으로 각 드라이브를 등록하고,
 * 부트 블록의 실린더 수 * 헤드 수 * 트랙당 섹터 수로 다음 드라이브까지의
 * 오프셋을 추정한 뒤, 파티션 타입 니블에 따라 RISCiX 또는 Linux "2차"
 * 파티션 체인을 추가로 펼친다. ADFS 부트 블록이 아니거나 크기가 0이면
 * 체인이 끝난 것으로 보고 루프를 빠져나온다.
 * 실행 컨텍스트는 블록 디바이스 파티션 스캔 중인 단일 스레드다.
 *
 * 호출 체인:
 *   check_partition()(block/partitions/core.c, 추정) -> [adfspart_check_CUMANA]
 *     -> adfs_partition() -> riscix_partition()/linux_partition() -> put_partition()
 */
int adfspart_check_CUMANA(struct parsed_partitions *state)
{
	unsigned long first_sector = 0;	/* [한국어] 현재까지 누적된, 다음에 검사할 드라이브의 절대 시작 섹터. 디스크 맨 앞(0)부터 시작 */
	unsigned int start_blk = 0;	/* [한국어] BLOCK_SIZE(커널 블록 크기) 단위의 누적 오프셋 -- 섹터 단위 first_sector와 별도로 read_part_sector 호출 위치 계산에 사용 */
	Sector sect;	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들 */
	unsigned char *data;	/* [한국어] 매 반복마다 새로 읽어들이는 ADFS 부트 블록 원시 바이트 포인터 */
	char *name = "CUMANA/ADFS";	/* [한국어] 첫 번째 드라이브에만 붙일 진단 이름 -- 이후 NULL로 바뀌어 중복 출력을 막음 */
	int first = 1;	/* [한국어] 아직 첫 드라이브도 등록하지 못했는지 표시 -- 최종 반환값(0 vs 1)을 결정하는 플래그 */
	int slot = 1;	/* [한국어] 다음에 등록할 parsed_partitions 슬롯 인덱스(0번은 디스크 전체를 가리키는 예약 슬롯이므로 1부터 시작) */

	/*
	 * Try Cumana style partitions - sector 6 contains ADFS boot block
	 * with pointer to next 'drive'.
	 *
	 * There are unknowns in this code - is the 'cylinder number' of the
	 * next partition relative to the start of this one - I'm assuming
	 * it is.
	 *
	 * Also, which ID did Cumana use?
	 *
	 * This is totally unfinished, and will require more work to get it
	 * going. Hence it is totally untested.
	 */
	do {	/* [한국어] ADFS 부트 블록을 찾지 못하거나 크기가 0이 될 때까지 드라이브를 계속 연결해 나가는 루프 */
		struct adfs_discrecord *dr;	/* [한국어] adfs_partition()이 돌려주는 디스크 레코드 포인터 -- CHS 정보로 다음 오프셋 계산에 사용 */
		unsigned int nr_sects;	/* [한국어] 현재 드라이브의 추정 섹터 수(다음 드라이브까지의 거리로도 쓰임) */

		data = read_part_sector(state, start_blk * 2 + 6, &sect);	/* [한국어] 현재 드라이브의 ADFS 부트 블록 섹터(기준 섹터 6에 누적 오프셋을 더한 위치)를 읽음 */
		if (!data)	/* [한국어] 섹터 읽기 실패(디스크 끝을 벗어났거나 I/O 오류) */
			return -1;	/* [한국어] 치명적 오류로 간주해 스캔 전체를 중단 */

		if (slot == state->limit)	/* [한국어] 커널 파티션 테이블 슬롯이 가득 찼는지 확인 */
			break;	/* [한국어] 더 등록할 슬롯이 없으므로 체인 탐색 중단(이미 찾은 파티션은 유지) */

		dr = adfs_partition(state, name, data, first_sector, slot++);	/* [한국어] 이번 드라이브를 ADFS 파티션으로 등록 시도 -- 성공하면 디스크 레코드, 실패하면 NULL */
		if (!dr)	/* [한국어] 이 섹터가 ADFS 부트 블록이 아님 */
			break;	/* [한국어] 체인이 여기서 끝난 것으로 보고 루프 종료 */

		name = NULL;	/* [한국어] 첫 드라이브 이름은 이미 출력했으므로 이후 반복에서는 진단 이름을 중복 출력하지 않도록 초기화 */

		nr_sects = (data[0x1fd] + (data[0x1fe] << 8)) *	/* [한국어] 부트 블록 오프셋 0x1fd/0x1fe에 있는 실린더 수(리틀엔디안 16비트, CHS 잔재)를 조합 */
			   (dr->heads + (dr->lowsector & 0x40 ? 1 : 0)) *	/* [한국어] 헤드 수에 lowsector의 0x40 비트(상위 헤드 비트로 추정)를 보정해 곱함 */
			   dr->secspertrack;	/* [한국어] 트랙당 섹터 수를 곱해 (실린더 수 * 헤드 수 * 트랙당 섹터 수) = 이 드라이브의 총 섹터 수(CHS 기하 추정치)를 계산 */

		if (!nr_sects)	/* [한국어] 계산된 크기가 0이면 더 이상 유효한 드라이브가 없다는 뜻 */
			break;	/* [한국어] 체인 탐색 종료 */

		first = 0;	/* [한국어] 최소 한 개의 드라이브는 찾았음을 기록 -- 함수 최종 반환값을 1로 만드는 조건 */
		first_sector += nr_sects;	/* [한국어] 다음 드라이브의 절대 시작 섹터로 누적 */
		start_blk += nr_sects >> (BLOCK_SIZE_BITS - 9);	/* [한국어] 섹터 수를 커널 블록 크기 단위로 환산해 start_blk에도 누적(BLOCK_SIZE_BITS=10이면 섹터를 2로 나누는 것과 동일) */
		nr_sects = 0; /* hmm - should be partition size */	/* [한국어] 이후 RISCiX/Linux 하위 파서에 넘길 크기를 일단 0으로 리셋 -- 원본 주석(hmm - should be partition size)대로 실제 파티션 크기를 정확히 몰라 미완성 상태임을 인정하는 부분 */

		switch (data[0x1fc] & 15) {	/* [한국어] 부트 블록 오프셋 0x1fc의 하위 4비트(파티션 타입 니블)로 분기 */
		case 0: /* No partition / ADFS? */	/* [한국어] 타입 0 -- 추가 파티션 없음(순수 ADFS로 추정) -- 아무 것도 하지 않고 루프 다음 반복으로 */
			break;	/* [한국어] switch 탈출, 다음 do-while 반복으로 */

		/* [한국어] RISCiX 지원이 켜졌을 때만 이 case를 컴파일 */
#ifdef CONFIG_ACORN_PARTITION_RISCIX
		case PARTITION_RISCIX_SCSI:	/* [한국어] 파티션 타입이 RISCiX SCSI(값 2)인 경우 */
			/* RISCiX - we don't know how to find the next one. */
			slot = riscix_partition(state, first_sector, slot,	/* [한국어] RISCiX 2차 파티션 체인을 파싱해 등록하고 갱신된 slot을 받음 */
						nr_sects);
			break;	/* [한국어] switch 탈출 */
/* [한국어] PARTITION_RISCIX_SCSI case를 감쌌던 CONFIG_ACORN_PARTITION_RISCIX 게이트 종료 */
#endif

		case PARTITION_LINUX:	/* [한국어] 파티션 타입이 Acorn Linux(값 9)인 경우 */
			slot = linux_partition(state, first_sector, slot,	/* [한국어] Linux 2차 파티션 체인을 파싱해 등록하고 갱신된 slot을 받음 */
					       nr_sects);
			break;	/* [한국어] switch 탈출 */
		}
		put_dev_sector(sect);	/* [한국어] 이번 반복에서 읽은 부트 블록 섹터 버퍼 해제 */
		if (slot == -1)	/* [한국어] 하위 파서(riscix_partition/linux_partition)가 섹터 읽기 실패로 -1을 돌려줬는지 확인 */
			return -1;	/* [한국어] 하위 파서 실패를 그대로 상위에 전파해 스캔 중단 */
	} while (1);	/* [한국어] break로만 빠져나가는 무한 루프 -- 체인이 끝나거나 오류가 나야 종료 */
	put_dev_sector(sect);	/* [한국어] 루프를 빠져나온 시점의 마지막 섹터 버퍼도 해제(break 직후에도 안전하게 정리) */
	return first ? 0 : 1;	/* [한국어] 한 번도 드라이브를 찾지 못했으면 0(이 스킴 아님), 하나라도 찾았으면 1(성공)을 반환 */
}
#endif
/* [한국어] adfspart_check_CUMANA()를 감쌌던 CONFIG_ACORN_PARTITION_CUMANA 게이트 종료 */

/* [한국어] 단일 ADFS 부트 블록 + 비-ADFS 나머지 영역 방식 지원이 커널 설정에서 켜졌을 때만 이 진입점 함수를 컴파일 */
#ifdef CONFIG_ACORN_PARTITION_ADFS
/*
 * Purpose: allocate ADFS partitions.
 *
 * Params : hd		- pointer to gendisk structure to store partition info.
 *	    dev		- device number to access.
 *
 * Returns: -1 on error, 0 for no ADFS boot sector, 1 for ok.
 *
 * Alloc  : hda  = whole drive
 *	    hda1 = ADFS partition on first drive.
 *	    hda2 = non-ADFS partition.
 */
/*
 * [한국어]
 * adfspart_check_ADFS() - 단일 ADFS 부트 블록을 찾고 그 뒤 비-ADFS 영역을 추가 탐색
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트. state->disk로
 *         get_capacity()를 호출해 디스크 총 용량을 얻는다.
 * @return: 위 원본(영어) 문서 주석 그대로 -- 오류 시 -1, ADFS 부트
 *          섹터가 없으면 0, 정상 인식되면 1.
 *
 * 위 원본 영어 주석(Purpose/Params/Returns/Alloc)이 이미 이 함수의 의도를
 * 설명하고 있다: 섹터 6에서 ADFS 부트 블록을 찾아 hda1(ADFS 영역)로
 * 등록하고, 그 CHS 기하 정보로 hda1이 끝나는 지점(start_sect)을 계산해,
 * 남은 디스크 전체(get_capacity() - start_sect)를 hda2(비-ADFS 영역
 * 후보)로 취급한다. start_sect가 0보다 크면(즉 ADFS 영역 뒤에 실제로
 * 남는 공간이 있으면) 부트 블록의 파티션 타입 니블을 보고 RISCiX 또는
 * Linux 2차 파티션 체인을 그 나머지 영역에서 추가로 펼친다.
 * adfspart_check_CUMANA()와 달리 이 함수는 연결된 여러 드라이브를
 * 찾는 것이 아니라 "ADFS 영역 하나 + 그 뒤 비-ADFS 영역 하나"라는
 * 고정된 2단 구조만 가정한다. 실행 컨텍스트는 단일 스캔 스레드다.
 *
 * 호출 체인:
 *   check_partition()(block/partitions/core.c, 추정) -> [adfspart_check_ADFS]
 *     -> adfs_partition() -> riscix_partition()/linux_partition() -> put_partition()
 */
int adfspart_check_ADFS(struct parsed_partitions *state)
{
	unsigned long start_sect, nr_sects, sectscyl, heads;	/* [한국어] start_sect=비-ADFS 영역 시작 섹터, nr_sects=그 영역 크기, sectscyl=실린더당 섹터 수, heads=헤드 수(모두 CHS 기하 계산용) */
	Sector sect;	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들 */
	unsigned char *data;	/* [한국어] 섹터 6에서 읽은 ADFS 부트 블록 원시 바이트 포인터 */
	struct adfs_discrecord *dr;	/* [한국어] adfs_partition()이 돌려주는 디스크 레코드 포인터 -- heads/secspertrack 등 CHS 정보를 담음 */
	unsigned char id;	/* [한국어] 부트 블록의 파티션 타입 니블(data[0x1fc] & 15) -- RISCiX/Linux 분기에 사용 */
	int slot = 1;	/* [한국어] 다음에 등록할 parsed_partitions 슬롯 인덱스(0번은 예약 슬롯) */

	data = read_part_sector(state, 6, &sect);	/* [한국어] ADFS 부트 블록은 항상 섹터 6에 고정으로 위치(CUMANA와 달리 오프셋 계산 없음) */
	if (!data)	/* [한국어] 섹터 읽기 실패 */
		return -1;	/* [한국어] 치명적 오류로 스캔 중단 */

	dr = adfs_partition(state, "ADFS", data, 0, slot++);	/* [한국어] 디스크 맨 앞(0)부터 시작하는 ADFS 파티션으로 등록 시도, slot 전진 */
	if (!dr) {	/* [한국어] 섹터 6이 ADFS 부트 블록이 아님 */
		put_dev_sector(sect);	/* [한국어] 읽어둔 섹터 버퍼 해제 */
    		return 0;	/* [한국어] 이 스킴이 아니라는 뜻으로 0 반환 -- 호출자가 다음 파티션 스킴을 시도하게 함 */
	}

	heads = dr->heads + ((dr->lowsector >> 6) & 1);	/* [한국어] 헤드 수에 lowsector의 6번 비트(상위 헤드 비트로 추정)를 보정해 실제 헤드 수를 산출 */
	sectscyl = dr->secspertrack * heads;	/* [한국어] 트랙당 섹터 수 * 헤드 수 = 실린더 하나당 섹터 수 */
	start_sect = ((data[0x1fe] << 8) + data[0x1fd]) * sectscyl;	/* [한국어] 부트 블록에 적힌 실린더 번호(리틀엔디안 16비트, CHS 잔재)에 실린더당 섹터 수를 곱해 비-ADFS 영역이 시작하는 절대 섹터를 계산 */
	id = data[0x1fc] & 15;	/* [한국어] 비-ADFS 영역의 파티션 타입 니블 추출(RISCiX_SCSI/RISCiX_MFM/LINUX 등과 비교할 값) */
	put_dev_sector(sect);	/* [한국어] 부트 블록 섹터 버퍼는 이제 필요한 값을 모두 뽑았으므로 해제 -- 이후 data/dr 포인터는 더 이상 유효하지 않음 */

	/*
	 * Work out start of non-adfs partition.
	 */
	nr_sects = get_capacity(state->disk) - start_sect;	/* [한국어] 디스크 전체 용량(get_capacity, 섹터 단위)에서 ADFS 영역 크기를 빼 비-ADFS 영역의 남은 섹터 수를 계산 */

	if (start_sect) {	/* [한국어] 비-ADFS 영역이 실제로 존재하는 경우(0이면 디스크 전체가 ADFS이므로 추가 파싱 불필요) */
		switch (id) {	/* [한국어] 비-ADFS 영역의 파티션 타입 니블로 분기 */
		/* [한국어] RISCiX 지원이 커널 설정에서 켜졌을 때만 아래 두 case를 컴파일 */
#ifdef CONFIG_ACORN_PARTITION_RISCIX
		case PARTITION_RISCIX_SCSI:	/* [한국어] RISCiX SCSI 타입 -- 아래 MFM과 동일하게 처리(폴스루) */
		case PARTITION_RISCIX_MFM:	/* [한국어] RISCiX MFM 타입 -- 두 SCSI/MFM 타입 모두 riscix_partition()으로 위임 */
			riscix_partition(state, start_sect, slot,	/* [한국어] 비-ADFS 영역 시작 위치부터 RISCiX 2차 파티션 체인 파싱(반환값의 slot 갱신은 이 스킴에서는 사용하지 않음) */
						nr_sects);
			break;	/* [한국어] switch 탈출 */
/* [한국어] RISCiX case들을 감쌌던 CONFIG_ACORN_PARTITION_RISCIX 게이트 종료 */
#endif

		case PARTITION_LINUX:	/* [한국어] Acorn Linux 타입 */
			linux_partition(state, start_sect, slot,	/* [한국어] 비-ADFS 영역 시작 위치부터 Linux 2차 파티션 체인 파싱 */
					       nr_sects);
			break;	/* [한국어] switch 탈출 */
		}
	}
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 이 스킴의 진단 로그 한 줄을 줄바꿈으로 마무리 */
	return 1;	/* [한국어] ADFS 스킴을 성공적으로 인식했음을 알림 */
}
#endif
/* [한국어] adfspart_check_ADFS()를 감쌌던 CONFIG_ACORN_PARTITION_ADFS 게이트 종료 */

/* [한국어] ICS 인터페이스 카드용 파티션 지원이 커널 설정에서 켜졌을 때만 이 블록 전체(구조체 + 세 함수)를 컴파일 */
#ifdef CONFIG_ACORN_PARTITION_ICS

/*
 * [한국어]
 * struct ics_part - ICS 파티션 테이블의 엔트리 1개(온디스크 레이아웃)
 *
 * ICS 방식은 섹터 0 전체를 파티션 테이블로 쓰며, 이 구조체 배열이
 * 섹터 맨 앞부터 연속으로 나열되고 size가 0인 엔트리에서 배열이 끝난다.
 */
struct ics_part {
	__le32 start;
	/* [한국어] 파티션 시작 섹터(리틀엔디안 32비트, 절대 섹터로 추정).
	 * 설정자: ICS 카드용 RISC OS 파티셔닝 도구.
	 * 읽는 자: adfspart_check_ICS()가 le32_to_cpu()로 변환해 start로 사용,
	 *          이후 필요 시 시그니처 섹터 보정(+1)을 거쳐 put_partition()의
	 *          from 인자로 전달.
	 * 값 범위: 0 이상의 절대 섹터 번호.
	 * 동기화: 읽기 전용, 락 불필요. */
	__le32 size;
	/* [한국어] 파티션 섹터 수(리틀엔디안 32비트, 부호 있는 값으로 해석됨).
	 * 설정자: ICS 파티셔닝 도구 -- 이 필드를 음수로 채우면 RISC OS의 ICS
	 *         드라이버에게 "이 영역은 ADFS 파일시스템이 아니니 무시하라"는
	 *         신호가 된다(원본 코드 주석 참고).
	 * 읽는 자: adfspart_check_ICS()가 s32(부호 있는 32비트)로 변환해 부호를
	 *          검사 -- 음수면 절댓값을 취하고, 크기가 1보다 크면 첫 섹터를
	 *          adfspart_check_ICSLinux()로 추가 검사해 "LinuxPart" 시그니처
	 *          유무에 따라 시작/크기를 1섹터 보정한다.
	 * 값 범위: 0(배열 종료 표시), 양수(ADFS 파티션), 음수(비-ADFS 영역,
	 *          절댓값이 실제 크기).
	 * 동기화: 읽기 전용, 락 불필요. */
};

/*
 * [한국어]
 * adfspart_check_ICSLinux() - 지정 섹터가 ICS 확장 "LinuxPart" 시그니처를 갖는지 검사
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트(read_part_sector() 호출에 필요).
 * @block: 시그니처를 검사할 절대 섹터 번호 -- 음수 크기(비-ADFS 표시)를 가진
 *         ICS 엔트리의 시작 섹터가 전달된다.
 * @return: 해당 섹터의 첫 9바이트가 "LinuxPart" 문자열과 일치하면 1,
 *          아니면(섹터 읽기 실패 포함) 0을 반환한다.
 *
 * ICS 표준 포맷에는 없는 이 커널만의 확장으로, 음수 크기로 표시된
 * "ADFS가 아닌" 영역의 정체를 그 영역 첫 섹터의 매직 문자열로 추가 식별한다.
 * 원본 주석대로 이 시그니처는 ADFS 파일시스템에는 보이지 않아야 하므로,
 * 호출자(adfspart_check_ICS())가 시그니처를 발견하면 그 섹터를 파티션
 * 데이터 영역에서 제외(시작 +1, 크기 -1)하도록 보정한다.
 * 실행 컨텍스트는 단일 스캔 스레드다.
 *
 * 호출 체인:
 *   adfspart_check_ICS() -> [adfspart_check_ICSLinux] -> read_part_sector()
 */
static int adfspart_check_ICSLinux(struct parsed_partitions *state,
				   unsigned long block)
{
	Sector sect;	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들 */
	unsigned char *data = read_part_sector(state, block, &sect);	/* [한국어] 검사 대상 섹터를 읽음 -- 실패하면 NULL */
	int result = 0;	/* [한국어] 기본값은 '시그니처 없음'(0) -- 읽기 실패 시에도 이 값 그대로 반환 */

	if (data) {	/* [한국어] 섹터 읽기가 성공했을 때만 시그니처 검사 진행 */
		if (memcmp(data, "LinuxPart", 9) == 0)	/* [한국어] 섹터의 첫 9바이트가 정확히 'LinuxPart' 문자열과 일치하는지 비교 */
			result = 1;	/* [한국어] 일치하면 이 영역이 리눅스 전용임을 표시 */
		put_dev_sector(sect);	/* [한국어] 검사용으로 읽은 섹터 버퍼 해제 */
	}

	return result;	/* [한국어] 시그니처 발견 여부(1/0)를 호출자에 돌려줌 */
}

/*
 * Check for a valid ICS partition using the checksum.
 */
/*
 * [한국어]
 * valid_ics_sector() - ICS 파티션 테이블 섹터의 체크섬을 검증
 *
 * @data: 섹터 0에서 읽은 512바이트 원시 데이터.
 * @return: 체크섬이 일치하면 참(0이 아님, 실제로는 1), 불일치하면 거짓(0).
 *
 * ICS는 512바이트 섹터 중 앞 508바이트를 문자열 "Part"(0x50617274)를
 * 초기 시드로 삼아 바이트 단위로 누적 가산한 체크섬을 만들고, 마지막
 * 4바이트(오프셋 508)에 그 체크섬 값을 리틀엔디안으로 저장해 둔다.
 * 이 함수는 508바이트를 다시 더한 뒤 저장된 값을 빼서 0이 되는지로
 * 검증한다. 이 검증에 실패하면 이 섹터는 ICS 파티션 테이블이 아닌
 * 것으로 간주되어 adfspart_check_ICS()가 "스킴 아님"(0)을 반환한다.
 * 실행 컨텍스트는 단일 스캔 스레드다.
 *
 * 호출 체인:
 *   adfspart_check_ICS() -> [valid_ics_sector]
 */
static inline int valid_ics_sector(const unsigned char *data)
{
	unsigned long sum;	/* [한국어] 누적 체크섬 값 */
	int i;	/* [한국어] 508바이트를 순회할 인덱스 */

	for (i = 0, sum = 0x50617274; i < 508; i++)	/* [한국어] 초기값 0x50617274('Part'의 ASCII 값)에서 시작해 앞 508바이트(512 - 체크섬 필드 4바이트)만 순회 */
		sum += data[i];	/* [한국어] 각 바이트를 그대로(부호 확장 없이 unsigned char) 더해 누적 -- 단순 가산 체크섬 */

	sum -= le32_to_cpu(*(__le32 *)(&data[508]));	/* [한국어] 오프셋 508에 저장된 리틀엔디안 32비트 체크섬 값을 읽어 누적값에서 빼기 -- 원본이 올바르면 0이 남음 */

	return sum == 0;	/* [한국어] 뺀 결과가 정확히 0이면 체크섬 일치(유효한 ICS 섹터) */
}

/*
 * Purpose: allocate ICS partitions.
 * Params : hd		- pointer to gendisk structure to store partition info.
 *	    dev		- device number to access.
 * Returns: -1 on error, 0 for no ICS table, 1 for partitions ok.
 * Alloc  : hda  = whole drive
 *	    hda1 = ADFS partition 0 on first drive.
 *	    hda2 = ADFS partition 1 on first drive.
 *		..etc..
 */
/*
 * [한국어]
 * adfspart_check_ICS() - ICS 방식 파티션 테이블(섹터 0)을 파싱
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트.
 * @return: 위 원본(영어) Purpose/Params/Returns/Alloc 주석 그대로 --
 *          섹터 읽기 실패 시 -1, 체크섬 불일치(ICS 테이블 아님) 시 0,
 *          정상 파싱되면 1.
 *
 * 섹터 0을 통째로 struct ics_part 배열로 재해석해, valid_ics_sector()로
 * 체크섬을 먼저 검증한 뒤, size가 0이 아닌 동안 배열을 순회하며 각
 * 엔트리를 등록한다. size가 음수면 "이 영역은 ADFS가 아니다"라는 표시로
 * 보고 절댓값을 취하되, adfspart_check_ICSLinux()로 첫 섹터의
 * "LinuxPart" 시그니처를 추가 검사해 발견하면 시작/크기를 1섹터씩
 * 보정(그 시그니처 섹터 자체는 파티션 데이터에서 제외)한다.
 * parsed_partitions->limit(슬롯 상한)에 도달하면 더 이상 등록하지 않고
 * 순회를 중단한다. 실행 컨텍스트는 단일 스캔 스레드다.
 *
 * 호출 체인:
 *   check_partition()(block/partitions/core.c, 추정) -> [adfspart_check_ICS]
 *     -> valid_ics_sector() -> adfspart_check_ICSLinux() -> put_partition()
 */
int adfspart_check_ICS(struct parsed_partitions *state)
{
	const unsigned char *data;	/* [한국어] 섹터 0의 원시 바이트 포인터 */
	const struct ics_part *p;	/* [한국어] data를 ics_part 배열로 재해석해 순회할 포인터 */
	int slot;	/* [한국어] 다음에 등록할 parsed_partitions 슬롯 인덱스(for 루프 초기화식에서 1로 설정) */
	Sector sect;	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들 */

	/*
	 * Try ICS style partitions - sector 0 contains partition info.
	 */
	data = read_part_sector(state, 0, &sect);	/* [한국어] ICS 테이블은 항상 섹터 0에 고정 위치 */
	if (!data)	/* [한국어] 섹터 읽기 실패 */
	    	return -1;	/* [한국어] 치명적 오류로 스캔 중단 */

	if (!valid_ics_sector(data)) {	/* [한국어] 체크섬이 맞지 않으면 이 섹터는 ICS 테이블이 아님 */
	    	put_dev_sector(sect);	/* [한국어] 읽어둔 섹터 버퍼 해제 */
		return 0;	/* [한국어] 이 스킴이 아니라는 뜻으로 0 반환 */
	}

	seq_buf_puts(&state->pp_buf, " [ICS]");	/* [한국어] 진단 로그에 ICS 스킴 인식을 표시 */

	for (slot = 1, p = (const struct ics_part *)data; p->size; p++) {	/* [한국어] slot을 1로 초기화하고 p를 배열 시작으로 설정, size가 0인 엔트리를 만날 때까지 순회(포인터를 엔트리 크기만큼 전진) */
		u32 start = le32_to_cpu(p->start);	/* [한국어] 현재 엔트리의 시작 섹터를 호스트 바이트 순서로 변환 */
		s32 size = le32_to_cpu(p->size); /* yes, it's signed. */	/* [한국어] 크기를 부호 있는 정수로 해석(원본 주석이 강조하듯 unsigned가 아님에 유의) */

		if (slot == state->limit)	/* [한국어] 커널 파티션 테이블 슬롯이 가득 찼는지 확인 */
			break;	/* [한국어] 더 등록할 슬롯이 없으므로 순회 중단 */

		/*
		 * Negative sizes tell the RISC OS ICS driver to ignore
		 * this partition - in effect it says that this does not
		 * contain an ADFS filesystem.
		 */
		if (size < 0) {	/* [한국어] 음수 크기 -- RISC OS ICS 드라이버에게 'ADFS 아님'을 알리는 표시 */
			size = -size;	/* [한국어] 이후 계산에 쓸 실제 섹터 수는 절댓값 */

			/*
			 * Our own extension - We use the first sector
			 * of the partition to identify what type this
			 * partition is.  We must not make this visible
			 * to the filesystem.
			 */
			if (size > 1 && adfspart_check_ICSLinux(state, start)) {	/* [한국어] 영역이 2섹터 이상이고 첫 섹터가 'LinuxPart' 시그니처를 가지면 */
				start += 1;	/* [한국어] 시그니처가 담긴 첫 섹터는 실제 파티션 데이터가 아니므로 시작 위치를 1섹터 뒤로 미룸 */
				size -= 1;	/* [한국어] 그만큼 크기도 1섹터 줄임 */
			}
		}

		if (size)	/* [한국어] 보정 후에도 크기가 0이 아니면(즉 유효한 파티션이면) */
			put_partition(state, slot++, start, size);	/* [한국어] 이 엔트리를 커널 파티션으로 등록하고 slot 전진 */
	}

	put_dev_sector(sect);	/* [한국어] 섹터 0 버퍼 해제 -- 이후 p/data 포인터는 더 이상 유효하지 않음 */
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 진단 로그 한 줄을 줄바꿈으로 마무리 */
	return 1;	/* [한국어] ICS 스킴을 성공적으로 인식했음을 알림 */
}
#endif
/* [한국어] struct ics_part 및 세 함수를 감쌌던 CONFIG_ACORN_PARTITION_ICS 게이트 종료 */

/* [한국어] PowerTec 인터페이스 카드용 파티션 지원이 커널 설정에서 켜졌을 때만 이 블록 전체(구조체 + 두 함수)를 컴파일 */
#ifdef CONFIG_ACORN_PARTITION_POWERTEC
/*
 * [한국어]
 * struct ptec_part - PowerTec 파티션 테이블의 엔트리 1개(온디스크 레이아웃)
 *
 * 섹터 0 전체가 이 구조체 12개(아래 adfspart_check_POWERTEC()의 for
 * (i < 12) 루프 참고)의 고정 배열로 채워져 있다고 가정하는 포맷이다.
 */
struct ptec_part {
	__le32 unused1;
	/* [한국어] 용도를 알 수 없는 패딩/예약 필드 1(리틀엔디안 32비트, 추정).
	 * 설정자/읽는 자: 이 파서는 값을 읽거나 쓰지 않는다 -- 온디스크
	 *                 레이아웃에서 start/size 필드 앞의 자리만 맞추기 위해
	 *                 선언돼 있다.
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
	__le32 unused2;
	/* [한국어] 용도를 알 수 없는 패딩/예약 필드 2(리틀엔디안 32비트, 추정).
	 * 설정자/읽는 자: unused1과 동일하게 이 파서는 사용하지 않음.
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
	__le32 start;
	/* [한국어] 파티션 시작 섹터(리틀엔디안 32비트, 절대 섹터로 추정).
	 * 설정자: PowerTec 카드용 RISC OS 파티셔닝 도구.
	 * 읽는 자: adfspart_check_POWERTEC()가 le32_to_cpu()로 변환해
	 *          put_partition()의 from 인자로 그대로 전달.
	 * 값 범위: 0 이상의 절대 섹터 번호.
	 * 동기화: 읽기 전용, 락 불필요. */
	__le32 size;
	/* [한국어] 파티션 섹터 수(리틀엔디안 32비트).
	 * 설정자: PowerTec 파티셔닝 도구.
	 * 읽는 자: adfspart_check_POWERTEC()가 0이 아닐 때만 put_partition()으로
	 *          등록(0이면 미사용 엔트리로 건너뜀).
	 * 값 범위: 0(미사용 엔트리) 또는 양수(사용 중인 파티션 크기).
	 * 동기화: 읽기 전용, 락 불필요. */
	__le32 unused5;
	/* [한국어] 용도를 알 수 없는 패딩/예약 필드 5(리틀엔디안 32비트, 추정).
	 * 설정자/읽는 자: 이 파서는 사용하지 않음 -- type 필드 앞자리를
	 *                 맞추기 위한 것으로 추정.
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
	char type[8];
	/* [한국어] 파티션 타입을 나타내는 문자열(8바이트, 추정).
	 * 설정자: PowerTec 파티셔닝 도구.
	 * 읽는 자: 이 파서는 이 필드를 읽지 않는다(체크섬/시작/크기만 사용) --
	 *          RISC OS 측 드라이버가 파일시스템 종류 등을 식별하는 데
	 *          쓰는 것으로 추정.
	 * 값 범위: 알 수 없음(추정, NUL 종단 여부 불명).
	 * 동기화: 해당 없음. */
};

/*
 * [한국어]
 * valid_ptec_sector() - PowerTec 파티션 섹터의 체크섬을 검증하고 PC MBR과 구분
 *
 * @data: 섹터 0에서 읽은 512바이트 원시 데이터.
 * @return: PC/BIOS MBR 시그니처(0x55, 0xaa)가 보이면 무조건 0(PowerTec
 *          아님), 그 외에는 511바이트 가산 체크섬이 마지막 바이트와
 *          일치할 때만 참(0이 아님)을 반환한다.
 *
 * PowerTec 파티션 테이블도 섹터 0을 사용하므로, 같은 섹터 0을 쓰는 PC
 * 표준 MBR과 오인식될 위험이 있다. 이를 막기 위해 먼저 섹터 마지막
 * 두 바이트(오프셋 510/511)가 MBR 부트 시그니처(0x55 0xaa)인지부터
 * 확인해, 그렇다면 PowerTec 검증을 포기하고 즉시 0을 반환한다. 그렇지
 * 않으면 초기값 0x2a에서 시작해 앞 511바이트를 가산한 체크섬이 마지막
 * 바이트(오프셋 511)와 일치하는지로 PowerTec 여부를 판정한다.
 * 실행 컨텍스트는 단일 스캔 스레드다.
 *
 * 호출 체인:
 *   adfspart_check_POWERTEC() -> [valid_ptec_sector]
 */
static inline int valid_ptec_sector(const unsigned char *data)
{
	unsigned char checksum = 0x2a;	/* [한국어] 체크섬 초기 시드값(임의로 정해진 8비트 값, 0으로 시작하지 않는 PowerTec 고유 관례) */
	int i;	/* [한국어] 511바이트를 순회할 인덱스 */

	/*
	 * If it looks like a PC/BIOS partition, then it
	 * probably isn't PowerTec.
	 */
	if (data[510] == 0x55 && data[511] == 0xaa)	/* [한국어] PC/BIOS MBR의 표준 부트 섹터 시그니처(0x55 0xaa)가 마지막 2바이트에 있는지 확인 */
		return 0;	/* [한국어] MBR로 보이면 PowerTec 포맷이 아니라고 판단해 즉시 실패 반환 */

	for (i = 0; i < 511; i++)	/* [한국어] 마지막 체크섬 바이트(오프셋 511)를 제외한 앞 511바이트를 순회 */
		checksum += data[i];	/* [한국어] 8비트 unsigned char 가산 -- 오버플로우는 자연스럽게 랩어라운드됨(8비트 모듈로 체크섬) */

	return checksum == data[511];	/* [한국어] 계산된 체크섬이 섹터 마지막 바이트에 저장된 값과 같은지 비교 */
}

/*
 * Purpose: allocate ICS partitions.
 * Params : hd		- pointer to gendisk structure to store partition info.
 *	    dev		- device number to access.
 * Returns: -1 on error, 0 for no ICS table, 1 for partitions ok.
 * Alloc  : hda  = whole drive
 *	    hda1 = ADFS partition 0 on first drive.
 *	    hda2 = ADFS partition 1 on first drive.
 *		..etc..
 */
/*
 * [한국어]
 * adfspart_check_POWERTEC() - PowerTec 방식 파티션 테이블을 파싱
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트.
 * @return: 위 원본(영어) Purpose/Params/Returns/Alloc 주석 참고. 다만 이
 *          원본 주석은 사실 바로 위 ICS 함수의 문서를 그대로 복사해 온
 *          것으로 보이는 문구("allocate ICS partitions" 등)를 담고 있어
 *          함수 이름/포맷과 맞지 않는다 -- 커널 원본 코드 자체의 복사·붙여넣기
 *          흔적으로 보이며, 실제 동작은 PowerTec 포맷 기준이다. 섹터 읽기
 *          실패 시 -1, 체크섬/시그니처 불일치(PowerTec 테이블 아님) 시 0,
 *          정상 파싱되면 1을 반환한다.
 *
 * 섹터 0을 읽어 valid_ptec_sector()로 검증한 뒤, 통과하면 섹터를 struct
 * ptec_part 배열(고정 12개 엔트리)로 재해석해 순회하며, size가 0이 아닌
 * 엔트리만 put_partition()으로 등록한다. ICS/ADFS와 달리 슬롯 상한
 * (state->limit) 검사가 없다는 점이 특이하다(엔트리 수가 애초에 12개로
 * 고정돼 있어 상한을 넘길 일이 거의 없다고 가정한 것으로 추정).
 * 실행 컨텍스트는 단일 스캔 스레드다.
 *
 * 호출 체인:
 *   check_partition()(block/partitions/core.c, 추정) -> [adfspart_check_POWERTEC]
 *     -> valid_ptec_sector() -> put_partition()
 */
int adfspart_check_POWERTEC(struct parsed_partitions *state)
{
	Sector sect;	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들 */
	const unsigned char *data;	/* [한국어] 섹터 0의 원시 바이트 포인터 */
	const struct ptec_part *p;	/* [한국어] data를 ptec_part 배열로 재해석해 순회할 포인터 */
	int slot = 1;	/* [한국어] 다음에 등록할 parsed_partitions 슬롯 인덱스(0번은 예약 슬롯) */
	int i;	/* [한국어] 고정 12개 엔트리를 순회할 인덱스 */

	data = read_part_sector(state, 0, &sect);	/* [한국어] PowerTec 테이블도 섹터 0에 고정 위치 */
	if (!data)	/* [한국어] 섹터 읽기 실패 */
		return -1;	/* [한국어] 치명적 오류로 스캔 중단 */

	if (!valid_ptec_sector(data)) {	/* [한국어] MBR 시그니처가 있거나 체크섬이 틀리면 PowerTec 테이블이 아님 */
		put_dev_sector(sect);	/* [한국어] 읽어둔 섹터 버퍼 해제 */
		return 0;	/* [한국어] 이 스킴이 아니라는 뜻으로 0 반환 */
	}

	seq_buf_puts(&state->pp_buf, " [POWERTEC]");	/* [한국어] 진단 로그에 PowerTec 스킴 인식을 표시 */

	for (i = 0, p = (const struct ptec_part *)data; i < 12; i++, p++) {	/* [한국어] 섹터 0을 고정 12개 엔트리 배열로 보고 순서대로 순회(포인터를 엔트리 크기만큼 전진) */
		u32 start = le32_to_cpu(p->start);	/* [한국어] 현재 엔트리의 시작 섹터를 호스트 바이트 순서로 변환 */
		u32 size = le32_to_cpu(p->size);	/* [한국어] 현재 엔트리의 섹터 수를 호스트 바이트 순서로 변환(부호 없는 값 -- ICS와 달리 음수 표시 관례가 없음) */

		if (size)	/* [한국어] 크기가 0이 아니면(사용 중인 엔트리면) */
			put_partition(state, slot++, start, size);	/* [한국어] 이 엔트리를 커널 파티션으로 등록하고 slot 전진 */
	}

	put_dev_sector(sect);	/* [한국어] 섹터 0 버퍼 해제 -- 이후 p/data 포인터는 더 이상 유효하지 않음 */
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 진단 로그 한 줄을 줄바꿈으로 마무리 */
	return 1;	/* [한국어] PowerTec 스킴을 성공적으로 인식했음을 알림 */
}
#endif
/* [한국어] struct ptec_part 및 두 함수를 감쌌던 CONFIG_ACORN_PARTITION_POWERTEC 게이트 종료 */

/* [한국어] EESOX SCSI 인터페이스 카드용 파티션 지원이 커널 설정에서 켜졌을 때만 이 블록 전체(구조체 + 함수)를 컴파일 */
#ifdef CONFIG_ACORN_PARTITION_EESOX
/*
 * [한국어]
 * struct eesox_part - EESOX SCSI 파티션 테이블의 엔트리 1개(온디스크 레이아웃, XOR 복호화 후)
 *
 * 섹터 7을 eesox_name[]으로 XOR 복호화한 256바이트 버퍼를 이 구조체
 * 8개의 배열로 재해석한다. 이 포맷은 파티션 크기를 직접 저장하지 않고,
 * 다음 엔트리의 start 값과의 차이로 크기를 유추해야 하는 특이한 구조다
 * (아래 adfspart_check_EESOX() 함수 설명 참고).
 */
struct eesox_part {
	char	magic[6];
	/* [한국어] "Eesox" 포맷 시그니처 문자열(6바이트, NUL 포함 비교).
	 * 설정자: EESOX 파티셔닝 도구가 각 유효 엔트리 앞에 고정 문자열을 기록.
	 * 읽는 자: adfspart_check_EESOX()가 memcmp(p->magic, "Eesox", 6)로 검사
	 *          -- 불일치하면 엔트리 배열의 끝(또는 미사용 슬롯)으로 간주해
	 *          순회를 중단.
	 * 값 범위: "Eesox\0"과 정확히 일치하거나, 불일치 시 배열 종료 표시.
	 * 동기화: 읽기 전용, 락 불필요. */
	char	name[10];
	/* [한국어] 파티션 이름(10바이트, 추정: 사용자 지정 레이블).
	 * 설정자: EESOX 파티셔닝 도구.
	 * 읽는 자: 이 파서는 name을 읽지 않는다(시그니처/시작 위치만 사용) --
	 *          RISC OS 측 드라이버가 표시용으로 쓰는 것으로 추정.
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
	__le32	start;
	/* [한국어] 다음(또는 이 엔트리 자신의, 해석 방식은 아래 함수 설명 참고)
	 * 파티션이 시작하는 절대 섹터(리틀엔디안 32비트).
	 * 설정자: EESOX 파티셔닝 도구.
	 * 읽는 자: adfspart_check_EESOX()가 le32_to_cpu()로 변환한 뒤, 이전
	 *          엔트리에서 기억해 둔 start 값과의 차이(next - start)를
	 *          "이전 파티션의 크기"로 역산해 put_partition()에 전달한다.
	 * 값 범위: 0 이상, 엔트리가 나열될수록 오름차순으로 증가하는 것으로
	 *          가정(그렇지 않으면 크기 역산이 음수가 될 수 있음).
	 * 동기화: 읽기 전용, 락 불필요. */
	__le32	unused6;
	/* [한국어] 용도를 알 수 없는 패딩/예약 필드 6(리틀엔디안 32비트, 추정).
	 * 설정자/읽는 자: 이 파서는 사용하지 않음.
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
	__le32	unused7;
	/* [한국어] 용도를 알 수 없는 패딩/예약 필드 7(리틀엔디안 32비트, 추정).
	 * 설정자/읽는 자: 이 파서는 사용하지 않음.
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
	__le32	unused8;
	/* [한국어] 용도를 알 수 없는 패딩/예약 필드 8(리틀엔디안 32비트, 추정).
	 * 설정자/읽는 자: 이 파서는 사용하지 않음.
	 * 값 범위: 알 수 없음(추정).
	 * 동기화: 해당 없음. */
};

/*
 * Guess who created this format?
 */
/* [한국어] eesox_name[]의 각 바이트를 이어 읽으면 포맷 제작자의 이름('Neil Critchell')이 되도록 심어둔 XOR 복호화 키 -- 원본 영어 주석('Guess who created this format?')이 가리키는 농담의 답 */
static const char eesox_name[] = {	/* [한국어] 섹터 7의 EESOX 테이블을 '복호화'하는 데 쓰이는 16바이트 XOR 키 배열 */
	'N', 'e', 'i', 'l', ' ',
	'C', 'r', 'i', 't', 'c', 'h', 'e', 'l', 'l', ' ', ' '
};

/*
 * EESOX SCSI partition format.
 *
 * This is a goddamned awful partition format.  We don't seem to store
 * the size of the partition in this table, only the start addresses.
 *
 * There are two possibilities where the size comes from:
 *  1. The individual ADFS boot block entries that are placed on the disk.
 *  2. The start address of the next entry.
 */
/*
 * [한국어]
 * adfspart_check_EESOX() - EESOX SCSI 파티션 테이블을 복호화하고 파티션 범위를 등록
 *
 * @state: 파티션 등록 대상 parsed_partitions 컨텍스트.
 * @return: EESOX 시그니처를 가진 엔트리를 하나도 찾지 못하면 0, 하나
 *          이상 찾았으면 1. (섹터 읽기 실패 시에만 -1 -- 이는 아래 코드의
 *          data 읽기 실패 검사에서만 발생.)
 *
 * 위 원본(영어) 주석이 이 포맷의 근본적인 문제를 설명한다: EESOX 테이블은
 * 파티션 크기를 저장하지 않고 각 엔트리의 시작 섹터만 저장하므로, 크기는
 * "다음 엔트리의 시작 섹터와의 차이"로만 유추할 수 있다. 게다가 섹터 7의
 * 원시 데이터는 eesox_name[]을 16바이트 주기로 반복 적용하는 XOR로
 * "암호화"돼 있어 먼저 복호화를 거쳐야 한다(원본 주석: "God knows
 * why..."). 이 함수는 (1) 섹터 7을 읽어 256바이트를 복호화하고, (2) 그
 * 결과를 struct eesox_part 8개짜리 배열로 보고 "Eesox" 시그니처가 있는
 * 동안 순회하면서, 두 번째 엔트리부터는 직전에 기억해 둔 시작 섹터와
 * 현재 엔트리의 시작 섹터 차이를 "직전 파티션의 크기"로 등록하고, (3)
 * 마지막으로 찾은 엔트리는 디스크 끝(get_capacity())까지를 크기로 등록해
 * 마무리한다. 실행 컨텍스트는 단일 스캔 스레드다.
 *
 * 호출 체인:
 *   check_partition()(block/partitions/core.c, 추정) -> [adfspart_check_EESOX]
 *     -> put_partition()
 */
int adfspart_check_EESOX(struct parsed_partitions *state)
{
	Sector sect;	/* [한국어] read_part_sector()가 채워줄 섹터 버퍼 핸들 */
	const unsigned char *data;	/* [한국어] 섹터 7에서 읽은 암호화된(XOR된) 원시 바이트 포인터 */
	unsigned char buffer[256];	/* [한국어] XOR 복호화 결과를 담을 스택 버퍼 -- 섹터 512바이트 중 앞 256바이트만 사용(구조체 8개 * 32바이트) */
	struct eesox_part *p;	/* [한국어] buffer를 eesox_part 배열로 재해석해 순회할 포인터 */
	sector_t start = 0;	/* [한국어] 직전 엔트리의 시작 섹터를 기억해 두는 변수 -- 다음 엔트리와의 차이로 크기를 역산하는 데 사용 */
	int i, slot = 1;	/* [한국어] i=엔트리 순회 인덱스, slot=다음에 등록할 parsed_partitions 슬롯 인덱스(0번은 예약 슬롯) */

	data = read_part_sector(state, 7, &sect);	/* [한국어] EESOX 테이블은 섹터 7에 고정 위치 */
	if (!data)	/* [한국어] 섹터 읽기 실패 */
		return -1;	/* [한국어] 치명적 오류로 스캔 중단 */

	/*
	 * "Decrypt" the partition table.  God knows why...
	 */
	for (i = 0; i < 256; i++)	/* [한국어] 섹터 512바이트 중 실제 테이블이 담긴 앞 256바이트만 복호화 대상으로 순회 */
		buffer[i] = data[i] ^ eesox_name[i & 15];	/* [한국어] 16바이트 키를 (i & 15)로 반복 적용하는 XOR 복호화 -- 대칭 연산이므로 같은 키로 암호화도 복호화도 가능 */

	put_dev_sector(sect);	/* [한국어] 복호화에 필요한 원본 바이트를 buffer로 이미 복사했으므로 원본 섹터 버퍼는 더 이상 필요 없어 해제 */

	for (i = 0, p = (struct eesox_part *)buffer; i < 8; i++, p++) {	/* [한국어] 복호화된 buffer를 고정 8개 엔트리 배열로 보고 순서대로 순회(포인터를 엔트리 크기만큼 전진) */
		sector_t next;	/* [한국어] 이번 엔트리의 시작 섹터(다음 반복에서는 start로 이월됨) */

		if (memcmp(p->magic, "Eesox", 6))	/* [한국어] 시그니처가 'Eesox'와 다르면 유효한 엔트리가 아님 */
			break;	/* [한국어] 배열의 유효 엔트리가 끝났다고 보고 순회 중단 */

		next = le32_to_cpu(p->start);	/* [한국어] 이번 엔트리의 시작 섹터를 호스트 바이트 순서로 변환 */
		if (i)	/* [한국어] 첫 번째 엔트리(i == 0)는 아직 비교할 '직전 시작 섹터'가 없으므로 건너뛰고, 두 번째 엔트리부터 등록 */
			put_partition(state, slot++, start, next - start);	/* [한국어] 직전 엔트리(start)를, 이번 엔트리 시작(next)과의 차이를 크기로 삼아 등록하고 slot 전진 */
		start = next;	/* [한국어] 다음 반복에서 비교할 수 있도록 이번 시작 섹터를 기억해 둠 */
	}

	if (i != 0) {	/* [한국어] 순회 도중 하나라도 유효한 EESOX 엔트리를 찾았다면(i가 0에서 증가했다면) */
		sector_t size;	/* [한국어] 마지막으로 발견된 파티션의 크기(디스크 끝까지) */

		size = get_capacity(state->disk);	/* [한국어] 디스크 전체 용량(섹터 단위)을 가져와 */
		put_partition(state, slot++, start, size - start);	/* [한국어] 마지막 파티션은 다음 엔트리가 없으므로 '디스크 끝 - 시작 섹터'를 크기로 등록 */
		seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 진단 로그 한 줄을 줄바꿈으로 마무리(엔트리를 하나도 못 찾았으면 이 줄바꿈도 출력되지 않음) */
	}

	return i ? 1 : 0;	/* [한국어] 유효 엔트리를 하나라도 찾았으면(i != 0) 1, 전혀 못 찾았으면 0을 반환 */
}
#endif
/* [한국어] struct eesox_part 및 adfspart_check_EESOX()를 감쌌던 CONFIG_ACORN_PARTITION_EESOX 게이트 종료 -- 파일의 마지막 조건부 컴파일 블록 */
