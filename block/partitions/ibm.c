// SPDX-License-Identifier: GPL-2.0
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 *                  Volker Sameske <sameske@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2012
 */
/*
 * [한국어 설명] IBM DASD(S/390) VTOC 파티션 파서 (ibm.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 블록 계층의 파티션 테이블 감지 프레임워크
 * (block/partitions/*.c) 중 하나로, IBM 메인프레임(S/390, zSeries) DASD
 * (Direct Access Storage Device) 디스크에 기록된 볼륨 레이블(VOL1/LNX1/CMS1)과
 * VTOC(Volume Table Of Contents)를 해석해 파티션 경계를 찾아낸다. DASD는
 * 오래된 메인프레임 CHR(Cylinder-Head-Record) 주소 체계와 EBCDIC 문자
 * 인코딩을 사용하므로, 이 파일은 CCHH/CCHHB 주소를 리눅스식 선형 블록
 * 번호로 변환하고 EBCDIC 문자열을 ASCII로 변환하는 레거시 호환 계층 역할을
 * 겸한다. CDL(Compatible Disk Layout, z/OS와 호환되는 VTOC 기반 다중
 * 데이터셋 레이아웃), LDL(Linux Disk Layout, 단일 대형 파티션의 리눅스
 * 전용 레이아웃), CMS(VM/CMS 미니디스크 레이아웃) 세 가지 포맷을 모두
 * 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인 (위 → 아래):
 *   블록 장치 등록/재스캔: device_add_disk() 또는 BLKRRPART ioctl
 *     → bdev_disk_changed() → blk_add_partitions()      ← block/partitions/core.c
 *     → check_partition()                                ← block/partitions/check.c
 *     → check_part[] 테이블 순회 → ibm_partition()        ← 이 파일 (엔트리 포인트)
 *     → find_label() → find_vol1_partitions()/find_lnx1_partitions()/
 *       find_cms1_partitions()                            ← 이 파일 내부 디스패치
 *     → put_partition()                                   ← block/partitions/core.c
 *   check_partition()은 ibm_partition()이 0을 반환하면 다음 파서(msdos, gpt 등)로
 *   넘어가고, 1 이상을 반환하면 이 파서가 파티션을 확정한 것으로 간주한다.
 *   실행 컨텍스트: 커널 유저스페이스(태스크 컨텍스트) — 디스크 attach 시
 *   또는 사용자가 blockdev --rereadpt/파티션 재스캔을 요청했을 때 동기적으로
 *   실행되며, 인터럽트 컨텍스트에서는 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/partitions/check.h  : struct parsed_partitions, read_part_sector(),
 *                                 put_dev_sector(), put_partition() 등 공용 헬퍼.
 *   - asm/vtoc.h (s390)         : struct vtoc_cchh/vtoc_cchhb/vtoc_volume_label_cdl/
 *                                 vtoc_volume_label_ldl/vtoc_cms_label/vtoc_format1_label
 *                                 등 DASD 온디스크 레이아웃 구조체 정의.
 *   - asm/ebcdic.h (s390)      : EBCASC()/_ascebc[] — EBCDIC ↔ ASCII 변환 테이블.
 *   - asm/dasd.h (s390)        : dasd_information2_t — DASD 드라이버가 제공하는
 *                                 확장 장치 정보(CU/디바이스 타입, 포맷 등).
 *   - linux/dasd_mod.h          : dasd_biodasdinfo 심볼 선언 — s390 dasd 드라이버가
 *                                 export하는 콜백을 symbol_get()으로 동적 참조.
 *   - drivers/s390/block/dasd*.c: 실제 DASD 디바이스 드라이버(이 트리에는 없음).
 *                                 disk->fops->getgeo와 dasd_biodasdinfo를 구현.
 * 데이터 흐름:
 *   물리 DASD 트랙(EBCDIC 레이블/DSCB) → read_part_sector()로 섹터 버퍼 확보
 *   → union label_t로 오버레이 → EBCASC() 변환 → get_label_by_type()으로 포맷 판별
 *   → 포맷별 find_*_partitions()가 오프셋/크기를 sector_t 단위로 계산
 *   → put_partition()이 struct parsed_partitions에 등록 → 이후 제네릭 블록 계층이
 *   해당 영역을 파티션 block_device로 노출.
 * 공유 핵심 자료구조:
 *   struct parsed_partitions : 파티션 스캔 진행 상태(limit, pp_buf 등) — 이 파일은
 *   상태를 소비만 하고 정의는 block/partitions/check.h에 있다.
 *   union label_t             : 이 파일에 정의된 3-way 오버레이 버퍼(§구조체 참고).
 *   dasd_information2_t       : dasd 드라이버가 채워주는 장치 메타데이터 — 이 파일은
 *   포인터로만 참조하며 정의는 s390 전용 헤더에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * ibm_partition()          : 이 파일의 진입점. geo/label/info 버퍼를 할당하고
 *                            레이블 탐색 후 포맷별 파서로 분기, 실패 시 단계적으로
 *                            자원을 해제하는 goto 기반 정리 루틴을 갖는다.
 * find_label()             : VOL1/LNX1/CMS1 레이블이 있을 법한 섹터를 후보군에서
 *                            순차 탐색하고 EBCDIC 볼륨 시리얼을 ASCII로 변환한다.
 * find_vol1_partitions()   : VTOC의 format-1/format-8 DSCB를 순회하며 각 데이터셋의
 *                            첫 번째 extent를 파티션으로 등록한다(CDL 레이아웃).
 * find_lnx1_partitions()   : LNX1 레이블 기반 단일 대형 파티션 크기를 계산한다
 *                            (LDL 레이아웃).
 * find_cms1_partitions()   : CMS1 레이블 기반 VM 미니디스크 파티션을 계산한다.
 * cchh2blk()/cchhb2blk()   : CCHH(실린더-헤드)/CCHHB(+블록) 메인프레임 주소를
 *                            hd_geometry 기준 선형 블록 번호로 변환한다.
 * union label_t            : VOL1/LNX1/CMS1 세 레이블 포맷을 겹쳐 담는 스크래치
 *                            버퍼(§구조체 주석 참고).
 */

#include <linux/buffer_head.h>	/* [한국어] 섹터 버퍼(struct buffer_head) 관련 인터페이스 - read_part_sector()가 내부적으로 의존하는 버퍼 캐시 계층 */
#include <linux/hdreg.h>	/* [한국어] struct hd_geometry(cylinders/heads/sectors/start) 정의 - disk->fops->getgeo()가 채우는 CHS 지오메트리 타입 */
#include <linux/slab.h>	/* [한국어] kmalloc/kfree 등 슬랩 할당자 인터페이스 - info/geo/label 버퍼 할당·해제에 사용 */
#include <asm/dasd.h>	/* [한국어] dasd_information2_t 등 s390 DASD 전용 확장 장치 정보 구조체 정의(cu_type/dev_type/label_block/format 등) */
#include <asm/ebcdic.h>	/* [한국어] EBCASC() 매크로 및 _ascebc[] 테이블 정의 - EBCDIC ↔ ASCII 문자 인코딩 변환에 사용 */
#include <linux/uaccess.h>	/* [한국어] 유저 공간 접근 헬퍼(get_user/put_user 등) 정의 헤더 */
#include <asm/vtoc.h>	/* [한국어] struct vtoc_cchh/vtoc_cchhb/vtoc_volume_label_cdl/vtoc_volume_label_ldl/vtoc_cms_label/vtoc_format1_label 등 DASD 온디스크 레이블·DSCB 구조체 정의 */
#include <linux/module.h>	/* [한국어] 커널 모듈 매크로(symbol_get()/symbol_put() 포함) - dasd 드라이버가 모듈로 빌드된 경우 동적 심볼 참조에 필요 */
#include <linux/dasd_mod.h>	/* [한국어] dasd_biodasdinfo 함수 포인터 심볼 선언 - symbol_get(dasd_biodasdinfo)로 이 파일이 참조 */

#include "check.h"	/* [한국어] struct parsed_partitions, read_part_sector(), put_dev_sector(), put_partition() 등 파티션 파서 공용 인터페이스(block/partitions/check.h) */

/*
 * [한국어]
 * union label_t - VOL1/LNX1/CMS1 세 가지 DASD 볼륨 레이블 포맷을 겹쳐 담는
 * 스크래치 버퍼.
 *
 * DASD 볼륨의 실제 포맷(CDL/LDL/CMS)은 섹터를 읽어 매직 문자열(EBCDIC "VOL1"/
 * "LNX1"/"CMS1")을 확인하기 전까지는 알 수 없다. 세 포맷의 온디스크 레이블
 * 구조체 크기가 서로 다르므로, find_label()은 일단 섹터 전체를 가장 큰
 * 멤버 크기만큼 이 union에 복사한 뒤, 판별된 타입에 맞는 멤버로 재해석한다.
 * C 표준의 유니온 규칙에 따라 세 멤버는 동일한 메모리 주소에서 시작하므로
 * memcpy 한 번으로 세 가지 해석이 모두 가능해진다.
 */
union label_t {
	struct vtoc_volume_label_cdl vol;
	/* [한국어] CDL(Compatible Disk Layout, z/OS 호환) 볼륨 레이블 뷰.
	 * 온디스크 VOL1 레이블 섹터를 이 구조체로 해석하면 volid(볼륨 시리얼),
	 * vtoc(VTOC 시작 위치를 가리키는 CCHHB), 보안 바이트 등을 얻을 수 있다.
	 * 설정자: find_label()이 read_part_sector()로 읽은 섹터 전체를
	 * memcpy(label, data, sizeof(*label))로 복사할 때 이 뷰로도 함께 채워진다.
	 * 읽는 자: find_label()이 label->vol.volid를 볼륨 시리얼 추출에 사용하고,
	 * find_vol1_partitions()가 label->vol.vtoc를 cchhb2blk()에 전달해 VTOC
	 * 시작 블록을 계산한다.
	 * 값 범위: 온디스크 값은 EBCDIC 인코딩이므로, volid 등 문자열 필드는
	 * EBCASC()로 변환하기 전까지 EBCDIC 바이트로 남아 있다.
	 * 동기화: 파티션 스캔은 단일 스레드(디스크 add/rescan 경로)에서 순차
	 * 실행되므로 별도 락 없이 지역 변수처럼 사용된다. */
	struct vtoc_volume_label_ldl lnx;
	/* [한국어] LDL(Linux Disk Layout) 볼륨 레이블 뷰.
	 * volid(볼륨 시리얼), ldl_version(레이블 버전 바이트), formatted_blocks
	 * (대형 볼륨 지원 시 전체 포맷된 블록 수, 64비트) 필드를 제공한다.
	 * 설정자: find_label()의 memcpy(label, data, sizeof(*label))로 동일 섹터를
	 * 다시 해석한 것 - vol과 물리적으로 같은 메모리를 공유(union)한다.
	 * 읽는 자: find_lnx1_partitions()가 ldl_version으로 대형 볼륨 지원 여부를
	 * 판단하고, formatted_blocks로 파티션 크기를 산출한다.
	 * 값 범위: ldl_version == 0xf2 이면 formatted_blocks가 유효한 대형 볼륨
	 * 포맷; 그 외 값이면 formatted_blocks를 신뢰할 수 없어 CHS 지오메트리
	 * 기반 계산으로 대체한다.
	 * 동기화: vol과 동일 - 단일 스레드 스캔 경로에서만 접근. */
	struct vtoc_cms_label cms;
	/* [한국어] VM/CMS 미니디스크 레이블 뷰.
	 * block_size(이 디스크가 실제로 포맷된 블록 크기), disk_offset(예약된
	 * 미니디스크일 때 실 데이터 시작 오프셋), block_count(전체 CMS 블록 수)
	 * 필드를 제공한다.
	 * 설정자: find_label()의 공통 memcpy(label, data, sizeof(*label)).
	 * 읽는 자: find_cms1_partitions()가 세 필드를 모두 사용해 파티션의
	 * 시작 블록과 크기를 계산하며, blocksize 지역 변수를 label->cms.block_size
	 * 로 덮어써 이후 secperblk 계산 기준을 CMS 레이블 값으로 바꾼다.
	 * 값 범위: disk_offset != 0 이면 "예약된 미니디스크(MDSK)"로 취급된다.
	 * 동기화: vol/lnx와 동일 - 단일 스레드 스캔 경로에서만 접근. */
};

/*
 * compute the block number from a
 * cyl-cyl-head-head structure
 */
/*
 * [한국어]
 * cchh2blk - CCHH(Cylinder-Cylinder-Head-Head) 트랙 주소를 선형 블록 번호로 변환.
 *
 * @ptr: 변환할 CCHH 주소. cc(실린더 하위 16비트)와 hh(상위 실린더 4비트 +
 *       헤드 번호 4비트가 섞인 필드)로 구성된 메인프레임 트랙 주소.
 *       DS1EXT1.llimit/ulimit(익스텐트의 하한/상한 트랙)처럼 "블록 번호가
 *       없는" 순수 트랙 단위 주소에 사용된다.
 * @geo: 대상 DASD의 CHS 지오메트리(heads=트랙당 헤드 수, sectors=트랙당
 *       블록 수). disk->fops->getgeo()가 채운 값을 그대로 전달받는다.
 * @return: 해당 트랙의 첫 번째 블록 번호(디바이스 고유 블록 단위, 아직
 *          512바이트 섹터로 환산되지 않음).
 *
 * 메인프레임 DASD는 실린더/헤드/블록(CHR, Cylinder-Head-Record) 좌표계를
 * 사용하며, 큰 볼륨을 지원하기 위해 hh 필드의 상위 니블에 실린더 상위
 * 비트를 밀어 넣는 확장 인코딩을 쓴다(구식 16비트 cc만으로는 대형 볼륨의
 * 실린더 수를 다 표현할 수 없기 때문). 이 함수는 그 인코딩을 풀어 순수
 * 실린더 번호(cyl)와 헤드 번호(head)를 복원한 뒤, "실린더*헤드수*트랙당
 * 블록수 + 헤드*트랙당블록수" 공식으로 해당 트랙의 시작 블록 번호를 계산한다.
 * 실행 컨텍스트: find_vol1_partitions()가 파티션 스캔 중 동기적으로 호출;
 * 별도 락 없이 스칼라 계산만 수행하므로 재진입에 안전하다.
 * 호출자: find_vol1_partitions() (DS1EXT1.llimit/ulimit 변환).
 * 피호출자: 없음(순수 산술 함수).
 *
 * 호출 체인:
 *   find_vol1_partitions → [cchh2blk]
 */
static sector_t cchh2blk(struct vtoc_cchh *ptr, struct hd_geometry *geo)
{
	sector_t cyl;	/* [한국어] 복원할 순수 실린더 번호를 담을 지역 변수 - CHS 좌표계의 C(실린더) */
	__u16 head;	/* [한국어] 복원할 헤드 번호(0..heads-1) - CHS 좌표계의 H(헤드) */

	/* decode cylinder and heads for large volumes */
	cyl = ptr->hh & 0xFFF0;	/* [한국어] hh 필드 상위 12비트를 실린더 상위 비트로 추출 - 대형 볼륨에서 cc 16비트를 넘는 실린더 확장분 */
	cyl <<= 12;		/* [한국어] 추출한 상위 비트를 12비트 왼쪽 시프트 - 이후 cc(하위 16비트)와 합쳐 28비트 실린더 번호를 구성하기 위함 */
	cyl |= ptr->cc;		/* [한국어] 하위 16비트 실린더 값을 병합 - 최종 실린더 번호(cyl) 완성 */
	head = ptr->hh & 0x000F;	/* [한국어] hh 필드 하위 4비트가 실제 헤드 번호 - 최대 16개 헤드까지 표현 가능 */
	return cyl * geo->heads * geo->sectors +	/* [한국어] (실린더 번호 * 실린더당 헤드수 * 트랙당 블록수)로 이 실린더의 시작 블록 계산 */
	       head * geo->sectors;	/* [한국어] 여기에 (헤드 번호 * 트랙당 블록수)를 더해 해당 트랙의 시작 블록 번호 확정 */
}

/*
 * compute the block number from a
 * cyl-cyl-head-head-block structure
 */
/*
 * [한국어]
 * cchhb2blk - CCHHB(Cylinder-Cylinder-Head-Head-Block) 정밀 주소를 선형
 * 블록 번호로 변환.
 *
 * @ptr: 변환할 CCHHB 주소. cchh2blk()의 CCHH에 더해 트랙 내 블록 인덱스(b)
 *       까지 포함하는 "정확한 한 블록"을 가리키는 주소. label->vol.vtoc
 *       (VTOC 자신을 가리키는 F4 레이블의 포인터)처럼 특정 레코드 하나를
 *       정확히 지목해야 하는 경우에 사용된다.
 * @geo: 대상 DASD의 CHS 지오메트리(heads/sectors). cchh2blk()와 동일하게
 *       disk->fops->getgeo()가 채운 값을 전달받는다.
 * @return: ptr이 가리키는 정확한 블록의 절대 블록 번호(디바이스 고유 블록
 *          단위, 아직 512바이트 섹터로 환산되지 않음).
 *
 * cchh2blk()와 동일한 방식으로 실린더/헤드를 복원하되, 트랙 시작 블록에
 * ptr->b(트랙 내 블록 오프셋)를 더해 트랙 안의 특정 레코드까지 정확히
 * 짚어낸다. VTOC의 F4(자기 자신) 레이블이 어디 있는지는 볼륨 레이블의
 * vtoc 필드(CCHHB)로만 알 수 있으므로, VTOC 탐색을 시작하려면 이 함수가
 * 반드시 필요하다.
 * 실행 컨텍스트: find_vol1_partitions()가 VTOC 스캔 시작 블록을 구할 때
 * 동기적으로 1회 호출; 락 불필요.
 * 호출자: find_vol1_partitions() (label->vol.vtoc 변환).
 * 피호출자: 없음(순수 산술 함수).
 *
 * 호출 체인:
 *   find_vol1_partitions → [cchhb2blk]
 */
static sector_t cchhb2blk(struct vtoc_cchhb *ptr, struct hd_geometry *geo)
{
	sector_t cyl;	/* [한국어] 복원할 순수 실린더 번호를 담을 지역 변수 - cchh2blk()와 동일한 CHS C(실린더) */
	__u16 head;	/* [한국어] 복원할 헤드 번호(0..heads-1) - cchh2blk()와 동일한 CHS H(헤드) */

	/* decode cylinder and heads for large volumes */
	cyl = ptr->hh & 0xFFF0;	/* [한국어] hh 필드 상위 12비트를 실린더 상위 비트로 추출 - 대형 볼륨 확장 인코딩(cchh2blk()와 동일 로직) */
	cyl <<= 12;		/* [한국어] 추출한 상위 비트를 12비트 왼쪽 시프트 */
	cyl |= ptr->cc;		/* [한국어] 하위 16비트 실린더 값을 병합 - 최종 실린더 번호(cyl) 완성 */
	head = ptr->hh & 0x000F;	/* [한국어] hh 필드 하위 4비트가 실제 헤드 번호 */
	return	cyl * geo->heads * geo->sectors +	/* [한국어] (실린더 번호 * 실린더당 헤드수 * 트랙당 블록수)로 이 실린더의 시작 블록 계산 */
		head * geo->sectors +		/* [한국어] 여기에 (헤드 번호 * 트랙당 블록수)를 더해 해당 트랙의 시작 블록 번호 산출 */
		ptr->b;		/* [한국어] 트랙 내 블록 오프셋(ptr->b)까지 더해 정확한 단일 블록(레코드) 번호를 확정 - cchh2blk()와의 유일한 차이점 */
}

/* Volume Label Type/ID Length */
#define DASD_VOL_TYPE_LEN	4	/* [한국어] 'VOL1'/'LNX1'/'CMS1' 매직 문자열 길이(4바이트) - get_label_by_type()/find_label()의 memcmp 길이 기준 */
#define DASD_VOL_ID_LEN		6	/* [한국어] 볼륨 시리얼(volser) 길이(6바이트) - 메인프레임 관례상 볼륨 식별자는 최대 6문자 */

/* Volume Label Types */
#define DASD_VOLLBL_TYPE_VOL1 0	/* [한국어] dasd_vollabels[] 배열의 VOL1 항목 인덱스이자 get_label_by_type()이 반환하는 'CDL(VTOC 기반) 레이블' 식별자 */
#define DASD_VOLLBL_TYPE_LNX1 1	/* [한국어] LNX1(LDL) 레이블 식별자 - dasd_vollabels[] 인덱스 1 */
#define DASD_VOLLBL_TYPE_CMS1 2	/* [한국어] CMS1(VM 미니디스크) 레이블 식별자 - dasd_vollabels[] 인덱스 2 */

/*
 * [한국어]
 * struct dasd_vollabel - 레이블 타입 문자열과 내부 식별자를 매핑하는 테이블 항목.
 *
 * get_label_by_type()이 섹터에서 읽은 4바이트 매직 문자열을 dasd_vollabels[]의
 * 각 항목과 순서대로 비교해 어떤 DASD_VOLLBL_TYPE_* 값에 해당하는지 찾아내는
 * 데 사용된다. 이 구조체 자체는 값을 담는 정적 테이블의 원소일 뿐, 별도로
 * 동적 할당되거나 수정되지 않는다.
 */
struct dasd_vollabel {
	char *type;
	/* [한국어] 매직 문자열 리터럴 포인터("VOL1"/"LNX1"/"CMS1" 중 하나).
	 * 설정자: dasd_vollabels[] 정적 초기화 시 문자열 리터럴 주소로 고정.
	 * 읽는 자: get_label_by_type()이 memcmp(type, dasd_vollabels[i].type,
	 * DASD_VOL_TYPE_LEN)으로 섹터에서 읽어 EBCDIC→ASCII 변환한 문자열과 비교.
	 * 값 범위: 항상 4바이트 이상의 유효한 문자열 리터럴 주소 (NULL 아님).
	 * 동기화: 정적 상수 테이블이므로 런타임에 변경되지 않아 락 불필요. */
	int idx;
	/* [한국어] 이 항목이 대표하는 DASD_VOLLBL_TYPE_* 값(0/1/2).
	 * 설정자: dasd_vollabels[] 정적 초기화 시 배열 인덱스와 동일한 값으로 설정.
	 * 읽는 자: get_label_by_type()이 매칭된 항목의 idx를 그대로 반환하며,
	 * 이 값은 find_label()/ibm_partition()의 switch 문에서 분기 기준으로 쓰인다.
	 * 값 범위: DASD_VOLLBL_TYPE_VOL1(0) / _LNX1(1) / _CMS1(2).
	 * 동기화: idx도 배열 인덱스로 고정되어 불변 - 락 불필요. */
};

/* [한국어] dasd_vollabels - 레이블 타입 문자열 ↔ 식별자 매핑 정적 테이블.
 * 지정 초기화자([DASD_VOLLBL_TYPE_VOL1] = {...})를 사용해 배열 인덱스와
 * .idx 값이 항상 일치하도록 강제한다. get_label_by_type()이 이 테이블을
 * 순회하며 매직 문자열을 찾는다. 커널 전역 수명(모듈 로드 시부터 존재)을
 * 가지며 읽기 전용으로만 사용되어 동기화가 필요 없다. */
static struct dasd_vollabel dasd_vollabels[] = {
	[DASD_VOLLBL_TYPE_VOL1] = {	/* [한국어] 인덱스 0 항목 - VOL1(CDL) 레이블 매핑 */
		.type = "VOL1",		/* [한국어] 매칭 대상 매직 문자열 - EBCDIC→ASCII 변환 후 'VOL1'과 비교됨 */
		.idx = DASD_VOLLBL_TYPE_VOL1,	/* [한국어] get_label_by_type()이 반환할 식별자 값(0) */
	},
	[DASD_VOLLBL_TYPE_LNX1] = {	/* [한국어] 인덱스 1 항목 - LNX1(LDL) 레이블 매핑 */
		.type = "LNX1",		/* [한국어] 매칭 대상 매직 문자열 - LDL 포맷 볼륨 레이블 시그니처 */
		.idx = DASD_VOLLBL_TYPE_LNX1,	/* [한국어] get_label_by_type()이 반환할 식별자 값(1) */
	},
	[DASD_VOLLBL_TYPE_CMS1] = {	/* [한국어] 인덱스 2 항목 - CMS1(VM 미니디스크) 레이블 매핑 */
		.type = "CMS1",		/* [한국어] 매칭 대상 매직 문자열 - VM/CMS 미니디스크 레이블 시그니처 */
		.idx = DASD_VOLLBL_TYPE_CMS1,	/* [한국어] get_label_by_type()이 반환할 식별자 값(2) */
	},
};

/*
 * [한국어]
 * get_label_by_type - 4바이트 매직 문자열로부터 DASD 레이블 타입 식별자를 조회.
 *
 * @type: EBCDIC→ASCII 변환이 끝난 4바이트 레이블 타입 문자열('VOL1'/'LNX1'/
 *        'CMS1' 중 하나로 기대되지만, 알 수 없는 값일 수도 있다).
 * @return: 매칭되는 DASD_VOLLBL_TYPE_* 값(0/1/2), 매칭 실패 시 -1.
 *
 * dasd_vollabels[] 정적 테이블을 처음부터 순서대로 순회하며 각 항목의
 * .type 문자열과 memcmp로 비교한다. 테이블 크기가 3개뿐이므로 선형 탐색으로도
 * 충분하며 별도의 해시나 이진 탐색이 필요 없다. 반환값 -1은 find_label()과
 * ibm_partition()의 switch 문에서 어떤 case에도 매칭되지 않아 default 경로로
 * 빠지게 하는 sentinel 값으로 활용된다.
 * 실행 컨텍스트: find_label()이 섹터 후보를 검사할 때, ibm_partition()이
 * 레이블 타입에 따라 파서를 선택할 때 동기적으로 호출; 락 불필요(읽기 전용
 * 정적 테이블 참조).
 * 호출자: find_label(), ibm_partition().
 * 피호출자: memcmp() (문자열 비교).
 *
 * 호출 체인:
 *   find_label/ibm_partition → [get_label_by_type] → memcmp
 */
static int get_label_by_type(const char *type)
{
	int i;	/* [한국어] dasd_vollabels[] 순회 인덱스 */

	for (i = 0; i < ARRAY_SIZE(dasd_vollabels); i++) {	/* [한국어] 테이블의 각 항목(VOL1/LNX1/CMS1)을 순서대로 검사 - ARRAY_SIZE로 배열 크기(3) 계산 */
		if (!memcmp(type, dasd_vollabels[i].type, DASD_VOL_TYPE_LEN))	/* [한국어] 4바이트 매직 문자열이 정확히 일치하는지 확인 - 길이·내용 모두 정확히 일치해야 함 */
			return dasd_vollabels[i].idx;	/* [한국어] 일치하는 항목을 찾으면 즉시 해당 식별자(0/1/2) 반환 */
	}

	return -1;	/* [한국어] 3개 항목 모두 불일치 - 알 수 없는 레이블 타입을 의미하는 sentinel 값 반환 */
}

/*
 * [한국어]
 * find_label - VOL1/LNX1/CMS1 볼륨 레이블이 있는 섹터를 찾아 내용을 읽어온다.
 *
 * @state: 파티션 스캔 진행 상태(read_part_sector()가 내부적으로 참조하는
 *         block_device 등을 포함) - block/partitions/check.h 정의.
 * @info: dasd 드라이버가 제공한 확장 장치 정보. NULL이면 어떤 장치 타입인지
 *        모른다는 뜻이며, 이 경우 가능한 모든 후보 섹터를 다 검사한다.
 * @geo: 다른 find_*_partitions() 함수와 인터페이스를 통일하기 위해 전달되는
 *       CHS 지오메트리 포인터(이 함수 내부 로직에서는 참조되지 않는다).
 * @blocksize: 디바이스의 논리 블록 크기(바이트) - 후보 섹터 번호를 512바이트
 *             단위로 환산하는 데 사용(blocksize >> 9).
 * @labelsect: [출력] 실제로 레이블을 찾은 섹터 번호(512바이트 단위)를 저장.
 *             이후 find_lnx1_partitions()/find_cms1_partitions()가 파티션
 *             시작 위치 계산의 기준점으로 사용한다.
 * @name: [출력] EBCDIC→ASCII로 변환된 볼륨 시리얼(volser) 문자열, 길이
 *        DASD_VOL_ID_LEN(6)바이트.
 * @type: [출력] EBCDIC→ASCII로 변환된 4바이트 레이블 타입 문자열.
 * @label: [출력] 매칭된 섹터 전체를 담은 union label_t 버퍼 - 이후 포맷별
 *         파서가 세부 필드를 다시 읽는다.
 * @return: 레이블을 찾았으면 1, 세 후보 모두 실패하면 0.
 *
 * 레이블이 있을 수 있는 위치는 디바이스 종류에 따라 다르다: ECKD(Extended
 * Count Key Data) 디스크는 블록 2, FBA(Fixed Block Architecture) 디스크는
 * 블록 1, DIAG 디시플린을 쓰는 CMS 포맷 FBA 디스크는 블록 크기와 무관하게
 * 물리 섹터 1이다. info가 있으면(dasd 드라이버가 장치를 인식한 경우) 정확히
 * 한 곳만 검사하면 되지만, info가 없으면(드라이버 부재, 또는 아직 인식되지
 * 않은 장치) 세 후보를 순서대로 다 시도한다. cu_type/dev_type의 특정 조합
 * (0x6310/0x9336, 0x3880/0x3370)은 label_block 필드가 이미 512바이트 섹터
 * 단위로 주어지는 예외적인 FBA 장치 모델이라 별도 스케일 변환 없이 그대로
 * 쓴다.
 * 실행 컨텍스트: ibm_partition()에서 동기적으로 1회 호출; read_part_sector()가
 * 내부적으로 블록 계층 I/O를 동기적으로 수행하므로 이 함수는 블로킹될 수 있다.
 * 호출자: ibm_partition().
 * 피호출자: read_part_sector(), put_dev_sector(), memcpy(), EBCASC(),
 *          get_label_by_type().
 * 에러 경로: read_part_sector()가 NULL을 반환하면(섹터 읽기 실패) 해당 후보를
 * 건너뛰고 다음 후보로 계속 진행한다. 모든 후보가 실패하거나 매칭되는 타입이
 * 없으면 0을 반환해 ibm_partition()이 "레이블 없음" 경로를 타게 한다.
 *
 * 호출 체인:
 *   ibm_partition → [find_label] → read_part_sector/get_label_by_type
 */
static int find_label(struct parsed_partitions *state,
		      dasd_information2_t *info,
		      struct hd_geometry *geo,
		      int blocksize,
		      sector_t *labelsect,
		      char name[],
		      char type[],
		      union label_t *label)
{
	sector_t testsect[3];	/* [한국어] 레이블이 있을 법한 후보 섹터 번호(최대 3개, 512바이트 단위) */
	int i, testcount;	/* [한국어] i: 순회 인덱스, testcount: 실제 검사할 후보 개수(info 유무에 따라 1 또는 3) */
	Sector sect;		/* [한국어] read_part_sector()가 채우는 버퍼 핸들 - put_dev_sector()로 반드시 해제해야 함 */
	void *data;		/* [한국어] 읽어들인 섹터의 커널 매핑 포인터, 실패 시 NULL */

	/* There a three places where we may find a valid label:
	 * - on an ECKD disk it's block 2
	 * - on an FBA disk it's block 1
	 * - on an CMS formatted FBA disk it is sector 1, even if the block size
	 *   is larger than 512 bytes (possible if the DIAG discipline is used)
	 * If we have a valid info structure, then we know exactly which case we
	 * have, otherwise we just search through all possebilities.
	 */
	if (info) {	/* [한국어] dasd 드라이버가 장치 정보를 성공적으로 제공한 경우 - 정확히 한 위치만 알면 됨 */
		if ((info->cu_type == 0x6310 && info->dev_type == 0x9336) ||		/* [한국어] 특정 FBA 모델(제어장치 0x6310/장치타입 0x9336) - label_block이 이미 섹터 단위 */
		    (info->cu_type == 0x3880 && info->dev_type == 0x3370))		/* [한국어] 또 다른 FBA 모델 조합(0x3880/0x3370) - 동일하게 섹터 단위로 취급 */
			testsect[0] = info->label_block;			/* [한국어] 예외 장치는 label_block을 그대로 섹터 번호로 사용(추가 스케일링 불필요) */
		else	/* [한국어] 위 두 모델 이외의 장치 - label_block이 512바이트 섹터가 아니라 디바이스 블록 번호이므로 환산이 필요하다. */
			testsect[0] = info->label_block * (blocksize >> 9);			/* [한국어] 일반적인 경우 label_block(디바이스 블록 단위)을 512바이트 섹터 단위로 환산 */
		testcount = 1;		/* [한국어] info가 있으므로 후보를 하나만 검사하면 충분 */
	} else {
		testsect[0] = 1;		/* [한국어] 후보1: 물리 섹터 1 - DIAG 디시플린의 CMS 포맷 FBA 디스크용 */
		testsect[1] = (blocksize >> 9);		/* [한국어] 후보2: 블록 1을 섹터 단위로 환산 - 일반 FBA 디스크용 */
		testsect[2] = 2 * (blocksize >> 9);		/* [한국어] 후보3: 블록 2를 섹터 단위로 환산 - ECKD 디스크용 */
		testcount = 3;		/* [한국어] info가 없어 장치 종류를 모르므로 세 후보 모두 검사 */
	}
	for (i = 0; i < testcount; ++i) {	/* [한국어] 결정된 후보 섹터들을 순서대로 검사 */
		data = read_part_sector(state, testsect[i], &sect);		/* [한국어] 후보 섹터를 블록 계층에서 동기적으로 읽어옴 - 성공 시 데이터 포인터, 실패 시 NULL */
		if (data == NULL)		/* [한국어] 섹터 읽기 실패(I/O 오류 등) */
			continue;			/* [한국어] 이 후보는 포기하고 다음 후보로 진행 */
		memcpy(label, data, sizeof(*label));		/* [한국어] 섹터 전체를 union label_t 크기만큼 복사 - 아직 타입을 모르므로 최대 크기로 우선 확보 */
		memcpy(type, data, DASD_VOL_TYPE_LEN);		/* [한국어] 섹터 맨 앞 4바이트(레이블 타입 매직 문자열)만 별도로 복사 */
		EBCASC(type, DASD_VOL_TYPE_LEN);		/* [한국어] EBCDIC로 기록된 매직 문자열을 ASCII로 제자리 변환 - 이후 'VOL1' 등과 직접 비교 가능해짐 */
		put_dev_sector(sect);		/* [한국어] 섹터 데이터는 이미 복사했으므로 버퍼 참조를 즉시 해제 */
		switch (get_label_by_type(type)) {		/* [한국어] 변환된 매직 문자열로 레이블 타입 식별 */
		case DASD_VOLLBL_TYPE_VOL1:		/* [한국어] CDL(VTOC 기반) 레이블로 확인됨 */
			memcpy(name, label->vol.volid, DASD_VOL_ID_LEN);			/* [한국어] VOL1 레이블의 volid 필드에서 볼륨 시리얼 추출 */
			EBCASC(name, DASD_VOL_ID_LEN);			/* [한국어] 볼륨 시리얼도 EBCDIC→ASCII 변환 */
			*labelsect = testsect[i];			/* [한국어] 호출자에게 레이블을 찾은 섹터 번호를 알려줌 */
			return 1;			/* [한국어] 레이블 발견 성공 - 즉시 반환 */
		case DASD_VOLLBL_TYPE_LNX1:		/* [한국어] LDL 레이블로 확인됨 */
		case DASD_VOLLBL_TYPE_CMS1:		/* [한국어] CMS 레이블로 확인됨 - LNX1과 동일하게 처리(volid 오프셋이 같음) */
			memcpy(name, label->lnx.volid, DASD_VOL_ID_LEN);			/* [한국어] LNX1/CMS1 모두 union의 lnx 뷰로 volid를 읽어도 동일한 오프셋을 가짐 */
			EBCASC(name, DASD_VOL_ID_LEN);			/* [한국어] 볼륨 시리얼 EBCDIC→ASCII 변환 */
			*labelsect = testsect[i];			/* [한국어] 레이블을 찾은 섹터 번호 기록 */
			return 1;			/* [한국어] 레이블 발견 성공 - 즉시 반환 */
		default:		/* [한국어] 알 수 없는 타입(get_label_by_type()이 -1 반환) - 이 후보는 레이블이 아님 */
			break;			/* [한국어] switch만 빠져나가고 for 루프는 다음 후보로 계속 진행 */
		}
	}

	return 0;	/* [한국어] 모든 후보를 다 검사했지만 유효한 레이블을 찾지 못함 */
}

/*
 * [한국어]
 * find_vol1_partitions - VOL1(CDL) 레이블 디스크의 VTOC를 순회해 각 데이터셋을
 * 파티션으로 등록한다.
 *
 * @state: 파티션 스캔 상태 - put_partition()으로 결과를 등록하고 pp_buf에
 *         사람이 읽을 로그 문자열을 남긴다.
 * @geo: CHS 지오메트리 - cchh2blk()/cchhb2blk()로 트랙 주소를 블록 번호로
 *       변환할 때 필요.
 * @blocksize: 디바이스 논리 블록 크기(바이트) - secperblk(블록당 512바이트
 *             섹터 수) 계산에 사용.
 * @name: find_label()이 채운 볼륨 시리얼(로그 출력용).
 * @label: find_label()이 채운 VOL1 레이블 버퍼 - label->vol.vtoc(VTOC 자신의
 *         위치를 가리키는 CCHHB)를 얻는 데 사용.
 * @return: 최소 한 개 이상 파티션을 찾거나 VTOC를 끝까지 읽었으면 1, VTOC의
 *          마지막 섹터 읽기가 실패했으면 -1.
 *
 * CDL(Compatible Disk Layout)에서 VTOC(Volume Table Of Contents)는 여러
 * 개의 DSCB(Data Set Control Block, 96바이트 고정 크기 레코드)가 연속된
 * 트랙에 순서대로 나열된 구조다. label->vol.vtoc는 VTOC 자신을 기술하는
 * format-4 DSCB의 위치를 가리키므로, cchhb2blk()로 그 블록 번호를 구한 뒤
 * +1 하여 바로 다음 DSCB부터 읽기 시작한다. 이후 while 루프에서 DSCB를
 * 한 블록씩 순서대로 읽어, format-1/format-8(실제 데이터셋 정의) DSCB를
 * 만나면 그 DS1EXT1(첫 번째 익스텐트)의 시작/끝 트랙을 블록 번호로 변환해
 * 파티션 하나로 등록한다. format-4/5/7/9(VTOC 메타데이터, 자유공간 등)는
 * 건너뛰고, 그 외 알 수 없는 포맷 ID를 만나면 VTOC의 끝으로 간주해 루프를
 * 종료한다. 모든 포맷 ID 비교는 EBCDIC 인코딩 상수(_ascebc['1'] 등)와
 * 이루어지는데, DSCB의 DS1FMTID 필드가 디스크에 EBCDIC 문자로 기록되어
 * 있기 때문이다.
 * 실행 컨텍스트: ibm_partition()에서 VOL1 레이블이 확인된 경우 동기적으로
 * 1회 호출; read_part_sector() 호출마다 블로킹 가능.
 * 호출자: ibm_partition().
 * 피호출자: cchhb2blk(), cchh2blk(), read_part_sector(), put_dev_sector(),
 *          put_partition(), seq_buf_printf()/seq_buf_puts().
 * 에러 경로: state->limit(파티션 개수 상한)에 도달하면 더 이상 등록하지
 * 않고 루프를 중단한다. 루프 종료 시점에 마지막 read_part_sector()가 NULL을
 * 반환한 상태(VTOC 끝이 아니라 실제 I/O 실패로 종료)였다면 -1을 반환해
 * ibm_partition()에게 이상 상황을 알린다.
 *
 * 호출 체인:
 *   ibm_partition → [find_vol1_partitions] → cchhb2blk/cchh2blk/put_partition
 */
static int find_vol1_partitions(struct parsed_partitions *state,
				struct hd_geometry *geo,
				int blocksize,
				char name[],
				union label_t *label)
{
	sector_t blk;	/* [한국어] 현재 읽고 있는 DSCB의 블록 번호(디바이스 블록 단위) - VTOC를 순차 순회하는 커서 */
	int counter;	/* [한국어] 지금까지 등록한 파티션 개수 - put_partition()의 파티션 번호(counter+1) 산출에도 사용 */
	Sector sect;	/* [한국어] read_part_sector()가 채우는 버퍼 핸들 - put_dev_sector()로 즉시 해제 */
	unsigned char *data;	/* [한국어] 읽어들인 DSCB 섹터의 커널 매핑 포인터, 실패 시 NULL(while 루프 종료 조건) */
	loff_t offset, size;	/* [한국어] 파티션 시작 오프셋과 크기(모두 512바이트 섹터 단위) */
	struct vtoc_format1_label f1;	/* [한국어] 현재 순회 중인 DSCB를 format-1 레이아웃으로 해석한 지역 사본 */
	int secperblk;	/* [한국어] 디바이스 블록 하나가 몇 개의 512바이트 섹터에 해당하는지(blocksize/512) */

	seq_buf_printf(&state->pp_buf, "VOL1/%8s:", name);	/* [한국어] 파티션 스캔 로그(pp_buf)에 'VOL1/<볼륨시리얼>:' 접두어 기록 */
	/*
	 * get start of VTOC from the disk label and then search for format1
	 * and format8 labels
	 */
	secperblk = blocksize >> 9;	/* [한국어] 512바이트 단위 섹터 환산 계수 계산(예: blocksize=4096이면 secperblk=8) */
	blk = cchhb2blk(&label->vol.vtoc, geo) + 1;	/* [한국어] VTOC의 첫 DSCB(format-4, 자기 자신 기술)의 블록 번호를 구하고, 그 다음 DSCB부터 읽기 위해 +1 */
	counter = 0;	/* [한국어] 파티션 카운터 초기화 */
	data = read_part_sector(state, blk * secperblk, &sect);	/* [한국어] blk를 512바이트 섹터 단위로 환산해 첫 DSCB 섹터를 읽음 */
	while (data != NULL) {	/* [한국어] 섹터 읽기가 성공하는 동안 VTOC를 계속 순회 */
		memcpy(&f1, data, sizeof(struct vtoc_format1_label));		/* [한국어] 방금 읽은 섹터를 format-1 DSCB 구조로 복사 - 실제 포맷과 무관하게 우선 f1 레이아웃으로 해석(포맷 ID는 공통 오프셋에 위치) */
		put_dev_sector(sect);		/* [한국어] 섹터 데이터를 이미 복사했으므로 버퍼 참조 즉시 해제 */
		/* skip FMT4 / FMT5 / FMT7 labels */
		if (f1.DS1FMTID == _ascebc['4']		/* [한국어] format-4(VTOC 자체 기술 DSCB)인지 확인 - EBCDIC '4' 코드와 비교 */
		    || f1.DS1FMTID == _ascebc['5']		    /* [한국어] format-5(자유 공간 DSCB)인지 확인 */
		    || f1.DS1FMTID == _ascebc['7']		    /* [한국어] format-7(대형 볼륨용 확장 자유 공간 DSCB)인지 확인 */
		    || f1.DS1FMTID == _ascebc['9']) {		    /* [한국어] format-9(format-8과 짝을 이루는 추가 익스텐트 DSCB)인지 확인 */
			blk++;			/* [한국어] 데이터셋 정의가 아닌 메타데이터 DSCB이므로 건너뛰고 다음 블록으로 이동 */
			data = read_part_sector(state, blk * secperblk, &sect);			/* [한국어] 다음 DSCB 섹터를 읽어 while 조건 재평가에 사용 */
			continue;			/* [한국어] 이번 DSCB는 파티션으로 등록하지 않고 루프 재시작 */
		}
		/* only FMT1 and 8 labels valid at this point */
		if (f1.DS1FMTID != _ascebc['1'] &&		/* [한국어] format-1(일반 데이터셋 DSCB)이 아니고 */
		    f1.DS1FMTID != _ascebc['8'])		    /* [한국어] format-8(대형 데이터셋용 확장 DSCB)도 아니면 */
			break;			/* [한국어] 알 수 없는 포맷 ID - VTOC의 끝(또는 비정상 데이터)으로 간주하고 순회 종료 */
		/* OK, we got valid partition data */
		offset = cchh2blk(&f1.DS1EXT1.llimit, geo);		/* [한국어] 이 데이터셋의 첫 익스텐트 하한 트랙 주소를 블록 번호로 변환 - 파티션 시작 블록 */
		size  = cchh2blk(&f1.DS1EXT1.ulimit, geo) -		/* [한국어] 상한 트랙 주소를 블록 번호로 변환 */
			offset + geo->sectors;			/* [한국어] (상한 블록 - 하한 블록 + 트랙당 블록수)로 익스텐트 전체 블록 수 계산 - 상한 트랙 전체를 포함하기 위해 트랙 크기를 더함 */
		offset *= secperblk;		/* [한국어] 디바이스 블록 단위 오프셋을 512바이트 섹터 단위로 환산 */
		size *= secperblk;		/* [한국어] 디바이스 블록 단위 크기를 512바이트 섹터 단위로 환산 */
		if (counter >= state->limit)		/* [한국어] 이 디스크에서 허용되는 최대 파티션 개수를 이미 채웠는지 확인 */
			break;			/* [한국어] 더 이상 등록할 수 없으므로 VTOC 순회를 중단 */
		put_partition(state, counter + 1, offset, size);		/* [한국어] 파티션 번호(counter+1, 1부터 시작)로 이 익스텐트를 파티션으로 등록 */
		counter++;		/* [한국어] 등록한 파티션 개수 증가 */
		blk++;		/* [한국어] 다음 DSCB 블록으로 이동 */
		data = read_part_sector(state, blk * secperblk, &sect);		/* [한국어] 다음 DSCB 섹터를 읽어 while 루프 계속 여부 결정 */
	}
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 이 디스크에 대한 로그 줄을 개행으로 마무리 */

	if (!data)	/* [한국어] 루프가 while 조건 실패(섹터 읽기 실패)로 끝났는지 확인 - break로 정상 종료된 경우와 구분 */
		return -1;		/* [한국어] VTOC 끝이 아니라 실제 I/O 실패로 종료된 것으로 판단해 오류 반환 */

	return 1;	/* [한국어] VTOC 순회를 정상적으로 마침(파티션이 0개든 여러 개든) - 성공 반환 */
}

/*
 * [한국어]
 * find_lnx1_partitions - LNX1(LDL) 레이블 디스크에 단일 대형 파티션 하나를
 * 정의한다.
 *
 * @state: 파티션 스캔 상태.
 * @geo: CHS 지오메트리 - 대형 볼륨 미지원 포맷일 때 geo_size 계산에 사용.
 * @blocksize: 디바이스 논리 블록 크기 - secperblk 계산에 사용.
 * @name: 로그 출력용 볼륨 시리얼.
 * @label: find_label()이 채운 LNX1 레이블 버퍼 - ldl_version/formatted_blocks
 *         필드를 사용.
 * @labelsect: find_label()이 알려준 레이블 섹터 번호 - 파티션은 이 바로
 *             다음 블록부터 시작한다.
 * @nr_sectors: bdev_nr_sectors()로 얻은 디바이스 전체 512바이트 섹터 수 -
 *              대형 볼륨 미지원 포맷일 때 크기 산정의 실측 기준.
 * @info: dasd 드라이버가 제공한 확장 정보(NULL 가능) - ECKD 여부 판단에 사용.
 * @return: 항상 1(파티션을 정의하지 못하고 조기 반환하는 경우에도 'LNX1으로
 *          인식은 했다'는 의미로 1을 반환).
 *
 * LDL(Linux Disk Layout)은 리눅스 전용의 단순 레이아웃으로, 볼륨 전체가
 * 원칙적으로 하나의 큰 파티션이다. ldl_version이 0xf2(대형 볼륨 지원 버전)
 * 이면 레이블에 기록된 formatted_blocks(64비트, 디바이스 블록 단위)를 그대로
 * 신뢰해 크기를 계산한다. 그렇지 않은 구버전 포맷이면 formatted_blocks가
 * 없거나 신뢰할 수 없으므로, CHS 지오메트리로부터 계산한 geo_size와 블록
 * 계층이 보고한 nr_sectors를 비교하는 sanity check을 수행한다. 둘이 같으면
 * 안전하게 nr_sectors를 사용하고, 다르면 info가 없을 때는 크기를 확정하지
 * 못해 파티션 없이 조기 반환하며, info가 있고 장치 타입이 'ECKD'이면 더
 * 작은 쪽(geo_size)을 채택해 지오메트리를 벗어나는 접근을 방지한다. ECKD가
 * 아닌 타입(FBA 등)은 nr_sectors 기반 크기를 그대로 유지한다.
 * 실행 컨텍스트: ibm_partition()에서 LNX1 레이블이 확인된 경우 동기적으로
 * 1회 호출.
 * 호출자: ibm_partition().
 * 피호출자: put_partition(), seq_buf_printf()/seq_buf_puts(), strcmp().
 * 에러 경로: 크기를 확정할 수 없는 경우(불일치 && info 없음) 파티션을
 * 등록하지 않고 로그만 남긴 뒤 1을 반환 - "레이블은 인식했으나 파티션 정보
 * 없음" 상태로, ibm_partition()은 이를 실패로 취급하지 않는다.
 *
 * 호출 체인:
 *   ibm_partition → [find_lnx1_partitions] → put_partition
 */
static int find_lnx1_partitions(struct parsed_partitions *state,
				struct hd_geometry *geo,
				int blocksize,
				char name[],
				union label_t *label,
				sector_t labelsect,
				sector_t nr_sectors,
				dasd_information2_t *info)
{
	loff_t offset, geo_size, size;	/* [한국어] offset: 파티션 시작 위치, geo_size: 지오메트리 기반 추정 크기, size: 최종 채택 크기(모두 512바이트 섹터 단위) */
	int secperblk;	/* [한국어] 디바이스 블록당 512바이트 섹터 수 */

	seq_buf_printf(&state->pp_buf, "LNX1/%8s:", name);	/* [한국어] 스캔 로그에 'LNX1/<볼륨시리얼>:' 접두어 기록 */
	secperblk = blocksize >> 9;	/* [한국어] 섹터 환산 계수 계산 */
	if (label->lnx.ldl_version == 0xf2) {	/* [한국어] 0xf2 버전이면 대형 볼륨 지원 포맷 - formatted_blocks 필드가 유효 */
		size = label->lnx.formatted_blocks * secperblk;		/* [한국어] 레이블에 기록된 포맷 블록 수를 그대로 신뢰해 섹터 단위 크기로 환산 */
	} else {
		/*
		 * Formated w/o large volume support. If the sanity check
		 * 'size based on geo == size based on nr_sectors' is true, then
		 * we can safely assume that we know the formatted size of
		 * the disk, otherwise we need additional information
		 * that we can only get from a real DASD device.
		 */
		geo_size = geo->cylinders * geo->heads		/* [한국어] 실린더 수 * 헤드 수 */
			* geo->sectors * secperblk;		/* [한국어] * 트랙당 블록수 * secperblk = 지오메트리로부터 추정한 전체 섹터 수 */
		size = nr_sectors;		/* [한국어] 우선 블록 계층이 보고한 실측 섹터 수를 기본값으로 채택 */
		if (size != geo_size) {		/* [한국어] 지오메트리 기반 추정치와 실측치가 다르면 sanity check 실패 - 추가 판단 필요 */
			if (!info) {			/* [한국어] dasd 확장 정보가 없어 어느 쪽이 맞는지 판단할 근거가 없음 */
				seq_buf_puts(&state->pp_buf, "\n");				/* [한국어] 로그 줄만 마무리하고 */
				return 1;				/* [한국어] 파티션을 정의하지 않은 채 '레이블은 인식함' 상태로 조기 반환 */
			}
			if (!strcmp(info->type, "ECKD"))			/* [한국어] 장치 타입이 ECKD(지오메트리가 신뢰할 수 있는 유형)인지 확인 */
				if (geo_size < size)				/* [한국어] 지오메트리 기반 추정치가 실측치보다 작다면 */
					size = geo_size;					/* [한국어] 더 보수적인(작은) 지오메트리 기반 크기를 채택해 범위 초과 접근 방지 */
			/* else keep size based on nr_sectors */
		}
	}
	/* first and only partition starts in the first block after the label */
	offset = labelsect + secperblk;	/* [한국어] 레이블이 있는 섹터 바로 다음 블록부터 파티션 시작 */
	put_partition(state, 1, offset, size - offset);	/* [한국어] 파티션 번호 1로 단일 파티션 등록 - 크기는 전체 크기에서 레이블 영역을 제외한 나머지 */
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 로그 줄 마무리 */
	return 1;	/* [한국어] LNX1 처리 성공 */
}

/*
 * [한국어]
 * find_cms1_partitions - CMS1(VM/CMS 미니디스크) 레이블 디스크에 단일
 * 파티션을 정의한다.
 *
 * @state: 파티션 스캔 상태.
 * @geo: 다른 find_*_partitions() 함수와 시그니처를 맞추기 위해 유지되는
 *       매개변수 - 이 함수 내부에서는 실제로 사용되지 않는다(CMS 레이블
 *       자체에 필요한 크기 정보가 모두 들어있기 때문).
 * @blocksize: 호출 시점의 디바이스 논리 블록 크기 - 함수 내부에서
 *             label->cms.block_size 값으로 즉시 덮어써진다(지역 변수 재활용).
 * @name: 로그 출력용 볼륨 시리얼.
 * @label: find_label()이 채운 CMS1 레이블 버퍼 - block_size/disk_offset/
 *         block_count 필드를 사용.
 * @labelsect: find_label()이 알려준 레이블 섹터 번호 - DIAG FBA 특수
 *             케이스 판별에 사용.
 * @return: 항상 1(CMS1 레이블 인식 성공).
 *
 * VM(가상 머신) 환경의 CMS(Conversational Monitor System)가 포맷한 미니
 * 디스크를 처리한다. 먼저 blocksize 지역 변수를 레이블에 기록된 실제
 * block_size로 교체해 이후 secperblk 계산의 기준을 CMS 레이블 값으로
 * 통일한다. disk_offset이 0이 아니면 이 디스크가 더 큰 실제 디스크의 일부를
 * 떼어낸 "예약된 미니디스크(reserved minidisk, MDSK)"라는 뜻이므로,
 * disk_offset을 파티션 시작 오프셋으로 쓰고 block_count에서 1을 뺀 만큼을
 * 크기로 사용한다(레이블 자신이 차지하는 블록 1개를 제외). disk_offset이
 * 0이면 일반 미니디스크이며, DIAG 디시플린 FBA 장치의 특수 케이스(레이블이
 * 블록 1이 아니라 물리 섹터 1에 있는 경우, labelsect==1로 식별)에서는
 * 파티션이 여전히 블록 2에서 시작한다고 가정하고, 그 외에는 레이블 바로
 * 다음 블록부터 시작한다고 가정한다.
 * 실행 컨텍스트: ibm_partition()에서 CMS1 레이블이 확인된 경우 동기적으로
 * 1회 호출.
 * 호출자: ibm_partition().
 * 피호출자: put_partition(), seq_buf_printf()/seq_buf_puts().
 *
 * 호출 체인:
 *   ibm_partition → [find_cms1_partitions] → put_partition
 */
static int find_cms1_partitions(struct parsed_partitions *state,
				struct hd_geometry *geo,
				int blocksize,
				char name[],
				union label_t *label,
				sector_t labelsect)
{
	loff_t offset, size;	/* [한국어] 파티션 시작 오프셋과 크기(모두 512바이트 섹터 단위) */
	int secperblk;	/* [한국어] CMS 레이블 기준 블록당 512바이트 섹터 수 */

	/*
	 * VM style CMS1 labeled disk
	 */
	blocksize = label->cms.block_size;	/* [한국어] 매개변수로 받은 blocksize를 CMS 레이블에 기록된 실제 블록 크기로 교체(지역 변수 재사용) */
	secperblk = blocksize >> 9;	/* [한국어] 교체된 blocksize 기준으로 섹터 환산 계수 재계산 */
	if (label->cms.disk_offset != 0) {	/* [한국어] 0이 아니면 이 CMS 디스크가 더 큰 실디스크 안의 예약된 미니디스크(MDSK)임을 의미 */
		seq_buf_printf(&state->pp_buf, "CMS1/%8s(MDSK):", name);		/* [한국어] 로그에 미니디스크임을 나타내는 '(MDSK)' 태그 포함 */
		/* disk is reserved minidisk */
		offset = label->cms.disk_offset * secperblk;		/* [한국어] 레이블에 기록된 오프셋(디바이스 블록 단위)을 512바이트 섹터로 환산해 파티션 시작 위치로 사용 */
		size = (label->cms.block_count - 1) * secperblk;		/* [한국어] 전체 블록 수에서 레이블 블록 1개를 제외한 만큼을 크기로 환산 */
	} else {
		seq_buf_printf(&state->pp_buf, "CMS1/%8s:", name);		/* [한국어] 일반(비-미니디스크) CMS1 볼륨 로그 출력 */
		/*
		 * Special case for FBA devices:
		 * If an FBA device is CMS formatted with blocksize > 512 byte
		 * and the DIAG discipline is used, then the CMS label is found
		 * in sector 1 instead of block 1. However, the partition is
		 * still supposed to start in block 2.
		 */
		if (labelsect == 1)		/* [한국어] 레이블이 블록 1이 아니라 물리 섹터 1에서 발견된 DIAG FBA 특수 케이스 */
			offset = 2 * secperblk;			/* [한국어] 이 경우에도 파티션은 관례대로 블록 2부터 시작한다고 가정 */
		else	/* [한국어] 레이블이 정상적으로 블록 경계에서 발견된 일반 경로 - 위 특수 케이스처럼 위치를 고정하지 않고 레이블 위치에서 이어 계산한다. */
			offset = labelsect + secperblk;			/* [한국어] 일반적인 경우 레이블 바로 다음 블록부터 파티션 시작 */
		size = label->cms.block_count * secperblk;		/* [한국어] 미니디스크가 아니므로 전체 block_count를 그대로 크기로 환산(빼기 없음) */
	}

	put_partition(state, 1, offset, size-offset);	/* [한국어] 파티션 번호 1로 등록 - 크기는 계산된 size에서 시작 오프셋을 뺀 나머지 */
	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 로그 줄 마무리 */
	return 1;	/* [한국어] CMS1 처리 성공 */
}


/*
 * This is the main function, called by check.c
 */
/*
 * [한국어]
 * ibm_partition - IBM DASD 파티션 파서의 진입점 (check.c의 파티션 포맷 테이블에서 호출됨).
 *
 * @state: 파티션 스캔 상태 - state->disk(대상 gendisk), state->limit(등록 가능한
 *         최대 파티션 수), state->pp_buf(로그 버퍼) 등을 담고 있다.
 * @return: 1 이상이면 이 파서가 (부분적으로라도) 파티션을 확정했다는 뜻이고,
 *          0이면 이 디스크가 DASD 형식이 아니거나 인식에 실패해 check.c가 다음
 *          파티션 포맷 파서로 넘어가야 함을 뜻한다.
 *
 * block/partitions/check.c는 등록된 모든 파티션 포맷 파서를 순서대로 시도하며,
 * 이 함수는 그 중 하나로서 대상 디스크가 s390 DASD 장치인지, 그리고 어떤
 * 레이블(VOL1/LNX1/CMS1)로 포맷되어 있는지를 판별해 파티션 경계를 등록한다.
 * 처리 단계:
 *   1) disk->fops->getgeo가 없으면 애초에 DASD 지오메트리를 얻을 수 없는
 *      디바이스이므로 즉시 0을 반환(다른 파서로 위임).
 *   2) symbol_get(dasd_biodasdinfo)로 s390 dasd 드라이버가 export한 콜백을
 *      동적으로 참조한다. 이 파일은 s390 전용이 아니라 모든 아키텍처의
 *      제네릭 블록 계층에 링크되므로, dasd 드라이버가 없는 커널/아키텍처에서도
 *      빌드·링크가 깨지지 않도록 symbol_get()으로 느슨하게 연결한다.
 *   3) bdev_logical_block_size()/bdev_nr_sectors()로 블록 크기와 전체 용량을
 *      얻고, 둘 중 하나라도 비정상이면 조기 종료.
 *   4) info/geo/label 세 버퍼를 순서대로 kmalloc, 각 단계 실패 시 그 이전
 *      단계까지 할당된 자원만 정확히 해제하는 goto 체인(out_freeall →
 *      out_nolab → out_nogeo → out_symbol → out_exit)을 사용한다.
 *   5) geo->start를 get_start_sect()로 미리 채워, getgeo 콜백이 start를
 *      채우지 않는 드라이버(virtio-blk 등으로 에뮬레이션되는 경우)에도
 *      대비한다. 그 후 실제 getgeo()를 호출해 CHS 지오메트리를 얻는다.
 *   6) fn(dasd_biodasdinfo)이 없거나 호출이 실패하면 info를 무효화(kfree 후
 *      NULL)한다 - dasd 드라이버가 이 디스크를 인식하지 못했다는 뜻이며,
 *      이후 로직은 info==NULL을 "메타데이터 없이 추측만으로 진행"하는
 *      신호로 사용한다.
 *   7) find_label()로 레이블을 탐색해 성공하면 레이블 타입에 따라
 *      find_vol1_partitions()/find_lnx1_partitions()/find_cms1_partitions()
 *      중 하나로 위임한다.
 *   8) 레이블을 찾지 못했지만 info가 유효하다면(dasd 드라이버가 이 장치를
 *      DASD로 인식은 했다는 뜻) 하위 호환을 위해 무조건 이 디스크를
 *      "claim"(res=1)하고, LDL 포맷인데 레이블이 없는 경우("(nonl)" =
 *      no label) label_block 다음 블록부터 끝까지를 단일 파티션으로 정의한다.
 *   9) 레이블도 못 찾고 info도 없으면(DASD가 아니거나 완전히 인식 실패)
 *      res=0으로 다른 파서에 위임한다.
 * 실행 컨텍스트: 디스크 등록/재스캔 시 태스크 컨텍스트에서 동기적으로
 * 실행되며, 내부적으로 다수의 블로킹 I/O(read_part_sector 등)를 수행한다.
 * 호출자: check_partition() (block/partitions/check.c) - check_part[] 포맷
 *         테이블을 통해 함수 포인터로 호출됨.
 * 피호출자: symbol_get()/symbol_put(), disk->fops->getgeo(), find_label(),
 *          find_vol1_partitions(), find_lnx1_partitions(),
 *          find_cms1_partitions(), put_partition(), kmalloc_obj()/kfree().
 * 에러 경로: 각 단계별 실패는 out_* 레이블로 점프해 그 시점까지 획득한
 * 자원만 역순으로(가장 나중에 할당한 것부터) 해제하는 고전적인 커널
 * "goto 체인" 패턴을 사용한다. res 변수는 각 goto 지점에서 이미 설정된
 * 값(대부분 0, 일부 경로에서 1)을 그대로 유지한 채 out_exit까지 흘러간다.
 *
 * 호출 체인:
 *   check_partition → [ibm_partition] → find_label →
 *   find_vol1_partitions/find_lnx1_partitions/find_cms1_partitions
 */
int ibm_partition(struct parsed_partitions *state)
{
	int (*fn)(struct gendisk *disk, dasd_information2_t *info);	/* [한국어] dasd_biodasdinfo 심볼의 함수 포인터 타입 선언 - symbol_get()의 반환값을 담을 지역 변수 */
	struct gendisk *disk = state->disk;	/* [한국어] 파티션을 스캔 중인 대상 범용 디스크(gendisk) */
	struct block_device *bdev = disk->part0;	/* [한국어] 디스크 전체를 나타내는 '파티션 0' 의사 block_device - 여기서 블록 크기/전체 섹터 수를 조회 */
	int blocksize, res;	/* [한국어] blocksize: 논리 블록 크기(바이트), res: 이 함수의 최종 반환값(기본 0) */
	loff_t offset, size;	/* [한국어] '레이블 없는 LDL' 폴백 경로에서 사용할 파티션 오프셋/크기(512바이트 섹터 단위) */
	sector_t nr_sectors;	/* [한국어] 디바이스 전체 512바이트 섹터 수 */
	dasd_information2_t *info;	/* [한국어] dasd 드라이버가 채워주는 확장 장치 정보 버퍼(동적 할당) */
	struct hd_geometry *geo;	/* [한국어] disk->fops->getgeo()가 채우는 CHS 지오메트리 버퍼(동적 할당) */
	char type[DASD_VOL_TYPE_LEN + 1] = "";	/* [한국어] find_label()이 채울 4바이트 레이블 타입 문자열 + NUL 종단 여유 1바이트 */
	char name[DASD_VOL_ID_LEN + 1] = "";	/* [한국어] find_label()이 채울 6바이트 볼륨 시리얼 + NUL 종단 여유 1바이트 */
	sector_t labelsect;	/* [한국어] find_label()이 알려주는, 레이블을 실제로 찾은 섹터 번호 */
	union label_t *label;	/* [한국어] find_label()이 섹터 내용을 복사해 넣을 버퍼(동적 할당) */

	res = 0;	/* [한국어] 기본값은 '인식 실패'(0) - 각 성공 경로에서만 명시적으로 갱신됨 */
	if (!disk->fops->getgeo)	/* [한국어] 이 gendisk의 블록 장치 드라이버가 CHS 지오메트리 콜백을 아예 구현하지 않은 경우 */
		goto out_exit;		/* [한국어] DASD일 가능성이 없는 디바이스로 판단해 아무 자원도 할당하지 않고 즉시 반환 */
	fn = symbol_get(dasd_biodasdinfo);	/* [한국어] s390 dasd 드라이버가 모듈로 로드되어 있다면 그 심볼 주소를, 없으면 NULL을 얻음 - 느슨한 런타임 의존성 */
	blocksize = bdev_logical_block_size(bdev);	/* [한국어] 블록 계층이 보고하는 논리 블록 크기 조회 */
	if (blocksize <= 0)	/* [한국어] 비정상적인(0 이하) 블록 크기 - 유효한 디바이스가 아님 */
		goto out_symbol;		/* [한국어] symbol_get()으로 얻은 참조만 반환하고 종료(아직 info/geo/label 미할당) */
	nr_sectors = bdev_nr_sectors(bdev);	/* [한국어] 디바이스 전체 512바이트 섹터 수 조회 */
	if (nr_sectors == 0)	/* [한국어] 용량이 0인 디바이스 - 파티션을 가질 수 없음 */
		goto out_symbol;		/* [한국어] 마찬가지로 symbol 참조만 정리 */
	info = kmalloc_obj(dasd_information2_t);	/* [한국어] dasd 확장 정보를 담을 버퍼 커널 메모리 할당 */
	if (info == NULL)	/* [한국어] 메모리 부족 등으로 할당 실패 */
		goto out_symbol;		/* [한국어] 아직 아무것도 할당되지 않았으므로 symbol 참조만 정리 */
	geo = kmalloc_obj(struct hd_geometry);	/* [한국어] CHS 지오메트리를 담을 버퍼 할당 */
	if (geo == NULL)	/* [한국어] 할당 실패 */
		goto out_nogeo;		/* [한국어] info는 이미 할당되었으므로 info까지만 해제하는 단계로 점프 */
	label = kmalloc_obj(union label_t);	/* [한국어] 레이블 섹터 내용을 담을 버퍼 할당 */
	if (label == NULL)	/* [한국어] 할당 실패 */
		goto out_nolab;		/* [한국어] info/geo는 이미 할당되었으므로 그 둘까지 해제하는 단계로 점프 */
	/* set start if not filled by getgeo function e.g. virtblk */
	geo->start = get_start_sect(bdev);	/* [한국어] getgeo 콜백이 start를 채우지 않는 드라이버(virtio-blk 등)에도 대비해 미리 기본값을 채워둠 */
	if (disk->fops->getgeo(disk, geo))	/* [한국어] 실제 드라이버의 getgeo 콜백 호출 - CHS 필드(cylinders/heads/sectors 등)를 채움, 0이 아니면 실패 */
		goto out_freeall;		/* [한국어] 지오메트리를 얻지 못하면 이후 모든 계산이 불가능하므로 할당한 세 버퍼 모두 해제하고 종료 */
	if (!fn || fn(disk, info)) {	/* [한국어] dasd 드라이버가 없거나(fn NULL), 있어도 이 디스크에 대한 정보 조회 자체가 실패한 경우 */
		kfree(info);		/* [한국어] 쓸모없어진 info 버퍼를 즉시 해제 */
		info = NULL;		/* [한국어] 이후 코드에서 'dasd 드라이버가 이 장치를 인식하지 못함' 상태를 나타내는 신호로 NULL 사용 */
	}

	if (find_label(state, info, geo, blocksize, &labelsect, name, type, label)) {	/* [한국어] VOL1/LNX1/CMS1 레이블 탐색 시도 - 성공(1)하면 타입별 파서로 분기 */
		switch (get_label_by_type(type)) {		/* [한국어] 찾은 레이블의 4바이트 타입 문자열로 포맷 종류 판별 */
		case DASD_VOLLBL_TYPE_VOL1:		/* [한국어] CDL(VTOC 기반 다중 데이터셋) 레이아웃 */
			res = find_vol1_partitions(state, geo, blocksize, name,			/* [한국어] VOL1 레이블 확인 - VTOC 기반 파서로 위임 시작 */
						   label);					   /* [한국어] VTOC를 순회해 데이터셋별 파티션을 등록하고 결과 코드를 res에 저장 */
			break;			/* [한국어] VOL1 처리 완료 */
		case DASD_VOLLBL_TYPE_LNX1:		/* [한국어] LDL(리눅스 전용 단일 대형 파티션) 레이아웃 */
			res = find_lnx1_partitions(state, geo, blocksize, name,			/* [한국어] LNX1 레이블 확인 - LDL 단일 파티션 파서로 위임 시작 */
						   label, labelsect, nr_sectors,					   /* [한국어] labelsect/nr_sectors/info까지 전달해 대형 볼륨 여부 및 sanity check에 활용 */
						   info);					   /* [한국어] 단일 파티션 크기를 계산해 등록하고 결과 코드를 res에 저장 */
			break;			/* [한국어] LNX1 처리 완료 */
		case DASD_VOLLBL_TYPE_CMS1:		/* [한국어] VM/CMS 미니디스크 레이아웃 */
			res = find_cms1_partitions(state, geo, blocksize, name,			/* [한국어] CMS1 레이블 확인 - VM 미니디스크 파서로 위임 시작 */
						   label, labelsect);					   /* [한국어] CMS 레이블 기준 단일 파티션을 계산해 등록하고 결과 코드를 res에 저장 */
			break;			/* [한국어] CMS1 처리 완료 */
		}
	} else if (info) {	/* [한국어] 레이블은 찾지 못했지만, dasd 드라이버가 이 디스크를 유효한 DASD로 인식은 한 경우 */
		/*
		 * ugly but needed for backward compatibility:
		 * If the block device is a DASD (i.e. BIODASDINFO2 works),
		 * then we claim it in any case, even though it has no valid
		 * label. If it has the LDL format, then we simply define a
		 * partition as if it had an LNX1 label.
		 */
		res = 1;		/* [한국어] 레이블이 없어도 DASD로 확인되었으므로 하위 호환을 위해 무조건 이 디스크를 '인식함'으로 처리 */
		if (info->format == DASD_FORMAT_LDL) {		/* [한국어] LDL 포맷인데 레이블을 못 찾은 경우('nonl' = no label 케이스) */
			seq_buf_puts(&state->pp_buf, "(nonl)");			/* [한국어] 로그에 '레이블 없음'을 나타내는 태그 기록 */
			size = nr_sectors;			/* [한국어] 크기는 디바이스 전체 섹터 수를 그대로 사용 */
			offset = (info->label_block + 1) * (blocksize >> 9);			/* [한국어] 레이블이 있어야 할 블록 바로 다음 블록부터 파티션 시작(섹터 단위로 환산) */
			put_partition(state, 1, offset, size-offset);			/* [한국어] 파티션 번호 1로 단일 파티션 등록 */
			seq_buf_puts(&state->pp_buf, "\n");			/* [한국어] 로그 줄 마무리 */
		}
	} else	/* [한국어] 레이블도 없고 dasd 드라이버 인식도 실패 - 이 디스크는 DASD가 아니거나 완전히 인식 불가 */
		res = 0;		/* [한국어] 다른 파티션 파서에게 위임하도록 명시적으로 0 유지 */

out_freeall:
	kfree(label);	/* [한국어] label 버퍼 해제 - getgeo 실패 등으로 여기 도달해도, 이미 할당된 세 버퍼를 모두 해제하는 첫 단계 */
out_nolab:
	kfree(geo);	/* [한국어] geo 버퍼 해제 - label 할당 실패 시에는 out_nolab부터 시작해 label은 건너뛰고 geo/info만 해제 */
out_nogeo:
	kfree(info);	/* [한국어] info 버퍼 해제 - geo 할당 실패 시에는 out_nogeo부터 시작해 info만 해제(info가 이미 NULL로 바뀐 경우 kfree(NULL)은 안전한 no-op) */
out_symbol:
	if (fn)	/* [한국어] symbol_get()이 실제로 유효한 심볼 주소를 반환했었는지 확인 */
		symbol_put(dasd_biodasdinfo);		/* [한국어] 커널 모듈 참조 카운트를 되돌려 dasd 드라이버 모듈이 언로드될 수 있도록 함 */
out_exit:
	return res;	/* [한국어] 최종 결과 반환 - 0이면 다른 파서에 위임, 1 이상이면 이 파서가 처리했음을 의미 */
}
