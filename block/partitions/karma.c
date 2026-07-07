// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/karma.c
 *  Rio Karma partition info.
 *
 *  Copyright (C) 2006 Bob Copeland (me@bobcopeland.com)
 *  based on osf.c
 */

/*
 * [한국어 설명] Rio Karma MP3 플레이어 전용 디스크 레이블 파티션 파서 (karma.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Rio(SonicBlue)사가 2000년대 초 판매한 휴대용 MP3 플레이어
 * "Rio Karma"가 내장 하드디스크의 첫 섹터에 기록하는 독자(proprietary)
 * 디스크 레이블 포맷을 해석해, 리눅스 커널의 공통 파티션 테이블 상태
 * (parsed_partitions)에 등록하는 파티션 검출기(prober)다. Rio Karma는
 * USB 대용량 저장장치(USB Mass Storage)로 호스트 PC에 연결되며, 표준
 * MBR/GPT 대신 자신만의 매우 단순한 레이블 - 고정 270바이트 예약 영역
 * + 최대 2개의 파티션 엔트리(엔트리당 16바이트) + 208바이트 패딩 +
 * 2바이트 매직으로 정확히 512바이트(표준 섹터 하나)를 채우는 구조 - 을
 * 섹터 0에 기록한다. 원본 파일 헤더 주석에 "based on osf.c"라고 적혀
 * 있듯 OSF/Tru64 디스크 레이블 파서(block/partitions/osf.c)를 뼈대로
 * 단순화해 만들어졌지만, 실제 on-disk 필드 구성은 OSF와 다르며 CHS
 * (실린더/헤드/섹터) 기하 정보 없이 오직 (오프셋, 크기) 쌍만으로
 * 파티션을 표현한다는 점이 특징이다. block/partitions/Kconfig의
 * CONFIG_KARMA_PARTITION 도움말에 명시된 대로, 이 파서가 없으면 리눅스는
 * Rio Karma를 마운트할 때 그 독자 파티션 테이블을 전혀 인식하지 못하고
 * 디스크 전체를 파티션 없는 단일 블록 장치로만 다루게 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층은 새 gendisk가 등록되거나(device_add_disk()) 재스캔 ioctl
 * (BLKRRPART 등)이 발생할 때 bdev_disk_changed() -> blk_add_partitions()
 * -> check_partition()(모두 block/partitions/core.c)의 순서로 여러
 * 파티션 포맷 검출기를 순차적으로 시도한다. check_partition()은
 * block/partitions/core.c의 check_part[] 함수 포인터 배열을 앞에서부터
 * 순회하는데, 이 파일의 karma_partition()은 그 표에서 msdos/osf/sun/
 * amiga/atari/mac/ultrix/ibm_partition보다 뒤, sysv68_partition보다
 * 앞에 위치한다(CONFIG_KARMA_PARTITION이 켜져 있을 때만 표에 포함됨).
 * 즉 karma_partition()이 호출되는 시점에는 이미 그보다 앞선 검출기들이
 * 모두 "이 포맷이 아니다"(0)를 반환한 뒤이며, karma_partition() 자신도
 * 실패하면 sysv68_partition()으로 넘어간다. 이 함수가 1을 반환하면
 * check_partition() 루프는 즉시 종료되고, blk_add_partitions()가
 * state->parts[]를 읽어 실제 block_device(해당 USB 장치의 파티션 노드)
 * 를 생성한다. 실행 컨텍스트는 디스크 프로브/재스캔이라는 드문 콜드
 * 패스(cold path)의 프로세스 컨텍스트이며, 디스크 하나당 스캔이
 * 진행되는 동안 동기적으로 단 한 번만 호출되는 단일 스레드 경로다
 * (인터럽트 컨텍스트나 재진입을 고려할 필요가 없다).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: block/partitions/check.h가 선언하는 struct
 * parsed_partitions(스캔 세션 상태), Sector(섹터 버퍼 래퍼 타입),
 * read_part_sector()/put_dev_sector()(섹터 읽기/반납), put_partition()
 * (파티션 등록)을 그대로 사용한다. <linux/compiler.h>는 아래 struct
 * disklabel 선언에 붙는 __packed 속성을 제공해, 컴파일러가 필드 사이에
 * 정렬 패딩을 끼워 넣지 못하게 한다(이 구조체는 온디스크 바이트
 * 오프셋과 1:1로 맞아떨어져야 하므로 필수적이다). 이 파일에 의존하는
 * 모듈: block/partitions/core.c의 check_part[] 배열이 karma_partition()
 * 을 함수 포인터로 등록해 호출하며, block/partitions/check.h는
 * karma_partition()의 프로토타입을 선언한다. 데이터 흐름은 디스크(또는
 * USB 대용량 저장장치) 섹터 0 -> read_part_sector()가 채우는 data 버퍼
 * -> struct disklabel로의 캐스팅 -> d_magic 검증 -> d_partitions[2]
 * 순회 -> 유효한 엔트리마다 put_partition()을 통한
 * parsed_partitions->parts[] 등록 -> 이후 core.c가 실제 block_device로
 * 반영하는 순서로 흐른다. 공유하는 핵심 자료구조는 struct
 * parsed_partitions(state)이며, 다른 모든 파티션 검출기와 동일한 계약
 * (state->limit, state->pp_buf, put_partition() 호출 규약)을 그대로
 * 따른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - karma_partition(): 이 파일의 유일한 진입점. 섹터 0을 읽어 매직
 *   (KARMA_LABEL_MAGIC)을 확인하고, 최대 2개의 d_partition 엔트리를
 *   순회하며 유효한 것만 put_partition()으로 등록한다.
 * - struct disklabel: Rio Karma 온디스크 레이블 전체(270바이트 예약
 *   영역 + d_partitions[2] + 208바이트 패딩 + 2바이트 매직 = 정확히
 *   512바이트)를 표현하는 __packed 구조체.
 * - struct d_partition (disklabel 내부에 중첩 정의): 파티션 하나의
 *   타입(p_fstype)과 (오프셋, 크기) 쌍을 담는 16바이트 온디스크 항목.
 * - KARMA_LABEL_MAGIC(0xAB56): d_magic 필드와 비교되는 시그니처 상수로,
 *   이 값이 일치해야만 디스크가 Rio Karma 레이블로 확정된다.
 */

#include "check.h"		/* [한국어] 파티션 스캔 프레임워크 헤더 -- struct parsed_partitions, Sector, read_part_sector()/put_dev_sector()/put_partition() 선언을 가져옴. 이 파일의 모든 디스크 접근과 파티션 등록은 이 공용 계약을 통해서만 이뤄진다 */
#include <linux/compiler.h>	/* [한국어] __packed 등 컴파일러 속성 매크로 제공 -- 아래 struct disklabel이 온디스크 바이트 레이아웃과 1:1로 대응하도록 컴파일러의 구조체 정렬 패딩 삽입을 막는 데 필요 */

/*
 * [한국어] KARMA_LABEL_MAGIC - Rio Karma 레이블의 시그니처 상수.
 * Rio Karma 펌웨어가 디스크를 초기화할 때 struct disklabel의 마지막
 * 필드인 d_magic(오프셋 510~511, 리틀엔디안 16비트)에 기록해 두는
 * 고정값이다. karma_partition()은 read_part_sector()로 읽어온 섹터
 * 0을 le16_to_cpu(label->d_magic)으로 변환한 뒤 이 매크로와 비교해,
 * 그 디스크가 Rio Karma 레이블을 갖고 있는지 판별한다. 값이 다르면
 * 이 디스크는 Rio Karma 포맷이 아니라고 보고 0을 반환해
 * check_partition()이 다음 검출기(sysv68_partition())로 넘어가게 한다.
 */
#define KARMA_LABEL_MAGIC		0xAB56	/* [한국어] Rio Karma 레이블 매직 넘버(리틀엔디안 0xAB56) -- struct disklabel.d_magic과 비교되는 유일한 포맷 판별 기준 */

/*
 * [한국어]
 * karma_partition - Rio Karma MP3 플레이어의 디스크 레이블을 검출하고 파티션을 등록한다.
 *
 * @state: 파티션 스캔 세션 상태(block/partitions/check.h의 struct
 *         parsed_partitions). block/partitions/core.c의 check_partition()이
 *         이 디스크(state->disk)에 대한 스캔을 시작하며 할당해 전달한다.
 *         이 함수는 state->limit(파티션 슬롯 상한), state->pp_buf(사용자
 *         에게 보일 요약 로그 버퍼)를 읽고, put_partition()을 통해
 *         state->parts[]에 결과를 기록한다.
 * @return: 1  - 디스크가 Rio Karma 레이블(매직 일치)을 갖고 있어 검출에
 *              성공한 경우. 파티션이 하나도 없더라도(둘 다 p_fstype
 *              불일치) 매직이 맞으면 1을 반환한다 - "이 포맷이 맞다"는
 *              것과 "파티션을 찾았다"는 것이 이 함수에서는 같은 의미로
 *              취급된다.
 *          0  - 섹터 0은 정상적으로 읽었지만 d_magic이
 *              KARMA_LABEL_MAGIC과 달라 Rio Karma 레이블이 아닌 경우.
 *              check_partition()은 이 값을 보고 다음 검출기
 *              (sysv68_partition())를 시도한다.
 *          -1 - read_part_sector()가 실패한 경우(디스크 I/O 오류 또는
 *              섹터 버퍼 할당 실패). check_partition()은 이 값을 저장해
 *              두었다가, 다른 모든 검출기도 실패하면 최종 에러로
 *              승격시켜 보고한다.
 *
 * 디스크(또는 USB로 연결된 Rio Karma 장치)의 섹터 0을
 * read_part_sector()로 동기적으로 읽어와 struct disklabel로 재해석한
 * 뒤, d_magic 필드가 KARMA_LABEL_MAGIC과 일치하는지 검사한다. 일치하면
 * d_partitions[2] 배열(최대 2개 엔트리)을 순서대로 순회하며, p_fstype이
 * 0x4d이고 p_size가 0이 아닌 엔트리만 유효한 파티션으로 간주해
 * put_partition()으로 등록한다. 각 엔트리는 배열 인덱스에 대응하는
 * 고정 슬롯 번호(1, 2)를 그대로 사용하며(매치되지 않은 엔트리가 있어도
 * 슬롯 번호를 재사용하지 않고 건너뜀), state->limit에 도달하면 더 이상
 * 진행하지 않고 루프를 빠져나온다.
 * 실행 컨텍스트: block/partitions/core.c의 check_partition()이 디스크
 * 프로브/재스캔 시점에 프로세스 컨텍스트에서 동기적으로 한 번 호출하는
 * 단일 스레드 경로다. state는 스캔이 진행되는 동안 다른 스레드와
 * 공유되지 않으므로 락이나 원자적 연산이 필요 없다.
 * 호출자: block/partitions/core.c의 check_partition()이 check_part[]
 * 배열을 순회하며 다른 포맷 검출기들과 함께 시도한다.
 * 피호출자: read_part_sector()/put_dev_sector()(섹터 I/O), le16_to_cpu()/
 * le32_to_cpu()(리틀엔디안 필드 변환), put_partition()(파티션 등록),
 * seq_buf_puts()(로그 문자열 종결).
 * 에러 경로: read_part_sector()가 NULL을 반환하면 즉시 -1을 반환하고
 * 함수를 종료한다(이 경우 아직 어떤 섹터 버퍼도 잡지 않았으므로
 * put_dev_sector() 호출이 필요 없다). d_magic이 불일치하면
 * put_dev_sector()로 섹터 버퍼를 반납한 뒤 0을 반환한다. 그 외에는
 * 치명적 에러 경로가 없다 - p_fstype이 기대값과 다른 개별 엔트리는
 * 단순히 건너뛸 뿐 함수 실행을 중단시키지 않는다.
 *
 * 호출 체인:
 *   check_partition() → [karma_partition] → read_part_sector()/put_partition()/put_dev_sector()
 */
int karma_partition(struct parsed_partitions *state)	/* [한국어] state->disk가 가리키는 디스크(또는 USB Rio Karma 장치)에 대해 이 검출기가 호출됨 -- check_part[] 표의 항목으로 등록되어 core.c가 함수 포인터로 호출 */
{
	int i;			/* [한국어] d_partitions[2] 배열을 순회하는 인덱스(0, 1) -- 각 값이 곧 파티션 슬롯 번호(slot=i+1)와 대응 */
	int slot = 1;		/* [한국어] put_partition()에 넘길 파티션 슬롯 번호 -- 관례상 0번 슬롯은 비워두고 1번부터 시작 */
	Sector sect;		/* [한국어] read_part_sector()가 채워주는 섹터 버퍼 래퍼 -- 사용이 끝나면 반드시 put_dev_sector(sect)로 해제해야 함 */
	unsigned char *data;	/* [한국어] read_part_sector()가 반환한 섹터 0의 원시 바이트에 대한 커널 가상 주소 포인터 */
	struct disklabel {
		u8 d_reserved[270];
		/* [한국어] 레이블 맨 앞 270바이트의 예약 영역.
		 * 설정자: Rio Karma 펌웨어가 디스크 초기화 시 기록(정확한
		 *   용도는 공개된 문서가 없어 알 수 없음 - 추정).
		 * 읽는 자: 이 드라이버는 참조하지 않는다 - d_partitions[]가
		 *   시작하는 오프셋(270)을 맞추기 위한 패딩 역할만 한다.
		 * 값 범위: 임의 바이트, 의미 불명.
		 * 동기화: 스캔 스레드가 단 한 번 읽는 읽기 전용 스냅샷이며
		 *   별도 락이 필요 없다. */
		struct d_partition {
			__le32 p_res;
			/* [한국어] 예약 필드(리틀엔디안 32비트).
			 * 설정자: Rio Karma 펌웨어(용도 불명 - 추정).
			 * 읽는 자: 이 드라이버는 참조하지 않는다 - 온디스크
			 *   16바이트 엔트리 레이아웃(4+1+3+4+4)을 맞추기
			 *   위한 자리만 차지한다.
			 * 값 범위: 의미 불명.
			 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
			u8 p_fstype;
			/* [한국어] 이 파티션 엔트리의 타입 식별자 1바이트.
			 * 설정자: Rio Karma 펌웨어(또는 그 위에서 동작하는
			 *   파티셔닝 도구)가 파티션을 만들 때 기록.
			 * 읽는 자: karma_partition()이 이 값이 정확히 0x4d
			 *   ('M')인지 검사해, 그 경우에만(그리고 p_size가
			 *   0이 아닌 경우에만) 이 엔트리를 유효한 파티션으로
			 *   취급해 put_partition()으로 등록한다. 0x4d 외의
			 *   값이 어떤 의미를 갖는지는 커널 소스에 문서화되어
			 *   있지 않다(추정 - 예: 미사용/예약 슬롯 표시일
			 *   가능성).
			 * 값 범위: 0~255. 이 드라이버가 실제로 특별하게
			 *   취급하는 값은 0x4d 하나뿐이다.
			 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
			u8 p_res2[3];
			/* [한국어] 예약 필드(3바이트) - p_fstype과 p_offset
			 * 사이의 온디스크 패딩.
			 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지
			 *   않는다.
			 * 값 범위: 의미 불명.
			 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
			__le32 p_offset;
			/* [한국어] 이 파티션이 시작하는 절대 섹터 번호
			 * (리틀엔디안 32비트).
			 * 설정자: Rio Karma 파티셔닝 도구/펌웨어.
			 * 읽는 자: karma_partition()이 le32_to_cpu()로
			 *   변환한 뒤 put_partition()의 from 인자로 그대로
			 *   전달한다 - 이후 이 값은 state->parts[slot].from에
			 *   기록되어, 상위 블록 계층이 이 파티션의
			 *   block_device를 생성할 때 시작 오프셋으로 사용한다.
			 * 값 범위: 0 이상, 디스크 전체 용량 미만이어야 정상.
			 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
			__le32 p_size;
			/* [한국어] 이 파티션의 길이(섹터 수, 리틀엔디안
			 * 32비트).
			 * 설정자: Rio Karma 파티셔닝 도구/펌웨어.
			 * 읽는 자: karma_partition()이 이 값이 0이 아닌지를
			 *   p_fstype 검사와 함께 "유효한 파티션" 판정 조건
			 *   으로 사용하고, le32_to_cpu()로 변환해
			 *   put_partition()의 size 인자로 전달한다.
			 * 값 범위: 0(빈 엔트리 - 등록 안 함) 또는 1 이상
			 *   (등록 대상).
			 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
		} d_partitions[2];
		/* [한국어] 오프셋 270부터 시작하는, 최대 2개의 파티션
		 * 엔트리 배열(엔트리 하나당 4+1+3+4+4=16바이트, 합계
		 * 32바이트 - 오프셋 270~301을 차지).
		 * 설정자: Rio Karma 펌웨어/파티셔닝 도구.
		 * 읽는 자: karma_partition()의 for 루프가 p 포인터로 이
		 *   배열을 순회하며 각 엔트리를 검사·등록한다.
		 * 값 범위: 배열 크기 고정 2 - Rio Karma는 파티션을 최대
		 *   2개까지만 지원한다(확장 파티션 체인이 없다).
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
		u8 d_blank[208];
		/* [한국어] d_partitions[] 끝(오프셋 302)부터 d_magic
		 * 직전(오프셋 509)까지의 미사용/패딩 영역(208바이트).
		 * 설정자: Rio Karma 펌웨어(0으로 채워지는지는 불명 - 추정).
		 * 읽는 자: 이 드라이버는 참조하지 않는다 - d_magic이
		 *   섹터의 마지막 2바이트(오프셋 510~511)에 오도록 자리를
		 *   채우는 패딩 역할만 한다.
		 * 값 범위: 의미 불명.
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
		__le16 d_magic;
		/* [한국어] 레이블 매직 넘버(리틀엔디안 16비트) - 섹터의
		 * 마지막 2바이트(오프셋 510~511)에 위치.
		 * 설정자: Rio Karma 펌웨어가 디스크 초기화 시
		 *   KARMA_LABEL_MAGIC(0xAB56) 값으로 기록.
		 * 읽는 자: karma_partition()이
		 *   le16_to_cpu(label->d_magic)로 변환한 뒤
		 *   KARMA_LABEL_MAGIC과 비교 - 이 파일의 유일한 포맷
		 *   판별 기준이다.
		 * 값 범위: 0~0xFFFF. KARMA_LABEL_MAGIC과 일치해야만 이
		 *   디스크가 Rio Karma 레이블로 인정된다.
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	} __packed *label;
	/* [한국어] read_part_sector()가 읽어온 섹터 0의 원시 바이트(data)를
	 * 위 struct disklabel 레이아웃으로 재해석하기 위한 포인터.
	 * __packed 속성 덕분에 컴파일러가 필드 사이에 정렬 패딩을 넣지
	 * 않으므로, 구조체 필드 오프셋이 온디스크 바이트 오프셋과 정확히
	 * 1:1로 대응한다(합계 270+32+208+2 = 512바이트, 표준 섹터 하나와
	 * 정확히 일치).
	 * 설정자: 아래 label = (struct disklabel *)data; 대입 한 번뿐.
	 * 읽는 자: d_magic 검사, d_partitions 순회 시 계속 참조.
	 * 값 범위: data가 NULL이 아님을 이미 확인한 뒤에만 유효하게
	 *   캐스팅되어 사용된다.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	struct d_partition *p;	/* [한국어] label->d_partitions 배열을 순회하는 포인터 -- 아래 for 루프에서 p++로 다음 엔트리로 전진 */

	data = read_part_sector(state, 0, &sect);
				/* [한국어] 디스크 섹터 0을 동기적으로 읽어온다 -- Rio Karma 레이블은 항상 섹터 0(디스크 맨 앞)에 위치하므로 두 번째 인자는 고정값 0. 반환값은 섹터 데이터의 커널 가상 주소, sect에는 folio 참조가 보관됨 */
	if (!data)		/* [한국어] 섹터 읽기 실패 여부 판정 -- 디스크 I/O 오류 또는 섹터 버퍼(페이지 캐시 folio) 할당 실패 시 NULL이 반환됨 */
		return -1;	/* [한국어] 아직 어떤 섹터 버퍼도 잡지 않았으므로 put_dev_sector() 없이 바로 -1 반환 -- check_partition()은 이를 I/O 오류로 기억해 두었다가 모든 검출기가 실패하면 최종 에러로 승격시킴 */

	label = (struct disklabel *)data;
				/* [한국어] 섹터 0의 원시 바이트를 struct disklabel 레이아웃으로 재해석(캐스팅) -- __packed이므로 바이트 오프셋이 그대로 필드 오프셋이 됨 */
	if (le16_to_cpu(label->d_magic) != KARMA_LABEL_MAGIC) {
				/* [한국어] 리틀엔디안 d_magic을 CPU 바이트오더로 변환해 KARMA_LABEL_MAGIC(0xAB56)과 비교 -- 다르면 이 디스크는 Rio Karma 레이블이 아님 */
		put_dev_sector(sect);	/* [한국어] 매직 불일치로 더 이상 이 섹터가 필요 없으므로 즉시 반납(folio 참조 카운트 감소) */
		return 0;		/* [한국어] "이 포맷이 아님"을 알리는 0 반환 -- check_partition()이 다음 검출기(sysv68_partition())로 넘어감 */
	}

	p = label->d_partitions;
				/* [한국어] 파티션 엔트리 배열의 시작(오프셋 270, d_partitions[0])을 가리키도록 p를 초기화 */
	for (i = 0 ; i < 2; i++, p++) {
				/* [한국어] 최대 2개 엔트리(Rio Karma가 지원하는 파티션 수의 상한)를 순회 -- 매 반복 p++로 다음 엔트리로 전진 */
		if (slot == state->limit)
			break;		/* [한국어] 파티션 슬롯 배열(state->parts[])의 상한에 도달 -- 더 이상 put_partition()을 호출해도 조용히 무시될 뿐이므로 조기에 루프 탈출 */

		if (p->p_fstype == 0x4d && le32_to_cpu(p->p_size)) {
				/* [한국어] p_fstype이 0x4d('M')이고 p_size(리틀엔디안 변환 후)가 0이 아닐 때만 유효한 파티션으로 판정 -- 둘 중 하나라도 아니면 이 엔트리는 건너뜀(빈 슬롯 또는 알 수 없는 타입) */
			put_partition(state, slot, le32_to_cpu(p->p_offset),
				le32_to_cpu(p->p_size));
				/* [한국어] state->parts[slot]에 (시작 섹터, 길이)를 기록하고 pp_buf 로그 문자열에 이름을 덧붙임 -- from=p_offset, size=p_size를 각각 리틀엔디안 변환해서 전달 */
		}
		slot++;		/* [한국어] 다음 슬롯 번호로 이동 -- p_fstype이 맞지 않아 이번 엔트리를 등록하지 않았더라도 슬롯 번호는 그대로 증가시켜(재사용하지 않고) 배열 인덱스와 엔트리 순서의 대응을 유지 */
	}
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 사용자에게 보일 로그 문자열(pp_buf)의 끝에 개행을 추가해 마무리 -- 이 디스크에 대한 파티션 요약 한 줄을 완성 */
	put_dev_sector(sect);			/* [한국어] 섹터 0을 담고 있던 버퍼를 반납(folio 참조 카운트 감소) -- 파티션 정보는 이미 state->parts[]에 복사되었으므로 이제 버퍼는 필요 없음 */
	return 1;
					/* [한국어] Rio Karma 레이블 검출 성공을 알리는 1 반환 -- check_partition() 루프가 종료되고, blk_add_partitions()가 state->parts[]를 바탕으로 실제 block_device를 생성함 */
}

