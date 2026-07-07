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
 * [한국어 설명] DEC OSF/1(Digital UNIX, 이후 Tru64 UNIX) 디스크레이블 파서 (osf.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 DEC(Digital Equipment Corporation) Alpha 워크스테이션에서 동작하던
 * OSF/1(이후 Digital UNIX로, 다시 이후 Tru64 UNIX로 개명된 운영체제)이 디스크에
 * 기록하는 BSD 계열 disklabel(디스크 레이블) 포맷을 인식하고, 그 안에 정의된
 * 파티션 항목들을 커널의 공용 파티션 스캔 상태(struct parsed_partitions)에
 * 등록하는 단 하나의 함수 osf_partition()을 정의한다. OSF/1 disklabel은 4.4BSD
 * 의 disklabel 구조를 기반으로 하되 파티션 개수를 확장한 변형(이 파일 기준
 * MAX_OSF_PARTITIONS=18)으로, 디스크 첫 섹터(LBA 0) 안에서 64바이트 오프셋
 * 위치에 기록된다(부트 코드가 차지하는 앞부분 64바이트를 피하는 4.4BSD 계열의
 * 관례). DEC Alpha 하드웨어가 사실상 단종된 오늘날에도, 그 하드웨어에서 만들어진
 * 디스크 이미지나 레거시 볼륨을 리눅스에서 인식해야 하는 경우가 있어 유지되는
 * 레거시(legacy) 코드다. 파일 자체는 원래 drivers/block/genhd.c에 있던 범용
 * 제네릭 디스크 코드에서 추출된 것으로(파일 상단 원본 영어 주석 참고), 리누스
 * 토르발스가 1991~1998년에 작성하고 러셀 킹이 1998년에 재구성했다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * gendisk(범용 디스크)가 add_disk()로 등록되거나 BLKRRPART 재스캔 ioctl이
 * 발생하면 block/partitions/core.c의 bdev_disk_changed() -> blk_add_partitions()
 * -> check_partition() 순으로 호출 체인이 이어지고, check_partition()은 static
 * 배열 check_part[]에 등재된 20여 개의 포맷별 프로버(msdos_partition,
 * efi_partition, sun_partition 등)를 배열 순서대로 하나씩 호출한다. 이 파일의
 * osf_partition()은 CONFIG_OSF_PARTITION이 활성화된 경우에만 그 배열에
 * 포함되며(block/partitions/core.c의 check_part[] 정의부 참고), msdos_partition
 * 다음, sun_partition 이전 위치에 놓인다. 각 프로버는 자신이 인식하는 포맷의
 * 시그니처가 아니면 0을 반환해 다음 프로버로 넘기고, 시그니처는 찾았으나 이후
 * 처리 중 I/O 오류가 나면 음수를 반환하며, 성공적으로 파티션을 등록했으면
 * 1(양수)을 반환한다 - osf_partition()도 이 계약을 그대로 따른다. 실행
 * 컨텍스트는 디스크 최초 인식 또는 사용자가 요청한 재스캔을 처리하는 단일
 * 프로세스 컨텍스트이며, 인터럽트나 별도 커널 스레드가 아니라
 * blk_add_partitions() 호출자와 동일한 컨텍스트에서 동기적으로 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 block/partitions/check.h가 선언하는 공용 인터페이스에 전적으로
 * 의존한다: struct parsed_partitions(스캔 세션 상태), Sector(섹터 버퍼 래퍼),
 * read_part_sector()/put_dev_sector()(섹터 읽기/반납), put_partition()(파티션
 * 등록)이 그것이다. 데이터는 read_part_sector()를 통해 대상 블록 장치(gendisk)
 * 의 첫 섹터가 페이지 캐시(캐시 미스 시 하위 블록 드라이버의 실제 I/O)로부터
 * 커널 가상 주소로 흘러 들어오고, 이 파일이 그 바이트를 struct disklabel로
 * 재해석(오버레이)한 뒤, 검증을 통과한 파티션 좌표(offset/size)가 다시
 * put_partition()을 거쳐 state->parts[]로 흘러 나간다. 이 파일이 직접 의존하는
 * 상위 모듈은 없으며(다른 파일이 osf_partition()을 직접 호출하지 않고, 오직
 * core.c의 함수 포인터 배열을 통해서만 간접 호출된다), 이 파일에 의존하는
 * 쪽은 block/partitions/core.c 하나뿐이다. 공유하는 핵심 자료구조는 struct
 * parsed_partitions(check.h 정의)이며, 이 파일 자체가 정의하는 struct
 * disklabel/struct d_partition은 osf_partition() 함수 내부에서만 쓰이는
 * 지역(local) 타입으로 다른 파일과 공유되지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - osf_partition(): 이 파일의 유일한 함수. LBA 0을 읽어 offset 64에서
 *   disklabel을 찾고, 매직 두 개(d_magic/d_magic2)를 검증한 뒤 d_partitions[]
 *   배열을 순회하며 유효한 항목을 put_partition()으로 등록한다.
 * - struct disklabel: OSF/1 disklabel 전체를 표현하는 지역 구조체. 매직/타입
 *   이름/기하(CHS) 정보/파티션 개수/체크섬 등 BSD 계열 disklabel의 전형적인
 *   필드와 함께, 파티션 배열(d_partitions[])을 포함한다.
 * - struct d_partition: disklabel 안에 중첩된, 파티션 하나의 크기/시작 위치/
 *   파일시스템 타입 등을 담는 16바이트 고정 크기 구조체.
 * - MAX_OSF_PARTITIONS(18): d_partitions[] 배열의 컴파일 타임 크기이자
 *   d_npartitions 필드에 대한 상한 검증 기준.
 * - DISKLABELMAGIC(0x82564557): disklabel의 유효성을 나타내는 매직 넘버로,
 *   d_magic과 d_magic2 두 위치에 중복 저장되어 있어야 유효한 레이블로
 *   인정된다.
 */

#include "check.h" /* [한국어] parsed_partitions/Sector 타입과 read_part_sector()/put_dev_sector()/put_partition() 등 파티션 스캔 공용 인터페이스 선언 - 이 파일이 유일하게 의존하는 헤더 */

#define MAX_OSF_PARTITIONS 18 /* [한국어] disklabel 안의 d_partitions[] 배열 길이(=이 파서가 인식하는 최대 파티션 수). d_npartitions가 이 값을 넘으면 손상된 레이블로 간주해 거부한다 */
#define DISKLABELMAGIC (0x82564557UL) /* [한국어] OSF/1 disklabel의 매직 넘버. d_magic과 d_magic2 두 필드가 모두 이 값과 같아야 유효한 OSF/1 disklabel로 인정한다(UL 접미사: unsigned long 리터럴) */

/*
 * [한국어]
 * osf_partition() - OSF/1(Digital UNIX) disklabel을 인식해 파티션을 등록한다.
 *
 * @state: 스캔 세션 상태(parsed_partitions). state->disk에서 대상 gendisk를
 *         얻어 read_part_sector()로 LBA 0을 읽고, 검증을 통과한 파티션들을
 *         state->parts[]/state->pp_buf에 기록하는 데 쓰인다. state->limit은
 *         parts[] 배열의 상한으로, 파티션 등록 루프의 방어적 종료 조건에
 *         쓰인다.
 * @return: 1 = OSF/1 disklabel을 인식하고 파티션 등록을 마쳤음(성공).
 *          0 = 이 섹터에 OSF/1 disklabel이 없음(매직 불일치, 또는 파티션
 *          개수가 MAX_OSF_PARTITIONS를 초과) - check_partition()이 다음 포맷
 *          프로버를 계속 시도하게 하는 값이다. -1 = LBA 0을 읽는 데 실패함
 *          (I/O 오류 또는 EOD 초과) - check_partition()은 이 값을 음수로
 *          인식해 누적 에러로 기억해 두고 마찬가지로 다음 프로버를 계속
 *          시도한다.
 *
 * 목적: DEC Alpha 워크스테이션에서 OSF/1이 만든 볼륨을 리눅스가 인식하려면
 * PC의 MBR이나 다른 BSD의 disklabel과는 오프셋/매직이 다른, OSF/1 고유의
 * disklabel 레이아웃을 별도로 해석해야 한다. 이 함수는 그 해석 로직 전체를
 * 담당하는 이 파일의 유일한 진입점이다.
 * 동작 과정:
 *  (1) read_part_sector()로 LBA 0(디스크의 첫 논리 섹터)을 읽는다. 읽기
 *      자체가 실패하면(디스크 끝을 넘어서거나 I/O 오류) 다른 검사 없이
 *      즉시 -1을 반환해 호출자에게 I/O 오류를 알린다.
 *  (2) 읽어온 버퍼의 시작 주소(data)에서 64바이트 뒤(data+64)를 struct
 *      disklabel으로 캐스팅한다 - OSF/1(4.4BSD 계열) disklabel은 부트
 *      코드가 차지하는 앞부분 64바이트 다음에 위치하는 관례를 따른다.
 *  (3) d_magic이 DISKLABELMAGIC과 다르면 이 섹터에는 OSF/1 disklabel이
 *      없다고 판단해 섹터 버퍼를 반납하고 0을 반환한다(다른 프로버가
 *      이어서 시도하도록).
 *  (4) d_magic2(구조체 끝부분에 중복 저장된 보조 매직)도 같은 값인지
 *      확인한다 - 매직 하나만으로는 우연한 바이트 패턴 일치를 완전히
 *      배제할 수 없으므로 이중으로 검증한다.
 *  (5) d_npartitions가 MAX_OSF_PARTITIONS(18, d_partitions[] 배열의 컴파일
 *      타임 크기)를 넘으면 레이블이 손상되었거나 이 코드가 알지 못하는
 *      확장 포맷이라고 보고 0을 반환한다.
 *  (6) d_partitions[] 배열을 처음부터 d_npartitions개만큼 순회하며,
 *      p_size가 0이 아닌 항목만 put_partition()으로 등록한다(크기 0인
 *      항목은 「사용되지 않는 슬롯」으로 간주해 건너뛴다). 슬롯 번호(slot)는
 *      항목이 실제 등록되었는지와 무관하게 매 반복마다 증가하며,
 *      state->limit에 도달하면 더 등록할 공간이 없으므로 루프를 조기
 *      종료한다.
 *  (7) 로그 문자열(pp_buf)에 개행을 덧붙이고 섹터 버퍼를 반납한 뒤 1을
 *      반환해 성공을 알린다.
 * 실행 컨텍스트: check_partition()과 동일한 단일 프로세스 컨텍스트에서
 * 동기적으로 실행되며, 이 함수 자체는 별도의 락이나 원자적 연산을 쓰지
 * 않는다 - 한 번의 파티션 스캔은 항상 하나의 스레드가 순차적으로 수행하기
 * 때문에 동시성 문제가 없다.
 * 호출자(caller): check_partition()(block/partitions/core.c)이 check_part[]
 * 함수 포인터 배열을 통해 간접 호출한다(CONFIG_OSF_PARTITION 활성화 시에만
 * 배열에 포함됨).
 * 피호출자(callee): read_part_sector(), put_dev_sector(), put_partition(),
 * seq_buf_puts(), le32_to_cpu()/le16_to_cpu()(리틀 엔디언 -> 호스트 바이트
 * 순서 변환).
 * 에러 처리: read_part_sector() 실패만 -1(I/O 오류)로 구분해 즉시 반환하고,
 * 그 외 「이 포맷이 아님/손상됨」 판정은 모두 0으로 통일해 반환한다. -1
 * 경로를 제외한 모든 조기 반환(0을 반환하는 세 지점)은 공통적으로
 * put_dev_sector()로 앞서 확보한 섹터 버퍼를 먼저 반납한 뒤 반환한다.
 *
 * 호출 체인:
 *   check_partition() → [osf_partition] → read_part_sector() /
 *     put_partition() / put_dev_sector() / seq_buf_puts()
 */
int osf_partition(struct parsed_partitions *state) /* [한국어] osf_partition() 함수 시작 - check_part[] 배열을 통해 check_partition()이 간접 호출 */
{
	int i; /* [한국어] d_partitions[] 순회 인덱스(0..npartitions-1). for 루프에서만 쓰인다 */
	int slot = 1; /* [한국어] 다음에 등록할 파티션 번호(슬롯). 1부터 시작 - 관례상 슬롯 0은 비워 두고 파티션 번호를 1번부터 부여한다(state->parts[0]은 이 함수가 채우지 않음) */
	unsigned int npartitions; /* [한국어] disklabel에 기록된 유효 파티션 개수(d_npartitions를 호스트 바이트 순서로 변환한 값). MAX_OSF_PARTITIONS 초과 여부 검사와 순회 루프의 상한으로 쓰인다 */
	Sector sect; /* [한국어] read_part_sector()가 채워 주는 출력 파라미터 - 내부적으로 folio 포인터 하나를 담는 얇은 래퍼. put_dev_sector(sect)로 반드시 반납해야 한다 */
	unsigned char *data; /* [한국어] read_part_sector()의 반환값 - LBA 0 섹터 데이터가 시작하는 커널 가상 주소. NULL이면 읽기 실패를 의미하며 이후 disklabel로 캐스팅해서는 안 된다 */
	/*
	 * [한국어]
	 * struct disklabel - OSF/1(Digital UNIX) disklabel 전체를 표현하는 지역 구조체.
	 *
	 * 이 구조체는 osf_partition() 함수 내부에서만 쓰이는 지역(local) 타입으로, 디스크의
	 * LBA 0 + 64바이트 위치에 실제로 기록되어 있는 원시 바이트를 그대로 겹쳐 씌우는
	 * (overlay) 용도로만 쓰인다. 즉 이 구조체의 어떤 필드도 이 커널 코드가 직접
	 * 대입(write)하지 않으며, 모든 필드는 DEC Alpha 위의 OSF/1이 disklabel(8) 계열
	 * 유틸리티로 디스크에 이미 써 놓은 값을 읽기만(read-only) 한다. 각 필드는
	 * __le32/__le16(고정 리틀 엔디언) 타입으로 선언되어 있어, 호스트가 빅 엔디언
	 * 아키텍처이더라도 le32_to_cpu()/le16_to_cpu()로 명시적으로 변환해야 올바른 값을
	 * 얻는다(DEC Alpha 자체는 리틀 엔디언이지만, 이 디스크 이미지를 다른 아키텍처의
	 * 리눅스 머신에서 읽을 수도 있으므로 변환이 필요하다). 아래 각 필드 옆에 표기한
	 * 바이트 오프셋은 이 구조체에 __packed 지정이 없으므로 표준 정렬 규칙에 따라
	 * 컴파일러가 배치하는 실제 오프셋이며(offsetof 계산으로 검증됨), 필드 나열 순서가
	 * 이미 각 타입의 자연 정렬 경계에 맞아떨어지므로 패딩 바이트는 발생하지 않는다.
	 * 구조체 전체 크기는 436바이트(0x1b4)로, offset 64에서 시작해도 512바이트 섹터
	 * 안에 완전히 들어간다.
	 */
	struct disklabel {
		__le32 d_magic;
		/* [한국어] disklabel 유효성 매직 넘버 (오프셋 0x00, disklabel 구조체의 첫 필드).
		 * 설정자: 커널이 아니라 DEC Alpha의 OSF/1이 disklabel을 생성할 때 디스크에
		 *   기록한 값 - 이 구조체는 그 바이트를 겹쳐 읽기(overlay)만 한다.
		 * 읽는 자: osf_partition()이 le32_to_cpu(label->d_magic)로 읽어
		 *   DISKLABELMAGIC(0x82564557)과 비교하고, 다르면 이 섹터를 OSF/1 disklabel이
		 *   아니라고 판단해 0을 반환한다.
		 * 값 범위: 정상적인 OSF/1 disklabel이면 항상 DISKLABELMAGIC과 동일해야 한다.
		 *   그 외 값은 다른 포맷이거나 손상된 데이터를 의미한다.
		 * 동기화: 파티션 스캔 도중 한 번 읽히는 읽기 전용 스냅샷이므로 락이 필요 없다. */

		__le16 d_type,d_subtype;
		/* [한국어] 디스크 드라이브 종류(d_type, 예: SCSI/ST506 등을 나타내는 OSF/1 정의
		 * enum 값)와 그 세부 종류(d_subtype)를 나타내는 필드 쌍 (오프셋 0x04 / 0x06).
		 * 설정자: OSF/1의 disklabel 생성 유틸리티가 디스크 드라이버 종류에 따라 기록.
		 *   이 커널 파서는 이 필드에 절대 쓰지 않는다.
		 * 읽는 자: 이 파일의 osf_partition()은 이 필드를 전혀 참조하지 않는다 - 파티션
		 *   등록에는 필요 없는 정보이며, 4.4BSD 계열 disklabel.h의 구조체 레이아웃을
		 *   그대로 이식하는 과정에서 남아 있는 필드다(추정).
		 * 값 범위: OSF/1이 정의한 드라이브 타입 enum 값 - 이 파일은 해석하지 않는다.
		 * 동기화: 읽기 전용, 락 불필요. */

		u8 d_typename[16];
		/* [한국어] 디스크(드라이브 모델 등)를 식별하는 이름 문자열 (오프셋 0x08, 16바이트).
		 * 설정자: disklabel(8) 생성 시 OSF/1이 기록. NUL 종단이 보장되지 않을 수 있는
		 *   고정 길이 ASCII 필드다(4.4BSD 관례).
		 * 읽는 자: osf_partition()은 참조하지 않는다 - 파티션 등록 로직과 무관한 표시용
		 *   메타데이터.
		 * 값 범위: 최대 16바이트 ASCII 문자열(길이 초과 시 잘림, NUL 없을 수 있음).
		 * 동기화: 읽기 전용, 락 불필요. */

		u8 d_packname[16];
		/* [한국어] 디스크 팩/볼륨의 이름 문자열 (오프셋 0x18, 16바이트) - d_typename과 같은
		 * 형식이나 대상이 물리 드라이브가 아니라 교체 가능한 디스크 팩을 가리킨다.
		 * 설정자: disklabel(8) 생성 시 OSF/1이 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 최대 16바이트 ASCII 문자열.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_secsize;
		/* [한국어] 물리 섹터 크기(바이트 단위) (오프셋 0x28) - 이 disklabel이 기록될 당시
		 * 디스크의 논리 섹터 크기(전형적으로 512).
		 * 설정자: disklabel(8) 생성 시 OSF/1이 실제 드라이브 지오메트리를 조회해 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다 - 커널은 블록 장치 자체의 논리 섹터
		 *   크기(queue_logical_block_size() 등)를 따로 알고 있으므로 이 값을 신뢰하지
		 *   않는다.
		 * 값 범위: 바이트 단위 양의 정수(전형적으로 512).
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_nsectors;
		/* [한국어] 트랙 당 섹터 수 (오프셋 0x2c) - CHS(실린더/헤드/섹터) 지오메트리의
		 * 구성 요소.
		 * 설정자: disklabel(8)이 드라이브 지오메트리 조회 결과로 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다 - 파티션 offset/size는 이미 섹터
		 *   단위 절대값(d_partitions[].p_offset/p_size)으로 저장되어 있어 CHS 환산이
		 *   필요 없다.
		 * 값 범위: 양의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_ntracks;
		/* [한국어] 실린더 당 트랙(=헤드) 수 (오프셋 0x30) - CHS 지오메트리 구성 요소.
		 * 설정자: disklabel(8)이 드라이브 지오메트리 조회 결과로 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 양의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_ncylinders;
		/* [한국어] 디스크 전체의 실린더 수 (오프셋 0x34) - CHS 지오메트리 구성 요소.
		 * 설정자: disklabel(8)이 드라이브 지오메트리 조회 결과로 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 양의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_secpercyl;
		/* [한국어] 실린더 당 섹터 수 (오프셋 0x38, 통상 d_nsectors * d_ntracks와 같음) -
		 * CHS <-> LBA 환산에 쓰이는 파생 지오메트리 값.
		 * 설정자: disklabel(8)이 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 양의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_secprtunit;
		/* [한국어] 유닛(디스크 전체) 당 총 섹터 수 (오프셋 0x3c) - 디스크의 전체 용량을
		 * 섹터 단위로 나타낸다.
		 * 설정자: disklabel(8)이 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다 - 커널은 gendisk의 실제 용량
		 *   (get_capacity())을 별도로 알고 있다.
		 * 값 범위: 양의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le16 d_sparespertrack;
		/* [한국어] 트랙 당 예비(스페어) 섹터 수 (오프셋 0x40) - 배드섹터 대체용으로
		 * 예약된 섹터 수(레거시 하드디스크의 배드섹터 재배치 방식).
		 * 설정자: disklabel(8)이 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 0 이상의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le16 d_sparespercyl;
		/* [한국어] 실린더 당 예비(스페어) 섹터 수 (오프셋 0x42) - d_sparespertrack과 같은
		 * 목적의 실린더 단위 버전.
		 * 설정자: disklabel(8)이 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 0 이상의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_acylinders;
		/* [한국어] 예비(대체)용으로 예약된 실린더 수 (오프셋 0x44) - 배드섹터 재배치
		 * 영역으로 쓰이는 디스크 끝부분의 실린더 개수.
		 * 설정자: disklabel(8)이 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 0 이상의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le16 d_rpm, d_interleave, d_trackskew, d_cylskew;
		/* [한국어] 레거시 물리 디스크 파라미터 4종을 한 줄에 선언한 필드들:
		 *   d_rpm(오프셋 0x48)         - 스핀들 회전 속도(분당 회전수).
		 *   d_interleave(오프셋 0x4a)  - 섹터 인터리브 계수(연속 섹터 사이의 논리적 간격).
		 *   d_trackskew(오프셋 0x4c)   - 트랙 간 시작 섹터 오프셋(스큐).
		 *   d_cylskew(오프셋 0x4e)     - 실린더 간 시작 섹터 오프셋(스큐).
		 * 설정자: disklabel(8)이 드라이브 지오메트리 조회 결과로 기록.
		 * 읽는 자: osf_partition()은 네 필드 모두 참조하지 않는다 - 회전 매체의 seek/
		 *   rotational latency 최적화용 힌트로, 플래시/최신 디스크 컨트롤러에서는
		 *   의미가 없다(추정).
		 * 값 범위: 각각 0 이상의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_headswitch, d_trkseek, d_flags;
		/* [한국어] 레거시 타이밍/플래그 필드 3종:
		 *   d_headswitch(오프셋 0x50) - 헤드 전환에 걸리는 지연 시간.
		 *   d_trkseek(오프셋 0x54)    - 인접 트랙 탐색(seek)에 걸리는 지연 시간.
		 *   d_flags(오프셋 0x58)      - 드라이브 특성을 나타내는 비트 플래그(제거 가능
		 *     매체 여부 등, OSF/1 정의).
		 * 설정자: disklabel(8)이 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: d_headswitch/d_trkseek은 시간 단위 정수, d_flags는 OSF/1 정의
		 *   비트마스크.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_drivedata[5];
		/* [한국어] 드라이브 유형별 고유 데이터 5워드(20바이트) (오프셋 0x5c) - OSF/1
		 * 드라이버가 특정 드라이브 모델에 필요한 부가 정보를 자유 형식으로 담아 두는
		 * 영역.
		 * 설정자: disklabel(8)/드라이브별 드라이버가 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 드라이브 종류에 따라 의미가 다른 불투명(opaque) 데이터.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_spare[5];
		/* [한국어] 예약(reserved) 필드 5워드(20바이트) (오프셋 0x70) - 향후 확장을 위해
		 * 비워 둔 영역.
		 * 설정자: 통상 0으로 채워지며, disklabel(8)이 특별히 쓰지 않는 한 의미 있는
		 *   값이 들어있지 않다(추정).
		 * 읽는 자: osf_partition()은 참조하지 않는다.
		 * 값 범위: 정의되지 않음(예약).
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_magic2;
		/* [한국어] d_magic과 동일한 값이 기록되어야 하는 보조/중복 매직 (오프셋 0x84,
		 * 구조체의 뒷부분에 위치) - disklabel의 무결성을 이중으로 검증하기 위한 필드.
		 * 설정자: OSF/1이 disklabel 생성 시 d_magic과 동일한 값을 여기에도 기록.
		 * 읽는 자: osf_partition()이 le32_to_cpu(label->d_magic2)로 읽어
		 *   DISKLABELMAGIC과 비교 - d_magic 검사를 통과한 뒤에도 이 값이 다르면 여전히
		 *   0을 반환해 「이 포맷 아님」으로 처리한다(매직 하나만 우연히 일치하는
		 *   오탐(false positive)을 줄이기 위함).
		 * 값 범위: 정상 레이블이면 DISKLABELMAGIC과 동일.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le16 d_checksum;
		/* [한국어] disklabel 전체에 대한 체크섬 값 (오프셋 0x88) - OSF/1 disklabel(8)
		 * 유틸리티가 레이블 내용이 손상되지 않았는지 검증하기 위해 계산해 두는 값.
		 * 설정자: disklabel(8)이 레이블을 쓸 때 계산해 기록.
		 * 읽는 자: 이 파일의 osf_partition()은 이 필드를 전혀 검증하지 않는다 - 매직
		 *   두 개(d_magic/d_magic2) 일치만으로 유효성을 판단하며, 체크섬 알고리즘
		 *   자체를 이 파서가 구현하고 있지 않기 때문이다(추정: 리눅스 이식 과정에서
		 *   생략된 것으로 보인다).
		 * 값 범위: OSF/1의 체크섬 알고리즘이 계산한 임의의 16비트 값.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le16 d_npartitions;
		/* [한국어] d_partitions[] 배열에서 유효한(정의된) 파티션 항목의 개수 (오프셋 0x8a).
		 * 설정자: disklabel(8)/디스크를 처음 구성한 관리자가 파티션 테이블을 편집할 때
		 *   기록.
		 * 읽는 자: osf_partition()이 le16_to_cpu(label->d_npartitions)로 읽어
		 *   지역 변수 npartitions에 저장한 뒤, (a) MAX_OSF_PARTITIONS(18) 초과 여부
		 *   검사, (b) d_partitions[] 순회 루프의 상한으로 사용한다.
		 * 값 범위: 0 이상 MAX_OSF_PARTITIONS(18) 이하여야 정상. 초과하면 osf_partition()
		 *   이 레이블을 거부(0 반환)한다.
		 * 동기화: 읽기 전용, 락 불필요. */

		__le32 d_bbsize, d_sbsize;
		/* [한국어] 파일시스템 예약 영역 크기 2종:
		 *   d_bbsize(오프셋 0x8c) - 부트 블록(boot block)에 예약된 바이트 수.
		 *   d_sbsize(오프셋 0x90) - 슈퍼블록(superblock)에 예약된 바이트 수.
		 * 설정자: 각 파티션에 파일시스템을 생성(newfs 계열)할 때 기록.
		 * 읽는 자: osf_partition()은 참조하지 않는다 - 파티션의 시작 위치/크기 등록과
		 *   무관하며, 실제 파일시스템 마운트 시점에 상위 VFS/파일시스템 드라이버가
		 *   참고할 정보다(이 커널 트리에는 해당 파일시스템 드라이버가 없다).
		 * 값 범위: 바이트 단위 양의 정수.
		 * 동기화: 읽기 전용, 락 불필요. */

		/*
		 * [한국어]
		 * struct d_partition - disklabel 안에 중첩된, 파티션 하나를 표현하는 구조체.
		 *
		 * struct disklabel과 마찬가지로 디스크 원본 바이트를 겹쳐 읽는(overlay) 지역
		 * 타입이며, d_partitions[MAX_OSF_PARTITIONS] 배열의 원소 타입으로 쓰인다. 각
		 * 필드는 여기 struct d_partition 기준의 상대 오프셋으로 표기했다(offsetof 계산
		 * 으로 검증됨). 이 구조체 하나의 크기는 16바이트로, p_cpg 뒤에 패딩 없이 정확히
		 * 4바이트 배수로 맞아떨어진다.
		 */
		struct d_partition {
			__le32 p_size;
			/* [한국어] 이 파티션의 크기(섹터 단위 길이) (구조체 내 오프셋 0x00).
			 * 설정자: 디스크 관리자가 disklabel(8) 계열 유틸리티로 파티션 테이블을 편집할
			 *   때 기록 - 커널의 이 파서는 쓰지 않고 읽기만 한다.
			 * 읽는 자: osf_partition()의 for 루프가 le32_to_cpu(partition->p_size)로 읽어
			 *   0이 아닌지 검사한 뒤, 0이 아니면 그 값 그대로 put_partition()의 size
			 *   인자로 전달한다.
			 * 값 범위: 0(미사용 슬롯 표시) 또는 양의 섹터 수. 0이면 이 항목은 등록되지
			 *   않고 건너뛴다.
			 * 동기화: 읽기 전용, 락 불필요. */

			__le32 p_offset;
			/* [한국어] 이 파티션이 시작하는 절대 LBA(논리 블록 주소) (구조체 내 오프셋 0x04).
			 * 설정자: 디스크 관리자가 disklabel(8) 계열 유틸리티로 기록.
			 * 읽는 자: osf_partition()이 le32_to_cpu(partition->p_offset)로 읽어
			 *   put_partition()의 from(시작 LBA) 인자로 그대로 전달한다.
			 * 값 범위: 0 이상, 디스크 전체 용량 미만이어야 정상.
			 * 동기화: 읽기 전용, 락 불필요. */

			__le32 p_fsize;
			/* [한국어] 이 파티션에 만들어질 파일시스템의 조각(fragment) 크기 (구조체 내
			 * 오프셋 0x08) - 4.4BSD 계열 FFS(Fast File System)의 블록 하위 단위 크기.
			 * 설정자: newfs 계열 유틸리티가 파일시스템 생성 시 기록.
			 * 읽는 자: osf_partition()은 참조하지 않는다 - 파티션 등록(offset/size)과
			 *   무관하며, 실제 FFS 드라이버가 마운트 시점에 참고할 정보다(이 커널
			 *   트리에는 해당 파일시스템 드라이버가 없다).
			 * 값 범위: 바이트 단위 양의 정수(전형적으로 512의 배수).
			 * 동기화: 읽기 전용, 락 불필요. */

			u8  p_fstype;
			/* [한국어] 이 파티션에 담긴 파일시스템의 종류를 나타내는 열거값 (구조체 내 오프셋
			 * 0x0c, 1바이트) - 4.4BSD 계열의 FS_UNUSED/FS_BSDFFS/FS_SWAP 등에 대응하는
			 * OSF/1판 정의(추정).
			 * 설정자: disklabel(8)/newfs 유틸리티가 기록.
			 * 읽는 자: osf_partition()은 참조하지 않는다 - 이 파서는 파티션의 위치/크기만
			 *   등록하고, 파일시스템 종류 판별은 상위(마운트 시점의 슈퍼블록 매직 검사)에
			 *   맡긴다.
			 * 값 범위: OSF/1이 정의한 파일시스템 타입 enum 값 1바이트.
			 * 동기화: 읽기 전용, 락 불필요. */

			u8  p_frag;
			/* [한국어] p_fsize에 대한 파일시스템 블록 크기 배수(조각 개수) (구조체 내 오프셋
			 * 0x0d, 1바이트) - FFS의 fragments-per-block 값.
			 * 설정자: newfs 유틸리티가 기록.
			 * 읽는 자: osf_partition()은 참조하지 않는다.
			 * 값 범위: 전형적으로 1/2/4/8 등 2의 거듭제곱.
			 * 동기화: 읽기 전용, 락 불필요. */

			__le16 p_cpg;
			/* [한국어] 이 파티션의 파일시스템이 사용하는 실린더 그룹(cylinder group) 당
			 * 실린더 수 (구조체 내 오프셋 0x0e, 2바이트) - FFS의 실린더 그룹 레이아웃
			 * 파라미터.
			 * 설정자: newfs 유틸리티가 기록.
			 * 읽는 자: osf_partition()은 참조하지 않는다.
			 * 값 범위: 양의 정수.
			 * 동기화: 읽기 전용, 락 불필요. */

		} d_partitions[MAX_OSF_PARTITIONS]; /* [한국어] struct d_partition 종료 + disklabel의 파티션 테이블 배열 필드
		 * (오프셋 0x94, 18 * 16 = 288바이트) - 이 배열의 인덱스 0..d_npartitions-1이
		 * osf_partition()이 순회하는 대상이다. 설정자: disklabel(8)이 각 파티션을
		 * 편집할 때 기록. 읽는 자: osf_partition()의 for 루프가 partition 포인터를
		 * 증가시키며 순회. 값 범위: MAX_OSF_PARTITIONS(18)개 고정 크기 배열 - 이
		 * 컴파일 타임 크기가 바로 d_npartitions에 대한 상한 검사 근거다. 동기화:
		 * 읽기 전용, 락 불필요. */
	} * label; /* [한국어] struct disklabel 정의 종료 + 그 포인터 지역 변수 label 선언.
	 * label은 아직 초기화되지 않은 채 선언만 된 상태이며, 실제 값은 뒤의
	 * 「label = (struct disklabel *) (data+64);」 대입에서 채워진다(=읽어온 섹터
	 * 버퍼 위에 겹쳐 씌우는 재해석 포인터). */
	struct d_partition * partition; /* [한국어] d_partitions[] 배열을 순회하기 위한 커서 포인터. 초기값은 뒤에서
	 * 「partition = label->d_partitions;」로 배열의 첫 원소를 가리키도록 설정되고,
	 * for 루프의 partition++ 로 다음 원소로 전진한다. */

	/*
	 * LBA 0(디스크 첫 논리 섹터) 읽기: 이 섹터 안에 OSF/1 disklabel이 있는지 확인
	 * 하기 위한 원시 바이트가 필요하다. read_part_sector()는 대상 gendisk의
	 * page-cache 매핑을 통해 이 섹터를 읽어(캐시 미스 시 하위 블록 드라이버의
	 * 실제 I/O를 유발) 커널 가상 주소를 반환한다.
	 */
	data = read_part_sector(state, 0, &sect); /* [한국어] state->disk의 LBA 0을 읽어 커널 가상 주소를 data에, folio 참조를 sect에 저장 - 실패 시 NULL 반환 및 sect는 반납 불필요한 상태로 유지 */
	if (!data) /* [한국어] 읽기 자체가 실패했는지 검사 - EOD(디스크 끝) 초과 또는 페이지 캐시/블록 계층 I/O 오류일 때 data가 NULL이 된다 */
		/* [한국어] 읽기 실패는 다른 프로버가 재시도해도 마찬가지로 실패할 가능성이 높은
		 * I/O 오류이므로, 매직 검사조차 시도하지 않고 즉시 -1(check_partition()이
		 * 음수로 인식해 누적 에러로 기억)을 반환한다. 이 경로는 read_part_sector()가
		 * 이미 실패했으므로 반납할 섹터 버퍼가 없다(put_dev_sector() 호출 없음에 유의). */
		return -1; /* [한국어] -1(음수) 반환 - check_partition()이 I/O 오류로 인식해 err에 기억해 두고 다음 프로버를 계속 시도하게 함 */

	/*
	 * OSF/1(4.4BSD 계열) disklabel은 섹터의 맨 앞(오프셋 0)이 아니라 오프셋 64바이트
	 * 위치에서 시작한다 - 앞의 64바이트는 부트 코드가 차지하는 영역이기 때문이다.
	 * 따라서 읽어온 섹터 버퍼(data)에 64를 더한 주소를 disklabel 구조체로
	 * 재해석(캐스팅)한다.
	 */
	label = (struct disklabel *) (data+64); /* [한국어] data+64 지점을 struct disklabel로 재해석(overlay) - 이 대입 이후
	 * label->필드 접근이 그 오프셋의 원시 바이트를 해당 타입으로 읽어온다 */
	partition = label->d_partitions; /* [한국어] partition 커서를 d_partitions[] 배열의 첫 원소(인덱스 0)로 초기화 - 아래 for 루프가 이 포인터를 증가시키며 각 항목을 순회한다 */
	/*
	 * 1차 매직 검사: d_magic이 DISKLABELMAGIC과 다르면 이 섹터에는 OSF/1 disklabel이
	 * 없는 것으로 판단한다. 이는 「이 포맷이 아님」을 뜻하며, check_partition()이
	 * 다른 포맷 프로버(msdos.c, sun.c 등)를 계속 시도할 수 있도록 0을 반환해야 한다
	 * (실패로 취급해 스캔 전체를 중단시키면 안 된다).
	 */
	if (le32_to_cpu(label->d_magic) != DISKLABELMAGIC) { /* [한국어] le32_to_cpu()로 리틀 엔디언 필드를 호스트 바이트 순서로 변환 후 DISKLABELMAGIC(0x82564557)과 비교 - 다르면 조건 참, 아래 블록으로 진입 */
		put_dev_sector(sect); /* [한국어] 매직 불일치로 확정되기 전에, 앞서 read_part_sector()로 확보한 섹터 버퍼(folio 참조)를 여기서 반납 - 반환 전에 반드시 필요한 리소스 정리 */
		return 0; /* [한국어] 0 반환 = 「이 포맷 아님」 - check_partition()이 다음 프로버로 계속 진행하게 하는 값(음수인 -1과는 의미가 다름에 유의) */
	}
	/*
	 * 2차 매직 검사: 구조체 뒷부분에 중복 저장된 d_magic2도 같은 값인지 확인한다.
	 * 매직 필드 하나만으로는 우연히 같은 4바이트 패턴을 가진 다른 포맷의 데이터를
	 * OSF/1 disklabel로 오판(false positive)할 가능성을 완전히 배제할 수 없으므로,
	 * 서로 떨어진 두 위치의 매직이 모두 일치해야 유효한 레이블로 인정한다.
	 */
	if (le32_to_cpu(label->d_magic2) != DISKLABELMAGIC) { /* [한국어] d_magic2도 DISKLABELMAGIC과 같은지 재확인 - 다르면(조건 참) 여전히 「이 포맷 아님」으로 처리 */
		put_dev_sector(sect); /* [한국어] 위와 동일한 이유로 반환 전 섹터 버퍼 반납 */
		return 0; /* [한국어] 0 반환 = 「이 포맷 아님」 - 매직 하나만 우연히 일치했던 경우를 여기서
	 * 걸러낸다 */
	}
	/*
	 * 파티션 개수 검사: d_npartitions가 이 코드가 다룰 수 있는 최대치
	 * (MAX_OSF_PARTITIONS=18, d_partitions[] 배열의 컴파일 타임 크기)를 넘으면
	 * 레이블이 손상되었거나 이 파서가 모르는 확장 포맷일 가능성이 있으므로, 안전하게
	 * 「이 포맷 아님」으로 처리해 하위 for 루프가 배열 범위를 벗어나 읽지 않도록
	 * 방어한다.
	 */
	npartitions = le16_to_cpu(label->d_npartitions); /* [한국어] d_npartitions(리틀 엔디언 16비트)를 호스트 바이트 순서로 변환해 지역 변수 npartitions에 저장 - 이후 상한 검사와 루프 상한 양쪽에 쓰인다 */
	if (npartitions > MAX_OSF_PARTITIONS) { /* [한국어] 변환된 개수가 배열 크기(MAX_OSF_PARTITIONS=18)를 초과하는지 검사 - 초과하면(조건 참) 손상/미지원 레이블로 간주 */
		put_dev_sector(sect); /* [한국어] 반환 전 섹터 버퍼 반납 */
		return 0; /* [한국어] 0 반환 = 「이 포맷 아님」 - d_partitions[] 배열을 벗어난 접근을 사전에 차단하는 방어적 조기 반환 */
	}
	/*
	 * 파티션 등록 루프: d_partitions[] 배열의 항목 0..npartitions-1을 순회하며,
	 * 크기가 0이 아닌(=실제 사용 중인) 항목만 put_partition()으로
	 * state->parts[]에 등록한다. state->parts[] 배열 자체의 상한(state->limit)에
	 * 도달하면 더 등록할 공간이 없으므로 즉시 루프를 빠져나온다(방어적 종료 -
	 * 이 파서가 다루는 최대치는 18개뿐이라 실제로는 state->limit인 DISK_MAX_PARTS
	 * =256에 도달할 일이 거의 없지만, 다른 포맷과 공통된 관례를 그대로 따른다).
	 */
	for (i = 0 ; i < npartitions; i++, partition++) { /* [한국어] i를 0부터 npartitions-1까지 증가시키며(후위 증가), partition 포인터도 매 반복 d_partitions[] 배열에서 다음 원소로 함께 전진(partition++) */
		if (slot == state->limit) /* [한국어] 지금까지 등록한 슬롯 번호(slot)가 이 스캔 세션의 최대 파티션 개수 (state->limit)에 도달했는지 검사 - 더 등록하면 state->parts[] 배열을 벗어난다 */
		/* [한국어] 배열 상한 도달 - 더 이상 등록할 수 없으므로 for 루프를 즉시 종료.
		 * 원본 코드의 들여쓰기(탭 2개 + 스페이스 8개)는 커널 스타일 변형이며 이
		 * 주석 작업에서는 그대로 보존한다. */
		        break;
		if (le32_to_cpu(partition->p_size)) /* [한국어] p_size가 0이 아닌지 검사(참 = 0이 아님) - 0이면 「사용되지 않는 슬롯」으로 간주해 put_partition() 호출 없이 건너뛴다 */
			/* [한국어] put_partition() 호출부 시작 - state, 등록할 슬롯 번호(slot), 시작 LBA, 크기(둘 다 리틀 엔디언 -> 호스트 변환)를 전달 */
			put_partition(state, slot,
				le32_to_cpu(partition->p_offset),
				/* [한국어] 위: 파티션 시작 LBA(p_offset)를 호스트 바이트 순서로 변환해 put_partition()의 from 인자로 전달 */
				le32_to_cpu(partition->p_size));
				/* [한국어] 위: 파티션 크기(p_size, 섹터 수)를 호스트 바이트 순서로 변환해 put_partition()의 size 인자로 전달 - 이 호출로 state->parts[slot]에 좌표가 기록되고 로그 문자열에도 이름이 이어붙는다 */
		slot++; /* [한국어] 다음 슬롯 번호로 전진 - p_size==0이라 위 if를 건너뛴 경우에도 무조건 증가한다(빈 항목도 슬롯 번호 하나를 「소비」하는 동작에 유의) */
	}
	/*
	 * 스캔 성공 마무리: 지금까지 put_partition()들이 이어붙인 로그 문자열
	 * (예: 「 nvme0n1: p1 p2」류의 「 osfN: p1 p2」 형태)에 개행을 덧붙여 완성하고,
	 * 더 이상 필요 없는 섹터 버퍼를 반납한 뒤 성공(1)을 반환한다.
	 */
	seq_buf_puts(&state->pp_buf, "\n"); /* [한국어] pp_buf(로그 문자열 버퍼)에 개행 하나를 추가 - check_partition()이 성공 시 이 문자열 전체를 printk(KERN_INFO)로 한 번에 출력하기 위한 마무리 */
	put_dev_sector(sect); /* [한국어] read_part_sector()로 확보했던 LBA 0 섹터 버퍼(folio 참조)를 반납 - 이 시점 이후로는 label/partition 포인터를 더 이상 역참조하면 안 된다 */
	return 1; /* [한국어] 1 반환 = 「OSF/1 disklabel을 인식하고 파티션 등록을 마쳤음」(성공) - check_partition()의 while 루프가 여기서 즉시 종료되고 이 state가 그대로 호출자에게 반환된다 */
}

/*
 * [한국어] 파일 하단 핵심 요약
 * ------------------------
 * - osf_partition()은 CONFIG_OSF_PARTITION이 켜져 있을 때 check_part[] 배열을
 *   통해 check_partition()이 정확히 한 번 호출하는, 이 파일의 유일한 진입점이다.
 * - 매직 검증은 반드시 두 곳(d_magic, d_magic2)이 모두 DISKLABELMAGIC과 일치해야
 *   통과되며, 파티션 개수(d_npartitions)는 컴파일 타임 배열 크기
 *   MAX_OSF_PARTITIONS(18)를 넘을 수 없다.
 * - 등록 대상은 d_partitions[] 중 p_size가 0이 아닌 항목뿐이지만, 슬롯 번호(slot)
 *   자체는 크기가 0인 항목에서도 증가한다는 점이 이 함수의 미묘한 동작이다.
 * - struct disklabel/struct d_partition의 CHS(실린더/헤드/섹터)류 지오메트리
 *   필드(d_nsectors/d_ntracks/d_ncylinders 등)는 이 파서가 전혀 사용하지 않으며,
 *   오직 d_magic/d_magic2/d_npartitions/p_size/p_offset 다섯 필드만 실제로
 *   읽힌다.
 * - block/partitions/core.c의 check_part[] 배열에 등록된 20여 개 포맷 검출기 중
 *   하나로, msdos.c/efi.c/sun.c 등과 동일한 인터페이스 계약
 *   (@return: 1=성공, 0=이 포맷 아님, 음수=I/O 오류)을 따른다.
 */
