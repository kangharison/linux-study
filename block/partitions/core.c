// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1991-1998  Linus Torvalds
 * Re-organised Feb 1998 Russell King
 * Copyright (C) 2020 Christoph Hellwig
 */
/*
 * [한국어 설명] 파티션 프레임워크 핵심 구현 - 포맷별 파서 순회, block_device 생성/삭제/재스캔 (core.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 블록 레이어 파티션(partition) 서브시스템의 핵심 프레임워크다.
 * gendisk(범용 디스크) 하나에 대해 check_part[] 배열에 등록된 여러 파티션
 * 스킴 파서(parser)를 순서대로 시도하여 첫 번째로 성공하는 파서의 결과를
 * 채택하고, 그 결과로 파악된 각 파티션을 독립적인 struct block_device로
 * 동적 생성해 gendisk에 연결(등록)한다. msdos(MBR), GPT/EFI, Mac, Amiga,
 * Atari, SGI, Sun, OSF, LDM, ADFS 계열(ICS/POWERTEC/EESOX/CUMANA/ADFS) 등
 * 서로 다른 파티션 테이블 포맷을 struct parsed_partitions라는 공통 인터페이스로
 * 추상화하여, 어떤 블록 디바이스 드라이버(NVMe, SCSI, virtio-blk, loop 등)든
 * 파티션 인식 로직을 직접 구현하지 않아도 되게 한다. 또한 사용자 공간의
 * ioctl(BLKPG_ADD_PARTITION 등) 요청에 따라 파티션을 수동으로 추가/삭제/
 * 크기조정하는 bdev_add_partition()/bdev_del_partition()/
 * bdev_resize_partition() API도 이 파일이 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디스크 드라이버가 add_disk()로 gendisk를 커널에 등록(block/genhd.c)한 뒤,
 * 최초 open 시점이나 명시적 재스캔 요청 시 bdev_disk_changed()가 호출되어
 * 이 파일의 파티션 스캔 파이프라인이 시작된다. 대표적 호출 체인은:
 *   add_disk()/blkdev_get_by_dev() -> bdev_disk_changed()
 *     -> blk_add_partitions() -> check_partition()
 *       -> check_part[i](state)  (block/partitions/msdos.c, efi.c, mac.c 등)
 *     -> blk_add_partition() -> add_partition()
 *       -> bdev_alloc()/bdev_add() (block/bdev.c)
 * 이 파일 자체는 특정 스토리지 프로토콜에 종속되지 않는 순수 블록 레이어
 * 코드이며, open()/ioctl() 시스템 호출을 처리하는 프로세스 컨텍스트
 * (커널 스레드) 또는 드라이버의 revalidate 경로에서 동기적으로 실행된다.
 * 파티션 테이블 자체를 읽는 I/O(read_part_sector())는 페이지 캐시
 * (read_mapping_folio())를 경유하므로, 캐시 미스 시 그 아래에서
 * submit_bio() -> 블록 I/O 스택 -> 드라이버의 ->submit_bio/queue_rq 콜백까지
 * 내려가 실제 매체에서 데이터를 가져온다(예: NVMe라면 nvme_queue_rq() ->
 * NVMe SQ에 READ 커맨드 제출 -> 도어벨 갱신 -> CQE 완료 인터럽트).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: block/partitions/check.h(struct parsed_partitions 정의 및
 * 각 포맷 파서 함수 선언), block/genhd.c(struct gendisk, disk_to_dev(),
 * disk_live(), disk_has_partscan(), blk_alloc_ext_minor()), block/bdev.c
 * (bdev_alloc(), bdev_add(), bdev_unhash(), bdev_drop(), bdev_set_nr_sectors()),
 * block/blk.h(part_stat_show 등 sysfs 콜백 선언과 ADDPART_FLAG_* 매크로),
 * drivers/md/*(md_autodetect_dev() - RAID superblock 자동 인식), 그리고
 * block/partitions/{msdos,efi,mac,...}.c(포맷별 파서 실제 구현체).
 * 이 파일에 의존하는 모듈: block/ioctl.c 등에서 BLKPG_ADD_PARTITION/
 * BLKPG_DEL_PARTITION/BLKPG_RESIZE_PARTITION ioctl을 처리할 때
 * bdev_add_partition()/bdev_del_partition()/bdev_resize_partition()을
 * 호출하고, 드라이버 open 경로(blkdev_get_by_dev 등)는 bdev_disk_changed()를
 * 호출한다. 데이터 흐름 관점에서는 디스크 원시 바이트(LBA 0, 0xdc0 등
 * 파티션 테이블 위치) -> read_part_sector()가 채우는
 * struct parsed_partitions.parts[] 배열(시작 섹터/길이/플래그/메타정보)
 * -> add_partition()이 만드는 struct block_device(gendisk->part_tbl
 * xarray에 partno로 색인) -> 최종적으로 사용자 공간에 /dev/<disk>pN 노드와
 * /sys/.../<part>/{partition,start,size,stat,...} sysfs 파일로 노출되는
 * 순서로 흐른다. 핵심 공유 자료구조는 struct gendisk의 part_tbl(xarray,
 * partno -> block_device), open_mutex(파티션 추가/삭제/재스캔을 직렬화하는
 * 락), state(GD_NATIVE_CAPACITY/GD_NEED_PART_SCAN 등 비트 플래그)이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - check_partition(): check_part[] 배열의 각 포맷 파서를 순서대로 실행해
 *   첫 성공 결과를 반환. I/O 에러는 기록해 두었다가 모든 포맷이 실패한
 *   경우에만 최종 보고한다.
 * - add_partition(): 파티션 하나에 대응하는 struct block_device를 할당하고
 *   devt/sysfs/part_tbl에 등록. 실패 시 goto 체인으로 부분 초기화를 되돌린다.
 * - blk_add_partitions(): check_partition() 결과를 바탕으로 모든 파티션을
 *   blk_add_partition()/add_partition()으로 순차 등록.
 * - bdev_disk_changed(): 미디어 변경/재스캔 시 기존 파티션을 모두 지우고
 *   파티션 테이블을 다시 읽어 재등록한다. loop/dasd 등 레거시 드라이버 전용
 *   export 심볼이다.
 * - bdev_add_partition()/bdev_del_partition()/bdev_resize_partition():
 *   사용자 요청(ioctl)에 의한 수동 파티션 추가/삭제/크기조정.
 * - read_part_sector(): 파티션 테이블이 위치한 섹터를 페이지 캐시를 통해
 *   읽어 각 포맷 파서에 공급한다.
 * - check_part[]: Kconfig 설정에 따라 컴파일되는 파티션 포맷 파서 함수
 *   포인터 배열. NULL 종단(sentinel)으로 순회를 끝낸다.
 * - part_type/part_attr_group(s): 파티션 block_device의 sysfs 장치 모델
 *   (디바이스 타입, 속성 그룹)을 정의한다.
 */
#include <linux/fs.h> /* [한국어] struct block_device/struct address_space 등 VFS·블록 계층 기본 타입 선언 */
#include <linux/major.h> /* [한국어] BLOCK_EXT_MAJOR 등 메이저 번호 상수 - 확장 minor 파티션의 devt 구성에 사용 */
#include <linux/slab.h> /* [한국어] kzalloc_obj()/kfree() 힙 할당자 - parsed_partitions/bd_meta_info 등 동적 할당 */
#include <linux/string.h> /* [한국어] strscpy()/strlen() 등 문자열 함수 - 디스크 이름 복사와 접미사 판별에 사용 */
#include <linux/sysfs.h> /* [한국어] sysfs_emit() 등 sysfs 출력 헬퍼 - part_xxx_show() 콜백들이 사용 */
#include <linux/ctype.h> /* [한국어] isdigit() - 디스크 이름이 숫자로 끝나는지 검사해 'p' 접미사 부착 여부 결정 */
#include <linux/vmalloc.h> /* [한국어] vzalloc()/vfree() - parts[] 배열은 최대 DISK_MAX_PARTS(256)개라 커질 수 있어 vmalloc 계열 사용 */
#include <linux/raid/detect.h> /* [한국어] md_autodetect_dev() 선언 - RAID superblock을 가진 파티션의 자동 인식(md_autodetect_dev) */
#include "check.h" /* [한국어] struct parsed_partitions 정의와 각 포맷 파서 함수 선언을 담은 로컬(비공개) 헤더 */

/*
 * [한국어]
 * check_part[] - 파티션 포맷 프로버(parser) 함수 포인터 배열.
 *
 * 배열 각 원소는 int (*)(struct parsed_partitions *) 시그니처를 갖는
 * 포맷별 프로버(예: msdos_partition, efi_partition, mac_partition 등)이며,
 * Kconfig(CONFIG_..._PARTITION)로 활성화된 항목만 #ifdef를 통해 배열에
 * 포함된다. 마지막 원소는 항상 NULL이며, 이는 check_partition()의
 * while (!res && check_part[i]) 순회를 끝내는 sentinel이다.
 * 순서가 중요하다: ADFS 계열이 msdos보다 먼저 오는 이유는 ADFS 미디어가
 * 오래된 PC/BIOS 파티션 테이블 잔재를 가질 수 있어 msdos 프로버가 먼저
 * 매칭되면 안 되기 때문이고, of_partition/efi_partition/ldm_partition도
 * 각각 OF(devicetree)·GPT·LDM 메타데이터가 msdos MBR 시그니처와 혼동되지
 * 않도록 msdos_partition보다 앞에 배치된다.
 * 설정자: 컴파일 타임에 Kconfig 매크로로 확정되며 런타임에는 불변(const)이다.
 * 읽는 자: check_partition()이 인덱스 i를 증가시키며 순차 호출한다.
 * 값 범위: 각 함수 포인터는 유효한 코드 주소이거나(활성화된 경우) 배열에서
 * 아예 빠진다(비활성화된 경우). 마지막 값은 항상 NULL.
 * 동기화: static const 배열이며 런타임에 갱신되지 않으므로 락 불필요.
 */
static int (*const check_part[])(struct parsed_partitions *) = { /* [한국어] 함수 포인터 배열 시작 - 각 포맷 프로버는 성공 시 파싱한 파티션 수(>0), 실패 시 0 또는 음수(I/O 에러)를 반환 */
	/* [한국어] 아래 원본 영어 주석 번역: 디스크 주소 0에 테이블이 있고 0xdc0에도 ADFS 부트블록을 갖는 포맷들을 먼저 시도한다. */
	/*
	 * Probe partition formats with tables at disk address 0
	 * that also have an ADFS boot block at 0xdc0.
	 */
#ifdef CONFIG_ACORN_PARTITION_ICS /* [한국어] ICS(Introl/Cumana ICS) 파티션 포맷이 커널에 빌드된 경우에만 포함 */
	adfspart_check_ICS, /* [한국어] LBA 0/0xdc0의 ICS 파티션 메타데이터를 검증하는 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_ACORN_PARTITION_ICS #ifdef 종료 */
#ifdef CONFIG_ACORN_PARTITION_POWERTEC /* [한국어] POWERTEC 파티션 포맷이 커널에 빌드된 경우에만 포함 */
	adfspart_check_POWERTEC, /* [한국어] POWERTEC 파티션 메타데이터 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_ACORN_PARTITION_POWERTEC #ifdef 종료 */
#ifdef CONFIG_ACORN_PARTITION_EESOX /* [한국어] EESOX 파티션 포맷이 커널에 빌드된 경우에만 포함 */
	adfspart_check_EESOX, /* [한국어] EESOX 파티션 메타데이터 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_ACORN_PARTITION_EESOX #ifdef 종료 */

	/* [한국어] 아래 원본 영어 주석 번역: 이제 0xdc0에만 파티션 정보가 있는 포맷으로 넘어간다 - 오래된 PC/BIOS 테이블 잔재가 있을 수 있어 msdos 항목보다 먼저 와야 한다. */
	/*
	 * Now move on to formats that only have partition info at
	 * disk address 0xdc0.  Since these may also have stale
	 * PC/BIOS partition tables, they need to come before
	 * the msdos entry.
	 */
#ifdef CONFIG_ACORN_PARTITION_CUMANA /* [한국어] CUMANA 파티션 포맷이 커널에 빌드된 경우에만 포함 */
	adfspart_check_CUMANA, /* [한국어] CUMANA 파티션 메타데이터 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_ACORN_PARTITION_CUMANA #ifdef 종료 */
#ifdef CONFIG_ACORN_PARTITION_ADFS /* [한국어] ADFS 파티션 포맷이 커널에 빌드된 경우에만 포함 */
	adfspart_check_ADFS, /* [한국어] ADFS 파티션 메타데이터 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_ACORN_PARTITION_ADFS #ifdef 종료 */

#ifdef CONFIG_CMDLINE_PARTITION /* [한국어] 커널 커맨드라인(cmdline)으로 지정한 고정 파티션 레이아웃이 활성화된 경우에만 포함 */
	cmdline_partition, /* [한국어] cmdline= 파라미터로 주어진 파티션 레이아웃을 적용하는 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_CMDLINE_PARTITION #ifdef 종료 */
#ifdef CONFIG_OF_PARTITION /* [한국어] Open Firmware(devicetree) 파티션 정보가 활성화된 경우에만 포함 */
	of_partition,		/* cmdline have priority to OF */ /* [한국어] 원본 주석 그대로: cmdline이 OF보다 우선 - cmdline_partition이 배열에서 앞서 오므로 먼저 매칭됨 */
#endif /* [한국어] CONFIG_OF_PARTITION #ifdef 종료 */
#ifdef CONFIG_EFI_PARTITION /* [한국어] EFI(GPT) 파티션 포맷이 활성화된 경우에만 포함 */
	efi_partition,		/* this must come before msdos */ /* [한국어] 원본 주석 그대로: msdos보다 먼저 와야 함 - GPT 디스크는 호환성을 위해 protective MBR도 갖고 있어 msdos 프로버가 먼저 매칭되면 안 됨 */
#endif /* [한국어] CONFIG_EFI_PARTITION #ifdef 종료 */
#ifdef CONFIG_SGI_PARTITION /* [한국어] SGI 파티션 포맷이 활성화된 경우에만 포함 */
	sgi_partition, /* [한국어] SGI(IRIX) 볼륨 헤더 파티션 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_SGI_PARTITION #ifdef 종료 */
#ifdef CONFIG_LDM_PARTITION /* [한국어] LDM(Windows Dynamic Disk) 파티션 포맷이 활성화된 경우에만 포함 */
	ldm_partition,		/* this must come before msdos */ /* [한국어] 원본 주석 그대로: msdos보다 먼저 와야 함 - LDM은 MBR 파티션 안에 중첩되어 존재하므로 우선 인식 필요 */
#endif /* [한국어] CONFIG_LDM_PARTITION #ifdef 종료 */
#ifdef CONFIG_MSDOS_PARTITION /* [한국어] MSDOS(MBR) 파티션 포맷이 활성화된 경우에만 포함 - 가장 흔한 PC 파티션 테이블 */
	msdos_partition, /* [한국어] LBA 0의 512B 부트섹터에서 0x55AA 시그니처와 4개 primary 엔트리를 파싱하는 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_MSDOS_PARTITION #ifdef 종료 */
#ifdef CONFIG_OSF_PARTITION /* [한국어] OSF/1(Digital UNIX) 디스크레이블 파티션 포맷이 활성화된 경우에만 포함 */
	osf_partition, /* [한국어] OSF 디스크레이블 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_OSF_PARTITION #ifdef 종료 */
#ifdef CONFIG_SUN_PARTITION /* [한국어] Sun 디스크레이블(VTOC) 파티션 포맷이 활성화된 경우에만 포함 */
	sun_partition, /* [한국어] Sun VTOC 파티션 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_SUN_PARTITION #ifdef 종료 */
#ifdef CONFIG_AMIGA_PARTITION /* [한국어] Amiga Rigid Disk Block(RDB) 파티션 포맷이 활성화된 경우에만 포함 */
	amiga_partition, /* [한국어] Amiga RDB 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_AMIGA_PARTITION #ifdef 종료 */
#ifdef CONFIG_ATARI_PARTITION /* [한국어] Atari ST 파티션 포맷이 활성화된 경우에만 포함 */
	atari_partition, /* [한국어] Atari 파티션 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_ATARI_PARTITION #ifdef 종료 */
#ifdef CONFIG_MAC_PARTITION /* [한국어] Mac(Apple Partition Map) 파티션 포맷이 활성화된 경우에만 포함 */
	mac_partition, /* [한국어] Apple Partition Map 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_MAC_PARTITION #ifdef 종료 */
#ifdef CONFIG_ULTRIX_PARTITION /* [한국어] DEC Ultrix 파티션 포맷이 활성화된 경우에만 포함 */
	ultrix_partition, /* [한국어] Ultrix 파티션 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_ULTRIX_PARTITION #ifdef 종료 */
#ifdef CONFIG_IBM_PARTITION /* [한국어] IBM(디스크레이블) 파티션 포맷이 활성화된 경우에만 포함 */
	ibm_partition, /* [한국어] IBM 디스크레이블 프로버 함수 포인터 (S/390 DASD 등) */
#endif /* [한국어] CONFIG_IBM_PARTITION #ifdef 종료 */
#ifdef CONFIG_KARMA_PARTITION /* [한국어] Karma(리모트 MP3 플레이어) 파티션 포맷이 활성화된 경우에만 포함 */
	karma_partition, /* [한국어] Karma 파티션 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_KARMA_PARTITION #ifdef 종료 */
#ifdef CONFIG_SYSV68_PARTITION /* [한국어] SYSV68(Motorola System V/68) 파티션 포맷이 활성화된 경우에만 포함 */
	sysv68_partition, /* [한국어] SYSV68 파티션 프로버 함수 포인터 */
#endif /* [한국어] CONFIG_SYSV68_PARTITION #ifdef 종료 */
	NULL /* [한국어] 배열 종단 sentinel - check_partition()의 while (!res && check_part[i]) 루프를 여기서 멈추게 함 */
}; /* [한국어] check_part[] 배열 정의 종료 */

/*
 * [한국어]
 * allocate_partitions() - 파티션 스캔에 사용할 parsed_partitions 상태를 할당한다.
 *
 * @hd: 파티션을 스캔할 대상 gendisk. 이 시점에는 아직 state->disk에
 *      대입되지 않으며(실제 대입은 check_partition()에서 수행), 이 함수
 *      안에서는 사용되지 않는다(과거에는 사용되었을 수 있으나 현재는
 *      단순히 시그니처 일관성을 위해 남아 있다 - 추정).
 * @return: 성공 시 초기화된 struct parsed_partitions 포인터, 메모리 부족
 *          시 NULL. 이 함수는 절대 ERR_PTR을 반환하지 않는다(NULL만 반환).
 *
 * 이 함수는 파티션 후보를 담을 parts[] 배열(DISK_MAX_PARTS개, 즉 256개)과
 * 그 컨테이너인 parsed_partitions 구조체를 힙에 할당하는 첫 단계이다.
 * parts[] 배열은 각 원소가 struct partition_meta_info 등을 포함해 크기가
 * 커질 수 있으므로 kmalloc이 아닌 vzalloc(가상 연속 메모리)을 사용한다.
 * 동작 순서: (1) kzalloc_obj로 parsed_partitions 구조체 자체를 0-채움
 * 할당, (2) 실패하면 즉시 NULL 반환, (3) 성공하면 parts[] 배열을 vzalloc으로
 * 할당, (4) 배열 할당 실패 시 앞서 할당한 state도 kfree로 되돌리고(rollback)
 * NULL 반환, (5) 모두 성공하면 limit 필드에 배열 크기를 기록하고 state 반환.
 * 실행 컨텍스트: check_partition() 호출자와 동일한 프로세스 컨텍스트에서
 * 동기적으로 실행되며 별도의 동시성 보호가 필요 없다(호출자가 매번 새
 * state를 만들어 자신만 사용).
 * 호출자(caller): check_partition().
 * 피호출자(callee): kzalloc_obj(), vzalloc(), array_size(), kfree().
 * 에러 경로: 두 단계의 할당 중 어느 하나라도 실패하면 지금까지 확보한
 * 자원을 정리(kfree)하고 NULL을 반환해 상위에서 스캔을 포기하게 한다.
 *
 * 호출 체인:
 *   check_partition() → [allocate_partitions] → kzalloc_obj()/vzalloc()
 */
static struct parsed_partitions *allocate_partitions(struct gendisk *hd) /* [한국어] parsed_partitions 상태 할당 함수 시작 - hd는 향후 확장을 위한 파라미터(현재 본문에서는 사용되지 않음, 추정) */
{
	struct parsed_partitions *state; /* [한국어] 반환할 parsed_partitions 포인터 - 아직 미할당(초기화 전) */
	int nr = DISK_MAX_PARTS; /* [한국어] parts[] 배열 크기의 상한값 - DISK_MAX_PARTS(256)으로 고정. gendisk당 최대 파티션 수와 동일한 제약 */

	state = kzalloc_obj(*state); /* [한국어] *state 크기(sizeof(struct parsed_partitions))만큼 0으로 채워 할당하는 kzalloc 래퍼(sizeof(*state)를 자동 계산) */
	if (!state) /* [한국어] 할당 실패 검사 - 메모리 부족 시 더 진행하지 않음 */
		return NULL; /* [한국어] 스캔 시작조차 못하고 NULL 반환 - 호출자는 이를 '스캔 불가'로 처리 */

	state->parts = vzalloc(array_size(nr, sizeof(state->parts[0]))); /* [한국어] array_size(nr, sizeof(...))는 nr*sizeof 곱셈의 정수 오버플로를 검사하는 안전한 헬퍼; vzalloc은 0-채움 가상 메모리(큰 배열이라 커널 힙 대신 vmalloc 영역 사용) */
	if (!state->parts) { /* [한국어] parts[] 배열 할당 실패 검사 */
		kfree(state); /* [한국어] 앞서 할당한 state 구조체를 롤백(해제) - 부분 초기화 상태를 남기지 않기 위함 */
		return NULL; /* [한국어] 부분 실패를 NULL로 알림 */
	} /* [한국어] if 블록 종료 */

	state->limit = nr; /* [한국어] limit에 parts[] 배열의 유효 인덱스 상한(256)을 기록 - 이후 blk_add_partitions()의 for (p = 1; p < state->limit; p++) 순회 기준이 됨 */

	return state; /* [한국어] 모든 할당이 성공한 완전히 초기화된 state를 호출자에게 반환 */
} /* [한국어] allocate_partitions() 함수 종료 */

/*
 * [한국어]
 * free_partitions() - allocate_partitions()으로 할당한 상태를 해제한다.
 *
 * @state: allocate_partitions()이 반환한, parts[]까지 이미 채워진(또는
 *         스캔이 끝난) parsed_partitions 포인터. NULL이 아니어야 한다
 *         (호출부에서 이미 NULL 체크를 거친 뒤 호출).
 * @return: 없음(void).
 *
 * parts[] 배열(vzalloc으로 할당)과 state 구조체 자체(kzalloc_obj로 할당)를
 * 각각 vfree/kfree로 되돌리는 단순한 해제 함수이다. allocate_partitions()의
 * 정확한 역연산(inverse)이며, 항상 쌍으로 호출된다.
 * 실행 컨텍스트: check_partition()/blk_add_partitions()와 같은 동기 경로에서
 * 호출되며 별도 동기화가 필요 없다(state는 호출 스택 로컬 소유물).
 * 호출자(caller): check_partition()(에러/성공 양쪽 경로), blk_add_partitions()
 *                 (파티션 등록을 모두 마친 뒤).
 * 피호출자(callee): vfree(), kfree().
 * 에러 경로: 해제 함수이므로 실패 개념이 없다.
 *
 * 호출 체인:
 *   check_partition()/blk_add_partitions() → [free_partitions] → vfree()/kfree()
 */
static void free_partitions(struct parsed_partitions *state) /* [한국어] parsed_partitions 해제 함수 시작 */
{
	vfree(state->parts); /* [한국어] vzalloc으로 할당했던 parts[] 배열을 가상메모리 해제 */
	kfree(state); /* [한국어] kzalloc_obj로 할당했던 state 구조체 자체를 해제 */
} /* [한국어] free_partitions() 함수 종료 */

/*
 * [한국어]
 * check_partition() - 디스크에 기록된 파티션 테이블을 프로브(probe)한다.
 *
 * @hd: 파티션 테이블을 검사할 대상 gendisk(전체 디스크). 아직 파티션이
 *      등록되어 있지 않은 "새로 스캔되는" 디스크이거나 재스캔 대상이다.
 * @return: 성공 시 파싱된 파티션 정보를 담은 parsed_partitions 포인터
 *          (state->pp_buf.buffer는 이미 free_page된 상태). 인식 가능한
 *          포맷이 하나도 없으면 ERR_PTR(res)(res는 0 또는 음수 errno,
 *          호출자는 IS_ERR()로 판별). 메모리 부족 시에는 NULL(별개의
 *          "아예 시도조차 못함" 케이스).
 *
 * check_part[] 배열에 등록된 모든 포맷 프로버(msdos_partition,
 * efi_partition, mac_partition 등)를 순서대로 호출해 파티션 테이블을
 * 인식시키는 이 파일의 핵심 디스패처다. 동작 순서: (1) allocate_partitions()
 * 로 parts[] 컨테이너를 만들고, (2) 스캔 로그를 쌓을 4KiB 페이지
 * (pp_buf.buffer)를 할당해 seq_buf로 초기화, (3) state->disk/name을
 * 채우고 이름이 숫자로 끝나면(nvme0n1처럼) 파티션 접미사로 'p'를 준비,
 * (4) check_part[i]를 하나씩 호출하며 각 시도 전에 parts[] 배열을 0으로
 * 리셋(이전 프로버가 남긴 잔여 데이터 제거), (5) 프로버가 음수(I/O 에러)를
 * 반환하면 err에 기록해 두고 res를 0으로 되돌려 다음 프로버를 계속
 * 시도하게 함(하나의 I/O 에러가 다른 포맷의 인식 기회를 막지 않도록),
 * (6) 어떤 프로버가 res>0(파티션 개수)을 반환하면 즉시 성공 처리,
 * (7) 모든 프로버가 실패하면 access_beyond_eod 플래그와 누적된 err를
 * 참고해 최종 에러 코드를 결정하고 로그를 남긴 뒤 리소스를 해제한다.
 * 실행 컨텍스트: blk_add_partitions() 호출자와 동일한 프로세스 컨텍스트
 * (open()/재스캔 경로)에서 동기적으로 실행되며, 각 프로버 함수는 필요 시
 * read_part_sector()를 통해 페이지 캐시(및 캐시 미스 시 실제 블록 I/O)를
 * 유발할 수 있어 이 함수 전체가 블로킹(sleep 가능) 컨텍스트에서 호출되어야
 * 한다(GFP_KERNEL 할당이 사용되는 것과 일치).
 * 호출자(caller): blk_add_partitions().
 * 피호출자(callee): allocate_partitions(), seq_buf_init/printf/puts/str,
 * check_part[i](각 포맷 프로버), free_page(), free_partitions().
 * 에러 경로: 인식 실패 + I/O 에러가 있었던 경우 res에 err를 실어 seq_buf에
 * "unable to read partition table" 메시지를 추가하고, 최종적으로
 * ERR_PTR(res)로 blk_add_partitions()에 알려 -ENOSPC(EOD 초과, 재시도
 * 가능)와 그 외 값을 구분해 처리하게 한다.
 *
 * 호출 체인:
 *   blk_add_partitions() → [check_partition] → allocate_partitions() →
 *     check_part[i](state) (msdos_partition/efi_partition/... )
 */
static struct parsed_partitions *check_partition(struct gendisk *hd) /* [한국어] 파티션 테이블 프로브 함수 시작 */
{
	struct parsed_partitions *state; /* [한국어] 각 프로버의 반환값을 모으는 지역 변수들 - i: 다음 시도할 프로버 인덱스, res: 현재/최종 결과, err: 누적된 I/O 에러 코드 */
	int i, res, err;

	state = allocate_partitions(hd); /* [한국어] parts[] 컨테이너와 parsed_partitions 구조체를 먼저 확보 */
	if (!state) /* [한국어] 메모리 부족 검사 */
		return NULL; /* [한국어] 스캔 자체를 시작할 수 없음 - NULL 반환(ERR_PTR이 아님에 유의) */
	state->pp_buf.buffer = (char *)__get_free_page(GFP_KERNEL); /* [한국어] 스캔 로그 문자열을 쌓을 4KiB 페이지 1장을 GFP_KERNEL로 할당(슬립 가능 컨텍스트에서 호출됨을 전제) */
	if (!state->pp_buf.buffer) { /* [한국어] 페이지 할당 실패 검사 */
		free_partitions(state); /* [한국어] 앞서 allocate_partitions()로 확보한 state를 롤백 - 페이지 실패로 스캔 포기 */
		return NULL; /* [한국어] NULL 반환으로 '스캔 시작 불가'를 알림 */
	} /* [한국어] if 블록 종료 */
	seq_buf_init(&state->pp_buf, state->pp_buf.buffer, PAGE_SIZE); /* [한국어] seq_buf(순차 문자열 버퍼)를 방금 할당한 페이지로 초기화 - 이후 seq_buf_printf/puts로 로그를 이어붙임 */

	state->disk = hd; /* [한국어] 스캔 대상 gendisk를 state에 기록 - read_part_sector() 등 이후 단계가 state->disk로 참조 */
	strscpy(state->name, hd->disk_name); /* [한국어] gendisk 이름(예: 「sda」, 「nvme0n1」)을 state->name 버퍼로 안전하게(널종단 보장) 복사 */
	seq_buf_printf(&state->pp_buf, " %s:", state->name); /* [한국어] 커널 로그 접두어로 「 <이름>:」 형태를 seq_buf에 기록 시작 - 이후 각 프로버가 인식한 파티션 요약이 이어붙여짐 */
	if (isdigit(state->name[strlen(state->name)-1])) /* [한국어] 이름의 마지막 글자가 숫자인지 검사(예: nvme0n1, loop0) - 파티션 번호 접미사와 혼동을 피하기 위해 구분자가 필요한 경우 */
		sprintf(state->name, "p"); /* [한국어] 이름이 숫자로 끝나면 state->name을 「p」로 덮어써 이후 파티션 이름 생성 시 접두어 사이에 'p' 구분자가 들어가게 함(예: nvme0n1 + p + 1 = nvme0n1p1) */

	i = res = err = 0; /* [한국어] 루프 인덱스/결과/에러 누적값을 모두 0으로 초기화(연쇄 대입) */
	while (!res && check_part[i]) { /* [한국어] res가 아직 실패(0)가 아니고 check_part[i]가 NULL(sentinel)이 아닌 동안 반복 - 즉 '아직 인식 못했고 시도할 프로버가 남은 동안' */
		memset(state->parts, 0, state->limit * sizeof(state->parts[0])); /* [한국어] 이전 프로버 시도에서 parts[]에 남겼을 수 있는 부분 결과를 0으로 리셋 - 각 프로버는 매번 깨끗한 상태에서 시작해야 함 */
		res = check_part[i++](state); /* [한국어] i번째 프로버를 호출하고 후위 증가로 다음 회차를 준비; 반환값(파싱된 파티션 개수 또는 <=0)을 res에 저장 */
		if (res < 0) { /* [한국어] 프로버가 음수를 반환하면 I/O 에러가 발생한 것으로 간주 */
			/*
			 * We have hit an I/O error which we don't report now.
			 * But record it, and let the others do their job.
			 */
			err = res; /* [한국어] 지금 발생한 I/O 에러 코드를 err에 보관(추후 모든 프로버가 실패했을 때만 보고하기 위함) */
			res = 0; /* [한국어] res를 0으로 되돌려 while 조건이 계속되게(다음 포맷 프로버를 마저 시도) 함 */
		} /* [한국어] if(res<0) 블록 종료 */

	} /* [한국어] while 루프 종료 */
	if (res > 0) { /* [한국어] 어떤 프로버든 res>0(파티션 인식 성공)을 반환했으면 성공 경로로 진입 */
		printk(KERN_INFO "%s", seq_buf_str(&state->pp_buf)); /* [한국어] 지금까지 쌓인 스캔 로그 문자열을 KERN_INFO 레벨로 커널 로그에 출력 */

		free_page((unsigned long)state->pp_buf.buffer); /* [한국어] 로그용으로 빌렸던 페이지를 해제 - 더 이상 seq_buf에 쓸 일이 없음 */
		return state; /* [한국어] 파싱된 파티션 정보(state)를 호출자(blk_add_partitions)에게 반환 */
	} /* [한국어] if(res>0) 블록 종료 */
	if (state->access_beyond_eod) /* [한국어] 어떤 프로버라도 EOD(디스크 끝)를 넘어 읽으려 했다면 access_beyond_eod 플래그가 설정되어 있음 */
		err = -ENOSPC; /* [한국어] 이 경우 최종 에러코드를 -ENOSPC로 지정 - blk_add_partitions()가 native capacity unlock 재시도를 판단하는 신호가 됨 */
	/*
	 * The partition is unrecognized. So report I/O errors if there were any
	 */
	if (err) /* [한국어] 그 외의 경우, 누적된 I/O 에러(err)가 있었다면 그것을 최종 결과(res)로 채택 */
		res = err; /* [한국어] err를 res에 반영 */
	if (res) { /* [한국어] 최종적으로 에러가 남아 있는 경우(res != 0) 로그에 실패 메시지를 남긴다 */
		seq_buf_puts(&state->pp_buf,
			     " unable to read partition table\n"); /* [한국어] 「unable to read partition table」 메시지를 seq_buf에 추가 */
		printk(KERN_INFO "%s", seq_buf_str(&state->pp_buf)); /* [한국어] 완성된 로그 문자열을 KERN_INFO로 출력 */
	} /* [한국어] if(res) 블록 종료 */

	free_page((unsigned long)state->pp_buf.buffer); /* [한국어] 실패 경로에서도 로그용 페이지를 반드시 해제 */
	free_partitions(state); /* [한국어] parts[] 배열과 state 구조체 전체를 해제(메모리 누수 방지) */
	return ERR_PTR(res); /* [한국어] res(0 또는 음수 errno)를 ERR_PTR로 감싸 반환 - 호출자는 IS_ERR()로 실패를 판별 */
} /* [한국어] check_partition() 함수 종료 */

/*
 * [한국어]
 * part_partition_show() - sysfs "partition" 속성: 파티션 번호 출력.
 *
 * @dev: /sys/.../<part>/ 의 struct device (block_device를 감싸는 장치).
 * @attr: 이 속성을 기술하는 device_attribute(사용되지 않음, sysfs 콜백
 *        시그니처 규약상 존재).
 * @buf: sysfs read(2)가 사용자 공간에 돌려줄 PAGE_SIZE 버퍼.
 * @return: buf에 쓴 바이트 수(sysfs_emit의 반환값).
 *
 * dev_to_bdev()로 device를 감싸는 block_device를 얻고, bdev_partno()로
 * 그 파티션 번호(1부터 시작, whole-disk는 0)를 십진수 문자열로 출력한다.
 * 실행 컨텍스트: sysfs read(2) 시스템 호출을 처리하는 프로세스 컨텍스트.
 * 호출자(caller): sysfs VFS 계층(커널 sysfs 파일 read 핸들러가 등록된
 * dev_attr_partition.show를 통해 간접 호출).
 * 피호출자(callee): dev_to_bdev(), bdev_partno(), sysfs_emit().
 * 에러 경로: 별도 실패 없음(항상 성공).
 *
 * 호출 체인:
 *   sysfs read("partition") → [part_partition_show] → bdev_partno()
 */
static ssize_t part_partition_show(struct device *dev, /* [한국어] 「partition」 속성 show 콜백 시작 - 파라미터는 sysfs show 콜백 표준 시그니처 */
				   struct device_attribute *attr, char *buf) /* [한국어] attr는 사용하지 않지만 device_attribute의 show 콜백 시그니처를 맞추기 위해 필요 */
{
	return sysfs_emit(buf, "%d\n", bdev_partno(dev_to_bdev(dev))); /* [한국어] dev로부터 block_device를 얻고 그 파티션 번호를 10진 정수 문자열로 buf에 기록 */
} /* [한국어] part_partition_show() 함수 종료 */

/*
 * [한국어]
 * part_start_show() - sysfs "start" 속성: 파티션 시작 섹터 출력.
 *
 * @dev: /sys/.../<part>/ 의 struct device.
 * @attr: 사용되지 않음(콜백 시그니처 규약).
 * @buf: 출력 버퍼.
 * @return: 기록한 바이트 수.
 *
 * block_device->bd_start_sect(전체 디스크 기준 시작 섹터, 512B 단위가
 * 아니라 논리 섹터 단위)를 부호 없는 64비트 정수로 출력한다. 이 값은
 * add_partition() 호출 시 전달된 start 인자가 그대로 저장된 것이다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * 호출자(caller): sysfs VFS 계층.
 * 피호출자(callee): dev_to_bdev(), sysfs_emit().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   sysfs read("start") → [part_start_show] → bd_start_sect 필드 읽기
 */
static ssize_t part_start_show(struct device *dev, /* [한국어] 「start」 속성 show 콜백 시작 */
			       struct device_attribute *attr, char *buf) /* [한국어] attr 미사용, buf에 결과 기록 */
{
	return sysfs_emit(buf, "%llu\n", dev_to_bdev(dev)->bd_start_sect); /* [한국어] block_device의 시작 섹터(bd_start_sect)를 %llu로 출력 - add_partition()에서 설정된 값 */
} /* [한국어] part_start_show() 함수 종료 */

/*
 * [한국어]
 * part_ro_show() - sysfs "ro" 속성: 읽기 전용 여부 출력.
 *
 * @dev: /sys/.../<part>/ 의 struct device.
 * @attr: 사용되지 않음.
 * @buf: 출력 버퍼.
 * @return: 기록한 바이트 수.
 *
 * bdev_read_only()로 이 파티션(혹은 전체 디스크)이 읽기 전용으로
 * 마운트/설정되었는지(0 또는 1)를 확인해 정수 문자열로 출력한다.
 * 읽기 전용 여부는 ADDPART_FLAG_READONLY 플래그로 add_partition() 시
 * BD_READ_ONLY 비트가 설정되었는지, 혹은 상위 디스크 자체가 읽기
 * 전용인지에 따라 결정된다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * 호출자(caller): sysfs VFS 계층.
 * 피호출자(callee): dev_to_bdev(), bdev_read_only(), sysfs_emit().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   sysfs read("ro") → [part_ro_show] → bdev_read_only()
 */
static ssize_t part_ro_show(struct device *dev, /* [한국어] 「ro」 속성 show 콜백 시작 */
			    struct device_attribute *attr, char *buf) /* [한국어] attr 미사용, buf에 결과 기록 */
{
	return sysfs_emit(buf, "%d\n", bdev_read_only(dev_to_bdev(dev))); /* [한국어] 읽기 전용 여부(0/1)를 정수 문자열로 출력 - BD_READ_ONLY 플래그 상태를 반영 */
} /* [한국어] part_ro_show() 함수 종료 */

/*
 * [한국어]
 * part_alignment_offset_show() - sysfs "alignment_offset" 속성 출력.
 *
 * @dev: /sys/.../<part>/ 의 struct device.
 * @attr: 사용되지 않음.
 * @buf: 출력 버퍼.
 * @return: 기록한 바이트 수.
 *
 * bdev_alignment_offset()으로 파티션 시작 위치가 하위 저장 매체의
 * 물리 블록 경계와 얼마나 어긋나 있는지(바이트 단위 오프셋)를 계산해
 * 출력한다. SSD/AF(Advanced Format) HDD처럼 논리 섹터와 물리 섹터
 * 크기가 다른 장치에서 파티션 정렬 최적화에 사용되는 값이다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * 호출자(caller): sysfs VFS 계층.
 * 피호출자(callee): dev_to_bdev(), bdev_alignment_offset(), sysfs_emit().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   sysfs read("alignment_offset") → [part_alignment_offset_show]
 *     → bdev_alignment_offset()
 */
static ssize_t part_alignment_offset_show(struct device *dev, /* [한국어] 「alignment_offset」 속성 show 콜백 시작 */
					  struct device_attribute *attr, char *buf) /* [한국어] attr 미사용, buf에 결과 기록 */
{
	return sysfs_emit(buf, "%u\n", bdev_alignment_offset(dev_to_bdev(dev))); /* [한국어] 파티션 시작 위치와 물리 블록 경계 사이의 정렬 오프셋(바이트)을 부호 없는 정수로 출력 */
} /* [한국어] part_alignment_offset_show() 함수 종료 */

/*
 * [한국어]
 * part_discard_alignment_show() - sysfs "discard_alignment" 속성 출력.
 *
 * @dev: /sys/.../<part>/ 의 struct device.
 * @attr: 사용되지 않음.
 * @buf: 출력 버퍼.
 * @return: 기록한 바이트 수.
 *
 * bdev_discard_alignment()으로 discard(TRIM/UNMAP/Deallocate) 요청이
 * 최적의 성능을 내기 위해 정렬되어야 할 오프셋을 계산해 출력한다.
 * SSD/NVMe에서 Deallocate 명령이 내부 소거 단위(erase block)와 맞지
 * 않으면 write amplification이 늘어날 수 있어, 이 값을 참고해 파일
 * 시스템이나 관리 도구가 discard 요청 정렬을 최적화할 수 있다.
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트.
 * 호출자(caller): sysfs VFS 계층.
 * 피호출자(callee): dev_to_bdev(), bdev_discard_alignment(), sysfs_emit().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   sysfs read("discard_alignment") → [part_discard_alignment_show]
 *     → bdev_discard_alignment()
 */
static ssize_t part_discard_alignment_show(struct device *dev, /* [한국어] 「discard_alignment」 속성 show 콜백 시작 */
					   struct device_attribute *attr, char *buf) /* [한국어] attr 미사용, buf에 결과 기록 */
{
	return sysfs_emit(buf, "%u\n", bdev_discard_alignment(dev_to_bdev(dev))); /* [한국어] discard(TRIM) 요청 정렬 오프셋(바이트)을 부호 없는 정수로 출력 */
} /* [한국어] part_discard_alignment_show() 함수 종료 */

/*
 * [한국어]
 * 아래 DEVICE_ATTR(...) 매크로 호출들은 각각 정적 struct device_attribute
 * 인스턴스(dev_attr_<name>)를 생성한다. DEVICE_ATTR(name, mode, show, store)는
 * "name" sysfs 파일 이름, 권한 mode, 읽기 시 호출될 show 콜백, 쓰기 시 호출될
 * store 콜백(NULL이면 쓰기 불가)을 지정하는 표준 커널 매크로다. 여기서는
 * store가 모두 NULL이므로 전부 읽기 전용(0444) 속성이며, part_stat_show/
 * part_inflight_show는 이 파일이 아니라 block/genhd.c(및 선언은 block/blk.h)
 * 에 정의되어 있다 - 이 파일은 파티션 프레임워크이고 I/O 통계 집계 로직은
 * genhd.c의 공용 코드가 담당하기 때문이다.
 */
static DEVICE_ATTR(partition, 0444, part_partition_show, NULL); /* [한국어] /sys/.../<part>/partition - 파티션 번호(위 part_partition_show 콜백) */
static DEVICE_ATTR(start, 0444, part_start_show, NULL); /* [한국어] /sys/.../<part>/start - 시작 섹터(위 part_start_show 콜백) */
static DEVICE_ATTR(size, 0444, part_size_show, NULL); /* [한국어] /sys/.../<part>/size - 크기(섹터) - part_size_show는 block/genhd.c에 정의된 공용 콜백 재사용 */
static DEVICE_ATTR(ro, 0444, part_ro_show, NULL); /* [한국어] /sys/.../<part>/ro - 읽기 전용 여부(위 part_ro_show 콜백) */
static DEVICE_ATTR(alignment_offset, 0444, part_alignment_offset_show, NULL); /* [한국어] /sys/.../<part>/alignment_offset - 정렬 오프셋(위 part_alignment_offset_show 콜백) */
static DEVICE_ATTR(discard_alignment, 0444, part_discard_alignment_show, NULL); /* [한국어] /sys/.../<part>/discard_alignment - discard 정렬(위 part_discard_alignment_show 콜백) */
static DEVICE_ATTR(stat, 0444, part_stat_show, NULL); /* [한국어] /sys/.../<part>/stat - I/O 통계(part_stat_show는 genhd.c의 공용 구현) */
static DEVICE_ATTR(inflight, 0444, part_inflight_show, NULL); /* [한국어] /sys/.../<part>/inflight - 진행 중 I/O 수(part_inflight_show는 genhd.c의 공용 구현) */
#ifdef CONFIG_FAIL_MAKE_REQUEST /* [한국어] CONFIG_FAIL_MAKE_REQUEST(fault injection) 빌드에서만 make-it-fail 속성 포함 */
static struct device_attribute dev_attr_fail = /* [한국어] make-it-fail은 0444가 아닌 0644(쓰기 가능) 속성이라 DEVICE_ATTR 매크로 대신 __ATTR을 직접 사용해 struct device_attribute를 수동 초기화 */
	__ATTR(make-it-fail, 0644, part_fail_show, part_fail_store); /* [한국어] 읽기: part_fail_show, 쓰기: part_fail_store(둘 다 genhd.c 공용 구현) - echo 1 > make-it-fail로 해당 파티션 I/O를 인위적으로 실패시킬 수 있음 */
#endif /* [한국어] CONFIG_FAIL_MAKE_REQUEST #ifdef 종료 */

/*
 * [한국어]
 * part_attrs[] - 위에서 정의한 device_attribute들을 struct attribute
 * 포인터 배열로 모은 것. sysfs의 attribute_group 메커니즘은 각 속성을
 * struct attribute*(공통 베이스)로 취급하므로 &dev_attr_xxx.attr 형태로
 * 캐스팅 없이 임베디드 필드의 주소를 넘긴다. NULL 종단으로 배열 길이를
 * 표시한다(sysfs 코드가 순회 시 이 NULL을 감지해 멈춘다).
 */
static struct attribute *part_attrs[] = { /* [한국어] 속성 포인터 배열 시작 */
	&dev_attr_partition.attr, /* [한국어] 「partition」 속성 */
	&dev_attr_start.attr, /* [한국어] 「start」 속성 */
	&dev_attr_size.attr, /* [한국어] 「size」 속성 */
	&dev_attr_ro.attr, /* [한국어] 「ro」 속성 */
	&dev_attr_alignment_offset.attr, /* [한국어] 「alignment_offset」 속성 */
	&dev_attr_discard_alignment.attr, /* [한국어] 「discard_alignment」 속성 */
	&dev_attr_stat.attr, /* [한국어] 「stat」 속성 */
	&dev_attr_inflight.attr, /* [한국어] 「inflight」 속성 */
#ifdef CONFIG_FAIL_MAKE_REQUEST /* [한국어] CONFIG_FAIL_MAKE_REQUEST 빌드에서만 다음 항목 포함 */
	&dev_attr_fail.attr, /* [한국어] 「make-it-fail」 속성 */
#endif /* [한국어] #ifdef 종료 */
	NULL /* [한국어] 배열 종단 sentinel */
}; /* [한국어] part_attrs[] 배열 정의 종료 */

/*
 * [한국어]
 * part_attr_group - part_attrs[]를 감싸는 attribute_group.
 *
 * struct attribute_group의 .attrs 필드:
 *   설정자: 이 정의에서 part_attrs[] 배열 주소로 고정 초기화(런타임 불변).
 *   읽는 자: sysfs 코어가 그룹을 등록/해제할 때 순회하며 각 struct
 *            attribute에 대해 실제 파일 노드를 만든다.
 *   값 범위: NULL 종단 배열의 시작 주소 - 항상 유효한 정적 배열.
 *   동기화: const로 선언되어 있어 런타임 변경이 없으므로 락 불필요.
 */
static const struct attribute_group part_attr_group = { /* [한국어] attribute_group 인스턴스 정의 시작 - part_type.groups에 연결되어 device_add() 시 sysfs에 등록됨 */
	.attrs = part_attrs, /* [한국어] .attrs 필드에 위 part_attrs[] 배열 연결 */
}; /* [한국어] part_attr_group 정의 종료 */

/*
 * [한국어]
 * part_attr_groups[] - part_type.groups에 연결될 attribute_group 포인터
 * 배열. CONFIG_BLK_DEV_IO_TRACE(blktrace)가 활성화된 커널에서는
 * blk_trace_attr_group(선언은 block/blk.h, 정의는 blktrace 코어)도 함께
 * 등록되어 /sys/.../<part>/trace/ 하위에 blktrace 제어 파일이 추가된다.
 * NULL 종단으로 배열 길이를 표시한다.
 */
static const struct attribute_group *part_attr_groups[] = { /* [한국어] attribute_group 포인터 배열 시작 - device_add()가 이 배열을 순회하며 sysfs 파일들을 생성 */
	&part_attr_group, /* [한국어] 위에서 정의한 파티션 기본 속성 그룹 */
#ifdef CONFIG_BLK_DEV_IO_TRACE /* [한국어] blktrace(I/O 추적) 기능이 빌드된 경우에만 다음 그룹 포함 */
	&blk_trace_attr_group, /* [한국어] blktrace 관련 sysfs 속성 그룹(block/blk.h에 extern 선언, blktrace 코어에서 정의) */
#endif /* [한국어] CONFIG_BLK_DEV_IO_TRACE #ifdef 종료 */
	NULL /* [한국어] 배열 종단 sentinel */
}; /* [한국어] part_attr_groups[] 배열 정의 종료 */

/*
 * [한국어]
 * part_release() - 파티션 struct device의 참조 카운트가 0이 될 때
 * 커널 device 코어가 호출하는 최종 해제(release) 콜백.
 *
 * @dev: 참조 카운트가 0이 된 파티션의 struct device(embedded in
 *       block_device->bd_device).
 * @return: 없음(void). device_type.release 콜백 시그니처가 void이므로
 *          실패를 표현할 방법이 없다 - 반드시 성공해야 하는 정리 코드.
 *
 * device_del()+put_device()로 마지막 참조가 사라지면 device 코어가
 * 이 콜백을 통해 device를 감싸는 상위 객체(block_device)를 실제로
 * 해제하도록 위임한다. put_disk()로 이 파티션이 속한 gendisk의 참조를
 * 낮추고(파티션이 gendisk 존속 기간 동안 gendisk를 붙들고 있었으므로),
 * bdev_drop()으로 block_device 구조체 자체의 메모리를 반납한다.
 * 실행 컨텍스트: 임의의 put_device() 호출 컨텍스트(주로 drop_partition()
 * 이후 마지막 참조가 해제되는 시점) - 인터럽트 컨텍스트가 아닌 프로세스
 * 컨텍스트로 가정된다(bdev_drop 내부에서 슬립 가능한 정리를 수행할 수
 * 있으므로).
 * 호출자(caller): 커널 device 코어(kobject_release → device_release →
 * 이 함수)가 참조 카운트 0 시점에 자동 호출. 직접 호출되지 않는다.
 * 피호출자(callee): dev_to_bdev(), put_disk(), bdev_drop().
 * 에러 경로: 없음(반드시 성공).
 *
 * 호출 체인:
 *   put_device() (마지막 참조 해제) → device 코어 → [part_release]
 *     → put_disk()/bdev_drop()
 */
static void part_release(struct device *dev) /* [한국어] 파티션 device release 콜백 시작 - 이 파티션의 마지막 참조가 해제될 때 device 코어가 호출 */
{
	put_disk(dev_to_bdev(dev)->bd_disk); /* [한국어] add_partition()에서 get_device(disk_to_dev(disk))로 올려두었던 gendisk 참조를 반대로 낮춤(put_disk) */
	bdev_drop(dev_to_bdev(dev)); /* [한국어] block_device 구조체 자체의 메모리를 반납 - bdev_alloc()의 대응 해제 */
} /* [한국어] part_release() 함수 종료 */

/*
 * [한국어]
 * part_uevent() - 파티션 관련 uevent(KOBJ_ADD/CHANGE/REMOVE 등) 발생 시
 * 환경 변수를 채워주는 device_type.uevent 콜백.
 *
 * @dev: uevent가 발생하는 파티션의 struct device(const로, 이 콜백은
 *       dev의 상태를 읽기만 함).
 * @env: udev 등 사용자 공간에 전달될 환경 변수 목록을 채울 버퍼
 *       (kobj_uevent_env). add_uevent_var()로 "KEY=value" 형태 문자열을
 *       계속 추가한다.
 * @return: 항상 0(add_uevent_var 실패는 이 함수에서 별도로 검사하지
 *          않음 - env 버퍼가 가득 차면 add_uevent_var 자체가 내부적으로
 *          에러를 env->error에 기록하고 이후 호출은 무시됨).
 *
 * udev가 /dev 노드를 만들거나 규칙을 매칭할 때 참고할 수 있도록
 * PARTN(파티션 번호), 그리고 GPT 등에서 얻은 PARTNAME/PARTUUID(있는
 * 경우에만)를 uevent 환경 변수로 추가한다. bd_meta_info는 add_partition()
 * 호출 시 info 인자가 있었던 경우에만(즉 GPT처럼 이름/UUID 메타데이터를
 * 지원하는 포맷에서 인식된 파티션만) 채워지므로, 매번 NULL 체크와 빈
 * 문자열 체크(volname[0]/uuid[0])를 먼저 수행한다.
 * 실행 컨텍스트: kobject_uevent() 호출 컨텍스트(주로 add_partition()의
 * KOBJ_ADD 경로, device_add()의 내부 KOBJ_ADD 등) - 프로세스 컨텍스트.
 * 호출자(caller): 커널 device/kobject 코어가 uevent 생성 시 자동 호출.
 * 피호출자(callee): dev_to_bdev(), bdev_partno(), add_uevent_var().
 * 에러 경로: 실패를 이 함수 차원에서 별도로 처리하지 않음(항상 0 반환).
 *
 * 호출 체인:
 *   kobject_uevent(KOBJ_ADD/...) → device 코어 → [part_uevent]
 *     → add_uevent_var()
 */
static int part_uevent(const struct device *dev, struct kobj_uevent_env *env) /* [한국어] 파티션 uevent 콜백 시작 - dev는 const(읽기 전용 접근) */
{
	const struct block_device *part = dev_to_bdev(dev); /* [한국어] dev를 감싸는 block_device를 상수 포인터로 획득(이 함수는 상태를 변경하지 않음) */

	add_uevent_var(env, "PARTN=%u", bdev_partno(part)); /* [한국어] PARTN=<파티션 번호> 환경 변수 추가 - 예: PARTN=1 */
	if (part->bd_meta_info && part->bd_meta_info->volname[0]) /* [한국어] GPT 등에서 파싱된 볼륨 이름이 존재하는지 확인(메타정보 포인터 NULL 아님 + 첫 글자가 널 문자가 아님) */
		add_uevent_var(env, "PARTNAME=%s", part->bd_meta_info->volname); /* [한국어] PARTNAME=<볼륨 이름> 환경 변수 추가 */
	if (part->bd_meta_info && part->bd_meta_info->uuid[0]) /* [한국어] GPT 등에서 파싱된 PARTUUID가 존재하는지 확인 */
		add_uevent_var(env, "PARTUUID=%s", part->bd_meta_info->uuid); /* [한국어] PARTUUID=<UUID 문자열> 환경 변수 추가 */
	return 0; /* [한국어] 항상 성공(0)을 반환 - uevent 콜백 표준 관례 */
} /* [한국어] part_uevent() 함수 종료 */

/*
 * [한국어]
 * part_type - 파티션 struct device들이 공유하는 device_type 인스턴스.
 * 이 심볼은 static이 아니라 전역(non-static)으로 선언되어 있어 다른
 * 파일(예: block/genhd.c의 bdev_is_partition() 계열 헬퍼가 dev->type ==
 * &part_type 비교로 "이 device가 파티션인지 whole-disk인지"를 판별하는
 * 용도)에서도 참조된다.
 *
 * struct device_type의 각 필드:
 *
 * .name = "partition"
 *   설정자: 이 정의에서 문자열 리터럴로 고정.
 *   읽는 자: /sys/.../uevent 파일 등에서 DEVTYPE=partition으로 노출되어
 *            udev 규칙이 "이 장치가 파티션이다"를 식별하는 데 사용.
 *   값 범위: 항상 "partition" 고정 문자열.
 *   동기화: 불변 정적 데이터, 락 불필요.
 *
 * .groups = part_attr_groups
 *   설정자: 위에서 정의한 part_attr_groups[] 배열 주소.
 *   읽는 자: device_add()가 이 배열을 순회하며 sysfs 속성 파일들
 *            (partition/start/size/stat/... )을 자동 생성.
 *   값 범위: NULL 종단 attribute_group* 배열의 시작 주소.
 *   동기화: 정적 데이터, 락 불필요.
 *
 * .release = part_release
 *   설정자: 위에서 정의한 콜백 함수 포인터.
 *   읽는 자: device 코어가 이 struct device의 마지막 참조가 해제될 때
 *            자동 호출.
 *   값 범위: 항상 유효한 함수 포인터(part_release).
 *   동기화: 콜백 자체는 device 코어의 kref 규칙을 따름(참조 카운트 0
 *           전이는 원자적으로 보장됨).
 *
 * .uevent = part_uevent
 *   설정자: 위에서 정의한 콜백 함수 포인터.
 *   읽는 자: device 코어가 uevent(KOBJ_ADD/CHANGE/REMOVE) 발생 시
 *            환경 변수를 채우기 위해 호출.
 *   값 범위: 항상 유효한 함수 포인터(part_uevent).
 *   동기화: 콜백 호출 시점은 device 코어가 직렬화(각 kobject의 uevent
 *           발생은 순차적으로 처리됨).
 */
const struct device_type part_type = { /* [한국어] const 이므로 런타임에 필드를 바꿀 수 없음 - 컴파일 타임에 4개 콜백/이름/그룹이 모두 확정 */
	.name		= "partition", /* [한국어] sysfs DEVTYPE 값으로 노출되는 이름 */
	.groups		= part_attr_groups, /* [한국어] 자동 등록될 sysfs 속성 그룹 배열 */
	.release	= part_release, /* [한국어] 마지막 참조 해제 시 호출될 콜백 */
	.uevent		= part_uevent, /* [한국어] uevent 발생 시 환경 변수를 채울 콜백 */
}; /* [한국어] part_type 정의 종료 */

/*
 * [한국어]
 * drop_partition() - 이미 등록되어 있는 파티션 하나를 완전히 제거한다.
 *
 * @part: 제거할 파티션의 block_device. part->bd_disk->part_tbl에
 *        등록되어 있어야 하며, 호출 전 이미 bdev_unhash()/invalidate_bdev()
 *        등으로 "사용 중단" 처리가 끝난 상태여야 한다(이 함수는 그
 *        전처리를 하지 않고 바로 등록 해제/디바이스 삭제만 수행).
 * @return: 없음(void).
 *
 * gendisk->part_tbl(xarray, 파티션 번호 → block_device)에서 항목을
 * 지우고, /sys/.../holders kobject 참조를 낮추고, device_del()로
 * sysfs/devtmpfs에서 파티션 노드를 제거한 뒤 put_device()로 참조를
 * 낮춘다(실제 메모리 해제는 참조 카운트가 0이 되는 시점에 part_release()
 * 콜백에서 이루어짐 - 이 함수가 반드시 즉시 해제를 의미하진 않음).
 * 실행 컨텍스트: 호출자가 disk->open_mutex를 이미 들고 있는 상태에서
 * 호출되어야 한다(lockdep_assert_held로 강제). 이는 part_tbl 갱신과
 * bdev 상태 변화가 다른 열기/재스캔 시도와 경합하지 않도록 보장한다.
 * 호출자(caller): bdev_del_partition()(사용자 요청 파티션 삭제),
 * bdev_disk_changed()(전체 재스캔 시 기존 파티션 일괄 제거).
 * 피호출자(callee): lockdep_assert_held(), xa_erase(), kobject_put(),
 * device_del(), put_device().
 * 에러 경로: 반환값이 없으므로 실패를 표현하지 않는다(호출자가 사전
 * 조건 - part_tbl에 실제로 존재 - 을 이미 확인했다고 가정).
 *
 * 호출 체인:
 *   bdev_del_partition()/bdev_disk_changed() → [drop_partition]
 *     → xa_erase()/device_del()/put_device()
 */
void drop_partition(struct block_device *part) /* [한국어] 파티션 제거 함수 시작 - 전역(non-static) 심볼로 다른 파일에서도 호출 가능 */
{
	lockdep_assert_held(&part->bd_disk->open_mutex); /* [한국어] 호출자가 open_mutex를 들고 있는지 lockdep으로 검증 - part_tbl 변경은 항상 이 락 아래에서만 허용 */

	xa_erase(&part->bd_disk->part_tbl, bdev_partno(part)); /* [한국어] gendisk의 part_tbl(xarray)에서 이 파티션 번호에 해당하는 항목을 삭제 - 이후 xa_load로 조회 불가 */
	kobject_put(part->bd_holder_dir); /* [한국어] add_partition()에서 kobject_create_and_add()로 만들었던 holders 디렉터리의 참조를 낮춤 */

	device_del(&part->bd_device); /* [한국어] sysfs/devtmpfs에서 이 파티션의 device 노드를 제거(예: /dev/sda1이 더 이상 보이지 않게 됨) */
	put_device(&part->bd_device); /* [한국어] device 참조 카운트를 낮춤 - 0이 되면 part_release() 콜백에서 실제 메모리 해제가 일어남 */
} /* [한국어] drop_partition() 함수 종료 */

/*
 * [한국어]
 * whole_disk_show() - sysfs "whole_disk" 속성: 항상 빈 값(0바이트)을
 * 반환하는 마커(marker) 파일.
 *
 * @dev: 사용되지 않음.
 * @attr: 사용되지 않음.
 * @buf: 사용되지 않음(아무것도 쓰지 않음).
 * @return: 항상 0(0바이트 기록 - 즉 파일 내용이 비어 있음).
 *
 * 이 파일 자체의 "존재 여부"가 정보다: ADDPART_FLAG_WHOLEDISK 플래그로
 * add_partition()이 호출되었을 때만 device_create_file()로 이 속성이
 * 동적으로 추가되므로, 사용자 공간은 /sys/.../whole_disk 파일이
 * 존재하는지만 확인하면 이 block_device가 "파티션이 아니라 전체
 * 디스크를 나타내는 특수 등록"인지 구분할 수 있다(내용 자체는 의미가
 * 없음 - 존재만이 신호).
 * 실행 컨텍스트: sysfs read(2) 프로세스 컨텍스트(호출될 일이 거의
 * 없음 - 보통 존재 여부만 stat(2)/access(2)로 확인됨).
 * 호출자(caller): sysfs VFS 계층(파일이 실제로 read되는 드문 경우).
 * 피호출자(callee): 없음.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   sysfs read("whole_disk") → [whole_disk_show] (내용 없이 0 반환)
 */
static ssize_t whole_disk_show(struct device *dev, /* [한국어] whole_disk 마커 파일 show 콜백 시작 - 파라미터 3개 모두 미사용 */
			       struct device_attribute *attr, char *buf) /* [한국어] 미사용 파라미터들 */
{
	return 0; /* [한국어] 항상 0바이트를 기록(즉 아무 내용도 쓰지 않음) - 파일의 존재 자체가 「whole-disk 등록」이라는 신호 */
} /* [한국어] whole_disk_show() 함수 종료 */
static const DEVICE_ATTR(whole_disk, 0444, whole_disk_show, NULL); /* [한국어] 0444(읽기 전용) whole_disk 속성 정의 - add_partition()에서 ADDPART_FLAG_WHOLEDISK일 때만 device_create_file()로 동적 추가됨 */

/*
 * [한국어]
 * add_partition() - gendisk 위에 새 파티션 block_device를 생성/등록한다.
 *
 * @disk: 파티션을 추가할 대상 gendisk(전체 디스크).
 * @partno: 부여할 파티션 번호(1..DISK_MAX_PARTS-1). 0은 전체 디스크
 *          자신(disk->part0)이 이미 사용 중이므로 파티션에는 쓰이지 않음.
 * @start: 파티션의 시작 섹터(디스크 전체 기준, 논리 섹터 단위).
 * @len: 파티션의 길이(섹터 수).
 * @flags: ADDPART_FLAG_NONE/RAID/WHOLEDISK/READONLY의 비트 OR 조합.
 * @info: GPT 등에서 얻은 파티션 이름/UUID 메타데이터(NULL이면 없음).
 * @return: 성공 시 새로 생성된 block_device 포인터, 실패 시
 *          ERR_PTR(-EINVAL|-ENXIO|-EBUSY|-ENOMEM 등).
 *
 * 아래 "Must be called..." 원본 주석대로, 이 함수는 반드시
 * disk->open_mutex를 쥔 상태에서 - 디스크가 아직 open되기 전이거나
 * 모든 opener가 사라진 뒤 - 호출되어야 한다. 이는 part_tbl과 devt
 * 네임스페이스가 동시에 두 번 같은 partno로 채워지는 것을 막기 위함이다.
 * 동작 순서(성공 경로): (1) partno 상한 검사, (2) host-managed zoned
 * 디스크는 파티션 자체를 지원하지 않으므로 거부, (3) 이미 같은 partno가
 * 등록되어 있으면 -EBUSY, (4) 전체 디스크 참조를 get_device()로 올려
 * 파티션이 살아있는 동안 gendisk가 사라지지 않게 보장, (5) bdev_alloc()
 * 으로 block_device를 할당하고 bd_start_sect/bd_nr_sectors를 설정,
 * (6) 디스크 이름이 숫자로 끝나면 "%sp%d", 아니면 "%s%d" 형식으로 장치
 * 이름을 정함(예: nvme0n1 -> nvme0n1p1, sda -> sda1), (7) 파티션 번호가
 * 이 gendisk에 정적으로 할당된 minor 범위(disk->minors) 안에 들어오면
 * 연속 minor를, 아니면 blk_alloc_ext_minor()로 확장 minor 공간에서 동적
 * 할당, (8) info가 있으면 kmemdup으로 메타정보 복사, (9) uevent를
 * 억제한 채 device_add()로 sysfs에 등록, (10) holders 디렉터리 생성,
 * (11) uevent 억제 해제 및 필요 시 whole_disk 속성 파일 추가, (12)
 * READONLY 플래그면 BD_READ_ONLY 설정, (13) part_tbl에 xa_insert로
 * 최종 등록, (14) bdev_add()로 devt를 부여하고 inode 해시에 삽입,
 * (15) 디스크가 uevent를 억제하지 않는 한 KOBJ_ADD를 통지.
 * 각 단계 실패 시 goto out_del/out_put/out_put_disk로 그 시점까지
 * 확보한 자원만 정확히 되돌리는 다단계 롤백 구조를 취한다.
 * 실행 컨텍스트: disk->open_mutex를 보유한 프로세스 컨텍스트(파티션
 * 스캔 경로 또는 ioctl 경로). device_add() 등 슬립 가능한 호출을
 * 포함하므로 인터럽트/RCU 컨텍스트에서 호출 불가.
 * 호출자(caller): blk_add_partition()(파티션 테이블 스캔 결과 등록),
 * bdev_add_partition()(사용자 ioctl 요청).
 * 피호출자(callee): bdev_alloc(), bdev_set_nr_sectors(), device_initialize(),
 * blk_alloc_ext_minor(), kmemdup(), device_add(),
 * kobject_create_and_add(), device_create_file(), xa_insert(), bdev_add(),
 * kobject_uevent().
 * 에러 경로: 각 out_* 레이블이 실패 시점에 따라 필요한 만큼만 정리한다
 * (holders 디렉터리 생성 후 실패하면 kobject_put+device_del까지,
 * device_add 전 실패면 put_device만, bdev_alloc 자체 실패면 put_disk만).
 *
 * 호출 체인:
 *   blk_add_partition()/bdev_add_partition() → [add_partition]
 *     → bdev_alloc() → device_add() → bdev_add()
 */
/*
 * Must be called either with open_mutex held, before a disk can be opened or
 * after all disk users are gone.
 */
static struct block_device *add_partition(struct gendisk *disk, int partno, /* [한국어] 파티션 block_device 생성 함수 시작 - 반환은 성공 시 bdev 포인터, 실패 시 ERR_PTR() */
				sector_t start, sector_t len, int flags,
				struct partition_meta_info *info) /* [한국어] info: GPT 등에서 얻은 파티션 이름/UUID (NULL 가능) */
{
	dev_t devt = MKDEV(0, 0); /* [한국어] devt(디바이스 번호)를 일단 (0,0)으로 초기화 - 아래에서 실제 값을 결정 */
	struct device *ddev = disk_to_dev(disk); /* [한국어] 상위(전체) 디스크의 struct device 포인터 - 새 파티션 device의 parent로 쓰임 */
	struct device *pdev; /* [한국어] 새로 만들 파티션의 struct device 포인터(초기값 미정) */
	struct block_device *bdev; /* [한국어] 새로 만들 block_device 포인터(초기값 미정) */
	const char *dname; /* [한국어] 상위 디스크의 sysfs 장치 이름(「sda」, 「nvme0n1」 등) - 파티션 이름 조합에 사용 */
	int err; /* [한국어] 각 단계의 실패 코드를 담을 변수 */

	lockdep_assert_held(&disk->open_mutex); /* [한국어] 호출자가 open_mutex를 들고 있는지 검증 - part_tbl 갱신은 이 락 아래에서만 안전 */

	if (partno >= DISK_MAX_PARTS) /* [한국어] 파티션 번호가 허용 상한(DISK_MAX_PARTS=256)을 넘으면 즉시 거부 */
		return ERR_PTR(-EINVAL); /* [한국어] 잘못된 인자 에러 반환 */

	/*
	 * Partitions are not supported on zoned block devices that are used as
	 * such.
	 */
	if (bdev_is_zoned(disk->part0)) { /* [한국어] host-managed zoned 블록 디바이스(SMR HDD, ZNS SSD 등)는 zone 순서 쓰기 제약 때문에 파티션 분할을 지원하지 않음 */
		pr_warn("%s: partitions not supported on host managed zoned block device\n", /* [한국어] 사용자에게 zoned 디바이스 파티션 미지원 경고 로그 출력 */
			disk->disk_name);
		return ERR_PTR(-ENXIO); /* [한국어] -ENXIO(장치 또는 주소 없음)로 거부 */
	} /* [한국어] if 블록 종료 */

	if (xa_load(&disk->part_tbl, partno)) /* [한국어] xa_load로 같은 partno가 이미 part_tbl에 등록되어 있는지 확인 */
		return ERR_PTR(-EBUSY); /* [한국어] 이미 존재하면 -EBUSY - 중복 등록 방지 */

	/* ensure we always have a reference to the whole disk */
	get_device(disk_to_dev(disk)); /* [한국어] 전체 디스크의 device 참조 카운트를 하나 올림 - 이 파티션이 살아있는 동안 gendisk가 먼저 해제되지 않도록 보장(part_release에서 put_disk로 대칭적으로 낮춤) */

	err = -ENOMEM; /* [한국어] 이후 실패 경로들의 기본 에러 코드를 -ENOMEM으로 설정 */
	bdev = bdev_alloc(disk, partno); /* [한국어] 파티션 전용 block_device 구조체를 할당(block/bdev.c) - 아직 devt는 미부여 상태 */
	if (!bdev) /* [한국어] 할당 실패 검사 */
		goto out_put_disk; /* [한국어] bdev 할당 실패 시 앞서 올린 디스크 참조를 되돌리는 out_put_disk로 점프 */

	bdev->bd_start_sect = start; /* [한국어] 파티션의 시작 섹터를 기록 - 이후 모든 I/O가 이 오프셋을 기준으로 상위 디스크 LBA로 변환됨 */
	bdev_set_nr_sectors(bdev, len); /* [한국어] 파티션 길이(섹터 수)를 설정 - bdev_set_nr_sectors 내부에서 i_size 등도 함께 갱신(block/bdev.c) */

	pdev = &bdev->bd_device; /* [한국어] 새 block_device에 임베드된 struct device 포인터를 꺼냄 */
	dname = dev_name(ddev); /* [한국어] 상위 디스크의 sysfs 이름을 얻음(예: 「nvme0n1」) */
	if (isdigit(dname[strlen(dname) - 1])) /* [한국어] 상위 디스크 이름이 숫자로 끝나는지 검사(예: nvme0n1의 '1') - 파티션 번호와 시각적으로 구분하기 위해 'p' 구분자가 필요한지 판단 */
		dev_set_name(pdev, "%sp%d", dname, partno); /* [한국어] 숫자로 끝나면 「이름p번호」 형식(예: nvme0n1p1) */
	else
		dev_set_name(pdev, "%s%d", dname, partno); /* [한국어] 그렇지 않으면 「이름번호」 형식(예: sda1) */

	device_initialize(pdev); /* [한국어] device 구조체 기본 필드(kobject 등)를 초기화 - 아직 sysfs에 등록되지는 않음(device_add 전) */
	pdev->class = &block_class; /* [한국어] 블록 장치 클래스로 지정 - udev가 /sys/class/block 아래에서도 이 장치를 찾을 수 있게 함 */
	pdev->type = &part_type; /* [한국어] device_type을 위에서 정의한 part_type으로 지정 - release/uevent/groups 콜백이 여기 연결됨 */
	pdev->parent = ddev; /* [한국어] parent를 상위 디스크의 device로 설정 - /sys/block/<disk>/<part> 계층 구조를 형성 */

	/* in consecutive minor range? */
	if (bdev_partno(bdev) < disk->minors) { /* [한국어] 이 파티션 번호가 gendisk 생성 시 정적으로 예약해둔 minor 범위(disk->minors) 안에 들어오는지 검사 */
		devt = MKDEV(disk->major, disk->first_minor + bdev_partno(bdev)); /* [한국어] 들어오면 major는 디스크와 동일, minor는 first_minor + partno로 계산되는 「연속」 방식 사용 */
	} else { /* [한국어] 정적 minor 범위를 벗어나면(파티션이 많은 디스크) 확장(extended) minor 공간에서 동적 할당 필요 */
		err = blk_alloc_ext_minor(); /* [한국어] ext_devt_ida에서 동적 minor 번호를 하나 할당받음(block/genhd.c) */
		if (err < 0) /* [한국어] 할당 실패(음수) 검사 */
			goto out_put; /* [한국어] 실패 시 지금까지 확보한 bdev/디스크 참조를 되돌리는 out_put으로 점프 */
		devt = MKDEV(BLOCK_EXT_MAJOR, err); /* [한국어] BLOCK_EXT_MAJOR(확장 전용 메이저 번호)와 방금 할당받은 동적 minor로 devt 구성 */
	} /* [한국어] if-else 블록 종료 */
	pdev->devt = devt; /* [한국어] 결정된 devt를 device 구조체에 기록(아직 실제 inode 해시 등록은 아래 bdev_add에서 수행) */

	if (info) { /* [한국어] GPT 등에서 전달받은 메타데이터(info)가 있는 경우에만 복사 */
		err = -ENOMEM; /* [한국어] 복사 실패 시 기본 에러 코드를 -ENOMEM으로 재설정 */
		bdev->bd_meta_info = kmemdup(info, sizeof(*info), GFP_KERNEL); /* [한국어] info 내용을 힙에 복제해 bd_meta_info에 연결 - 호출자가 넘긴 info는 스택 등 임시 버퍼일 수 있어 별도로 소유권을 가져야 함 */
		if (!bdev->bd_meta_info) /* [한국어] 복제 실패 검사 */
			goto out_put; /* [한국어] 실패 시 out_put으로 점프해 지금까지 만든 pdev/bdev/디스크 참조를 정리 */
	} /* [한국어] if 블록 종료 */

	/* delay uevent until 'holders' subdir is created */
	dev_set_uevent_suppress(pdev, 1); /* [한국어] holders 서브디렉터리가 만들어지기 전까지는 uevent를 잠시 억제 - 사용자 공간이 아직 불완전한 sysfs 트리를 보는 것을 방지 */
	err = device_add(pdev); /* [한국어] device_add()로 실제 sysfs/devtmpfs에 파티션 노드를 등록(이 시점부터 /sys/.../<part>가 보이기 시작) */
	if (err) /* [한국어] 등록 실패 검사 */
		goto out_put; /* [한국어] 실패 시 out_put(device_add 실패 - device_del 불필요, put_device만으로 충분)으로 점프 */

	err = -ENOMEM; /* [한국어] 이후 holders 디렉터리 생성 실패 시의 기본 에러 코드를 -ENOMEM으로 설정 */
	bdev->bd_holder_dir = kobject_create_and_add("holders", &pdev->kobj); /* [한국어] /sys/.../<part>/holders/ 디렉터리를 kobject로 생성 - 이 파티션을 사용 중인 상위 계층(예: dm, md)의 심볼릭 링크가 여기 모임 */
	if (!bdev->bd_holder_dir) /* [한국어] 생성 실패 검사 */
		goto out_del; /* [한국어] 실패 시 out_del(device_add까지는 성공했으므로 device_del 필요)로 점프 */

	dev_set_uevent_suppress(pdev, 0); /* [한국어] holders 디렉터리까지 만들어졌으므로 이제 uevent 억제를 해제 - 이후 device_create_file/kobject_uevent가 온전한 sysfs 트리를 대상으로 동작 */
	if (flags & ADDPART_FLAG_WHOLEDISK) { /* [한국어] WHOLEDISK 플래그(파티션 테이블 없이 전체 디스크 자체를 등록하는 특수 경우)면 whole_disk 마커 속성을 추가 */
		err = device_create_file(pdev, &dev_attr_whole_disk); /* [한국어] /sys/.../<part>/whole_disk 파일 생성 시도 */
		if (err) /* [한국어] 생성 실패 검사 */
			goto out_del; /* [한국어] 실패 시 out_del로 점프(device_add+holders까지 성공했으므로 둘 다 정리 필요) */
	} /* [한국어] if 블록 종료 */

	if (flags & ADDPART_FLAG_READONLY) /* [한국어] READONLY 플래그가 지정된 경우(사용자 요청 또는 파티션 테이블상 읽기 전용 표시) */
		bdev_set_flag(bdev, BD_READ_ONLY); /* [한국어] block_device에 BD_READ_ONLY 비트 플래그를 설정 - 이후 write 시도는 -EROFS 등으로 거부됨(다른 코드 경로) */

	/* everything is up and running, commence */
	err = xa_insert(&disk->part_tbl, partno, bdev, GFP_KERNEL); /* [한국어] 여기까지 오면 모든 준비가 끝났다는 뜻 - 이제 part_tbl에 실제로 삽입해 「공식적으로 존재하는」 파티션으로 만든다 */
	if (err) /* [한국어] xa_insert 실패 검사(이론상 앞서 xa_load로 없음을 확인했지만 open_mutex로 직렬화되므로 경합은 없어야 함 - 그래도 방어적으로 확인) */
		goto out_del; /* [한국어] 실패 시 out_del로 점프 */
	bdev_add(bdev, devt); /* [한국어] bdev에 최종 devt를 부여하고 inode 해시에 삽입 - 이 시점부터 /dev/<name> open(2)으로 이 파티션을 찾을 수 있음(block/bdev.c) */

	/* suppress uevent if the disk suppresses it */
	if (!dev_get_uevent_suppress(ddev)) /* [한국어] 상위 디스크 자체가 uevent를 억제하고 있지 않다면(즉 디스크 등록이 정상적으로 진행 중이라면) */
		kobject_uevent(&pdev->kobj, KOBJ_ADD); /* [한국어] 사용자 공간(udev)에 이 파티션이 추가되었음을 KOBJ_ADD로 통지 */
	return bdev; /* [한국어] 성공적으로 생성된 block_device를 호출자에게 반환 */

out_del: /* [한국어] out_del: device_add()와 holders 디렉터리 생성까지는 성공했으나 이후 단계에서 실패한 경우의 정리 레이블 */
	kobject_put(bdev->bd_holder_dir); /* [한국어] holders kobject 참조 해제 */
	device_del(pdev); /* [한국어] sysfs/devtmpfs에서 device 노드 제거(device_add의 역연산) */
out_put: /* [한국어] out_put: device_add() 이전 단계 또는 device_del 이후 공통으로 도달하는 정리 레이블 */
	put_device(pdev); /* [한국어] device 참조를 낮춤 - 0이 되면 이 device의 release 콜백(연결 전이면 아직 part_type이 아닐 수도 있음)이 실제 메모리 정리 수행 */
	return ERR_PTR(err); /* [한국어] 기록된 에러 코드를 ERR_PTR로 감싸 반환 */
out_put_disk: /* [한국어] out_put_disk: bdev_alloc() 자체가 실패해 bdev/pdev가 전혀 만들어지지 않은 가장 이른 실패 지점의 정리 레이블 */
	put_disk(disk); /* [한국어] 앞서 get_device()로 올렸던 상위 디스크 참조를 되돌림(put_disk) */
	return ERR_PTR(err); /* [한국어] 에러 코드를 ERR_PTR로 감싸 반환 */
} /* [한국어] add_partition() 함수 종료 */

/*
 * [한국어]
 * partition_overlaps() - 새 파티션/재조정 범위가 기존 파티션과 겹치는지
 * 검사한다.
 *
 * @disk: 검사 대상 gendisk.
 * @start: 검사할 구간의 시작 섹터.
 * @length: 검사할 구간의 길이(섹터 수).
 * @skip_partno: 비교에서 제외할 파티션 번호(자기 자신과는 당연히
 *               겹치므로, resize 시 자기 자신을 제외하기 위함). 새로
 *              추가하는 경우에는 어떤 partno와도 매칭되지 않을 -1을 전달.
 * @return: 하나라도 겹치면 true, 전혀 겹치지 않으면 false.
 *
 * gendisk->part_tbl(xarray)에 등록된 모든 파티션(1번부터, 0번은
 * whole-disk 자신이므로 xa_for_each_start(..., 1)로 건너뜀)을 순회하며
 * [start, start+length) 구간이 기존 파티션의
 * [bd_start_sect, bd_start_sect+bdev_nr_sectors) 구간과 겹치는지
 * 산술적으로 비교한다. 두 구간이 겹치지 않는 조건은 "하나가 완전히
 * 다른 것보다 앞에 있거나 뒤에 있는 것"이므로, 그 부정(하나라도
 * 겹치는 조건)은 "start < 기존 파티션 끝 AND start+length > 기존
 * 파티션 시작"으로 표현된다.
 * 실행 컨텍스트: RCU read-side critical section 안에서 part_tbl을
 * 순회하므로 락 없이 안전하게 읽기만 가능(단, 이 함수 자체는 쓰기를
 * 하지 않음). 호출자가 이미 open_mutex를 들고 있어 part_tbl에 대한
 * 동시 쓰기는 없다고 가정하지만, RCU를 쓰는 이유는 xa_for_each 자체의
 * 순회 안전성(다른 리더와의 lock-free 공존)을 위함이다.
 * 호출자(caller): bdev_add_partition()(새 파티션 추가 전 충돌 검사),
 * bdev_resize_partition()(크기 조정 후 다른 파티션과 안 겹치는지 검사).
 * 피호출자(callee): rcu_read_lock/unlock(), xa_for_each_start(),
 * bdev_partno(), bdev_nr_sectors().
 * 에러 경로: 없음(단순 불리언 판정 함수).
 *
 * 호출 체인:
 *   bdev_add_partition()/bdev_resize_partition() → [partition_overlaps]
 *     → xa_for_each_start()
 */
static bool partition_overlaps(struct gendisk *disk, sector_t start, /* [한국어] 파티션 겹침 검사 함수 시작 */
		sector_t length, int skip_partno) /* [한국어] skip_partno: resize 시 자기 자신은 비교에서 제외하기 위한 파티션 번호(추가 시엔 -1) */
{
	struct block_device *part; /* [한국어] 순회 중 만나는 기존 파티션을 가리킬 포인터 */
	bool overlap = false; /* [한국어] 결과를 담을 불리언 - 기본값 false(겹치지 않음) */
	unsigned long idx; /* [한국어] xa_for_each_start가 내부적으로 사용할 순회 인덱스 */

	rcu_read_lock(); /* [한국어] xarray 순회를 위한 RCU 읽기 임계구역 진입 */
	xa_for_each_start(&disk->part_tbl, idx, part, 1) { /* [한국어] part_tbl을 인덱스 1부터(0번인 whole-disk 자신은 제외) 순회 */
		if (bdev_partno(part) != skip_partno && /* [한국어] 지금 보고 있는 파티션이 skip 대상(resize 중인 자기 자신)이 아니고 */
		    start < part->bd_start_sect + bdev_nr_sectors(part) && /* [한국어] 새 구간의 시작이 기존 파티션의 끝(시작+길이)보다 앞이면서 */
		    start + length > part->bd_start_sect) { /* [한국어] 새 구간의 끝이 기존 파티션의 시작보다 뒤라면 - 두 구간이 겹친다는 뜻 */
			overlap = true; /* [한국어] 겹침을 발견했음을 기록 */
			break; /* [한국어] 더 순회할 필요 없이 즉시 루프 탈출 */
		} /* [한국어] if 블록 종료 */
	} /* [한국어] xa_for_each_start 루프 종료 */
	rcu_read_unlock(); /* [한국어] RCU 읽기 임계구역 종료 */

	return overlap; /* [한국어] 겹침 여부를 호출자에게 반환 */
} /* [한국어] partition_overlaps() 함수 종료 */

/*
 * [한국어]
 * bdev_add_partition() - 사용자(ioctl) 요청에 따라 파티션을 하나 추가한다.
 *
 * @disk: 파티션을 추가할 대상 gendisk.
 * @partno: 부여할 파티션 번호.
 * @start: 시작 섹터.
 * @length: 길이(섹터 수).
 * @return: 성공 시 0, 실패 시 음수 errno(-ENXIO/-EINVAL/-EBUSY 및
 *          add_partition()이 반환할 수 있는 값들).
 *
 * ioctl(BLKPG, BLKPG_ADD_PARTITION) 등 사용자 공간 요청을 처리하는
 * 최상위 진입점이다. add_partition()과 달리 이 함수는 open_mutex를
 * 직접 획득/해제하며, 디스크가 살아있는지(disk_live), 파티션을 아예
 * 지원하지 않는 디스크는 아닌지(GENHD_FL_NO_PART), 요청 구간이 기존
 * 파티션과 겹치지 않는지(partition_overlaps)를 차례로 검사한 뒤에만
 * 실제 add_partition()을 호출한다.
 * 실행 컨텍스트: ioctl(2) 시스템 호출을 처리하는 프로세스 컨텍스트.
 * mutex_lock으로 인해 슬립 가능해야 한다.
 * 호출자(caller): block/ioctl.c의 BLKPG_ADD_PARTITION 처리 경로(추정 -
 * 이 파일 밖의 코드).
 * 피호출자(callee): mutex_lock/unlock(), disk_live(), partition_overlaps(),
 * add_partition(), PTR_ERR_OR_ZERO().
 * 에러 경로: 각 사전 조건 검사 실패 시 ret에 적절한 errno를 채우고
 * out 레이블로 점프해 반드시 mutex_unlock 후 반환(락 해제 누락 방지를
 * 위한 단일 출구 패턴).
 *
 * 호출 체인:
 *   ioctl(BLKPG_ADD_PARTITION) → [bdev_add_partition] → add_partition()
 */
int bdev_add_partition(struct gendisk *disk, int partno, sector_t start, /* [한국어] 사용자 요청 파티션 추가 함수 시작 - 전역 심볼(EXPORT 여부는 헤더 선언에 따름) */
		sector_t length)
{ /* [한국어] add_partition()이 반환할 새 block_device(성공 시에만 유효, 반환값 자체는 errno로 변환됨) */
	struct block_device *part; /* [한국어] 최종 반환할 errno(또는 0) */
	int ret;
 /* [한국어] part_tbl 갱신을 직렬화하기 위해 open_mutex 획득(add_partition()의 사전 조건) */
	mutex_lock(&disk->open_mutex); /* [한국어] 디스크가 등록 해제(dead) 상태가 아닌지 확인 */
	if (!disk_live(disk)) { /* [한국어] dead 상태면 파티션을 추가할 수 없음 */
		ret = -ENXIO; /* [한국어] -ENXIO(장치 없음)로 설정 */
		goto out; /* [한국어] 락 해제 후 반환하는 공통 out 레이블로 점프 */
	} /* [한국어] if 블록 종료 */
 /* [한국어] 이 디스크가 애초에 파티션을 지원하지 않도록 플래그 설정되어 있는지 확인(GENHD_FL_NO_PART) */
	if (disk->flags & GENHD_FL_NO_PART) { /* [한국어] 파티션 미지원 디스크에는 추가 불가 */
		ret = -EINVAL; /* [한국어] -EINVAL(잘못된 인자)로 설정 */
		goto out; /* [한국어] out으로 점프 */
	} /* [한국어] if 블록 종료 */
 /* [한국어] 요청한 [start, start+length) 구간이 기존 파티션과 겹치는지 확인(자기 자신을 제외할 필요 없으므로 skip_partno=-1) */
	if (partition_overlaps(disk, start, length, -1)) { /* [한국어] 겹치면 추가 거부 */
		ret = -EBUSY; /* [한국어] -EBUSY(자원 사용 중)로 설정 */
		goto out; /* [한국어] out으로 점프 */
	} /* [한국어] if 블록 종료 */

	part = add_partition(disk, partno, start, length, /* [한국어] 모든 사전 조건을 통과했으므로 실제 block_device 생성을 시도(플래그 없음, 메타정보 없음) */
			ADDPART_FLAG_NONE, NULL); /* [한국어] ADDPART_FLAG_NONE: RAID/WHOLEDISK/READONLY 등 특수 플래그 없이 일반 파티션으로 추가 */
	ret = PTR_ERR_OR_ZERO(part); /* [한국어] add_partition()의 반환값(포인터 또는 ERR_PTR)을 성공 시 0, 실패 시 errno로 변환 */
out: /* [한국어] 공통 출구 레이블 - 성공/실패 모든 경로가 여기로 모여 락을 반드시 해제 */
	mutex_unlock(&disk->open_mutex); /* [한국어] open_mutex 해제 */
	return ret; /* [한국어] 최종 결과(0 또는 errno)를 호출자에게 반환 */
} /* [한국어] bdev_add_partition() 함수 종료 */

/*
 * [한국어]
 * bdev_del_partition() - 사용자(ioctl) 요청에 따라 파티션을 삭제한다.
 *
 * @disk: 대상 gendisk.
 * @partno: 삭제할 파티션 번호.
 * @return: 성공 시 0, 실패 시 -ENXIO(존재하지 않음) 또는
 *          -EBUSY(열려 있는 opener가 있어 삭제 불가).
 *
 * ioctl(BLKPG, BLKPG_DEL_PARTITION) 처리 경로의 최상위 진입점이다.
 * part_tbl에서 해당 partno를 조회하고, bd_openers(현재 열려 있는
 * 파일/디바이스 수)가 0보다 크면 사용 중인 파티션을 강제로 지우지
 * 않고 -EBUSY를 반환한다(파일시스템 마운트 중이거나 열려 있는 fd가
 * 있는 파티션을 삭제하면 use-after-free 위험이 있으므로). bd_openers가
 * 0임을 확인했다면(그리고 open_mutex를 들고 있어 그 사이 새로운
 * open이 불가능하므로) holder 콜백을 부를 필요 없이 바로
 * bdev_unhash()+invalidate_bdev()+drop_partition()으로 안전하게
 * 제거할 수 있다(원본 주석이 이 불변조건을 설명).
 * 실행 컨텍스트: ioctl(2)을 처리하는 프로세스 컨텍스트, open_mutex
 * 보유 하에 슬립 가능.
 * 호출자(caller): block/ioctl.c의 BLKPG_DEL_PARTITION 처리 경로(추정).
 * 피호출자(callee): mutex_lock/unlock(), xa_load(), atomic_read(),
 * bdev_unhash(), invalidate_bdev(), drop_partition().
 * 에러 경로: 존재하지 않으면 -ENXIO, 열려 있으면 -EBUSY, 각각
 * out_unlock으로 점프해 락을 해제하고 반환한다.
 *
 * 호출 체인:
 *   ioctl(BLKPG_DEL_PARTITION) → [bdev_del_partition] → drop_partition()
 */
int bdev_del_partition(struct gendisk *disk, int partno) /* [한국어] 사용자 요청 파티션 삭제 함수 시작 */
{
	struct block_device *part = NULL; /* [한국어] 조회된 파티션 포인터 - 못 찾으면 NULL 유지 */
	int ret = -ENXIO; /* [한국어] 기본 실패 코드를 -ENXIO(해당 파티션 없음)로 미리 설정 */

	mutex_lock(&disk->open_mutex); /* [한국어] part_tbl 조회/갱신을 직렬화하기 위해 open_mutex 획득 */
	part = xa_load(&disk->part_tbl, partno); /* [한국어] partno에 해당하는 block_device를 part_tbl에서 조회 */
	if (!part) /* [한국어] 존재하지 않으면(NULL) */
		goto out_unlock; /* [한국어] 이미 설정된 -ENXIO를 그대로 반환하기 위해 out_unlock으로 점프 */

	ret = -EBUSY; /* [한국어] 다음 실패 코드를 -EBUSY(사용 중)로 갱신 */
	if (atomic_read(&part->bd_openers)) /* [한국어] 현재 이 파티션을 열고 있는 opener 수(원자적 카운터)를 확인 */
		goto out_unlock; /* [한국어] 하나라도 열려 있으면 삭제 불가 - out_unlock으로 점프 */

	/*
	 * We verified that @part->bd_openers is zero above and so
	 * @part->bd_holder{_ops} can't be set. And since we hold
	 * @disk->open_mutex the device can't be claimed by anyone.
	 *
	 * So no need to call @part->bd_holder_ops->mark_dead() here.
	 * Just delete the partition and invalidate it.
	 */

	bdev_unhash(part); /* [한국어] inode lookup 해시에서 제거 - 이후 새로운 open(2) 시도는 이 파티션을 찾지 못함(이미 열려 있던 fd는 영향 없으나 opener=0을 확인했으므로 존재하지 않음) */
	invalidate_bdev(part); /* [한국어] 이 파티션의 page cache를 무효화(더티 페이지 없음이 보장된 상태 - opener 0이므로) */
	drop_partition(part); /* [한국어] part_tbl/sysfs에서 실제로 파티션을 제거 */
	ret = 0; /* [한국어] 성공(0)으로 결과 갱신 */
out_unlock: /* [한국어] 공통 출구 레이블 - 존재하지 않음/사용 중/성공 모든 경로가 도달 */
	mutex_unlock(&disk->open_mutex); /* [한국어] open_mutex 해제 */
	return ret; /* [한국어] 결과(0 또는 errno) 반환 */
} /* [한국어] bdev_del_partition() 함수 종료 */

/*
 * [한국어]
 * bdev_resize_partition() - 기존 파티션의 길이(크기)를 조정한다.
 *
 * @disk: 대상 gendisk.
 * @partno: 크기를 조정할 파티션 번호.
 * @start: 기존 파티션과 반드시 일치해야 하는 시작 섹터(검증용 - 이
 *         함수는 시작 위치 자체는 바꾸지 않는다).
 * @length: 새로 설정할 길이(섹터 수).
 * @return: 성공 시 0, 실패 시 -ENXIO(없음)/-EINVAL(start 불일치)/
 *          -EBUSY(다른 파티션과 겹침).
 *
 * ioctl(BLKPG, BLKPG_RESIZE_PARTITION) 처리 경로의 최상위 진입점이다.
 * 시작 섹터는 파티션 정체성의 일부로 간주되어 변경을 허용하지 않고
 * (호출자가 잘못된 start를 넘기면 다른 파티션을 실수로 건드리는 것을
 * 막기 위한 방어적 검증), 새 길이로 인해 다른 파티션과 겹치게 되지
 * 않는지 partition_overlaps()로 재검사한 뒤에만
 * bdev_set_nr_sectors()로 실제 크기를 갱신한다.
 * 실행 컨텍스트: ioctl(2)을 처리하는 프로세스 컨텍스트, open_mutex
 * 보유 하에 슬립 가능.
 * 호출자(caller): block/ioctl.c의 BLKPG_RESIZE_PARTITION 처리 경로(추정).
 * 피호출자(callee): mutex_lock/unlock(), xa_load(), partition_overlaps(),
 * bdev_set_nr_sectors().
 * 에러 경로: 각 실패 조건마다 ret을 설정하고 out_unlock으로 점프.
 *
 * 호출 체인:
 *   ioctl(BLKPG_RESIZE_PARTITION) → [bdev_resize_partition]
 *     → bdev_set_nr_sectors()
 */
int bdev_resize_partition(struct gendisk *disk, int partno, sector_t start, /* [한국어] 파티션 크기 조정 함수 시작 */
		sector_t length)
{ /* [한국어] 조회된 파티션 포인터 */
	struct block_device *part = NULL; /* [한국어] 기본 실패 코드를 -ENXIO로 설정 */
	int ret = -ENXIO;
 /* [한국어] open_mutex 획득 */
	mutex_lock(&disk->open_mutex); /* [한국어] partno로 part_tbl에서 조회 */
	part = xa_load(&disk->part_tbl, partno); /* [한국어] 존재하지 않으면 */
	if (!part) /* [한국어] out_unlock으로 점프(이미 -ENXIO 설정됨) */
		goto out_unlock;
 /* [한국어] 다음 실패 코드를 -EINVAL로 갱신 */
	ret = -EINVAL; /* [한국어] 호출자가 넘긴 start가 실제 파티션의 시작 섹터와 다르면(정체성 불일치) */
	if (start != part->bd_start_sect) /* [한국어] 잘못된 요청으로 거부 - out_unlock으로 점프 */
		goto out_unlock;
 /* [한국어] 다음 실패 코드를 -EBUSY로 갱신 */
	ret = -EBUSY; /* [한국어] 새 길이로 다른 파티션과 겹치는지 확인(자기 자신은 skip_partno=partno로 제외) */
	if (partition_overlaps(disk, start, length, partno)) /* [한국어] 겹치면 거부 */
		goto out_unlock; /* [한국어] out_unlock으로 점프 */
 /* [한국어] bdev의 크기(섹터 수)를 새 길이로 갱신(block/bdev.c) - i_size 등도 함께 갱신됨 */
	bdev_set_nr_sectors(part, length);
 /* [한국어] 성공(0)으로 결과 갱신 */
	ret = 0; /* [한국어] 공통 출구 레이블 */
out_unlock: /* [한국어] open_mutex 해제 */
	mutex_unlock(&disk->open_mutex); /* [한국어] 결과 반환 */
	return ret; /* [한국어] bdev_resize_partition() 함수 종료 */
}

/*
 * [한국어]
 * disk_unlock_native_capacity() - 드라이버가 숨겨온 디스크의 실제(native)
 * 용량을 노출하도록 요청한다.
 *
 * @disk: 대상 gendisk.
 * @return: unlock을 실제로 수행했으면 true(호출자는 재스캔을 시도해야
 *          함), 이미 시도했었거나 드라이버가 이 기능을 지원하지 않으면
 *          false(더 이상 손쓸 방법이 없다는 뜻).
 *
 * 일부 레거시 드라이버(예: 호스트 보호 영역 HPA를 가진 ATA 디스크)는
 * BIOS/펌웨어 호환을 위해 기본적으로 실제 용량보다 작은 용량을
 * 보고하다가, 파티션 테이블이 그 축소된 용량을 넘어서는 것을 발견하면
 * (from/size가 get_capacity()를 초과) 그제서야 "native capacity"(HPA
 * 해제 등)를 노출하도록 요청받는다. GD_NATIVE_CAPACITY 비트를
 * test_and_set_bit으로 원자적으로 검사/설정해 이 unlock 시도가
 * 디스크당 정확히 한 번만 일어나게 한다(그렇지 않으면 파티션 테이블이
 * 여전히 EOD를 넘는 경우 무한 재시도가 될 수 있음).
 * 실행 컨텍스트: blk_add_partition()/blk_add_partitions() 호출 경로와
 * 동일한 프로세스 컨텍스트. fops->unlock_native_capacity 콜백은 드라이버
 * 구현에 따라 슬립할 수 있다.
 * 호출자(caller): blk_add_partition()(파티션이 EOD를 넘을 때),
 * blk_add_partitions()(파티션 테이블 자체가 EOD를 넘어 읽혔을 때).
 * 피호출자(callee): test_and_set_bit(), disk->fops->unlock_native_capacity().
 * 에러 경로: 콜백이 없거나 이미 시도된 경우 false를 반환해 호출자가
 * "더 시도할 것이 없다"는 것으로 처리하게 한다(이 경우 파티션이나
 * 테이블 읽기는 잘려나간(truncated) 상태로 남는다).
 *
 * 호출 체인:
 *   blk_add_partition()/blk_add_partitions() → [disk_unlock_native_capacity]
 *     → disk->fops->unlock_native_capacity()
 */
static bool disk_unlock_native_capacity(struct gendisk *disk) /* [한국어] native 용량 unlock 요청 함수 시작 */
{
	if (!disk->fops->unlock_native_capacity || /* [한국어] 드라이버가 unlock 콜백을 아예 제공하지 않거나 */
	    test_and_set_bit(GD_NATIVE_CAPACITY, &disk->state)) { /* [한국어] GD_NATIVE_CAPACITY 비트를 원자적으로 확인 후 설정 - 이미 설정되어 있었다면(이전에 unlock을 이미 시도했다면) true를 반환해 이 if 조건에 걸림 */
		printk(KERN_CONT "truncated\n"); /* [한국어] 「잘려나간(truncated) 채로 남긴다」는 로그를 이어붙임(KERN_CONT - 앞선 printk의 연속 라인) */
		return false; /* [한국어] unlock을 시도할 수 없거나 이미 시도했으므로 false 반환 */
	} /* [한국어] if 블록 종료 */

	printk(KERN_CONT "enabling native capacity\n"); /* [한국어] native 용량을 활성화한다는 로그를 이어붙임 */
	disk->fops->unlock_native_capacity(disk); /* [한국어] 드라이버 고유의 unlock 콜백을 호출 - 예: HPA 해제 후 실제 섹터 수로 get_capacity()가 갱신되도록 함(드라이버 구현에 위임) */
	return true; /* [한국어] unlock을 이번에 실제로 수행했으므로 true 반환 - 호출자는 재스캔(rescan)을 시도해야 함 */
} /* [한국어] disk_unlock_native_capacity() 함수 종료 */

/*
 * [한국어]
 * blk_add_partition() - 파싱된 파티션 후보 하나를 검증해 실제로 등록한다.
 *
 * @disk: 대상 gendisk.
 * @state: check_partition()이 채워 넣은 parsed_partitions(parts[p]에
 *         from/size/flags/info가 들어 있음).
 * @p: 처리할 파티션 인덱스(1..state->limit-1).
 * @return: 정상 진행(등록 성공/스킵 모두 포함) 시 true, native 용량
 *          unlock을 실제로 수행해 전체 스캔을 처음부터 다시 해야 하는
 *          경우에만 false.
 *
 * parts[p]가 실제 유효한 파티션(size != 0)인지 먼저 확인하고, 시작
 * 위치나 끝 위치가 디스크의 현재 알려진 용량(get_capacity)을 벗어나는
 * 손상된/legacy 파티션 테이블을 방어적으로 처리한다: 시작이 이미
 * EOD 너머면 native 용량 unlock을 시도해보고, 성공했다면(false 반환 -
 * 재스캔 필요) 그대로 포기, 실패했다면(unlock 불가) 이 파티션만
 * 조용히 건너뛴다(true 반환, 나머지는 계속 처리). 크기가 EOD를 넘는
 * 경우도 유사하되, unlock이 안 되면 파티션을 완전히 버리지 않고
 * "카메라 펌웨어 등이 만든 깨진 테이블"을 감안해 EOD까지만 크기를
 * 잘라(clamp) 유효한 block_device를 만든다. 마지막으로
 * add_partition()을 호출하고, ADDPART_FLAG_RAID 플래그가 있으며
 * CONFIG_BLK_DEV_MD가 내장(built-in)되어 있으면 md_autodetect_dev()로
 * 이 파티션이 RAID superblock을 갖고 있는지 자동 탐지를 요청한다.
 * 실행 컨텍스트: blk_add_partitions()의 for 루프 안, 동일한 프로세스
 * 컨텍스트에서 순차 호출.
 * 호출자(caller): blk_add_partitions().
 * 피호출자(callee): get_capacity(), disk_unlock_native_capacity(),
 * add_partition(), md_autodetect_dev().
 * 에러 경로: add_partition()이 -ENXIO 외의 에러로 실패하면 로그만
 * 남기고 계속 진행한다(하나의 파티션 등록 실패가 전체 스캔을 막지
 * 않도록) - 오직 native 용량 unlock 성공(재스캔 필요)만 blk_add_partitions()
 * 의 전체 루프를 중단시킨다.
 *
 * 호출 체인:
 *   blk_add_partitions() → [blk_add_partition] → add_partition()
 */
static bool blk_add_partition(struct gendisk *disk, /* [한국어] 파티션 후보 1개 검증/등록 함수 시작 */
		struct parsed_partitions *state, int p) /* [한국어] p: state->parts[] 배열 인덱스(1부터, 0은 whole-disk 몫이라 건너뜀 - 호출자 루프가 1부터 시작) */
{
	sector_t size = state->parts[p].size; /* [한국어] 파싱된 파티션의 크기(섹터 수)를 지역 변수로 캐시 */
	sector_t from = state->parts[p].from; /* [한국어] 파싱된 파티션의 시작 섹터를 지역 변수로 캐시 */
	struct block_device *part; /* [한국어] add_partition() 성공 시 결과를 받을 포인터(현재는 성공 실패 여부만 확인, 재사용은 안 함) */

	if (!size) /* [한국어] 크기가 0이면 이 슬롯은 애초에 파티션이 없다는 뜻(check_part 프로버가 채우지 않은 빈 엔트리) */
		return true; /* [한국어] 아무 것도 하지 않고 「정상 진행」 의미로 true 반환 - 다음 p로 계속 */

	if (from >= get_capacity(disk)) { /* [한국어] 파티션의 시작 자체가 이미 디스크가 보고하는 용량(EOD)을 넘어선 경우 - 손상된 테이블이거나 native 용량이 숨겨진 경우 */
		printk(KERN_WARNING /* [한국어] 「p%d start ... is beyond EOD」 형태의 경고 로그 출력 시작 */
		       "%s: p%d start %llu is beyond EOD, ",
		       disk->disk_name, p, (unsigned long long) from);
		if (disk_unlock_native_capacity(disk)) /* [한국어] native 용량을 풀 수 있는지 시도 */
			return false; /* [한국어] 실제로 unlock을 수행했다면(재시도 시 용량이 커져 있을 수 있음) 전체 재스캔이 필요하므로 false 반환 */
		return true; /* [한국어] unlock도 못 했다면(더 손쓸 방법이 없음) 이 파티션만 포기하고 나머지는 계속 처리(true) */
	} /* [한국어] if 블록 종료 */

	if (from + size > get_capacity(disk)) { /* [한국어] 시작은 EOD 안이지만 시작+크기가 EOD를 넘어서는 경우(파티션이 디스크 끝을 살짝 넘어감) */
		printk(KERN_WARNING /* [한국어] 「p%d size ... extends beyond EOD」 경고 로그 출력 시작 */
		       "%s: p%d size %llu extends beyond EOD, ",
		       disk->disk_name, p, (unsigned long long) size);

		if (disk_unlock_native_capacity(disk)) /* [한국어] native 용량을 풀 수 있는지 먼저 시도 - 실제 용량이 더 크다면 이 파티션도 정상 범위 안에 들어올 수 있음 */
			return false; /* [한국어] unlock을 실제로 수행했다면 재스캔 필요(false) */

		/*
		 * We can not ignore partitions of broken tables created by for
		 * example camera firmware, but we limit them to the end of the
		 * disk to avoid creating invalid block devices.
		 */
		size = get_capacity(disk) - from; /* [한국어] unlock도 안 되고 정말 깨진 테이블이라면, 파티션을 완전히 버리는 대신 EOD까지만 크기를 잘라 유효한 block_device를 만든다(카메라 펌웨어 등이 만드는 오프바이원 테이블 대응) */
	} /* [한국어] if 블록 종료 */

	part = add_partition(disk, p, from, size, state->parts[p].flags, /* [한국어] 검증된(또는 잘라낸) from/size로 실제 block_device를 생성 - 파싱된 flags/info도 함께 전달 */
			     &state->parts[p].info); /* [한국어] state->parts[p].info의 주소를 넘김 - add_partition()이 kmemdup으로 내용을 복제하므로 이 임시 배열이 나중에 재사용/해제되어도 안전 */
	if (IS_ERR(part)) { /* [한국어] add_partition()이 실패(ERR_PTR)했는지 확인 */
		if (PTR_ERR(part) != -ENXIO) { /* [한국어] -ENXIO(예: zoned 디바이스라 파티션 미지원)는 이미 add_partition() 내부에서 경고를 남겼으므로 중복 로그를 피하기 위해 별도 처리 */
			printk(KERN_ERR " %s: p%d could not be added: %pe\n", /* [한국어] 그 외의 실패는 「could not be added」 에러 로그로 상세 원인을 남김(%pe로 에러 포인터를 사람이 읽을 수 있는 문자열로 출력) */
			       disk->disk_name, p, part); /* [한국어] printk 인자 계속 */
		} /* [한국어] if 블록 종료 */
		return true; /* [한국어] 이 파티션 하나의 등록 실패는 전체 스캔을 막지 않음 - true 반환으로 나머지 파티션 계속 처리 */
	} /* [한국어] if(IS_ERR(part)) 블록 종료 */

	if (IS_BUILTIN(CONFIG_BLK_DEV_MD) && /* [한국어] CONFIG_BLK_DEV_MD가 모듈이 아니라 커널에 내장(built-in)되어 있고(모듈이면 이 시점에 아직 로드되지 않았을 수 있어 자동 탐지 불가) */
	    (state->parts[p].flags & ADDPART_FLAG_RAID)) /* [한국어] 이 파티션이 ADDPART_FLAG_RAID로 표시되어 있다면(파티션 파서가 RAID superblock 시그니처를 인식한 경우) */
		md_autodetect_dev(part->bd_dev); /* [한국어] md 드라이버에 이 파티션의 dev_t를 넘겨 RAID 자동 조립(assemble) 후보로 등록 요청 */

	return true; /* [한국어] 정상적으로 처리 완료(등록 성공이든 개별 실패든) - true 반환 */
} /* [한국어] blk_add_partition() 함수 종료 */

/*
 * [한국어]
 * blk_add_partitions() - 디스크의 파티션 테이블을 읽어 모든 파티션을
 * 등록한다.
 *
 * @disk: 대상 gendisk.
 * @return: 성공적으로 스캔을 마쳤으면(파티션이 없거나 파티션 스캔이
 *          꺼져 있는 경우 포함) 0. 재스캔이 필요하면(native 용량
 *          unlock 성공) -EAGAIN(호출자 bdev_disk_changed()가 이를
 *          받아 다시 시도). 그 외 I/O 에러는 -EIO.
 *
 * check_partition()으로 파티션 테이블을 파싱한 뒤, 배열의 각 항목에
 * blk_add_partition()을 호출해 실제 block_device들을 만드는 조율자
 * (orchestrator) 함수다. 동작 순서: (1) 애초에 이 디스크가 파티션
 * 스캔 대상이 아니면(disk_has_partscan 거짓) 조용히 0 반환, (2)
 * check_partition() 호출 - NULL(메모리 부족)이면 0 반환(에러로 취급
 * 안 함), IS_ERR이면 -ENOSPC(EOD 초과)인 경우에 한해 native 용량
 * unlock을 시도하고 성공하면 -EAGAIN, 그 외에는 -EIO, (3) 여기까지
 * 왔으면 파티션 테이블 파싱에 성공한 것 - host-managed zoned
 * 디바이스는 파티션 자체를 무시(파티션 지원 안 함이 확정된 디바이스),
 * (4) 파싱 도중 일부라도 EOD 너머를 읽으려 했다면(테이블은 인식됐지만
 * 일부 파티션이 잘렸을 수 있음) native 용량 unlock을 한 번 더 시도,
 * (5) 미디어/파티션 변경을 사용자 공간에 KOBJ_CHANGE로 통지, (6)
 * parts[1..limit)을 순회하며 blk_add_partition()을 호출 - 이 함수가
 * false를 반환하면(재스캔 필요) 즉시 중단, (7) 정상 종료 시 state를
 * 해제하고 0 반환.
 * 실행 컨텍스트: bdev_disk_changed()와 동일한 프로세스 컨텍스트,
 * open_mutex 보유 상태에서 호출됨(간접적으로 - 이 함수 자체는 락을
 * 잡지 않지만 호출자가 이미 잡고 있음).
 * 호출자(caller): bdev_disk_changed().
 * 피호출자(callee): disk_has_partscan(), check_partition(),
 * disk_unlock_native_capacity(), bdev_is_zoned(), kobject_uevent(),
 * blk_add_partition(), free_partitions().
 * 에러 경로: out_free_state 레이블로 모여 항상 free_partitions()를
 * 호출한 뒤 ret을 반환 - state 메모리 누수를 막는 단일 정리 지점.
 *
 * 호출 체인:
 *   bdev_disk_changed() → [blk_add_partitions] → check_partition() →
 *     blk_add_partition() → add_partition()
 */
static int blk_add_partitions(struct gendisk *disk) /* [한국어] 디스크 전체 파티션 스캔/등록 조율 함수 시작 */
{
	struct parsed_partitions *state; /* [한국어] check_partition()이 반환할 파싱 결과 */
	int ret = -EAGAIN, p; /* [한국어] 기본 반환값을 -EAGAIN으로 초기화(재스캔 루프의 안전한 기본값 - 실제로는 아래에서 모든 경로가 명시적으로 덮어씀) */

	if (!disk_has_partscan(disk)) /* [한국어] 이 디스크가 애초에 파티션 스캔 대상이 아니면(예: GENHD_FL_NO_PART 등) */
		return 0; /* [한국어] 스캔할 필요 없이 성공(0)으로 간주하고 종료 */

	state = check_partition(disk); /* [한국어] check_part[] 배열의 각 포맷 프로버를 순회해 파티션 테이블을 파싱 시도 */
	if (!state) /* [한국어] 메모리 부족 등으로 아예 시도조차 못한 경우(NULL, ERR_PTR이 아님에 유의) */
		return 0; /* [한국어] 에러로 취급하지 않고 0(성공) 반환 - 파티션 없이 진행 */
	if (IS_ERR(state)) { /* [한국어] 인식 가능한 포맷이 없었거나 I/O 에러가 있었던 경우(ERR_PTR) */
		/*
		 * I/O error reading the partition table.  If we tried to read
		 * beyond EOD, retry after unlocking the native capacity.
		 */
		if (PTR_ERR(state) == -ENOSPC) { /* [한국어] 특히 EOD를 넘어 읽으려다 실패한 경우(-ENOSPC)라면 native 용량이 숨겨져 있을 가능성이 있음 */
			printk(KERN_WARNING "%s: partition table beyond EOD, ", /* [한국어] 「partition table beyond EOD」 경고 로그 출력 */
			       disk->disk_name);
			if (disk_unlock_native_capacity(disk)) /* [한국어] native 용량 unlock을 시도해 실제로 풀렸다면 */
				return -EAGAIN; /* [한국어] 더 큰 용량으로 처음부터 재스캔해야 하므로 -EAGAIN 반환(bdev_disk_changed의 rescan 레이블로 되돌아감) */
		} /* [한국어] if 블록 종료 */
		return -EIO; /* [한국어] unlock도 안 되거나 -ENOSPC가 아닌 다른 에러였다면 일반 I/O 에러로 취급 */
	} /* [한국어] if(IS_ERR(state)) 블록 종료 */

	/*
	 * Partitions are not supported on host managed zoned block devices.
	 */
	if (bdev_is_zoned(disk->part0)) { /* [한국어] host-managed zoned 블록 디바이스는 파티션을 아예 지원하지 않으므로(add_partition()에서도 거부됨) 여기서도 미리 걸러냄 */
		pr_warn("%s: ignoring partition table on host managed zoned block device\n", /* [한국어] 무시한다는 경고 로그 출력 */
			disk->disk_name);
		ret = 0; /* [한국어] 파티션 없이 성공으로 취급 */
		goto out_free_state; /* [한국어] 정리 후 반환하는 out_free_state로 점프 */
	} /* [한국어] if 블록 종료 */

	/*
	 * If we read beyond EOD, try unlocking native capacity even if the
	 * partition table was successfully read as we could be missing some
	 * partitions.
	 */
	if (state->access_beyond_eod) { /* [한국어] 파티션 테이블 자체는 성공적으로 읽혔지만, 그 파싱 과정에서 일부 항목이 EOD 너머를 참조했다면(테이블은 있지만 일부 파티션이 안 보일 수 있음) */
		printk(KERN_WARNING /* [한국어] 「partition table partially beyond EOD」 경고 로그 출력 */
		       "%s: partition table partially beyond EOD, ",
		       disk->disk_name);
		if (disk_unlock_native_capacity(disk)) /* [한국어] native 용량 unlock을 시도해 실제로 풀렸다면 */
			goto out_free_state; /* [한국어] 더 큰 용량으로 재스캔해야 하므로 out_free_state로 점프(ret은 여전히 초기값 -EAGAIN이므로 그대로 반환됨) */
	} /* [한국어] if 블록 종료 */

	/* tell userspace that the media / partition table may have changed */
	kobject_uevent(&disk_to_dev(disk)->kobj, KOBJ_CHANGE); /* [한국어] 파티션 테이블/미디어가 바뀌었을 수 있음을 사용자 공간(udev)에 KOBJ_CHANGE uevent로 통지 */

	for (p = 1; p < state->limit; p++) /* [한국어] parts[1]부터 parts[limit-1]까지(0번은 whole-disk 몫이라 제외) 순회하며 각 파티션 후보를 처리 */
		if (!blk_add_partition(disk, state, p)) /* [한국어] blk_add_partition()이 false를 반환하면(native 용량 unlock 성공 - 재스캔 필요) */
			goto out_free_state; /* [한국어] 더 이상 순회하지 않고 정리 후 반환(ret은 여전히 -EAGAIN 초기값) */

	ret = 0; /* [한국어] 모든 파티션을 순회했고 재스캔이 필요 없었으므로 최종 성공(0)으로 확정 */
out_free_state: /* [한국어] 공통 정리 레이블 - 성공/실패 모든 경로가 여기로 모임 */
	free_partitions(state); /* [한국어] check_partition()이 할당했던 parsed_partitions 메모리를 반드시 해제(메모리 누수 방지) */
	return ret; /* [한국어] ret(0, -EAGAIN, -EIO 중 하나)을 호출자에게 반환 */
} /* [한국어] blk_add_partitions() 함수 종료 */

/*
 * [한국어]
 * bdev_disk_changed() - 미디어/디스크 상태 변화에 대응해 파티션 전체를
 * 재스캔한다.
 *
 * @disk: 대상 gendisk.
 * @invalidate: true면 미디어 자체가 바뀌었거나 제거되었다고 간주해
 *              기존 캐시/용량을 무효화한다(예: CD-ROM 미디어 교체,
 *              BLKRRPART ioctl). false면 단순히 파티션 레이아웃만
 *              다시 읽는 재스캔(용량은 유지).
 * @return: 성공 시 0, 그 외 blk_add_partitions()가 반환하는 -EIO 등,
 *          또는 열려 있는 파티션이 있어 재스캔이 불가능한 -EBUSY.
 *
 * 이 함수는 디스크의 모든 상태를 "백지"로 되돌린 뒤 처음부터 다시
 * 파티션을 인식시키는 전체 재스캔 루틴이다. 동작 순서: (1) 파티션이
 * 하나라도 열려 있으면(open_partitions > 0) 재스캔 자체가 불가능하므로
 * -EBUSY, (2) whole-disk(part0)의 더티 데이터를 동기화하고 페이지
 * 캐시를 무효화, (3) part_tbl에 남아있는 기존 파티션들을 모두
 * bdev_unhash+invalidate_bdev+drop_partition으로 제거(이 시점에는 이미
 * 열려 있는 opener가 없다고 가정 - WARN_ON_ONCE로 방어적 검증), (4)
 * GD_NEED_PART_SCAN 플래그 해제(스캔이 진행 중임을 표시 해제), (5)
 * invalidate가 true고 이 디스크가 "파티션 지원 O + 이동식(removable)"
 * 조합이 아니면 용량을 0으로 잠시 리셋(레거시 ide-cdrom과의 호환을
 * 위한 특수 케이스는 원본 주석 참고), (6) 용량이 남아 있다면
 * blk_add_partitions()로 실제 재스캔을 수행 - -EAGAIN이면 rescan
 * 레이블로 돌아가 처음부터 다시 시도(native 용량이 새로 풀렸을 때),
 * (7) 용량이 0이 되었고 invalidate라면 미디어 제거를 KOBJ_CHANGE로
 * 통지.
 * 실행 컨텍스트: 호출자가 disk->open_mutex를 이미 보유한 상태에서
 * 호출되어야 한다(lockdep_assert_held). 슬립 가능한 프로세스 컨텍스트
 * (sync_blockdev 등 블로킹 호출 포함).
 * 호출자(caller): 드라이버의 open/revalidate 경로, BLKRRPART ioctl
 * 처리 경로 등(이 파일 밖, 추정) - loop/dasd 드라이버는 특히 이 심볼을
 * 역사적 이유로 직접 export 받아 사용한다(아래 원본 주석 참고).
 * 피호출자(callee): disk_live(), sync_blockdev(), invalidate_bdev(),
 * xa_for_each_start(), bdev_unhash(), drop_partition(),
 * clear_bit()/set_capacity(), get_capacity(), blk_add_partitions(),
 * kobject_uevent().
 * 에러 경로: open_partitions가 남아있으면 -EBUSY로 조기 반환,
 * blk_add_partitions()가 -EIO 등을 반환하면 그대로 호출자에게 전파.
 *
 * 호출 체인:
 *   (드라이버 open/revalidate 경로, ioctl BLKRRPART 등) →
 *     [bdev_disk_changed] → blk_add_partitions() → check_partition()
 */
int bdev_disk_changed(struct gendisk *disk, bool invalidate) /* [한국어] 전체 재스캔 함수 시작 - EXPORT_SYMBOL_GPL로 다른 모듈(loop/dasd)에도 노출됨(아래 참고) */
{
	struct block_device *part; /* [한국어] xa_for_each_start 순회용 파티션 포인터 */
	unsigned long idx; /* [한국어] xa_for_each_start 순회용 인덱스 */
	int ret = 0; /* [한국어] 최종 반환값 - 기본값 0(성공) */

	lockdep_assert_held(&disk->open_mutex); /* [한국어] 호출자가 open_mutex를 들고 있는지 lockdep으로 검증 - part_tbl/용량 갱신은 이 락 아래에서만 안전 */

	if (!disk_live(disk)) /* [한국어] 디스크가 이미 dead 상태(등록 해제됨)라면 */
		return -ENXIO; /* [한국어] 재스캔할 대상 자체가 없으므로 -ENXIO 즉시 반환 */

rescan: /* [한국어] rescan: native 용량 unlock으로 인해 처음부터 다시 시도해야 할 때 goto로 돌아오는 레이블 */
	if (disk->open_partitions) /* [한국어] 현재 열려 있는(open 상태인) 파티션이 하나라도 있으면 */
		return -EBUSY; /* [한국어] 재스캔 도중 파티션을 지웠다가 다시 만들면 열려 있는 fd와 불일치가 생기므로 -EBUSY로 거부 */
	sync_blockdev(disk->part0); /* [한국어] whole-disk(part0)에 대해 아직 디스크에 쓰이지 않은 더티 페이지를 동기화 */
	invalidate_bdev(disk->part0); /* [한국어] whole-disk의 page cache를 무효화 - 오래된 캐시 데이터로 새 파티션 테이블을 잘못 읽는 것을 방지 */

	xa_for_each_start(&disk->part_tbl, idx, part, 1) { /* [한국어] part_tbl에 현재 등록되어 있는 모든 파티션(인덱스 1부터)을 순회하며 전부 제거 준비 */
		/*
		 * Remove the block device from the inode hash, so that
		 * it cannot be looked up any more even when openers
		 * still hold references.
		 */
		bdev_unhash(part); /* [한국어] inode lookup 해시에서 제거 - 이후 이 파티션은 새로 open될 수 없음 */

		/*
		 * If @disk->open_partitions isn't elevated but there's
		 * still an active holder of that block device things
		 * are broken.
		 */
		WARN_ON_ONCE(atomic_read(&part->bd_openers)); /* [한국어] 원래는 opener가 없어야 정상 - 있다면 버그(위쪽에서 open_partitions 체크를 이미 통과했으므로 이 시점엔 0이어야 함) */
		invalidate_bdev(part); /* [한국어] 이 파티션의 page cache 무효화 */
		drop_partition(part); /* [한국어] part_tbl/sysfs에서 실제로 제거 */
	} /* [한국어] xa_for_each_start 루프 종료 - 모든 기존 파티션 제거 완료 */
	clear_bit(GD_NEED_PART_SCAN, &disk->state); /* [한국어] 「파티션 스캔이 필요함」 플래그를 해제 - 지금 이 함수가 그 스캔을 수행하고 있으므로 */

	/*
	 * Historically we only set the capacity to zero for devices that
	 * support partitions (independ of actually having partitions created).
	 * Doing that is rather inconsistent, but changing it broke legacy
	 * udisks polling for legacy ide-cdrom devices.  Use the crude check
	 * below to get the sane behavior for most device while not breaking
	 * userspace for this particular setup.
	 */
	if (invalidate) { /* [한국어] invalidate=true(미디어 자체가 바뀌었다고 간주하는 경우)라면 */
		if (!(disk->flags & GENHD_FL_NO_PART) || /* [한국어] 이 디스크가 「파티션 미지원」 플래그가 없거나(즉 파티션을 지원하거나) */
		    !(disk->flags & GENHD_FL_REMOVABLE)) /* [한국어] 「이동식(removable)」이 아니라면(레거시 ide-cdrom 호환을 위한 예외 - 이동식 CD-ROM은 용량을 0으로 리셋하지 않음, 위 원본 주석 참고) */
			set_capacity(disk, 0); /* [한국어] 용량을 0으로 리셋 - 아래에서 다시 채워지거나(파티션 있으면) 0인 채로 남아 미디어 없음을 표시 */
	} /* [한국어] if 블록 종료 */

	if (get_capacity(disk)) { /* [한국어] 디스크에 여전히(혹은 native unlock 등으로 새로) 용량이 있다면 */
		ret = blk_add_partitions(disk); /* [한국어] 실제 파티션 테이블을 읽고 block_device들을 등록 */
		if (ret == -EAGAIN) /* [한국어] native 용량이 새로 풀려 재시도가 필요하다면(-EAGAIN) */
			goto rescan; /* [한국어] rescan 레이블로 돌아가 위 (1)단계부터 전부 다시 수행 */
	} else if (invalidate) { /* [한국어] 용량이 0이 된 상태이고(파티션도 없고) invalidate 요청이었다면(즉 미디어가 실제로 없어졌거나 제거됨) */
		/*
		 * Tell userspace that the media / partition table may have
		 * changed.
		 */
		kobject_uevent(&disk_to_dev(disk)->kobj, KOBJ_CHANGE); /* [한국어] 주석 이어짐 */
	} /* [한국어] 미디어 제거/변경을 사용자 공간(udev)에 KOBJ_CHANGE로 통지 */

	return ret; /* [한국어] 최종 결과(보통 0, blk_add_partitions에서 온 -EIO 등)를 호출자에게 반환 */
} /* [한국어] bdev_disk_changed() 함수 종료 */
/*
 * Only exported for loop and dasd for historic reasons.  Don't use in new
 * code!
 */
EXPORT_SYMBOL_GPL(bdev_disk_changed); /* [한국어] loop/dasd 드라이버가 이 심볼을 직접 호출할 수 있도록 GPL 라이선스 모듈에 export - 신규 코드는 사용 금지(원본 주석) */

/*
 * [한국어]
 * read_part_sector() - 파티션 테이블이 위치한 섹터 하나를 페이지 캐시를
 * 통해 읽는다.
 *
 * @state: 스캔 대상 디스크(state->disk)와 access_beyond_eod 플래그를
 *         담고 있는 parsed_partitions.
 * @n: 읽을 섹터 번호(디스크 전체 기준 논리 섹터, 0-based).
 * @p: 결과로 얻은 folio 참조를 담을 출력 파라미터(Sector 타입 -
 *     호출자는 다 쓴 뒤 put_dev_sector() 등으로 이 참조를 반드시
 *     해제해야 페이지가 계속 붙들려 있지 않음 - 이 함수 밖의 관례).
 * @return: 성공 시 섹터 n의 시작 주소를 가리키는 커널 가상 주소,
 *          실패(EOD 초과 또는 폴리오 획득 실패) 시 NULL.
 *
 * 각 포맷별 파티션 프로버(msdos_partition, efi_partition 등)가 공통으로
 * 사용하는 저수준 섹터 읽기 헬퍼다. 직접 bio를 만들어 제출하는 대신
 * whole-disk(state->disk->part0)의 struct address_space(페이지 캐시
 * 매핑)를 통해 read_mapping_folio()를 호출하므로, 이미 캐시에 있는
 * 섹터는 I/O 없이 즉시 반환되고, 캐시에 없으면 그 안에서 필요한 블록
 * I/O(궁극적으로 드라이버의 submit_bio/queue_rq)가 유발된다. 요청한
 * 섹터 n이 디스크가 현재 보고하는 용량(get_capacity)을 벗어나면
 * access_beyond_eod 플래그를 설정해 상위(check_partition/
 * blk_add_partitions)가 이를 근거로 native 용량 unlock을 시도하게 한다.
 * 반환된 folio 내에서 실제 섹터 데이터가 시작하는 오프셋은
 * offset_in_folio(folio, n * SECTOR_SIZE)로 계산한다(SECTOR_SIZE=512
 * 바이트 논리 섹터를 가정).
 * 실행 컨텍스트: check_partition() 호출 경로와 동일한 프로세스
 * 컨텍스트(포맷 프로버 함수들이 이 경로에서 호출됨). read_mapping_folio가
 * 슬립 가능(디스크 I/O 대기)하므로 인터럽트 컨텍스트에서 호출 불가.
 * 호출자(caller): 각 포맷별 check_xxx()/xxx_partition() 프로버 함수들
 * (이 파일 밖, block/partitions/msdos.c 등).
 * 피호출자(callee): get_capacity(), read_mapping_folio(),
 * folio_address(), offset_in_folio().
 * 에러 경로: EOD 초과 또는 read_mapping_folio 실패(IS_ERR) 시 out
 * 레이블로 점프해 p->v를 NULL로 설정하고 NULL을 반환한다(호출자는
 * NULL 반환을 "이 섹터를 읽을 수 없음"으로 처리).
 *
 * 호출 체인:
 *   check_xxx()/xxx_partition() (각 포맷 프로버, 파일 밖) →
 *     [read_part_sector] → read_mapping_folio()
 */
void *read_part_sector(struct parsed_partitions *state, sector_t n, Sector *p) /* [한국어] 파티션 테이블 섹터 읽기 함수 시작 - 전역 심볼로 각 포맷 파서(파일 밖)에서 호출 */
{
	struct address_space *mapping = state->disk->part0->bd_mapping; /* [한국어] whole-disk(part0)의 page cache 매핑 - 파티션이 아직 등록되기 전이므로 항상 전체 디스크 기준으로 읽음 */
	struct folio *folio; /* [한국어] read_mapping_folio()가 반환할 페이지 캐시 폴리오 포인터 */

	if (n >= get_capacity(state->disk)) { /* [한국어] 요청한 섹터 n이 디스크가 보고하는 현재 용량(get_capacity, 섹터 단위)을 벗어나는지 확인 */
		state->access_beyond_eod = true; /* [한국어] EOD 초과 접근이었음을 state에 기록 - 상위 호출자가 native 용량 unlock을 시도하는 근거가 됨 */
		goto out; /* [한국어] out 레이블로 점프해 실패로 처리 */
	} /* [한국어] if 블록 종료 */

	folio = read_mapping_folio(mapping, n >> PAGE_SECTORS_SHIFT, NULL); /* [한국어] 섹터 번호 n을 페이지 인덱스로 변환(PAGE_SECTORS_SHIFT만큼 우측 시프트 - 한 페이지에 몇 개의 섹터가 들어가는지에 따른 비트 수)한 뒤 해당 폴리오를 페이지 캐시에서 찾거나 읽어옴 */
	if (IS_ERR(folio)) /* [한국어] 폴리오 획득이 실패했는지(IS_ERR) 확인 - 예: I/O 에러, 메모리 부족 */
		goto out; /* [한국어] 실패 시 out으로 점프 */

	p->v = folio; /* [한국어] 획득한 폴리오를 출력 파라미터에 저장 - 호출자가 나중에 참조 해제(put)할 때 사용 */
	return folio_address(folio) + offset_in_folio(folio, n * SECTOR_SIZE); /* [한국어] 폴리오의 커널 가상 주소에 섹터 n의 바이트 오프셋(offset_in_folio로 계산, n*SECTOR_SIZE)을 더해 실제 데이터 시작 주소를 반환 */
out: /* [한국어] out: 실패(EOD 초과 또는 폴리오 획득 실패) 시 공통으로 도달하는 레이블 */
	p->v = NULL; /* [한국어] 실패했으므로 출력 파라미터를 NULL로 명시(호출자가 유효하지 않은 폴리오를 참조하지 않도록) */
	return NULL; /* [한국어] NULL 반환 - 호출자(각 포맷 프로버)는 이 섹터를 읽을 수 없다고 판단 */
} /* [한국어] read_part_sector() 함수 종료 */
