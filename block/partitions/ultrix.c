// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/ultrix.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Re-organised Jul 1999 Russell King
 */

/*
 * [한국어 설명] DEC Ultrix 디스크레이블 파티션 검출기 (block/partitions/ultrix.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block/partitions/ 디렉터리에 있는 약 20종의 파티션 포맷
 * 검출기 중 하나인 ultrix_partition() 단 하나의 함수만을 정의한다. DEC
 * (Digital Equipment Corporation)의 Ultrix 운영체제가 VAX/MIPS 기반
 * 워크스테이션(DECstation 등)에서 사용하던 고유의 디스크레이블(disklabel)
 * 형식을 인식하기 위한 코드다. 이 레이블은 디스크의 시작 부분, 정확히는
 * 바이트 오프셋 16384(=16KiB) 지점에서 끝나는 위치에 pt_magic/pt_valid
 * 매직값과 8개의 파티션 슬롯(pt_part[8])을 담고 있으며, 이 함수는 해당
 * 위치의 섹터를 읽어 매직이 일치하면 비어 있지 않은 슬롯들을
 * parsed_partitions 상태에 파티션으로 등록한다. 오늘날에는 사실상 쓰이지
 * 않는 레거시 포맷이지만, 하위 호환을 위해 여전히 커널 파티션 스캔
 * 파이프라인의 프로버 목록에 남아 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 함수가 호출되는 상위 흐름은 block/partitions/core.c에 정의되어 있다:
 * add_disk()/blkdev_get_by_dev() -> bdev_disk_changed() ->
 * blk_add_partitions() -> check_partition() 이며, check_partition()은
 * core.c의 정적 배열 check_part[]에 등록된 포맷 프로버 함수 포인터를
 * 순서대로 하나씩 호출한다. ultrix_partition은 커널 설정
 * CONFIG_ULTRIX_PARTITION이 켜져 있을 때만 이 배열에 포함되며
 * (block/partitions/core.c의 #ifdef CONFIG_ULTRIX_PARTITION 블록 참고),
 * 앞선 프로버들이 모두 "이 포맷이 아님"(0)을 반환한 뒤에야 시도된다.
 * 실행 컨텍스트는 디스크가 처음 등록되거나 사용자가 재스캔을 요청한
 * 호스트 커널의 단일 프로세스 컨텍스트(동기적, blocking 가능)이며,
 * 인터럽트나 별도 워커 스레드와는 무관하다. 이 함수 자체는 다른 함수를
 * 거의 부르지 않는 말단(leaf)에 가까운 코드이며, 아래로는 check.h가
 * 선언한 3개의 헬퍼(read_part_sector, put_partition, put_dev_sector)만
 * 호출한다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 같은 디렉터리의 block/partitions/check.h가 정의하는 공용
 * 계약에 전적으로 의존한다: 스캔 세션 상태를 담는 struct
 * parsed_partitions, 섹터 하나를 페이지 캐시 경유로 읽어오는
 * read_part_sector()/그 반납 짝인 put_dev_sector(), 그리고 발견한
 * 파티션 하나를 상태에 기록하는 put_partition(). 이 세 헬퍼의 실제
 * 정의는 block/partitions/core.c에 있다. 데이터 흐름은: (1)
 * read_part_sector()가 디스크 원시 바이트를 커널 가상 주소로 반환하고,
 * (2) 이 함수가 그 바이트를 struct ultrix_disklabel로 재해석하며, (3)
 * 매직이 맞으면 각 슬롯의 pi_blkoff/pi_nblocks를 put_partition()을 통해
 * state->parts[]에 옮겨 적고, (4) 이후 blk_add_partitions()가 그
 * parts[]를 순회하며 실제 struct block_device(예: sda1, sda2)를
 * 만들어낸다. 이 파일은 msdos.c, efi.c, sun.c, sgi.c, osf.c 등 형제
 * 프로버 파일들과 완전히 동일한 인터페이스 계약(@state, @return: 1/0/
 * 음수)을 공유하며, 서로 코드를 직접 참조하지는 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - ultrix_partition(): 이 파일의 유일한 함수. Ultrix 디스크레이블이
 *   위치할 섹터를 읽고, 매직/valid 플래그를 검사한 뒤, 최대 8개의
 *   파티션을 등록한다.
 * - struct ultrix_disklabel(함수 지역 구조체): 온디스크(on-disk) Ultrix
 *   레이블의 메모리 레이아웃을 그대로 옮겨 놓은 것. pt_magic/pt_valid로
 *   레이블 유효성을 표시하고, pt_part[8](struct pt_info 배열)에 각
 *   파티션의 길이(pi_nblocks)와 시작 오프셋(pi_blkoff)을 담는다.
 */

#include "check.h"
/* [한국어] block/partitions/ 서브디렉터리 전용 내부 헤더를 포함 - struct
 * parsed_partitions/Sector 타입 정의와 read_part_sector()/put_dev_sector()/
 * put_partition() 헬퍼 선언, 그리고 이 파일이 구현하는 ultrix_partition()
 * 자신의 프로토타입 선언이 이 헤더에 있다. 이 include가 없으면 아래
 * 함수 시그니처의 파라미터 타입(struct parsed_partitions)과 본문에서
 * 쓰는 세 헬퍼 함수를 전혀 알 수 없다. */

/*
 * [한국어]
 * ultrix_partition() - DEC Ultrix 디스크레이블을 검출하고 파티션을 등록한다.
 *
 * @state: 현재 진행 중인 파티션 스캔 세션의 상태(block/partitions/check.h의
 *   struct parsed_partitions). core.c의 check_partition()이 이 함수를
 *   호출하기 전에 이미 allocate_partitions()로 할당해 둔 것이며,
 *   state->disk가 검사 대상 gendisk를, state->parts[]가 발견된 파티션을
 *   쌓아 둘 배열을, state->pp_buf가 커널 로그에 남길 문자열 버퍼를 가리킨다.
 * @return: 1이면 Ultrix 레이블을 발견해 파티션 등록(0개일 수도 있음)까지
 *   마쳤다는 뜻이며 check_partition()의 프로버 순회 루프가 즉시 멈춘다.
 *   0이면 이 섹터에 Ultrix 매직이 없다는 뜻이며 check_partition()이 배열의
 *   다음 프로버로 넘어간다. -1이면 섹터 자체를 읽는 데 실패했다는 뜻이며
 *   (디스크 용량 초과 LBA 요청, 페이지 캐시 확보 실패 등) check_partition()이
 *   이를 I/O 오류 후보로 기억해 둔다.
 *
 * 이 함수가 필요한 이유: DEC VAX/MIPS 워크스테이션에서 만들어진 디스크를
 * 리눅스로 마운트하려면 Ultrix 고유의 온디스크 파티션 테이블 형식을 이해해야
 * 하는데, 이 형식은 MBR/GPT와 바이트 레이아웃이 전혀 다르므로 별도의
 * 프로버가 필요하다. 동작 과정은 다음과 같다: (1) Ultrix 레이블이 항상
 * 디스크의 바이트 오프셋 16384에서 끝나도록 배치된다는 사전 지식을 이용해
 * 그 레이블을 포함하는 섹터 번호를 (16384 - sizeof(*label))/512로 계산한다.
 * (2) read_part_sector()로 그 섹터 512바이트를 읽어온다. (3) 그 512바이트
 * 버퍼의 끝에서 sizeof(*label)만큼 앞으로 물러난 위치를 struct
 * ultrix_disklabel로 재해석한다(레이블이 섹터의 맨 끝에 정확히 맞춰
 * 배치되기 때문). (4) pt_magic과 pt_valid가 모두 기대값과 같은지 검사한다.
 * (5) 일치하면 8개의 pt_part[] 슬롯을 순회하며 길이(pi_nblocks)가 0이
 * 아닌 슬롯만 put_partition()으로 등록한다. (6) 섹터 버퍼를
 * put_dev_sector()로 반납하고, 로그 버퍼에 개행을 추가한 뒤 1을 반환한다.
 * 매직이 불일치하면 섹터 버퍼만 반납하고 0을 반환한다.
 * 실행 컨텍스트: add_disk() 또는 재스캔 ioctl을 처리하는 프로세스 컨텍스트에서
 * 동기적으로 실행되며, 이 스캔 세션(state)은 단일 스레드만 다루므로 락이
 * 필요 없다. read_part_sector()가 캐시 미스를 만나면 내부적으로 블로킹 I/O가
 * 발생할 수 있다.
 * 호출하는 쪽(caller): block/partitions/core.c의 check_partition()이
 * check_part[] 배열을 순회하며 함수 포인터로 호출한다(CONFIG_ULTRIX_PARTITION
 * 활성화 시에만 배열에 포함).
 * 호출되는 쪽(callee): read_part_sector(), put_partition(), put_dev_sector()
 * (모두 block/partitions/check.h 선언, core.c 정의).
 * 에러 처리 경로: read_part_sector()가 NULL을 반환하면(디스크 용량을 넘는
 * 섹터 요청이거나 페이지 캐시/I/O 실패) 그 즉시 -1을 반환하며, 이 경우
 * put_dev_sector()를 호출하지 않는다(반납할 대상이 없으므로).
 *
 * 호출 체인:
 *   check_partition() 루프 -> [ultrix_partition] -> read_part_sector() /
 *     put_partition() / put_dev_sector()
 */
int ultrix_partition(struct parsed_partitions *state)
{
	int i;				/* [한국어] pt_part[] 8개 슬롯을 0부터 7까지 순회하는 루프 카운터 - 아래 for 문에서만 사용 */
	Sector sect;			/* [한국어] read_part_sector()가 채워 줄 출력 파라미터. 내부적으로 folio 포인터 하나만 담는 값 타입이며, 이 값을 근거로 나중에 put_dev_sector()가 참조를 반납한다 */
	unsigned char *data;		/* [한국어] read_part_sector()가 반환하는, 읽어들인 섹터 512바이트가 시작하는 커널 가상 주소. 이후 struct ultrix_disklabel으로 재해석(캐스팅)할 원본 바이트 포인터 */
	struct ultrix_disklabel {
	/* [한국어]
	 * struct ultrix_disklabel - Ultrix가 디스크에 기록하는 파티션 레이블의
	 * 온디스크 레이아웃을 그대로 옮겨 놓은 지역(local) 구조체. 이 함수
	 * 안에서만 쓰이므로 다른 파일에서는 보이지 않는다. 아래 read_part_sector()
	 * 로 읽어온 원시 바이트 버퍼(data)를 이 타입 포인터로 캐스팅해 필드
	 * 단위로 해석한다(디스크 바이트 순서를 그대로 구조체 오프셋에 대응시키는
	 * 방식이므로, 커널이 빌드된 아키텍처의 엔디안이 Ultrix가 이 레이블을
	 * 기록할 당시의 엔디안과 같아야 올바르게 해석된다 - VAX/MIPS 리틀엔디안
	 * 전제). 전체 크기는 pt_magic(4B) + pt_valid(4B) + pt_part[8]*(pi_nblocks
	 * 4B + pi_blkoff 4B = 8B) = 72바이트이며, 아래 read_part_sector() 호출의
	 * 섹터 번호 계산과 label 포인터 오프셋 계산 모두 이 크기(sizeof(*label))
	 * 에 의존한다. */
		s32	pt_magic;	/* magic no. indicating part. info exits */
		/* [한국어] 이 섹터에 유효한 Ultrix 파티션 정보가 있는지 식별하는 매직 넘버.
		 * 설정자: Ultrix 쪽 디스크 포맷 도구/드라이버가 디스크에 기록해 둔
		 *   값이며, 리눅스 커널은 이 필드를 쓰지 않고 읽기만 한다.
		 * 읽는 자: 아래 ultrix_partition() 본문의 if 조건에서 매크로
		 *   PT_MAGIC(0x032957)과 비교된다.
		 * 값 범위: 정확히 0x032957이어야 "Ultrix 레이블일 가능성 있음"으로
		 *   간주된다. 그 외의 값은 이 섹터가 Ultrix 포맷이 아니거나(다른
		 *   OS/파티션 형식) 손상되었음을 뜻하며, 이 경우 함수는 0을 반환해
		 *   check_partition()이 다음 프로버로 넘어가게 한다.
		 * 동기화: 스캔 세션 하나에서 단일 스레드로만 읽히는 온디스크 값의
		 *   메모리 사본이므로 별도 락이 필요 없다. */
		s32	pt_valid;	/* set by driver if pt is current */
		/* [한국어] 이 레이블 내용이 "현재 유효한 상태"임을 Ultrix 드라이버가
		 * 표시해 두는 플래그.
		 * 설정자: Ultrix 드라이버가 레이블을 최신 상태로 유지할 때 1로
		 *   기록해 둔 값(리눅스는 읽기 전용으로 취급).
		 * 읽는 자: pt_magic과 함께 아래 if 조건에서 PT_VALID(1)와 비교된다.
		 *   pt_magic만 맞고 pt_valid가 다르면 레이블을 신뢰하지 않는다.
		 * 값 범위: 1(PT_VALID)일 때만 유효로 간주. 그 외 값(예: 레이블을
		 *   만들었지만 아직 커밋되지 않은 상태를 나타내는 0 등)이면 함수는
		 *   이 섹터를 Ultrix 파티션 정보로 인정하지 않고 0을 반환한다.
		 * 동기화: pt_magic과 동일 - 단일 스레드 읽기 전용 접근. */
		struct  pt_info {
		/* [한국어] 파티션 하나를 기술하는 8바이트짜리 내부 레코드 타입.
		 * pt_part[8] 배열의 원소 타입으로만 쓰이며, 이 구조체 밖에서는
		 * 참조되지 않는다. 필드 순서(길이 먼저, 오프셋 다음)는 Ultrix가
		 * 온디스크에 기록한 순서를 그대로 반영한다. */
			s32		pi_nblocks; /* no. of sectors */
			/* [한국어] 이 파티션 슬롯이 차지하는 섹터(512바이트 블록) 개수.
			 * 설정자: Ultrix 레이블 작성 시점에 기록된 온디스크 값(리눅스는
			 *   읽기만 한다).
			 * 읽는 자: 아래 for 루프의 if (label->pt_part[i].pi_nblocks)
			 *   조건에서 "이 슬롯이 사용 중인지"를 판별하는 데 쓰이고,
			 *   0이 아니면 put_partition()의 size 인자로 그대로 전달된다.
			 * 값 범위: 0이면 미사용 슬롯(등록하지 않고 건너뜀). 양수이면
			 *   유효한 파티션 길이. 이 함수는 이 값이 디스크 전체 용량을
			 *   넘는지 자체적으로 검증하지 않는다(put_partition()도 슬롯
			 *   인덱스 범위만 검사할 뿐 크기는 검증하지 않음).
			 * 동기화: 단일 스캔 스레드의 읽기 전용 접근. */
			u32		pi_blkoff;  /* block offset for start */
			/* [한국어] 이 파티션이 디스크 맨 앞을 기준으로 시작하는 절대
			 * 섹터 번호(블록 오프셋).
			 * 설정자: pi_nblocks와 마찬가지로 Ultrix가 레이블 작성 시 기록한
			 *   온디스크 값.
			 * 읽는 자: put_partition()의 from 인자로 그대로 전달되어
			 *   state->parts[i+1].from에 옮겨 적힌다.
			 * 값 범위: 0 이상. pi_blkoff + pi_nblocks가 디스크 용량을
			 *   넘어서는 안 되지만, 이 함수/put_partition() 어느 쪽도 그
			 *   범위를 직접 검증하지는 않는다(레이블 자체의 무결성에
			 *   의존하는 레거시 코드).
			 * 동기화: 단일 스캔 스레드의 읽기 전용 접근. */
		} pt_part[8];
		/* [한국어] Ultrix가 지원하는 파티션 슬롯 8개를 담는 고정 크기 배열.
		 * 설정자: Ultrix 레이블 작성기가 최대 8개까지 파티션 정보를 순서대로
		 *   기록해 둔다(사용하지 않는 슬롯은 pi_nblocks=0으로 남겨 둠).
		 * 읽는 자: 아래 for (i=0; i<8; i++) 루프가 인덱스 0..7을 순서대로
		 *   방문하며 각 슬롯을 검사한다.
		 * 값 범위: 인덱스 0부터 7까지 고정(가변 길이가 아님 - Ultrix
		 *   하드웨어/포맷 자체의 상한이 8개이기 때문). put_partition()
		 *   호출 시 파티션 번호로는 i가 아니라 i+1(1부터 시작)이 쓰인다.
		 * 동기화: 단일 스캔 스레드의 읽기 전용 접근. */
	} *label;
	/* [한국어] read_part_sector()로 읽어온 원시 바이트 버퍼(data)를 위
	 * struct ultrix_disklabel 타입으로 재해석해 가리키는 포인터.
	 * 설정자: 아래에서 label = (struct ultrix_disklabel *)(data + 512 -
	 *   sizeof(*label));로 단 한 번 대입된다 - 별도의 메모리 할당은
	 *   일어나지 않고 기존 data 버퍼 안의 한 위치를 가리킬 뿐이다.
	 * 읽는 자: 이후 label->pt_magic/pt_valid/pt_part[]에 대한 모든 접근.
	 * 값 범위: data가 유효한 동안(즉 put_dev_sector(sect) 호출 전까지)만
	 *   유효한 포인터이며, 그 이후에는 댕글링(dangling) 상태가 된다.
	 * 동기화: 이 함수 호출 하나에 국한된 지역 변수이므로 별도 동기화가
	 *   필요 없다. */

#define PT_MAGIC	0x032957	/* Partition magic number */
	/* [한국어] Ultrix 파티션 레이블임을 나타내는 매직 상수 0x032957(8진수
	 * 032957을 16진수로 표기한 리터럴). label->pt_magic과 비교되는 유일한
	 * 사용처는 아래 if 조건문 하나뿐이며, 이 파일 내부에서만 의미를 갖는
	 * 매크로다. */
#define PT_VALID	1		/* Indicates if struct is valid */
	/* [한국어] label->pt_valid가 "레이블이 현재 유효함"을 나타낼 때 가져야
	 * 하는 값 1. PT_MAGIC과 마찬가지로 아래 if 조건문에서만 비교에 쓰인다. */

	data = read_part_sector(state, (16384 - sizeof(*label))/512, &sect);
	/* [한국어] Ultrix 레이블은 디스크 시작 지점 기준 바이트 오프셋 16384(=16KiB)에서
	 * 끝나도록 고정 배치되어 있다는 Ultrix 온디스크 규약을 이용해, 그 레이블을
	 * 포함하는 512바이트 섹터의 번호를 계산해 읽어온다. (16384 - sizeof(*label))
	 * 는 레이블이 시작하는 바이트 오프셋(sizeof(*label)=72바이트이므로 16312)이고,
	 * 이를 섹터 크기 512로 나눈 정수 몫(31)이 그 레이블이 속한 섹터의 번호다.
	 * read_part_sector()는 이 섹터 번호(n)를 state->disk의 page cache를 통해
	 * 읽어 커널 가상 주소를 돌려주는, block/partitions/check.h가 선언하고
	 * core.c가 정의하는 공용 헬퍼다. */
	if (!data)
	/* [한국어] read_part_sector()가 NULL을 반환했는지 검사 - 요청한 섹터 번호가
	 * 디스크 용량(state->disk의 get_capacity 결과)을 넘어서거나, 페이지 캐시
	 * 확보/기반 I/O가 실패한 경우에 해당한다. */
		return -1;
		/* [한국어] 섹터를 읽지 못했으므로 이 프로버는 더 진행할 수 없다.
		 * -1을 반환해 check_partition()에게 "포맷이 아니라서가 아니라 I/O
		 * 오류로 실패했음"을 알린다 - 이 경우 아직 아무 섹터도 확보하지
		 * 못했으므로 put_dev_sector()를 호출할 필요도, 호출해서도 안 된다. */
	
	label = (struct ultrix_disklabel *)(data + 512 - sizeof(*label));
	/* [한국어] 방금 읽어온 512바이트 섹터 버퍼(data)의 맨 끝에서
	 * sizeof(*label)(72바이트)만큼 앞으로 물러난 위치를 struct
	 * ultrix_disklabel 시작 주소로 재해석한다. 위 read_part_sector() 호출이
	 * 이미 "레이블이 끝나는 지점이 포함된 섹터"를 골라 읽어 왔으므로, 그
	 * 섹터 안에서 레이블은 항상 마지막 72바이트(오프셋 440~511)에 위치한다
	 * - 즉 섹터 시작(offset 0)이 아니라 섹터 끝(offset 512)을 기준으로
	 * 거꾸로 오프셋을 잡는다. */

	if (label->pt_magic == PT_MAGIC && label->pt_valid == PT_VALID) {
	/* [한국어] 매직 넘버와 valid 플래그가 둘 다 기대값과 일치하는지 검사한다.
	 * 두 값을 함께 검사함으로써 매직만 우연히 일치하는 손상된 데이터를
	 * 걸러낸다. 일치하면 이 섹터를 신뢰할 수 있는 Ultrix 레이블로 확정하고
	 * 아래 then 블록에서 파티션을 등록하며, 불일치하면 else 블록에서 섹터만
	 * 반납하고 "이 포맷 아님"(0)을 반환한다. */
		for (i=0; i<8; i++)
		/* [한국어] Ultrix가 지원하는 파티션 슬롯 8개(pt_part[0]..pt_part[7])를
		 * 순서대로 방문한다. 아래 put_partition() 호출 시 파티션 번호로는
		 * i+1(1부터 시작)을 사용하므로, 등록되는 파티션 번호는 1..8이 된다. */
			if (label->pt_part[i].pi_nblocks)
			/* [한국어] 이 슬롯의 길이(섹터 수)가 0이 아닌 경우에만, 즉 실제로
			 * 사용 중인 파티션인 경우에만 등록을 진행한다 - pi_nblocks가
			 * 0인 슬롯은 Ultrix가 애초에 파티션을 할당하지 않은 빈 슬롯이므로
			 * 건너뛴다. */
				put_partition(state, i+1, 
					      label->pt_part[i].pi_blkoff,
					      label->pt_part[i].pi_nblocks);
					      /* [한국어] put_partition(state, i+1, from, size) 호출 -
					       * 파티션 번호 i+1, 시작 섹터 pi_blkoff, 길이 pi_nblocks를
					       * state->parts[i+1]에 기록하고 state->pp_buf 로그 버퍼에
					       * " <name><i+1>" 형태 문자열을 이어붙인다. n(=i+1)이
					       * state->limit 이상이면 put_partition() 내부에서 조용히
					       * 무시된다(방어적 설계). */
		put_dev_sector(sect);
		/* [한국어] 위에서 read_part_sector()가 확보해 둔 섹터(folio) 참조를
		 * 반납한다 - 이 시점 이후로 label/data 포인터는 더 이상 유효하지
		 * 않은 것으로 취급해야 한다(댕글링). 8개 슬롯 처리를 모두 마친 뒤
		 * 한 번만 호출된다. */
		seq_buf_puts(&state->pp_buf, "\n");
		/* [한국어] 이 디스크에 대한 파티션 스캔 로그 한 줄을 개행 문자로
		 * 마무리한다. 지금까지 위 put_partition() 호출들이 이어붙인
		 * " p1 p2 ..." 형태 문자열 뒤에 붙으며, 나중에 check_partition()이
		 * printk(KERN_INFO, ...)로 이 버퍼 전체를 커널 로그(dmesg)에 한 번에
		 * 출력할 때 줄 끝을 맺는 역할을 한다. */
		return 1;
		/* [한국어] Ultrix 레이블을 발견하고 파티션 등록까지 마쳤음을
		 * check_partition()에게 알린다 - 이 반환값을 받으면 check_partition()
		 * 은 나머지 프로버를 더 이상 시도하지 않고 순회를 즉시 종료한다. */
	} else {
		put_dev_sector(sect);
		/* [한국어] 매직/valid가 불일치해 이 섹터를 Ultrix 레이블로 인정하지
		 * 않는 경로에서도, read_part_sector()로 확보해 둔 섹터 참조는
		 * 반드시 반납해야 한다(그렇지 않으면 folio 참조 카운트가 남아
		 * 페이지가 회수되지 않는 누수가 생긴다). */
		return 0;
		/* [한국어] 이 섹터에는 Ultrix 파티션 정보가 없다는 뜻으로 0을
		 * 반환한다 - check_partition()은 이 값을 보고 check_part[] 배열의
		 * 다음 프로버로 넘어가 계속 시도한다. */
	}
}
