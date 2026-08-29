// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 HUAWEI
 * Author: Cai Zhiyong <caizhiyong@huawei.com>
 *
 * Read block device partition table from the command line.
 * Typically used for fixed block (eMMC) embedded devices.
 * It has no MBR, so saves storage space. Bootloader can be easily accessed
 * by absolute address of data on the block device.
 * Users can easily change the partition.
 *
 * The format for the command line is just like mtdparts.
 *
 * For further information, see "Documentation/block/cmdline-partition.rst"
 *
 */

/*
 * [한국어 설명] 커널 커맨드라인 기반 고정 파티션 테이블 파서 (cmdline.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 커널 부트 커맨드라인에 전달된 "blkdevparts=" 파라미터 문자열을
 * 파싱하여, 온디스크 파티션 테이블(MBR/GPT 등)이 전혀 없는 블록 장치에 대해
 * 고정된 파티션 구성을 소프트웨어적으로 만들어내는 파티션 프로버(prober)이다.
 * eMMC, raw NAND/NOR flash 등 임베디드 장치는 부트로더가 절대 주소로 커널/
 * 루트파일시스템에 접근해야 하므로 MBR을 둘 저장공간 여유가 없거나 두지 않는
 * 경우가 많은데, 이런 장치에서도 사용자 공간에는 "/dev/mmcblk0p1"처럼 여러
 * 개의 독립된 파티션 block_device로 보이도록 해 준다. 문자열 하나에서 여러
 * 블록 장치(";" 구분)와 각 장치 안의 여러 파티션("," 구분)을 파싱해 (이름,
 * 시작 오프셋, 크기, 플래그) 튜플들의 연결 리스트로 저장한다. 이 파일이
 * 만들어내는 파티션 정보는 디스크의 실제 섹터를 단 한 번도 읽지 않고, 오직
 * 부트로더/커널 커맨드라인이 넘겨준 문자열만을 신뢰(trust)해서 구성된다는
 * 점이 MBR/GPT 파서들과 근본적으로 다르다. 마지막으로 파싱된 파티션들 사이에
 * 겹치는 구간이 있는지도 검증해 잘못된 커맨드라인 설정을 커널 로그로 경고한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층이 새 디스크(gendisk)를 등록하면 block/partitions/core.c의
 * check_partition()이 check_part[] 배열에 등록된 여러 파티션 포맷 프로버를
 * 순서대로 시도한다 (호출 체인: blk_add_partitions -> check_partition ->
 * check_part[i](state)). CONFIG_CMDLINE_PARTITION이 켜져 있으면 이 배열에
 * cmdline_partition()이 포함되며, ADFS 계열 프로버들 다음, of_partition/
 * efi_partition/msdos_partition보다는 먼저 시도된다(우선순위가 더 높다).
 * 즉 이 파일의 유일한 외부 진입점인 cmdline_partition()은 디스크마다 한 번씩,
 * 디스크 프로브/파티션 스캔 경로의 프로세스 컨텍스트에서 호출된다. 커맨드라인
 * 원본 문자열 자체는 이보다 훨씬 이른 시점, 즉 커널 초기화 중 __setup()
 * 메커니즘을 통해 cmdline_parts_setup()이 호출될 때 한 번 저장되며, 실제
 * 파싱(cmdline_parts_parse)은 최초의 cmdline_partition() 호출 시점까지
 * 지연(lazy)된다. 이 파일 내부의 나머지 모든 함수(parse_subpart, parse_parts,
 * cmdline_parts_set 등)는 static 헬퍼로서 오직 cmdline_partition()을 정점으로
 * 하는 호출 트리 안에서만 쓰인다. 실행 컨텍스트는 시스템 부팅 시 블록 디바이스
 * 프로브 경로 또는 이후 파티션 재스캔(BLKRRPART ioctl 등) 경로이며, 인터럽트
 * 컨텍스트나 DMA 완료 경로와는 무관하다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 모듈: block/partitions/check.h는 struct parsed_partitions
 * (파티션 스캔 상태), put_partition()(슬롯에 파티션 등록) 등 파티션 프로버
 * 공용 인터페이스를 제공한다(단, 이 파일은 실제 섹터를 읽는 read_part_sector는
 * 쓰지 않는다). check.h가 내부적으로 포함하는 block/blk.h는 ADDPART_FLAG_READONLY
 * 등 파티션 등록 플래그를 정의한다. include/linux/blkdev.h는 struct gendisk,
 * BDEVNAME_SIZE, get_capacity() 등 디스크 용량/이름 관련 자료구조와 헬퍼를
 * 제공한다. 이 파일에 의존하는(호출하는) 쪽은 block/partitions/core.c 하나이며,
 * check_part[] 테이블을 통해 함수 포인터로 간접 호출한다. 데이터 흐름은 다음과
 * 같다: (1) 부트로더가 커널 커맨드라인에 "blkdevparts=..." 문자열을 심음 ->
 * (2) 커널 초기화 중 __setup 매크로가 cmdline_parts_setup()을 호출해 포인터만
 * 저장 -> (3) 특정 디스크의 파티션 스캔 시점에 cmdline_parts_parse()가 이
 * 문자열 전체를 파싱해 cmdline_parts/cmdline_subpart 연결 리스트(전역 변수
 * bdev_parts)를 만듦 -> (4) cmdline_parts_find()로 현재 스캔 중인 디스크
 * 이름과 일치하는 항목을 찾음 -> (5) cmdline_parts_set()이 디스크 실제
 * 용량에 맞춰 잘라내며 add_part()/put_partition()으로 parsed_partitions.parts[]
 * 배열에 기록 -> (6) core.c가 이 배열을 읽어 실제 파티션 block_device를
 * 생성한다. 공유하는 핵심 자료구조는 struct cmdline_subpart(파티션 1개),
 * struct cmdline_parts(장치 1개의 파티션 모음), 그리고 check.h가 정의하는
 * struct parsed_partitions이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - cmdline_partition()      : 이 파일의 유일한 공개 진입점. 최초 1회 파싱을
 *   트리거하고, 현재 디스크에 해당하는 파티션 셋을 찾아 등록/검증까지 수행.
 * - cmdline_parts_parse()/parse_parts()/parse_subpart() : ";"로 구분된 여러
 *   장치 정의, 각 장치 내 ","로 구분된 여러 파티션 정의, "size@offset(name)ro"
 *   형태 파티션 하나를 각각 파싱하는 3단계 하강 파서.
 * - cmdline_parts_set()/add_part() : 파싱된 논리적 오프셋/크기를 디스크의
 *   실제 용량에 맞춰 자르고(clip), parsed_partitions 상태 배열에 등록한다.
 * - cmdline_parts_verifier()/has_overlaps() : 등록이 끝난 파티션들 사이에
 *   구간 중첩이 있는지 전수 비교해 커널 로그로 경고한다.
 * - struct cmdline_subpart   : 파티션 하나(이름/시작 오프셋/크기/플래그)와
 *   다음 파티션으로의 포인터.
 * - struct cmdline_parts     : 블록 장치 하나(이름)와 그 장치에 속한
 *   cmdline_subpart 연결 리스트, 그리고 다음 장치로의 포인터.
 */

#include <linux/blkdev.h>	/* [한국어] struct gendisk, sector_t, BDEVNAME_SIZE, get_capacity() 등 블록 계층 공용 자료구조/헬퍼 선언. 디스크 용량 조회와 이름 버퍼 크기가 여기서 온다. */
#include <linux/fs.h>	/* [한국어] 블록 장치와 파일시스템 계층이 만나는 공용 정의 모음(추정) - 블록 계층 헤더 체인이 관례적으로 함께 포함한다. */
#include <linux/slab.h>	/* [한국어] kzalloc_obj()/kfree(): cmdline_subpart·cmdline_parts 구조체를 0으로 초기화해 할당/해제하는 슬랩 할당자 API. */
#include "check.h"	/* [한국어] 로컬 헤더: struct parsed_partitions, put_partition(), 이 파일이 구현하는 cmdline_partition()의 프로토타입을 선언. ADDPART_FLAG_READONLY를 정의하는 block/blk.h도 내부적으로 포함. */


/* [한국어] parse_subpart()가 파티션 정의 문자열 끝의 "ro"/"lk" 접미사를 해석해 cmdline_subpart.flags 필드에 OR로 채워 넣는 비트 플래그 모음. */
/* partition flags */
#define PF_RDONLY                   0x01 /* Device is read only */
/* [한국어] bit0. add_part()가 이 비트를 검사해 ADDPART_FLAG_READONLY로
 * 변환하며, 최종적으로 해당 파티션이 읽기 전용 block_device로 등록되게 만든다. */
#define PF_POWERUP_LOCK             0x02 /* Always locked after reset */
/* [한국어] bit1. parse_subpart()가 "lk" 접미사를 만나면 세팅하지만, 이
 * 파일 안에서는 add_part()가 PF_RDONLY만 검사하므로 PF_POWERUP_LOCK은
 * 소비되지 않는다 — flags 필드에 정보로만 보존되며 이 파일 범위의 동작에는
 * 영향을 주지 않는다(하드웨어 write-protect 래치 등을 하위 드라이버가
 * 별도로 참조할 가능성이 있음, 추정). */

/*
 * [한국어]
 * struct cmdline_subpart - 파티션 하나를 표현하는 노드.
 *
 * blkdevparts= 문자열 중 "size@offset(name)ro" 형태의 파티션 정의 하나가
 * parse_subpart()에 의해 이 구조체 인스턴스 하나로 변환된다. 같은 블록
 * 장치에 속한 여러 파티션은 next_subpart로 단방향 연결 리스트를 이룬다.
 */
struct cmdline_subpart {
	char name[BDEVNAME_SIZE]; /* partition name, such as 'rootfs' */
	/* [한국어] 파티션의 볼륨 이름(예: "rootfs", "kernel").
	 * 설정자: parse_subpart()가 "(name)" 괄호 구문을 strsep()으로 잘라
	 *         strscpy()로 복사한다. 괄호가 없으면 name[0]='\0'(빈 문자열).
	 * 읽는 자: add_part()가 그대로 partition_meta_info.volname으로 복사해
	 *         넘기며, 커널이 PARTNAME uevent와 /dev/disk/by-partlabel/<name>
	 * 심볼릭 링크를 만드는 데 사용한다.
	 * 값 범위: 최대 BDEVNAME_SIZE(32)-1 문자의 NUL 종단 문자열. 빈 문자열 허용.
	 * 동기화: 부팅 시 1회 파싱되는 값이라 단일 스레드에서만 쓰기가 일어나며
	 *         별도 락이 필요 없다. */
	sector_t from;
	/* [한국어] 파티션 시작 오프셋. 파싱 직후에는 "바이트" 단위이며,
	 * add_part()에 전달되는 시점에 그 안에서 >>9로 섹터 단위로 변환된다.
	 * 설정자: parse_subpart()가 "@offset" 구문을 memparse()로 해석해 대입.
	 *         "@"가 없으면 (sector_t)(~0ULL)(전 비트 1, sentinel)을 대입해
	 *         "명시되지 않음"을 표시하고, cmdline_parts_set()이 이 sentinel을
	 *         만나면 직전 파티션이 끝난 지점(누적 커서 from)으로 재계산해 덮어쓴다.
	 * 읽는 자: cmdline_parts_set(), add_part(), cmdline_parts_verifier()/has_overlaps().
	 * 값 범위: 0 ~ (sector_t)(~0ULL). add_part() 호출 시점에는 항상 sentinel이
	 *         실제 값으로 대체되어 있다.
	 * 동기화: boot-time 파싱 스레드 하나만 쓰고 읽으므로 잠금이 필요 없다. */
	sector_t size;
	/* [한국어] 파티션 크기(파싱 직후 기준 바이트 단위).
	 * 설정자: parse_subpart()가 크기 토큰을 memparse()로 해석하거나, 토큰이
	 *         "-"이면 (sector_t)(~0ULL)을 대입해 "남은 공간 전부"를 의미하게
	 *         한다. cmdline_parts_set()이 디스크 남은 용량보다 크면
	 *         (disk_size - from)으로 잘라(clip) 실제 값으로 덮어쓴다.
	 * 읽는 자: cmdline_parts_set(), add_part()(섹터 변환 후 put_partition에
	 *         전달), has_overlaps()(겹침 판정에 사용).
	 * 값 범위: 최소 PAGE_SIZE 이상(미만이면 parse_subpart()가 -EINVAL로 거부),
	 *         최대 (sector_t)(~0ULL) 또는 디스크 크기로 clip된 값.
	 * 동기화: from과 동일하게 boot-time 단일 스레드 처리라 락 불필요. */
	int flags;
	/* [한국어] PF_RDONLY / PF_POWERUP_LOCK 비트를 OR로 담는 플래그.
	 * 설정자: parse_subpart()가 파티션 정의 문자열 끝의 "ro"/"lk" 접미사를
	 *         strncmp()로 검사해 세팅한다. 접미사가 없으면 0(플래그 없음).
	 * 읽는 자: add_part()가 PF_RDONLY 비트만 검사해 ADDPART_FLAG_READONLY로
	 *         변환한다. PF_POWERUP_LOCK은 이 파일 안에서는 소비되지 않는다.
	 * 값 범위: 0, PF_RDONLY(0x01), PF_POWERUP_LOCK(0x02), 또는 OR인 0x03.
	 * 동기화: 위와 동일, boot-time 단일 스레드. */
	struct cmdline_subpart *next_subpart;
	/* [한국어] 같은 cmdline_parts(블록 장치) 안에서 다음 파티션을 가리키는
	 * 단방향 연결 리스트 포인터.
	 * 설정자: parse_parts()가 파티션 문자열을 ","로 순회하며 next_subpart의
	 *         주소(이중 포인터)에 새 파티션을 이어붙인다.
	 * 읽는 자: free_subpart()(전체 해제 순회), cmdline_parts_set()(등록 순회).
	 * 값 범위: 유효한 cmdline_subpart 포인터 또는 리스트 끝을 뜻하는 NULL.
	 * 동기화: boot-time 단일 스레드에서만 구성되며, 이후에는 읽기 전용으로만
	 *         순회된다. */
};

/*
 * [한국어]
 * struct cmdline_parts - 블록 장치 하나에 속한 모든 파티션의 모음.
 *
 * blkdevparts= 문자열 중 ";"로 구분된 "device:part1,part2,..." 정의 하나가
 * parse_parts()에 의해 이 구조체 인스턴스 하나로 변환된다. 여러 장치는
 * next_parts로 단방향 연결 리스트를 이루며, 그 head가 전역 변수 bdev_parts다.
 */
struct cmdline_parts {
	char name[BDEVNAME_SIZE]; /* block device, such as 'mmcblk0' */
	/* [한국어] 이 파티션 집합이 적용될 블록 장치 이름(예: "mmcblk0").
	 * 설정자: parse_parts()가 "device:part1,part2..." 문자열에서 ":" 앞부분을
	 *         strsep()으로 잘라 strscpy()로 복사한다.
	 * 읽는 자: cmdline_parts_find()가 strncmp()로 state->disk->disk_name과
	 *         비교해 현재 스캔 중인 디스크에 해당하는 항목을 찾는다.
	 * 값 범위: 최대 BDEVNAME_SIZE(32)-1 문자의 장치명 문자열.
	 * 동기화: boot-time 파싱 이후에는 읽기 전용. */
	unsigned int nr_subparts;
	/* [한국어] 이 장치에 속한 파티션(cmdline_subpart) 개수.
	 * 설정자: parse_parts()가 파티션 하나를 성공적으로 파싱할 때마다 1씩 증가.
	 * 읽는 자: 이 파일 안에서 값을 다시 읽는 코드는 없다 — subpart 연결
	 *         리스트를 NULL 종단까지 순회하는 방식이 주로 쓰이므로, 이 필드는
	 *         주로 디버깅/향후 확장을 위한 카운터 역할로 보인다(추정).
	 * 값 범위: 0 이상. 파싱이 끝난 뒤에도 0이면 parse_parts()가 -EINVAL로
	 *         실패 처리한다(newparts->subpart가 NULL인 경우와 대응).
	 * 동기화: boot-time 단일 스레드에서만 갱신. */
	struct cmdline_subpart *subpart;
	/* [한국어] 이 장치에 속한 파티션 연결 리스트의 head 포인터.
	 * 설정자: parse_parts()가 next_subpart(이중 포인터)를 통해 첫 번째
	 *         파티션을 이 필드에 연결한다.
	 * 읽는 자: free_subpart()(순회하며 kfree), cmdline_parts_set()(순회하며
	 *         디스크에 실제 등록).
	 * 값 범위: 유효한 cmdline_subpart 포인터, 또는 파티션이 하나도 없을 때
	 *         NULL(이 경우 parse_parts()가 에러로 처리하므로 파싱 성공 후에는
	 *         항상 비-NULL).
	 * 동기화: boot-time 단일 스레드 구성 후 읽기 전용 순회. */
	struct cmdline_parts *next_parts;
	/* [한국어] ";"로 구분된 다음 블록 장치의 cmdline_parts를 가리키는
	 * 단방향 연결 리스트 포인터.
	 * 설정자: cmdline_parts_parse()가 next_parts(이중 포인터)를 통해 장치별
	 *         파싱 결과를 순서대로 이어붙인다.
	 * 읽는 자: cmdline_parts_free()(전체 해제 순회), cmdline_parts_find()
	 *         (디스크 이름 매칭 순회).
	 * 값 범위: 유효한 cmdline_parts 포인터 또는 리스트 끝을 뜻하는 NULL.
	 * 동기화: 이 체인은 cmdline_partition()이 재파싱을 위해 통째로 해제
	 *         후 재구성하는 경우를 제외하면 boot 시 한 번 구성되고 이후
	 *         읽기 전용으로 유지된다. */
};

/*
 * [한국어]
 * parse_subpart - 파티션 정의 문자열 하나를 파싱해 cmdline_subpart 하나를 만든다.
 *
 * @subpart: 출력 파라미터(이중 포인터). 성공하면 새로 kzalloc_obj()로 할당한
 *           cmdline_subpart를 가리키도록 채워지고, 실패하거나 완료 전이면
 *           함수 초입에 NULL로 리셋된다. 호출자(parse_parts)는 이 포인터가
 *           가리키는 위치에 연결 리스트 노드를 이어붙인다.
 * @partdef: 파싱할 파티션 정의 문자열의 시작 지점. 형식은
 *           "<size>[@<offset>](<name>)[ro][lk]"
 *           (Documentation/block/cmdline-partition.rst 참고). 함수 내부에서
 *           memparse()/strsep() 등이 이 포인터를 소비하며 전진시키므로,
 *           호출이 끝나면 partdef가 가리키는 내용은 이미 잘려 있다.
 * @return: 0이면 성공(*subpart에 새 노드가 채워짐). 음수(-ENOMEM, -EINVAL)면
 *          실패이며 *subpart는 NULL로 유지된다.
 *
 * 이 함수가 왜 필요한가: blkdevparts= 문자열은 ","로 구분된 파티션 정의들의
 * 나열이며, 이 함수는 그 중 하나의 정의를 해석해 크기/오프셋/이름/플래그를
 * 뽑아내는 최하위 파서다. 크기가 "-"이면 "남은 공간 전부", 오프셋이 생략되면
 * "직전 파티션 바로 뒤"라는 의미의 sentinel 값 (sector_t)(~0ULL)을 잠정적으로
 * 채워두고, 실제 숫자로 확정하는 일은 이후 cmdline_parts_set()이 디스크 전체
 * 레이아웃을 알게 되는 시점으로 미룬다(2단계 처리).
 * 동작 순서: (1) 노드 할당 -> (2) 크기 토큰 파싱("-" 또는 memparse) -> (3)
 * "@offset" 유무 파싱 -> (4) "(name)" 유무 파싱 -> (5) "ro"/"lk" 접미사 플래그
 * 파싱 -> (6) 성공 시 *subpart에 대입.
 * 실행 컨텍스트: 커널 부팅 중 또는 디스크 파티션 스캔 경로의 단일 실행
 * 스레드. 재진입/동시 호출을 고려하지 않아도 되는 위치이다.
 * 호출자: parse_parts()가 ","로 자른 파티션 정의 토큰마다 이 함수를 한 번씩 호출.
 * 피호출자: kzalloc_obj(), memparse(), strsep(), strscpy(), strncmp(), pr_warn(), kfree().
 * 에러 경로: 메모리 할당 실패(-ENOMEM)는 즉시 반환. 그 외 문자열 형식 오류는
 * "fail:" 레이블로 goto해 할당했던 new_subpart를 kfree()로 되돌리고 에러 코드를
 * 반환한다.
 *
 * 호출 체인:
 *   cmdline_parts_parse -> parse_parts -> [이 함수]
 */
static int parse_subpart(struct cmdline_subpart **subpart, char *partdef)
{
	int ret = 0;	/* [한국어] 이 함수의 반환값이 될 에러 코드 누산 변수. 기본 0(성공)으로 시작하며, 형식 오류를 만나면 -EINVAL 등으로 덮어써진다. */
	struct cmdline_subpart *new_subpart;	/* [한국어] 이번 호출에서 새로 만들 파티션 노드를 가리킬 로컬 포인터. 아직 미할당. */

	*subpart = NULL;	/* [한국어] 출력 파라미터를 우선 NULL로 리셋 - 이후 실패 경로로 빠지더라도 호출자가 미완성 포인터를 보지 않게 한다. */

	new_subpart = kzalloc_obj(struct cmdline_subpart);	/* [한국어] cmdline_subpart 크기만큼 힙을 0으로 채워 할당(타입-안전 kzalloc_obj, kzalloc(sizeof(struct cmdline_subpart), GFP_KERNEL)와 동등). 0-초기화라 flags 등 명시적으로 안 채운 필드는 자동으로 0이 된다. */
	if (!new_subpart)	/* [한국어] 할당 실패 검사 - new_subpart가 NULL이면 메모리 부족. */
		return -ENOMEM;		/* [한국어] 메모리 부족으로 이 파티션 정의의 파싱을 포기하고, 상위 호출자(parse_parts)에게 -ENOMEM을 그대로 전파한다. */

	if (*partdef == '-') {	/* [한국어] 크기 토큰의 첫 글자가 '-'이면 "남은 공간 전부"를 의미하는 특수 표기 - 뒤이은 @offset/(name) 파싱은 계속 진행된다. */
		new_subpart->size = (sector_t)(~0ULL);		/* [한국어] size에 (sector_t)(~0ULL)(전 비트 1) sentinel을 대입 - 실제 크기는 cmdline_parts_set()이 디스크 남은 공간으로 확정한다. */
		partdef++;		/* [한국어] '-' 한 글자를 소비하고 다음 파싱 지점('@' 또는 '(' 등)으로 전진. */
	} else {	/* [한국어] '-'가 아니면 명시적 숫자+단위(K/M/G/T/P/E) 크기 표기 분기. */
		new_subpart->size = (sector_t)memparse(partdef, &partdef);		/* [한국어] memparse()가 숫자+단위 접미사를 바이트 수로 환산하고, partdef 포인터를 소비한 만큼 전진시킨다(두 번째 인자로 갱신된 위치를 돌려받음). */
		if (new_subpart->size < (sector_t)PAGE_SIZE) {		/* [한국어] 파싱된 크기가 PAGE_SIZE(보통 4096바이트) 미만이면 의미 없는 파티션으로 간주해 거부. */
			pr_warn("cmdline partition size is invalid.");			/* [한국어] 커널 로그에 형식 오류를 알림 - 사용자가 blkdevparts= 문자열을 점검하도록 유도. */
			ret = -EINVAL;			/* [한국어] 반환할 에러 코드를 -EINVAL(Invalid argument)로 설정. */
			goto fail;			/* [한국어] fail 레이블로 점프 - 이미 할당한 new_subpart를 되돌리고 함수를 종료한다. */
		}
	}

	if (*partdef == '@') {	/* [한국어] '@' 문자가 있으면 시작 오프셋이 명시적으로 지정된 경우. */
		partdef++;		/* [한국어] '@' 한 글자를 소비하고 오프셋 숫자 파싱 지점으로 전진. */
		new_subpart->from = (sector_t)memparse(partdef, &partdef);		/* [한국어] memparse()로 오프셋(바이트)을 해석 - '@' 뒤의 수치가 파티션 시작 지점이 된다. */
	} else {	/* [한국어] '@'가 없으면 오프셋 생략 - "직전 파티션 바로 뒤에 이어 붙인다"는 의미. */
		new_subpart->from = (sector_t)(~0ULL);		/* [한국어] from에 (sector_t)(~0ULL) sentinel 대입 - cmdline_parts_set()이 순회 중 누적 커서 값으로 재계산해 덮어쓴다. */
	}

	if (*partdef == '(') {	/* [한국어] '(' 문자가 있으면 괄호로 감싼 파티션 이름이 뒤따르는 경우. */
		partdef++;		/* [한국어] '(' 한 글자를 소비하고 이름 문자열 파싱 지점으로 전진. */
		char *next = strsep(&partdef, ")");		/* [한국어] strsep()으로 ')' 구분자까지를 잘라내 이름 토큰을 얻는다 - partdef는 ')' 다음 위치로 갱신(또는 구분자가 없으면 전체 소비 후 NULL). */

		if (!next) {		/* [한국어] next가 NULL이면(즉 partdef 포인터 자체가 이미 NULL이었던 방어적 상황) 형식 오류로 처리. */
			pr_warn("cmdline partition format is invalid.");			/* [한국어] 커널 로그에 괄호 형식 오류를 알림. */
			ret = -EINVAL;			/* [한국어] 에러 코드를 -EINVAL로 설정. */
			goto fail;			/* [한국어] fail 레이블로 점프. */
		}

		strscpy(new_subpart->name, next, sizeof(new_subpart->name));		/* [한국어] strscpy()로 이름 토큰을 new_subpart->name 버퍼(BDEVNAME_SIZE 크기)에 NUL 종단 복사 - 버퍼 크기를 넘는 길이는 잘린다. */
	} else	/* [한국어] '('가 없으면 이름 생략 -> 빈 문자열로 초기화하는 else 분기(중괄호 없는 단일 문장). */
		new_subpart->name[0] = '\0';		/* [한국어] name[0]에 NUL을 대입해 빈 문자열로 명시 - kzalloc_obj가 이미 0으로 채웠으므로 사실상 결과는 동일하지만 의도를 명확히 드러낸다. */

	new_subpart->flags = 0;	/* [한국어] flags를 0으로 재설정 - kzalloc_obj가 이미 0으로 초기화했으므로 실질적으로는 중복이나, 아래에서 OR로 비트를 쌓기 전 초기 상태를 명시적으로 보증한다. */

	if (!strncmp(partdef, "ro", 2)) {	/* [한국어] 남은 문자열이 "ro"로 시작하는지 검사 - 읽기 전용 접미사. */
		new_subpart->flags |= PF_RDONLY;		/* [한국어] PF_RDONLY 비트를 OR로 세팅. */
		partdef += 2;		/* [한국어] "ro" 두 글자를 소비하고 다음(예: "lk") 파싱 지점으로 전진. */
	}

	if (!strncmp(partdef, "lk", 2)) {	/* [한국어] 남은 문자열이 "lk"로 시작하는지 검사 - 전원 인가 시 잠금 접미사. "ro" 뒤에도 독립적으로 검사되므로 "rolk"처럼 이어 붙여 두 플래그를 함께 지정할 수 있다. */
		new_subpart->flags |= PF_POWERUP_LOCK;		/* [한국어] PF_POWERUP_LOCK 비트를 OR로 세팅. */
		partdef += 2;		/* [한국어] "lk" 두 글자를 소비. */
	}

	*subpart = new_subpart;	/* [한국어] 완성된 new_subpart를 출력 파라미터에 연결 - 호출자(parse_parts)가 이 포인터로 리스트에 이어붙인다. */
	return 0;	/* [한국어] 성공 반환. */
fail:	/* [한국어] 실패 시 점프 목적지 - 지금까지 할당한 new_subpart를 되돌리는 정리 구간 시작. */
	kfree(new_subpart);	/* [한국어] 실패 경로: 여기까지 오는 동안 할당했던 new_subpart를 해제해 메모리 누수를 막는다. */
	return ret;	/* [한국어] 위에서 goto로 설정된 에러 코드(ret)를 그대로 호출자에게 반환. */
}

/*
 * [한국어]
 * free_subpart - 한 블록 장치(cmdline_parts)에 속한 모든 cmdline_subpart를 해제한다.
 *
 * @parts: 파티션 리스트를 비울 대상 cmdline_parts. 함수가 끝나면
 *         parts->subpart는 NULL이 된다(리스트가 완전히 빈 상태).
 * @return: 없음(void).
 *
 * 이 함수가 왜 필요한가: parse_parts()가 파티션 정의 중 하나라도 파싱에
 * 실패하면 그때까지 만들어 둔 subpart 노드들을 되돌려야 하고, 또
 * cmdline_parts_free()가 장치 전체를 해제할 때도 먼저 그 장치의 파티션들부터
 * 청소해야 한다 - 이 두 경우를 공통 처리하는 헬퍼다.
 * 동작 순서: parts->subpart가 NULL이 될 때까지, head를 하나씩 떼어
 * next_subpart로 다음 노드를 가리키게 한 뒤 kfree() - 전형적인 연결 리스트
 * 전체 해제 패턴.
 * 실행 컨텍스트: 호출자와 동일한 boot-time/파티션 스캔 단일 스레드.
 * 호출자: parse_parts()(실패 시 되돌림), cmdline_parts_free()(정상 해제).
 * 피호출자: kfree().
 * 에러 경로: 없음(항상 성공, 반환값도 없음).
 *
 * 호출 체인:
 *   parse_parts(실패시) -> [이 함수]
 *   cmdline_parts_free -> [이 함수]
 */
static void free_subpart(struct cmdline_parts *parts)
{
	struct cmdline_subpart *subpart;	/* [한국어] 순회하며 떼어낼 현재 노드를 담을 로컬 포인터. */

	while (parts->subpart) {	/* [한국어] head(parts->subpart)가 NULL이 될 때까지 반복 - 리스트가 완전히 빌 때 종료. */
		subpart = parts->subpart;		/* [한국어] 현재 head 노드를 subpart에 잡아둔다(곧 해제할 대상). */
		parts->subpart = subpart->next_subpart;		/* [한국어] head를 다음 노드로 전진시켜 리스트 앞부분을 미리 끊어낸다 - kfree 이후에도 리스트 무결성이 깨지지 않도록 순서를 지킨다. */
		kfree(subpart);		/* [한국어] 떼어낸 노드의 메모리를 반납. */
	}
}

/*
 * [한국어]
 * parse_parts - "device:part1,part2,..." 형태의 장치 하나 분량 정의를 파싱한다.
 *
 * @parts: 출력 파라미터(이중 포인터). 성공하면 새로 할당한 cmdline_parts를
 *         가리키도록 채워지고, 실패 시 NULL로 유지된다.
 * @bdevdef: 파싱할 "device:part1,part2,..." 문자열. strsep() 호출들이 이
 *           버퍼를 직접 변형(구분자 위치에 NUL 기록)하며 소비하므로, 호출자가
 *           건네는 버퍼는 반드시 쓰기 가능한(수정 가능한) 메모리여야 한다
 *           (cmdline_parts_parse()가 kstrdup()으로 미리 복사해 두는 이유).
 * @return: 0이면 성공. 음수(-ENOMEM, -EINVAL)면 실패이며 *parts는 NULL.
 *
 * 이 함수가 왜 필요한가: blkdevparts=의 최상위 구조는 ";"로 구분된 장치별
 * 정의의 나열이고, 각 장치 정의는 다시 "장치이름:파티션들"로 나뉜다. 이
 * 함수는 그 중 장치 하나의 정의를 맡아 장치 이름을 뽑아내고, ","로 구분된
 * 파티션 정의들을 순서대로 parse_subpart()에 위임해 cmdline_subpart 연결
 * 리스트를 완성한다.
 * 동작 순서: (1) cmdline_parts 노드 할당 -> (2) ":" 앞부분을 장치 이름으로
 * 저장 -> (3) ","로 파티션 정의들을 순회하며 parse_subpart() 호출, 성공할
 * 때마다 리스트에 이어붙이고 개수 증가 -> (4) 파티션이 하나도 만들어지지
 * 않았으면 에러 -> (5) 성공 시 *parts에 대입.
 * 실행 컨텍스트: 호출자와 동일한 boot-time 단일 스레드.
 * 호출자: cmdline_parts_parse()가 ";"로 자른 장치별 정의 토큰마다 한 번씩 호출.
 * 피호출자: kzalloc_obj(), strsep(), strscpy(), parse_subpart(), pr_warn(),
 *           free_subpart(), kfree().
 * 에러 경로: 각 실패 지점은 "fail:" 레이블로 모여 free_subpart() + kfree()로
 * 지금까지 만든 노드들을 되돌린 뒤 에러 코드를 반환한다. 장치 이름이 없는
 * 경우(장치 이름을 strsep()으로 자르지 못한 방어 분기)는 ret의 초기값인 -EINVAL이 그대로 반환값이 된다.
 *
 * 호출 체인:
 *   cmdline_parts_parse -> [이 함수] -> parse_subpart
 */
static int parse_parts(struct cmdline_parts **parts, char *bdevdef)
{
	int ret = -EINVAL;	/* [한국어] 반환값 누산 변수. 기본값 -EINVAL은 "장치 이름이 없음" 에러 경로(아래 next 파싱 직후의 방어 분기)가 별도 대입 없이 이 값을 그대로 쓰기 위한 것이다. */
	char *next;	/* [한국어] strsep()이 토큰을 잘라 돌려줄 때 담을 로컬 포인터. */
	struct cmdline_subpart **next_subpart;	/* [한국어] 다음 파티션 노드를 이어붙일 위치를 가리키는 이중 포인터 - 처음엔 newparts->subpart 자체의 주소. */
	struct cmdline_parts *newparts;	/* [한국어] 이번 호출에서 새로 만들 cmdline_parts(장치 하나)를 가리킬 로컬 포인터. */

	*parts = NULL;	/* [한국어] 출력 파라미터를 우선 NULL로 리셋 - 실패 시 호출자가 미완성 포인터를 보지 않게 한다. */

	newparts = kzalloc_obj(struct cmdline_parts);	/* [한국어] cmdline_parts 크기만큼 힙을 0으로 채워 할당. */
	if (!newparts)	/* [한국어] 할당 실패 검사. */
		return -ENOMEM;		/* [한국어] 메모리 부족: 상위(cmdline_parts_parse)에 -ENOMEM 전파. */

	next = strsep(&bdevdef, ":");	/* [한국어] ":" 구분자 앞부분(장치 이름)을 잘라낸다 - bdevdef는 ":" 다음(파티션 목록 시작)으로 갱신. */
	if (!next) {	/* [한국어] next가 NULL이면 ":" 자체를 포함해 아무 것도 없었다는 뜻(방어적 검사, bdevdef가 이미 NULL이었던 경우). */
		pr_warn("cmdline partition has no block device.");		/* [한국어] 커널 로그에 장치 이름 누락을 알림. */
		goto fail;		/* [한국어] fail로 점프 - 이때 ret은 함수 시작부에서 초기화해 둔 -EINVAL 값 그대로 사용된다(별도 대입 없음). */
	}

	strscpy(newparts->name, next, sizeof(newparts->name));	/* [한국어] 장치 이름 토큰을 newparts->name 버퍼(BDEVNAME_SIZE)에 NUL 종단 복사. */
	newparts->nr_subparts = 0;	/* [한국어] 파티션 개수 카운터를 0으로 초기화(kzalloc_obj로 이미 0이었으나 명시적으로 재확인). */

	next_subpart = &newparts->subpart;	/* [한국어] 다음에 연결할 위치를 리스트 head 필드(subpart)의 주소로 설정 - 아직 파티션이 하나도 없는 초기 상태. */

	while ((next = strsep(&bdevdef, ","))) {	/* [한국어] ","로 파티션 정의 토큰을 하나씩 잘라내며 반복 - 더 이상 토큰이 없으면(strsep이 NULL 반환) 종료. */
		ret = parse_subpart(next_subpart, next);		/* [한국어] 이번 토큰을 parse_subpart()에 위임해 cmdline_subpart 하나로 변환, *next_subpart 위치에 채워 넣는다. */
		if (ret)		/* [한국어] parse_subpart 실패 여부 검사. */
			goto fail;			/* [한국어] 실패 시 fail로 점프 - ret에는 parse_subpart()가 반환한 에러 코드가 그대로 담겨 있다. */

		newparts->nr_subparts++;		/* [한국어] 파티션 하나가 성공적으로 추가됐으므로 카운터 증가. */
		next_subpart = &(*next_subpart)->next_subpart;		/* [한국어] 다음에 연결할 위치를 방금 추가한 노드의 next_subpart 주소로 전진 - 리스트 맨 끝을 계속 추적. */
	}

	if (!newparts->subpart) {	/* [한국어] 파티션이 단 하나도 만들어지지 않았으면(subpart가 여전히 NULL) 유효하지 않은 정의로 간주. */
		pr_warn("cmdline partition has no valid partition.");		/* [한국어] 커널 로그에 유효한 파티션이 없음을 알림. */
		ret = -EINVAL;		/* [한국어] 에러 코드를 -EINVAL로 설정. */
		goto fail;		/* [한국어] fail로 점프. */
	}

	*parts = newparts;	/* [한국어] 완성된 newparts를 출력 파라미터에 연결. */

	return 0;	/* [한국어] 성공 반환. */
fail:	/* [한국어] 실패 시 점프 목적지 - 이 장치를 위해 만들었던 subpart들과 newparts 자체를 되돌리는 정리 구간 시작. */
	free_subpart(newparts);	/* [한국어] 실패 경로: 지금까지 이 장치에 대해 만들어진 subpart들을 전부 해제. */
	kfree(newparts);	/* [한국어] cmdline_parts 노드 자체도 해제. */
	return ret;	/* [한국어] 누적된 에러 코드를 호출자에게 반환. */
}

/*
 * [한국어]
 * cmdline_parts_free - 파싱된 모든 블록 장치의 파티션 목록을 통째로 해제한다.
 *
 * @parts: 해제할 리스트의 head를 가리키는 이중 포인터(보통 &bdev_parts).
 *         함수가 끝나면 *parts는 NULL이 된다.
 * @return: 없음(void).
 *
 * 이 함수가 왜 필요한가: cmdline_partition()이 새 커맨드라인 문자열로 다시
 * 파싱하기 전에 기존 bdev_parts 전역 리스트를 비우거나, cmdline_parts_parse()
 * 자체가 도중에 실패했을 때 지금까지 만든 장치들을 되돌리기 위해 쓰인다.
 * 동작 순서: *parts가 NULL이 될 때까지 리스트의 각 cmdline_parts 노드에
 * 대해 (1) 그 장치의 파티션들을 free_subpart()로 먼저 비우고 (2) 노드
 * 자체를 kfree()한 뒤 (3) 다음 노드로 전진.
 * 실행 컨텍스트: 호출자와 동일한 boot-time/파티션 스캔 단일 스레드.
 * 호출자: cmdline_parts_parse()(실패 시), cmdline_partition()(재파싱 전 기존
 * 결과 정리).
 * 피호출자: free_subpart(), kfree().
 * 에러 경로: 없음(항상 성공).
 *
 * 호출 체인:
 *   cmdline_parts_parse(실패시) -> [이 함수]
 *   cmdline_partition -> [이 함수]
 */
static void cmdline_parts_free(struct cmdline_parts **parts)
{
	struct cmdline_parts *next_parts;	/* [한국어] 순회 중 현재 노드를 해제한 뒤 진행할 다음 노드를 임시로 담아두는 포인터. */

	while (*parts) {	/* [한국어] *parts가 NULL이 될 때까지(리스트가 완전히 빌 때까지) 반복. */
		next_parts = (*parts)->next_parts;		/* [한국어] 현재 노드를 해제하기 전에 다음 노드 주소를 미리 저장 - 해제 후에는 접근할 수 없으므로. */
		free_subpart(*parts);		/* [한국어] 이 장치에 속한 파티션들을 먼저 모두 해제(리스트 내부 자원부터 정리). */
		kfree(*parts);		/* [한국어] cmdline_parts 노드 자체를 해제. */
		*parts = next_parts;		/* [한국어] head(*parts)를 미리 저장해 둔 다음 노드로 전진. */
	}
}

/*
 * [한국어]
 * cmdline_parts_parse - "blkdevparts=" 전체 문자열을 파싱해 모든 장치의
 * 파티션 정의를 만든다.
 *
 * @parts: 출력 파라미터(이중 포인터). 성공하면 파싱된 cmdline_parts 리스트의
 *         head를 가리키도록 채워진다.
 * @cmdline: __setup()으로 저장돼 있던, 커널이 관리하는 커맨드라인 문자열에
 *           대한 const 포인터. strsep()이 버퍼를 직접 수정하며 소비하므로
 *           이 함수는 반드시 자체 복사본(buf)을 만든 뒤 그 복사본만 변형한다.
 * @return: 0이면 성공. 음수(-ENOMEM, -EINVAL)면 실패.
 *
 * 이 함수가 왜 필요한가: blkdevparts=의 최상위 구조는 ";"로 구분된 여러
 * 장치 정의의 나열이다. 이 함수는 그 전체 문자열을 한 번만 훑으며 각 장치
 * 정의를 parse_parts()에 위임하고, 그 결과들을 next_parts로 이어 하나의
 * 전역 리스트를 완성한다. 이 리스트가 곧 cmdline_partition()이 참조하는
 * 전역 변수 bdev_parts의 내용이 된다.
 * 동작 순서: (1) cmdline을 kstrdup()으로 복제해 mutable 버퍼(buf) 확보 ->
 * (2) ";"로 장치 정의 토큰을 순회하며 parse_parts() 호출, 성공할 때마다
 * 리스트에 이어붙임 -> (3) 장치가 하나도 파싱되지 않았으면 에러 -> (4)
 * 성공/실패 어느 경로든 kfree(buf)로 복사본을 반납.
 * 실행 컨텍스트: 디스크 파티션 스캔 경로의 단일 스레드(cmdline_partition의
 * 최초 호출 시 1회만 실행됨).
 * 호출자: cmdline_partition().
 * 피호출자: kstrdup(), strsep(), parse_parts(), pr_warn(), kfree(),
 *           cmdline_parts_free().
 * 에러 경로: 실패 지점들은 모두 "fail:" 레이블로 모이고, cmdline_parts_free()로
 * 지금까지 만든 리스트를 되돌린 뒤 공통 종료 경로인 "done:"으로 흘러가
 * kfree(buf) 후 에러 코드를 반환한다.
 *
 * 호출 체인:
 *   cmdline_partition -> [이 함수] -> parse_parts
 */
static int cmdline_parts_parse(struct cmdline_parts **parts,
		const char *cmdline)
{
	int ret;	/* [한국어] 반환값 누산 변수. */
	char *buf;	/* [한국어] kstrdup()이 돌려준 할당 블록의 시작 주소 - 나중에 kfree()할 때는 반드시 이 포인터를 써야 한다(pbuf는 소비되며 이동하므로). */
	char *pbuf;	/* [한국어] strsep()이 실제로 갉아먹으며 전진시키는 이동용 커서 - buf와 시작은 같지만 파싱이 진행되며 값이 바뀐다. */
	char *next;	/* [한국어] strsep()이 토큰을 잘라 돌려줄 때 담을 로컬 포인터. */
	struct cmdline_parts **next_parts;	/* [한국어] 다음 장치 노드를 이어붙일 위치를 가리키는 이중 포인터 - 처음엔 parts 자체의 주소. */

	*parts = NULL;	/* [한국어] 출력 파라미터를 우선 NULL로 리셋. */

	pbuf = buf = kstrdup(cmdline, GFP_KERNEL);	/* [한국어] const cmdline 문자열을 kstrdup()으로 복제 - strsep()이 구분자 자리에 NUL을 써 넣으며 원본을 변형하므로, 커널이 계속 소유하는 원본 커맨드라인 버퍼를 훼손하지 않기 위해 반드시 복사본이 필요하다. pbuf와 buf 둘 다 같은 주소로 초기화. */
	if (!buf)	/* [한국어] 복제 실패 검사. */
		return -ENOMEM;		/* [한국어] 메모리 부족: 상위(cmdline_partition)에 -ENOMEM 전파. */

	next_parts = parts;	/* [한국어] 다음에 연결할 위치를 리스트 head(parts)의 주소로 설정. */

	while ((next = strsep(&pbuf, ";"))) {	/* [한국어] ";"로 장치 정의 토큰을 하나씩 잘라내며 반복 - pbuf가 전진하며 소비된다. */
		ret = parse_parts(next_parts, next);		/* [한국어] 이번 토큰(하나의 "device:part1,part2.." 정의)을 parse_parts()에 위임. */
		if (ret)		/* [한국어] parse_parts 실패 여부 검사. */
			goto fail;			/* [한국어] 실패 시 fail로 점프 - ret에는 parse_parts()가 반환한 에러 코드가 담겨 있다. */

		next_parts = &(*next_parts)->next_parts;		/* [한국어] 다음에 연결할 위치를 방금 추가한 장치 노드의 next_parts 주소로 전진. */
	}

	if (!*parts) {	/* [한국어] 장치 정의가 단 하나도 파싱되지 않았으면(*parts가 여전히 NULL) 유효하지 않은 커맨드라인으로 간주. */
		pr_warn("cmdline partition has no valid partition.");		/* [한국어] 커널 로그에 유효한 파티션 정의가 없음을 알림. */
		ret = -EINVAL;		/* [한국어] 에러 코드를 -EINVAL로 설정. */
		goto fail;		/* [한국어] fail로 점프. */
	}

	ret = 0;	/* [한국어] 여기까지 도달했다면 전체 파싱 성공 - ret을 0으로 확정. */
done:	/* [한국어] 성공/실패 공통 종료 목적지 - buf 해제 후 ret을 반환하는 마무리 구간 시작. */
	kfree(buf);	/* [한국어] 성공/실패 공통 종료 지점(done:) - 복제해 두었던 buf를 반납. buf는 이동한 적이 없으므로 항상 유효한 할당 시작 주소를 가리킨다. */
	return ret;	/* [한국어] 최종 결과 코드를 호출자(cmdline_partition)에게 반환. */

fail:	/* [한국어] 실패 시 점프 목적지 - bdev_parts 전체를 되돌린 뒤 위의 done: 으로 합류. */
	cmdline_parts_free(parts);	/* [한국어] 실패 경로: 지금까지 만들어진 장치 리스트 전체(및 그 안의 파티션들)를 되돌린다. */
	goto done;	/* [한국어] 공통 종료 지점(done)으로 합류해 buf 해제와 반환을 공유한다. */
}

/*
 * [한국어]
 * cmdline_parts_find - 파싱 결과 리스트에서 주어진 블록 장치 이름에 해당하는
 * cmdline_parts를 찾는다.
 *
 * @parts: 검색을 시작할 리스트의 첫 노드(보통 전역 변수 bdev_parts).
 * @bdev: 찾고자 하는 블록 장치 이름(예: state->disk->disk_name, "mmcblk0").
 * @return: 이름이 일치하는 cmdline_parts 포인터, 없으면 NULL.
 *
 * 이 함수가 왜 필요한가: bdev_parts는 blkdevparts= 문자열에 나열된 "모든"
 * 블록 장치의 파티션 정의를 담고 있는 전역 리스트다. cmdline_partition()은
 * 특정 디스크 하나에 대해서만 호출되므로, 그 디스크 이름에 해당하는 항목만
 * 골라내는 선형 검색이 필요하다.
 * 동작: parts가 NULL이 아니고 이름이 다른 동안(strncmp != 0) 계속
 * next_parts로 전진. 종료하면 (일치하는 노드) 또는 (리스트 끝 NULL) 중
 * 하나가 parts에 남아 있으므로 그대로 반환.
 * 실행 컨텍스트: 호출자와 동일한 디스크 파티션 스캔 경로.
 * 호출자: cmdline_partition().
 * 피호출자: strncmp().
 * 에러 경로: 검색 실패는 에러가 아니라 정상적인 "이 디스크는 blkdevparts=에
 * 없음"을 뜻하며, 호출자가 NULL을 보고 0(내 파티션 테이블 아님)을 반환한다.
 *
 * 호출 체인:
 *   cmdline_partition -> [이 함수]
 */
static struct cmdline_parts *cmdline_parts_find(struct cmdline_parts *parts,
					 const char *bdev)
{
	while (parts && strncmp(bdev, parts->name, sizeof(parts->name)))	/* [한국어] parts가 리스트 끝(NULL)에 도달하지 않았고, 현재 노드 이름이 찾는 이름과 다른 동안(strncmp가 0이 아닌 동안) 계속 진행 - sizeof(parts->name)(BDEVNAME_SIZE)만큼만 비교. */
		parts = parts->next_parts;		/* [한국어] 다음 장치 노드로 전진. */
	return parts;	/* [한국어] 일치하는 노드(찾음) 또는 NULL(못 찾음)을 그대로 반환. */
}

/*
 * [한국어] 이 파일 전체가 공유하는 두 전역 상태.
 *
 * cmdline/bdev_parts는 "부팅 시 커맨드라인 문자열 1개를 최초 1회만
 * 파싱해 그 결과를 캐싱"하는 지연(lazy) 파싱 설계의 핵심이다. 이 파일
 * 안에는 명시적인 락(스핀락/뮤텍스)이 전혀 없는데, 이는 실제로는 여러
 * 디스크의 파티션 스캔이 시간적으로 겹치지 않는다는 전제(혹은 상위
 * 계층의 직렬화)에 의존하고 있는 것으로 보인다(추정) - 만약 두 디스크의
 * check_partition()이 진짜 동시에 실행된다면 cmdline/bdev_parts에 대한
 * 경쟁이 이론적으로 가능하다.
 */
static char *cmdline;	/* [한국어] cmdline_parts_setup()이 __setup("blkdevparts=", ...) 콜백으로
 * 저장해 두는 원본 커맨드라인 문자열 포인터(커널이 소유한 버퍼를 가리킴,
 * 이 파일이 직접 할당한 메모리가 아니다). NULL이면 "아직 (재)파싱할
 * blkdevparts= 값이 없다"는 뜻이고, cmdline_partition()이 파싱을 마치면
 * 곧바로 NULL로 되돌려 "이미 소비했다"는 표시로 삼는다. */
static struct cmdline_parts *bdev_parts;	/* [한국어] cmdline_parts_parse()가 만들어낸, blkdevparts=에 나열된 모든
 * 블록 장치의 파티션 정의 연결 리스트 head. cmdline_parts_find()가 이
 * 리스트를 순회하며 특정 디스크 이름에 해당하는 항목을 찾는다.
 * cmdline_partition()이 새로 파싱하기 전에는 cmdline_parts_free()로
 * 기존 값을 비운 뒤 다시 채운다. */

/*
 * [한국어]
 * add_part - 파싱된 파티션 하나를 parsed_partitions 상태 배열의 지정 슬롯에 등록한다.
 *
 * @slot: 등록할 파티션 번호(인덱스). 1부터 시작(슬롯 0은 whole-disk 자리이며
 *        이 파일은 건드리지 않는다) - 호출자 cmdline_parts_set()이 순회
 *        카운터로 넘겨준다.
 * @subpart: 등록할 파티션의 파싱 결과(바이트 단위 from/size, flags, name이
 *           채워져 있고 cmdline_parts_set()이 디스크 크기에 맞춰 이미 clip한
 *           상태).
 * @state: 현재 디스크의 파티션 스캔 상태(parsed_partitions). 이 함수가
 *         state->parts[slot]을 직접 채운다.
 * @return: 0이면 이 파티션 등록 성공. 1이면 슬롯 한도 초과로 이 파티션은
 *          등록하지 못했음(에러는 아니고 "그만 두라"는 신호) - 호출자가 이
 *          값을 보고 순회를 break한다.
 *
 * 이 함수가 왜 필요한가: cmdline_subpart(바이트 단위, 이 파일 전용 표현)를
 * parsed_partitions.parts[](섹터 단위, block layer 공용 표현)로 변환해
 * 실제로 파티션 block_device가 만들어지도록 등록하는 마지막 단계다.
 * 동작 순서: (1) 슬롯 한도 검사 -> (2) put_partition()으로 from/size를
 * 섹터 단위(>>9)로 변환해 등록 -> (3) PF_RDONLY면 ADDPART_FLAG_READONLY로
 * 변환 -> (4) partition_meta_info.volname에 이름 복사 -> (5) 로그 버퍼에
 * "(name)" 추가 -> (6) has_info를 true로 표시(검증기가 이 슬롯을 스캔
 * 대상으로 인식하게 함).
 * 실행 컨텍스트: 호출자와 동일한 디스크 파티션 스캔 단일 스레드.
 * 호출자: cmdline_parts_set()이 파티션마다 한 번씩 호출.
 * 피호출자: put_partition(), strscpy(), seq_buf_printf().
 * 에러 경로: 슬롯 초과 외의 실패는 없다(메모리 할당이 없는 순수 등록 로직).
 *
 * 호출 체인:
 *   cmdline_parts_set -> [이 함수] -> put_partition
 */
static int add_part(int slot, struct cmdline_subpart *subpart,
		struct parsed_partitions *state)
{
	struct partition_meta_info *info;	/* [한국어] 방금 등록한 슬롯의 partition_meta_info(uuid/volname)를 가리킬 포인터. */

	if (slot >= state->limit)	/* [한국어] 요청한 슬롯 번호가 parsed_partitions가 허용하는 최대 파티션 수(state->limit, DISK_MAX_PARTS)를 넘는지 검사. */
		return 1;		/* [한국어] 한도 초과 - 더 이상 등록할 자리가 없으므로 1을 반환해 호출자가 순회를 중단하게 한다(에러 코드가 아닌 "중단 신호"). */

	/* [한국어] >>9는 512로 나누는 것과 같다. blkdevparts= 문법의 크기/오프셋은
	 * 사람이 쓰기 편하도록 바이트 단위(1G, 64M 같은 접미사 포함)로 파싱되는
	 * 반면, put_partition()은 512바이트 섹터 단위를 받으므로 여기서 단위가
	 * 바뀐다. 나눗셈 대신 시프트를 쓰는 것은 커널에서 64비트 나눗셈을 피하는
	 * 관행이며, 파서가 이미 섹터 정렬을 보장하므로 절삭 손실은 없다. */
	put_partition(state, slot, subpart->from >> 9,
		      subpart->size >> 9);

	if (subpart->flags & PF_RDONLY)	/* [한국어] 파싱 단계에서 "ro" 접미사로 세팅됐던 PF_RDONLY 비트 검사. */
		state->parts[slot].flags |= ADDPART_FLAG_READONLY;		/* [한국어] block layer 공용 플래그 ADDPART_FLAG_READONLY로 변환해 이 슬롯에 OR - 이후 파티션 block_device가 읽기 전용으로 만들어진다. */

	info = &state->parts[slot].info;	/* [한국어] 방금 put_partition()이 채운 슬롯의 메타 정보 필드 주소를 잡아둔다. */

	strscpy(info->volname, subpart->name, sizeof(info->volname));	/* [한국어] 파싱된 볼륨 이름을 partition_meta_info.volname(고정 크기 버퍼)에 NUL 종단 복사. */

	seq_buf_printf(&state->pp_buf, "(%s)", info->volname);	/* [한국어] 파티션 스캔 로그 버퍼(check_partition이 나중에 printk로 출력)에 "(이름)" 형태를 추가 - put_partition이 이미 붙인 " 장치명slot" 뒤에 붙어 "mmcblk0p1(rootfs)"처럼 보이게 된다. */

	state->parts[slot].has_info = true;	/* [한국어] 이 슬롯에 유효한 메타 정보가 채워졌음을 표시 - cmdline_parts_verifier()가 겹침을 검사할 슬롯 범위를 판단하는 기준이 된다. */

	return 0;	/* [한국어] 정상 등록 완료. */
}

/*
 * [한국어]
 * cmdline_parts_set - 찾은 장치의 모든 파티션을 실제 디스크 크기에 맞춰
 * 조정(clip)하고 등록한다.
 *
 * @parts: cmdline_parts_find()로 찾은, 현재 디스크에 해당하는 파티션 정의
 *         리스트.
 * @disk_size: 현재 디스크의 실제 전체 용량(바이트 단위) - cmdline_partition()이
 *             get_capacity()로 얻어 넘겨준다.
 * @state: 현재 디스크의 파티션 스캔 상태.
 * @return: 마지막으로 시도된(또는 도달한) 슬롯 번호. 주의: 현재 유일한
 *          호출자인 cmdline_partition()은 이 반환값을 사용하지 않고 무시한다.
 *
 * 이 함수가 왜 필요한가: parse_subpart()는 파티션 정의 문자열만 보고
 * from/size를 확정하며, "-"(남은 공간 전부)나 "@" 생략(직전 파티션 바로
 * 뒤)처럼 실제 디스크 크기를 알아야만 확정할 수 있는 값은 sentinel
 * (sector_t)(~0ULL)로 남겨 둔다. 이 함수는 디스크의 실제 용량을 알고 있는
 * 유일한 지점이므로, 이 sentinel들을 실제 값으로 채우고, 디스크 용량을
 * 넘어서는 크기를 잘라내는(clip) 최종 조정을 담당한다.
 * 동작 순서: 파티션 리스트를 순회하며 각 항목에 대해 (1) from이 sentinel이면
 * 누적 커서(from 지역변수)로 채우고, 아니면 명시된 값으로 커서를 이동 ->
 * (2) 커서가 이미 디스크 크기를 넘으면 더 이상 파티션을 넣을 자리가 없으므로
 * 중단 -> (3) size가 남은 공간보다 크면 남은 공간만큼으로 자름 -> (4) 커서를
 * 이 파티션 크기만큼 전진 -> (5) add_part()로 실제 등록, 슬롯이 부족하면 중단.
 * 실행 컨텍스트: 호출자와 동일한 디스크 파티션 스캔 단일 스레드.
 * 호출자: cmdline_partition().
 * 피호출자: add_part().
 * 에러 경로: 이 함수 자체는 실패를 반환하지 않는다(용량 초과/슬롯 부족은
 * 단순히 나머지 파티션들을 건너뛰는 것으로 처리되며, 이미 등록된 파티션들은
 * 그대로 유효하다).
 *
 * 호출 체인:
 *   cmdline_partition -> [이 함수] -> add_part
 */
static int cmdline_parts_set(struct cmdline_parts *parts, sector_t disk_size,
		struct parsed_partitions *state)
{
	sector_t from = 0;	/* [한국어] 지금까지 배치된 파티션들의 끝을 추적하는 누적 커서(바이트, 디스크 시작인 0에서 출발) - '@' 생략 시 이 값이 다음 파티션의 시작 오프셋이 된다. */
	struct cmdline_subpart *subpart;	/* [한국어] for 루프가 순회할 현재 파티션 노드를 담을 로컬 포인터 - parts->subpart부터 시작해 next_subpart를 따라 전진한다. */
	int slot = 1;	/* [한국어] 등록할 슬롯 번호. 1부터 시작 - 슬롯 0은 whole-disk 예약 자리이므로 파티션은 1번부터(mmcblk0p1, p2, ...) 채워진다. */

	for (subpart = parts->subpart; subpart;	/* [한국어] 파티션 리스트를 head부터 순회 - 반복마다 slot을 1씩 증가시켜 다음 파티션 번호를 준비한다. */
	     subpart = subpart->next_subpart, slot++) {		/* [한국어] for 루프 갱신식의 연속 - subpart를 다음 노드로, slot을 다음 번호로. */
		if (subpart->from == (sector_t)(~0ULL))		/* [한국어] from이 sentinel(~0ULL, '@' 생략을 뜻함)인지 검사. */
			subpart->from = from;			/* [한국어] 직전까지 누적된 커서 값을 이 파티션의 시작 오프셋으로 확정. */
		else		/* [한국어] '@'로 명시된 오프셋이 있었던 경우의 else 분기. */
			from = subpart->from;			/* [한국어] 커서를 이 파티션이 명시한 시작 오프셋으로 이동(건너뛰기 발생 가능 - 이전 파티션과의 사이에 빈 공간이 생길 수 있음). */

		if (from >= disk_size)		/* [한국어] 커서가 이미 디스크 전체 크기 이상이면 더 넣을 공간이 없다는 뜻. */
			break;			/* [한국어] 디스크 용량을 초과했으므로 나머지 파티션 정의들은 모두 건너뛰고 루프를 종료. */

		if (subpart->size > (disk_size - from))		/* [한국어] 이 파티션의 크기가 디스크에 남은 공간(disk_size - from)보다 크면. */
			subpart->size = disk_size - from;			/* [한국어] 남은 공간 크기로 잘라(clip) 디스크 밖을 침범하지 않게 한다 - '-' 표기(남은 공간 전부)의 실제 확정도 이 경로에서 일어난다. */

		from += subpart->size;		/* [한국어] 커서를 이 파티션이 차지한 만큼 전진시켜 다음 파티션의 기본 시작점을 준비. */

		if (add_part(slot, subpart, state))		/* [한국어] 실제 등록을 add_part()에 위임 - 반환값이 참(1, 슬롯 한도 초과)이면. */
			break;			/* [한국어] 더 이상 등록할 슬롯이 없으므로 나머지 파티션들은 포기하고 루프 종료. */
	}

	return slot;	/* [한국어] 마지막으로 도달한 슬롯 번호를 반환 - 다만 앞서 설명했듯 현재 호출자는 이 값을 사용하지 않는다. */
}

/*
 * [한국어]
 * cmdline_parts_setup - 커널 부트 파라미터 "blkdevparts=..."를 받아 전역
 * 변수에 저장한다.
 *
 * @s: 커널 커맨드라인 파서가 "blkdevparts=" 뒤에 오는 부분 문자열을 가리키는
 *     포인터로 넘겨주는 값. 커널이 소유/관리하는 정적 버퍼의 일부다.
 * @return: 1(항상) - __setup 콜백 관례상 1은 "이 파라미터를 인식하고 처리했다"는
 *          뜻이다(값 자체의 유효성 검증은 아직 하지 않는다).
 *
 * 이 함수가 왜 필요한가: 커널 커맨드라인 파싱은 스케줄러/블록 계층 등
 * 대부분의 서브시스템이 아직 초기화되기 전인 아주 이른 부팅 단계에서
 * 일어난다. 이 시점에는 파티션 스캔에 필요한 자료구조(parsed_partitions
 * 등)가 존재하지 않으므로, 여기서는 문자열 포인터만 저장해 두고 실제
 * 파싱(cmdline_parts_parse)은 훨씬 나중, 디스크가 실제로 프로브될 때로
 * 미룬다(지연 파싱).
 * 실행 컨텍스트: __init 구간 - 초기 부팅 스레드에서 한 번만 실행되고, 이후
 * 이 함수의 코드 자체가 메모리에서 회수(discard)된다.
 * 호출자: 커널 커맨드라인 파서(do_early_param 등)가 __setup 매크로가 등록한
 * 테이블을 통해 "blkdevparts=" 프리픽스를 만나면 호출.
 * 피호출자: 없음(단순 대입).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   (커널 커맨드라인 파서) -> [이 함수]
 */
static int __init cmdline_parts_setup(char *s)
{
	cmdline = s;	/* [한국어] 넘겨받은 포인터를 그대로 전역 변수 cmdline에 저장 - 별도 복사 없이 포인터만 보관하며, 실제 파싱은 이 문자열이 가리키는 메모리가 여전히 유효한 이후 시점(cmdline_partition 최초 호출)에 이루어진다. */
	return 1;	/* [한국어] __setup 콜백 규약에 따라 "처리했음"을 뜻하는 1을 반환. */
}
__setup("blkdevparts=", cmdline_parts_setup);
/* [한국어] __setup 매크로: 커널 커맨드라인에서 "blkdevparts=" 프리픽스를
 * 만나면 cmdline_parts_setup()을 호출하도록 부트 파라미터 테이블에
 * 등록한다(링커 섹션 기반 정적 등록, 함수 호출로 등록하는 것이 아님). */

/*
 * [한국어]
 * has_overlaps - 두 개의 반열림 구간 [from, from+size)와 [from2, from2+size2)가
 * 겹치는지 검사한다.
 *
 * @from:  첫 번째 구간의 시작(섹터 단위 - add_part 이후 값이므로 이미 섹터
 *         단위로 변환돼 있다).
 * @size:  첫 번째 구간의 길이.
 * @from2: 두 번째 구간의 시작.
 * @size2: 두 번째 구간의 길이.
 * @return: 두 구간이 한 섹터라도 공유하면 true, 완전히 분리돼 있으면 false.
 *
 * 이 함수가 왜 필요한가: 사용자가 blkdevparts= 커맨드라인을 잘못 입력해
 * 두 파티션이 겹치는 오프셋/크기로 설정될 수 있다(예: 오타로 offset을
 * 잘못 계산). 겹친 파티션에 서로 다른 파일시스템이 동시에 쓰기를 하면
 * 데이터가 상호 훼손될 수 있으므로, 이를 사전에 검출해 경고하기 위한
 * 순수 산술 판정 함수다.
 * 동작: 두 구간의 끝(end, end2)을 계산한 뒤, "한 구간의 시작 또는 끝이
 * 다른 구간 내부에 들어있는지"를 네 가지 경우로 모두 검사한다(대칭적으로
 * 중복되게 짜여 있어 포함 관계까지 포함해 모든 겹침 패턴을 놓치지 않는다).
 * 실행 컨텍스트: 호출자와 동일한 디스크 파티션 스캔 단일 스레드. 순수 함수라
 * 전역 상태를 건드리지 않으므로 재진입에도 안전하다.
 * 호출자: cmdline_parts_verifier()가 등록된 파티션 쌍마다 호출.
 * 피호출자: 없음(산술 비교만 수행).
 * 에러 경로: 없음(항상 bool 값을 반환).
 *
 * 호출 체인:
 *   cmdline_parts_verifier -> [이 함수]
 */
static bool has_overlaps(sector_t from, sector_t size,
			 sector_t from2, sector_t size2)
{
	sector_t end = from + size;	/* [한국어] 첫 번째 구간의 끝(배타적 상한, exclusive end) - 이 위치 자체는 구간에 포함되지 않는다. */
	sector_t end2 = from2 + size2;	/* [한국어] 두 번째 구간의 끝(배타적 상한). */

	if (from >= from2 && from < end2)	/* [한국어] 케이스 1: 첫 구간의 시작(from)이 두 번째 구간 [from2, end2) 내부에 있는 경우. */
		return true;		/* [한국어] 겹침 확정 - true 반환. */

	if (end > from2 && end <= end2)	/* [한국어] 케이스 2: 첫 구간의 끝(end)이 두 번째 구간 (from2, end2] 범위(시작 초과, 끝 이하)에 있는 경우 - 첫 구간이 두 번째 구간의 뒷부분과 겹치는 패턴. */
		return true;		/* [한국어] 겹침 확정. */

	if (from2 >= from && from2 < end)	/* [한국어] 케이스 3: 두 번째 구간의 시작(from2)이 첫 구간 [from, end) 내부에 있는 경우 - 케이스 1의 대칭. */
		return true;		/* [한국어] 겹침 확정. */

	if (end2 > from && end2 <= end)	/* [한국어] 케이스 4: 두 번째 구간의 끝(end2)이 첫 구간 (from, end] 범위에 있는 경우 - 케이스 2의 대칭. */
		return true;		/* [한국어] 겹침 확정. */

	return false;	/* [한국어] 네 경우 모두 해당하지 않으면 두 구간은 완전히 분리돼 있음 - false 반환. */
}

/*
 * [한국어]
 * overlaps_warns_header - 파티션 중첩 경고 메시지의 머리말 두 줄을 출력한다.
 *
 * (파라미터 없음)
 * @return: 없음(void).
 *
 * 이 함수가 왜 필요한가: cmdline_parts_verifier()는 겹치는 파티션 쌍마다
 * 상세 경고를 한 줄씩 찍는데, 그 앞에 "겹치는 파티션이 있다"는 공통 머리말을
 * 한 번만 붙이고 싶다 - 자명해 보이는 한 줄짜리 래퍼처럼 보이지만, 이렇게
 * 별도 함수로 분리해 둔 덕분에 호출부에서 "처음 겹침을 발견했을 때만" 호출
 * 여부를 간단한 bool 플래그(header) 하나로 제어할 수 있다.
 * 실행 컨텍스트: 호출자와 동일한 단일 스레드. static inline이라 컴파일러가
 * 호출부에 그대로 펼쳐 넣을 수 있다(함수 호출 오버헤드가 사실상 없음).
 * 호출자: cmdline_parts_verifier()가 최초로 겹침을 발견한 그 순간에 딱 한 번.
 * 피호출자: pr_warn().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   cmdline_parts_verifier -> [이 함수]
 */
static inline void overlaps_warns_header(void)
{
	pr_warn("Overlapping partitions are used in command line partitions.");		/* [한국어] 첫 번째 머리말 줄: 커맨드라인 파티션 설정에 겹침이 있다는 사실을 경고. */
	pr_warn("Don't use filesystems on overlapping partitions:");		/* [한국어] 두 번째 머리말 줄: 겹치는 파티션에는 파일시스템을 두지 말라고 권고(데이터 훼손 위험 안내). */
}

/*
 * [한국어]
 * cmdline_parts_verifier - 등록된 파티션들 사이에 구간 중첩이 있는지 전수
 * 비교해 커널 로그로 경고한다.
 *
 * @slot: 검사를 시작할 슬롯 번호. 유일한 호출자인 cmdline_partition()은
 *        항상 1을 넘긴다(슬롯 0은 whole-disk 자리이므로 제외).
 * @state: add_part()가 이미 채워 둔 파티션 슬롯들을 담고 있는 파티션 스캔
 *         상태.
 * @return: 없음(void) - 잘못을 고치지는 않고 커널 로그로 경고만 한다.
 *
 * 이 함수가 왜 필요한가: cmdline_parts_set()/add_part()는 겹치는 파티션이
 * 있어도 등록 자체는 그대로 진행한다(등록을 막지 않음). 잘못된 blkdevparts=
 * 설정을 사용자가 알아챌 수 있도록, 등록이 모두 끝난 뒤 슬롯들을 서로
 * 비교해 겹침이 있으면 커널 로그에 남기는 사후 검증 단계다.
 * 동작 순서: has_info가 true인(=add_part가 실제로 채운) 슬롯들만 대상으로
 * 이중 루프를 돌며 모든 (slot, i) 쌍(slot < i)을 has_overlaps()로 비교한다.
 * 겹침을 발견하면, 아직 머리말을 출력하지 않았을 때만(header 플래그)
 * overlaps_warns_header()를 한 번 호출한 뒤, 두 파티션의 이름과 범위(바이트
 * 단위로 환산해서)를 담은 상세 경고를 pr_warn()으로 출력한다.
 * 실행 컨텍스트: 호출자와 동일한 디스크 파티션 스캔 단일 스레드. 최악의 경우
 * O(limit^2)이지만 limit(DISK_MAX_PARTS)은 상수이므로 문제되지 않는다.
 * 호출자: cmdline_partition()이 cmdline_parts_set() 직후 한 번 호출.
 * 피호출자: has_overlaps(), overlaps_warns_header(), pr_warn().
 * 에러 경로: 없음(경고만 하고 계속 진행 - 겹치는 파티션도 그대로 유지된다).
 *
 * 호출 체인:
 *   cmdline_partition -> [이 함수] -> has_overlaps
 */
static void cmdline_parts_verifier(int slot, struct parsed_partitions *state)
{
	int i;	/* [한국어] 안쪽 비교 루프의 인덱스. */
	bool header = true;	/* [한국어] 머리말을 아직 출력하지 않았음을 표시하는 플래그 - true(미출력) 상태로 시작. */

	for (; slot < state->limit && state->parts[slot].has_info; slot++) {	/* [한국어] 바깥 루프: has_info가 true인 슬롯(=실제 등록된 파티션)만 골라 slot을 증가시키며 순회 - limit에 도달하거나 미등록 슬롯을 만나면 종료. */
		for (i = slot+1; i < state->limit && state->parts[i].has_info;		/* [한국어] 안쪽 루프 시작: slot보다 뒤에 있는 슬롯들과만 비교해 같은 쌍을 두 번 검사하는 것을 방지. */
		     i++) {		     /* [한국어] 안쪽 루프의 계속 조건: limit 이내이고 해당 슬롯도 has_info(등록됨)여야 함. */
			/* [한국어] 커맨드라인 파티션은 커널이 스스로 계산한 것이 아니라
			 * 사람이 손으로 적어 넣은 값이라, 오프셋 계산 실수로 서로 겹치는
			 * 구간이 나오기 쉽다. 겹친 채로 마운트하면 한쪽 쓰기가 다른 쪽을
			 * 조용히 파괴하므로, 등록은 그대로 진행하되 경고는 반드시 남긴다. */
			if (has_overlaps(state->parts[slot].from,
					 state->parts[slot].size,
					 state->parts[i].from,
					 state->parts[i].size)) {
				if (header) {					/* [한국어] 이번이 이 디스크에서 처음 발견된 겹침이면(header가 아직 true). */
					header = false;						/* [한국어] 다음부터는 머리말을 다시 찍지 않도록 플래그를 내림. */
					overlaps_warns_header();						/* [한국어] 겹침 경고 머리말 두 줄을 한 번만 출력. */
				}
				/* [한국어] 겹치는 두 파티션을 이름과 [시작,크기]로 함께
				 * 찍는다. <<9로 다시 바이트 단위로 되돌리는 것이 핵심인데,
				 * 사용자가 blkdevparts=에 적은 것과 같은 단위로 보여 줘야
				 * 어느 항목을 고쳐야 하는지 바로 알 수 있기 때문이다.
				 * (u64) 캐스팅은 sector_t가 32비트로 설정된 커널에서도
				 * %llu와 타입을 맞추기 위한 것이다. */
				pr_warn("%s[%llu,%llu] overlaps with "
					"%s[%llu,%llu].",
					state->parts[slot].info.volname,
					(u64)state->parts[slot].from << 9,
					(u64)state->parts[slot].size << 9,
					state->parts[i].info.volname,
					(u64)state->parts[i].from << 9,
					(u64)state->parts[i].size << 9);
			}
		}
	}
}

/*
 * [한국어]
 * cmdline_partition - block layer 파티션 스캐너가 호출하는 이 파일의 유일한
 * 진입점.
 *
 * @state: 스캔 대상 디스크에 대한 parsed_partitions 상태(core.c의
 *         check_partition()이 할당해 넘겨준다). state->disk로 디스크 이름과
 *         용량에 접근한다.
 * @return: 아래 원문 주석(Purpose/Returns)이 설명하는 3가지 값 중 하나 -
 *          -1(파티션 테이블을 읽을 수 없음, 여기서는 "커맨드라인 파싱
 *          실패"라는 의미로 재사용됨), 0(내 파티션 테이블이 아님, 다른
 *          프로버가 계속 시도해야 함), 1(성공).
 *
 * 이 함수가 왜 필요한가: block/partitions/core.c의 check_part[] 배열은
 * 파티션 포맷마다 "int (*)(struct parsed_partitions *)" 시그니처의 프로버
 * 함수를 등록해 순서대로 호출한다. 이 함수가 그 배열에 들어가는, blkdevparts=
 * 방식을 대표하는 프로버다.
 * 동작 순서: (1) 전역 변수 cmdline이 non-NULL이면(최초 호출 또는 아직
 * 소비 전) bdev_parts를 비우고 cmdline 전체를 다시 파싱, 소비했다는 표시로
 * cmdline을 NULL로 되돌림 -> (2) bdev_parts가 여전히 없으면(blkdevparts=
 * 자체가 없었거나 파싱 결과가 비어 있으면) 0 반환 -> (3) 현재 디스크 이름에
 * 해당하는 항목을 cmdline_parts_find()로 검색, 없으면 0 반환 -> (4)
 * get_capacity()로 디스크 실제 용량(바이트)을 구함 -> (5)
 * cmdline_parts_set()으로 실제 등록, cmdline_parts_verifier()로 겹침 검사 ->
 * (6) 로그 버퍼를 개행으로 마무리하고 1 반환.
 * 실행 컨텍스트: 디스크 프로브/파티션 (재)스캔 경로의 프로세스 컨텍스트.
 * 디스크마다 한 번씩 호출되지만, 전역 파싱(1단계)은 cmdline이 NULL로
 * 바뀐 뒤부터는 실행되지 않으므로 사실상 첫 번째로 스캔되는 디스크에서만
 * 무거운 파싱 비용이 발생한다.
 * 호출자: block/partitions/core.c의 check_partition() - check_part[] 테이블을
 * 통한 함수 포인터 호출.
 * 피호출자: cmdline_parts_free(), cmdline_parts_parse(), cmdline_parts_find(),
 * get_capacity(), cmdline_parts_set(), cmdline_parts_verifier(), seq_buf_puts().
 * 에러 경로: 커맨드라인 파싱 자체가 실패하면 -1을 반환해 check_partition()이
 * I/O 에러로 기록하되 다른 프로버 시도는 계속하게 한다(이 파일의 원문 주석
 * 참고). 이 디스크 대상이 아니면(못 찾으면) 0을 반환해 다른 프로버가
 * 정상적으로 이어받게 한다.
 *
 * 호출 체인:
 *   check_partition (block/partitions/core.c) -> [이 함수] ->
 *     cmdline_parts_parse -> parse_parts -> parse_subpart
 *     cmdline_parts_find
 *     cmdline_parts_set -> add_part -> put_partition
 *     cmdline_parts_verifier -> has_overlaps
 */
/*
 * Purpose: allocate cmdline partitions.
 * Returns:
 * -1 if unable to read the partition table
 *  0 if this isn't our partition table
 *  1 if successful
 */
int cmdline_partition(struct parsed_partitions *state)
{
	sector_t disk_size;	/* [한국어] 현재 디스크의 실제 전체 용량을 담을 변수(바이트 단위로 환산해 저장 예정). */
	struct cmdline_parts *parts;	/* [한국어] cmdline_parts_find()가 찾아줄, 이 디스크에 해당하는 파티션 정의 리스트. */

	if (cmdline) {	/* [한국어] 전역 변수 cmdline이 non-NULL이면 - "아직 이번 커맨드라인 값을 파싱해 반영하지 않았음"을 뜻하는 분기. 통상 부팅 후 최초 호출 시 딱 한 번 참이 된다. */
		if (bdev_parts)		/* [한국어] 이전에 파싱해 둔 결과가 남아 있는지 검사(재파싱 대비 방어적 처리 - 보통 최초 호출에서는 NULL). */
			cmdline_parts_free(&bdev_parts);			/* [한국어] 남아 있다면 재구성 전에 기존 리스트부터 완전히 해제. */

		if (cmdline_parts_parse(&bdev_parts, cmdline)) {		/* [한국어] cmdline 전체 문자열을 파싱해 bdev_parts를 새로 채운다 - 실패(0이 아닌 값 반환)하면. */
			cmdline = NULL;			/* [한국어] 다시 시도해도 같은 문자열은 같은 이유로 계속 실패할 것이므로, cmdline을 NULL로 되돌려 다음 디스크 스캔부터는 이 무거운 파싱을 재시도하지 않게 한다. */
			return -1;			/* [한국어] 원문 주석의 규약대로 -1(파티션 테이블을 읽을 수 없음)을 반환. */
		}
		cmdline = NULL;		/* [한국어] 파싱 성공 시에도 마찬가지로 cmdline을 NULL로 되돌려 "이미 소비했음"을 표시 - 이후 디스크들에서는 이 if 블록 전체가 스킵된다. */
	}

	if (!bdev_parts)	/* [한국어] 파싱 결과 전역 리스트가 여전히 비어 있다면(blkdevparts= 자체가 주어지지 않았거나 파싱이 애초에 시도되지 않은 경우). */
		return 0;		/* [한국어] 이 파일이 다룰 파티션 정보가 전혀 없으므로 0(내 파티션 테이블 아님)을 반환 - core.c가 다음 프로버(msdos/efi 등)를 시도한다. */

	parts = cmdline_parts_find(bdev_parts, state->disk->disk_name);	/* [한국어] 전역 리스트에서 현재 스캔 중인 디스크 이름(state->disk->disk_name)과 일치하는 항목을 검색. */
	if (!parts)	/* [한국어] 이 디스크에 대한 blkdevparts= 정의가 없다면(parts가 NULL). */
		return 0;		/* [한국어] 마찬가지로 0을 반환 - 이 디스크는 다른 파티션 포맷(또는 파티션 없음)으로 처리돼야 함. */

	disk_size = get_capacity(state->disk) << 9;	/* [한국어] get_capacity()는 512바이트 섹터 수를 반환하므로 <<9로 바이트 단위로 환산 - cmdline_subpart의 from/size와 동일한(바이트) 단위로 맞추기 위함. */

	cmdline_parts_set(parts, disk_size, state);	/* [한국어] 실제 등록 단계 - 디스크 크기에 맞춰 clip하며 parsed_partitions.parts[]에 채운다(반환값은 사용하지 않음, 앞선 함수 설명 참고). */
	cmdline_parts_verifier(1, state);	/* [한국어] 등록이 끝난 파티션들 사이에 겹침이 있는지 검사해 경고 - 슬롯 1부터 시작(슬롯 0은 whole-disk). */

	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] check_partition()이 printk로 출력할 로그 버퍼(pp_buf)를 개행으로 마무리 - 지금까지 " 장치명:p1(name1) p2(name2)..." 형태로 누적돼 있었다. */

	return 1;	/* [한국어] 성공 - 이 프로버가 파티션 테이블을 제공했음을 core.c에 알린다. */
}
