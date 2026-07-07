// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ldm - Support for Windows Logical Disk Manager (Dynamic Disks)
 *
 * Copyright (C) 2001,2002 Richard Russon <ldm@flatcap.org>
 * Copyright (c) 2001-2012 Anton Altaparmakov
 * Copyright (C) 2001,2002 Jakob Kemi <jakob.kemi@telia.com>
 *
 * Documentation is available at http://www.linux-ntfs.org/doku.php?id=downloads 
 */
/*
 * [한국어 설명] Windows LDM(Logical Disk Manager, 동적 디스크) 데이터베이스 파서 (ldm.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block/partitions/ldm.h가 정의한 온디스크 자료구조를 이용해, 실제로
 * 디스크에서 PRIVHEAD -> TOCBLOCK -> VMDB -> VBLK 순서로 LDM(동적 디스크) 데이터베이스를
 * 읽어 들이고 검증한 뒤, 이 물리 디스크에 속한 파티션들을 리눅스 커널의
 * parsed_partitions 구조에 등록하는 상태 머신을 구현한다. Windows는 하나의 물리
 * 디스크만으로 표현할 수 없는 스팬/스트라이프/미러/RAID5 볼륨을 여러 디스크에
 * 걸쳐 구성하기 위해 MBR 대신 이 LDM 방식을 쓰며, 이 파일은 그 중 "지금 이
 * gendisk가 나타내는 물리 디스크"에 실제로 존재하는 파티션들만 골라내는 역할을
 * 한다(RAID/스팬 토폴로지 자체를 재구성하지는 않는다 - 아래 "주요 함수/구조체
 * 요약" 참고).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 진입점은 이 파일 맨 끝의 ldm_partition()이며, block/partitions/core.c의
 * check_partition()이 check_part[] 파티션 스킴 배열을 순회하다가 이 함수를
 * 호출한다(msdos_partition보다 반드시 먼저 오도록 배치돼 있는데, LDM 데이터베이스
 * 자체가 MBR 파티션 테이블 안에 타입 0x42(LDM_PARTITION)로 중첩돼 있어 일반 MBR
 * 파서가 먼저 가로채면 안 되기 때문이다). ldm_partition()은 크게 다음 단계로
 * 진행된다: (1) ldm_validate_partition_table()로 MBR 안에 0x42 파티션이 있는지
 * 약식 확인, (2) ldm_validate_privheads()로 PRIVHEAD 3중 사본을 읽고 비교,
 * (3) ldm_validate_tocblocks()/ldm_validate_vmdb()로 TOCBLOCK 4중 사본과 VMDB
 * 헤더를 읽고 검증, (4) ldm_get_vblks()로 VBLK 테이블 전체를 순회하며(조각난
 * 레코드는 ldm_frag_add()/ldm_frag_commit()으로 재조립) ldm_ldmdb_add()가 각
 * VBLK를 타입별 리스트에 분류, (5) ldm_create_data_partitions()가 v_disk에서
 * 현재 디스크를, v_part에서 그 디스크에 속한 파티션들을 찾아 put_partition()으로
 * 등록. 실행 컨텍스트는 블록 디바이스 초기화/재스캔 경로의 단일 프로세스
 * 컨텍스트이며, 인터럽트 컨텍스트에서는 호출되지 않는다. 동시성 문제는 사실상
 * 없다 - 하나의 parsed_partitions/ldmdb 인스턴스는 이 호출 스코프 안에서만
 * 존재하고 공유되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * "ldm.h"가 정의하는 모든 온디스크 상수(MAGIC_*, VBLK_*, OFF_*)와 in-memory
 * 구조체(struct privhead/tocblock/vmdb/vblk_시리즈/vblk/ldmdb/frag)에 의존하며,
 * "check.h"가 정의하는 파티션 프레임워크 공용 API(struct parsed_partitions,
 * typedef Sector, read_part_sector(), put_dev_sector(), put_partition())에도
 * 의존한다. read_part_sector()는 내부적으로 페이지 캐시(read_mapping_folio())를
 * 거쳐 하위 블록 드라이버(SCSI/ATA/NVMe/virtio-blk 등, submit_bio 경로)까지
 * 내려가는 유일한 디스크 접근 지점이며, 이 파일의 모든 파싱 함수는 이 함수가
 * 반환한 512바이트 섹터 버퍼를 대상으로만 동작한다. <linux/uuid.h>(uuid_parse/
 * uuid_equal/import_uuid), <linux/unaligned.h>(get_unaligned_beNN, ldm.h를 통해
 * 전이적으로 얻음), <linux/msdos_partition.h>(MBR 파싱)에도 의존한다. 데이터
 * 흐름은 "디스크 섹터(빅엔디안 원시 바이트) -> get_unaligned_beNN()으로 CPU
 * 정수 변환 -> struct privhead/tocblock/vmdb/vblk 필드 -> struct list_head로
 * 연결된 ldmdb 캐시 -> put_partition()이 등록하는 parsed_partitions.parts[]"
 * 순서로 흐른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - ldm_partition(): 유일한 외부 진입점(check.h에 프로토타입 선언). 전체 파싱
 *   상태 머신을 조율하고 성공/실패/비동적디스크를 1/-1/0으로 반환한다.
 * - ldm_validate_privheads()/ldm_validate_tocblocks()/ldm_validate_vmdb():
 *   각각 PRIVHEAD 3중 사본, TOCBLOCK 4중 사본, VMDB 1부를 읽고 검증하는 상위
 *   레벨 함수. 내부적으로 ldm_parse_privhead()/ldm_parse_tocblock()/
 *   ldm_parse_vmdb()(순수 파싱)와 ldm_compare_privheads()/ldm_compare_tocblocks()
 *   (사본 간 비교)를 호출한다.
 * - ldm_get_vblks(): VMDB 뒤에 이어지는 VBLK 레코드 테이블 전체를 섹터 단위로
 *   읽으며, 조각나지 않은 레코드는 바로 ldm_ldmdb_add()에, 조각난 레코드는
 *   ldm_frag_add()의 임시 struct frag 리스트에 넘긴다.
 * - ldm_parse_vblk() 및 그 헬퍼(ldm_parse_cmp3/dgr3/dgr4/dsk3/dsk4/prt3/vol5):
 *   VBLK 공통 헤더를 읽은 뒤 type에 따라 union vblk의 해당 멤버를 채운다.
 *   ldm_relative()/ldm_get_vnum()/ldm_get_vstr()는 LDM 특유의 "길이-접두
 *   가변폭 필드" 인코딩을 해석하는 공용 헬퍼다.
 * - ldm_create_data_partitions(): ldm_get_disk_objid()로 현재 물리 디스크에
 *   해당하는 Disk VBLK를 찾은 뒤, v_part 리스트를 순회하며 그 디스크에 속한
 *   파티션만 골라 put_partition()으로 등록한다.
 * - _ldm_printk()/ldm_debug()/ldm_info()/ldm_error()/ldm_crit(): 이 파일 전용
 *   로깅 헬퍼. ldm_debug()만 CONFIG_LDM_DEBUG가 꺼져 있으면 완전한 NOP으로
 *   컴파일된다.
 */

#include <linux/slab.h>
/* [한국어] kmalloc()/kfree()/kmalloc_obj()/kmalloc_objs(): 이 파일 전체의 힙 할당은 슬랩 할당자를 통한다 - PRIVHEAD 백업 2부(ph[1]/ph[2]), TOCBLOCK 백업 3부(tb[1..3]),
 * struct vblk 하나, struct ldmdb 하나가 모두 이 계열 함수로 할당되며 kzalloc류는 전혀 쓰이지 않는다(즉 미대입 필드는 초기화되지 않은 채로 남을 수 있음). */
#include <linux/pagemap.h>
/* [한국어] read_mapping_folio() 등 페이지 캐시 API가 정의된 헤더 - check.h의 read_part_sector()가 내부적으로 사용하며, 이 파일은 그 반환값(u8 * 가상 주소)만 소비한다. */
#include <linux/stringify.h>
/* [한국어] __stringify() 매크로 정의 헤더. 이 파일에서 __stringify() 자체를 직접 호출하는 코드는 보이지 않아 실사용처는 확인되지 않는다(추정: 과거 코드의 잔재이거나 매크로 확장 경로에서 간접적으로 필요할 수 있음). */
#include <linux/kernel.h>
/* [한국어] printk()/KERN_* 로그 레벨 매크로 - _ldm_printk()가 이 파일 전용 ldm_debug/info/ error/crit() 매크로를 실제 커널 로그로 출력할 때 사용한다. */
#include <linux/uuid.h>
/* [한국어] uuid_t/uuid_parse()/uuid_equal()/import_uuid(): PRIVHEAD와 Disk VBLK의 GUID를 파싱/비교하는 데 쓰인다. 주의: ldm.h는 uuid_t를 필드 타입으로 쓰면서도 이 헤더를 스스로 include하지 않으므로, 이 줄이 반드시 아래쪽의 "ldm.h" include보다 앞에 와야 컴파일된다(암묵적 순서 의존). */
#include <linux/msdos_partition.h>
/* [한국어] struct msdos_partition/MSDOS_LABEL_MAGIC: ldm_validate_partition_table()이 MBR 부트섹터(오프셋 0x1BE)의 4개 파티션 엔트리를 이 구조체로 캐스팅해 타입 0x42(LDM_PARTITION) 여부를 검사할 때 사용한다. */

#include "ldm.h"
/* [한국어] 이 파일이 사용하는 온디스크 상수(MAGIC_*, VBLK_*, OFF_* 등)와 in-memory 구조체(privhead/tocblock/vmdb/vblk_시리즈/vblk/ldmdb/frag) 전체를 가져온다 - 이 파일의 핵심 타입 정의 원천. 위에서 언급했듯 <linux/uuid.h>가 반드시 이 줄보다 먼저 include돼 있어야 ldm.h 안의 uuid_t 필드가 컴파일된다. */
#include "check.h"
/* [한국어] struct parsed_partitions, typedef Sector, read_part_sector()/put_dev_sector()/ put_partition() 등 파티션 프레임워크 공용 계약을 가져온다 - 이 디렉터리의 모든 파티션 프로버가 공유하는 유일한 인터페이스. */

/*
 * ldm_debug/info/error/crit - Output an error message
 * @f:    A printf format string containing the message
 * @...:  Variables to substitute into @f
 *
 * ldm_debug() writes a DEBUG level message to the syslog but only if the
 * driver was compiled with debug enabled. Otherwise, the call turns into a NOP.
 */
/* [한국어] 위 원문 docbook 주석 보강: ldm_debug/info/error/crit()는 printf 형식 문자열 @f와
 * 가변 인자 @...를 받아 이 파일 전용 포맷("%s%s(): %pV\n")으로 syslog에 출력하는
 * 매크로들이다. ldm_debug()만 CONFIG_LDM_DEBUG가 꺼져 있으면 완전한 NOP(do {} while (0))
 * 으로 컴파일돼, 디버그 로그가 배포 커널의 코드 크기/성능에 영향을 주지 않도록 한다. */
#ifndef CONFIG_LDM_DEBUG
/* [한국어] CONFIG_LDM_DEBUG가 꺼져 있을 때: ldm_debug(...)를 완전히 아무 일도 하지 않는 do-while(0)로 치환한다 - 인자를 평가조차 하지 않으므로 디버그 로그용 문자열 리터럴이 바이너리에도 남지 않는다(컴파일러가 최적화). */
#define ldm_debug(...)	do {} while (0)
/* [한국어] ldm_debug(...)가 아무 일도 하지 않는 구문으로 확장됨 - do/while(0) 관용구는 매크로를 마치 함수 호출처럼 세미콜론과 함께 쓸 수 있게 해주는 표준 커널 기법. */
#else
/* [한국어] CONFIG_LDM_DEBUG가 켜져 있을 때의 대안 분기. */
#define ldm_debug(f, a...) _ldm_printk (KERN_DEBUG, __func__, f, ##a)
/* [한국어] ldm_debug(f, a...)를 KERN_DEBUG 레벨로 _ldm_printk()를 호출하도록 확장 - __func__로 호출한 함수 이름을 자동으로 넣어준다(가변 인자 매크로 ##a는 인자가 없어도 콤마 문제 없이 확장되는 GNU 확장 문법). */
#endif
/* [한국어] #ifndef CONFIG_LDM_DEBUG 분기 종료. */

#define ldm_crit(f, a...)  _ldm_printk (KERN_CRIT,  __func__, f, ##a)
/* [한국어] ldm_crit(): KERN_CRIT(치명적) 레벨 로그 - 데이터베이스가 손상돼 더 이상 진행할 수 없는 상황(TOCBLOCK/VMDB 찾기 실패 등)에서 쓰인다. */
#define ldm_error(f, a...) _ldm_printk (KERN_ERR,   __func__, f, ##a)
/* [한국어] ldm_error(): KERN_ERR 레벨 로그 - 버전 불일치, 범위 초과 등 개별 검증 실패 시 쓰인다. */
#define ldm_info(f, a...)  _ldm_printk (KERN_INFO,  __func__, f, ##a)
/* [한국어] ldm_info(): KERN_INFO 레벨 로그 - 에러는 아니지만 사용자에게 알릴 만한 이례적 상황(예: config_size가 기본값과 다름)에 쓰인다. 이 세 매크로는 CONFIG_LDM_DEBUG와 무관하게 항상 활성화된다(ldm_debug()만 조건부). */

/*
 * [한국어]
 * _ldm_printk() - ldm_debug/info/error/crit() 매크로가 공통으로 호출하는 실제 출력 함수
 *
 * @level:    printk() KERN_* 로그 레벨 문자열 (매크로가 KERN_DEBUG/INFO/ERR/CRIT 중 하나로 전달)
 * @function: 로그를 남긴 함수 이름 - 매크로가 __func__로 자동 전달
 * @fmt:      printf 형식의 나머지 메시지 포맷
 * @...:      @fmt에 대응하는 가변 인자
 * @return:   없음 (void)
 *
 * 이 파일의 모든 로그 매크로(ldm_debug/info/error/crit)가 결국 이 함수 하나로 수렴하도록 만들어,
 * "함수이름(): 메시지" 형태의 통일된 로그 포맷을 보장한다. va_list로 가변 인자를 받아 struct
 * va_format(vaf)에 담고, printk()의 %pV 확장 지정자(포맷 문자열 자체를 지연 평가)로 한 번에
 * 출력한다. __printf(3,4) 속성은 컴파일러가 세 번째 인자(fmt)를 printf 형식 문자열로, 네 번째부터를
 * 그 가변 인자로 취급해 형식 문자열 오류를 정적으로 검사하게 한다. 실행 컨텍스트는 호출한 파싱
 * 함수와 동일한 프로세스 컨텍스트이며, 재진입/동시성 문제는 printk() 자체의 내부 락에 위임된다.
 *
 * 호출 체인:
 *   ldm_debug/info/error/crit(매크로) -> [_ldm_printk] -> printk()
 */
static __printf(3, 4)
/* [한국어] level: KERN_DEBUG 등 로그 레벨 문자열, function: __func__로 전달된 호출자 함수 이름, fmt/...: printf 형식 메시지. __printf(3,4)는 gcc/clang이 fmt(3번째 인자)와 그 뒤 가변 인자를 printf 형식 검사 대상으로 인식하게 하는 속성이다. */
void _ldm_printk(const char *level, const char *function, const char *fmt, ...)
{
	/* [한국어] printk()의 지연 포맷팅(%pV)에 넘길 va_format 구조체 - fmt 문자열과 va_list 포인터를 함께 담아 printk() 내부에서 한 번에 포맷팅되도록 한다. */
	struct va_format vaf;
	/* [한국어] fmt 뒤의 가변 인자들을 순회하기 위한 표준 C va_list 핸들. */
	va_list args;

	/* [한국어] va_start로 fmt 매개변수 바로 다음부터 가변 인자 목록을 열어 args에 바인딩. */
	va_start (args, fmt);

	/* [한국어] vaf.fmt에 실제 포맷 문자열 대입 - printk의 %pV가 이 필드를 읽어 지연 포맷팅한다. */
	vaf.fmt = fmt;
	/* [한국어] vaf.va에 args의 주소 대입 - %pV 처리기가 이 va_list를 이용해 인자를 소비한다. */
	vaf.va = &args;

	/* [한국어] 실제 커널 로그 출력. level(KERN_*) + "함수이름(): " 접두어 + %pV(지연 포맷된 메시지) + 개행 순서로 한 줄을 만든다 - 이 파일의 모든 로그가 이 한 줄로 귀결된다. */
	printk("%s%s(): %pV\n", level, function, &vaf);

	/* [한국어] va_start와 짝을 이루는 마무리 호출 - va_list 자원을 정리한다(대부분의 아키텍처에서는 no-op이지만 이식성을 위해 항상 호출해야 함). */
	va_end(args);
}

/*
 * [한국어]
 * ldm_parse_privhead() - PRIVHEAD 섹터 원시 바이트를 struct privhead로 파싱
 *
 * @data: read_part_sector()가 읽어온 PRIVHEAD 섹터 원시 바이트(512B) 시작 주소
 * @ph:   파싱 결과를 채워 넣을 in-memory struct privhead (호출자가 마련한 버퍼)
 * @return: true  - @ph에 유효한 PRIVHEAD 내용이 채워짐
 *          false - 매직 불일치/버전 불일치/범위 오류/GUID 파싱 실패 등으로 @ph 내용이 정의되지 않음
 *
 * PRIVHEAD는 디스크당 정확히 한 벌(주 사본 + 2개 백업)만 존재하는, LDM 데이터베이스에서 가장
 * 먼저 확인해야 하는 구조체다. 이 함수는 (1) 8바이트 매직 "PRIVHEAD" 확인, (2) 버전 필드를 읽어
 * Win2000/XP(2.11) 또는 Vista(2.12)인지 확인, (3) 논리 디스크/설정 DB 영역의 시작 LBA와 크기를
 * 읽고 논리 디스크 영역이 설정 DB 영역을 침범하지 않는지 검사, (4) config_size가 기대값
 * (LDM_DB_SIZE)과 다르면 경고만 남기고 계속 진행, (5) 물리 디스크 GUID를 uuid_parse()로 파싱하는
 * 순서로 진행한다. 각 단계는 실패 시 즉시 false를 반환하는 얕은 중첩(guard clause) 스타일이다.
 * 실행 컨텍스트는 파티션 스캔 중인 단일 프로세스 컨텍스트이며 재진입 문제는 없다.
 *
 * 호출 체인:
 *   ldm_validate_privheads -> [ldm_parse_privhead]
 */
/**
 * ldm_parse_privhead - Read the LDM Database PRIVHEAD structure
 * @data:  Raw database PRIVHEAD structure loaded from the device
 * @ph:    In-memory privhead structure in which to return parsed information
 *
 * This parses the LDM database PRIVHEAD structure supplied in @data and
 * sets up the in-memory privhead structure @ph with the obtained information.
 *
 * Return:  'true'   @ph contains the PRIVHEAD data
 *          'false'  @ph contents are undefined
 */
/* [한국어] data: 파싱할 PRIVHEAD 섹터 원시 바이트, ph: 결과를 채울 in-memory 구조체. */
static bool ldm_parse_privhead(const u8 *data, struct privhead *ph)
{
	/* [한국어] Windows Vista(버전 2.12) 여부를 나타내는 지역 플래그 - 로그 메시지 문구 선택에만 쓰인다. */
	/* [한국어] 기본값 false: 아직 Vista로 확인되지 않은 상태에서 시작. */
	bool is_vista = false;

	/* [한국어] 호출 계약 위반(널 포인터) 검사 - data/ph 둘 중 하나라도 NULL이면 커널 패닉으로 즉시 중단(프로그래밍 오류를 조기에 발견하기 위한 방어적 단언). */
	BUG_ON(!data || !ph);
	/* [한국어] data 앞 8바이트를 빅엔디안 64비트로 읽어 MAGIC_PRIVHEAD("PRIVHEAD" 문자열)와 비교 - PRIVHEAD 시그니처가 없으면 이 섹터는 PRIVHEAD가 아니거나 손상된 것. */
	if (MAGIC_PRIVHEAD != get_unaligned_be64(data)) {
		/* [한국어] 매직 불일치 - 이 섹터에서 PRIVHEAD를 찾지 못했다는 치명적 오류를 로그로 남김. */
		ldm_error("Cannot find PRIVHEAD structure. LDM database is"
			" corrupt. Aborting.");
		/* [한국어] 파싱 실패를 호출자에게 알림 - ph 내용은 정의되지 않은 상태로 남는다. */
		return false;
	}
	/* [한국어] data+0x000C에서 빅엔디안 16비트로 주 버전 번호를 읽어 ph->ver_major에 저장. */
	ph->ver_major = get_unaligned_be16(data + 0x000C);
	/* [한국어] data+0x000E에서 빅엔디안 16비트로 부 버전 번호를 읽어 ph->ver_minor에 저장. */
	ph->ver_minor = get_unaligned_be16(data + 0x000E);
	/* [한국어] data+0x011B에서 빅엔디안 64비트로 논리 디스크(사용자 데이터 영역) 시작 LBA를 읽음 - 이후 파티션 절대 주소 계산의 기준점이 되는 핵심 값. */
	ph->logical_disk_start = get_unaligned_be64(data + 0x011B);
	/* [한국어] data+0x0123에서 빅엔디안 64비트로 논리 디스크 영역 크기(섹터)를 읽음. */
	ph->logical_disk_size = get_unaligned_be64(data + 0x0123);
	/* [한국어] data+0x012B에서 빅엔디안 64비트로 LDM 설정 데이터베이스 시작 LBA를 읽음 - 이후 모든 OFF_TOCB 계열 및 OFF_VMDB 상대 오프셋의 기준(base)이 되는 값. */
	ph->config_start = get_unaligned_be64(data + 0x012B);
	/* [한국어] data+0x0133에서 빅엔디안 64비트로 설정 데이터베이스 크기(섹터)를 읽음. */
	ph->config_size = get_unaligned_be64(data + 0x0133);
	/* Version 2.11 is Win2k/XP and version 2.12 is Vista. */
	/* [한국어] 버전 2.12는 Windows Vista가 사용하는 PRIVHEAD 포맷 - 원문 주석대로 2.11은 Win2k/XP, 2.12는 Vista. */
	if (ph->ver_major == 2 && ph->ver_minor == 12)
		/* [한국어] Vista로 확인되면 플래그를 세워, 이후 버전 검사와 로그 문구에 반영한다. */
		is_vista = true;
	/* [한국어] Vista(2.12)도 아니고 Win2k/XP(2.11)도 아니면 지원하지 않는 버전 - 계속 진행하면 이후 오프셋 해석이 틀어질 수 있으므로 여기서 중단한다. */
	if (!is_vista && (ph->ver_major != 2 || ph->ver_minor != 11)) {
		/* [한국어] 어떤 버전이 발견됐는지와 함께 에러 로그 - 사용자가 원인을 파악할 수 있도록 실제 값을 포맷 문자열에 포함. */
		ldm_error("Expected PRIVHEAD version 2.11 or 2.12, got %d.%d."
			" Aborting.", ph->ver_major, ph->ver_minor);
		/* [한국어] 지원하지 않는 버전이므로 파싱 실패로 반환. */
		return false;
	}
	/* [한국어] 여기까지 왔다면 버전 검증 통과 - 어떤 버전(2000/XP 또는 Vista)인지 디버그 로그로 남김(운영 환경에서는 CONFIG_LDM_DEBUG가 꺼져 있으면 이 호출이 NOP). */
	ldm_debug("PRIVHEAD version %d.%d (Windows %s).", ph->ver_major,
			ph->ver_minor, is_vista ? "Vista" : "2000/XP");
	/* [한국어] 설정 DB 크기가 표준값(LDM_DB_SIZE=2048섹터=1MiB)과 다른지 검사 - 원문 인라인 주석대로 1MiB를 섹터로 환산한 값과 비교. */
	if (ph->config_size != LDM_DB_SIZE) {	/* 1 MiB in sectors. */
		/* [한국어] 표준과 다르더라도 치명적 오류로 취급하지 않고 계속 진행하되, 사용자가 알 수 있도록 안내 로그만 남긴다(방어적이지만 관대한 정책). */
		/* Warn the user and continue, carefully. */
		/* [한국어] 기대값과 실제 값을 함께 로그에 남겨 진단을 돕는다. */
		ldm_info("Database is normally %u bytes, it claims to "
			"be %llu bytes.", LDM_DB_SIZE,
			(unsigned long long)ph->config_size);
	}
	/* [한국어] 논리 디스크 크기가 0이거나(비정상), 논리 디스크 영역(시작+크기)이 설정 DB 영역 시작을 넘어서면(두 영역이 겹치면) 디스크 레이아웃이 손상된 것으로 간주. */
	if ((ph->logical_disk_size == 0) || (ph->logical_disk_start +
			ph->logical_disk_size > ph->config_start)) {
		/* [한국어] 두 영역이 겹치는 심각한 불일치이므로 에러 로그 후 실패 처리. */
		ldm_error("PRIVHEAD disk size doesn't match real disk size");
		/* [한국어] 이 오류는 방금 전 config_size 불일치와 달리 관대하게 넘어가지 않고 즉시 실패로 처리 - 실제 디스크 접근 범위를 벗어날 수 있는 위험한 상황이기 때문. */
		return false;
	}
	/* [한국어] data+0x0030 위치의 16바이트를 표준 GUID 문자열/바이너리 형식으로 파싱해 ph->disk_id(uuid_t)에 저장 - 실패(0이 아닌 반환값)하면 GUID가 깨진 것. */
	if (uuid_parse(data + 0x0030, &ph->disk_id)) {
		/* [한국어] GUID 파싱 실패 로그. */
		ldm_error("PRIVHEAD contains an invalid GUID.");
		/* [한국어] 유효한 물리 디스크 식별자를 얻지 못했으므로 실패 처리. */
		return false;
	}
	/* [한국어] 여기까지 도달했으면 모든 검증을 통과 - 성공 디버그 로그. */
	ldm_debug("Parsed PRIVHEAD successfully.");
	/* [한국어] @ph가 완전히 채워졌음을 호출자에게 알림. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_tocblock() - TOCBLOCK 섹터 원시 바이트를 struct tocblock으로 파싱
 *
 * @data: read_part_sector()가 읽어온 TOCBLOCK 섹터 원시 바이트(512B) 시작 주소
 * @toc:  파싱 결과를 채워 넣을 in-memory struct tocblock
 * @return: true  - @toc에 유효한 TOCBLOCK 내용이 채워짐
 *          false - 매직 불일치 또는 비트맵 이름 불일치로 @toc 내용이 정의되지 않음
 *
 * TOCBLOCK(Table Of Contents)은 데이터베이스 안에 정확히 두 개의 비트맵("config"와 "log")이
 * 있다는 전제 하에 그 위치/크기만 담는 목차다. 이 함수는 8바이트 매직 "TOCBLOCK"을 먼저 확인한
 * 뒤, 첫 번째 비트맵의 이름/시작/크기를 읽어 이름이 반드시 "config"인지 검증하고, 두 번째
 * 비트맵도 같은 방식으로 읽어 이름이 "log"인지 검증한다. 원문 주석대로 여기서 읽은 *_start/
 * *_size 값 자체의 범위 검사(디스크 용량을 넘지 않는지 등)는 이 함수의 책임이 아니며, 호출자인
 * ldm_validate_tocblocks()가 나중에 수행한다.
 *
 * 호출 체인:
 *   ldm_validate_tocblocks -> [ldm_parse_tocblock]
 */
/**
 * ldm_parse_tocblock - Read the LDM Database TOCBLOCK structure
 * @data:  Raw database TOCBLOCK structure loaded from the device
 * @toc:   In-memory toc structure in which to return parsed information
 *
 * This parses the LDM Database TOCBLOCK (table of contents) structure supplied
 * in @data and sets up the in-memory tocblock structure @toc with the obtained
 * information.
 *
 * N.B.  The *_start and *_size values returned in @toc are not range-checked.
 *
 * Return:  'true'   @toc contains the TOCBLOCK data
 *          'false'  @toc contents are undefined
 */
/* [한국어] data: 파싱할 TOCBLOCK 섹터 원시 바이트, toc: 결과를 채울 in-memory 구조체. */
static bool ldm_parse_tocblock (const u8 *data, struct tocblock *toc)
{
	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!data || !toc);

	/* [한국어] data 앞 8바이트를 빅엔디안 64비트로 읽어 MAGIC_TOCBLOCK("TOCBLOCK" 문자열)과 비교. */
	if (MAGIC_TOCBLOCK != get_unaligned_be64(data)) {
		/* [한국어] 매직 불일치 - TOCBLOCK을 찾지 못했다는 치명적 오류(ldm_crit). */
		ldm_crit ("Cannot find TOCBLOCK, database may be corrupt.");
		/* [한국어] 파싱 실패 반환. */
		return false;
	}
	/* [한국어] data+0x24 위치의 첫 번째 비트맵 이름 문자열을 sizeof(toc->bitmap1_name)만큼 널 패딩하며 복사 - strscpy_pad()는 남는 바이트를 0으로 채워 이후 strncmp() 비교를 안전하게 만든다. */
	strscpy_pad(toc->bitmap1_name, data + 0x24, sizeof(toc->bitmap1_name));
	/* [한국어] data+0x2E에서 빅엔디안 64비트로 첫 번째("config") 비트맵의 시작 오프셋(섹터, config_start 기준 상대값)을 읽음. */
	toc->bitmap1_start = get_unaligned_be64(data + 0x2E);
	/* [한국어] data+0x36에서 빅엔디안 64비트로 첫 번째 비트맵의 크기(섹터)를 읽음. */
	toc->bitmap1_size  = get_unaligned_be64(data + 0x36);

	/* [한국어] 방금 읽은 이름이 TOC_BITMAP1("config") 상수와 정확히 일치하는지 strncmp()로 검사. */
	if (strncmp (toc->bitmap1_name, TOC_BITMAP1,
			sizeof (toc->bitmap1_name)) != 0) {
		/* [한국어] 이름이 다르면 TOCBLOCK 레이아웃 자체가 예상과 다른 것 - 어떤 이름이 나왔는지 함께 로그로 남김. */
		ldm_crit ("TOCBLOCK's first bitmap is '%s', should be '%s'.",
				TOC_BITMAP1, toc->bitmap1_name);
		/* [한국어] 파싱 실패 반환. */
		return false;
	}
	/* [한국어] data+0x46 위치의 두 번째 비트맵 이름을 마찬가지로 널 패딩 복사. */
	strscpy_pad(toc->bitmap2_name, data + 0x46, sizeof(toc->bitmap2_name));
	/* [한국어] data+0x50에서 빅엔디안 64비트로 두 번째("log") 비트맵 시작 오프셋을 읽음. */
	toc->bitmap2_start = get_unaligned_be64(data + 0x50);
	/* [한국어] data+0x58에서 빅엔디안 64비트로 두 번째 비트맵 크기를 읽음. */
	toc->bitmap2_size  = get_unaligned_be64(data + 0x58);
	/* [한국어] 두 번째 비트맵 이름이 TOC_BITMAP2("log")와 일치하는지 검사. */
	if (strncmp (toc->bitmap2_name, TOC_BITMAP2,
			sizeof (toc->bitmap2_name)) != 0) {
		/* [한국어] 이름 불일치 로그. */
		ldm_crit ("TOCBLOCK's second bitmap is '%s', should be '%s'.",
				TOC_BITMAP2, toc->bitmap2_name);
		/* [한국어] 파싱 실패 반환. */
		return false;
	}
	/* [한국어] 두 비트맵 모두 이름 검증을 통과 - 성공 디버그 로그. */
	ldm_debug ("Parsed TOCBLOCK successfully.");
	/* [한국어] @toc가 완전히 채워졌음을 호출자에게 알림. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_vmdb() - VMDB 섹터 원시 바이트를 struct vmdb로 파싱
 *
 * @data: read_part_sector()가 읽어온 VMDB 섹터 원시 바이트(512B) 시작 주소
 * @vm:   파싱 결과를 채워 넣을 in-memory struct vmdb
 * @return: true  - @vm에 유효한 VMDB 정보가 채워짐
 *          false - 매직/버전 불일치 또는 vblk_size가 0이라 @vm 내용이 정의되지 않음
 *
 * VMDB(Virtual [Media] DataBase)는 이어지는 VBLK 레코드 테이블의 메타데이터(레코드 크기, 시작
 * 오프셋, 마지막 시퀀스 번호)를 담는 헤더로, 데이터베이스 안에 정확히 1부만 존재한다. 이 함수는
 * (1) 4바이트 매직 "VMDB" 확인, (2) 버전이 정확히 4.10인지 확인(PRIVHEAD와 달리 단일 버전만
 * 허용), (3) vblk_size가 0이면 나눗셈에 쓰일 값이므로 즉시 실패 처리, (4) vblk_offset과
 * last_vblk_seq를 읽는 순서로 진행한다. 원문 주석대로 여기서 읽은 시작/크기/시퀀스류 값의
 * 범위 검사는 호출자인 ldm_validate_vmdb()가 나중에 수행한다.
 *
 * 호출 체인:
 *   ldm_validate_vmdb -> [ldm_parse_vmdb]
 */
/**
 * ldm_parse_vmdb - Read the LDM Database VMDB structure
 * @data:  Raw database VMDB structure loaded from the device
 * @vm:    In-memory vmdb structure in which to return parsed information
 *
 * This parses the LDM Database VMDB structure supplied in @data and sets up
 * the in-memory vmdb structure @vm with the obtained information.
 *
 * N.B.  The *_start, *_size and *_seq values will be range-checked later.
 *
 * Return:  'true'   @vm contains VMDB info
 *          'false'  @vm contents are undefined
 */
/* [한국어] data: 파싱할 VMDB 섹터 원시 바이트, vm: 결과를 채울 in-memory 구조체. */
static bool ldm_parse_vmdb (const u8 *data, struct vmdb *vm)
{
	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!data || !vm);

	/* [한국어] data 앞 4바이트를 빅엔디안 32비트로 읽어 MAGIC_VMDB("VMDB" 문자열)와 비교. */
	if (MAGIC_VMDB != get_unaligned_be32(data)) {
		/* [한국어] 매직 불일치 - VMDB를 찾지 못했다는 치명적 오류. */
		ldm_crit ("Cannot find the VMDB, database may be corrupt.");
		/* [한국어] 파싱 실패 반환. */
		return false;
	}

	/* [한국어] data+0x12에서 빅엔디안 16비트로 VMDB 주 버전을 읽음. */
	vm->ver_major = get_unaligned_be16(data + 0x12);
	/* [한국어] data+0x14에서 빅엔디안 16비트로 VMDB 부 버전을 읽음. */
	vm->ver_minor = get_unaligned_be16(data + 0x14);
	/* [한국어] 정확히 4.10이 아니면(PRIVHEAD의 2중 버전 허용과 달리 단일 버전만 허용) 지원하지 않는 포맷으로 간주. */
	if ((vm->ver_major != 4) || (vm->ver_minor != 10)) {
		/* [한국어] 기대한 버전(4.10)과 실제 버전을 함께 로그로 남김. */
		ldm_error ("Expected VMDB version %d.%d, got %d.%d. "
			"Aborting.", 4, 10, vm->ver_major, vm->ver_minor);
		/* [한국어] 파싱 실패 반환. */
		return false;
	}

	/* [한국어] data+0x08에서 빅엔디안 32비트로 VBLK 레코드 하나의 온디스크 크기(바이트)를 읽음 - 이후 섹터당 VBLK 개수 계산(perbuf = 512/size)의 분모가 되는 핵심 값. */
	vm->vblk_size     = get_unaligned_be32(data + 0x08);
	/* [한국어] 크기가 0이면 이후 나눗셈(512/size)에서 0으로 나누게 되므로 반드시 여기서 걸러야 한다. */
	if (vm->vblk_size == 0) {
		/* [한국어] 잘못된 VBLK 크기 로그. */
		ldm_error ("Illegal VBLK size");
		/* [한국어] 파싱 실패 반환. */
		return false;
	}

	/* [한국어] data+0x0C에서 빅엔디안 32비트로 VBLK 테이블이 VMDB로부터 시작하는 바이트 오프셋을 읽음(정상적으로는 512, 즉 다음 섹터부터 시작). */
	vm->vblk_offset   = get_unaligned_be32(data + 0x0C);
	/* [한국어] data+0x04에서 빅엔디안 32비트로 마지막 유효 VBLK의 시퀀스 번호를 읽음 - VBLK 테이블을 몇 섹터까지 스캔해야 하는지 결정하는 값. */
	vm->last_vblk_seq = get_unaligned_be32(data + 0x04);

	/* [한국어] 성공 디버그 로그. */
	ldm_debug ("Parsed VMDB successfully.");
	/* [한국어] @vm이 완전히 채워졌음을 호출자에게 알림. */
	return true;
}

/*
 * [한국어]
 * ldm_compare_privheads() - 두 PRIVHEAD 객체가 내용상 동일한지 비교
 *
 * @ph1: 첫 번째 privhead(보통 주 사본)
 * @ph2: 두 번째 privhead(보통 백업 사본)
 * @return: true  - 두 privhead의 모든 비교 대상 필드가 동일
 *          false - 하나라도 다름
 *
 * PRIVHEAD는 디스크 손상에 대비해 3중으로 저장되므로, 각 사본이 서로 일치하는지 확인해야
 * 데이터베이스를 신뢰할 수 있다. 이 함수는 버전(major/minor), 논리 디스크 시작/크기, 설정 DB
 * 시작/크기, 그리고 물리 디스크 GUID(uuid_equal())까지 총 7개 필드를 모두 AND로 묶어 비교한다 -
 * 하나라도 다르면 전체 결과가 false다. 순수 비교 함수로 부작용이 없다.
 *
 * 호출 체인:
 *   ldm_validate_privheads -> [ldm_compare_privheads]
 */
/**
 * ldm_compare_privheads - Compare two privhead objects
 * @ph1:  First privhead
 * @ph2:  Second privhead
 *
 * This compares the two privhead structures @ph1 and @ph2.
 *
 * Return:  'true'   Identical
 *          'false'  Different
 */
/* [한국어] ph1/ph2: 비교할 두 privhead 포인터(순서는 의미 없음, 대칭 비교). */
static bool ldm_compare_privheads (const struct privhead *ph1,
				   const struct privhead *ph2)
{
	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!ph1 || !ph2);

	/* [한국어] 아래 7개 필드를 모두 &&로 연결 - 하나라도 다르면 전체가 false가 되는 단축 평가(short-circuit) 비교. 버전(major/minor)부터 시작. */
	return ((ph1->ver_major          == ph2->ver_major)		&&
		/* [한국어] 부 버전 비교. */
		(ph1->ver_minor          == ph2->ver_minor)		&&
		/* [한국어] 논리 디스크 시작 LBA 비교 - 다르면 두 사본이 가리키는 물리적 위치가 달라 심각한 손상을 의미. */
		(ph1->logical_disk_start == ph2->logical_disk_start)	&&
		/* [한국어] 논리 디스크 크기 비교. */
		(ph1->logical_disk_size  == ph2->logical_disk_size)	&&
		/* [한국어] 설정 DB 시작 LBA 비교. */
		(ph1->config_start       == ph2->config_start)		&&
		/* [한국어] 설정 DB 크기 비교. */
		(ph1->config_size        == ph2->config_size)		&&
		/* [한국어] 물리 디스크 GUID를 uuid_equal()로 비교(단순 memcmp가 아니라 uuid 전용 비교 함수 사용) - 최종 결과가 이 함수의 반환값이 된다. */
		uuid_equal(&ph1->disk_id, &ph2->disk_id));
}

/*
 * [한국어]
 * ldm_compare_tocblocks() - 두 TOCBLOCK 객체가 내용상 동일한지 비교
 *
 * @toc1: 첫 번째 tocblock
 * @toc2: 두 번째 tocblock
 * @return: true  - 두 tocblock의 비트맵 위치/크기/이름이 모두 동일
 *          false - 하나라도 다름
 *
 * TOCBLOCK도 최대 4중으로 저장되므로 사본 간 일치 여부를 확인해야 한다. 이 함수는 두 비트맵
 * 각각의 시작/크기(4개 필드)와 이름 문자열 2개(strncmp())까지 총 6개 조건을 &&로 묶어 비교한다.
 * ldm_compare_privheads()와 마찬가지로 순수 비교 함수다.
 *
 * 호출 체인:
 *   ldm_validate_tocblocks -> [ldm_compare_tocblocks]
 */
/**
 * ldm_compare_tocblocks - Compare two tocblock objects
 * @toc1:  First toc
 * @toc2:  Second toc
 *
 * This compares the two tocblock structures @toc1 and @toc2.
 *
 * Return:  'true'   Identical
 *          'false'  Different
 */
/* [한국어] toc1/toc2: 비교할 두 tocblock 포인터. */
static bool ldm_compare_tocblocks (const struct tocblock *toc1,
				   const struct tocblock *toc2)
{
	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!toc1 || !toc2);

	/* [한국어] 첫 번째 비트맵 시작 오프셋 비교부터 시작해 &&로 연결. */
	return ((toc1->bitmap1_start == toc2->bitmap1_start)	&&
		/* [한국어] 첫 번째 비트맵 크기 비교. */
		(toc1->bitmap1_size  == toc2->bitmap1_size)	&&
		/* [한국어] 두 번째 비트맵 시작 오프셋 비교. */
		(toc1->bitmap2_start == toc2->bitmap2_start)	&&
		/* [한국어] 두 번째 비트맵 크기 비교. */
		(toc1->bitmap2_size  == toc2->bitmap2_size)	&&
		/* [한국어] 첫 번째 비트맵 이름을 strncmp()로 비교(0이면 동일하므로 !strncmp로 부정). */
		!strncmp (toc1->bitmap1_name, toc2->bitmap1_name,
			sizeof (toc1->bitmap1_name))		&&
		/* [한국어] 두 번째 비트맵 이름 비교 - 최종 결과가 이 함수의 반환값이 된다. */
		!strncmp (toc1->bitmap2_name, toc2->bitmap2_name,
			sizeof (toc1->bitmap2_name)));
}

/*
 * [한국어]
 * ldm_validate_privheads() - PRIVHEAD 주 사본과 두 백업을 읽어 비교/검증
 *
 * @state: 대상 블록 디바이스를 담은 파티션 스캔 상태(read_part_sector()에 전달됨)
 * @ph1:   검증 통과 시 최종 대표값을 채워 넣을 in-memory struct privhead (호출자가 마련한 버퍼,
 *         보통 ldb->ph)
 * @return: true  - @ph1에 검증된 PRIVHEAD 내용이 채워짐
 *          false - 디스크 읽기 실패, 파싱 실패, 범위 초과, 또는 사본 불일치로 실패
 *
 * PRIVHEAD는 디스크 절대 섹터 OFF_PRIV1(=6)의 주 사본과, 그로부터 얻은 config_start를 기준으로
 *한 OFF_PRIV2/OFF_PRIV3 위치의 두 백업까지 총 3벌이 존재한다. 이 함수는 세 사본을 차례로 읽어
 * ldm_parse_privhead()로 파싱하되, 세 번째(마지막) 백업 읽기 실패는 홀수 크기 디스크에서 흔히
 * 발생할 수 있어 무시하고 넘어간다(원문 FIXME 주석). 이후 디스크 전체 용량(get_capacity())을
 * 기준으로 설정 DB 영역과 논리 디스크 영역이 디스크 범위를 벗어나거나 서로 겹치지 않는지
 * range-check하고, 마지막으로 ldm_compare_privheads()로 주 사본과 첫 번째 백업이 일치하는지
 * 검증한다(두 번째 백업과의 비교는 원문에서 FIXME로 주석 처리되어 건너뜀). 성공 시 임시로
 * 할당했던 백업 버퍼(ph[1], ph[2])는 해제하고 @ph1(=ph[0])만 호출자에게 남긴다.
 *
 * 호출 체인:
 *   ldm_partition -> [ldm_validate_privheads] -> ldm_parse_privhead / ldm_compare_privheads
 */
/**
 * ldm_validate_privheads - Compare the primary privhead with its backups
 * @state: Partition check state including device holding the LDM Database
 * @ph1:   Memory struct to fill with ph contents
 *
 * Read and compare all three privheads from disk.
 *
 * The privheads on disk show the size and location of the main disk area and
 * the configuration area (the database).  The values are range-checked against
 * @hd, which contains the real size of the disk.
 *
 * Return:  'true'   Success
 *          'false'  Error
 */
/* [한국어] state: read_part_sector() 호출에 필요한 디바이스 컨텍스트, ph1: 최종 대표값을 받을 버퍼(대개 ldb->ph의 주소). */
static bool ldm_validate_privheads(struct parsed_partitions *state,
				   struct privhead *ph1)
{
	/* [한국어] 세 PRIVHEAD 사본의 상대 오프셋 테이블(섹터 단위) - 인덱스 0이 주 사본. */
	static const int off[3] = { OFF_PRIV1, OFF_PRIV2, OFF_PRIV3 };
	/* [한국어] ph[0]은 호출자가 넘겨준 ph1을 그대로 가리키고, ph[1]/ph[2](백업용)는 아래에서 kmalloc_obj()로 새로 할당한다 - 초기화 목록 { ph1 }은 나머지 원소를 NULL로 채운다. */
	struct privhead *ph[3] = { ph1 };
	/* [한국어] read_part_sector()가 반환한 folio 참조를 담을 핸들 - 사용 후 put_dev_sector()로 반드시 반납. */
	Sector sect;
	/* [한국어] read_part_sector()가 반환하는 섹터 데이터 포인터. */
	u8 *data;
	/* [한국어] 함수 최종 반환값 - 기본값 false(실패)로 시작해 모든 검증을 통과했을 때만 true로 바뀐다. */
	bool result = false;
	/* [한국어] get_capacity()로 얻을 디스크 전체 섹터 수 - 범위 검사에 사용. */
	long num_sects;
	/* [한국어] 세 사본을 순회할 루프 인덱스. */
	int i;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!state || !ph1);

	/* [한국어] 첫 번째 백업용 struct privhead를 힙에 할당(sizeof(*ph[1])만큼, kmalloc_obj는 타입을 인자 표현식에서 추론하는 편의 매크로) - zero-fill은 없으므로 파싱 실패 시 내용은 쓰레기 값일 수 있다. */
	ph[1] = kmalloc_obj(*ph[1]);
	/* [한국어] 두 번째 백업용 struct privhead를 마찬가지로 할당. */
	ph[2] = kmalloc_obj(*ph[2]);
	/* [한국어] 둘 중 하나라도 할당 실패하면 계속 진행할 수 없음. */
	if (!ph[1] || !ph[2]) {
		/* [한국어] 메모리 부족 치명적 오류 로그. */
		ldm_crit ("Out of memory.");
		/* [한국어] result(false)를 반환하기 전에 정리 경로(out)로 점프 - 이미 할당된 쪽이 있어도 out에서 kfree(NULL)은 안전하므로 문제 없다. */
		goto out;
	}

	/* [한국어] off[1]/off[2](OFF_PRIV2/OFF_PRIV3)는 config_start 기준 상대 오프셋으로 정의돼 있으므로, 주 사본을 읽기 전 임시로 기준점을 0으로 리셋 - 아래 루프의 첫 반복(i=0, off[0]=OFF_PRIV1=6)은 디스크 절대 섹터 6을 그대로 가리키게 된다. */
	/* off[1 & 2] are relative to ph[0]->config_start */
	/* [한국어] ph[0]->config_start(아직 파싱 전이므로 이 시점엔 의미 없는 초기값)를 0으로 강제 설정 - 이후 루프에서 "ph[0]->config_start + off[i]"로 절대 섹터를 계산하는 코드가 1회차(i=0)에는 off[0]=6을 그대로 쓰도록 만드는 트릭. */
	ph[0]->config_start = 0;

	/* [한국어] 세 사본을 순서대로 읽고 파싱하는 루프. */
	/* Read and parse privheads */
	/* [한국어] i=0(주 사본, 섹터 6), i=1(백업1), i=2(백업2) 순서로 반복. */
	for (i = 0; i < 3; i++) {
		/* [한국어] ph[0]->config_start(1회차는 0) + off[i] 위치의 섹터를 읽음 - 2/3회차부터는 config_start가 실제 값(1회차 파싱 결과)으로 채워져 있으므로 OFF_PRIV2/3가 진짜 config_start 기준 상대 오프셋으로 해석된다. */
		data = read_part_sector(state, ph[0]->config_start + off[i],
					/* [한국어] read_part_sector()의 두 번째 인자(줄바꿈된 계속) - sect는 반납용 핸들. */
					&sect);
		/* [한국어] 디스크 읽기 자체가 실패한 경우(섹터가 디스크 범위를 넘거나 I/O 오류). */
		if (!data) {
			/* [한국어] 읽기 실패 치명적 오류 로그. */
			ldm_crit ("Disk read failed.");
			/* [한국어] 정리 경로로 점프 - 디스크 읽기 실패는 이후 사본으로도 복구 불가능한 치명적 상황이므로 루프를 계속하지 않는다. */
			goto out;
		}
		/* [한국어] 읽은 섹터를 ldm_parse_privhead()로 파싱해 ph[i]에 채움. */
		result = ldm_parse_privhead (data, ph[i]);
		/* [한국어] 이번 섹터에 대한 folio 참조 반납 - data 포인터는 이 호출 이후 더 이상 유효하지 않음. */
		put_dev_sector (sect);
		/* [한국어] 파싱 자체가 실패한 경우(매직/버전/GUID 오류 등). */
		if (!result) {
			/* [한국어] 이미 ldm_parse_privhead() 내부에서 구체적 원인이 로그로 남았으므로, 여기서는 몇 번째 사본(i+1)이 실패했는지만 추가로 남김. */
			ldm_error ("Cannot find PRIVHEAD %d.", i+1); /* Log again */
			/* [한국어] 주 사본(i=0) 또는 첫 백업(i=1) 실패는 치명적 - 정리 경로로 점프. */
			if (i < 2)
				/* [한국어] 이미 로그를 남겼으므로 추가 설명 없이 바로 점프. */
				goto out;	/* Already logged */
			/* [한국어] i==2(세 번째, 마지막 백업)인 경우에만 아래 else로 진입. */
			else
				/* [한국어] 세 번째 PRIVHEAD 실패는 무시하고 루프를 빠져나감(FIXME 원문 주석: 홀수 크기 디스크에서 흔히 실패할 수 있어 일단 무시) - result는 직전 반복(i=1)에서 이미 true였던 값을 그대로 들고 나간다. */
				break;	/* FIXME ignore for now, 3rd PH can fail on odd-sized disks */
		}
	}

	/* [한국어] 디스크 전체 용량(섹터 수)을 gendisk로부터 조회 - 이후 range-check의 기준값. */
	num_sects = get_capacity(state->disk);

	/* [한국어] 설정 DB 시작이 디스크 용량을 넘거나, 설정 DB(시작+크기)가 디스크 끝을 넘으면 명백히 손상된 값. */
	if ((ph[0]->config_start > num_sects) ||
	   ((ph[0]->config_start + ph[0]->config_size) > num_sects)) {
		/* [한국어] 범위 초과 치명적 오류 로그. */
		ldm_crit ("Database extends beyond the end of the disk.");
		/* [한국어] 정리 경로로 점프. */
		goto out;
	}

	/* [한국어] 논리 디스크 시작이 설정 DB 시작보다 크거나(위치가 뒤바뀌었거나), 논리 디스크 영역(시작+크기)이 설정 DB 시작을 넘어서면(두 영역이 겹치면) 손상된 것. */
	if ((ph[0]->logical_disk_start > ph[0]->config_start) ||
	   ((ph[0]->logical_disk_start + ph[0]->logical_disk_size)
		    > ph[0]->config_start)) {
		/* [한국어] 겹침 오류 치명적 로그. */
		ldm_crit ("Disk and database overlap.");
		/* [한국어] 정리 경로로 점프. */
		goto out;
	}

	/* [한국어] 주 사본(ph[0])과 첫 번째 백업(ph[1])이 내용상 일치하는지 ldm_compare_privheads() 로 검증. */
	if (!ldm_compare_privheads (ph[0], ph[1])) {
		/* [한국어] 불일치 치명적 로그 - 두 사본이 다르면 어느 쪽이 손상됐는지 알 수 없어 안전을 위해 실패 처리. */
		ldm_crit ("Primary and backup PRIVHEADs don't match.");
		/* [한국어] 정리 경로로 점프. */
		goto out;
	}
	/* FIXME ignore this for now
	if (!ldm_compare_privheads (ph[0], ph[2])) {
		ldm_crit ("Primary and backup PRIVHEADs don't match.");
		goto out;
	}*/
	/* [한국어] 원문 FIXME: 주 사본과 두 번째 백업(ph[2])의 비교는 현재 주석 처리(비활성화)돼 있다 - 이 함수 앞부분에서 세 번째 백업은 애초에 읽기 실패해도 무시하도록 짜여 있는 정책과 일관되게, 신뢰성이 상대적으로 낮은 마지막 백업까지는 엄격히 검증하지 않겠다는 완화된 정책으로 보인다. */
	/* [한국어] 여기까지 도달했으면 모든 검증 통과 - 성공 디버그 로그. */
	ldm_debug ("Validated PRIVHEADs successfully.");
	/* [한국어] 최종 성공 표시. */
	result = true;
	/* [한국어] out: 정리 레이블 - 성공/실패 경로 모두 여기로 모여 백업 버퍼를 해제한다. */
out:
	/* [한국어] 백업 1 해제(kfree(NULL)도 안전하므로 할당되지 않았던 경우도 문제 없음). */
	kfree (ph[1]);
	/* [한국어] 백업 2 해제. */
	kfree (ph[2]);
	/* [한국어] 최종 결과(true/false)를 호출자에게 반환 - 성공이면 ph1(=ph[0])에 대표값이 남아 있다. */
	return result;
}

/*
 * [한국어]
 * ldm_validate_tocblocks() - TOCBLOCK 최대 4중 사본을 읽어 비교/검증
 *
 * @state: 파티션 스캔 상태(디바이스 컨텍스트)
 * @base:  LDM 설정 데이터베이스의 디스크 절대 시작 섹터(= privhead.config_start)
 * @ldb:   검증 결과를 채워 넣을 in-memory struct ldmdb (ldb->ph는 이미 채워져 있어야 함)
 * @return: true  - @ldb->toc에 검증된 TOCBLOCK 내용이 채워짐
 *          false - 유효한 TOCBLOCK을 하나도 찾지 못했거나, 범위 초과, 또는 사본 간 불일치
 *
 * TOCBLOCK은 최대 4중(OFF_TOCB1~4)으로 저장되지만, Windows Vista(LDM 2.12)는 4개를 모두 두지
 * 않는 경우가 있어(원문 주석) 이 함수는 읽기/파싱에 실패한 사본은 건너뛰고 성공한 사본만
 * 모아(nr_tbs) 최소 1개 이상이면 통과시키는 완화된 정책을 쓴다. tb[1..3]은 하나의
 * kmalloc_objs(*tb[1], 3) 호출로 연속 배열처럼 할당한 뒤 포인터 산술로 tb[2]/tb[3]을 파생시킨다.
 * 성공적으로 읽은 사본이 있으면 (1) 첫 사본(tb[0], 곧 ldb->toc)의 비트맵 범위가
 * ldb->ph.config_size를 넘지 않는지 검사, (2) 나머지 성공한 사본들을 ldm_compare_tocblocks()로
 * tb[0]과 비교하는 순서로 진행한다.
 *
 * 호출 체인:
 *   ldm_partition -> [ldm_validate_tocblocks] -> ldm_parse_tocblock / ldm_compare_tocblocks
 */
/**
 * ldm_validate_tocblocks - Validate the table of contents and its backups
 * @state: Partition check state including device holding the LDM Database
 * @base:  Offset, into @state->disk, of the database
 * @ldb:   Cache of the database structures
 *
 * Find and compare the four tables of contents of the LDM Database stored on
 * @state->disk and return the parsed information into @toc1.
 *
 * The offsets and sizes of the configs are range-checked against a privhead.
 *
 * Return:  'true'   @toc1 contains validated TOCBLOCK info
 *          'false'  @toc1 contents are undefined
 */
/* [한국어] state: 디바이스 컨텍스트, base: 설정 DB 시작 절대 섹터, ldb: 결과를 담을 캐시. */
static bool ldm_validate_tocblocks(struct parsed_partitions *state,
				   unsigned long base, struct ldmdb *ldb)
{
	/* [한국어] 네 TOCBLOCK 사본의 base 기준 상대 오프셋 테이블. */
	static const int off[4] = { OFF_TOCB1, OFF_TOCB2, OFF_TOCB3, OFF_TOCB4};
	/* [한국어] 최대 4개의 tocblock 포인터 - 성공적으로 읽은 것만 앞쪽부터 채워진다. */
	struct tocblock *tb[4];
	/* [한국어] ldb->ph를 가리키는 포인터 - 이미 검증된 PRIVHEAD로 범위 검사 기준값을 얻는다. */
	struct privhead *ph;
	/* [한국어] read_part_sector() 핸들. */
	Sector sect;
	/* [한국어] 읽은 섹터 데이터 포인터. */
	u8 *data;
	/* [한국어] i: 루프 인덱스, nr_tbs: 지금까지 성공적으로 파싱한 사본 개수. */
	int i, nr_tbs;
	/* [한국어] 최종 반환값, 기본 false. */
	bool result = false;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON(!state || !ldb);
	/* [한국어] ph = &ldb->ph - 이미 ldm_validate_privheads()가 채워둔 값. */
	ph = &ldb->ph;
	/* [한국어] tb[0]은 ldb->toc 자체를 직접 가리킨다(별도 할당 없이 최종 저장 위치를 바로 사용) - 성공하면 이 값이 곧 최종 결과가 된다. */
	tb[0] = &ldb->toc;
	/* [한국어] tb[1..3] 세 개를 위한 공간을 kmalloc_objs(*tb[1], 3)으로 한 번에(연속 메모리로) 할당. */
	tb[1] = kmalloc_objs(*tb[1], 3);
	/* [한국어] 할당 실패 시. */
	if (!tb[1]) {
		/* [한국어] 메모리 부족 치명적 로그. */
		ldm_crit("Out of memory.");
		/* [한국어] 실패 정리 경로(err)로 점프. */
		goto err;
	}
	/* [한국어] tb[2]를 tb[1] 바로 다음 struct tocblock 크기만큼 뒤로 포인터 산술로 계산 - 3개짜리 배열의 두 번째 원소를 가리키게 됨. */
	tb[2] = (struct tocblock*)((u8*)tb[1] + sizeof(*tb[1]));
	/* [한국어] tb[3]도 같은 방식으로 tb[2] 다음 위치를 계산 - 결과적으로 tb[1..3]은 kmalloc_objs()가 할당한 연속 배열의 개별 원소를 가리키는 별칭이 된다. */
	tb[3] = (struct tocblock*)((u8*)tb[2] + sizeof(*tb[2]));
	/*
	 * Try to read and parse all four TOCBLOCKs.
	 *
	 * Windows Vista LDM v2.12 does not always have all four TOCBLOCKs so
	 * skip any that fail as long as we get at least one valid TOCBLOCK.
	 */
	/* [한국어] 네 사본을 순서대로 시도하되, 실패한 것은 건너뛰고 성공한 것만 앞쪽 tb[]
	 * 슬롯(nr_tbs가 가리키는 위치)부터 채워 넣는다 - i는 오프셋 테이블 인덱스, nr_tbs는
	 * 성공 개수이자 다음에 채울 tb[] 슬롯 인덱스. */
	for (nr_tbs = i = 0; i < 4; i++) {
		/* [한국어] base(설정 DB 시작 절대 섹터) + off[i]의 섹터를 읽음. */
		data = read_part_sector(state, base + off[i], &sect);
		/* [한국어] 디스크 읽기 자체가 실패한 경우. */
		if (!data) {
			/* [한국어] 이 사본만 실패했다는 에러 로그를 남기되(ldm_crit이 아니라 ldm_error로 격을 낮춤 - 아직 다른 사본으로 복구 가능하므로 치명적이지 않음). */
			ldm_error("Disk read failed for TOCBLOCK %d.", i);
			/* [한국어] 이 사본만 건너뛰고 다음 사본 시도 - break가 아니라 continue인 점이 ldm_validate_privheads()의 정책과 다르다(사본 하나 실패가 전체를 막지 않음). */
			continue;
		}
		/* [한국어] 파싱에 성공하면(tb[nr_tbs] 슬롯에 채움) nr_tbs를 하나 증가시켜 다음 성공 사본이 채워질 슬롯을 전진시킨다 - 파싱 실패 시에는 nr_tbs를 늘리지 않아 실패한 사본은 조용히 무시된다. */
		if (ldm_parse_tocblock(data, tb[nr_tbs]))
			/* [한국어] 성공 카운트 증가. */
			nr_tbs++;
		/* [한국어] 이번 섹터의 folio 참조 반납. */
		put_dev_sector(sect);
	}
	/* [한국어] 네 사본 모두 읽기/파싱에 실패해 성공한 사본이 하나도 없는 경우. */
	if (!nr_tbs) {
		/* [한국어] 유효한 TOCBLOCK을 전혀 찾지 못했다는 치명적 로그. */
		ldm_crit("Failed to find a valid TOCBLOCK.");
		/* [한국어] 실패 정리 경로로 점프. */
		goto err;
	}
	/* Range check the TOCBLOCK against a privhead. */
	/* [한국어] 첫 성공 사본(tb[0], 곧 ldb->toc)의 두 비트맵 범위가 PRIVHEAD의 config_size를
	 * 넘지 않는지 검사 - 이 시점에야 비로소 ldm_parse_tocblock()이 미루었던 range-check를
	 * 수행한다. */
	if (((tb[0]->bitmap1_start + tb[0]->bitmap1_size) > ph->config_size) ||
			((tb[0]->bitmap2_start + tb[0]->bitmap2_size) >
			ph->config_size)) {
		/* [한국어] 범위 초과 치명적 로그. */
		ldm_crit("The bitmaps are out of range.  Giving up.");
		/* [한국어] 실패 정리 경로로 점프. */
		goto err;
	}
	/* [한국어] 두 번째로 성공한 사본부터(i=1) tb[0]과 하나씩 비교. */
	/* Compare all loaded TOCBLOCKs. */
	/* [한국어] 성공한 사본 개수(nr_tbs)만큼만 비교 - 4개 중 성공 못한 것은 애초에 tb[]에 채워지지 않았으므로 비교 대상에서 자연스럽게 제외된다. */
	for (i = 1; i < nr_tbs; i++) {
		/* [한국어] ldm_compare_tocblocks()로 tb[0]과 tb[i] 비교. */
		if (!ldm_compare_tocblocks(tb[0], tb[i])) {
			/* [한국어] 불일치 발견 - 몇 번째 사본이 다른지 로그로 남김. */
			ldm_crit("TOCBLOCKs 0 and %d do not match.", i);
			/* [한국어] 실패 정리 경로로 점프 - 사본 중 하나라도 다르면 신뢰할 수 없다고 판단. */
			goto err;
		}
	}
	/* [한국어] 성공한 사본 개수와 함께 검증 성공 디버그 로그. */
	ldm_debug("Validated %d TOCBLOCKs successfully.", nr_tbs);
	/* [한국어] 최종 성공 표시. */
	result = true;
	/* [한국어] err: 정리 레이블 - 성공/실패 모두 여기로 모여 tb[1] 할당 블록을 해제한다
	 * (tb[2]/tb[3]은 포인터 산술로 파생된 별칭이므로 별도 해제 불필요, tb[0]은 ldb->toc라서
	 * 애초에 별도 할당물이 아님). */
err:
	/* [한국어] tb[1..3]을 담고 있던 연속 할당 블록 해제. */
	kfree(tb[1]);
	/* [한국어] 최종 결과 반환. */
	return result;
}

/*
 * [한국어]
 * ldm_validate_vmdb() - VMDB를 읽어 파싱하고 트랜잭션/크기 정합성까지 검증
 *
 * @state: 파티션 스캔 상태(디바이스 컨텍스트)
 * @base:  LDM 설정 데이터베이스의 디스크 절대 시작 섹터
 * @ldb:   검증 결과를 채워 넣을 in-memory struct ldmdb (ldb->toc는 이미 채워져 있어야 함)
 * @return: true  - @ldb->vm에 검증된 VMDB 정보가 채워짐
 *          false - 디스크 읽기 실패, 파싱 실패, 미완료 트랜잭션, 또는 크기 범위 초과
 *
 * VMDB는 TOCBLOCK과 달리 백업 사본이 없으므로(base+OFF_VMDB 위치에 단 1부), 이 함수는
 * ldm_parse_tocblock()류처럼 여러 사본을 비교하는 대신 단일 사본을 읽어 세 가지를 추가로
 * 확인한다: (1) ldm_parse_vmdb()로 기본 파싱, (2) data+0x10 위치의 커밋 플래그가 0x01(커밋 완료)
 * 인지 확인해 미완료 트랜잭션이 있는 상태의 데이터베이스를 거부, (3) vblk_offset이 표준값
 * 512와 다르면 정보성 로그만 남기고 계속 진행, (4) VBLK 테이블 전체 크기(vblk_size *
 * last_vblk_seq)가 TOCBLOCK 첫 비트맵 크기(섹터 -> 바이트로 <<9 변환)를 넘지 않는지 range-check.
 *
 * 호출 체인:
 *   ldm_partition -> [ldm_validate_vmdb] -> ldm_parse_vmdb
 */
/**
 * ldm_validate_vmdb - Read the VMDB and validate it
 * @state: Partition check state including device holding the LDM Database
 * @base:  Offset, into @bdev, of the database
 * @ldb:   Cache of the database structures
 *
 * Find the vmdb of the LDM Database stored on @bdev and return the parsed
 * information in @ldb.
 *
 * Return:  'true'   @ldb contains validated VBDB info
 *          'false'  @ldb contents are undefined
 */
/* [한국어] state: 디바이스 컨텍스트, base: 설정 DB 시작 절대 섹터, ldb: 결과를 담을 캐시. */
static bool ldm_validate_vmdb(struct parsed_partitions *state,
			      unsigned long base, struct ldmdb *ldb)
{
	/* [한국어] read_part_sector() 핸들. */
	Sector sect;
	/* [한국어] 읽은 섹터 데이터 포인터. */
	u8 *data;
	/* [한국어] 최종 반환값, 기본 false. */
	bool result = false;
	/* [한국어] ldb->vm을 가리키는 지역 별칭 - 이후 코드 가독성을 위한 포인터. */
	struct vmdb *vm;
	/* [한국어] ldb->toc를 가리키는 지역 별칭 - TOCBLOCK 크기 범위 검사에 사용. */
	struct tocblock *toc;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!state || !ldb);

	/* [한국어] vm = &ldb->vm. */
	vm  = &ldb->vm;
	/* [한국어] toc = &ldb->toc(이미 ldm_validate_tocblocks()가 채워둔 값). */
	toc = &ldb->toc;

	/* [한국어] base + OFF_VMDB(=17) 위치의 섹터를 읽음 - VMDB는 백업이 없어 단일 사본만 읽으면 된다. */
	data = read_part_sector(state, base + OFF_VMDB, &sect);
	/* [한국어] 디스크 읽기 실패. */
	if (!data) {
		/* [한국어] 치명적 오류 로그. */
		ldm_crit ("Disk read failed.");
		/* [한국어] 이 경로는 out 레이블을 거치지 않고 바로 반환하는데, 아직 sect가 유효한 섹터를 담고 있지 않으므로(data가 NULL) put_dev_sector() 호출이 불필요/위험하기 때문이다. */
		return false;
	}

	/* [한국어] ldm_parse_vmdb()로 매직/버전/vblk_size 기본 검증. */
	if (!ldm_parse_vmdb (data, vm))
		/* [한국어] 실패 시 out으로 점프(원인은 ldm_parse_vmdb() 내부에서 이미 로그됨) - 이 시점부터는 sect가 유효하므로 out에서 put_dev_sector()가 필요하다. */
		goto out;				/* Already logged */

	/* [한국어] data+0x10의 빅엔디안 16비트 값이 0x01이 아니면 커밋되지 않은(미완료) 트랜잭션이 남아 있다는 뜻 - Windows가 쓰기 도중 중단된 상태일 수 있어 신뢰할 수 없다. */
	/* Are there uncommitted transactions? */
	/* [한국어] 이 값이 0x01(커밋 완료 상태)이 아니면 조건 성립 - 미완료 트랜잭션 분기로 진입. */
	if (get_unaligned_be16(data + 0x10) != 0x01) {
		/* [한국어] 비일관 상태 치명적 로그. */
		ldm_crit ("Database is not in a consistent state.  Aborting.");
		/* [한국어] out으로 점프. */
		goto out;
	}

	/* [한국어] VBLK 테이블이 표준 오프셋(512, 곧 다음 섹터)이 아닌 곳에서 시작하면 이례적인 상황이므로 정보성 로그만 남기고 계속 진행(치명적 오류로 취급하지 않음). */
	if (vm->vblk_offset != 512)
		/* [한국어] 실제 오프셋 값을 16진수로 로그에 남김. */
		ldm_info ("VBLKs start at offset 0x%04x.", vm->vblk_offset);

	/*
	 * The last_vblkd_seq can be before the end of the vmdb, just make sure
	 * it is not out of bounds.
	 */
	/* [한국어] VBLK 테이블 전체 바이트 수(레코드 크기 x 마지막 시퀀스 번호)가 TOCBLOCK 첫 비트맵 크기(섹터 단위이므로 <<9로 바이트 환산)를 넘으면 손상된 것으로 간주. */
	if ((vm->vblk_size * vm->last_vblk_seq) > (toc->bitmap1_size << 9)) {
		/* [한국어] 크기 초과 치명적 로그. */
		ldm_crit ("VMDB exceeds allowed size specified by TOCBLOCK.  "
				"Database is corrupt.  Aborting.");
		/* [한국어] out으로 점프. */
		goto out;
	}

	/* [한국어] 여기까지 도달했으면 모든 검증 통과. */
	result = true;
	/* [한국어] out: 정리 레이블 - 성공/실패 모두 여기로 모여 섹터 참조를 반납한다(단, 이 함수 앞부분의 최초 읽기 실패 경로만은 이 레이블을 거치지 않고 직접 반환한다는 점에 유의). */
out:
	/* [한국어] 이번에 읽은 섹터의 folio 참조 반납. */
	put_dev_sector (sect);
	/* [한국어] 최종 결과 반환. */
	return result;
}


/*
 * [한국어]
 * ldm_validate_partition_table() - MBR을 훑어 "동적 디스크일 가능성"을 약식으로 판별
 *
 * @state: 파티션 스캔 상태(디바이스 컨텍스트)
 * @return: true  - MBR 안에 타입 0x42(LDM_PARTITION) 파티션 엔트리가 하나라도 있음
 *          false - 그런 엔트리가 없거나, 유효한 MBR 시그니처가 아니거나, 섹터 읽기 실패
 *
 * 원문 주석대로 이 함수는 "약한(weak) 테스트"다 - 디스크 섹터 0을 읽어 부트 시그니처
 * (0x1FE 위치의 0xAA55, MSDOS_LABEL_MAGIC)를 확인하고, 유효하면 오프셋 0x1BE의 4개 MBR
 * 파티션 엔트리를 struct msdos_partition 배열로 캐스팅해 sys_ind가 LDM_PARTITION(0x42)인
 * 것이 있는지만 본다. 이 검사를 통과했다고 해서 실제로 유효한 LDM 데이터베이스가 있다는 보장은
 * 없으며(이후 ldm_validate_privheads() 등이 진짜 검증을 수행), 반대로 이 검사가 실패(false)해도
 * 반드시 디스크 읽기 오류를 의미하지는 않는다(원문 주석: "그런 경우 0을 반환해 다른 파티션
 * 스킴이 시도하도록 둔다"). 즉 ldm_partition()이 이 함수 호출 이후 사용하는 반환값 구분은
 * true(동적 디스크 가능성 있음)/false(아니거나 판단 불가) 둘뿐이며, 디스크 읽기 실패와 "MBR은
 * 있으나 0x42가 없음"을 이 함수 내부에서는 구분하지 않는다.
 *
 * 호출 체인:
 *   ldm_partition -> [ldm_validate_partition_table]
 */
/**
 * ldm_validate_partition_table - Determine whether bdev might be a dynamic disk
 * @state: Partition check state including device holding the LDM Database
 *
 * This function provides a weak test to decide whether the device is a dynamic
 * disk or not.  It looks for an MS-DOS-style partition table containing at
 * least one partition of type 0x42 (formerly SFS, now used by Windows for
 * dynamic disks).
 *
 * N.B.  The only possible error can come from the read_part_sector and that is
 *       only likely to happen if the underlying device is strange.  If that IS
 *       the case we should return zero to let someone else try.
 *
 * Return:  'true'   @state->disk is a dynamic disk
 *          'false'  @state->disk is not a dynamic disk, or an error occurred
 */
/* [한국어] state: 디바이스 컨텍스트. */
static bool ldm_validate_partition_table(struct parsed_partitions *state)
{
	/* [한국어] read_part_sector() 핸들. */
	Sector sect;
	/* [한국어] 읽은 섹터(디스크 0번, 즉 MBR) 데이터 포인터. */
	u8 *data;
	/* [한국어] MBR 파티션 엔트리 4개를 순회할 포인터. */
	struct msdos_partition *p;
	/* [한국어] 루프 인덱스. */
	int i;
	/* [한국어] 최종 반환값, 기본 false(0x42 파티션을 찾지 못한 것으로 시작). */
	bool result = false;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON(!state);

	/* [한국어] 디스크 절대 섹터 0(MBR)을 읽음. */
	data = read_part_sector(state, 0, &sect);
	/* [한국어] 디스크 읽기 실패. */
	if (!data) {
		/* [한국어] 원문 주석대로 이 실패는 흔치 않은 상황(이상한 디바이스)일 때만 발생한다고 보아 ldm_info() 수준으로 로그(치명적 취급 안 함). */
		ldm_info ("Disk read failed.");
		/* [한국어] false 반환 - 다른 파티션 스킴이 이어서 시도할 수 있도록 함. */
		return false;
	}

	/* [한국어] 부트 섹터 시그니처(오프셋 0x1FE의 리틀엔디안 16비트 값)가 0xAA55 (MSDOS_LABEL_MAGIC)와 다르면 유효한 MBR 자체가 아니다. */
	if (*(__le16*) (data + 0x01FE) != cpu_to_le16 (MSDOS_LABEL_MAGIC))
		/* [한국어] MBR이 아니므로 더 볼 것 없이 정리 경로로 - result는 false 그대로. */
		goto out;

	/* [한국어] MBR 파티션 엔트리 테이블 시작(오프셋 0x1BE)을 struct msdos_partition* 로 캐스팅. */
	p = (struct msdos_partition *)(data + 0x01BE);
	/* [한국어] 4개의 고정 크기(16바이트) 엔트리를 순회(p++로 다음 엔트리로 전진). */
	for (i = 0; i < 4; i++, p++)
		/* [한국어] 이 엔트리의 파티션 타입(sys_ind)이 LDM_PARTITION(0x42)인지 확인. */
		if (p->sys_ind == LDM_PARTITION) {
			/* [한국어] 찾았으므로 true로 전환. */
			result = true;
			/* [한국어] 첫 번째로 발견된 엔트리만으로 충분 - 나머지는 볼 필요 없이 루프 종료. */
			break;
		}

	/* [한국어] 하나라도 찾았으면. */
	if (result)
		/* [한국어] Windows 2000 계열 동적 디스크 파티션 타입을 찾았다는 디버그 로그. */
		ldm_debug ("Found W2K dynamic disk partition type.");

	/* [한국어] out: 정리 레이블 - 성공/실패 모두 여기로 모여 섹터 참조를 반납한다. */
out:
	/* [한국어] 섹터 folio 참조 반납. */
	put_dev_sector (sect);
	/* [한국어] 최종 결과 반환. */
	return result;
}

/*
 * [한국어]
 * ldm_get_disk_objid() - "지금 이 물리 디스크"에 해당하는 Disk VBLK를 v_disk 리스트에서 검색
 *
 * @ldb: 파싱이 끝난(v_disk 리스트가 채워진) in-memory 데이터베이스 캐시
 * @return: 일치하는 struct vblk* - ldb->ph.disk_id(현재 디스크의 PRIVHEAD GUID)와 GUID가
 *          같은 Disk VBLK를 찾은 경우
 *          NULL - 일치하는 항목이 없는 경우
 *
 * LDM 데이터베이스는 디스크 그룹에 속한 "모든" 디스크의 정보를 담고 있으므로(원문 주석), 이
 * 함수가 v_disk 리스트를 list_for_each()로 순회하며 각 Disk VBLK의 GUID(vblk.disk.disk_id)를
 * PRIVHEAD의 disk_id(ldb->ph.disk_id, 곧 지금 이 gendisk의 물리 디스크 GUID)와 uuid_equal()로
 * 비교해, 일치하는 첫 항목을 찾으면 즉시 반환한다. 이 반환값(struct vblk*)의 obj_id 필드가
 * ldm_create_data_partitions()에서 "이 디스크에 속한 파티션"을 걸러내는 필터 키로 재사용된다.
 *
 * 호출 체인:
 *   ldm_create_data_partitions -> [ldm_get_disk_objid]
 */
/**
 * ldm_get_disk_objid - Search a linked list of vblk's for a given Disk Id
 * @ldb:  Cache of the database structures
 *
 * The LDM Database contains a list of all partitions on all dynamic disks.
 * The primary PRIVHEAD, at the beginning of the physical disk, tells us
 * the GUID of this disk.  This function searches for the GUID in a linked
 * list of vblk's.
 *
 * Return:  Pointer, A matching vblk was found
 *          NULL,    No match, or an error
 */
/* [한국어] ldb: v_disk 리스트가 이미 채워진 데이터베이스 캐시. */
static struct vblk * ldm_get_disk_objid (const struct ldmdb *ldb)
{
	/* [한국어] list_for_each() 순회에 쓰이는 지역 list_head 포인터(반복자). */
	struct list_head *item;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!ldb);

	/* [한국어] ldb->v_disk 리스트(Disk VBLK 전체)를 처음부터 끝까지 순회. */
	list_for_each (item, &ldb->v_disk) {
		/* [한국어] list_head 포인터(item)로부터 container_of 패턴(list_entry)을 이용해 감싸는 struct vblk 전체를 얻음. */
		struct vblk *v = list_entry (item, struct vblk, list);
		/* [한국어] 이 Disk VBLK의 GUID(v->vblk.disk.disk_id)가 현재 디스크의 PRIVHEAD GUID (ldb->ph.disk_id)와 같은지 uuid_equal()로 비교 - 이것이 "이 gendisk가 데이터베이스 안의 어느 디스크 레코드에 해당하는가"를 결정하는 유일한 지점. */
		if (uuid_equal(&v->vblk.disk.disk_id, &ldb->ph.disk_id))
			/* [한국어] 일치하는 즉시 그 vblk를 반환(더 순회할 필요 없음). */
			return v;
	}

	/* [한국어] 리스트 전체를 순회했지만 일치하는 항목을 찾지 못함 - 이 디스크에 해당하는 정보가 데이터베이스에 없다는 뜻(비정상 상황). */
	return NULL;
}

/*
 * [한국어]
 * ldm_create_data_partitions() - 현재 디스크에 속한 데이터 파티션들을 커널에 등록
 *
 * @pp:  지금까지 파싱된 파티션들을 담을 parsed_partitions 상태(등록 대상)
 * @ldb: 파싱이 끝난 in-memory 데이터베이스 캐시(v_disk/v_part가 채워져 있어야 함)
 * @return: true  - 파티션 등록(또는 등록 시도) 완료
 *          false - 이 디스크에 해당하는 Disk VBLK를 찾지 못함(치명적 오류)
 *
 * 데이터베이스는 디스크 그룹의 "모든" 디스크에 속한 "모든" 파티션을 담고 있으므로(원문 주석),
 * 이 함수는 먼저 ldm_get_disk_objid()로 현재 디스크의 obj_id를 얻은 뒤, ldb->v_part 리스트
 * (ldm_ldmdb_add()가 이미 시작 섹터 순으로 정렬해 둔 리스트)를 순회하며 disk_id가 일치하는
 * 파티션만 골라 put_partition()으로 등록한다. 등록되는 절대 시작 LBA는 항상
 * "ldb->ph.logical_disk_start + part->start"로 계산되며, 이는 PRIVHEAD가 정의한 논리 디스크
 * 영역의 시작을 기준으로 한 상대 위치를 절대 위치로 변환하는 것이다. 원문 주석대로 파티션은
 * 데이터베이스에 나타난(=시작 섹터 정렬된) 순서 그대로 만들어진다.
 *
 * 호출 체인:
 *   ldm_partition -> [ldm_create_data_partitions] -> ldm_get_disk_objid / put_partition
 */
/**
 * ldm_create_data_partitions - Create data partitions for this device
 * @pp:   List of the partitions parsed so far
 * @ldb:  Cache of the database structures
 *
 * The database contains ALL the partitions for ALL disk groups, so we need to
 * filter out this specific disk. Using the disk's object id, we can find all
 * the partitions in the database that belong to this disk.
 *
 * Add each partition in our database, to the parsed_partitions structure.
 *
 * N.B.  This function creates the partitions in the order it finds partition
 *       objects in the linked list.
 *
 * Return:  'true'   Partition created
 *          'false'  Error, probably a range checking problem
 */
/* [한국어] pp: 등록 대상 parsed_partitions, ldb: 파싱 완료된 데이터베이스 캐시. */
static bool ldm_create_data_partitions (struct parsed_partitions *pp,
					const struct ldmdb *ldb)
{
	/* [한국어] list_for_each() 반복자. */
	struct list_head *item;
	/* [한국어] v_part 리스트를 순회하며 얻는 struct vblk* 임시 변수. */
	struct vblk *vb;
	/* [한국어] ldm_get_disk_objid()가 찾아준, 현재 디스크에 해당하는 Disk VBLK. */
	struct vblk *disk;
	/* [한국어] vb->vblk.part로 접근할 때 쓰는 지역 별칭 포인터. */
	struct vblk_part *part;
	/* [한국어] 리눅스가 매길 다음 파티션 번호(1부터 시작) - put_partition()의 슬롯 인덱스로 쓰인다. */
	int part_num = 1;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!pp || !ldb);

	/* [한국어] 현재 디스크(gendisk)에 해당하는 Disk VBLK를 데이터베이스에서 검색. */
	disk = ldm_get_disk_objid (ldb);
	/* [한국어] 찾지 못한 경우. */
	if (!disk) {
		/* [한국어] 이 물리 디스크의 ID를 데이터베이스 안에서 찾을 수 없다는 치명적 오류 - PRIVHEAD와 v_disk 리스트가 서로 다른 디스크 그룹을 가리키는 등 심각한 불일치를 의미. */
		ldm_crit ("Can't find the ID of this disk in the database.");
		/* [한국어] false 반환 - 이 함수는 실패를 알리는 유일한 경로. */
		return false;
	}

	/* [한국어] 진단 로그 문자열에 " [LDM]" 표시를 덧붙여, 이 디스크가 LDM 동적 디스크로 인식됐음을 사용자에게 보여준다(다른 파티션 스킴들의 로그 형식과 통일된 방식). */
	seq_buf_puts(&pp->pp_buf, " [LDM]");

	/* [한국어] 데이터 파티션들을 생성하는 본 루프 - v_part는 disk_id별로 시작 섹터 오름차순 정렬돼 있으므로(ldm_ldmdb_add() 참고) 자연스럽게 순서대로 등록된다. */
	/* Create the data partitions */
	/* [한국어] ldb->v_part(전체 디스크 그룹의 모든 파티션) 전체를 순회. */
	list_for_each (item, &ldb->v_part) {
		/* [한국어] list_head로부터 감싸는 struct vblk 전체를 얻음. */
		vb = list_entry (item, struct vblk, list);
		/* [한국어] union vblk의 part 멤버(vb->type이 VBLK_PRT3이므로 이 멤버가 유효)로 접근. */
		part = &vb->vblk.part;

		/* [한국어] 이 파티션의 disk_id(소유 디스크를 가리키는 obj_id 참조)가 현재 디스크의 obj_id와 다르면, 다른 디스크(또는 다른 디스크 그룹 멤버)에 속한 파티션이므로 건너뛴다 - 이 필터링이 "데이터베이스 전체 파티션" 중 "이 물리 디스크의 파티션"만 골라내는 핵심 로직이다. */
		if (part->disk_id != disk->obj_id)
			/* [한국어] 다음 파티션으로. */
			continue;

		/* [한국어] 일치하는 파티션을 실제로 등록 - 절대 시작 LBA는 논리 디스크 시작(ph.logical_disk_start) + 이 파티션의 상대 시작(part->start), 크기는 part->size 그대로. */
		put_partition (pp, part_num, ldb->ph.logical_disk_start +
				/* [한국어] put_partition()의 나머지 인자(줄바꿈된 계속) - 슬롯 번호 part_num, 이어서 size. */
				part->start, part->size);
		/* [한국어] 다음에 등록할 파티션 번호로 전진. */
		part_num++;
	}

	/* [한국어] 루프 종료 후, 로그 문자열에 개행을 추가해 이 디스크에 대한 진단 한 줄을 마무리한다. */
	seq_buf_puts(&pp->pp_buf, "\n");
	/* [한국어] 성공 반환 - 파티션이 하나도 없어도(모두 다른 디스크에 속했어도) 이 디스크 자체를 처리하는 과정 자체는 성공으로 간주된다. */
	return true;
}


/*
 * [한국어]
 * ldm_relative() - 가변폭 필드 뒤에 오는 다음 필드의 상대 오프셋을 계산
 *
 * @buffer: 파싱 중인 VBLK 원시 바이트 블록
 * @buflen: @buffer의 전체 크기(바이트) - 범위 검사 기준
 * @base:   지금까지의 고정폭 필드들의 누적 크기(오프셋)
 * @offset: 바로 이전 가변폭 필드(들)의 누적 크기 - 최초 호출 시 0
 * @return: -1 오류(계산된 오프셋이 buflen을 넘거나 base/offset이 잘못됨)
 *          n  base+offset 위치를 기준으로 다음 가변폭 필드가 시작하는 상대 오프셋
 *
 * LDM의 VBLK 레코드는 이름/GUID 등 여러 필드가 "1바이트 길이 + 실제 데이터"로 인코딩된
 * 가변폭(variable-width) 필드로 이어진다(원문 주석: "많은 VBLK 필드가 가변폭이라 이전 오프셋과
 * 그 필드 길이를 바탕으로 각 오프셋을 계산해야 한다"). 이 함수는 base+offset 위치(직전 가변폭
 * 필드가 끝난 지점)에서 그 필드의 길이 바이트(buffer[base])를 읽어, "그 필드 길이 + offset + 1"
 * (길이 바이트 자신의 1바이트를 더함)을 다음 필드의 상대 오프셋으로 반환한다. 즉 이 함수를 연쇄
 * 호출하면(각 호출의 offset 인자로 이전 호출의 반환값을 넘기면) VBLK 레코드 안의 가변폭 필드들을
 * 순서대로 훑어나갈 수 있다. buffer가 NULL이거나 offset이 음수이거나 base가 buflen을 넘으면,
 * 그리고 계산된 다음 오프셋이 buflen을 넘으면 모두 -1을 반환해 호출자가 파싱을 중단하게 한다.
 *
 * 호출 체인:
 *   ldm_parse_{cmp3,dgr3,dgr4,dsk3,dsk4,prt3,vol5}/ldm_parse_vblk -> [ldm_relative]
 */
/**
 * ldm_relative - Calculate the next relative offset
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @base:    Size of the previous fixed width fields
 * @offset:  Cumulative size of the previous variable-width fields
 *
 * Because many of the VBLK fields are variable-width, it's necessary
 * to calculate each offset based on the previous one and the length
 * of the field it pointed to.
 *
 * Return:  -1 Error, the calculated offset exceeded the size of the buffer
 *           n OK, a range-checked offset into buffer
 */
/* [한국어] buffer/buflen: 파싱 대상과 그 크기, base: 고정폭 필드 누적 크기, offset: 직전 가변폭 필드까지의 누적 오프셋. */
static int ldm_relative(const u8 *buffer, int buflen, int base, int offset)
{

	/* [한국어] base에 offset을 더해, "직전 가변폭 필드가 끝난 실제 위치"를 base 자체에 반영 - 이후 코드는 이 갱신된 base만 사용한다. */
	base += offset;
	/* [한국어] 세 가지 기본 오류 조건을 한 번에 검사: buffer가 NULL이거나, offset이 음수이거나(호출자 실수), base(갱신된, 즉 base+offset)가 buflen을 넘으면 이 위치를 읽는 것 자체가 범위를 벗어난다. */
	if (!buffer || offset < 0 || base > buflen) {
		/* [한국어] 어느 조건이 원인인지 구분해서 로그를 남기기 위해 각각 재검사. */
		if (!buffer)
			/* [한국어] buffer 자체가 NULL인 경우. */
			ldm_error("!buffer");
		/* [한국어] offset이 음수인 경우(직전 ldm_relative() 호출이 이미 실패해 -1을 반환했는데 그 값을 그대로 다음 호출에 넘긴 경우일 수 있음 - 오류 전파 패턴). */
		if (offset < 0)
			/* [한국어] 실제 offset 값을 로그에 남김. */
			ldm_error("offset (%d) < 0", offset);
		/* [한국어] base가 buflen을 넘는 경우. */
		if (base > buflen)
			/* [한국어] base와 buflen 값을 함께 로그에 남겨 어느 정도 초과했는지 알 수 있게 함. */
			ldm_error("base (%d) > buflen (%d)", base, buflen);
		/* [한국어] 세 조건 중 하나라도 해당하면 -1로 오류를 알림. */
		return -1;
	}
	/* [한국어] buffer[base]는 지금 위치에 있는 필드의 "길이 바이트" - 이 필드 자체가 buflen을 넘어서까지 뻗어나가는지 미리 검사(base + 길이가 buflen 이상이면 이 필드를 다 읽을 수 없다는 뜻). */
	if (base + buffer[base] >= buflen) {
		/* [한국어] 범위 초과 원인을 base/buffer[base]/buflen 세 값과 함께 로그로 남김. */
		ldm_error("base (%d) + buffer[base] (%d) >= buflen (%d)", base,
				buffer[base], buflen);
		/* [한국어] 오류로 -1 반환. */
		return -1;
	}
	/* [한국어] 정상 계산: 이 필드의 길이(buffer[base]) + 이전까지의 누적 offset + 길이 바이트 자신의 1바이트 = 다음 필드가 시작하는 상대 오프셋. */
	return buffer[base] + offset + 1;
}

/*
 * [한국어]
 * ldm_get_vnum() - 길이-접두 빅엔디안 가변폭 정수를 CPU 정수로 변환
 *
 * @block: 변환할 가변폭 숫자의 시작 위치(첫 바이트가 길이, 그 뒤로 길이만큼의 빅엔디안 바이트)
 * @return: 변환된 정수값 - 오류(길이가 0이거나 8 초과) 시 0을 반환
 *
 * LDM 데이터베이스의 큰 수치들은 "1바이트 길이 + 그 길이만큼의 빅엔디안 바이트" 형태로 압축
 * 저장된다(원문 주석). 이 함수는 첫 바이트를 길이로 읽은 뒤, 그 길이가 1~8 범위이면 바이트를
 * 하나씩 왼쪽으로 시프트하며 큰 정수(u64)로 누적한다. 원문 주석대로 이 함수는 범위 검사를 하지
 * 않으므로(N.B. 참고), 호출자가 미리 ldm_relative() 등으로 안전한 오프셋임을 확인해 둬야 한다 -
 * 최대 8바이트만 읽으므로 버퍼 오버런 자체는 발생하지 않지만, 그 8바이트가 실제로 유효한
 * 메모리인지는 호출자 책임이다.
 *
 * 호출 체인:
 *   ldm_parse_{cmp3,dgr3,prt3,vol5} 등 -> [ldm_get_vnum]
 */
/**
 * ldm_get_vnum - Convert a variable-width, big endian number, into cpu order
 * @block:  Pointer to the variable-width number to convert
 *
 * Large numbers in the LDM Database are often stored in a packed format.  Each
 * number is prefixed by a one byte width marker.  All numbers in the database
 * are stored in big-endian byte order.  This function reads one of these
 * numbers and returns the result
 *
 * N.B.  This function DOES NOT perform any range checking, though the most
 *       it will read is eight bytes.
 *
 * Return:  n A number
 *          0 Zero, or an error occurred
 */
/* [한국어] block: 길이 바이트로 시작하는 가변폭 숫자의 위치. */
static u64 ldm_get_vnum (const u8 *block)
{
	/* [한국어] 누적 결과를 담을 64비트 정수 - 0으로 초기화(오류 시 반환값이기도 함). */
	u64 tmp = 0;
	/* [한국어] 이 숫자의 바이트 길이(첫 바이트에서 읽음). */
	u8 length;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!block);

	/* [한국어] 첫 바이트를 길이로 읽고, block 포인터를 한 바이트 전진(후위 증가로 값을 먼저 쓰고 포인터를 이동). */
	length = *block++;

	/* [한국어] 길이가 0보다 크고(0이면 값 없음) 8 이하(u64 한 워드에 담을 수 있는 최대 바이트 수)인 정상 범위인지 확인. */
	if (length && length <= 8)
		/* [한국어] length를 감소시키며(0이 될 때까지) 반복 - length--는 후위 감소로 조건 평가 후 감소. */
		while (length--)
			/* [한국어] 누적값을 8비트 왼쪽 시프트한 뒤 다음 바이트를 OR로 붙임(빅엔디안이므로 가장 유의미한 바이트가 먼저 나옴) - block++로 매 반복 다음 바이트로 전진. */
			tmp = (tmp << 8) | *block++;
	/* [한국어] 길이가 0이거나 8을 넘는 비정상 값. */
	else
		/* [한국어] 잘못된 길이 값을 로그로 남김 - tmp는 초기값 0 그대로 반환된다. */
		ldm_error ("Illegal length %d.", length);

	/* [한국어] 정상적으로 누적된 값(또는 오류 시 0)을 반환. */
	return tmp;
}

/*
 * [한국어]
 * ldm_get_vstr() - 길이-접두 문자열을 NULL 종료 버퍼로 복사
 *
 * @block:  복사할 문자열의 시작 위치(첫 바이트가 길이, 그 뒤로 길이만큼의 문자 데이터)
 * @buffer: 복사해 넣을 출력 버퍼(호출자가 마련)
 * @buflen: @buffer의 전체 크기(NULL 종료 바이트를 포함해야 함)
 * @return: 0        오류(이 함수 자체는 0을 반환하는 명시적 오류 경로가 없어 실질적으로는
 *                    잘린(length==0) 경우에만 해당)
 *          n         복사된 문자열 길이(NULL 제외)
 *          buflen-1  버퍼가 작아 문자열이 잘린 경우
 *
 * LDM의 많은 문자열 필드는 NULL 종료가 아니라 1바이트 길이 접두사로 시작한다(원문 주석). 이
 * 함수는 그 길이(block[0])가 출력 버퍼(buflen)보다 크거나 같으면 버퍼 크기에 맞춰 잘라내고
 * (원문 N.B.: 입력 자체에 대한 범위 검사는 하지 않으므로 버퍼가 작으면 출력만 잘릴 뿐, block이
 * 가리키는 원본 메모리가 실제로 그만큼 유효한지는 호출자 책임이다), memcpy()로 문자 데이터를
 * 복사한 뒤 마지막에 명시적으로 0을 써서 NULL 종료 문자열로 만든다.
 *
 * 호출 체인:
 *   ldm_parse_{cmp3,dgr3,dgr4,dsk3,vol5}/ldm_parse_vblk -> [ldm_get_vstr]
 */
/**
 * ldm_get_vstr - Read a length-prefixed string into a buffer
 * @block:   Pointer to the length marker
 * @buffer:  Location to copy string to
 * @buflen:  Size of the output buffer
 *
 * Many of the strings in the LDM Database are not NULL terminated.  Instead
 * they are prefixed by a one byte length marker.  This function copies one of
 * these strings into a buffer.
 *
 * N.B.  This function DOES NOT perform any range checking on the input.
 *       If the buffer is too small, the output will be truncated.
 *
 * Return:  0, Error and @buffer contents are undefined
 *          n, String length in characters (excluding NULL)
 *          buflen-1, String was truncated.
 */
/* [한국어] block: 길이-접두 문자열 위치, buffer/buflen: 복사해 넣을 출력 버퍼와 그 크기. */
static int ldm_get_vstr (const u8 *block, u8 *buffer, int buflen)
{
	/* [한국어] 실제로 복사할 문자열 길이(범위 보정 후 최종값). */
	int length;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!block || !buffer);

	/* [한국어] block[0]이 이 문자열의 원래 길이(1바이트). */
	length = block[0];
	/* [한국어] 원래 길이가 출력 버퍼 크기 이상이면(NULL 종료 바이트를 넣을 자리가 없으면) 잘라야 한다. */
	if (length >= buflen) {
		/* [한국어] 잘림이 발생했다는 사실을 로그로 남김(원래 길이 -> 버퍼 크기). */
		ldm_error ("Truncating string %d -> %d.", length, buflen);
		/* [한국어] 실제 복사 길이를 buflen-1로 강제(마지막 한 바이트는 NULL 종료용으로 남김). */
		length = buflen - 1;
	}
	/* [한국어] block+1(길이 바이트 다음)부터 length 바이트를 buffer로 복사. */
	memcpy (buffer, block + 1, length);
	/* [한국어] 복사된 문자열 끝에 명시적으로 0을 써서 NULL 종료 문자열로 만듦. */
	buffer[length] = 0;
	/* [한국어] 실제로 복사된(널 제외) 길이를 반환. */
	return length;
}


/*
 * [한국어]
 * ldm_parse_cmp3() - Component VBLK(버전 3) 레코드를 struct vblk_comp로 파싱
 *
 * @buffer: VBLK 레코드 원시 바이트(공통 헤더 포함 전체)
 * @buflen: @buffer 크기
 * @vb:     공통 헤더가 이미 채워진 struct vblk (union vblk.comp에 결과를 채움)
 * @return: true  - @vb->vblk.comp에 유효한 Component 정보가 채워짐
 *          false - 가변폭 오프셋 계산 실패 또는 선언 길이 불일치
 *
 * Component VBLK는 볼륨을 구성하는 스트라이프/기본/RAID 구성 단위 하나를 표현한다. objid ->
 * name -> vstate(상태 문자열) -> child(자식 개수) -> parent(부모 Volume obj_id) 순서로
 * ldm_relative()를 연쇄 호출해 가변폭 필드 오프셋을 누적 계산하고, buffer[0x12]의
 * VBLK_FLAG_COMP_STRIPE 비트가 켜져 있으면 stripe/cols(컬럼 수) 필드까지 추가로 계산한다.
 * 마지막으로 계산된 누적 길이(len)에 VBLK_SIZE_CMP3(고정부 크기)를 더한 값이 레코드에 기록된
 * 선언 길이(buffer+0x14의 빅엔디안 32비트 값)와 정확히 일치(!=)해야 성공으로 처리한다. 이후
 * state/parent_id/type/children/chunksize를 실제로 읽어 struct vblk_comp에 채운다.
 *
 * 호출 체인:
 *   ldm_parse_vblk -> [ldm_parse_cmp3] (buf[0x13] == VBLK_CMP3 인 경우)
 */
/**
 * ldm_parse_cmp3 - Read a raw VBLK Component object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Component object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Component VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buffer/buflen: VBLK 레코드 원시 바이트와 크기, vb: 결과를 채울 struct vblk. */
static bool ldm_parse_cmp3 (const u8 *buffer, int buflen, struct vblk *vb)
{
	/* [한국어] r_*: ldm_relative()가 계산해 주는 각 가변폭 필드의 상대 오프셋, len: 최종 누적 길이(선언 길이와 비교할 값). */
	int r_objid, r_name, r_vstate, r_child, r_parent, r_stripe, r_cols, len;
	/* [한국어] vb->vblk.comp에 접근할 때 쓸 지역 별칭 포인터. */
	struct vblk_comp *comp;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!buffer || !vb);

	/* [한국어] 고정 헤더(0x18) 바로 뒤 objid 필드의 상대 오프셋(offset=0에서 시작). */
	r_objid  = ldm_relative (buffer, buflen, 0x18, 0);
	/* [한국어] objid 다음 name 필드의 오프셋(이전 결과 r_objid를 offset으로 연쇄 전달). */
	r_name   = ldm_relative (buffer, buflen, 0x18, r_objid);
	/* [한국어] name 다음 vstate(상태 문자열) 필드의 오프셋. */
	r_vstate = ldm_relative (buffer, buflen, 0x18, r_name);
	/* [한국어] vstate 다음 child(자식 개수) 필드 - base가 0x1D로 바뀐 것에 유의(고정폭 필드가 하나 더 끼어 있음을 의미). */
	r_child  = ldm_relative (buffer, buflen, 0x1D, r_vstate);
	/* [한국어] child 다음 parent(부모 Volume obj_id) 필드 - base가 다시 0x2D로 바뀜. */
	r_parent = ldm_relative (buffer, buflen, 0x2D, r_child);

	/* [한국어] buffer[0x12](플래그 바이트)의 COMP_STRIPE 비트로 스트라이프 전용 확장 필드 존재 여부 판단. */
	if (buffer[0x12] & VBLK_FLAG_COMP_STRIPE) {
		/* [한국어] 스트라이프 크기 필드의 오프셋(base가 0x2E로 바뀜). */
		r_stripe = ldm_relative (buffer, buflen, 0x2E, r_parent);
		/* [한국어] 컬럼(column) 수 필드의 오프셋. */
		r_cols   = ldm_relative (buffer, buflen, 0x2E, r_stripe);
		/* [한국어] 스트라이프가 있으면 컬럼 오프셋까지가 전체 가변폭 누적 길이. */
		len = r_cols;
	/* [한국어] 스트라이프 비트가 꺼져 있으면. */
	} else {
		/* [한국어] r_stripe를 0으로 - 이후 chunksize 계산에서 "스트라이프 없음"을 나타내는 센티널로 재사용된다. */
		r_stripe = 0;
		/* [한국어] 이 경우 parent 오프셋까지가 전체 누적 길이. */
		len = r_parent;
	}
	/* [한국어] 오프셋 계산 중 하나라도 ldm_relative()가 -1(오류)을 반환했으면 이후 계산은 무의미하므로 조기 실패. */
	if (len < 0)
		/* [한국어] 실패 반환(로그는 ldm_relative() 내부에서 이미 남겨짐). */
		return false;

	/* [한국어] 누적된 가변폭 길이에 Component v3 고정부 크기(VBLK_SIZE_CMP3)를 더해 전체 레코드 길이를 계산. */
	len += VBLK_SIZE_CMP3;
	/* [한국어] 레코드에 실제로 기록된 선언 길이(buffer+0x14, 빅엔디안 32비트)와 계산값이 다르면 파싱이 스펙과 어긋난 것으로 간주(다른 타입들과 마찬가지로 '!=' 비교, prt3/vol5만 예외적으로 '>'를 씀). */
	if (len != get_unaligned_be32(buffer + 0x14))
		/* [한국어] 불일치 시 실패 반환(별도 로그 없이 조용히 실패 - 다른 검증 실패 경로들과 달리 ldm_error() 호출이 없다는 점이 특이하다). */
		return false;

	/* [한국어] 여기부터 실제 필드 값을 읽어 vb->vblk.comp에 채우는 단계. */
	comp = &vb->vblk.comp;
	/* [한국어] name 오프셋 위치의 길이-접두 문자열을 comp->state(상태 이름)로 복사. */
	ldm_get_vstr (buffer + 0x18 + r_name, comp->state,
		/* [한국어] ldm_get_vstr()의 나머지 인자(줄바꿈된 계속) - 출력 버퍼 크기. */
		sizeof (comp->state));
	/* [한국어] vstate 오프셋 위치의 1바이트를 컴포넌트 타입(COMP_STRIPE/BASIC/RAID)으로 저장. */
	comp->type      = buffer[0x18 + r_vstate];
	/* [한국어] child 오프셋 위치의 가변폭 정수를 자식 개수로 저장(u8 필드에 저장되므로 8비트를 넘는 값은 잘릴 수 있음에 유의). */
	comp->children  = ldm_get_vnum (buffer + 0x1D + r_vstate);
	/* [한국어] parent 오프셋 위치의 가변폭 정수를 부모 Volume obj_id로 저장. */
	comp->parent_id = ldm_get_vnum (buffer + 0x2D + r_child);
	/* [한국어] 스트라이프가 있으면(r_stripe != 0) 스트라이프 크기 필드를 읽고, 없으면 0 - 삼항 연산자로 조건부 처리. */
	comp->chunksize = r_stripe ? ldm_get_vnum (buffer+r_parent+0x2E) : 0;

	/* [한국어] 모든 필드가 성공적으로 채워졌으므로 true 반환. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_dgr3() - Disk Group VBLK(버전 3) 레코드를 struct vblk_dgrp로 파싱
 *
 * @buffer: VBLK 레코드 원시 바이트
 * @buflen: @buffer 크기
 * @vb:     공통 헤더가 이미 채워진 struct vblk (union vblk.dgrp에 결과를 채움)
 * @return: (int로 선언돼 있으나 실질적으로 true(1)/false(0)만 반환) 성공/실패
 *
 * Disk Group VBLK는 여러 디스크를 하나의 그룹으로 묶는 식별 문자열만 담는 단순한 타입이다.
 * objid -> name -> diskid 순으로 오프셋을 계산하고, VBLK_FLAG_DGR3_IDS 비트가 켜져 있으면 확장
 * ID 필드 두 개(id1/id2)까지 오프셋만 계산한다(값 자체는 읽지 않고 길이 검증에만 사용). 계산된
 * 길이에 VBLK_SIZE_DGR3를 더해 선언 길이와 비교한 뒤, diskid 오프셋의 문자열을
 * dgrp->disk_id(이름과 달리 uuid_t가 아니라 문자열 배열)로 복사한다.
 *
 * 호출 체인:
 *   ldm_parse_vblk -> [ldm_parse_dgr3] (buf[0x13] == VBLK_DGR3 인 경우)
 */
/**
 * ldm_parse_dgr3 - Read a raw VBLK Disk Group object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk Group object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk Group VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buffer/buflen/vb: 위 ldm_parse_cmp3()와 동일한 역할. 반환형이 bool이 아니라 int로 선언돼 있으나 return문은 true/false만 쓰므로 실질적으로는 0/1 값만 나온다. */
static int ldm_parse_dgr3 (const u8 *buffer, int buflen, struct vblk *vb)
{
	/* [한국어] r_*: 오프셋 계산 결과들, len: 누적 길이. */
	int r_objid, r_name, r_diskid, r_id1, r_id2, len;
	/* [한국어] vb->vblk.dgrp 접근용 지역 별칭. */
	struct vblk_dgrp *dgrp;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!buffer || !vb);

	/* [한국어] objid 필드 오프셋. */
	r_objid  = ldm_relative (buffer, buflen, 0x18, 0);
	/* [한국어] name 필드 오프셋. */
	r_name   = ldm_relative (buffer, buflen, 0x18, r_objid);
	/* [한국어] diskid(그룹 식별 문자열) 필드 오프셋. */
	r_diskid = ldm_relative (buffer, buflen, 0x18, r_name);

	/* [한국어] buffer[0x12]의 DGR3_IDS 비트(PART_INDEX와 값은 같은 0x08이지만 이 함수 문맥에서는 "확장 ID 필드 존재"를 의미)로 확장 필드 여부 판단. */
	if (buffer[0x12] & VBLK_FLAG_DGR3_IDS) {
		/* [한국어] 확장 ID1 필드 오프셋(base가 0x24로 바뀜). */
		r_id1 = ldm_relative (buffer, buflen, 0x24, r_diskid);
		/* [한국어] 확장 ID2 필드 오프셋. */
		r_id2 = ldm_relative (buffer, buflen, 0x24, r_id1);
		/* [한국어] 확장 필드가 있으면 id2 오프셋까지가 전체 누적 길이. */
		len = r_id2;
	/* [한국어] 확장 필드가 없으면. */
	} else
		/* [한국어] diskid 오프셋까지가 전체 누적 길이. */
		len = r_diskid;
	/* [한국어] 오프셋 계산 오류 검사. */
	if (len < 0)
		/* [한국어] 실패 반환. */
		return false;

	/* [한국어] Disk Group v3 고정부 크기(VBLK_SIZE_DGR3)를 더해 전체 레코드 길이 계산. */
	len += VBLK_SIZE_DGR3;
	/* [한국어] 선언 길이와 불일치 검사. */
	if (len != get_unaligned_be32(buffer + 0x14))
		/* [한국어] 실패 반환(여기도 명시적 에러 로그 없이 조용히 실패). */
		return false;

	/* [한국어] name 오프셋 위치의 길이-접두 문자열을 dgrp->disk_id(그룹 식별 문자열, uuid_t가 아니라 char 배열)로 복사 - 필드 이름이 disk_id지만 GUID가 아니라는 점에 주의. */
	dgrp = &vb->vblk.dgrp;
		/* [한국어] ldm_get_vstr()의 나머지 인자(줄바꿈된 계속) - 출력 버퍼 크기. */
	ldm_get_vstr (buffer + 0x18 + r_name, dgrp->disk_id,
		sizeof (dgrp->disk_id));
	/* [한국어] 성공 반환. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_dgr4() - Disk Group VBLK(버전 4) 레코드를 검증(파싱 결과는 저장하지 않음)
 *
 * @buffer: VBLK 레코드 원시 바이트
 * @buflen: @buffer 크기
 * @vb:     공통 헤더가 이미 채워진 struct vblk (이 함수는 vb의 어떤 필드도 갱신하지 않음)
 * @return: true  - 길이 검증 통과(그러나 파싱한 이름 문자열은 버려짐)
 *          false - 오프셋 계산 실패 또는 길이 불일치
 *
 * v3와 달리 v4는 objid -> name 순으로만 오프셋을 계산하고, VBLK_FLAG_DGR4_IDS 비트에 따라 확장
 * ID 필드 두 개(id1/id2, base가 0x44로 v3와 다름)까지 오프셋만 계산한다. 마지막에
 * ldm_get_vstr()로 objid 오프셋의 문자열을 지역 스택 변수 buf[64]에 복사하지만, 이 값을 vb나
 * ldb 어디에도 저장하지 않고 함수가 끝나면서 버려진다 - 사실상 "길이가 선언과 맞는지"만
 * 검증하고 실제 내용은 버리는 셈이다(파싱 결과를 실제로 활용하는 v3와의 중요한 차이).
 *
 * 호출 체인:
 *   ldm_parse_vblk -> [ldm_parse_dgr4] (buf[0x13] == VBLK_DGR4 인 경우)
 */
/**
 * ldm_parse_dgr4 - Read a raw VBLK Disk Group object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk Group object (version 4) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk Group VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buffer/buflen/vb: 다른 ldm_parse_*()와 동일. vb는 읽기만 하고 쓰지 않는다 (길이 검증 전용 함수). */
static bool ldm_parse_dgr4 (const u8 *buffer, int buflen, struct vblk *vb)
{
	/* [한국어] 파싱한 이름을 담을 지역 스택 버퍼 - 이 함수를 벗어나면 사라지는 임시 값. */
	char buf[64];
	/* [한국어] r_*: 오프셋 계산 결과, len: 누적 길이. */
	int r_objid, r_name, r_id1, r_id2, len;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!buffer || !vb);

	/* [한국어] objid 필드 오프셋. */
	r_objid  = ldm_relative (buffer, buflen, 0x18, 0);
	/* [한국어] name 필드 오프셋. */
	r_name   = ldm_relative (buffer, buflen, 0x18, r_objid);

	/* [한국어] buffer[0x12]의 DGR4_IDS 비트로 확장 필드 존재 여부 판단(값은 0x08로 DGR3_IDS와 같지만 이 함수 문맥에서 재해석됨). */
	if (buffer[0x12] & VBLK_FLAG_DGR4_IDS) {
		/* [한국어] 확장 ID1 오프셋(base가 v3와 다른 0x44). */
		r_id1 = ldm_relative (buffer, buflen, 0x44, r_name);
		/* [한국어] 확장 ID2 오프셋. */
		r_id2 = ldm_relative (buffer, buflen, 0x44, r_id1);
		/* [한국어] 확장 필드가 있으면 id2까지가 누적 길이. */
		len = r_id2;
	/* [한국어] 확장 필드가 없으면. */
	} else
		/* [한국어] name 오프셋까지가 누적 길이. */
		len = r_name;
	/* [한국어] 오프셋 계산 오류 검사. */
	if (len < 0)
		/* [한국어] 실패 반환. */
		return false;

	/* [한국어] Disk Group v4 고정부 크기(VBLK_SIZE_DGR4)를 더해 전체 레코드 길이 계산. */
	len += VBLK_SIZE_DGR4;
	/* [한국어] 선언 길이와 불일치 검사. */
	if (len != get_unaligned_be32(buffer + 0x14))
		/* [한국어] 실패 반환. */
		return false;

	/* [한국어] objid 오프셋 위치의 문자열을 지역 buf로만 복사(길이 검증 부산물) - 결과는 어디에도 저장되지 않고 이 줄이 끝나면 buf와 함께 버려진다. */
	ldm_get_vstr (buffer + 0x18 + r_objid, buf, sizeof (buf));
	/* [한국어] 길이 검증을 통과했으므로 성공 반환(내용은 버렸지만 "이 레코드가 DGR4로서 구조적으로 유효하다"는 확인 자체는 의미가 있다). */
	return true;
}

/*
 * [한국어]
 * ldm_parse_dsk3() - Disk VBLK(버전 3) 레코드를 struct vblk_disk로 파싱
 *
 * @buffer: VBLK 레코드 원시 바이트
 * @buflen: @buffer 크기
 * @vb:     공통 헤더가 이미 채워진 struct vblk (union vblk.disk에 결과를 채움)
 * @return: true  - @vb->vblk.disk에 유효한 Disk 정보가 채워짐
 *          false - 오프셋 계산 실패, 길이 불일치, 또는 GUID 파싱 실패
 *
 * Disk VBLK(v3)는 물리 디스크 하나를 나타내며, v4와 달리 대체 이름(alt_name) 문자열까지 함께
 * 담는다. objid -> name -> diskid -> altname 순으로 오프셋을 계산한 뒤(확장 플래그 분기가 없는
 * 가장 단순한 형태), VBLK_SIZE_DSK3를 더해 길이를 검증한다. 이후 diskid 오프셋의 문자열을
 * disk->alt_name으로 복사하고, name 오프셋 바로 다음(0x19, 문자열이 아니라 고정 16바이트
 * 위치)의 GUID를 uuid_parse()로 disk->disk_id에 파싱한다 - 이 disk_id가 나중에
 * ldm_get_disk_objid()에서 PRIVHEAD의 disk_id와 비교되는 핵심 필드다.
 *
 * 호출 체인:
 *   ldm_parse_vblk -> [ldm_parse_dsk3] (buf[0x13] == VBLK_DSK3 인 경우)
 */
/**
 * ldm_parse_dsk3 - Read a raw VBLK Disk object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buffer/buflen/vb: 다른 ldm_parse_*()와 동일. */
static bool ldm_parse_dsk3 (const u8 *buffer, int buflen, struct vblk *vb)
{
	/* [한국어] r_*: 오프셋 계산 결과, len: 누적 길이. */
	int r_objid, r_name, r_diskid, r_altname, len;
	/* [한국어] vb->vblk.disk 접근용 지역 별칭. */
	struct vblk_disk *disk;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!buffer || !vb);

	/* [한국어] objid 필드 오프셋. */
	r_objid   = ldm_relative (buffer, buflen, 0x18, 0);
	/* [한국어] name 필드 오프셋. */
	r_name    = ldm_relative (buffer, buflen, 0x18, r_objid);
	/* [한국어] diskid(대체 이름 문자열이 아니라 또 다른 가변폭 필드) 오프셋. */
	r_diskid  = ldm_relative (buffer, buflen, 0x18, r_name);
	/* [한국어] altname(대체 이름) 필드 오프셋 - 이 타입은 확장 플래그 분기가 없어 바로 마지막 필드까지 연쇄 계산한다. */
	r_altname = ldm_relative (buffer, buflen, 0x18, r_diskid);
	/* [한국어] altname 오프셋까지가 전체 누적 길이. */
	len = r_altname;
	/* [한국어] 오프셋 계산 오류 검사. */
	if (len < 0)
		/* [한국어] 실패 반환. */
		return false;

	/* [한국어] Disk v3 고정부 크기(VBLK_SIZE_DSK3)를 더해 전체 레코드 길이 계산. */
	len += VBLK_SIZE_DSK3;
	/* [한국어] 선언 길이와 불일치 검사. */
	if (len != get_unaligned_be32(buffer + 0x14))
		/* [한국어] 실패 반환. */
		return false;

	/* [한국어] vb->vblk.disk에 접근할 포인터 설정. */
	disk = &vb->vblk.disk;
	/* [한국어] diskid 오프셋 위치의 길이-접두 문자열을 disk->alt_name(대체 이름)으로 복사 - v4 파서는 이 필드를 전혀 채우지 않는다는 점과 대조적. */
	ldm_get_vstr (buffer + 0x18 + r_diskid, disk->alt_name,
		/* [한국어] ldm_get_vstr()의 나머지 인자(줄바꿈된 계속) - 출력 버퍼 크기. */
		sizeof (disk->alt_name));
	/* [한국어] name 오프셋 바로 다음(0x19, 고정 위치)의 16바이트를 표준 GUID 형식으로 파싱해 disk->disk_id에 저장 - 실패하면 GUID가 깨진 것. */
	if (uuid_parse(buffer + 0x19 + r_name, &disk->disk_id))
		/* [한국어] GUID 파싱 실패 시 별도 로그 없이 바로 실패 반환. */
		return false;

	/* [한국어] 성공 반환. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_dsk4() - Disk VBLK(버전 4) 레코드를 struct vblk_disk로 파싱
 *
 * @buffer: VBLK 레코드 원시 바이트
 * @buflen: @buffer 크기
 * @vb:     공통 헤더가 이미 채워진 struct vblk (union vblk.disk에 결과를 채움)
 * @return: true  - @vb->vblk.disk.disk_id에 유효한 GUID가 채워짐
 *          false - 오프셋 계산 실패 또는 길이 불일치
 *
 * v3보다 훨씬 단순한 v4 레이아웃: objid -> name 오프셋만 계산하고(diskid/altname 필드 자체가
 * 없음), VBLK_SIZE_DSK4를 더해 길이를 검증한 뒤, name 오프셋 위치의 GUID를 import_uuid()(v3의
 * uuid_parse()와는 다른 API - 오프셋 계산 기준점도 v3의 0x19 고정 오프셋과 달리 name 오프셋에
 * 바로 얹힘)로 disk->disk_id에 저장한다. 이 함수는 disk->alt_name을 전혀 건드리지 않으므로,
 * v4 Disk VBLK로 채워진 struct vblk는 alt_name이 초기화되지 않은(vb가 kmalloc_obj()로만
 * 할당되어 zero-fill 없음) 상태로 남는다.
 *
 * 호출 체인:
 *   ldm_parse_vblk -> [ldm_parse_dsk4] (buf[0x13] == VBLK_DSK4 인 경우)
 */
/**
 * ldm_parse_dsk4 - Read a raw VBLK Disk object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Disk object (version 4) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Disk VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buffer/buflen/vb: 다른 ldm_parse_*()와 동일. */
static bool ldm_parse_dsk4 (const u8 *buffer, int buflen, struct vblk *vb)
{
	/* [한국어] r_*: 오프셋 계산 결과, len: 누적 길이. */
	int r_objid, r_name, len;
	/* [한국어] vb->vblk.disk 접근용 지역 별칭. */
	struct vblk_disk *disk;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!buffer || !vb);

	/* [한국어] objid 필드 오프셋. */
	r_objid = ldm_relative (buffer, buflen, 0x18, 0);
	/* [한국어] name 필드 오프셋 - v4는 이 다음 확장 필드가 없어 바로 마지막 필드. */
	r_name  = ldm_relative (buffer, buflen, 0x18, r_objid);
	/* [한국어] name 오프셋까지가 전체 누적 길이. */
	len     = r_name;
	/* [한국어] 오프셋 계산 오류 검사. */
	if (len < 0)
		/* [한국어] 실패 반환. */
		return false;

	/* [한국어] Disk v4 고정부 크기(VBLK_SIZE_DSK4)를 더해 전체 레코드 길이 계산. */
	len += VBLK_SIZE_DSK4;
	/* [한국어] 선언 길이와 불일치 검사. */
	if (len != get_unaligned_be32(buffer + 0x14))
		/* [한국어] 실패 반환. */
		return false;

	/* [한국어] vb->vblk.disk에 접근할 포인터 설정. */
	disk = &vb->vblk.disk;
	/* [한국어] name 오프셋 위치의 16바이트를 import_uuid()로 disk->disk_id에 저장 - v3의 uuid_parse()와 다른 API를 쓴다는 점, 그리고 오프셋 기준(0x18+r_name, v3는 0x19+r_name)도 미묘하게 다르다는 점에 유의. */
	import_uuid(&disk->disk_id, buffer + 0x18 + r_name);
	/* [한국어] 성공 반환. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_prt3() - Partition VBLK(버전 3) 레코드를 struct vblk_part로 파싱
 *
 * @buffer: VBLK 레코드 원시 바이트
 * @buflen: @buffer 크기
 * @vb:     공통 헤더가 이미 채워진 struct vblk (union vblk.part에 결과를 채움)
 * @return: true  - @vb->vblk.part에 유효한 Partition 정보가 채워짐(리눅스 파티션 등록에 직접 사용)
 *          false - 오프셋 계산 실패 또는 선언 길이 초과
 *
 * 이 파일에서 가장 중요한 파서다 - 여기서 채워지는 struct vblk_part가 곧
 * ldm_create_data_partitions()가 put_partition()에 넘길 실제 파티션 정보이기 때문이다. 다른
 * ldm_parse_*() 함수들과 달리 각 ldm_relative() 호출마다 개별적으로 실패를 검사하고 원인을
 * 상세히 로그로 남기는 좀 더 방어적인 스타일을 쓴다. objid -> name -> size -> parent -> diskid
 * 순으로 오프셋을 계산하고, VBLK_FLAG_PART_INDEX 비트가 있으면 index 필드까지 추가 계산한다.
 * 길이 검증은 다른 타입들과 달리 '>' 비교(선언 길이를 초과하면 실패, 정확히 같지 않아도 허용)를
 * 쓴다는 점이 특이하다. 마지막으로 start/volume_offset(둘 다 고정폭 필드, 유일하게 가변폭이
 * 아닌 위치 정보)와 size/parent_id/disk_id(가변폭)를 읽고, partnum은 PART_INDEX 플래그가 있을
 * 때만 채우고 없으면 0으로 고정한다.
 *
 * 호출 체인:
 *   ldm_parse_vblk -> [ldm_parse_prt3] (buf[0x13] == VBLK_PRT3 인 경우)
 *   -> (이후) ldm_create_data_partitions -> put_partition
 */
/**
 * ldm_parse_prt3 - Read a raw VBLK Partition object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Partition object (version 3) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Partition VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buffer/buflen/vb: 다른 ldm_parse_*()와 동일. */
static bool ldm_parse_prt3(const u8 *buffer, int buflen, struct vblk *vb)
{
	/* [한국어] r_*: 오프셋 계산 결과, len: 누적 길이. */
	int r_objid, r_name, r_size, r_parent, r_diskid, r_index, len;
	/* [한국어] vb->vblk.part 접근용 지역 별칭. */
	struct vblk_part *part;

	/* [한국어] 널 포인터 방어적 단언(BUG_ON은 매크로 스타일 차이일 뿐 다른 파서들의 BUG_ON (...)과 동일). */
	BUG_ON(!buffer || !vb);
	/* [한국어] objid 필드 오프셋. */
	r_objid = ldm_relative(buffer, buflen, 0x18, 0);
	/* [한국어] 오프셋 계산 실패를 다른 파서들과 달리 이 함수는 매 단계마다 즉시 검사 - 더 상세한 진단을 위한 방어적 스타일. */
	if (r_objid < 0) {
		/* [한국어] 실패한 오프셋 이름과 값을 로그로 남김. */
		ldm_error("r_objid %d < 0", r_objid);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] name 필드 오프셋. */
	r_name = ldm_relative(buffer, buflen, 0x18, r_objid);
	/* [한국어] 오프셋 계산 실패 검사. */
	if (r_name < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_name %d < 0", r_name);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] size 필드 오프셋(base가 0x34로 바뀜 - 사이에 고정폭 필드 start/volume_offset이 끼어 있어 base가 name의 0x18보다 커짐). */
	r_size = ldm_relative(buffer, buflen, 0x34, r_name);
	/* [한국어] 오프셋 계산 실패 검사. */
	if (r_size < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_size %d < 0", r_size);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] parent(부모 Component obj_id) 필드 오프셋. */
	r_parent = ldm_relative(buffer, buflen, 0x34, r_size);
	/* [한국어] 오프셋 계산 실패 검사. */
	if (r_parent < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_parent %d < 0", r_parent);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] diskid(소유 디스크를 가리키는 obj_id 참조) 필드 오프셋. */
	r_diskid = ldm_relative(buffer, buflen, 0x34, r_parent);
	/* [한국어] 오프셋 계산 실패 검사. */
	if (r_diskid < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_diskid %d < 0", r_diskid);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] buffer[0x12]의 PART_INDEX 비트로 파티션 인덱스 확장 필드 존재 여부 판단. */
	if (buffer[0x12] & VBLK_FLAG_PART_INDEX) {
		/* [한국어] index 필드 오프셋. */
		r_index = ldm_relative(buffer, buflen, 0x34, r_diskid);
		/* [한국어] 오프셋 계산 실패 검사. */
		if (r_index < 0) {
			/* [한국어] 실패 로그. */
			ldm_error("r_index %d < 0", r_index);
			/* [한국어] 실패 반환. */
			return false;
		}
		/* [한국어] 확장 필드가 있으면 index 오프셋까지가 전체 누적 길이. */
		len = r_index;
	/* [한국어] 확장 필드가 없으면. */
	} else
		/* [한국어] diskid 오프셋까지가 전체 누적 길이. */
		len = r_diskid;
	/* [한국어] len 자체가 이미 음수(오류)로 넘어온 경우 재검사(위에서 이미 개별 검사했지만 이중 방어). */
	if (len < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("len %d < 0", len);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] Partition v3 고정부 크기(VBLK_SIZE_PRT3)를 더해 전체 레코드 길이 계산. */
	len += VBLK_SIZE_PRT3;
	/* [한국어] 다른 타입들과 달리 '>' 비교 - 계산된 길이가 선언 길이를 "초과"하면 실패, 정확히 같지 않아도(더 작으면) 허용하는 완화된 검사. */
	if (len > get_unaligned_be32(buffer + 0x14)) {
		/* [한국어] 계산값과 선언값을 함께 로그로 남김. */
		ldm_error("len %d > BE32(buffer + 0x14) %d", len,
			/* [한국어] get_unaligned_be32() 재호출(줄바꿈된 계속) - 로그 인자로 다시 값을 읽음. */
				get_unaligned_be32(buffer + 0x14));
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] vb->vblk.part에 접근할 포인터 설정. */
	part = &vb->vblk.part;
	/* [한국어] buffer+0x24+r_name 위치에서 고정폭(!) 빅엔디안 64비트로 파티션 시작 LBA를 직접 읽음 - 다른 필드들과 달리 가변폭 인코딩이 아닌 고정폭 필드라는 점에 유의. */
	part->start = get_unaligned_be64(buffer + 0x24 + r_name);
	/* [한국어] buffer+0x2C+r_name 위치에서 고정폭 빅엔디안 64비트로 볼륨 내 오프셋을 읽음 - 이 값 역시 이 파일 내에서는 파싱만 되고 이후 재사용되지 않는다. */
	part->volume_offset = get_unaligned_be64(buffer + 0x2C + r_name);
	/* [한국어] size 오프셋 위치의 가변폭 정수를 파티션 크기(섹터)로 저장 - put_partition()에 그대로 전달될 핵심 값. */
	part->size = ldm_get_vnum(buffer + 0x34 + r_name);
	/* [한국어] size 오프셋 위치(재사용)의 가변폭 정수를 부모 Component obj_id로 저장. */
	part->parent_id = ldm_get_vnum(buffer + 0x34 + r_size);
	/* [한국어] parent 오프셋 위치의 가변폭 정수를 소유 디스크 참조(obj_id)로 저장 - ldm_create_data_partitions()의 필터링 키. */
	part->disk_id = ldm_get_vnum(buffer + 0x34 + r_parent);
	/* [한국어] vb->flags(이미 ldm_parse_vblk()가 buf[0x12]로 설정해 둔 값)로 PART_INDEX 비트를 다시 확인 - 이 함수 앞부분에서는 buffer[0x12]를 직접 봤는데 여기서는 vb->flags를 보는 점이 다르다(같은 값이므로 결과는 동일하지만 접근 경로가 함수 안에서 일관되지 않음). */
	if (vb->flags & VBLK_FLAG_PART_INDEX)
		/* [한국어] 인덱스 확장 필드가 있으면 실제 파티션 인덱스 값을 읽음. */
		part->partnum = buffer[0x35 + r_diskid];
	/* [한국어] 확장 필드가 없으면. */
	else
		/* [한국어] partnum을 0으로 고정. */
		part->partnum = 0;
	/* [한국어] 성공 반환 - 이제 vb->vblk.part가 리눅스 파티션 등록에 바로 쓰일 수 있는 상태가 됨. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_vol5() - Volume VBLK(버전 5) 레코드를 struct vblk_volu로 파싱
 *
 * @buffer: VBLK 레코드 원시 바이트
 * @buflen: @buffer 크기
 * @vb:     공통 헤더가 이미 채워진 struct vblk (union vblk.volu에 결과를 채움)
 * @return: true  - @vb->vblk.volu에 유효한 Volume 정보가 채워짐
 *          false - 오프셋 계산 실패 또는 선언 길이 초과
 *
 * 이 파일에서 가장 많은 가변폭 필드를 갖는(따라서 가장 긴) 파서다. objid -> name -> vtype
 * (볼륨 종류 문자열) -> disable_drive_letter(볼륨 상태 문자열이 위치) -> child(부모 격
 * 관계) -> size 순으로 기본 오프셋을 계산한 뒤, 네 개의 독립적인 플래그(VBLK_FLAG_VOLU_ID1/
 * ID2/SIZE/DRIVE)에 따라 최대 4개의 추가 확장 필드(id1/id2/size2/drive)를 조건부로 계산한다 -
 * 각 확장 필드는 켜져 있으면 새로 오프셋을 계산하고, 꺼져 있으면 바로 이전 확장 필드의 오프셋을
 * 그대로 이어받는(예: r_id1 = r_size) 체이닝 패턴을 쓴다. 길이 검증은 ldm_parse_prt3()와
 * 마찬가지로 '>' 비교(초과 시에만 실패)를 쓴다. 필드 값을 채우는 단계에서는 volume_type/
 * volume_state를 각각 ldm_get_vstr()(길이-접두)과 memcpy()(고정 16바이트, 길이-접두 아님)로
 * 서로 다른 방식으로 복사하고, guid는 uuid_parse()가 아니라 memcpy()로 원시 바이트만 복사한다는
 * 점이 다른 GUID류 필드들과 다르다.
 *
 * 호출 체인:
 *   ldm_parse_vblk -> [ldm_parse_vol5] (buf[0x13] == VBLK_VOL5 인 경우)
 */
/**
 * ldm_parse_vol5 - Read a raw VBLK Volume object into a vblk structure
 * @buffer:  Block of data being worked on
 * @buflen:  Size of the block of data
 * @vb:      In-memory vblk in which to return information
 *
 * Read a raw VBLK Volume object (version 5) into a vblk structure.
 *
 * Return:  'true'   @vb contains a Volume VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buffer/buflen/vb: 다른 ldm_parse_*()와 동일. */
static bool ldm_parse_vol5(const u8 *buffer, int buflen, struct vblk *vb)
{
	/* [한국어] 기본 오프셋 체인(objid~size)을 담을 변수들. */
	int r_objid, r_name, r_vtype, r_disable_drive_letter, r_child, r_size;
	/* [한국어] 조건부 확장 필드(id1/id2/size2/drive)와 최종 누적 길이 len. */
	int r_id1, r_id2, r_size2, r_drive, len;
	/* [한국어] vb->vblk.volu 접근용 지역 별칭. */
	struct vblk_volu *volu;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON(!buffer || !vb);
	/* [한국어] objid 필드 오프셋. */
	r_objid = ldm_relative(buffer, buflen, 0x18, 0);
	/* [한국어] 이하 각 단계마다 즉시 오류를 검사하는 방어적 스타일(ldm_parse_prt3()와 동일). */
	if (r_objid < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_objid %d < 0", r_objid);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] name 필드 오프셋. */
	r_name = ldm_relative(buffer, buflen, 0x18, r_objid);
	/* [한국어] 오류 검사. */
	if (r_name < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_name %d < 0", r_name);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] vtype(볼륨 종류 문자열) 필드 오프셋. */
	r_vtype = ldm_relative(buffer, buflen, 0x18, r_name);
	/* [한국어] 오류 검사. */
	if (r_vtype < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_vtype %d < 0", r_vtype);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] disable_drive_letter 필드 오프셋 - 이름과 달리 실제로는 이 위치에 볼륨 상태 문자열(volume_state)이 저장된다(변수명이 필드 용도와 어긋나는 원문 코드의 특이점). */
	r_disable_drive_letter = ldm_relative(buffer, buflen, 0x18, r_vtype);
	/* [한국어] 오류 검사. */
	if (r_disable_drive_letter < 0) {
		/* [한국어] 실패 로그(변수명이 길어 두 줄에 걸쳐 출력). */
		ldm_error("r_disable_drive_letter %d < 0",
				/* [한국어] ldm_error()의 나머지 인자(줄바꿈된 계속). */
				r_disable_drive_letter);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] child 필드 오프셋(base가 0x2D로 바뀜 - 부모 관계 필드). */
	r_child = ldm_relative(buffer, buflen, 0x2D, r_disable_drive_letter);
	/* [한국어] 오류 검사. */
	if (r_child < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_child %d < 0", r_child);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] size 필드 오프셋(base가 0x3D로 바뀜). */
	r_size = ldm_relative(buffer, buflen, 0x3D, r_child);
	/* [한국어] 오류 검사. */
	if (r_size < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("r_size %d < 0", r_size);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] buffer[0x12]의 VOLU_ID1 비트로 확장 ID1 필드 존재 여부 판단. */
	if (buffer[0x12] & VBLK_FLAG_VOLU_ID1) {
		/* [한국어] id1 필드 오프셋(base가 0x52로 바뀜 - 모든 확장 필드가 공유하는 base). */
		r_id1 = ldm_relative(buffer, buflen, 0x52, r_size);
		/* [한국어] 오류 검사. */
		if (r_id1 < 0) {
			/* [한국어] 실패 로그. */
			ldm_error("r_id1 %d < 0", r_id1);
			/* [한국어] 실패 반환. */
			return false;
		}
	/* [한국어] 비트가 꺼져 있으면. */
	} else
		/* [한국어] id1 오프셋을 그대로 size 오프셋으로 대체(확장 필드가 없으므로 다음 필드가 size 바로 뒤에서 시작한다는 뜻) - 이후 id2/size2/drive도 같은 체이닝 패턴을 반복. */
		r_id1 = r_size;
	/* [한국어] buffer[0x12]의 VOLU_ID2 비트로 확장 ID2 필드 존재 여부 판단. */
	if (buffer[0x12] & VBLK_FLAG_VOLU_ID2) {
		/* [한국어] id2 필드 오프셋(id1 다음, 같은 base 0x52). */
		r_id2 = ldm_relative(buffer, buflen, 0x52, r_id1);
		/* [한국어] 오류 검사. */
		if (r_id2 < 0) {
			/* [한국어] 실패 로그. */
			ldm_error("r_id2 %d < 0", r_id2);
			/* [한국어] 실패 반환. */
			return false;
		}
	/* [한국어] 비트가 꺼져 있으면. */
	} else
		/* [한국어] id2를 id1과 동일하게(확장 없음을 체이닝으로 전파). */
		r_id2 = r_id1;
	/* [한국어] buffer[0x12]의 VOLU_SIZE 비트로 확장 size2 필드 존재 여부 판단. */
	if (buffer[0x12] & VBLK_FLAG_VOLU_SIZE) {
		/* [한국어] size2 필드 오프셋. */
		r_size2 = ldm_relative(buffer, buflen, 0x52, r_id2);
		/* [한국어] 오류 검사. */
		if (r_size2 < 0) {
			/* [한국어] 실패 로그. */
			ldm_error("r_size2 %d < 0", r_size2);
			/* [한국어] 실패 반환. */
			return false;
		}
	/* [한국어] 비트가 꺼져 있으면. */
	} else
		/* [한국어] size2를 id2와 동일하게. */
		r_size2 = r_id2;
	/* [한국어] buffer[0x12]의 VOLU_DRIVE 비트로 확장 drive(드라이브 힌트) 필드 존재 여부 판단. */
	if (buffer[0x12] & VBLK_FLAG_VOLU_DRIVE) {
		/* [한국어] drive 필드 오프셋. */
		r_drive = ldm_relative(buffer, buflen, 0x52, r_size2);
		/* [한국어] 오류 검사. */
		if (r_drive < 0) {
			/* [한국어] 실패 로그. */
			ldm_error("r_drive %d < 0", r_drive);
			/* [한국어] 실패 반환. */
			return false;
		}
	/* [한국어] 비트가 꺼져 있으면. */
	} else
		/* [한국어] drive를 size2와 동일하게 - 네 확장 필드 체이닝의 마지막 단계. */
		r_drive = r_size2;
	/* [한국어] drive 오프셋이 곧 전체 누적 길이(모든 조건부 확장 필드를 거친 최종값). */
	len = r_drive;
	/* [한국어] len 자체가 이미 음수인 경우 재검사(다른 파서와 동일한 이중 방어). */
	if (len < 0) {
		/* [한국어] 실패 로그. */
		ldm_error("len %d < 0", len);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] Volume v5 고정부 크기(VBLK_SIZE_VOL5)를 더해 전체 레코드 길이 계산. */
	len += VBLK_SIZE_VOL5;
	/* [한국어] ldm_parse_prt3()와 동일하게 '>' 비교(초과 시에만 실패). */
	if (len > get_unaligned_be32(buffer + 0x14)) {
		/* [한국어] 계산값과 선언값을 함께 로그로 남김. */
		ldm_error("len %d > BE32(buffer + 0x14) %d", len,
			/* [한국어] get_unaligned_be32() 재호출(줄바꿈된 계속). */
				get_unaligned_be32(buffer + 0x14));
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] vb->vblk.volu에 접근할 포인터 설정. */
	volu = &vb->vblk.volu;
	/* [한국어] name 오프셋 위치의 길이-접두 문자열을 volu->volume_type(볼륨 종류)으로 복사. */
	ldm_get_vstr(buffer + 0x18 + r_name, volu->volume_type,
			/* [한국어] ldm_get_vstr()의 나머지 인자(줄바꿈된 계속) - 출력 버퍼 크기. */
			sizeof(volu->volume_type));
	/* [한국어] disable_drive_letter 오프셋(실제로는 상태 문자열 위치) 16바이트를 volu->volume_state로 memcpy() - ldm_get_vstr()이 아니라 memcpy()를 쓰므로 길이-접두 규칙을 따르지 않는 고정폭 복사라는 점에 유의. */
	memcpy(volu->volume_state, buffer + 0x18 + r_disable_drive_letter,
			/* [한국어] memcpy()의 나머지 인자(줄바꿈된 계속) - 복사 크기(고정 16바이트). */
			sizeof(volu->volume_state));
	/* [한국어] child 오프셋 위치의 가변폭 정수를 볼륨 전체 크기(섹터)로 저장 - 이 값은 이 파일 내에서 이후 재사용되지 않는다(실제 파티션 크기는 vblk_part.size가 개별 담당). */
	volu->size = ldm_get_vnum(buffer + 0x3D + r_child);
	/* [한국어] size 오프셋 위치의 1바이트를 partition_type(MBR sys_ind와 유사한 의미로 추정)으로 저장. */
	volu->partition_type = buffer[0x41 + r_size];
	/* [한국어] size 오프셋 위치의 16바이트를 volu->guid로 memcpy() - uuid_parse()/import_uuid()가 아니라 원시 바이트 그대로 복사하므로 uuid_t로 해석되지 않은 raw 배열이라는 점에서 다른 disk_id류 GUID 필드들과 다르다. */
	memcpy(volu->guid, buffer + 0x42 + r_size, sizeof(volu->guid));
	/* [한국어] VOLU_DRIVE 플래그가 켜져 있을 때만 드라이브 힌트 필드를 실제로 읽는다(오프셋 계산은 이미 위에서 끝났지만, 값 자체를 채우는 것은 이 조건 블록 안에서만 이뤄짐). */
	if (buffer[0x12] & VBLK_FLAG_VOLU_DRIVE) {
		/* [한국어] size 오프셋 위치의 길이-접두 문자열을 volu->drive_hint로 복사. */
		ldm_get_vstr(buffer + 0x52 + r_size, volu->drive_hint,
				/* [한국어] ldm_get_vstr()의 나머지 인자(줄바꿈된 계속) - 출력 버퍼 크기. */
				sizeof(volu->drive_hint));
	/* [한국어] 조건이 꺼져 있으면 drive_hint는 초기화되지 않은 채로 남는다(vb가 kmalloc_obj() 로만 할당되어 zero-fill 없음). */
	}
	/* [한국어] 성공 반환. */
	return true;
}

/*
 * [한국어]
 * ldm_parse_vblk() - VBLK 공통 헤더를 읽고 type별 파서로 디스패치
 *
 * @buf: 재조립이 끝난(또는 애초에 조각나지 않은) VBLK 레코드 원시 바이트 전체
 * @len: @buf 크기
 * @vb:  결과를 채워 넣을 struct vblk (호출자가 kmalloc_obj()로 할당한 버퍼, zero-fill 없음)
 * @return: true  - @vb에 유효한 VBLK 정보가 채워짐(공통 헤더 + type별 페이로드)
 *          false - 공통 헤더 오프셋 계산 실패, 또는 type별 파서가 실패, 또는 type이 인식되지 않음
 *
 * 이 함수는 모든 VBLK 타입에 공통인 부분(objid 오프셋 계산, flags/type/obj_id/name)만 먼저
 * 채운 뒤, vb->type(buf[0x13])에 따라 ldm_parse_{cmp3,dsk3,dsk4,dgr3,dgr4,prt3,vol5}() 중 정확히
 * 하나에 나머지 파싱을 위임하는 원문 주석 그대로의 "헬퍼 함수로 위임(delegates the rest of the
 * work to helper functions)" 구조다. switch문에 해당하는 case가 없으면(알 수 없는 type) result는
 * 초기값 false 그대로 남아 실패로 처리된다. 성공/실패 여부와 무관하게 마지막에 obj_id/type을
 * 포함한 디버그 또는 에러 로그를 남긴다.
 *
 * 호출 체인:
 *   ldm_ldmdb_add -> [ldm_parse_vblk] -> ldm_parse_{cmp3,dsk3,dsk4,dgr3,dgr4,prt3,vol5}
 */
/**
 * ldm_parse_vblk - Read a raw VBLK object into a vblk structure
 * @buf:  Block of data being worked on
 * @len:  Size of the block of data
 * @vb:   In-memory vblk in which to return information
 *
 * Read a raw VBLK object into a vblk structure.  This function just reads the
 * information common to all VBLK types, then delegates the rest of the work to
 * helper functions: ldm_parse_*.
 *
 * Return:  'true'   @vb contains a VBLK
 *          'false'  @vb contents are not defined
 */
/* [한국어] buf/len: 재조립 완료된 VBLK 레코드와 그 크기, vb: 결과를 채울 버퍼. */
static bool ldm_parse_vblk (const u8 *buf, int len, struct vblk *vb)
{
	/* [한국어] type별 파서의 성공 여부(최종 반환값) - 기본 false(알 수 없는 type이면 이 값이 그대로 반환됨). */
	bool result = false;
	/* [한국어] objid 필드의 오프셋. */
	int r_objid;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!buf || !vb);

	/* [한국어] 고정 헤더(0x18) 바로 뒤 objid 필드 오프셋 계산 - 모든 VBLK 타입이 공유하는 최초의 가변폭 필드. */
	r_objid = ldm_relative (buf, len, 0x18, 0);
	/* [한국어] 이 최초 계산부터 실패하면 헤더 자체가 손상된 것이므로 type별 파서를 시도할 필요조차 없다. */
	if (r_objid < 0) {
		/* [한국어] 헤더 손상 에러 로그. */
		ldm_error ("VBLK header is corrupt.");
		/* [한국어] 즉시 실패 반환. */
		return false;
	}

	/* [한국어] buf[0x12](플래그 바이트)를 그대로 vb->flags에 복사 - 이후 각 type별 파서가 이 값(또는 원본 buf[0x12])으로 확장 필드 존재 여부를 판단. */
	vb->flags  = buf[0x12];
	/* [한국어] buf[0x13](타입 코드)를 vb->type에 복사 - 아래 switch문의 디스패치 키. */
	vb->type   = buf[0x13];
	/* [한국어] buf+0x18 위치의 가변폭 정수를 obj_id로 저장 - 모든 VBLK가 공유하는 로컬 식별자. */
	vb->obj_id = ldm_get_vnum (buf + 0x18);
	/* [한국어] objid 오프셋 다음 위치의 길이-접두 문자열을 vb->name으로 복사 - 모든 타입이 공유하는 마지막 공통 필드. */
	ldm_get_vstr (buf+0x18+r_objid, vb->name, sizeof (vb->name));

	/* [한국어] 여기서부터 type별 파서로 디스패치 - 정확히 하나의 case만 실행된다. */
	switch (vb->type) {
		/* [한국어] Component v3 -> ldm_parse_cmp3(). */
		case VBLK_CMP3:  result = ldm_parse_cmp3 (buf, len, vb); break;
		/* [한국어] Disk v3 -> ldm_parse_dsk3(). */
		case VBLK_DSK3:  result = ldm_parse_dsk3 (buf, len, vb); break;
		/* [한국어] Disk v4 -> ldm_parse_dsk4(). */
		case VBLK_DSK4:  result = ldm_parse_dsk4 (buf, len, vb); break;
		/* [한국어] Disk Group v3 -> ldm_parse_dgr3() (반환형이 int인 함수를 bool result에 대입 - 0/1 값만 나오므로 문제 없음). */
		case VBLK_DGR3:  result = ldm_parse_dgr3 (buf, len, vb); break;
		/* [한국어] Disk Group v4 -> ldm_parse_dgr4(). */
		case VBLK_DGR4:  result = ldm_parse_dgr4 (buf, len, vb); break;
		/* [한국어] Partition v3 -> ldm_parse_prt3() - 실제 리눅스 파티션으로 이어지는 핵심 경로. */
		case VBLK_PRT3:  result = ldm_parse_prt3 (buf, len, vb); break;
		/* [한국어] Volume v5 -> ldm_parse_vol5(). */
		case VBLK_VOL5:  result = ldm_parse_vol5 (buf, len, vb); break;
	}

	/* [한국어] 디스패치 결과에 따라 로그 레벨을 달리함. */
	if (result)
		/* [한국어] 성공: obj_id와 type을 16진수로 남기는 디버그 로그. */
		ldm_debug ("Parsed VBLK 0x%llx (type: 0x%02x) ok.",
			 (unsigned long long) vb->obj_id, vb->type);
	/* [한국어] 실패(또는 type 미인식). */
	else
		/* [한국어] 동일한 정보를 에러 레벨로 남김 - 운영 환경에서도(CONFIG_LDM_DEBUG 꺼짐) 실패 사실만은 항상 로그에 남도록 함. */
		ldm_error ("Failed to parse VBLK 0x%llx (type: 0x%02x).",
			(unsigned long long) vb->obj_id, vb->type);

	/* [한국어] 최종 결과를 호출자(ldm_ldmdb_add())에 반환. */
	return result;
}


/*
 * [한국어]
 * ldm_ldmdb_add() - 파싱된 VBLK 하나를 타입에 맞는 ldmdb 리스트에 삽입
 *
 * @data: 재조립이 끝난 VBLK 레코드 원시 바이트
 * @len:  @data 크기
 * @ldb:  삽입 대상 in-memory 데이터베이스 캐시
 * @return: true  - VBLK를 파싱해 알맞은 리스트에 추가함
 *          false - 메모리 부족 또는 ldm_parse_vblk() 파싱 실패
 *
 * 이 함수는 (1) 새 struct vblk를 kmalloc_obj()로 힙에 할당(zero-fill 없음), (2)
 * ldm_parse_vblk()로 그 내용을 채움(실패 시 방금 할당한 vb를 kfree()하고 실패 반환), (3)
 * vb->type에 따라 다섯 리스트(v_dgrp/v_disk/v_volu/v_comp/v_part) 중 하나에 연결하는 순서로
 * 진행한다. 특히 VBLK_PRT3(파티션)의 경우 단순히 리스트 끝에 추가하지 않고, 같은 disk_id를 가진
 * 기존 항목들과 start(시작 섹터)를 비교하며 정렬된 위치에 삽입한다(list_for_each로 처음 만나는,
 * "같은 디스크이면서 시작 섹터가 이 항목보다 더 큰" 항목 바로 앞에 끼워 넣고, 그런 항목이 없으면
 * 리스트 끝에 추가) - 이 정렬 덕분에 ldm_create_data_partitions()가 별도 정렬 없이도 항상
 * 시작 섹터 오름차순으로 파티션을 등록할 수 있다. 원문 주석대로 이 함수는 VBLK의 유효성 자체는
 * 검사하지 않는다(ldm_parse_vblk()가 이미 검사).
 *
 * 호출 체인:
 *   ldm_get_vblks / ldm_frag_commit -> [ldm_ldmdb_add] -> ldm_parse_vblk
 */
/**
 * ldm_ldmdb_add - Adds a raw VBLK entry to the ldmdb database
 * @data:  Raw VBLK to add to the database
 * @len:   Size of the raw VBLK
 * @ldb:   Cache of the database structures
 *
 * The VBLKs are sorted into categories.  Partitions are also sorted by offset.
 *
 * N.B.  This function does not check the validity of the VBLKs.
 *
 * Return:  'true'   The VBLK was added
 *          'false'  An error occurred
 */
/* [한국어] data/len: 파싱할 VBLK 레코드와 크기, ldb: 삽입 대상 데이터베이스 캐시. */
static bool ldm_ldmdb_add (u8 *data, int len, struct ldmdb *ldb)
{
	/* [한국어] 새로 할당해 채울 struct vblk 포인터. */
	struct vblk *vb;
	/* [한국어] 파티션 정렬 삽입 시 list_for_each() 반복자로 쓰임. */
	struct list_head *item;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!data || !ldb);

	/* [한국어] struct vblk 하나를 힙에 할당(sizeof(*vb)) - zero-fill 없음에 유의. */
	vb = kmalloc_obj(*vb);
	/* [한국어] 할당 실패. */
	if (!vb) {
		/* [한국어] 메모리 부족 치명적 로그. */
		ldm_crit ("Out of memory.");
		/* [한국어] 실패 반환. */
		return false;
	}

	/* [한국어] 방금 할당한 vb에 실제 내용을 채움 - 공통 헤더 + type별 페이로드. */
	if (!ldm_parse_vblk (data, len, vb)) {
		/* [한국어] 파싱 실패 시 애초에 쓸모없어진 vb를 즉시 해제 - 메모리 누수 방지. */
		kfree(vb);
		/* [한국어] 실패 반환(원인은 ldm_parse_vblk() 내부에서 이미 로그됨). */
		return false;			/* Already logged */
	}

	/* [한국어] 이제부터 vb->type에 따라 올바른 리스트에 연결. */
	/* Put vblk into the correct list. */
	/* [한국어] type을 디스패치 키로 쓰는 switch문. */
	switch (vb->type) {
		/* [한국어] Disk Group(v3 또는 v4)이면. */
	case VBLK_DGR3:
	case VBLK_DGR4:
			/* [한국어] v_dgrp 리스트 끝에 추가(정렬 불필요). */
		list_add (&vb->list, &ldb->v_dgrp);
			/* [한국어] 이 case 종료. */
		break;
		/* [한국어] Disk(v3 또는 v4)이면. */
	case VBLK_DSK3:
	case VBLK_DSK4:
			/* [한국어] v_disk 리스트 끝에 추가. */
		list_add (&vb->list, &ldb->v_disk);
			/* [한국어] 이 case 종료. */
		break;
		/* [한국어] Volume이면. */
	case VBLK_VOL5:
			/* [한국어] v_volu 리스트 끝에 추가. */
		list_add (&vb->list, &ldb->v_volu);
			/* [한국어] 이 case 종료. */
		break;
		/* [한국어] Component면. */
	case VBLK_CMP3:
			/* [한국어] v_comp 리스트 끝에 추가. */
		list_add (&vb->list, &ldb->v_comp);
			/* [한국어] 이 case 종료. */
		break;
		/* [한국어] Partition이면 - 유일하게 정렬 삽입이 필요한 타입. */
	case VBLK_PRT3:
			/* [한국어] 원문 주석: 파티션의 시작 섹터 기준으로 정렬해 삽입한다. */
		/* Sort by the partition's start sector. */
			/* [한국어] v_part 리스트를 처음부터 순회하며 삽입 위치를 탐색. */
		list_for_each (item, &ldb->v_part) {
				/* [한국어] list_head로부터 감싸는 struct vblk 전체를 얻음. */
			struct vblk *v = list_entry (item, struct vblk, list);
				/* [한국어] 같은 물리 디스크에 속한 파티션이면서(disk_id 일치), 기존 항목의 시작 섹터가 지금 삽입할 항목보다 더 크면(정렬 위치를 지나쳤으면) 여기가 삽입할 자리. */
			if ((v->vblk.part.disk_id == vb->vblk.part.disk_id) &&
					/* [한국어] 조건(disk_id 일치 && start 비교)의 나머지 절(줄바꿈된 계속). */
			    (v->vblk.part.start > vb->vblk.part.start)) {
					/* [한국어] 찾은 위치(v) 바로 앞에 새 항목을 끼워 넣음 - list_add_tail(new, v)는 v 바로 앞에 new를 넣는 것과 동등(list_add_tail의 두 번째 인자가 "뒤에 붙일 대상 리스트"가 아니라 "바로 앞에 삽입할 기준 노드"로 쓰이는 관용적 패턴). */
				list_add_tail (&vb->list, &v->list);
					/* [한국어] 삽입 완료 후 즉시 true 반환 - 이 case의 나머지(break 등)를 거치지 않고 함수 자체를 빠져나감. */
				return true;
			}
		}
			/* [한국어] 리스트 전체를 순회했지만 삽입할 위치를 못 찾음(같은 디스크에 이 항목보다 시작 섹터가 큰 기존 항목이 없음, 즉 이 항목이 그 디스크에서 가장 뒤에 온다는 뜻) - 리스트 맨 끝에 추가. */
		list_add_tail (&vb->list, &ldb->v_part);
			/* [한국어] 이 case 종료. */
		break;
	}
	/* [한국어] (Partition의 즉시 반환 경로를 제외한) 나머지 모든 타입은 여기서 성공 반환. */
	return true;
}

/*
 * [한국어]
 * ldm_frag_add() - 조각난 VBLK 레코드 하나를 struct frag 리스트에 누적
 *
 * @data:  이번에 도착한 조각(하나의 512B급 레코드 슬롯) 원시 바이트
 * @size:  @data 크기(= vm.vblk_size, 이 VBLK 타입의 온디스크 레코드 크기)
 * @frags: ldm_get_vblks()의 지역 struct frag 리스트(같은 group의 조각들이 여기 모임)
 * @return: true  - 조각을 성공적으로 리스트에 반영(아직 그룹이 완성되지 않았을 수도 있음)
 *          false - 크기/개수/순번 검증 실패, 또는 메모리 부족
 *
 * VBLK 하나가 여러 512B 레코드 슬롯에 걸쳐 저장된 경우(원문 주석: 조각들이 데이터베이스 안에서
 * 연속적이지 않을 수 있어 나중에 조립할 수 있도록 리스트에 둔다), 이 함수가 조각을 하나씩 받아
 * group(어느 VBLK에 속하는지), rec(몇 번째 조각인지), num(총 조각 수, 1~4 범위) 값을 검사한 뒤,
 * 같은 group ID를 가진 기존 struct frag를 찾거나(list_for_each) 없으면 새로 할당해
 * (kmalloc(sizeof(*f) + size*num, ...)로 재조립 버퍼까지 한 번에 확보) 리스트에 추가한다. 이후
 * f->map 비트맵으로 중복 조각을 걸러내고(중복이면 f->map &= 0x7F로 그룹을 영구 무효화), 정상
 * 조각이면 f->map에 비트를 세팅한 뒤 VBLK_SIZE_HEAD(16바이트) 공통 헤더 이후의 페이로드를
 * f->data의 해당 위치(rec 순번에 맞춘 오프셋)에 memcpy()한다. 첫 조각(rec==0)만 공통 헤더
 * 16바이트 전체를 f->data 맨 앞에 복사한다.
 *
 * 호출 체인:
 *   ldm_get_vblks -> [ldm_frag_add]
 */
/**
 * ldm_frag_add - Add a VBLK fragment to a list
 * @data:   Raw fragment to be added to the list
 * @size:   Size of the raw fragment
 * @frags:  Linked list of VBLK fragments
 *
 * Fragmented VBLKs may not be consecutive in the database, so they are placed
 * in a list so they can be pieced together later.
 *
 * Return:  'true'   Success, the VBLK was added to the list
 *          'false'  Error, a problem occurred
 */
/* [한국어] data/size: 이번 조각과 그 크기, frags: 같은 그룹 조각들을 모으는 지역 리스트. */
static bool ldm_frag_add (const u8 *data, int size, struct list_head *frags)
{
	/* [한국어] 같은 group을 찾거나 새로 만든 struct frag - found: 레이블 이후에도 계속 쓰인다. */
	struct frag *f;
	/* [한국어] list_for_each() 반복자. */
	struct list_head *item;
	/* [한국어] 이번 조각의 rec(순번)/num(총 개수)/group(그룹 ID) - 온디스크 값에서 읽음. */
	int rec, num, group;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!data || !frags);

	/* [한국어] 조각 하나의 크기가 최소한 공통 헤더 두 배(2 * VBLK_SIZE_HEAD)는 돼야 한다는 최소 크기 검사 - 그보다 작으면 헤더+페이로드를 담을 수조차 없는 비정상 크기. */
	if (size < 2 * VBLK_SIZE_HEAD) {
		/* [한국어] 크기 이상 로그. */
		ldm_error("Value of size is too small.");
		/* [한국어] 실패 반환. */
		return false;
	}

	/* [한국어] data+0x08에서 빅엔디안 32비트로 이 조각이 속한 VBLK의 그룹 ID를 읽음. */
	group = get_unaligned_be32(data + 0x08);
	/* [한국어] data+0x0C에서 빅엔디안 16비트로 이 조각의 순번(rec)을 읽음. */
	rec   = get_unaligned_be16(data + 0x0C);
	/* [한국어] data+0x0E에서 빅엔디안 16비트로 이 VBLK의 총 조각 개수(num)를 읽음. */
	num   = get_unaligned_be16(data + 0x0E);
	/* [한국어] num이 1 미만이거나 4 초과면 비정상(원문 상수 그대로 1~4만 유효 범위로 간주). */
	if ((num < 1) || (num > 4)) {
		/* [한국어] 몇 개라고 주장했는지 로그로 남김. */
		ldm_error ("A VBLK claims to have %d parts.", num);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] 이번 조각의 순번(rec)이 총 개수(num) 이상이면 범위를 벗어난 순번 - 예를 들어 num=2인데 rec=2 이상은 있을 수 없음(0-indexed이므로 유효 범위는 0..num-1). */
	if (rec >= num) {
		/* [한국어] rec와 num 값을 함께 로그로 남김. */
		ldm_error("REC value (%d) exceeds NUM value (%d)", rec, num);
		/* [한국어] 실패 반환. */
		return false;
	}

	/* [한국어] 기존에 이 group ID로 등록된 struct frag가 있는지 리스트 전체를 순회하며 탐색. */
	list_for_each (item, frags) {
		/* [한국어] list_head로부터 감싸는 struct frag 전체를 얻음. */
		f = list_entry (item, struct frag, list);
		/* [한국어] group ID가 일치하는 기존 그룹을 찾으면. */
		if (f->group == group)
			/* [한국어] found 레이블로 점프해 신규 할당을 건너뛰고 기존 f를 재사용. */
			goto found;
	}

	/* [한국어] 여기 도달했다면 처음 보는 group - 재조립 버퍼(f->data[])까지 포함해 한 번에 할당: 구조체 자체 크기 + (조각 하나 크기 * 총 조각 수). */
	f = kmalloc (sizeof (*f) + size*num, GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!f) {
		/* [한국어] 메모리 부족 치명적 로그. */
		ldm_crit ("Out of memory.");
		/* [한국어] 실패 반환. */
		return false;
	}

	/* [한국어] 새 그룹의 group ID 기록. */
	f->group = group;
	/* [한국어] 새 그룹의 총 조각 개수 기록. */
	f->num   = num;
	/* [한국어] 새 그룹을 처음 만든 이 조각의 rec 값을 기록 - 앞서 ldm.h 주석에서 확인했듯 이후 이 필드를 다시 읽는 코드는 없어 사실상 write-only(죽은) 필드다. */
	f->rec   = rec;
	/* [한국어] 완성 판정 비트맵 초기화: 0xFF에서 하위 num비트를 왼쪽으로 밀어(즉 하위 num비트만 0) - 이 num개 비트가 모두 OR로 채워졌을 때만 f->map이 정확히 0xFF가 되도록 설계됨. */
	f->map   = 0xFF << num;

	/* [한국어] 새로 만든 struct frag를 frags 리스트 끝에 연결. */
	list_add_tail (&f->list, frags);
	/* [한국어] found: 레이블 - 기존 그룹을 찾은 경우 위 신규 생성 블록을 건너뛰고 여기로 옴. */
found:
	/* [한국어] 이번 조각의 rec이 그룹의 num(f->num, 그룹 생성 시 정한 총 개수)을 넘으면 비정상 - 같은 group ID인데 num이 서로 다른 값으로 주장되는 손상 상황. */
	if (rec >= f->num) {
		/* [한국어] rec와 f->num을 함께 로그로 남김. */
		ldm_error("REC value (%d) exceeds NUM value (%d)", rec, f->num);
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] f->map의 rec번째 비트가 이미 1이면(이 조각이 이미 도착한 적이 있으면) 중복. */
	if (f->map & (1 << rec)) {
		/* [한국어] 중복 조각 발견 에러 로그. */
		ldm_error ("Duplicate VBLK, part %d.", rec);
		/* [한국어] 최상위 비트(0x80)를 강제로 꺼서 이후 이 그룹은 모든 하위 비트가 채워져도 0xFF가 될 수 없게(영구 불량 그룹으로) 만듦. */
		f->map &= 0x7F;			/* Mark the group as broken */
		/* [한국어] 실패 반환. */
		return false;
	}
	/* [한국어] 정상 조각이면 해당 rec 비트를 세팅. */
	f->map |= (1 << rec);
	/* [한국어] rec==0(맨 첫 조각)인 경우에만. */
	if (!rec)
		/* [한국어] 공통 헤더 16바이트(VBLK_SIZE_HEAD) 전체를 f->data 맨 앞에 복사 - 이 헤더는 조각마다 반복되지 않고 딱 한 번만 재조립 결과에 반영된다. */
		memcpy(f->data, data, VBLK_SIZE_HEAD);
	/* [한국어] data 포인터를 헤더 다음(페이로드 시작)으로 전진. */
	data += VBLK_SIZE_HEAD;
	/* [한국어] size에서도 헤더 크기를 빼 이제부터는 순수 페이로드 크기만 의미하도록 갱신. */
	size -= VBLK_SIZE_HEAD;
	/* [한국어] 이 조각의 페이로드(size 바이트)를 f->data의 "헤더 다음 + rec번째 슬롯" 위치에 복사 - 결과적으로 f->data는 [헤더][조각0 페이로드][조각1 페이로드]...] 순서로 채워진다. */
	memcpy(f->data + VBLK_SIZE_HEAD + rec * size, data, size);
	/* [한국어] 이번 조각 처리 성공. */
	return true;
}

/*
 * [한국어]
 * ldm_frag_free() - struct frag 연결 리스트 전체를 해제
 *
 * @list: 해제할 struct frag 리스트의 헤드
 * @return: 없음 (void)
 *
 * ldm_get_vblks()가 성공/실패 여부와 무관하게 반드시 호출하는 정리 함수로, list_for_each_safe()
 * (순회 중 현재 노드를 kfree()해도 안전한 변형)로 각 struct frag를 kfree()한다. 원문 주석대로
 * 단순히 리스트를 순회하며 해제하는 것 외에 다른 부작용은 없다.
 *
 * 호출 체인:
 *   ldm_get_vblks -> [ldm_frag_free]
 */
/**
 * ldm_frag_free - Free a linked list of VBLK fragments
 * @list:  Linked list of fragments
 *
 * Free a linked list of VBLK fragments
 *
 * Return:  none
 */
/* [한국어] list: 해제할 struct frag 리스트 헤드. */
static void ldm_frag_free (struct list_head *list)
{
	/* [한국어] item: 현재 노드, tmp: list_for_each_safe()가 현재 노드 해제 후에도 다음 노드를 안전하게 찾기 위해 미리 저장해 두는 다음 포인터. */
	struct list_head *item, *tmp;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!list);

	/* [한국어] _safe 변형 순회 - 순회 도중 현재 노드를 free()하므로 일반 list_for_each()를 쓰면 안 되고 반드시 이 변형을 써야 한다(안 그러면 해제된 메모리의 next 포인터를 읽게 됨). */
	list_for_each_safe (item, tmp, list)
		/* [한국어] list_head로부터 struct frag 전체를 얻어 즉시 해제. */
		kfree (list_entry (item, struct frag, list));
}

/*
 * [한국어]
 * ldm_frag_commit() - 완성된(모든 조각이 도착한) 조각 그룹들을 데이터베이스에 반영
 *
 * @frags: ldm_frag_add()가 채워온 struct frag 리스트
 * @ldb:   조각을 재조립한 VBLK를 최종적으로 추가할 in-memory 데이터베이스 캐시
 * @return: true  - 모든 그룹이 완성 상태였고 전부 성공적으로 추가됨
 *          false - 하나라도 미완성 그룹이 있거나 ldm_ldmdb_add()가 실패
 *
 * ldm_get_vblks()가 VBLK 테이블 전체를 다 읽은 뒤 마지막에 한 번 호출하는 마무리 함수다.
 * frags 리스트의 각 struct frag에 대해 f->map이 정확히 0xFF(모든 조각이 도착했고 중복도
 * 없었음)인지 확인하고, 하나라도 미완성이면 즉시 실패를 반환한다(원문 주석: "이제 모든 조각난
 * VBLK가 모였으니 나중에 쓸 수 있도록 데이터베이스에 추가해야 한다"). 완성된 그룹은
 * f->num*ldb->vm.vblk_size(재조립된 전체 바이트 수)를 크기로 ldm_ldmdb_add()에 넘겨 최종
 * 파싱/분류시킨다.
 *
 * 호출 체인:
 *   ldm_get_vblks -> [ldm_frag_commit] -> ldm_ldmdb_add
 */
/**
 * ldm_frag_commit - Validate fragmented VBLKs and add them to the database
 * @frags:  Linked list of VBLK fragments
 * @ldb:    Cache of the database structures
 *
 * Now that all the fragmented VBLKs have been collected, they must be added to
 * the database for later use.
 *
 * Return:  'true'   All the fragments we added successfully
 *          'false'  One or more of the fragments we invalid
 */
/* [한국어] frags: 조각 리스트, ldb: 최종 추가 대상 데이터베이스 캐시. */
static bool ldm_frag_commit (struct list_head *frags, struct ldmdb *ldb)
{
	/* [한국어] 순회 중 현재 그룹. */
	struct frag *f;
	/* [한국어] list_for_each() 반복자. */
	struct list_head *item;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!frags || !ldb);

	/* [한국어] frags 리스트의 모든 그룹을 순회(이번엔 해제하지 않으므로 _safe 불필요). */
	list_for_each (item, frags) {
		/* [한국어] list_head로부터 struct frag 전체를 얻음. */
		f = list_entry (item, struct frag, list);

		/* [한국어] 완성 판정: f->map이 정확히 0xFF가 아니면(일부 조각 누락, 또는 중복으로 인해 최상위 비트가 강제로 꺼진 영구 불량 그룹) 미완성. */
		if (f->map != 0xFF) {
			/* [한국어] 어느 그룹(f->group)이 얼마나 채워졌는지(f->map)를 16진수로 로그. */
			ldm_error ("VBLK group %d is incomplete (0x%02x).",
				/* [한국어] ldm_error()의 나머지 인자(줄바꿈된 계속). */
				f->group, f->map);
			/* [한국어] 하나라도 미완성이면 전체 커밋을 실패로 처리하고 즉시 반환(나머지 그룹은 확인하지 않음). */
			return false;
		}

		/* [한국어] 완성된 그룹의 재조립 데이터(f->data)를 전체 크기(조각 수 x 레코드 크기)로 ldm_ldmdb_add()에 넘겨 실제 파싱 및 리스트 분류를 수행. */
		if (!ldm_ldmdb_add (f->data, f->num*ldb->vm.vblk_size, ldb))
			/* [한국어] 실패 시 즉시 반환(이미 ldm_ldmdb_add() 내부에서 로그됨). */
			return false;		/* Already logged */
	}
	/* [한국어] 모든 그룹이 완성 상태였고 전부 성공적으로 추가됨. */
	return true;
}

/*
 * [한국어]
 * ldm_get_vblks() - VBLK 테이블 전체를 섹터 단위로 순회하며 온디스크 VBLK를 메모리로 적재
 *
 * @state: 파티션 스캔 상태(디바이스 컨텍스트)
 * @base:  LDM 설정 데이터베이스의 디스크 절대 시작 섹터
 * @ldb:   결과를 채워 넣을 in-memory 데이터베이스 캐시(ldb->vm은 이미 채워져 있어야 함)
 * @return: true  - VBLK 테이블 전체를 성공적으로 읽고 분류함(조각난 레코드까지 재조립 포함)
 *          false - 디스크 읽기 실패, 매직 불일치, 또는 조각 커밋 실패
 *
 * 원문 주석대로 "VBLK 정보를 쓰려면 디스크에서 읽어 언팩하고 검증해야 한다"는 이 파일의 핵심
 * 루프다. vm.vblk_size(레코드 크기)로 섹터당 VBLK 개수(perbuf = 512/size)를 계산하고,
 * vm.vblk_offset(>>9로 섹터 환산, skip)부터 vm.vblk_size*vm.last_vblk_seq(>>9로 섹터 환산,
 * finish)까지 섹터를 순회한다. 각 섹터 안에서는 perbuf개의 VBLK 슬롯을 순회하며 매직(MAGIC_VBLK)
 * 을 확인하고, 레코드 헤더의 레코드 개수(recs, data+0x0E)에 따라 recs==1이면 조각나지 않은
 * 것이므로 바로 ldm_ldmdb_add()에, recs>1이면 조각난 것이므로 ldm_frag_add()의 임시 리스트에
 * 위임한다(recs==0이면 사용되지 않는 슬롯으로 그냥 건너뜀). 모든 섹터를 다 읽은 뒤에는
 * ldm_frag_commit()으로 조각 리스트를 최종 반영하고, 성공/실패와 무관하게 마지막에는 항상
 * ldm_frag_free()로 임시 리스트를 해제한다(LIST_HEAD 매크로로 스택에 선언된 지역 리스트이므로
 * 함수를 벗어나기 전에 반드시 정리해야 함).
 *
 * 호출 체인:
 *   ldm_partition -> [ldm_get_vblks] -> ldm_ldmdb_add / ldm_frag_add / ldm_frag_commit / ldm_frag_free
 */
/**
 * ldm_get_vblks - Read the on-disk database of VBLKs into memory
 * @state: Partition check state including device holding the LDM Database
 * @base:  Offset, into @state->disk, of the database
 * @ldb:   Cache of the database structures
 *
 * To use the information from the VBLKs, they need to be read from the disk,
 * unpacked and validated.  We cache them in @ldb according to their type.
 *
 * Return:  'true'   All the VBLKs were read successfully
 *          'false'  An error occurred
 */
/* [한국어] state: 디바이스 컨텍스트, base: 설정 DB 시작 절대 섹터, ldb: 결과를 담을 캐시 (ldb->vm이 미리 채워져 있어야 함). */
static bool ldm_get_vblks(struct parsed_partitions *state, unsigned long base,
			  struct ldmdb *ldb)
{
	/* [한국어] size: VBLK 레코드 크기(바이트), perbuf: 섹터당 VBLK 개수, skip/finish: 스캔할 시작/끝 섹터, s/v: 섹터/슬롯 순회 인덱스, recs: 이번 슬롯의 레코드 개수. */
	int size, perbuf, skip, finish, s, v, recs;
	/* [한국어] 현재 읽어들인 섹터 데이터 포인터 - NULL로 시작해 out 레이블에서 "아직 읽은 섹터가 있는지" 판단하는 센티널로도 쓰인다. */
	u8 *data = NULL;
	/* [한국어] read_part_sector() 핸들. */
	Sector sect;
	/* [한국어] 최종 반환값, 기본 false. */
	bool result = false;
	/* [한국어] 조각난 VBLK들을 모을 지역 리스트 헤드 - LIST_HEAD 매크로가 이 스코프 안에서 초기화된 struct list_head 변수를 선언(힙 할당이 아닌 스택 변수). */
	LIST_HEAD (frags);

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON(!state || !ldb);

	/* [한국어] VBLK 레코드 하나의 온디스크 크기(vm.vblk_size)를 지역 변수로. */
	size   = ldb->vm.vblk_size;
	/* [한국어] 섹터당 VBLK 개수 = 512 / size(정수 나눗셈, 나머지는 버림). */
	perbuf = 512 / size;
	/* [한국어] VBLK 테이블 시작 오프셋(바이트, vblk_offset)을 섹터로 환산(>>9는 /512와 동일) - 원문 인라인 주석대로 "바이트를 섹터로". */
	skip   = ldb->vm.vblk_offset >> 9;		/* Bytes to sectors */
	/* [한국어] 전체 VBLK 바이트 수(size*last_vblk_seq)를 섹터로 환산해 스캔 종료 지점을 계산. */
	finish = (size * ldb->vm.last_vblk_seq) >> 9;

	/* [한국어] skip부터 finish 직전까지, 섹터 단위로 순회(원문 인라인 주석: "각 섹터마다"). */
	for (s = skip; s < finish; s++) {		/* For each sector */
		/* [한국어] base(설정 DB 시작) + OFF_VMDB(VMDB 오프셋) + s(이번 섹터) 위치를 읽음 - VBLK 테이블이 VMDB 바로 다음 위치부터 시작하므로 같은 OFF_VMDB 기준을 그대로 쓴다. */
		data = read_part_sector(state, base + OFF_VMDB + s, &sect);
		/* [한국어] 디스크 읽기 실패. */
		if (!data) {
			/* [한국어] 치명적 오류 로그. */
			ldm_crit ("Disk read failed.");
			/* [한국어] 정리 경로(out)로 점프. */
			goto out;
		}

		/* [한국어] 이번 섹터 안의 perbuf개 VBLK 슬롯을 순회(원문 인라인 주석: "각 vblk마다") - data+=size로 매 반복 다음 슬롯으로 전진. */
		for (v = 0; v < perbuf; v++, data+=size) {  /* For each vblk */
			/* [한국어] 이 슬롯 맨 앞 4바이트가 MAGIC_VBLK("VBLK" 시그니처)와 다르면 이 위치에 VBLK가 없다는 뜻 - 데이터베이스가 손상됐거나 계산이 잘못된 것. */
			if (MAGIC_VBLK != get_unaligned_be32(data)) {
				/* [한국어] 시그니처 불일치 에러 로그. */
				ldm_error ("Expected to find a VBLK.");
				/* [한국어] 정리 경로로 점프 - 한 슬롯이라도 시그니처가 깨지면 이후 오프셋 계산도 신뢰할 수 없으므로 전체를 중단. */
				goto out;
			}

			/* [한국어] data+0x0E에서 빅엔디안 16비트로 이 VBLK가 몇 개의 레코드(조각)로 구성되는지 읽음(원문 인라인 주석: "레코드 개수"). */
			recs = get_unaligned_be16(data + 0x0E);	/* Number of records */
			/* [한국어] 정확히 1개 레코드(조각나지 않음)인 경우. */
			if (recs == 1) {
				/* [한국어] 바로 ldm_ldmdb_add()로 파싱/분류 시도. */
				if (!ldm_ldmdb_add (data, size, ldb))
					/* [한국어] 실패 시 정리 경로로 점프(이미 로그됨). */
					goto out;	/* Already logged */
			/* [한국어] 2개 이상 레코드(조각남)인 경우. */
			} else if (recs > 1) {
				/* [한국어] ldm_frag_add()로 임시 리스트에 조각을 누적(아직 전부 모이지 않았을 수 있음). */
				if (!ldm_frag_add (data, size, &frags))
					/* [한국어] 실패 시 정리 경로로 점프(이미 로그됨). */
					goto out;	/* Already logged */
			}
			/* [한국어] recs가 0인 경우(그 외 값): 원문 주석대로 사용되지 않는 레코드이므로 그냥 무시하고 다음 슬롯으로. */
			/* else Record is not in use, ignore it. */
		}
		/* [한국어] 이번 섹터의 folio 참조 반납. */
		put_dev_sector (sect);
		/* [한국어] data를 NULL로 되돌려, 다음 반복에서 새로 read_part_sector()를 호출해야 함을 명시(동시에 out 레이블의 "읽은 섹터가 남아있는지" 판단 센티널도 갱신). */
		data = NULL;
	}

	/* [한국어] 모든 섹터를 다 읽었으면, 지금까지 모인 조각들을 최종 반영(완성되지 않은 그룹이 있으면 이 호출 자체가 false를 반환) - 원문 인라인 주석: 실패는 이미 로그됨. */
	result = ldm_frag_commit (&frags, ldb);	/* Failures, already logged */
	/* [한국어] out: 정리 레이블 - 성공/실패 모두 여기로 모인다. */
out:
	/* [한국어] data가 NULL이 아니면(마지막으로 읽은 섹터를 아직 반납하지 않았다면) - 루프 중간에 goto out으로 빠져나온 경우에만 해당(정상 종료 시에는 루프 안에서 이미 매번 반납했으므로 data가 NULL). */
	if (data)
		/* [한국어] 마지막으로 읽은 섹터의 folio 참조를 반납. */
		put_dev_sector (sect);
	/* [한국어] 성공/실패와 무관하게 항상 조각 임시 리스트를 해제 - 스택 변수(LIST_HEAD)이므로 함수를 벗어나기 전에 반드시 정리해야 메모리 누수가 없다. */
	ldm_frag_free (&frags);

	/* [한국어] 최종 결과 반환. */
	return result;
}

/*
 * [한국어]
 * ldm_free_vblks() - struct vblk 연결 리스트 하나를 해제
 *
 * @lh: 해제할 struct vblk 리스트의 헤드(ldb->v_dgrp/v_disk/v_volu/v_comp/v_part 중 하나)
 * @return: 없음 (void)
 *
 * ldm_partition()이 파싱 성공/실패와 무관하게 cleanup 단계에서 다섯 리스트 각각에 대해 한 번씩
 * 호출하는 정리 함수다. ldm_frag_free()와 마찬가지로 list_for_each_safe()로 순회하며 각 struct
 * vblk를 kfree()한다 - 다만 vblk는 struct frag와 달리 union으로 여러 타입을 담고 있어도, 이
 * 구조체 자체가 하나의 연속된 메모리 블록(kmalloc_obj(*vb))이므로 kfree() 한 번으로 union 내용까지
 * 함께 해제된다(포인터를 따로 갖는 필드가 없으므로 추가 해제가 필요 없음).
 *
 * 호출 체인:
 *   ldm_partition -> [ldm_free_vblks] (v_dgrp/v_disk/v_volu/v_comp/v_part 각각에 대해 호출)
 */
/**
 * ldm_free_vblks - Free a linked list of vblk's
 * @lh:  Head of a linked list of struct vblk
 *
 * Free a list of vblk's and free the memory used to maintain the list.
 *
 * Return:  none
 */
/* [한국어] lh: 해제할 struct vblk 리스트 헤드. */
static void ldm_free_vblks (struct list_head *lh)
{
	/* [한국어] item: 현재 노드, tmp: _safe 변형이 다음 노드를 미리 저장해 두는 포인터. */
	struct list_head *item, *tmp;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON (!lh);

	/* [한국어] _safe 변형 순회(현재 노드를 free()하므로 반드시 이 변형 사용). */
	list_for_each_safe (item, tmp, lh)
		/* [한국어] list_head로부터 struct vblk 전체를 얻어 즉시 해제. */
		kfree (list_entry (item, struct vblk, list));
}


/*
 * [한국어]
 * ldm_partition() - LDM 동적 디스크를 감지하고, 맞으면 파티션들을 등록하는 유일한 외부 진입점
 *
 * @state: 파티션 스캔 상태(check.h가 정의, 대상 gendisk/parts[] 등을 담음)
 * @return: 1  - @state->disk가 동적 디스크로 확인됐고 파티션 등록까지 마침
 *          0  - @state->disk가 동적 디스크가 아님(다른 파티션 스킴이 계속 시도해야 함)
 *         -1  - 충분한 정보를 읽기 전에 오류가 발생했거나, 동적 디스크이지만 데이터베이스가
 *               손상됐을 수 있음
 *
 * 이 함수는 block/partitions/core.c의 check_partition()이 check_part[] 배열을 순회하며 호출하는
 * 여러 파티션 스킴 프로버 중 하나로 등록된다(msdos_partition보다 먼저 시도되도록 배치 - LDM DB가
 * MBR 파티션 타입 0x42로 중첩되어 있기 때문). 원문 주석대로, 더미 디바이스 1(LDM 데이터베이스
 * 자체가 차지하는 영역에 대응하는 것으로 보이나 실제로는 이 함수가 그런 더미 파티션을 직접 만들지
 * 않고, 대신 2번부터 실제 데이터 파티션을 순서대로 만든다)과 이후 실제 데이터 파티션들(hda2,
 * hda3, ...)을 만든다는 것이 원래 설계 의도다. 처리 순서: (1)
 * ldm_validate_partition_table()로 약식 판별(아니면 0으로 조기 반환), (2) struct ldmdb를
 * kmalloc_obj()로 힙에 할당, (3) ldm_validate_privheads()로 PRIVHEAD 검증, (4) 검증된
 * config_start를 base로 고정, (5) ldm_validate_tocblocks()와 ldm_validate_vmdb()로 TOCBLOCK과
 * VMDB 검증, (6) 다섯 개의 vblk 리스트를 INIT_LIST_HEAD()로 초기화, (7) ldm_get_vblks()로 VBLK
 * 테이블 전체 적재(실패 시 cleanup 경로로), (8) ldm_create_data_partitions()로 실제 파티션 등록,
 * (9) cleanup 레이블에서 다섯 리스트를 전부 ldm_free_vblks()로 해제, (10) out 레이블에서 ldb
 * 자체를 kfree()하고 결과 반환.
 *
 * 호출 체인:
 *   check_partition (block/partitions/core.c) -> [ldm_partition] -> ldm_validate_partition_table /
 *     ldm_validate_privheads / ldm_validate_tocblocks / ldm_validate_vmdb / ldm_get_vblks /
 *     ldm_create_data_partitions / ldm_free_vblks
 */
/**
 * ldm_partition - Find out whether a device is a dynamic disk and handle it
 * @state: Partition check state including device holding the LDM Database
 *
 * This determines whether the device @bdev is a dynamic disk and if so creates
 * the partitions necessary in the gendisk structure pointed to by @hd.
 *
 * We create a dummy device 1, which contains the LDM database, and then create
 * each partition described by the LDM database in sequence as devices 2+. For
 * example, if the device is hda, we would have: hda1: LDM database, hda2, hda3,
 * and so on: the actual data containing partitions.
 *
 * Return:  1 Success, @state->disk is a dynamic disk and we handled it
 *          0 Success, @state->disk is not a dynamic disk
 *         -1 An error occurred before enough information had been read
 *            Or @state->disk is a dynamic disk, but it may be corrupted
 */
/* [한국어] state: check.h가 정의하는 파티션 스캔 상태(check_partition()이 넘겨줌). */
int ldm_partition(struct parsed_partitions *state)
{
	/* [한국어] 파싱 전 과정 동안 유지할 in-memory 데이터베이스 캐시(힙에 할당). */
	struct ldmdb  *ldb;
	/* [한국어] 검증된 config_start를 담아 이후 모든 상대 오프셋 계산의 기준으로 삼는 변수. */
	unsigned long base;
	/* [한국어] 최종 반환값 - 기본값 -1(오류/불충분한 정보)로 시작. */
	int result = -1;

	/* [한국어] 널 포인터 방어적 단언. */
	BUG_ON(!state);

	/* [한국어] 원문 주석: 동적 디스크의 흔적이 있는지 먼저 약식으로 확인. */
	/* Look for signs of a Dynamic Disk */
	/* [한국어] MBR 안에 타입 0x42(LDM_PARTITION) 파티션이 없으면. */
	if (!ldm_validate_partition_table(state))
		/* [한국어] 즉시 0 반환 - "동적 디스크가 아님"을 뜻하며, 별도 정리(ldb 할당 전이므로 정리할 것도 없음)나 로그 없이 다른 파티션 스킴이 시도하도록 조용히 넘어간다. */
		return 0;

	/* [한국어] 여기부터는 동적 디스크일 가능성이 있으므로 in-memory 데이터베이스 캐시를 힙에 할당. */
	ldb = kmalloc_obj(*ldb);
	/* [한국어] 할당 실패. */
	if (!ldb) {
		/* [한국어] 메모리 부족 치명적 로그. */
		ldm_crit ("Out of memory.");
		/* [한국어] out으로 점프(ldb가 NULL이므로 kfree(NULL)은 안전) - result는 -1 그대로. */
		goto out;
	}

	/* [한국어] 원문 주석: PRIVHEAD를 파싱하고 검증. */
	/* Parse and check privheads. */
	/* [한국어] PRIVHEAD 3중 사본을 읽어 검증하고 ldb->ph에 대표값을 채움. */
	if (!ldm_validate_privheads(state, &ldb->ph))
		/* [한국어] 실패 시 out으로 점프(원인은 이미 로그됨) - result는 -1(불충분한 정보/오류) 그대로. */
		goto out;		/* Already logged */

	/* [한국어] 원문 주석: 이후 모든 참조는 base(데이터베이스 시작)를 기준으로 한 상대값. */
	/* All further references are relative to base (database start). */
	/* [한국어] 검증된 PRIVHEAD의 config_start를 base로 고정 - 이후 OFF_TOCB 계열 및 OFF_VMDB 계산의 유일한 기준점. */
	base = ldb->ph.config_start;

	/* [한국어] 원문 주석: toc와 vmdb를 파싱하고 검사. */
	/* Parse and check tocs and vmdb. */
	/* [한국어] TOCBLOCK 최대 4중 사본 검증과 VMDB 검증을 순서대로 시도(둘 다 성공해야 다음 단계로 진행 - && 단축 평가로 TOCBLOCK이 실패하면 VMDB 검증은 시도조차 되지 않음). */
	if (!ldm_validate_tocblocks(state, base, ldb) ||
	/* [한국어] (조건은 위 줄에서 이미 평가, 두 함수 호출을 이어붙임) */
	    !ldm_validate_vmdb(state, base, ldb))
		/* [한국어] 둘 중 하나라도 실패하면 out으로 점프(이미 로그됨) - 아직 vblk 리스트를 초기화하지 않았으므로 cleanup이 아니라 out으로 바로 감. */
	    	goto out;		/* Already logged */

	/* [한국어] 원문 주석: ldmdb 구조체 안의 vblk 리스트들을 초기화. */
	/* Initialize vblk lists in ldmdb struct */
	/* [한국어] Disk Group 리스트 초기화(빈 리스트로 - 자기 자신을 가리키는 head 상태). */
	INIT_LIST_HEAD (&ldb->v_dgrp);
	/* [한국어] Disk 리스트 초기화. */
	INIT_LIST_HEAD (&ldb->v_disk);
	/* [한국어] Volume 리스트 초기화. */
	INIT_LIST_HEAD (&ldb->v_volu);
	/* [한국어] Component 리스트 초기화. */
	INIT_LIST_HEAD (&ldb->v_comp);
	/* [한국어] Partition 리스트 초기화 - 이 다섯 줄 이후부터 ldm_ldmdb_add()가 안전하게 각 리스트에 연결할 수 있다(초기화 전에 list_add()를 호출하면 정의되지 않은 동작). */
	INIT_LIST_HEAD (&ldb->v_part);

	/* [한국어] VBLK 테이블 전체를 읽어 다섯 리스트에 분류(조각 재조립 포함). */
	if (!ldm_get_vblks(state, base, ldb)) {
		/* [한국어] 실패 치명적 로그. */
		ldm_crit ("Failed to read the VBLKs from the database.");
		/* [한국어] 이 시점부터는 리스트가 이미 초기화(및 일부 채워짐)됐으므로 out이 아니라 cleanup으로 점프해 다섯 리스트를 모두 해제해야 한다(메모리 누수 방지). */
		goto cleanup;
	}

	/* [한국어] 원문 주석: 마지막으로 실제 데이터 파티션 디바이스들을 생성. */
	/* Finally, create the data partition devices. */
	/* [한국어] v_disk에서 현재 디스크를 찾고 v_part를 필터링해 put_partition()으로 등록. */
	if (ldm_create_data_partitions(state, ldb)) {
		/* [한국어] 성공 디버그 로그. */
		ldm_debug ("Parsed LDM database successfully.");
		/* [한국어] 최종 성공(1)으로 전환. */
		result = 1;
	}
	/* [한국어] 실패한 경우 별도 처리 없음 - 원문 주석: 이미 로그됨(ldm_create_data_partitions() 내부에서 실패 원인이 남는다는 뜻이 아니라, 이 함수 자체가 필터링 결과 파티션이 하나도 없어도 true를 반환할 수 있으므로 사실상 이 else 분기는 거의 타지 않음). */
	/* else Already logged */

	/* [한국어] cleanup: 레이블 - VBLK 읽기 이후 실패 경로와 정상 종료 경로가 모두 여기로 모여 다섯 리스트를 해제한다. */
cleanup:
	/* [한국어] Disk Group 리스트 해제. */
	ldm_free_vblks (&ldb->v_dgrp);
	/* [한국어] Disk 리스트 해제. */
	ldm_free_vblks (&ldb->v_disk);
	/* [한국어] Volume 리스트 해제. */
	ldm_free_vblks (&ldb->v_volu);
	/* [한국어] Component 리스트 해제. */
	ldm_free_vblks (&ldb->v_comp);
	/* [한국어] Partition 리스트 해제. */
	ldm_free_vblks (&ldb->v_part);
	/* [한국어] out: 정리 레이블 - PRIVHEAD/TOCBLOCK/VMDB 검증 실패 경로(리스트 초기화 전)와 cleanup을 거친 정상/VBLK실패 경로가 모두 최종적으로 여기서 합류한다. */
out:
	/* [한국어] ldb 자체를 해제(NULL이어도 kfree(NULL)은 안전 - 맨 위 할당 실패 경로도 여기로 옴). */
	kfree (ldb);
	/* [한국어] 최종 결과(1/0/-1)를 check_partition()에 반환. */
	return result;
}
