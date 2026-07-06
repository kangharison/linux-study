// SPDX-License-Identifier: GPL-2.0-only
/*
 * Code for looking up block devices in the early boot code before mounting the
 * root file system.
 */
/*
 * [한국어 설명] 부팅 초기 루트 블록 장치 이름 해석 (early-lookup.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 커널 커맨드라인의 root= 인자(및 이와 유사한 초기 마운트 지정자)로
 * 주어진 "장치 이름 문자열"을 실제 블록 장치 번호인 dev_t(major:minor 조합)로
 * 변환하는 최소 기능 파서를 제공한다. 지원하는 표기법은
 * PARTUUID=<uuid>[/PARTNROFF=<n>], PARTLABEL=<label>,
 * /dev/<disk_name>[<partno>|p<partno>], 그리고 <major>:<minor> 또는
 * 16진수 <hex_major><hex_minor> 형태의 직접 지정 네 가지다. 루트 파일 시스템이
 * 아직 마운트되지 않은 시점에 실행되므로, dentry/inode 캐시나 실제 파일
 * 시스템 접근 없이 오직 driver core가 관리하는 block_class(및 그 아래
 * devtmpfs/블록 드라이버 probe가 만들어낸 gendisk/block_device 객체들)만을
 * 순회하여 이름을 매칭한다. 실패 시 진단을 돕기 위한 printk_all_partitions()도
 * 이 파일에 함께 존재한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (위 → 아래):
 *   init/do_mounts.c: name_to_dev_t()  (이 스터디 트리에는 init/ 디렉터리가
 *                                       포함되어 있지 않아 코드가 보이지 않음 — 개념상 위치만 표기)
 *     → early_lookup_bdev()                              ← 이 파일, 최상위 진입점
 *        → devt_from_partuuid()  (root=PARTUUID=...)      ← 이 파일
 *           → match_dev_by_uuid()                          ← 이 파일 (class_find_device 콜백)
 *        → devt_from_partlabel() (root=PARTLABEL=...)     ← 이 파일
 *           → match_dev_by_label()                         ← 이 파일 (class_find_device 콜백)
 *        → devt_from_devname()   (root=/dev/...)          ← 이 파일
 *           → blk_lookup_devt()                            ← 이 파일
 *        → devt_from_devnum()    (그 외 major:minor/hex 표기) ← 이 파일
 *   실패 시:
 *     루트를 마운트하지 못한 경우 호출자 쪽에서 printk_all_partitions()를 호출해
 *     block_class에 등록된 모든 파티션을 콘솔에 나열해 사용자가 원인을 추정하도록 돕는다.
 *
 * initcall 순서상의 위치: 이 파일의 조회 함수들은 모두 __init 섹션에 위치하며,
 * 커널 부팅 중 "루트 파일 시스템을 마운트하기 직전" 단 한 번만 호출되고,
 * 부팅이 끝나 free_initmem()이 실행되면 코드 자체가 메모리에서 회수된다.
 * printk_all_partitions()만 예외적으로 __init이 아닌데, 이는 부팅 이후에도
 * (예: 나중에 루트 재탐색을 시도하는 드문 경로에서) 참조될 가능성을 열어 둔
 * 것으로 보인다(추정). 이 시점에는 아직 VFS 마운트 테이블이 구성되지 않았고
 * 실제 파일 시스템 드라이버가 슈퍼블록을 읽을 수 없으므로, 오직 devtmpfs/driver
 * core가 이미 만들어 둔 block_class 트리(각 블록 드라이버의 probe()가
 * add_disk()/device_add_disk()를 호출한 결과 등록된 gendisk와 그 파티션들)만
 * 신뢰할 수 있는 정보원이다. 실행 컨텍스트는 부팅 태스크(초기 유일 스레드)
 * 컨텍스트이며, 이 시점에는 SMP secondary CPU도 다른 커널 스레드도 아직
 * 이 코드와 경쟁하지 않으므로 락 없이 동작한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - include/linux/blkdev.h : struct block_device, block_class, dev_to_bdev()/dev_to_disk(),
 *                              bdev_partno(), part_devt(), early_lookup_bdev() 선언
 *   - include/linux/ctype.h  : isdigit() — devt_from_devname()의 파티션 번호 파싱에 사용
 *   - drivers/base/core.c    : class_find_device(), class_dev_iter_init()/_next()/_exit()
 *                              — driver core가 관리하는 block_class 아래 device 트리 순회
 *   - block/genhd.c          : gendisk 등록/해제(add_disk 등)로 block_class에 디바이스가
 *                              실제로 나타나게 만드는 쪽 — 이 파일은 그 결과만 "조회"할 뿐이다
 *   - block/partitions/*     : 파티션 스캔 결과로 bd_meta_info(uuid/volname)가 채워짐 —
 *                              devt_from_partuuid()/devt_from_partlabel()이 이를 비교
 *
 * 이 파일에 의존하는 모듈:
 *   - init/do_mounts.c (이 트리에는 없음) : name_to_dev_t()가 early_lookup_bdev()를 호출해
 *                              root= 커맨드라인 인자를 dev_t로 해석한다
 *
 * 데이터 흐름:
 *   커널 커맨드라인 문자열(root=...) → early_lookup_bdev()가 접두어로 형식 판별
 *   → block_class 트리 순회(class_find_device()/class_dev_iter_*()) → 일치하는
 *   struct device 발견 → dev_to_bdev()/dev->devt 또는 part_devt()로 dev_t 추출
 *   → 호출자(name_to_dev_t)가 이 dev_t로 블록 장치를 열어 루트로 마운트한다.
 *
 * 이 파일이 처리하지 "않는" 것 (경계 명확화):
 *   root=UUID=<fs-uuid> (파티션 UUID가 아니라 파일 시스템 UUID) 형식은 이 파일이
 *   아니라 호출자인 init/do_mounts.c 쪽의 별도 로직(모든 블록 장치를 순회하며
 *   슈퍼블록의 파일 시스템 UUID를 비교)에서 처리된다(추정 — 이 트리에는 해당
 *   코드가 없어 직접 확인은 못했다). 이 파일은 PARTUUID=(파티션 테이블 자체의 UUID),
 *   PARTLABEL=, /dev/이름, major:minor/hex 네 가지 "장치/파티션" 단위 표기만 다룬다.
 *
 * 공유 핵심 자료구조:
 *   struct uuidcmp      : PARTUUID= 문자열 비교를 위한 임시 컨텍스트 (이 파일 로컬 정의)
 *   struct device       : driver core의 범용 디바이스 객체 — block_class 아래 등록된 것들
 *   struct block_device : 커널의 블록 디바이스 핸들 — dev_to_bdev()로 struct device에서 획득
 *   struct gendisk      : 디스크 단위 객체(파티션 테이블 포함) — dev_to_disk()로 획득
 *
 * === 주요 함수/구조체 요약 ===
 * early_lookup_bdev()    : root= 문자열의 접두어를 보고 4가지 하위 파서 중 하나로 분기하는 최상위 진입점
 * devt_from_partuuid()   : "PARTUUID=<uuid>[/PARTNROFF=<n>]" 파싱 — GPT/MBR 파티션 UUID로 검색
 * devt_from_partlabel()  : "PARTLABEL=<label>" 파싱 — GPT 파티션 레이블로 검색
 * devt_from_devname()    : "/dev/<name>[<n>|p<n>]" 파싱 — 디스크 이름 + 파티션 번호로 검색
 * devt_from_devnum()     : "<major>:<minor>" 또는 16진수 문자열을 직접 dev_t로 변환
 * blk_lookup_devt()      : block_class를 순회하며 이름이 일치하는 gendisk를 찾아 dev_t 계산
 * match_dev_by_uuid()/match_dev_by_label() : class_find_device()에 전달되는 비교 콜백
 * printk_all_partitions(): 루트 마운트 실패 시 등록된 모든 파티션을 콘솔에 진단 출력
 * struct uuidcmp         : uuid 포인터 + 길이 — PARTNROFF 접미어를 잘라낸 "순수 UUID 부분"만 비교하기 위한 임시 구조체
 */
#include <linux/blkdev.h>	/* [한국어] block_class, struct block_device/gendisk, dev_to_bdev()/dev_to_disk(), part_devt() 등 이 파일 전역에서 쓰는 블록 계층 핵심 선언 */
#include <linux/ctype.h>	/* [한국어] isdigit() — devt_from_devname()에서 "diskNAMEp3" 형식의 말단 숫자를 파티션 번호로 인식할 때 사용 */

/*
 * [한국어]
 * struct uuidcmp - PARTUUID= 커맨드라인 인자를 파싱한 결과를 담는 비교 컨텍스트
 *
 * devt_from_partuuid()가 "PARTUUID=<uuid>[/PARTNROFF=<n>]" 문자열에서
 * PARTNROFF= 접미어를 제외한 순수 UUID 부분만 잘라내 이 구조체에 채우고,
 * class_find_device()의 match 콜백인 match_dev_by_uuid()에 opaque data
 * 포인터로 전달한다. match_dev_by_uuid()는 매 device마다 이 구조체의
 * uuid/len을 bdev->bd_meta_info->uuid와 strncasecmp()로 비교한다.
 * devt_from_partuuid()의 스택 프레임에 임시로 생성되어 그 호출이 끝나면
 * 즉시 소멸하는 수명이 짧은(one-shot) 헬퍼 구조체이며, 별도의 동기화가
 * 필요 없다 (단일 부팅 스레드에서만 생성·사용·폐기됨).
 */
struct uuidcmp {
	const char *uuid;
	/* [한국어] 비교 대상이 되는 UUID 문자열의 시작 포인터.
	 * 설정자: devt_from_partuuid()가 커널 커맨드라인 인자 uuid_str을
	 *         그대로(별도 복사 없이) 가리키도록 설정한다 — cmp.uuid = uuid_str.
	 * 읽는 자: match_dev_by_uuid()가 strncasecmp(cmp->uuid, bdev->bd_meta_info->uuid, cmp->len)
	 *         호출 시 첫 번째 인자로 읽는다.
	 * 값 범위: NUL로 끝나지 않을 수도 있는 부분 문자열(예: "UUID/PARTNROFF=2"에서
	 *         '/' 앞부분만 유효 구간) — 반드시 len과 함께 사용해야 하며 단독으로
	 *         strlen() 등을 호출하면 PARTNROFF= 뒷부분까지 포함되어 버그가 된다.
	 * 동기화: 단일 부팅 스레드에서만 생성·참조되므로 락 불필요. */

	int len;
	/* [한국어] uuid 필드에서 실제로 비교에 사용할 바이트 길이.
	 * 설정자: devt_from_partuuid()가 "/PARTNROFF=" 접미어가 있으면
	 *         슬래시 이전까지의 길이(slash - uuid_str)로, 없으면
	 *         strlen(uuid_str) 전체 길이로 설정한다.
	 * 읽는 자: match_dev_by_uuid()가 strncasecmp()의 세 번째 인자로 읽어
	 *         정확히 이 길이만큼만 대소문자 무시 비교를 수행한다.
	 * 값 범위: 0이면 devt_from_partuuid()가 out_invalid로 분기해 이 구조체를
	 *         아예 사용하지 않으므로, match_dev_by_uuid()에 도달하는 시점에는
	 *         항상 1 이상. 음수 불가(strlen 또는 포인터 차이의 결과이므로).
	 * 동기화: 단일 부팅 스레드에서만 생성·참조되므로 락 불필요. */
};

/**
 * match_dev_by_uuid - callback for finding a partition using its uuid
 * @dev:	device passed in by the caller
 * @data:	opaque pointer to the desired struct uuidcmp to match
 *
 * Returns 1 if the device matches, and 0 otherwise.
 */
/*
 * [한국어]
 * match_dev_by_uuid - PARTUUID 값으로 파티션을 찾기 위한 class_find_device() 콜백
 *
 * @dev:  block_class를 순회하며 driver core가 넘겨주는 후보 device.
 *        class_find_device()가 내부적으로 klist_iter를 돌며 이 콜백에
 *        후보를 하나씩 전달한다.
 * @data: devt_from_partuuid()가 스택에 만든 struct uuidcmp*를 그대로 전달한
 *        opaque 포인터. match 콜백 시그니처가 const void *이므로 여기서
 *        struct uuidcmp*로 캐스팅해 사용한다.
 * @return: 1이면 class_find_device()가 즉시 순회를 멈추고 이 dev의 참조 카운트를
 *          올려 반환한다(내부적으로 get_device() 처리). 0이면 다음 후보로 계속 순회.
 *
 * class_find_device()는 block_class 아래의 device 목록을 순회하며 이
 * match 콜백이 1을 반환하는 첫 device를 찾으면 즉시 멈춘다. 이 함수는
 * bdev->bd_meta_info(파티션 스캔 코드가 GPT/MBR을 읽으며 채워 둔 UUID/레이블
 * 메타데이터)가 없는 장치는 매칭 대상에서 제외하고, 있으면 대소문자를
 * 구분하지 않고 cmp->len 바이트만 비교한다(strncasecmp) — PARTNROFF= 접미어가
 * 잘려 나간 상태이므로 정확히 UUID 부분만 비교하기 위함이다.
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션 — 재진입/동시성 고려 불필요.
 * 호출자: devt_from_partuuid()가 class_find_device()의 네 번째 인자로 전달.
 * 호출 대상: strncasecmp() 외에는 없음(리프 함수).
 * 에러 경로: 별도 에러 코드가 없고 단순 불리언(0/1) 판정만 수행 — 실패 시
 *          호출자인 class_find_device()가 다음 device로 넘어갈 뿐이다.
 *
 * 호출 체인:
 *   devt_from_partuuid() → class_find_device() → [match_dev_by_uuid()]
 */
static int __init match_dev_by_uuid(struct device *dev, const void *data)
{
	struct block_device *bdev = dev_to_bdev(dev);	// [한국어] struct device를 감싸는 struct block_device로 변환 - bd_meta_info(UUID/레이블) 필드 접근을 위해 필요
	const struct uuidcmp *cmp = data;	// [한국어] opaque data 포인터를 실제 타입인 struct uuidcmp*로 캐스팅 - devt_from_partuuid()가 넘긴 비교 컨텍스트 복원

	if (!bdev->bd_meta_info ||	// [한국어] 파티션 메타데이터(uuid/volname)가 아예 없는 장치(예: 파티션 스캔에서 UUID를 얻지 못한 디스크)면 매칭 불가로 판정
	    strncasecmp(cmp->uuid, bdev->bd_meta_info->uuid, cmp->len))	// [한국어] cmp->len 바이트만 대소문자 무시 비교 - PARTNROFF= 접미어가 잘린 순수 UUID 구간만 비교하기 위함
		return 0;	// [한국어] 메타데이터 없음 또는 UUID 불일치 - class_find_device()가 다음 후보로 계속 순회하도록 0 반환
	return 1;	// [한국어] UUID 일치 - class_find_device()가 이 dev를 찾은 것으로 간주하고 순회 종료 + 참조 카운트 증가
}

/**
 * devt_from_partuuid - looks up the dev_t of a partition by its UUID
 * @uuid_str:	char array containing ascii UUID
 * @devt:	dev_t result
 *
 * The function will return the first partition which contains a matching
 * UUID value in its partition_meta_info struct.  This does not search
 * by filesystem UUIDs.
 *
 * If @uuid_str is followed by a "/PARTNROFF=%d", then the number will be
 * extracted and used as an offset from the partition identified by the UUID.
 *
 * Returns 0 on success or a negative error code on failure.
 */
/*
 * [한국어]
 * devt_from_partuuid - "PARTUUID=<uuid>[/PARTNROFF=<n>]" 문자열로 dev_t를 조회
 *
 * @uuid_str: early_lookup_bdev()가 "PARTUUID=" 접두어(9글자)를 잘라낸 나머지
 *            문자열. "/PARTNROFF=<n>"이 뒤에 붙어 있을 수 있다.
 * @devt:     성공 시 결과가 채워질 출력 버퍼(호출자 스택의 dev_t 변수 주소).
 * @return:   0이면 성공(*devt에 유효한 값이 채워짐). -EINVAL이면 PARTNROFF=
 *            구문이 잘못됐거나 UUID 부분이 비어 있는 경우. -ENODEV면 UUID가
 *            일치하는 파티션을 block_class에서 찾지 못한 경우.
 *
 * PARTUUID 하나로 여러 파티션을 상대적으로 지정하고 싶을 때(예: 동일 UUID를
 * 여러 디스크에 복제한 뒤 오프셋으로 구분) "/PARTNROFF=<n>"을 붙여 쓸 수
 * 있는데, 이 함수는 먼저 '/' 위치를 찾아 UUID 부분과 오프셋 부분을 분리한
 * 뒤(문자열을 잘라내지는 않고 길이(cmp.len)만 조정), sscanf()로 오프셋 정수를
 * 파싱한다. 그런 다음 class_find_device()로 UUID가 일치하는 첫 파티션을 찾고,
 * 오프셋이 있으면 그 파티션 번호에 오프셋을 더한 새 파티션 번호로 dev_t를
 * 재계산한다(아직 존재하지 않는 파티션 번호일 수도 있음 — part_devt()가
 * xarray 조회 실패 시 0을 반환하는 것으로 처리됨, devt_from_devname() 참고).
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션.
 * 호출자: early_lookup_bdev()가 "PARTUUID=" 접두어를 확인한 뒤 호출.
 * 호출 대상: strchr(), sscanf(), class_find_device(), part_devt(), dev_to_disk(),
 *          bdev_partno(), dev_to_bdev(), put_device(), pr_err().
 * 에러 경로: PARTNROFF= 구문 오류나 빈 UUID는 out_invalid 레이블로 점프해
 *          pr_err()로 커널 로그에 경고를 남기고 -EINVAL 반환. 매칭 실패는
 *          -ENODEV로 즉시 반환(참조 카운트 문제 없음 — dev가 NULL이므로 put 불필요).
 *
 * 호출 체인:
 *   early_lookup_bdev() → [devt_from_partuuid()] → match_dev_by_uuid()
 */
static int __init devt_from_partuuid(const char *uuid_str, dev_t *devt)
{
	struct uuidcmp cmp;	// [한국어] match_dev_by_uuid() 콜백에 넘길 UUID 비교 컨텍스트 - 스택에 임시 생성
	struct device *dev = NULL;	// [한국어] class_find_device()가 찾아줄 결과 device 포인터 - 초기값 NULL은 "아직 못 찾음"을 의미
	int offset = 0;	// [한국어] PARTNROFF=<n> 파싱 결과를 담을 변수 - 기본값 0은 "오프셋 없음"을 의미
	char *slash;	// [한국어] uuid_str 안에서 "/PARTNROFF=" 구분자를 가리킬 포인터

	cmp.uuid = uuid_str;	// [한국어] 비교 대상 UUID 문자열의 시작 지점을 uuid_str 그대로 지정 (복사하지 않음)

	slash = strchr(uuid_str, '/');	// [한국어] "/PARTNROFF=" 접미어가 있는지 '/' 문자를 찾아 확인
	/* Check for optional partition number offset attributes. */
	if (slash) {	// [한국어] '/'가 있으면 PARTNROFF= 오프셋 표기가 붙어 있다고 보고 파싱 시도
		char c = 0;	// [한국어] sscanf()로 "%d%c" 매칭 시 숫자 뒤에 남는 잉여 문자를 검출하기 위한 보초(sentinel) 변수

		/* Explicitly fail on poor PARTUUID syntax. */
		if (sscanf(slash + 1, "PARTNROFF=%d%c", &offset, &c) != 1)	// [한국어] "PARTNROFF=" 뒤에 정수 하나만 있어야 매칭 개수 1 - %c까지 매칭되면(잉여 문자 존재) 구문 오류로 간주
			goto out_invalid;	// [한국어] 잘못된 PARTNROFF= 구문 - 에러 로그를 남기고 -EINVAL로 반환하는 공통 경로로 점프
		cmp.len = slash - uuid_str;	// [한국어] '/' 이전까지의 바이트 수만 UUID 길이로 설정 - PARTNROFF= 부분은 비교에서 제외
	} else {
		cmp.len = strlen(uuid_str);	// [한국어] '/'가 없으면 문자열 전체가 UUID이므로 strlen()으로 전체 길이 사용
	}

	if (!cmp.len)	// [한국어] UUID 길이가 0이면(예: "PARTUUID=" 뒤에 아무것도 없는 경우) 비교할 대상이 없으므로 오류 처리
		goto out_invalid;	// [한국어] 빈 UUID - 공통 에러 처리 경로로 점프

	dev = class_find_device(&block_class, NULL, &cmp, &match_dev_by_uuid);	// [한국어] block_class에 등록된 모든 device 중 match_dev_by_uuid() 콜백이 1을 반환하는 첫 device 검색 (성공 시 참조 카운트 증가된 채로 반환)
	if (!dev)	// [한국어] 일치하는 UUID를 가진 파티션이 하나도 없으면
		return -ENODEV;	// [한국어] "해당 장치 없음"을 의미하는 표준 에러코드 반환

	if (offset) {	// [한국어] PARTNROFF= 오프셋이 지정되어 있으면 UUID로 찾은 파티션 번호에서 상대 이동
		/*
		 * Attempt to find the requested partition by adding an offset
		 * to the partition number found by UUID.
		 */
		*devt = part_devt(dev_to_disk(dev),	// [한국어] UUID로 찾은 dev가 속한 gendisk(디스크 전체)를 얻고
				  bdev_partno(dev_to_bdev(dev)) + offset);	// [한국어] 그 dev의 원래 파티션 번호에 offset을 더한 번호로 dev_t를 재계산 - 아직 존재하지 않는 파티션 번호일 수도 있음
	} else {
		*devt = dev->devt;	// [한국어] 오프셋이 없으면 UUID로 직접 찾은 device의 dev_t를 그대로 사용
	}

	put_device(dev);	// [한국어] class_find_device()가 올려둔 참조 카운트를 반환 - dev 자체를 보관하지 않으므로 사용 후 즉시 해제
	return 0;	// [한국어] 성공 - *devt에 유효한 결과가 채워진 상태로 반환

out_invalid:
	pr_err("VFS: PARTUUID= is invalid.\n"	// [한국어] 커널 로그(dmesg)에 사용자가 알아볼 수 있는 진단 메시지 출력 - 부팅 실패 원인 파악을 돕기 위함
	       "Expected PARTUUID=<valid-uuid-id>[/PARTNROFF=%%d]\n");	// [한국어] 올바른 문법 예시를 함께 출력 - %%d는 printf 계열 포맷에서 리터럴 '%d'를 출력하기 위한 이스케이프
	return -EINVAL;	// [한국어] 잘못된 인자 형식임을 알리는 표준 에러코드 반환
}

/**
 * match_dev_by_label - callback for finding a partition using its label
 * @dev:	device passed in by the caller
 * @data:	opaque pointer to the label to match
 *
 * Returns 1 if the device matches, and 0 otherwise.
 */
/*
 * [한국어]
 * match_dev_by_label - PARTLABEL 값으로 파티션을 찾기 위한 class_find_device() 콜백
 *
 * @dev:  block_class를 순회하며 driver core가 넘겨주는 후보 device.
 * @data: devt_from_partlabel()이 전달한 레이블 문자열(const char *)을
 *        opaque 포인터로 받은 것 — match_dev_by_uuid()와 달리 별도 구조체
 *        없이 문자열 포인터 자체를 그대로 data로 사용한다(레이블은 길이
 *        비교가 필요 없는 NUL 종단 문자열이라 strcmp()로 충분하기 때문).
 * @return: 1이면 레이블이 정확히 일치, 0이면 불일치 또는 메타데이터 없음.
 *
 * match_dev_by_uuid()와 거의 동일한 구조이지만, 부분 문자열 비교
 * (strncasecmp + 길이)가 아니라 완전 일치 비교(strcmp)를 사용한다는 점이
 * 다르다 — PARTLABEL은 PARTNROFF= 같은 접미어 문법이 없기 때문이다.
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션.
 * 호출자: devt_from_partlabel()이 class_find_device()의 네 번째 인자로 전달.
 * 호출 대상: strcmp() 외에는 없음(리프 함수).
 *
 * 호출 체인:
 *   devt_from_partlabel() → class_find_device() → [match_dev_by_label()]
 */
static int __init match_dev_by_label(struct device *dev, const void *data)
{
	struct block_device *bdev = dev_to_bdev(dev);	// [한국어] struct device를 struct block_device로 변환 - bd_meta_info->volname(파티션 레이블) 접근을 위해 필요
	const char *label = data;	// [한국어] opaque data 포인터를 문자열 포인터로 캐스팅 - devt_from_partlabel()이 넘긴 PARTLABEL= 값

	if (!bdev->bd_meta_info || strcmp(label, bdev->bd_meta_info->volname))	// [한국어] 메타데이터가 없거나(GPT가 아니어서 레이블 개념이 없는 경우 등) 레이블 문자열이 정확히 일치하지 않으면
		return 0;	// [한국어] 불일치 - class_find_device()가 다음 후보로 계속 순회
	return 1;	// [한국어] 레이블 완전 일치 - class_find_device()가 이 dev를 찾은 것으로 간주하고 순회 종료
}

/*
 * [한국어]
 * devt_from_partlabel - "PARTLABEL=<label>" 문자열로 dev_t를 조회
 *
 * @label: early_lookup_bdev()가 "PARTLABEL=" 접두어(10글자)를 잘라낸 나머지
 *         문자열 — GPT 파티션 엔트리의 PartitionName(UTF-16LE를 커널 내부에서
 *         변환한 문자열)과 비교할 값.
 * @devt:  성공 시 결과가 채워질 출력 버퍼.
 * @return: 0이면 성공. -ENODEV면 레이블이 일치하는 파티션을 찾지 못한 경우.
 *          devt_from_partuuid()와 달리 이 함수는 잘못된 입력 형식에 대한
 *          별도 검증(-EINVAL 경로)이 없다 — 레이블은 임의의 문자열이 모두
 *          유효하기 때문이다.
 *
 * devt_from_partuuid()보다 훨씬 단순한 버전으로, PARTNROFF= 같은 상대
 * 오프셋 개념이 없어 class_find_device()로 찾은 device의 dev_t를 그대로
 * 반환한다.
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션.
 * 호출자: early_lookup_bdev()가 "PARTLABEL=" 접두어를 확인한 뒤 호출.
 * 호출 대상: class_find_device(), put_device().
 * 에러 경로: 매칭 실패 시 -ENODEV 즉시 반환 (dev가 NULL이므로 put_device() 불필요).
 *
 * 호출 체인:
 *   early_lookup_bdev() → [devt_from_partlabel()] → match_dev_by_label()
 */
static int __init devt_from_partlabel(const char *label, dev_t *devt)
{
	struct device *dev;	// [한국어] class_find_device()가 찾아줄 결과 device 포인터

	dev = class_find_device(&block_class, NULL, label, &match_dev_by_label);	// [한국어] block_class에서 match_dev_by_label() 콜백이 1을 반환하는 첫 device 검색 (성공 시 참조 카운트 증가된 채로 반환)
	if (!dev)	// [한국어] 일치하는 레이블을 가진 파티션이 없으면
		return -ENODEV;	// [한국어] "해당 장치 없음" 에러코드 반환
	*devt = dev->devt;	// [한국어] 찾은 device의 dev_t를 출력 버퍼에 저장
	put_device(dev);	// [한국어] class_find_device()가 올려둔 참조 카운트 반환
	return 0;	// [한국어] 성공
}

/*
 * [한국어]
 * blk_lookup_devt - 디스크 이름(및 선택적 파티션 번호)으로 block_class를 순회해 dev_t를 계산
 *
 * @name:   찾고자 하는 gendisk의 이름과 정확히 일치해야 하는 문자열
 *          (dev_name()이 반환하는 이름, 예: "sda", "nvme0n1"). devt_from_devname()이
 *          경로 구분자 '/'를 '!'로 치환해 넘기므로 슬래시가 포함된 디스크
 *          이름도 이 함수 안에서는 항상 정규화된 형태로 비교된다.
 * @partno: 찾고자 하는 파티션 번호. 0이면 디스크 전체(파티션 0 = whole
 *          disk)를 의미한다.
 * @return: 일치하는 항목을 찾으면 그 dev_t, 못 찾으면 MKDEV(0, 0)
 *          (major=0, minor=0 — 유효하지 않은 장치 번호).
 *
 * class_dev_iter_init/_next/_exit로 block_class 아래의 disk_type 장치들만
 * 순회하면서 이름이 일치하는 gendisk를 찾는다. 이름이 일치해도 두 가지
 * 경우로 나뉘는데, partno가 disk->minors(이 디스크가 가질 수 있는 minor
 * 번호 개수, 즉 파티션 슬롯 수) 범위 안이면 아직 파티션 테이블에 실제로
 * 등록되지 않은 파티션이라도 minor 번호를 산술적으로 계산해 반환한다(예:
 * md 등 나중에 파티션이 나타날 수 있는 디바이스 대응). 범위를 벗어나면
 * part_devt()로 실제 xarray(part_tbl)에서 해당 파티션이 등록되어 있는지
 * 조회하고, 있으면 그 dev_t를 사용하고 루프를 즉시 종료한다.
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션.
 * 호출자: devt_from_devname()이 디스크 이름 추정 시도마다 호출(최대 3회).
 * 호출 대상: class_dev_iter_init/_next/_exit(), dev_to_disk(), strcmp(),
 *          part_devt().
 * 에러 경로: 별도 에러코드 없이 못 찾으면 MKDEV(0,0)을 반환 — 호출자가
 *          "*devt가 0이면 실패"로 해석한다(devt_from_devname() 참고).
 *
 * 호출 체인:
 *   devt_from_devname() → [blk_lookup_devt()]
 */
static dev_t __init blk_lookup_devt(const char *name, int partno)
{
	dev_t devt = MKDEV(0, 0);	// [한국어] "찾지 못함"을 의미하는 기본값(major=0, minor=0)으로 초기화
	struct class_dev_iter iter;	// [한국어] block_class 순회 상태를 담는 반복자 - class_dev_iter_init()으로 초기화 필요
	struct device *dev;	// [한국어] 순회 중 현재 후보 device를 가리킬 포인터

	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);	// [한국어] block_class 아래 disk_type(디스크 전체를 나타내는 device_type)에 해당하는 device들만 순회하도록 반복자 초기화
	while ((dev = class_dev_iter_next(&iter))) {	// [한국어] 다음 후보 device를 하나씩 가져옴 - NULL이면 순회 종료
		struct gendisk *disk = dev_to_disk(dev);	// [한국어] 현재 device를 감싸는 gendisk(디스크 객체)로 변환 - minors/part_tbl 접근을 위해 필요

		if (strcmp(dev_name(dev), name))	// [한국어] 이 device의 이름이 찾는 이름과 다르면
			continue;	// [한국어] 다음 후보로 건너뜀

		if (partno < disk->minors) {	// [한국어] 요청한 파티션 번호가 이 디스크의 minor 번호 할당 범위 안이면 - 아직 실제로 존재하지 않는 파티션이라도 번호 계산이 가능
			/* We need to return the right devno, even
			 * if the partition doesn't exist yet.
			 */
			devt = MKDEV(MAJOR(dev->devt),	// [한국어] 디스크 전체의 major 번호는 그대로 사용
				     MINOR(dev->devt) + partno);	// [한국어] 디스크의 base minor에 파티션 번호를 더해 목표 파티션의 minor 계산 (아직 파티션 테이블에 없어도 산술적으로 유효)
		} else {
			devt = part_devt(disk, partno);	// [한국어] minor 범위를 벗어난 파티션 번호는 실제 파티션 테이블(xarray)에서 존재 여부를 조회해야 함
			if (devt)	// [한국어] 실제로 등록된 파티션을 찾았으면
				break;	// [한국어] 더 순회할 필요 없이 즉시 루프 종료 (devt에 결과가 채워진 상태)
		}
	}
	class_dev_iter_exit(&iter);	// [한국어] 반복자가 잡고 있던 리소스(klist 위치 등) 해제
	return devt;	// [한국어] 찾은 dev_t 또는 못 찾았을 때의 MKDEV(0,0)을 반환
}

/*
 * [한국어]
 * devt_from_devname - "/dev/" 접두어를 제거한 전통적 장치 경로 이름을 dev_t로 변환
 *
 * @name: early_lookup_bdev()가 "/dev/" 접두어(5글자)를 잘라낸 나머지 문자열
 *        (예: "nvme0n1p2", "sda1", "md/0"). 31바이트를 넘는 이름은 허용하지
 *        않는다(고정 크기 스택 버퍼 s[32] 제약, 아래 참고).
 * @devt: 성공 시 결과가 채워질 출력 버퍼.
 * @return: 0이면 성공. -EINVAL이면 이름이 31바이트를 초과. -ENODEV면
 *          디스크 이름 자체를 찾지 못했거나, 말단 숫자를 파티션 번호로
 *          해석해도 일치하는 파티션이 없는 경우.
 *
 * 이 함수는 이름을 3단계로 시도한다.
 * 1) 이름 전체를 디스크 이름으로 보고(파티션 번호 0, 즉 whole-disk)
 *    blk_lookup_devt()를 호출 — "/dev/sda"처럼 파티션이 없는 디스크 자체를
 *    가리키는 경우 이 단계에서 바로 성공한다.
 * 2) 실패하면 이름 끝에서부터 숫자를 모두 잘라내(예: "sda1" → "sda" + 1)
 *    그 앞부분을 디스크 이름, 숫자를 파티션 번호로 보고 재시도 —
 *    "sda1", "nvme0n1p2"의 "2" 처리 등에 해당하지만, 이 단계만으로는
 *    "nvme0n1"의 말단 숫자 "1"이 디스크 이름의 일부인지 파티션 번호인지
 *    구분이 안 되므로 다음 단계가 필요하다.
 * 3) 그래도 실패하고, 잘라낸 위치 바로 앞 문자가 'p'이면(예: "nvme0n1p2"에서
 *    파티션 숫자 "2"를 뗀 뒤 남는 문자가 'p') 그 'p'까지 추가로 제거하고
 *    (예: "nvme0n1p2" → "nvme0n1" + 2) 다시 시도 — 디스크 이름이 숫자로
 *    끝나서(nvme0n1처럼) 파티션 번호와 구분하기 위해 관례적으로 'p'를
 *    끼워 넣는 명명 규칙(예: nvme, mmcblk, md 등)을 처리하기 위함이다.
 * 이름 안의 '/'는 미리 '!'로 치환한다 — block_class에 등록되는 kobject/
 * device 이름 규칙상 경로 구분자를 그대로 쓸 수 없기 때문이다(예: "cciss/c0d0"
 * 같은 이름이 실제로는 "cciss!c0d0"로 등록됨).
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션.
 * 호출자: early_lookup_bdev()가 "/dev/" 접두어를 확인한 뒤 호출.
 * 호출 대상: strlen(), strcpy(), blk_lookup_devt(), isdigit(), simple_strtoul().
 * 에러 경로: 이름 길이 초과는 -EINVAL로 즉시 반환. 세 단계 모두 실패하면
 *          각 단계에서 -ENODEV로 반환.
 *
 * 호출 체인:
 *   early_lookup_bdev() → [devt_from_devname()] → blk_lookup_devt()
 */
static int __init devt_from_devname(const char *name, dev_t *devt)
{
	int part;	// [한국어] 이름 끝에서 파싱해낸 파티션 번호를 저장
	char s[32];	// [한국어] name을 복사해 '/' → '!' 치환 및 말단 숫자 제거(문자열 자르기)를 안전하게 수행할 로컬 스크래치 버퍼 (원본 name은 const라 직접 수정 불가)
	char *p;	// [한국어] s 안에서 현재 순회/절단 위치를 가리키는 커서 포인터

	if (strlen(name) > 31)	// [한국어] s[32] 버퍼(NUL 포함 32바이트)에 안전하게 담을 수 있는 최대 길이(31바이트)를 초과하면
		return -EINVAL;	// [한국어] 오버플로우를 피하기 위해 즉시 잘못된 인자 에러 반환
	strcpy(s, name);	// [한국어] 위에서 길이를 이미 검증했으므로 안전하게 name을 로컬 버퍼로 복사
	for (p = s; *p; p++) {	// [한국어] s의 첫 글자부터 NUL 종단까지 한 글자씩 순회
		if (*p == '/')	// [한국어] 경로 구분자 '/'를 발견하면
			*p = '!';	// [한국어] block_class 이름 규칙에 맞게 '!'로 치환 (예: cciss/c0d0 -> cciss!c0d0)
	}

	*devt = blk_lookup_devt(s, 0);	// [한국어] 1단계: 이름 전체를 디스크 이름으로 보고 파티션 번호 0(전체 디스크)으로 조회 시도
	if (*devt)	// [한국어] devt가 0이 아니면(MKDEV(0,0)이 아니면) 유효한 결과를 찾은 것
		return 0;	// [한국어] 성공 - 더 이상의 파싱 불필요

	/*
	 * Try non-existent, but valid partition, which may only exist after
	 * opening the device, like partitioned md devices.
	 */
	while (p > s && isdigit(p[-1]))	// [한국어] 문자열 끝(p는 for 루프 이후 NUL을 가리킴)에서부터 거꾸로 숫자인 동안 계속 이동 - 말단의 연속된 숫자 구간을 찾기 위함
		p--;	// [한국어] 커서를 한 글자 앞으로 이동 (숫자 구간의 시작 지점을 찾을 때까지)
	if (p == s || !*p || *p == '0')	// [한국어] 숫자가 전혀 없거나(p==s), *p가 빈 문자열이거나(이론상 도달 안 함), 파티션 번호가 '0'으로 시작하면(선행 0은 유효한 파티션 번호 표기가 아님, 예: "sda01" 방지)
		return -ENODEV;	// [한국어] 파티션 번호로 해석할 수 있는 말단 숫자가 없으므로 실패 반환

	/* try disk name without <part number> */
	part = simple_strtoul(p, NULL, 10);	// [한국어] p가 가리키는 말단 숫자 문자열을 10진수 정수로 변환해 파티션 번호로 사용
	*p = '\0';	// [한국어] 숫자 시작 위치에 NUL을 써서 s를 "디스크 이름" 부분만 남도록 절단 (예: "sda1" -> "sda")
	*devt = blk_lookup_devt(s, part);	// [한국어] 2단계: 절단된 디스크 이름 + 파싱한 파티션 번호로 재조회
	if (*devt)	// [한국어] 유효한 결과를 찾았으면
		return 0;	// [한국어] 성공

	/* try disk name without p<part number> */
	if (p < s + 2 || !isdigit(p[-2]) || p[-1] != 'p')	// [한국어] 3단계 전제 조건 확인: 앞에 최소 2글자가 더 있어야 하고, p 바로 앞의 앞 글자(p[-2])가 숫자여야 하며(디스크 이름이 숫자로 끝남), p[-1]이 정확히 'p'여야 함 (예: "nvme0n1p" 형태)
		return -ENODEV;	// [한국어] "p<숫자>" 규칙에 맞지 않으면 더 시도할 방법이 없으므로 실패 반환
	p[-1] = '\0';	// [한국어] 'p' 위치에도 NUL을 써서 s를 한 글자 더 절단 (예: "nvme0n1p" -> "nvme0n1")
	*devt = blk_lookup_devt(s, part);	// [한국어] 3단계: 더 절단된 디스크 이름 + 동일한 파티션 번호로 재조회
	if (*devt)	// [한국어] 유효한 결과를 찾았으면
		return 0;	// [한국어] 성공
	return -ENODEV;	// [한국어] 세 단계 모두 실패 - 이 이름으로는 어떤 디스크/파티션도 찾을 수 없음을 알림
}

/*
 * [한국어]
 * devt_from_devnum - "<major>:<minor>[:<offset>]" 또는 순수 16진수 문자열을 dev_t로 변환
 *
 * @name: early_lookup_bdev()에서 PARTUUID=/PARTLABEL=//dev/ 어느 접두어에도
 *        해당하지 않은 나머지 모든 문자열(예: "8:1", "b302").
 * @devt: 성공 시 결과가 채워질 출력 버퍼.
 * @return: 0이면 성공. -EINVAL이면 두 형식 중 어느 것으로도 완전히 파싱되지
 *          않았거나(잉여 문자 존재), major/minor 값이 dev_t 인코딩 폭을
 *          벗어나 MKDEV() 왕복 변환 후 값이 달라진 경우.
 *
 * 두 가지 형식을 시도한다.
 * 1) "%u:%u%c" 또는 "%u:%u:%u:%c" 형식 — 콜론으로 구분된 major:minor
 *    (선택적으로 세 번째 오프셋 필드까지) 십진수 표기. %c까지 매칭되면
 *    (반환값이 3 또는 4) 뒤에 잉여 문자가 있다는 뜻이므로 실패로 간주하고,
 *    정확히 2개 또는 3개 필드만 매칭된 경우(반환값 2 또는 3)만 성공으로 친다.
 *    MKDEV()로 만든 뒤 MAJOR()/MINOR()로 되돌린 값이 원래 파싱한 maj/min과
 *    다르면 dev_t의 비트 폭(예: 12비트 major, 20비트 minor)을 벗어난 것이므로
 *    -EINVAL로 거부한다.
 * 2) 위 콜론 형식이 아니면 순수 16진수로 보고 simple_strtoul(base=16)로
 *    파싱한 뒤 new_decode_dev()로 커널 내부 dev_t 인코딩으로 변환한다.
 *    이때 문자열 전체가 숫자로 소진되지 않고 잉여 문자(*p)가 남으면 실패.
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션.
 * 호출자: early_lookup_bdev()가 앞의 세 접두어에 모두 해당하지 않을 때 호출
 *          (즉 사실상의 기본/폴백 경로).
 * 호출 대상: sscanf(), simple_strtoul(), new_decode_dev().
 * 에러 경로: 두 형식 모두 파싱 실패 또는 검증 실패 시 -EINVAL 반환 — 이
 *          함수에는 -ENODEV 경로가 없다(장치 존재 여부를 확인하지 않고
 *          순수 숫자 변환만 수행하기 때문).
 *
 * 호출 체인:
 *   early_lookup_bdev() → [devt_from_devnum()]
 */
static int __init devt_from_devnum(const char *name, dev_t *devt)
{
	unsigned maj, min, offset;	// [한국어] 파싱한 major/minor/(선택적) 오프셋 값을 담을 변수들 - offset은 3필드 형식에서만 채워지고 이후 사용되지는 않음(포맷 검증 목적)
	char *p, dummy;	// [한국어] p는 16진수 파싱 후 잉여 문자 검사용 커서, dummy는 sscanf의 "%c" 매칭으로 잉여 문자 존재 여부를 검출하기 위한 보초 변수

	if (sscanf(name, "%u:%u%c", &maj, &min, &dummy) == 2 ||	// [한국어] "major:minor" 형식 시도 - 정확히 2개 필드(정수 2개)만 매칭되고 뒤에 잉여 문자가 없어야 성공(반환값 2)
	    sscanf(name, "%u:%u:%u:%c", &maj, &min, &offset, &dummy) == 3) {	// [한국어] 위 형식이 실패하면 "major:minor:offset:" 3필드 형식도 시도 - 정확히 3개 필드만 매칭되어야 성공(반환값 3)
		*devt = MKDEV(maj, min);	// [한국어] 파싱한 major/minor로 dev_t 인코딩 생성
		if (maj != MAJOR(*devt) || min != MINOR(*devt))	// [한국어] 만든 dev_t를 다시 분해했을 때 원래 값과 다르면 - major/minor가 dev_t 비트 폭을 초과해 잘려나간 것
			return -EINVAL;	// [한국어] 값 손실이 발생했으므로 잘못된 인자로 거부
	} else {
		*devt = new_decode_dev(simple_strtoul(name, &p, 16));	// [한국어] 콜론 형식이 아니면 전체 문자열을 16진수로 해석 - simple_strtoul이 파싱 종료 지점을 p에 남김, new_decode_dev()로 커널 내부 dev_t 인코딩(old/huge 여부 등)으로 변환
		if (*p)	// [한국어] p가 NUL이 아니면 문자열 끝까지 숫자로 소진되지 않고 잉여 문자가 남아있다는 뜻
			return -EINVAL;	// [한국어] 잉여 문자가 있으므로 잘못된 형식으로 거부
	}

	return 0;	// [한국어] 성공 - *devt에 유효한 dev_t가 채워진 상태
}

/*
 *	Convert a name into device number.  We accept the following variants:
 *
 *	1) <hex_major><hex_minor> device number in hexadecimal represents itself
 *         no leading 0x, for example b302.
 *	3) /dev/<disk_name> represents the device number of disk
 *	4) /dev/<disk_name><decimal> represents the device number
 *         of partition - device number of disk plus the partition number
 *	5) /dev/<disk_name>p<decimal> - same as the above, that form is
 *	   used when disk name of partitioned disk ends on a digit.
 *	6) PARTUUID=00112233-4455-6677-8899-AABBCCDDEEFF representing the
 *	   unique id of a partition if the partition table provides it.
 *	   The UUID may be either an EFI/GPT UUID, or refer to an MSDOS
 *	   partition using the format SSSSSSSS-PP, where SSSSSSSS is a zero-
 *	   filled hex representation of the 32-bit "NT disk signature", and PP
 *	   is a zero-filled hex representation of the 1-based partition number.
 *	7) PARTUUID=<UUID>/PARTNROFF=<int> to select a partition in relation to
 *	   a partition with a known unique id.
 *	8) <major>:<minor> major and minor number of the device separated by
 *	   a colon.
 *	9) PARTLABEL=<name> with name being the GPT partition label.
 *	   MSDOS partitions do not support labels!
 *
 *	If name doesn't have fall into the categories above, we return (0,0).
 *	block_class is used to check if something is a disk name. If the disk
 *	name contains slashes, the device name has them replaced with
 *	bangs.
 */
/*
 * [한국어] 위 영문 주석(원문)의 번역 및 보충 설명 — early_lookup_bdev()가
 * 인식하는 root= 문자열 형식 전체 목록:
 *
 *   1) <16진수 major><16진수 minor> : 앞에 "0x" 없이 붙여 쓴 순수 16진수
 *      (예: "b302") — devt_from_devnum()의 두 번째 분기(new_decode_dev)가 처리.
 *      (원문에 2번 항목이 없는 것은 원본 커널 주석 자체의 번호 누락이며,
 *      이 파일에서는 원문을 그대로 보존한다.)
 *   3) /dev/<disk_name>            : 디스크 전체 — devt_from_devname() 1단계.
 *   4) /dev/<disk_name><decimal>   : 디스크 + 파티션 번호 — devt_from_devname() 2단계.
 *   5) /dev/<disk_name>p<decimal>  : 디스크 이름이 숫자로 끝날 때의 표기 —
 *      devt_from_devname() 3단계.
 *   6) PARTUUID=<uuid>             : GPT UUID 또는 MSDOS 파티션 서명(SSSSSSSS-PP
 *      형식, SSSSSSSS는 32비트 NT 디스크 시그니처, PP는 1-based 파티션 번호) —
 *      devt_from_partuuid().
 *   7) PARTUUID=<uuid>/PARTNROFF=<int> : UUID로 찾은 파티션 기준 상대 오프셋 —
 *      devt_from_partuuid()의 offset 처리 분기.
 *   8) <major>:<minor>             : 콜론으로 구분된 major:minor — devt_from_devnum()의
 *      첫 번째 분기(sscanf "%u:%u").
 *   9) PARTLABEL=<name>            : GPT 파티션 레이블(MSDOS는 레이블 미지원) —
 *      devt_from_partlabel().
 *
 * 위 어느 형식에도 맞지 않으면 (0,0)을 반환한다(devt_from_devnum()의
 * 16진수 분기가 결국 0을 반환하거나, sscanf가 실패해 devt_from_devnum()이
 * -EINVAL을 반환하는 경로로 귀결). block_class는 어떤 문자열이 "디스크
 * 이름"인지 확인하는 용도로 쓰이며, 디스크 이름에 슬래시가 있으면
 * devt_from_devname()이 뱅(!)으로 치환한다.
 */

/**
 * early_lookup_bdev - looks up a block device by name
 * @name:	pointer to device name and it may end with a partition number
 * @devt:	dev_t result
 *
 * Devices are checked in this order:
 *  1. devtmpfs, using the name in it
 *  2. if dev_t was created via "<hex_major><hex_minor>" boot option
 *  3. by details saved in a partition table on a device
 *  4. if name is in "major:minor" format
 * (해당 kernel-doc은 이 트리의 원본에는 존재하지 않지만, 실제 상위 upstream
 * 커널의 대응 함수 설명을 참고 삼아 한국어 함수 주석에서 그대로 다룬다.)
 */
/*
 * [한국어]
 * early_lookup_bdev - root= 문자열 형식을 판별해 4가지 하위 파서로 분기하는 최상위 진입점
 *
 * @name: 커널 커맨드라인의 root=(또는 유사한) 인자 값 그대로의 문자열.
 *        "PARTUUID=", "PARTLABEL=", "/dev/" 접두어 여부로 분기하며, 그
 *        어느 것도 아니면 major:minor/16진수 형식으로 간주한다.
 * @devt: 성공 시 결과가 채워질 출력 버퍼 — 실제로는 각 하위 함수가 직접 채운다.
 * @return: 하위 함수가 반환한 값을 그대로 전달(0=성공, 음수=에러코드).
 *          이 함수 자체는 추가 에러를 만들지 않는 단순 디스패처(dispatcher)다.
 *
 * 위의 "Convert a name into device number" 영문 주석(및 그 한국어 번역)에
 * 정리된 9가지 표기법 중 어느 것에 해당하는지 접두어 문자열만 보고 판별한다.
 * strncmp()로 각 접두어를 순서대로 검사하며, 첫 매칭되는 접두어의 처리
 * 함수로 나머지 문자열(접두어를 제외한 부분)을 넘긴다. 어느 접두어와도
 * 일치하지 않으면 devt_from_devnum()으로 폴백해 major:minor 또는 순수
 * 16진수 표기로 최종 시도한다.
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션 — 부팅 중 루트를
 * 결정할 때 단 한 번(많아야 재시도 몇 회) 호출되는 경로다.
 * 호출자: init/do_mounts.c의 name_to_dev_t() (이 트리에는 없음, 개념상 위치).
 * 호출 대상: devt_from_partuuid(), devt_from_partlabel(), devt_from_devname(),
 *          devt_from_devnum() — 정확히 이 중 하나만 호출됨.
 * 에러 경로: 각 하위 함수의 반환값을 그대로 전달 — 이 함수 차원의 별도
 *          에러 처리는 없다.
 *
 * 호출 체인:
 *   name_to_dev_t() → [early_lookup_bdev()] → devt_from_{partuuid,partlabel,devname,devnum}()
 */
int __init early_lookup_bdev(const char *name, dev_t *devt)
{
	if (strncmp(name, "PARTUUID=", 9) == 0)	// [한국어] "PARTUUID=" 접두어(9글자)와 정확히 일치하는지 확인
		return devt_from_partuuid(name + 9, devt);	// [한국어] 접두어를 제외한 나머지(UUID[/PARTNROFF=n]) 부분을 넘겨 UUID 기반 조회 수행
	if (strncmp(name, "PARTLABEL=", 10) == 0)	// [한국어] "PARTLABEL=" 접두어(10글자)와 정확히 일치하는지 확인
		return devt_from_partlabel(name + 10, devt);	// [한국어] 접두어를 제외한 레이블 문자열을 넘겨 레이블 기반 조회 수행
	if (strncmp(name, "/dev/", 5) == 0)	// [한국어] "/dev/" 접두어(5글자)와 정확히 일치하는지 확인
		return devt_from_devname(name + 5, devt);	// [한국어] 접두어를 제외한 디스크 경로 이름을 넘겨 이름 기반 조회 수행
	return devt_from_devnum(name, devt);	// [한국어] 위 세 접두어 어디에도 해당하지 않으면 major:minor 또는 16진수 표기로 간주해 폴백 처리
}

/*
 * [한국어]
 * bdevt_str - dev_t(major:minor)를 root= 인자와 같은 형식의 16진수 문자열로 변환
 *
 * @devt: 문자열로 표현할 장치 번호(major:minor).
 * @buf:  결과를 저장할 버퍼 — 호출자가 최소 BDEVT_SIZE 바이트를 보장해야 한다.
 * @return: buf 포인터를 그대로 반환(호출부에서 printk() 인자로 바로
 *          연쇄 사용할 수 있게 하기 위한 편의 반환값).
 *
 * major/minor가 모두 0xff(255) 이하로 "옛날" 8비트 major/minor 체계에
 * 들어맞으면 "%02x%02x" 형식(예: "0800", 실제로는 뒤에 공백 패딩까지 포함해
 * 왼쪽 정렬 9글자 폭으로 맞춤 — "%-9s")으로, 그 범위를 넘는 현대적인 확장
 * major/minor(12비트/20비트 등)이면 "%03x:%05x" 형식(콜론으로 구분, 예:
 * "103:00001")으로 출력한다. 두 형식 모두 root= 커맨드라인에서 그대로
 * 받아들일 수 있는 16진수 표기와 호환된다.
 * 실행 컨텍스트: 부팅 초기 단일 스레드, __init 섹션.
 * 호출자: printk_all_partitions()가 각 파티션마다 호출.
 * 호출 대상: snprintf().
 * 에러 경로: 없음 — snprintf()가 버퍼 크기를 초과해도 잘림 처리만 될 뿐
 *          별도 에러 반환은 없다.
 *
 * 호출 체인:
 *   printk_all_partitions() → [bdevt_str()]
 */
static char __init *bdevt_str(dev_t devt, char *buf)
{
	if (MAJOR(devt) <= 0xff && MINOR(devt) <= 0xff) {	// [한국어] major/minor가 모두 8비트(0~255) 범위에 들어오면 - 전통적인 "옛날" 장치 번호 체계
		char tbuf[BDEVT_SIZE];	// [한국어] 최종 왼쪽 정렬 결과를 만들기 전, "MMmm" 형식의 중간 결과를 담을 임시 버퍼
		snprintf(tbuf, BDEVT_SIZE, "%02x%02x", MAJOR(devt), MINOR(devt));	// [한국어] major/minor 각각을 2자리 16진수(0 패딩)로 붙여 "MMmm" 문자열 생성 (예: major=8,minor=0 -> "0800")
		snprintf(buf, BDEVT_SIZE, "%-9s", tbuf);	// [한국어] 위 문자열을 폭 9의 왼쪽 정렬로 buf에 채움 (오른쪽은 공백 패딩) - printk_all_partitions() 출력 정렬용
	} else	// [한국어] major 또는 minor 중 하나라도 0xff를 초과하면 - 확장된(현대적) major/minor 체계
		snprintf(buf, BDEVT_SIZE, "%03x:%05x", MAJOR(devt), MINOR(devt));	// [한국어] "major:minor"를 콜론으로 구분한 16진수(각각 3자리/5자리, 0 패딩)로 buf에 직접 채움

	return buf;	// [한국어] 채워진 buf 포인터를 그대로 반환 - 호출자가 printk() 인자 표현식 안에서 바로 사용 가능하게 함
}

/*
 * print a full list of all partitions - intended for places where the root
 * filesystem can't be mounted and thus to give the victim some idea of what
 * went wrong
 */
/*
 * [한국어] 위 영문 주석 번역: 루트 파일 시스템을 마운트할 수 없을 때, 무엇이
 * 잘못되었는지 사용자("victim" — 원문의 다소 유머러스한 표현을 그대로 유지)가
 * 짐작할 수 있도록 등록된 모든 파티션의 전체 목록을 출력하기 위한 함수.
 *
 * [한국어]
 * printk_all_partitions - block_class에 등록된 모든 파티션을 콘솔에 진단 출력
 *
 * (매개변수 없음, 반환값 void)
 * @return: 없음(void) — 오직 printk()를 통한 부수효과(커널 로그 출력)만 수행.
 *
 * blk_lookup_devt()/printk_all_partitions() 둘 다 class_dev_iter로
 * block_class의 disk_type 장치들을 순회하지만, 이 함수는 이름 매칭이
 * 아니라 각 디스크의 파티션 테이블(part_tbl, xarray 자료구조) 전체를
 * xa_for_each()로 순회하며 모든 파티션 정보를 출력한다는 점이 다르다.
 * 용량이 0이거나 GENHD_FL_HIDDEN 플래그가 설정된 디스크는 건너뛰어
 * 사용자에게 의미 없는(또는 숨겨야 할) 장치를 노출하지 않는다. 파티션마다
 * "major:minor(16진수) 섹터수 %pg(디스크 이름) UUID" 형식으로 한 줄을
 * 출력하며, 파티션이 아니라 디스크 전체 항목이면 이어서 드라이버 이름까지
 * 함께 출력해 어떤 드라이버가 이 디스크를 담당하는지 보여준다.
 * xa_for_each()로 part_tbl을 순회하는 동안 RCU(Read-Copy-Update) read
 * lock을 잡는데, 이는 파티션 테이블이 다른 CPU/경로에서 RCU 방식으로
 * 갱신될 수 있음을 전제로 한 것이다(비록 이 시점은 사실상 단일 스레드
 * 부팅 컨텍스트이지만, 이 함수 자체는 __init이 아니라서 이론상 SMP가 이미
 * 올라온 이후에도 호출될 수 있어 정석대로 rcu_read_lock을 사용한다).
 * 실행 컨텍스트: __init이 아닌 일반 커널 컨텍스트에서도 호출 가능(다른
 * 함수들과 달리 부팅 이후 free_initmem()에도 살아남음) — 그러나 이 함수
 * 안에서 RCU read-side 임계 구역 동안은 슬립할 수 없다.
 * 호출자: 루트 마운트 실패 경로(예: mount_block_root() 계열, 이 트리에는
 *        해당 호출부가 없어 개념상으로만 표기).
 * 호출 대상: class_dev_iter_init/_next/_exit(), dev_to_disk(), get_capacity(),
 *          rcu_read_lock/_unlock(), xa_for_each(), bdev_nr_sectors(),
 *          bdevt_str(), bdev_is_partition(), printk().
 * 에러 경로: 없음 — 진단 출력 목적이므로 실패해도 그냥 다음 항목으로 넘어감.
 *
 * 호출 체인:
 *   (루트 마운트 실패 경로) → [printk_all_partitions()] → bdevt_str()
 */
void __init printk_all_partitions(void)
{
	struct class_dev_iter iter;	// [한국어] block_class 순회 상태를 담는 반복자
	struct device *dev;	// [한국어] 순회 중 현재 디스크 device를 가리킬 포인터

	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);	// [한국어] block_class 아래 disk_type(디스크 전체) device들만 순회하도록 초기화
	while ((dev = class_dev_iter_next(&iter))) {	// [한국어] 다음 디스크 device를 하나씩 가져옴 - NULL이면 순회 종료
		struct gendisk *disk = dev_to_disk(dev);	// [한국어] 현재 device를 gendisk로 변환 - part_tbl/flags 접근을 위해 필요
		struct block_device *part;	// [한국어] xa_for_each 순회 중 각 파티션(또는 디스크 전체 항목)을 가리킬 포인터
		char devt_buf[BDEVT_SIZE];	// [한국어] bdevt_str()이 채울 "major:minor" 16진수 문자열 버퍼
		unsigned long idx;	// [한국어] xa_for_each()가 사용하는 xarray 인덱스(파티션 번호) 순회 변수

		/*
		 * Don't show empty devices or things that have been
		 * suppressed
		 */
		if (get_capacity(disk) == 0 || (disk->flags & GENHD_FL_HIDDEN))	// [한국어] 용량이 0인 디스크(예: 아직 초기화 안 된 장치)이거나 GENHD_FL_HIDDEN 플래그로 명시적으로 숨김 처리된 디스크면
			continue;	// [한국어] 출력 목록에서 제외하고 다음 디스크로 건너뜀

		/*
		 * Note, unlike /proc/partitions, I am showing the numbers in
		 * hex - the same format as the root= option takes.
		 */
		rcu_read_lock();	// [한국어] part_tbl(xarray)을 RCU 방식으로 안전하게 순회하기 위해 read-side 임계 구역 진입 - 순회 중 갱신되더라도 use-after-free 없이 일관된 스냅샷을 봄
		xa_for_each(&disk->part_tbl, idx, part) {	// [한국어] 이 디스크의 파티션 테이블(xarray)에 등록된 모든 항목(디스크 전체 항목 포함)을 순회
			if (!bdev_nr_sectors(part))	// [한국어] 섹터 수가 0인 항목(예: 아직 크기가 확정되지 않은 파티션)이면
				continue;	// [한국어] 의미 없는 항목이므로 건너뜀
			printk("%s%s %10llu %pg %s",	// [한국어] 한 파티션/디스크 항목을 출력 - "%pg"는 block_device 포인터를 받아 디스크 이름 문자열로 포맷하는 커널 전용 지시자
			       bdev_is_partition(part) ? "  " : "",	// [한국어] 파티션이면 들여쓰기용 공백 2칸, 디스크 전체 항목이면 들여쓰기 없음 - /proc/partitions와 유사한 트리 형태 표현
			       bdevt_str(part->bd_dev, devt_buf),	// [한국어] 이 항목의 dev_t를 "major:minor" 16진수 문자열로 변환
			       bdev_nr_sectors(part) >> 1, part,	// [한국어] 섹터 수를 1비트 우측 시프트(즉 2로 나눔)해 킬로바이트 단위로 환산(섹터=512바이트 가정) 후, part 포인터 자체를 "%pg" 포맷 인자로 전달
			       part->bd_meta_info ?	// [한국어] 파티션 메타데이터(UUID/레이블)가 있으면
					part->bd_meta_info->uuid : "");	// [한국어] 그 UUID 문자열을, 없으면 빈 문자열을 마지막 필드로 출력
			if (bdev_is_partition(part))	// [한국어] 방금 출력한 항목이 파티션이면(디스크 전체가 아니면)
				printk("\n");	// [한국어] 드라이버 이름 없이 줄바꿈만 출력하고 해당 줄 마무리
			else if (dev->parent && dev->parent->driver)	// [한국어] 디스크 전체 항목이고, 부모 device와 그 driver 정보가 모두 존재하면
				printk(" driver: %s\n",	// [한국어] 이 디스크를 담당하는 드라이버 이름까지 이어서 출력 - 어떤 드라이버가 이 하드웨어를 관리하는지 보여주는 진단 정보
					dev->parent->driver->name);	// [한국어] 부모 device에 바인딩된 driver 구조체의 이름 문자열
			else	// [한국어] 드라이버 정보를 알 수 없는 경우(부모 device가 없거나 driver가 바인딩되지 않음)
				printk(" (driver?)\n");	// [한국어] "드라이버 불명"을 뜻하는 자리표시자 문자열 출력
		}
		rcu_read_unlock();	// [한국어] RCU read-side 임계 구역 종료 - part_tbl에 대한 보호 해제
	}
	class_dev_iter_exit(&iter);	// [한국어] 반복자가 잡고 있던 리소스 해제
}
