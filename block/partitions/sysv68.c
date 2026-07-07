// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/sysv68.c
 *
 *  Copyright (C) 2007 Philippe De Muyter <phdm@macqel.be>
 */
/*
 * [한국어 설명] Motorola System V/68(SysV68) 슬라이스 파티션 테이블 파서 (sysv68.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 1980~90년대 Motorola VME버스 기반 68k 워크스테이션/서버에서
 * 동작하던 System V Release 계열 유닉스인 "System V/68"(줄여서 sysV68)이
 * 하드디스크에 기록하던 독자(proprietary) 파티션 테이블 포맷을 해석해,
 * 리눅스 커널의 공통 파티션 스캔 상태(struct parsed_partitions)에 등록하는
 * 파티션 검출기(prober)다. sysV68은 표준 PC BIOS의 MBR이나 이후의 GPT와
 * 전혀 다른, 훨씬 단순한 온디스크 레이아웃을 사용한다 - 디스크 맨 앞
 * 256바이트 단위로 "볼륨 ID 블록"(시그니처 "MOTOROLA")과 "설정
 * 블록"(슬라이스 테이블의 위치/개수)이 있고, 설정 블록이 가리키는 섹터에
 * (오프셋, 크기) 쌍으로 이루어진 "슬라이스(slice)" 배열이 이어지는
 * 구조다. block/partitions/Kconfig의 CONFIG_SYSV68_PARTITION 항목은 VME
 * 플랫폼(Motorola Delta 시리즈 등)에서 기본값 y로 활성화되도록 되어
 * 있어, 이 파서가 없으면 그런 레거시 VME 장비에 연결된 디스크의 슬라이스를
 * 리눅스가 전혀 인식하지 못하고 파티션 없는 단일 블록 장치로만 다루게
 * 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 새 gendisk가 등록되거나(device_add_disk()) 재스캔 ioctl(BLKRRPART 등)이
 * 발생하면 블록 계층은 bdev_disk_changed() -> blk_add_partitions() ->
 * check_partition()(모두 block/partitions/core.c)의 순서로 여러 파티션
 * 포맷 검출기를 순차 시도한다. check_partition()이 순회하는
 * check_part[] 함수 포인터 배열(core.c) 안에서 이 파일의
 * sysv68_partition()은 CONFIG_KARMA_PARTITION의 karma_partition() 바로
 * 다음, 그리고 배열의 맨 마지막 항목(NULL sentinel 직전)으로 등록되어
 * 있다 - 즉 sysV68 검출은 다른 모든 활성화된 포맷(msdos/efi/sun/amiga/
 * atari/mac/ultrix/ibm/karma 등)이 전부 "이 포맷이 아니다"(0)를 반환한
 * 뒤에야 시도되는 최후의 수단이다. 이 함수가 1을 반환하면
 * check_partition() 루프는 즉시 종료되고, blk_add_partitions()가
 * state->parts[]를 읽어 실제 block_device(파티션 노드)를 생성한다.
 * 실행 컨텍스트는 디스크 프로브/재스캔이라는 드문 콜드 패스(cold path)의
 * 프로세스 컨텍스트이며, 디스크 한 개당 스캔이 진행되는 동안 동기적으로
 * 단 한 번만 호출되는 단일 스레드 경로다(인터럽트 컨텍스트나 재진입을
 * 고려할 필요가 없다).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: block/partitions/check.h가 선언하는 struct
 * parsed_partitions(스캔 세션 상태), Sector(섹터 버퍼 래퍼 타입),
 * read_part_sector()/put_dev_sector()(섹터 읽기/반납), put_partition()
 * (파티션 등록)을 그대로 사용한다. read_part_sector()는 인자 n을
 * SECTOR_SIZE(512바이트) 단위의 절대 섹터 번호로 취급하는데(core.c의
 * read_part_sector() 정의 참고), 이 파일의 struct dkblk0가 정확히
 * 256+256=512바이트로 설계되어 있어 read_part_sector(state, 0, ...)
 * 단 한 번의 호출로 볼륨 ID와 설정 블록을 동시에 읽어올 수 있다. 이
 * 파일에 의존하는 모듈: block/partitions/core.c의 check_part[] 배열이
 * sysv68_partition()을 함수 포인터로 등록해 호출하며,
 * block/partitions/check.h는 그 프로토타입을 선언한다. 데이터 흐름은
 * 디스크 섹터 0(볼륨 ID + 설정 블록) -> read_part_sector()가 채우는
 * data 버퍼 -> struct dkblk0로 캐스팅 -> "MOTOROLA" 시그니처 검증 ->
 * 설정 블록에서 얻은 슬라이스 테이블 위치(ios_slcblk)의 섹터를 다시
 * read_part_sector() -> struct slice 배열로 캐스팅 -> 각 유효 엔트리를
 * put_partition()으로 parsed_partitions->parts[]에 등록 -> 이후 core.c가
 * 실제 block_device로 반영하는 순서로 흐른다. 공유하는 핵심 자료구조는
 * struct parsed_partitions(state)이며, 다른 모든 파티션 검출기와 동일한
 * 계약(state->limit, state->name, state->pp_buf, put_partition() 호출
 * 규약)을 그대로 따른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - sysv68_partition(): 이 파일의 유일한 진입점. 섹터 0에서 "MOTOROLA"
 *   시그니처를 확인하고, 설정 블록이 가리키는 섹터에서 슬라이스 테이블을
 *   읽어 마지막(전체 디스크) 항목을 제외한 나머지를 put_partition()으로
 *   등록한다.
 * - struct volumeid: 섹터 0의 첫 256바이트 - 248바이트 예약 영역과
 *   8바이트 "MOTOROLA" 시그니처(vid_mac)로 구성된 볼륨 식별 블록.
 * - struct dkconfig: 섹터 0의 다음 256바이트 - 슬라이스 테이블의 시작
 *   섹터 번호(ios_slcblk)와 항목 수(ios_slccnt)를 담은 설정 블록.
 * - struct dkblk0: volumeid + dkconfig를 이어붙인 512바이트(=1
 *   SECTOR_SIZE) 전체 구조체로, read_part_sector()가 반환하는 섹터
 *   하나를 그대로 매핑한다.
 * - struct slice: 슬라이스 테이블의 항목 하나(크기 nblocks, 오프셋
 *   blkoff, 각각 4바이트 빅엔디안) - Motorola 68k는 빅엔디안
 *   아키텍처이므로 온디스크 필드들이 모두 __be32/__be16 타입이다.
 */

#include "check.h"		/* [한국어] 파티션 스캔 프레임워크 헤더 -- struct parsed_partitions, Sector, read_part_sector()/put_dev_sector()/put_partition() 선언을 가져옴. 이 파일의 모든 디스크 접근과 파티션 등록은 이 공용 계약을 통해서만 이뤄진다 */

/*
 *	Volume ID structure: on first 256-bytes sector of disk
 */
/*
 * [한국어] 디스크의 첫 256바이트("섹터")에 위치하는 볼륨 식별 블록.
 * sysv68_partition()은 read_part_sector(state, 0, &sect)로 얻은 512바이트
 * (SECTOR_SIZE) 버퍼의 앞쪽 절반을 이 구조체로 해석해, vid_mac 필드가
 * ASCII 문자열 "MOTOROLA"와 일치하는지만 검사한다 - 그것이 이 디스크가
 * sysV68 포맷인지 판별하는 유일한 기준이다. 일치하지 않으면
 * sysv68_partition()은 즉시 0을 반환해, check_part[] 배열의 마지막
 * 항목인 이 검출기 이후로는 더 시도할 포맷이 없다는 뜻이 된다.
 */

struct volumeid {
	u8	vid_unused[248];
	/* [한국어] 볼륨 ID 블록의 앞쪽 248바이트, 오프셋 0~247.
	 * 설정자: sysV68 디스크 초기화 도구/펌웨어가 기록(구체적인 내용은
	 *   공개 문서가 없어 알 수 없음 - 추정).
	 * 읽는 자: 이 드라이버는 참조하지 않는다 - vid_mac이 정확히 블록의
	 *   마지막 8바이트(오프셋 248~255)에 오도록 자리를 채우는 패딩
	 *   역할만 한다.
	 * 값 범위: 임의 바이트, 의미 불명.
	 * 동기화: 스캔 스레드가 read_part_sector()로 얻은 읽기 전용
	 *   스냅샷을 단 한 번만 참조하므로 별도 락이 필요 없다. */

	u8	vid_mac[8];	/* ASCII string "MOTOROLA" */
	/* [한국어] 볼륨 ID 블록의 마지막 8바이트(오프셋 248~255) - sysV68
	 * 포맷임을 나타내는 시그니처 문자열.
	 * 설정자: sysV68 디스크 초기화 시 고정 문자열 "MOTOROLA"(8글자,
	 *   NUL 종료 없이 정확히 8바이트)로 기록된다.
	 * 읽는 자: sysv68_partition()이
	 *   memcmp(b->dk_vid.vid_mac, "MOTOROLA", sizeof(b->dk_vid.vid_mac))
	 *   으로 정확히 8바이트를 비교한다 - 이 파일의 유일한 포맷 판별
	 *   기준이다.
	 * 값 범위: 임의의 8바이트. "MOTOROLA"와 정확히 일치해야만(memcmp
	 *   결과 0) 이 디스크가 sysV68 레이블로 인정된다.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
};

/*
 *	config block: second 256-bytes sector on disk
 */
/*
 * [한국어] 볼륨 ID 블록 바로 뒤, 디스크의 두 번째 256바이트("섹터")에
 * 위치하는 설정 블록. read_part_sector(state, 0, &sect)가 반환한 같은
 * 512바이트 버퍼의 뒤쪽 절반(dkblk0 기준 오프셋 256~511)이 이 구조체로
 * 해석된다. "MOTOROLA" 시그니처가 확인된 뒤에만 sysv68_partition()이
 * 이 블록의 ios_slcblk/ios_slccnt 두 필드를 읽어, 슬라이스 테이블이
 * 어디에 몇 개 있는지를 알아낸다.
 */

struct dkconfig {
	u8	ios_unused0[128];
	/* [한국어] 설정 블록 앞쪽 128바이트(dkblk0 기준 오프셋 256~383).
	 * 설정자/읽는 자: 이 드라이버는 참조하지 않는다 - ios_slcblk가
	 *   정확한 위치(오프셋 384)에 오도록 자리를 채우는 패딩으로
	 *   보인다(추정).
	 * 값 범위: 의미 불명.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */

	__be32	ios_slcblk;	/* Slice table block number */
	/* [한국어] 슬라이스 테이블이 위치한 섹터 번호(빅엔디안 32비트,
	 * dkblk0 기준 오프셋 384~387).
	 * 설정자: sysV68 디스크 초기화 도구가 슬라이스 테이블을 배치한
	 *   섹터 번호를 빅엔디안으로 기록(Motorola 68k는 빅엔디안
	 *   아키텍처이므로 온디스크 정수도 그 순서를 따름).
	 * 읽는 자: sysv68_partition()이 be32_to_cpu()로 변환한 값을 로컬
	 *   변수 i에 저장한 뒤, 그대로 read_part_sector(state, i, &sect)의
	 *   두 번째 인자로 넘겨 슬라이스 테이블 섹터를 읽어온다 - 즉 이
	 *   필드는 read_part_sector()가 기대하는 것과 동일한 절대 섹터
	 *   번호 단위로 쓰인다고 가정한다(추정).
	 * 값 범위: 0 이상, 디스크 용량(get_capacity()) 미만이어야
	 *   read_part_sector()가 성공한다.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */

	__be16	ios_slccnt;	/* Number of entries in slice table */
	/* [한국어] 슬라이스 테이블의 전체 항목 수(빅엔디안 16비트, dkblk0
	 * 기준 오프셋 388~389) - 마지막 한 항목은 항상 "전체 디스크"를
	 * 나타내는 것으로 간주된다(원본 주석 "last slice is the whole
	 * disk" 참고).
	 * 설정자: sysV68 디스크 초기화 도구가 실제 슬라이스 개수 + 1(전체
	 *   디스크 항목)을 기록.
	 * 읽는 자: sysv68_partition()이 be16_to_cpu()로 변환해 로컬 변수
	 *   slices에 저장한 뒤 1을 빼(slices -= 1) 순회할 실제 슬라이스
	 *   개수를 계산하고, 이 값이 for 루프의 상한이 된다.
	 * 값 범위: 1 이상(최소한 전체 디스크 항목 하나는 있어야 함).
	 *   비정상적으로 큰 값이 들어와도 이 파일 안에는 별도 검증이 없고,
	 *   오직 state->limit(파티션 슬롯 상한)에 의해서만 방어된다(추정 -
	 *   손상된 테이블에 대한 명시적 검증은 보이지 않는다).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */

	u8	ios_unused1[122];
	/* [한국어] 설정 블록의 나머지 122바이트(dkblk0 기준 오프셋
	 * 390~511) - 128+4+2+122=256바이트로 설정 블록 전체 크기를
	 * 맞추는 패딩.
	 * 설정자/읽는 자: 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
};

/*
 *	combined volumeid and dkconfig block
 */
/*
 * [한국어] volumeid(256바이트)와 dkconfig(256바이트)를 이어붙인
 * 512바이트(정확히 1 SECTOR_SIZE) 구조체. sysv68_partition()은
 * read_part_sector(state, 0, &sect) 단 한 번의 호출로 얻은 512바이트
 * 버퍼를 이 구조체로 캐스팅해, 볼륨 ID 검증과 설정 블록 파싱을 동시에
 * 처리한다 - 512바이트를 정확히 채우도록 설계되어 있기 때문에 두 번째
 * read_part_sector() 호출 없이 시그니처 확인과 슬라이스 테이블 위치
 * 파악을 한 번의 섹터 읽기로 끝낼 수 있다.
 */

struct dkblk0 {
	struct volumeid dk_vid;
	/* [한국어] 오프셋 0~255 - "MOTOROLA" 시그니처를 담은 볼륨 ID
	 * 블록.
	 * 설정자/읽는 자: 위 struct volumeid 필드 설명 참고.
	 * 값 범위: struct volumeid 전체.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */

	struct dkconfig dk_ios;
	/* [한국어] 오프셋 256~511 - 슬라이스 테이블 위치/개수를 담은 설정
	 * 블록.
	 * 설정자/읽는 자: 위 struct dkconfig 필드 설명 참고.
	 * 값 범위: struct dkconfig 전체.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
};

/*
 *	Slice Table Structure
 */
/*
 * [한국어] 슬라이스 테이블 항목 하나의 온디스크 표현(8바이트, 빅엔디안).
 * ios_slcblk가 가리키는 섹터에는 이 struct slice가 ios_slccnt개
 * 연속으로 나열되어 있으며, sysv68_partition()은 이 배열을 순서대로
 * 순회하면서 마지막 한 항목(전체 디스크)을 제외한 나머지 중 nblocks가
 * 0이 아닌 것만 put_partition()으로 등록한다. Sun VTOC의 "전체 디스크"
 * 슬라이스 관례와 유사하게, sysV68도 항상 마지막 항목에 디스크 전체
 * 범위를 담아두는 것으로 보인다(추정).
 */

struct slice {
	__be32	nblocks;		/* slice size (in blocks) */
	/* [한국어] 이 슬라이스의 크기(섹터 수, 빅엔디안 32비트, 항목 내
	 * 오프셋 0~3).
	 * 설정자: sysV68 디스크 초기화/파티셔닝 도구가 기록.
	 * 읽는 자: sysv68_partition()의 for 루프가 be32_to_cpu()로 변환해
	 *   0인지 검사(0이면 미사용 슬라이스로 보고 건너뜀)하고, 0이
	 *   아니면 put_partition()의 size 인자로 그대로 전달한다.
	 * 값 범위: 0(빈 슬라이스) 또는 1 이상(등록 대상). 디스크 용량을
	 *   넘는 값이 들어와도 이 파일에서는 별도 검증을 하지 않는다
	 *   (추정 - 상위 블록 계층에서 최종적으로 걸러질 수 있음).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */

	__be32	blkoff;			/* block offset of slice */
	/* [한국어] 이 슬라이스가 시작하는 섹터 오프셋(빅엔디안 32비트,
	 * 항목 내 오프셋 4~7).
	 * 설정자: sysV68 디스크 초기화/파티셔닝 도구가 기록.
	 * 읽는 자: sysv68_partition()이 be32_to_cpu()로 변환해
	 *   put_partition()의 from 인자로 전달 - state->parts[slot].from에
	 *   기록되어, 이후 상위 블록 계층이 이 파티션의 block_device를
	 *   생성할 때 시작 오프셋(절대 섹터 번호)으로 사용한다.
	 * 값 범위: 0 이상, 디스크 용량 미만이어야 정상.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
};


/*
 * [한국어]
 * sysv68_partition - Motorola System V/68 디스크의 슬라이스 테이블을
 * 검출하고 파티션을 등록한다.
 *
 * @state: 파티션 스캔 세션 상태(block/partitions/check.h의 struct
 *         parsed_partitions). block/partitions/core.c의
 *         check_partition()이 이 디스크(state->disk)에 대한 스캔을
 *         시작하며 할당해 전달한다. 이 함수는 state->limit(파티션 슬롯
 *         상한), state->name(로그에 쓰일 디스크 이름), state->pp_buf
 *         (사용자에게 보일 요약 로그 버퍼)를 읽고, put_partition()을
 *         통해 state->parts[]에 결과를 기록한다.
 * @return: 1  - 섹터 0에서 "MOTOROLA" 시그니처를 확인해 sysV68 포맷으로
 *              판정한 경우. 슬라이스 테이블에 등록 가능한 파티션이
 *              하나도 없더라도(예: 전부 nblocks == 0) 시그니처만
 *              맞으면 1을 반환한다 - "이 포맷이 맞다"와 "파티션을
 *              찾았다"가 이 함수에서는 같은 의미로 취급된다.
 *          0  - 섹터 0은 정상적으로 읽었지만 vid_mac이 "MOTOROLA"와
 *              달라 sysV68 레이블이 아닌 경우. check_part[] 배열에서
 *              이 함수가 마지막 항목이므로, check_partition()은 이
 *              값을 받으면 더 시도할 검출기 없이 "포맷을 알 수 없음"
 *              으로 마무리한다.
 *          -1 - read_part_sector()가 실패한 경우(디스크 I/O 오류 또는
 *              섹터 버퍼 할당 실패) - 섹터 0을 읽을 때, 또는 슬라이스
 *              테이블 섹터를 읽을 때 모두 이 값을 반환할 수 있다.
 *              check_partition()은 이 값을 저장해 두었다가, 다른 모든
 *              검출기도 실패하면 최종 에러로 승격시켜 보고한다.
 *
 * 섹터 0(볼륨 ID + 설정 블록, 합쳐서 512바이트)을 read_part_sector()로
 * 읽어와 struct dkblk0로 재해석한 뒤, dk_vid.vid_mac이 "MOTOROLA"와
 * 일치하는지 검사한다. 일치하지 않으면 즉시 0을 반환한다. 일치하면
 * dk_ios에서 슬라이스 테이블의 위치(ios_slcblk)와 항목 수(ios_slccnt)를
 * 얻어 첫 번째 섹터 버퍼를 반납하고, ios_slcblk가 가리키는 섹터를 다시
 * read_part_sector()로 읽어 struct slice 배열로 재해석한다. 항목 수에서
 * 1을 뺀 나머지(마지막 "전체 디스크" 항목 제외)를 순회하며, nblocks가
 * 0이 아닌 슬라이스만 put_partition()으로 슬롯 1번부터 순서대로
 * 등록한다. state->limit에 도달하면 테이블에 항목이 더 남아 있어도
 * 즉시 순회를 멈춘다. 마지막으로 로그 버퍼를 개행으로 마무리하고, 두
 * 번째 섹터 버퍼를 반납한 뒤 1을 반환한다.
 * 실행 컨텍스트: block/partitions/core.c의 check_partition()이 디스크
 * 프로브/재스캔 시점에 프로세스 컨텍스트에서 동기적으로 한 번 호출하는
 * 단일 스레드 경로다. state는 스캔이 진행되는 동안 다른 스레드와
 * 공유되지 않으므로 락이나 원자적 연산이 필요 없다.
 * 호출자: block/partitions/core.c의 check_partition()이 check_part[]
 * 배열을 순회하며(이 배열의 마지막 항목으로서) 시도한다.
 * 피호출자: read_part_sector()/put_dev_sector()(섹터 I/O),
 * be16_to_cpu()/be32_to_cpu()(빅엔디안 필드 변환 - Motorola 68k가
 * 빅엔디안이므로 필요), memcmp()(시그니처 비교), put_partition()
 * (파티션 등록), seq_buf_printf()/seq_buf_puts()(로그 문자열 작성).
 * 에러 경로: 두 번의 read_part_sector() 호출 중 어느 쪽이든 NULL을
 * 반환하면 즉시 -1을 반환한다(첫 번째 실패 시에는 아직 아무 버퍼도
 * 잡지 않았으므로 put_dev_sector()가 필요 없고, 두 번째 실패 시에는
 * 이미 첫 번째 섹터 버퍼를 반납한 뒤이므로 역시 추가로 해제할 것이
 * 없다). "MOTOROLA" 시그니처가 불일치하면 put_dev_sector()로 첫
 * 번째 섹터 버퍼를 반납한 뒤 0을 반환한다. 그 외에는 치명적 에러
 * 경로가 없다 - nblocks가 0인 개별 슬라이스는 단순히 건너뛸 뿐 함수
 * 실행을 중단시키지 않는다.
 *
 * 호출 체인:
 *   check_partition() → [sysv68_partition] → read_part_sector()/put_partition()/put_dev_sector()
 */
int sysv68_partition(struct parsed_partitions *state)	/* [한국어] state->disk가 가리키는 디스크에 대해 이 검출기가 호출됨 -- check_part[] 표의 맨 마지막 항목으로 등록되어 core.c가 함수 포인터로 호출 */
{
	int i, slices;			/* [한국어] i: 처음엔 dk_ios.ios_slcblk(슬라이스 테이블 섹터 번호)를 담는 임시 변수, 이후 for 루프의 슬라이스 인덱스로 재사용됨. slices: dk_ios.ios_slccnt에서 마지막 "전체 디스크" 항목을 뺀, 등록할 슬라이스 개수 */
	int slot = 1;			/* [한국어] put_partition()에 넘길 파티션 슬롯 번호 -- 관례상 0번 슬롯은 비워두고 1번부터 시작 */
	Sector sect;			/* [한국어] read_part_sector()가 채워주는 섹터 버퍼 래퍼 -- 사용이 끝나면 반드시 put_dev_sector(sect)로 해제해야 함. 이 함수 안에서 두 번(섹터 0, 슬라이스 테이블 섹터) 재사용됨 */
	unsigned char *data;		/* [한국어] read_part_sector()가 반환한 섹터의 원시 바이트에 대한 커널 가상 주소 포인터 -- 첫 번째는 dkblk0, 두 번째는 슬라이스 테이블로 각각 재해석됨 */
	struct dkblk0 *b;			/* [한국어] 섹터 0의 데이터를 볼륨 ID + 설정 블록 레이아웃으로 재해석하기 위한 포인터 */
	struct slice *slice;		/* [한국어] 슬라이스 테이블 섹터의 데이터를 struct slice 배열로 재해석해 순회하기 위한 포인터 */

	data = read_part_sector(state, 0, &sect);
				/* [한국어] 디스크 절대 섹터 0(볼륨 ID + 설정 블록, 512바이트)을 동기적으로 읽어옴 -- sysV68 레이블은 항상 섹터 0에 위치하므로 두 번째 인자는 고정값 0. 반환값은 데이터의 커널 가상 주소, sect에는 folio 참조가 보관됨 */
	if (!data)		/* [한국어] 섹터 읽기 실패 여부 판정 -- 디스크 I/O 오류 또는 페이지 캐시 folio 획득 실패 시 NULL이 반환됨 */
		return -1;	/* [한국어] 아직 어떤 섹터 버퍼도 잡지 않았으므로 put_dev_sector() 없이 바로 -1 반환 -- check_partition()은 이를 I/O 오류로 기억해 두었다가 모든 검출기가 실패하면 최종 에러로 승격시킴 */

	b = (struct dkblk0 *)data;
				/* [한국어] 섹터 0의 원시 바이트를 struct dkblk0(volumeid + dkconfig, 512바이트) 레이아웃으로 재해석(캐스팅) */
	if (memcmp(b->dk_vid.vid_mac, "MOTOROLA", sizeof(b->dk_vid.vid_mac))) {
				/* [한국어] vid_mac 8바이트가 "MOTOROLA"와 정확히 일치하는지 검사 -- memcmp()가 0이 아니면(불일치) 이 디스크는 sysV68 포맷이 아님 */
		put_dev_sector(sect);
					/* [한국어] 시그니처 불일치로 더 이상 이 섹터가 필요 없으므로 즉시 반납(folio 참조 카운트 감소) */
		return 0;
					/* [한국어] "이 포맷이 아님"을 알리는 0 반환 -- check_part[] 배열의 마지막 항목이므로 check_partition()은 더 시도할 검출기 없이 스캔을 마무리함 */
	}
	slices = be16_to_cpu(b->dk_ios.ios_slccnt);
				/* [한국어] 빅엔디안 ios_slccnt(슬라이스 테이블 총 항목 수)를 CPU 바이트오더로 변환해 slices에 저장 -- 아래에서 1을 빼 실제 순회 개수로 다시 쓰임 */
	i = be32_to_cpu(b->dk_ios.ios_slcblk);
				/* [한국어] 빅엔디안 ios_slcblk(슬라이스 테이블이 위치한 섹터 번호)를 CPU 바이트오더로 변환해 i에 저장 -- 곧바로 아래 read_part_sector()의 섹터 번호 인자로 재사용됨 */
	put_dev_sector(sect);
				/* [한국어] 섹터 0용 버퍼 반납 -- 필요한 필드(slices, i)는 이미 로컬 변수로 복사해 두었으므로 이 버퍼는 더 이상 필요 없음 */

	data = read_part_sector(state, i, &sect);
				/* [한국어] ios_slcblk가 가리키는 섹터(슬라이스 테이블)를 읽어옴 -- i를 read_part_sector()가 기대하는 것과 동일한 절대 섹터 번호로 그대로 사용(추정) */
	if (!data)		/* [한국어] 두 번째 섹터 읽기 실패 여부 판정 */
		return -1;	/* [한국어] 첫 번째 섹터 버퍼는 이미 위에서 반납했으므로 추가 해제 없이 바로 -1 반환 */

	slices -= 1; /* last slice is the whole disk */
				/* [한국어] 마지막 슬라이스 항목은 항상 "전체 디스크"를 나타내므로 순회 대상에서 제외 -- 실제 등록 가능한 슬라이스 개수만 남김(원본 주석 그대로) */
	seq_buf_printf(&state->pp_buf, "sysV68: %s(s%u)", state->name, slices);
				/* [한국어] "sysV68: <디스크이름>(s<slices>)" 형태의 요약 헤더 문자열을 로그 버퍼(pp_buf)에 기록 -- 이후 등록되는 슬라이스마다 put_partition()과 아래 seq_buf_printf()가 문자열을 이어붙임 */
	slice = (struct slice *)data;
				/* [한국어] 슬라이스 테이블 섹터의 원시 바이트를 struct slice(8바이트/항목) 배열로 재해석 */
	for (i = 0; i < slices; i++, slice++) {
				/* [한국어] i를 0부터 slices-1까지 증가시키며 슬라이스 테이블을 순회 -- 매 반복 slice 포인터도 함께 다음 항목으로 전진(slice++) */
		if (slot == state->limit)
					/* [한국어] 파티션 슬롯 배열(state->parts[])의 상한에 도달했는지 검사 */
			break;
					/* [한국어] 더 이상 put_partition()을 호출해도 등록되지 않으므로(슬롯 범위 밖) 조기에 루프 탈출 */
		if (be32_to_cpu(slice->nblocks)) {
					/* [한국어] 빅엔디안 nblocks를 변환한 값이 0이 아닌지 검사 -- 0이면 미사용 슬라이스이므로 아래 등록을 건너뜀 */
			put_partition(state, slot,
				be32_to_cpu(slice->blkoff),
				be32_to_cpu(slice->nblocks));
						/* [한국어] state->parts[slot]에 (시작 섹터=blkoff, 길이=nblocks)를 기록하고, put_partition() 내부에서 pp_buf에 " <name><slot>" 형태의 이름도 덧붙임 -- blkoff/nblocks 모두 빅엔디안이므로 be32_to_cpu()로 변환 후 전달 */
			seq_buf_printf(&state->pp_buf, "(s%u)", i);
						/* [한국어] 방금 등록한 슬라이스의 원래 테이블 인덱스 i를 "(sN)" 형태로 로그 버퍼에 추가 기록 -- put_partition()이 남긴 이름 뒤에 이어붙여짐 */
		}
		slot++;
					/* [한국어] 다음 파티션 슬롯 번호로 이동 -- nblocks가 0이라 이번 항목을 등록하지 않았더라도 슬롯 번호는 그대로 증가시켜(재사용하지 않음), i(테이블 인덱스)와 항상 slot = i + 1 관계를 유지 */
	}
	seq_buf_puts(&state->pp_buf, "\n");
				/* [한국어] 이 디스크에 대한 파티션 요약 로그 문자열 끝에 개행을 추가해 마무리 */
	put_dev_sector(sect);
				/* [한국어] 슬라이스 테이블 섹터 버퍼를 반납(folio 참조 카운트 감소) -- 파티션 정보는 이미 state->parts[]에 복사되었으므로 이제 버퍼는 필요 없음 */
	return 1;
				/* [한국어] sysV68 파티션 검출 성공을 알리는 1 반환 -- check_partition() 루프가 종료되고, blk_add_partitions()가 state->parts[]를 바탕으로 실제 block_device(파티션 노드)를 생성함 */
}
