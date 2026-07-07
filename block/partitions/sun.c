// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/sun.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 */

/*
 * [한국어 설명] Sun SPARC 디스크레이블(Sun disklabel/VTOC) 파티션 파서 (sun.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 SPARC 아키텍처 기반 Sun 워크스테이션/서버(Solaris, SunOS, 구형
 * 오픈펌웨어 부트 환경)가 디스크 맨 앞 섹터에 기록하는 "Sun disklabel"
 * (VTOC, Volume Table Of Contents 포함) 포맷을 인식해, 리눅스 블록 계층이
 * 이해할 수 있는 파티션 목록(struct parsed_partitions)으로 변환하는 형식
 * 검출기(format prober) 하나를 구현한다. Sun disklabel은 정확히 512바이트
 * 한 섹터 안에 사람이 읽는 텍스트 라벨, VTOC 메타데이터, CHS(실린더/헤드/
 * 섹터) 기하 정보, 최대 8개의 파티션 엔트리, 그리고 레이블 전체를 16비트
 * 워드 단위로 XOR한 체크섬이 함께 들어 있다. 이 파일의 유일한 목적은 이
 * 512바이트를 struct sun_disklabel로 해석하고, 매직 넘버와 체크섬으로
 * 무결성을 검증한 뒤, VTOC(있으면)나 고정 8슬롯(없으면) 방식으로 파티션을
 * parsed_partitions에 등록하는 것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스는 새 gendisk가 등록되거나 재스캔 ioctl(BLKRRPART 등)이 발생하면
 * bdev_disk_changed() -> blk_add_partitions() -> check_partition()
 * (모두 block/partitions/core.c)의 순서로 파티션 재스캔을 수행한다.
 * check_partition()은 core.c의 check_part[] 배열에 등록된 형식 검출기를
 * 순서대로 하나씩 호출하며, sun_partition()이 그 중 하나이다(check.h에
 * `int sun_partition(struct parsed_partitions *state);`로 원형이 선언되어
 * 있다). sun_partition()이 1을 반환하면 check_partition()은 그 결과
 * (state->parts[])를 blk_add_partition() -> add_partition() 경로로 넘겨
 * 실제 파티션 block_device를 만든다. 0을 반환하면 "이 포맷이 아님"으로
 * 간주해 다음 검출기(msdos_partition, efi_partition 등)로 넘어가고, -1을
 * 반환하면 섹터 읽기 자체가 실패한 I/O 오류로 기록된다. 실행 컨텍스트는
 * 커널 내부이며, 디스크 하나당 스캔이 진행되는 동안 동기적으로 한 번만
 * 호출되는 단일 스레드 경로다(재진입/동시 호출을 고려할 필요가 없다).
 *
 * === 타 모듈과의 연결 ===
 * - check.h: struct parsed_partitions, Sector, read_part_sector()/
 *   put_dev_sector()(섹터 읽기/해제), put_partition()(파티션 등록) 등 모든
 *   형식 검출기가 공유하는 인프라를 제공한다. sun_partition()은 이 계약을
 *   그대로 따르는 20여 개 검출기 중 하나다.
 * - core.c: 이 파일의 유일한 호출자. sun_partition()이 채운 state를 읽어
 *   최종적으로 파티션 block_device 노드를 만든다.
 * - block/blk.h(간접, check.h 경유): ADDPART_FLAG_RAID/ADDPART_FLAG_
 *   WHOLEDISK 플래그 정의를 제공하며, VTOC의 파티션 id에 따라 이 값들이
 *   state->parts[slot].flags에 세팅된다.
 * 데이터 흐름: 블록 디바이스의 LBA 0(섹터 0) -> read_part_sector()가 페이지
 * 캐시를 통해 채운 버퍼를 struct sun_disklabel *로 캐스팅 -> 매직/체크섬
 * 검증 -> VTOC 유효성 검사 -> partitions[8] 배열(또는 VTOC.infos[]) 순회 ->
 * 유효한 엔트리마다 CHS(start_cylinder) x spc(실린더당 섹터 수)로 절대 LBA
 * 계산 -> put_partition()으로 parsed_partitions->parts[]에 {시작 LBA, 길이}
 * 기록 -> 호출자(core.c)가 이 배열을 읽어 실제 파티션 device를 생성.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct sun_disklabel(함수 내부 지역 타입): 512바이트 온디스크 Sun
 *   disklabel 전체 레이아웃. info(텍스트 라벨), vtoc(VTOC 메타데이터),
 *   CHS 기하 필드들, partitions[8](파티션 엔트리 8개), magic/csum(포맷
 *   식별과 무결성 검증용)으로 구성된다.
 * - struct sun_vtoc(중첩 타입): VTOC 표준 메타데이터. sanity/version/
 *   nparts로 VTOC 자체의 유효성을 판단하고, infos[8]로 각 파티션의
 *   id(파티션 태그)와 flags를 제공한다.
 * - struct sun_info(중첩 타입): vtoc.infos[]의 원소 타입으로, 파티션 하나의
 *   id(예: LINUX_RAID_PARTITION, SUN_WHOLE_DISK)와 flags를 담는다.
 * - struct sun_partition(중첩 타입, partitions[8]의 원소 타입): 파티션
 *   하나의 시작 실린더(start_cylinder)와 길이(num_sectors, 섹터 수)를
 *   담는다. spc(실린더당 섹터 수)를 곱해야 절대 LBA/섹터 길이로 환산된다.
 * - sun_partition(state): 이 파일의 유일한 함수이자 진입점. LBA 0을 읽고,
 *   매직 넘버와 XOR 체크섬으로 레이블 무결성을 검증한 뒤, VTOC 유효성에
 *   따라 최대 8개의 파티션을 parsed_partitions에 등록한다.
 */
#include "check.h"	/* [한국어] struct parsed_partitions, Sector, read_part_sector(), put_dev_sector(), put_partition() 등 파티션 검출기 공용 인프라 선언 - 이 파일이 사용하는 유일한 헤더 */

#define SUN_LABEL_MAGIC          0xDABE	/* [한국어] Sun disklabel 매직 넘버 - 레이블의 마지막 4바이트(오프셋 508~511) 중 앞 2바이트(오프셋 508, __be16)에 위치. sun_partition()이 label->magic과 이 값을 비교해 "이 섹터가 Sun disklabel인지"를 최초 판별하는 기준 */
#define SUN_VTOC_SANITY          0x600DDEEE	/* [한국어] VTOC(Volume Table Of Contents) sanity 매직 - label->vtoc.sanity(오프셋 188, __be32)와 비교해 VTOC 영역이 의미 있는 값으로 채워졌는지 검사할 때 쓰인다. version==1, nparts<=8 조건과 AND로 결합되어 use_vtoc를 결정 */

/*
 * [한국어] 아래 enum은 매크로 대신 컴파일 타임 정수 상수 두 개를 정의한다.
 * SUN_WHOLE_DISK와 LINUX_RAID_PARTITION은 모두 label->vtoc.infos[i].id
 * (VTOC가 각 파티션에 매긴 태그 값)와 비교되는 상수로, 파티션의 id가 이
 * 값과 같으면 state->parts[slot].flags에 ADDPART_FLAG_WHOLEDISK 또는
 * ADDPART_FLAG_RAID를 설정하는 근거가 된다(코드는 아래 sun_partition()
 * 본문 참고). enum으로 선언되어 있으나 매크로(#define)와 동작상 차이는
 * 없으며, 타입 있는 정수 상수 그룹으로 묶어 가독성을 높이기 위한
 * 선택으로 보인다(추정).
 */
enum {
	SUN_WHOLE_DISK = 5,	/* [한국어] Sun VTOC에서 "파티션 전체가 디스크 전체(whole disk)"를 뜻하는 표준 파티션 id 값(Solaris 관례상 슬라이스 2에 해당). vtoc.infos[i].id가 5이면 ADDPART_FLAG_WHOLEDISK를 세팅 */
	LINUX_RAID_PARTITION = 0xfd,	/* autodetect RAID partition */	/* [한국어] 리눅스 md(멀티 디스크) RAID 자동 인식용 관례적 파티션 id(0xfd) - MBR의 0xFD 파티션 타입과 동일한 의미를 Sun VTOC 세계에 그대로 가져온 값. vtoc.infos[i].id가 이 값이면 ADDPART_FLAG_RAID를 세팅해 상위 md 계층이 자동 조립 대상으로 인식하게 함 */
};

/*
 * [한국어]
 * sun_partition() - Sun disklabel(VTOC 포함)을 파싱해 parsed_partitions에
 * 등록하는 이 파일의 진입점.
 *
 * @state: core.c의 allocate_partitions()가 미리 할당한 파티션 스캔 상태.
 *         state->disk(스캔 대상 gendisk), state->parts[](채워 넣을 파티션
 *         배열), state->pp_buf(커널 로그에 남길 요약 문자열 버퍼)를
 *         제공한다. 이 함수는 state->parts[]를 채우고 state->pp_buf에
 *         개행을 덧붙이는 부수효과를 가진다.
 * @return: 1  - Sun disklabel로 판별해 파티션 등록(0개 이상)까지 마쳤음
 *              (성공 - num_sectors가 0인 슬롯은 건너뛰므로 등록된 파티션이
 *              하나도 없을 수도 있다).
 *          0  - Sun disklabel이 아니거나(매직 불일치) 체크섬이 맞지 않아
 *               레이블이 손상되었다고 판단한 경우(오류 아님 - 다음
 *               검출기로 계속 진행).
 *          -1 - LBA 0(섹터 0) 자체를 읽지 못한 경우(I/O 오류).
 *          호출자 core.c의 check_partition()은 0/-1을 받아도 err만 기록해
 *          두고 check_part[] 배열의 다음 형식 검출기로 넘어간다(이 함수의
 *          실패가 전체 파티션 스캔을 중단시키지는 않는다).
 *
 * 동작 개요:
 *   1) LBA 0을 read_part_sector()로 읽어 struct sun_disklabel *로
 *      캐스팅(label). 읽기 자체가 실패하면 -1 반환.
 *   2) label->magic이 SUN_LABEL_MAGIC(0xDABE)와 다르면 Sun disklabel이
 *      아니므로 버퍼를 해제하고 0 반환.
 *   3) 레이블 전체(256개의 16비트 워드)를 끝에서부터 XOR해 체크섬을
 *      계산한다 - 결과가 0이 아니면 레이블이 손상됐다고 보고 0 반환.
 *   4) VTOC의 sanity/version/nparts 값이 유효 범위인지(또는 레거시 호환을
 *      위해 셋 다 0인지) 검사해 use_vtoc를 결정하고, 그 결과에 따라
 *      순회할 파티션 개수(nparts, VTOC 사용 시 vtoc.nparts, 아니면 고정
 *      8개)를 정한다.
 *   5) spc(실린더당 섹터 수 = ntrks * nsect)를 계산해 CHS -> LBA 변환
 *      계수로 사용한다.
 *   6) partitions[0..nparts-1]을 순회하며 각 엔트리의 start_cylinder *
 *      spc를 시작 LBA로, num_sectors를 길이로 삼아 put_partition()으로
 *      등록한다. num_sectors가 0인 빈 슬롯은 건너뛴다. use_vtoc가 참이면
 *      같은 인덱스의 vtoc.infos[i].id를 보고 RAID/whole-disk 플래그를
 *      추가로 세팅한다.
 *   7) 로그를 개행으로 마무리하고 섹터 버퍼를 해제한 뒤 1을 반환한다.
 *
 * 실행 컨텍스트: 커널 내부, 디스크 하나당 파티션 스캔이 진행되는 동안
 * 동기적으로 단 한 번 호출된다. 지역 변수(label, p, sect 등)만 사용하고
 * 전역/공유 상태를 갱신하지 않으므로(state->parts[]는 이 스캔 세션
 * 전용) 별도의 락이나 원자적 연산이 필요 없다.
 * 호출자: block/partitions/core.c의 check_partition() - check_part[]
 * 배열을 순회하며 이 함수를 포함한 20여 개 형식 검출기를 차례로 호출한다.
 * 피호출자: read_part_sector()/put_dev_sector()/put_partition()(check.h),
 * be16_to_cpu()/be32_to_cpu()(빅엔디안->CPU 엔디안 변환), printk(),
 * seq_buf_puts().
 * 에러 처리: 매직 불일치와 체크섬 실패는 모두 "이 포맷이 아니거나 손상됨"
 * 으로 취급해 0을 반환하고(errno 없음), 섹터 읽기 실패만 -1로 구분해 I/O
 * 오류임을 알린다.
 *
 * 호출 체인:
 *   bdev_disk_changed() -> blk_add_partitions() -> check_partition()
 *     -> sun_partition() -> read_part_sector()/put_dev_sector()/
 *        put_partition() (check.h)
 */
int sun_partition(struct parsed_partitions *state)
{
	int i;	/* [한국어] partitions[8](또는 VTOC 유효 시 vtoc.nparts개) 배열을 순회하는 인덱스이자, use_vtoc일 때 vtoc.infos[i]에서 같은 파티션의 id/flags를 조회하는 데도 재사용된다 */
	__be16 csum;	/* [한국어] 레이블 전체를 16비트 워드 단위로 XOR 누적하는 체크섬 변수 - 아래 for 루프에서 256개 워드(레이블 512바이트 전체, csum 필드 자신 포함)를 모두 XOR한 뒤 0이 아니면 레이블이 손상된 것으로 판단한다. 바이트 순서 변환 없이 원시 비트 패턴 그대로 XOR한다(자기 자신과의 XOR 성질은 엔디안과 무관하므로) */
	int slot = 1;	/* [한국어] parsed_partitions->parts[]에 등록할 다음 파티션 슬롯 번호. 1부터 시작하는 이유는 슬롯 0을 whole-disk/예약 용도로 비워두는 리눅스 파티션 프레임워크의 공통 관례(다른 포맷 검출기들도 동일) 때문이다 */
	__be16 *ush;	/* [한국어] 체크섬 계산용 커서 포인터 - 레이블의 마지막 16비트 워드(오프셋 510, csum 필드 자신)에서 시작해 첫 워드(오프셋 0)까지 역순으로 순회한다 */
	Sector sect;	/* [한국어] read_part_sector()가 LBA 0을 읽어 채워주는 섹터 버퍼 핸들(check.h의 Sector 타입) - 함수를 빠져나가는 모든 경로에서 put_dev_sector()로 반드시 해제해야 하는 페이지 캐시 참조를 감싼다 */
	/*
	 * [한국어]
	 * struct sun_disklabel - Sun disklabel의 온디스크(on-disk) 512바이트
	 * 레이아웃 전체를 그대로 표현하는 지역(함수 스코프) 구조체 타입.
	 *
	 * 이 구조체는 read_part_sector()가 반환한 페이지 캐시 버퍼를 그대로
	 * 캐스팅해서 읽기 전용으로 해석하는 "온디스크 포맷 템플릿"이며, 커널이
	 * 새로 생성/기록하는 자료구조가 아니다(Sun/Solaris의 format(1M) 등
	 * 디스크 유틸리티가 디스크에 기록한 값을 그대로 읽어들일 뿐이다).
	 * 함수 내부에 지역 타입으로 선언되어 있어 태그 이름(sun_disklabel,
	 * sun_vtoc, sun_info, sun_partition)의 스코프가 이 함수 본문으로
	 * 한정되며, 파일 전역에서는 보이지 않는다. 전체 크기는 정확히
	 * 512바이트(디스크 한 섹터)이며, 아래 각 필드는 온디스크 바이트
	 * 오프셋과 함께 설명한다.
	 */
	struct sun_disklabel {
		unsigned char info[128];   /* Informative text string */
		/*
		 * [한국어] 온디스크 오프셋 0~127(128바이트) - 사람이 읽을 수 있는
		 * 자유 텍스트 라벨(디스크 모델명, 용도 메모 등 Sun의 format(1M)
		 * 유틸리티가 사용자 입력을 그대로 저장). 커널의 sun_partition()은
		 * 이 필드를 파싱하거나 검증하지 않으며, 단지 레이블 전체 512바이트
		 * 중 앞부분을 차지해 이후 필드들의 오프셋을 고정시키는 역할만
		 * 한다(체크섬 XOR 계산에는 다른 모든 필드와 동일하게 포함됨).
		 */
		/*
		 * [한국어]
		 * struct sun_vtoc - VTOC(Volume Table Of Contents, 볼륨 목차)
		 * 메타데이터. 온디스크 오프셋 128부터 시작하는 136바이트 영역.
		 *
		 * VTOC는 Solaris/SunOS가 disklabel에 추가한 확장 영역으로, 파티션
		 * 개수(nparts)와 각 파티션의 용도 태그(infos[].id)를 명시적으로
		 * 기록해, 순수 CHS 기반 partitions[8] 배열만으로는 알 수 없는
		 * "이 슬롯이 RAID 멤버인지, whole-disk인지" 같은 의미 정보를
		 * 제공한다. sanity/version/nparts 세 필드가 예상 범위를 벗어나면
		 * (그리고 셋 다 0도 아니면) 커널은 이 VTOC 전체를 신뢰하지 않고
		 * (use_vtoc=0) 고정 8슬롯 방식으로만 partitions[]를 순회한다.
		 */
		struct sun_vtoc {
		    __be32 version;     /* Layout version */
		    /*
		     * [한국어] 온디스크 오프셋 128~131(4바이트) - VTOC 레이아웃
		     * 버전. sun_partition()은 be32_to_cpu(label->vtoc.version)
		     * == 1인지만 검사하며(use_vtoc 판정의 조건 중 하나), 버전 1
		     * 이외의 값이 실제로 무엇을 의미하는지는 이 드라이버가 알지
		     * 못한다 - 버전이 다르면 VTOC 전체를 신뢰하지 않고 고정
		     * 8슬롯 방식으로 대체한다.
		     */
		    char   volume[8];   /* Volume name */
		    /*
		     * [한국어] 온디스크 오프셋 132~139(8바이트) - 볼륨(디스크)
		     * 이름 문자열. Solaris의 format(1M)/newfs 유틸리티가 사용자가
		     * 지정한 볼륨 레이블을 기록하는 칸이다. sun_partition()은 이
		     * 필드를 읽거나 검증하지 않으며, VTOC 유효성 판정과도 무관
		     * 하다 - 체크섬 XOR 계산에만 다른 필드와 동일하게 포함된다.
		     */
		    __be16 nparts;      /* Number of partitions */
		    /*
		     * [한국어] 온디스크 오프셋 140~141(2바이트) - VTOC가 밝히는
		     * 실제 파티션 개수. sun_partition()은 be16_to_cpu() 변환 후
		     * 8 이하인지 검사해(use_vtoc 판정의 조건 중 하나) partitions[]/
		     * infos[] 배열 크기(8)를 벗어나지 않는지 방어적으로 확인하고,
		     * use_vtoc가 참으로 확정되면 이 값을 그대로 메인 루프의 반복
		     * 횟수(nparts 지역 변수)로 사용한다.
		     */
		    /*
		     * [한국어]
		     * struct sun_info(중첩 지역 타입) - VTOC가 각 파티션 슬롯에
		     * 대해 갖는 메타데이터 엔트리 하나(온디스크 4바이트: id
		     * 2바이트 + flags 2바이트). 원본 영어 주석의 "sec 2"는 이
		     * 구조체 정의가 유래한 Sun 관련 문서/매뉴얼의 절 번호를
		     * 가리키는 것으로 보인다(추정). 아래 vtoc.infos[8] 배열의
		     * 원소 타입이며, 배열 인덱스가 partitions[8]의 같은 인덱스와
		     * 짝을 이룬다는 것이 이 파일 전체 로직의 핵심 전제다.
		     */
		    struct sun_info {           /* Partition hdrs, sec 2 */
			__be16 id;
			/*
			 * [한국어] 온디스크 상대 오프셋 +0(엔트리당 4바이트 중
			 * 앞 2바이트) - 이 파티션 슬롯의 용도 태그. 커널이 특별히
			 * 인식하는 값은 이 파일의 enum이 정의하는
			 * LINUX_RAID_PARTITION(0xfd)과 SUN_WHOLE_DISK(5) 두 가지뿐
			 * 이며, sun_partition()의 메인 루프에서
			 * be16_to_cpu(label->vtoc.infos[i].id)로 읽어 이 두 값과
			 * 비교한 뒤 각각 ADDPART_FLAG_RAID/ADDPART_FLAG_WHOLEDISK로
			 * 매핑한다. 그 밖의 값(예: Solaris의 다른 파일시스템 태그)은
			 * 이 드라이버가 특별히 취급하지 않는다.
			 */
			__be16 flags;
			/*
			 * [한국어] 온디스크 상대 오프셋 +2(엔트리당 나머지 2바이트)
			 * - Solaris VTOC 자체가 해석하는 파티션 속성 비트(예: 읽기
			 * 전용, 마운트 불가 등). 이 리눅스 드라이버(sun_partition())는
			 * 이 필드를 전혀 읽지 않으며, 오직 위 id 필드만으로 RAID/
			 * whole-disk 판정을 수행한다 - 체크섬 XOR 계산에는 다른
			 * 필드와 동일하게 포함된다.
			 */
		    } infos[8];
		    /*
		     * [한국어] struct sun_info 8개로 이루어진 배열(온디스크
		     * 오프셋 142~173, 32바이트) - VTOC가 지원하는 최대 파티션
		     * 개수(8)에 맞춰 인덱스 0~7이 partitions[0..7]과 1:1로
		     * 대응한다. sun_partition()의 메인 루프는 partitions[i]로
		     * 위치/길이를, infos[i]로 태그(id)를 각각 조회해 같은 i가
		     * 같은 파티션을 가리킨다고 가정한다.
		     */
		    __be16 padding;     /* Alignment padding */
		    /*
		     * [한국어] 온디스크 오프셋 174~175(2바이트) - 구조체 정렬을
		     * 맞추기 위한 예약/패딩 필드. sun_partition()은 이 값을
		     * 읽거나 쓰지 않으며, 의미 있는 데이터를 담지 않는다(체크섬
		     * XOR 계산에만 다른 필드와 동일하게 포함됨).
		     */
		    __be32 bootinfo[3];  /* Info needed by mboot */
		    /*
		     * [한국어] 온디스크 오프셋 176~187(12바이트, __be32 3개) -
		     * SPARC 오픈펌웨어(mboot/OpenBoot)가 이 디스크에서 부팅할 때
		     * 필요한 추가 정보(예: 2차 부트로더 위치)를 담는 것으로
		     * 보이는 레거시 필드(추정 - 3개 워드 각각의 정확한 의미는
		     * 이 소스에 문서화되어 있지 않다). sun_partition()은 이
		     * 필드를 읽지 않으며, 체크섬 계산에만 포함된다.
		     */
		    __be32 sanity;       /* To verify vtoc sanity */
		    /*
		     * [한국어] 온디스크 오프셋 188~191(4바이트) - VTOC 자체의
		     * sanity(정합성) 매직 넘버. sun_partition()이
		     * be32_to_cpu(label->vtoc.sanity) == SUN_VTOC_SANITY
		     * (0x600DDEEE)로 비교하는 use_vtoc 판정의 첫 번째 조건이다.
		     * 이 값이 일치해야(또는 아래 레거시 예외처럼 sanity/version/
		     * nparts가 모두 0이어야) VTOC 나머지 필드(특히 infos[].id)를
		     * 신뢰해 RAID/whole-disk 플래그 판정에 사용한다.
		     */
		    __be32 reserved[10]; /* Free space */
		    /*
		     * [한국어] 온디스크 오프셋 192~231(40바이트, __be32 10개) -
		     * 향후 확장을 위해 비워둔 예약 영역. sun_partition()은 이
		     * 필드를 읽거나 검증하지 않는다(체크섬 XOR 계산에만 다른
		     * 필드와 동일하게 포함됨).
		     */
		    __be32 timestamp[8]; /* Partition timestamp */
		    /*
		     * [한국어] 온디스크 오프셋 232~263(32바이트, __be32 8개) -
		     * 파티션별 마지막 변경/마운트 시각을 기록하는 것으로 보이는
		     * 필드(추정 - Solaris VTOC 관례). sun_partition()은 이 값을
		     * 읽지 않으며, 체크섬 계산에만 포함된다.
		     */
		} vtoc;
		/*
		 * [한국어] struct sun_vtoc 타입의 vtoc 필드 자체 - 온디스크
		 * 오프셋 128~263(136바이트) 전체를 감싸는 중첩 구조체 인스턴스.
		 * 위 use_vtoc 판정과 메인 루프의 RAID/whole-disk 플래그 결정이
		 * 모두 label->vtoc.* 형태로 이 필드를 통해 이루어진다.
		 */
		__be32 write_reinstruct; /* sectors to skip, writes */
		/*
		 * [한국어] 온디스크 오프셋 264~267(4바이트) - 옛 SMD/MFM류
		 * 하드디스크에서 쓰기 명령 사이에 헤드가 안정될 때까지 건너뛰어야
		 * 하는 섹터 수(레거시 회전 지연 보정값). 순수 CHS 시대의 저수준
		 * 컨트롤러 튜닝 파라미터로, sun_partition()은 이 필드를 전혀
		 * 읽지 않는다(체크섬 계산에만 포함).
		 */
		__be32 read_reinstruct;  /* sectors to skip, reads */
		/*
		 * [한국어] 온디스크 오프셋 268~271(4바이트) - 위 write_reinstruct와
		 * 같은 성격으로, 읽기 명령 사이에 건너뛰어야 하는 섹터 수.
		 * sun_partition()은 이 필드를 읽지 않는다(체크섬 계산에만 포함).
		 */
		unsigned char spare[148]; /* Padding */
		/*
		 * [한국어] 온디스크 오프셋 272~419(148바이트) - 512바이트 레이블
		 * 안에서 다른 명명된 필드들이 채우고 남은 여유 공간(패딩/예비
		 * 영역). sun_partition()은 이 영역을 읽거나 쓰지 않으며, 체크섬
		 * XOR 계산에는 다른 바이트들과 동일하게 포함된다(148바이트이므로
		 * 74개의 16비트 워드로 처리됨).
		 */
		__be16 rspeed;     /* Disk rotational speed */
		/*
		 * [한국어] 온디스크 오프셋 420~421(2바이트) - 디스크 회전 속도
		 * (RPM 등, 레거시 회전 매체 파라미터). sun_partition()은 이
		 * 값을 읽지 않는다(체크섬 계산에만 포함).
		 */
		__be16 pcylcount;  /* Physical cylinder count */
		/*
		 * [한국어] 온디스크 오프셋 422~423(2바이트) - 물리 실린더 개수
		 * (아래 ncyl의 "데이터용" 실린더 수와 달리, 예비 실린더까지
		 * 포함한 물리적 전체 실린더 수). sun_partition()은 이 값을
		 * 읽지 않고, spc 계산에도 사용하지 않는다(체크섬 계산에만 포함).
		 */
		__be16 sparecyl;   /* extra sects per cylinder */
		/*
		 * [한국어] 온디스크 오프셋 424~425(2바이트) - 실린더당 예비
		 * 섹터 수(불량 섹터 재배치 등에 대비한 여유분, 레거시 SMD
		 * 디스크 개념). sun_partition()은 이 값을 읽지 않는다(체크섬
		 * 계산에만 포함).
		 */
		__be16 obs1;       /* gap1 */
		/*
		 * [한국어] 온디스크 오프셋 426~427(2바이트) - 원본 영어 주석
		 * "gap1"이 가리키듯, 옛 컨트롤러가 섹터 사이에 두던 간격(gap)
		 * 크기를 나타내던 필드로 지금은 쓰이지 않는(obsolete) 레거시
		 * 값(obs 접두사가 이를 나타냄). sun_partition()은 읽지 않는다
		 * (체크섬 계산에만 포함).
		 */
		__be16 obs2;       /* gap2 */
		/*
		 * [한국어] 온디스크 오프셋 428~429(2바이트) - "gap2", 위 obs1과
		 * 같은 성격의 폐기된(obsolete) 섹터 간격 필드. sun_partition()은
		 * 읽지 않는다(체크섬 계산에만 포함).
		 */
		__be16 ilfact;     /* Interleave factor */
		/*
		 * [한국어] 온디스크 오프셋 430~431(2바이트) - 인터리브 계수
		 * (Interleave factor, 옛 디스크가 회전 지연을 보정하기 위해
		 * 논리 섹터 번호를 물리 섹터에 재배열하던 비율). 현대 디스크는
		 * 인터리빙을 쓰지 않으므로 사실상 레거시 값이며,
		 * sun_partition()은 읽지 않는다(체크섬 계산에만 포함).
		 */
		__be16 ncyl;       /* Data cylinder count */
		/*
		 * [한국어] 온디스크 오프셋 432~433(2바이트) - 데이터 저장에
		 * 실제로 쓰이는 실린더 수(위 pcylcount에서 예비 실린더를 제외한
		 * 값). CHS 기하 정보의 일부이지만, sun_partition()의 spc 계산은
		 * ntrks/nsect만 사용하고 ncyl 자체는 읽지 않는다(체크섬 계산에는
		 * 포함됨).
		 */
		__be16 nacyl;      /* Alt. cylinder count */
		/*
		 * [한국어] 온디스크 오프셋 434~435(2바이트) - 대체(예비) 실린더
		 * 개수(Alternate cylinder count, 불량 실린더 재배치용 여유분).
		 * sun_partition()은 읽지 않는다(체크섬 계산에만 포함).
		 */
		__be16 ntrks;      /* Tracks per cylinder */
		/*
		 * [한국어] 온디스크 오프셋 436~437(2바이트) - 실린더당 트랙 수
		 * (헤드 수와 사실상 동일한 의미의 CHS 기하 파라미터).
		 * sun_partition()이 spc(실린더당 섹터 수) = ntrks * nsect를
		 * 계산할 때 직접 사용하는 두 필드 중 하나 - CHS 좌표를 절대
		 * LBA로 바꾸는 계산에서 실질적으로 유일하게 쓰이는 기하 필드다.
		 */
		__be16 nsect;      /* Sectors per track */
		/*
		 * [한국어] 온디스크 오프셋 438~439(2바이트) - 트랙당 섹터 수.
		 * 위 ntrks와 곱해져 spc(실린더당 섹터 수)를 이루는 나머지 한
		 * 필드 - sun_partition()이 CHS의 start_cylinder를 절대 LBA로
		 * 환산할 때 쓰는 변환 계수의 절반을 담당한다.
		 */
		__be16 obs3;       /* bhead - Label head offset */
		/*
		 * [한국어] 온디스크 오프셋 440~441(2바이트) - 원본 영어 주석의
		 * "bhead - Label head offset"이 가리키듯, 레이블이 기록된
		 * 헤드(면) 오프셋을 나타내던 폐기된(obsolete) 필드.
		 * sun_partition()은 읽지 않는다(체크섬 계산에만 포함).
		 */
		__be16 obs4;       /* ppart - Physical Partition */
		/*
		 * [한국어] 온디스크 오프셋 442~443(2바이트) - 원본 영어 주석의
		 * "ppart - Physical Partition"이 가리키듯, 물리 파티션 번호를
		 * 나타내던 폐기된(obsolete) 필드. sun_partition()은 읽지 않는다
		 * (체크섬 계산에만 포함).
		 */
		/*
		 * [한국어]
		 * struct sun_partition(중첩 지역 타입) - CHS 기반 파티션 엔트리
		 * 하나. 이 태그 이름은 이 파일의 함수 sun_partition()과 글자가
		 * 같지만, C에서 struct 태그와 함수/변수 식별자는 서로 다른
		 * 네임스페이스에 속하므로 이름이 겹쳐도 충돌하지 않는다. 아래
		 * partitions[8] 배열의 원소 타입이며, 함수 하단부에서 지역
		 * 변수 `struct sun_partition *p;`가 이 배열을 순회하는 커서로
		 * 쓰인다.
		 */
		struct sun_partition {
			__be32 start_cylinder;
			/*
			 * [한국어] 온디스크 상대 오프셋 +0(엔트리당 8바이트 중 앞
			 * 4바이트) - 이 파티션이 시작하는 실린더 번호(CHS 좌표,
			 * 빅엔디안). sun_partition()의 메인 루프가
			 * be32_to_cpu(p->start_cylinder) * spc로 절대 LBA(섹터 번호)를
			 * 계산해 put_partition()의 from 인자로 전달한다.
			 */
			__be32 num_sectors;
			/*
			 * [한국어] 온디스크 상대 오프셋 +4(엔트리당 나머지 4바이트)
			 * - 이 파티션의 길이(섹터 수, 빅엔디안). sun_partition()은
			 * be32_to_cpu(p->num_sectors)로 변환한 값이 0이면 "미사용
			 * 슬롯"으로 건너뛰고, 0이 아니면 put_partition()의 size
			 * 인자로 그대로 전달한다.
			 */
		} partitions[8];
		/*
		 * [한국어] struct sun_partition(CHS 엔트리) 8개로 이루어진 배열
		 * (온디스크 오프셋 444~507, 64바이트) - Sun disklabel이 지원하는
		 * 최대 파티션 개수(8)에 대응한다. 함수 진입부의 지역 변수
		 * `p = label->partitions;`가 이 배열의 첫 원소를 가리키도록
		 * 초기화되며, 메인 for 루프가 p++로 이 배열을 순회한다. VTOC가
		 * 유효할 때는 같은 인덱스의 vtoc.infos[i]와 짝을 이뤄 태그(id)
		 * 정보까지 함께 해석된다.
		 */
		__be16 magic;      /* Magic number */
		/*
		 * [한국어] 온디스크 오프셋 508~509(0x1FC, 2바이트) - Sun
		 * disklabel 식별 매직 넘버. sun_partition()이 함수 시작부에서
		 * be16_to_cpu(label->magic) != SUN_LABEL_MAGIC(0xDABE)으로
		 * 가장 먼저 검사하는 필드로, 여기서 불일치하면 이 섹터를 Sun
		 * disklabel로 전혀 인정하지 않고 즉시 0을 반환한다(뒤이은
		 * 체크섬/VTOC 검사까지 가지 않음).
		 */
		__be16 csum;       /* Label xor'd checksum */
		/*
		 * [한국어] 온디스크 오프셋 510~511(0x1FE, 2바이트) - 레이블
		 * 전체 256개 16비트 워드의 XOR 체크섬 값(레이블 작성 도구가
		 * 미리 계산해 저장). 이 필드 자체가 "정답"이 아니라 "XOR 체인의
		 * 한 원소"로 취급되는 점에 유의 - sun_partition()의 체크섬 검증
		 * 루프(위 ush/for 문)는 이 필드를 포함한 256개 워드 전부를 XOR해
		 * 그 결과가 0인지만 확인하며, 이 필드만 따로 꺼내 비교하는
		 * 로직은 없다. 함수 최상단의 지역 변수 csum(__be16)과는 이름만
		 * 비슷할 뿐 서로 다른 객체다.
		 */
	} * label;
	/*
	 * [한국어] label - read_part_sector()가 반환한 LBA 0 버퍼를 위에서
	 * 정의한 struct sun_disklabel *로 캐스팅해 담는 포인터. 함수 전체에서
	 * 온디스크 512바이트 레이블에 접근하는 유일한 통로이며,
	 * put_dev_sector(sect)로 섹터 버퍼를 해제한 뒤에는 더 이상
	 * 역참조하면 안 된다(dangling pointer). NULL이면 read_part_sector()
	 * 실패를 의미하며 -1을 반환한다.
	 */
	/*
	 * [한국어] label->partitions[8] 배열을 순회하는 커서. 초기값은 배열
	 * 첫 원소(아래 p = label->partitions;)이며, 메인 루프에서 p++로
	 * 한 원소씩 전진한다. 타입은 위에서 정의한 지역 중첩 구조체
	 * sun_partition(함수 이름과 같지만 태그 네임스페이스가 달라
	 * 충돌하지 않음)이다.
	 */
	struct sun_partition *p;
	/*
	 * [한국어] Sectors Per Cylinder(실린더당 섹터 수) = ntrks(트랙 수) *
	 * nsect(트랙당 섹터 수). CHS(실린더/헤드/섹터) 좌표인
	 * start_cylinder를 절대 LBA로 환산하는 유일한 변환 계수 - 아래에서
	 * 한 번 계산되어 루프 내내 재사용된다.
	 */
	unsigned long spc;
	/*
	 * [한국어] VTOC(vtoc 필드)를 신뢰하고 사용할지 여부를 나타내는
	 * 불리언 성격의 정수. sanity/version/nparts가 유효 범위이거나
	 * (정상 VTOC) 셋 다 0이면(레거시 호환) 참이 되며, 이후 nparts
	 * 결정과 RAID/whole-disk 플래그 설정 여부를 좌우한다.
	 */
	int use_vtoc;
	/*
	 * [한국어] 실제로 순회할 파티션 엔트리 개수. use_vtoc가 참이면
	 * vtoc.nparts(레이블이 스스로 밝힌 개수, 최대 8), 아니면 고정값
	 * 8(모든 슬롯을 다 검사).
	 */
	int nparts;

	/*
	 * [한국어] LBA 0(디스크의 첫 섹터)을 동기적으로 읽어 struct
	 * sun_disklabel *로 반환받음 - 실패 시 NULL, 성공 시 sect에 folio
	 * 참조가 보관되어 이후 put_dev_sector(sect)로 해제해야 함.
	 */
	label = read_part_sector(state, 0, &sect);
	/*
	 * [한국어] 섹터 읽기 실패(디스크 끝을 넘는 요청이거나 I/O 오류)
	 * 여부 검사.
	 */
	if (!label)
		/*
		 * [한국어] 읽기 실패를 호출자(check_partition())에게 I/O 오류로
		 * 알림 - "이 포맷이 아님"(0)과 구분되는 유일한 음수 반환 경로.
		 */
		return -1;

	/*
	 * [한국어] 파티션 엔트리 순회 커서를 배열 첫 원소(partitions[0])로
	 * 초기화 - 이후 for 루프가 p++로 전진.
	 */
	p = label->partitions;
	/*
	 * [한국어] 온디스크 빅엔디안 __be16 magic 값을 CPU 엔디안으로
	 * 변환해 SUN_LABEL_MAGIC(0xDABE)과 비교 - 다르면 이 섹터는 Sun
	 * disklabel이 아니라고 판단.
	 */
	if (be16_to_cpu(label->magic) != SUN_LABEL_MAGIC) {
		/*
		 * [한국어] 매직 불일치로 더 이상 쓰지 않을 섹터 버퍼(folio 참조)
		 * 해제 - 반환 전 반드시 필요한 정리.
		 */
		put_dev_sector(sect);
		/*
		 * [한국어] "Sun disklabel 아님"을 알리는 반환값(오류 아님) -
		 * check_partition()이 다음 형식 검출기(msdos, efi 등)를 계속
		 * 시도하게 함.
		 */
		return 0;
	}
	/* Look at the checksum */
	/*
	 * [한국어] 아래부터 레이블 512바이트 전체를 16비트 워드 단위로
	 * XOR해 무결성을 검증하는 블록 시작 - 상세 원리는 바로 아래
	 * 주석 참고.
	 */
	/*
	 * [한국어] Sun disklabel의 무결성 검증 알고리즘 - 16비트 워드 단위
	 * XOR 체크섬
	 *
	 * 원리: Sun disklabel은 정확히 512바이트(=256개의 16비트 빅엔디안
	 * 워드)로 구성되며, 마지막 워드(오프셋 510, label->csum 필드
	 * 자신)에는 "나머지 255개 워드를 모두 XOR한 값"이 레이블 작성
	 * 도구(Sun/Solaris format(1M) 등)에 의해 미리 기록되어 있다.
	 * 따라서 label->csum을 포함한 256개 워드 전부를 처음부터 끝까지
	 * XOR하면, XOR의 결합/교환 법칙과 "a XOR a = 0" 성질에 의해
	 * (255개 워드의 XOR) XOR (그 255개 워드의 XOR과 같은 label->csum)
	 * = 0이 되어야 정상이다. 즉 별도의 "계산값과 저장값을 비교"하는
	 * 로직 없이, 256개 워드를 전부 XOR한 최종 결과가 0인지만
	 * 확인하면 손상 여부를 판별할 수 있다 - 이것이 아래 for 루프가
	 * label->csum을 특별 취급하지 않고 다른 워드와 동일하게 XOR
	 * 체인에 포함시키는 이유다.
	 * 엔디안 처리: __be16(빅엔디안 16비트) 값을 CPU 엔디안으로 변환
	 * 하지 않고 원시 비트 패턴 그대로 XOR한다. XOR은 값의 각 비트
	 * 위치에만 의존하는 연산이므로, 워드 내부의 바이트 순서가
	 * 어떻든(빅엔디안이든 리틀엔디안이든) "레이블 작성 시점에 계산한
	 * 방식과 동일한 순서로만 XOR하면" 결과의 정오 여부(0인지 아닌지)
	 * 판정에는 영향이 없다 - 다만 반드시 레이블을 만들 때와 "같은
	 * 워드 경계, 같은 순회 방향"으로 XOR해야 하므로, be16_to_cpu()
	 * 변환 없이 원본 __be16 값 그대로 사용한다.
	 */
	/*
	 * [한국어] label+1은 "struct sun_disklabel 전체(512바이트) 바로
	 * 다음 주소" - 이를 __be16*로 캐스팅한 뒤 -1(2바이트 후진)하면
	 * 레이블의 마지막 16비트 워드, 즉 오프셋 510에 위치한
	 * label->csum 필드 자신의 주소를 가리키게 된다. 포인터 산술은
	 * 캐스팅된 타입(__be16, 2바이트) 단위로 이루어짐에 유의.
	 */
	ush = ((__be16 *) (label+1)) - 1;
	/*
	 * [한국어] csum 누산기를 0으로 초기화하고, ush가 레이블 시작
	 * 주소((__be16*)label, 오프셋 0) 이상인 동안(즉 오프셋 510부터
	 * 0까지 역순으로, 256개 워드 전부를 포함하는 동안) 반복 -
	 * 증감식이 비어 있고 루프 본문(다음 줄)에서 ush--로 직접
	 * 후진시킨다.
	 */
	for (csum = 0; ush >= ((__be16 *) label);)
		/*
		 * [한국어] 현재 ush가 가리키는 16비트 워드를 csum에 XOR 누적한
		 * 뒤(먼저 *ush를 읽어 XOR), 후위 감소(--)로 ush를 2바이트 앞
		 * (낮은 오프셋 방향)으로 이동 - 다음 반복에서 이전 워드를
		 * 가리키게 됨. 256회 반복 후 csum이 0이 아니면 레이블이 손상된
		 * 것이다.
		 */
		csum ^= *ush--;
	/*
	 * [한국어] XOR 누적 결과가 0이 아니면(=256개 워드를 전부 XOR해도
	 * 상쇄되지 않으면) 레이블이 손상됐다고 판단 - 위 알고리즘 설명
	 * 참고.
	 */
	if (csum) {
		/*
		 * [한국어] 체크섬 불일치를 커널 로그에 남김 - 디스크 이름과 함께
		 * "레이블 손상" 경고를 출력해 사용자가 원인을 진단할 수 있게 함.
		 */
		printk("Dev %s Sun disklabel: Csum bad, label corrupted\n",
		/*
		 * [한국어] printk 포맷 문자열의 %s 인자 - 스캔 대상 gendisk의
		 * 이름(예: "sda", "nvme0n1").
		 */
		       state->disk->disk_name);
		/*
		 * [한국어] 체크섬 실패로 더 이상 쓰지 않을 섹터 버퍼 해제 - 반환
		 * 전 정리.
		 */
		put_dev_sector(sect);
		/*
		 * [한국어] 체크섬 손상 시에도 "포맷 인식 실패"와 동일하게 0을
		 * 반환(오류 아님) - 손상된 레이블을 신뢰해 잘못된 파티션을
		 * 등록하는 것보다 이 검출기를 건너뛰는 쪽을 선택.
		 */
		return 0;
	}

	/* Check to see if we can use the VTOC table */
	/*
	 * [한국어] 아래부터 VTOC(Volume Table Of Contents) 메타데이터의
	 * sanity/version/nparts 세 값이 신뢰할 만한 범위인지 검사해
	 * use_vtoc를 결정하는 블록.
	 */
	/*
	 * [한국어] 조건1: vtoc.sanity(오프셋 188, __be32)가
	 * SUN_VTOC_SANITY(0x600DDEEE)와 일치하는지 - VTOC 영역이 의미
	 * 있는 값으로 채워졌다는 첫 번째 근거.
	 */
	use_vtoc = ((be32_to_cpu(label->vtoc.sanity) == SUN_VTOC_SANITY) &&
		/*
		 * [한국어] 조건2: vtoc.version(오프셋 128, __be32)이 1인지 -
		 * 커널이 이해하는 VTOC 레이아웃 버전은 1뿐이므로, 다른 버전이면
		 * 필드 의미가 다를 수 있어 신뢰하지 않음.
		 */
		    (be32_to_cpu(label->vtoc.version) == 1) &&
		/*
		 * [한국어] 조건3: vtoc.nparts(오프셋 140, __be16)가 8 이하인지 -
		 * partitions[8]/infos[8] 배열 크기를 벗어나는 값이면 신뢰하지
		 * 않음(방어적 검사). 세 조건이 모두 참이어야 use_vtoc=1(참).
		 */
		    (be16_to_cpu(label->vtoc.nparts) <= 8));

	/* Use 8 partition entries if not specified in validated VTOC */
	/*
	 * [한국어] use_vtoc가 거짓이면(VTOC를 신뢰할 수 없으면)
	 * vtoc.nparts 대신 고정값 8을 사용해 partitions[8] 전체 슬롯을
	 * 다 검사한다(어차피 num_sectors==0인 빈 슬롯은 아래에서 건너뜀).
	 */
	/*
	 * [한국어] 삼항 연산자: use_vtoc가 참이면 vtoc.nparts(빅엔디안->
	 * CPU 변환)를, 거짓이면 8을 nparts에 대입 - 아래 for 루프의
	 * 반복 횟수를 결정.
	 */
	nparts = (use_vtoc) ? be16_to_cpu(label->vtoc.nparts) : 8;

	/*
	 * So that old Linux-Sun partitions continue to work,
	 * alow the VTOC to be used under the additional condition ...
	 */
	/*
	 * [한국어] 위 영어 주석 보강: 이 하위 호환 처리가 필요한 이유
	 * (추정) - 초창기 Linux의 Sun 파티션 지원 코드는 VTOC를 전혀
	 * 채우지 않고 disklabel의 CHS 필드와 partitions[8]만 사용하는
	 * 방식으로 작성된 디스크가 존재했을 수 있다. 그런 디스크는
	 * sanity/version/nparts가 모두 0(그 구조체 영역이 초기화되지
	 * 않은 상태)일 것이므로, 위 use_vtoc 판정(sanity==매직 &&
	 * version==1 && nparts<=8)만으로는 false가 나와 VTOC를 신뢰하지
	 * 못하게 된다. 이런 레이블도 계속 인식할 수 있도록, "세 필드가
	 * 모두 0"인 경우도 예외적으로 VTOC 사용 가능(use_vtoc=참)으로
	 * 완화한다. 이 경우에도 vtoc.infos[]의 실제 내용은 여전히 0으로
	 * 비어 있으므로, 아래 루프의 RAID/whole-disk 플래그 판정은
	 * 사실상 아무 파티션에도 매치되지 않아 항상 건너뛰게 된다.
	 */
	/*
	 * [한국어] 이미 참이면 그대로 유지(||단락 평가로 아래 식은
	 * 평가 안 함); 거짓이었다면 sanity/version/nparts 세 필드가
	 * "모두 0"인지 추가로 검사 - 셋 중 하나라도 0이 아니면 !(...)가
	 * 거짓이 되어 use_vtoc는 계속 거짓.
	 */
	use_vtoc = use_vtoc || !(label->vtoc.sanity ||
				 /*
				  * [한국어] 위 줄과 이어지는 OR 체인의 나머지 두 항 - sanity/
				  * version/nparts가 셋 다 0이어야(원시 __be16/__be32 값이 0인지
				  * 그대로 비교해도 0은 0이므로 무관) !(...)가 참이 되어
				  * use_vtoc=1로 승격.
				  */
				 label->vtoc.version || label->vtoc.nparts);
	/*
	 * [한국어] spc(Sectors Per Cylinder) = ntrks(오프셋 436, 트랙
	 * 수) * nsect(오프셋 438, 트랙당 섹터 수) - 두 값 다 빅엔디안->
	 * CPU 변환 후 곱함. CHS의 실린더 좌표를 절대 섹터(LBA) 단위로
	 * 바꾸는 유일한 계수이며, ntrks/nsect 중 하나라도 0이면 spc=0이
	 * 되어 아래 모든 파티션의 시작 LBA가 0으로 계산됨(레거시 CHS
	 * 필드가 채워지지 않은 손상/비표준 레이블일 가능성).
	 */
	spc = be16_to_cpu(label->ntrks) * be16_to_cpu(label->nsect);
	/*
	 * [한국어] i=0부터 nparts-1까지 partitions[] 엔트리를 순회 - 매
	 * 반복마다 i를 증가시키는 동시에 파티션 엔트리 커서 p도 함께 한
	 * 칸씩 전진(p++)시켜, p가 항상 label->partitions[i]를 가리키도록
	 * 유지한다.
	 */
	for (i = 0; i < nparts; i++, p++) {
		/*
		 * [한국어] 이번 파티션 엔트리의 절대 시작 LBA(섹터 번호) - CHS의
		 * start_cylinder에 spc를 곱해 계산되며, put_partition()의 from
		 * 인자로 전달됨.
		 */
		unsigned long st_sector;
		/*
		 * [한국어] 이번 파티션 엔트리의 길이(섹터 수) - p->num_sectors를
		 * CPU 엔디안으로 변환한 값이며, 0이면 "비어 있는 슬롯"으로
		 * 취급되어 등록을 건너뜀.
		 */
		unsigned int num_sectors;

		/*
		 * [한국어] p->start_cylinder(시작 실린더 번호, 빅엔디안->CPU
		 * 변환)에 spc(실린더당 섹터 수)를 곱해 절대 LBA로 환산 - CHS
		 * 좌표계를 리눅스 블록 계층이 쓰는 선형 LBA로 바꾸는 핵심
		 * 계산이다.
		 */
		st_sector = be32_to_cpu(p->start_cylinder) * spc;
		/*
		 * [한국어] p->num_sectors(파티션 길이, 섹터 수)를 빅엔디안->CPU
		 * 엔디안으로 변환 - 아래 if에서 0인지 검사해 빈 슬롯 여부를
		 * 판정한다.
		 */
		num_sectors = be32_to_cpu(p->num_sectors);
		/*
		 * [한국어] num_sectors가 0이 아닐 때만(=실제로 쓰이는 슬롯일
		 * 때만) 파티션을 등록 - Sun disklabel은 8개 슬롯을 항상 고정
		 * 배열로 갖지만 실제 사용하는 개수는 그보다 적을 수 있어, 길이
		 * 0인 엔트리는 "미사용"으로 건너뛴다.
		 */
		if (num_sectors) {
			/*
			 * [한국어] parsed_partitions->parts[slot]에 {시작 LBA=st_sector,
			 * 길이=num_sectors}를 기록하고 pp_buf 로그에 이름을 덧붙인다
			 * (check.h) - slot이 state->limit 이상이면 put_partition() 내부
			 * 에서 조용히 무시된다.
			 */
			put_partition(state, slot, st_sector, num_sectors);
			/*
			 * [한국어] 이번 슬롯의 flags를 0(ADDPART_FLAG_NONE)으로 초기화 -
			 * put_partition()은 flags를 건드리지 않으므로, RAID/whole-disk
			 * 플래그를 세팅하기 전 깨끗한 상태를 보장하기 위해 명시적으로
			 * 리셋한다.
			 */
			state->parts[slot].flags = 0;
			/*
			 * [한국어] VTOC를 신뢰할 수 있을 때만(use_vtoc 참) vtoc.infos[i]
			 * 의 파티션 id를 근거로 추가 플래그를 세팅 - VTOC가 없으면 id
			 * 정보 자체가 없으므로 플래그 판정을 시도하지 않는다.
			 */
			if (use_vtoc) {
				/*
				 * [한국어] vtoc.infos[i].id(이번 파티션의 태그, 빅엔디안->CPU
				 * 변환)가 LINUX_RAID_PARTITION(0xfd)과 같은지 검사 - i는 바깥
				 * for 루프의 인덱스와 동일해 partitions[i]와 infos[i]가 같은
				 * 파티션을 가리킨다는 전제 위에서 성립한다.
				 */
				if (be16_to_cpu(label->vtoc.infos[i].id) == LINUX_RAID_PARTITION)
					/*
					 * [한국어] RAID 자동 인식 힌트 플래그를 OR로 추가 - 상위 md
					 * 계층이 이 파티션을 RAID 멤버 후보로 스캔하게 하는 표식
					 * (block/blk.h 정의).
					 */
					state->parts[slot].flags |= ADDPART_FLAG_RAID;
				/*
				 * [한국어] RAID가 아니면, id가 SUN_WHOLE_DISK(5)인지 추가로
				 * 검사 - Solaris 관례상 이 태그는 "파티션이 아니라 디스크
				 * 전체"를 나타내는 특수 슬라이스다.
				 */
				else if (be16_to_cpu(label->vtoc.infos[i].id) == SUN_WHOLE_DISK)
					/*
					 * [한국어] whole-disk 플래그를 OR로 추가 - 이 슬롯이 실제로는
					 * 개별 파티션이 아니라 디스크 전체 영역임을 상위 계층에 알린다
					 * (block/blk.h 정의).
					 */
					state->parts[slot].flags |= ADDPART_FLAG_WHOLEDISK;
			}
		}
		/*
		 * [한국어] 다음 파티션이 등록될 슬롯 번호를 미리 하나 증가 -
		 * num_sectors==0으로 건너뛴 엔트리도 slot 번호는 소비된다(즉
		 * 빈 슬롯도 slot 카운터에 포함되어, partitions[] 인덱스와
		 * slot이 대체로 나란히 증가하는 관계를 유지한다).
		 */
		slot++;
	}
	/*
	 * [한국어] 이 디스크에 대한 로그 라인(예: " sda: sda1 sda2")을
	 * 개행으로 마무리 - check_partition()이 나중에 printk로 한
	 * 번에 출력한다.
	 */
	seq_buf_puts(&state->pp_buf, "\n");
	/*
	 * [한국어] 함수 진입 시 읽어 두었던 LBA 0 섹터 버퍼를 최종 해제
	 * - 이 시점부터 label/p 포인터는 더 이상 유효하지 않다(folio
	 * 반납됨).
	 */
	put_dev_sector(sect);
	/*
	 * [한국어] Sun disklabel로 성공적으로 인식/등록했음을 알리는
	 * 반환값 - check_partition()의 형식 검출기 순회 루프를
	 * 종료시키는 유일한 양수 반환 경로.
	 */
	return 1;
}
