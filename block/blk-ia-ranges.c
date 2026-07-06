// SPDX-License-Identifier: GPL-2.0
/*
 *  Block device concurrent positioning ranges.
 *
 *  Copyright (C) 2021 Western Digital Corporation or its Affiliates.
 */

/*
 * [한국어 설명] 블록 디바이스의 "독립 접근 범위(Independent Access Range, IAR)"를
 * sysfs에 등록/해제하고 그 유효성을 검증하는 코드 (block/blk-ia-ranges.c)
 *
 * === 파일의 역할 ===
 * 하나의 논리 블록 디바이스(struct gendisk, 예: /dev/sdX)가 내부적으로는
 * 서로 물리적으로 독립된 실행 자원(예: 멀티 액추에이터 HDD의 액추에이터별
 * 헤드 세트)으로 나뉘어, LBA(Logical Block Address) 구간별로 "병렬로"
 * 명령을 처리할 수 있는 경우가 있다. 이 파일은 그런 구간들의 집합
 * (struct blk_independent_access_ranges와 그 하위 struct
 * blk_independent_access_range 배열, 둘 다 include/linux/blkdev.h에 정의)을
 * 커널 내부에서 관리하고, /sys/block/<disk>/queue/independent_access_ranges/<N>/
 * {sector,nr_sectors} 형태의 읽기 전용 sysfs 속성으로 사용자 공간에 노출하는
 * 역할을 한다. 또한 드라이버가 보고한 범위 집합이 실제로 디스크 전체
 * 용량을 겹침·구멍 없이 정확히 커버하는지 검증하고(disk_check_ia_ranges),
 * revalidation 시 이전 설정과 실질적으로 달라졌는지 비교해(disk_ia_ranges_changed)
 * 불필요한 sysfs 재등록 비용을 피한다. 원본 커널 커밋 메시지와 파일 상단
 * 주석("concurrent positioning ranges")이 가리키듯, 이 기능의 1차 사용례는
 * SCSI/SAS 기반 멀티 액추에이터 HDD가 SBC-4 표준의 VPD(Vital Product Data)
 * 페이지 0xB9(Concurrent Positioning Ranges)로 보고하는 액추에이터별 LBA
 * 구간이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 블록 계층(block layer)의 "디스크 등록/해제" 단계와 "I/O
 * 스케줄러 초기화" 단계 사이에 위치하는 보조 인프라다. 실행 흐름은
 * 대략 다음과 같다.
 *   (생산자, 드라이버) VPD 0xB9 등으로 하드웨어에 질의
 *     -> disk_alloc_independent_access_ranges()로 컨테이너 할당
 *     -> disk_set_independent_access_ranges()로 disk에 설치
 *        (내부에서 disk_check_ia_ranges()/disk_ia_ranges_changed() 호출,
 *         필요 시 disk_unregister_/disk_register_independent_access_ranges()
 *         로 sysfs 재등록)
 *   (등록 경로) add_disk() -> blk_register_queue()
 *     -> disk_register_independent_access_ranges() (block/blk-sysfs.c에서 호출)
 *   (해제 경로) del_gendisk() -> blk_unregister_queue()
 *     -> disk_unregister_independent_access_ranges() (block/blk-sysfs.c에서 호출)
 *   (소비자, 이 저장소 안의 실제 예) elevator_init() -> bfq_init_queue()
 *     (block/bfq-iosched.c)가 q->disk->ia_ranges를 읽어 액추에이터별
 *     bfqd->sector[]/nr_sectors[] 배열(최대 BFQ_MAX_ACTUATORS개)을 채우고,
 *     BFQ가 액추에이터별로 독립적인 스케줄링 도메인을 운영하도록 한다.
 * 이 파일 자체는 항상 프로세스 컨텍스트(디스크 프로브/revalidate/스케줄러
 * 전환 경로, q->sysfs_lock을 잡을 수 있는 컨텍스트)에서만 실행되며, I/O
 * 완료 인터럽트나 hot I/O 경로(bio 제출/완료)에서는 호출되지 않는다 -
 * 즉 여기서 다루는 것은 "구성 정보의 등록"이지 "I/O 요청의 실제 분배"가
 * 아니다. 이 저장소(linux-study)는 drivers/nvme, drivers/pci 등 일부
 * 드라이버만 남긴 축소판이라 실제 IAR 생산자인 SCSI sd 드라이버
 * (drivers/scsi/sd.c의 sd_read_cpr())는 포함돼 있지 않으며, drivers/nvme
 * 안에도 이 IAR 메커니즘을 사용하는 코드는 없다(NVMe ZNS의 zone 개념과는
 * 별개의 기능) - 참고용 배경 지식으로만 남긴다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자) 방향: block/blk-sysfs.c의 blk_register_queue()/
 *   blk_unregister_queue()가 각각 disk_register_/disk_unregister_
 *   independent_access_ranges()를 호출해 disk->queue_kobj 하위에 sysfs
 *   트리를 붙이거나 뗀다. disk_alloc_/disk_set_independent_access_ranges()는
 *   EXPORT_SYMBOL_GPL로 내보내져 드라이버(대표적으로 SCSI sd)가 직접
 *   호출하는 공개 API다.
 * - 하위(피호출자) 방향: kobject_init_and_add()/kobject_del()/kobject_put()
 *   등 driver core의 kobject/sysfs 인프라, sysfs_ops.show 콜백 체계,
 *   kzalloc_node()/kfree() 슬랩 할당자를 사용한다.
 * - 소비자 방향: block/bfq-iosched.c가 q->disk->ia_ranges를 sysfs를 거치지
 *   않고 커널 내부에서 직접 읽어 액추에이터 인식 스케줄링에 사용한다(이
 *   구조체가 sysfs 등록 여부와 무관하게 커널 내부 자료구조로도 소비된다는
 *   점에 유의 - sysfs_registered는 오직 "이 sysfs 트리가 이미 붙어 있는가"
 *   만 나타낼 뿐, disk->ia_ranges 자체의 유효성과는 별개다).
 * - 데이터 흐름: 드라이버가 채운 sector/nr_sectors 값 -> disk->ia_ranges
 *   포인터에 저장 -> (a) sysfs 파일로 사용자 공간에 노출, (b) bfqd 배열로
 *   커널 내부에 복제. 즉 동일한 원본 데이터가 "사용자 공간 조회용"과
 *   "커널 내부 스케줄링용" 두 갈래로 흘러간다.
 * - 공유 자료구조: struct blk_independent_access_range{kobj, sector,
 *   nr_sectors}와 그 컨테이너 struct blk_independent_access_ranges{kobj,
 *   sysfs_registered, nr_ia_ranges, ia_range[]}(둘 다 include/linux/blkdev.h에
 *   정의되며, 이 파일은 정의하지 않고 사용만 한다). struct gendisk의
 *   ia_ranges 필드가 이 컨테이너에 대한 단일 소유(exclusive-owning) 포인터다.
 *
 * === 주요 함수/구조체 요약 ===
 * - disk_alloc_independent_access_ranges(): range 개수만큼 컨테이너와 배열을
 *   한 번의 kzalloc_node()로 할당(구조체 뒤에 가변 길이 배열을 붙이는
 *   flexible array member 패턴, struct_size()로 오버플로 없이 크기 계산).
 * - disk_set_independent_access_ranges(): 드라이버가 만든 새 range 집합을
 *   검증한 뒤 disk에 설치하고, 필요하면 sysfs를 갱신하는 최상위 진입점.
 * - disk_check_ia_ranges(): range들이 서로 겹치거나 구멍 없이 디스크 용량
 *   전체를 정확히 커버하는지 검사하면서, 동시에 배열을 LBA 오름차순으로
 *   제자리 정렬한다.
 * - disk_ia_ranges_changed(): 새 range 집합이 기존과 실질적으로 다른지
 *   비교해 불필요한 sysfs 재등록을 피한다.
 * - disk_register_/disk_unregister_independent_access_ranges(): 상위
 *   kobject(iars->kobj, "independent_access_ranges" 디렉터리)와 하위
 *   kobject(각 range를 나타내는 정수 이름 디렉터리 "0", "1", ...)를
 *   sysfs에 붙이거나 뗀다. 호출자는 반드시 q->sysfs_lock을 잡고 있어야
 *   한다.
 * - disk_find_ia_range(): 주어진 LBA가 속한 range를 선형 검색하며,
 *   disk_check_ia_ranges()의 정렬/검증 루프에서만 쓰인다.
 * - struct blk_ia_range_sysfs_entry: sysfs 속성 파일("sector",
 *   "nr_sectors") 하나하나를 attribute + show 콜백 쌍으로 표현하는 이
 *   파일 전용 로컬 구조체.
 */

#include <linux/kernel.h>
/* [한국어] sprintf(), pr_warn() 등 커널 전반의 기본 매크로/함수 선언 - 이
 * 파일의 sysfs show 콜백이 문자열을 만들 때와, 무결성 검증 실패 시 경고
 * 로그를 남길 때 사용한다. */
#include <linux/blkdev.h>
/* [한국어] struct gendisk, struct request_queue, struct
 * blk_independent_access_range(s) 등 이 파일이 사용하는 핵심 자료구조의
 * 실제 정의가 있는 헤더 - 이 파일은 그 정의를 include만 하고, 구조체 자체는
 * 여기서 다시 정의하지 않는다. */
#include <linux/slab.h>
/* [한국어] kzalloc_node()/kfree() 선언 - range 컨테이너를 disk->queue->node에
 * 맞춰 NUMA 지역성 있게 할당/해제할 때 사용한다. */
#include <linux/init.h>
/* [한국어] 커널 초기화 관련 매크로 모음 - 이 파일에 __init/__exit로 표시된
 * 함수는 없지만, 블록 계층 소스 파일들이 관례적으로 함께 포함하는 헤더다. */

#include "blk.h"
/* [한국어] block layer 내부(비공개) 선언 헤더 - blk_queue_registered()처럼
 * request_queue의 등록 상태(QUEUE_FLAG_REGISTERED)를 확인하는 내부 헬퍼가
 * 여기 선언돼 있으며, disk_set_independent_access_ranges()에서 사용한다. */

/*
 * [한국어]
 * blk_ia_range_sector_show - "sector" sysfs 속성의 show 콜백
 *
 * @iar: 값을 읽어올 대상 independent access range - kobject를 통해
 *       container_of()로 역참조된 struct blk_independent_access_range
 *       (include/linux/blkdev.h에 정의: kobj/sector/nr_sectors 3개 필드).
 * @buf: sysfs가 호출자에게 미리 마련해 준 PAGE_SIZE 크기의 버퍼 - 이 버퍼에
 *       문자열을 쓰면 그대로 사용자 공간 read()의 반환 데이터가 된다.
 * @return: buf에 쓴 바이트 수(성공 시 항상 양수) - sysfs_ops.show 콜백의
 *          계약(반환값 = 표시할 바이트 수)대로 sprintf()의 반환값을 그대로
 *          넘긴다.
 *
 * /sys/block/<disk>/queue/independent_access_ranges/<N>/sector 파일을
 * 사용자 공간에서 read()할 때 sysfs 코어가 최종적으로 호출하는 콜백이다.
 * iar->sector 필드(이 range가 시작하는 LBA)를 10진수 문자열로 그대로
 * 노출한다. 별도의 락 없이 값을 읽는데, 이는 이 필드가 disk_check_ia_ranges()
 * 에서 정렬·검증을 마친 뒤(즉 sysfs에 kobject가 등록되기 전) 한 번 확정되면
 * 더 이상 in-place로 바뀌지 않는 사실상 불변값이기 때문이다 - 값이 바뀌는
 * 유일한 경로는 disk_set_independent_access_ranges()가 구조체 전체를
 * 통째로 교체(재등록)하는 것뿐이다.
 * 실행 컨텍스트: 사용자 공간 read() 시스템 콜을 처리하는 프로세스 컨텍스트.
 * 호출자: blk_ia_range_sysfs_show()가 entry->show(iar, buf) 형태로 호출.
 * 피호출자: sprintf() 하나뿐, 별도 시스템 호출 없음.
 * 에러 경로: 이 함수 자체가 실패할 조건은 없다(sprintf가 버퍼 오버플로를
 * 일으키지 않는 한).
 *
 * 호출 체인:
 *   사용자 read() -> kernfs_fop_read -> blk_ia_range_sysfs_show
 *     -> [blk_ia_range_sector_show]
 */
static ssize_t
blk_ia_range_sector_show(struct blk_independent_access_range *iar,
			 char *buf)
{
	return sprintf(buf, "%llu\n", iar->sector);
	/* [한국어] iar->sector(이 range의 시작 LBA, 값 범위: 0 ~ 디스크 용량-1)를
	 * "%llu\n" 형식의 10진수 문자열로 buf에 기록해 그대로 반환한다.
	 * 설정자: disk_check_ia_ranges()의 정렬/검증 루프(swap() 포함)가 이
	 * 필드의 최종값을 확정한다. 읽는 자: 이 show 콜백(사용자 공간 read
	 * 경로)과, 커널 내부에서는 disk_find_ia_range()/disk_ia_ranges_changed()
	 * 가 비교·검색 목적으로 읽는다. 동기화: 별도 락 없음 - kobject가
	 * sysfs에 등록되기 전에 값이 확정되고 이후에는 갱신되지 않는 read-only
	 * 성격이기 때문. */
}

/*
 * [한국어]
 * blk_ia_range_nr_sectors_show - "nr_sectors" sysfs 속성의 show 콜백
 *
 * @iar: 값을 읽어올 대상 independent access range.
 * @buf: sysfs가 제공하는 출력 버퍼.
 * @return: buf에 쓴 바이트 수.
 *
 * /sys/block/<disk>/queue/independent_access_ranges/<N>/nr_sectors 파일을
 * read()할 때 호출되며, iar->nr_sectors(이 range의 길이, 섹터 단위)를
 * 10진수 문자열로 노출한다. blk_ia_range_sector_show()와 짝을 이루어,
 * 사용자 공간이 [iar->sector, iar->sector + iar->nr_sectors) 형태의 반개
 * 구간(half-open interval)을 재구성할 수 있게 한다.
 * 실행 컨텍스트: 사용자 공간 read() 시스템 콜을 처리하는 프로세스 컨텍스트.
 * 호출자: blk_ia_range_sysfs_show()가 entry->show(iar, buf) 형태로 호출.
 * 피호출자: sprintf() 하나뿐.
 * 에러 경로: 없음(항상 성공).
 *
 * 호출 체인:
 *   사용자 read() -> kernfs_fop_read -> blk_ia_range_sysfs_show
 *     -> [blk_ia_range_nr_sectors_show]
 */
static ssize_t
blk_ia_range_nr_sectors_show(struct blk_independent_access_range *iar,
			     char *buf)
{
	return sprintf(buf, "%llu\n", iar->nr_sectors);
	/* [한국어] iar->nr_sectors(이 range의 섹터 길이, 값 범위: 1 ~ 디스크
	 * 용량)를 문자열로 기록. 설정자: disk_check_ia_ranges()의 정렬/검증
	 * 루프(swap() 포함). 읽는 자: 이 show 콜백과 disk_find_ia_range()의
	 * 경계 계산(sector < iar->sector + iar->nr_sectors), 그리고
	 * disk_check_ia_ranges() 자신의 sector += iar->nr_sectors 누적.
	 * 동기화: sector 필드와 동일하게 등록 전 확정 후 불변. */
}

/*
 * [한국어]
 * struct blk_ia_range_sysfs_entry - sysfs 속성 파일 하나를 나타내는 로컬 구조체
 *
 * 이 파일에서만 쓰이는 내부 구조체로, "sector"/"nr_sectors" 두 sysfs 속성
 * 파일 각각을 attribute(이름/권한 메타데이터)와 show 콜백의 쌍으로 표현한다.
 * blk_ia_range_sysfs_show()가 이 구조체를 container_of()로 역참조해 알맞은
 * show 콜백을 찾아 호출하는 디스패치 테이블 역할을 한다.
 */
struct blk_ia_range_sysfs_entry {
	struct attribute attr;
	/* sysfs 속성의 이름과 권한 비트를 담는 커널 공용 구조체(struct
	 * attribute: name, mode 필드).
	 * 설정자: blk_ia_range_sector_entry/blk_ia_range_nr_sectors_entry의
	 * 정적 초기화 리터럴(.attr = { .name = "sector", .mode = 0444 } 형태)에서
	 * 컴파일 타임에 채워지며, 런타임에 값을 바꾸지 않는다.
	 * 읽는 자: sysfs core가 디렉터리 항목을 나열/오픈할 때 attr.name으로
	 * 파일을 식별하고, blk_ia_range_sysfs_show()가 container_of(attr, ...)
	 * 로 이 구조체 전체를 역참조할 때 진입점으로 쓰인다.
	 * 값 범위: name은 "sector" 또는 "nr_sectors" 중 하나, mode는 0444(모든
	 * 사용자 읽기 전용, 쓰기 불가 - IAR 정보는 하드웨어가 보고하는 값이므로
	 * 사용자 공간에서 변경할 수 없다).
	 * 동기화: 정적 const 데이터이므로 별도 동기화 불필요. */

	ssize_t (*show)(struct blk_independent_access_range *iar, char *buf);
	/* 실제 값을 문자열로 만들어 반환하는 함수 포인터
	 * (blk_ia_range_sector_show 또는 blk_ia_range_nr_sectors_show).
	 * 설정자: 두 static const entry의 .show 필드 초기화 리터럴에서 컴파일
	 * 타임에 고정.
	 * 읽는 자: blk_ia_range_sysfs_show()가 entry->show(iar, buf)로 호출.
	 * 값 범위: NULL이 아닌 유효한 함수 포인터 - blk_ia_range_attrs[]에 등록된
	 * 모든 entry는 반드시 show를 채운다(이 파일의 속성은 모두 읽기 전용이라
	 * store 콜백은 아예 존재하지 않는다).
	 * 동기화: 정적 const 데이터, 런타임 변경 없음. */
};

static const struct blk_ia_range_sysfs_entry blk_ia_range_sector_entry = {
	.attr = { .name = "sector", .mode = 0444 },
	/* [한국어] "sector"라는 이름의 읽기 전용(0444) sysfs 파일을 정의. */
	.show = blk_ia_range_sector_show,
	/* [한국어] 이 파일이 read()될 때 blk_ia_range_sector_show()를 호출하도록
	 * 연결. */
};

static const struct blk_ia_range_sysfs_entry blk_ia_range_nr_sectors_entry = {
	.attr = { .name = "nr_sectors", .mode = 0444 },
	/* [한국어] "nr_sectors"라는 이름의 읽기 전용(0444) sysfs 파일을 정의. */
	.show = blk_ia_range_nr_sectors_show,
	/* [한국어] 이 파일이 read()될 때 blk_ia_range_nr_sectors_show()를 호출하도록
	 * 연결. */
};

static const struct attribute *const blk_ia_range_attrs[] = {
	&blk_ia_range_sector_entry.attr,
	/* [한국어] "sector" 파일의 attribute 포인터 - ATTRIBUTE_GROUPS 매크로가
	 * 만드는 attribute_group의 .attrs 배열 원소가 된다. */
	&blk_ia_range_nr_sectors_entry.attr,
	/* [한국어] "nr_sectors" 파일의 attribute 포인터. */
	NULL,
	/* [한국어] attribute 배열의 끝을 표시하는 sentinel - sysfs core가 이
	 * NULL을 만날 때까지 배열을 순회한다. */
};
ATTRIBUTE_GROUPS(blk_ia_range);
/* [한국어] 매크로 전개 결과로 static const struct attribute_group
 * blk_ia_range_group = { .attrs = blk_ia_range_attrs }와, 그것 하나만 담은
 * static const struct attribute_group *blk_ia_range_groups[] = {
 * &blk_ia_range_group, NULL }을 생성한다. blk_ia_range_groups는 아래
 * blk_ia_range_ktype.default_groups로 연결되어, 각 range의 kobject가
 * kobject_init_and_add()될 때 "sector"/"nr_sectors" 두 파일이 자동으로
 * 함께 생성되게 한다. */

/*
 * [한국어]
 * blk_ia_range_sysfs_show - "sector"/"nr_sectors" 공통 sysfs read 진입점
 *
 * @kobj: read 대상 파일이 속한 kobject - struct blk_independent_access_range
 *        의 kobj 필드로 container_of() 역참조가 가능하다.
 * @attr: 읽으려는 속성(attribute) - "sector" 또는 "nr_sectors" 중 하나이며,
 *        blk_ia_range_sector_entry.attr 또는 blk_ia_range_nr_sectors_entry.attr
 *        의 주소가 그대로 전달된다.
 * @buf: 사용자 공간으로 복사될 값을 담을 출력 버퍼(PAGE_SIZE).
 * @return: entry->show()가 반환한, buf에 쓴 바이트 수.
 *
 * kobj_type.sysfs_ops.show로 등록된 공통 디스패처다. sysfs core는 어떤
 * 속성 파일이 read()됐는지와 상관없이 항상 이 함수 하나를 호출하며, 이
 * 함수는 attr을 container_of()로 되짚어 올바른 개별 show 콜백
 * (blk_ia_range_sector_show 또는 blk_ia_range_nr_sectors_show)을 찾아
 * 위임한다. 이런 "attr -> entry -> entry->show" 패턴은 struct
 * blk_ia_range_sysfs_entry가 attribute와 show 콜백을 한 쌍으로 묶어두었기
 * 때문에 가능하다.
 * 실행 컨텍스트: 사용자 공간 read() 시스템 콜을 처리하는 프로세스 컨텍스트,
 * 재진입 걱정 없음(양쪽 container_of 모두 읽기 전용 포인터 연산).
 * 호출자: sysfs/kernfs core(kernfs_fop_read 등)가 kobj_type.sysfs_ops.show를
 * 통해 호출.
 * 피호출자: blk_ia_range_sector_show() 또는 blk_ia_range_nr_sectors_show().
 * 에러 경로: 이 함수 자체는 실패하지 않음 - 잘못된 attr이 들어올 수 없도록
 * blk_ia_range_attrs[]가 정적으로 고정돼 있다.
 *
 * 호출 체인:
 *   사용자 read() -> kernfs_fop_read -> [blk_ia_range_sysfs_show]
 *     -> blk_ia_range_sector_show / blk_ia_range_nr_sectors_show
 */
static ssize_t blk_ia_range_sysfs_show(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	struct blk_ia_range_sysfs_entry *entry =
		container_of(attr, struct blk_ia_range_sysfs_entry, attr);
	/* [한국어] attr(요청된 파일의 attribute 포인터)이 실제로는
	 * blk_ia_range_sector_entry.attr 또는 blk_ia_range_nr_sectors_entry.attr
	 * 이라는 사실을 이용해, attr이 임베드된 바깥 struct
	 * blk_ia_range_sysfs_entry 전체의 주소를 역산한다(container_of는
	 * 포인터 산술: attr 주소 - offsetof(struct, attr)). 이를 통해 entry->show로
	 * 올바른 콜백에 접근할 수 있게 된다. */
	struct blk_independent_access_range *iar =
		container_of(kobj, struct blk_independent_access_range, kobj);
	/* [한국어] kobj(요청된 파일이 속한 디렉터리의 kobject)가 실제로는
	 * struct blk_independent_access_range의 kobj 필드라는 사실을 이용해
	 * 바깥 range 구조체 전체를 역참조한다. 이 kobj는
	 * disk_register_independent_access_ranges()가 &iars->ia_range[i].kobj로
	 * kobject_init_and_add()했던 바로 그 kobject다. */

	return entry->show(iar, buf);
	/* [한국어] 실제 값 포맷팅은 개별 show 콜백에 위임 - 반환값은 sysfs read()
	 * 시스템 콜의 결과(읽은 바이트 수)로 그대로 사용자 공간에 전달된다. */
}

static const struct sysfs_ops blk_ia_range_sysfs_ops = {
	.show	= blk_ia_range_sysfs_show,
	/* [한국어] 이 kobj_type을 쓰는 모든 kobject(각 range 디렉터리)의 read
	 * 요청이 공통적으로 blk_ia_range_sysfs_show()로 라우팅되도록 지정.
	 * .store 콜백은 정의하지 않음 - 모든 속성이 0444(읽기 전용)이므로 쓰기
	 * 경로 자체가 존재하지 않는다. */
};

/*
 * Independent access range entries are not freed individually, but alltogether
 * with struct blk_independent_access_ranges and its array of ranges. Since
 * kobject_add() takes a reference on the parent kobject contained in
 * struct blk_independent_access_ranges, the array of independent access range
 * entries cannot be freed until kobject_del() is called for all entries.
 * So we do not need to do anything here, but still need this no-op release
 * operation to avoid complaints from the kobject code.
 */
/*
 * [한국어]
 * blk_ia_range_sysfs_nop_release - 개별 range kobject의 release 콜백(no-op)
 *
 * @kobj: reference count가 0이 되어 해제 절차에 들어간 kobject(개별 range).
 * @return: 없음(void).
 *
 * kobject 코어는 모든 kobj_type이 .release를 채우도록 강제하지만(비워두면
 * WARN 발생), 개별 range 항목은 독립적으로 kfree()할 대상이 아니다 - 이
 * range들은 struct blk_independent_access_ranges가 kzalloc_node() 한 번으로
 * 통째로 할당한 배열(iars->ia_range[])의 원소이기 때문에, 배열 하나하나를
 * 따로 해제하면 안 되고 컨테이너 전체가 한 번에 kfree()되어야 한다. 바로
 * 위 원본 영어 주석이 설명하듯, kobject_add()가 부모(iars->kobj)에 대한
 * 참조를 잡고 있으므로 이 배열은 모든 range 항목이 kobject_del()되기
 * 전까지는 어차피 해제될 수 없다. 따라서 이 콜백은 kobject 서브시스템의
 * "release 콜백 필수" 요구를 만족시키기 위한 형식적 no-op일 뿐이다.
 * 실행 컨텍스트: kobject_put()이 마지막 참조를 놓는 순간 호출되며, 보통
 * disk_unregister_independent_access_ranges()가 kobject_del() 직후
 * 호출되는 흐름 안에서 프로세스 컨텍스트로 실행된다.
 * 호출자: kobject_put(&iars->ia_range[i].kobj)의 참조 카운트가 0이 될 때
 * kobject 코어가 내부적으로 호출.
 * 피호출자: 없음.
 * 에러 경로: 없음 - 항상 성공(아무 일도 하지 않으므로).
 *
 * 호출 체인:
 *   kobject_put(range kobj) -> kref_put -> [blk_ia_range_sysfs_nop_release]
 */
static void blk_ia_range_sysfs_nop_release(struct kobject *kobj)
{
	/* [한국어] 의도적으로 비워둠 - 실제 메모리 해제는 상위 컨테이너의 release인
	 * blk_ia_ranges_sysfs_release()가 한 번에 수행한다. */
}

static const struct kobj_type blk_ia_range_ktype = {
	.sysfs_ops	= &blk_ia_range_sysfs_ops,
	/* [한국어] 이 kobj_type을 쓰는 kobject(각 range)의 sysfs read를
	 * blk_ia_range_sysfs_show()로 연결. */
	.default_groups	= blk_ia_range_groups,
	/* [한국어] kobject_init_and_add() 시 "sector"/"nr_sectors" 두 속성
	 * 파일이 자동으로 함께 생성되도록 지정(ATTRIBUTE_GROUPS 매크로가 만든
	 * 배열). */
	.release	= blk_ia_range_sysfs_nop_release,
	/* [한국어] 참조 카운트가 0이 될 때 호출되는 콜백 - 위에서 설명한 대로
	 * no-op. */
};

/*
 * This will be executed only after all independent access range entries are
 * removed with kobject_del(), at which point, it is safe to free everything,
 * including the array of ranges.
 */
/*
 * [한국어]
 * blk_ia_ranges_sysfs_release - 상위(컨테이너) kobject의 release 콜백
 *
 * @kobj: reference count가 0이 되어 해제 절차에 들어간 상위 kobject
 *        (iars->kobj, "independent_access_ranges" 디렉터리 자체).
 * @return: 없음(void).
 *
 * 바로 위 원본 영어 주석대로, 이 콜백은 모든 하위 range kobject가
 * kobject_del()로 제거된 뒤에만(즉 더 이상 어떤 하위 항목도 이 부모에
 * 대한 참조를 들고 있지 않을 때만) 호출된다는 것이 보장되므로, 이 시점에
 * ia_range[] 배열을 포함한 struct blk_independent_access_ranges 전체를
 * 안전하게 kfree()할 수 있다. 이것이 disk->ia_ranges가 가리키는 메모리가
 * 실제로 해제되는 유일한 경로 중 하나다(다른 하나는 sysfs에 한 번도
 * 등록되지 않은 채 버려지는 disk_unregister_independent_access_ranges()의
 * else 분기).
 * 실행 컨텍스트: kobject_put(&iars->kobj)의 참조가 마지막으로 소멸하는
 * 순간, 프로세스 컨텍스트에서 호출.
 * 호출자: disk_unregister_independent_access_ranges()가 kobject_del() 직후
 * 호출하는 kobject_put(&iars->kobj)에서 참조 카운트가 0이 될 때 kobject
 * 코어가 호출.
 * 피호출자: kfree().
 * 에러 경로: 없음 - kfree()는 실패하지 않는다.
 *
 * 호출 체인:
 *   kobject_put(&iars->kobj) -> kref_put -> [blk_ia_ranges_sysfs_release]
 *     -> kfree
 */
static void blk_ia_ranges_sysfs_release(struct kobject *kobj)
{
	struct blk_independent_access_ranges *iars =
		container_of(kobj, struct blk_independent_access_ranges, kobj);
	/* [한국어] kobj(상위 "independent_access_ranges" 디렉터리의 kobject)로부터
	 * 이를 감싸는 struct blk_independent_access_ranges 전체 주소를
	 * container_of()로 역산 - 이 포인터가 곧 kfree()로 해제할 메모리 블록의
	 * 시작 주소(disk_alloc_independent_access_ranges()가 kzalloc_node()로
	 * 할당한 바로 그 블록)다. */

	kfree(iars);
	/* [한국어] iars(및 그 뒤에 붙은 가변 길이 배열 ia_range[] 전체)를 한 번에
	 * 해제한다 - kzalloc_node(struct_size(...))로 컨테이너와 배열을 하나의
	 * 연속된 메모리로 할당했기 때문에 별도로 배열만 따로 해제할 필요가
	 * 없다. */
}

static const struct kobj_type blk_ia_ranges_ktype = {
	.release	= blk_ia_ranges_sysfs_release,
	/* [한국어] 상위 kobject의 참조가 소멸할 때 호출될 release 콜백만 지정한다.
	 * .sysfs_ops/.default_groups는 의도적으로 채우지 않음 - 이 상위
	 * kobject("independent_access_ranges" 디렉터리) 자체는 자신의 속성
	 * 파일을 갖지 않고, 오직 하위 range 디렉터리들을 담는 컨테이너 역할만
	 * 하기 때문이다. */
};

/**
 * disk_register_independent_access_ranges - register with sysfs a set of
 *		independent access ranges
 * @disk:	Target disk
 *
 * Register with sysfs a set of independent access ranges for @disk.
 */
/*
 * [한국어]
 * disk_register_independent_access_ranges - IAR sysfs 트리 등록
 *
 * @disk: 대상 gendisk - disk->ia_ranges에 등록할 range 정보가, disk->queue와
 *        disk->queue_kobj에 등록 위치(부모 kobject)가 들어 있다.
 * @return: 0(성공, disk->ia_ranges가 NULL이라 할 일이 없었던 경우도 포함),
 *          음수 errno(kobject_init_and_add() 실패 시 그 에러 코드).
 *
 * disk->ia_ranges에 이미 채워져 있는 range 집합을
 * /sys/block/<disk>/queue/independent_access_ranges/ 디렉터리와 그 아래
 * "0", "1", ... 형태의 하위 디렉터리(각각 sector/nr_sectors 파일 포함)로
 * sysfs에 실체화한다. 상위 kobject 하나(iars->kobj)를 disk->queue_kobj의
 * 자식으로 먼저 등록한 뒤, nr_ia_ranges개의 하위 kobject를 그 아래 순서대로
 * 등록하는 2단계 구조다. 중간에 실패하면 이미 추가한 하위 kobject들을
 * 역순으로 되돌려(kobject_del) sysfs에 절반만 노출되는 상태를 만들지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 호출자가 q->sysfs_lock을 잡고 있어야
 * 한다(lockdep_assert_held로 강제) - block/blk-sysfs.c의 blk_register_queue()
 * 경로(add_disk() 중)와, disk_set_independent_access_ranges()가 이미 등록된
 * queue를 재검증(revalidate)하는 경로 두 곳에서 이 락을 쥔 채로 호출한다.
 * 호출자: block/blk-sysfs.c의 register_disk() 경로(add_disk 중),
 * disk_set_independent_access_ranges()(revalidation 시 queue가 이미
 * 등록돼 있으면).
 * 피호출자: kobject_init_and_add(), kobject_del(), kobject_put().
 * 에러 경로: 상위 kobject_init_and_add() 실패 시 disk->ia_ranges를 NULL로
 * 되돌리고 참조를 놓은 뒤 즉시 반환. 하위 kobject_init_and_add() 실패
 * 시에는 이미 추가된 하위 항목들을 역순으로 kobject_del()한 뒤 상위
 * kobject까지 정리하고 반환 - 이 시점에는 disk->ia_ranges가 이미 iars를
 * 가리키고 있지 않을 수 있음에 유의(호출자인
 * disk_set_independent_access_ranges()가 disk->ia_ranges = iars 대입을
 * 이 함수 호출보다 먼저 하므로, 실패 시에도 포인터 자체는 남지만 sysfs와의
 * 연결은 끊긴 상태가 된다).
 *
 * 호출 체인:
 *   add_disk -> blk_register_queue -> [disk_register_independent_access_ranges]
 *     -> kobject_init_and_add
 */
int disk_register_independent_access_ranges(struct gendisk *disk)
{
	struct blk_independent_access_ranges *iars = disk->ia_ranges;
	/* [한국어] disk에 이미 연결된 range 컨테이너 포인터를 지역 변수로 캐시
	 * (disk_set_independent_access_ranges()가 disk->ia_ranges = iars 대입을
	 * 먼저 해 둔 뒤 이 함수를 호출하는 흐름을 전제). */
	struct request_queue *q = disk->queue;
	/* [한국어] 이 disk의 request_queue - sysfs_lock을 검증하고,
	 * disk->queue_kobj(아래 kobject_init_and_add의 부모)에 접근하기 위해
	 * 필요. */
	int i, ret;
	/* [한국어] i: 하위 range 순회 인덱스이자 sysfs 디렉터리 이름("%d"). ret:
	 * kobject_init_and_add()의 반환값(errno)을 임시 저장. */

	lockdep_assert_held(&q->sysfs_lock);
	/* [한국어] 런타임 락 보유 여부를 lockdep으로 검증만 하는 디버그 도구 -
	 * 실제로 락을 걸지는 않는다. q->sysfs_lock은 이 sysfs 트리를
	 * 등록/해제/조회하는 여러 경로(add_disk, del_gendisk,
	 * disk_set_independent_access_ranges) 사이의 상호 배제를 보장하는
	 * mutex이며, 호출자가 이미 잡고 있어야 한다는 계약을 위반하면 여기서
	 * 즉시 경고가 뜬다. */

	if (!iars)
		return 0;
	/* [한국어] disk->ia_ranges가 NULL이면(즉 이 disk가 독립 접근 범위를 아예
	 * 보고하지 않는 일반 디스크라면) 등록할 것이 없으므로 조용히 성공
	 * 반환 - 이는 에러가 아니라 "해당 없음"을 뜻한다. */

	/*
	 * At this point, iars is the new set of sector access ranges that needs
	 * to be registered with sysfs.
	 */
	WARN_ON(iars->sysfs_registered);
	/* [한국어] 이미 등록된 상태(sysfs_registered == true)에서 다시
	 * 등록하려는 시도는 호출자 측 로직 오류이므로 경고만 남기고(치명적
	 * 오류로 막지는 않음) 계속 진행한다 - 정상 흐름에서는
	 * disk_unregister_independent_access_ranges()로 먼저 정리한 뒤에만
	 * 이 함수가 호출돼야 한다. */
	ret = kobject_init_and_add(&iars->kobj, &blk_ia_ranges_ktype,
				   &disk->queue_kobj, "%s",
				   "independent_access_ranges");
	/* [한국어] 상위 kobject(iars->kobj)를 disk->queue_kobj의 자식으로
	 * 초기화하고 sysfs에 "independent_access_ranges"라는 이름의 디렉터리로
	 * 추가한다 - 즉 /sys/block/<disk>/queue/independent_access_ranges/가
	 * 이 호출로 생성된다. blk_ia_ranges_ktype이므로 이 디렉터리 자체에는
	 * 속성 파일이 없다(release 콜백만 존재). */
	if (ret) {
		disk->ia_ranges = NULL;
		/* [한국어] 등록 실패 시 disk에서 range 정보를 즉시 끊어, 이후
		 * 코드가 절반만 초기화된 iars를 유효한 것으로 오인해 참조하는
		 * 일이 없도록 한다. */
		kobject_put(&iars->kobj);
		/* [한국어] kobject_init_and_add() 실패 시에도 kobject_init()
		 * 단계는 이미 참조 카운트를 1로 설정해 두므로, kobject_put()으로
		 * 그 참조를 반드시 놓아줘야 blk_ia_ranges_sysfs_release()를 거쳐
		 * iars가 최종적으로 kfree()된다. */
		return ret;
		/* [한국어] 에러 코드를 그대로 호출자에게 전달 - add_disk() 경로라면
		 * 디스크 등록 자체가 실패하고, revalidation 경로라면
		 * disk_set_independent_access_ranges()의 반환 없는 void
		 * 함수이므로 이 실패가 조용히 로그로만 남는다. */
	}

	for (i = 0; i < iars->nr_ia_ranges; i++) {
		/* [한국어] disk_check_ia_ranges()가 이미 LBA 오름차순으로 정렬해
		 * 둔 nr_ia_ranges개의 range를 순서대로 순회하며 각각을 하위
		 * kobject로 등록한다 - 인덱스 i가 그대로 sysfs 디렉터리 이름이
		 * 되므로, 등록 후에는 "0" 디렉터리가 가장 낮은 LBA range를
		 * 가리키는 것이 보장된다. */
		ret = kobject_init_and_add(&iars->ia_range[i].kobj,
					   &blk_ia_range_ktype, &iars->kobj,
					   "%d", i);
		/* [한국어] i번째 range의 kobj를 상위 iars->kobj의 자식으로 초기화하고
		 * sysfs에 정수 이름("0", "1", ...)의 디렉터리로 추가 - blk_ia_range_ktype
		 * 이므로 이 디렉터리 아래에 default_groups로 지정된 "sector"/
		 * "nr_sectors" 파일이 함께 생성된다. */
		if (ret) {
			while (--i >= 0)
				kobject_del(&iars->ia_range[i].kobj);
			/* [한국어] i번째 등록이 실패했으므로, 그 이전에 이미 성공적으로
			 * 추가된 0..i-1번 range 디렉터리들을 역순으로 kobject_del()해
			 * sysfs에서 제거한다 - 일부 range만 노출된 채로 남으면
			 * 사용자 공간이 불완전한 병렬 구성 정보를 읽게 되므로
			 * 전부 되돌린다. */
			kobject_del(&iars->kobj);
			/* [한국어] 하위 항목을 모두 정리한 뒤, 상위
			 * "independent_access_ranges" 디렉터리 자체도 제거. */
			kobject_put(&iars->kobj);
			/* [한국어] 상위 kobject의 마지막 참조를 놓아 release
			 * (blk_ia_ranges_sysfs_release)를 통해 iars 메모리를 반납할
			 * 수 있게 한다. */
			return ret;
			/* [한국어] 실패 코드를 호출자에게 전달. */
		}
	}

	iars->sysfs_registered = true;
	/* [한국어] 모든 하위 range까지 등록이 끝났음을 표시 - 이 플래그는
	 * disk_unregister_independent_access_ranges()가 "sysfs에서 실제로
	 * 지워야 할 것이 있는지" 판단하는 기준이 된다. */

	return 0;
	/* [한국어] 전체 등록 성공. */
}

/*
 * [한국어]
 * disk_unregister_independent_access_ranges - IAR sysfs 트리 등록 해제
 *
 * @disk: 대상 gendisk.
 * @return: 없음(void).
 *
 * sysfs에서 independent_access_ranges 디렉터리와 그 하위 range 디렉터리들을
 * 제거한다. 이미 sysfs에 등록된 상태(iars->sysfs_registered)라면 하위
 * kobject들을 먼저 kobject_del()한 뒤 상위 kobject를 del/put해 release
 * 콜백 체인을 통해 메모리까지 반납되게 하고, 아직 sysfs에 한 번도 등록되지
 * 않은 상태(예: disk_check_ia_ranges() 검증에는 통과했지만 아직
 * disk_register_independent_access_ranges()를 거치지 않은 경우는 이 경로로
 * 오지 않지만, disk_set_independent_access_ranges()가 등록 전에 이전 값을
 * 정리하려고 부를 수도 있는 일반적인 "정리" 함수이므로)라면 kobject
 * 인프라를 거치지 않고 바로 kfree()한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 호출자가 q->sysfs_lock을 잡고 있어야
 * 한다(lockdep_assert_held로 강제).
 * 호출자: block/blk-sysfs.c의 blk_unregister_queue()(del_gendisk 경로),
 * disk_set_independent_access_ranges()(새 range를 설치하기 전에 기존 것을
 * 먼저 치우는 용도).
 * 피호출자: kobject_del(), kobject_put(), kfree().
 * 에러 경로: 이 함수는 실패를 반환하지 않는다(void) - kobject 삭제/kfree는
 * 실패하지 않는 연산이기 때문.
 *
 * 호출 체인:
 *   del_gendisk -> blk_unregister_queue -> [disk_unregister_independent_access_ranges]
 */
void disk_unregister_independent_access_ranges(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	/* [한국어] sysfs_lock 검증을 위한 request_queue 포인터. */
	struct blk_independent_access_ranges *iars = disk->ia_ranges;
	/* [한국어] 해제 대상 range 컨테이너 - disk에서 분리하기 전에 지역 변수로
	 * 보관해 둔다. */
	int i;
	/* [한국어] 하위 range 순회 인덱스. */

	lockdep_assert_held(&q->sysfs_lock);
	/* [한국어] 등록 함수와 동일하게, 이 함수도 q->sysfs_lock을 쥔 채로만
	 * 호출되어야 한다는 것을 런타임에 검증. */

	if (!iars)
		return;
	/* [한국어] 애초에 range 정보가 없던 disk라면 정리할 것도 없으므로 즉시
	 * 반환. */

	if (iars->sysfs_registered) {
		for (i = 0; i < iars->nr_ia_ranges; i++)
			kobject_del(&iars->ia_range[i].kobj);
		/* [한국어] sysfs에 노출됐던 모든 하위 range 디렉터리를 먼저
		 * kobject_del()로 제거 - 순서를 지키는 이유는 상위 디렉터리를
		 * 먼저 지우면 커널 kobject 트리 불변식(자식보다 부모를 먼저
		 * 지우지 않는다)이 깨질 수 있기 때문. */
		kobject_del(&iars->kobj);
		/* [한국어] 하위 항목을 모두 지운 뒤 상위
		 * "independent_access_ranges" 디렉터리 자체를 제거. */
		kobject_put(&iars->kobj);
		/* [한국어] 상위 kobject의 참조를 놓는다 - 이 시점에 참조 카운트가
		 * 0이 되면 blk_ia_ranges_sysfs_release()가 실행되어 iars 전체가
		 * kfree()된다(하위 kobject들은 blk_ia_range_sysfs_nop_release로
		 * no-op이므로, 실제 해제는 여기서 한 번에 일어난다). */
	} else {
		kfree(iars);
		/* [한국어] sysfs에 등록된 적이 없으므로 kobject 삭제 절차를 거칠
		 * 필요 없이 곧바로 메모리를 반납 - disk_set_independent_access_ranges()
		 * 가 아직 request_queue가 등록되지 않은 상태에서 range를 교체할 때
		 * 이 분기를 탈 수 있다. */
	}

	disk->ia_ranges = NULL;
	/* [한국어] disk에서 range 컨테이너에 대한 참조를 끊는다 - 이 시점 이후
	 * disk->ia_ranges를 읽는 코드(예: bfq_init_queue())는 "이 disk는 독립
	 * 접근 범위를 보고하지 않는다"로 취급하게 된다. */
}

/*
 * [한국어]
 * disk_find_ia_range - 주어진 sector가 속한 range를 선형 검색
 *
 * @iars: 검색 대상 independent access ranges 집합.
 * @sector: 찾으려는 LBA(논리 블록 주소).
 * @return: sector를 포함하는 struct blk_independent_access_range 포인터,
 *          일치하는 range가 없으면 NULL.
 *
 * iars->ia_range[0..nr_ia_ranges) 배열을 처음부터 끝까지 선형 탐색하며,
 * [iar->sector, iar->sector + iar->nr_sectors) 반개 구간에 sector가
 * 포함되는 원소를 찾는다. 오직 disk_check_ia_ranges()의 정렬/검증 루프
 * 내부에서만 호출되는 정적(static, 파일-로컬) 헬퍼이며, 그 루프가 매
 * 반복마다 "현재까지 정렬된 부분과 아직 정렬되지 않은 부분이 섞인" 배열
 * 전체를 대상으로 이 함수를 호출하기 때문에 배열이 완전히 정렬되어 있다는
 * 가정을 하지 않는다(단순 선형 스캔, 이분 탐색이 아님).
 * 실행 컨텍스트: 프로세스 컨텍스트, disk_check_ia_ranges() 호출자와 동일한
 * q->sysfs_lock 보호 아래에서 실행된다(직접 락을 걸지는 않음).
 * 호출자: disk_check_ia_ranges()의 정렬/검증 for 루프.
 * 피호출자: 없음(순수 배열 순회).
 * 에러 경로: 없음 - 못 찾으면 NULL을 반환할 뿐 별도 에러 코드는 없다.
 *
 * 호출 체인:
 *   disk_set_independent_access_ranges -> disk_check_ia_ranges
 *     -> [disk_find_ia_range]
 */
static struct blk_independent_access_range *
disk_find_ia_range(struct blk_independent_access_ranges *iars,
		  sector_t sector)
{
	struct blk_independent_access_range *iar;
	/* [한국어] 순회 중 임시로 가리킬 range 포인터. */
	int i;
	/* [한국어] 배열 순회 인덱스. */

	for (i = 0; i < iars->nr_ia_ranges; i++) {
		/* [한국어] 0번부터 nr_ia_ranges-1번까지 모든 range를 검사 - 정렬
		 * 여부를 가정하지 않으므로 이분 탐색이 아닌 선형 탐색. */
		iar = &iars->ia_range[i];
		/* [한국어] i번째 range의 주소를 가져온다. */
		if (sector >= iar->sector &&
		    sector < iar->sector + iar->nr_sectors)
			return iar;
		/* [한국어] sector가 [iar->sector, iar->sector + iar->nr_sectors)
		 * 구간에 들어오면 이 range가 정답 - 즉시 반환. */
	}

	return NULL;
	/* [한국어] 모든 range를 검사했지만 포함하는 것이 없으면 NULL - 정상
	 * 동작에서는 disk_check_ia_ranges()가 이 함수를 호출하기 전에 이미
	 * "sector"가 어떤 range의 시작점이어야 한다"는 불변식을 기대하므로,
	 * NULL이 반환되면 range 구성에 구멍(hole)이 있다는 뜻이다. */
}

/*
 * [한국어]
 * disk_check_ia_ranges - IAR 집합의 무결성 검사 및 LBA 오름차순 제자리 정렬
 *
 * @disk: 대상 gendisk - get_capacity(disk)로 전체 용량과 비교하는 데 쓰인다.
 * @iars: 검사·정렬할 independent access ranges 집합(입력이자 출력 - 정렬
 *        결과가 iars->ia_range[] 배열에 그대로 반영된다).
 * @return: true(모든 검사를 통과한 유효한 range 집합), false(무효 - 겹침,
 *          구멍, 또는 용량 불일치가 하나라도 있으면).
 *
 * 이 함수는 검증과 정렬을 한 번의 순회로 동시에 수행하는 선택 정렬
 * (selection sort) 변형이다: 매 반복 i(0부터 nr_ia_ranges-1까지)마다 "지금까지
 * 누적된 LBA 오프셋(sector 지역 변수)"에서 시작하는 range를
 * disk_find_ia_range()로 배열 전체에서 찾는다. 만약 그런 range가 없거나
 * (tmp == NULL, 구멍 발생) 찾은 range의 시작 LBA가 기대값과 정확히 일치하지
 * 않으면(겹침 또는 구멍) 즉시 실패 처리한다. 찾은 range(tmp)가 현재 정렬
 * 위치(iar = &iars->ia_range[i])와 다른 원소라면, 두 원소의 값(sector,
 * nr_sectors)만 swap()으로 맞바꿔 배열을 제자리에서 LBA 오름차순으로
 * 재배열한다(이 시점에는 아직 kobject_init_and_add()가 호출되기 전이므로
 * kobj 필드까지 swap할 필요가 없다 - 어차피 초기화되지 않은 상태). 마지막에
 * 누적된 sector 총합이 디스크 전체 용량(get_capacity)과 정확히 같은지
 * 검사해, range들이 디스크 전체를 빠짐없이 덮는지 최종 확인한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, disk_set_independent_access_ranges()가
 * q->sysfs_lock을 쥔 채로 호출.
 * 호출자: disk_set_independent_access_ranges().
 * 피호출자: get_capacity(), WARN_ON_ONCE(), disk_find_ia_range(), pr_warn(),
 * swap()(매크로, 함수 호출 아님).
 * 에러 경로: 검사 실패 시 pr_warn()으로 원인을 로그에 남기고 false를 반환 -
 * 호출자인 disk_set_independent_access_ranges()는 이 경우 iars를 폐기
 * (kfree)하고 disk에 설치하지 않는다.
 *
 * 호출 체인:
 *   disk_set_independent_access_ranges -> [disk_check_ia_ranges]
 *     -> disk_find_ia_range
 */
static bool disk_check_ia_ranges(struct gendisk *disk,
				struct blk_independent_access_ranges *iars)
{
	struct blk_independent_access_range *iar, *tmp;
	/* [한국어] iar: 현재 정렬 위치(i번째 슬롯)를 가리키는 포인터. tmp:
	 * disk_find_ia_range()가 찾아준, "기대하는 다음 LBA"에서 시작하는
	 * range를 가리키는 포인터. */
	sector_t capacity = get_capacity(disk);
	/* [한국어] 디스크 전체 섹터 수 - 모든 range의 nr_sectors 합과 최종적으로
	 * 비교할 기준값. get_capacity()는 disk->part0(전체 디스크를 나타내는
	 * 0번 파티션)의 크기를 반환한다. */
	sector_t sector = 0;
	/* [한국어] 지금까지 검증된 range들이 커버한 LBA의 끝(=다음에 나와야 할
	 * range의 시작 LBA) - 0(디스크 맨 앞)부터 시작해 반복마다 누적된다. */
	int i;
	/* [한국어] 정렬/검증 루프의 반복 인덱스. */

	if (WARN_ON_ONCE(!iars->nr_ia_ranges))
		return false;
	/* [한국어] range 개수가 0인 것은 호출자(disk_alloc_independent_access_ranges
	 * 를 잘못 쓴 드라이버) 측 버그이므로, 커널 로그에 한 번만(ONCE) 경고를
	 * 남기고 무효 처리한다 - range가 하나도 없는 집합은 "독립 접근 범위가
	 * 있다"고 disk_set_independent_access_ranges()를 호출한 것 자체가
	 * 모순이기 때문. */

	/*
	 * While sorting the ranges in increasing LBA order, check that the
	 * ranges do not overlap, that there are no sector holes and that all
	 * sectors belong to one range.
	 */
	for (i = 0; i < iars->nr_ia_ranges; i++) {
		/* [한국어] i번째 정렬 위치를 하나씩 확정해 나가는 선택 정렬 바깥
		 * 루프 - i가 커질수록 0..i-1 구간은 이미 LBA 오름차순으로 확정된
		 * 상태가 된다. */
		tmp = disk_find_ia_range(iars, sector);
		/* [한국어] "지금까지의 LBA 누적 끝(sector)"에서 정확히 시작하는
		 * range를 배열 전체에서 찾는다 - 아직 순서가 뒤섞인 원본 배열
		 * (드라이버가 하드웨어 보고 순서대로 채웠을 수 있는)에서도 찾을 수
		 * 있도록 매번 전체를 다시 스캔한다. */
		if (!tmp || tmp->sector != sector) {
			pr_warn("Invalid non-contiguous independent access ranges\n");
			return false;
			/* [한국어] tmp가 NULL이면 sector 위치에서 시작하는 range가
			 * 아예 없다는 뜻(구멍). tmp->sector != sector이면
			 * disk_find_ia_range()가 sector를 "포함"하는 range를
			 * 찾았을 뿐 그 range가 sector에서 "시작"하지는 않는다는
			 * 뜻이며, 이는 이전 range와 겹친다(overlap)는 신호다. 두
			 * 경우 모두 경고를 남기고 무효 처리. */
		}

		iar = &iars->ia_range[i];
		/* [한국어] 이번 반복에서 확정할 슬롯(정렬된 배열 기준 i번째 위치)의
		 * 주소. */
		if (tmp != iar) {
			swap(iar->sector, tmp->sector);
			swap(iar->nr_sectors, tmp->nr_sectors);
			/* [한국어] 찾아낸 range(tmp)가 아직 i번째 슬롯에 있지 않다면,
			 * 두 슬롯의 sector/nr_sectors "값"만 서로 교환해 tmp가 갖고
			 * 있던 데이터를 iar(i번째) 위치로 옮긴다 - 포인터나 kobj까지
			 * 통째로 바꾸는 것이 아니라 필드 값만 바꾸므로, 이후
			 * disk_register_independent_access_ranges()가
			 * &iars->ia_range[i].kobj를 초기화할 때는 이 교환이 끝난
			 * 상태의 데이터를 기준으로 등록하게 된다. */
		}

		sector += iar->nr_sectors;
		/* [한국어] 이번에 확정한 range의 길이만큼 누적 LBA 커서를
		 * 전진시켜, 다음 반복이 "그 다음에 이어져야 할 시작 LBA"를
		 * 찾도록 한다 - 이 누적값이 마지막에 capacity와 같아야 전체
		 * 디스크가 빈틈없이 커버된 것이다. */
	}

	if (sector != capacity) {
		pr_warn("Independent access ranges do not match disk capacity\n");
		return false;
		/* [한국어] 모든 range를 확정했는데도 누적 sector 합이 디스크 전체
		 * 용량과 다르면, range 총합이 디스크보다 작거나(뒷부분에 구멍) 큰
		 * (드라이버가 잘못된 정보를 보고) 상태이므로 무효 처리. */
	}

	return true;
	/* [한국어] 겹침 없음, 구멍 없음, 용량 일치 - 이 range 집합은 sysfs에
	 * 노출해도 안전하다고 판단. */
}

/*
 * [한국어]
 * disk_ia_ranges_changed - 기존 IAR과 새 IAR이 실질적으로 다른지 비교
 *
 * @disk: 대상 gendisk - disk->ia_ranges가 "기존" 값으로 쓰인다.
 * @new: 새로 제안된 independent access ranges(디스크에 아직 설치되지 않은
 *       상태).
 * @return: true(다르다 - 기존이 아예 없었거나, range 개수/내용 중 하나라도
 *          다르면), false(완전히 동일하다).
 *
 * disk_set_independent_access_ranges()가 "새로 만든 range 집합을 굳이
 * sysfs에 재등록할 필요가 있는가"를 판단하기 위한 순수 비교 함수다. 기존
 * range가 없으면(disk->ia_ranges == NULL) 무조건 변경으로 간주하고, range
 * 개수가 다르면 즉시 변경으로 간주하며, 개수가 같다면 인덱스별로 sector와
 * nr_sectors를 하나하나 비교한다(이 비교가 의미 있으려면 new도 old도 이미
 * disk_check_ia_ranges()로 LBA 오름차순 정렬이 끝난 상태여야 한다 - 실제로
 * disk_set_independent_access_ranges()는 이 함수를 호출하기 전에
 * disk_check_ia_ranges(disk, iars)를 먼저 호출해 둔다).
 * 실행 컨텍스트: 프로세스 컨텍스트, q->sysfs_lock 보호 아래.
 * 호출자: disk_set_independent_access_ranges().
 * 피호출자: 없음(순수 필드 비교).
 * 에러 경로: 없음 - 항상 true/false 중 하나를 반환.
 *
 * 호출 체인:
 *   disk_set_independent_access_ranges -> [disk_ia_ranges_changed]
 */
static bool disk_ia_ranges_changed(struct gendisk *disk,
				   struct blk_independent_access_ranges *new)
{
	struct blk_independent_access_ranges *old = disk->ia_ranges;
	/* [한국어] 비교 기준이 되는, disk에 현재 설치돼 있는(설치 전이라면
	 * NULL) 기존 range 집합. */
	int i;
	/* [한국어] range 배열 비교 루프 인덱스. */

	if (!old)
		return true;
	/* [한국어] 이전에 range 정보가 전혀 없던 disk라면(최초 설정) 무조건
	 * "변경됨"으로 취급 - 새로 등록해야 할 것이 확실히 있기 때문. */

	if (old->nr_ia_ranges != new->nr_ia_ranges)
		return true;
	/* [한국어] range 개수 자체가 다르면 내용을 비교할 필요도 없이 변경으로
	 * 판정 - 개수가 다르면 sysfs 하위 디렉터리 구성 자체가 달라지므로
	 * 반드시 재등록해야 한다. */

	for (i = 0; i < old->nr_ia_ranges; i++) {
		if (new->ia_range[i].sector != old->ia_range[i].sector ||
		    new->ia_range[i].nr_sectors != old->ia_range[i].nr_sectors)
			return true;
		/* [한국어] 같은 인덱스 위치의 sector 또는 nr_sectors 중 하나라도
		 * 다르면 LBA 경계가 바뀐 것이므로 변경으로 판정하고 즉시
		 * 반환한다(더 볼 필요 없음). */
	}

	return false;
	/* [한국어] 개수도 같고 모든 인덱스의 sector/nr_sectors도 같으므로 완전히
	 * 동일 - 호출자는 새로 만든 new를 폐기(kfree)하고 기존 sysfs 등록을
	 * 그대로 유지한다. */
}

/**
 * disk_alloc_independent_access_ranges - Allocate an independent access ranges
 *                                        data structure
 * @disk:		target disk
 * @nr_ia_ranges:	Number of independent access ranges
 *
 * Allocate a struct blk_independent_access_ranges structure with @nr_ia_ranges
 * access range descriptors.
 */
/*
 * [한국어]
 * disk_alloc_independent_access_ranges - IAR 컨테이너 할당
 *
 * @disk: 대상 gendisk - disk->queue->node로 NUMA 배치 노드를 결정하는 데만
 *        쓰이고, 이 함수는 disk에 아무것도 대입하지 않는다(설치는
 *        disk_set_independent_access_ranges()의 몫).
 * @nr_ia_ranges: 할당할 독립 접근 범위 개수(드라이버가 하드웨어 질의로 미리
 *        파악한 값).
 * @return: 성공 시 새로 할당된 struct blk_independent_access_ranges 포인터
 *          (nr_ia_ranges 필드까지 채워진 상태), 메모리 부족 시 NULL.
 *
 * struct blk_independent_access_ranges 헤더와 그 뒤에 이어지는 가변 길이
 * 배열 ia_range[nr_ia_ranges]를 하나의 연속된 메모리 블록으로 할당하는
 * "flexible array member" 패턴의 전형적인 사용례다. struct_size()가
 * sizeof(*iars) + nr_ia_ranges * sizeof(iars->ia_range[0])를 정수 오버플로
 * 검사와 함께 계산해 주므로, 사용자가 직접 곱셈을 하지 않아도 된다. 이렇게
 * 한 번에 할당하면 이후 kfree() 한 번으로 컨테이너와 배열 전체를 함께 해제할
 * 수 있다(blk_ia_ranges_sysfs_release()가 하는 일).
 * 실행 컨텍스트: 드라이버의 프로브/revalidate 경로, 프로세스 컨텍스트,
 * 휴면 가능(GFP_KERNEL).
 * 호출자: 이 심볼을 사용하는 블록 디바이스 드라이버(대표적으로 SCSI sd의
 * 멀티 액추에이터 HDD 지원 코드, 이 저장소에는 포함되어 있지 않음) -
 * EXPORT_SYMBOL_GPL로 외부 모듈에도 공개된다.
 * 피호출자: kzalloc_node().
 * 에러 경로: kzalloc_node()가 NULL을 반환하면(메모리 부족) iars가 NULL이 된
 * 채로 그대로 반환되며, nr_ia_ranges 필드 대입은 건너뛴다(if (iars) 가드).
 *
 * 호출 체인:
 *   드라이버 프로브/revalidate -> [disk_alloc_independent_access_ranges]
 *     -> disk_set_independent_access_ranges (호출자가 이어서 사용)
 */
struct blk_independent_access_ranges *
disk_alloc_independent_access_ranges(struct gendisk *disk, int nr_ia_ranges)
{
	struct blk_independent_access_ranges *iars;
	/* [한국어] 반환할 컨테이너 포인터 - 아직 할당 전이므로 초기값은 없음. */

	iars = kzalloc_node(struct_size(iars, ia_range, nr_ia_ranges),
			    GFP_KERNEL, disk->queue->node);
	/* [한국어] struct_size(iars, ia_range, nr_ia_ranges): 컨테이너 헤더 +
	 * nr_ia_ranges개의 ia_range 원소 크기를 오버플로 안전하게 계산.
	 * GFP_KERNEL: 휴면 가능한 일반 커널 할당 플래그(인터럽트 컨텍스트에서
	 * 호출 불가). disk->queue->node: 이 request_queue가 속한 NUMA 노드에
	 * 메모리를 배치해 이후 이 데이터를 자주 읽는 CPU와의 지역성을 높인다.
	 * kzalloc이므로 모든 바이트가 0으로 초기화되어(kobj들도 zero, sector/
	 * nr_sectors도 0, sysfs_registered도 false) 별도 초기화 없이도 안전한
	 * 상태로 시작한다. */
	if (iars)
		iars->nr_ia_ranges = nr_ia_ranges;
		/* [한국어] 할당에 성공했을 때만 실제 range 개수를 기록 - 이후
		 * disk_check_ia_ranges()/disk_register_independent_access_ranges()
		 * 등 모든 for 루프가 이 필드를 반복 횟수 상한으로 사용한다. */
	return iars;
	/* [한국어] 성공 시 채워진 포인터, 실패 시 NULL을 그대로 호출자에게 반환 -
	 * 호출자는 NULL 여부로 메모리 부족을 판단해야 한다. */
}
EXPORT_SYMBOL_GPL(disk_alloc_independent_access_ranges);
/* [한국어] GPL 라이선스 모듈에서도 이 심볼을 링크해 쓸 수 있도록 내보낸다 -
 * 블록 드라이버가 모듈로 빌드되는 경우(예: 특정 HBA 드라이버)를 위한 공개
 * API. */

/**
 * disk_set_independent_access_ranges - Set a disk independent access ranges
 * @disk:	target disk
 * @iars:	independent access ranges structure
 *
 * Set the independent access ranges information of the request queue
 * of @disk to @iars. If @iars is NULL and the independent access ranges
 * structure already set is cleared. If there are no differences between
 * @iars and the independent access ranges structure already set, @iars
 * is freed.
 */
/*
 * [한국어]
 * disk_set_independent_access_ranges - 디스크에 IAR 설정(설치/교체/해제)
 *
 * @disk: 대상 gendisk.
 * @iars: 새로 설정할 independent access ranges - disk_alloc_independent_access_ranges()
 *        로 미리 할당해 채워 온 컨테이너, 또는 range 정보를 지우고 싶다면
 *        NULL.
 * @return: 없음(void) - 성공/실패와 무관하게 항상 반환하며, 유일한 실패
 *          시나리오(무결성 검사 실패)는 iars를 조용히 폐기하는 것으로
 *          처리된다.
 *
 * 드라이버가 하드웨어에서 읽어온 새 range 집합을 gendisk에 실제로
 * 반영하는 최상위 진입점이다. 순서는 다음과 같다: (1) q->sysfs_lock을 잡아
 * 이 함수 전체를 사용자 공간의 동시 sysfs 접근으로부터 보호, (2) iars가
 * 있는데 disk_check_ia_ranges()를 통과하지 못하면(디스크 용량을 겹침/구멍
 * 없이 커버하지 못하면) 잘못된 정보로 간주해 즉시 kfree()하고 iars를
 * NULL로 만들어 "설정 없음(해제)"과 동일하게 처리, (3) iars가 여전히
 * 있는데 disk_ia_ranges_changed()가 "변경 없음"을 보고하면 새로 만든
 * iars가 불필요하므로 kfree()하고 재등록 없이 그대로 잠금 해제, (4) 그 외의
 * 모든 경우(최초 설정/내용 변경/명시적 해제)에는 기존 sysfs 등록을
 * disk_unregister_independent_access_ranges()로 정리한 뒤 disk->ia_ranges를
 * 새 값(NULL일 수도 있음)으로 교체하고, request_queue가 이미 sysfs에
 * 등록돼 있는 상태(revalidation처럼 살아있는 디바이스를 갱신하는 경우)라면
 * disk_register_independent_access_ranges()로 즉시 재등록한다 - 아직
 * queue가 등록 전이라면(디바이스 최초 프로브 중) 뒤이어 진행될
 * add_disk()/blk_register_queue() 경로가 disk->ia_ranges를 보고 알아서
 * 등록해 줄 것이므로 여기서는 아무것도 하지 않아도 된다.
 * 실행 컨텍스트: 드라이버의 프로브/revalidate 경로, 프로세스 컨텍스트,
 * q->sysfs_lock(mutex, 휴면 가능)을 이 함수 안에서 직접 획득/해제한다.
 * 호출자: 이 심볼을 사용하는 블록 디바이스 드라이버(대표적으로 SCSI sd의
 * 멀티 액추에이터 HDD 지원 코드) - EXPORT_SYMBOL_GPL로 공개.
 * 피호출자: disk_check_ia_ranges(), disk_ia_ranges_changed(),
 * disk_unregister_independent_access_ranges(),
 * disk_register_independent_access_ranges(), blk_queue_registered(),
 * kfree().
 * 에러 경로: 무결성 검사 실패는 예외를 던지지 않고 iars를 폐기하는 것으로
 * "조용히" 처리된다(호출자에게 별도 에러 코드가 돌아가지 않음 - void
 * 반환) - 이는 드라이버가 매 revalidate마다 이 함수를 호출해도 신경 쓸
 * 반환값이 없다는 것을 의미한다.
 *
 * 호출 체인:
 *   드라이버 프로브/revalidate -> [disk_set_independent_access_ranges]
 *     -> disk_check_ia_ranges -> disk_ia_ranges_changed
 *     -> disk_unregister_independent_access_ranges
 *     -> disk_register_independent_access_ranges
 */
void disk_set_independent_access_ranges(struct gendisk *disk,
				struct blk_independent_access_ranges *iars)
{
	struct request_queue *q = disk->queue;
	/* [한국어] 이 disk의 request_queue - sysfs_lock과 등록 상태 확인에
	 * 필요. */

	mutex_lock(&q->sysfs_lock);
	/* [한국어] 이 함수 전체(검증-비교-교체-재등록)를 하나의 임계 구역으로
	 * 묶는다 - 사용자 공간이 sysfs를 통해 independent_access_ranges를
	 * 읽는 도중에 디렉터리 구조가 바뀌는 것을 막고, 동시에 여러
	 * revalidate가 겹치는 것도 배제한다. */
	if (iars && !disk_check_ia_ranges(disk, iars)) {
		/* [한국어] iars가 NULL이 아닌데(즉 실제로 range를 설정하려는
		 * 시도인데) 무결성 검사를 통과하지 못했다면 진입 - 겹침/구멍/
		 * 용량 불일치 중 하나. */
		kfree(iars);
		/* [한국어] 잘못된 range 정보이므로 즉시 메모리 반납 - 이 iars는
		 * 애초에 disk에 설치된 적이 없으므로(disk->ia_ranges는 아직
		 * 건드리지 않음) 단순 kfree만으로 충분하다(kobject 경로를 거칠
		 * 필요 없음). */
		iars = NULL;
		/* [한국어] 이후 로직에서 "설정할 range 없음"으로 취급되도록 지역
		 * 변수를 NULL로 재설정 - 아래의 disk_unregister/disk->ia_ranges =
		 * iars 흐름이 "완전 해제"와 동일하게 동작하게 된다. */
	}
	if (iars && !disk_ia_ranges_changed(disk, iars)) {
		/* [한국어] (검증을 통과한) iars가 기존 disk->ia_ranges와 내용이
		 * 완전히 동일하다면 재등록할 필요가 없다 - revalidate가 매번
		 * 호출되는데 하드웨어 구성이 그대로인 흔한 경우를 최적화. */
		kfree(iars);
		/* [한국어] 새로 만들었지만 쓸모없어진 iars를 반납 - 기존
		 * disk->ia_ranges(및 그 sysfs 등록)는 손대지 않고 그대로 둔다. */
		goto unlock;
		/* [한국어] 아래의 unregister/register 재등록 로직 전체를 건너뛰고
		 * 곧바로 잠금 해제로 점프 - 변경이 없으므로 sysfs 재구성 비용을
		 * 아예 발생시키지 않는다. */
	}

	/*
	 * This may be called for a registered queue. E.g. during a device
	 * revalidation. If that is the case, we need to unregister the old
	 * set of independent access ranges and register the new set. If the
	 * queue is not registered, registration of the device request queue
	 * will register the independent access ranges.
	 */
	disk_unregister_independent_access_ranges(disk);
	/* [한국어] 여기 도달했다는 것은 "최초 설정", "내용이 실제로 바뀐 갱신",
	 * "명시적 해제(iars == NULL)" 중 하나라는 뜻 - 어느 경우든 먼저 기존
	 * sysfs 등록(있다면)을 정리한다. disk->ia_ranges가 원래 NULL이었다면
	 * 이 호출은 조용히 아무 일도 하지 않고 반환한다(함수 내부의 !iars
	 * 가드). */
	disk->ia_ranges = iars;
	/* [한국어] disk의 range 포인터를 새 값으로 교체 - iars가 NULL이면 이
	 * 대입 자체가 "range 정보 완전 해제"를 뜻하고, 유효한 포인터면 새
	 * 구성으로 교체하는 것이다. */
	if (blk_queue_registered(q))
		disk_register_independent_access_ranges(disk);
		/* [한국어] blk_queue_registered(): q->queue_flags의
		 * QUEUE_FLAG_REGISTERED 비트를 검사하는 인라인 헬퍼(block/blk.h) -
		 * 이 disk가 이미 add_disk()를 거쳐 살아있는 상태(revalidation
		 * 중)라면, 방금 교체한 disk->ia_ranges를 즉시 sysfs에 (재)등록해
		 * 사용자 공간이 최신 값을 곧바로 볼 수 있게 한다. 아직 등록 전
		 * (디바이스 최초 프로브 도중)이라면, 이후 이어질
		 * add_disk()/blk_register_queue() 경로가 이 disk->ia_ranges를
		 * 보고 알아서 처음으로 등록해 줄 것이므로 여기서는 아무 것도
		 * 하지 않는다. */
unlock:
	mutex_unlock(&q->sysfs_lock);
	/* [한국어] 검증을 통과하지 못했거나(iars == NULL로 폴백), 변경이
	 * 없어서 goto로 건너뛰었거나, 정상적으로 교체/재등록까지 마친 모든
	 * 경로가 이 지점에서 합류해 잠금을 해제한다. */
}
EXPORT_SYMBOL_GPL(disk_set_independent_access_ranges);
/* [한국어] GPL 모듈로 빌드된 블록 드라이버도 이 API를 호출할 수 있도록
 * 공개 - disk_alloc_independent_access_ranges()와 함께 이 파일이 드라이버에
 * 제공하는 단 두 개의 "쓰기" 진입점이다(나머지 함수는 모두 static이며 이
 * 두 함수를 통해서만 간접 호출된다). */
