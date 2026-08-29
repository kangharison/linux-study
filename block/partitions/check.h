/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] 파티션 검출 인프라 공용 내부 헤더 (block/partitions/check.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 block/partitions/ 디렉터리 아래의 모든 파티션 포맷 검출기
 * (msdos.c, efi.c, mac.c, acorn.c, aix.c, amiga.c, atari.c, cmdline.c,
 * ibm.c, karma.c, ldm.c, of.c, osf.c, sgi.c, sun.c, sysv68.c, ultrix.c 등
 * 20여 개 파일)가 공유하는 단 하나의 공용 계약(contract)이다. 파티션
 * 스캔이 진행되는 동안의 임시 상태를 담는 struct parsed_partitions와,
 * 원시 섹터를 안전하게 읽고 반납하는 read_part_sector()/put_dev_sector(),
 * 검출한 파티션 하나를 상태에 기록하는 put_partition(), 그리고 각
 * 포맷별 프로버 함수(msdos_partition, efi_partition 등)의 프로토타입을
 * 선언한다. 이 파일은 block/partitions/ 서브디렉터리 전용 내부(private)
 * 헤더로, 이 디렉터리 밖의 코드에서는 include하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * gendisk가 등록(add_disk()) 되거나 재스캔 ioctl(BLKRRPART 등)이 발생하면
 * bdev_disk_changed() -> blk_add_partitions()(core.c, static)가 호출되고,
 * 그 안에서 static 함수 check_partition()이 본 헤더가 선언한 프로버들을
 * check_part[] 배열 순서대로(Kconfig로 활성화된 것만) 하나씩 함수
 * 포인터로 호출한다. 각 프로버는 이 헤더의 read_part_sector()로 디스크
 * 앞부분(대개 LBA 0/1)을 읽어 MBR 시그니처, GPT 헤더, 디스크 레이블 등
 * 포맷 고유의 매직을 검사하고, 성공하면 put_partition()으로 결과를
 * parsed_partitions.parts[]에 적재한다. 이 스캔 단계가 끝나면
 * blk_add_partitions()가 parts[]를 순회하며 blk_add_partition() ->
 * add_partition()을 통해 실제 block_device(예: /dev/nvme0n1p1,
 * /dev/sda1)를 생성한다. 즉 본 헤더는 "원시 디스크 바이트를 읽는 단계"와
 * "커널에 파티션 block_device를 등록하는 단계" 사이에 낀 파싱 전용
 * 스크래치 영역을 정의한다. 이 스터디에서는 NVMe SSD(예: nvme0n1)를
 * 구체적 예시 매체로 자주 사용하지만, 이 헤더 자체와 read_part_sector()의
 * page-cache 기반 구현은 특정 스토리지 프로토콜에 종속되지 않으며
 * SATA/SCSI/virtio-blk/loop 등 gendisk를 갖는 모든 블록 장치에 동일하게
 * 적용된다.
 *
 * === 타 모듈과의 연결 ===
 * 상위 방향으로는 block/partitions/core.c가 이 헤더의 유일한 조율자로,
 * allocate_partitions()에서 parsed_partitions를 생성하고 check_partition()
 * 루프를 돌며 각 프로버를 호출한 뒤 free_partitions()로 정리한다.
 * read_part_sector()의 실제 정의도 core.c에 있으며, 내부적으로
 * state->disk->part0->bd_mapping의 페이지 캐시를 read_mapping_folio()로
 * 조회한다(캐시 미스 시 하위 블록 드라이버의 submit_bio 경로가 실제 I/O를
 * 발생시킨다). 하위 방향으로는 msdos.c/efi.c/mac.c 등 20여 개 포맷별
 * 구현 파일이 이 헤더가 선언한 프로토타입을 정의하고, struct
 * parsed_partitions와 Sector, put_partition()/put_dev_sector()를 사용해
 * 각자의 파싱 로직을 구현한다. 공유 핵심 자료구조는 struct
 * parsed_partitions(스캔 세션 하나 = gendisk 하나) 이며, 그 안의 parts[]
 * 배열이 "이번 스캔에서 발견된 파티션 후보 목록"이라는 데이터 흐름의
 * 중심이다. struct partition_meta_info(include/linux/blkdev.h)는 GPT의
 * 파티션 GUID/이름처럼 포맷별 확장 메타데이터를 parts[].info에 실어
 * 나르는 보조 구조체다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct parsed_partitions: 파티션 스캔 세션 하나의 전체 상태(대상
 *   gendisk, 이름 문자열, 발견된 파티션 배열, 다음 슬롯 인덱스, 최대
 *   개수, EOD 초과 여부, 로그 버퍼)를 담는다.
 * - typedef struct Sector: read_part_sector()가 반환한 folio 하나를
 *   감싸는 얇은 래퍼로, put_dev_sector()로 반드시 해제해야 한다.
 * - read_part_sector(): LBA n을 페이지 캐시를 통해 읽어 가상 주소를
 *   반환하는 유일한 섹터 읽기 진입점(실제 정의는 core.c).
 * - put_dev_sector(): read_part_sector()가 잡은 folio 참조를 반납한다.
 * - put_partition(): 슬롯 n에 [from, from+size) 파티션을 기록하고 로그
 *   문자열에 이름을 덧붙인다.
 * - adfspart_check_ADFS 등 5종부터 aix_partition, ..., ultrix_partition까지:
 *   포맷별 검출 프로버 20종의 프로토타입. 모두 동일한 계약(@return: 1=성공,
 *   0=이 포맷 아님, 음수=I/O 오류)을 따른다.
 */
#include <linux/pagemap.h>	/* [한국어] read_mapping_folio() 등 페이지 캐시 접근 API - read_part_sector()가 LBA를 folio로 읽어올 때 필요 */
#include <linux/blkdev.h>	/* [한국어] struct gendisk/block_device, sector_t, partition_meta_info 등 블록 계층 핵심 타입 선언 - parsed_partitions가 이들을 그대로 사용 */
#include <linux/seq_buf.h>	/* [한국어] struct seq_buf / seq_buf_printf() - 파티션 스캔 결과를 "nvme0n1: p1 p2 ..." 형태 문자열로 누적할 때 사용 */
#include "../blk.h"		/* [한국어] block 계층 내부 전용 선언(예: ADDPART_FLAG_NONE/RAID/WHOLEDISK/READONLY) - block/partitions/ 는 blk.h가 정의하는 비공개 API에 의존 */

/*
 * add_gd_partition adds a partitions details to the devices partition
 * description.
 */
/*
 * [한국어] 위 원본 주석 보강:
 * add_gd_partition()이라는 이름의 함수는 현재 트리(block/ 전체)에는
 * 존재하지 않는다 - 오래된 커널 버전에서 파티션을 gendisk에 최종
 * 등록하던 함수의 이름이 남아있는 주석 유산으로 보인다(추정). 현재
 * 트리에서 그 역할은 block/partitions/core.c의 add_partition() /
 * blk_add_partition()이 담당한다. 이 헤더의 struct parsed_partitions는
 * 바로 그 최종 등록 단계가 참조할 원재료(파티션 시작 LBA/크기/플래그/
 * 메타정보)를 스캔 단계에서 미리 모아두는 임시 컨테이너 역할을 한다.
 */
/*
 * [한국어]
 * parsed_partitions - 파티션 스캔 세션 하나의 상태를 담는 구조체.
 *
 * gendisk 한 개에 대해 check_partition()이 호출될 때마다 이 구조체가
 * 하나 할당(allocate_partitions())되고, 등록된 모든 포맷 프로버가 이
 * 구조체를 통해 섹터를 읽고 발견한 파티션을 parts[]에 적재한다. 스캔이
 * 끝나면(성공/실패 불문) free_partitions()로 해제되는, 스캔 도중에만
 * 존재하는 단명(短命) 스크래치 상태다. 필드 상세 설명은 각 필드 옆
 * 주석을 참고.
 */
struct parsed_partitions {
	struct gendisk *disk;
	/* [한국어] 이번 파티션 스캔의 대상이 되는 gendisk(범용 디스크) 포인터.
	 * 설정자: allocate_partitions()에서 state->disk = hd;로 최초 설정.
	 * 읽는 자: read_part_sector()가 disk->part0->bd_mapping을 통해
	 *   page-cache 매핑을 얻을 때, 각 프로버가 get_capacity(state->disk)
	 *   등으로 전체 용량을 조회할 때 사용.
	 * 값 범위: NULL 아님 - blk_add_partitions() 호출 시점에 이미 등록되어
	 *   있는 유효한 gendisk를 가리킨다.
	 * 동기화: 파티션 스캔은 add_disk()/재스캔 ioctl 경로에서 단일
	 *   스레드로 수행되므로 이 필드에 대한 별도 락은 없다. */

	char name[BDEVNAME_SIZE];
	/* [한국어] 커널 로그와 파티션 접미사 생성에 쓰이는 디스크 이름 문자열.
	 * 설정자: allocate_partitions() 이후 core.c의 check_partition()이
	 *   strscpy(state->name, hd->disk_name)으로 채우고, 이름이 숫자로
	 *   끝나면(예: "nvme0n1") sprintf(state->name, "p")로 파티션 접미사
	 *   'p' 한 글자로 덮어써 재사용한다(즉 이 필드는 스캔 중 두 단계로
	 *   쓰인다: 처음엔 디스크 이름, 이후엔 접미사 문자).
	 * 읽는 자: put_partition()이 seq_buf_printf(&pp_buf, " %s%d",
	 *   p->name, n)으로 "p1", "p2" 같은 접미사를 로그에 이어붙일 때 읽음.
	 * 값 범위: BDEVNAME_SIZE(32바이트) 이내 NUL 종료 문자열.
	 * 동기화: 스캔 스레드 단독 접근, 락 불필요. */
	struct {
		sector_t from;
		/* [한국어] 이 파티션 후보가 시작하는 절대 LBA(논리 블록 주소).
		 * 설정자: 각 포맷 프로버(msdos_partition 등)가 put_partition()을
		 *   호출할 때 인자로 전달 -> put_partition() 내부에서
		 *   p->parts[n].from = from;으로 대입.
		 * 읽는 자: blk_add_partitions()가 스캔 종료 후 parts[]를 순회하며
		 *   blk_add_partition() -> add_partition()에 시작 오프셋으로
		 *   전달할 때 읽는다.
		 * 값 범위: 0 이상, get_capacity(state->disk) 미만이어야 정상이다.
		 *   포맷에 따라 CHS(실린더/헤드/섹터)를 LBA로 환산한 값일 수 있음.
		 * 동기화: 스캔 스레드 단독 접근. */

		sector_t size;
		/* [한국어] 이 파티션 후보의 길이(섹터 개수).
		 * 설정자: put_partition()이 from과 함께 p->parts[n].size = size;로
		 *   기록. 각 프로버가 파티션 테이블 엔트리에서 읽은 섹터 수를
		 *   그대로 전달하거나, 디스크 끝까지로 잘라 계산한 값을 전달한다.
		 * 읽는 자: blk_add_partitions()의 add_partition() 호출부가 block
		 *   device 크기로 사용.
		 * 값 범위: 1 이상. from+size가 디스크 용량을 넘으면 상위 계층
		 *   (core.c)에서 access_beyond_eod 플래그와 함께 처리된다.
		 * 동기화: 스캔 스레드 단독 접근. */

		int flags;
		/* [한국어] 이 파티션에 부여할 부가 속성 비트마스크(block/blk.h 정의).
		 * 설정자: 포맷 프로버가 필요 시 state->parts[slot].flags에
		 *   ADDPART_FLAG_RAID(=1, md/RAID 멤버 표시),
		 *   ADDPART_FLAG_WHOLEDISK(=2, 파티션 테이블 없이 디스크 전체를
		 *   등록), ADDPART_FLAG_READONLY(=4, 읽기 전용 등록)를 OR로 조합해
		 *   대입한다(msdos.c, efi.c, sun.c, sgi.c, mac.c, of.c, cmdline.c).
		 * 읽는 자: core.c의 add_partition()이 이 값을 그대로
		 *   bdev_add_partition() 계열에 전달해 실제 block_device의 읽기
		 *   전용 여부/RAID autodetect 힌트로 반영한다.
		 * 값 범위: ADDPART_FLAG_NONE(0)에서 비트 OR 조합. put_partition()
		 *   자체는 flags를 건드리지 않으므로 프로버가 명시적으로 설정하지
		 *   않으면 0(할당 시 vzalloc으로 이미 0).
		 * 동기화: 스캔 스레드 단독 접근. */

		bool has_info;
		/* [한국어] info 필드(바로 아래)가 유효한 메타데이터로 채워졌는지 표시.
		 * 설정자: GPT(efi.c)가 파티션 GUID/이름을 읽었을 때, cmdline.c가
		 *   커맨드라인에서 volname을 파싱했을 때, msdos.c가 UUID를
		 *   구성했을 때 state->parts[slot].has_info = true;로 설정한다.
		 *   기본값은 allocate_partitions()의 vzalloc으로 인해 false(0).
		 * 읽는 자: core.c의 add_partition() 호출부가 has_info를 보고
		 *   partition_meta_info 포인터를 넘길지(&state->parts[p].info)
		 *   NULL을 넘길지 결정한다.
		 * 값 범위: true/false.
		 * 동기화: 스캔 스레드 단독 접근. */

		struct partition_meta_info info;
		/* [한국어] 파티션의 확장 메타데이터(UUID 문자열, 볼륨 이름).
		 * 설정자: efi.c(GPT partition GUID/이름), cmdline.c(volname),
		 *   msdos.c(구성한 UUID 문자열)가 info.uuid[]/info.volname[]에
		 *   기록한다.
		 * 읽는 자: core.c의 add_partition()이 has_info==true일 때 이
		 *   구조체를 block_device의 메타정보로 복사해 sysfs
		 *   (/sys/block/.../partN/{partuuid,partlabel}) 등으로 노출한다.
		 * 값 범위: uuid는 PARTITION_META_INFO_UUIDLTH 이내 문자열,
		 *   volname은 PARTITION_META_INFO_VOLNAMELTH 이내 문자열(둘 다
		 *   include/linux/blkdev.h 정의). has_info==false일 때는 내용이
		 *   의미 없다(0으로 초기화된 상태).
		 * 동기화: 스캔 스레드 단독 접근. */
	} *parts;
	/* [한국어] 위 익명 struct를 원소로 하는, "이번 스캔에서 발견된 파티션
	 * 후보 전체"를 담는 배열의 포인터.
	 * 설정자: allocate_partitions()가 vzalloc(array_size(limit,
	 *   sizeof(state->parts[0])))로 최초 할당(0으로 초기화)한다.
	 *   check_partition()의 메인 루프는 각 프로버를 호출하기 전마다
	 *   memset(state->parts, 0, ...)으로 리셋해, 이전에 실패한 프로버가
	 *   남긴 반쪽짜리 결과를 지운다.
	 * 읽는 자: 각 프로버는 put_partition()을 통해서만 이 배열에 쓰고,
	 *   blk_add_partitions()는 스캔 성공 후 이 배열 전체를 순회하며 실제
	 *   block_device를 생성한다. free_partitions()가 vfree()로 해제한다.
	 * 값 범위: limit개 원소(인덱스 0..limit-1). 관례상 0번 인덱스는
	 *   파티션 번호가 1번부터 시작하는 포맷이 많아 비어 있는 경우가 흔함.
	 * 동기화: 스캔 스레드 단독 접근, 락 불필요. */
	int next;
	/* [한국어] 다음에 채울 parts[] 슬롯 인덱스(순차 할당 방식의 프로버가
	 * 사용하는 카운터).
	 * 설정자/읽는 자: put_partition() 자체는 next를 갱신하지 않고 슬롯
	 *   번호 n을 인자로 직접 받으므로, 이 필드는 각 프로버 구현이 필요에
	 *   따라 자체적으로 증가시키며 읽고 쓴다(순차 할당 로직을 쓰는
	 *   프로버가 "다음 빈 슬롯이 몇 번인지" 기억하는 용도).
	 * 값 범위: 0 이상 limit 이하.
	 * 동기화: 스캔 스레드 단독 접근. */
	int limit;
	/* [한국어] parts[] 배열의 최대 유효 인덱스 상한(원소 개수).
	 * 설정자: allocate_partitions()에서 DISK_MAX_PARTS(256)로 고정 설정.
	 * 읽는 자: put_partition()이 if (n < p->limit) 검사로 배열 범위를
	 *   벗어난 쓰기를 조용히 무시할 때, check_partition()이
	 *   memset(state->parts, 0, state->limit * sizeof(...))로 매 라운드
	 *   초기화할 크기를 계산할 때 읽는다.
	 * 값 범위: 현재 커널에서는 항상 DISK_MAX_PARTS(256)로 고정된다.
	 * 동기화: 스캔 스레드 단독 접근, 이후로는 읽기 전용으로만 사용. */
	bool access_beyond_eod;
	/* [한국어] 스캔 도중 디스크 끝(EOD, End Of Disk)을 넘어선 LBA를 읽으려
	 * 한 적이 있는지 표시하는 플래그.
	 * 설정자: read_part_sector()가 요청받은 LBA n이
	 *   get_capacity(state->disk) 이상일 때 state->access_beyond_eod =
	 *   true;로 설정한다(core.c). 한 번 세팅되면 스캔이 끝날 때까지
	 *   해제되지 않는다.
	 * 읽는 자: check_partition()이 모든 프로버가 실패(res <= 0)했을 때
	 *   이 플래그를 보고 err = -ENOSPC;로 승격시켜, "포맷을 인식 못한
	 *   것"과 "디스크 자체가 너무 작아 읽기가 실패한 것"을 구분해서
	 *   보고한다. blk_add_partitions()도 스캔 성공 시 이 플래그를 보고
	 *   native capacity를 잠금 해제한 뒤 재시도할지 판단한다.
	 * 값 범위: false(기본)/true.
	 * 동기화: 스캔 스레드 단독 접근. */
	struct seq_buf pp_buf;
	/* [한국어] 스캔 결과를 사람이 읽을 수 있는 한 줄 로그로 누적하는
	 * seq_buf(순차 문자열 버퍼) 컨트롤 블록.
	 * 설정자: check_partition()이 4KiB 페이지를 __get_free_page()로
	 *   할당해 seq_buf_init(&state->pp_buf, buffer, PAGE_SIZE)로 초기화한
	 *   뒤 " %s:" 형태로 디스크 이름을 먼저 기록한다. put_partition()이
	 *   파티션을 찾을 때마다 seq_buf_printf(&pp_buf, " %s%d", name, n)로
	 *   이어붙인다.
	 * 읽는 자: check_partition()이 성공 시 printk(KERN_INFO "%s",
	 *   seq_buf_str(&state->pp_buf))로 커널 로그에 한 줄 출력한다(예:
	 *   " nvme0n1: p1 p2"). 이후 free_page()로 버퍼를 해제한다.
	 * 값 범위: PAGE_SIZE(4096바이트) 이내로 누적되며, 초과분은 seq_buf
	 *   자체 규칙에 따라 잘린다(overflow 플래그로 표시).
	 * 동기화: 스캔 스레드 단독 접근. */
};

/*
 * [한국어]
 * Sector - read_part_sector()가 반환한 섹터 데이터를 감싸는 얇은 래퍼 타입.
 *
 * 이 타입 자체는 필드 하나(v)만 가지는 값 타입으로, 호출자의 스택에 두고
 * 사용한다. read_part_sector()가 이 구조체의 v를 채워주고, 사용이 끝나면
 * 반드시 put_dev_sector()를 호출해 v가 가리키는 folio 참조를 반납해야
 * 한다. 그렇지 않으면 folio의 참조 카운트가 영구히 남아 해당 페이지가
 * 회수되지 않는 누수가 발생한다.
 */
typedef struct {
	struct folio *v;
	/* [한국어] read_part_sector()가 읽어들인, 섹터 데이터를 담고 있는
	 * 페이지 캐시 folio(연속된 페이지들을 묶은 메모리 관리 단위).
	 * 설정자: read_part_sector()가 read_mapping_folio()의 반환값을
	 *   p->v = folio;로 대입한다. 실패 시(EOD 초과 또는 I/O 오류)에는
	 *   p->v = NULL로 설정된다.
	 * 읽는 자: put_dev_sector()가 folio_put(p.v)로 참조 카운트를
	 *   감소시킬 때 읽는다. 호출자는 read_part_sector()의 반환값(가상
	 *   주소)만으로 섹터 내용에 접근하고, v 자체는 해제 목적으로만
	 *   다시 사용한다.
	 * 값 범위: 유효한 folio 포인터 또는 NULL(읽기 실패 시). NULL인 채로
	 *   put_dev_sector()를 호출하면 folio_put(NULL)이 되므로, 호출자는
	 *   read_part_sector()가 NULL을 반환한 경우 put_dev_sector()를
	 *   호출하지 않아야 한다(각 프로버 구현의 관례).
	 * 동기화: 이 Sector 값 자체는 단일 스레드의 스택에서 생성/해제되는
	 *   지역 값이라 별도 락이 필요 없다. 다만 v가 가리키는 folio의 참조
	 *   카운트는 원자적(atomic) 연산으로 보호된다(folio_put 내부). */
} Sector;

/*
 * [한국어]
 * read_part_sector() - 파티션 테이블이 있는 단일 섹터를 동기적으로 읽는다.
 *
 * @state: 스캔 세션 상태. state->disk에서 대상 gendisk와 page-cache 매핑을
 *         얻고, 실패 시 state->access_beyond_eod를 갱신하는 데 쓰인다.
 * @n: 읽고자 하는 절대 LBA(sector_t, 512바이트 논리 블록 단위).
 * @p: 결과 folio 참조를 돌려받을 Sector 출력 파라미터(호출자가 스택에
 *     마련해 넘긴다).
 * @return: 섹터 데이터가 시작하는 커널 가상 주소. 실패(디스크 끝을 넘는
 *          LBA 요청, 페이지 캐시 확보 실패/I/O 오류) 시 NULL이며, 이 경우
 *          p->v도 NULL로 설정되므로 put_dev_sector()를 호출하면 안 된다.
 *
 * 목적: MBR/GPT 등 파티션 시그니처를 확인하려면 디스크의 특정 LBA 원시
 * 바이트가 필요한데, 이를 매번 새 bio를 만들어 읽는 대신 대상 gendisk의
 * page cache(state->disk->part0->bd_mapping)를 통해 read_mapping_folio()로
 * 읽어 folio 단위로 캐싱/재사용한다.
 * 동작 과정: (1) n이 get_capacity(state->disk) 이상이면 EOD 초과로 보고
 * access_beyond_eod 플래그만 세운 뒤 NULL을 반환한다. (2) 아니면
 * read_mapping_folio()를 호출한다 - 캐시 히트면 즉시 반환되고, 미스면
 * 내부적으로 하위 블록 드라이버(submit_bio 경로)까지 내려가 실제 디스크
 * 에서 읽어온다. (3) 성공하면 folio를 p->v에 저장하고,
 * folio_address(folio) + offset_in_folio(folio, n * SECTOR_SIZE)로 해당
 * LBA가 folio 내부의 몇 바이트 오프셋에 있는지 계산해 그 가상 주소를
 * 돌려준다(folio가 여러 섹터/페이지를 한 번에 담을 수 있기 때문에 필요한
 * 보정이다).
 * 실행 컨텍스트: 파티션 스캔은 add_disk() 또는 재스캔 ioctl을 처리하는
 * 단일 프로세스 컨텍스트에서 동기적으로 실행되며, 인터럽트/GPU 컨텍스트와
 * 무관하다. 캐시 미스 시 내부적으로 블로킹 I/O가 발생할 수 있다.
 * 호출자: 각 포맷 프로버(msdos_partition, efi_partition, mac_partition 등
 * 20여 개)가 자신의 시그니처 검사를 위해 직접 호출한다.
 * 피호출자: read_mapping_folio()(페이지 캐시), 캐시 미스 시 그 내부의
 * 블록 계층 읽기 경로.
 * 에러 처리: 호출자는 반환값이 NULL이면 "이 섹터를 읽을 수 없음"으로
 * 간주하고, 자신의 프로브 결과를 0(포맷 아님) 또는 음수(I/O 오류)로
 * 반환해 다음 프로버로 넘어가게 한다.
 * 이 프로토타입의 실제 정의는 이 헤더가 아니라 block/partitions/core.c에
 * 있다(본 헤더는 선언만 제공하는 공용 인터페이스).
 *
 * 호출 체인:
 *   blk_add_partitions() -> check_partition() -> check_part[](예:
 *     msdos_partition) -> [read_part_sector] -> read_mapping_folio()
 *     -> (캐시 미스 시) 하위 블록 드라이버 읽기 경로
 */
void *read_part_sector(struct parsed_partitions *state, sector_t n, Sector *p);
/*
 * [한국어]
 * put_dev_sector() - read_part_sector()로 얻은 섹터 folio의 참조를 반납한다.
 *
 * @p: read_part_sector()가 채워준 Sector 값(포인터가 아니라 값으로
 *     전달됨 - folio 포인터 하나만 담고 있어 복사 비용이 작기 때문).
 * @return: 없음(void).
 *
 * 목적: read_part_sector()가 확보해 둔 페이지 캐시 참조를, 프로버가 섹터
 * 내용을 다 사용한 뒤 반드시 되돌려주기 위한 짝 함수다. 이 호출을
 * 빠뜨리면 folio의 참조 카운트가 남아 해당 페이지가 영구히 회수되지
 * 않는 메모리 누수로 이어진다.
 * 동작: folio_put(p.v) 한 줄로, folio의 참조 카운트를 원자적으로
 * 감소시키고 0이 되면 folio를 페이지 할당자에 반납한다.
 * 실행 컨텍스트: read_part_sector()와 마찬가지로 파티션 스캔의 단일
 * 프로세스 컨텍스트에서 호출되며, 별도 락 없이 folio 내부의 원자적 참조
 * 카운트만으로 동시성이 보장된다(다른 CPU가 같은 folio를 잡고 있어도
 * 안전).
 * 호출자: read_part_sector()를 호출했던 각 포맷 프로버가 섹터 처리를
 * 마친 시점에 호출한다(성공적으로 non-NULL을 받았을 때만 호출해야 하며,
 * NULL을 받은 경우에는 호출하면 안 된다 - 각 프로버 구현의 관례).
 * 피호출자: folio_put()(mm 계층의 페이지 캐시 참조 카운트 감소 API).
 * 에러 처리: 이 함수 자체는 실패할 수 없다(반환값 없음).
 *
 * 호출 체인:
 *   <포맷 프로버>(read_part_sector() 사용 후) -> [put_dev_sector]
 *     -> folio_put()
 */
static inline void put_dev_sector(Sector p)
{
	folio_put(p.v);	/* [한국어] folio 참조 카운트를 원자적으로 1 감소 - 0이 되면 페이지 캐시가 folio를 회수 */
}

/*
 * [한국어]
 * put_partition() - 검출된 파티션 하나를 parsed_partitions 상태에 등록한다.
 *
 * @p: 등록 대상 스캔 상태(parsed_partitions). p->parts[]와 p->pp_buf가
 *     갱신 대상이다.
 * @n: 기록할 parts[] 슬롯 번호(파티션 번호, 보통 1부터 시작).
 * @from: 파티션 시작 LBA(절대 섹터 번호).
 * @size: 파티션 길이(섹터 수).
 * @return: 없음(void). n이 범위를 벗어나면 조용히 아무 일도 하지 않는다
 *          (호출자는 반환값으로 성공 여부를 알 수 없다 - 각 프로버가
 *          스스로 n < limit을 보장하도록 설계된 관례다).
 *
 * 목적: 각 포맷 프로버가 파싱해낸 "파티션 하나"의 위치 정보를 공용
 * 자료구조에 반영하고, 동시에 커널 로그용 문자열에도 이름을 남기는 단일
 * 진입점을 제공해, 모든 프로버가 parts[]를 직접 건드리지 않고 일관된
 * 방식으로 기록하게 한다.
 * 동작 과정: (1) n이 p->limit보다 작은지 검사한다 - 아니면 배열 범위를
 * 벗어나므로 아무 것도 하지 않고 반환한다(파티션 테이블이 손상되어
 * 비정상적으로 많은 엔트리를 담고 있을 때의 방어 코드). (2)
 * p->parts[n].from과 .size에 좌표를 기록한다. (3)
 * seq_buf_printf(&p->pp_buf, " %s%d", p->name, n)으로 " <name><n>"
 * 형태를 로그 버퍼에 이어붙인다.
 *
 * ★ 주의: 출력되는 name은 디스크 이름이 아니다 ★
 * check_partition()(block/partitions/core.c)이 디스크 이름의 마지막 글자가
 * 숫자이면(nvme0n1, loop0 등) state->name을 "p" 한 글자로 덮어쓴다. 그
 * 직전에 전체 이름을 pp_buf 접두어(" nvme0n1:")로 이미 찍어 두었기 때문이다.
 * 따라서 실제 로그는 " nvme0n1: p1 p2" 형태가 되고, 이 함수가 이어붙이는
 * 조각은 " p1"이지 " nvme0n1p1"이 아니다. 이름이 숫자로 끝나지 않는
 * 디스크(sda 등)라면 덮어쓰기가 없어 " sda1"이 그대로 붙는다.
 * /dev 노드 이름을 만드는 것은 이 값이 아니라 add_partition()의
 * dev_set_name()이므로, 여기서 이름이 짧아져도 장치 이름에는 영향이 없다.
 *
 * 이때 flags/has_info/
 * info는 건드리지 않으므로, RAID 플래그나 UUID가 필요한 프로버는
 * put_partition() 호출과 별개로 p->parts[n].flags 등을 직접 대입해야
 * 한다.
 * 실행 컨텍스트: 파티션 스캔의 단일 프로세스 컨텍스트에서, 프로버 함수
 * 내부의 루프(파티션 테이블 엔트리 순회)에서 반복 호출된다.
 * 호출자: msdos_partition(), efi_partition(), mac_partition() 등 거의
 * 모든 포맷 프로버가 파티션을 하나 찾을 때마다 호출한다.
 * 피호출자: seq_buf_printf()(문자열 버퍼 API) 외에는 순수 필드 대입뿐이라
 * 추가 하위 호출은 없다.
 * 에러 처리: 명시적 에러 반환이 없으며, 범위 초과는 "조용한 무시"로
 * 처리된다 - 프로버가 계산 실수로 n을 과도하게 늘리더라도 커널이 죽지
 * 않도록 하는 방어적 설계다.
 *
 * 호출 체인:
 *   <포맷 프로버>(파티션 엔트리 파싱 루프) -> [put_partition]
 *     -> seq_buf_printf()
 */
static inline void
put_partition(struct parsed_partitions *p, int n, sector_t from, sector_t size)
{
	if (n < p->limit) {	/* [한국어] 슬롯 인덱스가 배열 범위 안일 때만 기록 - 손상된 테이블의 과도한 엔트리를 방어 */
		p->parts[n].from = from;	/* [한국어] 이 파티션의 시작 LBA 기록 */
		p->parts[n].size = size;	/* [한국어] 이 파티션의 길이(섹터 수) 기록 */
		seq_buf_printf(&p->pp_buf, " %s%d", p->name, n);	/* [한국어] 로그 문자열에 " <name><n>" 이어붙이기. name이 "p"로 덮어써진
								 * 경우(디스크 이름이 숫자로 끝날 때)에는 " p1"이 붙는다 — 위 함수 주석 참고 */
	}
}

/* detection routines go here in alphabetical order: */
/*
 * [한국어] 위 원본 주석 보강 - 파티션 포맷 검출기 20종의 공용 계약:
 *
 * 아래 모든 프로토타입은 동일한 인터페이스 계약을 따른다.
 * @state: 스캔 세션 상태(parsed_partitions). 각 프로버는 이 안의
 *   state->disk로 대상 gendisk를 얻고, read_part_sector()로 필요한
 *   섹터를 읽어 자신의 포맷 시그니처(매직 넘버, 체크섬 등)를 검사한 뒤,
 *   파티션을 찾을 때마다 put_partition()(및 필요 시 flags/info 직접
 *   대입)으로 결과를 state->parts[]에 적재한다.
 * @return: 1(또는 양수) = 이 포맷으로 성공적으로 인식하고 파싱을
 *   마쳤음(check_partition()의 루프가 즉시 종료됨). 0 = 이 포맷의
 *   시그니처가 아님(다음 프로버를 계속 시도). 음수(대개 -errno) = 포맷
 *   시그니처는 있었으나 섹터를 읽는 도중 I/O 오류가 발생했음(core.c가
 *   이 값을 기억해두었다가, 다른 모든 프로버도 실패하면 최종 에러로
 *   승격시켜 보고한다).
 * 각 프로버의 실제 파싱 로직(매직 넘버, CHS/LBA 변환, CRC 검증 등)은 이
 * 헤더가 아니라 동일 디렉터리의 각 .c 구현 파일에 있으며, 아래에는
 * 함수별로 담당 포맷과 구현 파일만 요약한다. core.c의 check_part[] 배열
 * 등록 순서는 알파벳 순이 아니라 Kconfig 의존성 선언 순서를 따르므로,
 * 아래 알파벳 순 나열은 어디까지나 "이 헤더에서 선언을 찾기 쉽게 하기
 * 위한" 정렬이며 실제 시도 순서와는 다를 수 있다.
 */
/*
 * [한국어]
 * adfspart_check_ADFS() - Acorn ADFS(Advanced Disc Filing System) 기본 파티션을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * Acorn 계열 컴퓨터의 ADFS 파일시스템이 사용하는 기본 파티션 배치를 검사한다.
 * 구현: block/partitions/acorn.c.
 * 호출 체인: check_partition() 루프 -> [adfspart_check_ADFS] -> put_partition()
 */
int adfspart_check_ADFS(struct parsed_partitions *state);
/*
 * [한국어]
 * adfspart_check_CUMANA() - Cumana SCSI 어댑터용 ADFS 파티션 배치를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * Cumana사의 SCSI 어댑터가 사용하던 ADFS 변형 파티션 테이블을 검사한다.
 * 구현: block/partitions/acorn.c.
 * 호출 체인: check_partition() 루프 -> [adfspart_check_CUMANA] -> put_partition()
 */
int adfspart_check_CUMANA(struct parsed_partitions *state);
/*
 * [한국어]
 * adfspart_check_EESOX() - EESOX SCSI 어댑터용 ADFS 파티션 배치를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * EESOX사의 SCSI 어댑터가 사용하던 ADFS 변형 파티션 테이블을 검사한다.
 * 구현: block/partitions/acorn.c.
 * 호출 체인: check_partition() 루프 -> [adfspart_check_EESOX] -> put_partition()
 */
int adfspart_check_EESOX(struct parsed_partitions *state);
/*
 * [한국어]
 * adfspart_check_ICS() - ICS(Integrated Control Systems) 어댑터용 ADFS 파티션 배치를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * ICS사의 IDE/SCSI 어댑터가 사용하던 ADFS 변형 파티션 테이블을 검사한다.
 * 구현: block/partitions/acorn.c.
 * 호출 체인: check_partition() 루프 -> [adfspart_check_ICS] -> put_partition()
 */
int adfspart_check_ICS(struct parsed_partitions *state);
/*
 * [한국어]
 * adfspart_check_POWERTEC() - PowerTec SCSI 어댑터용 ADFS 파티션 배치를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * PowerTec사의 SCSI 어댑터가 사용하던 ADFS 변형 파티션 테이블을 검사한다.
 * 구현: block/partitions/acorn.c.
 * 호출 체인: check_partition() 루프 -> [adfspart_check_POWERTEC] -> put_partition()
 */
int adfspart_check_POWERTEC(struct parsed_partitions *state);
/*
 * [한국어]
 * aix_partition() - AIX LVM(Logical Volume Manager) 파티션을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * IBM AIX가 사용하는 LVM 볼륨 그룹 디스크립터 영역을 읽어 파티션을 파싱한다.
 * 구현: block/partitions/aix.c.
 * 호출 체인: check_partition() 루프 -> [aix_partition] -> put_partition()
 */
int aix_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * amiga_partition() - Amiga RDB(Rigid Disk Block) 파티션을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * Commodore Amiga가 사용하는 RDB 헤더와 파티션 블록 체인을 파싱한다.
 * 구현: block/partitions/amiga.c.
 * 호출 체인: check_partition() 루프 -> [amiga_partition] -> put_partition()
 */
int amiga_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * atari_partition() - Atari ST 파티션(루트 섹터/XGM 확장)을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * Atari ST의 루트 섹터에 기록된 기본/확장(XGM) 파티션 엔트리를 파싱한다.
 * 구현: block/partitions/atari.c.
 * 호출 체인: check_partition() 루프 -> [atari_partition] -> put_partition()
 */
int atari_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * cmdline_partition() - 커널 부팅 커맨드라인으로 수동 지정된 파티션을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * 디스크 자체의 파티션 테이블을 읽는 대신, "blkdevparts=" 등 커널 커맨드
 * 라인 인자로 미리 지정된 (이름, 시작, 크기) 목록을 파싱해 파티션으로
 * 등록한다. 임베디드 기기에서 파티션 테이블이 없는 스토리지에 주로 사용.
 * 구현: block/partitions/cmdline.c.
 * 호출 체인: check_partition() 루프 -> [cmdline_partition] -> put_partition()
 */
int cmdline_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * efi_partition() - EFI GPT(GUID Partition Table)를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * LBA 1의 GPT 헤더와 파티션 엔트리 배열을 읽어 CRC32를 검증하고, 각
 * 파티션의 GUID/이름을 has_info/info에 채운다. 현대 x86/ARM 서버 및
 * NVMe SSD에서 가장 흔히 쓰이는 파티션 형식이다.
 * 구현: block/partitions/efi.c.
 * 호출 체인: check_partition() 루프 -> [efi_partition] -> put_partition()
 */
int efi_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * ibm_partition() - IBM DASD/CMS 볼륨 레이블 파티션(S/390)을 검출한다.
 * @return: 공용 계약(위 설명) 그대로. (원본 선언에는 파라미터 이름이
 *   생략되어 있으나 다른 프로버와 동일하게 parsed_partitions * 를 받는다.)
 * S/390 계열의 DASD(Direct Access Storage Device) 볼륨 레이블을 읽어
 * CMS/LDL/CDL 파티션 구조를 파싱한다.
 * 구현: block/partitions/ibm.c.
 * 호출 체인: check_partition() 루프 -> [ibm_partition] -> put_partition()
 */
int ibm_partition(struct parsed_partitions *);
/*
 * [한국어]
 * karma_partition() - Rio Karma MP3 플레이어 파티션을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * Rio Karma 휴대용 MP3 플레이어가 사용하는 전용 파티션 배치를 검사한다.
 * 구현: block/partitions/karma.c.
 * 호출 체인: check_partition() 루프 -> [karma_partition] -> put_partition()
 */
int karma_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * ldm_partition() - Windows LDM(Logical Disk Manager) 동적 디스크를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * Windows 동적 디스크의 PRIVHEAD/TOC(Table Of Contents)/VMDB 구조를 읽어
 * LDM 파티션을 파싱한다.
 * 구현: block/partitions/ldm.c.
 * 호출 체인: check_partition() 루프 -> [ldm_partition] -> put_partition()
 */
int ldm_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * mac_partition() - Apple 파티션 맵(APM, Apple Partition Map)을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * 옛 Mac OS/PowerPC Mac이 사용하는 드라이버 디스크립터 맵과 파티션 맵
 * 엔트리를 LBA 1부터 순회하며 파싱한다.
 * 구현: block/partitions/mac.c.
 * 호출 체인: check_partition() 루프 -> [mac_partition] -> put_partition()
 */
int mac_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * msdos_partition() - MBR(Master Boot Record, DOS 파티션 테이블)을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * LBA 0의 512바이트 부트섹터에서 0x55AA 시그니처를 확인하고, 4개의 기본
 * 파티션 엔트리와 확장 파티션(EBR) 체인을 순회하며 파싱한다. BSD/Solaris
 * 서브 파티션이 있으면 재귀적으로 추가 처리한다.
 * 구현: block/partitions/msdos.c.
 * 호출 체인: check_partition() 루프 -> [msdos_partition] -> put_partition()
 */
int msdos_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * of_partition() - PowerPC OpenFirmware가 노출한 파티션을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * OpenFirmware(구 Open Boot) 펌웨어가 디바이스 트리를 통해 미리 정의해 둔
 * 파티션 레이아웃을 읽어 등록한다.
 * 구현: block/partitions/of.c.
 * 호출 체인: check_partition() 루프 -> [of_partition] -> put_partition()
 */
int of_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * osf_partition() - OSF/1(Tru64 UNIX) 디스크 레이블을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * DEC OSF/1(이후 Tru64 UNIX)이 LBA 0 부근에 기록하는 디스크 레이블
 * 구조체를 읽어 파티션을 파싱한다.
 * 구현: block/partitions/osf.c.
 * 호출 체인: check_partition() 루프 -> [osf_partition] -> put_partition()
 */
int osf_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * sgi_partition() - SGI(Silicon Graphics) 볼륨 헤더를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * IRIX가 사용하는 SGI 볼륨 헤더(디스크 첫 섹터의 볼륨 디렉터리/파티션
 * 테이블)를 읽어 파싱한다.
 * 구현: block/partitions/sgi.c.
 * 호출 체인: check_partition() 루프 -> [sgi_partition] -> put_partition()
 */
int sgi_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * sun_partition() - Sun VTOC(Volume Table Of Contents)를 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * SPARC/Solaris가 사용하는 Sun 디스크 레이블과 VTOC 파티션 테이블을
 * 읽어 파싱한다.
 * 구현: block/partitions/sun.c.
 * 호출 체인: check_partition() 루프 -> [sun_partition] -> put_partition()
 */
int sun_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * sysv68_partition() - SysV68(Motorola System V/68) 파티션을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * Motorola System V/68 계열이 사용하던 파티션 테이블 형식을 읽어
 * 파싱한다.
 * 구현: block/partitions/sysv68.c.
 * 호출 체인: check_partition() 루프 -> [sysv68_partition] -> put_partition()
 */
int sysv68_partition(struct parsed_partitions *state);
/*
 * [한국어]
 * ultrix_partition() - DEC Ultrix 디스크 레이블을 검출한다.
 * @state/@return: 공용 계약(위 설명) 그대로.
 * DEC Ultrix가 사용하는 디스크 레이블 구조체를 읽어 파티션을 파싱한다.
 * 구현: block/partitions/ultrix.c.
 * 호출 체인: check_partition() 루프 -> [ultrix_partition] -> put_partition()
 */
int ultrix_partition(struct parsed_partitions *state);

/*
 * [한국어] 핵심 요약
 *
 * - 이 헤더는 파티션 "검출" 단계(원시 섹터 읽기 -> 포맷 시그니처 매칭 ->
 *   parts[] 적재)만을 다루며, 실제 block_device 생성/삭제는
 *   block/partitions/core.c의 add_partition()/blk_add_partition()이
 *   담당한다.
 * - struct parsed_partitions는 gendisk 하나당 스캔 1회분의 단명 상태이며,
 *   parts[]가 그 스캔에서 발견된 파티션 후보의 유일한 저장소다.
 * - read_part_sector()/put_dev_sector()는 항상 짝으로 쓰이는 folio
 *   획득/반납 쌍으로, 페이지 캐시(read_mapping_folio)를 경유하므로 캐시
 *   히트 시에는 실제 디스크 I/O 없이 섹터를 재사용할 수 있다.
 * - put_partition()은 limit 미만일 때만 등록하므로, 손상되거나 악의적인
 *   파티션 테이블이 parts[] 배열 경계를 넘어 쓰지 못하도록 막는 방어선
 *   역할도 겸한다.
 * - 20여 개의 <포맷>_partition()/adfspart_check_*() 함수는 모두 동일한
 *   (state, return 1/0/음수) 계약을 공유하므로, core.c의 check_partition()
 *   루프는 각 포맷의 내부 구현을 전혀 알 필요 없이 균일하게 순회할 수
 *   있다 - 이것이 이 헤더가 제공하는 추상화의 핵심이다.
 */
